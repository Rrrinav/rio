module;

export module rio:fut.task;

import std;

import :futures;

namespace rio::fut {

template <typename T>
struct Task_concept
{
    virtual ~Task_concept() = default;
    virtual rio::fut::res<T> poll() = 0;
};

template <typename Fut, typename T>
struct Task_model final : Task_concept<T>
{
    Fut fut;

    explicit Task_model(Fut f) : fut(std::move(f))
    {}

    rio::fut::res<T> poll() override
    {
        return rio::poll(fut);
    }
};

export template <typename T>
struct Task
{
    using value_type = T;

    std::unique_ptr<Task_concept<T>> ptr;

    Task() = default;

    template <Pollable Fut>
        requires std::same_as<typename std::decay_t<Fut>::value_type, T> && (!std::same_as<std::decay_t<Fut>, Task>)
    Task(Fut &&f) : ptr(std::make_unique<Task_model<std::decay_t<Fut>, T>>(std::forward<Fut>(f)))
    {}

    Task(Task &&) noexcept = default;
    Task &operator=(Task &&) noexcept = default;

    rio::fut::res<T> poll()
    {
        if (!ptr)
            return rio::fut::res<T>::error(std::make_error_code(std::errc::operation_canceled));
        return ptr->poll();
    }

    friend auto tag_invoke(rio::tag_invoke_impl::poll_t, Task &t)
    {
        return t.poll();
    }
};

} // namespace rio::fut
