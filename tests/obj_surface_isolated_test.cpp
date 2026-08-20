#include "../src/obj_surface.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>

namespace fs = std::filesystem;

namespace {

int fail(int code, const std::string& message) {
    std::cerr << message << '\n';
    return code;
}

pvt::Image uniform_image(int width, int height, float red, float green,
                         float blue, float alpha) {
    pvt::Image image;
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<std::size_t>(width)
                        * static_cast<std::size_t>(height) * 4U);
    for (std::size_t offset = 0U; offset < image.pixels.size(); offset += 4U) {
        image.pixels[offset] = red;
        image.pixels[offset + 1U] = green;
        image.pixels[offset + 2U] = blue;
        image.pixels[offset + 3U] = alpha;
    }
    return image;
}

pvt::Image uv_gradient_image(int width, int height, float alpha) {
    pvt::Image image;
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<std::size_t>(width)
                        * static_cast<std::size_t>(height) * 4U);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float* output = image.pixels.data()
                + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
                   + static_cast<std::size_t>(x)) * 4U;
            output[0] = static_cast<float>(x) / static_cast<float>(width - 1);
            output[1] = static_cast<float>(y) / static_cast<float>(height - 1);
            output[2] = 0.2F;
            output[3] = alpha;
        }
    }
    return image;
}

float* pixel(pvt::Image& image, int x, int y) {
    return image.pixels.data()
           + (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width)
              + static_cast<std::size_t>(x)) * 4U;
}

bool apply_classic_obj_surface(const pvt::Image& source,
                               pvt::Image& destination,
                               const std::string& path,
                               int rotations_per_loop,
                               double phase_degrees,
                               double curvature,
                               double lighting,
                               double loop_phase,
                               std::string* error,
                               const std::atomic_bool* cancel = nullptr) {
    pvt::SurfaceConfig surface;
    surface.enabled = true;
    surface.mapping = pvt::SurfaceMapping::CustomObj;
    surface.projection = pvt::SurfaceProjection::Perspective;
    surface.sizing = pvt::SurfaceSizing::ShortSide;
    surface.rotation_x_degrees = -20.0535228296;
    surface.rotation_y_turns_per_loop = rotations_per_loop;
    surface.rotation_y_degrees = phase_degrees + 31.5126787322;
    surface.size_percent = 104.0;
    surface.camera_distance = 3.4;
    surface.focal_length = 2.5;
    surface.curvature = curvature;
    surface.lighting = lighting;
    return pvt::detail::apply_obj_surface_mapping(
        source, destination, path, surface, loop_phase, error, cancel);
}

} // namespace

