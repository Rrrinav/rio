module;

#include <liburing.h>

export module rio:asio;

export import :utils;
export import :handle;
export import :socket;
export import :context;

namespace rio::as {

enum class Req_type { Read, Write, Accept };

export struct accept_result
{
    rio::Tcp_socket client;
    rio::address address;
};

template <typename Fn, typename User_data>
struct uring_request
{
    internals::uring_request_header header;

    using callable_t = Fn;
    Req_type type{};
    int handle;
    iovec io_v;
    User_data *user_data;
    Fn callback;
    rio::context &context;

    uring_request(Req_type t, int h, iovec v, User_data *ud, Fn &&cb, rio::context &c)
        : type(t), handle(h), io_v(v), user_data(ud), callback(std::forward<Fn>(cb)), context(c)
    {}

    static void on_complete(internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<uring_request *>(ptr);

        if (res < 0)
            self->callback(self->context, std::unexpected(rio::Err{-res, "IO operation failed"}), self->user_data);
        else
            self->callback(self->context, static_cast<std::size_t>(res), self->user_data);

        self->context.recycle(self);
    }
};

template <typename Fn, typename User_data>
struct uring_accept_request
{
    internals::uring_request_header header;

    rio::context &context;
    User_data *user_data;
    Fn callback;

    int listener_fd;
    rio::address client_addr;
    socklen_t addr_len;

    uring_accept_request(rio::context &c, User_data *ud, Fn &&cb, int fd)
        : context(c), user_data(ud), callback(std::forward<Fn>(cb)), listener_fd(fd), addr_len(sizeof(sockaddr_storage))
    {}

    static void on_complete(internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<uring_accept_request *>(ptr);

        if (res < 0) {
            self->callback(self->context, std::unexpected(rio::Err{-res, "Accept failed"}), self->user_data);
        } else {
            auto client_sock = rio::Tcp_socket::attach(res);
            self->client_addr.len = self->addr_len;

            // Construct our dedicated result struct
            accept_result result{.client = std::move(client_sock), .address = std::move(self->client_addr)};

            self->callback(self->context, std::move(result), self->user_data);
        }

        self->context.recycle(self);
    }
};

template <typename Fn, typename T>
concept On_Read_CB_C = std::invocable<Fn, rio::context &, rio::result<std::size_t>, T *>;

template <typename Fn, typename T>
concept On_Write_CB_C = std::invocable<Fn, rio::context &, rio::result<std::size_t>, T *>;

template <typename Fn, typename T>
concept On_Accept_CB_C = std::invocable<Fn, rio::context &, rio::result<accept_result>, T *>;

export template <typename T, typename Fn>
    requires On_Read_CB_C<Fn, T>
void read(rio::context &context, rio::Tcp_socket &sock, std::span<char> buffer, Fn &&on_read, T *user)
{
    auto *sqe = context.sqe();
    if (!sqe)
        return;

    using request_type = uring_request<std::decay_t<Fn>, T>;

    auto *req = context.make_pooled<request_type>(
        Req_type::Read, sock.fd.native_handle(),
        iovec{.iov_base = buffer.data(), .iov_len = buffer.size()},
        user, std::forward<Fn>(on_read), context);
    req->header.call = &request_type::on_complete;

    io_uring_prep_readv(sqe, req->handle, &req->io_v, 1, 0);

    io_uring_sqe_set_data(sqe, static_cast<internals::uring_request_header *>(&req->header));
    context.submit();
}

export template <typename T, typename Fn>
    requires On_Write_CB_C<Fn, T>
void write(rio::context &context, rio::Tcp_socket &sock, std::span<const char> buffer, Fn &&on_write, T *user)
{
    auto *sqe = context.sqe();
    if (!sqe)
        return;

    using request_type = uring_request<std::decay_t<Fn>, T>;

    auto *req = context.make_pooled<request_type>(
        Req_type::Write, sock.fd.native_handle(),
        iovec{.iov_base = const_cast<char *>(buffer.data()), .iov_len = buffer.size()},
        user, std::forward<Fn>(on_write), context);
    req->header.call = &request_type::on_complete;

    io_uring_prep_writev(sqe, req->handle, &req->io_v, 1, 0);

    io_uring_sqe_set_data(sqe, static_cast<internals::uring_request_header *>(&req->header));
    context.submit();
}

export template <typename T, typename Fn>
    requires On_Accept_CB_C<Fn, T>
void accept(rio::context &context, rio::Tcp_socket &listener, Fn &&on_accept, T *user)
{
    auto *sqe = context.sqe();
    if (!sqe)
        return;

    using request_type = uring_accept_request<std::decay_t<Fn>, T>;

    auto *req = context.make_pooled<request_type>(context, user, std::forward<Fn>(on_accept), listener.fd.native_handle());
    req->header.call = &request_type::on_complete;

    io_uring_prep_accept(sqe, req->listener_fd, reinterpret_cast<sockaddr *>(&req->client_addr.storage), &req->addr_len, 0);

    io_uring_sqe_set_data(sqe, static_cast<internals::uring_request_header *>(&req->header));
    context.submit();
}

} // namespace rio::as
