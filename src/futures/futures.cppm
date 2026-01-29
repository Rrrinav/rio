module;
#include <cstdint>
#include <optional>
#include <system_error>
#include <concepts>
#include <utility>
#include <type_traits>

export module rio:futures;

import std;
import std.compat;

namespace rio {

    namespace fut {

    export enum class status : ::uint8_t { pending, error, ready };

    export template <typename T>
    struct res
    {
        using value_type = T;
        status state;
        std::optional<T> value = std::nullopt;
        std::error_code err = {};

        static res pending() { return {.state = status::pending}; }
        static res ready(T v) { return {.state = status::ready, .value = std::move(v)}; }
        static res error(std::errc ec) { return {.state = status::error, .err = std::make_error_code(ec)}; }
        static res error(std::error_code ec) { return {.state = status::error, .err = ec}; }
    };

    export template <>
    struct res<void>
    {
        using value_type = void;
        status state;
        std::error_code err = {};

        static res pending() { return {.state = status::pending}; }
        static res ready() { return {.state = status::ready}; }
        static res error(std::errc ec) { return {.state = status::error, .err = std::make_error_code(ec)}; }
        static res error(std::error_code ec) { return {.state = status::error, .err = ec}; }
    };

    }  // namespace fut

    namespace tag_invoke_ns {

    // If compiler finds this one during resolution that means type you are trying to poll is not a valid future.
    void tag_invoke();

    struct poll_t
    {
        template <typename T>
        requires requires(T &&t) { tag_invoke(std::declval<const poll_t &>(), std::forward<T>(t)); }
        constexpr auto operator()(T &&t) const -> decltype(tag_invoke(*this, std::forward<T>(t)))
        {
            return tag_invoke(*this, std::forward<T>(t));
        }
    };

    }  // namespace tag_invoke_ns

using tag_invoke_ns::poll_t;
export inline constexpr poll_t poll{};

template <typename T>
struct is_poll_res : std::false_type {};
template <typename T>
struct is_poll_res<fut::res<T>> : std::true_type {};

export template <typename F>
concept Pollable = requires(F &f) {
    typename F::value_type;
    { poll(f) } -> std::same_as<fut::res<typename F::value_type>>;
};

export template <typename Fn, typename State>
concept PollFunction = std::invocable<Fn &, State &> && is_poll_res<std::invoke_result_t<Fn &, State &>>::value;

export template <typename State, typename PollFn>
struct Future
{
    using value_type = typename std::invoke_result_t<PollFn &, State &>::value_type;

    State state;
    PollFn fn;

    fut::res<value_type> poll() { return fn(state); }
    friend auto tag_invoke(poll_t, Future &f) { return f.poll(); }

    template <typename Fn>
    auto then(Fn fn) &&;

    template <typename Rep, typename Period>
    auto timeout(std::chrono::duration<Rep, Period> d) &&;
};

export template <typename State, typename PollFn>
Future(State, PollFn) -> Future<State, PollFn>;

    namespace detail {

    template <typename Fn, typename Input>
    struct next_fut_t
    {
        using type = std::invoke_result_t<Fn &, Input>;
    };

    template <typename Fn>
    struct next_fut_t<Fn, void>
    {
        using type = std::invoke_result_t<Fn &>;
    };

    }  // namespace detail

    namespace fut {
    export template <Pollable F, typename Fn>
    struct Then
    {
        using input_type = typename F::value_type;

        using NextFuture = typename detail::next_fut_t<Fn, input_type>::type;
        using value_type = typename NextFuture::value_type;

        F first_fut;
        Fn fn;

        enum class Phase : uint8_t { First, Next, Done } phase = Phase::First;
        std::optional<NextFuture> next{};

