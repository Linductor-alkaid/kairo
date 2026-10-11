#pragma once

#include "kairo/types.hpp"
#include <string>
#include <map>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <vector>

namespace kairo {
namespace monitor {

/**
 * @brief 任务监控器
 *
 * 接收任务生命周期事件（start/complete/timeout），按 task_type 聚合统计。
 * 供 ThreadPool 在 execute_task 前后钩子调用。
 *
 * @note CR-071/CR-163（v0.7.0 M0）：单把全局 mutex 会在提交/执行热路径上
 * 串行化所有线程，且采样率为 0 时仍取锁——反馈测量会混入监控锁争用，
 * 自适应调度会学到噪声。改为按 key 哈希分片：
 *  - task_id → task_type 映射与 in-flight 快照按 task_id 分片；
 *  - 聚合统计按 task_type 分片；
 *  - dropped/evicted 计数与 in-flight 总数为原子量。
 * 热路径快速门：统计采样率为 0 时不触碰 id 映射与统计分片（
 * set_sampling_rate(0) 会清空 id 映射，已开始未结算的样本随之丢弃——
 * 语义即"关闭统计采样"）；in-flight 采样率为 0 或容量为 0 时不触碰
 * in-flight 分片（对应配置变更时清空，保证快速门与分片内容一致）。
 * 两个门都关时 record_task_start/complete/timeout 零取锁。
 */
class TaskMonitor {
public:
    TaskMonitor() = default;
    // 定义在 .cpp：析构前必须取全部分片锁，与最后一个持分片锁的读者
    // （线程池 worker 的 record_task_*）建立 happens-before，否则分片
    // 成员的无锁析构与 shutdown 路径上 worker 的持锁访问构成数据竞争。
    // CR-073: record_task_* 为虚函数且已存在派生（测试注入），经基类指针
    // delete 派生对象是 UB——析构必须为虚。
    virtual ~TaskMonitor();

    TaskMonitor(const TaskMonitor&) = delete;
    TaskMonitor& operator=(const TaskMonitor&) = delete;
    TaskMonitor(TaskMonitor&&) = delete;
    TaskMonitor& operator=(TaskMonitor&&) = delete;

    /**
     * @brief 记录任务开始
     * @param task_id 任务 ID
     * @param task_type 任务类型（默认 "default"），用于聚合统计
     */
    virtual void record_task_start(const std::string& task_id,
                           const std::string& task_type = "default");

    /** Record a successfully accepted task before it reaches a worker. */
    void record_task_queued(const std::string& task_id,
                            const std::string& task_type = "default",
                            const std::string& executor_name = "default");

    /** Record a facade task before it has been accepted by a backend. */
    void record_task_pending(const std::string& task_id,
                             const std::string& task_type,
                             const std::string& executor_name);

    /** Update a previously sampled in-flight task without changing sampling. */
    void record_task_state(const std::string& task_id, TaskLifecycleState state);

    /** Remove a task whose lifecycle reached a terminal state. */
    void record_task_terminal(const std::string& task_id);

    /**
     * @brief 记录任务完成
     * @param task_id 任务 ID
     * @param success 是否成功
     * @param execution_time_ns 执行时间（纳秒）
     */
    virtual void record_task_complete(const std::string& task_id,
                              bool success,
                              int64_t execution_time_ns);

    /**
     * @brief 记录任务超时
     * @param task_id 任务 ID
     */
    void record_task_timeout(const std::string& task_id);

    /** Return a bounded value copy of sampled queued/running tasks. */
    std::vector<TaskLifecycleSnapshot> get_in_flight_tasks() const;
    InFlightTaskDiagnostics get_in_flight_diagnostics() const;
    std::map<TaskLifecycleState, size_t> get_in_flight_state_counts() const;
    size_t get_in_flight_count() const;
    size_t get_in_flight_dropped_count() const;
    /// CR-075: set_in_flight_capacity 缩容驱逐数（独立于运行期准入丢弃，
    /// 不参与 incomplete 推导）。
    size_t get_in_flight_evicted_count() const;
    std::chrono::nanoseconds get_oldest_in_flight_age() const;
    bool in_flight_diagnostics_incomplete() const;

    /** Capacity 0 disables in-flight retention without disabling statistics. */
    void set_in_flight_capacity(size_t capacity);
    size_t get_in_flight_capacity() const;
    /** Sampling is independent from aggregate TaskStatistics sampling. */
    void set_in_flight_sampling_rate(double rate);
    double get_in_flight_sampling_rate() const;

    /**
     * @brief 按 task_type 获取聚合统计
     * @param task_type 任务类型
     * @return 统计信息；若不存在则返回全 0
     */
    TaskStatistics get_statistics(const std::string& task_type) const;

    /**
     * @brief 获取全部 task_type 的聚合统计
     */
    std::map<std::string, TaskStatistics> get_all_statistics() const;

    void set_enabled(bool enabled);
    bool is_enabled() const;

    /**
     * @brief 设置采样率（0.0-1.0）
     * @param rate 采样率，0.01 表示 1% 采样
     */
    void set_sampling_rate(double rate);
    double get_sampling_rate() const;

private:
    bool should_sample() const;
    bool should_sample_in_flight() const;
    /// CR-071 热路径快速门：in-flight 分片当前是否可能持有条目。
    /// 关闭（采样率 0 或容量 0）时 setter 已清空分片，后续更新可无锁跳过。
    bool in_flight_active() const;

    static constexpr size_t kIdShardCount = 16;
    static constexpr size_t kTypeShardCount = 8;
    static size_t shard_of(const std::string& key, size_t shard_count);

    /// task_id → task_type，用于 complete/timeout 时查找（按 task_id 分片）
    struct IdShard {
        mutable std::mutex mutex;
        std::unordered_map<std::string, std::string> id_to_type;
    };
    /// 已采样排队/运行中任务快照（按 task_id 分片）
    struct InFlightShard {
        mutable std::mutex mutex;
        std::unordered_map<std::string, TaskLifecycleSnapshot> tasks;
    };
    /// 按 task_type 聚合的统计（内部存储，与 TaskStatistics 一致；按
    /// task_type 分片）
    struct Stats {
        int64_t total_count = 0;
        int64_t success_count = 0;
        int64_t fail_count = 0;
        int64_t timeout_count = 0;
        int64_t total_execution_time_ns = 0;
        int64_t max_execution_time_ns = 0;
        int64_t min_execution_time_ns = 0;  /// 0 表示尚未有样本
    };
    struct TypeShard {
        mutable std::mutex mutex;
        std::map<std::string, Stats> stats;
    };

    std::atomic<bool> enabled_{true};
    std::atomic<uint32_t> sampling_rate_{100};  // 百分比，100=100%，1=1%
    mutable std::atomic<uint64_t> sample_counter_{0};
    std::atomic<size_t> in_flight_capacity_{128};
    std::atomic<uint32_t> in_flight_sampling_rate_{100};
    mutable std::atomic<uint64_t> in_flight_sample_counter_{0};
    /// in-flight 总条数（原子维护，容量检查与 count 查询不再聚合各分片）
    std::atomic<size_t> in_flight_count_{0};
    // CR-075: 缩容驱逐计数独立于运行期准入丢弃，二者均为跨分片原子量。
    std::atomic<size_t> in_flight_dropped_count_{0};
    std::atomic<size_t> in_flight_evicted_count_{0};

    IdShard id_shards_[kIdShardCount];
    InFlightShard in_flight_shards_[kIdShardCount];
    TypeShard type_shards_[kTypeShardCount];
};

}  // namespace monitor
}  // namespace kairo
