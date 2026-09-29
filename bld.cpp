// bld.cpp — rio build script.
//
//   ./bld [-j N] [-build-all] [-build-std] [-std] [-run] [-build-examples]
//         [-compile src -o out] [-clean] [-clean-all] [-clean-example]
//
// Incremental by default: module PCMs/objs/libs/exe form a dependency
// graph (bld::Plan) so only stale tasks run.

#include <algorithm>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#define B_LDR_IMPLEMENTATION
#include "b_ldr.hpp"

namespace {

namespace stdfs = std::filesystem;

struct Config {
    const stdfs::path dir_src = "src/";
    const stdfs::path dir_bin = "bin/";
    const stdfs::path dir_pcm = "bin/pcms/";
    const stdfs::path dir_obj = "bin/objs/";
    const stdfs::path dir_std = "bin/std/";
    const stdfs::path dir_libs = "bin/libs/";
    const stdfs::path dir_example_bin = "bin-example/";

    const std::string exe_name = "rio";
    const std::string lib_static = "librio.a";
    const std::string lib_shared = "librio.so";
    const std::string main_src = "main.cpp";

    const std::string compiler = "clang++";
    const std::string archiver = "ar";

    // Enforce libc++ globally.
    const std::vector<std::string> flags_common = {"-std=c++23", "-Wall", "-Wextra", "-O2", "-fPIC", "-g"};
    const std::vector<std::string> flags_linker = {"-stdlib=libc++", "-luring", "-lc++abi"};

