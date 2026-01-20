#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <set>
#include <map>
#include <filesystem>
#include <fstream>

#define B_LDR_IMPLEMENTATION
#include "b_ldr.hpp"

namespace fs = std::filesystem;

// --- Configuration ---
const std::vector<std::string> Flags = {"-std=c++23", "-Wall", "-Wextra", "-O2"};
const std::string Src = "src/";
const std::string Build = "build/";

struct module_info
{
    std::string name;
    std::string file;
    std::vector<std::string> deps;
};

// --- Module Registry ---
std::vector<module_info> modules = {
    {"rio.utils:assert",              Src + "utils/assert.cppm",               {}},
    {"rio.utils:result",              Src + "utils/result.cppm",               {}},
    {"rio.utils",                     Src + "utils/utils.cppm",                {"rio.utils:assert", "rio.utils:result"}},
    {"rio.socket:address",            Src + "sockets/address.cppm",            {"rio.utils"}},
    {"rio.socket:tcp_socket",         Src + "sockets/tcp_socket.cppm",         {"rio.socket:address", "rio.utils", "rio.handle"}},
    {"rio.socket",                    Src + "socket.cppm",                     {"rio.socket:address", "rio.socket:tcp_socket", "rio.utils"}},
    {"rio.handle",                    Src + "handle.cppm",                     {}},
    {"rio.file",                      Src + "file.cppm",                       {"rio.utils", "rio.handle"}},
    {"rio.context",                   Src + "context.cppm",                    {}},
    {"rio.io",                        Src + "io/io.cppm",                      {"rio.utils", "rio.file"}},
    {"rio",                           Src + "rio.cppm",                        {"rio.utils", "rio.io", "rio.file", "rio.context", "rio.socket"}}
};

// --- Helpers ---

std::string to_path(std::string name, const std::string &ext)
{
    std::replace(name.begin(), name.end(), ':', '-');
    return Build + name + ext;
}

void collect_deps(const std::string &name, const std::map<std::string, std::vector<std::string>> &map, std::set<std::string> &out)
{
    if (map.contains(name))
    {
        for (const auto &d : map.at(name))
            if (out.insert(d).second)
                collect_deps(d, map, out);
    }
}

std::vector<module_info> get_build_order(const std::vector<module_info> &input)
{
    std::map<std::string, const module_info *> lookup;
    for (const auto &m : input) lookup[m.name] = &m;

    std::vector<module_info> sorted;
    std::set<std::string> visited;
    std::set<std::string> temp;

    auto visit = [&](auto &&self, const std::string &name) -> void
    {
        if (visited.contains(name))
            return;
        if (temp.contains(name))
        {
            bld::log(bld::Log_type::ERR, "Cycle detected: " + name);
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

bool is_outdated(const std::string &source, const std::string &target)
{
    if (!fs::exists(source))
    {
        bld::log(bld::Log_type::ERR, "Missing source file: " + source);
        exit(1);
    }
    if (!fs::exists(target))
        return true;
    return fs::last_write_time(source) > fs::last_write_time(target);
}

void emit_compile_commands(const std::string &directory, const std::vector<std::string> &commands, const std::vector<std::string> &files)
{
    std::ofstream out("compile_commands.json");
    out << "[\n";
    for (size_t i = 0; i < commands.size(); ++i)
    {
        out << "  {\n";
        out << "    \"directory\": \"" << fs::absolute(directory).string() << "\",\n";
        out << "    \"command\": \"" << commands[i] << "\",\n";
        out << "    \"file\": \"" << fs::absolute(files[i]).string() << "\"\n";
        out << "  }" << (i == commands.size() - 1 ? "" : ",") << "\n";
    }
    out << "]\n";
}

// --- Main ---

int main(int argc, char *argv[])
{
    BLD_REBUILD_YOURSELF_ONCHANGE();
    auto &cfg = bld::Config::get();
    BLD_HANDLE_ARGS();

    if (!fs::exists(Build))
        fs::create_directory(Build);

    std::map<std::string, std::vector<std::string>> dep_map;
    for (const auto &m : modules) dep_map[m.name] = m.deps;

    auto build_order = get_build_order(modules);

    std::vector<std::string> json_cmds;
    std::vector<std::string> json_files;

    for (const auto &mod : build_order)
    {
        std::string pcm = to_path(mod.name, ".pcm");
        std::string obj = to_path(mod.name, ".o");

        // Prepare Precompile Command
        std::vector<std::string> pc_cmd = {"clang++"};
        for (auto &f : Flags) pc_cmd.push_back(f);
        pc_cmd.push_back("--precompile");
        pc_cmd.push_back(mod.file);
        pc_cmd.push_back("-o");
        pc_cmd.push_back(pcm);

        std::set<std::string> all_deps;
        collect_deps(mod.name, dep_map, all_deps);
        for (const auto &d : all_deps) pc_cmd.push_back("-fmodule-file=" + d + "=" + to_path(d, ".pcm"));

        // Capture for LSP
        std::string full_cmd_str = cfg.compiler;
        for (auto &part : pc_cmd)
            if (part != cfg.compiler)
                full_cmd_str += " " + part;
        json_cmds.push_back(full_cmd_str);
        json_files.push_back(mod.file);

        // Build if outdated
        if (is_outdated(mod.file, pcm))
        {
            bld::log(bld::Log_type::INFO, "Building: " + mod.name);
            bld::Command cmd;
            cmd.parts = pc_cmd;
            if (!bld::execute(cmd))
                return 1;

            std::vector<std::string> obj_cmd = {"clang++"};
            for (auto &f : Flags) obj_cmd.push_back(f);
            obj_cmd.push_back("-c");
            obj_cmd.push_back(pcm);
            obj_cmd.push_back("-o");
            obj_cmd.push_back(obj);
            // Objects also need the module mapping
            for (const auto &d : all_deps) obj_cmd.push_back("-fmodule-file=" + d + "=" + to_path(d, ".pcm"));

            cmd.parts = obj_cmd;

            if (!bld::execute(cmd))
                return 1;
        }
    }

    emit_compile_commands(".", json_cmds, json_files);
    // 4. Final Linking Stage
    std::string executable = Src + "rio";
    std::string main_source = "main.cpp";  // Your entry point

    // Check if we need to relink (if any object file or main.cpp is newer than the binary)
    bool need_relink = is_outdated(main_source, executable);
    for (const auto &mod : modules)
        if (is_outdated(to_path(mod.name, ".o"), executable))
            need_relink = true;

    if (need_relink)
    {
        bld::log(bld::Log_type::INFO, "Linking final executable: " + executable);

        std::vector<std::string> link_cmd = {"clang++"};
        for (auto &f : Flags) link_cmd.push_back(f);

        link_cmd.push_back(main_source);
        link_cmd.push_back("-o");
        link_cmd.push_back(executable);

        // Add all module objects
        for (const auto &mod : build_order) link_cmd.push_back(to_path(mod.name, ".o"));

        // Add module mapping for the main.cpp so it can find 'import rio'
        for (const auto &mod : build_order) link_cmd.push_back("-fmodule-file=" + mod.name + "=" + to_path(mod.name, ".pcm"));

        // Add system libraries (like io_uring)
        link_cmd.push_back("-luring");

        bld::Command cmd;
        cmd.parts = link_cmd;

        if (!bld::execute(cmd))
            return 1;
    }
    bld::log(bld::Log_type::INFO, "Build complete. compile_commands.json generated.");

    return 0;
}
