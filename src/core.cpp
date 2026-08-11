#include "procedural_visualizer_tool.h"

#include "frame_renderer_internal.h"
#include "obj_surface.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace pvt {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kTau = 2.0 * kPi;
constexpr int kMaximumDimension = 16384;
constexpr int kMaximumFrames = 1000000;
constexpr std::size_t kMaximumPeakBytes = std::size_t{1} << 30;
constexpr std::size_t kMaximumNameBytes = 256;
constexpr std::size_t kMaximumPathBytes = 4096;
constexpr std::size_t kMaximumPrefixBytes = 128;
constexpr std::size_t kMaximumMeterExpressionBytes = 256;
constexpr std::size_t kMaximumMeterMeasures = 32;
constexpr std::size_t kMaximumMeterGroups = 256;
constexpr int kMaximumMeterValue = 1024;
constexpr double kMaximumMusicDurationSeconds = 1000000.0;

struct Color {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 0.0;
};

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

double clamp_value(double value, double minimum, double maximum) {
    return std::max(minimum, std::min(maximum, value));
}

double mix_value(double first, double second, double amount) {
    return first + (second - first) * amount;
}

double smoothstep(double value) {
    value = clamp_value(value, 0.0, 1.0);
    return value * value * (3.0 - 2.0 * value);
}

double radians(double degrees) {
    return degrees * kPi / 180.0;
}

double wrap_unit(double value) {
    value = std::fmod(value, 1.0);
    return value < 0.0 ? value + 1.0 : value;
}

