module;

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <cerrno>

export module rio:io;

import std;
import :utils;

namespace rio::io {

export template <typename T>
concept HandleLike = requires(T t) {
    { t.fd.native_handle() } -> std::convertible_to<int>;
};

constexpr int get_fd(const HandleLike auto &h) { return h.fd.native_handle(); }
constexpr int get_fd(int fd) { return fd; }

export auto read(int fd, std::span<char> buf) -> result<size_t>
{
    while (true)
    {
        ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return std::unexpected(rio::Err::sys("read failed"));
        }
        return static_cast<size_t>(n);
    }
}

export auto read(const HandleLike auto &h, std::span<char> buf) { return read(get_fd(h), buf); }

export auto write(int fd, std::span<const char> buf) -> result<size_t>
{
    while (true)
    {
        ssize_t n = ::write(fd, buf.data(), buf.size());
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return std::unexpected(rio::Err::sys("write failed"));
        }
        return static_cast<size_t>(n);
    }
}

export auto write(const HandleLike auto &h, std::span<const char> buf) { return write(get_fd(h), buf); }

export auto try_read(int fd, std::span<char> buf) -> result<size_t>
{
    while (true)
    {
        ssize_t n = ::recv(fd, buf.data(), buf.size(), MSG_DONTWAIT);

        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return std::unexpected(rio::Err::app(std::errc::operation_would_block, "try_read: would block"));

            // Fallback for files
            if (errno == ENOTSOCK)
                return std::unexpected(rio::Err::app(std::errc::function_not_supported, "try_read only works on sockets"));

            return std::unexpected(rio::Err::sys("try_read failed"));
        }
        return static_cast<size_t>(n);
    }
}

export auto try_read(const HandleLike auto &h, std::span<char> buf) { return try_read(get_fd(h), buf); }

export auto try_write(int fd, std::span<const char> buf) -> result<size_t>
{
    while (true)
    {
        // MSG_NOSIGNAL prevents SIGPIPE on disconnected sockets
        ssize_t n = ::send(fd, buf.data(), buf.size(), MSG_DONTWAIT | MSG_NOSIGNAL);

        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return std::unexpected(rio::Err::app(std::errc::operation_would_block, "try_write: would block"));

            if (errno == ENOTSOCK)
                return std::unexpected(rio::Err::app(std::errc::function_not_supported, "try_write only works on sockets"));

            return std::unexpected(rio::Err::sys("try_write failed"));
        }
        return static_cast<size_t>(n);
    }
}

export auto try_write(const HandleLike auto &h, std::span<const char> buf) { return try_write(get_fd(h), buf); }

export auto read_exactly(int fd, std::span<char> buf) -> result<void>
{
    size_t total = 0;
    while (total < buf.size())
    {
        auto res = read(fd, buf.subspan(total));
        if (!res)
            return std::unexpected(res.error());

        size_t n = *res;
        if (n == 0)
            return std::unexpected(rio::Err::app(std::errc::broken_pipe, "Unexpected EOF in read_exactly"));

        total += n;
    }
    return {};
}

export auto read_exactly(const HandleLike auto &h, std::span<char> buf) { return read_exactly(get_fd(h), buf); }

export auto write_all(int fd, std::span<const char> buf) -> result<void>
{
    size_t total = 0;
    while (total < buf.size())
    {
        auto res = write(fd, buf.subspan(total));
        if (!res)
            return std::unexpected(res.error());

        total += *res;
    }
    return {};
}

export auto write_all(const HandleLike auto &h, std::span<const char> buf) { return write_all(get_fd(h), buf); }

export auto read_till_eof(int fd) -> result<std::string>
{
    std::string out;
    out.reserve(4096);
    char buf[4096];

    while (true)
    {
        auto res = read(fd, std::span{buf});
        if (!res)
            return std::unexpected(res.error());

        size_t n = *res;
        if (n == 0)
            break;  // EOF

        out.append(buf, n);
    }
    return out;
}

export auto read_till_eof(const HandleLike auto &h) { return read_till_eof(get_fd(h)); }

// very inefficient
export auto read_till(int fd, char delim) -> result<std::string>
{
    std::string out;
    out.reserve(128);
    char c;

    while (true)
    {
        // Very inefficient (1 byte syscalls), but safe for blocking mixed streams.
        auto res = read(fd, std::span{&c, 1});
        if (!res)
            return std::unexpected(res.error());

        size_t n = *res;
        if (n == 0)
            break;  // EOF

        if (c == delim)
            break;
        out.push_back(c);
    }
    return out;
}

// very inefficient
export auto read_till(const HandleLike auto &h, char delim) { return read_till(get_fd(h), delim); }

// very inefficient
export auto read_line(const HandleLike auto &h) { return read_till(get_fd(h), '\n'); }

}  // namespace rio::io
