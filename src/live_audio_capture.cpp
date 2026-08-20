#include "live_audio_capture.h"

#include "audio_input_processing.h"

#include "miniaudio.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

namespace pvt::audio {
namespace {

constexpr ma_uint32 kCaptureChannels = 1U;
constexpr ma_uint32 kCaptureSampleRate = 48000U;
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr std::string_view kRuntimeDevicePrefix = "miniaudio-device-v1:";

bool fail(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
}

std::string miniaudio_error(const char* action, ma_result result) {
    const char* description = ma_result_description(result);
    return std::string(action) + (description != nullptr ? ": " : ".")
           + (description != nullptr ? description : "");
}

std::int64_t monotonic_nanoseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

float unit(float value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

double positive_fraction(double value) noexcept {
    value = std::fmod(value, 1.0);
    return value < 0.0 ? value + 1.0 : value;
}

std::string runtime_device_id(ma_backend backend, const ma_device_id& id) {
    static constexpr char kHex[] = "0123456789abcdef";
    const auto* bytes = reinterpret_cast<const unsigned char*>(&id);
    std::string result;
    result.reserve(kRuntimeDevicePrefix.size() + 12U + 2U * sizeof(id));
    result.append(kRuntimeDevicePrefix);
    result.append(std::to_string(static_cast<unsigned int>(backend)));
    result.push_back(':');
    for (std::size_t index = 0U; index < sizeof(id); ++index) {
        result.push_back(kHex[bytes[index] >> 4U]);
        result.push_back(kHex[bytes[index] & 0x0fU]);
    }
    return result;
}

std::vector<LiveAudioDevice> describe_devices(
    ma_backend backend, const ma_device_info* captures, ma_uint32 count) {
    std::vector<LiveAudioDevice> result;
    result.reserve(count);
    for (ma_uint32 index = 0U; index < count; ++index) {
        LiveAudioDevice device;
        device.name = captures[index].name;
        device.is_default = captures[index].isDefault == MA_TRUE;
        device.runtime_id = runtime_device_id(backend, captures[index].id);
        device.display_name = device.name.empty() ? "Unnamed audio input"
                                                   : device.name;
        result.push_back(std::move(device));
    }
    for (std::size_t index = 0U; index < result.size(); ++index) {
        std::size_t duplicate_count = 0U;
        std::size_t ordinal = 0U;
        for (std::size_t candidate = 0U; candidate < result.size(); ++candidate) {
            if (result[candidate].name != result[index].name) continue;
            ++duplicate_count;
            if (candidate <= index) ++ordinal;
        }
        if (duplicate_count > 1U) {
            result[index].display_name += " (" + std::to_string(ordinal)
                + " of " + std::to_string(duplicate_count) + ')';
        }
    }
    return result;
}

} // namespace

std::optional<std::size_t> find_live_audio_device(
    const std::vector<LiveAudioDevice>& devices,
    const std::string& runtime_id_or_name,
    std::string* error) {
    if (error != nullptr) error->clear();
    if (runtime_id_or_name.empty()) {
        fail(error, "A non-default live audio device selection is empty.");
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < devices.size(); ++index) {
        if (!devices[index].runtime_id.empty()
            && devices[index].runtime_id == runtime_id_or_name) {
            return index;
        }
    }
    std::optional<std::size_t> name_match;
    for (std::size_t index = 0U; index < devices.size(); ++index) {
        if (devices[index].name != runtime_id_or_name) continue;
        if (name_match.has_value()) {
            fail(error,
                 "Multiple live audio devices have this name; refresh and select a specific device.");
            return std::nullopt;
        }
        name_match = index;
    }
    if (!name_match.has_value()) {
        fail(error, "The selected live audio device is no longer available.");
    }
    return name_match;
}

LiveBeatTiming live_beat_timing(std::uint64_t detected_beat_count,
                                std::uint64_t current_frame,
                                std::uint64_t anchor_frame,
                                std::uint32_t sample_rate,
                                double detected_bpm) noexcept {
    LiveBeatTiming result;
    if (detected_beat_count == 0U || sample_rate == 0U
        || !std::isfinite(detected_bpm) || detected_bpm <= 0.0) {
        return result;
    }
    const double rate = static_cast<double>(sample_rate);
    result.anchor_seconds = static_cast<double>(anchor_frame) / rate;
    const std::uint64_t elapsed_frames = current_frame >= anchor_frame
        ? current_frame - anchor_frame : 0U;
    const double elapsed_beats = static_cast<double>(elapsed_frames) / rate
        * detected_bpm / 60.0;
    result.position = static_cast<double>(detected_beat_count - 1U)
        + elapsed_beats;
    if (!std::isfinite(result.position)) {
        result.position = static_cast<double>(detected_beat_count - 1U);
    }
    result.phase = positive_fraction(result.position);
    return result;
}

std::optional<double> live_beat_route_phase(
    std::uint64_t detected_beat_count,
    double beat_position,
    double reference_beats_per_loop,
    double signed_latency_beats,
    ClockInterpolation interpolation) noexcept {
    if (detected_beat_count == 0U || !std::isfinite(beat_position)
        || !std::isfinite(reference_beats_per_loop)
        || reference_beats_per_loop <= 0.0
        || !std::isfinite(signed_latency_beats)) {
        return std::nullopt;
    }
    // Positive input latency means the physical beat happened before its
    // sample reached the analyzer, so compensate by advancing the musical
    // position. Negative values deliberately delay an early source.
    double routed_position = beat_position + signed_latency_beats;
    const double whole_beats = std::floor(routed_position);
    const double fraction = routed_position - whole_beats;
    switch (interpolation) {
        case ClockInterpolation::Hold:
            routed_position = whole_beats;
            break;
        case ClockInterpolation::Linear:
            break;
        case ClockInterpolation::Smoothstep:
            routed_position = whole_beats
                + fraction * fraction * (3.0 - 2.0 * fraction);
            break;
        default:
            return std::nullopt;
    }
    const double loops = routed_position / reference_beats_per_loop;
    if (!std::isfinite(loops)) return std::nullopt;
    return positive_fraction(loops);
}

std::uint64_t live_audio_frame_clock_increment(
    std::uint32_t callback_frames, bool has_input_samples) noexcept {
    return has_input_samples ? callback_frames : 0U;
}

double live_audio_extrapolated_beat_position(
    double beat_position_at_last_callback, double beats_per_minute,
    double last_valid_callback_age_seconds) noexcept {
    if (!std::isfinite(beat_position_at_last_callback)
        || !std::isfinite(beats_per_minute) || beats_per_minute <= 0.0
        || !std::isfinite(last_valid_callback_age_seconds)
        || last_valid_callback_age_seconds <= 0.0) {
        return beat_position_at_last_callback;
    }
    const double extrapolated = beat_position_at_last_callback
        + last_valid_callback_age_seconds * beats_per_minute / 60.0;
    return std::isfinite(extrapolated)
        ? extrapolated : beat_position_at_last_callback;
}

bool live_audio_callback_within_holdover(
    double last_valid_callback_age_seconds,
    int holdover_milliseconds) noexcept {
    if (!std::isfinite(last_valid_callback_age_seconds)
        || last_valid_callback_age_seconds < 0.0) {
        return false;
    }
    const int allowed_milliseconds = std::max(
        kLiveAudioNormalCallbackToleranceMilliseconds,
        std::max(0, holdover_milliseconds));
    return last_valid_callback_age_seconds * 1000.0
        <= static_cast<double>(allowed_milliseconds);
}

struct LiveAudioCapture::Impl {
    struct FrequencyStreamState {
        std::string uuid;
        AudioFrequencyRangeProcessor filter;
        std::atomic<float> energy{0.0F};
        std::atomic<float> onset{0.0F};
        std::atomic<double> bpm{0.0};
        // Even values publish a coherent (count, BPM, anchor) tuple. The
        // callback is the only writer; readers retry while an onset update is
        // in progress. This remains lock-free and allocation-free on the
        // hardware callback.
        std::atomic<std::uint64_t> beat_generation{0U};
        std::atomic<std::uint64_t> beat_count{0U};
        std::atomic<std::uint64_t> last_beat_frame{0U};
        double squared = 0.0;
        double previous_energy = 0.0;
        double adaptive_peak = 0.02;

