import std;
import rio;

using namespace std::chrono_literals;

using Accept_fut = decltype(rio::fut::accept(std::declval<rio::context &>(), std::declval<rio::Tcp_socket &>()));

struct Session
{
    rio::context &ctx;
    std::optional<Accept_fut> pending; // the in-flight op, held by the task
};

int main()
{
    rio::context IO{256};
    rio::Runtime rt{&IO};

    auto listen_res = rio::Tcp_socket::open_and_listen("127.0.0.1", 0, rio::s_opt::async_server_v4);
    if (!listen_res) {
        return 1;
    }
    auto [listener, _addr] = std::move(*listen_res);

    // 1. Timeout with recovery: the kernel cancels the op at the
    //    deadline, or_else handles the resulting timed_out error.
    {
        auto res = rt.block_on(rio::fut::accept(IO, listener).timeout(150ms).or_else([](std::error_code ec) {
            std::println("1) timeout fired, recovered from: {}", ec.message());
            return rio::fut::error<rio::Tcp_accept_result>(std::make_error_code(std::errc::operation_canceled));
        }));
        std::println("1) block_on -> {}", res.error().message());
    }

    // 2. Arbitrary cancel: grab the op handle and cancel at any time.
    //    The op reports error_code operation_canceled.
    {
        auto f = rio::fut::accept(IO, listener);
        f.data.cancel();
        auto res = rt.block_on(std::move(f));
        std::println("2) manual cancel -> {}", res.error().message());
    }

    // 3. Cancel through a composition chain: rio::cancel reaches the
    //    innermost op via tag_invoke forwarding (then/map/or_else).
    {
        auto f = rio::fut::accept(IO, listener).map([](rio::Tcp_accept_result r) { return r; });
        rio::cancel(f.data);
        auto res = rt.block_on(std::move(f));
        std::println("3) chain cancel -> {}", res.error().message());
    }

    // 4. Scheduled cancel: cancel the op at an absolute deadline,
    //    without .timeout()'s timed_out mapping (raw operation_canceled).
    {
        auto f = rio::fut::accept(IO, listener);
        f.data.cancel_after(std::chrono::steady_clock::now() + 120ms);
        auto res = rt.block_on(std::move(f));
        std::println("4) cancel_after -> {}", res.error().message());
    }

    // 5. Arbitrary cancel from another task: task A parks an in-flight
    //    accept in shared state, task B cancels it on a timer, and task
    //    A observes operation_canceled.
    //
    //    NOTE: a loop body must return a future that stays PENDING until
    //    the next check (here: a 50ms timer). Returning an already-ready
    //    future makes the loop spin forever inside one poll().
    {
        auto sess = std::make_shared<Session>(Session{.ctx = IO, .pending = {}});
        sess->pending.emplace(rio::fut::accept(IO, listener));

        rt.spawn(rio::fut::loop(sess, [](std::shared_ptr<Session> s) {
            return rio::fut::wake_up_after(s->ctx, 50ms).then([s]() {
                auto r = rio::poll(*s->pending);
                if (r.state == rio::fut::status::ready) {
                    std::println("5) task A: ACCEPTED");
                    return rio::fut::error<std::shared_ptr<Session>>(std::make_error_code(std::errc::broken_pipe));
                }
                if (r.state == rio::fut::status::error) {
                    std::println("5) task A: op ended -> {}", r.err.message());
                    return rio::fut::error<std::shared_ptr<Session>>(std::make_error_code(std::errc::broken_pipe));
                }
                return rio::fut::ready(s);
            });
        }));

        rt.spawn(
            rio::fut::wake_up_after(IO, 150ms)
                .then([sess]() {
                    std::println("5) task B: cancelling task A's op");
                    rio::cancel(*sess->pending);
                    return rio::fut::ready();
                })
                .or_else([](std::error_code ec) {
                    std::println("5) task B timer error: {}", ec.message());
                    return rio::fut::ready();
                }));

        rt.start(); // runs until both tasks die
    }

    // 6. Per-iteration deadlines inside a task loop: each iteration
    //    accepts with a timeout; on timeout the loop retries, then
    //    gives up. The chain's value type is the loop state.
    {
        auto attempts = std::make_shared<std::atomic<int>>(0);
        rt.spawn(rio::fut::loop(attempts, [&IO, &listener](std::shared_ptr<std::atomic<int>> st) {
            return rio::fut::accept(IO, listener)
                .timeout(300ms)
                .map([st](rio::Tcp_accept_result) {
                    std::println("6) accepted!");
                    return st;
                })
                .or_else([st](std::error_code ec) {
                    if (ec == std::errc::timed_out) {
                        if (st->fetch_add(1) + 1 < 2) {
                            std::println("6) timeout, retrying...");
                            return rio::fut::ready(st);
                        }
                        std::println("6) gave up after 2 timeouts");
                    }
                    return rio::fut::error<std::shared_ptr<std::atomic<int>>>(std::make_error_code(std::errc::broken_pipe));
                });
        }));
        rt.start();
    }

    // ---------------------------------------------------------------
    // 7. Async teardown (shutdown + close): a supervisor shuts the
    //    listener down to abort a parked accept (it wakes with an
    //    error), then the listener is closed through the ring.
    //    shutdown() is fire-and-forget here: the future is dropped
    //    immediately, but the op still runs (cancel_on_drop = false).
    // ---------------------------------------------------------------
    {
        rt.spawn(rio::fut::accept(IO, listener)
            .then([](rio::Tcp_accept_result) {
                std::println("7) ACCEPTED (unexpected)");
                return rio::fut::ready();
            })
            .or_else([](std::error_code ec) {
                std::println("7) parked accept woke with: {}", ec.message());
                return rio::fut::ready();
            }));

        rt.spawn(rio::fut::wake_up_after(IO, 150ms).then([&IO, &listener]() {
            std::println("7) supervisor: shutdown(listener), dropping future");
            rio::fut::shutdown(IO, listener.fd.native_handle(), static_cast<int>(rio::Tcp_socket::shut::both));
            return rio::fut::ready();
        }));

        rt.start(); // runs until the accept task dies

        auto close_res = rt.block_on(rio::fut::close(IO, std::move(listener)));
        if (close_res)
            std::println("7) listener closed via IORING_OP_CLOSE");
        else
            std::println("7) listener close failed: {}", close_res.error().message());
    }

    std::println("all done");
    return 0;
}
