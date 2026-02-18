import std;
import rio;

#define __TRY(var, expr)                                                                                               \
    auto __res_##var = (expr);                                                                                         \
    if (!__res_##var) {                                                                                                \
        std::println("Error: {}", __res_##var.error());                                                                \
        return 1;                                                                                                      \
    }                                                                                                                  \
    auto var = std::move(*__res_##var)

struct Session
{
    rio::context &ctx;
    rio::Tcp_socket sock;
    rio::address addr;
    rio::Async_buffered_reader<rio::Tcp_socket, 4096> reader;
    std::string write_buf;

    Session(rio::context &c, rio::Tcp_socket &&s, rio::address a)
        : ctx(c), sock(std::move(s)), addr(a), reader(ctx, sock)
    {}
};

auto handle_client(rio::context &ctx, rio::Tcp_socket sock, rio::address addr)
{
    auto session = std::make_shared<Session>(ctx, std::move(sock), addr);

    return rio::fut::loop(session, [](std::shared_ptr<Session> sess) {
        return rio::fut::buff::read_till(sess->reader, '0')
            .then([sess](std::optional<std::string> msg) {
                if (!msg) {
                    std::println(" [RIO]: {} disconnected", sess->addr);
                    sess->write_buf.clear();
                } else {
                    while (!msg->empty() && (msg->back() == '\n' || msg->back() == '\r'))
                        msg->pop_back();
                    std::println(" [RIO]: {} sent: {}", sess->addr, *msg);
                    msg->push_back('\n');
                    sess->write_buf = std::move(*msg);
                }
                return rio::fut::write_all(sess->ctx, sess->sock, sess->write_buf);
            })
            .then([sess]() -> rio::fut::ready_t<std::shared_ptr<Session>> {
                if (sess->write_buf.empty())
                    return rio::fut::error<std::shared_ptr<Session>>(std::make_error_code(std::errc::broken_pipe));
                return rio::fut::ready(sess);
            });
    });
}

using ClientFuture = decltype(handle_client(
    std::declval<rio::context &>(), std::declval<rio::Tcp_socket>(), std::declval<rio::address>()));

int main()
{
    rio::context IO;
    std::vector<ClientFuture> clients;

    __TRY(listen_res, rio::Tcp_socket::open_and_listen("0.0.0.0", 6969, rio::s_opt::async_server_v4));
    auto [server_sock, addr] = std::move(listen_res);

    std::println("Async Echo Server running on port 6969...");

    auto accept_loop = rio::fut::accept_all(IO, server_sock, [&](rio::Tcp_socket client, rio::address addr) {
        std::println(" [RIO]: New Client: {}", addr);
        clients.push_back(handle_client(IO, std::move(client), addr));
    });

    while (true) {
        rio::poll(accept_loop);

        for (std::size_t i = 0; i < clients.size();) {
            auto res = clients[i].poll();
            if (res.state != rio::fut::status::pending) {
                if (i != clients.size() - 1)
                    clients[i] = std::move(clients.back());
                clients.pop_back();
            } else {
                ++i;
            }
        }

        IO.poll();
    }

    return 0;
}
