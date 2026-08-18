import std;
import rio;

using namespace std::chrono_literals;

// catch_exception: ONE catch point per chain. Any throw from any callback
// upstream (then/map/or_else/loop bodies) unwinds into the single try/catch
// inside the catch_exception combinator at the bottom of the chain.
// Handler signature: void/Value/Future( std::exception_ptr )
// error_code stays with .or_else - two channels, two combinators.

struct Protocol_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct Session
{
    rio::context &ctx;
    rio::Tcp_socket sock;
    rio::Async_buffered_reader<rio::Tcp_socket> reader;
    std::string pending_reply{};

    Session(rio::context &c, rio::Tcp_socket s) : ctx(c), sock(std::move(s)), reader(c, sock)
    {}
};

using Loop_state = std::shared_ptr<Session>;

// protocol: "ECHO:<payload>\n" -> reply "<payload>\n"; anything else throws
std::string parse(const std::string &line)
{
    if (!line.starts_with("ECHO:")) {
        throw Protocol_error("malformed line: \"" + line + "\"");
    }
    return line.substr(5) + "\n";
}

auto session_chain(std::shared_ptr<Session> s)
{
    return rio::fut::loop(
               s,
               [s](std::shared_ptr<Session> st) {
                   // NOTE: branchy callbacks must return ready()/error() of the SAME
                   // type (here: Immediate futures of Loop_state). Actual op chains
                   // go in a separate .then below, which keeps the chain type fixed.
                   return rio::fut::buff::read_line(st->reader)
                       .then([st](std::optional<std::string> line) {
                           if (!line) { // client closed -> end session normally
                               return rio::fut::error<Loop_state>(std::make_error_code(std::errc::broken_pipe));
                           }
                           st->pending_reply = parse(*line); // THROWS Protocol_error on garbage
                           return rio::fut::ready(st);       // same type as error_t<Loop_state>
                       })
                       .then([st](std::shared_ptr<Session>) {
                           return rio::fut::write_all(st->ctx, st->sock, std::span<const char>{st->pending_reply}).then([st]() {
                               return rio::fut::ready(st);
                           });
                       });
               })
        .catch_exception([s](std::exception_ptr e) {
            try {
                std::rethrow_exception(e);
            } catch (const Protocol_error &ex) {
                std::println("[server] session killed by protocol error: {}", ex.what());
            } catch (...) {
                std::println("[server] session killed by unknown exception");
            }
            // the handler returns a NEW future: async teardown
            std::println("[server] teardown: shutdown + close");
            return rio::fut::shutdown(s->ctx, s->sock, rio::Tcp_socket::shut::both)
                .or_else([](std::error_code ec) {
                    std::println("[server] shutdown note: {}", ec.message());
                    return rio::fut::ready();
                })
                .then([s]() { return rio::fut::close(s->ctx, std::move(s->sock)); });
        });
}

int main()
{
    rio::context IO{128};
    rio::Runtime rt{&IO};

    auto listen_res = rio::Tcp_socket::open_and_listen("127.0.0.1", 0, rio::s_opt::async_server_v4);
    if (!listen_res) {
        std::println(std::cerr, "listen failed");
        return 1;
    }
    auto [listener, _addr] = std::move(*listen_res);
    rio::address addr = *listener.local_endpoint();

    // 1. A throw anywhere upstream lands in the single catch_exception at
    //    the bottom; the handler recovers with a plain value.
    {
        auto res = rt.block_on(
            rio::fut::ready(1).then([](int) -> rio::fut::ready_t<int> { throw std::runtime_error("boom"); }).catch_exception([](std::exception_ptr) {
                return 42;
            }));
        std::println("1) recover with value -> {}", res ? "42" : res.error().message());
    }

    // 2. Void handler on a void chain: swallow, the chain just completes.
    {
        auto res = rt.block_on(
            rio::fut::ready().then([]() -> rio::fut::ready_t<void> { throw std::runtime_error("boom"); }).catch_exception([](std::exception_ptr) {}));
        std::println("2) swallow -> {}", res ? "ready" : res.error().message());
    }

    // 3. The two channels are separate and compose: or_else sees error_codes,
    //    catch_exception sees exceptions.
    {
        auto res = rt.block_on(
            rio::fut::error<int>(std::make_error_code(std::errc::broken_pipe))
                .or_else([](std::error_code ec) {
                    std::println("3) or_else saw: {}", ec.message());
                    return rio::fut::ready(1);
                })
                .then([](int) -> rio::fut::ready_t<int> { throw std::runtime_error("then threw"); })
                .catch_exception([](std::exception_ptr) { return 99; }));
        std::println("3) catch_exception saw: {}", res ? "99" : res.error().message());
    }

    // 4. Real usage: a protocol server. One session = one chain:
    //      loop { read line -> parse (may throw) -> echo } .catch_exception(teardown)
    //    The parser throws Protocol_error on garbage; the single catch at the
    //    bottom logs it and returns the async teardown chain as its recovery.
    {
        auto client_res = rio::Tcp_socket::open(rio::s_opt::client_v4);
        if (!client_res) {
            std::println(std::cerr, "client open failed");
            return 1;
        }
        auto client = std::move(*client_res);
        auto conn = rio::io::connect(client, addr);
        if (!conn) {
            std::println(std::cerr, "connect failed: {}", conn.error().message());
            return 1;
        }

        // server: accept once, run the session chain
        rt.spawn(
            rio::fut::accept(IO, listener)
                .then([&IO](rio::Tcp_accept_result r) {
                    auto s = std::make_shared<Session>(IO, std::move(r.client));
                    return session_chain(s);
                })
                .or_else([](std::error_code ec) {
                    std::println("[server] accept failed: {}", ec.message());
                    return rio::fut::ready();
                }));

        // client: valid line -> echo, then garbage -> server throws and closes
        rt.spawn(
            rio::fut::write_all(IO, client, std::string_view{"ECHO:hello\n"})
                .then([&client, &IO]() { return rio::fut::read_line(IO, client); })
                .then([&client, &IO](std::string echo) {
                    std::println("[client] echo: \"{}\"", echo);
                    return rio::fut::write_all(IO, client, std::string_view{"GARBAGE\n"});
                })
                .then([&client, &IO]() { return rio::fut::read_till_eof(IO, client); })
                .then([](std::string rest) {
                    std::println("[client] server closed the connection after the garbage line ({} bytes after)", rest.size());
                    return rio::fut::ready();
                })
                .or_else([](std::error_code ec) {
                    std::println("[client] error: {}", ec.message());
                    return rio::fut::ready();
                }));

        rt.start(); // runs until both tasks die
    }

    auto close_res = rt.block_on(rio::fut::close(IO, std::move(listener)));
    if (close_res) {
        std::println("listener closed via IORING_OP_CLOSE");
    } else {
        std::println("listener close failed: {}", close_res.error().message());
    }

    std::println("all done");
    return 0;
}
