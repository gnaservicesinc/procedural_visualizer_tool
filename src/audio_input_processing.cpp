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
        if (!usable_frequency(config.low_pass_hz, sample_rate)) {
            return fail(error, "The low-pass cutoff must be below the audio Nyquist frequency.");
        }
        prepared.push_back(low_pass(config.low_pass_hz, sample_rate));
    }
    if (config.equalizer_enabled) {
        if (config.equalizer_bands.size() > kMaximumAudioEqualizerBands) {
            return fail(error, "The graphical equalizer exceeds its real-time band limit.");
        }
        for (const auto& band : config.equalizer_bands) {
            if (!usable_frequency(band.frequency_hz, sample_rate)
                || !std::isfinite(band.gain_db)
                || band.gain_db < -24.0 || band.gain_db > 24.0) {
                return fail(error, "An equalizer band has an unsupported frequency or gain.");
            }
            if (std::fabs(band.gain_db) > 1.0e-12) {
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

} // namespace pvt::audio
