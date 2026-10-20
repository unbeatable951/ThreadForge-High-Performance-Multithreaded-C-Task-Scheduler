#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <iostream>
#include <sstream>
#include "threadforge/task_graph.hpp"

namespace threadforge {

/**
 * @brief Representation of an individual compilation unit / GPU kernel.
 */
struct CompilationUnit {
    std::string name;
    size_t lines_of_ptx;
    size_t optimization_level; // 0 to 3
};

/**
 * @brief Realistic simulation of compiler pipeline stages executed by the PTX Compiler team:
 *
 *  Stage 1: PTX Syntax & Semantic Parsing
 *           AST construction, symbol table resolution.
 *  Stage 2: Mid-Level Intermediate Representation (IR) Optimization
 *           Inlining, Dead Code Elimination (DCE), Loop Unrolling.
 *  Stage 3: Register Allocation (Chaitin-Briggs Graph Coloring / Linear Scan)
 *           Spill code insertion, live-range splitting.
 *  Stage 4: Instruction Scheduling & Hazard Minimization
 *           Dependency tracking, scoreboard latency hiding, dual-issue packing.
 *  Stage 5: Object Emission & Linking (SASS machine binary / cubin container)
 */
class PTXCompilerPipeline {
public:
    static TaskGraph build_pipeline(const std::vector<CompilationUnit>& units,
                                    std::vector<std::string>& log_output,
                                    std::mutex& log_mutex) {
        TaskGraph graph;
        std::vector<TaskID> codegen_tasks;

        for (const auto& unit : units) {
            // Stage 1: PTX Parsing
            TaskID parse_id = graph.add_task("Parse_" + unit.name, [unit, &log_output, &log_mutex] {
                // Simulate parsing workload proportional to lines
                std::this_thread::sleep_for(std::chrono::microseconds(unit.lines_of_ptx * 5));
                std::lock_guard<std::mutex> lock(log_mutex);
                log_output.push_back("[PTX Parse] " + unit.name + " (" + std::to_string(unit.lines_of_ptx) + " instructions)");
            });

            // Stage 2: IR Optimization (depends on Parse)
            TaskID opt_id = graph.add_task("Opt_" + unit.name, [unit, &log_output, &log_mutex] {
                std::this_thread::sleep_for(std::chrono::microseconds(unit.lines_of_ptx * 10 * (unit.optimization_level + 1)));
                std::lock_guard<std::mutex> lock(log_mutex);
                log_output.push_back("[IR Optimizer] " + unit.name + " (Level -O" + std::to_string(unit.optimization_level) + ")");
            });
            graph.add_dependency(parse_id, opt_id);

            // Stage 3: Register Allocation (depends on IR Opt)
            TaskID regalloc_id = graph.add_task("RegAlloc_" + unit.name, [unit, &log_output, &log_mutex] {
                std::this_thread::sleep_for(std::chrono::microseconds(unit.lines_of_ptx * 8));
                std::lock_guard<std::mutex> lock(log_mutex);
                log_output.push_back("[Register Alloc] " + unit.name + " (Graph coloring, 64 physical registers)");
            });
            graph.add_dependency(opt_id, regalloc_id);

            // Stage 4: Instruction Scheduling (depends on RegAlloc)
            TaskID sched_id = graph.add_task("InstrSched_" + unit.name, [unit, &log_output, &log_mutex] {
                std::this_thread::sleep_for(std::chrono::microseconds(unit.lines_of_ptx * 6));
                std::lock_guard<std::mutex> lock(log_mutex);
                log_output.push_back("[Instr Scheduling] " + unit.name + " (Latency hiding & dual-issue packing)");
            });
            graph.add_dependency(regalloc_id, sched_id);

            // Stage 5: Machine Code Generation (depends on InstrSched)
            TaskID codegen_id = graph.add_task("CodeGen_" + unit.name, [unit, &log_output, &log_mutex] {
                std::this_thread::sleep_for(std::chrono::microseconds(unit.lines_of_ptx * 4));
                std::lock_guard<std::mutex> lock(log_mutex);
                log_output.push_back("[CodeGen SASS] " + unit.name + " (Emitted machine binary)");
            });
            graph.add_dependency(sched_id, codegen_id);

            codegen_tasks.push_back(codegen_id);
        }

        // Final Stage: Linker & Device ELF Generation (depends on ALL Codegen tasks)
        TaskID link_id = graph.add_task("DeviceLinker", [&log_output, &log_mutex] {
            std::this_thread::sleep_for(std::chrono::microseconds(2000));
            std::lock_guard<std::mutex> lock(log_mutex);
            log_output.push_back("[Device Linker] Assembled final .cubin fatbinary image");
        });

        for (TaskID cg_id : codegen_tasks) {
            graph.add_dependency(cg_id, link_id);
        }

        return graph;
    }
};

} // namespace threadforge
