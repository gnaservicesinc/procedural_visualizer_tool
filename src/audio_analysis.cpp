#include "audio_analysis.h"

#include "BTT.h"
#include "miniaudio.h"
#include "path_utf8.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <numeric>
#include <string>
#include <string_view>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace pvt {
namespace audio {
namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kAnalysisSampleRate = 44100U;
constexpr std::uint64_t kMaximumDurationSeconds = 2U * 60U * 60U;
constexpr std::uint64_t kMaximumSourceBytes = kMaximumEmbeddedAssetBytes;
constexpr std::uint32_t kMinimumSourceSampleRate = 8000U;
constexpr std::uint32_t kMaximumSourceSampleRate = 384000U;
constexpr std::uint32_t kMaximumSourceChannels = 32U;
constexpr std::uint64_t kDecodeChunkFrames = 4096U;
constexpr std::uint64_t kHashChunkBytes = 1024U * 1024U;
constexpr std::uint64_t kProgressTotal = 1000U;
constexpr std::uint64_t kHashProgressEnd = 150U;
constexpr std::uint64_t kDecodeProgressEnd = 750U;
constexpr std::size_t kHopFrames = 441U; // 10 ms at the canonical rate.
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr char kAnalyzerVersion[] = "pvt-adaptive-spectral-audio-3";

bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool is_cancelled(const std::atomic_bool* cancel) {
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

class ProgressReporter {
public:
    ProgressReporter(const AudioProgressCallback& callback,
                     const std::atomic_bool* cancel,
                     std::string* error)
        : callback_(callback), cancel_(cancel), error_(error) {}

    bool report(std::uint64_t completed) {
        if (is_cancelled(cancel_)) {
            return fail(error_, "Audio analysis was cancelled.");
        }
        completed = (std::min)(completed, kProgressTotal);
        if (completed < last_) {
            completed = last_;
        }
        if (callback_ && completed != last_reported_) {
            try {
                if (!callback_(completed, kProgressTotal)) {
                    return fail(error_,
                                "Audio analysis was cancelled by the progress callback.");
                }
            } catch (const std::exception& exception) {
                return fail(error_, "The audio progress callback failed: "
                                        + std::string(exception.what()));
            } catch (...) {
                return fail(error_,
                            "The audio progress callback failed with an unknown exception.");
            }
            last_reported_ = completed;
        }
        last_ = completed;
        return true;
    }

private:
    const AudioProgressCallback& callback_;
    const std::atomic_bool* cancel_ = nullptr;
    std::string* error_ = nullptr;
    std::uint64_t last_ = 0U;
    std::uint64_t last_reported_ = (std::numeric_limits<std::uint64_t>::max)();
};

std::uint64_t scaled_progress(std::uint64_t completed,
                              std::uint64_t total,
                              std::uint64_t begin,
                              std::uint64_t end) {
    if (total == 0U || completed >= total) {
        return end;
    }
    const long double fraction = static_cast<long double>(completed)
                                 / static_cast<long double>(total);
    const long double span = static_cast<long double>(end - begin);
    return begin + static_cast<std::uint64_t>(fraction * span);
}

std::uint32_t rotate_right(std::uint32_t value, unsigned count) {
    return (value >> count) | (value << (32U - count));
}

class Sha256 {
public:
    Sha256()
        : state_ {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                  0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

    void update(const std::uint8_t* bytes, std::size_t count) {
        if (count > ((std::numeric_limits<std::uint64_t>::max)() - byte_count_)) {
            throw std::overflow_error("SHA-256 input length overflow");
        }
        byte_count_ += static_cast<std::uint64_t>(count);
        while (count > 0U) {
            const std::size_t copied =
                (std::min)(count, block_.size() - block_size_);
            std::memcpy(block_.data() + block_size_, bytes, copied);
            block_size_ += copied;
            bytes += copied;
            count -= copied;
            if (block_size_ == block_.size()) {
                transform(block_.data());
                block_size_ = 0U;
            }
        }
    }

    std::array<std::uint8_t, 32U> finish() {
        const std::uint64_t bit_count = byte_count_ * 8U;
        block_[block_size_++] = 0x80U;
        if (block_size_ > 56U) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_),
                      block_.end(), 0U);
            transform(block_.data());
            block_size_ = 0U;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_),
                  block_.begin() + 56, 0U);
        for (unsigned index = 0U; index < 8U; ++index) {
            block_[63U - index] =
                static_cast<std::uint8_t>(bit_count >> (index * 8U));
        }
        transform(block_.data());

        std::array<std::uint8_t, 32U> result {};
        for (std::size_t word = 0U; word < state_.size(); ++word) {
            for (unsigned byte = 0U; byte < 4U; ++byte) {
                result[word * 4U + byte] = static_cast<std::uint8_t>(
                    state_[word] >> ((3U - byte) * 8U));
            }
        }
        return result;
    }

private:
    void transform(const std::uint8_t* block) {
        static constexpr std::array<std::uint32_t, 64U> constants {
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
            0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
            0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
            0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
            0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

        std::array<std::uint32_t, 64U> words {};
        for (std::size_t index = 0U; index < 16U; ++index) {
            const std::size_t offset = index * 4U;
            words[index] = (static_cast<std::uint32_t>(block[offset]) << 24U)
                           | (static_cast<std::uint32_t>(block[offset + 1U]) << 16U)
                           | (static_cast<std::uint32_t>(block[offset + 2U]) << 8U)
                           | static_cast<std::uint32_t>(block[offset + 3U]);
        }
        for (std::size_t index = 16U; index < words.size(); ++index) {
            const std::uint32_t first = words[index - 15U];
            const std::uint32_t second = words[index - 2U];
            const std::uint32_t small0 = rotate_right(first, 7U)
                                         ^ rotate_right(first, 18U) ^ (first >> 3U);
            const std::uint32_t small1 = rotate_right(second, 17U)
                                         ^ rotate_right(second, 19U) ^ (second >> 10U);
            words[index] = words[index - 16U] + small0 + words[index - 7U] + small1;
        }

        std::uint32_t a = state_[0U];
        std::uint32_t b = state_[1U];
        std::uint32_t c = state_[2U];
        std::uint32_t d = state_[3U];
        std::uint32_t e = state_[4U];
        std::uint32_t f = state_[5U];
        std::uint32_t g = state_[6U];
        std::uint32_t h = state_[7U];

        for (std::size_t index = 0U; index < words.size(); ++index) {
            const std::uint32_t big1 = rotate_right(e, 6U)
                                       ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
            const std::uint32_t choose = (e & f) ^ ((~e) & g);
            const std::uint32_t temporary1 =
                h + big1 + choose + constants[index] + words[index];
            const std::uint32_t big0 = rotate_right(a, 2U)
                                       ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = big0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }

        state_[0U] += a;
        state_[1U] += b;
        state_[2U] += c;
        state_[3U] += d;
        state_[4U] += e;
        state_[5U] += f;
        state_[6U] += g;
        state_[7U] += h;
    }

    std::array<std::uint32_t, 8U> state_;
    std::array<std::uint8_t, 64U> block_ {};
    std::size_t block_size_ = 0U;
    std::uint64_t byte_count_ = 0U;
};

std::string to_hex(const std::array<std::uint8_t, 32U>& bytes) {
    static constexpr char alphabet[] = "0123456789abcdef";
    std::string result(bytes.size() * 2U, '0');
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        result[index * 2U] = alphabet[bytes[index] >> 4U];
        result[index * 2U + 1U] = alphabet[bytes[index] & 0x0fU];
    }
    return result;
}

char ascii_lower(char value) {
    if (value >= 'A' && value <= 'F') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

bool valid_sha256(std::string_view digest) {
    if (digest.size() != 64U) {
        return false;
    }
    return std::all_of(digest.begin(), digest.end(), [](char value) {
        return (value >= '0' && value <= '9')
               || (value >= 'a' && value <= 'f')
               || (value >= 'A' && value <= 'F');
    });
}

struct DigestResult {
    std::string sha256;
    std::uint64_t file_size = 0U;
    std::array<std::uint8_t, 16U> prefix {};
    std::size_t prefix_size = 0U;
};

bool music_path_is_reparse_point(const fs::path& path) {
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
           && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
#else
    (void)path;
    return false;
#endif
}

bool preflight_path(const std::string& path,
                    fs::path& native_path,
                    std::uint64_t& file_size,
                    std::string* error) {
    if (path.empty() || path.size() > 4096U
        || path.find('\0') != std::string::npos) {
        return fail(error, "The music source path is empty or invalid.");
    }
    native_path = detail::path_from_utf8(path);
    std::error_code status_error;
    const fs::file_status status = fs::symlink_status(native_path, status_error);
    if (status_error || fs::is_symlink(status)
        || music_path_is_reparse_point(native_path)
        || !fs::is_regular_file(status)) {
        return fail(error,
                    "The music source must be a readable regular file, not a link or special file.");
    }
    const std::uintmax_t size = fs::file_size(native_path, status_error);
    if (status_error || size == 0U || size > kMaximumSourceBytes) {
        return fail(error,
                    "The music source is empty, unreadable, or exceeds the portable 512 MiB attachment limit.");
    }
    file_size = static_cast<std::uint64_t>(size);
    return true;
}

bool digest_file(const fs::path& path,
                 std::uint64_t file_size,
                 std::uint64_t progress_begin,
                 std::uint64_t progress_end,
                 ProgressReporter& progress,
                 DigestResult& result,
                 std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return fail(error, "Could not open the music source for hashing.");
    }
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(kHashChunkBytes));
    Sha256 hash;
    std::uint64_t completed = 0U;
    while (completed < file_size) {
        const std::uint64_t remaining = file_size - completed;
        const std::size_t requested = static_cast<std::size_t>(
            (std::min)(remaining, kHashChunkBytes));
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(requested));
        const std::streamsize extracted = input.gcount();
        if (extracted <= 0) {
            return fail(error, "The music source ended while it was being hashed.");
        }
        const std::size_t count = static_cast<std::size_t>(extracted);
        if (completed == 0U) {
            result.prefix_size = (std::min)(count, result.prefix.size());
            std::copy_n(buffer.begin(), result.prefix_size, result.prefix.begin());
        }
        hash.update(buffer.data(), count);
        completed += static_cast<std::uint64_t>(count);
        if (!progress.report(scaled_progress(completed, file_size,
                                             progress_begin, progress_end))) {
            return false;
        }
    }
    if (input.bad()) {
        return fail(error, "The music source could not be read completely.");
    }
    result.sha256 = to_hex(hash.finish());
    result.file_size = file_size;
    return true;
}

