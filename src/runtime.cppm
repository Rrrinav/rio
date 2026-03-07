module;
export module rio:runtime;

import std;
import :context;
import :futures;
import :fut.task;

namespace rio {

export class Runtime
{
private:
    rio::context *ctx_;
    std::vector<rio::fut::Task<void>> active_tasks_;
    std::vector<rio::fut::Task<void>> pending_spawns_;

public:
    explicit Runtime(rio::context *c) : ctx_(c)
    {
        active_tasks_.reserve(1024);
        pending_spawns_.reserve(128);
    }

    auto ctx() -> rio::context &
    {
        return *ctx_;
    }

    template <rio::Pollable Fut>
    void spawn(Fut &&f)
    {
        pending_spawns_.emplace_back(std::forward<Fut>(f));
    }

    void start()
    {
        while (!active_tasks_.empty() || !pending_spawns_.empty()) {

            bool task_progress = false;

            if (!pending_spawns_.empty()) {
                active_tasks_.insert(
                    active_tasks_.end(),
                    std::make_move_iterator(pending_spawns_.begin()),
                    std::make_move_iterator(pending_spawns_.end()));
                pending_spawns_.clear();
                task_progress = true;
            }

            for (std::size_t i = 0; i < active_tasks_.size();) {
                auto res = active_tasks_[i].poll();

                if (res.state != rio::fut::status::pending) {
                    if (i != active_tasks_.size() - 1)
                        active_tasks_[i] = std::move(active_tasks_.back());

                    active_tasks_.pop_back();
                    task_progress = true;
                } else {
                    ++i;
                }
            }

            // THE FIX: Do not sleep if we have pending spawns waiting to be processed!
            if (task_progress || active_tasks_.empty() || !pending_spawns_.empty()) {
                ctx_->try_poll();
            } else {
                ctx_->poll();
            }

            ctx_->purge_graveyard();
        }
    }

    template <rio::Pollable Fut>
    auto block_on(Fut &&root_fut) -> std::expected<
        std::conditional_t<std::is_void_v<typename std::decay_t<Fut>::value_type>, std::monostate, typename std::decay_t<Fut>::value_type>,
        std::error_code>
    {
        using Return_type = typename std::decay_t<Fut>::value_type;

        while (true) {
            bool task_progress = false;

            auto root_res = rio::poll(root_fut);

            if (root_res.state == rio::fut::status::ready) {
                if constexpr (std::is_void_v<Return_type>)
                    return std::monostate{};
                else
                    return std::move(*root_res.value);
            }
            if (root_res.state == rio::fut::status::error) {
                return std::unexpected(root_res.err);
            }

            if (!pending_spawns_.empty()) {
                active_tasks_.insert(
                    active_tasks_.end(),
                    std::make_move_iterator(pending_spawns_.begin()),
                    std::make_move_iterator(pending_spawns_.end()));
                pending_spawns_.clear();
                task_progress = true;
            }

            for (std::size_t i = 0; i < active_tasks_.size();) {
                auto res = active_tasks_[i].poll();

                if (res.state != rio::fut::status::pending) {
                    if (i != active_tasks_.size() - 1)
                        active_tasks_[i] = std::move(active_tasks_.back());
                    active_tasks_.pop_back();
                    task_progress = true;
                } else {
                    ++i;
                }
            }

            // THE FIX: Also applied here to block_on
            if (task_progress || active_tasks_.empty() || !pending_spawns_.empty()) {
                ctx_->try_poll();
            } else {
                ctx_->poll();
            }

            ctx_->purge_graveyard();
        }
    }
};

} // namespace rio
