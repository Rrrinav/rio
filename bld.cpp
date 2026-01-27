#include <cstdlib>
#include <vector>
#include <string>
#include <string_view>
#include <algorithm>
#include <set>
#include <map>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>

#define B_LDR_IMPLEMENTATION
#include "b_ldr.hpp"


auto &bld_cfg = bld::Config::get();

namespace fs = std::filesystem;

// TODO: Support g++ too.
// TODO: Implement parallel build but if I have a bottleneck.
struct Config
{
    const fs::path dir_src = "src/";
    const fs::path dir_pcm = "bin/pcms/";
    const fs::path dir_obj = "bin/objs/";
    const fs::path dir_bin = "bin/";
    const fs::path dir_std = "bin/std/";
    const fs::path dir_libs = "bin/libs/";

    const std::string exe_name = "rio";
    const std::string lib_static = "librio.a";
    const std::string lib_shared = "librio.so";

    const std::string main_src = "main.cpp";
    const std::string compiler = "clang++";
    const std::string archiver = "ar";

    const std::vector<std::string> flags_common = {"-std=c++23", "-Wall", "-Wextra", "-O2", "-fPIC"};
    const std::vector<std::string> flags_stdlib = {"-stdlib=libc++"};

    std::vector<std::string> get_mod_flags() const
    {
        return {"-fprebuilt-module-path=" + dir_pcm.string() + "/", "-fprebuilt-module-path=" + dir_std.string() + "/"};
    }
};

struct Module
{
    std::string name;
    fs::path file;
    std::vector<std::string> deps;

    std::string safe_name() const
    {
        std::string s = name;
        std::replace(s.begin(), s.end(), ':', '-');
        return s;
    }
    fs::path pcm(const Config &cfg) const { return cfg.dir_pcm / (safe_name() + ".pcm"); }
    fs::path obj(const Config &cfg) const { return cfg.dir_obj / (safe_name() + ".o"); }
};

struct Compile_command
{
    std::string directory;
    std::string command;
    std::string file;
};

std::pair<bool, std::string> run_cmd(const std::vector<std::string> &parts, bool dry_run = false)
{
    std::string full_cmd_str;
    for (const auto &p : parts) full_cmd_str += p + " ";

    if (dry_run)
        return {true, full_cmd_str};

    bld::Command cmd;
    cmd.parts = parts;
    return {bld::execute(cmd), full_cmd_str};
}

void emit_json(const std::vector<Compile_command> &cmds)
{
    std::ofstream out("compile_commands.json");
    out << "[\n";
    for (size_t i = 0; i < cmds.size(); ++i)
    {
        out << "  {\n";
        out << "    \"directory\": \"" << fs::current_path().string() << "\",\n";
        out << "    \"command\": \"" << cmds[i].command << "\",\n";
        out << "    \"file\": \"" << fs::absolute(cmds[i].file).string() << "\"\n";
        out << "  }" << (i == cmds.size() - 1 ? "" : ",") << "\n";
    }
    out << "]\n";
}

const std::string CACHE_FILE = ".bld_std_path";

std::optional<fs::path> find_safe(const fs::path &root, const std::string &filename)
{
    if (!fs::exists(root))
        return std::nullopt;
    auto opts = fs::directory_options::skip_permission_denied;
    std::error_code ec;

    fs::recursive_directory_iterator it(root, opts, ec);
    fs::recursive_directory_iterator end;

    for (; it != end; it.increment(ec))
    {
        if (ec)
            continue;
        const auto &entry = *it;
        if (entry.is_directory())
        {
            std::string p = entry.path().string();
            bool likely = (p.find("include") != std::string::npos || p.find("c++") != std::string::npos ||
                           p.find("v1") != std::string::npos || p.find("share") != std::string::npos || p.find("lib") != std::string::npos);
            if (!likely)
            {
                it.disable_recursion_pending();
                continue;
            }
        }
        if (entry.is_regular_file() && entry.path().filename() == filename)
            return entry.path();
    }
    return std::nullopt;
}

std::optional<fs::path> load_cache()
{
    if (fs::exists(CACHE_FILE))
    {
        std::ifstream in(CACHE_FILE);
        std::string s;
        if (std::getline(in, s) && fs::exists(s))
            return fs::path(s);
    }
    return std::nullopt;
}