bool bytes_equal(const std::array<std::uint8_t, 16U>& prefix,
                 std::size_t offset,
                 const char* text,
                 std::size_t count) {
    return offset <= prefix.size() && count <= prefix.size() - offset
           && std::memcmp(prefix.data() + offset, text, count) == 0;
}

std::string source_format(const DigestResult& digest) {
    if (digest.prefix_size >= 12U
        && (bytes_equal(digest.prefix, 0U, "RIFF", 4U)
            || bytes_equal(digest.prefix, 0U, "RF64", 4U))
        && bytes_equal(digest.prefix, 8U, "WAVE", 4U)) {
        return "WAV";
    }
    if (digest.prefix_size >= 4U && bytes_equal(digest.prefix, 0U, "fLaC", 4U)) {
        return "FLAC";
    }
    if ((digest.prefix_size >= 3U && bytes_equal(digest.prefix, 0U, "ID3", 3U))
        || (digest.prefix_size >= 2U && digest.prefix[0U] == 0xffU
            && (digest.prefix[1U] & 0xe0U) == 0xe0U)) {
        return "MP3";
    }
    return {};
}

std::uint16_t load_u16_le(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(bytes[0U])
           | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1U]) << 8U);
}

std::uint32_t load_u32_le(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0U])
           | (static_cast<std::uint32_t>(bytes[1U]) << 8U)
           | (static_cast<std::uint32_t>(bytes[2U]) << 16U)
           | (static_cast<std::uint32_t>(bytes[3U]) << 24U);
}

struct WavePreflight {
    bool wave = false;
    std::uint64_t frame_count = 0U;
};

bool preflight_wave(const fs::path& path,
                    std::uint64_t file_size,
                    const std::string& format,
                    WavePreflight& result,
                    std::string* error) {
    if (format != "WAV") {
        return true;
    }
    result.wave = true;
    std::ifstream input(path, std::ios::binary);
    std::array<std::uint8_t, 12U> header {};
    input.read(reinterpret_cast<char*>(header.data()),
               static_cast<std::streamsize>(header.size()));
    if (input.gcount() != static_cast<std::streamsize>(header.size())) {
        return fail(error, "The WAV header is truncated.");
    }
    if (std::memcmp(header.data(), "RF64", 4U) == 0) {
        return fail(error,
                    "RF64 music sources are not yet supported; use WAV, FLAC, or MP3 below 4 GiB.");
    }
    const std::uint64_t riff_end = static_cast<std::uint64_t>(load_u32_le(header.data() + 4U))
                                   + 8U;
    if (riff_end < 12U || riff_end > file_size) {
        return fail(error, "The WAV RIFF length exceeds the source file.");
    }

    bool found_format = false;
    bool found_data = false;
    std::uint16_t block_align = 0U;
    std::uint64_t offset = 12U;
    std::uint64_t data_bytes = 0U;
    std::size_t chunks = 0U;
    while (offset + 8U <= riff_end) {
        if (++chunks > 65536U) {
            return fail(error, "The WAV source contains too many chunks.");
        }
        input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        std::array<std::uint8_t, 8U> chunk {};
        input.read(reinterpret_cast<char*>(chunk.data()),
                   static_cast<std::streamsize>(chunk.size()));
        if (input.gcount() != static_cast<std::streamsize>(chunk.size())) {
            return fail(error, "A WAV chunk header is truncated.");
        }
        const std::uint64_t chunk_bytes = load_u32_le(chunk.data() + 4U);
        const std::uint64_t data_offset = offset + 8U;
        if (data_offset > riff_end || chunk_bytes > riff_end - data_offset) {
            return fail(error, "A WAV chunk extends beyond the RIFF container.");
        }
        if (std::memcmp(chunk.data(), "fmt ", 4U) == 0) {
            if (found_format || chunk_bytes < 16U) {
                return fail(error, "The WAV format chunk is missing or malformed.");
            }
            std::array<std::uint8_t, 16U> format_bytes {};
            input.read(reinterpret_cast<char*>(format_bytes.data()),
                       static_cast<std::streamsize>(format_bytes.size()));
            if (input.gcount() != static_cast<std::streamsize>(format_bytes.size())) {
                return fail(error, "The WAV format chunk is truncated.");
            }
            const std::uint16_t encoding = load_u16_le(format_bytes.data());
            block_align = load_u16_le(format_bytes.data() + 12U);
            if ((encoding != 1U && encoding != 3U && encoding != 0xfffeU)
                || block_align == 0U) {
                return fail(error, "The WAV encoding or block alignment is unsupported.");
            }
            found_format = true;
        } else if (std::memcmp(chunk.data(), "data", 4U) == 0) {
            if (data_bytes > (std::numeric_limits<std::uint64_t>::max)() - chunk_bytes) {
                return fail(error, "The WAV data length overflows its supported range.");
            }
            data_bytes += chunk_bytes;
            found_data = true;
        }
        const std::uint64_t padded = chunk_bytes + (chunk_bytes & 1U);
        if (padded > riff_end - data_offset) {
            return fail(error, "The final WAV chunk or its padding is truncated.");
        }
        offset = data_offset + padded;
    }
    if (!found_format || !found_data || data_bytes == 0U || block_align == 0U
        || data_bytes % block_align != 0U) {
        return fail(error, "The WAV source has invalid or incomplete PCM data.");
    }
    if (offset != riff_end) {
        return fail(error, "The WAV source has a truncated trailing chunk header.");
    }
    result.frame_count = data_bytes / block_align;
    return true;
}

struct DecoderGuard {
    ma_decoder value {};
    bool initialized = false;

    ~DecoderGuard() {
        if (initialized) {
            static_cast<void>(ma_decoder_uninit(&value));
        }
    }
};

std::string decoder_error(const char* action, ma_result result) {
    const char* description = ma_result_description(result);
    return std::string(action) + ": "
           + (description != nullptr ? description : "unknown decoder error") + ".";
}

struct HopRecord {
    float energy = 0.0F;
    float bass = 0.0F;
    float midrange = 0.0F;
    float treble = 0.0F;
    float onset = 0.0F;
    float energy_onset = 0.0F;
    float spectral_onset = 0.0F;
    float spectral_centroid = 0.0F;
    float spectral_flatness = 0.0F;
    float chroma_hue = 0.0F;
    float chroma_strength = 0.0F;
    std::uint16_t peak_offset = 0U;
};

struct TempoObservation {
    double time_seconds = 0.0;
    double bpm = 0.0;
};

// BTT is intentionally used as one local observer, not as a whole-song BPM
// oracle. Its causal beat callbacks and quarter-second tempo snapshots are
// reconciled with the independent offline spectral-flux analysis below. This
// lets sudden/ramped tempo changes survive even when either tracker lags.
class AdaptiveBeatObserver {
public:
    AdaptiveBeatObserver() {
        beat_times_.reserve(kMaximumObserverBeats);
        tempo_.reserve(kMaximumObserverTempoPoints);
        tracker_ = btt_new_default();
        if (tracker_ == nullptr) {
            failed_ = true;
            return;
        }
        btt_set_min_tempo(tracker_, 55.0);
        btt_set_max_tempo(tracker_, 210.0);
        btt_set_count_in_n(tracker_, 1);
        // A roughly one-second history follows expressive changes without
        // allowing a single onset to flip the pulse grid.
        btt_set_gaussian_tempo_histogram_decay(tracker_, 0.997);
        btt_set_spectral_compression_gamma(tracker_, 10.0);
        btt_set_onset_tracking_callback(tracker_, nullptr, nullptr);
        btt_set_beat_tracking_callback(tracker_, &beat_callback, this);
    }

    AdaptiveBeatObserver(const AdaptiveBeatObserver&) = delete;
    AdaptiveBeatObserver& operator=(const AdaptiveBeatObserver&) = delete;

    ~AdaptiveBeatObserver() {
        if (tracker_ != nullptr) {
            tracker_ = btt_destroy(tracker_);
        }
    }

