#include "config_codec.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <map>
#include <new>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace pvt::detail {
namespace {

constexpr std::size_t kMaximumCodecLineBytes = kMaximumUiItems;

static_assert(std::is_nothrow_move_assignable_v<RenderData>);
static_assert(std::is_nothrow_move_assignable_v<CanvasLoopConfig>);
static_assert(std::is_nothrow_move_assignable_v<ExportConfig>);
static_assert(std::is_nothrow_move_assignable_v<MusicAnalysis>);

bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

void clear_error(std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size()
           && text.compare(0U, prefix.size(), prefix) == 0;
}

bool checked_add_within(std::size_t& total, std::size_t addition,
                        std::size_t maximum) {
    if (addition > maximum - total) return false;
    total += addition;
    return true;
}

bool append_bounded_line(std::string& destination, std::string_view line) {
    std::size_t final_size = destination.size();
    if (!checked_add_within(final_size, line.size(), kMaximumSetupBytes)
        || !checked_add_within(final_size, 1U, kMaximumSetupBytes)) {
        return false;
    }
    destination.append(line);
    destination.push_back('\n');
    return true;
}

bool append_bounded_record(std::string& destination, std::string_view key,
                           std::string_view value) {
    std::size_t final_size = destination.size();
    if (!checked_add_within(final_size, key.size(), kMaximumSetupBytes)
        || !checked_add_within(final_size, 1U, kMaximumSetupBytes)
        || !checked_add_within(final_size, value.size(), kMaximumSetupBytes)
        || !checked_add_within(final_size, 1U, kMaximumSetupBytes)) {
        return false;
    }
    destination.append(key);
    destination.push_back('\t');
    destination.append(value);
    destination.push_back('\n');
    return true;
}

bool document_version(const std::string& serialized,
                      std::string_view kind,
                      std::uint32_t& version) {
    const std::size_t newline = serialized.find('\n');
    std::string_view header(serialized.data(),
                            newline == std::string::npos
                                ? serialized.size() : newline);
    if (!header.empty() && header.back() == '\r') header.remove_suffix(1U);
    const std::string prefix = std::string(kind) + "\t";
    if (!starts_with(header, prefix)) return false;
    const std::string_view number = header.substr(prefix.size());
    if (number.empty()) return false;
    std::uint32_t parsed = 0U;
    const auto result = std::from_chars(number.data(),
                                        number.data() + number.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != number.data() + number.size()
        || parsed == 0U) {
        return false;
    }
    version = parsed;
    return true;
}

bool is_render_key(std::string_view key) {
    constexpr std::array<std::string_view, 14U> prefixes{{
        "waves.", "swings.", "effects.", "rhythm.", "appearance.",
        "audio_reactive.", "alpha.", "quantization.", "surface.",
        "palette.", "transform.", "layer_clock.", "motion.",
        "source_image.",
    }};
    for (const std::string_view prefix : prefixes) {
        if (starts_with(key, prefix)) {
            return true;
        }
    }
    return false;
}

bool is_output_key(std::string_view key) {
    return starts_with(key, "canvas.") || starts_with(key, "timing.")
           || starts_with(key, "output.") || starts_with(key, "paths.")
           || starts_with(key, "audio_response_defaults.");
}

bool is_setup_v5_key(std::string_view key) {
    return starts_with(key, "timing.clock.")
           || starts_with(key, "timing.music.")
           || key == "rhythm.swings_enabled"
           || starts_with(key, "audio_reactive.")
           || key == "surface.obj_sha256"
           || key == "surface.obj_basename";
}

bool has_suffix(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size()
           && text.compare(text.size() - suffix.size(), suffix.size(), suffix)
                  == 0;
}

bool is_setup_v2_key(std::string_view key) {
    return key == "surface.obj_path"
           || key == "output.png_compression_level";
}

bool is_setup_v3_key(std::string_view key) {
    return key == "output.write_alpha";
}

bool is_setup_v4_key(std::string_view key) {
    return starts_with(key, "palette.") || starts_with(key, "transform.")
           || (starts_with(key, "swings.")
               && (has_suffix(key, ".center_x")
                   || has_suffix(key, ".center_y")
                   || has_suffix(key, ".radius")))
           || (starts_with(key, "effects.")
               && (has_suffix(key, ".space")
                   || has_suffix(key, ".area_radius")));
}

bool is_setup_v6_key(std::string_view key) {
    return key == "timing.clock.data_only"
           || starts_with(key, "layer_clock.")
           || starts_with(key, "motion.");
}

bool is_setup_v7_key(std::string_view key) {
    return starts_with(key, "source_image.")
           || starts_with(key, "paths.")
           || key == "motion.rotation_offset_degrees"
           || starts_with(key, "motion.custom_path.")
           || ((starts_with(key, "waves.")
                || starts_with(key, "effects."))
               && key.find(".path.") != std::string_view::npos);
}

bool is_setup_v8_key(std::string_view key) {
    return starts_with(key, "audio_response_defaults.")
           || key == "audio_reactive.override_enabled"
           || ((starts_with(key, "waves.")
                || starts_with(key, "effects."))
               && has_suffix(key, ".audio_response"));
}

bool supported_layer_version(const std::string& serialized,
                             std::uint32_t& layer_version,
                             std::uint32_t& setup_version) {
    if (!document_version(serialized, "PVT_LAYER", layer_version)) return false;
    setup_version = layer_version == 1U ? 3U
                    : layer_version == 2U ? 4U
                    : layer_version == 3U ? 5U
                    : layer_version == 4U ? 6U
                    : layer_version == 5U ? 7U
                    : layer_version == 6U ? 8U : 9U;
    return true;
}

bool supported_render_output_version(const std::string& serialized,
                                     std::uint32_t& output_version,
                                     std::uint32_t& setup_version) {
    if (!document_version(serialized, "PVT_RENDER_OUTPUT", output_version)) {
        return false;
    }
    setup_version = output_version == 1U ? 4U
                    : output_version == 2U ? 5U
                    : output_version == 3U ? 6U
                    : output_version == 4U ? 7U : 8U;
    return true;
}

bool valid_ascii_record(std::string_view line, std::string_view& key) {
    if (line.empty() || line.size() > kMaximumCodecLineBytes) {
        return false;
    }
    const std::size_t tab = line.find('\t');
    if (tab == std::string_view::npos
        || line.find('\t', tab + 1U) != std::string_view::npos) {
        return false;
    }
    key = line.substr(0U, tab);
    if (key.empty() || key.size() > kMaximumUiItems) {
        return false;
    }
    for (const char raw : line) {
        const unsigned char value = static_cast<unsigned char>(raw);
        if (value == '\t') {
            continue;
        }
        if (value < 0x20U || value > 0x7eU) {
            return false;
        }
    }
    return true;
}

bool recovery_percent_encode(std::string_view decoded, std::string& encoded) {
    constexpr char hexadecimal[] = "0123456789ABCDEF";
    std::size_t encoded_size = 0U;
    for (const char raw : decoded) {
        const unsigned char value = static_cast<unsigned char>(raw);
        const bool unreserved = (value >= 'A' && value <= 'Z')
                                || (value >= 'a' && value <= 'z')
                                || (value >= '0' && value <= '9')
                                || value == '-' || value == '.'
                                || value == '_' || value == '~';
        if (!checked_add_within(encoded_size, unreserved ? 1U : 3U,
                                kMaximumSetupBytes)) {
            return false;
        }
    }
    encoded.clear();
    encoded.reserve(encoded_size);
    for (const char raw : decoded) {
        const unsigned char value = static_cast<unsigned char>(raw);
        const bool unreserved = (value >= 'A' && value <= 'Z')
                                || (value >= 'a' && value <= 'z')
                                || (value >= '0' && value <= '9')
                                || value == '-' || value == '.'
                                || value == '_' || value == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(value));
        } else {
            encoded.push_back('%');
            encoded.push_back(hexadecimal[value >> 4U]);
            encoded.push_back(hexadecimal[value & 0x0fU]);
        }
    }
    return true;
}

