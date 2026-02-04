module;

#include <liburing.h>

export module rio:context;

import std;

namespace rio {

    namespace internals {

    export struct uring_request_header
    {
        void (*call)(uring_request_header* self, int res);
    };

    };

export struct context
{
    io_uring ring{};

    struct tombstone
    {
        void *ptr;
        void (*destroy)(void *);
    };

    std::vector<tombstone> graveyard;

    explicit context(unsigned entries = 128)
    {
        if (int ret = io_uring_queue_init(entries, &ring, 0); ret < 0)
            throw std::runtime_error(std::format("Failed to init io_uring, return: {}.", std::to_string(-ret)));
    }

    ~context()
    {
        if (ring.ring_fd >= 0)
            io_uring_queue_exit(&ring);
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
        if (this != &other)
        {
            if (ring.ring_fd >= 0)
                io_uring_queue_exit(&ring);

            ring = other.ring;
            other.ring.ring_fd = -1;
        }
        return *this;
    }

    [[nodiscard]]
    auto sqe() -> io_uring_sqe*
    {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        if (!sqe) [[unlikely]]
        {
            io_uring_submit(&ring);
            sqe = io_uring_get_sqe(&ring);
            if (!sqe) [[unlikely]]
                return nullptr;
        }
        return sqe;
    }

    void submit()
    {
        io_uring_submit(&ring);
    }

    void poll()
    {
        io_uring_cqe *cqe;

        // If no completions, return
        if (io_uring_wait_cqe(&ring, &cqe) < 0)
            return;

        try_poll();
    }

    void try_poll()
    {
        io_uring_cqe *cqe;

        // Process all available completions in the batch
        unsigned head;
        unsigned count = 0;

        io_uring_for_each_cqe(&ring, head, cqe)
        {
            count++;

            auto *ptr = io_uring_cqe_get_data(cqe);

            if (ptr)
            {
                auto *req = static_cast<internals::uring_request_header *>(ptr);
                req->call(req, cqe->res);
            }
        }

        io_uring_cq_advance(&ring, count);
    }

    auto run(bool& quit)
    {
        while(!quit)
            this->poll();
    }
    void run()
    {
        while (true)
            this->poll();
    }

    template <typename T>
    void defer_delete(T *ptr)
    {
        if (!ptr)
            return;

        graveyard.push_back({.ptr = static_cast<void *>(ptr), .destroy = [](void *p) { delete static_cast<T *>(p); }});
    }

    void purge_graveyard()
    {
        if (graveyard.empty())
            return;

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
        for (auto it = batch.begin(); it != ret.begin(); ++it) it->destroy(it->ptr);
    }
};

} // namespace rio

