#include "displacement_surface.h"

#include "obj_surface.h"
#include "source_image.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace pvt::detail {
namespace {

constexpr std::size_t kMaximumCachedMeshBytes =
    std::size_t{512} * 1024U * 1024U;
constexpr std::size_t kMaximumCachedMeshes = 16U;

struct DisplacementCancelled final {};

struct CachedMesh {
    std::shared_ptr<const Image> height_image;
    int render_width = 0;
    int render_height = 0;
    int pixels_per_node = 0;
    double minimum = 0.0;
    double maximum = 0.0;
    double midpoint = 0.0;
    std::shared_ptr<const ObjMesh> mesh;
    std::size_t bytes = 0U;
    std::uint64_t last_used = 0U;
};

std::mutex mesh_cache_mutex;
std::vector<CachedMesh> mesh_cache;
std::size_t mesh_cache_bytes = 0U;
std::uint64_t mesh_cache_clock = 0U;

bool fail(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
}

void clear_error(std::string* error) {
    if (error != nullptr) error->clear();
}

void throw_if_cancelled(const std::atomic_bool* cancel) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
        throw DisplacementCancelled{};
    }
}

bool checked_add(std::size_t first, std::size_t second,
                 std::size_t& result) {
    if (second > (std::numeric_limits<std::size_t>::max)() - first) {
        return false;
    }
    result = first + second;
    return true;
}

bool checked_multiply(std::size_t first, std::size_t second,
                      std::size_t& result) {
    if (first != 0U
        && second > (std::numeric_limits<std::size_t>::max)() / first) {
        return false;
    }
    result = first * second;
    return true;
}

ObjVec3 subtract(ObjVec3 first, ObjVec3 second) {
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}

ObjVec3 cross(ObjVec3 first, ObjVec3 second) {
    return {first.y * second.z - first.z * second.y,
            first.z * second.x - first.x * second.z,
            first.x * second.y - first.y * second.x};
}

ObjVec3 normalize(ObjVec3 value) {
    const double squared = value.x * value.x + value.y * value.y
                           + value.z * value.z;
    if (!std::isfinite(squared) || squared <= 1.0e-24) {
        return {0.0, 0.0, 1.0};
    }
    const double inverse = 1.0 / std::sqrt(squared);
    return {value.x * inverse, value.y * inverse, value.z * inverse};
}

double sample_height(const Image& image, double x, double y) {
    x = std::clamp(x, 0.0, static_cast<double>(image.width - 1));
    y = std::clamp(y, 0.0, static_cast<double>(image.height - 1));
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, image.width - 1);
    const int y1 = std::min(y0 + 1, image.height - 1);
    const double tx = x - x0;
    const double ty = y - y0;
    const auto luminance = [&image](int px, int py) {
        const std::size_t offset =
            (static_cast<std::size_t>(py)
                 * static_cast<std::size_t>(image.width)
             + static_cast<std::size_t>(px))
            * 4U;
        return 0.2126 * image.pixels[offset]
               + 0.7152 * image.pixels[offset + 1U]
               + 0.0722 * image.pixels[offset + 2U];
    };
    const double top = luminance(x0, y0)
                       + (luminance(x1, y0) - luminance(x0, y0)) * tx;
    const double bottom = luminance(x0, y1)
                          + (luminance(x1, y1) - luminance(x0, y1)) * tx;
    return std::clamp(top + (bottom - top) * ty, 0.0, 1.0);
}

double map_displacement(double sample,
                        const PlaneDisplacementConfig& config) {
    sample = std::clamp(sample, 0.0, 1.0);
    if (sample <= config.midpoint) {
        if (config.midpoint <= 0.0) return 0.0;
        const double amount = sample / config.midpoint;
        return config.minimum + (0.0 - config.minimum) * amount;
    }
    if (config.midpoint >= 1.0) return 0.0;
    const double amount = (sample - config.midpoint)
                          / (1.0 - config.midpoint);
    return config.maximum * amount;
}

bool cache_key_matches(const CachedMesh& cached,
                       const std::shared_ptr<const Image>& height_image,
                       const PlaneDisplacementConfig& config,
                       int render_width,
                       int render_height) {
    return cached.height_image == height_image
           && cached.render_width == render_width
           && cached.render_height == render_height
           && cached.pixels_per_node == config.pixels_per_node
           && cached.minimum == config.minimum
           && cached.maximum == config.maximum
           && cached.midpoint == config.midpoint
           && cached.mesh;
}

