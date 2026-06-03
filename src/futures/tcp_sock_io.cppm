module;

#include <liburing.h>
#include <sys/socket.h>

export module rio:fut.tcp.io;

import std;
import :context;
import :socket;
import :promise;
import :futures;
import :fut.io;

namespace rio::fut {

struct Accept_payload
{
    rio::address client_addr;
    socklen_t addr_len = sizeof(sockaddr_storage);
};

using Accept_req = Basic_op<Tcp_accept_result, Accept_payload>;

struct Accept_req_impl : Accept_req
{
    using Accept_req::Accept_req;

    static void finish(Accept_req_impl *self, std::error_code ec)
    {
        self->state.reject(ec);
        self->io_done = true;
        if (self->future_dropped)
            self->ctx->recycle(self);
    }

    static void finish(Accept_req_impl *self, Tcp_accept_result result)
    {
        self->state.resolve(std::move(result));
        self->io_done = true;
        if (self->future_dropped)
            self->ctx->recycle(self);
    }

    static void on_complete(rio::internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<Accept_req_impl *>(ptr);
        if (res < 0) {
            finish(self, std::error_code(-res, std::system_category()));
        } else {
            self->payload.client_addr.len = self->payload.addr_len;
            finish(self, Tcp_accept_result{.client = rio::Tcp_socket::attach(res), .address = std::move(self->payload.client_addr)});
        }
    }
};

using Connect_req = Basic_op<void, rio::address>;

struct Connect_req_impl : Connect_req
{
    using Connect_req::Connect_req;

    static void finish(Connect_req_impl *self, std::error_code ec)
    {
        self->state.reject(ec);
        self->io_done = true;
        if (self->future_dropped)
            self->ctx->recycle(self);
    }

    static void finish(Connect_req_impl *self)
    {
        self->state.resolve();
        self->io_done = true;
        if (self->future_dropped)
            self->ctx->recycle(self);
    }

    static void on_complete(rio::internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<Connect_req_impl *>(ptr);
        if (res < 0)
            finish(self, std::error_code(-res, std::system_category()));
        else
            finish(self);
    }
};

export auto accept(rio::context &ctx, rio::Tcp_socket &listener)
{
    auto *req = ctx.make_pooled<Accept_req_impl>(ctx);
    req->header.call = &Accept_req_impl::on_complete;
    auto *sqe = ctx.sqe();

    io_uring_prep_accept(
        sqe, listener.fd.native_handle(), reinterpret_cast<sockaddr *>(&req->payload.client_addr.storage), &req->payload.addr_len, SOCK_CLOEXEC);

    io_uring_sqe_set_data(sqe, &req->header);
    return rio::Future(Async_handle<Accept_req_impl>{req}, rio::fut::Call_poll{});
}

export auto connect(rio::context &ctx, rio::Tcp_socket &client, const rio::address &addr)
{
    auto *req = ctx.make_pooled<Connect_req_impl>(ctx, addr);
    req->header.call = &Connect_req_impl::on_complete;
    auto *sqe = ctx.sqe();
    io_uring_prep_connect(sqe, client.fd.native_handle(), req->payload.data(), req->payload.size());
    io_uring_sqe_set_data(sqe, &req->header);
    return rio::Future(Async_handle<Connect_req_impl>{req}, rio::fut::Call_poll{});
}

export template <typename Handler>
struct Accept_many_impl
{
    rio::context *ctx;
    rio::Tcp_socket *listener;
    size_t remaining;
    Handler handler;
    std::optional<decltype(accept(std::declval<rio::context &>(), std::declval<rio::Tcp_socket &>()))> curr_fut;

    auto poll() -> rio::fut::res<void>
    {
        while (remaining > 0) {
            if (!curr_fut)
                curr_fut.emplace(accept(*ctx, *listener));
            auto r = curr_fut->poll();

            if (r.state == rio::fut::status::pending)
                return rio::fut::res<void>::pending();
            if (r.state == rio::fut::status::error)
                return rio::fut::res<void>::error(r.err);

            auto [sock, addr] = std::move(*r.value);
            std::invoke(handler, std::move(sock), std::move(addr));

            curr_fut.reset();
            remaining--;
        }
        return rio::fut::res<void>::ready();
    }
};

export template <typename Handler>
auto accept_many(rio::context &ctx, rio::Tcp_socket &listener, size_t limit, Handler &&handler)
{
    return rio::fut::make(Accept_many_impl<std::decay_t<Handler>>{&ctx, &listener, limit, std::forward<Handler>(handler)}, [](auto &s) {
        return s.poll();
    });
}

export template <typename Handler>
struct Accept_all_impl
{
    rio::context *ctx;
    rio::Tcp_socket *listener;
    Handler handler;
    std::optional<decltype(accept(std::declval<rio::context &>(), std::declval<rio::Tcp_socket &>()))> curr_fut{};

