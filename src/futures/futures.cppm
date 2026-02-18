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
    static res pending()
    {
        return { .state = status::pending };
    }
    static res ready(T v)
    {
        return { .state = status::ready, .value = std::move(v) };
    }
    static res error(std::error_code ec)
    {
        return { .state = status::error, .err = ec };
    }
    static res error(std::errc ec)
    {
        return { .state = status::error, .err = std::make_error_code(ec) };
    }
};

export template <>
struct res<void>
{
    using value_type = void;
    status state;
    std::error_code err = {};
    static res pending()
    {
        return { .state = status::pending };
    }
    static res ready()
    {
        return { .state = status::ready };
    }
    static res error(std::error_code ec)
    {
        return { .state = status::error, .err = ec };
    }
    static res error(std::errc ec)
    {
        return { .state = status::error, .err = std::make_error_code(ec) };
    }
};
} // namespace fut

namespace tag_invoke_impl {
// If compiler finds this during resolution, then the thing you are trying to poll is not a valid future.
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
} // namespace tag_invoke_impl

using tag_invoke_impl::poll_t;
export inline constexpr poll_t poll{};

template <typename T>
struct is_poll_res : std::false_type
{};
template <typename T>
struct is_poll_res<fut::res<T>> : std::true_type
{};

export template <typename F>
concept Pollable = requires(F &f) {
    typename F::value_type;
    { poll(f) } -> std::same_as<fut::res<typename F::value_type>>;
};

export template <typename Fn, typename State>
concept PollFunction = std::invocable<Fn &, State &> && is_poll_res<std::invoke_result_t<Fn &, State &>>::value;

namespace fut {
export struct Call_poll
{
    template <typename T>
    auto operator()(T &t) const
    {
        return t.poll();
    }
};

export template <typename State, typename Fn>
auto make(State &&s, Fn &&fn);
export template <Pollable F, typename Fn>
struct Then_impl;
export template <typename F>
struct Timeout_impl;
export template <typename F, typename Callback, typename RecoveryFut>
struct Timeout_with_impl;
export template <typename State, typename BodyFn>
struct Loop_impl;
} // namespace fut

export template <typename State, typename Poll_fn>
struct Future
{
    using state_type = State;
    using value_type = typename std::invoke_result_t<Poll_fn &, State &>::value_type;

    State data;
    Poll_fn fn;

    Future(State s, Poll_fn f) : data(std::move(s)), fn(std::move(f))
    {}

    Future(Future &&) noexcept(
        std::is_nothrow_move_constructible_v<State> && std::is_nothrow_move_constructible_v<Poll_fn>) = default;

    Future &operator=(Future &&other) noexcept(
        std::is_nothrow_move_constructible_v<State> && std::is_nothrow_move_constructible_v<Poll_fn>)
    {
        if (this != &other) {
            data = std::move(other.data);
            std::destroy_at(&fn);
            std::construct_at(&fn, std::move(other.fn));
        }
        return *this;
    }

    fut::res<value_type> poll()
    {
        return fn(data);
    }
    friend auto tag_invoke(poll_t, Future &f)
    {
        return f.poll();
    }

    template <typename Fn>
    auto then(Fn &&fn) &&;

    template <typename Fn>
    auto map(Fn &&fn) &&;

    template <typename Fn>
    auto or_else(Fn &&fn) &&;

    template <typename Rep, typename Period>
    auto timeout(std::chrono::duration<Rep, Period> d) &&;

    template <typename Rep, typename Period, typename Callback>
    auto timeout_with(std::chrono::duration<Rep, Period> d, Callback cb) &&;
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
} // namespace detail

namespace fut {

export template <Pollable Fut, typename Fn>
struct Then_impl
{
    using input_type = typename Fut::value_type;
    using next_future_type = typename detail::next_fut_t<Fn, input_type>::type;
    using value_type = typename next_future_type::value_type;

    Fut first_fut;
    Fn fn;
    enum class Phase : uint8_t { First, Next, Done } phase = Phase::First;
    std::optional<next_future_type> next{};

