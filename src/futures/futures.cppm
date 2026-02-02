module;
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
            static res error(std::error_code ec) { return {.state = status::error, .err = ec}; }
            static res error(std::errc ec) { return {.state = status::error, .err = std::make_error_code(ec)}; }
        };

        export template <>
        struct res<void>
        {
            using value_type = void;
            status state;
            std::error_code err = {};
            static res pending() { return {.state = status::pending}; }
            static res ready() { return {.state = status::ready}; }
            static res error(std::error_code ec) { return {.state = status::error, .err = ec}; }
            static res error(std::errc ec) { return {.state = status::error, .err = std::make_error_code(ec)}; }
        };
    }

    namespace tag_invoke_impl {
        // If compiler finds this during resolution, then the thing you are trying to poll is not a valid future.
        // Some error.
        void tag_invoke();
        export struct poll_t
        {
            template <typename T>
            requires requires(T &&t) { tag_invoke(std::declval<const poll_t &>(), std::forward<T>(t)); }
            constexpr auto operator()(T &&t) const -> decltype(tag_invoke(*this, std::forward<T>(t)))
            {
                return tag_invoke(*this, std::forward<T>(t));
            }
        };
    }
using tag_invoke_impl::poll_t;
export inline constexpr poll_t poll{};

template <typename T> struct is_poll_res : std::false_type {};
template <typename T> struct is_poll_res<fut::res<T>> : std::true_type {};

export template <typename F>
concept Pollable = requires(F &f) {
    typename F::value_type;
    { poll(f) } -> std::same_as<fut::res<typename F::value_type>>;
};

