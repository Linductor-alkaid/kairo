// CR-013: 只读诊断路径是否懒创建默认线程池（executor_manager.cpp:167-185）
//
// 判定方法：新进程内读取 /proc/self/status 的线程数，
// 调用 const 诊断接口 Executor::get_async_executor_status()，再读线程数。
// 线程数显著增加（默认池 worker 被拉起）→ CONFIRMED。
#include <kairo/executor.hpp>

#include <cstdio>
#include <cstring>
#include <string>

using namespace kairo;

static int read_thread_count() {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    int threads = -1;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "Threads:", 8) == 0) {
            threads = std::atoi(line + 8);
            break;
        }
    }
    std::fclose(f);
    return threads;
}

int main(int argc, char** argv) {
    bool control = (argc > 1 && std::string(argv[1]) == "control");
    int before = read_thread_count();
    if (control) {
        // 对照：不调用任何 facade 接口，仅等待后重读
        struct timespec ts{0, 100 * 1000 * 1000};  // 100ms
        nanosleep(&ts, nullptr);
        int after = read_thread_count();
        std::printf("CONTROL: threads before=%d after=%d delta=%d\n", before,
                    after, after - before);
        return 0;
    }

    // 此时进程尚未触碰任何 facade 接口；instance() 本身只构造 facade 对象。
    auto& ex = Executor::instance();
    int after_instance = read_thread_count();

    // 只读诊断接口（const）：executor.cpp:1561 get_async_executor_status()
    // → manager_->get_default_async_executor_snapshot() → call_once 懒初始化。
    auto status = ex.get_async_executor_status();
    int after_read = read_thread_count();

    std::printf("READ PATH: threads before=%d after_instance()=%d "
                "after get_async_executor_status()=%d delta=%d\n",
                before, after_instance, after_read, after_read - before);
    std::printf("status: name=%s is_running=%d threads=%u\n",
                status.name.c_str(), static_cast<int>(status.is_running),
                status.queue_size);
    std::printf("hardware_concurrency=%u\n", std::thread::hardware_concurrency());

    if (after_read - before >= 2) {
        std::printf("CR-013 VERDICT: CONFIRMED — read-only diagnostic spun up "
                    "the default pool (%d -> %d threads)\n",
                    before, after_read);
        return 0;
    }
    std::printf("CR-013 VERDICT: NOT REPRODUCED (delta=%d)\n",
                after_read - before);
    return 1;
}