    Then_impl(Fut f, Fn func) : first_fut(std::move(f)), fn(std::move(func))
    {}

    Then_impl(Then_impl &&) noexcept = default;
    Then_impl &operator=(Then_impl &&other) noexcept
    {
        if (this != &other) {
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
        if (phase == Phase::First) {
            auto r = rio::poll(first_fut);
            if (r.state == fut::status::pending)
                return fut::res<value_type>::pending();
            if (r.state == fut::status::error) {
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
    friend auto tag_invoke(poll_t, Then_impl &t)
    {
        return t.poll();
    }
};
export template <Pollable Fut, typename Fn>
Then_impl(Fut, Fn) -> Then_impl<Fut, Fn>;

export template <Pollable Fut, typename Fn>
struct Map_impl
{
    using input_type = typename Fut::value_type;

    static_assert(!std::is_void_v<input_type>, "Map cannot be used on void Futures. Use .then() instead.");
    using value_type = std::invoke_result_t<Fn, input_type>;

    Fut fut;
    Fn fn;

    Map_impl(Fut f, Fn func) : fut(std::move(f)), fn(std::move(func))
    {}

    Map_impl(Map_impl &&) noexcept = default;
    Map_impl &operator=(Map_impl &&other) noexcept
    {
        if (this != &other) {
            fut = std::move(other.fut);
            std::destroy_at(&fn);
            std::construct_at(&fn, std::move(other.fn));
        }
        return *this;
    }

    fut::res<value_type> poll()
    {
        auto r = rio::poll(fut);

        if (r.state == fut::status::pending)
            return fut::res<value_type>::pending();

        if (r.state == fut::status::error)
            return fut::res<value_type>::error(r.err);

        if constexpr (std::is_void_v<input_type>) {
            if constexpr (std::is_void_v<value_type>) {
                fn();
                return fut::res<void>::ready();
            } else {
                return fut::res<value_type>::ready(fn());
            }
        } else if constexpr (std::is_void_v<value_type>) {
            fn(std::move(*r.value));
            return fut::res<void>::ready();
        } else {
            return fut::res<value_type>::ready(fn(std::move(*r.value)));
        }
    }
    friend auto tag_invoke(poll_t, Map_impl &m)
    {
        return m.poll();
    }
};
export template <Pollable Fut, typename Fn>
Map_impl(Fut, Fn) -> Map_impl<Fut, Fn>;

export template <Pollable Fut, typename Fn>
struct Or_else_impl
{
    using value_type = typename Fut::value_type;
    using next_future_type = std::invoke_result_t<Fn, std::error_code>;

    static_assert(Pollable<next_future_type> && std::is_same_v<typename next_future_type::value_type, value_type>,
        "or_else recovery function must return a Future with the same value_type");
    Fut first_fut;
    Fn fn;
    enum class Phase : uint8_t { First, Recovery, Done } phase = Phase::First;
    std::optional<next_future_type> recovery{};

    Or_else_impl(Fut f, Fn func) : first_fut(std::move(f)), fn(std::move(func))
    {}

    Or_else_impl(Or_else_impl &&) noexcept = default;
    Or_else_impl &operator=(Or_else_impl &&other) noexcept
    {
        if (this != &other) {
            first_fut = std::move(other.first_fut);
            phase = other.phase;
            recovery = std::move(other.recovery);
            std::destroy_at(&fn);
            std::construct_at(&fn, std::move(other.fn));
        }
        return *this;
    }

    fut::res<value_type> poll()
    {
        if (phase == Phase::Done)
            return fut::res<value_type>::error(std::make_error_code(std::errc::operation_not_permitted));

        if (phase == Phase::First) {
            auto r = rio::poll(first_fut);

            if (r.state == fut::status::ready) {
                phase = Phase::Done;
                if constexpr (std::is_void_v<value_type>)
                    return fut::res<void>::ready();
                else
                    return fut::res<value_type>::ready(std::move(*r.value));
            }

            if (r.state == fut::status::pending)
                return fut::res<value_type>::pending();

            if (r.state == fut::status::error) {
                recovery.emplace(fn(r.err));
                phase = Phase::Recovery;
            }
        }

        auto r = rio::poll(*recovery);
        if (r.state != fut::status::pending)
            phase = Phase::Done;
        return r;
    }
    friend auto tag_invoke(poll_t, Or_else_impl &o)
    {
        return o.poll();
    }
};
export template <Pollable Fut, typename Fn>
Or_else_impl(Fut, Fn) -> Or_else_impl<Fut, Fn>;

export template <typename State, typename Body_fn>
struct Loop_impl
{
    using value_type = void;
    using inner_future_type = std::invoke_result_t<Body_fn &, State &>;
    using function_type = Body_fn;

    State data;
    Body_fn body_fn;
    std::optional<inner_future_type> curr_fut{};

    Loop_impl(State s, Body_fn fn) : data(std::move(s)), body_fn(std::move(fn))
    {}

    Loop_impl(Loop_impl &&) noexcept = default;
    Loop_impl &operator=(Loop_impl &&other) noexcept
    {
        if (this != &other) {
            this->data = std::move(other.data);
            other.data = State{};
            this->curr_fut = std::move(other.curr_fut);
            other.curr_fut = std::nullopt;
            std::destroy_at(&body_fn);
            std::construct_at(&body_fn, other.body_fn);
        }
        return *this;
    }

    fut::res<void> poll()
    {
        while (true) {
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
    friend auto tag_invoke(poll_t, Loop_impl &l)
    {
        return l.poll();
    }
};
export template <typename State, typename BodyFn>
Loop_impl(State, BodyFn) -> Loop_impl<State, BodyFn>;

export template <typename Fut>
struct Timeout_impl
{
    using value_type = typename Fut::value_type;
    using future_type = Fut;
    Fut fut;
    std::chrono::steady_clock::time_point deadline;
    bool timed_out = false;

    Timeout_impl(Fut f, std::chrono::steady_clock::time_point t) : fut(std::move(f)), deadline(t)
    {}
    Timeout_impl(Timeout_impl &&) noexcept = default;
    Timeout_impl &operator=(Timeout_impl &&) noexcept = default;

    fut::res<value_type> poll()
    {
        if (timed_out)
            return fut::res<value_type>::error(std::make_error_code(std::errc::timed_out));
        auto r = rio::poll(fut);
        if (r.state != fut::status::pending)
            return r;
        if (std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            return fut::res<value_type>::error(std::make_error_code(std::errc::timed_out));
        }
        return fut::res<value_type>::pending();
    }
    friend auto tag_invoke(poll_t, Timeout_impl &t)
    {
        return t.poll();
    }
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

    Timeout_with_impl(F f, std::chrono::steady_clock::time_point t, Callback c)
        : first_fut(std::move(f)), deadline(t), callback(std::move(c))
    {}
    Timeout_with_impl(Timeout_with_impl &&) noexcept = default;
    Timeout_with_impl &operator=(Timeout_with_impl &&) noexcept = default;

    fut::res<value_type> poll()
    {
        if (phase == Phase::Done)
            return fut::res<value_type>::error(std::make_error_code(std::errc::operation_not_permitted));
        if (phase == Phase::Normal) {
            auto r = rio::poll(first_fut);
            if (r.state != fut::status::pending)
                return r;
            if (std::chrono::steady_clock::now() >= deadline) {
                recovery_fut.emplace(callback(std::move(first_fut.data)));
                phase = Phase::Recovery;
            } else
                return fut::res<value_type>::pending();
        }
        auto r = rio::poll(*recovery_fut);
        if (r.state != fut::status::pending)
            phase = Phase::Done;
        return r;
    }
    friend auto tag_invoke(poll_t, Timeout_with_impl &t)
    {
        return t.poll();
    }
};
export template <typename F, typename C>
Timeout_with_impl(F, std::chrono::steady_clock::time_point, C)
    -> Timeout_with_impl<F, C, std::invoke_result_t<C &, typename F::state_type &&>>;

struct Monostate
{};
template <typename T>
using non_void_t = std::conditional_t<std::is_void_v<T>, Monostate, T>;

export template <typename Fut1, typename Fut2>
struct Both_impl
{
    Fut1 fut1;
    Fut2 fut2;

    using res_type1 = decltype(fut1.poll());
    using res_type2 = decltype(fut2.poll());

    using value_type1 = typename res_type1::value_type;
    using value_type2 = typename res_type2::value_type;

    using storage_type1 = non_void_t<value_type1>;
    using storage_type2 = non_void_t<value_type2>;

    using value_type = std::pair<value_type1, value_type2>;

    std::optional<storage_type1> r1{};
    std::optional<storage_type2> r2{};

    Both_impl(Fut1 f1, Fut2 f2) : fut1(std::move(f1)), fut2(std::move(f2))
    {}
    Both_impl(Both_impl &) = delete;
    Both_impl &operator=(Both_impl &) = delete;
    Both_impl(Both_impl &&) noexcept = default;

    Both_impl &operator=(Both_impl &&other) noexcept
    {
        if (this != &other) {
            fut1 = std::move(other.fut1);
            fut2 = std::move(other.fut2);
        }
        return *this;
    }

    auto poll() -> rio::fut::res<value_type>
    {
        bool pending = false;

        if (!r1) {
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

        if (!r2) {
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

    friend auto tag_invoke(poll_t, Both_impl &f)
    {
        return f.poll();
    }
};
export template <typename F1, typename F2>
Both_impl(F1, F2) -> Both_impl<F1, F2>;

export template <typename Container, typename Poll_fn>
struct For_all
{
    using State = typename Container::value_type;
    using Res = std::invoke_result_t<Poll_fn &, State &>;
    using Val = typename Res::value_type;

    Container states;
    Poll_fn fn;
    std::vector<std::optional<Res>> results;

    For_all(Container c, Poll_fn f) : states(std::move(c)), fn(std::move(f))
    {
        results.resize(states.size());
    }

    For_all(For_all &) = delete;
    For_all &operator=(For_all &) = delete;
    For_all(For_all &&) noexcept = default;

    For_all &operator=(For_all &&other) noexcept
    {
        if (this != &other) {
            states = std::move(other.states);
            results = std::move(other.results);
            std::destroy_at(&fn);
            std::construct_at(&fn, other.fn);
        }
        return *this;
    }

    void set_states(Container c)
    {
        states = std::move(c);
        results.assign(states.size(), std::nullopt);
    }

    auto poll() -> rio::fut::res<std::vector<Val>>
    {
        auto active_tasks = std::views::zip(states, results) |
                            std::views::filter([](const auto &pair) { return !std::get<1>(pair).has_value(); });

        bool pending = false;

        for (auto &&[state, res_opt] : active_tasks) {
            auto r = fn(state);

            if (r.state != rio::fut::status::pending)
                res_opt.emplace(std::move(r));
            else
                pending = true;
        }

        if (pending)
            return rio::fut::res<std::vector<Val>>::pending();

        for (auto &r : results)
            if (r->state == rio::fut::status::error)
                return rio::fut::res<std::vector<Val>>::error(r->err);

        if constexpr (std::is_void_v<Val>)
            return rio::fut::res<std::vector<Val>>::ready();
        else {
            std::vector<Val> final_vec;
            final_vec.reserve(results.size());
            for (auto &r : results)
                final_vec.push_back(std::move(*r->value));
            return rio::fut::res<std::vector<Val>>::ready(std::move(final_vec));
        }
    }

    friend auto tag_invoke(rio::tag_invoke_impl::poll_t, For_all &f)
    {
        return f.poll();
    }
};

export template <typename Container, typename Poll_fn>
For_all(Container, Poll_fn) -> For_all<Container, Poll_fn>;

export template <typename Container, typename Poll_fn>
auto for_all(Container &&c, Poll_fn &&f)
{
    using For_all_type = For_all<std::decay_t<Container>, std::decay_t<Poll_fn>>;

    return rio::Future{ For_all_type{ std::move(c), std::move(f) }, Call_poll{} };
}

export template <typename... Futs>
struct Join_impl
{
    std::tuple<Futs...> futures;

    using value_type = std::tuple<typename Futs::value_type...>;

    std::tuple<std::optional<typename Futs::value_type>...> results;

    Join_impl(Futs... f) : futures(std::move(f)...)
    {}

    auto poll() -> rio::fut::res<value_type>
    {
        bool pending = false;
        bool error = false;
        std::error_code ec;

        auto process = [&](auto &fut, auto &res_opt) {
            if (res_opt.has_value())
                return;

            auto r = rio::poll(fut);
            if (r.state == rio::fut::status::ready) {
                if constexpr (std::is_void_v<typename std::decay_t<decltype(fut)>::value_type>)
                    res_opt.emplace(rio::fut::res<void>::ready());
                else
                    res_opt.emplace(std::move(*r.value));
            } else if (r.state == rio::fut::status::error) {
                error = true;
                ec = r.err;
            } else {
                pending = true;
            }
        };

        std::apply(
            [&](auto &...f_args) { std::apply([&](auto &...r_args) { (process(f_args, r_args), ...); }, results); },
            futures);

        if (error)
            return rio::fut::res<value_type>::error(ec);
        if (pending)
            return rio::fut::res<value_type>::pending();

        return std::apply(
            [&](auto &...r_args) { return rio::fut::res<value_type>::ready(std::make_tuple(std::move(*r_args)...)); },
            results);
    }

    friend auto tag_invoke(rio::tag_invoke_impl::poll_t, Join_impl &j)
    {
        return j.poll();
    }
};

export template <typename... Futs>
Join_impl(Futs...) -> Join_impl<Futs...>;

export template <typename... Futs>
auto join(Futs &&...futs)
{
    using JoinType = Join_impl<std::decay_t<Futs>...>;
    return rio::Future{ JoinType{ std::forward<Futs>(futs)... }, Call_poll{} };
}

export template <typename... Futs>
struct First_of_impl
{
    std::tuple<Futs...> futures;

    using value_type = std::variant<
        std::conditional_t<std::is_void_v<typename Futs::value_type>, std::monostate, typename Futs::value_type>...>;

    First_of_impl(Futs... f) : futures(std::move(f)...)
    {}

    auto poll() -> rio::fut::res<value_type>
    {
        std::optional<rio::fut::res<value_type>> winner;

        auto process = [&]<std::size_t I>(auto &fut) {
            if (winner)
                return;

            auto r = rio::poll(fut);

            if (r.state == rio::fut::status::ready) {
                if constexpr (std::is_void_v<typename std::decay_t<decltype(fut)>::value_type>)
                    winner.emplace(rio::fut::res<value_type>::ready(value_type(std::in_place_index<I>)));
                else
                    winner.emplace(
                        rio::fut::res<value_type>::ready(value_type(std::in_place_index<I>, std::move(*r.value))));
            } else if (r.state == rio::fut::status::error) {
                winner.emplace(rio::fut::res<value_type>::error(r.err));
            }
        };

        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (process.template operator()<Is>(std::get<Is>(futures)), ...);
        }(std::make_index_sequence<sizeof...(Futs)>{});

        if (winner)
            return std::move(*winner);
        return rio::fut::res<value_type>::pending();
    }

    friend auto tag_invoke(rio::tag_invoke_impl::poll_t, First_of_impl &r)
    {
        return r.poll();
    }
};

export template <typename... Futs>
First_of_impl(Futs...) -> First_of_impl<Futs...>;

export template <typename... Futs>
auto first_of(Futs &&...futs)
{
    using Race_type = First_of_impl<std::decay_t<Futs>...>;
    return rio::Future{ Race_type{ std::forward<Futs>(futs)... }, Call_poll{} };
}
} // namespace fut

namespace fut {

export template <typename State, typename Poll_fn>
auto make(State &&s, Poll_fn &&fn)
{
    return Future<std::decay_t<State>, std::decay_t<Poll_fn>>{ std::forward<State>(s), std::forward<Poll_fn>(fn) };
}

export template <typename T>
struct Immediate_impl
{
    using value_type = T;
    std::optional<T> val{ std::nullopt };
    std::error_code err{};

    res<T> poll()
    {
        if (err)
            return res<T>::error(err);
        return res<T>::ready(std::move(*val));
    }

    friend auto tag_invoke(poll_t, Immediate_impl &i)
    {
        return i.poll();
    }
};

export template <typename T>
auto ready(T val)
{
    return Future{ Immediate_impl<T>{ .val = std::move(val) }, Call_poll{} };
}

export template <typename T>
auto error(std::error_code ec)
{
    return Future{ Immediate_impl<T>{ .err = ec }, Call_poll{} };
}

export template <typename T>
using ready_t = Future<Immediate_impl<T>, Call_poll>;
export template <typename T>
using error_t = Future<Immediate_impl<T>, Call_poll>;

export template <>
struct Immediate_impl<void>
{
    using value_type = void;
    bool is_ready = false;
    std::error_code err{};

    res<void> poll()
    {
        if (err)
            return res<void>::error(err);
        // If constructed via ready(), implies success
        return res<void>::ready();
    }

    friend auto tag_invoke(poll_t, Immediate_impl &i)
    {
        return i.poll();
    }
};

export inline auto ready()
{
    return Future{ Immediate_impl<void>{ .is_ready = true }, Call_poll{} };
}

export template <typename State, typename BodyFn>
auto loop(State &&s, BodyFn &&fn)
{
    using L = Loop_impl<std::decay_t<State>, std::decay_t<BodyFn>>;
    return make(L{ std::forward<State>(s), std::forward<BodyFn>(fn) }, Call_poll{});
}

export template <typename Fut1, typename Fut2>
auto both(Fut1 &&f1, Fut2 &&f2)
{
    using Both_type = Both_impl<std::decay_t<Fut1>, std::decay_t<Fut2>>;
    return rio::Future{ Both_type{ std::move(f1), std::move(f2) }, Call_poll{} };
}
} // namespace fut

template <typename S, typename P>
template <typename Fn>
auto Future<S, P>::then(Fn &&fn) &&
{
    using T = fut::Then_impl<Future, std::decay_t<Fn>>;
    return fut::make(T{ std::move(*this), std::forward<Fn>(fn) }, fut::Call_poll{});
}

template <typename S, typename P>
template <typename Fn>
auto Future<S, P>::map(Fn &&fn) &&
{
    using M = fut::Map_impl<Future, std::decay_t<Fn>>;
    return fut::make(M{ std::move(*this), std::forward<Fn>(fn) }, fut::Call_poll{});
}

template <typename S, typename P>
template <typename Fn>
auto Future<S, P>::or_else(Fn &&fn) &&
{
    using O = fut::Or_else_impl<Future, std::decay_t<Fn>>;
    return fut::make(O{ std::move(*this), std::forward<Fn>(fn) }, fut::Call_poll{});
}

template <typename S, typename P>
template <typename Rep, typename Period>
auto Future<S, P>::timeout(std::chrono::duration<Rep, Period> d) &&
{
    using T = fut::Timeout_impl<Future>;
    return fut::make(T{ std::move(*this), std::chrono::steady_clock::now() + d }, fut::Call_poll{});
}

template <typename S, typename P>
template <typename Rep, typename Period, typename Callback>
auto Future<S, P>::timeout_with(std::chrono::duration<Rep, Period> d, Callback cb) &&
{
    using RecFut = std::invoke_result_t<Callback &, S &&>;
    using T = fut::Timeout_with_impl<Future, std::decay_t<Callback>, RecFut>;
    return fut::make(T{ std::move(*this), std::chrono::steady_clock::now() + d, std::move(cb) }, fut::Call_poll{});
}

} // namespace rio
