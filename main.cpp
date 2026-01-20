#include <print>
#include <utility>
#include <string>

import rio;

int main()
{
    rio::context foo{1};
    auto res = rio::file::open("./build.sh", rio::f_mode::read_only);

    if (!res)
    {
        std::println(stderr, "{}", res.error());
        return 1;
    }

    auto f = std::move(*res);

    std::string buf;

    if (rio::io::read(f, buf))
        std::println("{}", buf);
    else
        std::println(stderr, "Error or EOF reached unexpectedly");

    auto sock_res = rio::Tcp_socket::open(rio::s_opt::sync_server_v4);
    if (!sock_res)
    {
        std::println(stderr, "Socket creation failed: {}", sock_res.error());
        return 1;
    }
    auto &server_sock = *sock_res;

    // 2. Bind to localhost on port 9900
    auto addr = rio::address::localhost_ipv4(9900);

    if (auto res = rio::bind(server_sock, addr); !res)
    {
        std::println(stderr, "Bind failed: {}", res.error());
        return 1;
    }

    // 3. Start listening (Crucial step!)
    if (auto res = rio::listen(server_sock, 128); !res)
    {
        std::println(stderr, "Listen failed: {}", res.error());
        return 1;
    }

    std::println("Server listening on {}", addr.to_string());

    while (true)
    {
        std::println("Waiting for connection...");

        rio::address peer;
        auto client_res = rio::accept_from(server_sock, peer);

        if (!client_res)
        {
            std::println(stderr, "Accept failed: {}", client_res.error());
            continue;
        }

        auto client_sock = std::move(*client_res);
        std::println("Accepted connection from {}", peer.to_string());

        char msg_buf[1024];
        std::span<char> span(msg_buf);

        size_t n = rio::io::read(reinterpret_cast<const rio::file &>(client_sock), span);

        if (n > 0)
            std::println("Received: {}", std::string_view(msg_buf, n));

        // Client socket closes automatically here via RAII
        std::println("Client disconnected.");
    }

    return 0;
}
