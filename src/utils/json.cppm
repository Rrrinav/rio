module;

export module rio:utils.json;

import std;

/*
 * HOW TO USE THIS LIBRARY:
 * Simply import this module into your translation unit:
 * import rio:utils.json;
 *
 * FEATURES:
 * - C++20 Concepts and std::format integration.
 * - Safe extraction via the `View` type.
 * - Ergonomic JSON building context.
 * - High-performance zero-allocation string formatting and number parsing.
 *
 * =====================================================================================
 * PARSING AND DATA EXTRACTION
 * =====================================================================================
 * The `rio::jsn::parse` function takes a `std::string_view` and returns a
 * `rio::jsn::Json` variant (Object or Array). It throws on syntax errors.
 *
 * To read data safely without throwing exceptions on missing keys or out-of-bounds
 * indices, wrap the parsed JSON in a `rio::jsn::View`.
 *
 * EXAMPLE:
 * std::string_view raw_data = R"({ "server": "localhost", "ports": [80, 443] })";
 * rio::jsn::Json my_json = rio::jsn::parse(raw_data);
 * rio::jsn::View v = rio::jsn::view(my_json);
 *
 * // Safe extraction with fallbacks
 * std::string host = v["server"].as_or<std::string>("127.0.0.1");
 * double first_port = v["ports"][0].as_or<double>(8080.0);
 *
 * =====================================================================================
 * BUILDING JSON
 * =====================================================================================
 * Use `rio::jsn::Context` to programmatically build JSON. The API is heavily fluent.
 *
 * EXAMPLE:
 * rio::jsn::Context ctx;
 * ctx.obj_b()
 * .obj_k("status").inject("success")
 * .obj_k("data").arr_b()
 * .inject(10)
 * .arr_e()
 * .obj_e();
 *
 * rio::jsn::Json built_json = ctx.get();
 *
 * =====================================================================================
 * PRINTING AND FORMATTING
 * =====================================================================================
 * The library natively specializes `std::formatter` for `rio::jsn::Json`.
 * Custom Format Specifiers:
 * - [N]w : Indentation width. default is `4w`. Use `0w` for strict minified output.
 * - [N]f : Format doubles with fixed decimal places (e.g., `2f` for 3.14).
 * - [N]g : Format doubles with significant figures.
 * - E    : Disable string escaping (raw strings).
 * =====================================================================================
 */

namespace rio::jsn {

// ---- Error Management ----

export namespace errors {
inline std::string mismatched_obj_end = "[RIO: ERR] Mis-matched obj_e(): no active object to close.";
inline std::string mismatched_arr_end = "[RIO: ERR] Mis-matched arr_e(): no active array to close.";
inline std::string no_active_obj_for_key = "[RIO: ERR] No active object to assign a key to.";
inline std::string no_active_obj_for_val = "[RIO: ERR] No active object to assign a value to.";
inline std::string missing_key = "[RIO: ERR] Missing key for the current value.";
inline std::string no_active_arr_for_val = "[RIO: ERR] No active array to assign a value to.";
inline std::string unclosed_containers = "[RIO: ERR] Cannot get JSON: there are unclosed containers.";
inline std::string missing_key_nested = "[RIO: ERR] Missing key for nested container.";
inline std::string no_active_container = "[RIO: ERR] Cannot inject value: no active container.";

inline std::string parse_unexpected_eof = "[RIO: ERR] Parse error: unexpected end of file.";
inline std::string parse_unexpected_tok = "[RIO: ERR] Parse error: unexpected token.";
inline std::string parse_expected_colon = "[RIO: ERR] Parse error: expected ':' after key.";
inline std::string parse_expected_key = "[RIO: ERR] Parse error: expected string key in object.";
inline std::string parse_expected_root = "[RIO: ERR] Parse error: root must be an object or array.";
inline std::string parse_invalid_number = "[RIO: ERR] Parse error: invalid number format.";

inline std::string type_mismatch = "[RIO: ERR] Access error: type mismatch.";
inline std::string key_not_found = "[RIO: ERR] Access error: key not found.";
inline std::string index_out_of_bounds = "[RIO: ERR] Access error: array index out of bounds.";
inline std::string not_an_object = "[RIO: ERR] Access error: container is not an object.";
inline std::string not_an_array = "[RIO: ERR] Access error: container is not an array.";
inline std::string invalid_view = "[RIO: ERR] Access error: view is invalid (key or index not found).";
} // namespace errors

// ---- Core Types ----

export struct Value;

export using Object = std::vector<std::pair<std::string, Value>>;
export using Array = std::vector<Value>;

export template <typename T>
concept Compatible_c = std::is_same_v<T, double> || std::is_same_v<T, bool> || std::is_same_v<T, std::string>
    || std::convertible_to<T, std::string> || std::is_same_v<T, Object> || std::is_same_v<T, Array> || std::is_same_v<T, std::nullptr_t>;

struct Value
{
    std::variant<double, bool, std::string, Object, Array, std::nullptr_t> inner;

