#ifndef PVT_APP_RENDERER_DIAGNOSTICS_H
#define PVT_APP_RENDERER_DIAGNOSTICS_H

#include "procedural_visualizer_tool.h"

#include <sstream>
#include <string>
#include <thread>

namespace pvt::app {

inline const char* renderer_build_architecture() noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "ARM64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86-64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__arm__) || defined(_M_ARM)
    return "ARM";
#else
    return "other";
#endif
}

// Application diagnostics only: no additions to the installed renderer ABI.
inline std::string renderer_diagnostic_report(
    const RendererCapabilities& capabilities) {
    std::ostringstream report;
    constexpr std::size_t mebibyte = std::size_t{1024U} * 1024U;
    const RuntimeResourceLimits resource_limits = resolved_resource_limits();
    report << "Renderer report\n"
           << "Build architecture: " << renderer_build_architecture() << '\n'
           << "Logical CPU workers reported by host: "
           << std::thread::hardware_concurrency() << '\n'
           << "Installed physical memory: "
           << physical_memory_bytes() / mebibyte << " MiB\n"
           << "Automatic foreground render budget: "
           << automatic_render_memory_budget_bytes() / mebibyte << " MiB\n"
           << "Effective resource limits (MiB): decoded-image "
           << resource_limits.maximum_decoded_image_bytes / mebibyte
           << ", OBJ-file "
           << resource_limits.maximum_obj_file_bytes / mebibyte
           << ", expanded-OBJ "
           << resource_limits.maximum_obj_mesh_bytes / mebibyte
           << ", expanded-project "
           << resource_limits.maximum_project_bundle_expanded_bytes / mebibyte
           << "\nRetained cache limits (MiB/entries): images "
           << resource_limits.source_image_cache_bytes / mebibyte << '/'
           << resource_limits.source_image_cache_entries << ", OBJ "
           << resource_limits.obj_mesh_cache_bytes / mebibyte << '/'
           << resource_limits.obj_mesh_cache_entries << ", height-mesh "
           << resource_limits.displacement_mesh_cache_bytes / mebibyte << '/'
           << resource_limits.displacement_mesh_cache_entries << '\n'
           << "Working color: linear straight-alpha RGBA float32\n";
    const auto backend = [&](const char* name, bool compiled, bool available,
                             const std::string& device,
                             const std::string& status) {
        report << name << ": "
               << (!compiled ? "not compiled" : available ? "ready" : "unavailable")
               << '\n';
        if (!device.empty()) report << "  Device: " << device << '\n';
        if (!status.empty()) report << "  Status: " << status << '\n';
    };
    backend("Metal", capabilities.metal_compiled, capabilities.metal_available,
            capabilities.metal_device_name, capabilities.metal_status);
    backend("OpenGL", capabilities.opengl_surface_compiled,
            capabilities.opengl_surface_available,
            capabilities.opengl_surface_device_name,
            capabilities.opengl_surface_status);
    report << "Automatic preference: "
           << (capabilities.metal_available ? "Metal with bounded CPU work"
               : capabilities.opengl_surface_available ? "OpenGL with bounded CPU work"
                                                       : "CPU")
           << "\nGPU runtime failures are reported; a failed frame is not retried on CPU.\n";
    return report.str();
}

} // namespace pvt::app

#endif
