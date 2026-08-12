#include "project_bundle.h"

#include "audio_analysis.h"
#include "bundle_archive.h"
#include "config_codec.h"
#include "path_utf8.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#ifndef PVT_PROGRAM_VERSION
#  define PVT_PROGRAM_VERSION "6.0.0"
#endif

namespace pvt {

struct ProjectAttachmentCache {
    std::string directory;
    std::map<std::string, std::string> materialized_by_identity;
    // ProjectDocument snapshots deliberately share this immutable-byte cache.
    // Attachment vectors remain copy-on-write at the document level, while
    // materialization may happen on a worker during a GUI import transaction.
    mutable std::mutex mutex;
    ~ProjectAttachmentCache();
};

namespace {

namespace fs = std::filesystem;
using Records = std::map<std::string, std::string>;

constexpr std::size_t kMaximumMetadataBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumMetadataRecords = 32768U;
constexpr std::size_t kMaximumVersions = 4096U;
constexpr std::size_t kMaximumLineageAliases = 8192U;
constexpr std::size_t kMaximumProjectNameBytes = 256U;
constexpr std::size_t kMaximumPortableRootBytes = 240U;
constexpr std::uint32_t kProjectVersionFormatVersion = 4U;

struct RootMetadata {
    struct PreservedVersion {
        std::string observed_metadata_digest;
        std::string tree_digest;
        std::set<std::string> lineage_aliases;
    };

    std::string project_uuid;
    std::string project_name;
    std::string first_created_utc;
    std::string last_opened_utc;
    std::string last_saved_utc;
    std::string created_with_version;
    std::string last_changed_with_version;
    std::map<std::uint64_t, std::string> version_digests;
    std::map<std::uint64_t, std::string> version_tree_digests;
    // Parent-digest identities retained after an externally deleted history
    // directory can no longer be preserved as raw data.
    std::set<std::string> lineage_aliases;
    // Numeric directories that are intentionally retained byte-for-byte but
    // are not canonical, loadable project versions. Recording their exact raw
    // tree keeps malformed or unrelated external history visible and
    // verifiable without discarding it.
    std::map<std::uint64_t, PreservedVersion> preserved_versions;
};

struct VersionManifest {
    std::uint32_t format_version = kProjectVersionFormatVersion;
    BundleVersionInfo info;
    std::string project_name;
    std::string render_output_digest;
    std::string music_analysis_digest;
    std::vector<LayerConfig> layers;
    std::vector<std::string> layer_digests;
    std::vector<ProjectAttachment> attachments;
    std::string reverted_from_digest;
};

bool sync_project_attachment_references(ProjectDocument& document,
                                        std::string* error);

bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

void clear_error(std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
}

bool valid_key(std::string_view key) {
    if (key.empty() || key.size() > 128U) {
        return false;
    }
    for (const char character : key) {
        if ((character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9')
            || character == '.' || character == '_') {
            continue;
        }
        return false;
    }
    return true;
}

bool unreserved(unsigned char character) {
    return (character >= 'a' && character <= 'z')
           || (character >= 'A' && character <= 'Z')
           || (character >= '0' && character <= '9')
           || character == '-' || character == '.' || character == '_'
           || character == '~';
}

bool percent_encode(std::string_view value, std::string& encoded) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::size_t encoded_size = 0U;
    for (const char raw : value) {
        const std::size_t addition =
            unreserved(static_cast<unsigned char>(raw)) ? 1U : 3U;
        if (encoded_size > kMaximumMetadataBytes - addition) return false;
        encoded_size += addition;
    }
    encoded.clear();
    encoded.reserve(encoded_size);
    for (const char raw : value) {
        const unsigned char character = static_cast<unsigned char>(raw);
        if (unreserved(character)) {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('%');
            encoded.push_back(digits[character >> 4U]);
            encoded.push_back(digits[character & 0x0fU]);
        }
    }
    return true;
}

int hex_digit(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

bool percent_decode(std::string_view value, std::string& decoded) {
    decoded.clear();
    decoded.reserve(value.size());
    for (std::size_t index = 0U; index < value.size();) {
        const unsigned char character = static_cast<unsigned char>(value[index]);
        if (character == '%') {
            if (index + 2U >= value.size()) return false;
            const int high = hex_digit(value[index + 1U]);
            const int low = hex_digit(value[index + 2U]);
            if (high < 0 || low < 0) return false;
            const unsigned char decoded_character =
                static_cast<unsigned char>((high << 4) | low);
            if (unreserved(decoded_character)) return false; // canonical encoding only
            decoded.push_back(static_cast<char>(decoded_character));
            index += 3U;
        } else {
            if (!unreserved(character)) return false;
            decoded.push_back(static_cast<char>(character));
            ++index;
        }
    }
    return true;
}

class TextBuilder {
public:
    TextBuilder(std::string_view kind, std::uint32_t version) {
        bytes_.append(kind);
        bytes_.push_back('\t');
        bytes_.append(std::to_string(version));
        bytes_.push_back('\n');
    }

    bool add(std::string_view key, std::string value) {
        if (!valid_key(key) || value.find('\n') != std::string::npos
            || value.find('\r') != std::string::npos
            || value.find('\t') != std::string::npos) {
            ok_ = false;
            return false;
        }
        if (key.size() > kMaximumMetadataBytes
            || value.size() > kMaximumMetadataBytes - key.size()
            || key.size() + value.size() > kMaximumMetadataBytes - 2U) {
            ok_ = false;
            return false;
        }
        const std::size_t addition = key.size() + value.size() + 2U;
        if (bytes_.size() > kMaximumMetadataBytes - addition) {
            ok_ = false;
            return false;
        }
        bytes_.append(key);
        bytes_.push_back('\t');
        bytes_.append(value);
        bytes_.push_back('\n');
        return true;
    }

    bool string(std::string_view key, std::string_view value) {
        std::string encoded;
        if (!percent_encode(value, encoded)) {
            ok_ = false;
            return false;
        }
        return add(key, std::move(encoded));
    }

    template <typename Integer>
    bool integer(std::string_view key, Integer value) {
        std::array<char, 64U> buffer{};
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                          value, 10);
        return result.ec == std::errc{}
               && add(key, std::string(buffer.data(), result.ptr));
    }

    bool real(std::string_view key, double value) {
        if (!std::isfinite(value)) {
            ok_ = false;
            return false;
        }
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(std::numeric_limits<double>::max_digits10)
               << value;
        return stream && add(key, stream.str());
    }

    bool boolean(std::string_view key, bool value) {
        return add(key, value ? "1" : "0");
    }

    bool ok() const { return ok_; }
    const std::string& bytes() const { return bytes_; }

private:
    std::string bytes_;
    bool ok_ = true;
};

bool parse_text(const std::string& bytes,
                std::string_view expected_kind,
                std::uint32_t expected_version,
                Records& records,
                std::string* error) {
    if (bytes.empty() || bytes.size() > kMaximumMetadataBytes) {
        return fail(error, "Metadata is empty or exceeds the 4 MiB limit.");
    }
    const std::string header = std::string(expected_kind) + "\t"
                               + std::to_string(expected_version);
    std::size_t start = 0U;
    std::size_t line_number = 0U;
    while (start < bytes.size()) {
        const std::size_t newline = bytes.find('\n', start);
        const std::size_t end = newline == std::string::npos ? bytes.size() : newline;
        std::string_view line(bytes.data() + start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1U);
        ++line_number;
        if (line_number == 1U) {
            if (line != header) {
                return fail(error, "Unsupported or malformed metadata header.");
            }
        } else {
            const std::size_t tab = line.find('\t');
            if (line.empty() || tab == std::string_view::npos
                || line.find('\t', tab + 1U) != std::string_view::npos
                || !valid_key(line.substr(0U, tab))) {
                return fail(error, "Malformed metadata line "
                                       + std::to_string(line_number) + ".");
            }
            for (const char raw : line) {
                const unsigned char character = static_cast<unsigned char>(raw);
                if (character == '\t') continue;
                if (character < 0x20U || character > 0x7eU) {
                    return fail(error, "Metadata contains non-ASCII raw data.");
                }
            }
            if (records.size() >= kMaximumMetadataRecords
                || !records.emplace(std::string(line.substr(0U, tab)),
                                    std::string(line.substr(tab + 1U))).second) {
                return fail(error, "Metadata has too many or duplicate records.");
            }
        }
        if (newline == std::string::npos) break;
        start = newline + 1U;
    }
    return true;
}

bool take(Records& records, const std::string& key,
          std::string& value, std::string* error) {
    const auto found = records.find(key);
    if (found == records.end()) {
        return fail(error, "Missing metadata key '" + key + "'.");
    }
    value = std::move(found->second);
    records.erase(found);
    return true;
}

bool take_string(Records& records, const std::string& key,
                 std::string& value, std::string* error) {
    std::string encoded;
    return take(records, key, encoded, error)
           && (percent_decode(encoded, value)
               || fail(error, "Invalid encoded metadata string '" + key + "'."));
}

template <typename Integer>
bool take_integer(Records& records, const std::string& key,
                  Integer& value, std::string* error) {
    std::string text;
    if (!take(records, key, text, error) || text.empty()) return false;
    Integer candidate{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(),
                                        candidate, 10);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return fail(error, "Invalid integer metadata key '" + key + "'.");
    }
    value = candidate;
    return true;
}

bool take_bool(Records& records, const std::string& key,
               bool& value, std::string* error) {
    std::string text;
    if (!take(records, key, text, error)) return false;
    if (text == "0") value = false;
    else if (text == "1") value = true;
    else return fail(error, "Invalid Boolean metadata key '" + key + "'.");
    return true;
}

bool take_real(Records& records, const std::string& key,
               double& value, std::string* error) {
    std::string text;
    if (!take(records, key, text, error) || text.empty()) return false;
    std::istringstream stream(text);
    stream.imbue(std::locale::classic());
    double candidate = 0.0;
    stream >> std::noskipws >> candidate;
    if (!stream || stream.peek() != std::char_traits<char>::eof()
        || !std::isfinite(candidate)) {
        return fail(error, "Invalid real metadata key '" + key + "'.");
    }
    value = candidate;
    return true;
}

bool canonical_uuid(const std::string& uuid) {
    if (uuid.size() != 36U || uuid[8] != '-' || uuid[13] != '-'
        || uuid[18] != '-' || uuid[23] != '-') return false;
    for (std::size_t index = 0U; index < uuid.size(); ++index) {
        if (index == 8U || index == 13U || index == 18U || index == 23U) continue;
        const char character = uuid[index];
        if (!((character >= '0' && character <= '9')
              || (character >= 'a' && character <= 'f'))) return false;
    }
    return uuid[14] == '4'
           && (uuid[19] == '8' || uuid[19] == '9'
               || uuid[19] == 'a' || uuid[19] == 'b');
}

bool canonical_hash(const std::string& hash) {
    if (hash.size() != 64U) return false;
    for (const char character : hash) {
        if (!((character >= '0' && character <= '9')
              || (character >= 'a' && character <= 'f'))) return false;
    }
    return true;
}

bool canonical_timestamp(const std::string& timestamp) {
    if (timestamp.size() != 20U || timestamp[4] != '-' || timestamp[7] != '-'
        || timestamp[10] != 'T' || timestamp[13] != ':' || timestamp[16] != ':'
        || timestamp[19] != 'Z') return false;
    for (std::size_t index = 0U; index < timestamp.size(); ++index) {
        if (index == 4U || index == 7U || index == 10U || index == 13U
            || index == 16U || index == 19U) continue;
        if (timestamp[index] < '0' || timestamp[index] > '9') return false;
    }
    const auto number = [&timestamp](std::size_t at, std::size_t count) {
        int value = 0;
        for (std::size_t index = 0U; index < count; ++index) {
            value = value * 10 + (timestamp[at + index] - '0');
        }
        return value;
    };
    const int year = number(0U, 4U);
    const int month = number(5U, 2U);
    const int day = number(8U, 2U);
    const int hour = number(11U, 2U);
    const int minute = number(14U, 2U);
    const int second = number(17U, 2U);
    if (year < 1 || month < 1 || month > 12 || hour > 23
        || minute > 59 || second > 59) return false;
    constexpr std::array<int, 12U> month_days{{
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}};
    int maximum_day = month_days[static_cast<std::size_t>(month - 1)];
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (month == 2 && leap) ++maximum_day;
    return day >= 1 && day <= maximum_day;
}

bool parse_program_version(std::string_view text,
                           std::array<std::uint64_t, 3U>& parts) {
    std::size_t start = 0U;
    for (std::size_t index = 0U; index < parts.size(); ++index) {
        const std::size_t dot = text.find('.', start);
        std::size_t end = index + 1U == parts.size()
                              ? text.find_first_of("-+", start) : dot;
        if (index + 1U == parts.size() && end == std::string_view::npos) {
            end = text.size();
        }
        if (end == start
            || (index + 1U != parts.size() && dot == std::string_view::npos)) {
            return false;
        }
        const std::string_view component = text.substr(start, end - start);
        if (component.size() > 1U && component.front() == '0') return false;
        const auto parsed = std::from_chars(component.data(),
                                             component.data() + component.size(),
                                             parts[index], 10);
        if (parsed.ec != std::errc{}
            || parsed.ptr != component.data() + component.size()) return false;
        start = end + 1U;
    }
    return start == text.size() + 1U
           || (start <= text.size()
               && (text[start - 1U] == '-' || text[start - 1U] == '+'));
}

bool program_version_is_newer(std::string_view candidate) {
    std::array<std::uint64_t, 3U> candidate_parts{};
    std::array<std::uint64_t, 3U> current_parts{};
    if (!parse_program_version(candidate, candidate_parts)
        || !parse_program_version(PVT_PROGRAM_VERSION, current_parts)) return false;
    return candidate_parts > current_parts;
}

bool ascii_case_suffix(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size()) return false;
    const std::size_t offset = text.size() - suffix.size();
    for (std::size_t index = 0U; index < suffix.size(); ++index) {
        unsigned char left = static_cast<unsigned char>(text[offset + index]);
        unsigned char right = static_cast<unsigned char>(suffix[index]);
        if (left >= 'A' && left <= 'Z') left = static_cast<unsigned char>(left + 32U);
        if (right >= 'A' && right <= 'Z') right = static_cast<unsigned char>(right + 32U);
        if (left != right) return false;
    }
    return true;
}

std::string utc_now() {
    const std::time_t now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

std::string blend_token(BlendMode mode) {
    switch (mode) {
        case BlendMode::Normal: return "none";
        case BlendMode::SoftLight: return "soft_light";
        case BlendMode::GrainMerge: return "grain_merge";
        case BlendMode::Overlay: return "overlay";
        case BlendMode::ColorDodge: return "color_dodge";
        case BlendMode::LinearBurn: return "linear_burn";
        case BlendMode::ColorBurn: return "color_burn";
        case BlendMode::Difference: return "difference";
        case BlendMode::Subtract: return "subtract";
        case BlendMode::Multiply: return "multiply";
        case BlendMode::Add: return "add";
    }
    return {};
}

bool parse_blend(const std::string& token, BlendMode& mode) {
    constexpr std::array<std::pair<std::string_view, BlendMode>, 11U> values{{
        {"none", BlendMode::Normal}, {"soft_light", BlendMode::SoftLight},
        {"grain_merge", BlendMode::GrainMerge}, {"overlay", BlendMode::Overlay},
        {"color_dodge", BlendMode::ColorDodge}, {"linear_burn", BlendMode::LinearBurn},
        {"color_burn", BlendMode::ColorBurn}, {"difference", BlendMode::Difference},
        {"subtract", BlendMode::Subtract}, {"multiply", BlendMode::Multiply},
        {"add", BlendMode::Add},
    }};
    for (const auto& value : values) {
        if (token == value.first) {
            mode = value.second;
            return true;
        }
    }
    return false;
}

std::string indexed(std::string_view collection, std::size_t index,
                    std::string_view field) {
    return std::string(collection) + "." + std::to_string(index) + "."
           + std::string(field);
}

bool valid_semantic_project_name(const std::string& name) {
    if (name.empty() || name.size() > kMaximumProjectNameBytes
        || !detail::valid_utf8(name)) return false;
    for (std::size_t index = 0U; index < name.size();) {
        const unsigned char first = static_cast<unsigned char>(name[index]);
        std::uint32_t codepoint = first;
        std::size_t length = 1U;
        if (first >= 0xc2U && first <= 0xdfU) {
            codepoint = first & 0x1fU;
            length = 2U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            codepoint = first & 0x0fU;
            length = 3U;
        } else if (first >= 0xf0U) {
            codepoint = first & 0x07U;
            length = 4U;
        }
        for (std::size_t offset = 1U; offset < length; ++offset) {
            codepoint = (codepoint << 6U)
                        | (static_cast<unsigned char>(name[index + offset]) & 0x3fU);
        }
        if (codepoint < 0x20U || (codepoint >= 0x7fU && codepoint <= 0x9fU)
            || codepoint == static_cast<std::uint32_t>('/')
            || codepoint == static_cast<std::uint32_t>('\\')) return false;
        index += length;
    }
    return true;
}

bool valid_attachment_reference_id(std::string_view value) {
    if (value.empty() || value.size() > 256U) return false;
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= 'a' && character <= 'z')
               || (character >= 'A' && character <= 'Z')
               || (character >= '0' && character <= '9')
               || character == '.' || character == '_' || character == '-';
    });
}

