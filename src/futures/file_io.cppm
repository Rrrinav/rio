module;

#include <liburing.h>
#include <sys/socket.h>

export module rio:fut.file.io;

import std;
import :context;
import :file;
import :promise;
import :futures;
import :fut.io;

namespace rio::fut {

using Open_req = Basic_op<rio::file, std::string>;

struct Open_req_impl : Open_req
{
    using Open_req::Open_req;

    static void finish(Open_req_impl *self, std::error_code ec)
    {
        self->state.reject(ec);
        self->io_done = true;
        if (self->future_dropped)
            self->ctx->recycle(self);
    }

    static void finish(Open_req_impl *self, rio::file f)
    {
        self->state.resolve(std::move(f));
        self->io_done = true;
        if (self->future_dropped)
            self->ctx->recycle(self);
    }

    static void on_complete(rio::internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<Open_req_impl *>(ptr);
        if (res < 0)
            finish(self, std::error_code(-res, std::system_category()));
        else
            finish(self, rio::file::attach(res));
    }
};

using Sync_req = Basic_op<void>;

struct Sync_req_impl : Sync_req
{
    using Sync_req::Sync_req;

    static void finish(Sync_req_impl *self, std::error_code ec)
    {
        self->state.reject(ec);
        self->io_done = true;
        if (self->future_dropped)
            self->ctx->recycle(self);
    }

    static void finish(Sync_req_impl *self)
    {
        self->state.resolve();
        self->io_done = true;
        if (self->future_dropped)
            self->ctx->recycle(self);
    }

    static void on_complete(rio::internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<Sync_req_impl *>(ptr);
        if (res < 0)
            finish(self, std::error_code(-res, std::system_category()));
        else
            finish(self);
    }
};

export auto open_file(rio::context &ctx, std::string_view path, rio::f_mode flags, mode_t mode = 0644)
{
    auto *req = ctx.make_pooled<Open_req_impl>(ctx, std::string(path));
    req->header.call = &Open_req_impl::on_complete;

    auto *sqe = ctx.sqe();
    io_uring_prep_openat(sqe, AT_FDCWD, req->payload.c_str(), static_cast<int>(flags), mode);
    io_uring_sqe_set_data(sqe, &req->header);
    return rio::Future(Async_handle<Open_req_impl>{req}, rio::fut::Call_poll{});
}

export auto sync(rio::context &ctx, rio::file &f)
{
    auto *req = ctx.make_pooled<Sync_req_impl>(ctx);
    req->header.call = &Sync_req_impl::on_complete;

    auto *sqe = ctx.sqe();
    io_uring_prep_fsync(sqe, f.fd.native_handle(), 0);
    io_uring_sqe_set_data(sqe, &req->header);
    return rio::Future(Async_handle<Sync_req_impl>{req}, rio::fut::Call_poll{});
}

export auto read_at(rio::context &ctx, rio::file &f, std::span<char> buf, size_t offset)
{
    return rio::fut::read(ctx, f.fd.native_handle(), buf, offset);
}

export auto write_at(rio::context &ctx, rio::file &f, std::span<const char> buf, size_t offset)
{
    return rio::fut::write(ctx, f.fd.native_handle(), buf, offset);
}

} // namespace rio::fut
