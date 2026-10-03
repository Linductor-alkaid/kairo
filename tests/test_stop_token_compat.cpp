#include <atomic>
#include <iostream>
#include <thread>
#include <type_traits>

#include <kairo/stop_token.hpp>

#if defined(KAIRO_STOP_TOKEN_FORCE_FALLBACK)
static_assert(!std::is_same_v<kairo::StopToken, std::stop_token>,
              "forced fallback must instantiate executor's own StopToken implementation");
#elif !defined(__ANDROID__) || defined(__cpp_lib_jthread)
static_assert(std::is_same_v<kairo::StopToken, std::stop_token>,
              "kairo::StopToken must alias std::stop_token on desktop/standard-lib path");
#endif

#define TEST_ASSERT(condition, message)                                      \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << "FAILED: " << message << " at " << __FILE__      \
                      << ':' << __LINE__ << '\n';                          \
            return false;                                                    \
        }                                                                    \
    } while (0)

bool test_stop_source_request_once() {
    kairo::StopSource source;
    const kairo::StopToken token = source.get_token();

    TEST_ASSERT(token.stop_possible(), "stop token must be possible from a source");
    TEST_ASSERT(!token.stop_requested(), "stop token must not be requested initially");
    TEST_ASSERT(source.request_stop(), "first request_stop must report a new request");
    TEST_ASSERT(token.stop_requested(), "stop token must observe request_stop");
    TEST_ASSERT(!source.request_stop(), "repeated request_stop must report no new request");
    return true;
}

bool test_jthread_passes_stop_token_and_joins() {
    std::atomic<bool> entered{false};
    std::atomic<bool> left{false};

    kairo::detail::JThread thread([&](kairo::StopToken token) {
        entered.store(true, std::memory_order_release);
        while (!token.stop_requested()) {
            std::this_thread::yield();
        }
        left.store(true, std::memory_order_release);
    });

    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    TEST_ASSERT(thread.joinable(), "jthread must be joinable while running");
    TEST_ASSERT(thread.request_stop(), "jthread request_stop must report a new request");
    thread.join();
    TEST_ASSERT(left.load(std::memory_order_acquire), "jthread entry must observe stop token");
    TEST_ASSERT(!thread.joinable(), "jthread must not be joinable after join");
    return true;
}

bool test_jthread_destructor_requests_stop_and_joins() {
    std::atomic<bool> entered{false};
    std::atomic<bool> left{false};

    {
        kairo::detail::JThread thread([&](kairo::StopToken token) {
            entered.store(true, std::memory_order_release);
            while (!token.stop_requested()) {
                std::this_thread::yield();
            }
            left.store(true, std::memory_order_release);
        });

        while (!entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        TEST_ASSERT(thread.joinable(), "jthread must be joinable before destruction");
    }

    TEST_ASSERT(left.load(std::memory_order_acquire),
                "jthread destructor must request stop and join its entry");
    return true;
}

bool test_jthread_move_constructor_does_not_double_join() {
    std::atomic<bool> entered{false};
    std::atomic<bool> left{false};

    {
        kairo::detail::JThread source([&](kairo::StopToken token) {
            entered.store(true, std::memory_order_release);
            while (!token.stop_requested()) {
                std::this_thread::yield();
            }
            left.store(true, std::memory_order_release);
        });

        while (!entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        kairo::detail::JThread target(std::move(source));
        TEST_ASSERT(!source.joinable(), "moved-from jthread must not be joinable");
        TEST_ASSERT(target.joinable(), "move-constructed jthread must own the thread");
        TEST_ASSERT(target.request_stop(), "target jthread must expose its stop source");
        target.join();
    }

    TEST_ASSERT(left.load(std::memory_order_acquire),
                "move-constructed jthread entry must observe stop token");
    return true;
}

bool test_jthread_move_assignment_joins_previous_thread() {
    std::atomic<bool> first_entered{false};
    std::atomic<bool> first_left{false};
    std::atomic<bool> second_entered{false};
    std::atomic<bool> second_left{false};

    {
        kairo::detail::JThread target([&](kairo::StopToken token) {
            first_entered.store(true, std::memory_order_release);
            while (!token.stop_requested()) {
                std::this_thread::yield();
            }
            first_left.store(true, std::memory_order_release);
        });

        while (!first_entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        kairo::detail::JThread source([&](kairo::StopToken token) {
            second_entered.store(true, std::memory_order_release);
            while (!token.stop_requested()) {
                std::this_thread::yield();
            }
            second_left.store(true, std::memory_order_release);
        });

        while (!second_entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        target = std::move(source);
        TEST_ASSERT(first_left.load(std::memory_order_acquire),
                    "move assignment must stop and join the previous thread first");
        TEST_ASSERT(!source.joinable(), "moved-from jthread must not be joinable");
        TEST_ASSERT(target.joinable(), "move-assigned jthread must own the new thread");
    }

    TEST_ASSERT(second_left.load(std::memory_order_acquire),
                "destructor must stop and join the move-assigned thread");
    return true;
}

int main() {
    bool ok = true;
    ok &= test_stop_source_request_once();
    ok &= test_jthread_passes_stop_token_and_joins();
    ok &= test_jthread_destructor_requests_stop_and_joins();
    ok &= test_jthread_move_constructor_does_not_double_join();
    ok &= test_jthread_move_assignment_joins_previous_thread();
    std::cout << (ok ? "All stop_token compatibility tests PASSED\n"
                     : "stop_token compatibility tests FAILED\n");
    return ok ? 0 : 1;
}
