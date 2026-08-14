#include "audio_analysis.h"
#include "audio_playback.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << __FILE__ << ':' << __LINE__                            \
                      << ": check failed: " #condition << '\n';                \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto seed = std::chrono::high_resolution_clock::now()
                              .time_since_epoch().count();
        for (std::uint64_t attempt = 0U; attempt < 100U; ++attempt) {
            path_ = fs::temp_directory_path()
                    / ("pvt-audio-analysis-" + std::to_string(seed) + "-"
                       + std::to_string(attempt));
            std::error_code error;
            if (fs::create_directory(path_, error)) {
                return;
            }
        }
        throw std::runtime_error("could not create audio test directory");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void write_u16(std::ostream& output, std::uint16_t value) {
    const std::array<char, 2U> bytes {
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU)};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_u32(std::ostream& output, std::uint32_t value) {
    const std::array<char, 4U> bytes {
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 24U) & 0xffU)};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_wave_header(std::ostream& output,
                       std::uint16_t encoding,
                       std::uint16_t channels,
                       std::uint32_t sample_rate,
                       std::uint16_t bits_per_sample,
                       std::uint32_t data_bytes) {
    output.write("RIFF", 4);
    write_u32(output, 36U + data_bytes);
    output.write("WAVEfmt ", 8);
    write_u32(output, 16U);
    write_u16(output, encoding);
    write_u16(output, channels);
    write_u32(output, sample_rate);
    const std::uint16_t block_align =
        static_cast<std::uint16_t>(channels * (bits_per_sample / 8U));
    write_u32(output, sample_rate * block_align);
    write_u16(output, block_align);
    write_u16(output, bits_per_sample);
    output.write("data", 4);
    write_u32(output, data_bytes);
}

bool write_pcm16_wave(const fs::path& path,
                      std::uint32_t sample_rate,
                      std::uint16_t channels,
                      const std::vector<float>& samples,
                      std::uint32_t declared_extra_bytes = 0U) {
    if (channels == 0U || samples.size() % channels != 0U
        || samples.size() > (std::numeric_limits<std::uint32_t>::max)() / 2U) {
        return false;
    }
    const std::uint32_t actual_bytes =
        static_cast<std::uint32_t>(samples.size() * 2U);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    write_wave_header(output, 1U, channels, sample_rate, 16U,
                      actual_bytes + declared_extra_bytes);
    for (float sample : samples) {
        const float limited = (std::max)(-1.0F, (std::min)(1.0F, sample));
        const std::int16_t encoded = static_cast<std::int16_t>(
            std::lrint(static_cast<double>(limited) * 32767.0));
        write_u16(output, static_cast<std::uint16_t>(encoded));
    }
    return static_cast<bool>(output);
}

bool write_float32_wave(const fs::path& path,
                        std::uint32_t sample_rate,
                        std::uint16_t channels,
                        const std::vector<float>& samples) {
    if (channels == 0U || samples.size() % channels != 0U
        || samples.size() > (std::numeric_limits<std::uint32_t>::max)() / 4U) {
        return false;
    }
    const std::uint32_t data_bytes =
        static_cast<std::uint32_t>(samples.size() * 4U);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    write_wave_header(output, 3U, channels, sample_rate, 32U, data_bytes);
    for (float sample : samples) {
        std::uint32_t bits = 0U;
        static_assert(sizeof(bits) == sizeof(sample), "float32 test fixture required");
        std::memcpy(&bits, &sample, sizeof(bits));
        write_u32(output, bits);
    }
    return static_cast<bool>(output);
}

