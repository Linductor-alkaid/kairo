#include <cassert>
#include <iostream>
#include <cmath>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <kairo/gpu/kernel_launch_optimizer.hpp>
#include <kairo/gpu/transfer_optimizer.hpp>
#include <kairo/gpu/task_scheduler_optimizer.hpp>

using namespace kairo::gpu;

#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

// ============================================================
// KernelLaunchOptimizer Tests
// ============================================================

bool test_kernel_param_cache_store_lookup() {
    KernelLaunchOptimizer opt;

    KernelParamCacheEntry entry;
    entry.grid_size[0] = 128;
    entry.block_size[0] = 256;
    entry.shared_memory_bytes = 4096;

    opt.store_params("matmul_kernel", entry);
    TEST_ASSERT(opt.cache_size() == 1, "Cache should have 1 entry");

    KernelParamCacheEntry out;
    bool found = opt.lookup_params("matmul_kernel", out);
    TEST_ASSERT(found, "Should find cached params");
    TEST_ASSERT(out.grid_size[0] == 128, "Grid size should match");
    TEST_ASSERT(out.block_size[0] == 256, "Block size should match");
    TEST_ASSERT(out.shared_memory_bytes == 4096, "Shared memory should match");
    TEST_ASSERT(out.hit_count == 1, "Hit count should be 1");

    std::cout << "PASSED: test_kernel_param_cache_store_lookup" << std::endl;
    return true;
}

bool test_kernel_param_cache_miss() {
    KernelLaunchOptimizer opt;

    KernelParamCacheEntry out;
    bool found = opt.lookup_params("nonexistent", out);
    TEST_ASSERT(!found, "Should not find nonexistent kernel");

    auto stats = opt.get_stats();
    TEST_ASSERT(stats.cache_misses == 1, "Should have 1 cache miss");

    std::cout << "PASSED: test_kernel_param_cache_miss" << std::endl;
    return true;
}

bool test_kernel_cache_lru_eviction() {
    KernelLaunchOptimizer::Config config;
    config.max_cache_entries = 3;
    KernelLaunchOptimizer opt(config);

    KernelParamCacheEntry entry;
    entry.block_size[0] = 64;

    opt.store_params("k1", entry);
    opt.store_params("k2", entry);
    opt.store_params("k3", entry);
    TEST_ASSERT(opt.cache_size() == 3, "Cache should have 3 entries");

    // Access k1 to make it recently used
    KernelParamCacheEntry out;
    opt.lookup_params("k1", out);

    // Adding k4 should evict k2 (least recently used)
    opt.store_params("k4", entry);
    TEST_ASSERT(opt.cache_size() == 3, "Cache should still have 3 entries");

    bool found_k1 = opt.lookup_params("k1", out);
    bool found_k2 = opt.lookup_params("k2", out);
    bool found_k4 = opt.lookup_params("k4", out);
    TEST_ASSERT(found_k1, "k1 should still be cached (recently accessed)");
    TEST_ASSERT(!found_k2, "k2 should be evicted (LRU)");
    TEST_ASSERT(found_k4, "k4 should be cached");

    std::cout << "PASSED: test_kernel_cache_lru_eviction" << std::endl;
    return true;
}

bool test_kernel_batch_enqueue_flush() {
    KernelLaunchOptimizer::Config config;
    config.batch_threshold = 3;
    KernelLaunchOptimizer opt(config);

    for (int i = 0; i < 2; ++i) {
        BatchedKernelRequest req;
        req.priority = 1;
        opt.enqueue(std::move(req));
    }
    TEST_ASSERT(opt.pending_count() == 2, "Should have 2 pending");

    auto batch = opt.flush_if_ready();
    TEST_ASSERT(batch.empty(), "Should not flush below threshold");

    BatchedKernelRequest req;
    req.priority = 1;
    opt.enqueue(std::move(req));

    batch = opt.flush_if_ready();
    TEST_ASSERT(batch.size() == 3, "Should flush 3 kernels at threshold");
    TEST_ASSERT(opt.pending_count() == 0, "Queue should be empty after flush");

    auto stats = opt.get_stats();
    TEST_ASSERT(stats.batched_launches == 1, "Should have 1 batched launch");

    std::cout << "PASSED: test_kernel_batch_enqueue_flush" << std::endl;
    return true;
}

