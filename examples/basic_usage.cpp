#include <iostream>
#include <chrono>
#include "threadforge/thread_pool.hpp"

using namespace threadforge;

int main() {
    std::cout << "--- ThreadForge Basic Usage Example ---\n";

    ThreadPool pool(4);

    // 1. Submit value-returning task
    auto fut1 = pool.submit([](int a, int b) {
        return a * b;
    }, 6, 7);

    // 2. Submit void-returning task
    auto fut2 = pool.submit([] {
        std::cout << "[Task] Hello from worker thread: " 
                  << std::this_thread::get_id() << "\n";
    });

    // 3. Submit task that catches an exception
    auto fut3 = pool.submit([] {
        throw std::runtime_error("Simulated parsing failure");
    });

    fut2.get();
    std::cout << "[Result] 6 * 7 = " << fut1.get() << "\n";

    try {
        fut3.get();
    } catch (const std::exception& e) {
        std::cout << "[Caught Expected Exception] " << e.what() << "\n";
    }

    pool.wait_idle();
    std::cout << "\n" << pool.metrics().summary() << "\n";
    return 0;
}
