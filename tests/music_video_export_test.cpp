#include "music_video_export.h"

#include <QBuffer>
#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << __FILE__ << ':' << __LINE__                            \
                      << ": check failed: " #condition << '\n';                \
            ++failures;                                                        \
        }                                                                       \
    } while (false)

bool write_test_wav(const QString& path,
                    std::uint32_t sample_rate,
                    std::uint32_t sample_frames) {
    constexpr std::uint16_t channels = 2U;
    constexpr std::uint16_t bits_per_sample = 16U;
    const std::uint32_t bytes_per_sample = bits_per_sample / 8U;
    const std::uint32_t data_bytes = sample_frames
                                     * static_cast<std::uint32_t>(channels)
                                     * bytes_per_sample;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    if (stream.writeRawData("RIFF", 4) != 4) return false;
    stream << static_cast<std::uint32_t>(36U + data_bytes);
    if (stream.writeRawData("WAVEfmt ", 8) != 8) return false;
    stream << static_cast<std::uint32_t>(16U);
    stream << static_cast<std::uint16_t>(1U);
    stream << channels;
    stream << sample_rate;
    stream << static_cast<std::uint32_t>(sample_rate
                                         * static_cast<std::uint32_t>(channels)
                                         * bytes_per_sample);
    stream << static_cast<std::uint16_t>(channels * bytes_per_sample);
    stream << bits_per_sample;
    if (stream.writeRawData("data", 4) != 4) return false;
    stream << data_bytes;

    constexpr double pi = 3.14159265358979323846;
    for (std::uint32_t frame = 0U; frame < sample_frames; ++frame) {
        const double time = static_cast<double>(frame)
                            / static_cast<double>(sample_rate);
        double sample = 0.20 * std::sin(2.0 * pi * 440.0 * time);
        if (frame % (sample_rate / 4U) < 100U) {
            sample += 0.65 * std::exp(
                -static_cast<double>(frame % (sample_rate / 4U)) / 18.0);
        }
        const auto encoded = static_cast<std::int16_t>(
            std::lround((std::clamp)(sample, -1.0, 1.0) * 32767.0));
        stream << encoded << encoded;
    }
    file.close();
    return file.error() == QFileDevice::NoError;
}

QString sha256_file(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        hash.addData(file.read(64 * 1024));
    }
    return QString::fromLatin1(hash.result().toHex());
}

void unit_checks() {
    pvt::gui::RenderedImageSequence sequence;
    sequence.directory = QStringLiteral("/tmp/percent % and quote ' and snow \u96ea");
    sequence.filename_prefix = QStringLiteral("pulse%'");
    sequence.first_frame_number = 7;
    sequence.filename_digits = 4;
    sequence.frame_count = 3;

    const QString frame = pvt::gui::detail::rendered_frame_path(sequence, 1);
    CHECK(frame.endsWith(QStringLiteral("pulse%'0008.png")));

    QBuffer manifest;
    CHECK(manifest.open(QIODevice::WriteOnly));
    QString error;
    CHECK(pvt::gui::detail::write_concat_manifest(
        manifest, sequence, 101U, 100U, 2.0, &error));
    CHECK(error.isEmpty());
    const QByteArray bytes = manifest.data();
    CHECK(bytes.startsWith("ffconcat version 1.0\n"));
    CHECK(bytes.contains("pulse%"));
    CHECK(bytes.contains("'\\''"));
    CHECK(bytes.contains(QStringLiteral("\u96ea").toUtf8()));
    CHECK(bytes.count("file '") == 4);
    CHECK(bytes.contains("duration 0.010000000000\n"));

    const QStringList arguments = pvt::gui::detail::build_ffmpeg_arguments(
        QStringLiteral("/tmp/list %.ffconcat"),
        QStringLiteral("/tmp/audio %.wav"),
        QStringLiteral("/tmp/output %.mp4"),
        101U, 100U, 2.0, 3);
    const qsizetype frame_option = arguments.indexOf(QStringLiteral("-frames:v"));
    CHECK(frame_option >= 0);
    CHECK(frame_option + 1 < arguments.size());
    if (frame_option >= 0 && frame_option + 1 < arguments.size()) {
        CHECK(arguments.at(frame_option + 1) == QStringLiteral("3"));
    }
    CHECK(arguments.contains(QStringLiteral("/tmp/audio %.wav")));
    CHECK(arguments.contains(QStringLiteral("-fps_mode")));
    CHECK(arguments.contains(QStringLiteral("vfr")));
    CHECK(arguments.join(QLatin1Char(' ')).contains(
        QStringLiteral("pad=ceil(iw/2)*2:ceil(ih/2)*2")));
    CHECK(!arguments.join(QLatin1Char(' ')).contains(QStringLiteral("%04d")));

    const auto missing = pvt::gui::probe_ffmpeg(
        QStringLiteral("/definitely/not/a/real/pvt-ffmpeg"));
    CHECK(!missing.available);
    CHECK(!missing.error.isEmpty());

    pvt::gui::MusicVideoExportRequest cancelled_request;
    std::atomic_bool cancelled {true};
    CHECK(!pvt::gui::export_music_video(cancelled_request, {},
                                        &cancelled, &error));
    CHECK(error.contains(QStringLiteral("cancel"), Qt::CaseInsensitive));
}

