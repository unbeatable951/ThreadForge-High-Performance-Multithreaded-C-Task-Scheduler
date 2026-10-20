#include "threadforge/task_graph.hpp"
#include <queue>

namespace threadforge {

TaskGraph::TaskGraph() : state_(std::make_unique<GraphState>()) {}
TaskGraph::~TaskGraph() = default;
TaskGraph::TaskGraph(TaskGraph&&) noexcept = default;
TaskGraph& TaskGraph::operator=(TaskGraph&&) noexcept = default;

TaskID TaskGraph::add_task(std::string name, std::function<void()> func) {
    TaskID id = nodes_.size();
    auto node = std::make_unique<TaskNode>();
    node->id = id;
    node->name = std::move(name);
    node->func = std::move(func);
    node->status.store(TaskStatus::Pending, std::memory_order_relaxed);
    nodes_.push_back(std::move(node));
    return id;
}

void TaskGraph::add_dependency(TaskID predecessor, TaskID successor) {
    if (predecessor >= nodes_.size() || successor >= nodes_.size()) {
        throw std::out_of_range("TaskGraph: TaskID out of range in add_dependency");
    }
    if (predecessor == successor) {
        throw CycleDetectedException("Self-dependency detected: Task cannot depend on itself");
    }

    nodes_[predecessor]->dependents.push_back(successor);
    nodes_[successor]->dependencies.push_back(predecessor);
}

bool TaskGraph::has_cycle() const {
    try {
        topological_sort();
        return false;
    } catch (const CycleDetectedException&) {
        return true;
    }
}

std::vector<TaskID> TaskGraph::topological_sort() const {
    const size_t n = nodes_.size();
    std::vector<size_t> in_degree(n, 0);

    for (size_t i = 0; i < n; ++i) {
        in_degree[i] = nodes_[i]->dependencies.size();
    }

    std::queue<TaskID> q;
    for (size_t i = 0; i < n; ++i) {
        if (in_degree[i] == 0) {
            q.push(i);
        }
    }

    std::vector<TaskID> order;
    order.reserve(n);

    while (!q.empty()) {
        TaskID u = q.front();
        q.pop();
        order.push_back(u);

        for (TaskID v : nodes_[u]->dependents) {
            if (--in_degree[v] == 0) {
                q.push(v);
            }
        }
    }

    if (order.size() != n) {
        throw CycleDetectedException("TaskGraph contains a circular dependency");
    }

    return order;
}

void TaskGraph::reset() {
    state_->cancelled.store(false, std::memory_order_release);
    state_->completed_or_cancelled_count.store(0, std::memory_order_release);

    for (auto& node : nodes_) {
        node->remaining_dependencies.store(node->dependencies.size(), std::memory_order_relaxed);
        node->executed.store(false, std::memory_order_relaxed);
        node->status.store(TaskStatus::Pending, std::memory_order_relaxed);
        node->exception = nullptr;
    }
}

void TaskGraph::execute(ThreadPool& pool) {
    // Validate acyclicity prior to starting execution
    topological_sort();

    reset();

    std::vector<TaskID> root_tasks;
    for (size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i]->dependencies.empty()) {
            root_tasks.push_back(i);
        }
    }

    if (root_tasks.empty() && !nodes_.empty()) {
        throw CycleDetectedException("No root tasks found to initiate DAG execution");
    }

    if (nodes_.empty()) {
        return;
    }

    for (TaskID id : root_tasks) {
        nodes_[id]->status.store(TaskStatus::Ready, std::memory_order_release);
        pool.submit([this, id, &pool] {
            if (state_->cancelled.load(std::memory_order_acquire)) {
                nodes_[id]->status.store(TaskStatus::Cancelled, std::memory_order_release);
                cascade_cancel(id);
                return;
            }

            bool expected = false;
            if (!nodes_[id]->executed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                return; // Guard against duplicate execution
            }

            nodes_[id]->status.store(TaskStatus::Running, std::memory_order_release);
            try {
                nodes_[id]->func();
                nodes_[id]->status.store(TaskStatus::Completed, std::memory_order_release);
                on_task_completed(id, pool);
            } catch (...) {
                nodes_[id]->status.store(TaskStatus::Failed, std::memory_order_release);
                nodes_[id]->exception = std::current_exception();
                on_task_failed(id, nodes_[id]->exception, pool);
            }
        });
    }
}

