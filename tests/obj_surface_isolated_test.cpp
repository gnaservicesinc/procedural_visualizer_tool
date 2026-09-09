#include "../src/obj_surface.h"
#include "../src/packed_mask.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <iomanip>
#include <sstream>
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
                               const std::atomic_bool* cancel = nullptr,
                               const pvt::MeshConstructionConfig* construction =
                                   nullptr) {
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
    if (construction != nullptr) {
        surface.mesh_construction = *construction;
    }
    return pvt::detail::apply_obj_surface_mapping(
        source, destination, path, surface, loop_phase, error, cancel);
}

bool apply_neutral_obj_surface(const pvt::Image& source,
                               pvt::Image& destination,
                               const std::string& path,
                               std::string* error) {
    pvt::SurfaceConfig surface;
    surface.enabled = true;
    surface.mapping = pvt::SurfaceMapping::CustomObj;
    surface.projection = pvt::SurfaceProjection::Orthographic;
    surface.sizing = pvt::SurfaceSizing::Contain;
    surface.outside = pvt::SurfaceOutside::Transparent;
    surface.curvature = 1.0;
    surface.lighting = 0.0;
    return pvt::detail::apply_obj_surface_mapping(
        source, destination, path, surface, 0.0, error, nullptr);
}

} // namespace

