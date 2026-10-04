// CR-031: LockFreeTaskExecutor pool nodes keep the previous task's callable
// alive after execution (object_pool.hpp release() never destroys the stored
// std::function; lockfree_task_executor.cpp:476 release_bulk happens right
// after batch[i]->func()). The stale callable is destroyed later, on a
// PRODUCER thread, when the node is re-acquired and
// `wrapper->func = std::move(task)` runs (lockfree_task_executor.cpp:119).
//
// Sub-check (a) retention: T1 holds a shared_ptr<Guard>; after the worker has
//   executed T1 and 300ms elapsed, Guard must still be alive if the callable
//   lingers in the pool node.
// Sub-check (b) destructor-thread transfer: a subsequent push_task from the
//   main thread re-acquires that node; Guard's destructor must then run on
//   the MAIN (producer) thread, not the worker thread.

#include "kairo/lockfree_task_executor.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

using kairo::LockFreeTaskExecutor;

namespace {

pid_t gettid_sys() { return static_cast<pid_t>(syscall(SYS_gettid)); }

long long now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

struct DtorReport {
    std::atomic<bool> ran{false};
    std::atomic<pid_t> tid{0};
    std::atomic<long long> when{0};
};

struct Guard {
    DtorReport* rep;
    ~Guard() {
        rep->when = now_ns();
        rep->tid = gettid_sys();
        rep->ran = true;
    }
};

}  // namespace

int main() {
    const pid_t main_tid = gettid_sys();
    LockFreeTaskExecutor ex(1024);
    if (!ex.start()) {
        std::printf("FATAL: executor start failed\n");
        return 2;
    }

    DtorReport rep;
    auto guard = std::make_shared<Guard>(&rep);
    std::weak_ptr<Guard> w = guard;

    std::atomic<bool> t1_done{false};
    std::atomic<pid_t> t1_exec_tid{0};
    std::atomic<long long> t2_exec_at{0};

    // T1: the pooled callable owns the Guard.
    {
        auto t1 = [guard, &t1_done, &t1_exec_tid] {
            t1_exec_tid = gettid_sys();
            t1_done = true;
        };
        if (!ex.push_task(std::move(t1))) {
            std::printf("FATAL: push T1 failed\n");
            return 2;
        }
    }
    guard.reset();  // only the pooled callable holds the Guard now

    while (!t1_done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const pid_t worker_tid = t1_exec_tid.load();
    std::printf("T1 executed on tid=%d (main=%d)\n", worker_tid, main_tid);

    // (a) give the worker ample time to have released the wrapper back to the
    // pool, then check whether the Guard survived in the node.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const bool retained = !w.expired();
    std::printf("(a) 300ms after T1 execution: Guard still alive=%s -> %s\n",
                retained ? "YES" : "NO",
                retained ? "retention CONFIRMED" : "retention NOT reproduced");

    // (b) producer-side push to force reuse of the released node.
    const long long t2_push_started = now_ns();
    const bool ok2 = ex.push_task([&t2_exec_at] {
        t2_exec_at = now_ns();
    });
    const long long t2_push_returned = now_ns();
    std::printf("(b) push T2 from main: ok=%s\n", ok2 ? "true" : "false");
    while (ex.processed_count() < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const bool dtor_ran = rep.ran.load();
    const pid_t dtor_tid = rep.tid.load();
    const long long dtor_when = rep.when.load();
    const bool dtor_on_main = dtor_ran && dtor_tid == main_tid;
    const bool dtor_during_push =
        dtor_ran && dtor_when >= t2_push_started && dtor_when <= t2_push_returned;

    std::printf("(b) Guard dtor ran=%s on tid=%d (main=%d, worker=%d), "
                "during T2 push=%s\n",
                dtor_ran ? "YES" : "NO", dtor_tid, main_tid, worker_tid,
                dtor_during_push ? "YES" : "NO");

    ex.stop_and_join();

    const bool retention_confirmed = retained;
    const bool transfer_confirmed = dtor_on_main && dtor_during_push && w.expired();
    std::printf("CR-031 VERDICT: (a) retention %s | (b) destructor transferred "
                "to producer thread %s\n",
                retention_confirmed ? "CONFIRMED" : "NOT REPRODUCED",
                transfer_confirmed ? "CONFIRMED" : "NOT REPRODUCED");
    return 0;
}