        void begin_block() noexcept { squared = 0.0; }

        void push(float sample) noexcept {
            const double filtered = static_cast<double>(filter.process(sample));
            squared += filtered * filtered;
        }

        void finish_block(std::uint64_t now_frame, ma_uint32 frame_count,
                          double response) noexcept {
            const double rms = std::sqrt(
                squared / static_cast<double>(frame_count));
            adaptive_peak = std::max(rms, adaptive_peak * 0.9992);
            const double normalization = std::max(0.015, adaptive_peak);
            const float normalized = unit(static_cast<float>(
                response * rms / normalization));
            const double flux = std::max(0.0, rms - previous_energy);
            const float attack = unit(static_cast<float>(
                flux * response * 18.0 / normalization));
            previous_energy = rms;
            energy.store(normalized, std::memory_order_relaxed);
            onset.store(attack, std::memory_order_relaxed);
            const std::uint64_t previous = last_beat_frame.load(
                std::memory_order_relaxed);
            const std::uint64_t refractory = kCaptureSampleRate / 4U;
            if (attack >= 0.55F
                && (previous == 0U || now_frame - previous >= refractory)) {
                beat_generation.fetch_add(1U, std::memory_order_acq_rel);
                if (previous != 0U) {
                    const double interval = static_cast<double>(now_frame - previous)
                                            / kCaptureSampleRate;
                    const double candidate = 60.0 / interval;
                    if (candidate >= 40.0 && candidate <= 240.0) {
                        const double old = bpm.load(std::memory_order_relaxed);
                        bpm.store(old > 0.0 ? 0.78 * old + 0.22 * candidate
                                            : candidate,
                                  std::memory_order_relaxed);
                    }
                }
                last_beat_frame.store(now_frame, std::memory_order_relaxed);
                beat_count.store(
                    beat_count.load(std::memory_order_relaxed) + 1U,
                    std::memory_order_relaxed);
                beat_generation.fetch_add(1U, std::memory_order_release);
            }
        }

