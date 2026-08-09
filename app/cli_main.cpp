#include "procedural_visualizer_tool.h"
#include "project_bundle.h"

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
    return prompt_text("Name", wave.name, kMaximumNameBytes)
           && prompt_bool("Enabled", wave.enabled)
           && prompt_bool("Synchronized (optional)", wave.synchronized)
           && prompt_real("Horizontal source location (%)", wave.x_percent, -100.0, 200.0)
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
                      << " | amp " << wave.amplitude << " | dir " << wave.direction << '\n';
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
                 {EffectType::BlockScale, "Block scale"}});
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
        || !prompt_bool("Synchronized (optional)", effect.synchronized)
        || !prompt_enum("Effect space (texture is before surface mapping)",
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
                                effect.magnitude, 0.001, 10.0)
                || !prompt_real("Maximum block-size multiplier",
                                effect.frequency, 0.001, 1000.0)) {
                return false;
            }
            int steps = static_cast<int>(std::llround(effect.secondary));
            if (!prompt_int("Quantization steps (0 is smooth)", steps, 0, 100)) {
                return false;
            }
            effect.secondary = static_cast<double>(steps);
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

void configure_rhythm(RenderConfig& config) {
    std::cout << "\n-- Rhythm, starting colors, and post-effects quantization --\n";
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

    for (;;) {
        std::cout << "\nStarting palette: "
                  << (config.palette.enabled ? config.palette.name : "disabled")
                  << " (" << config.palette.colors.size() << " color(s))\n"
                  << "Swing modulators (" << config.swings.size() << "):\n";
        for (std::size_t i = 0; i < config.swings.size(); ++i) {
            const auto& swing = config.swings[i];
            std::cout << "  " << (i + 1) << ") " << (swing.enabled ? "on  " : "off ")
                      << swing.name << " | " << pvt::waveform_name(swing.waveform)
                      << " | amount " << swing.amount
                      << " | radius " << swing.radius << '\n';
        }
        std::string input;
        if (!read_line("Number to edit, p palette, a to add, d N to delete, "
                       "m FROM TO to move, or b [b]: ", input)
            || input.empty() || input == "b" || input == "B") {
            return;
        }
        if (input == "p" || input == "P") {
            if (!configure_palette(config)) {
                return;
            }
            continue;
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

void configure_canvas(RenderConfig& config) {
    std::cout << "\n-- Canvas and timing --\n";
    prompt_int("Width", config.width, 16, 16384);
    prompt_int("Height", config.height, 16, 16384);
    prompt_int("Block size", config.block_size, 1, 16384);
    prompt_real("Playback FPS", config.fps, 1.0, 240.0);
    prompt_int("Frames per loop", config.total_frames, 2, 1000000);
}

void configure_surface(RenderConfig& config) {
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
        if (!prompt_text("OBJ file path", config.surface.obj_path,
                         kMaximumPathBytes)) {
            return;
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

bool parse_numeric_version(const std::string& text,
                           std::vector<std::uint64_t>& components) {
    if (text.empty() || text.size() > 64U) {
        return false;
    }
    std::vector<std::uint64_t> parsed;
    std::size_t cursor = 0U;
    while (cursor < text.size() && parsed.size() < 8U) {
        if (!std::isdigit(static_cast<unsigned char>(text[cursor]))) {
            return false;
        }
        std::uint64_t value = 0U;
        while (cursor < text.size()
               && std::isdigit(static_cast<unsigned char>(text[cursor]))) {
            const unsigned digit = static_cast<unsigned>(text[cursor] - '0');
            if (value > (1000000000ULL - digit) / 10ULL) {
                return false;
            }
            value = value * 10ULL + digit;
            ++cursor;
        }
        parsed.push_back(value);
        if (cursor == text.size() || text[cursor] == '-' || text[cursor] == '+') {
            break;
        }
        if (text[cursor] != '.') {
            return false;
        }
        ++cursor;
        if (cursor == text.size()) {
            return false;
        }
    }
    if (parsed.empty() || parsed.size() > 8U
        || (cursor < text.size() && text[cursor] != '-' && text[cursor] != '+')) {
        return false;
    }
    components = std::move(parsed);
    return true;
}

bool version_is_newer(const std::string& candidate, const std::string& current) {
    std::vector<std::uint64_t> candidate_parts;
    std::vector<std::uint64_t> current_parts;
    if (!parse_numeric_version(candidate, candidate_parts)
        || !parse_numeric_version(current, current_parts)) {
        return false;
    }
    const std::size_t count = std::max(candidate_parts.size(), current_parts.size());
    candidate_parts.resize(count, 0U);
    current_parts.resize(count, 0U);
    return std::lexicographical_compare(current_parts.begin(), current_parts.end(),
                                        candidate_parts.begin(), candidate_parts.end());
}

void warn_if_created_by_newer_version(const ProjectDocument& document) {
#ifdef PVT_PROGRAM_VERSION
    const std::string current = PVT_PROGRAM_VERSION;
#else
    const std::string current = "4.0.1";
#endif
    const bool newer_created = version_is_newer(document.created_with_version, current);
    const bool newer_changed = version_is_newer(document.last_changed_with_version, current);
    if (document.newer_program_version || newer_created || newer_changed) {
        std::cerr << "Warning: this project records creating version "
                  << document.created_with_version << " and last-changing version "
                  << document.last_changed_with_version << ", newer than this "
                  << current << " build. Loading did not modify it; review it before saving.\n";
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
                        {pvt::BlendMode::Add, "Add"}});
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
        std::cout << "No changes; validated the complete bundle.\n";
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
    std::cout << "\n============================================================\n"
              << " " << project.name << " — Procedural Visualizer Tool\n"
              << "============================================================\n"
              << "Canvas: " << project.canvas.width << 'x' << project.canvas.height
              << " | block " << project.canvas.block_size << " | "
              << project.canvas.total_frames << " frames at " << project.canvas.fps
              << " fps\n"
              << "Layers: " << project.layers.size() << " | editing " << layer.name
              << " (" << (layer.enabled ? "on" : "off") << ", "
              << pvt::blend_mode_name(layer.blend_mode) << ", opacity "
              << layer.opacity << ")\n"
              << "Active stack: " << layer.render.waves.size() << " wave(s), "
              << layer.render.swings.size() << " swing(s), "
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
    }
    std::cout << "\n1) Project name and layers\n"
              << "2) Canvas and timing (global)\n"
              << "3) Waves for active layer\n"
              << "4) Effects for active layer\n"
              << "5) Surface, transforms, and procedural features for active layer\n"
              << "6) Rhythm, swings, palette, color, and quantization for active layer\n"
              << "7) Procedural alpha modulation for active layer\n"
              << "8) Export settings (global)\n"
              << "9) Save project bundle\n"
              << "10) Open bundle, directory, or legacy .pvt\n"
              << "11) Version history\n"
              << "12) Restore new-project defaults\n"
              << "13) Render composite sequence (press Enter)\n"
              << "0) Quit\n";
}

bool interactive_menu(CliState& state) {
    for (;;) {
        print_summary(state);
        std::string input;
        if (!read_line("Choice [13]: ", input)) {
            if (state.document.dirty) {
                std::cerr << "Input ended; the unsaved project was not written.\n";
            }
            return true;
        }
        if (input.empty()) {
            input = "13";
        }
        long long choice = 0;
        if (!parse_integer(input, 0, 13, choice)) {
            std::cout << "Choose a menu number from 0 through 13.\n";
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
        if (choice >= 2 && choice <= 8) {
            RenderConfig config = active_render_config(state);
            g_prompt_changed = false;
            switch (choice) {
                case 2: configure_canvas(config); break;
                case 3: configure_waves(config); break;
                case 4: configure_effects(config); break;
                case 5: configure_surface(config); break;
                case 6: configure_rhythm(config); break;
                case 7: configure_alpha(config); break;
                case 8: configure_export(config); break;
                default: break;
            }
            commit_active_render(state, config, g_prompt_changed);
            continue;
        }
        switch (choice) {
            case 9:
                if (!save_project_interactive(state)) {
                    return true;
                }
                break;
            case 10: {
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
                    warn_if_created_by_newer_version(state.document);
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
            case 11:
                manage_versions(state);
                break;
            case 12:
                if (!resolve_unsaved_changes(state, "start a new project")) {
                    break;
                }
                state.document = pvt::default_project_document();
                state.active_layer = 0;
                std::cout << "Started a new project with default fire settings.\n";
                break;
            case 13: {
                std::string error;
                if (pvt::render_project_sequence(
                        state.document.project,
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
        << "Usage:\n"
        << "  " << program << "                         Interactive menu\n"
        << "  " << program << " --render [options]      Render a composite sequence\n"
        << "  " << program << " --load FILE [--render]  Open a bundle/directory/.pvt\n"
        << "  " << program << " --self-test             Quick library smoke test (use alone)\n\n"
        << "Project and layer options:\n"
        << "  --project-name TEXT --layer N (1 is bottom) --add-layer NAME\n"
        << "  --blend none|softlight|grain-merge|overlay|color-dodge|linear-burn|\n"
        << "          burn|difference|subtract|multiply|add\n"
        << "  --layer-opacity N --enable-layer --disable-layer\n"
        << "  --alpha --no-alpha                 Final RGB/RGBA channel selection\n"
        << "  --alpha-modulation --no-alpha-modulation  Active-layer artwork\n\n"
        << "Render and output options:\n"
        << "  --render (or --defaults)\n"
        << "  --width N --height N --block-size N --frames N --fps N\n"
        << "  --waves N --bit-depth 8|16|32 --png-compression 0..9\n"
        << "  --workers 0.." << pvt::kMaximumSequenceWorkers
        << "  (0 auto, 1 sequential)\n"
        << "  --obj FILE  (enable two-sided custom OBJ wrapping and final alpha)\n"
        << "  --dither blue|bayer|floyd --no-dither\n"
        << "  --output-dir PATH --prefix TEXT --start-frame N --digits N\n"
        << "  --overwrite\n\n"
        << "Persistence options:\n"
        << "  --load FILE                         ZIP, unpacked bundle, or legacy .pvt\n"
        << "  --save FILE                         Save a versioned bundle\n"
        << "  --save-default                      Save to <portable project name>.zip\n"
        << "  --save-legacy FILE                  Explicit one-layer .pvt export\n"
        << "  --list-versions --help\n\n"
        << "Options are processed from left to right. Put --load before overrides.\n"
        << "Normal saves never overwrite an imported legacy .pvt. The explicit\n"
        << "--save-legacy escape hatch is rejected for multi-layer projects.\n"
        << "PNG compression defaults to 5 (0 is off, 9 is maximum).\n"
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
           || option == "--png-compression" || option == "--workers"
           || option == "--obj"
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
            warn_if_created_by_newer_version(state.document);
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
        } else if (option == "--obj" && valid_output_directory(value)) {
            mutate_active([&](RenderConfig& config) {
                const bool changed = !config.surface.enabled
                                     || config.surface.mapping
                                            != pvt::SurfaceMapping::CustomObj
                                     || config.surface.obj_path != value
                                     || !config.output.write_alpha;
                config.surface.enabled = true;
                config.surface.mapping = pvt::SurfaceMapping::CustomObj;
                config.surface.obj_path = value;
                config.output.write_alpha = true;
                return changed;
            });
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
            std::cout << "No changes; validated the complete bundle.\n";
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
