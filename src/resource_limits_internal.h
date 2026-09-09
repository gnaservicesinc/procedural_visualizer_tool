#ifndef PVT_RESOURCE_LIMITS_INTERNAL_H
#define PVT_RESOURCE_LIMITS_INTERNAL_H

#include "procedural_visualizer_tool.h"

#include <algorithm>
#include <cstddef>

namespace pvt::detail {

constexpr std::size_t kResourceLimitMebibyte =
    std::size_t{1024U} * 1024U;

// Pure policy helpers kept separate from host detection so representative
// small and large machines can be regression-tested deterministically.
inline std::size_t automatic_render_memory_budget_for_physical_memory(
    std::size_t physical_memory) noexcept {
    if (physical_memory == 0U) return kDefaultSequenceMemoryBudgetBytes;
    return (std::max)(std::size_t{1U}, physical_memory / 2U);
}

inline RuntimeResourceLimits automatic_resource_limits_for_physical_memory(
    std::size_t physical_memory) noexcept {
    RuntimeResourceLimits limits;
    if (physical_memory == 0U) {
        constexpr std::size_t fallback =
            std::size_t{512U} * kResourceLimitMebibyte;
        limits.maximum_decoded_image_bytes = fallback;
        limits.maximum_obj_file_bytes = fallback;
        limits.maximum_obj_mesh_bytes = fallback;
        // Preserve the former aggregate bundle boundary when the host cannot
        // expose RAM. Individual entries retain their signed-int boundary.
        limits.maximum_project_bundle_expanded_bytes = kMaximumUiItems;
        limits.source_image_cache_bytes = fallback;
        limits.source_image_cache_entries = 64U;
        limits.obj_mesh_cache_bytes = fallback;
        limits.obj_mesh_cache_entries = 16U;
        limits.displacement_mesh_cache_bytes = fallback;
        limits.displacement_mesh_cache_entries = 16U;
        return limits;
    }

    const auto fraction = [physical_memory](std::size_t denominator) {
        return (std::max)(std::size_t{1U}, physical_memory / denominator);
    };
    // PVT is normally the foreground workload. Active render admission gets
    // half of RAM (above), while retained caches divide another quarter. The
    // remaining quarter is left for the document, OS, driver, and transient
    // codec allocations. Per-asset admission bounds do not reserve memory.
    limits.maximum_decoded_image_bytes = fraction(4U);
    limits.maximum_obj_file_bytes = fraction(8U);
    limits.maximum_obj_mesh_bytes = fraction(4U);
    limits.maximum_project_bundle_expanded_bytes = fraction(4U);
    limits.source_image_cache_bytes = fraction(8U);
    limits.source_image_cache_entries = (std::max)(
        std::size_t{64U}, limits.source_image_cache_bytes
                              / (std::size_t{64U}
                                 * kResourceLimitMebibyte));
    limits.obj_mesh_cache_bytes = fraction(16U);
    limits.obj_mesh_cache_entries = (std::max)(
        std::size_t{16U}, limits.obj_mesh_cache_bytes
                              / (std::size_t{64U}
                                 * kResourceLimitMebibyte));
    limits.displacement_mesh_cache_bytes = fraction(16U);
    limits.displacement_mesh_cache_entries = (std::max)(
        std::size_t{16U}, limits.displacement_mesh_cache_bytes
                              / (std::size_t{64U}
                                 * kResourceLimitMebibyte));
    return limits;
}

} // namespace pvt::detail

#endif
