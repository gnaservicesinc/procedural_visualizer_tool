#include "procedural_visualizer_tool.h"
#include "resource_limits_internal.h"

#include <algorithm>
#include <atomic>
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

namespace pvt {
namespace {

struct ResourceLimitState {
    std::atomic_flag access = ATOMIC_FLAG_INIT;
    std::size_t maximum_decoded_image_bytes = 0U;
    std::size_t maximum_obj_file_bytes = 0U;
    std::size_t maximum_obj_mesh_bytes = 0U;
    std::size_t maximum_project_bundle_expanded_bytes = 0U;
    std::size_t source_image_cache_bytes = 0U;
    std::size_t source_image_cache_entries = 0U;
    std::size_t obj_mesh_cache_bytes = 0U;
    std::size_t obj_mesh_cache_entries = 0U;
    std::size_t displacement_mesh_cache_bytes = 0U;
    std::size_t displacement_mesh_cache_entries = 0U;
};

ResourceLimitState& overrides() noexcept {
    static ResourceLimitState limits;
    return limits;
}

[[maybe_unused]] std::size_t bounded_bytes(long double requested) noexcept {
    if (std::isnan(requested) || requested <= 0.0L) return 0U;
    const long double maximum = static_cast<long double>(
        (std::numeric_limits<std::size_t>::max)());
    if (std::isinf(requested) || requested >= maximum) {
        return (std::numeric_limits<std::size_t>::max)();
    }
    return static_cast<std::size_t>(std::floor(requested));
}

std::size_t selected(std::size_t override_value,
                     std::size_t automatic_value,
                     std::size_t hard_maximum =
                         (std::numeric_limits<std::size_t>::max)()) noexcept {
    return (std::min)(override_value == 0U ? automatic_value : override_value,
                      hard_maximum);
}

} // namespace

std::size_t detect_physical_memory_bytes() noexcept {
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) == 0) return 0U;
    const auto maximum = static_cast<unsigned long long>(
        (std::numeric_limits<std::size_t>::max)());
    return static_cast<std::size_t>(
        (std::min)(status.ullTotalPhys, maximum));
#elif defined(__APPLE__)
    std::uint64_t bytes = 0U;
    std::size_t length = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &length, nullptr, 0) != 0
        || length != sizeof(bytes)) {
        return 0U;
    }
    const std::uint64_t maximum = static_cast<std::uint64_t>(
        (std::numeric_limits<std::size_t>::max)());
    return static_cast<std::size_t>((std::min)(bytes, maximum));
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || page_size <= 0) return 0U;
    return bounded_bytes(static_cast<long double>(pages)
                         * static_cast<long double>(page_size));
#endif
}

std::size_t physical_memory_bytes() noexcept {
    // Installed physical memory is stable for the lifetime of the process and
    // querying sysctl/sysconf on every cache access adds avoidable overhead.
    static const std::size_t detected = detect_physical_memory_bytes();
    return detected;
}

std::size_t automatic_render_memory_budget_bytes() noexcept {
    return detail::automatic_render_memory_budget_for_physical_memory(
        physical_memory_bytes());
}

RuntimeResourceLimits automatic_resource_limits() noexcept {
    return detail::automatic_resource_limits_for_physical_memory(
        physical_memory_bytes());
}

RuntimeResourceLimits resource_limit_overrides() noexcept {
    ResourceLimitState& source = overrides();
    while (source.access.test_and_set(std::memory_order_acquire)) {}
    RuntimeResourceLimits limits;
    limits.maximum_decoded_image_bytes = source.maximum_decoded_image_bytes;
    limits.maximum_obj_file_bytes = source.maximum_obj_file_bytes;
    limits.maximum_obj_mesh_bytes = source.maximum_obj_mesh_bytes;
    limits.maximum_project_bundle_expanded_bytes =
        source.maximum_project_bundle_expanded_bytes;
    limits.source_image_cache_bytes = source.source_image_cache_bytes;
    limits.source_image_cache_entries = source.source_image_cache_entries;
    limits.obj_mesh_cache_bytes = source.obj_mesh_cache_bytes;
    limits.obj_mesh_cache_entries = source.obj_mesh_cache_entries;
    limits.displacement_mesh_cache_bytes =
        source.displacement_mesh_cache_bytes;
    limits.displacement_mesh_cache_entries =
        source.displacement_mesh_cache_entries;
    source.access.clear(std::memory_order_release);
    return limits;
}

void set_resource_limit_overrides(
    const RuntimeResourceLimits& limits) noexcept {
    ResourceLimitState& destination = overrides();
    while (destination.access.test_and_set(std::memory_order_acquire)) {}
    destination.maximum_decoded_image_bytes =
        limits.maximum_decoded_image_bytes;
    destination.maximum_obj_file_bytes = limits.maximum_obj_file_bytes;
    destination.maximum_obj_mesh_bytes = limits.maximum_obj_mesh_bytes;
    destination.maximum_project_bundle_expanded_bytes =
        limits.maximum_project_bundle_expanded_bytes;
    destination.source_image_cache_bytes = limits.source_image_cache_bytes;
    destination.source_image_cache_entries =
        limits.source_image_cache_entries;
    destination.obj_mesh_cache_bytes = limits.obj_mesh_cache_bytes;
    destination.obj_mesh_cache_entries = limits.obj_mesh_cache_entries;
    destination.displacement_mesh_cache_bytes =
        limits.displacement_mesh_cache_bytes;
    destination.displacement_mesh_cache_entries =
        limits.displacement_mesh_cache_entries;
    destination.access.clear(std::memory_order_release);
}

RuntimeResourceLimits resolve_resource_limits(
    const RuntimeResourceLimits& override) noexcept {
    const RuntimeResourceLimits automatic = automatic_resource_limits();
    RuntimeResourceLimits limits;
    limits.maximum_decoded_image_bytes = selected(
        override.maximum_decoded_image_bytes,
        automatic.maximum_decoded_image_bytes);
    limits.maximum_obj_file_bytes = selected(
        override.maximum_obj_file_bytes, automatic.maximum_obj_file_bytes);
    limits.maximum_obj_mesh_bytes = selected(
        override.maximum_obj_mesh_bytes, automatic.maximum_obj_mesh_bytes);
    limits.maximum_project_bundle_expanded_bytes = selected(
        override.maximum_project_bundle_expanded_bytes,
        automatic.maximum_project_bundle_expanded_bytes);
    limits.source_image_cache_bytes = selected(
        override.source_image_cache_bytes, automatic.source_image_cache_bytes);
    limits.source_image_cache_entries = selected(
        override.source_image_cache_entries,
        automatic.source_image_cache_entries, kMaximumUiItems);
    limits.obj_mesh_cache_bytes = selected(
        override.obj_mesh_cache_bytes, automatic.obj_mesh_cache_bytes);
    limits.obj_mesh_cache_entries = selected(
        override.obj_mesh_cache_entries, automatic.obj_mesh_cache_entries,
        kMaximumUiItems);
    limits.displacement_mesh_cache_bytes = selected(
        override.displacement_mesh_cache_bytes,
        automatic.displacement_mesh_cache_bytes);
    limits.displacement_mesh_cache_entries = selected(
        override.displacement_mesh_cache_entries,
        automatic.displacement_mesh_cache_entries, kMaximumUiItems);
    return limits;
}

RuntimeResourceLimits resolved_resource_limits() noexcept {
    return resolve_resource_limits(resource_limit_overrides());
}

} // namespace pvt
