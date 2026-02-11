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

template <typename Val_type>
struct Uring_req
{
    rio::internals::uring_request_header header;
    Async_state<Val_type> *state;
    static void on_complete(rio::internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<Uring_req *>(ptr);
        rio::Promise<Async_state<Val_type>> p{.state = self->state};
        if (res < 0)
            p.reject(std::error_code(-res, std::system_category()));
        else
            p.resolve(static_cast<Val_type>(res));
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
            p.resolve(Accept_result{.client = rio::Tcp_socket::attach(res), .address = self->client_addr});
        }
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
    using Val_type = std::size_t;
    auto *s = new Async_state<Val_type>();
    auto *req = new Uring_req<Val_type>{.header = {.call = &Uring_req<Val_type>::on_complete}, .state = s};
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
    using Val_type = Accept_result;
    auto *s = new Async_state<Val_type>();
    auto *req = new Accept_req{.header = {.call = &Accept_req::on_complete}, .state = s, .client_addr = {}, .addr_len = sizeof(sockaddr_storage)};
    auto *sqe = ctx.sqe();
    io_uring_prep_accept(sqe, listener.fd.native_handle(), reinterpret_cast<sockaddr *>(&req->client_addr.storage), &req->addr_len, 0);
    io_uring_sqe_set_data(sqe, &req->header);
    ctx.submit();
    return rio::Future(Async_handle{s}, Async_poller{});
}

struct Timer_req
{
    rio::internals::uring_request_header header;
    Async_state<void> *state;
    __kernel_timespec ts;

    static void on_complete(rio::internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<Timer_req *>(ptr);
        rio::Promise<Async_state<void>> p{.state = self->state};

        // io_uring returns -ETIME if the timer expired successfully.
        if (res == -ETIME || res == 0)
        {
            p.resolve();
        }
        else if (res == -ECANCELED)
        {
            // If we implement cancellation later
            p.reject(std::make_error_code(std::errc::operation_canceled));
        }
        else
        {
            p.reject(std::error_code(-res, std::system_category()));
        }

        // Cleanup
        self->state->io_done = true;
        if (self->state->future_dropped)
            delete self->state;
        delete self;
    }
};

export template <typename Rep, typename Period>
auto wake_up_after(rio::context &ctx, std::chrono::duration<Rep, Period> d)
{
    using namespace std::chrono;

    auto sec = duration_cast<seconds>(d);
    auto nsec = duration_cast<nanoseconds>(d - sec);

    auto *s = new Async_state<void>();

    auto *req = new Timer_req{.header = {.call = &Timer_req::on_complete},
        .state = s,
        .ts = {.tv_sec = static_cast<long long>(sec.count()), .tv_nsec = static_cast<long long>(nsec.count())}};

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

        rio::fut::res<Val_t> outcome;

        if (result_var.index() == 0)
            if constexpr (std::is_void_v<Val_t>)
                outcome = rio::fut::res<Val_t>::ready();
            else
                outcome = rio::fut::res<Val_t>::ready(std::move(std::get<0>(result_var)));
        else
            outcome = rio::fut::res<Val_t>::error(std::make_error_code(std::errc::timed_out));

        return rio::fut::make(std::move(outcome), [](rio::fut::res<Val_t> &s) { return std::move(s); });
    });
}

struct Link_timeout_req
{
    rio::internals::uring_request_header header{};
    __kernel_timespec ts{};

    static void on_complete(rio::internals::uring_request_header *ptr, int /*res*/)
    {
        // We interpret the result:
        // -ETIME:     We fired, killing the main op.
        // -ECANCELED: The main op finished, killing us.
        delete reinterpret_cast<Link_timeout_req *>(ptr);
    }
};

export template <typename Rep, typename Period>
auto read(rio::context &ctx, rio::handle fd, std::span<char> buf, std::chrono::duration<Rep, Period> timeout)
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
auto accept(rio::context &ctx, rio::Tcp_socket &listener, std::chrono::duration<Rep, Period> timeout)
{
    using namespace std::chrono;
    using ValType = Accept_result;

    auto *s = new Async_state<ValType>();

    // 1. Accept Request with Cancel detection
    auto *accept_req = new Accept_req{ .header = { .call = [](rio::internals::uring_request_header *ptr, int res) {
        auto *self = reinterpret_cast<Accept_req *>(ptr);
        if (res == -ECANCELED) {
            rio::Promise<Async_state<ValType>> p{.state = self->state};
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
