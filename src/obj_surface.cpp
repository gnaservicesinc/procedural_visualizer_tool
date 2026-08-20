#include "obj_surface.h"

#include "obj_mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace pvt {
namespace detail {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kTau = 2.0 * kPi;
constexpr double kMinimumCameraDepth = 1.0e-6;
constexpr double kOpaqueThreshold = 1.0 - 1.0e-7;
constexpr double kDepthRelativeEpsilon = 1.0e-6;

struct ObjSurfaceCancelled final {};

void throw_if_cancelled(const std::atomic_bool* cancel) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
        throw ObjSurfaceCancelled{};
    }
}

struct Color {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 0.0;
};

struct ScreenPoint {
    double x = 0.0;
    double y = 0.0;
};

struct ProjectedVertex {
    ObjVec3 object{};
    ObjVec3 world{};
    ScreenPoint screen{};
    double camera_depth = 0.0;
    double inverse_depth = 0.0;
    bool valid = false;
};

struct RasterVertex {
    const ProjectedVertex* projected = nullptr;
    ObjVec2 uv{};
    ObjVec3 normal{};
};

void clear_error(std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
}

bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

double clamp_value(double value, double minimum, double maximum) {
    return std::max(minimum, std::min(maximum, value));
}

bool checked_multiply(std::size_t first, std::size_t second,
                      std::size_t& result) {
    if (first != 0U && second > std::numeric_limits<std::size_t>::max() / first) {
        return false;
    }
    result = first * second;
    return true;
}

ObjVec3 add(ObjVec3 first, ObjVec3 second) {
    return {first.x + second.x, first.y + second.y, first.z + second.z};
}

ObjVec3 subtract(ObjVec3 first, ObjVec3 second) {
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}

ObjVec3 multiply(ObjVec3 value, double amount) {
    return {value.x * amount, value.y * amount, value.z * amount};
}

