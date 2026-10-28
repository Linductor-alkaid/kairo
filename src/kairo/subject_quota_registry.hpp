// 分主体在途份额注册表（分桶 admission，issue #222 /
// docs/design/subject_bucket_admission.md）。
//
// 形态为 mutex + unordered_map（与 TaskInferenceTable 同族）。键空间以
// 已配置集合为界：只有经 set_subject_max_in_flight_tasks() 显式配置的
// 主体才产生表项（"未知即宽容"——未配置主体仅受全局约束），因此不存在
// 无界增长。set(0) 解除即 erase 表项；每个键的在途计数经 shared_ptr
// 持有：接纳点在注册表锁外 fetch_add，AdmissionReleaser 以共享所有权
// keepalive，保证解除键后仍在途的任务释放时安全落账（计数随后不可查询，
// 语义见设计文档 D-5）。
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "kairo/types.hpp"

namespace kairo::detail {

class SubjectQuotaRegistry {
public:
    SubjectQuotaRegistry() = default;

    SubjectQuotaRegistry(const SubjectQuotaRegistry&) = delete;
    SubjectQuotaRegistry& operator=(const SubjectQuotaRegistry&) = delete;

    // 查表：命中时输出份额上限并返回计数共享所有权（接纳点据此在锁外
    // fetch_add，AdmissionReleaser keepalive）；未配置返回 null。
    std::shared_ptr<std::atomic<int64_t>> find(
        const std::string& subject, int64_t& max_out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(subject);
        if (it == entries_.end()) {
            return nullptr;
        }
        max_out = it->second.max_in_flight;
        return it->second.counter;
    }

    // 配置份额；max == 0 解除（erase 表项）。重复配置保留既有计数
    // （调小不驱逐已接纳任务，只约束后续提交），新配置的键从 0 起计。
    void set(const std::string& subject, size_t max) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (max == 0) {
            entries_.erase(subject);
            return;
        }
        Entry& entry = entries_[subject];
        entry.max_in_flight = static_cast<int64_t>(max);
        if (!entry.counter) {
            entry.counter = std::make_shared<std::atomic<int64_t>>(0);
        }
    }

    // 快照：按 subject 字典序返回（诊断面，弱一致）。
    std::vector<SubjectQuotaStatus> status() const {
        std::vector<SubjectQuotaStatus> snapshot;
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot.reserve(entries_.size());
        for (const auto& [subject, entry] : entries_) {
            SubjectQuotaStatus status;
            status.subject = subject;
            status.max_in_flight_tasks =
                static_cast<size_t>(entry.max_in_flight);
            const int64_t current =
                entry.counter->load(std::memory_order_relaxed);
            status.in_flight_submissions =
                current > 0 ? static_cast<size_t>(current) : 0;
            snapshot.push_back(std::move(status));
        }
        std::sort(snapshot.begin(), snapshot.end(),
                  [](const SubjectQuotaStatus& a, const SubjectQuotaStatus& b) {
                      return a.subject < b.subject;
                  });
        return snapshot;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.empty();
    }

private:
    struct Entry {
        int64_t max_in_flight = 0;
        std::shared_ptr<std::atomic<int64_t>> counter;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace kairo::detail
