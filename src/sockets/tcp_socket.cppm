module;

#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>

export module rio:socket.tcp_socket;
import :socket.address;
import :handle;
import :utils;

import std;

namespace rio {

export enum class s_opt : uint32_t {
    none            = 0,
    v4              = 1 << 0,
    v6              = 1 << 1,
    dualstack       = 1 << 2,
    nonblock        = 1 << 3,
    cloexec         = 1 << 4,
    reuse           = 1 << 5,    // REUSEADDR + REUSEPORT
    nodelay         = 1 << 6,  // TCP_NODELAY
    keepalive       = 1 << 7,

    // IPv4 Only
    sync_server_v4  = v4 | cloexec | reuse | keepalive,
    async_server_v4 = sync_server_v4 | nonblock | nodelay,

    // IPv6 Only (Strict)
    sync_server_v6  = v6 | cloexec | reuse | keepalive,
    async_server_v6 = sync_server_v6 | nonblock | nodelay,

    // Universal (IPv6 + IPv4 Dual-stack)
    sync_server     = v6 | dualstack | cloexec | reuse | keepalive,
    async_server    = sync_server | nonblock | nodelay,

    // Outbound Clients
    client_v4       = v4 | cloexec | nodelay,
    client_v6       = v6 | cloexec | nodelay,
    client          = client_v4
};

// Bitwise operators
export constexpr s_opt operator|(s_opt a, s_opt b) { return static_cast<s_opt>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); }

export constexpr s_opt operator&(s_opt a, s_opt b) { return static_cast<s_opt>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b)); }

constexpr bool has(s_opt subject, s_opt flag)
{
    return (static_cast<uint32_t>(subject) & static_cast<uint32_t>(flag)) == static_cast<uint32_t>(flag);
}

export struct Tcp_socket
{
    rio::handle fd{};

    Tcp_socket() = default;
    explicit Tcp_socket(rio::handle &&h) : fd(std::move(h)) {}

    static auto open(s_opt options) -> result<Tcp_socket>;
    static auto open(const rio::address &address, s_opt options) -> result<Tcp_socket>;
    static auto open(const char *ip, uint16_t port, s_opt options) -> result<std::tuple<Tcp_socket, rio::address>>;

    static auto open_and_listen(const rio::address &address, s_opt options = s_opt::sync_server_v4, int backlog = 128) -> result<Tcp_socket>;
    static auto open_and_listen(const char *ip, uint16_t port, s_opt options= s_opt::sync_server_v4, int backlog = 128) -> result<std::tuple<Tcp_socket, rio::address>>;

    static auto attach(int raw_fd) -> Tcp_socket;
    explicit operator bool() const;
};

auto Tcp_socket::open(s_opt options) -> result<Tcp_socket>
{
    // Validate conflicting options
    if (has(options, s_opt::v4) && has(options, s_opt::v6))
        return std::unexpected(Err{EINVAL, "Cannot specify both IPv4 and IPv6"});

    if (has(options, s_opt::dualstack) && !has(options, s_opt::v6))
        return std::unexpected(Err{EINVAL, "Dualstack requires IPv6"});

    // Determine socket domain
    const int domain = (has(options, s_opt::v6) || has(options, s_opt::dualstack)) ? AF_INET6 : AF_INET;

    // Build socket type with flags
    int type = SOCK_STREAM;
    if (has(options, s_opt::cloexec))
        type |= SOCK_CLOEXEC;
    if (has(options, s_opt::nonblock))
        type |= SOCK_NONBLOCK;

    // Create socket
    const int s = ::socket(domain, type, 0);

    if (s == -1)
        return std::unexpected(Err{errno, "Failed to create TCP socket"});

    // Helper lambda for socket options with error handling
    auto set_sockopt = [s](int level, int optname, const int &value, const char *opt_name) -> result<void>
    {
        if (::setsockopt(s, level, optname, &value, sizeof(value)) == -1)
        {
            const int err = errno;
            ::close(s);
            return std::unexpected(Err{err, std::format("Failed to set {}", opt_name)});
        }
        return {};
    };

    constexpr int on = 1;
    constexpr int off = 0;

    // Apply socket options
    if (has(options, s_opt::dualstack))
        if (auto res = set_sockopt(IPPROTO_IPV6, IPV6_V6ONLY, off, "IPV6_V6ONLY"); !res)
            return std::unexpected(res.error());

    if (has(options, s_opt::reuse))
    {
        if (auto res = set_sockopt(SOL_SOCKET, SO_REUSEADDR, on, "SO_REUSEADDR"); !res)
            return std::unexpected(res.error());
        if (auto res = set_sockopt(SOL_SOCKET, SO_REUSEPORT, on, "SO_REUSEPORT"); !res)
            return std::unexpected(res.error());
    }

    if (has(options, s_opt::nodelay))
        if (auto res = set_sockopt(IPPROTO_TCP, TCP_NODELAY, on, "TCP_NODELAY"); !res)
            return std::unexpected(res.error());

    if (has(options, s_opt::keepalive))
        if (auto res = set_sockopt(SOL_SOCKET, SO_KEEPALIVE, on, "SO_KEEPALIVE"); !res)
            return std::unexpected(res.error());

    return Tcp_socket::attach(s);
}

