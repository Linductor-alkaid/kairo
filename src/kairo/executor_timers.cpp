#include "kairo/executor.hpp"

#include <stdexcept>

// Executor facade — 定时器单元：TimerScheduler 懒创建与 hook 配置、定时线程
// 生命周期、周期任务提交（普通/可取消）、周期状态查询。
// （v0.7.0 M0：自 executor.cpp 按职责拆分，纯结构改动，零行为变化。）

namespace kairo {

void Executor::set_timer_thread_factory_for_test(
    std::function<std::thread(std::function<void()>)> factory) {
    ensure_timers().set_thread_factory_for_test(std::move(factory));
}

detail::TimerScheduler& Executor::ensure_timers() {
    // timers_ 在构造函数创建且生命周期内地址稳定；此处仅做防御性兜底
    // （例如用户在其它成员构造完成前经由合法路径进入）。
    std::lock_guard<std::mutex> lock(timers_mutex_);
    if (!timers_) {
        timers_ = std::make_shared<detail::TimerScheduler>();
        configure_timer_scheduler_hooks();
    }
    return *timers_;
}

void Executor::configure_timer_scheduler_hooks() {
    if (!timers_) {
        return;
    }
    // 取消传播 hook：向已派发任务（delayed 派发后 / periodic 在途 tick）
    // 传播排队/运行中取消。graph_handle 为空：定时任务不在任务图中。
    timers_->set_task_cancel_hook(
        [this](const std::string& task_state_id,
               const std::shared_ptr<TaskCancellationState>& state) noexcept {
            propagate_timer_task_cancel(task_state_id, state);
        });
}

void Executor::start_timer_thread() {
    ensure_timers().start();
}

void Executor::stop_timer_thread() {
    if (timers_) {
        timers_->stop();
    }
}

// 提交带句柄的周期任务
TimerHandle Executor::submit_periodic(int64_t period_ms,
                                      std::function<void()> task) {
    if (period_ms <= 0) {
        throw std::invalid_argument("period_ms must be greater than 0");
    }
    if (!task) {
        throw std::invalid_argument("task must not be null");
    }

    auto executor = manager_->get_default_async_executor_snapshot();
    const std::string executor_name = executor ? executor->get_name() : "default";
    if (!executor) {
        record_submit_rejected(
            executor_name,
            "facade_submit_periodic",
            "Async executor not initialized. Call initialize() first.");
        throw std::runtime_error("Async executor not initialized. Call initialize() first.");
    }

    const std::string task_id = generate_task_id();

    detail::TimerScheduler::TickBuilderFactory tick_builder =
        [this, executor_name, task_id, task = std::move(task)]()
            -> detail::TimerTickPlan {
        std::function<void()> pool_task =
            [this, executor_name, task_id, task]() mutable {
                try {
                    task();
                    timers_->report_tick_success(task_id);
                } catch (...) {
                    auto exception = std::current_exception();
                    timers_->report_tick_failure(
                        task_id, "Periodic task threw an exception");
                    record_periodic_task_exception(
                        executor_name,
                        task_id,
                        "Periodic task threw an exception",
                        exception);
                    throw;
                }
            };

        std::function<void()> dispatch =
            [this, executor_name, task_id,
             pool_task = std::move(pool_task)]() mutable {
            auto executor_snapshot = manager_->get_default_async_executor_snapshot();
            if (!executor_snapshot) {
                record_periodic_submit_rejected(
                    executor_name,
                    task_id,
                    "Async executor unavailable for periodic task");
                return;
            }
            if (!executor_snapshot->try_submit_task(std::move(pool_task))) {
                auto exception = std::make_exception_ptr(std::runtime_error(
                    "Async executor rejected periodic task submission"));
                record_periodic_submit_rejected(
                    executor_name,
                    task_id,
                    "Async executor rejected periodic task submission",
                    exception);
            }
        };

        return detail::TimerTickPlan{std::move(dispatch), nullptr, {}};
    };

    try {
        start_timer_thread();
    } catch (...) {
        auto exception = std::current_exception();
        record_submit_rejected(
            executor_name,
            task_id,
            "Timer thread creation failed for periodic task",
            exception);
        throw;
    }

    const std::string scheduled_id =
        ensure_timers().schedule_periodic(period_ms, task_id,
                                          std::move(tick_builder));
    if (scheduled_id.empty()) {
        auto exception = std::make_exception_ptr(std::runtime_error(
            "Timer stopped before periodic task execution"));
        record_submit_rejected(executor_name, task_id,
                               "Timer stopped before periodic task execution",
                               exception);
        throw std::runtime_error(
            "Timer stopped before periodic task execution");
    }


    return TimerHandle(task_id, timers_);
}

TimerHandle Executor::submit_periodic_cancellable(
    int64_t period_ms, std::function<void(StopToken)> task) {
    if (period_ms <= 0) {
        throw std::invalid_argument("period_ms must be greater than 0");
    }
    if (!task) {
        throw std::invalid_argument("task must not be null");
    }

    auto executor = manager_->get_default_async_executor_snapshot();
    const std::string executor_name = executor ? executor->get_name() : "default";
    if (!executor) {
        record_submit_rejected(
            executor_name,
            "facade_submit_periodic_cancellable",
            "Async executor not initialized. Call initialize() first.");
        throw std::runtime_error("Async executor not initialized. Call initialize() first.");
    }

    const std::string timer_id = generate_task_id();

    detail::TimerScheduler::TickBuilderFactory tick_builder =
        [this, timer_id, task = std::move(task)]() -> detail::TimerTickPlan {
        auto state = std::make_shared<TaskCancellationState>();
        const std::string tick_id = generate_task_id();

        std::function<void()> pool_task =
            [this, timer_id, tick_id, state, task]() mutable {
                if (!state->try_begin_execution()) {
                    timers_->release_tick(timer_id, tick_id);
                    return;  // 已取消，无 future 需要满足
                }
                try {
                    task(state->stop_token());
                } catch (const TaskCancelled&) {
                    if (state->cancel_requested()) {
                        state->try_finish_running(
                            TaskCancellationState::Phase::Cancelled);
                        timers_->release_tick(timer_id, tick_id);
                        return;  // 协作取消：生命周期事件，不记 failure
                    }
                    throw;
                } catch (...) {
                    auto exception = std::current_exception();
                    state->try_finish_running(
                        TaskCancellationState::Phase::Failed);
                    record_task_exception(
                        "default",
                        tick_id,
                        "Periodic cancellable tick threw an exception",
                        exception);
                    timers_->report_tick_failure(
                        timer_id, "Periodic task threw an exception");
                    timers_->release_tick(timer_id, tick_id);
                    throw;
                }
                state->try_finish_running(
                    TaskCancellationState::Phase::Succeeded);
                if (state->cancel_requested()) {
                    cancellation_registry_->on_completed_after_request();
                }
                timers_->report_tick_success(timer_id);
                timers_->release_tick(timer_id, tick_id);
            };

        std::function<void()> dispatch =
            [this, timer_id, tick_id, state,
             pool_task = std::move(pool_task)]() mutable {
            auto executor_snapshot = manager_->get_default_async_executor_snapshot();
            if (!executor_snapshot) {
                state->try_reject();
                record_periodic_submit_rejected(
                    "default", timer_id,
                    "Async executor unavailable for periodic task");
                return;
            }
            if (!executor_snapshot->try_submit_task(std::move(pool_task))) {
                state->try_reject();
                auto exception = std::make_exception_ptr(std::runtime_error(
                    "Async executor rejected periodic task submission"));
                record_periodic_submit_rejected(
                    "default", timer_id,
                    "Async executor rejected periodic task submission",
                    exception);
                // pool_task 不会运行，active 登记在此释放。
                timers_->release_tick(timer_id, tick_id);
            }
        };

        return detail::TimerTickPlan{std::move(dispatch), std::move(state),
                                     tick_id};
    };

    try {
        start_timer_thread();
    } catch (...) {
        auto exception = std::current_exception();
        record_submit_rejected(
            executor_name,
            timer_id,
            "Timer thread creation failed for periodic task",
            exception);
        throw;
    }

    const std::string scheduled_id =
        ensure_timers().schedule_periodic(period_ms, timer_id,
                                          std::move(tick_builder));
    if (scheduled_id.empty()) {
        auto exception = std::make_exception_ptr(std::runtime_error(
            "Timer stopped before periodic task execution"));
        record_submit_rejected(executor_name, timer_id,
                               "Timer stopped before periodic task execution",
                               exception);
        throw std::runtime_error(
            "Timer stopped before periodic task execution");
    }

    return TimerHandle(timer_id, timers_);
}

TimerStatusSummary Executor::get_timer_status_summary() const {
    if (!timers_) {
        return {};
    }
    return timers_->summary();
}

std::optional<PeriodicTaskStatus> Executor::get_periodic_task_status(
    const std::string& task_id) const {
    if (!timers_) {
        return std::nullopt;
    }
    return timers_->get_periodic_status(task_id);
}

std::vector<PeriodicTaskStatus> Executor::get_all_periodic_task_status() const {
    if (!timers_) {
        return {};
    }
    return timers_->get_all_periodic_status();
}

} // namespace kairo
