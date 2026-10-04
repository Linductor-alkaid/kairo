// CR-030 探针 2：直接调用库的 set_thread_priority，观察返回值与各线程 nice。
#include "kairo/util/thread_utils.hpp"
#include <sys/resource.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <thread>

static int own_nice() { errno = 0; int v = getpriority(PRIO_PROCESS, 0); return errno ? -999 : v; }

int main() {
    std::printf("main initial nice=%d\n", own_nice());
    {
        std::thread worker([] { std::this_thread::sleep_for(std::chrono::seconds(1)); });
        errno = 0;
        const bool r = kairo::util::set_thread_priority(worker.native_handle(), 19);
        std::printf("set_thread_priority(worker, 19) = %d ; main nice now=%d\n", r, own_nice());
        worker.join();
    }
    {
        std::thread worker([] {});
        errno = 0;
        const bool r = kairo::util::set_thread_priority(worker.native_handle(), 20);
        std::printf("set_thread_priority(worker, 20) = %d ; main nice now=%d\n", r, own_nice());
        worker.join();
    }
    return 0;
}
