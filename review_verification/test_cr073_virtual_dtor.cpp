// CR-073: TaskMonitor has virtual functions but a non-virtual destructor.
// Deleting a Derived through TaskMonitor* is UB. Compile this TU with
//   g++ -std=c++20 -Wall -Wextra -Wdelete-non-virtual-dtor -c
// and capture the compiler diagnostic as evidence.
#include "kairo/monitor/task_monitor.hpp"

using kairo::monitor::TaskMonitor;

namespace {
class DerivedMonitor : public TaskMonitor {
public:
    void record_task_start(const std::string& task_id,
                           const std::string& task_type) override {
        ++starts_;
        (void)task_id;
        (void)task_type;
    }
    void record_task_complete(const std::string& task_id,
                              bool success,
                              int64_t execution_time_ns) override {
        ++completes_;
        (void)task_id;
        (void)success;
        (void)execution_time_ns;
    }
    int starts_ = 0;
    int completes_ = 0;
};
} // namespace

int main() {
    TaskMonitor* p = new DerivedMonitor();
    p->record_task_start("t1", "test");
    p->record_task_complete("t1", true, 100);
    delete p; // non-virtual dtor: Derived part never destroyed (UB)
    return 0;
}