bool checked_multiply(std::size_t left, std::size_t right, std::size_t& result) {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

ValidationResult invalid_result(const std::string& message,
                                std::size_t estimated_peak_bytes = 0) {
    ValidationResult result;
    result.ok = false;
    result.message = message;
    result.estimated_peak_bytes = estimated_peak_bytes;
    return result;
}

bool finite_in_range(double value, double minimum, double maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool valid_name(const std::string& value) {
    if (value.size() > kMaximumNameBytes) {
        return false;
    }
    for (char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (character == 0 || character == 0x7f
            || (character < 0x20 && character != '\t')) {
            return false;
        }
    }
    return true;
}

bool valid_path_text(const std::string& value, std::size_t maximum_size, bool prefix) {
    if (value.empty() || value.size() >= maximum_size) {
        return false;
    }
    for (char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (character == 0 || character < 0x20 || character == 0x7f) {
            return false;
        }
        if (prefix) {
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
                    return false;
                default:
                    break;
            }
        }
    }
    return true;
}

bool valid_enum(EdgeMode value) {
    switch (value) {
        case EdgeMode::Alpha:
        case EdgeMode::Black:
        case EdgeMode::White:
        case EdgeMode::Reflect:
            return true;
    }
    return false;
}

bool valid_enum(EffectType value) {
    switch (value) {
        case EffectType::EndlessZoom:
        case EffectType::Ripple:
        case EffectType::Shake:
        case EffectType::FlagWave:
        case EffectType::Glow:
        case EffectType::BlockScale:
            return true;
    }
    return false;
}

bool valid_enum(EffectSpace value) {
    switch (value) {
        case EffectSpace::Texture:
        case EffectSpace::Surface:
            return true;
    }
    return false;
}

bool valid_enum(DitherMethod value) {
    switch (value) {
        case DitherMethod::BlueNoise:
        case DitherMethod::OrderedBayer:
        case DitherMethod::FloydSteinberg:
            return true;
    }
    return false;
}

bool valid_enum(SurfaceMapping value) {
    switch (value) {
        case SurfaceMapping::Plane:
        case SurfaceMapping::Cylinder:
        case SurfaceMapping::Sphere:
        case SurfaceMapping::Cube:
        case SurfaceMapping::CustomObj:
            return true;
    }
    return false;
}

bool valid_enum(Waveform value) {
    switch (value) {
        case Waveform::Sine:
        case Waveform::Triangle:
        case Waveform::SmoothPulse:
        case Waveform::Bounce:
            return true;
    }
    return false;
}

bool valid_enum(QuantizationMode value) {
    switch (value) {
        case QuantizationMode::Rgb:
        case QuantizationMode::Luminance:
        case QuantizationMode::Hue:
            return true;
    }
    return false;
}

bool valid_enum(MirrorMode value) {
    switch (value) {
        case MirrorMode::None:
        case MirrorMode::LeftToRight:
        case MirrorMode::RightToLeft:
        case MirrorMode::TopToBottom:
        case MirrorMode::BottomToTop:
        case MirrorMode::FourWay:
            return true;
    }
    return false;
}

bool valid_enum(ClockMode value) {
    switch (value) {
        case ClockMode::Default:
        case ClockMode::Frame:
        case ClockMode::Time:
        case ClockMode::Meter:
        case ClockMode::Music:
            return true;
    }
    return false;
}

bool valid_enum(ClockInterpolation value) {
    switch (value) {
        case ClockInterpolation::Hold:
        case ClockInterpolation::Linear:
        case ClockInterpolation::Smoothstep:
            return true;
    }
    return false;
}

bool valid_enum(ClockFit value) {
    switch (value) {
        case ClockFit::Exact:
        case ClockFit::FitSequence:
            return true;
    }
    return false;
}

bool valid_enum(MusicTempoMode value) {
    switch (value) {
        case MusicTempoMode::Half:
        case MusicTempoMode::Detected:
        case MusicTempoMode::Double:
            return true;
    }
    return false;
}

bool valid_enum(MusicFeature value) {
    switch (value) {
        case MusicFeature::Energy:
        case MusicFeature::Bass:
        case MusicFeature::Midrange:
        case MusicFeature::Treble:
        case MusicFeature::Onset:
        case MusicFeature::Beat:
        case MusicFeature::SpectralCentroid:
        case MusicFeature::SpectralFlatness:
        case MusicFeature::ChromaHue:
        case MusicFeature::ChromaStrength:
            return true;
    }
    return false;
}

bool valid_enum(MusicSwingPolicy value) {
    switch (value) {
        case MusicSwingPolicy::SuppressAll:
        case MusicSwingPolicy::SuppressGlobal:
        case MusicSwingPolicy::KeepAll:
            return true;
    }
    return false;
}

struct MeterGroup {
    int numerator = 1;
    int denominator = 4;
};

struct ParsedMeter {
    std::vector<std::vector<MeterGroup>> measures;
    std::vector<MeterGroup> pulse_pattern;
    std::string canonical;
};

bool parse_meter_integer(std::string_view text, std::size_t& position,
                         int& value, std::string& message) {
    while (position < text.size()
           && std::isspace(static_cast<unsigned char>(text[position])) != 0) {
        ++position;
    }
    const std::size_t first = position;
    unsigned int parsed = 0U;
    while (position < text.size()
           && text[position] >= '0' && text[position] <= '9') {
        const unsigned int digit =
            static_cast<unsigned int>(text[position] - '0');
        if (parsed > (static_cast<unsigned int>(kMaximumMeterValue) - digit)
                         / 10U) {
            message = "Meter values must be between 1 and 1024.";
            return false;
        }
        parsed = parsed * 10U + digit;
        ++position;
    }
    if (position == first || parsed == 0U) {
        message = "Each meter numerator group and denominator must be a positive integer.";
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

void skip_meter_space(std::string_view text, std::size_t& position) {
    while (position < text.size()
           && std::isspace(static_cast<unsigned char>(text[position])) != 0) {
        ++position;
    }
}

bool parse_meter_expression(std::string_view expression, ParsedMeter& parsed,
                            std::string& message) {
    parsed = ParsedMeter{};
    if (expression.empty() || expression.size() > kMaximumMeterExpressionBytes) {
        message = "Meter expression must contain 1 to 256 bytes.";
        return false;
    }

    std::size_t position = 0U;
    std::ostringstream canonical;
    while (true) {
        if (parsed.measures.size() >= kMaximumMeterMeasures) {
            message = "Meter expression contains more than 32 mixed measures.";
            return false;
        }
        skip_meter_space(expression, position);
        std::vector<int> numerators;
        int numerator = 0;
        if (!parse_meter_integer(expression, position, numerator, message)) {
            return false;
        }
        numerators.push_back(numerator);
        skip_meter_space(expression, position);
        while (position < expression.size() && expression[position] == '+') {
            ++position;
            if (numerators.size() >= kMaximumMeterGroups) {
                message = "Meter expression contains too many additive groups.";
                return false;
            }
            if (!parse_meter_integer(expression, position, numerator, message)) {
                return false;
            }
            numerators.push_back(numerator);
            skip_meter_space(expression, position);
        }
        if (position >= expression.size() || expression[position] != '/') {
            message = "Each meter measure must use numerator/denominator syntax.";
            return false;
        }
        ++position;
        int denominator = 0;
        if (!parse_meter_integer(expression, position, denominator, message)) {
            return false;
        }
        skip_meter_space(expression, position);

        std::vector<MeterGroup> measure;
        if (numerators.size() == 1U) {
            // A plain 7/8 describes seven denominator-note pulses. Additive
            // spelling such as 3+2+2/8 deliberately preserves larger groups.
            if (static_cast<std::size_t>(numerators.front())
                    > kMaximumMeterGroups - parsed.pulse_pattern.size()) {
                message = "Meter expression expands to more than 256 pulses.";
                return false;
            }
            measure.reserve(static_cast<std::size_t>(numerators.front()));
            for (int index = 0; index < numerators.front(); ++index) {
                measure.push_back({1, denominator});
            }
        } else {
            if (numerators.size()
                > kMaximumMeterGroups - parsed.pulse_pattern.size()) {
                message = "Meter expression expands to more than 256 pulses.";
                return false;
            }
            measure.reserve(numerators.size());
            for (const int group : numerators) {
                measure.push_back({group, denominator});
            }
        }
        parsed.pulse_pattern.insert(parsed.pulse_pattern.end(),
                                    measure.begin(), measure.end());
        parsed.measures.push_back(std::move(measure));

        if (parsed.measures.size() > 1U) {
            canonical << " | ";
        }
        for (std::size_t index = 0U; index < numerators.size(); ++index) {
            if (index != 0U) canonical << '+';
            canonical << numerators[index];
        }
        canonical << '/' << denominator;

        if (position == expression.size()) {
            break;
        }
        if (expression[position] != '|') {
            message = "Mixed meter measures must be separated with '|'.";
            return false;
        }
        ++position;
        skip_meter_space(expression, position);
        if (position == expression.size()) {
            message = "Meter expression cannot end with a mixed-measure separator.";
            return false;
        }
    }
    parsed.canonical = canonical.str();
    return true;
}

bool valid_lower_hex_digest(const std::string& value) {
    if (value.size() != 64U) return false;
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9')
               || (character >= 'a' && character <= 'f');
    });
}

bool valid_music_basename(const std::string& value) {
    return valid_path_text(value, kMaximumPathBytes, false)
           && value.find('/') == std::string::npos
           && value.find('\\') == std::string::npos
           && value != "." && value != "..";
}

bool effective_frame_count_impl(int stored_count, double fps,
                                const ClockConfig& clock, int& result,
                                std::string& message) {
    if (stored_count < 2 || stored_count > kMaximumFrames) {
        message = "Frame count must be between 2 and 1000000.";
        return false;
    }
    if (!finite_in_range(fps, 1.0, 240.0)) {
        message = "FPS must be finite and between 1 and 240.";
        return false;
    }
    if (clock.mode != ClockMode::Music) {
        result = stored_count;
        return true;
    }
    if (clock.music.schema_version != 1U
        || clock.music.analyzer_version.empty()
        || !valid_lower_hex_digest(clock.music.source_sha256)
        || !valid_music_basename(clock.music.source_basename)
        || clock.music.source_format.empty()
        || clock.music.source_frame_count == 0U
        || clock.music.source_sample_rate == 0U
        || clock.music.source_channel_count == 0U
        || clock.music.beat_times_seconds.empty()
        || !std::isfinite(clock.music.duration_seconds)) {
        message = "Music clock requires complete cached analysis before rendering.";
        return false;
    }
    long double duration = static_cast<long double>(clock.music.duration_seconds);
    if (clock.music.source_frame_count != 0U
        && clock.music.source_sample_rate != 0U) {
        duration = static_cast<long double>(clock.music.source_frame_count)
                   / static_cast<long double>(clock.music.source_sample_rate);
    }
    if (!(duration > 0.0L)
        || duration > static_cast<long double>(kMaximumMusicDurationSeconds)) {
        message = "Music analysis must contain a positive bounded duration.";
        return false;
    }
    const long double frames =
        std::ceil(duration * static_cast<long double>(fps));
    if (!(frames >= 1.0L)
        || frames > static_cast<long double>(kMaximumFrames)) {
        message = "Music duration and FPS require more than 1000000 frames.";
        return false;
    }
    result = static_cast<int>(frames);
    return true;
}

struct TimelineSample {
    double normalized_phase = 0.0;
    MusicFeatureSample music;
};

double interpolated_position(double position,
                             ClockInterpolation interpolation) {
    const double whole = std::floor(position);
    const double fraction = position - whole;
    switch (interpolation) {
        case ClockInterpolation::Hold:
            return whole;
        case ClockInterpolation::Linear:
            return position;
        case ClockInterpolation::Smoothstep:
            return whole + smoothstep(fraction);
    }
    return position;
}

MusicFeatureSample mix_music_sample(const MusicFeatureSample& first,
                                    const MusicFeatureSample& second,
                                    double amount) {
    const auto blend = [amount](float left, float right) {
        return static_cast<float>(mix_value(static_cast<double>(left),
                                            static_cast<double>(right), amount));
    };
    MusicFeatureSample result;
    result.energy = blend(first.energy, second.energy);
    result.bass = blend(first.bass, second.bass);
    result.midrange = blend(first.midrange, second.midrange);
    result.treble = blend(first.treble, second.treble);
    result.onset = blend(first.onset, second.onset);
    result.beat = blend(first.beat, second.beat);
    result.spectral_centroid = blend(first.spectral_centroid,
                                     second.spectral_centroid);
    result.spectral_flatness = blend(first.spectral_flatness,
                                     second.spectral_flatness);
    // Hue is circular: 0.99 and 0.01 are close, not opposite ends of a ramp.
    double hue_delta = static_cast<double>(second.chroma_hue)
                       - static_cast<double>(first.chroma_hue);
    if (hue_delta > 0.5) {
        hue_delta -= 1.0;
    } else if (hue_delta < -0.5) {
        hue_delta += 1.0;
    }
    double hue = static_cast<double>(first.chroma_hue) + amount * hue_delta;
    hue -= std::floor(hue);
    result.chroma_hue = static_cast<float>(hue);
    result.chroma_strength = blend(first.chroma_strength,
                                   second.chroma_strength);
    return result;
}

MusicFeatureSample music_features_at(const MusicAnalysis& analysis,
                                     double time_seconds) {
    if (analysis.feature_samples.empty()
        || !(analysis.duration_seconds > 0.0)) {
        return {};
    }
    if (analysis.feature_samples.size() == 1U) {
        return analysis.feature_samples.front();
    }
    const double position = clamp_value(
        time_seconds / analysis.duration_seconds, 0.0, 1.0)
        * static_cast<double>(analysis.feature_samples.size() - 1U);
    const std::size_t first = static_cast<std::size_t>(std::floor(position));
    const std::size_t second = std::min(first + 1U,
                                        analysis.feature_samples.size() - 1U);
    return mix_music_sample(analysis.feature_samples[first],
                            analysis.feature_samples[second],
                            position - static_cast<double>(first));
}

double apply_clock_transform(double phase, const ClockConfig& clock) {
    const double direction = clock.reverse ? -phase : phase;
    return wrap_unit(direction + clock.phase_offset_degrees / 360.0);
}

double meter_position_at(const std::vector<double>& pulse_seconds,
                         double cycle_seconds, double time_seconds) {
    if (pulse_seconds.empty() || !(cycle_seconds > 0.0)) return 0.0;
    const double cycles = std::floor(time_seconds / cycle_seconds);
    double within = time_seconds - cycles * cycle_seconds;
    // Floating remainder at a negative exact boundary can equal cycle_seconds.
    if (within >= cycle_seconds) {
        within = 0.0;
    } else if (within < 0.0) {
        within += cycle_seconds;
    }
    double elapsed = 0.0;
    for (std::size_t index = 0U; index < pulse_seconds.size(); ++index) {
        const double next = elapsed + pulse_seconds[index];
        if (within < next || index + 1U == pulse_seconds.size()) {
            const double fraction = clamp_value(
                (within - elapsed) / pulse_seconds[index], 0.0, 1.0);
            return cycles * static_cast<double>(pulse_seconds.size())
                   + static_cast<double>(index) + fraction;
        }
        elapsed = next;
    }
    return (cycles + 1.0) * static_cast<double>(pulse_seconds.size());
}

std::vector<double> music_anchors(const ClockConfig& clock) {
    std::vector<double> selected;
    selected.reserve(clock.music.beat_times_seconds.size() * 2U + 2U);
    selected.push_back(0.0);
    const auto& beats = clock.music.beat_times_seconds;
    if (clock.music_tempo == MusicTempoMode::Half) {
        for (std::size_t index = 0U; index < beats.size(); index += 2U) {
            selected.push_back(beats[index]);
        }
    } else {
        for (std::size_t index = 0U; index < beats.size(); ++index) {
            selected.push_back(beats[index]);
            if (clock.music_tempo == MusicTempoMode::Double
                && index + 1U < beats.size()) {
                selected.push_back(0.5 * (beats[index] + beats[index + 1U]));
            }
        }
    }
    selected.push_back(clock.music.duration_seconds);
    std::sort(selected.begin(), selected.end());
    selected.erase(std::unique(selected.begin(), selected.end(),
                               [](double left, double right) {
                                   return std::fabs(left - right) <= 1.0e-12;
                               }),
                   selected.end());
    return selected;
}

TimelineSample resolve_timeline_sample(const RenderConfig& config,
                                       int frame_index) {
    int frame_count = config.total_frames;
    std::string ignored;
    (void)effective_frame_count_impl(config.total_frames, config.fps,
                                     config.clock, frame_count, ignored);
    int frame = frame_index % frame_count;
    if (frame < 0) frame += frame_count;

    TimelineSample result;
    const double direct_phase = static_cast<double>(frame)
                                / static_cast<double>(frame_count);
    if (config.clock.mode == ClockMode::Default) {
        result.normalized_phase = apply_clock_transform(direct_phase,
                                                        config.clock);
        return result;
    }

    const double duration = static_cast<double>(frame_count) / config.fps;
    const double time_seconds = static_cast<double>(frame) / config.fps;
    double phase = direct_phase;

    if (config.clock.mode == ClockMode::Frame) {
        double interval = static_cast<double>(config.clock.frame_interval);
        double normalization = static_cast<double>(frame_count) / interval;
        if (config.clock.fit == ClockFit::FitSequence) {
            const double pulses = std::max(1.0, std::round(normalization));
            interval = static_cast<double>(frame_count) / pulses;
            normalization = pulses;
        }
        phase = interpolated_position(static_cast<double>(frame) / interval,
                                      config.clock.interpolation)
                / normalization;
    } else if (config.clock.mode == ClockMode::Time) {
        double interval = static_cast<double>(
            config.clock.time_interval_microseconds) / 1000000.0;
        double normalization = duration / interval;
        if (config.clock.fit == ClockFit::FitSequence) {
            const double pulses = std::max(1.0, std::round(normalization));
            interval = duration / pulses;
            normalization = pulses;
        }
        phase = interpolated_position(time_seconds / interval,
                                      config.clock.interpolation)
                / normalization;
    } else if (config.clock.mode == ClockMode::Meter) {
        ParsedMeter meter;
        std::string ignored_meter_error;
        (void)parse_meter_expression(config.clock.meter.expression, meter,
                                     ignored_meter_error);
        std::vector<double> pulse_seconds;
        pulse_seconds.reserve(meter.pulse_pattern.size());
        double cycle_seconds = 0.0;
        for (const MeterGroup& group : meter.pulse_pattern) {
            const double seconds = 60.0 / config.clock.meter.bpm
                * static_cast<double>(group.numerator)
                * static_cast<double>(config.clock.meter.tempo_note_denominator)
                / static_cast<double>(group.denominator);
            pulse_seconds.push_back(seconds);
            cycle_seconds += seconds;
        }
        if (config.clock.fit == ClockFit::FitSequence) {
            const double cycles = std::max(1.0,
                                           std::round(duration / cycle_seconds));
            const double scale = duration / (cycles * cycle_seconds);
            for (double& seconds : pulse_seconds) seconds *= scale;
            cycle_seconds *= scale;
        }
        const double offset = static_cast<double>(
            config.clock.beat_offset_microseconds) / 1000000.0;
        const double start_position = meter_position_at(
            pulse_seconds, cycle_seconds, offset);
        const double current_position = meter_position_at(
            pulse_seconds, cycle_seconds, time_seconds + offset);
        const double end_position = meter_position_at(
            pulse_seconds, cycle_seconds, duration + offset);
        const double denominator = end_position - start_position;
        phase = (interpolated_position(current_position,
                                       config.clock.interpolation)
                 - interpolated_position(start_position,
                                         config.clock.interpolation))
                / denominator;
    } else if (config.clock.mode == ClockMode::Music) {
        const std::vector<double> anchors = music_anchors(config.clock);
        const double offset = static_cast<double>(
            config.clock.beat_offset_microseconds) / 1000000.0;
        const double music_time = clamp_value(time_seconds + offset, 0.0,
                                              config.clock.music.duration_seconds);
        const auto upper = std::upper_bound(anchors.begin(), anchors.end(),
                                            music_time);
        const std::size_t second = upper == anchors.end()
                                       ? anchors.size() - 1U
                                       : static_cast<std::size_t>(upper
                                                                  - anchors.begin());
        const std::size_t first = second == 0U ? 0U : second - 1U;
        const double span = anchors[second] - anchors[first];
        double amount = span > 0.0
                            ? (music_time - anchors[first]) / span
                            : 0.0;
        if (config.clock.interpolation == ClockInterpolation::Hold) {
            amount = 0.0;
        } else if (config.clock.interpolation
                   == ClockInterpolation::Smoothstep) {
            amount = smoothstep(amount);
        }
        phase = (static_cast<double>(first) + amount)
                / static_cast<double>(anchors.size() - 1U);
        // Beat anchors drive only the base motion clock. The independently
        // authored audio-reactive routes consume the dense analysis envelope
        // at the actual frame timestamp, retaining within-beat transients.
        result.music = music_features_at(config.clock.music, music_time);
    }

    result.normalized_phase = apply_clock_transform(phase, config.clock);
    return result;
}

double music_feature_value(const MusicFeatureSample& sample,
                           MusicFeature feature) {
    switch (feature) {
        case MusicFeature::Energy: return sample.energy;
        case MusicFeature::Bass: return sample.bass;
        case MusicFeature::Midrange: return sample.midrange;
        case MusicFeature::Treble: return sample.treble;
        case MusicFeature::Onset: return sample.onset;
        case MusicFeature::Beat: return sample.beat;
        case MusicFeature::SpectralCentroid: return sample.spectral_centroid;
        case MusicFeature::SpectralFlatness: return sample.spectral_flatness;
        // A pitch hue without tonal evidence is arbitrary (especially during
        // silence/noise), so palette routing fades it out by confidence.
        case MusicFeature::ChromaHue:
            return sample.chroma_hue * sample.chroma_strength;
        case MusicFeature::ChromaStrength: return sample.chroma_strength;
    }
    return 0.0;
}

bool effect_has_render_work(const EffectConfig& effect) {
    if (!effect.enabled || effect.intensity <= 0.0) {
        return false;
    }
    switch (effect.type) {
        case EffectType::Glow:
            return effect.radius_pixels > 0.0;
        case EffectType::BlockScale:
            return effect.magnitude > 0.0 && effect.frequency > 0.0;
        case EffectType::EndlessZoom:
        case EffectType::Ripple:
        case EffectType::Shake:
        case EffectType::FlagWave:
            return effect.magnitude > 0.0;
    }
    return false;
}

bool effect_uses_edge_mode(EffectType type) {
    return type != EffectType::Glow && type != EffectType::BlockScale;
}

bool surface_has_render_work(const SurfaceConfig& surface) {
    if (!surface.enabled) {
        return false;
    }
    if (surface.mapping != SurfaceMapping::Plane) {
        return surface.curvature > 0.0;
    }
    const bool identity_rotation = surface.rotations_per_loop == 0
                                   && std::fmod(surface.phase_degrees, 360.0) == 0.0;
    return !identity_rotation;
}

double srgb_to_linear(double value) {
    value = clamp_value(value, 0.0, 1.0);
    return value <= 0.04045
               ? value / 12.92
               : std::pow((value + 0.055) / 1.055, 2.4);
}

Color hsl_to_linear_rgb(double hue, double saturation, double lightness) {
    hue = std::fmod(hue, 360.0);
    if (hue < 0.0) {
        hue += 360.0;
    }
    saturation = clamp_value(saturation, 0.0, 1.0);
    lightness = clamp_value(lightness, 0.0, 1.0);

    const double chroma = (1.0 - std::fabs(2.0 * lightness - 1.0)) * saturation;
    const double hue_prime = hue / 60.0;
    const double x = chroma * (1.0 - std::fabs(std::fmod(hue_prime, 2.0) - 1.0));
    const double match = lightness - chroma / 2.0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;

    if (hue_prime < 1.0) {
        red = chroma;
        green = x;
    } else if (hue_prime < 2.0) {
        red = x;
        green = chroma;
    } else if (hue_prime < 3.0) {
        green = chroma;
        blue = x;
    } else if (hue_prime < 4.0) {
        green = x;
        blue = chroma;
    } else if (hue_prime < 5.0) {
        red = x;
        blue = chroma;
    } else {
        red = chroma;
        blue = x;
    }

    return {srgb_to_linear(red + match),
            srgb_to_linear(green + match),
            srgb_to_linear(blue + match),
            1.0};
}

double linear_to_srgb_for_hue(double value) {
    value = clamp_value(value, 0.0, 1.0);
    return value <= 0.0031308
               ? 12.92 * value
               : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

Color rotate_linear_hue(Color color, double degrees) {
    if (std::abs(degrees) <= 1.0e-12) return color;
    const double red = linear_to_srgb_for_hue(color.r);
    const double green = linear_to_srgb_for_hue(color.g);
    const double blue = linear_to_srgb_for_hue(color.b);
    const double maximum = std::max(red, std::max(green, blue));
    const double minimum = std::min(red, std::min(green, blue));
    const double delta = maximum - minimum;
    if (delta <= 1.0e-12) return color;

    double hue = 0.0;
    if (maximum == red) {
        hue = std::fmod((green - blue) / delta, 6.0);
    } else if (maximum == green) {
        hue = (blue - red) / delta + 2.0;
    } else {
        hue = (red - green) / delta + 4.0;
    }
    hue = wrap_unit(hue / 6.0 + degrees / 360.0);
    const double saturation = maximum > 1.0e-12 ? delta / maximum : 0.0;
    const double chroma = maximum * saturation;
    const double sector = hue * 6.0;
    const double intermediate =
        chroma * (1.0 - std::fabs(std::fmod(sector, 2.0) - 1.0));
    const double match = maximum - chroma;
    double rotated_red = 0.0;
    double rotated_green = 0.0;
    double rotated_blue = 0.0;
    if (sector < 1.0) {
        rotated_red = chroma; rotated_green = intermediate;
    } else if (sector < 2.0) {
        rotated_red = intermediate; rotated_green = chroma;
    } else if (sector < 3.0) {
        rotated_green = chroma; rotated_blue = intermediate;
    } else if (sector < 4.0) {
        rotated_green = intermediate; rotated_blue = chroma;
    } else if (sector < 5.0) {
        rotated_red = intermediate; rotated_blue = chroma;
    } else {
        rotated_red = chroma; rotated_blue = intermediate;
    }
    color.r = srgb_to_linear(rotated_red + match);
    color.g = srgb_to_linear(rotated_green + match);
    color.b = srgb_to_linear(rotated_blue + match);
    return color;
}

double evaluate_waveform(Waveform waveform, double phase, double shape) {
    const double sine = std::sin(phase);
    switch (waveform) {
        case Waveform::Sine:
            return sine;
        case Waveform::Triangle:
            return (2.0 / kPi) * std::asin(clamp_value(sine, -1.0, 1.0));
        case Waveform::SmoothPulse: {
            const double sharpness = 0.25 + 8.0 * clamp_value(shape, 0.0, 1.0);
            return std::tanh(sharpness * sine) / std::tanh(sharpness);
        }
        case Waveform::Bounce:
            return 1.0 - 2.0 * std::fabs(std::sin(phase * 0.5));
    }
    return 0.0;
}

double circular_influence(double center_x, double center_y, double radius,
                          double x, double y, int width, int height) {
    if (radius <= 1.0e-12) {
        return 1.0;
    }
    const double short_side = static_cast<double>(std::min(width, height));
    const double dx = x - center_x * static_cast<double>(width - 1);
    const double dy = y - center_y * static_cast<double>(height - 1);
    const double normalized_distance = std::hypot(dx, dy) / short_side;
    // Keep most of the selected circle at full strength and feather its outer
    // fifth. This avoids a visible hard ring while keeping placement intuitive.
    const double feather_start = radius * 0.8;
    if (normalized_distance <= feather_start) {
        return 1.0;
    }
    if (normalized_distance >= radius) {
        return 0.0;
    }
    return 1.0 - smoothstep((normalized_distance - feather_start)
                            / std::max(1.0e-12, radius - feather_start));
}

struct SpatialSwingSample {
    double center_x = 0.5;
    double center_y = 0.5;
    double radius = 0.0;
    double contribution = 0.0;
};

struct MotionClockState {
    double global_phase = 0.0;
    std::array<SpatialSwingSample, kMaximumSwings> spatial_swings{};
    std::size_t spatial_swing_count = 0U;
};

MotionClockState prepare_motion_clock(const RenderConfig& config,
                                      double loop_phase) {
    MotionClockState state;
    state.global_phase = loop_phase
                         + config.phrase_warp * std::sin(loop_phase);
    if (!config.swings_enabled) {
        return state;
    }
    for (const SwingConfig& swing : config.swings) {
        if (!swing.enabled) {
            continue;
        }
        const double swing_phase =
            static_cast<double>(swing.cycles_per_loop) * loop_phase
            + radians(swing.phase_degrees);
        const double contribution =
            swing.amount
            * evaluate_waveform(swing.waveform, swing_phase, swing.shape);
        if (swing.radius <= 1.0e-12) {
            state.global_phase += contribution;
            continue;
        }
        // Validation guarantees the configured collection fits this bounded
        // array. Keeping it inline avoids a per-frame heap allocation.
        if (state.spatial_swing_count < state.spatial_swings.size()) {
            state.spatial_swings[state.spatial_swing_count++] = {
                swing.center_x, swing.center_y, swing.radius, contribution};
        }
    }
    return state;
}

double motion_phase_at(const MotionClockState& state, double x, double y,
                       int width, int height) {
    double result = state.global_phase;
    for (std::size_t index = 0U; index < state.spatial_swing_count; ++index) {
        const SpatialSwingSample& swing = state.spatial_swings[index];
        result += swing.contribution * circular_influence(
            swing.center_x, swing.center_y, swing.radius, x, y,
            width, height);
    }
    return result;
}

double wave_coordinate(const WaveConfig& wave, double x, double y,
                       const RenderConfig& config) {
    const double short_side = static_cast<double>(std::min(config.width, config.height));
    const double source_x = wave.x_percent * 0.01 * static_cast<double>(config.width);
    const double source_y = wave.y_percent * 0.01 * static_cast<double>(config.height);
    const double dx = (x - source_x) / short_side;
    const double dy = (y - source_y) / short_side;
    const double radial = std::hypot(dx, dy);

    if (wave.direction < 0.5) {
        return mix_value(radial, dx, 1.0 - 2.0 * wave.direction);
    }
    return mix_value(radial, dy, 2.0 * wave.direction - 1.0);
}

double wave_height(const RenderConfig& config, double x, double y,
                   double loop_phase, double motion_phase,
                   const MusicFeatureSample& music) {
    double height = 0.0;
    for (const WaveConfig& wave : config.waves) {
        if (!wave.enabled) {
            continue;
        }
        const double clock = wave.synchronized ? motion_phase : loop_phase;
        const double phase = static_cast<double>(wave.cycles_per_loop) * clock;
        double amplitude = wave.amplitude;
        if (config.audio_reactive.enabled
            && config.audio_reactive.waves_enabled
            && (!config.audio_reactive.synchronized_only
                || wave.synchronized)) {
            amplitude *= std::max(
                0.0, 1.0 + config.audio_reactive.wave_amount
                               * music_feature_value(
                                   music, config.audio_reactive.wave_source));
        }
        height += amplitude
                  * std::sin(kTau * wave.spatial_frequency
                                 * wave_coordinate(wave, x, y, config)
                             - phase + radians(wave.phase_degrees));
    }
    return height;
}

void ensure_image(Image& image, int width, int height) {
    image.width = width;
    image.height = height;
    const std::size_t count = static_cast<std::size_t>(width)
                              * static_cast<std::size_t>(height) * 4U;
    image.pixels.resize(count);
}

float stored_channel(double value) {
    // Valid configurations are bounded so normal rendering never reaches this
    // guard. Keep the final double-to-float conversion defensive as well: a
    // malformed future effect must not leak NaN/Inf into PNG or EXR writers.
    if (std::isnan(value)) {
        return 0.0F;
    }
    constexpr double maximum = static_cast<double>(std::numeric_limits<float>::max());
    return static_cast<float>(clamp_value(value, -maximum, maximum));
}

std::size_t pixel_offset_unchecked(const Image& image, int x, int y) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width)
            + static_cast<std::size_t>(x)) * 4U;
}

