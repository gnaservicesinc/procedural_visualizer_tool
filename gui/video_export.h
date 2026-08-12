#ifndef PVT_VIDEO_EXPORT_H
#define PVT_VIDEO_EXPORT_H

#include "procedural_visualizer_tool.h"

#include <atomic>
#include <functional>
#include <string>

namespace pvt::video {

enum class Codec {
    PngLossless,
    ProRes4444,
    ProRes4444Xq,
    Hevc
};

enum class HardwarePolicy {
    Prefer,
    Require,
    Software
};

enum class HevcQuality {
    MaximumFidelity,
    VeryLightCompression,
    HighQuality
};

struct Capabilities {
    bool available = false;
    bool png_lossless = false;
    bool prores_4444 = false;
    bool prores_4444_xq = false;
    bool hevc = false;
    bool hevc_alpha = false;
    bool prores_4444_hardware = false;
    bool prores_4444_xq_hardware = false;
    bool hevc_hardware = false;
    bool hevc_alpha_hardware = false;
    std::string status;
};

struct Options {
    Codec codec = Codec::ProRes4444;
    HardwarePolicy hardware = HardwarePolicy::Prefer;
    HevcQuality hevc_quality = HevcQuality::MaximumFidelity;
    bool preserve_alpha = false;
    bool include_project_music = true;
    bool overwrite_existing = false;
    std::string music_source_path;
    // Native movies preserve presentation order at the AVFoundation boundary,
    // but independent frames are rendered and converted ahead of it. Zero
    // selects host concurrency and the shared 2 GiB sequence memory budget.
    std::size_t worker_count = 0;
    std::size_t memory_budget_bytes = 0;
    pvt::FrameRenderOptions frame;
};

struct Report {
    bool hardware_required = false;
    bool hardware_available = false;
    bool included_audio = false;
    std::size_t render_workers = 0;
    std::string format_name;
};

using ProgressCallback = std::function<bool(int completed, int total)>;

Capabilities capabilities();

// Writes to a sibling temporary and installs only a complete movie. The
// destination therefore remains untouched on render, encode, or cancellation
// failure. The macOS implementation uses AVFoundation containers and
// VideoToolbox encoder selection; other platforms return an unavailable error.
bool export_project(const pvt::ProjectConfig& project,
                    const std::string& destination,
                    const Options& options,
                    const ProgressCallback& progress,
                    const std::atomic_bool* cancel,
                    Report* report = nullptr,
                    std::string* error = nullptr);

const char* codec_name(Codec codec);

} // namespace pvt::video

#endif