double dot(ObjVec3 first, ObjVec3 second) {
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

ObjVec3 cross(ObjVec3 first, ObjVec3 second) {
    return {first.y * second.z - first.z * second.y,
            first.z * second.x - first.x * second.z,
            first.x * second.y - first.y * second.x};
}

ObjVec3 normalize(ObjVec3 value) {
    const double squared = dot(value, value);
    if (!std::isfinite(squared) || squared <= 1.0e-24) {
        return {};
    }
    return multiply(value, 1.0 / std::sqrt(squared));
}

ObjVec3 rotate_x(ObjVec3 value, double angle) {
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    return {value.x, cosine * value.y - sine * value.z,
            sine * value.y + cosine * value.z};
}

ObjVec3 rotate_y(ObjVec3 value, double angle) {
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    return {cosine * value.x + sine * value.z, value.y,
            -sine * value.x + cosine * value.z};
}

ObjVec3 rotate_z(ObjVec3 value, double angle) {
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    return {cosine * value.x - sine * value.y,
            sine * value.x + cosine * value.y, value.z};
}

struct SurfaceAngles {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

SurfaceAngles surface_angles(const SurfaceConfig& surface, double loop_phase) {
    return {
        surface.rotation_x_degrees * kPi / 180.0
            + static_cast<double>(surface.rotation_x_turns_per_loop)
                  * loop_phase,
        surface.rotation_y_degrees * kPi / 180.0
            + static_cast<double>(surface.rotation_y_turns_per_loop)
                  * loop_phase,
        surface.rotation_z_degrees * kPi / 180.0
            + static_cast<double>(surface.rotation_z_turns_per_loop)
                  * loop_phase};
}

ObjVec3 rotate_surface(ObjVec3 value, const SurfaceAngles& angles,
                       SurfaceRotationOrder order) {
    switch (order) {
        case SurfaceRotationOrder::XYZ:
            return rotate_z(rotate_y(rotate_x(value, angles.x), angles.y),
                            angles.z);
        case SurfaceRotationOrder::XZY:
            return rotate_y(rotate_z(rotate_x(value, angles.x), angles.z),
                            angles.y);
        case SurfaceRotationOrder::YXZ:
            return rotate_z(rotate_x(rotate_y(value, angles.y), angles.x),
                            angles.z);
        case SurfaceRotationOrder::YZX:
            return rotate_x(rotate_z(rotate_y(value, angles.y), angles.z),
                            angles.x);
        case SurfaceRotationOrder::ZXY:
            return rotate_y(rotate_x(rotate_z(value, angles.z), angles.x),
                            angles.y);
        case SurfaceRotationOrder::ZYX:
            return rotate_x(rotate_y(rotate_z(value, angles.z), angles.y),
                            angles.x);
    }
    return value;
}

Color load_color(const Image& image, std::size_t pixel) {
    const std::size_t offset = pixel * 4U;
    return {image.pixels[offset], image.pixels[offset + 1U],
            image.pixels[offset + 2U], image.pixels[offset + 3U]};
}

void store_color(Image& image, std::size_t pixel, const Color& color) {
    const std::size_t offset = pixel * 4U;
    image.pixels[offset] = static_cast<float>(color.r);
    image.pixels[offset + 1U] = static_cast<float>(color.g);
    image.pixels[offset + 2U] = static_cast<float>(color.b);
    image.pixels[offset + 3U] = static_cast<float>(clamp_value(color.a, 0.0, 1.0));
}

Color mix_color(Color first, Color second, double amount) {
    amount = clamp_value(amount, 0.0, 1.0);
    const double first_weight = 1.0 - amount;
    return {first.r * first_weight + second.r * amount,
            first.g * first_weight + second.g * amount,
            first.b * first_weight + second.b * amount,
            first.a * first_weight + second.a * amount};
}

Color composite_over(Color front, Color back) {
    const double front_alpha = clamp_value(front.a, 0.0, 1.0);
    const double back_weight = clamp_value(back.a, 0.0, 1.0)
                               * (1.0 - front_alpha);
    const double output_alpha = front_alpha + back_weight;
    if (output_alpha <= 1.0e-12) {
        // The first peeled fragment's RGB is installed before compositing.
        // Retain that useful straight RGB if every layer has zero coverage.
        return {front.r, front.g, front.b, 0.0};
    }
    return {(front.r * front_alpha + back.r * back_weight) / output_alpha,
            (front.g * front_alpha + back.g * back_weight) / output_alpha,
            (front.b * front_alpha + back.b * back_weight) / output_alpha,
            output_alpha};
}

Color shade(Color color, ObjVec3 normal, const SurfaceConfig& surface,
            double lighting) {
    const ObjVec3 light = normalize({surface.light_direction_x,
                                     surface.light_direction_y,
                                     surface.light_direction_z});
    const double diffuse = std::max(0.0, dot(normalize(normal), light));
    const double lit = surface.light_ambient
                       + surface.light_diffuse * diffuse;
    const double multiplier = std::max(0.0, 1.0 + lighting * (lit - 1.0));
    color.r *= multiplier;
    color.g *= multiplier;
    color.b *= multiplier;
    return color;
}

Color sample_source(const Image& source, ObjVec2 uv, bool repeat_u) {
    const double width = static_cast<double>(source.width);
    // Keep canonical OBJ endpoints intuitive (u=1 reaches the rightmost texel)
    // while repeating authored tiled coordinates outside the 0..1 interval.
    const bool tiled_u = repeat_u && (uv.x < 0.0 || uv.x > 1.0);
    double sample_x = tiled_u ? uv.x * width
                              : clamp_value(uv.x, 0.0, 1.0)
                                    * static_cast<double>(source.width - 1);
    if (tiled_u) {
        sample_x = std::fmod(sample_x, width);
        if (sample_x < 0.0) {
            sample_x += width;
        }
    }
    const double sample_y = clamp_value(1.0 - uv.y, 0.0, 1.0)
                            * static_cast<double>(source.height - 1);
    const long long x0_unwrapped = static_cast<long long>(std::floor(sample_x));
    const long long y0_unbounded = static_cast<long long>(std::floor(sample_y));
    const int x0 = tiled_u
        ? static_cast<int>(x0_unwrapped % source.width)
        : static_cast<int>(clamp_value(static_cast<double>(x0_unwrapped), 0.0,
                                      static_cast<double>(source.width - 1)));
    const int x1 = tiled_u ? (x0 + 1) % source.width
                           : std::min(x0 + 1, source.width - 1);
    const int y0 = static_cast<int>(clamp_value(static_cast<double>(y0_unbounded), 0.0,
                                               static_cast<double>(source.height - 1)));
    const int y1 = std::min(y0 + 1, source.height - 1);
    const double x_fraction = sample_x - std::floor(sample_x);
    const double y_fraction = sample_y - std::floor(sample_y);
    const std::array<std::size_t, 4U> pixels = {
        static_cast<std::size_t>(y0) * static_cast<std::size_t>(source.width)
            + static_cast<std::size_t>(x0),
        static_cast<std::size_t>(y0) * static_cast<std::size_t>(source.width)
            + static_cast<std::size_t>(x1),
        static_cast<std::size_t>(y1) * static_cast<std::size_t>(source.width)
            + static_cast<std::size_t>(x0),
        static_cast<std::size_t>(y1) * static_cast<std::size_t>(source.width)
            + static_cast<std::size_t>(x1)};
    const std::array<double, 4U> weights = {
        (1.0 - x_fraction) * (1.0 - y_fraction),
        x_fraction * (1.0 - y_fraction),
        (1.0 - x_fraction) * y_fraction,
        x_fraction * y_fraction};
    Color result;
    for (std::size_t index = 0U; index < pixels.size(); ++index) {
        const Color sample = load_color(source, pixels[index]);
        result.r += sample.r * weights[index];
        result.g += sample.g * weights[index];
        result.b += sample.b * weights[index];
        result.a += sample.a * weights[index];
    }
    result.a = clamp_value(result.a, 0.0, 1.0);
    return result;
}

ObjVec2 box_uv(ObjVec3 point, ObjVec3 normal) {
    const double absolute_x = std::fabs(normal.x);
    const double absolute_y = std::fabs(normal.y);
    const double absolute_z = std::fabs(normal.z);
    if (absolute_x >= absolute_y && absolute_x >= absolute_z) {
        return {normal.x > 0.0 ? (1.0 - point.z) * 0.5
                              : (point.z + 1.0) * 0.5,
                (point.y + 1.0) * 0.5};
    }
    if (absolute_y >= absolute_x && absolute_y >= absolute_z) {
        return {(point.x + 1.0) * 0.5,
                normal.y > 0.0 ? (point.z + 1.0) * 0.5
                               : (1.0 - point.z) * 0.5};
    }
    return {normal.z > 0.0 ? (point.x + 1.0) * 0.5
                           : (1.0 - point.x) * 0.5,
            (point.y + 1.0) * 0.5};
}

double edge(ScreenPoint first, ScreenPoint second, ScreenPoint point) {
    return (second.x - first.x) * (point.y - first.y)
           - (second.y - first.y) * (point.x - first.x);
}

bool top_left(ScreenPoint first, ScreenPoint second) {
    return second.y > first.y
           || (second.y == first.y && second.x < first.x);
}

bool edge_inside(double value, bool owns_edge) {
    return value > 0.0 || (value == 0.0 && owns_edge);
}

template <typename FragmentCallback>
void rasterize_mesh(const ObjMesh& mesh,
                    const std::vector<ProjectedVertex>& projected,
                    const std::vector<ObjVec3>& world_normals,
                    const Image& source,
                    int width,
                    int height,
                    const SurfaceConfig& surface,
                    double lighting,
                    const std::atomic_bool* cancel,
                    FragmentCallback&& fragment) {
    std::size_t triangle_index = 0U;
    for (const ObjTriangle& triangle : mesh.triangles) {
        if ((triangle_index++ & 63U) == 0U) {
            throw_if_cancelled(cancel);
        }
        std::array<RasterVertex, 3U> vertices{};
        bool valid = true;
        bool authored_uv = true;
        bool authored_normals = true;
        for (std::size_t corner = 0U; corner < vertices.size(); ++corner) {
            const ObjCorner& source_corner = triangle.corners[corner];
            if (source_corner.position >= projected.size()
                || !projected[source_corner.position].valid) {
                valid = false;
                break;
            }
            vertices[corner].projected = &projected[source_corner.position];
            authored_uv = authored_uv && source_corner.texcoord != ObjCorner::missing
                          && source_corner.texcoord < mesh.texcoords.size();
            authored_normals = authored_normals
                               && source_corner.normal != ObjCorner::missing
                               && source_corner.normal < world_normals.size();
        }
        if (!valid) {
            continue;
        }

        const ObjVec3 object_face_normal = normalize(cross(
            subtract(vertices[1].projected->object, vertices[0].projected->object),
            subtract(vertices[2].projected->object, vertices[0].projected->object)));
        ObjVec3 world_face_normal = normalize(cross(
            subtract(vertices[1].projected->world, vertices[0].projected->world),
            subtract(vertices[2].projected->world, vertices[0].projected->world)));
        if (dot(object_face_normal, object_face_normal) <= 1.0e-20
            || dot(world_face_normal, world_face_normal) <= 1.0e-20) {
            continue;
        }
        for (std::size_t corner = 0U; corner < vertices.size(); ++corner) {
            const ObjCorner& source_corner = triangle.corners[corner];
            vertices[corner].uv = authored_uv
                ? mesh.texcoords[source_corner.texcoord]
                : box_uv(vertices[corner].projected->object, object_face_normal);
            vertices[corner].normal = authored_normals
                ? world_normals[source_corner.normal] : world_face_normal;
        }

        double area = edge(vertices[0].projected->screen,
                           vertices[1].projected->screen,
                           vertices[2].projected->screen);
        if (!std::isfinite(area) || std::fabs(area) <= 1.0e-12) {
            continue;
        }
        // This is orientation normalization, not culling: both windings reach
        // the rasterizer and are shaded face-forward below.
        if (area < 0.0) {
            std::swap(vertices[1], vertices[2]);
            area = -area;
        }

        const double minimum_x = std::min({vertices[0].projected->screen.x,
                                           vertices[1].projected->screen.x,
                                           vertices[2].projected->screen.x});
        const double maximum_x = std::max({vertices[0].projected->screen.x,
                                           vertices[1].projected->screen.x,
                                           vertices[2].projected->screen.x});
        const double minimum_y = std::min({vertices[0].projected->screen.y,
                                           vertices[1].projected->screen.y,
                                           vertices[2].projected->screen.y});
        const double maximum_y = std::max({vertices[0].projected->screen.y,
                                           vertices[1].projected->screen.y,
                                           vertices[2].projected->screen.y});
        const int first_x = std::max(0, static_cast<int>(std::ceil(minimum_x - 0.5)));
        const int last_x = std::min(width - 1,
                                    static_cast<int>(std::floor(maximum_x - 0.5)));
        const int first_y = std::max(0, static_cast<int>(std::ceil(minimum_y - 0.5)));
        const int last_y = std::min(height - 1,
                                    static_cast<int>(std::floor(maximum_y - 0.5)));
        if (first_x > last_x || first_y > last_y) {
            continue;
        }

        const bool owns_0 = top_left(vertices[1].projected->screen,
                                     vertices[2].projected->screen);
        const bool owns_1 = top_left(vertices[2].projected->screen,
                                     vertices[0].projected->screen);
        const bool owns_2 = top_left(vertices[0].projected->screen,
                                     vertices[1].projected->screen);
        for (int y = first_y; y <= last_y; ++y) {
            throw_if_cancelled(cancel);
            for (int x = first_x; x <= last_x; ++x) {
                const ScreenPoint sample{static_cast<double>(x) + 0.5,
                                         static_cast<double>(y) + 0.5};
                const double edge_0 = edge(vertices[1].projected->screen,
                                           vertices[2].projected->screen, sample);
                const double edge_1 = edge(vertices[2].projected->screen,
                                           vertices[0].projected->screen, sample);
                const double edge_2 = edge(vertices[0].projected->screen,
                                           vertices[1].projected->screen, sample);
                if (!edge_inside(edge_0, owns_0) || !edge_inside(edge_1, owns_1)
                    || !edge_inside(edge_2, owns_2)) {
                    continue;
                }
                const std::array<double, 3U> barycentric = {
                    edge_0 / area, edge_1 / area, edge_2 / area};
                double denominator = 0.0;
                for (std::size_t corner = 0U; corner < vertices.size(); ++corner) {
                    denominator += barycentric[corner]
                                   * vertices[corner].projected->inverse_depth;
                }
                if (!std::isfinite(denominator) || denominator <= 0.0) {
                    continue;
                }
                double depth = 1.0 / denominator;
                if (surface.projection == SurfaceProjection::Orthographic) {
                    depth = 0.0;
                    for (std::size_t corner = 0U;
                         corner < vertices.size(); ++corner) {
                        depth += barycentric[corner]
                                 * vertices[corner].projected->camera_depth;
                    }
                }
                const std::size_t pixel = static_cast<std::size_t>(y)
                                          * static_cast<std::size_t>(width)
                                          + static_cast<std::size_t>(x);
                if (!fragment.accepts(pixel, depth)) {
                    continue;
                }

                ObjVec2 uv{};
                ObjVec3 normal{};
                ObjVec3 world{};
                for (std::size_t corner = 0U; corner < vertices.size(); ++corner) {
                    const double weight = barycentric[corner]
                                          * vertices[corner].projected->inverse_depth
                                          / denominator;
                    uv.x += vertices[corner].uv.x * weight;
                    uv.y += vertices[corner].uv.y * weight;
                    normal = add(normal, multiply(vertices[corner].normal, weight));
                    world = add(world,
                                multiply(vertices[corner].projected->world, weight));
                }
                normal = normalize(normal);
                const ObjVec3 toward_camera =
                    surface.projection == SurfaceProjection::Perspective
                        ? ObjVec3{-world.x, -world.y,
                                  surface.camera_distance - world.z}
                        : ObjVec3{0.0, 0.0, 1.0};
                if (dot(normal, toward_camera) < 0.0) {
                    normal = multiply(normal, -1.0);
                }
                Color color = sample_source(source, uv, authored_uv);
                color = shade(color, normal, surface, lighting);
                fragment.store(pixel, depth, color);
            }
        }
    }
}

bool validate_source(const Image& source, std::size_t& pixel_count,
                     std::string* error) {
    if (source.width <= 0 || source.height <= 0) {
        return fail(error, "OBJ surface source image has invalid dimensions.");
    }
    std::size_t component_count = 0U;
    if (!checked_multiply(static_cast<std::size_t>(source.width),
                          static_cast<std::size_t>(source.height), pixel_count)
        || !checked_multiply(pixel_count, 4U, component_count)
        || source.pixels.size() != component_count) {
        return fail(error, "OBJ surface source image metadata is inconsistent.");
    }
    return true;
}

bool source_is_opaque(const Image& source, std::size_t pixel_count,
                      const std::atomic_bool* cancel) {
    for (std::size_t pixel = 0U; pixel < pixel_count; ++pixel) {
        if ((pixel & 4095U) == 0U) throw_if_cancelled(cancel);
        const float alpha = source.pixels[pixel * 4U + 3U];
        if (!std::isfinite(alpha) || alpha < kOpaqueThreshold) {
            return false;
        }
    }
    return true;
}

ObjVec3 surface_object_position(const ObjMesh& mesh, ObjVec3 position,
                                const SurfaceConfig& surface) {
    if (surface.mapping == SurfaceMapping::CustomObj
        && !surface.normalize_obj) {
        return position;
    }
    return multiply(subtract(position, mesh.normalization_center),
                    mesh.normalization_scale);
}

std::vector<ProjectedVertex> project_positions(
    const ObjMesh& mesh, int width, int height, const SurfaceConfig& surface,
    const SurfaceAngles& angles, const std::atomic_bool* cancel) {
    std::vector<ProjectedVertex> result(mesh.positions.size());
    double half_x = 0.0;
    double half_y = 0.0;
    for (const ObjVec3 position : mesh.positions) {
        const ObjVec3 object = surface_object_position(mesh, position, surface);
        half_x = std::max(half_x, std::fabs(object.x));
        half_y = std::max(half_y, std::fabs(object.y));
    }
    half_x = std::max(half_x, 1.0e-9);
    half_y = std::max(half_y, 1.0e-9);
    const double width_span = static_cast<double>(std::max(1, width - 1));
    const double height_span = static_cast<double>(std::max(1, height - 1));
    const double size = surface.size_percent / 100.0;
    const double contain = std::min(width_span / (2.0 * half_x),
                                    height_span / (2.0 * half_y));
    const double cover = std::max(width_span / (2.0 * half_x),
                                  height_span / (2.0 * half_y));
    double screen_scale_x = contain * size;
    double screen_scale_y = contain * size;
    switch (surface.sizing) {
        case SurfaceSizing::Contain:
            break;
        case SurfaceSizing::Cover:
            screen_scale_x = cover * size;
            screen_scale_y = cover * size;
            break;
        case SurfaceSizing::Stretch:
            screen_scale_x = width_span / (2.0 * half_x) * size;
            screen_scale_y = height_span / (2.0 * half_y) * size;
            break;
        case SurfaceSizing::ShortSide:
            screen_scale_x = 0.5 * static_cast<double>(
                std::min(width, height)) * size;
            screen_scale_y = screen_scale_x;
            break;
    }
    const double center_x = 0.5 * width_span
        + surface.position_x_percent * width_span / 100.0;
    const double center_y = 0.5 * height_span
        - surface.position_y_percent * height_span / 100.0;
    for (std::size_t index = 0U; index < mesh.positions.size(); ++index) {
        if ((index & 4095U) == 0U) throw_if_cancelled(cancel);
        ProjectedVertex& projected = result[index];
        projected.object = surface_object_position(
            mesh, mesh.positions[index], surface);
        const ObjVec3 scaled = {
            projected.object.x * surface.scale_x,
            projected.object.y * surface.scale_y,
            projected.object.z * surface.scale_z};
        projected.world = rotate_surface(scaled, angles,
                                         surface.rotation_order);
        projected.world.z += surface.position_z;
        projected.camera_depth = surface.camera_distance - projected.world.z;
        if (!std::isfinite(projected.camera_depth)
            || projected.camera_depth <= kMinimumCameraDepth) {
            continue;
        }
        if (surface.projection == SurfaceProjection::Perspective) {
            projected.inverse_depth = 1.0 / projected.camera_depth;
            projected.screen = {
                center_x + projected.world.x * surface.focal_length
                               / projected.camera_depth * screen_scale_x,
                center_y - projected.world.y * surface.focal_length
                               / projected.camera_depth * screen_scale_y};
        } else {
            projected.inverse_depth = 1.0;
            projected.screen = {
                center_x + projected.world.x * screen_scale_x,
                center_y - projected.world.y * screen_scale_y};
        }
        projected.valid = std::isfinite(projected.screen.x)
                          && std::isfinite(projected.screen.y);
    }
    return result;
}

std::vector<ObjVec3> transform_normals(const ObjMesh& mesh,
                                       const SurfaceConfig& surface,
                                       const SurfaceAngles& angles,
                                       const std::atomic_bool* cancel) {
    std::vector<ObjVec3> result;
    result.reserve(mesh.normals.size());
    std::size_t index = 0U;
    for (const ObjVec3 normal : mesh.normals) {
        if ((index++ & 4095U) == 0U) throw_if_cancelled(cancel);
        const ObjVec3 inverse_scaled = {
            normal.x / surface.scale_x,
            normal.y / surface.scale_y,
            normal.z / surface.scale_z};
        result.push_back(normalize(rotate_surface(
            inverse_scaled, angles, surface.rotation_order)));
    }
    return result;
}

void blend_with_planar(const Image& source, Image& mapped,
                       const std::vector<unsigned char>& coverage,
                       std::size_t pixel_count, const SurfaceConfig& surface,
                       double curvature,
                       const std::atomic_bool* cancel) {
    for (std::size_t pixel = 0U; pixel < pixel_count; ++pixel) {
        if ((pixel & 4095U) == 0U) throw_if_cancelled(cancel);
        const Color planar = load_color(source, pixel);
        if (coverage[pixel] == 0U
            && surface.outside != SurfaceOutside::Transparent) {
            store_color(mapped, pixel, planar);
        } else {
            store_color(mapped, pixel,
                        mix_color(planar, load_color(mapped, pixel), curvature));
        }
    }
}

struct OpaqueFragments {
    std::vector<float>& depth;
    Image& mapped;
    std::vector<unsigned char>& coverage;

    bool accepts(std::size_t pixel, double candidate_depth) const {
        return candidate_depth < depth[pixel];
    }

    void store(std::size_t pixel, double candidate_depth, Color color) {
        depth[pixel] = static_cast<float>(candidate_depth);
        store_color(mapped, pixel, color);
        coverage[pixel] = 1U;
    }
};

struct LayerFragments {
    const std::vector<float>& previous_depth;
    std::vector<float>& next_depth;
    Image& layer;
    std::vector<unsigned char>& coverage;

    bool accepts(std::size_t pixel, double candidate_depth) const {
        const double previous = previous_depth[pixel];
        if (previous == std::numeric_limits<float>::infinity()) {
            return false;
        }
        const double epsilon = !std::isfinite(previous) || previous < 0.0
            ? 0.0 : std::max(1.0e-6, std::fabs(previous) * kDepthRelativeEpsilon);
        return candidate_depth > previous + epsilon
               && candidate_depth < next_depth[pixel];
    }

    void store(std::size_t pixel, double candidate_depth, Color color) {
        next_depth[pixel] = static_cast<float>(candidate_depth);
        store_color(layer, pixel, color);
        coverage[pixel] = 1U;
    }
};

} // namespace

bool apply_mesh_surface_mapping(const Image& source,
                                Image& destination,
                                const ObjMesh& mesh,
                                const SurfaceConfig& surface,
                                double loop_phase,
                                std::string* error,
                                const std::atomic_bool* cancel) {
    clear_error(error);
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
        return fail(error,
                    "Mesh surface rendering was cancelled; destination was unchanged.");
    }
    std::size_t pixel_count = 0U;
    if (!validate_source(source, pixel_count, error)) {
        return false;
    }
    if (!std::isfinite(surface.rotation_x_degrees)
        || !std::isfinite(surface.rotation_y_degrees)
        || !std::isfinite(surface.rotation_z_degrees)
        || !std::isfinite(surface.curvature)
        || !std::isfinite(surface.lighting) || !std::isfinite(loop_phase)) {
        return fail(error, "Mesh surface parameters must be finite.");
    }
    const double curvature = clamp_value(surface.curvature, 0.0, 1.0);
    if (curvature == 0.0) {
        try {
            Image unchanged = source;
            destination = std::move(unchanged);
            return true;
        } catch (const std::bad_alloc&) {
            return fail(error, "Not enough memory to copy the neutral mesh surface.");
        }
    }

    try {
        throw_if_cancelled(cancel);
        double wrapped_loop_phase = std::fmod(loop_phase, kTau);
        if (wrapped_loop_phase < 0.0) {
            wrapped_loop_phase += kTau;
        }
        const SurfaceAngles angles = surface_angles(surface,
                                                    wrapped_loop_phase);
        const std::vector<ProjectedVertex> projected =
            project_positions(mesh, source.width, source.height, surface,
                              angles, cancel);
        const std::vector<ObjVec3> world_normals =
            transform_normals(mesh, surface, angles, cancel);

        Image mapped;
        mapped.width = source.width;
        mapped.height = source.height;
        mapped.pixels.assign(pixel_count * 4U, 0.0F);
        std::vector<unsigned char> coverage(pixel_count, 0U);
        const double mapped_lighting = clamp_value(
            surface.lighting, 0.0,
            maximum_render_parameter_magnitude()) * curvature;

        if (source_is_opaque(source, pixel_count, cancel)
            || !surface.composite_backfaces) {
            std::vector<float> depth(pixel_count,
                                     std::numeric_limits<float>::infinity());
            OpaqueFragments fragments{depth, mapped, coverage};
            rasterize_mesh(mesh, projected, world_normals, source,
                           source.width, source.height, surface,
                           mapped_lighting, cancel, fragments);
        } else {
            std::vector<float> previous_depth(
                pixel_count, -std::numeric_limits<float>::infinity());
            std::vector<float> next_depth(
                pixel_count, std::numeric_limits<float>::infinity());
            Image layer;
            layer.width = source.width;
            layer.height = source.height;
            layer.pixels.resize(pixel_count * 4U);

            bool first_layer = true;
            for (;;) {
                throw_if_cancelled(cancel);
                LayerFragments fragments{previous_depth, next_depth, layer,
                                         coverage};
                rasterize_mesh(mesh, projected, world_normals, source,
                               source.width, source.height, surface,
                               mapped_lighting, cancel, fragments);

                std::size_t hit_count = 0U;
                std::size_t transparent_count = 0U;
                for (std::size_t pixel = 0U; pixel < pixel_count; ++pixel) {
                    if ((pixel & 4095U) == 0U) throw_if_cancelled(cancel);
                    if (!std::isfinite(next_depth[pixel])) {
                        previous_depth[pixel] = std::numeric_limits<float>::infinity();
                        continue;
                    }
                    ++hit_count;
                    const Color fragment = load_color(layer, pixel);
                    const Color accumulated = first_layer
                        ? fragment : composite_over(load_color(mapped, pixel), fragment);
                    store_color(mapped, pixel, accumulated);
                    if (accumulated.a < kOpaqueThreshold) {
                        previous_depth[pixel] = next_depth[pixel];
                        ++transparent_count;
                    } else {
                        previous_depth[pixel] = std::numeric_limits<float>::infinity();
                    }
                    next_depth[pixel] = std::numeric_limits<float>::infinity();
                }
                if (hit_count == 0U || transparent_count == 0U) {
                    break;
                }
                first_layer = false;
            }
        }

        blend_with_planar(source, mapped, coverage, pixel_count, surface,
                          curvature, cancel);
        throw_if_cancelled(cancel);
        destination.width = mapped.width;
        destination.height = mapped.height;
        destination.pixels.swap(mapped.pixels);
        return true;
    } catch (const ObjSurfaceCancelled&) {
        return fail(error,
                    "Mesh surface rendering was cancelled; destination was unchanged.");
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to render the mesh surface; destination was unchanged.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Could not render mesh surface; destination was unchanged: ")
                           + exception.what());
    }
}

