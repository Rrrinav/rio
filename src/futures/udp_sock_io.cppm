module;

#include <liburing.h>
#include <sys/socket.h>

export module rio:fut.udp.io;

import std;
import :context;
import :socket;
import :promise;
import :futures;
import :fut.io;
import :io.udp_socket;

namespace rio::fut {

struct Udp_recv_payload
{
    rio::address peer_addr;
    struct iovec iov;
    struct msghdr msg;
};

using Udp_recv_req = Basic_op<rio::Udp_recv_result, Udp_recv_payload>;

export auto recv_from() -> result<rio::Udp_recv_result>
{
    return {};
}

} // namespace rio::fut