    auto poll() -> rio::fut::res<void>
    {
        while (true) {
            if (!curr_fut)
                curr_fut.emplace(accept(*ctx, *listener));
            auto r = curr_fut->poll();

            if (r.state == rio::fut::status::pending)
                return rio::fut::res<void>::pending();
            if (r.state == rio::fut::status::error)
                return rio::fut::res<void>::error(r.err);

            auto [sock, addr] = std::move(*r.value);
            std::invoke(handler, std::move(sock), std::move(addr));

            curr_fut.reset();
        }
    }
};

export template <typename Handler>
auto accept_all(rio::context &ctx, rio::Tcp_socket &listener, Handler &&handler)
{
    return rio::fut::make(Accept_all_impl<std::decay_t<Handler>>{&ctx, &listener, std::forward<Handler>(handler)}, rio::fut::Call_poll{});
}

export template <typename OutputIt>
struct Accept_into_impl
{
    rio::context *ctx;
    rio::Tcp_socket *listener;
    size_t remaining;
    OutputIt it;
    std::optional<decltype(accept(std::declval<rio::context &>(), std::declval<rio::Tcp_socket &>()))> curr_fut;

    auto poll() -> rio::fut::res<void>
    {
        while (remaining > 0) {
            if (!curr_fut)
                curr_fut.emplace(accept(*ctx, *listener));
            auto r = curr_fut->poll();

            if (r.state == rio::fut::status::pending)
                return rio::fut::res<void>::pending();
            if (r.state == rio::fut::status::error)
                return rio::fut::res<void>::error(r.err);

            *it++ = std::move(*r.value);

            curr_fut.reset();
            remaining--;
        }
        return rio::fut::res<void>::ready();
    }
};

export template <typename OutputIt>
auto accept_into(rio::context &ctx, rio::Tcp_socket &listener, size_t limit, OutputIt it)
{
    return rio::fut::make(Accept_into_impl<OutputIt>{&ctx, &listener, limit, it}, rio::fut::Call_poll{});
}

export template <typename Rep, typename Period>
auto stop_accept_after(rio::context &ctx, rio::Tcp_socket &listener, std::chrono::duration<Rep, Period> timeout)
{
    using namespace std::chrono;
    struct Timed_accept_req : Accept_req
    {
        using Accept_req::Accept_req;

        static void finish(Timed_accept_req *self, std::error_code ec)
        {
            self->state.reject(ec);
            self->io_done = true;
            if (self->future_dropped)
                self->ctx->recycle(self);
        }

        static void finish(Timed_accept_req *self, Tcp_accept_result result)
        {
            self->state.resolve(std::move(result));
            self->io_done = true;
            if (self->future_dropped)
                self->ctx->recycle(self);
        }

        static void on_complete(rio::internals::uring_request_header *ptr, int res)
        {
            auto *self = reinterpret_cast<Timed_accept_req *>(ptr);
            if (res == -ECANCELED) {
                finish(self, std::make_error_code(std::errc::timed_out));
            } else if (res < 0) {
                finish(self, std::error_code(-res, std::system_category()));
            } else {
                self->payload.client_addr.len = self->payload.addr_len;
                finish(self, Tcp_accept_result{.client = rio::Tcp_socket::attach(res), .address = std::move(self->payload.client_addr)});
            }
        }
    };

    auto *accept_req = ctx.make_pooled<Timed_accept_req>(ctx);
    accept_req->header.call = &Timed_accept_req::on_complete;

    auto *timer_req = ctx.make_pooled<Link_timeout_req>(ctx);
    timer_req->header.call = &Link_timeout_req::on_complete;
    auto sec = duration_cast<seconds>(timeout);
    auto nsec = duration_cast<nanoseconds>(timeout - sec);
    timer_req->ts.tv_sec = sec.count();
    timer_req->ts.tv_nsec = nsec.count();

    auto *sqe_accept = ctx.sqe();
    auto *sqe_timer = ctx.sqe();

    io_uring_prep_accept(
        sqe_accept,
        listener.fd.native_handle(),
        reinterpret_cast<sockaddr *>(&accept_req->payload.client_addr.storage),
        &accept_req->payload.addr_len,
        SOCK_CLOEXEC);

    io_uring_sqe_set_flags(sqe_accept, IOSQE_IO_LINK);
    io_uring_sqe_set_data(sqe_accept, &accept_req->header);

    io_uring_prep_link_timeout(sqe_timer, &timer_req->ts, 0);
    io_uring_sqe_set_data(sqe_timer, &timer_req->header);

    return rio::Future(Async_handle<Timed_accept_req>{accept_req}, rio::fut::Call_poll{});
}

} // namespace rio::fut
