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

export struct Async_poller
{
    template <typename HandleType>
    auto operator()(HandleType &h) const { return h.poll(); }
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

    Async_handle(Async_state<T> *s) : ptr(s) {}

    Async_handle(Async_handle &&other) noexcept : ptr(other.ptr) { other.ptr = nullptr; }

    Async_handle &operator=(Async_handle &&other) noexcept
    {
        if (this != &other)
        {
            if (ptr)
            {
                if (ptr->io_done) delete ptr;
                else ptr->future_dropped = true;
            }
            ptr = other.ptr;
            other.ptr = nullptr;
        }
        return *this;
    }

    ~Async_handle()
    {
        if (!ptr) return;
        if (ptr->io_done) delete ptr;
        else ptr->future_dropped = true;
    }

    auto poll() { return ptr->poll(); }
};

export template <typename ValType>
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

export auto read(rio::context &ctx, int fd, std::span<char> buf)
{
    using Val_type = std::size_t;
    auto *s = new Async_state<Val_type>();
    auto *req = new Uring_req<Val_type>{.header = {.call = &Uring_req<Val_type>::on_complete}, .state = s};

    auto *sqe = ctx.sqe();
    io_uring_prep_read(sqe, fd, buf.data(), buf.size(), -1);
    io_uring_sqe_set_data(sqe, &req->header);

    ctx.submit();
    return rio::Future(Async_handle{s}, Async_poller{});
}

export auto write(rio::context &ctx, int fd, std::span<const char> buf)
{
    using Val_type = std::size_t;
    auto *s = new Async_state<Val_type>();
    auto *req = new Uring_req<Val_type>{.header = {.call = &Uring_req<Val_type>::on_complete}, .state = s};

    auto *sqe = ctx.sqe();
    io_uring_prep_write(sqe, fd, const_cast<char *>(buf.data()), buf.size(), -1);
    io_uring_sqe_set_data(sqe, &req->header);

    ctx.submit();
    return rio::Future(Async_handle{s}, Async_poller{});
}

export template <typename HandleT>
requires requires(HandleT h) { h.fd.native_handle(); }
auto read(rio::context &ctx, HandleT &h, std::span<char> buf) { return read(ctx, h.fd.native_handle(), buf); }

export template <typename HandleT>
requires requires(HandleT h) { h.fd.native_handle(); }
auto write(rio::context &ctx, HandleT &h, std::span<const char> buf) { return write(ctx, h.fd.native_handle(), buf); }

struct Timer_req
{
    rio::internals::uring_request_header header;
    Async_state<void> *state;
    __kernel_timespec ts;

    static void on_complete(rio::internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<Timer_req *>(ptr);
        rio::Promise<Async_state<void>> p{.state = self->state};

        if (res == -ETIME || res == 0) p.resolve();
        else if (res == -ECANCELED) p.reject(std::make_error_code(std::errc::operation_canceled));
        else p.reject(std::error_code(-res, std::system_category()));

        self->state->io_done = true;
        if (self->state->future_dropped) delete self->state;
        delete self;
    }
};

export struct Link_timeout_req
{
    rio::internals::uring_request_header header{};
    __kernel_timespec ts{};
    static void on_complete(rio::internals::uring_request_header *ptr, int) { delete reinterpret_cast<Link_timeout_req *>(ptr); }
};

export template <typename Rep, typename Period>
auto wake_up_after(rio::context &ctx, std::chrono::duration<Rep, Period> d)
{
    using namespace std::chrono;
    auto sec = duration_cast<seconds>(d);
    auto nsec = duration_cast<nanoseconds>(d - sec);

    auto *s = new Async_state<void>();
    auto *req = new Timer_req{.header = {.call = &Timer_req::on_complete},
        .state = s, .ts = {.tv_sec = static_cast<long long>(sec.count()), .tv_nsec = static_cast<long long>(nsec.count())}};

    auto *sqe = ctx.sqe();
    io_uring_prep_timeout(sqe, &req->ts, 0, 0);
    io_uring_sqe_set_data(sqe, &req->header);
    ctx.submit();
    return rio::Future(Async_handle{s}, Async_poller{});
}

export template <typename Fut, typename Rep, typename Period>
auto stop_after(rio::context &ctx, Fut &&f, std::chrono::duration<Rep, Period> d)
{
    auto timer = rio::fut::wake_up_after(ctx, d);
    return first_of(std::move(f), std::move(timer)).then([](auto result_var) {
        using Val_t = typename std::decay_t<Fut>::value_type;
        if (result_var.index() == 0) 
            if constexpr (std::is_void_v<Val_t>) return rio::fut::res<Val_t>::ready();
            else return rio::fut::res<Val_t>::ready(std::move(std::get<0>(result_var)));
        else 
            return rio::fut::res<Val_t>::error(std::make_error_code(std::errc::timed_out));
    });
}

export struct Write_all_impl
{
    rio::context *ctx;
    int fd;
    std::span<const char> remaining;

    using WriteFut = decltype(rio::fut::write(std::declval<rio::context &>(), 0, std::span<const char>{}));
    std::optional<WriteFut> curr_write;

    Write_all_impl(rio::context &c, int f, std::span<const char> b) : ctx(&c), fd(f), remaining(b) {}

    Write_all_impl(Write_all_impl &&other) noexcept
        : ctx(other.ctx), fd(other.fd), remaining(other.remaining), curr_write(std::move(other.curr_write)) {}

