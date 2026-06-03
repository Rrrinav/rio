module;

#include <liburing.h>

export module rio:context;

import std;

namespace rio {

namespace internals {

export struct uring_request_header
{
    void (*call)(uring_request_header *self, int res);
};

}; // namespace internals

export struct context
{
    io_uring ring{};
    static constexpr std::size_t pool_alignment = alignof(std::max_align_t);
    static constexpr std::uint32_t dynamic_bucket = static_cast<std::uint32_t>(-1);

    struct alignas(pool_alignment) pool_node
    {
        pool_node *next;
        std::uint32_t bucket_index;
    };

    struct pool_bucket
    {
        std::size_t payload_size{};
        std::size_t stride{};
        std::size_t blocks_per_slab{};
        pool_node *free_list{};
        std::vector<void *> slabs{};
    };

    static constexpr std::array<std::size_t, 5> bucket_sizes{128, 256, 512, 1024, 2048};

    struct tombstone
    {
        void *ptr;
        void (*destroy)(void *);
    };

    std::vector<tombstone> graveyard;
    std::array<pool_bucket, bucket_sizes.size()> op_buckets_{};
    bool has_pending_submit_ = false;
    unsigned queued_sqes_ = 0;

    explicit context(unsigned entries = 128)
    {
        if (int ret = io_uring_queue_init(entries, &ring, 0); ret < 0) {
            throw std::runtime_error(std::format("Failed to init io_uring, return: {}.", std::to_string(-ret)));
        }

        for (std::size_t i = 0; i < bucket_sizes.size(); ++i) {
            auto &bucket = op_buckets_[i];
            bucket.payload_size = bucket_sizes[i];
            bucket.stride = align_up(sizeof(pool_node) + bucket.payload_size, pool_alignment);
            bucket.blocks_per_slab = std::max<std::size_t>(8, 4096 / bucket.stride);
        }
    }

    ~context()
    {
        if (ring.ring_fd >= 0) {
            io_uring_queue_exit(&ring);
        }

        for (auto &bucket : op_buckets_) {
            for (void *slab : bucket.slabs) {
                ::operator delete(slab, std::align_val_t(pool_alignment));
            }
        }
    }

    context(const context &) = delete;
    context &operator=(const context &) = delete;

    context(context &&other) noexcept
    {
        ring = other.ring;
        other.ring.ring_fd = -1;
    }

    context &operator=(context &&other) noexcept
    {
        if (this != &other) {
            if (ring.ring_fd >= 0) {
                io_uring_queue_exit(&ring);
            }

            ring = other.ring;
            other.ring.ring_fd = -1;
        }
        return *this;
    }
    void cancel_request(void *original_user_data)
    {
        auto sqe = this->sqe();
        if (!sqe) {
            return;
        }

        io_uring_prep_cancel(sqe, original_user_data, 0);

        io_uring_sqe_set_data(sqe, nullptr);

        this->submit();
    }

    [[nodiscard]]
    auto sqe() -> io_uring_sqe *
    {
        io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        if (!sqe) [[unlikely]] {
            flush();
            sqe = io_uring_get_sqe(&ring);
            if (!sqe) [[unlikely]] {
                return nullptr;
            }
        }
        has_pending_submit_ = true;
        ++queued_sqes_;
        return sqe;
    }

    void flush()
    {
        if (!has_pending_submit_) {
            return;
        }

        io_uring_submit(&ring);
        has_pending_submit_ = false;
        queued_sqes_ = 0;
    }

    void submit()
    {
        flush();
    }

    void poll()
    {
        flush();

        io_uring_cqe *cqe;

        // If no completions, return
        if (io_uring_wait_cqe(&ring, &cqe) < 0) {
            return;
        }

        try_poll();
    }

    void try_poll()
    {
        flush();

        io_uring_cqe *cqe;

        // Process all available completions in the batch
        unsigned head;
        unsigned count = 0;

        io_uring_for_each_cqe(&ring, head, cqe)
        {
            count++;

            auto *ptr = io_uring_cqe_get_data(cqe);

            if (ptr) {
                auto *req = static_cast<internals::uring_request_header *>(ptr);
                req->call(req, cqe->res);
            }
        }

        io_uring_cq_advance(&ring, count);
    }

