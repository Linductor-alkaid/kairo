#pragma once

#include "config.hpp"
#include "types.hpp"
#include "task_options.hpp"
#include "task_router.hpp"
#include "task_cancellation.hpp"
#include "timer.hpp"
#include "serial_execution_context.hpp"
#include "interfaces.hpp"
#include "executor_manager.hpp"
#include "blocking_io.hpp"
#include "lockfree_task_executor.hpp"
#include "gpu/gpu_scheduler.hpp"
#include <future>
#include <functional>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <chrono>
#include <concepts>
#include <type_traits>
#include <tuple>
#include <map>
#include <queue>
#include <deque>
#include <optional>

namespace kairo {

class TaskDependencyManager;
namespace monitor { class ExecutorMonitor; }

/**
 * @brief Executor Facade
 * 
 * 提供统一的高级 API，内部委托给 ExecutorManager。
 * 支持单例模式和实例化模式。
 * 
 * 功能：
 * - 任务提交（submit, submit_priority, submit_delayed, submit_periodic）
 * - 实时任务管理（register_realtime_task, start_realtime_task, stop_realtime_task）
 * - 监控查询（get_async_executor_status, get_realtime_executor_status）
 */
class Executor {
public:
    /**
     * @brief 获取单例实例
     * 
     * 使用全局 ExecutorManager 单例，同一进程内共享。
     * 
     * @return Executor 单例引用
     */
    static Executor& instance();

    /**
     * @brief 构造函数（实例化模式）
     * 
     * 创建独立的 Executor 实例，内部创建独立的 ExecutorManager 实例。
     * 用于资源隔离场景。
     */
    Executor();

    /**
     * @brief 析构函数（RAII）
     * 
     * 自动关闭定时器线程，ExecutorManager 析构时会自动释放所有执行器。
     */
    ~Executor();

    // 禁止拷贝和赋值
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    /**
     * @brief 初始化执行器
     * 
     * 初始化默认异步执行器（线程池）。
     * 
     * @param config 执行器配置
     * @return 是否初始化成功
     */
    bool initialize(const ExecutorConfig& config);

    /**
     * @brief 初始化执行器并返回可诊断结果
     */
    ExecutorResult initialize_ex(const ExecutorConfig& config);

    /**
     * @brief 关闭执行器
     * 
     * 关闭所有执行器（异步执行器和实时执行器）。
     * 
     * @param wait_for_tasks 是否等待任务完成（默认：true）
     */
    ShutdownResult shutdown(bool wait_for_tasks = true);

    /**
     * @brief 设置定时器线程工厂（仅用于测试）
     *
     * 允许测试注入线程创建失败，验证 start_timer_thread() 的异常回滚行为。
     */
    void set_timer_thread_factory_for_test(
        std::function<std::thread(std::function<void()>)> factory);

    /**
     * @brief 提交任务（使用默认线程池）
     * 
     * @tparam F 可调用对象类型
     * @tparam Args 参数类型
     * @param f 可调用对象
     * @param args 参数
     * @return std::future 任务执行结果的 future
     */
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type>;

    template<typename F, typename... Args>
    auto submit_with_handle(F&& f, Args&&... args)
        -> TaskSubmission<typename std::invoke_result<F, Args...>::type>;

