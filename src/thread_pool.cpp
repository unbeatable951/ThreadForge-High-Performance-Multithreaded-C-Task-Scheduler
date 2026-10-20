#include "threadforge/thread_pool.hpp"
#include <iostream>

namespace threadforge {

ThreadPool::ThreadPool(size_t threads)
    : stop_requested_(false),
      active_tasks_(0),
      metrics_(threads == 0 ? 1 : threads) {
    
    size_t count = threads == 0 ? 1 : threads;
    metrics_.start_timing();

    workers_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        workers_.emplace_back([this, i] {
            this->worker_loop(i);
        });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::worker_loop(size_t worker_id) {
    while (true) {
        TimedTask task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_task_available_.wait(lock, [this] {
                return stop_requested_.load(std::memory_order_acquire) || !tasks_.empty();
            });

            if (tasks_.empty()) {
                if (stop_requested_.load(std::memory_order_acquire)) {
                    break;
                }
                continue;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
            active_tasks_.fetch_add(1, std::memory_order_relaxed);
        }

        auto exec_start = Clock::now();
        uint64_t wait_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(exec_start - task.submit_time).count()
        );

        bool success = true;
        try {
            task.func(worker_id);
        } catch (...) {
            success = false;
        }

        auto exec_end = Clock::now();
        uint64_t exec_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(exec_end - exec_start).count()
        );

        if (success) {
            metrics_.record_task_completed(worker_id, wait_ns, exec_ns);
        } else {
            metrics_.record_task_failed(worker_id, wait_ns, exec_ns);
        }

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
            if (tasks_.empty() && active_tasks_.load(std::memory_order_acquire) == 0) {
                cv_idle_.notify_all();
            }
        }
    }
}

void ThreadPool::wait_idle() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    cv_idle_.wait(lock, [this] {
        return tasks_.empty() && active_tasks_.load(std::memory_order_acquire) == 0;
    });
}

void ThreadPool::shutdown() {
    bool already_stopped = stop_requested_.exchange(true, std::memory_order_acq_rel);
    if (already_stopped) {
        return;
    }

    cv_task_available_.notify_all();

    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();

    metrics_.stop_timing();
}

} // namespace threadforge
