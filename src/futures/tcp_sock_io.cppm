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

struct Accept_req
{
    rio::internals::uring_request_header header;
    Async_state<Tcp_accept_result> *state;
    rio::address client_addr;
    socklen_t addr_len = sizeof(sockaddr_storage);

    static void on_complete(rio::internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<Accept_req *>(ptr);
        rio::Promise<Async_state<Tcp_accept_result>> p{.state = self->state};
        if (res < 0)
        {
            p.reject(std::error_code(-res, std::system_category()));
        }
        else
        {
            self->client_addr.len = self->addr_len;
            p.resolve(Tcp_accept_result{.client = rio::Tcp_socket::attach(res), .address = std::move(self->client_addr)});
        }
        self->state->io_done = true;
        if (self->state->future_dropped)
            delete self->state;
        delete self;
    }
};

struct Connect_req
{
    rio::internals::uring_request_header header;
    Async_state<void> *state;

    static void on_complete(rio::internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<Connect_req *>(ptr);
        rio::Promise<Async_state<void>> p{.state = self->state};
        if (res < 0)
            p.reject(std::error_code(-res, std::system_category()));
        else
            p.resolve();

        self->state->io_done = true;
        if (self->state->future_dropped)
            delete self->state;
        delete self;
    }
};

export auto accept(rio::context &ctx, rio::Tcp_socket &listener)
{
    using Val_type = Tcp_accept_result;
    auto *s = new Async_state<Val_type>();
    auto *req =
        new Accept_req{.header = {.call = &Accept_req::on_complete}, .state = s, .client_addr = {}, .addr_len = sizeof(sockaddr_storage)};
    auto *sqe = ctx.sqe();
    io_uring_prep_accept(sqe, listener.fd.native_handle(), reinterpret_cast<sockaddr *>(&req->client_addr.storage), &req->addr_len, SOCK_CLOEXEC);
    io_uring_sqe_set_data(sqe, &req->header);
    ctx.submit();
    return rio::Future(Async_handle{s}, Async_poller{});
}

export auto connect(rio::context &ctx, rio::Tcp_socket &client, const rio::address &addr)
{
    auto *s = new Async_state<void>();
    auto *req = new Connect_req{.header = {.call = &Connect_req::on_complete}, .state = s};
    auto *sqe = ctx.sqe();
    io_uring_prep_connect(sqe, client.fd.native_handle(), addr.data(), addr.size());
    io_uring_sqe_set_data(sqe, &req->header);
    ctx.submit();
    return rio::Future(Async_handle{s}, Async_poller{});
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
        while (remaining > 0)
        {
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
    return rio::fut::make(
        Accept_many_impl<std::decay_t<Handler>>{&ctx, &listener, limit, std::forward<Handler>(handler)},
        [](auto &s) { return s.poll(); }
    );
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
        while (true)
        {
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
    return rio::fut::make(
        Accept_all_impl<std::decay_t<Handler>>{&ctx, &listener, std::forward<Handler>(handler)},
        [](auto &s) { return s.poll(); }
    );
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
        while (remaining > 0)
        {
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
    return rio::fut::make(Accept_into_impl<OutputIt>{&ctx, &listener, limit, it}, [](auto &s) { return s.poll(); });
}

export template <typename Rep, typename Period>
auto stop_accept_after(rio::context &ctx, rio::Tcp_socket &listener, std::chrono::duration<Rep, Period> timeout)
{
    using namespace std::chrono;
    using Val_type = Tcp_accept_result;
    auto *s = new Async_state<Val_type>();

    auto *accept_req = new Accept_req{.header = {.call = [](rio::internals::uring_request_header *ptr, int res) {
        auto *self = reinterpret_cast<Accept_req *>(ptr);
        if (res == -ECANCELED)
        {
            rio::Promise<Async_state<Val_type>> p{.state = self->state};
            p.reject(std::make_error_code(std::errc::timed_out));
            self->state->io_done = true;
            if (self->state->future_dropped)
                delete self->state;
            delete self;
        }
        else Accept_req::on_complete(ptr, res);
    }},
        .state = s,
        .client_addr = {},
        .addr_len = sizeof(sockaddr_storage)
    };

    auto *timer_req = new Link_timeout_req{.header = {.call = &Link_timeout_req::on_complete}};
    auto sec = duration_cast<seconds>(timeout);
    auto nsec = duration_cast<nanoseconds>(timeout - sec);
    timer_req->ts.tv_sec = sec.count();
    timer_req->ts.tv_nsec = nsec.count();

    auto *sqe_accept = ctx.sqe();
    auto *sqe_timer = ctx.sqe();

    io_uring_prep_accept(sqe_accept, listener.fd.native_handle(), reinterpret_cast<sockaddr *>(&accept_req->client_addr.storage),
        &accept_req->addr_len, SOCK_CLOEXEC);
    io_uring_sqe_set_flags(sqe_accept, IOSQE_IO_LINK);
    io_uring_sqe_set_data(sqe_accept, &accept_req->header);

    io_uring_prep_link_timeout(sqe_timer, &timer_req->ts, 0);
    io_uring_sqe_set_data(sqe_timer, &timer_req->header);

    ctx.submit();
    return rio::Future(Async_handle{s}, Async_poller{});
}

} // namespace rio::fut