    auto run(bool &quit)
    {
        while (!quit) {
            this->poll();
        }
    }
    void run()
    {
        while (true) {
            this->poll();
        }
    }

    template <typename T>
    void defer_delete(T *ptr)
    {
        if (!ptr) {
            return;
        }

        graveyard.push_back({.ptr = static_cast<void *>(ptr), .destroy = [](void *p) { delete static_cast<T *>(p); }});
    }

    void purge_graveyard()
    {
        if (graveyard.empty()) {
            return;
        }

        // 1. SWAP: Move pending items to a local batch.
        //    This prevents crashes if a destructor calls defer_delete() recursively.
        std::vector<tombstone> batch;
        batch.swap(graveyard);

        // 2. SORT: Bring duplicates together
        std::ranges::sort(batch, {}, &tombstone::ptr);

        // 3. UNIQUE: Move duplicates to the end
        //    'ret' is a subrange {first_duplicate, end}
        auto ret = std::ranges::unique(batch, {}, &tombstone::ptr);

        // 4. DESTROY: Only the unique items
        for (auto it = batch.begin(); it != ret.begin(); ++it) {
            it->destroy(it->ptr);
        }
    }

    template <typename T, typename... Args>
    T *make_pooled(Args &&...args)
    {
        static_assert(alignof(T) <= pool_alignment, "Pooled types with over-aligned requirements are not supported.");
        void *mem = acquire_pool_block(sizeof(T));
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    template <typename T>
    void recycle(T *ptr)
    {
        if (!ptr) {
            return;
        }

        ptr->~T();
        release_pool_block(ptr);
    }

private:
    static constexpr std::size_t align_up(std::size_t value, std::size_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    [[nodiscard]]
    auto acquire_pool_block(std::size_t payload_size) -> void *
    {
        const auto bucket_index = bucket_index_for(payload_size);

        if (bucket_index == dynamic_bucket) {
            const std::size_t stride = align_up(sizeof(pool_node) + payload_size, pool_alignment);
            auto *node = static_cast<pool_node *>(::operator new(stride, std::align_val_t(pool_alignment)));
            node->bucket_index = dynamic_bucket;
            node->next = nullptr;
            return node + 1;
        }

        auto &bucket = op_buckets_[bucket_index];
        if (!bucket.free_list) {
            replenish_bucket(bucket, static_cast<std::size_t>(bucket_index));
        }

        pool_node *node = bucket.free_list;
        bucket.free_list = node->next;
        node->next = nullptr;
        return node + 1;
    }

    void release_pool_block(void *payload)
    {
        auto *node = static_cast<pool_node *>(payload) - 1;

        if (node->bucket_index == dynamic_bucket) {
            ::operator delete(node, std::align_val_t(pool_alignment));
            return;
        }

        auto &bucket = op_buckets_[node->bucket_index];
        node->next = bucket.free_list;
        bucket.free_list = node;
    }

    [[nodiscard]]
    auto bucket_index_for(std::size_t payload_size) const -> std::uint32_t
    {
        for (std::uint32_t i = 0; i < bucket_sizes.size(); ++i) {
            if (payload_size <= bucket_sizes[i]) {
                return i;
            }
        }
        return dynamic_bucket;
    }

    void replenish_bucket(pool_bucket &bucket, std::size_t bucket_index)
    {
        const std::size_t slab_size = bucket.stride * bucket.blocks_per_slab;
        auto *slab = static_cast<std::byte *>(::operator new(slab_size, std::align_val_t(pool_alignment)));
        bucket.slabs.push_back(slab);

        for (std::size_t i = 0; i < bucket.blocks_per_slab; ++i) {
            auto *node = reinterpret_cast<pool_node *>(slab + i * bucket.stride);
            node->bucket_index = static_cast<std::uint32_t>(bucket_index);
            node->next = bucket.free_list;
            bucket.free_list = node;
        }
    }
};

} // namespace rio