bool split_document(const std::string& serialized,
                    std::string_view expected_header,
                    std::vector<std::string_view>& records,
                    std::string* error) {
    if (serialized.empty()) {
        return fail(error, "Configuration data is empty.");
    }
    if (serialized.size() > kMaximumSetupBytes) {
        return fail(error, "Configuration data exceeds the signed-int format limit.");
    }

    std::size_t start = 0U;
    std::size_t line_number = 0U;
    while (start < serialized.size()) {
        const std::size_t newline = serialized.find('\n', start);
        const std::size_t end = newline == std::string::npos
                                    ? serialized.size() : newline;
        std::string_view line(serialized.data() + start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1U);
        }
        ++line_number;
        if (line_number == 1U) {
            if (line != expected_header) {
                return fail(error, "Unsupported or malformed configuration header.");
            }
        } else {
            std::string_view key;
            if (!valid_ascii_record(line, key)) {
                return fail(error, "Configuration line "
                                       + std::to_string(line_number)
                                       + " is malformed.");
            }
            records.push_back(line);
        }
        if (newline == std::string::npos) {
            break;
        }
        start = newline + 1U;
    }
    return true;
}

bool filter_setup(const std::string& setup,
                  bool keep_render,
                  std::string_view replacement_header,
                  std::string& destination,
                  std::string* error) {
    const std::string setup_header =
        "PVT_SETUP\t" + std::to_string(kSetupFormatVersion);
    std::vector<std::string_view> records;
    if (!split_document(setup, setup_header, records, error)) {
        return false;
    }
    destination.clear();
    destination.reserve(setup.size());
    destination.append(replacement_header);
    destination.push_back('\n');
    for (const std::string_view line : records) {
        const std::string_view key = line.substr(0U, line.find('\t'));
        const bool render = is_render_key(key);
        const bool output = is_output_key(key);
        if (!render && !output) {
            return fail(error, "The legacy setup codec produced an unclassified key.");
        }
        if (render == keep_render) {
            if (!append_bounded_line(destination, line)) {
                return fail(error, "Filtered configuration exceeds the signed-int format limit.");
            }
        }
    }
    return true;
}

