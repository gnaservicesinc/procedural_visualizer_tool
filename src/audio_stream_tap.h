#ifndef PVT_AUDIO_STREAM_TAP_H
#define PVT_AUDIO_STREAM_TAP_H
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
namespace pvt::audio {
// One audio producer, one GUI consumer. No allocation, lock or network access
// on the audio thread. Overflow drops incoming samples rather than blocking.
class AudioStreamTap {
public:
    void enable(bool value) noexcept { enabled_.store(value, std::memory_order_relaxed); }
    void write(const float* stereo, std::size_t frames) noexcept {
        if (!enabled_.load(std::memory_order_relaxed)) return;
        auto write = write_.load(std::memory_order_relaxed);
        const auto read = read_.load(std::memory_order_acquire);
        const auto count = std::min(frames, capacity - (write - read));
        for (std::size_t frame = 0; frame < count; ++frame) {
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const auto value = stereo[frame * 2 + channel];
                const auto sample = static_cast<std::int16_t>(std::lround(
                    (std::isfinite(value) ? std::clamp(value, -1.0F, 1.0F) : 0.0F) * 32767));
                const auto offset = ((write + frame) % capacity) * 4 + channel * 2;
                data_[offset] = static_cast<std::uint8_t>(sample & 255);
                data_[offset + 1] = static_cast<std::uint8_t>((static_cast<std::uint16_t>(sample) >> 8) & 255);
            }
        }
        write_.store(write + count, std::memory_order_release);
    }
    bool read(std::uint8_t* pcm) noexcept {
        auto read = read_.load(std::memory_order_relaxed);
        const auto write = write_.load(std::memory_order_acquire);
        if (write - read < 960) return false;
        // At most 100 ms retained; skip old frames after GUI stalls.
        if (write - read > 4800) read = write - 4800;
        for (std::size_t i = 0; i < 960; ++i)
            for (std::size_t b = 0; b < 4; ++b) pcm[i * 4 + b] = data_[((read + i) % capacity) * 4 + b];
        read_.store(read + 960, std::memory_order_release);
        return true;
    }
    void discard() noexcept { read_.store(write_.load(std::memory_order_acquire), std::memory_order_release); }
private:
    static constexpr std::size_t capacity = 9600;
    std::array<std::uint8_t, capacity * 4> data_{};
    std::atomic_size_t write_{0}, read_{0};
    std::atomic_bool enabled_{false};
};
}
#endif
