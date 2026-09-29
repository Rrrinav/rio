module;
export module rio:promise;

import std;
import :futures;

namespace rio {

template <typename S>
concept StateLike = requires(S *s, std::error_code ec) {
    { s->poll() } -> std::same_as<rio::fut::res<typename S::value_type>>;
    { s->reject(ec) };
};

export template <typename StateT>
struct Promise {
    StateT *state = nullptr;

    Promise() = default;
    explicit Promise(StateT *s) : state(s)
    {}

    // Non-owning observer over shared completion state: copies would
    // alias the same state and make double-settle invisible, so a
    // Promise moves but never copies.
    Promise(const Promise &) = delete;
    Promise &operator=(const Promise &) = delete;
    Promise(Promise &&other) noexcept : state(other.state)
    {
        other.state = nullptr;
    }
    Promise &operator=(Promise &&other) noexcept
    {
        state = other.state;
        other.state = nullptr;
        return *this;
    }

    explicit operator bool() const
    {
        return state != nullptr;
    }

    template <typename... Args>
    auto resolve(Args &&...args) -> bool
    {
        if (!state) {
            return false;
        }
        return state->resolve(std::forward<Args>(args)...);
    }

    auto reject(std::error_code ec) -> bool
    {
        if (!state) {
            return false;
        }
        return state->reject(ec);
    }
};

export namespace promise {
template <typename T>
struct State {
    using value_type = T;

    std::optional<T> value{};
    std::error_code error{};
    bool is_ready = false;

    // Set-once: a second settle is a logic error. It is ignored (and
    // reported as false) instead of silently overwriting the outcome.
    auto resolve(T v) -> bool
    {
        if (is_ready) {
            return false;
        }
        value.emplace(std::move(v));
        is_ready = true;
        return true;
    }

    auto reject(std::error_code ec) -> bool
    {
        if (is_ready) {
            return false;
        }
        error = ec;
        is_ready = true;
        return true;
    }

    auto poll() -> rio::fut::res<T>
    {
        if (is_ready) {
            if (value) {
                return rio::fut::res<T>::ready(std::move(*value));
            } else {
                return rio::fut::res<T>::error(error);
            }
        }
        return rio::fut::res<T>::pending();
    }
};

template <>
struct State<void> {
    using value_type = void;

    std::error_code error{};
    bool is_ready = false;

    // Set-once: see State<T>.
    auto resolve() -> bool
    {
        if (is_ready) {
            return false;
        }
        is_ready = true;
        return true;
    }

    auto reject(std::error_code ec) -> bool
    {
        if (is_ready) {
            return false;
        }
        error = ec;
        is_ready = true;
        return true;
    }

    auto poll() -> rio::fut::res<void>
    {
        if (is_ready) {
            if (error) {
                return rio::fut::res<void>::error(error);
            }
            return rio::fut::res<void>::ready();
        }
        return rio::fut::res<void>::pending();
    }
};
} // namespace promise

export namespace promise {

template <StateLike StateT>
auto bind(StateT &s)
{
    rio::Promise<StateT> p{&s};

    auto f = rio::fut::make(&s, rio::fut::Call_poll_ptr{});
    return std::pair{std::move(p), std::move(f)};
}

template <StateLike StateT>
auto bind(StateT *s)
{
    rio::Promise<StateT> p{s};

    auto f = rio::fut::make(s, rio::fut::Call_poll_ptr{});
    return std::pair{std::move(p), std::move(f)};
}

template <typename T>
auto make()
{
    auto shared_state = std::make_shared<State<T>>();

    rio::Promise<State<T>> p{shared_state.get()};
    auto f = rio::fut::make(std::move(shared_state), rio::fut::Call_poll_ptr{});
    return std::pair{p, std::move(f)};
}

template <typename T>
auto make_unique()
{
    auto unique_state = std::make_unique<State<T>>();
    rio::Promise<State<T>> p{unique_state.get()};
    auto f = rio::fut::make(std::move(unique_state), rio::fut::Call_poll_ptr{});
    return std::pair{p, std::move(f)};
}

} // namespace promise
} // namespace rio
