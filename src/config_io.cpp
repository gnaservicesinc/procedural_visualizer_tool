#include "procedural_visualizer_tool.h"
#include "config_codec.h"
#include "path_utf8.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include "windows_file_install.h"
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace pvt {
namespace {

// PVT setup files are deliberately line-oriented and deterministic:
//
//   PVT_SETUP<TAB>1
//   canvas.width<TAB>1920
//   waves.count<TAB>1
//   waves.0.name<TAB>Main%20wave
//
// Every subsequent line is exactly one key, one tab, and one value. Collection
// records use zero-based indexes (`waves.N.*`, `swings.N.*`, `effects.N.*`).
// Strings use RFC 3986-style percent encoding over their exact bytes: only
// ALPHA / DIGIT / "-._~" remain literal. There are no comments, aliases, or
// optional records within a given version. This keeps parsing unambiguous,
// safely rejectable, and friendly to source control while still allowing
// arbitrary string bytes. Version 2 adds PNG compression and custom-OBJ path
// fields. Version 3 separates final output alpha from procedural layer alpha.
// Version 4 adds spatial swings/effects, effect stage selection, palettes, and
// layer transforms. Version 5 adds project clocks, bounded cached music
// analysis, a master swing switch, per-layer audio response, and portable
// custom-OBJ attachment identity. Older versions remain accepted with neutral
// defaults for every field introduced later.

constexpr std::size_t kMaximumLineBytes = 256U * 1024U;
constexpr std::size_t kMaximumKeyBytes = 128U;
constexpr std::size_t kMaximumDecodedStringBytes = 64U * 1024U;
constexpr std::size_t kMaximumRecordCount = 131072U;
constexpr std::size_t kMaximumMeterExpressionBytes = 256U;
constexpr std::size_t kMaximumAnalyzerVersionBytes = 256U;
constexpr std::size_t kMaximumMusicBasenameBytes = 4096U;
constexpr std::size_t kMaximumMusicFormatBytes = 64U;
constexpr std::size_t kSha256HexBytes = 64U;

static_assert(kSetupFormatVersion == 5U,
              "config_io.cpp implements setup format version 5");
static_assert(std::is_nothrow_move_assignable_v<RenderConfig>,
              "transactional setup loading requires a non-throwing commit");

using Records = std::map<std::string, std::string>;

void clear_error(std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
}

bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

std::string record_error(std::string_view prefix, std::string_view key) {
    std::string message;
    message.reserve(prefix.size() + key.size() + 4U);
    message.append(prefix);
    message.push_back(' ');
    message.push_back('\'');
    message.append(key);
    message.push_back('\'');
    message.push_back('.');
    return message;
}

bool valid_key(std::string_view key) {
    if (key.empty() || key.size() > kMaximumKeyBytes) {
        return false;
    }
    for (const char raw_character : key) {
        const unsigned char character = static_cast<unsigned char>(raw_character);
        if ((character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9')
            || character == '.' || character == '_') {
            continue;
        }
        return false;
    }
    return true;
}

bool line_is_ascii_text(std::string_view line) {
    for (const char raw_character : line) {
        const unsigned char character = static_cast<unsigned char>(raw_character);
        if (character == '\t') {
            continue;
        }
        if (character < 0x20U || character > 0x7eU) {
            return false;
        }
    }
    return true;
}

bool parse_records(const std::string& contents,
                   Records& records,
                   std::uint32_t& setup_version,
                   std::string* error) {
    if (contents.empty()) {
        return fail(error, "Setup file is empty.");
    }

    std::size_t line_start = 0;
    std::size_t line_number = 0;
    while (line_start < contents.size()) {
        const std::size_t newline = contents.find('\n', line_start);
        const std::size_t raw_end = newline == std::string::npos ? contents.size() : newline;
        if (raw_end - line_start > kMaximumLineBytes) {
            return fail(error, "Setup line " + std::to_string(line_number + 1U)
                                   + " exceeds the 256 KiB line limit.");
        }

        std::string_view line(contents.data() + line_start, raw_end - line_start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1U);
        }
        ++line_number;

        if (line.empty()) {
            return fail(error, "Setup line " + std::to_string(line_number)
                                   + " is empty; blank lines are not valid in setup files.");
        }
        if (!line_is_ascii_text(line)) {
            return fail(error, "Setup line " + std::to_string(line_number)
                                   + " contains a raw control or non-ASCII byte; strings must be percent-encoded.");
        }

        if (line_number == 1U) {
            if (line == "PVT_SETUP\t1") {
                setup_version = 1U;
            } else if (line == "PVT_SETUP\t2") {
                setup_version = 2U;
            } else if (line == "PVT_SETUP\t3") {
                setup_version = 3U;
            } else if (line == "PVT_SETUP\t4") {
                setup_version = 4U;
            } else if (line == "PVT_SETUP\t5") {
                setup_version = 5U;
            } else {
                return fail(error,
                            "Unsupported or malformed setup header; expected "
                            "'PVT_SETUP\\t1' through 'PVT_SETUP\\t5'.");
            }
        } else {
            const std::size_t tab = line.find('\t');
            if (tab == std::string_view::npos || line.find('\t', tab + 1U) != std::string_view::npos) {
                return fail(error, "Setup line " + std::to_string(line_number)
                                       + " must contain exactly one tab delimiter.");
            }

            const std::string_view key_view = line.substr(0U, tab);
            const std::string_view value_view = line.substr(tab + 1U);
            if (!valid_key(key_view)) {
                return fail(error, "Setup line " + std::to_string(line_number)
                                       + " has an invalid or overlong key.");
            }
            if (records.size() >= kMaximumRecordCount) {
                return fail(error, "Setup file exceeds the 131072-record limit.");
            }

            std::string key(key_view);
            const auto inserted = records.emplace(key, std::string(value_view));
            if (!inserted.second) {
                return fail(error, record_error("Duplicate setup key", key));
            }
        }

        if (newline == std::string::npos) {
            break;
        }
        line_start = newline + 1U;
    }

    if (line_number == 0U) {
        return fail(error, "Setup file is empty.");
    }
    return true;
}

bool read_setup_file(const std::string& path, std::string& contents, std::string* error) {
    if (path.empty() || path.size() > kMaximumDecodedStringBytes
        || path.find('\0') != std::string::npos) {
        return fail(error, "Setup path is empty, contains a NUL byte, or exceeds 64 KiB.");
    }

    const std::filesystem::path native_path = detail::path_from_utf8(path);
    std::ifstream input(native_path, std::ios::binary);
    if (!input) {
        return fail(error, "Could not open setup file '" + path + "' for reading.");
    }

    contents.clear();
    std::array<char, 8192U> buffer{};
    for (;;) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            const std::size_t byte_count = static_cast<std::size_t>(count);
            if (contents.size() > kMaximumSetupBytes - byte_count) {
                return fail(error, "Setup file exceeds the 8 MiB input limit.");
            }
            contents.append(buffer.data(), byte_count);
        }
        if (input.eof()) {
            break;
        }
        if (!input) {
            return fail(error, "I/O error while reading setup file '" + path + "'.");
        }
    }
    return true;
}

bool take_record(Records& records,
                 const std::string& key,
                 std::string& value,
                 std::string* error) {
    const auto found = records.find(key);
    if (found == records.end()) {
        return fail(error, record_error("Missing required setup key", key));
    }
    value = std::move(found->second);
    records.erase(found);
    return true;
}

template <typename Integer>
bool parse_integer_exact(std::string_view text, Integer& destination) {
    static_assert(std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>);
    if (text.empty()) {
        return false;
    }
    Integer parsed{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return false;
    }
    destination = parsed;
    return true;
}

bool parse_double_exact(std::string_view text, double& destination) {
    if (text.empty()) {
        return false;
    }
    std::istringstream stream{std::string(text)};
    stream.imbue(std::locale::classic());
    stream >> std::noskipws;
    double parsed = 0.0;
    if (!(stream >> parsed) || stream.peek() != std::char_traits<char>::eof()
        || !std::isfinite(parsed)) {
        return false;
    }
    destination = parsed;
    return true;
}

bool parse_bool_exact(std::string_view text, bool& destination) {
    if (text == "0") {
        destination = false;
        return true;
    }
    if (text == "1") {
        destination = true;
        return true;
    }
    return false;
}

int hexadecimal_value(unsigned char character) {
    if (character >= '0' && character <= '9') {
        return static_cast<int>(character - '0');
    }
    if (character >= 'A' && character <= 'F') {
        return static_cast<int>(character - 'A') + 10;
    }
    if (character >= 'a' && character <= 'f') {
        return static_cast<int>(character - 'a') + 10;
    }
    return -1;
}

bool is_unreserved(unsigned char character) {
    return (character >= 'A' && character <= 'Z')
           || (character >= 'a' && character <= 'z')
           || (character >= '0' && character <= '9')
           || character == '-' || character == '.' || character == '_'
           || character == '~';
}

