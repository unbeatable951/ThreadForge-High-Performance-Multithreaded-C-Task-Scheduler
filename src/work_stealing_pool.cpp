#include "threadforge/work_stealing_pool.hpp"
#include <chrono>

namespace threadforge {

WorkStealingPool::WorkStealingPool(size_t threads)
    : worker_count_(threads == 0 ? 1 : threads),
      metrics_(threads == 0 ? 1 : threads) {

    metrics_.start_timing();

    queues_.reserve(worker_count_);
    for (size_t i = 0; i < worker_count_; ++i) {
        queues_.push_back(std::make_unique<SynchronizedWorkStealingQueue>());
    }

    workers_.reserve(worker_count_);
    for (size_t i = 0; i < worker_count_; ++i) {
        workers_.emplace_back([this, i] {
            t_local_worker_id = static_cast<int>(i);
            this->worker_loop(i);
        });
    }
}

WorkStealingPool::~WorkStealingPool() {
    shutdown();
}

std::optional<Task> WorkStealingPool::try_steal(size_t worker_id) {
    if (worker_count_ <= 1) {
        return std::nullopt;
    }

    // Pseudo-random offset victim selection
    static thread_local std::mt19937 gen(std::random_device{}() + static_cast<unsigned int>(worker_id));
    std::uniform_int_distribution<size_t> dist(1, worker_count_ - 1);

    size_t victim_offset = dist(gen);
    size_t victim_id = (worker_id + victim_offset) % worker_count_;

    total_steal_attempts_.fetch_add(1, std::memory_order_relaxed);
    auto stolen = queues_[victim_id]->steal_top();
    if (stolen.has_value()) {
        successful_steals_.fetch_add(1, std::memory_order_relaxed);
    }
    return stolen;
}

void WorkStealingPool::worker_loop(size_t worker_id) {
    int idle_spins = 0;

    while (true) {
        std::optional<Task> task_opt;

        // 1. Pop from local queue (LIFO - Cache warm)
        task_opt = queues_[worker_id]->pop_bottom();

        // 2. If local queue empty, check global injector queue
        if (!task_opt.has_value()) {
            std::unique_lock<std::mutex> lock(injector_mutex_, std::try_to_lock);
            if (lock.owns_lock() && !injector_queue_.empty()) {
                task_opt = std::move(injector_queue_.front());
                injector_queue_.pop();
            }
        }

        // 3. If still empty, attempt to steal from peer worker
        if (!task_opt.has_value()) {
            task_opt = try_steal(worker_id);
        }

        // 4. If work was found, execute it
        if (task_opt.has_value()) {
            idle_spins = 0;
            auto start = Clock::now();
            bool success = true;

            try {
                (*task_opt)(worker_id);
            } catch (...) {
                success = false;
            }

            auto end = Clock::now();
            uint64_t exec_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()
            );

            if (success) {
                metrics_.record_task_completed(worker_id, 0, exec_ns);
            } else {
                metrics_.record_task_failed(worker_id, 0, exec_ns);
            }

            size_t remaining = active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
            if (remaining == 1) {
                std::unique_lock<std::mutex> lock(wait_mutex_);
                cv_idle_.notify_all();
            }
            continue;
        }

        // 5. No task found: check if shutdown initiated
        if (stop_requested_.load(std::memory_order_acquire)) {
            // Check if any lingering tasks exist anywhere
            bool has_remaining = false;
            for (const auto& q : queues_) {
                if (!q->empty()) {
                    has_remaining = true;
                    break;
                }
            }
            if (!has_remaining) {
                std::lock_guard<std::mutex> lock(injector_mutex_);
                if (injector_queue_.empty()) {
                    break; // All queues completely drained
                }
            }
        }

        // 6. Exponential backoff and sleep to prevent excessive CPU spinning
        if (++idle_spins < 32) {
            std::this_thread::yield();
        } else {
            std::unique_lock<std::mutex> lock(wait_mutex_);
            cv_work_available_.wait_for(lock, std::chrono::microseconds(500), [this] {
                return stop_requested_.load(std::memory_order_acquire) ||
                       active_tasks_.load(std::memory_order_relaxed) > 0;
            });
            idle_spins = 0;
        }
    }
}

void WorkStealingPool::wait_idle() {
    std::unique_lock<std::mutex> lock(wait_mutex_);
    cv_idle_.wait(lock, [this] {
        return active_tasks_.load(std::memory_order_acquire) == 0;
    });
}

void WorkStealingPool::shutdown() {
    bool already = stop_requested_.exchange(true, std::memory_order_acq_rel);
    if (already) return;

    {
        std::lock_guard<std::mutex> lock(wait_mutex_);
        cv_work_available_.notify_all();
    }

    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();

    metrics_.stop_timing();
}

} // namespace threadforge
