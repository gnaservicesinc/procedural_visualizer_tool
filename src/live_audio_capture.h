#ifndef PVT_LIVE_AUDIO_CAPTURE_H
#define PVT_LIVE_AUDIO_CAPTURE_H

#include "procedural_visualizer_tool.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pvt::audio {

// Capture-device identities are deliberately runtime-only. Projects persist a
// logical endpoint and optional human-readable match hint, never one of these
// backend IDs or any captured samples.
struct LiveAudioDevice {
    std::string name;
    bool is_default = false;
};

struct LiveAudioSnapshot {
    MusicFeatureSample features;
    double detected_bpm = 0.0;
    double beat_phase = 0.0;
    double stream_seconds = 0.0;
    double estimated_input_latency_ms = 0.0;
    std::uint64_t callback_dropouts = 0U;
    bool receiving = false;
};

// A small, allocation-free incremental analyzer runs in miniaudio's capture
// callback. It is intentionally causal: the snapshot reflects only samples
// already received and is never written into MusicAnalysis or a project.
class LiveAudioCapture final {
public:
    LiveAudioCapture();
    ~LiveAudioCapture();

    LiveAudioCapture(const LiveAudioCapture&) = delete;
    LiveAudioCapture& operator=(const LiveAudioCapture&) = delete;

    std::vector<LiveAudioDevice> devices(std::string* error = nullptr) const;
    bool start(const std::string& runtime_device_name = {},
               std::uint32_t requested_period_frames = 128U,
               std::string* error = nullptr);
    void stop() noexcept;
    bool is_running() const noexcept;
    LiveAudioSnapshot snapshot() const noexcept;

    // Gain and sensitivity are live-performance controls, not destructive
    // edits to incoming audio. Both are lock-free and callback safe.
    void set_gain(double gain) noexcept;
    void set_sensitivity(double sensitivity) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pvt::audio

#endif
