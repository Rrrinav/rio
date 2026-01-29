import std;
import rio;

auto main() -> int
{
    // opened socket in sync mode: sync read will block and wont silently return 0, for WOULD_BLOCK
    auto sock_res = rio::Tcp_socket::open_and_listen("0.0.0.0", 6969, rio::s_opt::sync_server_v4);

    if (!sock_res)
    {
        std::println("{}", sock_res.error());
        return 1;
    }

    auto [ server_sock, server_addr ] = std::move(*sock_res);

    std::println(" [RIO]: Listening on: {}", server_addr);

    char buf[1024];

    for (;;)
    {
        if (auto res = rio::accept(server_sock); res)
        {
            auto [ client_sock, client_addr ] = std::move(res.value());
            std::println(" [RIO]: Client: {} connected.", client_addr);
            auto n = rio::io::read(client_sock, std::span{buf});
            std::print(" [RIO]: Received: ");
            if (n == 0)
            {
                std::println("Client disconnected.");
                std::cout.flush();
                continue;
            }
            std::string_view sv(buf, n);

            if (!sv.ends_with("\n"))
                std::println(" {}", sv);
            else
                std::print(" {}", sv);

            if (auto sz = rio::io::write(client_sock, std::span{buf, n}); sz < 0)
            {
                std::println("{}", (std::make_error_code(static_cast<std::errc>(sz))).message());
                std::cout.flush();
                continue;
            }
            std::cout.flush();
            continue;
        }
    }
    return 0;
}
