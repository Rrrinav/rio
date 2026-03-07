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
    not_implemented       = 501
};

export constexpr auto reason_phrase(status_code code) -> std::string_view
{
    switch (code) {
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
    std::vector<header> headers{};
    std::string body{};

    // Fluent header builder
    auto set_header(std::string name, std::string value) -> response &
    {
        for (auto &h : headers) {
            if (h.name == name) {
                h.value = std::move(value);
                return *this;
            }
        }
        headers.push_back({std::move(name), std::move(value)});
        return *this;
    }

    // Convert the entire object into a raw HTTP wire string
    [[nodiscard]]
    auto to_string() const -> std::string
    {
        std::string out;
        // Pre-allocate a reasonable size to avoid reallocations
        out.reserve(128 + body.size());

        // 1. Status line
        std::format_to(std::back_inserter(out), "HTTP/1.1 {} {}\r\n", static_cast<std::uint16_t>(code), reason_phrase(code));

        // 2. Headers
        bool has_content_length = false;
        for (const auto &h : headers) {
            std::format_to(std::back_inserter(out), "{}: {}\r\n", h.name, h.value);
            // In a real server, you'd make this case-insensitive
            if (h.name == "Content-Length")
                has_content_length = true;
        }

        // Automatically inject Content-Length if the user forgot it
        if (!has_content_length) {
            std::format_to(std::back_inserter(out), "Content-Length: {}\r\n", body.size());
        }

        // 3. Blank line to end headers, then the body
        out += "\r\n";
        out += body;

        return out;
    }

    // --- Static Factory Methods ---

    static auto empty(status_code c = status_code::no_content) -> response
    {
        return response{.code = c};
    }

    static auto text(std::string_view txt, status_code c = status_code::ok) -> response
    {
        response res{.code = c, .body = std::string(txt)};
        res.set_header("Content-Type", "text/plain");
        return res;
    }

    // Pass a rio::jsn::Json directly. It automatically formats it!
    static auto json(const rio::jsn::Json &j, status_code c = status_code::ok) -> response
    {
        response res{.code = c, .body = std::format("{}", j)};
        res.set_header("Content-Type", "application/json");
        return res;
    }

    // For passing raw pre-formatted JSON strings
    static auto json(std::string raw_json, status_code c = status_code::ok) -> response
    {
        response res{.code = c, .body = std::move(raw_json)};
        res.set_header("Content-Type", "application/json");
        return res;
    }
};

} // namespace rio::http::v1_1
