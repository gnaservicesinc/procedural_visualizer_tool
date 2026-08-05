#include "procedural_visualizer_tool.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

using pvt::EffectType;
using pvt::RenderConfig;

constexpr std::size_t kMaximumNameBytes = 256;
constexpr std::size_t kMaximumPathBytes = 4095;
constexpr std::size_t kMaximumPrefixBytes = 127;

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
            value = static_cast<int>(parsed);
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
            value = true;
            return true;
        }
        if (input == "n" || input == "no" || input == "false" || input == "0") {
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
            value = std::move(input);
            return true;
        }
        std::cout << "Text must contain at most " << maximum
                  << " bytes of printable text.\n";
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
            value = choices[static_cast<std::size_t>(parsed - 1)].first;
            return true;
        }
        std::cout << "Choose one of the listed numbers.\n";
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
            configure_wave(config, config.waves.size() - 1);
            continue;
        }
        if ((input[0] == 'd' || input[0] == 'D') && input.size() > 1) {
            long long selected = 0;
            if (parse_integer(trim(input.substr(1)), 1,
                              static_cast<long long>(config.waves.size()), selected)) {
                config.waves.erase(config.waves.begin() + (selected - 1));
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
        || !prompt_int("Cycles per loop", effect.cycles_per_loop, -1000, 1000)
        || !prompt_real("Starting phase (degrees)", effect.phase_degrees, -36000.0, 36000.0)) {
        return false;
    }
    switch (effect.type) {
        case EffectType::EndlessZoom:
            return prompt_real("Mix/intensity", effect.intensity, 0.0, 100.0)
                   && prompt_real("Zoom strength", effect.magnitude, 0.0, 10.0)
                   && prompt_real("Zoom octave multiplier", effect.frequency, 0.0, 1000.0)
                   && prompt_real("Center X (0-1 is on-canvas)", effect.center_x, -10.0, 10.0)
                   && prompt_real("Center Y (0-1 is on-canvas)", effect.center_y, -10.0, 10.0)
                   && configure_edge_mode(effect.edge_mode);
        case EffectType::Ripple:
            return prompt_real("Mix/intensity", effect.intensity, 0.0, 100.0)
                   && prompt_real("Magnitude (fraction of short edge)", effect.magnitude, 0.0, 10.0)
                   && prompt_real("Spatial frequency", effect.frequency, 0.0, 1000.0)
                   && prompt_real("Distance attenuation", effect.secondary, -100.0, 100.0)
                   && prompt_real("Center X (0-1 is on-canvas)", effect.center_x, -10.0, 10.0)
                   && prompt_real("Center Y (0-1 is on-canvas)", effect.center_y, -10.0, 10.0)
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
                   && prompt_real("Center X (0-1 is on-canvas)", effect.center_x, -10.0, 10.0)
                   && prompt_real("Center Y (0-1 is on-canvas)", effect.center_y, -10.0, 10.0)
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
                      << " [" << pvt::effect_type_name(effect.type) << "]\n";
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
            configure_effect(config, config.effects.size() - 1);
            continue;
        }
        if ((input[0] == 'd' || input[0] == 'D') && input.size() > 1) {
            long long selected = 0;
            if (parse_integer(trim(input.substr(1)), 1,
                              static_cast<long long>(config.effects.size()), selected)) {
                config.effects.erase(config.effects.begin() + (selected - 1));
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
           && prompt_real("Waveform shape", swing.shape, 0.0, 1.0);
}

void configure_rhythm(RenderConfig& config) {
    std::cout << "\n-- Rhythm, color, and visual quantization --\n";
    if (!prompt_real("Phrase warp amount", config.phrase_warp, 0.0, 2.0)
        || !prompt_real("Ghost mix", config.ghost_mix, 0.0, 1.0)
        || !prompt_real("Ghost lag (degrees)", config.ghost_lag_degrees, -360.0, 360.0)
        || !prompt_int("Hue rotations per loop", config.hue_cycles, -100, 100)
        || !prompt_real("Color saturation", config.saturation, 0.0, 1.0)
        || !prompt_bool("Visual quantization enabled", config.quantization.enabled)
        || !prompt_int("Visual quantization levels", config.quantization.levels, 2, 65536)
        || !prompt_real("Visual quantization mix", config.quantization.mix, 0.0, 1.0)
        || !prompt_enum("Visual quantization mode", config.quantization.mode,
                       {{pvt::QuantizationMode::Rgb, "RGB channels"},
                        {pvt::QuantizationMode::Luminance, "Luminance"},
                        {pvt::QuantizationMode::Hue, "Hue"}})) {
        return;
    }

    for (;;) {
        std::cout << "\nSwing modulators (" << config.swings.size() << "):\n";
        for (std::size_t i = 0; i < config.swings.size(); ++i) {
            const auto& swing = config.swings[i];
            std::cout << "  " << (i + 1) << ") " << (swing.enabled ? "on  " : "off ")
                      << swing.name << " | " << pvt::waveform_name(swing.waveform)
                      << " | amount " << swing.amount << '\n';
        }
        std::string input;
        if (!read_line("Number to edit, a to add, d N to delete, m FROM TO to move, or b [b]: ", input)
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
            configure_swing(config, config.swings.size() - 1);
            continue;
        }
        if ((input[0] == 'd' || input[0] == 'D') && input.size() > 1) {
            long long selected = 0;
            if (parse_integer(trim(input.substr(1)), 1,
                              static_cast<long long>(config.swings.size()), selected)) {
                config.swings.erase(config.swings.begin() + (selected - 1));
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
    if (config.surface.enabled
        && config.surface.mapping != pvt::SurfaceMapping::Plane
        && config.surface.curvature > 0.0) {
        config.alpha.enabled = true;
        std::cout << "Alpha output enabled for the 3D surface exterior.\n";
    }
}

void configure_alpha(RenderConfig& config) {
    std::cout << "\n-- Alpha channel --\n"
              << "RGB remains present even where alpha is zero (straight/unassociated alpha).\n";
    prompt_bool("Use alpha channel", config.alpha.enabled);
    prompt_real("Minimum alpha", config.alpha.minimum, 0.0, 1.0);
    prompt_real("Maximum alpha", config.alpha.maximum, 0.0, 1.0);
    prompt_real("Alpha spatial frequency", config.alpha.spatial_frequency, 0.0, 1000.0);
    prompt_int("Alpha cycles per loop", config.alpha.cycles_per_loop, -1000, 1000);
    prompt_real("Alpha starting phase (degrees)", config.alpha.phase_degrees,
                -36000.0, 36000.0);
}

void configure_export(RenderConfig& config) {
    std::cout << "\n-- Export --\n";
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

std::string output_extension(const RenderConfig& config) {
    return config.output.bit_depth == 32 ? ".exr" : ".png";
}

void print_summary(const RenderConfig& config) {
    const auto validation = pvt::validate(config);
    std::cout << "\n============================================================\n"
              << " Procedural Visualizer Tool\n"
              << "============================================================\n"
              << "Canvas: " << config.width << 'x' << config.height << " | block "
              << config.block_size << " | " << config.total_frames << " frames at "
              << config.fps << " fps\n"
              << "Stack: " << config.waves.size() << " wave(s), "
              << config.swings.size() << " swing(s), " << config.effects.size()
              << " effect(s)\n"
              << "Output: " << config.output.bit_depth
              << (config.output.bit_depth == 32 ? "-bit float " : "-bit ")
              << (config.alpha.enabled ? "RGBA" : "RGB") << output_extension(config)
              << " | " << (config.output.dither_enabled && config.output.bit_depth != 32
                                 ? pvt::dither_method_name(config.output.dither_method)
                                 : "dither off")
              << (config.output.bit_depth == 32
                      ? ""
                      : " | PNG compression "
                            + std::to_string(config.output.png_compression_level))
              << "\nPeak working-memory estimate: " << std::fixed << std::setprecision(1)
              << static_cast<double>(validation.estimated_peak_bytes) / (1024.0 * 1024.0)
              << " MiB\n";
    if (!validation.ok) {
        std::cout << "Configuration needs attention: " << validation.message << "\n";
    }
    std::cout << "\n1) Canvas and timing\n"
              << "2) Waves: add/remove/move/configure\n"
              << "3) Effects: add/remove/move/configure\n"
              << "4) Surface and procedural feature toggles\n"
              << "5) Rhythm, swings, color, and visual quantization\n"
              << "6) Alpha channel\n"
              << "7) Export format, PNG compression, dithering, and filenames\n"
              << "8) Save setup\n"
              << "9) Load setup\n"
              << "10) Restore defaults\n"
              << "11) Render sequence (press Enter)\n"
              << "0) Quit\n";
}

bool interactive_menu(RenderConfig& config) {
    for (;;) {
        print_summary(config);
        std::string input;
        if (!read_line("Choice [11]: ", input)) {
            return true;
        }
        if (input.empty()) {
            input = "11";
        }
        long long choice = 0;
        if (!parse_integer(input, 0, 11, choice)) {
            std::cout << "Choose a menu number from 0 through 11.\n";
            continue;
        }
        switch (choice) {
            case 0:
                return true;
            case 1:
                configure_canvas(config);
                break;
            case 2:
                configure_waves(config);
                break;
            case 3:
                configure_effects(config);
                break;
            case 4:
                configure_surface(config);
                break;
            case 5:
                configure_rhythm(config);
                break;
            case 6:
                configure_alpha(config);
                break;
            case 7:
                configure_export(config);
                break;
            case 8: {
                std::string path = "setup.pvt";
                std::string error;
                if (!prompt_text("Setup file", path, kMaximumPathBytes)) {
                    return true;
                }
                if (pvt::save_setup(config, path, &error)) {
                    std::cout << "Saved setup to " << path << ".\n";
                } else {
                    std::cout << "Could not save setup: " << error << '\n';
                }
                break;
            }
            case 9: {
                std::string path = "setup.pvt";
                std::string error;
                if (!prompt_text("Setup file", path, kMaximumPathBytes)) {
                    return true;
                }
                if (pvt::load_setup(path, config, &error)) {
                    std::cout << "Loaded setup from " << path << ".\n";
                } else {
                    std::cout << "Could not load setup; current settings are unchanged: "
                              << error << '\n';
                }
                break;
            }
            case 10:
                config = pvt::default_config();
                std::cout << "Restored defaults.\n";
                break;
            case 11: {
                std::string error;
                if (pvt::render_sequence(
                        config,
                        [](int completed, int total) {
                            std::cout << '\r' << "Rendered " << completed << '/' << total
                                      << std::flush;
                            return true;
                        },
                        nullptr, &error)) {
                    std::cout << "\nDone. The sequence loops without a duplicated endpoint.\n";
                    return true;
                }
                std::cout << "\nRender did not complete: " << error << '\n';
                break;
            }
        }
    }
}

void print_help(const char* program) {
    std::cout
        << "Usage:\n"
        << "  " << program << "                         Interactive menu\n"
        << "  " << program << " --render [options]      Render a sequence\n"
        << "  " << program << " --load FILE [--render]  Load a setup transactionally\n"
        << "  " << program << " --self-test             Quick library smoke test (use alone)\n\n"
        << "Options:\n"
        << "  --render (or --defaults)\n"
        << "  --width N --height N --block-size N --frames N --fps N\n"
        << "  --waves N --bit-depth 8|16|32 --png-compression 0..9\n"
        << "  --obj FILE  (enable two-sided custom OBJ wrapping and alpha)\n"
        << "  --alpha --no-alpha\n"
        << "  --dither blue|bayer|floyd --no-dither\n"
        << "  --output-dir PATH --prefix TEXT --start-frame N --digits N\n"
        << "  --overwrite --save FILE --help\n\n"
        << "Options are processed from left to right. Put --load before overrides.\n"
        << "PNG compression defaults to 5 (0 is off, 9 is maximum).\n"
        << "Float EXR output ignores PNG compression and dithering. "
           "Unspecified values keep their defaults.\n";
}

bool option_takes_value(const std::string& option) {
    return option == "--load" || option == "--save" || option == "--width"
           || option == "--height" || option == "--block-size" || option == "--frames"
           || option == "--fps" || option == "--waves" || option == "--bit-depth"
           || option == "--png-compression"
           || option == "--obj"
           || option == "--dither" || option == "--output-dir" || option == "--prefix"
           || option == "--start-frame" || option == "--digits";
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
    RenderConfig config = pvt::default_config();
    config.width = 97;
    config.height = 65;
    config.block_size = 8;
    config.total_frames = 12;
    for (auto& effect : config.effects) {
        effect.enabled = true;
    }
    config.alpha.enabled = true;
    pvt::Image first;
    pvt::Image repeated;
    std::string error;
    if (!pvt::render_frame_at_phase(config, 0.0, first, &error)
        || !pvt::render_frame_at_phase(config, 0.0, repeated, &error)
        || first.pixels != repeated.pixels) {
        std::cerr << "Self-test failed: " << (error.empty() ? "non-deterministic frame" : error)
                  << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "Self-test passed: float RGBA rendering and the full effect stack are deterministic.\n";
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    RenderConfig config = pvt::default_config();
    bool render_now = false;
    bool loaded_setup = false;
    std::string setup_to_save;

    if (argc == 1) {
        return interactive_menu(config) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

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
        if (option == "--overwrite") {
            config.output.overwrite_existing = true;
            continue;
        }
        if (option == "--alpha") {
            config.alpha.enabled = true;
            continue;
        }
        if (option == "--no-alpha") {
            config.alpha.enabled = false;
            continue;
        }
        if (option == "--no-dither") {
            config.output.dither_enabled = false;
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
            std::string error;
            if (!pvt::load_setup(value, config, &error)) {
                std::cerr << "Could not load setup: " << error << '\n';
                return EXIT_FAILURE;
            }
            loaded_setup = true;
        } else if (option == "--save") {
            setup_to_save = value;
        } else if (option == "--width" && parse_integer(value, 16, 16384, integer)) {
            config.width = static_cast<int>(integer);
        } else if (option == "--height" && parse_integer(value, 16, 16384, integer)) {
            config.height = static_cast<int>(integer);
        } else if (option == "--block-size" && parse_integer(value, 1, 16384, integer)) {
            config.block_size = static_cast<int>(integer);
        } else if (option == "--frames" && parse_integer(value, 2, 1000000, integer)) {
            config.total_frames = static_cast<int>(integer);
        } else if (option == "--fps" && parse_real(value, 1.0, 240.0, real)) {
            config.fps = real;
        } else if (option == "--waves" && parse_integer(value, 0,
                                                          pvt::kMaximumWaves, integer)
                   && resize_waves(config, static_cast<std::size_t>(integer))) {
        } else if (option == "--bit-depth" && parse_integer(value, 8, 32, integer)
                   && (integer == 8 || integer == 16 || integer == 32)) {
            config.output.bit_depth = static_cast<int>(integer);
            if (integer == 32) {
                config.output.dither_enabled = false;
            }
        } else if (option == "--png-compression"
                   && parse_integer(value, 0, 9, integer)) {
            config.output.png_compression_level = static_cast<int>(integer);
        } else if (option == "--obj" && valid_output_directory(value)) {
            config.surface.enabled = true;
            config.surface.mapping = pvt::SurfaceMapping::CustomObj;
            config.surface.obj_path = value;
            config.alpha.enabled = true;
        } else if (option == "--dither") {
            config.output.dither_enabled = true;
            if (value == "blue") {
                config.output.dither_method = pvt::DitherMethod::BlueNoise;
            } else if (value == "bayer") {
                config.output.dither_method = pvt::DitherMethod::OrderedBayer;
            } else if (value == "floyd") {
                config.output.dither_method = pvt::DitherMethod::FloydSteinberg;
            } else {
                std::cerr << "Dither method must be blue, bayer, or floyd.\n";
                return EXIT_FAILURE;
            }
        } else if (option == "--output-dir" && valid_output_directory(value)) {
            config.output.output_directory = value;
        } else if (option == "--prefix" && valid_filename_prefix(value)) {
            config.output.filename_prefix = value;
        } else if (option == "--start-frame"
                   && parse_integer(value, 0, 1000000000, integer)) {
            config.output.first_frame_number = static_cast<int>(integer);
        } else if (option == "--digits" && parse_integer(value, 1, 12, integer)) {
            config.output.filename_digits = static_cast<int>(integer);
        } else {
            std::cerr << "Invalid option or value near '" << option
                      << "'. Use --help for usage.\n";
            return EXIT_FAILURE;
        }
    }

    // Float EXR never crosses an integer quantization boundary, regardless of
    // command-line option order. Keep saved state consistent with that fact.
    if (config.output.bit_depth == 32) {
        config.output.dither_enabled = false;
    }

    std::string error;
    if (!setup_to_save.empty() && !pvt::save_setup(config, setup_to_save, &error)) {
        std::cerr << "Could not save setup: " << error << '\n';
        return EXIT_FAILURE;
    }
    if (!render_now) {
        if (!setup_to_save.empty()) {
            return EXIT_SUCCESS;
        }
        if (loaded_setup) {
            return interactive_menu(config) ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        std::cerr << "No action selected. Add --render, --save FILE, or use --help.\n";
        return EXIT_FAILURE;
    }

    if (!pvt::render_sequence(
            config,
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
