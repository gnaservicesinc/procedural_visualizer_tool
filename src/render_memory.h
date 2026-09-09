#ifndef PVT_RENDER_MEMORY_H
#define PVT_RENDER_MEMORY_H

#include "procedural_visualizer_tool.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace pvt::detail {

// A validation/render invocation owns this ledger. Identity deduplicates the
// immutable allocation rather than its path (decode intents/versions differ).
// Handles keep the measured allocation alive through worker admission.
struct SharedRenderMemory {
    std::unordered_map<const void*, std::shared_ptr<const void>> owners;
    // File version / decode intent / generation parameters identify leases.
    // Workers reuse these even if the bounded process caches evict an asset.
    std::unordered_map<std::string, std::shared_ptr<const void>> leases;
    std::size_t bytes = 0U;

    template <typename T>
    bool retain(const std::shared_ptr<const T>& owner, std::size_t allocation_bytes) {
        if (!owner || owners.count(owner.get()) != 0U) return true;
        if (allocation_bytes > (std::numeric_limits<std::size_t>::max)() - bytes) return false;
        owners.emplace(owner.get(), owner);
        bytes += allocation_bytes;
        return true;
    }
};

// This context is internal to the renderer. Validation alone may publish
// leases; workers only read the completed ledger, without locking or mutation.
inline thread_local const SharedRenderMemory* render_memory_reader = nullptr;
inline thread_local SharedRenderMemory* render_memory_writer = nullptr;

class RenderMemoryScope {
    const SharedRenderMemory* previous_reader_ = render_memory_reader;
    SharedRenderMemory* previous_writer_ = render_memory_writer;

  public:
    explicit RenderMemoryScope(SharedRenderMemory& memory) {
        render_memory_reader = &memory;
        render_memory_writer = &memory;
    }
    explicit RenderMemoryScope(const SharedRenderMemory& memory) {
        render_memory_reader = &memory;
        render_memory_writer = nullptr;
    }
    ~RenderMemoryScope() {
        render_memory_reader = previous_reader_;
        render_memory_writer = previous_writer_;
    }
    RenderMemoryScope(const RenderMemoryScope&) = delete;
    RenderMemoryScope& operator=(const RenderMemoryScope&) = delete;
};

// Length-delimited path and individually appended scalars avoid separator
// collisions and struct padding. Keys live only within this process.
inline std::string render_asset_key(char kind, const std::string& path) {
    std::string key(1U, kind);
    const auto length = path.size();
    key.append(reinterpret_cast<const char*>(&length), sizeof(length));
    key.append(path);
    return key;
}
template <typename T> void append_render_asset_key(std::string& key, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    key.append(reinterpret_cast<const char*>(&value), sizeof(value));
}
template <typename T> std::shared_ptr<const T> find_render_asset(const std::string& key) {
    if (!render_memory_reader) return {};
    const auto found = render_memory_reader->leases.find(key);
    return found == render_memory_reader->leases.end()
               ? std::shared_ptr<const T>{}
               : std::static_pointer_cast<const T>(found->second);
}
template <typename T>
void remember_render_asset(const std::string& key, const std::shared_ptr<const T>& owner) {
    if (render_memory_writer && owner) render_memory_writer->leases.emplace(key, owner);
}

struct ProjectRenderMemory {
    SharedRenderMemory shared;
    std::size_t worker_bytes = 0U;
    std::size_t composite_bytes = 0U;

    std::size_t worker_budget(std::size_t total_budget) const noexcept {
        // Reserve once; one oversized worker is still permitted by admission.
        if (shared.bytes >= total_budget) return 0U;
        const std::size_t remaining = total_budget - shared.bytes;
        return composite_bytes >= remaining ? 0U : remaining - composite_bytes;
    }
};

inline std::size_t memory_limited_outer_workers(
    std::size_t aggregate_budget, std::size_t shared_bytes,
    std::size_t per_worker_bytes, std::size_t requested_workers) noexcept {
    if (per_worker_bytes == 0U) return requested_workers;
    const std::size_t available = shared_bytes >= aggregate_budget
        ? 0U : aggregate_budget - shared_bytes;
    return std::max<std::size_t>(1U, available / per_worker_bytes);
}

// Convert an aggregate invocation budget into the per-frame value consumed by
// the existing project-layer scheduler. That scheduler subtracts shared bytes
// from its input, so add the invocation's one shared reservation to each
// derived value without dividing or reserving it again.
inline std::size_t outer_worker_frame_memory_budget(
    std::size_t aggregate_budget, std::size_t shared_bytes,
    std::size_t worker_count, std::size_t worker_index) noexcept {
    worker_count = std::max<std::size_t>(1U, worker_count);
    worker_index = std::min(worker_index, worker_count - 1U);
    const std::size_t available = shared_bytes >= aggregate_budget
        ? 0U : aggregate_budget - shared_bytes;
    const std::size_t share = available / worker_count
        + (worker_index < available % worker_count ? 1U : 0U);
    return std::max<std::size_t>(1U, shared_bytes + share);
}

// Internal diagnostic/dispatch API; the public ValidationResult ABI is intact.
PVT_API ValidationResult validate_project_render_memory(const ProjectConfig& project,
                                                        ProjectRenderMemory& memory);

// Render one already-bounded frame while borrowing the immutable asset leases
// from an outer synchronous invocation. Sequence/movie coordinators use this
// after one full validation so every frame observes the same admitted asset
// snapshot without rescanning the project or multiplying shared ownership.
PVT_API bool render_project_frame_validated(
    const ProjectConfig& project, int frame_index, int frame_count,
    const FrameRenderOptions& options, const ProjectRenderMemory& memory,
    Image& destination, const std::atomic_bool* cancel,
    std::string* error);

} // namespace pvt::detail
#endif