int main(int argc, char** argv) {
    const fs::path source_root = argc > 1 ? fs::path(argv[1]) : fs::current_path();
    const std::string cube =
        (source_root / "tests" / "assets" / "obj" / "closed_cube.obj").string();
    const std::string layered_uv_cube =
        (source_root / "tests" / "assets" / "obj" / "layered_uv_cube.obj").string();
    const std::string right_edge =
        (source_root / "tests" / "assets" / "obj" / "uv_right_edge.obj").string();
    std::string error;

    // A half-alpha closed shell must retain its exit surface. Two equal layers
    // composite to alpha 0.75; nearest-only rendering would remain 0.5.
    const pvt::Image translucent = uniform_image(64, 64, 0.8F, 0.3F, 0.1F, 0.5F);
    pvt::Image layered;
    if (!apply_classic_obj_surface(translucent, layered, cube,
                                   0, 0.0, 1.0, 0.0, 0.0, &error)) {
        return fail(1, "translucent cube render failed: " + error);
    }
    const float* center = pixel(layered, layered.width / 2, layered.height / 2);
    if (center[3] < 0.72F || center[3] > 0.78F
        || std::fabs(center[0] - 0.8F) > 1.0e-4F
        || std::fabs(center[1] - 0.3F) > 1.0e-4F) {
        return fail(2, "rear/exit surface was not composited through front alpha");
    }

    // A 0.75 alpha alone could result from blending the nearest sample twice.
    // Compare a UV gradient against the opaque nearest-hit fast path to prove
    // that the exit face contributes a distinct texture sample.
    const pvt::Image translucent_gradient = uv_gradient_image(64, 64, 0.5F);
    const pvt::Image opaque_gradient = uv_gradient_image(64, 64, 1.0F);
    pvt::Image layered_gradient;
    pvt::Image nearest_gradient;
    if (!apply_classic_obj_surface(
            translucent_gradient, layered_gradient, cube,
            0, 0.0, 1.0, 0.0, 0.0, &error)
        || !apply_classic_obj_surface(
            opaque_gradient, nearest_gradient, cube,
            0, 0.0, 1.0, 0.0, 0.0, &error)) {
        return fail(8, "gradient rear-surface render failed: " + error);
    }
    bool found_distinct_rear_sample = false;
    for (std::size_t offset = 0U; offset < layered_gradient.pixels.size();
         offset += 4U) {
        if (layered_gradient.pixels[offset + 3U] < 0.7F) {
            continue;
        }
        found_distinct_rear_sample = found_distinct_rear_sample
            || std::fabs(layered_gradient.pixels[offset]
                         - nearest_gradient.pixels[offset]) > 1.0e-4F
            || std::fabs(layered_gradient.pixels[offset + 1U]
                         - nearest_gradient.pixels[offset + 1U]) > 1.0e-4F;
    }
    if (!found_distinct_rear_sample) {
        return fail(9, "rear face did not contribute a distinct UV/color sample");
    }

    // Give entry and exit faces disjoint UV color regions. Correct front-over-
    // rear composition is 2/3 red + 1/3 blue at alpha 0.75.
    pvt::Image colored_source = uniform_image(8, 4, 0.0F, 0.0F, 0.0F, 0.5F);
    for (int y = 0; y < colored_source.height; ++y) {
        for (int x = 0; x < colored_source.width; ++x) {
            float* value = pixel(colored_source, x, y);
            value[0] = x < colored_source.width / 2 ? 1.0F : 0.0F;
            value[2] = x < colored_source.width / 2 ? 0.0F : 1.0F;
        }
    }
    pvt::Image colored_layers;
    constexpr double cancel_initial_y_degrees = -31.51267873219528;
    if (!apply_classic_obj_surface(
            colored_source, colored_layers, layered_uv_cube, 0,
            cancel_initial_y_degrees, 1.0, 0.0, 0.0, &error)) {
        return fail(10, "colored layered cube render failed: " + error);
    }
    const float* colored_center = pixel(colored_layers, colored_layers.width / 2,
                                        colored_layers.height / 2);
    if (colored_center[3] < 0.72F || colored_center[3] > 0.78F
        || colored_center[0] < 0.60F || colored_center[0] > 0.72F
        || colored_center[2] < 0.27F || colored_center[2] > 0.40F) {
        return fail(11, "rear UV color was not composited behind the front UV color: rgba="
                            + std::to_string(colored_center[0]) + ","
                            + std::to_string(colored_center[1]) + ","
                            + std::to_string(colored_center[2]) + ","
                            + std::to_string(colored_center[3]));
    }

    // The canonical 0..1 OBJ interval keeps u=1 on the right edge. Only
    // authored values outside that interval use tiled repeat behavior.
    pvt::Image edge_source = uniform_image(8, 4, 1.0F, 0.0F, 0.0F, 1.0F);
    for (int y = 0; y < edge_source.height; ++y) {
        float* right = pixel(edge_source, edge_source.width - 1, y);
        right[0] = 0.0F;
        right[2] = 1.0F;
    }
    pvt::Image edge_result;
    if (!apply_classic_obj_surface(
            edge_source, edge_result, right_edge, 0, cancel_initial_y_degrees,
            1.0, 0.0, 0.0, &error)) {
        return fail(12, "UV boundary render failed: " + error);
    }
    const float* edge_center = pixel(edge_result, edge_result.width / 2,
                                     edge_result.height / 2);
    if (edge_center[2] < 0.99F || edge_center[0] > 0.01F) {
        return fail(13, "canonical authored u=1 wrapped to the left edge");
    }

    // Curvature zero is exactly neutral and does not require a readable mesh.
    pvt::Image neutral;
    if (!apply_classic_obj_surface(translucent, neutral,
                                   "/path/that/does/not/exist.obj",
                                   3, 91.0, 0.0, 8.0, 4.0, &error)
        || neutral.pixels != translucent.pixels) {
        return fail(3, "zero-curvature OBJ mapping was not neutral");
    }

    // Opaque input takes the nearest-only fast path and still renders every
    // winding: coverage is opaque at the center and transparent outside.
    const pvt::Image opaque = uniform_image(64, 64, 0.2F, 0.6F, 0.9F, 1.0F);
    pvt::Image nearest;
    if (!apply_classic_obj_surface(opaque, nearest, cube,
                                   1, 17.0, 1.0, 0.0, 1.25, &error)) {
        return fail(4, "opaque cube render failed: " + error);
    }
    const float* opaque_center = pixel(nearest, nearest.width / 2,
                                       nearest.height / 2);
    const float* corner = pixel(nearest, 0, 0);
    if (opaque_center[3] < 0.999F || corner[3] > 1.0e-6F) {
        return fail(5, "opaque nearest/exterior coverage is incorrect");
    }

    // Custom OBJ lighting must honor authored values above the former hidden
    // renderer clamp of 10, just like the analytic surfaces and editor.
    pvt::Image lighting_ten;
    pvt::Image lighting_eleven;
    if (!apply_classic_obj_surface(
            opaque, lighting_ten, right_edge,
            0, 5.729577951, 1.0, 10.0, 0.0, &error)
        || !apply_classic_obj_surface(
            opaque, lighting_eleven, right_edge,
            0, 5.729577951, 1.0, 11.0, 0.0, &error)
        || lighting_ten.pixels == lighting_eleven.pixels) {
        return fail(15, "custom OBJ lighting still clamps authored values at 10");
    }

    // Integer rotations close at the loop endpoint.
    pvt::Image seam_start;
    pvt::Image seam_end;
    constexpr double tau = 6.283185307179586476925286766559;
    if (!apply_classic_obj_surface(opaque, seam_start, cube,
                                   2, -31.0, 1.0, 0.2, 0.0, &error)
        || !apply_classic_obj_surface(opaque, seam_end, cube,
                                      2, -31.0, 1.0, 0.2, tau, &error)
        || seam_start.pixels != seam_end.pixels) {
        return fail(6, "OBJ rotation does not close exactly at the loop seam");
    }

    // Failure is transactional.
    pvt::Image unchanged = nearest;
    if (apply_classic_obj_surface(opaque, unchanged,
                                  "/path/that/does/not/exist.obj",
                                  0, 0.0, 1.0, 0.0, 0.0, &error)
        || unchanged.pixels != nearest.pixels) {
        return fail(7, "OBJ render failure changed the destination");
    }

    std::atomic_bool cancelled {true};
    unchanged = nearest;
    if (apply_classic_obj_surface(
            opaque, unchanged, cube, 0, 0.0, 1.0, 0.0, 0.0, &error,
            &cancelled)
        || error.find("cancelled") == std::string::npos
        || unchanged.pixels != nearest.pixels) {
        return fail(14, "OBJ cancellation was not transactional");
    }

    std::cout << "OBJ surface isolated tests passed\n";
    return 0;
}
