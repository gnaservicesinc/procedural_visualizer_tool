#ifndef PVT_LIVE_AUDIO_CAPTURE_H
#define PVT_LIVE_AUDIO_CAPTURE_H

#include "procedural_visualizer_tool.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pvt::audio {

// Capture-device identities are deliberately runtime-only. Projects persist a
// logical endpoint and optional human-readable match hint, never one of these
// backend IDs or any captured samples.
struct LiveAudioDevice {
    std::string name;
    bool is_default = false;
    // Opaque identity from the current miniaudio enumeration. It exists only
    // to distinguish equal display names while the host binds a logical role;
    // projects and portable setup records must never persist it.
    std::string runtime_id;
    std::string display_name;
};

// Finds a non-default capture device without touching hardware. Opaque runtime
// IDs are preferred; a legacy display-name selection is accepted only when it
// identifies exactly one device.
std::optional<std::size_t> find_live_audio_device(
    const std::vector<LiveAudioDevice>& devices,
    const std::string& runtime_id_or_name,
    std::string* error = nullptr);

struct LiveBeatTiming {
    double anchor_seconds = 0.0;
    // Unwrapped musical position. The first accepted beat is zero and time
    // continues to advance at detected_bpm even when a later onset is missed.
    double position = 0.0;
    // The wrapped position is retained for meters and calibration UI.
    double phase = 0.0;
};

// Pure timing helpers shared by capture and the Live clock router. The route
// divisor is intentionally supplied as authored/reference beats per visual
// loop; detected BPM belongs only in live_beat_timing() and must not be used in
// both numerator and divisor (which would reduce the route to elapsed time).
LiveBeatTiming live_beat_timing(std::uint64_t detected_beat_count,
                                std::uint64_t current_frame,
                                std::uint64_t anchor_frame,
                                std::uint32_t sample_rate,
                                double detected_bpm) noexcept;
std::optional<double> live_beat_route_phase(
    std::uint64_t detected_beat_count,
    double beat_position,
    double reference_beats_per_loop,
    // Positive latency advances the route because captured audio arrived
    // after the physical beat; negative latency delays an early source.
    double signed_latency_beats = 0.0,
    ClockInterpolation interpolation = ClockInterpolation::Linear) noexcept;

// A backend can report elapsed callback frames with no captured samples during
// a device dropout. Those frames must not advance the analyzer's sample clock:
// Live's holdover path already extrapolates from the last valid snapshot.
std::uint64_t live_audio_frame_clock_increment(
    std::uint32_t callback_frames, bool has_input_samples) noexcept;
double live_audio_extrapolated_beat_position(
    double beat_position_at_last_callback, double beats_per_minute,
    double last_valid_callback_age_seconds) noexcept;
// A zero/very short authored holdover must still tolerate ordinary callback
// and GUI scheduling jitter. This floor is intentionally much shorter than
// LiveAudioSnapshot::receiving's coarse 500 ms status window; route loss is
// measured from the last valid callback, not from that status transition.
inline constexpr int kLiveAudioNormalCallbackToleranceMilliseconds = 100;
bool live_audio_callback_within_holdover(
    double last_valid_callback_age_seconds,
    int holdover_milliseconds) noexcept;

struct LiveAudioSnapshot {
    struct SpectrumBand {
        double frequency_hz = 0.0;
        float level = 0.0F;
    };

    struct FrequencyStream {
        std::string uuid;
        MusicFeatureSample features;
        double detected_bpm = 0.0;
        std::uint64_t beat_count = 0U;
        double beat_anchor_seconds = 0.0;
        double beat_position = 0.0;
        double beat_phase = 0.0;
    };

    MusicFeatureSample features;
    double detected_bpm = 0.0;
    std::uint64_t beat_count = 0U;
    double beat_anchor_seconds = 0.0;
    double beat_position = 0.0;
    double beat_phase = 0.0;
    double stream_seconds = 0.0;
    // Monotonic wall-clock age of the last callback that contained samples.
    // Negative means capture has not delivered a valid callback yet.
    double last_valid_callback_age_seconds = -1.0;
    double estimated_input_latency_ms = 0.0;
    std::uint64_t callback_dropouts = 0U;
    bool receiving = false;
    bool gate_open = true;
    std::vector<SpectrumBand> spectrum;
    std::vector<FrequencyStream> frequency_streams;
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
    bool start(const std::string& runtime_device_id_or_name = {},
               std::uint32_t requested_period_frames = 128U,
               std::string* error = nullptr);
    void stop() noexcept;
    bool is_running() const noexcept;
    LiveAudioSnapshot snapshot() const;

    // Must be called while stopped. The next start prepares all coefficients
    // and stream state before the callback becomes visible to the backend.
    bool set_processing_config(const AudioInputProcessingConfig& config,
                               std::string* error = nullptr);
    // Machine-local performance calibration. Must be called while stopped.
    bool set_gate_config(bool enabled, double threshold_db,
                         double attack_milliseconds,
                         double release_milliseconds,
                         std::string* error = nullptr);

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
