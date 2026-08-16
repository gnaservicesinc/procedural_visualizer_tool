#include "procedural_visualizer_tool.h"
#include "video_export.h"

#import <AVFoundation/AVFoundation.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace {

namespace fs = std::filesystem;

void write_u16(std::ofstream& output, std::uint16_t value) {
    const std::array<char, 2> bytes = {
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU)};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_u32(std::ofstream& output, std::uint32_t value) {
    const std::array<char, 4> bytes = {
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 24U) & 0xffU)};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

bool write_wav(const fs::path& path) {
    constexpr std::uint32_t sample_rate = 48000U;
    constexpr std::uint16_t channels = 1U;
    constexpr std::uint16_t bits = 16U;
    constexpr std::uint32_t frames = 4800U;
    constexpr std::uint32_t data_bytes = frames * channels * (bits / 8U);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write("RIFF", 4);
    write_u32(output, 36U + data_bytes);
    output.write("WAVEfmt ", 8);
    write_u32(output, 16U);
    write_u16(output, 1U);
    write_u16(output, channels);
    write_u32(output, sample_rate);
    write_u32(output, sample_rate * channels * (bits / 8U));
    write_u16(output, channels * (bits / 8U));
    write_u16(output, bits);
    output.write("data", 4);
    write_u32(output, data_bytes);
    for (std::uint32_t frame = 0U; frame < frames; ++frame) {
        write_u16(output, 0U);
    }
    output.flush();
    return static_cast<bool>(output);
}

std::vector<char> read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

bool inspect_movie(const fs::path& path, bool expect_audio,
                   std::string& error) {
    @autoreleasepool {
        NSString* native = [NSString stringWithUTF8String:path.string().c_str()];
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:
            [NSURL fileURLWithPath:native] options:nil];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        const NSUInteger videos =
            [asset tracksWithMediaType:AVMediaTypeVideo].count;
        const NSUInteger audios =
            [asset tracksWithMediaType:AVMediaTypeAudio].count;
#pragma clang diagnostic pop
        if (videos != 1U || (expect_audio ? audios != 1U : audios != 0U)) {
            error = "Movie track layout is incorrect.";
            return false;
        }
        if (!CMTIME_IS_NUMERIC(asset.duration)
            || CMTimeGetSeconds(asset.duration) <= 0.0) {
            error = "Movie duration is missing.";
            return false;
        }
        return true;
    }
}

} // namespace