bool synthesize_setup(const std::string& partial,
                      std::string_view expected_header,
                      bool partial_is_render,
                      std::uint32_t setup_version,
                      std::string& destination,
                      std::string* error,
                      const std::vector<CubicMotionPath>* motion_paths = nullptr,
                      ConfigCompatibility* partial_compatibility = nullptr) {
    std::vector<std::string_view> partial_records;
    if (!split_document(partial, expected_header, partial_records, error)) {
        return false;
    }
    std::vector<std::string_view> usable_records;
    usable_records.reserve(partial_records.size());
    for (const std::string_view line : partial_records) {
        const std::string_view key = line.substr(0U, line.find('\t'));
        if (starts_with(key, "compatibility.rejected.")) {
            usable_records.push_back(line);
            continue;
        }
        if ((partial_is_render && !is_render_key(key))
            || (!partial_is_render && !is_output_key(key))) {
            if (partial_compatibility != nullptr) {
                partial_compatibility->records.push_back(
                    {std::string(key),
                     std::string(line.substr(key.size() + 1U)), false});
                partial_compatibility->repair_notes.push_back(
                    "Kept field '" + std::string(key)
                    + "' from the wrong data block without using it.");
                continue;
            }
            return fail(error,
                        "Configuration contains a key in the wrong data block: '"
                            + std::string(key) + "'.");
        }
        usable_records.push_back(line);
    }

    ProjectConfig defaults = default_project();
    if (motion_paths != nullptr) {
        defaults.canvas.motion_paths = *motion_paths;
    }
    if (partial_is_render) {
        defaults.output.write_alpha = true;
    }
    const RenderData default_render = defaults.layers.empty()
                                          ? RenderData{} : defaults.layers.front().render;
    const RenderConfig full_default =
        apply_global_config(defaults.canvas, defaults.output, default_render);
    std::string default_setup;
    if (!serialize_setup_config(full_default, default_setup, error)) {
        return false;
    }

    std::vector<std::string_view> default_records;
    const std::string setup_header =
        "PVT_SETUP\t" + std::to_string(setup_version);
    const std::string current_setup_header =
        "PVT_SETUP\t" + std::to_string(kSetupFormatVersion);
    if (!split_document(default_setup, current_setup_header,
                        default_records, error)) {
        return false;
    }

    destination.clear();
    destination.reserve(default_setup.size());
    destination.append(setup_header);
    destination.push_back('\n');
    for (const std::string_view line : default_records) {
        const std::string_view key = line.substr(0U, line.find('\t'));
        if (setup_version < 2U && is_setup_v2_key(key)) {
            continue;
        }
        if (setup_version < 3U && is_setup_v3_key(key)) {
            continue;
        }
        if (setup_version < 4U && is_setup_v4_key(key)) {
            continue;
        }
        if (setup_version < 5U && is_setup_v5_key(key)) {
            continue;
        }
        if (setup_version < 6U && is_setup_v6_key(key)) {
            continue;
        }
        if (setup_version < 7U && is_setup_v7_key(key)) {
            continue;
        }
        if (setup_version < 8U && is_setup_v8_key(key)) {
            continue;
        }
        if (is_render_key(key) != partial_is_render) {
            destination.append(line);
            destination.push_back('\n');
        }
    }
    for (const std::string_view line : usable_records) {
        if (!append_bounded_line(destination, line)) {
            return fail(error, "Combined configuration exceeds the signed-int format limit.");
        }
    }
    return true;
}

} // namespace