void store_color(Image& image, int x, int y, const Color& color) {
    float* pixel = image.pixels.data() + pixel_offset_unchecked(image, x, y);
    pixel[0] = stored_channel(color.r);
    pixel[1] = stored_channel(color.g);
    pixel[2] = stored_channel(color.b);
    pixel[3] = static_cast<float>(clamp_value(color.a, 0.0, 1.0));
}

Color load_color(const Image& image, int x, int y) {
    const float* pixel = image.pixels.data() + pixel_offset_unchecked(image, x, y);
    return {pixel[0], pixel[1], pixel[2], pixel[3]};
}

int reflected_index(long long index, int size) {
    if (size <= 1) {
        return 0;
    }
    const long long period = 2LL * static_cast<long long>(size - 1);
    index %= period;
    if (index < 0) {
        index += period;
    }
    if (index >= size) {
        index = period - index;
    }
    return static_cast<int>(index);
}

Color edge_color(EdgeMode mode) {
    switch (mode) {
        case EdgeMode::Alpha:
            return {0.0, 0.0, 0.0, 0.0};
        case EdgeMode::Black:
            return {0.0, 0.0, 0.0, 1.0};
        case EdgeMode::White:
            return {1.0, 1.0, 1.0, 1.0};
        case EdgeMode::Reflect:
            break;
    }
    return {0.0, 0.0, 0.0, 0.0};
}

Color sample_texel(const Image& image, long long x, long long y, EdgeMode mode) {
    if (x >= 0 && x < image.width && y >= 0 && y < image.height) {
        return load_color(image, static_cast<int>(x), static_cast<int>(y));
    }
    if (mode != EdgeMode::Reflect) {
        return edge_color(mode);
    }
    return load_color(image, reflected_index(x, image.width),
                      reflected_index(y, image.height));
}

Color sample_bilinear(const Image& image, double x, double y, EdgeMode mode) {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return edge_color(mode);
    }
    const double bounded_x = clamp_value(
        x, static_cast<double>(std::numeric_limits<long long>::min() / 4),
        static_cast<double>(std::numeric_limits<long long>::max() / 4));
    const double bounded_y = clamp_value(
        y, static_cast<double>(std::numeric_limits<long long>::min() / 4),
        static_cast<double>(std::numeric_limits<long long>::max() / 4));
    const long long x0 = static_cast<long long>(std::floor(bounded_x));
    const long long y0 = static_cast<long long>(std::floor(bounded_y));
    const double tx = bounded_x - static_cast<double>(x0);
    const double ty = bounded_y - static_cast<double>(y0);
    const std::array<Color, 4> samples = {
        sample_texel(image, x0, y0, mode),
        sample_texel(image, x0 + 1, y0, mode),
        sample_texel(image, x0, y0 + 1, mode),
        sample_texel(image, x0 + 1, y0 + 1, mode)};
    const std::array<double, 4> weights = {
        (1.0 - tx) * (1.0 - ty), tx * (1.0 - ty),
        (1.0 - tx) * ty, tx * ty};

    Color result;
    double rgb_weight = 0.0;
    for (std::size_t index = 0; index < samples.size(); ++index) {
        // RGB is intentionally independent of coverage. This is straight alpha:
        // a fully transparent procedural pixel still retains its useful color
        // for alpha-aware compositors and later transforms.
        const long long sample_x = x0 + static_cast<long long>(index & 1U);
        const long long sample_y = y0 + static_cast<long long>(index >> 1U);
        const bool outside = sample_x < 0 || sample_x >= image.width
                             || sample_y < 0 || sample_y >= image.height;
        // Alpha edge samples represent missing coverage, not black paint. Do
        // not let those synthetic transparent texels darken straight RGB; the
        // alpha interpolation alone supplies the edge fade. Transparent texels
        // that are genuinely inside the image still contribute their RGB.
        if (mode != EdgeMode::Alpha || !outside) {
            result.r += samples[index].r * weights[index];
            result.g += samples[index].g * weights[index];
            result.b += samples[index].b * weights[index];
            rgb_weight += weights[index];
        }
        result.a += samples[index].a * weights[index];
    }
    if (mode == EdgeMode::Alpha && rgb_weight > 1.0e-12) {
        result.r /= rgb_weight;
        result.g /= rgb_weight;
        result.b /= rgb_weight;
    }
    result.a = clamp_value(result.a, 0.0, 1.0);
    return result;
}

Color sample_bilinear_wrapped_x(const Image& image, double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y) || image.width <= 0 || image.height <= 0) {
        return {};
    }
    const double width = static_cast<double>(image.width);
    double wrapped_x = std::fmod(x, width);
    if (wrapped_x < 0.0) {
        wrapped_x += width;
    }
    const long long x0 = static_cast<long long>(std::floor(wrapped_x));
    const long long x1 = (x0 + 1LL) % image.width;
    const double bounded_y = clamp_value(
        y, static_cast<double>(std::numeric_limits<long long>::min() / 4),
        static_cast<double>(std::numeric_limits<long long>::max() / 4));
    const long long y0 = static_cast<long long>(std::floor(bounded_y));
    const double tx = wrapped_x - static_cast<double>(x0);
    const double ty = bounded_y - static_cast<double>(y0);
    const std::array<Color, 4> samples = {
        load_color(image, static_cast<int>(x0), reflected_index(y0, image.height)),
        load_color(image, static_cast<int>(x1), reflected_index(y0, image.height)),
        load_color(image, static_cast<int>(x0), reflected_index(y0 + 1, image.height)),
        load_color(image, static_cast<int>(x1), reflected_index(y0 + 1, image.height))};
    const std::array<double, 4> weights = {
        (1.0 - tx) * (1.0 - ty), tx * (1.0 - ty),
        (1.0 - tx) * ty, tx * ty};
    Color result;
    for (std::size_t index = 0; index < samples.size(); ++index) {
        result.r += samples[index].r * weights[index];
        result.g += samples[index].g * weights[index];
        result.b += samples[index].b * weights[index];
        result.a += samples[index].a * weights[index];
    }
    result.a = clamp_value(result.a, 0.0, 1.0);
    return result;
}

Color blend_straight_alpha(const Color& first, const Color& second, double amount) {
    amount = clamp_value(amount, 0.0, 1.0);
    const double first_weight = 1.0 - amount;
    return {first.r * first_weight + second.r * amount,
            first.g * first_weight + second.g * amount,
            first.b * first_weight + second.b * amount,
            first.a * first_weight + second.a * amount};
}

Color composite_straight_alpha_over(const Color& front, const Color& back) {
    const double front_alpha = clamp_value(front.a, 0.0, 1.0);
    const double back_weight = clamp_value(back.a, 0.0, 1.0)
                               * (1.0 - front_alpha);
    const double output_alpha = front_alpha + back_weight;
    if (output_alpha <= 1.0e-12) {
        // Preserve useful straight RGB even when neither surface contributes
        // coverage, matching the rest of the renderer's transparent-color
        // convention.
        return {front.r, front.g, front.b, 0.0};
    }
    return {(front.r * front_alpha + back.r * back_weight) / output_alpha,
            (front.g * front_alpha + back.g * back_weight) / output_alpha,
            (front.b * front_alpha + back.b * back_weight) / output_alpha,
            output_alpha};
}

bool image_pixel_offset(const Image& image, int x, int y, std::size_t& offset) {
    if (x < 0 || y < 0 || x >= image.width || y >= image.height) {
        return false;
    }
    std::size_t expected_pixels = 0;
    std::size_t expected_components = 0;
    if (!checked_multiply(static_cast<std::size_t>(image.width),
                          static_cast<std::size_t>(image.height), expected_pixels)
        || !checked_multiply(expected_pixels, 4U, expected_components)
        || image.pixels.size() != expected_components) {
        return false;
    }
    std::size_t row_offset = 0;
    std::size_t pixel_offset = 0;
    if (!checked_multiply(static_cast<std::size_t>(y),
                          static_cast<std::size_t>(image.width), row_offset)
        || row_offset > std::numeric_limits<std::size_t>::max()
                             - static_cast<std::size_t>(x)) {
        return false;
    }
    pixel_offset = row_offset + static_cast<std::size_t>(x);
    if (!checked_multiply(pixel_offset, 4U, offset)
        || offset > image.pixels.size()
                        || image.pixels.size() - offset < 4U) {
        return false;
    }
    return true;
}

} // namespace

float* Image::pixel(int x, int y) {
    std::size_t index = 0;
    if (!image_pixel_offset(*this, x, y, index)) {
        return nullptr;
    }
    return pixels.data() + index;
}

const float* Image::pixel(int x, int y) const {
    std::size_t index = 0;
    if (!image_pixel_offset(*this, x, y, index)) {
        return nullptr;
    }
    return pixels.data() + index;
}

int effective_frame_count(const CanvasLoopConfig& canvas,
                          std::string* error) {
    int result = -1;
    std::string message;
    if (!effective_frame_count_impl(canvas.total_frames, canvas.fps,
                                    canvas.clock, result, message)) {
        set_error(error, message);
        return -1;
    }
    set_error(error, std::string{});
    return result;
}

int effective_frame_count(const RenderConfig& config,
                          std::string* error) {
    int result = -1;
    std::string message;
    if (!effective_frame_count_impl(config.total_frames, config.fps,
                                    config.clock, result, message)) {
        set_error(error, message);
        return -1;
    }
    set_error(error, std::string{});
    return result;
}

bool describe_meter(const std::string& expression,
                    std::string& description,
                    std::string* error) {
    ParsedMeter parsed;
    std::string message;
    if (!parse_meter_expression(expression, parsed, message)) {
        set_error(error, message);
        return false;
    }
    std::ostringstream summary;
    summary << parsed.canonical << " (" << parsed.measures.size()
            << (parsed.measures.size() == 1U ? " measure, " : " measures, ")
            << parsed.pulse_pattern.size()
            << (parsed.pulse_pattern.size() == 1U ? " pulse)" : " pulses)");
    description = summary.str();
    set_error(error, std::string{});
    return true;
}

const char* effect_type_name(EffectType value) {
    switch (value) {
        case EffectType::EndlessZoom: return "Endless zoom";
        case EffectType::Ripple: return "Ripple";
        case EffectType::Shake: return "Shake";
        case EffectType::FlagWave: return "Flag wave";
        case EffectType::Glow: return "Glow";
        case EffectType::BlockScale: return "Block scale";
    }
    return "Unknown";
}

const char* effect_space_name(EffectSpace value) {
    switch (value) {
        case EffectSpace::Texture: return "Texture";
        case EffectSpace::Surface: return "Mapped object";
    }
    return "Unknown";
}

const char* edge_mode_name(EdgeMode value) {
    switch (value) {
        case EdgeMode::Alpha: return "Alpha";
        case EdgeMode::Black: return "Black";
        case EdgeMode::White: return "White";
        case EdgeMode::Reflect: return "Reflected pattern";
    }
    return "Unknown";
}

const char* dither_method_name(DitherMethod value) {
    switch (value) {
        case DitherMethod::BlueNoise: return "Blue noise";
        case DitherMethod::OrderedBayer: return "Ordered Bayer";
        case DitherMethod::FloydSteinberg: return "Floyd-Steinberg";
    }
    return "Unknown";
}

const char* surface_mapping_name(SurfaceMapping value) {
    switch (value) {
        case SurfaceMapping::Plane: return "Plane";
        case SurfaceMapping::Cylinder: return "Cylinder";
        case SurfaceMapping::Sphere: return "Sphere";
        case SurfaceMapping::Cube: return "Cube";
        case SurfaceMapping::CustomObj: return "Custom OBJ";
    }
    return "Unknown";
}

const char* waveform_name(Waveform value) {
    switch (value) {
        case Waveform::Sine: return "Sine";
        case Waveform::Triangle: return "Triangle";
        case Waveform::SmoothPulse: return "Smooth pulse";
        case Waveform::Bounce: return "Bounce";
    }
    return "Unknown";
}

const char* quantization_mode_name(QuantizationMode value) {
    switch (value) {
        case QuantizationMode::Rgb: return "RGB";
        case QuantizationMode::Luminance: return "Luminance";
        case QuantizationMode::Hue: return "Hue";
    }
    return "Unknown";
}

const char* mirror_mode_name(MirrorMode value) {
    switch (value) {
        case MirrorMode::None: return "Off";
        case MirrorMode::LeftToRight: return "Left to right";
        case MirrorMode::RightToLeft: return "Right to left";
        case MirrorMode::TopToBottom: return "Top to bottom";
        case MirrorMode::BottomToTop: return "Bottom to top";
        case MirrorMode::FourWay: return "Four-way from top left";
    }
    return "Unknown";
}

WaveConfig default_wave(std::size_t index) {
    WaveConfig wave;
    wave.name = "Wave " + std::to_string(index + 1U);
    if (index == 0U) {
        wave.x_percent = 50.0000;
        wave.y_percent = 50.0000;
        wave.amplitude = 0.55;
        wave.spatial_frequency = 3.7815;
        wave.phase_degrees = 25.7831;
    } else if (index == 1U) {
        wave.x_percent = 29.1667;
        wave.y_percent = 26.8519;
        wave.amplitude = 0.25;
        wave.spatial_frequency = 9.4538;
        wave.phase_degrees = 25.7831;
    } else if (index == 2U) {
        wave.x_percent = 70.8333;
        wave.y_percent = 73.1481;
        wave.amplitude = 0.20;
        wave.spatial_frequency = 12.0321;
        wave.phase_degrees = 25.7831;
    } else {
        const double angle = std::fmod(static_cast<double>(index) * 137.50776405003785,
                                      360.0);
        wave.x_percent = 50.0 + 32.0 * std::cos(radians(angle));
        wave.y_percent = 50.0 + 32.0 * std::sin(radians(angle));
        wave.amplitude = 0.20;
        wave.spatial_frequency = 4.0 + static_cast<double>(index % 9U);
        wave.phase_degrees = angle;
    }
    return wave;
}

SwingConfig default_swing(std::size_t index) {
    SwingConfig swing;
    swing.name = "Swing " + std::to_string(index + 1U);
    if (index == 0U) {
        swing.amount = 0.28;
        swing.cycles_per_loop = 16;
    } else {
        swing.amount = 0.15;
        swing.cycles_per_loop = 4 + static_cast<int>(index % 13U);
        swing.phase_degrees = std::fmod(static_cast<double>(index) * 137.50776405003785,
                                       360.0);
    }
    return swing;
}

