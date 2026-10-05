// tests/test_current_thread_utils.cpp
//
// MIRA-20260922-001 新增 API set_current_thread_priority /
// set_current_thread_affinity 的行为测试（standalone 风格，同 test_util.cpp）。
// 核心诉求：调用线程自应用优先级/亲和性时无需也无法构造
// std::thread::native_handle_type（MinGW posix 线程模型下 Win32 伪 HANDLE
// 与该类型不相容），平台实现内部自取线程标识。
//
// 验证内容：
//  1. set_current_thread_priority(0) 返回 true，get_current_thread_priority()
//     读回与线程实际调度属性一致（Linux SCHED_OTHER 下该调用不改 nice，
//     读回应等于线程实际 nice 值；Windows 下 NORMAL 读回 0）。
//  2. set_current_thread_affinity(get_current_thread_affinity() 的子集) 成功
//     后读回恰等于该子集；失败（权限/cgroup 限制）时容错放行并如实记录，
//     参照 test_util.cpp 现有写法。
//  3. 边界：空列表与负 CPU 编号必须返回 false（两平台实现均如此）。
//  4. 隔离性：worker 线程体内自应用亲和性不得波及主线程（回归守护，
//     防“改错线程”一类缺陷复发）。
//
// Linux 主机实跑；Windows 分支随 MinGW 交叉编译做编译/链接验证，运行时
// 语义需实机 Windows 环境。

#include <algorithm>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "kairo/util/thread_utils.hpp"

using kairo::util::get_current_thread_affinity;
using kairo::util::get_current_thread_priority;
using kairo::util::set_current_thread_affinity;
using kairo::util::set_current_thread_priority;

// 测试辅助宏（同 test_util.cpp）
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":" \
                      << __LINE__ << std::endl; \
            return false; \
        } \
    } while (0)

namespace {

// 取 current 的前缀构造子集：size >= 2 时是真子集（前 2 个），size == 1 时
// 为单元素列表。current 由实现升序返回，前缀保持有序。
std::vector<int> prefix_subset(const std::vector<int>& current) {
    const size_t n = std::min<size_t>(2, current.size());
    return std::vector<int>(current.begin(),
                            current.begin() + static_cast<std::ptrdiff_t>(n));
}

}  // namespace

#if defined(__linux__)

#include <cerrno>
#include <sched.h>
#include <sys/resource.h>

// 用例 1：set_current_thread_priority(0) 在主线程返回 true，读回一致。
// set(0) 走 SCHED_OTHER 且跳过 nice 调整，因此线程 nice 不变，
// get_current_thread_priority()（SCHED_OTHER 分支读 nice）应与内核实际值一致。
bool test_set_current_thread_priority_zero_main() {
    std::cout << "Testing set_current_thread_priority(0) on main thread..."
              << std::endl;

    errno = 0;
    const int nice_before = getpriority(PRIO_PROCESS, 0);
    TEST_ASSERT(errno == 0, "getpriority(PRIO_PROCESS, 0) before failed");

    const int policy_before = sched_getscheduler(0);  // 0 = 调用线程

    TEST_ASSERT(set_current_thread_priority(0),
                "set_current_thread_priority(0) should succeed for the "
                "calling thread on Linux (SCHED_OTHER)");

    errno = 0;
    const int nice_after = getpriority(PRIO_PROCESS, 0);
    TEST_ASSERT(errno == 0, "getpriority(PRIO_PROCESS, 0) after failed");

    const int readback = get_current_thread_priority();
    // set(0) 后策略必为 SCHED_OTHER，读回应等于线程实际 nice。
    TEST_ASSERT(readback == nice_after,
                "get_current_thread_priority() should equal the thread's "
                "actual nice value after set(0)");
    if (policy_before == SCHED_OTHER) {
        TEST_ASSERT(nice_after == nice_before,
                    "set(0) must not change the thread's nice value");
    }

    std::cout << "  policy_before=" << policy_before
              << " nice_before=" << nice_before
              << " nice_after=" << nice_after
              << " readback=" << readback << std::endl;
    std::cout << "  set_current_thread_priority(0) main thread: PASSED"
              << std::endl;
    return true;
}

