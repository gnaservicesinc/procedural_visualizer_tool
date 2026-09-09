#include "procedural_visualizer_tool.h"
#include "../src/displacement_surface.h"
#include "../src/render_asset_cache.h"
#include "../src/source_image.h"
#include "../src/render_memory.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <limits>
#include <stdexcept>

namespace fs = std::filesystem;

int main() {
    const fs::path temporary = fs::temp_directory_path()
        / ("pvt-asset-cache-" + pvt::generate_uuid());
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
    } cleanup{temporary};
    try {
        const auto require = [](bool ok, const char* message) {
            if (!ok) throw std::runtime_error(message);
        };
        fs::create_directories(temporary);
        const std::string path = (temporary / "source.png").string();
        pvt::RenderConfig config = pvt::default_config();
        config.width = 32;
        config.height = 32;
        pvt::Image source;
        source.width = 32;
        source.height = 32;
        source.pixels.assign(32U * 32U * 4U, 1.0F);
        std::string error;
        require(pvt::write_image(path, source, config, 0U, &error), error.c_str());

        pvt::ProjectConfig project = pvt::default_project();
        project.canvas.width = 32;
        project.canvas.height = 32;
        project.output.write_alpha = true;
        auto& render = project.layers.front().render;
        render.starting_image.enabled = true;
        render.starting_image.path = path;
        render.surface.enabled = true;
        render.surface.mapping = pvt::SurfaceMapping::Plane;
        render.surface.plane_displacement.enabled = true;
        render.surface.plane_displacement.path = path;
        render.surface.plane_displacement.pixels_per_node = 8;

        pvt::Image expected;
        require(pvt::render_project_frame(project, 0, expected, nullptr, &error),
                error.c_str());
        {
            pvt::detail::ProjectRenderMemory memory;
            require(pvt::detail::validate_project_render_memory(project, memory).ok,
                    "source/displacement ledger validation failed");
            require(memory.shared.leases.size() == 3U && memory.shared.owners.size() == 3U,
                    "color, height decode intent, and generated geometry were not leased separately");
            pvt::detail::SharedRenderMemory overflow;
            const auto first = std::make_shared<const int>(1);
            const auto second_owner = std::make_shared<const int>(2);
            const auto maximum = (std::numeric_limits<std::size_t>::max)();
            require(overflow.retain(first, maximum) && overflow.retain(first, maximum)
                        && !overflow.retain(second_owner, 1U)
                        && overflow.bytes == maximum && overflow.owners.size() == 1U,
                    "shared ledger overflow or duplicate retention changed accounting");
        }
        std::shared_ptr<const pvt::Image> decoded;
        std::shared_ptr<const pvt::detail::HeightImage> height;
        std::shared_ptr<const pvt::detail::ObjMesh> mesh;
        require(pvt::detail::load_starting_image_source(path, decoded, nullptr, &error),
                error.c_str());
        require(pvt::detail::load_height_image_source(path, height, nullptr, &error),
                error.c_str());
        require(pvt::detail::load_displacement_plane_mesh(
                    render.surface.plane_displacement, 32, 32, mesh, nullptr, &error),
                error.c_str());
        std::weak_ptr<const pvt::Image> image_lifetime = decoded;
        std::weak_ptr<const pvt::detail::HeightImage> height_lifetime = height;
        std::weak_ptr<const pvt::detail::ObjMesh> mesh_lifetime = mesh;
        decoded.reset();
        height.reset();
        mesh.reset();
        pvt::detail::prune_render_asset_caches(project);
        require(!image_lifetime.expired() && !height_lifetime.expired()
                    && !mesh_lifetime.expired(), "active assets lost cache ownership");

        // A retained mesh is self-contained; evicting its decoded height map
        // must free that map even while the geometry remains cached.
        pvt::detail::prune_source_image_cache({path}, {});
        require(height_lifetime.expired() && !mesh_lifetime.expired(),
                "cached geometry pinned an evicted decoded height map");
        require(pvt::detail::load_height_image_source(path, height, nullptr, &error),
                error.c_str());
        height_lifetime = height;
        height.reset();

        // Disabling one of two references must not release the shared asset.
        auto second = pvt::default_layer(1U);
        second.render = render;
        project.layers.front().enabled = false;
        project.layers.push_back(second);
        pvt::detail::prune_render_asset_caches(project);
        require(!image_lifetime.expired() && !mesh_lifetime.expired(),
                "shared active assets were evicted");

        pvt::LayerGroup group;
        group.uuid = pvt::generate_uuid();
        group.enabled = false;
        project.groups.push_back(group);
        project.layers.back().group_uuid = group.uuid;
        pvt::Image disabled;
        require(pvt::render_project_frame(project, 0, disabled, nullptr, &error),
                error.c_str());
        require(image_lifetime.expired() && height_lifetime.expired()
                    && mesh_lifetime.expired(), "disabled group retained decoded assets");
        require(project.layers.front().render.starting_image.path == path,
                "pruning changed an authored source path");

        project.layers.pop_back();
        project.groups.clear();
        project.layers.front().enabled = true;
        pvt::Image reenabled;
        require(pvt::render_project_frame(project, 0, reenabled, nullptr, &error),
                error.c_str());
        require(reenabled.pixels == expected.pixels, "reenabling changed output");

        // Eviction must not revoke an in-flight caller's shared handle.
        require(pvt::detail::load_starting_image_source(path, decoded, nullptr, &error),
                error.c_str());
        image_lifetime = decoded;
        project.layers.front().opacity = 0.0;
        pvt::detail::prune_render_asset_caches(project);
        require(!image_lifetime.expired() && !decoded->pixels.empty(),
                "pruning revoked an active immutable image");
        decoded.reset();
        require(image_lifetime.expired(), "zero-opacity layer retained the image");

        // An enabled layer with disabled source/surface controls retains only
        // its authored paths, not decoded images or generated geometry.
        require(pvt::detail::load_starting_image_source(path, decoded, nullptr, &error),
                error.c_str());
        image_lifetime = decoded;
        decoded.reset();
        project.layers.front().opacity = 1.0;
        project.layers.front().render.starting_image.enabled = false;
        project.layers.front().render.surface.enabled = false;
        pvt::detail::prune_render_asset_caches(project);
        require(image_lifetime.expired(), "unused source remained resident");

        // Disabled LFO definitions must preserve exact CPU output through both
        // public entry points without triggering materialization.
        pvt::Image plain;
        require(pvt::render_frame(config, 1, plain, &error), error.c_str());
        pvt::ParameterLfo lfo;
        lfo.id = 1U;
        lfo.enabled = false;
        lfo.target_path = "surface.curvature";
        config.parameter_lfos.push_back(lfo);
        pvt::Image inactive;
        require(pvt::render_frame(config, 1, inactive, &error), error.c_str());
        require(inactive.pixels == plain.pixels, "disabled LFO changed CPU output");
        pvt::FrameRenderOptions options;
        require(pvt::render_frame(config, 1, options, inactive, nullptr, &error),
                error.c_str());
        require(inactive.pixels == plain.pixels, "disabled LFO changed backend output");
        require(config.parameter_lfos.size() == 1U,
                "render discarded an authored disabled LFO");

        // Vertex storage can dominate a small canvas even with few visible
        // faces. Exercise this case through multiworker admission and then
        // disable every referencing layer to verify ownership is released.
        const std::string large_path = (temporary / "many-vertices.obj").string();
        {
            std::ofstream output(large_path);
            output << "v -1 -1 0\nv 1 -1 0\nv 0 1 0\n";
            for (int vertex = 0; vertex < 20000; ++vertex) output << "v 0 0 0\n";
            output << "f 1 2 3\n";
        }
        project = pvt::default_project();
        project.canvas.width = project.canvas.height = 32;
        project.output.write_alpha = true;
        for (unsigned layer_index = 0; layer_index < 4U; ++layer_index) {
            if (layer_index > 0U) {
                auto layer = pvt::default_layer(layer_index);
                layer.file_id = pvt::allocate_layer_file_id(project);
                project.layers.push_back(std::move(layer));
            }
            auto& layer = project.layers.back();
            layer.opacity = 0.6;
            layer.render.surface.enabled = true;
            layer.render.surface.mapping = pvt::SurfaceMapping::CustomObj;
            layer.render.surface.obj_path = large_path;
        }
        const auto large_estimate = pvt::validate(project);
        require(large_estimate.ok && large_estimate.estimated_peak_bytes > 20003U * 64U,
                "project admission omitted large OBJ projection storage");
        pvt::detail::prune_render_asset_caches(project);
        pvt::detail::ProjectRenderMemory shared_memory;
        const auto shared_estimate = pvt::detail::validate_project_render_memory(project, shared_memory);
        require(shared_estimate.ok && shared_memory.shared.bytes > 0U,
                "shared OBJ memory was not separated from worker storage");
        require(shared_estimate.estimated_peak_bytes == shared_memory.shared.bytes
                    + shared_memory.worker_bytes + shared_memory.composite_bytes,
                "project memory categories do not sum to the public estimate");
        auto one_layer = project;
        one_layer.layers.resize(1U);
        pvt::detail::ProjectRenderMemory one_memory;
        require(pvt::detail::validate_project_render_memory(one_layer, one_memory).ok,
                "single-layer memory inspection failed");
        require(one_memory.shared.bytes == shared_memory.shared.bytes
                    && one_memory.worker_bytes == shared_memory.worker_bytes,
                "repeated OBJ references duplicated the immutable allocation");
        const std::size_t two_worker_budget = shared_memory.shared.bytes
            + shared_memory.composite_bytes + shared_memory.worker_bytes * 2U;
        require(shared_memory.worker_budget(two_worker_budget) == shared_memory.worker_bytes * 2U
                    && shared_memory.worker_budget(1U) == 0U,
                "shared/composite memory was not reserved once before admission");
        std::weak_ptr<const void> shared_lifetime = shared_memory.shared.owners.begin()->second;
        // Settings clear each cache after a resource-limit change. Exercise
        // those entry points directly across the shared-library boundary and
        // ensure already-admitted renders keep their immutable assets.
        const pvt::detail::AssetPaths none;
        pvt::detail::prune_opengl_mesh_cache(none);
        pvt::detail::prune_displacement_mesh_cache(none);
        pvt::detail::prune_obj_mesh_cache(none);
        pvt::detail::prune_source_image_cache(none, none);
        require(!shared_lifetime.expired(), "eviction revoked an admitted asset handle");
        one_memory = {};
        shared_memory = {};
        require(shared_lifetime.expired(), "completed admission retained an evicted asset");
        options.backend = pvt::RenderBackend::Cpu;
        options.maximum_cpu_workers = 1U;
        require(pvt::render_project_frame(project, 0, options, expected, nullptr, &error), error.c_str());
        options.maximum_cpu_workers = 4U;
        options.cpu_memory_budget_bytes = 1U;
        require(pvt::render_project_frame(project, 0, options, reenabled, nullptr, &error), error.c_str());
        require(expected.pixels == reenabled.pixels,
                "memory-constrained OBJ layer rendering changed output");
        options.cpu_memory_budget_bytes = two_worker_budget;
        require(pvt::render_project_frame(project, 0, options, reenabled, nullptr, &error), error.c_str());
        require(expected.pixels == reenabled.pixels, "shared-budget parallel rendering changed output");
        for (auto& layer : project.layers) layer.enabled = false;
        require(pvt::render_project_frame(project, 0, options, disabled, nullptr, &error), error.c_str());
        // Inactive authored references remain valid if their files disappear.
        fs::remove(large_path);
        require(pvt::validate(project).ok, "disabled OBJ validation inspected its file");
        std::cout << "Asset cache lifetime and inactive configuration tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