std::vector<float> click_track(std::uint32_t sample_rate,
                               std::uint16_t channels,
                               double seconds,
                               const std::vector<double>& click_times) {
    const std::size_t frames = static_cast<std::size_t>(
        std::llround(seconds * static_cast<double>(sample_rate)));
    std::vector<float> samples(frames * channels, 0.0F);
    for (double click_time : click_times) {
        const std::size_t frame = static_cast<std::size_t>(
            std::llround(click_time * static_cast<double>(sample_rate)));
        for (std::size_t pulse = 0U; pulse < 12U && frame + pulse < frames; ++pulse) {
            const float value = static_cast<float>(0.95 * std::exp(
                -static_cast<double>(pulse) / 3.0));
            for (std::size_t channel = 0U; channel < channels; ++channel) {
                samples[(frame + pulse) * channels + channel] =
                    channel == 0U ? value : value * 0.8F;
            }
        }
    }
    return samples;
}

std::vector<double> constant_clicks(double first,
                                    double end,
                                    double interval) {
    std::vector<double> result;
    for (double time = first; time < end; time += interval) {
        result.push_back(time);
    }
    return result;
}

bool contains_tempo(const pvt::MusicAnalysis& analysis,
                    double expected,
                    double tolerance) {
    return std::any_of(analysis.tempo_points.begin(), analysis.tempo_points.end(),
                       [expected, tolerance](const pvt::MusicTempoPoint& point) {
                           return std::abs(point.bpm - expected) <= tolerance;
                       });
}

double maximum_nearest_error(const std::vector<double>& detected,
                             const std::vector<double>& expected) {
    double maximum = 0.0;
    for (double target : expected) {
        double nearest = (std::numeric_limits<double>::infinity)();
        for (double value : detected) {
            nearest = (std::min)(nearest, std::abs(value - target));
        }
        maximum = (std::max)(maximum, nearest);
    }
    return maximum;
}

void check_beat_bounds(const pvt::MusicAnalysis& analysis) {
    CHECK(!analysis.beat_times_seconds.empty());
    double previous = -1.0;
    for (double beat : analysis.beat_times_seconds) {
        CHECK(std::isfinite(beat));
        CHECK(beat >= 0.0);
        CHECK(beat <= analysis.duration_seconds);
        CHECK(beat > previous);
        previous = beat;
    }
}

void test_pcm_click_track_and_digest(const fs::path& directory) {
    const fs::path path = directory / "click-120.wav";
    const std::vector<double> clicks = constant_clicks(0.25, 5.0, 0.5);
    CHECK(write_pcm16_wave(path, 44100U, 1U,
                           click_track(44100U, 1U, 5.0, clicks)));

    pvt::MusicAnalysis analysis;
    std::string error;
    std::uint64_t previous = 0U;
    int progress_calls = 0;
    CHECK(pvt::audio::analyze_music_file(
        path.string(), analysis,
        [&previous, &progress_calls](std::uint64_t completed, std::uint64_t total) {
            CHECK(total == 1000U);
            CHECK(completed >= previous);
            previous = completed;
            ++progress_calls;
            return true;
        },
        nullptr, &error));
    CHECK(error.empty());
    check_beat_bounds(analysis);
    CHECK(previous == 1000U);
    CHECK(progress_calls > 4);
    CHECK(analysis.analyzer_version == "pvt-adaptive-spectral-audio-3");
    CHECK(analysis.source_format == "WAV");
    CHECK(analysis.source_basename == "click-120.wav");
    CHECK(analysis.source_sample_rate == 44100U);
    CHECK(analysis.source_channel_count == 1U);
    CHECK(analysis.source_frame_count == 220500U);
    CHECK(std::abs(analysis.duration_seconds - 5.0) < 1.0e-9);
    CHECK(analysis.source_sha256.size() == 64U);
    CHECK(analysis.feature_samples.size() <= pvt::kMaximumMusicFeatureSamples);
    CHECK(analysis.feature_samples.size() >= 400U);
    CHECK(analysis.beat_times_seconds.size() >= 8U);
    CHECK(std::abs(analysis.detected_bpm - 120.0) < 2.0);
    CHECK(analysis.tempo_confidence > 0.5);
    CHECK(contains_tempo(analysis, 120.0, 2.0));
    const double beat_error = maximum_nearest_error(
        analysis.beat_times_seconds, clicks);
    CHECK(beat_error < 0.002);
    std::cout << "PCM16 120 BPM: " << analysis.beat_times_seconds.size()
              << " beats, BPM error " << std::abs(analysis.detected_bpm - 120.0)
              << ", maximum beat error " << beat_error * 1000.0 << " ms\n";

    CHECK(pvt::audio::verify_music_source(path.string(), analysis.source_sha256,
                                          {}, nullptr, &error));
    CHECK(error.empty());
    std::string uppercase = analysis.source_sha256;
    std::transform(uppercase.begin(), uppercase.end(), uppercase.begin(), [](char value) {
        if (value >= 'a' && value <= 'f') {
            return static_cast<char>(value - ('a' - 'A'));
        }
        return value;
    });
    CHECK(pvt::audio::verify_music_source(path.string(), uppercase,
                                          {}, nullptr, &error));
    std::string mismatch(64U, '0');
    CHECK(!pvt::audio::verify_music_source(path.string(), mismatch,
                                           {}, nullptr, &error));
    CHECK(error.find("does not match") != std::string::npos);
    CHECK(!pvt::audio::verify_music_source(path.string(), "bad",
                                           {}, nullptr, &error));
    CHECK(error.find("invalid") != std::string::npos);
}