    /** Submit a task through a FIFO serialized context while retaining facade admission. */
    template<typename F, typename... Args>
    auto submit_on(SerialExecutionContext& context, F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type>;

    template<typename F, typename... Args>
    auto submit_on_with_handle(SerialExecutionContext& context, F&& f, Args&&... args)
        -> TaskSubmission<typename std::invoke_result<F, Args...>::type>;

    template<typename F, typename... Args>
    auto submit_after(const TaskHandle& dependency, F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type>;

    template<typename F, typename... Args>
    auto submit_after(const std::vector<TaskHandle>& dependencies, F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type>;

    template<typename F, typename... Args>
    auto submit_after_with_handle(const TaskHandle& dependency, F&& f, Args&&... args)
        -> TaskSubmission<typename std::invoke_result<F, Args...>::type>;

    template<typename F, typename... Args>
    auto submit_after_with_handle(const std::vector<TaskHandle>& dependencies, F&& f, Args&&... args)
        -> TaskSubmission<typename std::invoke_result<F, Args...>::type>;

    TaskHandle when_all(std::vector<TaskHandle> dependencies);

    /**
     * @brief Configure how many terminal task-graph handles remain usable.
     *
     * A terminal handle can be used to create a later dependent task while it
     * remains retained.  Evicted handles are rejected as expired.  Active
     * dependency chains are never evicted early.
     */
    void set_task_graph_retention_capacity(size_t capacity);
    size_t task_graph_retention_capacity() const;

    /**
     * @brief 总量有界 admission 运行期配置与观测
     *
     * 上限属于本 facade 实例的默认异步提交（已接纳未结算）。调小不驱逐已
     * 接纳任务，只约束后续提交。`get_in_flight_submissions()` 在未启用时
     * 恒为 0。见 docs/design/bounded_admission.md。
     */
    void set_max_in_flight_tasks(size_t max);
    size_t get_max_in_flight_tasks() const;
    size_t get_in_flight_submissions() const;

    /**
     * @brief 提交优先级任务
     * 
     * @tparam F 可调用对象类型
     * @tparam Args 参数类型
     * @param priority 优先级（0=LOW, 1=NORMAL, 2=HIGH, 3=CRITICAL）
     * @param f 可调用对象
     * @param args 参数
     * @return std::future 任务执行结果的 future
     */
    template<typename F, typename... Args>
    auto submit_priority(int priority, F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type>;

    /**
     * @brief 提交延迟任务
     * 
     * 任务将在指定延迟时间后执行。
     * 
     * @tparam F 可调用对象类型
     * @tparam Args 参数类型
     * @param delay_ms 延迟时间（毫秒）
     * @param f 可调用对象
     * @param args 参数
     * @return std::future 任务执行结果的 future
     */
    template<typename F, typename... Args>
    auto submit_delayed(int64_t delay_ms, F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type>;

    /**
     * @brief 提交周期性任务
     * 
     * 任务将按指定周期重复执行。
     * 
     * @param period_ms 周期（毫秒）
     * @param task 任务函数
     * @return 任务 ID（可用于取消任务）
     */
    std::string submit_periodic(int64_t period_ms, std::function<void()> task);

    /**
     * @brief 取消任务
     *
     * 取消指定的周期性任务。
     *
     * @param task_id 任务 ID
     * @return 是否取消成功
     */
    bool cancel_task(const std::string& task_id);

    // ------------------------------------------------------------------
    // 任务级协作取消（C1）
    // ------------------------------------------------------------------

    /**
     * @brief 请求取消一个带句柄的任务（排队取消或运行中协作请求）。
     *
     * - 排队中（未开始执行，含依赖未满足）：任务不再执行，future 以
     *   TaskCancelled(Explicit) 就绪，不产生 failure 事件；
     * - 运行中：只置位任务的协作停止 token（不抢占、不中断）；任务通过
     *   submit_cancellable* 收到的 StopToken 轮询退出；
     * - 重复/过期句柄幂等：返回 AlreadyRequested / AlreadyCompleted /
     *   NotFound，不写 failure。
     *
     * @param handle submit_with_handle/submit_after_with_handle/
     *               submit_cancellable* 返回的句柄
     * @return 取消请求结果
     */
    TaskCancellationResponse request_task_cancel(const TaskHandle& handle) noexcept;

    /**
     * @brief 获取取消生命周期独立计数（不并入 ExecutorFailureStatus）。
     */
    CancellationStatus get_cancellation_status() const;

    /**
     * @brief 超时闭包墓地当前规模（诊断观测）。

     * on_timeout 闭包的 promise/state 捕获在输家/赢家路径转入墓地延迟析构
     * （避免定时器/池线程无同步析构结算状态，见
     * docs/design/dependency_driven_scheduling.md §6 D1 实现注记）。
     * 规模以"parked 超时触发 + 竞争输家"次数为界，shutdown 终局清空、
     * facade 析构释放；长期高频超时的进程可借此观测累积。
     */
    size_t closure_graveyard_size() const;

    /**
     * @brief 设置按句柄取消 registry 的容量（active 与 tombstone 各自上限）。
     *
     * 容量耗尽时新的可取消提交被明确拒绝（SubmitRejected 诊断），
     * 不会退化成"提交成功但无法取消"。
     */
    void set_cancellation_registry_capacity(size_t capacity);

    size_t cancellation_registry_capacity() const;

    /**
     * @brief 提交可协作取消的任务；executor 注入 StopToken 作为首参数。
     *
     * token 是任务与 executor 之间唯一的协作取消通道：request_task_cancel()
     * 在任务运行中只置位 token，任务负责轮询 stop_requested() 并自行退出。
     * 阻塞在无 wakeup 机制的调用上时，取消不会打断该调用。
     *
     * @return 句柄 + future；句柄可用于 request_task_cancel()
     */
    template<typename F, typename... Args>
    auto submit_cancellable(F&& f, Args&&... args)
        -> TaskSubmission<typename std::invoke_result<F, StopToken, Args...>::type>;

    /** @brief submit_cancellable 的优先级版本。 */
    template<typename F, typename... Args>
    auto submit_cancellable_priority(int priority, F&& f, Args&&... args)
        -> TaskSubmission<typename std::invoke_result<F, StopToken, Args...>::type>;

    /** @brief submit_cancellable 的依赖图版本（单个依赖句柄）。 */
    template<typename F, typename... Args>
    auto submit_cancellable_after(const TaskHandle& dependency, F&& f, Args&&... args)
        -> TaskSubmission<typename std::invoke_result<F, StopToken, Args...>::type>;

    /** @brief submit_cancellable 的依赖图版本（等待全部依赖）。 */
    template<typename F, typename... Args>
    auto submit_cancellable_after(const std::vector<TaskHandle>& dependencies,
                                  F&& f, Args&&... args)
        -> TaskSubmission<typename std::invoke_result<F, StopToken, Args...>::type>;

    // ------------------------------------------------------------------
    // 定时句柄（T1）
    // ------------------------------------------------------------------

    /**
     * @brief 提交带句柄的延迟任务。
     *
     * 与 submit_delayed() 的 future 语义一致，额外返回可取消/可重排的
     * TimerHandle（析构不取消）。调度线程停止（shutdown）时未到期任务以
     * TaskCancelled(Shutdown) 就绪，不产生 failure 事件。
     */
    template<typename F, typename... Args>
    auto submit_delayed_with_handle(int64_t delay_ms, F&& f, Args&&... args)
        -> TimerSubmission<typename std::invoke_result<F, Args...>::type>;

    /**
     * @brief 提交带句柄、可协作取消的延迟任务（StopToken 作为首参数注入）。
     *
     * 取消在到期前生效时任务不执行；到期派发后取消继续向排队/运行中的
     * 任务传播（CancellationRequestedAfterDispatch）。
     */
    template<typename F, typename... Args>
    auto submit_delayed_cancellable_with_handle(int64_t delay_ms, F&& f, Args&&... args)
        -> TimerSubmission<typename std::invoke_result<F, StopToken, Args...>::type>;

    /**
     * @brief 提交带句柄的周期任务。
     *
     * 与 submit_periodic() 的诊断语义一致（tick 异常进入 failure 体系与
     * PeriodicTaskStatus），额外返回 TimerHandle：cancel 阻止后续 tick，
     * reschedule_after 只改下一次到期时间、不改周期。
     */
    TimerHandle submit_periodic_with_handle(int64_t period_ms,
                                            std::function<void()> task);

    /**
     * @brief 提交带句柄、可协作取消的周期任务（StopToken 作为首参数注入）。
     *
     * 每个 tick 独立注入 token；cancel 原子阻止后续 tick 并对在途 tick
     * 请求排队/协作取消，但不撤回已取得执行权的 callback，也不等待完成。
     */
    TimerHandle submit_periodic_cancellable_with_handle(
        int64_t period_ms, std::function<void(StopToken)> task);

    /** @brief 定时任务计数快照（pending/executed/cancelled）。 */
    TimerStatusSummary get_timer_status_summary() const;


    /**
     * @brief 查询单个周期任务状态
     *
     * 返回 std::nullopt 表示任务不存在或已取消。
     */
    std::optional<PeriodicTaskStatus> get_periodic_task_status(
        const std::string& task_id) const;

    /**
     * @brief 查询所有当前注册的周期任务状态
     */
    std::vector<PeriodicTaskStatus> get_all_periodic_task_status() const;

    /**
     * @brief 批量提交任务
     *
     * 批量提交多个任务，可减少重复提交路径开销。
     * 实际性能收益取决于任务数量、任务体、线程数、硬件和构建配置。
     *
     * @tparam F 可调用对象类型
     * @param tasks 任务列表
     * @return std::vector<std::future<void>> 任务执行结果的 future 列表
     *
     * @note 不承诺固定加速比；需要性能结论时请运行本地 benchmark。
     *
     * 示例：
     * @code
     * std::vector<std::function<void()>> tasks;
     * for (int i = 0; i < 1000; ++i) {
     *     tasks.push_back([i]() { process(i); });
     * }
     * auto futures = executor.submit_batch(tasks);
     * @endcode
     */
    template<typename F>
    std::vector<std::future<void>> submit_batch(const std::vector<F>& tasks);

    /**
     * @brief 批量提交任务（无返回值版本）
     *
     * 批量提交多个任务，不返回 future，省去逐个 future 的管理开销。
     * 适用于不需要等待任务完成的场景（fire-and-forget）。
     *
     * @tparam F 可调用对象类型
     * @param tasks 任务列表
     *
     * @note 相比返回 future 的版本，避免了 packaged_task 的开销
     *
     * 示例：
     * @code
     * std::vector<std::function<void()>> tasks;
     * for (int i = 0; i < 1000; ++i) {
     *     tasks.push_back([i]() { process(i); });
     * }
     * executor.submit_batch_no_future(tasks);
     * @endcode
     */
    template<typename F>
    void submit_batch_no_future(const std::vector<F>& tasks);

    /**
     * @brief 批量提交优先级任务
     *
     * 批量提交多个优先级任务。
     *
     * @tparam F 可调用对象类型
     * @param priority 优先级（0=LOW, 1=NORMAL, 2=HIGH, 3=CRITICAL）
     * @param tasks 任务列表
     * @return std::vector<std::future<void>> 任务执行结果的 future 列表
     */
    template<typename F>
    std::vector<std::future<void>> submit_batch_priority(
        int priority,
        const std::vector<F>& tasks);

    /**
     * @brief 注册实时任务
     * 
     * 创建并注册实时执行器（专用实时线程）。
     * 
     * @param name 任务名称
     * @param config 实时线程配置
     * @return 是否注册成功
     */
    bool register_realtime_task(const std::string& name,
                               const RealtimeThreadConfig& config);

    /**
     * @brief 注册实时任务并返回可诊断结果
     */
    ExecutorResult register_realtime_task_ex(const std::string& name,
                                             const RealtimeThreadConfig& config);

    /**
     * @brief 启动实时任务
     * 
     * @param name 任务名称
     * @return 是否启动成功
     */
    bool start_realtime_task(const std::string& name);

    /**
     * @brief 启动实时任务并返回可诊断结果
     */
    ExecutorResult start_realtime_task_ex(const std::string& name);

    /**
     * @brief 停止实时任务
     * 
     * @param name 任务名称
     */
    void stop_realtime_task(const std::string& name);

    bool register_blocking_io_worker(const std::string& name,
                                     const BlockingIoConfig& config,
                                     std::unique_ptr<IBlockingIoWorker> worker);

    ExecutorResult register_blocking_io_worker_ex(
        const std::string& name,
        const BlockingIoConfig& config,
        std::unique_ptr<IBlockingIoWorker> worker);

    bool start_blocking_io_worker(const std::string& name);
    ExecutorResult start_blocking_io_worker_ex(const std::string& name);
    void stop_blocking_io_worker(const std::string& name);
    BlockingIoExecutorStatus get_blocking_io_worker_status(const std::string& name) const;
    std::vector<std::string> get_blocking_io_worker_list() const;

    /**
     * @brief Register and start a dedicated Blocking I/O worker in one facade call.
     *
     * The returned handle controls the worker lifecycle; it does not represent
     * completion of a one-shot task or transfer worker ownership.
     */
    WorkerHandle start_worker(BlockingWorkerSpec spec);

    /**
     * @brief 通过 facade 推送任务到指定实时执行器
     *
     * 失败会同时通过返回值、RealtimeExecutorStatus 计数和 facade failure event 可见。
     */
    bool push_realtime_task(const std::string& name, std::function<void()> task);

    /**
     * @brief push_realtime_task 的显式 try 命名别名
     */
    bool try_push_realtime_task(const std::string& name, std::function<void()> task);

    /**
     * @brief 获取实时执行器的非持有裸指针
     *
     * 高级逃生口。返回值不延长生命周期，不能跨或并发于 shutdown() 使用；
     * 普通任务推送请使用 push_realtime_task()。
     *
     * @param name 执行器名称
     * @return 实时执行器指针，如果不存在则返回 nullptr
     */
    IRealtimeExecutor* get_realtime_executor(const std::string& name);

    /**
     * @brief 获取所有实时任务列表
     * 
     * @return 实时任务名称列表
     */
    std::vector<std::string> get_realtime_task_list() const;

    bool register_lockfree_executor(const std::string& name,
                                    std::unique_ptr<LockFreeTaskExecutor> executor);
    bool start_lockfree_executor(const std::string& name);
    void stop_lockfree_executor(const std::string& name);
    std::vector<std::string> get_lockfree_executor_names() const;

    /**
     * @brief Dispatch to an explicitly selected bounded, fire-and-forget backend.
     *
     * `accepted` reports queue admission only; it never represents task
     * completion. `LowLatency` requires a running named lock-free executor.
     */
    DispatchResult dispatch_auto(TaskOptions options, std::function<void()> task);

    /** @brief Enumerate advisory state snapshots for every registered backend. */
    std::vector<ExecutorCapability> get_executor_capabilities() const;

    /**
     * @brief 获取异步执行器状态
     * 
     * @return 异步执行器状态
     */
    AsyncExecutorStatus get_async_executor_status() const;

    /**
     * @brief 获取实时执行器状态
     * 
     * @param name 执行器名称
     * @return 实时执行器状态
     */
    RealtimeExecutorStatus get_realtime_executor_status(const std::string& name) const;

    /**
     * @brief 设置 facade 失败事件回调
     *
     * 未设置回调时，失败事件仍会进入状态计数和最近事件缓冲。
     * callback 自身抛出的异常会被隔离，不会杀死 worker 或后台线程。
     */
    void set_failure_callback(ExecutorFailureCallback callback);

    /**
     * @brief 获取累计失败状态
     */
    ExecutorFailureStatus get_failure_status() const;

    /**
     * @brief 获取最近失败事件
     *
     * @param max_count 最多返回事件数；0 表示返回当前缓冲区内全部事件。
     * @return 按发生时间从旧到新排序的失败事件列表
     */
    std::vector<ExecutorFailureEvent> get_recent_failures(size_t max_count = 0) const;

    /**
     * @brief 清空最近失败事件
     *
     * 只清空 ring buffer，不重置累计计数。
     */
    void clear_recent_failures();

    /**
     * @brief 设置最近失败事件缓冲容量
     *
     * 容量为 0 时不保留最近事件，但累计状态和 callback 仍生效。
     */
    void set_recent_failure_capacity(size_t capacity);

    std::optional<RoutingDecision> get_last_routing_decision() const;
    std::vector<RoutingDecision> get_recent_routing_decisions(size_t max_count = 0) const;
    void clear_recent_routing_decisions();
    void set_recent_routing_capacity(size_t capacity);
    void set_routing_callback(std::function<void(const RoutingDecision&)> callback);

    /**
     * @brief 启用或禁用任务监控
     */
    void enable_monitoring(bool enable);

    /**
     * @brief 设置监控采样率
     * @param rate 采样率 (0.0-1.0)，0.01 表示 1% 采样
     */
    void set_monitoring_sampling_rate(double rate);

    /**
     * @brief Limit sampled queued/running task diagnostics retained by snapshots.
     *
     * A capacity of 0 disables in-flight retention. It does not disable the
     * existing aggregate TaskStatistics monitor.
     */
    void set_in_flight_task_capacity(size_t capacity);

    /** Set the independent sampling rate for in-flight task diagnostics. */
    void set_in_flight_task_sampling_rate(double rate);

    /**
     * @brief 按 task_type 获取任务统计
     */
    TaskStatistics get_task_statistics(const std::string& task_type) const;

    /**
     * @brief 获取全部 task_type 的任务统计
     */
    std::map<std::string, TaskStatistics> get_all_task_statistics() const;

    /**
     * @brief 等待默认异步后端已提交的 future 型任务完成
     *
     * 兼容旧调用方，最多等待 kDefaultWaitForCompletionTimeout。
     * 超时时不抛异常，但会记录 FailureKind::WaitTimeout。
     */
    void wait_for_completion();

    /**
     * @brief 等待默认异步后端已提交的 future 型任务完成并返回是否完成
     *
     * @param timeout 最长等待时间
     * @return true 表示所有任务在 timeout 内完成；false 表示等待超时。
     *         超时时记录 FailureKind::WaitTimeout，可通过 get_failure_status()
     *         观察 wait_timeout_count。
     */
    bool try_wait_for_completion(std::chrono::milliseconds timeout);

    /**
     * @brief 等待默认异步后端已提交的 future 型任务完成并返回是否完成
     */
    template<typename Rep, typename Period>
    bool wait_for_completion_for(
        const std::chrono::duration<Rep, Period>& timeout);

    /**
     * @brief 等待默认异步后端已提交的 future 型任务完成并返回诊断结果
     */
    WaitResult wait_for_completion_ex(std::chrono::milliseconds timeout);

    /**
     * @brief 当前默认异步执行器是否没有排队或执行中的任务
     */
    bool is_idle() const;

    /**
     * @brief 获取默认异步执行器完成状态快照
     */
    CompletionStatus get_completion_status() const;

    /**
     * @brief 获取 Executor 的完整生命周期诊断快照。
     *
     * 这是低频、best-effort 的只读诊断接口。查询不会创建默认异步执行器，
     * 不承诺跨后端事务级一致性，也不应在实时周期中调用。
     */
    ExecutorSnapshot get_snapshot() const;

    /**
     * @brief 返回稳定的行式生命周期快照文本，适用于日志和故障支持包。
     */
    std::string get_snapshot_text() const;

    /**
     * @brief 设置低频故障现场回调。
     *
     * 回调在 wait 超时及 facade 生命周期/注册/启动失败的调用线程执行；
     * 回调异常被隔离，且不得从实时周期或任务热路径调用此 API。
     */
    void set_snapshot_diagnostic_callback(ExecutorSnapshotCallback callback);

    /**
     * @brief 注册 GPU 执行器
     * 
     * 创建并注册 GPU 执行器。
     * 
     * @param name 执行器名称
     * @param config GPU 执行器配置
     * @return 是否注册成功
     */
    bool register_gpu_executor(const std::string& name,
                              const gpu::GpuExecutorConfig& config);

    /**
     * @brief 注册 GPU 执行器并返回可诊断结果
     */
    ExecutorResult register_gpu_executor_ex(const std::string& name,
                                            const gpu::GpuExecutorConfig& config);

    /**
     * @brief 提交 GPU kernel 任务
     * 
     * @tparam KernelFunc GPU kernel 函数类型
     * @param executor_name GPU 执行器名称
     * @param kernel GPU kernel 函数
     * @param config GPU 任务配置
     * @return std::future<void> 任务执行结果的 future
     */
    template<typename KernelFunc>
    auto submit_gpu(const std::string& executor_name,
                   KernelFunc&& kernel,
                   const gpu::GpuTaskConfig& config)
        -> std::future<void>;

    /**
     * @brief 获取 GPU 执行器的非持有裸指针
     *
     * 高级逃生口。返回值不延长生命周期，不能跨或并发于 shutdown() 使用。
     *
     * @param name 执行器名称
     * @return GPU 执行器指针，如果不存在则返回 nullptr
     */
    IGpuExecutor* get_gpu_executor(const std::string& name);

    /**
     * @brief 获取所有 GPU 执行器名称
     * 
     * @return GPU 执行器名称列表
     */
    std::vector<std::string> get_gpu_executor_names() const;

    /**
     * @brief 获取 GPU 执行器状态
     * 
     * @param name 执行器名称
     * @return GPU 执行器状态
     */
    gpu::GpuExecutorStatus get_gpu_executor_status(const std::string& name) const;

    /**
     * @brief 获取所有 GPU 执行器状态（监控查询）
     *
     * @return 执行器名称到状态的映射
     */
    std::map<std::string, gpu::GpuExecutorStatus> get_all_gpu_executor_status() const;

    /**
     * @brief 自动选择 CPU/GPU 执行器提交任务（legacy overload）
     *
     * 根据任务特征自动选择 CPU 或 GPU 执行器。
     * 如果选择 GPU，调用 submit_gpu()；如果选择 CPU，在 CPU 线程池执行。
     *
     * @deprecated 迁移期内保持现有语义：CPU 路径会以 nullptr stream 调用
     * kernel，GPU 不可用时不会隐式回退。新代码应使用 cpu_gpu_task()，由两条
     * 明确 callable 表达 CPU 与 GPU 路径。
     *
     * @tparam KernelFunc GPU kernel 函数类型
     * @param characteristics 任务特征（数据大小、计算强度等）
     * @param gpu_executor_name GPU 执行器名称（GPU 被选中时使用）
     * @param kernel GPU kernel 函数（需支持 nullptr stream 用于 CPU 执行）
     * @param gpu_config GPU 任务配置（GPU 被选中时使用）
     * @return std::future<void> 任务执行结果的 future
     */
    template<typename KernelFunc>
    auto submit_auto(
        const gpu::TaskCharacteristics& characteristics,
        const std::string& gpu_executor_name,
        KernelFunc&& kernel,
        const gpu::GpuTaskConfig& gpu_config)
        -> std::future<void>;

    /**
     * @brief 提交一般 CPU 任务到自动路由入口。
     *
     * 首版 `Auto` 只选择默认异步线程池；此 overload 与 `submit()` 保持相同的
     * future 完成语义，为后续路由诊断提供稳定入口。
     */
    template<typename F, typename... Args>
    auto submit_auto(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type>;

    /**
     * @brief 提交带任务意图的普通 callable。
     *
     * 阶段一仅接受 `Auto` 或 `GeneralCpu`，其他意图必须使用对应 typed API。
     */
    template<typename Function>
    auto submit_auto(TaskBuilder<Function> task)
        -> std::future<typename std::invoke_result<Function&>::type>;

    /**
     * @brief 提交 CPU/GPU 双路径任务。
     */
    template<typename CpuFunction, typename GpuFunction>
    std::future<void> submit_auto(CpuGpuTask<CpuFunction, GpuFunction> task);

    /**
     * @brief 更新调度器配置
     *
     * @param config 调度器配置
     */
    void update_scheduler_config(const gpu::GpuScheduler::Config& config);

    /**
     * @brief 获取调度器配置
     *
     * @return 当前调度器配置
     */
    gpu::GpuScheduler::Config get_scheduler_config() const;

private:
    // 定时器调度器（registry + generation heap + 调度线程）以 shared_ptr
    // 持有：TimerHandle 只持有 weak 锚点，Executor 析构后句柄安全失效。
    // 线程启停/代际管理封装在 detail::TimerScheduler 内。
    Executor(ExecutorManager& manager);

    /**
     * @brief 记录 facade 失败事件
     */
    void record_failure(ExecutorFailureEvent event);
    void record_routing_decision(RoutingDecision decision);
    RoutingDecision route_task(const TaskOptions& options,
                               bool cpu_gpu_task,
                               std::optional<bool> gpu_selected = std::nullopt) const;
    RoutingDecision route_dispatch(const TaskOptions& options) const;

    void record_result_failure(const ExecutorResult& result,
                               FailureKind kind,
                               const std::string& executor_name,
                               const std::string& task_id);

    void record_submit_rejected(const std::string& executor_name,
                                const std::string& task_id,
                                const std::string& message,
                                std::exception_ptr exception = nullptr);

    void record_task_exception(const std::string& executor_name,
                               const std::string& task_id,
                               const std::string& message,
                               std::exception_ptr exception);

    void record_task_timeout(const std::string& executor_name,
                             const std::string& task_id,
                             const std::string& message,
                             std::exception_ptr exception);

    void record_realtime_drop(const std::string& executor_name,
                              const std::string& task_id,
                              const std::string& message,
                              std::exception_ptr exception = nullptr);

    void record_periodic_task_exception(const std::string& executor_name,
                                        const std::string& task_id,
                                        const std::string& message,
                                        std::exception_ptr exception);

    void record_periodic_submit_rejected(const std::string& executor_name,
                                         const std::string& task_id,
                                         const std::string& message,
                                         std::exception_ptr exception = nullptr);

    /**
     * @brief submit() 的内部实现，额外暴露"提交被拒绝/未送达"观察者。
     *
     * 任务图句柄路径必须知道提交是否真正送达执行器：若提交被拒绝（执行器
     * 已停止、提交路径抛异常等），wrapper 永远不会运行，对应的任务图节点
     * 会停留在 Pending。没有这个通知，任何依赖该句柄的 submit_after /
     * when_all 都会在 worker 线程上无限期等待，耗尽线程池并挂死 shutdown。
     * on_rejected 在设置拒绝异常的同一位置被调用（可能为 null）。
     */
    template<typename F, typename... Args>
    std::future<typename std::invoke_result<F, Args...>::type>
    submit_with_rejection_observer(
        std::function<void(std::exception_ptr)> on_rejected,
        F&& f,
        Args&&... args);

    // ------------------------------------------------------------------
    // 协作取消内部管道（C1）
    // ------------------------------------------------------------------

    // 推导带/不带 token 注入的 tracked 任务返回类型。
    template <bool kInjectToken, typename F, typename... Args>
    struct tracked_invoke_result
        : std::conditional_t<
              kInjectToken,
              std::invoke_result<F, StopToken, Args...>,
              std::invoke_result<F, Args...>> {};

    /**
     * @brief 带句柄 + 共享取消状态的统一提交路径。
     *
     * 覆盖 submit_with_handle / submit_after_with_handle / submit_cancellable*
     * 的公共语义：句柄与取消 state 一一对应，排队取消、运行中协作取消、
     * queued soft timeout、提交拒绝与 worker 完成通过同一 phase CAS 仲裁，
     * promise 恰好满足一次。
     *
     * @param priority 优先级；nullopt 走普通队列
     * @param dependencies 依赖句柄；空指针表示无依赖
     * @param kInjectToken 是否向 callable 首位注入 StopToken
     */
    template <bool kInjectToken, typename F, typename... Args>
    auto submit_tracked(
        std::optional<int> priority,
        std::shared_ptr<const std::vector<TaskHandle>> dependencies,
        F&& f,
        Args&&... args)
        -> TaskSubmission<typename tracked_invoke_result<kInjectToken, F, Args...>::type>;

    template <bool kInjectToken, typename F, typename... Args>
    auto submit_tracked_with_hook(
        std::optional<int> priority,
        std::shared_ptr<const std::vector<TaskHandle>> dependencies,
        std::function<void()> terminal_hook,
        F&& f,
        Args&&... args)
        -> TaskSubmission<typename tracked_invoke_result<kInjectToken, F, Args...>::type>;

    /**
     * @brief 取消状态传播核心（排队取消 / 运行中协作请求 / 终态判定）。
     *
     * request_task_cancel()（按 registry 句柄）与定时器 cancel 的传播
     * hook（按已持有 state）共用。graph_handle 非空时同步推进任务图终态
     * 并唤醒依赖等待者。
     */
    TaskCancellationResponse propagate_cancel_state(
        const std::string& task_id,
        const std::shared_ptr<TaskCancellationState>& state,
        const TaskHandle* graph_handle) noexcept;

    /** 定时器 hook：向已派发的 delayed/periodic tick 任务传播取消。 */
    void propagate_timer_task_cancel(
        const std::string& task_state_id,
        const std::shared_ptr<TaskCancellationState>& state) noexcept;

    /** 依赖异常按设计归类：依赖被取消时改写为 DependencyCancelled。 */
    std::exception_ptr reclassify_dependency_exception(
        std::exception_ptr exception) const;

    // ------------------------------------------------------------------
    // 定时器内部管道（T1）
    // ------------------------------------------------------------------

    /** 懒创建定时器调度器（进程内单实例、稳定地址，供句柄锚定）。 */
    detail::TimerScheduler& ensure_timers();

    /** 给调度器安装取消传播 hook（构造与防御性重建时调用）。 */
    void configure_timer_scheduler_hooks();

    /** 确保调度线程已启动；线程创建失败按 legacy 语义向上抛出。 */
    void start_timer_thread();

    /** 停止调度线程并按 TaskCancelled(Shutdown) 清理未到期任务。 */
    void stop_timer_thread();

    /**
     * @brief submit_delayed 系列的统一实现。
     *
     * kInjectToken 为 true 时向 callable 首位注入 StopToken。返回句柄 +
     * future；legacy submit_delayed() 丢弃句柄保持旧返回类型。
     */
    template <bool kInjectToken, typename F, typename... Args>
    auto submit_delayed_impl(int64_t delay_ms, F&& f, Args&&... args)
        -> TimerSubmission<typename tracked_invoke_result<kInjectToken, F, Args...>::type>;

    enum class TaskGraphState {
        Pending,
        Running,
        Succeeded,
        Failed,
        WhenAll
    };

    // 依赖未就绪时驻留调度侧的提交载荷（dependency-driven scheduling，
    // 见 docs/design/dependency_driven_scheduling.md）：依赖图任务不再
    // 入队占用 worker 等待条件变量，而是停在节点上，由依赖终态级联出队。
    // wrapper / on_timeout / priority / executor 在提交时定格，ready 时
    // 原样出队提交；settle_without_run 用于依赖失败或出队被拒时，在不运行
    // callable 的前提下完成 admission 释放（计数先于 future）、future 结算、
    // registry 终结与 terminal hook。
    struct ParkedSubmission {
        std::function<void()> wrapper;
        std::function<void(std::exception_ptr)> on_timeout;
        std::function<void(std::exception_ptr)> settle_without_run;
        std::optional<int> priority;
        std::shared_ptr<IAsyncExecutor> executor;
    };

    // 级联解析在锁内收集、锁外执行的 ready 出队项。
    struct ParkedReady {
        TaskHandle handle;
        std::string executor_name;
        std::shared_ptr<ParkedSubmission> payload;
    };

    // mark_task_graph_* 的级联收集袋：传入非空指针时，mark 只收集不执行，
    // 由调用方在自己完整收尾（promise 结算、registry finalize、观测记账、
    // terminal hook）之后调用 drain_parked_resolutions——保证 dependent 的
    // future 严格晚于上游任务完全终态就绪（否则观测者按 dependent future
    // 触发 shutdown/析构时会与上游收尾并发）。缺省 nullptr 时 mark 内部
    // 立即 drain（行为等同 PR-1 初版，适用于无下游观测关键的路径）。
    struct ParkedDrainBag {
        std::vector<ParkedReady> ready;
        std::vector<std::function<void()>> failures;
    };

    struct TaskGraphNode {
        TaskGraphState state = TaskGraphState::Pending;
        std::exception_ptr exception;
        std::string error_message;
        std::vector<std::string> dependencies;
        // 未满足（未 Succeeded）依赖计数；仅 parked 节点维护。
        size_t unmet_count = 0;
        // 依赖未就绪时非空；终态或出队时清空。
        std::shared_ptr<ParkedSubmission> parked;
    };

    TaskHandle allocate_task_handle();
    bool task_handle_known_locked(const TaskHandle& handle) const;
    bool register_task_graph_dependencies(const TaskHandle& handle,
                                          const std::vector<TaskHandle>& dependencies,
                                          std::string& error_message);
    std::exception_ptr dependency_failure_locked(const std::vector<TaskHandle>& dependencies) const;
    bool dependencies_succeeded_locked(const std::vector<TaskHandle>& dependencies) const;
    void mark_task_graph_running(const TaskHandle& handle);
    void mark_task_graph_succeeded(const TaskHandle& handle,
                                   ParkedDrainBag* deferred = nullptr);
    void mark_task_graph_failed(const TaskHandle& handle,
                                std::exception_ptr exception,
                                std::string message,
                                ParkedDrainBag* deferred = nullptr);
    void resolve_task_graph_dependents_locked(
        const std::string& task_id,
        std::vector<ParkedReady>& ready_parked,
        std::vector<std::function<void()>>& deferred_failures);
    // 锁外执行级联收集的 parked 决算：失败结算先于 ready 出队；出队被拒
    // 按提交被拒路径落 Failed 并结算。
    void drain_parked_resolutions(
        std::vector<ParkedReady>& ready_parked,
        std::vector<std::function<void()>>& deferred_failures);

    // shutdown 终局：对所有仍 parked（依赖未满足、永不可能就绪）的节点
    // 统一失败结算（D2）。释放 admission/registry 槽位并级联下游，使
    // in-flight 计数收敛、future 不悬空。
    void fail_all_parked_tasks_for_shutdown();
    void finalize_task_graph_node_locked(const std::string& task_id);
    void trim_task_graph_retention_locked();
    std::exception_ptr make_dependency_exception(const std::string& message) const;

    /**
     * @brief 当前 facade 最近失败事件缓冲容量
     */
    size_t recent_failure_capacity() const;
    void emit_snapshot_diagnostic() const;
    void emit_snapshot_diagnostic(const ExecutorSnapshot& snapshot) const;

    // ExecutorManager 指针（单例或实例）
    ExecutorManager* manager_;

    // 实例化模式时拥有的 ExecutorManager
    std::unique_ptr<ExecutorManager> owned_manager_;

    // 仅由 facade 生命周期边界写入；Monitor 对运行后端状态作保守补充。
    std::atomic<ExecutorLifecycleState> lifecycle_state_{ExecutorLifecycleState::Created};
    std::unique_ptr<monitor::ExecutorMonitor> monitor_;

    mutable std::mutex snapshot_diagnostic_mutex_;
    ExecutorSnapshotCallback snapshot_diagnostic_callback_;

    // 按句柄取消 registry（active + 有界 tombstone + lifecycle 计数）。
    std::unique_ptr<TaskCancellationRegistry> cancellation_registry_;

    // 总量有界 admission（docs/design/bounded_admission.md）：上限 0 = 未启用，
    // 提交热路径零额外开销。接纳经 try_admit_submission() 单次 fetch_add，
    // 释放由 AdmissionReleaser 恰好一次完成（显式路径先于 future 结算，
    // 析构兜底覆盖池丢弃）。
    std::atomic<size_t> max_in_flight_tasks_{0};
    std::atomic<int64_t> in_flight_submissions_{0};
    // 默认异步执行器的 queued soft timeout（毫秒，0 = 不启用）。parked
    // 依赖图任务的超时定时器以此为时长、自提交时刻起算（D1，见
    // docs/design/dependency_driven_scheduling.md）：池自身的队列计时器
    // 在出队后才武装，parked 期间的预算由 facade 定时器覆盖。取值与
    // initialize_ex 的 ExecutorConfig.task_timeout_ms 一致——默认池正是
    // 由同一配置创建。
    std::atomic<int64_t> default_task_timeout_ms_{0};
    // 超时闭包墓地：输家/赢家 on_timeout 闭包的 promise/state 捕获转入
    // 此处延迟析构，避免定时器/池线程在与消费者使用异常对象无
    // happens-before 的时点上析构共享状态（TSAN 实证 data race）。
    // task_graph_mutex_ 保护；规模以超时/竞争次数为界，facade 析构释放。
    std::vector<std::shared_ptr<void>> closure_graveyard_;

    /**
     * @brief admission 容量的恰好一次释放器（内部管道类型）。
     *
     * 经 shared_ptr 被所有可能结算 future 的闭包共享；任何路径先调用
     * release()（计数先于 future 的观测不变式），析构兜底无人结算的路径
     * （如池 shutdown(false) 丢弃排队任务）。
     */
    struct AdmissionReleaser {
        std::atomic<int64_t>* counter;
        std::atomic_bool released{false};

        explicit AdmissionReleaser(std::atomic<int64_t>* c) noexcept : counter(c) {}

        void release() noexcept {
            bool expected = false;
            if (released.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                counter->fetch_sub(1, std::memory_order_release);
            }
        }

        ~AdmissionReleaser() { release(); }
    };

    struct AdmissionDecision {
        bool accepted = true;
        std::shared_ptr<AdmissionReleaser> releaser;  // null 当 admission 未启用
    };

    /** 接纳检查；拒绝时已记录 CapacityExhausted failure 事件。 */
    AdmissionDecision try_admit_submission(
        const std::string& executor_name,
        const std::string& task_id,
        const std::string& scope);

    void record_capacity_exhausted(const std::string& executor_name,
                                   const std::string& task_id,
                                   const std::string& message);

    // 定时器调度器（delayed/periodic registry + generation heap + 线程）。
    // 懒创建：地址在 Executor 生命周期内稳定，TimerHandle 经 weak_ptr 锚定。
    std::shared_ptr<detail::TimerScheduler> timers_;
    mutable std::mutex timers_mutex_;

    static constexpr size_t kDefaultRecentFailureCapacity = 128;
    static constexpr size_t kDefaultRecentRoutingCapacity = 128;

    mutable std::mutex failure_mutex_;
    ExecutorFailureStatus failure_status_;
    std::deque<ExecutorFailureEvent> recent_failures_;
    size_t recent_failure_capacity_ = kDefaultRecentFailureCapacity;
    ExecutorFailureCallback failure_callback_;

    mutable std::mutex routing_mutex_;
    std::deque<RoutingDecision> recent_routing_decisions_;
    size_t recent_routing_capacity_ = kDefaultRecentRoutingCapacity;
    std::function<void(const RoutingDecision&)> routing_callback_;
    // CR-106: 热路径观测快速开关——容量 0 且无回调时 record_routing_decision
    // 直接返回。由 set_recent_routing_capacity / set_routing_callback 维护；
    // 初始容量 kDefaultRecentRoutingCapacity > 0，故初始为观测态。
    std::atomic<bool> routing_observed_{true};
    TaskRouter task_router_;

    mutable std::mutex task_graph_mutex_;
    // PR-3：task_graph_cv_ 已退役——dependency-driven 调度下不再有 worker
    // 阻塞等待依赖，定向出队替代 notify_all 惊群。
    std::unique_ptr<TaskDependencyManager> task_dependencies_;
    std::unordered_map<std::string, TaskGraphNode> task_graph_nodes_;
    std::unordered_map<std::string, std::vector<std::string>> task_graph_dependents_;
    std::deque<std::string> task_graph_terminal_order_;
    size_t task_graph_retention_capacity_ = 1024;

    // GPU 调度器
    gpu::GpuScheduler scheduler_;
};

// 模板方法实现
template<typename Rep, typename Period>
bool Executor::wait_for_completion_for(
    const std::chrono::duration<Rep, Period>& timeout) {
    return wait_for_completion_ex(
        std::chrono::duration_cast<std::chrono::milliseconds>(timeout)).completed;
}

template<typename F, typename... Args>
auto Executor::submit(F&& f, Args&&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type> {
    return submit_with_rejection_observer(
        nullptr, std::forward<F>(f), std::forward<Args>(args)...);
}

template<typename F, typename... Args>
auto Executor::submit_on(SerialExecutionContext& context, F&& f, Args&&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type> {
    return submit_on_with_handle(context, std::forward<F>(f), std::forward<Args>(args)...).future;
}

template<typename F, typename... Args>
auto Executor::submit_on_with_handle(SerialExecutionContext& context, F&& f, Args&&... args)
    -> TaskSubmission<typename std::invoke_result<F, Args...>::type> {
    using return_type = typename std::invoke_result<F, Args...>::type;

    TaskSubmission<return_type> stopped_submission;
    auto ticket = context.reserve();
    if (!ticket) {
        auto promise = std::make_shared<std::promise<return_type>>();
        stopped_submission.future = promise->get_future();
        promise->set_exception(std::make_exception_ptr(
            ExecutorStopping("Serial execution context is stopped")));
        return stopped_submission;
    }

    TaskSubmission<return_type> submission;
    submission.handle = allocate_task_handle();
    TaskHandle handle = submission.handle;

    // 派发/结算分离（docs/design/serial_execution_context.md"派发与结算结构"）：
    // 池 worker 只做有界非阻塞发布，业务 future 由共享状态结算。旧实现把
    // mutex/cv/finished 放在 wrapper 栈上并等待串行 callback（TSAN 竞争 +
    // 多 worker 饥饿），本结构按构造消除两者。
    auto promise = std::make_shared<std::promise<return_type>>();
    auto promise_ready = std::make_shared<std::atomic_bool>(false);
    submission.future = promise->get_future();

    // CR-012：派发/守卫闭包不再捕获 SerialExecutionContext& 裸引用，改持
    // shared_state() 共享句柄。context 先于任务发布析构时（fire-and-forget
    // 常见），Shared 已 detach：post_reserved 返回 false → publish_task 走
    // 既有 ExecutorStopping 结算路径；abandon 为幂等空操作。不再存在对已
    // 析构对象的触达（此前复现为 worker 永久 futex 挂死 + shutdown 挂死）。
    auto context_state = context.shared_state();

    auto published_holder = std::make_shared<std::atomic_bool>(false);

    // 发布成功后 ticket 已被上下文消费；只有未发布路径需要 abandon 释放 FIFO。
    auto complete_terminal = [context_state, ticket = *ticket, published_holder]() {
        if (!published_holder->load(std::memory_order_acquire)) {
            context_state->abandon(ticket);
        }
    };

    auto settle_exception = [promise, promise_ready](std::exception_ptr exception) {
        bool expected = false;
        if (promise_ready->compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            promise->set_exception(exception);
        }
    };

    // 总量有界 admission（先于 registry；拒绝时 ticket 经 complete_terminal
    // 释放，后续 FIFO 不阻塞）。
    auto admission = try_admit_submission("default", handle.id(), "serial submit");
    if (!admission.accepted) {
        auto exception = std::make_exception_ptr(CapacityExhaustedException(
            "In-flight submission capacity exhausted"));
        settle_exception(exception);
        mark_task_graph_failed(handle, exception, "Serial dispatch submission rejected");
        complete_terminal();
        return submission;
    }
    auto release_admission = [releaser = std::move(admission.releaser)]() {
        if (releaser) releaser->release();
    };

    auto state = std::make_shared<TaskCancellationState>();
    state->set_completion_sink(
        [settle_exception, complete_terminal, release_admission](std::exception_ptr exception) mutable {
            release_admission();  // 计数先于 future
            settle_exception(std::move(exception));
            complete_terminal();
        });

    // registry admission：容量耗尽按提交拒绝处理，不静默失去取消能力。
    if (!cancellation_registry_->register_state(handle.id(), state)) {
        auto exception = std::make_exception_ptr(std::runtime_error(
            "Cancellation registry capacity exhausted; serial dispatch rejected"));
        release_admission();
        state->try_reject();
        settle_exception(exception);
        mark_task_graph_failed(handle, exception, "Serial dispatch submission rejected");
        record_submit_rejected(
            "default", handle.id(),
            "Cancellation registry capacity exhausted for serial dispatch", exception);
        complete_terminal();
        return submission;
    }

    auto executor_snapshot = manager_->get_default_async_executor_snapshot();
    const std::string executor_name =
        executor_snapshot ? executor_snapshot->get_name() : "default";
    if (!executor_snapshot) {
        const std::string message =
            "Async executor not initialized. Call initialize() first.";
        auto exception = std::make_exception_ptr(std::runtime_error(message));
        state->try_reject();
        settle_exception(exception);
        mark_task_graph_failed(handle, exception, message);
        record_submit_rejected(executor_name, handle.id(), message, exception);
        cancellation_registry_->finalize(handle.id());
        complete_terminal();
        throw std::runtime_error(message);
    }

    // queued soft timeout：与取消经同一 phase CAS 仲裁，只赢一次。
    // 捕获 promise/promise_ready 而非 settle_exception 闭包，便于输家/
    // 赢家路径把捕获转入 closure_graveyard_（原因同 tracked 路径：
    // 定时器线程析构结算状态与消费者使用异常对象无 happens-before）。
    auto on_timeout = [this, handle, state, executor_name, promise, promise_ready,
                       complete_terminal, release_admission](std::exception_ptr exception) mutable {
        if (!state->try_timeout_before_start()) {
            {
                std::lock_guard<std::mutex> lock(task_graph_mutex_);
                closure_graveyard_.push_back(std::move(promise));
                closure_graveyard_.push_back(std::move(state));
            }
            promise = nullptr;
            state = nullptr;
            return;  // 取消已赢或已开始执行
        }
        release_admission();  // 计数先于 future
        record_task_timeout(
            executor_name, handle.id(),
            "Serial dispatch timed out before execution", exception);
        bool expected = false;
        if (promise_ready->compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            promise->set_exception(exception);
        }
        ParkedDrainBag drain_bag;
        mark_task_graph_failed(
            handle, exception, "Serial dispatch timed out before execution",
            &drain_bag);
        cancellation_registry_->finalize(handle.id());
        complete_terminal();
        drain_parked_resolutions(drain_bag.ready, drain_bag.failures);
        {
            std::lock_guard<std::mutex> lock(task_graph_mutex_);
            closure_graveyard_.push_back(std::move(promise));
            closure_graveyard_.push_back(std::move(state));
        }
        promise = nullptr;
        state = nullptr;
    };

    std::shared_ptr<decltype(std::bind(std::forward<F>(f), std::forward<Args>(args)...))> bound;
    try {
        bound = std::make_shared<decltype(std::bind(std::forward<F>(f), std::forward<Args>(args)...))>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
    } catch (...) {
        // CR-010：bind 的 decay-copy 抛异常时，registry 槽位（:register_state）、
        // admission 计数与图节点均已登记。此前只 abandon ticket 直接 rethrow，
        // registry active 条目与 Pending 节点永久泄漏（65536 次失败后可取消
        // 提交整体失效）。对齐 executor-not-initialized 路径的终态化顺序：
        // try_reject（sink 内 release_admission + settle + complete_terminal）
        // → mark graph failed → record → finalize → complete_terminal → throw。
        auto exception = std::current_exception();
        release_admission();
        state->try_reject();
        settle_exception(exception);
        mark_task_graph_failed(handle, exception,
                               "Serial dispatch argument binding failed");
        record_submit_rejected(executor_name, handle.id(),
                               "Serial dispatch argument binding failed", exception);
        cancellation_registry_->finalize(handle.id());
        complete_terminal();
        throw;
    }

    // 串行线程上的终态化：admission 与计数、registry finalize 先于业务
    // future 就绪，延续 tracked 路径的观测不变式。级联收集经出参交还
    // serial_callback 在业务 future 结算后 drain（dependent 晚于上游
    // 完全终态就绪）。
    auto finish_success = [this, handle, state, complete_terminal,
                           release_admission](ParkedDrainBag& drain_bag) mutable {
        mark_task_graph_succeeded(handle, &drain_bag);
        if (state->cancel_requested()) {
            cancellation_registry_->on_completed_after_request();
        }
        state->try_finish_running(TaskCancellationState::Phase::Succeeded);
        cancellation_registry_->finalize(handle.id());
        release_admission();
        complete_terminal();
    };

    auto serial_callback = [this, handle, state, executor_name, bound, promise,
                            promise_ready, complete_terminal, release_admission,
                            finish_success]() mutable {
        ParkedDrainBag drain_bag;
        try {
            if constexpr (std::is_void_v<return_type>) {
                std::invoke(*bound);
                finish_success(drain_bag);
                bool expected = false;
                if (promise_ready->compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    promise->set_value();
                }
            } else {
                auto result = std::invoke(*bound);
                finish_success(drain_bag);
                bool expected = false;
                if (promise_ready->compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    promise->set_value(std::move(result));
                }
            }
            drain_parked_resolutions(drain_bag.ready, drain_bag.failures);
        } catch (...) {
            auto exception = std::current_exception();
            bool cooperative_cancel = false;
            try {
                std::rethrow_exception(exception);
            } catch (const TaskCancelled&) {
                cooperative_cancel = state->cancel_requested();
            } catch (...) {
                cooperative_cancel = false;
            }

            if (cooperative_cancel) {
                state->try_finish_running(TaskCancellationState::Phase::Cancelled);
                release_admission();  // 计数先于 future
                bool expected = false;
                if (promise_ready->compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    promise->set_exception(exception);
                }
                mark_task_graph_failed(
                    handle, exception, "Task cancelled during execution",
                    &drain_bag);
                manager_->record_in_flight_task_state(
                    handle.id(), TaskLifecycleState::Cancelled);
            } else {
                state->try_finish_running(TaskCancellationState::Phase::Failed);
                release_admission();  // 计数先于 future
                bool expected = false;
                if (promise_ready->compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    promise->set_exception(exception);
                }
                // 失败统计先于级联写入（与 tracked 路径同一不变式）。
                record_task_exception(
                    executor_name, handle.id(),
                    "Serial context task threw an exception", exception);
                mark_task_graph_failed(
                    handle, exception, "Serial context task failed", &drain_bag);
                if (state->cancel_requested()) {
                    cancellation_registry_->on_completed_after_request();
                }
            }
            cancellation_registry_->finalize(handle.id());
            complete_terminal();
            drain_parked_resolutions(drain_bag.ready, drain_bag.failures);
        }
    };

    // 派发任务被池丢弃（shutdown(false) 清队列）时释放 ticket 并兜底结算。
    // 经 shared_ptr 共享，闭包拷贝不会提前触发析构。
    struct TicketGuard {
        std::shared_ptr<typename SerialExecutionContext::Shared> context;
        SerialExecutionContext::Ticket ticket;
        std::shared_ptr<std::atomic_bool> published;
        std::shared_ptr<TaskCancellationState> state;
        std::shared_ptr<std::promise<return_type>> promise;
        std::shared_ptr<std::atomic_bool> promise_ready;
        Executor* owner = nullptr;
        TaskHandle handle;

        ~TicketGuard() {
            if (published->load(std::memory_order_acquire)) {
                return;
            }
            context->abandon(ticket);
            if (state->terminal()) {
                // 取消/超时/拒绝的结算方仍在途，由其完成 future 与图终态。
                return;
            }
            auto exception = std::make_exception_ptr(ExecutorStopping(
                "Serial dispatch dropped by executor shutdown"));
            bool expected = false;
            if (promise_ready->compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                promise->set_exception(exception);
            }
            // 被丢弃的派发按失败落图终态：parked 下游即时级联结算，
            // 不必等到 shutdown sweep（自身 future 已先结算，顺序不变式保持）。
            if (owner) {
                owner->mark_task_graph_failed(
                    handle, exception,
                    "Serial dispatch dropped by executor shutdown");
            }
        }
    };
    auto guard = std::make_shared<TicketGuard>();
    guard->context = context_state;
    guard->ticket = *ticket;
    guard->published = published_holder;
    guard->state = state;
    guard->promise = promise;
    guard->promise_ready = promise_ready;
    guard->owner = this;
    guard->handle = handle;

    auto publish_task = [this, handle, state, executor_name, context_state,
                         ticket = *ticket,
                         promise, promise_ready, published_holder, complete_terminal,
                         release_admission,
                         guard, serial_callback = std::move(serial_callback)]() mutable {
        // 开始执行仲裁（单一 CAS 线性化点）。
        if (!state->try_begin_execution()) {
            return;  // 取消/超时已先赢，future 已满足，ticket 已释放
        }

        mark_task_graph_running(handle);
        const bool accepted =
            context_state->post_reserved(ticket, std::move(serial_callback));
        published_holder->store(true, std::memory_order_release);
        if (accepted) {
            // 业务 future 由串行线程按 ticket 顺序结算；发布不等待 callback，
            // worker 立即可服务其他任务（包括更早 ticket 的发布）。
            return;
        }

        // context shutdown 等拒绝：ticket 已由上下文释放，按 ExecutorStopping
        // 结算业务 future。
        auto exception = std::make_exception_ptr(
            ExecutorStopping("Serial execution context is stopped"));
        state->try_finish_running(TaskCancellationState::Phase::Failed);
        release_admission();  // 计数先于 future
        bool expected = false;
        if (promise_ready->compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            promise->set_exception(exception);
        }
        record_task_exception(
            executor_name, handle.id(),
            "Serial dispatch rejected by stopped context", exception);
        ParkedDrainBag drain_bag;
        mark_task_graph_failed(
            handle, exception, "Serial execution context stopped before dispatch",
            &drain_bag);
        cancellation_registry_->finalize(handle.id());
        complete_terminal();
        drain_parked_resolutions(drain_bag.ready, drain_bag.failures);
    };

    try {
        if (!executor_snapshot->try_submit_task(std::move(publish_task), std::move(on_timeout))) {
            auto exception = std::make_exception_ptr(std::runtime_error(
                "Async executor rejected serial dispatch submission"));
            state->try_reject();
            settle_exception(exception);
            mark_task_graph_failed(handle, exception, "Serial dispatch submission rejected");
            cancellation_registry_->finalize(handle.id());
            record_submit_rejected(
                executor_name, handle.id(),
                "Async executor rejected serial dispatch submission", exception);
            complete_terminal();
        }
    } catch (...) {
        auto exception = std::current_exception();
        mark_task_graph_failed(handle, exception, "Serial dispatch submission failed");
        cancellation_registry_->finalize(handle.id());
        complete_terminal();
        throw;
    }

    return submission;
}

template<typename F, typename... Args>
auto Executor::submit_with_rejection_observer(
    std::function<void(std::exception_ptr)> on_rejected,
    F&& f,
    Args&&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type> {
    using return_type = typename std::invoke_result<F, Args...>::type;

    auto executor = manager_->get_default_async_executor_snapshot();
    const std::string executor_name = executor ? executor->get_name() : "default";
    const std::string task_id = "facade_submit";
    if (!executor) {
        record_submit_rejected(
            executor_name,
            task_id,
            "Async executor not initialized. Call initialize() first.");
        auto exception = std::make_exception_ptr(std::runtime_error(
            "Async executor not initialized. Call initialize() first."));
        if (on_rejected) {
            on_rejected(exception);
        }
        throw std::runtime_error(
            "Async executor not initialized. Call initialize() first.");
    }

    auto promise = std::make_shared<std::promise<return_type>>();
    auto promise_ready = std::make_shared<std::atomic_bool>(false);
    auto future = promise->get_future();

    if constexpr (sizeof...(Args) == 0) {
        if (detail::is_empty_std_function(f)) {
            auto exception = std::make_exception_ptr(std::invalid_argument("empty task"));
            promise_ready->store(true, std::memory_order_release);
            promise->set_exception(exception);
            record_submit_rejected(
                executor_name,
                task_id,
                "Async executor rejected empty task submission",
                exception);
            if (on_rejected) {
                on_rejected(exception);
            }
            return future;
        }
    }

    auto bound_task = std::make_shared<decltype(std::bind(std::forward<F>(f), std::forward<Args>(args)...))>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    // 总量有界 admission：拒绝时 future 立即以 CapacityExhaustedException
    // 就绪（不抛出），failure 事件已由 try_admit_submission 记录。
    auto admission = try_admit_submission(executor_name, task_id, "submit");
    if (!admission.accepted) {
        auto exception = std::make_exception_ptr(CapacityExhaustedException(
            "In-flight submission capacity exhausted"));
        promise_ready->store(true, std::memory_order_release);
        promise->set_exception(exception);
        return future;
    }
    auto release_admission = [releaser = std::move(admission.releaser)]() {
        if (releaser) releaser->release();
    };

    auto task_wrapper = [this, executor_name, task_id, promise, promise_ready, bound_task,
                         release_admission]() mutable {
        try {
            if constexpr (std::is_void_v<return_type>) {
                std::invoke(*bound_task);
                release_admission();
                promise->set_value();
            } else {
                auto result = std::invoke(*bound_task);
                release_admission();
                promise->set_value(std::move(result));
            }
            promise_ready->store(true, std::memory_order_release);
        } catch (...) {
            auto exception = std::current_exception();
            release_admission();
            promise->set_exception(exception);
            promise_ready->store(true, std::memory_order_release);
            record_task_exception(
                executor_name,
                task_id,
                "Async task threw an exception",
                exception);
            throw;
        }
    };

    auto on_timeout = [this, executor_name, task_id, promise, promise_ready,
                       release_admission](std::exception_ptr exception) {
        bool expected = false;
        if (promise_ready->compare_exchange_strong(expected, true)) {
            release_admission();
            promise->set_exception(exception);
            record_task_timeout(
                executor_name,
                task_id,
                "Async task timed out before execution",
                exception);
        }
    };

    if (!executor->try_submit_task(std::move(task_wrapper), std::move(on_timeout))) {
        auto exception = std::make_exception_ptr(
            std::runtime_error("Async executor rejected task submission"));
        bool expected = false;
        if (promise_ready->compare_exchange_strong(expected, true)) {
            release_admission();
            promise->set_exception(exception);
            record_submit_rejected(
                executor_name,
                task_id,
                "Async executor rejected task submission",
                exception);
        }
        // wrapper 不会运行：通知任务图路径把句柄置为 Failed，避免依赖方
        // 在 worker 线程上等待一个永不到来的终态。
        if (on_rejected) {
            on_rejected(exception);
        }
    }

    return future;
}

template<typename F, typename... Args>
auto Executor::submit_with_handle(F&& f, Args&&... args)
    -> TaskSubmission<typename std::invoke_result<F, Args...>::type> {
    return submit_tracked<false>(
        std::nullopt, nullptr, std::forward<F>(f), std::forward<Args>(args)...);
}

template<typename F, typename... Args>
auto Executor::submit_cancellable(F&& f, Args&&... args)
    -> TaskSubmission<typename std::invoke_result<F, StopToken, Args...>::type> {
    return submit_tracked<true>(
        std::nullopt, nullptr, std::forward<F>(f), std::forward<Args>(args)...);
}

template<typename F, typename... Args>
auto Executor::submit_cancellable_priority(int priority, F&& f, Args&&... args)
    -> TaskSubmission<typename std::invoke_result<F, StopToken, Args...>::type> {
    return submit_tracked<true>(
        priority, nullptr, std::forward<F>(f), std::forward<Args>(args)...);
}

template<typename F, typename... Args>
auto Executor::submit_cancellable_after(const TaskHandle& dependency,
                                        F&& f,
                                        Args&&... args)
    -> TaskSubmission<typename std::invoke_result<F, StopToken, Args...>::type> {
    auto dependencies =
        std::make_shared<const std::vector<TaskHandle>>(std::vector<TaskHandle>{dependency});
    return submit_tracked<true>(
        std::nullopt, std::move(dependencies), std::forward<F>(f),
        std::forward<Args>(args)...);
}

template<typename F, typename... Args>
auto Executor::submit_cancellable_after(const std::vector<TaskHandle>& dependencies,
                                        F&& f,
                                        Args&&... args)
    -> TaskSubmission<typename std::invoke_result<F, StopToken, Args...>::type> {
    auto dependencies_copy =
        std::make_shared<const std::vector<TaskHandle>>(dependencies);
    return submit_tracked<true>(
        std::nullopt, std::move(dependencies_copy), std::forward<F>(f),
        std::forward<Args>(args)...);
}

template<typename F, typename... Args>
auto Executor::submit_after(const TaskHandle& dependency, F&& f, Args&&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type> {
    std::vector<TaskHandle> dependencies{dependency};
    return submit_after(std::move(dependencies), std::forward<F>(f), std::forward<Args>(args)...);
}

template<typename F, typename... Args>
auto Executor::submit_after(const std::vector<TaskHandle>& dependencies, F&& f, Args&&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type> {
    return submit_after_with_handle(dependencies, std::forward<F>(f), std::forward<Args>(args)...).future;
}

template<typename F, typename... Args>
auto Executor::submit_after_with_handle(const TaskHandle& dependency, F&& f, Args&&... args)
    -> TaskSubmission<typename std::invoke_result<F, Args...>::type> {
    std::vector<TaskHandle> dependencies{dependency};
    return submit_after_with_handle(std::move(dependencies), std::forward<F>(f), std::forward<Args>(args)...);
}

template<typename F, typename... Args>
auto Executor::submit_after_with_handle(const std::vector<TaskHandle>& dependencies, F&& f, Args&&... args)
    -> TaskSubmission<typename std::invoke_result<F, Args...>::type> {
    auto dependencies_copy =
        std::make_shared<const std::vector<TaskHandle>>(dependencies);
    return submit_tracked<false>(
        std::nullopt, std::move(dependencies_copy), std::forward<F>(f),
        std::forward<Args>(args)...);
}

// submit_tracked：带句柄 + 共享取消状态的统一提交路径。
//
// 取消/超时/开始执行通过 TaskCancellationState 的单一 phase CAS 仲裁：
// - 取消先赢：任务不调用 callable，future 由 completion sink 立即以
//   TaskCancelled 就绪，任务图节点由取消方落 Failed 终态并唤醒依赖等待；
// - 超时先赢（queued soft timeout）：on_timeout 处理器完成 TimedOut 终态，
//   future 以 TimedOutException 就绪并计入 TaskTimeout 诊断；
// - worker 先赢（进入 Running）：取消只置位 StopToken（协作），callable
//   抛出的 TaskCancelled 在已请求取消时按取消归类，不触发 failure。
template <bool kInjectToken, typename F, typename... Args>
auto Executor::submit_tracked(
    std::optional<int> priority,
    std::shared_ptr<const std::vector<TaskHandle>> dependencies,
    F&& f,
    Args&&... args)
    -> TaskSubmission<typename tracked_invoke_result<kInjectToken, F, Args...>::type> {
    return submit_tracked_with_hook<kInjectToken>(
        priority, std::move(dependencies), nullptr,
        std::forward<F>(f), std::forward<Args>(args)...);
}

template <bool kInjectToken, typename F, typename... Args>
auto Executor::submit_tracked_with_hook(
    std::optional<int> priority,
    std::shared_ptr<const std::vector<TaskHandle>> dependencies,
    std::function<void()> terminal_hook,
    F&& f,
    Args&&... args)
    -> TaskSubmission<typename tracked_invoke_result<kInjectToken, F, Args...>::type> {
    using return_type =
        typename tracked_invoke_result<kInjectToken, F, Args...>::type;

    auto terminal_called = std::make_shared<std::atomic_bool>(false);
    auto complete_terminal = [terminal_hook = std::move(terminal_hook), terminal_called]() mutable {
        if (!terminal_hook) return;
        bool expected = false;
        if (terminal_called->compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            try { terminal_hook(); } catch (...) {}
        }
    };

    TaskSubmission<return_type> submission;
    submission.handle = allocate_task_handle();
    TaskHandle handle = submission.handle;

    // 依赖登记（依赖图变体）。
    std::string validation_error;
    if (dependencies && !dependencies->empty()) {
        const bool dependencies_valid =
            register_task_graph_dependencies(handle, *dependencies, validation_error);
        if (!dependencies_valid) {
            auto exception = make_dependency_exception(validation_error);
            mark_task_graph_failed(handle, exception, validation_error);
            auto promise = std::make_shared<std::promise<return_type>>();
            submission.future = promise->get_future();
            promise->set_exception(exception);
            record_submit_rejected("default", handle.id(), validation_error, exception);
            complete_terminal();
            return submission;
        }
        manager_->record_in_flight_task_state(
            handle.id(), TaskLifecycleState::DependencyBlocked);
    }

    auto promise = std::make_shared<std::promise<return_type>>();
    auto promise_ready = std::make_shared<std::atomic_bool>(false);
    submission.future = promise->get_future();

    // 总量有界 admission（先于 registry，见 docs/design/bounded_admission.md
    // D-2）：拒绝的提交不占用 registry 槽位；releaser 被所有可能结算 future
    // 的闭包共享，显式路径先于结算释放，析构兜底池丢弃路径。
    auto admission = try_admit_submission("default", handle.id(), "tracked submit");
    if (!admission.accepted) {
        auto exception = std::make_exception_ptr(CapacityExhaustedException(
            "In-flight submission capacity exhausted"));
        bool expected = false;
        if (promise_ready->compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            promise->set_exception(exception);
        }
        mark_task_graph_failed(handle, exception, "Tracked task submission rejected");
        complete_terminal();
        return submission;
    }
    auto release_admission = [releaser = std::move(admission.releaser)]() {
        if (releaser) releaser->release();
    };

    auto state = std::make_shared<TaskCancellationState>();
    state->set_completion_sink(
        [promise, promise_ready, complete_terminal, release_admission](std::exception_ptr exception) mutable {
            release_admission();  // 计数先于 future（观测不变式）
            bool expected = false;
            if (promise_ready->compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                promise->set_exception(exception);
            }
            complete_terminal();
        });

    // registry admission：容量耗尽按提交拒绝处理，不静默失去取消能力。
    if (!cancellation_registry_->register_state(handle.id(), state)) {
        auto exception = std::make_exception_ptr(std::runtime_error(
            "Cancellation registry capacity exhausted; tracked task rejected"));
        release_admission();
        state->try_reject();
        bool expected = false;
        if (promise_ready->compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            promise->set_exception(exception);
        }
        mark_task_graph_failed(handle, exception, "Tracked task submission rejected");
        record_submit_rejected(
            "default", handle.id(),
            "Cancellation registry capacity exhausted for tracked task", exception);
        complete_terminal();
        return submission;
    }

    // 注意：与 submit() 不同，这里不做 empty-std::function 预检。旧语义是
    // 空 std::function 真正入队、执行时抛 bad_function_call，任务图节点经
    // 执行异常路径落 Failed（见 test_task_graph_rejected_dependency）。

    auto executor_snapshot = manager_->get_default_async_executor_snapshot();
    const std::string executor_name =
        executor_snapshot ? executor_snapshot->get_name() : "default";
    if (!executor_snapshot) {
        const std::string message =
            "Async executor not initialized. Call initialize() first.";
        auto exception = std::make_exception_ptr(std::runtime_error(message));
        state->try_reject();
        mark_task_graph_failed(handle, exception, message);
        record_submit_rejected(executor_name, handle.id(), message, exception);
        cancellation_registry_->finalize(handle.id());
        complete_terminal();
        throw std::runtime_error(message);
    }

    // 提交被拒时 wrapper 永不运行：节点必须落 Failed，否则依赖它的后续
    // 任务会在 worker 线程上等待 Pending 节点直到挂死。
    auto on_rejected = [this, handle, state, promise, promise_ready, complete_terminal,
                        release_admission](std::exception_ptr exception) mutable {
        release_admission();  // 计数先于 future
        state->try_reject();
        bool expected = false;
        if (promise_ready->compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            promise->set_exception(exception);
        }
        ParkedDrainBag drain_bag;
        mark_task_graph_failed(handle, exception, "Tracked task submission rejected",
                               &drain_bag);
        cancellation_registry_->finalize(handle.id());
        complete_terminal();
        drain_parked_resolutions(drain_bag.ready, drain_bag.failures);
    };

    // queued soft timeout：与取消经同一 phase CAS 仲裁，只赢一次。
    // 输家/赢家路径结束时 promise/state 转入 closure_graveyard_ 延迟析构：
    // 这两个捕获的析构链会释放 future 共享状态与异常对象，若发生在
    // 定时器/池线程上，与消费者使用异常对象之间没有任何 happens-before
    // 边（TSAN 实证 data race）；转入墓地后析构时点归 facade 终局所有。
    auto on_timeout = [this, handle, state, executor_name, promise, promise_ready,
                       complete_terminal,
                       release_admission](std::exception_ptr exception) mutable {
        if (!state->try_timeout_before_start()) {
            // 输家即释：结算已由赢家完成，捕获转入墓地，不在本线程析构。
            {
                std::lock_guard<std::mutex> lock(task_graph_mutex_);
                closure_graveyard_.push_back(std::move(promise));
                closure_graveyard_.push_back(std::move(state));
            }
            promise = nullptr;
            state = nullptr;
            return;  // 取消已赢或已开始执行
        }
        release_admission();  // 计数先于 future
        // 失败统计先于自身 future 结算（与 PR-1 级联顺序同一不变式）。
        record_task_timeout(
            executor_name, handle.id(),
            "Tracked async task timed out before execution", exception);
        bool expected = false;
        if (promise_ready->compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            promise->set_exception(exception);
        }
        ParkedDrainBag drain_bag;
        mark_task_graph_failed(
            handle, exception, "Tracked async task timed out before execution",
            &drain_bag);
        cancellation_registry_->finalize(handle.id());
        complete_terminal();
        drain_parked_resolutions(drain_bag.ready, drain_bag.failures);
        {
            std::lock_guard<std::mutex> lock(task_graph_mutex_);
            closure_graveyard_.push_back(std::move(promise));
            closure_graveyard_.push_back(std::move(state));
        }
        promise = nullptr;
        state = nullptr;
    };

    // 不运行 callable 的 parked 结算路径（依赖失败 / 出队被拒）：phase CAS
    // 仲裁保证与取消/超时只赢一方，"计数先于 future" 与提交拒绝路径同序。
    // 异常归类由调用方完成（依赖失败先经 reclassify_dependency_exception）。
    auto settle_without_run = [this, handle, state, promise, promise_ready,
                               complete_terminal,
                               release_admission](std::exception_ptr exception) mutable {
        if (state->try_reject()) {
            release_admission();  // 计数先于 future
            bool expected = false;
            if (promise_ready->compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                promise->set_exception(exception);
            }
        }
        cancellation_registry_->finalize(handle.id());
        complete_terminal();
    };

    // CR-011：make_tuple 的 decay-copy（含 reference_wrapper 解包语义，类型
    // 须经 decltype 精确保留）是 tracked 提交在注册之后唯一现实可抛的构造
    // 步骤——此前异常直接穿出，registry 槽位、admission 计数与图节点全部
    // 泄漏，上游 dependents 反向边永不摘除导致任务图无界增长。
    // 经 optional::emplace 原地构造：不要求元素类型可默认构造/可赋值（直用
    // 具名 tuple 会造成 API 回归，复验时已实测），对元素的要求与原 lambda
    // 捕获的移动语义完全一致。
    using BoundArgs = decltype(std::make_tuple(std::declval<Args>()...));
    std::optional<BoundArgs> bound_args;
    try {
        bound_args.emplace(std::make_tuple(std::forward<Args>(args)...));
    } catch (...) {
        auto exception = std::current_exception();
        release_admission();
        state->try_reject();
        bool expected = false;
        if (promise_ready->compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            promise->set_exception(exception);
        }
        mark_task_graph_failed(handle, exception,
                               "Tracked task argument binding failed");
        record_submit_rejected(executor_name, handle.id(),
                               "Tracked task argument binding failed", exception);
        cancellation_registry_->finalize(handle.id());
        complete_terminal();
        throw;
    }

    auto invoke_user_callable =
        [f = std::forward<F>(f),
         args_tuple = std::move(*bound_args),
         state](StopToken token) mutable -> return_type {
        if constexpr (kInjectToken) {
            return std::apply(
                [&f, &token](auto&&... unpacked) -> return_type {
                    return f(token, std::forward<decltype(unpacked)>(unpacked)...);
                },
                std::move(args_tuple));
        } else {
            (void)token;
            return std::apply(std::move(f), std::move(args_tuple));
        }
    };

    auto task_wrapper = [this,
                         handle,
                         state,
                         executor_name,
                         promise,
                         promise_ready,
                         complete_terminal,
                         release_admission,
                         invoke = std::move(invoke_user_callable)]() mutable {
        // PR-3：依赖等待块退役。dependency-driven 调度下依赖图任务只在
        // 依赖全部 Succeeded 后才会被入队（park 决策与终态级联双入口，
        // 终态不可逆），运行时无需任何依赖检查，task_graph_cv_ 随之删除。
        // 开始执行仲裁（单一 CAS 线性化点）。
        if (!state->try_begin_execution()) {
            return;  // 取消/超时已先赢，future 已满足
        }

        mark_task_graph_running(handle);
        ParkedDrainBag drain_bag;
        try {
            // 观测不变式：future 就绪之前，取消生命周期计数必须已最终化
            // （否则等待 future 后立即读 get_cancellation_status() 会与
            // worker 侧计数竞态）。
            if constexpr (std::is_void_v<return_type>) {
                invoke(state->stop_token());
                mark_task_graph_succeeded(handle, &drain_bag);
                if (state->cancel_requested()) {
                    // 运行中收到停止请求后仍正常完成：保留业务结果，只计数。
                    cancellation_registry_->on_completed_after_request();
                }
                state->try_finish_running(TaskCancellationState::Phase::Succeeded);
                release_admission();  // 计数先于 future
                promise->set_value();
            } else {
                auto result = invoke(state->stop_token());
                mark_task_graph_succeeded(handle, &drain_bag);
                if (state->cancel_requested()) {
                    cancellation_registry_->on_completed_after_request();
                }
                state->try_finish_running(TaskCancellationState::Phase::Succeeded);
                release_admission();  // 计数先于 future
                promise->set_value(std::move(result));
            }
            promise_ready->store(true, std::memory_order_release);
            cancellation_registry_->finalize(handle.id());
            complete_terminal();
            // dependent 的出队/结算必须晚于本任务自身完全终态（future 就绪、
            // registry finalize、terminal hook），否则依赖本任务 future 的
            // 观测者可在上游收尾前触发 shutdown/析构（UAF 窗口）。
            drain_parked_resolutions(drain_bag.ready, drain_bag.failures);
        } catch (...) {
            auto exception = std::current_exception();
            // 判定是否为"已请求取消后的协作退出"：只有 stop state 已被请求
            // 时 TaskCancelled 才按取消归类；无取消请求时主动抛出的
            // TaskCancelled 仍按任务异常处理，防止绕过 failure 统计。
            bool cooperative_cancel = false;
            try {
                std::rethrow_exception(exception);
            } catch (const TaskCancelled&) {
                cooperative_cancel = state->cancel_requested();
            } catch (...) {
                cooperative_cancel = false;
            }

            if (cooperative_cancel) {
                // 运行中协作取消：任务记为 Cancelled，不触发 failure。
                state->try_finish_running(
                    TaskCancellationState::Phase::Cancelled);
                release_admission();  // 计数先于 future
                bool expected = false;
                if (promise_ready->compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    promise->set_exception(exception);
                }
                mark_task_graph_failed(
                    handle, exception, "Task cancelled during execution",
                    &drain_bag);
                manager_->record_in_flight_task_state(
                    handle.id(), TaskLifecycleState::Cancelled);
                cancellation_registry_->finalize(handle.id());
                complete_terminal();
                drain_parked_resolutions(drain_bag.ready, drain_bag.failures);
                return;
            }

            state->try_finish_running(TaskCancellationState::Phase::Failed);
            release_admission();  // 计数先于 future
            bool expected = false;
            if (promise_ready->compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                promise->set_exception(exception);
            }
            // 失败统计必须先于级联写入：PR-1 起 dependent 的 future 在
            // mark_task_graph_failed 内同步结算，观测者据此读取失败面板时
            // 计数必须已可见。
            record_task_exception(
                executor_name, handle.id(),
                "Tracked async task threw an exception", exception);
            mark_task_graph_failed(handle, exception, "Tracked task failed",
                                   &drain_bag);
            if (state->cancel_requested()) {
                cancellation_registry_->on_completed_after_request();
            }
            cancellation_registry_->finalize(handle.id());
            complete_terminal();
            drain_parked_resolutions(drain_bag.ready, drain_bag.failures);
            throw;
        }
    };

    // dependency-driven 调度（PR-1，见 docs/design/dependency_driven_scheduling.md）：
    // 依赖图任务在入队前做 park 决策——依赖全部成功才入队；存在失败则不运行
    // callable 直接结算；否则驻留节点，由依赖终态级联在调度侧唤醒（出队），
    // 不再占用 worker 等待条件变量。wrapper 内的既有依赖等待块保留为安全网
    // （parked 出队后谓词立即通过），PR-3 退役。
    if (dependencies && !dependencies->empty()) {
        bool submit_now = false;
        bool parked_stored = false;
        std::function<void(std::exception_ptr)> timer_on_timeout;
        std::exception_ptr dependency_exception;
        {
            std::lock_guard<std::mutex> lock(task_graph_mutex_);
            dependency_exception = dependency_failure_locked(*dependencies);
            if (!dependency_exception) {
                if (dependencies_succeeded_locked(*dependencies)) {
                    submit_now = true;
                } else {
                    auto node_it = task_graph_nodes_.find(handle.id());
                    size_t unmet = 0;
                    for (const auto& dependency : *dependencies) {
                        const auto dep_it = task_graph_nodes_.find(dependency.id());
                        if (dep_it == task_graph_nodes_.end() ||
                            dep_it->second.state != TaskGraphState::Succeeded) {
                            ++unmet;
                        }
                    }
                    if (node_it != task_graph_nodes_.end()) {
                        node_it->second.unmet_count = unmet;
                        auto parked = std::make_shared<ParkedSubmission>();
                        timer_on_timeout = on_timeout;  // 定时器持有一份拷贝
                        parked->wrapper = std::move(task_wrapper);
                        parked->on_timeout = std::move(on_timeout);
                        parked->settle_without_run = settle_without_run;
                        parked->priority = priority;
                        parked->executor = executor_snapshot;
                        node_it->second.parked = std::move(parked);
                        parked_stored = true;
                    } else {
                        // 节点缺失只可能是终态被 trim 后复用 id 的非法场景；
                        // 防御性按提交失败处理，不静默吞掉任务。
                        dependency_exception = make_dependency_exception(
                            "task graph node missing at park decision");
                    }
                }
            }
        }
        if (dependency_exception) {
            dependency_exception =
                reclassify_dependency_exception(dependency_exception);
            ParkedDrainBag drain_bag;
            mark_task_graph_failed(
                handle,
                dependency_exception,
                "Dependency failed before dependent task execution",
                &drain_bag);
            settle_without_run(dependency_exception);
            drain_parked_resolutions(drain_bag.ready, drain_bag.failures);
            return submission;
        }
        if (!submit_now) {
            // D1：queued soft timeout 自提交起算，parked 期间同样可触发。
            // 池自身队列计时器在出队后才武装，且与 facade 定时器经同一
            // phase CAS 仲裁——双计时器恰好一个赢家，后到者直接返回。
            // 定时器不随任务提前终态取消：获胜者之外的 on_timeout 在 CAS
            // 落败处返回，代价是至多一个到期空转记录（寿命 ≤ 超时时长）。
            const auto timeout_ms =
                default_task_timeout_ms_.load(std::memory_order_acquire);
            if (parked_stored && timeout_ms > 0) {
                // ensure_timers() 只创建调度器对象；调度线程需显式启动
                // （全新 facade 此前可能从未提交过 delayed/periodic）。
                bool timer_running = false;
                try {
                    start_timer_thread();
                    timer_running = true;
                } catch (...) {
                    // 线程创建失败：任务本身仍由依赖驱动，降级为无超时
                    // 覆盖继续 parked，不以超时能力缺失否决提交。
                }
                if (timer_running) {
                    auto timeout_exception = std::make_exception_ptr(
                        TimedOutException("Task timed out after " +
                                          std::to_string(timeout_ms) + "ms"));
                    const auto scheduled_id = ensure_timers().schedule_once(
                        timeout_ms,
                        generate_task_id(),
                        nullptr,
                        {},
                    [timer_on_timeout = std::move(timer_on_timeout),
                     exception = std::move(timeout_exception)]() mutable {
                        timer_on_timeout(exception);
                        // 调用后即释本拷贝：闭包链不得在定时器线程的任意
                        // 析构时点锚定结算状态（ graveyard 之外仍可能持有
                        // complete_terminal/releaser 等幂等成员，就地清空）。
                        timer_on_timeout = nullptr;
                        exception = nullptr;
                    },
                        nullptr);
                    // scheduled_id 为空表示调度器已停止（shutdown 竞态）：
                    // 同样降级，D2 sweep 兜底结算。
                }
            }
            return submission;  // parked：依赖终态时由调度侧级联出队
        }
    }

    try {
        bool accepted = false;
        if (priority) {
            accepted = executor_snapshot->try_submit_priority_task(
                *priority, std::move(task_wrapper), std::move(on_timeout));
        } else {
            accepted = executor_snapshot->try_submit_task(
                std::move(task_wrapper), std::move(on_timeout));
        }
        if (!accepted) {
            auto exception = std::make_exception_ptr(std::runtime_error(
                "Async executor rejected task submission"));
            on_rejected(exception);
            record_submit_rejected(
                executor_name, handle.id(),
                "Async executor rejected tracked task submission", exception);
        }
    } catch (...) {
        auto exception = std::current_exception();
        mark_task_graph_failed(handle, exception, "Tracked task submission failed");
        cancellation_registry_->finalize(handle.id());
        complete_terminal();
        throw;
    }

    return submission;
}

template<typename F, typename... Args>
auto Executor::submit_priority(int priority, F&& f, Args&&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type> {
    using return_type = typename std::invoke_result<F, Args...>::type;

    auto executor = manager_->get_default_async_executor_snapshot();
    const std::string executor_name = executor ? executor->get_name() : "default";
    const std::string task_id = "facade_submit_priority";
    if (!executor) {
        record_submit_rejected(
            executor_name,
            task_id,
            "Async executor not initialized. Call initialize() first.");
        throw std::runtime_error("Async executor not initialized. Call initialize() first.");
    }

    auto promise = std::make_shared<std::promise<return_type>>();
    auto promise_ready = std::make_shared<std::atomic_bool>(false);
    auto future = promise->get_future();

    if constexpr (sizeof...(Args) == 0) {
        if (detail::is_empty_std_function(f)) {
            auto exception = std::make_exception_ptr(std::invalid_argument("empty task"));
            promise_ready->store(true, std::memory_order_release);
            promise->set_exception(exception);
            record_submit_rejected(
                executor_name,
                task_id,
                "Async executor rejected empty priority task submission",
                exception);
            return future;
        }
    }

    auto bound_task = std::make_shared<decltype(std::bind(std::forward<F>(f), std::forward<Args>(args)...))>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    // 总量有界 admission（与 submit 相同语义：拒绝不抛出，future 立即就绪）。
    auto admission = try_admit_submission(executor_name, task_id, "submit_priority");
    if (!admission.accepted) {
        auto exception = std::make_exception_ptr(CapacityExhaustedException(
            "In-flight submission capacity exhausted"));
        promise_ready->store(true, std::memory_order_release);
        promise->set_exception(exception);
        return future;
    }
    auto release_admission = [releaser = std::move(admission.releaser)]() {
        if (releaser) releaser->release();
    };

    auto task_wrapper = [this, executor_name, task_id, promise, promise_ready, bound_task,
                         release_admission]() mutable {
        try {
            if constexpr (std::is_void_v<return_type>) {
                std::invoke(*bound_task);
                release_admission();
                promise->set_value();
            } else {
                auto result = std::invoke(*bound_task);
                release_admission();
                promise->set_value(std::move(result));
            }
            promise_ready->store(true, std::memory_order_release);
        } catch (...) {
            auto exception = std::current_exception();
            release_admission();
            promise->set_exception(exception);
            promise_ready->store(true, std::memory_order_release);
            record_task_exception(
                executor_name,
                task_id,
                "Priority async task threw an exception",
                exception);
            throw;
        }
    };

    auto on_timeout = [this, executor_name, task_id, promise, promise_ready,
                       release_admission](std::exception_ptr exception) {
        bool expected = false;
        if (promise_ready->compare_exchange_strong(expected, true)) {
            release_admission();
            promise->set_exception(exception);
            record_task_timeout(
                executor_name,
                task_id,
                "Priority async task timed out before execution",
                exception);
        }
    };

    if (!executor->try_submit_priority_task(
            priority, std::move(task_wrapper), std::move(on_timeout))) {
        auto exception = std::make_exception_ptr(
            std::runtime_error("Async executor rejected priority task submission"));
        bool expected = false;
        if (promise_ready->compare_exchange_strong(expected, true)) {
            release_admission();
            promise->set_exception(exception);
            record_submit_rejected(
                executor_name,
                task_id,
                "Async executor rejected priority task submission",
                exception);
        }
    }

    return future;
}

template<typename F, typename... Args>
auto Executor::submit_delayed(int64_t delay_ms, F&& f, Args&&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type> {
    // legacy 变体：保持只返回 future；句柄被丢弃，因此不可按句柄取消。
    return submit_delayed_impl<false>(
        delay_ms, std::forward<F>(f), std::forward<Args>(args)...).future;
}

template<typename F, typename... Args>
auto Executor::submit_delayed_with_handle(int64_t delay_ms, F&& f, Args&&... args)
    -> TimerSubmission<typename std::invoke_result<F, Args...>::type> {
    return submit_delayed_impl<false>(
        delay_ms, std::forward<F>(f), std::forward<Args>(args)...);
}

template<typename F, typename... Args>
auto Executor::submit_delayed_cancellable_with_handle(
    int64_t delay_ms, F&& f, Args&&... args)
    -> TimerSubmission<typename std::invoke_result<F, StopToken, Args...>::type> {
    return submit_delayed_impl<true>(
        delay_ms, std::forward<F>(f), std::forward<Args>(args)...);
}

// submit_delayed_impl：delayed 系列的统一实现。
//
// - 到期前（Scheduled）cancel：任务不执行，future 以 TaskCancelled(Explicit)
//   就绪（TimerScheduler 在锁内完成状态仲裁后回调 on_cancelled）；
// - 到期派发后 cancel：经共享 TaskCancellationState 继续向排队/运行中的
//   任务传播（CancellationRequestedAfterDispatch）；
// - shutdown 清理未到期任务：future 以 TaskCancelled(Shutdown) 就绪，
//   不产生 failure 事件（生命周期事件，非失败）；
// - queued soft timeout / 派发被拒：保持既有 TimedOutException /
//   SubmitRejected 诊断语义。
template <bool kInjectToken, typename F, typename... Args>
auto Executor::submit_delayed_impl(int64_t delay_ms, F&& f, Args&&... args)
    -> TimerSubmission<typename tracked_invoke_result<kInjectToken, F, Args...>::type> {
    using return_type =
        typename tracked_invoke_result<kInjectToken, F, Args...>::type;

    TimerSubmission<return_type> submission;

    // legacy 语义：默认异步执行器未初始化时，提交立即失败（而非等到期）。
    {
        auto executor = manager_->get_default_async_executor_snapshot();
        if (!executor) {
            record_submit_rejected(
                "default",
                "facade_submit_delayed",
                "Async executor not initialized. Call initialize() first.");
            throw std::runtime_error(
                "Async executor not initialized. Call initialize() first.");
        }
    }

    auto promise = std::make_shared<std::promise<return_type>>();
    auto promise_ready = std::make_shared<std::atomic_bool>(false);
    submission.future = promise->get_future();

    // cancellable 变体：任务取消状态从登记时刻即存在，关闭"到期派发瞬间
    // cancel 找不到传播目标"的竞争窗口。
    std::shared_ptr<TaskCancellationState> state;
    if constexpr (kInjectToken) {
        state = std::make_shared<TaskCancellationState>();
        state->set_completion_sink(
            [promise, promise_ready](std::exception_ptr exception) {
                bool expected = false;
                if (promise_ready->compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    promise->set_exception(exception);
                }
            });
    }

    const std::string timer_id = generate_task_id();
    const std::string task_state_id = timer_id + "#run";
    const std::string task_id = kInjectToken ? task_state_id
                                             : std::string("facade_submit_delayed");

    // 到期派发闭包：把已包装任务提交到默认异步执行器。
    auto dispatch = [this, state, task_state_id, task_id, promise, promise_ready,
                     f = std::forward<F>(f),
                     args_tuple = std::make_tuple(std::forward<Args>(args)...)]() mutable {
        auto executor = manager_->get_default_async_executor_snapshot();
        const std::string executor_name =
            executor ? executor->get_name() : "default";

        auto reject = [this, state, executor_name, task_id, promise,
                       promise_ready](const char* message) {
            auto exception = std::make_exception_ptr(std::runtime_error(message));
            if (state) {
                state->try_reject();
            }
            bool expected = false;
            if (promise_ready->compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                promise->set_exception(exception);
            }
            record_submit_rejected(executor_name, task_id, message, exception);
        };

        if (!executor) {
            reject("Async executor unavailable for delayed task");
            return;
        }

        auto pool_task = [this, state, executor_name, task_id, promise,
                          promise_ready, f = std::move(f),
                          args_tuple = std::move(args_tuple)]() mutable {
            if (state && !state->try_begin_execution()) {
                return;  // 已取消/已超时，future 已满足
            }
            try {
                if constexpr (std::is_void_v<return_type>) {
                    if constexpr (kInjectToken) {
                        std::apply(
                            [&f, token = state->stop_token()](auto&&... unpacked) {
                                f(token,
                                  std::forward<decltype(unpacked)>(unpacked)...);
                            },
                            std::move(args_tuple));
                    } else {
                        std::apply(std::move(f), std::move(args_tuple));
                    }
                    promise->set_value();
                } else {
                    if constexpr (kInjectToken) {
                        auto result = std::apply(
                            [&f, token = state->stop_token()](
                                auto&&... unpacked) -> return_type {
                                return f(
                                    token,
                                    std::forward<decltype(unpacked)>(unpacked)...);
                            },
                            std::move(args_tuple));
                        promise->set_value(std::move(result));
                    } else {
                        auto result = std::apply(
                            std::move(f), std::move(args_tuple));
                        promise->set_value(std::move(result));
                    }
                }
                if (state) {
                    // 观测不变式：future 就绪前完成取消后完成计数（与
                    // submit_tracked 一致）。
                    if (state->cancel_requested()) {
                        cancellation_registry_->on_completed_after_request();
                    }
                    state->try_finish_running(
                        TaskCancellationState::Phase::Succeeded);
                }
                promise_ready->store(true, std::memory_order_release);
            } catch (const TaskCancelled&) {
                if (!state || !state->cancel_requested()) {
                    throw;  // 无取消请求：按任务异常处理
                }
                state->try_finish_running(TaskCancellationState::Phase::Cancelled);
                auto exception = std::current_exception();
                bool expected = false;
                if (promise_ready->compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    promise->set_exception(exception);
                }
                // 协作取消是生命周期事件，不记 failure。
            } catch (...) {
                auto exception = std::current_exception();
                if (state) {
                    state->try_finish_running(
                        TaskCancellationState::Phase::Failed);
                }
                bool expected = false;
                if (promise_ready->compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    promise->set_exception(exception);
                }
                record_task_exception(
                    executor_name,
                    task_id,
                    "Delayed async task threw an exception",
                    exception);
                throw;
            }
        };

        auto on_timeout = [this, state, executor_name, task_id, promise,
                           promise_ready](std::exception_ptr exception) {
            if (state && !state->try_timeout_before_start()) {
                return;  // 取消已赢或已开始执行
            }
            bool expected = false;
            if (promise_ready->compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                promise->set_exception(exception);
            }
            record_task_timeout(
                executor_name,
                task_id,
                "Delayed async task timed out before execution",
                exception);
        };

        if (!executor->try_submit_task(std::move(pool_task), std::move(on_timeout))) {
            reject("Async executor rejected delayed task submission");
        }
    };

    // 派发前取消回调：TimerScheduler 在锁内完成 Scheduled -> Cancelled 仲裁
    // 后调用；future 以 TaskCancelled(Explicit) 就绪，无 failure 事件。
    auto on_cancelled = [state, promise, promise_ready](
                            std::exception_ptr exception) {
        bool expected = false;
        if (state) {
            if (!state->try_cancel_before_start()) {
                return;  // 任务侧已终态（并发窗口），future 已满足
            }
            state->notify_cancelled(exception);
            return;
        }
        if (promise_ready->compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            promise->set_exception(exception);
        }
    };

    const std::string executor_check_name = "default";
    try {
        start_timer_thread();
    } catch (...) {
        auto exception = std::current_exception();
        bool expected = false;
        if (promise_ready->compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            promise->set_exception(exception);
        }
        record_submit_rejected(
            executor_check_name,
            task_id,
            "Timer thread creation failed for delayed task",
            exception);
        throw;
    }

    const std::string scheduled_id = ensure_timers().schedule_once(
        delay_ms, timer_id, state, task_state_id, std::move(dispatch),
        std::move(on_cancelled));
    if (scheduled_id.empty()) {
        // 定时器已停止（并发 shutdown 窗口）：按 TaskCancelled(Shutdown)
        // 满足 future；生命周期事件，不写 failure。注意 on_cancelled 已被
        // 移入调度器，这里直接满足 promise，不能调用移空后的闭包。
        auto exception = std::make_exception_ptr(TaskCancelled(
            TaskCancellationReason::Shutdown,
            "Timer stopped before delayed task execution"));
        if (state) {
            if (state->try_cancel_before_start()) {
                state->notify_cancelled(exception);
            }
        } else {
            bool expected = false;
            if (promise_ready->compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                promise->set_exception(exception);
            }
        }
    } else {
        submission.handle = TimerHandle(timer_id, timers_);
    }

    return submission;
}

// 批量任务提交模板方法实现
template<typename F>
std::vector<std::future<void>> Executor::submit_batch(const std::vector<F>& tasks) {
    auto executor = manager_->get_default_async_executor_snapshot();
    const std::string executor_name = executor ? executor->get_name() : "default";
    if (!executor) {
        record_submit_rejected(
            executor_name,
            "facade_submit_batch",
            "Async executor not initialized. Call initialize() first.");
        throw std::runtime_error("Async executor not initialized. Call initialize() first.");
    }

    std::vector<std::function<void()>> task_wrappers;
    std::vector<std::function<void(std::exception_ptr)>> timeout_handlers;
    std::vector<std::future<void>> futures;
    std::vector<std::shared_ptr<std::promise<void>>> promises;
    std::vector<std::shared_ptr<std::atomic_bool>> promise_ready_flags;

    task_wrappers.reserve(tasks.size());
    timeout_handlers.reserve(tasks.size());
    futures.reserve(tasks.size());
    promises.reserve(tasks.size());
    promise_ready_flags.reserve(tasks.size());

    bool has_empty_task = false;
    for (size_t i = 0; i < tasks.size(); ++i) {
        auto promise = std::make_shared<std::promise<void>>();
        auto promise_ready = std::make_shared<std::atomic_bool>(false);
        futures.push_back(promise->get_future());
        promises.push_back(promise);
        promise_ready_flags.push_back(promise_ready);

        std::string task_id = "facade_submit_batch[" + std::to_string(i) + "]";

        if (detail::is_empty_std_function(tasks[i])) {
            has_empty_task = true;
        }

        // 总量有界 admission：逐任务独立接纳，部分接纳合法（设计稿 D-4）。
        // 被拒任务的 future 立即以 CapacityExhaustedException 就绪，不进入
        // 池提交；接纳任务的 releaser 由 wrapper/timeout 闭包持有。
        auto admission = try_admit_submission(executor_name, task_id, "submit_batch");
        if (!admission.accepted) {
            promise_ready->store(true, std::memory_order_release);
            promise->set_exception(std::make_exception_ptr(CapacityExhaustedException(
                "In-flight submission capacity exhausted")));
            continue;
        }
        auto release_admission = [releaser = std::move(admission.releaser)]() {
            if (releaser) releaser->release();
        };

        task_wrappers.push_back([this, executor_name, task_id, promise, promise_ready, task = tasks[i],
                                 release_admission]() mutable {
            try {
                task();
                release_admission();  // 计数先于 future
                promise->set_value();
                promise_ready->store(true, std::memory_order_release);
            } catch (...) {
                auto exception = std::current_exception();
                release_admission();  // 计数先于 future
                promise->set_exception(exception);
                promise_ready->store(true, std::memory_order_release);
                record_task_exception(
                    executor_name,
                    task_id,
                    "Batch async task threw an exception",
                    exception);
                throw;
            }
        });
        timeout_handlers.push_back(
            [this, executor_name, task_id, promise, promise_ready,
             release_admission](std::exception_ptr exception) {
                bool expected = false;
                if (promise_ready->compare_exchange_strong(expected, true)) {
                    release_admission();  // 计数先于 future
                    promise->set_exception(exception);
                    record_task_timeout(
                        executor_name,
                        task_id,
                        "Batch async task timed out before execution",
                        exception);
                }
            });
    }

    if (has_empty_task) {
        auto exception = std::make_exception_ptr(std::invalid_argument("empty task"));
        for (size_t i = 0; i < promises.size(); ++i) {
            // CAS：容量拒绝的 future 已就绪，不得双重结算。
            bool expected = false;
            if (promise_ready_flags[i]->compare_exchange_strong(expected, true)) {
                promises[i]->set_exception(exception);
            }
        }
        record_submit_rejected(
            executor_name,
            "facade_submit_batch",
            "Async executor rejected batch task submission with empty task",
            exception);
        return futures;
    }

    if (!executor->try_submit_batch_tasks(
            std::move(task_wrappers), std::move(timeout_handlers))) {
        auto exception = std::make_exception_ptr(
            std::runtime_error("Async executor rejected batch task submission"));
        bool marked_any = false;
        for (size_t i = 0; i < promises.size(); ++i) {
            bool expected = false;
            if (promise_ready_flags[i]->compare_exchange_strong(expected, true)) {
                promises[i]->set_exception(exception);
                marked_any = true;
            }
        }
        if (marked_any || tasks.empty()) {
            record_submit_rejected(
                executor_name,
                "facade_submit_batch",
                tasks.empty()
                    ? "Async executor rejected empty batch task submission"
                    : "Async executor rejected batch task submission",
                exception);
        }
    }

    return futures;
}

template<typename F>
std::vector<std::future<void>> Executor::submit_batch_priority(
    int priority,
    const std::vector<F>& tasks) {
    auto executor = manager_->get_default_async_executor_snapshot();
    const std::string executor_name = executor ? executor->get_name() : "default";
    if (!executor) {
        record_submit_rejected(
            executor_name,
            "facade_submit_batch_priority",
            "Async executor not initialized. Call initialize() first.");
        throw std::runtime_error("Async executor not initialized. Call initialize() first.");
    }

    std::vector<std::future<void>> futures;
    futures.reserve(tasks.size());

    for (const auto& task : tasks) {
        futures.push_back(submit_priority(priority, task));
    }

    return futures;
}

template<typename F>
void Executor::submit_batch_no_future(const std::vector<F>& tasks) {
    auto executor = manager_->get_default_async_executor_snapshot();
    const std::string executor_name = executor ? executor->get_name() : "default";
    if (!executor) {
        record_submit_rejected(
            executor_name,
            "facade_submit_batch_no_future",
            "Async executor not initialized. Call initialize() first.");
        throw std::runtime_error("Async executor not initialized. Call initialize() first.");
    }

    std::vector<std::function<void()>> task_wrappers;
    task_wrappers.reserve(tasks.size());
    auto execution_failure_seen = std::make_shared<std::atomic_bool>(false);

    for (size_t i = 0; i < tasks.size(); ++i) {
        std::string task_id =
            "facade_submit_batch_no_future[" + std::to_string(i) + "]";

        // 总量有界 admission：逐任务接纳；被拒任务仅记录事件（无 future）。
        auto admission = try_admit_submission(executor_name, task_id, "submit_batch_no_future");
        if (!admission.accepted) {
            continue;
        }
        auto release_admission = [releaser = std::move(admission.releaser)]() {
            if (releaser) releaser->release();
        };

        task_wrappers.push_back([this, executor_name, task_id, execution_failure_seen, task = tasks[i],
                                 release_admission]() mutable {
            try {
                task();
                release_admission();
            } catch (...) {
                auto exception = std::current_exception();
                release_admission();
                execution_failure_seen->store(true, std::memory_order_release);
                record_task_exception(
                    executor_name,
                    task_id,
                    "Fire-and-forget batch async task threw an exception",
                    exception);
                throw;
            }
        });
    }

    if (!executor->try_submit_batch_tasks(std::move(task_wrappers))) {
        auto exception = std::make_exception_ptr(
            std::runtime_error("Async executor rejected fire-and-forget batch task submission"));
        if (!execution_failure_seen->load(std::memory_order_acquire)) {
            record_submit_rejected(
                executor_name,
                "facade_submit_batch_no_future",
                tasks.empty()
                    ? "Async executor rejected empty fire-and-forget batch task submission"
                    : "Async executor rejected fire-and-forget batch task submission",
                exception);
        }
    }
}

// GPU 任务提交模板方法实现
template<typename KernelFunc>
auto Executor::submit_gpu(const std::string& executor_name,
                         KernelFunc&& kernel,
                         const gpu::GpuTaskConfig& config)
    -> std::future<void> {
    auto executor = manager_->get_gpu_executor_snapshot(executor_name);
    if (!executor) {
        const std::string message =
            "submit_gpu: no GPU executor registered with name " + executor_name;
        record_submit_rejected(executor_name, "facade_submit_gpu", message);
        throw std::runtime_error("GPU executor '" + executor_name + "' not found. Call register_gpu_executor() first.");
    }
    return executor->submit_kernel(std::forward<KernelFunc>(kernel), config);
}

// 智能调度模板方法实现
template<typename KernelFunc>
auto Executor::submit_auto(
    const gpu::TaskCharacteristics& characteristics,
    const std::string& gpu_executor_name,
    KernelFunc&& kernel,
    const gpu::GpuTaskConfig& gpu_config)
    -> std::future<void> {

    TaskOptions routing_options;
    routing_options.name = "facade_submit_auto_legacy";
    routing_options.intent = ExecutionIntent::CpuOrGpu;
    routing_options.preferred_executor = gpu_executor_name;
    record_routing_decision(route_task(
        routing_options,
        true,
        scheduler_.decide(characteristics) == gpu::ExecutorChoice::GPU));

    auto choice = scheduler_.decide(characteristics);

    if (choice == gpu::ExecutorChoice::GPU) {
        return submit_gpu(gpu_executor_name, std::forward<KernelFunc>(kernel), gpu_config);
    } else {
        // CPU fallback: execute kernel with nullptr stream
        return submit([kernel = std::forward<KernelFunc>(kernel)]() mutable {
            kernel(nullptr);
        });
    }
}

template<typename F, typename... Args>
auto Executor::submit_auto(F&& f, Args&&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type> {
    TaskOptions options;
    record_routing_decision(route_task(options, false));
    return submit(std::forward<F>(f), std::forward<Args>(args)...);
}

template<typename Function>
auto Executor::submit_auto(TaskBuilder<Function> task)
    -> std::future<typename std::invoke_result<Function&>::type> {
    const auto& options = task.options();
    const auto decision = route_task(options, false);
    record_routing_decision(decision);
    if (decision.reason == RoutingReason::Rejected) {
        const std::string message = "submit_auto: " + decision.detail;
        record_submit_rejected("default", options.name, message);
        std::promise<typename std::invoke_result<Function&>::type> promise;
        promise.set_exception(std::make_exception_ptr(std::runtime_error(message)));
        return promise.get_future();
    }
    return submit_priority(static_cast<int>(options.priority), std::move(task).function());
}

template<typename CpuFunction, typename GpuFunction>
std::future<void> Executor::submit_auto(CpuGpuTask<CpuFunction, GpuFunction> task) {
    static_assert(requires(CpuFunction& cpu) {
                      { cpu() } -> std::same_as<void>;
                  },
                  "CpuGpuTask CPU callable must be invocable with no arguments and return void");
    static_assert(requires(GpuFunction& gpu) {
                      { gpu(static_cast<void*>(nullptr)) } -> std::same_as<void>;
                  } || requires(GpuFunction& gpu) {
                      { gpu() } -> std::same_as<void>;
                  },
                  "CpuGpuTask GPU callable must be invocable with void* stream or no arguments and return void");

    const auto& options = task.options();
    const auto task_name = options.name.empty() ? "facade_submit_auto" : options.name;
    auto decision = route_task(options, true);
    if (decision.selected_backend == ExecutionBackend::Gpu &&
        options.fallback != FallbackPolicy::RequireRequestedBackend) {
        decision = route_task(
            options, true, scheduler_.decide(task.characteristics()) == gpu::ExecutorChoice::GPU);
    }
    record_routing_decision(decision);

    const auto reject = [this, &task_name, &decision](const std::string& message) {
        record_submit_rejected(decision.selected_executor_name.empty()
                                   ? "gpu" : decision.selected_executor_name,
                               task_name, message);
        std::promise<void> promise;
        promise.set_exception(std::make_exception_ptr(std::runtime_error(message)));
        return promise.get_future();
    };

    if (decision.reason == RoutingReason::Rejected ||
        (decision.selected_backend == ExecutionBackend::DefaultAsync && !decision.fell_back &&
         options.fallback != FallbackPolicy::AllowCpu)) {
        return reject("submit_auto: " + decision.detail);
    }

    if (decision.selected_backend == ExecutionBackend::DefaultAsync) {
        return submit(std::move(task).take_cpu());
    }

    const auto& gpu_name = decision.selected_executor_name;
    try {
        auto gpu_config = task.gpu_config();
        return submit_gpu(gpu_name, std::move(task).take_gpu(), gpu_config);
    } catch (const std::exception& error) {
        if (options.fallback == FallbackPolicy::AllowCpu) {
            RoutingDecision fallback = decision;
            fallback.selected_backend = ExecutionBackend::DefaultAsync;
            fallback.selected_executor_name = "default";
            fallback.reason = RoutingReason::FallbackPolicy;
            fallback.fell_back = true;
            fallback.detail = std::string("GPU submission rejected; falling back to CPU: ") + error.what();
            fallback.timestamp = std::chrono::steady_clock::now();
            record_routing_decision(std::move(fallback));
            return submit(std::move(task).take_cpu());
        }
        return reject(std::string("submit_auto: GPU submission rejected: ") + error.what());
    }
}

} // namespace kairo