bool test_kernel_launch_latency_tracking() {
    KernelLaunchOptimizer opt;

    opt.record_launch_latency(10.0);
    opt.record_launch_latency(20.0);
    opt.record_launch_latency(30.0);

    auto stats = opt.get_stats();
    TEST_ASSERT(stats.total_launches == 3, "Should have 3 launches");
    TEST_ASSERT(std::abs(stats.avg_launch_latency_us - 20.0) < 0.01, "Avg should be 20.0us");
    TEST_ASSERT(std::abs(stats.min_launch_latency_us - 10.0) < 0.01, "Min should be 10.0us");
    TEST_ASSERT(std::abs(stats.max_launch_latency_us - 30.0) < 0.01, "Max should be 30.0us");

    opt.reset_stats();
    stats = opt.get_stats();
    TEST_ASSERT(stats.total_launches == 0, "Should be reset");

    std::cout << "PASSED: test_kernel_launch_latency_tracking" << std::endl;
    return true;
}

// ============================================================
// TransferOptimizer Tests
// ============================================================

bool test_transfer_batch_grouping() {
    TransferOptimizer opt;

    char buf[64] = {};
    // 3 H2D transfers on stream 0
    for (int i = 0; i < 3; ++i) {
        TransferRequest req;
        req.dst = buf;
        req.src = buf;
        req.size = 1024;
        req.direction = TransferDirection::HOST_TO_DEVICE;
        req.stream_id = 0;
        opt.enqueue_transfer(req);
    }
    // 2 D2H transfers on stream 0
    for (int i = 0; i < 2; ++i) {
        TransferRequest req;
        req.dst = buf;
        req.src = buf;
        req.size = 512;
        req.direction = TransferDirection::DEVICE_TO_HOST;
        req.stream_id = 0;
        opt.enqueue_transfer(req);
    }

    TEST_ASSERT(opt.pending_transfer_count() == 5, "Should have 5 pending");

    auto batches = opt.flush_batches();
    TEST_ASSERT(batches.size() == 2, "Should have 2 batches (H2D + D2H)");
    TEST_ASSERT(batches[0].size() == 3, "First batch should have 3 H2D");
    TEST_ASSERT(batches[1].size() == 2, "Second batch should have 2 D2H");
    TEST_ASSERT(opt.pending_transfer_count() == 0, "Queue should be empty");

    std::cout << "PASSED: test_transfer_batch_grouping" << std::endl;
    return true;
}

bool test_transfer_pipeline_build() {
    TransferOptimizer opt;

    std::vector<PipelineStage> stages;
    for (int i = 0; i < 3; ++i) {
        PipelineStage stage;
        stage.transfer.size = 1024;
        stage.transfer.direction = TransferDirection::HOST_TO_DEVICE;
        stage.compute_func = [](void*) {};
        stages.push_back(std::move(stage));
    }

    auto actions = opt.build_pipeline(stages, /*transfer_stream=*/1, /*compute_stream=*/2);
    TEST_ASSERT(!actions.empty(), "Pipeline should produce actions");

    // Verify pipeline has interleaved transfer and compute
    size_t transfer_count = 0, compute_count = 0, sync_count = 0;
    for (const auto& a : actions) {
        if (a.type == TransferOptimizer::PipelineAction::TRANSFER) ++transfer_count;
        else if (a.type == TransferOptimizer::PipelineAction::COMPUTE) ++compute_count;
        else if (a.type == TransferOptimizer::PipelineAction::SYNC_STREAM) ++sync_count;
    }
    TEST_ASSERT(transfer_count == 3, "Should have 3 transfers");
    TEST_ASSERT(compute_count == 3, "Should have 3 computes");
    TEST_ASSERT(sync_count > 0, "Should have sync points");

    std::cout << "PASSED: test_transfer_pipeline_build" << std::endl;
    return true;
}

