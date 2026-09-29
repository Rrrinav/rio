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

// Typestate UDP socket: an unbound socket can send_to (POSIX allows it)
// but only a bound one can receive. Transitions consume (&&).
//
//   Udp_socket<closed>   — empty token; only close()/default produce one.
//   Udp_socket<unbound>  — fresh fd; bind()/connect() consume it.
//   Udp_socket<bound>    — bound (or connected); the only readable state.
//
// Same ownership and escape rules as Tcp_socket<>: move-only throughout,
// attach()/detatch() are the explicit raw-fd crossings.
export enum class udp_state { closed, unbound, bound };

export template <udp_state S>
struct Udp_socket;

export template <>
struct Udp_socket<udp_state::closed> {
    Udp_socket() = default;

    Udp_socket(const Udp_socket &) = delete;
    Udp_socket &operator=(const Udp_socket &) = delete;
    Udp_socket(Udp_socket &&) = default;
    Udp_socket &operator=(Udp_socket &&) = default;
};

namespace detail {
// Shared interior for the open states; state-specific operations live
// on the specializations, not here.
template <typename Sock>
struct udp_base {
protected:
    rio::handle fd{};

public:
    udp_base() = default;
    explicit udp_base(rio::handle &&h) noexcept : fd(std::move(h))
    {}

    udp_base(const udp_base &) = delete;
    udp_base &operator=(const udp_base &) = delete;
    udp_base(udp_base &&) = default;
    udp_base &operator=(udp_base &&) = default;

    auto native_handle() const -> int
    {
        return fd.native_handle();
    }

    auto close() && -> Udp_socket<udp_state::closed>
    {
        fd.close();
        return Udp_socket<udp_state::closed>{};
    }
    auto detatch() && -> int
    {
        return std::move(fd).detatch();
    }

    auto local_endpoint() const -> result<rio::address>
    {
        rio::address addr;
        addr.len = sizeof(addr.storage);
        if (::getsockname(fd.native_handle(), &addr.storage.general, &addr.len) == -1) {
            return std::unexpected(Err{errno, "getsockname failed"});
        }
        return addr;
    }

    template <typename T>
    auto set_option(int level, int optname, T value) -> Sock &
    {
        ::setsockopt(fd.native_handle(), level, optname, &value, sizeof(value));
        return static_cast<Sock &>(*this);
    }

    auto set_broadcast(bool enable) -> Sock &
    {
        int val = enable ? 1 : 0;
        set_option(SOL_SOCKET, SO_BROADCAST, val);
        return static_cast<Sock &>(*this);
    }

    auto set_reuse_address(bool enable) -> Sock &
    {
        int val = enable ? 1 : 0;
        set_option(SOL_SOCKET, SO_REUSEADDR, val);
        set_option(SOL_SOCKET, SO_REUSEPORT, val);
        return static_cast<Sock &>(*this);
    }
};
} // namespace detail

export template <>
struct Udp_socket<udp_state::unbound> : detail::udp_base<Udp_socket<udp_state::unbound>> {
    Udp_socket() = default;
    explicit Udp_socket(rio::handle &&h) noexcept : udp_base(std::move(h))
    {}

    static auto open(udp_opt options = udp_opt::v4) -> result<Udp_socket>;
    static auto open_and_bind(const rio::address &addr, udp_opt options = udp_opt::v4) -> result<Udp_socket<udp_state::bound>>;
    static auto open_and_bind(const char *ip, uint16_t port, udp_opt options = udp_opt::v4)
        -> result<std::tuple<Udp_socket<udp_state::bound>, rio::address>>;

    // Unsafe entry: takes ownership of a raw fd with no validation.
    static auto attach(int raw_fd) -> Udp_socket;

    // Consuming transitions. On failure the socket is consumed and its
    // fd closed: a failed transition has no valid source to go back to.
    auto bind(const address &addr) && -> result<Udp_socket<udp_state::bound>>;
    auto connect(const address &addr) && -> result<Udp_socket<udp_state::bound>>;
};

export template <>
struct Udp_socket<udp_state::bound> : detail::udp_base<Udp_socket<udp_state::bound>> {
    Udp_socket() = default;
    explicit Udp_socket(rio::handle &&h) noexcept : udp_base(std::move(h))
    {}

    // Capability marker: only a bound socket (plus file<open>, connected
    // TCP and bare handles) matches the generic io/fut read/write overloads.
    using readable_tag = void;

    // Unsafe entry: an already-bound fd.
    static auto attach(int raw_fd) -> Udp_socket;
};

// State transitions. Each consumes the source (&&).
export auto bind(Udp_socket<udp_state::unbound> &&s, const address &addr) -> result<Udp_socket<udp_state::bound>>;
export auto connect(Udp_socket<udp_state::unbound> &&s, const address &addr) -> result<Udp_socket<udp_state::bound>>;

auto Udp_socket<udp_state::unbound>::bind(const address &addr) && -> result<Udp_socket<udp_state::bound>>
{
    if (::bind(fd.native_handle(), addr.data(), addr.size()) == -1) [[unlikely]] {
        return std::unexpected(Err{errno, "Failed to bind UDP socket"});
    }
    return Udp_socket<udp_state::bound>{std::move(fd)};
}

auto Udp_socket<udp_state::unbound>::connect(const address &addr) && -> result<Udp_socket<udp_state::bound>>
{
    if (::connect(fd.native_handle(), addr.data(), addr.size()) == -1) [[unlikely]] {
        return std::unexpected(Err{errno, "Failed to connect UDP socket"});
    }
    return Udp_socket<udp_state::bound>{std::move(fd)};
}

auto Udp_socket<udp_state::unbound>::attach(int raw_fd) -> Udp_socket
{
    return Udp_socket{rio::handle(raw_fd)};
}
auto Udp_socket<udp_state::bound>::attach(int raw_fd) -> Udp_socket
{
    return Udp_socket{rio::handle(raw_fd)};
}

auto bind(Udp_socket<udp_state::unbound> &&s, const address &addr) -> result<Udp_socket<udp_state::bound>>
{
    return std::move(s).bind(addr);
}

auto connect(Udp_socket<udp_state::unbound> &&s, const address &addr) -> result<Udp_socket<udp_state::bound>>
{
    return std::move(s).connect(addr);
}

auto Udp_socket<udp_state::unbound>::open(udp_opt options) -> result<Udp_socket>
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

auto Udp_socket<udp_state::unbound>::open_and_bind(const rio::address &addr, udp_opt options) -> result<Udp_socket<udp_state::bound>>
{
    auto o_res = open(options);
    if (!o_res) [[unlikely]] {
        return std::unexpected(o_res.error());
    }

    return rio::bind(std::move(*o_res), addr);
}

auto Udp_socket<udp_state::unbound>::open_and_bind(const char *ip, uint16_t port, udp_opt options)
    -> result<std::tuple<Udp_socket<udp_state::bound>, rio::address>>
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

} // namespace rio
