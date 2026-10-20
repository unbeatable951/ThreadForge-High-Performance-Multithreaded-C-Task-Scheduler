#pragma once

#include <vector>
#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include <exception>
#include <unordered_map>
#include <unordered_set>

#include "threadforge/thread_pool.hpp"

namespace threadforge {

using TaskID = size_t;

class CycleDetectedException : public std::runtime_error {
public:
    explicit CycleDetectedException(const std::string& msg = "Cycle detected in task dependency graph")
        : std::runtime_error(msg) {}
};

class DependencyFailedException : public std::runtime_error {
public:
    explicit DependencyFailedException(const std::string& msg = "Task cancelled because a prerequisite dependency failed")
        : std::runtime_error(msg) {}
};

enum class TaskStatus {
    Pending,
    Ready,
    Running,
    Completed,
    Failed,
    Cancelled
};

inline const char* to_string(TaskStatus status) {
    switch (status) {
        case TaskStatus::Pending:   return "Pending";
        case TaskStatus::Ready:     return "Ready";
        case TaskStatus::Running:   return "Running";
        case TaskStatus::Completed: return "Completed";
        case TaskStatus::Failed:    return "Failed";
        case TaskStatus::Cancelled: return "Cancelled";
        default:                    return "Unknown";
    }
}

inline std::ostream& operator<<(std::ostream& os, TaskStatus status) {
    return os << to_string(status);
}

/**
 * @brief Node representation inside a TaskGraph DAG.
 */
struct TaskNode {
    TaskID id;
    std::string name;
    std::function<void()> func;

    std::vector<TaskID> dependencies; // Predecessors: must finish before this task
    std::vector<TaskID> dependents;   // Successors: wait on this task to finish

    std::atomic<size_t> remaining_dependencies{0};
    std::atomic<bool> executed{false};
    std::atomic<TaskStatus> status{TaskStatus::Pending};
    std::exception_ptr exception{nullptr};
};

/**
 * @brief Directed Acyclic Graph (DAG) Task Scheduler.
 * Features:
 *  - Deterministic cycle detection using topological sort (Kahn's algorithm)
 *  - Lock-free / atomic in-degree dependency counting
 *  - Safe exception propagation (downstream tasks fail safely if a dependency fails)
 *  - Cancellation and graceful shutdown support
 *  - Guarantees each task executes at most once
 */
class TaskGraph {
public:
    struct GraphState {
        std::atomic<bool> cancelled{false};
        std::atomic<size_t> completed_or_cancelled_count{0};
        std::mutex wait_mutex;
        std::condition_variable cv_all_done;
    };

    TaskGraph();
    ~TaskGraph();

    // Non-copyable
    TaskGraph(const TaskGraph&) = delete;
    TaskGraph& operator=(const TaskGraph&) = delete;

    // Movable
    TaskGraph(TaskGraph&&) noexcept;
    TaskGraph& operator=(TaskGraph&&) noexcept;

    /**
     * @brief Add a task to the graph.
     * @param name Descriptive identifier for the task.
     * @param func Callable task logic.
     * @return TaskID Unique task identifier.
     */
    TaskID add_task(std::string name, std::function<void()> func);

    /**
     * @brief Declare that 'successor' depends on 'predecessor'.
     * (i.e. 'predecessor' must finish before 'successor' can start).
     */
    void add_dependency(TaskID predecessor, TaskID successor);

    /**
     * @brief Checks if the graph contains any cycles without modifying state.
     */
    bool has_cycle() const;

    /**
     * @brief Computes a topological ordering of the task IDs.
     * @throws CycleDetectedException if a cycle exists in the graph.
     */
    std::vector<TaskID> topological_sort() const;

    /**
     * @brief Submit and execute the DAG across the provided thread pool.
     * Performs cycle detection prior to running.
     * Non-blocking: returns immediately; call wait() to block for completion.
     */
    void execute(ThreadPool& pool);

    /**
     * @brief Blocks until all tasks in the graph have completed, failed, or been cancelled.
     */
    void wait();

    /**
     * @brief Cancels execution of any pending tasks that have not yet started.
     */
    void cancel();

    /**
     * @brief Reset task execution states so the same DAG can be executed again.
     */
    void reset();

    // Queries
    size_t size() const noexcept { return nodes_.size(); }
    TaskStatus get_status(TaskID id) const;
    std::exception_ptr get_exception(TaskID id) const;
    const std::string& get_name(TaskID id) const;

    /**
     * @brief If any task threw an unhandled exception, rethrows the first encountered exception.
     */
    void throw_if_failed() const;

    bool is_completed() const;

private:
    void on_task_completed(TaskID id, ThreadPool& pool);
    void on_task_failed(TaskID id, std::exception_ptr ep, ThreadPool& pool);
    void cascade_cancel(TaskID id);

    std::vector<std::unique_ptr<TaskNode>> nodes_;
    std::unique_ptr<GraphState> state_;
};

} // namespace threadforge
