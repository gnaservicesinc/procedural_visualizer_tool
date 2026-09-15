#include "photo_import.h"
#include <QColorSpace>
#import <AVFoundation/AVFoundation.h>
#import <CoreImage/CoreImage.h>
#import <ImageIO/ImageIO.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr size_t maximumPixels = 64U * 1024U * 1024U;
QImage colorImage(CGImageSourceRef source, size_t index, CIContext* context) {
    NSDictionary* properties = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source, index, nullptr));
    const size_t width = [properties[(__bridge NSString*)kCGImagePropertyPixelWidth] unsignedLongLongValue];
    const size_t height = [properties[(__bridge NSString*)kCGImagePropertyPixelHeight] unsignedLongLongValue];
    if (!width || !height || width > maximumPixels / height) return {};
    CGImageRef cg = CGImageSourceCreateImageAtIndex(source, index, nullptr);
    if (!cg) return {};
    CIImage* image = [CIImage imageWithCGImage:cg]; CGImageRelease(cg);
    int orientation = [properties[(__bridge NSString*)kCGImagePropertyOrientation] intValue];
    image = [image imageByApplyingOrientation:orientation ? orientation : 1];
    QImage result(static_cast<int>(image.extent.size.width), static_cast<int>(image.extent.size.height), QImage::Format_RGBA64);
    if (result.isNull()) return {};
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    [context render:image toBitmap:result.bits() rowBytes:result.bytesPerLine() bounds:image.extent format:kCIFormatRGBA16 colorSpace:space];
    CGColorSpaceRelease(space);
    result.setColorSpace(QColorSpace::SRgb);
    return result;
}
QImage dataImage(CVPixelBufferRef buffer, bool depth) {
    if (!buffer) return {};
    const size_t width = CVPixelBufferGetWidth(buffer), height = CVPixelBufferGetHeight(buffer);
    if (!width || !height || width > maximumPixels / height) return {};
    if (CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return {};
    QImage result(static_cast<int>(width), static_cast<int>(height), QImage::Format_Grayscale16);
    if (result.isNull()) { CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly); return {}; }
    const auto* base = static_cast<const unsigned char*>(CVPixelBufferGetBaseAddress(buffer));
    const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
    const OSType format = CVPixelBufferGetPixelFormatType(buffer);
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    if (depth) for (size_t y = 0; y < height; ++y) {
        const auto* values = reinterpret_cast<const float*>(base + y * stride);
        for (size_t x = 0; x < width; ++x) if (std::isfinite(values[x]) && values[x] > 0) {
            lo = std::min(lo, double(values[x])); hi = std::max(hi, double(values[x]));
        }
    }
    for (size_t y = 0; y < height; ++y) {
        auto* pixels = reinterpret_cast<quint16*>(result.scanLine(static_cast<int>(y)));
        for (size_t x = 0; x < width; ++x) {
            double value = 0;
            if (depth) {
                const double raw = reinterpret_cast<const float*>(base + y * stride)[x];
                value = std::isfinite(raw) && raw > 0 && hi > lo ? (raw - lo) / (hi - lo) : .5;
            } else if (format == kCVPixelFormatType_OneComponent8) value = base[y * stride + x] / 255.;
            else if (format == kCVPixelFormatType_OneComponent32Float) value = reinterpret_cast<const float*>(base + y * stride)[x];
            else { CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly); return {}; }
            pixels[x] = static_cast<quint16>(std::clamp(std::isfinite(value) ? value : 0., 0., 1.) * 65535.);
        }
    }
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    return result;
}
}