// 用例 1b：同一调用在新 worker 线程体内（执行器的实际使用模式）同样成立。
bool test_set_current_thread_priority_zero_worker() {
    std::cout << "Testing set_current_thread_priority(0) inside a worker..."
              << std::endl;

    struct Result {
        bool set_ok = false;
        int nice_before = 0;
        int nice_after = 0;
        int readback = 0;
    } result;

    std::thread worker([&result]() {
        errno = 0;
        result.nice_before = getpriority(PRIO_PROCESS, 0);
        result.set_ok = set_current_thread_priority(0);
        errno = 0;
        result.nice_after = getpriority(PRIO_PROCESS, 0);
        result.readback = get_current_thread_priority();
    });
    worker.join();  // join 即同步，Result 可安全读取

    TEST_ASSERT(result.set_ok,
                "set_current_thread_priority(0) should succeed inside the "
                "worker thread itself");
    TEST_ASSERT(result.nice_after == result.nice_before,
                "set(0) in worker must not change the worker's nice value");
    TEST_ASSERT(result.readback == result.nice_after,
                "worker read-back should equal the worker's actual nice");

    std::cout << "  nice_before=" << result.nice_before
              << " nice_after=" << result.nice_after
              << " readback=" << result.readback << std::endl;
    std::cout << "  set_current_thread_priority(0) worker: PASSED" << std::endl;
    return true;
}

// 用例 2：子集亲和性设置后读回一致；受限环境失败时容错。
bool test_set_current_thread_affinity_subset() {
    std::cout << "Testing set_current_thread_affinity with a subset..."
              << std::endl;

    const std::vector<int> current = get_current_thread_affinity();
    if (current.empty()) {
        std::cout << "  SKIP: cannot read current affinity (restricted env)"
                  << std::endl;
        return true;
    }
    std::cout << "  current affinity (" << current.size() << " cpus): ";
    for (int cpu : current) std::cout << cpu << " ";
    std::cout << std::endl;

    const std::vector<int> subset = prefix_subset(current);
    const bool set_ok = set_current_thread_affinity(subset);
    if (!set_ok) {
        // 权限/cgroup 限制导致的失败按现有测试惯例容错，如实记录。
        std::cout << "  set failed (permissions/cgroup), tolerated"
                  << std::endl;
        std::cout << "  set_current_thread_affinity subset: PASSED (tolerated)"
                  << std::endl;
        return true;
    }

    const std::vector<int> after = get_current_thread_affinity();
    TEST_ASSERT(after == subset,
                "affinity read-back should exactly equal the requested subset");

    std::cout << "  subset: ";
    for (int cpu : subset) std::cout << cpu << " ";
    std::cout << "| read-back: ";
    for (int cpu : after) std::cout << cpu << " ";
    std::cout << std::endl;
    std::cout << "  set_current_thread_affinity subset: PASSED" << std::endl;
    return true;
}

// 用例 3：平台无关边界——空列表与负 CPU 编号必须失败。
bool test_set_current_thread_affinity_invalid_input() {
    std::cout << "Testing invalid affinity inputs..." << std::endl;

    TEST_ASSERT(!set_current_thread_affinity({}),
                "empty cpu list must be rejected");
    TEST_ASSERT(!set_current_thread_affinity({-1}),
                "negative cpu id must be rejected");

    std::cout << "  invalid inputs rejected: PASSED" << std::endl;
    return true;
}

// 用例 4：隔离性——worker 内自应用亲和性不得波及主线程。
bool test_current_thread_affinity_isolation() {
    std::cout << "Testing main-thread affinity isolation from worker..."
              << std::endl;

    const std::vector<int> before = get_current_thread_affinity();
    if (before.empty()) {
        std::cout << "  SKIP: cannot read main-thread affinity" << std::endl;
        return true;
    }

    std::thread worker([&before]() {
        // worker 只改自己；失败（权限）不影响本用例结论。
        (void)set_current_thread_affinity({before[0]});
    });
    worker.join();

    const std::vector<int> after = get_current_thread_affinity();
    TEST_ASSERT(after == before,
                "main-thread affinity must not be affected by a worker "
                "applying affinity to itself");

    std::cout << "  main-thread affinity unchanged after worker self-apply: "
              << "PASSED" << std::endl;
    return true;
}

#elif defined(_WIN32)