bool append_config_compatibility(
    std::string& serialized,
    const ConfigCompatibility* first,
    const ConfigCompatibility* second,
    std::string* error) {
    std::map<std::string, std::string> existing;
    const std::size_t first_newline = serialized.find('\n');
    if (first_newline == std::string::npos) {
        return fail(error, "Cannot preserve compatibility data in a malformed configuration.");
    }
    std::size_t start = first_newline + 1U;
    while (start < serialized.size()) {
        const std::size_t newline = serialized.find('\n', start);
        const std::size_t end = newline == std::string::npos
                                    ? serialized.size() : newline;
        std::string_view line(serialized.data() + start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1U);
        std::string_view key;
        if (!valid_ascii_record(line, key)) {
            return fail(error, "Cannot preserve compatibility data in a malformed configuration.");
        }
        existing.emplace(std::string(key),
                         std::string(line.substr(key.size() + 1U)));
        if (newline == std::string::npos) break;
        start = newline + 1U;
    }

    std::map<std::string, std::string> unknown;
    std::vector<PreservedConfigRecord> rejected;
    const auto collect = [&](const ConfigCompatibility* compatibility) -> bool {
        if (compatibility == nullptr) return true;
        for (const PreservedConfigRecord& record : compatibility->records) {
            const std::string line = record.key + "\t" + record.value;
            std::string_view checked_key;
            const bool safe = valid_ascii_record(line, checked_key);
            if (!safe) {
                return fail(error,
                            "Cannot save malformed preserved field '"
                                + record.key + "'; no preserved data was discarded.");
            }
            const bool reserved = starts_with(record.key,
                                               "compatibility.rejected.");
            if (!record.rejected && safe && !reserved
                && existing.find(record.key) == existing.end()) {
                const auto inserted = unknown.emplace(record.key, record.value);
                if (!inserted.second
                    && inserted.first->second != record.value) {
                    rejected.push_back(record);
                }
            } else {
                rejected.push_back(record);
            }
        }
        return true;
    };
    if (!collect(first) || !collect(second)) return false;
    std::sort(rejected.begin(), rejected.end(),
              [](const PreservedConfigRecord& left,
                 const PreservedConfigRecord& right) {
                  return std::tie(left.key, left.value)
                         < std::tie(right.key, right.value);
              });
    rejected.erase(std::unique(
                       rejected.begin(), rejected.end(),
                       [](const PreservedConfigRecord& left,
                          const PreservedConfigRecord& right) {
                           return left.key == right.key
                                  && left.value == right.value;
                       }),
                   rejected.end());

    const auto append_line = [&](std::string_view key,
                                 std::string_view value) -> bool {
        if (!append_bounded_record(serialized, key, value)) {
            return fail(error,
                        "Preserved compatibility data exceeds the signed-int configuration limit.");
        }
        return true;
    };
    for (const auto& record : unknown) {
        if (!append_line(record.first, record.second)) return false;
    }
    if (!rejected.empty()) {
        if (!append_line("compatibility.rejected.count",
                         std::to_string(rejected.size()))) return false;
        for (std::size_t index = 0U; index < rejected.size(); ++index) {
            std::string encoded_key;
            std::string encoded_value;
            if (!recovery_percent_encode(rejected[index].key, encoded_key)
                || !recovery_percent_encode(rejected[index].value,
                                            encoded_value)
                || !append_line("compatibility.rejected."
                                    + std::to_string(index) + ".key",
                                encoded_key)
                || !append_line("compatibility.rejected."
                                    + std::to_string(index) + ".value",
                                encoded_value)) {
                return false;
            }
        }
    }
    return true;
}

