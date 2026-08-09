#include "config_codec.h"

#include <array>
#include <cstddef>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

namespace pvt::detail {
namespace {

constexpr std::uint32_t kLayerFormatVersion = 2U;
constexpr std::uint32_t kRenderOutputFormatVersion = 1U;
constexpr std::size_t kMaximumCodecLineBytes = 256U * 1024U;

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
    constexpr std::array<std::string_view, 10U> prefixes{{
        "waves.", "swings.", "effects.", "rhythm.", "appearance.",
        "alpha.", "quantization.", "surface.", "palette.", "transform.",
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
           || starts_with(key, "output.");
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
        return fail(error, "Configuration data exceeds the 4 MiB limit.");
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
                return fail(error, "Filtered configuration exceeds the 4 MiB limit.");
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
                      std::string* error) {
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
        if (is_render_key(key) != partial_is_render) {
            destination.append(line);
            destination.push_back('\n');
        }
    }
    for (const std::string_view line : partial_records) {
        if (destination.size() > kMaximumSetupBytes - line.size() - 1U) {
            return fail(error, "Combined configuration exceeds the 4 MiB limit.");
        }
        destination.append(line);
        destination.push_back('\n');
    }
    return true;
}

} // namespace

bool serialize_layer_config(const RenderData& render,
                            std::string& serialized,
                            std::string* error) {
    clear_error(error);
    try {
        ProjectConfig project = default_project();
        if (project.layers.empty()) {
            return fail(error, "The default project does not contain a layer.");
        }
        project.layers.front().render = render;
        // Layer validity must not depend on an arbitrary final RGB/RGBA choice.
        // The project-global output codec validates that choice separately.
        project.output.write_alpha = true;
        const ValidationResult validation = validate(project);
        if (!validation.ok) {
            return fail(error, "Cannot save invalid layer: " + validation.message);
        }
        const RenderConfig config =
            apply_global_config(project.canvas, project.output, render);
        std::string setup;
        if (!serialize_setup_config(config, setup, error)) {
            return false;
        }
        return filter_setup(setup, true,
                            "PVT_LAYER\t" + std::to_string(kLayerFormatVersion),
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
                              std::string* error) {
    clear_error(error);
    try {
        const bool legacy_layer = starts_with(serialized, "PVT_LAYER\t1\n")
                                  || starts_with(serialized, "PVT_LAYER\t1\r\n");
        const std::uint32_t layer_version = legacy_layer ? 1U : kLayerFormatVersion;
        const std::uint32_t setup_version = legacy_layer ? 3U : kSetupFormatVersion;
        std::string setup;
        if (!synthesize_setup(serialized,
                              "PVT_LAYER\t" + std::to_string(layer_version),
                              true, setup_version, setup, error)) {
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
            "PVT_RENDER_OUTPUT\t" + std::to_string(kRenderOutputFormatVersion),
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
        std::string setup;
        if (!synthesize_setup(
                serialized,
                "PVT_RENDER_OUTPUT\t" + std::to_string(kRenderOutputFormatVersion),
                false, kSetupFormatVersion, setup, error)) {
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
        ExportConfig candidate_output = loaded.output;
        canvas = candidate_canvas;
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

} // namespace pvt::detail
