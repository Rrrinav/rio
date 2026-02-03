#include <cstdlib>
#include <print>
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <thread>

#define B_LDR_IMPLEMENTATION
#include "b_ldr.hpp"

namespace fs = std::filesystem;
auto &bld_cfg = bld::Config::get();

// ==============================================================================
// 1. CONFIGURATION
// ==============================================================================

struct Config
{
    // Directories
    const fs::path dir_src = "src/";
    const fs::path dir_bin = "bin/";
    const fs::path dir_pcm = "bin/pcms/";
    const fs::path dir_obj = "bin/objs/";
    const fs::path dir_std = "bin/std/";
    const fs::path dir_libs = "bin/libs/";

    // Artifacts
    const std::string exe_name = "rio";
    const std::string lib_static = "librio.a";
    const std::string lib_shared = "librio.so";
    const std::string main_src = "main.cpp";

    // Tools
    const std::string compiler = "clang++";
    const std::string archiver = "ar";

    // Flags - Enforce libc++ globally
    const std::vector<std::string> flags_common = {"-std=c++23", "-stdlib=libc++", "-Wall", "-Wextra", "-O2", "-fPIC", "-g"};

    // Linker Flags
    const std::vector<std::string> flags_linker = {"-stdlib=libc++", "-luring", "-lc++abi"};

    std::vector<std::string> get_mod_paths() const
    {
        return {"-fprebuilt-module-path=" + dir_pcm.string(), "-fprebuilt-module-path=" + dir_std.string()};
    }
};

struct Module
{
    std::string name;
    fs::path file;
    std::vector<std::string> imports;

    std::string safe_name() const
    {
        std::string s = name;
        std::replace(s.begin(), s.end(), ':', '-');
        return s;
    }
    fs::path pcm(const Config &cfg) const { return cfg.dir_pcm / (safe_name() + ".pcm"); }
    fs::path obj(const Config &cfg) const { return cfg.dir_obj / (safe_name() + ".o"); }
};

struct CompilationEntry
{
    std::string directory;
    std::string command;
    std::string file;
};

// ==============================================================================
// 2. HELPERS
// ==============================================================================

bld::Command make_cmd(const std::vector<std::string> &parts)
{
    bld::Command cmd;
    cmd.parts = parts;
    return cmd;
}

void emit_json(const std::vector<CompilationEntry> &entries)
{
    std::ofstream out("compile_commands.json");
    out << "[\n";
    for (size_t i = 0; i < entries.size(); ++i)
    {
        out << "  {\n";
        out << "    \"directory\": \"" << fs::current_path().string() << "\",\n";
        out << "    \"command\": \"" << entries[i].command << "\",\n";
        out << "    \"file\": \"" << fs::absolute(entries[i].file).string() << "\"\n";
        out << "  }" << (i == entries.size() - 1 ? "" : ",") << "\n";
    }
    out << "]\n";
}

// ==============================================================================
// 3. STD MODULE BUILDER
// ==============================================================================

const std::string CACHE_FILE = ".bld_std_path";

std::optional<fs::path> find_std_cppm()
{
    if (fs::exists(CACHE_FILE))
    {
        std::ifstream in(CACHE_FILE);
        std::string s;
        if (std::getline(in, s) && fs::exists(s))
            return fs::path(s);
    }
    std::vector<fs::path> roots = {"/usr/lib/llvm-19/share/libc++/v1", "/usr/lib/llvm-18/share/libc++/v1", "/usr/share/libc++/v1",
        "/usr/local/share", "/opt/homebrew"};
    for (const auto &r : roots)
    {
        if (!fs::exists(r))
            continue;
        auto opts = fs::directory_options::skip_permission_denied;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(r, opts, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
                continue;
            if (it->is_directory())
            {
                std::string p = it->path().string();
                if (p.find("include") == std::string::npos && p.find("c++") == std::string::npos && p.find("v1") == std::string::npos)
                    it.disable_recursion_pending();
                continue;
            }
            if (it->path().filename() == "std.cppm")
                return it->path();
        }
    }
    return std::nullopt;
}