bool percent_decode(std::string_view encoded, std::string& decoded) {
    decoded.clear();
    decoded.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size();) {
        const unsigned char character = static_cast<unsigned char>(encoded[index]);
        if (character == '%') {
            if (index + 2U >= encoded.size()) {
                return false;
            }
            const int high = hexadecimal_value(static_cast<unsigned char>(encoded[index + 1U]));
            const int low = hexadecimal_value(static_cast<unsigned char>(encoded[index + 2U]));
            if (high < 0 || low < 0) {
                return false;
            }
            decoded.push_back(static_cast<char>((high << 4) | low));
            index += 3U;
        } else {
            if (!is_unreserved(character)) {
                return false;
            }
            decoded.push_back(static_cast<char>(character));
            ++index;
        }
        if (decoded.size() > kMaximumDecodedStringBytes) {
            return false;
        }
    }
    return true;
}

bool percent_encode(std::string_view decoded, std::string& encoded) {
    if (decoded.size() > kMaximumDecodedStringBytes) {
        return false;
    }
    constexpr char hexadecimal[] = "0123456789ABCDEF";
    encoded.clear();
    encoded.reserve(decoded.size() * 3U);
    for (const char raw_character : decoded) {
        const unsigned char character = static_cast<unsigned char>(raw_character);
        if (is_unreserved(character)) {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('%');
            encoded.push_back(hexadecimal[character >> 4U]);
            encoded.push_back(hexadecimal[character & 0x0fU]);
        }
    }
    return true;
}

template <typename Integer>
bool consume_integer(Records& records,
                     const std::string& key,
                     Integer& destination,
                     std::string* error) {
    std::string value;
    if (!take_record(records, key, value, error)) {
        return false;
    }
    if (!parse_integer_exact(value, destination)) {
        return fail(error, record_error("Invalid integer in setup key", key));
    }
    return true;
}

bool consume_double(Records& records,
                    const std::string& key,
                    double& destination,
                    std::string* error) {
    std::string value;
    if (!take_record(records, key, value, error)) {
        return false;
    }
    if (!parse_double_exact(value, destination)) {
        return fail(error, record_error("Invalid finite number in setup key", key));
    }
    return true;
}

bool consume_bool(Records& records,
                  const std::string& key,
                  bool& destination,
                  std::string* error) {
    std::string value;
    if (!take_record(records, key, value, error)) {
        return false;
    }
    if (!parse_bool_exact(value, destination)) {
        return fail(error, record_error("Invalid boolean (expected 0 or 1) in setup key", key));
    }
    return true;
}

bool consume_string(Records& records,
                    const std::string& key,
                    std::string& destination,
                    std::string* error) {
    std::string value;
    if (!take_record(records, key, value, error)) {
        return false;
    }
    std::string decoded;
    if (!percent_decode(value, decoded)) {
        return fail(error, record_error("Invalid or overlong percent-encoded string in setup key", key));
    }
    destination = std::move(decoded);
    return true;
}

bool consume_bounded_string(Records& records,
                            const std::string& key,
                            std::size_t maximum,
                            std::string& destination,
                            std::string* error) {
    std::string value;
    if (!consume_string(records, key, value, error)) {
        return false;
    }
    if (value.size() > maximum) {
        return fail(error, record_error("Decoded string exceeds its field limit in setup key",
                                        key));
    }
    destination = std::move(value);
    return true;
}

bool consume_float(Records& records,
                   const std::string& key,
                   float& destination,
                   std::string* error) {
    double value = 0.0;
    if (!consume_double(records, key, value, error)) {
        return false;
    }
    if (value < -static_cast<double>(std::numeric_limits<float>::max())
        || value > static_cast<double>(std::numeric_limits<float>::max())) {
        return fail(error, record_error("Number exceeds the float range in setup key", key));
    }
    destination = static_cast<float>(value);
    return true;
}

bool is_lowercase_sha256(std::string_view digest) {
    if (digest.empty()) {
        return true;
    }
    if (digest.size() != kSha256HexBytes) {
        return false;
    }
    return std::all_of(digest.begin(), digest.end(), [](char character) {
        return (character >= '0' && character <= '9')
               || (character >= 'a' && character <= 'f');
    });
}

template <typename Enum, std::size_t Count>
bool consume_enum(Records& records,
                  const std::string& key,
                  Enum& destination,
                  const std::array<std::pair<std::string_view, Enum>, Count>& values,
                  std::string* error) {
    std::string value;
    if (!take_record(records, key, value, error)) {
        return false;
    }
    for (const auto& entry : values) {
        if (value == entry.first) {
            destination = entry.second;
            return true;
        }
    }
    return fail(error, record_error("Unknown enum token in setup key", key));
}

bool consume_count(Records& records,
                   const std::string& key,
                   std::size_t maximum,
                   std::size_t& destination,
                   std::string* error) {
    std::uint64_t parsed = 0;
    if (!consume_integer(records, key, parsed, error)) {
        return false;
    }
    if (parsed > static_cast<std::uint64_t>(maximum)) {
        return fail(error, record_error("Collection count exceeds its configured maximum in setup key", key));
    }
    destination = static_cast<std::size_t>(parsed);
    return true;
}

constexpr std::array<std::pair<std::string_view, EdgeMode>, 4U> kEdgeModes{{
    {"alpha", EdgeMode::Alpha},
    {"black", EdgeMode::Black},
    {"white", EdgeMode::White},
    {"reflect", EdgeMode::Reflect},
}};

constexpr std::array<std::pair<std::string_view, EffectType>, 6U> kEffectTypes{{
    {"endless_zoom", EffectType::EndlessZoom},
    {"ripple", EffectType::Ripple},
    {"shake", EffectType::Shake},
    {"flag_wave", EffectType::FlagWave},
    {"glow", EffectType::Glow},
    {"block_scale", EffectType::BlockScale},
}};

constexpr std::array<std::pair<std::string_view, EffectSpace>, 2U> kEffectSpaces{{
    {"texture", EffectSpace::Texture},
    {"surface", EffectSpace::Surface},
}};

constexpr std::array<std::pair<std::string_view, DitherMethod>, 3U> kDitherMethods{{
    {"blue_noise", DitherMethod::BlueNoise},
    {"ordered_bayer", DitherMethod::OrderedBayer},
    {"floyd_steinberg", DitherMethod::FloydSteinberg},
}};

constexpr std::array<std::pair<std::string_view, SurfaceMapping>, 5U> kSurfaceMappings{{
    {"plane", SurfaceMapping::Plane},
    {"cylinder", SurfaceMapping::Cylinder},
    {"sphere", SurfaceMapping::Sphere},
    {"cube", SurfaceMapping::Cube},
    {"custom_obj", SurfaceMapping::CustomObj},
}};

constexpr std::array<std::pair<std::string_view, Waveform>, 4U> kWaveforms{{
    {"sine", Waveform::Sine},
    {"triangle", Waveform::Triangle},
    {"smooth_pulse", Waveform::SmoothPulse},
    {"bounce", Waveform::Bounce},
}};

constexpr std::array<std::pair<std::string_view, QuantizationMode>, 3U> kQuantizationModes{{
    {"rgb", QuantizationMode::Rgb},
    {"luminance", QuantizationMode::Luminance},
    {"hue", QuantizationMode::Hue},
}};

constexpr std::array<std::pair<std::string_view, MirrorMode>, 6U> kMirrorModes{{
    {"none", MirrorMode::None},
    {"left_to_right", MirrorMode::LeftToRight},
    {"right_to_left", MirrorMode::RightToLeft},
    {"top_to_bottom", MirrorMode::TopToBottom},
    {"bottom_to_top", MirrorMode::BottomToTop},
    {"four_way", MirrorMode::FourWay},
}};

constexpr std::array<std::pair<std::string_view, ClockMode>, 5U> kClockModes{{
    {"default", ClockMode::Default},
    {"frame", ClockMode::Frame},
    {"time", ClockMode::Time},
    {"meter", ClockMode::Meter},
    {"music", ClockMode::Music},
}};

constexpr std::array<std::pair<std::string_view, ClockInterpolation>, 3U>
    kClockInterpolations{{
        {"hold", ClockInterpolation::Hold},
        {"linear", ClockInterpolation::Linear},
        {"smoothstep", ClockInterpolation::Smoothstep},
    }};

constexpr std::array<std::pair<std::string_view, ClockFit>, 2U> kClockFits{{
    {"exact", ClockFit::Exact},
    {"fit_sequence", ClockFit::FitSequence},
}};

constexpr std::array<std::pair<std::string_view, MusicTempoMode>, 3U>
    kMusicTempoModes{{
        {"half", MusicTempoMode::Half},
        {"detected", MusicTempoMode::Detected},
        {"double", MusicTempoMode::Double},
    }};

