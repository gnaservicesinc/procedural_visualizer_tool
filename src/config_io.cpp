#include "procedural_visualizer_tool.h"
#include "config_codec.h"
#include "frame_renderer_internal.h"
#include "path_utf8.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <new>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include "windows_file_install.h"
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace pvt {
namespace {

// PVT setup files are deliberately line-oriented and deterministic:
//
//   PVT_SETUP<TAB>1
//   canvas.width<TAB>1920
//   waves.count<TAB>1
//   waves.0.name<TAB>Main%20wave
//
// Every subsequent line is exactly one key, one tab, and one value. Collection
// records use zero-based indexes (`waves.N.*`, `swings.N.*`, `effects.N.*`).
// Strings use RFC 3986-style percent encoding over their exact bytes: only
// ALPHA / DIGIT / "-._~" remain literal. There are no comments, aliases, or
// optional records before version 8. The small set of v8 inheritance fields
// explicitly accepts missing/null values; every other field remains required
// for its version. This keeps parsing unambiguous, safely rejectable, and
// friendly to source control while still allowing arbitrary string bytes.
// Version 2 adds PNG compression and custom-OBJ path
// fields. Version 3 separates final output alpha from procedural layer alpha.
// Version 4 adds spatial swings/effects, effect stage selection, palettes, and
// layer transforms. Version 5 adds project clocks, bounded cached music
// analysis, a master swing switch, per-layer audio response, and portable
// custom-OBJ attachment identity. Versions 6 and 7 add local clocks/motion and
// starting-image/reusable-path data. Version 8 adds project/layer audio-response
// inheritance and nullable per-wave/per-effect force/ignore routing. Version 9
// extends those same selectors with explicit audio-feature overrides. Older
// versions remain accepted with neutral defaults for every field introduced
// later. Version 10 adds source RGBA, generated source-color ordering,
// image-to-palette dithering, and configurable blur fields. Version 11 adds
// active-layer/project clock mixing, generated pattern shaping, and Glitch,
// Starburst, and Lens distortion effect types. Version 12 adds bounded,
// layer-local RGB/alpha inversion and edge antialiasing post-process controls,
// plus portable Live endpoints, mappings, scenes, clock routes, calibration,
// output preferences, and watchdog/dropout safety. Version 13 adds generated
// plane-displacement geometry and its portable height-map attachment identity.
// Version 14 adds pre-analysis audio filters/EQ, named frequency streams,
// per-clock stream selection, and Live device-sleep policy. Version 15 adds
// independent particle profile, size variation, definition, twinkle, seed,
// orientation, and rotation controls. Version 16 replaces implicit surface
// camera/orientation constants with a completely authored surface view.
// Version 17 adds layer-local numeric parameter LFOs with stable target paths.
// Version 18 adds independently mixed red, green, and blue inversion stages
// after the existing combined RGB inversion.
// Version 19 adds simultaneous RGBA channel mapping and an authored exact
// permutation of all post-process stages, including quantization.
// Version 20 adds the native loop-safe Water refraction effect type plus
// environment-map lighting and loop-safe mesh-construction surface records.

constexpr std::size_t kMaximumLineBytes = kMaximumUiItems;
constexpr std::size_t kMaximumKeyBytes = kMaximumUiItems;
constexpr std::size_t kMaximumDecodedStringBytes = kMaximumUiItems;
constexpr std::size_t kMaximumRecordCount = kMaximumUiItems;
constexpr std::size_t kMaximumMeterExpressionBytes = kMaximumUiItems;
constexpr std::size_t kMaximumAnalyzerVersionBytes = kMaximumUiItems;
constexpr std::size_t kMaximumMusicBasenameBytes = kMaximumUiItems;
constexpr std::size_t kMaximumMusicFormatBytes = kMaximumUiItems;
constexpr std::size_t kSha256HexBytes = 64U;

static_assert(kSetupFormatVersion == 20U,
              "config_io.cpp implements setup format version 20");
static_assert(std::is_nothrow_move_assignable_v<RenderConfig>,
              "transactional setup loading requires a non-throwing commit");

using Records = std::map<std::string, std::string>;

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size()
           && text.compare(0U, prefix.size(), prefix) == 0;
}

bool render_record_key(std::string_view key) {
    constexpr std::array<std::string_view, 17U> prefixes{{
        "waves.", "swings.", "effects.", "rhythm.", "appearance.",
        "audio_reactive.", "alpha.", "quantization.", "surface.",
        "palette.", "transform.", "layer_clock.", "motion.",
        "source_image.",
        "starting_colors.",
        "post_process.", "parameter_lfos.",
    }};
    return std::any_of(prefixes.begin(), prefixes.end(),
                       [key](std::string_view prefix) {
                           return starts_with(key, prefix);
                       });
}

bool setup_v5_record(std::string_view key) {
    return starts_with(key, "timing.clock.")
           || starts_with(key, "timing.music.")
           || key == "rhythm.swings_enabled"
           || starts_with(key, "audio_reactive.")
           || key == "surface.obj_sha256"
           || key == "surface.obj_basename";
}

bool setup_v2_record(std::string_view key) {
    return key == "surface.obj_path"
           || key == "output.png_compression_level";
}

bool setup_v3_record(std::string_view key) {
    return key == "output.write_alpha";
}

bool setup_v4_record(std::string_view key) {
    const auto suffix = [key](std::string_view value) {
        return key.size() >= value.size()
               && key.compare(key.size() - value.size(), value.size(), value)
                      == 0;
    };
    return starts_with(key, "palette.") || starts_with(key, "transform.")
           || (starts_with(key, "swings.")
               && (suffix(".center_x") || suffix(".center_y")
                   || suffix(".radius")))
           || (starts_with(key, "effects.")
               && (suffix(".space") || suffix(".area_radius")));
}

bool setup_v6_record(std::string_view key) {
    return key == "timing.clock.data_only"
           || starts_with(key, "layer_clock.")
           || starts_with(key, "motion.");
}

bool setup_v7_record(std::string_view key) {
    return starts_with(key, "source_image.")
           || starts_with(key, "paths.")
           || key == "motion.rotation_offset_degrees"
           || starts_with(key, "motion.custom_path.")
           || ((starts_with(key, "waves.")
                || starts_with(key, "effects."))
               && key.find(".path.") != std::string_view::npos);
}

bool setup_v8_record(std::string_view key) {
    const auto suffix = [key](std::string_view value) {
        return key.size() >= value.size()
               && key.compare(key.size() - value.size(), value.size(), value)
                      == 0;
    };
    return starts_with(key, "audio_response_defaults.")
           || key == "audio_reactive.override_enabled"
           || ((starts_with(key, "waves.")
                || starts_with(key, "effects."))
               && suffix(".audio_response"));
}

bool setup_v10_record(std::string_view key) {
    const auto suffix = [key](std::string_view value) {
        return key.size() >= value.size()
               && key.compare(key.size() - value.size(), value.size(), value)
                      == 0;
    };
    return starts_with(key, "starting_colors.")
           || key == "alpha.use_source_alpha"
           || key == "source_image.palette_dither_enabled"
           || key == "source_image.palette_dither_method"
           || (starts_with(key, "palette.colors.") && suffix(".alpha"))
           || (starts_with(key, "effects.")
               && (suffix(".blur_type") || suffix(".blur_passes")
                   || suffix(".blur_samples") || suffix(".blur_minimum")
                   || suffix(".blur_maximum")
                   || suffix(".blur_pulses_per_cycle")));
}

bool setup_v11_record(std::string_view key) {
    const auto suffix = [key](std::string_view value) {
        return key.size() >= value.size()
               && key.compare(key.size() - value.size(), value.size(), value)
                      == 0;
    };
    return key == "layer_clock.mix" || key == "layer_clock.mix_enabled"
           || starts_with(key, "starting_colors.kaleidoscope.")
           || starts_with(key, "starting_colors.domain_warp.")
           || key == "palette.columns"
           || (starts_with(key, "palette.colors.")
               && (suffix(".name") || suffix(".encoding")));
}

bool setup_v12_record(std::string_view key) {
    return starts_with(key, "post_process.") || starts_with(key, "live.");
}

bool setup_v13_record(std::string_view key) {
    return starts_with(key, "surface.plane_displacement.");
}

bool setup_v14_record(std::string_view key) {
    return starts_with(key, "timing.clock.audio_input.")
           || starts_with(key, "timing.music.input_processing.")
           || starts_with(key, "timing.music.frequency_streams.")
           || key == "timing.clock.frequency_stream_uuid"
           || starts_with(key, "layer_clock.clock.audio_input.")
           || starts_with(key, "layer_clock.music.input_processing.")
           || starts_with(key, "layer_clock.music.frequency_streams.")
           || key == "layer_clock.clock.frequency_stream_uuid"
           || starts_with(key, "live.audio_input.")
           || key == "live.safety.prevent_device_sleep"
           || (starts_with(key, "effects.")
               && key.size() >= std::string_view(".particle_shape").size()
               && key.compare(key.size()
                                  - std::string_view(".particle_shape").size(),
                              std::string_view(".particle_shape").size(),
                              ".particle_shape") == 0)
           || (starts_with(key, "live.clock_inputs.")
               && key.size() >= std::string_view(".frequency_stream_uuid").size()
               && key.compare(key.size()
                                  - std::string_view(".frequency_stream_uuid").size(),
                              std::string_view(".frequency_stream_uuid").size(),
                              ".frequency_stream_uuid") == 0);
}

bool setup_v15_record(std::string_view key) {
    if (!starts_with(key, "effects.")) return false;
    const auto suffix = [key](std::string_view value) {
        return key.size() >= value.size()
               && key.compare(key.size() - value.size(), value.size(), value)
                      == 0;
    };
    return suffix(".particle_profile")
           || suffix(".particle_size_variation")
           || suffix(".particle_definition")
           || suffix(".particle_twinkle")
           || suffix(".particle_seed")
           || suffix(".particle_orientation")
           || suffix(".particle_rotation_degrees");
}

bool setup_v16_record(std::string_view key) {
    return key == "surface.projection"
           || key == "surface.sizing"
           || key == "surface.outside"
           || key == "surface.rotation_order"
           || key == "surface.rotation_x_turns_per_loop"
           || key == "surface.rotation_y_turns_per_loop"
           || key == "surface.rotation_z_turns_per_loop"
           || key == "surface.rotation_x_degrees"
           || key == "surface.rotation_y_degrees"
           || key == "surface.rotation_z_degrees"
           || key == "surface.size_percent"
           || key == "surface.scale_x"
           || key == "surface.scale_y"
           || key == "surface.scale_z"
           || key == "surface.position_x_percent"
           || key == "surface.position_y_percent"
           || key == "surface.position_z"
           || key == "surface.camera_distance"
           || key == "surface.focal_length"
           || key == "surface.light_direction_x"
           || key == "surface.light_direction_y"
           || key == "surface.light_direction_z"
           || key == "surface.light_ambient"
           || key == "surface.light_diffuse"
           || key == "surface.composite_backfaces"
           || key == "surface.normalize_obj";
}

bool setup_v17_record(std::string_view key) {
    return starts_with(key, "parameter_lfos.");
}

bool setup_v18_record(std::string_view key) {
    return key == "post_process.invert_red_enabled"
           || key == "post_process.invert_red_mix"
           || key == "post_process.invert_green_enabled"
           || key == "post_process.invert_green_mix"
           || key == "post_process.invert_blue_enabled"
           || key == "post_process.invert_blue_mix";
}

bool setup_v19_record(std::string_view key) {
    return starts_with(key, "post_process.channel_map.")
           || starts_with(key, "post_process.order.");
}

bool setup_v20_record(std::string_view key) {
    return starts_with(key, "surface.environment_map.")
           || starts_with(key, "surface.mesh_construction.");
}

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

std::string record_error(std::string_view prefix, std::string_view key) {
    std::string message;
    message.reserve(prefix.size() + key.size() + 4U);
    message.append(prefix);
    message.push_back(' ');
    message.push_back('\'');
    message.append(key);
    message.push_back('\'');
    message.push_back('.');
    return message;
}

bool valid_key(std::string_view key) {
    if (key.empty() || key.size() > kMaximumKeyBytes) {
        return false;
    }
    for (const char raw_character : key) {
        const unsigned char character = static_cast<unsigned char>(raw_character);
        if ((character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9')
            || character == '.' || character == '_') {
            continue;
        }
        return false;
    }
    return true;
}

bool line_is_ascii_text(std::string_view line) {
    for (const char raw_character : line) {
        const unsigned char character = static_cast<unsigned char>(raw_character);
        if (character == '\t') {
            continue;
        }
        if (character < 0x20U || character > 0x7eU) {
            return false;
        }
    }
    return true;
}

bool parse_records(const std::string& contents,
                   Records& records,
                   std::uint32_t& setup_version,
                   std::string* error) {
    if (contents.empty()) {
        return fail(error, "Setup file is empty.");
    }

    std::size_t line_start = 0;
    std::size_t line_number = 0;
    while (line_start < contents.size()) {
        const std::size_t newline = contents.find('\n', line_start);
        const std::size_t raw_end = newline == std::string::npos ? contents.size() : newline;
        if (raw_end - line_start > kMaximumLineBytes) {
            return fail(error, "Setup line " + std::to_string(line_number + 1U)
                                   + " exceeds the signed-int line limit.");
        }

        std::string_view line(contents.data() + line_start, raw_end - line_start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1U);
        }
        ++line_number;

        if (line.empty()) {
            return fail(error, "Setup line " + std::to_string(line_number)
                                   + " is empty; blank lines are not valid in setup files.");
        }
        if (!line_is_ascii_text(line)) {
            return fail(error, "Setup line " + std::to_string(line_number)
                                   + " contains a raw control or non-ASCII byte; strings must be percent-encoded.");
        }

        if (line_number == 1U) {
            constexpr std::string_view prefix = "PVT_SETUP\t";
            if (!starts_with(line, prefix) || line.size() == prefix.size()) {
                return fail(error,
                            "Unsupported or malformed setup header; expected "
                            "'PVT_SETUP\\t' followed by a positive version.");
            }
            std::uint32_t declared_version = 0U;
            const std::string_view number = line.substr(prefix.size());
            const auto parsed = std::from_chars(
                number.data(), number.data() + number.size(),
                declared_version, 10);
            if (parsed.ec != std::errc{}
                || parsed.ptr != number.data() + number.size()
                || declared_version == 0U) {
                return fail(error,
                            "Unsupported or malformed setup header; expected "
                            "'PVT_SETUP\\t' followed by a positive version.");
            }
            setup_version = std::min(declared_version,
                                     kSetupFormatVersion);
        } else {
            const std::size_t tab = line.find('\t');
            if (tab == std::string_view::npos || line.find('\t', tab + 1U) != std::string_view::npos) {
                return fail(error, "Setup line " + std::to_string(line_number)
                                       + " must contain exactly one tab delimiter.");
            }

            const std::string_view key_view = line.substr(0U, tab);
            const std::string_view value_view = line.substr(tab + 1U);
            if (!valid_key(key_view)) {
                return fail(error, "Setup line " + std::to_string(line_number)
                                       + " has an invalid or overlong key.");
            }
            if (records.size() >= kMaximumRecordCount) {
                return fail(error, "Setup file exceeds the signed-int record limit.");
            }

            std::string key(key_view);
            const auto inserted = records.emplace(key, std::string(value_view));
            if (!inserted.second) {
                return fail(error, record_error("Duplicate setup key", key));
            }
        }

        if (newline == std::string::npos) {
            break;
        }
        line_start = newline + 1U;
    }

    if (line_number == 0U) {
        return fail(error, "Setup file is empty.");
    }
    return true;
}

bool read_setup_file(const std::string& path, std::string& contents, std::string* error) {
    if (path.empty() || path.size() > kMaximumDecodedStringBytes
        || path.find('\0') != std::string::npos) {
        return fail(error, "Setup path is empty, contains a NUL byte, or exceeds the signed-int text API limit.");
    }

    const std::filesystem::path native_path = detail::path_from_utf8(path);
    std::ifstream input(native_path, std::ios::binary);
    if (!input) {
        return fail(error, "Could not open setup file '" + path + "' for reading.");
    }

    contents.clear();
    std::array<char, 8192U> buffer{};
    for (;;) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            const std::size_t byte_count = static_cast<std::size_t>(count);
            if (contents.size() > kMaximumSetupBytes - byte_count) {
                return fail(error, "Setup file exceeds the signed-int input limit.");
            }
            contents.append(buffer.data(), byte_count);
        }
        if (input.eof()) {
            break;
        }
        if (!input) {
            return fail(error, "I/O error while reading setup file '" + path + "'.");
        }
    }
    return true;
}

bool take_record(Records& records,
                 const std::string& key,
                 std::string& value,
                 std::string* error) {
    const auto found = records.find(key);
    if (found == records.end()) {
        return fail(error, record_error("Missing required setup key", key));
    }
    value = std::move(found->second);
    records.erase(found);
    return true;
}

template <typename Integer>
bool parse_integer_exact(std::string_view text, Integer& destination) {
    static_assert(std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>);
    if (text.empty()) {
        return false;
    }
    Integer parsed{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return false;
    }
    destination = parsed;
    return true;
}

bool parse_double_exact(std::string_view text, double& destination) {
    if (text.empty()) {
        return false;
    }
    // Music analyses contain hundreds of thousands of decimal fields. A
    // locale-owning stringstream per value dominated large-project loads.
    // The C-locale fast path avoids stream/facet setup; retain the old parser
    // whenever the process uses another numeric locale or for an accepted
    // spelling that strtod does not consume exactly.
    const std::lconv* numeric_locale = std::localeconv();
    if (numeric_locale != nullptr && numeric_locale->decimal_point != nullptr
        && numeric_locale->decimal_point[0] == '.'
        && numeric_locale->decimal_point[1] == '\0'
        && !std::isspace(static_cast<unsigned char>(text.front()))) {
        const std::string terminated(text);
        char* end = nullptr;
        errno = 0;
        const double fast_parsed = std::strtod(terminated.c_str(), &end);
        if (errno != ERANGE && end == terminated.c_str() + terminated.size()
            && std::isfinite(fast_parsed)) {
            destination = fast_parsed;
            return true;
        }
    }
    std::istringstream stream{std::string(text)};
    stream.imbue(std::locale::classic());
    stream >> std::noskipws;
    double parsed = 0.0;
    if (!(stream >> parsed) || stream.peek() != std::char_traits<char>::eof()
        || !std::isfinite(parsed)) {
        return false;
    }
    destination = parsed;
    return true;
}

bool parse_bool_exact(std::string_view text, bool& destination) {
    if (text == "0") {
        destination = false;
        return true;
    }
    if (text == "1") {
        destination = true;
        return true;
    }
    return false;
}

int hexadecimal_value(unsigned char character) {
    if (character >= '0' && character <= '9') {
        return static_cast<int>(character - '0');
    }
    if (character >= 'A' && character <= 'F') {
        return static_cast<int>(character - 'A') + 10;
    }
    if (character >= 'a' && character <= 'f') {
        return static_cast<int>(character - 'a') + 10;
    }
    return -1;
}

bool is_unreserved(unsigned char character) {
    return (character >= 'A' && character <= 'Z')
           || (character >= 'a' && character <= 'z')
           || (character >= '0' && character <= '9')
           || character == '-' || character == '.' || character == '_'
           || character == '~';
}

bool percent_decode(std::string_view encoded, std::string& decoded) {
    decoded.clear();
    decoded.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size();) {
        const unsigned char character = static_cast<unsigned char>(encoded[index]);
        if (character == '%') {
            if (index + 2U >= encoded.size()) {
                return false;
            }
            const int high = hexadecimal_value(static_cast<unsigned char>(encoded[index + 1U]));
            const int low = hexadecimal_value(static_cast<unsigned char>(encoded[index + 2U]));
            if (high < 0 || low < 0) {
                return false;
            }
            decoded.push_back(static_cast<char>((high << 4) | low));
            index += 3U;
        } else {
            if (!is_unreserved(character)) {
                return false;
            }
            decoded.push_back(static_cast<char>(character));
            ++index;
        }
        if (decoded.size() > kMaximumDecodedStringBytes) {
            return false;
        }
    }
    return true;
}

bool percent_encode(std::string_view decoded, std::string& encoded) {
    if (decoded.size() > kMaximumDecodedStringBytes) {
        return false;
    }
    constexpr char hexadecimal[] = "0123456789ABCDEF";
    std::size_t encoded_size = 0U;
    for (const char raw_character : decoded) {
        const unsigned char character = static_cast<unsigned char>(raw_character);
        const std::size_t addition = is_unreserved(character) ? 1U : 3U;
        if (addition > kMaximumDecodedStringBytes - encoded_size) {
            return false;
        }
        encoded_size += addition;
    }
    encoded.clear();
    encoded.reserve(encoded_size);
    for (const char raw_character : decoded) {
        const unsigned char character = static_cast<unsigned char>(raw_character);
        if (is_unreserved(character)) {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('%');
            encoded.push_back(hexadecimal[character >> 4U]);
            encoded.push_back(hexadecimal[character & 0x0fU]);
        }
    }
    return true;
}

template <typename Integer>
bool consume_integer(Records& records,
                     const std::string& key,
                     Integer& destination,
                     std::string* error) {
    std::string value;
    if (!take_record(records, key, value, error)) {
        return false;
    }
    if (!parse_integer_exact(value, destination)) {
        return fail(error, record_error("Invalid integer in setup key", key));
    }
    return true;
}

bool consume_double(Records& records,
                    const std::string& key,
                    double& destination,
                    std::string* error) {
    std::string value;
    if (!take_record(records, key, value, error)) {
        return false;
    }
    if (!parse_double_exact(value, destination)) {
        return fail(error, record_error("Invalid finite number in setup key", key));
    }
    return true;
}

bool consume_bool(Records& records,
                  const std::string& key,
                  bool& destination,
                  std::string* error) {
    std::string value;
    if (!take_record(records, key, value, error)) {
        return false;
    }
    if (!parse_bool_exact(value, destination)) {
        return fail(error, record_error("Invalid boolean (expected 0 or 1) in setup key", key));
    }
    return true;
}

bool consume_string(Records& records,
                    const std::string& key,
                    std::string& destination,
                    std::string* error) {
    std::string value;
    if (!take_record(records, key, value, error)) {
        return false;
    }
    std::string decoded;
    if (!percent_decode(value, decoded)) {
        return fail(error, record_error("Invalid or overlong percent-encoded string in setup key", key));
    }
    destination = std::move(decoded);
    return true;
}

bool consume_bounded_string(Records& records,
                            const std::string& key,
                            std::size_t maximum,
                            std::string& destination,
                            std::string* error) {
    std::string value;
    if (!consume_string(records, key, value, error)) {
        return false;
    }
    if (value.size() > maximum) {
        return fail(error, record_error("Decoded string exceeds its field limit in setup key",
                                        key));
    }
    destination = std::move(value);
    return true;
}

bool consume_float(Records& records,
                   const std::string& key,
                   float& destination,
                   std::string* error) {
    double value = 0.0;
    if (!consume_double(records, key, value, error)) {
        return false;
    }
    if (value < -static_cast<double>(std::numeric_limits<float>::max())
        || value > static_cast<double>(std::numeric_limits<float>::max())) {
        return fail(error, record_error("Number exceeds the float range in setup key", key));
    }
    destination = static_cast<float>(value);
    return true;
}

bool is_lowercase_sha256(std::string_view digest) {
    if (digest.empty()) {
        return true;
    }
    if (digest.size() != kSha256HexBytes) {
        return false;
    }
    return std::all_of(digest.begin(), digest.end(), [](char character) {
        return (character >= '0' && character <= '9')
               || (character >= 'a' && character <= 'f');
    });
}

template <typename Enum, std::size_t Count>
bool consume_enum(Records& records,
                  const std::string& key,
                  Enum& destination,
                  const std::array<std::pair<std::string_view, Enum>, Count>& values,
                  std::string* error) {
    std::string value;
    if (!take_record(records, key, value, error)) {
        return false;
    }
    for (const auto& entry : values) {
        if (value == entry.first) {
            destination = entry.second;
            return true;
        }
    }
    return fail(error, record_error("Unknown enum token in setup key", key));
}

template <typename Enum, std::size_t Count>
bool consume_optional_enum(
    Records& records,
    const std::string& key,
    Enum& destination,
    Enum default_value,
    const std::array<std::pair<std::string_view, Enum>, Count>& values,
    std::string* error) {
    const auto found = records.find(key);
    if (found == records.end()) {
        destination = default_value;
        return true;
    }
    const std::string value = std::move(found->second);
    records.erase(found);
    if (value == "null") {
        destination = default_value;
        return true;
    }
    for (const auto& entry : values) {
        if (value == entry.first) {
            destination = entry.second;
            return true;
        }
    }
    return fail(error, record_error("Unknown enum token in setup key", key));
}

bool consume_optional_bool(Records& records,
                           const std::string& key,
                           bool& destination,
                           bool default_value,
                           std::string* error) {
    const auto found = records.find(key);
    if (found == records.end()) {
        destination = default_value;
        return true;
    }
    const std::string value = std::move(found->second);
    records.erase(found);
    if (value == "null") {
        destination = default_value;
        return true;
    }
    if (!parse_bool_exact(value, destination)) {
        return fail(error,
                    record_error(
                        "Invalid nullable boolean (expected 0, 1, or null) in setup key",
                        key));
    }
    return true;
}

bool consume_count(Records& records,
                   const std::string& key,
                   std::size_t maximum,
                   std::size_t& destination,
                   std::string* error) {
    std::uint64_t parsed = 0;
    if (!consume_integer(records, key, parsed, error)) {
        return false;
    }
    if (parsed > static_cast<std::uint64_t>(maximum)) {
        return fail(error, record_error("Collection count exceeds its configured maximum in setup key", key));
    }
    destination = static_cast<std::size_t>(parsed);
    return true;
}

constexpr std::array<std::pair<std::string_view, EdgeMode>, 4U> kEdgeModes{{
    {"alpha", EdgeMode::Alpha},
    {"black", EdgeMode::Black},
    {"white", EdgeMode::White},
    {"reflect", EdgeMode::Reflect},
}};

