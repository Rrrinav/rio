#include <unordered_set>
#include <print>

struct Server;

struct Client
{
    Server* server;
    rio::client::sock sock;
    rio::buffer buf;
};

struct Server
{
    rio::TCP_Socket listener;
    std::unordered_set<Client*> clients;
};

static void client_kill(rio::context& IO, Client* c)
{
    if (!c) return;

    rio::as::kill(IO, c->sock);
    c->server->clients.erase(c);

    // Will prevent double delete by tracking what is pushed already
    rio::as::defer_delete(IO, c);
}

void on_write(rio::context& IO, Client* c, rio::error_code ec, std::size_t)
{
    if (ec) return client_kill(IO, c);

    rio::as::read(IO, c->sock, c->buf, on_read, c);
}

void on_read(rio::context& IO, Client* c, rio::error_code ec, std::size_t n)
{
    if (ec || n == 0)
    {
        std::println("Client {} disconnected", c->sock.ip());
        return client_kill(IO, c);
    }

    rio::as::write(IO, c->sock, c->buf, n, on_write, c);
}

void on_accept(rio::context& IO, Server* server, rio::error_code ec, rio::client::sock s)
{
    rio::as::accept(IO, server->listener, on_accept, server);
    if (ec) return;

    auto* c = new Client{ .server = server, .sock = std::move(s) };
    server->clients.insert(c);

    std::println("Got connection: {}", c->sock.ip());
    rio::as::read(IO, c->sock, c->buf, on_read, c);
}

auto main() -> int
{
    rio::context IO;
    Server server;

    auto res = rio::open(8000, rio::F::close_after_use | rio::F::non_blocking);
    if (!res)
    {
        std::println("Error: {}", res.error());
        return 1;
    }

    server.listener = std::move(*res);

    rio::as::accept(IO, server.listener, on_accept, &server);

    while (true) IO.poll();
}

