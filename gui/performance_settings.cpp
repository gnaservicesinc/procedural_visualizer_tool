#include "performance_settings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__APPLE__)
#  include <sys/sysctl.h>
#else
#  include <unistd.h>
#endif

namespace {

constexpr std::size_t kMebibyte = std::size_t{1024U} * 1024U;
constexpr std::size_t kGibibyte = kMebibyte * 1024U;

std::size_t bounded_bytes(long double requested) noexcept {
    if (std::isnan(requested) || requested <= 0.0L) return 0U;
    const long double maximum = static_cast<long double>(
        (std::numeric_limits<std::size_t>::max)());
    if (std::isinf(requested) || requested >= maximum) {
        return (std::numeric_limits<std::size_t>::max)();
    }
    return static_cast<std::size_t>(std::floor(requested + 0.5L));
}

std::size_t automatic_memory_budget(std::size_t physical) noexcept {
    if (physical == 0U) return std::size_t{2U} * kGibibyte;
    // One quarter scales well on unified-memory workstations while leaving
    // substantial headroom for the OS, encoders, and other creative tools.
    // Small machines may use less than the historical 2 GiB default rather
    // than forcing paging merely to satisfy a nominal concurrency budget.
    const std::size_t quarter = physical / 4U;
    return std::min(physical, std::max(std::size_t{512U} * kMebibyte,
                                      quarter));
}

} // namespace

std::size_t total_physical_memory_bytes() noexcept {
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) == 0) return 0U;
    const auto maximum = static_cast<unsigned long long>(
        (std::numeric_limits<std::size_t>::max)());
    return static_cast<std::size_t>(
        std::min<unsigned long long>(status.ullTotalPhys, maximum));
#elif defined(__APPLE__)
    std::uint64_t bytes = 0U;
    std::size_t length = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &length, nullptr, 0) != 0
        || length != sizeof(bytes)) {
        return 0U;
    }
    const std::uint64_t maximum = static_cast<std::uint64_t>(
        (std::numeric_limits<std::size_t>::max)());
    return static_cast<std::size_t>(std::min(bytes, maximum));
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || page_size <= 0) return 0U;
    const long double bytes = static_cast<long double>(pages)
                              * static_cast<long double>(page_size);
    return bounded_bytes(bytes);
#endif
}

std::size_t resolved_render_memory_budget_bytes(
    const PerformanceSettings& settings) noexcept {
    const std::size_t physical = total_physical_memory_bytes();
    const long double value = std::isfinite(settings.render_memory_budget_value)
        ? std::max(0.0, settings.render_memory_budget_value) : 0.0;
    switch (settings.render_memory_budget_mode) {
        case RenderMemoryBudgetMode::Automatic:
            return automatic_memory_budget(physical);
        case RenderMemoryBudgetMode::Mebibytes:
            return bounded_bytes(value * static_cast<long double>(kMebibyte));
        case RenderMemoryBudgetMode::Gibibytes:
            return bounded_bytes(value * static_cast<long double>(kGibibyte));
        case RenderMemoryBudgetMode::PercentOfPhysicalMemory:
            if (physical == 0U) return automatic_memory_budget(physical);
            return bounded_bytes(static_cast<long double>(physical)
                                 * value / 100.0L);
    }
    return automatic_memory_budget(physical);
}