void integration_check() {
    const pvt::gui::FfmpegProbe probe = pvt::gui::probe_ffmpeg();
    if (!probe.available) {
        std::cout << "SKIP ffmpeg integration: "
                  << probe.error.toStdString() << '\n';
        return;
    }

    QTemporaryDir directory(
        QDir::tempPath() + QStringLiteral("/pvt video % quote ' snow \u96ea-XXXXXX"));
    CHECK(directory.isValid());
    if (!directory.isValid()) {
        return;
    }

    constexpr std::uint32_t sample_rate = 44100U;
    // 1.000998 seconds at 24 FPS needs 25 frames. An untrimmed CFR stream
    // would last 1.041667 seconds, well outside the duration tolerance below.
    constexpr std::uint32_t sample_frames = 44144U;
    constexpr double fps = 24.0;
    constexpr int frame_count = 25;

    const QString audio_path = directory.filePath(QStringLiteral("song % \u96ea.wav"));
    CHECK(write_test_wav(audio_path, sample_rate, sample_frames));
    const QString digest = sha256_file(audio_path);
    CHECK(digest.size() == 64);

    pvt::gui::RenderedImageSequence sequence;
    sequence.directory = directory.path();
    sequence.filename_prefix = QStringLiteral("visual % pulse ' \u96ea_");
    sequence.first_frame_number = 3;
    sequence.filename_digits = 4;
    sequence.frame_count = frame_count;

    for (int index = 0; index < frame_count; ++index) {
        QImage image(33, 33, QImage::Format_RGBA8888);
        // Straight-alpha white with alpha zero must become black in H.264,
        // rather than leaking hidden RGB when the alpha plane is discarded.
        image.fill(QColor(255, 255, 255, 0));
        CHECK(image.save(pvt::gui::detail::rendered_frame_path(sequence, index),
                         "PNG"));
    }

    const QString destination = directory.filePath(
        QStringLiteral("finished music % \u96ea.mp4"));
    pvt::gui::MusicVideoExportRequest request;
    request.sequence = sequence;
    request.fps = fps;
    request.audio_sample_frame_count = sample_frames;
    request.audio_sample_rate = sample_rate;
    request.audio_path = audio_path;
    request.expected_audio_sha256 = digest;
    request.destination_path = destination;
    request.ffmpeg_executable = probe.executable;
    request.overwrite_existing = true;
    {
        QFile previous(destination);
        CHECK(previous.open(QIODevice::WriteOnly | QIODevice::Truncate));
        CHECK(previous.write("previous video placeholder") > 0);
    }

    std::array<std::int64_t, 4U> last_progress {{-1, -1, -1, -1}};
    QString error;
    const bool exported = pvt::gui::export_music_video(
        request,
        [&last_progress](pvt::gui::MusicVideoStage stage,
                         std::int64_t completed,
                         std::int64_t total) {
            const auto index = static_cast<std::size_t>(stage);
            CHECK(index < last_progress.size());
            if (index < last_progress.size()) {
                CHECK(completed >= last_progress[index]);
                CHECK(completed >= 0);
                CHECK(total > 0);
                CHECK(completed <= total);
                last_progress[index] = completed;
            }
            return true;
        },
        nullptr, &error);
    CHECK(exported);
    if (!exported) {
        std::cerr << "ffmpeg integration export failed: "
                  << error.toStdString() << '\n';
        return;
    }
    CHECK(QFileInfo(destination).isFile());
    CHECK(QFileInfo(destination).size() > 0);

    request.overwrite_existing = false;
    CHECK(!pvt::gui::export_music_video(request, {}, nullptr, &error));
    CHECK(error.contains(QStringLiteral("overwrite"), Qt::CaseInsensitive));

#if !defined(_WIN32)
    const QString victim_path = directory.filePath(QStringLiteral("victim.bin"));
    const QString symlink_path = directory.filePath(QStringLiteral("video-link.mp4"));
    {
        QFile victim(victim_path);
        CHECK(victim.open(QIODevice::WriteOnly | QIODevice::Truncate));
        CHECK(victim.write("must remain unchanged") > 0);
    }
    CHECK(QFile::link(victim_path, symlink_path));
    request.destination_path = symlink_path;
    request.overwrite_existing = true;
    CHECK(!pvt::gui::export_music_video(request, {}, nullptr, &error));
    CHECK(error.contains(QStringLiteral("symbolic link"), Qt::CaseInsensitive));
    QFile victim(victim_path);
    CHECK(victim.open(QIODevice::ReadOnly));
    CHECK(victim.readAll() == QByteArrayLiteral("must remain unchanged"));
#endif

    QString ffprobe = QDir(QFileInfo(probe.executable).absolutePath())
                          .filePath(QStringLiteral("ffprobe"));
#if defined(_WIN32)
    if (!QFileInfo::exists(ffprobe)) {
        ffprobe += QStringLiteral(".exe");
    }
#endif
    if (!QFileInfo(ffprobe).isExecutable()) {
        ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    }
    if (ffprobe.isEmpty()) {
        std::cout << "SKIP ffprobe duration assertion (video export succeeded).\n";
        return;
    }

    QProcess inspect;
    inspect.setProcessChannelMode(QProcess::MergedChannels);
    inspect.start(ffprobe,
                  {QStringLiteral("-v"), QStringLiteral("error"),
                   QStringLiteral("-count_frames"),
                   QStringLiteral("-select_streams"), QStringLiteral("v:0"),
                   QStringLiteral("-show_entries"),
                   QStringLiteral("stream=width,height,nb_read_frames:format=duration"),
                   QStringLiteral("-of"),
                   QStringLiteral("default=noprint_wrappers=1"),
                   destination},
                  QIODevice::ReadOnly);
    CHECK(inspect.waitForFinished(10000));
    CHECK(inspect.exitStatus() == QProcess::NormalExit);
    CHECK(inspect.exitCode() == 0);
    const QString metadata = QString::fromUtf8(inspect.readAll());
    const QRegularExpression duration_expression(
        QStringLiteral("(?:^|\\n)duration=([0-9]+(?:\\.[0-9]+)?)"));
    const auto duration_match = duration_expression.match(metadata);
    CHECK(duration_match.hasMatch());
    if (duration_match.hasMatch()) {
        const double measured = duration_match.captured(1).toDouble();
        const double expected = static_cast<double>(sample_frames)
                                / static_cast<double>(sample_rate);
        if (std::abs(measured - expected) > 0.002) {
            std::cerr << "measured duration=" << measured
                      << " expected=" << expected << '\n';
        }
        CHECK(std::abs(measured - expected) <= 0.002);
    }
    CHECK(metadata.contains(QStringLiteral("width=34")));
    CHECK(metadata.contains(QStringLiteral("height=34")));
    if (!metadata.contains(QStringLiteral("nb_read_frames=25"))) {
        std::cerr << "ffprobe metadata:\n" << metadata.toStdString() << '\n';
    }
    CHECK(metadata.contains(QStringLiteral("nb_read_frames=25")));

    QProcess decode;
    decode.start(probe.executable,
                 {QStringLiteral("-hide_banner"),
                  QStringLiteral("-loglevel"), QStringLiteral("error"),
                  QStringLiteral("-i"), destination,
                  QStringLiteral("-frames:v"), QStringLiteral("1"),
                  QStringLiteral("-pix_fmt"), QStringLiteral("rgb24"),
                  QStringLiteral("-f"), QStringLiteral("rawvideo"),
                  QStringLiteral("pipe:1")},
                 QIODevice::ReadOnly);
    CHECK(decode.waitForFinished(10000));
    CHECK(decode.exitStatus() == QProcess::NormalExit);
    CHECK(decode.exitCode() == 0);
    const QByteArray first_frame = decode.readAllStandardOutput();
    CHECK(first_frame.size() == 34 * 34 * 3);
    bool black = true;
    for (const char value : first_frame) {
        if (static_cast<unsigned char>(value) > 16U) {
            black = false;
            break;
        }
    }
    CHECK(black);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    unit_checks();
    integration_check();
    if (failures != 0) {
        std::cerr << failures << " music video export check(s) failed\n";
        return 1;
    }
    std::cout << "Music video export checks passed\n";
    return 0;
}
