// CR-042 verification: RealtimeChannel::drain_for_cycle treats
// max_items_per_cycle == 0 as UNLIMITED.
//
// Expected behavior under review claim: with RealtimeChannelOptions::max_items_per_cycle
// set to 0, one drain_for_cycle call drains the entire queue (5000 items here),
// instead of 0 or a small default budget.
// Contrasts: default options (max_items_per_cycle=64) drain at most 64 per call;
// explicit drain_for_cycle(h, 0) falls back to the option value, not unlimited.
#include <kairo/comm/mailbox.hpp>

#include <cstdio>

using kairo::comm::RealtimeChannel;
using kairo::comm::RealtimeChannelOptions;

int main() {
    int result = 0;

    // 1) option max_items_per_cycle = 0  => unlimited?
    {
        RealtimeChannelOptions opts;
        opts.capacity = 8192;
        opts.max_items_per_cycle = 0;
        opts.name = "cr042-unlimited";
        RealtimeChannel<int> ch(opts);
        for (int i = 0; i < 5000; ++i) {
            if (!ch.try_send(i)) {
                std::printf("UNEXPECTED: send %d failed\n", i);
                return 2;
            }
        }
        const size_t drained = ch.drain_for_cycle([](int&) {});
        std::printf("max_items_per_cycle=0, queued=5000: one drain_for_cycle() drained %zu items, remaining=%zu\n",
                    drained, ch.size_approx());
        std::printf("verdict: %s (drained %s)\n",
                    drained == 5000 ? "CONFIRMED - 0 means unlimited"
                                    : "NOT REPRODUCED",
                    drained == 5000 ? "ALL" : "NOT all");
        if (drained != 5000) result = 1;
    }

    // 2) default options (max_items_per_cycle=64): one call drains 64
    {
        RealtimeChannel<int> ch;  // defaults
        for (int i = 0; i < 5000; ++i) (void)ch.try_send(i);
        const size_t drained = ch.drain_for_cycle([](int&) {});
        std::printf("default option (=64), queued=5000: one drain drained %zu, remaining=%zu (contrast)\n",
                    drained, ch.size_approx());
    }

    // 3) explicit max_items=0 argument on default channel: option fallback, not unlimited
    {
        RealtimeChannel<int> ch;
        for (int i = 0; i < 5000; ++i) (void)ch.try_send(i);
        const size_t drained = ch.drain_for_cycle([](int&) {}, 0);
        std::printf("explicit arg 0 on default channel: drained %zu (explicit 0 falls back to option value 64, NOT unlimited)\n",
                    drained);
    }

    // 4) explicit budget 10
    {
        RealtimeChannel<int> ch;
        for (int i = 0; i < 100; ++i) (void)ch.try_send(i);
        const size_t drained = ch.drain_for_cycle([](int&) {}, 10);
        std::printf("explicit arg 10: drained %zu (contrast)\n", drained);
    }

    return result;
}