    Value() : inner(nullptr)
    {}

    template <typename T>
        requires Compatible_c<std::decay_t<T>>
    Value(T &&v) : inner(std::forward<T>(v))
    {}

    template <typename T>
    auto is() const -> bool
    {
        return std::holds_alternative<T>(inner);
    }

    template <typename T>
    auto get() const -> const T &
    {
        if (const auto *p = std::get_if<T>(&inner))
            return *p;
        throw std::runtime_error(errors::type_mismatch);
    }

    template <typename T>
    auto get_optional() const -> std::optional<T>
    {
        if (const auto *p = std::get_if<T>(&inner))
            return *p;
        return std::nullopt;
    }
};

export using Json = std::variant<Object, Array>;

// ---- Safe Extraction View ----

export struct View
{
    const Value *v_ptr = nullptr;
    const Json *j_ptr = nullptr;

    View() = default;
    View(const Value *v) : v_ptr(v)
    {}
    View(const Json *j) : j_ptr(j)
    {}

    auto is_valid() const -> bool
    {
        return v_ptr != nullptr || j_ptr != nullptr;
    }

    explicit operator bool() const
    {
        return is_valid();
    }

    auto operator[](std::string_view key) const -> View
    {
        if (v_ptr) {
            if (const auto *obj = std::get_if<Object>(&v_ptr->inner)) {
                for (const auto &pair : *obj) {
                    if (pair.first == key)
                        return View(&pair.second);
                }
            }
        } else if (j_ptr) {
            if (const auto *obj = std::get_if<Object>(j_ptr)) {
                for (const auto &pair : *obj) {
                    if (pair.first == key)
                        return View(&pair.second);
                }
            }
        }
        return View();
    }

    auto operator[](std::size_t index) const -> View
    {
        if (v_ptr) {
            if (const auto *arr = std::get_if<Array>(&v_ptr->inner)) {
                if (index < arr->size())
                    return View(&(*arr)[index]);
            }
        } else if (j_ptr) {
            if (const auto *arr = std::get_if<Array>(j_ptr)) {
                if (index < arr->size())
                    return View(&(*arr)[index]);
            }
        }
        return View();
    }

    template <typename K, typename... Rest>
    auto at(const K &key, const Rest &...rest) const -> View
    {
        View next = (*this)[key];
        if constexpr (sizeof...(rest) > 0) {
            return next.at(rest...);
        } else {
            return next;
        }
    }

    template <typename T>
    auto as() const -> std::optional<T>
    {
        if (v_ptr) {
            if (const auto *val = std::get_if<T>(&v_ptr->inner))
                return *val;
        }
        return std::nullopt;
    }

    template <typename T>
    auto as_or(T fallback) const -> T
    {
        return as<T>().value_or(fallback);
    }

