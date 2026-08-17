import std;
import rio;

namespace {

using Clock = std::chrono::steady_clock;

struct Benchmark_config
{
    std::size_t clients = 32;
    std::size_t messages = 2000;
    std::size_t warmup_messages = 200;
    std::size_t message_size = 256;
    std::size_t runtime_entries = 2048;
    int backlog = 256;
};

struct Benchmark_session
{
    rio::context *ctx;
    rio::Tcp_socket sock;
    std::vector<char> buffer;
    std::size_t remaining;
};

struct Benchmark_result
{
    Benchmark_config cfg;
    std::chrono::nanoseconds wall_time{};
    std::chrono::nanoseconds cumulative_client_time{};
    std::size_t total_round_trips = 0;
    std::size_t errors = 0;
};

auto parse_size(std::string_view text, std::size_t &out) -> bool
{
    const char *first = text.data();
    const char *last = first + text.size();
    auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}

auto parse_int(std::string_view text, int &out) -> bool
{
    const char *first = text.data();
    const char *last = first + text.size();
    auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}

auto parse_args(int argc, char **argv) -> std::optional<Benchmark_config>
{
    Benchmark_config cfg;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        auto next = [&]() -> std::string_view {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::format("Missing value for {}", arg));
            }
            return argv[++i];
        };

        if (arg == "--clients") {
            if (!parse_size(next(), cfg.clients)) {
                throw std::runtime_error("Invalid --clients value");
            }
        } else if (arg == "--messages") {
            if (!parse_size(next(), cfg.messages)) {
                throw std::runtime_error("Invalid --messages value");
            }
        } else if (arg == "--warmup") {
            if (!parse_size(next(), cfg.warmup_messages)) {
                throw std::runtime_error("Invalid --warmup value");
            }
        } else if (arg == "--size") {
            if (!parse_size(next(), cfg.message_size)) {
                throw std::runtime_error("Invalid --size value");
            }
        } else if (arg == "--entries") {
            if (!parse_size(next(), cfg.runtime_entries)) {
                throw std::runtime_error("Invalid --entries value");
            }
        } else if (arg == "--backlog") {
            if (!parse_int(next(), cfg.backlog)) {
                throw std::runtime_error("Invalid --backlog value");
            }
        } else if (arg == "--help" || arg == "-h") {
            std::println("Usage: 14-runtime-benchmark [--clients N] [--messages N] [--warmup N] [--size BYTES] [--entries N] [--backlog N]");
            return std::nullopt;
        } else {
            throw std::runtime_error(std::format("Unknown argument: {}", arg));
        }
    }

    if (cfg.clients == 0 || cfg.messages == 0 || cfg.message_size == 0 || cfg.runtime_entries == 0 || cfg.backlog <= 0) {
        throw std::runtime_error("All numeric options must be greater than zero");
    }

    return cfg;
}

auto handle_benchmark_client(rio::context &ctx, rio::Tcp_socket sock, std::size_t total_messages, std::size_t message_size) -> rio::fut::Task<void>
{
    auto session = std::make_shared<Benchmark_session>(Benchmark_session{
        .ctx = &ctx,
        .sock = std::move(sock),
        .buffer = std::vector<char>(message_size),
        .remaining = total_messages,
    });

    return rio::fut::loop(
               session,
               [](std::shared_ptr<Benchmark_session> sess) {
                   return rio::fut::read_till_full(*sess->ctx, sess->sock, std::span<char>(sess->buffer.data(), sess->buffer.size()))
                       .then([sess]() {
                           return rio::fut::write_all(*sess->ctx, sess->sock, std::span<const char>(sess->buffer.data(), sess->buffer.size()));
                       })
                       .then([sess]() -> rio::fut::ready_t<std::shared_ptr<Benchmark_session>> {
                           if (--sess->remaining == 0) {
                               return rio::fut::error<std::shared_ptr<Benchmark_session>>(std::make_error_code(std::errc::broken_pipe));
                           }
                           return rio::fut::ready(sess);
                       });
               })
        .or_else([](std::error_code ec) {
            if (ec == std::errc::broken_pipe || ec == std::errc::connection_aborted) {
                return rio::fut::ready();
            }
            return rio::fut::error<void>(ec);
        });
}

