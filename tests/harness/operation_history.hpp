#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace kairo::test::harness {

struct Operation {
    enum class Type {
        Send,
        Recv,
        Publish,
        Read
    };

    std::uint64_t thread_id{0};
    std::uint64_t op_id{0};
    Type type{Type::Send};
    std::uint64_t value{0};
    bool success{false};
    std::chrono::steady_clock::time_point start{};
    std::chrono::steady_clock::time_point end{};
};

struct InvariantResult {
    bool ok{true};
    std::vector<std::string> failures{};

    void fail(std::string message) {
        ok = false;
        failures.push_back(std::move(message));
    }
};

class OperationHistory {
public:
    void record(Operation operation) {
        std::lock_guard<std::mutex> lock(mutex_);
        operations_.push_back(std::move(operation));
    }

    void record_now(std::uint64_t thread_id,
                    std::uint64_t op_id,
                    Operation::Type type,
                    std::uint64_t value,
                    bool success) {
        const auto now = std::chrono::steady_clock::now();
        record(Operation{
            .thread_id = thread_id,
            .op_id = op_id,
            .type = type,
            .value = value,
            .success = success,
            .start = now,
            .end = now,
        });
    }

    [[nodiscard]] std::vector<Operation> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return operations_;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return operations_.size();
    }

    [[nodiscard]] std::size_t count(Operation::Type type) const {
        const auto copy = snapshot();
        std::size_t total = 0;
        for (const Operation& operation : copy) {
            if (operation.type == type) {
                ++total;
            }
        }
        return total;
    }

    [[nodiscard]] std::size_t count_successful(Operation::Type type) const {
        const auto copy = snapshot();
        std::size_t total = 0;
        for (const Operation& operation : copy) {
            if (operation.type == type && operation.success) {
                ++total;
            }
        }
        return total;
    }

    [[nodiscard]] InvariantResult check_no_duplicate_successful_receives() const {
        const auto copy = snapshot();
        InvariantResult result;
        std::set<std::uint64_t> received_values;

        for (const Operation& operation : copy) {
            if (operation.type != Operation::Type::Recv || !operation.success) {
                continue;
            }
            if (!received_values.insert(operation.value).second) {
                result.fail("duplicate successful receive for value " + std::to_string(operation.value));
            }
        }

        return result;
    }

    [[nodiscard]] InvariantResult check_no_phantom_successful_receives() const {
        const auto copy = snapshot();
        InvariantResult result;
        std::set<std::uint64_t> sent_values;

        for (const Operation& operation : copy) {
            if (operation.type == Operation::Type::Send && operation.success) {
                sent_values.insert(operation.value);
            }
        }

        for (const Operation& operation : copy) {
            if (operation.type != Operation::Type::Recv || !operation.success) {
                continue;
            }
            if (sent_values.find(operation.value) == sent_values.end()) {
                result.fail("phantom successful receive for value " + std::to_string(operation.value));
            }
        }

        return result;
    }

    [[nodiscard]] InvariantResult check_unique_operation_ids() const {
        const auto copy = snapshot();
        InvariantResult result;
        std::set<std::pair<std::uint64_t, std::uint64_t>> seen;

        for (const Operation& operation : copy) {
            const auto key = std::make_pair(operation.thread_id, operation.op_id);
            if (!seen.insert(key).second) {
                result.fail("duplicate operation id thread=" + std::to_string(operation.thread_id) +
                            " op=" + std::to_string(operation.op_id));
            }
        }

        return result;
    }

    [[nodiscard]] InvariantResult check_time_ranges() const {
        const auto copy = snapshot();
        InvariantResult result;

        for (const Operation& operation : copy) {
            if (operation.end < operation.start) {
                result.fail("operation end before start thread=" + std::to_string(operation.thread_id) +
                            " op=" + std::to_string(operation.op_id));
            }
        }

        return result;
    }

    [[nodiscard]] InvariantResult check_basic_channel_invariants() const {
        InvariantResult result = check_no_duplicate_successful_receives();
        InvariantResult no_phantom = check_no_phantom_successful_receives();
        if (!no_phantom.ok) {
            result.ok = false;
            result.failures.insert(result.failures.end(), no_phantom.failures.begin(), no_phantom.failures.end());
        }
        InvariantResult unique_ops = check_unique_operation_ids();
        if (!unique_ops.ok) {
            result.ok = false;
            result.failures.insert(result.failures.end(), unique_ops.failures.begin(), unique_ops.failures.end());
        }
        InvariantResult time_ranges = check_time_ranges();
        if (!time_ranges.ok) {
            result.ok = false;
            result.failures.insert(result.failures.end(), time_ranges.failures.begin(), time_ranges.failures.end());
        }
        return result;
    }

    [[nodiscard]] static const char* type_name(Operation::Type type) {
        switch (type) {
            case Operation::Type::Send:
                return "send";
            case Operation::Type::Recv:
                return "recv";
            case Operation::Type::Publish:
                return "publish";
            case Operation::Type::Read:
                return "read";
            default:
                return "unknown";
        }
    }

private:
    mutable std::mutex mutex_;
    std::vector<Operation> operations_;
};

} // namespace kairo::test::harness
