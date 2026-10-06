#pragma once

#include "../kairo/util/cpu_pause.hpp"

#include <cstddef>
#include <thread>
#include <cstdint>
#include <atomic>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>

namespace kairo {
namespace util {

/**
 * @brief Object pool for task allocation (PA-5: lock-free)
 *
 * Pre-allocates objects and hands them out via a lock-free index free-list
 * (tagged Treiber stack). acquire()/release() are wait-free bounded CAS
 * loops: the LockFreeTaskExecutor submit path and the realtime worker's
 * per-task release no longer serialize on a mutex, which also removes the
 * priority-inversion window where an RT thread blocked in release() behind
 * a preempted producer.
 *
 * All nodes live in a single contiguous array (owned by one unique_ptr) so
 * that release() can recover a node from its data pointer in O(1) via
 * pointer arithmetic instead of scanning storage_. Contiguity is essential:
 * the index of a node is just (byte offset from the first node) / sizeof(Node),
 * which only holds when nodes are equally spaced in one allocation.
 *
 * Concurrency protocol:
 *  - head_ packs (tag:32 | index:32) into one 64-bit word. index is the
 *    top of the free-list (kNilIndex when empty); tag is an ABA counter
 *    bumped by every successful release. A head value therefore can never
 *    repeat, so an acquire() that stalls between loading head_ and its CAS
 *    cannot splice a stale free-list tail onto a recycled node.
 *  - node->next is only meaningful while the node is on the free-list. It
 *    is written before the releasing head_ CAS (program order) and read by
 *    the acquirer only after an acquire-load of a head_ value that still
 *    passes the CAS, so the write is visible (happens-before via the
 *    release CAS on head_).
 *  - node->state distinguishes allocated/free purely for double-release
 *    detection: release() CASes it 1→0 and treats failure as a double
 *    release; acquire() stores 1 after winning head_. This mirrors the
 *    detection semantics of the previous mutex implementation (a double
 *    release after re-acquire is still undetectable, exactly as before).
 *
 * Tag wrap-around: the tag repeats only after 2^32 releases *of the same
 * head value race window*, which requires a stalled acquirer to survive
 * 4 billion intervening pushes — unreachable in practice and the standard
 * trade-off of tagged pointer free-lists.
 */
template<typename T>
class ObjectPool {
public:
    explicit ObjectPool(size_t capacity = 1024) : capacity_(capacity) {
        // Reject zero capacity: the free-list construction below would
        // underflow (`capacity - 1` wraps to SIZE_MAX) and dereference
        // `storage_[0]` / `storage_[SIZE_MAX]`, triggering UB and very
        // likely a segfault before acquire()/release() are ever called.
        if (capacity == 0) {
            throw std::invalid_argument("ObjectPool capacity must be > 0");
        }
        // The free-list encodes node indexes as uint32_t with one sentinel
        // reserved, so capacities beyond that cannot be addressed.
        if (capacity > kMaxCapacity) {
            throw std::invalid_argument(
                "ObjectPool capacity must be <= " + std::to_string(kMaxCapacity));
        }

        // Allocate every node in ONE contiguous array. Contiguity is what
        // makes release() O(1): a node's index is recovered from its data
        // pointer by pure pointer arithmetic. A vector of individually
        // heap-allocated nodes would scatter them across the heap and force
        // release() back into an O(n) scan.
        storage_ = std::make_unique<Node[]>(capacity);

        // Every node starts on the free list, chained in index order.
        for (size_t i = 0; i < capacity; ++i) {
            storage_[i].state.store(kStateFree, std::memory_order_relaxed);
            storage_[i].next.store(
                static_cast<uint32_t>(i + 1 < capacity ? i + 1 : kNilIndex),
                std::memory_order_relaxed);
        }
        head_.store(pack(0, 0), std::memory_order_relaxed);
    }