    template <typename T>
    auto get() const -> T
    {
        if (!is_valid())
            throw std::runtime_error(errors::invalid_view);
        if (v_ptr) {
            if (const auto *val = std::get_if<T>(&v_ptr->inner))
                return *val;
            throw std::runtime_error(errors::type_mismatch);
        }
        throw std::runtime_error(errors::type_mismatch);
    }
};

export inline auto view(const Json &j) -> View
{
    return View(&j);
}

// ---- Internal Engine Declarations ----

namespace detail {

constexpr inline int num_idx = 0;
constexpr inline int bol_idx = 1;
constexpr inline int str_idx = 2;
constexpr inline int obj_idx = 3;
constexpr inline int arr_idx = 4;
constexpr inline int nul_idx = 5;

enum class Precision_mode { shortest, sig_figs, decimal_places };

struct Format_spec
{
    int precision = -1;
    Precision_mode precision_mode = Precision_mode::shortest;
    int indent_size = 4;
    bool escape_str = true;
};

static constexpr auto parse_spec(auto &ctx, Format_spec &spec)
{
    auto it = ctx.begin();

    auto parse_int = [&]() {
        int n = 0;
        while (it != ctx.end() && (*it >= '0' && *it <= '9')) {
            n = n * 10 + (*it++ - '0');
        }
        return n;
    };

    while (it != ctx.end() && *it != '}') {
        if (*it >= '0' && *it <= '9') {
            int n = parse_int();
            if (it != ctx.end()) {
                if (*it == 'g') {
                    spec.precision = n;
                    spec.precision_mode = Precision_mode::sig_figs;
                    ++it;
                } else if (*it == 'f') {
                    spec.precision = n;
                    spec.precision_mode = Precision_mode::decimal_places;
                    ++it;
                } else if (*it == 'w') {
                    spec.indent_size = n;
                    ++it;
                }
            }
        } else if (*it == 'e' || *it == 'E') {
            spec.escape_str = (*it == 'e');
            ++it;
        } else {
            ++it;
        }
    }
    return it;
}

void make_indent(const Format_spec &spec, int level, std::string &out);
void escape_to(std::string_view s, std::string &out);
void maybe_escape_to(std::string_view s, const Format_spec &spec, std::string &out);
void format_double(double v, const Format_spec &spec, std::string &out);
void format_object(const Object &obj, const Format_spec &spec, int level, std::string &out);
void format_array(const Array &arr, const Format_spec &spec, int level, std::string &out);
void format_value(const Value &v, const Format_spec &spec, int level, std::string &out);
std::string format_json(const Json &json, const Format_spec &spec);
std::string format_value_root(const Value &v, const Format_spec &spec);

struct Frame
{
    Json container;
    std::string pending_key{};
};

enum class Token_type {
    obj_open,
    obj_close,
    arr_open,
    arr_close,
    colon,
    comma,
    string_val,
    number_val,
    bool_true,
    bool_false,
    null_val,
    eof,
    error
};

struct Token
{
    Token_type type;
    std::string_view value;
};

class Scanner
{
    std::string_view src;
    std::size_t pos{0};

    auto skip_whitespace() -> void;
    auto peek() const -> char;
    auto advance() -> char;
    auto parse_string(std::size_t start) -> Token;
    auto parse_number(std::size_t start) -> Token;
    auto parse_literal(std::size_t start) -> Token;

public:
    explicit Scanner(std::string_view source) : src{source}
    {}
    auto next() -> Token;
};

class Parser
{
    Scanner scanner;
    Token current_token;

    auto advance() -> void;
    auto unescape_string(std::string_view raw) -> std::string;
    auto parse_value() -> Value;
    auto parse_object() -> Object;
    auto parse_array() -> Array;

public:
    explicit Parser(std::string_view src) : scanner(src)
    {
        advance();
    }
    auto parse() -> Json;
};

} // namespace detail

// ---- Top-Level API ----

export auto parse(std::string_view src) -> Json;

export struct Context;

export template <typename T>
concept Iterable_c = requires(T t) {
    std::begin(t);
    std::end(t);
} && !std::convertible_to<std::decay_t<T>, std::string>;

export template <Iterable_c T>
void to_json(Context &ctx, const T &container);

struct Context
{
    Json root{};
    std::vector<detail::Frame> stack{};

    auto obj_b() -> Context &;
    auto arr_b() -> Context &;
    auto obj_e() -> Context &;
    auto arr_e() -> Context &;
    auto obj_k(const std::string &k) -> Context &;
    auto obj_v(Value v) -> Context &;
    auto obj_kv(const std::string &k, Value v) -> Context &;
    auto arr_v(Value v) -> Context &;

    template <typename... Args>
    auto arr_vs(Args &&...args) -> Context &
    {
        (arr_v(std::forward<Args>(args)), ...);
        return *this;
    }

    auto is_in_object() const -> bool;
    auto is_in_array() const -> bool;

    template <typename T>
    auto inject(const T &val) -> Context &
    {
        using Decayed_t = std::decay_t<T>;

        if constexpr (Compatible_c<Decayed_t>) {
            if (is_in_object())
                return obj_v(val);
            else if (is_in_array())
                return arr_v(val);
            else
                throw std::runtime_error(errors::no_active_container);
        } else if constexpr (std::is_integral_v<Decayed_t> && !std::is_same_v<Decayed_t, bool>) {
            if (is_in_object())
                return obj_v(static_cast<double>(val));
            else if (is_in_array())
                return arr_v(static_cast<double>(val));
            else
                throw std::runtime_error(errors::no_active_container);
        } else {
            to_json(*this, val);
            return *this;
        }
    }

