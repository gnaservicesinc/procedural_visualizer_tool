#include "obj_mesh.h"
#include "path_utf8.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <locale>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace pvt {
namespace detail {
namespace {

constexpr std::size_t kMaximumPathBytes = 4095U;
constexpr std::size_t kMaximumNumericTokenBytes = 128U;
constexpr double kGeometryEpsilon = 1.0e-12;

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

std::string line_error(std::size_t line, std::string_view message) {
    return "OBJ line " + std::to_string(line) + ": " + std::string(message);
}

bool checked_add(std::size_t left, std::size_t right, std::size_t& result) {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

bool checked_multiply(std::size_t left, std::size_t right, std::size_t& result) {
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

std::vector<std::string_view> tokens(std::string_view line) {
    std::vector<std::string_view> result;
    std::size_t cursor = 0U;
    while (cursor < line.size()) {
        while (cursor < line.size()
               && (line[cursor] == ' ' || line[cursor] == '\t')) {
            ++cursor;
        }
        const std::size_t start = cursor;
        while (cursor < line.size()
               && line[cursor] != ' ' && line[cursor] != '\t') {
            ++cursor;
        }
        if (cursor > start) {
            result.emplace_back(line.data() + start, cursor - start);
        }
    }
    return result;
}

bool parse_double(std::string_view token, double& value) {
    if (token.empty() || token.size() > kMaximumNumericTokenBytes) {
        return false;
    }
    std::istringstream stream{std::string(token)};
    stream.imbue(std::locale::classic());
    stream >> std::noskipws;
    double parsed = 0.0;
    if (!(stream >> parsed) || stream.peek() != std::char_traits<char>::eof()
        || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

bool parse_index(std::string_view token, std::int64_t& value) {
    if (token.empty() || token.size() > kMaximumNumericTokenBytes) {
        return false;
    }
    std::int64_t parsed = 0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(),
                                        parsed, 10);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()
        || parsed == 0) {
        return false;
    }
    value = parsed;
    return true;
}

bool resolve_index(std::string_view token,
                   std::size_t count,
                   std::uint32_t& destination) {
    std::int64_t index = 0;
    if (!parse_index(token, index)) {
        return false;
    }
    const std::int64_t count_signed = static_cast<std::int64_t>(count);
    const std::int64_t resolved = index > 0 ? index - 1 : count_signed + index;
    if (resolved < 0 || resolved >= count_signed
        || static_cast<std::uint64_t>(resolved)
               >= static_cast<std::uint64_t>(ObjCorner::missing)) {
        return false;
    }
    destination = static_cast<std::uint32_t>(resolved);
    return true;
}

bool parse_corner(std::string_view token,
                  const ObjMesh& mesh,
                  ObjCorner& corner) {
    std::array<std::string_view, 3U> fields{};
    std::size_t field_count = 0U;
    std::size_t start = 0U;
    for (;;) {
        if (field_count >= fields.size()) {
            return false;
        }
        const std::size_t slash = token.find('/', start);
        const std::size_t end = slash == std::string_view::npos
                                  ? token.size() : slash;
        fields[field_count++] = token.substr(start, end - start);
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1U;
    }

    if (fields[0].empty()
        || !resolve_index(fields[0], mesh.positions.size(), corner.position)) {
        return false;
    }
    if (field_count == 1U) {
        return true;
    }
    if (field_count == 2U) {
        return !fields[1].empty()
               && resolve_index(fields[1], mesh.texcoords.size(), corner.texcoord);
    }
    if (!fields[1].empty()
        && !resolve_index(fields[1], mesh.texcoords.size(), corner.texcoord)) {
        return false;
    }
    return !fields[2].empty()
           && resolve_index(fields[2], mesh.normals.size(), corner.normal);
}

ObjVec3 subtract(ObjVec3 first, ObjVec3 second) {
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}

double length_squared(ObjVec3 value) {
    return value.x * value.x + value.y * value.y + value.z * value.z;
}

ObjVec2 project_position(ObjVec3 position, std::size_t dropped_axis) {
    switch (dropped_axis) {
        case 0U: return {position.y, position.z};
        case 1U: return {position.x, position.z};
        default: return {position.x, position.y};
    }
}

double cross_2d(ObjVec2 first, ObjVec2 second, ObjVec2 third) {
    return (second.x - first.x) * (third.y - first.y)
           - (second.y - first.y) * (third.x - first.x);
}

int sign_with_epsilon(double value, double epsilon) {
    return value > epsilon ? 1 : (value < -epsilon ? -1 : 0);
}

bool point_on_segment(ObjVec2 point, ObjVec2 first, ObjVec2 second,
                      double epsilon) {
    if (std::fabs(cross_2d(first, second, point)) > epsilon) {
        return false;
    }
    return point.x >= std::min(first.x, second.x) - epsilon
           && point.x <= std::max(first.x, second.x) + epsilon
           && point.y >= std::min(first.y, second.y) - epsilon
           && point.y <= std::max(first.y, second.y) + epsilon;
}

bool segments_intersect(ObjVec2 a, ObjVec2 b, ObjVec2 c, ObjVec2 d,
                        double epsilon) {
    const int ab_c = sign_with_epsilon(cross_2d(a, b, c), epsilon);
    const int ab_d = sign_with_epsilon(cross_2d(a, b, d), epsilon);
    const int cd_a = sign_with_epsilon(cross_2d(c, d, a), epsilon);
    const int cd_b = sign_with_epsilon(cross_2d(c, d, b), epsilon);
    if (ab_c != 0 && ab_d != 0 && cd_a != 0 && cd_b != 0) {
        return ab_c != ab_d && cd_a != cd_b;
    }
    return (ab_c == 0 && point_on_segment(c, a, b, epsilon))
           || (ab_d == 0 && point_on_segment(d, a, b, epsilon))
           || (cd_a == 0 && point_on_segment(a, c, d, epsilon))
           || (cd_b == 0 && point_on_segment(b, c, d, epsilon));
}

bool point_in_triangle(ObjVec2 point, ObjVec2 first, ObjVec2 second,
                       ObjVec2 third, int orientation, double epsilon) {
    const double first_edge = orientation * cross_2d(first, second, point);
    const double second_edge = orientation * cross_2d(second, third, point);
    const double third_edge = orientation * cross_2d(third, first, point);
    return first_edge >= -epsilon && second_edge >= -epsilon
           && third_edge >= -epsilon;
}

bool triangulate_polygon(const std::vector<ObjCorner>& input,
                         ObjMesh& mesh,
                         std::size_t line,
                         const ObjLoadLimits& limits,
                         std::string* error) {
    std::vector<ObjCorner> polygon = input;
    if (polygon.size() > 3U
        && polygon.front().position == polygon.back().position) {
        polygon.pop_back();
    }
    if (polygon.size() < 3U) {
        return fail(error, line_error(line, "face has fewer than three distinct corners"));
    }
    for (std::size_t i = 0U; i < polygon.size(); ++i) {
        const std::size_t next = (i + 1U) % polygon.size();
        if (polygon[i].position == polygon[next].position) {
            return fail(error, line_error(line, "face has adjacent duplicate vertices"));
        }
    }

    ObjVec3 newell{};
    for (std::size_t i = 0U; i < polygon.size(); ++i) {
        const ObjVec3 current = mesh.positions[polygon[i].position];
        const ObjVec3 next = mesh.positions[polygon[(i + 1U) % polygon.size()].position];
        newell.x += (current.y - next.y) * (current.z + next.z);
        newell.y += (current.z - next.z) * (current.x + next.x);
        newell.z += (current.x - next.x) * (current.y + next.y);
    }
    if (!std::isfinite(length_squared(newell))
        || length_squared(newell) <= kGeometryEpsilon * kGeometryEpsilon) {
        return fail(error, line_error(line, "face is geometrically degenerate"));
    }

    const std::array<double, 3U> components = {
        std::fabs(newell.x), std::fabs(newell.y), std::fabs(newell.z)};
    const std::size_t dropped_axis = static_cast<std::size_t>(
        std::distance(components.begin(),
                      std::max_element(components.begin(), components.end())));

    std::vector<ObjVec2> projected;
    projected.reserve(polygon.size());
    double maximum_coordinate = 1.0;
    for (const ObjCorner& corner : polygon) {
        const ObjVec2 point = project_position(mesh.positions[corner.position],
                                               dropped_axis);
        projected.push_back(point);
        maximum_coordinate = std::max(maximum_coordinate,
                                      std::max(std::fabs(point.x), std::fabs(point.y)));
    }
    const double epsilon = kGeometryEpsilon * maximum_coordinate * maximum_coordinate;

    double twice_area = 0.0;
    for (std::size_t i = 0U; i < projected.size(); ++i) {
        const ObjVec2 first = projected[i];
        const ObjVec2 second = projected[(i + 1U) % projected.size()];
        twice_area += first.x * second.y - second.x * first.y;
    }
    if (std::fabs(twice_area) <= epsilon) {
        return fail(error, line_error(line, "face projection has zero area"));
    }
    const int orientation = twice_area > 0.0 ? 1 : -1;
    // Reject self-intersecting polygons instead of producing unpredictable
    // overlapping triangles. Adjacent edges share a permitted endpoint.
    for (std::size_t first = 0U; first < projected.size(); ++first) {
        const std::size_t first_next = (first + 1U) % projected.size();
        for (std::size_t second = first + 1U; second < projected.size(); ++second) {
            const std::size_t second_next = (second + 1U) % projected.size();
            if (first == second || first_next == second
                || second_next == first) {
                continue;
            }
            if (segments_intersect(projected[first], projected[first_next],
                                   projected[second], projected[second_next],
                                   epsilon)) {
                return fail(error, line_error(line, "face polygon self-intersects"));
            }
        }
    }

    const std::size_t added_triangles = polygon.size() - 2U;
    if (added_triangles > limits.maximum_triangles
        || mesh.triangles.size() > limits.maximum_triangles - added_triangles) {
        return fail(error, line_error(line, "triangulated mesh exceeds triangle limit"));
    }

    std::vector<std::size_t> remaining(polygon.size());
    for (std::size_t index = 0U; index < remaining.size(); ++index) {
        remaining[index] = index;
    }
    while (remaining.size() > 3U) {
        bool clipped = false;
        for (std::size_t cursor = 0U; cursor < remaining.size(); ++cursor) {
            const std::size_t previous = remaining[
                (cursor + remaining.size() - 1U) % remaining.size()];
            const std::size_t current = remaining[cursor];
            const std::size_t next = remaining[(cursor + 1U) % remaining.size()];
            if (orientation * cross_2d(projected[previous], projected[current],
                                       projected[next]) <= epsilon) {
                continue;
            }
            bool contains_vertex = false;
            for (const std::size_t candidate : remaining) {
                if (candidate == previous || candidate == current
                    || candidate == next) {
                    continue;
                }
                if (point_in_triangle(projected[candidate], projected[previous],
                                      projected[current], projected[next],
                                      orientation, epsilon)) {
                    contains_vertex = true;
                    break;
                }
            }
            if (contains_vertex) {
                continue;
            }
            mesh.triangles.push_back(
                {{{polygon[previous], polygon[current], polygon[next]}}});
            remaining.erase(remaining.begin()
                            + static_cast<std::ptrdiff_t>(cursor));
            clipped = true;
            break;
        }
        if (!clipped) {
            return fail(error, line_error(
                line, "face could not be triangulated cleanly"));
        }
    }
    mesh.triangles.push_back(
        {{{polygon[remaining[0]], polygon[remaining[1]], polygon[remaining[2]]}}});
    return true;
}

bool parse_vertex_record(const std::vector<std::string_view>& fields,
                         ObjMesh& mesh,
                         std::size_t line,
                         const ObjLoadLimits& limits,
                         std::string* error) {
    if (fields.size() != 4U && fields.size() != 5U) {
        return fail(error, line_error(line, "v expects x y z and optional w"));
    }
    if (mesh.positions.size() >= limits.maximum_positions) {
        return fail(error, line_error(line, "position limit exceeded"));
    }
    ObjVec3 position;
    if (!parse_double(fields[1], position.x)
        || !parse_double(fields[2], position.y)
        || !parse_double(fields[3], position.z)) {
        return fail(error, line_error(line, "v contains a non-finite number"));
    }
    if (fields.size() == 5U) {
        double weight = 0.0;
        if (!parse_double(fields[4], weight) || std::fabs(weight) <= kGeometryEpsilon) {
            return fail(error, line_error(line, "v has an invalid homogeneous w"));
        }
        position.x /= weight;
        position.y /= weight;
        position.z /= weight;
    }
    if (!std::isfinite(position.x) || !std::isfinite(position.y)
        || !std::isfinite(position.z)) {
        return fail(error, line_error(line, "v overflows after homogeneous division"));
    }
    mesh.positions.push_back(position);
    return true;
}

bool parse_texcoord_record(const std::vector<std::string_view>& fields,
                           ObjMesh& mesh,
                           std::size_t line,
                           const ObjLoadLimits& limits,
                           std::string* error) {
    if (fields.size() < 2U || fields.size() > 4U) {
        return fail(error, line_error(line, "vt expects u and optional v/w"));
    }
    if (mesh.texcoords.size() >= limits.maximum_texcoords) {
        return fail(error, line_error(line, "texture-coordinate limit exceeded"));
    }
    ObjVec2 coordinate;
    if (!parse_double(fields[1], coordinate.x)
        || (fields.size() >= 3U && !parse_double(fields[2], coordinate.y))) {
        return fail(error, line_error(line, "vt contains a non-finite number"));
    }
    if (fields.size() == 4U) {
        double ignored = 0.0;
        if (!parse_double(fields[3], ignored)) {
            return fail(error, line_error(line, "vt contains a non-finite w"));
        }
    }
    mesh.texcoords.push_back(coordinate);
    return true;
}

bool parse_normal_record(const std::vector<std::string_view>& fields,
                         ObjMesh& mesh,
                         std::size_t line,
                         const ObjLoadLimits& limits,
                         std::string* error) {
    if (fields.size() != 4U) {
        return fail(error, line_error(line, "vn expects x y z"));
    }
    if (mesh.normals.size() >= limits.maximum_normals) {
        return fail(error, line_error(line, "normal limit exceeded"));
    }
    ObjVec3 normal;
    if (!parse_double(fields[1], normal.x)
        || !parse_double(fields[2], normal.y)
        || !parse_double(fields[3], normal.z)) {
        return fail(error, line_error(line, "vn contains a non-finite number"));
    }
    const double squared = length_squared(normal);
    if (!std::isfinite(squared) || squared <= kGeometryEpsilon * kGeometryEpsilon) {
        return fail(error, line_error(line, "vn is a zero-length normal"));
    }
    const double inverse_length = 1.0 / std::sqrt(squared);
    normal.x *= inverse_length;
    normal.y *= inverse_length;
    normal.z *= inverse_length;
    mesh.normals.push_back(normal);
    return true;
}

bool parse_face_record(const std::vector<std::string_view>& fields,
                       ObjMesh& mesh,
                       std::size_t line,
                       const ObjLoadLimits& limits,
                       std::string* error) {
    if (fields.size() < 4U) {
        return fail(error, line_error(line, "f expects at least three corners"));
    }
    const std::size_t corner_count = fields.size() - 1U;
    if (corner_count > limits.maximum_polygon_corners) {
        return fail(error, line_error(line, "face exceeds polygon-corner limit"));
    }
    std::vector<ObjCorner> polygon;
    polygon.reserve(corner_count);
    for (std::size_t i = 1U; i < fields.size(); ++i) {
        ObjCorner corner;
        if (!parse_corner(fields[i], mesh, corner)) {
            return fail(error, line_error(line, "f contains an invalid or unavailable index"));
        }
        polygon.push_back(corner);
    }
    return triangulate_polygon(polygon, mesh, line, limits, error);
}

bool finalize_mesh(ObjMesh& mesh,
                   const ObjLoadLimits& limits,
                   std::string* error) {
    if (mesh.triangles.empty()) {
        return fail(error, "OBJ contains no usable faces.");
    }
    ObjVec3 minimum{
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity()};
    ObjVec3 maximum{
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()};
    for (const ObjTriangle& triangle : mesh.triangles) {
        for (const ObjCorner& corner : triangle.corners) {
            const ObjVec3 position = mesh.positions[corner.position];
            minimum.x = std::min(minimum.x, position.x);
            minimum.y = std::min(minimum.y, position.y);
            minimum.z = std::min(minimum.z, position.z);
            maximum.x = std::max(maximum.x, position.x);
            maximum.y = std::max(maximum.y, position.y);
            maximum.z = std::max(maximum.z, position.z);
        }
    }
    const ObjVec3 extent = subtract(maximum, minimum);
    const double longest = std::max(extent.x, std::max(extent.y, extent.z));
    if (!std::isfinite(longest) || longest <= kGeometryEpsilon) {
        return fail(error, "OBJ referenced geometry has zero extent.");
    }
    mesh.bounds_min = minimum;
    mesh.bounds_max = maximum;
    mesh.normalization_center = {
        0.5 * (minimum.x + maximum.x),
        0.5 * (minimum.y + maximum.y),
        0.5 * (minimum.z + maximum.z)};
    mesh.normalization_scale = 2.0 / longest;
    if (mesh.estimated_bytes() > limits.maximum_mesh_bytes) {
        return fail(error, "OBJ expanded mesh exceeds the configured memory limit.");
    }
    return true;
}

} // namespace

std::size_t ObjMesh::estimated_bytes() const noexcept {
    std::size_t total = sizeof(*this);
    std::size_t bytes = 0U;
    for (const auto item : {
             std::pair<std::size_t, std::size_t>{positions.capacity(), sizeof(ObjVec3)},
             {texcoords.capacity(), sizeof(ObjVec2)},
             {normals.capacity(), sizeof(ObjVec3)},
             {triangles.capacity(), sizeof(ObjTriangle)}}) {
        if (!checked_multiply(item.first, item.second, bytes)
            || !checked_add(total, bytes, total)) {
            return std::numeric_limits<std::size_t>::max();
        }
    }
    return total;
}

bool parse_obj_mesh(std::string_view contents,
                    ObjMesh& destination,
                    std::string* error,
                    const ObjLoadLimits& limits) {
    clear_error(error);
    if (contents.size() > limits.maximum_file_bytes) {
        return fail(error, "OBJ exceeds the configured file-size limit.");
    }
    try {
        ObjMesh candidate;
        std::size_t line_number = 0U;
        std::size_t cursor = 0U;
        if (contents.size() >= 3U
            && static_cast<unsigned char>(contents[0]) == 0xefU
            && static_cast<unsigned char>(contents[1]) == 0xbbU
            && static_cast<unsigned char>(contents[2]) == 0xbfU) {
            cursor = 3U;
        }
        while (cursor < contents.size()) {
            const std::size_t newline = contents.find('\n', cursor);
            const std::size_t end = newline == std::string_view::npos
                                      ? contents.size() : newline;
            ++line_number;
            if (end - cursor > limits.maximum_line_bytes) {
                return fail(error, line_error(line_number, "line-size limit exceeded"));
            }
            std::string_view line = contents.substr(cursor, end - cursor);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1U);
            }
            const std::size_t comment = line.find('#');
            if (comment != std::string_view::npos) {
                line = line.substr(0U, comment);
            }
            const std::vector<std::string_view> fields = tokens(line);
            if (!fields.empty()) {
                const std::string_view record = fields.front();
                if (record == "v") {
                    if (!parse_vertex_record(fields, candidate, line_number, limits, error)) {
                        return false;
                    }
                } else if (record == "vt") {
                    if (!parse_texcoord_record(fields, candidate, line_number, limits, error)) {
                        return false;
                    }
                } else if (record == "vn") {
                    if (!parse_normal_record(fields, candidate, line_number, limits, error)) {
                        return false;
                    }
                } else if (record == "f") {
                    if (!parse_face_record(fields, candidate, line_number, limits, error)) {
                        return false;
                    }
                }
                // o/g/s/usemtl/mtllib and unsupported geometry records are
                // intentionally ignored. In particular, mtllib never causes a
                // second file to be opened.
            }
            if (newline == std::string_view::npos) {
                break;
            }
            cursor = newline + 1U;
        }
        if (!finalize_mesh(candidate, limits, error)) {
            return false;
        }
        destination = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to parse OBJ; destination was unchanged.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Could not parse OBJ; destination was unchanged: ")
                           + exception.what());
    }
}

