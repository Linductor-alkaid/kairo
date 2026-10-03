#include <cassert>
#include <iostream>
#include <cmath>
#include <kairo/gpu/gpu_scheduler.hpp>

using namespace kairo;
using namespace kairo::gpu;

// Test helper macro
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

// Test 1: Default configuration
bool test_default_config() {
    GpuScheduler scheduler;
    auto config = scheduler.get_config();

    TEST_ASSERT(config.data_size_threshold == 1024 * 1024, "Default data size threshold should be 1MB");
    TEST_ASSERT(config.compute_intensity_threshold == 2.0f, "Default compute intensity threshold should be 2.0");

    std::cout << "PASSED: test_default_config" << std::endl;
    return true;
}

// Test 2: Small data -> CPU
bool test_small_data_cpu() {
    GpuScheduler scheduler;

    TaskCharacteristics chars;
    chars.data_size_bytes = 512 * 1024;  // 512KB < 1MB threshold
    chars.compute_intensity = 3.0f;       // High compute, but small data

    auto choice = scheduler.decide(chars);
    TEST_ASSERT(choice == ExecutorChoice::CPU, "Small data should choose CPU");

    std::cout << "PASSED: test_small_data_cpu" << std::endl;
    return true;
}

// Test 3: Large data + high compute -> GPU
bool test_large_data_high_compute_gpu() {
    GpuScheduler scheduler;

    TaskCharacteristics chars;
    chars.data_size_bytes = 10 * 1024 * 1024;  // 10MB > 1MB threshold
    chars.compute_intensity = 5.0f;             // 5.0 > 2.0 threshold

    auto choice = scheduler.decide(chars);
    TEST_ASSERT(choice == ExecutorChoice::GPU, "Large data + high compute should choose GPU");

    std::cout << "PASSED: test_large_data_high_compute_gpu" << std::endl;
    return true;
}

// Test 4: Large data + low compute -> CPU
bool test_large_data_low_compute_cpu() {
    GpuScheduler scheduler;

    TaskCharacteristics chars;
    chars.data_size_bytes = 10 * 1024 * 1024;  // 10MB > threshold
    chars.compute_intensity = 1.0f;             // 1.0 < 2.0 threshold

    auto choice = scheduler.decide(chars);
    TEST_ASSERT(choice == ExecutorChoice::CPU, "Large data but low compute should choose CPU");

    std::cout << "PASSED: test_large_data_low_compute_cpu" << std::endl;
    return true;
}

// Test 5: User preference hint
bool test_prefer_gpu_hint() {
    GpuScheduler scheduler;

    TaskCharacteristics chars;
    chars.data_size_bytes = 100;  // Very small data
    chars.compute_intensity = 0.5f;  // Low compute
    chars.prefer_gpu = true;  // But user prefers GPU

    auto choice = scheduler.decide(chars);
    TEST_ASSERT(choice == ExecutorChoice::GPU, "prefer_gpu hint should override heuristics");

    std::cout << "PASSED: test_prefer_gpu_hint" << std::endl;
    return true;
}

// Test 6: Configuration update
bool test_config_update() {
    GpuScheduler scheduler;

    GpuScheduler::Config new_config;
    new_config.data_size_threshold = 512 * 1024;  // 512KB
    new_config.compute_intensity_threshold = 1.5f;

    scheduler.update_config(new_config);
    auto config = scheduler.get_config();

    TEST_ASSERT(config.data_size_threshold == 512 * 1024, "Config should be updated");
    TEST_ASSERT(config.compute_intensity_threshold == 1.5f, "Config should be updated");

    // Test with new thresholds
    TaskCharacteristics chars;
    chars.data_size_bytes = 600 * 1024;  // 600KB > 512KB
    chars.compute_intensity = 2.0f;       // 2.0 > 1.5

    auto choice = scheduler.decide(chars);
    TEST_ASSERT(choice == ExecutorChoice::GPU, "Should choose GPU with updated thresholds");

    std::cout << "PASSED: test_config_update" << std::endl;
    return true;
}

// Test 7: Record performance and history count
bool test_record_performance() {
    GpuScheduler scheduler;

    TaskCharacteristics chars;
    chars.data_size_bytes = 2 * 1024 * 1024;
    chars.compute_intensity = 3.0f;

    scheduler.record_performance(chars, ExecutorChoice::GPU, 10.0);
    scheduler.record_performance(chars, ExecutorChoice::CPU, 50.0);

    TEST_ASSERT(scheduler.history_count() == 2, "Should have 2 records");

    scheduler.clear_history();
    TEST_ASSERT(scheduler.history_count() == 0, "Should have 0 records after clear");

    std::cout << "PASSED: test_record_performance" << std::endl;
    return true;
}