constexpr std::array<std::pair<std::string_view, MusicFeature>, 10U>
    kMusicFeatures{{
        {"energy", MusicFeature::Energy},
        {"bass", MusicFeature::Bass},
        {"midrange", MusicFeature::Midrange},
        {"treble", MusicFeature::Treble},
        {"onset", MusicFeature::Onset},
        {"beat", MusicFeature::Beat},
        {"spectral_centroid", MusicFeature::SpectralCentroid},
        {"spectral_flatness", MusicFeature::SpectralFlatness},
        {"chroma_hue", MusicFeature::ChromaHue},
        {"chroma_strength", MusicFeature::ChromaStrength},
    }};

constexpr std::array<std::pair<std::string_view, MusicSwingPolicy>, 3U>
    kMusicSwingPolicies{{
        {"suppress_all", MusicSwingPolicy::SuppressAll},
        {"suppress_global", MusicSwingPolicy::SuppressGlobal},
        {"keep_all", MusicSwingPolicy::KeepAll},
    }};

std::string indexed_key(std::string_view collection,
                        std::size_t index,
                        std::string_view field) {
    std::string key;
    const std::string index_text = std::to_string(index);
    key.reserve(collection.size() + index_text.size() + field.size() + 2U);
    key.append(collection);
    key.push_back('.');
    key.append(index_text);
    key.push_back('.');
    key.append(field);
    return key;
}

template <typename Enum, std::size_t Count>
bool enum_token(Enum value,
                const std::array<std::pair<std::string_view, Enum>, Count>& values,
                std::string_view& token) {
    for (const auto& entry : values) {
        if (entry.second == value) {
            token = entry.first;
            return true;
        }
    }
    return false;
}

class SetupBuilder {
public:
    explicit SetupBuilder(std::string* error)
        : error_(error),
          contents_("PVT_SETUP\t" + std::to_string(kSetupFormatVersion) + "\n") {}

    bool add(std::string_view key, std::string_view value) {
        if (!ok_) {
            return false;
        }
        if (!valid_key(key)) {
            ok_ = fail(error_, "Internal setup serializer produced an invalid key.");
            return false;
        }
        if (key.size() + value.size() + 1U > kMaximumLineBytes) {
            ok_ = fail(error_, record_error("Serialized setup line exceeds 256 KiB at key", key));
            return false;
        }
        const std::size_t added = key.size() + value.size() + 2U;
        if (contents_.size() > kMaximumSetupBytes - added) {
            ok_ = fail(error_, "Serialized setup exceeds the 8 MiB format limit.");
            return false;
        }
        contents_.append(key);
        contents_.push_back('\t');
        contents_.append(value);
        contents_.push_back('\n');
        return true;
    }

    template <typename Integer>
    bool add_integer(std::string_view key, Integer value) {
        static_assert(std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>);
        std::array<char, 64U> buffer{};
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 10);
        if (result.ec != std::errc{}) {
            ok_ = fail(error_, record_error("Could not serialize integer setup key", key));
            return false;
        }
        return add(key, std::string_view(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())));
    }

    bool add_double(std::string_view key, double value) {
        if (!std::isfinite(value)) {
            ok_ = fail(error_, record_error("Cannot serialize non-finite setup value at key", key));
            return false;
        }
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
        if (!stream) {
            ok_ = fail(error_, record_error("Could not serialize numeric setup key", key));
            return false;
        }
        return add(key, stream.str());
    }

    bool add_bool(std::string_view key, bool value) {
        return add(key, value ? "1" : "0");
    }

    bool add_string(std::string_view key, const std::string& value) {
        std::string encoded;
        if (!percent_encode(value, encoded)) {
            ok_ = fail(error_, record_error("String exceeds the 64 KiB setup limit at key", key));
            return false;
        }
        return add(key, encoded);
    }

    template <typename Enum, std::size_t Count>
    bool add_enum(std::string_view key,
                  Enum value,
                  const std::array<std::pair<std::string_view, Enum>, Count>& values) {
        std::string_view token;
        if (!enum_token(value, values, token)) {
            ok_ = fail(error_, record_error("Cannot serialize unknown enum value at key", key));
            return false;
        }
        return add(key, token);
    }

    bool ok() const { return ok_; }
    const std::string& contents() const { return contents_; }

private:
    std::string* error_ = nullptr;
    std::string contents_;
    bool ok_ = true;
};

bool validate_persistence_bounds(const RenderConfig& config,
                                 std::string* error) {
    const MusicAnalysis& music = config.clock.music;
    if (config.clock.meter.expression.size() > kMaximumMeterExpressionBytes) {
        return fail(error, "Cannot save configuration: the meter expression exceeds 256 bytes.");
    }
    if (music.analyzer_version.size() > kMaximumAnalyzerVersionBytes
        || music.source_basename.size() > kMaximumMusicBasenameBytes
        || music.source_format.size() > kMaximumMusicFormatBytes) {
        return fail(error, "Cannot save configuration: music source metadata exceeds its field limit.");
    }
    if (!is_lowercase_sha256(music.source_sha256)) {
        return fail(error,
                    "Cannot save configuration: the music source digest must be empty or 64 lowercase hexadecimal characters.");
    }
    if (!is_lowercase_sha256(config.surface.obj_sha256)
        || config.surface.obj_basename.size()
               > kMaximumAttachmentBasenameBytes) {
        return fail(error,
                    "Cannot save configuration: custom OBJ attachment metadata is invalid.");
    }
    if (music.beat_times_seconds.size() > kMaximumMusicBeats
        || music.tempo_points.size() > kMaximumMusicTempoPoints
        || music.feature_samples.size() > kMaximumMusicFeatureSamples) {
        return fail(error,
                    "Cannot save configuration: cached music analysis exceeds a public collection maximum.");
    }
    return true;
}

