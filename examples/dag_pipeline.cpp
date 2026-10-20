#include <iostream>
#include <chrono>
#include "threadforge/task_graph.hpp"

using namespace threadforge;

int main() {
    std::cout << "--- ThreadForge Task Graph DAG Pipeline Example ---\n";

    ThreadPool pool(4);
    TaskGraph graph;

    // Task A: Load data
    TaskID tA = graph.add_task("Task A: Load Data", [] {
        std::cout << "[Step 1] Loading raw input dataset...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });

    // Task B: Transform data (depends on A)
    TaskID tB = graph.add_task("Task B: Transform Data", [] {
        std::cout << "[Step 2A] Transforming feature vectors...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    });

    // Task C: Process data (depends on A)
    TaskID tC = graph.add_task("Task C: Process Data", [] {
        std::cout << "[Step 2B] Processing graph topology...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    });

    // Task D: Combine results (depends on B and C)
    TaskID tD = graph.add_task("Task D: Combine Results", [] {
        std::cout << "[Step 3] Combining transformation and graph results into final model output!\n";
    });

    graph.add_dependency(tA, tB);
    graph.add_dependency(tA, tC);
    graph.add_dependency(tB, tD);
    graph.add_dependency(tC, tD);

    std::cout << "Topological Order of Execution:\n";
    for (TaskID id : graph.topological_sort()) {
        std::cout << " -> " << graph.get_name(id) << "\n";
    }

    std::cout << "\nExecuting DAG...\n";
    graph.execute(pool);
    graph.wait();

    std::cout << "\nDAG execution finished successfully!\n";
    return 0;
}