auto Tcp_socket::attach(int raw_fd) -> Tcp_socket { return Tcp_socket{rio::handle(raw_fd)}; }

Tcp_socket::operator bool() const { return static_cast<bool>(fd); }

export auto bind(Tcp_socket &s, const address &addr) -> result<void>
{
    if (::bind(s.fd, &addr.storage.general, addr.len) == -1) [[unlikely]]
        return std::unexpected(Err{errno, "Failed to bind socket"});
    return {};
}

export auto listen(Tcp_socket &s, int backlog = 128) -> result<void>
{
    if (::listen(s.fd, backlog) == -1) [[unlikely]]
        return std::unexpected(Err{errno, "Failed to listen on socket"});
    return {};
}

export auto accept(Tcp_socket &s, s_opt options = s_opt::none) -> result<std::tuple<Tcp_socket, address>>
{
    const int flags = SOCK_CLOEXEC | (has(options, s_opt::nonblock) ? SOCK_NONBLOCK : 0);

    rio::address peer_addr;
    socklen_t len = sizeof(peer_addr.storage);
    const int fd = ::accept4(s.fd, &peer_addr.storage.general, &len, flags);

    if (fd == -1) [[unlikely]]
        return std::unexpected(Err{errno, "Failed to accept connection"});

    if (has(options, s_opt::nodelay)) [[unlikely]]
    {
        constexpr int on = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    }

    peer_addr.len = len;
    return std::make_tuple(Tcp_socket::attach(fd), peer_addr);
}

export auto accept_fast(Tcp_socket &s) -> result<Tcp_socket>
{
    constexpr int flags = SOCK_CLOEXEC | SOCK_NONBLOCK;

    const int fd = ::accept4(s.fd, nullptr, nullptr, flags);

    if (fd == -1) [[unlikely]]
        return std::unexpected(Err{errno, "Failed to accept connection"});

    constexpr int on = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

    return Tcp_socket::attach(fd);
}

// Batch accept: accept multiple connections in one call
#ifdef __linux__
export auto accept_many(Tcp_socket &s, std::span<std::tuple<Tcp_socket, address>> out, s_opt options = s_opt::none) -> result<size_t>
{
    const int flags = SOCK_CLOEXEC | (has(options, s_opt::nonblock) ? SOCK_NONBLOCK : 0);

    size_t accepted = 0;
    const bool apply_nodelay = has(options, s_opt::nodelay);
    constexpr int on = 1;

    for (auto &sock : out)
    {
        rio::address peer_addr;
        socklen_t len = sizeof(peer_addr.storage);
        const int fd = ::accept4(s.fd, &peer_addr.storage.general, &len, flags);

        if (fd == -1) [[unlikely]]
        {
            // EAGAIN/EWOULDBLOCK means no more connections
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;

            return std::unexpected(Err{errno, "Failed to accept connection"});
        }

        if (apply_nodelay) [[unlikely]]
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

        peer_addr.len = len;
        sock = std::make_tuple(Tcp_socket::attach(fd), peer_addr);
        ++accepted;
    }

    return accepted;
}
#endif

