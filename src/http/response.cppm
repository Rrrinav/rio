module;

export module rio:http.response;

import std;
import :http.request;
import :utils.json;

namespace rio::http::v1_1 {

export enum class status_code : std::uint16_t {
    ok                    = 200,
    created               = 201,
    no_content            = 204,
    bad_request           = 400,
    unauthorized          = 401,
    forbidden             = 403,
    not_found             = 404,
    method_not_allowed    = 405,
    internal_server_error = 500,
    not_implemented       = 501,
};

export constexpr std::string_view reason_phrase(status_code c) noexcept
{
    switch (c) {
    case status_code::ok:
        return "OK";
    case status_code::created:
        return "Created";
    case status_code::no_content:
        return "No Content";
    case status_code::bad_request:
        return "Bad Request";
    case status_code::unauthorized:
        return "Unauthorized";
    case status_code::forbidden:
        return "Forbidden";
    case status_code::not_found:
        return "Not Found";
    case status_code::method_not_allowed:
        return "Method Not Allowed";
    case status_code::internal_server_error:
        return "Internal Server Error";
    case status_code::not_implemented:
        return "Not Implemented";
    default:
        return "Unknown";
    }
}

export struct response
{
    status_code code{status_code::ok};
    std::vector<std::pair<std::string, std::string>> headers{};
    std::string body{};

    auto &set_header(std::string name, std::string value) &
    {
        for (auto &[n, v] : headers) {
            if (n == name) {
                v = std::move(value);
                return *this;
            }
        }
        headers.emplace_back(std::move(name), std::move(value));
        return *this;
    }

    template <typename Buffer>
    void write_to(Buffer &out) const
    {
        // Status line
        std::format_to(std::back_inserter(out), "HTTP/1.1 {} {}\r\n", static_cast<std::uint16_t>(code), reason_phrase(code));

        // User headers
        bool has_content_length = false;
        for (const auto &[n, v] : headers) {
            std::format_to(std::back_inserter(out), "{}: {}\r\n", n, v);
            if (n == "Content-Length")
                has_content_length = true;
        }

        // Inject Content-Length if absent
        if (!has_content_length)
            std::format_to(std::back_inserter(out), "Content-Length: {}\r\n", body.size());

        // Header / body separator
        out.append("\r\n", 2);

        // Body
        out.append(body.data(), body.size());
    }

    // Convenience: produces an owned wire string.
    // Pre-allocates to avoid repeated reallocation.
    [[nodiscard]]
    std::string to_string() const
    {
        std::string out;
        out.reserve(128 + body.size());
        write_to(out);
        return out;
    }

    [[nodiscard]] static response empty(status_code c = status_code::no_content)
    {
        return response{.code = c};
    }

    [[nodiscard]] static response text(std::string_view txt, status_code c = status_code::ok)
    {
        response r{.code = c, .body = std::string(txt)};
        r.set_header("Content-Type", "text/plain");
        return r;
    }

    [[nodiscard]] static response json(const rio::jsn::Json &j, status_code c = status_code::ok)
    {
        response r{.code = c, .body = std::format("{}", j)};
        r.set_header("Content-Type", "application/json");
        return r;
    }

    [[nodiscard]] static response json(std::string raw, status_code c = status_code::ok)
    {
        response r{.code = c, .body = std::move(raw)};
        r.set_header("Content-Type", "application/json");
        return r;
    }
};

} // namespace rio::http::v1_1
