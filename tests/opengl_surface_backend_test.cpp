#include "procedural_visualizer_tool.h"

#include <QGuiApplication>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

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

pvt::RenderConfig analytic_config(pvt::SurfaceMapping mapping) {
    pvt::RenderConfig config = pvt::default_config();
    config.width = 67;
    config.height = 53;
    config.total_frames = 17;
    config.surface.enabled = true;
    config.surface.mapping = mapping;
    config.surface.curvature = 0.82;
    config.surface.lighting = 0.47;
    config.surface.rotations_per_loop = 2;
    config.surface.phase_degrees = 17.0;
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

    for (const pvt::SurfaceMapping mapping : {
             pvt::SurfaceMapping::Cylinder, pvt::SurfaceMapping::Sphere,
             pvt::SurfaceMapping::Cube}) {
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
        const double hybrid_difference = maximum_difference(
            reference, accelerated, &hybrid_index);
        const double strict_difference = maximum_difference(
            reference, strict, &strict_index);
        if (hybrid_difference > 0.0035 || strict_difference > 0.0035) {
            pvt::RenderConfig identity_config = config;
            identity_config.surface.enabled = false;
            pvt::Image identity;
            CHECK(pvt::render_frame(identity_config, 5, cpu, identity, nullptr,
                                    &error));
            pvt::RenderConfig inverse_config = config;
            inverse_config.surface.rotations_per_loop =
                -inverse_config.surface.rotations_per_loop;
            inverse_config.surface.phase_degrees =
                -inverse_config.surface.phase_degrees;
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

    pvt::RenderConfig displaced = analytic_config(pvt::SurfaceMapping::Plane);
    displaced.surface.plane_displacement.enabled = true;
    pvt::Image rejected;
    std::string error;
    CHECK(!pvt::render_frame(displaced, 0, gpu, rejected, nullptr, &error));
    CHECK(error.find("displacement-Plane") != std::string::npos);

    pvt::RenderConfig flat_plane = analytic_config(pvt::SurfaceMapping::Plane);
    CHECK(!pvt::render_frame(flat_plane, 0, gpu, rejected, nullptr, &error));
    CHECK(error.find("flat Plane rotation") != std::string::npos);

    pvt::RenderConfig neutral = pvt::default_config();
    neutral.width = 32;
    neutral.height = 24;
    CHECK(!pvt::render_frame(neutral, 0, gpu, rejected, nullptr, &error));
    CHECK(error.find("analytic") != std::string::npos);

    if (failures != 0) {
        std::cerr << failures << " OpenGL surface backend test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "OpenGL analytic Cylinder/Sphere/Cube acceleration and strict "
                 "unsupported-Plane/mesh policy passed on "
              << capabilities.opengl_surface_device_name << ".\n";
    return EXIT_SUCCESS;
}