    bool valid() const { return tracker_ != nullptr && !failed_; }

    void push(float sample) {
        if (tracker_ == nullptr || failed_) {
            return;
        }
        buffer_[buffer_size_++] = sample;
        if (buffer_size_ == buffer_.size()) {
            process_buffer();
        }
    }

    bool finish() {
        if (tracker_ == nullptr || failed_) {
            return false;
        }
        if (buffer_size_ != 0U) {
            btt_process(tracker_, buffer_.data(), static_cast<int>(buffer_size_));
            samples_processed_ += buffer_size_;
            buffer_size_ = 0U;
            observe_tempo();
        }
        return !failed_;
    }

    const std::vector<double>& beat_times() const { return beat_times_; }
    const std::vector<TempoObservation>& tempo() const { return tempo_; }

private:
    static constexpr std::size_t kObserverBufferFrames = 512U;
    static constexpr std::size_t kMaximumObserverBeats = 30000U;
    static constexpr std::size_t kMaximumObserverTempoPoints = 30000U;
    static constexpr std::uint64_t kTempoObservationFrames = 11025U;

    static void beat_callback(void* context,
                              unsigned long long sample_time) noexcept {
        auto* observer = static_cast<AdaptiveBeatObserver*>(context);
        if (observer == nullptr || observer->failed_
            || observer->beat_times_.size() == kMaximumObserverBeats) {
            if (observer != nullptr
                && observer->beat_times_.size() == kMaximumObserverBeats) {
                observer->failed_ = true;
            }
            return;
        }
        try {
            const double time = static_cast<double>(sample_time)
                                / static_cast<double>(kAnalysisSampleRate);
            if (time >= 0.0
                && (observer->beat_times_.empty()
                    || time > observer->beat_times_.back())) {
                observer->beat_times_.push_back(time);
            }
        } catch (...) {
            // Never propagate a C++ exception through the C callback boundary.
            observer->failed_ = true;
        }
    }

    void process_buffer() {
        btt_process(tracker_, buffer_.data(),
                    static_cast<int>(buffer_.size()));
        samples_processed_ += buffer_.size();
        buffer_size_ = 0U;
        observe_tempo();
    }

    void observe_tempo() {
        while (samples_processed_ >= next_tempo_observation_) {
            if (tempo_.size() == kMaximumObserverTempoPoints) {
                failed_ = true;
                return;
            }
            const double bpm = btt_get_tempo_bpm(tracker_);
            if (std::isfinite(bpm) && bpm >= 55.0 && bpm <= 210.0) {
                tempo_.push_back(TempoObservation {
                    static_cast<double>(next_tempo_observation_)
                        / static_cast<double>(kAnalysisSampleRate),
                    bpm});
            }
            next_tempo_observation_ += kTempoObservationFrames;
        }
    }

    BTT* tracker_ = nullptr;
    std::array<dft_sample_t, kObserverBufferFrames> buffer_ {};
    std::size_t buffer_size_ = 0U;
    std::uint64_t samples_processed_ = 0U;
    std::uint64_t next_tempo_observation_ = kTempoObservationFrames;
    std::vector<double> beat_times_;
    std::vector<TempoObservation> tempo_;
    bool failed_ = false;
};

class HopAccumulator {
public:
    static constexpr std::size_t kSpectrumFrames = 2048U;
    static constexpr std::uint64_t spectrum_latency_frames() {
        // Flux is emitted on the first 10 ms analysis hop after an attack.
        return kHopFrames;
    }

    explicit HopAccumulator(AdaptiveBeatObserver& beat_observer)
        : low_pole_(std::exp(-2.0 * kPi * 250.0
                             / static_cast<double>(kAnalysisSampleRate))),
          mid_pole_(std::exp(-2.0 * kPi * 4000.0
                             / static_cast<double>(kAnalysisSampleRate))),
          beat_observer_(beat_observer) {}

    void push(float input) {
        beat_observer_.push(input);
        const double sample = static_cast<double>(input);
        low_state_ = low_pole_ * low_state_ + (1.0 - low_pole_) * sample;
        mid_state_ = mid_pole_ * mid_state_ + (1.0 - mid_pole_) * sample;
        const double bass = low_state_;
        const double midrange = mid_state_ - low_state_;
        const double treble = sample - mid_state_;
        energy_sum_ += sample * sample;
        bass_sum_ += bass * bass;
        mid_sum_ += midrange * midrange;
        treble_sum_ += treble * treble;
        const double magnitude = std::abs(sample);
        if (magnitude > peak_magnitude_) {
            peak_magnitude_ = magnitude;
            peak_offset_ = count_;
        }
        spectral_ring_[spectral_ring_index_] = input;
        spectral_ring_index_ = (spectral_ring_index_ + 1U) % spectral_ring_.size();
        spectral_samples_ = (std::min)(spectral_samples_ + 1U,
                                      spectral_ring_.size());
        ++count_;
        ++canonical_frames_;
        if (count_ == kHopFrames) {
            finish_hop();
        }
    }

    void finish() {
        if (count_ != 0U) {
            finish_hop();
        }
    }

    const std::vector<HopRecord>& records() const { return records_; }
    std::uint64_t canonical_frames() const { return canonical_frames_; }

private:
    static void fft(std::array<std::complex<double>, kSpectrumFrames>& values) {
        for (std::size_t index = 1U, reversed = 0U;
             index < values.size(); ++index) {
            std::size_t bit = values.size() >> 1U;
            while ((reversed & bit) != 0U) {
                reversed ^= bit;
                bit >>= 1U;
            }
            reversed ^= bit;
            if (index < reversed) {
                std::swap(values[index], values[reversed]);
            }
        }
        for (std::size_t length = 2U; length <= values.size(); length <<= 1U) {
            const double angle = -2.0 * kPi / static_cast<double>(length);
            const std::complex<double> step(std::cos(angle), std::sin(angle));
            for (std::size_t begin = 0U; begin < values.size(); begin += length) {
                std::complex<double> weight(1.0, 0.0);
                for (std::size_t offset = 0U; offset < length / 2U; ++offset) {
                    const std::complex<double> even = values[begin + offset];
                    const std::complex<double> odd =
                        values[begin + offset + length / 2U] * weight;
                    values[begin + offset] = even + odd;
                    values[begin + offset + length / 2U] = even - odd;
                    weight *= step;
                }
            }
        }
    }

    void spectral_features(HopRecord& record) {
        std::fill(spectrum_work_.begin(), spectrum_work_.end(),
                  std::complex<double>(0.0, 0.0));
        const std::size_t missing = spectral_ring_.size() - spectral_samples_;
        for (std::size_t index = missing; index < spectral_ring_.size(); ++index) {
            const std::size_t age = spectral_ring_.size() - 1U - index;
            const std::size_t ring =
                (spectral_ring_index_ + spectral_ring_.size() - 1U - age)
                % spectral_ring_.size();
            const double window = 0.5 - 0.5 * std::cos(
                2.0 * kPi * static_cast<double>(index)
                / static_cast<double>(spectral_ring_.size() - 1U));
            spectrum_work_[index] =
                std::complex<double>(static_cast<double>(spectral_ring_[ring])
                                         * window,
                                     0.0);
        }
        fft(spectrum_work_);

        constexpr std::size_t bin_count = kSpectrumFrames / 2U + 1U;
        double magnitude_sum = 0.0;
        double centroid_sum = 0.0;
        double log_power_sum = 0.0;
        double power_sum = 0.0;
        double flux = 0.0;
        std::array<double, 12U> chroma {};
        std::size_t flatness_bins = 0U;
        for (std::size_t bin = 1U; bin < bin_count; ++bin) {
            const double frequency = static_cast<double>(bin)
                                     * static_cast<double>(kAnalysisSampleRate)
                                     / static_cast<double>(kSpectrumFrames);
            if (frequency > 20000.0) {
                break;
            }
            const double power = std::norm(spectrum_work_[bin]);
            const double magnitude = std::sqrt(power);
            magnitude_sum += magnitude;
            centroid_sum += frequency * magnitude;
            power_sum += power;
            log_power_sum += std::log(power + 1.0e-20);
            ++flatness_bins;

            const double compressed = std::log1p(10.0 * magnitude);
            flux += (std::max)(0.0, compressed - previous_spectrum_[bin]);
            previous_spectrum_[bin] = compressed;

            if (frequency >= 55.0 && frequency <= 5000.0) {
                const double midi = 69.0 + 12.0 * std::log2(frequency / 440.0);
                const double nearest = std::round(midi);
                int pitch_class = static_cast<int>(nearest) % 12;
                if (pitch_class < 0) {
                    pitch_class += 12;
                }
                const double tuning_weight = std::exp(
                    -2.0 * std::pow(midi - nearest, 2.0));
                // Magnitude compression keeps upper harmonics and a loud bass
                // fundamental from overwhelming the pitch-class color.
                chroma[static_cast<std::size_t>(pitch_class)] +=
                    std::sqrt(magnitude) * tuning_weight;
            }
        }
        record.spectral_centroid = magnitude_sum > 1.0e-12
                                       ? static_cast<float>(centroid_sum
                                                            / magnitude_sum
                                                            / 20000.0)
                                       : 0.0F;
        if (power_sum > 1.0e-20 && flatness_bins != 0U) {
            const double geometric = std::exp(
                log_power_sum / static_cast<double>(flatness_bins));
            const double arithmetic = power_sum
                                      / static_cast<double>(flatness_bins);
            record.spectral_flatness = static_cast<float>(
                (std::min)(1.0, geometric / arithmetic));
        }
        record.spectral_onset = static_cast<float>(flux);

        const double chroma_sum = std::accumulate(chroma.begin(), chroma.end(), 0.0);
        if (chroma_sum > 1.0e-12) {
            const auto strongest = std::max_element(chroma.begin(), chroma.end());
            const std::size_t pitch = static_cast<std::size_t>(
                std::distance(chroma.begin(), strongest));
            const std::size_t previous = (pitch + 11U) % 12U;
            const std::size_t next = (pitch + 1U) % 12U;
            const double neighbor_total = chroma[previous] + chroma[next];
            const double direction = neighbor_total > 1.0e-12
                                         ? 0.5 * (chroma[next] - chroma[previous])
                                               / neighbor_total
                                         : 0.0;
            double hue = (static_cast<double>(pitch) + direction) / 12.0;
            hue -= std::floor(hue);
            const double mean = chroma_sum / 12.0;
            const double concentration = (*strongest - mean)
                                         / (chroma_sum - mean + 1.0e-12);
            record.chroma_hue = static_cast<float>(hue);
            record.chroma_strength = static_cast<float>(std::clamp(
                concentration * std::sqrt((std::max)(
                                    0.0, 1.0 - record.spectral_flatness)),
                0.0, 1.0));
        }
    }

