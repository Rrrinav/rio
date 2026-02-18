module;

#include <cerrno>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>

export module rio:io.file;

import std;
import :io;
import :file;
import :utils;
import :handle;

namespace rio::io {

export auto read_file(const char *path) -> result<std::string>
{
    auto f_res = rio::file::open(path, rio::f_mode::read_only);
    if (!f_res)
        return std::unexpected(f_res.error());

    // Uses generic read_till_eof on the rio::file object
    return read_till_eof(*f_res);
}

export auto write_file(const char *path, std::span<const char> data) -> result<void>
{
    auto f_res = rio::file::open(path, rio::f_mode::write_new);
    if (!f_res)
        return std::unexpected(f_res.error());

    return write_all(*f_res, data);
}

export auto append_file(const char *path, std::span<const char> data) -> result<void>
{
    auto f_res = rio::file::open(path, rio::f_mode::write_app);
    if (!f_res)
        return std::unexpected(f_res.error());

    return write_all(*f_res, data);
}

export auto file_size(const rio::file &f) -> result<size_t>
{
    struct stat st;
    if (::fstat(f.fd.native_handle(), &st) < 0)
        return std::unexpected(rio::Err::sys("fstat failed"));

    return static_cast<size_t>(st.st_size);
}

// Raw send_file wrapper (int, int)
export auto send_file(int out_fd, int in_fd, size_t count, off_t *offset = nullptr) -> result<size_t>
{
    while (true) {
        ssize_t n = ::sendfile(out_fd, in_fd, offset, count);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return std::unexpected(rio::Err::app(std::errc::operation_would_block, "sendfile blocked"));

            return std::unexpected(rio::Err::sys("sendfile failed"));
        }

        return static_cast<size_t>(n);
    }
}

export template <typename Out_handle>
auto send_file(Out_handle &out, const rio::file &in, size_t count) -> result<size_t>
{
    return send_file(out.fd.native_handle(), in.fd.native_handle(), count, nullptr);
}

export auto read_all(const rio::file &f) -> result<std::string>
{
    struct stat st;
    if (::fstat(f.fd.native_handle(), &st) < 0)
        return std::unexpected(rio::Err::sys("fstat failed"));

    std::string out;
    try {
        out.resize(st.st_size);
    } catch (...) {
        return std::unexpected(rio::Err{ ENOMEM, "Failed to allocate file buffer" });
    }

    if (auto res = rio::io::read_till_full(f.fd.native_handle(), std::span{ out }); !res)
        return std::unexpected(res.error());

    return out;
}

export auto read_all(const rio::file &f, std::string &buff) -> result<void>
{
    if (auto res = read_all(f); !res)
        return std::unexpected(res.error());
    else
        buff = res.value();

    return {};
}

} // namespace rio::io