bool valid_portable_root_name(const std::string& name);

bool valid_attachment_basename(const std::string& value) {
    return !value.empty()
           && value.size() <= kMaximumAttachmentBasenameBytes
           && value != "." && value != ".."
           && valid_semantic_project_name(value)
           && valid_portable_root_name(value);
}

bool attachment_path_is_reparse_point(const fs::path& path) {
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
           && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
#else
    (void)path;
    return false;
#endif
}

std::string legacy_attachment_asset_path(std::string_view digest) {
    return "assets/" + std::string(digest);
}

std::string attachment_asset_path(std::string_view digest,
                                  std::string_view basename) {
    return "assets/" + std::string(digest) + "/" + std::string(basename);
}

std::string attachment_asset_path(const ProjectAttachment& attachment,
                                  std::uint32_t format_version) {
    return format_version >= 3U
               ? attachment_asset_path(attachment.sha256, attachment.basename)
               : legacy_attachment_asset_path(attachment.sha256);
}

std::string music_analysis_asset_path(std::string_view digest) {
    return "assets/" + std::string(digest) + "/music_analysis.txt";
}

std::string safe_cache_extension(const std::string& basename) {
    const std::size_t dot = basename.find_last_of('.');
    if (dot == std::string::npos || dot + 1U == basename.size()
        || basename.size() - dot > 17U) {
        return ".asset";
    }
    std::string extension = basename.substr(dot);
    for (char& character : extension) {
        if (character == '.') continue;
        const unsigned char raw = static_cast<unsigned char>(character);
        if (!((raw >= 'a' && raw <= 'z') || (raw >= 'A' && raw <= 'Z')
              || (raw >= '0' && raw <= '9'))) {
            return ".asset";
        }
        if (raw >= 'A' && raw <= 'Z') {
            character = static_cast<char>(raw - 'A' + 'a');
        }
    }
    return extension;
}

bool read_attachment_source(const std::string& path,
                            std::string& basename,
                            std::string& bytes,
                            std::string& digest,
                            std::string* error) {
    if (path.empty() || path.size() > 4096U || path.find('\0') != std::string::npos
        || !detail::valid_utf8(path)) {
        return fail(error, "Attachment source path is invalid or overlong.");
    }
    const fs::path native = detail::path_from_utf8(path);
    std::error_code filesystem_error;
    const fs::file_status status = fs::symlink_status(native, filesystem_error);
    if (filesystem_error || fs::is_symlink(status)
        || attachment_path_is_reparse_point(native)
        || !fs::is_regular_file(status)) {
        return fail(error,
                    "Attachment source must be a readable regular file, not a link or special file.");
    }
    const std::uintmax_t size = fs::file_size(native, filesystem_error);
    if (filesystem_error || size > kMaximumProjectAttachmentBytes) {
        return fail(error, "Attachment exceeds the 512 MiB per-file limit.");
    }
    basename = detail::path_to_utf8(native.filename());
    if (!valid_attachment_basename(basename)) {
        return fail(error, "Attachment basename is invalid or not portable.");
    }
    std::ifstream input(native, std::ios::binary);
    if (!input) return fail(error, "Could not open attachment source.");
    bytes.assign(static_cast<std::size_t>(size), '\0');
    if (!bytes.empty()) {
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    if (!input || input.peek() != std::char_traits<char>::eof()) {
        return fail(error, "Could not read the complete attachment source.");
    }
    return detail::sha256_hex(bytes, digest, error);
}

bool ensure_attachment_cache(std::shared_ptr<ProjectAttachmentCache>& cache,
                             std::string* error) {
    if (cache != nullptr && !cache->directory.empty()) return true;
    std::error_code filesystem_error;
    const fs::path temporary_root = fs::temp_directory_path(filesystem_error);
    if (filesystem_error) {
        return fail(error, "Could not locate the temporary directory for attachments.");
    }
    auto candidate = std::make_shared<ProjectAttachmentCache>();
    for (int attempt = 0; attempt < 128; ++attempt) {
        const fs::path directory = temporary_root
                                   / detail::path_from_utf8(
                                       "pvt-asset-cache-" + generate_uuid());
        filesystem_error.clear();
        if (fs::create_directory(directory, filesystem_error)
            && !filesystem_error) {
            fs::permissions(directory, fs::perms::owner_all,
                            fs::perm_options::replace, filesystem_error);
            if (filesystem_error) {
                std::error_code ignored;
                fs::remove(directory, ignored);
                return fail(error,
                            "Could not secure the temporary attachment directory.");
            }
            candidate->directory = detail::path_to_utf8(directory);
            cache = std::move(candidate);
            return true;
        }
    }
    return fail(error, "Could not create a unique attachment cache directory.");
}

bool materialize_attachment_bytes(
    std::shared_ptr<ProjectAttachmentCache>& cache,
    const std::string& digest,
    const std::string& basename,
    const std::string& bytes,
    std::string& local_path,
    std::string* error) {
    if (!canonical_hash(digest) || !valid_attachment_basename(basename)
        || bytes.size() > kMaximumProjectAttachmentBytes) {
        return fail(error, "Cannot materialize invalid attachment metadata.");
    }
    std::string actual;
    if (!detail::sha256_hex(bytes, actual, error) || actual != digest) {
        return fail(error, "Embedded attachment bytes do not match their SHA-256 identity.");
    }
    if (!ensure_attachment_cache(cache, error)) return false;
    // Keep lookup, validation, installation, and registration one transaction.
    // In particular, two copied ProjectDocuments may otherwise race while
    // materializing the same digest into their shared cache directory.
    const std::unique_lock<std::mutex> cache_lock(cache->mutex);
    const std::string extension = safe_cache_extension(basename);
    const std::string cache_key = digest + extension;
    const auto existing = cache->materialized_by_identity.find(cache_key);
    if (existing != cache->materialized_by_identity.end()) {
        std::error_code filesystem_error;
        const fs::file_status status = fs::symlink_status(
            detail::path_from_utf8(existing->second), filesystem_error);
        if (!filesystem_error && fs::is_regular_file(status)
            && !fs::is_symlink(status)) {
            local_path = existing->second;
            return true;
        }
        cache->materialized_by_identity.erase(existing);
    }
    const fs::path directory = detail::path_from_utf8(cache->directory);
    const fs::path destination = directory / detail::path_from_utf8(
        cache_key);
    const fs::path temporary = directory / detail::path_from_utf8(
        ".attachment-" + generate_uuid() + ".tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return fail(error, "Could not create temporary attachment cache file.");
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return fail(error, "Could not write temporary attachment cache file.");
        }
    }
    std::error_code filesystem_error;
    fs::rename(temporary, destination, filesystem_error);
    if (filesystem_error) {
        fs::remove(temporary, filesystem_error);
        return fail(error, "Could not install temporary attachment cache file.");
    }
    fs::permissions(destination,
                    fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, filesystem_error);
    if (filesystem_error) {
        fs::remove(destination, filesystem_error);
        return fail(error, "Could not secure materialized attachment file.");
    }
    local_path = detail::path_to_utf8(destination);
    cache->materialized_by_identity[cache_key] = local_path;
    return true;
}

