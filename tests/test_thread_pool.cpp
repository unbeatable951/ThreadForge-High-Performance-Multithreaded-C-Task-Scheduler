#include "test_framework.hpp"
#include "threadforge/thread_pool.hpp"
#include <atomic>
#include <numeric>
#include <stdexcept>
#include <vector>

using namespace threadforge;

TEST(ThreadPoolTest, BasicExecutionAndFuture) {
    ThreadPool pool(4);
    auto fut = pool.submit([](int a, int b) {
        return a + b;
    }, 40, 2);

    ASSERT_EQ(fut.get(), 42);
}

TEST(ThreadPoolTest, VoidReturnTask) {
    ThreadPool pool(4);
    std::atomic<bool> executed{false};

    auto fut = pool.submit([&executed] {
        executed.store(true);
    });

    fut.get();
    ASSERT_TRUE(executed.load());
}

TEST(ThreadPoolTest, ExceptionPropagation) {
    ThreadPool pool(4);
    auto fut = pool.submit([] {
        throw std::invalid_argument("Intentional PTX test exception");
    });

    ASSERT_THROW(fut.get(), std::invalid_argument);
}

TEST(ThreadPoolTest, SubmitAfterShutdownThrows) {
    ThreadPool pool(2);
    pool.shutdown();
    ASSERT_TRUE(pool.is_stopped());

    ASSERT_THROW(
        pool.submit([] { return 1; }),
        std::runtime_error
    );
}

TEST(ThreadPoolTest, ConcurrentSubmissionStress) {
    ThreadPool pool(8);
    const int num_tasks = 2000;
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

TEST(ThreadPoolTest, RepeatedConstructionDestruction) {
    for (int iter = 0; iter < 10; ++iter) {
        ThreadPool pool(4);
        auto fut = pool.submit([] { return 100; });
        ASSERT_EQ(fut.get(), 100);
    }
}

TEST(ThreadPoolTest, WaitIdleSemantics) {
    ThreadPool pool(4);
    std::atomic<int> val{0};

    for (int i = 0; i < 50; ++i) {
        pool.submit([&val] {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            val.fetch_add(1);
        });
    }

    pool.wait_idle();
    ASSERT_EQ(val.load(), 50);
}