bool load_obj_mesh(const std::string& utf8_path,
                   ObjMesh& destination,
                   std::string* error,
                   const ObjLoadLimits& limits) {
    clear_error(error);
    if (utf8_path.empty() || utf8_path.size() > kMaximumPathBytes
        || utf8_path.find('\0') != std::string::npos) {
        return fail(error, "OBJ path is empty, contains NUL, or exceeds 4095 bytes.");
    }
    try {
        const std::filesystem::path path = path_from_utf8(utf8_path);
        std::error_code status_error;
        const std::filesystem::file_status status =
            std::filesystem::status(path, status_error);
        if (status_error || !std::filesystem::is_regular_file(status)) {
            return fail(error, "OBJ path does not name a readable regular file.");
        }
        const std::uintmax_t native_size = std::filesystem::file_size(path, status_error);
        if (status_error || native_size > limits.maximum_file_bytes) {
            return fail(error, "OBJ exceeds the configured file-size limit.");
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return fail(error, "Could not open OBJ file for reading.");
        }
        if (native_size > std::numeric_limits<std::size_t>::max()) {
            return fail(error, "OBJ is too large for this platform's address space.");
        }
        std::string contents;
        contents.reserve(static_cast<std::size_t>(native_size));
        std::array<char, 64U * 1024U> buffer{};
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = input.gcount();
            if (count > 0) {
                const std::size_t byte_count = static_cast<std::size_t>(count);
                if (byte_count > limits.maximum_file_bytes
                    || contents.size() > limits.maximum_file_bytes - byte_count) {
                    return fail(error, "OBJ grew beyond the file-size limit while reading.");
                }
                contents.append(buffer.data(), byte_count);
            }
        }
        if (!input.eof()) {
            return fail(error, "I/O error while reading OBJ file.");
        }
        return parse_obj_mesh(contents, destination, error, limits);
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to read OBJ; destination was unchanged.");
    } catch (const std::filesystem::filesystem_error& exception) {
        return fail(error, std::string("Filesystem error while reading OBJ: ")
                           + exception.what());
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected error while reading OBJ: ")
                           + exception.what());
    }
}