bool valid_portable_root_name(const std::string& name) {
    if (!valid_semantic_project_name(name) || name == "." || name == ".."
        || name.back() == ' ' || name.back() == '.') return false;
    static constexpr std::string_view forbidden = "<>:\"/\\|?*";
    for (const char raw : name) {
        const unsigned char character = static_cast<unsigned char>(raw);
        if (character < 0x20U || character == 0x7fU
            || forbidden.find(static_cast<char>(character)) != std::string_view::npos) {
            return false;
        }
    }
    std::string folded;
    folded.reserve(name.size());
    for (const char character : name) {
        folded.push_back(static_cast<char>(std::toupper(
            static_cast<unsigned char>(character))));
    }
    const std::size_t dot = folded.find('.');
    const std::string basename = folded.substr(0U, dot);
    static const std::set<std::string> reserved{
        "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
        "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
        "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    return reserved.find(basename) == reserved.end();
}

std::string portable_root_name(const std::string& project_name) {
    std::string sanitized;
    sanitized.reserve((std::min)(project_name.size(), kMaximumProjectNameBytes));
    for (const char raw : project_name) {
        const unsigned char character = static_cast<unsigned char>(raw);
        if (character < 0x20U || character == 0x7fU
            || std::string_view("<>:\"/\\|?*").find(raw) != std::string_view::npos) {
            sanitized.push_back('_');
        } else {
            sanitized.push_back(raw);
        }
    }
    if (sanitized.size() > kMaximumPortableRootBytes) {
        sanitized.resize(kMaximumPortableRootBytes);
        while (!sanitized.empty() && !detail::valid_utf8(sanitized)) {
            sanitized.pop_back();
        }
    }
    while (!sanitized.empty() && (sanitized.back() == ' ' || sanitized.back() == '.')) {
        sanitized.pop_back();
    }
    if (sanitized.empty() || !detail::valid_utf8(sanitized)) sanitized = "Untitled Fire";
    if (!valid_portable_root_name(sanitized)) sanitized.insert(sanitized.begin(), '_');
    if (!valid_portable_root_name(sanitized)) sanitized = "Untitled Fire";
    return sanitized;
}

bool serialize_root_metadata(const RootMetadata& metadata,
                             std::string& bytes,
                             std::string* error) {
    std::size_t alias_count = metadata.lineage_aliases.size();
    if (alias_count > kMaximumLineageAliases) {
        return fail(error, "Root metadata exceeds the lineage-alias limit.");
    }
    for (const auto& preserved : metadata.preserved_versions) {
        if (preserved.second.lineage_aliases.size()
                > kMaximumLineageAliases - alias_count) {
            return fail(error, "Root metadata exceeds the lineage-alias limit.");
        }
        alias_count += preserved.second.lineage_aliases.size();
    }
    TextBuilder builder("PVT_BUNDLE", kProjectBundleFormatVersion);
    builder.string("project.uuid", metadata.project_uuid);
    builder.string("project.name", metadata.project_name);
    builder.string("project.first_created_utc", metadata.first_created_utc);
    builder.string("project.last_opened_utc", metadata.last_opened_utc);
    builder.string("project.last_saved_utc", metadata.last_saved_utc);
    builder.string("project.created_with_version", metadata.created_with_version);
    builder.string("project.last_changed_with_version",
                   metadata.last_changed_with_version);
    builder.integer("versions.count", metadata.version_digests.size());
    std::size_t index = 0U;
    for (const auto& version : metadata.version_digests) {
        builder.integer(indexed("versions", index, "number"), version.first);
        builder.add(indexed("versions", index, "metadata_sha256"), version.second);
        const auto tree = metadata.version_tree_digests.find(version.first);
        if (tree == metadata.version_tree_digests.end()) {
            return fail(error, "Root metadata is missing a version tree checksum.");
        }
        builder.add(indexed("versions", index, "tree_sha256"), tree->second);
        ++index;
    }
    builder.integer("lineage_aliases.count", metadata.lineage_aliases.size());
    index = 0U;
    for (const std::string& alias : metadata.lineage_aliases) {
        builder.add(indexed("lineage_aliases", index, "sha256"), alias);
        ++index;
    }
    builder.integer("preserved.count", metadata.preserved_versions.size());
    index = 0U;
    for (const auto& preserved : metadata.preserved_versions) {
        builder.integer(indexed("preserved", index, "number"), preserved.first);
        builder.string(indexed("preserved", index, "metadata_sha256"),
                       preserved.second.observed_metadata_digest);
        builder.add(indexed("preserved", index, "tree_sha256"),
                    preserved.second.tree_digest);
        builder.integer(indexed("preserved", index, "aliases.count"),
                        preserved.second.lineage_aliases.size());
        std::size_t alias_index = 0U;
        for (const std::string& alias : preserved.second.lineage_aliases) {
            builder.add(indexed("preserved", index,
                                "aliases." + std::to_string(alias_index)),
                        alias);
            ++alias_index;
        }
        ++index;
    }
    if (!builder.ok()) {
        return fail(error, "Could not serialize root bundle metadata.");
    }
    bytes = builder.bytes();
    return true;
}

bool parse_root_metadata(const std::string& bytes,
                         RootMetadata& destination,
                         std::string* error) {
    Records records;
    if (!parse_text(bytes, "PVT_BUNDLE", kProjectBundleFormatVersion,
                    records, error)) return false;
    RootMetadata candidate;
    if (!take_string(records, "project.uuid", candidate.project_uuid, error)
        || !take_string(records, "project.name", candidate.project_name, error)
        || !take_string(records, "project.first_created_utc",
                        candidate.first_created_utc, error)
        || !take_string(records, "project.last_opened_utc",
                        candidate.last_opened_utc, error)
        || !take_string(records, "project.last_saved_utc",
                        candidate.last_saved_utc, error)
        || !take_string(records, "project.created_with_version",
                        candidate.created_with_version, error)
        || !take_string(records, "project.last_changed_with_version",
                        candidate.last_changed_with_version, error)) return false;
    std::size_t count = 0U;
    if (!take_integer(records, "versions.count", count, error)
        || count > kMaximumVersions) {
        return fail(error, "Root metadata has an invalid version count.");
    }
    for (std::size_t index = 0U; index < count; ++index) {
        std::uint64_t number = 0U;
        std::string digest;
        std::string tree_digest;
        if (!take_integer(records, indexed("versions", index, "number"),
                          number, error)
            || !take(records, indexed("versions", index, "metadata_sha256"),
                     digest, error)
            || !take(records, indexed("versions", index, "tree_sha256"),
                     tree_digest, error)
            || !canonical_hash(digest)
            || !canonical_hash(tree_digest)
            || !candidate.version_digests.emplace(number, digest).second
            || !candidate.version_tree_digests.emplace(number, tree_digest).second) {
            return fail(error, "Root metadata has an invalid version entry.");
        }
    }
    std::size_t total_aliases = 0U;
    if (records.find("lineage_aliases.count") != records.end()) {
        if (!take_integer(records, "lineage_aliases.count", total_aliases, error)
            || total_aliases > kMaximumLineageAliases) {
            return fail(error, "Root metadata has an invalid lineage-alias count.");
        }
        for (std::size_t alias_index = 0U; alias_index < total_aliases;
             ++alias_index) {
            std::string alias;
            if (!take(records,
                      indexed("lineage_aliases", alias_index, "sha256"),
                      alias, error)
                || !canonical_hash(alias)
                || !candidate.lineage_aliases.insert(std::move(alias)).second) {
                return fail(error, "Root metadata has an invalid lineage alias.");
            }
        }
    }
    std::size_t preserved_count = 0U;
    const bool has_preserved = records.find("preserved.count") != records.end();
    if (has_preserved
        && (!take_integer(records, "preserved.count", preserved_count, error)
            || preserved_count > kMaximumVersions
            || count > kMaximumVersions - preserved_count)) {
        return fail(error, "Root metadata has an invalid preserved-history count.");
    }
    for (std::size_t index = 0U; index < preserved_count; ++index) {
        std::uint64_t number = 0U;
        RootMetadata::PreservedVersion preserved;
        std::size_t alias_count = 0U;
        if (!take_integer(records, indexed("preserved", index, "number"),
                          number, error)
            || !take_string(records,
                            indexed("preserved", index, "metadata_sha256"),
                            preserved.observed_metadata_digest, error)
            || !take(records, indexed("preserved", index, "tree_sha256"),
                     preserved.tree_digest, error)
            || !take_integer(records,
                             indexed("preserved", index, "aliases.count"),
                             alias_count, error)
            || alias_count > kMaximumLineageAliases
            || total_aliases > kMaximumLineageAliases - alias_count
            || (!preserved.observed_metadata_digest.empty()
                && !canonical_hash(preserved.observed_metadata_digest))
            || !canonical_hash(preserved.tree_digest)
            || candidate.version_digests.find(number)
                   != candidate.version_digests.end()) {
            return fail(error, "Root metadata has an invalid preserved-history entry.");
        }
        total_aliases += alias_count;
        for (std::size_t alias_index = 0U; alias_index < alias_count;
             ++alias_index) {
            std::string alias;
            if (!take(records,
                      indexed("preserved", index,
                              "aliases." + std::to_string(alias_index)),
                      alias, error)
                || !canonical_hash(alias)
                || !preserved.lineage_aliases.insert(std::move(alias)).second) {
                return fail(error,
                            "Root metadata has an invalid preserved-history alias.");
            }
        }
        if (!candidate.preserved_versions.emplace(number,
                                                   std::move(preserved)).second) {
            return fail(error, "Root metadata has a duplicate preserved-history entry.");
        }
    }
    if (!records.empty() || !canonical_uuid(candidate.project_uuid)
        || !valid_semantic_project_name(candidate.project_name)
        || !canonical_timestamp(candidate.first_created_utc)
        || !canonical_timestamp(candidate.last_opened_utc)
        || !canonical_timestamp(candidate.last_saved_utc)
        || candidate.created_with_version.empty()
        || candidate.last_changed_with_version.empty()) {
        return fail(error, "Root bundle metadata failed validation.");
    }
    destination = std::move(candidate);
    return true;
}

bool serialize_checksum(std::string_view digest, std::string& bytes,
                        std::string* error) {
    if (!canonical_hash(std::string(digest))) {
        return fail(error, "Cannot serialize invalid checksum.");
    }
    TextBuilder builder("PVT_SHA256", 1U);
    builder.add("metadata.sha256", std::string(digest));
    bytes = builder.bytes();
    return builder.ok();
}

bool parse_checksum(const std::string& bytes, std::string& digest,
                    std::string* error) {
    Records records;
    return parse_text(bytes, "PVT_SHA256", 1U, records, error)
           && take(records, "metadata.sha256", digest, error)
           && records.empty() && canonical_hash(digest);
}

bool serialize_current(std::uint64_t version, std::string_view digest,
                       std::string& bytes, std::string* error) {
    if (!canonical_hash(std::string(digest))) {
        return fail(error, "Cannot serialize current pointer with invalid checksum.");
    }
    TextBuilder builder("PVT_CURRENT", 1U);
    builder.integer("version", version);
    builder.add("metadata.sha256", std::string(digest));
    bytes = builder.bytes();
    return builder.ok();
}

bool parse_current(const std::string& bytes, std::uint64_t& version,
                   std::string& digest, std::string* error) {
    Records records;
    return parse_text(bytes, "PVT_CURRENT", 1U, records, error)
           && take_integer(records, "version", version, error)
           && take(records, "metadata.sha256", digest, error)
           && records.empty() && canonical_hash(digest);
}

bool serialize_music_analysis_reference(std::string_view digest,
                                        std::string& bytes,
                                        std::string* error) {
    if (!canonical_hash(std::string(digest))) {
        return fail(error, "Cannot reference invalid music-analysis content.");
    }
    TextBuilder builder("PVT_MUSIC_ANALYSIS_REF", 1U);
    builder.add("sha256", std::string(digest));
    bytes = builder.bytes();
    return builder.ok();
}

bool parse_music_analysis_reference(const std::string& bytes,
                                    std::string& digest,
                                    std::string* error) {
    Records records;
    return parse_text(bytes, "PVT_MUSIC_ANALYSIS_REF", 1U, records, error)
           && take(records, "sha256", digest, error)
           && records.empty() && canonical_hash(digest);
}

bool split_render_output_and_music(
    const CanvasLoopConfig& canvas, const ExportConfig& output,
    std::string& render_output, std::string& analysis_bytes,
    std::string& analysis_digest, std::string* error) {
    if (!detail::serialize_music_analysis_config(
            canvas.clock.music, analysis_bytes, error)) return false;
    MusicAnalysis empty_analysis;
    std::string empty_bytes;
    if (!detail::serialize_music_analysis_config(
            empty_analysis, empty_bytes, error)) return false;
    if (analysis_bytes == empty_bytes) {
        analysis_bytes.clear();
        analysis_digest.clear();
        return detail::serialize_render_output_config(
            canvas, output, render_output, error);
    }
    if (!detail::sha256_hex(analysis_bytes, analysis_digest, error)) {
        return false;
    }
    return detail::serialize_split_render_output_config(
        canvas, output, render_output, error);
}

bool stage_music_analysis(
    detail::BundleFileSet& files, const std::string& analysis_bytes,
    const std::string& analysis_digest, std::string* error) {
    if (analysis_digest.empty()) {
        if (analysis_bytes.empty()) return true;
        return fail(error,
                    "Shared music analysis bytes are missing their content identity.");
    }
    const std::string path = music_analysis_asset_path(analysis_digest);
    const auto existing = files.files.find(path);
    if (existing != files.files.end()) {
        if (existing->second != analysis_bytes) {
            return fail(error,
                        "Existing shared music analysis does not match its content identity.");
        }
        return true;
    }
    files.files.emplace(path, analysis_bytes);
    return true;
}

bool serialize_version_manifest(const VersionManifest& manifest,
                                std::string& bytes,
                                std::string* error) {
    if (manifest.attachments.size() > kMaximumProjectAttachmentReferences) {
        return fail(error, "Version exceeds the attachment-reference limit.");
    }
    TextBuilder builder("PVT_VERSION", kProjectVersionFormatVersion);
    builder.integer("version.number", manifest.info.number);
    builder.string("version.uuid", manifest.info.uuid);
    builder.string("version.parent_digest", manifest.info.parent_digest);
    builder.string("version.reason", manifest.info.reason);
    builder.string("version.saved_utc", manifest.info.saved_utc);
    builder.string("version.saved_with_version", manifest.info.saved_with_version);
    builder.string("version.reverted_from_digest", manifest.reverted_from_digest);
    builder.string("project.name", manifest.project_name);
    builder.add("render_output.sha256", manifest.render_output_digest);
    builder.string("music_analysis.sha256", manifest.music_analysis_digest);
    builder.integer("layers.count", manifest.layers.size());
    for (std::size_t index = 0U; index < manifest.layers.size(); ++index) {
        const LayerConfig& layer = manifest.layers[index];
        builder.integer(indexed("layers", index, "file_id"), layer.file_id);
        builder.string(indexed("layers", index, "uuid"), layer.uuid);
        builder.string(indexed("layers", index, "name"), layer.name);
        builder.boolean(indexed("layers", index, "enabled"), layer.enabled);
        builder.add(indexed("layers", index, "blend_mode"),
                    blend_token(layer.blend_mode));
        builder.real(indexed("layers", index, "opacity"), layer.opacity);
        builder.add(indexed("layers", index, "sha256"),
                    manifest.layer_digests[index]);
    }
    builder.integer("attachments.count", manifest.attachments.size());
    std::set<std::string> reference_ids;
    for (std::size_t index = 0U; index < manifest.attachments.size(); ++index) {
        const ProjectAttachment& attachment = manifest.attachments[index];
        if (!valid_attachment_reference_id(attachment.reference_id)
            || !canonical_hash(attachment.sha256)
            || !valid_attachment_basename(attachment.basename)
            || attachment.size_bytes > kMaximumProjectAttachmentBytes
            || !reference_ids.insert(attachment.reference_id).second) {
            return fail(error, "Version contains invalid attachment metadata.");
        }
        builder.string(indexed("attachments", index, "reference_id"),
                       attachment.reference_id);
        builder.add(indexed("attachments", index, "sha256"),
                    attachment.sha256);
        builder.string(indexed("attachments", index, "basename"),
                       attachment.basename);
        builder.integer(indexed("attachments", index, "size_bytes"),
                        attachment.size_bytes);
    }
    if (!builder.ok()) {
        return fail(error, "Could not serialize version metadata.");
    }
    bytes = builder.bytes();
    return true;
}

bool parse_version_manifest(const std::string& bytes,
                            VersionManifest& destination,
                            std::string* error) {
    Records records;
    std::uint32_t format_version = 0U;
    if (bytes.rfind("PVT_VERSION\t1\n", 0U) == 0U
        || bytes.rfind("PVT_VERSION\t1\r\n", 0U) == 0U) {
        format_version = 1U;
    } else if (bytes.rfind("PVT_VERSION\t2\n", 0U) == 0U
               || bytes.rfind("PVT_VERSION\t2\r\n", 0U) == 0U) {
        format_version = 2U;
    } else if (bytes.rfind("PVT_VERSION\t3\n", 0U) == 0U
               || bytes.rfind("PVT_VERSION\t3\r\n", 0U) == 0U) {
        format_version = 3U;
    } else if (bytes.rfind("PVT_VERSION\t4\n", 0U) == 0U
               || bytes.rfind("PVT_VERSION\t4\r\n", 0U) == 0U) {
        format_version = 4U;
    } else {
        return fail(error, "Unsupported version metadata format.");
    }
    if (!parse_text(bytes, "PVT_VERSION", format_version, records, error)) {
        return false;
    }
    VersionManifest candidate;
    candidate.format_version = format_version;
    if (!take_integer(records, "version.number", candidate.info.number, error)
        || !take_string(records, "version.uuid", candidate.info.uuid, error)
        || !take_string(records, "version.parent_digest",
                        candidate.info.parent_digest, error)
        || !take_string(records, "version.reason", candidate.info.reason, error)
        || !take_string(records, "version.saved_utc", candidate.info.saved_utc, error)
        || !take_string(records, "version.saved_with_version",
                        candidate.info.saved_with_version, error)
        || !take_string(records, "version.reverted_from_digest",
                        candidate.reverted_from_digest, error)
        || !take_string(records, "project.name", candidate.project_name, error)
        || !take(records, "render_output.sha256",
                 candidate.render_output_digest, error)) return false;
    if (format_version >= 4U
        && !take_string(records, "music_analysis.sha256",
                        candidate.music_analysis_digest, error)) return false;
    std::size_t count = 0U;
    if (!take_integer(records, "layers.count", count, error)
        || count == 0U || count > kMaximumLayers) {
        return fail(error, "Version metadata has an invalid layer count.");
    }
    candidate.layers.resize(count);
    candidate.layer_digests.resize(count);
    std::unordered_set<std::uint64_t> file_ids;
    std::unordered_set<std::string> uuids;
    for (std::size_t index = 0U; index < count; ++index) {
        LayerConfig& layer = candidate.layers[index];
        std::string blend;
        if (!take_integer(records, indexed("layers", index, "file_id"),
                          layer.file_id, error)
            || !take_string(records, indexed("layers", index, "uuid"),
                            layer.uuid, error)
            || !take_string(records, indexed("layers", index, "name"),
                            layer.name, error)
            || !take_bool(records, indexed("layers", index, "enabled"),
                          layer.enabled, error)
            || !take(records, indexed("layers", index, "blend_mode"),
                     blend, error)
            || !take_real(records, indexed("layers", index, "opacity"),
                          layer.opacity, error)
            || !take(records, indexed("layers", index, "sha256"),
                     candidate.layer_digests[index], error)
            || !parse_blend(blend, layer.blend_mode)
            || !canonical_uuid(layer.uuid)
            || !canonical_hash(candidate.layer_digests[index])
            || !file_ids.insert(layer.file_id).second
            || !uuids.insert(layer.uuid).second) {
            return fail(error, "Version metadata has an invalid layer entry.");
        }
    }
    if (format_version >= 2U) {
        std::size_t attachment_count = 0U;
        if (!take_integer(records, "attachments.count", attachment_count, error)
            || attachment_count > kMaximumProjectAttachmentReferences) {
            return fail(error,
                        "Version metadata has an invalid attachment-reference count.");
        }
        candidate.attachments.resize(attachment_count);
        std::set<std::string> reference_ids;
        for (std::size_t index = 0U; index < attachment_count; ++index) {
            ProjectAttachment& attachment = candidate.attachments[index];
            if (!take_string(records,
                             indexed("attachments", index, "reference_id"),
                             attachment.reference_id, error)
                || !take(records, indexed("attachments", index, "sha256"),
                         attachment.sha256, error)
                || !take_string(records,
                                indexed("attachments", index, "basename"),
                                attachment.basename, error)
                || !take_integer(records,
                                 indexed("attachments", index, "size_bytes"),
                                 attachment.size_bytes, error)
                || !valid_attachment_reference_id(attachment.reference_id)
                || !canonical_hash(attachment.sha256)
                || !valid_attachment_basename(attachment.basename)
                || attachment.size_bytes > kMaximumProjectAttachmentBytes
                || !reference_ids.insert(attachment.reference_id).second) {
                return fail(error,
                            "Version metadata has an invalid attachment reference.");
            }
        }
    }
    const bool valid_parent = candidate.info.parent_digest.empty()
                              || canonical_hash(candidate.info.parent_digest);
    const bool valid_revert = candidate.reverted_from_digest.empty()
                              || canonical_hash(candidate.reverted_from_digest);
    if (!records.empty() || !canonical_uuid(candidate.info.uuid)
        || !valid_parent || !valid_revert
        || !canonical_timestamp(candidate.info.saved_utc)
        || !valid_semantic_project_name(candidate.project_name)
        || candidate.info.reason.empty() || candidate.info.saved_with_version.empty()
        || !canonical_hash(candidate.render_output_digest)
        || (!candidate.music_analysis_digest.empty()
            && !canonical_hash(candidate.music_analysis_digest))) {
        return fail(error, "Version metadata failed validation.");
    }
    candidate.info.layer_count = count;
    destination = std::move(candidate);
    return true;
}

bool numeric_component(std::string_view text, std::uint64_t& value) {
    if (text.empty() || (text.size() > 1U && text.front() == '0')) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(),
                                        value, 10);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

std::set<std::uint64_t> numeric_version_directories(
    const detail::BundleFileSet& files) {
    std::set<std::uint64_t> versions;
    for (const auto& entry : files.files) {
        const std::size_t slash = entry.first.find('/');
        if (slash == std::string::npos) continue;
        std::uint64_t version = 0U;
        if (numeric_component(std::string_view(entry.first).substr(0U, slash),
                              version)) {
            versions.insert(version);
        }
    }
    return versions;
}

std::string version_path(std::uint64_t version, std::string_view filename) {
    return std::to_string(version) + "/" + std::string(filename);
}

bool version_directory_present(const detail::BundleFileSet& files,
                               std::uint64_t version) {
    const std::string prefix = std::to_string(version) + "/";
    const auto entry = files.files.lower_bound(prefix);
    return entry != files.files.end()
           && entry->first.compare(0U, prefix.size(), prefix) == 0;
}

bool version_tree_digest(const detail::BundleFileSet& files,
                         std::uint64_t version,
                         std::string& digest,
                         std::string* error) {
    detail::BundleFileSet tree;
    tree.root_name = std::to_string(version);
    const std::string prefix = tree.root_name + "/";
    for (const auto& entry : files.files) {
        if (entry.first.compare(0U, prefix.size(), prefix) == 0) {
            tree.files.emplace(entry.first.substr(prefix.size()), entry.second);
        }
    }
    if (tree.files.empty()) {
        return fail(error, "Version directory is missing or empty.");
    }
    return detail::bundle_file_set_digest(tree, digest, error);
}

bool raw_version_digests(const detail::BundleFileSet& files,
                         std::uint64_t version,
                         std::string& metadata_digest,
                         std::string& tree_digest,
                         std::string* error) {
    metadata_digest.clear();
    const auto metadata = files.files.find(version_path(version, "metadata.txt"));
    if (metadata != files.files.end()
        && !detail::sha256_hex(metadata->second, metadata_digest, error)) {
        return false;
    }
    return version_tree_digest(files, version, tree_digest, error);
}

bool hash_matches(const std::string& bytes, const std::string& expected,
                  bool& matches, std::string* error) {
    std::string actual;
    if (!detail::sha256_hex(bytes, actual, error)) return false;
    matches = actual == expected;
    return true;
}

bool find_file(const detail::BundleFileSet& files, const std::string& path,
               const std::string*& bytes, std::string* error) {
    const auto found = files.files.find(path);
    if (found == files.files.end()) {
        return fail(error, "Bundle is missing required file '" + path + "'.");
    }
    bytes = &found->second;
    return true;
}

bool project_content_digest(const ProjectConfig& project,
                            const std::vector<ProjectAttachment>& attachments,
                            std::string& digest,
                            std::string* error) {
    std::string output_bytes;
    if (!detail::serialize_render_output_config(project.canvas, project.output,
                                                output_bytes, error)) return false;
    TextBuilder builder("PVT_PROJECT_CONTENT", 1U);
    builder.string("project.uuid", project.uuid);
    builder.string("project.name", project.name);
    std::string output_digest;
    if (!detail::sha256_hex(output_bytes, output_digest, error)) return false;
    builder.add("render_output.sha256", output_digest);
    builder.integer("layers.count", project.layers.size());
    for (std::size_t index = 0U; index < project.layers.size(); ++index) {
        const LayerConfig& layer = project.layers[index];
        std::string layer_bytes;
        if (!detail::serialize_layer_config(layer.render, layer_bytes, error)) return false;
        std::string layer_digest;
        if (!detail::sha256_hex(layer_bytes, layer_digest, error)) return false;
        builder.integer(indexed("layers", index, "file_id"), layer.file_id);
        builder.string(indexed("layers", index, "uuid"), layer.uuid);
        builder.string(indexed("layers", index, "name"), layer.name);
        builder.boolean(indexed("layers", index, "enabled"), layer.enabled);
        builder.add(indexed("layers", index, "blend_mode"),
                    blend_token(layer.blend_mode));
        builder.real(indexed("layers", index, "opacity"), layer.opacity);
        builder.add(indexed("layers", index, "render_sha256"), layer_digest);
    }
    std::vector<const ProjectAttachment*> ordered_attachments;
    ordered_attachments.reserve(attachments.size());
    for (const ProjectAttachment& attachment : attachments) {
        ordered_attachments.push_back(&attachment);
    }
    std::sort(ordered_attachments.begin(), ordered_attachments.end(),
              [](const ProjectAttachment* left,
                 const ProjectAttachment* right) {
                  return left->reference_id < right->reference_id;
              });
    builder.integer("attachments.count", ordered_attachments.size());
    for (std::size_t index = 0U; index < ordered_attachments.size(); ++index) {
        const ProjectAttachment& attachment = *ordered_attachments[index];
        if (!valid_attachment_reference_id(attachment.reference_id)
            || !canonical_hash(attachment.sha256)
            || !valid_attachment_basename(attachment.basename)
            || attachment.size_bytes > kMaximumProjectAttachmentBytes) {
            return fail(error, "Could not canonicalize invalid project attachment.");
        }
        builder.string(indexed("attachments", index, "reference_id"),
                       attachment.reference_id);
        builder.add(indexed("attachments", index, "sha256"),
                    attachment.sha256);
        builder.string(indexed("attachments", index, "basename"),
                       attachment.basename);
        builder.integer(indexed("attachments", index, "size_bytes"),
                        attachment.size_bytes);
    }
    if (!builder.ok()) {
        return fail(error, "Could not canonicalize project content.");
    }
    return detail::sha256_hex(builder.bytes(), digest, error);
}

struct CachedGlobalConfig {
    CanvasLoopConfig canvas;
    ExportConfig output;
    std::string canonical_output_digest;
};

struct BundleValidationCache {
    std::map<std::string, std::string> asset_digests;
    std::map<std::string, std::string> analysis_digests;
    std::map<std::string, CachedGlobalConfig> global_configs;
};

bool load_snapshot(const detail::BundleFileSet& files,
                   const RootMetadata& root,
                   std::uint64_t version,
                   ProjectConfig& project,
                   BundleVersionInfo& version_info,
                   std::string& semantic_digest,
                   bool& externally_modified,
                   std::string* error,
                   std::vector<ProjectAttachment>* snapshot_attachments = nullptr,
                   BundleValidationCache* validation_cache = nullptr) {
    const std::string* metadata_bytes = nullptr;
    if (!find_file(files, version_path(version, "metadata.txt"),
                   metadata_bytes, error)) return false;
    std::string actual_metadata_digest;
    if (!detail::sha256_hex(*metadata_bytes, actual_metadata_digest, error)) return false;

    VersionManifest manifest;
    if (!parse_version_manifest(*metadata_bytes, manifest, error)
        || manifest.info.number != version) {
        return fail(error, "Version metadata number does not match its directory.");
    }
    manifest.info.metadata_digest = actual_metadata_digest;
    manifest.info.externally_modified = false;
    bool metadata_recorded = false;
    const auto indexed_digest = root.version_digests.find(version);
    const auto preserved = root.preserved_versions.find(version);
    if (indexed_digest != root.version_digests.end()) {
        metadata_recorded = indexed_digest->second == actual_metadata_digest;
    } else if (preserved != root.preserved_versions.end()) {
        metadata_recorded =
            preserved->second.observed_metadata_digest == actual_metadata_digest;
    }
    bool external = !metadata_recorded;

    const std::string* output_bytes = nullptr;
    if (!find_file(files, version_path(version, "render_output.txt"),
                   output_bytes, error)) return false;
    std::string stored_output_digest;
    if (!detail::sha256_hex(*output_bytes,
                            stored_output_digest, error)) return false;

    ProjectConfig candidate;
    candidate.uuid = root.project_uuid;
    candidate.name = manifest.project_name;
    const std::string analysis_reference_path =
        version_path(version, "music_analysis.txt");
    const auto analysis_reference = files.files.find(analysis_reference_path);
    const bool has_analysis_reference =
        analysis_reference != files.files.end();
    if (manifest.format_version >= 4U
        && has_analysis_reference != !manifest.music_analysis_digest.empty()) {
        return fail(error,
                    "Version music-analysis reference does not match its metadata.");
    }

    const bool split_output =
        output_bytes->rfind("PVT_RENDER_OUTPUT_SPLIT\t1\n", 0U) == 0U
        || output_bytes->rfind("PVT_RENDER_OUTPUT_SPLIT\t1\r\n", 0U) == 0U;
    const std::string* shared_analysis_bytes = nullptr;
    std::string actual_analysis_digest;
    if (has_analysis_reference) {
        std::string referenced_digest;
        if (!parse_music_analysis_reference(
                analysis_reference->second, referenced_digest, error)) {
            return false;
        }
        if (manifest.format_version >= 4U
            && referenced_digest != manifest.music_analysis_digest) {
            return fail(error,
                        "Version music-analysis reference checksum disagrees with metadata.");
        }
        const std::string object_path =
            music_analysis_asset_path(referenced_digest);
        const auto object = files.files.find(object_path);
        if (object == files.files.end()) {
            return fail(error, "Version references missing shared music analysis '"
                                   + object_path + "'.");
        }
        shared_analysis_bytes = &object->second;
        bool found_cached_analysis = false;
        if (validation_cache != nullptr) {
            const auto cached_analysis =
                validation_cache->analysis_digests.find(object_path);
            if (cached_analysis != validation_cache->analysis_digests.end()) {
                actual_analysis_digest = cached_analysis->second;
                found_cached_analysis = true;
            }
        }
        if (!found_cached_analysis) {
            if (!detail::sha256_hex(object->second,
                                    actual_analysis_digest, error)) return false;
            if (validation_cache != nullptr) {
                validation_cache->analysis_digests.emplace(
                    object_path, actual_analysis_digest);
            }
        }
        external = external || actual_analysis_digest != referenced_digest;
    }

    const std::string global_cache_key =
        stored_output_digest + ":" + actual_analysis_digest;
    std::string canonical_output_digest;
    bool found_cached_global = false;
    if (validation_cache != nullptr) {
        const auto cached_global =
            validation_cache->global_configs.find(global_cache_key);
        if (cached_global != validation_cache->global_configs.end()) {
            candidate.canvas = cached_global->second.canvas;
            candidate.output = cached_global->second.output;
            canonical_output_digest =
                cached_global->second.canonical_output_digest;
            found_cached_global = true;
        }
    }
    if (!found_cached_global) {
        if (split_output) {
            if (shared_analysis_bytes == nullptr
                || !detail::deserialize_split_render_output_config(
                    *output_bytes, *shared_analysis_bytes,
                    candidate.canvas, candidate.output, error)) return false;
        } else {
            if (!detail::deserialize_render_output_config(
                    *output_bytes, candidate.canvas,
                    candidate.output, error)) return false;
            if (shared_analysis_bytes != nullptr) {
                MusicAnalysis shared_analysis;
                if (!detail::deserialize_music_analysis_config(
                        *shared_analysis_bytes,
                        shared_analysis, error)) return false;
                std::string embedded_analysis;
                if (!detail::serialize_music_analysis_config(
                        candidate.canvas.clock.music,
                        embedded_analysis, error)) return false;
                if (embedded_analysis != *shared_analysis_bytes) {
                    return fail(error,
                                "Version embeds music analysis that disagrees with its shared reference.");
                }
                candidate.canvas.clock.music = std::move(shared_analysis);
            }
        }
        std::string canonical_output;
        if (!detail::serialize_render_output_config(
                candidate.canvas, candidate.output,
                canonical_output, error)
            || !detail::sha256_hex(canonical_output,
                                   canonical_output_digest, error)) return false;
        if (validation_cache != nullptr) {
            validation_cache->global_configs.emplace(
                global_cache_key,
                CachedGlobalConfig{candidate.canvas, candidate.output,
                                   canonical_output_digest});
        }
    }
    if (manifest.format_version >= 4U
        && has_analysis_reference != split_output) {
        return fail(error,
                    "Version render/output storage does not match its declared format.");
    }

    bool output_hash_matches = false;
    if (manifest.format_version >= 4U) {
        output_hash_matches = stored_output_digest
                              == manifest.render_output_digest;
    } else {
        output_hash_matches = canonical_output_digest
                              == manifest.render_output_digest;
    }
    external = external || !output_hash_matches;
    candidate.layers = manifest.layers;
    std::set<std::string> expected_paths{
        version_path(version, "metadata.txt"),
        version_path(version, "render_output.txt")};
    if (has_analysis_reference) {
        expected_paths.insert(analysis_reference_path);
    }
    for (std::size_t index = 0U; index < candidate.layers.size(); ++index) {
        LayerConfig& layer = candidate.layers[index];
        const std::string filename = std::to_string(layer.file_id) + ".pvt";
        const std::string path = version_path(version, filename);
        expected_paths.insert(path);
        const std::string* layer_bytes = nullptr;
        if (!find_file(files, path, layer_bytes, error)) return false;
        bool layer_hash_matches = false;
        if (!hash_matches(*layer_bytes, manifest.layer_digests[index],
                          layer_hash_matches, error)) return false;
        external = external || !layer_hash_matches;
        if (!detail::deserialize_layer_config(*layer_bytes, layer.render, error)) {
            return false;
        }
    }
    if (manifest.format_version >= 2U) {
        const auto attachment_for = [&manifest](std::string_view reference_id)
            -> const ProjectAttachment* {
            const auto found = std::find_if(
                manifest.attachments.begin(), manifest.attachments.end(),
                [reference_id](const ProjectAttachment& attachment) {
                    return attachment.reference_id == reference_id;
                });
            return found == manifest.attachments.end() ? nullptr : &*found;
        };
        const MusicAnalysis& music = candidate.canvas.clock.music;
        const ProjectAttachment* music_source =
            attachment_for(kMusicSourceAttachmentId);
        if (music.source_sha256.empty() != (music_source == nullptr)
            || (music_source != nullptr
                && (music_source->sha256 != music.source_sha256
                    || music_source->basename != music.source_basename))) {
            return fail(error,
                        "Music analysis and its embedded source attachment disagree.");
        }
        for (const LayerConfig& layer : candidate.layers) {
            const MusicAnalysis& layer_music =
                layer.render.layer_clock.clock.music;
            const ProjectAttachment* layer_music_source =
                attachment_for(layer_music_attachment_id(layer.uuid));
            if (layer_music.source_sha256.empty()
                    != (layer_music_source == nullptr)
                || (layer_music_source != nullptr
                    && (layer_music_source->sha256
                            != layer_music.source_sha256
                        || layer_music_source->basename
                            != layer_music.source_basename))) {
                return fail(error,
                            "Active-layer music analysis and its embedded source attachment disagree.");
            }
            const SurfaceConfig& surface = layer.render.surface;
            const ProjectAttachment* obj =
                attachment_for(surface_obj_attachment_id(layer.uuid));
            if (surface.obj_sha256.empty() != (obj == nullptr)
                || (obj != nullptr
                    && (obj->sha256 != surface.obj_sha256
                        || obj->basename != surface.obj_basename))) {
                return fail(error,
                            "Custom OBJ configuration and its embedded attachment disagree.");
            }
        }
    }
    std::set<std::string> manifest_asset_paths;
    for (const ProjectAttachment& attachment : manifest.attachments) {
        manifest_asset_paths.insert(attachment_asset_path(
            attachment, manifest.format_version));
    }
    std::map<std::string, std::string> resolved_missing_asset_paths;
    std::map<std::string, std::string> local_verified_assets;
    auto& verified_assets = validation_cache != nullptr
                                ? validation_cache->asset_digests
                                : local_verified_assets;
    for (ProjectAttachment& attachment : manifest.attachments) {
        const std::string expected_asset_path = attachment_asset_path(
            attachment, manifest.format_version);
        std::string asset_path = expected_asset_path;
        auto asset = files.files.find(asset_path);
        bool renamed = false;
        if (asset == files.files.end() && manifest.format_version >= 3U) {
            const auto already_resolved = resolved_missing_asset_paths.find(
                expected_asset_path);
            if (already_resolved != resolved_missing_asset_paths.end()) {
                asset_path = already_resolved->second;
                asset = files.files.find(asset_path);
                renamed = asset != files.files.end();
            } else {
                const std::string directory = legacy_attachment_asset_path(
                                                  attachment.sha256)
                                              + "/";
                auto renamed_entry = files.files.end();
                for (auto entry = files.files.lower_bound(directory);
                     entry != files.files.end()
                     && entry->first.compare(0U, directory.size(), directory) == 0;
                     ++entry) {
                    if (manifest_asset_paths.find(entry->first)
                        != manifest_asset_paths.end()) {
                        continue;
                    }
                    if (renamed_entry != files.files.end()) {
                        renamed_entry = files.files.end();
                        break; // More than one unclaimed filename is ambiguous.
                    }
                    renamed_entry = entry;
                }
                if (renamed_entry != files.files.end()) {
                    asset_path = renamed_entry->first;
                    asset = renamed_entry;
                    renamed = true;
                    resolved_missing_asset_paths.emplace(expected_asset_path,
                                                         asset_path);
                }
            }
        }
        if (asset == files.files.end()) {
            return fail(error, "Version references missing embedded asset '"
                                   + expected_asset_path + "'.");
        }
        if (renamed) {
            const std::size_t basename_at = asset_path.find_last_of('/');
            const std::string renamed_basename = asset_path.substr(basename_at + 1U);
            if (!valid_attachment_basename(renamed_basename)) {
                return fail(error, "Directly renamed asset has an invalid filename.");
            }
            attachment.basename = renamed_basename;
        }
        std::string actual;
        const auto verified = verified_assets.find(asset_path);
        if (verified == verified_assets.end()) {
            if (!detail::sha256_hex(asset->second, actual, error)) return false;
            verified_assets.emplace(asset_path, actual);
        } else {
            actual = verified->second;
        }
        const bool changed = renamed
                             || asset->second.size() != attachment.size_bytes
                             || actual != attachment.sha256;
        if (changed && manifest.format_version < 3U) {
            return fail(error,
                        "Legacy content-addressed asset content does not match version metadata.");
        }
        attachment.local_path.clear();
        attachment.bundle_path = asset_path;
        attachment.externally_modified = changed;
        if (changed) {
            // Version-3-or-newer asset paths deliberately keep their readable
            // filename independent of integrity metadata. A user may therefore
            // replace the file directly; load the new bytes dirty and let Save
            // promote them exactly as an in-application replacement would.
            attachment.sha256 = std::move(actual);
            attachment.size_bytes = static_cast<std::uint64_t>(asset->second.size());
            external = true;
        }
    }
    if (manifest.format_version >= 3U) {
        const auto attachment_for = [&manifest](std::string_view reference_id)
            -> const ProjectAttachment* {
            const auto found = std::find_if(
                manifest.attachments.begin(), manifest.attachments.end(),
                [reference_id](const ProjectAttachment& attachment) {
                    return attachment.reference_id == reference_id;
                });
            return found == manifest.attachments.end() ? nullptr : &*found;
        };
        if (ProjectAttachment const* music_source =
                attachment_for(kMusicSourceAttachmentId)) {
            candidate.canvas.clock.music.source_sha256 = music_source->sha256;
            candidate.canvas.clock.music.source_basename = music_source->basename;
        }
        for (LayerConfig& layer : candidate.layers) {
            MusicAnalysis& layer_music =
                layer.render.layer_clock.clock.music;
            if (const ProjectAttachment* music = attachment_for(
                    layer_music_attachment_id(layer.uuid))) {
                layer_music.source_sha256 = music->sha256;
                layer_music.source_basename = music->basename;
            }
            SurfaceConfig& surface = layer.render.surface;
            if (const ProjectAttachment* obj = attachment_for(
                    surface_obj_attachment_id(layer.uuid))) {
                surface.obj_sha256 = obj->sha256;
                surface.obj_basename = obj->basename;
            }
        }
    }
    const std::string prefix = std::to_string(version) + "/";
    for (const auto& entry : files.files) {
        if (entry.first.compare(0U, prefix.size(), prefix) == 0
            && expected_paths.find(entry.first) == expected_paths.end()) {
            return fail(error, "Version directory contains unexpected entry '"
                                   + entry.first + "'.");
        }
    }
    const ValidationResult validation = validate(candidate);
    if (!validation.ok) {
        return fail(error, "Version project failed validation: " + validation.message);
    }
    if (!project_content_digest(candidate, manifest.attachments,
                                semantic_digest, error)) return false;
    std::string actual_tree_digest;
    if (!version_tree_digest(files, version, actual_tree_digest, error)) return false;
    const auto recorded_tree = root.version_tree_digests.find(version);
    bool tree_recorded = recorded_tree != root.version_tree_digests.end()
                         && recorded_tree->second == actual_tree_digest;
    if (indexed_digest == root.version_digests.end()
        && preserved != root.preserved_versions.end()) {
        tree_recorded = preserved->second.tree_digest == actual_tree_digest;
    }
    const bool changed_since_recorded = !tree_recorded;
    external = external || changed_since_recorded;
    manifest.info.externally_modified = external;
    manifest.info.changed_since_recorded = changed_since_recorded;
    project = std::move(candidate);
    if (snapshot_attachments != nullptr) {
        *snapshot_attachments = std::move(manifest.attachments);
    }
    version_info = std::move(manifest.info);
    externally_modified = external;
    return true;
}

bool materialize_snapshot_attachments(
    const detail::BundleFileSet& files,
    ProjectConfig& project,
    std::vector<ProjectAttachment>& attachments,
    std::shared_ptr<ProjectAttachmentCache>& cache,
    std::string* error) {
    for (ProjectAttachment& attachment : attachments) {
        const auto bytes = files.files.find(attachment.bundle_path);
        if (bytes == files.files.end()
            || !materialize_attachment_bytes(cache, attachment.sha256,
                                             attachment.basename, bytes->second,
                                             attachment.local_path, error)) {
            return false;
        }
    }
    const auto music_source = std::find_if(
        attachments.begin(), attachments.end(),
        [](const ProjectAttachment& attachment) {
            return attachment.reference_id == kMusicSourceAttachmentId;
        });
    if (music_source != attachments.end()
        && music_source->externally_modified) {
        MusicAnalysis replacement;
        std::string analysis_error;
        if (!audio::analyze_music_file(music_source->local_path, replacement,
                                       {}, nullptr, &analysis_error)) {
            return fail(error,
                        "The directly edited music asset could not be accepted as a GUI replacement: "
                            + analysis_error);
        }
        if (replacement.source_sha256 != music_source->sha256) {
            return fail(error,
                        "The directly edited music asset changed during reanalysis.");
        }
        replacement.source_basename = music_source->basename;
        project.canvas.clock.music = std::move(replacement);
    }
    for (LayerConfig& layer : project.layers) {
        MusicAnalysis& local_music = layer.render.layer_clock.clock.music;
        if (!local_music.source_sha256.empty()) {
            const std::string music_reference =
                layer_music_attachment_id(layer.uuid);
            const auto local_source = std::find_if(
                attachments.begin(), attachments.end(),
                [&music_reference](const ProjectAttachment& attachment) {
                    return attachment.reference_id == music_reference;
                });
            if (local_source == attachments.end()) {
                return fail(error,
                            "Active-layer music attachment disappeared during materialization.");
            }
            if (local_source->externally_modified) {
                MusicAnalysis replacement;
                std::string analysis_error;
                if (!audio::analyze_music_file(local_source->local_path,
                                               replacement, {}, nullptr,
                                               &analysis_error)) {
                    return fail(error,
                                "A directly edited active-layer music asset could not be reanalyzed: "
                                    + analysis_error);
                }
                if (replacement.source_sha256 != local_source->sha256) {
                    return fail(error,
                                "An active-layer music asset changed during reanalysis.");
                }
                replacement.source_basename = local_source->basename;
                local_music = std::move(replacement);
            }
        }
        if (layer.render.surface.obj_sha256.empty()) continue;
        const std::string reference_id =
            surface_obj_attachment_id(layer.uuid);
        const auto found = std::find_if(
            attachments.begin(), attachments.end(),
            [&reference_id](const ProjectAttachment& attachment) {
                return attachment.reference_id == reference_id;
            });
        if (found == attachments.end()) {
            return fail(error, "Custom OBJ attachment disappeared during materialization.");
        }
        layer.render.surface.obj_path = found->local_path;
    }
    const ValidationResult validation = validate(project);
    return validation.ok
               || fail(error, "Materialized project failed validation: "
                                  + validation.message);
}

bool parse_root(const detail::BundleFileSet& files,
                RootMetadata& root,
                bool& externally_modified,
                std::string* error) {
    const std::string* metadata = nullptr;
    if (!find_file(files, "metadata.txt", metadata, error)
        || !parse_root_metadata(*metadata, root, error)) return false;
    std::string actual;
    if (!detail::sha256_hex(*metadata, actual, error)) return false;
    externally_modified = true;
    const auto checksum_file = files.files.find("metadata.sha256");
    if (checksum_file != files.files.end()) {
        std::string expected;
        std::string checksum_error;
        if (parse_checksum(checksum_file->second, expected, &checksum_error)) {
            externally_modified = actual != expected;
        }
    }
    return true;
}

bool validate_root_paths(const detail::BundleFileSet& files,
                         const RootMetadata& root,
                         std::string* error) {
    (void)root;
    const std::set<std::uint64_t> directories = numeric_version_directories(files);
    if (directories.size() > kMaximumVersions) {
        return fail(error, "Bundle exceeds the version directory limit.");
    }
    for (const auto& entry : files.files) {
        if (entry.first == "metadata.txt" || entry.first == "metadata.sha256"
            || entry.first == "current") continue;
        if (entry.first.rfind("assets/", 0U) == 0U) {
            const std::string_view relative(entry.first.data() + 7U,
                                            entry.first.size() - 7U);
            const std::size_t slash = relative.find('/');
            const std::string digest(relative.substr(0U, slash));
            const bool legacy_path = slash == std::string_view::npos;
            const std::string basename = legacy_path
                                             ? std::string{}
                                             : std::string(relative.substr(slash + 1U));
            if (!canonical_hash(digest)
                || (!legacy_path
                    && (basename.find('/') != std::string::npos
                        || !valid_attachment_basename(basename)))) {
                return fail(error, "Bundle contains an invalid asset path '"
                                       + entry.first + "'.");
            }
            if (entry.second.size() > kMaximumProjectAttachmentBytes) {
                return fail(error, "Bundle asset exceeds the 512 MiB limit.");
            }
            if (legacy_path) {
                std::string actual;
                if (!detail::sha256_hex(entry.second, actual, error)) return false;
                if (actual != digest) {
                    return fail(error,
                                "Legacy bundle asset content does not match its SHA-256 path.");
                }
            }
            continue;
        }
        const std::size_t slash = entry.first.find('/');
        std::uint64_t version = 0U;
        if (slash == std::string::npos
            || !numeric_component(std::string_view(entry.first).substr(0U, slash),
                                  version)
            ) {
            return fail(error, "Bundle contains unexpected root entry '"
                                   + entry.first + "'.");
        }
    }
    return true;
}

bool validate_history_accounting(const detail::BundleFileSet& files,
                                 const RootMetadata& root,
                                 std::string* error) {
    const std::set<std::uint64_t> directories =
        numeric_version_directories(files);
    std::set<std::uint64_t> accounted;
    for (const auto& version : root.version_digests) {
        if (root.version_tree_digests.find(version.first)
                == root.version_tree_digests.end()
            || !accounted.insert(version.first).second) {
            return fail(error, "Root metadata has inconsistent indexed history.");
        }
    }
    if (root.version_tree_digests.size() != root.version_digests.size()) {
        return fail(error, "Root metadata has an unexpected version tree checksum.");
    }
    for (const auto& preserved : root.preserved_versions) {
        if (!accounted.insert(preserved.first).second) {
            return fail(error, "A version directory has conflicting history records.");
        }
    }
    if (accounted != directories) {
        return fail(error,
                    "Root metadata does not account for every numeric version directory.");
    }
    return true;
}

bool read_document_source(const std::string& path,
                          detail::BundleFileSet& files,
                          RootMetadata& root,
                          bool& root_external,
                          std::string* error) {
    return detail::read_bundle_file_set(path, files, error)
           && parse_root(files, root, root_external, error)
           && validate_root_paths(files, root, error);
}

bool current_candidate(const detail::BundleFileSet& files,
                       std::uint64_t& version,
                       std::string& digest,
                       std::string* error) {
    const std::string* current = nullptr;
    return find_file(files, "current", current, error)
           && parse_current(*current, version, digest, error);
}

bool collect_version_infos(const detail::BundleFileSet& files,
                           const RootMetadata& root,
                           std::vector<BundleVersionInfo>& destination,
                           std::string* error,
                           BundleValidationCache* shared_cache = nullptr) {
    BundleValidationCache local_cache;
    BundleValidationCache* validation_cache =
        shared_cache != nullptr ? shared_cache : &local_cache;
    std::set<std::uint64_t> numbers = numeric_version_directories(files);
    for (const auto& indexed_version : root.version_digests) {
        numbers.insert(indexed_version.first);
    }
    if (numbers.size() > kMaximumVersions) {
        return fail(error, "Bundle exceeds the 4096-version history limit.");
    }
    std::vector<BundleVersionInfo> candidate;
    candidate.reserve(numbers.size());
    for (const std::uint64_t number : numbers) {
        ProjectConfig project;
        BundleVersionInfo info;
        std::string semantic_digest;
        bool external = false;
        std::string integrity_error;
        const bool indexed = root.version_digests.find(number)
                             != root.version_digests.end();
        if (load_snapshot(files, root, number, project, info, semantic_digest,
                          external, &integrity_error, nullptr,
                          validation_cache)) {
            info.indexed = indexed;
            info.valid = true;
            info.externally_modified = external;
            if (!indexed && !external && !info.changed_since_recorded) {
                info.integrity_message =
                    "Preserved noncanonical version matches its recorded raw tree.";
            } else if (!indexed) {
                info.integrity_message =
                    "Valid unindexed version recovered from an interrupted or external change.";
            } else if (info.changed_since_recorded) {
                info.integrity_message =
                    "Version changed since the exact state recorded by the last save.";
            } else if (external) {
                info.integrity_message =
                    "Recorded external-origin version is valid and its raw state is unchanged.";
            }
            candidate.push_back(std::move(info));
            continue;
        }

        BundleVersionInfo invalid;
        invalid.number = number;
        invalid.indexed = indexed;
        invalid.valid = false;
        std::string observed_metadata_digest;
        std::string observed_tree_digest;
        const bool directory_present = version_directory_present(files, number);
        if (directory_present
            && !raw_version_digests(files, number, observed_metadata_digest,
                                    observed_tree_digest, error)) return false;
        const auto preserved = root.preserved_versions.find(number);
        const bool preserved_matches = directory_present && !indexed
                                       && preserved
                                              != root.preserved_versions.end()
                                       && preserved->second.observed_metadata_digest
                                              == observed_metadata_digest
                                       && preserved->second.tree_digest
                                              == observed_tree_digest;
        invalid.externally_modified = !preserved_matches;
        invalid.changed_since_recorded = !preserved_matches;
        if (preserved_matches) {
            invalid.integrity_message =
                "Preserved malformed history matches its recorded raw tree.";
        } else if (!directory_present) {
            invalid.integrity_message = "Version directory is missing.";
        } else {
            invalid.integrity_message = integrity_error.empty()
                                            ? "Version is malformed or incomplete."
                                            : std::move(integrity_error);
        }
        invalid.metadata_digest = observed_metadata_digest;
        const auto metadata = files.files.find(version_path(number, "metadata.txt"));
        if (metadata != files.files.end()) {
            VersionManifest manifest;
            std::string ignored;
            if (parse_version_manifest(metadata->second, manifest, &ignored)) {
                invalid.uuid = std::move(manifest.info.uuid);
                invalid.parent_digest = std::move(manifest.info.parent_digest);
                invalid.reason = std::move(manifest.info.reason);
                invalid.saved_utc = std::move(manifest.info.saved_utc);
                invalid.saved_with_version =
                    std::move(manifest.info.saved_with_version);
                invalid.layer_count = manifest.info.layer_count;
            }
        }
        candidate.push_back(std::move(invalid));
    }
    destination = std::move(candidate);
    return true;
}

bool validate_loaded_bundle_state(
    const detail::BundleFileSet& files,
    const RootMetadata& root,
    bool root_external,
    std::vector<BundleVersionInfo>& checked,
    std::string* error) {
    if (root_external) {
        return fail(error, "Root metadata checksum does not match.");
    }
    if (!validate_history_accounting(files, root, error)) return false;
    std::set<std::string> metadata_digests;
    std::set<std::string> parent_digest_aliases;
    std::set<std::string> version_uuids;
    parent_digest_aliases.insert(root.lineage_aliases.begin(),
                                 root.lineage_aliases.end());
    for (const auto& preserved : root.preserved_versions) {
        std::string observed_metadata_digest;
        std::string observed_tree_digest;
        if (!raw_version_digests(files, preserved.first,
                                 observed_metadata_digest,
                                 observed_tree_digest, error)) return false;
        if (observed_metadata_digest
                != preserved.second.observed_metadata_digest
            || observed_tree_digest != preserved.second.tree_digest) {
            return fail(error, "Preserved history version "
                                   + std::to_string(preserved.first)
                                   + " changed since its last observed save state.");
        }
        if (!observed_metadata_digest.empty()) {
            parent_digest_aliases.insert(observed_metadata_digest);
        }
        parent_digest_aliases.insert(
            preserved.second.lineage_aliases.begin(),
            preserved.second.lineage_aliases.end());
    }
    for (const auto& indexed_version : root.version_digests) {
        parent_digest_aliases.insert(indexed_version.second);
    }
    for (BundleVersionInfo& info : checked) {
        if (info.indexed && !info.valid) {
            return fail(error, "Indexed version " + std::to_string(info.number)
                                   + " is malformed or incomplete: "
                                   + info.integrity_message);
        }
        if (info.changed_since_recorded) {
            return fail(error, "Version " + std::to_string(info.number)
                                   + " changed since its last observed save state.");
        }
        if (info.valid && info.externally_modified) {
            info.integrity_message =
                "Recorded external-origin version is valid and its raw state is unchanged.";
        }
        if (info.valid) {
            if (!metadata_digests.insert(info.metadata_digest).second
                || !version_uuids.insert(info.uuid).second) {
                return fail(error, "Bundle has duplicate version identity.");
            }
            parent_digest_aliases.insert(info.metadata_digest);
        }
    }
    for (const BundleVersionInfo& info : checked) {
        if (info.indexed && info.valid && !info.parent_digest.empty()
            && parent_digest_aliases.find(info.parent_digest)
                   == parent_digest_aliases.end()) {
            return fail(error,
                        "Version parent digest does not identify a bundle version.");
        }
    }
    std::uint64_t current = 0U;
    std::string current_digest;
    if (!current_candidate(files, current, current_digest, error)) return false;
    const auto root_current = root.version_digests.find(current);
    if (root_current == root.version_digests.end()
        || root_current->second != current_digest) {
        return fail(error,
                    "Current pointer does not match indexed version metadata.");
    }
    const auto current_info = std::find_if(
        checked.begin(), checked.end(), [current](const BundleVersionInfo& info) {
            return info.number == current;
        });
    if (current_info == checked.end() || current_info->externally_modified) {
        return fail(error,
                    "Current version is missing or has an integrity mismatch.");
    }
    return true;
}

bool preserve_raw_version(RootMetadata& root,
                          const detail::BundleFileSet& files,
                          std::uint64_t number,
                          std::string* error) {
    RootMetadata::PreservedVersion preserved;
    const auto existing = root.preserved_versions.find(number);
    if (existing != root.preserved_versions.end()) {
        preserved = existing->second;
    }
    const auto indexed = root.version_digests.find(number);
    if (indexed != root.version_digests.end()) {
        preserved.lineage_aliases.insert(indexed->second);
    }
    if (!version_directory_present(files, number)) {
        if (!preserved.observed_metadata_digest.empty()) {
            root.lineage_aliases.insert(preserved.observed_metadata_digest);
        }
        root.lineage_aliases.insert(preserved.lineage_aliases.begin(),
                                    preserved.lineage_aliases.end());
        root.version_digests.erase(number);
        root.version_tree_digests.erase(number);
        root.preserved_versions.erase(number);
        return true;
    }
    if (!raw_version_digests(files, number,
                             preserved.observed_metadata_digest,
                             preserved.tree_digest, error)) return false;
    root.version_digests.erase(number);
    root.version_tree_digests.erase(number);
    root.preserved_versions[number] = std::move(preserved);
    return true;
}

bool build_version(ProjectConfig project,
                   std::vector<ProjectAttachment> attachments,
                   std::uint64_t number,
                   const std::string& parent_digest,
                   const std::string& reason,
                   const std::string& reverted_from_digest,
                   detail::BundleFileSet& files,
                   BundleVersionInfo& version_info,
                   std::string& semantic_digest,
                   std::string* error) {
    const ValidationResult validation = validate(project);
    if (!validation.ok || !valid_semantic_project_name(project.name)) {
        return fail(error, "Cannot save invalid project: " + validation.message);
    }
    VersionManifest manifest;
    manifest.info.number = number;
    manifest.info.uuid = generate_uuid();
    manifest.info.parent_digest = parent_digest;
    manifest.info.reason = reason;
    manifest.info.saved_utc = utc_now();
    manifest.info.saved_with_version = PVT_PROGRAM_VERSION;
    manifest.info.layer_count = project.layers.size();
    manifest.info.indexed = true;
    manifest.info.valid = true;
    manifest.reverted_from_digest = reverted_from_digest;
    manifest.project_name = project.name;
    manifest.layers = project.layers;
    manifest.attachments = std::move(attachments);

    std::string output_bytes;
    std::string analysis_bytes;
    if (!split_render_output_and_music(
            project.canvas, project.output, output_bytes, analysis_bytes,
            manifest.music_analysis_digest, error)
        || !detail::sha256_hex(output_bytes,
                               manifest.render_output_digest, error)
        || !stage_music_analysis(files, analysis_bytes,
                                 manifest.music_analysis_digest, error)) {
        return false;
    }
    files.files[version_path(number, "render_output.txt")] = std::move(output_bytes);
    if (!manifest.music_analysis_digest.empty()) {
        std::string reference;
        if (!serialize_music_analysis_reference(
                manifest.music_analysis_digest, reference, error)) return false;
        files.files[version_path(number, "music_analysis.txt")] =
            std::move(reference);
    }
    manifest.layer_digests.reserve(project.layers.size());
    for (const LayerConfig& layer : project.layers) {
        std::string layer_bytes;
        std::string digest;
        if (!detail::serialize_layer_config(layer.render, layer_bytes, error)
            || !detail::sha256_hex(layer_bytes, digest, error)) return false;
        files.files[version_path(number, std::to_string(layer.file_id) + ".pvt")] =
            std::move(layer_bytes);
        manifest.layer_digests.push_back(std::move(digest));
    }
    std::string metadata_bytes;
    if (!serialize_version_manifest(manifest, metadata_bytes, error)
        || !detail::sha256_hex(metadata_bytes, manifest.info.metadata_digest, error)
        || !project_content_digest(project, manifest.attachments,
                                   semantic_digest, error)) return false;
    files.files[version_path(number, "metadata.txt")] = std::move(metadata_bytes);
    version_info = std::move(manifest.info);
    return true;
}

bool compact_embedded_music_analysis(
    detail::BundleFileSet& files,
    std::vector<BundleVersionInfo>& versions,
    bool& compacted,
    std::string* error) {
    compacted = false;
    for (BundleVersionInfo& version : versions) {
        if (!version.valid || !version.indexed) continue;
        const std::string metadata_path =
            version_path(version.number, "metadata.txt");
        const std::string output_path =
            version_path(version.number, "render_output.txt");
        const std::string reference_path =
            version_path(version.number, "music_analysis.txt");
        if (files.files.find(reference_path) != files.files.end()) continue;
        const auto metadata = files.files.find(metadata_path);
        const auto output = files.files.find(output_path);
        if (metadata == files.files.end() || output == files.files.end()) continue;

        VersionManifest manifest;
        std::string ignored;
        if (!parse_version_manifest(metadata->second, manifest, &ignored)
            || manifest.format_version >= 4U) {
            continue;
        }
        std::string actual_output_digest;
        if (!detail::sha256_hex(output->second,
                                actual_output_digest, error)) return false;
        if (actual_output_digest != manifest.render_output_digest) {
            continue; // Preserve direct edits for normal external promotion.
        }

        CanvasLoopConfig canvas;
        ExportConfig export_config;
        if (!detail::deserialize_render_output_config(
                output->second, canvas, export_config, &ignored)) {
            continue;
        }
        std::string compact_output;
        std::string analysis_bytes;
        std::string analysis_digest;
        if (!split_render_output_and_music(
                canvas, export_config, compact_output,
                analysis_bytes, analysis_digest, error)) return false;
        if (analysis_digest.empty()) continue;

        std::string reconstructed;
        std::string reconstructed_digest;
        if (!detail::serialize_render_output_config(
                canvas, export_config, reconstructed, error)
            || !detail::sha256_hex(reconstructed,
                                   reconstructed_digest, error)) return false;
        if (reconstructed_digest != manifest.render_output_digest) {
            continue;
        }
        if (!stage_music_analysis(
                files, analysis_bytes, analysis_digest, error)) return false;
        std::string reference;
        if (!serialize_music_analysis_reference(
                analysis_digest, reference, error)) return false;
        files.files[output_path] = std::move(compact_output);
        files.files[reference_path] = std::move(reference);
        files.transactional_updates.insert(output_path);
        files.transactional_updates.insert(reference_path);
        version.changed_since_recorded = true;
        compacted = true;
    }
    return true;
}

bool write_root_files(const ProjectDocument& document,
                      const std::vector<BundleVersionInfo>& versions,
                      std::uint64_t current_version,
                      const RootMetadata* previous_root,
                      detail::BundleFileSet& files,
                      std::string* error) {
    RootMetadata root;
    root.project_uuid = document.project.uuid;
    root.project_name = document.project.name;
    root.first_created_utc = document.first_created_utc;
    root.last_opened_utc = document.last_opened_utc;
    root.last_saved_utc = document.last_saved_utc;
    root.created_with_version = document.created_with_version;
    root.last_changed_with_version = document.last_changed_with_version;
    if (previous_root != nullptr) {
        root.version_digests = previous_root->version_digests;
        root.version_tree_digests = previous_root->version_tree_digests;
        root.lineage_aliases = previous_root->lineage_aliases;
        root.preserved_versions = previous_root->preserved_versions;
    }
    const BundleVersionInfo* current = nullptr;
    for (const BundleVersionInfo& version : versions) {
        if (version.valid && version.indexed) {
            std::string lineage_digest;
            const auto preserved = root.preserved_versions.find(version.number);
            if (preserved != root.preserved_versions.end()
                && !preserved->second.lineage_aliases.empty()) {
                lineage_digest = *preserved->second.lineage_aliases.begin();
            }
            root.preserved_versions.erase(version.number);
            if (root.version_digests.find(version.number)
                == root.version_digests.end()) {
                root.version_digests.emplace(
                    version.number,
                    lineage_digest.empty() ? version.metadata_digest
                                           : std::move(lineage_digest));
            }
        }
        if (version.valid && version.indexed
            && (version.changed_since_recorded
                || root.version_tree_digests.find(version.number)
                       == root.version_tree_digests.end())) {
            std::string tree_digest;
            if (!version_tree_digest(files, version.number, tree_digest, error)) {
                return false;
            }
            root.version_tree_digests[version.number] = std::move(tree_digest);
        }
        if (version.valid && version.number == current_version) current = &version;
    }
    if (!validate_history_accounting(files, root, error)) return false;
    if (current == nullptr) {
        return fail(error, "Cannot write current pointer to an unknown version.");
    }
    const auto indexed_current = root.version_digests.find(current_version);
    if (indexed_current == root.version_digests.end()
        || indexed_current->second != current->metadata_digest) {
        return fail(error, "Cannot make an unindexed or checksum-mismatched version current.");
    }
    std::string metadata;
    std::string metadata_digest;
    std::string checksum;
    std::string current_bytes;
    if (!serialize_root_metadata(root, metadata, error)
        || !detail::sha256_hex(metadata, metadata_digest, error)
        || !serialize_checksum(metadata_digest, checksum, error)
        || !serialize_current(current_version, indexed_current->second,
                              current_bytes, error)) return false;
    files.files["metadata.txt"] = std::move(metadata);
    files.files["metadata.sha256"] = std::move(checksum);
    files.files["current"] = std::move(current_bytes);
    return true;
}

const BundleVersionInfo* find_version(const ProjectDocument& document,
                                      std::uint64_t number) {
    const auto found = std::find_if(
        document.versions.begin(), document.versions.end(),
        [number](const BundleVersionInfo& version) { return version.number == number; });
    return found == document.versions.end() ? nullptr : &*found;
}

std::string basename_without_extension(const std::string& path) {
    fs::path native = detail::path_from_utf8(path);
    std::string name = detail::path_to_utf8(native.stem());
    if (!valid_semantic_project_name(name)) name = "Imported Fire";
    return name;
}

} // namespace