EffectConfig default_effect(EffectType type) {
    EffectConfig effect;
    effect.type = type;
    effect.name = effect_type_name(type);
    effect.enabled = false;
    switch (type) {
        case EffectType::EndlessZoom:
            effect.intensity = 1.0;
            effect.magnitude = 1.0;
            effect.frequency = 1.0;
            break;
        case EffectType::Ripple:
            effect.intensity = 0.65;
            effect.magnitude = 0.03;
            effect.frequency = 6.0;
            break;
        case EffectType::Shake:
            effect.intensity = 0.5;
            effect.magnitude = 0.02;
            effect.frequency = 3.0;
            effect.secondary = 0.8;
            break;
        case EffectType::FlagWave:
            effect.intensity = 0.6;
            effect.magnitude = 0.03;
            effect.frequency = 3.0;
            effect.secondary = 0.35;
            break;
        case EffectType::Glow:
            effect.intensity = 0.7;
            effect.secondary = 0.35;
            effect.radius_pixels = 16.0;
            effect.threshold = 0.30;
            effect.soft_knee = 0.40;
            break;
        case EffectType::BlockScale:
            effect.intensity = 1.0;
            effect.magnitude = 1.0;
            effect.frequency = 3.0;
            effect.secondary = 0.0;
            break;
    }
    return effect;
}

PaletteConfig default_palette(std::size_t index) {
    PaletteConfig palette;
    palette.enabled = true;
    switch (index % kBuiltInPaletteCount) {
        case 0U:
            palette.name = "Ember";
            palette.colors = {{0.08, 0.01, 0.02}, {0.55, 0.03, 0.02},
                              {1.00, 0.24, 0.02}, {1.00, 0.75, 0.12},
                              {1.00, 0.97, 0.72}};
            break;
        case 1U:
            palette.name = "Deep Ocean";
            palette.colors = {{0.01, 0.04, 0.16}, {0.00, 0.20, 0.42},
                              {0.00, 0.52, 0.66}, {0.20, 0.86, 0.82},
                              {0.78, 1.00, 0.92}};
            break;
        case 2U:
            palette.name = "Vaporwave";
            palette.colors = {{0.12, 0.02, 0.24}, {0.42, 0.10, 0.72},
                              {0.96, 0.18, 0.72}, {0.12, 0.86, 0.96},
                              {1.00, 0.78, 0.96}};
            break;
        case 3U:
            palette.name = "Forest Biolume";
            palette.colors = {{0.01, 0.08, 0.05}, {0.02, 0.28, 0.15},
                              {0.10, 0.58, 0.30}, {0.42, 0.92, 0.38},
                              {0.84, 1.00, 0.62}};
            break;
        case 4U:
            palette.name = "Arcade";
            palette.colors = {{0.02, 0.02, 0.04}, {0.98, 0.08, 0.22},
                              {1.00, 0.82, 0.08}, {0.10, 0.92, 0.42},
                              {0.06, 0.36, 1.00}, {0.72, 0.12, 1.00}};
            break;
        default:
            palette.name = "Moonlight";
            palette.colors = {{0.02, 0.03, 0.08}, {0.12, 0.16, 0.28},
                              {0.34, 0.40, 0.58}, {0.68, 0.74, 0.88},
                              {0.96, 0.98, 1.00}};
            break;
    }
    return palette;
}

RenderConfig default_config() {
    RenderConfig config;
    config.waves.reserve(3);
    for (std::size_t index = 0; index < 3; ++index) {
        WaveConfig wave = default_wave(index);
        wave.id = static_cast<std::uint64_t>(index) + 1U;
        config.waves.push_back(std::move(wave));
    }

    config.swings.push_back(default_swing(0));
    config.swings[0].id = 4;

    config.effects.reserve(6);
    const std::array<EffectType, 6> types = {
        EffectType::EndlessZoom, EffectType::Ripple, EffectType::Shake,
        EffectType::FlagWave, EffectType::Glow, EffectType::BlockScale};
    for (std::size_t index = 0; index < types.size(); ++index) {
        EffectConfig effect = default_effect(types[index]);
        effect.id = static_cast<std::uint64_t>(index) + 5U;
        config.effects.push_back(std::move(effect));
    }
    config.palette = default_palette(0U);
    config.palette.enabled = false;
    return config;
}

LayerConfig default_layer(std::size_t index) {
    const RenderConfig legacy = default_config();
    LayerConfig layer;
    layer.uuid = generate_uuid();
    layer.file_id = static_cast<std::uint64_t>(index);
    layer.name = "Layer " + std::to_string(index + 1U);
    layer.render = static_cast<const RenderData&>(legacy);
    return layer;
}

ProjectConfig default_project() {
    const RenderConfig legacy = default_config();
    ProjectConfig project;
    project.uuid = generate_uuid();
    project.canvas.width = legacy.width;
    project.canvas.height = legacy.height;
    project.canvas.block_size = legacy.block_size;
    project.canvas.total_frames = legacy.total_frames;
    project.canvas.fps = legacy.fps;
    project.canvas.clock = legacy.clock;
    project.output = legacy.output;
    project.layers.push_back(default_layer(0));
    return project;
}

RenderConfig apply_global_config(const CanvasLoopConfig& canvas,
                                 const ExportConfig& output,
                                 const RenderData& render) {
    RenderConfig config;
    static_cast<RenderData&>(config) = render;
    config.width = canvas.width;
    config.height = canvas.height;
    config.block_size = canvas.block_size;
    config.total_frames = canvas.total_frames;
    config.fps = canvas.fps;
    config.clock = canvas.clock;
    config.output = output;
    return config;
}

std::uint64_t allocate_id(const RenderData& render) {
    std::unordered_set<std::uint64_t> used;
    used.reserve(render.waves.size() + render.swings.size() + render.effects.size());
    std::uint64_t maximum = 0;
    const auto remember = [&](std::uint64_t id) {
        if (id != 0) {
            used.insert(id);
            maximum = std::max(maximum, id);
        }
    };
    for (const WaveConfig& wave : render.waves) remember(wave.id);
    for (const SwingConfig& swing : render.swings) remember(swing.id);
    for (const EffectConfig& effect : render.effects) remember(effect.id);

    if (maximum != std::numeric_limits<std::uint64_t>::max()) {
        return maximum + 1U;
    }
    for (std::uint64_t candidate = 1; candidate != 0; ++candidate) {
        if (used.find(candidate) == used.end()) {
            return candidate;
        }
    }
    return 0;
}

std::uint64_t allocate_id(const RenderConfig& config) {
    return allocate_id(static_cast<const RenderData&>(config));
}

std::uint64_t allocate_layer_file_id(const ProjectConfig& project) {
    std::unordered_set<std::uint64_t> used;
    used.reserve(project.layers.size());
    std::uint64_t maximum = 0;
    for (const LayerConfig& layer : project.layers) {
        used.insert(layer.file_id);
        maximum = std::max(maximum, layer.file_id);
    }
    if (project.layers.empty()) {
        return 0;
    }
    if (maximum != std::numeric_limits<std::uint64_t>::max()) {
        return maximum + 1U;
    }
    for (std::uint64_t candidate = 0;; ++candidate) {
        if (used.find(candidate) == used.end()) {
            return candidate;
        }
    }
}

