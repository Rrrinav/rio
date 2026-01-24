#include <sys/types.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <print>
#include <string>

import std;

import rio;

const rio::handle out(STDOUT_FILENO);
const rio::handle in(STDOUT_FILENO);

void accept_handler(rio::Tcp_socket sock, const rio::address& add)
{
    rio::io::write(out, std::format(" [RIO]: Accepted connection from {}\n", add));
    rio::io::write_all(sock, "Welcome to rio\r\n");

    char msg_buf[1024];
    std::span<char> span(msg_buf);

    ::size_t n = rio::io::read(sock, span);

    if (n <= 0)
    {
        rio::io::write(out, " [RIO]: Receive issue.\n");
        rio::io::write(out, " [RIO]: Client disconnected.\n");
        rio::kill(sock.fd);
        return;
    }

    rio::io::write(out, " [RIO]: Received: ");
    rio::io::write(out, std::span<char>{msg_buf, n});
    rio::io::write(sock, "You sent: ");
    rio::io::write(sock, std::span<char>{msg_buf, n});
    rio::io::write(sock, "\r\n");

    // Not required because handle (fd) in Tcp_socket will destroy itself using RAII.
    // but still safe because destructor has a check.
    rio::kill(sock.fd);
    rio::io::write(out, " [RIO]: Client disconnected.\n");
}

int main()
{
    rio::Tcp_socket sock{};
    rio::address addr;
    rio::io::write(out, " [RIO]: Enter port: ");
    uint16_t default_port = 6969;

    // Totally unnecessary btw, I was just trying to learn monadic operations on std::expected
    uint16_t port = rio::io::read_line(in)
        .and_then([&](std::string s) -> rio::result<uint16_t> {
            if (!s.empty() && s.back() == '\n')
                s.pop_back();

            if (s.empty())
            {
                std::println(" [RIO]: No port entered, choosing default: {}", default_port);
                return default_port;
            }

            int p = 0;
            auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), p);

            if (ec != std::errc{} || ptr != s.data() + s.size())
            {
                std::println(" [RIO]: Invalid port '{}', choosing default: {}", s, default_port);
                return default_port;
            }

            if (p < 1024 || p > std::numeric_limits<uint16_t>::max()) // I can't remember 65535
            {
                std::println(" [RIO]: Port {} out of range (1024..65535), choosing default: {}", p, default_port);
                return default_port;
            }

            auto out = static_cast<uint16_t>(p);
            std::println("Using port: {}", out);
            return out;
        }).or_else([&](rio::Err e) -> rio::result<uint16_t>{
            std::println("Failed to read input: {}", e);
            std::println("Choosing default port: {}", default_port);
            return default_port;
        }).value_or(default_port);

    // Equivalent to: auto sock_res = rio::Tcp_socket::open_and_listen("localhost", 6969, rio::s_opt::sync_server_v4, 128);
    auto sock_res = rio::Tcp_socket::open_and_listen("localhost", port);

    if (!sock_res)
    {
        rio::io::write(out, std::format("Socket creation failed: {}\n", sock_res.error()));
        return 1;
    }

    std::tie(sock, addr) = std::move(*sock_res);
    std::println(" [RIO]: listening to: {}", addr);

    while (true)
    {
        rio::io::write(out, " [RIO]: Waiting for connection...\n");
        if (auto n = rio::accept(sock, accept_handler); !n)
        {
            rio::io::write(out, std::format(" [RIO]: Accept failed: {}\n", n.error()));
            return 1;
        }
    }

    return 0;
}
