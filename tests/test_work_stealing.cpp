#include "test_framework.hpp"
#include "threadforge/work_stealing_pool.hpp"
#include "threadforge/work_stealing_queue.hpp"
#include <atomic>
#include <vector>

using namespace threadforge;

TEST(WorkStealingQueueTest, SynchronizedLIFOAndFIFO) {
    SynchronizedWorkStealingQueue q;
    std::vector<int> execution_order;

    q.push_bottom([](size_t) {}); // Task 0 (oldest)
    q.push_bottom([](size_t) {}); // Task 1
    q.push_bottom([](size_t) {}); // Task 2 (newest)

    ASSERT_EQ(q.size(), 3);

    // Stealer should steal oldest (Task 0)
    auto stolen = q.steal_top();
    ASSERT_TRUE(stolen.has_value());

    // Worker should pop newest (Task 2)
    auto local = q.pop_bottom();
    ASSERT_TRUE(local.has_value());

    ASSERT_EQ(q.size(), 1);
}

TEST(WorkStealingQueueTest, ChaseLevPushPopSteal) {
    ChaseLevDeque deque(64);

    std::atomic<int> completed{0};

    // Owner pushes tasks
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(deque.push([&completed](size_t) {
            completed.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    ASSERT_EQ(deque.size(), 10);

    // Stealer steals one
    auto stolen = deque.steal();
    ASSERT_TRUE(stolen.has_value());
    (*stolen)(1);

    // Owner pops one
    auto popped = deque.pop();
    ASSERT_TRUE(popped.has_value());
    (*popped)(0);

    ASSERT_EQ(completed.load(), 2);
    ASSERT_EQ(deque.size(), 8);
}

TEST(WorkStealingPoolTest, HighConcurrencyLoadBalancing) {
    WorkStealingPool pool(4);
    const int num_tasks = 1000;
    std::atomic<int> counter{0};
    std::vector<std::future<void>> futures;
    futures.reserve(num_tasks);

    for (int i = 0; i < num_tasks; ++i) {
        futures.push_back(pool.submit([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    for (auto& f : futures) {
        f.get();
    }

    ASSERT_EQ(counter.load(), num_tasks);
    ASSERT_EQ(pool.metrics().completed(), static_cast<uint64_t>(num_tasks));
}

TEST(WorkStealingPoolTest, UnevenWorkloadStealing) {
    WorkStealingPool pool(4);
    std::atomic<int> total{0};

    // Submit a burst of tasks with varying durations
    for (int i = 0; i < 200; ++i) {
        pool.submit([&total, i] {
            if (i % 5 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            total.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool.wait_idle();
    ASSERT_EQ(total.load(), 200);
}