ValidationResult validate_impl(const RenderConfig& config, bool include_export) {
    if (config.width < 16 || config.width > kMaximumDimension
        || config.height < 16 || config.height > kMaximumDimension) {
        return invalid_result("Width and height must each be between 16 and 16384 pixels.");
    }
    if (config.block_size < 1
        || config.block_size > std::max(config.width, config.height)) {
        return invalid_result("Block size must be between 1 and the larger image dimension.");
    }
    if (config.total_frames < 2 || config.total_frames > kMaximumFrames) {
        return invalid_result("Frame count must be between 2 and 1000000.");
    }
    if (!finite_in_range(config.fps, 1.0, 240.0)) {
        return invalid_result("FPS must be finite and between 1 and 240.");
    }
    if (!valid_enum(config.clock.mode)
        || !valid_enum(config.clock.interpolation)
        || !valid_enum(config.clock.fit)
        || !valid_enum(config.clock.music_tempo)
        || !valid_enum(config.clock.music_swing_policy)) {
        return invalid_result("The synchronized clock contains an unknown mode or policy.");
    }
    if (config.clock.frame_interval < 1
        || config.clock.frame_interval > kMaximumFrames
        || config.clock.time_interval_microseconds < 1
        || config.clock.time_interval_microseconds > INT64_C(1000000000000)
        || config.clock.beat_offset_microseconds < INT64_C(-86400000000)
        || config.clock.beat_offset_microseconds > INT64_C(86400000000)
        || !finite_in_range(config.clock.phase_offset_degrees,
                            -36000.0, 36000.0)) {
        return invalid_result("Clock intervals, offset, or phase are outside their allowed range.");
    }
    ParsedMeter parsed_meter;
    std::string meter_error;
    if (!finite_in_range(config.clock.meter.bpm, 1.0, 1000.0)
        || config.clock.meter.tempo_note_denominator < 1
        || config.clock.meter.tempo_note_denominator > kMaximumMeterValue
        || !parse_meter_expression(config.clock.meter.expression,
                                   parsed_meter, meter_error)) {
        return invalid_result(meter_error.empty()
                                  ? "Meter tempo values are outside their allowed range."
                                  : "Invalid meter expression: " + meter_error);
    }

    const MusicAnalysis& music = config.clock.music;
    if (music.schema_version != 1U
        || music.analyzer_version.size() > kMaximumNameBytes
        || (!music.analyzer_version.empty()
            && !valid_name(music.analyzer_version))
        || (!music.source_sha256.empty()
            && !valid_lower_hex_digest(music.source_sha256))
        || (!music.source_basename.empty()
            && !valid_music_basename(music.source_basename))
        || music.source_format.size() > 64U
        || (!music.source_format.empty() && !valid_name(music.source_format))
        || music.source_sample_rate > 768000U
        || music.source_channel_count > 64U
        || !finite_in_range(music.duration_seconds, 0.0,
                            kMaximumMusicDurationSeconds)
        || !finite_in_range(music.detected_bpm, 0.0, 1000.0)
        || !finite_in_range(music.tempo_confidence, 0.0, 1.0)
        || music.beat_times_seconds.size() > kMaximumMusicBeats
        || music.tempo_points.size() > kMaximumMusicTempoPoints
        || music.feature_samples.size() > kMaximumMusicFeatureSamples) {
        return invalid_result("Music analysis metadata is invalid or exceeds its safety limits.");
    }
    double previous_beat = -1.0;
    for (const double beat : music.beat_times_seconds) {
        if (!std::isfinite(beat) || beat < 0.0
            || beat > music.duration_seconds || beat <= previous_beat) {
            return invalid_result(
                "Music beat times must be finite, strictly increasing, and within the source duration.");
        }
        previous_beat = beat;
    }
    double previous_tempo_time = -1.0;
    for (const MusicTempoPoint& tempo : music.tempo_points) {
        if (!std::isfinite(tempo.time_seconds) || tempo.time_seconds < 0.0
            || tempo.time_seconds > music.duration_seconds
            || tempo.time_seconds <= previous_tempo_time
            || !finite_in_range(tempo.bpm, 1.0, 1000.0)
            || !finite_in_range(tempo.confidence, 0.0, 1.0)) {
            return invalid_result(
                "Music tempo points must be ordered and contain bounded time, BPM, and confidence values.");
        }
        previous_tempo_time = tempo.time_seconds;
    }
    for (const MusicFeatureSample& sample : music.feature_samples) {
        if (!finite_in_range(sample.energy, 0.0, 1.0)
            || !finite_in_range(sample.bass, 0.0, 1.0)
            || !finite_in_range(sample.midrange, 0.0, 1.0)
            || !finite_in_range(sample.treble, 0.0, 1.0)
            || !finite_in_range(sample.onset, 0.0, 1.0)
            || !finite_in_range(sample.beat, 0.0, 1.0)
            || !finite_in_range(sample.spectral_centroid, 0.0, 1.0)
            || !finite_in_range(sample.spectral_flatness, 0.0, 1.0)
            || !finite_in_range(sample.chroma_hue, 0.0, 1.0)
            || !finite_in_range(sample.chroma_strength, 0.0, 1.0)) {
            return invalid_result(
                "Music feature samples must contain finite normalized values from 0 to 1.");
        }
    }
    if (music.source_frame_count != 0U
        && music.source_sample_rate == 0U) {
        return invalid_result(
            "Music source frame count requires a nonzero sample rate.");
    }
    if (music.source_sample_rate != 0U) {
        const long double source_duration =
            static_cast<long double>(music.source_frame_count)
            / static_cast<long double>(music.source_sample_rate);
        if (source_duration
            > static_cast<long double>(kMaximumMusicDurationSeconds)) {
            return invalid_result("Music source frame count exceeds the duration limit.");
        }
        if (music.source_frame_count != 0U) {
            const double tolerance = std::max(
                1.0 / static_cast<double>(music.source_sample_rate), 1.0e-6);
            if (std::fabs(static_cast<double>(source_duration)
                          - music.duration_seconds) > tolerance) {
                return invalid_result(
                    "Music duration does not agree with its decoded frame count and sample rate.");
            }
        }
    }
    if (config.clock.mode == ClockMode::Music) {
        int resolved_count = 0;
        std::string frame_error;
        if (music.analyzer_version.empty()
            || !valid_lower_hex_digest(music.source_sha256)
            || !valid_music_basename(music.source_basename)
            || music.source_format.empty()
            || music.source_frame_count == 0U
            || music.source_sample_rate == 0U
            || music.source_channel_count == 0U
            || music.beat_times_seconds.empty()
            || !effective_frame_count_impl(config.total_frames, config.fps,
                                            config.clock, resolved_count,
                                            frame_error)) {
            return invalid_result(frame_error.empty()
                                      ? "Music clock requires complete bounded cached analysis."
                                      : frame_error);
        }
    }
    if (!valid_enum(config.audio_reactive.wave_source)
        || !valid_enum(config.audio_reactive.effect_source)
        || !valid_enum(config.audio_reactive.color_source)
        || !finite_in_range(config.audio_reactive.wave_amount, -1.0, 10.0)
        || !finite_in_range(config.audio_reactive.effect_amount, -1.0, 10.0)
        || !finite_in_range(config.audio_reactive.color_amount_degrees,
                            -3600.0, 3600.0)) {
        return invalid_result("Audio-reactive routing contains an invalid source or amount.");
    }
    if (config.waves.size() > kMaximumWaves) {
        return invalid_result("The configuration contains too many waves.");
    }
    if (config.swings.size() > kMaximumSwings) {
        return invalid_result("The configuration contains too many swings.");
    }
    if (config.effects.size() > kMaximumEffects) {
        return invalid_result("The configuration contains too many effects.");
    }

    std::unordered_set<std::uint64_t> identifiers;
    identifiers.reserve(config.waves.size() + config.swings.size() + config.effects.size());
    const auto accept_id = [&identifiers](std::uint64_t id) {
        return id != 0 && identifiers.insert(id).second;
    };

    for (std::size_t index = 0; index < config.waves.size(); ++index) {
        const WaveConfig& wave = config.waves[index];
        if (!accept_id(wave.id)) {
            return invalid_result("Every wave, swing, and effect must have a unique nonzero ID.");
        }
        if (!valid_name(wave.name)) {
            return invalid_result("Wave " + std::to_string(index + 1U)
                                  + " has an invalid or overlong name.");
        }
        if (!finite_in_range(wave.x_percent, -100.0, 200.0)
            || !finite_in_range(wave.y_percent, -100.0, 200.0)
            || !finite_in_range(wave.amplitude, 0.0, 10.0)
            || !finite_in_range(wave.spatial_frequency, 0.0, 1000.0)
            || wave.cycles_per_loop < -1000 || wave.cycles_per_loop > 1000
            || !finite_in_range(wave.phase_degrees, -36000.0, 36000.0)
            || !finite_in_range(wave.direction, 0.0, 1.0)) {
            return invalid_result("Wave " + std::to_string(index + 1U)
                                  + " has a value outside its allowed range.");
        }
    }

    for (std::size_t index = 0; index < config.swings.size(); ++index) {
        const SwingConfig& swing = config.swings[index];
        if (!accept_id(swing.id)) {
            return invalid_result("Every wave, swing, and effect must have a unique nonzero ID.");
        }
        if (!valid_name(swing.name) || !valid_enum(swing.waveform)
            || !finite_in_range(swing.amount, -2.0, 2.0)
            || swing.cycles_per_loop < 0 || swing.cycles_per_loop > 1000
            || !finite_in_range(swing.phase_degrees, -36000.0, 36000.0)
            || !finite_in_range(swing.shape, 0.0, 1.0)
            || !finite_in_range(swing.center_x, -10.0, 10.0)
            || !finite_in_range(swing.center_y, -10.0, 10.0)
            || !finite_in_range(swing.radius, 0.0, 10.0)) {
            return invalid_result("Swing " + std::to_string(index + 1U)
                                  + " has a value outside its allowed range.");
        }
    }

    bool has_enabled_effect = false;
    bool has_enabled_glow = false;
    bool has_transparent_edge_effect = false;
    // Base generation is bounded near unit range. Track a deliberately
    // conservative upper bound for sequential additive glow amplification so
    // every accepted setup remains representable by 32-bit float channels.
    double logarithmic_color_bound = std::log(8.0);
    for (std::size_t index = 0; index < config.effects.size(); ++index) {
        const EffectConfig& effect = config.effects[index];
        if (!accept_id(effect.id)) {
            return invalid_result("Every wave, swing, and effect must have a unique nonzero ID.");
        }
        if (!valid_name(effect.name) || !valid_enum(effect.type)
            || !valid_enum(effect.space)
            || !valid_enum(effect.edge_mode)
            || effect.cycles_per_loop < -1000 || effect.cycles_per_loop > 1000
            || !finite_in_range(effect.phase_degrees, -36000.0, 36000.0)
            || !finite_in_range(effect.intensity, 0.0, 100.0)
            || !finite_in_range(effect.magnitude, 0.0, 10.0)
            || !finite_in_range(effect.frequency, 0.0, 1000.0)
            || !finite_in_range(effect.secondary, -100.0, 100.0)
            || !finite_in_range(effect.center_x, -10.0, 10.0)
            || !finite_in_range(effect.center_y, -10.0, 10.0)
            || !finite_in_range(effect.angle_degrees, -36000.0, 36000.0)
            || !finite_in_range(effect.radius_pixels, 0.0,
                                static_cast<double>(kMaximumDimension))
            || !finite_in_range(effect.threshold, 0.0, 64.0)
            || !finite_in_range(effect.soft_knee, 0.0, 1.0)
            || !finite_in_range(effect.area_radius, 0.0, 10.0)) {
            return invalid_result("Effect " + std::to_string(index + 1U)
                                  + " has a value outside its allowed range.");
        }
        if (effect.type == EffectType::BlockScale
            && (effect.intensity > 1.0
                || effect.magnitude <= 0.0
                || effect.frequency < effect.magnitude
                || effect.secondary < 0.0
                || std::floor(effect.secondary) != effect.secondary)) {
            return invalid_result(
                "Block scale effect " + std::to_string(index + 1U)
                + " requires a mix from 0 to 1, positive ordered multipliers, "
                  "and whole quantization steps from 0 to 100.");
        }
        const bool active_effect = effect_has_render_work(effect);
        const bool active_glow = active_effect && effect.type == EffectType::Glow;
        has_enabled_effect = has_enabled_effect || active_effect;
        has_enabled_glow = has_enabled_glow || active_glow;
        has_transparent_edge_effect = has_transparent_edge_effect
                                      || (active_effect
                                          && effect_uses_edge_mode(effect.type)
                                          && effect.edge_mode == EdgeMode::Alpha);
        if (active_glow) {
            double maximum_intensity = effect.intensity;
            if (config.audio_reactive.enabled
                && config.audio_reactive.effects_enabled
                && (!config.audio_reactive.synchronized_only
                    || effect.synchronized)) {
                maximum_intensity *= std::max(
                    0.0, 1.0 + std::max(0.0,
                                       config.audio_reactive.effect_amount));
            }
            logarithmic_color_bound += std::log1p(maximum_intensity);
        }
    }
    if (logarithmic_color_bound
        >= std::log(static_cast<double>(std::numeric_limits<float>::max()))) {
        return invalid_result(
            "The enabled glow stack can exceed the 32-bit float color range; "
            "reduce glow intensity or the number of enabled glow effects.");
    }

    if (!finite_in_range(config.phrase_warp, 0.0, 2.0)
        || !finite_in_range(config.ghost_mix, 0.0, 1.0)
        || !finite_in_range(config.ghost_lag_degrees, -360.0, 360.0)
        || !finite_in_range(config.displacement, 0.0, 1000.0)
        || !finite_in_range(config.wave_depth, 0.0, 10.0)
        || !finite_in_range(config.spiral_frequency, 0.0, 1000.0)
        || config.spiral_arms < -100 || config.spiral_arms > 100
        || !finite_in_range(config.wall_frequency, 0.0, 1000.0)
        || !finite_in_range(config.wall_mix, 0.0, 5.0)
        || config.hue_cycles < -100 || config.hue_cycles > 100
        || !finite_in_range(config.saturation, 0.0, 1.0)) {
        return invalid_result("One or more pattern, rhythm, or lighting values are out of range.");
    }

    if (!valid_name(config.palette.name)
        || config.palette.colors.size() > kMaximumPaletteColors
        || (config.palette.enabled && config.palette.colors.empty())) {
        return invalid_result(
            "An enabled starting palette needs 1 to 256 colors and a valid name.");
    }
    for (const PaletteColor& color : config.palette.colors) {
        if (!finite_in_range(color.red, 0.0, 1.0)
            || !finite_in_range(color.green, 0.0, 1.0)
            || !finite_in_range(color.blue, 0.0, 1.0)) {
            return invalid_result("Palette colors must contain finite RGB values from 0 to 1.");
        }
    }
    if (!valid_enum(config.transform.mirror)) {
        return invalid_result("The layer transform contains an unknown mirror mode.");
    }

    if (!finite_in_range(config.alpha.minimum, 0.0, 1.0)
        || !finite_in_range(config.alpha.maximum, 0.0, 1.0)
        || config.alpha.minimum > config.alpha.maximum
        || !finite_in_range(config.alpha.spatial_frequency, 0.0, 1000.0)
        || config.alpha.cycles_per_loop < -1000
        || config.alpha.cycles_per_loop > 1000
        || !finite_in_range(config.alpha.phase_degrees, -36000.0, 36000.0)) {
        return invalid_result("Alpha modulation values are out of range.");
    }
    if (config.quantization.levels < 2 || config.quantization.levels > 65536
        || !finite_in_range(config.quantization.mix, 0.0, 1.0)
        || !valid_enum(config.quantization.mode)) {
        return invalid_result("Quantization values are out of range.");
    }
    if (!valid_enum(config.surface.mapping)
        || config.surface.rotations_per_loop < -1000
        || config.surface.rotations_per_loop > 1000
        || !finite_in_range(config.surface.phase_degrees, -36000.0, 36000.0)
        || !finite_in_range(config.surface.curvature, 0.0, 1.0)
        || !finite_in_range(config.surface.lighting, 0.0, 10.0)) {
        return invalid_result("Surface mapping values are out of range.");
    }
    if ((!config.surface.obj_path.empty()
         && !valid_path_text(config.surface.obj_path, kMaximumPathBytes, false))
        || (config.surface.obj_sha256.empty()
                != config.surface.obj_basename.empty())
        || (!config.surface.obj_sha256.empty()
            && (!valid_lower_hex_digest(config.surface.obj_sha256)
                || config.surface.obj_basename.size()
                       > kMaximumAttachmentBasenameBytes
                || !valid_music_basename(config.surface.obj_basename)))
        || (surface_has_render_work(config.surface)
            && config.surface.mapping == SurfaceMapping::CustomObj
            && config.surface.obj_path.empty()
            && config.surface.obj_sha256.empty())) {
        return invalid_result(
            "A custom OBJ surface requires a valid runtime path or embedded attachment identity.");
    }
    if (include_export) {
        const bool has_transparent_surface =
            surface_has_render_work(config.surface)
            && config.surface.mapping != SurfaceMapping::Plane;
        if (!config.alpha.enabled && !config.output.write_alpha
            && (has_transparent_edge_effect || has_transparent_surface)) {
            return invalid_result(
                "Alpha output must be enabled when an active effect uses transparent "
                "edge handling or an active 3D surface has a transparent exterior.");
        }
        if (config.output.bit_depth != 8 && config.output.bit_depth != 16
            && config.output.bit_depth != 32) {
            return invalid_result("Export bit depth must be 8, 16, or 32.");
        }
        if (config.output.png_compression_level < 0
            || config.output.png_compression_level > 9) {
            return invalid_result("PNG compression level must be between 0 and 9.");
        }
        if (!valid_enum(config.output.dither_method)
            || !valid_path_text(config.output.output_directory, kMaximumPathBytes, false)
            || !valid_path_text(config.output.filename_prefix, kMaximumPrefixBytes, true)
            || config.output.first_frame_number < 0
            || config.output.first_frame_number > 1000000000
            || config.output.filename_digits < 1 || config.output.filename_digits > 12) {
            return invalid_result("One or more output values are invalid.");
        }
    }

    std::size_t pixel_count = 0;
    std::size_t float_count = 0;
    std::size_t frame_bytes = 0;
    if (!checked_multiply(static_cast<std::size_t>(config.width),
                          static_cast<std::size_t>(config.height), pixel_count)
        || !checked_multiply(pixel_count, 4U, float_count)
        || !checked_multiply(float_count, sizeof(float), frame_bytes)) {
        return invalid_result("The requested image dimensions overflow addressable memory.");
    }

    // Rendering is transactional: a previously rendered destination remains
    // alive until the new frame succeeds and is swapped into it.
    std::size_t buffer_count = 2U;
    if (has_enabled_effect || surface_has_render_work(config.surface)) {
        ++buffer_count;
    }
    if (has_enabled_glow) {
        ++buffer_count;
    }
    std::size_t peak_bytes = 0;
    if (!checked_multiply(frame_bytes, buffer_count, peak_bytes)) {
        return invalid_result("The renderer's peak memory estimate overflowed.");
    }
    if (surface_has_render_work(config.surface)
        && config.surface.mapping == SurfaceMapping::CustomObj) {
        std::size_t obj_working_bytes = 0;
        if (!checked_multiply(pixel_count,
                              detail::kObjSurfaceLayeredBytesPerPixel,
                              obj_working_bytes)
            || obj_working_bytes > std::numeric_limits<std::size_t>::max()
                                       - peak_bytes) {
            return invalid_result("The custom OBJ peak memory estimate overflowed.");
        }
        // Validate against the transparent, multi-layer path. Opaque images
        // automatically use a smaller nearest-fragment buffer at render time.
        peak_bytes += obj_working_bytes;
        if (detail::kObjSurfaceMaximumMeshAndProjectionBytes
            > std::numeric_limits<std::size_t>::max() - peak_bytes) {
            return invalid_result("The custom OBJ mesh memory estimate overflowed.");
        }
        peak_bytes += detail::kObjSurfaceMaximumMeshAndProjectionBytes;
    }
    if (peak_bytes > kMaximumPeakBytes) {
        std::ostringstream message;
        message << "Estimated peak rendering memory is "
                << (peak_bytes / (1024U * 1024U))
                << " MiB, above the 1024 MiB safety budget.";
        return invalid_result(message.str(), peak_bytes);
    }

    ValidationResult result;
    result.ok = true;
    result.message = "Configuration is valid.";
    result.estimated_peak_bytes = peak_bytes;
    return result;
}

ValidationResult validate(const RenderConfig& config) {
    return validate_impl(config, true);
}

namespace {

struct RenderCancelled final {};

void throw_if_cancelled(const std::atomic_bool* cancel) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
        throw RenderCancelled{};
    }
}

std::vector<Color> prepare_starting_palette(const PaletteConfig& palette) {
    std::vector<Color> prepared;
    if (!palette.enabled) {
        return prepared;
    }
    prepared.reserve(palette.colors.size());
    for (const PaletteColor& authored : palette.colors) {
        prepared.push_back({srgb_to_linear(authored.red),
                            srgb_to_linear(authored.green),
                            srgb_to_linear(authored.blue), 1.0});
    }
    return prepared;
}

Color nearest_starting_color(const Color& input,
                             const std::vector<Color>& palette) {
    if (palette.empty()) {
        return input;
    }
    const Color* closest = &palette.front();
    double closest_distance = std::numeric_limits<double>::infinity();
    for (const Color& candidate : palette) {
        const double dr = input.r - candidate.r;
        const double dg = input.g - candidate.g;
        const double db = input.b - candidate.b;
        // Linear-light luminance weights make starting-color selection better
        // match what the eye sees than an unweighted RGB cube.
        const double distance = 0.2126 * dr * dr
                                + 0.7152 * dg * dg
                                + 0.0722 * db * db;
        if (distance < closest_distance) {
            closest = &candidate;
            closest_distance = distance;
        }
    }
    return *closest;
}

double alpha_at(const RenderConfig& config, int x, int y, double loop_phase) {
    if (!config.alpha.enabled) {
        return 1.0;
    }
    const double width_scale = config.width > 1
                                   ? static_cast<double>(x) / (config.width - 1)
                                   : 0.0;
    const double height_scale = config.height > 1
                                    ? static_cast<double>(y) / (config.height - 1)
                                    : 0.0;
    const double spatial = (width_scale + height_scale) * 0.7071067811865476;
    const double phase = kTau * config.alpha.spatial_frequency * spatial
                         - static_cast<double>(config.alpha.cycles_per_loop) * loop_phase
                         + radians(config.alpha.phase_degrees);
    const double amount = 0.5 + 0.5 * std::sin(phase);
    return mix_value(config.alpha.minimum, config.alpha.maximum, amount);
}