void record_error(std::mutex &mu, std::vector<std::string> &errors, std::string msg)
{
    std::lock_guard lock(mu);
    if (errors.size() < 8) {
        errors.push_back(std::move(msg));
    }
}

void print_report(const Benchmark_result &result)
{
    const double wall_seconds = std::chrono::duration<double>(result.wall_time).count();
    const double avg_latency_us = result.total_round_trips == 0
        ? 0.0
        : static_cast<double>(result.cumulative_client_time.count()) / static_cast<double>(result.total_round_trips) / 1000.0;
    const double round_trips_per_sec = wall_seconds == 0.0 ? 0.0 : static_cast<double>(result.total_round_trips) / wall_seconds;
    const double wire_bytes = static_cast<double>(result.total_round_trips) * static_cast<double>(result.cfg.message_size) * 2.0;
    const double mib_per_sec = wall_seconds == 0.0 ? 0.0 : wire_bytes / wall_seconds / (1024.0 * 1024.0);

    std::println("runtime benchmark");
    std::println("  clients             : {}", result.cfg.clients);
    std::println("  warmup/client       : {}", result.cfg.warmup_messages);
    std::println("  measured/client     : {}", result.cfg.messages);
    std::println("  payload bytes       : {}", result.cfg.message_size);
    std::println("  io_uring entries    : {}", result.cfg.runtime_entries);
    std::println("  total round-trips   : {}", result.total_round_trips);
    std::println("  wall time           : {:.3f} ms", wall_seconds * 1000.0);
    std::println("  throughput          : {:.0f} round-trips/s", round_trips_per_sec);
    std::println("  wire throughput     : {:.2f} MiB/s", mib_per_sec);
    std::println("  avg observed rtt    : {:.2f} us", avg_latency_us);
    std::println("  client errors       : {}", result.errors);
}

} // namespace

