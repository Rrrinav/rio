module;

#include <cstddef>
export module rio:socket;
export import :socket.tcp_socket;
export import :socket.udp_socket;
export import :socket.address;

namespace rio {
export struct Tcp_accept_result
{
    rio::Tcp_socket client;
    rio::address address;
};

export struct Udp_recv_result
{
    std::size_t len;
    rio::address addr;
};
} // namespace rio