void generate_base_image(const RenderConfig& config, double loop_phase,
                         const MotionClockState& motion_clock,
                         const MusicFeatureSample& music, Image& image,
                         const std::atomic_bool* cancel) {
    throw_if_cancelled(cancel);
    ensure_image(image, config.width, config.height);
    throw_if_cancelled(cancel);
    const double short_side = static_cast<double>(std::min(config.width, config.height));
    double center_x = 0.5 * static_cast<double>(config.width);
    double center_y = 0.5 * static_cast<double>(config.height);
    for (const WaveConfig& wave : config.waves) {
        if (wave.enabled) {
            center_x = wave.x_percent * 0.01 * static_cast<double>(config.width);
            center_y = wave.y_percent * 0.01 * static_cast<double>(config.height);
            break;
        }
    }

    const double breath = 0.85 + 0.35 * std::sin(loop_phase);
    const std::vector<Color> starting_palette =
        prepare_starting_palette(config.palette);

    std::size_t block_counter = 0U;
    for (int block_y = 0; block_y < config.height; block_y += config.block_size) {
        throw_if_cancelled(cancel);
        for (int block_x = 0; block_x < config.width; block_x += config.block_size) {
            if ((block_counter++ & 63U) == 0U) {
                throw_if_cancelled(cancel);
            }
            const double motion_phase = motion_phase_at(
                motion_clock, static_cast<double>(block_x),
                static_cast<double>(block_y), config.width, config.height);
            const double motion_phase_right =
                motion_clock.spatial_swing_count == 0U
                    ? motion_phase
                    : motion_phase_at(
                          motion_clock,
                          static_cast<double>(block_x + config.block_size),
                          static_cast<double>(block_y), config.width,
                          config.height);
            const double motion_phase_down =
                motion_clock.spatial_swing_count == 0U
                    ? motion_phase
                    : motion_phase_at(
                          motion_clock, static_cast<double>(block_x),
                          static_cast<double>(block_y + config.block_size),
                          config.width, config.height);
            const double ghost_phase = motion_phase
                                       - radians(config.ghost_lag_degrees);
            const double height_here = wave_height(config, block_x, block_y,
                                                   loop_phase, motion_phase,
                                                   music);
            const double height_right = wave_height(config,
                                                     block_x + config.block_size,
                                                     block_y,
                                                     loop_phase,
                                                     motion_phase_right,
                                                     music);
            const double height_down = wave_height(config, block_x,
                                                    block_y + config.block_size,
                                                    loop_phase,
                                                    motion_phase_down,
                                                    music);
            const double slope_x = height_right - height_here;
            const double slope_y = height_down - height_here;
            const double displacement = config.displacement_enabled
                                            ? config.displacement * breath
                                            : 0.0;
            const double displaced_x = block_x + slope_x * displacement;
            const double displaced_y = block_y + slope_y * displacement;
            const double dx = displaced_x - center_x;
            const double dy = displaced_y - center_y;
            const double normalized_distance = std::hypot(dx, dy) / short_side;
            const double angle = std::atan2(dy, dx);
            const double wall_distance = std::min(
                std::min(static_cast<double>(block_x),
                         static_cast<double>(config.width - 1 - block_x)),
                std::min(static_cast<double>(block_y),
                         static_cast<double>(config.height - 1 - block_y)));
            const double normalized_wall_distance = wall_distance / short_side;

            const double main_spiral = config.spiral_enabled
                ? std::sin(kTau * config.spiral_frequency * normalized_distance
                           + angle * config.spiral_arms - motion_phase)
                : 0.0;
            const double ghost_spiral = config.spiral_enabled
                ? std::sin(kTau * config.spiral_frequency * normalized_distance
                           + angle * config.spiral_arms - ghost_phase)
                : 0.0;
            const double main_wall = config.wall_reflection_enabled
                ? std::sin(kTau * config.wall_frequency * normalized_wall_distance
                           + 2.0 * motion_phase)
                : 0.0;
            const double ghost_wall = config.wall_reflection_enabled
                ? std::sin(kTau * config.wall_frequency * normalized_wall_distance
                           + 2.0 * ghost_phase)
                : 0.0;
            const double main_signal = main_spiral + config.wall_mix * main_wall;
            const double ghost_signal = ghost_spiral + config.wall_mix * ghost_wall;
            const double combined_signal = (1.0 - config.ghost_mix) * main_signal
                                           + config.ghost_mix * ghost_signal;
            const double hue = (combined_signal + 1.45) * 260.0
                               + 360.0 * config.hue_cycles * (loop_phase / kTau);
            const double audio_hue_shift =
                config.audio_reactive.enabled
                        && config.audio_reactive.color_enabled
                    ? config.audio_reactive.color_amount_degrees
                          * music_feature_value(
                              music, config.audio_reactive.color_source)
                    : 0.0;

            double lightness = 0.40;
            if (config.lighting_enabled) {
                const double reflection = (slope_x + slope_y) * -0.7071067811865476;
                const double normalized_light = reflection * config.wave_depth * breath;
                lightness += normalized_light < 0.0
                                 ? 0.36 * normalized_light
                                 : 0.28 * normalized_light;
            }
            lightness = clamp_value(lightness, 0.04, 0.68);
            Color base;
            if (starting_palette.empty()) {
                base = hsl_to_linear_rgb(hue, config.saturation, lightness);
                base = rotate_linear_hue(base, audio_hue_shift);
            } else {
                // Choose an authored source color before procedural slope
                // lighting. Otherwise a one-color palette would accidentally
                // make that independent layer toggle inert.
                base = nearest_starting_color(
                    hsl_to_linear_rgb(hue, config.saturation, 0.40),
                    starting_palette);
                // Audio hue response transforms the selected authored color,
                // rather than merely changing which fixed palette entry wins.
                // This keeps even a one-color starting palette visibly musical.
                base = rotate_linear_hue(base, audio_hue_shift);
                if (config.lighting_enabled) {
                    const double lighting_scale = lightness / 0.40;
                    base.r *= lighting_scale;
                    base.g *= lighting_scale;
                    base.b *= lighting_scale;
                }
            }
            // Effects, surface lighting, and later stages remain free to create
            // colors outside the starting palette. Selecting once per block
            // also avoids the old width*height*palette-size restriction pass.
            const int end_x = std::min(block_x + config.block_size, config.width);
            const int end_y = std::min(block_y + config.block_size, config.height);
            for (int y = block_y; y < end_y; ++y) {
                throw_if_cancelled(cancel);
                for (int x = block_x; x < end_x; ++x) {
                    Color output = base;
                    output.a = alpha_at(config, x, y, loop_phase);
                    store_color(image, x, y, output);
                }
            }
        }
    }
}

double effect_phase(const RenderConfig& config, const EffectConfig& effect,
                    double loop_phase,
                    const MotionClockState& motion_clock) {
    const double center_x = effect.center_x
                            * static_cast<double>(config.width - 1);
    const double center_y = effect.center_y
                            * static_cast<double>(config.height - 1);
    // Localized Swings live in source/UV space. A mapped-object center is a
    // post-projection canvas coordinate and cannot be mapped back uniquely for
    // cylinders, meshes, or mirrored layers, so it synchronizes to the honest
    // global portion of the shared clock instead of an unrelated UV location.
    const double synchronized_clock = effect.space == EffectSpace::Surface
                                          ? motion_clock.global_phase
                                          : motion_phase_at(
                                                motion_clock, center_x,
                                                center_y, config.width,
                                                config.height);
    const double clock = effect.synchronized ? synchronized_clock : loop_phase;
    return static_cast<double>(effect.cycles_per_loop) * clock
           + radians(effect.phase_degrees);
}

void apply_coordinate_effect(const Image& source, Image& destination,
                             const EffectConfig& effect, double phase,
                             const std::atomic_bool* cancel) {
    throw_if_cancelled(cancel);
    ensure_image(destination, source.width, source.height);
    throw_if_cancelled(cancel);
    const double short_side = static_cast<double>(std::min(source.width, source.height));
    const double center_x = effect.center_x * static_cast<double>(source.width - 1);
    const double center_y = effect.center_y * static_cast<double>(source.height - 1);
    const double intensity = std::max(0.0, effect.intensity);
    const double displacement = effect.magnitude * short_side * intensity;
    const double angle = radians(effect.angle_degrees);
    const double axis_x = std::cos(angle);
    const double axis_y = std::sin(angle);
    const double perpendicular_x = -axis_y;
    const double perpendicular_y = axis_x;

    const int shake_harmonic = std::max(1, std::min(1000,
        static_cast<int>(std::lround(effect.frequency))));
    const double shake_x = displacement
        * (0.72 * std::sin(shake_harmonic * phase)
           + 0.20 * std::sin((shake_harmonic + 2) * phase + 1.234)
           + 0.08 * std::sin((shake_harmonic + 4) * phase + 3.456));
    const double shake_y = displacement * effect.secondary
        * (0.70 * std::cos((shake_harmonic + 1) * phase + 0.731)
           + 0.22 * std::sin((shake_harmonic + 3) * phase + 2.718)
           + 0.08 * std::cos((shake_harmonic + 5) * phase + 4.123));
    const double rotated_shake_x = axis_x * shake_x - axis_y * shake_y;
    const double rotated_shake_y = axis_y * shake_x + axis_x * shake_y;

    const double zoom_fraction = wrap_unit(phase / kTau);
    const double zoom_blend = smoothstep(zoom_fraction);
    const double zoom_octaves = clamp_value(
        effect.magnitude * std::max(0.01, effect.frequency), 0.0, 4.0);
    const double zoom_ratio = std::pow(2.0, zoom_octaves);
    const double zoom_scale_a = std::pow(zoom_ratio, zoom_fraction);
    const double zoom_scale_b = zoom_ratio > 1.0e-12
                                    ? zoom_scale_a / zoom_ratio
                                    : zoom_scale_a;

    for (int y = 0; y < source.height; ++y) {
        throw_if_cancelled(cancel);
        for (int x = 0; x < source.width; ++x) {
            double sample_x = static_cast<double>(x);
            double sample_y = static_cast<double>(y);
            const double area = circular_influence(
                effect.center_x, effect.center_y, effect.area_radius,
                static_cast<double>(x), static_cast<double>(y),
                source.width, source.height);
            Color sampled;

            switch (effect.type) {
                case EffectType::EndlessZoom: {
                    if (effect.magnitude <= 1.0e-12 || intensity <= 1.0e-12) {
                        sampled = load_color(source, x, y);
                        break;
                    }
                    const double relative_x = x - center_x;
                    const double relative_y = y - center_y;
                    const Color first = sample_bilinear(
                        source, center_x + relative_x / zoom_scale_a,
                        center_y + relative_y / zoom_scale_a, effect.edge_mode);
                    const Color second = sample_bilinear(
                        source, center_x + relative_x / zoom_scale_b,
                        center_y + relative_y / zoom_scale_b, effect.edge_mode);
                    const Color zoomed = blend_straight_alpha(first, second, zoom_blend);
                    sampled = blend_straight_alpha(load_color(source, x, y), zoomed,
                                                   clamp_value(intensity * area,
                                                               0.0, 1.0));
                    break;
                }
                case EffectType::Ripple: {
                    const double dx = x - center_x;
                    const double dy = y - center_y;
                    const double distance = std::hypot(dx, dy);
                    if (distance > 1.0e-12) {
                        const double wave = std::sin(kTau * effect.frequency
                                                     * (distance / short_side) - phase);
                        const double attenuation = effect.secondary > 1.0
                            ? 1.0 / (1.0 + (effect.secondary - 1.0)
                                           * distance / short_side)
                            : 1.0;
                        sample_x -= dx / distance * displacement * wave
                                    * attenuation * area;
                        sample_y -= dy / distance * displacement * wave
                                    * attenuation * area;
                    }
                    sampled = sample_bilinear(source, sample_x, sample_y, effect.edge_mode);
                    break;
                }
                case EffectType::Shake:
                    sampled = sample_bilinear(source,
                                              x - rotated_shake_x * area,
                                              y - rotated_shake_y * area,
                                              effect.edge_mode);
                    break;
                case EffectType::FlagWave: {
                    const double dx = x - center_x;
                    const double dy = y - center_y;
                    const double along = (dx * axis_x + dy * axis_y) / short_side;
                    const double flag = std::sin(kTau * effect.frequency * along - phase);
                    const double harmonic = std::sin(kTau * effect.frequency * 0.5 * along
                                                     - 2.0 * phase + 1.0472);
                    sample_x -= perpendicular_x * displacement
                                * (flag + effect.secondary * 0.35 * harmonic)
                                * area;
                    sample_y -= perpendicular_y * displacement
                                * (flag + effect.secondary * 0.35 * harmonic)
                                * area;
                    sampled = sample_bilinear(source, sample_x, sample_y, effect.edge_mode);
                    break;
                }
                case EffectType::Glow:
                case EffectType::BlockScale:
                    sampled = load_color(source, x, y);
                    break;
            }
            store_color(destination, x, y, sampled);
        }
    }
}

void apply_block_scale(const Image& source, Image& destination,
                       const EffectConfig& effect, double phase,
                       int base_block_size, const std::atomic_bool* cancel) {
    throw_if_cancelled(cancel);
    ensure_image(destination, source.width, source.height);
    throw_if_cancelled(cancel);

    double travel = 0.5 - 0.5 * std::cos(phase);
    const int quantization_steps = static_cast<int>(std::llround(effect.secondary));
    if (quantization_steps > 0) {
        travel = std::round(travel * static_cast<double>(quantization_steps))
                 / static_cast<double>(quantization_steps);
    }
    const double multiplier = mix_value(effect.magnitude, effect.frequency, travel);
    const double requested_size = static_cast<double>(base_block_size) * multiplier;
    const int maximum_size = std::max(source.width, source.height);
    const int block_size = std::max(
        1, std::min(maximum_size,
                    static_cast<int>(std::llround(std::min(
                        requested_size, static_cast<double>(maximum_size))))));
    const double amount = clamp_value(effect.intensity, 0.0, 1.0);

    std::size_t block_counter = 0U;
    for (int block_y = 0; block_y < source.height; block_y += block_size) {
        throw_if_cancelled(cancel);
        const int end_y = std::min(block_y + block_size, source.height);
        for (int block_x = 0; block_x < source.width; block_x += block_size) {
            if ((block_counter++ & 63U) == 0U) {
                throw_if_cancelled(cancel);
            }
            const int end_x = std::min(block_x + block_size, source.width);
            Color average;
            std::size_t sample_count = 0U;
            for (int y = block_y; y < end_y; ++y) {
                throw_if_cancelled(cancel);
                for (int x = block_x; x < end_x; ++x) {
                    const Color sample = load_color(source, x, y);
                    average.r += sample.r;
                    average.g += sample.g;
                    average.b += sample.b;
                    average.a += sample.a;
                    ++sample_count;
                }
            }
            const double reciprocal = 1.0 / static_cast<double>(sample_count);
            average.r *= reciprocal;
            average.g *= reciprocal;
            average.b *= reciprocal;
            average.a *= reciprocal;
            for (int y = block_y; y < end_y; ++y) {
                throw_if_cancelled(cancel);
                for (int x = block_x; x < end_x; ++x) {
                    store_color(destination, x, y,
                                blend_straight_alpha(load_color(source, x, y),
                                                     average, amount));
                }
            }
        }
    }
}

double bloom_weight(double luminance, double threshold, double soft_knee) {
    const double knee = std::max(1.0e-6, threshold * soft_knee);
    double soft = clamp_value(luminance - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1.0e-12);
    const double contribution = std::max(luminance - threshold, soft);
    return luminance > 1.0e-12 ? clamp_value(contribution / luminance, 0.0, 1.0) : 0.0;
}

void extract_bright(const Image& source, Image& bright,
                    const EffectConfig& effect,
                    const std::atomic_bool* cancel) {
    throw_if_cancelled(cancel);
    ensure_image(bright, source.width, source.height);
    throw_if_cancelled(cancel);
    for (int y = 0; y < source.height; ++y) {
        throw_if_cancelled(cancel);
        for (int x = 0; x < source.width; ++x) {
            const Color input = load_color(source, x, y);
            const double luminance = 0.2126 * input.r + 0.7152 * input.g + 0.0722 * input.b;
            const double area = circular_influence(
                effect.center_x, effect.center_y, effect.area_radius,
                static_cast<double>(x), static_cast<double>(y),
                source.width, source.height);
            const double weight = area * bloom_weight(
                luminance, effect.threshold, effect.soft_knee);
            store_color(bright, x, y,
                        {input.r, input.g, input.b, input.a * weight});
        }
    }
}

void blur_nine_tap(const Image& source, Image& destination,
                   double radius, bool horizontal,
                   const std::atomic_bool* cancel) {
    throw_if_cancelled(cancel);
    ensure_image(destination, source.width, source.height);
    throw_if_cancelled(cancel);
    constexpr std::array<double, 9> weights = {
        0.02763055, 0.06628225, 0.12383154, 0.18017382, 0.20416369,
        0.18017382, 0.12383154, 0.06628225, 0.02763055};
    const double spacing = radius / 4.0;
    for (int y = 0; y < source.height; ++y) {
        throw_if_cancelled(cancel);
        for (int x = 0; x < source.width; ++x) {
            Color accumulated;
            for (int tap = -4; tap <= 4; ++tap) {
                const long long offset = static_cast<long long>(
                    std::llround(static_cast<double>(tap) * spacing));
                const long long sample_x = horizontal
                                               ? static_cast<long long>(x) + offset
                                               : x;
                const long long sample_y = horizontal
                                               ? y
                                               : static_cast<long long>(y) + offset;
                const Color sample = sample_texel(source, sample_x, sample_y,
                                                  EdgeMode::Reflect);
                const double weight = weights[static_cast<std::size_t>(tap + 4)];
                accumulated.r += sample.r * sample.a * weight;
                accumulated.g += sample.g * sample.a * weight;
                accumulated.b += sample.b * sample.a * weight;
                accumulated.a += sample.a * weight;
            }
            if (accumulated.a > 1.0e-12) {
                accumulated.r /= accumulated.a;
                accumulated.g /= accumulated.a;
                accumulated.b /= accumulated.a;
            } else {
                accumulated.r = accumulated.g = accumulated.b = 0.0;
            }
            store_color(destination, x, y, accumulated);
        }
    }
}