namespace {

struct ObjFileStamp {
    std::string normalized_path;
    std::uintmax_t size = 0U;
    std::filesystem::file_time_type write_time{};
};

struct ObjMeshCache {
    std::mutex mutex;
    ObjFileStamp stamp;
    ObjLoadLimits limits;
    std::shared_ptr<const ObjMesh> mesh;
};

ObjMeshCache& obj_mesh_cache() {
    static ObjMeshCache cache;
    return cache;
}

bool same_limits(const ObjLoadLimits& first, const ObjLoadLimits& second) {
    return first.maximum_file_bytes == second.maximum_file_bytes
           && first.maximum_line_bytes == second.maximum_line_bytes
           && first.maximum_positions == second.maximum_positions
           && first.maximum_texcoords == second.maximum_texcoords
           && first.maximum_normals == second.maximum_normals
           && first.maximum_triangles == second.maximum_triangles
           && first.maximum_polygon_corners == second.maximum_polygon_corners
           && first.maximum_mesh_bytes == second.maximum_mesh_bytes;
}

bool same_stamp(const ObjFileStamp& first, const ObjFileStamp& second) {
    return first.normalized_path == second.normalized_path
           && first.size == second.size
           && first.write_time == second.write_time;
}

bool inspect_obj_file(const std::string& utf8_path,
                      const ObjLoadLimits& limits,
                      ObjFileStamp& stamp,
                      std::string* error) {
    if (utf8_path.empty() || utf8_path.size() > kMaximumPathBytes
        || utf8_path.find('\0') != std::string::npos) {
        return fail(error, "OBJ path is empty, contains NUL, or exceeds 4095 bytes.");
    }
    std::error_code path_error;
    const std::filesystem::path requested = path_from_utf8(utf8_path);
    const std::filesystem::path absolute =
        std::filesystem::absolute(requested, path_error).lexically_normal();
    if (path_error) {
        return fail(error, "Could not resolve OBJ path to an absolute path.");
    }
    const std::filesystem::file_status status =
        std::filesystem::status(absolute, path_error);
    if (path_error || !std::filesystem::is_regular_file(status)) {
        return fail(error, "OBJ path does not name a readable regular file.");
    }
    const std::uintmax_t size = std::filesystem::file_size(absolute, path_error);
    if (path_error || size > limits.maximum_file_bytes) {
        return fail(error, "OBJ exceeds the configured file-size limit.");
    }
    const std::filesystem::file_time_type write_time =
        std::filesystem::last_write_time(absolute, path_error);
    if (path_error) {
        return fail(error, "Could not inspect OBJ modification time.");
    }
    stamp.normalized_path = path_to_utf8(absolute);
    stamp.size = size;
    stamp.write_time = write_time;
    return true;
}

} // namespace

