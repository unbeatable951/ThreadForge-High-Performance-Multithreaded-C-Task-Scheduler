#include <iostream>
#include <vector>
#include <mutex>
#include "threadforge/compiler_pipeline.hpp"

using namespace threadforge;

int main() {
    std::cout << "=========================================================\n";
    std::cout << " ThreadForge PTX Compiler Pipeline Execution Simulation  \n";
    std::cout << " (Simulating parallel compilation in NVIDIA PTXAS runtime)\n";
    std::cout << "=========================================================\n";

    ThreadPool pool(4);
    std::vector<std::string> log_output;
    std::mutex log_mutex;

    std::vector<CompilationUnit> units = {
        {"vector_add_kernel.ptx", 120, 2},
        {"matrix_gemm_kernel.ptx", 480, 3},
        {"layer_norm_kernel.ptx", 210, 2},
        {"flash_attention_kernel.ptx", 650, 3}
    };

    std::cout << "Submitted Compilation Units:\n";
    for (const auto& u : units) {
        std::cout << " - " << u.name << " (" << u.lines_of_ptx << " PTX instructions, Opt Level: -O" << u.optimization_level << ")\n";
    }

    std::cout << "\nBuilding Stage Dependency DAG...\n";
    TaskGraph pipeline = PTXCompilerPipeline::build_pipeline(units, log_output, log_mutex);
    std::cout << "Total DAG Nodes: " << pipeline.size() << "\n";

    std::cout << "Executing PTX Compilation Pipeline across " << pool.worker_count() << " workers...\n";
    auto start = Clock::now();
    pipeline.execute(pool);
    pipeline.wait();
    auto end = Clock::now();

    double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();

    std::cout << "\nCompilation Pipeline Trace:\n";
    for (const auto& line : log_output) {
        std::cout << "  " << line << "\n";
    }

    std::cout << "\nTotal Compilation Time: " << elapsed_ms << " ms\n";
    std::cout << pool.metrics().summary() << "\n";

    return 0;
}
