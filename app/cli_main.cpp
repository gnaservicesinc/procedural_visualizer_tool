#include "procedural_visualizer_tool.h"
#include "audio_analysis.h"
#include "project_bundle.h"
#include "source_image.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

namespace {

#ifndef PVT_PROGRAM_VERSION
#  define PVT_PROGRAM_VERSION "development"
#endif

using pvt::EffectType;
using pvt::ProjectDocument;
using pvt::RenderConfig;

constexpr std::size_t kMaximumNameBytes = 256;
constexpr std::size_t kMaximumPathBytes = 4095;
constexpr std::size_t kMaximumPrefixBytes = 127;

// The interactive editor uses this to distinguish visiting an editor from
// actually changing a value. That preserves the bundle invariant that a
// no-change Save validates the project without manufacturing a new version.
bool g_prompt_changed = false;

struct CliState {
    ProjectDocument document = pvt::default_project_document();
    std::size_t active_layer = 0;
};

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool read_line(const std::string& prompt, std::string& value) {
    std::cout << prompt << std::flush;
    if (!std::getline(std::cin, value)) {
        return false;
    }
    value = trim(std::move(value));
    return true;
}

bool parse_integer(const std::string& text, long long minimum, long long maximum,
                   long long& value) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' || parsed < minimum
        || parsed > maximum) {
        return false;
    }
    value = parsed;
    return true;
}

bool parse_real(const std::string& text, double minimum, double maximum, double& value) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text.c_str(), &end);
    if (errno != 0 || end == text.c_str() || *end != '\0' || !std::isfinite(parsed)
        || parsed < minimum || parsed > maximum) {
        return false;
    }
    value = parsed;
    return true;
}

bool valid_utf8_without_controls(const std::string& value, bool allow_tab) {
    for (std::size_t index = 0U; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::uint32_t code_point = 0U;
        std::size_t length = 0U;
        if (first <= 0x7fU) {
            code_point = first;
            length = 1U;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            code_point = first & 0x1fU;
            length = 2U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            code_point = first & 0x0fU;
            length = 3U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            code_point = first & 0x07U;
            length = 4U;
        } else {
            return false;
        }
        if (index + length > value.size()) {
            return false;
        }
        for (std::size_t continuation_index = 1U;
             continuation_index < length; ++continuation_index) {
            const auto continuation =
                static_cast<unsigned char>(value[index + continuation_index]);
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3fU);
        }
        if ((length == 3U && code_point < 0x800U)
            || (length == 4U && code_point < 0x10000U)
            || code_point > 0x10ffffU
            || (code_point >= 0xd800U && code_point <= 0xdfffU)
            || (code_point < 0x20U
                && !(allow_tab && code_point == static_cast<std::uint32_t>('\t')))
            || (code_point >= 0x7fU && code_point <= 0x9fU)) {
            return false;
        }
        index += length;
    }
    return true;
}

bool valid_project_name_text(const std::string& value) {
    return !value.empty() && value.size() <= kMaximumNameBytes
           && valid_utf8_without_controls(value, false)
           && value.find('/') == std::string::npos
           && value.find('\\') == std::string::npos;
}

bool valid_layer_name_text(const std::string& value) {
    return value.size() <= kMaximumNameBytes
           && valid_utf8_without_controls(value, true);
}

bool prompt_int(const std::string& label, int& value, int minimum, int maximum) {
    for (;;) {
        std::string input;
        if (!read_line(label + " [" + std::to_string(value) + "]: ", input)) {
            return false;
        }
        if (input.empty()) {
            return true;
        }
        long long parsed = 0;
        if (parse_integer(input, minimum, maximum, parsed)) {
            const int next = static_cast<int>(parsed);
            g_prompt_changed = g_prompt_changed || value != next;
            value = next;
            return true;
        }
        std::cout << "Enter a whole number from " << minimum << " to " << maximum
                  << ", or press Enter to keep the current value.\n";
    }
}

bool prompt_real(const std::string& label, double& value, double minimum,
                 double maximum) {
    for (;;) {
        std::ostringstream prompt;
        prompt << label << " [" << std::setprecision(8) << value << "]: ";
        std::string input;
        if (!read_line(prompt.str(), input)) {
            return false;
        }
        if (input.empty()) {
            return true;
        }
        double parsed = 0.0;
        if (parse_real(input, minimum, maximum, parsed)) {
            g_prompt_changed = g_prompt_changed || value != parsed;
            value = parsed;
            return true;
        }
        std::cout << "Enter a finite number from " << minimum << " to " << maximum
                  << ", or press Enter to keep it.\n";
    }
}

