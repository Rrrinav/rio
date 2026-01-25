module;

#include <liburing.h>

export module rio:context;

import std;

namespace rio {

export struct context
{
    io_uring ring{};

    explicit context(unsigned entries = 8)
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
};

} // namespace rio