    auto get() -> Json;

private:
    auto close_container() -> void;
};

template <Iterable_c T>
void to_json(Context &ctx, const T &container)
{
    ctx.arr_b();
    for (const auto &item : container)
        ctx.inject(item);
    ctx.arr_e();
}

} // namespace rio::jsn

// ---- std::formatter Specializations ----

export template <>
struct std::formatter<rio::jsn::Value>
{
    rio::jsn::detail::Format_spec spec;

    constexpr auto parse(std::format_parse_context &ctx)
    {
        return rio::jsn::detail::parse_spec(ctx, spec);
    }

    auto format(const rio::jsn::Value &v, std::format_context &ctx) const
    {
        return std::format_to(ctx.out(), "{}", rio::jsn::detail::format_value_root(v, spec));
    }
};

export template <>
struct std::formatter<rio::jsn::Json>
{
    rio::jsn::detail::Format_spec spec;

    constexpr auto parse(std::format_parse_context &ctx)
    {
        return rio::jsn::detail::parse_spec(ctx, spec);
    }

    auto format(const rio::jsn::Json &json, std::format_context &ctx) const
    {
        return std::format_to(ctx.out(), "{}", rio::jsn::detail::format_json(json, spec));
    }
};

// =====================================================================================
// IMPLEMENTATION
// =====================================================================================

namespace rio::jsn::detail {

void make_indent(const Format_spec &spec, int level, std::string &out)
{
    if (spec.indent_size > 0)
        out.append(spec.indent_size * level, ' ');
}

void escape_to(std::string_view s, std::string &out)
{
    for (char c : s) {
        switch (c) {
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        default:
            out += c;
        }
    }
}

void maybe_escape_to(std::string_view s, const Format_spec &spec, std::string &out)
{
    if (!spec.escape_str)
        out += s;
    else
        escape_to(s, out);
}

void format_double(double v, const Format_spec &spec, std::string &out)
{
    std::size_t start_len = out.size();
    switch (spec.precision_mode) {
    case Precision_mode::shortest:
        std::format_to(std::back_inserter(out), "{}", v);
        break;
    case Precision_mode::sig_figs:
        std::format_to(std::back_inserter(out), "{:.{}g}", v, spec.precision);
        break;
    case Precision_mode::decimal_places:
        std::format_to(std::back_inserter(out), "{:.{}f}", v, spec.precision);
        break;
    }

    if (spec.precision_mode != Precision_mode::decimal_places) {
        std::string_view added(out.data() + start_len, out.size() - start_len);
        if (added.find('.') == std::string_view::npos && added.find('e') == std::string_view::npos)
            out += ".0";
    }
}

void format_object(const Object &obj, const Format_spec &spec, int level, std::string &out)
{
    if (obj.empty()) {
        out += "{}";
        return;
    }

    out += (spec.indent_size > 0) ? "{\n" : "{";
    for (std::size_t i = 0; i < obj.size(); ++i) {
        make_indent(spec, level, out);
        out += '"';
        maybe_escape_to(obj[i].first, spec, out);
        out += (spec.indent_size > 0) ? "\": " : "\":";
        format_value(obj[i].second, spec, level, out);

        if (i + 1 < obj.size())
            out += ',';

        if (spec.indent_size > 0)
            out += '\n';
    }
    make_indent(spec, level - 1, out);
    out += '}';
}

void format_array(const Array &arr, const Format_spec &spec, int level, std::string &out)
{
    if (arr.empty()) {
        out += "[]";
        return;
    }

    out += (spec.indent_size > 0) ? "[\n" : "[";
    for (std::size_t i = 0; i < arr.size(); ++i) {
        make_indent(spec, level, out);
        format_value(arr[i], spec, level, out);

        if (i + 1 < arr.size())
            out += ',';

        if (spec.indent_size > 0)
            out += '\n';
    }
    make_indent(spec, level - 1, out);
    out += ']';
}

void format_value(const Value &v, const Format_spec &spec, int level, std::string &out)
{
    switch (v.inner.index()) {
    case num_idx:
        format_double(std::get<num_idx>(v.inner), spec, out);
        break;
    case bol_idx:
        out += std::get<bol_idx>(v.inner) ? "true" : "false";
        break;
    case str_idx:
        out += '"';
        maybe_escape_to(std::get<str_idx>(v.inner), spec, out);
        out += '"';
        break;
    case obj_idx:
        format_object(std::get<obj_idx>(v.inner), spec, level + 1, out);
        break;
    case arr_idx:
        format_array(std::get<arr_idx>(v.inner), spec, level + 1, out);
        break;
    case nul_idx:
    default:
        out += "null";
        break;
    }
}

std::string format_json(const Json &json, const Format_spec &spec)
{
    std::string out;
    out.reserve(4096);
    std::visit(
        [&](const auto &v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Object>)
                format_object(v, spec, 1, out);
            else
                format_array(v, spec, 1, out);
        },
        json);
    return out;
}

