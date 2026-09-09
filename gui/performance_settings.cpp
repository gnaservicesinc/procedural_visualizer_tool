#include "performance_settings.h"

#include <algorithm>
#include <cmath>
#include <limits>

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

} // namespace

std::size_t resolved_render_memory_budget_bytes(
    const PerformanceSettings& settings) noexcept {
    const std::size_t physical = pvt::physical_memory_bytes();
    const long double value = std::isfinite(settings.render_memory_budget_value)
        ? std::max(0.0, settings.render_memory_budget_value) : 0.0;
    switch (settings.render_memory_budget_mode) {
        case RenderMemoryBudgetMode::Automatic:
            return pvt::automatic_render_memory_budget_bytes();
        case RenderMemoryBudgetMode::Mebibytes:
            return bounded_bytes(value * static_cast<long double>(kMebibyte));
        case RenderMemoryBudgetMode::Gibibytes:
            return bounded_bytes(value * static_cast<long double>(kGibibyte));
        case RenderMemoryBudgetMode::PercentOfPhysicalMemory:
            if (physical == 0U) {
                return pvt::automatic_render_memory_budget_bytes();
            }
            return bounded_bytes(static_cast<long double>(physical)
                                 * value / 100.0L);
    }
    return pvt::automatic_render_memory_budget_bytes();
}