    [[nodiscard]] auto mod_paths() const -> std::vector<std::string>
    {
        return {"-fprebuilt-module-path=" + dir_pcm.string(), "-fprebuilt-module-path=" + dir_std.string()};
    }
};

struct Module {
    bld::fs::Cpp_module inner;
    [[nodiscard]] auto safe_name() const -> std::string
    {
        std::string s = inner.name;
        std::replace(s.begin(), s.end(), ':', '-');
        return s;
    }
    [[nodiscard]] auto pcm(const Config &cfg) const -> stdfs::path
    {
        return cfg.dir_pcm / (safe_name() + ".pcm");
    }
    [[nodiscard]] auto obj(const Config &cfg) const -> stdfs::path
    {
        return cfg.dir_obj / (safe_name() + ".o");
    }
};

// Option keys are dash-insensitive in this library version ("-j", "--j"
// and "j" are one key), so argv passes straight through to parse().
auto parse_cli(int argc, char *argv[]) -> std::expected<bld::Config::Parse_outcome, bld::Err>
{
    auto &cfg = bld::Config::get();
    cfg.add_option("clean", bld::Config::Bool, "Remove libs/objs/pcms", false)
        .add_option("clean-all", bld::Config::Bool, "Remove bin dir", false)
        .add_option("clean-example", bld::Config::Bool, "Remove example binaries", false)
        .add_option("std", bld::Config::Bool, "Rebuild std module only", false)
        .add_option("build-std", bld::Config::Bool, "Force std module rebuild", false)
        .add_option("build-all", bld::Config::Bool, "Force full rebuild", false)
        .add_option("run", bld::Config::Bool, "Run executable after build", false)
        .add_option("build-examples", bld::Config::Bool, "Build examples", false)
        .add_option("j", bld::Config::Int, "Parallel jobs (default: auto)", 0)
        .add_option("compile", bld::Config::String, "Compile single file", std::string(""))
        .add_option("o", bld::Config::String, "Output file for compile", std::string(""));
    return cfg.parse(argc, argv);
}

[[nodiscard]] auto flag(std::string_view key) -> bool
{
    return static_cast<bool>(bld::Config::get()[key]);
}

[[nodiscard]] auto opt_str(std::string_view key) -> std::string
{
    const auto &cfg = bld::Config::get();
    if (auto v = cfg.get_val<std::string>(key); v) {
        return *v;
    }
    if (auto v = cfg.get_val<int>(key); v) {
        return std::to_string(*v);
    }
    if (auto v = cfg.get_val<double>(key); v) {
        return std::to_string(*v);
    }
    return "";
}

// Positive -j N, otherwise nullopt (library default: one core free).
[[nodiscard]] auto opt_jobs() -> std::optional<std::size_t>
{
    if (auto v = bld::Config::get().get_val<int>("j"); v && *v > 0) {
        return static_cast<std::size_t>(*v);
    }
    return std::nullopt;
}

auto run_ok(const bld::Cmd &cmd) -> std::expected<void, bld::Err>
{
    auto proc = bld::run(cmd);
    if (!proc) {
        return std::unexpected(proc.error());
    }
    if (proc->status_code() != 0) {
        return std::unexpected(
            bld::Err::erc(std::errc::operation_canceled, std::format("command {:?} exited with code {}", cmd, proc->status_code())));
    }
    return {};
}

// Base compile command: compiler + common flags.
[[nodiscard]] auto cxx(const Config &cfg) -> bld::Cmd
{
    bld::Cmd c{cfg.compiler};
    for (const auto &f : cfg.flags_common) {
        c.push(f);
    }
    return c;
}

constexpr std::string_view CACHE_FILE = ".bld_std_path";
constexpr std::string_view STD_PCM = "std.pcm";

[[nodiscard]] auto search_root(const stdfs::path &root) -> std::optional<stdfs::path>
{
    if (!bld::fs::exists(root.string())) {
        return std::nullopt;
    }
    std::optional<stdfs::path> hit;
    bld::fs::Controller ctl;
    for (const auto &e : bld::fs::walk_dir(root.string(), ctl)) {
        if (e.is_dir()) {
            // Prune branches that cannot host libc++ sources.
            const std::string p = e.path.string();
            if (p.find("include") == std::string::npos && p.find("c++") == std::string::npos && p.find("v1") == std::string::npos) {
                ctl.dont_recurse = true;
            }
            continue;
        }
        if (e.filename() == "std.cppm") {
            hit = e.path;
            break;
        }
    }
    if (ctl.failed()) {
        bld::log::w("std.cppm search under '{}' failed: {}", root.string(), ctl.error().message());
    }
    return hit;
}

[[nodiscard]] auto find_std_cppm() -> std::optional<stdfs::path>
{
    if (auto cached = bld::fs::read_file(CACHE_FILE); cached && bld::fs::exists(*cached)) {
        return stdfs::path{*cached};
    }
    for (const char *root :
         {"/usr/lib/llvm-19/share/libc++/v1", "/usr/lib/llvm-18/share/libc++/v1", "/usr/share/libc++/v1", "/usr/local/share", "/opt/homebrew"}) {
        if (auto hit = search_root(stdfs::path{root})) {
            std::ignore = bld::fs::write_file(CACHE_FILE, hit->string()); // cache for next time
            return hit;
        }
    }
    return std::nullopt;
}

auto ensure_std(const Config &cfg, bool force) -> std::expected<void, bld::Err>
{
    const std::string pcm = (cfg.dir_std / STD_PCM).string();
    if (!force && !flag("build-std") && !flag("build-all") && bld::fs::exists(pcm)) {
        return {};
    }

    bld::log::i("Building standard module...");
    auto cppm = find_std_cppm();
    if (!cppm) {
        bld::log::w("Could not find 'std.cppm'.");
        std::cout << "Path to std.cppm: ";
        std::string input;
        std::getline(std::cin, input);
        if (input.size() >= 2 && input.front() == '"') {
            input = input.substr(1, input.size() - 2);
        }
        if (!bld::fs::exists(input)) {
            return std::unexpected(bld::Err::erc(std::errc::no_such_file_or_directory, "std.cppm not found"));
        }
        std::ignore = bld::fs::write_file(CACHE_FILE, input);
        cppm = stdfs::path{input};
    }

    if (!bld::fs::make_dirs(cfg.dir_std.string())) {
        return std::unexpected(bld::Err::erc(std::errc::io_error, "cannot create std dir"));
    }

    bld::Cmd pre = cxx(cfg);
    pre.push("-stdlib=libc++");
    pre.push("--precompile");
    pre.push(cppm->string());
    pre.push("-o");
    pre.push(pcm);
    if (auto r = run_ok(pre); !r) {
        return r;
    }

    if (const auto compat = cppm->parent_path() / "std.compat.cppm"; bld::fs::exists(compat.string())) {
        bld::Cmd cc = cxx(cfg);
        cc.push("-stdlib=libc++");
        cc.push("-fprebuilt-module-path=" + cfg.dir_std.string() + "/");
        cc.push("--precompile");
        cc.push(compat.string());
        cc.push("-o");
        cc.push((cfg.dir_std / "std.compat.pcm").string());
        std::ignore = run_ok(cc);
    }
    return {};
}

// One representative task: command + inputs + output (+ compile-db mark).
// The absolute root cwd is stamped on every task: besides pinning task
// execution to the project root, it is what lands in compile_commands.json
// as "directory" — clangd ignores DB entries with a relative directory,
// so this is what makes module-aware IDE features work.
auto link_step(
    bld::Plan &plan,
    const std::string &root,
    const std::string &name,
    bld::Cmd cmd,
    const std::vector<std::string> &inputs,
    const std::string &output,
    bool compile_command = false) -> void
{
    bld::Exec_spec spec;
    spec.cmd = std::move(cmd);
    spec.cfg.cwd = root;
    plan.add(name, std::move(spec));
    for (const auto &in : inputs) {
        plan.needs(name, in);
    }
    plan.produces(name, output);
    if (compile_command) {
        plan.mark_compile_command(name);
    }
}

[[nodiscard]] auto collect_modules(const Config &cfg) -> std::expected<std::vector<Module>, bld::Err>
{
    std::vector<Module> mods;
    for (auto &m : bld::fs::scan_modules(cfg.dir_src.string())) {
        mods.push_back(Module{m});
    }
    if (mods.empty()) {
        return std::unexpected(bld::Err::erc(std::errc::no_such_file_or_directory, std::format("no modules found in {}", cfg.dir_src.string())));
    }
    return mods;
}

// jobs{} means "library default"; only pass an explicit budget when set.
auto run_graph(bld::Plan &plan, std::optional<std::size_t> jobs, bool force, std::string_view db = "")
    -> std::expected<bld::Run_result, bld::Err>
{
    if (jobs && force && !db.empty()) {
        return bld::run(plan, bld::jobs{*jobs}, bld::force{}, bld::write_compile_commands{db});
    }
    if (jobs && force) {
        return bld::run(plan, bld::jobs{*jobs}, bld::force{});
    }
    if (jobs && !db.empty()) {
        return bld::run(plan, bld::jobs{*jobs}, bld::write_compile_commands{db});
    }
    if (jobs) {
        return bld::run(plan, bld::jobs{*jobs});
    }
    if (force && !db.empty()) {
        return bld::run(plan, bld::force{}, bld::write_compile_commands{db});
    }
    if (force) {
        return bld::run(plan, bld::force{});
    }
    if (!db.empty()) {
        return bld::run(plan, bld::write_compile_commands{db});
    }
    return bld::run(plan);
}

auto build_main(const Config &cfg, std::optional<std::size_t> jobs, bool force) -> std::expected<void, bld::Err>
{
    if (!bld::fs::make_dirs(cfg.dir_pcm.string(), cfg.dir_obj.string(), cfg.dir_std.string(), cfg.dir_bin.string(), cfg.dir_libs.string())) {
        return std::unexpected(bld::Err::erc(std::errc::io_error, "cannot create build dirs"));
    }
    if (auto r = ensure_std(cfg, force); !r) {
        return r;
    }

    auto mods_res = collect_modules(cfg);
    if (!mods_res) {
        return std::unexpected(mods_res.error());
    }
    const auto &mods = *mods_res;

    std::unordered_map<std::string, std::string> providers;
    for (const auto &m : mods) {
        providers[m.inner.name] = m.pcm(cfg).string();
    }

    bld::Plan plan;
    std::vector<std::string> objs;
    objs.reserve(mods.size());
    const std::string std_pcm = (cfg.dir_std / STD_PCM).string();
    const auto inc = cfg.mod_paths();

    auto root_res = bld::fs::current_path();
    if (!root_res) {
        return std::unexpected(root_res.error());
    }
    const std::string root = std::move(*root_res);

    for (const auto &m : mods) {
        const std::string src = m.inner.file.string();
        const std::string pcm = m.pcm(cfg).string();
        const std::string obj = m.obj(cfg).string();

        bld::Cmd pre = cxx(cfg);
        pre.push("-stdlib=libc++");
        pre.push("--precompile");
        pre.push(src);
        pre.push("-o");
        pre.push(pcm);
        for (const auto &p : inc) {
            pre.push(p);
        }

        const std::string pt = "pcm:" + m.inner.name;
        bld::Exec_spec pre_spec;
        pre_spec.cmd = std::move(pre);
        pre_spec.cfg.cwd = root;
        plan.add(pt, std::move(pre_spec));
        plan.needs(pt, src);
        plan.needs(pt, std_pcm);
        for (const auto &dep : m.inner.imports) {
            if (dep == m.inner.name) {
                continue; // self-import picked up from a comment
            }
            if (auto it = providers.find(dep); it != providers.end()) {
                plan.needs(pt, it->second);
            } else {
                bld::log::w("Module '{}' imports '{}' (not found)", m.inner.name, dep);
            }
        }
        plan.produces(pt, pcm);
        plan.mark_compile_command(pt);

        bld::Cmd cc = cxx(cfg);
        cc.push("-c");
        cc.push(pcm);
        cc.push("-o");
        cc.push(obj);
        for (const auto &p : inc) {
            cc.push(p);
        }
        const std::string ot = "obj:" + m.inner.name;
        link_step(plan, root, ot, std::move(cc), {pcm}, obj);
        objs.push_back(obj);
    }

    const std::string static_lib = (cfg.dir_libs / cfg.lib_static).string();
    bld::Cmd ar{cfg.archiver, "rcs", static_lib};
    for (const auto &o : objs) {
        ar.push(o);
    }
    link_step(plan, root, "lib-static", std::move(ar), objs, static_lib);

    const std::string shared_lib = (cfg.dir_libs / cfg.lib_shared).string();
    bld::Cmd so{cfg.compiler, "-shared", "-o", shared_lib};
    for (const auto &f : cfg.flags_common) {
        so.push(f);
    }
    for (const auto &o : objs) {
        so.push(o);
    }
    for (const auto &f : cfg.flags_linker) {
        so.push(f);
    }
    link_step(plan, root, "lib-shared", std::move(so), objs, shared_lib);

    if (bld::fs::exists(cfg.main_src)) {
        const std::string exe = (cfg.dir_bin / cfg.exe_name).string();
        bld::Cmd link = cxx(cfg);
        link.push(cfg.main_src);
        for (const auto &o : objs) {
            link.push(o);
        }
        link.push("-o");
        link.push(exe);
        for (const auto &p : inc) {
            link.push(p);
        }
        for (const auto &f : cfg.flags_linker) {
            link.push(f);
        }
        std::vector<std::string> inputs{cfg.main_src};
        inputs.insert(inputs.end(), objs.begin(), objs.end());
        link_step(plan, root, "exe", std::move(link), inputs, exe, true);
    }

    bld::log::i("Building with {} threads...", bld::resolve_parallel_width(jobs));
    if (auto res = run_graph(plan, jobs, force, "compile_commands.json"); !res) {
        return std::unexpected(res.error());
    } else {
        bld::log::i("Build successful ({} ran, {} up to date).", res->ran, res->skipped);
    }
    return {};
}

// Examples are independent leaf tasks in a Plan, so the scheduler orders
// and skips them incrementally (a bare task span always runs everything).
auto build_examples(const Config &cfg, std::optional<std::size_t> jobs, std::string_view dir = "./examples/")
    -> std::expected<void, bld::Err>
{
    const std::string lib = (cfg.dir_libs / cfg.lib_static).string();
    const auto inc = cfg.mod_paths();
    bld::Plan plan;

    bld::fs::Controller ctl;
    for (const auto &e : bld::fs::walk_dir(dir, ctl) | bld::fs::only_files | bld::fs::only_extensions("cpp")) {
        const std::string src = e.path.string();
        auto rel = bld::fs::relative(src, dir);
        if (!rel) {
            return std::unexpected(rel.error());
        }
        std::string out = (cfg.dir_example_bin / *rel).string();
        out = out.substr(0, out.size() - std::string(".cpp").size());
        std::ignore = bld::fs::make_dirs(bld::fs::parent_dir(out));

        bld::Cmd c{cfg.compiler};
        c.push("-o");
        c.push(out);
        c.push(src);
        c.push(lib);
        for (const auto &f : cfg.flags_common) {
            c.push(f);
        }
        for (const auto &p : inc) {
            c.push(p);
        }
        for (const auto &f : cfg.flags_linker) {
            c.push(f);
        }
        plan.add(out, std::move(c));
        plan.needs(out, src);
        plan.needs(out, lib);
        plan.produces(out, out);
    }
    if (ctl.failed()) {
        return std::unexpected(bld::Err::erc(std::errc::io_error, "cannot scan examples: " + ctl.error().message()));
    }

    if (plan.tasks.empty()) {
        bld::log::i("No examples found.");
        return {};
    }
    bld::log::i("Building {} examples...", plan.tasks.size());
    if (auto res = run_graph(plan, jobs, false); !res) {
        return std::unexpected(res.error());
    } else {
        bld::log::i("Examples OK ({} built, {} up to date).", res->ran, res->skipped);
    }
    return {};
}

auto compile_one(const Config &cfg, std::string_view src, std::string_view out) -> std::expected<void, bld::Err>
{
    bld::Cmd c{cfg.compiler};
    c.push("-o");
    c.push(out);
    c.push(src);
    c.push((cfg.dir_libs / cfg.lib_static).string());
    for (const auto &f : cfg.flags_common) {
        c.push(f);
    }
    for (const auto &p : cfg.mod_paths()) {
        c.push(p);
    }
    for (const auto &f : cfg.flags_linker) {
        c.push(f);
    }
    return run_ok(c);
}

auto run_exe(const Config &cfg) -> int
{
    const std::string exe = (cfg.dir_bin / cfg.exe_name).string();
    if (!bld::fs::exists(exe)) {
        bld::log::e("Executable not found. Build first.");
        return 1;
    }
    auto proc = bld::run(bld::Cmd{exe});
    if (!proc) {
        bld::log::e("Failed to run executable: {}", proc.error());
        return 1;
    }
    return proc->status_code();
}

} // namespace