bool serialize_layer_config(const RenderData& render,
                            std::string& serialized,
                            std::string* error,
                            const std::vector<CubicMotionPath>* motion_paths) {
    clear_error(error);
    try {
        ProjectConfig project = default_project();
        if (project.layers.empty()) {
            return fail(error, "The default project does not contain a layer.");
        }
        project.layers.front().render = render;
        if (motion_paths != nullptr) {
            project.canvas.motion_paths = *motion_paths;
        }
        // An extracted cache path is a runtime implementation detail. Once an
        // OBJ has a content identity, only its digest and display basename are
        // portable project semantics.
        if (!project.layers.front().render.surface.obj_sha256.empty()) {
            project.layers.front().render.surface.obj_path.clear();
        }
        if (!project.layers.front().render.starting_image.sha256.empty()) {
            project.layers.front().render.starting_image.path.clear();
        }
        // Layer validity must not depend on an arbitrary final RGB/RGBA choice.
        // The project-global output codec validates that choice separately.
        project.output.write_alpha = true;
        const ValidationResult validation = validate(project);
        if (!validation.ok) {
            return fail(error, "Cannot save invalid layer: " + validation.message);
        }
        const RenderConfig config =
            apply_global_config(project.canvas, project.output,
                                project.layers.front().render);
        std::string setup;
        if (!serialize_setup_config(config, setup, error)) {
            return false;
        }
        if (!filter_setup(setup, true,
                          "PVT_LAYER\t" + std::to_string(kLayerConfigFormatVersion),
                          serialized, error)) {
            return false;
        }
        return append_config_compatibility(
            serialized, &render.source_compatibility, nullptr, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to serialize layer configuration.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected layer serialization error: ")
                               + exception.what());
    }
}

bool deserialize_layer_config(const std::string& serialized,
                              RenderData& destination,
                              std::string* error,
                              const std::vector<CubicMotionPath>* motion_paths) {
    clear_error(error);
    try {
        std::uint32_t layer_version = 0U;
        std::uint32_t setup_version = 0U;
        if (!supported_layer_version(serialized, layer_version, setup_version)) {
            return fail(error, "Unsupported or malformed layer configuration header.");
        }
        ConfigCompatibility partial_compatibility;
        std::string setup;
        if (!synthesize_setup(serialized,
                              "PVT_LAYER\t" + std::to_string(layer_version),
                              true, setup_version, setup, error,
                              motion_paths, &partial_compatibility)) {
            return false;
        }
        RenderConfig loaded;
        if (!deserialize_setup_config(setup, loaded, error)) {
            return false;
        }
        for (const PreservedConfigRecord& record :
             loaded.output_compatibility.records) {
            loaded.source_compatibility.records.push_back(record);
        }
        loaded.source_compatibility.repair_notes.insert(
            loaded.source_compatibility.repair_notes.end(),
            loaded.output_compatibility.repair_notes.begin(),
            loaded.output_compatibility.repair_notes.end());
        loaded.source_compatibility.records.insert(
            loaded.source_compatibility.records.end(),
            partial_compatibility.records.begin(),
            partial_compatibility.records.end());
        loaded.source_compatibility.repair_notes.insert(
            loaded.source_compatibility.repair_notes.end(),
            partial_compatibility.repair_notes.begin(),
            partial_compatibility.repair_notes.end());
        RenderData candidate = loaded;
        destination = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error,
                    "Not enough memory to load layer; destination was not changed.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected layer load error: ")
                               + exception.what());
    }
}

bool serialize_render_output_config(const CanvasLoopConfig& canvas,
                                    const ExportConfig& output,
                                    std::string& serialized,
                                    std::string* error) {
    clear_error(error);
    try {
        const ProjectConfig defaults = default_project();
        const RenderData render = defaults.layers.empty()
                                      ? RenderData{} : defaults.layers.front().render;
        const RenderConfig config = apply_global_config(canvas, output, render);
        std::string setup;
        if (!serialize_setup_config(config, setup, error)) {
            return false;
        }
        if (!filter_setup(
                setup, false,
                "PVT_RENDER_OUTPUT\t"
                    + std::to_string(kRenderOutputConfigFormatVersion),
                serialized, error)) {
            return false;
        }
        return append_config_compatibility(
            serialized, &canvas.output_compatibility,
            &canvas.clock.music.compatibility, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to serialize render/output data.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected render/output serialization error: ")
                               + exception.what());
    }
}

