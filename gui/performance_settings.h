#ifndef PVT_PERFORMANCE_SETTINGS_H
#define PVT_PERFORMANCE_SETTINGS_H

#include <cstddef>

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
    // Stored in MiB for stable, human-readable QSettings values. Zero selects
    // the renderer's automatic aggregate budget.
    std::size_t render_memory_budget_mib = 0U;
};

inline bool operator==(const PerformanceSettings& left,
                       const PerformanceSettings& right) noexcept {
    return left.backend == right.backend
           && left.preview_live_cpu_workers
                  == right.preview_live_cpu_workers
           && left.export_frame_workers == right.export_frame_workers
           && left.export_cpu_workers == right.export_cpu_workers
           && left.gpu_frames_in_flight == right.gpu_frames_in_flight
           && left.render_memory_budget_mib
                  == right.render_memory_budget_mib;
}

inline bool operator!=(const PerformanceSettings& left,
                       const PerformanceSettings& right) noexcept {
    return !(left == right);
}

#endif