void test_fractional_duration_beat_bounds(const fs::path& directory) {
    constexpr double duration = 5.017007;
    const fs::path path = directory / "fractional-duration.wav";
    const std::vector<double> clicks = constant_clicks(0.25, duration, 2.0 / 3.0);
    CHECK(write_pcm16_wave(path, 44100U, 1U,
                           click_track(44100U, 1U, duration, clicks)));
    pvt::MusicAnalysis analysis;
    std::string error;
    CHECK(pvt::audio::analyze_music_file(path.string(), analysis,
                                         {}, nullptr, &error));
    CHECK(error.empty());
    check_beat_bounds(analysis);
}

void test_float32_tempo_change(const fs::path& directory) {
    const fs::path path = directory / "float-tempo-change.wav";
    std::vector<double> clicks = constant_clicks(0.25, 4.0, 0.5);
    const std::vector<double> faster = constant_clicks(4.083333333, 8.0, 1.0 / 3.0);
    clicks.insert(clicks.end(), faster.begin(), faster.end());
    CHECK(write_float32_wave(path, 48000U, 2U,
                             click_track(48000U, 2U, 8.0, clicks)));

    pvt::MusicAnalysis analysis;
    std::string error;
    CHECK(pvt::audio::analyze_music_file(path.string(), analysis,
                                         {}, nullptr, &error));
    CHECK(error.empty());
    CHECK(analysis.source_format == "WAV");
    CHECK(analysis.source_sample_rate == 48000U);
    CHECK(analysis.source_channel_count == 2U);
    CHECK(analysis.source_frame_count == 384000U);
    CHECK(std::abs(analysis.duration_seconds - 8.0) < 1.0e-9);
    CHECK(analysis.beat_times_seconds.size() >= 16U);
    CHECK(contains_tempo(analysis, 120.0, 3.0));
    CHECK(contains_tempo(analysis, 180.0, 4.0));
    CHECK(analysis.tempo_points.size() <= pvt::kMaximumMusicTempoPoints);
    const double beat_error = maximum_nearest_error(
        analysis.beat_times_seconds, clicks);
    CHECK(beat_error < 0.003);
    std::cout << "Float32 120-to-180 BPM: "
              << analysis.beat_times_seconds.size()
              << " beats, maximum beat error " << beat_error * 1000.0
              << " ms, " << analysis.tempo_points.size()
              << " tempo segments\n";
}

