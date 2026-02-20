# FUTURES

Futures are just a way to:
- store a state that can be used to generate data in future.
- hopefully a way to know if that data exists yet and getting that data out.
- Way to cook data from state.

How do we do these two:

## **store data**

```cpp
template <typename State, typename Poll_fn = rio::fut::Call_poll>
struct Future {
    State data;
    // other
};
```
Here we give it an initial data to do something with to generate data

---
## **Know if data exists and getting it out**

```cpp
auto r = rio::poll(future);
auto r = future.poll();
```

type of `r` is

```cpp
enum class rio::fut::status : ::uint8_t { pending, error, ready };
rio::fut::res {
    using value_type = T;
    status state;
    std::optional<T> value = std::nullopt;
    std::error_code err = {};
}
```

**Here:**
- pending: means data isn't cooked from state yet.
- error: means data couldn't be cooked.
    res::err contains error code for error.
- ready: menas data is cooked.
    value contains data

> `value` & `err` are not valid outside of when `state` says they are, in library.

If you use them personally, you can do whatever.

---
## **A way to cook data from state.**

Things are a little complicated here

This cooked data has to be of form `rio::fut::res<T>`.

```cpp
template <typename State, typename Poll_fn = rio::fut::Call_poll>
struct Future {
    State data{};
    Poll_fn fn = rio::fut::Call_poll{};
}
```

So basically the poll function is part of the type signature, this reduces virtual call overhead but in return introduces complexity because of complicated type signatures.

**rio::fut::Call_poll{}:** Basically calls `.poll()` method of the state of the future. Obvious, poll method has to accept `State&` & return `rio::fut:res<SomeType>`.

You can also give custom poller.

### Examples

**We will construct basic counters**

_State:_
```cpp
struct counter
{
    int l, h;
};
```

**First way:**
```cpp
rio::Future f {
    counter{0, 10},
    [](counter &c) {
        if (++c.l >= c.h)
            return rio::fut::res<std::size_t>::ready(c.h);
        else
            return rio::fut::res<std::size_t>::pending();
    }
};

while (true) {
    if (auto s = f.poll(); s.state == rio::fut::status::ready) {
        std::println("val: {}", s.value.value());
        return 0;
    }
}
return 1;
```

Here we are using lambda as a poll funcion, so `rio::fut::Call_poll` will be overriden and this lambda will be part of the type.
but if I write same future definition twice they won't be same type becuase both inlined lambdas tho same, will be different types.

**So, second way:**
```cpp
auto poll_fn = [](counter &c) {
    if (++c.l == c.h)
        return rio::fut::res<std::size_t>::ready(c.h);
    else if (c.l > c.h)
        return rio::fut::res<std::size_t>::error(std::errc::invalid_seek);
    else
        return rio::fut::res<std::size_t>::pending();
};

rio::Future f1{counter{0, 10}, poll_fn};
rio::Future f2{counter{0, 20}, poll_fn};

static_assert(std::is_same_v<decltype(f1), decltype(f2)>, "This won't fail here.");
constexpr auto pending_state = rio::fut::status::pending;

while (true) {
    auto s1 = rio::poll(f1);
    auto s2 = rio::poll(f2);

    if (s1.state == rio::fut::status::ready)
        std::println("{}", s1.value.value());
    if (s2.state == rio::fut::status::ready)
        std::println("{}", s2.value.value());

    if (!(s1.state == pending_state || s2.state == pending_state))
        break;
}
return 1;
```

**Third way:**
```cpp
struct fut
{
    counter c{0, 10};
    auto poll()
    {
        if (++c.l == c.h)
            return rio::fut::res<std::size_t>::ready(c.h);
        else if (c.l > c.h)
            return rio::fut::res<std::size_t>::error(std::errc::invalid_seek);
        else
            return rio::fut::res<std::size_t>::pending();
    }
};

int main()
{
    rio::Future<fut> f3{ fut { .c = counter{0, 40} } };
    rio::Future<fut> f2{counter{0, 20}};
    rio::Future<fut> f1{};

    std::array<rio::Future<fut>, 3> futures{std::move(f1), std::move(f2), std::move(f3) };

    while (true) {
        auto pending_count = std::ranges::count_if(futures, [](auto &f) {
            auto s = rio::poll(f);
            // side effect.
            if (s.state == rio::fut::status::ready)
                std::println("{}", s.value.value());

            return s.state == rio::fut::status::pending;
        });

        if (pending_count == 0) {
            break;
        }
    }
    return 1;
}
```