bool extractPhoto(const QString& path, ImportedPhoto& photo, QString& error) {
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.toUtf8().constData()]];
        CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
        if (!source) { error = QObject::tr("The photo could not be decoded."); return false; }
        CIContext* context = [CIContext contextWithOptions:@{kCIContextUseSoftwareRenderer:@YES}];
        const size_t primary = CGImageSourceGetPrimaryImageIndex(source);
        photo.primary = colorImage(source, primary, context);
        if (photo.primary.isNull()) { CFRelease(source); error = QObject::tr("The photo is unreadable or exceeds the 64 megapixel import limit."); return false; }
        NSDictionary* properties = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source, primary, nullptr));
        auto orientation = static_cast<CGImagePropertyOrientation>([properties[(__bridge NSString*)kCGImagePropertyOrientation] intValue] ?: 1);
        NSDictionary* container = CFBridgingRelease(CGImageSourceCopyProperties(source, nullptr));
        NSArray* groups = container[(__bridge NSString*)kCGImagePropertyGroups];
        for (NSDictionary* group in groups) {
            NSNumber* left = group[(__bridge NSString*)kCGImagePropertyGroupImageIndexLeft];
            NSNumber* right = group[(__bridge NSString*)kCGImagePropertyGroupImageIndexRight];
            if (left && right && left.unsignedIntegerValue < CGImageSourceGetCount(source) && right.unsignedIntegerValue < CGImageSourceGetCount(source)) {
                auto l = colorImage(source, left.unsignedIntegerValue, context);
                auto r = colorImage(source, right.unsignedIntegerValue, context);
                if (!l.isNull() && !r.isNull()) {
                    photo.images.push_back({"color", "Left view", l}); photo.images.push_back({"color", "Right view", r});
                }
                break;
            }
        }
        for (CFStringRef type : {kCGImageAuxiliaryDataTypeDisparity, kCGImageAuxiliaryDataTypeDepth}) {
            NSDictionary* info = CFBridgingRelease(CGImageSourceCopyAuxiliaryDataInfoAtIndex(source, primary, type));
            if (!info) continue;
            NSError* failure = nil;
            AVDepthData* depth = [AVDepthData depthDataFromDictionaryRepresentation:info error:&failure];
            depth = [[depth depthDataByConvertingToDepthDataType:kCVPixelFormatType_DisparityFloat32] depthDataByApplyingExifOrientation:orientation];
            auto image = dataImage(depth.depthDataMap, true);
            if (!image.isNull()) { photo.images.push_back({"depth", QObject::tr("Camera depth (normalized near = white)"), image}); break; }
            photo.notes += QObject::tr(" Camera depth was present but could not be decoded.");
        }
        const std::pair<CFStringRef, const char*> types[] = {
            {kCGImageAuxiliaryDataTypePortraitEffectsMatte, "Subject"},
            {kCGImageAuxiliaryDataTypeSemanticSegmentationHairMatte, "Hair"},
            {kCGImageAuxiliaryDataTypeSemanticSegmentationSkinMatte, "Skin"},
            {kCGImageAuxiliaryDataTypeSemanticSegmentationTeethMatte, "Teeth"},
            {kCGImageAuxiliaryDataTypeSemanticSegmentationGlassesMatte, "Glasses"},
            {kCGImageAuxiliaryDataTypeSemanticSegmentationSkyMatte, "Sky"}};
        size_t retained = static_cast<size_t>(photo.primary.sizeInBytes());
        for (const auto& image : photo.images) retained += static_cast<size_t>(image.image.sizeInBytes());
        for (auto [type, name] : types) {
            if (retained > 512U * 1024U * 1024U) {
                photo.notes += QObject::tr(" Additional camera masks were omitted to limit memory use.");
                break;
            }
            NSDictionary* info = CFBridgingRelease(CGImageSourceCopyAuxiliaryDataInfoAtIndex(source, primary, type));
            if (!info) continue;
            NSError* failure = nil;
            QImage image;
            if (type == kCGImageAuxiliaryDataTypePortraitEffectsMatte) {
                AVPortraitEffectsMatte* matte = [AVPortraitEffectsMatte portraitEffectsMatteFromDictionaryRepresentation:info error:&failure];
                matte = [matte portraitEffectsMatteByApplyingExifOrientation:orientation];
                image = dataImage(matte.mattingImage, false);
            } else {
                AVSemanticSegmentationMatte* matte = [AVSemanticSegmentationMatte semanticSegmentationMatteFromImageSourceAuxiliaryDataType:type dictionaryRepresentation:info error:&failure];
                matte = [matte semanticSegmentationMatteByApplyingExifOrientation:orientation];
                image = dataImage(matte.mattingImage, false);
            }
            if (!image.isNull()) { retained += static_cast<size_t>(image.sizeInBytes()); photo.images.push_back({"mask", QString::fromUtf8(name), image}); }
            else photo.notes += QObject::tr(" A camera mask was present but could not be decoded.");
        }
        CFRelease(source);
        return true;
    }
}
