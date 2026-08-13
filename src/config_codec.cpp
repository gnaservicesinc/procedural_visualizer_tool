#include "config_codec.h"

#include <array>
#include <cstddef>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace pvt::detail {
namespace {

constexpr std::size_t kMaximumCodecLineBytes = 256U * 1024U;

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
           || starts_with(key, "output.") || starts_with(key, "paths.");
}

bool is_setup_v5_key(std::string_view key) {
    return starts_with(key, "timing.clock.")
           || starts_with(key, "timing.music.")
           || key == "rhythm.swings_enabled"
           || starts_with(key, "audio_reactive.")
           || key == "surface.obj_sha256"
           || key == "surface.obj_basename";
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

bool supported_layer_version(const std::string& serialized,
                             std::uint32_t& layer_version,
                             std::uint32_t& setup_version) {
    for (std::uint32_t version = 1U;
         version <= kLayerConfigFormatVersion; ++version) {
        const std::string header = "PVT_LAYER\t" + std::to_string(version);
        if (starts_with(serialized, header + "\n")
            || starts_with(serialized, header + "\r\n")) {
            layer_version = version;
            setup_version = version == 1U ? 3U
                            : version == 2U ? 4U
                            : version == 3U ? 5U
                            : version == 4U ? 6U : 7U;
            return true;
        }
    }
    return false;
}

bool supported_render_output_version(const std::string& serialized,
                                     std::uint32_t& output_version,
                                     std::uint32_t& setup_version) {
    for (std::uint32_t version = 1U;
         version <= kRenderOutputConfigFormatVersion; ++version) {
        const std::string header =
            "PVT_RENDER_OUTPUT\t" + std::to_string(version);
        if (starts_with(serialized, header + "\n")
            || starts_with(serialized, header + "\r\n")) {
            output_version = version;
            setup_version = version == 1U ? 4U
                            : version == 2U ? 5U
                            : version == 3U ? 6U : 7U;
            return true;
        }
    }
    return false;
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
    if (key.empty() || key.size() > 128U) {
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

bool split_document(const std::string& serialized,
                    std::string_view expected_header,
                    std::vector<std::string_view>& records,
                    std::string* error) {
    if (serialized.empty()) {
        return fail(error, "Configuration data is empty.");
    }
    if (serialized.size() > kMaximumSetupBytes) {
        return fail(error, "Configuration data exceeds the 8 MiB limit.");
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
            if (destination.size() > kMaximumSetupBytes - line.size() - 1U) {
                return fail(error, "Filtered configuration exceeds the 8 MiB limit.");
            }
            destination.append(line);
            destination.push_back('\n');
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
                      const std::vector<CubicMotionPath>* motion_paths = nullptr) {
    std::vector<std::string_view> partial_records;
    if (!split_document(partial, expected_header, partial_records, error)) {
        return false;
    }
    for (const std::string_view line : partial_records) {
        const std::string_view key = line.substr(0U, line.find('\t'));
        if ((partial_is_render && !is_render_key(key))
            || (!partial_is_render && !is_output_key(key))) {
            return fail(error, "Configuration contains a key in the wrong data block: '"
                                   + std::string(key) + "'.");
        }
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
    destination.reserve(default_setup.size() + partial.size());
    destination.append(setup_header);
    destination.push_back('\n');
    for (const std::string_view line : default_records) {
        const std::string_view key = line.substr(0U, line.find('\t'));
        if (setup_version < 5U && is_setup_v5_key(key)) {
            continue;
        }
        if (setup_version < 6U && is_setup_v6_key(key)) {
            continue;
        }
        if (setup_version < 7U && is_setup_v7_key(key)) {
            continue;
        }
        if (is_render_key(key) != partial_is_render) {
            destination.append(line);
            destination.push_back('\n');
        }
    }
    for (const std::string_view line : partial_records) {
        if (destination.size() > kMaximumSetupBytes - line.size() - 1U) {
            return fail(error, "Combined configuration exceeds the 8 MiB limit.");
        }
        destination.append(line);
        destination.push_back('\n');
    }
    return true;
}

} // namespace

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
        return filter_setup(setup, true,
                            "PVT_LAYER\t" + std::to_string(kLayerConfigFormatVersion),
                            serialized, error);
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
        std::string setup;
        if (!synthesize_setup(serialized,
                              "PVT_LAYER\t" + std::to_string(layer_version),
                              true, setup_version, setup, error,
                              motion_paths)) {
            return false;
        }
        RenderConfig loaded;
        if (!deserialize_setup_config(setup, loaded, error)) {
            return false;
        }
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
        return filter_setup(
            setup, false,
            "PVT_RENDER_OUTPUT\t"
                + std::to_string(kRenderOutputConfigFormatVersion),
            serialized, error);
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
        std::string setup;
        if (!synthesize_setup(
                serialized,
                "PVT_RENDER_OUTPUT\t" + std::to_string(output_version),
                false, setup_version, setup, error)) {
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
        candidate_canvas.motion_paths = std::move(loaded.motion_paths);
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
            if (serialized.size() > kMaximumSetupBytes - line.size() - 1U) {
                return fail(error, "Music analysis exceeds the 8 MiB limit.");
            }
            serialized.append(line);
            serialized.push_back('\n');
        }
        return true;
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
        const std::string analysis_header =
            "PVT_MUSIC_ANALYSIS\t"
            + std::to_string(kMusicAnalysisConfigFormatVersion);
        std::vector<std::string_view> analysis_records;
        if (!split_document(serialized, analysis_header,
                            analysis_records, error)) return false;
        for (const std::string_view line : analysis_records) {
            const std::string_view key = line.substr(0U, line.find('\t'));
            if (!starts_with(key, "timing.music.")) {
                return fail(error,
                            "Music-analysis data contains an unrelated field.");
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
        combined.reserve(default_setup.size() + serialized.size());
        combined.append(setup_header);
        combined.push_back('\n');
        for (const std::string_view line : default_records) {
            const std::string_view key = line.substr(0U, line.find('\t'));
            if (starts_with(key, "timing.music.")) continue;
            combined.append(line);
            combined.push_back('\n');
        }
        for (const std::string_view line : analysis_records) {
            if (combined.size() > kMaximumSetupBytes - line.size() - 1U) {
                return fail(error, "Music analysis exceeds the 8 MiB limit.");
            }
            combined.append(line);
            combined.push_back('\n');
        }
        RenderConfig loaded;
        if (!deserialize_setup_config(combined, loaded, error)) return false;
        MusicAnalysis candidate = std::move(loaded.clock.music);
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
            if (serialized.size() > kMaximumSetupBytes - line.size() - 1U) {
                return fail(error, "Split render/output data exceeds the 8 MiB limit.");
            }
            serialized.append(line);
            serialized.push_back('\n');
        }
        return true;
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
        const std::string split_header =
            "PVT_RENDER_OUTPUT_SPLIT\t"
            + std::to_string(kSplitRenderOutputConfigFormatVersion);
        std::vector<std::string_view> split_records;
        if (!split_document(serialized, split_header,
                            split_records, error)) return false;
        for (const std::string_view line : split_records) {
            const std::string_view key = line.substr(0U, line.find('\t'));
            if (!is_output_key(key) || starts_with(key, "timing.music.")) {
                return fail(error,
                            "Split render/output data contains a field in the wrong block.");
            }
        }

        const std::string analysis_header =
            "PVT_MUSIC_ANALYSIS\t"
            + std::to_string(kMusicAnalysisConfigFormatVersion);
        std::vector<std::string_view> analysis_records;
        if (!split_document(music_analysis, analysis_header,
                            analysis_records, error)) return false;
        for (const std::string_view line : analysis_records) {
            const std::string_view key = line.substr(0U, line.find('\t'));
            if (!starts_with(key, "timing.music.")) {
                return fail(error,
                            "Music-analysis data contains an unrelated field.");
            }
        }

        std::string combined;
        combined.reserve(serialized.size() + music_analysis.size());
        combined.append("PVT_RENDER_OUTPUT\t");
        combined.append(std::to_string(kRenderOutputConfigFormatVersion));
        combined.push_back('\n');
        for (const std::string_view line : split_records) {
            combined.append(line);
            combined.push_back('\n');
        }
        for (const std::string_view line : analysis_records) {
            if (combined.size() > kMaximumSetupBytes - line.size() - 1U) {
                return fail(error,
                            "Combined render/output and analysis data exceeds the 8 MiB limit.");
            }
            combined.append(line);
            combined.push_back('\n');
        }
        CanvasLoopConfig candidate_canvas;
        ExportConfig candidate_output;
        if (!deserialize_render_output_config(
                combined, candidate_canvas, candidate_output, error)) {
            return false;
        }
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