void test_subdivision_accents_and_missing_beat(const fs::path& directory) {
    const fs::path path = directory / "accented-subdivisions.wav";
    const std::vector<double> expected = constant_clicks(0.25, 8.0, 0.5);
    std::vector<double> audible_main = expected;
    audible_main.erase(std::remove_if(audible_main.begin(), audible_main.end(),
                                      [](double value) {
                                          return std::abs(value - 3.25) < 0.001;
                                      }),
                        audible_main.end());
    const std::vector<double> subdivisions = constant_clicks(0.50, 8.0, 0.5);
    std::vector<float> samples = click_track(44100U, 1U, 8.0, audible_main);
    const std::vector<float> quiet = click_track(44100U, 1U, 8.0, subdivisions);
    for (std::size_t index = 0U; index < samples.size(); ++index) {
        samples[index] += quiet[index] * 0.24F;
    }
    CHECK(write_pcm16_wave(path, 44100U, 1U, samples));

    pvt::MusicAnalysis analysis;
    std::string error;
    CHECK(pvt::audio::analyze_music_file(path.string(), analysis,
                                         {}, nullptr, &error));
    CHECK(error.empty());
    CHECK(std::abs(analysis.detected_bpm - 120.0) < 2.0);
    CHECK(analysis.beat_times_seconds.size() >= expected.size() - 1U);
    CHECK(analysis.beat_times_seconds.size() <= expected.size() + 1U);
    const double beat_error = maximum_nearest_error(
        analysis.beat_times_seconds, expected);
    CHECK(beat_error < 0.003);
    CHECK(analysis.tempo_confidence > 0.5);
    std::cout << "Accented eighth notes with one missing beat: "
              << analysis.beat_times_seconds.size()
              << " tracked beats, BPM error "
              << std::abs(analysis.detected_bpm - 120.0)
              << ", maximum grid error " << beat_error * 1000.0 << " ms\n";
}

void test_ninety_bpm_with_eighth_notes(const fs::path& directory) {
    constexpr double interval = 2.0 / 3.0;
    const fs::path path = directory / "ninety-bpm-polyrhythm.wav";
    const std::vector<double> expected = constant_clicks(0.50, 16.0, interval);
    std::vector<double> audible = expected;
    audible.erase(std::remove_if(audible.begin(), audible.end(), [](double value) {
                      return std::abs(value - (0.50 + 8.0 * interval)) < 0.001;
                  }),
                  audible.end());
    std::vector<double> eighths;
    for (double time = 0.50 + interval * 0.5; time < 16.0;
         time += interval) {
        eighths.push_back(time);
    }
    std::vector<float> samples = click_track(44100U, 1U, 16.0, audible);
    const std::vector<float> quiet = click_track(44100U, 1U, 16.0, eighths);
    for (std::size_t index = 0U; index < samples.size(); ++index) {
        samples[index] += quiet[index] * 0.28F;
    }
    CHECK(write_pcm16_wave(path, 44100U, 1U, samples));

    pvt::MusicAnalysis analysis;
    std::string error;
    CHECK(pvt::audio::analyze_music_file(path.string(), analysis,
                                         {}, nullptr, &error));
    CHECK(error.empty());
    CHECK(std::abs(analysis.detected_bpm - 90.0) < 2.0);
    CHECK(contains_tempo(analysis, 90.0, 2.0));
    CHECK(!contains_tempo(analysis, 180.0, 3.0));
    const double beat_error = maximum_nearest_error(
        analysis.beat_times_seconds, expected);
    CHECK(beat_error < 0.003);
    CHECK(analysis.beat_times_seconds.size() >= expected.size() - 1U);
    std::cout << "Adaptive 90 BPM with eighth notes: "
              << analysis.beat_times_seconds.size()
              << " beats, BPM error "
              << std::abs(analysis.detected_bpm - 90.0)
              << ", maximum grid error " << beat_error * 1000.0 << " ms\n";
}