bool deserialize_render_output_config(const std::string& serialized,
                                      CanvasLoopConfig& canvas,
                                      ExportConfig& output,
                                      std::string* error) {
    clear_error(error);
    try {
        std::uint32_t output_version = 0U;
        std::uint32_t setup_version = 0U;
        if (!supported_render_output_version(serialized, output_version,
                                             setup_version)) {
            return fail(error,
                        "Unsupported or malformed render/output configuration header.");
        }
        ConfigCompatibility partial_compatibility;
        std::string setup;
        if (!synthesize_setup(
                serialized,
                "PVT_RENDER_OUTPUT\t" + std::to_string(output_version),
                false, setup_version, setup, error, nullptr,
                &partial_compatibility)) {
            return false;
        }
        RenderConfig loaded;
        if (!deserialize_setup_config(setup, loaded, error)) {
            return false;
        }
        CanvasLoopConfig candidate_canvas;
        candidate_canvas.width = loaded.width;
        candidate_canvas.height = loaded.height;
        candidate_canvas.block_size = loaded.block_size;
        candidate_canvas.total_frames = loaded.total_frames;
        candidate_canvas.fps = loaded.fps;
        candidate_canvas.clock = std::move(loaded.clock);
        candidate_canvas.audio_reactive_defaults =
            loaded.audio_reactive_defaults;
        candidate_canvas.motion_paths = std::move(loaded.motion_paths);
        candidate_canvas.output_compatibility =
            std::move(loaded.output_compatibility);
        candidate_canvas.output_compatibility.records.insert(
            candidate_canvas.output_compatibility.records.end(),
            partial_compatibility.records.begin(),
            partial_compatibility.records.end());
        candidate_canvas.output_compatibility.repair_notes.insert(
            candidate_canvas.output_compatibility.repair_notes.end(),
            partial_compatibility.repair_notes.begin(),
            partial_compatibility.repair_notes.end());
        ExportConfig candidate_output = loaded.output;
        canvas = std::move(candidate_canvas);
        output = std::move(candidate_output);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error,
                    "Not enough memory to load render/output data; destination was not changed.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected render/output load error: ")
                               + exception.what());
    }
}

bool serialize_music_analysis_config(const MusicAnalysis& analysis,
                                     std::string& serialized,
                                     std::string* error) {
    clear_error(error);
    try {
        ProjectConfig project = default_project();
        if (project.layers.empty()) {
            return fail(error, "The default project does not contain a layer.");
        }
        project.canvas.clock.music = analysis;
        const RenderConfig config = apply_global_config(
            project.canvas, project.output, project.layers.front().render);
        std::string setup;
        if (!serialize_setup_config(config, setup, error)) return false;

        const std::string setup_header =
            "PVT_SETUP\t" + std::to_string(kSetupFormatVersion);
        std::vector<std::string_view> records;
        if (!split_document(setup, setup_header, records, error)) return false;
        serialized.clear();
        serialized.reserve(setup.size());
        serialized.append("PVT_MUSIC_ANALYSIS\t");
        serialized.append(std::to_string(kMusicAnalysisConfigFormatVersion));
        serialized.push_back('\n');
        for (const std::string_view line : records) {
            const std::string_view key = line.substr(0U, line.find('\t'));
            if (!starts_with(key, "timing.music.")) continue;
            if (!append_bounded_line(serialized, line)) {
                return fail(error, "Music analysis exceeds the signed-int format limit.");
            }
        }
        return append_config_compatibility(
            serialized, &analysis.compatibility, nullptr, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to serialize music analysis.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected music-analysis serialization error: ")
                               + exception.what());
    }
}

