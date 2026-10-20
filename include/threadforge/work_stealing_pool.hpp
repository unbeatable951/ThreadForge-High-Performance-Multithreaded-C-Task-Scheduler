#pragma once

#include <vector>
#include <thread>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <future>
#include <random>
#include <queue>
#include <functional>

#include "threadforge/work_stealing_queue.hpp"
#include "threadforge/metrics.hpp"

namespace threadforge {

/**
 * @brief Thread-local worker ID to distinguish internal worker threads from external client threads.
 */
inline thread_local int t_local_worker_id = -1;

/**
 * @brief High-performance Work-Stealing Task Scheduler.
 * Each worker thread maintains a private LIFO work queue.
 * Idle workers actively steal tasks (FIFO) from peer workers, eliminating load imbalance
 * in irregular workloads, recursive fork-join graphs, and uneven task durations.
 */
class WorkStealingPool {
public:
    explicit WorkStealingPool(size_t threads = std::thread::hardware_concurrency());
    ~WorkStealingPool();

    // Non-copyable, non-movable
    WorkStealingPool(const WorkStealingPool&) = delete;
    WorkStealingPool& operator=(const WorkStealingPool&) = delete;

    /**
     * @brief Submit a callable task to the work-stealing pool.
     * If submitted from a worker thread, it pushes directly to its local queue (zero contention).
     * If submitted from an external thread, it is placed in the global injector queue.
     */
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args) 
        -> std::future<std::invoke_result_t<F, Args...>> {
        using ReturnType = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            [func = std::forward<F>(f), 
             ...captured_args = std::forward<Args>(args)]() mutable -> ReturnType {
                return std::invoke(std::move(func), std::move(captured_args)...);
            }
        );

        std::future<ReturnType> res = task->get_future();

        if (stop_requested_.load(std::memory_order_acquire)) {
            throw std::runtime_error("WorkStealingPool: submit() called on stopped scheduler");
        }

        metrics_.record_task_submitted();
        active_tasks_.fetch_add(1, std::memory_order_relaxed);

        auto wrapped_task = [task](size_t worker_id) {
            try {
                (*task)();
            } catch (...) {
                // Stored in future
            }
        };

        if (t_local_worker_id >= 0 && static_cast<size_t>(t_local_worker_id) < worker_count_) {
            // Fast path: push to worker's own local queue
            queues_[t_local_worker_id]->push_bottom(std::move(wrapped_task));
            cv_work_available_.notify_one();
        } else {
            // External path: push to global injector queue
            {
                std::lock_guard<std::mutex> lock(injector_mutex_);
                injector_queue_.push(std::move(wrapped_task));
            }
            cv_work_available_.notify_all();
        }

        return res;
    }

    void wait_idle();
    void shutdown();

    size_t worker_count() const noexcept { return worker_count_; }
    uint64_t steal_attempts() const noexcept { return total_steal_attempts_.load(std::memory_order_relaxed); }
    uint64_t successful_steals() const noexcept { return successful_steals_.load(std::memory_order_relaxed); }

    const ExecutionMetrics& metrics() const noexcept { return metrics_; }
    ExecutionMetrics& metrics() noexcept { return metrics_; }

private:
    void worker_loop(size_t worker_id);
    std::optional<Task> try_steal(size_t worker_id);

    const size_t worker_count_;
    std::vector<std::thread> workers_;
    std::vector<std::unique_ptr<SynchronizedWorkStealingQueue>> queues_;

    std::queue<Task> injector_queue_;
    std::mutex injector_mutex_;

    std::atomic<bool> stop_requested_{false};
    std::atomic<size_t> active_tasks_{0};

    std::mutex wait_mutex_;
    std::condition_variable cv_work_available_;
    std::condition_variable cv_idle_;

    std::atomic<uint64_t> total_steal_attempts_{0};
    std::atomic<uint64_t> successful_steals_{0};

    ExecutionMetrics metrics_;
};

} // namespace threadforge
