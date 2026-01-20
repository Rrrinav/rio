#include <cstddef>
#include <print>
#include <unordered_set>

struct Client
{
    rio::client::sock sock;
    rio::buffer buf;
};

struct Server
{
    rio::TCP_Socket listener;
    std::unordered_set<Client *> clients;
};

static void client_kill(Server& server, Client *c)
{
    server.clients.erase(c);
    rio::as::kill(IO, c);
    rio::as::defered_delete(IO, c);
}

rio::task<void> client_loop(rio::context &IO, Client *c)
{
    while (true)
    {
        // if your API returns result<size_t>, use that instead
        auto res = co_await rio::co::read(IO, c->sock, c->buf);

        if (!res)
        {
            std::println("Client {} disconnected", c->sock.ip());
            std::println("[ERR]: ec: {}: {}", res.error().ec, res.error());
            client_kill(c);
            co_return;
        }

        std::size_t n = *res;
        std::size_t written = co_await rio::co::write(IO, c->sock, c->buf, n, ec);

        if (ec || written == 0)
        {
            std::println("Client {} write failed/disconnected", c->sock.ip());
            client_kill(c);
            co_return;
        }
    }
}

rio::task<void> accept_loop(rio::context &IO, Server& server)
{
    while (true)
    {
        rio::error_code ec;
        rio::client::sock s = co_await rio::co::accept(IO, server.listener, ec);

        if (ec)
        {
            std::println("Accept error: {}", ec.message());
            continue;
        }

        auto *c = new Client{.sock = std::move(s)};
        server.clients.insert(c);

        std::println("Got connection: {}", c->sock.ip());

        // start the client task (don't block accept loop)
        rio::co::spawn(IO, client_loop(IO, c));
    }
}

auto main() -> int
{
    Server server;
    rio::context IO(rio::context::type::Epoll);
    // rio::context IO(rio::context::type::Uring);

    auto res = rio::open(8000, rio::F::close_after_use | rio::F::non_blocking);
    if (!res)
    {
        std::println("Error: {}", res.error());
        return 1;
    }

    server.listener = std::move(*res);

    rio::co::spawn(IO, accept_loop(IO, server));

    while (true) IO.poll();
}
