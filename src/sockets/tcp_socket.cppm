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

export enum class tcp_shut : int { read = SHUT_RD, write = SHUT_WR, both = SHUT_RDWR };

// Typestate TCP socket: lifecycle states live in the type, so operating
// on a socket in the wrong state is a compile error, not a runtime errno.
//
//   Tcp_socket<closed>     — empty token; only close()/default produce one.
//   Tcp_socket<unbound>    — fresh fd; bind()/connect() consume it.
//   Tcp_socket<bound>      — bound; listen() consumes it.
//   Tcp_socket<listening>  — accepts connections; never reads/writes.
//   Tcp_socket<connected>  — a live stream; the only readable state.
//
// Ownership is move-only throughout. Transitions consume (&&): after a
// call the old object is moved-from and unusable, by construction.
// The only raw-fd crossings are the explicitly-marked attach()/detatch().
// native_handle() is available on every open state as a documented
// low-level escape; the operation signatures above it enforce the machine.
export enum class tcp_state { closed, unbound, bound, listening, connected };

export template <tcp_state S>
struct Tcp_socket;

export template <>
struct Tcp_socket<tcp_state::closed> {
    Tcp_socket() = default;

    Tcp_socket(const Tcp_socket &) = delete;
    Tcp_socket &operator=(const Tcp_socket &) = delete;
    Tcp_socket(Tcp_socket &&) = default;
    Tcp_socket &operator=(Tcp_socket &&) = default;
};

namespace detail {
// Shared interior for every open state: the owned fd plus the socket
// options, which are valid pre- and post-bind alike. State-specific
// operations are NOT here; each specialization exposes only its own.
template <typename Sock>
struct tcp_base {
protected:
    rio::handle fd{};

public:
    tcp_base() = default;
    explicit tcp_base(rio::handle &&h) noexcept : fd(std::move(h))
    {}

    tcp_base(const tcp_base &) = delete;
    tcp_base &operator=(const tcp_base &) = delete;
    tcp_base(tcp_base &&) = default;
    tcp_base &operator=(tcp_base &&) = default;

    auto native_handle() const -> int
    {
        return fd.native_handle();
    }

    // Consuming exits available on every open state.
    auto close() && -> Tcp_socket<tcp_state::closed>
    {
        fd.close();
        return Tcp_socket<tcp_state::closed>{};
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

    auto set_reuse_address(bool enable) -> Sock &
    {
        int val = enable ? 1 : 0;
        set_option(SOL_SOCKET, SO_REUSEADDR, val);
        set_option(SOL_SOCKET, SO_REUSEPORT, val);

        return static_cast<Sock &>(*this);
    }

    auto set_nodelay(bool enable) -> Sock &
    {
        set_option(IPPROTO_TCP, TCP_NODELAY, enable ? 1 : 0);
        return static_cast<Sock &>(*this);
    }

    auto set_keepalive(bool enable) -> Sock &
    {
        set_option(SOL_SOCKET, SO_KEEPALIVE, enable ? 1 : 0);
        return static_cast<Sock &>(*this);
    }

    auto set_blocking_policy(bool blocking) -> Sock &
    {
        int flags = ::fcntl(fd.native_handle(), F_GETFL, 0);

        if (blocking) {
            flags &= ~O_NONBLOCK;
        } else {
            flags |= O_NONBLOCK;
        }

        ::fcntl(fd.native_handle(), F_SETFL, flags);

        return static_cast<Sock &>(*this);
    }

    auto set_blocking() -> Sock &
    {
        return set_blocking_policy(true);
    }
    auto set_nonblocking() -> Sock &
    {
        return set_blocking_policy(false);
    }

    template <typename T>
    auto get_option(int level, int optname, T &value) const -> result<void>
    {
        socklen_t len = sizeof(T);
        if (::getsockopt(fd.native_handle(), level, optname, &value, &len) == -1) {
            return std::unexpected(Err{errno, "getsockopt failed"});
        }
        return {};
    }
};
} // namespace detail

export template <>
struct Tcp_socket<tcp_state::unbound> : detail::tcp_base<Tcp_socket<tcp_state::unbound>> {
    Tcp_socket() = default;
    explicit Tcp_socket(rio::handle &&h) noexcept : tcp_base(std::move(h))
    {}

    static auto open(s_opt options) -> result<Tcp_socket>;
    static auto open(const rio::address &address, s_opt options) -> result<Tcp_socket<tcp_state::bound>>;
    static auto open(const char *ip, uint16_t port, s_opt options) -> result<std::tuple<Tcp_socket<tcp_state::bound>, rio::address>>;

    static auto open_and_listen(const rio::address &address, s_opt options = s_opt::sync_server_v4, int backlog = 128)
        -> result<Tcp_socket<tcp_state::listening>>;
    static auto open_and_listen(const char *ip, uint16_t port, s_opt options = s_opt::sync_server_v4, int backlog = 128)
        -> result<std::tuple<Tcp_socket<tcp_state::listening>, rio::address>>;