constexpr std::array<std::pair<std::string_view, EffectType>, 7U> kEffectTypesV9{{
    {"endless_zoom", EffectType::EndlessZoom},
    {"ripple", EffectType::Ripple},
    {"shake", EffectType::Shake},
    {"flag_wave", EffectType::FlagWave},
    {"glow", EffectType::Glow},
    {"block_scale", EffectType::BlockScale},
    {"particle_field", EffectType::ParticleField},
}};

constexpr std::array<std::pair<std::string_view, EffectType>, 8U>
    kEffectTypesV10{{
    {"endless_zoom", EffectType::EndlessZoom},
    {"ripple", EffectType::Ripple},
    {"shake", EffectType::Shake},
    {"flag_wave", EffectType::FlagWave},
    {"glow", EffectType::Glow},
    {"block_scale", EffectType::BlockScale},
    {"particle_field", EffectType::ParticleField},
    {"blur", EffectType::Blur},
}};

constexpr std::array<std::pair<std::string_view, EffectType>, 11U>
    kEffectTypesV13{{
    {"endless_zoom", EffectType::EndlessZoom},
    {"ripple", EffectType::Ripple},
    {"shake", EffectType::Shake},
    {"flag_wave", EffectType::FlagWave},
    {"glow", EffectType::Glow},
    {"block_scale", EffectType::BlockScale},
    {"particle_field", EffectType::ParticleField},
    {"blur", EffectType::Blur},
    {"glitch", EffectType::Glitch},
    {"starburst", EffectType::Starburst},
    {"lens_distortion", EffectType::LensDistortion},
}};

constexpr std::array<std::pair<std::string_view, EffectType>, 13U>
    kEffectTypesV19{{
    {"endless_zoom", EffectType::EndlessZoom},
    {"ripple", EffectType::Ripple},
    {"shake", EffectType::Shake},
    {"flag_wave", EffectType::FlagWave},
    {"glow", EffectType::Glow},
    {"block_scale", EffectType::BlockScale},
    {"particle_field", EffectType::ParticleField},
    {"blur", EffectType::Blur},
    {"glitch", EffectType::Glitch},
    {"starburst", EffectType::Starburst},
    {"lens_distortion", EffectType::LensDistortion},
    {"edge_detect", EffectType::EdgeDetect},
    {"twirl", EffectType::Twirl},
}};

constexpr std::array<std::pair<std::string_view, EffectType>, 14U> kEffectTypes{{
    {"endless_zoom", EffectType::EndlessZoom},
    {"ripple", EffectType::Ripple},
    {"shake", EffectType::Shake},
    {"flag_wave", EffectType::FlagWave},
    {"glow", EffectType::Glow},
    {"block_scale", EffectType::BlockScale},
    {"particle_field", EffectType::ParticleField},
    {"blur", EffectType::Blur},
    {"glitch", EffectType::Glitch},
    {"starburst", EffectType::Starburst},
    {"lens_distortion", EffectType::LensDistortion},
    {"edge_detect", EffectType::EdgeDetect},
    {"twirl", EffectType::Twirl},
    {"water", EffectType::Water},
}};

constexpr std::array<std::pair<std::string_view, BlurType>, 5U> kBlurTypes{{
    {"gaussian", BlurType::Gaussian},
    {"box", BlurType::Box},
    {"directional", BlurType::Directional},
    {"radial", BlurType::Radial},
    {"zoom", BlurType::Zoom},
}};

constexpr std::array<std::pair<std::string_view, ParticleShape>, 5U>
    kParticleShapes{{
    {"spark", ParticleShape::Spark},
    {"soft_orb", ParticleShape::SoftOrb},
    {"ring", ParticleShape::Ring},
    {"diamond", ParticleShape::Diamond},
    {"star", ParticleShape::Star},
}};

constexpr std::array<std::pair<std::string_view, ParticleRenderProfile>, 2U>
    kParticleProfiles{{
        {"legacy_glow", ParticleRenderProfile::LegacyGlow},
        {"defined", ParticleRenderProfile::Defined},
    }};

constexpr std::array<std::pair<std::string_view, ParticleOrientation>, 3U>
    kParticleOrientations{{
        {"fixed", ParticleOrientation::Fixed},
        {"follow_motion", ParticleOrientation::FollowMotion},
        {"random", ParticleOrientation::Random},
    }};

constexpr std::array<std::pair<std::string_view, StartingColorMode>, 8U>
    kStartingColorModes{{
        // Retain the existing token so older PVT versions can still read newly
        // saved Continuous hue projects. "Legacy" is not a product-facing name.
        {"legacy_hue", StartingColorMode::ContinuousHue},
        {"channel_loops", StartingColorMode::HorizontalRainbow},
        {"interleaved", StartingColorMode::VerticalRainbow},
        {"additive", StartingColorMode::DiagonalRainbow},
        {"spiral", StartingColorMode::SpiralRainbow},
        {"square_spiral", StartingColorMode::SquareSpiralRainbow},
        // Version 1.2.5 and earlier used this token for the rectangular-ring
        // traversal that is now product-facing as Square spiral rainbow.
        {"subtractive", StartingColorMode::SquareSpiralRainbow},
        {"random", StartingColorMode::Random},
    }};

constexpr std::array<std::pair<std::string_view, LayerClockScale>, 5U>
    kLayerClockScales{{
        {"smart_loop_fit", LayerClockScale::SmartLoopFit},
        {"straight_fit", LayerClockScale::StraightFit},
        {"play_once", LayerClockScale::PlayOnce},
        {"play_once_then_project", LayerClockScale::PlayOnceThenProject},
        {"original_speed_loop", LayerClockScale::OriginalSpeedLoop},
    }};

constexpr std::array<std::pair<std::string_view, LayerClockMixMode>, 5U>
    kLayerClockMixModes{{
        {"replace", LayerClockMixMode::Replace},
        {"add", LayerClockMixMode::Add},
        {"difference", LayerClockMixMode::Difference},
        {"soft_xor", LayerClockMixMode::SoftXor},
        {"bitwise_xor", LayerClockMixMode::BitwiseXor},
    }};

constexpr std::array<std::pair<std::string_view, PaletteColorEncoding>, 2U>
    kPaletteColorEncodings{{
        {"srgb", PaletteColorEncoding::Srgb},
        {"linear", PaletteColorEncoding::Linear},
    }};

constexpr std::array<std::pair<std::string_view, LayerMotionPath>, 5U>
    kLayerMotionPaths{{
        {"none", LayerMotionPath::None},
        {"orbit", LayerMotionPath::Orbit},
        {"figure_eight", LayerMotionPath::FigureEight},
        {"bounce", LayerMotionPath::Bounce},
        {"lissajous", LayerMotionPath::Lissajous},
    }};

constexpr std::array<std::pair<std::string_view, EffectSpace>, 2U> kEffectSpaces{{
    {"texture", EffectSpace::Texture},
    {"surface", EffectSpace::Surface},
}};

constexpr std::array<std::pair<std::string_view, DitherMethod>, 3U> kDitherMethods{{
    {"blue_noise", DitherMethod::BlueNoise},
    {"ordered_bayer", DitherMethod::OrderedBayer},
    {"floyd_steinberg", DitherMethod::FloydSteinberg},
}};

constexpr std::array<std::pair<std::string_view, SurfaceMapping>, 5U> kSurfaceMappings{{
    {"plane", SurfaceMapping::Plane},
    {"cylinder", SurfaceMapping::Cylinder},
    {"sphere", SurfaceMapping::Sphere},
    {"cube", SurfaceMapping::Cube},
    {"custom_obj", SurfaceMapping::CustomObj},
}};

constexpr std::array<std::pair<std::string_view, SurfaceProjection>, 2U>
    kSurfaceProjections{{
        {"orthographic", SurfaceProjection::Orthographic},
        {"perspective", SurfaceProjection::Perspective},
    }};

constexpr std::array<std::pair<std::string_view, SurfaceSizing>, 4U>
    kSurfaceSizings{{
        {"contain", SurfaceSizing::Contain},
        {"cover", SurfaceSizing::Cover},
        {"stretch", SurfaceSizing::Stretch},
        {"short_side", SurfaceSizing::ShortSide},
    }};

constexpr std::array<std::pair<std::string_view, SurfaceOutside>, 3U>
    kSurfaceOutsides{{
        {"transparent", SurfaceOutside::Transparent},
        {"source", SurfaceOutside::Source},
        {"reflect", SurfaceOutside::Reflect},
    }};

constexpr std::array<std::pair<std::string_view, SurfaceRotationOrder>, 6U>
    kSurfaceRotationOrders{{
        {"xyz", SurfaceRotationOrder::XYZ},
        {"xzy", SurfaceRotationOrder::XZY},
        {"yxz", SurfaceRotationOrder::YXZ},
        {"yzx", SurfaceRotationOrder::YZX},
        {"zxy", SurfaceRotationOrder::ZXY},
        {"zyx", SurfaceRotationOrder::ZYX},
    }};

constexpr std::array<std::pair<std::string_view, EnvironmentMapEncoding>, 3U>
    kEnvironmentMapEncodings{{
        {"auto", EnvironmentMapEncoding::Auto},
        {"srgb", EnvironmentMapEncoding::Srgb},
        {"linear", EnvironmentMapEncoding::Linear},
    }};

constexpr std::array<std::pair<std::string_view, MeshConstructionMode>, 4U>
    kMeshConstructionModes{{
        {"none", MeshConstructionMode::None},
        {"explode", MeshConstructionMode::Explode},
        {"deconstruct", MeshConstructionMode::Deconstruct},
        {"reconstruct", MeshConstructionMode::Reconstruct},
    }};

constexpr std::array<std::pair<std::string_view, MeshFragmentation>, 3U>
    kMeshFragmentations{{
        {"automatic", MeshFragmentation::Automatic},
        {"connected_components", MeshFragmentation::ConnectedComponents},
        {"triangle_clusters", MeshFragmentation::TriangleClusters},
    }};

constexpr std::array<std::pair<std::string_view, StartingImageFit>, 4U>
    kStartingImageFits{{
        {"stretch", StartingImageFit::Stretch},
        {"contain", StartingImageFit::Contain},
        {"cover", StartingImageFit::Cover},
        {"tile", StartingImageFit::Tile},
    }};

constexpr std::array<std::pair<std::string_view, PathHandleMode>, 4U>
    kPathHandleModes{{
        {"corner", PathHandleMode::Corner},
        {"auto_smooth", PathHandleMode::AutoSmooth},
        {"smooth", PathHandleMode::Smooth},
        {"symmetric", PathHandleMode::Symmetric},
    }};

constexpr std::array<std::pair<std::string_view, Waveform>, 4U> kWaveforms{{
    {"sine", Waveform::Sine},
    {"triangle", Waveform::Triangle},
    {"smooth_pulse", Waveform::SmoothPulse},
    {"bounce", Waveform::Bounce},
}};

constexpr std::array<std::pair<std::string_view, QuantizationMode>, 3U> kQuantizationModes{{
    {"rgb", QuantizationMode::Rgb},
    {"luminance", QuantizationMode::Luminance},
    {"hue", QuantizationMode::Hue},
}};

constexpr std::array<std::pair<std::string_view, ChannelSource>, 6U>
    kChannelSources{{
        {"red", ChannelSource::Red},
        {"green", ChannelSource::Green},
        {"blue", ChannelSource::Blue},
        {"alpha", ChannelSource::Alpha},
        {"zero", ChannelSource::Zero},
        {"one", ChannelSource::One},
    }};

constexpr std::array<std::pair<std::string_view, PostProcessStage>,
                     kPostProcessStageCount>
    kPostProcessStages{{
        {"invert_rgb", PostProcessStage::InvertRgb},
        {"invert_red", PostProcessStage::InvertRed},
        {"invert_green", PostProcessStage::InvertGreen},
        {"invert_blue", PostProcessStage::InvertBlue},
        {"invert_alpha", PostProcessStage::InvertAlpha},
        {"channel_map", PostProcessStage::ChannelMap},
        {"antialias", PostProcessStage::Antialias},
        {"quantization", PostProcessStage::Quantization},
    }};

constexpr std::array<std::pair<std::string_view, MirrorMode>, 6U> kMirrorModes{{
    {"none", MirrorMode::None},
    {"left_to_right", MirrorMode::LeftToRight},
    {"right_to_left", MirrorMode::RightToLeft},
    {"top_to_bottom", MirrorMode::TopToBottom},
    {"bottom_to_top", MirrorMode::BottomToTop},
    {"four_way", MirrorMode::FourWay},
}};

constexpr std::array<std::pair<std::string_view, ClockMode>, 5U> kClockModes{{
    {"default", ClockMode::Default},
    {"frame", ClockMode::Frame},
    {"time", ClockMode::Time},
    {"meter", ClockMode::Meter},
    {"music", ClockMode::Music},
}};

constexpr std::array<std::pair<std::string_view, ClockInterpolation>, 3U>
    kClockInterpolations{{
        {"hold", ClockInterpolation::Hold},
        {"linear", ClockInterpolation::Linear},
        {"smoothstep", ClockInterpolation::Smoothstep},
    }};

constexpr std::array<std::pair<std::string_view, ClockFit>, 2U> kClockFits{{
    {"exact", ClockFit::Exact},
    {"fit_sequence", ClockFit::FitSequence},
}};

constexpr std::array<std::pair<std::string_view, MusicTempoMode>, 3U>
    kMusicTempoModes{{
        {"half", MusicTempoMode::Half},
        {"detected", MusicTempoMode::Detected},
        {"double", MusicTempoMode::Double},
    }};

constexpr std::array<std::pair<std::string_view, MusicFeature>, 10U>
    kMusicFeatures{{
        {"energy", MusicFeature::Energy},
        {"bass", MusicFeature::Bass},
        {"midrange", MusicFeature::Midrange},
        {"treble", MusicFeature::Treble},
        {"onset", MusicFeature::Onset},
        {"beat", MusicFeature::Beat},
        {"spectral_centroid", MusicFeature::SpectralCentroid},
        {"spectral_flatness", MusicFeature::SpectralFlatness},
        {"chroma_hue", MusicFeature::ChromaHue},
        {"chroma_strength", MusicFeature::ChromaStrength},
    }};

constexpr std::array<std::pair<std::string_view, AudioResponseMode>, 13U>
    kAudioResponseModes{{
        {"default", AudioResponseMode::Default},
        {"enabled", AudioResponseMode::Enabled},
        {"disabled", AudioResponseMode::Disabled},
        {"energy", AudioResponseMode::Energy},
        {"bass", AudioResponseMode::Bass},
        {"midrange", AudioResponseMode::Midrange},
        {"treble", AudioResponseMode::Treble},
        {"onset", AudioResponseMode::Onset},
        {"beat", AudioResponseMode::Beat},
        {"spectral_centroid", AudioResponseMode::SpectralCentroid},
        {"spectral_flatness", AudioResponseMode::SpectralFlatness},
        {"chroma_hue", AudioResponseMode::ChromaHue},
        {"chroma_strength", AudioResponseMode::ChromaStrength},
    }};

// Format 8 only defined these tokens. Decode against this exact table so a
// hand-edited v8 document cannot silently claim format-9 source semantics.
constexpr std::array<std::pair<std::string_view, AudioResponseMode>, 3U>
    kAudioResponseModesV8{{
        {"default", AudioResponseMode::Default},
        {"enabled", AudioResponseMode::Enabled},
        {"disabled", AudioResponseMode::Disabled},
    }};

constexpr std::array<std::pair<std::string_view, MusicSwingPolicy>, 3U>
    kMusicSwingPolicies{{
        {"suppress_all", MusicSwingPolicy::SuppressAll},
        {"suppress_global", MusicSwingPolicy::SuppressGlobal},
        {"keep_all", MusicSwingPolicy::KeepAll},
    }};

constexpr std::array<std::pair<std::string_view, LiveEndpointProtocol>, 4U>
    kLiveEndpointProtocols{{
        {"audio", LiveEndpointProtocol::Audio},
        {"midi", LiveEndpointProtocol::Midi},
        {"osc", LiveEndpointProtocol::Osc},
        {"foot_controller", LiveEndpointProtocol::FootController},
    }};

constexpr std::array<std::pair<std::string_view, LiveEndpointDirection>, 3U>
    kLiveEndpointDirections{{
        {"input", LiveEndpointDirection::Input},
        {"output", LiveEndpointDirection::Output},
        {"bidirectional", LiveEndpointDirection::Bidirectional},
    }};

constexpr std::array<std::pair<std::string_view, LiveControlInput>, 7U>
    kLiveControlInputs{{
        {"midi_cc", LiveControlInput::MidiControlChange},
        {"midi_note", LiveControlInput::MidiNote},
        {"midi_program", LiveControlInput::MidiProgramChange},
        {"midi_pitch_bend", LiveControlInput::MidiPitchBend},
        {"midi_channel_pressure", LiveControlInput::MidiChannelPressure},
        {"osc_value", LiveControlInput::OscValue},
        {"footswitch", LiveControlInput::Footswitch},
    }};

constexpr std::array<std::pair<std::string_view, LiveMappingMode>, 5U>
    kLiveMappingModes{{
        {"absolute", LiveMappingMode::Absolute},
        {"relative", LiveMappingMode::Relative},
        {"toggle", LiveMappingMode::Toggle},
        {"momentary", LiveMappingMode::Momentary},
        {"trigger", LiveMappingMode::Trigger},
    }};

constexpr std::array<std::pair<std::string_view, LiveMappingTarget>, 3U>
    kLiveMappingTargets{{
        {"setting", LiveMappingTarget::Setting},
        {"action", LiveMappingTarget::Action},
        {"scene", LiveMappingTarget::Scene},
    }};

constexpr std::array<std::pair<std::string_view, LiveAction>, 6U>
    kLiveActions{{
        {"freeze", LiveAction::Freeze},
        {"blackout", LiveAction::Blackout},
        {"next_scene", LiveAction::NextScene},
        {"previous_scene", LiveAction::PreviousScene},
        {"restart_scene", LiveAction::RestartScene},
        {"tap_tempo", LiveAction::TapTempo},
    }};

constexpr std::array<std::pair<std::string_view, LiveClockTarget>, 2U>
    kLiveClockTargets{{
        {"project", LiveClockTarget::Project},
        {"layer", LiveClockTarget::Layer},
    }};

constexpr std::array<std::pair<std::string_view, LiveClockInputSource>, 2U>
    kLiveClockInputSources{{
        {"midi_clock", LiveClockInputSource::MidiClock},
        {"audio_stream", LiveClockInputSource::AudioStream},
    }};

constexpr std::array<std::pair<std::string_view, LiveSceneValueType>, 5U>
    kLiveSceneValueTypes{{
        {"boolean", LiveSceneValueType::Boolean},
        {"integer", LiveSceneValueType::Integer},
        {"real", LiveSceneValueType::Real},
        {"enum", LiveSceneValueType::EnumToken},
        {"string", LiveSceneValueType::String},
    }};

constexpr std::array<std::pair<std::string_view, LiveDropoutBehavior>, 2U>
    kLiveDropoutBehaviors{{
        {"last_good_frame", LiveDropoutBehavior::LastGoodFrame},
        {"blackout", LiveDropoutBehavior::Blackout},
    }};

std::string indexed_key(std::string_view collection,
                        std::size_t index,
                        std::string_view field) {
    std::string key;
    const std::string index_text = std::to_string(index);
    key.reserve(collection.size() + index_text.size() + field.size() + 2U);
    key.append(collection);
    key.push_back('.');
    key.append(index_text);
    key.push_back('.');
    key.append(field);
    return key;
}

template <typename Enum, std::size_t Count>
bool enum_token(Enum value,
                const std::array<std::pair<std::string_view, Enum>, Count>& values,
                std::string_view& token) {
    for (const auto& entry : values) {
        if (entry.second == value) {
            token = entry.first;
            return true;
        }
    }
    return false;
}

class SetupBuilder {
public:
    explicit SetupBuilder(std::string* error)
        : error_(error),
          contents_("PVT_SETUP\t" + std::to_string(kSetupFormatVersion) + "\n") {}

    bool add(std::string_view key, std::string_view value) {
        if (!ok_) {
            return false;
        }
        if (!valid_key(key)) {
            ok_ = fail(error_, "Internal setup serializer produced an invalid key.");
            return false;
        }
        if (key.size() + value.size() + 1U > kMaximumLineBytes) {
            ok_ = fail(error_, record_error("Serialized setup line exceeds the signed-int limit at key", key));
            return false;
        }
        const std::size_t added = key.size() + value.size() + 2U;
        if (contents_.size() > kMaximumSetupBytes - added) {
            ok_ = fail(error_, "Serialized setup exceeds the signed-int format limit.");
            return false;
        }
        contents_.append(key);
        contents_.push_back('\t');
        contents_.append(value);
        contents_.push_back('\n');
        return true;
    }

    template <typename Integer>
    bool add_integer(std::string_view key, Integer value) {
        static_assert(std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>);
        std::array<char, 64U> buffer{};
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 10);
        if (result.ec != std::errc{}) {
            ok_ = fail(error_, record_error("Could not serialize integer setup key", key));
            return false;
        }
        return add(key, std::string_view(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())));
    }

    bool add_double(std::string_view key, double value) {
        if (!std::isfinite(value)) {
            ok_ = fail(error_, record_error("Cannot serialize non-finite setup value at key", key));
            return false;
        }
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
        if (!stream) {
            ok_ = fail(error_, record_error("Could not serialize numeric setup key", key));
            return false;
        }
        return add(key, stream.str());
    }

    bool add_bool(std::string_view key, bool value) {
        return add(key, value ? "1" : "0");
    }

    bool add_string(std::string_view key, const std::string& value) {
        std::string encoded;
        if (!percent_encode(value, encoded)) {
            ok_ = fail(error_, record_error("String exceeds the signed-int setup limit at key", key));
            return false;
        }
        return add(key, encoded);
    }

    template <typename Enum, std::size_t Count>
    bool add_enum(std::string_view key,
                  Enum value,
                  const std::array<std::pair<std::string_view, Enum>, Count>& values) {
        std::string_view token;
        if (!enum_token(value, values, token)) {
            ok_ = fail(error_, record_error("Cannot serialize unknown enum value at key", key));
            return false;
        }
        return add(key, token);
    }

    bool ok() const { return ok_; }
    const std::string& contents() const { return contents_; }

private:
    std::string* error_ = nullptr;
    std::string contents_;
    bool ok_ = true;
};

void add_path_binding_records(SetupBuilder& builder,
                              std::string_view prefix,
                              const PathBinding& binding) {
    const auto key = [prefix](std::string_view suffix) {
        std::string result(prefix);
        result.append(suffix);
        return result;
    };
    builder.add_bool(key("enabled"), binding.enabled);
    builder.add_integer(key("path_id"), binding.path_id);
    builder.add_bool(key("synchronized"), binding.synchronized);
    builder.add_integer(key("cycles_per_loop"), binding.cycles_per_loop);
    builder.add_double(key("phase_degrees"), binding.phase_degrees);
    builder.add_bool(key("reverse"), binding.reverse);
    builder.add_double(key("offset_x"), binding.offset_x);
    builder.add_double(key("offset_y"), binding.offset_y);
    builder.add_bool(key("follow_tangent"), binding.follow_tangent);
}

void add_audio_input_records(SetupBuilder& builder, std::string_view prefix,
                             const AudioInputProcessingConfig& processing) {
    const auto key = [prefix](std::string_view suffix) {
        std::string result(prefix);
        result.append(suffix);
        return result;
    };
    builder.add_bool(key("high_pass_enabled"), processing.high_pass_enabled);
    builder.add_double(key("high_pass_hz"), processing.high_pass_hz);
    builder.add_bool(key("low_pass_enabled"), processing.low_pass_enabled);
    builder.add_double(key("low_pass_hz"), processing.low_pass_hz);
    builder.add_bool(key("equalizer_enabled"), processing.equalizer_enabled);
    const std::string equalizer = key("equalizer_bands");
    builder.add_integer(equalizer + ".count",
                        processing.equalizer_bands.size());
    for (std::size_t index = 0U;
         index < processing.equalizer_bands.size(); ++index) {
        const auto& band = processing.equalizer_bands[index];
        builder.add_double(indexed_key(equalizer, index, "frequency_hz"),
                           band.frequency_hz);
        builder.add_double(indexed_key(equalizer, index, "gain_db"),
                           band.gain_db);
    }
    const std::string streams = key("frequency_streams");
    builder.add_integer(streams + ".count",
                        processing.frequency_streams.size());
    for (std::size_t index = 0U;
         index < processing.frequency_streams.size(); ++index) {
        const auto& stream = processing.frequency_streams[index];
        builder.add_string(indexed_key(streams, index, "uuid"), stream.uuid);
        builder.add_string(indexed_key(streams, index, "name"), stream.name);
        builder.add_double(indexed_key(streams, index, "low_hz"),
                           stream.low_hz);
        builder.add_double(indexed_key(streams, index, "high_hz"),
                           stream.high_hz);
    }
}

