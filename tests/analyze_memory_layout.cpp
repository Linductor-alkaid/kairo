/**
 * 内存布局分析工具
 * 检查 LockFreeQueue 的内存布局，识别 false sharing 风险
 */

#include "kairo/util/lockfree_queue.hpp"
#include <iostream>
#include <iomanip>
#include <functional>

using namespace kairo::util;

template<typename T>
void analyze_layout() {
    LockFreeQueue<T> queue(16);

    std::cout << "=== LockFreeQueue<" << typeid(T).name() << "> 内存布局分析 ===\n\n";
    std::cout << "对象大小: " << sizeof(queue) << " bytes\n";
    std::cout << "缓存行大小: 64 bytes\n\n";

    // 获取成员变量地址（通过反射技巧）
    auto base = reinterpret_cast<uintptr_t>(&queue);

    std::cout << "对象基地址: 0x" << std::hex << base << std::dec << "\n\n";
    std::cout << "成员变量布局:\n";
    std::cout << std::setw(30) << "成员" << std::setw(20) << "偏移(bytes)"
              << std::setw(20) << "缓存行" << "\n";
    std::cout << std::string(70, '-') << "\n";

    // 注意：这里使用 offsetof 的替代方法
    // 由于 LockFreeQueue 成员是私有的，我们通过创建实例并计算地址来分析

    std::cout << "⚠️  无法直接访问私有成员，使用理论分析:\n\n";

    std::cout << "理论布局（基于类定义顺序）:\n";
    std::cout << "  1. capacity_ (size_t, 8 bytes) - offset 0\n";
    std::cout << "  2. mask_ (size_t, 8 bytes) - offset 8\n";
    std::cout << "  3. buffer_ (std::vector<T>, 24 bytes) - offset 16\n";
    std::cout << "  4. sequences_ (std::vector<atomic<size_t>>, 24 bytes) - offset 40\n";
    std::cout << "  5. enqueue_pos_ (atomic<size_t>, 8 bytes) - offset 64\n";
    std::cout << "  6. dequeue_pos_ (atomic<size_t>, 8 bytes) - offset 72\n\n";

    std::cout << "缓存行分布:\n";
    std::cout << "  缓存行 0 (0-63): capacity_, mask_, buffer_, sequences_(部分)\n";
    std::cout << "  缓存行 1 (64-127): sequences_(部分), enqueue_pos_, dequeue_pos_\n\n";

    std::cout << "⚠️  FALSE SHARING 风险识别:\n";
    std::cout << "  [高风险] enqueue_pos_ 和 dequeue_pos_ 在同一缓存行\n";
    std::cout << "    - 多个生产者写 enqueue_pos_\n";
    std::cout << "    - 消费者写 dequeue_pos_\n";
    std::cout << "    - 导致缓存行乒乓效应\n\n";
}

int main() {
    analyze_layout<int>();

    std::cout << "\n=== 优化建议 ===\n\n";
    std::cout << "1. 使用 alignas(64) 分离热点变量:\n";
    std::cout << "   alignas(64) std::atomic<size_t> enqueue_pos_;\n";
    std::cout << "   alignas(64) std::atomic<size_t> dequeue_pos_;\n\n";

    std::cout << "2. 添加 padding 避免与其他成员共享:\n";
    std::cout << "   char padding1_[64 - sizeof(...)];  // 在 enqueue_pos_ 前\n";
    std::cout << "   char padding2_[64 - sizeof(...)];  // 在 dequeue_pos_ 后\n\n";

    return 0;
}