ProjectAttachmentCache::~ProjectAttachmentCache() {
    if (directory.empty()) return;
    std::error_code filesystem_error;
    const std::filesystem::path native = detail::path_from_utf8(directory);
    const std::filesystem::path temporary_root =
        std::filesystem::temp_directory_path(filesystem_error);
    if (filesystem_error || native.parent_path() != temporary_root
        || detail::path_to_utf8(native.filename()).rfind("pvt-asset-cache-", 0U)
               != 0U) {
        return;
    }
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(native, filesystem_error);
    if (filesystem_error || !std::filesystem::is_directory(status)
        || std::filesystem::is_symlink(status)) {
        return;
    }
    for (std::filesystem::directory_iterator iterator(native, filesystem_error), end;
         !filesystem_error && iterator != end; ++iterator) {
        const std::filesystem::file_status entry_status =
            std::filesystem::symlink_status(iterator->path(), filesystem_error);
        if (filesystem_error || !std::filesystem::is_regular_file(entry_status)
            || std::filesystem::is_symlink(entry_status)) {
            return; // Do not traverse or remove a cache directory that was altered.
        }
    }
    if (!filesystem_error) {
        std::filesystem::remove_all(native, filesystem_error);
    }
}

ProjectDocument default_project_document() {
    ProjectDocument document;
    document.project = default_project();
    const std::string now = utc_now();
    document.bundle_root_name = portable_root_name(document.project.name);
    document.first_created_utc = now;
    document.last_opened_utc = now;
    document.last_saved_utc.clear();
    document.created_with_version = PVT_PROGRAM_VERSION;
    document.last_changed_with_version = PVT_PROGRAM_VERSION;
    document.dirty = true;
    return document;
}

