module;

#include <cerrno>
#include <unistd.h>
#include <sys/socket.h>

export module rio:io;

import std;
import std.compat;

import :file;
import :socket.tcp_socket;
import :utils;

namespace rio::io {

export template<typename T>
concept HasHandle = requires(T t) {
    { t.fd } -> std::same_as<rio::handle&>;
};

export auto read(const rio::file &f, std::span<char> buf) -> result<std::size_t>
{
    if (!f)
        return std::unexpected(Err{EBADF, "File not open"});

    int n = ::read(f.fd, buf.data(), buf.size());

    if (n == -1)
    {
        if (errno == EINTR)
            return read(f, buf);
        return std::unexpected(Err{errno, "File read failed"});
    }
    return static_cast<std::size_t>(n);
}

export auto write(const rio::file &f, std::span<const char> data) -> result<std::size_t>
{
    if (!f)
        return std::unexpected(Err{EBADF, "File not open"});

    int n = ::write(f.fd, data.data(), data.size());
    if (n == -1)
    {
        if (errno == EINTR)
            return write(f, data);
        return std::unexpected(Err{errno, "File write failed"});
    }
    return static_cast<std::size_t>(n);
}

// --- Socket I/O (Streaming/Dynamic) ---

export auto read(const rio::Tcp_socket &s, std::span<char> buf) -> std::size_t
{
    // We use recv for sockets. In sync mode, this returns as soon as
    // any data is available, solving your "wait for disconnect" issue.
    int n = ::recv(s.fd, buf.data(), buf.size(), 0);

    if (n == -1)
    {
        if (EINTR)
            return read(s, buf);
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        else
            return -1;
    }
    return static_cast<std::size_t>(n);
}

export auto write(const rio::Tcp_socket &s, std::span<const char> data) -> std::size_t
{
    // MSG_NOSIGNAL is the critical "Pro" default to prevent SIGPIPE
    int n = ::send(s.fd, data.data(), data.size(), MSG_NOSIGNAL);

    if (n == -1)
    {
        if (EINTR)
            return write(s, data);
        return -1;
    }
    return static_cast<std::size_t>(n);
}

export auto write_all(const HasHandle auto &resource, std::span<const char> data) -> bool
{
    std::size_t total = 0;
    while (total < data.size())
    {
        auto res = write(resource, data.subspan(total));
        if (res < 0)
            return false;
        total += res;
    }
    return {};
}

export auto read_all(const HasHandle auto &resource, std::string &out) -> result<std::size_t>
{
    if (!resource)
        return std::unexpected(Err{EBADF, "Resource closed"});

    std::size_t total_read = 0;
    char chunk[4096];  // 4KB is the standard page size for Linux I/O

    while (true)
    {
        // Use our existing 'read' overload (handles EINTR/retries)
        auto res = read(resource, std::span{chunk});

        if (!res)
            return std::unexpected(res.error());

        std::size_t n = *res;
        if (n == 0)
            break;  // EOF reached

        out.append(chunk, n);
        total_read += n;
    }

    return total_read;
}
}  // namespace rio::io
