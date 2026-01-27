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

    static void on_complete(internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<uring_request *>(ptr);

        if (res < 0)
            self->callback(self->context, std::unexpected(rio::Err{-res, "IO operation failed"}), self->user_data);
        else
            self->callback(self->context, static_cast<std::size_t>(res), self->user_data);

        delete self;
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

    static void on_complete(internals::uring_request_header *ptr, int res)
    {
        auto *self = reinterpret_cast<uring_accept_request *>(ptr);

        if (res < 0)
        {
            self->callback(self->context, std::unexpected(rio::Err{-res, "Accept failed"}), self->user_data);
        }
        else
        {
            auto client_sock = rio::Tcp_socket::attach(res);
            self->client_addr.len = self->addr_len;

            // Construct our dedicated result struct
            accept_result result{.client = std::move(client_sock), .address = std::move(self->client_addr)};

            self->callback(self->context, std::move(result), self->user_data);
        }

        delete self;
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
    if (!sqe) return;

    using request_type = uring_request<std::decay_t<Fn>, T>;

    auto *req = new request_type {
        .header = {.call = &request_type::on_complete},
        .type = Req_type::Read,
        .handle = sock.fd.native_handle(),
        .io_v = iovec{.iov_base = buffer.data(), .iov_len = buffer.size()},
        .user_data = user,
        .callback = std::forward<Fn>(on_read),
        .context = context
    };

    io_uring_prep_readv(sqe, req->handle, &req->io_v, 1, 0);

    io_uring_sqe_set_data(sqe, static_cast<internals::uring_request_header *>(&req->header));
    context.submit();
}

export template <typename T, typename Fn>
requires On_Write_CB_C<Fn, T>
void write(rio::context &context, rio::Tcp_socket &sock, std::span<const char> buffer, Fn &&on_write, T *user)
{
    auto *sqe = context.sqe();
    if (!sqe) return;

    using request_type = uring_request<std::decay_t<Fn>, T>;

    auto *req = new request_type{
        .header = {.call = &request_type::on_complete},
        .type = Req_type::Write,
        .handle = sock.fd.native_handle(),
        .io_v = iovec{.iov_base = const_cast<char *>(buffer.data()), .iov_len = buffer.size()},
        .user_data = user,
        .callback = std::forward<Fn>(on_write),
        .context = context
    };

    io_uring_prep_writev(sqe, req->handle, &req->io_v, 1, 0);

    io_uring_sqe_set_data(sqe, static_cast<internals::uring_request_header *>(&req->header));
    context.submit();
}

export template <typename T, typename Fn>
requires On_Accept_CB_C<Fn, T>
void accept(rio::context &context, rio::Tcp_socket &listener, Fn &&on_accept, T *user)
{
    auto *sqe = context.sqe();
    if (!sqe) return;

    using request_type = uring_accept_request<std::decay_t<Fn>, T>;

    auto *req = new request_type{
        .header = {.call = &request_type::on_complete},
        .context = context,
        .user_data = user,
        .callback = std::forward<Fn>(on_accept),
        .listener_fd = listener.fd.native_handle(),
        .client_addr = {},
        .addr_len = sizeof(sockaddr_storage)
    };

    io_uring_prep_accept(sqe, req->listener_fd, reinterpret_cast<sockaddr *>(&req->client_addr.storage), &req->addr_len, 0);

    io_uring_sqe_set_data(sqe, static_cast<internals::uring_request_header *>(&req->header));
    context.submit();
}

}  // namespace rio::as