void add_music_records(SetupBuilder& builder, std::string_view prefix,
                       const MusicAnalysis& music) {
    const auto key = [prefix](std::string_view suffix) {
        std::string result(prefix);
        result.append(suffix);
        return result;
    };
    builder.add_integer(key("schema_version"), music.schema_version);
    builder.add_string(key("analyzer_version"), music.analyzer_version);
    builder.add_string(key("source_sha256"), music.source_sha256);
    builder.add_string(key("source_basename"), music.source_basename);
    builder.add_string(key("source_format"), music.source_format);
    builder.add_integer(key("source_frame_count"), music.source_frame_count);
    builder.add_integer(key("source_sample_rate"), music.source_sample_rate);
    builder.add_integer(key("source_channel_count"), music.source_channel_count);
    builder.add_double(key("duration_seconds"), music.duration_seconds);
    builder.add_double(key("detected_bpm"), music.detected_bpm);
    builder.add_double(key("tempo_confidence"), music.tempo_confidence);
    const std::string beats = key("beat_times");
    builder.add_integer(beats + ".count", music.beat_times_seconds.size());
    for (std::size_t index = 0U; index < music.beat_times_seconds.size(); ++index) {
        builder.add_double(indexed_key(beats, index, "seconds"),
                           music.beat_times_seconds[index]);
    }
    const std::string tempos = key("tempo_points");
    builder.add_integer(tempos + ".count", music.tempo_points.size());
    for (std::size_t index = 0U; index < music.tempo_points.size(); ++index) {
        const MusicTempoPoint& point = music.tempo_points[index];
        builder.add_double(indexed_key(tempos, index, "time_seconds"),
                           point.time_seconds);
        builder.add_double(indexed_key(tempos, index, "bpm"), point.bpm);
        builder.add_double(indexed_key(tempos, index, "confidence"),
                           point.confidence);
    }
    const std::string samples = key("feature_samples");
    builder.add_integer(samples + ".count", music.feature_samples.size());
    for (std::size_t index = 0U; index < music.feature_samples.size(); ++index) {
        const MusicFeatureSample& sample = music.feature_samples[index];
        builder.add_double(indexed_key(samples, index, "energy"), sample.energy);
        builder.add_double(indexed_key(samples, index, "bass"), sample.bass);
        builder.add_double(indexed_key(samples, index, "midrange"), sample.midrange);
        builder.add_double(indexed_key(samples, index, "treble"), sample.treble);
        builder.add_double(indexed_key(samples, index, "onset"), sample.onset);
        builder.add_double(indexed_key(samples, index, "beat"), sample.beat);
        builder.add_double(indexed_key(samples, index, "spectral_centroid"),
                           sample.spectral_centroid);
        builder.add_double(indexed_key(samples, index, "spectral_flatness"),
                           sample.spectral_flatness);
        builder.add_double(indexed_key(samples, index, "chroma_hue"),
                           sample.chroma_hue);
        builder.add_double(indexed_key(samples, index, "chroma_strength"),
                           sample.chroma_strength);
    }
    const std::string streams = key("frequency_streams");
    builder.add_integer(streams + ".count", music.frequency_streams.size());
    for (std::size_t stream_index = 0U;
         stream_index < music.frequency_streams.size(); ++stream_index) {
        const auto& stream = music.frequency_streams[stream_index];
        const std::string item = indexed_key(streams, stream_index, "analysis");
        builder.add_string(indexed_key(streams, stream_index, "uuid"),
                           stream.uuid);
        builder.add_double(indexed_key(streams, stream_index, "low_hz"),
                           stream.low_hz);
        builder.add_double(indexed_key(streams, stream_index, "high_hz"),
                           stream.high_hz);
        builder.add_double(item + ".detected_bpm", stream.detected_bpm);
        builder.add_double(item + ".tempo_confidence",
                           stream.tempo_confidence);
        builder.add_integer(item + ".beat_times.count",
                            stream.beat_times_seconds.size());
        for (std::size_t index = 0U;
             index < stream.beat_times_seconds.size(); ++index) {
            builder.add_double(indexed_key(item + ".beat_times", index,
                                           "seconds"),
                               stream.beat_times_seconds[index]);
        }
        builder.add_integer(item + ".tempo_points.count",
                            stream.tempo_points.size());
        for (std::size_t index = 0U; index < stream.tempo_points.size(); ++index) {
            const auto& point = stream.tempo_points[index];
            builder.add_double(indexed_key(item + ".tempo_points", index,
                                           "time_seconds"),
                               point.time_seconds);
            builder.add_double(indexed_key(item + ".tempo_points", index,
                                           "bpm"), point.bpm);
            builder.add_double(indexed_key(item + ".tempo_points", index,
                                           "confidence"), point.confidence);
        }
        builder.add_integer(item + ".feature_samples.count",
                            stream.feature_samples.size());
        for (std::size_t index = 0U;
             index < stream.feature_samples.size(); ++index) {
            const auto& sample = stream.feature_samples[index];
            const std::string sample_key = indexed_key(
                item + ".feature_samples", index, "sample");
            builder.add_double(sample_key + ".energy", sample.energy);
            builder.add_double(sample_key + ".bass", sample.bass);
            builder.add_double(sample_key + ".midrange", sample.midrange);
            builder.add_double(sample_key + ".treble", sample.treble);
            builder.add_double(sample_key + ".onset", sample.onset);
            builder.add_double(sample_key + ".beat", sample.beat);
            builder.add_double(sample_key + ".spectral_centroid",
                               sample.spectral_centroid);
            builder.add_double(sample_key + ".spectral_flatness",
                               sample.spectral_flatness);
            builder.add_double(sample_key + ".chroma_hue", sample.chroma_hue);
            builder.add_double(sample_key + ".chroma_strength",
                               sample.chroma_strength);
        }
    }
    add_audio_input_records(builder, key("input_processing."),
                            music.input_processing);
}

void add_clock_records(SetupBuilder& builder, std::string_view prefix,
                       std::string_view music_prefix,
                       const ClockConfig& clock) {
    const auto key = [prefix](std::string_view suffix) {
        std::string result(prefix);
        result.append(suffix);
        return result;
    };
    builder.add_enum(key("mode"), clock.mode, kClockModes);
    builder.add_enum(key("interpolation"), clock.interpolation,
                     kClockInterpolations);
    builder.add_enum(key("fit"), clock.fit, kClockFits);
    builder.add_integer(key("frame_interval"), clock.frame_interval);
    builder.add_integer(key("time_interval_microseconds"),
                        clock.time_interval_microseconds);
    builder.add_string(key("meter.expression"), clock.meter.expression);
    builder.add_double(key("meter.bpm"), clock.meter.bpm);
    builder.add_integer(key("meter.tempo_note_denominator"),
                        clock.meter.tempo_note_denominator);
    builder.add_enum(key("music_tempo"), clock.music_tempo, kMusicTempoModes);
    builder.add_enum(key("music_swing_policy"), clock.music_swing_policy,
                     kMusicSwingPolicies);
    builder.add_integer(key("beat_offset_microseconds"),
                        clock.beat_offset_microseconds);
    builder.add_double(key("phase_offset_degrees"),
                       clock.phase_offset_degrees);
    builder.add_bool(key("reverse"), clock.reverse);
    builder.add_bool(key("data_only"), clock.data_only);
    builder.add_string(key("frequency_stream_uuid"),
                       clock.frequency_stream_uuid);
    add_audio_input_records(builder, key("audio_input."),
                            clock.audio_processing);
    add_music_records(builder, music_prefix, clock.music);
}

void add_audio_reactive_records(SetupBuilder& builder,
                                std::string_view prefix,
                                const AudioReactiveConfig& audio) {
    const auto key = [prefix](std::string_view suffix) {
        std::string result(prefix);
        result.append(suffix);
        return result;
    };
    builder.add_bool(key("enabled"), audio.enabled);
    builder.add_bool(key("synchronized_only"), audio.synchronized_only);
    builder.add_bool(key("waves_enabled"), audio.waves_enabled);
    builder.add_enum(key("wave_source"), audio.wave_source, kMusicFeatures);
    builder.add_double(key("wave_amount"), audio.wave_amount);
    builder.add_bool(key("effects_enabled"), audio.effects_enabled);
    builder.add_enum(key("effect_source"), audio.effect_source,
                     kMusicFeatures);
    builder.add_double(key("effect_amount"), audio.effect_amount);
    builder.add_bool(key("color_enabled"), audio.color_enabled);
    builder.add_enum(key("color_source"), audio.color_source,
                     kMusicFeatures);
    builder.add_double(key("color_amount_degrees"),
                       audio.color_amount_degrees);
}

void add_live_records(SetupBuilder& builder, const LiveConfig& live) {
    builder.add_bool("live.enabled", live.enabled);
    builder.add_integer("live.endpoints.count", live.endpoints.size());
    for (std::size_t index = 0U; index < live.endpoints.size(); ++index) {
        const LiveEndpointConfig& endpoint = live.endpoints[index];
        builder.add_string(indexed_key("live.endpoints", index, "uuid"),
                           endpoint.uuid);
        builder.add_string(indexed_key("live.endpoints", index, "name"),
                           endpoint.name);
        builder.add_enum(indexed_key("live.endpoints", index, "protocol"),
                         endpoint.protocol, kLiveEndpointProtocols);
        builder.add_enum(indexed_key("live.endpoints", index, "direction"),
                         endpoint.direction, kLiveEndpointDirections);
        builder.add_integer(
            indexed_key("live.endpoints", index,
                        "input_latency_microseconds"),
            endpoint.input_latency_microseconds);
        builder.add_integer(
            indexed_key("live.endpoints", index,
                        "output_latency_microseconds"),
            endpoint.output_latency_microseconds);
    }

    builder.add_integer("live.mappings.count", live.mappings.size());
    for (std::size_t index = 0U; index < live.mappings.size(); ++index) {
        const LiveControlMapping& mapping = live.mappings[index];
        builder.add_bool(indexed_key("live.mappings", index, "enabled"),
                         mapping.enabled);
        builder.add_string(indexed_key("live.mappings", index, "name"),
                           mapping.name);
        builder.add_string(
            indexed_key("live.mappings", index, "endpoint_uuid"),
            mapping.endpoint_uuid);
        builder.add_enum(indexed_key("live.mappings", index, "input"),
                         mapping.input, kLiveControlInputs);
        builder.add_integer(
            indexed_key("live.mappings", index, "midi_channel"),
            mapping.midi_channel);
        builder.add_integer(
            indexed_key("live.mappings", index, "control_number"),
            mapping.control_number);
        builder.add_string(
            indexed_key("live.mappings", index, "osc_address"),
            mapping.osc_address);
        builder.add_enum(indexed_key("live.mappings", index, "target"),
                         mapping.target, kLiveMappingTargets);
        builder.add_string(
            indexed_key("live.mappings", index, "target_path"),
            mapping.target_path);
        builder.add_enum(indexed_key("live.mappings", index, "action"),
                         mapping.action, kLiveActions);
        builder.add_string(
            indexed_key("live.mappings", index, "scene_uuid"),
            mapping.scene_uuid);
        builder.add_enum(indexed_key("live.mappings", index, "mode"),
                         mapping.mode, kLiveMappingModes);
        builder.add_double(
            indexed_key("live.mappings", index, "input_minimum"),
            mapping.input_minimum);
        builder.add_double(
            indexed_key("live.mappings", index, "input_maximum"),
            mapping.input_maximum);
        builder.add_double(
            indexed_key("live.mappings", index, "output_minimum"),
            mapping.output_minimum);
        builder.add_double(
            indexed_key("live.mappings", index, "output_maximum"),
            mapping.output_maximum);
        builder.add_double(indexed_key("live.mappings", index, "curve"),
                           mapping.curve);
        builder.add_double(indexed_key("live.mappings", index, "dead_zone"),
                           mapping.dead_zone);
        builder.add_integer(
            indexed_key("live.mappings", index, "smoothing_milliseconds"),
            mapping.smoothing_milliseconds);
    }

    builder.add_integer("live.clock_inputs.count", live.clock_inputs.size());
    for (std::size_t index = 0U; index < live.clock_inputs.size(); ++index) {
        const LiveClockInputConfig& clock = live.clock_inputs[index];
        builder.add_bool(
            indexed_key("live.clock_inputs", index, "enabled"),
            clock.enabled);
        builder.add_enum(indexed_key("live.clock_inputs", index, "target"),
                         clock.target, kLiveClockTargets);
        builder.add_string(
            indexed_key("live.clock_inputs", index, "layer_uuid"),
            clock.layer_uuid);
        builder.add_enum(indexed_key("live.clock_inputs", index, "source"),
                         clock.source, kLiveClockInputSources);
        builder.add_string(
            indexed_key("live.clock_inputs", index, "endpoint_uuid"),
            clock.endpoint_uuid);
        builder.add_integer(
            indexed_key("live.clock_inputs", index, "audio_channel"),
            clock.audio_channel);
        builder.add_string(
            indexed_key("live.clock_inputs", index,
                        "frequency_stream_uuid"),
            clock.frequency_stream_uuid);
        builder.add_bool(
            indexed_key("live.clock_inputs", index,
                        "follow_midi_transport"),
            clock.follow_midi_transport);
        builder.add_integer(
            indexed_key("live.clock_inputs", index,
                        "holdover_milliseconds"),
            clock.holdover_milliseconds);
    }

    builder.add_integer("live.midi_clock_outputs.count",
                        live.midi_clock_outputs.size());
    for (std::size_t index = 0U;
         index < live.midi_clock_outputs.size(); ++index) {
        const LiveMidiClockOutputConfig& output =
            live.midi_clock_outputs[index];
        builder.add_bool(
            indexed_key("live.midi_clock_outputs", index, "enabled"),
            output.enabled);
        builder.add_enum(
            indexed_key("live.midi_clock_outputs", index, "source"),
            output.source, kLiveClockTargets);
        builder.add_string(
            indexed_key("live.midi_clock_outputs", index, "layer_uuid"),
            output.layer_uuid);
        builder.add_string(
            indexed_key("live.midi_clock_outputs", index, "endpoint_uuid"),
            output.endpoint_uuid);
        builder.add_bool(
            indexed_key("live.midi_clock_outputs", index,
                        "send_transport"),
            output.send_transport);
        builder.add_bool(
            indexed_key("live.midi_clock_outputs", index,
                        "send_song_position"),
            output.send_song_position);
    }

    builder.add_integer("live.scenes.count", live.scenes.size());
    for (std::size_t index = 0U; index < live.scenes.size(); ++index) {
        const LiveSceneConfig& scene = live.scenes[index];
        builder.add_string(indexed_key("live.scenes", index, "uuid"),
                           scene.uuid);
        builder.add_string(indexed_key("live.scenes", index, "name"),
                           scene.name);
        builder.add_integer(
            indexed_key("live.scenes", index, "transition_milliseconds"),
            scene.transition_milliseconds);
        const std::string values =
            indexed_key("live.scenes", index, "values");
        builder.add_integer(values + ".count", scene.values.size());
        for (std::size_t value_index = 0U;
             value_index < scene.values.size(); ++value_index) {
            const LiveSceneValue& value = scene.values[value_index];
            builder.add_string(indexed_key(values, value_index, "target_path"),
                               value.target_path);
            builder.add_enum(indexed_key(values, value_index, "type"),
                             value.type, kLiveSceneValueTypes);
            builder.add_string(indexed_key(values, value_index, "value"),
                               value.value);
        }
    }
    builder.add_string("live.startup_scene_uuid", live.startup_scene_uuid);
    builder.add_bool("live.output.fullscreen", live.output.fullscreen);
    builder.add_bool("live.output.prefer_secondary_display",
                     live.output.prefer_secondary_display);
    builder.add_bool("live.output.hide_cursor", live.output.hide_cursor);
    builder.add_enum("live.safety.dropout_behavior",
                     live.safety.dropout_behavior, kLiveDropoutBehaviors);
    builder.add_bool("live.safety.frame_time_watchdog_enabled",
                     live.safety.frame_time_watchdog_enabled);
    builder.add_integer("live.safety.watchdog_timeout_milliseconds",
                        live.safety.watchdog_timeout_milliseconds);
    builder.add_integer("live.safety.audio_dropout_grace_milliseconds",
                        live.safety.audio_dropout_grace_milliseconds);
    builder.add_integer(
        "live.safety.last_good_frame_timeout_milliseconds",
        live.safety.last_good_frame_timeout_milliseconds);
    builder.add_bool("live.safety.prevent_device_sleep",
                     live.safety.prevent_device_sleep);
    add_audio_input_records(builder, "live.audio_input.",
                            live.audio_processing);
}

bool validate_persistence_bounds(const RenderConfig& config,
                                 std::string* error) {
    const MusicAnalysis& music = config.clock.music;
    const MusicAnalysis& layer_music = config.layer_clock.clock.music;
    if (config.clock.meter.expression.size() > kMaximumMeterExpressionBytes
        || config.layer_clock.clock.meter.expression.size()
               > kMaximumMeterExpressionBytes) {
        return fail(error, "Cannot save configuration: the meter expression exceeds the signed-int text limit.");
    }
    const auto oversized_music_text = [](const MusicAnalysis& candidate) {
        return candidate.analyzer_version.size() > kMaximumAnalyzerVersionBytes
               || candidate.source_basename.size() > kMaximumMusicBasenameBytes
               || candidate.source_format.size() > kMaximumMusicFormatBytes;
    };
    if (oversized_music_text(music) || oversized_music_text(layer_music)
    ) {
        return fail(error, "Cannot save configuration: music source metadata exceeds its field limit.");
    }
    if (!is_lowercase_sha256(music.source_sha256)
        || !is_lowercase_sha256(layer_music.source_sha256)) {
        return fail(error,
                    "Cannot save configuration: the music source digest must be empty or 64 lowercase hexadecimal characters.");
    }
    if (!is_lowercase_sha256(config.surface.obj_sha256)
        || config.surface.obj_basename.size()
               > kMaximumAttachmentBasenameBytes) {
        return fail(error,
                    "Cannot save configuration: custom OBJ attachment metadata is invalid.");
    }
    if (!is_lowercase_sha256(config.surface.environment_map.sha256)
        || config.surface.environment_map.basename.size()
               > kMaximumAttachmentBasenameBytes) {
        return fail(
            error,
            "Cannot save configuration: environment-map attachment metadata is invalid.");
    }
    if (!is_lowercase_sha256(config.starting_image.sha256)
        || config.starting_image.basename.size()
               > kMaximumAttachmentBasenameBytes) {
        return fail(error,
                    "Cannot save configuration: starting-image attachment metadata is invalid.");
    }
    const auto oversized_music_collections = [](const MusicAnalysis& candidate) {
        return candidate.beat_times_seconds.size() > kMaximumMusicBeats
               || candidate.tempo_points.size() > kMaximumMusicTempoPoints
               || candidate.feature_samples.size() > kMaximumMusicFeatureSamples;
    };
    if (oversized_music_collections(music)
        || oversized_music_collections(layer_music)
    ) {
        return fail(error,
                    "Cannot save configuration: cached music analysis exceeds a public collection maximum.");
    }
    return true;
}