        void reset() noexcept {
            filter.reset();
            energy.store(0.0F, std::memory_order_relaxed);
            onset.store(0.0F, std::memory_order_relaxed);
            beat_generation.fetch_add(1U, std::memory_order_acq_rel);
            bpm.store(0.0, std::memory_order_relaxed);
            beat_count.store(0U, std::memory_order_relaxed);
            last_beat_frame.store(0U, std::memory_order_relaxed);
            beat_generation.fetch_add(1U, std::memory_order_release);
            squared = 0.0;
            previous_energy = 0.0;
            adaptive_peak = 0.02;
        }
    };

    ma_context context{};
    ma_device device{};
    bool context_initialized = false;
    bool device_initialized = false;
    std::atomic_bool running{false};
    std::atomic<float> gain{1.0F};
    std::atomic<float> sensitivity{1.0F};
    std::atomic<float> energy{0.0F};
    std::atomic<float> bass{0.0F};
    std::atomic<float> midrange{0.0F};
    std::atomic<float> treble{0.0F};
    std::atomic<float> onset{0.0F};
    std::atomic<float> centroid{0.0F};
    std::atomic<float> flatness{0.0F};
    std::atomic<float> chroma_hue{0.0F};
    std::atomic<float> chroma_strength{0.0F};
    std::atomic<double> bpm{0.0};
    std::atomic<std::uint64_t> received_frames{0U};
    std::atomic<std::uint64_t> beat_generation{0U};
    std::atomic<std::uint64_t> beat_count{0U};
    std::atomic<std::uint64_t> last_beat_frame{0U};
    std::atomic<std::uint64_t> dropouts{0U};
    std::atomic<std::int64_t> last_callback_ns{0};
    std::atomic<double> estimated_latency_ms{0.0};
    AudioInputProcessingConfig processing_config;
    AudioInputProcessor input_processor;
    std::vector<std::unique_ptr<FrequencyStreamState>> frequency_streams;

    // Callback-thread-only analyzer state.
    double low_state = 0.0;
    double mid_low_state = 0.0;
    double previous_energy = 0.0;
    double adaptive_peak = 0.02;
    float previous_sample = 0.0F;

