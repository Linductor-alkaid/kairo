#include <iostream>

#include <kairo/blocking_io.hpp>

using namespace kairo;

int main() {
    BlockingIoConfig config;
    BlockingIoExecutorStatus status;

    if (!config.thread_name.empty() || config.enable_memory_lock ||
        config.startup_timeout.count() != 1000 || status.is_running || status.ready ||
        status.stop_reason != BlockingIoStopReason::None || status.wakeup_count != 0) {
        std::cerr << "Blocking I/O public type defaults are invalid\n";
        return 1;
    }

    std::cout << "Blocking I/O public type defaults: PASSED\n";
    return 0;
}