bool serialize_setup(const RenderConfig& config,
                     std::string& serialized,
                     std::string* error,
                     bool enforce_particle_workload = true) {
    const ValidationResult validation = enforce_particle_workload
        ? validate(config)
        : detail::validate_render_config_structure(config);
    if (!validation.ok) {
        return fail(error, "Cannot save invalid configuration: " + validation.message);
    }
    if (config.waves.size() > kMaximumWaves
        || config.swings.size() > kMaximumSwings
        || config.effects.size() > kMaximumEffects
        || config.parameter_lfos.size() > kMaximumParameterLfos) {
        return fail(error, "Cannot save configuration: a collection exceeds its public maximum.");
    }
    if (!validate_persistence_bounds(config, error)) {
        return false;
    }

    SetupBuilder builder(error);
    builder.add_integer("canvas.width", config.width);
    builder.add_integer("canvas.height", config.height);
    builder.add_integer("canvas.block_size", config.block_size);
    builder.add_integer("timing.total_frames", config.total_frames);
    builder.add_double("timing.fps", config.fps);

    builder.add_enum("timing.clock.mode", config.clock.mode, kClockModes);
    builder.add_enum("timing.clock.interpolation", config.clock.interpolation,
                     kClockInterpolations);
    builder.add_enum("timing.clock.fit", config.clock.fit, kClockFits);
    builder.add_integer("timing.clock.frame_interval",
                        config.clock.frame_interval);
    builder.add_integer("timing.clock.time_interval_microseconds",
                        config.clock.time_interval_microseconds);
    builder.add_string("timing.clock.meter.expression",
                       config.clock.meter.expression);
    builder.add_double("timing.clock.meter.bpm", config.clock.meter.bpm);
    builder.add_integer("timing.clock.meter.tempo_note_denominator",
                        config.clock.meter.tempo_note_denominator);
    builder.add_enum("timing.clock.music_tempo", config.clock.music_tempo,
                     kMusicTempoModes);
    builder.add_enum("timing.clock.music_swing_policy",
                     config.clock.music_swing_policy, kMusicSwingPolicies);
    builder.add_integer("timing.clock.beat_offset_microseconds",
                        config.clock.beat_offset_microseconds);
    builder.add_double("timing.clock.phase_offset_degrees",
                       config.clock.phase_offset_degrees);
    builder.add_bool("timing.clock.reverse", config.clock.reverse);
    builder.add_bool("timing.clock.data_only", config.clock.data_only);
    builder.add_string("timing.clock.frequency_stream_uuid",
                       config.clock.frequency_stream_uuid);
    add_audio_input_records(builder, "timing.clock.audio_input.",
                            config.clock.audio_processing);
    add_audio_reactive_records(builder, "audio_response_defaults.",
                               config.audio_reactive_defaults);
    add_live_records(builder, config.live);

    builder.add_integer("paths.count", config.motion_paths.size());
    for (std::size_t path_index = 0U;
         path_index < config.motion_paths.size(); ++path_index) {
        const CubicMotionPath& path = config.motion_paths[path_index];
        builder.add_integer(indexed_key("paths", path_index, "id"), path.id);
        builder.add_string(indexed_key("paths", path_index, "name"), path.name);
        const std::string nodes = indexed_key("paths", path_index, "nodes");
        builder.add_integer(nodes + ".count", path.nodes.size());
        for (std::size_t node_index = 0U;
             node_index < path.nodes.size(); ++node_index) {
            const CubicPathNode& node = path.nodes[node_index];
            builder.add_integer(indexed_key(nodes, node_index, "id"), node.id);
            builder.add_double(indexed_key(nodes, node_index, "x"), node.x);
            builder.add_double(indexed_key(nodes, node_index, "y"), node.y);
            builder.add_double(indexed_key(nodes, node_index, "in_x"), node.in_x);
            builder.add_double(indexed_key(nodes, node_index, "in_y"), node.in_y);
            builder.add_double(indexed_key(nodes, node_index, "out_x"), node.out_x);
            builder.add_double(indexed_key(nodes, node_index, "out_y"), node.out_y);
            builder.add_enum(indexed_key(nodes, node_index, "handle_mode"),
                             node.handle_mode, kPathHandleModes);
        }
    }

    add_music_records(builder, "timing.music.", config.clock.music);

    builder.add_bool("layer_clock.enabled", config.layer_clock.enabled);
    builder.add_enum("layer_clock.scale", config.layer_clock.scale,
                     kLayerClockScales);
    builder.add_enum("layer_clock.mix", config.layer_clock.mix,
                     kLayerClockMixModes);
    builder.add_bool("layer_clock.mix_enabled",
                     config.layer_clock.mix_enabled);
    add_clock_records(builder, "layer_clock.clock.", "layer_clock.music.",
                      config.layer_clock.clock);

    builder.add_integer("waves.count", config.waves.size());
    for (std::size_t index = 0; index < config.waves.size(); ++index) {
        const WaveConfig& wave = config.waves[index];
        builder.add_integer(indexed_key("waves", index, "id"), wave.id);
        builder.add_string(indexed_key("waves", index, "name"), wave.name);
        builder.add_bool(indexed_key("waves", index, "enabled"), wave.enabled);
        builder.add_bool(indexed_key("waves", index, "synchronized"), wave.synchronized);
        builder.add_enum(indexed_key("waves", index, "audio_response"),
                         wave.audio_response, kAudioResponseModes);
        builder.add_double(indexed_key("waves", index, "x_percent"), wave.x_percent);
        builder.add_double(indexed_key("waves", index, "y_percent"), wave.y_percent);
        builder.add_double(indexed_key("waves", index, "amplitude"), wave.amplitude);
        builder.add_double(indexed_key("waves", index, "spatial_frequency"), wave.spatial_frequency);
        builder.add_integer(indexed_key("waves", index, "cycles_per_loop"), wave.cycles_per_loop);
        builder.add_double(indexed_key("waves", index, "phase_degrees"), wave.phase_degrees);
        builder.add_double(indexed_key("waves", index, "direction"), wave.direction);
        add_path_binding_records(
            builder, indexed_key("waves", index, "path") + ".", wave.path);
    }

    builder.add_integer("swings.count", config.swings.size());
    for (std::size_t index = 0; index < config.swings.size(); ++index) {
        const SwingConfig& swing = config.swings[index];
        builder.add_integer(indexed_key("swings", index, "id"), swing.id);
        builder.add_string(indexed_key("swings", index, "name"), swing.name);
        builder.add_bool(indexed_key("swings", index, "enabled"), swing.enabled);
        builder.add_enum(indexed_key("swings", index, "waveform"), swing.waveform, kWaveforms);
        builder.add_double(indexed_key("swings", index, "amount"), swing.amount);
        builder.add_integer(indexed_key("swings", index, "cycles_per_loop"), swing.cycles_per_loop);
        builder.add_double(indexed_key("swings", index, "phase_degrees"), swing.phase_degrees);
        builder.add_double(indexed_key("swings", index, "shape"), swing.shape);
        builder.add_double(indexed_key("swings", index, "center_x"), swing.center_x);
        builder.add_double(indexed_key("swings", index, "center_y"), swing.center_y);
        builder.add_double(indexed_key("swings", index, "radius"), swing.radius);
    }

    builder.add_integer("effects.count", config.effects.size());
    for (std::size_t index = 0; index < config.effects.size(); ++index) {
        const EffectConfig& effect = config.effects[index];
        builder.add_integer(indexed_key("effects", index, "id"), effect.id);
        builder.add_string(indexed_key("effects", index, "name"), effect.name);
        builder.add_enum(indexed_key("effects", index, "type"), effect.type, kEffectTypes);
        builder.add_enum(indexed_key("effects", index, "space"), effect.space, kEffectSpaces);
        builder.add_bool(indexed_key("effects", index, "enabled"), effect.enabled);
        builder.add_bool(indexed_key("effects", index, "synchronized"), effect.synchronized);
        builder.add_enum(indexed_key("effects", index, "audio_response"),
                         effect.audio_response, kAudioResponseModes);
        builder.add_integer(indexed_key("effects", index, "cycles_per_loop"), effect.cycles_per_loop);
        builder.add_double(indexed_key("effects", index, "phase_degrees"), effect.phase_degrees);
        builder.add_enum(indexed_key("effects", index, "edge_mode"), effect.edge_mode, kEdgeModes);
        builder.add_double(indexed_key("effects", index, "intensity"), effect.intensity);
        builder.add_double(indexed_key("effects", index, "magnitude"), effect.magnitude);
        builder.add_double(indexed_key("effects", index, "frequency"), effect.frequency);
        builder.add_double(indexed_key("effects", index, "secondary"), effect.secondary);
        builder.add_double(indexed_key("effects", index, "center_x"), effect.center_x);
        builder.add_double(indexed_key("effects", index, "center_y"), effect.center_y);
        builder.add_double(indexed_key("effects", index, "angle_degrees"), effect.angle_degrees);
        builder.add_double(indexed_key("effects", index, "radius_pixels"), effect.radius_pixels);
        builder.add_double(indexed_key("effects", index, "threshold"), effect.threshold);
        builder.add_double(indexed_key("effects", index, "soft_knee"), effect.soft_knee);
        builder.add_double(indexed_key("effects", index, "area_radius"), effect.area_radius);
        builder.add_enum(indexed_key("effects", index, "blur_type"),
                         effect.blur_type, kBlurTypes);
        builder.add_enum(indexed_key("effects", index, "particle_shape"),
                         effect.particle_shape, kParticleShapes);
        builder.add_enum(indexed_key("effects", index, "particle_profile"),
                         effect.particle_profile, kParticleProfiles);
        builder.add_double(
            indexed_key("effects", index, "particle_size_variation"),
            effect.particle_size_variation);
        builder.add_double(indexed_key("effects", index, "particle_definition"),
                           effect.particle_definition);
        builder.add_double(indexed_key("effects", index, "particle_twinkle"),
                           effect.particle_twinkle);
        builder.add_integer(indexed_key("effects", index, "particle_seed"),
                            effect.particle_seed);
        builder.add_enum(indexed_key("effects", index, "particle_orientation"),
                         effect.particle_orientation, kParticleOrientations);
        builder.add_double(
            indexed_key("effects", index, "particle_rotation_degrees"),
            effect.particle_rotation_degrees);
        builder.add_integer(indexed_key("effects", index, "blur_passes"),
                            effect.blur_passes);
        builder.add_integer(indexed_key("effects", index, "blur_samples"),
                            effect.blur_samples);
        builder.add_double(indexed_key("effects", index, "blur_minimum"),
                           effect.blur_minimum);
        builder.add_double(indexed_key("effects", index, "blur_maximum"),
                           effect.blur_maximum);
        builder.add_integer(
            indexed_key("effects", index, "blur_pulses_per_cycle"),
            effect.blur_pulses_per_cycle);
        add_path_binding_records(
            builder, indexed_key("effects", index, "path") + ".", effect.path);
    }

    builder.add_integer("parameter_lfos.count",
                        config.parameter_lfos.size());
    for (std::size_t index = 0U;
         index < config.parameter_lfos.size(); ++index) {
        const ParameterLfo& lfo = config.parameter_lfos[index];
        builder.add_bool(indexed_key("parameter_lfos", index, "enabled"),
                         lfo.enabled);
        builder.add_string(
            indexed_key("parameter_lfos", index, "target_path"),
            lfo.target_path);
        builder.add_enum(indexed_key("parameter_lfos", index, "waveform"),
                         lfo.waveform, kWaveforms);
        builder.add_double(indexed_key("parameter_lfos", index, "minimum"),
                           lfo.minimum);
        builder.add_double(indexed_key("parameter_lfos", index, "maximum"),
                           lfo.maximum);
        builder.add_integer(
            indexed_key("parameter_lfos", index, "cycles_per_loop"),
            lfo.cycles_per_loop);
        builder.add_double(
            indexed_key("parameter_lfos", index, "phase_degrees"),
            lfo.phase_degrees);
        builder.add_double(indexed_key("parameter_lfos", index, "shape"),
                           lfo.shape);
    }

    builder.add_bool("rhythm.swings_enabled", config.swings_enabled);
    builder.add_double("rhythm.phrase_warp", config.phrase_warp);
    builder.add_double("rhythm.ghost_mix", config.ghost_mix);
    builder.add_double("rhythm.ghost_lag_degrees", config.ghost_lag_degrees);

    builder.add_bool("audio_reactive.override_enabled",
                     config.audio_reactive_override_enabled);
    add_audio_reactive_records(builder, "audio_reactive.",
                               config.audio_reactive);

    builder.add_bool("appearance.displacement_enabled", config.displacement_enabled);
    builder.add_double("appearance.displacement", config.displacement);
    builder.add_bool("appearance.lighting_enabled", config.lighting_enabled);
    builder.add_double("appearance.wave_depth", config.wave_depth);
    builder.add_bool("appearance.spiral_enabled", config.spiral_enabled);
    builder.add_double("appearance.spiral_frequency", config.spiral_frequency);
    builder.add_integer("appearance.spiral_arms", config.spiral_arms);
    builder.add_bool("appearance.wall_reflection_enabled", config.wall_reflection_enabled);
    builder.add_double("appearance.wall_frequency", config.wall_frequency);
    builder.add_double("appearance.wall_mix", config.wall_mix);
    builder.add_integer("appearance.hue_cycles", config.hue_cycles);
    builder.add_double("appearance.saturation", config.saturation);

    builder.add_bool("alpha.enabled", config.alpha.enabled);
    builder.add_double("alpha.minimum", config.alpha.minimum);
    builder.add_double("alpha.maximum", config.alpha.maximum);
    builder.add_double("alpha.spatial_frequency", config.alpha.spatial_frequency);
    builder.add_integer("alpha.cycles_per_loop", config.alpha.cycles_per_loop);
    builder.add_double("alpha.phase_degrees", config.alpha.phase_degrees);
    builder.add_bool("alpha.use_source_alpha", config.alpha.use_source_alpha);

    builder.add_enum("starting_colors.mode", config.starting_colors.mode,
                     kStartingColorModes);
    builder.add_bool("starting_colors.include_alpha",
                     config.starting_colors.include_alpha);
    builder.add_integer("starting_colors.red_steps",
                        config.starting_colors.red_steps);
    builder.add_integer("starting_colors.green_steps",
                        config.starting_colors.green_steps);
    builder.add_integer("starting_colors.blue_steps",
                        config.starting_colors.blue_steps);
    builder.add_integer("starting_colors.alpha_steps",
                        config.starting_colors.alpha_steps);
    builder.add_double("starting_colors.red_minimum",
                       config.starting_colors.red_minimum);
    builder.add_double("starting_colors.red_maximum",
                       config.starting_colors.red_maximum);
    builder.add_double("starting_colors.green_minimum",
                       config.starting_colors.green_minimum);
    builder.add_double("starting_colors.green_maximum",
                       config.starting_colors.green_maximum);
    builder.add_double("starting_colors.blue_minimum",
                       config.starting_colors.blue_minimum);
    builder.add_double("starting_colors.blue_maximum",
                       config.starting_colors.blue_maximum);
    builder.add_double("starting_colors.alpha_minimum",
                       config.starting_colors.alpha_minimum);
    builder.add_double("starting_colors.alpha_maximum",
                       config.starting_colors.alpha_maximum);
    builder.add_bool("starting_colors.kaleidoscope.enabled",
                     config.starting_colors.kaleidoscope.enabled);
    builder.add_integer("starting_colors.kaleidoscope.mirrored_segments",
                        config.starting_colors.kaleidoscope.mirrored_segments);
    builder.add_double("starting_colors.kaleidoscope.rotation_degrees",
                       config.starting_colors.kaleidoscope.rotation_degrees);
    builder.add_double("starting_colors.kaleidoscope.mix",
                       config.starting_colors.kaleidoscope.mix);
    builder.add_bool("starting_colors.domain_warp.enabled",
                     config.starting_colors.domain_warp.enabled);
    builder.add_double("starting_colors.domain_warp.strength",
                       config.starting_colors.domain_warp.strength);
    builder.add_double("starting_colors.domain_warp.scale",
                       config.starting_colors.domain_warp.scale);
    builder.add_integer("starting_colors.domain_warp.octaves",
                        config.starting_colors.domain_warp.octaves);
    builder.add_integer("starting_colors.domain_warp.cycles_per_loop",
                        config.starting_colors.domain_warp.cycles_per_loop);
    builder.add_integer("starting_colors.domain_warp.seed",
                        config.starting_colors.domain_warp.seed);

    builder.add_bool("quantization.enabled", config.quantization.enabled);
    builder.add_integer("quantization.levels", config.quantization.levels);
    builder.add_double("quantization.mix", config.quantization.mix);
    builder.add_enum("quantization.mode", config.quantization.mode, kQuantizationModes);

    builder.add_bool("post_process.invert_rgb_enabled",
                     config.post_process.invert_rgb_enabled);
    builder.add_double("post_process.invert_rgb_mix",
                       config.post_process.invert_rgb_mix);
    builder.add_bool("post_process.invert_red_enabled",
                     config.post_process.invert_red_enabled);
    builder.add_double("post_process.invert_red_mix",
                       config.post_process.invert_red_mix);
    builder.add_bool("post_process.invert_green_enabled",
                     config.post_process.invert_green_enabled);
    builder.add_double("post_process.invert_green_mix",
                       config.post_process.invert_green_mix);
    builder.add_bool("post_process.invert_blue_enabled",
                     config.post_process.invert_blue_enabled);
    builder.add_double("post_process.invert_blue_mix",
                       config.post_process.invert_blue_mix);
    builder.add_bool("post_process.invert_alpha_enabled",
                     config.post_process.invert_alpha_enabled);
    builder.add_double("post_process.invert_alpha_mix",
                       config.post_process.invert_alpha_mix);
    builder.add_bool("post_process.channel_map.enabled",
                     config.post_process.channel_map.enabled);
    builder.add_double("post_process.channel_map.mix",
                       config.post_process.channel_map.mix);
    builder.add_enum("post_process.channel_map.red_source",
                     config.post_process.channel_map.red_source,
                     kChannelSources);
    builder.add_enum("post_process.channel_map.green_source",
                     config.post_process.channel_map.green_source,
                     kChannelSources);
    builder.add_enum("post_process.channel_map.blue_source",
                     config.post_process.channel_map.blue_source,
                     kChannelSources);
    builder.add_enum("post_process.channel_map.alpha_source",
                     config.post_process.channel_map.alpha_source,
                     kChannelSources);
    builder.add_bool("post_process.antialias_enabled",
                     config.post_process.antialias_enabled);
    builder.add_double("post_process.antialias_strength",
                       config.post_process.antialias_strength);
    builder.add_double("post_process.antialias_threshold",
                       config.post_process.antialias_threshold);
    builder.add_integer("post_process.antialias_passes",
                        config.post_process.antialias_passes);
    builder.add_integer("post_process.order.count",
                        config.post_process.order.size());
    for (std::size_t index = 0U;
         index < config.post_process.order.size(); ++index) {
        builder.add_enum(indexed_key("post_process.order", index, "stage"),
                         config.post_process.order[index],
                         kPostProcessStages);
    }

    builder.add_bool("surface.enabled", config.surface.enabled);
    builder.add_enum("surface.mapping", config.surface.mapping, kSurfaceMappings);
    builder.add_enum("surface.projection", config.surface.projection,
                     kSurfaceProjections);
    builder.add_enum("surface.sizing", config.surface.sizing,
                     kSurfaceSizings);
    builder.add_enum("surface.outside", config.surface.outside,
                     kSurfaceOutsides);
    builder.add_enum("surface.rotation_order", config.surface.rotation_order,
                     kSurfaceRotationOrders);
    builder.add_integer("surface.rotation_x_turns_per_loop",
                        config.surface.rotation_x_turns_per_loop);
    builder.add_integer("surface.rotation_y_turns_per_loop",
                        config.surface.rotation_y_turns_per_loop);
    builder.add_integer("surface.rotation_z_turns_per_loop",
                        config.surface.rotation_z_turns_per_loop);
    builder.add_double("surface.rotation_x_degrees",
                       config.surface.rotation_x_degrees);
    builder.add_double("surface.rotation_y_degrees",
                       config.surface.rotation_y_degrees);
    builder.add_double("surface.rotation_z_degrees",
                       config.surface.rotation_z_degrees);
    builder.add_double("surface.size_percent", config.surface.size_percent);
    builder.add_double("surface.scale_x", config.surface.scale_x);
    builder.add_double("surface.scale_y", config.surface.scale_y);
    builder.add_double("surface.scale_z", config.surface.scale_z);
    builder.add_double("surface.position_x_percent",
                       config.surface.position_x_percent);
    builder.add_double("surface.position_y_percent",
                       config.surface.position_y_percent);
    builder.add_double("surface.position_z", config.surface.position_z);
    builder.add_double("surface.camera_distance",
                       config.surface.camera_distance);
    builder.add_double("surface.focal_length", config.surface.focal_length);
    builder.add_double("surface.curvature", config.surface.curvature);
    builder.add_double("surface.lighting", config.surface.lighting);
    builder.add_double("surface.light_direction_x",
                       config.surface.light_direction_x);
    builder.add_double("surface.light_direction_y",
                       config.surface.light_direction_y);
    builder.add_double("surface.light_direction_z",
                       config.surface.light_direction_z);
    builder.add_double("surface.light_ambient", config.surface.light_ambient);
    builder.add_double("surface.light_diffuse", config.surface.light_diffuse);
    builder.add_bool("surface.composite_backfaces",
                     config.surface.composite_backfaces);
    builder.add_bool("surface.normalize_obj", config.surface.normalize_obj);
    builder.add_string("surface.obj_path", config.surface.obj_path);
    builder.add_string("surface.obj_sha256", config.surface.obj_sha256);
    builder.add_string("surface.obj_basename", config.surface.obj_basename);
    const PlaneDisplacementConfig& plane =
        config.surface.plane_displacement;
    builder.add_bool("surface.plane_displacement.enabled", plane.enabled);
    builder.add_double("surface.plane_displacement.minimum", plane.minimum);
    builder.add_double("surface.plane_displacement.maximum", plane.maximum);
    builder.add_double("surface.plane_displacement.midpoint", plane.midpoint);
    builder.add_integer("surface.plane_displacement.pixels_per_node",
                        plane.pixels_per_node);
    builder.add_string("surface.plane_displacement.path", plane.path);
    builder.add_string("surface.plane_displacement.sha256", plane.sha256);
    builder.add_string("surface.plane_displacement.basename", plane.basename);
    const EnvironmentMapConfig& environment =
        config.surface.environment_map;
    builder.add_bool("surface.environment_map.enabled", environment.enabled);
    builder.add_enum("surface.environment_map.encoding", environment.encoding,
                     kEnvironmentMapEncodings);
    builder.add_double("surface.environment_map.rotation_degrees",
                       environment.rotation_degrees);
    builder.add_double("surface.environment_map.exposure_stops",
                       environment.exposure_stops);
    builder.add_double("surface.environment_map.intensity",
                       environment.intensity);
    builder.add_double("surface.environment_map.mix", environment.mix);
    builder.add_string("surface.environment_map.path", environment.path);
    builder.add_string("surface.environment_map.sha256", environment.sha256);
    builder.add_string("surface.environment_map.basename",
                       environment.basename);
    const MeshConstructionConfig& construction =
        config.surface.mesh_construction;
    builder.add_enum("surface.mesh_construction.mode", construction.mode,
                     kMeshConstructionModes);
    builder.add_enum("surface.mesh_construction.fragmentation",
                     construction.fragmentation, kMeshFragmentations);
    builder.add_integer("surface.mesh_construction.target_fragments",
                        construction.target_fragments);
    builder.add_integer("surface.mesh_construction.cycles_per_loop",
                        construction.cycles_per_loop);
    builder.add_double("surface.mesh_construction.phase_degrees",
                       construction.phase_degrees);
    builder.add_double("surface.mesh_construction.distance",
                       construction.distance);
    builder.add_double("surface.mesh_construction.rotation_degrees",
                       construction.rotation_degrees);
    builder.add_double("surface.mesh_construction.minimum_scale",
                       construction.minimum_scale);
    builder.add_double("surface.mesh_construction.stagger",
                       construction.stagger);
    builder.add_integer("surface.mesh_construction.seed", construction.seed);

    builder.add_bool("source_image.enabled", config.starting_image.enabled);
    builder.add_enum("source_image.fit", config.starting_image.fit,
                     kStartingImageFits);
    builder.add_string("source_image.path", config.starting_image.path);
    builder.add_string("source_image.sha256", config.starting_image.sha256);
    builder.add_string("source_image.basename", config.starting_image.basename);
    builder.add_bool("source_image.palette_dither_enabled",
                     config.starting_image.palette_dither_enabled);
    builder.add_enum("source_image.palette_dither_method",
                     config.starting_image.palette_dither_method,
                     kDitherMethods);

    builder.add_bool("palette.enabled", config.palette.enabled);
    builder.add_string("palette.name", config.palette.name);
    builder.add_integer("palette.columns", config.palette.columns);
    builder.add_integer("palette.colors.count", config.palette.colors.size());
    for (std::size_t index = 0; index < config.palette.colors.size(); ++index) {
        const PaletteColor& color = config.palette.colors[index];
        builder.add_double(indexed_key("palette.colors", index, "red"), color.red);
        builder.add_double(indexed_key("palette.colors", index, "green"), color.green);
        builder.add_double(indexed_key("palette.colors", index, "blue"), color.blue);
        builder.add_double(indexed_key("palette.colors", index, "alpha"), color.alpha);
        builder.add_string(indexed_key("palette.colors", index, "name"), color.name);
        builder.add_enum(indexed_key("palette.colors", index, "encoding"),
                         color.encoding, kPaletteColorEncodings);
    }

    builder.add_bool("transform.flip_horizontal", config.transform.flip_horizontal);
    builder.add_bool("transform.flip_vertical", config.transform.flip_vertical);
    builder.add_enum("transform.mirror", config.transform.mirror, kMirrorModes);

    builder.add_bool("motion.enabled", config.motion.enabled);
    builder.add_enum("motion.path", config.motion.path, kLayerMotionPaths);
    builder.add_double("motion.center_x", config.motion.center_x);
    builder.add_double("motion.center_y", config.motion.center_y);
    builder.add_double("motion.travel_x", config.motion.travel_x);
    builder.add_double("motion.travel_y", config.motion.travel_y);
    builder.add_integer("motion.cycles_x", config.motion.cycles_x);
    builder.add_integer("motion.cycles_y", config.motion.cycles_y);
    builder.add_double("motion.phase_degrees", config.motion.phase_degrees);
    builder.add_integer("motion.rotations_per_loop",
                        config.motion.rotations_per_loop);
    builder.add_double("motion.rotation_offset_degrees",
                       config.motion.rotation_offset_degrees);
    builder.add_double("motion.scale_pulse", config.motion.scale_pulse);
    add_path_binding_records(builder, "motion.custom_path.",
                             config.motion.custom_path);

    builder.add_integer("output.bit_depth", config.output.bit_depth);
    builder.add_integer("output.png_compression_level",
                        config.output.png_compression_level);
    builder.add_bool("output.dither_enabled",
                     config.output.bit_depth != 32 && config.output.dither_enabled);
    builder.add_enum("output.dither_method", config.output.dither_method, kDitherMethods);
    builder.add_bool("output.write_alpha", config.output.write_alpha);
    builder.add_string("output.output_directory", config.output.output_directory);
    builder.add_string("output.filename_prefix", config.output.filename_prefix);
    builder.add_integer("output.first_frame_number", config.output.first_frame_number);
    builder.add_integer("output.filename_digits", config.output.filename_digits);
    builder.add_bool("output.overwrite_existing", config.output.overwrite_existing);

    if (!builder.ok()) {
        return false;
    }
    serialized = builder.contents();
    return true;
}

bool consume_audio_input_records(Records& records, std::string_view prefix,
                                 AudioInputProcessingConfig& processing,
                                 std::string* error) {
    const auto key = [prefix](std::string_view suffix) {
        std::string result(prefix);
        result.append(suffix);
        return result;
    };
    std::size_t equalizer_count = 0U;
    std::size_t stream_count = 0U;
    if (!consume_bool(records, key("high_pass_enabled"),
                      processing.high_pass_enabled, error)
        || !consume_double(records, key("high_pass_hz"),
                           processing.high_pass_hz, error)
        || !consume_bool(records, key("low_pass_enabled"),
                         processing.low_pass_enabled, error)
        || !consume_double(records, key("low_pass_hz"),
                           processing.low_pass_hz, error)
        || !consume_bool(records, key("equalizer_enabled"),
                         processing.equalizer_enabled, error)
        || !consume_count(records, key("equalizer_bands.count"),
                          kMaximumAudioEqualizerBands, equalizer_count, error)) {
        return false;
    }
    processing.equalizer_bands.assign(equalizer_count, {});
    const std::string equalizer = key("equalizer_bands");
    for (std::size_t index = 0U; index < equalizer_count; ++index) {
        auto& band = processing.equalizer_bands[index];
        if (!consume_double(records,
                            indexed_key(equalizer, index, "frequency_hz"),
                            band.frequency_hz, error)
            || !consume_double(records,
                               indexed_key(equalizer, index, "gain_db"),
                               band.gain_db, error)) return false;
    }
    if (!consume_count(records, key("frequency_streams.count"),
                       kMaximumAudioFrequencyStreams, stream_count, error)) {
        return false;
    }
    processing.frequency_streams.assign(stream_count, {});
    const std::string streams = key("frequency_streams");
    for (std::size_t index = 0U; index < stream_count; ++index) {
        auto& stream = processing.frequency_streams[index];
        if (!consume_bounded_string(records,
                                    indexed_key(streams, index, "uuid"),
                                    kMaximumLiveTextBytes, stream.uuid, error)
            || !consume_bounded_string(records,
                                       indexed_key(streams, index, "name"),
                                       kMaximumLiveTextBytes, stream.name, error)
            || !consume_double(records,
                               indexed_key(streams, index, "low_hz"),
                               stream.low_hz, error)
            || !consume_double(records,
                               indexed_key(streams, index, "high_hz"),
                               stream.high_hz, error)) return false;
    }
    return true;
}

bool consume_music_extensions(Records& records, std::string_view prefix,
                              MusicAnalysis& music, std::string* error) {
    const auto key = [prefix](std::string_view suffix) {
        std::string result(prefix);
        result.append(suffix);
        return result;
    };
    std::size_t stream_count = 0U;
    if (!consume_count(records, key("frequency_streams.count"),
                       kMaximumAudioFrequencyStreams, stream_count, error)) {
        return false;
    }
    music.frequency_streams.assign(stream_count, {});
    const std::string streams = key("frequency_streams");
    for (std::size_t stream_index = 0U; stream_index < stream_count;
         ++stream_index) {
        auto& stream = music.frequency_streams[stream_index];
        const std::string item = indexed_key(streams, stream_index, "analysis");
        std::size_t beat_count = 0U;
        std::size_t tempo_count = 0U;
        std::size_t sample_count = 0U;
        if (!consume_bounded_string(
                records, indexed_key(streams, stream_index, "uuid"),
                kMaximumLiveTextBytes, stream.uuid, error)
            || !consume_double(records,
                               indexed_key(streams, stream_index, "low_hz"),
                               stream.low_hz, error)
            || !consume_double(records,
                               indexed_key(streams, stream_index, "high_hz"),
                               stream.high_hz, error)
            || !consume_double(records, item + ".detected_bpm",
                               stream.detected_bpm, error)
            || !consume_double(records, item + ".tempo_confidence",
                               stream.tempo_confidence, error)
            || !consume_count(records, item + ".beat_times.count",
                              kMaximumMusicBeats, beat_count, error)) {
            return false;
        }
        stream.beat_times_seconds.assign(beat_count, 0.0);
        for (std::size_t index = 0U; index < beat_count; ++index) {
            if (!consume_double(
                    records,
                    indexed_key(item + ".beat_times", index, "seconds"),
                    stream.beat_times_seconds[index], error)) return false;
        }
        if (!consume_count(records, item + ".tempo_points.count",
                           kMaximumMusicTempoPoints, tempo_count, error)) {
            return false;
        }
        stream.tempo_points.assign(tempo_count, {});
        for (std::size_t index = 0U; index < tempo_count; ++index) {
            auto& point = stream.tempo_points[index];
            if (!consume_double(
                    records, indexed_key(item + ".tempo_points", index,
                                         "time_seconds"),
                    point.time_seconds, error)
                || !consume_double(
                    records, indexed_key(item + ".tempo_points", index, "bpm"),
                    point.bpm, error)
                || !consume_double(
                    records, indexed_key(item + ".tempo_points", index,
                                         "confidence"),
                    point.confidence, error)) return false;
        }
        if (!consume_count(records, item + ".feature_samples.count",
                           kMaximumMusicFeatureSamples, sample_count, error)) {
            return false;
        }
        stream.feature_samples.assign(sample_count, {});
        for (std::size_t index = 0U; index < sample_count; ++index) {
            auto& sample = stream.feature_samples[index];
            const std::string sample_key = indexed_key(
                item + ".feature_samples", index, "sample");
            if (!consume_float(records, sample_key + ".energy", sample.energy,
                               error)
                || !consume_float(records, sample_key + ".bass", sample.bass,
                                  error)
                || !consume_float(records, sample_key + ".midrange",
                                  sample.midrange, error)
                || !consume_float(records, sample_key + ".treble",
                                  sample.treble, error)
                || !consume_float(records, sample_key + ".onset", sample.onset,
                                  error)
                || !consume_float(records, sample_key + ".beat", sample.beat,
                                  error)
                || !consume_float(records, sample_key + ".spectral_centroid",
                                  sample.spectral_centroid, error)
                || !consume_float(records, sample_key + ".spectral_flatness",
                                  sample.spectral_flatness, error)
                || !consume_float(records, sample_key + ".chroma_hue",
                                  sample.chroma_hue, error)
                || !consume_float(records, sample_key + ".chroma_strength",
                                  sample.chroma_strength, error)) return false;
        }
    }
    return consume_audio_input_records(
        records, key("input_processing."), music.input_processing, error);
}

