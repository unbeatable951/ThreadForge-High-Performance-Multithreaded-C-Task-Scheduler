#include "test_framework.hpp"
#include "threadforge/cache_alignment.hpp"
#include <thread>
#include <vector>
#include <chrono>

using namespace threadforge;

TEST(CacheAlignmentTest, StructureSizeAndAlignment) {
    ASSERT_TRUE(alignof(AlignedCounter) >= 64);
    ASSERT_TRUE(sizeof(AlignedCounter) >= 64);

    CacheAligned<int> aligned_int(42);
    ASSERT_EQ(*aligned_int, 42);
    ASSERT_TRUE(reinterpret_cast<uintptr_t>(&aligned_int) % 64 == 0);
}

TEST(CacheAlignmentTest, CorrectnessUnderConcurrentAtomicContention) {
    constexpr int kThreads = 4;
    constexpr uint64_t kIncrements = 200'000;

    // 1. Unaligned counters (shared cache line)
    UnalignedCounter unaligned_counters[kThreads];
    {
        std::vector<std::thread> workers;
        for (int i = 0; i < kThreads; ++i) {
            workers.emplace_back([&unaligned_counters, i] {
                for (uint64_t j = 0; j < kIncrements; ++j) {
                    unaligned_counters[i].counter.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& w : workers) w.join();
    }

    for (int i = 0; i < kThreads; ++i) {
        ASSERT_EQ(unaligned_counters[i].counter.load(), kIncrements);
    }

    // 2. Cache-aligned counters (isolated cache lines)
    AlignedCounter aligned_counters[kThreads];
    {
        std::vector<std::thread> workers;
        for (int i = 0; i < kThreads; ++i) {
            workers.emplace_back([&aligned_counters, i] {
                for (uint64_t j = 0; j < kIncrements; ++j) {
                    aligned_counters[i].counter.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& w : workers) w.join();
    }

    for (int i = 0; i < kThreads; ++i) {
        ASSERT_EQ(aligned_counters[i].counter.load(), kIncrements);
    }
}
