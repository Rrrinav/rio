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
            p.reject(std::error_code(-res, std::system_category()));
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

export auto accept(rio::context &ctx, rio::Tcp_socket &listener)
{
    using Val_type = Tcp_accept_result;
    auto *s = new Async_state<Val_type>();
    auto *req = new Accept_req{.header = {.call = &Accept_req::on_complete}, .state = s, .client_addr = {}, .addr_len = sizeof(sockaddr_storage)};
    auto *sqe = ctx.sqe();
    io_uring_prep_accept(sqe, listener.fd.native_handle(), reinterpret_cast<sockaddr *>(&req->client_addr.storage), &req->addr_len, 0);
    io_uring_sqe_set_data(sqe, &req->header);
    ctx.submit();
    return rio::Future(Async_handle{s}, Async_poller{});
}

export template <typename Rep, typename Period>
auto stop_read_after(rio::context &ctx, rio::handle fd, std::span<char> buf, std::chrono::duration<Rep, Period> timeout)
{
    using namespace std::chrono;
    using Val_type = std::size_t;

    // 1. Setup the MAIN Operation (Read)
    //    We wrap the callback to catch the specific cancellation error.
    auto *s = new Async_state<Val_type>();

    auto *read_req = new Uring_req<Val_type>{.header = { .call = [](rio::internals::uring_request_header *ptr, int res) {
        auto *self = reinterpret_cast<Uring_req<Val_type> *>(ptr);

        // SPECIAL HANDLING: If we were canceled, it means the Timer won.
        if (res == -ECANCELED)
        {
            rio::Promise<Async_state<Val_type>> p{.state = self->state};
            p.reject(std::make_error_code(std::errc::timed_out));

            // Manual cleanup since standard flow was interrupted
            self->state->io_done = true;
            if (self->state->future_dropped)
                delete self->state;
            delete self;
        }
        else
        {
            // Normal completion (Success or other error)
            Uring_req<Val_type>::on_complete(ptr, res);
        }
    }}, .state = s };

    // 2. Setup the TIMER Operation
    auto *timer_req = new Link_timeout_req{.header = {.call = &Link_timeout_req::on_complete}};

    auto sec = duration_cast<seconds>(timeout);
    auto nsec = duration_cast<nanoseconds>(timeout - sec);
    timer_req->ts.tv_sec = sec.count();
    timer_req->ts.tv_nsec = nsec.count();

    // 3. Submit Linked Request
    //    Note: We need 2 consecutive slots.
    auto *sqe_read = ctx.sqe();
    auto *sqe_timer = ctx.sqe();

    // Op 1: READ
    io_uring_prep_read(sqe_read, fd, buf.data(), buf.size(), 0);
    io_uring_sqe_set_flags(sqe_read, IOSQE_IO_LINK);  // <--- THE GLUE
    io_uring_sqe_set_data(sqe_read, &read_req->header);

    // Op 2: LINK_TIMEOUT (Cancels Op 1 if time runs out)
    io_uring_prep_link_timeout(sqe_timer, &timer_req->ts, 0);
    io_uring_sqe_set_data(sqe_timer, &timer_req->header);

    ctx.submit();
    return rio::Future(Async_handle{s}, Async_poller{});
}

export template <typename Rep, typename Period>
auto stop_accept_after(rio::context &ctx, rio::Tcp_socket &listener, std::chrono::duration<Rep, Period> timeout)
{
    using namespace std::chrono;
    using Val_type = Tcp_accept_result;

    auto *s = new Async_state<Val_type>();

    // 1. Accept Request with Cancel detection
    auto *accept_req = new Accept_req{ .header = { .call = [](rio::internals::uring_request_header *ptr, int res) {
        auto *self = reinterpret_cast<Accept_req *>(ptr);
        if (res == -ECANCELED) {
            rio::Promise<Async_state<Val_type>> p{.state = self->state};
            p.reject(std::make_error_code(std::errc::timed_out));
            self->state->io_done = true;
            if (self->state->future_dropped) delete self->state;
            delete self;
        } else {
            Accept_req::on_complete(ptr, res);
        }
    }},
        .state = s,
        .client_addr = {},
        .addr_len = sizeof(sockaddr_storage)
    };

    // 2. Timer Request
    auto *timer_req = new Link_timeout_req{.header = {.call = &Link_timeout_req::on_complete} };
    auto sec = duration_cast<seconds>(timeout);
    auto nsec = duration_cast<nanoseconds>(timeout - sec);
    timer_req->ts.tv_sec = sec.count();
    timer_req->ts.tv_nsec = nsec.count();

    auto *sqe_accept = ctx.sqe();
    auto *sqe_timer = ctx.sqe();

    io_uring_prep_accept(sqe_accept, listener.fd.native_handle(), reinterpret_cast<sockaddr *>(&accept_req->client_addr.storage), &accept_req->addr_len, 0);
    io_uring_sqe_set_flags(sqe_accept, IOSQE_IO_LINK);
    io_uring_sqe_set_data(sqe_accept, &accept_req->header);

    io_uring_prep_link_timeout(sqe_timer, &timer_req->ts, 0);
    io_uring_sqe_set_data(sqe_timer, &timer_req->header);

    ctx.submit();
    return rio::Future(Async_handle{s}, Async_poller{});
}
}  // namespace rio::fut
