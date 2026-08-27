#include "audio_input_processing.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace pvt::audio {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kButterworthQ = 0.70710678118654752440;
constexpr double kEqualizerQ = 1.0;

bool fail(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
}

bool usable_frequency(double value, double sample_rate) {
    return std::isfinite(value) && value > 0.0
        && std::isfinite(sample_rate) && sample_rate > 0.0
        && value < 0.5 * sample_rate;
}

AudioInputProcessor::Biquad low_pass(double frequency, double sample_rate) {
    const double omega = 2.0 * kPi * frequency / sample_rate;
    const double cosine = std::cos(omega);
    const double sine = std::sin(omega);
    const double alpha = sine / (2.0 * kButterworthQ);
    const double a0 = 1.0 + alpha;
    AudioInputProcessor::Biquad result;
    result.b0 = (1.0 - cosine) * 0.5 / a0;
    result.b1 = (1.0 - cosine) / a0;
    result.b2 = result.b0;
    result.a1 = -2.0 * cosine / a0;
    result.a2 = (1.0 - alpha) / a0;
    return result;
}

AudioInputProcessor::Biquad high_pass(double frequency, double sample_rate) {
    const double omega = 2.0 * kPi * frequency / sample_rate;
    const double cosine = std::cos(omega);
    const double sine = std::sin(omega);
    const double alpha = sine / (2.0 * kButterworthQ);
    const double a0 = 1.0 + alpha;
    AudioInputProcessor::Biquad result;
    result.b0 = (1.0 + cosine) * 0.5 / a0;
    result.b1 = -(1.0 + cosine) / a0;
    result.b2 = result.b0;
    result.a1 = -2.0 * cosine / a0;
    result.a2 = (1.0 - alpha) / a0;
    return result;
}

AudioInputProcessor::Biquad peaking(double frequency, double gain_db,
                                    double sample_rate) {
    const double amplitude = std::pow(10.0, gain_db / 40.0);
    const double omega = 2.0 * kPi * frequency / sample_rate;
    const double cosine = std::cos(omega);
    const double alpha = std::sin(omega) / (2.0 * kEqualizerQ);
    const double a0 = 1.0 + alpha / amplitude;
    AudioInputProcessor::Biquad result;
    result.b0 = (1.0 + alpha * amplitude) / a0;
    result.b1 = -2.0 * cosine / a0;
    result.b2 = (1.0 - alpha * amplitude) / a0;
    result.a1 = -2.0 * cosine / a0;
    result.a2 = (1.0 - alpha / amplitude) / a0;
    return result;
}

} // namespace

float AudioInputProcessor::Biquad::process(float input) noexcept {
    const double sample = static_cast<double>(input);
    const double output = b0 * sample + z1;
    z1 = b1 * sample - a1 * output + z2;
    z2 = b2 * sample - a2 * output;
    if (!std::isfinite(output) || !std::isfinite(z1) || !std::isfinite(z2)) {
        reset();
        return 0.0F;
    }
    return static_cast<float>(std::clamp(
        output, -static_cast<double>((std::numeric_limits<float>::max)()),
        static_cast<double>((std::numeric_limits<float>::max)())));
}

bool AudioInputProcessor::configure(const AudioInputProcessingConfig& config,
                                    double sample_rate, std::string* error) {
    if (error != nullptr) error->clear();
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0) {
        return fail(error, "Audio processing requires a positive sample rate.");
    }
    std::vector<Biquad> prepared;
    prepared.reserve(2U + config.equalizer_bands.size());
    if (config.high_pass_enabled) {
        if (!usable_frequency(config.high_pass_hz, sample_rate)) {
            return fail(error, "The high-pass cutoff must be below the audio Nyquist frequency.");
        }
        prepared.push_back(high_pass(config.high_pass_hz, sample_rate));
    }
    if (config.low_pass_enabled) {
        if (!std::isfinite(config.low_pass_hz) || config.low_pass_hz <= 0.0) {
            return fail(error, "The low-pass cutoff must be a positive frequency.");
        }
        // A portable cutoff above this source's Nyquist frequency is an exact
        // no-op: the source cannot contain those frequencies. This keeps the
        // gentle 20 kHz project default usable with lower-rate audio.
        if (config.low_pass_hz < 0.5 * sample_rate) {
            prepared.push_back(low_pass(config.low_pass_hz, sample_rate));
        }
    }
    if (config.equalizer_enabled) {
        if (config.equalizer_bands.size() > kMaximumAudioEqualizerBands) {
            return fail(error, "The graphical equalizer exceeds its real-time band limit.");
        }
        for (const auto& band : config.equalizer_bands) {
            if (!std::isfinite(band.frequency_hz) || band.frequency_hz <= 0.0
                || !std::isfinite(band.gain_db)
                || band.gain_db < -24.0 || band.gain_db > 24.0) {
                return fail(error, "An equalizer band has an unsupported frequency or gain.");
            }
            // A band centered outside the source spectrum cannot contribute.
            // Skip it instead of making portable EQ presets sample-rate-bound.
            if (band.frequency_hz < 0.5 * sample_rate
                && std::fabs(band.gain_db) > 1.0e-12) {
                prepared.push_back(peaking(band.frequency_hz, band.gain_db,
                                           sample_rate));
            }
        }
    }
    filters_ = std::move(prepared);
    return true;
}