void test_continuous_tempo_ramp(const fs::path& directory) {
    const fs::path path = directory / "tempo-ramp.wav";
    std::vector<double> clicks;
    double time = 0.35;
    while (time < 14.0) {
        clicks.push_back(time);
        const double bpm = 90.0 + 60.0 * (time / 14.0);
        time += 60.0 / bpm;
    }
    CHECK(write_pcm16_wave(path, 44100U, 1U,
                           click_track(44100U, 1U, 14.0, clicks)));

    pvt::MusicAnalysis analysis;
    std::string error;
    CHECK(pvt::audio::analyze_music_file(path.string(), analysis,
                                         {}, nullptr, &error));
    CHECK(error.empty());
    CHECK(contains_tempo(analysis, 100.0, 10.0));
    CHECK(contains_tempo(analysis, 140.0, 10.0));
    CHECK(analysis.tempo_points.size() >= 3U);
    CHECK(analysis.tempo_points.front().bpm
          < analysis.tempo_points.back().bpm);
    const double beat_error = maximum_nearest_error(
        analysis.beat_times_seconds, clicks);
    CHECK(beat_error < 0.006);
    std::cout << "Continuous 90-to-150 BPM ramp: "
              << analysis.tempo_points.size()
              << " local tempo points, maximum beat error "
              << beat_error * 1000.0 << " ms\n";
}

double average_feature(const pvt::MusicAnalysis& analysis,
                       double begin_seconds,
                       double end_seconds,
                       float pvt::MusicFeatureSample::*member) {
    if (analysis.feature_samples.empty() || !(analysis.duration_seconds > 0.0)) {
        return 0.0;
    }
    const std::size_t begin = static_cast<std::size_t>(
        begin_seconds / analysis.duration_seconds
        * static_cast<double>(analysis.feature_samples.size()));
    const std::size_t end = (std::min)(
        analysis.feature_samples.size(), static_cast<std::size_t>(
            end_seconds / analysis.duration_seconds
            * static_cast<double>(analysis.feature_samples.size())));
    double sum = 0.0;
    for (std::size_t index = begin; index < end; ++index) {
        sum += analysis.feature_samples[index].*member;
    }
    return end > begin ? sum / static_cast<double>(end - begin) : 0.0;
}

void test_spectral_palette_controls_and_quiet_tone(const fs::path& directory) {
    constexpr std::uint32_t sample_rate = 44100U;
    constexpr double duration = 6.0;
    const std::size_t frames = static_cast<std::size_t>(duration * sample_rate);
    std::vector<float> samples(frames, 0.0F);
    std::uint32_t noise = 0x12345678U;
    for (std::size_t frame = 0U; frame < frames; ++frame) {
        const double seconds = static_cast<double>(frame) / sample_rate;
        if (seconds < 2.0) {
            samples[frame] = static_cast<float>(
                0.06 * std::sin(2.0 * 3.14159265358979323846 * 220.0 * seconds));
        } else if (seconds < 4.0) {
            samples[frame] = static_cast<float>(
                0.06 * std::sin(2.0 * 3.14159265358979323846 * 880.0 * seconds));
        } else {
            noise = noise * 1664525U + 1013904223U;
            const double unit = static_cast<double>(noise >> 8U)
                                / static_cast<double>(0x00ffffffU);
            samples[frame] = static_cast<float>(0.035 * (2.0 * unit - 1.0));
        }
    }
    const fs::path path = directory / "quiet-spectral-controls.wav";
    CHECK(write_float32_wave(path, sample_rate, 1U, samples));

    pvt::MusicAnalysis analysis;
    std::string error;
    CHECK(pvt::audio::analyze_music_file(path.string(), analysis,
                                         {}, nullptr, &error));
    CHECK(error.empty());
    const double low_centroid = average_feature(
        analysis, 0.5, 1.8, &pvt::MusicFeatureSample::spectral_centroid);
    const double high_centroid = average_feature(
        analysis, 2.5, 3.8, &pvt::MusicFeatureSample::spectral_centroid);
    const double tone_flatness = average_feature(
        analysis, 0.5, 3.8, &pvt::MusicFeatureSample::spectral_flatness);
    const double noise_flatness = average_feature(
        analysis, 4.4, 5.8, &pvt::MusicFeatureSample::spectral_flatness);
    const double tone_chroma = average_feature(
        analysis, 0.5, 3.8, &pvt::MusicFeatureSample::chroma_strength);
    const double noise_chroma = average_feature(
        analysis, 4.4, 5.8, &pvt::MusicFeatureSample::chroma_strength);
    CHECK(high_centroid > low_centroid + 0.015);
    CHECK(noise_flatness > tone_flatness + 0.20);
    CHECK(tone_chroma > noise_chroma + 0.10);
    const std::size_t tonal_beats = static_cast<std::size_t>(std::count_if(
        analysis.beat_times_seconds.begin(), analysis.beat_times_seconds.end(),
        [](double beat) { return beat < 3.9; }));
    CHECK(tonal_beats <= 4U);
    std::cout << "Spectral palette controls: centroid " << low_centroid
              << " -> " << high_centroid << ", flatness " << tone_flatness
              << " -> " << noise_flatness << ", tonal strength "
              << tone_chroma << " vs noise " << noise_chroma << '\n';
}