bool build_std_module(const Config &cfg)
{
    if (!bld_cfg["build-std"] && fs::exists(cfg.dir_std / "std.pcm") && !bld_cfg["build-all"])
        return true;

    bld::log(bld::Log_type::INFO, "Building Standard Module...");
    auto std_cppm_opt = find_std_cppm();

    if (!std_cppm_opt)
    {
        bld::log(bld::Log_type::WARNING, "Could not find 'std.cppm'.");
        std::cout << "Path to std.cppm: ";
        std::string input;
        std::getline(std::cin, input);
        if (input.size() >= 2 && input.front() == '"')
            input = input.substr(1, input.size() - 2);
        if (fs::exists(input))
        {
            std_cppm_opt = input;
            std::ofstream out(CACHE_FILE);
            out << input;
        }
        else
            return false;
    }

    fs::path std_cppm = *std_cppm_opt;
    fs::create_directories(cfg.dir_std);

    std::vector<std::string> args = {cfg.compiler};
    args.insert(args.end(), cfg.flags_common.begin(), cfg.flags_common.end());

    std::vector<std::string> cmd1 = args;
    cmd1.insert(cmd1.end(), {"--precompile", std_cppm.string(), "-o", (cfg.dir_std / "std.pcm").string()});
    if (!bld::execute(make_cmd(cmd1)).normal)
        return false;

    fs::path compat = std_cppm.parent_path() / "std.compat.cppm";
    if (fs::exists(compat))
    {
        std::vector<std::string> cmd2 = args;
        cmd2.push_back("-fprebuilt-module-path=" + cfg.dir_std.string() + "/");
        cmd2.insert(cmd2.end(), {"--precompile", compat.string(), "-o", (cfg.dir_std / "std.compat.pcm").string()});
        bld::execute(make_cmd(cmd2));
    }
    return true;
}

// ==============================================================================
// 4. ROBUST MODULE SCANNER (Tokenizer)
// ==============================================================================