std::string surface_obj_attachment_id(const std::string& layer_uuid) {
    return "layer." + layer_uuid + ".surface.obj";
}

std::string layer_music_attachment_id(const std::string& layer_uuid) {
    return "layer." + layer_uuid + ".clock.music";
}

const ProjectAttachment* find_project_attachment(
    const ProjectDocument& document,
    const std::string& reference_id) {
    const auto found = std::find_if(
        document.attachments.begin(), document.attachments.end(),
        [&reference_id](const ProjectAttachment& attachment) {
            return attachment.reference_id == reference_id;
        });
    return found == document.attachments.end() ? nullptr : &*found;
}

std::string project_attachment_path(const ProjectDocument& document,
                                    const std::string& reference_id) {
    const ProjectAttachment* attachment =
        find_project_attachment(document, reference_id);
    return attachment == nullptr ? std::string{} : attachment->local_path;
}

bool attach_project_file(ProjectDocument& document,
                         const std::string& reference_id,
                         const std::string& source_path,
                         ProjectAttachment* attached,
                         std::string* error) {
    clear_error(error);
    try {
        if (!valid_attachment_reference_id(reference_id)) {
            return fail(error, "Attachment reference ID is invalid.");
        }
        std::string basename;
        std::string bytes;
        std::string digest;
        if (!read_attachment_source(source_path, basename, bytes, digest, error)) {
            return false;
        }
        std::shared_ptr<ProjectAttachmentCache> cache = document.attachment_cache;
        ProjectAttachment candidate;
        candidate.reference_id = reference_id;
        candidate.sha256 = digest;
        candidate.basename = basename;
        candidate.size_bytes = static_cast<std::uint64_t>(bytes.size());
        if (!materialize_attachment_bytes(cache, digest, basename, bytes,
                                          candidate.local_path, error)) {
            return false;
        }
        auto existing = std::find_if(
            document.attachments.begin(), document.attachments.end(),
            [&reference_id](const ProjectAttachment& attachment) {
                return attachment.reference_id == reference_id;
            });
        const bool changed = existing == document.attachments.end()
                             || existing->sha256 != candidate.sha256
                             || existing->basename != candidate.basename
                             || existing->size_bytes != candidate.size_bytes;
        if (existing == document.attachments.end()) {
            if (document.attachments.size()
                >= kMaximumProjectAttachmentReferences) {
                return fail(error,
                            "Project has reached the attachment-reference limit.");
            }
            document.attachments.push_back(candidate);
        } else {
            *existing = candidate;
        }
        document.attachment_cache = std::move(cache);
        document.dirty = document.dirty || changed;
        if (attached != nullptr) *attached = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to attach project file.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected attachment error: ")
                               + exception.what());
    }
}