bool build_std_module(const Config &cfg)
{
    if (fs::exists(cfg.dir_std / "std.pcm") && !bld_cfg["build-all"])
        return true;

    bld::log(bld::Log_type::INFO, "Building Standard Module...");
    fs::path std_cppm;

    if (auto cached = load_cache())
    {
        std_cppm = *cached;
        bld::log(bld::Log_type::INFO, "Using cached path: " + std_cppm.string());
    }
    else
    {
        bld::log(bld::Log_type::INFO, "Searching for std.cppm...");
        std::vector<fs::path> roots = {"/usr/share", "/usr/lib", "/usr/lib64", "/usr/include", "/usr/local/share", "/opt/homebrew", "/usr"};
        for (const auto &r : roots)
        {
            if (auto found = find_safe(r, "std.cppm"))
            {
                std_cppm = *found;
                break;
            }
        }
    }

    if (std_cppm.empty())
    {
        bld::log(bld::Log_type::WARNING, "Could not find 'std.cppm' automatically.");
        std::cout << "Please enter the absolute path to 'std.cppm': ";
        std::string input;
        std::getline(std::cin, input);
        if (input.size() >= 2 && input.front() == '"' && input.back() == '"')
            input = input.substr(1, input.size() - 2);
        size_t last = input.find_last_not_of(" \t\n\r");
        if (last != std::string::npos)
            input = input.substr(0, last + 1);

        fs::path p(input);
        if (fs::exists(p))
        {
            std_cppm = p;
            std::ofstream out(CACHE_FILE);
            out << std_cppm.string();
        }
        else
        {
            bld::log(bld::Log_type::ERR, "Invalid path. Aborting.");
            return false;
        }
    }

    std::vector<std::string> args = {cfg.compiler};
    args.insert(args.end(), cfg.flags_common.begin(), cfg.flags_common.end());
    args.insert(args.end(), cfg.flags_stdlib.begin(), cfg.flags_stdlib.end());

    std::vector<std::string> cmd1 = args;
    cmd1.insert(cmd1.end(), {"--precompile", std_cppm.string(), "-o", (cfg.dir_std / "std.pcm").string()});
    if (!run_cmd(cmd1).first)
        return false;

    fs::path compat = std_cppm.parent_path() / "std.compat.cppm";
    if (fs::exists(compat))
    {
        std::vector<std::string> cmd2 = args;
        cmd2.push_back("-fprebuilt-module-path=" + cfg.dir_std.string() + "/");
        cmd2.insert(cmd2.end(), {"--precompile", compat.string(), "-o", (cfg.dir_std / "std.compat.pcm").string()});
        run_cmd(cmd2);
    }
    return true;
}

std::vector<Module> scan_modules(const Config &cfg)
{
    std::vector<Module> modules;
    auto walker = [&](bld::fs::Walk_fn_opt &opt) -> bool
    {
        if (!fs::is_regular_file(opt.path) || opt.path.extension() != ".cppm")
            return true;
        std::ifstream file(opt.path);
        std::string raw_line;
        Module mod;
        mod.file = opt.path;
        bool found = false, in_comment = false;
        std::vector<std::string> raw_deps;
        std::string primary;

        while (std::getline(file, raw_line))
        {
            std::string clean;
            clean.reserve(raw_line.size());
            for (size_t i = 0; i < raw_line.size(); ++i)
            {
                if (in_comment)
                {
                    if (i + 1 < raw_line.size() && raw_line[i] == '*' && raw_line[i + 1] == '/')
                    {
                        in_comment = false;
                        i++;
                        clean += ' ';
                    }
                }
                else
                {
                    if (i + 1 < raw_line.size() && raw_line[i] == '/' && raw_line[i + 1] == '/')
                        break;
                    else if (i + 1 < raw_line.size() && raw_line[i] == '/' && raw_line[i + 1] == '*')
                    {
                        in_comment = true;
                        i++;
                        clean += ' ';
                    }
                    else
                        clean += raw_line[i];
                }
            }
            if (clean.empty())
                continue;
            std::string_view line = clean;
            size_t f = line.find_first_not_of(" \t");
            if (f == std::string_view::npos)
                continue;
            line.remove_prefix(f);
            if (line.starts_with("export"))
            {
                line.remove_prefix(6);
                size_t n = line.find_first_not_of(" \t");
                if (n != std::string_view::npos)
                    line.remove_prefix(n);
            }
            bool is_m = line.starts_with("module"), is_i = line.starts_with("import");
            if (is_m || is_i)
            {
                line.remove_prefix(6);
                size_t s = line.find_first_not_of(" \t");
                if (s == std::string_view::npos)
                    continue;
                line.remove_prefix(s);
                size_t e = line.find_first_of(" \t;");
                if (e == std::string_view::npos)
                    continue;
                std::string n(line.substr(0, e));
                if (is_m)
                {
                    mod.name = n;
                    found = true;
                    size_t c = n.find(':');
                    primary = (c != std::string::npos) ? n.substr(0, c) : n;
                }
                else if (is_i && !n.starts_with("std") && n != "std.compat")
                    raw_deps.push_back(std::move(n));
            }
        }
        if (found)
        {
            for (const auto &d : raw_deps) mod.deps.push_back(d.starts_with(':') ? primary + d : d);
            modules.push_back(std::move(mod));
        }
        return true;
    };
    bld::fs::walk_directory(cfg.dir_src.string(), walker);
    return modules;
}

