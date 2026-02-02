module;
#include <liburing.h>
#include <sys/socket.h>

export module rio:fut.io;

import std;
import :context;
import :socket;
import :promise;
import :futures;

namespace rio::fut {

struct Async_poller
{
    template <typename HandleType>
    auto operator()(HandleType &h) const { return h.poll(); }
};

export struct Accept_result
{
    rio::Tcp_socket client;
    rio::address address;
};

template <typename T>
struct Async_state : public rio::promise::State<T>
{
    bool io_done = false;
    bool future_dropped = false;
};

template <typename T>
struct Async_handle
{
    Async_state<T> *ptr = nullptr;

    Async_handle(Async_state<T> *s) : ptr(s) {}

    Async_handle(Async_handle &&other) noexcept : ptr(other.ptr) { other.ptr = nullptr; }

    Async_handle &operator=(Async_handle &&other) noexcept
    {
        if (this != &other)
        {
            if (ptr)
            {
                if (ptr->io_done)
                    delete ptr;
                else
                    ptr->future_dropped = true;
            }

            ptr = other.ptr;
            other.ptr = nullptr;
        }
        return *this;
    }

    Async_handle(const Async_handle &) = delete;
    Async_handle &operator=(const Async_handle &) = delete;

    ~Async_handle()
    {
        if (!ptr)
            return;
        if (ptr->io_done)
            delete ptr;
        else
            ptr->future_dropped = true;
    }

    auto poll() { return ptr->poll(); }
};

template <typename ValType>
struct Uring_req
{
    rio::internals::uring_request_header header;
    Async_state<ValType> *state;
    static void on_complete(rio::internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<Uring_req *>(ptr);
        rio::Promise<Async_state<ValType>> p{.state = self->state};
        if (res < 0)
            p.reject(std::error_code(-res, std::system_category()));
        else
            p.resolve(static_cast<ValType>(res));
        self->state->io_done = true;
        if (self->state->future_dropped)
            delete self->state;
        delete self;
    }
};

struct Accept_req
{
    rio::internals::uring_request_header header;
    Async_state<Accept_result> *state;
    rio::address client_addr;
    socklen_t addr_len = sizeof(sockaddr_storage);
    static void on_complete(rio::internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<Accept_req *>(ptr);
        rio::Promise<Async_state<Accept_result>> p{.state = self->state};
        if (res < 0)
            p.reject(std::error_code(-res, std::system_category()));
        else
        {
            self->client_addr.len = self->addr_len;
            p.resolve(Accept_result{.client = rio::Tcp_socket::attach(res), .address = std::move(self->client_addr)});
        }
        self->state->io_done = true;
        if (self->state->future_dropped)
            delete self->state;
        delete self;
    }
};

export auto read(rio::context &ctx, int fd, std::span<char> buf)
{
    using ValType = std::size_t;
    auto *s = new Async_state<ValType>();
    auto *req = new Uring_req<ValType>{.header = {.call = &Uring_req<ValType>::on_complete}, .state = s};
    auto *sqe = ctx.sqe();
    io_uring_prep_read(sqe, fd, buf.data(), buf.size(), 0);
    io_uring_sqe_set_data(sqe, &req->header);
    ctx.submit();
    return rio::Future(Async_handle{s}, Async_poller{});
}

export template <typename HandleT>
requires requires(HandleT h) { h.fd.native_handle(); }
auto read(rio::context &ctx, HandleT &h, std::span<char> buf) { return read(ctx, h.fd.native_handle(), buf); }

export auto write(rio::context &ctx, int fd, std::span<const char> buf)
{
    using ValType = std::size_t;
    auto *s = new Async_state<ValType>();
    auto *req = new Uring_req<ValType>{.header = {.call = &Uring_req<ValType>::on_complete}, .state = s};
    auto *sqe = ctx.sqe();
    io_uring_prep_write(sqe, fd, const_cast<char *>(buf.data()), buf.size(), 0);
    io_uring_sqe_set_data(sqe, &req->header);
    ctx.submit();
    return rio::Future(Async_handle{s}, Async_poller{});
}

export template <typename HandleT>
requires requires(HandleT h) { h.fd.native_handle(); }
auto write(rio::context &ctx, HandleT &h, std::span<const char> buf)
{
    return write(ctx, h.fd.native_handle(), buf);
}

export auto accept(rio::context &ctx, rio::Tcp_socket &listener)
{
    using ValType = Accept_result;
    auto *s = new Async_state<ValType>();
    auto *req = new Accept_req{.header = {.call = &Accept_req::on_complete}, .state = s, .client_addr = {}, .addr_len = sizeof(sockaddr_storage)};
    auto *sqe = ctx.sqe();
    io_uring_prep_accept(sqe, listener.fd.native_handle(), reinterpret_cast<sockaddr *>(&req->client_addr.storage), &req->addr_len, 0);
    io_uring_sqe_set_data(sqe, &req->header);
    ctx.submit();
    return rio::Future(Async_handle{s}, Async_poller{});
}

}  // namespace rio::fut