    void finish_hop() {
        const double divisor = static_cast<double>(count_);
        HopRecord record;
        record.energy = static_cast<float>(std::sqrt(energy_sum_ / divisor));
        record.bass = static_cast<float>(std::sqrt(bass_sum_ / divisor));
        record.midrange = static_cast<float>(std::sqrt(mid_sum_ / divisor));
        record.treble = static_cast<float>(std::sqrt(treble_sum_ / divisor));
        spectral_features(record);
        const double current_log = std::log1p(24.0 * static_cast<double>(record.energy));
        const double energy_onset = (std::max)(0.0, current_log - previous_log_);
        // Multiband spectral flux finds note attacks that barely change RMS;
        // energy flux retains precise response to broad-band clicks.
        record.energy_onset = static_cast<float>(energy_onset);
        record.onset = static_cast<float>(
            std::log1p(static_cast<double>(record.spectral_onset))
            + 2.0 * energy_onset);
        record.peak_offset = static_cast<std::uint16_t>(peak_offset_);
        records_.push_back(record);
        previous_log_ = current_log;
        energy_sum_ = 0.0;
        bass_sum_ = 0.0;
        mid_sum_ = 0.0;
        treble_sum_ = 0.0;
        peak_magnitude_ = 0.0;
        peak_offset_ = 0U;
        count_ = 0U;
    }

    const double low_pole_;
    const double mid_pole_;
    AdaptiveBeatObserver& beat_observer_;
    double low_state_ = 0.0;
    double mid_state_ = 0.0;
    double energy_sum_ = 0.0;
    double bass_sum_ = 0.0;
    double mid_sum_ = 0.0;
    double treble_sum_ = 0.0;
    double peak_magnitude_ = 0.0;
    double previous_log_ = 0.0;
    std::array<float, kSpectrumFrames> spectral_ring_ {};
    std::array<double, kSpectrumFrames / 2U + 1U> previous_spectrum_ {};
    std::array<std::complex<double>, kSpectrumFrames> spectrum_work_ {};
    std::size_t spectral_ring_index_ = 0U;
    std::size_t spectral_samples_ = 0U;
    std::size_t peak_offset_ = 0U;
    std::size_t count_ = 0U;
    std::uint64_t canonical_frames_ = 0U;
    std::vector<HopRecord> records_;
};

class LinearResampler {
public:
    LinearResampler(std::uint32_t source_rate, HopAccumulator& output)
        : source_rate_(source_rate), output_(output) {}

    void push(float sample) {
        if (!have_previous_) {
            previous_ = sample;
            have_previous_ = true;
            output_.push(sample);
            output_index_ = 1U;
            return;
        }
        const std::uint64_t current_index = source_index_ + 1U;
        const std::uint64_t current_numerator = current_index * kAnalysisSampleRate;
        while (output_index_ * source_rate_ <= current_numerator) {
            const std::uint64_t position = output_index_ * source_rate_;
            const std::uint64_t base = source_index_ * kAnalysisSampleRate;
            const double fraction = static_cast<double>(position - base)
                                    / static_cast<double>(kAnalysisSampleRate);
            const double interpolated = static_cast<double>(previous_)
                                        + (static_cast<double>(sample)
                                           - static_cast<double>(previous_)) * fraction;
            output_.push(static_cast<float>(interpolated));
            ++output_index_;
        }
        previous_ = sample;
        source_index_ = current_index;
    }

    void finish(std::uint64_t source_frames) {
        if (!have_previous_ || source_frames == 0U) {
            return;
        }
        const std::uint64_t duration_numerator = source_frames * kAnalysisSampleRate;
        while (output_index_ * source_rate_ < duration_numerator) {
            output_.push(previous_);
            ++output_index_;
        }
    }

private:
    const std::uint64_t source_rate_;
    HopAccumulator& output_;
    bool have_previous_ = false;
    float previous_ = 0.0F;
    std::uint64_t source_index_ = 0U;
    std::uint64_t output_index_ = 0U;
};

double percentile(std::vector<float> values, double proportion) {
    values.erase(std::remove_if(values.begin(), values.end(), [](float value) {
                     return !(value > 0.0F) || !std::isfinite(value);
                 }),
                 values.end());
    if (values.empty()) {
        return 1.0;
    }
    std::sort(values.begin(), values.end());
    const double position = proportion * static_cast<double>(values.size() - 1U);
    const std::size_t index = static_cast<std::size_t>(position);
    return (std::max)(static_cast<double>(values[index]), 1.0e-12);
}

float quantized_unit(double value) {
    const double clamped = (std::max)(0.0, (std::min)(1.0, value));
    constexpr double scale = 65535.0;
    return static_cast<float>(std::round(clamped * scale) / scale);
}

double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    const std::size_t middle = values.size() / 2U;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle),
                     values.end());
    double result = values[middle];
    if (values.size() % 2U == 0U) {
        const auto lower = std::max_element(
            values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle));
        result = (*lower + result) * 0.5;
    }
    return result;
}

struct BeatCandidate {
    std::size_t hop = 0U;
    float strength = 0.0F;
    double time_seconds = 0.0;
    bool observer_only = false;
};

constexpr std::size_t kTempoStepHops = 100U; // One-second local tempo nodes.
constexpr std::size_t kTempoRadiusHops = 300U; // Six-second centered evidence.
constexpr std::size_t kMinimumTempoPeriodHops = 30U; // 200 BPM.
constexpr std::size_t kMaximumTempoPeriodHops = 109U; // About 55 BPM.

struct TempoTrack {
    std::vector<double> periods;
    std::vector<double> confidence;
};

double observer_bpm_near(const std::vector<TempoObservation>& observations,
                         double center_seconds) {
    std::vector<double> local;
    for (const TempoObservation& observation : observations) {
        if (std::abs(observation.time_seconds - center_seconds) <= 3.0) {
            local.push_back(observation.bpm);
        }
    }
    if (local.empty() && !observations.empty()) {
        const auto nearest = std::min_element(
            observations.begin(), observations.end(),
            [center_seconds](const TempoObservation& first,
                             const TempoObservation& second) {
                return std::abs(first.time_seconds - center_seconds)
                       < std::abs(second.time_seconds - center_seconds);
            });
        if (std::abs(nearest->time_seconds - center_seconds) <= 8.0) {
            local.push_back(nearest->bpm);
        }
    }
    return median(std::move(local));
}