bool consume_music_records(Records& records, std::string_view prefix,
                           MusicAnalysis& music, bool extended,
                           std::string* error) {
    const auto key = [prefix](std::string_view suffix) {
        std::string result(prefix);
        result.append(suffix);
        return result;
    };
    if (!consume_integer(records, key("schema_version"), music.schema_version, error)
        || !consume_bounded_string(records, key("analyzer_version"),
                                   kMaximumAnalyzerVersionBytes,
                                   music.analyzer_version, error)
        || !consume_bounded_string(records, key("source_sha256"), kSha256HexBytes,
                                   music.source_sha256, error)
        || !consume_bounded_string(records, key("source_basename"),
                                   kMaximumMusicBasenameBytes,
                                   music.source_basename, error)
        || !consume_bounded_string(records, key("source_format"),
                                   kMaximumMusicFormatBytes,
                                   music.source_format, error)
        || !consume_integer(records, key("source_frame_count"),
                            music.source_frame_count, error)
        || !consume_integer(records, key("source_sample_rate"),
                            music.source_sample_rate, error)
        || !consume_integer(records, key("source_channel_count"),
                            music.source_channel_count, error)
        || !consume_double(records, key("duration_seconds"),
                           music.duration_seconds, error)
        || !consume_double(records, key("detected_bpm"), music.detected_bpm, error)
        || !consume_double(records, key("tempo_confidence"),
                           music.tempo_confidence, error)
        || !is_lowercase_sha256(music.source_sha256)) {
        return false;
    }
    const std::string beats = key("beat_times");
    std::size_t beat_count = 0U;
    if (!consume_count(records, beats + ".count", kMaximumMusicBeats,
                       beat_count, error)) return false;
    music.beat_times_seconds.assign(beat_count, 0.0);
    for (std::size_t index = 0U; index < beat_count; ++index) {
        if (!consume_double(records, indexed_key(beats, index, "seconds"),
                            music.beat_times_seconds[index], error)) return false;
    }
    const std::string tempos = key("tempo_points");
    std::size_t tempo_count = 0U;
    if (!consume_count(records, tempos + ".count", kMaximumMusicTempoPoints,
                       tempo_count, error)) return false;
    music.tempo_points.assign(tempo_count, {});
    for (std::size_t index = 0U; index < tempo_count; ++index) {
        MusicTempoPoint& point = music.tempo_points[index];
        if (!consume_double(records, indexed_key(tempos, index, "time_seconds"),
                            point.time_seconds, error)
            || !consume_double(records, indexed_key(tempos, index, "bpm"),
                               point.bpm, error)
            || !consume_double(records, indexed_key(tempos, index, "confidence"),
                               point.confidence, error)) return false;
    }
    const std::string samples = key("feature_samples");
    std::size_t sample_count = 0U;
    if (!consume_count(records, samples + ".count", kMaximumMusicFeatureSamples,
                       sample_count, error)) return false;
    music.feature_samples.assign(sample_count, {});
    for (std::size_t index = 0U; index < sample_count; ++index) {
        MusicFeatureSample& sample = music.feature_samples[index];
        if (!consume_float(records, indexed_key(samples, index, "energy"),
                           sample.energy, error)
            || !consume_float(records, indexed_key(samples, index, "bass"),
                              sample.bass, error)
            || !consume_float(records, indexed_key(samples, index, "midrange"),
                              sample.midrange, error)
            || !consume_float(records, indexed_key(samples, index, "treble"),
                              sample.treble, error)
            || !consume_float(records, indexed_key(samples, index, "onset"),
                              sample.onset, error)
            || !consume_float(records, indexed_key(samples, index, "beat"),
                              sample.beat, error)
            || !consume_float(records,
                              indexed_key(samples, index, "spectral_centroid"),
                              sample.spectral_centroid, error)
            || !consume_float(records,
                              indexed_key(samples, index, "spectral_flatness"),
                              sample.spectral_flatness, error)
            || !consume_float(records, indexed_key(samples, index, "chroma_hue"),
                              sample.chroma_hue, error)
            || !consume_float(records,
                              indexed_key(samples, index, "chroma_strength"),
                              sample.chroma_strength, error)) return false;
    }
    return !extended || consume_music_extensions(records, prefix, music, error);
}

bool consume_clock_records(Records& records, std::string_view prefix,
                           std::string_view music_prefix,
                           ClockConfig& clock, bool extended,
                           std::string* error) {
    const auto key = [prefix](std::string_view suffix) {
        std::string result(prefix);
        result.append(suffix);
        return result;
    };
    return consume_enum(records, key("mode"), clock.mode, kClockModes, error)
           && consume_enum(records, key("interpolation"), clock.interpolation,
                           kClockInterpolations, error)
           && consume_enum(records, key("fit"), clock.fit, kClockFits, error)
           && consume_integer(records, key("frame_interval"),
                              clock.frame_interval, error)
           && consume_integer(records, key("time_interval_microseconds"),
                              clock.time_interval_microseconds, error)
           && consume_bounded_string(records, key("meter.expression"),
                                     kMaximumMeterExpressionBytes,
                                     clock.meter.expression, error)
           && consume_double(records, key("meter.bpm"), clock.meter.bpm, error)
           && consume_integer(records, key("meter.tempo_note_denominator"),
                              clock.meter.tempo_note_denominator, error)
           && consume_enum(records, key("music_tempo"), clock.music_tempo,
                           kMusicTempoModes, error)
           && consume_enum(records, key("music_swing_policy"),
                           clock.music_swing_policy, kMusicSwingPolicies, error)
           && consume_integer(records, key("beat_offset_microseconds"),
                              clock.beat_offset_microseconds, error)
           && consume_double(records, key("phase_offset_degrees"),
                             clock.phase_offset_degrees, error)
           && consume_bool(records, key("reverse"), clock.reverse, error)
           && consume_bool(records, key("data_only"), clock.data_only, error)
           && (!extended
               || (consume_bounded_string(
                       records, key("frequency_stream_uuid"),
                       kMaximumLiveTextBytes, clock.frequency_stream_uuid, error)
                   && consume_audio_input_records(
                       records, key("audio_input."), clock.audio_processing,
                       error)))
           && consume_music_records(records, music_prefix, clock.music,
                                    extended, error);
}

bool consume_audio_reactive_records(Records& records,
                                    std::string_view prefix,
                                    AudioReactiveConfig& audio,
                                    bool optional_block,
                                    std::string* error) {
    const auto key = [prefix](std::string_view suffix) {
        std::string result(prefix);
        result.append(suffix);
        return result;
    };
    if (optional_block) {
        const bool present = std::any_of(
            records.begin(), records.end(), [prefix](const auto& record) {
                return starts_with(record.first, prefix);
            });
        if (!present) return true;
    }
    const bool enabled_ok = optional_block
        ? consume_optional_bool(records, key("enabled"), audio.enabled,
                                false, error)
        : consume_bool(records, key("enabled"), audio.enabled, error);
    return enabled_ok
           && consume_bool(records, key("synchronized_only"),
                           audio.synchronized_only, error)
           && consume_bool(records, key("waves_enabled"),
                           audio.waves_enabled, error)
           && consume_enum(records, key("wave_source"), audio.wave_source,
                           kMusicFeatures, error)
           && consume_double(records, key("wave_amount"), audio.wave_amount,
                             error)
           && consume_bool(records, key("effects_enabled"),
                           audio.effects_enabled, error)
           && consume_enum(records, key("effect_source"),
                           audio.effect_source, kMusicFeatures, error)
           && consume_double(records, key("effect_amount"),
                             audio.effect_amount, error)
           && consume_bool(records, key("color_enabled"),
                           audio.color_enabled, error)
           && consume_enum(records, key("color_source"), audio.color_source,
                           kMusicFeatures, error)
           && consume_double(records, key("color_amount_degrees"),
                             audio.color_amount_degrees, error);
}

bool consume_live_records(Records& records, LiveConfig& live, bool extended,
                          std::string* error) {
    if (!consume_bool(records, "live.enabled", live.enabled, error)) {
        return false;
    }

    std::size_t endpoint_count = 0U;
    if (!consume_count(records, "live.endpoints.count", kMaximumLiveEndpoints,
                       endpoint_count, error)) {
        return false;
    }
    live.endpoints.assign(endpoint_count, {});
    for (std::size_t index = 0U; index < endpoint_count; ++index) {
        LiveEndpointConfig& endpoint = live.endpoints[index];
        if (!consume_bounded_string(
                records, indexed_key("live.endpoints", index, "uuid"),
                kMaximumLiveTextBytes, endpoint.uuid, error)
            || !consume_bounded_string(
                records, indexed_key("live.endpoints", index, "name"),
                kMaximumLiveTextBytes, endpoint.name, error)
            || !consume_enum(
                records, indexed_key("live.endpoints", index, "protocol"),
                endpoint.protocol, kLiveEndpointProtocols, error)
            || !consume_enum(
                records, indexed_key("live.endpoints", index, "direction"),
                endpoint.direction, kLiveEndpointDirections, error)
            || !consume_integer(
                records,
                indexed_key("live.endpoints", index,
                            "input_latency_microseconds"),
                endpoint.input_latency_microseconds, error)
            || !consume_integer(
                records,
                indexed_key("live.endpoints", index,
                            "output_latency_microseconds"),
                endpoint.output_latency_microseconds, error)) {
            return false;
        }
    }

    std::size_t mapping_count = 0U;
    if (!consume_count(records, "live.mappings.count", kMaximumLiveMappings,
                       mapping_count, error)) {
        return false;
    }
    live.mappings.assign(mapping_count, {});
    for (std::size_t index = 0U; index < mapping_count; ++index) {
        LiveControlMapping& mapping = live.mappings[index];
        if (!consume_bool(
                records, indexed_key("live.mappings", index, "enabled"),
                mapping.enabled, error)
            || !consume_bounded_string(
                records, indexed_key("live.mappings", index, "name"),
                kMaximumLiveTextBytes, mapping.name, error)
            || !consume_bounded_string(
                records,
                indexed_key("live.mappings", index, "endpoint_uuid"),
                kMaximumLiveTextBytes, mapping.endpoint_uuid, error)
            || !consume_enum(
                records, indexed_key("live.mappings", index, "input"),
                mapping.input, kLiveControlInputs, error)
            || !consume_integer(
                records, indexed_key("live.mappings", index, "midi_channel"),
                mapping.midi_channel, error)
            || !consume_integer(
                records,
                indexed_key("live.mappings", index, "control_number"),
                mapping.control_number, error)
            || !consume_bounded_string(
                records, indexed_key("live.mappings", index, "osc_address"),
                kMaximumLiveTextBytes, mapping.osc_address, error)
            || !consume_enum(
                records, indexed_key("live.mappings", index, "target"),
                mapping.target, kLiveMappingTargets, error)
            || !consume_bounded_string(
                records, indexed_key("live.mappings", index, "target_path"),
                kMaximumLiveTextBytes, mapping.target_path, error)
            || !consume_enum(
                records, indexed_key("live.mappings", index, "action"),
                mapping.action, kLiveActions, error)
            || !consume_bounded_string(
                records, indexed_key("live.mappings", index, "scene_uuid"),
                kMaximumLiveTextBytes, mapping.scene_uuid, error)
            || !consume_enum(
                records, indexed_key("live.mappings", index, "mode"),
                mapping.mode, kLiveMappingModes, error)
            || !consume_double(
                records,
                indexed_key("live.mappings", index, "input_minimum"),
                mapping.input_minimum, error)
            || !consume_double(
                records,
                indexed_key("live.mappings", index, "input_maximum"),
                mapping.input_maximum, error)
            || !consume_double(
                records,
                indexed_key("live.mappings", index, "output_minimum"),
                mapping.output_minimum, error)
            || !consume_double(
                records,
                indexed_key("live.mappings", index, "output_maximum"),
                mapping.output_maximum, error)
            || !consume_double(
                records, indexed_key("live.mappings", index, "curve"),
                mapping.curve, error)
            || !consume_double(
                records, indexed_key("live.mappings", index, "dead_zone"),
                mapping.dead_zone, error)
            || !consume_integer(
                records,
                indexed_key("live.mappings", index,
                            "smoothing_milliseconds"),
                mapping.smoothing_milliseconds, error)) {
            return false;
        }
    }

    std::size_t input_count = 0U;
    if (!consume_count(records, "live.clock_inputs.count",
                       kMaximumLiveClockInputs, input_count, error)) {
        return false;
    }
    live.clock_inputs.assign(input_count, {});
    for (std::size_t index = 0U; index < input_count; ++index) {
        LiveClockInputConfig& clock = live.clock_inputs[index];
        if (!consume_bool(
                records, indexed_key("live.clock_inputs", index, "enabled"),
                clock.enabled, error)
            || !consume_enum(
                records, indexed_key("live.clock_inputs", index, "target"),
                clock.target, kLiveClockTargets, error)
            || !consume_bounded_string(
                records,
                indexed_key("live.clock_inputs", index, "layer_uuid"),
                kMaximumLiveTextBytes, clock.layer_uuid, error)
            || !consume_enum(
                records, indexed_key("live.clock_inputs", index, "source"),
                clock.source, kLiveClockInputSources, error)
            || !consume_bounded_string(
                records,
                indexed_key("live.clock_inputs", index, "endpoint_uuid"),
                kMaximumLiveTextBytes, clock.endpoint_uuid, error)
            || !consume_integer(
                records,
                indexed_key("live.clock_inputs", index, "audio_channel"),
                clock.audio_channel, error)
            || (extended
                && !consume_bounded_string(
                    records,
                    indexed_key("live.clock_inputs", index,
                                "frequency_stream_uuid"),
                    kMaximumLiveTextBytes, clock.frequency_stream_uuid,
                    error))
            || !consume_bool(
                records,
                indexed_key("live.clock_inputs", index,
                            "follow_midi_transport"),
                clock.follow_midi_transport, error)
            || !consume_integer(
                records,
                indexed_key("live.clock_inputs", index,
                            "holdover_milliseconds"),
                clock.holdover_milliseconds, error)) {
            return false;
        }
    }

    std::size_t output_count = 0U;
    if (!consume_count(records, "live.midi_clock_outputs.count",
                       kMaximumLiveClockOutputs, output_count, error)) {
        return false;
    }
    live.midi_clock_outputs.assign(output_count, {});
    for (std::size_t index = 0U; index < output_count; ++index) {
        LiveMidiClockOutputConfig& output = live.midi_clock_outputs[index];
        if (!consume_bool(
                records,
                indexed_key("live.midi_clock_outputs", index, "enabled"),
                output.enabled, error)
            || !consume_enum(
                records,
                indexed_key("live.midi_clock_outputs", index, "source"),
                output.source, kLiveClockTargets, error)
            || !consume_bounded_string(
                records,
                indexed_key("live.midi_clock_outputs", index, "layer_uuid"),
                kMaximumLiveTextBytes, output.layer_uuid, error)
            || !consume_bounded_string(
                records,
                indexed_key("live.midi_clock_outputs", index,
                            "endpoint_uuid"),
                kMaximumLiveTextBytes, output.endpoint_uuid, error)
            || !consume_bool(
                records,
                indexed_key("live.midi_clock_outputs", index,
                            "send_transport"),
                output.send_transport, error)
            || !consume_bool(
                records,
                indexed_key("live.midi_clock_outputs", index,
                            "send_song_position"),
                output.send_song_position, error)) {
            return false;
        }
    }

    std::size_t scene_count = 0U;
    if (!consume_count(records, "live.scenes.count", kMaximumLiveScenes,
                       scene_count, error)) {
        return false;
    }
    live.scenes.assign(scene_count, {});
    std::size_t remaining_values = kMaximumLiveSceneValues;
    for (std::size_t index = 0U; index < scene_count; ++index) {
        LiveSceneConfig& scene = live.scenes[index];
        const std::string values = indexed_key("live.scenes", index, "values");
        std::size_t value_count = 0U;
        if (!consume_bounded_string(
                records, indexed_key("live.scenes", index, "uuid"),
                kMaximumLiveTextBytes, scene.uuid, error)
            || !consume_bounded_string(
                records, indexed_key("live.scenes", index, "name"),
                kMaximumLiveTextBytes, scene.name, error)
            || !consume_integer(
                records,
                indexed_key("live.scenes", index, "transition_milliseconds"),
                scene.transition_milliseconds, error)
            || !consume_count(records, values + ".count", remaining_values,
                              value_count, error)) {
            return false;
        }
        remaining_values -= value_count;
        scene.values.assign(value_count, {});
        for (std::size_t value_index = 0U;
             value_index < value_count; ++value_index) {
            LiveSceneValue& value = scene.values[value_index];
            if (!consume_bounded_string(
                    records,
                    indexed_key(values, value_index, "target_path"),
                    kMaximumLiveTextBytes, value.target_path, error)
                || !consume_enum(
                    records, indexed_key(values, value_index, "type"),
                    value.type, kLiveSceneValueTypes, error)
                || !consume_bounded_string(
                    records, indexed_key(values, value_index, "value"),
                    kMaximumLiveTextBytes, value.value, error)) {
                return false;
            }
        }
    }

    return consume_bounded_string(
               records, "live.startup_scene_uuid", kMaximumLiveTextBytes,
               live.startup_scene_uuid, error)
           && consume_bool(records, "live.output.fullscreen",
                           live.output.fullscreen, error)
           && consume_bool(records, "live.output.prefer_secondary_display",
                           live.output.prefer_secondary_display, error)
           && consume_bool(records, "live.output.hide_cursor",
                           live.output.hide_cursor, error)
           && consume_enum(records, "live.safety.dropout_behavior",
                           live.safety.dropout_behavior,
                           kLiveDropoutBehaviors, error)
           && consume_bool(records,
                           "live.safety.frame_time_watchdog_enabled",
                           live.safety.frame_time_watchdog_enabled, error)
           && consume_integer(records,
                              "live.safety.watchdog_timeout_milliseconds",
                              live.safety.watchdog_timeout_milliseconds,
                              error)
           && consume_integer(
               records, "live.safety.audio_dropout_grace_milliseconds",
               live.safety.audio_dropout_grace_milliseconds, error)
           && consume_integer(
               records,
               "live.safety.last_good_frame_timeout_milliseconds",
               live.safety.last_good_frame_timeout_milliseconds, error)
           && (!extended
               || (consume_bool(records,
                                "live.safety.prevent_device_sleep",
                                live.safety.prevent_device_sleep, error)
                   && consume_audio_input_records(
                       records, "live.audio_input.", live.audio_processing,
                       error)));
}

bool consume_path_binding_records(Records& records,
                                  std::string_view prefix,
                                  PathBinding& binding,
                                  std::string* error) {
    const auto key = [prefix](std::string_view suffix) {
        std::string result(prefix);
        result.append(suffix);
        return result;
    };
    return consume_bool(records, key("enabled"), binding.enabled, error)
           && consume_integer(records, key("path_id"), binding.path_id, error)
           && consume_bool(records, key("synchronized"),
                           binding.synchronized, error)
           && consume_integer(records, key("cycles_per_loop"),
                              binding.cycles_per_loop, error)
           && consume_double(records, key("phase_degrees"),
                             binding.phase_degrees, error)
           && consume_bool(records, key("reverse"), binding.reverse, error)
           && consume_double(records, key("offset_x"), binding.offset_x, error)
           && consume_double(records, key("offset_y"), binding.offset_y, error)
           && consume_bool(records, key("follow_tangent"),
                           binding.follow_tangent, error);
}

