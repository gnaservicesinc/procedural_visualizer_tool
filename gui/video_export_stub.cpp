#include "video_export.h"

namespace pvt::video {

Capabilities capabilities() {
    Capabilities result;
    result.status = "Native video export is currently available on macOS.";
    return result;
}

bool export_project(const pvt::ProjectConfig&, const std::string&,
                    const Options&, const ProgressCallback&,
                    const std::atomic_bool*, Report*, std::string* error) {
    if (error != nullptr) {
        *error = "Native video export is currently available on macOS.";
    }
    return false;
}

const char* codec_name(Codec codec) {
    switch (codec) {
        case Codec::PngLossless: return "Lossless PNG in QuickTime";
        case Codec::ProRes4444: return "Apple ProRes 4444";
        case Codec::ProRes4444Xq: return "Apple ProRes 4444 XQ";
        case Codec::Hevc: return "HEVC";
    }
    return "Unknown";
}

} // namespace pvt::video