export auto accept_from(Tcp_socket &s, address &peer_addr, s_opt options = s_opt::none) -> result<Tcp_socket>
{
    const int flags = SOCK_CLOEXEC | (has(options, s_opt::nonblock) ? SOCK_NONBLOCK : 0);

    socklen_t len = sizeof(peer_addr.storage);
    const int fd = ::accept4(s.fd, &peer_addr.storage.general, &len, flags);

    if (fd == -1) [[unlikely]]
        return std::unexpected(Err{errno, "Failed to accept connection"});

    peer_addr.len = len;

    if (has(options, s_opt::nodelay)) [[unlikely]]
    {
        constexpr int on = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    }

    return Tcp_socket::attach(fd);
}

export template <class H>
concept accept_handler = std::invocable<H&, Tcp_socket, const address&> || std::invocable<H&, Tcp_socket&, const address&>;

export auto accept(Tcp_socket &s, accept_handler auto &&handler, s_opt options = s_opt::none) -> result<size_t>
{
    const int flags = SOCK_CLOEXEC | (has(options, s_opt::nonblock) ? SOCK_NONBLOCK : 0);

    size_t count = 0;
    const bool apply_nodelay = has(options, s_opt::nodelay);
    constexpr int on = 1;

    // Edge-triggered epoll fast path: accept until EAGAIN
    while (true)
    {
        rio::address peer_addr;
        socklen_t len = sizeof(peer_addr.storage);
        const int fd = ::accept4(s.fd, &peer_addr.storage.general, &len, flags);
        peer_addr.len = len;

        if (fd == -1) [[unlikely]]
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return count;  // Success: drained accept queue
            return std::unexpected(Err{errno, "Failed to accept connection"});
        }

        if (apply_nodelay) [[unlikely]]
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

        auto sock = Tcp_socket::attach(fd);

        if constexpr (std::invocable<decltype(handler)&, Tcp_socket&, const address&>)
            handler(sock, peer_addr);
        else
            handler(std::move(sock), peer_addr);

        ++count;
    }
}

auto Tcp_socket::open(const rio::address &address, s_opt options) -> result<Tcp_socket>
{
    auto o_res = open(options);

    if (!o_res) [[unlikely]]
        return std::unexpected(o_res.error());

    if (auto res = bind(*o_res, address); !res) [[unlikely]]
        return std::unexpected(res.error());

    return o_res;
}

auto Tcp_socket::open(const char *ip, uint16_t port, s_opt options) -> result<std::tuple<Tcp_socket, rio::address>>
{
    auto addr = rio::address::from_ip(ip, port);

    if (!addr) [[unlikely]]
        return std::unexpected(addr.error());

    auto o_res = open(*addr, options);

    if (!o_res) [[unlikely]]
        return std::unexpected(o_res.error());

    return std::make_tuple(std::move(*o_res), std::move(*addr));
}

auto Tcp_socket::open_and_listen(const rio::address &address, s_opt options, int backlog) -> result<Tcp_socket>
{
    auto o_res = open(address, options);

    if (!o_res) [[unlikely]]
        return std::unexpected(o_res.error());

    auto l_res = rio::listen(*o_res, backlog);

    if (!l_res) [[unlikely]]
        return std::unexpected(l_res.error());

    return o_res;
}

auto Tcp_socket::open_and_listen(const char *ip, uint16_t port, s_opt options, int backlog) -> result<std::tuple<Tcp_socket, rio::address>>
{
    auto o_res = open(ip, port, options);

    if (!o_res) [[unlikely]]
        return std::unexpected(o_res.error());

    auto &[sock, addr] = *o_res;

    auto l_res = rio::listen(sock, backlog);

    if (!l_res) [[unlikely]]
        return std::unexpected(l_res.error());

    return o_res;
}

}  // namespace rio
