module;

#include <liburing.h>
#include <sys/socket.h>

export module rio:fut.io;

import std;
import :context;
import :handle;
import :promise;
import :futures;

namespace rio::fut {

export template <typename T>
concept Handle_like_c = requires(T h) { h.fd.native_handle(); };

export template <typename T, typename Payload = std::monostate>
struct Basic_op
{
    using value_type = T;

    rio::internals::uring_request_header header{};
    rio::context *ctx{};
    rio::promise::State<T> state{};
    bool io_done = false;
    bool future_dropped = false;
    Payload payload{};

    explicit Basic_op(rio::context &c) : ctx(&c)
    {}

    template <typename... Args>
    Basic_op(rio::context &c, Args &&...args) : ctx(&c), payload(std::forward<Args>(args)...)
    {}

    auto poll()
    {
        return state.poll();
    }
};

namespace op_detail {

template <typename Op>
void recycle_if_possible(Op *op)
{
    op->io_done = true;
    if (op->future_dropped)
        op->ctx->recycle(op);
}

template <typename Op, typename Value>
void finish_success(Op *op, Value &&value)
{
    op->state.resolve(std::forward<Value>(value));
    recycle_if_possible(op);
}

template <typename Op>
void finish_success(Op *op)
{
    op->state.resolve();
    recycle_if_possible(op);
}

template <typename Op>
void finish_error(Op *op, std::error_code ec)
{
    op->state.reject(ec);
    recycle_if_possible(op);
}

} // namespace op_detail

export template <typename Op>
struct Async_handle
{
    using value_type = typename Op::value_type;

    Op *ptr = nullptr;

    explicit Async_handle(Op *s) : ptr(s)
    {}

    Async_handle(Async_handle &&other) noexcept : ptr(other.ptr)
    {
        other.ptr = nullptr;
    }

    Async_handle &operator=(Async_handle &&other) noexcept
    {
        if (this != &other) {
            if (ptr) {
                if (ptr->io_done)
                    ptr->ctx->recycle(ptr);
                else {
                    ptr->future_dropped = true;
                    if (ptr->ctx)
                        ptr->ctx->cancel_request(&ptr->header);
                }
            }
            ptr = other.ptr;
            other.ptr = nullptr;
        }
        return *this;
    }

    ~Async_handle()
    {
        if (!ptr) return;

        if (ptr->io_done) {
            ptr->ctx->recycle(ptr);
        } else {
            ptr->future_dropped = true;
            if (ptr->ctx)
                ptr->ctx->cancel_request(&ptr->header);
        }
    }

    auto poll()
    {
        return ptr->poll();
    }
};

template <typename Op>
auto make_async_future(Op *op)
{
    return rio::Future(Async_handle<Op>{op}, rio::fut::Call_poll{});
}

template <typename T>
struct Counted_op : Basic_op<T>
{
    using Basic_op<T>::Basic_op;

    auto poll()
    {
        return this->state.poll();
    }

    static void on_complete(rio::internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<Counted_op *>(ptr);
        if (res < 0)
            op_detail::finish_error(self, std::error_code(-res, std::system_category()));
        else
            op_detail::finish_success(self, static_cast<T>(res));
    }
};

using Read_write_op = Counted_op<std::size_t>;

export auto read(rio::context &ctx, int fd, std::span<char> buf, size_t offset = 0)
{
    auto *op = ctx.make_pooled<Read_write_op>(ctx);
    op->header.call = &Read_write_op::on_complete;

    auto *sqe = ctx.sqe();
    io_uring_prep_read(sqe, fd, buf.data(), buf.size(), offset);
    io_uring_sqe_set_data(sqe, &op->header);
    return make_async_future(op);
}

export auto write(rio::context &ctx, int fd, std::span<const char> buf, size_t offset = 0)
{
    auto *op = ctx.make_pooled<Read_write_op>(ctx);
    op->header.call = &Read_write_op::on_complete;

    auto *sqe = ctx.sqe();
    io_uring_prep_write(sqe, fd, const_cast<char *>(buf.data()), buf.size(), offset);
    io_uring_sqe_set_data(sqe, &op->header);
    return make_async_future(op);
}

struct Timer_payload
{
    __kernel_timespec ts{};
};

using Timer_op = Basic_op<void, Timer_payload>;

struct Timer_op_impl : Timer_op
{
    using Timer_op::Timer_op;