        fut::res<value_type> poll()
        {
            if (phase == Phase::Done)
                return fut::res<value_type>::error(std::make_error_code(std::errc::operation_not_permitted));

            if (phase == Phase::First)
            {
                auto r = rio::poll(first_fut);
                if (r.state == fut::status::pending)
                    return fut::res<value_type>::pending();
                if (r.state == fut::status::error)
                {
                    phase = Phase::Done;
                    return fut::res<value_type>::error(r.err);
                }

                if constexpr (std::is_void_v<input_type>)
                    next.emplace(fn());
                else
                    next.emplace(fn(std::move(*r.value)));

                phase = Phase::Next;
            }

            auto r = rio::poll(*next);
            if (r.state != fut::status::pending)
                phase = Phase::Done;
            return r;
        }
        friend auto tag_invoke(poll_t, Then &t) { return t.poll(); }
    };

    export template <typename State, typename BodyFn>
    struct Loop
    {
        using value_type = void;
        using InnerFuture = std::invoke_result_t<BodyFn &, State &>;

        State state;
        BodyFn body_fn;
        std::optional<InnerFuture> curr_fut{};

        fut::res<void> poll()
        {
            while (true)
            {
                if (!curr_fut)
                    curr_fut.emplace(body_fn(state));
                auto r = rio::poll(*curr_fut);

                if (r.state == fut::status::pending)
                    return fut::res<void>::pending();
                if (r.state == fut::status::error)
                    return fut::res<void>::error(r.err);

                state = std::move(*r.value);
                curr_fut.reset();
            }
        }
        friend auto tag_invoke(poll_t, Loop &l) { return l.poll(); }
    };

    export template <typename State, typename BodyFn>
    Loop(State, BodyFn) -> Loop<State, BodyFn>;

    template<typename Fut>
    struct Timeout
    {
        using value_type = Fut::value_type;
        Fut fut;
        std::chrono::steady_clock::time_point deadline;
        bool timed_out = false;

        fut::res<value_type> poll()
        {
            if (timed_out) [[unlikely]]
                return fut::res<value_type>::error(std::make_error_code(std::errc::timed_out));

            auto r = rio::poll(fut);

            if (r.state != fut::status::pending)
                return r;

            if (std::chrono::steady_clock::now() >= deadline)
            {
                timed_out = true;
                return fut::res<value_type>::error(std::make_error_code(std::errc::timed_out));
            }

            return fut::res<value_type>::pending();
        }

        friend auto tag_invoke(poll_t, Timeout &t) { return t.poll(); }
    };

    export template <typename F>
    Timeout(F, std::chrono::steady_clock::time_point) -> Timeout<F>;

    } // namespace fut

    namespace fut {

    export template <typename State, typename Fn>
    auto make(State &&s, Fn &&fn)
    {
        return Future<std::decay_t<State>, std::decay_t<Fn>>{.state = std::forward<State>(s), .fn = std::forward<Fn>(fn)};
    }

    export template <typename T>
    auto ready(T val)
    {
        return make(std::move(val), [](T &v) { return res<T>::ready(std::move(v)); });
    }

    export inline auto ready()
    {
        struct empty_t {};
        return make(empty_t{}, [](empty_t &) { return res<void>::ready(); });
    }

    export template <typename State, typename BodyFn>
    auto loop(State &&s, BodyFn &&fn)
    {
        return Loop<std::decay_t<State>, std::decay_t<BodyFn>>{.state = std::forward<State>(s), .body_fn = std::forward<BodyFn>(fn)};
    }
    }  // namespace fut

template <typename S, typename P>
template <typename Fn>
auto Future<S, P>::then(Fn fn) &&
{
    return fut::Then<Future, Fn>{.first_fut = std::move(*this), .fn = std::move(fn)};
}


template <typename S, typename P>
template <typename Rep, typename Period>
auto Future<S, P>::timeout(std::chrono::duration<Rep, Period> d) &&
{
    return fut::Timeout<Future>{.inner_fut = std::move(*this), .deadline = std::chrono::steady_clock::now() + d};
}

}  // namespace rio
