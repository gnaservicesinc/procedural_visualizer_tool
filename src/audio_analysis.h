#ifndef PVT_AUDIO_ANALYSIS_H
#define PVT_AUDIO_ANALYSIS_H

#include "procedural_visualizer_tool.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace pvt {
namespace audio {

// Progress is reported as monotonically increasing logical work units. The
// second argument is fixed for the duration of a call. Returning false requests
// cancellation, with the same transactional behavior as the atomic token.
using AudioProgressCallback =
    std::function<bool(std::uint64_t completed, std::uint64_t total)>;

// Decodes a representation-bounded WAV, FLAC, or MP3 source and creates the cached analysis
// consumed by Music clocks. The destination is changed only after the complete
// file has been decoded, analyzed, and hashed successfully.
bool analyze_music_file(const std::string& path,
                        MusicAnalysis& destination,
                        const AudioProgressCallback& progress = {},
                        const std::atomic_bool* cancel = nullptr,
                        std::string* error = nullptr);

// Re-hashes a linked source without decoding it. expected_sha256 must be 64
// lowercase or uppercase hexadecimal characters. A mismatch is a normal false
// result and includes a concise diagnostic when error is supplied.
bool verify_music_source(const std::string& path,
                         const std::string& expected_sha256,
                         const AudioProgressCallback& progress = {},
                         const std::atomic_bool* cancel = nullptr,
                         std::string* error = nullptr);

} // namespace audio
} // namespace pvt

#endif