void apply_glow(Image& image, Image& scratch, Image& auxiliary,
                const EffectConfig& effect, double phase,
                const std::atomic_bool* cancel) {
    throw_if_cancelled(cancel);
    const double pulse_depth = clamp_value(std::fabs(effect.secondary), 0.0, 1.0);
    const double pulse = 0.5 + 0.5 * std::sin(phase);
    const double animated_intensity = effect.intensity
                                      * mix_value(1.0, pulse, pulse_depth);
    if (animated_intensity <= 1.0e-12 || effect.radius_pixels <= 1.0e-12) {
        return;
    }
    extract_bright(image, scratch, effect, cancel);
    blur_nine_tap(scratch, auxiliary, effect.radius_pixels, true, cancel);
    blur_nine_tap(auxiliary, scratch, effect.radius_pixels, false, cancel);
    for (int y = 0; y < image.height; ++y) {
        throw_if_cancelled(cancel);
        for (int x = 0; x < image.width; ++x) {
            Color original = load_color(image, x, y);
            const Color glow = load_color(scratch, x, y);
            // Build the additive halo in premultiplied space, then return to
            // the library's straight-alpha representation. This prevents a
            // partially transparent source from attenuating the halo twice.
            const double glow_coverage = clamp_value(
                animated_intensity * glow.a, 0.0, 1.0);
            const double output_alpha = original.a
                                        + glow_coverage * (1.0 - original.a);
            if (output_alpha > 1.0e-12) {
                original.r = (original.r * original.a
                              + animated_intensity * glow.r * glow.a)
                             / output_alpha;
                original.g = (original.g * original.a
                              + animated_intensity * glow.g * glow.a)
                             / output_alpha;
                original.b = (original.b * original.a
                              + animated_intensity * glow.b * glow.a)
                             / output_alpha;
            }
            original.a = output_alpha;
            store_color(image, x, y, original);
        }
    }
}

Vec3 add(Vec3 first, Vec3 second) {
    return {first.x + second.x, first.y + second.y, first.z + second.z};
}

Vec3 multiply(Vec3 value, double amount) {
    return {value.x * amount, value.y * amount, value.z * amount};
}

double dot(Vec3 first, Vec3 second) {
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

Vec3 face_forward(Vec3 normal, Vec3 ray_direction) {
    return dot(normal, ray_direction) > 0.0 ? multiply(normal, -1.0) : normal;
}

Vec3 normalize(Vec3 value) {
    const double length = std::sqrt(dot(value, value));
    return length > 1.0e-12 ? multiply(value, 1.0 / length) : Vec3{};
}

Vec3 rotate_x(Vec3 value, double angle) {
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    return {value.x, cosine * value.y - sine * value.z,
            sine * value.y + cosine * value.z};
}

Vec3 rotate_y(Vec3 value, double angle) {
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    return {cosine * value.x + sine * value.z, value.y,
            -sine * value.x + cosine * value.z};
}

Color shade_surface(Color color, Vec3 normal, double lighting) {
    const Vec3 light = normalize({-0.45, -0.55, 0.75});
    const double diffuse = std::max(0.0, dot(normalize(normal), light));
    const double lit = 0.28 + 0.72 * diffuse;
    const double multiplier = std::max(0.0, 1.0 + lighting * (lit - 1.0));
    color.r *= multiplier;
    color.g *= multiplier;
    color.b *= multiplier;
    return color;
}

struct CubeHit {
    double distance = 0.0;
    Vec3 point;
    Vec3 normal;
};

struct CubeIntersections {
    CubeHit front;
    CubeHit back;
    bool has_back = false;
};

Vec3 cube_normal(Vec3 point) {
    const double absolute_x = std::fabs(point.x);
    const double absolute_y = std::fabs(point.y);
    const double absolute_z = std::fabs(point.z);
    if (absolute_x >= absolute_y && absolute_x >= absolute_z) {
        return {point.x >= 0.0 ? 1.0 : -1.0, 0.0, 0.0};
    }
    if (absolute_y >= absolute_x && absolute_y >= absolute_z) {
        return {0.0, point.y >= 0.0 ? 1.0 : -1.0, 0.0};
    }
    return {0.0, 0.0, point.z >= 0.0 ? 1.0 : -1.0};
}

bool intersect_cube(Vec3 origin, Vec3 direction, CubeIntersections& intersections) {
    double near_distance = -std::numeric_limits<double>::infinity();
    double far_distance = std::numeric_limits<double>::infinity();
    const std::array<double, 3> origins = {origin.x, origin.y, origin.z};
    const std::array<double, 3> directions = {direction.x, direction.y, direction.z};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (std::fabs(directions[axis]) < 1.0e-12) {
            if (origins[axis] < -1.0 || origins[axis] > 1.0) {
                return false;
            }
            continue;
        }
        double first = (-1.0 - origins[axis]) / directions[axis];
        double second = (1.0 - origins[axis]) / directions[axis];
        if (first > second) {
            std::swap(first, second);
        }
        near_distance = std::max(near_distance, first);
        far_distance = std::min(far_distance, second);
        if (near_distance > far_distance) {
            return false;
        }
    }
    if (far_distance < 0.0) {
        return false;
    }

    intersections = {};
    intersections.front.distance = near_distance >= 0.0
                                       ? near_distance
                                       : far_distance;
    intersections.front.point = add(
        origin, multiply(direction, intersections.front.distance));
    intersections.front.normal = cube_normal(intersections.front.point);
    if (near_distance >= 0.0 && far_distance - near_distance > 1.0e-10) {
        intersections.back.distance = far_distance;
        intersections.back.point = add(origin, multiply(direction, far_distance));
        intersections.back.normal = cube_normal(intersections.back.point);
        intersections.has_back = true;
    }
    return true;
}

std::pair<double, double> cube_uv(Vec3 point, Vec3 normal) {
    double u = 0.5;
    double v = 0.5;
    if (std::fabs(normal.x) > 0.5) {
        u = normal.x > 0.0 ? (1.0 - point.z) * 0.5 : (point.z + 1.0) * 0.5;
        v = (1.0 - point.y) * 0.5;
    } else if (std::fabs(normal.y) > 0.5) {
        u = (point.x + 1.0) * 0.5;
        v = normal.y > 0.0 ? (point.z + 1.0) * 0.5 : (1.0 - point.z) * 0.5;
    } else {
        u = normal.z > 0.0 ? (point.x + 1.0) * 0.5 : (1.0 - point.x) * 0.5;
        v = (1.0 - point.y) * 0.5;
    }
    return {clamp_value(u, 0.0, 1.0), clamp_value(v, 0.0, 1.0)};
}

bool apply_surface_mapping(const Image& source, Image& destination,
                           const SurfaceConfig& surface, double loop_phase,
                           std::string* error,
                           const std::atomic_bool* cancel) {
    throw_if_cancelled(cancel);
    if (surface.mapping == SurfaceMapping::CustomObj) {
        const bool rendered = detail::apply_obj_surface_mapping(
            source, destination, surface.obj_path, surface.rotations_per_loop,
            surface.phase_degrees, surface.curvature, surface.lighting,
            loop_phase, error, cancel);
        throw_if_cancelled(cancel);
        return rendered;
    }
    ensure_image(destination, source.width, source.height);
    throw_if_cancelled(cancel);
    const double phase = static_cast<double>(surface.rotations_per_loop) * loop_phase
                         + radians(surface.phase_degrees);
    const double curvature = clamp_value(surface.curvature, 0.0, 1.0);
    const double short_side = static_cast<double>(std::min(source.width, source.height));
    const double center_x = 0.5 * static_cast<double>(source.width - 1);
    const double center_y = 0.5 * static_cast<double>(source.height - 1);

    for (int y = 0; y < source.height; ++y) {
        throw_if_cancelled(cancel);
        for (int x = 0; x < source.width; ++x) {
            const double screen_u = source.width > 1
                                      ? static_cast<double>(x) / (source.width - 1)
                                      : 0.5;
            const double screen_v = source.height > 1
                                      ? static_cast<double>(y) / (source.height - 1)
                                      : 0.5;
            Color output;
            bool visible = true;

            switch (surface.mapping) {
                case SurfaceMapping::Plane: {
                    const double dx = x - center_x;
                    const double dy = y - center_y;
                    const double cosine = std::cos(-phase);
                    const double sine = std::sin(-phase);
                    const double sample_x = center_x + cosine * dx - sine * dy;
                    const double sample_y = center_y + sine * dx + cosine * dy;
                    output = sample_bilinear(source, sample_x, sample_y,
                                             EdgeMode::Reflect);
                    break;
                }
                case SurfaceMapping::Cylinder: {
                    const double radius = 0.46 * short_side;
                    const double normalized_x = (x - center_x) / radius;
                    const double normalized_y = (y - center_y)
                                                / (0.46 * source.height);
                    if (std::fabs(normalized_x) > 1.0
                        || std::fabs(normalized_y) > 1.0) {
                        visible = false;
                        break;
                    }
                    const double longitude = std::asin(clamp_value(normalized_x, -1.0, 1.0));
                    const double surface_v = 0.5 + 0.5 * normalized_y;
                    const double normalized_z = std::sqrt(std::max(
                        0.0, 1.0 - normalized_x * normalized_x));
                    const auto sample_side = [&](double side_longitude,
                                                 double normal_z) {
                        const double wrapped_u = wrap_unit(
                            0.5 + side_longitude / kTau - phase / kTau);
                        Color sampled = sample_bilinear_wrapped_x(
                            source, wrapped_u * source.width,
                            surface_v * (source.height - 1));
                        const Vec3 outward_normal = {normalized_x, 0.0, normal_z};
                        return shade_surface(sampled,
                                             face_forward(outward_normal,
                                                          {0.0, 0.0, -1.0}),
                                             surface.lighting);
                    };
                    Color wrapped = sample_side(longitude, normalized_z);
                    if (normalized_z > 1.0e-10) {
                        const double rear_longitude = normalized_x >= 0.0
                                                          ? kPi - longitude
                                                          : -kPi - longitude;
                        const Color rear = sample_side(rear_longitude, -normalized_z);
                        wrapped = composite_straight_alpha_over(wrapped, rear);
                    }
                    const Color planar = load_color(source, x, y);
                    output = blend_straight_alpha(planar, wrapped, curvature);
                    break;
                }
                case SurfaceMapping::Sphere: {
                    const double radius = 0.46 * short_side;
                    const double normalized_x = (x - center_x) / radius;
                    const double normalized_y = (center_y - y) / radius;
                    const double radius_squared = normalized_x * normalized_x
                                                  + normalized_y * normalized_y;
                    if (radius_squared > 1.0) {
                        visible = false;
                        break;
                    }
                    const double normalized_z = std::sqrt(std::max(0.0,
                        1.0 - radius_squared));
                    const auto sample_side = [&](double normal_z) {
                        const Vec3 normal = {normalized_x, normalized_y, normal_z};
                        const Vec3 texture_normal = rotate_y(normal, -phase);
                        const double longitude = std::atan2(texture_normal.x,
                                                            texture_normal.z);
                        const double latitude = std::asin(
                            clamp_value(texture_normal.y, -1.0, 1.0));
                        const double wrapped_u = wrap_unit(0.5 + longitude / kTau);
                        const double sphere_v = 0.5 - latitude / kPi;
                        Color sampled = sample_bilinear_wrapped_x(
                            source, wrapped_u * source.width,
                            sphere_v * (source.height - 1));
                        return shade_surface(sampled,
                                             face_forward(normal, {0.0, 0.0, -1.0}),
                                             surface.lighting);
                    };
                    Color wrapped = sample_side(normalized_z);
                    if (normalized_z > 1.0e-10) {
                        const Color rear = sample_side(-normalized_z);
                        wrapped = composite_straight_alpha_over(wrapped, rear);
                    }
                    const Color planar = load_color(source, x, y);
                    output = blend_straight_alpha(planar, wrapped, curvature);
                    break;
                }
                case SurfaceMapping::Cube: {
                    const double scale = 0.52 * short_side;
                    const double screen_x = (x - center_x) / scale;
                    const double screen_y = (center_y - y) / scale;
                    Vec3 origin = {0.0, 0.0, 3.4};
                    Vec3 direction = normalize({screen_x, screen_y, -2.5});
                    const double fixed_x_rotation = -0.35;
                    const double y_rotation = 0.55 + phase;
                    origin = rotate_x(rotate_y(origin, -y_rotation),
                                      -fixed_x_rotation);
                    direction = rotate_x(rotate_y(direction, -y_rotation),
                                         -fixed_x_rotation);
                    CubeIntersections intersections;
                    if (!intersect_cube(origin, direction, intersections)) {
                        visible = false;
                        break;
                    }
                    const auto sample_hit = [&](const CubeHit& hit) {
                        const auto uv = cube_uv(hit.point, hit.normal);
                        const double mapped_u = mix_value(screen_u, uv.first, curvature);
                        const double mapped_v = mix_value(screen_v, uv.second, curvature);
                        Color sampled = sample_bilinear(
                            source, mapped_u * (source.width - 1),
                            mapped_v * (source.height - 1), EdgeMode::Reflect);
                        const Vec3 lighting_normal = face_forward(hit.normal, direction);
                        const Vec3 world_normal = rotate_y(
                            rotate_x(lighting_normal, fixed_x_rotation), y_rotation);
                        return shade_surface(sampled, world_normal,
                                             surface.lighting * curvature);
                    };
                    const Color front = sample_hit(intersections.front);
                    output = front;
                    if (intersections.has_back) {
                        const Color back = sample_hit(intersections.back);
                        const Color layered = composite_straight_alpha_over(front, back);
                        // Cube curvature already morphs UVs and lighting. Fade
                        // rear-face coverage in separately so curvature zero
                        // remains exactly planar instead of doubling alpha.
                        output = blend_straight_alpha(front, layered, curvature);
                    }
                    break;
                }
                case SurfaceMapping::CustomObj:
                    // Dispatched before the analytic per-pixel mapper above.
                    break;
            }

            if (!visible) {
                // Fade both color and coverage continuously from the planar
                // source to the primitive's transparent exterior. At curvature
                // one this retains the established fully transparent/black mask;
                // an infinitesimal curvature now remains infinitesimally close
                // to the planar image instead of abruptly cropping it.
                output = blend_straight_alpha(load_color(source, x, y), {}, curvature);
            }
            store_color(destination, x, y, output);
        }
    }
    return true;
}

double quantize_value(double value, int levels) {
    const double maximum_index = static_cast<double>(levels - 1);
    return std::round(clamp_value(value, 0.0, 1.0) * maximum_index)
           / maximum_index;
}

std::array<double, 3> rgb_to_hsv(double red, double green, double blue) {
    const double maximum = std::max(red, std::max(green, blue));
    const double minimum = std::min(red, std::min(green, blue));
    const double delta = maximum - minimum;
    double hue = 0.0;
    if (delta > 1.0e-12) {
        if (maximum == red) {
            hue = std::fmod((green - blue) / delta, 6.0);
        } else if (maximum == green) {
            hue = (blue - red) / delta + 2.0;
        } else {
            hue = (red - green) / delta + 4.0;
        }
        hue /= 6.0;
        if (hue < 0.0) {
            hue += 1.0;
        }
    }
    const double saturation = maximum > 1.0e-12 ? delta / maximum : 0.0;
    return {hue, saturation, maximum};
}

std::array<double, 3> hsv_to_rgb(double hue, double saturation, double value) {
    hue = wrap_unit(hue);
    saturation = clamp_value(saturation, 0.0, 1.0);
    const double chroma = value * saturation;
    const double hue_sector = hue * 6.0;
    const double x = chroma * (1.0 - std::fabs(std::fmod(hue_sector, 2.0) - 1.0));
    const double match = value - chroma;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    if (hue_sector < 1.0) {
        red = chroma; green = x;
    } else if (hue_sector < 2.0) {
        red = x; green = chroma;
    } else if (hue_sector < 3.0) {
        green = chroma; blue = x;
    } else if (hue_sector < 4.0) {
        green = x; blue = chroma;
    } else if (hue_sector < 5.0) {
        red = x; blue = chroma;
    } else {
        red = chroma; blue = x;
    }
    return {red + match, green + match, blue + match};
}

