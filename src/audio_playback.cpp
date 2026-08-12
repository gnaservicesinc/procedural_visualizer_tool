#include "audio_playback.h"

#include "miniaudio.h"
#include "path_utf8.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <new>

namespace pvt::audio {
namespace {

constexpr ma_uint32 kPlaybackChannels = 2U;
constexpr ma_uint32 kPlaybackSampleRate = 48000U;

bool fail(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
}

std::string miniaudio_error(const char* action, ma_result result) {
    const char* description = ma_result_description(result);
    return std::string(action) + (description != nullptr ? ": " : ".")
           + (description != nullptr ? description : "");
}

} // namespace

struct AudioPlayback::Impl {
    ma_decoder decoder{};
    ma_device device{};
    bool decoder_initialized = false;
    bool device_initialized = false;
    std::atomic_bool started{false};
    std::atomic_bool loop{false};
    std::atomic<ma_uint64> cursor_frames{0U};
    std::atomic<float> volume{1.0F};

    static void data_callback(ma_device* device, void* output,
                              const void*, ma_uint32 requested_frames) {
        auto* self = static_cast<Impl*>(device != nullptr
                                           ? device->pUserData : nullptr);
        if (self == nullptr || output == nullptr || requested_frames == 0U) {
            return;
        }
        auto* samples = static_cast<float*>(output);
        ma_silence_pcm_frames(samples, requested_frames, ma_format_f32,
                              kPlaybackChannels);
        if (!self->decoder_initialized
            || !self->started.load(std::memory_order_relaxed)) {
            return;
        }

        ma_uint64 completed = 0U;
        bool loop_attempted = false;
        while (completed < requested_frames) {
            ma_uint64 decoded = 0U;
            const ma_result result = ma_decoder_read_pcm_frames(
                &self->decoder,
                ma_offset_pcm_frames_ptr_f32(samples, completed,
                                              kPlaybackChannels),
                static_cast<ma_uint64>(requested_frames) - completed,
                &decoded);
            if (decoded > 0U) {
                completed += decoded;
                self->cursor_frames.fetch_add(decoded,
                                              std::memory_order_relaxed);
                loop_attempted = false;
            }
            if (completed >= requested_frames) break;
            if ((result != MA_SUCCESS && result != MA_AT_END) || decoded > 0U) {
                if (result != MA_SUCCESS && result != MA_AT_END) break;
                continue;
            }
            if (!self->loop.load(std::memory_order_relaxed) || loop_attempted
                || ma_decoder_seek_to_pcm_frame(&self->decoder, 0U)
                       != MA_SUCCESS) {
                self->started.store(false, std::memory_order_relaxed);
                break;
            }
            self->cursor_frames.store(0U, std::memory_order_relaxed);
            loop_attempted = true;
        }

        const float gain = std::clamp(
            self->volume.load(std::memory_order_relaxed), 0.0F, 1.0F);
        if (gain != 1.0F && completed > 0U) {
            ma_apply_volume_factor_pcm_frames_f32(
                samples, completed, kPlaybackChannels, gain);
        }
    }

    void uninitialize() noexcept {
        started.store(false, std::memory_order_relaxed);
        if (device_initialized) {
            (void)ma_device_stop(&device);
            ma_device_uninit(&device);
            device_initialized = false;
        }
        if (decoder_initialized) {
            (void)ma_decoder_uninit(&decoder);
            decoder_initialized = false;
        }
        cursor_frames.store(0U, std::memory_order_relaxed);
    }

    ~Impl() { uninitialize(); }
};

AudioPlayback::AudioPlayback() : impl_(std::make_unique<Impl>()) {}
AudioPlayback::~AudioPlayback() = default;

bool AudioPlayback::start(const std::string& path, double position_seconds,
                          bool should_loop, std::string* error) {
    if (error != nullptr) error->clear();
    if (path.empty() || !std::isfinite(position_seconds)
        || position_seconds < 0.0) {
        return fail(error, "Audio playback requires a source and a finite, nonnegative position.");
    }
    impl_->uninitialize();

    ma_decoder_config decoder_config = ma_decoder_config_init(
        ma_format_f32, kPlaybackChannels, kPlaybackSampleRate);
#if defined(_WIN32)
    const std::filesystem::path native = detail::path_from_utf8(path);
    const ma_result decoder_result = ma_decoder_init_file_w(
        native.c_str(), &decoder_config, &impl_->decoder);
#else
    const ma_result decoder_result = ma_decoder_init_file(
        path.c_str(), &decoder_config, &impl_->decoder);
#endif
    if (decoder_result != MA_SUCCESS) {
        return fail(error, miniaudio_error("Could not open the project music for playback",
                                           decoder_result));
    }
    impl_->decoder_initialized = true;

    const long double requested =
        static_cast<long double>(position_seconds)
        * static_cast<long double>(kPlaybackSampleRate);
    const ma_uint64 frame = requested
                                    >= static_cast<long double>(
                                        std::numeric_limits<ma_uint64>::max())
                                ? std::numeric_limits<ma_uint64>::max()
                                : static_cast<ma_uint64>(requested);
    const ma_result seek_result =
        ma_decoder_seek_to_pcm_frame(&impl_->decoder, frame);
    if (seek_result != MA_SUCCESS) {
        impl_->uninitialize();
        return fail(error, miniaudio_error("Could not seek the project music",
                                           seek_result));
    }
    impl_->cursor_frames.store(frame, std::memory_order_relaxed);
    impl_->loop.store(should_loop, std::memory_order_relaxed);

    ma_device_config device_config = ma_device_config_init(ma_device_type_playback);
    device_config.playback.format = ma_format_f32;
    device_config.playback.channels = kPlaybackChannels;
    device_config.sampleRate = kPlaybackSampleRate;
    device_config.dataCallback = &Impl::data_callback;
    device_config.pUserData = impl_.get();
    const ma_result device_result = ma_device_init(
        nullptr, &device_config, &impl_->device);
    if (device_result != MA_SUCCESS) {
        impl_->uninitialize();
        return fail(error, miniaudio_error("Could not open the default audio output",
                                           device_result));
    }
    impl_->device_initialized = true;
    impl_->started.store(true, std::memory_order_relaxed);
    const ma_result start_result = ma_device_start(&impl_->device);
    if (start_result != MA_SUCCESS) {
        impl_->uninitialize();
        return fail(error, miniaudio_error("Could not start audio playback",
                                           start_result));
    }
    return true;
}

void AudioPlayback::stop() {
    if (impl_ != nullptr) impl_->uninitialize();
}

bool AudioPlayback::is_playing() const noexcept {
    return impl_ != nullptr
           && impl_->started.load(std::memory_order_relaxed);
}

double AudioPlayback::position_seconds() const noexcept {
    if (impl_ == nullptr) return 0.0;
    return static_cast<double>(
               impl_->cursor_frames.load(std::memory_order_relaxed))
           / static_cast<double>(kPlaybackSampleRate);
}

void AudioPlayback::set_volume(double requested) noexcept {
    if (impl_ == nullptr || !std::isfinite(requested)) return;
    impl_->volume.store(
        static_cast<float>(std::clamp(requested, 0.0, 1.0)),
        std::memory_order_relaxed);
}

} // namespace pvt::audio