    static void on_complete(rio::internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<Timer_op_impl *>(ptr);
        if (res == -ETIME || res == 0)
            op_detail::finish_success(self);
        else if (res == -ECANCELED)
            op_detail::finish_error(self, std::make_error_code(std::errc::operation_canceled));
        else
            op_detail::finish_error(self, std::error_code(-res, std::system_category()));
    }
};

export struct Link_timeout_req
{
    rio::internals::uring_request_header header{};
    rio::context *ctx{};
    __kernel_timespec ts{};

    explicit Link_timeout_req(rio::context &c) : ctx(&c)
    {}

    static void on_complete(rio::internals::uring_request_header *ptr, int)
    {
        auto *self = reinterpret_cast<Link_timeout_req *>(ptr);
        self->ctx->recycle(self);
    }
};

export template <typename Rep, typename Period>
auto wake_up_after(rio::context &ctx, std::chrono::duration<Rep, Period> d)
{
    using namespace std::chrono;
    auto sec = duration_cast<seconds>(d);
    auto nsec = duration_cast<nanoseconds>(d - sec);

    auto *op = ctx.make_pooled<Timer_op_impl>(ctx);
    op->header.call = &Timer_op_impl::on_complete;
    op->payload.ts = {.tv_sec = static_cast<long long>(sec.count()), .tv_nsec = static_cast<long long>(nsec.count())};

    auto *sqe = ctx.sqe();
    io_uring_prep_timeout(sqe, &op->payload.ts, 0, 0);
    io_uring_sqe_set_data(sqe, &op->header);
    return make_async_future(op);
}

// Composite IO Operations
export struct Write_all_impl
{
    rio::context *ctx;
    int fd;
    std::span<const char> remaining{};
    std::optional<decltype(read(std::declval<rio::context &>(), 0, {}))> curr_write{};

    auto poll() -> rio::fut::res<void>
    {
        while (true) {
            if (remaining.empty())
                return rio::fut::res<void>::ready();
            if (!curr_write)
                curr_write.emplace(write(*ctx, fd, remaining));

            auto r = curr_write->poll();
            if (r.state == rio::fut::status::pending)
                return rio::fut::res<void>::pending();
            if (r.state == rio::fut::status::error)
                return rio::fut::res<void>::error(r.err);

            remaining = remaining.subspan(*r.value);
            curr_write.reset();
        }
    }
};

export struct Read_till_full_impl
{
    rio::context *ctx;
    int fd;
    std::span<char> remaining{};
    std::optional<decltype(read(std::declval<rio::context &>(), 0, {}))> curr_read{};

    auto poll() -> rio::fut::res<void>
    {
        while (true) {
            if (remaining.empty())
                return rio::fut::res<void>::ready();
            if (!curr_read)
                curr_read.emplace(read(*ctx, fd, remaining));

            auto r = curr_read->poll();
            if (r.state == rio::fut::status::pending)
                return rio::fut::res<void>::pending();
            if (r.state == rio::fut::status::error)
                return rio::fut::res<void>::error(r.err);

            size_t n = *r.value;
            if (n == 0)
                return rio::fut::res<void>::error(std::make_error_code(std::errc::broken_pipe));

            remaining = remaining.subspan(n);
            curr_read.reset();
        }
    }
};

export struct Read_till_eof_impl
{
    rio::context *ctx;
    int fd;
    std::string out{};
    char chunk[4096]{};
    std::optional<decltype(read(std::declval<rio::context &>(), 0, {}))> curr_read{};

    auto poll() -> rio::fut::res<std::string>
    {
        while (true) {
            if (!curr_read)
                curr_read.emplace(read(*ctx, fd, std::span{chunk}));

            auto r = curr_read->poll();
            if (r.state == rio::fut::status::pending)
                return rio::fut::res<std::string>::pending();
            if (r.state == rio::fut::status::error)
                return rio::fut::res<std::string>::error(r.err);

            size_t n = *r.value;
            curr_read.reset();
            if (n == 0)
                return rio::fut::res<std::string>::ready(std::move(out));
            out.append(chunk, n);
        }
    }
};

export struct Read_till_impl
{
    rio::context *ctx;
    int fd;
    char delim{};
    std::string out{};
    char c{};
    std::optional<decltype(read(std::declval<rio::context &>(), 0, {}))> curr_read{};

