#ifndef PVT_AUDIO_PLAYBACK_H
#define PVT_AUDIO_PLAYBACK_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pvt::audio {

struct PlaybackTrack {
    std::string path;
    // Source position at the requested project-timeline start.
    double source_position_seconds = 0.0;
    // Source seconds advanced for each project-timeline second.
    double playback_rate = 1.0;
    bool loop = false;
    // Zero means no extra limit. One-shot tracks use their remaining audible
    // project-timeline duration here and then contribute silence.
    double stop_after_seconds = 0.0;
};

// Device-playback wrapper around the same decoder used by the offline analyzer.
// Constant-rate voices are mixed in the real-time callback, which lets project
// and layer clock sources audition together without creating a temporary file.
class AudioPlayback final {
public:
    AudioPlayback();
    ~AudioPlayback();

    AudioPlayback(const AudioPlayback&) = delete;
    AudioPlayback& operator=(const AudioPlayback&) = delete;

    bool start(const std::string& path, double position_seconds, bool loop,
               std::string* error = nullptr);
    bool start_mix(const std::vector<PlaybackTrack>& tracks,
                   double timeline_position_seconds,
                   std::string* error = nullptr);
    // Decode through the same voices without opening a playback device. One
    // caller owns read_stream; stop/reconfigure only after that caller joins.
    bool prepare_stream(const std::vector<PlaybackTrack>& tracks,
                        std::string* error = nullptr);
    void read_stream(float* stereo, std::uint32_t frames) noexcept;
    void stop();
    bool is_playing() const noexcept;
    double position_seconds() const noexcept;
    void set_volume(double volume) noexcept;

    void enable_remote_audio(bool enabled) noexcept;
    bool read_remote_audio(std::uint8_t* pcm) noexcept;

private:
    bool prepare_mix(const std::vector<PlaybackTrack>& tracks,
                     double timeline_position_seconds, bool open_device,
                     std::string* error);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Writes a format-bounded stereo float32 WAV mix for native movie export. The caller
// supplies a private temporary destination; partial output is removed on any
// decode, write, or cancellation failure.
bool write_mix_wav(const std::vector<PlaybackTrack>& tracks,
                   double duration_seconds, const std::string& destination,
                   const std::atomic_bool* cancel = nullptr,
                   std::string* error = nullptr);

} // namespace pvt::audio

#endif