std::vector<Module> sort_modules(const std::vector<Module> &input)
{
    std::map<std::string, const Module *> lookup;
    for (const auto &m : input) lookup[m.name] = &m;
    std::vector<Module> sorted;
    std::set<std::string> visited, temp;
    auto visit = [&](auto &&self, const std::string &name) -> void
    {
        if (visited.contains(name))
            return;
        if (temp.contains(name))
        {
            bld::log(bld::Log_type::ERR, "Cycle: " + name);
            exit(1);
        }
        temp.insert(name);
        if (lookup.contains(name))
        {
            for (const auto &d : lookup[name]->deps) self(self, d);
            sorted.push_back(*lookup[name]);
        }
        temp.erase(name);
        visited.insert(name);
    };
    for (const auto &m : input) visit(visit, m.name);
    return sorted;
}

// --- Main ---
int main(int argc, char *argv[])
{
    BLD_REBUILD_YOURSELF_ONCHANGE();
    BLD_HANDLE_ARGS();

    Config cfg;

    if (bld_cfg["clean-all"])
    {
        bld::fs::remove_dir(cfg.dir_bin);
        return 0;
    }
    if (bld_cfg["clean"])
    {
        std::vector<std::string> dirs;
        bld::fs::walk_directory(cfg.dir_bin, [&] (bld::fs::Walk_fn_opt& opt) -> bool {
            // Idk how to handle this '+ "/"' thing properly, standard library must handle this but ok.
            // They must do semantic and not lexical comparison.
            if ((opt.path.string() + "/") == cfg.dir_std.string())
            {
                opt.action = bld::fs::Walk_act::Ignore;
                return true;
            }
            if (std::filesystem::is_directory(opt.path))
            {
                dirs.push_back(opt.path.string());
            }
            return true;
        });
        for (auto& d : dirs)
            bld::fs::remove_dir(d);
        return 0;
    }
    if (bld_cfg["run"])
    {
        std::string command = cfg.dir_bin.string() + cfg.exe_name;
        std::system(command.c_str());
        return 0;
    }

    fs::create_directories(cfg.dir_pcm);
    fs::create_directories(cfg.dir_obj);
    fs::create_directories(cfg.dir_std);
    fs::create_directories(cfg.dir_bin);
    fs::create_directories(cfg.dir_libs);

    if (!build_std_module(cfg))
        return 1;

    auto modules = sort_modules(scan_modules(cfg));

    bool link_needed = false;
    std::vector<Compile_command> json_entries;

    // 1. Build Modules
    for (const auto &mod : modules)
    {
        fs::path pcm = mod.pcm(cfg);
        fs::path obj = mod.obj(cfg);

        bool build = false;
        if (bld_cfg["build-all"]) build = true;
        else build = bld::is_executable_outdated(mod.file, pcm);

        // A. Precompile
        std::vector<std::string> cmd_pcm = {cfg.compiler};
        cmd_pcm.insert(cmd_pcm.end(), cfg.flags_common.begin(), cfg.flags_common.end());
        cmd_pcm.insert(cmd_pcm.end(), cfg.flags_stdlib.begin(), cfg.flags_stdlib.end());
        cmd_pcm.push_back("--precompile");
        cmd_pcm.push_back(mod.file.string());
        cmd_pcm.push_back("-o");
        cmd_pcm.push_back(pcm.string());
        auto paths = cfg.get_mod_flags();
        cmd_pcm.insert(cmd_pcm.end(), paths.begin(), paths.end());

        auto res_pcm = run_cmd(cmd_pcm, !build);
        json_entries.push_back({cfg.dir_src.string(), res_pcm.second, mod.file.string()});
        if (!res_pcm.first)
            return 1;

        // B. Compile Object
        std::vector<std::string> cmd_obj = {cfg.compiler};
        cmd_obj.insert(cmd_obj.end(), cfg.flags_common.begin(), cfg.flags_common.end());
        cmd_obj.push_back("-c");
        cmd_obj.push_back(pcm.string());
        cmd_obj.push_back("-o");
        cmd_obj.push_back(obj.string());
        cmd_obj.insert(cmd_obj.end(), paths.begin(), paths.end());

        if (build)
        {
            bld::log(bld::Log_type::INFO, "Compiling: " + mod.name);
            if (!run_cmd(cmd_obj).first)
                return 1;
            link_needed = true;
        }
    }

    fs::path static_lib = cfg.dir_libs / cfg.lib_static;
    if (link_needed || !fs::exists(static_lib))
    {
        bld::log(bld::Log_type::INFO, "Archiving: " + cfg.lib_static);
        std::vector<std::string> cmd_ar = {cfg.archiver, "rcs", static_lib.string()};
        for (const auto &m : modules) cmd_ar.push_back(m.obj(cfg).string());
        if (!run_cmd(cmd_ar).first)
            return 1;
    }

    fs::path shared_lib = cfg.dir_libs / cfg.lib_shared;
    if (link_needed || !fs::exists(shared_lib))
    {
        bld::log(bld::Log_type::INFO, "Linking Shared: " + cfg.lib_shared);
        std::vector<std::string> cmd_so = {cfg.compiler, "-shared", "-o", shared_lib.string()};

        cmd_so.insert(cmd_so.end(), cfg.flags_common.begin(), cfg.flags_common.end());
        cmd_so.insert(cmd_so.end(), cfg.flags_stdlib.begin(), cfg.flags_stdlib.end());

        for (const auto &m : modules) cmd_so.push_back(m.obj(cfg).string());

        // Include system libs (luring)
        cmd_so.push_back("-luring");

        if (!run_cmd(cmd_so).first)
            return 1;
    }

    // 4. Link Executable
    fs::path exe = cfg.dir_bin / cfg.exe_name;

    bool build_req = false;

    if (bld_cfg["build-all"]) build_req = true;
    else build_req = bld::is_executable_outdated(cfg.main_src, exe);

    if (link_needed || build_req)
    {
        bld::log(bld::Log_type::INFO, "Linking Executable...");
        std::vector<std::string> cmd_link = {cfg.compiler};
        cmd_link.insert(cmd_link.end(), cfg.flags_common.begin(), cfg.flags_common.end());
        cmd_link.insert(cmd_link.end(), cfg.flags_stdlib.begin(), cfg.flags_stdlib.end());
        cmd_link.push_back(cfg.main_src);
        for (const auto &m : modules) cmd_link.push_back(m.obj(cfg).string());
        cmd_link.push_back("-o");
        cmd_link.push_back(exe.string());
        auto paths = cfg.get_mod_flags();
        cmd_link.insert(cmd_link.end(), paths.begin(), paths.end());
        cmd_link.push_back("-luring");

        auto res_link = run_cmd(cmd_link);
        if (!res_link.first)
            return 1;
        json_entries.push_back({fs::current_path().string(), res_link.second, cfg.main_src});
        bld::log(bld::Log_type::INFO, "Done.");
    }
    else
    {
        bld::log(bld::Log_type::INFO, "Up to date.");
    }

    bld::log(bld::Log_type::INFO, "Emitting json...");
    emit_json(json_entries);
    return 0;
}