int main(int argc, char *argv[])
{
    if (auto r = bld::rebuild_this_when_needed_ext(argc, argv); !r) {
        bld::log::e("Failed to rebuild build script: {}", r.error());
        return 1;
    }
    if (auto parsed = parse_cli(argc, argv); !parsed) {
        bld::log::e("Failed to parse args: {}", parsed.error());
        return 1;
    } else if (parsed->help_requested) {
        return 0;
    }

    const Config cfg;
    if (flag("clean")) {
        std::ignore = bld::fs::remove(cfg.dir_libs.string(), cfg.dir_obj.string(), cfg.dir_pcm.string());
        return 0;
    }
    if (flag("clean-all")) {
        std::ignore = bld::fs::remove(cfg.dir_bin.string());
        return 0;
    }
    if (flag("clean-example")) {
        std::ignore = bld::fs::remove(cfg.dir_example_bin.string());
        return 0;
    }
    if (flag("std")) {
        if (auto r = ensure_std(cfg, true); !r) {
            bld::log::e("std build failed: {}", r.error());
            return 1;
        }
        return 0;
    }

    const auto jobs = opt_jobs();
    const bool force = flag("build-all");
    if (auto r = build_main(cfg, jobs, force); !r) {
        bld::log::e("Build failed: {}", r.error());
        return 1;
    }

    if (flag("run")) {
        return run_exe(cfg);
    }
    if (flag("build-examples")) {
        if (auto r = build_examples(cfg, jobs); !r) {
            bld::log::e("Examples failed: {}", r.error());
            return 1;
        }
        return 0;
    }
    if (const auto src = opt_str("compile"); !src.empty()) {
        if (const auto out = opt_str("o"); out.empty()) {
            bld::log::e("You must provide output file using '-o' flag.");
            return 1;
        } else if (auto r = compile_one(cfg, src, out); !r) {
            bld::log::e("Compile failed: {}", r.error());
            return 1;
        }
    }
    return 0;
}
