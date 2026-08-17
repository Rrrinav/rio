module;

export module rio:fut.task;

import std;

import :futures;

namespace rio::fut {

namespace detail {

template <typename T>
struct task_erasure
{
    struct vtable
    {
        rio::fut::res<T> (*poll)(void *state);
        void (*destroy)(void *state) noexcept;
        void (*move_construct)(void *dst, void *src) noexcept;
        std::size_t size;
        std::size_t align;
    };

    template <typename Fut>
    struct model
    {
        static rio::fut::res<T> do_poll(void *state)
        {
            return rio::poll(*static_cast<Fut *>(state));
        }

        static void do_destroy(void *state) noexcept
        {
            std::destroy_at(static_cast<Fut *>(state));
        }

        static void do_move_construct(void *dst, void *src) noexcept
        {
            std::construct_at(static_cast<Fut *>(dst), std::move(*static_cast<Fut *>(src)));
        }

        static constexpr vtable vt{
            .poll = &do_poll,
            .destroy = &do_destroy,
            .move_construct = &do_move_construct,
            .size = sizeof(Fut),
            .align = alignof(Fut),
        };
    };
};

} // namespace detail

export template <typename T>
struct Task
{
    using value_type = T;

    static constexpr std::size_t inline_capacity = 512;

    const detail::task_erasure<T>::vtable *vt_ = nullptr;
    alignas(std::max_align_t) std::byte storage_[inline_capacity]{};
    void *heap_ = nullptr;

    Task() = default;

    template <Pollable Fut>
        requires std::same_as<typename std::decay_t<Fut>::value_type, T> && (!std::same_as<std::decay_t<Fut>, Task>)
    Task(Fut &&f)
    {
        using Fut_t = std::decay_t<Fut>;

        if constexpr (sizeof(Fut_t) <= inline_capacity && alignof(Fut_t) <= alignof(std::max_align_t)) {
            std::construct_at(reinterpret_cast<Fut_t *>(storage_), std::forward<Fut>(f));
        } else {
            heap_ = new Fut_t(std::forward<Fut>(f));
        }
        vt_ = &detail::task_erasure<T>::template model<Fut_t>::vt;
    }

    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;

    Task(Task &&other) noexcept
    {
        move_from(other);
    }

    Task &operator=(Task &&other) noexcept
    {
        if (this != &other) {
            destroy();
            move_from(other);
        }
        return *this;
    }

    ~Task()
    {
        destroy();
    }

    rio::fut::res<T> poll()
    {
        if (!vt_)
            return rio::fut::res<T>::error(std::make_error_code(std::errc::operation_canceled));
        return vt_->poll(heap_ ? heap_ : storage_);
    }

    friend auto tag_invoke(rio::tag_invoke_impl::poll_t, Task &t)
    {
        return t.poll();
    }

private:
    void *state_ptr() noexcept
    {
        return heap_ ? heap_ : storage_;
    }

    void destroy() noexcept
    {
        if (vt_) {
            vt_->destroy(state_ptr());
            if (heap_) {
                ::operator delete(heap_);
                heap_ = nullptr;
            }
            vt_ = nullptr;
        }
    }

    void move_from(Task &other) noexcept
    {
        vt_ = other.vt_;
        heap_ = other.heap_;

        if (vt_) {
            if (!heap_)
                vt_->move_construct(storage_, other.storage_);

            other.vt_ = nullptr;
            other.heap_ = nullptr;
        }
    }
};

} // namespace rio::fut
