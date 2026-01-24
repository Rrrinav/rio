module;

export module rio:utils.defer;

import std;
import std.compat;

export namespace rio {

class Defer_stack
{
    using call_t = void(*)(void*) noexcept;
    using dtor_t = void(*)(void*) noexcept;
public:
    struct entry
    {
        call_t call;
        dtor_t destroy;
        void *obj;
    };
    std::vector<entry> entries;
    std::vector<std::byte> arena;

    ~Defer_stack()
    {
        for (auto it = entries.rbegin(); it != entries.rend(); ++it)
        {
            it->call(it->obj);
            it->destroy(it->obj);
        }
    }

    void reserve(std::size_t n_entries, std::size_t arena_bytes = 0)
    {
        entries.reserve(n_entries);
        if (arena_bytes)
            arena.reserve(arena_bytes);
    }

    template <class F>
    void defer(F &&f)
    {
        using Fn = std::decay_t<F>;

        static_assert(std::is_nothrow_move_constructible_v<Fn>, "Lambda must be noexcept.");

        constexpr std::size_t A = alignof(Fn);

        std::size_t pos = arena.size();
        std::size_t aligned = (pos + (A - 1)) & ~(A - 1);
        std::size_t need = aligned + sizeof(Fn);

        arena.resize(need);
        void *mem = arena.data() + aligned;
        Fn *obj = ::new (mem) Fn(std::forward<F>(f));

        entries.push_back(entry{
            .call = +[](void *p) noexcept { (*static_cast<Fn *>(p))(); },
            .destroy = +[](void *p) noexcept { static_cast<Fn *>(p)->~Fn(); },
            .obj = obj
        });
    }
};

template <class F>
class Scope_guard
{
    F f;
    bool active = true;

public:
    using func_type = F;

    Scope_guard(F &&fn) noexcept(std::is_nothrow_move_constructible_v<F>) : f(std::forward<F>(fn)) {}

    Scope_guard(const Scope_guard &) = delete;
    Scope_guard &operator=(const Scope_guard &) = delete;

    Scope_guard(Scope_guard &&other) noexcept(std::is_nothrow_move_constructible_v<F>) : f(std::move(other.f)), active(other.active)
    {
        other.active = false;
    }

    ~Scope_guard() noexcept
    {
        if (active)
            f();
    }

    void dismiss() noexcept { active = false; }
};

// CTAD helper
template <class F>
Scope_guard(F) -> Scope_guard<F>;

// helper function (nice API)
template <class F>
auto make_scope_guard(F &&f) noexcept(std::is_nothrow_move_constructible_v<std::decay_t<F>>)
{
    return Scope_guard<std::decay_t<F>>(std::forward<F>(f));
}

} //namespace rio