int main() {
    @autoreleasepool {
        const pvt::video::Capabilities available = pvt::video::capabilities();
        if (!available.available || !available.png_lossless
            || !available.prores_4444) {
            std::cerr << "Required native video formats are unavailable: "
                      << available.status << '\n';
            return 1;
        }
        std::array<char, 128> directory_template{};
        const std::string pattern =
            (fs::temp_directory_path() / "pvt-video-test-XXXXXX").string();
        std::copy(pattern.begin(), pattern.end(), directory_template.begin());
        char* created = ::mkdtemp(directory_template.data());
        if (created == nullptr) {
            std::cerr << "Could not create video test directory.\n";
            return 1;
        }
        const fs::path directory(created);
        struct Cleanup {
            fs::path path;
            ~Cleanup() {
                std::error_code ignored;
                fs::remove_all(path, ignored);
            }
        } cleanup{directory};

        pvt::ProjectConfig project = pvt::default_project();
        project.canvas.width = 192;
        project.canvas.height = 108;
        project.canvas.block_size = 6;
        project.canvas.total_frames = 2;
        project.canvas.fps = 24.0;
        project.output.write_alpha = true;
        const fs::path music = directory / "silence.wav";
        if (!write_wav(music)) {
            std::cerr << "Could not create audio fixture.\n";
            return 1;
        }

        std::atomic_bool cancel{false};
        pvt::video::Options options;
        options.codec = pvt::video::Codec::PngLossless;
        options.preserve_alpha = true;
        options.include_project_music = true;
        options.music_source_path = music.string();
        options.frame.backend = pvt::RenderBackend::Cpu;
        int last_progress = -1;
        pvt::video::Report report;
        std::string error;
        const fs::path lossless = directory / "lossless.mov";
        if (!pvt::video::export_project(
                project, lossless.string(), options,
                [&last_progress](int completed, int total) {
                    if (completed < last_progress || completed > total) return false;
                    last_progress = completed;
                    return true;
                }, &cancel, &report, &error)
            || last_progress != 2 || !report.included_audio
            || report.render_workers != 2U
            || !inspect_movie(lossless, true, error)) {
            std::cerr << "Lossless native video export failed: " << error << '\n';
            return 1;
        }
        const std::vector<char> original = read_file(lossless);
        if (original.empty()) {
            std::cerr << "Lossless movie is empty.\n";
            return 1;
        }
        options.first_frame = 1;
        options.frame_count = 1;
        int segment_progress = -1;
        const fs::path segment = directory / "segment.mov";
        if (!pvt::video::export_project(
                project, segment.string(), options,
                [&segment_progress](int completed, int total) {
                    if (total != 1 || completed < segment_progress) return false;
                    segment_progress = completed;
                    return true;
                }, &cancel, &report, &error)
            || segment_progress != 1 || !report.included_audio
            || !inspect_movie(segment, true, error)) {
            std::cerr << "Ranged video/audio export failed: " << error << '\n';
            return 1;
        }
        options.first_frame = 0;
        options.frame_count = 0;
        if (pvt::video::export_project(project, lossless.string(), options, {},
                                       &cancel, nullptr, &error)
            || read_file(lossless) != original) {
            std::cerr << "No-clobber video export changed an existing movie.\n";
            return 1;
        }
        if (::chmod(lossless.c_str(), 0640) != 0) {
            std::cerr << "Could not prepare video permission-preservation test.\n";
            return 1;
        }
        options.overwrite_existing = true;
        options.memory_budget_bytes = 1U;
        if (!pvt::video::export_project(project, lossless.string(), options, {},
                                        &cancel, &report, &error)
            || report.render_workers != 1U) {
            std::cerr << "Atomic video replacement failed: " << error << '\n';
            return 1;
        }
        // AVAssetWriter may legitimately vary container metadata and media
        // interleaving between runs. Verify the installed movie semantically;
        // pvt_core separately compares single- and multi-worker frame pixels.
        if (!inspect_movie(lossless, true, error)) {
            std::cerr << "Single-worker replacement movie is invalid: "
                      << error << '\n';
            return 1;
        }
        struct stat replaced{};
        if (::stat(lossless.c_str(), &replaced) != 0
            || (replaced.st_mode & 07777) != 0640) {
            std::cerr << "Video replacement did not preserve permissions.\n";
            return 1;
        }

        options.overwrite_existing = false;
        options.memory_budget_bytes = 0U;
        options.include_project_music = false;
        options.music_source_path.clear();
        options.worker_count = pvt::kMaximumSequenceWorkers + 1U;
        const fs::path invalid_workers = directory / "invalid-workers.mov";
        if (pvt::video::export_project(
                project, invalid_workers.string(), options, {}, &cancel,
                nullptr, &error)
            || fs::exists(invalid_workers)) {
            std::cerr << "Invalid video worker count was accepted.\n";
            return 1;
        }
        options.worker_count = 0U;
        const fs::path cancelled_movie = directory / "cancelled.mov";
        if (pvt::video::export_project(
                project, cancelled_movie.string(), options,
                [](int completed, int) { return completed < 1; },
                &cancel, nullptr, &error)
            || fs::exists(cancelled_movie)) {
            std::cerr << "Cancelled video export installed a destination.\n";
            return 1;
        }

        options.codec = pvt::video::Codec::ProRes4444;
        options.hardware = pvt::video::HardwarePolicy::Prefer;
        options.preserve_alpha = true;
        options.include_project_music = false;
        options.music_source_path.clear();
        const fs::path prores = directory / "prores.mov";
        if (!pvt::video::export_project(project, prores.string(), options, {},
                                        &cancel, &report, &error)
            || !inspect_movie(prores, false, error)) {
            std::cerr << "ProRes native video export failed: " << error << '\n';
            return 1;
        }
        if (available.hevc) {
            options.codec = pvt::video::Codec::Hevc;
            options.preserve_alpha = false;
            options.hardware = available.hevc_hardware
                                   ? pvt::video::HardwarePolicy::Require
                                   : pvt::video::HardwarePolicy::Prefer;
            options.hevc_quality =
                pvt::video::HevcQuality::MaximumFidelity;
            const fs::path hevc = directory / "hevc.mov";
            if (!pvt::video::export_project(project, hevc.string(), options, {},
                                            &cancel, &report, &error)
                || (available.hevc_hardware && !report.hardware_required)
                || !inspect_movie(hevc, false, error)) {
                std::cerr << "HEVC native video export failed: " << error << '\n';
                return 1;
            }
        }
        std::cout << "Native video export checks passed.\n";
        return 0;
    }
}
