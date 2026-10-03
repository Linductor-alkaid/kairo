#pragma once

#include <cstddef>
#include <memory>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <deque>
#include <functional>
#include <atomic>
#include <chrono>

namespace kairo {
namespace gpu {

/**
 * @brief 内存传输方向
 */
enum class TransferDirection {
    HOST_TO_DEVICE,
    DEVICE_TO_HOST,
    DEVICE_TO_DEVICE
};

/**
 * @brief 单次传输请求
 */
struct TransferRequest {
    void* dst = nullptr;
    const void* src = nullptr;
    size_t size = 0;
    TransferDirection direction = TransferDirection::HOST_TO_DEVICE;
    int stream_id = 0;
    bool async = true;
};

/**
 * @brief 流水线阶段
 */
struct PipelineStage {
    TransferRequest transfer;                  ///< 传输操作
    std::function<void(void*)> compute_func;   ///< 计算操作（接收 stream 句柄）
    int compute_stream_id = 0;                 ///< 计算使用的 stream
};

/**
 * @brief 内存传输统计
 */
struct TransferStats {
    size_t total_transfers = 0;
    size_t batched_transfers = 0;
    size_t pinned_transfers = 0;
    size_t pipeline_stages_executed = 0;
    double total_bytes_transferred = 0.0;
    double avg_bandwidth_gbps = 0.0;          ///< 平均带宽 (GB/s)
    double avg_transfer_latency_us = 0.0;     ///< 平均传输延迟 (微秒)
};

/**
 * @brief 内存传输优化器
 *
 * 提供三项优化：
 * 1. 传输批量化 — 合并同方向、同 stream 的小传输为一次大传输
 * 2. 传输与计算流水线 — 使用双 stream 交替执行传输和计算，隐藏传输延迟
 * 3. 小数据传输优化 — 对小数据使用 pinned memory 减少传输开销
 */
class TransferOptimizer {
public:
    struct Config {
        size_t batch_size_threshold = 64 * 1024;   ///< 批量合并的最小总字节数
        size_t max_batch_count = 32;               ///< 单批最大传输数
        size_t small_transfer_threshold = 4096;    ///< 小数据阈值（字节），低于此值使用 pinned memory
        size_t pinned_buffer_size = 1024 * 1024;   ///< Pinned memory 缓冲区大小
        bool enable_batching = true;
        bool enable_pipeline = true;
        bool enable_pinned_optimization = true;
    };

    TransferOptimizer();
    explicit TransferOptimizer(const Config& config);

    // --- 传输批量化 ---

    /// 将传输请求加入批量队列
    void enqueue_transfer(const TransferRequest& request);

    /// 取出可合并的一批传输（同方向、同 stream），返回合并后的批次
    std::vector<std::vector<TransferRequest>> flush_batches();

    /// 当前排队的传输数
    size_t pending_transfer_count() const;

    // --- 传输与计算流水线 ---

    /// 构建流水线：交替在两个 stream 上执行传输和计算
    /// 返回按执行顺序排列的 (stream_id, 操作) 序列
    struct PipelineAction {
        enum Type { TRANSFER, COMPUTE, SYNC_STREAM };
        Type type;
        int stream_id = 0;
        TransferRequest transfer;                 ///< type==TRANSFER 时有效
        std::function<void(void*)> compute_func;  ///< type==COMPUTE 时有效
    };

    std::vector<PipelineAction> build_pipeline(
        const std::vector<PipelineStage>& stages,
        int transfer_stream_id,
        int compute_stream_id);

    // --- 小数据优化 ---

    /// 判断是否应使用 pinned memory 优化
    bool should_use_pinned(size_t transfer_size) const;

    /// 获取推荐的 pinned buffer 大小
    size_t recommended_pinned_buffer_size() const;

    // --- 统计 ---

    /// 记录一次传输完成
    void record_transfer(size_t bytes, double latency_us, bool was_batched, bool was_pinned);

    TransferStats get_stats() const;
    void reset_stats();

    Config get_config() const;
    void update_config(const Config& config);

private:
    // CR-063: config 以不可变快照发布（原子指针替换）。此前 update_config
    // 持 batch_mutex_ 写、多个读路径无锁读同一 Config 对象，构成数据竞争
    // （TSAN 实证 9 条）。读侧一次原子加载取得一致快照，零锁零竞争；
    // 写侧每次 update 生成新快照对象。
    std::shared_ptr<const Config> config_snapshot_;

    mutable std::mutex batch_mutex_;
    std::deque<TransferRequest> pending_transfers_;

    mutable std::mutex stats_mutex_;
    size_t total_transfers_ = 0;
    size_t batched_transfers_ = 0;
    size_t pinned_transfers_ = 0;
    size_t pipeline_stages_ = 0;
    double total_bytes_ = 0.0;
    double total_latency_us_ = 0.0;
};

} // namespace gpu
} // namespace kairo