bool serialize_setup(const RenderConfig& config,
                     std::string& serialized,
                     std::string* error) {
    const ValidationResult validation = validate(config);
    if (!validation.ok) {
        return fail(error, "Cannot save invalid configuration: " + validation.message);
    }
    if (config.waves.size() > kMaximumWaves
        || config.swings.size() > kMaximumSwings
        || config.effects.size() > kMaximumEffects) {
        return fail(error, "Cannot save configuration: a collection exceeds its public maximum.");
    }
    if (!validate_persistence_bounds(config, error)) {
        return false;
    }

    SetupBuilder builder(error);
    builder.add_integer("canvas.width", config.width);
    builder.add_integer("canvas.height", config.height);
    builder.add_integer("canvas.block_size", config.block_size);
    builder.add_integer("timing.total_frames", config.total_frames);
    builder.add_double("timing.fps", config.fps);

    builder.add_enum("timing.clock.mode", config.clock.mode, kClockModes);
    builder.add_enum("timing.clock.interpolation", config.clock.interpolation,
                     kClockInterpolations);
    builder.add_enum("timing.clock.fit", config.clock.fit, kClockFits);
    builder.add_integer("timing.clock.frame_interval",
                        config.clock.frame_interval);
    builder.add_integer("timing.clock.time_interval_microseconds",
                        config.clock.time_interval_microseconds);
    builder.add_string("timing.clock.meter.expression",
                       config.clock.meter.expression);
    builder.add_double("timing.clock.meter.bpm", config.clock.meter.bpm);
    builder.add_integer("timing.clock.meter.tempo_note_denominator",
                        config.clock.meter.tempo_note_denominator);
    builder.add_enum("timing.clock.music_tempo", config.clock.music_tempo,
                     kMusicTempoModes);
    builder.add_enum("timing.clock.music_swing_policy",
                     config.clock.music_swing_policy, kMusicSwingPolicies);
    builder.add_integer("timing.clock.beat_offset_microseconds",
                        config.clock.beat_offset_microseconds);
    builder.add_double("timing.clock.phase_offset_degrees",
                       config.clock.phase_offset_degrees);
    builder.add_bool("timing.clock.reverse", config.clock.reverse);

    const MusicAnalysis& music = config.clock.music;
    builder.add_integer("timing.music.schema_version", music.schema_version);
    builder.add_string("timing.music.analyzer_version", music.analyzer_version);
    builder.add_string("timing.music.source_sha256", music.source_sha256);
    builder.add_string("timing.music.source_basename", music.source_basename);
    builder.add_string("timing.music.source_format", music.source_format);
    builder.add_integer("timing.music.source_frame_count",
                        music.source_frame_count);
    builder.add_integer("timing.music.source_sample_rate",
                        music.source_sample_rate);
    builder.add_integer("timing.music.source_channel_count",
                        music.source_channel_count);
    builder.add_double("timing.music.duration_seconds", music.duration_seconds);
    builder.add_double("timing.music.detected_bpm", music.detected_bpm);
    builder.add_double("timing.music.tempo_confidence", music.tempo_confidence);
    builder.add_integer("timing.music.beat_times.count",
                        music.beat_times_seconds.size());
    for (std::size_t index = 0U; index < music.beat_times_seconds.size(); ++index) {
        builder.add_double(indexed_key("timing.music.beat_times", index,
                                       "seconds"),
                           music.beat_times_seconds[index]);
    }
    builder.add_integer("timing.music.tempo_points.count",
                        music.tempo_points.size());
    for (std::size_t index = 0U; index < music.tempo_points.size(); ++index) {
        const MusicTempoPoint& point = music.tempo_points[index];
        builder.add_double(indexed_key("timing.music.tempo_points", index,
                                       "time_seconds"),
                           point.time_seconds);
        builder.add_double(indexed_key("timing.music.tempo_points", index, "bpm"),
                           point.bpm);
        builder.add_double(indexed_key("timing.music.tempo_points", index,
                                       "confidence"),
                           point.confidence);
    }
    builder.add_integer("timing.music.feature_samples.count",
                        music.feature_samples.size());
    for (std::size_t index = 0U; index < music.feature_samples.size(); ++index) {
        const MusicFeatureSample& sample = music.feature_samples[index];
        builder.add_double(indexed_key("timing.music.feature_samples", index,
                                       "energy"), sample.energy);
        builder.add_double(indexed_key("timing.music.feature_samples", index,
                                       "bass"), sample.bass);
        builder.add_double(indexed_key("timing.music.feature_samples", index,
                                       "midrange"), sample.midrange);
        builder.add_double(indexed_key("timing.music.feature_samples", index,
                                       "treble"), sample.treble);
        builder.add_double(indexed_key("timing.music.feature_samples", index,
                                       "onset"), sample.onset);
        builder.add_double(indexed_key("timing.music.feature_samples", index,
                                       "beat"), sample.beat);
        builder.add_double(indexed_key("timing.music.feature_samples", index,
                                       "spectral_centroid"),
                           sample.spectral_centroid);
        builder.add_double(indexed_key("timing.music.feature_samples", index,
                                       "spectral_flatness"),
                           sample.spectral_flatness);
        builder.add_double(indexed_key("timing.music.feature_samples", index,
                                       "chroma_hue"), sample.chroma_hue);
        builder.add_double(indexed_key("timing.music.feature_samples", index,
                                       "chroma_strength"),
                           sample.chroma_strength);
    }

    builder.add_integer("waves.count", config.waves.size());
    for (std::size_t index = 0; index < config.waves.size(); ++index) {
        const WaveConfig& wave = config.waves[index];
        builder.add_integer(indexed_key("waves", index, "id"), wave.id);
        builder.add_string(indexed_key("waves", index, "name"), wave.name);
        builder.add_bool(indexed_key("waves", index, "enabled"), wave.enabled);
        builder.add_bool(indexed_key("waves", index, "synchronized"), wave.synchronized);
        builder.add_double(indexed_key("waves", index, "x_percent"), wave.x_percent);
        builder.add_double(indexed_key("waves", index, "y_percent"), wave.y_percent);
        builder.add_double(indexed_key("waves", index, "amplitude"), wave.amplitude);
        builder.add_double(indexed_key("waves", index, "spatial_frequency"), wave.spatial_frequency);
        builder.add_integer(indexed_key("waves", index, "cycles_per_loop"), wave.cycles_per_loop);
        builder.add_double(indexed_key("waves", index, "phase_degrees"), wave.phase_degrees);
        builder.add_double(indexed_key("waves", index, "direction"), wave.direction);
    }

    builder.add_integer("swings.count", config.swings.size());
    for (std::size_t index = 0; index < config.swings.size(); ++index) {
        const SwingConfig& swing = config.swings[index];
        builder.add_integer(indexed_key("swings", index, "id"), swing.id);
        builder.add_string(indexed_key("swings", index, "name"), swing.name);
        builder.add_bool(indexed_key("swings", index, "enabled"), swing.enabled);
        builder.add_enum(indexed_key("swings", index, "waveform"), swing.waveform, kWaveforms);
        builder.add_double(indexed_key("swings", index, "amount"), swing.amount);
        builder.add_integer(indexed_key("swings", index, "cycles_per_loop"), swing.cycles_per_loop);
        builder.add_double(indexed_key("swings", index, "phase_degrees"), swing.phase_degrees);
        builder.add_double(indexed_key("swings", index, "shape"), swing.shape);
        builder.add_double(indexed_key("swings", index, "center_x"), swing.center_x);
        builder.add_double(indexed_key("swings", index, "center_y"), swing.center_y);
        builder.add_double(indexed_key("swings", index, "radius"), swing.radius);
    }

    builder.add_integer("effects.count", config.effects.size());
    for (std::size_t index = 0; index < config.effects.size(); ++index) {
        const EffectConfig& effect = config.effects[index];
        builder.add_integer(indexed_key("effects", index, "id"), effect.id);
        builder.add_string(indexed_key("effects", index, "name"), effect.name);
        builder.add_enum(indexed_key("effects", index, "type"), effect.type, kEffectTypes);
        builder.add_enum(indexed_key("effects", index, "space"), effect.space, kEffectSpaces);
        builder.add_bool(indexed_key("effects", index, "enabled"), effect.enabled);
        builder.add_bool(indexed_key("effects", index, "synchronized"), effect.synchronized);
        builder.add_integer(indexed_key("effects", index, "cycles_per_loop"), effect.cycles_per_loop);
        builder.add_double(indexed_key("effects", index, "phase_degrees"), effect.phase_degrees);
        builder.add_enum(indexed_key("effects", index, "edge_mode"), effect.edge_mode, kEdgeModes);
        builder.add_double(indexed_key("effects", index, "intensity"), effect.intensity);
        builder.add_double(indexed_key("effects", index, "magnitude"), effect.magnitude);
        builder.add_double(indexed_key("effects", index, "frequency"), effect.frequency);
        builder.add_double(indexed_key("effects", index, "secondary"), effect.secondary);
        builder.add_double(indexed_key("effects", index, "center_x"), effect.center_x);
        builder.add_double(indexed_key("effects", index, "center_y"), effect.center_y);
        builder.add_double(indexed_key("effects", index, "angle_degrees"), effect.angle_degrees);
        builder.add_double(indexed_key("effects", index, "radius_pixels"), effect.radius_pixels);
        builder.add_double(indexed_key("effects", index, "threshold"), effect.threshold);
        builder.add_double(indexed_key("effects", index, "soft_knee"), effect.soft_knee);
        builder.add_double(indexed_key("effects", index, "area_radius"), effect.area_radius);
    }

    builder.add_bool("rhythm.swings_enabled", config.swings_enabled);
    builder.add_double("rhythm.phrase_warp", config.phrase_warp);
    builder.add_double("rhythm.ghost_mix", config.ghost_mix);
    builder.add_double("rhythm.ghost_lag_degrees", config.ghost_lag_degrees);

    builder.add_bool("audio_reactive.enabled", config.audio_reactive.enabled);
    builder.add_bool("audio_reactive.synchronized_only",
                     config.audio_reactive.synchronized_only);
    builder.add_bool("audio_reactive.waves_enabled",
                     config.audio_reactive.waves_enabled);
    builder.add_enum("audio_reactive.wave_source",
                     config.audio_reactive.wave_source, kMusicFeatures);
    builder.add_double("audio_reactive.wave_amount",
                       config.audio_reactive.wave_amount);
    builder.add_bool("audio_reactive.effects_enabled",
                     config.audio_reactive.effects_enabled);
    builder.add_enum("audio_reactive.effect_source",
                     config.audio_reactive.effect_source, kMusicFeatures);
    builder.add_double("audio_reactive.effect_amount",
                       config.audio_reactive.effect_amount);
    builder.add_bool("audio_reactive.color_enabled",
                     config.audio_reactive.color_enabled);
    builder.add_enum("audio_reactive.color_source",
                     config.audio_reactive.color_source, kMusicFeatures);
    builder.add_double("audio_reactive.color_amount_degrees",
                       config.audio_reactive.color_amount_degrees);

    builder.add_bool("appearance.displacement_enabled", config.displacement_enabled);
    builder.add_double("appearance.displacement", config.displacement);
    builder.add_bool("appearance.lighting_enabled", config.lighting_enabled);
    builder.add_double("appearance.wave_depth", config.wave_depth);
    builder.add_bool("appearance.spiral_enabled", config.spiral_enabled);
    builder.add_double("appearance.spiral_frequency", config.spiral_frequency);
    builder.add_integer("appearance.spiral_arms", config.spiral_arms);
    builder.add_bool("appearance.wall_reflection_enabled", config.wall_reflection_enabled);
    builder.add_double("appearance.wall_frequency", config.wall_frequency);
    builder.add_double("appearance.wall_mix", config.wall_mix);
    builder.add_integer("appearance.hue_cycles", config.hue_cycles);
    builder.add_double("appearance.saturation", config.saturation);

    builder.add_bool("alpha.enabled", config.alpha.enabled);
    builder.add_double("alpha.minimum", config.alpha.minimum);
    builder.add_double("alpha.maximum", config.alpha.maximum);
    builder.add_double("alpha.spatial_frequency", config.alpha.spatial_frequency);
    builder.add_integer("alpha.cycles_per_loop", config.alpha.cycles_per_loop);
    builder.add_double("alpha.phase_degrees", config.alpha.phase_degrees);

    builder.add_bool("quantization.enabled", config.quantization.enabled);
    builder.add_integer("quantization.levels", config.quantization.levels);
    builder.add_double("quantization.mix", config.quantization.mix);
    builder.add_enum("quantization.mode", config.quantization.mode, kQuantizationModes);

    builder.add_bool("surface.enabled", config.surface.enabled);
    builder.add_enum("surface.mapping", config.surface.mapping, kSurfaceMappings);
    builder.add_integer("surface.rotations_per_loop", config.surface.rotations_per_loop);
    builder.add_double("surface.phase_degrees", config.surface.phase_degrees);
    builder.add_double("surface.curvature", config.surface.curvature);
    builder.add_double("surface.lighting", config.surface.lighting);
    builder.add_string("surface.obj_path", config.surface.obj_path);
    builder.add_string("surface.obj_sha256", config.surface.obj_sha256);
    builder.add_string("surface.obj_basename", config.surface.obj_basename);

    builder.add_bool("palette.enabled", config.palette.enabled);
    builder.add_string("palette.name", config.palette.name);
    builder.add_integer("palette.colors.count", config.palette.colors.size());
    for (std::size_t index = 0; index < config.palette.colors.size(); ++index) {
        const PaletteColor& color = config.palette.colors[index];
        builder.add_double(indexed_key("palette.colors", index, "red"), color.red);
        builder.add_double(indexed_key("palette.colors", index, "green"), color.green);
        builder.add_double(indexed_key("palette.colors", index, "blue"), color.blue);
    }

    builder.add_bool("transform.flip_horizontal", config.transform.flip_horizontal);
    builder.add_bool("transform.flip_vertical", config.transform.flip_vertical);
    builder.add_enum("transform.mirror", config.transform.mirror, kMirrorModes);

    builder.add_integer("output.bit_depth", config.output.bit_depth);
    builder.add_integer("output.png_compression_level",
                        config.output.png_compression_level);
    builder.add_bool("output.dither_enabled",
                     config.output.bit_depth != 32 && config.output.dither_enabled);
    builder.add_enum("output.dither_method", config.output.dither_method, kDitherMethods);
    builder.add_bool("output.write_alpha", config.output.write_alpha);
    builder.add_string("output.output_directory", config.output.output_directory);
    builder.add_string("output.filename_prefix", config.output.filename_prefix);
    builder.add_integer("output.first_frame_number", config.output.first_frame_number);
    builder.add_integer("output.filename_digits", config.output.filename_digits);
    builder.add_bool("output.overwrite_existing", config.output.overwrite_existing);

    if (!builder.ok()) {
        return false;
    }
    serialized = builder.contents();
    return true;
}

