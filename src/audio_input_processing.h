#ifndef PVT_AUDIO_INPUT_PROCESSING_H
#define PVT_AUDIO_INPUT_PROCESSING_H

#include "procedural_visualizer_tool.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pvt::audio {

// Prepared, allocation-free sample processing. Construction/configuration is
// kept off real-time callback threads; process() mutates only owned DSP state.
class AudioInputProcessor final {
public:
    bool configure(const AudioInputProcessingConfig& config,
                   double sample_rate, std::string* error = nullptr);
    float process(float sample) noexcept;
    void reset() noexcept;

    // Public only so the translation unit can use compact coefficient factory
    // helpers; this is a private, non-installed header.
    struct Biquad {
        double b0 = 1.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
        double z1 = 0.0;
        double z2 = 0.0;

        float process(float sample) noexcept;
        void reset() noexcept { z1 = z2 = 0.0; }
    };

private:
    std::vector<Biquad> filters_;
};

class AudioFrequencyRangeProcessor final {
public:
    bool configure(double low_hz, double high_hz, double sample_rate,
                   std::string* error = nullptr);
    float process(float sample) noexcept;
    void reset() noexcept;

private:
    AudioInputProcessor processor_;
};

// Lightweight causal gate used by Live capture after filtering/EQ and input
// gain. Configuration happens off the audio thread; process() is allocation-
// free and owns all of its detector state.
class AudioNoiseGate final {
public:
    bool configure(bool enabled, double threshold_db,
                   double attack_milliseconds, double release_milliseconds,
                   double sample_rate, std::string* error = nullptr);
    // Extended channel-strip controls. The legacy configure() overload is
    // retained and delegates here with zero hold and zero hysteresis, so
    // existing callers preserve their current behavior.
    bool configure_advanced(bool enabled, double threshold_db,
                            double attack_milliseconds,
                            double hold_milliseconds,
                            double release_milliseconds,
                            double hysteresis_db,
                            double sample_rate,
                            std::string* error = nullptr);
    float process(float sample) noexcept;
    void reset() noexcept;
    bool is_open() const noexcept { return open_; }

private:
    bool enabled_ = false;
    bool open_ = true;
    double open_threshold_linear_ = 0.0;
    double close_threshold_linear_ = 0.0;
    double attack_coefficient_ = 0.0;
    double release_coefficient_ = 0.0;
    double envelope_ = 0.0;
    double gain_ = 1.0;
    std::uint64_t hold_samples_ = 0U;
    std::uint64_t hold_remaining_ = 0U;
};

} // namespace pvt::audio

#endif