bool apply_obj_surface_mapping(const Image& source,
                               Image& destination,
                               const std::string& utf8_obj_path,
                               const SurfaceConfig& surface,
                               double loop_phase,
                               std::string* error,
                               const std::atomic_bool* cancel) {
    clear_error(error);
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
        return fail(error,
                    "OBJ surface rendering was cancelled; destination was unchanged.");
    }
    std::size_t pixel_count = 0U;
    if (!validate_source(source, pixel_count, error)) {
        return false;
    }
    if (!std::isfinite(surface.rotation_x_degrees)
        || !std::isfinite(surface.rotation_y_degrees)
        || !std::isfinite(surface.rotation_z_degrees)
        || !std::isfinite(surface.curvature)
        || !std::isfinite(surface.lighting) || !std::isfinite(loop_phase)) {
        return fail(error, "OBJ surface parameters must be finite.");
    }
    if (clamp_value(surface.curvature, 0.0, 1.0) == 0.0) {
        try {
            Image unchanged = source;
            destination = std::move(unchanged);
            return true;
        } catch (const std::bad_alloc&) {
            return fail(error, "Not enough memory to copy the neutral OBJ surface.");
        }
    }
    try {
        std::shared_ptr<const ObjMesh> mesh;
        if (!load_obj_mesh_cached(utf8_obj_path, mesh, error)) {
            return false;
        }
        return apply_mesh_surface_mapping(
            source, destination, *mesh, surface, loop_phase, error, cancel);
    } catch (const std::bad_alloc&) {
        return fail(error,
                    "Not enough memory to load the OBJ surface; destination was unchanged.");
    } catch (const std::exception& exception) {
        return fail(error,
                    std::string("Could not load OBJ surface; destination was unchanged: ")
                        + exception.what());
    }
}

} // namespace detail
} // namespace pvt
