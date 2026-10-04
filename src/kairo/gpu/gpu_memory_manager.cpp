#include "gpu_memory_manager.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

namespace kairo {
namespace gpu {

GpuMemoryManager::GpuMemoryManager(RawAlloc raw_alloc, RawFree raw_free, size_t pool_size)
    : raw_alloc_(std::move(raw_alloc))
    , raw_free_(std::move(raw_free))
    , pool_size_(pool_size) {}

GpuMemoryManager::~GpuMemoryManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pool_backing_ != nullptr && raw_free_) {
        raw_free_(pool_backing_);
        pool_backing_ = nullptr;
    }
    for (void* block : raw_alloced_blocks_) {
        if (raw_free_) {
            raw_free_(block);
        }
    }
    for (void* block : direct_alloced_blocks_) {
        if (raw_free_) {
            raw_free_(block);
        }
    }
    raw_alloced_blocks_.clear();
    direct_alloced_blocks_.clear();
    block_size_map_.clear();
    free_blocks_.clear();
}

size_t GpuMemoryManager::align_up(size_t size, size_t alignment) const {
    return (size + alignment - 1) & ~(alignment - 1);
}

void* GpuMemoryManager::allocate(size_t size) {
    if (size == 0 || !raw_alloc_) {
        return nullptr;
    }
    const size_t max_size = std::numeric_limits<size_t>::max();
    if (size > max_size - kAlignment - kHeaderSize) {
        return nullptr;
    }
    const size_t need = align_up(size, kAlignment) + kHeaderSize;

    std::lock_guard<std::mutex> lock(mutex_);

    if (pool_size_ == 0) {
        void* block = raw_alloc_(need);
        if (block == nullptr) {
            return nullptr;
        }
        raw_alloced_blocks_.insert(block);
        block_size_map_[block] = size;
        total_allocated_ += size;
        allocation_count_++;
        return static_cast<char*>(block) + kHeaderSize;
    }

    if (pool_backing_ == nullptr) {
        pool_backing_ = raw_alloc_(pool_size_);
        if (pool_backing_ == nullptr) {
            return nullptr;
        }
        free_blocks_.push_back(FreeBlock{pool_backing_, pool_size_});
    }

    auto it = std::find_if(free_blocks_.begin(), free_blocks_.end(),
                           [need](const FreeBlock& fb) { return fb.size >= need; });
    if (it != free_blocks_.end()) {
        void* block_start = it->ptr;
        size_t block_size = it->size;
        free_blocks_.erase(it);
        if (block_size > need) {
            free_blocks_.push_back(
                FreeBlock{static_cast<char*>(block_start) + need, block_size - need});
        }
        block_size_map_[block_start] = size;
        total_allocated_ += size;
        allocation_count_++;
        return static_cast<char*>(block_start) + kHeaderSize;
    }

    void* block = raw_alloc_(need);
    if (block == nullptr) {
        return nullptr;
    }
    direct_alloced_blocks_.insert(block);
    block_size_map_[block] = size;
    total_allocated_ += size;
    allocation_count_++;
    return static_cast<char*>(block) + kHeaderSize;
}

void GpuMemoryManager::free(void* ptr) {
    if (ptr == nullptr || !raw_free_) {
        return;
    }
    void* block_start = static_cast<char*>(ptr) - kHeaderSize;

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = block_size_map_.find(block_start);
    if (it == block_size_map_.end()) {
        return;
    }
    const size_t user_size = it->second;
    block_size_map_.erase(it);
    total_allocated_ -= user_size;
    allocation_count_--;

    if (raw_alloced_blocks_.count(block_start) != 0) {
        raw_alloced_blocks_.erase(block_start);
        raw_free_(block_start);
        return;
    }

    if (direct_alloced_blocks_.count(block_start) != 0) {
        direct_alloced_blocks_.erase(block_start);
        raw_free_(block_start);
        return;
    }

    const size_t block_size = align_up(user_size, kAlignment) + kHeaderSize;
    free_blocks_.push_back(FreeBlock{block_start, block_size});
    coalesce_free_blocks();
}

GpuMemoryManager::MemoryStats GpuMemoryManager::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    MemoryStats s;
    s.total_allocated = total_allocated_;
    s.allocation_count = allocation_count_;
    for (const FreeBlock& fb : free_blocks_) {
        s.total_free += fb.size;
    }
    return s;
}

void GpuMemoryManager::defragment() {
    std::lock_guard<std::mutex> lock(mutex_);
    coalesce_free_blocks();
}

void GpuMemoryManager::coalesce_free_blocks() {
    if (free_blocks_.empty()) {
        return;
    }
    std::sort(free_blocks_.begin(), free_blocks_.end(),
              [](const FreeBlock& a, const FreeBlock& b) {
                  return std::less<uintptr_t>{}(reinterpret_cast<uintptr_t>(a.ptr),
                                                reinterpret_cast<uintptr_t>(b.ptr));
              });
    std::vector<FreeBlock> merged;
    merged.push_back(free_blocks_.front());
    for (size_t i = 1; i < free_blocks_.size(); ++i) {
        FreeBlock& prev = merged.back();
        const FreeBlock& curr = free_blocks_[i];
        if (static_cast<char*>(prev.ptr) + prev.size == static_cast<char*>(curr.ptr)) {
            prev.size += curr.size;
        } else {
            merged.push_back(curr);
        }
    }
    free_blocks_ = std::move(merged);
}

}  // namespace gpu
}  // namespace kairo
