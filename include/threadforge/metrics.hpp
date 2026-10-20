#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <memory>
#include "threadforge/cache_alignment.hpp"

namespace threadforge {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Nanoseconds = std::chrono::nanoseconds;

/**
 * @brief Holds high-performance runtime execution metrics for ThreadForge.
 * Per-worker completed counters are cache-line aligned to eliminate false sharing.
 */
class ExecutionMetrics {
public:
    explicit ExecutionMetrics(size_t worker_count = 0)
        : worker_count_(worker_count),
          per_worker_completed_(worker_count) {
        reset();
    }

    void reset() {
        tasks_submitted_.store(0, std::memory_order_relaxed);
        tasks_completed_.store(0, std::memory_order_relaxed);
        tasks_failed_.store(0, std::memory_order_relaxed);
        total_wait_time_ns_.store(0, std::memory_order_relaxed);
        total_exec_time_ns_.store(0, std::memory_order_relaxed);
        for (auto& count : per_worker_completed_) {
            count.value.store(0, std::memory_order_relaxed);
        }
        start_time_ = Clock::now();
        end_time_ = start_time_;
    }

    void start_timing() noexcept {
        start_time_ = Clock::now();
    }

    void stop_timing() noexcept {
        end_time_ = Clock::now();
    }

    void record_task_submitted() noexcept {
        tasks_submitted_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_task_completed(size_t worker_id, uint64_t wait_ns, uint64_t exec_ns) noexcept {
        tasks_completed_.fetch_add(1, std::memory_order_relaxed);
        total_wait_time_ns_.fetch_add(wait_ns, std::memory_order_relaxed);
        total_exec_time_ns_.fetch_add(exec_ns, std::memory_order_relaxed);
        if (worker_id < per_worker_completed_.size()) {
            per_worker_completed_[worker_id].value.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void record_task_failed(size_t worker_id, uint64_t wait_ns, uint64_t exec_ns) noexcept {
        tasks_failed_.fetch_add(1, std::memory_order_relaxed);
        total_wait_time_ns_.fetch_add(wait_ns, std::memory_order_relaxed);
        total_exec_time_ns_.fetch_add(exec_ns, std::memory_order_relaxed);
        if (worker_id < per_worker_completed_.size()) {
            per_worker_completed_[worker_id].value.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Accessors
    uint64_t submitted() const noexcept { return tasks_submitted_.load(std::memory_order_relaxed); }
    uint64_t completed() const noexcept { return tasks_completed_.load(std::memory_order_relaxed); }
    uint64_t failed() const noexcept { return tasks_failed_.load(std::memory_order_relaxed); }
    uint64_t worker_completed(size_t worker_id) const noexcept {
        if (worker_id < per_worker_completed_.size()) {
            return per_worker_completed_[worker_id].value.load(std::memory_order_relaxed);
        }
        return 0;
    }

    double total_wall_clock_ms() const noexcept {
        // Use current time if stop_timing() hasn't been called yet
        TimePoint end = (end_time_ == start_time_) ? Clock::now() : end_time_;
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start_time_);
        return static_cast<double>(duration.count()) / 1000.0;
    }

    double throughput_tasks_per_sec() const noexcept {
        double wall_sec = total_wall_clock_ms() / 1000.0;
        if (wall_sec <= 0.0) return 0.0;
        return static_cast<double>(completed()) / wall_sec;
    }

    double avg_wait_time_us() const noexcept {
        uint64_t comp = completed() + failed();
        if (comp == 0) return 0.0;
        return static_cast<double>(total_wait_time_ns_.load(std::memory_order_relaxed)) / (static_cast<double>(comp) * 1000.0);
    }

    double avg_exec_time_us() const noexcept {
        uint64_t comp = completed() + failed();
        if (comp == 0) return 0.0;
        return static_cast<double>(total_exec_time_ns_.load(std::memory_order_relaxed)) / (static_cast<double>(comp) * 1000.0);
    }

    double worker_utilization_pct() const noexcept {
        if (worker_count_ == 0) return 0.0;
        double wall_ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end_time_ - start_time_).count());
        if (wall_ns <= 0.0) return 0.0;
        double total_cpu_busy_ns = static_cast<double>(total_exec_time_ns_.load(std::memory_order_relaxed));
        double max_available_ns = wall_ns * static_cast<double>(worker_count_);
        double pct = (total_cpu_busy_ns / max_available_ns) * 100.0;
        return pct > 100.0 ? 100.0 : pct;
    }

    std::string summary() const {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2);
        ss << "========================================\n"
           << " ThreadForge Runtime Performance Report \n"
           << "========================================\n"
           << "Workers:                  " << worker_count_ << "\n"
           << "Tasks Submitted:          " << submitted() << "\n"
           << "Tasks Completed:          " << completed() << "\n"
           << "Tasks Failed:             " << failed() << "\n"
           << "Total Wall Clock:         " << total_wall_clock_ms() << " ms\n"
           << "Throughput:               " << throughput_tasks_per_sec() << " tasks/sec\n"
           << "Average Queue Wait Time:  " << avg_wait_time_us() << " us\n"
           << "Average Execution Time:   " << avg_exec_time_us() << " us\n"
           << "Worker Utilization:       " << worker_utilization_pct() << " %\n"
           << "--- Per-Worker Distribution ---\n";
        for (size_t i = 0; i < per_worker_completed_.size(); ++i) {
            ss << "  Worker [" << i << "]: " << worker_completed(i) << " tasks\n";
        }
        ss << "========================================\n";
        return ss.str();
    }

private:
    size_t worker_count_{0};
    std::atomic<uint64_t> tasks_submitted_{0};
    std::atomic<uint64_t> tasks_completed_{0};
    std::atomic<uint64_t> tasks_failed_{0};
    std::atomic<uint64_t> total_wait_time_ns_{0};
    std::atomic<uint64_t> total_exec_time_ns_{0};
    std::vector<CacheAligned<std::atomic<uint64_t>>> per_worker_completed_;
    TimePoint start_time_{Clock::now()};
    TimePoint end_time_{Clock::now()};
};

} // namespace threadforge