bool deserialize_music_analysis_config(const std::string& serialized,
                                       MusicAnalysis& analysis,
                                       std::string* error) {
    clear_error(error);
    try {
        std::uint32_t analysis_version = 0U;
        if (!document_version(serialized, "PVT_MUSIC_ANALYSIS",
                              analysis_version)) {
            return fail(error,
                        "Unsupported or malformed music-analysis header.");
        }
        const std::string analysis_header =
            "PVT_MUSIC_ANALYSIS\t" + std::to_string(analysis_version);
        std::vector<std::string_view> analysis_records;
        if (!split_document(serialized, analysis_header,
                            analysis_records, error)) return false;
        ConfigCompatibility partial_compatibility;
        std::vector<std::string_view> usable_analysis_records;
        usable_analysis_records.reserve(analysis_records.size());
        for (const std::string_view line : analysis_records) {
            const std::string_view key = line.substr(0U, line.find('\t'));
            if (starts_with(key, "timing.music.")
                || starts_with(key, "compatibility.rejected.")) {
                usable_analysis_records.push_back(line);
            } else {
                partial_compatibility.records.push_back(
                    {std::string(key),
                     std::string(line.substr(key.size() + 1U)), false});
                partial_compatibility.repair_notes.push_back(
                    "Kept unrelated music-analysis field '"
                    + std::string(key) + "' without using it.");
            }
        }

        ProjectConfig defaults = default_project();
        if (defaults.layers.empty()) {
            return fail(error, "The default project does not contain a layer.");
        }
        const RenderConfig default_config = apply_global_config(
            defaults.canvas, defaults.output, defaults.layers.front().render);
        std::string default_setup;
        if (!serialize_setup_config(default_config, default_setup, error)) {
            return false;
        }
        const std::string setup_header =
            "PVT_SETUP\t" + std::to_string(kSetupFormatVersion);
        std::vector<std::string_view> default_records;
        if (!split_document(default_setup, setup_header,
                            default_records, error)) return false;

        std::string combined;
        combined.reserve(default_setup.size());
        combined.append(setup_header);
        combined.push_back('\n');
        for (const std::string_view line : default_records) {
            const std::string_view key = line.substr(0U, line.find('\t'));
            if (starts_with(key, "timing.music.")) continue;
            combined.append(line);
            combined.push_back('\n');
        }
        for (const std::string_view line : usable_analysis_records) {
            if (!append_bounded_line(combined, line)) {
                return fail(error, "Music analysis exceeds the signed-int format limit.");
            }
        }
        RenderConfig loaded;
        if (!deserialize_setup_config(combined, loaded, error)) return false;
        MusicAnalysis candidate = std::move(loaded.clock.music);
        candidate.compatibility.records.insert(
            candidate.compatibility.records.end(),
            partial_compatibility.records.begin(),
            partial_compatibility.records.end());
        candidate.compatibility.repair_notes.insert(
            candidate.compatibility.repair_notes.end(),
            partial_compatibility.repair_notes.begin(),
            partial_compatibility.repair_notes.end());
        analysis = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error,
                    "Not enough memory to load music analysis; destination was not changed.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected music-analysis load error: ")
                               + exception.what());
    }
}

bool serialize_split_render_output_config(const CanvasLoopConfig& canvas,
                                          const ExportConfig& output,
                                          std::string& serialized,
                                          std::string* error) {
    clear_error(error);
    try {
        const ProjectConfig defaults = default_project();
        const RenderData render = defaults.layers.empty()
                                      ? RenderData{} : defaults.layers.front().render;
        const RenderConfig config = apply_global_config(canvas, output, render);
        std::string setup;
        if (!serialize_setup_config(config, setup, error)) return false;
        const std::string setup_header =
            "PVT_SETUP\t" + std::to_string(kSetupFormatVersion);
        std::vector<std::string_view> records;
        if (!split_document(setup, setup_header, records, error)) return false;
        serialized.clear();
        serialized.reserve(setup.size());
        serialized.append("PVT_RENDER_OUTPUT_SPLIT\t");
        serialized.append(std::to_string(kSplitRenderOutputConfigFormatVersion));
        serialized.push_back('\n');
        for (const std::string_view line : records) {
            const std::string_view key = line.substr(0U, line.find('\t'));
            if (!is_output_key(key) || starts_with(key, "timing.music.")) {
                continue;
            }
            if (!append_bounded_line(serialized, line)) {
                return fail(error, "Split render/output data exceeds the signed-int format limit.");
            }
        }
        return append_config_compatibility(
            serialized, &canvas.output_compatibility, nullptr, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to split render/output data.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected split render/output error: ")
                               + exception.what());
    }
}

