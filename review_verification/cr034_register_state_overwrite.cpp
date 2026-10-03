// CR-034: TaskCancellationRegistry::register_state 对重复 task_id 静默覆盖。
// 纯头文件单测，无需链接库。
//
// 预期（若缺陷成立）：register_state("id1", A) 后再次 register_state("id1", B)
// 返回 true 且静默覆盖，registry 再也无法找到 A；随后经 registry 的取消只
// 触达 B，A 永远收不到取消。

#include "kairo/task_cancellation.hpp"

#include <iostream>
#include <memory>
#include <string>

using namespace kairo;

static const char* lookup_str(TaskCancellationRegistry::LookupResult r) {
    switch (r) {
    case TaskCancellationRegistry::LookupResult::Active:    return "Active";
    case TaskCancellationRegistry::LookupResult::Terminal:  return "Terminal";
    default:                                                return "NotFound";
    }
}

int main() {
    TaskCancellationRegistry reg;
    auto stateA = std::make_shared<TaskCancellationState>();
    auto stateB = std::make_shared<TaskCancellationState>();

    const bool ok1 = reg.register_state("id1", stateA);
    const bool ok2 = reg.register_state("id1", stateB);  // 重复 id

    std::cout << "register_state(id1, A) -> " << ok1 << "\n";
    std::cout << "register_state(id1, B) [duplicate id] -> " << ok2
              << " (no error signal, active_size=" << reg.active_size() << ")\n";

    std::shared_ptr<TaskCancellationState> found;
    const auto lr = reg.find("id1", found);
    std::cout << "find(id1) -> " << lookup_str(lr)
              << " ; found == A ? " << (found == stateA)
              << " ; found == B ? " << (found == stateB) << "\n";

    // 经 registry 的正常取消路径（facade 语义：find 命中后 CAS 取消）
    const bool cancelled = found && found->try_cancel_before_start();
    std::cout << "cancel on registry-resolved state -> " << cancelled
              << " ; B.phase = " << int(stateB->phase())
              << " (Cancelled=" << int(TaskCancellationState::Phase::Cancelled) << ")\n";

    std::cout << "state A: cancel_requested=" << stateA->cancel_requested()
              << " ; A.phase = " << int(stateA->phase())
              << " (Pending=" << int(TaskCancellationState::Phase::Pending) << ")\n";

    const bool overwritten = (found == stateB) && (found != stateA);
    const bool a_never_cancelled =
        stateA->phase() == TaskCancellationState::Phase::Pending &&
        !stateA->cancel_requested();

    std::cout << "---- verdict ----\n";
    if (overwritten && a_never_cancelled) {
        std::cout << "CR-034 CONFIRMED: duplicate register_state silently "
                     "overwrote A; registry can no longer reach A and A never "
                     "receives cancellation (behavior confirmation; whether it "
                     "is a defect depends on facade contract — facade "
                     "generate_task_id() uses an atomic counter so ids are "
                     "unique in practice).\n";
        return 0;
    }
    std::cout << "CR-034 NOT CONFIRMED: behavior differs from claim.\n";
    return 1;
}