bool test_transfer_pinned_recommendation() {
    TransferOptimizer::Config config;
    config.small_transfer_threshold = 4096;
    TransferOptimizer opt(config);

    TEST_ASSERT(opt.should_use_pinned(1024), "1KB should use pinned");
    TEST_ASSERT(opt.should_use_pinned(4096), "4KB should use pinned");
    TEST_ASSERT(!opt.should_use_pinned(8192), "8KB should not use pinned");

    std::cout << "PASSED: test_transfer_pinned_recommendation" << std::endl;
    return true;
}

bool test_transfer_stats() {
    TransferOptimizer opt;

    opt.record_transfer(1024, 5.0, true, false);
    opt.record_transfer(2048, 8.0, false, true);
    opt.record_transfer(4096, 10.0, true, true);

    auto stats = opt.get_stats();
    TEST_ASSERT(stats.total_transfers == 3, "Should have 3 transfers");
    TEST_ASSERT(stats.batched_transfers == 2, "Should have 2 batched");
    TEST_ASSERT(stats.pinned_transfers == 2, "Should have 2 pinned");
    TEST_ASSERT(stats.total_bytes_transferred == 7168.0, "Total bytes should be 7168");
    TEST_ASSERT(stats.avg_transfer_latency_us > 0.0, "Avg latency should be > 0");

    opt.reset_stats();
    stats = opt.get_stats();
    TEST_ASSERT(stats.total_transfers == 0, "Should be reset");

    std::cout << "PASSED: test_transfer_stats" << std::endl;
    return true;
}

// ============================================================
// TaskSchedulerOptimizer Tests
// ============================================================

bool test_task_add_and_ready() {
    TaskSchedulerOptimizer opt;

    GpuTaskNode t1;
    t1.task_id = "t1";
    t1.priority = 2;
    TEST_ASSERT(opt.add_task(t1), "Should add t1");

    GpuTaskNode t2;
    t2.task_id = "t2";
    t2.priority = 1;
    t2.dependencies = {"t1"};
    TEST_ASSERT(opt.add_task(t2), "Should add t2");

    TEST_ASSERT(opt.pending_count() == 2, "Should have 2 pending");

    auto ready = opt.get_ready_tasks();
    TEST_ASSERT(ready.size() == 1, "Only t1 should be ready");
    TEST_ASSERT(ready[0].task_id == "t1", "Ready task should be t1");

    std::cout << "PASSED: test_task_add_and_ready" << std::endl;
    return true;
}

bool test_task_dependency_resolution() {
    TaskSchedulerOptimizer opt;

    GpuTaskNode t1;
    t1.task_id = "t1";
    opt.add_task(t1);

    GpuTaskNode t2;
    t2.task_id = "t2";
    t2.dependencies = {"t1"};
    opt.add_task(t2);

    GpuTaskNode t3;
    t3.task_id = "t3";
    t3.dependencies = {"t1"};
    opt.add_task(t3);

    // Only t1 ready initially
    auto ready = opt.get_ready_tasks();
    TEST_ASSERT(ready.size() == 1, "Only t1 ready initially");

    // Complete t1 -> t2 and t3 become ready
    opt.mark_completed("t1");
    ready = opt.get_ready_tasks();
    TEST_ASSERT(ready.size() == 2, "t2 and t3 should be ready after t1 completes");

    std::cout << "PASSED: test_task_dependency_resolution" << std::endl;
    return true;
}

bool test_task_priority_inheritance() {
    TaskSchedulerOptimizer::Config config;
    config.enable_priority_inheritance = true;
    TaskSchedulerOptimizer opt(config);

    GpuTaskNode t1;
    t1.task_id = "t1";
    t1.priority = 0;  // LOW
    opt.add_task(t1);

    GpuTaskNode t2;
    t2.task_id = "t2";
    t2.priority = 3;  // CRITICAL, depends on t1
    t2.dependencies = {"t1"};
    opt.add_task(t2);

    // t1 should be promoted to priority 3
    auto ready = opt.get_ready_tasks();
    TEST_ASSERT(ready.size() == 1, "Only t1 ready");
    TEST_ASSERT(ready[0].priority == 3, "t1 should inherit priority 3 from t2");

    auto stats = opt.get_stats();
    TEST_ASSERT(stats.priority_promotions > 0, "Should have priority promotions");

    std::cout << "PASSED: test_task_priority_inheritance" << std::endl;
    return true;
}