bool deserialize_setup(Records& records,
                       std::uint32_t setup_version,
                       RenderConfig& candidate,
                       std::string* error,
                       bool enforce_particle_workload = true) {
    int legacy_surface_turns = 0;
    double legacy_surface_phase = 0.0;
    if (!consume_integer(records, "canvas.width", candidate.width, error)
        || !consume_integer(records, "canvas.height", candidate.height, error)
        || !consume_integer(records, "canvas.block_size", candidate.block_size, error)
        || !consume_integer(records, "timing.total_frames", candidate.total_frames, error)
        || !consume_double(records, "timing.fps", candidate.fps, error)) {
        return false;
    }

    if (setup_version >= 8U
        && !consume_audio_reactive_records(
            records, "audio_response_defaults.",
            candidate.audio_reactive_defaults, true, error)) {
        return false;
    }
    if (setup_version >= 12U
        && !consume_live_records(records, candidate.live,
                                 setup_version >= 14U, error)) {
        return false;
    }

    if (setup_version >= 7U) {
        std::size_t path_count = 0U;
        if (!consume_count(records, "paths.count", kMaximumMotionPaths,
                           path_count, error)) {
            return false;
        }
        candidate.motion_paths.assign(path_count, {});
        for (std::size_t path_index = 0U;
             path_index < path_count; ++path_index) {
            CubicMotionPath& path = candidate.motion_paths[path_index];
            const std::string nodes =
                indexed_key("paths", path_index, "nodes");
            std::size_t node_count = 0U;
            if (!consume_integer(records,
                                 indexed_key("paths", path_index, "id"),
                                 path.id, error)
                || !consume_string(records,
                                   indexed_key("paths", path_index, "name"),
                                   path.name, error)
                || !consume_count(records, nodes + ".count",
                                  kMaximumMotionPathNodes, node_count, error)) {
                return false;
            }
            path.nodes.assign(node_count, {});
            for (std::size_t node_index = 0U;
                 node_index < node_count; ++node_index) {
                CubicPathNode& node = path.nodes[node_index];
                if (!consume_integer(records,
                                     indexed_key(nodes, node_index, "id"),
                                     node.id, error)
                    || !consume_double(records,
                                       indexed_key(nodes, node_index, "x"),
                                       node.x, error)
                    || !consume_double(records,
                                       indexed_key(nodes, node_index, "y"),
                                       node.y, error)
                    || !consume_double(records,
                                       indexed_key(nodes, node_index, "in_x"),
                                       node.in_x, error)
                    || !consume_double(records,
                                       indexed_key(nodes, node_index, "in_y"),
                                       node.in_y, error)
                    || !consume_double(records,
                                       indexed_key(nodes, node_index, "out_x"),
                                       node.out_x, error)
                    || !consume_double(records,
                                       indexed_key(nodes, node_index, "out_y"),
                                       node.out_y, error)
                    || !consume_enum(records,
                                     indexed_key(nodes, node_index,
                                                 "handle_mode"),
                                     node.handle_mode, kPathHandleModes,
                                     error)) {
                    return false;
                }
            }
        }
    }
    if (setup_version >= 10U
        && (!consume_bool(records, "source_image.palette_dither_enabled",
                          candidate.starting_image.palette_dither_enabled,
                          error)
            || !consume_enum(records, "source_image.palette_dither_method",
                             candidate.starting_image.palette_dither_method,
                             kDitherMethods, error))) {
        return false;
    }

    if (setup_version >= 5U) {
        MusicAnalysis& music = candidate.clock.music;
        if (!consume_enum(records, "timing.clock.mode", candidate.clock.mode,
                          kClockModes, error)
            || !consume_enum(records, "timing.clock.interpolation",
                             candidate.clock.interpolation,
                             kClockInterpolations, error)
            || !consume_enum(records, "timing.clock.fit", candidate.clock.fit,
                             kClockFits, error)
            || !consume_integer(records, "timing.clock.frame_interval",
                                candidate.clock.frame_interval, error)
            || !consume_integer(records, "timing.clock.time_interval_microseconds",
                                candidate.clock.time_interval_microseconds, error)
            || !consume_bounded_string(records, "timing.clock.meter.expression",
                                       kMaximumMeterExpressionBytes,
                                       candidate.clock.meter.expression, error)
            || !consume_double(records, "timing.clock.meter.bpm",
                               candidate.clock.meter.bpm, error)
            || !consume_integer(records,
                                "timing.clock.meter.tempo_note_denominator",
                                candidate.clock.meter.tempo_note_denominator,
                                error)
            || !consume_enum(records, "timing.clock.music_tempo",
                             candidate.clock.music_tempo,
                             kMusicTempoModes, error)
            || !consume_enum(records, "timing.clock.music_swing_policy",
                             candidate.clock.music_swing_policy,
                             kMusicSwingPolicies, error)
            || !consume_integer(records, "timing.clock.beat_offset_microseconds",
                                candidate.clock.beat_offset_microseconds, error)
            || !consume_double(records, "timing.clock.phase_offset_degrees",
                               candidate.clock.phase_offset_degrees, error)
            || !consume_bool(records, "timing.clock.reverse",
                             candidate.clock.reverse, error)
            || !consume_integer(records, "timing.music.schema_version",
                                music.schema_version, error)
            || !consume_bounded_string(records, "timing.music.analyzer_version",
                                       kMaximumAnalyzerVersionBytes,
                                       music.analyzer_version, error)
            || !consume_bounded_string(records, "timing.music.source_sha256",
                                       kSha256HexBytes,
                                       music.source_sha256, error)
            || !consume_bounded_string(records, "timing.music.source_basename",
                                       kMaximumMusicBasenameBytes,
                                       music.source_basename, error)
            || !consume_bounded_string(records, "timing.music.source_format",
                                       kMaximumMusicFormatBytes,
                                       music.source_format, error)
            || !consume_integer(records, "timing.music.source_frame_count",
                                music.source_frame_count, error)
            || !consume_integer(records, "timing.music.source_sample_rate",
                                music.source_sample_rate, error)
            || !consume_integer(records, "timing.music.source_channel_count",
                                music.source_channel_count, error)
            || !consume_double(records, "timing.music.duration_seconds",
                               music.duration_seconds, error)
            || !consume_double(records, "timing.music.detected_bpm",
                               music.detected_bpm, error)
            || !consume_double(records, "timing.music.tempo_confidence",
                               music.tempo_confidence, error)) {
            return false;
        }
        if (!is_lowercase_sha256(music.source_sha256)) {
            return fail(error,
                        record_error("Invalid lowercase SHA-256 digest in setup key",
                                     "timing.music.source_sha256"));
        }

        std::size_t beat_count = 0U;
        if (!consume_count(records, "timing.music.beat_times.count",
                           kMaximumMusicBeats, beat_count, error)) {
            return false;
        }
        music.beat_times_seconds.clear();
        music.beat_times_seconds.resize(beat_count);
        for (std::size_t index = 0U; index < beat_count; ++index) {
            if (!consume_double(records,
                                indexed_key("timing.music.beat_times", index,
                                            "seconds"),
                                music.beat_times_seconds[index], error)) {
                return false;
            }
        }

        std::size_t tempo_count = 0U;
        if (!consume_count(records, "timing.music.tempo_points.count",
                           kMaximumMusicTempoPoints, tempo_count, error)) {
            return false;
        }
        music.tempo_points.clear();
        music.tempo_points.resize(tempo_count);
        for (std::size_t index = 0U; index < tempo_count; ++index) {
            MusicTempoPoint& point = music.tempo_points[index];
            if (!consume_double(records,
                                indexed_key("timing.music.tempo_points", index,
                                            "time_seconds"),
                                point.time_seconds, error)
                || !consume_double(records,
                                   indexed_key("timing.music.tempo_points", index,
                                               "bpm"),
                                   point.bpm, error)
                || !consume_double(records,
                                   indexed_key("timing.music.tempo_points", index,
                                               "confidence"),
                                   point.confidence, error)) {
                return false;
            }
        }

        std::size_t feature_count = 0U;
        if (!consume_count(records, "timing.music.feature_samples.count",
                           kMaximumMusicFeatureSamples, feature_count, error)) {
            return false;
        }
        music.feature_samples.clear();
        music.feature_samples.resize(feature_count);
        for (std::size_t index = 0U; index < feature_count; ++index) {
            MusicFeatureSample& sample = music.feature_samples[index];
            if (!consume_float(records,
                               indexed_key("timing.music.feature_samples", index,
                                           "energy"),
                               sample.energy, error)
                || !consume_float(records,
                                  indexed_key("timing.music.feature_samples", index,
                                              "bass"),
                                  sample.bass, error)
                || !consume_float(records,
                                  indexed_key("timing.music.feature_samples", index,
                                              "midrange"),
                                  sample.midrange, error)
                || !consume_float(records,
                                  indexed_key("timing.music.feature_samples", index,
                                              "treble"),
                                  sample.treble, error)
                || !consume_float(records,
                                  indexed_key("timing.music.feature_samples", index,
                                              "onset"),
                                  sample.onset, error)
                || !consume_float(records,
                                  indexed_key("timing.music.feature_samples", index,
                                              "beat"),
                                  sample.beat, error)
                || !consume_float(records,
                                  indexed_key("timing.music.feature_samples", index,
                                              "spectral_centroid"),
                                  sample.spectral_centroid, error)
                || !consume_float(records,
                                  indexed_key("timing.music.feature_samples", index,
                                              "spectral_flatness"),
                                  sample.spectral_flatness, error)
                || !consume_float(records,
                                  indexed_key("timing.music.feature_samples", index,
                                              "chroma_hue"),
                                  sample.chroma_hue, error)
                || !consume_float(records,
                                  indexed_key("timing.music.feature_samples", index,
                                              "chroma_strength"),
                                  sample.chroma_strength, error)) {
                return false;
            }
        }
    }
    if (setup_version >= 14U
        && (!consume_bounded_string(
                records, "timing.clock.frequency_stream_uuid",
                kMaximumLiveTextBytes,
                candidate.clock.frequency_stream_uuid, error)
            || !consume_audio_input_records(
                records, "timing.clock.audio_input.",
                candidate.clock.audio_processing, error)
            || !consume_music_extensions(
                records, "timing.music.", candidate.clock.music, error))) {
        return false;
    }
    if (setup_version >= 7U) {
        if (!consume_bool(records, "source_image.enabled",
                          candidate.starting_image.enabled, error)
            || !consume_enum(records, "source_image.fit",
                             candidate.starting_image.fit,
                             kStartingImageFits, error)
            || !consume_string(records, "source_image.path",
                               candidate.starting_image.path, error)
            || !consume_bounded_string(records, "source_image.sha256",
                                       kSha256HexBytes,
                                       candidate.starting_image.sha256, error)
            || !consume_bounded_string(records, "source_image.basename",
                                       kMaximumAttachmentBasenameBytes,
                                       candidate.starting_image.basename, error)
            || !is_lowercase_sha256(candidate.starting_image.sha256)) {
            return fail(error,
                        record_error("Invalid starting-image attachment metadata at setup key",
                                     "source_image.sha256"));
        }
    }

    if (setup_version >= 6U) {
        if (!consume_bool(records, "timing.clock.data_only",
                          candidate.clock.data_only, error)
            || !consume_bool(records, "layer_clock.enabled",
                             candidate.layer_clock.enabled, error)
            || !consume_enum(records, "layer_clock.scale",
                             candidate.layer_clock.scale,
                             kLayerClockScales, error)
            || !consume_clock_records(records, "layer_clock.clock.",
                                      "layer_clock.music.",
                                      candidate.layer_clock.clock,
                                      setup_version >= 14U, error)) {
            return false;
        }
    }
    if (setup_version >= 11U
        && !consume_enum(records, "layer_clock.mix",
                         candidate.layer_clock.mix,
                         kLayerClockMixModes, error)) {
        return false;
    }
    if (setup_version >= 11U
        && !consume_bool(records, "layer_clock.mix_enabled",
                         candidate.layer_clock.mix_enabled, error)) {
        return false;
    }

    std::size_t wave_count = 0;
    if (!consume_count(records, "waves.count", kMaximumWaves, wave_count, error)) {
        return false;
    }
    candidate.waves.clear();
    candidate.waves.resize(wave_count);
    for (std::size_t index = 0; index < wave_count; ++index) {
        WaveConfig& wave = candidate.waves[index];
        if (!consume_integer(records, indexed_key("waves", index, "id"), wave.id, error)
            || !consume_string(records, indexed_key("waves", index, "name"), wave.name, error)
            || !consume_bool(records, indexed_key("waves", index, "enabled"), wave.enabled, error)
            || !consume_bool(records, indexed_key("waves", index, "synchronized"), wave.synchronized, error)
            || (setup_version >= 9U
                && !consume_optional_enum(
                    records, indexed_key("waves", index, "audio_response"),
                    wave.audio_response, AudioResponseMode::Default,
                    kAudioResponseModes, error))
            || (setup_version == 8U
                && !consume_optional_enum(
                    records, indexed_key("waves", index, "audio_response"),
                    wave.audio_response, AudioResponseMode::Default,
                    kAudioResponseModesV8, error))
            || !consume_double(records, indexed_key("waves", index, "x_percent"), wave.x_percent, error)
            || !consume_double(records, indexed_key("waves", index, "y_percent"), wave.y_percent, error)
            || !consume_double(records, indexed_key("waves", index, "amplitude"), wave.amplitude, error)
            || !consume_double(records, indexed_key("waves", index, "spatial_frequency"), wave.spatial_frequency, error)
            || !consume_integer(records, indexed_key("waves", index, "cycles_per_loop"), wave.cycles_per_loop, error)
            || !consume_double(records, indexed_key("waves", index, "phase_degrees"), wave.phase_degrees, error)
            || !consume_double(records, indexed_key("waves", index, "direction"), wave.direction, error)) {
            return false;
        }
        if (setup_version >= 7U
            && !consume_path_binding_records(
                records, indexed_key("waves", index, "path") + ".",
                wave.path, error)) {
            return false;
        }
    }

    std::size_t swing_count = 0;
    if (!consume_count(records, "swings.count", kMaximumSwings, swing_count, error)) {
        return false;
    }
    candidate.swings.clear();
    candidate.swings.resize(swing_count);
    for (std::size_t index = 0; index < swing_count; ++index) {
        SwingConfig& swing = candidate.swings[index];
        if (!consume_integer(records, indexed_key("swings", index, "id"), swing.id, error)
            || !consume_string(records, indexed_key("swings", index, "name"), swing.name, error)
            || !consume_bool(records, indexed_key("swings", index, "enabled"), swing.enabled, error)
            || !consume_enum(records, indexed_key("swings", index, "waveform"), swing.waveform, kWaveforms, error)
            || !consume_double(records, indexed_key("swings", index, "amount"), swing.amount, error)
            || !consume_integer(records, indexed_key("swings", index, "cycles_per_loop"), swing.cycles_per_loop, error)
            || !consume_double(records, indexed_key("swings", index, "phase_degrees"), swing.phase_degrees, error)
            || !consume_double(records, indexed_key("swings", index, "shape"), swing.shape, error)) {
            return false;
        }
        if (setup_version >= 4U
            && (!consume_double(records, indexed_key("swings", index, "center_x"),
                                swing.center_x, error)
                || !consume_double(records, indexed_key("swings", index, "center_y"),
                                   swing.center_y, error)
                || !consume_double(records, indexed_key("swings", index, "radius"),
                                   swing.radius, error))) {
            return false;
        }
    }

    std::size_t effect_count = 0;
    if (!consume_count(records, "effects.count", kMaximumEffects, effect_count, error)) {
        return false;
    }
    if (effect_count > records.size()) {
        return fail(
            error,
            record_error(
                "Collection count exceeds the records available to decode in setup key",
                "effects.count"));
    }
    candidate.effects.clear();
    candidate.effects.resize(effect_count);
    for (std::size_t index = 0; index < effect_count; ++index) {
        EffectConfig& effect = candidate.effects[index];
        if (!consume_integer(records, indexed_key("effects", index, "id"), effect.id, error)
            || !consume_string(records, indexed_key("effects", index, "name"), effect.name, error)) {
            return false;
        }
        if (setup_version >= 20U) {
            if (!consume_enum(records, indexed_key("effects", index, "type"),
                              effect.type, kEffectTypes, error)) {
                return false;
            }
        } else if (setup_version >= 14U) {
            if (!consume_enum(records, indexed_key("effects", index, "type"),
                              effect.type, kEffectTypesV19, error)) {
                return false;
            }
        } else if (setup_version >= 11U) {
            if (!consume_enum(records, indexed_key("effects", index, "type"),
                              effect.type, kEffectTypesV13, error)) {
                return false;
            }
        } else if (setup_version >= 10U) {
            if (!consume_enum(records, indexed_key("effects", index, "type"),
                              effect.type, kEffectTypesV10, error)) {
                return false;
            }
        } else if (!consume_enum(records, indexed_key("effects", index, "type"),
                                 effect.type, kEffectTypesV9, error)) {
            return false;
        }
        if (setup_version >= 4U
            && !consume_enum(records, indexed_key("effects", index, "space"),
                             effect.space, kEffectSpaces, error)) {
            return false;
        }
        if (!consume_bool(records, indexed_key("effects", index, "enabled"), effect.enabled, error)
            || !consume_bool(records, indexed_key("effects", index, "synchronized"), effect.synchronized, error)
            || (setup_version >= 9U
                && !consume_optional_enum(
                    records, indexed_key("effects", index, "audio_response"),
                    effect.audio_response, AudioResponseMode::Default,
                    kAudioResponseModes, error))
            || (setup_version == 8U
                && !consume_optional_enum(
                    records, indexed_key("effects", index, "audio_response"),
                    effect.audio_response, AudioResponseMode::Default,
                    kAudioResponseModesV8, error))
            || !consume_integer(records, indexed_key("effects", index, "cycles_per_loop"), effect.cycles_per_loop, error)
            || !consume_double(records, indexed_key("effects", index, "phase_degrees"), effect.phase_degrees, error)
            || !consume_enum(records, indexed_key("effects", index, "edge_mode"), effect.edge_mode, kEdgeModes, error)
            || !consume_double(records, indexed_key("effects", index, "intensity"), effect.intensity, error)
            || !consume_double(records, indexed_key("effects", index, "magnitude"), effect.magnitude, error)
            || !consume_double(records, indexed_key("effects", index, "frequency"), effect.frequency, error)
            || !consume_double(records, indexed_key("effects", index, "secondary"), effect.secondary, error)
            || !consume_double(records, indexed_key("effects", index, "center_x"), effect.center_x, error)
            || !consume_double(records, indexed_key("effects", index, "center_y"), effect.center_y, error)
            || !consume_double(records, indexed_key("effects", index, "angle_degrees"), effect.angle_degrees, error)
            || !consume_double(records, indexed_key("effects", index, "radius_pixels"), effect.radius_pixels, error)
            || !consume_double(records, indexed_key("effects", index, "threshold"), effect.threshold, error)
            || !consume_double(records, indexed_key("effects", index, "soft_knee"), effect.soft_knee, error)) {
            return false;
        }
        if (setup_version >= 4U
            && !consume_double(records, indexed_key("effects", index, "area_radius"),
                               effect.area_radius, error)) {
            return false;
        }
        if (setup_version >= 10U
            && (!consume_enum(records, indexed_key("effects", index, "blur_type"),
                              effect.blur_type, kBlurTypes, error)
                || !consume_integer(records,
                                    indexed_key("effects", index, "blur_passes"),
                                    effect.blur_passes, error)
                || !consume_integer(records,
                                    indexed_key("effects", index, "blur_samples"),
                                    effect.blur_samples, error)
                || !consume_double(records,
                                   indexed_key("effects", index, "blur_minimum"),
                                   effect.blur_minimum, error)
                || !consume_double(records,
                                   indexed_key("effects", index, "blur_maximum"),
                                   effect.blur_maximum, error)
                || !consume_integer(
                    records,
                    indexed_key("effects", index, "blur_pulses_per_cycle"),
                    effect.blur_pulses_per_cycle, error))) {
            return false;
        }
        if (setup_version >= 14U
            && !consume_enum(records,
                             indexed_key("effects", index, "particle_shape"),
                             effect.particle_shape, kParticleShapes, error)) {
            return false;
        }
        if (setup_version >= 15U
            && (!consume_enum(
                    records, indexed_key("effects", index, "particle_profile"),
                    effect.particle_profile, kParticleProfiles, error)
                || !consume_double(
                    records,
                    indexed_key("effects", index, "particle_size_variation"),
                    effect.particle_size_variation, error)
                || !consume_double(
                    records, indexed_key("effects", index, "particle_definition"),
                    effect.particle_definition, error)
                || !consume_double(
                    records, indexed_key("effects", index, "particle_twinkle"),
                    effect.particle_twinkle, error)
                || !consume_integer(
                    records, indexed_key("effects", index, "particle_seed"),
                    effect.particle_seed, error)
                || !consume_enum(
                    records,
                    indexed_key("effects", index, "particle_orientation"),
                    effect.particle_orientation, kParticleOrientations, error)
                || !consume_double(
                    records,
                    indexed_key("effects", index, "particle_rotation_degrees"),
                    effect.particle_rotation_degrees, error))) {
            return false;
        }
        if (setup_version >= 7U
            && !consume_path_binding_records(
                records, indexed_key("effects", index, "path") + ".",
                effect.path, error)) {
            return false;
        }
    }

    if (setup_version >= 17U) {
        std::size_t lfo_count = 0U;
        if (!consume_count(records, "parameter_lfos.count",
                           kMaximumParameterLfos, lfo_count, error)) {
            return false;
        }
        candidate.parameter_lfos.assign(lfo_count, {});
        for (std::size_t index = 0U; index < lfo_count; ++index) {
            ParameterLfo& lfo = candidate.parameter_lfos[index];
            if (!consume_bool(
                    records,
                    indexed_key("parameter_lfos", index, "enabled"),
                    lfo.enabled, error)
                || !consume_bounded_string(
                    records,
                    indexed_key("parameter_lfos", index, "target_path"),
                    kMaximumDecodedStringBytes, lfo.target_path, error)
                || !consume_enum(
                    records,
                    indexed_key("parameter_lfos", index, "waveform"),
                    lfo.waveform, kWaveforms, error)
                || !consume_double(
                    records,
                    indexed_key("parameter_lfos", index, "minimum"),
                    lfo.minimum, error)
                || !consume_double(
                    records,
                    indexed_key("parameter_lfos", index, "maximum"),
                    lfo.maximum, error)
                || !consume_integer(
                    records,
                    indexed_key("parameter_lfos", index,
                                "cycles_per_loop"),
                    lfo.cycles_per_loop, error)
                || !consume_double(
                    records,
                    indexed_key("parameter_lfos", index, "phase_degrees"),
                    lfo.phase_degrees, error)
                || !consume_double(
                    records,
                    indexed_key("parameter_lfos", index, "shape"),
                    lfo.shape, error)) {
                return false;
            }
        }
    }

    if (setup_version >= 5U) {
        if (!consume_bool(records, "rhythm.swings_enabled",
                          candidate.swings_enabled, error)
            || !consume_audio_reactive_records(
                records, "audio_reactive.", candidate.audio_reactive,
                false, error)) {
            return false;
        }
        if (setup_version >= 8U) {
            // Missing/null is the authored Default state: inherit the project
            // block. Serializers still emit the canonical explicit boolean.
            candidate.audio_reactive_override_enabled = false;
            if (!consume_optional_bool(
                    records, "audio_reactive.override_enabled",
                    candidate.audio_reactive_override_enabled, false, error)) {
                return false;
            }
        } else {
            // Before v8 every layer block was authoritative. Preserve those
            // visuals exactly when importing old setups and bundles.
            candidate.audio_reactive_override_enabled = true;
        }
    }

    if (!consume_double(records, "rhythm.phrase_warp", candidate.phrase_warp, error)
        || !consume_double(records, "rhythm.ghost_mix", candidate.ghost_mix, error)
        || !consume_double(records, "rhythm.ghost_lag_degrees", candidate.ghost_lag_degrees, error)
        || !consume_bool(records, "appearance.displacement_enabled", candidate.displacement_enabled, error)
        || !consume_double(records, "appearance.displacement", candidate.displacement, error)
        || !consume_bool(records, "appearance.lighting_enabled", candidate.lighting_enabled, error)
        || !consume_double(records, "appearance.wave_depth", candidate.wave_depth, error)
        || !consume_bool(records, "appearance.spiral_enabled", candidate.spiral_enabled, error)
        || !consume_double(records, "appearance.spiral_frequency", candidate.spiral_frequency, error)
        || !consume_integer(records, "appearance.spiral_arms", candidate.spiral_arms, error)
        || !consume_bool(records, "appearance.wall_reflection_enabled", candidate.wall_reflection_enabled, error)
        || !consume_double(records, "appearance.wall_frequency", candidate.wall_frequency, error)
        || !consume_double(records, "appearance.wall_mix", candidate.wall_mix, error)
        || !consume_integer(records, "appearance.hue_cycles", candidate.hue_cycles, error)
        || !consume_double(records, "appearance.saturation", candidate.saturation, error)) {
        return false;
    }

    if (!consume_bool(records, "alpha.enabled", candidate.alpha.enabled, error)
        || !consume_double(records, "alpha.minimum", candidate.alpha.minimum, error)
        || !consume_double(records, "alpha.maximum", candidate.alpha.maximum, error)
        || !consume_double(records, "alpha.spatial_frequency", candidate.alpha.spatial_frequency, error)
        || !consume_integer(records, "alpha.cycles_per_loop", candidate.alpha.cycles_per_loop, error)
        || !consume_double(records, "alpha.phase_degrees", candidate.alpha.phase_degrees, error)
        || !consume_bool(records, "quantization.enabled", candidate.quantization.enabled, error)
        || !consume_integer(records, "quantization.levels", candidate.quantization.levels, error)
        || !consume_double(records, "quantization.mix", candidate.quantization.mix, error)
        || !consume_enum(records, "quantization.mode", candidate.quantization.mode, kQuantizationModes, error)
        || !consume_bool(records, "surface.enabled", candidate.surface.enabled, error)
        || !consume_enum(records, "surface.mapping", candidate.surface.mapping, kSurfaceMappings, error)) {
        return false;
    }
    SurfaceConfig& surface = candidate.surface;
    if (setup_version >= 16U) {
        if (!consume_enum(records, "surface.projection", surface.projection,
                          kSurfaceProjections, error)
            || !consume_enum(records, "surface.sizing", surface.sizing,
                             kSurfaceSizings, error)
            || !consume_enum(records, "surface.outside", surface.outside,
                             kSurfaceOutsides, error)
            || !consume_enum(records, "surface.rotation_order",
                             surface.rotation_order,
                             kSurfaceRotationOrders, error)
            || !consume_integer(records, "surface.rotation_x_turns_per_loop",
                                surface.rotation_x_turns_per_loop, error)
            || !consume_integer(records, "surface.rotation_y_turns_per_loop",
                                surface.rotation_y_turns_per_loop, error)
            || !consume_integer(records, "surface.rotation_z_turns_per_loop",
                                surface.rotation_z_turns_per_loop, error)
            || !consume_double(records, "surface.rotation_x_degrees",
                               surface.rotation_x_degrees, error)
            || !consume_double(records, "surface.rotation_y_degrees",
                               surface.rotation_y_degrees, error)
            || !consume_double(records, "surface.rotation_z_degrees",
                               surface.rotation_z_degrees, error)
            || !consume_double(records, "surface.size_percent",
                               surface.size_percent, error)
            || !consume_double(records, "surface.scale_x", surface.scale_x,
                               error)
            || !consume_double(records, "surface.scale_y", surface.scale_y,
                               error)
            || !consume_double(records, "surface.scale_z", surface.scale_z,
                               error)
            || !consume_double(records, "surface.position_x_percent",
                               surface.position_x_percent, error)
            || !consume_double(records, "surface.position_y_percent",
                               surface.position_y_percent, error)
            || !consume_double(records, "surface.position_z",
                               surface.position_z, error)
            || !consume_double(records, "surface.camera_distance",
                               surface.camera_distance, error)
            || !consume_double(records, "surface.focal_length",
                               surface.focal_length, error)
            || !consume_double(records, "surface.curvature",
                               surface.curvature, error)
            || !consume_double(records, "surface.lighting", surface.lighting,
                               error)
            || !consume_double(records, "surface.light_direction_x",
                               surface.light_direction_x, error)
            || !consume_double(records, "surface.light_direction_y",
                               surface.light_direction_y, error)
            || !consume_double(records, "surface.light_direction_z",
                               surface.light_direction_z, error)
            || !consume_double(records, "surface.light_ambient",
                               surface.light_ambient, error)
            || !consume_double(records, "surface.light_diffuse",
                               surface.light_diffuse, error)
            || !consume_bool(records, "surface.composite_backfaces",
                             surface.composite_backfaces, error)
            || !consume_bool(records, "surface.normalize_obj",
                             surface.normalize_obj, error)) {
            return false;
        }
    } else if (!consume_integer(records, "surface.rotations_per_loop",
                                legacy_surface_turns, error)
               || !consume_double(records, "surface.phase_degrees",
                                  legacy_surface_phase, error)
               || !consume_double(records, "surface.curvature",
                                  surface.curvature, error)
               || !consume_double(records, "surface.lighting",
                                  surface.lighting, error)) {
        return false;
    }
    if (setup_version >= 12U) {
        PostProcessConfig& post = candidate.post_process;
        if (!consume_bool(records, "post_process.invert_rgb_enabled",
                          post.invert_rgb_enabled, error)
            || !consume_double(records, "post_process.invert_rgb_mix",
                               post.invert_rgb_mix, error)
            || !consume_bool(records, "post_process.invert_alpha_enabled",
                             post.invert_alpha_enabled, error)
            || !consume_double(records, "post_process.invert_alpha_mix",
                               post.invert_alpha_mix, error)
            || !consume_bool(records, "post_process.antialias_enabled",
                             post.antialias_enabled, error)
            || !consume_double(records, "post_process.antialias_strength",
                               post.antialias_strength, error)
            || !consume_double(records, "post_process.antialias_threshold",
                               post.antialias_threshold, error)
            || !consume_integer(records, "post_process.antialias_passes",
                                post.antialias_passes, error)) {
            return false;
        }
    }
    if (setup_version >= 18U) {
        PostProcessConfig& post = candidate.post_process;
        if (!consume_bool(records, "post_process.invert_red_enabled",
                          post.invert_red_enabled, error)
            || !consume_double(records, "post_process.invert_red_mix",
                               post.invert_red_mix, error)
            || !consume_bool(records, "post_process.invert_green_enabled",
                             post.invert_green_enabled, error)
            || !consume_double(records, "post_process.invert_green_mix",
                               post.invert_green_mix, error)
            || !consume_bool(records, "post_process.invert_blue_enabled",
                             post.invert_blue_enabled, error)
            || !consume_double(records, "post_process.invert_blue_mix",
                               post.invert_blue_mix, error)) {
            return false;
        }
    }
    if (setup_version >= 19U) {
        PostProcessConfig& post = candidate.post_process;
        std::size_t order_count = 0U;
        if (!consume_bool(records, "post_process.channel_map.enabled",
                          post.channel_map.enabled, error)
            || !consume_double(records, "post_process.channel_map.mix",
                               post.channel_map.mix, error)
            || !consume_enum(records,
                             "post_process.channel_map.red_source",
                             post.channel_map.red_source, kChannelSources,
                             error)
            || !consume_enum(records,
                             "post_process.channel_map.green_source",
                             post.channel_map.green_source, kChannelSources,
                             error)
            || !consume_enum(records,
                             "post_process.channel_map.blue_source",
                             post.channel_map.blue_source, kChannelSources,
                             error)
            || !consume_enum(records,
                             "post_process.channel_map.alpha_source",
                             post.channel_map.alpha_source, kChannelSources,
                             error)
            || !consume_count(records, "post_process.order.count",
                              kPostProcessStageCount, order_count, error)) {
            return false;
        }
        post.order.assign(order_count, PostProcessStage::InvertRgb);
        for (std::size_t index = 0U; index < order_count; ++index) {
            if (!consume_enum(
                    records,
                    indexed_key("post_process.order", index, "stage"),
                    post.order[index], kPostProcessStages, error)) {
                return false;
            }
        }
    }
    if (setup_version >= 10U) {
        StartingColorConfig& starting = candidate.starting_colors;
        if (!consume_bool(records, "alpha.use_source_alpha",
                          candidate.alpha.use_source_alpha, error)
            || !consume_enum(records, "starting_colors.mode", starting.mode,
                             kStartingColorModes, error)
            || !consume_bool(records, "starting_colors.include_alpha",
                             starting.include_alpha, error)
            || !consume_integer(records, "starting_colors.red_steps",
                                starting.red_steps, error)
            || !consume_integer(records, "starting_colors.green_steps",
                                starting.green_steps, error)
            || !consume_integer(records, "starting_colors.blue_steps",
                                starting.blue_steps, error)
            || !consume_integer(records, "starting_colors.alpha_steps",
                                starting.alpha_steps, error)
            || !consume_double(records, "starting_colors.red_minimum",
                               starting.red_minimum, error)
            || !consume_double(records, "starting_colors.red_maximum",
                               starting.red_maximum, error)
            || !consume_double(records, "starting_colors.green_minimum",
                               starting.green_minimum, error)
            || !consume_double(records, "starting_colors.green_maximum",
                               starting.green_maximum, error)
            || !consume_double(records, "starting_colors.blue_minimum",
                               starting.blue_minimum, error)
            || !consume_double(records, "starting_colors.blue_maximum",
                               starting.blue_maximum, error)
            || !consume_double(records, "starting_colors.alpha_minimum",
                               starting.alpha_minimum, error)
            || !consume_double(records, "starting_colors.alpha_maximum",
                               starting.alpha_maximum, error)) {
            return false;
        }
    }
    if (setup_version >= 11U) {
        StartingColorConfig& starting = candidate.starting_colors;
        if (!consume_bool(records, "starting_colors.kaleidoscope.enabled",
                          starting.kaleidoscope.enabled, error)
            || !consume_integer(
                records,
                "starting_colors.kaleidoscope.mirrored_segments",
                starting.kaleidoscope.mirrored_segments, error)
            || !consume_double(
                records, "starting_colors.kaleidoscope.rotation_degrees",
                starting.kaleidoscope.rotation_degrees, error)
            || !consume_double(records, "starting_colors.kaleidoscope.mix",
                               starting.kaleidoscope.mix, error)
            || !consume_bool(records, "starting_colors.domain_warp.enabled",
                             starting.domain_warp.enabled, error)
            || !consume_double(records, "starting_colors.domain_warp.strength",
                               starting.domain_warp.strength, error)
            || !consume_double(records, "starting_colors.domain_warp.scale",
                               starting.domain_warp.scale, error)
            || !consume_integer(records, "starting_colors.domain_warp.octaves",
                                starting.domain_warp.octaves, error)
            || !consume_integer(
                records, "starting_colors.domain_warp.cycles_per_loop",
                starting.domain_warp.cycles_per_loop, error)
            || !consume_integer(records, "starting_colors.domain_warp.seed",
                                starting.domain_warp.seed, error)) {
            return false;
        }
    }
    if (setup_version >= 2U
        && !consume_string(records, "surface.obj_path",
                           candidate.surface.obj_path, error)) {
        return false;
    }
    if (setup_version >= 5U) {
        if (!consume_bounded_string(records, "surface.obj_sha256",
                                    kSha256HexBytes,
                                    candidate.surface.obj_sha256, error)
            || !consume_bounded_string(records, "surface.obj_basename",
                                       kMaximumAttachmentBasenameBytes,
                                       candidate.surface.obj_basename, error)
            || !is_lowercase_sha256(candidate.surface.obj_sha256)) {
            return fail(error,
                        record_error("Invalid custom OBJ attachment metadata at setup key",
                                     "surface.obj_sha256"));
        }
    }
    if (setup_version >= 13U) {
        PlaneDisplacementConfig& plane =
            candidate.surface.plane_displacement;
        if (!consume_bool(records, "surface.plane_displacement.enabled",
                          plane.enabled, error)
            || !consume_double(records, "surface.plane_displacement.minimum",
                               plane.minimum, error)
            || !consume_double(records, "surface.plane_displacement.maximum",
                               plane.maximum, error)
            || !consume_double(records, "surface.plane_displacement.midpoint",
                               plane.midpoint, error)
            || !consume_integer(
                records, "surface.plane_displacement.pixels_per_node",
                plane.pixels_per_node, error)
            || !consume_string(records, "surface.plane_displacement.path",
                               plane.path, error)
            || !consume_bounded_string(
                records, "surface.plane_displacement.sha256",
                kSha256HexBytes, plane.sha256, error)
            || !consume_bounded_string(
                records, "surface.plane_displacement.basename",
                kMaximumAttachmentBasenameBytes, plane.basename, error)
            || !is_lowercase_sha256(plane.sha256)) {
            return fail(
                error,
                record_error(
                    "Invalid plane-displacement attachment metadata at setup key",
                    "surface.plane_displacement.sha256"));
        }
    }
    if (setup_version >= 20U) {
        EnvironmentMapConfig& environment = surface.environment_map;
        if (!consume_bool(records, "surface.environment_map.enabled",
                          environment.enabled, error)
            || !consume_enum(records, "surface.environment_map.encoding",
                             environment.encoding, kEnvironmentMapEncodings,
                             error)
            || !consume_double(
                records, "surface.environment_map.rotation_degrees",
                environment.rotation_degrees, error)
            || !consume_double(
                records, "surface.environment_map.exposure_stops",
                environment.exposure_stops, error)
            || !consume_double(records, "surface.environment_map.intensity",
                               environment.intensity, error)
            || !consume_double(records, "surface.environment_map.mix",
                               environment.mix, error)
            || !consume_string(records, "surface.environment_map.path",
                               environment.path, error)
            || !consume_bounded_string(
                records, "surface.environment_map.sha256", kSha256HexBytes,
                environment.sha256, error)
            || !consume_bounded_string(
                records, "surface.environment_map.basename",
                kMaximumAttachmentBasenameBytes, environment.basename, error)
            || !is_lowercase_sha256(environment.sha256)) {
            return fail(
                error,
                record_error(
                    "Invalid environment-map attachment metadata at setup key",
                    "surface.environment_map.sha256"));
        }
        MeshConstructionConfig& construction = surface.mesh_construction;
        if (!consume_enum(records, "surface.mesh_construction.mode",
                          construction.mode, kMeshConstructionModes, error)
            || !consume_enum(
                records, "surface.mesh_construction.fragmentation",
                construction.fragmentation, kMeshFragmentations, error)
            || !consume_integer(
                records, "surface.mesh_construction.target_fragments",
                construction.target_fragments, error)
            || !consume_integer(
                records, "surface.mesh_construction.cycles_per_loop",
                construction.cycles_per_loop, error)
            || !consume_double(
                records, "surface.mesh_construction.phase_degrees",
                construction.phase_degrees, error)
            || !consume_double(records, "surface.mesh_construction.distance",
                               construction.distance, error)
            || !consume_double(
                records, "surface.mesh_construction.rotation_degrees",
                construction.rotation_degrees, error)
            || !consume_double(
                records, "surface.mesh_construction.minimum_scale",
                construction.minimum_scale, error)
            || !consume_double(records, "surface.mesh_construction.stagger",
                               construction.stagger, error)
            || !consume_integer(records, "surface.mesh_construction.seed",
                                construction.seed, error)) {
            return false;
        }
    }

    if (setup_version < 16U) {
        constexpr double kLegacyTiltDegrees =
            -0.35 * 180.0 / 3.141592653589793238462643383279502884;
        constexpr double kLegacyTurnDegrees =
            0.55 * 180.0 / 3.141592653589793238462643383279502884;
        surface.composite_backfaces = true;
        surface.normalize_obj = true;
        surface.camera_distance = 3.4;
        surface.focal_length = 2.5;
        surface.light_direction_x = -0.45;
        surface.light_direction_y = -0.55;
        surface.light_direction_z = 0.75;
        surface.light_ambient = 0.28;
        surface.light_diffuse = 0.72;
        surface.rotation_order = SurfaceRotationOrder::XYZ;
        if (surface.mapping == SurfaceMapping::Plane
            && !surface.plane_displacement.enabled) {
            surface.projection = SurfaceProjection::Orthographic;
            surface.sizing = SurfaceSizing::Stretch;
            surface.outside = SurfaceOutside::Reflect;
            // The legacy 2D sampler inverse-rotated output pixel offsets.
            // The explicit 3D Plane rotates the surface itself, so the same
            // established image orientation uses the opposite authored Z
            // angle and loop direction.
            surface.rotation_z_turns_per_loop =
                legacy_surface_turns == (std::numeric_limits<int>::min)()
                    ? (std::numeric_limits<int>::max)()
                    : -legacy_surface_turns;
            surface.rotation_z_degrees = -legacy_surface_phase;
        } else if (surface.mapping == SurfaceMapping::Sphere) {
            surface.projection = SurfaceProjection::Orthographic;
            surface.sizing = SurfaceSizing::ShortSide;
            surface.size_percent = 92.0;
            surface.outside = SurfaceOutside::Transparent;
            surface.rotation_y_turns_per_loop = legacy_surface_turns;
            surface.rotation_y_degrees = legacy_surface_phase;
        } else {
            surface.projection = SurfaceProjection::Perspective;
            surface.sizing = SurfaceSizing::ShortSide;
            surface.size_percent = 104.0;
            surface.outside = SurfaceOutside::Transparent;
            surface.rotation_x_degrees = kLegacyTiltDegrees;
            surface.rotation_y_turns_per_loop = legacy_surface_turns;
            surface.rotation_y_degrees = legacy_surface_phase;
            if (surface.mapping == SurfaceMapping::Cube
                || surface.mapping == SurfaceMapping::CustomObj
                || (surface.mapping == SurfaceMapping::Plane
                    && surface.plane_displacement.enabled)) {
                surface.rotation_y_degrees += kLegacyTurnDegrees;
            }
            if (surface.mapping == SurfaceMapping::Cylinder) {
                // Legacy Cylinder spun around local Y before applying the
                // fixed X tilt. Preserve that composition explicitly.
                surface.rotation_order = SurfaceRotationOrder::YXZ;
            }
        }
    }

    if (setup_version >= 4U) {
        std::size_t palette_color_count = 0U;
        if (!consume_bool(records, "palette.enabled", candidate.palette.enabled, error)
            || !consume_string(records, "palette.name", candidate.palette.name, error)
            || (setup_version >= 11U
                && !consume_count(records, "palette.columns",
                                  kMaximumPaletteColors,
                                  candidate.palette.columns, error))
            || !consume_count(records, "palette.colors.count",
                              kMaximumPaletteColors, palette_color_count, error)) {
            return false;
        }
        candidate.palette.colors.clear();
        candidate.palette.colors.resize(palette_color_count);
        for (std::size_t index = 0U; index < palette_color_count; ++index) {
            PaletteColor& color = candidate.palette.colors[index];
            if (!consume_double(records, indexed_key("palette.colors", index, "red"),
                                color.red, error)
                || !consume_double(records, indexed_key("palette.colors", index, "green"),
                                   color.green, error)
                || !consume_double(records, indexed_key("palette.colors", index, "blue"),
                                   color.blue, error)) {
                return false;
            }
            if (setup_version >= 10U
                && !consume_double(
                    records, indexed_key("palette.colors", index, "alpha"),
                    color.alpha, error)) {
                return false;
            }
            if (setup_version >= 11U
                && (!consume_string(
                        records,
                        indexed_key("palette.colors", index, "name"),
                        color.name, error)
                    || !consume_enum(
                        records,
                        indexed_key("palette.colors", index, "encoding"),
                        color.encoding, kPaletteColorEncodings, error))) {
                return false;
            }
        }
        if (!consume_bool(records, "transform.flip_horizontal",
                          candidate.transform.flip_horizontal, error)
            || !consume_bool(records, "transform.flip_vertical",
                             candidate.transform.flip_vertical, error)
            || !consume_enum(records, "transform.mirror",
                             candidate.transform.mirror, kMirrorModes, error)) {
            return false;
        }
    }

    if (setup_version >= 6U
        && (!consume_bool(records, "motion.enabled", candidate.motion.enabled,
                          error)
            || !consume_enum(records, "motion.path", candidate.motion.path,
                             kLayerMotionPaths, error)
            || !consume_double(records, "motion.center_x",
                               candidate.motion.center_x, error)
            || !consume_double(records, "motion.center_y",
                               candidate.motion.center_y, error)
            || !consume_double(records, "motion.travel_x",
                               candidate.motion.travel_x, error)
            || !consume_double(records, "motion.travel_y",
                               candidate.motion.travel_y, error)
            || !consume_integer(records, "motion.cycles_x",
                                candidate.motion.cycles_x, error)
            || !consume_integer(records, "motion.cycles_y",
                                candidate.motion.cycles_y, error)
            || !consume_double(records, "motion.phase_degrees",
                               candidate.motion.phase_degrees, error)
            || !consume_integer(records, "motion.rotations_per_loop",
                                candidate.motion.rotations_per_loop, error)
            || (setup_version >= 7U
                && !consume_double(records, "motion.rotation_offset_degrees",
                                   candidate.motion.rotation_offset_degrees,
                                   error))
            || !consume_double(records, "motion.scale_pulse",
                               candidate.motion.scale_pulse, error))) {
        return false;
    }
    if (setup_version >= 7U
        && !consume_path_binding_records(records, "motion.custom_path.",
                                         candidate.motion.custom_path,
                                         error)) {
        return false;
    }

    if (!consume_integer(records, "output.bit_depth", candidate.output.bit_depth, error)) {
        return false;
    }
    if (setup_version >= 2U
        && !consume_integer(records, "output.png_compression_level",
                            candidate.output.png_compression_level, error)) {
        return false;
    }
    if (!consume_bool(records, "output.dither_enabled", candidate.output.dither_enabled, error)
        || !consume_enum(records, "output.dither_method", candidate.output.dither_method, kDitherMethods, error)) {
        return false;
    }
    if (setup_version >= 3U) {
        if (!consume_bool(records, "output.write_alpha",
                          candidate.output.write_alpha, error)) {
            return false;
        }
    } else {
        candidate.output.write_alpha = candidate.alpha.enabled;
    }
    if (!consume_string(records, "output.output_directory", candidate.output.output_directory, error)
        || !consume_string(records, "output.filename_prefix", candidate.output.filename_prefix, error)
        || !consume_integer(records, "output.first_frame_number", candidate.output.first_frame_number, error)
        || !consume_integer(records, "output.filename_digits", candidate.output.filename_digits, error)
        || !consume_bool(records, "output.overwrite_existing", candidate.output.overwrite_existing, error)) {
        return false;
    }

    if (!records.empty()) {
        return fail(error, record_error("Unknown setup key for format v"
                                        + std::to_string(setup_version),
                                        records.begin()->first));
    }

    // Format v1 files written by early builds could retain a now-meaningless
    // dither toggle while selecting full-float EXR. Accept and normalize those
    // files; no integer quantization occurs at 32-bit output.
    if (candidate.output.bit_depth == 32) {
        candidate.output.dither_enabled = false;
    }

    const ValidationResult validation = enforce_particle_workload
        ? validate(candidate)
        : detail::validate_render_config_structure(candidate);
    if (!validation.ok) {
        return fail(error, "Loaded setup failed validation: " + validation.message);
    }
    return true;
}