// Independent ray/triangle oracle: validates clipping coverage and interpolated
// UVs without reproducing the polygon-clipping implementation.
bool check_camera_crossing(pvt::SurfaceProjection projection, bool two_behind,
                           bool reverse_winding, float alpha, std::string& error) {
    using pvt::detail::ObjVec3;
    const auto subtract = [](ObjVec3 a, ObjVec3 b) {
        return ObjVec3{a.x - b.x, a.y - b.y, a.z - b.z};
    };
    const auto cross = [](ObjVec3 a, ObjVec3 b) {
        return ObjVec3{a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
    };
    const auto dot = [](ObjVec3 a, ObjVec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; };
    pvt::detail::ObjMesh mesh;
    mesh.positions = {{-0.9, -0.8, 0.0}, {0.8, -0.7, two_behind ? 1.7 : 0.1},
                      {0.1, 0.9, 1.8}};
    mesh.texcoords = {{0.1, 0.15}, {0.85, 0.2}, {0.55, 0.9}};
    mesh.normals = {{0.0, 0.0, 1.0}};
    pvt::detail::ObjTriangle triangle;
    triangle.corners = {{{0U, 0U, 0U}, {1U, 1U, 0U}, {2U, 2U, 0U}}};
    if (reverse_winding) std::swap(triangle.corners[1], triangle.corners[2]);
    mesh.triangles.push_back(triangle);
    pvt::SurfaceConfig surface;
    surface.enabled = true;
    surface.mapping = pvt::SurfaceMapping::CustomObj;
    surface.normalize_obj = false;
    surface.sizing = pvt::SurfaceSizing::ShortSide;
    surface.projection = projection;
    surface.camera_distance = 1.0;
    surface.focal_length = 1.0;
    pvt::Image output;
    if (!pvt::detail::apply_mesh_surface_mapping(
            uv_gradient_image(65, 63, alpha), output, mesh, surface, 0.0, &error)) return false;
    const ObjVec3 e1 = subtract(mesh.positions[1], mesh.positions[0]);
    const ObjVec3 e2 = subtract(mesh.positions[2], mesh.positions[0]);
    std::size_t checked_hits = 0U;
    for (int y = 0; y < output.height; ++y) {
        for (int x = 0; x < output.width; ++x) {
            const double sx = (x + 0.5 - 32.0) / 31.5;
            const double sy = (31.0 - y - 0.5) / 31.5;
            const bool perspective = projection == pvt::SurfaceProjection::Perspective;
            const ObjVec3 origin = perspective ? ObjVec3{0.0, 0.0, 1.0} : ObjVec3{sx, sy, 1.0};
            const ObjVec3 direction = perspective ? ObjVec3{sx, sy, -1.0} : ObjVec3{0.0, 0.0, -1.0};
            const auto p = cross(direction, e2);
            const double determinant = dot(e1, p);
            if (std::abs(determinant) < 1.0e-10) continue;
            const auto t = subtract(origin, mesh.positions[0]);
            const auto q = cross(t, e1);
            const double u = dot(t, p) / determinant;
            const double v = dot(direction, q) / determinant;
            const double distance = dot(e2, q) / determinant;
            // Exclude exact shared-edge ties from this independent oracle.
            if (std::abs(u) < 1.0e-8 || std::abs(v) < 1.0e-8
                || std::abs(u + v - 1.0) < 1.0e-8
                || std::abs(distance - 1.0e-6) < 1.0e-8) continue;
            const bool hit = u > 0.0 && v > 0.0 && u + v < 1.0 && distance >= 1.0e-6;
            const float* actual = pixel(output, x, y);
            if (!hit) {
                if (actual[3] != 0.0F) { error = "clipped triangle painted outside ray intersection"; return false; }
                continue;
            }
            ++checked_hits;
            const double expected_u = (1.0-u-v)*0.1 + u*0.85 + v*0.55;
            const double expected_v = (1.0-u-v)*0.15 + u*0.2 + v*0.9;
            if (std::abs(actual[0] - expected_u) > 2.0e-5
                || std::abs(actual[1] - (1.0-expected_v)) > 2.0e-5
                || std::abs(actual[3] - alpha) > 1.0e-6) {
                error = "camera clipping disagrees with independent ray/UV oracle";
                return false;
            }
        }
    }
    return checked_hits > 10U;
}

int main(int argc, char** argv) {
    for (std::size_t size : {0U, 1U, 63U, 64U, 65U, 127U, 129U}) {
        pvt::detail::PackedMask mask(size);
        for (std::size_t bit = 0; bit < size; ++bit) {
            if (mask.test(bit)) return fail(30, "coverage was not zero initialized");
            if (bit % 3U == 0U) mask.set(bit);
        }
        for (std::size_t bit = 0; bit < size; ++bit) {
            if (mask.test(bit) != (bit % 3U == 0U)) {
                return fail(31, "packed coverage changed a neighboring bit");
            }
        }
    }
    const fs::path source_root = argc > 1 ? fs::path(argv[1]) : fs::current_path();
    const std::string cube =
        (source_root / "tests" / "assets" / "obj" / "closed_cube.obj").string();
    const std::string layered_uv_cube =
        (source_root / "tests" / "assets" / "obj" / "layered_uv_cube.obj").string();
    const std::string right_edge =
        (source_root / "tests" / "assets" / "obj" / "uv_right_edge.obj").string();
    const std::string multi_part =
        (source_root / "tests" / "assets" / "obj" / "multi_part_assembly.obj").string();
    std::string error;
    for (auto projection : {pvt::SurfaceProjection::Perspective,
                             pvt::SurfaceProjection::Orthographic}) {
        for (bool two_behind : {false, true}) {
            for (bool reverse : {false, true}) {
                for (float alpha : {0.5F, 1.0F}) {
                    if (!check_camera_crossing(projection, two_behind, reverse, alpha, error)) {
                        return fail(38, "near-plane regression: " + error);
                    }
                }
            }
        }
    }

    // A half-alpha closed shell must retain its exit surface. Two equal layers
    // composite to alpha 0.75; nearest-only rendering would remain 0.5.
    const pvt::Image translucent = uniform_image(64, 64, 0.8F, 0.3F, 0.1F, 0.5F);
    pvt::detail::ObjMesh inside_mesh;
    if (!pvt::detail::load_obj_mesh(cube, inside_mesh, &error)) return fail(39, error);
    pvt::SurfaceConfig inside_surface;
    inside_surface.enabled = true;
    inside_surface.mapping = pvt::SurfaceMapping::CustomObj;
    inside_surface.projection = pvt::SurfaceProjection::Perspective;
    inside_surface.camera_distance = 0.5;
    inside_surface.focal_length = 0.3;
    pvt::Image inside, inside_reversed;
    if (!pvt::detail::apply_mesh_surface_mapping(translucent, inside, inside_mesh,
            inside_surface, 0.0, &error)) return fail(39, error);
    for (auto& triangle : inside_mesh.triangles) std::swap(triangle.corners[1], triangle.corners[2]);
    if (!pvt::detail::apply_mesh_surface_mapping(translucent, inside_reversed, inside_mesh,
            inside_surface, 0.0, &error)
        || inside.pixels != inside_reversed.pixels || pixel(inside, 32, 32)[3] != 0.5F) {
        std::ostringstream detail;
        detail << std::setprecision(17) << "camera-inside two-sided shell changed with winding: "
               << error << "; center alpha=" << pixel(inside, 32, 32)[3]
               << "; components=" << inside.pixels.size() << '/' << inside_reversed.pixels.size();
        const std::size_t common = std::min(inside.pixels.size(), inside_reversed.pixels.size());
        for (std::size_t i = 0U; i < common; ++i) {
            if (inside.pixels[i] != inside_reversed.pixels[i]) {
                detail << "; first mismatch pixel=(" << (i / 4U) % 64U << ','
                       << (i / 4U) / 64U << "), channel=" << i % 4U
                       << ": " << inside.pixels[i] << " versus " << inside_reversed.pixels[i];
                break;
            }
        }
        return fail(39, detail.str());
    }
    // Every cyclic/reversed ordering of a crossing triangle must produce the
    // same fan and attributes, including compilers that contract multiply/add.
    pvt::detail::ObjMesh crossing;
    crossing.positions = {{-0.9, -0.8, 0.0}, {0.8, -0.7, 0.1}, {0.1, 0.9, 1.8}};
    crossing.texcoords = {{0.1, 0.15}, {0.85, 0.2}, {0.55, 0.9}};
    pvt::detail::ObjTriangle crossing_triangle;
    crossing_triangle.corners = {{{0U, 0U, pvt::detail::ObjCorner::missing},
                                  {1U, 1U, pvt::detail::ObjCorner::missing},
                                  {2U, 2U, pvt::detail::ObjCorner::missing}}};
    crossing.triangles.push_back(crossing_triangle);
    inside_surface.normalize_obj = false;
    inside_surface.camera_distance = 1.0;
    std::array<unsigned, 3U> order = {0U, 1U, 2U};
    pvt::Image canonical_crossing;
    do {
        for (unsigned corner = 0U; corner < 3U; ++corner) {
            crossing.triangles[0].corners[corner] = crossing_triangle.corners[order[corner]];
        }
        if (!pvt::detail::apply_mesh_surface_mapping(uv_gradient_image(65, 63, 0.5F), inside, crossing,
                                                     inside_surface, 0.0, &error))
            return fail(41, error);
        if (canonical_crossing.pixels.empty())
            canonical_crossing = inside;
        else if (canonical_crossing.pixels != inside.pixels) {
            return fail(41, "clipped fan depends on source corner order");
        }
    } while (std::next_permutation(order.begin(), order.end()));

    // Covered rear faces can be rejected only after depth proves them hidden.
    // Exercise both windings, projections, partial/absent coverage, clipping,
    // nearest-only alpha and actual depth peeling against the unculled path.
    pvt::detail::ObjMesh hidden_faces;
    hidden_faces.positions = {{-10, -10, 2.9}, {10, -10, 2.9}, {0, 10, 2.9},
                              {-2, -2, -10},   {2, -2, -10},   {0, 2, -10}};
    hidden_faces.texcoords = {{0.1, 0.1}, {0.9, 0.1}, {0.5, 0.9}};
    pvt::detail::ObjTriangle front, rear;
    for (unsigned i = 0; i < 3U; ++i) {
        front.corners[i] = {i, i, pvt::detail::ObjCorner::missing};
        rear.corners[i] = {i + 3U, i, pvt::detail::ObjCorner::missing};
    }
    hidden_faces.triangles.assign(256U, rear);
    hidden_faces.triangles.insert(hidden_faces.triangles.begin(), front);
    inside_surface.camera_distance = 3.0;
    inside_surface.focal_length = 1.0;
    inside_surface.sizing = pvt::SurfaceSizing::ShortSide;
    for (auto projection :
         {pvt::SurfaceProjection::Perspective, pvt::SurfaceProjection::Orthographic}) {
        inside_surface.projection = projection;
        for (double front_z : {2.9, 0.0, 3.1}) {
            for (unsigned i = 0; i < 3U; ++i)
                hidden_faces.positions[i].z = front_z;
            for (bool reverse : {false, true}) {
                if (reverse)
                    for (auto& triangle : hidden_faces.triangles)
                        std::swap(triangle.corners[1], triangle.corners[2]);
                for (float alpha : {1.0F, 0.5F}) {
                    for (bool composite : {false, true}) {
                        inside_surface.composite_backfaces = composite;
                        pvt::Image reference, actual;
                        const auto input = uv_gradient_image(65, 63, alpha);
                        if (!pvt::detail::apply_mesh_surface_mapping(input, reference, hidden_faces,
                                                                     inside_surface, 0.0, &error,
                                                                     nullptr, false) ||
                            !pvt::detail::apply_mesh_surface_mapping(input, actual, hidden_faces,
                                                                     inside_surface, 0.0, &error,
                                                                     nullptr, true) ||
                            reference.pixels != actual.pixels)
                            return fail(42, "occlusion rejection changed mesh pixels: " + error);
                    }
                }
            }
        }
    }
    inside_surface.composite_backfaces = true;
    std::mt19937 random(4317U);
    const auto coordinate = [&] { return (static_cast<double>(random() % 20001U) - 10000.0) / 4000.0; };
    for (unsigned i = 0; i < 192U; ++i) {
        pvt::detail::ObjTriangle added;
        for (unsigned j = 0; j < 3U; ++j) {
            added.corners[j] = {static_cast<std::uint32_t>(hidden_faces.positions.size()), j,
                                pvt::detail::ObjCorner::missing};
            hidden_faces.positions.push_back({coordinate(), coordinate(), coordinate()});
        }
        hidden_faces.triangles.push_back(added);
    }
    for (auto projection :
         {pvt::SurfaceProjection::Perspective, pvt::SurfaceProjection::Orthographic}) {
        inside_surface.projection = projection;
        for (double z : {1.7, -0.3}) {
            for (unsigned i = 0; i < 3U; ++i)
                hidden_faces.positions[i].z = z;
            for (double scale : {1.0, -1.0}) {
                inside_surface.scale_x = scale;
                pvt::Image reference, actual;
                const auto input = uv_gradient_image(65, 63, 1.0F);
                if (!pvt::detail::apply_mesh_surface_mapping(
                        input, reference, hidden_faces, inside_surface, 0.0, &error, nullptr, false) ||
                    !pvt::detail::apply_mesh_surface_mapping(
                        input, actual, hidden_faces, inside_surface, 0.0, &error, nullptr, true) ||
                    reference.pixels != actual.pixels)
                    return fail(43, "mixed visible/occluded faces changed pixels: " + error);
            }
        }
    }
    inside_surface.scale_x = 1.0;

    pvt::detail::ObjMesh near_mesh;
    near_mesh.positions = {{-1,-1,0}, {1,-1,0}, {0,1,0}};
    pvt::detail::ObjTriangle near_triangle;
    near_triangle.corners[0].position = 0U;
    near_triangle.corners[1].position = 1U;
    near_triangle.corners[2].position = 2U;
    near_mesh.triangles.push_back(near_triangle);
    inside_surface.projection = pvt::SurfaceProjection::Orthographic;
    inside_surface.camera_distance = 1.0e-6;
    if (!pvt::detail::apply_mesh_surface_mapping(translucent, inside, near_mesh,
            inside_surface, 0.0, &error) || pixel(inside, 32, 32)[3] != 0.5F) {
        return fail(40, "triangle on the near plane was discarded: " + error);
    }
    inside_surface.position_z = 2.0e-6;
    if (!pvt::detail::apply_mesh_surface_mapping(translucent, inside, near_mesh,
            inside_surface, 0.0, &error)
        || std::any_of(inside.pixels.begin(), inside.pixels.end(), [](float value) { return value != 0.0F; })) {
        return fail(40, "entirely behind triangle was not culled: " + error);
    }
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

    // Standard o/g records may divide one file into disconnected named parts.
    // Both parts must survive import and share one normalization/projection;
    // a loader that stops at or replaces on the second object fails this.
    pvt::Image multi_part_result;
    if (!apply_neutral_obj_surface(
            opaque, multi_part_result, multi_part, &error)) {
        return fail(24, "multi-part OBJ render failed: " + error);
    }
    const float* left_part = pixel(multi_part_result, 12, 32);
    const float* part_gap = pixel(multi_part_result, 32, 32);
    const float* right_part = pixel(multi_part_result, 51, 32);
    if (left_part[3] < 0.999F || right_part[3] < 0.999F
        || part_gap[3] > 1.0e-6F) {
        return fail(25,
                    "disconnected OBJ parts were not rendered as one assembly");
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

    // Construction animation uses the single-shell cube's deterministic
    // triangle-cluster fallback. Every mode must close exactly even across
    // multiple authored cycles; assembled states bypass fragment arithmetic so
    // they are bit-identical to the unanimated mesh.
    pvt::Image construction_baseline;
    if (!apply_classic_obj_surface(opaque, construction_baseline, cube,
                                   0, -31.0, 1.0, 0.2, 0.0, &error)) {
        return fail(16, "construction baseline render failed: " + error);
    }
    pvt::MeshConstructionConfig construction;
    construction.fragmentation = pvt::MeshFragmentation::Automatic;
    construction.target_fragments = 8;
    construction.cycles_per_loop = 3;
    construction.distance = 0.35;
    construction.rotation_degrees = 70.0;
    construction.minimum_scale = 0.6;
    construction.stagger = 0.65;
    construction.seed = 1947U;
    constexpr double construction_peak = tau / 6.0;
    for (const pvt::MeshConstructionMode mode : {
             pvt::MeshConstructionMode::Explode,
             pvt::MeshConstructionMode::Deconstruct,
             pvt::MeshConstructionMode::Reconstruct}) {
        construction.mode = mode;
        pvt::Image start;
        pvt::Image end;
        pvt::Image peak;
        if (!apply_classic_obj_surface(
                opaque, start, cube, 0, -31.0, 1.0, 0.2, 0.0, &error,
                nullptr, &construction)
            || !apply_classic_obj_surface(
                opaque, end, cube, 0, -31.0, 1.0, 0.2, tau, &error,
                nullptr, &construction)
            || !apply_classic_obj_surface(
                opaque, peak, cube, 0, -31.0, 1.0, 0.2,
                construction_peak, &error, nullptr, &construction)
            || start.pixels != end.pixels) {
            return fail(17, "mesh construction did not close exactly: " + error);
        }
        if (mode == pvt::MeshConstructionMode::Reconstruct) {
            if (start.pixels == construction_baseline.pixels
                || peak.pixels != construction_baseline.pixels) {
                return fail(18,
                            "reconstruct seam/midpoint states are incorrect");
            }
        } else if (start.pixels != construction_baseline.pixels
                   || peak.pixels == construction_baseline.pixels) {
            return fail(19,
                        "explode/deconstruct seam/midpoint states are incorrect");
        }
    }

    construction.mode = pvt::MeshConstructionMode::Explode;
    construction.seed = 31U;
    pvt::Image seeded_first;
    pvt::Image seeded_repeat;
    pvt::Image seeded_other;
    if (!apply_classic_obj_surface(
            opaque, seeded_first, cube, 0, -31.0, 1.0, 0.2,
            construction_peak, &error, nullptr, &construction)
        || !apply_classic_obj_surface(
            opaque, seeded_repeat, cube, 0, -31.0, 1.0, 0.2,
            construction_peak, &error, nullptr, &construction)) {
        return fail(20, "seeded construction render failed: " + error);
    }
    construction.seed = 32U;
    if (!apply_classic_obj_surface(
            opaque, seeded_other, cube, 0, -31.0, 1.0, 0.2,
            construction_peak, &error, nullptr, &construction)
        || seeded_first.pixels != seeded_repeat.pixels
        || seeded_first.pixels == seeded_other.pixels) {
        return fail(21, "mesh construction seed is not stable and meaningful");
    }

    construction.mode = pvt::MeshConstructionMode::Deconstruct;
    construction.cycles_per_loop = -2;
    construction.phase_degrees = 23.0;
    pvt::Image reverse_start;
    pvt::Image reverse_end;
    if (!apply_classic_obj_surface(
            opaque, reverse_start, cube, 0, -31.0, 1.0, 0.2, 0.0,
            &error, nullptr, &construction)
        || !apply_classic_obj_surface(
            opaque, reverse_end, cube, 0, -31.0, 1.0, 0.2, tau,
            &error, nullptr, &construction)
        || reverse_start.pixels != reverse_end.pixels) {
        return fail(22, "negative construction cycles did not close exactly");
    }

    construction.cycles_per_loop = 0;
    construction.phase_degrees = 90.0;
    pvt::Image static_start;
    pvt::Image static_later;
    if (!apply_classic_obj_surface(
            opaque, static_start, cube, 0, -31.0, 1.0, 0.2, 0.0,
            &error, nullptr, &construction)
        || !apply_classic_obj_surface(
            opaque, static_later, cube, 0, -31.0, 1.0, 0.2, 1.2345,
            &error, nullptr, &construction)
        || static_start.pixels != static_later.pixels
        || static_start.pixels == construction_baseline.pixels) {
        return fail(23, "zero-cycle construction was not a valid static state");
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

    // A representably translucent front must retain even a very faint exit
    // contribution. The old near-opaque cutoff kept alpha below one here.
    pvt::Image nearly_opaque = uniform_image(
        65, 63, 0.8F, 0.3F, 0.1F, std::nextafter(1.0F, 0.0F));
    pvt::Image faint_rear;
    if (!apply_classic_obj_surface(nearly_opaque, faint_rear, cube,
                                   0, 0.0, 1.0, 0.0, 0.0, &error)) {
        return fail(32, "near-opaque render failed: " + error);
    }
    bool accumulated_to_one = false;
    for (std::size_t offset = 3U; offset < faint_rear.pixels.size(); offset += 4U) {
        accumulated_to_one |= faint_rear.pixels[offset] == 1.0F;
    }
    if (!accumulated_to_one) return fail(33, "faint rear coverage was discarded");

    pvt::detail::ObjMesh quad;
    if (!pvt::detail::parse_obj_mesh(
            "v -1 -1 0\nv 1 -1 0\nv 1 1 0\nv -1 1 0\nf 1 2 3 4\n",
            quad, &error)) return fail(34, error);
    pvt::SurfaceConfig extreme;
    extreme.enabled = true;
    extreme.mapping = pvt::SurfaceMapping::CustomObj;
    extreme.scale_x = 1.0e12;
    extreme.scale_y = 1.0e12;
    pvt::Image enormous;
    if (!pvt::detail::apply_mesh_surface_mapping(
            opaque, enormous, quad, extreme, 0.0, &error)
        || enormous.pixels != opaque.pixels) {
        return fail(35, "extreme triangle clipping lost coverage: " + error);
    }
    for (auto& triangle : quad.triangles) {
        std::swap(triangle.corners[1], triangle.corners[2]);
    }
    pvt::Image reverse_winding;
    if (!pvt::detail::apply_mesh_surface_mapping(
            opaque, reverse_winding, quad, extreme, 0.0, &error)
        || reverse_winding.pixels != enormous.pixels) {
        return fail(36, "two-sided surface changed with winding: " + error);
    }
    extreme.position_x_percent = 1.0e16;
    if (!pvt::detail::apply_mesh_surface_mapping(
            opaque, enormous, quad, extreme, 0.0, &error)
        || std::any_of(enormous.pixels.begin(), enormous.pixels.end(),
                       [](float component) { return component != 0.0F; })) {
        return fail(37, "offscreen triangle was not rejected: " + error);
    }

    std::cout << "OBJ surface isolated tests passed\n";
    return 0;
}