    /**
     * @brief Acquire an object from the pool
     * @return Pointer to object, or nullptr if pool is exhausted
     */
    T* acquire() {
        uint64_t head = head_.load(std::memory_order_acquire);
        // 指数退避序列与原实现一致：1, 2, 4, ... 封顶 32。
        PauseBackoff backoff{1, 32};
        while (true) {
            const uint32_t index = index_of(head);
            if (index == kNilIndex) {
                return nullptr;
            }
            Node* node = &storage_[index];
            const uint32_t next = node->next.load(std::memory_order_relaxed);
            const uint64_t new_head = (head & kTagMask) | next;
            if (head_.compare_exchange_weak(
                    head, new_head,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                node->state.store(kStateAllocated, std::memory_order_relaxed);
                return &node->data;
            }
            // 失败后指数 PAUSE 退避（失败 CAS 是对热行的独占 RFO，立即
            // 重试会叠加一致性流量）。退避后【必须重读 head_ 刷新期望值】：
            // 退避时长（封顶 ~1µs）内在持续争用下 head_ 已被推进数十次，
            // 带陈旧期望值重试是确定性失败——偶发参与者（如 RT 线程）
            // 会因此饿死秒级（实测 20s+）。重读是共享读，不独占缓存行。
            backoff.pause();
            head = head_.load(std::memory_order_acquire);
        }
    }

    /**
     * @brief Release an object back to the pool
     * @param obj Pointer to object to release
     *
     * O(1) in capacity: the node index is recovered from the data pointer by
     * pointer arithmetic on the contiguous node array (data is the first member
     * of Node, so consecutive nodes are exactly sizeof(Node) apart), and
     * double-release is caught with a per-node state CAS instead of a scan.
     */
    void release(T* obj) {
        release_bulk(&obj, 1);
    }

    /**
     * @brief Release N objects with ONE free-list splice
     *
     * Same validation and double-release detection as release(), but the
     * whole batch enters the free-list with a single head CAS: a consumer
     * draining a batch of tasks pays one head_ round-trip instead of N.
     * head_ is the only word that producers (acquire) and the consumer
     * (release) both RMW, so batching the consumer side keeps the lock-free
     * pool competitive with the retired mutex pool at low producer counts,
     * where round-trip latency rather than lock serialization dominates.
     *
     * objs[i] == nullptr entries are skipped; the array may be reordered.
     */
    void release_bulk(T** objs, size_t n) {
        if (n == 0) return;

        // Validate pointers and flip per-node state first. These touch only
        // per-node lines, never the shared head_ line. A validated objs[i]
        // numerically equals its Node* (data is Node's first member), so the
        // array doubles as the chain after reinterpret below.
        size_t valid = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!objs[i]) continue;
            Node* node = validated_node(objs[i]);
            uint32_t state = kStateAllocated;
            if (!node->state.compare_exchange_strong(
                    state, kStateFree,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                throw std::logic_error(
                    std::string("ObjectPool: double release of node ")
                    + std::to_string(reinterpret_cast<std::uintptr_t>(node)));
            }
            objs[valid++] = reinterpret_cast<T*>(node);
        }
        if (valid == 0) return;

        // Splice the whole chain under the final CAS:
        // node(objs[0]) -> ... -> node(objs[valid-1]) -> old head. The tag
        // bumps once, so a stalled acquire() holding the previous head value
        // cannot succeed after any part of this splice. The intra-chain
        // links are call-constant and written once; only the tail link and
        // the expected head change per retry.
        Node** nodes = reinterpret_cast<Node**>(objs);
        for (size_t i = 0; i + 1 < valid; ++i) {
            nodes[i]->next.store(index_of_node(nodes[i + 1]),
                                 std::memory_order_relaxed);
        }
        while (true) {
            // 每轮重试前重读 head_ 刷新期望值（同 acquire：退避时长内的
            // 推进会让陈旧期望值确定性失败，偶发参与者会饿死秒级）。
            uint64_t head = head_.load(std::memory_order_relaxed);
            nodes[valid - 1]->next.store(index_of(head),
                                         std::memory_order_relaxed);
            const uint64_t new_head =
                pack(static_cast<uint32_t>(tag_of(head) + 1),
                     index_of_node(nodes[0]));
            if (head_.compare_exchange_weak(
                    head, new_head,
                    std::memory_order_release, std::memory_order_relaxed)) {
                return;
            }
            KAIRO_CPU_PAUSE();
        }
    }

private:
    struct Node {
        T data;
        std::atomic<uint32_t> next{kNilIndex};
        std::atomic<uint32_t> state{kStateFree};
    };

    static constexpr uint32_t kNilIndex = 0xFFFFFFFFu;
    static constexpr uint32_t kStateFree = 0;
    static constexpr uint32_t kStateAllocated = 1;
    static constexpr size_t kMaxCapacity =
        static_cast<size_t>(kNilIndex) - 1;

    static constexpr uint64_t kIndexMask = 0xFFFFFFFFull;
    static constexpr uint64_t kTagMask = ~kIndexMask;

    static uint64_t pack(uint32_t tag, uint32_t index) {
        return (static_cast<uint64_t>(tag) << 32) | index;
    }
    static uint32_t index_of(uint64_t head) {
        return static_cast<uint32_t>(head & kIndexMask);
    }
    static uint32_t tag_of(uint64_t head) {
        return static_cast<uint32_t>(head >> 32);
    }

    // A validated T* sits exactly at a Node boundary (data is Node's first
    // member), so it reinterprets back to its Node* and index via plain
    // pointer arithmetic on the contiguous array.
    uint32_t index_of_node(const Node* node) const {
        return static_cast<uint32_t>(
            (reinterpret_cast<std::uintptr_t>(node) -
             reinterpret_cast<std::uintptr_t>(storage_.get())) /
            sizeof(Node));
    }

    /**
     * Validates that obj points at a node boundary inside storage_ and
     * returns the enclosing Node. Throws std::logic_error for foreign or
     * misaligned pointers, exactly like the previous implementation.
     */
    Node* validated_node(T* obj) {
        // Validate the integer address before subtracting it.  A foreign
        // pointer can be farther than ptrdiff_t can represent from storage_,
        // so signed address subtraction would be undefined behavior.
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(obj);
        const std::uintptr_t storage_begin =
            reinterpret_cast<std::uintptr_t>(&storage_[0].data);
        const std::uintptr_t storage_size = capacity_ * sizeof(Node);
        if (storage_size > std::numeric_limits<std::uintptr_t>::max() - storage_begin
            || address < storage_begin
            || address > storage_begin + storage_size) {
            throw std::logic_error(
                std::string("ObjectPool: release of foreign pointer ")
                + std::to_string(address));
        }

        const std::uintptr_t offset = address - storage_begin;
        if (offset % sizeof(Node) != 0) {
            throw std::logic_error(
                std::string("ObjectPool: release of foreign pointer ")
                + std::to_string(address));
        }

        const size_t index = static_cast<size_t>(offset / sizeof(Node));
        if (index >= capacity_) {
            throw std::logic_error(
                std::string("ObjectPool: release of foreign pointer ")
                + std::to_string(address));
        }
        return &storage_[index];
    }

    size_t capacity_;
    std::unique_ptr<Node[]> storage_;  // contiguous; enables O(1) release()
    alignas(64) std::atomic<uint64_t> head_{0};  // (tag << 32) | free-list top
};

} // namespace util
} // namespace kairo