    static void data_callback(ma_device* source, void*, const void* input,
                              ma_uint32 frame_count) noexcept {
        auto* self = static_cast<Impl*>(source != nullptr
                                           ? source->pUserData : nullptr);
        if (self == nullptr || frame_count == 0U) return;
        const std::uint64_t valid_frames = live_audio_frame_clock_increment(
            frame_count, input != nullptr);
        if (valid_frames == 0U) {
            self->dropouts.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        const std::uint64_t first_frame = self->received_frames.fetch_add(
            valid_frames, std::memory_order_relaxed);
        const auto* samples = static_cast<const float*>(input);
        const double input_gain = static_cast<double>(
            self->gain.load(std::memory_order_relaxed));
        const double response = static_cast<double>(
            self->sensitivity.load(std::memory_order_relaxed));
        for (const auto& stream : self->frequency_streams) {
            stream->begin_block();
        }
        // One-pole crossovers are cheap enough for the audio callback and make
        // the three controls useful without a block FFT or callback allocation.
        const double bass_alpha = 1.0 - std::exp(
            -2.0 * kPi * 220.0 / static_cast<double>(kCaptureSampleRate));
        const double mid_alpha = 1.0 - std::exp(
            -2.0 * kPi * 2400.0 / static_cast<double>(kCaptureSampleRate));
        double total_squared = 0.0;
        double bass_squared = 0.0;
        double mid_squared = 0.0;
        double treble_squared = 0.0;
        std::uint32_t zero_crossings = 0U;
        for (ma_uint32 frame = 0U; frame < frame_count; ++frame) {
            const float processed = self->input_processor.process(samples[frame]);
            const double sample = static_cast<double>(processed) * input_gain;
            for (const auto& stream : self->frequency_streams) {
                stream->push(static_cast<float>(sample));
            }
            self->low_state += bass_alpha * (sample - self->low_state);
            self->mid_low_state += mid_alpha * (sample - self->mid_low_state);
            const double low = self->low_state;
            const double middle = self->mid_low_state - low;
            const double high = sample - self->mid_low_state;
            total_squared += sample * sample;
            bass_squared += low * low;
            mid_squared += middle * middle;
            treble_squared += high * high;
            const float current = static_cast<float>(sample);
            if ((current >= 0.0F) != (self->previous_sample >= 0.0F)) {
                ++zero_crossings;
            }
            self->previous_sample = current;
        }

        const double divisor = static_cast<double>(frame_count);
        const double rms = std::sqrt(total_squared / divisor);
        // A slowly falling peak keeps response useful for both quiet line input
        // and hot microphones without an authoring-time normalization pass.
        self->adaptive_peak = std::max(rms, self->adaptive_peak * 0.9992);
        const double normalization = std::max(0.015, self->adaptive_peak);
        const float normalized_energy = unit(static_cast<float>(
            response * rms / normalization));
        const float normalized_bass = unit(static_cast<float>(
            response * std::sqrt(bass_squared / divisor) / normalization));
        const float normalized_mid = unit(static_cast<float>(
            response * std::sqrt(mid_squared / divisor) / normalization));
        const float normalized_treble = unit(static_cast<float>(
            response * std::sqrt(treble_squared / divisor) / normalization));
        const double positive_flux = std::max(0.0, rms - self->previous_energy);
        const float normalized_onset = unit(static_cast<float>(
            positive_flux * response * 18.0 / normalization));
        self->previous_energy = rms;

        self->energy.store(normalized_energy, std::memory_order_relaxed);
        self->bass.store(normalized_bass, std::memory_order_relaxed);
        self->midrange.store(normalized_mid, std::memory_order_relaxed);
        self->treble.store(normalized_treble, std::memory_order_relaxed);
        self->onset.store(normalized_onset, std::memory_order_relaxed);
        const double band_sum = static_cast<double>(normalized_bass)
                                + normalized_mid + normalized_treble + 1.0e-9;
        self->centroid.store(unit(static_cast<float>(
            (0.10 * normalized_bass + 0.48 * normalized_mid
             + 0.92 * normalized_treble) / band_sum)),
            std::memory_order_relaxed);
        const double geometric = std::cbrt(
            std::max(1.0e-9, static_cast<double>(normalized_bass))
            * std::max(1.0e-9, static_cast<double>(normalized_mid))
            * std::max(1.0e-9, static_cast<double>(normalized_treble)));
        self->flatness.store(unit(static_cast<float>(
            geometric / (band_sum / 3.0))), std::memory_order_relaxed);

        const double crossing_hz = static_cast<double>(zero_crossings)
                                   * kCaptureSampleRate
                                   / (2.0 * divisor);
        if (crossing_hz >= 35.0 && crossing_hz <= 6000.0
            && normalized_energy > 0.025F) {
            double pitch_class = std::log2(crossing_hz / 261.6255653005986);
            pitch_class -= std::floor(pitch_class);
            self->chroma_hue.store(static_cast<float>(pitch_class),
                                   std::memory_order_relaxed);
            // Zero-crossing pitch is intentionally confidence-weighted. Noisy
            // input still drives energy/bands but does not fling hue randomly.
            const float tonal = unit(static_cast<float>(
                normalized_energy * (1.0 - self->flatness.load(
                    std::memory_order_relaxed))));
            self->chroma_strength.store(tonal, std::memory_order_relaxed);
        } else {
            self->chroma_strength.store(0.0F, std::memory_order_relaxed);
        }

        const std::uint64_t now_frame = first_frame + frame_count;
        for (const auto& stream : self->frequency_streams) {
            stream->finish_block(now_frame, frame_count, response);
        }
        const std::uint64_t previous_beat_frame = self->last_beat_frame.load(
            std::memory_order_relaxed);
        const std::uint64_t refractory = kCaptureSampleRate / 4U;
        if (normalized_onset >= 0.55F
            && (previous_beat_frame == 0U
                || now_frame - previous_beat_frame >= refractory)) {
            self->beat_generation.fetch_add(1U, std::memory_order_acq_rel);
            if (previous_beat_frame != 0U) {
                const double interval = static_cast<double>(
                    now_frame - previous_beat_frame) / kCaptureSampleRate;
                const double candidate = 60.0 / interval;
                if (candidate >= 40.0 && candidate <= 240.0) {
                    const double previous_bpm = self->bpm.load(
                        std::memory_order_relaxed);
                    self->bpm.store(previous_bpm > 0.0
                                        ? 0.78 * previous_bpm + 0.22 * candidate
                                        : candidate,
                                    std::memory_order_relaxed);
                }
            }
            self->last_beat_frame.store(now_frame,
                                        std::memory_order_relaxed);
            self->beat_count.store(
                self->beat_count.load(std::memory_order_relaxed) + 1U,
                std::memory_order_relaxed);
            self->beat_generation.fetch_add(1U, std::memory_order_release);
        }
        // received_frames describes the end of this valid sample block, so
        // publish its wall-clock anchor after the analysis reaches that same
        // point. Null callbacks deliberately never refresh this timestamp.
        self->last_callback_ns.store(monotonic_nanoseconds(),
                                     std::memory_order_release);
    }

    void uninitialize() noexcept {
        running.store(false, std::memory_order_relaxed);
        if (device_initialized) {
            (void)ma_device_stop(&device);
            ma_device_uninit(&device);
            device_initialized = false;
        }
        if (context_initialized) {
            ma_context_uninit(&context);
            context_initialized = false;
        }
    }

    void clear_analysis() noexcept {
        energy.store(0.0F, std::memory_order_relaxed);
        bass.store(0.0F, std::memory_order_relaxed);
        midrange.store(0.0F, std::memory_order_relaxed);
        treble.store(0.0F, std::memory_order_relaxed);
        onset.store(0.0F, std::memory_order_relaxed);
        centroid.store(0.0F, std::memory_order_relaxed);
        flatness.store(0.0F, std::memory_order_relaxed);
        chroma_hue.store(0.0F, std::memory_order_relaxed);
        chroma_strength.store(0.0F, std::memory_order_relaxed);
        received_frames.store(0U, std::memory_order_relaxed);
        beat_generation.fetch_add(1U, std::memory_order_acq_rel);
        bpm.store(0.0, std::memory_order_relaxed);
        beat_count.store(0U, std::memory_order_relaxed);
        last_beat_frame.store(0U, std::memory_order_relaxed);
        beat_generation.fetch_add(1U, std::memory_order_release);
        dropouts.store(0U, std::memory_order_relaxed);
        last_callback_ns.store(0, std::memory_order_relaxed);
        low_state = 0.0;
        mid_low_state = 0.0;
        previous_energy = 0.0;
        adaptive_peak = 0.02;
        previous_sample = 0.0F;
        input_processor.reset();
        for (const auto& stream : frequency_streams) stream->reset();
    }

    ~Impl() { uninitialize(); }
};

LiveAudioCapture::LiveAudioCapture() : impl_(std::make_unique<Impl>()) {}
LiveAudioCapture::~LiveAudioCapture() = default;

std::vector<LiveAudioDevice> LiveAudioCapture::devices(
    std::string* error) const {
    if (error != nullptr) error->clear();
    ma_context context{};
    ma_result result = ma_context_init(nullptr, 0U, nullptr, &context);
    if (result != MA_SUCCESS) {
        fail(error, miniaudio_error("Could not initialize audio-device discovery",
                                    result));
        return {};
    }
    ma_device_info* captures = nullptr;
    ma_uint32 capture_count = 0U;
    result = ma_context_get_devices(&context, nullptr, nullptr,
                                    &captures, &capture_count);
    std::vector<LiveAudioDevice> found;
    if (result == MA_SUCCESS) {
        try {
            found = describe_devices(context.backend, captures, capture_count);
        } catch (...) {
            fail(error, "Not enough memory to list capture devices.");
            found.clear();
        }
    } else {
        fail(error, miniaudio_error("Could not enumerate audio capture devices",
                                    result));
    }
    ma_context_uninit(&context);
    return found;
}

bool LiveAudioCapture::start(const std::string& runtime_device_id_or_name,
                             std::uint32_t requested_period_frames,
                             std::string* error) {
    if (error != nullptr) error->clear();
    stop();
    if (!impl_->input_processor.configure(
            impl_->processing_config, kCaptureSampleRate, error)) {
        return false;
    }
    requested_period_frames = std::max<ma_uint32>(1U,
                                                  requested_period_frames);
    ma_result result = ma_context_init(nullptr, 0U, nullptr, &impl_->context);
    if (result != MA_SUCCESS) {
        return fail(error, miniaudio_error(
            "Could not initialize live audio capture", result));
    }
    impl_->context_initialized = true;

    ma_device_id selected_id{};
    const ma_device_id* selected = nullptr;
    if (!runtime_device_id_or_name.empty()) {
        ma_device_info* captures = nullptr;
        ma_uint32 capture_count = 0U;
        result = ma_context_get_devices(&impl_->context, nullptr, nullptr,
                                        &captures, &capture_count);
        if (result != MA_SUCCESS) {
            impl_->uninitialize();
            return fail(error, miniaudio_error(
                "Could not enumerate live audio devices", result));
        }
        std::vector<LiveAudioDevice> available;
        try {
            available = describe_devices(impl_->context.backend, captures,
                                         capture_count);
        } catch (...) {
            impl_->uninitialize();
            return fail(error,
                        "Not enough memory to select a live audio device.");
        }
        const auto selected_index = find_live_audio_device(
            available, runtime_device_id_or_name, error);
        if (!selected_index.has_value()) {
            impl_->uninitialize();
            return false;
        }
        selected_id = captures[*selected_index].id;
        selected = &selected_id;
    }

    impl_->clear_analysis();
    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.pDeviceID = selected;
    config.capture.format = ma_format_f32;
    config.capture.channels = kCaptureChannels;
    config.sampleRate = kCaptureSampleRate;
    config.periodSizeInFrames = requested_period_frames;
    config.periods = 2U;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.noPreSilencedOutputBuffer = MA_FALSE;
    config.dataCallback = &Impl::data_callback;
    config.pUserData = impl_.get();
    result = ma_device_init(&impl_->context, &config, &impl_->device);
    if (result != MA_SUCCESS) {
        impl_->uninitialize();
        return fail(error, miniaudio_error(
            "Could not open the live audio input", result));
    }
    impl_->device_initialized = true;
    const ma_uint32 actual_period = impl_->device.capture.internalPeriodSizeInFrames
        != 0U ? impl_->device.capture.internalPeriodSizeInFrames
              : requested_period_frames;
    const ma_uint32 actual_periods = impl_->device.capture.internalPeriods != 0U
        ? impl_->device.capture.internalPeriods : config.periods;
    const ma_uint32 actual_rate = impl_->device.capture.internalSampleRate != 0U
        ? impl_->device.capture.internalSampleRate : kCaptureSampleRate;
    const double latency_ms = 1000.0
        * static_cast<double>(actual_period)
        * static_cast<double>(actual_periods)
        / static_cast<double>(actual_rate);
    impl_->estimated_latency_ms.store(latency_ms,
                                      std::memory_order_relaxed);
    result = ma_device_start(&impl_->device);
    if (result != MA_SUCCESS) {
        impl_->uninitialize();
        return fail(error, miniaudio_error(
            "Could not start the live audio input", result));
    }
    impl_->running.store(true, std::memory_order_release);
    return true;
}

void LiveAudioCapture::stop() noexcept {
    impl_->uninitialize();
}

bool LiveAudioCapture::is_running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

LiveAudioSnapshot LiveAudioCapture::snapshot() const {
    LiveAudioSnapshot value;
    value.features.energy = impl_->energy.load(std::memory_order_relaxed);
    value.features.bass = impl_->bass.load(std::memory_order_relaxed);
    value.features.midrange = impl_->midrange.load(std::memory_order_relaxed);
    value.features.treble = impl_->treble.load(std::memory_order_relaxed);
    value.features.onset = impl_->onset.load(std::memory_order_relaxed);
    value.features.spectral_centroid = impl_->centroid.load(
        std::memory_order_relaxed);
    value.features.spectral_flatness = impl_->flatness.load(
        std::memory_order_relaxed);
    value.features.chroma_hue = impl_->chroma_hue.load(
        std::memory_order_relaxed);
    value.features.chroma_strength = impl_->chroma_strength.load(
        std::memory_order_relaxed);
    const std::uint64_t frames = impl_->received_frames.load(
        std::memory_order_relaxed);
    std::uint64_t beat_frame = 0U;
    for (;;) {
        const std::uint64_t before = impl_->beat_generation.load(
            std::memory_order_acquire);
        if ((before & 1U) != 0U) continue;
        value.beat_count = impl_->beat_count.load(std::memory_order_relaxed);
        value.detected_bpm = impl_->bpm.load(std::memory_order_relaxed);
        beat_frame = impl_->last_beat_frame.load(std::memory_order_relaxed);
        const std::uint64_t after = impl_->beat_generation.load(
            std::memory_order_acquire);
        if (before == after) break;
    }
    value.stream_seconds = static_cast<double>(frames) / kCaptureSampleRate;
    if (value.beat_count != 0U) {
        value.beat_anchor_seconds = static_cast<double>(beat_frame)
            / kCaptureSampleRate;
        value.beat_position = static_cast<double>(value.beat_count - 1U);
        if (frames >= beat_frame) {
            const double seconds_since = static_cast<double>(
                frames - beat_frame) / kCaptureSampleRate;
            const double beat_period = value.detected_bpm > 0.0
                                           ? 60.0 / value.detected_bpm : 0.5;
            if (value.detected_bpm > 0.0) {
                const auto timing = live_beat_timing(
                    value.beat_count, frames, beat_frame,
                    kCaptureSampleRate, value.detected_bpm);
                value.beat_position = timing.position;
                value.beat_phase = timing.phase;
            }
            value.features.beat = unit(static_cast<float>(
                1.0 - seconds_since / std::min(0.16, beat_period)));
        }
    }
    value.estimated_input_latency_ms = impl_->estimated_latency_ms.load(
        std::memory_order_relaxed);
    value.callback_dropouts = impl_->dropouts.load(std::memory_order_relaxed);
    const std::int64_t callback_ns = impl_->last_callback_ns.load(
        std::memory_order_acquire);
    if (callback_ns != 0) {
        const std::int64_t age_ns = std::max<std::int64_t>(
            0, monotonic_nanoseconds() - callback_ns);
        value.last_valid_callback_age_seconds =
            static_cast<double>(age_ns) / 1000000000.0;
    }
    value.receiving = is_running()
        && value.last_valid_callback_age_seconds >= 0.0
        && value.last_valid_callback_age_seconds < 0.5;
    value.frequency_streams.reserve(impl_->frequency_streams.size());
    for (const auto& stream : impl_->frequency_streams) {
        LiveAudioSnapshot::FrequencyStream item;
        item.uuid = stream->uuid;
        item.features.energy = stream->energy.load(std::memory_order_relaxed);
        item.features.onset = stream->onset.load(std::memory_order_relaxed);
        std::uint64_t stream_beat = 0U;
        for (;;) {
            const std::uint64_t before = stream->beat_generation.load(
                std::memory_order_acquire);
            if ((before & 1U) != 0U) continue;
            item.beat_count = stream->beat_count.load(
                std::memory_order_relaxed);
            item.detected_bpm = stream->bpm.load(std::memory_order_relaxed);
            stream_beat = stream->last_beat_frame.load(
                std::memory_order_relaxed);
            const std::uint64_t after = stream->beat_generation.load(
                std::memory_order_acquire);
            if (before == after) break;
        }
        if (item.beat_count != 0U) {
            item.beat_anchor_seconds = static_cast<double>(stream_beat)
                / kCaptureSampleRate;
            item.beat_position = static_cast<double>(item.beat_count - 1U);
            if (frames >= stream_beat) {
                const double seconds_since = static_cast<double>(
                    frames - stream_beat) / kCaptureSampleRate;
                const double period = item.detected_bpm > 0.0
                                          ? 60.0 / item.detected_bpm : 0.5;
                if (item.detected_bpm > 0.0) {
                    const auto timing = live_beat_timing(
                        item.beat_count, frames, stream_beat,
                        kCaptureSampleRate, item.detected_bpm);
                    item.beat_position = timing.position;
                    item.beat_phase = timing.phase;
                }
                item.features.beat = unit(static_cast<float>(
                    1.0 - seconds_since / std::min(0.16, period)));
            }
        }
        value.frequency_streams.push_back(std::move(item));
    }
    return value;
}

bool LiveAudioCapture::set_processing_config(
    const AudioInputProcessingConfig& config, std::string* error) {
    if (error != nullptr) error->clear();
    if (is_running()) {
        return fail(error,
                    "Stop live audio before changing its input processing.");
    }
    AudioInputProcessor prepared_input;
    if (!prepared_input.configure(config, kCaptureSampleRate, error)) {
        return false;
    }
    if (config.frequency_streams.size() > kMaximumAudioFrequencyStreams) {
        return fail(error,
                    "The frequency-stream table exceeds its real-time limit.");
    }
    std::vector<std::unique_ptr<Impl::FrequencyStreamState>> prepared_streams;
    prepared_streams.reserve(config.frequency_streams.size());
    for (const auto& authored : config.frequency_streams) {
        auto stream = std::make_unique<Impl::FrequencyStreamState>();
        stream->uuid = authored.uuid;
        if (!stream->filter.configure(authored.low_hz, authored.high_hz,
                                      kCaptureSampleRate, error)) {
            return false;
        }
        prepared_streams.push_back(std::move(stream));
    }
    impl_->processing_config = config;
    impl_->input_processor = std::move(prepared_input);
    impl_->frequency_streams = std::move(prepared_streams);
    return true;
}

void LiveAudioCapture::set_gain(double requested) noexcept {
    if (!std::isfinite(requested)) return;
    impl_->gain.store(static_cast<float>(std::clamp(
                          requested, 0.0,
                          static_cast<double>((std::numeric_limits<float>::max)()))),
                      std::memory_order_relaxed);
}

void LiveAudioCapture::set_sensitivity(double requested) noexcept {
    if (!std::isfinite(requested)) return;
    impl_->sensitivity.store(
        static_cast<float>(std::clamp(
            requested, 0.0,
            static_cast<double>((std::numeric_limits<float>::max)()))),
        std::memory_order_relaxed);
}

} // namespace pvt::audio
