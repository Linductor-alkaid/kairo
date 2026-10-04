// CR-063: TransferOptimizer and KernelLaunchOptimizer hold config_ under
// different locks than the hot-path readers use (or no lock at all):
//   TransferOptimizer: update_config/get_config lock batch_mutex_
//     (.cpp:238-246) but enqueue_transfer reads config_.enable_batching
//     before locking (.cpp:14), build_pipeline reads config_.enable_pipeline
//     unlocked (.cpp:77), should_use_pinned/recommended_pinned_buffer_size
//     read unlocked (.cpp:187-193).
//   KernelLaunchOptimizer: update_config/get_config lock cache_mutex_
//     (.cpp:197-205) but lookup_params (.cpp:15), store_params (.cpp:41),
//     enqueue (.cpp:86), flush_if_ready (.cpp:100,110,115), flush_all
//     (.cpp:144) and record_launch_latency (.cpp:160) read config_ under
//     batch_mutex_/stats_mutex_/cache-free paths.
// 1 writer thread flips config fields; 3 reader threads hammer the real read
// paths. TSAN must report data races on config_.
#include "kairo/gpu/transfer_optimizer.hpp"
#include "kairo/gpu/kernel_launch_optimizer.hpp"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

using kairo::gpu::TransferOptimizer;
using kairo::gpu::KernelLaunchOptimizer;
using kairo::gpu::TransferRequest;
using kairo::gpu::TransferDirection;
using kairo::gpu::PipelineStage;
using kairo::gpu::BatchedKernelRequest;
using kairo::gpu::KernelParamCacheEntry;

static std::atomic<bool> g_go{false};
static std::atomic<int> g_done{0};

void writer(TransferOptimizer& to, KernelLaunchOptimizer& klo, int iters) {
    while (!g_go.load()) std::this_thread::yield();
    for (int i = 0; i < iters; ++i) {
        const bool on = (i & 1) != 0;
        auto tc = to.get_config();  // locked read, then mutate and write back
        tc.enable_batching = on;
        tc.enable_pipeline = !on;
        tc.enable_pinned_optimization = on;
        tc.small_transfer_threshold = on ? 4096 : 8192;
        tc.batch_size_threshold = on ? 65536 : 131072;
        to.update_config(tc);

        auto kc = klo.get_config();
        kc.enable_param_cache = on;
        kc.enable_batching = !on;
        kc.track_latency = on;
        kc.batch_threshold = on ? 4 : 8;
        kc.batch_window_us = on ? 100 : 200;
        klo.update_config(kc);
    }
    g_done.fetch_add(1);
}

void reader(TransferOptimizer& to, KernelLaunchOptimizer& klo, int iters, int id) {
    while (!g_go.load()) std::this_thread::yield();
    size_t sink = 0;
    for (int i = 0; i < iters; ++i) {
        // --- TransferOptimizer read paths (all touch config_) ---
        TransferRequest req;
        req.dst = &sink;
        req.src = &sink;
        req.size = 128;  // below both small_transfer_threshold variants
        req.direction = TransferDirection::HOST_TO_DEVICE;
        to.enqueue_transfer(req);                       // reads enable_batching
        sink += to.should_use_pinned(req.size) ? 1 : 0; // reads pinned config
        sink += to.recommended_pinned_buffer_size();    // reads pinned_buffer_size

        std::vector<PipelineStage> stages(2);
        stages[0].transfer = req;
        stages[1].transfer = req;
        auto actions = to.build_pipeline(stages, 0, 1); // reads enable_pipeline
        sink += actions.size();

        auto batches = to.flush_batches();              // locked path (control)
        sink += batches.size();

        // --- KernelLaunchOptimizer read paths (all touch config_) ---
        const std::string kname = "k" + std::to_string(id);
        KernelParamCacheEntry entry;
        entry.grid_size[0] = 8;
        KernelParamCacheEntry out;
        if (!klo.lookup_params(kname, out)) {               // reads enable_param_cache
            klo.store_params(kname, entry);
        }
        BatchedKernelRequest br;
        br.kernel_func = nullptr;
        sink += klo.enqueue(br);                        // reads enable_batching
        auto flushed = klo.flush_if_ready();            // reads batch_threshold/window
        sink += flushed.size();
        klo.record_launch_latency(1.0);                 // reads track_latency
        if ((i & 1023) == 0) {
            auto all = klo.flush_all();                 // reads enable_batching
            sink += all.size();
        }
    }
    std::printf("reader %d done, sink=%zu\n", id, sink);
    g_done.fetch_add(1);
}

int main(int argc, char** argv) {
    const int iters = (argc > 1) ? std::atoi(argv[1]) : 200000;
    TransferOptimizer to;
    KernelLaunchOptimizer klo;

    std::vector<std::thread> threads;
    threads.emplace_back(writer, std::ref(to), std::ref(klo), iters);
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back(reader, std::ref(to), std::ref(klo), iters, i);
    }
    g_go.store(true);
    for (auto& t : threads) t.join();
    std::printf("all %d threads finished (%d iters each)\n", g_done.load(), iters);
    return 0;
}