bool prompt_bool(const std::string& label, bool& value) {
    for (;;) {
        std::string input;
        if (!read_line(label + " [" + (value ? "yes" : "no") + "]: ", input)) {
            return false;
        }
        if (input.empty()) {
            return true;
        }
        std::transform(input.begin(), input.end(), input.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (input == "y" || input == "yes" || input == "true" || input == "1") {
            g_prompt_changed = g_prompt_changed || !value;
            value = true;
            return true;
        }
        if (input == "n" || input == "no" || input == "false" || input == "0") {
            g_prompt_changed = g_prompt_changed || value;
            value = false;
            return true;
        }
        std::cout << "Enter yes or no, or press Enter to keep it.\n";
    }
}

bool prompt_text(const std::string& label, std::string& value, std::size_t maximum) {
    for (;;) {
        std::string input;
        if (!read_line(label + " [" + value + "]: ", input)) {
            return false;
        }
        if (input.empty()) {
            return true;
        }
        if (input.size() <= maximum
            && std::none_of(input.begin(), input.end(), [](unsigned char c) {
                   return c < 0x20U || c == 0x7fU;
               })) {
            g_prompt_changed = g_prompt_changed || value != input;
            value = std::move(input);
            return true;
        }
        std::cout << "Text must contain at most " << maximum
                  << " bytes of printable text.\n";
    }
}

bool prompt_project_name(std::string& value) {
    for (;;) {
        std::string input;
        if (!read_line("Project name [" + value + "]: ", input)) {
            return false;
        }
        if (input.empty()) {
            return true;
        }
        if (valid_project_name_text(input)) {
            g_prompt_changed = g_prompt_changed || value != input;
            value = std::move(input);
            return true;
        }
        std::cout << "Use 1 to " << kMaximumNameBytes
                  << " bytes of well-formed UTF-8 without controls or path "
                     "separators (/ and backslash).\n";
    }
}

bool prompt_layer_name(std::string& value) {
    for (;;) {
        std::string input;
        if (!read_line("Layer name [" + value + "]: ", input)) {
            return false;
        }
        if (input.empty()) {
            return true;
        }
        if (valid_layer_name_text(input)) {
            g_prompt_changed = g_prompt_changed || value != input;
            value = std::move(input);
            return true;
        }
        std::cout << "Use at most " << kMaximumNameBytes
                  << " bytes of well-formed UTF-8 without controls other than tab.\n";
    }
}

bool valid_filename_prefix(const std::string& value) {
    if (value.empty() || value.size() > kMaximumPrefixBytes) {
        return false;
    }
    return std::none_of(value.begin(), value.end(), [](unsigned char character) {
        if (character < 0x20U || character == 0x7fU) {
            return true;
        }
        switch (character) {
            case '<':
            case '>':
            case ':':
            case '"':
            case '/':
            case '\\':
            case '|':
            case '?':
            case '*':
                return true;
            default:
                return false;
        }
    });
}

bool valid_output_directory(const std::string& value) {
    return !value.empty() && value.size() <= kMaximumPathBytes
           && std::none_of(value.begin(), value.end(), [](unsigned char character) {
                  return character < 0x20U || character == 0x7fU;
              });
}

bool prompt_filename_prefix(std::string& value) {
    for (;;) {
        std::string input;
        if (!read_line("Filename prefix [" + value + "]: ", input)) {
            return false;
        }
        if (input.empty()) {
            return true;
        }
        if (valid_filename_prefix(input)) {
            g_prompt_changed = g_prompt_changed || value != input;
            value = std::move(input);
            return true;
        }
        std::cout << "Use 1 to " << kMaximumPrefixBytes
                  << " bytes of printable text without <, >, :, \", /, \\, |, ?, or *.\n";
    }
}

template <typename Enum>
bool prompt_enum(const std::string& heading, Enum& value,
                 const std::vector<std::pair<Enum, std::string>>& choices) {
    std::cout << heading << '\n';
    std::size_t current = 0;
    for (std::size_t i = 0; i < choices.size(); ++i) {
        std::cout << "  " << (i + 1) << ") " << choices[i].second << '\n';
        if (choices[i].first == value) {
            current = i;
        }
    }
    for (;;) {
        std::string input;
        if (!read_line("Choice [" + std::to_string(current + 1) + "]: ", input)) {
            return false;
        }
        if (input.empty()) {
            return true;
        }
        long long parsed = 0;
        if (parse_integer(input, 1, static_cast<long long>(choices.size()), parsed)) {
            const Enum next = choices[static_cast<std::size_t>(parsed - 1)].first;
            g_prompt_changed = g_prompt_changed || value != next;
            value = next;
            return true;
        }
        std::cout << "Choose one of the listed numbers.\n";
    }
}

std::vector<std::pair<pvt::MusicFeature, std::string>> music_feature_choices() {
    std::vector<std::pair<pvt::MusicFeature, std::string>> choices;
    for (int raw = 0;
         raw <= static_cast<int>((std::numeric_limits<std::uint8_t>::max)());
         ++raw) {
        const auto feature = static_cast<pvt::MusicFeature>(raw);
        const char* const name = pvt::music_feature_name(feature);
        if (name == nullptr || std::string(name) == "Unknown") continue;
        choices.emplace_back(feature, name);
    }
    return choices;
}

std::vector<std::pair<pvt::AudioResponseMode, std::string>>
audio_response_choices() {
    return {
        {pvt::AudioResponseMode::Default,
         "Default (use effective profile)"},
        {pvt::AudioResponseMode::Beat, "Beat"},
        {pvt::AudioResponseMode::Onset, "Onset"},
        {pvt::AudioResponseMode::Energy, "Energy"},
        {pvt::AudioResponseMode::Bass, "Bass"},
        {pvt::AudioResponseMode::Midrange, "Midrange"},
        {pvt::AudioResponseMode::Treble, "Treble"},
        {pvt::AudioResponseMode::SpectralCentroid, "Spectral brightness"},
        {pvt::AudioResponseMode::SpectralFlatness, "Spectral noisiness"},
        {pvt::AudioResponseMode::ChromaHue,
         "Pitch color (tonality-weighted)"},
        {pvt::AudioResponseMode::ChromaStrength, "Tonal strength"},
        {pvt::AudioResponseMode::Enabled,
         "Profile source (force this item on)"},
        {pvt::AudioResponseMode::Disabled, "Ignore audio"},
    };
}

bool palettes_equal(const pvt::PaletteConfig& left,
                    const pvt::PaletteConfig& right) {
    if (left.enabled != right.enabled || left.name != right.name
        || left.colors.size() != right.colors.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.colors.size(); ++index) {
        if (left.colors[index].red != right.colors[index].red
            || left.colors[index].green != right.colors[index].green
            || left.colors[index].blue != right.colors[index].blue) {
            return false;
        }
    }
    return true;
}

bool configure_palette(RenderConfig& config) {
    for (;;) {
        const auto& palette = config.palette;
        std::cout << "\n-- Layer starting palette: " << palette.name << " ("
                  << (palette.enabled ? "used" : "not used") << ") --\n";
        for (std::size_t index = 0U; index < palette.colors.size(); ++index) {
            const auto& color = palette.colors[index];
            std::cout << "  " << (index + 1U) << ") RGB " << color.red << ", "
                      << color.green << ", " << color.blue << '\n';
        }
        if (palette.colors.empty()) {
            std::cout << "  (no custom colors)\n";
        }
        std::cout << "Presets:";
        for (std::size_t index = 0U; index < pvt::kBuiltInPaletteCount; ++index) {
            std::cout << " p" << (index + 1U) << ' '
                      << pvt::default_palette(index).name;
            if (index + 1U != pvt::kBuiltInPaletteCount) {
                std::cout << " |";
            }
        }
        std::cout << "\nCommands: e toggle starting palette, p N preset, n rename, number edit, "
                     "a add, d N delete, b back.\n";

        std::string input;
        if (!read_line("Palette action [b]: ", input)) {
            return false;
        }
        if (input.empty() || input == "b" || input == "B") {
            return true;
        }
        if (input == "e" || input == "E") {
            if (config.palette.colors.empty()) {
                const pvt::PaletteConfig next = pvt::default_palette(0U);
                g_prompt_changed = g_prompt_changed
                                   || !palettes_equal(config.palette, next);
                config.palette = next;
                std::cout << "Loaded and enabled the Ember preset because a starting "
                             "palette needs at least one color.\n";
            } else {
                config.palette.enabled = !config.palette.enabled;
                g_prompt_changed = true;
            }
            continue;
        }
        if ((input[0] == 'p' || input[0] == 'P') && input.size() > 1U) {
            long long selected = 0;
            if (!parse_integer(trim(input.substr(1)), 1,
                               static_cast<long long>(pvt::kBuiltInPaletteCount),
                               selected)) {
                std::cout << "Use p followed by a preset number from 1 through "
                          << pvt::kBuiltInPaletteCount << ".\n";
                continue;
            }
            pvt::PaletteConfig next = pvt::default_palette(
                static_cast<std::size_t>(selected - 1));
            next.enabled = config.palette.enabled;
            g_prompt_changed = g_prompt_changed
                               || !palettes_equal(config.palette, next);
            config.palette = next;
            continue;
        }
        if (input == "n" || input == "N") {
            if (!prompt_text("Palette name", config.palette.name,
                             kMaximumNameBytes)) {
                return false;
            }
            continue;
        }
        if (input == "a" || input == "A") {
            if (config.palette.colors.size() >= pvt::kMaximumPaletteColors) {
                std::cout << "The safety limit of " << pvt::kMaximumPaletteColors
                          << " palette colors has been reached.\n";
                continue;
            }
            const pvt::PaletteColor next = config.palette.colors.empty()
                                               ? pvt::PaletteColor{1.0, 1.0, 1.0}
                                               : config.palette.colors.back();
            config.palette.colors.push_back(next);
            g_prompt_changed = true;
            const std::size_t index = config.palette.colors.size() - 1U;
            if (!prompt_real("Red (sRGB)", config.palette.colors[index].red,
                             0.0, 1.0)
                || !prompt_real("Green (sRGB)", config.palette.colors[index].green,
                                0.0, 1.0)
                || !prompt_real("Blue (sRGB)", config.palette.colors[index].blue,
                                0.0, 1.0)) {
                return false;
            }
            continue;
        }
        if ((input[0] == 'd' || input[0] == 'D') && input.size() > 1U) {
            long long selected = 0;
            if (!parse_integer(trim(input.substr(1)), 1,
                               static_cast<long long>(config.palette.colors.size()),
                               selected)) {
                std::cout << "Use d followed by an existing color number.\n";
                continue;
            }
            if (config.palette.enabled && config.palette.colors.size() == 1U) {
                std::cout << "A starting palette needs at least one color. Disable it "
                             "or add a replacement first.\n";
                continue;
            }
            config.palette.colors.erase(
                config.palette.colors.begin() + (selected - 1));
            g_prompt_changed = true;
            continue;
        }

        long long selected = 0;
        if (!parse_integer(input, 1,
                           static_cast<long long>(config.palette.colors.size()),
                           selected)) {
            std::cout << "Unrecognized palette action.\n";
            continue;
        }
        auto& color = config.palette.colors[
            static_cast<std::size_t>(selected - 1)];
        if (!prompt_real("Red (sRGB)", color.red, 0.0, 1.0)
            || !prompt_real("Green (sRGB)", color.green, 0.0, 1.0)
            || !prompt_real("Blue (sRGB)", color.blue, 0.0, 1.0)) {
            return false;
        }
    }
}

bool configure_wave(RenderConfig& config, std::size_t index) {
    auto& wave = config.waves[index];
    std::cout << "\n-- Wave " << (index + 1) << " --\n"
              << "Synchronized waves use the shared swung master clock. Unsynchronized\n"
              << "waves use a linear but still seamless per-loop clock.\n";
    if (!prompt_text("Name", wave.name, kMaximumNameBytes)
        || !prompt_bool("Enabled", wave.enabled)
        || !prompt_bool("Use synchronized clock", wave.synchronized)) {
        return false;
    }
    if (wave.synchronized
        && !prompt_enum(
            "Audio response source (Default inherits the effective profile)",
            wave.audio_response, audio_response_choices())) {
        return false;
    }
    return prompt_real("Horizontal source location (%)", wave.x_percent, -100.0, 200.0)
           && prompt_real("Vertical source location (%)", wave.y_percent, -100.0, 200.0)
           && prompt_real("Amplitude", wave.amplitude, 0.0, 10.0)
           && prompt_real("Spatial frequency", wave.spatial_frequency, 0.0, 1000.0)
           && prompt_int("Motion cycles per loop", wave.cycles_per_loop, -1000, 1000)
           && prompt_real("Starting phase (degrees)", wave.phase_degrees, -36000.0, 36000.0)
           && prompt_real("Propagation direction (0 horizontal, .5 radial, 1 vertical)",
                          wave.direction, 0.0, 1.0);
}

bool parse_move_command(const std::string& input, std::size_t count,
                        std::size_t& from, std::size_t& to) {
    std::istringstream stream(input);
    char command = '\0';
    long long source = 0;
    long long target = 0;
    std::string extra;
    if (!(stream >> command >> source >> target) || (stream >> extra)
        || (command != 'm' && command != 'M') || source < 1 || target < 1
        || static_cast<std::size_t>(source) > count
        || static_cast<std::size_t>(target) > count) {
        return false;
    }
    from = static_cast<std::size_t>(source - 1);
    to = static_cast<std::size_t>(target - 1);
    return true;
}

void configure_waves(RenderConfig& config) {
    for (;;) {
        std::cout << "\n-- Waves (" << config.waves.size() << ") --\n";
        for (std::size_t i = 0; i < config.waves.size(); ++i) {
            const auto& wave = config.waves[i];
            std::cout << "  " << (i + 1) << ") " << (wave.enabled ? "on  " : "off ")
                      << (wave.synchronized ? "sync " : "free ") << wave.name
                      << " | amp " << wave.amplitude << " | dir " << wave.direction;
            if (wave.synchronized) {
                std::cout << " | audio "
                          << pvt::audio_response_mode_name(
                                 wave.audio_response);
            }
            std::cout << '\n';
        }
        std::cout << "Enter a number to edit, a to add, d N to delete, m FROM TO to move,"
                     " or b to go back.\n";
        std::string input;
        if (!read_line("Wave action [b]: ", input) || input.empty() || input == "b"
            || input == "B") {
            return;
        }
        if (input == "a" || input == "A") {
            if (config.waves.size() >= pvt::kMaximumWaves) {
                std::cout << "The safety limit of " << pvt::kMaximumWaves
                          << " waves has been reached.\n";
                continue;
            }
            auto wave = pvt::default_wave(config.waves.size());
            wave.id = pvt::allocate_id(config);
            config.waves.push_back(std::move(wave));
            g_prompt_changed = true;
            configure_wave(config, config.waves.size() - 1);
            continue;
        }
        if ((input[0] == 'd' || input[0] == 'D') && input.size() > 1) {
            long long selected = 0;
            if (parse_integer(trim(input.substr(1)), 1,
                              static_cast<long long>(config.waves.size()), selected)) {
                config.waves.erase(config.waves.begin() + (selected - 1));
                g_prompt_changed = true;
            } else {
                std::cout << "Use d followed by an existing wave number.\n";
            }
            continue;
        }
        std::size_t from = 0;
        std::size_t to = 0;
        if (parse_move_command(input, config.waves.size(), from, to)) {
            auto item = std::move(config.waves[from]);
            config.waves.erase(config.waves.begin() + static_cast<std::ptrdiff_t>(from));
            config.waves.insert(config.waves.begin() + static_cast<std::ptrdiff_t>(to),
                                std::move(item));
            g_prompt_changed = true;
            continue;
        }
        long long selected = 0;
        if (parse_integer(input, 1, static_cast<long long>(config.waves.size()), selected)) {
            configure_wave(config, static_cast<std::size_t>(selected - 1));
        } else {
            std::cout << "Unrecognized wave action.\n";
        }
    }
}

EffectType choose_effect_type() {
    EffectType type = EffectType::Ripple;
    prompt_enum("Effect type", type,
                {{EffectType::EndlessZoom, "Endless zoom"},
                 {EffectType::Ripple, "Ripple"},
                 {EffectType::Shake, "Shake"},
                 {EffectType::FlagWave, "Flag wave"},
                 {EffectType::Glow, "Glow"},
                 {EffectType::BlockScale, "Block scale"},
                 {EffectType::ParticleField, "Particle field"}});
    return type;
}

bool configure_edge_mode(pvt::EdgeMode& mode) {
    return prompt_enum("Out-of-frame fill", mode,
                       {{pvt::EdgeMode::Alpha, "Transparent alpha"},
                        {pvt::EdgeMode::Black, "Black"},
                        {pvt::EdgeMode::White, "White"},
                        {pvt::EdgeMode::Reflect, "Reflected pattern"}});
}

bool configure_effect(RenderConfig& config, std::size_t index) {
    auto& effect = config.effects[index];
    std::cout << "\n-- " << pvt::effect_type_name(effect.type) << " effect --\n";
    if (!prompt_text("Name", effect.name, kMaximumNameBytes)
        || !prompt_bool("Enabled", effect.enabled)
        || !prompt_bool("Use synchronized clock", effect.synchronized)) {
        return false;
    }
    if (effect.synchronized
        && !prompt_enum(
            "Audio response source (Default inherits the effective profile)",
            effect.audio_response, audio_response_choices())) {
        return false;
    }
    if (!prompt_enum("Effect space (texture is before surface mapping)",
                     effect.space,
                     {{pvt::EffectSpace::Texture, "Texture/artwork"},
                      {pvt::EffectSpace::Surface, "Mapped object/silhouette"}})
        || !prompt_int("Cycles per loop", effect.cycles_per_loop, -1000, 1000)
        || !prompt_real("Starting phase (degrees)", effect.phase_degrees, -36000.0, 36000.0)) {
        return false;
    }
    if (effect.type != EffectType::BlockScale
        && (!prompt_real("Center X (0-1 is on-canvas)", effect.center_x,
                         -10.0, 10.0)
            || !prompt_real("Center Y (0-1 is on-canvas)", effect.center_y,
                            -10.0, 10.0)
            || !prompt_real("Local area radius (0 whole layer; fraction of short edge)",
                            effect.area_radius, 0.0, 10.0))) {
        return false;
    }
    switch (effect.type) {
        case EffectType::EndlessZoom:
            return prompt_real("Mix/intensity", effect.intensity, 0.0, 100.0)
                   && prompt_real("Zoom strength", effect.magnitude, 0.0, 10.0)
                   && prompt_real("Zoom octave multiplier", effect.frequency, 0.0, 1000.0)
                   && configure_edge_mode(effect.edge_mode);
        case EffectType::Ripple:
            return prompt_real("Mix/intensity", effect.intensity, 0.0, 100.0)
                   && prompt_real("Magnitude (fraction of short edge)", effect.magnitude, 0.0, 10.0)
                   && prompt_real("Spatial frequency", effect.frequency, 0.0, 1000.0)
                   && prompt_real("Distance attenuation", effect.secondary, -100.0, 100.0)
                   && configure_edge_mode(effect.edge_mode);
        case EffectType::Shake:
            return prompt_real("Mix/intensity", effect.intensity, 0.0, 100.0)
                   && prompt_real("Magnitude (fraction of short edge)", effect.magnitude, 0.0, 10.0)
                   && prompt_real("Shake harmonic", effect.frequency, 0.0, 1000.0)
                   && prompt_real("Cross-axis harmonic mix", effect.secondary, -100.0, 100.0)
                   && prompt_real("Direction angle (degrees)", effect.angle_degrees, -36000.0, 36000.0)
                   && configure_edge_mode(effect.edge_mode);
        case EffectType::FlagWave:
            return prompt_real("Mix/intensity", effect.intensity, 0.0, 100.0)
                   && prompt_real("Magnitude (fraction of short edge)", effect.magnitude, 0.0, 10.0)
                   && prompt_real("Spatial frequency", effect.frequency, 0.0, 1000.0)
                   && prompt_real("Secondary harmonic mix", effect.secondary, -100.0, 100.0)
                   && prompt_real("Wave angle (degrees)", effect.angle_degrees, -36000.0, 36000.0)
                   && configure_edge_mode(effect.edge_mode);
        case EffectType::Glow:
            return prompt_real("Bloom/glow strength", effect.intensity, 0.0, 100.0)
                   && prompt_real("Pulse depth", effect.secondary, -100.0, 100.0)
                   && prompt_real("Radius (pixels)", effect.radius_pixels, 0.0, 16384.0)
                   && prompt_real("Linear-luminance threshold (lower affects more)",
                                  effect.threshold, 0.0, 64.0)
                   && prompt_real("Soft knee", effect.soft_knee, 0.0, 1.0);
        case EffectType::BlockScale: {
            if (!prompt_real("Mix", effect.intensity, 0.0, 1.0)
                || !prompt_real("Minimum block-size multiplier",
                                effect.magnitude, 0.001, 10.0)) {
                return false;
            }
            if (effect.frequency < effect.magnitude) {
                effect.frequency = effect.magnitude;
                g_prompt_changed = true;
            }
            if (!prompt_real("Maximum block-size multiplier",
                             effect.frequency, effect.magnitude, 1000.0)) {
                return false;
            }
            int steps = static_cast<int>(std::llround(effect.secondary));
            if (!prompt_int("Quantization steps (0 is smooth)", steps, 0, 100)) {
                return false;
            }
            effect.secondary = static_cast<double>(steps);
            return true;
        }
        case EffectType::ParticleField: {
            int particles = static_cast<int>(std::llround(effect.frequency));
            if (!prompt_real("Spark brightness", effect.intensity, 0.0, 100.0)
                || !prompt_real("Travel per loop (fraction of short edge)",
                                effect.magnitude, 0.0, 10.0)
                || !prompt_int("Particle count", particles, 1, 1000)
                || !prompt_real("Trail amount", effect.secondary, 0.0, 1.0)
                || !prompt_real("Travel angle (degrees)", effect.angle_degrees,
                                -36000.0, 36000.0)
                || !prompt_real("Particle radius (pixels)", effect.radius_pixels,
                                0.01, 16384.0)
                || !prompt_real("White-hot core", effect.threshold, 0.0, 1.0)
                || !prompt_real("Glow softness", effect.soft_knee, 0.0, 1.0)) {
                return false;
            }
            effect.frequency = static_cast<double>(particles);
            return true;
        }
    }
    return true;
}

void configure_effects(RenderConfig& config) {
    for (;;) {
        std::cout << "\n-- Ordered effect stack (" << config.effects.size() << ") --\n";
        for (std::size_t i = 0; i < config.effects.size(); ++i) {
            const auto& effect = config.effects[i];
            std::cout << "  " << (i + 1) << ") " << (effect.enabled ? "on  " : "off ")
                      << (effect.synchronized ? "sync " : "free ") << effect.name
                      << " [" << pvt::effect_type_name(effect.type) << ", "
                      << pvt::effect_space_name(effect.space) << ']';
            if (effect.synchronized) {
                std::cout << " | audio "
                          << pvt::audio_response_mode_name(
                                 effect.audio_response);
            }
            if (effect.type != EffectType::BlockScale) {
                std::cout << " | area " << effect.area_radius;
            }
            std::cout << '\n';
        }
        std::cout << "Enter a number to edit, a to add, d N to delete, m FROM TO to move,"
                     " or b to go back.\n";
        std::string input;
        if (!read_line("Effect action [b]: ", input) || input.empty() || input == "b"
            || input == "B") {
            return;
        }
        if (input == "a" || input == "A") {
            if (config.effects.size() >= pvt::kMaximumEffects) {
                std::cout << "The safety limit of " << pvt::kMaximumEffects
                          << " effects has been reached.\n";
                continue;
            }
            auto effect = pvt::default_effect(choose_effect_type());
            effect.id = pvt::allocate_id(config);
            config.effects.push_back(std::move(effect));
            g_prompt_changed = true;
            configure_effect(config, config.effects.size() - 1);
            continue;
        }
        if ((input[0] == 'd' || input[0] == 'D') && input.size() > 1) {
            long long selected = 0;
            if (parse_integer(trim(input.substr(1)), 1,
                              static_cast<long long>(config.effects.size()), selected)) {
                config.effects.erase(config.effects.begin() + (selected - 1));
                g_prompt_changed = true;
            } else {
                std::cout << "Use d followed by an existing effect number.\n";
            }
            continue;
        }
        std::size_t from = 0;
        std::size_t to = 0;
        if (parse_move_command(input, config.effects.size(), from, to)) {
            auto item = std::move(config.effects[from]);
            config.effects.erase(config.effects.begin() + static_cast<std::ptrdiff_t>(from));
            config.effects.insert(config.effects.begin() + static_cast<std::ptrdiff_t>(to),
                                  std::move(item));
            g_prompt_changed = true;
            continue;
        }
        long long selected = 0;
        if (parse_integer(input, 1, static_cast<long long>(config.effects.size()), selected)) {
            configure_effect(config, static_cast<std::size_t>(selected - 1));
        } else {
            std::cout << "Unrecognized effect action.\n";
        }
    }
}

bool configure_swing(RenderConfig& config, std::size_t index) {
    auto& swing = config.swings[index];
    return prompt_text("Name", swing.name, kMaximumNameBytes)
           && prompt_bool("Enabled", swing.enabled)
           && prompt_enum("Waveform", swing.waveform,
                          {{pvt::Waveform::Sine, "Sine"},
                           {pvt::Waveform::Triangle, "Triangle"},
                           {pvt::Waveform::SmoothPulse, "Smooth pulse"},
                           {pvt::Waveform::Bounce, "Bounce"}})
           && prompt_real("Swing amount", swing.amount, -2.0, 2.0)
           && prompt_int("Pulses per loop", swing.cycles_per_loop, 0, 1000)
           && prompt_real("Starting phase (degrees)", swing.phase_degrees, -36000.0, 36000.0)
           && prompt_real("Waveform shape", swing.shape, 0.0, 1.0)
           && prompt_real("Center X (0-1 is on-canvas)", swing.center_x,
                          -10.0, 10.0)
           && prompt_real("Center Y (0-1 is on-canvas)", swing.center_y,
                          -10.0, 10.0)
           && prompt_real("Local radius (0 whole layer; fraction of short edge)",
                          swing.radius, 0.0, 10.0);
}

bool prompt_meter_expression(std::string& expression) {
    for (;;) {
        std::string input;
        if (!read_line("Meter (examples: 7/8, 3+2+3/8, 5/4 | 6/4) ["
                           + expression + "]: ",
                       input)) {
            return false;
        }
        if (input.empty()) {
            return true;
        }
        std::string description;
        std::string error;
        if (input.size() <= 256U
            && pvt::describe_meter(input, description, &error)) {
            g_prompt_changed = g_prompt_changed || expression != input;
            expression = std::move(input);
            std::cout << "Meter: " << description << '\n';
            return true;
        }
        std::cout << "Invalid meter: " << error << '\n';
    }
}

bool analyze_music_interactive(pvt::ClockConfig& clock,
                               pvt::AudioReactiveConfig& response,
                               ProjectDocument& document,
                               const std::string& attachment_reference) {
    std::cout << "Cached music: "
              << (clock.music.source_sha256.empty()
                      ? "none"
                      : clock.music.source_basename + " | "
                            + std::to_string(clock.music.duration_seconds)
                            + " s | " + std::to_string(clock.music.detected_bpm)
                            + " BPM")
              << '\n';
    std::string path;
    if (!read_line("Music file to analyze (Enter keeps cached analysis, c clears): ", path)) {
        return false;
    }
    if (path.empty()) {
        return true;
    }
    if (path == "c" || path == "C") {
        if (!clock.music.source_sha256.empty()) {
            ProjectDocument candidate = document;
            std::string detach_error;
            if (!pvt::detach_project_file(
                    candidate, attachment_reference, &detach_error)) {
                std::cout << "Could not clear the embedded music source: "
                          << detach_error << '\n';
                return true;
            }
            document = std::move(candidate);
            clock.music = {};
            if (clock.mode == pvt::ClockMode::Music) {
                clock.mode = pvt::ClockMode::Default;
            }
            g_prompt_changed = true;
        }
        return true;
    }
    if (!valid_output_directory(path)) {
        std::cout << "The music path is empty, too long, or contains control characters.\n";
        return true;
    }

    pvt::MusicAnalysis analysis;
    std::string error;
    unsigned last_percent = 101U;
    std::cout << "Analyzing music…" << std::flush;
    const bool ok = pvt::audio::analyze_music_file(
        path, analysis,
        [&last_percent](std::uint64_t completed, std::uint64_t total) {
            const unsigned percent = total == 0U
                                         ? 0U
                                         : static_cast<unsigned>(
                                               std::min<long double>(
                                                   100.0L,
                                                   static_cast<long double>(completed)
                                                       * 100.0L
                                                       / static_cast<long double>(total)));
            if (percent != last_percent) {
                std::cout << '\r' << "Analyzing music… " << percent << '%' << std::flush;
                last_percent = percent;
            }
            return true;
        },
        nullptr, &error);
    if (!ok) {
        std::cout << "\rMusic analysis failed; cached analysis is unchanged: "
                  << error << "\n";
        return true;
    }
    ProjectDocument candidate = document;
    pvt::ProjectAttachment attached;
    if (!pvt::attach_project_file(candidate, attachment_reference,
                                  path, &attached, &error)) {
        std::cout << "\rMusic was analyzed, but its source could not be embedded; "
                     "the previous music remains unchanged: "
                  << error << "\n";
        return true;
    }
    if (attached.sha256 != analysis.source_sha256) {
        std::cout << "\rThe music file changed while it was being analyzed. "
                     "Please select it again; the previous music remains unchanged.\n";
        return true;
    }
    std::cout << "\rAnalyzed " << analysis.source_basename << ": "
              << analysis.duration_seconds << " s, " << analysis.detected_bpm
              << " BPM, " << analysis.beat_times_seconds.size() << " beat(s).\n";
    const bool first_music_source = clock.music.source_sha256.empty();
    document = std::move(candidate);
    clock.music = std::move(analysis);
    clock.mode = pvt::ClockMode::Music;
    clock.music_swing_policy = pvt::MusicSwingPolicy::KeepAll;
    if (first_music_source) response.enabled = true;
    g_prompt_changed = true;
    return true;
}

bool configure_audio_response_profile(
    const std::string& title,
    pvt::AudioReactiveConfig& response) {
    std::cout << "\n-- " << title << " --\n";
    if (!prompt_bool("Audio response enabled", response.enabled)) {
        return false;
    }
    return !response.enabled
           || (prompt_bool("Limit response to synchronized waves/effects",
                           response.synchronized_only)
               && prompt_bool("Drive wave values by default",
                              response.waves_enabled)
               && prompt_enum("Wave feature", response.wave_source,
                              music_feature_choices())
               && prompt_real("Wave response amount", response.wave_amount,
                              -1.0, 10.0)
               && prompt_bool("Drive effect values by default",
                              response.effects_enabled)
               && prompt_enum("Effect feature", response.effect_source,
                              music_feature_choices())
               && prompt_real("Effect response amount",
                              response.effect_amount, -1.0, 10.0)
               && prompt_bool("Shift hue from a music feature",
                              response.color_enabled)
               && prompt_enum("Hue feature", response.color_source,
                              music_feature_choices())
               && prompt_real("Maximum hue shift (degrees)",
                              response.color_amount_degrees,
                              -3600.0, 3600.0));
}

void configure_active_layer_clock(RenderConfig& config,
                                  ProjectDocument& document,
                                  const std::string& layer_uuid) {
    auto& local = config.layer_clock;
    std::cout << "\n-- Clock — active layer --\n"
              << "This optional clock stays on the project timeline while its pulses and "
                 "music data drive only this layer.\n";
    if (!prompt_bool("Override the project-wide clock", local.enabled)) return;
    if (!local.enabled) return;
    if (!prompt_enum("Duration mapping", local.scale,
                     {{pvt::LayerClockScale::SmartLoopFit,
                       "Smart loop fit (most loops, least stretch)"},
                      {pvt::LayerClockScale::StraightFit,
                       "Straight fit (one traversal)"},
                      {pvt::LayerClockScale::PlayOnce,
                       "Play once, then hold"},
                      {pvt::LayerClockScale::PlayOnceThenProject,
                       "Play once, then use project clock"},
                      {pvt::LayerClockScale::OriginalSpeedLoop,
                       "Original-speed loop (partial final loop allowed)"}})) return;
    auto& clock = local.clock;
    if (!prompt_enum("Layer clock source", clock.mode,
                     {{pvt::ClockMode::Default, "Default seamless loop"},
                      {pvt::ClockMode::Frame, "Pulse every N frames"},
                      {pvt::ClockMode::Time, "Pulse every N milliseconds"},
                      {pvt::ClockMode::Meter, "Tempo and meter"},
                      {pvt::ClockMode::Music, "Detected music beats"}})
        || !prompt_enum("Between calculated pulses", clock.interpolation,
                        {{pvt::ClockInterpolation::Hold, "Hold"},
                         {pvt::ClockInterpolation::Linear, "Linear"},
                         {pvt::ClockInterpolation::Smoothstep, "Smooth eased"}})
        || !prompt_bool("Reverse the layer clock", clock.reverse)
        || !prompt_real("Layer clock phase offset (degrees)",
                        clock.phase_offset_degrees, -36000.0, 36000.0)) return;
    if (clock.mode == pvt::ClockMode::Frame) {
        if (!prompt_int("Frames per pulse", clock.frame_interval, 1, 1000000)
            || !prompt_enum("Interval fit", clock.fit,
                            {{pvt::ClockFit::Exact, "Exact interval"},
                             {pvt::ClockFit::FitSequence, "Fit whole sequence"}})) return;
    } else if (clock.mode == pvt::ClockMode::Time) {
        double milliseconds =
            static_cast<double>(clock.time_interval_microseconds) / 1000.0;
        if (!prompt_real("Milliseconds per pulse", milliseconds, 0.001,
                         86400000.0)
            || !prompt_enum("Interval fit", clock.fit,
                            {{pvt::ClockFit::Exact, "Exact interval"},
                             {pvt::ClockFit::FitSequence, "Fit whole sequence"}})) return;
        clock.time_interval_microseconds =
            static_cast<std::int64_t>(std::llround(milliseconds * 1000.0));
    } else if (clock.mode == pvt::ClockMode::Meter) {
        if (!prompt_meter_expression(clock.meter.expression)
            || !prompt_real("Tempo (BPM)", clock.meter.bpm, 1.0, 1000.0)
            || !prompt_int("BPM note denominator",
                           clock.meter.tempo_note_denominator, 1, 1024)
            || !prompt_enum("Interval fit", clock.fit,
                            {{pvt::ClockFit::Exact, "Exact tempo"},
                             {pvt::ClockFit::FitSequence, "Fit whole measures"}})) return;
    } else if (clock.mode == pvt::ClockMode::Music) {
        const bool first_layer_music = clock.music.source_sha256.empty();
        if (!analyze_music_interactive(
                clock, config.audio_reactive, document,
                pvt::layer_music_attachment_id(layer_uuid))
            || !prompt_bool("Data only (mute this layer source in playback and movies)",
                            clock.data_only)
            || !prompt_enum("Beat interpretation", clock.music_tempo,
                            {{pvt::MusicTempoMode::Half, "Half-time"},
                             {pvt::MusicTempoMode::Detected, "Detected tempo"},
                             {pvt::MusicTempoMode::Double, "Double-time"}})) return;
        double offset_ms =
            static_cast<double>(clock.beat_offset_microseconds) / 1000.0;
        if (!prompt_real("Beat offset (milliseconds)", offset_ms,
                         -86400000.0, 86400000.0)) return;
        clock.beat_offset_microseconds =
            static_cast<std::int64_t>(std::llround(offset_ms * 1000.0));
        if (first_layer_music && !clock.music.source_sha256.empty()) {
            config.audio_reactive_override_enabled = true;
        }
    }
}

void configure_swings(RenderConfig& config) {
    if (!prompt_bool("Enable the authored swing block", config.swings_enabled)) {
        return;
    }
    for (;;) {
        std::cout << "\nSwing modulators (" << config.swings.size() << "):"
                  << (config.swings_enabled ? "\n" : " disabled as a block\n");
        for (std::size_t i = 0; i < config.swings.size(); ++i) {
            const auto& swing = config.swings[i];
            std::cout << "  " << (i + 1) << ") " << (swing.enabled ? "on  " : "off ")
                      << swing.name << " | " << pvt::waveform_name(swing.waveform)
                      << " | amount " << swing.amount
                      << " | radius " << swing.radius << '\n';
        }
        std::string input;
        if (!read_line("Number to edit, a to add, d N to delete, "
                       "m FROM TO to move, or b [b]: ", input)
            || input.empty() || input == "b" || input == "B") {
            return;
        }
        if (input == "a" || input == "A") {
            if (config.swings.size() >= pvt::kMaximumSwings) {
                std::cout << "The safety limit of " << pvt::kMaximumSwings
                          << " swing modulators has been reached.\n";
                continue;
            }
            auto swing = pvt::default_swing(config.swings.size());
            swing.id = pvt::allocate_id(config);
            config.swings.push_back(std::move(swing));
            g_prompt_changed = true;
            configure_swing(config, config.swings.size() - 1);
            continue;
        }
        if ((input[0] == 'd' || input[0] == 'D') && input.size() > 1) {
            long long selected = 0;
            if (parse_integer(trim(input.substr(1)), 1,
                              static_cast<long long>(config.swings.size()), selected)) {
                config.swings.erase(config.swings.begin() + (selected - 1));
                g_prompt_changed = true;
            } else {
                std::cout << "Use d followed by an existing swing number.\n";
            }
            continue;
        }
        std::size_t from = 0;
        std::size_t to = 0;
        if (parse_move_command(input, config.swings.size(), from, to)) {
            auto item = std::move(config.swings[from]);
            config.swings.erase(config.swings.begin() + static_cast<std::ptrdiff_t>(from));
            config.swings.insert(config.swings.begin() + static_cast<std::ptrdiff_t>(to),
                                 std::move(item));
            g_prompt_changed = true;
            continue;
        }
        long long selected = 0;
        if (parse_integer(input, 1, static_cast<long long>(config.swings.size()), selected)) {
            configure_swing(config, static_cast<std::size_t>(selected - 1));
        } else {
            std::cout << "Unrecognized swing action.\n";
        }
    }
}

void configure_color(RenderConfig& config) {
    std::cout << "\n-- Starting colors and post-effects quantization --\n";
    if (!prompt_real("Phrase warp amount", config.phrase_warp, 0.0, 2.0)
        || !prompt_real("Ghost mix", config.ghost_mix, 0.0, 1.0)
        || !prompt_real("Ghost lag (degrees)", config.ghost_lag_degrees, -360.0, 360.0)
        || !prompt_int("Hue rotations per loop", config.hue_cycles, -100, 100)
        || !prompt_real("Color saturation", config.saturation, 0.0, 1.0)
        || !prompt_bool("Post-effects quantization enabled", config.quantization.enabled)
        || !prompt_int("Post-effects quantization levels", config.quantization.levels, 2, 65536)
        || !prompt_real("Post-effects quantization mix", config.quantization.mix, 0.0, 1.0)
        || !prompt_enum("Post-effects quantization mode", config.quantization.mode,
                       {{pvt::QuantizationMode::Rgb, "RGB channels"},
                        {pvt::QuantizationMode::Luminance, "Luminance"},
                        {pvt::QuantizationMode::Hue, "Hue"}})) {
        return;
    }

    configure_palette(config);
}

void configure_synchronization(RenderConfig& config,
                               ProjectDocument& document,
                               const std::string& layer_uuid) {
    std::cout << "\n-- Synchronization --\n"
              << "The base clock is calculated first; authored swings are a reversible "
                 "layer on top.\n";
    if (!prompt_enum("Base clock", config.clock.mode,
                     {{pvt::ClockMode::Default, "Default seamless loop"},
                      {pvt::ClockMode::Frame, "Pulse every N frames"},
                      {pvt::ClockMode::Time, "Pulse every N milliseconds"},
                      {pvt::ClockMode::Meter, "Tempo and meter"},
                      {pvt::ClockMode::Music, "Detected music beats"}})
        || !prompt_enum("Between calculated pulses", config.clock.interpolation,
                        {{pvt::ClockInterpolation::Hold,
                          "Hold synchronized clock state until the next pulse"},
                         {pvt::ClockInterpolation::Linear, "Linear interpolation"},
                         {pvt::ClockInterpolation::Smoothstep,
                          "Smooth eased interpolation"}})
        || !prompt_bool("Reverse the base clock", config.clock.reverse)
        || !prompt_real("Clock phase offset (degrees)",
                        config.clock.phase_offset_degrees, -36000.0, 36000.0)) {
        return;
    }

    if (config.clock.mode == pvt::ClockMode::Frame) {
        if (!prompt_int("Frames per pulse", config.clock.frame_interval, 1, 1000000)
            || !prompt_enum("Interval fit", config.clock.fit,
                            {{pvt::ClockFit::Exact,
                              "Exact interval (partial final interval allowed)"},
                             {pvt::ClockFit::FitSequence,
                              "Fit whole pulse intervals to the sequence"}})) {
            return;
        }
    } else if (config.clock.mode == pvt::ClockMode::Time) {
        double milliseconds =
            static_cast<double>(config.clock.time_interval_microseconds) / 1000.0;
        const double before = milliseconds;
        if (!prompt_real("Milliseconds per pulse", milliseconds, 0.001, 86400000.0)
            || !prompt_enum("Interval fit", config.clock.fit,
                            {{pvt::ClockFit::Exact,
                              "Exact interval (partial final interval allowed)"},
                             {pvt::ClockFit::FitSequence,
                              "Fit whole pulse intervals to the sequence"}})) {
            return;
        }
        const auto next = static_cast<std::int64_t>(std::llround(milliseconds * 1000.0));
        if (before != milliseconds
            || config.clock.time_interval_microseconds != next) {
            config.clock.time_interval_microseconds = next;
            g_prompt_changed = true;
        }
    } else if (config.clock.mode == pvt::ClockMode::Meter) {
        if (!prompt_meter_expression(config.clock.meter.expression)
            || !prompt_real("Tempo (BPM)", config.clock.meter.bpm, 1.0, 1000.0)
            || !prompt_int("BPM note denominator (4 quarter, 8 eighth, etc.)",
                           config.clock.meter.tempo_note_denominator, 1, 1024)
            || !prompt_enum("Interval fit", config.clock.fit,
                            {{pvt::ClockFit::Exact,
                              "Exact tempo (partial final measure allowed)"},
                             {pvt::ClockFit::FitSequence,
                              "Fit whole measures to the sequence"}})) {
            return;
        }
    } else if (config.clock.mode == pvt::ClockMode::Music) {
        if (!analyze_music_interactive(config.clock,
                                       config.audio_reactive_defaults,
                                       document, pvt::kMusicSourceAttachmentId)
            || !prompt_enum("Beat interpretation", config.clock.music_tempo,
                            {{pvt::MusicTempoMode::Half, "Half-time"},
                             {pvt::MusicTempoMode::Detected, "Detected tempo"},
                             {pvt::MusicTempoMode::Double, "Double-time"}})) {
            return;
        }
        if (!prompt_bool("Data only (mute this source in playback and movies)",
                         config.clock.data_only)) {
            return;
        }
        double offset_ms =
            static_cast<double>(config.clock.beat_offset_microseconds) / 1000.0;
        const double before = offset_ms;
        if (!prompt_real("Beat offset (milliseconds)", offset_ms,
                         -86400000.0, 86400000.0)) {
            return;
        }
        const auto next = static_cast<std::int64_t>(std::llround(offset_ms * 1000.0));
        if (before != offset_ms || config.clock.beat_offset_microseconds != next) {
            config.clock.beat_offset_microseconds = next;
            g_prompt_changed = true;
        }

    }

    if (!configure_audio_response_profile(
            "Audio response — project-wide defaults",
            config.audio_reactive_defaults)
        || !prompt_bool("Override project audio response for this layer",
                        config.audio_reactive_override_enabled)) {
        return;
    }
    if (config.audio_reactive_override_enabled
        && !configure_audio_response_profile(
            "Audio response — active-layer override",
            config.audio_reactive)) {
        return;
    }

    std::string count_error;
    const int count = pvt::effective_frame_count(config, &count_error);
    if (count > 0) {
        std::cout << "Effective export: " << count << " frame(s), "
                  << static_cast<double>(count) / config.fps << " seconds at "
                  << config.fps << " FPS.\n";
    } else {
        std::cout << "Clock needs attention: " << count_error << '\n';
    }

    configure_active_layer_clock(config, document, layer_uuid);

    std::cout << "\n-- Authored swings for the active layer --\n";
    configure_swings(config);
}

void configure_canvas(RenderConfig& config) {
    std::cout << "\n-- Canvas and timing --\n";
    prompt_int("Width", config.width, 16, 16384);
    prompt_int("Height", config.height, 16, 16384);
    prompt_int("Block size", config.block_size, 1, 16384);
    prompt_real("Playback FPS", config.fps, 1.0, 240.0);
    prompt_int("Frames per loop", config.total_frames, 2, 1000000);
}

void configure_surface(RenderConfig& config,
                       ProjectDocument& document,
                       const std::string& layer_uuid) {
    std::cout << "\n-- Surface and procedural features --\n";
    prompt_bool("Coordinate displacement", config.displacement_enabled);
    prompt_real("Displacement strength", config.displacement, 0.0, 1000.0);
    prompt_bool("Slope lighting", config.lighting_enabled);
    prompt_real("Lighting depth", config.wave_depth, 0.0, 10.0);
    prompt_bool("Spiral signal", config.spiral_enabled);
    prompt_real("Spiral frequency", config.spiral_frequency, 0.0, 1000.0);
    prompt_int("Spiral arms", config.spiral_arms, -100, 100);
    prompt_bool("Wall-reflection signal", config.wall_reflection_enabled);
    prompt_real("Wall-reflection frequency", config.wall_frequency, 0.0, 1000.0);
    prompt_real("Wall-reflection mix", config.wall_mix, 0.0, 5.0);
    prompt_bool("3D surface mapping enabled", config.surface.enabled);
    prompt_enum("Surface mapping", config.surface.mapping,
                {{pvt::SurfaceMapping::Plane, "Plane"},
                 {pvt::SurfaceMapping::Cylinder, "Cylinder"},
                 {pvt::SurfaceMapping::Sphere, "Sphere"},
                 {pvt::SurfaceMapping::Cube, "Cube"},
                 {pvt::SurfaceMapping::CustomObj, "Custom OBJ"}});
    if (config.surface.mapping == pvt::SurfaceMapping::CustomObj) {
        const std::string previous_path = config.surface.obj_path;
        const bool changed_before_path_prompt = g_prompt_changed;
        if (!prompt_text("OBJ file path", config.surface.obj_path,
                         kMaximumPathBytes)) {
            return;
        }
        if (config.surface.obj_path != previous_path) {
            ProjectDocument candidate = document;
            pvt::ProjectAttachment attached;
            std::string error;
            if (!pvt::attach_project_file(
                    candidate, pvt::surface_obj_attachment_id(layer_uuid),
                    config.surface.obj_path, &attached, &error)) {
                std::cout << "Could not embed that OBJ; the previous object remains: "
                          << error << '\n';
                config.surface.obj_path = previous_path;
                g_prompt_changed = changed_before_path_prompt;
                return;
            }
            document = std::move(candidate);
            config.surface.obj_path = attached.local_path;
            config.surface.obj_sha256 = attached.sha256;
            config.surface.obj_basename = attached.basename;
        }
    }
    prompt_int("Surface rotations per loop", config.surface.rotations_per_loop, -1000, 1000);
    prompt_real("Surface starting phase (degrees)", config.surface.phase_degrees,
                -36000.0, 36000.0);
    prompt_real("Surface curvature", config.surface.curvature, 0.0, 1.0);
    prompt_real("Surface lighting", config.surface.lighting, 0.0, 10.0);

    std::cout << "\n-- Layer transform (before mapped-object effects; mirror before flips) --\n";
    if (!prompt_enum("Mirror mode", config.transform.mirror,
                     {{pvt::MirrorMode::None, "Off"},
                      {pvt::MirrorMode::LeftToRight, "Copy left half to right"},
                      {pvt::MirrorMode::RightToLeft, "Copy right half to left"},
                      {pvt::MirrorMode::TopToBottom, "Copy top half to bottom"},
                      {pvt::MirrorMode::BottomToTop, "Copy bottom half to top"},
                      {pvt::MirrorMode::FourWay,
                       "Four-way mirror from the top-left quadrant"}})
        || !prompt_bool("Flip horizontally", config.transform.flip_horizontal)
        || !prompt_bool("Flip vertically", config.transform.flip_vertical)) {
        return;
    }
    std::cout << "\n-- Seamless layer motion (compact path animation) --\n";
    if (!prompt_bool("Enable layer motion", config.motion.enabled)) return;
    if (config.motion.enabled
        && (!prompt_enum("Closed motion path", config.motion.path,
                         {{pvt::LayerMotionPath::None, "Static placement only"},
                          {pvt::LayerMotionPath::Orbit, "Orbit"},
                          {pvt::LayerMotionPath::FigureEight, "Figure eight"},
                          {pvt::LayerMotionPath::Bounce, "Bounce"},
                          {pvt::LayerMotionPath::Lissajous, "Lissajous"}})
            || !prompt_real("Path center X", config.motion.center_x, -10.0, 10.0)
            || !prompt_real("Path center Y", config.motion.center_y, -10.0, 10.0)
            || !prompt_real("Horizontal travel", config.motion.travel_x, 0.0, 10.0)
            || !prompt_real("Vertical travel", config.motion.travel_y, 0.0, 10.0)
            || !prompt_int("Horizontal cycles", config.motion.cycles_x, -1000, 1000)
            || !prompt_int("Vertical cycles", config.motion.cycles_y, -1000, 1000)
            || !prompt_real("Path phase (degrees)", config.motion.phase_degrees,
                            -36000.0, 36000.0)
            || !prompt_int("Layer rotations per loop",
                           config.motion.rotations_per_loop, -1000, 1000)
            || !prompt_real("Scale pulse", config.motion.scale_pulse,
                            0.0, 0.95))) {
        return;
    }
    if (config.motion.enabled) config.output.write_alpha = true;
    if (config.surface.enabled
        && config.surface.mapping != pvt::SurfaceMapping::Plane
        && config.surface.curvature > 0.0) {
        g_prompt_changed = g_prompt_changed || !config.output.write_alpha;
        config.output.write_alpha = true;
        std::cout << "Final RGBA output enabled for the 3D surface exterior.\n";
    }
}

void configure_alpha(RenderConfig& config) {
    std::cout << "\n-- Per-layer alpha modulation --\n"
              << "RGB remains present even where alpha is zero (straight/unassociated alpha).\n";
    prompt_bool("Enable procedural alpha modulation", config.alpha.enabled);
    prompt_real("Minimum alpha", config.alpha.minimum, 0.0, 1.0);
    prompt_real("Maximum alpha", config.alpha.maximum, 0.0, 1.0);
    prompt_real("Alpha spatial frequency", config.alpha.spatial_frequency, 0.0, 1000.0);
    prompt_int("Alpha cycles per loop", config.alpha.cycles_per_loop, -1000, 1000);
    prompt_real("Alpha starting phase (degrees)", config.alpha.phase_degrees,
                -36000.0, 36000.0);
}

void configure_export(RenderConfig& config) {
    std::cout << "\n-- Export --\n";
    prompt_bool("Write final alpha channel (RGBA)", config.output.write_alpha);
    prompt_text("Output directory", config.output.output_directory, kMaximumPathBytes);
    prompt_filename_prefix(config.output.filename_prefix);
    prompt_int("First frame number", config.output.first_frame_number, 0, 1000000000);
    prompt_int("Minimum zero-padding digits", config.output.filename_digits, 1, 12);
    prompt_bool("Overwrite matching existing frames", config.output.overwrite_existing);

    for (;;) {
        int bit_depth = config.output.bit_depth;
        if (!prompt_int("Bit depth per channel (8/16 PNG, 32 float EXR)", bit_depth, 8, 32)) {
            return;
        }
        if (bit_depth == 8 || bit_depth == 16 || bit_depth == 32) {
            config.output.bit_depth = bit_depth;
            break;
        }
        std::cout << "Only 8, 16, or 32 are supported.\n";
    }
    if (config.output.bit_depth == 32) {
        config.output.dither_enabled = false;
        std::cout << "PNG compression and dithering are ignored for full-float EXR.\n";
    } else {
        prompt_int("PNG compression (0 off, 9 maximum)",
                   config.output.png_compression_level, 0, 9);
        prompt_bool("Dither before integer quantization", config.output.dither_enabled);
        prompt_enum("Dither method", config.output.dither_method,
                    {{pvt::DitherMethod::BlueNoise, "Deterministic blue-noise-like"},
                     {pvt::DitherMethod::OrderedBayer, "Ordered Bayer 8x8"},
                     {pvt::DitherMethod::FloydSteinberg, "Floyd-Steinberg error diffusion"}});
    }
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

void report_recovered_fields(const ProjectDocument& document) {
    const pvt::ProjectRecoveryInfo recovery =
        pvt::project_recovery_info(document.project);
    if (recovery.preserved_fields == 0U && recovery.notes.empty()) return;
    std::cerr << "Recovered project settings: applied every safe field and kept "
              << recovery.preserved_fields << " unrecognized/original field(s)"
              << " for lossless future saves";
    if (recovery.rejected_fields != 0U) {
        std::cerr << " (" << recovery.rejected_fields
                  << " could not be used safely)";
    }
    std::cerr << ".\n";
    const std::size_t shown = std::min<std::size_t>(recovery.notes.size(), 3U);
    for (std::size_t index = 0U; index < shown; ++index) {
        std::cerr << "  - " << recovery.notes[index] << '\n';
    }
    if (recovery.notes.size() > shown) {
        std::cerr << "  - " << (recovery.notes.size() - shown)
                  << " additional repair note(s).\n";
    }
}

bool has_case_insensitive_suffix(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size()
           && lower_ascii(value.substr(value.size() - suffix.size())) == lower_ascii(suffix);
}

std::string output_extension(const pvt::ExportConfig& output) {
    return output.bit_depth == 32 ? ".exr" : ".png";
}

RenderConfig active_render_config(const CliState& state) {
    const auto& project = state.document.project;
    return pvt::apply_global_config(project.canvas, project.output,
                                    project.layers.at(state.active_layer).render);
}

void commit_active_render(CliState& state, const RenderConfig& config, bool changed) {
    auto& project = state.document.project;
    project.canvas.width = config.width;
    project.canvas.height = config.height;
    project.canvas.block_size = config.block_size;
    project.canvas.total_frames = config.total_frames;
    project.canvas.fps = config.fps;
    project.canvas.clock = config.clock;
    project.canvas.audio_reactive_defaults =
        config.audio_reactive_defaults;
    project.canvas.motion_paths = config.motion_paths;
    project.canvas.output_compatibility = config.output_compatibility;
    project.output = config.output;
    project.layers.at(state.active_layer).render =
        static_cast<const pvt::RenderData&>(config);
    state.document.dirty = state.document.dirty || changed;
}

std::size_t display_to_internal(std::size_t display_index, std::size_t count) {
    return count - display_index - 1;
}

bool prompt_blend_mode(pvt::BlendMode& mode) {
    return prompt_enum("Blend mode (Normal is saved as 'none')", mode,
                       {{pvt::BlendMode::Normal, "Normal / none"},
                        {pvt::BlendMode::SoftLight, "Soft light"},
                        {pvt::BlendMode::GrainMerge, "Grain merge"},
                        {pvt::BlendMode::Overlay, "Overlay"},
                        {pvt::BlendMode::ColorDodge, "Color dodge"},
                        {pvt::BlendMode::LinearBurn, "Linear burn"},
                        {pvt::BlendMode::ColorBurn, "Color burn"},
                        {pvt::BlendMode::Difference, "Difference"},
                        {pvt::BlendMode::Subtract, "Subtract"},
                        {pvt::BlendMode::Multiply, "Multiply"},
                        {pvt::BlendMode::Add, "Add"},
                        {pvt::BlendMode::Erase, "Erase lower layers"},
                        {pvt::BlendMode::ColorEraseTones, "Color eraser (tones)"},
                        {pvt::BlendMode::ColorEraseBrightness,
                         "Color eraser (brightness)"}});
}

void configure_project_and_layers(CliState& state) {
    for (;;) {
        auto& project = state.document.project;
        const std::size_t count = project.layers.size();
        std::cout << "\n-- Project and layers --\n"
                  << "Project: " << project.name << "\n"
                  << "Rows are top-to-bottom paint order; the first row is topmost.\n";
        for (std::size_t row = 0; row < count; ++row) {
            const std::size_t index = display_to_internal(row, count);
            const auto& layer = project.layers[index];
            std::cout << (index == state.active_layer ? " *" : "  ") << (row + 1)
                      << ") " << (layer.enabled ? "on  " : "off ") << layer.name
                      << " | " << pvt::blend_mode_name(layer.blend_mode)
                      << " | opacity " << layer.opacity << '\n';
        }
        std::cout << "e N edit/select, a add, c N duplicate, d N delete, "
                     "m FROM TO move, p rename project, b back.\n";
        std::string input;
        if (!read_line("Layer action [b]: ", input) || input.empty()
            || input == "b" || input == "B") {
            return;
        }
        if (input == "p" || input == "P") {
            g_prompt_changed = false;
            prompt_project_name(project.name);
            state.document.dirty = state.document.dirty || g_prompt_changed;
            continue;
        }
        if (input == "a" || input == "A") {
            if (count >= pvt::kMaximumLayers) {
                std::cout << "The safety limit of " << pvt::kMaximumLayers
                          << " layers has been reached.\n";
                continue;
            }
            auto layer = pvt::default_layer(count);
            layer.file_id = pvt::allocate_layer_file_id(project);
            project.layers.push_back(std::move(layer));
            state.active_layer = project.layers.size() - 1;
            if (project.layers.size() > 1) {
                project.output.write_alpha = true;
            }
            state.document.dirty = true;
            continue;
        }

        std::istringstream command(input);
        char action = '\0';
        long long first = 0;
        long long second = 0;
        std::string extra;
        if (!(command >> action >> first) || first < 1
            || first > static_cast<long long>(count)) {
            std::cout << "Use one of the listed layer commands and row numbers.\n";
            continue;
        }
        const std::size_t first_index =
            display_to_internal(static_cast<std::size_t>(first - 1), count);
        if (action == 'e' || action == 'E') {
            if (command >> extra) {
                std::cout << "Edit accepts one row number.\n";
                continue;
            }
            state.active_layer = first_index;
            auto& layer = project.layers[first_index];
            g_prompt_changed = false;
            prompt_layer_name(layer.name);
            prompt_bool("Enabled", layer.enabled);
            prompt_blend_mode(layer.blend_mode);
            prompt_real("Opacity", layer.opacity, 0.0, 1.0);
            state.document.dirty = state.document.dirty || g_prompt_changed;
        } else if (action == 'c' || action == 'C') {
            if (command >> extra || count >= pvt::kMaximumLayers) {
                std::cout << (count >= pvt::kMaximumLayers
                                  ? "The layer safety limit has been reached.\n"
                                  : "Duplicate accepts one row number.\n");
                continue;
            }
            auto copy = project.layers[first_index];
            copy.uuid = pvt::generate_uuid();
            copy.file_id = pvt::allocate_layer_file_id(project);
            if (copy.name.size() <= kMaximumNameBytes - 5U) {
                copy.name += " Copy";
            }
            ProjectDocument staged_document = state.document;
            const auto duplicate_attachment =
                [&](const std::string& source_id,
                    const std::string& destination_id,
                    pvt::ProjectAttachment& attached) {
                    const pvt::ProjectAttachment* source_attachment =
                        pvt::find_project_attachment(staged_document, source_id);
                    if (source_attachment == nullptr
                        || source_attachment->local_path.empty()) {
                        return false;
                    }
                    const std::string source_path = source_attachment->local_path;
                    std::string attachment_error;
                    if (!pvt::attach_project_file(
                            staged_document, destination_id, source_path,
                            &attached, &attachment_error)) {
                        std::cout << "Could not duplicate the layer attachment: "
                                  << attachment_error << '\n';
                        return false;
                    }
                    return true;
                };
            if (!copy.render.surface.obj_sha256.empty()) {
                pvt::ProjectAttachment attached;
                if (!duplicate_attachment(
                        pvt::surface_obj_attachment_id(
                            project.layers[first_index].uuid),
                        pvt::surface_obj_attachment_id(copy.uuid), attached)) {
                    std::cout << "The embedded custom OBJ source is unavailable.\n";
                    continue;
                }
                copy.render.surface.obj_path = attached.local_path;
                copy.render.surface.obj_sha256 = attached.sha256;
                copy.render.surface.obj_basename = attached.basename;
            }
            if (!copy.render.layer_clock.clock.music.source_sha256.empty()) {
                pvt::ProjectAttachment attached;
                if (!duplicate_attachment(
                        pvt::layer_music_attachment_id(
                            project.layers[first_index].uuid),
                        pvt::layer_music_attachment_id(copy.uuid), attached)) {
                    std::cout << "The embedded active-layer music source is unavailable.\n";
                    continue;
                }
                copy.render.layer_clock.clock.music.source_sha256 = attached.sha256;
                copy.render.layer_clock.clock.music.source_basename = attached.basename;
            }
            if (!copy.render.starting_image.sha256.empty()) {
                pvt::ProjectAttachment attached;
                if (!duplicate_attachment(
                        pvt::starting_image_attachment_id(
                            project.layers[first_index].uuid),
                        pvt::starting_image_attachment_id(copy.uuid), attached)) {
                    std::cout << "The embedded starting image is unavailable.\n";
                    continue;
                }
                copy.render.starting_image.path = attached.local_path;
                copy.render.starting_image.sha256 = attached.sha256;
                copy.render.starting_image.basename = attached.basename;
            }
            state.document.attachments = std::move(staged_document.attachments);
            state.document.attachment_cache =
                std::move(staged_document.attachment_cache);
            project.layers.insert(project.layers.begin()
                                      + static_cast<std::ptrdiff_t>(first_index + 1),
                                  std::move(copy));
            state.active_layer = first_index + 1;
            project.output.write_alpha = true;
            state.document.dirty = true;
        } else if (action == 'd' || action == 'D') {
            if (command >> extra || count == 1) {
                std::cout << (count == 1 ? "A project must retain one layer.\n"
                                         : "Delete accepts one row number.\n");
                continue;
            }
            ProjectDocument staged_document = state.document;
            bool detached = true;
            for (const std::string& reference_id : {
                     pvt::surface_obj_attachment_id(
                         project.layers[first_index].uuid),
                     pvt::starting_image_attachment_id(
                         project.layers[first_index].uuid),
                     pvt::layer_music_attachment_id(
                         project.layers[first_index].uuid)}) {
                std::string attachment_error;
                if (!pvt::detach_project_file(
                        staged_document, reference_id, &attachment_error)) {
                    std::cout << "Could not remove the layer attachment: "
                              << attachment_error << '\n';
                    detached = false;
                    break;
                }
            }
            if (!detached) continue;
            state.document.attachments = std::move(staged_document.attachments);
            state.document.attachment_cache =
                std::move(staged_document.attachment_cache);
            project.layers.erase(project.layers.begin()
                                 + static_cast<std::ptrdiff_t>(first_index));
            if (state.active_layer == first_index) {
                state.active_layer = std::min(first_index, project.layers.size() - 1);
            } else if (state.active_layer > first_index) {
                --state.active_layer;
            }
            state.document.dirty = true;
        } else if (action == 'm' || action == 'M') {
            if (!(command >> second) || (command >> extra) || second < 1
                || second > static_cast<long long>(count)) {
                std::cout << "Move requires two existing row numbers.\n";
                continue;
            }
            const std::size_t second_index =
                display_to_internal(static_cast<std::size_t>(second - 1), count);
            if (first_index != second_index) {
                const std::string active_uuid = project.layers[state.active_layer].uuid;
                auto layer = std::move(project.layers[first_index]);
                project.layers.erase(project.layers.begin()
                                     + static_cast<std::ptrdiff_t>(first_index));
                project.layers.insert(project.layers.begin()
                                          + static_cast<std::ptrdiff_t>(second_index),
                                      std::move(layer));
                const auto active = std::find_if(
                    project.layers.begin(), project.layers.end(),
                    [&](const pvt::LayerConfig& candidate) {
                        return candidate.uuid == active_uuid;
                    });
                state.active_layer = static_cast<std::size_t>(
                    std::distance(project.layers.begin(), active));
                state.document.dirty = true;
            }
        } else {
            std::cout << "Unrecognized layer action.\n";
        }
    }
}

void print_version_list(const ProjectDocument& document) {
    std::cout << "\nVersions for " << document.source_path << ":\n";
    if (document.externally_modified) {
        std::cout << "  Bundle state has an external change/integrity mismatch; "
                     "an explicit Save will preserve it as a new version.\n";
    }
    for (const auto& version : document.versions) {
        std::cout << (version.number == document.current_version ? " * " : "   ")
                  << version.number << " | " << version.saved_utc << " | "
                  << version.layer_count << " layer(s) | " << version.reason;
        if (!version.indexed) {
            std::cout << " | not indexed by root metadata";
        }
        if (!version.valid) {
            std::cout << " | invalid";
        }
        if (version.externally_modified) {
            std::cout << " | integrity mismatch / external change";
        }
        if (!version.integrity_message.empty()) {
            std::cout << " | " << version.integrity_message;
        }
        std::cout << '\n';
    }
}

void manage_versions(CliState& state) {
    if (state.document.source_path.empty() || state.document.legacy_import) {
        std::cout << "Save this project as a bundle before managing versions.\n";
        return;
    }
    for (;;) {
        print_version_list(state.document);
        std::cout << "d BEFORE AFTER diff, c N make current, r N revert as new, "
                     "v validate bundle, b back.\n";
        std::string input;
        if (!read_line("Version action [b]: ", input) || input.empty()
            || input == "b" || input == "B") {
            return;
        }
        if (input == "v" || input == "V") {
            std::vector<pvt::BundleVersionInfo> versions;
            std::string error;
            if (pvt::validate_project_bundle(state.document.source_path, &versions, &error)) {
                std::cout << "Validated all " << versions.size() << " version(s).\n";
            } else {
                std::cout << "Validation failed: " << error << '\n';
            }
            continue;
        }
        std::istringstream command(input);
        char action = '\0';
        unsigned long long first = 0;
        unsigned long long second = 0;
        std::string extra;
        if (!(command >> action >> first)) {
            std::cout << "Use one of the listed version commands.\n";
            continue;
        }
        std::string error;
        if (action == 'd' || action == 'D') {
            if (!(command >> second) || (command >> extra)) {
                std::cout << "Diff requires two version numbers.\n";
                continue;
            }
            std::vector<pvt::BundleDiffEntry> diffs;
            if (!pvt::diff_project_versions(state.document, first, second, diffs, &error)) {
                std::cout << "Could not compare versions: " << error << '\n';
                continue;
            }
            if (diffs.empty()) {
                std::cout << "The snapshots are semantically identical.\n";
            }
            for (const auto& diff : diffs) {
                std::cout << diff.field << ": " << diff.before << " -> " << diff.after << '\n';
            }
        } else if (action == 'c' || action == 'C' || action == 'r' || action == 'R') {
            if (command >> extra) {
                std::cout << "This command accepts one version number.\n";
                continue;
            }
            if (state.document.dirty) {
                bool discard = false;
                g_prompt_changed = false;
                if (!prompt_bool("Discard unsaved session changes", discard) || !discard) {
                    continue;
                }
            }
            pvt::BundleSaveReport report;
            const bool ok = (action == 'c' || action == 'C')
                                ? pvt::make_project_version_current(
                                      state.document, first, &report, &error)
                                : pvt::revert_project_as_new(
                                      state.document, first, &report, &error);
            if (!ok) {
                std::cout << "Version operation failed: " << error << '\n';
            } else {
                state.active_layer = 0;
                std::cout << ((action == 'c' || action == 'C')
                                  ? "Changed the current pointer.\n"
                                  : "Created a reversible rollback as version "
                                        + std::to_string(report.version) + ".\n");
            }
        } else {
            std::cout << "Unrecognized version action.\n";
        }
    }
}

bool save_project_interactive(CliState& state) {
    std::string path = state.document.source_path.empty()
                           ? pvt::portable_project_filename(state.document.project.name)
                           : state.document.source_path;
    g_prompt_changed = false;
    if (!prompt_text("Bundle ZIP or directory", path, kMaximumPathBytes)) {
        return false;
    }
    if (has_case_insensitive_suffix(path, ".pvt")) {
        std::cout << "Normal saves are versioned bundles, not .pvt files. "
                     "Choose a .zip filename or bundle directory.\n";
        return true;
    }
    pvt::BundleSaveReport report;
    std::string error;
    if (!pvt::save_project_document(state.document, path, &report, &error)) {
        std::cout << "Could not save project: " << error << '\n';
        return true;
    }
    if (report.validated_only) {
        if (report.compacted_storage) {
            std::cout << "No project changes; validated and compacted shared music analysis.\n";
        } else {
            std::cout << "No changes; validated the complete bundle.\n";
        }
    } else {
        std::cout << "Saved version " << report.version << " to " << report.path << ".\n";
    }
    return true;
}

bool resolve_unsaved_changes(CliState& state, const std::string& action) {
    if (!state.document.dirty) {
        return true;
    }
    for (;;) {
        std::string input;
        if (!read_line("Unsaved changes: s save, d discard and " + action
                           + ", c cancel [c]: ",
                       input)) {
            std::cerr << "Input ended; the unsaved project was not written.\n";
            return false;
        }
        if (input.empty() || input == "c" || input == "C") {
            return false;
        }
        if (input == "d" || input == "D") {
            return true;
        }
        if (input == "s" || input == "S") {
            if (!save_project_interactive(state)) {
                return false;
            }
            if (!state.document.dirty) {
                return true;
            }
            std::cout << "The project is still unsaved. Choose another action.\n";
            continue;
        }
        std::cout << "Choose s, d, or c.\n";
    }
}

void print_summary(const CliState& state) {
    const auto& project = state.document.project;
    const auto& layer = project.layers.at(state.active_layer);
    const auto validation = pvt::validate(project);
    std::string frame_count_error;
    const int frame_count = pvt::effective_frame_count(project.canvas,
                                                        &frame_count_error);
    std::cout << "\n============================================================\n"
              << " " << project.name << " — Procedural Visualizer Tool\n"
              << "============================================================\n"
              << "Canvas: " << project.canvas.width << 'x' << project.canvas.height
              << " | block " << project.canvas.block_size << " | "
              << (frame_count > 0 ? frame_count : project.canvas.total_frames)
              << " effective frames at " << project.canvas.fps << " fps | clock "
              << pvt::clock_mode_name(project.canvas.clock.mode)
              << (frame_count > 0 && frame_count != project.canvas.total_frames
                      ? " (manual frame count retained: "
                            + std::to_string(project.canvas.total_frames) + ")"
                      : "")
              << "\n"
              << "Layers: " << project.layers.size() << " | editing " << layer.name
              << " (" << (layer.enabled ? "on" : "off") << ", "
              << pvt::blend_mode_name(layer.blend_mode) << ", opacity "
              << layer.opacity << ")\n"
              << "Active stack: " << layer.render.waves.size() << " wave(s), "
              << layer.render.swings.size() << " swing(s) "
              << (layer.render.swings_enabled ? "enabled" : "disabled") << ", "
              << layer.render.effects.size() << " effect(s) | alpha modulation "
              << (layer.render.alpha.enabled ? "on" : "off")
              << " | starting palette "
              << (layer.render.palette.enabled ? layer.render.palette.name : "off")
              << " | mirror " << pvt::mirror_mode_name(layer.render.transform.mirror)
              << (layer.render.transform.flip_horizontal ? " + flip H" : "")
              << (layer.render.transform.flip_vertical ? " + flip V" : "") << "\n"
              << "Output: " << project.output.bit_depth
              << (project.output.bit_depth == 32 ? "-bit float " : "-bit ")
              << (project.output.write_alpha ? "RGBA" : "RGB")
              << output_extension(project.output) << " | "
              << (project.output.dither_enabled && project.output.bit_depth != 32
                      ? pvt::dither_method_name(project.output.dither_method)
                      : "dither off")
              << (project.output.bit_depth == 32
                      ? ""
                      : " | PNG compression "
                            + std::to_string(project.output.png_compression_level))
              << "\nBundle: "
              << (state.document.source_path.empty() ? "not yet saved"
                                                     : state.document.source_path)
              << (state.document.dirty ? " | modified" : " | clean")
              << (state.document.externally_modified
                      ? " | external change/integrity mismatch" : "")
              << "\nPeak working-memory estimate: " << std::fixed << std::setprecision(1)
              << static_cast<double>(validation.estimated_peak_bytes) / (1024.0 * 1024.0)
              << " MiB\n" << std::defaultfloat << std::setprecision(6);
    if (!validation.ok) {
        std::cout << "Project needs attention: " << validation.message << "\n";
    } else if (frame_count < 0) {
        std::cout << "Clock needs attention: " << frame_count_error << "\n";
    }
    std::cout << "\n1) Project name and layers\n"
              << "2) Canvas size, FPS, and retained manual frame count (global)\n"
              << "3) Synchronization, music response, and swings\n"
              << "4) Waves for active layer\n"
              << "5) Effects for active layer\n"
              << "6) Surface, transforms, and procedural features for active layer\n"
              << "7) Palette, color, and quantization for active layer\n"
              << "8) Procedural alpha modulation for active layer\n"
              << "9) Export settings (global)\n"
              << "10) Save project bundle\n"
              << "11) Open bundle, directory, or legacy .pvt\n"
              << "12) Version history\n"
              << "13) Restore new-project defaults\n"
              << "14) Render composite sequence (press Enter)\n"
              << "0) Quit\n";
}

bool interactive_menu(CliState& state) {
    for (;;) {
        print_summary(state);
        std::string input;
        if (!read_line("Choice [14]: ", input)) {
            if (state.document.dirty) {
                std::cerr << "Input ended; the unsaved project was not written.\n";
            }
            return true;
        }
        if (input.empty()) {
            input = "14";
        }
        long long choice = 0;
        if (!parse_integer(input, 0, 14, choice)) {
            std::cout << "Choose a menu number from 0 through 14.\n";
            continue;
        }
        if (choice == 0) {
            if (resolve_unsaved_changes(state, "quit")) {
                return true;
            }
            continue;
        }
        if (choice == 1) {
            configure_project_and_layers(state);
            continue;
        }
        if (choice >= 2 && choice <= 9) {
            RenderConfig config = active_render_config(state);
            g_prompt_changed = false;
            switch (choice) {
                case 2: configure_canvas(config); break;
                case 3:
                    configure_synchronization(
                        config, state.document,
                        state.document.project.layers[state.active_layer].uuid);
                    break;
                case 4: configure_waves(config); break;
                case 5: configure_effects(config); break;
                case 6:
                    configure_surface(
                        config, state.document,
                        state.document.project.layers.at(state.active_layer).uuid);
                    break;
                case 7: configure_color(config); break;
                case 8: configure_alpha(config); break;
                case 9: configure_export(config); break;
                default: break;
            }
            commit_active_render(state, config, g_prompt_changed);
            continue;
        }
        switch (choice) {
            case 10:
                if (!save_project_interactive(state)) {
                    return true;
                }
                break;
            case 11: {
                if (!resolve_unsaved_changes(state, "open another project")) {
                    break;
                }
                std::string path = pvt::portable_project_filename(state.document.project.name);
                g_prompt_changed = false;
                if (!prompt_text("Bundle ZIP, directory, or legacy .pvt", path,
                                 kMaximumPathBytes)) {
                    return true;
                }
                ProjectDocument loaded;
                std::string error;
                if (pvt::load_project_document(path, loaded, &error)) {
                    state.document = std::move(loaded);
                    state.active_layer = 0;
                    report_recovered_fields(state.document);
                    std::cout << (state.document.legacy_import
                                      ? "Imported legacy setup. Its .pvt source will never "
                                        "be overwritten by a normal save.\n"
                                      : "Opened project bundle.\n");
                } else {
                    std::cout << "Could not open project; current state is unchanged: "
                              << error << '\n';
                }
                break;
            }
            case 12:
                manage_versions(state);
                break;
            case 13:
                if (!resolve_unsaved_changes(state, "start a new project")) {
                    break;
                }
                state.document = pvt::default_project_document();
                state.active_layer = 0;
                std::cout << "Started a new project with default fire settings.\n";
                break;
            case 14: {
                std::string error;
                pvt::SequenceRenderOptions render_options;
                render_options.frame.backend = pvt::RenderBackend::CpuAndGpu;
                if (pvt::render_project_sequence(
                        state.document.project,
                        render_options,
                        [](int completed, int total) {
                            std::cout << '\r' << "Rendered " << completed << '/' << total
                                      << std::flush;
                            return true;
                        },
                        nullptr, &error)) {
                    std::cout << "\nDone. The composite loops without a duplicated endpoint.\n";
                    break;
                }
                std::cout << "\nRender did not complete: " << error << '\n';
                break;
            }
            default:
                break;
        }
    }
}

void print_help(const char* program) {
    std::cout
        << "Procedural Visualizer Tool " << PVT_PROGRAM_VERSION << "\n\n"
        << "Usage:\n"
        << "  " << program << "                         Interactive menu\n"
        << "  " << program << " --render [options]      Render a composite sequence\n"
        << "  " << program << " --load FILE [--render]  Open a bundle/directory/.pvt\n"
        << "  " << program << " --self-test             Quick library smoke test (use alone)\n\n"
        << "Project and layer options:\n"
        << "  --project-name TEXT --layer N (1 is bottom) --add-layer NAME\n"
        << "  --blend none|softlight|grain-merge|overlay|color-dodge|linear-burn|\n"
        << "          burn|difference|subtract|multiply|add|erase|\n"
        << "          color-erase-tones|color-erase-brightness\n"
        << "  --layer-opacity N --enable-layer --disable-layer\n"
        << "  --alpha --no-alpha                 Final RGB/RGBA channel selection\n"
        << "  --alpha-modulation --no-alpha-modulation  Active-layer artwork\n\n"
        << "Render and output options:\n"
        << "  --render (or --defaults)\n"
        << "  --width N --height N --block-size N --frames N --fps N\n"
        << "  --clock default|frame|time|meter|music\n"
        << "  --clock-interpolation hold|linear|smoothstep\n"
        << "  --clock-fit exact|sequence --pulse-frames N --pulse-ms N\n"
        << "  --meter TEXT --bpm N --tempo-note N --clock-phase N\n"
        << "  --reverse-clock --forward-clock\n"
        << "  --music FILE --music-tempo half|detected|double\n"
        << "  --beat-offset-ms N\n"
        << "  --project-audio-reactive --no-project-audio-reactive\n"
        << "  --audio-reactive --no-audio-reactive --inherit-audio-reactive\n"
        << "  --swings --no-swings             Active-layer authored swing block\n"
        << "  --waves N --bit-depth 8|16|32 --png-compression 0..9\n"
        << "  --workers 0.." << pvt::kMaximumSequenceWorkers
        << "  (0 auto, 1 sequential)\n"
        << "  --backend cpu|cpu+gpu|gpu          Rendering policy (default cpu+gpu)\n"
        << "  --gpu-in-flight 0.." << pvt::kMaximumGpuFramesInFlight
        << "  (0 uses the bounded default of 2)\n"
        << "  --obj FILE  (enable two-sided custom OBJ wrapping and final alpha)\n"
        << "  --starting-image PNG --image-fit stretch|contain|cover|tile\n"
        << "  --no-starting-image\n"
        << "  --dither blue|bayer|floyd --no-dither\n"
        << "  --output-dir PATH --prefix TEXT --start-frame N --digits N\n"
        << "  --overwrite\n\n"
        << "Persistence options:\n"
        << "  --load FILE                         ZIP, unpacked bundle, or legacy .pvt\n"
        << "  --save FILE                         Save a versioned bundle\n"
        << "  --save-default                      Save to <portable project name>.zip\n"
        << "  --save-legacy FILE                  Explicit one-layer .pvt export\n"
        << "  --list-versions --version --help\n\n"
        << "Options are processed from left to right. Put --load before overrides.\n"
        << "Normal saves never overwrite an imported legacy .pvt. The explicit\n"
        << "--save-legacy escape hatch is rejected for multi-layer projects.\n"
        << "PNG compression defaults to 5 (0 is off, 9 is maximum).\n"
        << "--music stores analysis plus an integrity-checked bundled source asset. "
           "A first import enables project Audio Response; active-layer switches "
           "create an override, while --inherit-audio-reactive removes it.\n"
        << "Float EXR output ignores PNG compression and dithering. "
           "Unspecified values keep their defaults.\n";
}

bool option_takes_value(const std::string& option) {
    return option == "--load" || option == "--save" || option == "--save-legacy"
           || option == "--project-name" || option == "--layer"
           || option == "--add-layer" || option == "--blend"
           || option == "--layer-opacity" || option == "--width"
           || option == "--height" || option == "--block-size" || option == "--frames"
           || option == "--fps" || option == "--waves" || option == "--bit-depth"
           || option == "--clock" || option == "--clock-interpolation"
           || option == "--clock-fit" || option == "--pulse-frames"
           || option == "--pulse-ms" || option == "--meter" || option == "--bpm"
           || option == "--tempo-note" || option == "--clock-phase"
           || option == "--music" || option == "--music-tempo"
           || option == "--beat-offset-ms"
           || option == "--png-compression" || option == "--workers"
           || option == "--backend" || option == "--gpu-in-flight"
           || option == "--obj"
           || option == "--starting-image" || option == "--image-fit"
           || option == "--dither" || option == "--output-dir" || option == "--prefix"
           || option == "--start-frame" || option == "--digits";
}

bool parse_blend_mode(const std::string& text, pvt::BlendMode& mode) {
    const std::string value = lower_ascii(text);
    if (value == "none" || value == "normal") {
        mode = pvt::BlendMode::Normal;
    } else if (value == "softlight" || value == "soft-light") {
        mode = pvt::BlendMode::SoftLight;
    } else if (value == "grain-merge" || value == "grain_merge") {
        mode = pvt::BlendMode::GrainMerge;
    } else if (value == "overlay") {
        mode = pvt::BlendMode::Overlay;
    } else if (value == "color-dodge" || value == "color_dodge") {
        mode = pvt::BlendMode::ColorDodge;
    } else if (value == "linear-burn" || value == "linear_burn") {
        mode = pvt::BlendMode::LinearBurn;
    } else if (value == "burn" || value == "color-burn" || value == "color_burn") {
        mode = pvt::BlendMode::ColorBurn;
    } else if (value == "difference") {
        mode = pvt::BlendMode::Difference;
    } else if (value == "subtract") {
        mode = pvt::BlendMode::Subtract;
    } else if (value == "multiply") {
        mode = pvt::BlendMode::Multiply;
    } else if (value == "add") {
        mode = pvt::BlendMode::Add;
    } else if (value == "erase") {
        mode = pvt::BlendMode::Erase;
    } else if (value == "color-erase-tones"
               || value == "color_erase_tones") {
        mode = pvt::BlendMode::ColorEraseTones;
    } else if (value == "color-erase-brightness"
               || value == "color_erase_brightness") {
        mode = pvt::BlendMode::ColorEraseBrightness;
    } else {
        return false;
    }
    return true;
}

bool parse_starting_image_fit(const std::string& text,
                              pvt::StartingImageFit& fit) {
    const std::string value = lower_ascii(text);
    if (value == "stretch") fit = pvt::StartingImageFit::Stretch;
    else if (value == "contain") fit = pvt::StartingImageFit::Contain;
    else if (value == "cover") fit = pvt::StartingImageFit::Cover;
    else if (value == "tile") fit = pvt::StartingImageFit::Tile;
    else return false;
    return true;
}

bool parse_render_backend(const std::string& text,
                          pvt::RenderBackend& backend) {
    const std::string value = lower_ascii(text);
    if (value == "cpu") {
        backend = pvt::RenderBackend::Cpu;
    } else if (value == "cpu+gpu" || value == "cpu-gpu"
               || value == "hybrid" || value == "auto") {
        backend = pvt::RenderBackend::CpuAndGpu;
    } else if (value == "gpu" || value == "metal") {
        backend = pvt::RenderBackend::Gpu;
    } else {
        return false;
    }
    return true;
}

bool parse_clock_mode(const std::string& text, pvt::ClockMode& mode) {
    const std::string value = lower_ascii(text);
    if (value == "default") {
        mode = pvt::ClockMode::Default;
    } else if (value == "frame") {
        mode = pvt::ClockMode::Frame;
    } else if (value == "time") {
        mode = pvt::ClockMode::Time;
    } else if (value == "meter" || value == "time-signature") {
        mode = pvt::ClockMode::Meter;
    } else if (value == "music") {
        mode = pvt::ClockMode::Music;
    } else {
        return false;
    }
    return true;
}

bool parse_clock_interpolation(const std::string& text,
                               pvt::ClockInterpolation& interpolation) {
    const std::string value = lower_ascii(text);
    if (value == "hold" || value == "static") {
        interpolation = pvt::ClockInterpolation::Hold;
    } else if (value == "linear") {
        interpolation = pvt::ClockInterpolation::Linear;
    } else if (value == "smooth" || value == "smoothstep") {
        interpolation = pvt::ClockInterpolation::Smoothstep;
    } else {
        return false;
    }
    return true;
}

bool parse_clock_fit(const std::string& text, pvt::ClockFit& fit) {
    const std::string value = lower_ascii(text);
    if (value == "exact") {
        fit = pvt::ClockFit::Exact;
    } else if (value == "sequence" || value == "fit" || value == "fit-sequence") {
        fit = pvt::ClockFit::FitSequence;
    } else {
        return false;
    }
    return true;
}

bool parse_music_tempo(const std::string& text, pvt::MusicTempoMode& tempo) {
    const std::string value = lower_ascii(text);
    if (value == "half" || value == "half-time") {
        tempo = pvt::MusicTempoMode::Half;
    } else if (value == "detected" || value == "normal") {
        tempo = pvt::MusicTempoMode::Detected;
    } else if (value == "double" || value == "double-time") {
        tempo = pvt::MusicTempoMode::Double;
    } else {
        return false;
    }
    return true;
}

bool require_value(int argc, char** argv, int& index, const char*& value) {
    if (index + 1 >= argc) {
        std::cerr << "Option '" << argv[index] << "' requires a value.\n";
        return false;
    }
    value = argv[++index];
    return true;
}

bool resize_waves(RenderConfig& config, std::size_t count) {
    if (count > pvt::kMaximumWaves) {
        return false;
    }
    const std::size_t old_size = config.waves.size();
    config.waves.resize(count);
    for (std::size_t i = old_size; i < count; ++i) {
        config.waves[i] = pvt::default_wave(i);
        config.waves[i].id = pvt::allocate_id(config);
    }
    return true;
}

int quick_self_test() {
    pvt::ProjectConfig project = pvt::default_project();
    project.canvas.width = 97;
    project.canvas.height = 65;
    project.canvas.block_size = 8;
    project.canvas.total_frames = 12;
    project.output.write_alpha = true;
    auto& lower_render = project.layers.front().render;
    lower_render.surface.enabled = true;
    lower_render.surface.mapping = pvt::SurfaceMapping::Sphere;
    lower_render.surface.curvature = 0.65;
    lower_render.swings.front().center_x = 0.42;
    lower_render.swings.front().center_y = 0.58;
    lower_render.swings.front().radius = 0.36;
    for (std::size_t index = 0U; index < lower_render.effects.size(); ++index) {
        auto& effect = lower_render.effects[index];
        effect.enabled = true;
        effect.space = (index & 1U) == 0U ? pvt::EffectSpace::Texture
                                         : pvt::EffectSpace::Surface;
        if (effect.type != pvt::EffectType::BlockScale) {
            effect.center_x = 0.44;
            effect.center_y = 0.56;
            effect.area_radius = 0.38;
        }
    }
    lower_render.palette = pvt::default_palette(2U);
    lower_render.transform.mirror = pvt::MirrorMode::LeftToRight;
    lower_render.transform.flip_vertical = true;
    auto upper = pvt::default_layer(1);
    upper.file_id = pvt::allocate_layer_file_id(project);
    upper.name = "Self-test overlay";
    upper.blend_mode = pvt::BlendMode::SoftLight;
    upper.opacity = 0.35;
    project.layers.push_back(std::move(upper));
    pvt::Image first;
    pvt::Image repeated;
    std::string error;
    if (!pvt::render_project_frame_at_phase(project, 0.0, first, nullptr, &error)
        || !pvt::render_project_frame_at_phase(project, 0.0, repeated, nullptr, &error)
        || first.pixels != repeated.pixels) {
        std::cerr << "Self-test failed: "
                  << (error.empty() ? "non-deterministic composite frame" : error)
                  << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "Self-test passed: float RGBA layers, starting-palette/transform stages, "
                 "localized texture/object effects, and blending are deterministic.\n";
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    CliState state;
    pvt::SequenceRenderOptions render_options;
    render_options.frame.backend = pvt::RenderBackend::CpuAndGpu;
    bool render_now = false;
    bool loaded_document = false;
    bool save_default = false;
    bool list_versions = false;
    std::string bundle_to_save;
    std::string legacy_to_save;

    if (argc == 1) {
        return interactive_menu(state) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    const auto mark_changed = [&](auto& destination, const auto& next) {
        if (destination == next) {
            return false;
        }
        destination = next;
        state.document.dirty = true;
        return true;
    };
    const auto mutate_active = [&](auto&& mutation) {
        RenderConfig config = active_render_config(state);
        const bool changed = mutation(config);
        commit_active_render(state, config, changed);
    };

    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        const char* raw_value = nullptr;
        if (option == "--help" || option == "-h") {
            print_help(argv[0]);
            return EXIT_SUCCESS;
        }
        if (option == "--version" || option == "-V") {
            if (argc != 2) {
                std::cerr << "Option '--version' must be used by itself.\n";
                return EXIT_FAILURE;
            }
            std::cout << "Procedural Visualizer Tool "
                      << PVT_PROGRAM_VERSION << '\n';
            return EXIT_SUCCESS;
        }
        if (option == "--self-test") {
            if (argc != 2) {
                std::cerr << "Option '--self-test' must be used by itself.\n";
                return EXIT_FAILURE;
            }
            return quick_self_test();
        }
        if (option == "--render" || option == "--defaults") {
            render_now = true;
            continue;
        }
        if (option == "--save-default") {
            save_default = true;
            continue;
        }
        if (option == "--list-versions") {
            list_versions = true;
            continue;
        }
        if (option == "--overwrite") {
            mark_changed(state.document.project.output.overwrite_existing, true);
            continue;
        }
        if (option == "--alpha") {
            mark_changed(state.document.project.output.write_alpha, true);
            continue;
        }
        if (option == "--no-alpha") {
            mark_changed(state.document.project.output.write_alpha, false);
            continue;
        }
        if (option == "--alpha-modulation") {
            mark_changed(state.document.project.layers.at(state.active_layer).render.alpha.enabled,
                         true);
            continue;
        }
        if (option == "--no-alpha-modulation") {
            mark_changed(state.document.project.layers.at(state.active_layer).render.alpha.enabled,
                         false);
            continue;
        }
        if (option == "--enable-layer") {
            mark_changed(state.document.project.layers.at(state.active_layer).enabled, true);
            continue;
        }
        if (option == "--disable-layer") {
            mark_changed(state.document.project.layers.at(state.active_layer).enabled, false);
            continue;
        }
        if (option == "--no-dither") {
            mark_changed(state.document.project.output.dither_enabled, false);
            continue;
        }
        if (option == "--reverse-clock") {
            mark_changed(state.document.project.canvas.clock.reverse, true);
            continue;
        }
        if (option == "--forward-clock") {
            mark_changed(state.document.project.canvas.clock.reverse, false);
            continue;
        }
        if (option == "--swings") {
            mark_changed(
                state.document.project.layers.at(state.active_layer).render.swings_enabled,
                true);
            continue;
        }
        if (option == "--no-swings") {
            mark_changed(
                state.document.project.layers.at(state.active_layer).render.swings_enabled,
                false);
            continue;
        }
        if (option == "--audio-reactive") {
            auto& render = state.document.project.layers.at(state.active_layer)
                               .render;
            mark_changed(render.audio_reactive_override_enabled, true);
            mark_changed(render.audio_reactive.enabled, true);
            continue;
        }
        if (option == "--no-audio-reactive") {
            auto& render = state.document.project.layers.at(state.active_layer)
                               .render;
            mark_changed(render.audio_reactive_override_enabled, true);
            mark_changed(render.audio_reactive.enabled, false);
            continue;
        }
        if (option == "--inherit-audio-reactive") {
            mark_changed(state.document.project.layers.at(state.active_layer)
                             .render.audio_reactive_override_enabled,
                         false);
            continue;
        }
        if (option == "--project-audio-reactive") {
            mark_changed(
                state.document.project.canvas.audio_reactive_defaults.enabled,
                true);
            continue;
        }
        if (option == "--no-project-audio-reactive") {
            mark_changed(
                state.document.project.canvas.audio_reactive_defaults.enabled,
                false);
            continue;
        }
        if (option == "--no-starting-image") {
            mark_changed(state.document.project.layers.at(state.active_layer)
                             .render.starting_image.enabled,
                         false);
            continue;
        }
        if (!option_takes_value(option)) {
            std::cerr << "Unknown option '" << option << "'. Use --help for usage.\n";
            return EXIT_FAILURE;
        }
        if (!require_value(argc, argv, index, raw_value)) {
            return EXIT_FAILURE;
        }
        const std::string value = raw_value;
        long long integer = 0;
        double real = 0.0;

        if (option == "--load") {
            ProjectDocument loaded;
            std::string error;
            if (!pvt::load_project_document(value, loaded, &error)) {
                std::cerr << "Could not open project: " << error << '\n';
                return EXIT_FAILURE;
            }
            state.document = std::move(loaded);
            state.active_layer = 0;
            loaded_document = true;
            report_recovered_fields(state.document);
        } else if (option == "--save") {
            bundle_to_save = value;
        } else if (option == "--save-legacy") {
            legacy_to_save = value;
        } else if (option == "--project-name" && valid_project_name_text(value)) {
            mark_changed(state.document.project.name, value);
        } else if (option == "--layer"
                   && parse_integer(value, 1,
                                    static_cast<long long>(
                                        state.document.project.layers.size()),
                                    integer)) {
            state.active_layer = static_cast<std::size_t>(integer - 1);
        } else if (option == "--add-layer" && valid_layer_name_text(value)
                   && state.document.project.layers.size() < pvt::kMaximumLayers) {
            auto layer = pvt::default_layer(state.document.project.layers.size());
            layer.file_id = pvt::allocate_layer_file_id(state.document.project);
            layer.name = value;
            state.document.project.layers.push_back(std::move(layer));
            state.active_layer = state.document.project.layers.size() - 1;
            state.document.project.output.write_alpha = true;
            state.document.dirty = true;
        } else if (option == "--blend") {
            pvt::BlendMode mode;
            if (!parse_blend_mode(value, mode)) {
                std::cerr << "Unknown blend mode '" << value << "'. Use --help for names.\n";
                return EXIT_FAILURE;
            }
            mark_changed(state.document.project.layers.at(state.active_layer).blend_mode,
                         mode);
        } else if (option == "--layer-opacity"
                   && parse_real(value, 0.0, 1.0, real)) {
            mark_changed(state.document.project.layers.at(state.active_layer).opacity, real);
        } else if (option == "--width" && parse_integer(value, 16, 16384, integer)) {
            mutate_active([&](RenderConfig& config) {
                if (config.width == integer) return false;
                config.width = static_cast<int>(integer);
                return true;
            });
        } else if (option == "--height" && parse_integer(value, 16, 16384, integer)) {
            mutate_active([&](RenderConfig& config) {
                if (config.height == integer) return false;
                config.height = static_cast<int>(integer);
                return true;
            });
        } else if (option == "--block-size" && parse_integer(value, 1, 16384, integer)) {
            mutate_active([&](RenderConfig& config) {
                if (config.block_size == integer) return false;
                config.block_size = static_cast<int>(integer);
                return true;
            });
        } else if (option == "--frames" && parse_integer(value, 2, 1000000, integer)) {
            mutate_active([&](RenderConfig& config) {
                if (config.total_frames == integer) return false;
                config.total_frames = static_cast<int>(integer);
                return true;
            });
        } else if (option == "--fps" && parse_real(value, 1.0, 240.0, real)) {
            mutate_active([&](RenderConfig& config) {
                if (config.fps == real) return false;
                config.fps = real;
                return true;
            });
        } else if (option == "--clock") {
            pvt::ClockMode mode;
            if (!parse_clock_mode(value, mode)) {
                std::cerr << "Clock mode must be default, frame, time, meter, or music.\n";
                return EXIT_FAILURE;
            }
            mark_changed(state.document.project.canvas.clock.mode, mode);
        } else if (option == "--clock-interpolation") {
            pvt::ClockInterpolation interpolation;
            if (!parse_clock_interpolation(value, interpolation)) {
                std::cerr << "Clock interpolation must be hold, linear, or smoothstep.\n";
                return EXIT_FAILURE;
            }
            mark_changed(state.document.project.canvas.clock.interpolation,
                         interpolation);
        } else if (option == "--clock-fit") {
            pvt::ClockFit fit;
            if (!parse_clock_fit(value, fit)) {
                std::cerr << "Clock fit must be exact or sequence.\n";
                return EXIT_FAILURE;
            }
            mark_changed(state.document.project.canvas.clock.fit, fit);
        } else if (option == "--pulse-frames"
                   && parse_integer(value, 1, 1000000, integer)) {
            mark_changed(state.document.project.canvas.clock.frame_interval,
                         static_cast<int>(integer));
        } else if (option == "--pulse-ms"
                   && parse_real(value, 0.001, 86400000.0, real)) {
            mark_changed(state.document.project.canvas.clock.time_interval_microseconds,
                         static_cast<std::int64_t>(std::llround(real * 1000.0)));
        } else if (option == "--meter") {
            std::string description;
            std::string meter_error;
            if (value.size() > 256U
                || !pvt::describe_meter(value, description, &meter_error)) {
                std::cerr << "Invalid meter: " << meter_error << '\n';
                return EXIT_FAILURE;
            }
            mark_changed(state.document.project.canvas.clock.meter.expression, value);
        } else if (option == "--bpm" && parse_real(value, 1.0, 1000.0, real)) {
            mark_changed(state.document.project.canvas.clock.meter.bpm, real);
        } else if (option == "--tempo-note"
                   && parse_integer(value, 1, 1024, integer)) {
            mark_changed(state.document.project.canvas.clock.meter.tempo_note_denominator,
                         static_cast<int>(integer));
        } else if (option == "--clock-phase"
                   && parse_real(value, -36000.0, 36000.0, real)) {
            mark_changed(state.document.project.canvas.clock.phase_offset_degrees, real);
        } else if (option == "--music") {
            if (!valid_output_directory(value)) {
                std::cerr << "Music path is empty, too long, or contains controls.\n";
                return EXIT_FAILURE;
            }
            pvt::MusicAnalysis analysis;
            std::string analysis_error;
            unsigned last_percent = 101U;
            if (!pvt::audio::analyze_music_file(
                    value, analysis,
                    [&last_percent](std::uint64_t completed, std::uint64_t total) {
                        const unsigned percent = total == 0U
                                                     ? 0U
                                                     : static_cast<unsigned>(
                                                           std::min<long double>(
                                                               100.0L,
                                                               static_cast<long double>(completed)
                                                                   * 100.0L
                                                                   / static_cast<long double>(total)));
                        if (percent != last_percent) {
                            std::cerr << '\r' << "Analyzing music… " << percent << '%'
                                      << std::flush;
                            last_percent = percent;
                        }
                        return true;
                    },
                    nullptr, &analysis_error)) {
                std::cerr << "\rCould not analyze music: " << analysis_error << '\n';
                return EXIT_FAILURE;
            }
            std::cerr << "\rAnalyzed " << analysis.source_basename << ": "
                      << analysis.duration_seconds << " s, " << analysis.detected_bpm
                      << " BPM, " << analysis.beat_times_seconds.size() << " beat(s).\n";
            ProjectDocument candidate = state.document;
            pvt::ProjectAttachment attached;
            if (!pvt::attach_project_file(
                    candidate, pvt::kMusicSourceAttachmentId, value,
                    &attached, &analysis_error)) {
                std::cerr << "Could not embed the analyzed music source: "
                          << analysis_error << '\n';
                return EXIT_FAILURE;
            }
            if (attached.sha256 != analysis.source_sha256) {
                std::cerr << "The music source changed while it was being analyzed; "
                             "run the command again.\n";
                return EXIT_FAILURE;
            }
            auto& clock = candidate.project.canvas.clock;
            const bool first_music_source = clock.music.source_sha256.empty();
            clock.music = std::move(analysis);
            clock.mode = pvt::ClockMode::Music;
            clock.music_swing_policy = pvt::MusicSwingPolicy::KeepAll;
            if (first_music_source) {
                candidate.project.canvas.audio_reactive_defaults.enabled = true;
            }
            candidate.dirty = true;
            state.document = std::move(candidate);
        } else if (option == "--music-tempo") {
            pvt::MusicTempoMode tempo;
            if (!parse_music_tempo(value, tempo)) {
                std::cerr << "Music tempo must be half, detected, or double.\n";
                return EXIT_FAILURE;
            }
            mark_changed(state.document.project.canvas.clock.music_tempo, tempo);
        } else if (option == "--beat-offset-ms"
                   && parse_real(value, -86400000.0, 86400000.0, real)) {
            mark_changed(state.document.project.canvas.clock.beat_offset_microseconds,
                         static_cast<std::int64_t>(std::llround(real * 1000.0)));
        } else if (option == "--waves" && parse_integer(value, 0,
                                                          pvt::kMaximumWaves, integer)) {
            mutate_active([&](RenderConfig& config) {
                if (config.waves.size() == static_cast<std::size_t>(integer)) return false;
                return resize_waves(config, static_cast<std::size_t>(integer));
            });
        } else if (option == "--bit-depth" && parse_integer(value, 8, 32, integer)
                   && (integer == 8 || integer == 16 || integer == 32)) {
            mark_changed(state.document.project.output.bit_depth,
                         static_cast<int>(integer));
            if (integer == 32) {
                mark_changed(state.document.project.output.dither_enabled, false);
            }
        } else if (option == "--png-compression"
                   && parse_integer(value, 0, 9, integer)) {
            mark_changed(state.document.project.output.png_compression_level,
                         static_cast<int>(integer));
        } else if (option == "--workers"
                   && parse_integer(value, 0,
                                    static_cast<long long>(
                                        pvt::kMaximumSequenceWorkers),
                                    integer)) {
            render_options.worker_count = static_cast<std::size_t>(integer);
        } else if (option == "--backend") {
            pvt::RenderBackend backend;
            if (!parse_render_backend(value, backend)) {
                std::cerr << "Rendering backend must be cpu, cpu+gpu, or gpu.\n";
                return EXIT_FAILURE;
            }
            render_options.frame.backend = backend;
        } else if (option == "--gpu-in-flight"
                   && parse_integer(
                          value, 0,
                          static_cast<long long>(pvt::kMaximumGpuFramesInFlight),
                          integer)) {
            render_options.frame.maximum_gpu_frames_in_flight =
                static_cast<std::size_t>(integer);
        } else if (option == "--obj" && valid_output_directory(value)) {
            ProjectDocument candidate = state.document;
            pvt::ProjectAttachment attached;
            std::string attachment_error;
            const std::string reference_id = pvt::surface_obj_attachment_id(
                candidate.project.layers.at(state.active_layer).uuid);
            if (!pvt::attach_project_file(candidate, reference_id, value,
                                          &attached, &attachment_error)) {
                std::cerr << "Could not embed the custom OBJ: "
                          << attachment_error << '\n';
                return EXIT_FAILURE;
            }
            state.document = std::move(candidate);
            mutate_active([&](RenderConfig& config) {
                const bool changed = !config.surface.enabled
                                     || config.surface.mapping
                                            != pvt::SurfaceMapping::CustomObj
                                     || config.surface.obj_path != attached.local_path
                                     || config.surface.obj_sha256 != attached.sha256
                                     || config.surface.obj_basename != attached.basename
                                     || !config.output.write_alpha;
                config.surface.enabled = true;
                config.surface.mapping = pvt::SurfaceMapping::CustomObj;
                config.surface.obj_path = attached.local_path;
                config.surface.obj_sha256 = attached.sha256;
                config.surface.obj_basename = attached.basename;
                config.output.write_alpha = true;
                return changed;
            });
        } else if (option == "--starting-image"
                   && valid_output_directory(value)) {
            std::string source_error;
            if (!pvt::detail::validate_starting_image_source(
                    value, &source_error)) {
                std::cerr << "Could not decode the starting image: "
                          << source_error << '\n';
                return EXIT_FAILURE;
            }
            ProjectDocument candidate = state.document;
            pvt::ProjectAttachment attached;
            std::string attachment_error;
            const std::string reference_id = pvt::starting_image_attachment_id(
                candidate.project.layers.at(state.active_layer).uuid);
            if (!pvt::attach_project_file(candidate, reference_id, value,
                                          &attached, &attachment_error)) {
                std::cerr << "Could not embed the starting image: "
                          << attachment_error << '\n';
                return EXIT_FAILURE;
            }
            state.document = std::move(candidate);
            mutate_active([&](RenderConfig& config) {
                const bool changed = !config.starting_image.enabled
                                     || config.starting_image.path
                                            != attached.local_path
                                     || config.starting_image.sha256
                                            != attached.sha256
                                     || config.starting_image.basename
                                            != attached.basename
                                     || !config.output.write_alpha;
                config.starting_image.enabled = true;
                config.starting_image.path = attached.local_path;
                config.starting_image.sha256 = attached.sha256;
                config.starting_image.basename = attached.basename;
                config.output.write_alpha = true;
                return changed;
            });
        } else if (option == "--image-fit") {
            pvt::StartingImageFit fit;
            if (!parse_starting_image_fit(value, fit)) {
                std::cerr << "Image fit must be stretch, contain, cover, or tile.\n";
                return EXIT_FAILURE;
            }
            mark_changed(state.document.project.layers.at(state.active_layer)
                             .render.starting_image.fit,
                         fit);
        } else if (option == "--dither") {
            pvt::DitherMethod method;
            if (value == "blue") {
                method = pvt::DitherMethod::BlueNoise;
            } else if (value == "bayer") {
                method = pvt::DitherMethod::OrderedBayer;
            } else if (value == "floyd") {
                method = pvt::DitherMethod::FloydSteinberg;
            } else {
                std::cerr << "Dither method must be blue, bayer, or floyd.\n";
                return EXIT_FAILURE;
            }
            mark_changed(state.document.project.output.dither_enabled, true);
            mark_changed(state.document.project.output.dither_method, method);
        } else if (option == "--output-dir" && valid_output_directory(value)) {
            mark_changed(state.document.project.output.output_directory, value);
        } else if (option == "--prefix" && valid_filename_prefix(value)) {
            mark_changed(state.document.project.output.filename_prefix, value);
        } else if (option == "--start-frame"
                   && parse_integer(value, 0, 1000000000, integer)) {
            mark_changed(state.document.project.output.first_frame_number,
                         static_cast<int>(integer));
        } else if (option == "--digits" && parse_integer(value, 1, 12, integer)) {
            mark_changed(state.document.project.output.filename_digits,
                         static_cast<int>(integer));
        } else {
            std::cerr << "Invalid option or value near '" << option
                      << "'. Use --help for usage.\n";
            return EXIT_FAILURE;
        }
    }

    // Float EXR never crosses an integer quantization boundary, regardless of
    // command-line option order. Keep saved state consistent with that fact.
    if (state.document.project.output.bit_depth == 32) {
        mark_changed(state.document.project.output.dither_enabled, false);
    }

    std::string error;
    if (save_default && !bundle_to_save.empty()) {
        std::cerr << "Use either --save FILE or --save-default, not both.\n";
        return EXIT_FAILURE;
    }
    if ((save_default || !bundle_to_save.empty()) && !legacy_to_save.empty()) {
        std::cerr << "Use either a normal bundle save or --save-legacy, not both.\n";
        return EXIT_FAILURE;
    }
    if (save_default) {
        bundle_to_save = pvt::portable_project_filename(state.document.project.name);
    }
    if (!bundle_to_save.empty()) {
        if (has_case_insensitive_suffix(bundle_to_save, ".pvt")) {
            std::cerr << "Normal saves produce bundles. Use a .zip path or matching "
                         "bundle directory; --save-legacy is the explicit .pvt escape hatch.\n";
            return EXIT_FAILURE;
        }
        pvt::BundleSaveReport report;
        if (!pvt::save_project_document(state.document, bundle_to_save, &report, &error)) {
            std::cerr << "Could not save project bundle: " << error << '\n';
            return EXIT_FAILURE;
        }
        if (report.validated_only) {
            if (report.compacted_storage) {
                std::cout << "No project changes; validated and compacted shared music analysis.\n";
            } else {
                std::cout << "No changes; validated the complete bundle.\n";
            }
        } else {
            std::cout << "Saved project version " << report.version << " to "
                      << report.path << ".\n";
        }
    }
    if (!legacy_to_save.empty()) {
        if (!has_case_insensitive_suffix(legacy_to_save, ".pvt")) {
            std::cerr << "--save-legacy requires a .pvt destination.\n";
            return EXIT_FAILURE;
        }
        if (state.document.project.layers.size() != 1) {
            std::cerr << "Legacy .pvt export is intentionally limited to one-layer projects; "
                         "use a bundle to preserve every layer.\n";
            return EXIT_FAILURE;
        }
        const RenderConfig legacy = pvt::apply_global_config(
            state.document.project.canvas, state.document.project.output,
            state.document.project.layers.front().render);
        if (!pvt::save_setup(legacy, legacy_to_save, &error)) {
            std::cerr << "Could not save legacy setup: " << error << '\n';
            return EXIT_FAILURE;
        }
        std::cout << "Exported explicit legacy setup to " << legacy_to_save << ".\n";
    }
    if (list_versions) {
        if (state.document.legacy_import || state.document.source_path.empty()) {
            std::cout << "This project has no bundle version history yet.\n";
        } else {
            print_version_list(state.document);
        }
    }
    if (!render_now) {
        if (!bundle_to_save.empty() || !legacy_to_save.empty() || list_versions) {
            return EXIT_SUCCESS;
        }
        if (loaded_document) {
            return interactive_menu(state) ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        std::cerr << "No action selected. Add --render, --save FILE, or use --help.\n";
        return EXIT_FAILURE;
    }

    if (!pvt::render_project_sequence(
            state.document.project, render_options,
            [](int completed, int total) {
                std::cout << '\r' << "Rendered " << completed << '/' << total << std::flush;
                return true;
            },
            nullptr, &error)) {
        std::cerr << "\nRender failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "\nDone.\n";
    return EXIT_SUCCESS;
}