TempoTrack estimate_tempo_periods(
    const std::vector<BeatCandidate>& candidates,
    std::size_t record_count,
    double onset_scale,
    const std::vector<TempoObservation>& observer_tempo) {
    const std::size_t window_count =
        (record_count + kTempoStepHops - 1U) / kTempoStepHops;
    TempoTrack track;
    track.periods.assign(window_count, 50.0); // Used only if no evidence exists.
    track.confidence.assign(window_count, 0.0);
    std::vector<bool> measured(window_count, false);
    for (std::size_t window = 0U; window < window_count; ++window) {
        const std::size_t center = (std::min)(
            record_count - 1U, window * kTempoStepHops + kTempoStepHops / 2U);
        const std::size_t hop_begin = center > kTempoRadiusHops
                                          ? center - kTempoRadiusHops
                                          : 0U;
        const std::size_t hop_end =
            (std::min)(record_count, center + kTempoRadiusHops + 1U);
        const auto first = std::lower_bound(
            candidates.begin(), candidates.end(), hop_begin,
            [](const BeatCandidate& candidate, std::size_t hop) {
                return candidate.hop < hop;
            });
        const auto last = std::lower_bound(
            first, candidates.end(), hop_end,
            [](const BeatCandidate& candidate, std::size_t hop) {
                return candidate.hop < hop;
            });
        const std::size_t candidate_begin = static_cast<std::size_t>(
            std::distance(candidates.begin(), first));
        const std::size_t candidate_end = static_cast<std::size_t>(
            std::distance(candidates.begin(), last));
        const double observer_bpm = observer_bpm_near(
            observer_tempo,
            static_cast<double>(center * kHopFrames)
                / static_cast<double>(kAnalysisSampleRate));

        std::array<double, kMaximumTempoPeriodHops + 1U> raw_scores {};
        double raw_best = 0.0;
        std::size_t raw_best_period = 50U;
        std::size_t best_period = 50U;
        for (std::size_t period = kMinimumTempoPeriodHops;
             period <= kMaximumTempoPeriodHops; ++period) {
            const std::size_t tolerance = (std::max)(
                std::size_t {2}, static_cast<std::size_t>(std::llround(
                                      static_cast<double>(period) * 0.08)));
            std::vector<double> phase_strength(period, 0.0);
            std::vector<double> phase_hits(period, 0.0);
            for (std::size_t index = candidate_begin; index < candidate_end;
                 ++index) {
                const double strength = (std::min)(
                    1.5, static_cast<double>(candidates[index].strength)
                             / onset_scale);
                const std::size_t phase =
                    (candidates[index].hop - hop_begin) % period;
                for (std::int64_t offset = -static_cast<std::int64_t>(tolerance);
                     offset <= static_cast<std::int64_t>(tolerance); ++offset) {
                    std::int64_t wrapped = static_cast<std::int64_t>(phase) + offset;
                    if (wrapped < 0) {
                        wrapped += static_cast<std::int64_t>(period);
                    } else if (wrapped >= static_cast<std::int64_t>(period)) {
                        wrapped -= static_cast<std::int64_t>(period);
                    }
                    const double proximity =
                        1.0 - static_cast<double>(std::abs(offset))
                                  / static_cast<double>(tolerance + 1U);
                    const std::size_t bin = static_cast<std::size_t>(wrapped);
                    phase_strength[bin] += strength * proximity;
                    phase_hits[bin] += proximity;
                }
            }
            const double expected = (std::max)(
                1.0, static_cast<double>(hop_end - hop_begin)
                         / static_cast<double>(period));
            double grid_score = 0.0;
            for (std::size_t phase = 0U; phase < period; ++phase) {
                const double coverage = (std::min)(1.0,
                    phase_hits[phase] / expected);
                grid_score = (std::max)(
                    grid_score,
                    phase_strength[phase] / expected
                        * (0.65 + 0.35 * coverage));
            }

            double adjacent_support = 0.0;
            for (std::size_t index = candidate_begin + 1U;
                 index < candidate_end; ++index) {
                const double gap = static_cast<double>(
                    candidates[index].hop - candidates[index - 1U].hop);
                const double relative_error =
                    std::abs(gap - static_cast<double>(period))
                    / static_cast<double>(period);
                if (relative_error > 0.25) {
                    continue;
                }
                const double strength = std::sqrt(
                    static_cast<double>(candidates[index].strength)
                    * candidates[index - 1U].strength) / onset_scale;
                adjacent_support += (std::min)(1.5, strength)
                                    * (1.0 - relative_error / 0.25);
            }
            raw_scores[period] = grid_score
                                 + 1.00 * adjacent_support / expected;
            if (raw_scores[period] > raw_best) {
                raw_best = raw_scores[period];
                raw_best_period = period;
            }
        }

        double best_score = 0.0;
        double second_score = 0.0;
        double observer_bonus = 0.0;
        double observer_period = 0.0;
        if (observer_bpm > 0.0 && raw_best > 0.0) {
            observer_period = 6000.0 / observer_bpm;
            const double ratio = static_cast<double>(raw_best_period)
                                 / observer_period;
            const double octave_distance = std::abs(
                std::abs(std::log2(ratio)) - 1.0);
            // The causal observer is most valuable for the classic half/double
            // ambiguity. It must not flatten a genuine 120 -> 180 change just
            // because its decaying history has not caught up yet.
            if (octave_distance <= 0.12) {
                observer_bonus = 0.75;
            } else if (std::abs(std::log2(ratio)) <= 0.08) {
                observer_bonus = 0.10;
            }
        }
        for (std::size_t period = kMinimumTempoPeriodHops;
             period <= kMaximumTempoPeriodHops; ++period) {
            double score = raw_scores[period];
            if (observer_bonus > 0.0) {
                const double octaves = std::log2(
                    static_cast<double>(period) / observer_period);
                const double support = std::exp(-0.5 * std::pow(octaves / 0.10, 2.0));
                score += raw_best * observer_bonus * support;
            }
            if (score > best_score) {
                second_score = best_score;
                best_score = score;
                best_period = period;
            } else if (score > second_score) {
                second_score = score;
            }
        }
        if (best_score > 1.0e-9) {
            std::vector<double> implied_periods;
            for (std::size_t index = candidate_begin + 1U;
                 index < candidate_end; ++index) {
                for (std::size_t prior = index; prior > candidate_begin; --prior) {
                    const std::size_t predecessor = prior - 1U;
                    const double gap =
                        (candidates[index].time_seconds
                         - candidates[predecessor].time_seconds)
                        * static_cast<double>(kAnalysisSampleRate)
                        / static_cast<double>(kHopFrames);
                    if (gap > 4.5 * static_cast<double>(best_period)) {
                        break;
                    }
                    const double pulses = std::round(
                        gap / static_cast<double>(best_period));
                    if (pulses >= 1.0 && pulses <= 4.0
                        && std::abs(gap - pulses * static_cast<double>(best_period))
                               / static_cast<double>(best_period) <= 0.18) {
                        implied_periods.push_back(gap / pulses);
                    }
                }
            }
            track.periods[window] = implied_periods.empty()
                                        ? static_cast<double>(best_period)
                                        : median(std::move(implied_periods));
            const double separation = best_score > 0.0
                                          ? (best_score - second_score) / best_score
                                          : 0.0;
            const double evidence = (std::min)(
                1.0, static_cast<double>(candidate_end - candidate_begin) / 8.0);
            track.confidence[window] = std::clamp(
                0.35 + 0.35 * evidence + 0.30 * separation, 0.0, 1.0);
            measured[window] = true;
        } else if (observer_bpm > 0.0) {
            track.periods[window] = 6000.0 / observer_bpm;
            track.confidence[window] = 0.35;
            measured[window] = true;
        }
    }
    for (std::size_t index = 1U; index < track.periods.size(); ++index) {
        if (!measured[index]) {
            track.periods[index] = track.periods[index - 1U];
            track.confidence[index] = track.confidence[index - 1U] * 0.8;
        }
    }
    for (std::size_t index = track.periods.size(); index > 1U; --index) {
        if (!measured[index - 2U]) {
            track.periods[index - 2U] = track.periods[index - 1U];
            track.confidence[index - 2U] = track.confidence[index - 1U] * 0.8;
        }
    }
    // Remove isolated one-second octave glitches, but retain sustained or
    // non-octave tempo movement (including ramps and abrupt changes).
    std::vector<double> cleaned = track.periods;
    for (std::size_t index = 1U; index + 1U < track.periods.size(); ++index) {
        const double neighbors = 0.5 * (track.periods[index - 1U]
                                        + track.periods[index + 1U]);
        const double neighbor_agreement = std::abs(track.periods[index - 1U]
                                                   - track.periods[index + 1U])
                                          / neighbors;
        const double deviation = std::abs(track.periods[index] - neighbors)
                                 / neighbors;
        if (neighbor_agreement < 0.08 && deviation > 0.35) {
            cleaned[index] = neighbors;
            track.confidence[index] *= 0.75;
        }
    }
    track.periods = std::move(cleaned);
    return track;
}

double period_for_hop(const TempoTrack& track, double hop) {
    if (track.periods.empty()) {
        return 50.0;
    }
    const double position = (std::max)(0.0, hop)
                            / static_cast<double>(kTempoStepHops);
    const std::size_t first = (std::min)(
        track.periods.size() - 1U, static_cast<std::size_t>(position));
    const std::size_t second = (std::min)(first + 1U,
                                          track.periods.size() - 1U);
    const double amount = position - std::floor(position);
    return track.periods[first]
           + (track.periods[second] - track.periods[first]) * amount;
}

