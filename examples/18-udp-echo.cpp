import std;
import rio;

// Async UDP echo over loopback using the fut::recv_from / fut::send_to ops.
// The server is bound (the only state that can receive); the client binds
// an ephemeral port for the same reason, then sends to the server.
int main()
{
    rio::context IO{128};
    rio::Runtime rt{IO};

    auto srv_res = rio::Udp_socket<rio::udp_state::unbound>::open_and_bind("127.0.0.1", 0);
    if (!srv_res) {
        std::println(std::cerr, "server bind failed: {}", srv_res.error());
        return 1;
    }
    auto [server, ignored] = std::move(*srv_res);
    (void)ignored;

    auto bound_res = server.local_endpoint();
    if (!bound_res) {
        std::println(std::cerr, "local_endpoint failed: {}", bound_res.error());
        return 1;
    }
    rio::address server_addr = std::move(*bound_res);
    std::println("UDP echo server on {}", server_addr.to_string());

    auto cli_res = rio::Udp_socket<rio::udp_state::unbound>::open_and_bind("127.0.0.1", 0);
    if (!cli_res) {
        std::println(std::cerr, "client bind failed: {}", cli_res.error());
        return 1;
    }
    auto [client, ignored2] = std::move(*cli_res);
    (void)ignored2;

    std::array<char, 1024> server_buf{};
    std::array<char, 1024> client_buf{};
    std::string reply;
    bool ok = false;

    rt.spawn(
        rio::fut::recv_from(IO, server, std::span{server_buf})
            .then([&](rio::Udp_recv_result r) {
                std::println("server got {} bytes from {}", r.len, r.addr.to_string());
                std::span<const char> echo{server_buf.data(), r.len};
                return rio::fut::send_to(IO, server, echo, r.addr);
            })
            .then([](std::size_t n) {
                std::println("server echoed {} bytes", n);
                return rio::fut::ready();
            })
            .or_else([](std::error_code ec) {
                std::println(std::cerr, "server error: {}", ec.message());
                return rio::fut::ready();
            }));

    rt.spawn(
        rio::fut::send_to(
            IO, client, std::span<const char>{"hello udp", sizeof("hello udp") - 1}, server_addr)
            .then([&](std::size_t) { return rio::fut::recv_from(IO, client, std::span{client_buf}); })
            .then([&](rio::Udp_recv_result r) {
                reply.assign(client_buf.data(), r.len);
                ok = (reply == "hello udp");
                return rio::fut::ready();
            })
            .or_else([](std::error_code ec) {
                std::println(std::cerr, "client error: {}", ec.message());
                return rio::fut::ready();
            }));

    rt.start();

    if (ok) {
        std::println("UDP ECHO OK");
        return 0;
    }
    std::println("UDP ECHO MISMATCH");
    return 1;
}