// Windows 分支：验证新 API 可经 GetCurrentThread() 伪句柄自应用。
// 运行时验证需实机 Windows；交叉构建只做编译/链接验证。
bool test_set_current_thread_priority_zero_main() {
    std::cout << "Testing set_current_thread_priority(0) on current thread..."
              << std::endl;
    TEST_ASSERT(set_current_thread_priority(0),
                "set_current_thread_priority(0) should succeed");
    TEST_ASSERT(get_current_thread_priority() == 0,
                "THREAD_PRIORITY_NORMAL should read back as 0");
    std::cout << "  set_current_thread_priority(0): PASSED" << std::endl;
    return true;
}

bool test_set_current_thread_priority_zero_worker() {
    std::cout << "Testing set_current_thread_priority(0) inside a worker..."
              << std::endl;
    struct Result {
        bool set_ok = false;
        int readback = -1;
    } result;
    std::thread worker([&result]() {
        result.set_ok = set_current_thread_priority(0);
        result.readback = get_current_thread_priority();
    });
    worker.join();
    TEST_ASSERT(result.set_ok, "worker self-apply priority should succeed");
    TEST_ASSERT(result.readback == 0, "worker read-back should be NORMAL(0)");
    std::cout << "  worker priority: PASSED" << std::endl;
    return true;
}

bool test_set_current_thread_affinity_subset() {
    std::cout << "Testing set_current_thread_affinity with a subset..."
              << std::endl;
    const std::vector<int> current = get_current_thread_affinity();
    if (current.empty()) {
        std::cout << "  SKIP: cannot read current affinity" << std::endl;
        return true;
    }
    const std::vector<int> subset = prefix_subset(current);
    const bool set_ok = set_current_thread_affinity(subset);
    if (!set_ok) {
        std::cout << "  set failed (permissions), tolerated" << std::endl;
        return true;
    }
    const std::vector<int> after = get_current_thread_affinity();
    TEST_ASSERT(after == subset, "read-back should equal the subset");
    std::cout << "  affinity subset: PASSED" << std::endl;
    return true;
}

bool test_set_current_thread_affinity_invalid_input() {
    std::cout << "Testing invalid affinity inputs..." << std::endl;
    TEST_ASSERT(!set_current_thread_affinity({}),
                "empty cpu list must be rejected");
    TEST_ASSERT(!set_current_thread_affinity({-1}),
                "negative cpu id must be rejected");
    std::cout << "  invalid inputs rejected: PASSED" << std::endl;
    return true;
}

bool test_current_thread_affinity_isolation() {
    std::cout << "Testing main-thread affinity isolation from worker..."
              << std::endl;
    const std::vector<int> before = get_current_thread_affinity();
    if (before.empty()) {
        std::cout << "  SKIP: cannot read main-thread affinity" << std::endl;
        return true;
    }
    std::thread worker([&before]() {
        (void)set_current_thread_affinity({before[0]});
    });
    worker.join();
    const std::vector<int> after = get_current_thread_affinity();
    TEST_ASSERT(after == before,
                "main-thread affinity must not be affected by worker");
    std::cout << "  isolation: PASSED" << std::endl;
    return true;
}

#else

// 其他平台：本测试目标仅保证可编译，跳过行为验证。
bool test_set_current_thread_priority_zero_main() { return true; }
bool test_set_current_thread_priority_zero_worker() { return true; }
bool test_set_current_thread_affinity_subset() { return true; }
bool test_set_current_thread_affinity_invalid_input() { return true; }
bool test_current_thread_affinity_isolation() { return true; }

#endif

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Current-Thread Utils Tests (MIRA-20260922-001)" << std::endl;
    std::cout << "========================================" << std::endl;

    bool all_passed = true;

    all_passed &= test_set_current_thread_priority_zero_main();
    all_passed &= test_set_current_thread_priority_zero_worker();
    all_passed &= test_set_current_thread_affinity_subset();
    all_passed &= test_set_current_thread_affinity_invalid_input();
    all_passed &= test_current_thread_affinity_isolation();

    std::cout << "========================================" << std::endl;
    if (all_passed) {
        std::cout << "All tests PASSED!" << std::endl;
        return 0;
    }
    std::cout << "Some tests FAILED!" << std::endl;
    return 1;
}