std::string format_value_root(const Value &v, const Format_spec &spec)
{
    std::string out;
    out.reserve(1024);
    format_value(v, spec, 0, out);
    return out;
}

auto Scanner::skip_whitespace() -> void
{
    while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos]))) {
        ++pos;
    }
}

auto Scanner::peek() const -> char
{
    return pos < src.size() ? src[pos] : '\0';
}

auto Scanner::advance() -> char
{
    return pos < src.size() ? src[pos++] : '\0';
}

auto Scanner::parse_string(std::size_t start) -> Token
{
    while (pos < src.size()) {
        char c = advance();
        if (c == '"')
            return {Token_type::string_val, src.substr(start, pos - start)};
        if (c == '\\' && pos < src.size())
            advance();
    }
    return {Token_type::error, src.substr(start)};
}

auto Scanner::parse_number(std::size_t start) -> Token
{
    while (pos < src.size()) {
        char c = peek();
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E') {
            advance();
        } else {
            break;
        }
    }
    return {Token_type::number_val, src.substr(start, pos - start)};
}

auto Scanner::parse_literal(std::size_t start) -> Token
{
    while (pos < src.size() && std::isalpha(static_cast<unsigned char>(peek())))
        advance();
    std::string_view val = src.substr(start, pos - start);
    if (val == "true")
        return {Token_type::bool_true, val};
    if (val == "false")
        return {Token_type::bool_false, val};
    if (val == "null")
        return {Token_type::null_val, val};
    return {Token_type::error, val};
}

auto Scanner::next() -> Token
{
    skip_whitespace();
    if (pos >= src.size())
        return {Token_type::eof, ""};

    const std::size_t start = pos;
    const char c = advance();

    switch (c) {
    case '{':
        return {Token_type::obj_open, src.substr(start, 1)};
    case '}':
        return {Token_type::obj_close, src.substr(start, 1)};
    case '[':
        return {Token_type::arr_open, src.substr(start, 1)};
    case ']':
        return {Token_type::arr_close, src.substr(start, 1)};
    case ':':
        return {Token_type::colon, src.substr(start, 1)};
    case ',':
        return {Token_type::comma, src.substr(start, 1)};
    case '"':
        return parse_string(start);
    default:
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c)))
            return parse_number(start);
        if (std::isalpha(static_cast<unsigned char>(c)))
            return parse_literal(start);
        return {Token_type::error, src.substr(start, 1)};
    }
}

auto Parser::advance() -> void
{
    current_token = scanner.next();
}

auto Parser::unescape_string(std::string_view raw) -> std::string
{
    std::string res;
    res.reserve(raw.size());
    for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 2 < raw.size()) {
            ++i;
            switch (raw[i]) {
            case 'n':
                res += '\n';
                break;
            case 't':
                res += '\t';
                break;
            case 'r':
                res += '\r';
                break;
            case '"':
                res += '"';
                break;
            case '\\':
                res += '\\';
                break;
            default:
                res += raw[i];
                break;
            }
        } else {
            res += raw[i];
        }
    }
    return res;
}

auto Parser::parse_value() -> Value
{
    switch (current_token.type) {
    case Token_type::obj_open:
        return parse_object();
    case Token_type::arr_open:
        return parse_array();
    case Token_type::string_val: {
        std::string s = unescape_string(current_token.value);
        advance();
        return s;
    }
    case Token_type::number_val: {
        double d = 0.0;
        auto [ptr, ec] = std::from_chars(current_token.value.data(), current_token.value.data() + current_token.value.size(), d);
        if (ec != std::errc()) {
            throw std::runtime_error(errors::parse_invalid_number);
        }
        advance();
        return d;
    }
    case Token_type::bool_true:
        advance();
        return true;
    case Token_type::bool_false:
        advance();
        return false;
    case Token_type::null_val:
        advance();
        return nullptr;
    case Token_type::eof:
        throw std::runtime_error(errors::parse_unexpected_eof);
    default:
        throw std::runtime_error(errors::parse_unexpected_tok);
    }
}

