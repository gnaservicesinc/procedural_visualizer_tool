#ifndef PVT_PERFORMANCE_SETTINGS_H
#define PVT_PERFORMANCE_SETTINGS_H

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
    // Zero selects the renderer's conservative automatic admission limit.
    std::size_t gpu_frames_in_flight = 0U;
    // The unit and authored value remain machine-local. Automatic resolves to
    // a host-adaptive budget, while percentage mode follows the installed RAM
    // when preferences move between otherwise compatible hosts.
    RenderMemoryBudgetMode render_memory_budget_mode =
        RenderMemoryBudgetMode::Automatic;
    double render_memory_budget_value = 0.0;
    // Export normally owns the renderer exclusively. Artists may opt out when
    // an interactive editor preview is more important than maximum throughput.
    bool pause_editor_preview_during_export = true;
};

// Zero means that the host did not expose a trustworthy physical-memory size.
std::size_t total_physical_memory_bytes() noexcept;

// Resolve the preference into the byte budget passed to render coordinators.
// Automatic reserves most memory for the OS and other creative applications;
// an unknown host retains the renderer's historical 2 GiB fallback.
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
           && left.pause_editor_preview_during_export
                  == right.pause_editor_preview_during_export;
}

inline bool operator!=(const PerformanceSettings& left,
                       const PerformanceSettings& right) noexcept {
    return !(left == right);
}

#endif
