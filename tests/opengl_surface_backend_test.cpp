#include "procedural_visualizer_tool.h"

#include <QGuiApplication>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << __FILE__ << ':' << __LINE__                           \
                      << ": CHECK failed: " #condition "\n";                  \
            ++failures;                                                        \
        }                                                                      \
    } while (false)

double maximum_difference(const pvt::Image& first, const pvt::Image& second,
                          std::size_t* maximum_index = nullptr) {
    if (first.width != second.width || first.height != second.height
        || first.pixels.size() != second.pixels.size()) {
        return 1.0e30;
    }
    double result = 0.0;
    for (std::size_t index = 0U; index < first.pixels.size(); ++index) {
        const double difference =
            std::fabs(static_cast<double>(first.pixels[index])
                      - second.pixels[index]);
        if (difference > result) {
            result = difference;
            if (maximum_index != nullptr) *maximum_index = index;
        }
    }
    return result;
}

double maximum_straight_alpha_difference(
    const pvt::Image& first, const pvt::Image& second,
    std::size_t* maximum_index = nullptr) {
    if (first.width != second.width || first.height != second.height
        || first.pixels.size() != second.pixels.size()
        || first.pixels.size() % 4U != 0U) {
        return 1.0e30;
    }
    double result = 0.0;
    for (std::size_t pixel = 0U; pixel < first.pixels.size(); pixel += 4U) {
        const double first_alpha = std::clamp(
            static_cast<double>(first.pixels[pixel + 3U]), 0.0, 1.0);
        const double second_alpha = std::clamp(
            static_cast<double>(second.pixels[pixel + 3U]), 0.0, 1.0);
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            const double difference = std::fabs(
                static_cast<double>(first.pixels[pixel + channel])
                    * first_alpha
                - static_cast<double>(second.pixels[pixel + channel])
                    * second_alpha);
            if (difference > result) {
                result = difference;
                if (maximum_index != nullptr) {
                    *maximum_index = pixel + channel;
                }
            }
        }
        const double alpha_difference =
            std::fabs(first_alpha - second_alpha);
        if (alpha_difference > result) {
            result = alpha_difference;
            if (maximum_index != nullptr) *maximum_index = pixel + 3U;
        }
    }
    return result;
}

