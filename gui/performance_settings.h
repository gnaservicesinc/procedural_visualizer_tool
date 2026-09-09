#ifndef PVT_PERFORMANCE_SETTINGS_H
#define PVT_PERFORMANCE_SETTINGS_H

#include "procedural_visualizer_tool.h"

#include <cstddef>

enum class RenderMemoryBudgetMode : int {
    Automatic = 0,
    Mebibytes,
    Gibibytes,
    PercentOfPhysicalMemory
};

// Machine-local execution policy. These values live in QSettings rather than
// ProjectConfig so moving a project between computers never changes its
// authored output or dirties the document.
enum class RenderBackendPreference : int {
    Automatic = 0,
    Cpu,
    CpuAndGpu,
    Gpu
};

struct PerformanceSettings {
    RenderBackendPreference backend = RenderBackendPreference::Automatic;
    // Zero lets the renderer choose from host concurrency.
    std::size_t preview_live_cpu_workers = 0U;
    // Zero lets sequence/video export choose outer-frame concurrency from host
    // capacity and the aggregate memory budget.
    std::size_t export_frame_workers = 0U;
    // Maximum CPU layer workers inside each export frame. Zero lets the export
    // coordinator partition host capacity across the outer frames it admits.
    std::size_t export_cpu_workers = 0U;
    // Zero selects host-adaptive, device-memory-bounded Metal admission.
    std::size_t gpu_frames_in_flight = 0U;
    // The unit and authored value remain machine-local. Automatic resolves to
    // a host-adaptive budget, while percentage mode follows the installed RAM
    // when preferences move between otherwise compatible hosts.
    RenderMemoryBudgetMode render_memory_budget_mode =
        RenderMemoryBudgetMode::Automatic;
    double render_memory_budget_value = 0.0;
    // Zero-valued fields select host-adaptive runtime defaults. These limits
    // are machine-local and never enter project persistence.
    pvt::RuntimeResourceLimits resource_limits;
    // Export normally owns the renderer exclusively. Artists may opt out when
    // an interactive editor preview is more important than maximum throughput.
    bool pause_editor_preview_during_export = true;
};

// Resolve the preference into the byte budget passed to render coordinators.
// Automatic assigns half of physical RAM to foreground render admission. The
// adaptive retained caches share another quarter, leaving one quarter for the
// document, OS, driver, and transient codec allocations. An unknown host keeps
// the renderer's historical 2 GiB fallback.
std::size_t resolved_render_memory_budget_bytes(
    const PerformanceSettings& settings) noexcept;

inline bool operator==(const PerformanceSettings& left,
                       const PerformanceSettings& right) noexcept {
    return left.backend == right.backend
           && left.preview_live_cpu_workers
                  == right.preview_live_cpu_workers
           && left.export_frame_workers == right.export_frame_workers
           && left.export_cpu_workers == right.export_cpu_workers
           && left.gpu_frames_in_flight == right.gpu_frames_in_flight
           && left.render_memory_budget_mode
                  == right.render_memory_budget_mode
           && left.render_memory_budget_value
                  == right.render_memory_budget_value
           && left.resource_limits.maximum_decoded_image_bytes
                  == right.resource_limits.maximum_decoded_image_bytes
           && left.resource_limits.maximum_obj_file_bytes
                  == right.resource_limits.maximum_obj_file_bytes
           && left.resource_limits.maximum_obj_mesh_bytes
                  == right.resource_limits.maximum_obj_mesh_bytes
           && left.resource_limits.maximum_project_bundle_expanded_bytes
                  == right.resource_limits.maximum_project_bundle_expanded_bytes
           && left.resource_limits.source_image_cache_bytes
                  == right.resource_limits.source_image_cache_bytes
           && left.resource_limits.source_image_cache_entries
                  == right.resource_limits.source_image_cache_entries
           && left.resource_limits.obj_mesh_cache_bytes
                  == right.resource_limits.obj_mesh_cache_bytes
           && left.resource_limits.obj_mesh_cache_entries
                  == right.resource_limits.obj_mesh_cache_entries
           && left.resource_limits.displacement_mesh_cache_bytes
                  == right.resource_limits.displacement_mesh_cache_bytes
           && left.resource_limits.displacement_mesh_cache_entries
                  == right.resource_limits.displacement_mesh_cache_entries
           && left.pause_editor_preview_during_export
                  == right.pause_editor_preview_during_export;
}

inline bool operator!=(const PerformanceSettings& left,
                       const PerformanceSettings& right) noexcept {
    return !(left == right);
}

#endif
