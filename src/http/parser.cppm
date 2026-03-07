module;
export module rio:http.parser;

import std;
import :futures;
import :buff_reader_async;
export import :http.request;
import :utils.misc;

namespace rio::http::v1_1 {

// 1. A Real HTTP Error Category!
export enum class Parse_error { invalid_request_line = 1, malformed_header, content_length_mismatch, unsupported_transfer_encoding };

struct Http_error_category : std::error_category
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

inline const Http_error_category http_error_cat{};

inline std::error_code make_error_code(Parse_error e)
{
    return {static_cast<int>(e), http_error_cat};
}

} // namespace rio::http::v1_1

template <>
struct std::is_error_code_enum<rio::http::v1_1::Parse_error> : std::true_type
{};

namespace rio::http::v1_1 {

export auto parse_method(std::string_view m) -> method
{
    if (m == "GET")
        return method::get;
    if (m == "POST")
        return method::post;
    if (m == "PUT")
        return method::put;
    if (m == "DELETE")
        return method::del;
    if (m == "PATCH")
        return method::patch;
    if (m == "OPTIONS")
        return method::options;
    return method::unknown;
}

export template <typename Reader>
struct Parse_request_impl
{
    Reader *reader;
    request *req;

    Parse_state state{Parse_state::request_line};
    std::size_t content_length{0};

    // We store the two different futures we might need to poll
    std::optional<decltype(rio::fut::buff::read_till(*reader, '\n'))> curr_read_line{};
    std::optional<decltype(reader->peek())> curr_peek{};

    auto poll() -> rio::fut::res<void>
    {
        while (true) {

            // --- STATE: REQUEST LINE & HEADERS ---
            if (state == Parse_state::request_line || state == Parse_state::headers) {
                if (!curr_read_line) {
                    curr_read_line.emplace(rio::fut::buff::read_till(*reader, '\n'));
                }

                auto r = curr_read_line->poll();
                if (r.state == rio::fut::status::pending)
                    return rio::fut::res<void>::pending();
                if (r.state == rio::fut::status::error)
                    return rio::fut::res<void>::error(r.err);

                auto line_opt = std::move(*r.value);
                curr_read_line.reset();

                if (!line_opt)
                    return rio::fut::res<void>::error(std::make_error_code(std::errc::connection_aborted));

                std::string_view line = rio::util::trim(*line_opt);

                if (state == Parse_state::request_line) {
                    auto space1 = line.find(' ');
                    if (space1 != std::string_view::npos) {
                        auto space2 = line.find(' ', space1 + 1);
                        if (space2 != std::string_view::npos) {
                            req->method = parse_method(line.substr(0, space1));
                            req->path = std::string(line.substr(space1 + 1, space2 - space1 - 1));
                            req->version = std::string(line.substr(space2 + 1));

                            state = Parse_state::headers;
                            continue;
                        }
                    }
                    return rio::fut::res<void>::error(make_error_code(Parse_error::invalid_request_line));
                } else if (state == Parse_state::headers) {
                    if (line.empty()) {
                        // Check if we have a body
                        if (auto cl = req->get_header("Content-Length")) {
                            std::from_chars(cl->data(), cl->data() + cl->size(), content_length);
                            req->body.reserve(content_length);
                        }

                        if (auto te = req->get_header("Transfer-Encoding"); te && *te == "chunked") {
                            return rio::fut::res<void>::error(make_error_code(Parse_error::unsupported_transfer_encoding));
                        }

                        state = (content_length > 0) ? Parse_state::body : Parse_state::complete;
                        continue; // Proceed to body parsing immediately
                    } else {
                        if (auto pair = rio::util::split_once(line, ": ")) {
                            req->headers.emplace_back(std::string(pair->first), std::string(pair->second));
                        } else {
                            return rio::fut::res<void>::error(make_error_code(Parse_error::malformed_header));
                        }
                        continue;
                    }
                }
            }

            // --- STATE: BODY ---
            else if (state == Parse_state::body) {
                // If we've collected enough bytes, we are completely done!
                if (req->body.size() >= content_length) {
                    state = Parse_state::complete;
                    return rio::fut::res<void>::ready();
                }

                if (!curr_peek) {
                    curr_peek.emplace(reader->peek());
                }

                auto r = curr_peek->poll();
                if (r.state == rio::fut::status::pending)
                    return rio::fut::res<void>::pending();
                if (r.state == rio::fut::status::error)
                    return rio::fut::res<void>::error(r.err);

                auto view = *r.value;
                curr_peek.reset();

                if (view.empty()) {
                    return rio::fut::res<void>::error(std::make_error_code(std::errc::connection_aborted));
                }

                // Calculate how many bytes we still need
                std::size_t bytes_needed = content_length - req->body.size();
                std::size_t bytes_to_take = std::min(bytes_needed, view.size());

                // Append the raw memory to the string
                req->body.append(reinterpret_cast<const char *>(view.data()), bytes_to_take);

                // Advance the reader's cursor so we don't read the same bytes again
                reader->advance(bytes_to_take);

                continue; // Loop back around to check if we have reached content_length!
            }

            // --- STATE: COMPLETE ---
            else if (state == Parse_state::complete) {
                return rio::fut::res<void>::ready();
            }
        }
    }
};

export template <typename Reader>
auto parse_request(Reader &reader, request &req_out)
{
    return rio::fut::make(Parse_request_impl<Reader>{&reader, &req_out}, rio::fut::Call_poll{});
}

} // namespace rio::http::v1_1
