module;

export module rio:socket;
export import :socket.tcp_socket;
export import :socket.address;

namespace rio {
export struct Tcp_accept_result
{
    rio::Tcp_socket client;
    rio::address address;
};
} // namespace rio