bool build_mesh(const Image& height_image,
                const PlaneDisplacementConfig& config,
                int render_width,
                int render_height,
                std::shared_ptr<const ObjMesh>& destination,
                const std::atomic_bool* cancel,
                std::string* error) {
    std::size_t columns = 0U;
    std::size_t rows = 0U;
    std::size_t estimated_bytes = 0U;
    if (!displacement_mesh_requirements(
            render_width, render_height, config.pixels_per_node,
            columns, rows, estimated_bytes, error)) {
        return false;
    }
    try {
        throw_if_cancelled(cancel);
        auto mesh = std::make_shared<ObjMesh>();
        const std::size_t vertex_count = columns * rows;
        const std::size_t triangle_count =
            (columns - 1U) * (rows - 1U) * 2U;
        mesh->positions.resize(vertex_count);
        mesh->texcoords.resize(vertex_count);
        mesh->normals.resize(vertex_count);
        mesh->triangles.resize(triangle_count);

        const double aspect = static_cast<double>(render_width)
                              / static_cast<double>(render_height);
        const double half_width = aspect;
        for (std::size_t row = 0U; row < rows; ++row) {
            throw_if_cancelled(cancel);
            const double v = rows > 1U
                                 ? static_cast<double>(row)
                                       / static_cast<double>(rows - 1U)
                                 : 0.5;
            for (std::size_t column = 0U; column < columns; ++column) {
                const double u = columns > 1U
                                     ? static_cast<double>(column)
                                           / static_cast<double>(columns - 1U)
                                     : 0.5;
                const double height = map_displacement(
                    sample_height(
                        height_image,
                        u * static_cast<double>(height_image.width - 1),
                        v * static_cast<double>(height_image.height - 1)),
                    config);
                const std::size_t index = row * columns + column;
                mesh->positions[index] = {
                    (2.0 * u - 1.0) * half_width,
                    1.0 - 2.0 * v,
                    height};
                mesh->texcoords[index] = {u, 1.0 - v};
            }
        }

        for (std::size_t row = 0U; row < rows; ++row) {
            throw_if_cancelled(cancel);
            const std::size_t up = row == 0U ? row : row - 1U;
            const std::size_t down = std::min(row + 1U, rows - 1U);
            for (std::size_t column = 0U; column < columns; ++column) {
                const std::size_t left = column == 0U ? column : column - 1U;
                const std::size_t right = std::min(column + 1U, columns - 1U);
                const ObjVec3 tangent_x = subtract(
                    mesh->positions[row * columns + right],
                    mesh->positions[row * columns + left]);
                const ObjVec3 tangent_y = subtract(
                    mesh->positions[up * columns + column],
                    mesh->positions[down * columns + column]);
                mesh->normals[row * columns + column] = normalize(
                    cross(tangent_x, tangent_y));
            }
        }

        std::size_t triangle = 0U;
        for (std::size_t row = 0U; row + 1U < rows; ++row) {
            throw_if_cancelled(cancel);
            for (std::size_t column = 0U; column + 1U < columns; ++column) {
                const std::uint32_t top_left = static_cast<std::uint32_t>(
                    row * columns + column);
                const std::uint32_t top_right = top_left + 1U;
                const std::uint32_t bottom_left = static_cast<std::uint32_t>(
                    (row + 1U) * columns + column);
                const std::uint32_t bottom_right = bottom_left + 1U;
                const auto corner = [](std::uint32_t index) {
                    return ObjCorner{index, index, index};
                };
                mesh->triangles[triangle++].corners = {
                    corner(top_left), corner(bottom_left), corner(top_right)};
                mesh->triangles[triangle++].corners = {
                    corner(top_right), corner(bottom_left), corner(bottom_right)};
            }
        }

        mesh->bounds_min = {-half_width, -1.0, config.minimum};
        mesh->bounds_max = {half_width, 1.0, config.maximum};
        mesh->normalization_center = {};
        // Preserve the authored displacement scale instead of renormalizing it
        // every time the map extrema change. The plane's longest 2D axis alone
        // establishes its stable fit in the renderer and exported OBJ.
        mesh->normalization_scale = 1.0 / std::max(1.0, half_width);
        // The regular grid is connected by construction. Populate its immutable
        // topology metadata directly instead of allocating a temporary union-
        // find several times larger than a high-resolution plane.
        mesh->triangle_components.assign(triangle_count, 0U);
        mesh->connected_component_count = triangle_count == 0U ? 0U : 1U;
        destination = std::move(mesh);
        clear_error(error);
        return true;
    } catch (const DisplacementCancelled&) {
        return fail(error,
                    "Displacement-plane generation was cancelled; the cached mesh was unchanged.");
    } catch (const std::bad_alloc&) {
        return fail(error,
                    "Not enough memory to generate the displacement plane; the cached mesh was unchanged.");
    } catch (const std::exception& exception) {
        return fail(error,
                    std::string("Could not generate the displacement plane: ")
                        + exception.what());
    }
}

} // namespace

