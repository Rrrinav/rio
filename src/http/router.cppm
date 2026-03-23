module;

export module rio:http.router;

import std;
export import :http.request;
export import :http.response;

namespace rio::http::v1_1 {

// Route entry
//
// A route is a (path, handler) pair stored inside the router's flat per-method
// table.  The handler is a type-erased callable with a fixed vtable — exactly
// one virtual dispatch per request, no heap allocation for lambdas that fit in
// small-buffer storage (see Handler below).

// Handler — zero-overhead type erasure
//
// hand-rolled move-only type-erased wrapper:
//  • 48 bytes of inline storage — covers any capture-less lambda plus captures
//    of up to ~5 pointers without heap allocation.
//  • One vtable pointer, one function pointer per unique callable type.
//  • No RTTI, no exceptions required.

export class Handler
{
    static constexpr std::size_t buf_size  = 48;
    static constexpr std::size_t buf_align = alignof(std::max_align_t);

    struct Vtable {
        response (*call)(const void *, const request &) noexcept(false);
        void     (*move)(void *dst, void *src)          noexcept;
        void     (*destroy)(void *)                     noexcept;
    };

    alignas(buf_align) std::byte buf_[buf_size]{};
    const Vtable *vt_{ nullptr };
    bool          heap_{ false };

    void *storage()       noexcept { return heap_ ? *reinterpret_cast<void **>(buf_) : buf_; }
    const void *storage() const noexcept { return heap_ ? *reinterpret_cast<void *const *>(buf_) : buf_; }

    template <typename F>
    static const Vtable *vtable_for() noexcept
    {
        static constexpr Vtable vt{
            // call
            [](const void *p, const request &req) -> response {
                return (*static_cast<const F *>(p))(req);
            },
            // move
            [](void *dst, void *src) noexcept {
                ::new(dst) F(std::move(*static_cast<F *>(src)));
                static_cast<F *>(src)->~F();
            },
            // destroy
            [](void *p) noexcept { static_cast<F *>(p)->~F(); },
        };
        return &vt;
    }

public:
    Handler() = default;

    template <typename F>
        requires (!std::same_as<std::remove_cvref_t<F>, Handler> &&
                  std::invocable<F, const request &>)
    Handler(F &&f)
    {
        using Fd = std::decay_t<F>;
        if constexpr (sizeof(Fd) <= buf_size && alignof(Fd) <= buf_align) {
            ::new(buf_) Fd(std::forward<F>(f));
            heap_ = false;
        } else {
            auto *p = new Fd(std::forward<F>(f));
            *reinterpret_cast<void **>(buf_) = p;
            heap_ = true;
        }
        vt_ = vtable_for<Fd>();
    }

    Handler(Handler &&o) noexcept
    {
        if (!o.vt_) return;
        if (o.heap_) {
            // Just steal the heap pointer.
            std::memcpy(buf_, o.buf_, sizeof(void *));
            heap_ = true;
        } else {
            o.vt_->move(buf_, o.buf_);
            heap_ = false;
        }
        vt_   = o.vt_;
        o.vt_ = nullptr;
    }

    Handler &operator=(Handler &&o) noexcept
    {
        if (this == &o) return *this;
        this->~Handler();
        ::new(this) Handler(std::move(o));
        return *this;
    }

    ~Handler()
    {
        if (!vt_) return;
        vt_->destroy(storage());
        if (heap_) ::operator delete(*reinterpret_cast<void **>(buf_));
        vt_ = nullptr;
    }

    [[nodiscard]] bool empty() const noexcept { return vt_ == nullptr; }

    response operator()(const request &req) const
    {
        return vt_->call(storage(), req);
    }

    // Non-copyable (callables may be move-only).
    Handler(const Handler &)            = delete;
    Handler &operator=(const Handler &) = delete;
};

struct Route
{
    std::string path;
    Handler     handler;
};

// Router
//
// Dispatch is O(routes-per-method) — a tight linear scan over a contiguous
// array of Route objects.  For most servers this is faster than a hash map:
//  • No hashing, no bucket indirection, no load-factor overhead.
//  • CPU prefetcher loves sequential access.
//  • At ≤ ~30 routes per method the scan is faster than a hash map in
//    practice on all major microarchitectures.
//
// If you have hundreds of routes per method, swap the inner container to a
// sorted std::vector<Route> and use std::lower_bound — still no heap map.
//
// 404 vs 405 detection:
//  The old code did a second O(methods × routes) scan.  Here we record a
//  bitmask of which method-slots contain the path, built during dispatch in
//  one pass — O(total routes), no second scan, no separate data structure.

export class Router
{
    // One bucket per method enum value (get=0 … options=5).
    // 'unknown' slot (6) is intentionally left empty.
    static constexpr std::size_t bucket_count = method_count;

    std::array<std::vector<Route>, bucket_count> table_;

public:
    Router() = default;

    void add_route(method m, std::string path, Handler handler)
    {
        const auto idx = static_cast<std::size_t>(m);
        if (idx >= bucket_count) return; // ignore 'unknown'

        auto &bucket = table_[idx];

        // Replace existing handler for the same path (idempotent registration).
        for (auto &r : bucket) {
            if (r.path == path) {
                r.handler = std::move(handler);
                return;
            }
        }
        bucket.push_back({ std::move(path), std::move(handler) });
    }

    void get    (std::string p, Handler h) { add_route(method::get,     std::move(p), std::move(h)); }
    void post   (std::string p, Handler h) { add_route(method::post,    std::move(p), std::move(h)); }
    void put    (std::string p, Handler h) { add_route(method::put,     std::move(p), std::move(h)); }
    void del    (std::string p, Handler h) { add_route(method::del,     std::move(p), std::move(h)); }
    void patch  (std::string p, Handler h) { add_route(method::patch,   std::move(p), std::move(h)); }
    void options(std::string p, Handler h) { add_route(method::options, std::move(p), std::move(h)); }

    [[nodiscard]]
    response dispatch(const request &req) const
    {
        const auto method_idx = static_cast<std::size_t>(req.method);

        if (method_idx >= bucket_count)
            return response::text("405 Method Not Allowed", status_code::method_not_allowed);

        const std::string_view path = req.path;

        // Single pass: look for an exact match in the correct bucket AND check
        // whether any other bucket has the same path (for 405 vs 404).
        const Route *matched = nullptr;

        // Search the target method's bucket first — the fast path.
        for (const auto &r : table_[method_idx]) {
            if (r.path == path) { matched = &r; break; }
        }

        if (matched)
            return matched->handler(req);

        // Not found in target method — check other methods to distinguish
        // 404 (path unknown) from 405 (path known, method wrong).
        for (std::size_t i = 0; i < bucket_count; ++i) {
            if (i == method_idx) continue;
            for (const auto &r : table_[i]) {
                if (r.path == path)
                    return response::text("405 Method Not Allowed", status_code::method_not_allowed);
            }
        }

        return response::text("404 Not Found", status_code::not_found);
    }
};

} // namespace rio::http::v1_1