float AudioInputProcessor::process(float sample) noexcept {
    for (Biquad& filter : filters_) sample = filter.process(sample);
    return sample;
}

void AudioInputProcessor::reset() noexcept {
    for (Biquad& filter : filters_) filter.reset();
}

bool AudioFrequencyRangeProcessor::configure(double low_hz, double high_hz,
                                             double sample_rate,
                                             std::string* error) {
    AudioInputProcessingConfig config;
    config.equalizer_bands.clear();
    config.frequency_streams.clear();
    config.high_pass_enabled = low_hz > 0.0;
    config.high_pass_hz = low_hz;
    // Treat a range ending at/above Nyquist as open-ended, which also keeps
    // portable 20 kHz ranges usable with low-rate source files.
    config.low_pass_enabled = high_hz < 0.5 * sample_rate;
    config.low_pass_hz = high_hz;
    if (!(std::isfinite(low_hz) && std::isfinite(high_hz)
          && low_hz >= 0.0 && high_hz > low_hz)) {
        return fail(error, "A frequency stream needs an increasing finite range.");
    }
    return processor_.configure(config, sample_rate, error);
}

float AudioFrequencyRangeProcessor::process(float sample) noexcept {
    return processor_.process(sample);
}

void AudioFrequencyRangeProcessor::reset() noexcept { processor_.reset(); }

bool AudioNoiseGate::configure(bool enabled, double threshold_db,
                               double attack_milliseconds,
                               double release_milliseconds,
                               double sample_rate, std::string* error) {
    if (error != nullptr) error->clear();
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0
        || !std::isfinite(threshold_db) || threshold_db < -96.0
        || threshold_db > 0.0
        || !std::isfinite(attack_milliseconds)
        || attack_milliseconds < 0.1 || attack_milliseconds > 1000.0
        || !std::isfinite(release_milliseconds)
        || release_milliseconds < 1.0 || release_milliseconds > 5000.0) {
        return fail(error,
                    "The live gate needs a -96 to 0 dB threshold, a 0.1 to "
                    "1000 ms attack, and a 1 to 5000 ms release.");
    }
    enabled_ = enabled;
    threshold_linear_ = std::pow(10.0, threshold_db / 20.0);
    const auto coefficient = [sample_rate](double milliseconds) {
        return std::exp(-1.0 / (0.001 * milliseconds * sample_rate));
    };
    attack_coefficient_ = coefficient(attack_milliseconds);
    release_coefficient_ = coefficient(release_milliseconds);
    reset();
    return true;
}

float AudioNoiseGate::process(float input) noexcept {
    if (!enabled_) return input;
    const double sample = static_cast<double>(input);
    const double magnitude = std::fabs(sample);
    const double detector_coefficient = magnitude > envelope_
        ? attack_coefficient_ : release_coefficient_;
    envelope_ = detector_coefficient * envelope_
        + (1.0 - detector_coefficient) * magnitude;
    const double target = envelope_ >= threshold_linear_ ? 1.0 : 0.0;
    const double gain_coefficient = target > gain_
        ? attack_coefficient_ : release_coefficient_;
    gain_ = gain_coefficient * gain_ + (1.0 - gain_coefficient) * target;
    open_ = target > 0.5;
    const double output = sample * gain_;
    if (!std::isfinite(output)) {
        reset();
        return 0.0F;
    }
    return static_cast<float>(std::clamp(
        output, -static_cast<double>((std::numeric_limits<float>::max)()),
        static_cast<double>((std::numeric_limits<float>::max)())));
}

void AudioNoiseGate::reset() noexcept {
    envelope_ = 0.0;
    gain_ = enabled_ ? 0.0 : 1.0;
    open_ = !enabled_;
}

} // namespace pvt::audio
