module;

#include <netinet/in.h>
#include <arpa/inet.h>
#include <cerrno>

export module rio:socket.address;
import :utils.result;

import std;

namespace rio {

export struct address
{
    union storage_t
    {
        sockaddr general;
        sockaddr_in v4;
        sockaddr_in6 v6;
        sockaddr_storage any;

        storage_t() : any{} {}
    } storage;

    socklen_t len = 0;

    // Default constructor
    address() = default;

    [[nodiscard]]
    static auto from_ipv4(const char *ip, uint16_t port) -> result<address>
    {
        address addr;
        addr.storage.v4.sin_family = AF_INET;
        addr.storage.v4.sin_port = htons(port);
        addr.len = sizeof(sockaddr_in);

        std::string given_ip(ip);
        std::string ip_str = (given_ip == "localhost") ? "127.0.0.1" : given_ip;

        if (::inet_pton(AF_INET, ip_str.c_str(), &addr.storage.v4.sin_addr) != 1) [[unlikely]]
            return std::unexpected(Err{EINVAL, std::format("Invalid IPv4 address: '{}'", ip)});

        return addr;
    }

    [[nodiscard]]
    static auto from_ipv4(uint32_t ip_host_order, uint16_t port) -> address
    {
        address addr;
        addr.storage.v4.sin_family = AF_INET;
        addr.storage.v4.sin_port = htons(port);
        addr.storage.v4.sin_addr.s_addr = htonl(ip_host_order);
        addr.len = sizeof(sockaddr_in);
        return addr;
    }

    [[nodiscard]]
    static auto any_ipv4(uint16_t port) -> address
    {
        return from_ipv4(INADDR_ANY, port);
    }

    [[nodiscard]]
    static auto localhost_ipv4(uint16_t port) -> address
    {
        return from_ipv4(INADDR_LOOPBACK, port);
    }

    // --- IPv6 Construction ---

    [[nodiscard]]
    static auto from_ipv6(const char *ip, uint16_t port) -> result<address>
    {
        address addr;
        addr.storage.v6.sin6_family = AF_INET6;
        addr.storage.v6.sin6_port = htons(port);
        addr.len = sizeof(sockaddr_in6);

        std::string given_ip(ip);
        std::string ip_str = (given_ip == "localhost") ? "::1" : given_ip;

        if (::inet_pton(AF_INET6, ip_str.c_str(), &addr.storage.v6.sin6_addr) != 1) [[unlikely]]
            return std::unexpected(Err{EINVAL, std::format("Invalid IPv6 address: '{}'", ip)});

        return addr;
    }

    [[nodiscard]]
    static auto any_ipv6(uint16_t port) -> address
    {
        address addr;
        addr.storage.v6.sin6_family = AF_INET6;
        addr.storage.v6.sin6_port = htons(port);
        addr.storage.v6.sin6_addr = in6addr_any;
        addr.len = sizeof(sockaddr_in6);
        return addr;
    }

    [[nodiscard]]
    static auto localhost_ipv6(uint16_t port) -> address
    {
        address addr;
        addr.storage.v6.sin6_family = AF_INET6;
        addr.storage.v6.sin6_port = htons(port);
        addr.storage.v6.sin6_addr = in6addr_loopback;
        addr.len = sizeof(sockaddr_in6);
        return addr;
    }

    // --- Auto-detect IP version ---

    [[nodiscard]]
    static auto from_ip(const char *ip, uint16_t port) -> result<address>
    {
        std::string given_ip(ip);

        // Handle "localhost" - default to IPv4 for compatibility
        if (given_ip == "localhost")
            return from_ipv4("127.0.0.1", port);

        // Try IPv4 first (more common, faster)
        address addr;
        addr.storage.v4.sin_family = AF_INET;
        addr.storage.v4.sin_port = htons(port);

        if (::inet_pton(AF_INET, ip, &addr.storage.v4.sin_addr) == 1)
        {
            addr.len = sizeof(sockaddr_in);
            return addr;
        }

        // Try IPv6
        addr.storage.v6.sin6_family = AF_INET6;
        addr.storage.v6.sin6_port = htons(port);

        if (::inet_pton(AF_INET6, ip, &addr.storage.v6.sin6_addr) == 1)
        {
            addr.len = sizeof(sockaddr_in6);
            return addr;
        }

        return std::unexpected(Err{EINVAL, std::format("Invalid IP address: '{}'", ip)});
    }

    // --- Query Methods ---

    [[nodiscard]]
    auto family() const -> sa_family_t
    {
        return storage.general.sa_family;
    }

    [[nodiscard]]
    auto is_ipv4() const -> bool
    {
        return storage.general.sa_family == AF_INET;
    }

    [[nodiscard]]
    auto is_ipv6() const -> bool
    {
        return storage.general.sa_family == AF_INET6;
    }

    [[nodiscard]]
    auto port() const -> uint16_t
    {
        if (is_ipv4())
            return ntohs(storage.v4.sin_port);
        if (is_ipv6())
            return ntohs(storage.v6.sin6_port);
        return 0;
    }

    // --- String Conversion ---

    [[nodiscard]]
    auto to_string() const -> std::string
    {
        char buf[INET6_ADDRSTRLEN];

        if (is_ipv4())
        {
            if (!::inet_ntop(AF_INET, &storage.v4.sin_addr, buf, sizeof(buf)))
                return "";
            return std::format("{}:{}", buf, ntohs(storage.v4.sin_port));
        }

        if (is_ipv6())
        {
            if (!::inet_ntop(AF_INET, &storage.v4.sin_addr, buf, sizeof(buf)))
                return "";
            return std::format("[{}]:{}", buf, ntohs(storage.v6.sin6_port));
        }

        return "unknown";
    }

    // --- Low-level access ---

    [[nodiscard]]
    auto data() -> sockaddr *
    {
        return &storage.general;
    }

    [[nodiscard]]
    auto data() const -> const sockaddr *
    {
        return &storage.general;
    }

    [[nodiscard]]
    auto size() const -> socklen_t
    {
        return len;
    }
};
}  // namespace rio

template <>
struct std::formatter<rio::address> : std::formatter<std::string>
{
    auto format(const rio::address &addr, std::format_context &ctx) const
    {
        return std::formatter<std::string>::format(addr.to_string(), ctx);
    }
};