void test_portable_source_boundary_and_links(const fs::path& directory) {
    const fs::path oversized = directory / "oversized.wav";
    {
        std::ofstream output(oversized, std::ios::binary | std::ios::trunc);
        CHECK(static_cast<bool>(output));
    }
    std::error_code filesystem_error;
    fs::resize_file(oversized, pvt::kMaximumEmbeddedAssetBytes + 1U,
                    filesystem_error);
    CHECK(!filesystem_error);
    pvt::MusicAnalysis destination;
    destination.analyzer_version = "unchanged";
    std::string error;
    CHECK(!pvt::audio::analyze_music_file(oversized.string(), destination,
                                          {}, nullptr, &error));
    CHECK(error.find("signed-int portable attachment limit") != std::string::npos);
    CHECK(destination.analyzer_version == "unchanged");

    const fs::path target = directory / "link-target.wav";
    CHECK(write_pcm16_wave(target, 44100U, 1U,
                           click_track(44100U, 1U, 1.0, {0.25})));
    const fs::path link = directory / "linked-source.wav";
    fs::create_symlink(target, link, filesystem_error);
    if (!filesystem_error) {
        CHECK(!pvt::audio::analyze_music_file(link.string(), destination,
                                              {}, nullptr, &error));
        CHECK(error.find("link") != std::string::npos);
        CHECK(destination.analyzer_version == "unchanged");
    }
}

void test_non_finite_and_truncated_are_transactional(const fs::path& directory) {
    const fs::path non_finite_path = directory / "non-finite.wav";
    std::vector<float> samples(44100U, 0.0F);
    samples[100U] = (std::numeric_limits<float>::quiet_NaN)();
    CHECK(write_float32_wave(non_finite_path, 44100U, 1U, samples));

    pvt::MusicAnalysis destination;
    destination.analyzer_version = "sentinel";
    destination.source_frame_count = 77U;
    std::string error;
    CHECK(!pvt::audio::analyze_music_file(non_finite_path.string(), destination,
                                          {}, nullptr, &error));
    CHECK(error.find("non-finite") != std::string::npos);
    CHECK(destination.analyzer_version == "sentinel");
    CHECK(destination.source_frame_count == 77U);

    const fs::path truncated_path = directory / "truncated.wav";
    CHECK(write_pcm16_wave(truncated_path, 44100U, 1U,
                           std::vector<float>(128U, 0.0F), 32U));
    CHECK(!pvt::audio::analyze_music_file(truncated_path.string(), destination,
                                          {}, nullptr, &error));
    CHECK(error.find("length") != std::string::npos
          || error.find("truncated") != std::string::npos
          || error.find("beyond") != std::string::npos);
    CHECK(destination.analyzer_version == "sentinel");
}

