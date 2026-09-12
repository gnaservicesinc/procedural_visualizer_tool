// Native backend for the existing Remote video track. No FFmpeg video codec.
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
struct Encoder {
    VTCompressionSessionRef session = nullptr;
    std::vector<uint8_t> bytes;
    OSStatus status = noErr;
    int width, height;
    ~Encoder() {
        if (session) {
            VTCompressionSessionInvalidate(session);
            CFRelease(session);
        }
    }
};
void append_nal(Encoder& encoder, const uint8_t* bytes, size_t size) {
    constexpr uint8_t start[] = {0, 0, 0, 1};
    encoder.bytes.insert(encoder.bytes.end(), start, start + 4);
    encoder.bytes.insert(encoder.bytes.end(), bytes, bytes + size);
}
void compressed(void* context, void*, OSStatus status, VTEncodeInfoFlags,
                CMSampleBufferRef sample) {
    auto& encoder = *static_cast<Encoder*>(context);
    encoder.status = status;
    if (status || !sample || !CMSampleBufferDataIsReady(sample)) {
        encoder.status = status ? status : -1;
        return;
    }
    try {
        auto format = CMSampleBufferGetFormatDescription(sample);
        // Repeat SPS/PPS on every access unit, so a new/recovering viewer can
        // decode the next periodic keyframe without out-of-band state.
        int length_size = 0;
        size_t count = 0;
        const uint8_t* parameter = nullptr;
        size_t size = 0;
        encoder.status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            format, 0, &parameter, &size, &count, &length_size);
        if (encoder.status || length_size != 4) { encoder.status = -1; return; }
        for (size_t index = 0; index < count; ++index) {
            encoder.status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                format, index, &parameter, &size, nullptr, nullptr);
            if (encoder.status) return;
            append_nal(encoder, parameter, size);
        }
        auto block = CMSampleBufferGetDataBuffer(sample);
        const size_t total = CMBlockBufferGetDataLength(block);
        std::vector<uint8_t> avcc(total);
        encoder.status = CMBlockBufferCopyDataBytes(block, 0, total, avcc.data());
        if (encoder.status) return;
        size_t offset = 0;
        while (offset + 4 <= total) {
            const size_t length = (size_t(avcc[offset]) << 24)
                | (size_t(avcc[offset + 1]) << 16)
                | (size_t(avcc[offset + 2]) << 8) | avcc[offset + 3];
            offset += 4;
            if (!length || length > total - offset) { encoder.status = -1; return; }
            append_nal(encoder, avcc.data() + offset, length);
            offset += length;
        }
        if (offset != total) encoder.status = -1;
    } catch (...) { encoder.status = -1; }
}
}
extern "C" void* pvt_video_create(int width, int height) {
    @autoreleasepool {
        if (width < 2 || height < 2 || width > 1280 || height > 720
            || (width % 2) || (height % 2)) return nullptr;
        Encoder* encoder = nullptr;
        try {
            encoder = new Encoder;
            encoder->width = width; encoder->height = height;
            // VideoToolbox chooses the available system encoder. There is no
            // fallback to a bundled software video implementation.
            NSDictionary* attributes = @{
                (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                (id)kCVPixelBufferWidthKey: @(width),
                (id)kCVPixelBufferHeightKey: @(height),
                (id)kCVPixelBufferIOSurfacePropertiesKey: @{}
            };
            OSStatus status = VTCompressionSessionCreate(kCFAllocatorDefault,
                width, height, kCMVideoCodecType_H264, nullptr,
                (__bridge CFDictionaryRef)attributes, nullptr, compressed,
                encoder, &encoder->session);
            if (status) { delete encoder; return nullptr; }
            NSDictionary* properties = @{
                (id)kVTCompressionPropertyKey_ColorPrimaries: (id)kCVImageBufferColorPrimaries_ITU_R_709_2,
                (id)kVTCompressionPropertyKey_TransferFunction: (id)kCVImageBufferTransferFunction_ITU_R_709_2,
                (id)kVTCompressionPropertyKey_YCbCrMatrix: (id)kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                (id)kVTCompressionPropertyKey_RealTime: @YES,
                (id)kVTCompressionPropertyKey_AllowFrameReordering: @NO,
                (id)kVTCompressionPropertyKey_ProfileLevel: (id)kVTProfileLevel_H264_Baseline_3_1,
                (id)kVTCompressionPropertyKey_AverageBitRate: @4000000,
                (id)kVTCompressionPropertyKey_ExpectedFrameRate: @30,
                (id)kVTCompressionPropertyKey_MaxKeyFrameInterval: @30,
                (id)kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration: @1
            };
            if (VTSessionSetProperties(encoder->session, (__bridge CFDictionaryRef)properties)
                || VTCompressionSessionPrepareToEncodeFrames(encoder->session)) {
                delete encoder; return nullptr;
            }
            return encoder;
        } catch (...) { delete encoder; return nullptr; }
    }
}
extern "C" int pvt_video_encode(void* handle, const uint8_t* bgra, size_t size,
    int64_t pts, int keyframe, const uint8_t** output, size_t* output_size) {
    @autoreleasepool {
        auto* encoder = static_cast<Encoder*>(handle);
        if (!encoder || !bgra || !output || !output_size
            || size != size_t(encoder->width) * size_t(encoder->height) * 4) return -1;
        *output = nullptr; *output_size = 0;
        encoder->bytes.clear(); encoder->status = noErr;
        CVPixelBufferRef pixel = nullptr;
        OSStatus status = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault,
            VTCompressionSessionGetPixelBufferPool(encoder->session), &pixel);
        if (status) return status;
        CVBufferSetAttachment(pixel, kCVImageBufferColorPrimariesKey,
            kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pixel, kCVImageBufferTransferFunctionKey,
            kCVImageBufferTransferFunction_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pixel, kCVImageBufferYCbCrMatrixKey,
            kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
        status = CVPixelBufferLockBaseAddress(pixel, 0);
        if (status) { CFRelease(pixel); return status; }
        auto* destination = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(pixel));
        const size_t stride = CVPixelBufferGetBytesPerRow(pixel);
        for (int y = 0; y < encoder->height; ++y)
            std::memcpy(destination + size_t(y) * stride,
                bgra + size_t(y) * size_t(encoder->width) * 4, size_t(encoder->width) * 4);
        CVPixelBufferUnlockBaseAddress(pixel, 0);
        NSDictionary* options = keyframe ? @{(id)kVTEncodeFrameOptionKey_ForceKeyFrame: @YES} : nil;
        status = VTCompressionSessionEncodeFrame(encoder->session, pixel,
            CMTimeMake(pts, 90000), kCMTimeInvalid,
            (__bridge CFDictionaryRef)options, nullptr, nullptr);
        CFRelease(pixel);
        if (!status) status = VTCompressionSessionCompleteFrames(encoder->session, kCMTimeInvalid);
        if (status || encoder->status) return status ? status : encoder->status;
        *output = encoder->bytes.data(); *output_size = encoder->bytes.size();
        return *output_size ? 0 : -1;
    }
}
extern "C" void pvt_video_destroy(void* handle) { delete static_cast<Encoder*>(handle); }
