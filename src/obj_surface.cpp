#include "obj_surface.h"
#include "raster_occlusion.h"
#include "packed_mask.h"

#include "environment_map.h"
#include "obj_mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pvt {
namespace detail {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kTau = 2.0 * kPi;
constexpr double kMinimumCameraDepth = 1.0e-6;
constexpr double kOpaqueThreshold = 1.0;
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
    ObjVec3 world{};
    ScreenPoint screen{};
    double camera_depth = 0.0;
    double inverse_depth = 0.0;
    bool valid = false;
};
static_assert(sizeof(ProjectedVertex) <= 64U,
              "Projected vertices should fit in one cache line.");

struct RasterVertex {
    const ProjectedVertex* projected = nullptr;
    ObjVec2 uv{};
    ObjVec3 normal{};
};

struct ProjectionContext {
    double screen_scale_x = 1.0;
    double screen_scale_y = 1.0;
    double center_x = 0.0;
    double center_y = 0.0;
};

struct FragmentTransform {
    ObjVec3 pivot{};
    ObjVec3 direction{};
    ObjVec3 axis{};
    double amount = 0.0;
    double distance = 0.0;
    double angle = 0.0;
    double scale = 1.0;
};

struct MeshConstructionPlan {
    std::vector<std::uint32_t> triangle_fragments;
    std::vector<FragmentTransform> fragments;