void test_cancellation(const fs::path& directory) {
    const fs::path path = directory / "cancel.wav";
    CHECK(write_pcm16_wave(path, 44100U, 1U,
                           click_track(44100U, 1U, 2.0,
                                       constant_clicks(0.25, 2.0, 0.5))));
    pvt::MusicAnalysis destination;
    destination.analyzer_version = "unchanged";
    std::string error;
    std::atomic_bool cancelled {true};
    CHECK(!pvt::audio::analyze_music_file(path.string(), destination,
                                          {}, &cancelled, &error));
    CHECK(error.find("cancelled") != std::string::npos);
    CHECK(destination.analyzer_version == "unchanged");

    cancelled.store(false, std::memory_order_relaxed);
    CHECK(!pvt::audio::analyze_music_file(
        path.string(), destination,
        [](std::uint64_t completed, std::uint64_t) { return completed < 150U; },
        &cancelled, &error));
    CHECK(error.find("cancelled") != std::string::npos);
    CHECK(destination.analyzer_version == "unchanged");
}

void test_known_sha256(const fs::path& directory) {
    const fs::path path = directory / "sha256-known-vector.bin";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write("abc", 3);
        CHECK(static_cast<bool>(output));
    }
    constexpr const char* expected =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    std::string error;
    CHECK(pvt::audio::verify_music_source(path.string(), expected,
                                          {}, nullptr, &error));
    CHECK(error.empty());
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write("abd", 3);
        CHECK(static_cast<bool>(output));
    }
    CHECK(!pvt::audio::verify_music_source(path.string(), expected,
                                           {}, nullptr, &error));
    CHECK(error.find("does not match") != std::string::npos);
}

void test_long_track_density_and_transient(const fs::path& directory) {
    constexpr std::uint32_t sample_rate = 44100U;
    constexpr double duration = 180.0;
    constexpr double transient_time = 90.007;
    const fs::path path = directory / "three-minute-transient.wav";
    const std::vector<float> samples = click_track(
        sample_rate, 1U, duration, std::vector<double> {transient_time});
    CHECK(write_pcm16_wave(path, sample_rate, 1U, samples));

    pvt::MusicAnalysis analysis;
    std::string error;
    CHECK(pvt::audio::analyze_music_file(path.string(), analysis,
                                         {}, nullptr, &error));
    CHECK(error.empty());
    CHECK(analysis.feature_samples.size() > 8192U);
    const double spacing = analysis.duration_seconds
                           / static_cast<double>(analysis.feature_samples.size());
    CHECK(spacing <= 0.0221);
    const auto strongest = std::max_element(
        analysis.feature_samples.begin(), analysis.feature_samples.end(),
        [](const pvt::MusicFeatureSample& first,
           const pvt::MusicFeatureSample& second) {
            return first.onset < second.onset;
        });
    CHECK(strongest != analysis.feature_samples.end());
    CHECK(strongest->onset > 0.9F);
    const std::size_t index = static_cast<std::size_t>(
        std::distance(analysis.feature_samples.begin(), strongest));
    const double represented_time =
        (static_cast<double>(index) + 0.5) * spacing;
    CHECK(std::abs(represented_time - transient_time) <= spacing + 0.0101);
    std::cout << "Three-minute feature track: "
              << analysis.feature_samples.size() << " samples, "
              << spacing * 1000.0 << " ms spacing, transient error "
              << std::abs(represented_time - transient_time) * 1000.0
              << " ms\n";
}

void test_synchronized_audio_mix(const fs::path& directory) {
    const fs::path source = directory / "mix-source.wav";
    std::vector<float> samples(4800U * 2U, 0.2F);
    CHECK(write_float32_wave(source, 48000U, 2U, samples));
    pvt::audio::PlaybackTrack straight;
    straight.path = source.string();
    straight.playback_rate = 0.5;
    straight.stop_after_seconds = 0.2;
    pvt::audio::PlaybackTrack looped;
    looped.path = source.string();
    looped.loop = true;
    const fs::path mix = directory / "synchronized-mix.wav";
    std::string error;
    CHECK(pvt::audio::write_mix_wav(
        {straight, looped}, 0.2, mix.string(), nullptr, &error));
    CHECK(error.empty() && fs::exists(mix));
    pvt::MusicAnalysis analysis;
    CHECK(pvt::audio::analyze_music_file(mix.string(), analysis,
                                         {}, nullptr, &error));
    CHECK(analysis.source_channel_count == 2U);
    CHECK(analysis.source_sample_rate == 48000U);
    CHECK(std::abs(analysis.duration_seconds - 0.2) < 1.0e-6);

    std::atomic_bool cancelled{true};
    const fs::path cancelled_mix = directory / "cancelled-mix.wav";
    CHECK(!pvt::audio::write_mix_wav(
        {looped}, 0.2, cancelled_mix.string(), &cancelled, &error));
    CHECK(!fs::exists(cancelled_mix));
}

