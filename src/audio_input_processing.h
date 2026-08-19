#ifndef PVT_AUDIO_INPUT_PROCESSING_H
#define PVT_AUDIO_INPUT_PROCESSING_H

#include "procedural_visualizer_tool.h"

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

} // namespace pvt::audio

#endif
