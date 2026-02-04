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
struct Promise
{
    StateT *state;

    template <typename... Args>
    void resolve(Args &&...args)
    {
        state->resolve(std::forward<Args>(args)...);
    }

    void reject(std::error_code ec) { state->reject(ec); }
};

    export namespace promise {
    template <typename T>
    struct State
    {
        using value_type = T;

        std::optional<T> value{};
        std::error_code error{};
        bool is_ready = false;

        void resolve(T v)
        {
            value.emplace(std::move(v));
            is_ready = true;
        }

        void reject(std::error_code ec)
        {
            error = ec;
            is_ready = true;
        }

        auto poll() -> rio::fut::res<T>
        {
            if (is_ready)
            {
                if (value)
                    return rio::fut::res<T>::ready(std::move(*value));
                else
                    return rio::fut::res<T>::error(error);
            }
            return rio::fut::res<T>::pending();
        }
    };

    template <>
    struct State<void>
    {
        using value_type = void;

        std::error_code error{};
        bool is_ready = false;

        void resolve() { is_ready = true; }

        void reject(std::error_code ec)
        {
            error = ec;
            is_ready = true;
        }

        auto poll() -> rio::fut::res<void>
        {
            if (is_ready)
            {
                if (error)
                    return rio::fut::res<void>::error(error);
                return rio::fut::res<void>::ready();
            }
            return rio::fut::res<void>::pending();
        }
    };
    }  // namespace promise

    export namespace promise {

    template <StateLike StateT>
    auto bind(StateT &s)
    {
        rio::Promise<StateT> p{.state = &s};

        auto f = rio::fut::make(&s, [](StateT *ptr) { return ptr->poll(); });
        return std::pair{p, std::move(f)};
    }

    template <StateLike StateT>
    auto bind(StateT *s)
    {
        rio::Promise<StateT> p{.state = s};

        auto f = rio::fut::make(s, [](StateT *ptr) { return ptr->poll(); });
        return std::pair{p, std::move(f)};
    }

    template <typename T>
    auto make()
    {
        auto shared_state = std::make_shared<State<T>>();

        rio::Promise<State<T>> p{.state = shared_state.get()};
        auto f = rio::fut::make(std::move(shared_state), [](std::shared_ptr<State<T>> &s) { return s->poll(); });
        return std::pair{p, std::move(f)};
    }

    template <typename T>
    auto make_unique()
    {
        auto unique_state = std::make_unique<State<T>>();
        rio::Promise<State<T>> p{.state = unique_state.get()};
        auto f = rio::fut::make(std::move(unique_state), [](std::unique_ptr<State<T>> &s) { return s->poll(); });
        return std::pair{p, std::move(f)};
    }

    }  // namespace promise
}  // namespace rio