void apply_quantization(Image& image, const QuantizationConfig& quantization,
                        const std::atomic_bool* cancel) {
    throw_if_cancelled(cancel);
    if (!quantization.enabled || quantization.mix <= 0.0) {
        return;
    }
    const double amount = quantization.mix;
    for (int y = 0; y < image.height; ++y) {
        throw_if_cancelled(cancel);
        for (int x = 0; x < image.width; ++x) {
            Color color = load_color(image, x, y);
            Color quantized = color;
            switch (quantization.mode) {
                case QuantizationMode::Rgb:
                    quantized.r = quantize_value(color.r, quantization.levels);
                    quantized.g = quantize_value(color.g, quantization.levels);
                    quantized.b = quantize_value(color.b, quantization.levels);
                    break;
                case QuantizationMode::Luminance: {
                    const double luminance = 0.2126 * color.r
                                             + 0.7152 * color.g
                                             + 0.0722 * color.b;
                    const double quantized_luminance = quantize_value(
                        luminance, quantization.levels);
                    if (luminance > 1.0e-12) {
                        const double scale = quantized_luminance / luminance;
                        quantized.r *= scale;
                        quantized.g *= scale;
                        quantized.b *= scale;
                    } else {
                        quantized.r = quantized.g = quantized.b = 0.0;
                    }
                    break;
                }
                case QuantizationMode::Hue: {
                    const auto hsv = rgb_to_hsv(color.r, color.g, color.b);
                    const double quantized_hue = wrap_unit(
                        std::round(hsv[0] * quantization.levels)
                        / static_cast<double>(quantization.levels));
                    const auto rgb = hsv_to_rgb(quantized_hue, hsv[1], hsv[2]);
                    quantized.r = rgb[0];
                    quantized.g = rgb[1];
                    quantized.b = rgb[2];
                    break;
                }
            }
            color.r = mix_value(color.r, quantized.r, amount);
            color.g = mix_value(color.g, quantized.g, amount);
            color.b = mix_value(color.b, quantized.b, amount);
            store_color(image, x, y, color);
        }
    }
}

void copy_pixel(Image& image, int source_x, int source_y,
                int destination_x, int destination_y) {
    const std::size_t source = pixel_offset_unchecked(image, source_x, source_y);
    const std::size_t destination = pixel_offset_unchecked(
        image, destination_x, destination_y);
    for (std::size_t channel = 0U; channel < 4U; ++channel) {
        image.pixels[destination + channel] = image.pixels[source + channel];
    }
}

void mirror_left_to_right(Image& image, const std::atomic_bool* cancel) {
    for (int y = 0; y < image.height; ++y) {
        throw_if_cancelled(cancel);
        for (int x = (image.width + 1) / 2; x < image.width; ++x) {
            copy_pixel(image, image.width - 1 - x, y, x, y);
        }
    }
}

void mirror_right_to_left(Image& image, const std::atomic_bool* cancel) {
    for (int y = 0; y < image.height; ++y) {
        throw_if_cancelled(cancel);
        for (int x = 0; x < image.width / 2; ++x) {
            copy_pixel(image, image.width - 1 - x, y, x, y);
        }
    }
}

void mirror_top_to_bottom(Image& image, const std::atomic_bool* cancel) {
    for (int y = (image.height + 1) / 2; y < image.height; ++y) {
        throw_if_cancelled(cancel);
        for (int x = 0; x < image.width; ++x) {
            copy_pixel(image, x, image.height - 1 - y, x, y);
        }
    }
}

void mirror_bottom_to_top(Image& image, const std::atomic_bool* cancel) {
    for (int y = 0; y < image.height / 2; ++y) {
        throw_if_cancelled(cancel);
        for (int x = 0; x < image.width; ++x) {
            copy_pixel(image, x, image.height - 1 - y, x, y);
        }
    }
}

void flip_horizontal(Image& image, const std::atomic_bool* cancel) {
    for (int y = 0; y < image.height; ++y) {
        throw_if_cancelled(cancel);
        for (int x = 0; x < image.width / 2; ++x) {
            const std::size_t first = pixel_offset_unchecked(image, x, y);
            const std::size_t second = pixel_offset_unchecked(
                image, image.width - 1 - x, y);
            for (std::size_t channel = 0U; channel < 4U; ++channel) {
                std::swap(image.pixels[first + channel],
                          image.pixels[second + channel]);
            }
        }
    }
}

void flip_vertical(Image& image, const std::atomic_bool* cancel) {
    for (int y = 0; y < image.height / 2; ++y) {
        throw_if_cancelled(cancel);
        const int opposite = image.height - 1 - y;
        for (int x = 0; x < image.width; ++x) {
            const std::size_t first = pixel_offset_unchecked(image, x, y);
            const std::size_t second = pixel_offset_unchecked(image, x, opposite);
            for (std::size_t channel = 0U; channel < 4U; ++channel) {
                std::swap(image.pixels[first + channel],
                          image.pixels[second + channel]);
            }
        }
    }
}

void apply_layer_transform(Image& image,
                           const LayerTransformConfig& transform,
                           const std::atomic_bool* cancel) {
    switch (transform.mirror) {
        case MirrorMode::None:
            break;
        case MirrorMode::LeftToRight:
            mirror_left_to_right(image, cancel);
            break;
        case MirrorMode::RightToLeft:
            mirror_right_to_left(image, cancel);
            break;
        case MirrorMode::TopToBottom:
            mirror_top_to_bottom(image, cancel);
            break;
        case MirrorMode::BottomToTop:
            mirror_bottom_to_top(image, cancel);
            break;
        case MirrorMode::FourWay:
            mirror_left_to_right(image, cancel);
            mirror_top_to_bottom(image, cancel);
            break;
    }
    if (transform.flip_horizontal) {
        flip_horizontal(image, cancel);
    }
    if (transform.flip_vertical) {
        flip_vertical(image, cancel);
    }
}

} // namespace

namespace {

bool render_frame_at_timeline_sample_cancellable(
    const RenderConfig& config, const TimelineSample& timeline,
    Image& destination, const std::atomic_bool* cancel,
    bool configuration_already_validated, std::string* error) {
    try {
        throw_if_cancelled(cancel);
        if (!configuration_already_validated) {
            const ValidationResult validation = validate_impl(config, false);
            if (!validation.ok) {
                set_error(error, validation.message);
                return false;
            }
        }
        if (!std::isfinite(timeline.normalized_phase)) {
            set_error(error, "Normalized render phase must be finite.");
            return false;
        }

        const double loop_phase =
            kTau * wrap_unit(timeline.normalized_phase);
        const MotionClockState motion_clock =
            prepare_motion_clock(config, loop_phase);
        Image current;
        Image scratch;
        Image auxiliary;
        generate_base_image(config, loop_phase, motion_clock, timeline.music,
                            current, cancel);

        const auto apply_effect_stage = [&](EffectSpace stage) {
            for (const EffectConfig& authored_effect : config.effects) {
                throw_if_cancelled(cancel);
                if (authored_effect.space != stage) {
                    continue;
                }
                EffectConfig effect = authored_effect;
                if (config.audio_reactive.enabled
                    && config.audio_reactive.effects_enabled
                    && (!config.audio_reactive.synchronized_only
                        || effect.synchronized)) {
                    effect.intensity *= std::max(
                        0.0, 1.0 + config.audio_reactive.effect_amount
                                       * music_feature_value(
                                           timeline.music,
                                           config.audio_reactive.effect_source));
                }
                if (!effect_has_render_work(effect)) continue;
                const double phase = effect_phase(
                    config, effect, loop_phase, motion_clock);
                if (effect.type == EffectType::Glow) {
                    apply_glow(current, scratch, auxiliary, effect, phase, cancel);
                } else if (effect.type == EffectType::BlockScale) {
                    apply_block_scale(current, scratch, effect, phase,
                                      config.block_size, cancel);
                    current.pixels.swap(scratch.pixels);
                } else {
                    apply_coordinate_effect(current, scratch, effect, phase, cancel);
                    current.pixels.swap(scratch.pixels);
                }
            }
        };

        // Effects retain their relative order inside each explicit stage.
        // Texture effects alter the image painted onto a surface; mapped-object
        // effects run later and therefore move/deform the complete silhouette.
        apply_effect_stage(EffectSpace::Texture);

        // Curvature zero is the neutral setting for 3D surface mappings. The
        // surface's visibility mask and lighting must not crop or shade the
        // planar source in that state. Plane mapping remains active whenever its
        // configured phase or per-loop rotation can produce a 2D rotation.
        if (surface_has_render_work(config.surface)) {
            if (config.surface.mapping == SurfaceMapping::CustomObj) {
                // OBJ mapping builds its transactional mapped image locally.
                // Release the no-longer-needed effect scratch allocation so
                // that local image occupies the surface-work buffer already
                // included by central peak-memory validation.
                scratch = Image{};
            }
            if (!apply_surface_mapping(current, scratch, config.surface,
                                       loop_phase, error, cancel)) {
                return false;
            }
            current.pixels.swap(scratch.pixels);
        }
        // The layer transform defines the final canvas orientation. Applying
        // mapped-object effects afterward keeps their centers in honest screen
        // coordinates (including with mirrors/flips) and lets Shake move the
        // already transformed primitive as one object.
        apply_layer_transform(current, config.transform, cancel);
        apply_effect_stage(EffectSpace::Surface);
        apply_quantization(current, config.quantization, cancel);
        throw_if_cancelled(cancel);

        destination.width = current.width;
        destination.height = current.height;
        destination.pixels.swap(current.pixels);
        set_error(error, std::string{});
        return true;
    } catch (const RenderCancelled&) {
        set_error(error, "Rendering was cancelled; destination was unchanged.");
        return false;
    } catch (const std::bad_alloc&) {
        set_error(error, "The renderer could not allocate its validated working buffers.");
        return false;
    } catch (const std::exception& exception) {
        set_error(error, std::string("Rendering failed: ") + exception.what());
        return false;
    } catch (...) {
        set_error(error, "Rendering failed with an unknown exception.");
        return false;
    }
}

} // namespace

namespace detail {
namespace {

bool prepare_frame_for_backend_timeline(const RenderConfig& config,
                                        const TimelineSample& timeline,
                                        PreparedFrame& prepared,
                                        std::string* error) {
    if (!std::isfinite(timeline.normalized_phase)) {
        set_error(error, "Normalized render phase must be finite.");
        return false;
    }
    const ValidationResult validation = validate_impl(config, false);
    if (!validation.ok) {
        set_error(error, validation.message);
        return false;
    }

    PreparedFrame candidate;
    candidate.loop_phase = kTau * wrap_unit(timeline.normalized_phase);
    const MotionClockState motion_clock =
        prepare_motion_clock(config, candidate.loop_phase);
    candidate.global_motion_phase = motion_clock.global_phase;
    candidate.spatial_swings.reserve(motion_clock.spatial_swing_count);
    for (std::size_t index = 0U;
         index < motion_clock.spatial_swing_count; ++index) {
        const SpatialSwingSample& swing = motion_clock.spatial_swings[index];
        candidate.spatial_swings.push_back(
            {swing.center_x, swing.center_y, swing.radius,
             swing.contribution});
    }

    const double wave_response =
        config.audio_reactive.enabled
                && config.audio_reactive.waves_enabled
            ? music_feature_value(timeline.music,
                                  config.audio_reactive.wave_source)
            : 0.0;
    candidate.waves.reserve(config.waves.size());
    for (const WaveConfig& wave : config.waves) {
        if (!wave.enabled) {
            continue;
        }
        double amplitude = wave.amplitude;
        if (config.audio_reactive.enabled
            && config.audio_reactive.waves_enabled
            && (!config.audio_reactive.synchronized_only
                || wave.synchronized)) {
            amplitude *= std::max(
                0.0, 1.0 + config.audio_reactive.wave_amount * wave_response);
        }
        candidate.waves.push_back(
            {wave.x_percent * 0.01 * static_cast<double>(config.width),
             wave.y_percent * 0.01 * static_cast<double>(config.height),
             amplitude, wave.spatial_frequency,
             radians(wave.phase_degrees), wave.direction,
             wave.cycles_per_loop, wave.synchronized});
    }

    if (config.audio_reactive.enabled
        && config.audio_reactive.color_enabled) {
        candidate.audio_hue_shift_degrees =
            config.audio_reactive.color_amount_degrees
            * music_feature_value(timeline.music,
                                  config.audio_reactive.color_source);
    }

    const std::vector<Color> palette =
        prepare_starting_palette(config.palette);
    candidate.starting_palette.reserve(palette.size());
    for (const Color& color : palette) {
        candidate.starting_palette.push_back(
            {color.r, color.g, color.b, color.a});
    }

    candidate.effects.reserve(config.effects.size());
    for (const EffectConfig& authored : config.effects) {
        EffectConfig effect = authored;
        if (config.audio_reactive.enabled
            && config.audio_reactive.effects_enabled
            && (!config.audio_reactive.synchronized_only
                || effect.synchronized)) {
            effect.intensity *= std::max(
                0.0, 1.0 + config.audio_reactive.effect_amount
                               * music_feature_value(
                                     timeline.music,
                                     config.audio_reactive.effect_source));
        }
        if (!effect_has_render_work(effect)) {
            continue;
        }
        candidate.effects.push_back(
            {effect.type, effect.space, effect.edge_mode,
             effect_phase(config, effect, candidate.loop_phase,
                          motion_clock),
             effect.intensity, effect.magnitude, effect.frequency,
             effect.secondary, effect.center_x, effect.center_y,
             radians(effect.angle_degrees), effect.radius_pixels,
             effect.threshold, effect.soft_knee, effect.area_radius});
    }

    prepared = std::move(candidate);
    set_error(error, std::string{});
    return true;
}

} // namespace

bool prepare_frame_for_backend_at_phase(const RenderConfig& config,
                                        double normalized_phase,
                                        PreparedFrame& prepared,
                                        std::string* error) {
    TimelineSample timeline;
    timeline.normalized_phase = normalized_phase;
    if (config.clock.mode == ClockMode::Music
        && config.clock.music.duration_seconds > 0.0
        && std::isfinite(normalized_phase)) {
        timeline.music = music_features_at(
            config.clock.music,
            wrap_unit(normalized_phase) * config.clock.music.duration_seconds);
    }
    return prepare_frame_for_backend_timeline(config, timeline, prepared,
                                              error);
}

bool prepare_frame_for_backend(const RenderConfig& config, int frame_index,
                               PreparedFrame& prepared,
                               std::string* error) {
    return prepare_frame_for_backend_timeline(
        config, resolve_timeline_sample(config, frame_index), prepared, error);
}

} // namespace detail

bool render_frame_at_phase_cancellable(const RenderConfig& config,
                                       double normalized_phase,
                                       Image& destination,
                                       const std::atomic_bool* cancel,
                                       std::string* error) {
    TimelineSample direct;
    direct.normalized_phase = normalized_phase;
    if (config.clock.mode == ClockMode::Music
        && config.clock.music.duration_seconds > 0.0
        && std::isfinite(normalized_phase)) {
        direct.music = music_features_at(
            config.clock.music,
            wrap_unit(normalized_phase) * config.clock.music.duration_seconds);
    }
    return render_frame_at_timeline_sample_cancellable(
        config, direct, destination, cancel, false, error);
}

bool render_frame_at_phase(const RenderConfig& config, double normalized_phase,
                           Image& destination, std::string* error) {
    return render_frame_at_phase_cancellable(config, normalized_phase,
                                             destination, nullptr, error);
}

bool render_frame_cancellable(const RenderConfig& config, int frame_index,
                              Image& destination,
                              const std::atomic_bool* cancel,
                              std::string* error) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
        set_error(error, "Rendering was cancelled; destination was unchanged.");
        return false;
    }
    const ValidationResult validation = validate_impl(config, false);
    if (!validation.ok) {
        set_error(error, validation.message);
        return false;
    }
    const TimelineSample timeline = resolve_timeline_sample(config,
                                                            frame_index);
    return render_frame_at_timeline_sample_cancellable(
        config, timeline, destination, cancel, true, error);
}

bool render_frame(const RenderConfig& config, int frame_index,
                  Image& destination, std::string* error) {
    return render_frame_cancellable(config, frame_index, destination, nullptr,
                                    error);
}

} // namespace pvt