bool select_beat_grid(const std::vector<BeatCandidate>& candidates,
                      const std::vector<double>& observer_beats,
                      const TempoTrack& tempo,
                      double duration_seconds,
                      double onset_scale,
                      std::vector<double>& beat_times,
                      std::string* error) {
    std::vector<BeatCandidate> evidence = candidates;
    for (double observer : observer_beats) {
        if (!(observer >= 0.0) || observer > duration_seconds
            || !std::isfinite(observer)) {
            continue;
        }
        const auto nearest = std::lower_bound(
            evidence.begin(), evidence.end(), observer,
            [](const BeatCandidate& candidate, double time) {
                return candidate.time_seconds < time;
            });
        bool matched = false;
        bool nearby_onset = false;
        for (int direction = -1; direction <= 0; ++direction) {
            if (direction < 0 && nearest == evidence.begin()) {
                continue;
            }
            auto item = nearest;
            if (direction < 0) {
                --item;
            }
            if (item != evidence.end()) {
                const double distance = std::abs(item->time_seconds - observer);
                nearby_onset = nearby_onset || distance <= 0.250;
                if (distance <= 0.060) {
                    item->strength += static_cast<float>(0.20 * onset_scale);
                    matched = true;
                }
            }
        }
        if (!matched && !nearby_onset) {
            const std::size_t hop = static_cast<std::size_t>(std::llround(
                observer * static_cast<double>(kAnalysisSampleRate)
                / static_cast<double>(kHopFrames)));
            evidence.push_back(BeatCandidate {
                hop, static_cast<float>(0.30 * onset_scale), observer, true});
        }
    }
    std::sort(evidence.begin(), evidence.end(),
              [](const BeatCandidate& first, const BeatCandidate& second) {
                  return first.time_seconds < second.time_seconds;
              });
    std::vector<BeatCandidate> merged;
    for (const BeatCandidate& candidate : evidence) {
        if (!std::isfinite(candidate.time_seconds)
            || candidate.time_seconds < 0.0
            || candidate.time_seconds > duration_seconds) {
            continue;
        }
        if (!merged.empty()
            && candidate.time_seconds - merged.back().time_seconds < 0.060) {
            if (candidate.strength > merged.back().strength) {
                merged.back() = candidate;
            }
        } else {
            merged.push_back(candidate);
        }
    }
    if (merged.empty()) {
        return true;
    }

    std::size_t first = 0U;
    while (first + 1U < merged.size()
           && merged[first].strength < 0.20 * onset_scale) {
        ++first;
    }
    double current = merged[first].time_seconds;
    beat_times.push_back(current);
    std::size_t evidence_index = first + 1U;
    while (evidence_index < merged.size()) {
        if (beat_times.size() == kMaximumMusicBeats) {
            return fail(error,
                        "The source contains more detected beats than this project format can store.");
        }
        const double current_hop = current
                                   * static_cast<double>(kAnalysisSampleRate)
                                   / static_cast<double>(kHopFrames);
        const double initial_period = period_for_hop(tempo, current_hop);
        const double future_period = period_for_hop(
            tempo, current_hop + initial_period);
        const double period_hops = 0.5 * (initial_period + future_period);
        const double period_seconds = period_hops
                                      * static_cast<double>(kHopFrames)
                                      / static_cast<double>(kAnalysisSampleRate);
        const double predicted = current + period_seconds;
        const double tolerance = 0.43 * period_seconds;

        while (evidence_index < merged.size()
               && merged[evidence_index].time_seconds
                      < current + 0.45 * period_seconds) {
            ++evidence_index;
        }
        std::size_t best = merged.size();
        double best_score = -1.0;
        for (std::size_t index = evidence_index; index < merged.size(); ++index) {
            const double distance = std::abs(
                merged[index].time_seconds - predicted);
            if (merged[index].time_seconds > predicted + tolerance) {
                break;
            }
            if (distance <= tolerance) {
                const double phase_score = 1.0 - distance / tolerance;
                const double strength = (std::min)(
                    1.5, static_cast<double>(merged[index].strength) / onset_scale);
                const double score = 2.0 * phase_score + 0.8 * strength;
                if (score > best_score) {
                    best = index;
                    best_score = score;
                }
            }
        }
        if (best != merged.size()) {
            if (merged[best].observer_only) {
                // A causal observer event has useful phase information but is
                // only hop-accurate. When a later acoustic onset confirms a
                // short gap, reconcile the missing event between the two real
                // anchors instead of freezing the observer's quantization.
                std::size_t anchor = merged.size();
                double anchor_steps = 0.0;
                for (std::size_t index = best + 1U; index < merged.size(); ++index) {
                    if (merged[index].observer_only) continue;
                    const double anchor_gap = merged[index].time_seconds - current;
                    const double estimated_steps = anchor_gap / period_seconds;
                    if (estimated_steps > 4.35) break;
                    const double rounded_steps = std::round(estimated_steps);
                    if (rounded_steps >= 2.0 && rounded_steps <= 4.0
                        && std::abs(estimated_steps - rounded_steps) <= 0.30) {
                        anchor = index;
                        anchor_steps = rounded_steps;
                        break;
                    }
                }
                if (anchor != merged.size()) {
                    current += (merged[anchor].time_seconds - current) / anchor_steps;
                } else {
                    current = merged[best].time_seconds;
                }
            } else {
                current = merged[best].time_seconds;
            }
            evidence_index = best + 1U;
            beat_times.push_back(current);
            continue;
        }

        if (evidence_index >= merged.size()) {
            break;
        }
        const double gap = merged[evidence_index].time_seconds - current;
        if (gap > 4.5 * period_seconds) {
            // Do not hallucinate a long grid through silence. Resume from the
            // next supported local segment instead.
            current = merged[evidence_index].time_seconds;
            ++evidence_index;
        } else {
            // Missing/soft beat between supported neighbors: use both anchors
            // to avoid accumulating the hop-quantization error of the local
            // tempogram. This remains adaptive because it only bridges a short
            // locally supported gap; it never imposes a song-wide BPM grid.
            std::size_t bridge = merged.size();
            double bridge_steps = 0.0;
            double bridge_score = -1.0;
            for (std::size_t index = evidence_index; index < merged.size(); ++index) {
                const double bridge_gap = merged[index].time_seconds - current;
                const double estimated_steps = bridge_gap / period_seconds;
                if (estimated_steps > 4.35) break;
                const double rounded_steps = std::round(estimated_steps);
                const double phase_error = std::abs(estimated_steps - rounded_steps);
                if (rounded_steps < 2.0 || rounded_steps > 4.0
                    || phase_error > 0.30) {
                    continue;
                }
                const double score = static_cast<double>(merged[index].strength)
                                     / onset_scale + 1.0 - phase_error;
                if (score > bridge_score) {
                    bridge = index;
                    bridge_steps = rounded_steps;
                    bridge_score = score;
                }
            }
            if (bridge != merged.size()) {
                current += (merged[bridge].time_seconds - current) / bridge_steps;
            } else {
                current = predicted;
            }
        }
        if (current > beat_times.back() && current <= duration_seconds) {
            beat_times.push_back(current);
        }
    }
    return true;
}

