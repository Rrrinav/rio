// This was written by using this file only.
import rio;
import std;

#include <unistd.h>

int main()
{
    std::string path = std::source_location::current().file_name();

    rio::handle out(STDOUT_FILENO);
    rio::handle err(STDERR_FILENO);

    std::string str;

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

