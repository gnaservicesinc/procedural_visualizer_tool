#include "video_export.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#include <png.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pvt::video {
namespace {

namespace fs = std::filesystem;
constexpr CMVideoCodecType kPngVideoCodec = 'png ';
constexpr CMTimeScale kMovieTimeScale = 600000;

bool fail(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
}

bool cancelled(const std::atomic_bool* cancel) {
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

std::string ns_error(NSError* error, const char* fallback) {
    if (error == nil) return fallback;
    const char* text = error.localizedDescription.UTF8String;
    return text != nullptr ? text : fallback;
}

std::string writer_error(AVAssetWriter* writer, const char* fallback) {
    return writer != nil ? ns_error(writer.error, fallback) : fallback;
}

float linear_to_srgb(float value) {
    if (!std::isfinite(value)) return 0.0F;
    value = std::clamp(value, 0.0F, 1.0F);
    if (value <= 0.0031308F) return value * 12.92F;
    return 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
}

float linear_to_rec709(float value) {
    if (!std::isfinite(value)) return 0.0F;
    value = std::clamp(value, 0.0F, 1.0F);
    if (value < 0.018F) return value * 4.5F;
    return 1.099F * std::pow(value, 0.45F) - 0.099F;
}

unsigned char quantize(float value) {
    return static_cast<unsigned char>(
        std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

void convert_pixel(const float* source, bool preserve_alpha, bool rec709,
                   unsigned char& red, unsigned char& green,
                   unsigned char& blue, unsigned char& alpha) {
    const float a = std::clamp(source[3], 0.0F, 1.0F);
    const float coverage = preserve_alpha ? 1.0F : a;
    const auto transfer = rec709 ? linear_to_rec709 : linear_to_srgb;
    red = quantize(transfer(source[0] * coverage));
    green = quantize(transfer(source[1] * coverage));
    blue = quantize(transfer(source[2] * coverage));
    alpha = preserve_alpha ? quantize(a) : 255U;
}

struct PngWriteState {
    std::vector<unsigned char>* bytes = nullptr;
    bool failed = false;
};

void png_write_to_vector(png_structp png, png_bytep data, png_size_t size) {
    auto* state = static_cast<PngWriteState*>(png_get_io_ptr(png));
    if (state == nullptr || state->bytes == nullptr || state->failed) {
        png_error(png, "Invalid PNG memory writer state");
        return;
    }
    try {
        state->bytes->insert(state->bytes->end(), data, data + size);
    } catch (...) {
        state->failed = true;
        png_error(png, "PNG memory allocation failed");
    }
}

void png_flush_vector(png_structp) {}

bool encode_png_sample(const pvt::Image& image, bool preserve_alpha,
                       std::vector<unsigned char>& bytes,
                       std::string* error) {
    if (image.width <= 0 || image.height <= 0
        || image.pixels.size()
               != static_cast<std::size_t>(image.width)
                      * static_cast<std::size_t>(image.height) * 4U) {
        return fail(error, "Rendered frame metadata is inconsistent.");
    }
    const std::size_t components = preserve_alpha ? 4U : 3U;
    const std::size_t raw_bytes =
        static_cast<std::size_t>(image.width)
        * static_cast<std::size_t>(image.height) * components;
    const std::size_t reserve_overhead = raw_bytes / 32U + 65536U;
    if (reserve_overhead
        > std::numeric_limits<std::size_t>::max() - raw_bytes) {
        return fail(error, "Lossless movie frame memory estimate overflowed.");
    }
    try {
        bytes.clear();
        // Deflate's worst-case expansion and PNG chunk overhead are far below
        // this allowance. Reserving it prevents vector growth from briefly
        // doubling every worker's encoded-frame allocation.
        bytes.reserve(raw_bytes + reserve_overhead);
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to encode a lossless movie frame.");
    }
    png_structp png = png_create_write_struct(
        PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (png == nullptr) return fail(error, "Could not create the PNG movie encoder.");
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_write_struct(&png, nullptr);
        return fail(error, "Could not create PNG movie metadata.");
    }
    PngWriteState state{&bytes, false};
    if (setjmp(png_jmpbuf(png)) != 0) {
        png_destroy_write_struct(&png, &info);
        bytes.clear();
        return fail(error, state.failed
                               ? "Not enough memory to encode a lossless movie frame."
                               : "libpng rejected a lossless movie frame.");
    }
    png_set_write_fn(png, &state, png_write_to_vector, png_flush_vector);
    png_set_IHDR(png, info, static_cast<png_uint_32>(image.width),
                 static_cast<png_uint_32>(image.height), 8,
                 preserve_alpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_set_sRGB_gAMA_and_cHRM(png, info, PNG_sRGB_INTENT_PERCEPTUAL);
    // Lossless video favors reasonable encode time; this changes size only.
    png_set_compression_level(png, 3);
    png_write_info(png, info);
    std::vector<unsigned char> row(
        static_cast<std::size_t>(image.width) * components);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            unsigned char red = 0U;
            unsigned char green = 0U;
            unsigned char blue = 0U;
            unsigned char alpha = 255U;
            convert_pixel(image.pixel(x, y), preserve_alpha, false,
                          red, green, blue, alpha);
            const std::size_t offset = static_cast<std::size_t>(x) * components;
            row[offset] = red;
            row[offset + 1U] = green;
            row[offset + 2U] = blue;
            if (preserve_alpha) row[offset + 3U] = alpha;
        }
        png_write_row(png, row.data());
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    return !state.failed;
}

bool encode_bgra_sample(const pvt::Image& image, bool preserve_alpha,
                        std::vector<unsigned char>& bytes,
                        std::string* error) {
    if (image.width <= 0 || image.height <= 0
        || image.pixels.size()
               != static_cast<std::size_t>(image.width)
                      * static_cast<std::size_t>(image.height) * 4U) {
        return fail(error, "Rendered frame metadata is inconsistent.");
    }
    const std::size_t width = static_cast<std::size_t>(image.width);
    const std::size_t height = static_cast<std::size_t>(image.height);
    if (width > std::numeric_limits<std::size_t>::max() / height
        || width * height > std::numeric_limits<std::size_t>::max() / 4U) {
        return fail(error, "Rendered video frame is too large to convert.");
    }
    bytes.resize(width * height * 4U);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            unsigned char red = 0U;
            unsigned char green = 0U;
            unsigned char blue = 0U;
            unsigned char alpha = 255U;
            convert_pixel(image.pixel(x, y), preserve_alpha, true,
                          red, green, blue, alpha);
            const std::size_t offset =
                (static_cast<std::size_t>(y) * width
                 + static_cast<std::size_t>(x)) * 4U;
            bytes[offset] = blue;
            bytes[offset + 1U] = green;
            bytes[offset + 2U] = red;
            bytes[offset + 3U] = alpha;
        }
    }
    return true;
}

struct TemporaryMovie {
    std::string path;
    mode_t preserved_mode = 0;
    bool preserve_mode = false;

    ~TemporaryMovie() {
        if (!path.empty()) (void)::unlink(path.c_str());
    }
};

bool prepare_temporary_movie(const std::string& destination,
                             bool overwrite, TemporaryMovie& temporary,
                             std::string* error) {
    if (destination.empty()) return fail(error, "Video destination is empty.");
    const fs::path output(destination);
    const fs::path parent = output.parent_path().empty()
                                ? fs::current_path() : output.parent_path();
    std::error_code filesystem_error;
    const fs::file_status parent_status = fs::symlink_status(parent, filesystem_error);
    if (filesystem_error || !fs::is_directory(parent_status)
        || fs::is_symlink(parent_status)) {
        return fail(error, "Video destination directory is unavailable or unsafe.");
    }

    struct stat existing{};
    if (::lstat(destination.c_str(), &existing) == 0) {
        if (S_ISDIR(existing.st_mode)) {
            return fail(error, "Video destination is a directory.");
        }
        if (!overwrite) {
            return fail(error, "Video destination already exists.");
        }
        if (S_ISREG(existing.st_mode)) {
            temporary.preserved_mode = existing.st_mode & 07777;
            temporary.preserve_mode = true;
        }
    } else if (errno != ENOENT) {
        return fail(error, std::string("Could not inspect video destination: ")
                               + std::strerror(errno));
    }

    const std::string name = output.filename().string();
    fs::path pattern = parent / ("." + name + ".pvt-video-XXXXXX.mov");
    std::string writable = pattern.string();
    std::vector<char> buffer(writable.begin(), writable.end());
    buffer.push_back('\0');
    const int descriptor = ::mkstemps(buffer.data(), 4);
    if (descriptor < 0) {
        return fail(error, std::string("Could not reserve a sibling video temporary: ")
                               + std::strerror(errno));
    }
    const std::string reserved(buffer.data());
    if (::close(descriptor) != 0 || ::unlink(reserved.c_str()) != 0) {
        const int saved_errno = errno;
        (void)::unlink(reserved.c_str());
        return fail(error, std::string("Could not prepare the video temporary: ")
                               + std::strerror(saved_errno));
    }
    temporary.path = reserved;
    return true;
}

bool install_temporary_movie(TemporaryMovie& temporary,
                             const std::string& destination,
                             bool overwrite, std::string* error) {
    const int descriptor = ::open(temporary.path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        return fail(error, std::string("Could not reopen completed video: ")
                               + std::strerror(errno));
    }
    struct stat completed{};
    const bool valid_movie = ::fstat(descriptor, &completed) == 0
                             && S_ISREG(completed.st_mode)
                             && completed.st_size > 0;
    const bool synced = valid_movie && ::fsync(descriptor) == 0;
    const int close_result = ::close(descriptor);
    if (!valid_movie || !synced || close_result != 0) {
        return fail(error, valid_movie
                               ? "Could not durably flush the completed video."
                               : "The native encoder did not produce a valid nonempty movie file.");
    }
    if (temporary.preserve_mode
        && ::chmod(temporary.path.c_str(), temporary.preserved_mode) != 0) {
        return fail(error, std::string("Could not preserve destination permissions: ")
                               + std::strerror(errno));
    }

    const int rename_result = overwrite
                                  ? ::rename(temporary.path.c_str(), destination.c_str())
                                  : ::renamex_np(temporary.path.c_str(),
                                                 destination.c_str(), RENAME_EXCL);
    if (rename_result != 0) {
        return fail(error,
                    errno == EEXIST
                        ? "Video destination appeared during export; the completed temporary was not installed."
                        : std::string("Could not install completed video: ")
                              + std::strerror(errno));
    }
    temporary.path.clear();
    const fs::path output(destination);
    const fs::path parent = output.parent_path().empty()
                                ? fs::current_path() : output.parent_path();
    const int directory = ::open(parent.string().c_str(), O_RDONLY);
    if (directory >= 0) {
        (void)::fsync(directory);
        (void)::close(directory);
    }
    return true;
}

bool hardware_encoder_available(CMVideoCodecType codec) {
    CFArrayRef encoders = nullptr;
    if (VTCopyVideoEncoderList(nullptr, &encoders) != noErr
        || encoders == nullptr) {
        return false;
    }
    bool found = false;
    const CFIndex count = CFArrayGetCount(encoders);
    for (CFIndex index = 0; index < count && !found; ++index) {
        auto dictionary = static_cast<CFDictionaryRef>(
            CFArrayGetValueAtIndex(encoders, index));
        if (dictionary == nullptr) continue;
        auto codec_number = static_cast<CFNumberRef>(CFDictionaryGetValue(
            dictionary, kVTVideoEncoderList_CodecType));
        auto hardware = static_cast<CFBooleanRef>(CFDictionaryGetValue(
            dictionary, kVTVideoEncoderList_IsHardwareAccelerated));
        std::int32_t listed_codec = 0;
        if (codec_number != nullptr
            && CFNumberGetValue(codec_number, kCFNumberSInt32Type,
                                &listed_codec)
            && static_cast<CMVideoCodecType>(listed_codec) == codec
            && hardware == kCFBooleanTrue) {
            found = true;
        }
    }
    CFRelease(encoders);
    return found;
}

NSString* av_codec(Codec codec, bool alpha) {
    switch (codec) {
        case Codec::ProRes4444: return AVVideoCodecTypeAppleProRes4444;
        case Codec::ProRes4444Xq:
            if (@available(macOS 15.0, *)) {
                return AVVideoCodecTypeAppleProRes4444XQ;
            }
            return nil;
        case Codec::Hevc:
            return alpha ? AVVideoCodecTypeHEVCWithAlpha : AVVideoCodecTypeHEVC;
        case Codec::PngLossless: return nil;
    }
    return nil;
}

bool wait_until_ready(AVAssetWriterInput* input, AVAssetWriter* writer,
                      const std::atomic_bool* cancel, std::string* error) {
    while (!input.readyForMoreMediaData) {
        if (cancelled(cancel)) return fail(error, "Video export was cancelled.");
        if (writer.status == AVAssetWriterStatusFailed
            || writer.status == AVAssetWriterStatusCancelled) {
            return fail(error, writer_error(writer, "The movie writer stopped."));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

bool append_png_frame(AVAssetWriterInput* input, AVAssetWriter* writer,
                      CMVideoFormatDescriptionRef format,
                      const std::vector<unsigned char>& png,
                      CMTime presentation, CMTime duration,
                      const std::atomic_bool* cancel,
                      std::string* error) {
    if (!wait_until_ready(input, writer, cancel, error)) return false;
    CMBlockBufferRef block = nullptr;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault, nullptr, png.size(), kCFAllocatorDefault,
        nullptr, 0U, png.size(), 0U, &block);
    if (status == noErr) {
        status = CMBlockBufferReplaceDataBytes(
            png.data(), block, 0U, png.size());
    }
    if (status != noErr || block == nullptr) {
        if (block != nullptr) CFRelease(block);
        return fail(error, "Could not create a lossless video sample buffer.");
    }
    CMSampleTimingInfo timing{duration, presentation, kCMTimeInvalid};
    const std::size_t sample_size = png.size();
    CMSampleBufferRef sample = nullptr;
    status = CMSampleBufferCreateReady(
        kCFAllocatorDefault, block, format, 1U, 1U, &timing,
        1U, &sample_size, &sample);
    CFRelease(block);
    if (status != noErr || sample == nullptr) {
        if (sample != nullptr) CFRelease(sample);
        return fail(error, "Could not describe a lossless video sample.");
    }
    const bool appended = [input appendSampleBuffer:sample];
    CFRelease(sample);
    return appended
               ? true
               : fail(error, writer_error(writer,
                                           "Could not append a lossless video frame."));
}

bool fill_pixel_buffer(CVPixelBufferRef buffer,
                       const std::vector<unsigned char>& bgra,
                       int width, int height, bool preserve_alpha,
                       std::string* error) {
    if (buffer == nullptr || width <= 0 || height <= 0) {
        return fail(error, "Video pixel buffer is missing.");
    }
    const std::size_t packed_stride = static_cast<std::size_t>(width) * 4U;
    if (static_cast<std::size_t>(height)
            > std::numeric_limits<std::size_t>::max() / packed_stride
        || bgra.size()
               != packed_stride * static_cast<std::size_t>(height)) {
        return fail(error, "Converted video frame metadata is inconsistent.");
    }
    const CVReturn locked = CVPixelBufferLockBaseAddress(buffer, 0U);
    if (locked != kCVReturnSuccess) {
        return fail(error, "Could not lock a VideoToolbox pixel buffer.");
    }
    auto* base = static_cast<unsigned char*>(CVPixelBufferGetBaseAddress(buffer));
    const std::size_t stride = CVPixelBufferGetBytesPerRow(buffer);
    if (base == nullptr || stride < packed_stride
        || CVPixelBufferGetHeight(buffer)
               < static_cast<std::size_t>(height)) {
        CVPixelBufferUnlockBaseAddress(buffer, 0U);
        return fail(error, "VideoToolbox supplied an undersized pixel buffer.");
    }
    for (int y = 0; y < height; ++y) {
        unsigned char* row = base + static_cast<std::size_t>(y) * stride;
        const unsigned char* source =
            bgra.data() + static_cast<std::size_t>(y) * packed_stride;
        std::memcpy(row, source, packed_stride);
    }
    CVPixelBufferUnlockBaseAddress(buffer, 0U);
    CVBufferSetAttachment(buffer, kCVImageBufferColorPrimariesKey,
                          kCVImageBufferColorPrimaries_ITU_R_709_2,
                          kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(buffer, kCVImageBufferTransferFunctionKey,
                          kCVImageBufferTransferFunction_ITU_R_709_2,
                          kCVAttachmentMode_ShouldPropagate);
    if (preserve_alpha) {
        CVBufferSetAttachment(buffer, kCVImageBufferAlphaChannelModeKey,
                              kCVImageBufferAlphaChannelMode_StraightAlpha,
                              kCVAttachmentMode_ShouldPropagate);
    }
    return true;
}

bool checked_multiply(std::size_t left, std::size_t right,
                      std::size_t* result) {
    if (left != 0U
        && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    *result = left * right;
    return true;
}

bool checked_add(std::size_t left, std::size_t right,
                 std::size_t* result) {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    *result = left + right;
    return true;
}

bool select_video_worker_count(const pvt::ProjectConfig& project,
                               const pvt::ValidationResult& validation,
                               const Options& options, int total_frames,
                               std::size_t* worker_count,
                               std::string* error) {
    if (options.worker_count > pvt::kMaximumSequenceWorkers) {
        return fail(error, "Video render worker count cannot exceed "
                           + std::to_string(pvt::kMaximumSequenceWorkers)
                           + ".");
    }
    std::size_t pixel_count = 0U;
    std::size_t converted_frame_bytes = 0U;
    if (!checked_multiply(static_cast<std::size_t>(project.canvas.width),
                          static_cast<std::size_t>(project.canvas.height),
                          &pixel_count)
        || !checked_multiply(pixel_count, 4U, &converted_frame_bytes)) {
        return fail(error, "Video frame memory estimate overflowed.");
    }

    std::size_t render_peak = validation.estimated_peak_bytes;
    if (options.frame.backend == pvt::RenderBackend::CpuAndGpu
        && !checked_multiply(render_peak, 2U, &render_peak)) {
        render_peak = std::numeric_limits<std::size_t>::max();
    }
    std::size_t queued_frame_bytes = 0U;
    const std::size_t queue_overhead =
        converted_frame_bytes / 32U + 65536U;
    if (!checked_add(converted_frame_bytes, queue_overhead,
                     &queued_frame_bytes)) {
        queued_frame_bytes = std::numeric_limits<std::size_t>::max();
    }
    std::size_t worker_peak = 0U;
    if (!checked_add(render_peak, queued_frame_bytes, &worker_peak)) {
        worker_peak = std::numeric_limits<std::size_t>::max();
    }

    const std::size_t hardware =
        std::max<std::size_t>(1U, std::thread::hardware_concurrency());
    const std::size_t requested = options.worker_count == 0U
                                      ? hardware : options.worker_count;
    const std::size_t budget = options.memory_budget_bytes == 0U
                                   ? pvt::kDefaultSequenceMemoryBudgetBytes
                                   : options.memory_budget_bytes;
    const std::size_t memory_limited = worker_peak == 0U
        ? requested
        : std::max<std::size_t>(1U, budget / worker_peak);
    *worker_count = std::max<std::size_t>(
        1U, std::min({requested, memory_limited,
                      static_cast<std::size_t>(total_frames),
                      pvt::kMaximumSequenceWorkers}));
    return true;
}

struct VideoFrameResult {
    bool ok = false;
    int frame_index = -1;
    std::vector<unsigned char> bytes;
    std::string error;
    std::exception_ptr exception;
};

struct VideoWorkerSlot {
    bool ready = false;
    VideoFrameResult result;
};

std::string worker_exception_message(const std::exception_ptr& exception) {
    try {
        if (exception != nullptr) std::rethrow_exception(exception);
    } catch (const std::bad_alloc&) {
        return "worker ran out of memory";
    } catch (const std::exception& value) {
        return value.what();
    } catch (...) {
        return "worker failed with an unknown exception";
    }
    return "worker failed without an error";
}

class VideoWorkerJoiner {
public:
    VideoWorkerJoiner(std::vector<std::thread>& threads,
                      std::atomic_bool& stop,
                      std::condition_variable& wake)
        : threads_(threads), stop_(stop), wake_(wake) {}

    VideoWorkerJoiner(const VideoWorkerJoiner&) = delete;
    VideoWorkerJoiner& operator=(const VideoWorkerJoiner&) = delete;

    ~VideoWorkerJoiner() {
        stop_.store(true, std::memory_order_relaxed);
        wake_.notify_all();
        for (std::thread& thread : threads_) {
            if (thread.joinable()) thread.join();
        }
    }

private:
    std::vector<std::thread>& threads_;
    std::atomic_bool& stop_;
    std::condition_variable& wake_;
};

NSDictionary* video_settings(const pvt::ProjectConfig& project,
                             const Options& options) {
    NSString* codec = av_codec(options.codec, options.preserve_alpha);
    if (codec == nil) return nil;
    NSMutableDictionary* compression = [NSMutableDictionary dictionary];
    compression[AVVideoExpectedSourceFrameRateKey] = @(project.canvas.fps);
    compression[AVVideoAllowFrameReorderingKey] = @NO;
    if (options.codec == Codec::Hevc) {
        double bits_per_pixel = 1.5;
        if (options.hevc_quality == HevcQuality::VeryLightCompression) {
            bits_per_pixel = 0.75;
        } else if (options.hevc_quality == HevcQuality::HighQuality) {
            bits_per_pixel = 0.35;
        }
        const long double requested =
            static_cast<long double>(project.canvas.width)
            * static_cast<long double>(project.canvas.height)
            * static_cast<long double>(project.canvas.fps) * bits_per_pixel;
        const long long bitrate = static_cast<long long>(std::clamp(
            requested, static_cast<long double>(1000000.0L),
            static_cast<long double>(2000000000.0L)));
        compression[AVVideoAverageBitRateKey] = @(bitrate);
        compression[(__bridge NSString*)kVTCompressionPropertyKey_Quality] =
            options.hevc_quality == HevcQuality::MaximumFidelity
                ? @0.99 : (options.hevc_quality
                                   == HevcQuality::VeryLightCompression
                               ? @0.96 : @0.92);
    }
    if (options.preserve_alpha) {
        compression[(__bridge NSString*)kVTCompressionPropertyKey_PreserveAlphaChannel]
            = @YES;
        compression[(__bridge NSString*)kVTCompressionPropertyKey_TargetQualityForAlpha]
            = @1.0;
        compression[(__bridge NSString*)kVTCompressionPropertyKey_AlphaChannelMode]
            = (__bridge NSString*)kVTAlphaChannelMode_StraightAlpha;
    }

    NSMutableDictionary* encoder = [NSMutableDictionary dictionary];
    if (options.hardware == HardwarePolicy::Require) {
        encoder[(__bridge NSString*)
                    kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder]
            = @YES;
    } else {
        encoder[(__bridge NSString*)
                    kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder]
            = @(options.hardware == HardwarePolicy::Prefer);
    }
    NSMutableDictionary* settings = [@{
        AVVideoCodecKey : codec,
        AVVideoWidthKey : @(project.canvas.width),
        AVVideoHeightKey : @(project.canvas.height)
    } mutableCopy];
    if (options.codec == Codec::Hevc) {
        settings[AVVideoCompressionPropertiesKey] = compression;
    }
    settings[AVVideoEncoderSpecificationKey] = encoder;
    settings[AVVideoColorPropertiesKey] = @{
        AVVideoColorPrimariesKey : AVVideoColorPrimaries_ITU_R_709_2,
        AVVideoTransferFunctionKey : AVVideoTransferFunction_ITU_R_709_2,
        AVVideoYCbCrMatrixKey : AVVideoYCbCrMatrix_ITU_R_709_2
    };
    return settings;
}

struct AudioTrack {
    AVAssetReader* reader = nil;
    AVAssetReaderTrackOutput* output = nil;
    AVAssetWriterInput* input = nil;
};

bool prepare_audio_track(const std::string& path, CMTime start, CMTime duration,
                         AVAssetWriter* writer, AudioTrack& audio,
                         std::string* error) {
    NSString* source = [NSString stringWithUTF8String:path.c_str()];
    if (source == nil) return fail(error, "Project music path is not valid UTF-8.");
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:
        [NSURL fileURLWithPath:source] options:nil];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    // The synchronous accessor is retained for macOS 13/14 compatibility.
    // Export already runs off the UI thread; macOS 15's async replacement
    // would not improve responsiveness here.
    AVAssetTrack* track = [[asset tracksWithMediaType:AVMediaTypeAudio]
        firstObject];
#pragma clang diagnostic pop
    if (track == nil) return fail(error, "Project music contains no audio track.");
    NSError* reader_error = nil;
    audio.reader = [[AVAssetReader alloc] initWithAsset:asset
                                                  error:&reader_error];
    if (audio.reader == nil) {
        return fail(error, ns_error(reader_error,
                                    "Could not create the project-music reader."));
    }
    audio.reader.timeRange = CMTimeRangeMake(start, duration);
    audio.output = [[AVAssetReaderTrackOutput alloc]
        initWithTrack:track outputSettings:nil];
    audio.output.alwaysCopiesSampleData = NO;
    if (![audio.reader canAddOutput:audio.output]) {
        return fail(error, "The original project music cannot be read for movie export.");
    }
    [audio.reader addOutput:audio.output];
    CMFormatDescriptionRef hint = track.formatDescriptions.count > 0
                                      ? (__bridge CMFormatDescriptionRef)
                                            track.formatDescriptions.firstObject
                                      : nullptr;
    audio.input = [[AVAssetWriterInput alloc]
        initWithMediaType:AVMediaTypeAudio outputSettings:nil
        sourceFormatHint:hint];
    audio.input.expectsMediaDataInRealTime = NO;
    if (![writer canAddInput:audio.input]) {
        return fail(error,
                    "The QuickTime container cannot copy the original project-music format.");
    }
    [writer addInput:audio.input];
    return true;
}

bool finish_writer(AVAssetWriter* writer, std::string* error) {
    dispatch_semaphore_t completed = dispatch_semaphore_create(0);
    [writer finishWritingWithCompletionHandler:^{
        dispatch_semaphore_signal(completed);
    }];
    dispatch_semaphore_wait(completed, DISPATCH_TIME_FOREVER);
    return writer.status == AVAssetWriterStatusCompleted
               ? true
               : fail(error, writer_error(writer,
                                           "The native movie writer did not finish."));
}

} // namespace

const char* codec_name(Codec codec) {
    switch (codec) {
        case Codec::PngLossless: return "Lossless PNG in QuickTime";
        case Codec::ProRes4444: return "Apple ProRes 4444";
        case Codec::ProRes4444Xq: return "Apple ProRes 4444 XQ";
        case Codec::Hevc: return "HEVC";
    }
    return "Unknown";
}

Capabilities capabilities() {
    Capabilities result;
    result.available = true;
    result.png_lossless = true;
    result.prores_4444 = true;
    result.prores_4444_xq = false;
    if (@available(macOS 15.0, *)) result.prores_4444_xq = true;
    result.hevc = true;
    result.hevc_alpha = true;
    result.prores_4444_hardware = hardware_encoder_available(
        kCMVideoCodecType_AppleProRes4444);
    result.prores_4444_xq_hardware = result.prores_4444_xq
        && hardware_encoder_available(kCMVideoCodecType_AppleProRes4444XQ);
    result.hevc_hardware = hardware_encoder_available(kCMVideoCodecType_HEVC);
    result.hevc_alpha_hardware = hardware_encoder_available(
        kCMVideoCodecType_HEVCWithAlpha);
    result.status = "AVFoundation and VideoToolbox are available.";
    return result;
}

bool export_project(const pvt::ProjectConfig& project,
                    const std::string& destination,
                    const Options& options,
                    const ProgressCallback& progress,
                    const std::atomic_bool* cancel,
                    Report* report, std::string* error) {
    if (error != nullptr) error->clear();
    @autoreleasepool {
        const pvt::ValidationResult validation = pvt::validate(project);
        if (!validation.ok) return fail(error, validation.message);
        std::string frame_count_error;
        const int full_frame_count = pvt::effective_frame_count(
            project.canvas, &frame_count_error);
        if (full_frame_count < 1) return fail(error, frame_count_error);
        if (options.first_frame < 0 || options.first_frame >= full_frame_count
            || options.frame_count < 0
            || (options.frame_count > 0
                && options.frame_count
                       > full_frame_count - options.first_frame)) {
            return fail(error,
                        "The requested video frame range is outside the project timeline.");
        }
        const int total_frames = options.frame_count > 0
            ? options.frame_count : full_frame_count - options.first_frame;
        std::size_t render_worker_count = 0U;
        if (!select_video_worker_count(project, validation, options,
                                       total_frames, &render_worker_count,
                                       error)) {
            return false;
        }
        const std::size_t hardware_workers = std::max<std::size_t>(
            1U, std::thread::hardware_concurrency());
        const std::size_t total_cpu_budget = std::min(
            hardware_workers,
            pvt::kMaximumSequenceWorkers);
        const std::size_t cpu_workers_per_frame =
            total_cpu_budget / render_worker_count;
        const std::size_t cpu_worker_remainder =
            total_cpu_budget % render_worker_count;
        const std::size_t aggregate_memory_budget =
            options.memory_budget_bytes == 0U
                ? pvt::kDefaultSequenceMemoryBudgetBytes
                : options.memory_budget_bytes;
        const std::size_t memory_bytes_per_frame =
            aggregate_memory_budget / render_worker_count;
        const std::size_t memory_byte_remainder =
            aggregate_memory_budget % render_worker_count;
        if (options.codec != Codec::PngLossless
            && ((project.canvas.width & 1) != 0
                || (project.canvas.height & 1) != 0)) {
            return fail(error,
                        "VideoToolbox compressed export requires even canvas dimensions; use lossless PNG video for an odd-sized canvas.");
        }
        const Capabilities available = capabilities();
        bool codec_available = false;
        bool hardware_available = false;
        switch (options.codec) {
            case Codec::PngLossless:
                codec_available = available.png_lossless;
                break;
            case Codec::ProRes4444:
                codec_available = available.prores_4444;
                hardware_available = available.prores_4444_hardware;
                break;
            case Codec::ProRes4444Xq:
                codec_available = available.prores_4444_xq;
                hardware_available = available.prores_4444_xq_hardware;
                break;
            case Codec::Hevc:
                codec_available = options.preserve_alpha
                                      ? available.hevc_alpha : available.hevc;
                hardware_available = options.preserve_alpha
                                         ? available.hevc_alpha_hardware
                                         : available.hevc_hardware;
                break;
        }
        if (!codec_available) return fail(error, "Selected video codec is unavailable.");
        if (options.hardware == HardwarePolicy::Require
            && options.codec != Codec::PngLossless && !hardware_available) {
            return fail(error,
                        "No hardware VideoToolbox encoder advertises support for the selected format.");
        }
        if (cancelled(cancel)) return fail(error, "Video export was cancelled.");

        TemporaryMovie temporary;
        if (!prepare_temporary_movie(destination, options.overwrite_existing,
                                     temporary, error)) return false;
        NSString* temporary_path = [NSString stringWithUTF8String:
            temporary.path.c_str()];
        if (temporary_path == nil) return fail(error, "Temporary video path is invalid.");
        NSError* writer_creation_error = nil;
        AVAssetWriter* writer = [[AVAssetWriter alloc]
            initWithURL:[NSURL fileURLWithPath:temporary_path]
            fileType:AVFileTypeQuickTimeMovie error:&writer_creation_error];
        if (writer == nil) {
            return fail(error, ns_error(writer_creation_error,
                                        "Could not create the native movie writer."));
        }
        writer.shouldOptimizeForNetworkUse = NO;
        const CMTime frame_duration = CMTimeMakeWithSeconds(
            1.0 / project.canvas.fps, kMovieTimeScale);
        const CMTime movie_duration = CMTimeMultiply(
            frame_duration, static_cast<std::int32_t>(total_frames));
        const CMTime audio_start = CMTimeMultiply(
            frame_duration, static_cast<std::int32_t>(options.first_frame));

        AVAssetWriterInput* video_input = nil;
        AVAssetWriterInputPixelBufferAdaptor* adaptor = nil;
        CMVideoFormatDescriptionRef png_format = nullptr;
        if (options.codec == Codec::PngLossless) {
            const OSStatus format_status = CMVideoFormatDescriptionCreate(
                kCFAllocatorDefault, kPngVideoCodec, project.canvas.width,
                project.canvas.height, nullptr, &png_format);
            if (format_status != noErr || png_format == nullptr) {
                return fail(error, "Could not describe the lossless PNG movie track.");
            }
            video_input = [[AVAssetWriterInput alloc]
                initWithMediaType:AVMediaTypeVideo outputSettings:nil
                sourceFormatHint:png_format];
        } else {
            NSDictionary* settings = video_settings(project, options);
            if (settings == nil
                || ![writer canApplyOutputSettings:settings
                                      forMediaType:AVMediaTypeVideo]) {
                NSDictionary* minimal = @{
                    AVVideoCodecKey : av_codec(options.codec, options.preserve_alpha),
                    AVVideoWidthKey : @(project.canvas.width),
                    AVVideoHeightKey : @(project.canvas.height)
                };
                const bool minimal_supported =
                    [writer canApplyOutputSettings:minimal
                                      forMediaType:AVMediaTypeVideo];
                return fail(error,
                            minimal_supported
                                ? "VideoToolbox accepted the codec but rejected advanced encoder settings."
                                : "VideoToolbox rejected the selected codec or dimensions.");
            }
            video_input = [[AVAssetWriterInput alloc]
                initWithMediaType:AVMediaTypeVideo outputSettings:settings];
            NSDictionary* pixel_attributes = @{
                (NSString*)kCVPixelBufferPixelFormatTypeKey :
                    @(kCVPixelFormatType_32BGRA),
                (NSString*)kCVPixelBufferWidthKey : @(project.canvas.width),
                (NSString*)kCVPixelBufferHeightKey : @(project.canvas.height),
                (NSString*)kCVPixelBufferMetalCompatibilityKey : @YES,
                (NSString*)kCVPixelBufferIOSurfacePropertiesKey : @{}
            };
            adaptor = [[AVAssetWriterInputPixelBufferAdaptor alloc]
                initWithAssetWriterInput:video_input
                sourcePixelBufferAttributes:pixel_attributes];
        }
        video_input.expectsMediaDataInRealTime = NO;
        if (![writer canAddInput:video_input]) {
            if (png_format != nullptr) CFRelease(png_format);
            return fail(error, "Could not add the selected video track to the movie.");
        }
        [writer addInput:video_input];

        AudioTrack audio;
        const bool include_audio = options.include_project_music
                                   && !options.music_source_path.empty();
        if (include_audio
            && !prepare_audio_track(options.music_source_path, audio_start,
                                    movie_duration,
                                    writer, audio, error)) {
            if (png_format != nullptr) CFRelease(png_format);
            return false;
        }
        if (![writer startWriting]) {
            if (png_format != nullptr) CFRelease(png_format);
            return fail(error, writer_error(writer, "Could not start movie writing."));
        }
        [writer startSessionAtSourceTime:audio_start];

        std::atomic_bool audio_failed{false};
        std::mutex audio_error_mutex;
        std::string audio_error;
        std::thread audio_thread;
        if (include_audio) {
            if (![audio.reader startReading]) {
                [writer cancelWriting];
                if (png_format != nullptr) CFRelease(png_format);
                return fail(error, ns_error(audio.reader.error,
                                            "Could not start reading project music."));
            }
            audio_thread = std::thread([&] {
                @autoreleasepool {
                    for (;;) {
                        if (cancelled(cancel)) {
                            [audio.reader cancelReading];
                            break;
                        }
                        while (!audio.input.readyForMoreMediaData) {
                            if (cancelled(cancel)
                                || writer.status == AVAssetWriterStatusFailed
                                || writer.status == AVAssetWriterStatusCancelled) {
                                [audio.reader cancelReading];
                                break;
                            }
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(2));
                        }
                        if (cancelled(cancel)
                            || writer.status == AVAssetWriterStatusFailed
                            || writer.status == AVAssetWriterStatusCancelled) break;
                        CMSampleBufferRef sample =
                            [audio.output copyNextSampleBuffer];
                        if (sample == nullptr) break;
                        const bool appended =
                            [audio.input appendSampleBuffer:sample];
                        CFRelease(sample);
                        if (!appended) {
                            std::lock_guard<std::mutex> lock(audio_error_mutex);
                            audio_error = writer_error(
                                writer, "Could not append project music to the movie.");
                            audio_failed.store(true, std::memory_order_relaxed);
                            break;
                        }
                    }
                    [audio.input markAsFinished];
                    if (audio.reader.status == AVAssetReaderStatusFailed) {
                        std::lock_guard<std::mutex> lock(audio_error_mutex);
                        audio_error = ns_error(
                            audio.reader.error,
                            "Project music failed while the movie was being written.");
                        audio_failed.store(true, std::memory_order_relaxed);
                    }
                }
            });
        }

        bool ok = true;
        std::string work_error;
        try {
            if (progress && !progress(0, total_frames)) {
                ok = false;
                work_error =
                    "Video export was cancelled by the progress callback.";
            }
        } catch (const std::exception& exception) {
            ok = false;
            work_error = "Video progress callback failed: "
                         + std::string(exception.what());
        } catch (...) {
            ok = false;
            work_error =
                "Video progress callback failed with an unknown exception.";
        }

        {
            // Rendering and pixel conversion are independent across frames.
            // Each worker owns one ordered slot, bounding queued memory while
            // the calling thread remains the sole AVFoundation appender.
            std::atomic_bool stop{!ok};
            std::atomic<int> next_frame{0};
            std::mutex frame_mutex;
            std::condition_variable frame_wake;
            std::vector<VideoWorkerSlot> slots(render_worker_count);
            std::vector<std::thread> frame_threads;
            frame_threads.reserve(render_worker_count);
            std::exception_ptr scheduler_exception;
            VideoWorkerJoiner joiner(frame_threads, stop, frame_wake);

            if (ok) {
                try {
                    for (std::size_t worker = 0U;
                         worker < render_worker_count; ++worker) {
                        frame_threads.emplace_back([&, worker] {
                            @autoreleasepool {
                                try {
                                    pvt::FrameRenderOptions frame_options =
                                        options.frame;
                                    if (frame_options.maximum_cpu_workers == 0U) {
                                        frame_options.maximum_cpu_workers =
                                            std::max<std::size_t>(
                                                1U, cpu_workers_per_frame
                                                        + (worker
                                                               < cpu_worker_remainder
                                                               ? 1U : 0U));
                                    }
                                    if (frame_options.cpu_memory_budget_bytes
                                        == 0U) {
                                        frame_options.cpu_memory_budget_bytes =
                                            std::max<std::size_t>(
                                                1U, memory_bytes_per_frame
                                                        + (worker
                                                               < memory_byte_remainder
                                                               ? 1U : 0U));
                                    }
                                    pvt::Image image;
                                    for (;;) {
                                        {
                                            std::unique_lock<std::mutex> lock(
                                                frame_mutex);
                                            frame_wake.wait(lock, [&] {
                                                return stop.load(
                                                           std::memory_order_relaxed)
                                                       || !slots[worker].ready;
                                            });
                                        }
                                        if (stop.load(std::memory_order_relaxed)) {
                                            return;
                                        }
                                        const int frame = next_frame.fetch_add(
                                            1, std::memory_order_relaxed);
                                        if (frame >= total_frames) return;

                                        VideoFrameResult result;
                                        result.frame_index = frame;
                                        try {
                                            if (pvt::render_project_frame(
                                                    project,
                                                    options.first_frame + frame,
                                                    frame_options,
                                                    image, &stop, &result.error)) {
                                                result.ok = options.codec
                                                        == Codec::PngLossless
                                                    ? encode_png_sample(
                                                          image,
                                                          options.preserve_alpha,
                                                          result.bytes,
                                                          &result.error)
                                                    : encode_bgra_sample(
                                                          image,
                                                          options.preserve_alpha,
                                                          result.bytes,
                                                          &result.error);
                                            }
                                        } catch (...) {
                                            result.exception =
                                                std::current_exception();
                                        }
                                        {
                                            std::lock_guard<std::mutex> lock(
                                                frame_mutex);
                                            slots[worker].result =
                                                std::move(result);
                                            slots[worker].ready = true;
                                        }
                                        frame_wake.notify_all();
                                    }
                                } catch (...) {
                                    {
                                        std::lock_guard<std::mutex> lock(
                                            frame_mutex);
                                        if (scheduler_exception == nullptr) {
                                            scheduler_exception =
                                                std::current_exception();
                                        }
                                    }
                                    stop.store(true,
                                               std::memory_order_relaxed);
                                    frame_wake.notify_all();
                                }
                            }
                        });
                    }
                } catch (const std::exception& exception) {
                    ok = false;
                    work_error = "Could not start parallel video rendering: "
                                 + std::string(exception.what());
                    stop.store(true, std::memory_order_relaxed);
                    frame_wake.notify_all();
                } catch (...) {
                    ok = false;
                    work_error =
                        "Could not start parallel video rendering.";
                    stop.store(true, std::memory_order_relaxed);
                    frame_wake.notify_all();
                }
            }

            for (int expected = 0; ok && expected < total_frames;
                 ++expected) {
                VideoFrameResult result;
                {
                    std::unique_lock<std::mutex> lock(frame_mutex);
                    for (;;) {
                        if (scheduler_exception != nullptr) {
                            ok = false;
                            work_error = "Video render worker failed: "
                                + worker_exception_message(
                                      scheduler_exception) + ".";
                            break;
                        }
                        if (cancelled(cancel)) {
                            ok = false;
                            work_error = "Video export was cancelled.";
                            break;
                        }
                        if (audio_failed.load(std::memory_order_relaxed)) {
                            ok = false;
                            std::lock_guard<std::mutex> audio_lock(
                                audio_error_mutex);
                            work_error = audio_error;
                            break;
                        }
                        auto ready = std::find_if(
                            slots.begin(), slots.end(),
                            [expected](const VideoWorkerSlot& slot) {
                                return slot.ready
                                       && slot.result.frame_index == expected;
                            });
                        if (ready != slots.end()) {
                            result = std::move(ready->result);
                            ready->ready = false;
                            break;
                        }
                        frame_wake.wait_for(
                            lock, std::chrono::milliseconds(10));
                    }
                }
                frame_wake.notify_all();
                if (!ok) break;
                if (result.exception != nullptr) {
                    ok = false;
                    work_error = "Could not process video frame "
                        + std::to_string(expected) + ": "
                        + worker_exception_message(result.exception) + ".";
                    break;
                }
                if (!result.ok) {
                    ok = false;
                    work_error = "Could not render or convert video frame "
                        + std::to_string(expected) + ": " + result.error;
                    break;
                }

                const CMTime presentation = CMTimeMultiply(
                    frame_duration,
                    static_cast<std::int32_t>(options.first_frame + expected));
                if (options.codec == Codec::PngLossless) {
                    ok = append_png_frame(
                        video_input, writer, png_format, result.bytes,
                        presentation, frame_duration, cancel, &work_error);
                } else if (wait_until_ready(video_input, writer, cancel,
                                            &work_error)) {
                    CVPixelBufferRef pixel = nullptr;
                    const CVReturn pixel_status =
                        CVPixelBufferPoolCreatePixelBuffer(
                            kCFAllocatorDefault, adaptor.pixelBufferPool,
                            &pixel);
                    if (pixel_status != kCVReturnSuccess || pixel == nullptr) {
                        ok = false;
                        work_error =
                            "Could not allocate a VideoToolbox pixel buffer.";
                    } else {
                        ok = fill_pixel_buffer(
                                 pixel, result.bytes,
                                 project.canvas.width,
                                 project.canvas.height,
                                 options.preserve_alpha, &work_error)
                             && [adaptor appendPixelBuffer:pixel
                                     withPresentationTime:presentation];
                        if (!ok && work_error.empty()) {
                            work_error = writer_error(
                                writer,
                                "Could not append a VideoToolbox video frame.");
                        }
                        CVPixelBufferRelease(pixel);
                    }
                } else {
                    ok = false;
                }

                if (ok && progress) {
                    try {
                        if (!progress(expected + 1, total_frames)) {
                            ok = false;
                            work_error =
                                "Video export was cancelled by the progress callback.";
                        }
                    } catch (const std::exception& exception) {
                        ok = false;
                        work_error = "Video progress callback failed: "
                                     + std::string(exception.what());
                    } catch (...) {
                        ok = false;
                        work_error =
                            "Video progress callback failed with an unknown exception.";
                    }
                }
                if (audio_failed.load(std::memory_order_relaxed)) {
                    ok = false;
                    std::lock_guard<std::mutex> lock(audio_error_mutex);
                    work_error = audio_error;
                }
                if (!ok) {
                    stop.store(true, std::memory_order_relaxed);
                    frame_wake.notify_all();
                }
            }
            stop.store(true, std::memory_order_relaxed);
            frame_wake.notify_all();
        }
        [video_input markAsFinished];
        if (!ok || cancelled(cancel)) {
            if (include_audio) [audio.reader cancelReading];
            [writer cancelWriting];
        }
        if (audio_thread.joinable()) audio_thread.join();
        if (png_format != nullptr) CFRelease(png_format);
        if (audio_failed.load(std::memory_order_relaxed)) {
            ok = false;
            std::lock_guard<std::mutex> lock(audio_error_mutex);
            if (work_error.empty()) work_error = audio_error;
        }
        if (!ok || cancelled(cancel)) {
            return fail(error, work_error.empty()
                                   ? "Video export was cancelled." : work_error);
        }
        [writer endSessionAtSourceTime:CMTimeAdd(audio_start, movie_duration)];
        if (!finish_writer(writer, error)) return false;
        if (!install_temporary_movie(temporary, destination,
                                     options.overwrite_existing, error)) {
            return false;
        }
        if (report != nullptr) {
            report->hardware_required =
                options.hardware == HardwarePolicy::Require;
            report->hardware_available = hardware_available;
            report->included_audio = include_audio;
            report->render_workers = render_worker_count;
            report->format_name = codec_name(options.codec);
        }
        return true;
    }
}

} // namespace pvt::video
