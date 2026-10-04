// CR-033: LockFreeQueue::cancel_reservation 的自旋循环（lockfree_queue.hpp:526-548）
// 只检查 Published/Writing/BatchWriting 三种逃逸态，不检查 Cancelled ——
// 当另一消费者已把槽位取消后，本消费者仍会空转完整个 yield 预算。
//
// 构造（直接使用内部模板，buffer 由外部持有语义按 trivially-copyable 值传递）：
//  - 安装阻塞型 before_publish_hook，使生产者停在 Reserved 态（可取消窗口）；
//  - 消费者 C1 先 pop：进入 cancel_reservation 自旋（预算 = reservation_wait_yields）；
//  - Δ=200ms 后消费者 C2 pop：同样进入自旋；
//  - C1 预算先耗尽 → C1 完成 Reserved→Cancelled 取消并推进前沿；
//  - 若自旋不检查 Cancelled：C2 会无视已取消态，继续空转至自身预算耗尽，
//    其 pop 耗时 ≈ C1 耗时 + Δ（对照：若实现检查 Cancelled，C2 应在 C1 取消
//    瞬间提前退出，耗时 ≈ C1 耗时）。
//  - 对照组：单消费者单独取消同一场景，得到基线预算时长 D_base。

#include "kairo/util/lockfree_queue.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

using kairo::util::LockFreeQueue;

namespace {

constexpr size_t kBudgetYields = 200000;  // cancel_reservation 单次自旋预算
// 第二消费者必须在第一消费者的预算窗口内进入自旋（本机 200k yields ≈ 50ms），
// 取 20ms：缺陷成立时 C2 会在取消完成后仍空转 ~20ms。
constexpr auto kSecondConsumerDelay = std::chrono::milliseconds(20);

std::atomic<bool> g_hook_release{false};

void blocking_hook(void*) {
    while (!g_hook_release.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct PopResult {
    std::atomic<bool> done{false};
    std::atomic<uint64_t> dur_ns{0};
    std::atomic<size_t> popped{0};
};

void consumer_thread(LockFreeQueue<size_t>* q, PopResult* out) {
    size_t item = 0;
    const uint64_t t0 = now_ns();
    const bool ok = q->pop(item);
    const uint64_t t1 = now_ns();
    out->popped.store(ok ? 1 : 0);
    out->dur_ns.store(t1 - t0);
    out->done.store(true);
}

// 阶段流程：生产者驻停一个 Reserved 前沿槽位，按给定消费者数依次启动 pop，
// 返回各消费者耗时（ns）。
void run_phase(const char* name, int consumer_count,
               std::vector<uint64_t>& durs, bool& cancelled_seen) {
    LockFreeQueue<size_t> q(64, 1, true, kBudgetYields);
    g_hook_release.store(false);
    q.set_before_publish_hook(&blocking_hook, nullptr);

    std::thread producer([&] {
        size_t v = 42;
        (void)q.push(v);  // 预期最终因槽位被取消而返回 false
    });

    // 等待生产者进入 Reserved（hook 阻塞中）
    while (q.get_stats().reserved_count < 1) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    std::vector<std::thread> consumers;
    std::vector<PopResult> results(consumer_count);
    for (int i = 0; i < consumer_count; ++i) {
        consumers.emplace_back(consumer_thread, &q, &results[i]);
        if (i + 1 < consumer_count) {
            std::this_thread::sleep_for(kSecondConsumerDelay);
        }
    }
    for (auto& c : consumers) {
        c.join();
    }
    durs.clear();
    for (int i = 0; i < consumer_count; ++i) {
        durs.push_back(results[i].dur_ns.load());
        std::cout << "  [" << name << "] consumer" << i
                  << " pop dur=" << results[i].dur_ns.load() / 1000000.0
                  << "ms popped=" << results[i].popped.load() << "\n";
    }
    cancelled_seen = q.get_stats().cancelled_reservation_count >= 1;

    // 放开 hook，生产者 begin_write 的 Reserved→Writing CAS 将失败退出
    g_hook_release.store(true);
    producer.join();
    std::cout << "  [" << name << "] cancelled_reservation_count="
              << q.get_stats().cancelled_reservation_count
              << " reservation_cancelled_rejections="
              << q.get_stats().reservation_cancelled_rejections << "\n";
    q.set_before_publish_hook(nullptr, nullptr);
}

}  // namespace

int main() {
    std::cout << "CR-033: cancel_reservation spin ignores Cancelled state "
                 "(budget = " << kBudgetYields << " yields/consumer)\n";

    // 对照组：单消费者取消基线
    std::vector<uint64_t> base;
    bool seen = false;
    run_phase("baseline-1c", 1, base, seen);
    const double base_ms = base.empty() ? -1.0 : base[0] / 1000000.0;

    // 主实验：C1 先入，Δ 后 C2 入
    std::vector<uint64_t> durs;
    bool cancelled_seen = false;
    run_phase("two-consumers", 2, durs, cancelled_seen);
    if (durs.size() < 2) {
        std::cout << "phase failed\n";
        return 2;
    }
    const double d1 = durs[0] / 1000000.0;
    const double d2 = durs[1] / 1000000.0;
    const double delta = d2 - d1;

    std::cout << "---- analysis ----\n";
    std::cout << "baseline solo-cancel duration D_base = " << base_ms << "ms\n";
    std::cout << "C1 (first)  = " << d1 << "ms\n";
    std::cout << "C2 (second, started " << kSecondConsumerDelay.count()
              << "ms later) = " << d2 << "ms ; D2-D1 = " << delta << "ms\n";
    std::cout << "cancellation completed while C2 still spinning (slot "
                 "Cancelled observed via stats after C1 returned): "
              << (cancelled_seen ? "yes" : "no") << "\n";
    std::cout << "note: spin loop at lockfree_queue.hpp:526-548 escapes on "
                 "Published/Writing/BatchWriting but has no Cancelled check; "
                 "a Cancelled-aware implementation would end C2 at ~D1.\n";

    const bool c1_matches_budget = base_ms > 0 && d1 > 0.5 * base_ms;
    const bool c2_spun_past_cancel =
        delta >= 0.5 * kSecondConsumerDelay.count();
    if (c1_matches_budget && c2_spun_past_cancel && cancelled_seen) {
        std::cout << "---- verdict ----\n"
                  << "CR-033 CONFIRMED: second consumer kept spinning ~"
                  << delta << "ms past the completed cancellation "
                  "(full-budget spin despite Cancelled state)\n";
        return 0;
    }
    std::cout << "---- verdict ----\n"
              << "CR-033 NOT CONFIRMED by this run (c1_matches_budget="
              << c1_matches_budget << ", c2_spun_past_cancel="
              << c2_spun_past_cancel << ", cancelled_seen=" << cancelled_seen
              << ")\n";
    return 1;
}
