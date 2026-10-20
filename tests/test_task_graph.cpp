#include "test_framework.hpp"
#include "threadforge/task_graph.hpp"
#include <atomic>
#include <chrono>

using namespace threadforge;

TEST(TaskGraphTest, DiamondDAGExecutionOrder) {
    ThreadPool pool(4);
    TaskGraph graph;

    std::atomic<bool> a_done{false};
    std::atomic<bool> b_done{false};
    std::atomic<bool> c_done{false};
    std::atomic<bool> d_done{false};

    // A: Load data
    TaskID tA = graph.add_task("A_Load", [&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        a_done.store(true);
    });

    // B: Transform data (depends on A)
    TaskID tB = graph.add_task("B_Transform", [&] {
        ASSERT_TRUE(a_done.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        b_done.store(true);
    });

    // C: Process data (depends on A)
    TaskID tC = graph.add_task("C_Process", [&] {
        ASSERT_TRUE(a_done.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        c_done.store(true);
    });

    // D: Combine results (depends on B and C)
    TaskID tD = graph.add_task("D_Combine", [&] {
        ASSERT_TRUE(b_done.load());
        ASSERT_TRUE(c_done.load());
        d_done.store(true);
    });

    graph.add_dependency(tA, tB);
    graph.add_dependency(tA, tC);
    graph.add_dependency(tB, tD);
    graph.add_dependency(tC, tD);

    ASSERT_FALSE(graph.has_cycle());

    graph.execute(pool);
    graph.wait();

    ASSERT_TRUE(d_done.load());
    ASSERT_EQ(graph.get_status(tA), TaskStatus::Completed);
    ASSERT_EQ(graph.get_status(tB), TaskStatus::Completed);
    ASSERT_EQ(graph.get_status(tC), TaskStatus::Completed);
    ASSERT_EQ(graph.get_status(tD), TaskStatus::Completed);
}

TEST(TaskGraphTest, CycleDetectionSelfLoop) {
    TaskGraph graph;
    TaskID t1 = graph.add_task("SelfLoop", [] {});
    ASSERT_THROW(graph.add_dependency(t1, t1), CycleDetectedException);
}

TEST(TaskGraphTest, CycleDetectionMultiNode) {
    TaskGraph graph;
    TaskID tA = graph.add_task("A", [] {});
    TaskID tB = graph.add_task("B", [] {});
    TaskID tC = graph.add_task("C", [] {});

    graph.add_dependency(tA, tB);
    graph.add_dependency(tB, tC);
    graph.add_dependency(tC, tA); // Cycle!

    ASSERT_TRUE(graph.has_cycle());
    ASSERT_THROW(graph.topological_sort(), CycleDetectedException);

    ThreadPool pool(2);
    ASSERT_THROW(graph.execute(pool), CycleDetectedException);
}

TEST(TaskGraphTest, ExceptionPropagationAndCascadeCancel) {
    ThreadPool pool(4);
    TaskGraph graph;

    std::atomic<bool> b_ran{false};
    std::atomic<bool> c_ran{false};

    TaskID tA = graph.add_task("A_Failing", [] {
        throw std::runtime_error("Simulated PTX instruction syntax error");
    });

    TaskID tB = graph.add_task("B_Downstream", [&b_ran] {
        b_ran.store(true);
    });

    TaskID tC = graph.add_task("C_DownstreamOfB", [&c_ran] {
        c_ran.store(true);
    });

    graph.add_dependency(tA, tB);
    graph.add_dependency(tB, tC);

    graph.execute(pool);
    graph.wait();

    ASSERT_EQ(graph.get_status(tA), TaskStatus::Failed);
    ASSERT_EQ(graph.get_status(tB), TaskStatus::Cancelled);
    ASSERT_EQ(graph.get_status(tC), TaskStatus::Cancelled);

    ASSERT_FALSE(b_ran.load());
    ASSERT_FALSE(c_ran.load());

    ASSERT_THROW(graph.throw_if_failed(), std::runtime_error);
}

TEST(TaskGraphTest, ExecuteAtMostOnce) {
    ThreadPool pool(4);
    TaskGraph graph;

    std::atomic<int> exec_count{0};
    TaskID root = graph.add_task("Root", [&] {
        exec_count.fetch_add(1);
    });

    std::vector<TaskID> leaves;
    for (int i = 0; i < 20; ++i) {
        TaskID leaf = graph.add_task("Leaf_" + std::to_string(i), [] {});
        graph.add_dependency(root, leaf);
        leaves.push_back(leaf);
    }

    graph.execute(pool);
    graph.wait();

    ASSERT_EQ(exec_count.load(), 1);
}

TEST(TaskGraphTest, EmptyGraphSafe) {
    ThreadPool pool(2);
    TaskGraph graph;
    ASSERT_FALSE(graph.has_cycle());
    ASSERT_NO_THROW(graph.execute(pool));
    ASSERT_NO_THROW(graph.wait());
}