void TaskGraph::on_task_completed(TaskID id, ThreadPool& pool) {
    for (TaskID child_id : nodes_[id]->dependents) {
        if (state_->cancelled.load(std::memory_order_acquire)) {
            continue;
        }

        size_t prev = nodes_[child_id]->remaining_dependencies.fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 1) { // was 1, now 0: all dependencies satisfied!
            nodes_[child_id]->status.store(TaskStatus::Ready, std::memory_order_release);
            pool.submit([this, child_id, &pool] {
                if (state_->cancelled.load(std::memory_order_acquire)) {
                    nodes_[child_id]->status.store(TaskStatus::Cancelled, std::memory_order_release);
                    cascade_cancel(child_id);
                    return;
                }

                bool expected = false;
                if (!nodes_[child_id]->executed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                    return; // Ensure each task executes at most once
                }

                nodes_[child_id]->status.store(TaskStatus::Running, std::memory_order_release);
                try {
                    nodes_[child_id]->func();
                    nodes_[child_id]->status.store(TaskStatus::Completed, std::memory_order_release);
                    on_task_completed(child_id, pool);
                } catch (...) {
                    nodes_[child_id]->status.store(TaskStatus::Failed, std::memory_order_release);
                    nodes_[child_id]->exception = std::current_exception();
                    on_task_failed(child_id, nodes_[child_id]->exception, pool);
                }
            });
        }
    }

    size_t finished = state_->completed_or_cancelled_count.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (finished == nodes_.size()) {
        std::unique_lock<std::mutex> lock(state_->wait_mutex);
        state_->cv_all_done.notify_all();
    }
}

void TaskGraph::cascade_cancel(TaskID id) {
    std::queue<TaskID> q;
    for (TaskID child_id : nodes_[id]->dependents) {
        q.push(child_id);
    }

    while (!q.empty()) {
        TaskID curr = q.front();
        q.pop();

        TaskStatus current_status = nodes_[curr]->status.load(std::memory_order_acquire);
        if (current_status == TaskStatus::Pending || current_status == TaskStatus::Ready) {
            nodes_[curr]->status.store(TaskStatus::Cancelled, std::memory_order_release);
            nodes_[curr]->exception = std::make_exception_ptr(
                DependencyFailedException("Dependent task cancelled due to upstream cancellation or failure")
            );
            state_->completed_or_cancelled_count.fetch_add(1, std::memory_order_acq_rel);

            for (TaskID next_child : nodes_[curr]->dependents) {
                q.push(next_child);
            }
        }
    }
}

void TaskGraph::on_task_failed(TaskID id, std::exception_ptr ep, ThreadPool& pool) {
    cascade_cancel(id);

    size_t finished = state_->completed_or_cancelled_count.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (finished == nodes_.size()) {
        std::unique_lock<std::mutex> lock(state_->wait_mutex);
        state_->cv_all_done.notify_all();
    }
}

void TaskGraph::wait() {
    std::unique_lock<std::mutex> lock(state_->wait_mutex);
    state_->cv_all_done.wait(lock, [this] {
        return state_->completed_or_cancelled_count.load(std::memory_order_acquire) >= nodes_.size();
    });
}

void TaskGraph::cancel() {
    state_->cancelled.store(true, std::memory_order_release);
    for (size_t i = 0; i < nodes_.size(); ++i) {
        TaskStatus s = nodes_[i]->status.load(std::memory_order_acquire);
        if (s == TaskStatus::Pending || s == TaskStatus::Ready) {
            nodes_[i]->status.store(TaskStatus::Cancelled, std::memory_order_release);
            state_->completed_or_cancelled_count.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    std::unique_lock<std::mutex> lock(state_->wait_mutex);
    state_->cv_all_done.notify_all();
}

TaskStatus TaskGraph::get_status(TaskID id) const {
    if (id >= nodes_.size()) throw std::out_of_range("TaskID out of range");
    return nodes_[id]->status.load(std::memory_order_acquire);
}

std::exception_ptr TaskGraph::get_exception(TaskID id) const {
    if (id >= nodes_.size()) throw std::out_of_range("TaskID out of range");
    return nodes_[id]->exception;
}

const std::string& TaskGraph::get_name(TaskID id) const {
    if (id >= nodes_.size()) throw std::out_of_range("TaskID out of range");
    return nodes_[id]->name;
}

void TaskGraph::throw_if_failed() const {
    for (const auto& node : nodes_) {
        if (node->status.load(std::memory_order_acquire) == TaskStatus::Failed && node->exception) {
            std::rethrow_exception(node->exception);
        }
    }
}

bool TaskGraph::is_completed() const {
    return state_->completed_or_cancelled_count.load(std::memory_order_acquire) >= nodes_.size();
}

} // namespace threadforge
