#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <sstream>
#include <stdexcept>

namespace testfw {

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> func;
};

inline std::vector<TestCase>& get_registry() {
    static std::vector<TestCase> registry;
    return registry;
}

struct TestRegistrar {
    TestRegistrar(const std::string& suite, const std::string& name, std::function<void()> func) {
        get_registry().push_back({suite, name, std::move(func)});
    }
};

#define TEST(suite, name) \
    void test_##suite##_##name(); \
    static ::testfw::TestRegistrar reg_##suite##_##name(#suite, #name, test_##suite##_##name); \
    void test_##suite##_##name()

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            std::ostringstream ss; \
            ss << "Assertion failed: (" #cond ") at " << __FILE__ << ":" << __LINE__; \
            throw std::runtime_error(ss.str()); \
        } \
    } while (0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(val1, val2) \
    do { \
        if (!((val1) == (val2))) { \
            std::ostringstream ss; \
            ss << "Assertion failed: (" #val1 " == " #val2 ") [" << (val1) << " != " << (val2) << "] at " << __FILE__ << ":" << __LINE__; \
            throw std::runtime_error(ss.str()); \
        } \
    } while (0)

#define ASSERT_NE(val1, val2) \
    do { \
        if (!((val1) != (val2))) { \
            std::ostringstream ss; \
            ss << "Assertion failed: (" #val1 " != " #val2 ") at " << __FILE__ << ":" << __LINE__; \
            throw std::runtime_error(ss.str()); \
        } \
    } while (0)

#define ASSERT_THROW(expr, ExceptionType) \
    do { \
        bool caught = false; \
        try { \
            expr; \
        } catch (const ExceptionType&) { \
            caught = true; \
        } catch (...) { \
            std::ostringstream ss; \
            ss << "Expected exception " #ExceptionType " but caught different exception at " << __FILE__ << ":" << __LINE__; \
            throw std::runtime_error(ss.str()); \
        } \
        if (!caught) { \
            std::ostringstream ss; \
            ss << "Expected exception " #ExceptionType " was not thrown at " << __FILE__ << ":" << __LINE__; \
            throw std::runtime_error(ss.str()); \
        } \
    } while (0)

#define ASSERT_NO_THROW(expr) \
    do { \
        try { \
            expr; \
        } catch (const std::exception& e) { \
            std::ostringstream ss; \
            ss << "Expected no exception, but caught: " << e.what() << " at " << __FILE__ << ":" << __LINE__; \
            throw std::runtime_error(ss.str()); \
        } catch (...) { \
            std::ostringstream ss; \
            ss << "Expected no exception, but caught unknown exception at " << __FILE__ << ":" << __LINE__; \
            throw std::runtime_error(ss.str()); \
        } \
    } while (0)

inline int run_all_tests() {
    auto& tests = get_registry();
    std::cout << "=========================================================\n";
    std::cout << " [ThreadForge Test Suite] Running " << tests.size() << " tests...\n";
    std::cout << "=========================================================\n";

    size_t passed = 0;
    size_t failed = 0;

    auto total_start = std::chrono::steady_clock::now();

    for (const auto& test : tests) {
        std::cout << "[ RUN      ] " << test.suite << "." << test.name << std::endl;
        auto start = std::chrono::steady_clock::now();
        try {
            test.func();
            auto end = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            std::cout << "[       OK ] " << test.suite << "." << test.name << " (" << (static_cast<double>(ms) / 1000.0) << " ms)" << std::endl;
            passed++;
        } catch (const std::exception& e) {
            auto end = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            std::cout << "[  FAILED  ] " << test.suite << "." << test.name << " (" << (static_cast<double>(ms) / 1000.0) << " ms)" << std::endl;
            std::cout << "             Error: " << e.what() << std::endl;
            failed++;
        } catch (...) {
            std::cout << "[  FAILED  ] " << test.suite << "." << test.name << " (Unknown failure)" << std::endl;
            failed++;
        }
    }

    auto total_end = std::chrono::steady_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(total_end - total_start).count();

    std::cout << "=========================================================\n";
    std::cout << " [Summary] Total: " << tests.size() 
              << " | Passed: " << passed 
              << " | Failed: " << failed 
              << " | Time: " << total_ms << " ms\n";
    std::cout << "=========================================================\n";

    return (failed == 0) ? 0 : 1;
}

} // namespace testfw
