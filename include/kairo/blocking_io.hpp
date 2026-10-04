#pragma once

#include "config.hpp"
#include "stop_token.hpp"
#include "types.hpp"

#include <memory>
#include <string>

namespace kairo {

/**
 * @brief 应用实现的专属、可中断阻塞 I/O 循环。
 *
 * run() 可以阻塞等待 transport 事件，但必须在 wakeup() 后尽快返回并检查
 * StopToken。StopToken 本身不能中断第三方库的 read/poll/handle 调用。
 */
class IBlockingIoWorker {
public:
    virtual ~IBlockingIoWorker() = default;

    virtual void run(StopToken stop_token) = 0;
    virtual void wakeup() noexcept = 0;
};

/** @brief 专属阻塞 I/O worker 的 executor 接口。 */
class IBlockingIoExecutor {
public:
    virtual ~IBlockingIoExecutor() = default;

    virtual bool start() = 0;
    virtual void request_stop() noexcept = 0;
    virtual void stop() = 0;
    virtual std::string get_name() const = 0;
    virtual BlockingIoExecutorStatus get_status() const = 0;
};

/** @brief Input for Executor::start_worker(). */
struct BlockingWorkerSpec {
    std::string name;
    BlockingIoConfig config;
    std::unique_ptr<IBlockingIoWorker> worker;
};

class ExecutorManager;

/**
 * @brief Lifecycle handle for a registered Blocking I/O worker.
 *
 * The handle reports startup admission, not one-shot task completion. It is
 * valid while its originating Executor remains alive.
 */
class WorkerHandle {
public:
    WorkerHandle() = default;

    const std::string& name() const noexcept { return name_; }
    const ExecutorResult& start_result() const noexcept { return start_result_; }
    bool started() const noexcept { return start_result_.ok; }

    void request_stop() noexcept;
    void stop();
    BlockingIoExecutorStatus status() const;

private:
    friend class Executor;

    WorkerHandle(ExecutorManager* manager, std::string name, ExecutorResult start_result)
        : manager_(manager), name_(std::move(name)), start_result_(std::move(start_result)) {}

    ExecutorManager* manager_ = nullptr;
    std::string name_;
    ExecutorResult start_result_;
};

} // namespace kairo
