#include "photo_import.h"
#include <QImageReader>
#include <algorithm>
#include <cmath>
#include <limits>
#ifndef Q_OS_MACOS
bool extractPhoto(const QString& path, ImportedPhoto& photo, QString& error) {
    QImageReader reader(path); reader.setAutoTransform(true);
    photo.primary = reader.read();
    if (photo.primary.isNull()) { error = reader.errorString(); return false; }
    photo.notes = QObject::tr("This image decoder provides the primary image only. Embedded camera depth and semantic masks require the macOS photo importer. Extracted project assets remain portable.");
    return true;
}
#endif

void derivePhotoImages(ImportedPhoto& photo) {
    qsizetype bytes = photo.primary.sizeInBytes();
    for (const auto& image : photo.images) bytes += image.image.sizeInBytes();
    bool omitted_cutouts = false;
    const auto extracted = photo.images;
    for (const auto& item : extracted) {
        if (item.kind != "mask") continue;
        const qsizetype cutout_bytes = qsizetype(photo.primary.width()) * photo.primary.height() * 8;
        if (bytes + cutout_bytes > 512LL * 1024 * 1024) { omitted_cutouts = true; continue; }
        bytes += cutout_bytes;
        const QImage mask = item.image.scaled(photo.primary.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_Grayscale16);
        QImage cutout = photo.primary.convertToFormat(QImage::Format_RGBA64);
        for (int y = 0; y < cutout.height(); ++y) {
            auto* pixels = reinterpret_cast<QRgba64*>(cutout.scanLine(y));
            const auto* weights = reinterpret_cast<const quint16*>(mask.constScanLine(y));
            for (int x = 0; x < cutout.width(); ++x)
                pixels[x].setAlpha(static_cast<quint16>((quint32(pixels[x].alpha()) * weights[x] + 32767U) / 65535U));
        }
        photo.images.push_back({"color", item.name + QObject::tr(" cutout"), cutout});
    }
    if (omitted_cutouts) photo.notes += QObject::tr(" Some full-size cutouts were omitted to limit memory use; their masks remain available.");
    // Only a metadata-identified stereo pair may be used for disparity. Small
    // block matching is an explicit draft, never a replacement for camera depth.
    QImage left, right;
    bool has_depth = false;
    for (const auto& item : photo.images) {
        has_depth |= item.kind == "depth";
        if (item.name == "Left view") left = item.image;
        if (item.name == "Right view") right = item.image;
    }
    if (has_depth || left.isNull() || right.isNull() || left.size() != right.size()) return;
    photo.primary = left;
    left = left.scaled(384, 288, Qt::KeepAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_Grayscale8);
    right = right.scaled(left.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_Grayscale8);
    QImage depth(left.size(), QImage::Format_Grayscale16); depth.fill(32768);
    QImage confidence(left.size(), QImage::Format_Grayscale16); confidence.fill(0);
    constexpr int radius = 2, search = 32;
    for (int y = radius; y < left.height() - radius; ++y) {
        auto* output = reinterpret_cast<quint16*>(depth.scanLine(y));
        auto* quality = reinterpret_cast<quint16*>(confidence.scanLine(y));
        for (int x = radius; x < left.width() - radius; ++x) {
            int best = std::numeric_limits<int>::max(), second = best, disparity = 0;
            for (int d = -search; d <= search; ++d) {
                if (x - d < radius || x - d >= right.width() - radius) continue;
                int cost = 0;
                for (int yy = -radius; yy <= radius; ++yy) {
                    const auto* a = left.constScanLine(y + yy);
                    const auto* b = right.constScanLine(y + yy);
                    for (int xx = -radius; xx <= radius; ++xx) cost += std::abs(int(a[x + xx]) - int(b[x + xx - d]));
                }
                if (cost < best) { second = best; best = cost; disparity = d; }
                else second = std::min(second, cost);
            }
            const double q = second == std::numeric_limits<int>::max() ? 0 : double(second - best) / std::max(1, second);
            quality[x] = static_cast<quint16>(std::clamp(q * 4, 0., 1.) * 65535);
            output[x] = q < .03 ? 32768 : static_cast<quint16>((disparity + search) * 65535 / (2 * search));
        }
    }
    photo.images.push_back({"depth", QObject::tr("Estimated stereo depth (draft, primary-view alignment)"), depth});
    photo.images.push_back({"mask", QObject::tr("Stereo match confidence"), confidence});
    photo.notes += QObject::tr(" No embedded depth was present. Stereo depth is a low-resolution estimate; inspect occlusions, textureless areas, and alignment before enabling it.");
}
