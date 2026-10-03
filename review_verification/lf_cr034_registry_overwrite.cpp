// CR-034: TaskCancellationRegistry::register_state silently overwrites a
// duplicate task_id (task_cancellation.hpp:324 `active_[task_id] = state`).
//
// Minimal reproduction: register two different states under the same id,
// then run the facade-equivalent cancel path (find -> try_cancel_before_start
// -> notify_cancelled) and observe that state A is never reached.
//
// Expected if the finding is real:
//   - second register_state returns true (no rejection), active_size stays 1
//   - find("id1") returns state B
//   - cancel via found state fires B's sink; A's sink never fires, A stays
//     Pending and can still begin execution.

#include "kairo/task_cancellation.hpp"

#include <atomic>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

using namespace kairo;

static const char* lookup_str(TaskCancellationRegistry::LookupResult r) {
    switch (r) {
    case TaskCancellationRegistry::LookupResult::Active: return "Active";
    case TaskCancellationRegistry::LookupResult::Terminal: return "Terminal";
    default: return "NotFound";
    }
}

int main() {
    TaskCancellationRegistry reg;

    auto state_a = std::make_shared<TaskCancellationState>();
    auto state_b = std::make_shared<TaskCancellationState>();

    std::atomic<bool> a_sink_fired{false};
    std::atomic<bool> b_sink_fired{false};
    state_a->set_completion_sink([&](std::exception_ptr) { a_sink_fired = true; });
    state_b->set_completion_sink([&](std::exception_ptr) { b_sink_fired = true; });

    const bool ok1 = reg.register_state("id1", state_a);
    const bool ok2 = reg.register_state("id1", state_b);
    std::cout << "register_state(\"id1\", A) -> " << ok1 << "\n";
    std::cout << "register_state(\"id1\", B) -> " << ok2
              << " (active_size=" << reg.active_size() << ")\n";

    std::shared_ptr<TaskCancellationState> found;
    const auto lr = reg.find("id1", found);
    std::cout << "find(\"id1\") -> " << lookup_str(lr)
              << ", found==A: " << (found == state_a)
              << ", found==B: " << (found == state_b) << "\n";

    // Facade-equivalent cancel on whatever find() returned.
    bool cancel_won = false;
    if (found && found->try_cancel_before_start()) {
        cancel_won = true;
        found->notify_cancelled(std::make_exception_ptr(
            TaskCancelled(TaskCancellationReason::Explicit, "cr034")));
    }
    std::cout << "cancel via found state: won=" << cancel_won
              << ", B sink fired=" << b_sink_fired.load() << "\n";

    // Read A's phase BEFORE the probe: the probe itself legitimately
    // transitions A Pending -> Running, which is exactly the point (the
    // cancel on "id1" never touched A).
    const auto a_phase = state_a->phase();
    const bool a_can_still_begin = state_a->try_begin_execution();
    std::cout << "A: phase=" << static_cast<int>(a_phase)  // 0 == Pending
              << ", sink fired=" << a_sink_fired.load()
              << ", cancel_requested=" << state_a->cancel_requested()
              << ", try_begin_execution=" << a_can_still_begin << "\n";

    bool confirmed = ok1 && ok2 && reg.active_size() == 1 &&
                     found == state_b && cancel_won && b_sink_fired &&
                     !a_sink_fired &&
                     a_phase == TaskCancellationState::Phase::Pending &&
                     a_can_still_begin;
    std::cout << (confirmed
                      ? "CR-034 VERDICT: CONFIRMED - duplicate id silently "
                        "overwritten; A unreachable via registry and its "
                        "cancel/completion never triggers"
                      : "CR-034 VERDICT: NOT CONFIRMED")
              << "\n";
    return confirmed ? 0 : 1;
}