bool detach_project_file(ProjectDocument& document,
                         const std::string& reference_id,
                         std::string* error) {
    clear_error(error);
    try {
        if (!valid_attachment_reference_id(reference_id)) {
            return fail(error, "Attachment reference ID is invalid.");
        }
        const std::size_t before = document.attachments.size();
        document.attachments.erase(
            std::remove_if(
                document.attachments.begin(), document.attachments.end(),
                [&reference_id](const ProjectAttachment& attachment) {
                    return attachment.reference_id == reference_id;
                }),
            document.attachments.end());
        if (document.attachments.size() != before) document.dirty = true;
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to detach project file.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected detach error: ")
                               + exception.what());
    }
}

bool make_independent_project_copy(const ProjectConfig& project,
                                   ProjectDocument& destination,
                                   std::string* error) {
    clear_error(error);
    try {
        const ValidationResult validation = validate(project);
        if (!validation.ok || !valid_semantic_project_name(project.name)) {
            return fail(error, "Cannot copy invalid project: "
                                   + validation.message);
        }
        const bool has_embedded_identity =
            !project.canvas.clock.music.source_sha256.empty()
            || std::any_of(project.layers.begin(), project.layers.end(),
                           [](const LayerConfig& layer) {
                               return !layer.render.surface.obj_sha256.empty()
                                      || !layer.render.layer_clock.clock.music
                                              .source_sha256.empty();
                           });
        if (has_embedded_identity) {
            return fail(error,
                        "Attachment-bearing snapshots must be copied from ProjectDocument so their bytes are retained.");
        }

        ProjectDocument candidate = default_project_document();
        candidate.project = project;
        std::unordered_set<std::string> reserved_uuids;
        reserved_uuids.reserve(project.layers.size() * 2U + 2U);
        reserved_uuids.insert(project.uuid);
        for (const LayerConfig& layer : project.layers) {
            reserved_uuids.insert(layer.uuid);
        }
        const auto fresh_uuid = [&reserved_uuids]() {
            for (int attempt = 0; attempt < 128; ++attempt) {
                std::string candidate_uuid = generate_uuid();
                if (reserved_uuids.insert(candidate_uuid).second) {
                    return candidate_uuid;
                }
            }
            return std::string{};
        };
        candidate.project.uuid = fresh_uuid();
        for (std::size_t index = 0U;
             index < candidate.project.layers.size(); ++index) {
            candidate.project.layers[index].uuid = fresh_uuid();
            // File IDs are bundle-local stable identities. A detached copy has
            // no history to preserve, so give its sole initial snapshot a
            // compact, deterministic file layout.
            candidate.project.layers[index].file_id =
                static_cast<std::uint64_t>(index);
        }
        candidate.bundle_root_name = portable_root_name(candidate.project.name);
        candidate.dirty = true;

        const ValidationResult copied_validation = validate(candidate.project);
        if (!copied_validation.ok) {
            return fail(error, "Could not assign independent project identities: "
                                   + copied_validation.message);
        }
        destination = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to create an independent project copy.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected independent-copy error: ")
                               + exception.what());
    }
}

bool make_independent_project_copy(const ProjectDocument& source,
                                   ProjectDocument& destination,
                                   std::string* error) {
    clear_error(error);
    try {
        ProjectConfig attachment_free = source.project;
        attachment_free.canvas.clock.music.source_sha256.clear();
        attachment_free.canvas.clock.music.source_basename.clear();
        if (attachment_free.canvas.clock.mode == ClockMode::Music) {
            attachment_free.canvas.clock.mode = ClockMode::Default;
        }
        for (LayerConfig& layer : attachment_free.layers) {
            layer.render.surface.obj_sha256.clear();
            layer.render.surface.obj_basename.clear();
            layer.render.layer_clock.clock.music.source_sha256.clear();
            layer.render.layer_clock.clock.music.source_basename.clear();
            if (layer.render.layer_clock.clock.mode == ClockMode::Music) {
                layer.render.layer_clock.clock.mode = ClockMode::Default;
            }
        }
        ProjectDocument candidate;
        if (!make_independent_project_copy(attachment_free, candidate, error)) {
            return false;
        }
        // Restore the complete snapshot after independent identities have been
        // assigned, then remap stable layer-scoped attachment references by
        // layer position.
        candidate.project.canvas.clock = source.project.canvas.clock;
        candidate.project.output = source.project.output;
        candidate.attachments = source.attachments;
        candidate.attachment_cache = source.attachment_cache;
        for (std::size_t index = 0U; index < source.project.layers.size(); ++index) {
            candidate.project.layers[index].render =
                source.project.layers[index].render;
            const std::string old_id =
                surface_obj_attachment_id(source.project.layers[index].uuid);
            const std::string new_id =
                surface_obj_attachment_id(candidate.project.layers[index].uuid);
            const std::string old_music_id =
                layer_music_attachment_id(source.project.layers[index].uuid);
            const std::string new_music_id =
                layer_music_attachment_id(candidate.project.layers[index].uuid);
            for (ProjectAttachment& attachment : candidate.attachments) {
                if (attachment.reference_id == old_id) {
                    attachment.reference_id = new_id;
                } else if (attachment.reference_id == old_music_id) {
                    attachment.reference_id = new_music_id;
                }
            }
        }
        if (!sync_project_attachment_references(candidate, error)) return false;
        candidate.source_path.clear();
        candidate.imported_from_path.clear();
        candidate.loaded_snapshot_digest.clear();
        candidate.loaded_bundle_state_digest.clear();
        candidate.versions.clear();
        candidate.current_version = 0U;
        candidate.source_is_zip = false;
        candidate.legacy_import = false;
        candidate.externally_modified = false;
        candidate.newer_program_version = false;
        candidate.dirty = true;
        destination = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error,
                    "Not enough memory to copy project attachments independently.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected attachment-copy error: ")
                               + exception.what());
    }
}

bool import_legacy_setup(const std::string& path,
                         ProjectDocument& destination,
                         std::string* error) {
    clear_error(error);
    try {
        RenderConfig legacy;
        if (!load_setup(path, legacy, error)) return false;
        ProjectDocument candidate = default_project_document();
        candidate.project.uuid = generate_uuid();
        candidate.project.name = basename_without_extension(path);
        candidate.project.canvas.width = legacy.width;
        candidate.project.canvas.height = legacy.height;
        candidate.project.canvas.block_size = legacy.block_size;
        candidate.project.canvas.total_frames = legacy.total_frames;
        candidate.project.canvas.fps = legacy.fps;
        candidate.project.output = legacy.output;
        candidate.project.output.write_alpha =
            legacy.output.write_alpha || legacy.alpha.enabled;
        candidate.project.layers.clear();
        LayerConfig layer;
        layer.uuid = generate_uuid();
        layer.file_id = 0U;
        layer.name = "Layer 1";
        layer.render = static_cast<const RenderData&>(legacy);
        candidate.project.layers.push_back(std::move(layer));
        const ValidationResult validation = validate(candidate.project);
        if (!validation.ok) {
            return fail(error, "Imported setup is not a valid project: "
                                   + validation.message);
        }
        candidate.imported_from_path = path;
        candidate.source_path.clear();
        candidate.legacy_import = true;
        candidate.dirty = true;
        candidate.bundle_root_name = portable_root_name(candidate.project.name);
        destination = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to import legacy setup.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected legacy import error: ")
                               + exception.what());
    }
}