int main(int argc, char **argv)
{
    Benchmark_config cfg;

    try {
        auto parsed = parse_args(argc, argv);
        if (!parsed) {
            return 0;
        }
        cfg = *parsed;
    } catch (const std::exception &e) {
        std::println(std::cerr, "argument error: {}", e.what());
        return 1;
    }

    const std::size_t total_messages_per_client = cfg.warmup_messages + cfg.messages;

    auto listen_res = rio::Tcp_socket::open_and_listen("127.0.0.1", 0, rio::s_opt::async_server_v4, cfg.backlog);
    if (!listen_res) {
        std::println(std::cerr, "listen error: {}", listen_res.error().message());
        return 1;
    }

    auto [listener, _bind_addr] = std::move(*listen_res);
    auto endpoint_res = listener.local_endpoint();
    if (!endpoint_res) {
        std::println(std::cerr, "endpoint error: {}", endpoint_res.error().message());
        return 1;
    }
    rio::address server_addr = *endpoint_res;

    std::mutex error_mu;
    std::vector<std::string> errors;
    std::atomic<std::size_t> connected_clients = 0;
    std::mutex start_mu;
    std::condition_variable start_cv;
    bool start_flag = false;

    std::jthread server_thread([&](std::stop_token) {
        try {
            rio::context io(static_cast<unsigned>(cfg.runtime_entries));
            rio::Runtime rt(&io);

            rt.spawn(rio::fut::accept_many(io, listener, cfg.clients, [&](rio::Tcp_socket client, rio::address) {
                         rt.spawn(handle_benchmark_client(rt.ctx(), std::move(client), total_messages_per_client, cfg.message_size));
                     }).or_else([&](std::error_code ec) {
                record_error(error_mu, errors, std::format("accept loop: {}", ec.message()));
                return rio::fut::ready();
            }));

            rt.start();
        } catch (const std::exception &e) {
            record_error(error_mu, errors, std::format("server thread: {}", e.what()));
        }
    });

    std::vector<std::jthread> clients;
    clients.reserve(cfg.clients);
    std::vector<std::chrono::nanoseconds> client_times(cfg.clients);
    std::atomic<std::size_t> error_count = 0;

    for (std::size_t client_id = 0; client_id < cfg.clients; ++client_id) {
        clients.emplace_back([&, client_id](std::stop_token) {
            auto sock_res = rio::Tcp_socket::open(rio::s_opt::client_v4);
            if (!sock_res) {
                record_error(error_mu, errors, std::format("client {} open: {}", client_id, sock_res.error().message()));
                error_count.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            auto sock = std::move(*sock_res);
            if (auto res = rio::io::connect(sock, server_addr); !res) {
                record_error(error_mu, errors, std::format("client {} connect: {}", client_id, res.error().message()));
                error_count.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            std::vector<char> payload(cfg.message_size);
            std::vector<char> echo(cfg.message_size);
            for (std::size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>('a' + ((client_id + i) % 26));
            }

            connected_clients.fetch_add(1, std::memory_order_relaxed);
            start_cv.notify_one();

            {
                std::unique_lock lock(start_mu);
                start_cv.wait(lock, [&] { return start_flag; });
            }

            for (std::size_t i = 0; i < cfg.warmup_messages; ++i) {
                if (auto res = rio::io::write_all(sock, std::span<const char>(payload.data(), payload.size())); !res) {
                    record_error(error_mu, errors, std::format("client {} warmup write: {}", client_id, res.error().message()));
                    error_count.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (auto res = rio::io::read_till_full(sock, std::span<char>(echo.data(), echo.size())); !res) {
                    record_error(error_mu, errors, std::format("client {} warmup read: {}", client_id, res.error().message()));
                    error_count.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }

            const auto start = Clock::now();

            for (std::size_t i = 0; i < cfg.messages; ++i) {
                if (auto res = rio::io::write_all(sock, std::span<const char>(payload.data(), payload.size())); !res) {
                    record_error(error_mu, errors, std::format("client {} write: {}", client_id, res.error().message()));
                    error_count.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (auto res = rio::io::read_till_full(sock, std::span<char>(echo.data(), echo.size())); !res) {
                    record_error(error_mu, errors, std::format("client {} read: {}", client_id, res.error().message()));
                    error_count.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (!std::ranges::equal(payload, echo)) {
                    record_error(error_mu, errors, std::format("client {} payload mismatch", client_id));
                    error_count.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }

            client_times[client_id] = Clock::now() - start;
        });
    }

    {
        std::unique_lock lock(start_mu);
        const bool all_connected = start_cv.wait_for(lock, std::chrono::seconds(5), [&] {
            return connected_clients.load(std::memory_order_relaxed) == cfg.clients;
        });
        if (!all_connected) {
            std::println(std::cerr, "benchmark startup timed out waiting for {} clients to connect", cfg.clients);
            return 1;
        }
    }

    const auto wall_start = Clock::now();
    {
        std::lock_guard lock(start_mu);
        start_flag = true;
    }
    start_cv.notify_all();

    clients.clear();
    const auto wall_time = Clock::now() - wall_start;
    server_thread.join();

    Benchmark_result result{
        .cfg = cfg,
        .wall_time = wall_time,
        .cumulative_client_time = std::accumulate(client_times.begin(), client_times.end(), std::chrono::nanoseconds{}),
        .total_round_trips = cfg.clients * cfg.messages,
        .errors = error_count.load(std::memory_order_relaxed),
    };

    print_report(result);

    if (!errors.empty()) {
        std::println("\nfirst errors:");
        for (const auto &msg : errors) {
            std::println("  {}", msg);
        }
        return 1;
    }

    return 0;
}