export template <typename Fn, typename State>
concept PollFunction = std::invocable<Fn &, State &> && is_poll_res<std::invoke_result_t<Fn &, State &>>::value;

    namespace fut {
        export template <typename State, typename Fn> auto make(State &&s, Fn &&fn);
        export template <Pollable F, typename Fn> struct Then_impl;
        export template <typename F> struct Timeout_impl;
        export template <typename F, typename Callback, typename RecoveryFut> struct Timeout_with_impl;
        export template <typename State, typename BodyFn> struct Loop_impl;
    }

    export template <typename State, typename PollFn>
    struct Future {
        using state_type = State;
        using value_type = typename std::invoke_result_t<PollFn &, State &>::value_type;

        State data;
        PollFn fn;

        Future(State s, PollFn f) : data(std::move(s)), fn(std::move(f)) {}
        Future(Future &&) = default;

        Future &operator=(Future &&other) noexcept {
            if (this != &other) {
                data = std::move(other.data);
                std::destroy_at(&fn);
                std::construct_at(&fn, std::move(other.fn));
            }
            return *this;
        }

        fut::res<value_type> poll() { return fn(data); }
        friend auto tag_invoke(poll_t, Future &f) { return f.poll(); }

        template <typename Fn> auto then(Fn &&fn) &&;
        template <typename Rep, typename Period> auto timeout(std::chrono::duration<Rep, Period> d) &&;
        template <typename Rep, typename Period, typename Callback> auto timeout_with(std::chrono::duration<Rep, Period> d, Callback cb) &&;
    };
    export template <typename State, typename PollFn> Future(State, PollFn) -> Future<State, PollFn>;

    namespace detail {
        template <typename Fn, typename Input> struct next_fut_t { using type = std::invoke_result_t<Fn &, Input>; };
        template <typename Fn> struct next_fut_t<Fn, void> { using type = std::invoke_result_t<Fn &>; };
    }

    // =================================================================================================
    // 4. COMBINATOR DEFINITIONS
    // =================================================================================================
    namespace fut {

    export template <Pollable F, typename Fn>
    struct Then_impl
    {
        using input_type       = typename F::value_type;
        using next_future_type = typename detail::next_fut_t<Fn, input_type>::type;
        using value_type       = typename next_future_type::value_type;

        F first_fut;
        Fn fn;
        enum class Phase : uint8_t { First, Next, Done } phase = Phase::First;
        std::optional<next_future_type> next{};

        Then_impl(F f, Fn func) : first_fut(std::move(f)), fn(std::move(func)) {}
        Then_impl(Then_impl &&) = default;

        Then_impl &operator=(Then_impl &&other) noexcept
        {
            if (this != &other)
            {
                first_fut = std::move(other.first_fut);
                phase = other.phase;
                next = std::move(other.next);
                std::destroy_at(&fn);
                std::construct_at(&fn, std::move(other.fn));
            }
            return *this;
        }

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
        friend auto tag_invoke(poll_t, Then_impl &t) { return t.poll(); }
    };
    export template <Pollable Fut, typename Fn> Then_impl(Fut, Fn) -> Then_impl<Fut, Fn>;

    export template <typename State, typename Body_fn>
    struct Loop_impl
    {
        using value_type = void;
        using inner_future_type = std::invoke_result_t<Body_fn &, State &>;
        using function_type = Body_fn;

        State data;
        Body_fn body_fn;
        std::optional<inner_future_type> curr_fut{};

        Loop_impl(State s, Body_fn fn) : data(std::move(s)), body_fn(std::move(fn)) {}
        Loop_impl(Loop_impl &&) = default;

        Loop_impl &operator=(Loop_impl &&other) noexcept
        {
            if (this != &other)
            {
                data = std::move(other.data);
                curr_fut = std::move(other.curr_fut);
                std::destroy_at(&body_fn);
                std::construct_at(&body_fn, std::move(other.body_fn));
            }
            return *this;
        }

        fut::res<void> poll()
        {
            while (true)
            {
                if (!curr_fut)
                    curr_fut.emplace(body_fn(data));
                auto r = rio::poll(*curr_fut);
                if (r.state == fut::status::pending)
                    return fut::res<void>::pending();
                if (r.state == fut::status::error)
                    return fut::res<void>::error(r.err);
                if constexpr (!std::is_void_v<typename inner_future_type::value_type>)
                    data = std::move(*r.value);
                curr_fut.reset();
            }
        }
        friend auto tag_invoke(poll_t, Loop_impl &l) { return l.poll(); }
    };
    export template <typename State, typename BodyFn> Loop_impl(State, BodyFn) -> Loop_impl<State, BodyFn>;

    export template <typename F>
    struct Timeout_impl
    {
        using value_type = typename F::value_type;
        using future_type = F;
        F fut;
        std::chrono::steady_clock::time_point deadline;
        bool timed_out = false;

        Timeout_impl(F f, std::chrono::steady_clock::time_point t) : fut(std::move(f)), deadline(t) {}
        Timeout_impl(Timeout_impl &&) = default;

        Timeout_impl &operator=(Timeout_impl &&other) noexcept
        {
            if (this != &other)
            {
                fut = std::move(other.fut);
                deadline = other.deadline;
                timed_out = other.timed_out;
            }
            return *this;
        }

        fut::res<value_type> poll()
        {
            if (timed_out)
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
        friend auto tag_invoke(poll_t, Timeout_impl &t) { return t.poll(); }
    };
    export template <typename F>
    Timeout_impl(F, std::chrono::steady_clock::time_point) -> Timeout_impl<F>;

    export template <typename F, typename Callback, typename Recovery_fut>
    struct Timeout_with_impl
    {
        using value_type = typename F::value_type;
        using first_future_type = F;
        using callback_type = Callback;
        F first_fut;
        std::chrono::steady_clock::time_point deadline;
        Callback callback;
        enum class Phase : uint8_t { Normal, Recovery, Done } phase = Phase::Normal;
        std::optional<Recovery_fut> recovery_fut{};

        Timeout_with_impl(F f, std::chrono::steady_clock::time_point t, Callback c) : first_fut(std::move(f)), deadline(t), callback(std::move(c)) {}
        Timeout_with_impl(Timeout_with_impl &&) = default;

        Timeout_with_impl &operator=(Timeout_with_impl &&other) noexcept
        {
            if (this != &other)
            {
                first_fut = std::move(other.first_fut);
                deadline = other.deadline;
                phase = other.phase;
                recovery_fut = std::move(other.recovery_fut);
                std::destroy_at(&callback);
                std::construct_at(&callback, std::move(other.callback));
            }
            return *this;
        }

        fut::res<value_type> poll()
        {
            if (phase == Phase::Done)
                return fut::res<value_type>::error(std::make_error_code(std::errc::operation_not_permitted));
            if (phase == Phase::Normal)
            {
                auto r = rio::poll(first_fut);
                if (r.state != fut::status::pending)
                    return r;
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    recovery_fut.emplace(callback(std::move(first_fut.data)));
                    phase = Phase::Recovery;
                }
                else
                    return fut::res<value_type>::pending();
            }
            auto r = rio::poll(*recovery_fut);
            if (r.state != fut::status::pending)
                phase = Phase::Done;
            return r;
        }
        friend auto tag_invoke(poll_t, Timeout_with_impl &t) { return t.poll(); }
    };
    export template <typename F, typename C>
    Timeout_with_impl(F, std::chrono::steady_clock::time_point, C) -> Timeout_with_impl<F, C, std::invoke_result_t<C &, typename F::state_type &&>>;

    struct Monostate {};
    template <typename T>
    using NonVoid = std::conditional_t<std::is_void_v<T>, Monostate, T>;

    export template <typename Fut1, typename Fut2>
    struct Both_impl
    {
        Fut1 fut1;
        Fut2 fut2;

        using res_type1 = decltype(fut1.poll());
        using res_type2 = decltype(fut2.poll());

        using value_type1 = typename res_type1::value_type;
        using value_type2 = typename res_type2::value_type;

        using storage_type1 = NonVoid<value_type1>;
        using storage_type2 = NonVoid<value_type2>;

        using value_type = std::pair<value_type1, value_type2>;

        std::optional<storage_type1> r1{};
        std::optional<storage_type2> r2{};

        Both_impl(Fut1 f1, Fut2 f2) : fut1(std::move(f1)), fut2(std::move(f2)) {}
        Both_impl(Both_impl&&) = default;

        Both_impl&operator=(Both_impl&&other) noexcept {
            if (this != &other) {
                fut1= std::move(other.fut1);
                fut2= std::move(other.fut2);
            }
            return *this;
        }

        auto poll() -> rio::fut::res<value_type>
        {
            bool pending = false;

            if (!r1)
            {
                auto res = rio::poll(fut1);
                if (res.state == rio::fut::status::ready)
                    if constexpr (std::is_void_v<value_type1>)
                        r1.emplace(Monostate{});
                    else
                        r1.emplace(std::move(*res.value));
                else if (res.state == rio::fut::status::error)
                    return rio::fut::res<value_type>::error(res.err);
                else
                    pending = true;
            }

            if (!r2)
            {
                auto res = rio::poll(fut2);
                if (res.state == rio::fut::status::ready)
                    if constexpr (std::is_void_v<value_type2>)
                        r2.emplace(Monostate{});
                    else
                        r2.emplace(std::move(*res.value));
                else if (res.state == rio::fut::status::error)
                    return rio::fut::res<value_type>::error(res.err);
                else
                    pending = true;
            }

            if (pending)
                return rio::fut::res<value_type>::pending();

            return rio::fut::res<value_type>::ready(std::make_pair(std::move(*r1), std::move(*r2)));
        }

        friend auto tag_invoke(poll_t, Both_impl &f) { return f.poll(); }
    };
    export template <typename F1, typename F2>
    Both_impl(F1, F2) -> Both_impl<F1, F2>;
    }

    namespace fut {

    export template <typename State, typename Fn>
    auto make(State &&s, Fn &&fn)
    {
        return Future<std::decay_t<State>, std::decay_t<Fn>>{std::forward<State>(s), std::forward<Fn>(fn)};
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
        using L = Loop_impl<std::decay_t<State>, std::decay_t<BodyFn>>;
        return make(L{std::forward<State>(s), std::forward<BodyFn>(fn)}, [](L &l) { return l.poll(); });
    }

    export template<typename Fut1, typename Fut2>
    auto both(Fut1&& f1, Fut2&& f2)
    {
        using Both_type = Both_impl<std::decay_t<Fut1>, std::decay_t<Fut2>>;
        return rio::Future{
            Both_type{std::move(f1), std::move(f2)},
            [] (Both_type& b) { return b.poll(); }
        };
    }
    }  // namespace fut


template <typename S, typename P>
template <typename Fn>
auto Future<S, P>::then(Fn &&fn) &&
{
    using T = fut::Then_impl<Future, std::decay_t<Fn>>;
    return fut::make(T{std::move(*this), std::forward<Fn>(fn)}, [](T &s) { return s.poll(); });
}

template <typename S, typename P>
template <typename Rep, typename Period>
auto Future<S, P>::timeout(std::chrono::duration<Rep, Period> d) &&
{
    using T = fut::Timeout_impl<Future>;
    return fut::make(T{std::move(*this), std::chrono::steady_clock::now() + d}, [](T &s) { return s.poll(); });
}

template <typename S, typename P>
template <typename Rep, typename Period, typename Callback>
auto Future<S, P>::timeout_with(std::chrono::duration<Rep, Period> d, Callback cb) &&
{
    using RecFut = std::invoke_result_t<Callback &, S &&>;
    using T = fut::Timeout_with_impl<Future, std::decay_t<Callback>, RecFut>;
    return fut::make(T{std::move(*this), std::chrono::steady_clock::now() + d, std::move(cb)}, [](T &s) { return s.poll(); });
}

}