bool deserialize_split_render_output_config(
    const std::string& serialized,
    const std::string& music_analysis,
    CanvasLoopConfig& canvas,
    ExportConfig& output,
    std::string* error) {
    clear_error(error);
    try {
        std::uint32_t split_version = 0U;
        if (!document_version(serialized, "PVT_RENDER_OUTPUT_SPLIT",
                              split_version)) {
            return fail(error,
                        "Unsupported or malformed split render/output header.");
        }
        const std::string split_header =
            "PVT_RENDER_OUTPUT_SPLIT\t" + std::to_string(split_version);
        std::vector<std::string_view> split_records;
        if (!split_document(serialized, split_header,
                            split_records, error)) return false;

        ConfigCompatibility split_compatibility;
        std::vector<std::string_view> usable_split_records;
        usable_split_records.reserve(split_records.size());
        for (const std::string_view line : split_records) {
            const std::string_view key = line.substr(0U, line.find('\t'));
            if (starts_with(key, "timing.music.")) {
                split_compatibility.records.push_back(
                    {std::string(key),
                     std::string(line.substr(key.size() + 1U)), false});
                split_compatibility.repair_notes.push_back(
                    "Kept music-analysis field '" + std::string(key)
                    + "' from the render/output block without using it.");
            } else {
                usable_split_records.push_back(line);
            }
        }

        MusicAnalysis candidate_analysis;
        if (!deserialize_music_analysis_config(
                music_analysis, candidate_analysis, error)) {
            return false;
        }
        // The combined decoder validates a Music clock before the complete
        // analysis is installed below. Give it the real source metadata and
        // one beat, but do not serialize and parse the enormous feature table
        // a second time.
        MusicAnalysis validation_analysis = candidate_analysis;
        validation_analysis.compatibility = {};
        validation_analysis.feature_samples.clear();
        validation_analysis.tempo_points.clear();
        if (validation_analysis.beat_times_seconds.size() > 1U) {
            validation_analysis.beat_times_seconds.resize(1U);
        }
        std::string validation_analysis_bytes;
        if (!serialize_music_analysis_config(
                validation_analysis, validation_analysis_bytes, error)) {
            return false;
        }
        const std::string analysis_header =
            "PVT_MUSIC_ANALYSIS\t"
            + std::to_string(kMusicAnalysisConfigFormatVersion);
        std::vector<std::string_view> validation_analysis_records;
        if (!split_document(validation_analysis_bytes, analysis_header,
                            validation_analysis_records, error)) {
            return false;
        }

        std::string combined;
        combined.reserve(serialized.size());
        combined.append("PVT_RENDER_OUTPUT\t");
        // Split v1 predates reusable motion paths and corresponds to regular
        // render/output v3 (setup v6). Treating it as today's current schema
        // was the regression that made historical saves demand paths.count.
        combined.append(std::to_string(
            split_version == 1U ? 3U
            : split_version == 2U ? 4U
                                  : kRenderOutputConfigFormatVersion));
        combined.push_back('\n');
        for (const std::string_view line : usable_split_records) {
            if (!append_bounded_line(combined, line)) {
                return fail(error,
                            "Combined render/output data exceeds the signed-int format limit.");
            }
        }
        for (const std::string_view line : validation_analysis_records) {
            if (!append_bounded_line(combined, line)) {
                return fail(error,
                            "Combined render/output data exceeds the signed-int format limit.");
            }
        }
        CanvasLoopConfig candidate_canvas;
        ExportConfig candidate_output;
        if (!deserialize_render_output_config(
                combined, candidate_canvas, candidate_output, error)) {
            return false;
        }
        candidate_canvas.output_compatibility.records.insert(
            candidate_canvas.output_compatibility.records.end(),
            split_compatibility.records.begin(),
            split_compatibility.records.end());
        candidate_canvas.output_compatibility.repair_notes.insert(
            candidate_canvas.output_compatibility.repair_notes.end(),
            split_compatibility.repair_notes.begin(),
            split_compatibility.repair_notes.end());
        candidate_canvas.clock.music = std::move(candidate_analysis);
        canvas = std::move(candidate_canvas);
        output = std::move(candidate_output);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error,
                    "Not enough memory to combine render/output and music analysis.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected split render/output load error: ")
                               + exception.what());
    }
}

} // namespace pvt::detail