bool deserialize_setup(Records& records,
                       std::uint32_t setup_version,
                       RenderConfig& candidate,
                       std::string* error) {
    if (!consume_integer(records, "canvas.width", candidate.width, error)
        || !consume_integer(records, "canvas.height", candidate.height, error)
        || !consume_integer(records, "canvas.block_size", candidate.block_size, error)
        || !consume_integer(records, "timing.total_frames", candidate.total_frames, error)
        || !consume_double(records, "timing.fps", candidate.fps, error)) {
        return false;
    }

    if (setup_version >= 5U) {
        MusicAnalysis& music = candidate.clock.music;
        if (!consume_enum(records, "timing.clock.mode", candidate.clock.mode,
                          kClockModes, error)
            || !consume_enum(records, "timing.clock.interpolation",
                             candidate.clock.interpolation,
                             kClockInterpolations, error)
            || !consume_enum(records, "timing.clock.fit", candidate.clock.fit,
                             kClockFits, error)
            || !consume_integer(records, "timing.clock.frame_interval",
                                candidate.clock.frame_interval, error)
            || !consume_integer(records, "timing.clock.time_interval_microseconds",
                                candidate.clock.time_interval_microseconds, error)
            || !consume_bounded_string(records, "timing.clock.meter.expression",
                                       kMaximumMeterExpressionBytes,
                                       candidate.clock.meter.expression, error)
            || !consume_double(records, "timing.clock.meter.bpm",
                               candidate.clock.meter.bpm, error)
            || !consume_integer(records,
                                "timing.clock.meter.tempo_note_denominator",
                                candidate.clock.meter.tempo_note_denominator,
                                error)
            || !consume_enum(records, "timing.clock.music_tempo",
                             candidate.clock.music_tempo,
                             kMusicTempoModes, error)
            || !consume_enum(records, "timing.clock.music_swing_policy",
                             candidate.clock.music_swing_policy,
                             kMusicSwingPolicies, error)
            || !consume_integer(records, "timing.clock.beat_offset_microseconds",
                                candidate.clock.beat_offset_microseconds, error)
            || !consume_double(records, "timing.clock.phase_offset_degrees",
                               candidate.clock.phase_offset_degrees, error)
            || !consume_bool(records, "timing.clock.reverse",
                             candidate.clock.reverse, error)
            || !consume_integer(records, "timing.music.schema_version",
                                music.schema_version, error)
            || !consume_bounded_string(records, "timing.music.analyzer_version",
                                       kMaximumAnalyzerVersionBytes,
                                       music.analyzer_version, error)
            || !consume_bounded_string(records, "timing.music.source_sha256",
                                       kSha256HexBytes,
                                       music.source_sha256, error)
            || !consume_bounded_string(records, "timing.music.source_basename",
                                       kMaximumMusicBasenameBytes,
                                       music.source_basename, error)
            || !consume_bounded_string(records, "timing.music.source_format",
                                       kMaximumMusicFormatBytes,
                                       music.source_format, error)
            || !consume_integer(records, "timing.music.source_frame_count",
                                music.source_frame_count, error)
            || !consume_integer(records, "timing.music.source_sample_rate",
                                music.source_sample_rate, error)
            || !consume_integer(records, "timing.music.source_channel_count",
                                music.source_channel_count, error)
            || !consume_double(records, "timing.music.duration_seconds",
                               music.duration_seconds, error)
            || !consume_double(records, "timing.music.detected_bpm",
                               music.detected_bpm, error)
            || !consume_double(records, "timing.music.tempo_confidence",
                               music.tempo_confidence, error)) {
            return false;
        }
        if (!is_lowercase_sha256(music.source_sha256)) {
            return fail(error,
                        record_error("Invalid lowercase SHA-256 digest in setup key",
                                     "timing.music.source_sha256"));
        }

        std::size_t beat_count = 0U;
        if (!consume_count(records, "timing.music.beat_times.count",
                           kMaximumMusicBeats, beat_count, error)) {
            return false;
        }
        music.beat_times_seconds.clear();
        music.beat_times_seconds.resize(beat_count);
        for (std::size_t index = 0U; index < beat_count; ++index) {
            if (!consume_double(records,
                                indexed_key("timing.music.beat_times", index,
                                            "seconds"),
                                music.beat_times_seconds[index], error)) {
                return false;
            }
        }

        std::size_t tempo_count = 0U;
        if (!consume_count(records, "timing.music.tempo_points.count",
                           kMaximumMusicTempoPoints, tempo_count, error)) {
            return false;
        }
        music.tempo_points.clear();
        music.tempo_points.resize(tempo_count);
        for (std::size_t index = 0U; index < tempo_count; ++index) {
            MusicTempoPoint& point = music.tempo_points[index];
            if (!consume_double(records,
                                indexed_key("timing.music.tempo_points", index,
                                            "time_seconds"),
                                point.time_seconds, error)
                || !consume_double(records,
                                   indexed_key("timing.music.tempo_points", index,
                                               "bpm"),
                                   point.bpm, error)
                || !consume_double(records,
                                   indexed_key("timing.music.tempo_points", index,
                                               "confidence"),
                                   point.confidence, error)) {
                return false;
            }
        }

        std::size_t feature_count = 0U;
        if (!consume_count(records, "timing.music.feature_samples.count",
                           kMaximumMusicFeatureSamples, feature_count, error)) {
            return false;
        }
        music.feature_samples.clear();
        music.feature_samples.resize(feature_count);
        for (std::size_t index = 0U; index < feature_count; ++index) {
            MusicFeatureSample& sample = music.feature_samples[index];
            if (!consume_float(records,
                               indexed_key("timing.music.feature_samples", index,
                                           "energy"),
                               sample.energy, error)
                || !consume_float(records,
                                  indexed_key("timing.music.feature_samples", index,
                                              "bass"),
                                  sample.bass, error)
                || !consume_float(records,
                                  indexed_key("timing.music.feature_samples", index,
                                              "midrange"),
                                  sample.midrange, error)
                || !consume_float(records,
                                  indexed_key("timing.music.feature_samples", index,
                                              "treble"),
                                  sample.treble, error)
                || !consume_float(records,
                                  indexed_key("timing.music.feature_samples", index,
                                              "onset"),
                                  sample.onset, error)
                || !consume_float(records,
                                  indexed_key("timing.music.feature_samples", index,
                                              "beat"),
                                  sample.beat, error)
                || !consume_float(records,
                                  indexed_key("timing.music.feature_samples", index,
                                              "spectral_centroid"),
                                  sample.spectral_centroid, error)
                || !consume_float(records,
                                  indexed_key("timing.music.feature_samples", index,
                                              "spectral_flatness"),
                                  sample.spectral_flatness, error)
                || !consume_float(records,
                                  indexed_key("timing.music.feature_samples", index,
                                              "chroma_hue"),
                                  sample.chroma_hue, error)
                || !consume_float(records,
                                  indexed_key("timing.music.feature_samples", index,
                                              "chroma_strength"),
                                  sample.chroma_strength, error)) {
                return false;
            }
        }
    }

    std::size_t wave_count = 0;
    if (!consume_count(records, "waves.count", kMaximumWaves, wave_count, error)) {
        return false;
    }
    candidate.waves.clear();
    candidate.waves.resize(wave_count);
    for (std::size_t index = 0; index < wave_count; ++index) {
        WaveConfig& wave = candidate.waves[index];
        if (!consume_integer(records, indexed_key("waves", index, "id"), wave.id, error)
            || !consume_string(records, indexed_key("waves", index, "name"), wave.name, error)
            || !consume_bool(records, indexed_key("waves", index, "enabled"), wave.enabled, error)
            || !consume_bool(records, indexed_key("waves", index, "synchronized"), wave.synchronized, error)
            || !consume_double(records, indexed_key("waves", index, "x_percent"), wave.x_percent, error)
            || !consume_double(records, indexed_key("waves", index, "y_percent"), wave.y_percent, error)
            || !consume_double(records, indexed_key("waves", index, "amplitude"), wave.amplitude, error)
            || !consume_double(records, indexed_key("waves", index, "spatial_frequency"), wave.spatial_frequency, error)
            || !consume_integer(records, indexed_key("waves", index, "cycles_per_loop"), wave.cycles_per_loop, error)
            || !consume_double(records, indexed_key("waves", index, "phase_degrees"), wave.phase_degrees, error)
            || !consume_double(records, indexed_key("waves", index, "direction"), wave.direction, error)) {
            return false;
        }
    }

    std::size_t swing_count = 0;
    if (!consume_count(records, "swings.count", kMaximumSwings, swing_count, error)) {
        return false;
    }
    candidate.swings.clear();
    candidate.swings.resize(swing_count);
    for (std::size_t index = 0; index < swing_count; ++index) {
        SwingConfig& swing = candidate.swings[index];
        if (!consume_integer(records, indexed_key("swings", index, "id"), swing.id, error)
            || !consume_string(records, indexed_key("swings", index, "name"), swing.name, error)
            || !consume_bool(records, indexed_key("swings", index, "enabled"), swing.enabled, error)
            || !consume_enum(records, indexed_key("swings", index, "waveform"), swing.waveform, kWaveforms, error)
            || !consume_double(records, indexed_key("swings", index, "amount"), swing.amount, error)
            || !consume_integer(records, indexed_key("swings", index, "cycles_per_loop"), swing.cycles_per_loop, error)
            || !consume_double(records, indexed_key("swings", index, "phase_degrees"), swing.phase_degrees, error)
            || !consume_double(records, indexed_key("swings", index, "shape"), swing.shape, error)) {
            return false;
        }
        if (setup_version >= 4U
            && (!consume_double(records, indexed_key("swings", index, "center_x"),
                                swing.center_x, error)
                || !consume_double(records, indexed_key("swings", index, "center_y"),
                                   swing.center_y, error)
                || !consume_double(records, indexed_key("swings", index, "radius"),
                                   swing.radius, error))) {
            return false;
        }
    }

    std::size_t effect_count = 0;
    if (!consume_count(records, "effects.count", kMaximumEffects, effect_count, error)) {
        return false;
    }
    candidate.effects.clear();
    candidate.effects.resize(effect_count);
    for (std::size_t index = 0; index < effect_count; ++index) {
        EffectConfig& effect = candidate.effects[index];
        if (!consume_integer(records, indexed_key("effects", index, "id"), effect.id, error)
            || !consume_string(records, indexed_key("effects", index, "name"), effect.name, error)
            || !consume_enum(records, indexed_key("effects", index, "type"), effect.type, kEffectTypes, error)) {
            return false;
        }
        if (setup_version >= 4U
            && !consume_enum(records, indexed_key("effects", index, "space"),
                             effect.space, kEffectSpaces, error)) {
            return false;
        }
        if (!consume_bool(records, indexed_key("effects", index, "enabled"), effect.enabled, error)
            || !consume_bool(records, indexed_key("effects", index, "synchronized"), effect.synchronized, error)
            || !consume_integer(records, indexed_key("effects", index, "cycles_per_loop"), effect.cycles_per_loop, error)
            || !consume_double(records, indexed_key("effects", index, "phase_degrees"), effect.phase_degrees, error)
            || !consume_enum(records, indexed_key("effects", index, "edge_mode"), effect.edge_mode, kEdgeModes, error)
            || !consume_double(records, indexed_key("effects", index, "intensity"), effect.intensity, error)
            || !consume_double(records, indexed_key("effects", index, "magnitude"), effect.magnitude, error)
            || !consume_double(records, indexed_key("effects", index, "frequency"), effect.frequency, error)
            || !consume_double(records, indexed_key("effects", index, "secondary"), effect.secondary, error)
            || !consume_double(records, indexed_key("effects", index, "center_x"), effect.center_x, error)
            || !consume_double(records, indexed_key("effects", index, "center_y"), effect.center_y, error)
            || !consume_double(records, indexed_key("effects", index, "angle_degrees"), effect.angle_degrees, error)
            || !consume_double(records, indexed_key("effects", index, "radius_pixels"), effect.radius_pixels, error)
            || !consume_double(records, indexed_key("effects", index, "threshold"), effect.threshold, error)
            || !consume_double(records, indexed_key("effects", index, "soft_knee"), effect.soft_knee, error)) {
            return false;
        }
        if (setup_version >= 4U
            && !consume_double(records, indexed_key("effects", index, "area_radius"),
                               effect.area_radius, error)) {
            return false;
        }
    }

    if (setup_version >= 5U
        && (!consume_bool(records, "rhythm.swings_enabled",
                          candidate.swings_enabled, error)
            || !consume_bool(records, "audio_reactive.enabled",
                             candidate.audio_reactive.enabled, error)
            || !consume_bool(records, "audio_reactive.synchronized_only",
                             candidate.audio_reactive.synchronized_only, error)
            || !consume_bool(records, "audio_reactive.waves_enabled",
                             candidate.audio_reactive.waves_enabled, error)
            || !consume_enum(records, "audio_reactive.wave_source",
                             candidate.audio_reactive.wave_source,
                             kMusicFeatures, error)
            || !consume_double(records, "audio_reactive.wave_amount",
                               candidate.audio_reactive.wave_amount, error)
            || !consume_bool(records, "audio_reactive.effects_enabled",
                             candidate.audio_reactive.effects_enabled, error)
            || !consume_enum(records, "audio_reactive.effect_source",
                             candidate.audio_reactive.effect_source,
                             kMusicFeatures, error)
            || !consume_double(records, "audio_reactive.effect_amount",
                               candidate.audio_reactive.effect_amount, error)
            || !consume_bool(records, "audio_reactive.color_enabled",
                             candidate.audio_reactive.color_enabled, error)
            || !consume_enum(records, "audio_reactive.color_source",
                             candidate.audio_reactive.color_source,
                             kMusicFeatures, error)
            || !consume_double(records, "audio_reactive.color_amount_degrees",
                               candidate.audio_reactive.color_amount_degrees,
                               error))) {
        return false;
    }

    if (!consume_double(records, "rhythm.phrase_warp", candidate.phrase_warp, error)
        || !consume_double(records, "rhythm.ghost_mix", candidate.ghost_mix, error)
        || !consume_double(records, "rhythm.ghost_lag_degrees", candidate.ghost_lag_degrees, error)
        || !consume_bool(records, "appearance.displacement_enabled", candidate.displacement_enabled, error)
        || !consume_double(records, "appearance.displacement", candidate.displacement, error)
        || !consume_bool(records, "appearance.lighting_enabled", candidate.lighting_enabled, error)
        || !consume_double(records, "appearance.wave_depth", candidate.wave_depth, error)
        || !consume_bool(records, "appearance.spiral_enabled", candidate.spiral_enabled, error)
        || !consume_double(records, "appearance.spiral_frequency", candidate.spiral_frequency, error)
        || !consume_integer(records, "appearance.spiral_arms", candidate.spiral_arms, error)
        || !consume_bool(records, "appearance.wall_reflection_enabled", candidate.wall_reflection_enabled, error)
        || !consume_double(records, "appearance.wall_frequency", candidate.wall_frequency, error)
        || !consume_double(records, "appearance.wall_mix", candidate.wall_mix, error)
        || !consume_integer(records, "appearance.hue_cycles", candidate.hue_cycles, error)
        || !consume_double(records, "appearance.saturation", candidate.saturation, error)) {
        return false;
    }

    if (!consume_bool(records, "alpha.enabled", candidate.alpha.enabled, error)
        || !consume_double(records, "alpha.minimum", candidate.alpha.minimum, error)
        || !consume_double(records, "alpha.maximum", candidate.alpha.maximum, error)
        || !consume_double(records, "alpha.spatial_frequency", candidate.alpha.spatial_frequency, error)
        || !consume_integer(records, "alpha.cycles_per_loop", candidate.alpha.cycles_per_loop, error)
        || !consume_double(records, "alpha.phase_degrees", candidate.alpha.phase_degrees, error)
        || !consume_bool(records, "quantization.enabled", candidate.quantization.enabled, error)
        || !consume_integer(records, "quantization.levels", candidate.quantization.levels, error)
        || !consume_double(records, "quantization.mix", candidate.quantization.mix, error)
        || !consume_enum(records, "quantization.mode", candidate.quantization.mode, kQuantizationModes, error)
        || !consume_bool(records, "surface.enabled", candidate.surface.enabled, error)
        || !consume_enum(records, "surface.mapping", candidate.surface.mapping, kSurfaceMappings, error)
        || !consume_integer(records, "surface.rotations_per_loop", candidate.surface.rotations_per_loop, error)
        || !consume_double(records, "surface.phase_degrees", candidate.surface.phase_degrees, error)
        || !consume_double(records, "surface.curvature", candidate.surface.curvature, error)
        || !consume_double(records, "surface.lighting", candidate.surface.lighting, error)) {
        return false;
    }
    if (setup_version >= 2U
        && !consume_string(records, "surface.obj_path",
                           candidate.surface.obj_path, error)) {
        return false;
    }
    if (setup_version >= 5U) {
        if (!consume_bounded_string(records, "surface.obj_sha256",
                                    kSha256HexBytes,
                                    candidate.surface.obj_sha256, error)
            || !consume_bounded_string(records, "surface.obj_basename",
                                       kMaximumAttachmentBasenameBytes,
                                       candidate.surface.obj_basename, error)
            || !is_lowercase_sha256(candidate.surface.obj_sha256)) {
            return fail(error,
                        record_error("Invalid custom OBJ attachment metadata at setup key",
                                     "surface.obj_sha256"));
        }
    }

    if (setup_version >= 4U) {
        std::size_t palette_color_count = 0U;
        if (!consume_bool(records, "palette.enabled", candidate.palette.enabled, error)
            || !consume_string(records, "palette.name", candidate.palette.name, error)
            || !consume_count(records, "palette.colors.count",
                              kMaximumPaletteColors, palette_color_count, error)) {
            return false;
        }
        candidate.palette.colors.clear();
        candidate.palette.colors.resize(palette_color_count);
        for (std::size_t index = 0U; index < palette_color_count; ++index) {
            PaletteColor& color = candidate.palette.colors[index];
            if (!consume_double(records, indexed_key("palette.colors", index, "red"),
                                color.red, error)
                || !consume_double(records, indexed_key("palette.colors", index, "green"),
                                   color.green, error)
                || !consume_double(records, indexed_key("palette.colors", index, "blue"),
                                   color.blue, error)) {
                return false;
            }
        }
        if (!consume_bool(records, "transform.flip_horizontal",
                          candidate.transform.flip_horizontal, error)
            || !consume_bool(records, "transform.flip_vertical",
                             candidate.transform.flip_vertical, error)
            || !consume_enum(records, "transform.mirror",
                             candidate.transform.mirror, kMirrorModes, error)) {
            return false;
        }
    }

    if (!consume_integer(records, "output.bit_depth", candidate.output.bit_depth, error)) {
        return false;
    }
    if (setup_version >= 2U
        && !consume_integer(records, "output.png_compression_level",
                            candidate.output.png_compression_level, error)) {
        return false;
    }
    if (!consume_bool(records, "output.dither_enabled", candidate.output.dither_enabled, error)
        || !consume_enum(records, "output.dither_method", candidate.output.dither_method, kDitherMethods, error)) {
        return false;
    }
    if (setup_version >= 3U) {
        if (!consume_bool(records, "output.write_alpha",
                          candidate.output.write_alpha, error)) {
            return false;
        }
    } else {
        candidate.output.write_alpha = candidate.alpha.enabled;
    }
    if (!consume_string(records, "output.output_directory", candidate.output.output_directory, error)
        || !consume_string(records, "output.filename_prefix", candidate.output.filename_prefix, error)
        || !consume_integer(records, "output.first_frame_number", candidate.output.first_frame_number, error)
        || !consume_integer(records, "output.filename_digits", candidate.output.filename_digits, error)
        || !consume_bool(records, "output.overwrite_existing", candidate.output.overwrite_existing, error)) {
        return false;
    }

    if (!records.empty()) {
        return fail(error, record_error("Unknown setup key for format v"
                                        + std::to_string(setup_version),
                                        records.begin()->first));
    }

    // Format v1 files written by early builds could retain a now-meaningless
    // dither toggle while selecting full-float EXR. Accept and normalize those
    // files; no integer quantization occurs at 32-bit output.
    if (candidate.output.bit_depth == 32) {
        candidate.output.dither_enabled = false;
    }

    const ValidationResult validation = validate(candidate);
    if (!validation.ok) {
        return fail(error, "Loaded setup failed validation: " + validation.message);
    }
    return true;
}

