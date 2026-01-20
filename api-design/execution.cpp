#include <print>
#include <vector>
#include <memory>
#include "rio_execution.hpp"

// -----------------------------------------------------------------
// Data Structures
// -----------------------------------------------------------------
struct Server;

struct Client
{
    Server *server;
    rio::client::sock sock;
    rio::buffer buf;
};

struct Server
{
    rio::TCP_Socket listener;
    // Using Hive (Colony) for O(1) stable storage + cache locality
    std::hive<Client> clients;
};

// -----------------------------------------------------------------
// Business Logic: The Pipeline
// -----------------------------------------------------------------
void handle_client_pipeline(rio::context &IO, Client *c)
{
    namespace ex = stdexec;

    // 1. Define the Single Step (Read -> Write)
    // Returns 'true' to loop, 'false' to stop.
    auto ping_pong_step = [&IO, c]
    {
        return rio::snd::read(IO, c->sock, c->buf) | ex::let_value(
                                                         [&IO, c](size_t n)
                                                         {
                                                             // Logic: Disconnect on 0 bytes
                                                             if (n == 0)
                                                                 return ex::just(false);

                                                             // Logic: Write echo
                                                             return rio::snd::write(IO, c->sock, c->buf, n) |
                                                                    ex::then([](size_t) { return true; });
                                                         });
    };

    // 2. Define the Full Lifecycle
    // Loop until 'ping_pong_step' returns false (or errors).
    auto pipeline = ex::repeat_effect_until(ex::just() | ex::let_value(ping_pong_step)) |
                    ex::upon_error(
                        [&IO, c](std::exception_ptr)
                        {
                            // Catch generic exceptions (unlikely in no-throw code)
                            rio::defer_delete(IO, c);
                        }) |
                    ex::upon_error(
                        [&IO, c](rio::error_code ec)
                        {
                            // Catch IO Errors (Connection Reset, etc.)
                            std::println("Client Error: {}", ec.message());
                            rio::defer_delete(IO, c);
                        }) |
                    ex::then(
                        [&IO, c]()
                        {
                            // Catch Normal Exit (n == 0)
                            std::println("Client Disconnected");
                            rio::defer_delete(IO, c);
                        });

    // 3. Launch
    ex::start_detached(std::move(pipeline));
}

// -----------------------------------------------------------------
// Server Loop
// -----------------------------------------------------------------
void on_accept(rio::context &IO, Server *server, rio::error_code ec, rio::client::sock s)
{
    // Accept next connection immediately
    rio::as::accept(IO, server->listener, on_accept, server);
    if (ec)
        return;

    std::println("Got connection: {}", s.ip());

    // 1. Emplace into Hive (Stable memory location)
    auto it = server->clients.emplace_back(Client{.server = server, .sock = std::move(s)});

    // 2. Get stable pointer
    Client *c = &(*it);

    // 3. Start the Pipeline
    handle_client_pipeline(IO, c);
}

int main()
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

    // Start accepting
    rio::as::accept(IO, server.listener, on_accept, &server);

    std::println("Server running on port 8000 (C++26 Senders/Receivers)...");

    while (true)
    {
        IO.poll();

        // Cleanup Phase (The "Graveyard" processor)
        IO.cleanup_deferred_deletions(
            [&](void *ptr)
            {
                // Since we use Hive, we need to convert ptr back to iterator or
                // just implementation detail: in this specific example,
                // 'defer_delete' needs to know how to remove from Hive.
                // Simplest way: Pass a lambda to defer_delete.
            });
    }
}