void remember_preserved(ConfigCompatibility& compatibility,
                        std::string key,
                        std::string value,
                        bool rejected) {
    const auto duplicate = std::find_if(
        compatibility.records.begin(), compatibility.records.end(),
        [&key, &value](const PreservedConfigRecord& record) {
            return record.key == key && record.value == value;
        });
    if (duplicate == compatibility.records.end()) {
        compatibility.records.push_back(
            {std::move(key), std::move(value), rejected});
    } else if (rejected) {
        duplicate->rejected = true;
    }
}

void remember_note(ConfigCompatibility& compatibility, std::string note) {
    if (std::find(compatibility.repair_notes.begin(),
                  compatibility.repair_notes.end(), note)
        == compatibility.repair_notes.end()) {
        compatibility.repair_notes.push_back(std::move(note));
    }
}

bool safe_record_value(std::string_view value) {
    if (value.size() > kMaximumLineBytes) return false;
    for (const char raw : value) {
        const unsigned char byte = static_cast<unsigned char>(raw);
        if (byte < 0x20U || byte > 0x7eU) return false;
    }
    return true;
}

void extract_rejected_envelope(Records& records,
                               Records& authored,
                               ConfigCompatibility& compatibility) {
    constexpr std::string_view prefix = "compatibility.rejected.";
    std::vector<std::pair<std::string, std::string>> envelope;
    for (auto iterator = records.begin(); iterator != records.end();) {
        if (starts_with(iterator->first, prefix)) {
            envelope.push_back(*iterator);
            iterator = records.erase(iterator);
        } else {
            ++iterator;
        }
    }
    if (envelope.empty()) return;

    Records encoded(envelope.begin(), envelope.end());
    std::size_t count = 0U;
    bool well_formed = false;
    const auto count_record = encoded.find("compatibility.rejected.count");
    if (count_record != encoded.end()) {
        std::uint64_t parsed = 0U;
        well_formed = parse_integer_exact(count_record->second, parsed)
                      && parsed <= kMaximumRecordCount;
        if (well_formed) count = static_cast<std::size_t>(parsed);
    }
    std::set<std::string> consumed;
    std::vector<std::pair<std::string, std::string>> recovered;
    if (well_formed) {
        for (std::size_t index = 0U; index < count; ++index) {
            const std::string base = "compatibility.rejected."
                                     + std::to_string(index);
            const auto key_record = encoded.find(base + ".key");
            const auto value_record = encoded.find(base + ".value");
            std::string key;
            std::string value;
            if (key_record == encoded.end() || value_record == encoded.end()
                || !percent_decode(key_record->second, key)
                || !percent_decode(value_record->second, value)
                || !valid_key(key) || !safe_record_value(value)) {
                well_formed = false;
                break;
            }
            recovered.emplace_back(std::move(key), std::move(value));
        }
    }
    if (well_formed) {
        consumed.insert("compatibility.rejected.count");
        for (std::size_t index = 0U; index < count; ++index) {
            const std::string base = "compatibility.rejected."
                                     + std::to_string(index);
            consumed.insert(base + ".key");
            consumed.insert(base + ".value");
        }
        std::set<std::string> retried_keys;
        for (const auto& item : recovered) {
            if (retried_keys.insert(item.first).second) {
                authored[item.first] = item.second;
                records[item.first] = item.second;
            } else {
                remember_preserved(compatibility, item.first, item.second,
                                   true);
                remember_note(
                    compatibility,
                    "Kept an alternate original value for field '"
                        + item.first + "' without using it.");
            }
        }
    }
    if (!well_formed) {
        remember_note(compatibility,
                      "Kept a malformed compatibility recovery envelope without using it.");
    }
    for (const auto& item : envelope) {
        if (consumed.find(item.first) == consumed.end()) {
            remember_preserved(compatibility, item.first, item.second, true);
        }
    }
}

void repair_unsafe_particle_workloads(Records& records,
                                      std::uint32_t setup_version,
                                      ConfigCompatibility& compatibility) {
    int width = 0;
    int height = 0;
    std::size_t effect_count = 0U;
    const auto width_record = records.find("canvas.width");
    const auto height_record = records.find("canvas.height");
    const auto count_record = records.find("effects.count");
    if (width_record == records.end() || height_record == records.end()
        || count_record == records.end()
        || !parse_integer_exact(width_record->second, width)
        || !parse_integer_exact(height_record->second, height)
        || !parse_integer_exact(count_record->second, effect_count)
        || effect_count > kMaximumEffects
        // Every real effect contributes many distinct records. This weaker
        // necessary condition is enough to keep a tiny hostile document from
        // turning pre-recovery inspection into billions of empty iterations.
        || effect_count > records.size()
        || width <= 0 || height <= 0) {
        return;
    }
    std::size_t budget = 0U;
    if (!detail::particle_stamp_budget_for_canvas(width, height, budget)) {
        return;
    }
    std::size_t admitted_work = 0U;

    for (std::size_t index = 0U; index < effect_count; ++index) {
        const std::string enabled_key = indexed_key("effects", index, "enabled");
        const auto enabled_record = records.find(enabled_key);
        const auto type_record = records.find(indexed_key("effects", index, "type"));
        const auto intensity_record = records.find(
            indexed_key("effects", index, "intensity"));
        const auto magnitude_record = records.find(
            indexed_key("effects", index, "magnitude"));
        const auto frequency_record = records.find(
            indexed_key("effects", index, "frequency"));
        const auto trail_record = records.find(
            indexed_key("effects", index, "secondary"));
        const auto radius_record = records.find(
            indexed_key("effects", index, "radius_pixels"));
        if (enabled_record == records.end() || type_record == records.end()
            || intensity_record == records.end()
            || magnitude_record == records.end()
            || frequency_record == records.end()
            || trail_record == records.end() || radius_record == records.end()
            || type_record->second != "particle_field") {
            continue;
        }
        bool enabled = false;
        double intensity = 0.0;
        double magnitude = 0.0;
        double frequency = 0.0;
        double trail = 0.0;
        double radius = 0.0;
        if (!parse_bool_exact(enabled_record->second, enabled) || !enabled
            || !parse_double_exact(intensity_record->second, intensity)
            || intensity <= 0.0
            || !parse_double_exact(magnitude_record->second, magnitude)
            || !parse_double_exact(frequency_record->second, frequency)
            || frequency < 1.0
            || frequency
                   > static_cast<double>((std::numeric_limits<int>::max)())
            || std::floor(frequency) != frequency
            || !parse_double_exact(trail_record->second, trail)
            || trail < 0.0 || trail > 1.0
            || !parse_double_exact(radius_record->second, radius)
            || radius <= 0.0) {
            continue;
        }

        ParticleRenderProfile profile = ParticleRenderProfile::LegacyGlow;
        ParticleOrientation orientation = ParticleOrientation::Fixed;
        double size_variation = 0.0;
        bool workload_metadata_valid = true;
        if (setup_version >= 15U) {
            const auto profile_record = records.find(
                indexed_key("effects", index, "particle_profile"));
            const auto variation_record = records.find(
                indexed_key("effects", index, "particle_size_variation"));
            const auto orientation_record = records.find(
                indexed_key("effects", index, "particle_orientation"));
            if (profile_record == records.end()
                || variation_record == records.end()
                || orientation_record == records.end()
                || (profile_record->second != "legacy_glow"
                    && profile_record->second != "defined")
                || (orientation_record->second != "fixed"
                    && orientation_record->second != "follow_motion"
                    && orientation_record->second != "random")
                || !parse_double_exact(variation_record->second,
                                       size_variation)
                || size_variation < 0.0 || size_variation > 1.0) {
                // The typed recovery pass can replace malformed v15 fields,
                // but it runs after this workload gate. Conservatively
                // disable the effect now instead of letting a later semantic
                // failure discard the entire effects collection. The
                // authored enabled value and malformed field are both kept
                // by the compatibility envelope for deliberate repair.
                workload_metadata_valid = false;
            } else {
                profile = profile_record->second == "defined"
                    ? ParticleRenderProfile::Defined
                    : ParticleRenderProfile::LegacyGlow;
                if (orientation_record->second == "follow_motion") {
                    orientation = ParticleOrientation::FollowMotion;
                } else if (orientation_record->second == "random") {
                    orientation = ParticleOrientation::Random;
                }
            }
        }
        EffectConfig particle;
        particle.type = EffectType::ParticleField;
        particle.enabled = enabled;
        particle.intensity = intensity;
        particle.magnitude = magnitude;
        particle.frequency = frequency;
        particle.secondary = trail;
        particle.radius_pixels = radius;
        particle.particle_profile = profile;
        particle.particle_size_variation = size_variation;
        particle.particle_orientation = orientation;
        std::size_t work = 0U;
        const bool unsafe = !workload_metadata_valid
                            || !detail::particle_effect_stamp_workload(
                                width, height, particle, work)
                            || work > budget - admitted_work;
        if (!unsafe) {
            admitted_work += work;
            continue;
        }

        // Rejected records are retried automatically on the next load. Keep
        // the old enabled state under a deliberately non-applying, render-
        // scoped compatibility key instead, so reducing the workload while
        // leaving the effect disabled cannot silently re-enable it later.
        const std::string recovered_enabled_key = indexed_key(
            "effects", index, "recovery_unsafe_particle_enabled");
        remember_preserved(compatibility, recovered_enabled_key,
                           enabled_record->second, false);
        enabled_record->second = "0";
        remember_note(
            compatibility,
            "Disabled unsafe particle workload at field '" + enabled_key
                + "' while preserving its authored enabled value under non-applying recovery field '"
                + recovered_enabled_key
                + "'; reduce particle count, radius, size variation, or trail amount before re-enabling it deliberately.");
    }
}

bool record_belongs_to_version(std::string_view key,
                               std::uint32_t setup_version) {
    return !((setup_version < 2U && setup_v2_record(key))
             || (setup_version < 3U && setup_v3_record(key))
             || (setup_version < 4U && setup_v4_record(key))
             || (setup_version < 5U && setup_v5_record(key))
             || (setup_version < 6U && setup_v6_record(key))
             || (setup_version < 7U && setup_v7_record(key))
             || (setup_version < 8U && setup_v8_record(key))
             || (setup_version < 10U && setup_v10_record(key))
             || (setup_version < 11U && setup_v11_record(key))
             || (setup_version < 12U && setup_v12_record(key))
             || (setup_version < 13U && setup_v13_record(key))
             || (setup_version < 14U && setup_v14_record(key))
             || (setup_version < 15U && setup_v15_record(key))
             || (setup_version < 16U && setup_v16_record(key))
             || (setup_version < 17U && setup_v17_record(key))
             || (setup_version < 18U && setup_v18_record(key))
             || (setup_version < 19U && setup_v19_record(key))
             || (setup_version < 20U && setup_v20_record(key)));
}

bool build_default_records(std::uint32_t setup_version,
                           Records& defaults,
                           std::string* error) {
    std::string serialized;
    if (!serialize_setup(default_config(), serialized, error)) return false;
    std::uint32_t parsed_version = 0U;
    if (!parse_records(serialized, defaults, parsed_version, error)) return false;
    for (auto iterator = defaults.begin(); iterator != defaults.end();) {
        if (!record_belongs_to_version(iterator->first, setup_version)) {
            iterator = defaults.erase(iterator);
        } else {
            ++iterator;
        }
    }
    if (setup_version < 16U) {
        defaults.emplace("surface.rotations_per_loop", "0");
        defaults.emplace("surface.phase_degrees", "0");
    }
    return true;
}

