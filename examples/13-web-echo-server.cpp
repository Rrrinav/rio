import std;
import rio;

using namespace rio::http::v1_1;

struct Http_session
{
    rio::context &ctx;
    rio::Tcp_socket sock;
    rio::address addr;
    rio::Async_buffered_reader<rio::Tcp_socket, 4096> reader;
    request req{};
    std::string response_buf;

    Http_session(rio::context &c, rio::Tcp_socket &&s, rio::address a) : ctx(c), sock(std::move(s)), addr(a), reader(ctx, sock)
    {}
};

rio::fut::Task<void> handle_http_client(rio::context &ctx, rio::Tcp_socket sock, rio::address addr, std::shared_ptr<Router> router)
{
    auto sess = std::make_shared<Http_session>(ctx, std::move(sock), addr);

    return parse_request(sess->reader, sess->req)
        .then([sess, router]() {
            std::println(
                "[{:%H:%M:%S}] {}:{} -> {} {} ({} bytes)",
                std::chrono::system_clock::now(),
                sess->addr.to_string(),
                sess->addr.port(),
                rio::http::v1_1::method_to_str(sess->req.method),
                sess->req.path,
                sess->req.body.size());

            response res = router->dispatch(sess->req);
            res.set_header("Server", "rio-runtime");
            res.set_header("Connection", "close");

            sess->response_buf = res.to_string();
            return rio::fut::write_all(sess->ctx, sess->sock, sess->response_buf);
        })
        .or_else([](std::error_code ec) {
            if (ec != std::errc::connection_aborted && ec != std::errc::broken_pipe) {
                std::println(" [!] Connection Error: {}", ec.message());
            }
            return rio::fut::ready();
        });
}

int main()
{
    auto html_res = rio::io::read_file("assets/echoser.html");
    if (!html_res) {
        std::println("Error: Could not load index.html: {}", html_res.error().message());
        return 1;
    }
    std::string index_html = std::move(*html_res);

    rio::context IO{512};
    rio::Runtime rt{&IO};
    auto router = std::make_shared<Router>();

    router->get("/", [index_html](const auto &) {
        auto res = response::text(index_html);
        res.set_header("Content-Type", "text/html");
        return res;
    });

    router->post("/api/echo", [](const auto &req) {
        std::string msg = "empty";
        try {
            auto json = rio::jsn::parse(req.body);
            msg = rio::jsn::view(json)["message"].template as_or<std::string>("err");
        } catch (...) {}

        rio::jsn::Context ctx;
        ctx.obj_b().obj_k("echo").inject(msg).obj_e();
        return response::json(ctx.get());
    });

    auto listen_res = rio::Tcp_socket::open_and_listen("0.0.0.0", 8080, rio::s_opt::async_server_v4);
    if (!listen_res)
        return 1;

    auto [listener, server_addr] = std::move(*listen_res);
    std::println("RIO persistent console live: http://localhost:8080");

    rt.spawn(rio::fut::accept_all(IO, listener, [&rt, router](rio::Tcp_socket client, rio::address addr) {
        rt.spawn(handle_http_client(rt.ctx(), std::move(client), addr, router));
    }));

    rt.start();
    return 0;
}
