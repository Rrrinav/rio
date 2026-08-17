module;

#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

export module rio:socket.udp_socket;
import :socket.address;
import :handle;
import :utils;

import std;

namespace rio {

export enum class udp_opt : uint32_t {
    none = 0,
    v4 = 1 << 0,
    v6 = 1 << 1,
    dualstack = 1 << 2,
    nonblock = 1 << 3,
    cloexec = 1 << 4,
    reuse = 1 << 5,
    broadcast = 1 << 6
};

export constexpr udp_opt operator|(udp_opt a, udp_opt b)
{
    return static_cast<udp_opt>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
export constexpr udp_opt operator&(udp_opt a, udp_opt b)
{
    return static_cast<udp_opt>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
constexpr bool has(udp_opt subject, udp_opt flag)
{
    return (static_cast<uint32_t>(subject) & static_cast<uint32_t>(flag)) == static_cast<uint32_t>(flag);
}

export struct Udp_socket
{
    rio::handle fd{};

    Udp_socket() = default;
    explicit Udp_socket(rio::handle &&h) noexcept : fd(std::move(h))
    {}

    static auto open(udp_opt options = udp_opt::v4) -> result<Udp_socket>;
    static auto open_and_bind(const rio::address &addr, udp_opt options = udp_opt::v4) -> result<Udp_socket>;
    static auto open_and_bind(const char *ip, uint16_t port, udp_opt options = udp_opt::v4) -> result<std::tuple<Udp_socket, rio::address>>;

    static auto attach(int raw_fd) -> Udp_socket;
    explicit operator bool() const;

    auto local_endpoint() const -> result<rio::address>;

    template <typename T>
    auto set_option(int level, int optname, T value) -> Udp_socket &
    {
        ::setsockopt(fd.native_handle(), level, optname, &value, sizeof(value));
        return *this;
    }

    auto set_broadcast(bool enable) -> Udp_socket &
    {
        int val = enable ? 1 : 0;
        set_option(SOL_SOCKET, SO_BROADCAST, val);
        return *this;
    }

    auto set_reuse_address(bool enable) -> Udp_socket &
    {
        int val = enable ? 1 : 0;
        set_option(SOL_SOCKET, SO_REUSEADDR, val);
        set_option(SOL_SOCKET, SO_REUSEPORT, val);
        return *this;
    }
};

auto Udp_socket::attach(int raw_fd) -> Udp_socket
{
    return Udp_socket{rio::handle(raw_fd)};
}

Udp_socket::operator bool() const
{
    return static_cast<bool>(fd);
}

export auto bind(Udp_socket &s, const address &addr) -> result<void>
{
    if (::bind(s.fd.native_handle(), addr.data(), addr.size()) == -1) [[unlikely]] {
        return std::unexpected(Err{errno, "Failed to bind UDP socket"});
    }
    return {};
}

export auto connect(Udp_socket &s, const address &addr) -> result<void>
{
    if (::connect(s.fd.native_handle(), addr.data(), addr.size()) == -1) [[unlikely]] {
        return std::unexpected(Err{errno, "Failed to connect UDP socket"});
    }
    return {};
}

auto Udp_socket::open(udp_opt options) -> result<Udp_socket>
{
    if (has(options, udp_opt::v4) && has(options, udp_opt::v6)) {
        return std::unexpected(Err{EINVAL, "Cannot specify both IPv4 and IPv6"});
    }

    if (has(options, udp_opt::dualstack) && !has(options, udp_opt::v6)) {
        return std::unexpected(Err{EINVAL, "Dualstack requires IPv6"});
    }

    const int domain = (has(options, udp_opt::v6) || has(options, udp_opt::dualstack)) ? AF_INET6 : AF_INET;

    int type = SOCK_DGRAM;
    if (has(options, udp_opt::cloexec)) {
        type |= SOCK_CLOEXEC;
    }
    if (has(options, udp_opt::nonblock)) {
        type |= SOCK_NONBLOCK;
    }

    const int s = ::socket(domain, type, 0);
    if (s == -1) {
        return std::unexpected(Err{errno, "Failed to create UDP socket"});
    }

    auto set_sockopt = [s](int level, int optname, const int &value, const char *opt_name) -> result<void> {
        if (::setsockopt(s, level, optname, &value, sizeof(value)) == -1) {
            ::close(s);
            return std::unexpected(rio::Err::sys(std::format("Failed to set {}", opt_name)));
        }
        return {};
    };

    constexpr int on = 1;
    constexpr int off = 0;

    if (has(options, udp_opt::dualstack)) {
        if (auto res = set_sockopt(IPPROTO_IPV6, IPV6_V6ONLY, off, "IPV6_V6ONLY"); !res) {
            return std::unexpected(res.error());
        }
    }

    if (has(options, udp_opt::reuse)) {
        if (auto res = set_sockopt(SOL_SOCKET, SO_REUSEADDR, on, "SO_REUSEADDR"); !res) {
            return std::unexpected(res.error());
        }
        if (auto res = set_sockopt(SOL_SOCKET, SO_REUSEPORT, on, "SO_REUSEPORT"); !res) {
            return std::unexpected(res.error());
        }
    }

    if (has(options, udp_opt::broadcast)) {
        if (auto res = set_sockopt(SOL_SOCKET, SO_BROADCAST, on, "SO_BROADCAST"); !res) {
            return std::unexpected(res.error());
        }
    }

    return Udp_socket::attach(s);
}

auto Udp_socket::open_and_bind(const rio::address &addr, udp_opt options) -> result<Udp_socket>
{
    auto o_res = open(options);
    if (!o_res) [[unlikely]] {
        return std::unexpected(o_res.error());
    }

    if (auto res = bind(*o_res, addr); !res) [[unlikely]] {
        return std::unexpected(res.error());
    }

    return o_res;
}

auto Udp_socket::open_and_bind(const char *ip, uint16_t port, udp_opt options) -> result<std::tuple<Udp_socket, rio::address>>
{
    auto addr = rio::address::from_ip(ip, port);
    if (!addr) [[unlikely]] {
        return std::unexpected(addr.error());
    }

    if (has(options, udp_opt::v6) && addr->family() == AF_INET) {
        return std::unexpected(Err{EINVAL, "IPv6 socket cannot bind IPv4 address"});
    }
    if (has(options, udp_opt::v4) && addr->family() == AF_INET6) {
        return std::unexpected(Err{EINVAL, "IPv4 socket cannot bind IPv6 address"});
    }

    auto o_res = open_and_bind(*addr, options);
    if (!o_res) [[unlikely]] {
        return std::unexpected(o_res.error());
    }

    return std::make_tuple(std::move(*o_res), std::move(*addr));
}

auto Udp_socket::local_endpoint() const -> result<rio::address>
{
    rio::address addr;
    addr.len = sizeof(addr.storage);
    if (::getsockname(fd.native_handle(), &addr.storage.general, &addr.len) == -1) {
        return std::unexpected(Err{errno, "getsockname failed"});
    }
    return addr;
}

} // namespace rio
