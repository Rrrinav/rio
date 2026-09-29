module;

#include <cerrno>
#include <liburing.h>
#include <sys/socket.h>

export module rio:fut.udp.io;

import std;
import :context;
import :socket;
import :promise;
import :futures;
import :fut.io;
import :io.udp_socket;

namespace rio::fut {

struct Udp_recv_payload {
    rio::address peer_addr;
    struct iovec iov{};
    struct msghdr msg{};
};

using Udp_recv_req = Basic_op<rio::Udp_recv_result, Udp_recv_payload>;

struct Udp_recv_req_impl : Udp_recv_req {
    using Udp_recv_req::Udp_recv_req;

    static void finish(Udp_recv_req_impl *self, std::error_code ec)
    {
        self->state.reject(ec);
        self->io_done = true;
        if (self->future_dropped) {
            self->ctx->recycle(self);
        }
    }

    static void finish(Udp_recv_req_impl *self, rio::Udp_recv_result result)
    {
        self->state.resolve(std::move(result));
        self->io_done = true;
        if (self->future_dropped) {
            self->ctx->recycle(self);
        }
    }

    static void on_complete(rio::internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<Udp_recv_req_impl *>(ptr);
        if (res == -ECANCELED) {
            finish(self, std::make_error_code(std::errc::operation_canceled));
        } else if (res < 0) {
            finish(self, std::error_code(-res, std::system_category()));
        } else {
            self->payload.peer_addr.len = self->payload.msg.msg_namelen;
            finish(self, rio::Udp_recv_result{static_cast<std::size_t>(res), std::move(self->payload.peer_addr)});
        }
    }
};

struct Udp_send_payload {
    rio::address dest;
    struct iovec iov{};
    struct msghdr msg{};
};

using Udp_send_req = Basic_op<std::size_t, Udp_send_payload>;

struct Udp_send_req_impl : Udp_send_req {
    using Udp_send_req::Udp_send_req;

    static void finish(Udp_send_req_impl *self, std::error_code ec)
    {
        self->state.reject(ec);
        self->io_done = true;
        if (self->future_dropped) {
            self->ctx->recycle(self);
        }
    }

    static void finish(Udp_send_req_impl *self, std::size_t n)
    {
        self->state.resolve(n);
        self->io_done = true;
        if (self->future_dropped) {
            self->ctx->recycle(self);
        }
    }

    static void on_complete(rio::internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<Udp_send_req_impl *>(ptr);
        if (res == -ECANCELED) {
            finish(self, std::make_error_code(std::errc::operation_canceled));
        } else if (res < 0) {
            finish(self, std::error_code(-res, std::system_category()));
        } else {
            finish(self, static_cast<std::size_t>(res));
        }
    }
};

// Borrowed receive: only a bound socket can receive. The buffer must
// outlive the operation (same contract as the TCP read/write paths).
export auto recv_from(rio::context &ctx, rio::Udp_socket<rio::udp_state::bound> &sock, std::span<char> buf)
{
    auto *req = ctx.make_pooled<Udp_recv_req_impl>(ctx);
    req->header.call = &Udp_recv_req_impl::on_complete;

    auto &p = req->payload;
    p.peer_addr.len = sizeof(p.peer_addr.storage);
    p.iov = {.iov_base = buf.data(), .iov_len = buf.size()};
    p.msg.msg_name = p.peer_addr.data();
    p.msg.msg_namelen = sizeof(p.peer_addr.storage);
    p.msg.msg_iov = &p.iov;
    p.msg.msg_iovlen = 1;

    auto *sqe = ctx.sqe();
    io_uring_prep_recvmsg(sqe, sock.native_handle(), &p.msg, 0);
    io_uring_sqe_set_data(sqe, &req->header);
    return rio::Future(Async_handle<Udp_recv_req_impl>{req}, rio::fut::Call_poll{});
}

// Borrowed send: valid on any open UDP socket (POSIX assigns an ephemeral
// port to unbound senders). The destination is copied into the operation;
// the buffer must outlive it.
export auto send_to(rio::context &ctx, const rio::Udp_socket<rio::udp_state::unbound> &sock, std::span<const char> buf, const rio::address &dest)
{
    auto *req = ctx.make_pooled<Udp_send_req_impl>(ctx);
    req->header.call = &Udp_send_req_impl::on_complete;

    auto &p = req->payload;
    p.dest = dest;
    p.iov = {.iov_base = const_cast<char *>(buf.data()), .iov_len = buf.size()};
    p.msg.msg_name = p.dest.data();
    p.msg.msg_namelen = p.dest.size();
    p.msg.msg_iov = &p.iov;
    p.msg.msg_iovlen = 1;

    auto *sqe = ctx.sqe();
    io_uring_prep_sendmsg(sqe, sock.native_handle(), &p.msg, 0);
    io_uring_sqe_set_data(sqe, &req->header);
    return rio::Future(Async_handle<Udp_send_req_impl>{req}, rio::fut::Call_poll{});
}

export auto send_to(rio::context &ctx, const rio::Udp_socket<rio::udp_state::bound> &sock, std::span<const char> buf, const rio::address &dest)
{
    auto *req = ctx.make_pooled<Udp_send_req_impl>(ctx);
    req->header.call = &Udp_send_req_impl::on_complete;

    auto &p = req->payload;
    p.dest = dest;
    p.iov = {.iov_base = const_cast<char *>(buf.data()), .iov_len = buf.size()};
    p.msg.msg_name = p.dest.data();
    p.msg.msg_namelen = p.dest.size();
    p.msg.msg_iov = &p.iov;
    p.msg.msg_iovlen = 1;

    auto *sqe = ctx.sqe();
    io_uring_prep_sendmsg(sqe, sock.native_handle(), &p.msg, 0);
    io_uring_sqe_set_data(sqe, &req->header);
    return rio::Future(Async_handle<Udp_send_req_impl>{req}, rio::fut::Call_poll{});
}

} // namespace rio::fut