bool test_task_cycle_detection() {
    TaskSchedulerOptimizer opt;

    GpuTaskNode t1;
    t1.task_id = "t1";
    t1.dependencies = {"t3"};
    opt.add_task(t1);

    GpuTaskNode t2;
    t2.task_id = "t2";
    t2.dependencies = {"t1"};
    opt.add_task(t2);

    GpuTaskNode t3;
    t3.task_id = "t3";
    t3.dependencies = {"t2"};
    opt.add_task(t3);

    TEST_ASSERT(opt.has_cycle(), "Should detect cycle t1->t3->t2->t1");

    std::cout << "PASSED: test_task_cycle_detection" << std::endl;
    return true;
}

bool test_task_no_cycle() {
    TaskSchedulerOptimizer opt;

    GpuTaskNode t1;
    t1.task_id = "t1";
    opt.add_task(t1);

    GpuTaskNode t2;
    t2.task_id = "t2";
    t2.dependencies = {"t1"};
    opt.add_task(t2);

    GpuTaskNode t3;
    t3.task_id = "t3";
    t3.dependencies = {"t1", "t2"};
    opt.add_task(t3);

    TEST_ASSERT(!opt.has_cycle(), "Should not detect cycle in DAG");

    std::cout << "PASSED: test_task_no_cycle" << std::endl;
    return true;
}

bool test_task_load_balancing() {
    TaskSchedulerOptimizer opt;

    DeviceLoad load0;
    load0.device_id = 0;
    load0.pending_tasks = 10;
    load0.estimated_total_cost = 1000;
    opt.update_device_load(load0);

    DeviceLoad load1;
    load1.device_id = 1;
    load1.pending_tasks = 2;
    load1.estimated_total_cost = 200;
    opt.update_device_load(load1);

    GpuTaskNode task;
    task.task_id = "heavy";
    task.estimated_cost = 100;

    int best = opt.select_best_device(task);
    TEST_ASSERT(best == 1, "Should select device 1 (lower load)");

    auto loads = opt.get_device_loads();
    TEST_ASSERT(loads.size() == 2, "Should have 2 device loads");

    std::cout << "PASSED: test_task_load_balancing" << std::endl;
    return true;
}

bool test_task_duplicate_rejection() {
    TaskSchedulerOptimizer opt;

    GpuTaskNode t1;
    t1.task_id = "t1";
    TEST_ASSERT(opt.add_task(t1), "First add should succeed");
    TEST_ASSERT(!opt.add_task(t1), "Duplicate add should fail");

    std::cout << "PASSED: test_task_duplicate_rejection" << std::endl;
    return true;
}

// ============================================================
// Main
// ============================================================

int main() {
    std::cout << "Running GPU Performance Optimizer tests..." << std::endl;

    bool all_passed = true;

    // KernelLaunchOptimizer
    all_passed &= test_kernel_param_cache_store_lookup();
    all_passed &= test_kernel_param_cache_miss();
    all_passed &= test_kernel_cache_lru_eviction();
    all_passed &= test_kernel_batch_enqueue_flush();
    all_passed &= test_kernel_launch_latency_tracking();

    // TransferOptimizer
    all_passed &= test_transfer_batch_grouping();
    all_passed &= test_transfer_pipeline_build();
    all_passed &= test_transfer_pinned_recommendation();
    all_passed &= test_transfer_stats();

    // TaskSchedulerOptimizer
    all_passed &= test_task_add_and_ready();
    all_passed &= test_task_dependency_resolution();
    all_passed &= test_task_priority_inheritance();
    all_passed &= test_task_cycle_detection();
    all_passed &= test_task_no_cycle();
    all_passed &= test_task_load_balancing();
    all_passed &= test_task_duplicate_rejection();

    if (all_passed) {
        std::cout << "\nAll 16 tests PASSED!" << std::endl;
        return 0;
    } else {
        std::cerr << "\nSome tests FAILED!" << std::endl;
        return 1;
    }
}
