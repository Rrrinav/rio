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

        // Wait for at least 1 completion
        if (io_uring_wait_cqe(&ring, &cqe) < 0)
            return;

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

        // Must advance the ring, otherwise we read the same events again!
        io_uring_cq_advance(&ring, count);
    }
};

} // namespace rio