// Reads file content, strips comments (// and /* */), and returns clean string
std::string read_clean_source(const fs::path &path)
{
    std::ifstream file(path);
    if (!file)
        return "";

    std::string src((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::string clean;
    clean.reserve(src.size());

    bool in_block = false;
    bool in_line = false;
    bool in_str = false;

    for (size_t i = 0; i < src.size(); ++i)
    {
        if (in_block)
        {
            if (src[i] == '*' && i + 1 < src.size() && src[i + 1] == '/')
            {
                in_block = false;
                i++;
                clean += ' ';
            }
        }
        else if (in_line)
        {
            if (src[i] == '\n')
            {
                in_line = false;
                clean += '\n';
            }
        }
        else if (in_str)
        {
            clean += src[i];
            if (src[i] == '"' && src[i - 1] != '\\')
                in_str = false;
        }
        else if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*')
        {
            in_block = true;
            i++;
            clean += ' ';
        }
        else if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/')
        {
            in_line = true;
            i++;
        }
        else if (src[i] == '"')
        {
            in_str = true;
            clean += '"';
        }
        else
        {
            clean += src[i];
        }
    }
    return clean;
}

std::vector<Module> scan_modules(const Config &cfg)
{
    std::vector<Module> modules;

    bld::fs::walk_directory(cfg.dir_src.string(),
        [&](bld::fs::Walk_fn_opt &opt) -> bool
        {
            if (!fs::is_regular_file(opt.path) || opt.path.extension() != ".cppm")
                return true;

            std::string content = read_clean_source(opt.path);
            if (content.empty())
                return true;

            // Tokenize
            std::vector<std::string> tokens;
            std::string token;
            for (char c : content)
            {
                if (std::isspace(c) || c == ';')
                {
                    if (!token.empty())
                    {
                        tokens.push_back(token);
                        token.clear();
                    }
                    if (c == ';')
                        tokens.push_back(";");
                }
                else
                {
                    token += c;
                }
            }

            Module mod;
            mod.file = opt.path;
            bool found_name = false;
            std::string primary_name;
            std::unordered_set<std::string> seen_imports;

            for (size_t i = 0; i < tokens.size(); ++i)
            {
                if (tokens[i] == "export" && i + 2 < tokens.size() && tokens[i + 1] == "module")
                {
                    mod.name = tokens[i + 2];
                    found_name = true;
                    // Get primary name from partition (rio:part -> rio)
                    size_t colon = mod.name.find(':');
                    primary_name = (colon != std::string::npos) ? mod.name.substr(0, colon) : mod.name;
                }
                // Handle: import name; OR export import name;
                else if (tokens[i] == "import")
                {
                    // Skip if this is part of "export import"
                    if (i > 0 && tokens[i - 1] == "export")
                        continue;

                    if (i + 1 < tokens.size())
                    {
                        std::string dep = tokens[i + 1];
                        // Skip std and empty
                        if (!dep.starts_with("std") && dep != ";" && !dep.empty())
                        {
                            if (dep.starts_with(':') && !primary_name.empty())
                                dep = primary_name + dep;

                            // Only add if not seen before
                            if (seen_imports.insert(dep).second)
                                mod.imports.push_back(dep);
                        }
                    }
                }
                else if (tokens[i] == "export" && i + 2 < tokens.size() && tokens[i + 1] == "import")
                {
                    std::string dep = tokens[i + 2];
                    // Skip std and empty
                    if (!dep.starts_with("std") && dep != ";" && !dep.empty())
                    {
                        if (dep.starts_with(':') && !primary_name.empty())
                            dep = primary_name + dep;

                        // Only add if not seen before
                        if (seen_imports.insert(dep).second)
                            mod.imports.push_back(dep);
                    }
                }
            }

            if (found_name)
                modules.push_back(std::move(mod));
            else
                bld::log(bld::Log_type::WARNING, "Skipped file (no module decl found): " + opt.path.string());
            return true;
        });
    return modules;
}

// ==============================================================================
// 5. MAIN
// ==============================================================================

int main(int argc, char *argv[])
{
    BLD_REBUILD_YOURSELF_ONCHANGE();
    BLD_HANDLE_ARGS();
    Config cfg;

    if (bld_cfg["clean"] || bld_cfg["clean-all"])
    {
        bld::fs::remove_dir(cfg.dir_bin);
        if (!bld_cfg["clean-all"])
            fs::create_directories(cfg.dir_std);
        return 0;
    }

    if (bld_cfg["run"])
    {
        std::string cmd = (cfg.dir_bin / cfg.exe_name).string();
        if (!fs::exists(cmd))
        {
            bld::log(bld::Log_type::ERR, "Executable not found. Build first.");
            return 1;
        }
        return std::system(cmd.c_str());
    }

    if (bld_cfg["compile"])
    {
        std::string input = bld_cfg["compile"];
        std::string output = bld_cfg["o"] ? (std::string)bld_cfg["o"] : "a.out";
        bld::Command cmd = make_cmd({cfg.compiler});
        cmd.add_parts("-o", output, input);
        cmd.add_parts(cfg.dir_libs.string() + cfg.lib_static);
        cmd.parts.append_range(cfg.flags_common);
        cmd.parts.append_range(cfg.get_mod_paths());
        cmd.parts.append_range(cfg.flags_linker);
        return bld::execute(cmd).normal ? 0 : 1;
    }

    // --- SETUP ---
    fs::create_directories(cfg.dir_pcm);
    fs::create_directories(cfg.dir_obj);
    fs::create_directories(cfg.dir_std);
    fs::create_directories(cfg.dir_bin);
    fs::create_directories(cfg.dir_libs);

    if (!build_std_module(cfg))
        return 1;

    // 1. Scan
    auto modules = scan_modules(cfg);
    if (modules.empty())
    {
        bld::log(bld::Log_type::ERR, "No modules found in " + cfg.dir_src.string());
        return 1;
    }

    // 2. Map
    std::unordered_map<std::string, std::string> mod_map;
    for (const auto &m : modules) mod_map[m.name] = m.pcm(cfg).string();

    // 3. Build Graph
    bld::Dep_graph graph;
    std::vector<CompilationEntry> json_entries;
    std::vector<std::string> all_objs;
    auto mod_paths = cfg.get_mod_paths();

    for (const auto &mod : modules)
    {
        // A. PCM
        std::vector<std::string> pcm_deps = {mod.file.string()};
        if (fs::exists(cfg.dir_std / "std.pcm"))
            pcm_deps.push_back((cfg.dir_std / "std.pcm").string());

        for (const auto &d : mod.imports)
            if (mod_map.contains(d))
                pcm_deps.push_back(mod_map[d]);
            else
                bld::log(bld::Log_type::WARNING, "Module '" + mod.name + "' imports '" + d + "' (not found)");

        std::vector<std::string> cmd_pcm = {cfg.compiler};
        cmd_pcm.insert(cmd_pcm.end(), cfg.flags_common.begin(), cfg.flags_common.end());
        cmd_pcm.push_back("--precompile");
        cmd_pcm.push_back(mod.file.string());
        cmd_pcm.push_back("-o");
        cmd_pcm.push_back(mod.pcm(cfg).string());
        cmd_pcm.insert(cmd_pcm.end(), mod_paths.begin(), mod_paths.end());

        bld::Command c_pcm = make_cmd(cmd_pcm);
        graph.add_dep(bld::Dep(mod.pcm(cfg).string(), pcm_deps, c_pcm));
        json_entries.push_back({cfg.dir_src.string(), c_pcm.get_command_string(), mod.file.string()});

        // B. OBJ
        std::vector<std::string> obj_deps = {mod.pcm(cfg).string()};
        std::vector<std::string> cmd_obj = {cfg.compiler};
        cmd_obj.insert(cmd_obj.end(), cfg.flags_common.begin(), cfg.flags_common.end());
        cmd_obj.push_back("-c");
        cmd_obj.push_back(mod.pcm(cfg).string());
        cmd_obj.push_back("-o");
        cmd_obj.push_back(mod.obj(cfg).string());
        cmd_obj.insert(cmd_obj.end(), mod_paths.begin(), mod_paths.end());

        bld::Command c_obj = make_cmd(cmd_obj);
        graph.add_dep(bld::Dep(mod.obj(cfg).string(), obj_deps, c_obj));
        all_objs.push_back(mod.obj(cfg).string());
    }

    // C. Libs
    std::string static_lib = (cfg.dir_libs / cfg.lib_static).string();
    std::vector<std::string> ar_cmd = {cfg.archiver, "rcs", static_lib};
    ar_cmd.insert(ar_cmd.end(), all_objs.begin(), all_objs.end());
    graph.add_dep(bld::Dep(static_lib, all_objs, make_cmd(ar_cmd)));

    std::string shared_lib = (cfg.dir_libs / cfg.lib_shared).string();
    std::vector<std::string> so_cmd = {cfg.compiler, "-shared", "-o", shared_lib};
    so_cmd.insert(so_cmd.end(), cfg.flags_common.begin(), cfg.flags_common.end());
    so_cmd.insert(so_cmd.end(), all_objs.begin(), all_objs.end());
    so_cmd.insert(so_cmd.end(), cfg.flags_linker.begin(), cfg.flags_linker.end());
    graph.add_dep(bld::Dep(shared_lib, all_objs, make_cmd(so_cmd)));

    // D. Exe
    std::string exe_path = (cfg.dir_bin / cfg.exe_name).string();
    if (fs::exists(cfg.main_src))
    {
        std::vector<std::string> exe_deps = {cfg.main_src};
        exe_deps.insert(exe_deps.end(), all_objs.begin(), all_objs.end());

        std::vector<std::string> link_cmd = {cfg.compiler};
        link_cmd.insert(link_cmd.end(), cfg.flags_common.begin(), cfg.flags_common.end());
        link_cmd.push_back(cfg.main_src);
        link_cmd.insert(link_cmd.end(), all_objs.begin(), all_objs.end());
        link_cmd.push_back("-o");
        link_cmd.push_back(exe_path);
        link_cmd.insert(link_cmd.end(), mod_paths.begin(), mod_paths.end());
        link_cmd.insert(link_cmd.end(), cfg.flags_linker.begin(), cfg.flags_linker.end());

        bld::Command c_link = make_cmd(link_cmd);
        graph.add_dep(bld::Dep(exe_path, exe_deps, c_link));
        json_entries.push_back({fs::current_path().string(), c_link.get_command_string(), cfg.main_src});
    }

    // --- EXECUTE ---
    std::vector<std::string> final_targets = {static_lib, shared_lib};
    if (fs::exists(cfg.main_src))
        final_targets.push_back(exe_path);
    graph.add_phony("all", final_targets);

    size_t threads = bld_cfg["j"];
    if (threads == 0)
        threads = std::thread::hardware_concurrency();

    bld::log(bld::Log_type::INFO, "Building with " + std::to_string(threads) + " threads...");

    if (graph.build_parallel("all", 7))
    {
        bld::log(bld::Log_type::INFO, "Build Successful.");
        emit_json(json_entries);
        return 0;
    }
    else
    {
        bld::log(bld::Log_type::ERR, "Build Failed.");
        return 1;
    }
}
