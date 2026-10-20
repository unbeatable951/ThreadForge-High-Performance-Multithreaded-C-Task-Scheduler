#pragma once

#include <vector>
#include <queue>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <stdexcept>
#include <atomic>
#include <chrono>
#include <type_traits>

#include "threadforge/metrics.hpp"

namespace threadforge {

/**
 * @brief Internal wrapper for a timed scheduled task.
 * Captures submit timestamp to measure queue wait latency.
 */
struct TimedTask {
    std::function<void(size_t worker_id)> func;
    TimePoint submit_time;
};

/**
 * @brief ThreadPool: Industrial-grade thread pool conforming to modern C++20 concurrency standards.
 * Features:
 *  - Mutex + condition variable synchronization (no busy-waiting)
 *  - std::packaged_task / std::future integration for values and exception propagation
 *  - Graceful shutdown and immediate rejection upon post-shutdown submission
 *  - Execution metrics tracking queue latency, worker utilization, and task throughput
 */
class ThreadPool {
public:
    explicit ThreadPool(size_t threads = std::thread::hardware_concurrency());
    ~ThreadPool();

    // Non-copyable and non-movable to ensure robust thread lifetime management
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /**
     * @brief Submit a callable object (function, lambda, functor) to the queue.
     * @return std::future<ReturnType> for the result or exception.
     * @throws std::runtime_error if submitted after shutdown has started.
     */
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args) 
        -> std::future<std::invoke_result_t<F, Args...>> {
        using ReturnType = std::invoke_result_t<F, Args...>;

        // Bind arguments into a packaged_task
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            [func = std::forward<F>(f), 
             ...captured_args = std::forward<Args>(args)]() mutable -> ReturnType {
                return std::invoke(std::move(func), std::move(captured_args)...);
            }
        );

        std::future<ReturnType> res = task->get_future();

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (stop_requested_) {
                throw std::runtime_error("ThreadPool: submit() called on a stopped thread pool");
            }

            metrics_.record_task_submitted();

            tasks_.push(TimedTask{
                [task, this](size_t worker_id) {
                    try {
                        (*task)();
                    } catch (...) {
                        // The exception is caught by packaged_task and stored in the future!
                    }
                },
                Clock::now()
            });
        }

        cv_task_available_.notify_one();
        return res;
    }

    /**
     * @brief Blocks until all currently queued and running tasks finish.
     */
    void wait_idle();

    /**
     * @brief Gracefully initiates shutdown: finishes all remaining queued tasks and joins workers.
     */
    void shutdown();

    /**
     * @brief Query pool status
     */
    bool is_stopped() const noexcept {
        return stop_requested_.load(std::memory_order_acquire);
    }

    size_t worker_count() const noexcept {
        return workers_.size();
    }

    size_t pending_tasks() const noexcept {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        return tasks_.size();
    }

    const ExecutionMetrics& metrics() const noexcept {
        return metrics_;
    }

    ExecutionMetrics& metrics() noexcept {
        return metrics_;
    }

private:
    void worker_loop(size_t worker_id);

    std::vector<std::thread> workers_;
    std::queue<TimedTask> tasks_;

    mutable std::mutex queue_mutex_;
    std::condition_variable cv_task_available_;
    std::condition_variable cv_idle_;

    std::atomic<bool> stop_requested_{false};
    std::atomic<size_t> active_tasks_{0};

    ExecutionMetrics metrics_;
};

} // namespace threadforge
