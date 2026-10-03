// CR-030: Linux set_thread_priority 的 setpriority(PRIO_PROCESS, 0, ...) 作用于
// 调用线程而非 handle 指定的目标线程（thread_utils.cpp:234-242）。
//
// 方法：每个用例 fork 一个独立子进程（隔离 nice 状态，避免不可逆的 nice 提升
// 污染后续用例）。子进程 spawn worker（自报 tid / 初始 nice / 调度策略），
// 主线程调用 set_thread_priority(worker.native_handle(), priority)，随后双方
// 分别读取自身 nice（getpriority(PRIO_PROCESS,0) 在 Linux 上 who=0 等价于
// 当前线程）并用 /proc/<tid>/stat 第 19 列交叉验证。
//
// 预期（若缺陷成立）：SCHED_OTHER + 非 0 priority 路径下，
//   - 返回 true；
//   - 主线程（调用者）nice 变化；
//   - worker（目标线程）nice 不变。
// 另观察 priority=10（1-99 → SCHED_FIFO 路径）在 RLIMIT_RTPRIO=0 非 root 下
// 的实际行为。

#include "kairo/util/thread_utils.hpp"

#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

using kairo::util::set_thread_priority;

static int read_own_nice_getpriority() {
    errno = 0;
    const int n = getpriority(PRIO_PROCESS, 0);
    if (errno != 0) return -1000;
    return n;
}

// /proc/<tid>/stat: pid(1) comm(2) 其后为 state(3)...；nice 为第 19 列，
// 即 ')' 之后的第 17 个 token。
static int read_nice_proc(pid_t tid) {
    std::ifstream f("/proc/" + std::to_string(tid) + "/stat");
    if (!f) return -2000;
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    const auto close = content.rfind(')');
    if (close == std::string::npos) return -2000;
    std::istringstream rest(content.substr(close + 1));
    std::string tok;
    // ')' 后第一个 token 是 field 3 (state)
    for (int field = 3; field <= 19; ++field) {
        if (!(rest >> tok)) return -2000;
    }
    return std::stoi(tok);
}

struct WorkerReport {
    std::atomic<pid_t> tid{0};
    std::atomic<bool> ready{false};
    std::atomic<bool> applied{false};
    std::atomic<bool> done{false};
    std::atomic<int> nice_before{0};
    std::atomic<int> nice_after{0};
    std::atomic<int> proc_nice_after{0};
    std::atomic<int> policy_after{0};
};

static void worker_body(WorkerReport* r, std::atomic<bool>* go) {
    r->tid.store(static_cast<pid_t>(syscall(SYS_gettid)));
    r->nice_before.store(read_own_nice_getpriority());
    r->ready.store(true);
    while (!go->load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    r->nice_after.store(read_own_nice_getpriority());
    r->proc_nice_after.store(read_nice_proc(r->tid.load()));
    sched_param param{};
    int policy = 0;
    if (pthread_getschedparam(pthread_self(), &policy, &param) == 0) {
        r->policy_after.store(policy);
    }
    r->done.store(true);
}

static int run_case(int priority) {
    const int main_nice_before = read_own_nice_getpriority();

    WorkerReport report;
    std::atomic<bool> go{false};
    std::thread worker(worker_body, &report, &go);
    while (!report.ready.load()) {
        std::this_thread::yield();
    }

    const bool ret = set_thread_priority(worker.native_handle(), priority);
    go.store(true);
    while (!report.done.load()) {
        std::this_thread::yield();
    }
    worker.join();

    const int main_nice_after = read_own_nice_getpriority();

    std::printf("[case priority=%d]\n", priority);
    std::printf("  set_thread_priority(worker_handle, %d) -> %s\n", priority,
                ret ? "true" : "false");
    std::printf("  main (caller)  nice: %d -> %d\n", main_nice_before,
                main_nice_after);
    std::printf("  worker (target tid=%d) nice: %d -> %d (getpriority) / %d (/proc stat)\n",
                static_cast<int>(report.tid.load()), report.nice_before.load(),
                report.nice_after.load(), report.proc_nice_after.load());
    std::printf("  worker policy after: %d (0=SCHED_OTHER, 1=SCHED_FIFO, 2=SCHED_RR)\n",
                report.policy_after.load());

    const bool caller_changed = main_nice_after != main_nice_before;
    const bool target_changed =
        report.nice_after.load() != report.nice_before.load() ||
        report.proc_nice_after.load() != report.nice_before.load();

    if (ret && caller_changed && !target_changed) {
        std::printf("  => CR-030 CONFIRMED: effect applied to CALLER, target unchanged\n\n");
        return 1;
    }
    if (ret && !caller_changed && !target_changed) {
        std::printf("  => neither thread changed (priority skipped or no-op path)\n\n");
        return 0;
    }
    if (!ret) {
        std::printf("  => call failed before effect (permission path observed)\n\n");
        return 0;
    }
    std::printf("  => other outcome (target changed=%d, caller changed=%d)\n\n",
                target_changed, caller_changed);
    return 2;
}

int main() {
    std::printf("pid=%d initial nice=%d, RLIMIT_RTPRIO soft=%ld hard=%ld\n\n",
                static_cast<int>(getpid()), read_own_nice_getpriority(),
                -1L, -1L);
    struct rlimit rl;
    if (getrlimit(RLIMIT_RTPRIO, &rl) == 0) {
        std::printf("RLIMIT_RTPRIO: soft=%lu hard=%lu (0 => SCHED_FIFO path "
                    "will EPERM for non-root)\n\n", rl.rlim_cur, rl.rlim_max);
    }

    // 用例 1：reviewer 场景 priority=10（落在 1-99 → SCHED_FIFO 路径，
    //          非 root 下 EPERM 直接 false，永远到不了 nice 路径）
    // 用例 2：priority=100（钳到 19；SCHED_OTHER 路径，nice=19 降低优先级
    //          无需特权 → 直达 setpriority(PRIO_PROCESS,0,...) 缺陷路径）
    // 用例 3：priority=0（SCHED_OTHER，实现显式跳过 nice 调整）
    // 用例 4：priority=-5（SCHED_OTHER，nice=-5 提升优先级需要特权 →
    //          setpriority EACCES，观察失败路径）
    const int cases[] = {10, 100, 0, -5};
    int confirmed = 0;
    for (int prio : cases) {
        // fork 隔离：nice 变更不可逆（非 root 无法把 nice 升回 0）
        pid_t pid = fork();
        if (pid == 0) {
            // 重定向下 stdout 全缓冲：打印后必须显式 flush 再 _exit
            const int rc = run_case(prio);
            fflush(stdout);
            fflush(stderr);
            _exit(rc);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 1) {
            ++confirmed;
        }
    }
    std::printf("cases with caller-only effect: %d\n", confirmed);
    return 0;
}
