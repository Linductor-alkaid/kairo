// smoke: 验证链接与基础 API 用法（submit_with_handle / submit_on / cancel / snapshot）
#include <kairo/executor.hpp>
#include <kairo/serial_execution_context.hpp>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace kairo;
using namespace std::chrono_literals;

int main() {
    Executor ex;
    if (!ex.initialize(ExecutorConfig{})) {
        std::printf("initialize failed\n");
        return 1;
    }
    auto sub = ex.submit_with_handle([] { return 42; });
    if (sub.future.wait_for(5s) != std::future_status::ready) {
        std::printf("submit_with_handle future timeout\n");
        return 1;
    }
    std::printf("submit_with_handle result=%d\n", sub.future.get());

    SerialExecutionContext ctx;
    auto ssub = ex.submit_on_with_handle(ctx, [] { return 7; });
    if (ssub.future.wait_for(5s) != std::future_status::ready) {
        std::printf("submit_on future timeout\n");
        return 1;
    }
    std::printf("submit_on result=%d\n", ssub.future.get());

    auto blocked = ex.submit_with_handle([] {
        std::this_thread::sleep_for(50ms);
        return 1;
    });
    auto resp = ex.request_task_cancel(blocked.handle);
    std::printf("cancel resp=%d\n", static_cast<int>(resp.result));

    auto snap = ex.get_snapshot();
    std::printf("snapshot in_flight=%zu dropped=%zu cancel_req=%llu\n",
                snap.in_flight_count, snap.in_flight_dropped_count,
                static_cast<unsigned long long>(snap.cancellation.request_count));
    auto st = ex.get_async_executor_status();
    std::printf("async status name=%s running=%d\n", st.name.c_str(),
                static_cast<int>(st.is_running));
    (void)ex.shutdown(true);
    std::printf("SMOKE OK\n");
    return 0;
}