void test_fractional_playback_rate_does_not_drift(const fs::path& directory) {
    constexpr std::uint32_t source_rate = 1000U;
    constexpr std::size_t source_frames = 480000U;
    constexpr std::size_t step_frame = 479850U;
    std::vector<float> samples(source_frames, 0.0F);
    std::fill(samples.begin() + static_cast<std::ptrdiff_t>(step_frame),
              samples.end(), 1.0F);
    const fs::path source = directory / "fractional-rate-source.wav";
    CHECK(write_float32_wave(source, source_rate, 1U, samples));

    pvt::audio::PlaybackTrack track;
    track.path = source.string();
    track.playback_rate = 47.99;
    const fs::path mix = directory / "fractional-rate-mix.wav";
    std::string error;
    CHECK(pvt::audio::write_mix_wav(
        {track}, 10.0, mix.string(), nullptr, &error));
    CHECK(error.empty());

    std::ifstream input(mix, std::ios::binary);
    CHECK(static_cast<bool>(input));
    input.seekg(44, std::ios::beg);
    std::size_t first_loud_frame = (std::numeric_limits<std::size_t>::max)();
    for (std::size_t frame = 0U; frame < 480000U && input; ++frame) {
        std::array<std::uint32_t, 2U> bits{};
        for (std::uint32_t& value : bits) {
            std::array<unsigned char, 4U> bytes{};
            input.read(reinterpret_cast<char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
            value = static_cast<std::uint32_t>(bytes[0U])
                    | (static_cast<std::uint32_t>(bytes[1U]) << 8U)
                    | (static_cast<std::uint32_t>(bytes[2U]) << 16U)
                    | (static_cast<std::uint32_t>(bytes[3U]) << 24U);
        }
        float left = 0.0F;
        std::memcpy(&left, &bits[0U], sizeof(left));
        if (left > 0.5F) {
            first_loud_frame = frame;
            break;
        }
    }
    CHECK(first_loud_frame != (std::numeric_limits<std::size_t>::max)());
    // Exact 47.99x timing places the step near frame 479950. Rounding the
    // decoder rate to 1000 Hz plays at 48x and moves it near frame 479850.
    CHECK(first_loud_frame > 479900U);
}

} // namespace

int main() {
    try {
        const TemporaryDirectory directory;
        test_pcm_click_track_and_digest(directory.path());
        test_fractional_duration_beat_bounds(directory.path());
        test_float32_tempo_change(directory.path());
        test_subdivision_accents_and_missing_beat(directory.path());
        test_ninety_bpm_with_eighth_notes(directory.path());
        test_continuous_tempo_ramp(directory.path());
        test_spectral_palette_controls_and_quiet_tone(directory.path());
        test_portable_source_boundary_and_links(directory.path());
        test_non_finite_and_truncated_are_transactional(directory.path());
        test_cancellation(directory.path());
        test_known_sha256(directory.path());
        test_long_track_density_and_transient(directory.path());
        test_synchronized_audio_mix(directory.path());
        test_fractional_playback_rate_does_not_drift(directory.path());
    } catch (const std::exception& exception) {
        std::cerr << "unexpected test exception: " << exception.what() << '\n';
        return 2;
    }
    if (failures != 0) {
        std::cerr << failures << " audio analysis check(s) failed\n";
        return 1;
    }
    std::cout << "audio analysis tests passed\n";
    return 0;
}
