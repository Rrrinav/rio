import std;
import rio;

namespace {

using Clock = std::chrono::steady_clock;

struct Timer_benchmark_config
{
    std::size_t tasks = 256;
    std::size_t iterations = 4000;
    std::size_t warmup_iterations = 500;
    std::size_t runtime_entries = 4096;
};

struct Timer_task_state
{
    rio::context *ctx;
    std::size_t remaining;
};

auto parse_size(std::string_view text, std::size_t &out) -> bool
{
    const char *first = text.data();
    const char *last = first + text.size();
    auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}

auto parse_args(int argc, char **argv) -> std::optional<Timer_benchmark_config>
{
    Timer_benchmark_config cfg;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        auto next = [&]() -> std::string_view {
            if (i + 1 >= argc)
                throw std::runtime_error(std::format("Missing value for {}", arg));
            return argv[++i];
        };

        if (arg == "--tasks") {
            if (!parse_size(next(), cfg.tasks))
                throw std::runtime_error("Invalid --tasks value");
        } else if (arg == "--iterations") {
            if (!parse_size(next(), cfg.iterations))
                throw std::runtime_error("Invalid --iterations value");
        } else if (arg == "--warmup") {
            if (!parse_size(next(), cfg.warmup_iterations))
                throw std::runtime_error("Invalid --warmup value");
        } else if (arg == "--entries") {
            if (!parse_size(next(), cfg.runtime_entries))
                throw std::runtime_error("Invalid --entries value");
        } else if (arg == "--help" || arg == "-h") {
            std::println("Usage: 15-runtime-timer-benchmark [--tasks N] [--iterations N] [--warmup N] [--entries N]");
            return std::nullopt;
        } else {
            throw std::runtime_error(std::format("Unknown argument: {}", arg));
        }
    }

    if (cfg.tasks == 0 || cfg.iterations == 0 || cfg.runtime_entries == 0)
        throw std::runtime_error("All numeric options must be greater than zero");

    return cfg;
}

auto make_timer_task(rio::context &ctx, std::size_t iterations) -> rio::fut::Task<void>
{
    auto state = std::make_shared<Timer_task_state>(Timer_task_state{.ctx = &ctx, .remaining = iterations});

    return rio::fut::loop(state, [](std::shared_ptr<Timer_task_state> task) {
        return rio::fut::wake_up_after(*task->ctx, std::chrono::nanoseconds(1))
            .then([task]() -> rio::fut::ready_t<std::shared_ptr<Timer_task_state>> {
                if (--task->remaining == 0)
                    return rio::fut::error<std::shared_ptr<Timer_task_state>>(std::make_error_code(std::errc::broken_pipe));
                return rio::fut::ready(task);
            });
    }).or_else([](std::error_code ec) {
        if (ec == std::errc::broken_pipe)
            return rio::fut::ready();
        return rio::fut::error<void>(ec);
    });
}

auto run_phase(const Timer_benchmark_config &cfg, std::size_t iterations) -> std::chrono::nanoseconds
{
    rio::context io(static_cast<unsigned>(cfg.runtime_entries));
    rio::Runtime rt(&io);

    for (std::size_t i = 0; i < cfg.tasks; ++i)
        rt.spawn(make_timer_task(io, iterations));

    const auto start = Clock::now();
    rt.start();
    return Clock::now() - start;
}

} // namespace

int main(int argc, char **argv)
{
    Timer_benchmark_config cfg;

    try {
        auto parsed = parse_args(argc, argv);
        if (!parsed)
            return 0;
        cfg = *parsed;
    } catch (const std::exception &e) {
        std::println(std::cerr, "argument error: {}", e.what());
        return 1;
    }

    try {
        if (cfg.warmup_iterations > 0)
            run_phase(cfg, cfg.warmup_iterations);

        const auto measured = run_phase(cfg, cfg.iterations);
        const std::size_t total_ops = cfg.tasks * cfg.iterations;
        const double wall_seconds = std::chrono::duration<double>(measured).count();
        const double ops_per_sec = wall_seconds == 0.0 ? 0.0 : static_cast<double>(total_ops) / wall_seconds;
        const double avg_us = total_ops == 0 ? 0.0 : static_cast<double>(measured.count()) / static_cast<double>(total_ops) / 1000.0;

        std::println("runtime timer benchmark");
        std::println("  tasks              : {}", cfg.tasks);
        std::println("  warmup/task        : {}", cfg.warmup_iterations);
        std::println("  measured/task      : {}", cfg.iterations);
        std::println("  io_uring entries   : {}", cfg.runtime_entries);
        std::println("  total timer ops    : {}", total_ops);
        std::println("  wall time          : {:.3f} ms", wall_seconds * 1000.0);
        std::println("  throughput         : {:.0f} ops/s", ops_per_sec);
        std::println("  avg op time        : {:.2f} us", avg_us);
    } catch (const std::exception &e) {
        std::println(std::cerr, "benchmark error: {}", e.what());
        return 1;
    }

    return 0;
}
