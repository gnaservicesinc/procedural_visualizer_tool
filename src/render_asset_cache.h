#ifndef PVT_RENDER_ASSET_CACHE_H
#define PVT_RENDER_ASSET_CACHE_H

#include "procedural_visualizer_tool.h"

#include <string>
#include <unordered_set>

namespace pvt::detail {

using AssetPaths = std::unordered_set<std::string>;
struct SharedRenderMemory;
bool retain_source_cache_memory(SharedRenderMemory& memory);
bool retain_displacement_cache_memory(SharedRenderMemory& memory);
bool retain_obj_cache_memory(SharedRenderMemory& memory);

// Drop cache ownership only. In-flight renders keep immutable shared handles;
// authored paths, embedded assets, undo, and saved configurations are retained.
PVT_API void prune_render_asset_caches(const ProjectConfig& project) noexcept;
PVT_API void prune_source_image_cache(const AssetPaths& images,
                                       const AssetPaths& heights);
PVT_API void prune_displacement_mesh_cache(const AssetPaths& heights);
void prune_obj_mesh_cache(const AssetPaths& objects);
void prune_opengl_mesh_cache(const AssetPaths& heights);

} // namespace pvt::detail

#endif
