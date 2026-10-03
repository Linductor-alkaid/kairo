// CR-031: ObjectPool 节点不析构 callable —— 上一个任务的 lambda（及其捕获的
// 资源）在任务完成后仍滞留池节点内；下一次 acquire 复用该节点时，析构发生在
// 生产者线程（object_pool.hpp:103-131,159-209；调用点
// lockfree_task_executor.cpp:119,476,509）。
//
// 方法：
//  T1 捕获 shared_ptr<Guard>，Guard 析构时记录析构线程 tid 与时刻。
//  worker 执行 T1（ObjectPool release_bulk 不调用析构）→ 主线程丢弃自身引用
//  后 sleep 200ms：
//   (a) weak_ptr.lock() 仍为 true  → 资源滞留池节点 CONFIRMED；
//   (b) 随后生产者（主线程）提交 T2，push_task 内 wrapper->func = move(task)
//       对旧 function 赋值 → Guard 析构；若析构 tid == 主线程 tid 且
//       != worker tid → 析构转移到生产者线程 CONFIRMED。

#include "kairo/lockfree_task_executor.hpp"

#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>

using kairo::LockFreeTaskExecutor;

static uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Guard {
    std::atomic<pid_t>* dtid;
    std::atomic<uint64_t>* dtime;
    std::atomic<bool>* destroyed;
    Guard(std::atomic<pid_t>* a, std::atomic<uint64_t>* b, std::atomic<bool>* c)
        : dtid(a), dtime(b), destroyed(c) {}
    ~Guard() {
        if (dtid) dtid->store(static_cast<pid_t>(syscall(SYS_gettid)));
        if (dtime) dtime->store(now_ms());
        if (destroyed) destroyed->store(true);
    }
};

static pid_t self_tid() {
    return static_cast<pid_t>(syscall(SYS_gettid));
}

int main() {
    const pid_t main_tid = self_tid();

    LockFreeTaskExecutor exec(1024);
    if (!exec.start()) {
        std::cerr << "FATAL: executor start failed\n";
        return 2;
    }

    // 先用探测任务拿 worker 线程 tid
    std::atomic<pid_t> worker_tid{0};
    std::atomic<bool> probed{false};
    if (!exec.push_task([&] {
            worker_tid.store(self_tid());
            probed.store(true);
        })) {
        std::cerr << "FATAL: probe push failed\n";
        return 2;
    }
    while (!probed.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // T1：持有 Guard 的任务
    std::atomic<bool> t1_executed{false};
    std::atomic<pid_t> guard_dtid{0};
    std::atomic<uint64_t> guard_dtime{0};
    std::atomic<bool> guard_destroyed{false};

    auto guard = std::make_shared<Guard>(&guard_dtid, &guard_dtime, &guard_destroyed);
    std::weak_ptr<Guard> observer = guard;

    {
        auto g = guard;  // lambda 持有一份共享引用
        if (!exec.push_task([g, &t1_executed] { t1_executed.store(true); })) {
            std::cerr << "FATAL: T1 push failed\n";
            return 2;
        }
    }
    while (!t1_executed.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    guard = nullptr;  // 主线程放弃所有权，唯一副本应留在已执行的 lambda 内

    // (a) 滞留观察：worker 已执行并 release_bulk 回池，析构不应发生
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const bool still_alive_after_200ms = !observer.expired();
    const bool destroyed_before_t2 = guard_destroyed.load();
    const bool worker_done_releasing = exec.processed_count() >= 2;

    std::cout << "main_tid=" << static_cast<long>(main_tid)
              << " worker_tid=" << static_cast<long>(worker_tid.load()) << "\n";
    std::cout << "[retention] after T1 executed + 200ms:\n";
    std::cout << "  processed_count=" << exec.processed_count()
              << " (worker past release_bulk: " << worker_done_releasing << ")\n";
    std::cout << "  weak_ptr.lock()==" << (still_alive_after_200ms ? "true" : "false")
              << "  destroyed==" << destroyed_before_t2 << "\n";
    const bool retention_confirmed =
        still_alive_after_200ms && !destroyed_before_t2 && worker_done_releasing;

    // (b) 析构转移观察：生产者线程提交 T2，节点复用触发旧 lambda 析构
    const uint64_t t2_push_ms = now_ms();
    const bool t2_ok = exec.push_task([] {});
    const bool destroyed_immediately_after_t2_push = guard_destroyed.load();
    // 兜底：若一次 T2 未复用到该节点（理论上 LIFO 必然复用），再补几次
    int extra_pushes = 0;
    while (!guard_destroyed.load() && extra_pushes < 8) {
        exec.push_task([] {});
        ++extra_pushes;
    }
    while (exec.processed_count() < 2 + 1 + extra_pushes) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const pid_t dtid = guard_dtid.load();
    const uint64_t dtime = guard_dtime.load();
    std::cout << "[destruction transfer] T2 pushed from producer (main) thread:\n";
    std::cout << "  push_task(T2) ok=" << t2_ok << "\n";
    std::cout << "  Guard destroyed=" << guard_destroyed.load()
              << " at dtid=" << static_cast<long>(dtid)
              << " (main_tid=" << static_cast<long>(main_tid)
              << ", worker_tid=" << static_cast<long>(worker_tid.load()) << ")\n";
    std::cout << "  destroyed during T2 push on producer thread: "
              << destroyed_immediately_after_t2_push
              << " ; destructor lag after push: " << static_cast<long>(dtime - t2_push_ms)
              << "ms ; extra pushes needed: " << extra_pushes << "\n";

    const bool transfer_confirmed =
        guard_destroyed.load() && dtid == main_tid && dtid != worker_tid.load();

    exec.stop();

    std::cout << "---- verdict ----\n";
    std::cout << "CR-031a (callable retained in pool after task completion): "
              << (retention_confirmed ? "CONFIRMED" : "NOT REPRODUCED") << "\n";
    std::cout << "CR-031b (destructor runs on producer thread via node reuse): "
              << (transfer_confirmed ? "CONFIRMED" : "NOT REPRODUCED") << "\n";
    return (retention_confirmed && transfer_confirmed) ? 0 : 1;
}