    // Unsafe entry: takes ownership of a raw fd with no validation.
    static auto attach(int raw_fd) -> Tcp_socket;

    // Consuming transitions. On failure the socket is consumed and its
    // fd closed: a failed transition has no valid source to go back to.
    auto bind(const address &addr) && -> result<Tcp_socket<tcp_state::bound>>;
    auto connect(const address &addr) && -> result<Tcp_socket<tcp_state::connected>>;
    auto try_connect(const address &addr) && -> result<Tcp_socket<tcp_state::connected>>;
};

export template <>
struct Tcp_socket<tcp_state::bound> : detail::tcp_base<Tcp_socket<tcp_state::bound>> {
    Tcp_socket() = default;
    explicit Tcp_socket(rio::handle &&h) noexcept : tcp_base(std::move(h))
    {}

    auto listen(int backlog = 128) && -> result<Tcp_socket<tcp_state::listening>>;
};

export template <>
struct Tcp_socket<tcp_state::listening> : detail::tcp_base<Tcp_socket<tcp_state::listening>> {
    Tcp_socket() = default;
    explicit Tcp_socket(rio::handle &&h) noexcept : tcp_base(std::move(h))
    {}

    // Unsafe entry: an fd that is already listening.
    static auto attach(int raw_fd) -> Tcp_socket;
};

export template <>
struct Tcp_socket<tcp_state::connected> : detail::tcp_base<Tcp_socket<tcp_state::connected>> {
    Tcp_socket() = default;
    explicit Tcp_socket(rio::handle &&h) noexcept : tcp_base(std::move(h))
    {}

    // Capability marker: only a connected stream (plus file<open> and
    // bare handles) matches the generic io/fut read/write overloads.
    using readable_tag = void;

    // Unsafe entry: an already-connected fd (e.g. from accept).
    static auto attach(int raw_fd) -> Tcp_socket;

    auto shutdown(tcp_shut how = tcp_shut::write) -> result<void>
    {
        if (::shutdown(fd.native_handle(), static_cast<int>(how)) == -1) {
            return std::unexpected(Err{errno, "Socket shutdown failed"});
        }
        return {};
    }

