import std;
import rio;

constexpr int timeout_seconds = 7;

struct ClientContext
{
    rio::Tcp_socket sock;
    rio::address addr{};
    std::array<char, 1024> buf{};
    std::size_t n = 0;
};

auto make_echo_client(ClientContext ctx)
{
    return rio::fut::loop(
        std::move(ctx),
        [](ClientContext& ctx) {
            return rio::fut::make(
                std::move(ctx),
                [](ClientContext& c) -> rio::fut::res<ClientContext> {
                    using rio::fut::res;
                    auto r = rio::try_read(c.sock, c.buf);

                    if (!r)
                    {
                        if (r.error().code == std::errc::operation_would_block)
                            return res<ClientContext>::pending();
                        return res<ClientContext>::error(r.error().code);
                    }

                    if (*r == 0)
                        return res<ClientContext>::error(std::errc::connection_aborted);

                    c.n = *r;
                    std::string_view recvd_data(c.buf.data(), c.n);
                    if (recvd_data.ends_with("\n"))
                        std::print("{} sent: {}", c.addr, recvd_data);
                    else
                        std::println("{} sent: {}", c.addr, recvd_data);

                    return res<ClientContext>::ready(std::move(c));
                }
            ).timeout_with(
                std::chrono::seconds(timeout_seconds),
                [] (ClientContext c) { // Becuase this is transfer of ownership state is moved, either accept by value or as r value: ClientContext&&
                    std::println("Client {} timed out. Sending goodbye...", c.addr);

                    return rio::fut::make(
                        std::move(c),
                        [](ClientContext& c) -> rio::fut::res<ClientContext> {
                            std::string_view msg = "Timeout: You were too slow! Bye!\n";
                            auto r = rio::try_write(c.sock, std::span(msg));

                            if (!r && r.error().code == std::errc::operation_would_block)
                                return rio::fut::res<ClientContext>::pending();

                            return rio::fut::res<ClientContext>::error(std::errc::timed_out);
                        }
                    );
                }
            ).then([](ClientContext&& c) -> auto { // Becuase this is transfer of ownership state is moved, either accept by value or as r value: ClientContext&&
                return rio::fut::make(std::move(c),
                    [](ClientContext &c) -> rio::fut::res<ClientContext> {
                        using rio::fut::res;
                        auto r = rio::try_write(c.sock, std::span(c.buf).subspan(0, c.n));

                        if (!r)
                        {
                            if (r.error().code == std::errc::operation_would_block)
                                return res<ClientContext>::pending();
                            return res<ClientContext>::error(r.error().code);
                        }
                        return res<ClientContext>::ready(std::move(c));
                    }
                );
            });
        }
    );
}

using ClientFuture = decltype(make_echo_client(ClientContext{}));

auto main() -> int
{
    auto open_res = rio::Tcp_socket::open_and_listen("0.0.0.0", 6969, rio::s_opt::async_server_v4);
    if (!open_res)
    {
        std::println("Error: {}", open_res.error());
        return 1;
    }

    auto [listener, addr] = std::move(open_res.value());
    std::println("Server listening on {}", addr);

    std::vector<ClientFuture> clients;

    // Functions that have try return immediately on any error, like here try_accept will retutn on would block.
    // Same with try_read and try_write.
    auto acceptor = rio::Future {
        std::move(listener),
        [](rio::Tcp_socket &l) -> rio::fut::res<ClientContext> {
            rio::address addr{};
            auto r = rio::try_accept(l, addr);

            if (!r)
            {
                if (r.error().code == std::errc::operation_would_block)
                    return rio::fut::res<ClientContext>::pending();
                return rio::fut::res<ClientContext>::error(r.error().code);
            }

            std::println("New Client Connected: {}", addr);
            return rio::fut::res<ClientContext>::ready({.sock = std::move(r.value()), .addr = addr});
        }
    };

    while (true)
    {
        auto r = rio::poll(acceptor);
        if (r.state == rio::fut::status::ready)
            clients.push_back(make_echo_client(std::move(*r.value)));

        for (int i = clients.size() - 1; i >= 0; --i)
        {
            // If we weren't using fut::loop, we would have to handle looping manually like:
            //
            // if (session_res.state == rio::fut::status::ready)
            //     clients[i] = make_echo_client();
            auto session_res = rio::poll(clients[i]);

            if (session_res.state == rio::fut::status::error)
            {
                if (session_res.err == std::errc::connection_aborted)
                    std::println("Client Disconnected: {}", clients[i].state.state.addr);
                else
                    std::println("Error: {}", session_res.err.message());
                clients[i] = std::move(clients.back());
                clients.pop_back();
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    return 0;
}