auto Parser::parse_object() -> Object
{
    Object obj;
    advance();
    while (current_token.type != Token_type::obj_close) {
        if (current_token.type != Token_type::string_val)
            throw std::runtime_error(errors::parse_expected_key);
        std::string key = unescape_string(current_token.value);
        advance();

        if (current_token.type != Token_type::colon)
            throw std::runtime_error(errors::parse_expected_colon);
        advance();

        Value val = parse_value();
        obj.emplace_back(std::move(key), std::move(val));

        if (current_token.type == Token_type::comma)
            advance();
        else if (current_token.type != Token_type::obj_close)
            throw std::runtime_error(errors::parse_unexpected_tok);
    }
    advance();
    return obj;
}

auto Parser::parse_array() -> Array
{
    Array arr;
    advance();
    while (current_token.type != Token_type::arr_close) {
        arr.push_back(parse_value());
        if (current_token.type == Token_type::comma)
            advance();
        else if (current_token.type != Token_type::arr_close)
            throw std::runtime_error(errors::parse_unexpected_tok);
    }
    advance();
    return arr;
}

auto Parser::parse() -> Json
{
    if (current_token.type == Token_type::obj_open)
        return parse_object();
    else if (current_token.type == Token_type::arr_open)
        return parse_array();
    else
        throw std::runtime_error(errors::parse_expected_root);
}

} // namespace rio::jsn::detail

namespace rio::jsn {

auto parse(std::string_view src) -> Json
{
    detail::Parser parser(src);
    return parser.parse();
}

auto Context::obj_b() -> Context &
{
    stack.push_back(detail::Frame{Object{}, ""});
    return *this;
}

auto Context::arr_b() -> Context &
{
    stack.push_back(detail::Frame{Array{}, ""});
    return *this;
}

auto Context::obj_e() -> Context &
{
    if (stack.empty() || !std::holds_alternative<Object>(stack.back().container))
        throw std::runtime_error(errors::mismatched_obj_end);
    close_container();
    return *this;
}

auto Context::arr_e() -> Context &
{
    if (stack.empty() || !std::holds_alternative<Array>(stack.back().container))
        throw std::runtime_error(errors::mismatched_arr_end);
    close_container();
    return *this;
}

auto Context::obj_k(const std::string &k) -> Context &
{
    if (stack.empty() || !std::holds_alternative<Object>(stack.back().container))
        throw std::runtime_error(errors::no_active_obj_for_key);
    stack.back().pending_key = k;
    return *this;
}

auto Context::obj_v(Value v) -> Context &
{
    if (stack.empty() || !std::holds_alternative<Object>(stack.back().container))
        throw std::runtime_error(errors::no_active_obj_for_val);

    if (stack.back().pending_key.empty())
        throw std::runtime_error(errors::missing_key);

    std::get<Object>(stack.back().container).push_back({std::move(stack.back().pending_key), std::move(v)});
    stack.back().pending_key.clear();
    return *this;
}

auto Context::obj_kv(const std::string &k, Value v) -> Context &
{
    return obj_k(k).obj_v(std::move(v));
}

auto Context::arr_v(Value v) -> Context &
{
    if (stack.empty() || !std::holds_alternative<Array>(stack.back().container))
        throw std::runtime_error(errors::no_active_arr_for_val);

    std::get<Array>(stack.back().container).push_back(std::move(v));
    return *this;
}

auto Context::is_in_object() const -> bool
{
    return !stack.empty() && std::holds_alternative<Object>(stack.back().container);
}

auto Context::is_in_array() const -> bool
{
    return !stack.empty() && std::holds_alternative<Array>(stack.back().container);
}

auto Context::get() -> Json
{
    if (!stack.empty())
        throw std::runtime_error(errors::unclosed_containers);
    return root;
}

auto Context::close_container() -> void
{
    Json completed_container = std::move(stack.back().container);
    stack.pop_back();

    if (stack.empty()) {
        root = std::move(completed_container);
    } else {

        Value nested_value = std::visit([](auto &&arg) -> Value {
            return Value(std::forward<decltype(arg)>(arg));
        }, std::move(completed_container));

        if (std::holds_alternative<Object>(stack.back().container)) {
            if (stack.back().pending_key.empty())
                throw std::runtime_error(errors::missing_key_nested);

            std::get<Object>(stack.back().container).emplace_back(std::move(stack.back().pending_key), std::move(nested_value));
            stack.back().pending_key.clear();
        } else {
            std::get<Array>(stack.back().container).push_back(std::move(nested_value));
        }
    }
}

} // namespace rio::jsn