pvt::RenderConfig analytic_config(pvt::SurfaceMapping mapping) {
    pvt::RenderConfig config = pvt::default_config();
    config.width = 67;
    config.height = 53;
    config.total_frames = 17;
    config.surface.enabled = true;
    config.surface.mapping = mapping;
    config.surface.curvature = 0.82;
    config.surface.lighting = 0.47;
    config.surface.rotation_x_degrees = -11.0;
    config.surface.rotation_y_turns_per_loop = 2;
    config.surface.rotation_y_degrees = 17.0;
    config.surface.rotation_z_degrees = 9.0;
    config.surface.rotation_order = pvt::SurfaceRotationOrder::ZXY;
    config.starting_colors.mode = pvt::StartingColorMode::SquareSpiralRainbow;
    config.starting_colors.include_alpha = true;
    config.alpha.use_source_alpha = true;
    return config;
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    const pvt::RendererCapabilities capabilities =
        pvt::renderer_capabilities();
    CHECK(capabilities.opengl_surface_compiled);
    CHECK(!capabilities.opengl_surface_status.empty());
    if (!capabilities.opengl_surface_available) {
        std::cout << "OpenGL backend compiled; runtime context unavailable on "
                     "this hosted machine: "
                  << capabilities.opengl_surface_status << '\n';
        return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    CHECK(!capabilities.opengl_surface_device_name.empty());

    pvt::FrameRenderOptions cpu;
    cpu.backend = pvt::RenderBackend::Cpu;
    pvt::FrameRenderOptions hybrid;
    hybrid.backend = pvt::RenderBackend::CpuAndGpu;
    pvt::FrameRenderOptions gpu;
    gpu.backend = pvt::RenderBackend::Gpu;

    const std::vector<pvt::SurfaceMapping> accelerated_mappings = {
        pvt::SurfaceMapping::Plane, pvt::SurfaceMapping::Cylinder,
        pvt::SurfaceMapping::Sphere, pvt::SurfaceMapping::Cube};
    for (const pvt::SurfaceMapping mapping : accelerated_mappings) {
        const pvt::RenderConfig config = analytic_config(mapping);
        pvt::Image reference;
        pvt::Image accelerated;
        pvt::Image strict;
        std::string error;
        CHECK(pvt::render_frame(config, 5, cpu, reference, nullptr, &error));
        CHECK(pvt::render_frame(config, 5, hybrid, accelerated, nullptr,
                                &error));
        CHECK(pvt::render_frame(config, 5, gpu, strict, nullptr, &error));
        std::size_t hybrid_index = 0U;
        std::size_t strict_index = 0U;
        const double hybrid_difference = maximum_straight_alpha_difference(
            reference, accelerated, &hybrid_index);
        const double strict_difference = maximum_straight_alpha_difference(
            reference, strict, &strict_index);
        if (hybrid_difference > 0.0035 || strict_difference > 0.0035) {
            pvt::RenderConfig identity_config = config;
            identity_config.surface.enabled = false;
            pvt::Image identity;
            CHECK(pvt::render_frame(identity_config, 5, cpu, identity, nullptr,
                                    &error));
            pvt::RenderConfig inverse_config = config;
            inverse_config.surface.rotation_x_turns_per_loop =
                -inverse_config.surface.rotation_x_turns_per_loop;
            inverse_config.surface.rotation_y_turns_per_loop =
                -inverse_config.surface.rotation_y_turns_per_loop;
            inverse_config.surface.rotation_z_turns_per_loop =
                -inverse_config.surface.rotation_z_turns_per_loop;
            inverse_config.surface.rotation_x_degrees =
                -inverse_config.surface.rotation_x_degrees;
            inverse_config.surface.rotation_y_degrees =
                -inverse_config.surface.rotation_y_degrees;
            inverse_config.surface.rotation_z_degrees =
                -inverse_config.surface.rotation_z_degrees;
            pvt::Image inverse;
            CHECK(pvt::render_frame(inverse_config, 5, cpu, inverse, nullptr,
                                    &error));
            const std::size_t pixel_index = strict_index - strict_index % 4U;
            std::size_t nearest_identity_pixel = 0U;
            double nearest_identity_distance = 1.0e30;
            for (std::size_t candidate = 0U;
                 candidate < identity.pixels.size(); candidate += 4U) {
                double distance = 0.0;
                for (std::size_t channel = 0U; channel < 4U; ++channel) {
                    distance += std::fabs(
                        static_cast<double>(strict.pixels[pixel_index + channel])
                        - identity.pixels[candidate + channel]);
                }
                if (distance < nearest_identity_distance) {
                    nearest_identity_distance = distance;
                    nearest_identity_pixel = candidate;
                }
            }
            std::cerr << pvt::surface_mapping_name(mapping)
                      << " CPU/hybrid max difference " << hybrid_difference
                      << " at value " << hybrid_index << " (CPU "
                      << reference.pixels[hybrid_index] << ", GPU "
                      << accelerated.pixels[hybrid_index]
                      << "), CPU/GPU max difference " << strict_difference
                      << " at value " << strict_index << " (CPU "
                      << reference.pixels[strict_index] << ", GPU "
                      << strict.pixels[strict_index] << "); GPU/identity "
                      << maximum_difference(strict, identity)
                      << ", GPU/inverse "
                      << maximum_difference(strict, inverse) << "; CPU rgba [";
            for (std::size_t channel = 0U; channel < 4U; ++channel) {
                if (channel != 0U) std::cerr << ", ";
                std::cerr << reference.pixels[pixel_index + channel];
            }
            std::cerr << "], GPU rgba [";
            for (std::size_t channel = 0U; channel < 4U; ++channel) {
                if (channel != 0U) std::cerr << ", ";
                std::cerr << strict.pixels[pixel_index + channel];
            }
            const std::size_t nearest_pixel = nearest_identity_pixel / 4U;
            std::cerr << "], nearest input pixel ("
                      << nearest_pixel % static_cast<std::size_t>(identity.width)
                      << ", "
                      << nearest_pixel / static_cast<std::size_t>(identity.width)
                      << ") distance " << nearest_identity_distance << '\n';
        }
        CHECK(hybrid_difference <= 0.0035);
        CHECK(strict_difference <= 0.0035);
    }

    // The alpha-aware fixture above exercises straight-alpha and backface
    // composition without treating hidden RGB below alpha zero as visible.
    // Keep an all-opaque fixture as an uncompromised raw-float parity check.
    for (const pvt::SurfaceMapping mapping : accelerated_mappings) {
        pvt::RenderConfig config = analytic_config(mapping);
        config.starting_colors.include_alpha = false;
        config.alpha.use_source_alpha = false;
        config.surface.outside = pvt::SurfaceOutside::Source;
        pvt::Image reference;
        pvt::Image strict;
        std::string error;
        CHECK(pvt::render_frame(config, 5, cpu, reference, nullptr, &error));
        CHECK(pvt::render_frame(config, 5, gpu, strict, nullptr, &error));
        std::size_t maximum_index = 0U;
        const double difference = maximum_difference(reference, strict,
                                                     &maximum_index);
        if (difference > 0.0035) {
            const std::size_t pixel_index = maximum_index - maximum_index % 4U;
            std::cerr << pvt::surface_mapping_name(mapping)
                      << " opaque CPU/GPU max difference " << difference
                      << " at value " << maximum_index << " (CPU "
                      << reference.pixels[maximum_index] << ", GPU "
                      << strict.pixels[maximum_index] << "); CPU rgba [";
            for (std::size_t channel = 0U; channel < 4U; ++channel) {
                if (channel != 0U) std::cerr << ", ";
                std::cerr << reference.pixels[pixel_index + channel];
            }
            std::cerr << "], GPU rgba [";
            for (std::size_t channel = 0U; channel < 4U; ++channel) {
                if (channel != 0U) std::cerr << ", ";
                std::cerr << strict.pixels[pixel_index + channel];
            }
            std::cerr << "]\n";
        }
        CHECK(difference <= 0.0035);
    }

    const std::filesystem::path height_map =
        std::filesystem::temp_directory_path()
        / ("pvt-opengl-displacement-"
           + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count())
           + ".exr");
    pvt::Image height_image;
    height_image.width = 16;
    height_image.height = 16;
    height_image.pixels.resize(16U * 16U * 4U, 1.0F);
    for (int y = 0; y < height_image.height; ++y) {
        for (int x = 0; x < height_image.width; ++x) {
            const double dx = static_cast<double>(x) - 7.5;
            const double dy = static_cast<double>(y) - 7.5;
            const float value = static_cast<float>(std::clamp(
                1.0 - std::sqrt(dx * dx + dy * dy) / 10.7, 0.0, 1.0));
            const std::size_t offset =
                (static_cast<std::size_t>(y) * 16U
                 + static_cast<std::size_t>(x))
                * 4U;
            height_image.pixels[offset] = value;
            height_image.pixels[offset + 1U] = value;
            height_image.pixels[offset + 2U] = value;
        }
    }
    pvt::RenderConfig height_output = pvt::default_config();
    height_output.width = height_image.width;
    height_output.height = height_image.height;
    height_output.block_size = 1;
    height_output.output.bit_depth = 32;
    height_output.output.write_alpha = false;
    height_output.output.overwrite_existing = true;
    std::string error;
    const bool wrote_height_map = pvt::write_image(
        height_map.string(), height_image, height_output, 0U, &error);
    if (!wrote_height_map) {
        std::cerr << "OpenGL displacement EXR fixture: " << error << '\n';
    }
    CHECK(wrote_height_map);

    pvt::RenderConfig displaced = analytic_config(pvt::SurfaceMapping::Plane);
    displaced.surface.plane_displacement.enabled = true;
    displaced.surface.plane_displacement.path = height_map.string();
    displaced.surface.plane_displacement.minimum = -0.31;
    displaced.surface.plane_displacement.maximum = 0.44;
    displaced.surface.plane_displacement.midpoint = 0.46;
    displaced.surface.plane_displacement.pixels_per_node = 5;
    displaced.starting_colors.include_alpha = false;
    displaced.alpha.use_source_alpha = false;
    displaced.surface.outside = pvt::SurfaceOutside::Source;
    pvt::Image displaced_reference;
    pvt::Image displaced_hybrid;
    pvt::Image displaced_strict;
    CHECK(pvt::render_frame(displaced, 5, cpu, displaced_reference, nullptr,
                            &error));
    CHECK(pvt::render_frame(displaced, 5, hybrid, displaced_hybrid, nullptr,
                            &error));
    CHECK(pvt::render_frame(displaced, 5, gpu, displaced_strict, nullptr,
                            &error));
    CHECK(maximum_difference(displaced_reference, displaced_hybrid) <= 0.0035);
    CHECK(maximum_difference(displaced_reference, displaced_strict) <= 0.0035);

    pvt::RenderConfig translucent_displaced = displaced;
    translucent_displaced.starting_colors.include_alpha = true;
    translucent_displaced.alpha.use_source_alpha = true;
    translucent_displaced.surface.outside = pvt::SurfaceOutside::Transparent;
    translucent_displaced.surface.composite_backfaces = true;
    pvt::Image translucent_reference;
    pvt::Image translucent_strict;
    CHECK(pvt::render_frame(translucent_displaced, 5, cpu,
                            translucent_reference, nullptr, &error));
    CHECK(pvt::render_frame(translucent_displaced, 5, gpu,
                            translucent_strict, nullptr, &error));
    CHECK(maximum_straight_alpha_difference(
              translucent_reference, translucent_strict)
          <= 0.0035);
    std::error_code remove_error;
    std::filesystem::remove(height_map, remove_error);
    CHECK(!remove_error);

    pvt::RenderConfig flat_plane = analytic_config(pvt::SurfaceMapping::Plane);
    pvt::Image rejected;
    CHECK(pvt::render_frame(flat_plane, 0, gpu, rejected, nullptr, &error));

    pvt::RenderConfig neutral = pvt::default_config();
    neutral.width = 32;
    neutral.height = 24;
    CHECK(!pvt::render_frame(neutral, 0, gpu, rejected, nullptr, &error));
    CHECK(error.find("analytic") != std::string::npos);

    if (failures != 0) {
        std::cerr << failures << " OpenGL surface backend test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "OpenGL analytic surface acceleration and strict "
                 "unsupported-stage policy passed on "
              << capabilities.opengl_surface_device_name << ".\n";
    return EXIT_SUCCESS;
}