    Write_all_impl &operator=(Write_all_impl &&other) noexcept
    {
        if (this != &other)
        {
            ctx = other.ctx;
            fd = other.fd;
            remaining = other.remaining;
            curr_write = std::move(other.curr_write);
        }
        return *this;
    }

    auto poll() -> rio::fut::res<void>
    {
        while (true)
        {
            if (remaining.empty())
                return rio::fut::res<void>::ready();

            if (!curr_write)
                curr_write.emplace(rio::fut::write(*ctx, fd, remaining));

            auto r = curr_write->poll();
            if (r.state == rio::fut::status::pending)
                return rio::fut::res<void>::pending();
            if (r.state == rio::fut::status::error)
                return rio::fut::res<void>::error(r.err);

            std::size_t n = *r.value;
            curr_write.reset();

            // Advance buffer
            remaining = remaining.subspan(n);
        }
    }
};

export auto write_all(rio::context &ctx, int fd, std::span<const char> full_buf)
{
    return rio::fut::make(Write_all_impl{ctx, fd, full_buf}, [](Write_all_impl &s) { return s.poll(); });
}

export struct Read_exactly_impl
{
    rio::context *ctx;
    int fd;
    std::span<char> remaining;

    using ReadFut = decltype(rio::fut::read(std::declval<rio::context &>(), 0, std::span<char>{}));
    std::optional<ReadFut> curr_read;

    Read_exactly_impl(rio::context &c, int f, std::span<char> b) : ctx(&c), fd(f), remaining(b) {}

    Read_exactly_impl(Read_exactly_impl &&other) noexcept
        : ctx(other.ctx), fd(other.fd), remaining(other.remaining), curr_read(std::move(other.curr_read)) {}

    Read_exactly_impl &operator=(Read_exactly_impl &&other) noexcept
    {
        if (this != &other)
        {
            ctx = other.ctx;
            fd = other.fd;
            remaining = other.remaining;
            curr_read = std::move(other.curr_read);
        }
        return *this;
    }

    auto poll() -> rio::fut::res<void>
    {
        while (true)
        {
            if (remaining.empty())
                return rio::fut::res<void>::ready();

            if (!curr_read)
                curr_read.emplace(rio::fut::read(*ctx, fd, remaining));

            auto r = curr_read->poll();
            if (r.state == rio::fut::status::pending)
                return rio::fut::res<void>::pending();
            if (r.state == rio::fut::status::error)
                return rio::fut::res<void>::error(r.err);

            std::size_t n = *r.value;
            curr_read.reset();

            if (n == 0)
                return rio::fut::res<void>::error(std::make_error_code(std::errc::broken_pipe));  // Unexpected EOF

            remaining = remaining.subspan(n);
        }
    }
};

export auto read_exactly(rio::context &ctx, int fd, std::span<char> full_buf)
{
    return rio::fut::make(Read_exactly_impl{ctx, fd, full_buf}, [](Read_exactly_impl &s) { return s.poll(); });
}

// "Read Till EOF" - Slurp
export struct Read_till_eof_impl
{
    rio::context *ctx;
    int fd;
    std::string out;
    char chunk[4096];

    using ReadFut = decltype(rio::fut::read(std::declval<rio::context &>(), 0, std::span<char>{}));
    std::optional<ReadFut> curr_read;

    Read_till_eof_impl(rio::context &c, int f) : ctx(&c), fd(f) { out.reserve(4096); }

    Read_till_eof_impl(Read_till_eof_impl &&other) noexcept
        : ctx(other.ctx), fd(other.fd), out(std::move(other.out)), curr_read(std::move(other.curr_read)) {}

    Read_till_eof_impl &operator=(Read_till_eof_impl &&other) noexcept
    {
        if (this != &other)
        {
            ctx = other.ctx;
            fd = other.fd;
            out = std::move(other.out);
            curr_read = std::move(other.curr_read);
        }
        return *this;
    }

    auto poll() -> rio::fut::res<std::string>
    {
        while (true)
        {
            if (!curr_read)
                curr_read.emplace(rio::fut::read(*ctx, fd, std::span{chunk}));

            auto r = curr_read->poll();
            if (r.state == rio::fut::status::pending)
                return rio::fut::res<std::string>::pending();
            if (r.state == rio::fut::status::error)
                return rio::fut::res<std::string>::error(r.err);

            std::size_t n = *r.value;
            curr_read.reset();

            if (n == 0)
                return rio::fut::res<std::string>::ready(std::move(out));

            out.append(chunk, n);
        }
    }
};

export auto read_till_eof(rio::context &ctx, int fd)
{
    return rio::fut::make(Read_till_eof_impl{ctx, fd}, [](Read_till_eof_impl& s) { return s.poll(); });
}

// Generic Handle Wrappers
export template <typename HandleT>
requires requires(HandleT h) { h.fd.native_handle(); }
auto write_all(rio::context &ctx, HandleT &h, std::span<const char> b) { return write_all(ctx, h.fd.native_handle(), b); }

export template <typename HandleT>
requires requires(HandleT h) { h.fd.native_handle(); }
auto read_exactly(rio::context &ctx, HandleT &h, std::span<char> b) { return read_exactly(ctx, h.fd.native_handle(), b); }

export template <typename HandleT>
requires requires(HandleT h) { h.fd.native_handle(); }
auto read_till_eof(rio::context &ctx, HandleT &h) { return read_till_eof(ctx, h.fd.native_handle()); }

} // namespace rio::fut
