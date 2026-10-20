#pragma once

#include <deque>
#include <mutex>
#include <optional>
#include <functional>
#include <atomic>
#include <memory>
#include <vector>
#include "threadforge/cache_alignment.hpp"

namespace threadforge {

using Task = std::function<void(size_t worker_id)>;

/**
 * @brief Thread-safe Synchronized Work-Stealing Deque.
 * 
 * Design & Synchronization Model:
 *  - Worker thread interacts with BOTTOM:
 *      * push_bottom(task): pushes newest task (LIFO).
 *      * pop_bottom(): pops newest task (LIFO). Worker works locally on warm cache lines.
 *  - Stealer threads interact with TOP:
 *      * steal_top(): steals oldest task (FIFO). Takes larger task branches, amortizing steal cost.
 *  - Linearization & Safety:
 *      * Controlled via fine-grained per-queue mutex.
 *      * Guarantees zero duplicate execution, zero lost tasks, and race freedom.
 */
class SynchronizedWorkStealingQueue {
public:
    SynchronizedWorkStealingQueue() = default;
    ~SynchronizedWorkStealingQueue() = default;

    // Non-copyable, non-movable
    SynchronizedWorkStealingQueue(const SynchronizedWorkStealingQueue&) = delete;
    SynchronizedWorkStealingQueue& operator=(const SynchronizedWorkStealingQueue&) = delete;

    /**
     * @brief Worker pushes a task to the bottom (tail) of its own queue.
     */
    void push_bottom(Task task) {
        std::lock_guard<std::mutex> lock(mutex_);
        deque_.push_back(std::move(task));
    }

    /**
     * @brief Worker pops a task from the bottom (tail) of its own queue (LIFO).
     */
    std::optional<Task> pop_bottom() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (deque_.empty()) {
            return std::nullopt;
        }
        Task task = std::move(deque_.back());
        deque_.pop_back();
        return task;
    }

    /**
     * @brief Idle victim/stealer steals a task from the top (head) of this queue (FIFO).
     */
    std::optional<Task> steal_top() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (deque_.empty()) {
            return std::nullopt;
        }
        Task task = std::move(deque_.front());
        deque_.pop_front();
        return task;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deque_.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deque_.size();
    }

private:
    std::deque<Task> deque_;
    mutable std::mutex mutex_;
};

/**
 * @brief Chase-Lev Lock-Free Work-Stealing Bounded Circular Deque.
 * 
 * Memory Ordering & Synchronization Proof:
 *  - Only the OWNER worker thread invokes push() and pop().
 *  - Multiple STEALER threads invoke steal() concurrently with each other and the owner.
 *  - bottom_ is modified only by the owner thread.
 *  - top_ is read and modified by stealers via CAS, and read/modified by owner during conflict resolution.
 * 
 * Invariants & Linearization Points:
 *  1. push():
 *     - Writes buffer[b] before incrementing bottom_.
 *     - bottom_.store(b + 1, std::memory_order_release) ensures that the payload is visible
 *       to any stealer acquiring bottom_.
 *  2. pop():
 *     - Decrements bottom_ (std::memory_order_seq_cst) to reserve the slot.
 *     - Reads top_ (std::memory_order_seq_cst).
 *     - If top_ < bottom_, task is retrieved without contention.
 *     - If top_ == bottom_, the last element is contested: owner uses CAS on top_ to serialize against stealers.
 *  3. steal():
 *     - Reads top_ (acquire), then reads bottom_ (acquire).
 *     - If empty (top >= bottom), returns empty.
 *     - Reads task, then executes CAS(top_, t, t + 1) with acq_rel.
 *     - If CAS succeeds, task ownership is acquired; if CAS fails, someone else stole it.
 */
class ChaseLevDeque {
public:
    explicit ChaseLevDeque(size_t capacity = 1024)
        : capacity_(capacity),
          mask_(capacity - 1),
          buffer_(capacity),
          top_(0),
          bottom_(0) {
        // Capacity must be power of two for bitwise masking
        if ((capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("Capacity must be a power of two");
        }
    }

    bool push(Task task) {
        int64_t b = bottom_.load(std::memory_order_relaxed);
        int64_t t = top_.load(std::memory_order_acquire);

        if (b - t >= static_cast<int64_t>(capacity_ - 1)) {
            // Bounded queue full
            return false;
        }

        buffer_[b & mask_] = std::move(task);
        // Release ensures buffer write is visible before bottom is advanced
        bottom_.store(b + 1, std::memory_order_release);
        return true;
    }

    std::optional<Task> pop() {
        int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
        bottom_.store(b, std::memory_order_seq_cst);
        int64_t t = top_.load(std::memory_order_seq_cst);

        if (t <= b) {
            // Queue has tasks
            Task task = std::move(buffer_[b & mask_]);
            if (t == b) {
                // Last element race between owner pop and stealer steal
                int64_t expected = t;
                if (!top_.compare_exchange_strong(expected, t + 1, 
                                                   std::memory_order_seq_cst, 
                                                   std::memory_order_relaxed)) {
                    // Stealer won the race
                    bottom_.store(t + 1, std::memory_order_relaxed);
                    return std::nullopt;
                }
                bottom_.store(t + 1, std::memory_order_relaxed);
            }
            return task;
        } else {
            // Queue was already empty
            bottom_.store(t, std::memory_order_relaxed);
            return std::nullopt;
        }
    }

    std::optional<Task> steal() {
        while (true) {
            int64_t t = top_.load(std::memory_order_acquire);
            // Must have memory barrier so bottom read observes order after top read
            std::atomic_thread_fence(std::memory_order_seq_cst);
            int64_t b = bottom_.load(std::memory_order_acquire);

            if (t >= b) {
                return std::nullopt; // Empty
            }

            Task task = buffer_[t & mask_];
            if (top_.compare_exchange_strong(t, t + 1, 
                                             std::memory_order_seq_cst, 
                                             std::memory_order_relaxed)) {
                return task;
            }
            // Failed CAS: retry or yield
        }
    }

    bool empty() const noexcept {
        int64_t t = top_.load(std::memory_order_relaxed);
        int64_t b = bottom_.load(std::memory_order_relaxed);
        return t >= b;
    }

    size_t size() const noexcept {
        int64_t t = top_.load(std::memory_order_relaxed);
        int64_t b = bottom_.load(std::memory_order_relaxed);
        return (b > t) ? static_cast<size_t>(b - t) : 0;
    }

private:
    const size_t capacity_;
    const size_t mask_;
    std::vector<Task> buffer_;
    alignas(kCacheLineSize) std::atomic<int64_t> top_;
    alignas(kCacheLineSize) std::atomic<int64_t> bottom_;
};

} // namespace threadforge