    auto remote_endpoint() const -> result<rio::address>
    {
        rio::address addr;
        addr.len = sizeof(addr.storage);
        if (::getpeername(fd.native_handle(), &addr.storage.general, &addr.len) == -1) {
            return std::unexpected(Err{errno, "getpeername failed"});
        }
        return addr;
    }
};

// State transitions. Each consumes the source (&&): the pre-transition
// object cannot be used afterwards, by construction.
export auto bind(Tcp_socket<tcp_state::unbound> &&s, const address &addr) -> result<Tcp_socket<tcp_state::bound>>;
export auto listen(Tcp_socket<tcp_state::bound> &&s, int backlog = 128) -> result<Tcp_socket<tcp_state::listening>>;

auto Tcp_socket<tcp_state::unbound>::attach(int raw_fd) -> Tcp_socket
{
    return Tcp_socket{rio::handle(raw_fd)};
}
auto Tcp_socket<tcp_state::listening>::attach(int raw_fd) -> Tcp_socket
{
    return Tcp_socket{rio::handle(raw_fd)};
}
auto Tcp_socket<tcp_state::connected>::attach(int raw_fd) -> Tcp_socket
{
    return Tcp_socket{rio::handle(raw_fd)};
}

auto bind(Tcp_socket<tcp_state::unbound> &&s, const address &addr) -> result<Tcp_socket<tcp_state::bound>>
{
    return std::move(s).bind(addr);
}

auto listen(Tcp_socket<tcp_state::bound> &&s, int backlog) -> result<Tcp_socket<tcp_state::listening>>
{
    return std::move(s).listen(backlog);
}

auto Tcp_socket<tcp_state::unbound>::bind(const address &addr) && -> result<Tcp_socket<tcp_state::bound>>
{
    if (::bind(fd.native_handle(), &addr.storage.general, addr.len) == -1) [[unlikely]] {
        return std::unexpected(Err{errno, "Failed to bind socket"});
    }
    return Tcp_socket<tcp_state::bound>{std::move(fd)};
}

auto Tcp_socket<tcp_state::unbound>::connect(const address &addr) && -> result<Tcp_socket<tcp_state::connected>>
{
    if (::connect(fd.native_handle(), addr.data(), addr.size()) == -1) [[unlikely]] {
        return std::unexpected(Err{errno, "Failed to connect socket"});
    }
    return Tcp_socket<tcp_state::connected>{std::move(fd)};
}

auto Tcp_socket<tcp_state::unbound>::try_connect(const address &addr) && -> result<Tcp_socket<tcp_state::connected>>
{
    int raw = fd.native_handle();

    int flags = ::fcntl(raw, F_GETFL, 0);
    ::fcntl(raw, F_SETFL, flags | O_NONBLOCK);

    int res = ::connect(raw, addr.data(), addr.size());

    ::fcntl(raw, F_SETFL, flags);

    if (res < 0) {
        if (errno == EINPROGRESS) {
            return std::unexpected(Err{EINPROGRESS, "Connection started"});
        }
        return std::unexpected(Err{errno, "connect failed"});
    }

    return Tcp_socket<tcp_state::connected>{std::move(fd)};
}

auto Tcp_socket<tcp_state::bound>::listen(int backlog) && -> result<Tcp_socket<tcp_state::listening>>
{
    if (::listen(fd.native_handle(), backlog) == -1) [[unlikely]] {
        return std::unexpected(Err{errno, "Failed to listen on socket"});
    }
    return Tcp_socket<tcp_state::listening>{std::move(fd)};
}

auto Tcp_socket<tcp_state::unbound>::open(s_opt options) -> result<Tcp_socket>
{
    if (has(options, s_opt::v4) && has(options, s_opt::v6)) {
        return std::unexpected(Err{EINVAL, "Cannot specify both IPv4 and IPv6"});
    }

    if (has(options, s_opt::dualstack) && !has(options, s_opt::v6)) {
        return std::unexpected(Err{EINVAL, "Dualstack requires IPv6"});
    }

    const int domain = (has(options, s_opt::v6) || has(options, s_opt::dualstack)) ? AF_INET6 : AF_INET;

    int type = SOCK_STREAM;
    if (has(options, s_opt::cloexec)) {
        type |= SOCK_CLOEXEC;
    }
    if (has(options, s_opt::nonblock)) {
        type |= SOCK_NONBLOCK;
    }

    const int s = ::socket(domain, type, 0);
    if (s == -1) {
        return std::unexpected(Err{errno, "Failed to create TCP socket"});
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

    if (has(options, s_opt::dualstack)) {
        if (auto res = set_sockopt(IPPROTO_IPV6, IPV6_V6ONLY, off, "IPV6_V6ONLY"); !res) {
            return std::unexpected(res.error());
        }
    }

    if (has(options, s_opt::reuse)) {
        if (auto res = set_sockopt(SOL_SOCKET, SO_REUSEADDR, on, "SO_REUSEADDR"); !res) {
            return std::unexpected(res.error());
        }
        if (auto res = set_sockopt(SOL_SOCKET, SO_REUSEPORT, on, "SO_REUSEPORT"); !res) {
            return std::unexpected(res.error());
        }
    }

    if (has(options, s_opt::nodelay)) {
        if (auto res = set_sockopt(IPPROTO_TCP, TCP_NODELAY, on, "TCP_NODELAY"); !res) {
            return std::unexpected(res.error());
        }
    }

    if (has(options, s_opt::keepalive)) {
        if (auto res = set_sockopt(SOL_SOCKET, SO_KEEPALIVE, on, "SO_KEEPALIVE"); !res) {
            return std::unexpected(res.error());
        }
    }

    return Tcp_socket::attach(s);
}

auto Tcp_socket<tcp_state::unbound>::open(const rio::address &address, s_opt options) -> result<Tcp_socket<tcp_state::bound>>
{
    auto o_res = open(options);
    if (!o_res) [[unlikely]] {
        return std::unexpected(o_res.error());
    }

    return rio::bind(std::move(*o_res), address);
}

auto Tcp_socket<tcp_state::unbound>::open(const char *ip, uint16_t port, s_opt options)
    -> result<std::tuple<Tcp_socket<tcp_state::bound>, rio::address>>
{
    auto addr = rio::address::from_ip(ip, port);
    if (!addr) [[unlikely]] {
        return std::unexpected(addr.error());
    }

    if (has(options, s_opt::v6) && addr->family() == AF_INET) {
        return std::unexpected(Err{EINVAL, "IPv6 socket cannot bind IPv4 address"});
    }
    if (has(options, s_opt::v4) && addr->family() == AF_INET6) {
        return std::unexpected(Err{EINVAL, "IPv4 socket cannot bind IPv6 address"});
    }

    auto o_res = open(*addr, options);
    if (!o_res) [[unlikely]] {
        return std::unexpected(o_res.error());
    }

    return std::make_tuple(std::move(*o_res), std::move(*addr));
}

auto Tcp_socket<tcp_state::unbound>::open_and_listen(const rio::address &address, s_opt options, int backlog)
    -> result<Tcp_socket<tcp_state::listening>>
{
    auto o_res = open(address, options);
    if (!o_res) [[unlikely]] {
        return std::unexpected(o_res.error());
    }

    return rio::listen(std::move(*o_res), backlog);
}

auto Tcp_socket<tcp_state::unbound>::open_and_listen(const char *ip, uint16_t port, s_opt options, int backlog)
    -> result<std::tuple<Tcp_socket<tcp_state::listening>, rio::address>>
{
    auto o_res = open(ip, port, options);
    if (!o_res) [[unlikely]] {
        return std::unexpected(o_res.error());
    }

    auto &[sock, addr] = *o_res;
    auto l_res = rio::listen(std::move(sock), backlog);
    if (!l_res) [[unlikely]] {
        return std::unexpected(l_res.error());
    }

    return std::make_tuple(std::move(*l_res), std::move(addr));
}

} // namespace rio
