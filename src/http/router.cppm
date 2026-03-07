module;

export module rio:http.router;

import std;
export import :http.request;
export import :http.response;

namespace rio::http::v1_1 {

export using Handler = std::function<response(const request &)>;

export class Router
{
private:
    std::array<std::unordered_map<std::string, Handler>, 7> routes_;

public:
    Router() = default;

    void add_route(method m, std::string path, Handler handler)
    {
        routes_[static_cast<std::size_t>(m)][std::move(path)] = std::move(handler);
    }

    void get(std::string path, Handler handler)
    {
        add_route(method::get, std::move(path), std::move(handler));
    }
    void post(std::string path, Handler handler)
    {
        add_route(method::post, std::move(path), std::move(handler));
    }
    void put(std::string path, Handler handler)
    {
        add_route(method::put, std::move(path), std::move(handler));
    }
    void del(std::string path, Handler handler)
    {
        add_route(method::del, std::move(path), std::move(handler));
    }
    void patch(std::string path, Handler handler)
    {
        add_route(method::patch, std::move(path), std::move(handler));
    }

    [[nodiscard]]
    response dispatch(const request &req) const
    {
        auto method_idx = static_cast<std::size_t>(req.method);

        if (method_idx >= routes_.size()) {
            return response::text("Method Not Allowed", status_code::method_not_allowed);
        }

        const auto &method_map = routes_[method_idx];
        auto it = method_map.find(req.path);

        if (it != method_map.end()) {
            return it->second(req);
        }

        for (std::size_t i = 0; i < routes_.size(); ++i) {
            if (i == method_idx)
                continue;

            if (routes_[i].contains(req.path)) {
                return response::text("405 Method Not Allowed - rio", status_code::method_not_allowed);
            }
        }

        // Truly not found anywhere
        return response::text("404 Not Found - rio", status_code::not_found);
    }
};

} // namespace rio::http::v1_1
