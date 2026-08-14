#include "audio_playback.h"

#include "miniaudio.h"
#include "path_utf8.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <vector>

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

ma_result init_file_decoder(const std::string& path, ma_uint32 output_rate,
                            ma_decoder* decoder) {
    ma_decoder_config config = ma_decoder_config_init(
        ma_format_f32, kPlaybackChannels, output_rate);
#if defined(_WIN32)
    const std::filesystem::path native = detail::path_from_utf8(path);
    return ma_decoder_init_file_w(native.c_str(), &config, decoder);
#else
    return ma_decoder_init_file(path.c_str(), &config, decoder);
#endif
}

bool prepare_rate_adjusted_decoder(const PlaybackTrack& track,
                                   ma_decoder& decoder, bool& initialized,
                                   const char* open_action,
                                   const char* seek_action,
                                   std::string* error) {
    ma_uint32 nominal_output_rate = kPlaybackSampleRate;
    ma_result result = init_file_decoder(
        track.path, nominal_output_rate, &decoder);
    if (result != MA_SUCCESS) {
        return fail(error, miniaudio_error(open_action, result));
    }
    initialized = true;

    ma_uint32 source_rate = decoder.converter.sampleRateIn;
    if (decoder.converter.hasResampler == MA_FALSE) {
        // A decoder whose source already matches the device rate would normally
        // bypass resampling. Reopen it at an adjacent nominal rate so that the
        // converter can carry the exact project/source timing ratio below.
        (void)ma_decoder_uninit(&decoder);
        initialized = false;
        nominal_output_rate = source_rate == kPlaybackSampleRate
                                  ? kPlaybackSampleRate + 1U
                                  : kPlaybackSampleRate;
        result = init_file_decoder(track.path, nominal_output_rate, &decoder);
        if (result != MA_SUCCESS) {
            return fail(error, miniaudio_error(open_action, result));
        }
        initialized = true;
        source_rate = decoder.converter.sampleRateIn;
    }
    if (source_rate == 0U || decoder.converter.hasResampler == MA_FALSE) {
        return fail(error,
                    "Could not prepare an exact-rate project music resampler.");
    }

    // Seek while the decoder still has its nominal source/output ratio. This
    // maps source seconds to the correct backend frame before the converter is
    // retimed for device-rate playback.
    const long double requested =
        static_cast<long double>(track.source_position_seconds)
        * static_cast<long double>(nominal_output_rate);
    const ma_uint64 frame = requested
                                >= static_cast<long double>(
                                    (std::numeric_limits<ma_uint64>::max)())
                            ? (std::numeric_limits<ma_uint64>::max)()
                            : static_cast<ma_uint64>(requested);
    result = ma_decoder_seek_to_pcm_frame(&decoder, frame);
    if (result != MA_SUCCESS) {
        return fail(error, miniaudio_error(seek_action, result));
    }

    const long double exact_ratio =
        static_cast<long double>(source_rate)
        * static_cast<long double>(track.playback_rate)
        / static_cast<long double>(kPlaybackSampleRate);
    constexpr std::uint64_t kPreferredRateDenominator = UINT64_C(10000000);
    const long double maximum_rate = static_cast<long double>(
        (std::numeric_limits<ma_uint32>::max)());
    const std::uint64_t denominator = static_cast<std::uint64_t>(
        std::min<long double>(
            static_cast<long double>(kPreferredRateDenominator),
            std::floor(maximum_rate / std::max(1.0L, exact_ratio))));
    const long double scaled_numerator = exact_ratio
                                         * static_cast<long double>(denominator);
    const std::uint64_t numerator = scaled_numerator > maximum_rate
                                        ? 0U
                                        : static_cast<std::uint64_t>(
                                              std::llround(scaled_numerator));
    if (denominator == 0U || numerator == 0U
        || numerator > (std::numeric_limits<ma_uint32>::max)()) {
        return fail(error,
                    "The exact project music playback ratio is unsupported.");
    }
    result = ma_data_converter_set_rate(
        &decoder.converter, static_cast<ma_uint32>(numerator),
        static_cast<ma_uint32>(denominator));
    if (result != MA_SUCCESS) {
        return fail(error, miniaudio_error(
            "Could not configure exact project music playback timing", result));
    }
    return true;
}

} // namespace