    bool active() const noexcept {
        return !fragments.empty()
               && triangle_fragments.size() > 0U;
    }
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

ObjVec3 rotate_axis(ObjVec3 value, ObjVec3 axis, double angle) {
    if (angle == 0.0) return value;
    axis = normalize(axis);
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    return add(add(multiply(value, cosine),
                   multiply(cross(axis, value), sine)),
               multiply(axis, dot(axis, value) * (1.0 - cosine)));
}

double smoothstep(double value) {
    value = clamp_value(value, 0.0, 1.0);
    return value * value * (3.0 - 2.0 * value);
}

std::uint64_t stable_hash(std::uint64_t value) {
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27U)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

double hash_unit(std::uint64_t value) {
    // Retain 53 high bits so conversion is identical on all IEEE-754 double
    // implementations and never rounds up to 1.
    return static_cast<double>(stable_hash(value) >> 11U)
           * (1.0 / 9007199254740992.0);
}

ObjVec3 deterministic_unit_vector(std::uint64_t seed,
                                  std::size_t fragment,
                                  std::uint64_t salt) {
    const std::uint64_t identity = seed
        ^ stable_hash(static_cast<std::uint64_t>(fragment) + salt);
    const double z = 2.0 * hash_unit(identity) - 1.0;
    const double azimuth = kTau * hash_unit(identity ^ UINT64_C(0xd6e8feb86659fd93));
    const double radius = std::sqrt(std::max(0.0, 1.0 - z * z));
    return {radius * std::cos(azimuth), radius * std::sin(azimuth), z};
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
            double lighting,
            const PreparedEnvironmentMap* environment) {
    const ObjVec3 light = normalize({surface.light_direction_x,
                                     surface.light_direction_y,
                                     surface.light_direction_z});
    const double diffuse = std::max(0.0, dot(normalize(normal), light));
    const double lit = surface.light_ambient
                       + surface.light_diffuse * diffuse;
    if (environment != nullptr && *environment && environment->mix > 0.0) {
        const EnvironmentMapRgb sampled = sample_environment_map_diffuse(
            *environment, normal.x, normal.y, normal.z);
        const auto multiplier = [&](double environment_lit) {
            const double blended = lit
                + (environment_lit - lit) * environment->mix;
            return std::max(0.0, 1.0 + lighting * (blended - 1.0));
        };
        constexpr double maximum = static_cast<double>(
            (std::numeric_limits<float>::max)());
        const auto saturated = [&](double value, double amount) {
            return clamp_value(value * amount, -maximum, maximum);
        };
        color.r = saturated(color.r, multiplier(sampled.red));
        color.g = saturated(color.g, multiplier(sampled.green));
        color.b = saturated(color.b, multiplier(sampled.blue));
        return color;
    }
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

double canonical_edge(ScreenPoint first, ScreenPoint second, ScreenPoint point) {
    // At extreme projected scales, evaluating the same shared edge from
    // opposite endpoints can round both sides outward, leaving a crack.
    // Use one arithmetic orientation and negate it for the adjacent face.
    if (first.x > second.x || (first.x == second.x && first.y > second.y)) {
        return -edge(second, first, point);
    }
    return edge(first, second, point);
}

bool top_left(ScreenPoint first, ScreenPoint second) {
    return second.y > first.y
           || (second.y == first.y && second.x < first.x);
}

bool edge_inside(double value, bool owns_edge) {
    return value > 0.0 || (value == 0.0 && owns_edge);
}

ObjVec3 surface_object_position(const ObjMesh& mesh, ObjVec3 position,
                                const SurfaceConfig& surface);
ProjectedVertex project_object_position(
    ObjVec3 object,
    const ProjectionContext& context,
    const SurfaceConfig& surface,
    const SurfaceAngles& angles);
ObjVec3 transform_normal(ObjVec3 normal,
                         const SurfaceConfig& surface,
                         const SurfaceAngles& angles,
                         const FragmentTransform* fragment);

void project_camera_vertex(ProjectedVertex& projected,
                           const ProjectionContext& context,
                           const SurfaceConfig& surface);

template <typename FragmentCallback>
void rasterize_triangle(std::array<RasterVertex, 3U> vertices,
                        bool authored_uv, const Image& source,
                        int width, int height, const SurfaceConfig& surface,
                        double lighting, const PreparedEnvironmentMap* environment,
                        const std::atomic_bool* cancel,
                        FragmentCallback& fragment) {
    double area = edge(vertices[0].projected->screen,
                       vertices[1].projected->screen,
                       vertices[2].projected->screen);
    if (!std::isfinite(area) || std::fabs(area) <= 1.0e-12) {
        return;
    }
    // This is orientation normalization, not culling: both windings reach
    // the rasterizer and are shaded face-forward below.
    if (area < 0.0) {
        std::swap(vertices[1], vertices[2]);
        // Negating the old determinant need not equal evaluating the new
        // order when multiply/add contraction is enabled (notably GCC ARM64).
        // Use the same ordered vertices for both area and sample edges.
        area = edge(vertices[0].projected->screen,
                    vertices[1].projected->screen,
                    vertices[2].projected->screen);
        if (!std::isfinite(area) || area <= 1.0e-12) return;
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
    // Clip in floating point before conversion. Valid finite projections
    // can be far outside int range near the camera or at extreme scales.
    if (maximum_x < 0.5 || maximum_y < 0.5
        || minimum_x > static_cast<double>(width) - 0.5
        || minimum_y > static_cast<double>(height) - 0.5) {
        return;
    }
    const int first_x = static_cast<int>(std::ceil(
        std::max(0.0, minimum_x - 0.5)));
    const int last_x = static_cast<int>(std::floor(
        std::min(static_cast<double>(width - 1), maximum_x - 0.5)));
    const int first_y = static_cast<int>(std::ceil(
        std::max(0.0, minimum_y - 0.5)));
    const int last_y = static_cast<int>(std::floor(
        std::min(static_cast<double>(height - 1), maximum_y - 0.5)));
    if (first_x > last_x || first_y > last_y) {
        return;
    }

    const bool owns_0 = top_left(vertices[1].projected->screen,
                                 vertices[2].projected->screen);
    const bool owns_1 = top_left(vertices[2].projected->screen,
                                 vertices[0].projected->screen);
    const bool owns_2 = top_left(vertices[0].projected->screen,
                                 vertices[1].projected->screen);
    const double integer_limit = std::numeric_limits<int>::max();
    const auto extreme_vertex = [&](ScreenPoint point) {
        return std::fabs(point.x) > integer_limit
            || std::fabs(point.y) > integer_limit;
    };
    const bool extreme_0 = extreme_vertex(vertices[0].projected->screen);
    const bool extreme_1 = extreme_vertex(vertices[1].projected->screen);
    const bool extreme_2 = extreme_vertex(vertices[2].projected->screen);
    if (fragment.can_cull() &&
        static_cast<double>(last_x - first_x + 1) * (last_y - first_y + 1) >= 128.0) {
        // Bound the exact raster expression over the entire pixel rectangle.
        // Coverage tests may further restrict it but can never expand it.
        RasterInterval denominator{0.0, 0.0};
        RasterInterval linear_depth{0.0, 0.0};
        const RasterInterval px{first_x + 0.5, last_x + 0.5}, py{first_y + 0.5, last_y + 0.5};
        for (std::size_t i = 0U; i < 3U; ++i) {
            const auto a = vertices[(i + 1U) % 3U].projected->screen;
            const auto b = vertices[(i + 2U) % 3U].projected->screen;
            auto edges =
                (RasterInterval{b.x, b.x} - RasterInterval{a.x, a.x}) *
                    (py - RasterInterval{a.y, a.y}) -
                (RasterInterval{b.y, b.y} - RasterInterval{a.y, a.y}) * (px - RasterInterval{a.x, a.x});
            // canonical_edge may use the reversed expression; include both
            // rounded evaluations in the bound for extreme coordinates.
            const auto reverse =
                (RasterInterval{a.x, a.x} - RasterInterval{b.x, b.x}) *
                    (py - RasterInterval{b.y, b.y}) -
                (RasterInterval{a.y, a.y} - RasterInterval{b.y, b.y}) * (px - RasterInterval{b.x, b.x});
            edges.low = std::min(edges.low, -reverse.high);
            edges.high = std::max(edges.high, -reverse.low);
            edges.low = std::max(0.0, edges.low);
            const auto barycentric = edges.divide_positive(area);
            const double inverse = vertices[i].projected->inverse_depth;
            const double depth = vertices[i].projected->camera_depth;
            denominator = denominator + barycentric * RasterInterval{inverse, inverse};
            linear_depth = linear_depth + barycentric * RasterInterval{depth, depth};
        }
        const double minimum_depth = surface.projection == SurfaceProjection::Orthographic
                                         ? linear_depth.low
                                         : RasterInterval::down(1.0 / denominator.high);
        if (fragment.occludes(first_x, last_x, first_y, last_y, minimum_depth)) return;
    }

    const auto sample_edge = [&](ScreenPoint first, ScreenPoint second,
                                 ScreenPoint point, bool extreme) {
        return extreme ? canonical_edge(first, second, point)
                       : edge(first, second, point);
    };
    for (int y = first_y; y <= last_y; ++y) {
        throw_if_cancelled(cancel);
        for (int x = first_x; x <= last_x; ++x) {
            const ScreenPoint sample{static_cast<double>(x) + 0.5,
                                     static_cast<double>(y) + 0.5};
            const double edge_0 = sample_edge(vertices[1].projected->screen,
                                       vertices[2].projected->screen, sample,
                                       extreme_1 || extreme_2);
            const double edge_1 = sample_edge(vertices[2].projected->screen,
                                       vertices[0].projected->screen, sample,
                                       extreme_2 || extreme_0);
            const double edge_2 = sample_edge(vertices[0].projected->screen,
                                       vertices[1].projected->screen, sample,
                                       extreme_0 || extreme_1);
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
            color = shade(color, normal, surface, lighting, environment);
            fragment.store(pixel, depth, color);
        }
    }
}

template <typename FragmentCallback>
void rasterize_mesh(const ObjMesh& mesh,
                    const std::vector<ProjectedVertex>& projected,
                    const std::vector<ObjVec3>& world_normals,
                    const ProjectionContext& projection_context,
                    const MeshConstructionPlan& construction,
                    const SurfaceAngles& angles,
                    const Image& source,
                    int width,
                    int height,
                    const SurfaceConfig& surface,
                    double lighting,
                    const PreparedEnvironmentMap* environment,
                    const std::atomic_bool* cancel,
                    FragmentCallback&& fragment) {
    std::size_t triangle_index = 0U;
    for (const ObjTriangle& triangle : mesh.triangles) {
        const std::size_t current_triangle = triangle_index++;
        if ((current_triangle & 63U) == 0U) {
            throw_if_cancelled(cancel);
        }
        const FragmentTransform* construction_transform = nullptr;
        if (construction.active()
            && current_triangle < construction.triangle_fragments.size()) {
            const std::size_t fragment_index =
                construction.triangle_fragments[current_triangle];
            if (fragment_index < construction.fragments.size()
                && construction.fragments[fragment_index].amount != 0.0) {
                construction_transform =
                    &construction.fragments[fragment_index];
            }
        }
        std::array<RasterVertex, 3U> vertices{};
        std::array<ProjectedVertex, 3U> animated_projected{};
        std::array<ObjVec3, 3U> texture_objects{};
        bool valid = true;
        bool authored_uv = true;
        bool authored_normals = true;
        for (std::size_t corner = 0U; corner < vertices.size(); ++corner) {
            const ObjCorner& source_corner = triangle.corners[corner];
            if (source_corner.position >= projected.size()) {
                valid = false;
                break;
            }
            texture_objects[corner] = surface_object_position(
                mesh, mesh.positions[source_corner.position], surface);
            if (construction_transform != nullptr) {
                const ObjVec3 local = multiply(
                    subtract(texture_objects[corner],
                             construction_transform->pivot),
                    construction_transform->scale);
                const ObjVec3 animated_object = add(
                    add(construction_transform->pivot,
                        rotate_axis(local, construction_transform->axis,
                                    construction_transform->angle)),
                    multiply(construction_transform->direction,
                             construction_transform->distance));
                animated_projected[corner] = project_object_position(
                    animated_object, projection_context, surface, angles);
                vertices[corner].projected = &animated_projected[corner];
            } else {
                vertices[corner].projected =
                    &projected[source_corner.position];
            }
            if (!std::isfinite(vertices[corner].projected->camera_depth)
                || !std::isfinite(vertices[corner].projected->world.x)
                || !std::isfinite(vertices[corner].projected->world.y)) {
                valid = false;
                break;
            }
            authored_uv = authored_uv && source_corner.texcoord != ObjCorner::missing
                          && source_corner.texcoord < mesh.texcoords.size();
            authored_normals = authored_normals
                               && source_corner.normal != ObjCorner::missing
                               && source_corner.normal < world_normals.size();
        }
        if (!valid) {
            continue;
        }

        // View rejection is independent of winding, opacity, and topology.
        // Do it before face normals, generated UVs, and lighting preparation.
        const bool fully_projected = std::all_of(vertices.begin(), vertices.end(),
            [](const RasterVertex& vertex) { return vertex.projected->valid; });
        if (std::all_of(vertices.begin(), vertices.end(), [](const RasterVertex& vertex) {
                return vertex.projected->camera_depth < kMinimumCameraDepth;
            })) continue;
        if (fully_projected) {
            const auto outside = [&](auto predicate) {
                return std::all_of(vertices.begin(), vertices.end(),
                    [&](const RasterVertex& vertex) { return predicate(vertex.projected->screen); });
            };
            if (outside([](ScreenPoint p) { return p.x < 0.5; })
                || outside([](ScreenPoint p) { return p.y < 0.5; })
                || outside([&](ScreenPoint p) { return p.x > width - 0.5; })
                || outside([&](ScreenPoint p) { return p.y > height - 0.5; })) continue;
        }

        const ObjVec3 object_face_normal = normalize(cross(
            subtract(texture_objects[1], texture_objects[0]),
            subtract(texture_objects[2], texture_objects[0])));
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
                : box_uv(texture_objects[corner], object_face_normal);
            if (authored_normals) {
                vertices[corner].normal = construction_transform == nullptr
                    ? world_normals[source_corner.normal]
                    : transform_normal(mesh.normals[source_corner.normal],
                                       surface, angles,
                                       construction_transform);
            } else {
                vertices[corner].normal = world_face_normal;
            }
        }

        const auto draw = [&](const std::array<RasterVertex, 3U>& triangle_vertices) {
            rasterize_triangle(triangle_vertices, authored_uv, source,
                               width, height, surface, lighting, environment,
                               cancel, fragment);
        };
        if (fully_projected) {
            draw(vertices);
            continue;
        }

        // Clipped quads must use the same diagonal after reversing source
        // winding. Otherwise floating-point edge/interpolation rounding can
        // produce different coverage across compiler arithmetic modes.
        // Sort only the three original corners; any order still describes
        // the same triangle, and ordinary unclipped arithmetic is untouched.
        const auto position_less = [](const RasterVertex& a, const RasterVertex& b) {
            const ObjVec3 first = a.projected->world;
            const ObjVec3 second = b.projected->world;
            if (first.x != second.x) return first.x < second.x;
            if (first.y != second.y) return first.y < second.y;
            return first.z < second.z;
        };
        std::sort(vertices.begin(), vertices.end(), position_less);

        // Sutherland-Hodgman clipping against camera depth. A triangle becomes
        // at most a quad. Keep all storage on the stack, including interpolated
        // attributes; unclipped triangles retain their original arithmetic.
        std::array<ProjectedVertex, 4U> clipped_positions{};
        std::array<RasterVertex, 4U> clipped{};
        std::size_t count = 0U;
        for (std::size_t i = 0U; i < 3U; ++i) {
            const RasterVertex& first = vertices[i];
            const RasterVertex& second = vertices[(i + 1U) % 3U];
            const bool inside_first = first.projected->camera_depth >= kMinimumCameraDepth;
            const bool inside_second = second.projected->camera_depth >= kMinimumCameraDepth;
            if (inside_first) clipped[count++] = first;
            if (inside_first != inside_second) {
                // Always interpolate from the shallower endpoint. Shared edges
                // then produce identical intersections in either winding.
                const RasterVertex& near = inside_first ? second : first;
                const RasterVertex& far = inside_first ? first : second;
                const double t = (kMinimumCameraDepth - near.projected->camera_depth)
                    / (far.projected->camera_depth - near.projected->camera_depth);
                auto& position = clipped_positions[count];
                position.world = add(near.projected->world,
                    multiply(subtract(far.projected->world, near.projected->world), t));
                position.camera_depth = kMinimumCameraDepth;
                position.world.z = surface.camera_distance - kMinimumCameraDepth;
                project_camera_vertex(position, projection_context, surface);
                auto& vertex = clipped[count++];
                vertex.projected = &position;
                vertex.uv = {near.uv.x + (far.uv.x - near.uv.x) * t,
                             near.uv.y + (far.uv.y - near.uv.y) * t};
                vertex.normal = add(near.normal, multiply(subtract(far.normal, near.normal), t));
            }
        }
        for (std::size_t i = 1U; i + 1U < count; ++i) {
            if (clipped[0].projected->valid && clipped[i].projected->valid
                && clipped[i + 1U].projected->valid) {
                draw({clipped[0], clipped[i], clipped[i + 1U]});
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

ObjVec3 triangle_center(const ObjMesh& mesh, std::size_t triangle_index) {
    ObjVec3 center{};
    for (const ObjCorner& corner : mesh.triangles[triangle_index].corners) {
        if (corner.position >= mesh.positions.size()) {
            throw std::runtime_error(
                "Mesh construction encountered an unavailable position.");
        }
        center = add(center, mesh.positions[corner.position]);
    }
    return multiply(center, 1.0 / 3.0);
}

double coordinate(ObjVec3 point, std::size_t axis) {
    return axis == 0U ? point.x : (axis == 1U ? point.y : point.z);
}

std::vector<std::uint32_t> spatial_cluster_labels(
    const std::vector<ObjVec3>& points,
    std::size_t target,
    const std::atomic_bool* cancel) {
    std::vector<std::uint32_t> result(points.size(), 0U);
    if (points.empty() || target <= 1U) return result;
    target = std::min(target, points.size());
    std::vector<std::size_t> order(points.size());
    std::iota(order.begin(), order.end(), 0U);
    std::uint32_t next_label = 0U;
    const auto split = [&](const auto& self,
                           std::size_t begin,
                           std::size_t end,
                           std::size_t wanted) -> void {
        throw_if_cancelled(cancel);
        if (wanted <= 1U || end - begin <= 1U) {
            const std::uint32_t label = next_label++;
            for (std::size_t cursor = begin; cursor < end; ++cursor) {
                result[order[cursor]] = label;
            }
            return;
        }

        ObjVec3 minimum{
            std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::infinity()};
        ObjVec3 maximum{
            -std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity()};
        for (std::size_t cursor = begin; cursor < end; ++cursor) {
            const ObjVec3 point = points[order[cursor]];
            minimum.x = std::min(minimum.x, point.x);
            minimum.y = std::min(minimum.y, point.y);
            minimum.z = std::min(minimum.z, point.z);
            maximum.x = std::max(maximum.x, point.x);
            maximum.y = std::max(maximum.y, point.y);
            maximum.z = std::max(maximum.z, point.z);
        }
        const std::array<double, 3U> extents = {
            maximum.x - minimum.x,
            maximum.y - minimum.y,
            maximum.z - minimum.z};
        std::size_t axis = 0U;
        if (extents[1] > extents[axis]) axis = 1U;
        if (extents[2] > extents[axis]) axis = 2U;
        std::stable_sort(
            order.begin() + static_cast<std::ptrdiff_t>(begin),
            order.begin() + static_cast<std::ptrdiff_t>(end),
            [&](std::size_t left, std::size_t right) {
                const double left_value = coordinate(points[left], axis);
                const double right_value = coordinate(points[right], axis);
                return left_value < right_value
                       || (left_value == right_value && left < right);
            });

        const std::size_t left_wanted = wanted / 2U;
        const std::size_t right_wanted = wanted - left_wanted;
        const std::size_t count = end - begin;
        // Compute floor(count * left_wanted / wanted) without overflowing.
        // The remainder product is bounded by 65535 * 32768 because authored
        // fragment counts cannot exceed kMaximumMeshFragments.
        std::size_t left_count = count / wanted * left_wanted
            + (count % wanted) * left_wanted / wanted;
        left_count = std::max(left_wanted,
                              std::min(left_count, count - right_wanted));
        const std::size_t middle = begin + left_count;
        self(self, begin, middle, left_wanted);
        self(self, middle, end, right_wanted);
    };
    split(split, 0U, order.size(), target);
    return result;
}

std::vector<std::uint32_t> construction_triangle_fragments(
    const ObjMesh& mesh,
    const MeshConstructionConfig& config,
    const std::atomic_bool* cancel) {
    const std::size_t triangle_count = mesh.triangles.size();
    if (triangle_count == 0U) return {};
    const std::size_t authored_target = config.target_fragments > 0
        ? static_cast<std::size_t>(config.target_fragments) : 1U;
    const std::size_t target = std::min(
        {authored_target, kMaximumMeshFragments, triangle_count});
    const bool topology_available =
        mesh.triangle_components.size() == triangle_count
        && mesh.connected_component_count > 0U;
    bool use_components = config.fragmentation
                          == MeshFragmentation::ConnectedComponents;
    if (config.fragmentation == MeshFragmentation::Automatic) {
        use_components = topology_available
                         && mesh.connected_component_count > 1U;
    }
    if (!use_components || !topology_available) {
        std::vector<ObjVec3> centers;
        centers.reserve(triangle_count);
        for (std::size_t triangle = 0U; triangle < triangle_count; ++triangle) {
            if ((triangle & 4095U) == 0U) throw_if_cancelled(cancel);
            centers.push_back(triangle_center(mesh, triangle));
        }
        return spatial_cluster_labels(centers, target, cancel);
    }

    const std::size_t component_count = mesh.connected_component_count;
    std::vector<ObjVec3> component_centers(component_count);
    std::vector<std::size_t> component_triangles(component_count, 0U);
    for (std::size_t triangle = 0U; triangle < triangle_count; ++triangle) {
        if ((triangle & 4095U) == 0U) throw_if_cancelled(cancel);
        const std::size_t component = mesh.triangle_components[triangle];
        if (component >= component_count) {
            throw std::runtime_error(
                "Mesh construction encountered invalid component metadata.");
        }
        component_centers[component] = add(component_centers[component],
                                           triangle_center(mesh, triangle));
        ++component_triangles[component];
    }
    for (std::size_t component = 0U; component < component_count; ++component) {
        if (component_triangles[component] != 0U) {
            component_centers[component] = multiply(
                component_centers[component],
                1.0 / static_cast<double>(component_triangles[component]));
        }
    }
    std::vector<std::uint32_t> component_fragments;
    if (component_count <= target) {
        component_fragments.resize(component_count);
        for (std::size_t component = 0U; component < component_count;
             ++component) {
            component_fragments[component] =
                static_cast<std::uint32_t>(component);
        }
    } else {
        component_fragments = spatial_cluster_labels(component_centers, target,
                                                      cancel);
    }
    std::vector<std::uint32_t> result(triangle_count, 0U);
    for (std::size_t triangle = 0U; triangle < triangle_count; ++triangle) {
        result[triangle] = component_fragments[
            mesh.triangle_components[triangle]];
    }
    return result;
}

MeshConstructionPlan build_construction_plan(
    const ObjMesh& mesh,
    const SurfaceConfig& surface,
    double wrapped_loop_phase,
    const std::atomic_bool* cancel) {
    MeshConstructionPlan plan;
    const MeshConstructionConfig& config = surface.mesh_construction;
    if (config.mode == MeshConstructionMode::None || mesh.triangles.empty()) {
        return plan;
    }
    const double cycle_phase = std::fmod(
        wrapped_loop_phase / kTau
            * static_cast<double>(config.cycles_per_loop)
        + std::fmod(config.phase_degrees, 360.0) / 360.0,
        1.0);
    const double unit_phase = cycle_phase < 0.0
                                  ? cycle_phase + 1.0 : cycle_phase;
    const double tent = 1.0 - std::fabs(2.0 * unit_phase - 1.0);
    if ((config.mode == MeshConstructionMode::Explode
         || config.mode == MeshConstructionMode::Deconstruct)
        && tent == 0.0) {
        return plan;
    }
    if (config.mode == MeshConstructionMode::Reconstruct && tent == 1.0) {
        return plan;
    }

    plan.triangle_fragments = construction_triangle_fragments(mesh, config,
                                                               cancel);
    if (plan.triangle_fragments.empty()) return plan;
    const std::uint32_t maximum_fragment = *std::max_element(
        plan.triangle_fragments.begin(), plan.triangle_fragments.end());
    const std::size_t fragment_count =
        static_cast<std::size_t>(maximum_fragment) + 1U;
    plan.fragments.resize(fragment_count);

    std::vector<ObjVec3> weighted_centers(fragment_count);
    std::vector<double> weights(fragment_count, 0.0);
    for (std::size_t triangle = 0U; triangle < mesh.triangles.size();
         ++triangle) {
        if ((triangle & 4095U) == 0U) throw_if_cancelled(cancel);
        const std::size_t fragment = plan.triangle_fragments[triangle];
        std::array<ObjVec3, 3U> points{};
        for (std::size_t corner = 0U; corner < points.size(); ++corner) {
            const std::uint32_t position =
                mesh.triangles[triangle].corners[corner].position;
            if (position >= mesh.positions.size()) {
                throw std::runtime_error(
                    "Mesh construction encountered an unavailable position.");
            }
            points[corner] = surface_object_position(
                mesh, mesh.positions[position], surface);
        }
        const ObjVec3 center = multiply(
            add(add(points[0], points[1]), points[2]), 1.0 / 3.0);
        const ObjVec3 twice_area = cross(
            subtract(points[1], points[0]),
            subtract(points[2], points[0]));
        double weight = std::sqrt(std::max(0.0,
                                           dot(twice_area, twice_area)));
        if (!std::isfinite(weight) || weight <= 1.0e-20) weight = 1.0;
        weighted_centers[fragment] = add(
            weighted_centers[fragment], multiply(center, weight));
        weights[fragment] += weight;
    }

    std::vector<std::size_t> order(fragment_count);
    std::iota(order.begin(), order.end(), 0U);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t left,
                                                     std::size_t right) {
        const std::uint64_t left_key = stable_hash(
            config.seed ^ static_cast<std::uint64_t>(left)
            ^ UINT64_C(0x243f6a8885a308d3));
        const std::uint64_t right_key = stable_hash(
            config.seed ^ static_cast<std::uint64_t>(right)
            ^ UINT64_C(0x243f6a8885a308d3));
        return left_key < right_key || (left_key == right_key && left < right);
    });
    std::vector<std::size_t> ranks(fragment_count, 0U);
    for (std::size_t rank = 0U; rank < fragment_count; ++rank) {
        ranks[order[rank]] = rank;
    }

    const ObjVec3 mesh_center = surface_object_position(
        mesh, mesh.normalization_center, surface);
    const double distance_units =
        surface.mapping == SurfaceMapping::CustomObj && !surface.normalize_obj
        && mesh.normalization_scale > 0.0
            ? 1.0 / mesh.normalization_scale : 1.0;
    bool any_active = false;
    for (std::size_t fragment = 0U; fragment < fragment_count; ++fragment) {
        FragmentTransform& transform = plan.fragments[fragment];
        transform.pivot = weights[fragment] > 0.0
            ? multiply(weighted_centers[fragment], 1.0 / weights[fragment])
            : mesh_center;
        const ObjVec3 random_direction = deterministic_unit_vector(
            config.seed, fragment, UINT64_C(0x13198a2e03707344));
        const ObjVec3 radial = normalize(subtract(transform.pivot, mesh_center));
        transform.direction = dot(radial, radial) <= 1.0e-20
            ? random_direction
            : normalize(add(multiply(radial, 0.85),
                            multiply(random_direction, 0.15)));
        transform.axis = deterministic_unit_vector(
            config.seed, fragment, UINT64_C(0xa4093822299f31d0));

        double amount = 0.0;
        if (config.mode == MeshConstructionMode::Explode) {
            amount = smoothstep(tent);
        } else {
            const double stagger = clamp_value(config.stagger, 0.0, 1.0);
            const std::size_t progression_rank = config.cycles_per_loop < 0
                ? fragment_count - 1U - ranks[fragment]
                : ranks[fragment];
            const double delay = stagger
                * static_cast<double>(progression_rank)
                / static_cast<double>(fragment_count);
            const double staged = smoothstep((tent - delay) / (1.0 - delay));
            amount = config.mode == MeshConstructionMode::Reconstruct
                         ? 1.0 - staged : staged;
        }
        transform.amount = amount;
        transform.distance = config.distance * distance_units * amount;
        transform.angle = config.rotation_degrees * kPi / 180.0 * amount;
        transform.scale = 1.0
            + (config.minimum_scale - 1.0) * amount;
        any_active = any_active || amount != 0.0;
    }
    if (!any_active) {
        plan.triangle_fragments.clear();
        plan.fragments.clear();
    }
    return plan;
}

ProjectionContext make_projection_context(
    const ObjMesh& mesh, int width, int height, const SurfaceConfig& surface) {
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
    ProjectionContext context;
    context.screen_scale_x = contain * size;
    context.screen_scale_y = contain * size;
    switch (surface.sizing) {
        case SurfaceSizing::Contain:
            break;
        case SurfaceSizing::Cover:
            context.screen_scale_x = cover * size;
            context.screen_scale_y = cover * size;
            break;
        case SurfaceSizing::Stretch:
            context.screen_scale_x = width_span / (2.0 * half_x) * size;
            context.screen_scale_y = height_span / (2.0 * half_y) * size;
            break;
        case SurfaceSizing::ShortSide:
            context.screen_scale_x = 0.5 * static_cast<double>(
                std::min(width, height)) * size;
            context.screen_scale_y = context.screen_scale_x;
            break;
    }
    context.center_x = 0.5 * width_span
        + surface.position_x_percent * width_span / 100.0;
    context.center_y = 0.5 * height_span
        - surface.position_y_percent * height_span / 100.0;
    return context;
}

ProjectedVertex project_object_position(
    ObjVec3 object,
    const ProjectionContext& context,
    const SurfaceConfig& surface,
    const SurfaceAngles& angles) {
    ProjectedVertex projected;
    const ObjVec3 scaled = {
        object.x * surface.scale_x,
        object.y * surface.scale_y,
        object.z * surface.scale_z};
    projected.world = rotate_surface(scaled, angles, surface.rotation_order);
    projected.world.z += surface.position_z;
    projected.camera_depth = surface.camera_distance - projected.world.z;
    project_camera_vertex(projected, context, surface);
    return projected;
}

void project_camera_vertex(ProjectedVertex& projected,
                           const ProjectionContext& context,
                           const SurfaceConfig& surface) {
    if (!std::isfinite(projected.camera_depth)
        || projected.camera_depth < kMinimumCameraDepth) {
        return;
    }
    if (surface.projection == SurfaceProjection::Perspective) {
        projected.inverse_depth = 1.0 / projected.camera_depth;
        projected.screen = {
            context.center_x + projected.world.x * surface.focal_length
                / projected.camera_depth * context.screen_scale_x,
            context.center_y - projected.world.y * surface.focal_length
                / projected.camera_depth * context.screen_scale_y};
    } else {
        projected.inverse_depth = 1.0;
        projected.screen = {
            context.center_x + projected.world.x * context.screen_scale_x,
            context.center_y - projected.world.y * context.screen_scale_y};
    }
    projected.valid = std::isfinite(projected.screen.x)
                      && std::isfinite(projected.screen.y);
}

std::vector<ProjectedVertex> project_positions(
    const ObjMesh& mesh, const ProjectionContext& context,
    const SurfaceConfig& surface,
    const SurfaceAngles& angles, const std::atomic_bool* cancel) {
    std::vector<ProjectedVertex> result(mesh.positions.size());
    for (std::size_t index = 0U; index < mesh.positions.size(); ++index) {
        if ((index & 4095U) == 0U) throw_if_cancelled(cancel);
        result[index] = project_object_position(
            surface_object_position(mesh, mesh.positions[index], surface),
            context, surface, angles);
    }
    return result;
}

ObjVec3 transform_normal(ObjVec3 normal,
                         const SurfaceConfig& surface,
                         const SurfaceAngles& angles,
                         const FragmentTransform* fragment = nullptr) {
    if (fragment != nullptr && fragment->amount != 0.0) {
        normal = rotate_axis(normal, fragment->axis, fragment->angle);
    }
    const ObjVec3 inverse_scaled = {
        normal.x / surface.scale_x,
        normal.y / surface.scale_y,
        normal.z / surface.scale_z};
    return normalize(rotate_surface(
        inverse_scaled, angles, surface.rotation_order));
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
        result.push_back(transform_normal(normal, surface, angles));
    }
    return result;
}

void blend_with_planar(const Image& source, Image& mapped,
                       const PackedMask& coverage,
                       std::size_t pixel_count, const SurfaceConfig& surface,
                       double curvature,
                       const std::atomic_bool* cancel) {
    for (std::size_t pixel = 0U; pixel < pixel_count; ++pixel) {
        if ((pixel & 4095U) == 0U) throw_if_cancelled(cancel);
        const Color planar = load_color(source, pixel);
        if (!coverage.test(pixel)
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
    PackedMask& coverage;
    RasterOcclusion& occlusion;
    bool can_cull() const { return occlusion.has_coverage(); }
    bool occludes(int left, int right, int top, int bottom, double minimum_depth) const {
        return occlusion.occludes(left, right, top, bottom, minimum_depth);
    }

    bool accepts(std::size_t pixel, double candidate_depth) const {
        return candidate_depth < depth[pixel];
    }

    void store(std::size_t pixel, double candidate_depth, Color color) {
        depth[pixel] = static_cast<float>(candidate_depth);
        if (!coverage.test(pixel)) occlusion.first_hit(pixel, depth);
        store_color(mapped, pixel, color);
        coverage.set(pixel);
    }
};

struct LayerFragments {
    bool can_cull() const { return false; }
    bool occludes(int,int,int,int,double) const { return false; }
    const std::vector<float>& previous_depth;
    std::vector<float>& next_depth;
    Image& layer;
    PackedMask& coverage;

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
        coverage.set(pixel);
    }
};

} // namespace

bool mesh_geometry_memory_requirements(
    std::size_t positions, std::size_t normals, std::size_t triangles,
    bool opengl_upload, MeshGeometryMemory& result) noexcept {
    MeshGeometryMemory candidate;
    const auto add = [](std::size_t bytes, std::size_t& total) {
        if (bytes > (std::numeric_limits<std::size_t>::max)() - total) return false;
        total += bytes;
        return true;
    };
    if (!checked_multiply(positions, sizeof(ProjectedVertex), candidate.projected_vertices)
        || !checked_multiply(normals, sizeof(ObjVec3), candidate.transformed_normals)) {
        return false;
    }
    if (opengl_upload) {
        std::size_t vertices = 0U;
        std::size_t indices = 0U;
        if (!checked_multiply(positions, 8U * sizeof(float), vertices)
            || !checked_multiply(triangles, 3U * sizeof(std::uint32_t), indices)
            || !add(vertices, candidate.upload_staging)
            || !add(indices, candidate.upload_staging)) return false;
        candidate.gpu_buffers = candidate.upload_staging;
    }
    if (!add(candidate.projected_vertices, candidate.total)
        || !add(candidate.transformed_normals, candidate.total)
        || !add(candidate.upload_staging, candidate.total)
        || !add(candidate.gpu_buffers, candidate.total)) return false;
    result = candidate;
    return true;
}

bool mesh_occlusion_memory_requirements(int width, int height, std::size_t& bytes) noexcept {
    if (width <= 0 || height <= 0) return false;
    std::size_t count = 0U, candidate = 0U;
    if (!checked_multiply(static_cast<std::size_t>(width / 8 + (width % 8 != 0)),
                          static_cast<std::size_t>(height / 8 + (height % 8 != 0)), count) ||
        !checked_multiply(count, RasterOcclusion::tile_bytes, candidate))
        return false;
    bytes = candidate;
    return true;
}

bool apply_mesh_surface_mapping(const Image& source,
                                Image& destination,
                                const ObjMesh& mesh,
                                const SurfaceConfig& surface,
                                double loop_phase,
                                std::string* error,
                                const std::atomic_bool* cancel,
                                bool cull_occluded) {
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
    const MeshConstructionConfig& construction = surface.mesh_construction;
    const bool valid_construction_mode =
        construction.mode == MeshConstructionMode::None
        || construction.mode == MeshConstructionMode::Explode
        || construction.mode == MeshConstructionMode::Deconstruct
        || construction.mode == MeshConstructionMode::Reconstruct;
    const bool valid_fragmentation =
        construction.fragmentation == MeshFragmentation::Automatic
        || construction.fragmentation == MeshFragmentation::ConnectedComponents
        || construction.fragmentation == MeshFragmentation::TriangleClusters;
    if (!valid_construction_mode || !valid_fragmentation) {
        return fail(error, "Mesh construction mode or fragmentation is invalid.");
    }
    if (construction.mode != MeshConstructionMode::None
        && (construction.target_fragments < 1
            || static_cast<std::size_t>(construction.target_fragments)
                   > kMaximumMeshFragments
            || !std::isfinite(construction.phase_degrees)
            || !std::isfinite(construction.distance)
            || !std::isfinite(construction.rotation_degrees)
            || !std::isfinite(construction.minimum_scale)
            || !std::isfinite(construction.stagger)
            || construction.distance < 0.0
            || construction.minimum_scale < 0.0
            || construction.minimum_scale > 1.0
            || construction.stagger < 0.0
            || construction.stagger > 1.0)) {
        return fail(error,
                    "Active mesh construction parameters are outside their safe bounds.");
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
        const ProjectionContext projection_context = make_projection_context(
            mesh, source.width, source.height, surface);
        const std::vector<ProjectedVertex> projected =
            project_positions(mesh, projection_context, surface, angles,
                              cancel);
        const std::vector<ObjVec3> world_normals =
            transform_normals(mesh, surface, angles, cancel);
        const MeshConstructionPlan construction_plan =
            build_construction_plan(mesh, surface, wrapped_loop_phase, cancel);
        PreparedEnvironmentMap prepared_environment;
        const PreparedEnvironmentMap* environment = nullptr;
        if (surface.environment_map.enabled && surface.lighting > 0.0
            && surface.environment_map.mix > 0.0) {
            if (!prepare_environment_map(surface.environment_map,
                                         prepared_environment, cancel,
                                         error)) {
                return false;
            }
            environment = &prepared_environment;
        }

        Image mapped;
        mapped.width = source.width;
        mapped.height = source.height;
        mapped.pixels.assign(pixel_count * 4U, 0.0F);
        PackedMask coverage(pixel_count);
        const double mapped_lighting = clamp_value(
            surface.lighting, 0.0,
            maximum_render_parameter_magnitude()) * curvature;

        if (source_is_opaque(source, pixel_count, cancel)
            || !surface.composite_backfaces) {
            std::vector<float> depth(pixel_count,
                                     std::numeric_limits<float>::infinity());
            // Small meshes seldom amortize interval/tile setup.
            RasterOcclusion occlusion(source.width,source.height,
                                       cull_occluded && mesh.triangles.size() >= 64U);
            OpaqueFragments fragments{depth, mapped, coverage, occlusion};
            rasterize_mesh(mesh, projected, world_normals, projection_context,
                           construction_plan, angles, source,
                           source.width, source.height, surface,
                           mapped_lighting, environment, cancel, fragments);
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
                rasterize_mesh(mesh, projected, world_normals,
                               projection_context, construction_plan, angles,
                               source,
                               source.width, source.height, surface,
                               mapped_lighting, environment, cancel, fragments);

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
