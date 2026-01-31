import std;
import rio;

struct Client
{
    rio::Tcp_socket sock;
    rio::address addr;
    std::vector<char> buf = std::vector<char>(1024);
};

// we have to manage allocate client to keep address always valid inside uring.
auto make_client(rio::context &ctx, rio::Tcp_socket s, rio::address a)
{
    auto ptr = std::make_unique<Client>(Client{std::move(s), a});
    return rio::fut::loop(std::move(ptr),
        [&ctx](std::unique_ptr<Client> &ptr) {
            Client *c = ptr.get();
            return rio::fut::read(ctx, c->sock, c->buf).then([c, &ctx](std::size_t n) {
                std::println("{} sent: {}", c->addr, std::span(c->buf).first(n));
                return rio::fut::write(ctx, c->sock, std::span(c->buf).first(n));
            }).then([](std::size_t n) {
                return rio::fut::make(
                    n,
                    [](std::size_t n) {
                        if (n == 0)
                            return rio::fut::res<void>::error(std::errc::connection_aborted);

                        return rio::fut::res<void>::ready();
                    }
                );
            });
        });
}

using ClientFuture = decltype(make_client(std::declval<rio::context &>(), std::declval<rio::Tcp_socket>(), std::declval<rio::address>()));

int main()
{
    rio::context IO;
    auto server_sk = rio::Tcp_socket::open_and_listen(rio::address::any_ipv4(6969), rio::s_opt::async_server_v4).value();
    std::println("Listening on 6969...");

    std::vector<ClientFuture> clients;

    auto server = rio::fut::loop(std::move(server_sk),
        [&](rio::Tcp_socket &listener)
        {
            return rio::fut::accept(IO, listener)
                .then(
                    [&](rio::fut::AcceptResult res)
                    {
                        std::println("New Client: {}", res.address.to_string());
                        clients.push_back(make_client(IO, std::move(res.client), res.address));
                        return rio::fut::ready(std::move(listener));
                    });
        });

    while (true)
    {
        server.poll();

        for (std::size_t i = clients.size(); i-- > 0;)
        {
            if (clients[i].poll().state == rio::fut::status::error)
            {
                if (i != clients.size() - 1)
                    clients[i] = std::move(clients.back());
                clients.pop_back();
            }
        }
        IO.poll();
    }
}
