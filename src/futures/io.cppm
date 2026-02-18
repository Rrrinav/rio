module;

#include <liburing.h>
#include <sys/socket.h>

export module rio:fut.io;

import std;
import :context;
import :handle;
import :promise;
import :futures; // Contains rio::fut::Call_poll

namespace rio::fut {

export template <typename T>
concept Handle_like_c = requires(T h) { h.fd.native_handle(); };

// Named poller for basic IO (read/write)
export struct Async_poller
{
    template <typename HandleType>
    auto operator()(HandleType &h) const
    {
        return h.poll();
    }
};

export template <typename T>
struct Async_state : public rio::promise::State<T>
{
    bool io_done = false;
    bool future_dropped = false;
};

export template <typename T>
struct Async_handle
{
    Async_state<T> *ptr = nullptr;

    Async_handle(Async_state<T> *s) : ptr(s)
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
                    delete ptr;
                else
                    ptr->future_dropped = true;
            }
            ptr = other.ptr;
            other.ptr = nullptr;
        }
        return *this;
    }

    ~Async_handle()
    {
        if (!ptr)
            return;
        if (ptr->io_done)
            delete ptr;
        else
            ptr->future_dropped = true;
    }

    auto poll()
    {
        return ptr->poll();
    }
};

export template <typename ValType>
struct Uring_req
{
    rio::internals::uring_request_header header;
    Async_state<ValType> *state;

    static void on_complete(rio::internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<Uring_req *>(ptr);
        rio::Promise<Async_state<ValType>> p{ .state = self->state };

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

export auto read(rio::context &ctx, int fd, std::span<char> buf, size_t offset = 0)
{
    using Val_type = std::size_t;
    auto *s = new Async_state<Val_type>();
    auto *req = new Uring_req<Val_type>{ .header = { .call = &Uring_req<Val_type>::on_complete }, .state = s };

    auto *sqe = ctx.sqe();
    io_uring_prep_read(sqe, fd, buf.data(), buf.size(), offset);
    io_uring_sqe_set_data(sqe, &req->header);

    ctx.submit();
    return rio::Future(Async_handle{ s }, Async_poller{});
}

export auto write(rio::context &ctx, int fd, std::span<const char> buf, size_t offset = 0)
{
    using Val_type = std::size_t;
    auto *s = new Async_state<Val_type>();
    auto *req = new Uring_req<Val_type>{ .header = { .call = &Uring_req<Val_type>::on_complete }, .state = s };

    auto *sqe = ctx.sqe();
    io_uring_prep_write(sqe, fd, const_cast<char *>(buf.data()), buf.size(), offset);
    io_uring_sqe_set_data(sqe, &req->header);

    ctx.submit();
    return rio::Future(Async_handle{ s }, Async_poller{});
}

struct Timer_req
{
    rio::internals::uring_request_header header;
    Async_state<void> *state;
    __kernel_timespec ts;

    static void on_complete(rio::internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<Timer_req *>(ptr);
        rio::Promise<Async_state<void>> p{ .state = self->state };

        if (res == -ETIME || res == 0)
            p.resolve();
        else if (res == -ECANCELED)
            p.reject(std::make_error_code(std::errc::operation_canceled));
        else
            p.reject(std::error_code(-res, std::system_category()));

        self->state->io_done = true;
        if (self->state->future_dropped)
            delete self->state;
        delete self;
    }
};

export struct Link_timeout_req
{
    rio::internals::uring_request_header header{};
    __kernel_timespec ts{};
    static void on_complete(rio::internals::uring_request_header *ptr, int)
    {
        delete reinterpret_cast<Link_timeout_req *>(ptr);
    }
};

export template <typename Rep, typename Period>
auto wake_up_after(rio::context &ctx, std::chrono::duration<Rep, Period> d)
{
    using namespace std::chrono;
    auto sec = duration_cast<seconds>(d);
    auto nsec = duration_cast<nanoseconds>(d - sec);

    auto *s = new Async_state<void>();
    auto *req = new Timer_req{ .header = { .call = &Timer_req::on_complete },
        .state = s,
        .ts = { .tv_sec = static_cast<long long>(sec.count()), .tv_nsec = static_cast<long long>(nsec.count()) } };

    auto *sqe = ctx.sqe();
    io_uring_prep_timeout(sqe, &req->ts, 0, 0);
    io_uring_sqe_set_data(sqe, &req->header);
    ctx.submit();
    return rio::Future(Async_handle{ s }, Async_poller{});
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
                curr_read.emplace(read(*ctx, fd, std::span{ chunk }));

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
                curr_read.emplace(read(*ctx, fd, std::span{ &c, 1 }));

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
    using Val_type = std::size_t;

    auto *s = new Async_state<Val_type>();

    auto *read_req = new Uring_req<Val_type>{
        .header = { .call =
                        [](rio::internals::uring_request_header *ptr, int res) {
                            auto *self = reinterpret_cast<Uring_req<Val_type> *>(ptr);
                            if (res == -ECANCELED) {
                                rio::Promise<Async_state<Val_type>> p{ .state = self->state };
                                p.reject(std::make_error_code(std::errc::timed_out));
                                self->state->io_done = true;
                                if (self->state->future_dropped)
                                    delete self->state;
                                delete self;
                            } else {
                                Uring_req<Val_type>::on_complete(ptr, res);
                            }
                        } },
        .state = s
    };

    auto *timer_req = new Link_timeout_req{ .header = { .call = &Link_timeout_req::on_complete } };

    auto sec = duration_cast<seconds>(timeout);
    auto nsec = duration_cast<nanoseconds>(timeout - sec);
    timer_req->ts.tv_sec = sec.count();
    timer_req->ts.tv_nsec = nsec.count();

    auto *sqe_read = ctx.sqe();
    auto *sqe_timer = ctx.sqe();

    io_uring_prep_read(sqe_read, fd, buf.data(), buf.size(), 0);
    io_uring_sqe_set_flags(sqe_read, IOSQE_IO_LINK);
    io_uring_sqe_set_data(sqe_read, &read_req->header);

    io_uring_prep_link_timeout(sqe_timer, &timer_req->ts, 0);
    io_uring_sqe_set_data(sqe_timer, &timer_req->header);

    ctx.submit();
    return rio::Future(Async_handle{ s }, Async_poller{});
}

// ==========================================
// API Functions (Using Named Call_poll)
// ==========================================

export auto write_all(rio::context &ctx, int fd, std::span<const char> full_buf)
{
    return rio::fut::make(Write_all_impl{ &ctx, fd, full_buf }, rio::fut::Call_poll{});
}

export auto read_till_full(rio::context &ctx, int fd, std::span<char> full_buf)
{
    return rio::fut::make(Read_till_full_impl{ &ctx, fd, full_buf }, rio::fut::Call_poll{});
}

export auto read_till_eof(rio::context &ctx, int fd)
{
    return rio::fut::make(Read_till_eof_impl{ &ctx, fd, {} }, rio::fut::Call_poll{});
}

export auto read_till(rio::context &ctx, int fd, char delim)
{
    return rio::fut::make(Read_till_impl{ &ctx, fd, delim, {}, {} }, rio::fut::Call_poll{});
}

export auto read_line(rio::context &ctx, int fd)
{
    return read_till(ctx, fd, '\n');
}

export template <typename Rep, typename Period>
auto stop_read_after(
    rio::context &ctx, Handle_like_c auto &h, std::span<char> buf, std::chrono::duration<Rep, Period> t)
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