#if defined(_WIN32)

std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                              utf8.data(), static_cast<int>(utf8.size()),
                                              nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            utf8.data(), static_cast<int>(utf8.size()),
                            wide.data(), required) != required) {
        return {};
    }
    return wide;
}

std::string windows_error_message(std::string_view operation, DWORD code) {
    return std::string(operation) + " failed (Windows error " + std::to_string(code) + ").";
}

bool atomic_write_setup(const std::string& path,
                        const std::string& contents,
                        std::string* error) {
    const std::wstring destination = utf8_to_wide(path);
    if (destination.empty()) {
        return fail(error, "Setup path is not valid UTF-8 for Windows.");
    }

    const std::filesystem::path destination_path(destination);
    const std::filesystem::path filename = destination_path.filename();
    if (filename.empty() || filename == L"." || filename == L"..") {
        return fail(error, "Setup destination must name a file.");
    }
    std::filesystem::path directory = destination_path.parent_path();
    if (directory.empty()) {
        directory = L".";
    }

    static std::atomic_uint64_t sequence{0};
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::filesystem::path temporary_path;
    // Reserve a disjoint attempt range for each concurrent save. This avoids
    // needless CREATE_NEW collisions when stale temporary files are present.
    const std::uint64_t first = sequence.fetch_add(128U, std::memory_order_relaxed);
    for (unsigned int attempt = 0; attempt < 128U; ++attempt) {
        const std::uint64_t serial = first + static_cast<std::uint64_t>(attempt);
        const std::wstring temporary_name = L".pvt-setup-" + std::to_wstring(GetCurrentProcessId())
                                            + L"-" + std::to_wstring(serial) + L".tmp";
        temporary_path = directory / temporary_name;
        handle = CreateFileW(temporary_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            break;
        }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) {
            return fail(error, windows_error_message("Creating sibling temporary setup file", GetLastError()));
        }
    }
    if (handle == INVALID_HANDLE_VALUE) {
        return fail(error, "Could not allocate a unique sibling temporary setup file.");
    }

    bool success = true;
    DWORD failure = ERROR_SUCCESS;
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min)(
            contents.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!WriteFile(handle, contents.data() + offset, chunk, &written, nullptr)
            || written == 0U) {
            success = false;
            failure = GetLastError();
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (success && !FlushFileBuffers(handle)) {
        success = false;
        failure = GetLastError();
    }
    if (!CloseHandle(handle) && success) {
        success = false;
        failure = GetLastError();
    }
    if (!success) {
        DeleteFileW(temporary_path.c_str());
        return fail(error, windows_error_message("Writing temporary setup file", failure));
    }

    // FILE_ATTRIBUTE_TEMPORARY is useful while writing, but the installed setup
    // is permanent. Clear the hint before the atomic move so it cannot persist
    // on the destination and affect caching, indexing, or backup behavior.
    if (!SetFileAttributesW(temporary_path.c_str(), FILE_ATTRIBUTE_NORMAL)) {
        failure = GetLastError();
        DeleteFileW(temporary_path.c_str());
        return fail(error, windows_error_message(
                               "Preparing temporary setup file for installation", failure));
    }

    if (!detail::install_windows_temporary(temporary_path, destination_path,
                                           true, &failure)) {
        DeleteFileW(temporary_path.c_str());
        return fail(error, windows_error_message("Atomically replacing setup file", failure));
    }
    return true;
}