struct AudioPlayback::Impl {
    struct Voice {
        ma_decoder decoder{};
        bool initialized = false;
        bool active = false;
        bool loop = false;
        ma_uint64 remaining_output_frames = (std::numeric_limits<ma_uint64>::max)();

        ~Voice() {
            if (initialized) (void)ma_decoder_uninit(&decoder);
        }
    };

    std::vector<std::unique_ptr<Voice>> voices;
    ma_device device{};
    bool device_initialized = false;
    std::atomic_bool started{false};
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
        if (!self->started.load(std::memory_order_relaxed)) {
            return;
        }
        constexpr ma_uint64 kScratchFrames = 2048U;
        std::array<float, kScratchFrames * kPlaybackChannels> scratch{};
        const float voice_gain = self->voices.empty()
            ? 1.0F
            : static_cast<float>(1.0 / std::sqrt(
                  static_cast<double>(self->voices.size())));
        bool any_active = false;
        for (const auto& voice_owner : self->voices) {
            Voice& voice = *voice_owner;
            if (!voice.active || !voice.initialized) continue;
            ma_uint64 output_offset = 0U;
            bool loop_attempted = false;
            while (output_offset < requested_frames && voice.active) {
                const ma_uint64 wanted = std::min<ma_uint64>(
                    {kScratchFrames,
                     static_cast<ma_uint64>(requested_frames) - output_offset,
                     voice.remaining_output_frames});
                if (wanted == 0U) {
                    voice.active = false;
                    break;
                }
                ma_uint64 decoded = 0U;
                const ma_result result = ma_decoder_read_pcm_frames(
                    &voice.decoder, scratch.data(), wanted, &decoded);
                for (ma_uint64 frame = 0U; frame < decoded; ++frame) {
                    const ma_uint64 target = (output_offset + frame)
                                             * kPlaybackChannels;
                    const ma_uint64 source = frame * kPlaybackChannels;
                    samples[target] += scratch[source] * voice_gain;
                    samples[target + 1U] += scratch[source + 1U] * voice_gain;
                }
                output_offset += decoded;
                if (voice.remaining_output_frames
                    != (std::numeric_limits<ma_uint64>::max)()) {
                    voice.remaining_output_frames -= std::min(
                        voice.remaining_output_frames, decoded);
                }
                if (decoded > 0U) loop_attempted = false;
                if (decoded == wanted) continue;
                if ((result != MA_SUCCESS && result != MA_AT_END)
                    || !voice.loop || loop_attempted
                    || ma_decoder_seek_to_pcm_frame(&voice.decoder, 0U)
                           != MA_SUCCESS) {
                    voice.active = false;
                    break;
                }
                loop_attempted = true;
            }
            any_active = any_active || voice.active;
        }
        self->cursor_frames.fetch_add(requested_frames,
                                      std::memory_order_relaxed);
        const float gain = std::clamp(
            self->volume.load(std::memory_order_relaxed), 0.0F, 1.0F);
        const std::size_t sample_count = static_cast<std::size_t>(requested_frames)
                                         * kPlaybackChannels;
        for (std::size_t index = 0U; index < sample_count; ++index) {
            samples[index] = std::clamp(samples[index] * gain, -1.0F, 1.0F);
        }
        if (!any_active) self->started.store(false, std::memory_order_relaxed);
    }

    void uninitialize() noexcept {
        started.store(false, std::memory_order_relaxed);
        if (device_initialized) {
            (void)ma_device_stop(&device);
            ma_device_uninit(&device);
            device_initialized = false;
        }
        voices.clear();
        cursor_frames.store(0U, std::memory_order_relaxed);
    }

    ~Impl() { uninitialize(); }
};

