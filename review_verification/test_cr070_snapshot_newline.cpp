// CR-070: snapshot formatter missing '\n' after realtime timer_slack_applied.
// Builds an ExecutorSnapshot with a RealtimeExecutorStatus entry whose boolean
// fields are all true, formats it, and scans for lines where a bool value is
// immediately followed by the next key (regex-ish check: "=(true|false)[A-Za-z_]").
#include "kairo/monitor/executor_snapshot_formatter.hpp"
#include "kairo/types.hpp"

#include <iostream>
#include <regex>
#include <string>
#include <vector>

using namespace kairo;
using namespace kairo::monitor;

int main() {
    ExecutorSnapshot snapshot;
    snapshot.lifecycle = ExecutorLifecycleState::Running;
    snapshot.running_backend_count = 1;

    RealtimeExecutorStatus rt;
    rt.name = "rt1";
    rt.is_running = true;
    rt.priority_applied = true;
    rt.cpu_affinity_applied = true;
    rt.memory_locked = true;
    rt.timer_slack_applied = true;
    rt.dropped_task_count = 3;
    snapshot.realtime[rt.name] = rt;

    const std::string text = format_executor_snapshot(snapshot);
    std::cout << "===== FULL FORMATTED OUTPUT =====\n";
    std::cout << text;
    std::cout << "===== END OUTPUT =====\n";

    const std::regex malformed_re("=(true|false)[A-Za-z_]");
    std::vector<std::string> bad_lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        if (std::regex_search(line, malformed_re)) {
            bad_lines.push_back(line);
        }
        pos = nl + 1;
    }

    std::cout << "malformed lines: " << bad_lines.size() << "\n";
    for (const auto& l : bad_lines) {
        std::cout << "  MALFORMED: [" << l << "]\n";
    }
    return bad_lines.empty() ? 0 : 1;
}