#else

class TemporaryFileGuard {
public:
    explicit TemporaryFileGuard(std::string path) : path_(std::move(path)) {}
    ~TemporaryFileGuard() {
        if (armed_) {
            ::unlink(path_.c_str());
        }
    }
    void release() { armed_ = false; }

private:
    std::string path_;
    bool armed_ = true;
};

std::string posix_error_message(std::string_view operation, int code) {
    return std::string(operation) + ": " + std::generic_category().message(code) + ".";
}

int fsync_retry(int descriptor) {
    int result = 0;
    do {
        result = ::fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    return result;
}

void sync_directory_best_effort(const std::filesystem::path& directory) {
#  if defined(O_DIRECTORY)
    constexpr int directory_flag = O_DIRECTORY;
#  else
    constexpr int directory_flag = 0;
#  endif
    const std::string native_directory = directory.string();
    const int descriptor = ::open(native_directory.c_str(), O_RDONLY | directory_flag);
    if (descriptor >= 0) {
        (void)fsync_retry(descriptor);
        (void)::close(descriptor);
    }
}

bool atomic_write_setup(const std::string& path,
                        const std::string& contents,
                        std::string* error) {
    const std::filesystem::path destination = detail::path_from_utf8(path);
    const std::filesystem::path filename = destination.filename();
    if (filename.empty() || filename == "." || filename == "..") {
        return fail(error, "Setup destination must name a file.");
    }
    std::filesystem::path directory = destination.parent_path();
    if (directory.empty()) {
        directory = ".";
    }

    // mkstemp intentionally creates new files with mode 0600. When replacing
    // an existing regular file, retain that file's explicit permission bits so
    // a harmless setup edit does not silently make a shared configuration
    // private. Symlinks and other special entries are never followed here.
    mode_t preserved_mode = 0;
    bool preserve_mode = false;
    struct stat destination_status {};
    const std::string native_destination = destination.string();
    if (::lstat(native_destination.c_str(), &destination_status) == 0) {
        if (S_ISDIR(destination_status.st_mode)) {
            return fail(error, "Setup destination is a directory.");
        }
        if (S_ISREG(destination_status.st_mode)) {
            preserved_mode = destination_status.st_mode & 07777;
            preserve_mode = true;
        }
    } else if (errno != ENOENT) {
        return fail(error, posix_error_message("Could not inspect setup destination", errno));
    }

    std::filesystem::path template_path = directory / ".pvt-setup-XXXXXX";
    std::string temporary = template_path.string();
    std::vector<char> mutable_template(temporary.begin(), temporary.end());
    mutable_template.push_back('\0');

    const int descriptor = ::mkstemp(mutable_template.data());
    if (descriptor < 0) {
        return fail(error, posix_error_message("Could not create sibling temporary setup file", errno));
    }
    temporary.assign(mutable_template.data());
    TemporaryFileGuard cleanup(temporary);

    bool success = true;
    int failure = 0;
    std::size_t offset = 0;
    while (offset < contents.size()) {
        ssize_t written = 0;
        do {
            written = ::write(descriptor, contents.data() + offset,
                              contents.size() - offset);
        } while (written < 0 && errno == EINTR);
        if (written <= 0) {
            success = false;
            failure = written < 0 && errno != 0 ? errno : EIO;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    // Apply the final mode after writing: POSIX may clear set-user-ID or
    // set-group-ID bits when file contents change.
    if (success && preserve_mode && ::fchmod(descriptor, preserved_mode) != 0) {
        success = false;
        failure = errno;
    }
    if (success && fsync_retry(descriptor) != 0) {
        success = false;
        failure = errno;
    }
    if (::close(descriptor) != 0 && success) {
        success = false;
        failure = errno;
    }
    if (!success) {
        return fail(error,
                    posix_error_message("Could not write and flush temporary setup file",
                                        failure));
    }

    if (::rename(temporary.c_str(), destination.string().c_str()) != 0) {
        return fail(error, posix_error_message("Could not atomically replace setup file", errno));
    }
    cleanup.release();
    sync_directory_best_effort(directory);
    return true;
}

#endif

} // namespace

namespace detail {

bool serialize_setup_config(const RenderConfig& config,
                            std::string& serialized,
                            std::string* error) {
    clear_error(error);
    try {
        return serialize_setup(config, serialized, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to serialize setup.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected error while serializing setup: ")
                               + exception.what());
    }
}

bool deserialize_setup_config(const std::string& serialized,
                              RenderConfig& destination,
                              std::string* error) {
    clear_error(error);
    try {
        if (serialized.size() > kMaximumSetupBytes) {
            return fail(error, "Setup data exceeds the 8 MiB input limit.");
        }
        Records records;
        std::uint32_t setup_version = 0U;
        if (!parse_records(serialized, records, setup_version, error)) {
            return false;
        }
        RenderConfig candidate;
        if (!deserialize_setup(records, setup_version, candidate, error)) {
            return false;
        }
        destination = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error,
                    "Not enough memory to load setup; destination was not changed.");
    } catch (const std::exception& exception) {
        return fail(error,
                    std::string("Unexpected error while loading setup; destination was not changed: ")
                        + exception.what());
    }
}

} // namespace detail

bool save_setup(const RenderConfig& config,
                const std::string& path,
                std::string* error) {
    clear_error(error);
    try {
        if (path.empty() || path.size() > kMaximumDecodedStringBytes
            || path.find('\0') != std::string::npos) {
            return fail(error, "Setup path is empty, contains a NUL byte, or exceeds 64 KiB.");
        }
        std::string serialized;
        if (!detail::serialize_setup_config(config, serialized, error)) {
            return false;
        }
        return atomic_write_setup(path, serialized, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to save setup.");
    } catch (const std::filesystem::filesystem_error& exception) {
        return fail(error, std::string("Filesystem error while saving setup: ") + exception.what());
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected error while saving setup: ") + exception.what());
    }
}

bool load_setup(const std::string& path,
                RenderConfig& destination,
                std::string* error) {
    clear_error(error);
    try {
        std::string contents;
        if (!read_setup_file(path, contents, error)) {
            return false;
        }

        RenderConfig candidate;
        if (!detail::deserialize_setup_config(contents, candidate, error)) {
            return false;
        }

        // All potentially failing parsing, allocation, and validation occurs
        // above. Standard-allocator vector/string moves make this commit step
        // non-throwing in the supported C++17 implementations.
        destination = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to load setup; destination was not changed.");
    } catch (const std::filesystem::filesystem_error& exception) {
        return fail(error, std::string("Filesystem error while loading setup; destination was not changed: ")
                           + exception.what());
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected error while loading setup; destination was not changed: ")
                           + exception.what());
    }
}

const char* clock_mode_name(ClockMode value) {
    switch (value) {
        case ClockMode::Default: return "Default";
        case ClockMode::Frame: return "Frame";
        case ClockMode::Time: return "Time";
        case ClockMode::Meter: return "Time signature";
        case ClockMode::Music: return "Music";
    }
    return "Unknown";
}

const char* clock_interpolation_name(ClockInterpolation value) {
    switch (value) {
        case ClockInterpolation::Hold: return "Hold";
        case ClockInterpolation::Linear: return "Linear";
        case ClockInterpolation::Smoothstep: return "Smoothstep";
    }
    return "Unknown";
}

const char* clock_fit_name(ClockFit value) {
    switch (value) {
        case ClockFit::Exact: return "Exact interval";
        case ClockFit::FitSequence: return "Fit sequence";
    }
    return "Unknown";
}

const char* music_tempo_mode_name(MusicTempoMode value) {
    switch (value) {
        case MusicTempoMode::Half: return "Half tempo";
        case MusicTempoMode::Detected: return "Detected tempo";
        case MusicTempoMode::Double: return "Double tempo";
    }
    return "Unknown";
}

const char* music_feature_name(MusicFeature value) {
    switch (value) {
        case MusicFeature::Energy: return "Energy";
        case MusicFeature::Bass: return "Bass";
        case MusicFeature::Midrange: return "Midrange";
        case MusicFeature::Treble: return "Treble";
        case MusicFeature::Onset: return "Onset";
        case MusicFeature::Beat: return "Beat";
        case MusicFeature::SpectralCentroid: return "Spectral brightness";
        case MusicFeature::SpectralFlatness: return "Spectral noisiness";
        case MusicFeature::ChromaHue: return "Pitch color (tonality-weighted)";
        case MusicFeature::ChromaStrength: return "Tonal strength";
    }
    return "Unknown";
}

const char* music_swing_policy_name(MusicSwingPolicy value) {
    switch (value) {
        case MusicSwingPolicy::SuppressAll: return "Disable all swings";
        case MusicSwingPolicy::SuppressGlobal: return "Disable global swings";
        case MusicSwingPolicy::KeepAll: return "Keep all swings";
    }
    return "Unknown";
}

} // namespace pvt