bool detect_beats(const std::vector<HopRecord>& records,
                  const AdaptiveBeatObserver& observer,
                  MusicAnalysis& analysis,
                  ProgressReporter& progress,
                  std::string* error) {
    if (records.size() < 2U) {
        return progress.report(880U);
    }
    std::vector<float> onsets;
    onsets.reserve(records.size());
    for (const HopRecord& record : records) {
        onsets.push_back(record.onset);
    }
    const double onset_scale = percentile(onsets, 0.99);
    if (onset_scale <= 1.0e-10) {
        return progress.report(880U);
    }

    std::vector<double> prefix(records.size() + 1U, 0.0);
    for (std::size_t index = 0U; index < records.size(); ++index) {
        prefix[index + 1U] = prefix[index] + static_cast<double>(records[index].onset);
    }

    constexpr std::size_t local_radius = 25U;
    constexpr std::size_t peak_radius = 2U;
    constexpr std::size_t minimum_gap = 12U;
    std::vector<BeatCandidate> candidates;
    for (std::size_t index = 0U; index < records.size(); ++index) {
        if ((index & 16383U) == 0U && !progress.report(800U
                + scaled_progress(index, records.size(), 0U, 40U))) {
            return false;
        }
        const std::size_t begin = index > local_radius ? index - local_radius : 0U;
        const std::size_t end = (std::min)(records.size(), index + local_radius + 1U);
        const double local_mean = (prefix[end] - prefix[begin])
                                  / static_cast<double>(end - begin);
        const double value = records[index].onset;
        const double threshold = (std::max)(onset_scale * 0.06, local_mean * 1.45);
        if (value < threshold) {
            continue;
        }
        const std::size_t peak_begin = index > peak_radius ? index - peak_radius : 0U;
        const std::size_t peak_end =
            (std::min)(records.size(), index + peak_radius + 1U);
        bool maximum = true;
        for (std::size_t other = peak_begin; other < peak_end; ++other) {
            if (other != index
                && (records[other].onset > records[index].onset
                    || (records[other].onset == records[index].onset && other < index))) {
                maximum = false;
                break;
            }
        }
        if (!maximum) {
            continue;
        }
        const double spectral_component = std::log1p(
            static_cast<double>(records[index].spectral_onset));
        const double energy_component =
            2.0 * static_cast<double>(records[index].energy_onset);
        std::uint64_t sample_index = 0U;
        if (energy_component >= 0.75 * spectral_component) {
            sample_index = static_cast<std::uint64_t>(index) * kHopFrames
                           + records[index].peak_offset;
        } else {
            // The causal STFT flux peaks when an attack reaches the middle of
            // its Hann window. Compensate that fixed look-back so stored beat
            // events describe the sound, not analyzer latency.
            const std::size_t search_begin = index > 3U ? index - 3U : 0U;
            std::size_t attack = search_begin;
            for (std::size_t nearby = search_begin; nearby <= index; ++nearby) {
                if (records[nearby].energy_onset
                    > records[attack].energy_onset) {
                    attack = nearby;
                }
            }
            if (records[attack].energy_onset > 1.0e-8F) {
                sample_index = static_cast<std::uint64_t>(attack) * kHopFrames
                               + records[attack].peak_offset;
            } else {
                const std::uint64_t frame_end =
                    static_cast<std::uint64_t>(index + 1U) * kHopFrames;
                sample_index = frame_end
                                       > HopAccumulator::spectrum_latency_frames()
                                   ? frame_end
                                         - HopAccumulator::spectrum_latency_frames()
                                   : 0U;
            }
        }
        BeatCandidate candidate {index, records[index].onset,
                                 static_cast<double>(sample_index)
                                     / static_cast<double>(kAnalysisSampleRate),
                                 false};
        if (!candidates.empty() && index - candidates.back().hop < minimum_gap) {
            if (candidate.strength > candidates.back().strength) {
                candidates.back() = candidate;
            }
        } else {
            candidates.push_back(candidate);
        }
    }

    const TempoTrack tempo = estimate_tempo_periods(
        candidates, records.size(), onset_scale, observer.tempo());
    if (!select_beat_grid(candidates, observer.beat_times(), tempo,
                          analysis.duration_seconds, onset_scale,
                          analysis.beat_times_seconds, error)) {
        return false;
    }

    // Padded FFT/observer state may report one extrapolated event just beyond
    // the final decoded sample. Never let analysis cache an anchor that the
    // project clock cannot represent. A beatless/ambient source still receives
    // a neutral start anchor so Music mode remains usable.
    std::vector<double> bounded_beats;
    bounded_beats.reserve(analysis.beat_times_seconds.size());
    for (const double beat : analysis.beat_times_seconds) {
        if (!std::isfinite(beat) || beat < 0.0
            || beat > analysis.duration_seconds) {
            continue;
        }
        if (bounded_beats.empty() || beat > bounded_beats.back()) {
            bounded_beats.push_back(beat);
        }
    }
    if (bounded_beats.empty()) bounded_beats.push_back(0.0);
    analysis.beat_times_seconds = std::move(bounded_beats);

    std::vector<double> intervals;
    intervals.reserve(analysis.beat_times_seconds.size());
    for (std::size_t index = 1U; index < analysis.beat_times_seconds.size(); ++index) {
        const double interval = analysis.beat_times_seconds[index]
                                - analysis.beat_times_seconds[index - 1U];
        if (interval >= 0.20 && interval <= 1.5) {
            intervals.push_back(interval);
        }
    }
    if (intervals.empty()) {
        return progress.report(880U);
    }
    const double typical_interval = median(intervals);
    analysis.detected_bpm = 60.0 / typical_interval;

    const double count_confidence =
        (std::min)(1.0, static_cast<double>(intervals.size()) / 12.0);
    const double track_confidence = median(tempo.confidence);
    analysis.tempo_confidence = quantized_unit(
        count_confidence * std::clamp(track_confidence, 0.0, 1.0));

    std::vector<MusicTempoPoint> local_points;
    double last_bpm = 0.0;
    for (std::size_t index = 0U; index < tempo.periods.size(); ++index) {
        const double bpm = 6000.0 / tempo.periods[index];
        const bool meaningful = last_bpm == 0.0
                                || std::abs(bpm - last_bpm) / last_bpm >= 0.04
                                || index + 1U == tempo.periods.size();
        if (!meaningful) {
            continue;
        }
        MusicTempoPoint point;
        point.time_seconds = (std::min)(
            analysis.duration_seconds,
            (static_cast<double>(index * kTempoStepHops)
             + 0.5 * static_cast<double>(kTempoStepHops))
                * static_cast<double>(kHopFrames)
                / static_cast<double>(kAnalysisSampleRate));
        point.bpm = std::round(bpm * 1000.0) / 1000.0;
        point.confidence = quantized_unit(tempo.confidence[index]);
        if (local_points.empty()
            || point.time_seconds > local_points.back().time_seconds) {
            local_points.push_back(point);
            last_bpm = bpm;
        }
    }
    if (local_points.size() <= kMaximumMusicTempoPoints) {
        analysis.tempo_points = std::move(local_points);
    } else {
        analysis.tempo_points.reserve(kMaximumMusicTempoPoints);
        for (std::size_t output = 0U; output < kMaximumMusicTempoPoints; ++output) {
            const std::size_t source = output * (local_points.size() - 1U)
                                       / (kMaximumMusicTempoPoints - 1U);
            if (analysis.tempo_points.empty()
                || local_points[source].time_seconds
                       > analysis.tempo_points.back().time_seconds) {
                analysis.tempo_points.push_back(local_points[source]);
            }
        }
    }
    return progress.report(880U);
}

bool build_features(const std::vector<HopRecord>& records,
                    MusicAnalysis& analysis,
                    ProgressReporter& progress) {
    if (records.empty()) {
        return progress.report(990U);
    }
    std::vector<float> energy;
    std::vector<float> bass;
    std::vector<float> midrange;
    std::vector<float> treble;
    std::vector<float> onset;
    energy.reserve(records.size());
    bass.reserve(records.size());
    midrange.reserve(records.size());
    treble.reserve(records.size());
    onset.reserve(records.size());
    for (const HopRecord& record : records) {
        energy.push_back(record.energy);
        bass.push_back(record.bass);
        midrange.push_back(record.midrange);
        treble.push_back(record.treble);
        onset.push_back(record.onset);
    }
    const double energy_scale = percentile(energy, 0.95);
    const double bass_scale = percentile(bass, 0.95);
    const double mid_scale = percentile(midrange, 0.95);
    const double treble_scale = percentile(treble, 0.95);
    const double onset_scale = percentile(onset, 0.99);

    const std::size_t count = (std::min)(records.size(), kMaximumMusicFeatureSamples);
    analysis.feature_samples.assign(count, MusicFeatureSample {});
    for (std::size_t index = 0U; index < count; ++index) {
        if ((index & 63U) == 0U && !progress.report(
                880U + scaled_progress(index, count, 0U, 100U))) {
            return false;
        }
        const std::size_t begin = index * records.size() / count;
        std::size_t end = (index + 1U) * records.size() / count;
        end = (std::max)(end, begin + 1U);
        double energy_sum = 0.0;
        double bass_sum = 0.0;
        double mid_sum = 0.0;
        double treble_sum = 0.0;
        double onset_peak = 0.0;
        double centroid_sum = 0.0;
        double centroid_weight = 0.0;
        double flatness_sum = 0.0;
        double chroma_x = 0.0;
        double chroma_y = 0.0;
        double chroma_weight = 0.0;
        double chroma_strength_sum = 0.0;
        for (std::size_t item = begin; item < end; ++item) {
            energy_sum += static_cast<double>(records[item].energy)
                          * records[item].energy;
            bass_sum += static_cast<double>(records[item].bass)
                        * records[item].bass;
            mid_sum += static_cast<double>(records[item].midrange)
                       * records[item].midrange;
            treble_sum += static_cast<double>(records[item].treble)
                          * records[item].treble;
            onset_peak = (std::max)(onset_peak,
                                    static_cast<double>(records[item].onset));
            const double spectral_weight = (std::max)(
                1.0e-9, static_cast<double>(records[item].energy));
            centroid_sum += static_cast<double>(records[item].spectral_centroid)
                            * spectral_weight;
            centroid_weight += spectral_weight;
            flatness_sum += records[item].spectral_flatness;
            const double pitch_weight = spectral_weight
                                        * records[item].chroma_strength;
            const double angle = 2.0 * kPi * records[item].chroma_hue;
            chroma_x += std::cos(angle) * pitch_weight;
            chroma_y += std::sin(angle) * pitch_weight;
            chroma_weight += pitch_weight;
            chroma_strength_sum += records[item].chroma_strength;
        }
        const double divisor = static_cast<double>(end - begin);
        MusicFeatureSample& feature = analysis.feature_samples[index];
        feature.energy = quantized_unit(std::sqrt(energy_sum / divisor) / energy_scale);
        feature.bass = quantized_unit(std::sqrt(bass_sum / divisor) / bass_scale);
        feature.midrange = quantized_unit(std::sqrt(mid_sum / divisor) / mid_scale);
        feature.treble = quantized_unit(std::sqrt(treble_sum / divisor) / treble_scale);
        feature.onset = quantized_unit(onset_peak / onset_scale);
        feature.spectral_centroid = quantized_unit(
            centroid_weight > 0.0 ? centroid_sum / centroid_weight : 0.0);
        feature.spectral_flatness = quantized_unit(flatness_sum / divisor);
        if (chroma_weight > 1.0e-12) {
            double angle = std::atan2(chroma_y, chroma_x) / (2.0 * kPi);
            angle -= std::floor(angle);
            feature.chroma_hue = quantized_unit(angle);
            feature.chroma_strength = quantized_unit(
                chroma_strength_sum / divisor
                * std::hypot(chroma_x, chroma_y) / chroma_weight);
        }
    }
    if (analysis.duration_seconds > 0.0) {
        for (double beat_time : analysis.beat_times_seconds) {
            const double fraction = beat_time / analysis.duration_seconds;
            const std::size_t index = (std::min)(
                count - 1U, static_cast<std::size_t>((std::max)(0.0, fraction)
                                                      * static_cast<double>(count)));
            analysis.feature_samples[index].beat = 1.0F;
        }
    }
    return progress.report(990U);
}