bool load_project_document(const std::string& path,
                           ProjectDocument& destination,
                           std::string* error) {
    clear_error(error);
    try {
        if (!detail::path_is_zip_bundle(path)) {
            std::error_code status_error;
            const fs::path native = detail::path_from_utf8(path);
            if (fs::is_regular_file(fs::symlink_status(native, status_error))
                && ascii_case_suffix(path, ".pvt")) {
                return import_legacy_setup(path, destination, error);
            }
        }
        detail::BundleFileSet files;
        RootMetadata root;
        bool root_external = false;
        if (!read_document_source(path, files, root, root_external, error)) return false;
        std::string bundle_state_digest;
        if (!detail::bundle_file_set_digest(files, bundle_state_digest, error)) return false;
        std::string ignored_accounting_error;
        const bool history_fully_accounted =
            validate_history_accounting(files, root, &ignored_accounting_error);

        std::vector<std::uint64_t> candidates;
        std::uint64_t current = 0U;
        std::string current_digest;
        std::string ignored_current_error;
        const bool current_valid =
            current_candidate(files, current, current_digest, &ignored_current_error);
        if (current_valid) {
            candidates.push_back(current);
        }
        const std::set<std::uint64_t> numeric_versions =
            numeric_version_directories(files);
        for (auto iterator = numeric_versions.rbegin();
             iterator != numeric_versions.rend(); ++iterator) {
            if (candidates.empty() || *iterator != candidates.front()) {
                candidates.push_back(*iterator);
            }
        }

        std::string last_failure = "Bundle has no versions.";
        BundleValidationCache validation_cache;
        for (const std::uint64_t candidate_number : candidates) {
            ProjectConfig project;
            std::vector<ProjectAttachment> snapshot_attachments;
            BundleVersionInfo info;
            std::string semantic_digest;
            bool version_external = false;
            std::string load_error;
            if (!load_snapshot(files, root, candidate_number, project, info,
                               semantic_digest, version_external, &load_error,
                               &snapshot_attachments, &validation_cache)) {
                last_failure = std::move(load_error);
                continue;
            }
            bool display_name_external = false;
            if (current_valid && candidate_number == current) {
                display_name_external = project.name != root.project_name;
                if (display_name_external) {
                    project.name = root.project_name;
                    if (!project_content_digest(project, snapshot_attachments,
                                                semantic_digest,
                                                &load_error)) {
                        last_failure = std::move(load_error);
                        continue;
                    }
                }
            }
            const bool pointer_external = !current_valid
                                          || candidate_number != current
                                          || current_digest != info.metadata_digest;
            ProjectDocument document;
            if (!materialize_snapshot_attachments(
                    files, project, snapshot_attachments,
                    document.attachment_cache, &load_error)) {
                last_failure = std::move(load_error);
                continue;
            }
            if (!project_content_digest(project, snapshot_attachments,
                                        semantic_digest, &load_error)) {
                last_failure = std::move(load_error);
                continue;
            }
            document.project = std::move(project);
            document.attachments = std::move(snapshot_attachments);
            document.source_path = path;
            document.bundle_root_name = files.root_name;
            document.first_created_utc = root.first_created_utc;
            document.last_opened_utc = utc_now();
            document.last_saved_utc = root.last_saved_utc;
            document.created_with_version = root.created_with_version;
            document.last_changed_with_version = root.last_changed_with_version;
            document.loaded_snapshot_digest = std::move(semantic_digest);
            document.loaded_bundle_state_digest = bundle_state_digest;
            document.current_version = candidate_number;
            document.source_is_zip = files.from_zip;
            document.externally_modified = root_external || version_external
                                           || pointer_external
                                           || display_name_external
                                           || !history_fully_accounted;
            document.dirty = document.externally_modified;
            document.newer_program_version =
                program_version_is_newer(root.created_with_version)
                || program_version_is_newer(root.last_changed_with_version);
            if (!collect_version_infos(files, root, document.versions, error,
                                       &validation_cache)) return false;
            const bool unrecorded_history_change = std::any_of(
                document.versions.begin(), document.versions.end(),
                [](const BundleVersionInfo& value) {
                    return value.changed_since_recorded;
                });
            document.externally_modified = document.externally_modified
                                           || unrecorded_history_change;
            document.dirty = document.externally_modified;
            auto selected = std::find_if(
                document.versions.begin(), document.versions.end(),
                [candidate_number](const BundleVersionInfo& value) {
                    return value.number == candidate_number;
                });
            if (selected == document.versions.end()) {
                info.indexed = root.version_digests.find(candidate_number)
                               != root.version_digests.end();
                info.externally_modified = version_external || !info.indexed;
                document.versions.push_back(info);
                std::sort(document.versions.begin(), document.versions.end(),
                          [](const BundleVersionInfo& a, const BundleVersionInfo& b) {
                              return a.number < b.number;
                          });
            }
            destination = std::move(document);
            return true;
        }
        return fail(error, "No valid bundle version could be loaded. Last failure: "
                               + last_failure);
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to load project document.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected project load error: ")
                               + exception.what());
    }
}

bool load_project_version(const ProjectDocument& document,
                          std::uint64_t version,
                          ProjectConfig& destination,
                          std::string* error) {
    clear_error(error);
    try {
        if (document.source_path.empty() || document.legacy_import) {
            return fail(error, "Unsaved or legacy-imported documents have no bundle versions.");
        }
        detail::BundleFileSet files;
        RootMetadata root;
        bool root_external = false;
        if (!read_document_source(document.source_path, files, root,
                                  root_external, error)) return false;
        std::string actual_state;
        if (!detail::bundle_file_set_digest(files, actual_state, error)) return false;
        if (document.loaded_bundle_state_digest.empty()
            || actual_state != document.loaded_bundle_state_digest) {
            return fail(error,
                        "Project changed on disk since it was loaded; refusing stale version read.");
        }
        ProjectConfig candidate;
        std::vector<ProjectAttachment> snapshot_attachments;
        BundleVersionInfo info;
        std::string digest;
        bool version_external = false;
        if (!load_snapshot(files, root, version, candidate, info, digest,
                           version_external, error,
                           &snapshot_attachments)
            || !materialize_snapshot_attachments(
                files, candidate, snapshot_attachments,
                document.attachment_cache, error)) {
            return false;
        }
        destination = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to load project version.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected version load error: ")
                               + exception.what());
    }
}

namespace {

bool add_codec_fields(const std::string& prefix, const std::string& bytes,
                      std::map<std::string, std::string>& fields) {
    std::size_t start = bytes.find('\n');
    if (start == std::string::npos) return false;
    ++start;
    while (start < bytes.size()) {
        const std::size_t newline = bytes.find('\n', start);
        const std::size_t end = newline == std::string::npos ? bytes.size() : newline;
        const std::string_view line(bytes.data() + start, end - start);
        const std::size_t tab = line.find('\t');
        if (tab == std::string_view::npos) return false;
        fields[prefix + std::string(line.substr(0U, tab))] =
            std::string(line.substr(tab + 1U));
        if (newline == std::string::npos) break;
        start = newline + 1U;
    }
    return true;
}

bool semantic_fields(const ProjectConfig& project,
                     std::map<std::string, std::string>& fields,
                     std::string* error) {
    fields["project.uuid"] = project.uuid;
    fields["project.name"] = project.name;
    std::string output;
    if (!detail::serialize_render_output_config(project.canvas, project.output,
                                                output, error)
        || !add_codec_fields("global.", output, fields)) {
        return fail(error, "Could not build semantic output diff.");
    }
    // The split codecs percent-encode arbitrary strings so their on-disk
    // records stay one-line and deterministic. Semantic diffs are a user-facing
    // view, however, so replace only the known string-valued records with their
    // typed values. Blindly decoding every value would corrupt legitimate '%'
    // sequences in future enum or numeric fields.
    fields["global.output.output_directory"] = project.output.output_directory;
    fields["global.output.filename_prefix"] = project.output.filename_prefix;
    fields["global.timing.clock.meter.expression"] =
        project.canvas.clock.meter.expression;
    fields["global.timing.music.analyzer_version"] =
        project.canvas.clock.music.analyzer_version;
    fields["global.timing.music.source_sha256"] =
        project.canvas.clock.music.source_sha256;
    fields["global.timing.music.source_basename"] =
        project.canvas.clock.music.source_basename;
    fields["global.timing.music.source_format"] =
        project.canvas.clock.music.source_format;
    for (std::size_t index = 0U; index < project.layers.size(); ++index) {
        const LayerConfig& layer = project.layers[index];
        const std::string layer_prefix = "layer." + layer.uuid + ".";
        fields["order." + std::to_string(index)] = layer.uuid;
        fields[layer_prefix + "file_id"] = std::to_string(layer.file_id);
        fields[layer_prefix + "name"] = layer.name;
        fields[layer_prefix + "enabled"] = layer.enabled ? "1" : "0";
        fields[layer_prefix + "blend_mode"] = blend_token(layer.blend_mode);
        std::ostringstream opacity;
        opacity.imbue(std::locale::classic());
        opacity << std::setprecision(std::numeric_limits<double>::max_digits10)
                << layer.opacity;
        fields[layer_prefix + "opacity"] = opacity.str();
        std::string render;
        if (!detail::serialize_layer_config(layer.render, render, error)
            || !add_codec_fields(layer_prefix + "render.", render, fields)) {
            return fail(error, "Could not build semantic layer diff.");
        }
        const std::string render_prefix = layer_prefix + "render.";
        fields[render_prefix + "surface.obj_path"] =
            layer.render.surface.obj_sha256.empty()
                ? layer.render.surface.obj_path : std::string{};
        fields[render_prefix + "surface.obj_sha256"] =
            layer.render.surface.obj_sha256;
        fields[render_prefix + "surface.obj_basename"] =
            layer.render.surface.obj_basename;
        fields[render_prefix + "palette.name"] = layer.render.palette.name;
        for (std::size_t wave = 0U; wave < layer.render.waves.size(); ++wave) {
            fields[render_prefix + "waves." + std::to_string(wave) + ".name"] =
                layer.render.waves[wave].name;
        }
        for (std::size_t swing = 0U; swing < layer.render.swings.size(); ++swing) {
            fields[render_prefix + "swings." + std::to_string(swing) + ".name"] =
                layer.render.swings[swing].name;
        }
        for (std::size_t effect = 0U; effect < layer.render.effects.size(); ++effect) {
            fields[render_prefix + "effects." + std::to_string(effect) + ".name"] =
                layer.render.effects[effect].name;
        }
    }
    return true;
}

bool equivalent_path(const std::string& first, const std::string& second) {
    if (first.empty() || second.empty()) return false;
    std::error_code error;
    const fs::path first_absolute = fs::absolute(detail::path_from_utf8(first), error)
                                        .lexically_normal();
    if (error) return first == second;
    const fs::path second_absolute = fs::absolute(detail::path_from_utf8(second), error)
                                         .lexically_normal();
    return !error && first_absolute == second_absolute;
}

bool sync_project_attachment_references(ProjectDocument& document,
                                        std::string* error) {
    if (document.attachments.size() > kMaximumProjectAttachmentReferences) {
        return fail(error, "Project exceeds the attachment-reference limit.");
    }
    std::set<std::string> expected_obj_references;
    for (LayerConfig& layer : document.project.layers) {
        SurfaceConfig& surface = layer.render.surface;
        const std::string reference_id = surface_obj_attachment_id(layer.uuid);
        expected_obj_references.insert(reference_id);
        auto existing = std::find_if(
            document.attachments.begin(), document.attachments.end(),
            [&reference_id](const ProjectAttachment& attachment) {
                return attachment.reference_id == reference_id;
            });
        if (surface.obj_path.empty()) {
            if (!surface.obj_sha256.empty()
                && existing != document.attachments.end()
                && existing->sha256 == surface.obj_sha256
                && existing->basename == surface.obj_basename
                && !existing->local_path.empty()) {
                surface.obj_path = existing->local_path;
                continue;
            }
            surface.obj_sha256.clear();
            surface.obj_basename.clear();
            if (!detach_project_file(document, reference_id, error)) return false;
            continue;
        }
        if (existing != document.attachments.end()
            && existing->sha256 == surface.obj_sha256
            && existing->basename == surface.obj_basename
            && !existing->local_path.empty()
            && equivalent_path(surface.obj_path, existing->local_path)) {
            continue;
        }
        std::error_code status_error;
        const fs::file_status source_status = fs::symlink_status(
            detail::path_from_utf8(surface.obj_path), status_error);
        if ((status_error || !fs::is_regular_file(source_status)
            || fs::is_symlink(source_status)
            || attachment_path_is_reparse_point(
                detail::path_from_utf8(surface.obj_path)))
            && existing != document.attachments.end()
            && existing->sha256 == surface.obj_sha256
            && existing->basename == surface.obj_basename
            && !existing->local_path.empty()) {
            surface.obj_path = existing->local_path;
            continue;
        }
        ProjectAttachment attached;
        if (!attach_project_file(document, reference_id, surface.obj_path,
                                 &attached, error)) {
            return false;
        }
        surface.obj_sha256 = attached.sha256;
        surface.obj_basename = attached.basename;
        // Render from the managed copy immediately. This also makes deleting or
        // moving the selected original before Save harmless.
        surface.obj_path = attached.local_path;
    }
    document.attachments.erase(
        std::remove_if(
            document.attachments.begin(), document.attachments.end(),
            [&expected_obj_references](const ProjectAttachment& attachment) {
                return attachment.reference_id.rfind("layer.", 0U) == 0U
                       && attachment.reference_id.size() > 12U
                       && attachment.reference_id.compare(
                              attachment.reference_id.size() - 12U, 12U,
                              ".surface.obj") == 0
                       && expected_obj_references.find(attachment.reference_id)
                              == expected_obj_references.end();
            }),
        document.attachments.end());

    std::set<std::string> expected_layer_music_references;
    for (LayerConfig& layer : document.project.layers) {
        const std::string reference_id = layer_music_attachment_id(layer.uuid);
        expected_layer_music_references.insert(reference_id);
        const MusicAnalysis& analysis = layer.render.layer_clock.clock.music;
        auto existing = std::find_if(
            document.attachments.begin(), document.attachments.end(),
            [&reference_id](const ProjectAttachment& attachment) {
                return attachment.reference_id == reference_id;
            });
        if (analysis.source_sha256.empty()) {
            if (!detach_project_file(document, reference_id, error)) return false;
            continue;
        }
        if (existing == document.attachments.end()) {
            const auto same_bytes = std::find_if(
                document.attachments.begin(), document.attachments.end(),
                [&analysis](const ProjectAttachment& attachment) {
                    return attachment.sha256 == analysis.source_sha256
                           && attachment.basename == analysis.source_basename;
                });
            if (same_bytes == document.attachments.end()) {
                return fail(error,
                            "Active-layer music analysis source has not been attached to the project.");
            }
            ProjectAttachment alias = *same_bytes;
            alias.reference_id = reference_id;
            document.attachments.push_back(std::move(alias));
            existing = std::prev(document.attachments.end());
        }
        if (existing->sha256 != analysis.source_sha256
            || existing->basename != analysis.source_basename) {
            return fail(error,
                        "Active-layer music analysis does not match its attached source file.");
        }
    }
    document.attachments.erase(
        std::remove_if(
            document.attachments.begin(), document.attachments.end(),
            [&expected_layer_music_references](
                const ProjectAttachment& attachment) {
                constexpr std::string_view suffix = ".clock.music";
                return attachment.reference_id.rfind("layer.", 0U) == 0U
                       && attachment.reference_id.size() > suffix.size()
                       && attachment.reference_id.compare(
                              attachment.reference_id.size() - suffix.size(),
                              suffix.size(), suffix) == 0
                       && expected_layer_music_references.find(
                              attachment.reference_id)
                              == expected_layer_music_references.end();
            }),
        document.attachments.end());

    const std::string& music_digest =
        document.project.canvas.clock.music.source_sha256;
    auto music = std::find_if(
        document.attachments.begin(), document.attachments.end(),
        [](const ProjectAttachment& attachment) {
            return attachment.reference_id == kMusicSourceAttachmentId;
        });
    if (music_digest.empty()) {
        if (!detach_project_file(document, kMusicSourceAttachmentId, error)) {
            return false;
        }
    } else {
        if (music == document.attachments.end()) {
            const auto same_bytes = std::find_if(
                document.attachments.begin(), document.attachments.end(),
                [&music_digest](const ProjectAttachment& attachment) {
                    return attachment.sha256 == music_digest;
                });
            if (same_bytes == document.attachments.end()) {
                return fail(error,
                            "Music analysis source has not been attached to the project.");
            }
            ProjectAttachment alias = *same_bytes;
            alias.reference_id = kMusicSourceAttachmentId;
            document.attachments.push_back(std::move(alias));
            music = std::prev(document.attachments.end());
        }
        const MusicAnalysis& analysis = document.project.canvas.clock.music;
        if (music->sha256 != analysis.source_sha256
            || music->basename != analysis.source_basename) {
            return fail(error,
                        "Music analysis does not match its attached source file.");
        }
    }

    std::set<std::string> reference_ids;
    for (const ProjectAttachment& attachment : document.attachments) {
        if (!valid_attachment_reference_id(attachment.reference_id)
            || !canonical_hash(attachment.sha256)
            || !valid_attachment_basename(attachment.basename)
            || attachment.size_bytes > kMaximumProjectAttachmentBytes
            || !reference_ids.insert(attachment.reference_id).second) {
            return fail(error, "Project contains invalid attachment metadata.");
        }
    }
    std::sort(document.attachments.begin(), document.attachments.end(),
              [](const ProjectAttachment& left,
                 const ProjectAttachment& right) {
                  return left.reference_id < right.reference_id;
              });
    return true;
}

bool stage_attachment_assets(const ProjectDocument& document,
                             detail::BundleFileSet& files,
                             std::string* error) {
    std::set<std::string> staged;
    for (const ProjectAttachment& attachment : document.attachments) {
        const std::string asset_path = attachment_asset_path(
            attachment.sha256, attachment.basename);
        if (!staged.insert(asset_path).second) continue;
        const auto existing = files.files.find(asset_path);
        if (existing != files.files.end()) {
            std::string actual;
            if (existing->second.size() != attachment.size_bytes
                || !detail::sha256_hex(existing->second, actual, error)
                || actual != attachment.sha256) {
                return fail(error,
                            "Existing embedded asset does not match its content identity.");
            }
            continue;
        }
        if (attachment.local_path.empty()) {
            return fail(error, "New project attachment has no readable local source.");
        }
        std::string ignored_basename;
        std::string bytes;
        std::string digest;
        if (!read_attachment_source(attachment.local_path, ignored_basename,
                                    bytes, digest, error)) {
            return false;
        }
        if (digest != attachment.sha256
            || bytes.size() != attachment.size_bytes) {
            return fail(error,
                        "Materialized attachment changed before it could be saved.");
        }
        files.files.emplace(asset_path, std::move(bytes));
    }
    return true;
}

bool target_exists(const std::string& path) {
    std::error_code error;
    return fs::exists(fs::symlink_status(detail::path_from_utf8(path), error));
}

bool save_with_reason(ProjectDocument& document,
                      const std::string& path,
                      const std::string& reason_override,
                      const std::string& reverted_from,
                      BundleSaveReport* report,
                      std::string* error) {
    ProjectDocument working = document;
    if (path.empty() || !valid_semantic_project_name(working.project.name)) {
        return fail(error, "Project name or save path is not portable.");
    }
    if (!sync_project_attachment_references(working, error)) return false;
    const ValidationResult validation = validate(working.project);
    if (!validation.ok) {
        return fail(error, "Cannot save invalid project: " + validation.message);
    }
    const bool same_source = equivalent_path(working.source_path, path);
    const bool destination_exists = target_exists(path);
    if (!detail::path_is_zip_bundle(path) && !destination_exists) {
        const std::string directory_name = detail::path_to_utf8(
            detail::path_from_utf8(path).filename());
        if (directory_name != portable_root_name(working.project.name)) {
            return fail(error, "Unpacked bundle directory name must match project name.");
        }
    }

    detail::BundleFileSet files;
    RootMetadata root;
    bool root_external = false;
    bool have_root = false;
    std::vector<BundleVersionInfo> versions;
    if (!working.source_path.empty() && !working.legacy_import) {
        if (!read_document_source(working.source_path, files, root,
                                  root_external, error)
            || !collect_version_infos(files, root, versions, error)) return false;
        std::string actual_state;
        if (!detail::bundle_file_set_digest(files, actual_state, error)) return false;
        if (working.loaded_bundle_state_digest.empty()
            || actual_state != working.loaded_bundle_state_digest) {
            return fail(error,
                        "Project changed on disk since it was loaded; refusing stale save.");
        }
        have_root = true;
    }
    if (destination_exists && !same_source) {
        detail::BundleFileSet target_files;
        RootMetadata target_root;
        bool target_external = false;
        std::vector<BundleVersionInfo> target_versions;
        if (!read_document_source(path, target_files, target_root,
                                  target_external, error)
            || !collect_version_infos(target_files, target_root,
                                      target_versions, error)) return false;
        if (target_root.project_uuid != document.project.uuid) {
            return fail(error, "Save As destination belongs to a different project UUID.");
        }
        std::string target_state;
        if (!detail::bundle_file_set_digest(target_files, target_state, error)) return false;
        if (working.loaded_bundle_state_digest.empty()
            || target_state != working.loaded_bundle_state_digest) {
            return fail(error, "Save As destination has advanced or diverged on disk.");
        }
        files = std::move(target_files);
        root = std::move(target_root);
        root_external = target_external;
        versions = std::move(target_versions);
        have_root = true;
    }

    if (!stage_attachment_assets(working, files, error)) return false;
    std::string semantic_digest;
    if (!project_content_digest(working.project, working.attachments,
                                semantic_digest, error)) return false;
    const bool needs_version = versions.empty() || working.externally_modified
                               || root_external || working.legacy_import
                               || !reason_override.empty()
                               || semantic_digest != working.loaded_snapshot_digest;
    const std::string now = utc_now();
    if (working.last_opened_utc.empty()) working.last_opened_utc = now;
    working.last_saved_utc = now;
    working.last_changed_with_version = PVT_PROGRAM_VERSION;
    if (working.first_created_utc.empty()) working.first_created_utc = now;
    if (working.created_with_version.empty()) {
        working.created_with_version = PVT_PROGRAM_VERSION;
    }

    std::uint64_t current_version = working.current_version;
    bool promoted_external = false;
    if (needs_version) {
        if (have_root) {
            std::vector<std::uint64_t> missing_preserved;
            for (const auto& preserved : root.preserved_versions) {
                if (!version_directory_present(files, preserved.first)) {
                    missing_preserved.push_back(preserved.first);
                }
            }
            for (const std::uint64_t number : missing_preserved) {
                if (!preserve_raw_version(root, files, number, error)) return false;
            }
            for (BundleVersionInfo& version : versions) {
                const bool selected_valid_orphan =
                    version.valid && version.number == working.current_version;
                if (!version.valid
                    || (!version.indexed && !selected_valid_orphan)) {
                    // Preserve every noncanonical numeric directory exactly as
                    // observed. Invalid indexed ancestors retain their former
                    // metadata digest as a lineage alias for valid children.
                    if (!preserve_raw_version(root, files, version.number,
                                              error)) return false;
                    version.indexed = false;
                }
            }
        }
        const std::set<std::uint64_t> directory_versions =
            numeric_version_directories(files);
        if (directory_versions.size() >= kMaximumVersions) {
            return fail(error, "Bundle has reached the 4096-version history limit.");
        }
        const std::uint64_t number = directory_versions.empty()
                                         ? 0U : (*directory_versions.rbegin()
                                                  == std::numeric_limits<std::uint64_t>::max()
                                                      ? 0U
                                                      : *directory_versions.rbegin() + 1U);
        if (!directory_versions.empty() && number == 0U) {
            return fail(error, "Bundle version number space is exhausted.");
        }
        auto parent = std::find_if(
            versions.begin(), versions.end(), [&working](const BundleVersionInfo& value) {
                return value.number == working.current_version && value.valid;
            });
        if (!versions.empty() && parent == versions.end()) {
            return fail(error, "Current project version is missing from bundle history.");
        }
        if (parent != versions.end() && !parent->indexed) {
            // A valid crash-orphan selected during fallback becomes a first-class
            // origin before the canonical new version is appended.
            parent->indexed = true;
        }
        const std::string parent_digest = parent == versions.end()
                                              ? std::string{} : parent->metadata_digest;
        std::string reason = reason_override;
        if (reason.empty()) {
            reason = (working.externally_modified || root_external)
                         ? "external_change"
                     : working.legacy_import ? "legacy_import" : "save";
        }
        BundleVersionInfo new_version;
        if (!build_version(working.project, working.attachments,
                           number, parent_digest, reason,
                           reverted_from, files, new_version,
                           semantic_digest, error)) return false;
        versions.push_back(new_version);
        std::sort(versions.begin(), versions.end(),
                  [](const BundleVersionInfo& a, const BundleVersionInfo& b) {
                      return a.number < b.number;
                  });
        current_version = number;
        promoted_external = working.externally_modified || root_external;
    } else {
        if (!have_root
            || !validate_loaded_bundle_state(
                files, root, root_external, versions, error)) return false;
    }

    bool compacted_storage = false;
    if (!compact_embedded_music_analysis(
            files, versions, compacted_storage, error)) return false;

    if (!destination_exists) {
        files.root_name = portable_root_name(working.project.name);
    }
    if (!write_root_files(working, versions, current_version,
                          have_root ? &root : nullptr, files, error)) return false;
    if (!detail::write_bundle_file_set_if_unchanged(
            path, files, destination_exists,
            destination_exists ? working.loaded_bundle_state_digest
                               : std::string{},
            error)) return false;

    std::string committed_state_digest;
    if (!detail::bundle_file_set_digest(
            files, committed_state_digest, error)) return false;
    for (BundleVersionInfo& version : versions) {
        version.changed_since_recorded = false;
        if (version.valid && version.indexed
            && version.number == current_version) {
            version.externally_modified = false;
            version.integrity_message.clear();
        }
    }
    for (ProjectAttachment& attachment : working.attachments) {
        attachment.bundle_path = attachment_asset_path(
            attachment.sha256, attachment.basename);
        attachment.externally_modified = false;
    }
    working.source_path = path;
    working.imported_from_path.clear();
    working.bundle_root_name = files.root_name;
    working.loaded_snapshot_digest = semantic_digest;
    working.loaded_bundle_state_digest = std::move(committed_state_digest);
    working.current_version = current_version;
    working.versions = versions;
    working.source_is_zip = detail::path_is_zip_bundle(path);
    working.legacy_import = false;
    working.dirty = false;
    working.externally_modified = false;
    working.newer_program_version = false;
    BundleSaveReport completed_report;
    completed_report.path = path;
    completed_report.version = current_version;
    completed_report.created_version = needs_version;
    completed_report.validated_only = !needs_version;
    completed_report.compacted_storage = compacted_storage;
    completed_report.wrote_zip = detail::path_is_zip_bundle(path);
    completed_report.promoted_external_change = promoted_external;
    document = std::move(working);
    if (report != nullptr) *report = std::move(completed_report);
    return true;
}

} // namespace

