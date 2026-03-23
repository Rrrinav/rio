module;

export module rio:http.parser;

import std;
import :futures;
import :buff_reader_async;
export import :http.request;
import :utils.misc;

namespace rio::http::v1_1 {

//  Error category

export enum class Parse_error : std::uint8_t {
    invalid_request_line = 1,
    malformed_header = 2,
    content_length_mismatch = 3,
    unsupported_transfer_encoding = 4,
};

struct Http_error_category final : std::error_category
{
    const char *name() const noexcept override
    {
        return "http_parse";
    }

    std::string message(int ev) const override
    {
        switch (static_cast<Parse_error>(ev)) {
        case Parse_error::invalid_request_line:
            return "Invalid HTTP request line";
        case Parse_error::malformed_header:
            return "Malformed HTTP header";
        case Parse_error::content_length_mismatch:
            return "Body size does not match Content-Length";
        case Parse_error::unsupported_transfer_encoding:
            return "Unsupported Transfer-Encoding";
        default:
            return "Unknown HTTP parse error";
        }
    }
};

// Single instance — error_category must have static lifetime.
inline const Http_error_category http_error_cat{};

inline std::error_code make_error_code(Parse_error e) noexcept
{
    return {static_cast<int>(e), http_error_cat};
}

} // namespace rio::http::v1_1

template <>
struct std::is_error_code_enum<rio::http::v1_1::Parse_error> : std::true_type
{};

namespace rio::http::v1_1 {

//  Method parser
// Compares against sorted lengths first so we bail out of the switch before
// doing any string comparison for obviously wrong lengths.

export constexpr auto parse_method(std::string_view m) noexcept -> method
{
    switch (m.size()) {
    case 3:
        if (m == "GET")
            return method::get;
        if (m == "PUT")
            return method::put;
        break;
    case 4:
        if (m == "POST")
            return method::post;
        break;
    case 5:
        if (m == "PATCH")
            return method::patch;
        break;
    case 6:
        if (m == "DELETE")
            return method::del;
        break;
    case 7:
        if (m == "OPTIONS")
            return method::options;
        break;
    }
    return method::unknown;
}

//  Parser state machine
//
// Key zero-cost choices:
//  • All string fields on 'request' that survive only within one request cycle
//    (path, version, header name/value) are std::string_view pointing into
//    the underlying read buffer.  Zero allocation, zero copy.
//  • Body is the only field that copies bytes — it must own them because the
//    buffer window may slide before the handler runs.
//  • content_length is parsed with std::from_chars — no locale, no allocation.
//  • The "is there a body?" branch is resolved once and stored in a bool so
//    the state machine never re-checks headers.

export template <typename Reader>
struct Parse_request_impl
{
    Reader *reader;
    request *req;

    Parse_state state{Parse_state::request_line};
    std::size_t content_length{0};
    bool has_body{false};

    std::optional<decltype(rio::fut::buff::read_till(*reader, '\n'))> curr_read_line{};
    std::optional<decltype(reader->peek())> curr_peek{};

    auto poll() -> rio::fut::res<void>
    {
        while (true) {

            //  REQUEST LINE / HEADERS
            if (state == Parse_state::request_line || state == Parse_state::headers) {
                if (!curr_read_line)
                    curr_read_line.emplace(rio::fut::buff::read_till(*reader, '\n'));

                auto r = curr_read_line->poll();
                if (r.state == rio::fut::status::pending)
                    return rio::fut::res<void>::pending();
                if (r.state == rio::fut::status::error)
                    return rio::fut::res<void>::error(r.err);

                auto line_opt = std::move(*r.value);
                curr_read_line.reset();

                if (!line_opt)
                    return rio::fut::res<void>::error(std::make_error_code(std::errc::connection_aborted));

                // trim() returns a string_view into the *same* buffer — no copy.
                const std::string_view line = rio::util::trim(*line_opt);

                if (state == Parse_state::request_line) {
                    // Parse:  METHOD SP request-target SP HTTP-version
                    const auto sp1 = line.find(' ');
                    if (sp1 == std::string_view::npos)
                        return rio::fut::res<void>::error(make_error_code(Parse_error::invalid_request_line));

                    const auto sp2 = line.find(' ', sp1 + 1);
                    if (sp2 == std::string_view::npos)
                        return rio::fut::res<void>::error(make_error_code(Parse_error::invalid_request_line));

                    req->method = parse_method(line.substr(0, sp1));
                    // path and version must be owned: the read buffer backing
                    // `line` will be overwritten by the next read_till call.
                    req->path.assign(line.data() + sp1 + 1, sp2 - sp1 - 1);
                    req->version.assign(line.data() + sp2 + 1);

                    state = Parse_state::headers;
                    continue;
                }

                // state == headers
                if (line.empty()) {
                    // End of header section — resolve body length once.
                    if (auto cl = req->get_header("Content-Length")) {
                        std::from_chars(cl->data(), cl->data() + cl->size(), content_length);
                        if (content_length > 0) {
                            req->body.reserve(content_length);
                            has_body = true;
                        }
                    }

                    if (auto te = req->get_header("Transfer-Encoding"); te && *te == "chunked") {
                        return rio::fut::res<void>::error(make_error_code(Parse_error::unsupported_transfer_encoding));
                    }

                    state = has_body ? Parse_state::body : Parse_state::complete;
                    continue;
                }

                // Regular header line — split on ": " (colon + space per RFC 7230).
                if (auto pair = rio::util::split_once(line, ": ")) {
                    // line_opt is a local destroyed at end of this iteration,
                    // so we must copy both name and value into owned strings.
                    req->push_header(std::string(pair->first), std::string(pair->second));
                } else {
                    return rio::fut::res<void>::error(make_error_code(Parse_error::malformed_header));
                }
                continue;
            }

            //  BODY
            if (state == Parse_state::body) {
                if (req->body.size() >= content_length) {
                    state = Parse_state::complete;
                    return rio::fut::res<void>::ready();
                }

                if (!curr_peek)
                    curr_peek.emplace(reader->peek());

                auto r = curr_peek->poll();
                if (r.state == rio::fut::status::pending)
                    return rio::fut::res<void>::pending();
                if (r.state == rio::fut::status::error)
                    return rio::fut::res<void>::error(r.err);

                const auto view = *r.value;
                curr_peek.reset();

                if (view.empty())
                    return rio::fut::res<void>::error(std::make_error_code(std::errc::connection_aborted));

                const std::size_t needed = content_length - req->body.size();
                const std::size_t take = std::min(needed, view.size());

                // Single memcpy into the pre-reserved string — no reallocation.
                req->body.append(reinterpret_cast<const char *>(view.data()), take);
                reader->advance(take);
                continue;
            }

            //  COMPLETE
            // state == Parse_state::complete
            return rio::fut::res<void>::ready();
        }
    }
};

export template <typename Reader>
auto parse_request(Reader &reader, request &req_out)
{
    return rio::fut::make(Parse_request_impl<Reader>{&reader, &req_out}, rio::fut::Call_poll{});
}

} // namespace rio::http::v1_1
