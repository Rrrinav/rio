import std;
import std.compat;

import rio;

void accept_handler(rio::Tcp_socket sock, const rio::address& add)
{
    std::println(" [RIO]: Accepted connection from {}", add);
    rio::io::write_all(sock, "Welcome to rio\r\n");

    char msg_buf[1024];
    std::span<char> span(msg_buf);

    ::size_t n = rio::io::read(sock, span);

    if (n <= 0)
    {
        std::println(" [RIO]: Receive issue.");
        std::println(" [RIO]: Client disconnected.");
        return;
    }

    rio::io::write(sock, "You sent: ");
    rio::io::write(sock, std::span<char>{msg_buf, n});
    rio::io::write(sock, "\r\n");

    std::println(" [RIO]: Client disconnected.");
}

int main()
{
    rio::Tcp_socket sock{};
    rio::address addr;

    //auto sock_res = rio::Tcp_socket::open_and_listen("localhost", 6969, rio::s_opt::sync_server_v4);
    auto sock_res = rio::Tcp_socket::open_and_listen("localhost", 6969);

    if (!sock_res)
    {
        std::println(std::cerr, "Socket creation failed: {}", sock_res.error());
        return 1;
    }

    std::tie(sock, addr) = std::move(*sock_res);
    std::println(" [RIO]: listening to: {}", addr);

    while (true)
    {
        std::println(" [RIO]: Waiting for connection...");
        if (auto n = rio::accept(sock, accept_handler); !n)
        {
            std::println(" [RIO]: Accept failed: {}", n.error());
            return 1;
        }
    }

    return 0;
}
