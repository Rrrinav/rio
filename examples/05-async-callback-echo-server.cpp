import std;
import rio;

struct Session
{
    rio::Tcp_socket sock;
    rio::address addr;
    std::array<char, 4096> buffer;
};

struct Server
{
    rio::Tcp_socket listener;
};

// You can accept session* insetad of void*, this is made sure by templates
// You just have to send same thing to accepti function and receive same thing in callback.
void read_callback  (rio::context &ctx, rio::result<std::size_t> res, Session *s);
void write_callback (rio::context &ctx, rio::result<std::size_t> res, Session *s);
void accept_callback(rio::context &ctx, rio::result<rio::as::accept_result> res, Server *srv);

auto main() -> int
{
    rio::context ctx;

    auto res = rio::Tcp_socket::open_and_listen("0.0.0.0", 8000);
    if (!res)
    {
        std::println(" [RIO]: Fatal: {}", res.error().message());
        return 1;
    }

    auto [sock, addr] = std::move(*res);
    Server server{.listener = std::move(sock)};

    std::println(" [RIO]: Listening on 8000...");

    // To use async functions, use 'rio::as' namespace.
    rio::as::accept(ctx, server.listener, accept_callback, &server);

    while (true) ctx.poll();
}

void write_callback(rio::context &ctx, rio::result<std::size_t> res, Session *s)
{
    if (!res)
    {
        delete s;
        return;
    }

    rio::as::read(ctx, s->sock, s->buffer, read_callback, s);
}

void read_callback(rio::context &ctx, rio::result<std::size_t> res, Session *s)
{
    if (!res || *res == 0)
    {
        std::println(" [RIO]: Client disconnected [{}]", res ? "EOF" : res.error().message());
        // Calls delete at the end of the loop
        // Handles duplicates but that is an additional cost, you can handle deletion yourself too but remember
        // it may lead to use after free, if we have same operations in same batch.
        ctx.defer_delete(s);
        return;
    }

    std::size_t n = *res;
    std::string_view msg(s->buffer.data(), n);

    std::print(" [RIO]: {} sent: {}", s->addr, msg);

    if (!msg.ends_with('\n'))
        std::println();

    rio::as::write(ctx, s->sock, std::span(s->buffer.data(), n), write_callback, s);
}

void accept_callback(rio::context &ctx, rio::result<rio::as::accept_result> res, Server *srv)
{
    rio::as::accept(ctx, srv->listener, accept_callback, srv);

    if (!res)
    {
        std::print(" [RIO]: Accept failed: {}", res.error().message());
        return;
    }

    std::println(" [RIO]: New Connection: {}", res->address.to_string());
    auto *s = new Session{.sock = std::move(res->client), .addr = res->address, .buffer{}};

    // Since we are sending Session* here, we must accept same type there.
    // They have to be poitners. This applies to all callbacks.
    rio::as::read(ctx, s->sock, s->buffer, read_callback, s);
}

