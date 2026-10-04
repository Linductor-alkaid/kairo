// CR-001: 单例退出顺序 UAF（executor.cpp:185-198，~Executor 单例模式不排空）
//
// 模式：
//   singleton_plain : 单例 + 在途任务(300ms) + main 立即 return（自然退出顺序）
//   singleton_widow : 单例 + 在途任务(300ms) + 在 main 之前构造的全局对象的
//                     析构中 sleep 2s，把静态析构窗口拉长，让 worker 在
//                     facade 已析构、进程未退出的窗口内醒来触达 facade 成员
//   instance        : 对照组，实例模式（~Executor 执行 shutdown(true) 排空）
//
// 预期：singleton_widow 下 worker 醒来触达已析构的 facade 堆成员
// （cancellation_registry_ / task_dependencies_ 等）→ ASAN heap-use-after-free。
#include <kairo/executor.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

using namespace kairo;
using namespace std::chrono_literals;

namespace {

int g_widow_seconds = 0;  // >0 时全局哨兵析构 sleep，拉长静态析构窗口

struct WidowSentinel {
    // 全局对象：先于 main 构造 → 在所有函数级 static 之后析构。
    ~WidowSentinel() {
        if (g_widow_seconds > 0) {
            std::printf("[sentinel] sleeping %ds during static destruction\n",
                        g_widow_seconds);
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::seconds(g_widow_seconds));
            std::printf("[sentinel] done\n");
        }
    }
};

WidowSentinel g_sentinel;

}  // namespace

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "singleton_plain";

    if (std::strcmp(mode, "singleton_plain") == 0 ||
        std::strcmp(mode, "singleton_widow") == 0) {
        if (std::strcmp(mode, "singleton_widow") == 0) {
            g_widow_seconds = 2;
        }
        auto& ex = Executor::instance();
        if (!ex.initialize(ExecutorConfig{})) {
            std::printf("initialize failed\n");
            return 1;
        }
        auto payload = std::make_shared<std::string>("heap-payload");
        // 用 tracked 路径（submit_with_handle）：其 wrapper 收尾会触达
        // facade 的堆成员（task_dependencies_ / cancellation_registry_）。
        // submit_auto/submit 走 legacy 路径，不触达这些成员。
        auto sub = ex.submit_with_handle([payload] {
            std::this_thread::sleep_for(300ms);
            std::printf("[task] ran after exit began, payload=%s\n",
                        payload->c_str());
            std::fflush(stdout);
        });
        (void)sub;
        std::printf("[main] submitted, returning immediately (mode=%s)\n", mode);
        std::fflush(stdout);
        return 0;  // 静态析构：~Executor（单例不排空）→ sentinel
    }

    if (std::strcmp(mode, "instance") == 0) {
        {
            Executor ex;
            if (!ex.initialize(ExecutorConfig{})) {
                std::printf("initialize failed\n");
                return 1;
            }
            auto payload = std::make_shared<std::string>("heap-payload");
            auto sub = ex.submit_with_handle([payload] {
                std::this_thread::sleep_for(300ms);
                std::printf("[task] ran, payload=%s\n", payload->c_str());
                std::fflush(stdout);
            });
            (void)sub;
            std::printf("[main] instance mode, leaving scope (shutdown in dtor)\n");
            std::fflush(stdout);
        }  // ~Executor → shutdown(true)：等待在途任务
        std::printf("[main] instance mode finished cleanly\n");
        return 0;
    }

    std::printf("unknown mode %s\n", mode);
    return 2;
}
