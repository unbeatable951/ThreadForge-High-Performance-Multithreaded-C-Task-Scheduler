cmake_minimum_required(VERSION 3.20)
project(ThreadForge VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for language servers
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Sanitizer Options
option(ENABLE_ASAN  "Enable AddressSanitizer (ASan)" OFF)
option(ENABLE_TSAN  "Enable ThreadSanitizer (TSan)" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer (UBSan)" OFF)

# Include directories
include_directories(${PROJECT_SOURCE_DIR}/include)

# Threading support
find_package(Threads REQUIRED)

# Common compiler warnings & flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wno-interference-size
        -Wno-unused-parameter
        -pthread
    )
    if(ENABLE_ASAN)
        message(STATUS "AddressSanitizer enabled")
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
    if(ENABLE_TSAN)
        message(STATUS "ThreadSanitizer enabled")
        add_compile_options(-fsanitize=thread)
        add_link_options(-fsanitize=thread)
    endif()
    if(ENABLE_UBSAN)
        message(STATUS "UndefinedBehaviorSanitizer enabled")
        add_compile_options(-fsanitize=undefined)
        add_link_options(-fsanitize=undefined)
    endif()
elseif(MSVC)
    add_compile_options(/W4 /permissive-)
endif()

# Core static library
add_library(threadforge_core STATIC
    src/thread_pool.cpp
    src/task_graph.cpp
    src/work_stealing_pool.cpp
    src/metrics.cpp
)

target_include_directories(threadforge_core PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(threadforge_core PUBLIC Threads::Threads)

# Test Suite Executable
add_executable(threadforge_tests
    tests/test_main.cpp
    tests/test_thread_pool.cpp
    tests/test_task_graph.cpp
    tests/test_work_stealing.cpp
    tests/test_cache_alignment.cpp
)
target_include_directories(threadforge_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(threadforge_tests PRIVATE threadforge_core)

enable_testing()
add_test(NAME AllTests COMMAND threadforge_tests)

# Benchmark Executable
add_executable(threadforge_bench
    benchmarks/bench_main.cpp
)
target_link_libraries(threadforge_bench PRIVATE threadforge_core)

# Examples
add_executable(example_basic_usage
    examples/basic_usage.cpp
)
target_link_libraries(example_basic_usage PRIVATE threadforge_core)

add_executable(example_dag_pipeline
    examples/dag_pipeline.cpp
)
target_link_libraries(example_dag_pipeline PRIVATE threadforge_core)

add_executable(example_ptx_simulation
    examples/ptx_compiler_simulation.cpp
)
target_link_libraries(example_ptx_simulation PRIVATE threadforge_core)

# Print configuration summary
message(STATUS "--------------------------------------------------------")
message(STATUS "ThreadForge Configuration Summary")
message(STATUS "  C++ Standard:        ${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler:            ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build Type:          ${CMAKE_BUILD_TYPE}")
message(STATUS "  AddressSanitizer:    ${ENABLE_ASAN}")
message(STATUS "  ThreadSanitizer:     ${ENABLE_TSAN}")
message(STATUS "  UBSanitizer:         ${ENABLE_UBSAN}")
message(STATUS "--------------------------------------------------------")
