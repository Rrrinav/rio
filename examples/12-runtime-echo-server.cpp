import std;
import rio;

struct Session
{
    rio::context &ctx;
    rio::Tcp_socket sock;
    rio::address addr;
    rio::Async_buffered_reader<rio::Tcp_socket, 4096> reader;
    std::string write_buf;

    Session(rio::context &c, rio::Tcp_socket &&s, rio::address a) : ctx(c), sock(std::move(s)), addr(a), reader(ctx, sock)
    {}
};

rio::fut::Task<void> handle_client(rio::context &ctx, rio::Tcp_socket sock, rio::address addr)
{
    auto session = std::make_shared<Session>(ctx, std::move(sock), addr);

    return rio::fut::loop(session, [](std::shared_ptr<Session> sess) {
        return rio::fut::buff::read_till(sess->reader, '\n')
        .then([sess](std::optional<std::string> msg) {
            if (!msg) {
                std::println(" [RIO]: {} disconnected.", sess->addr);
                sess->write_buf.clear();
            } else {
                while (!msg->empty() && (msg->back() == '\r' || msg->back() == '\n')) {
                    msg->pop_back();
                }

                std::println(" [RIO]: {} sent: \"{}\"", sess->addr, rio::util::escape_string(*msg));

                sess->write_buf = std::move(*msg);
                sess->write_buf += "\r\n";
            }

            return rio::fut::write_all(sess->ctx, sess->sock, sess->write_buf);
        })
        .then([sess]() -> rio::fut::ready_t<std::shared_ptr<Session>> {
            if (sess->write_buf.empty()) {
                return rio::fut::error<std::shared_ptr<Session>>(std::make_error_code(std::errc::broken_pipe));
            }

            return rio::fut::ready(sess);
        });
    })
    .or_else([addr](std::error_code ec) {
        // Only log if it is an actual unexpected error, not our loop-exit signal
        if (ec != std::errc::broken_pipe && ec != std::errc::connection_aborted)
            std::println(" [RIO]: {} task terminated with error: {}", addr, ec.message());
        return rio::fut::ready();
    });
}

int main()
{
    rio::context IO{256};
    rio::Runtime rt{&IO};

    auto listen_res = rio::Tcp_socket::open_and_listen("0.0.0.0", 6969, rio::s_opt::async_server_v4);
    if (!listen_res) {
        std::println("Failed to open server: {}", listen_res.error().message());
        return 1;
    }

    auto [listener, server_addr] = std::move(*listen_res);
    std::println("Async Echo Server running on {}...", server_addr);

    // 1. Spawn the accept loop straight into the runtime!
    rt.spawn(
        rio::fut::accept_all(IO, listener, [&rt](rio::Tcp_socket client, rio::address addr) {
            std::println(" [RIO]: New Client Connected: {}", addr);
            rt.spawn(handle_client(rt.ctx(), std::move(client), addr));
        }).or_else([](std::error_code ec) {
            std::println("Server accept loop crashed: {}", ec.message());
            return rio::fut::ready();
        })
    );

    // 2. Start the engine. It will run forever until accept_all fails.
    rt.start();

    std::println("Server shut down.");
    return 0;
}
