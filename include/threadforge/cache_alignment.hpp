#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <atomic>

namespace threadforge {

// Hardware destructive interference size (false sharing cache line size).
// Portable fallback to 64 bytes if compiler or platform does not define it.
#if defined(__cpp_lib_hardware_interference_size) && !defined(__clang__) && !defined(_MSC_VER)
    inline constexpr std::size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
    inline constexpr std::size_t kCacheLineSize = 64;
#endif

/**
 * @brief CacheAligned wraps any type T and forces alignment to the L1 cache-line size (typically 64 bytes).
 * This prevents false sharing when multiple threads write to adjacent elements in an array or struct.
 */
template <typename T>
struct alignas(kCacheLineSize) CacheAligned {
    T value;

    constexpr CacheAligned() : value{} {}
    constexpr explicit CacheAligned(T val) : value(std::move(val)) {}

    // Convenience operators
    T& get() noexcept { return value; }
    const T& get() const noexcept { return value; }

    T& operator*() noexcept { return value; }
    const T& operator*() const noexcept { return value; }

    T* operator->() noexcept { return &value; }
    const T* operator->() const noexcept { return &value; }

    operator T&() noexcept { return value; }
    operator const T&() const noexcept { return value; }
};

/**
 * @brief Structure specifically designed for microbenchmarking false sharing:
 * UnalignedCounter vs AlignedCounter in an array.
 */
struct UnalignedCounter {
    std::atomic<uint64_t> counter{0};
};

struct alignas(kCacheLineSize) AlignedCounter {
    std::atomic<uint64_t> counter{0};
    // Padding to ensure isolation if size < cache line
    uint8_t padding[kCacheLineSize - sizeof(std::atomic<uint64_t>)]{};
};

} // namespace threadforge