bool analyze_impl(const std::string& path,
                  MusicAnalysis& destination,
                  const AudioProgressCallback& callback,
                  const std::atomic_bool* cancel,
                  std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    ProgressReporter progress(callback, cancel, error);
    if (!progress.report(0U)) {
        return false;
    }
    fs::path native_path;
    std::uint64_t file_size = 0U;
    if (!preflight_path(path, native_path, file_size, error)) {
        return false;
    }

    DigestResult digest;
    if (!digest_file(native_path, file_size, 0U, kHashProgressEnd,
                     progress, digest, error)) {
        return false;
    }
    const std::string format = source_format(digest);
    if (format.empty()) {
        return fail(error, "The source is not a supported WAV, FLAC, or MP3 file.");
    }
    WavePreflight wave;
    if (!preflight_wave(native_path, file_size, format, wave, error)) {
        return false;
    }

    ma_decoder_config decoder_config = ma_decoder_config_init(ma_format_f32, 1U, 0U);
    decoder_config.channelMixMode = ma_channel_mix_mode_rectangular;
    decoder_config.ditherMode = ma_dither_mode_none;
    DecoderGuard decoder;
#if defined(_WIN32)
    const std::wstring native = native_path.native();
    const ma_result init_result =
        ma_decoder_init_file_w(native.c_str(), &decoder_config, &decoder.value);
#else
    const ma_result init_result =
        ma_decoder_init_file(path.c_str(), &decoder_config, &decoder.value);
#endif
    if (init_result != MA_SUCCESS) {
        return fail(error, decoder_error("Could not decode the music source", init_result));
    }
    decoder.initialized = true;

    ma_format input_format = ma_format_unknown;
    ma_uint32 source_channels = 0U;
    ma_uint32 source_rate = 0U;
    const ma_result metadata_result = ma_data_source_get_data_format(
        decoder.value.pBackend, &input_format, &source_channels, &source_rate,
        nullptr, 0U);
    if (metadata_result != MA_SUCCESS) {
        return fail(error, decoder_error("Could not read source audio metadata",
                                         metadata_result));
    }
    if (source_channels == 0U || source_channels > kMaximumSourceChannels) {
        return fail(error, "The music source must contain between 1 and 32 channels.");
    }
    if (source_rate < kMinimumSourceSampleRate
        || source_rate > kMaximumSourceSampleRate) {
        return fail(error, "The music source sample rate must be between 8 kHz and 384 kHz.");
    }
    if (decoder.value.outputFormat != ma_format_f32
        || decoder.value.outputChannels != 1U
        || decoder.value.outputSampleRate != source_rate) {
        return fail(error, "The decoder could not create the canonical mono analysis stream.");
    }

    ma_uint64 declared_frames = 0U;
    const ma_result length_result = ma_data_source_get_length_in_pcm_frames(
        decoder.value.pBackend, &declared_frames);
    if (length_result != MA_SUCCESS) {
        declared_frames = 0U;
    }
    const std::uint64_t maximum_frames = kMaximumDurationSeconds * source_rate;
    if (declared_frames > maximum_frames) {
        return fail(error, "The music source exceeds the two-hour analysis limit.");
    }

    std::vector<float> decoded(static_cast<std::size_t>(kDecodeChunkFrames));
    AdaptiveBeatObserver beat_observer;
    if (!beat_observer.valid()) {
        return fail(error, "Could not initialize the adaptive beat tracker.");
    }
    HopAccumulator accumulator(beat_observer);
    LinearResampler resampler(source_rate, accumulator);
    std::uint64_t source_frames = 0U;
    while (true) {
        ma_uint64 frames_read = 0U;
        const ma_result read_result = ma_decoder_read_pcm_frames(
            &decoder.value, decoded.data(), kDecodeChunkFrames, &frames_read);
        if (read_result != MA_SUCCESS && read_result != MA_AT_END) {
            return fail(error, decoder_error("The music source is truncated or unreadable",
                                             read_result));
        }
        if (frames_read > kDecodeChunkFrames
            || source_frames > maximum_frames - frames_read) {
            return fail(error, "The decoded music source exceeds its supported duration.");
        }
        for (ma_uint64 index = 0U; index < frames_read; ++index) {
            float sample = decoded[static_cast<std::size_t>(index)];
            if (!std::isfinite(sample)) {
                return fail(error,
                            "The music source contains a non-finite audio sample.");
            }
            sample = (std::max)(-1.0F, (std::min)(1.0F, sample));
            decoded[static_cast<std::size_t>(index)] = sample;
            resampler.push(sample);
        }
        source_frames += frames_read;
        const std::uint64_t progress_total = declared_frames != 0U
                                                 ? declared_frames
                                                 : maximum_frames;
        if (!progress.report(scaled_progress(source_frames, progress_total,
                                             kHashProgressEnd,
                                             kDecodeProgressEnd))) {
            return false;
        }
        if (frames_read == 0U || read_result == MA_AT_END) {
            break;
        }
    }
    if (source_frames == 0U) {
        return fail(error, "The music source contains no decoded audio frames.");
    }
    if (wave.wave && source_frames != wave.frame_count) {
        return fail(error, "The decoded WAV frame count does not match its data chunks.");
    }
    resampler.finish(source_frames);
    accumulator.finish();
    if (!beat_observer.finish()) {
        return fail(error, "The adaptive beat tracker exceeded its bounded event cache.");
    }
    if (!progress.report(kDecodeProgressEnd)) {
        return false;
    }

    MusicAnalysis analysis;
    analysis.schema_version = 1U;
    analysis.analyzer_version = kAnalyzerVersion;
    analysis.source_sha256 = digest.sha256;
    analysis.source_basename = detail::path_to_utf8(native_path.filename());
    analysis.source_format = format;
    analysis.source_frame_count = source_frames;
    analysis.source_sample_rate = source_rate;
    analysis.source_channel_count = source_channels;
    analysis.duration_seconds = static_cast<double>(source_frames)
                                / static_cast<double>(source_rate);

    if (!detect_beats(accumulator.records(), beat_observer,
                      analysis, progress, error)
        || !build_features(accumulator.records(), analysis, progress)) {
        return false;
    }
    if (analysis.feature_samples.size() > kMaximumMusicFeatureSamples
        || analysis.beat_times_seconds.size() > kMaximumMusicBeats
        || analysis.tempo_points.size() > kMaximumMusicTempoPoints) {
        return fail(error, "Audio analysis exceeded the project format limits.");
    }

    // Hashing both before and after decoding prevents a same-path replacement
    // from pairing cached analysis with bytes that were not actually analyzed.
    std::uint64_t final_file_size = 0U;
    fs::path final_native_path;
    if (!preflight_path(path, final_native_path, final_file_size, error)) {
        return false;
    }
    if (final_file_size != file_size || final_native_path != native_path) {
        return fail(error, "The music source changed while it was being analyzed.");
    }
    DigestResult final_digest;
    if (!digest_file(final_native_path, final_file_size, 990U, kProgressTotal,
                     progress, final_digest, error)) {
        return false;
    }
    if (final_digest.sha256 != digest.sha256) {
        return fail(error, "The music source changed while it was being analyzed.");
    }
    if (!progress.report(kProgressTotal)) {
        return false;
    }
    destination = std::move(analysis);
    return true;
}

} // namespace

bool analyze_music_file(const std::string& path,
                        MusicAnalysis& destination,
                        const AudioProgressCallback& progress,
                        const std::atomic_bool* cancel,
                        std::string* error) {
    try {
        return analyze_impl(path, destination, progress, cancel, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "Audio analysis ran out of memory.");
    } catch (const std::exception& exception) {
        return fail(error, "Audio analysis failed: " + std::string(exception.what()));
    } catch (...) {
        return fail(error, "Audio analysis failed with an unknown error.");
    }
}

bool verify_music_source(const std::string& path,
                         const std::string& expected_sha256,
                         const AudioProgressCallback& callback,
                         const std::atomic_bool* cancel,
                         std::string* error) {
    try {
        if (error != nullptr) {
            error->clear();
        }
        if (!valid_sha256(expected_sha256)) {
            return fail(error, "The expected music SHA-256 digest is invalid.");
        }
        ProgressReporter progress(callback, cancel, error);
        if (!progress.report(0U)) {
            return false;
        }
        fs::path native_path;
        std::uint64_t file_size = 0U;
        if (!preflight_path(path, native_path, file_size, error)) {
            return false;
        }
        DigestResult digest;
        if (!digest_file(native_path, file_size, 0U, kProgressTotal,
                         progress, digest, error)) {
            return false;
        }
        std::string normalized(expected_sha256.size(), '0');
        std::transform(expected_sha256.begin(), expected_sha256.end(),
                       normalized.begin(), ascii_lower);
        if (digest.sha256 != normalized) {
            return fail(error,
                        "The selected music source does not match the stored SHA-256 digest.");
        }
        return progress.report(kProgressTotal);
    } catch (const std::bad_alloc&) {
        return fail(error, "Music source verification ran out of memory.");
    } catch (const std::exception& exception) {
        return fail(error, "Music source verification failed: "
                               + std::string(exception.what()));
    } catch (...) {
        return fail(error, "Music source verification failed with an unknown error.");
    }
}

} // namespace audio
} // namespace pvt
