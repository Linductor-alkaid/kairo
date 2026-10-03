#include "kairo/gpu/transfer_optimizer.hpp"
#include <algorithm>

namespace kairo {
namespace gpu {

TransferOptimizer::TransferOptimizer() : config_snapshot_(std::make_shared<const Config>()) {}

TransferOptimizer::TransferOptimizer(const Config& config)
    : config_snapshot_(std::make_shared<const Config>(config)) {}

// --- 传输批量化 ---

void TransferOptimizer::enqueue_transfer(const TransferRequest& request) {
    // CR-063: 一次原子加载取得一致配置快照（下同）。
    const Config config = *std::atomic_load_explicit(&config_snapshot_,
                                                     std::memory_order_acquire);
    if (!config.enable_batching) {
        return;
    }
    std::lock_guard lock(batch_mutex_);
    pending_transfers_.push_back(request);
}

std::vector<std::vector<TransferRequest>> TransferOptimizer::flush_batches() {
    const Config config = *std::atomic_load_explicit(&config_snapshot_,
                                                     std::memory_order_acquire);
    std::lock_guard lock(batch_mutex_);
    if (pending_transfers_.empty()) {
        return {};
    }

    std::vector<std::vector<TransferRequest>> batches;
    std::vector<TransferRequest> current_batch;
    size_t current_batch_size = 0;
    TransferDirection current_dir = pending_transfers_.front().direction;
    int current_stream = pending_transfers_.front().stream_id;

    for (const auto& req : pending_transfers_) {
        bool same_group = (req.direction == current_dir && req.stream_id == current_stream);
        bool batch_full = (current_batch.size() >= config.max_batch_count ||
                          current_batch_size >= config.batch_size_threshold);

        if (!same_group || batch_full) {
            if (!current_batch.empty()) {
                batches.push_back(std::move(current_batch));
                current_batch.clear();
                current_batch_size = 0;
            }
            current_dir = req.direction;
            current_stream = req.stream_id;
        }

        current_batch.push_back(req);
        current_batch_size += req.size;
    }

    if (!current_batch.empty()) {
        batches.push_back(std::move(current_batch));
    }

    pending_transfers_.clear();
    return batches;
}

size_t TransferOptimizer::pending_transfer_count() const {
    std::lock_guard lock(batch_mutex_);
    return pending_transfers_.size();
}

// --- 传输与计算流水线 ---

std::vector<TransferOptimizer::PipelineAction> TransferOptimizer::build_pipeline(
    const std::vector<PipelineStage>& stages,
    int transfer_stream_id,
    int compute_stream_id) {

    std::vector<PipelineAction> actions;
    if (stages.empty()) {
        return actions;
    }

    const Config config = *std::atomic_load_explicit(&config_snapshot_,
                                                     std::memory_order_acquire);
    if (!config.enable_pipeline || stages.size() == 1) {
        // 无流水线：顺序执行每个阶段的传输+计算
        for (const auto& stage : stages) {
            PipelineAction ta;
            ta.type = PipelineAction::TRANSFER;
            ta.stream_id = transfer_stream_id;
            ta.transfer = stage.transfer;
            actions.push_back(std::move(ta));

            PipelineAction sync;
            sync.type = PipelineAction::SYNC_STREAM;
            sync.stream_id = transfer_stream_id;
            actions.push_back(std::move(sync));

            if (stage.compute_func) {
                PipelineAction ca;
                ca.type = PipelineAction::COMPUTE;
                ca.stream_id = compute_stream_id;
                ca.compute_func = stage.compute_func;
                actions.push_back(std::move(ca));

                PipelineAction csync;
                csync.type = PipelineAction::SYNC_STREAM;
                csync.stream_id = compute_stream_id;
                actions.push_back(std::move(csync));
            }
        }
        {
            std::lock_guard lock(stats_mutex_);
            pipeline_stages_ += stages.size();
        }
        return actions;
    }

    // 双 stream 流水线：
    // Stage 0: transfer[0] on transfer_stream
    // Stage 1: sync transfer_stream, compute[0] on compute_stream + transfer[1] on transfer_stream
    // Stage N: sync both, compute[N-1] on compute_stream + transfer[N] on transfer_stream
    // Final:   sync transfer_stream, compute[last] on compute_stream, sync compute_stream

    // 启动第一个传输
    {
        PipelineAction ta;
        ta.type = PipelineAction::TRANSFER;
        ta.stream_id = transfer_stream_id;
        ta.transfer = stages[0].transfer;
        actions.push_back(std::move(ta));
    }

    for (size_t i = 1; i < stages.size(); ++i) {
        // 等待上一个传输完成
        PipelineAction sync_t;
        sync_t.type = PipelineAction::SYNC_STREAM;
        sync_t.stream_id = transfer_stream_id;
        actions.push_back(std::move(sync_t));

        // 并行：计算上一阶段 + 传输当前阶段
        if (stages[i - 1].compute_func) {
            PipelineAction ca;
            ca.type = PipelineAction::COMPUTE;
            ca.stream_id = compute_stream_id;
            ca.compute_func = stages[i - 1].compute_func;
            actions.push_back(std::move(ca));
        }

        PipelineAction ta;
        ta.type = PipelineAction::TRANSFER;
        ta.stream_id = transfer_stream_id;
        ta.transfer = stages[i].transfer;
        actions.push_back(std::move(ta));
    }

    // 等待最后一个传输
    PipelineAction sync_last_t;
    sync_last_t.type = PipelineAction::SYNC_STREAM;
    sync_last_t.stream_id = transfer_stream_id;
    actions.push_back(std::move(sync_last_t));

    // 等待倒数第二个计算（如果有）
    if (stages.size() > 1) {
        PipelineAction sync_c;
        sync_c.type = PipelineAction::SYNC_STREAM;
        sync_c.stream_id = compute_stream_id;
        actions.push_back(std::move(sync_c));
    }

    // 执行最后一个计算
    if (stages.back().compute_func) {
        PipelineAction ca;
        ca.type = PipelineAction::COMPUTE;
        ca.stream_id = compute_stream_id;
        ca.compute_func = stages.back().compute_func;
        actions.push_back(std::move(ca));

        PipelineAction sync_final;
        sync_final.type = PipelineAction::SYNC_STREAM;
        sync_final.stream_id = compute_stream_id;
        actions.push_back(std::move(sync_final));
    }

    {
        std::lock_guard lock(stats_mutex_);
        pipeline_stages_ += stages.size();
    }

    return actions;
}

// --- 小数据优化 ---

bool TransferOptimizer::should_use_pinned(size_t transfer_size) const {
    const Config config = *std::atomic_load_explicit(&config_snapshot_,
                                                     std::memory_order_acquire);
    return config.enable_pinned_optimization &&
           transfer_size <= config.small_transfer_threshold;
}

size_t TransferOptimizer::recommended_pinned_buffer_size() const {
    return std::atomic_load_explicit(&config_snapshot_,
                                     std::memory_order_acquire)
        ->pinned_buffer_size;
}

// --- 统计 ---

void TransferOptimizer::record_transfer(size_t bytes, double latency_us,
                                         bool was_batched, bool was_pinned) {
    std::lock_guard lock(stats_mutex_);
    ++total_transfers_;
    total_bytes_ += static_cast<double>(bytes);
    total_latency_us_ += latency_us;
    if (was_batched) ++batched_transfers_;
    if (was_pinned) ++pinned_transfers_;
}

TransferStats TransferOptimizer::get_stats() const {
    std::lock_guard lock(stats_mutex_);
    TransferStats stats;
    stats.total_transfers = total_transfers_;
    stats.batched_transfers = batched_transfers_;
    stats.pinned_transfers = pinned_transfers_;
    stats.pipeline_stages_executed = pipeline_stages_;
    stats.total_bytes_transferred = total_bytes_;
    if (total_transfers_ > 0) {
        stats.avg_transfer_latency_us = total_latency_us_
            / static_cast<double>(total_transfers_);
        // bandwidth = total_bytes / total_time_seconds / 1e9 (GB/s)
        double total_time_s = total_latency_us_ / 1e6;
        if (total_time_s > 0.0) {
            stats.avg_bandwidth_gbps = total_bytes_ / total_time_s / 1e9;
        }
    }
    return stats;
}

void TransferOptimizer::reset_stats() {
    std::lock_guard lock(stats_mutex_);
    total_transfers_ = 0;
    batched_transfers_ = 0;
    pinned_transfers_ = 0;
    pipeline_stages_ = 0;
    total_bytes_ = 0.0;
    total_latency_us_ = 0.0;
}

TransferOptimizer::Config TransferOptimizer::get_config() const {
    return *std::atomic_load_explicit(&config_snapshot_, std::memory_order_acquire);
}

void TransferOptimizer::update_config(const Config& config) {
    std::atomic_store_explicit(&config_snapshot_,
                               std::make_shared<const Config>(config),
                               std::memory_order_release);
}

} // namespace gpu
} // namespace kairo