AudioPlayback::AudioPlayback() : impl_(std::make_unique<Impl>()) {}
AudioPlayback::~AudioPlayback() = default;

bool AudioPlayback::start(const std::string& path, double position_seconds,
                          bool should_loop, std::string* error) {
    PlaybackTrack track;
    track.path = path;
    track.source_position_seconds = position_seconds;
    track.loop = should_loop;
    return start_mix({track}, position_seconds, error);
}

bool AudioPlayback::start_mix(const std::vector<PlaybackTrack>& tracks,
                              double timeline_position_seconds,
                              std::string* error) {
    if (error != nullptr) error->clear();
    impl_->uninitialize();
    if (tracks.empty() || !std::isfinite(timeline_position_seconds)
        || timeline_position_seconds < 0.0) {
        return fail(error, "Audio playback requires tracks and a finite timeline position.");
    }
    try {
        impl_->voices.reserve(tracks.size());
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to prepare the audio mix.");
    }
    for (const PlaybackTrack& track : tracks) {
        if (track.path.empty() || !std::isfinite(track.source_position_seconds)
            || track.source_position_seconds < 0.0
            || !std::isfinite(track.playback_rate)
            || track.playback_rate <= 0.0
            || !std::isfinite(track.stop_after_seconds)
            || track.stop_after_seconds < 0.0) {
            impl_->uninitialize();
            return fail(error, "Audio mix tracks require a source and finite, positive timing values.");
        }
        std::unique_ptr<Impl::Voice> voice;
        try {
            voice = std::make_unique<Impl::Voice>();
        } catch (const std::bad_alloc&) {
            impl_->uninitialize();
            return fail(error, "Not enough memory to prepare the audio mix.");
        }
        if (!prepare_rate_adjusted_decoder(
                track, voice->decoder, voice->initialized,
                "Could not open a project music source for playback",
                "Could not seek a project music source", error)) {
            impl_->uninitialize();
            return false;
        }
        voice->loop = track.loop;
        voice->active = true;
        if (track.stop_after_seconds > 0.0) {
            const long double remaining =
                static_cast<long double>(track.stop_after_seconds)
                * static_cast<long double>(kPlaybackSampleRate);
            voice->remaining_output_frames = remaining
                >= static_cast<long double>(
                    std::numeric_limits<ma_uint64>::max())
                    ? std::numeric_limits<ma_uint64>::max()
                    : static_cast<ma_uint64>(std::ceil(remaining));
        }
        try {
            impl_->voices.push_back(std::move(voice));
        } catch (const std::bad_alloc&) {
            impl_->uninitialize();
            return fail(error, "Not enough memory to prepare the audio mix.");
        }
    }

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
    const long double timeline_frame =
        static_cast<long double>(timeline_position_seconds)
        * static_cast<long double>(kPlaybackSampleRate);
    impl_->cursor_frames.store(
        timeline_frame >= static_cast<long double>(
                              (std::numeric_limits<ma_uint64>::max)())
            ? (std::numeric_limits<ma_uint64>::max)()
            : static_cast<ma_uint64>(timeline_frame),
        std::memory_order_relaxed);
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

bool write_mix_wav(const std::vector<PlaybackTrack>& tracks,
                   double duration_seconds, const std::string& destination,
                   const std::atomic_bool* cancel, std::string* error) {
    if (error != nullptr) error->clear();
    if (tracks.empty() || destination.empty()
        || !std::isfinite(duration_seconds) || duration_seconds <= 0.0) {
        return fail(error, "Audio export requires tracks, a temporary destination, and a positive duration.");
    }
    constexpr std::uint64_t kMaximumMixSeconds =
        static_cast<std::uint64_t>((std::numeric_limits<int>::max)());
    if (duration_seconds > static_cast<double>(kMaximumMixSeconds)) {
        return fail(error,
                    "Audio export duration exceeds the signed-int project/API time capacity.");
    }
    const std::uint64_t total_frames = static_cast<std::uint64_t>(
        std::ceil(duration_seconds * static_cast<double>(kPlaybackSampleRate)));
    const std::uint64_t data_bytes = total_frames * kPlaybackChannels
                                     * sizeof(float);
    if (data_bytes > (std::numeric_limits<std::uint32_t>::max)() - 36U) {
        return fail(error, "The mixed WAV would exceed its 4 GiB format limit.");
    }

    struct OfflineVoice {
        ma_decoder decoder{};
        bool initialized = false;
        bool active = false;
        bool loop = false;
        ma_uint64 remaining = (std::numeric_limits<ma_uint64>::max)();
        ~OfflineVoice() {
            if (initialized) (void)ma_decoder_uninit(&decoder);
        }
    };
    std::vector<std::unique_ptr<OfflineVoice>> voices;
    try {
        voices.reserve(tracks.size());
        for (const PlaybackTrack& track : tracks) {
            if (track.path.empty()
                || !std::isfinite(track.source_position_seconds)
                || track.source_position_seconds < 0.0
                || !std::isfinite(track.playback_rate)
                || track.playback_rate <= 0.0
                || !std::isfinite(track.stop_after_seconds)
                || track.stop_after_seconds < 0.0) {
                return fail(error, "An audio-export track has invalid timing values.");
            }
            auto voice = std::make_unique<OfflineVoice>();
            if (!prepare_rate_adjusted_decoder(
                    track, voice->decoder, voice->initialized,
                    "Could not decode an audible project source",
                    "Could not seek an audible project source", error)) {
                return false;
            }
            voice->active = true;
            voice->loop = track.loop;
            if (track.stop_after_seconds > 0.0) {
                voice->remaining = static_cast<ma_uint64>(std::ceil(
                    track.stop_after_seconds * kPlaybackSampleRate));
            }
            voices.push_back(std::move(voice));
        }
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to prepare the movie audio mix.");
    }

#if defined(_WIN32)
    std::ofstream output(detail::path_from_utf8(destination),
                         std::ios::binary | std::ios::trunc);
#else
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
#endif
    const auto remove_partial = [&destination] {
        std::error_code ignored;
        std::filesystem::remove(detail::path_from_utf8(destination), ignored);
    };
    if (!output) return fail(error, "Could not create the temporary movie audio mix.");
    const auto write_u16 = [&output](std::uint16_t value) {
        const std::array<char, 2> bytes{
            static_cast<char>(value & 0xffU),
            static_cast<char>((value >> 8U) & 0xffU)};
        output.write(bytes.data(), bytes.size());
    };
    const auto write_u32 = [&output](std::uint32_t value) {
        const std::array<char, 4> bytes{
            static_cast<char>(value & 0xffU),
            static_cast<char>((value >> 8U) & 0xffU),
            static_cast<char>((value >> 16U) & 0xffU),
            static_cast<char>((value >> 24U) & 0xffU)};
        output.write(bytes.data(), bytes.size());
    };
    output.write("RIFF", 4);
    write_u32(static_cast<std::uint32_t>(36U + data_bytes));
    output.write("WAVEfmt ", 8);
    write_u32(16U);
    write_u16(3U); // IEEE float
    write_u16(static_cast<std::uint16_t>(kPlaybackChannels));
    write_u32(kPlaybackSampleRate);
    write_u32(kPlaybackSampleRate * kPlaybackChannels * sizeof(float));
    write_u16(static_cast<std::uint16_t>(kPlaybackChannels * sizeof(float)));
    write_u16(32U);
    output.write("data", 4);
    write_u32(static_cast<std::uint32_t>(data_bytes));

    constexpr ma_uint64 kChunkFrames = 2048U;
    std::array<float, kChunkFrames * kPlaybackChannels> mixed{};
    std::array<float, kChunkFrames * kPlaybackChannels> decoded{};
    std::array<char, kChunkFrames * kPlaybackChannels * sizeof(float)>
        little_endian_samples{};
    const float voice_gain = static_cast<float>(
        1.0 / std::sqrt(static_cast<double>(voices.size())));
    for (ma_uint64 offset = 0U; offset < total_frames;) {
        if (cancel != nullptr
            && cancel->load(std::memory_order_relaxed)) {
            output.close();
            remove_partial();
            return fail(error, "Movie audio mixing was cancelled.");
        }
        const ma_uint64 chunk = std::min<ma_uint64>(
            kChunkFrames, total_frames - offset);
        std::fill_n(mixed.data(), static_cast<std::size_t>(chunk)
                                      * kPlaybackChannels, 0.0F);
        for (const auto& voice_owner : voices) {
            OfflineVoice& voice = *voice_owner;
            ma_uint64 completed = 0U;
            bool loop_attempted = false;
            while (completed < chunk && voice.active) {
                const ma_uint64 wanted = std::min(
                    chunk - completed, voice.remaining);
                if (wanted == 0U) {
                    voice.active = false;
                    break;
                }
                ma_uint64 got = 0U;
                const ma_result result = ma_decoder_read_pcm_frames(
                    &voice.decoder, decoded.data(), wanted, &got);
                for (ma_uint64 frame = 0U; frame < got; ++frame) {
                    const std::size_t target = static_cast<std::size_t>(
                        (completed + frame) * kPlaybackChannels);
                    const std::size_t source = static_cast<std::size_t>(
                        frame * kPlaybackChannels);
                    mixed[target] += decoded[source] * voice_gain;
                    mixed[target + 1U] += decoded[source + 1U] * voice_gain;
                }
                completed += got;
                if (voice.remaining
                    != (std::numeric_limits<ma_uint64>::max)()) {
                    voice.remaining -= std::min(voice.remaining, got);
                }
                if (got > 0U) loop_attempted = false;
                if (got == wanted) continue;
                if ((result != MA_SUCCESS && result != MA_AT_END)
                    || !voice.loop || loop_attempted
                    || ma_decoder_seek_to_pcm_frame(&voice.decoder, 0U)
                           != MA_SUCCESS) {
                    voice.active = false;
                    break;
                }
                loop_attempted = true;
            }
        }
        for (std::size_t index = 0U;
             index < static_cast<std::size_t>(chunk) * kPlaybackChannels;
             ++index) {
            mixed[index] = std::clamp(mixed[index], -1.0F, 1.0F);
        }
        const std::size_t chunk_samples = static_cast<std::size_t>(chunk)
                                          * kPlaybackChannels;
        for (std::size_t index = 0U; index < chunk_samples; ++index) {
            std::uint32_t bits = 0U;
            static_assert(sizeof(bits) == sizeof(mixed[index]));
            std::memcpy(&bits, &mixed[index], sizeof(bits));
            const std::size_t byte = index * sizeof(float);
            little_endian_samples[byte] = static_cast<char>(bits & 0xffU);
            little_endian_samples[byte + 1U] =
                static_cast<char>((bits >> 8U) & 0xffU);
            little_endian_samples[byte + 2U] =
                static_cast<char>((bits >> 16U) & 0xffU);
            little_endian_samples[byte + 3U] =
                static_cast<char>((bits >> 24U) & 0xffU);
        }
        output.write(little_endian_samples.data(),
                     static_cast<std::streamsize>(chunk_samples
                                                  * sizeof(float)));
        if (!output) {
            output.close();
            remove_partial();
            return fail(error, "Could not write the complete temporary movie audio mix.");
        }
        offset += chunk;
    }
    output.flush();
    if (!output) {
        output.close();
        remove_partial();
        return fail(error, "Could not flush the temporary movie audio mix.");
    }
    return true;
}

} // namespace pvt::audio