bool displacement_mesh_requirements(int render_width,
                                    int render_height,
                                    int pixels_per_node,
                                    std::size_t& columns,
                                    std::size_t& rows,
                                    std::size_t& estimated_bytes,
                                    std::string* error) {
    columns = 0U;
    rows = 0U;
    estimated_bytes = 0U;
    if (render_width < 2 || render_height < 2 || pixels_per_node < 1) {
        return fail(error,
                    "A displacement plane requires dimensions of at least 2x2 and a positive pixel-to-node ratio.");
    }
    columns = 1U + (static_cast<std::size_t>(render_width - 1)
                    + static_cast<std::size_t>(pixels_per_node - 1))
                       / static_cast<std::size_t>(pixels_per_node);
    rows = 1U + (static_cast<std::size_t>(render_height - 1)
                 + static_cast<std::size_t>(pixels_per_node - 1))
                    / static_cast<std::size_t>(pixels_per_node);
    std::size_t vertex_count = 0U;
    std::size_t cell_count = 0U;
    std::size_t triangle_count = 0U;
    if (!checked_multiply(columns, rows, vertex_count)
        || vertex_count > ObjCorner::missing
        || !checked_multiply(columns - 1U, rows - 1U, cell_count)
        || !checked_multiply(cell_count, 2U, triangle_count)) {
        return fail(error,
                    "The displacement-plane subdivision grid exceeds representable mesh indices.");
    }
    std::size_t vertex_bytes = 0U;
    std::size_t triangle_bytes = 0U;
    std::size_t component_bytes = 0U;
    std::size_t per_vertex = 0U;
    if (!checked_add(sizeof(ObjVec3), sizeof(ObjVec2), per_vertex)
        || !checked_add(per_vertex, sizeof(ObjVec3), per_vertex)
        || !checked_multiply(vertex_count, per_vertex, vertex_bytes)
        || !checked_multiply(triangle_count, sizeof(ObjTriangle),
                             triangle_bytes)
        || !checked_multiply(triangle_count, sizeof(std::size_t),
                             component_bytes)
        || !checked_add(vertex_bytes, triangle_bytes, estimated_bytes)
        || !checked_add(estimated_bytes, component_bytes, estimated_bytes)) {
        return fail(error,
                    "The displacement-plane mesh allocation exceeds addressable memory.");
    }
    clear_error(error);
    return true;
}

bool load_displacement_plane_mesh(
    const PlaneDisplacementConfig& displacement,
    int render_width,
    int render_height,
    std::shared_ptr<const ObjMesh>& destination,
    const std::atomic_bool* cancel,
    std::string* error) {
    clear_error(error);
    if (displacement.path.empty()) {
        return fail(error, "A displacement height-map image has not been selected.");
    }
    std::shared_ptr<const Image> height_image;
    if (!load_data_image_source(
            displacement.path, height_image, cancel, error)) {
        if (error != nullptr && !error->empty()) {
            *error = "Could not load displacement height map: " + *error;
        }
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(mesh_cache_mutex);
        const auto found = std::find_if(
            mesh_cache.begin(), mesh_cache.end(),
            [&](const CachedMesh& candidate) {
                return cache_key_matches(candidate, height_image, displacement,
                                         render_width, render_height);
            });
        if (found != mesh_cache.end()) {
            found->last_used = ++mesh_cache_clock;
            destination = found->mesh;
            return true;
        }
    }

    std::shared_ptr<const ObjMesh> generated;
    if (!build_mesh(*height_image, displacement, render_width, render_height,
                    generated, cancel, error)) {
        return false;
    }
    const std::size_t generated_bytes = generated->estimated_bytes();
    {
        const std::lock_guard<std::mutex> lock(mesh_cache_mutex);
        const auto existing = std::find_if(
            mesh_cache.begin(), mesh_cache.end(),
            [&](const CachedMesh& candidate) {
                return cache_key_matches(candidate, height_image, displacement,
                                         render_width, render_height);
            });
        if (existing != mesh_cache.end()) {
            existing->last_used = ++mesh_cache_clock;
            destination = existing->mesh;
            return true;
        }
        mesh_cache.push_back({height_image, render_width, render_height,
                              displacement.pixels_per_node,
                              displacement.minimum, displacement.maximum,
                              displacement.midpoint, generated,
                              generated_bytes, ++mesh_cache_clock});
        mesh_cache_bytes += generated_bytes;
        while ((mesh_cache.size() > kMaximumCachedMeshes
                || mesh_cache_bytes > kMaximumCachedMeshBytes)
               && mesh_cache.size() > 1U) {
            const auto oldest = std::min_element(
                mesh_cache.begin(), mesh_cache.end(),
                [](const CachedMesh& left, const CachedMesh& right) {
                    return left.last_used < right.last_used;
                });
            mesh_cache_bytes -= oldest->bytes;
            mesh_cache.erase(oldest);
        }
    }
    destination = std::move(generated);
    return true;
}

bool apply_displacement_plane_mapping(
    const Image& source,
    Image& destination,
    const SurfaceConfig& surface,
    double loop_phase,
    std::string* error,
    const std::atomic_bool* cancel) {
    std::shared_ptr<const ObjMesh> mesh;
    if (!load_displacement_plane_mesh(
            surface.plane_displacement, source.width, source.height,
            mesh, cancel, error)) {
        return false;
    }
    return apply_mesh_surface_mapping(
        source, destination, *mesh, surface, loop_phase, error, cancel);
}

void clear_displacement_mesh_cache() noexcept {
    const std::lock_guard<std::mutex> lock(mesh_cache_mutex);
    mesh_cache.clear();
    mesh_cache_bytes = 0U;
    mesh_cache_clock = 0U;
}

} // namespace pvt::detail