bool diff_project_versions(const ProjectDocument& document,
                           std::uint64_t before,
                           std::uint64_t after,
                           std::vector<BundleDiffEntry>& destination,
                           std::string* error) {
    clear_error(error);
    try {
        ProjectConfig before_project;
        ProjectConfig after_project;
        if (!load_project_version(document, before, before_project, error)
            || !load_project_version(document, after, after_project, error)) return false;
        std::map<std::string, std::string> before_fields;
        std::map<std::string, std::string> after_fields;
        if (!semantic_fields(before_project, before_fields, error)
            || !semantic_fields(after_project, after_fields, error)) return false;
        std::set<std::string> keys;
        for (const auto& value : before_fields) keys.insert(value.first);
        for (const auto& value : after_fields) keys.insert(value.first);
        std::vector<BundleDiffEntry> differences;
        for (const std::string& key : keys) {
            const auto old_value = before_fields.find(key);
            const auto new_value = after_fields.find(key);
            const std::string old_text = old_value == before_fields.end()
                                             ? std::string{} : old_value->second;
            const std::string new_text = new_value == after_fields.end()
                                             ? std::string{} : new_value->second;
            if (old_value == before_fields.end() || new_value == after_fields.end()
                || old_text != new_text) {
                differences.push_back({key, old_text, new_text});
            }
        }
        destination = std::move(differences);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to diff project versions.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected version diff error: ")
                               + exception.what());
    }
}

bool save_project_document(ProjectDocument& document,
                           const std::string& path,
                           BundleSaveReport* report,
                           std::string* error) {
    clear_error(error);
    try {
        return save_with_reason(document, path, {}, {}, report, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to save project document.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected project save error: ")
                               + exception.what());
    }
}

bool make_project_version_current(ProjectDocument& document,
                                  std::uint64_t version,
                                  BundleSaveReport* report,
                                  std::string* error) {
    clear_error(error);
    try {
        if (document.source_path.empty() || document.legacy_import) {
            return fail(error, "Unsaved document has no current bundle pointer.");
        }
        detail::BundleFileSet files;
        RootMetadata root;
        bool root_external = false;
        if (!read_document_source(document.source_path, files, root,
                                  root_external, error)) return false;
        std::string actual_state;
        if (!detail::bundle_file_set_digest(files, actual_state, error)) return false;
        if (document.loaded_bundle_state_digest.empty()
            || actual_state != document.loaded_bundle_state_digest) {
            return fail(error,
                        "Project changed on disk since it was loaded; refusing stale current change.");
        }
        ProjectConfig project;
        std::vector<ProjectAttachment> snapshot_attachments;
        BundleVersionInfo info;
        std::string semantic_digest;
        bool version_external = false;
        if (!load_snapshot(files, root, version, project, info, semantic_digest,
                           version_external, error,
                           &snapshot_attachments)) return false;
        if (root_external || version_external) {
            return fail(error, "Externally modified version must be promoted by Save, not made current in place.");
        }
        std::vector<BundleVersionInfo> versions;
        if (!collect_version_infos(files, root, versions, error)) return false;
        ProjectDocument updated = document;
        if (!materialize_snapshot_attachments(
                files, project, snapshot_attachments,
                updated.attachment_cache, error)) {
            return false;
        }
        updated.project = std::move(project);
        updated.attachments = std::move(snapshot_attachments);
        updated.current_version = version;
        updated.loaded_snapshot_digest = std::move(semantic_digest);
        updated.versions = versions;
        updated.last_saved_utc = utc_now();
        updated.last_changed_with_version = PVT_PROGRAM_VERSION;
        updated.dirty = false;
        updated.externally_modified = false;
        if (!write_root_files(updated, versions, version, &root, files, error)
            || !detail::write_bundle_file_set_if_unchanged(
                updated.source_path, files, true, actual_state, error)) return false;
        ProjectDocument reloaded;
        if (!load_project_document(updated.source_path, reloaded, error)) return false;
        reloaded.last_opened_utc = updated.last_opened_utc;
        BundleSaveReport completed_report;
        completed_report.path = updated.source_path;
        completed_report.version = version;
        completed_report.created_version = false;
        completed_report.validated_only = false;
        completed_report.wrote_zip = updated.source_is_zip;
        document = std::move(reloaded);
        if (report != nullptr) *report = std::move(completed_report);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to change current version.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected make-current error: ")
                               + exception.what());
    }
}

bool revert_project_as_new(ProjectDocument& document,
                           std::uint64_t version,
                           BundleSaveReport* report,
                           std::string* error) {
    clear_error(error);
    try {
        if (document.source_path.empty() || document.legacy_import) {
            return fail(error, "Unsaved document has no version to revert.");
        }
        const BundleVersionInfo* selected = find_version(document, version);
        if (selected == nullptr) return fail(error, "Requested revert version is unknown.");
        detail::BundleFileSet files;
        RootMetadata root;
        bool root_external = false;
        if (!read_document_source(document.source_path, files, root,
                                  root_external, error)) return false;
        std::string actual_state;
        if (!detail::bundle_file_set_digest(files, actual_state, error)) return false;
        if (document.loaded_bundle_state_digest.empty()
            || actual_state != document.loaded_bundle_state_digest) {
            return fail(error,
                        "Project changed on disk since it was loaded; refusing stale revert.");
        }
        ProjectConfig project;
        std::vector<ProjectAttachment> snapshot_attachments;
        BundleVersionInfo loaded_info;
        std::string semantic_digest;
        bool version_external = false;
        if (!load_snapshot(files, root, version, project, loaded_info,
                           semantic_digest, version_external, error,
                           &snapshot_attachments)) return false;
        ProjectDocument candidate = document;
        if (!materialize_snapshot_attachments(
                files, project, snapshot_attachments,
                candidate.attachment_cache, error)) {
            return false;
        }
        candidate.project = std::move(project);
        candidate.attachments = std::move(snapshot_attachments);
        candidate.dirty = true;
        candidate.externally_modified = false;
        return save_with_reason(candidate, candidate.source_path, "revert",
                                selected->metadata_digest, report, error)
               && (document = std::move(candidate), true);
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to revert project.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected revert error: ")
                               + exception.what());
    }
}

bool validate_project_bundle(const std::string& path,
                             std::vector<BundleVersionInfo>* versions,
                             std::string* error) {
    clear_error(error);
    try {
        detail::BundleFileSet files;
        RootMetadata root;
        bool root_external = false;
        if (!read_document_source(path, files, root, root_external, error)) return false;
        std::vector<BundleVersionInfo> checked;
        if (!collect_version_infos(files, root, checked, error)
            || !validate_loaded_bundle_state(
                files, root, root_external, checked, error)) return false;
        if (versions != nullptr) *versions = std::move(checked);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to validate project bundle.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected bundle validation error: ")
                               + exception.what());
    }
}

std::string portable_project_filename(const std::string& project_name) {
    return portable_root_name(project_name) + ".zip";
}

} // namespace pvt
