#include "music_video_export.h"

#include "audio_analysis.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include "windows_file_install.h"
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  if defined(__APPLE__)
#    include <sys/stdio.h>
#  endif
#endif

namespace pvt {
namespace gui {
namespace {

namespace fs = std::filesystem;

constexpr int kMaximumFrameCount = 1000000;
constexpr std::uint64_t kMaximumAudioDurationSeconds = 2U * 60U * 60U;
constexpr qsizetype kMaximumDiagnosticBytes = 64 * 1024;
constexpr int kProbeTimeoutMilliseconds = 5000;
constexpr int kProcessPollMilliseconds = 50;
constexpr char kVideoFilter[] =
    "format=rgba,premultiply=inplace=1,"
    "pad=ceil(iw/2)*2:ceil(ih/2)*2:color=black,"
    "format=yuv420p";

using ProbeCacheEntry = std::pair<QString, FfmpegProbe>;

std::mutex& probe_cache_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<ProbeCacheEntry>& probe_cache() {
    static std::vector<ProbeCacheEntry> cache;
    return cache;
}

QString probe_cache_key(const QString& executable) {
    const QFileInfo information(executable);
    return QStringLiteral("%1\n%2\n%3")
        .arg(information.absoluteFilePath())
        .arg(information.size())
        .arg(information.lastModified().toMSecsSinceEpoch());
}

bool read_probe_cache(const QString& key, FfmpegProbe* result) {
    std::lock_guard<std::mutex> lock(probe_cache_mutex());
    const auto& cache = probe_cache();
    const auto found = std::find_if(
        cache.begin(), cache.end(), [&key](const ProbeCacheEntry& entry) {
            return entry.first == key;
        });
    if (found == cache.end()) {
        return false;
    }
    *result = found->second;
    return true;
}

void write_probe_cache(const QString& key, const FfmpegProbe& result) {
    std::lock_guard<std::mutex> lock(probe_cache_mutex());
    auto& cache = probe_cache();
    const auto found = std::find_if(
        cache.begin(), cache.end(), [&key](const ProbeCacheEntry& entry) {
            return entry.first == key;
        });
    if (found != cache.end()) {
        found->second = result;
        return;
    }
    // UI availability checks normally use one executable. Keep the cache
    // tightly bounded even if a caller probes many configured paths.
    if (cache.size() >= 8U) {
        cache.erase(cache.begin());
    }
    cache.emplace_back(key, result);
}

bool fail(QString* error, QString message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool cancelled(const std::atomic_bool* cancel) {
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

fs::path native_path(const QString& path) {
#if defined(_WIN32)
    return fs::path(path.toStdWString());
#else
    const QByteArray encoded = QFile::encodeName(path);
    return fs::path(encoded.constData());
#endif
}

QString system_error_message(const QString& action,
                             const QString& path,
                             int code) {
    return QStringLiteral("%1 '%2': %3")
        .arg(action, path,
             QString::fromStdString(std::generic_category().message(code)));
}

bool report_progress(const MusicVideoProgressCallback& callback,
                     MusicVideoStage stage,
                     std::int64_t completed,
                     std::int64_t total,
                     QString* error) {
    if (!callback) {
        return true;
    }
    try {
        if (!callback(stage, completed, total)) {
            return fail(error, QStringLiteral("Music video export was cancelled "
                                              "by the progress callback."));
        }
    } catch (const std::exception& exception) {
        return fail(error,
                    QStringLiteral("The music video progress callback failed: %1")
                        .arg(QString::fromUtf8(exception.what())));
    } catch (...) {
        return fail(error,
                    QStringLiteral("The music video progress callback failed "
                                   "with an unknown exception."));
    }
    return true;
}

bool valid_hex_digest(const QString& value) {
    if (value.size() != 64) {
        return false;
    }
    for (const QChar character : value) {
        const ushort code = character.unicode();
        if (!((code >= static_cast<ushort>('0')
               && code <= static_cast<ushort>('9'))
              || (code >= static_cast<ushort>('a')
                  && code <= static_cast<ushort>('f'))
              || (code >= static_cast<ushort>('A')
                  && code <= static_cast<ushort>('F')))) {
            return false;
        }
    }
    return true;
}

bool contains_path_separator(const QString& value) {
    return value.contains(QLatin1Char('/'))
           || value.contains(QLatin1Char('\\'))
           || value.contains(QChar::Null);
}

bool contains_manifest_control(const QString& value) {
    return value.contains(QChar::Null) || value.contains(QLatin1Char('\n'))
           || value.contains(QLatin1Char('\r'));
}

QString format_extension(RenderedImageFormat format) {
    switch (format) {
        case RenderedImageFormat::Png:
            return QStringLiteral(".png");
        case RenderedImageFormat::OpenExr:
            return QStringLiteral(".exr");
    }
    return {};
}

long double audio_duration(std::uint64_t sample_frames,
                           std::uint32_t sample_rate) {
    if (sample_rate == 0U) {
        return 0.0L;
    }
    return static_cast<long double>(sample_frames)
           / static_cast<long double>(sample_rate);
}

int expected_frame_count(std::uint64_t sample_frames,
                         std::uint32_t sample_rate,
                         double fps) {
    if (sample_frames == 0U || sample_rate == 0U || !std::isfinite(fps)
        || fps <= 0.0) {
        return -1;
    }
    const long double frames =
        std::ceil(audio_duration(sample_frames, sample_rate)
                  * static_cast<long double>(fps));
    if (frames < 1.0L
        || frames > static_cast<long double>((std::numeric_limits<int>::max)())) {
        return -1;
    }
    return static_cast<int>(frames);
}

bool validate_request(const MusicVideoExportRequest& request, QString* error) {
    if (request.sequence.directory.isEmpty()) {
        return fail(error, QStringLiteral("The rendered sequence directory is empty."));
    }
    if (contains_manifest_control(request.sequence.directory)) {
        return fail(error,
                    QStringLiteral("The rendered sequence directory contains an "
                                   "unsupported control character."));
    }
    if (request.sequence.filename_prefix.isEmpty()
        || contains_path_separator(request.sequence.filename_prefix)
        || contains_manifest_control(request.sequence.filename_prefix)) {
        return fail(error,
                    QStringLiteral("The rendered sequence prefix must be a literal "
                                   "filename prefix without path separators."));
    }
    if (request.sequence.first_frame_number < 0) {
        return fail(error, QStringLiteral("The first rendered frame number cannot be negative."));
    }
    if (request.sequence.filename_digits < 1
        || request.sequence.filename_digits > 12) {
        return fail(error,
                    QStringLiteral("Rendered frame zero-padding must be between 1 and 12 digits."));
    }
    if (request.sequence.frame_count < 1
        || request.sequence.frame_count > kMaximumFrameCount) {
        return fail(error,
                    QStringLiteral("The rendered sequence must contain between 1 and %1 frames.")
                        .arg(kMaximumFrameCount));
    }
    if (format_extension(request.sequence.format).isEmpty()) {
        return fail(error, QStringLiteral("The rendered sequence format is unsupported."));
    }
    if (!std::isfinite(request.fps) || request.fps < 1.0 || request.fps > 240.0) {
        return fail(error, QStringLiteral("Video FPS must be finite and between 1 and 240."));
    }
    if (request.audio_sample_frame_count == 0U
        || request.audio_sample_rate == 0U) {
        return fail(error,
                    QStringLiteral("The analyzed audio duration is incomplete."));
    }
    const long double duration =
        audio_duration(request.audio_sample_frame_count, request.audio_sample_rate);
    if (!(duration > 0.0L)
        || duration > static_cast<long double>(kMaximumAudioDurationSeconds)) {
        return fail(error,
                    QStringLiteral("The analyzed audio duration must be positive and no "
                                   "longer than two hours."));
    }
    const int needed_frames = expected_frame_count(
        request.audio_sample_frame_count, request.audio_sample_rate, request.fps);
    if (needed_frames < 1 || needed_frames > kMaximumFrameCount) {
        return fail(error,
                    QStringLiteral("The analyzed duration and FPS require an unsupported "
                                   "number of video frames."));
    }
    if (needed_frames != request.sequence.frame_count) {
        return fail(error,
                    QStringLiteral("The rendered sequence contains %1 frames, but the "
                                   "analyzed audio requires exactly %2 at %3 FPS.")
                        .arg(request.sequence.frame_count)
                        .arg(needed_frames)
                        .arg(request.fps, 0, 'g', 12));
    }
    if (request.audio_path.isEmpty()) {
        return fail(error, QStringLiteral("The resolved embedded audio path is empty."));
    }
    if (!valid_hex_digest(request.expected_audio_sha256)) {
        return fail(error,
                    QStringLiteral("The expected audio SHA-256 digest is invalid."));
    }
    if (request.destination_path.isEmpty()) {
        return fail(error, QStringLiteral("The MP4 destination is empty."));
    }
    if (QFileInfo(request.destination_path).suffix().compare(
            QStringLiteral("mp4"), Qt::CaseInsensitive) != 0) {
        return fail(error, QStringLiteral("The music video destination must end in .mp4."));
    }
    return true;
}

bool inspect_regular_source(const QString& path,
                            const QString& description,
                            QString* error) {
    const QFileInfo information(path);
    if (information.isSymbolicLink()) {
        return fail(error,
                    QStringLiteral("%1 is a symbolic link and was rejected: '%2'.")
                        .arg(description, path));
    }
    if (!information.exists()) {
        return fail(error,
                    QStringLiteral("%1 does not exist: '%2'.")
                        .arg(description, path));
    }
    if (!information.isFile()) {
        return fail(error,
                    QStringLiteral("%1 is not a regular file: '%2'.")
                        .arg(description, path));
    }
    return true;
}

bool inspect_destination(const QString& destination,
                         bool overwrite,
                         QString* error) {
    const QFileInfo information(destination);
    const QFileInfo parent(information.absolutePath());
    if (!parent.exists() || !parent.isDir()) {
        return fail(error,
                    QStringLiteral("The MP4 destination directory does not exist or is not "
                                   "a directory: '%1'.")
                        .arg(parent.absoluteFilePath()));
    }
    if (information.isSymbolicLink()) {
        return fail(error,
                    QStringLiteral("The MP4 destination is a symbolic link and was rejected: '%1'.")
                        .arg(destination));
    }
    if (!information.exists()) {
        return true;
    }
    if (!information.isFile()) {
        return fail(error,
                    QStringLiteral("The MP4 destination is not a regular file: '%1'.")
                        .arg(destination));
    }
    if (!overwrite) {
        return fail(error,
                    QStringLiteral("The MP4 destination already exists and overwrite is disabled: '%1'.")
                        .arg(destination));
    }
    return true;
}

QString resolved_destination_path(const QString& requested, QString* error) {
    const QFileInfo information(requested);
    const QFileInfo parent(information.absolutePath());
    const QString canonical_parent = parent.canonicalFilePath();
    if (canonical_parent.isEmpty() || !QFileInfo(canonical_parent).isDir()) {
        fail(error,
             QStringLiteral("The MP4 destination directory could not be resolved: '%1'.")
                 .arg(parent.absoluteFilePath()));
        return {};
    }
    // Resolve only the directory. Canonicalizing the complete destination
    // would follow a final symlink, defeating the explicit target rejection.
    return QDir(canonical_parent).absoluteFilePath(information.fileName());
}

bool write_all(QIODevice& destination,
               const QByteArray& bytes,
               QString* error) {
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const qint64 written = destination.write(
            bytes.constData() + offset,
            static_cast<qint64>(bytes.size() - offset));
        if (written <= 0) {
            return fail(error,
                        QStringLiteral("Could not write the temporary ffmpeg frame list: %1")
                            .arg(destination.errorString()));
        }
        offset += static_cast<qsizetype>(written);
    }
    return true;
}

QByteArray concat_file_line(const QString& native_frame_path) {
    const QString absolute =
        QDir::cleanPath(QFileInfo(native_frame_path).absoluteFilePath());
    // ffconcat scripts are UTF-8. This is also essential on Windows, where
    // QFile::encodeName() would otherwise use a legacy local code page.
    const QByteArray encoded = absolute.toUtf8();
    QByteArray line = QByteArrayLiteral("file '");
    qsizetype offset = 0;
    for (;;) {
        const qsizetype quote = encoded.indexOf('\'', offset);
        if (quote < 0) {
            line.append(encoded.constData() + offset, encoded.size() - offset);
            break;
        }
        line.append(encoded.constData() + offset, quote - offset);
        // ffmpeg's quoting grammar cannot represent a quote inside a quoted
        // region. Close it, backslash-escape the quote, then reopen it.
        line += QByteArrayLiteral("'\\''");
        offset = quote + 1;
    }
    line += QByteArrayLiteral("'\n");
    return line;
}

void append_diagnostic_tail(QByteArray& destination,
                            const QByteArray& bytes,
                            bool& truncated) {
    if (bytes.isEmpty()) {
        return;
    }
    if (bytes.size() >= kMaximumDiagnosticBytes) {
        destination = bytes.right(kMaximumDiagnosticBytes);
        truncated = true;
        return;
    }
    const qsizetype overflow = destination.size() + bytes.size()
                               - kMaximumDiagnosticBytes;
    if (overflow > 0) {
        destination.remove(0, overflow);
        truncated = true;
    }
    destination.append(bytes);
}

QString diagnostic_text(const QByteArray& bytes, bool truncated) {
    QString result = QString::fromUtf8(bytes).trimmed();
    result.replace(QChar::Null, QChar::ReplacementCharacter);
    if (truncated) {
        result.prepend(QStringLiteral("[earlier ffmpeg output omitted]\n"));
    }
    return result;
}

struct ProcessCapture {
    bool started = false;
    bool timed_out = false;
    int exit_code = -1;
    QProcess::ExitStatus exit_status = QProcess::CrashExit;
    QByteArray output;
    bool truncated = false;
    QString process_error;
};

ProcessCapture run_bounded_process(const QString& executable,
                                   const QStringList& arguments,
                                   int timeout_milliseconds) {
    ProcessCapture result;
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(executable, arguments, QIODevice::ReadOnly);

    QElapsedTimer timer;
    timer.start();
    while (process.state() == QProcess::Starting
           && timer.elapsed() < timeout_milliseconds) {
        if (process.waitForStarted(kProcessPollMilliseconds)) {
            result.started = true;
            break;
        }
    }
    result.started = result.started
                     || process.state() == QProcess::Running;
    if (!result.started) {
        result.process_error = process.errorString();
        return result;
    }

    while (process.state() != QProcess::NotRunning
           && timer.elapsed() < timeout_milliseconds) {
        (void)process.waitForReadyRead(kProcessPollMilliseconds);
        append_diagnostic_tail(result.output, process.readAll(), result.truncated);
    }
    if (process.state() != QProcess::NotRunning) {
        result.timed_out = true;
        process.kill();
        (void)process.waitForFinished(1500);
    }
    append_diagnostic_tail(result.output, process.readAll(), result.truncated);
    result.exit_code = process.exitCode();
    result.exit_status = process.exitStatus();
    result.process_error = process.errorString();
    return result;
}

QString configured_ffmpeg_path(const QString& configured, QString* error) {
    QString requested = configured.trimmed();
    const bool explicitly_configured = !requested.isEmpty();
    if (requested.isEmpty()) {
        requested = qEnvironmentVariable("PVT_FFMPEG").trimmed();
    }
    if (requested.isEmpty()) {
        requested = QStringLiteral("ffmpeg");
    }

    QString resolved;
    const QFileInfo requested_info(requested);
    if (requested_info.isAbsolute() || requested.contains(QLatin1Char('/'))
        || requested.contains(QLatin1Char('\\'))) {
        resolved = requested_info.absoluteFilePath();
    } else {
        resolved = QStandardPaths::findExecutable(requested);
    }
    const QFileInfo resolved_info(resolved);
    const QString canonical = resolved_info.canonicalFilePath();
    const QFileInfo executable_info(canonical.isEmpty() ? resolved : canonical);
    if (resolved.isEmpty() || !executable_info.exists()
        || !executable_info.isFile() || !executable_info.isExecutable()) {
        const QString source = explicitly_configured
                                   ? QStringLiteral("configured ffmpeg")
                                   : QStringLiteral("ffmpeg");
        fail(error,
             QStringLiteral("Could not find a regular executable %1. Install ffmpeg, "
                            "put it on PATH, or configure its absolute path.")
                 .arg(source));
        return {};
    }
    return executable_info.absoluteFilePath();
}

bool encoder_list_contains(const QString& listing, const QString& encoder) {
    const QRegularExpression expression(
        QStringLiteral("(?m)^\\s*[VAS]\\S*\\s+%1(?:\\s|$)")
            .arg(QRegularExpression::escape(encoder)));
    return expression.match(listing).hasMatch();
}

QString select_working_encoder(const QString& executable,
                               const QString& listing,
                               const QStringList& candidates,
                               bool video,
                               QString* last_diagnostic) {
    for (const QString& candidate : candidates) {
        if (!encoder_list_contains(listing, candidate)) {
            continue;
        }
        QStringList arguments {
            QStringLiteral("-hide_banner"),
            QStringLiteral("-nostdin"),
            QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-f"), QStringLiteral("lavfi"),
            QStringLiteral("-i")
        };
        if (video) {
            arguments
                << QStringLiteral("color=c=black:s=32x32:r=1:d=0.04")
                << QStringLiteral("-frames:v") << QStringLiteral("1")
                << QStringLiteral("-an")
                << QStringLiteral("-vf")
                << QString::fromLatin1(kVideoFilter)
                << QStringLiteral("-c:v") << candidate
                << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
        } else {
            arguments
                << QStringLiteral("anullsrc=r=44100:cl=stereo:d=0.04")
                << QStringLiteral("-frames:a") << QStringLiteral("1")
                << QStringLiteral("-vn")
                << QStringLiteral("-c:a") << candidate;
        }
        arguments << QStringLiteral("-f") << QStringLiteral("null")
                  << QStringLiteral("-");
        const ProcessCapture probe = run_bounded_process(
            executable, arguments, kProbeTimeoutMilliseconds);
        if (probe.started && !probe.timed_out
            && probe.exit_status == QProcess::NormalExit
            && probe.exit_code == 0) {
            return candidate;
        }
        if (last_diagnostic != nullptr) {
            QString diagnostic = diagnostic_text(probe.output, probe.truncated);
            if (diagnostic.isEmpty()) {
                diagnostic = probe.timed_out
                                 ? QStringLiteral("timed out")
                                 : probe.process_error;
            }
            *last_diagnostic = QStringLiteral("%1: %2")
                                   .arg(candidate, diagnostic);
        }
    }
    return {};
}

void stop_process(QProcess& process) {
    if (process.state() == QProcess::NotRunning) {
        return;
    }
    process.terminate();
    if (!process.waitForFinished(1500)) {
        process.kill();
        (void)process.waitForFinished(1500);
    }
}

bool run_ffmpeg(const QString& executable,
                const QStringList& arguments,
                int frame_count,
                const MusicVideoProgressCallback& progress,
                const std::atomic_bool* cancel,
                QString* error) {
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(executable, arguments, QIODevice::ReadOnly);

    QElapsedTimer start_timer;
    start_timer.start();
    bool started = false;
    while (process.state() == QProcess::Starting
           && start_timer.elapsed() < kProbeTimeoutMilliseconds) {
        if (cancelled(cancel)) {
            stop_process(process);
            return fail(error, QStringLiteral("Music video export was cancelled."));
        }
        if (process.waitForStarted(kProcessPollMilliseconds)) {
            started = true;
            break;
        }
    }
    started = started || process.state() == QProcess::Running;
    if (!started) {
        return fail(error,
                    QStringLiteral("ffmpeg could not start: %1")
                        .arg(process.errorString()));
    }

    QByteArray progress_buffer;
    QByteArray diagnostics;
    bool diagnostics_truncated = false;
    std::int64_t last_frame = -1;

    const auto consume_output = [&] {
        progress_buffer.append(process.readAllStandardOutput());
        for (;;) {
            const qsizetype newline = progress_buffer.indexOf('\n');
            if (newline < 0) {
                break;
            }
            QByteArray line = progress_buffer.left(newline).trimmed();
            progress_buffer.remove(0, newline + 1);
            if (!line.startsWith("frame=")) {
                continue;
            }
            bool ok = false;
            const qlonglong reported = line.mid(6).toLongLong(&ok);
            if (!ok) {
                continue;
            }
            const std::int64_t completed = (std::clamp)(
                static_cast<std::int64_t>(reported), std::int64_t{0},
                static_cast<std::int64_t>(frame_count));
            if (completed > last_frame) {
                last_frame = completed;
                if (!report_progress(progress, MusicVideoStage::Encode,
                                     completed, frame_count, error)) {
                    return false;
                }
            }
        }
        return true;
    };

    if (!report_progress(progress, MusicVideoStage::Encode, 0,
                         frame_count, error)) {
        stop_process(process);
        return false;
    }

    while (process.state() != QProcess::NotRunning) {
        if (cancelled(cancel)) {
            stop_process(process);
            return fail(error, QStringLiteral("Music video export was cancelled."));
        }
        (void)process.waitForReadyRead(kProcessPollMilliseconds);
        if (!consume_output()) {
            stop_process(process);
            return false;
        }
        append_diagnostic_tail(diagnostics, process.readAllStandardError(),
                               diagnostics_truncated);
    }
    if (!consume_output()) {
        return false;
    }
    append_diagnostic_tail(diagnostics, process.readAllStandardError(),
                           diagnostics_truncated);

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        QString diagnostic = diagnostic_text(diagnostics, diagnostics_truncated);
        if (diagnostic.isEmpty()) {
            diagnostic = process.errorString();
        }
        return fail(error,
                    QStringLiteral("ffmpeg failed while encoding the music video "
                                   "(exit code %1): %2")
                        .arg(process.exitCode())
                        .arg(diagnostic));
    }
    return report_progress(progress, MusicVideoStage::Encode,
                           frame_count, frame_count, error);
}

bool sync_regular_file(const QString& path, QString* error) {
#if defined(_WIN32)
    const fs::path native = native_path(path);
    HANDLE handle = CreateFileW(native.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return fail(error,
                    QStringLiteral("Could not open the temporary MP4 for syncing "
                                   "(Windows error %1).")
                        .arg(GetLastError()));
    }
    const BOOL synced = FlushFileBuffers(handle);
    const DWORD sync_error = synced != 0 ? ERROR_SUCCESS : GetLastError();
    (void)CloseHandle(handle);
    if (synced == 0) {
        return fail(error,
                    QStringLiteral("Could not sync the temporary MP4 "
                                   "(Windows error %1).")
                        .arg(sync_error));
    }
#else
    const QByteArray encoded = QFile::encodeName(path);
    int descriptor = -1;
    do {
        descriptor = ::open(encoded.constData(), O_RDONLY);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        return fail(error,
                    system_error_message(QStringLiteral("Could not open the temporary MP4 for syncing"),
                                         path, errno));
    }
    int sync_result = -1;
    do {
        sync_result = ::fsync(descriptor);
    } while (sync_result != 0 && errno == EINTR);
    const int sync_error = sync_result == 0 ? 0 : errno;
    (void)::close(descriptor);
    if (sync_result != 0) {
        return fail(error,
                    system_error_message(QStringLiteral("Could not sync the temporary MP4"),
                                         path, sync_error));
    }
#endif
    return true;
}

#if !defined(_WIN32)
void sync_directory_best_effort(const QString& path) {
    const QByteArray encoded = QFile::encodeName(path);
#  if defined(O_DIRECTORY)
    constexpr int directory_flag = O_DIRECTORY;
#  else
    constexpr int directory_flag = 0;
#  endif
    int descriptor = -1;
    do {
        descriptor = ::open(encoded.constData(), O_RDONLY | directory_flag);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor >= 0) {
        int result = -1;
        do {
            result = ::fsync(descriptor);
        } while (result != 0 && errno == EINTR);
        (void)::close(descriptor);
    }
}
#endif

bool install_temporary(const QString& temporary,
                       const QString& destination,
                       bool overwrite,
                       QString* error) {
    const fs::path temporary_native = native_path(temporary);
    const fs::path destination_native = native_path(destination);
#if defined(_WIN32)
    DWORD code = ERROR_SUCCESS;
    if (!pvt::detail::install_windows_temporary(
            temporary_native, destination_native, overwrite, &code)) {
        return fail(error,
                    QStringLiteral("Could not atomically install the MP4 "
                                   "(Windows error %1).")
                        .arg(code));
    }
#else
    if (overwrite) {
        if (::rename(temporary_native.c_str(), destination_native.c_str()) != 0) {
            return fail(error,
                        system_error_message(QStringLiteral("Could not atomically install the MP4"),
                                             destination, errno));
        }
    } else {
#  if defined(__APPLE__)
        if (::renameatx_np(AT_FDCWD, temporary_native.c_str(),
                           AT_FDCWD, destination_native.c_str(),
                           RENAME_EXCL) != 0) {
            return fail(error,
                        system_error_message(QStringLiteral("Could not install the MP4 without replacing it"),
                                             destination, errno));
        }
#  else
        if (::link(temporary_native.c_str(), destination_native.c_str()) != 0) {
            return fail(error,
                        system_error_message(QStringLiteral("Could not install the MP4 without replacing it"),
                                             destination, errno));
        }
        // Failure to remove this sibling name does not invalidate the fully
        // installed destination. QTemporaryFile will retry cleanup.
        (void)::unlink(temporary_native.c_str());
#  endif
    }
#endif
    return true;
}

} // namespace

QString detail::rendered_frame_path(const RenderedImageSequence& sequence,
                                    int frame_index) {
    if (frame_index < 0 || frame_index >= sequence.frame_count
        || sequence.first_frame_number < 0
        || sequence.filename_digits < 1 || sequence.filename_digits > 12) {
        return {};
    }
    const qint64 number = static_cast<qint64>(sequence.first_frame_number)
                          + static_cast<qint64>(frame_index);
    const QString digits = QString::number(number).rightJustified(
        sequence.filename_digits, QLatin1Char('0'));
    return QDir(sequence.directory)
        .absoluteFilePath(sequence.filename_prefix + digits
                          + format_extension(sequence.format));
}

bool detail::write_concat_manifest(
    QIODevice& destination,
    const RenderedImageSequence& sequence,
    std::uint64_t audio_sample_frame_count,
    std::uint32_t audio_sample_rate,
    double fps,
    QString* error) {
    if (error != nullptr) {
        error->clear();
    }
    if (!destination.isWritable()) {
        return fail(error, QStringLiteral("The temporary ffmpeg frame list is not writable."));
    }
    const int needed_frames = expected_frame_count(
        audio_sample_frame_count, audio_sample_rate, fps);
    if (needed_frames < 1 || needed_frames != sequence.frame_count) {
        return fail(error,
                    QStringLiteral("The frame list does not match the exact analyzed audio duration."));
    }

    if (!write_all(destination, QByteArrayLiteral("ffconcat version 1.0\n"), error)) {
        return false;
    }
    const long double duration =
        audio_duration(audio_sample_frame_count, audio_sample_rate);
    const long double regular_duration = 1.0L / static_cast<long double>(fps);
    const long double last_duration = duration
        - static_cast<long double>(sequence.frame_count - 1) * regular_duration;
    if (!(last_duration > 0.0L)
        || last_duration > regular_duration * (1.0L + 1.0e-9L)) {
        return fail(error,
                    QStringLiteral("The fractional final video frame duration is invalid."));
    }

    for (int index = 0; index < sequence.frame_count; ++index) {
        const QString frame_path = rendered_frame_path(sequence, index);
        if (frame_path.isEmpty()) {
            return fail(error, QStringLiteral("Could not construct a rendered frame path."));
        }
        const long double frame_duration =
            index + 1 == sequence.frame_count ? last_duration : regular_duration;
        QByteArray entry = concat_file_line(frame_path);
        // Give the still-image demuxer a nanosecond time base. The concat
        // duration directives remain authoritative; using the nominal FPS as
        // the inner time base would round away a fractional final frame, while
        // microsecond rounding could accumulate visible drift in a long song.
        entry += QByteArrayLiteral("option framerate 1000000000\n");
        entry += QByteArrayLiteral("duration ");
        entry += QString::number(static_cast<double>(frame_duration), 'f', 12).toLatin1();
        entry += '\n';
        if (!write_all(destination, entry, error)) {
            return false;
        }
    }

    // concat determines the preceding packet's duration from the following
    // timestamp. Repeating only the final filename establishes its fractional
    // duration; -frames:v still constrains the encoded stream to the requested
    // number of distinct timeline frames.
    QByteArray final_entry = concat_file_line(rendered_frame_path(
        sequence, sequence.frame_count - 1));
    final_entry += QByteArrayLiteral("option framerate 1000000000\n");
    return write_all(destination, final_entry, error);
}

QStringList detail::build_ffmpeg_arguments(
    const QString& manifest_path,
    const QString& audio_path,
    const QString& temporary_output_path,
    std::uint64_t audio_sample_frame_count,
    std::uint32_t audio_sample_rate,
    double fps,
    int frame_count,
    const QString& h264_encoder,
    const QString& aac_encoder) {
    const QString nominal_fps = QString::number(fps, 'g', 12);
    const QString duration_text = QString::number(
        static_cast<double>(audio_duration(audio_sample_frame_count,
                                           audio_sample_rate)),
        'f', 9);
    const QString provenance = QStringLiteral(
        "Procedural Visualizer Tool; audio duration %1 s; nominal FPS %2")
                                   .arg(duration_text, nominal_fps);
    const QString video_encoder = h264_encoder.isEmpty()
                                      ? QStringLiteral("libx264")
                                      : h264_encoder;
    const QString audio_encoder = aac_encoder.isEmpty()
                                      ? QStringLiteral("aac")
                                      : aac_encoder;
    QStringList arguments {
        QStringLiteral("-hide_banner"),
        QStringLiteral("-nostdin"),
        QStringLiteral("-loglevel"), QStringLiteral("warning"),
        QStringLiteral("-nostats"),
        QStringLiteral("-progress"), QStringLiteral("pipe:1"),
        QStringLiteral("-f"), QStringLiteral("concat"),
        QStringLiteral("-safe"), QStringLiteral("0"),
        QStringLiteral("-protocol_whitelist"), QStringLiteral("file,crypto,data"),
        QStringLiteral("-i"), QFileInfo(manifest_path).absoluteFilePath(),
        QStringLiteral("-i"), QFileInfo(audio_path).absoluteFilePath(),
        QStringLiteral("-map"), QStringLiteral("0:v:0"),
        QStringLiteral("-map"), QStringLiteral("1:a:0"),
        QStringLiteral("-frames:v"), QString::number(frame_count),
        QStringLiteral("-vf"),
        QString::fromLatin1(kVideoFilter),
        QStringLiteral("-fps_mode"), QStringLiteral("vfr"),
        QStringLiteral("-c:v"), video_encoder
    };
    if (video_encoder == QStringLiteral("libx264")) {
        arguments << QStringLiteral("-preset") << QStringLiteral("medium")
                  << QStringLiteral("-crf") << QStringLiteral("18");
    } else {
        // Hardware and alternative software encoders do not share libx264's
        // CRF/preset options. A generous bounded bitrate remains portable.
        arguments << QStringLiteral("-b:v") << QStringLiteral("12M");
    }
    arguments
        << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p")
        << QStringLiteral("-c:a") << audio_encoder
        << QStringLiteral("-b:a") << QStringLiteral("256k")
        << QStringLiteral("-video_track_timescale") << QStringLiteral("1000000")
        << QStringLiteral("-metadata:s:v:0")
        << QStringLiteral("pvt_nominal_fps=") + nominal_fps
        << QStringLiteral("-map_metadata") << QStringLiteral("-1")
        << QStringLiteral("-metadata")
        << QStringLiteral("comment=") + provenance
        << QStringLiteral("-movflags") << QStringLiteral("+faststart")
        << QStringLiteral("-y")
        << QStringLiteral("-f") << QStringLiteral("mp4")
        << QFileInfo(temporary_output_path).absoluteFilePath();
    return arguments;
}

FfmpegProbe probe_ffmpeg(const QString& configured_executable) {
    FfmpegProbe result;
    QString resolution_error;
    result.executable = configured_ffmpeg_path(configured_executable,
                                               &resolution_error);
    if (result.executable.isEmpty()) {
        result.error = resolution_error;
        return result;
    }
    const QString cache_key = probe_cache_key(result.executable);
    if (read_probe_cache(cache_key, &result)) {
        return result;
    }

    const ProcessCapture version = run_bounded_process(
        result.executable,
        {QStringLiteral("-hide_banner"), QStringLiteral("-version")},
        kProbeTimeoutMilliseconds);
    if (!version.started) {
        result.error = QStringLiteral("ffmpeg could not start: %1")
                           .arg(version.process_error);
        return result;
    }
    if (version.timed_out) {
        result.error = QStringLiteral("ffmpeg did not answer the version probe within five seconds.");
        return result;
    }
    if (version.exit_status != QProcess::NormalExit || version.exit_code != 0) {
        result.error = QStringLiteral("ffmpeg failed its version probe: %1")
                           .arg(diagnostic_text(version.output, version.truncated));
        return result;
    }
    result.version = QString::fromUtf8(version.output)
                         .section(QLatin1Char('\n'), 0, 0)
                         .trimmed();

    const ProcessCapture encoders = run_bounded_process(
        result.executable,
        {QStringLiteral("-hide_banner"), QStringLiteral("-encoders")},
        kProbeTimeoutMilliseconds);
    if (!encoders.started || encoders.timed_out
        || encoders.exit_status != QProcess::NormalExit
        || encoders.exit_code != 0) {
        result.error = QStringLiteral("ffmpeg could not report its available encoders: %1")
                           .arg(diagnostic_text(encoders.output, encoders.truncated));
        return result;
    }
    const QString encoder_text = QString::fromUtf8(encoders.output);
    QString h264_diagnostic;
    QString aac_diagnostic;
    result.h264_encoder = select_working_encoder(
        result.executable, encoder_text,
        {QStringLiteral("libx264"), QStringLiteral("libopenh264"),
         QStringLiteral("h264_videotoolbox"), QStringLiteral("h264_nvenc"),
         QStringLiteral("h264_qsv"), QStringLiteral("h264_amf"),
         QStringLiteral("h264_mf"), QStringLiteral("h264_vaapi")},
        true, &h264_diagnostic);
    result.aac_encoder = select_working_encoder(
        result.executable, encoder_text,
        {QStringLiteral("aac"), QStringLiteral("libfdk_aac"),
         QStringLiteral("aac_at")},
        false, &aac_diagnostic);
    if (result.h264_encoder.isEmpty() || result.aac_encoder.isEmpty()) {
        const QString missing = result.h264_encoder.isEmpty()
                                    ? (result.aac_encoder.isEmpty()
                                           ? QStringLiteral("H.264 and AAC")
                                           : QStringLiteral("H.264"))
                                    : QStringLiteral("AAC");
        const QString diagnostic = result.h264_encoder.isEmpty()
                                       ? h264_diagnostic : aac_diagnostic;
        result.error = QStringLiteral(
            "This ffmpeg build is incompatible: no advertised %1 encoder "
            "completed a runtime check.%2")
                           .arg(missing,
                                diagnostic.isEmpty()
                                    ? QString{}
                                    : QStringLiteral(" Last check: %1")
                                          .arg(diagnostic));
        write_probe_cache(cache_key, result);
        return result;
    }
    result.available = true;
    write_probe_cache(cache_key, result);
    return result;
}

bool export_music_video(const MusicVideoExportRequest& request,
                        const MusicVideoProgressCallback& progress,
                        const std::atomic_bool* cancel,
                        QString* error) {
    if (error != nullptr) {
        error->clear();
    }
    try {
        if (cancelled(cancel)) {
            return fail(error, QStringLiteral("Music video export was cancelled."));
        }
        if (!validate_request(request, error)) {
            return false;
        }

        const QString destination =
            resolved_destination_path(request.destination_path, error);
        if (destination.isEmpty()) {
            return false;
        }
        if (!inspect_destination(destination, request.overwrite_existing, error)) {
            return false;
        }
        if (!inspect_regular_source(request.audio_path,
                                    QStringLiteral("The embedded audio source"),
                                    error)) {
            return false;
        }

        std::string verification_error;
        const bool verified = pvt::audio::verify_music_source(
            request.audio_path.toUtf8().toStdString(),
            request.expected_audio_sha256.toUtf8().toStdString(),
            [&progress, error](std::uint64_t completed, std::uint64_t total) {
                return report_progress(
                    progress, MusicVideoStage::VerifyAudio,
                    static_cast<std::int64_t>((std::min)(
                        completed,
                        static_cast<std::uint64_t>(
                            (std::numeric_limits<std::int64_t>::max)()))),
                    static_cast<std::int64_t>((std::min)(
                        total,
                        static_cast<std::uint64_t>(
                            (std::numeric_limits<std::int64_t>::max)()))),
                    error);
            },
            cancel, &verification_error);
        if (!verified) {
            if (cancelled(cancel)) {
                return fail(error, QStringLiteral("Music video export was cancelled."));
            }
            if (error != nullptr && !error->isEmpty()) {
                return false;
            }
            return fail(error,
                        QString::fromStdString(verification_error.empty()
                            ? "The embedded audio source did not match its saved digest."
                            : verification_error));
        }

        if (!report_progress(progress, MusicVideoStage::Prepare, 0,
                             request.sequence.frame_count, error)) {
            return false;
        }
        const int prepare_stride =
            (std::max)(1, request.sequence.frame_count / 200);
        for (int index = 0; index < request.sequence.frame_count; ++index) {
            if (cancelled(cancel)) {
                return fail(error, QStringLiteral("Music video export was cancelled."));
            }
            const QString frame_path = detail::rendered_frame_path(
                request.sequence, index);
            if (!inspect_regular_source(
                    frame_path,
                    QStringLiteral("Rendered frame %1").arg(index), error)) {
                return false;
            }
            const int completed = index + 1;
            if ((completed == request.sequence.frame_count
                 || completed % prepare_stride == 0)
                && !report_progress(progress, MusicVideoStage::Prepare,
                                    completed, request.sequence.frame_count,
                                    error)) {
                return false;
            }
        }

        const FfmpegProbe ffmpeg = probe_ffmpeg(request.ffmpeg_executable);
        if (!ffmpeg.available) {
            return fail(error, ffmpeg.error);
        }
        if (cancelled(cancel)) {
            return fail(error, QStringLiteral("Music video export was cancelled."));
        }

        const QFileInfo destination_info(destination);
        const QDir parent(destination_info.absolutePath());
        QTemporaryFile manifest(parent.filePath(
            QStringLiteral(".pvt-video-manifest-XXXXXX.ffconcat")));
        manifest.setAutoRemove(true);
        if (!manifest.open()) {
            return fail(error,
                        QStringLiteral("Could not create a temporary ffmpeg frame list: %1")
                            .arg(manifest.errorString()));
        }
        if (!detail::write_concat_manifest(
                manifest, request.sequence,
                request.audio_sample_frame_count,
                request.audio_sample_rate, request.fps, error)) {
            return false;
        }
        if (!manifest.flush()) {
            return fail(error,
                        QStringLiteral("Could not flush the temporary ffmpeg frame list: %1")
                            .arg(manifest.errorString()));
        }
        const QString manifest_path = manifest.fileName();
        manifest.close();
        if (manifest.error() != QFileDevice::NoError) {
            return fail(error,
                        QStringLiteral("Could not close the temporary ffmpeg frame list: %1")
                            .arg(manifest.errorString()));
        }

        QString temporary_template = QStringLiteral(".%1.pvt-video-XXXXXX.mp4")
            .arg(destination_info.completeBaseName());
        QTemporaryFile temporary_output(parent.filePath(temporary_template));
        temporary_output.setAutoRemove(true);
        if (!temporary_output.open()) {
            return fail(error,
                        QStringLiteral("Could not create a same-directory temporary MP4: %1")
                            .arg(temporary_output.errorString()));
        }
        const QString temporary_path = temporary_output.fileName();
        temporary_output.close();

        const QStringList arguments = detail::build_ffmpeg_arguments(
            manifest_path, request.audio_path, temporary_path,
            request.audio_sample_frame_count, request.audio_sample_rate,
            request.fps, request.sequence.frame_count,
            ffmpeg.h264_encoder, ffmpeg.aac_encoder);
        if (!run_ffmpeg(ffmpeg.executable, arguments,
                        request.sequence.frame_count,
                        progress, cancel, error)) {
            return false;
        }

        if (!inspect_regular_source(temporary_path,
                                    QStringLiteral("The encoded temporary MP4"),
                                    error)) {
            return false;
        }
        if (QFileInfo(temporary_path).size() <= 0) {
            return fail(error, QStringLiteral("ffmpeg produced an empty MP4."));
        }
        if (!sync_regular_file(temporary_path, error)) {
            return false;
        }
        if (!QFile::setPermissions(
                temporary_path,
                QFileDevice::ReadOwner | QFileDevice::WriteOwner
                    | QFileDevice::ReadGroup | QFileDevice::ReadOther)) {
            return fail(error,
                        QStringLiteral("Could not set safe permissions on the temporary MP4."));
        }
        if (cancelled(cancel)) {
            return fail(error, QStringLiteral("Music video export was cancelled."));
        }
        if (!report_progress(progress, MusicVideoStage::Install, 0, 1, error)) {
            return false;
        }

        // Repeat target inspection immediately before the atomic operation.
        if (!inspect_destination(destination, request.overwrite_existing, error)) {
            return false;
        }
        if (!install_temporary(temporary_path, destination,
                               request.overwrite_existing, error)) {
            return false;
        }
        if (!QFileInfo::exists(temporary_path)) {
            temporary_output.setAutoRemove(false);
        }
#if !defined(_WIN32)
        sync_directory_best_effort(destination_info.absolutePath());
#endif
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, QStringLiteral("Music video export ran out of memory."));
    } catch (const std::exception& exception) {
        return fail(error,
                    QStringLiteral("Music video export failed: %1")
                        .arg(QString::fromUtf8(exception.what())));
    } catch (...) {
        return fail(error,
                    QStringLiteral("Music video export failed with an unknown exception."));
    }
}

} // namespace gui
} // namespace pvt
