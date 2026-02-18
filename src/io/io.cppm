module;

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

export module rio:io;

import std;
import :utils;

namespace rio::io {

export template <typename T>
concept Handle_like_c = requires(T t) {
    { t.fd.native_handle() } -> std::convertible_to<int>;
};

constexpr inline int get_fd(const Handle_like_c auto &h)
{
    return h.fd.native_handle();
}
constexpr inline int get_fd(int fd)
{
    return fd;
}

export auto read(int fd, std::span<char> buf) -> result<size_t>
{
    while (true) {
        ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return std::unexpected(rio::Err::sys("read failed"));
        }
        return static_cast<size_t>(n);
    }
}

export auto read(const Handle_like_c auto &h, std::span<char> buf)
{
    return read(get_fd(h), buf);
}

export auto write(int fd, std::span<const char> buf) -> result<size_t>
{
    while (true) {
        ssize_t n = ::write(fd, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return std::unexpected(rio::Err::sys("write failed"));
        }
        return static_cast<size_t>(n);
    }
}

export auto write(const Handle_like_c auto &h, std::span<const char> buf)
{
    return write(get_fd(h), buf);
}

export auto read_till_full(int fd, std::span<char> buf) -> result<void>
{
    size_t total = 0;
    while (total < buf.size()) {
        auto res = read(fd, buf.subspan(total));
        if (!res)
            return std::unexpected(res.error());

        size_t n = *res;
        if (n == 0)
            return std::unexpected(rio::Err::app(std::errc::broken_pipe, "Unexpected EOF in read_till_full"));

        total += n;
    }
    return {};
}

export auto read_till_full(const Handle_like_c auto &h, std::span<char> buf)
{
    return read_till_full(get_fd(h), buf);
}

export auto write_all(int fd, std::span<const char> buf) -> result<void>
{
    size_t total = 0;
    while (total < buf.size()) {
        auto res = write(fd, buf.subspan(total));
        if (!res)
            return std::unexpected(res.error());

        total += *res;
    }
    return {};
}

export auto write_all(const Handle_like_c auto &h, std::span<const char> buf)
{
    return write_all(get_fd(h), buf);
}

export auto read_till_eof(int fd) -> result<std::string>
{
    std::string out;
    out.reserve(4096);
    char buf[4096];

    while (true) {
        auto res = read(fd, std::span{buf});
        if (!res)
            return std::unexpected(res.error());

        size_t n = *res;
        if (n == 0)
            break; // EOF

        out.append(buf, n);
    }
    return out;
}

export auto read_till_eof(const Handle_like_c auto &h)
{
    return read_till_eof(get_fd(h));
}

// very inefficient
export auto read_till(int fd, char delim) -> result<std::string>
{
    std::string out;
    out.reserve(128);
    char c;

    while (true) {
        // Very inefficient (1 byte syscalls), but safe for blocking mixed streams.
        auto res = read(fd, std::span{&c, 1});
        if (!res)
            return std::unexpected(res.error());

        size_t n = *res;
        if (n == 0)
            break; // EOF

        if (c == delim)
            break;
        out.push_back(c);
    }
    return out;
}

// very inefficient
export auto read_till(const Handle_like_c auto &h, char delim)
{
    return read_till(get_fd(h), delim);
}
// very inefficient
export auto read_line(const Handle_like_c auto &h)
{
    return read_till(get_fd(h), '\n');
}

} // namespace rio::io