// Test 8: Predict time with sufficient history
bool test_predict_time() {
    GpuScheduler scheduler;

    TaskCharacteristics chars;
    chars.data_size_bytes = 2 * 1024 * 1024;
    chars.compute_intensity = 3.0f;

    // Record GPU times (similar tasks)
    scheduler.record_performance(chars, ExecutorChoice::GPU, 10.0);
    scheduler.record_performance(chars, ExecutorChoice::GPU, 14.0);

    // Record CPU times
    scheduler.record_performance(chars, ExecutorChoice::CPU, 50.0);
    scheduler.record_performance(chars, ExecutorChoice::CPU, 54.0);

    double gpu_pred = scheduler.predict_time(chars, ExecutorChoice::GPU);
    double cpu_pred = scheduler.predict_time(chars, ExecutorChoice::CPU);

    TEST_ASSERT(gpu_pred > 0.0, "GPU prediction should be available");
    TEST_ASSERT(cpu_pred > 0.0, "CPU prediction should be available");
    TEST_ASSERT(std::abs(gpu_pred - 12.0) < 0.01, "GPU prediction should be ~12.0ms");
    TEST_ASSERT(std::abs(cpu_pred - 52.0) < 0.01, "CPU prediction should be ~52.0ms");

    std::cout << "PASSED: test_predict_time" << std::endl;
    return true;
}

// Test 9: Predict returns -1 with insufficient history
bool test_predict_insufficient_data() {
    GpuScheduler scheduler;

    TaskCharacteristics chars;
    chars.data_size_bytes = 2 * 1024 * 1024;
    chars.compute_intensity = 3.0f;

    // Only 1 record - need at least 2
    scheduler.record_performance(chars, ExecutorChoice::GPU, 10.0);

    double pred = scheduler.predict_time(chars, ExecutorChoice::GPU);
    TEST_ASSERT(pred < 0.0, "Should return -1 with insufficient data");

    std::cout << "PASSED: test_predict_insufficient_data" << std::endl;
    return true;
}

// Test 10: Adaptive scheduling overrides heuristic
bool test_adaptive_scheduling() {
    GpuScheduler::Config config;
    config.enable_adaptive = true;
    GpuScheduler scheduler(config);

    // Task that heuristic would send to CPU (small data, low compute)
    TaskCharacteristics chars;
    chars.data_size_bytes = 512 * 1024;  // Below threshold
    chars.compute_intensity = 1.5f;      // Below threshold

    // Without history, heuristic says CPU
    auto choice = scheduler.decide(chars);
    TEST_ASSERT(choice == ExecutorChoice::CPU, "Without history, should use heuristic (CPU)");

    // Record history showing GPU is faster for similar tasks
    scheduler.record_performance(chars, ExecutorChoice::GPU, 5.0);
    scheduler.record_performance(chars, ExecutorChoice::GPU, 6.0);
    scheduler.record_performance(chars, ExecutorChoice::CPU, 20.0);
    scheduler.record_performance(chars, ExecutorChoice::CPU, 22.0);

    // Now adaptive should choose GPU
    choice = scheduler.decide(chars);
    TEST_ASSERT(choice == ExecutorChoice::GPU, "Adaptive should override heuristic to GPU");

    std::cout << "PASSED: test_adaptive_scheduling" << std::endl;
    return true;
}

// Test 11: Adaptive disabled falls back to heuristic
bool test_adaptive_disabled() {
    GpuScheduler scheduler;  // adaptive disabled by default

    TaskCharacteristics chars;
    chars.data_size_bytes = 512 * 1024;
    chars.compute_intensity = 1.5f;

    // Record history showing GPU is faster
    scheduler.record_performance(chars, ExecutorChoice::GPU, 5.0);
    scheduler.record_performance(chars, ExecutorChoice::GPU, 6.0);
    scheduler.record_performance(chars, ExecutorChoice::CPU, 20.0);
    scheduler.record_performance(chars, ExecutorChoice::CPU, 22.0);

    // Adaptive disabled, should still use heuristic -> CPU
    auto choice = scheduler.decide(chars);
    TEST_ASSERT(choice == ExecutorChoice::CPU, "With adaptive disabled, should use heuristic");

    std::cout << "PASSED: test_adaptive_disabled" << std::endl;
    return true;
}

// Test 12: History size limit
bool test_history_size_limit() {
    GpuScheduler::Config config;
    config.history_size = 5;
    GpuScheduler scheduler(config);

    TaskCharacteristics chars;
    chars.data_size_bytes = 1024;
    chars.compute_intensity = 1.0f;

    for (int i = 0; i < 10; ++i) {
        scheduler.record_performance(chars, ExecutorChoice::CPU, 1.0);
    }

    TEST_ASSERT(scheduler.history_count() == 5, "History should be capped at 5");

    std::cout << "PASSED: test_history_size_limit" << std::endl;
    return true;
}

int main() {
    std::cout << "Running GPU Scheduler tests..." << std::endl;

    bool all_passed = true;
    all_passed &= test_default_config();
    all_passed &= test_small_data_cpu();
    all_passed &= test_large_data_high_compute_gpu();
    all_passed &= test_large_data_low_compute_cpu();
    all_passed &= test_prefer_gpu_hint();
    all_passed &= test_config_update();
    all_passed &= test_record_performance();
    all_passed &= test_predict_time();
    all_passed &= test_predict_insufficient_data();
    all_passed &= test_adaptive_scheduling();
    all_passed &= test_adaptive_disabled();
    all_passed &= test_history_size_limit();

    if (all_passed) {
        std::cout << "\nAll tests PASSED!" << std::endl;
        return 0;
    } else {
        std::cerr << "\nSome tests FAILED!" << std::endl;
        return 1;
    }
}
