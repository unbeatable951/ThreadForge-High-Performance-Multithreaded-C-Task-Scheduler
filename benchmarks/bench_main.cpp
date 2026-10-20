#include <iostream>
#include <vector>
#include <chrono>
#include <numeric>
#include <cmath>
#include <iomanip>
#include <functional>
#include <algorithm>

#include "threadforge/thread_pool.hpp"
#include "threadforge/task_graph.hpp"
#include "threadforge/work_stealing_pool.hpp"
#include "threadforge/cache_alignment.hpp"

using namespace threadforge;
using Clock = std::chrono::steady_clock;

// Workload kernels
uint64_t cpu_heavy_fibonacci(uint64_t n) {
    if (n <= 1) return n;
    uint64_t a = 0, b = 1;
    for (uint64_t i = 2; i <= n; ++i) {
        uint64_t c = a + b;
        a = b;
        b = c;
    }
    return b;
}

// Simulated irregular / jittered workload
void irregular_workload(int task_id) {
    if (task_id % 7 == 0) {
        // Heavy spike
        volatile uint64_t acc = 0;
        for (int i = 0; i < 20'000; ++i) acc += (i * 3) ^ (i >> 2);
    } else {
        // Lightweight
        volatile uint64_t acc = 0;
        for (int i = 0; i < 500; ++i) acc += i;
    }
}

// Parallel Matrix Multiplication Kernel (N x N)
void parallel_matrix_multiply(size_t n, size_t num_threads) {
    std::vector<float> A(n * n, 1.0f);
    std::vector<float> B(n * n, 2.0f);
    std::vector<float> C(n * n, 0.0f);

    ThreadPool pool(num_threads);
    size_t chunk_size = (n + num_threads - 1) / num_threads;
    std::vector<std::future<void>> futures;

    for (size_t t = 0; t < num_threads; ++t) {
        size_t row_start = t * chunk_size;
        size_t row_end = std::min(row_start + chunk_size, n);
        if (row_start >= row_end) break;

        futures.push_back(pool.submit([row_start, row_end, n, &A, &B, &C] {
            for (size_t i = row_start; i < row_end; ++i) {
                for (size_t k = 0; k < n; ++k) {
                    float r = A[i * n + k];
                    for (size_t j = 0; j < n; ++j) {
                        C[i * n + j] += r * B[k * n + j];
                    }
                }
            }
        }));
    }

    for (auto& f : futures) {
        f.get();
    }
}

void run_false_sharing_benchmark() {
    std::cout << "\n=========================================================\n";
    std::cout << " [Benchmark 1] Cache Contention & False Sharing Analysis\n";
    std::cout << "=========================================================\n";
    
    constexpr int kThreads = 4;
    constexpr uint64_t kIterations = 10'000'000;

    // 1. Unaligned counters sharing a cache line
    UnalignedCounter unaligned[kThreads];
    auto t1 = Clock::now();
    {
        std::vector<std::thread> workers;
        for (int i = 0; i < kThreads; ++i) {
            workers.emplace_back([&unaligned, i] {
                for (uint64_t iter = 0; iter < kIterations; ++iter) {
                    unaligned[i].counter.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& w : workers) w.join();
    }
    auto t2 = Clock::now();
    double unaligned_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();

    // 2. Cache-line aligned counters (isolated cache lines)
    AlignedCounter aligned[kThreads];
    auto t3 = Clock::now();
    {
        std::vector<std::thread> workers;
        for (int i = 0; i < kThreads; ++i) {
            workers.emplace_back([&aligned, i] {
                for (uint64_t iter = 0; iter < kIterations; ++iter) {
                    aligned[i].counter.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& w : workers) w.join();
    }
    auto t4 = Clock::now();
    double aligned_ms = std::chrono::duration<double, std::milli>(t4 - t3).count();

    std::cout << "Unaligned (False Sharing Contention): " << std::fixed << std::setprecision(2) << unaligned_ms << " ms\n";
    std::cout << "Aligned   (Cache Line Isolation):     " << aligned_ms << " ms\n";
    if (unaligned_ms > aligned_ms) {
        std::cout << "-> Speedup from Cache Alignment:      " << (unaligned_ms / aligned_ms) << "x faster!\n";
    } else {
        std::cout << "-> Note: Cache line contention variance influenced by CPU topology/L3 cache coherence.\n";
    }
}

void run_scheduler_comparison_benchmark(size_t num_threads) {
    std::cout << "\n=========================================================\n";
    std::cout << " [Benchmark 2] Multithreaded Schedulers Comparison\n";
    std::cout << " Threads: " << num_threads << " | Task Count: 100,000\n";
    std::cout << "=========================================================\n";

    constexpr size_t kTasks = 100'000;

    // 1. Sequential Baseline
    auto s1 = Clock::now();
    for (size_t i = 0; i < kTasks; ++i) {
        cpu_heavy_fibonacci(5000);
    }
    auto s2 = Clock::now();
    double seq_ms = std::chrono::duration<double, std::milli>(s2 - s1).count();
    std::cout << "1. Sequential Baseline:               " << std::fixed << std::setprecision(2) 
              << seq_ms << " ms | " << (kTasks / (seq_ms / 1000.0)) << " tasks/sec\n";

    // 2. Shared-Queue Thread Pool
    {
        ThreadPool pool(num_threads);
        auto t1 = Clock::now();
        std::vector<std::future<void>> futures;
        futures.reserve(kTasks);
        for (size_t i = 0; i < kTasks; ++i) {
            futures.push_back(pool.submit([] {
                cpu_heavy_fibonacci(5000);
            }));
        }
        for (auto& f : futures) {
            f.get();
        }
        auto t2 = Clock::now();
        double pool_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
        double speedup = seq_ms / pool_ms;
        std::cout << "2. Shared-Queue ThreadPool:          " << pool_ms << " ms | " 
                  << (kTasks / (pool_ms / 1000.0)) << " tasks/sec | Speedup: " << speedup << "x\n";
    }

    // 3. Work-Stealing Scheduler
    {
        WorkStealingPool ws_pool(num_threads);
        auto t1 = Clock::now();
        std::vector<std::future<void>> futures;
        futures.reserve(kTasks);
        for (size_t i = 0; i < kTasks; ++i) {
            futures.push_back(ws_pool.submit([] {
                cpu_heavy_fibonacci(5000);
            }));
        }
        for (auto& f : futures) {
            f.get();
        }
        auto t2 = Clock::now();
        double ws_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
        double speedup = seq_ms / ws_ms;
        std::cout << "3. Work-Stealing Pool:               " << ws_ms << " ms | " 
                  << (kTasks / (ws_ms / 1000.0)) << " tasks/sec | Speedup: " << speedup << "x\n";
        std::cout << "   (Steal attempts: " << ws_pool.steal_attempts() 
                  << ", Successful steals: " << ws_pool.successful_steals() << ")\n";
    }
}

void run_irregular_workload_benchmark(size_t num_threads) {
    std::cout << "\n=========================================================\n";
    std::cout << " [Benchmark 3] Uneven / Irregular Workload Imbalance\n";
    std::cout << " Tasks: 20,000 with Periodic Duration Spikes\n";
    std::cout << "=========================================================\n";

    constexpr size_t kTasks = 20'000;

    // Shared Queue
    double pool_ms = 0.0;
    {
        ThreadPool pool(num_threads);
        auto t1 = Clock::now();
        std::vector<std::future<void>> futures;
        futures.reserve(kTasks);
        for (size_t i = 0; i < kTasks; ++i) {
            futures.push_back(pool.submit([i] {
                irregular_workload(static_cast<int>(i));
            }));
        }
        for (auto& f : futures) f.get();
        auto t2 = Clock::now();
        pool_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    }

    // Work-Stealing Queue
    double ws_ms = 0.0;
    {
        WorkStealingPool ws_pool(num_threads);
        auto t1 = Clock::now();
        std::vector<std::future<void>> futures;
        futures.reserve(kTasks);
        for (size_t i = 0; i < kTasks; ++i) {
            futures.push_back(ws_pool.submit([i] {
                irregular_workload(static_cast<int>(i));
            }));
        }
        for (auto& f : futures) f.get();
        auto t2 = Clock::now();
        ws_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    }

    std::cout << "Shared-Queue ThreadPool:  " << std::fixed << std::setprecision(2) << pool_ms << " ms\n";
    std::cout << "Work-Stealing Scheduler:  " << ws_ms << " ms\n";
    std::cout << "Work-Stealing vs Shared:  " << (pool_ms / ws_ms) << "x speedup in irregular conditions\n";
}

void run_matrix_multiply_benchmark(size_t num_threads) {
    std::cout << "\n=========================================================\n";
    std::cout << " [Benchmark 4] Numerical Workload: Parallel Matrix Mult\n";
    std::cout << " Matrix Size: 512 x 512 Float32\n";
    std::cout << "=========================================================\n";

    constexpr size_t kSize = 512;

    auto t1 = Clock::now();
    parallel_matrix_multiply(kSize, 1);
    auto t2 = Clock::now();
    double single_thread_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();

    auto t3 = Clock::now();
    parallel_matrix_multiply(kSize, num_threads);
    auto t4 = Clock::now();
    double multi_thread_ms = std::chrono::duration<double, std::milli>(t4 - t3).count();

    std::cout << "Single-Threaded Execution: " << std::fixed << std::setprecision(2) << single_thread_ms << " ms\n";
    std::cout << "Multi-Threaded (" << num_threads << " workers):  " << multi_thread_ms << " ms\n";
    std::cout << "Parallel Speedup:          " << (single_thread_ms / multi_thread_ms) << "x\n";
}

int main() {
    unsigned int hw = std::thread::hardware_concurrency();
    size_t test_threads = (hw > 0) ? hw : 4;

    std::cout << "=========================================================\n";
    std::cout << "  ThreadForge Comprehensive Performance Benchmark Suite  \n";
    std::cout << "=========================================================\n";
    std::cout << "Hardware Concurrency (Cores/Threads): " << hw << "\n";
    std::cout << "Cache Line Alignment:                 " << kCacheLineSize << " bytes\n";
    std::cout << "Compiler C++ Standard:                C++20\n";
    std::cout << "=========================================================\n";

    run_false_sharing_benchmark();
    run_scheduler_comparison_benchmark(test_threads);
    run_irregular_workload_benchmark(test_threads);
    run_matrix_multiply_benchmark(test_threads);

    std::cout << "\nAll benchmarks completed successfully.\n";
    return 0;
}
