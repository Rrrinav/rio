module;

#include <liburing.h>
#include <sys/socket.h>

export module rio:fut.file.io;

import std;
import :context;
import :socket;
import :file;
import :promise;
import :futures;
import :fut.io;

namespace rio::fut {

struct Open_req
{
    rio::internals::uring_request_header header;
    Async_state<rio::file> *state;

    static void on_complete(rio::internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<Open_req *>(ptr);
        rio::Promise<Async_state<rio::file>> p{.state = self->state};

        if (res < 0)
            p.reject(std::error_code(-res, std::system_category()));
        else
            p.resolve(rio::file::attach(res));

        self->state->io_done = true;
        if (self->state->future_dropped) delete self->state;
        delete self;
    }
};

export auto open_file(rio::context &ctx, const char *path, rio::f_mode flags, mode_t mode = 0644)
{
    auto *s = new Async_state<rio::file>();
    auto *req = new Open_req{.header = {.call = &Open_req::on_complete}, .state = s};

    auto *sqe = ctx.sqe();
    // AT_FDCWD = open relative to current working directory
    io_uring_prep_openat(sqe, AT_FDCWD, path, static_cast<int>(flags), mode);
    io_uring_sqe_set_data(sqe, &req->header);

    ctx.submit();
    return rio::Future(Async_handle{s}, Async_poller{});
}

export auto read_at(rio::context &ctx, rio::file &f, std::span<char> buf, size_t offset)
{
    using ValType = std::size_t;
    auto *s = new Async_state<ValType>();

    // Reuse the generic Uring_req from fut.io since it returns size_t
    auto *req = new Uring_req<ValType>{.header = {.call = &Uring_req<ValType>::on_complete}, .state = s};

    auto *sqe = ctx.sqe();
    io_uring_prep_read(sqe, f.fd.native_handle(), buf.data(), buf.size(), offset);
    io_uring_sqe_set_data(sqe, &req->header);

    ctx.submit();
    return rio::Future(Async_handle{s}, Async_poller{});
}

export auto write_at(rio::context &ctx, rio::file &f, std::span<const char> buf, size_t offset)
{
    using ValType = std::size_t;
    auto *s = new Async_state<ValType>();

    auto *req = new Uring_req<ValType>{.header = {.call = &Uring_req<ValType>::on_complete}, .state = s};

    auto *sqe = ctx.sqe();
    io_uring_prep_write(sqe, f.fd.native_handle(), const_cast<char *>(buf.data()), buf.size(), offset);
    io_uring_sqe_set_data(sqe, &req->header);

    ctx.submit();
    return rio::Future(Async_handle{s}, Async_poller{});
}

struct Sync_req
{
    rio::internals::uring_request_header header;
    Async_state<void> *state;

    static void on_complete(rio::internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<Sync_req *>(ptr);
        rio::Promise<Async_state<void>> p{.state = self->state};

        if (res < 0)
            p.reject(std::error_code(-res, std::system_category()));
        else
            p.resolve();

        self->state->io_done = true;
        if (self->state->future_dropped) delete self->state;
        delete self;
    }
};

export auto sync(rio::context &ctx, rio::file &f)
{
    auto *s = new Async_state<void>();
    auto *req = new Sync_req{.header = {.call = &Sync_req::on_complete}, .state = s};

    auto *sqe = ctx.sqe();
    io_uring_prep_fsync(sqe, f.fd.native_handle(), 0);
    io_uring_sqe_set_data(sqe, &req->header);

    ctx.submit();
    return rio::Future(Async_handle{s}, Async_poller{});
}

} // namespace rio
