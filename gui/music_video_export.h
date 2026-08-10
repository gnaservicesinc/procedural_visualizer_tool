#ifndef PVT_MUSIC_VIDEO_EXPORT_H
#define PVT_MUSIC_VIDEO_EXPORT_H

#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <functional>

class QIODevice;

namespace pvt {
namespace gui {

enum class RenderedImageFormat {
    Png,
    OpenExr
};

// Describes the exact names produced by the renderer without relying on
// ffmpeg's image2 pattern expansion. Literal percent signs and Unicode in the
// directory or prefix are therefore safe.
struct RenderedImageSequence {
    QString directory;
    QString filename_prefix;
    int first_frame_number = 0;
    int filename_digits = 4;
    int frame_count = 0;
    RenderedImageFormat format = RenderedImageFormat::Png;
};

enum class MusicVideoStage {
    VerifyAudio,
    Prepare,
    Encode,
    Install
};

// Returning false requests cancellation. Callbacks run synchronously on the
// export worker's thread and must not update Qt widgets directly.
using MusicVideoProgressCallback =
    std::function<bool(MusicVideoStage stage,
                       std::int64_t completed,
                       std::int64_t total)>;

struct FfmpegProbe {
    bool available = false;
    QString executable;
    QString version;
    QString h264_encoder;
    QString aac_encoder;
    QString error;
};

struct MusicVideoExportRequest {
    RenderedImageSequence sequence;
    double fps = 60.0;

    // The decoded source duration is represented exactly as sample frames /
    // sample rate. This avoids rounding a fractional final video frame from a
    // display-only duration string.
    std::uint64_t audio_sample_frame_count = 0U;
    std::uint32_t audio_sample_rate = 0U;
    QString audio_path;
    QString expected_audio_sha256;

    QString destination_path;
    bool overwrite_existing = false;

    // Empty selects PVT_FFMPEG when set, then an ffmpeg on PATH. A configured
    // value may be either an absolute path or an executable name on PATH.
    QString ffmpeg_executable;
};

// Checks that ffmpeg starts and can actually encode a tiny H.264 frame and AAC
// packet (advertising an encoder is not sufficient). The returned executable
// is absolute and the selected encoder names are recorded in the result.
FfmpegProbe probe_ffmpeg(const QString& configured_executable = {});

// Verifies the linked audio digest, consumes exactly sequence.frame_count
// rendered frames, and writes an H.264/AAC MP4 whose timeline duration is
// derived from the source's exact sample count. The destination is installed
// atomically from a same-directory temporary. On failure or cancellation, no
// partial final file is left behind.
bool export_music_video(const MusicVideoExportRequest& request,
                        const MusicVideoProgressCallback& progress = {},
                        const std::atomic_bool* cancel = nullptr,
                        QString* error = nullptr);

namespace detail {

// Exposed only to keep command construction independently unit-testable.
QString rendered_frame_path(const RenderedImageSequence& sequence,
                            int frame_index);
bool write_concat_manifest(QIODevice& destination,
                           const RenderedImageSequence& sequence,
                           std::uint64_t audio_sample_frame_count,
                           std::uint32_t audio_sample_rate,
                           double fps,
                           QString* error = nullptr);
QStringList build_ffmpeg_arguments(const QString& manifest_path,
                                   const QString& audio_path,
                                   const QString& temporary_output_path,
                                   std::uint64_t audio_sample_frame_count,
                                   std::uint32_t audio_sample_rate,
                                   double fps,
                                   int frame_count,
                                   const QString& h264_encoder = {},
                                   const QString& aac_encoder = {});

} // namespace detail
} // namespace gui
} // namespace pvt

#endif
