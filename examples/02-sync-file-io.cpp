import rio;
import std;

#include <unistd.h>

int main()
{
    std::string path = std::source_location::current().file_name();

    rio::handle out(STDOUT_FILENO);
    rio::handle err(STDERR_FILENO);

    std::string str;

    /*
        rio::file<S> is an abstraction over rio::handle(01-basic-io.cpp) that acts as a file handle.
        The open/closed distinction lives in the type: only file<file_state::open>
        can be read, written or stat'ed; close() consumes the handle and hands
        back a file<file_state::closed> token. Use-after-close is a compile error.

        methods (on file<file_state::open>):
            1. static auto open(const char *path, f_mode flags = f_mode::read_only) -> result<file<file_state::open>>;
                // Opens using permission: 0644 (-rw-r--r--)
            2. static auto attach(int raw_fd) -> file<file_state::open>;
                // basically file{rio::handle(raw_fd)}
            3. auto detatch() && -> int;
                // returns fd and disarms the destructor. Consumes the handle.
            4. auto close() && -> file<file_state::closed>;
                // closes the fd and hands back an empty token. Consumes the handle.
    */
    {
        auto F_res = rio::file<rio::file_state::open>::open(path.c_str());
        if (!F_res) {
            rio::io::write(err, std::format("{}\n", F_res.error()));
            return 1;
        }
        auto F = std::move(*F_res);

        if (auto res = rio::io::read_all(F, str); !res) {
            rio::io::write(err, std::format("{}\n", res.error()));
            return 1;
        }

        rio::io::write(out, str);
    }
    /*
        Functions on open files
            export auto read(const file<file_state::open> &f, std::span<char> buf) -> result<std::size_t>
            export auto write(const file<file_state::open> &f, std::span<const char> data) -> result<std::size_t>

        You can still reach the raw fd via f.native_handle().
     */
    {
        const std::string header = "// This was written by using this file only.\n";

        if (str.starts_with(header))
            str.erase(0, header.size());

        str.insert(0, header);

        // Temp file path
        std::string temp_path = path + ".tmp";

        // Create temp file
        auto tmp_res = rio::file<rio::file_state::open>::open(temp_path.c_str(),
            rio::f_mode::write | rio::f_mode::create | rio::f_mode::truncate);
        if (!tmp_res) {
            rio::io::write(err, std::format("{}\n", tmp_res.error()));
            return 1;
        }
        auto tmp = std::move(*tmp_res);

        if (auto res = rio::io::write(tmp, str); !res) {
            rio::io::write(err, std::format("Write error: {}\n", res.error()));
            std::filesystem::remove(temp_path);
            return 1;
        }

        ::fsync(tmp.native_handle());

        try {
            std::filesystem::rename(temp_path, path);
        } catch (const std::exception &e) {
            rio::io::write(err, std::format("Rename failed: {}\n", e.what()));
            std::filesystem::remove(temp_path);
            return 1;
        }

        auto dir = std::filesystem::path(path).parent_path();
        if (dir.empty())
            dir = ".";

        if (auto res = rio::file<rio::file_state::open>::open(dir.c_str()); res) {
            ::fsync(res->native_handle());
        }

        std::filesystem::remove(temp_path);
    }

    return 0;
}
