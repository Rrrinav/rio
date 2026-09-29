module;

#include <cerrno>
#include <sys/socket.h>

export module rio:io.udp_socket;

import std;
import :socket;
import :socket.udp_socket;
import :utils;

namespace rio::io {

export auto recv_from(const rio::Udp_socket<rio::udp_state::bound> &sock, std::span<char> buf)
    -> result<rio::Udp_recv_result>
{
    rio::address peer_addr;
    peer_addr.len = sizeof(peer_addr.storage);

    while (true) {
        ssize_t n = ::recvfrom(sock.native_handle(), buf.data(), buf.size(), 0, peer_addr.data(), &peer_addr.len);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return std::unexpected(rio::Err::app(std::errc::operation_would_block, "Would block"));
            }

            return std::unexpected(rio::Err::sys("recvfrom failed"));
        }

        return rio::Udp_recv_result{static_cast<size_t>(n), peer_addr};
    }
}

namespace detail {
auto send_to_impl(int fd, std::span<const char> buf, const rio::address &dest) -> result<size_t>
{
    while (true) {
        ssize_t n = ::sendto(fd, buf.data(), buf.size(), 0, dest.data(), dest.size());

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return std::unexpected(rio::Err::app(std::errc::operation_would_block, "Would block"));
            }

            return std::unexpected(rio::Err::sys("sendto failed"));
        }

        return static_cast<size_t>(n);
    }
}
} // namespace detail

// send_to is valid on any open UDP socket: POSIX allows sending from an
// unbound socket (an ephemeral port is assigned). Receiving needs bound.
export auto send_to(const rio::Udp_socket<rio::udp_state::unbound> &sock, std::span<const char> buf,
    const rio::address &dest) -> result<size_t>
{
    return detail::send_to_impl(sock.native_handle(), buf, dest);
}

export auto send_to(const rio::Udp_socket<rio::udp_state::bound> &sock, std::span<const char> buf,
    const rio::address &dest) -> result<size_t>
{
    return detail::send_to_impl(sock.native_handle(), buf, dest);
}

} // namespace rio::io
