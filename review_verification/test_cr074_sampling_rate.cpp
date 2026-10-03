// CR-074: set_sampling_rate(-1.0) triggers double->uint32 UB per [conv.fpint].
// On x86-64 (cvttsd2si) the negative double truncates to a negative integer
// whose uint32 reinterpretation is a huge value, which is then clamped to 100
// => negative rates silently become 100% sampling. NaN likewise converts to a
// huge value and clamps to 100%. Test observes the stored rate AND the
// behavioral effect on record_task_start sampling.
#include "kairo/monitor/task_monitor.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>

using kairo::monitor::TaskMonitor;

static int count_sampled(TaskMonitor& m, int n, const char* tag) {
    int sampled = 0;
    for (int i = 0; i < n; ++i) {
        const std::string id = std::string(tag) + std::to_string(i);
        m.record_task_start(id, "probe");
        m.record_task_complete(id, true, 1);
    }
    return static_cast<int>(m.get_statistics("probe").total_count);
}

int main() {
    // Reference: what the C-style conversion actually produces on this ABI.
    const double neg = -1.0 * 100.0;
    const double nan_val = std::nan("");
    std::printf("pre-check: (uint32_t)(-100.0) via int cast chain = %u\n",
                static_cast<uint32_t>(static_cast<long long>(neg))); // defined reference only

    {
        TaskMonitor m;
        m.set_sampling_rate(-1.0);
        const double reported = m.get_sampling_rate();
        const int sampled = count_sampled(m, 1000, "neg_");
        std::printf("NEG -1.0: get_sampling_rate()=%.2f sampled=%d/1000 (%.1f%%)\n",
                    reported, sampled, 100.0 * sampled / 1000.0);
        if (reported > 1.0 || reported < 0.0) {
            std::printf("NEG: reported rate out of [0,1] contract: %.2f\n", reported);
        }
        if (sampled == 1000) {
            std::printf("NEG: -1.0 became 100%% sampling -> CONFIRMED behavior\n");
        }
    }
    {
        TaskMonitor m;
        m.set_sampling_rate(std::nan(""));
        const double reported = m.get_sampling_rate();
        const int sampled = count_sampled(m, 1000, "nan_");
        std::printf("NaN: get_sampling_rate()=%.2f sampled=%d/1000 (%.1f%%)\n",
                    reported, sampled, 100.0 * sampled / 1000.0);
        if (sampled == 1000) {
            std::printf("NaN: NaN became 100%% sampling\n");
        }
    }
    {
        TaskMonitor m;
        m.set_sampling_rate(1e300);
        const double reported = m.get_sampling_rate();
        const int sampled = count_sampled(m, 1000, "huge_");
        std::printf("HUGE 1e300: get_sampling_rate()=%.2f sampled=%d/1000\n",
                    reported, sampled);
    }
    {
        // Same conversion path, in-flight sampling (task_monitor.cpp:272).
        TaskMonitor m;
        m.set_in_flight_sampling_rate(-1.0);
        const double reported = m.get_in_flight_sampling_rate();
        std::printf("IN-FLIGHT NEG -1.0: get_in_flight_sampling_rate()=%.2f\n", reported);
    }
    return 0;
}
