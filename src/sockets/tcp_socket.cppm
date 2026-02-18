module;

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

export module rio:socket.tcp_socket;
import :socket.address;
import :handle;
import :utils;

import std;

namespace rio {

export enum class s_opt : uint32_t {
    none = 0,
    v4 = 1 << 0,
    v6 = 1 << 1,
    dualstack = 1 << 2,
    nonblock = 1 << 3,
    cloexec = 1 << 4,
    reuse = 1 << 5,   // REUSEADDR + REUSEPORT
    nodelay = 1 << 6, // TCP_NODELAY
    keepalive = 1 << 7,

    // IPv4 Only
    sync_server_v4 = v4 | cloexec | reuse | keepalive,
    async_server_v4 = sync_server_v4 | nonblock | nodelay,

    // IPv6 Only (Strict)
    sync_server_v6 = v6 | cloexec | reuse | keepalive,
    async_server_v6 = sync_server_v6 | nonblock | nodelay,

    // Universal (IPv6 + IPv4 Dual-stack)
    sync_server = v6 | dualstack | cloexec | reuse | keepalive,
    async_server = sync_server | nonblock | nodelay,

    // Outbound Clients
    client_v4 = v4 | cloexec | nodelay,
    client_v6 = v6 | cloexec | nodelay,
    client = client_v4
};

// Bitwise operators
export constexpr s_opt operator|(s_opt a, s_opt b)
{
    return static_cast<s_opt>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
export constexpr s_opt operator&(s_opt a, s_opt b)
{
    return static_cast<s_opt>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
constexpr bool has(s_opt subject, s_opt flag)
{
    return (static_cast<uint32_t>(subject) & static_cast<uint32_t>(flag)) == static_cast<uint32_t>(flag);
}

export struct Tcp_socket
{
    rio::handle fd{};

    Tcp_socket() = default;
    explicit Tcp_socket(rio::handle &&h) noexcept : fd(std::move(h))
    {}

    static auto open(s_opt options) -> result<Tcp_socket>;
    static auto open(const rio::address &address, s_opt options) -> result<Tcp_socket>;
    static auto open(const char *ip, uint16_t port, s_opt options) -> result<std::tuple<Tcp_socket, rio::address>>;

    static auto open_and_listen(const rio::address &address, s_opt options = s_opt::sync_server_v4, int backlog = 128)
        -> result<Tcp_socket>;
    static auto open_and_listen(const char *ip, uint16_t port, s_opt options = s_opt::sync_server_v4, int backlog = 128)
        -> result<std::tuple<Tcp_socket, rio::address>>;

    static auto attach(int raw_fd) -> Tcp_socket;
    explicit operator bool() const;

    enum class shut : int { read = SHUT_RD, write = SHUT_WR, both = SHUT_RDWR };

    auto shutdown(shut how = shut::write) -> result<void>
    {
        if (::shutdown(fd.native_handle(), static_cast<int>(how)) == -1)
            return std::unexpected(Err{errno, "Socket shutdown failed"});
        return {};
    }

    auto local_endpoint() const -> result<rio::address>
    {
        rio::address addr;
        addr.len = sizeof(addr.storage);
        if (::getsockname(fd.native_handle(), &addr.storage.general, &addr.len) == -1)
            return std::unexpected(Err{errno, "getsockname failed"});
        return addr;
    }

    auto remote_endpoint() const -> result<rio::address>
    {
        rio::address addr;
        addr.len = sizeof(addr.storage);
        if (::getpeername(fd.native_handle(), &addr.storage.general, &addr.len) == -1)
            return std::unexpected(Err{errno, "getpeername failed"});
        return addr;
    }

    template <typename T>
    auto set_option(int level, int optname, T value) -> result<void>
    {
        if (::setsockopt(fd.native_handle(), level, optname, &value, sizeof(value)) == -1)
            return std::unexpected(Err{errno, "setsockopt failed"});
        return {};
    }

    auto set_reuse_address(bool enable) -> result<void>
    {
        int val = enable ? 1 : 0;
        if (auto r = set_option(SOL_SOCKET, SO_REUSEADDR, val); !r)
            return r;
        return set_option(SOL_SOCKET, SO_REUSEPORT, val);
    }

    auto set_nodelay(bool enable) -> result<void>
    {
        return set_option(IPPROTO_TCP, TCP_NODELAY, enable ? 1 : 0);
    }
    auto set_keepalive(bool enable) -> result<void>
    {
        return set_option(SOL_SOCKET, SO_KEEPALIVE, enable ? 1 : 0);
    }

    auto set_policy(bool blocking) -> result<void>
    {
        int flags = ::fcntl(fd.native_handle(), F_GETFL, 0);
        if (flags == -1)
            return std::unexpected(Err{errno, "fcntl(F_GETFL) failed"});

        if (blocking)
            flags &= ~O_NONBLOCK;
        else
            flags |= O_NONBLOCK;

        if (::fcntl(fd.native_handle(), F_SETFL, flags) == -1)
            return std::unexpected(Err{errno, "fcntl(F_SETFL) failed"});
        return {};
    }

    auto set_blocking() -> result<void>
    {
        return set_policy(true);
    }
    auto set_nonblocking() -> result<void>
    {
        return set_policy(false);
    }

    template <typename T>
    auto get_option(int level, int optname, T &value) const -> result<void>
    {
        socklen_t len = sizeof(T);
        if (::getsockopt(fd.native_handle(), level, optname, &value, &len) == -1)
            return std::unexpected(Err{errno, "getsockopt failed"});
        return {};
    }
};

auto Tcp_socket::attach(int raw_fd) -> Tcp_socket
{
    return Tcp_socket{rio::handle(raw_fd)};
}
Tcp_socket::operator bool() const
{
    return static_cast<bool>(fd);
}

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

auto Tcp_socket::open(s_opt options) -> result<Tcp_socket>
{
    if (has(options, s_opt::v4) && has(options, s_opt::v6))
        return std::unexpected(Err{EINVAL, "Cannot specify both IPv4 and IPv6"});

    if (has(options, s_opt::dualstack) && !has(options, s_opt::v6))
        return std::unexpected(Err{EINVAL, "Dualstack requires IPv6"});

    const int domain = (has(options, s_opt::v6) || has(options, s_opt::dualstack)) ? AF_INET6 : AF_INET;

    int type = SOCK_STREAM;
    if (has(options, s_opt::cloexec))
        type |= SOCK_CLOEXEC;
    if (has(options, s_opt::nonblock))
        type |= SOCK_NONBLOCK;

    const int s = ::socket(domain, type, 0);
    if (s == -1)
        return std::unexpected(Err{errno, "Failed to create TCP socket"});

    auto set_sockopt = [s](int level, int optname, const int &value, const char *opt_name) -> result<void> {
        if (::setsockopt(s, level, optname, &value, sizeof(value)) == -1) {
            ::close(s);
            return std::unexpected(rio::Err::sys(std::format("Failed to set {}", opt_name)));
        }
        return {};
    };

    constexpr int on = 1;
    constexpr int off = 0;

    if (has(options, s_opt::dualstack))
        if (auto res = set_sockopt(IPPROTO_IPV6, IPV6_V6ONLY, off, "IPV6_V6ONLY"); !res)
            return std::unexpected(res.error());

    if (has(options, s_opt::reuse)) {
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

    if (has(options, s_opt::v6) && addr->family() == AF_INET)
        return std::unexpected(Err{EINVAL, "IPv6 socket cannot bind IPv4 address"});
    if (has(options, s_opt::v4) && addr->family() == AF_INET6)
        return std::unexpected(Err{EINVAL, "IPv4 socket cannot bind IPv6 address"});

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

} // namespace rio
