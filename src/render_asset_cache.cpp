#include "render_asset_cache.h"

#include "frame_renderer_internal.h"
#include "path_utf8.h"

#include <algorithm>
#include <filesystem>

namespace pvt::detail {

void prune_render_asset_caches(const ProjectConfig& project) noexcept {
    try {
        AssetPaths images;
        AssetPaths heights;
        AssetPaths objects;
        AssetPaths disabled_groups;
        for (const auto& group : project.groups) {
            if (!group.enabled) disabled_groups.insert(group.uuid);
        }
        for (const auto& layer : project.layers) {
            if (!layer.enabled || layer.opacity <= 0.0
                || disabled_groups.count(layer.group_uuid) != 0U) continue;
            const auto& render = layer.render;
            if (render.starting_image.enabled && !render.starting_image.path.empty()) {
                images.insert(render.starting_image.path);
            }
            const auto& surface = render.surface;
            if (!surface.enabled) continue;
            // Scalar LFO targets can activate curvature, lighting, or mix later
            // in the loop. Retain those assets without decoding them eagerly.
            const bool animated = has_enabled_parameter_lfo(render);
            if (surface.curvature > 0.0 || animated) {
                if (surface.mapping == SurfaceMapping::CustomObj
                    && !surface.obj_path.empty()) {
                    std::error_code error;
                    const auto absolute = std::filesystem::absolute(
                        path_from_utf8(surface.obj_path), error);
                    if (!error) objects.insert(path_to_utf8(absolute.lexically_normal()));
                }
                if (surface.mapping == SurfaceMapping::Plane
                    && surface.plane_displacement.enabled
                    && !surface.plane_displacement.path.empty()) {
                    heights.insert(surface.plane_displacement.path);
                }
            }
            if ((surface.mapping == SurfaceMapping::Plane
                 || surface.curvature > 0.0 || animated)
                && surface.environment_map.enabled
                && (surface.lighting > 0.0 || animated)
                && (surface.environment_map.mix > 0.0 || animated)
                && !surface.environment_map.path.empty()) {
                images.insert(surface.environment_map.path);
            }
        }
        prune_opengl_mesh_cache(heights);
        prune_displacement_mesh_cache(heights);
        prune_obj_mesh_cache(objects);
        prune_source_image_cache(images, heights);
    } catch (...) {
        // Cache maintenance must not turn a valid edit/render into an error
        // under allocation pressure. The next maintenance pass retries.
    }
}

} // namespace pvt::detail
