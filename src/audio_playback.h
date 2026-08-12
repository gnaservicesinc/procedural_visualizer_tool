#ifndef PVT_AUDIO_PLAYBACK_H
#define PVT_AUDIO_PLAYBACK_H

#include <memory>
#include <string>

namespace pvt::audio {

// Small device-playback wrapper around the same decoder already used by the
// offline music analyzer. It deliberately plays one source at a time: the GUI
// binds it only to the project-global Music clock, never to individual layers.
class AudioPlayback final {
public:
    AudioPlayback();
    ~AudioPlayback();

    AudioPlayback(const AudioPlayback&) = delete;
    AudioPlayback& operator=(const AudioPlayback&) = delete;

    bool start(const std::string& path, double position_seconds, bool loop,
               std::string* error = nullptr);
    void stop();
    bool is_playing() const noexcept;
    double position_seconds() const noexcept;
    void set_volume(double volume) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pvt::audio

#endif