bool quoted_error_key(std::string_view message, std::string& key) {
    const std::size_t end = message.rfind('\'');
    if (end == std::string_view::npos) return false;
    const std::size_t start = message.rfind('\'', end - 1U);
    if (start == std::string_view::npos || start + 1U == end) return false;
    key.assign(message.substr(start + 1U, end - start - 1U));
    return valid_key(key);
}

enum class RecoveryAttempt {
    Success,
    SemanticFailure,
    Unrecoverable,
};

RecoveryAttempt decode_with_record_repair(
    Records& working,
    const Records& defaults,
    const Records& authored,
    std::uint32_t setup_version,
    RenderConfig& destination,
    ConfigCompatibility& compatibility,
    std::string* error,
    bool enforce_particle_workload) {
    // Every unsuccessful repair either erases one authored key or inserts/
    // replaces one key with its default. This exact state-transition bound
    // prevents cycles without imposing an unrelated attempt ceiling.
    const std::size_t remaining =
        (std::numeric_limits<std::size_t>::max)() - working.size();
    const std::size_t maximum_attempts =
        defaults.size() >= remaining
            ? (std::numeric_limits<std::size_t>::max)()
            : working.size() + defaults.size() + 1U;
    for (std::size_t attempt = 0U; attempt < maximum_attempts; ++attempt) {
        Records decoding = working;
        RenderConfig candidate;
        std::string failure;
        if (deserialize_setup(decoding, setup_version, candidate, &failure,
                              enforce_particle_workload)) {
            destination = std::move(candidate);
            if (error != nullptr) error->clear();
            return RecoveryAttempt::Success;
        }
        std::string key;
        if (!quoted_error_key(failure, key)) {
            if (error != nullptr) *error = std::move(failure);
            return starts_with(error != nullptr ? *error : failure,
                               "Loaded setup failed validation:")
                       ? RecoveryAttempt::SemanticFailure
                       : RecoveryAttempt::Unrecoverable;
        }
        const auto current = working.find(key);
        const auto fallback = defaults.find(key);
        if (starts_with(failure, "Unknown setup key")) {
            if (current == working.end()) {
                if (error != nullptr) *error = std::move(failure);
                return RecoveryAttempt::Unrecoverable;
            }
            const auto original = authored.find(key);
            if (original != authored.end()) {
                remember_preserved(compatibility, key, original->second, false);
                remember_note(compatibility,
                              "Kept unknown field '" + key
                                  + "' without using it.");
            }
            working.erase(current);
            continue;
        }
        if (fallback == defaults.end()) {
            if (error != nullptr) *error = std::move(failure);
            return RecoveryAttempt::Unrecoverable;
        }
        if (current == working.end()) {
            working.emplace(key, fallback->second);
            remember_note(compatibility,
                          "Rebuilt missing field '" + key
                              + "' from a safe default.");
            continue;
        }
        if (current->second == fallback->second) {
            if (error != nullptr) *error = std::move(failure);
            return RecoveryAttempt::SemanticFailure;
        }
        const auto original = authored.find(key);
        if (original != authored.end()) {
            remember_preserved(compatibility, key, original->second, true);
        }
        current->second = fallback->second;
        remember_note(compatibility,
                      "Replaced unusable field '" + key
                          + "' with a safe default and kept its original value.");
    }
    return RecoveryAttempt::Unrecoverable;
}

std::string recovery_group(std::string_view key) {
    constexpr std::array<std::string_view, 24U> groups{{
        "paths.", "timing.music.", "timing.clock.", "canvas.",
        "output.", "waves.", "swings.", "effects.", "layer_clock.",
        "palette.", "surface.", "source_image.", "motion.", "alpha.",
        "quantization.", "transform.", "audio_reactive.", "appearance.",
        "rhythm.", "audio_response_defaults.",
        "starting_colors.",
        "post_process.", "parameter_lfos.",
        "live.",
    }};
    for (const std::string_view group : groups) {
        if (starts_with(key, group)) return std::string(group);
    }
    return std::string(key);
}

bool collection_recovery_group(std::string_view group) {
    return group == "paths." || group == "waves." || group == "swings."
           || group == "effects." || group == "palette."
           || group == "timing.music." || group == "layer_clock."
           || group == "live." || group == "parameter_lfos.";
}

bool recover_setup_records(Records records,
                           std::uint32_t setup_version,
                           RenderConfig& destination,
                           ConfigCompatibility& compatibility,
                           std::string* error,
                           bool enforce_particle_workload = true) {
    Records authored = records;
    extract_rejected_envelope(records, authored, compatibility);
    authored = records;
    if (enforce_particle_workload) {
        repair_unsafe_particle_workloads(records, setup_version,
                                         compatibility);
    }
    // When enabled, the safety repair preserves the original enabled value
    // under its explicit non-applying recovery key. Treat the resulting
    // records as authored for every later recovery strategy too; otherwise an
    // unrelated semantic error can make the greedy group pass retry the unsafe
    // enabled value and discard the entire effects collection. The standalone
    // layer path deliberately has no canvas and therefore performs no such
    // repair here.
    authored = records;

    Records defaults;
    if (!build_default_records(setup_version, defaults, error)) return false;

    Records working = records;
    RenderConfig candidate;
    const RecoveryAttempt direct = decode_with_record_repair(
        working, defaults, authored, setup_version, candidate,
        compatibility, error, enforce_particle_workload);
    if (direct == RecoveryAttempt::Success) {
        destination = std::move(candidate);
        return true;
    }

    // Typed parsing succeeded but one or more combinations were semantically
    // unsafe. Rebuild from the known-good template and admit independent field
    // groups one at a time. This keeps valid portions instead of rejecting the
    // entire save while avoiding use of a partially invalid configuration.
    Records accepted = defaults;
    std::map<std::string, Records> groups;
    for (const auto& record : authored) {
        if (record_belongs_to_version(record.first, setup_version)) {
            groups[recovery_group(record.first)].insert(record);
        }
    }
    ConfigCompatibility greedy_compatibility = compatibility;
    // Some otherwise-valid groups depend on another group (for example a
    // wave can bind to a reusable path, and a music clock depends on its
    // analysis). Retry deferred groups whenever admitting another group may
    // have satisfied that dependency. Map ordering must never decide which
    // valid user settings survive recovery.
    bool made_progress = true;
    while (made_progress && !groups.empty()) {
        made_progress = false;
        for (auto grouped = groups.begin(); grouped != groups.end();) {
            Records trial = accepted;
            if (collection_recovery_group(grouped->first)) {
                for (auto iterator = trial.begin(); iterator != trial.end();) {
                    if (starts_with(iterator->first, grouped->first)) {
                        iterator = trial.erase(iterator);
                    } else {
                        ++iterator;
                    }
                }
            }
            for (const auto& record : grouped->second) {
                trial[record.first] = record.second;
            }
            ConfigCompatibility trial_compatibility = greedy_compatibility;
            RenderConfig trial_candidate;
            std::string trial_error;
            const RecoveryAttempt result = decode_with_record_repair(
                trial, defaults, authored, setup_version, trial_candidate,
                trial_compatibility, &trial_error,
                enforce_particle_workload);
            if (result == RecoveryAttempt::Success) {
                accepted = std::move(trial);
                candidate = std::move(trial_candidate);
                greedy_compatibility = std::move(trial_compatibility);
                grouped = groups.erase(grouped);
                made_progress = true;
            } else {
                ++grouped;
            }
        }
    }
    for (const auto& grouped : groups) {
        for (const auto& record : grouped.second) {
            remember_preserved(greedy_compatibility, record.first,
                               record.second, true);
        }
        remember_note(greedy_compatibility,
                      "Kept but did not use invalid field group '"
                          + grouped.first + "'.");
    }
    Records final_records = accepted;
    ConfigCompatibility final_compatibility = greedy_compatibility;
    const RecoveryAttempt final_result = decode_with_record_repair(
        final_records, defaults, authored, setup_version, candidate,
        final_compatibility, error, enforce_particle_workload);
    if (final_result != RecoveryAttempt::Success) return false;
    compatibility = std::move(final_compatibility);
    destination = std::move(candidate);
    return true;
}

void distribute_compatibility(RenderConfig& config,
                              ConfigCompatibility compatibility) {
    for (PreservedConfigRecord& record : compatibility.records) {
        if (starts_with(record.key, "timing.music.")) {
            config.clock.music.compatibility.records.push_back(
                std::move(record));
        } else if (render_record_key(record.key)) {
            config.source_compatibility.records.push_back(std::move(record));
        } else {
            config.output_compatibility.records.push_back(std::move(record));
        }
    }
    for (std::string& note : compatibility.repair_notes) {
        std::string key;
        if (quoted_error_key(note, key)
            && starts_with(key, "timing.music.")) {
            config.clock.music.compatibility.repair_notes.push_back(
                std::move(note));
        } else if (!key.empty() && render_record_key(key)) {
            config.source_compatibility.repair_notes.push_back(std::move(note));
        } else {
            config.output_compatibility.repair_notes.push_back(std::move(note));
        }
    }
}

bool append_setup_compatibility(std::string& serialized,
                                const ConfigCompatibility& compatibility,
                                std::string* error) {
    Records existing;
    std::uint32_t version = 0U;
    if (!parse_records(serialized, existing, version, error)) return false;
    std::map<std::string, std::string> unknown;
    std::vector<PreservedConfigRecord> rejected;
    for (const PreservedConfigRecord& record : compatibility.records) {
        if (!valid_key(record.key) || !safe_record_value(record.value)) {
            return fail(error,
                        "Cannot save malformed preserved field '"
                            + record.key
                            + "'; no preserved data was discarded.");
        }
        if (!record.rejected
            && !starts_with(record.key, "compatibility.rejected.")
            && existing.find(record.key) == existing.end()) {
            const auto inserted = unknown.emplace(record.key, record.value);
            if (!inserted.second && inserted.first->second != record.value) {
                rejected.push_back(record);
            }
        } else {
            rejected.push_back(record);
        }
    }
    const auto append_line = [&](std::string_view key,
                                 std::string_view value) -> bool {
        std::size_t final_size = serialized.size();
        if (key.size() > kMaximumSetupBytes - final_size) {
            return fail(error,
                        "Preserved compatibility data exceeds the signed-int setup limit.");
        }
        final_size += key.size();
        if (1U > kMaximumSetupBytes - final_size) {
            return fail(error,
                        "Preserved compatibility data exceeds the signed-int setup limit.");
        }
        ++final_size;
        if (value.size() > kMaximumSetupBytes - final_size
            || 1U > kMaximumSetupBytes - final_size - value.size()) {
            return fail(error,
                        "Preserved compatibility data exceeds the signed-int setup limit.");
        }
        serialized.append(key);
        serialized.push_back('\t');
        serialized.append(value);
        serialized.push_back('\n');
        return true;
    };
    for (const auto& record : unknown) {
        if (!append_line(record.first, record.second)) return false;
    }
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
    if (!rejected.empty()
        && !append_line("compatibility.rejected.count",
                        std::to_string(rejected.size()))) {
        return false;
    }
    for (std::size_t index = 0U; index < rejected.size(); ++index) {
        std::string key;
        std::string value;
        if (!percent_encode(rejected[index].key, key)
            || !percent_encode(rejected[index].value, value)
            || !append_line("compatibility.rejected."
                                + std::to_string(index) + ".key",
                            key)
            || !append_line("compatibility.rejected."
                                + std::to_string(index) + ".value",
                            value)) {
            return false;
        }
    }
    return true;
}

#if defined(_WIN32)

std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                              utf8.data(), static_cast<int>(utf8.size()),
                                              nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            utf8.data(), static_cast<int>(utf8.size()),
                            wide.data(), required) != required) {
        return {};
    }
    return wide;
}

std::string windows_error_message(std::string_view operation, DWORD code) {
    return std::string(operation) + " failed (Windows error " + std::to_string(code) + ").";
}

bool atomic_write_setup(const std::string& path,
                        const std::string& contents,
                        std::string* error) {
    const std::wstring destination = utf8_to_wide(path);
    if (destination.empty()) {
        return fail(error, "Setup path is not valid UTF-8 for Windows.");
    }

    const std::filesystem::path destination_path(destination);
    const std::filesystem::path filename = destination_path.filename();
    if (filename.empty() || filename == L"." || filename == L"..") {
        return fail(error, "Setup destination must name a file.");
    }
    std::filesystem::path directory = destination_path.parent_path();
    if (directory.empty()) {
        directory = L".";
    }

    static std::atomic_uint64_t sequence{0};
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::filesystem::path temporary_path;
    // Reserve a disjoint attempt range for each concurrent save. This avoids
    // needless CREATE_NEW collisions when stale temporary files are present.
    const std::uint64_t first = sequence.fetch_add(128U, std::memory_order_relaxed);
    for (unsigned int attempt = 0; attempt < 128U; ++attempt) {
        const std::uint64_t serial = first + static_cast<std::uint64_t>(attempt);
        const std::wstring temporary_name = L".pvt-setup-" + std::to_wstring(GetCurrentProcessId())
                                            + L"-" + std::to_wstring(serial) + L".tmp";
        temporary_path = directory / temporary_name;
        handle = CreateFileW(temporary_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            break;
        }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) {
            return fail(error, windows_error_message("Creating sibling temporary setup file", GetLastError()));
        }
    }
    if (handle == INVALID_HANDLE_VALUE) {
        return fail(error, "Could not allocate a unique sibling temporary setup file.");
    }

    bool success = true;
    DWORD failure = ERROR_SUCCESS;
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min)(
            contents.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!WriteFile(handle, contents.data() + offset, chunk, &written, nullptr)
            || written == 0U) {
            success = false;
            failure = GetLastError();
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (success && !FlushFileBuffers(handle)) {
        success = false;
        failure = GetLastError();
    }
    if (!CloseHandle(handle) && success) {
        success = false;
        failure = GetLastError();
    }
    if (!success) {
        DeleteFileW(temporary_path.c_str());
        return fail(error, windows_error_message("Writing temporary setup file", failure));
    }

    // FILE_ATTRIBUTE_TEMPORARY is useful while writing, but the installed setup
    // is permanent. Clear the hint before the atomic move so it cannot persist
    // on the destination and affect caching, indexing, or backup behavior.
    if (!SetFileAttributesW(temporary_path.c_str(), FILE_ATTRIBUTE_NORMAL)) {
        failure = GetLastError();
        DeleteFileW(temporary_path.c_str());
        return fail(error, windows_error_message(
                               "Preparing temporary setup file for installation", failure));
    }

    if (!detail::install_windows_temporary(temporary_path, destination_path,
                                           true, &failure)) {
        DeleteFileW(temporary_path.c_str());
        return fail(error, windows_error_message("Atomically replacing setup file", failure));
    }
    return true;
}

#else

class TemporaryFileGuard {
public:
    explicit TemporaryFileGuard(std::string path) : path_(std::move(path)) {}
    ~TemporaryFileGuard() {
        if (armed_) {
            ::unlink(path_.c_str());
        }
    }
    void release() { armed_ = false; }

private:
    std::string path_;
    bool armed_ = true;
};

std::string posix_error_message(std::string_view operation, int code) {
    return std::string(operation) + ": " + std::generic_category().message(code) + ".";
}

int fsync_retry(int descriptor) {
    int result = 0;
    do {
        result = ::fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    return result;
}

void sync_directory_best_effort(const std::filesystem::path& directory) {
#  if defined(O_DIRECTORY)
    constexpr int directory_flag = O_DIRECTORY;
#  else
    constexpr int directory_flag = 0;
#  endif
    const std::string native_directory = directory.string();
    const int descriptor = ::open(native_directory.c_str(), O_RDONLY | directory_flag);
    if (descriptor >= 0) {
        (void)fsync_retry(descriptor);
        (void)::close(descriptor);
    }
}

bool atomic_write_setup(const std::string& path,
                        const std::string& contents,
                        std::string* error) {
    const std::filesystem::path destination = detail::path_from_utf8(path);
    const std::filesystem::path filename = destination.filename();
    if (filename.empty() || filename == "." || filename == "..") {
        return fail(error, "Setup destination must name a file.");
    }
    std::filesystem::path directory = destination.parent_path();
    if (directory.empty()) {
        directory = ".";
    }

    // mkstemp intentionally creates new files with mode 0600. When replacing
    // an existing regular file, retain that file's explicit permission bits so
    // a harmless setup edit does not silently make a shared configuration
    // private. Symlinks and other special entries are never followed here.
    mode_t preserved_mode = 0;
    bool preserve_mode = false;
    struct stat destination_status {};
    const std::string native_destination = destination.string();
    if (::lstat(native_destination.c_str(), &destination_status) == 0) {
        if (S_ISDIR(destination_status.st_mode)) {
            return fail(error, "Setup destination is a directory.");
        }
        if (S_ISREG(destination_status.st_mode)) {
            preserved_mode = destination_status.st_mode & 07777;
            preserve_mode = true;
        }
    } else if (errno != ENOENT) {
        return fail(error, posix_error_message("Could not inspect setup destination", errno));
    }

    std::filesystem::path template_path = directory / ".pvt-setup-XXXXXX";
    std::string temporary = template_path.string();
    std::vector<char> mutable_template(temporary.begin(), temporary.end());
    mutable_template.push_back('\0');

    const int descriptor = ::mkstemp(mutable_template.data());
    if (descriptor < 0) {
        return fail(error, posix_error_message("Could not create sibling temporary setup file", errno));
    }
    temporary.assign(mutable_template.data());
    TemporaryFileGuard cleanup(temporary);

    bool success = true;
    int failure = 0;
    std::size_t offset = 0;
    while (offset < contents.size()) {
        ssize_t written = 0;
        do {
            written = ::write(descriptor, contents.data() + offset,
                              contents.size() - offset);
        } while (written < 0 && errno == EINTR);
        if (written <= 0) {
            success = false;
            failure = written < 0 && errno != 0 ? errno : EIO;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    // Apply the final mode after writing: POSIX may clear set-user-ID or
    // set-group-ID bits when file contents change.
    if (success && preserve_mode && ::fchmod(descriptor, preserved_mode) != 0) {
        success = false;
        failure = errno;
    }
    if (success && fsync_retry(descriptor) != 0) {
        success = false;
        failure = errno;
    }
    if (::close(descriptor) != 0 && success) {
        success = false;
        failure = errno;
    }
    if (!success) {
        return fail(error,
                    posix_error_message("Could not write and flush temporary setup file",
                                        failure));
    }

    if (::rename(temporary.c_str(), destination.string().c_str()) != 0) {
        return fail(error, posix_error_message("Could not atomically replace setup file", errno));
    }
    cleanup.release();
    sync_directory_best_effort(directory);
    return true;
}

#endif

} // namespace

namespace detail {

bool serialize_setup_config(const RenderConfig& config,
                            std::string& serialized,
                            std::string* error) {
    clear_error(error);
    try {
        return serialize_setup(config, serialized, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to serialize setup.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected error while serializing setup: ")
                               + exception.what());
    }
}

bool deserialize_setup_config(const std::string& serialized,
                              RenderConfig& destination,
                              std::string* error) {
    clear_error(error);
    try {
        if (serialized.size() > kMaximumSetupBytes) {
            return fail(error, "Setup data exceeds the signed-int input limit.");
        }
        Records records;
        std::uint32_t setup_version = 0U;
        if (!parse_records(serialized, records, setup_version, error)) {
            return false;
        }
        RenderConfig candidate;
        ConfigCompatibility compatibility;
        if (!recover_setup_records(std::move(records), setup_version,
                                   candidate, compatibility, error)) {
            return false;
        }
        distribute_compatibility(candidate, std::move(compatibility));
        destination = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error,
                    "Not enough memory to load setup; destination was not changed.");
    } catch (const std::exception& exception) {
        return fail(error,
                    std::string("Unexpected error while loading setup; destination was not changed: ")
                        + exception.what());
    }
}

bool serialize_setup_config_without_particle_admission(
    const RenderConfig& config,
    std::string& serialized,
    std::string* error) {
    clear_error(error);
    try {
        return serialize_setup(config, serialized, error, false);
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to serialize layer setup.");
    } catch (const std::exception& exception) {
        return fail(error,
                    std::string("Unexpected error while serializing layer setup: ")
                        + exception.what());
    }
}

bool deserialize_setup_config_without_particle_admission(
    const std::string& serialized,
    RenderConfig& destination,
    std::string* error) {
    clear_error(error);
    try {
        if (serialized.size() > kMaximumSetupBytes) {
            return fail(error, "Setup data exceeds the signed-int input limit.");
        }
        Records records;
        std::uint32_t setup_version = 0U;
        if (!parse_records(serialized, records, setup_version, error)) {
            return false;
        }
        RenderConfig candidate;
        ConfigCompatibility compatibility;
        if (!recover_setup_records(std::move(records), setup_version,
                                   candidate, compatibility, error, false)) {
            return false;
        }
        distribute_compatibility(candidate, std::move(compatibility));
        destination = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(
            error,
            "Not enough memory to load layer setup; destination was not changed.");
    } catch (const std::exception& exception) {
        return fail(
            error,
            std::string("Unexpected error while loading layer setup; destination was not changed: ")
                + exception.what());
    }
}

} // namespace detail

bool save_setup(const RenderConfig& config,
                const std::string& path,
                std::string* error) {
    clear_error(error);
    try {
        if (path.empty() || path.size() > kMaximumDecodedStringBytes
            || path.find('\0') != std::string::npos) {
            return fail(error, "Setup path is empty, contains a NUL byte, or exceeds the signed-int text API limit.");
        }
        std::string serialized;
        if (!detail::serialize_setup_config(config, serialized, error)) {
            return false;
        }
        ConfigCompatibility compatibility = config.source_compatibility;
        compatibility.records.insert(
            compatibility.records.end(),
            config.output_compatibility.records.begin(),
            config.output_compatibility.records.end());
        compatibility.records.insert(
            compatibility.records.end(),
            config.clock.music.compatibility.records.begin(),
            config.clock.music.compatibility.records.end());
        if (!append_setup_compatibility(serialized, compatibility, error)) {
            return false;
        }
        return atomic_write_setup(path, serialized, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to save setup.");
    } catch (const std::filesystem::filesystem_error& exception) {
        return fail(error, std::string("Filesystem error while saving setup: ") + exception.what());
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected error while saving setup: ") + exception.what());
    }
}

bool load_setup(const std::string& path,
                RenderConfig& destination,
                std::string* error) {
    clear_error(error);
    try {
        std::string contents;
        if (!read_setup_file(path, contents, error)) {
            return false;
        }

        RenderConfig candidate;
        if (!detail::deserialize_setup_config(contents, candidate, error)) {
            return false;
        }

        // All potentially failing parsing, allocation, and validation occurs
        // above. Standard-allocator vector/string moves make this commit step
        // non-throwing in the supported C++17 implementations.
        destination = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to load setup; destination was not changed.");
    } catch (const std::filesystem::filesystem_error& exception) {
        return fail(error, std::string("Filesystem error while loading setup; destination was not changed: ")
                           + exception.what());
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected error while loading setup; destination was not changed: ")
                           + exception.what());
    }
}

const char* clock_mode_name(ClockMode value) {
    switch (value) {
        case ClockMode::Default: return "Default";
        case ClockMode::Frame: return "Frame";
        case ClockMode::Time: return "Time";
        case ClockMode::Meter: return "Time signature";
        case ClockMode::Music: return "Music";
    }
    return "Unknown";
}

const char* clock_interpolation_name(ClockInterpolation value) {
    switch (value) {
        case ClockInterpolation::Hold: return "Hold";
        case ClockInterpolation::Linear: return "Linear";
        case ClockInterpolation::Smoothstep: return "Smoothstep";
    }
    return "Unknown";
}

const char* clock_fit_name(ClockFit value) {
    switch (value) {
        case ClockFit::Exact: return "Exact interval";
        case ClockFit::FitSequence: return "Fit sequence";
    }
    return "Unknown";
}

const char* music_tempo_mode_name(MusicTempoMode value) {
    switch (value) {
        case MusicTempoMode::Half: return "Half tempo";
        case MusicTempoMode::Detected: return "Detected tempo";
        case MusicTempoMode::Double: return "Double tempo";
    }
    return "Unknown";
}

const char* music_feature_name(MusicFeature value) {
    switch (value) {
        case MusicFeature::Energy: return "Energy";
        case MusicFeature::Bass: return "Bass";
        case MusicFeature::Midrange: return "Midrange";
        case MusicFeature::Treble: return "Treble";
        case MusicFeature::Onset: return "Onset";
        case MusicFeature::Beat: return "Beat";
        case MusicFeature::SpectralCentroid: return "Spectral brightness";
        case MusicFeature::SpectralFlatness: return "Spectral noisiness";
        case MusicFeature::ChromaHue: return "Pitch color (tonality-weighted)";
        case MusicFeature::ChromaStrength: return "Tonal strength";
    }
    return "Unknown";
}

const char* audio_response_mode_name(AudioResponseMode value) {
    switch (value) {
        case AudioResponseMode::Default: return "Default";
        case AudioResponseMode::Enabled:
            return "Profile source (force this item on)";
        case AudioResponseMode::Disabled: return "Ignore audio";
        case AudioResponseMode::Energy: return "Energy";
        case AudioResponseMode::Bass: return "Bass";
        case AudioResponseMode::Midrange: return "Midrange";
        case AudioResponseMode::Treble: return "Treble";
        case AudioResponseMode::Onset: return "Onset";
        case AudioResponseMode::Beat: return "Beat";
        case AudioResponseMode::SpectralCentroid: return "Spectral brightness";
        case AudioResponseMode::SpectralFlatness: return "Spectral noisiness";
        case AudioResponseMode::ChromaHue:
            return "Pitch color (tonality-weighted)";
        case AudioResponseMode::ChromaStrength: return "Tonal strength";
    }
    return "Unknown";
}

const char* music_swing_policy_name(MusicSwingPolicy value) {
    switch (value) {
        case MusicSwingPolicy::SuppressAll: return "Disable all swings";
        case MusicSwingPolicy::SuppressGlobal: return "Disable global swings";
        case MusicSwingPolicy::KeepAll: return "Keep all swings";
    }
    return "Unknown";
}

} // namespace pvt
