module;

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>

export module rio:io.tcp_socket;

import std;
import :socket;
import :utils;

namespace rio::io {

export auto accept(const rio::Tcp_socket<rio::tcp_state::listening> &listener) -> result<Tcp_accept_result>
{
    rio::address client_addr;
    socklen_t len = sizeof(client_addr.storage);

    // accept4 allows setting CLOEXEC atomically
    // Blocks until connection arrives.
    int fd = ::accept4(listener.native_handle(), &client_addr.storage.general, &len, SOCK_CLOEXEC);

    if (fd < 0)
        return std::unexpected(rio::Err::sys("accept failed"));

    client_addr.len = len;
    return Tcp_accept_result{
        .client = rio::Tcp_socket<rio::tcp_state::connected>::attach(fd), .address = client_addr};
}

// Consuming connect: a successful call moves the socket into connected.
export auto connect(rio::Tcp_socket<rio::tcp_state::unbound> &&client, const rio::address &addr)
    -> result<rio::Tcp_socket<rio::tcp_state::connected>>
{
    // Blocks until connected.
    return std::move(client).connect(addr);
}

export auto try_accept(const rio::Tcp_socket<rio::tcp_state::listening> &listener) -> result<Tcp_accept_result>
{
    pollfd pfd;
    pfd.fd = listener.native_handle();
    pfd.events = POLLIN;

    int p = ::poll(&pfd, 1, 0); // Timeout 0 = return immediately

    if (p < 0)
        return std::unexpected(rio::Err::sys("poll failed"));
    if (p == 0)
        return std::unexpected(rio::Err::app(std::errc::operation_would_block, "No pending connections"));

    // 2. Safe to accept
    return accept(listener);
}

// "Try Connect": Initiates connection in non-blocking mode.
// Consumes the socket; a connected socket comes back on success.
export auto try_connect(rio::Tcp_socket<rio::tcp_state::unbound> &&client, const rio::address &addr)
    -> result<rio::Tcp_socket<rio::tcp_state::connected>>
{
    return std::move(client).try_connect(addr);
}

export auto try_read(const rio::Tcp_socket<rio::tcp_state::connected> &sock, std::span<char> buf) -> result<size_t>
{
    while (true) {
        ssize_t n = ::recv(sock.native_handle(), buf.data(), buf.size(), MSG_DONTWAIT);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return std::unexpected(rio::Err::app(std::errc::operation_would_block, "Would block"));

            return std::unexpected(rio::Err::sys("recv failed"));
        }
        return static_cast<size_t>(n);
    }
}

// "Try Write": Uses MSG_DONTWAIT. Safe on blocking sockets.
export auto try_write(const rio::Tcp_socket<rio::tcp_state::connected> &sock, std::span<const char> buf) -> result<size_t>
{
    while (true) {
        ssize_t n = ::send(sock.native_handle(), buf.data(), buf.size(), MSG_DONTWAIT | MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return std::unexpected(rio::Err::app(std::errc::operation_would_block, "Would block"));

            return std::unexpected(rio::Err::sys("send failed"));
        }
        return static_cast<size_t>(n);
    }
}

export template <typename Handler>
    requires std::invocable<Handler, rio::Tcp_socket<rio::tcp_state::connected>, rio::address>
auto accept_all(const rio::Tcp_socket<rio::tcp_state::listening> &listener, Handler &&handler) -> result<void>
{
    while (true) {
        auto res = accept(listener);
        if (!res)
            return std::unexpected(res.error());

        auto [sock, addr] = std::move(*res);
        std::invoke(std::forward<Handler>(handler), std::move(sock), std::move(addr));
    }
}

export template <typename Handler>
    requires std::invocable<Handler, rio::Tcp_socket<rio::tcp_state::connected>, rio::address>
auto accept_many(const rio::Tcp_socket<rio::tcp_state::listening> &listener, size_t limit, Handler &&handler)
    -> result<void>
{
    for (size_t i = 0; i < limit; ++i) {
        auto res = accept(listener);
        if (!res)
            return std::unexpected(res.error());

        auto [sock, addr] = std::move(*res);
        std::invoke(std::forward<Handler>(handler), std::move(sock), std::move(addr));
    }
    return {};
}

export template <typename OutputIt>
auto accept_into(const rio::Tcp_socket<rio::tcp_state::listening> &listener, size_t limit, OutputIt d_first)
    -> result<void>
{
    for (size_t i = 0; i < limit; ++i) {
        auto res = accept(listener);
        if (!res)
            return std::unexpected(res.error());

        *d_first++ = std::move(*res);
    }
    return {};
}

export template <typename Handler>
    requires std::invocable<Handler, rio::Tcp_socket<rio::tcp_state::connected>, rio::address>
auto try_accept_pending(const rio::Tcp_socket<rio::tcp_state::listening> &listener, size_t limit, Handler &&handler)
    -> result<size_t>
{
    size_t count = 0;
    while (count < limit) {
        // Use try_accept to check without blocking
        auto res = try_accept(listener);

        if (!res) {
            // If it's just empty, we are done with the batch
            if (res.error().code == std::errc::operation_would_block)
                break;

            return std::unexpected(res.error());
        }

        auto [sock, addr] = std::move(*res);
        std::invoke(std::forward<Handler>(handler), std::move(sock), std::move(addr));
        count++;
    }
    return count;
}

export template <typename Rep, typename Period>
auto accept_with_timeout(
    const rio::Tcp_socket<rio::tcp_state::listening> &listener, std::chrono::duration<Rep, Period> timeout)
    -> result<Tcp_accept_result>
{
    using namespace std::chrono;
    auto ms = duration_cast<milliseconds>(timeout).count();

    pollfd pfd;
    pfd.fd = listener.native_handle();
    pfd.events = POLLIN;

    // poll accepts milliseconds. -1 is infinite, 0 is non-blocking.
    int p = ::poll(&pfd, 1, static_cast<int>(ms));

    if (p < 0)
        return std::unexpected(rio::Err::sys("poll failed"));

    if (p == 0)
        return std::unexpected(rio::Err::app(std::errc::timed_out, "accept timed out"));

    // Ready to read
    return accept(listener);
}

} // namespace rio::io
