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
        rio::file is an abstraction over rio::handle(01-basic-io.cpp) that acts as a file handle

        methods:
            1. static auto open(const char *path, f_mode flags = f_mode::read_only) -> result<file>;
                // Opens using permission: 0644 (-rw-r--r--)
            2. static auto attach(int raw_fd) -> file;
                // basically file{rio::handle(raw_fd)}
            3. auto detatch() -> int;
                // returns fd and sets fd to -1;
            4. explicit operator bool() const;
                implicit bool conversion: returns fd < 0
    */
    {
        rio::file F;
        if (auto res = rio::file::open(path.c_str()); !res)
        {
            rio::io::write(err, std::format("{}\n", res.error()));
            return 1;
        }
        else
        {
            F = std::move(res.value());
        }

        if (auto res = rio::io::read_str(F, str); !res)
        {
            rio::io::write(err, std::format("{}\n", res.error()));
            return 1;
        }

        rio::io::write(out, str);
    }
    /*
        Functions on file
            export auto read(const rio::file &f, std::span<char> buf) -> result<std::size_t>
            export auto read_str(const rio::file &f, std::string& str) -> result<void>
            export auto write(const rio::file &f, std::span<const char> data) -> result<std::size_t>

        You can handle functions too just by using file.fd.
     */
    {
        const std::string header = "// This was written by using this file only.\n";

        if (str.starts_with(header))
            str.erase(0, header.size());

        str.insert(0, header);

        // Temp file path
        std::string temp_path = path + ".tmp";

        // Create temp file
        rio::file tmp;
        if (auto res = rio::file::open(temp_path.c_str(), rio::f_mode::write | rio::f_mode::create | rio::f_mode::truncate); !res)
        {
            rio::io::write(err, std::format("{}\n", res.error()));
            return 1;
        }
        else
        {
            tmp = std::move(res.value());
        }

        if (auto res = rio::io::write(tmp, str); !res)
        {
            rio::io::write(err, std::format("Write error: {}\n", res.error()));
            std::filesystem::remove(temp_path);
            return 1;
        }

        ::fsync(tmp.fd);

        try
        {
            std::filesystem::rename(temp_path, path);
        }
        catch (const std::exception& e)
        {
            rio::io::write(err, std::format("Rename failed: {}\n", e.what()));
            std::filesystem::remove(temp_path);
            return 1;
        }

        auto dir = std::filesystem::path(path).parent_path();
        if (dir.empty())
            dir = ".";

        if (auto res = rio::file::open(dir.c_str()); res)
        {
            ::fsync(res.value().fd);
        }

        std::filesystem::remove(temp_path);
    }

    return 0;
}