bool load_obj_mesh_cached(const std::string& utf8_path,
                          std::shared_ptr<const ObjMesh>& destination,
                          std::string* error,
                          const ObjLoadLimits& limits) {
    clear_error(error);
    try {
        for (int attempt = 0; attempt < 2; ++attempt) {
            ObjFileStamp before;
            if (!inspect_obj_file(utf8_path, limits, before, error)) {
                return false;
            }

            ObjMeshCache& cache = obj_mesh_cache();
            {
                std::lock_guard<std::mutex> lock(cache.mutex);
                if (cache.mesh && same_stamp(cache.stamp, before)
                    && same_limits(cache.limits, limits)) {
                    destination = cache.mesh;
                    return true;
                }
                // This is deliberately a one-entry cache. Drop a mismatched
                // strong entry before reading its replacement so a normal
                // cache miss never retains two maximum-sized meshes at once.
                // Meshes already handed to concurrent callers remain alive by
                // shared ownership and belong to those independent renders.
                cache.mesh.reset();
                cache.stamp = {};
                cache.limits = {};
            }

            auto loaded = std::make_shared<ObjMesh>();
            if (!load_obj_mesh(before.normalized_path, *loaded, error, limits)) {
                return false;
            }

            ObjFileStamp after;
            if (!inspect_obj_file(before.normalized_path, limits, after, error)) {
                return false;
            }
            if (!same_stamp(before, after)) {
                if (attempt == 0) {
                    continue;
                }
                return fail(error, "OBJ changed repeatedly while it was being loaded.");
            }

            std::shared_ptr<const ObjMesh> selected = std::move(loaded);
            {
                std::lock_guard<std::mutex> lock(cache.mutex);
                // Another thread may have completed the same parse while this
                // thread was outside the lock. Reuse its immutable instance.
                if (cache.mesh && same_stamp(cache.stamp, after)
                    && same_limits(cache.limits, limits)) {
                    selected = cache.mesh;
                } else {
                    cache.stamp = after;
                    cache.limits = limits;
                    cache.mesh = selected;
                }
            }
            destination = std::move(selected);
            return true;
        }
        return fail(error, "OBJ changed repeatedly while it was being loaded.");
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to cache OBJ; destination was unchanged.");
    } catch (const std::filesystem::filesystem_error& exception) {
        return fail(error, std::string("Filesystem error while caching OBJ: ")
                           + exception.what());
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected error while caching OBJ: ")
                           + exception.what());
    }
}

void clear_obj_mesh_cache() noexcept {
    ObjMeshCache& cache = obj_mesh_cache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.mesh.reset();
    cache.stamp = {};
    cache.limits = {};
}

} // namespace detail
} // namespace pvt
