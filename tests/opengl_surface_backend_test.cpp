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

double maximum_difference(const pvt::Image& first, const pvt::Image& second) {
    if (first.width != second.width || first.height != second.height
        || first.pixels.size() != second.pixels.size()) {
        return 1.0e30;
    }
    double result = 0.0;
    for (std::size_t index = 0U; index < first.pixels.size(); ++index) {
        result = (std::max)(result,
                            std::fabs(static_cast<double>(first.pixels[index])
                                      - second.pixels[index]));
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
             pvt::SurfaceMapping::Plane, pvt::SurfaceMapping::Cylinder,
             pvt::SurfaceMapping::Sphere, pvt::SurfaceMapping::Cube}) {
        const pvt::RenderConfig config = analytic_config(mapping);
        pvt::Image reference;
        pvt::Image accelerated;
        pvt::Image strict;
        std::string error;
        CHECK(pvt::render_frame(config, 5, cpu, reference, nullptr, &error));
        CHECK(pvt::render_frame(config, 5, hybrid, accelerated, nullptr,
                                &error));
        CHECK(pvt::render_frame(config, 5, gpu, strict, nullptr, &error));
        const double hybrid_difference =
            maximum_difference(reference, accelerated);
        const double strict_difference = maximum_difference(reference, strict);
        CHECK(hybrid_difference <= 0.0035);
        CHECK(strict_difference <= 0.0035);
    }

    pvt::RenderConfig displaced = analytic_config(pvt::SurfaceMapping::Plane);
    displaced.surface.plane_displacement.enabled = true;
    pvt::Image rejected;
    std::string error;
    CHECK(!pvt::render_frame(displaced, 0, gpu, rejected, nullptr, &error));
    CHECK(error.find("displacement-Plane") != std::string::npos);

    pvt::RenderConfig neutral = pvt::default_config();
    neutral.width = 32;
    neutral.height = 24;
    CHECK(!pvt::render_frame(neutral, 0, gpu, rejected, nullptr, &error));
    CHECK(error.find("analytic") != std::string::npos);

    if (failures != 0) {
        std::cerr << failures << " OpenGL surface backend test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "OpenGL analytic Plane/Cylinder/Sphere/Cube acceleration and "
                 "strict unsupported-mesh policy passed on "
              << capabilities.opengl_surface_device_name << ".\n";
    return EXIT_SUCCESS;
}
