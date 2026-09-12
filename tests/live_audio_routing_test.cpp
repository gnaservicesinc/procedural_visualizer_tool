#include "audio_playback.h"
#include "live_audio_capture.h"

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
void write_tone(const std::filesystem::path& path, float level) {
    // 20 ms of stereo PCM. Tests read well beyond EOF to verify endless looping.
    constexpr std::uint32_t bytes = 960U * 2U * sizeof(float);
    std::ofstream file(path, std::ios::binary);
    const auto u16 = [&](std::uint16_t value) {
        const char data[]{static_cast<char>(value), static_cast<char>(value >> 8)};
        file.write(data, 2);
    };
    const auto u32 = [&](std::uint32_t value) {
        const char data[]{static_cast<char>(value), static_cast<char>(value >> 8),
                          static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
        file.write(data, 4);
    };
    file.write("RIFF", 4); u32(bytes + 36); file.write("WAVEfmt ", 8);
    u32(16); u16(3); u16(2); u32(48000); u32(48000 * 8); u16(8); u16(32);
    file.write("data", 4); u32(bytes);
    for (int i = 0; i < 1920; ++i) file.write(reinterpret_cast<const char*>(&level), sizeof(level));
}
}

int main() {
    const auto directory = std::filesystem::temp_directory_path()
        / ("pvt-audio-routing-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } cleanup{directory};
    const auto positive = directory / "positive.wav";
    const auto negative = directory / "negative.wav";
    write_tone(positive, 0.25F);
    write_tone(negative, -0.25F);
    int failures = 0;
    const auto check = [&](bool passed, const char* message) {
        if (!passed) { ++failures; std::cerr << message << '\n'; }
    };
    std::string error;
    pvt::audio::PlaybackTrack track;
    track.path = positive.string(); track.loop = true;
    pvt::audio::AudioPlayback playback;
    check(playback.prepare_stream({track}, &error), error.c_str());
    std::array<float, 8192> samples{};
    for (int i = 0; i < 8; ++i) playback.read_stream(samples.data(), 4096);
    check(playback.is_playing(), "Clock file stopped at EOF");
    check(std::abs(playback.position_seconds() - 32768.0 / 48000) < 1e-6, "Clock stream cursor did not advance");
    check(std::abs(samples.back() - 0.25F) < 1e-5, "Looped clock audio changed its samples");
    playback.stop();
    playback.read_stream(samples.data(), 4096);
    check(samples.front() == 0 && samples.back() == 0, "Stopped stream did not produce silence");

    pvt::audio::LiveAudioCapture audio;
    pvt::audio::LiveAudioSourceRoute a{"song", {}, positive.string(), 1.0F, true};
    pvt::audio::LiveAudioSourceRoute b{"other", {}, negative.string(), 1.0F, true};
    const auto wait = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (audio.snapshot().stream_seconds < 0.15 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return audio.snapshot();
    };
    check(audio.start_routing({a}, {}, 256, &error), error.c_str());
    auto snapshot = wait();
    check(snapshot.receiving && snapshot.stream_seconds >= 0.15, "Looping file did not reach live analysis");
    check(std::abs(snapshot.pre_gate_rms - 0.25F) < 0.01, "Analyzer did not receive the selected clock stream");
    check(!audio.start_routing({a, a}, {}, 256, &error) && audio.is_running(), "Duplicate source IDs replaced an active route");
    check(!audio.start_routing({a}, {{"", {"missing"}}}, 256, &error) && audio.is_running(), "Unknown output source was accepted");
    check(!audio.start_routing({a}, {{"", {"song"}}, {"", {"song"}}}, 256, &error), "Duplicate output devices were accepted");
    check(!audio.start_routing({a}, {}, 8193, &error) && audio.is_running(), "Invalid period disturbed active audio");
    check(audio.start_routing({a, b}, {}, 256, &error), error.c_str());
    snapshot = wait();
    check(snapshot.receiving && snapshot.pre_gate_rms < 0.0001, "Analysis did not mix both selected sources");
    b.analysis = false;
    a.gain = 2.0F;
    check(audio.start_routing({a, b}, {}, 256, &error), error.c_str());
    snapshot = wait();
    check(std::abs(snapshot.pre_gate_rms - 0.5F) < 0.01, "Analysis selection or source gain was ignored");
    for (int i = 0; i < 3; ++i) {
        audio.stop();
        check(!audio.is_running(), "Audio routing did not stop");
        check(audio.start_routing({a}, {}, 128, &error), error.c_str());
        check(wait().receiving, "Audio routing did not restart");
    }
    audio.stop();
    const auto stopped = audio.snapshot().stream_seconds;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    check(audio.snapshot().stream_seconds == stopped, "Mixer kept running after stop");
    return failures == 0 ? 0 : 1;
}