    auto poll() -> rio::fut::res<std::string>
    {
        while (true) {
            if (!curr_read)
                curr_read.emplace(read(*ctx, fd, std::span{&c, 1}));

            auto r = curr_read->poll();
            if (r.state == rio::fut::status::pending)
                return rio::fut::res<std::string>::pending();
            if (r.state == rio::fut::status::error)
                return rio::fut::res<std::string>::error(r.err);

            curr_read.reset();
            if (*r.value == 0)
                return rio::fut::res<std::string>::ready(std::move(out));
            if (c == delim)
                return rio::fut::res<std::string>::ready(std::move(out));

            out.push_back(c);
        }
    }
};

export template <typename Rep, typename Period>
auto stop_read_after(rio::context &ctx, int fd, std::span<char> buf, std::chrono::duration<Rep, Period> timeout)
{
    using namespace std::chrono;
    struct Timed_read_op : Read_write_op
    {
        using Read_write_op::Read_write_op;

        static void on_complete(rio::internals::uring_request_header *ptr, int res)
        {
            auto *self = reinterpret_cast<Timed_read_op *>(ptr);
            if (res == -ECANCELED)
                op_detail::finish_error(self, std::make_error_code(std::errc::timed_out));
            else if (res < 0)
                op_detail::finish_error(self, std::error_code(-res, std::system_category()));
            else
                op_detail::finish_success(self, static_cast<std::size_t>(res));
        }
    };

    auto *read_op = ctx.make_pooled<Timed_read_op>(ctx);
    read_op->header.call = &Timed_read_op::on_complete;

    auto *timer_req = ctx.make_pooled<Link_timeout_req>(ctx);
    timer_req->header.call = &Link_timeout_req::on_complete;

    auto sec = duration_cast<seconds>(timeout);
    auto nsec = duration_cast<nanoseconds>(timeout - sec);
    timer_req->ts.tv_sec = sec.count();
    timer_req->ts.tv_nsec = nsec.count();

    auto *sqe_read = ctx.sqe();
    auto *sqe_timer = ctx.sqe();

    io_uring_prep_read(sqe_read, fd, buf.data(), buf.size(), 0);
    io_uring_sqe_set_flags(sqe_read, IOSQE_IO_LINK);
    io_uring_sqe_set_data(sqe_read, &read_op->header);

    io_uring_prep_link_timeout(sqe_timer, &timer_req->ts, 0);
    io_uring_sqe_set_data(sqe_timer, &timer_req->header);

    return make_async_future(read_op);
}

export auto write_all(rio::context &ctx, int fd, std::span<const char> full_buf)
{
    return rio::fut::make(Write_all_impl{&ctx, fd, full_buf}, rio::fut::Call_poll{});
}

export auto read_till_full(rio::context &ctx, int fd, std::span<char> full_buf)
{
    return rio::fut::make(Read_till_full_impl{&ctx, fd, full_buf}, rio::fut::Call_poll{});
}

export auto read_till_eof(rio::context &ctx, int fd)
{
    return rio::fut::make(Read_till_eof_impl{&ctx, fd, {}}, rio::fut::Call_poll{});
}

export auto read_till(rio::context &ctx, int fd, char delim)
{
    return rio::fut::make(Read_till_impl{&ctx, fd, delim, {}, {}}, rio::fut::Call_poll{});
}

export auto read_line(rio::context &ctx, int fd)
{
    return read_till(ctx, fd, '\n');
}

export template <typename Rep, typename Period>
auto stop_read_after(rio::context &ctx, Handle_like_c auto &h, std::span<char> buf, std::chrono::duration<Rep, Period> t)
{
    return stop_read_after(ctx, h.fd.native_handle(), buf, t);
}

export auto read(rio::context &ctx, Handle_like_c auto &h, std::span<char> buf)
{
    return read(ctx, h.fd.native_handle(), buf);
}
export auto write(rio::context &ctx, Handle_like_c auto &h, std::span<const char> buf)
{
    return write(ctx, h.fd.native_handle(), buf);
}
export auto write_all(rio::context &ctx, Handle_like_c auto &h, std::span<const char> b)
{
    return write_all(ctx, h.fd.native_handle(), b);
}
export auto read_till_full(rio::context &ctx, Handle_like_c auto &h, std::span<char> b)
{
    return read_till_full(ctx, h.fd.native_handle(), b);
}
export auto read_till_eof(rio::context &ctx, Handle_like_c auto &h)
{
    return read_till_eof(ctx, h.fd.native_handle());
}
export auto read_till(rio::context &ctx, Handle_like_c auto &h, char d)
{
    return read_till(ctx, h.fd.native_handle(), d);
}
export auto read_line(rio::context &ctx, Handle_like_c auto &h)
{
    return read_line(ctx, h.fd.native_handle());
}

} // namespace rio::fut
