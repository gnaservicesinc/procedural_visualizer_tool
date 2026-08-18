#include "palette_io.h"

#include "path_utf8.h"
#if defined(_WIN32)
#  include "windows_file_install.h"
#endif

#include "mz.h"
#include "mz_os.h"
#include "mz_strm.h"
#include "mz_strm_mem.h"
#include "mz_zip.h"
#include "mz_zip_rw.h"
#include <png.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace pvt::palette_io {
namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t kMaximumKplCompressionRatio = 1000U;
constexpr std::size_t kMaximumXmlTagBytes = 1U * 1024U * 1024U;
constexpr std::size_t kMaximumXmlAttributes = 64U;

bool fail(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
}

void clear_error(std::string* error) {
    if (error != nullptr) error->clear();
}

void add_warning(PaletteIoSummary& summary, const std::string& warning) {
    if (std::find(summary.warnings.begin(), summary.warnings.end(), warning)
        == summary.warnings.end()) {
        summary.warnings.push_back(warning);
    }
}

bool checked_multiply(std::size_t left, std::size_t right, std::size_t& result) {
    if (left != 0U && right > (std::numeric_limits<std::size_t>::max)() / left) {
        return false;
    }
    result = left * right;
    return true;
}

std::string lower_ascii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char raw) {
        const auto value = static_cast<unsigned char>(raw);
        return value >= static_cast<unsigned char>('A')
                       && value <= static_cast<unsigned char>('Z')
                   ? static_cast<char>(value - static_cast<unsigned char>('A')
                                       + static_cast<unsigned char>('a'))
                   : raw;
    });
    return text;
}

std::string_view trim_view(std::string_view text) {
    while (!text.empty()
           && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1U);
    }
    while (!text.empty()
           && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1U);
    }
    return text;
}

std::string trim_copy(std::string_view text) {
    const std::string_view trimmed = trim_view(text);
    return std::string(trimmed.data(), trimmed.size());
}

bool has_nul(std::string_view value) {
    return value.find('\0') != std::string_view::npos;
}

bool valid_utf8(std::string_view text) {
    std::size_t index = 0U;
    while (index < text.size()) {
        const unsigned char first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        std::size_t continuation = 0U;
        std::uint32_t codepoint = 0U;
        if (first >= 0xc2U && first <= 0xdfU) {
            continuation = 1U;
            codepoint = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            continuation = 2U;
            codepoint = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            continuation = 3U;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (index + continuation >= text.size()) return false;
        for (std::size_t offset = 1U; offset <= continuation; ++offset) {
            const unsigned char next = static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        if ((continuation == 2U && codepoint < 0x800U)
            || (continuation == 3U && codepoint < 0x10000U)
            || (codepoint >= 0xd800U && codepoint <= 0xdfffU)
            || codepoint > 0x10ffffU) {
            return false;
        }
        index += continuation + 1U;
    }
    return true;
}

bool has_forbidden_text_control(std::string_view text) {
    return std::any_of(text.begin(), text.end(), [](char raw) {
        const auto value = static_cast<unsigned char>(raw);
        return value < 0x20U && raw != '\t' && raw != '\n' && raw != '\r';
    });
}

bool inspect_import_path(const std::string& path, fs::path& native,
                         std::uintmax_t& size, std::string* error) {
    if (path.empty() || has_nul(path)) {
        return fail(error, "Palette path is empty or contains a NUL byte.");
    }
    native = detail::path_from_utf8(path);
    std::error_code code;
    const fs::file_status status = fs::symlink_status(native, code);
    if (code || !fs::is_regular_file(status) || fs::is_symlink(status)) {
        return fail(error,
                    "Palette input must be a readable regular file, not a link or special file.");
    }
    size = fs::file_size(native, code);
    if (code || size == 0U || size > kMaximumPaletteFileBytes) {
        return fail(error,
                    "Palette input is empty, unreadable, or exceeds the 64 MiB palette limit.");
    }
    return true;
}

bool read_file(const std::string& path, std::string& bytes, std::string* error) {
    fs::path native;
    std::uintmax_t size = 0U;
    if (!inspect_import_path(path, native, size, error)) return false;
    std::ifstream input(native, std::ios::binary);
    if (!input) return fail(error, "Could not open palette input.");
    bytes.resize(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    if (!input || input.peek() != std::char_traits<char>::eof()) {
        return fail(error, "Could not read the complete palette input.");
    }
    return true;
}

bool validate_entry(const PaletteEntry& entry, std::string* error) {
    if (!std::isfinite(entry.red) || !std::isfinite(entry.green)
        || !std::isfinite(entry.blue) || !std::isfinite(entry.alpha)) {
        return fail(error, "Palette colors cannot contain NaN or infinity.");
    }
    if (entry.alpha < 0.0 || entry.alpha > 1.0) {
        return fail(error, "Palette alpha must be in the finite [0, 1] range.");
    }
    if (entry.source_encoding == ColorEncoding::SRGB
        && (entry.red < 0.0 || entry.red > 1.0 || entry.green < 0.0
            || entry.green > 1.0 || entry.blue < 0.0 || entry.blue > 1.0)) {
        return fail(error, "Encoded sRGB palette components must be within [0, 1].");
    }
    return true;
}

bool validate_document(const PaletteDocument& document, std::string* error) {
    if (document.entries.empty()) {
        return fail(error, "A palette must contain at least one color.");
    }
    if (document.entries.size() > kMaximumPaletteEntries) {
        return fail(error, "Palette exceeds the one-million-entry safety limit.");
    }
    if (document.columns.has_value()
        && (*document.columns == 0U || *document.columns > kMaximumPaletteEntries)) {
        return fail(error, "Palette column count is outside the supported range.");
    }
    std::size_t metadata_bytes = document.name.size();
    if (document.name.size() > kMaximumPaletteTextLineBytes
        || has_nul(document.name) || has_forbidden_text_control(document.name)
        || !valid_utf8(document.name)) {
        return fail(error, "Palette name is invalid UTF-8, contains NUL, or exceeds 1 MiB.");
    }
    for (const PaletteEntry& entry : document.entries) {
        if (!validate_entry(entry, error)) return false;
        if (entry.name.size() > kMaximumPaletteTextLineBytes
            || has_nul(entry.name) || has_forbidden_text_control(entry.name)
            || !valid_utf8(entry.name)
            || entry.name.size() > kMaximumPaletteFileBytes - metadata_bytes) {
            return fail(error,
                        "Palette entry names are invalid UTF-8 or exceed metadata limits.");
        }
        metadata_bytes += entry.name.size();
    }
    return true;
}

double clamp_unit(double value) {
    return std::max(0.0, std::min(1.0, value));
}

double srgb_to_linear(double value) {
    if (value <= 0.04045) return value / 12.92;
    return std::pow((value + 0.055) / 1.055, 2.4);
}

double linear_to_srgb(double value) {
    if (value <= 0.0031308) return 12.92 * value;
    return 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

struct Encoded8 {
    std::array<unsigned int, 4U> component{{0U, 0U, 0U, 255U}};
};

Encoded8 encode_srgb8(const PaletteEntry& entry, PaletteIoSummary& summary) {
    Encoded8 result;
    const std::array<double, 4U> original{{
        entry.red, entry.green, entry.blue, entry.alpha}};
    for (std::size_t channel = 0U; channel < original.size(); ++channel) {
        double encoded = original[channel];
        if (channel < 3U && entry.source_encoding == ColorEncoding::Linear) {
            encoded = linear_to_srgb(encoded);
            summary.encoding_converted = true;
        }
        if (encoded < 0.0 || encoded > 1.0) {
            summary.precision_lost = true;
            add_warning(summary,
                        "Out-of-range or HDR components were clipped for an sRGB integer format.");
        }
        encoded = clamp_unit(encoded);
        const unsigned int quantized = static_cast<unsigned int>(
            std::llround(encoded * 255.0));
        result.component[channel] = quantized;
        if (std::abs(encoded - static_cast<double>(quantized) / 255.0) > 1e-12) {
            summary.precision_lost = true;
        }
    }
    if (summary.encoding_converted) {
        add_warning(summary,
                    "Linear RGB colors were converted to display sRGB for this format.");
    }
    if (summary.precision_lost) {
        add_warning(summary, "Color components were quantized to 8-bit values.");
    }
    return result;
}

char hex_digit(unsigned int value) {
    static constexpr char digits[] = "0123456789abcdef";
    return digits[value & 0x0fU];
}

std::string hex_color(const Encoded8& color, bool include_alpha) {
    std::string result(1U, '#');
    const std::size_t components = include_alpha ? 4U : 3U;
    for (std::size_t index = 0U; index < components; ++index) {
        const unsigned int value = color.component[index];
        result.push_back(hex_digit(value >> 4U));
        result.push_back(hex_digit(value));
    }
    return result;
}

bool parse_unsigned(std::string_view text, unsigned long long& value) {
    text = trim_view(text);
    if (text.empty()) return false;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value, 10);
    return parsed.ec == std::errc() && parsed.ptr == end;
}

bool parse_double(std::string_view text, double& value) {
    text = trim_view(text);
    if (text.empty() || text.size() > 128U) return false;
    std::istringstream stream{std::string(text)};
    stream.imbue(std::locale::classic());
    double candidate = 0.0;
    stream >> std::noskipws >> candidate;
    if (!stream || stream.peek() != std::char_traits<char>::eof()
        || !std::isfinite(candidate)) {
        return false;
    }
    value = candidate;
    return true;
}

int hex_value(char raw) {
    const auto value = static_cast<unsigned char>(raw);
    if (value >= static_cast<unsigned char>('0')
        && value <= static_cast<unsigned char>('9')) {
        return static_cast<int>(value - static_cast<unsigned char>('0'));
    }
    if (value >= static_cast<unsigned char>('a')
        && value <= static_cast<unsigned char>('f')) {
        return static_cast<int>(value - static_cast<unsigned char>('a')) + 10;
    }
    if (value >= static_cast<unsigned char>('A')
        && value <= static_cast<unsigned char>('F')) {
        return static_cast<int>(value - static_cast<unsigned char>('A')) + 10;
    }
    return -1;
}

bool parse_hex_color(std::string_view literal, PaletteEntry& entry) {
    literal = trim_view(literal);
    if (literal.empty() || literal.front() != '#') return false;
    literal.remove_prefix(1U);
    if (literal.size() != 6U && literal.size() != 8U) return false;
    std::array<unsigned int, 4U> values{{0U, 0U, 0U, 255U}};
    for (std::size_t index = 0U; index < literal.size() / 2U; ++index) {
        const int high = hex_value(literal[index * 2U]);
        const int low = hex_value(literal[index * 2U + 1U]);
        if (high < 0 || low < 0) return false;
        values[index] = static_cast<unsigned int>(high * 16 + low);
    }
    entry.red = static_cast<double>(values[0U]) / 255.0;
    entry.green = static_cast<double>(values[1U]) / 255.0;
    entry.blue = static_cast<double>(values[2U]) / 255.0;
    entry.alpha = static_cast<double>(values[3U]) / 255.0;
    entry.source_encoding = ColorEncoding::SRGB;
    return true;
}

bool next_line(std::string_view text, std::size_t& position,
               std::string_view& line, std::string* error) {
    if (position >= text.size()) return false;
    const std::size_t newline = text.find('\n', position);
    const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
    if (end - position > kMaximumPaletteTextLineBytes) {
        fail(error, "Palette text contains a line longer than the 1 MiB limit.");
        position = text.size();
        line = {};
        return false;
    }
    line = text.substr(position, end - position);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1U);
    position = newline == std::string_view::npos ? text.size() : newline + 1U;
    return true;
}

bool append_entry(PaletteDocument& document, PaletteEntry entry,
                  PaletteIoSummary& summary, std::string* error) {
    if (document.entries.size() >= kMaximumPaletteEntries) {
        return fail(error, "Palette exceeds the one-million-entry safety limit.");
    }
    if (!validate_entry(entry, error)) return false;
    entry.source_order = summary.scanned == 0U ? 0U : summary.scanned - 1U;
    document.entries.push_back(std::move(entry));
    ++summary.accepted;
    return true;
}

bool parse_quoted(std::string_view text, std::size_t& position,
                  std::string& value) {
    while (position < text.size()
           && std::isspace(static_cast<unsigned char>(text[position])) != 0) {
        ++position;
    }
    if (position >= text.size()
        || (text[position] != '\'' && text[position] != '"')) {
        return false;
    }
    const char quote = text[position++];
    value.clear();
    while (position < text.size()) {
        const char current = text[position++];
        if (current == quote) return true;
        if (current != '\\') {
            value.push_back(current);
            continue;
        }
        if (position >= text.size()) return false;
        const char escaped = text[position++];
        switch (escaped) {
            case '\\': value.push_back('\\'); break;
            case '\'': value.push_back('\''); break;
            case '"': value.push_back('"'); break;
            case '/': value.push_back('/'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: return false;
        }
        if (value.size() > kMaximumPaletteTextLineBytes) return false;
    }
    return false;
}

bool parse_pvt_display_name(std::string_view line, std::string& name) {
    const std::size_t marker = line.find("pvt-name:");
    if (marker == std::string_view::npos) return false;
    std::size_t position = marker + std::strlen("pvt-name:");
    return parse_quoted(line, position, name);
}

std::string escape_code_string(std::string_view text, char quote) {
    std::string result;
    result.reserve(text.size() + 8U);
    for (char raw : text) {
        switch (raw) {
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (raw == quote) result.push_back('\\');
                result.push_back(raw);
                break;
        }
    }
    return result;
}

std::string escape_css_comment_string(std::string_view text) {
    std::string result;
    result.reserve(text.size() + 8U);
    char previous = '\0';
    for (char raw : text) {
        if (raw == '/' && previous == '*') {
            // CSS comments have no quoting rules: inserting a literal
            // backslash between '*' and '/' is what prevents an imported
            // display name from terminating this metadata comment. The
            // palette parser decodes the escaped slash on re-import.
            result += "\\/";
        } else {
            switch (raw) {
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                case '"': result += "\\\""; break;
                default: result.push_back(raw); break;
            }
        }
        previous = raw;
    }
    return result;
}

std::string unique_code_name(std::string_view original,
                             std::set<std::string>& used,
                             bool css_identifier) {
    std::string base;
    base.reserve(original.size());
    for (char raw : original) {
        const auto value = static_cast<unsigned char>(raw);
        const bool permitted = (value >= static_cast<unsigned char>('a')
                                && value <= static_cast<unsigned char>('z'))
                               || (value >= static_cast<unsigned char>('A')
                                   && value <= static_cast<unsigned char>('Z'))
                               || (value >= static_cast<unsigned char>('0')
                                   && value <= static_cast<unsigned char>('9'))
                               || raw == '_' || (css_identifier && raw == '-');
        base.push_back(permitted ? raw : '_');
    }
    if (base.empty()) base = "color";
    if (std::isdigit(static_cast<unsigned char>(base.front())) != 0) {
        base.insert(base.begin(), '_');
    }
    std::string candidate = base;
    for (std::size_t suffix = 2U; !used.insert(candidate).second; ++suffix) {
        candidate = base + "_" + std::to_string(suffix);
    }
    return candidate;
}

std::string document_name_from_path(const std::string& path) {
    const fs::path native = detail::path_from_utf8(path);
    const std::string stem = detail::path_to_utf8(native.stem());
    return stem.empty() ? "Imported Palette" : stem;
}

bool ensure_import_has_entries(PaletteDocument& document,
                               PaletteIoSummary& summary,
                               std::string* error) {
    if (document.entries.empty()) {
        return fail(error, "No supported palette colors were found.");
    }
    summary.accepted = document.entries.size();
    return true;
}

bool parse_text_hex(std::string_view text, PaletteDocument& document,
                    PaletteIoSummary& summary, std::string* error) {
    std::size_t position = 0U;
    std::string_view line;
    while (next_line(text, position, line, error)) {
        line = trim_view(line);
        if (line.empty() || line.front() != '#') continue;
        ++summary.scanned;
        const std::size_t whitespace = line.find_first_of(" \t");
        const std::string_view literal = line.substr(0U, whitespace);
        PaletteEntry entry;
        if (!parse_hex_color(literal, entry)) {
            ++summary.skipped;
            continue;
        }
        if (whitespace != std::string_view::npos) {
            entry.name = trim_copy(line.substr(whitespace));
        }
        if (!append_entry(document, std::move(entry), summary, error)) return false;
    }
    if (error != nullptr && !error->empty()) return false;
    return ensure_import_has_entries(document, summary, error);
}

bool parse_gpl(std::string_view text, PaletteDocument& document,
               PaletteIoSummary& summary, std::string* error) {
    std::size_t position = 0U;
    std::string_view line;
    if (!next_line(text, position, line, error)
        || trim_view(line) != "GIMP Palette") {
        return fail(error, "GIMP GPL input is missing the 'GIMP Palette' header.");
    }
    while (next_line(text, position, line, error)) {
        line = trim_view(line);
        if (line.empty() || line.front() == '#') continue;
        if (line.rfind("Name:", 0U) == 0U) {
            document.name = trim_copy(line.substr(5U));
            continue;
        }
        if (line.rfind("Columns:", 0U) == 0U) {
            unsigned long long columns = 0U;
            if (!parse_unsigned(line.substr(8U), columns)
                || columns > kMaximumPaletteEntries) {
                return fail(error, "GIMP GPL Columns value is invalid.");
            }
            if (columns == 0U) document.columns.reset();
            else document.columns = static_cast<std::size_t>(columns);
            continue;
        }
        ++summary.scanned;
        std::array<unsigned long long, 3U> components{};
        std::size_t cursor = 0U;
        bool valid = true;
        for (std::size_t channel = 0U; channel < components.size(); ++channel) {
            while (cursor < line.size()
                   && std::isspace(static_cast<unsigned char>(line[cursor])) != 0) {
                ++cursor;
            }
            const std::size_t begin = cursor;
            while (cursor < line.size()
                   && std::isdigit(static_cast<unsigned char>(line[cursor])) != 0) {
                ++cursor;
            }
            valid = valid && begin != cursor
                    && parse_unsigned(line.substr(begin, cursor - begin), components[channel])
                    && components[channel] <= 255U;
        }
        if (!valid) {
            ++summary.skipped;
            continue;
        }
        PaletteEntry entry;
        entry.red = static_cast<double>(components[0U]) / 255.0;
        entry.green = static_cast<double>(components[1U]) / 255.0;
        entry.blue = static_cast<double>(components[2U]) / 255.0;
        entry.name = trim_copy(line.substr(cursor));
        if (!append_entry(document, std::move(entry), summary, error)) return false;
    }
    if (error != nullptr && !error->empty()) return false;
    return ensure_import_has_entries(document, summary, error);
}

bool parse_dictionary(std::string_view text, std::string_view separator,
                      PaletteDocument& document, PaletteIoSummary& summary,
                      std::string* error) {
    std::size_t position = 0U;
    std::string_view line;
    while (next_line(text, position, line, error)) {
        const std::size_t delimiter = line.find(separator);
        if (delimiter == std::string_view::npos) continue;
        ++summary.scanned;
        std::size_t key_position = line.find_first_of("\"'");
        std::string key;
        std::string literal;
        if (key_position == std::string_view::npos
            || !parse_quoted(line, key_position, key)) {
            ++summary.skipped;
            continue;
        }
        std::size_t value_position = delimiter + separator.size();
        if (!parse_quoted(line, value_position, literal)) {
            ++summary.skipped;
            continue;
        }
        PaletteEntry entry;
        if (!parse_hex_color(literal, entry)) {
            ++summary.skipped;
            continue;
        }
        entry.name = key;
        std::string display_name;
        if (parse_pvt_display_name(line, display_name)) entry.name = display_name;
        if (!append_entry(document, std::move(entry), summary, error)) return false;
    }
    if (error != nullptr && !error->empty()) return false;
    return ensure_import_has_entries(document, summary, error);
}

bool parse_css_color(std::string_view value, PaletteEntry& entry) {
    value = trim_view(value);
    if (!value.empty() && value.front() == '#') {
        const std::size_t end = value.find_first_of("; \t}");
        return parse_hex_color(value.substr(0U, end), entry);
    }
    const std::string lower = lower_ascii(std::string(value));
    const bool rgba = lower.rfind("rgba(", 0U) == 0U;
    if (!rgba && lower.rfind("rgb(", 0U) != 0U) return false;
    const std::size_t open = value.find('(');
    const std::size_t close = value.find(')', open + 1U);
    if (close == std::string_view::npos) return false;
    std::array<double, 4U> components{{0.0, 0.0, 0.0, 1.0}};
    std::size_t cursor = open + 1U;
    const std::size_t count = rgba ? 4U : 3U;
    for (std::size_t index = 0U; index < count; ++index) {
        const std::size_t comma = value.find(',', cursor);
        const std::size_t end = index + 1U == count ? close : comma;
        if (end == std::string_view::npos || !parse_double(value.substr(cursor, end - cursor),
                                                           components[index])) {
            return false;
        }
        if (index < 3U) components[index] /= 255.0;
        cursor = end + 1U;
    }
    if (components[0U] < 0.0 || components[0U] > 1.0
        || components[1U] < 0.0 || components[1U] > 1.0
        || components[2U] < 0.0 || components[2U] > 1.0
        || components[3U] < 0.0 || components[3U] > 1.0) {
        return false;
    }
    entry.red = components[0U];
    entry.green = components[1U];
    entry.blue = components[2U];
    entry.alpha = components[3U];
    entry.source_encoding = ColorEncoding::SRGB;
    return true;
}

bool parse_css(std::string_view text, PaletteDocument& document,
               PaletteIoSummary& summary, std::string* error) {
    std::size_t position = 0U;
    std::string_view line;
    while (next_line(text, position, line, error)) {
        const std::string lower = lower_ascii(std::string(line));
        const std::size_t color = lower.find("color:");
        const std::size_t brace = line.find('{');
        if (color == std::string::npos || brace == std::string_view::npos
            || brace > color) {
            continue;
        }
        ++summary.scanned;
        PaletteEntry entry;
        if (!parse_css_color(line.substr(color + 6U), entry)) {
            ++summary.skipped;
            continue;
        }
        std::string_view selector = trim_view(line.substr(0U, brace));
        if (!selector.empty() && (selector.front() == '.' || selector.front() == '#')) {
            selector.remove_prefix(1U);
        }
        entry.name = trim_copy(selector);
        std::string display_name;
        if (parse_pvt_display_name(line, display_name)) entry.name = display_name;
        if (!append_entry(document, std::move(entry), summary, error)) return false;
    }
    if (error != nullptr && !error->empty()) return false;
    return ensure_import_has_entries(document, summary, error);
}

bool parse_java(std::string_view text, PaletteDocument& document,
                PaletteIoSummary& summary, std::string* error) {
    std::size_t position = 0U;
    std::string_view line;
    while (next_line(text, position, line, error)) {
        const std::size_t put = line.find(".put(");
        const std::size_t color = line.find("new Color(", put);
        if (put == std::string_view::npos || color == std::string_view::npos) continue;
        ++summary.scanned;
        std::size_t key_position = put + 5U;
        std::string key;
        if (!parse_quoted(line, key_position, key)) {
            ++summary.skipped;
            continue;
        }
        const std::size_t close = line.find(')', color + 10U);
        if (close == std::string_view::npos) {
            ++summary.skipped;
            continue;
        }
        std::array<unsigned long long, 4U> components{{0U, 0U, 0U, 255U}};
        std::size_t cursor = color + 10U;
        std::size_t count = 0U;
        bool valid = true;
        while (cursor < close && count < components.size()) {
            const std::size_t comma = line.find(',', cursor);
            const std::size_t end = comma == std::string_view::npos || comma > close
                                        ? close : comma;
            valid = valid && parse_unsigned(line.substr(cursor, end - cursor),
                                             components[count])
                    && components[count] <= 255U;
            ++count;
            cursor = end + 1U;
        }
        if (!valid || (count != 3U && count != 4U)) {
            ++summary.skipped;
            continue;
        }
        PaletteEntry entry;
        entry.name = key;
        entry.red = static_cast<double>(components[0U]) / 255.0;
        entry.green = static_cast<double>(components[1U]) / 255.0;
        entry.blue = static_cast<double>(components[2U]) / 255.0;
        entry.alpha = static_cast<double>(components[3U]) / 255.0;
        std::string display_name;
        if (parse_pvt_display_name(line, display_name)) entry.name = display_name;
        if (!append_entry(document, std::move(entry), summary, error)) return false;
    }
    if (error != nullptr && !error->empty()) return false;
    return ensure_import_has_entries(document, summary, error);
}

bool append_utf8(std::uint32_t codepoint, std::string& destination) {
    if (codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU)
        || (codepoint < 0x20U && codepoint != 0x09U
            && codepoint != 0x0aU && codepoint != 0x0dU)) {
        return false;
    }
    if (codepoint <= 0x7fU) {
        destination.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        destination.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        destination.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        destination.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        destination.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        destination.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        destination.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
        destination.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
        destination.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        destination.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
    return true;
}

bool decode_xml_entities(std::string_view encoded, std::string& decoded) {
    decoded.clear();
    decoded.reserve(encoded.size());
    for (std::size_t index = 0U; index < encoded.size();) {
        if (encoded[index] != '&') {
            decoded.push_back(encoded[index++]);
            continue;
        }
        const std::size_t semicolon = encoded.find(';', index + 1U);
        if (semicolon == std::string_view::npos || semicolon - index > 16U) return false;
        const std::string_view entity =
            encoded.substr(index + 1U, semicolon - index - 1U);
        if (entity == "amp") decoded.push_back('&');
        else if (entity == "lt") decoded.push_back('<');
        else if (entity == "gt") decoded.push_back('>');
        else if (entity == "quot") decoded.push_back('"');
        else if (entity == "apos") decoded.push_back('\'');
        else if (!entity.empty() && entity.front() == '#') {
            const bool hexadecimal = entity.size() > 1U
                                     && (entity[1U] == 'x' || entity[1U] == 'X');
            const std::string_view digits =
                entity.substr(hexadecimal ? 2U : 1U);
            if (digits.empty()) return false;
            std::uint32_t codepoint = 0U;
            for (char raw : digits) {
                int digit = hexadecimal ? hex_value(raw)
                                        : (std::isdigit(static_cast<unsigned char>(raw)) != 0
                                               ? raw - '0' : -1);
                if (digit < 0 || codepoint > (0x10ffffU - static_cast<unsigned>(digit))
                                                 / (hexadecimal ? 16U : 10U)) {
                    return false;
                }
                codepoint = codepoint * (hexadecimal ? 16U : 10U)
                            + static_cast<unsigned>(digit);
            }
            if (!append_utf8(codepoint, decoded)) return false;
        } else {
            return false;
        }
        index = semicolon + 1U;
    }
    return true;
}

std::string escape_xml(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 16U);
    for (char raw : value) {
        switch (raw) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            case '\n': result += "&#10;"; break;
            case '\r': result += "&#13;"; break;
            case '\t': result += "&#9;"; break;
            default: result.push_back(raw); break;
        }
    }
    return result;
}

struct XmlTag {
    std::string name;
    std::map<std::string, std::string> attributes;
    bool closing = false;
    bool self_closing = false;
};

bool xml_name_character(char raw, bool first) {
    const auto value = static_cast<unsigned char>(raw);
    return (value >= static_cast<unsigned char>('a')
            && value <= static_cast<unsigned char>('z'))
           || (value >= static_cast<unsigned char>('A')
               && value <= static_cast<unsigned char>('Z'))
           || raw == '_' || raw == ':'
           || (!first && ((value >= static_cast<unsigned char>('0')
                           && value <= static_cast<unsigned char>('9'))
                          || raw == '-' || raw == '.'));
}

bool parse_xml_tag_body(std::string_view body, XmlTag& tag, std::string* error) {
    body = trim_view(body);
    tag = {};
    if (body.empty()) return fail(error, "Krita colorset XML contains an empty tag.");
    if (body.front() == '/') {
        tag.closing = true;
        body.remove_prefix(1U);
        body = trim_view(body);
    }
    if (!tag.closing && !body.empty() && body.back() == '/') {
        tag.self_closing = true;
        body.remove_suffix(1U);
        body = trim_view(body);
    }
    std::size_t position = 0U;
    if (position >= body.size() || !xml_name_character(body[position], true)) {
        return fail(error, "Krita colorset XML contains an invalid tag name.");
    }
    while (position < body.size() && xml_name_character(body[position], position == 0U)) {
        tag.name.push_back(body[position++]);
    }
    if (tag.closing) {
        if (!trim_view(body.substr(position)).empty()) {
            return fail(error, "Krita colorset XML closing tag contains extra data.");
        }
        return true;
    }
    while (position < body.size()) {
        while (position < body.size()
               && std::isspace(static_cast<unsigned char>(body[position])) != 0) {
            ++position;
        }
        if (position == body.size()) break;
        std::string attribute;
        if (!xml_name_character(body[position], true)) {
            return fail(error, "Krita colorset XML contains an invalid attribute name.");
        }
        while (position < body.size()
               && xml_name_character(body[position], attribute.empty())) {
            attribute.push_back(body[position++]);
        }
        while (position < body.size()
               && std::isspace(static_cast<unsigned char>(body[position])) != 0) {
            ++position;
        }
        if (position >= body.size() || body[position++] != '=') {
            return fail(error, "Krita colorset XML attribute is missing '='.");
        }
        while (position < body.size()
               && std::isspace(static_cast<unsigned char>(body[position])) != 0) {
            ++position;
        }
        if (position >= body.size()
            || (body[position] != '\'' && body[position] != '"')) {
            return fail(error, "Krita colorset XML attributes must use quoted values.");
        }
        const char quote = body[position++];
        const std::size_t end = body.find(quote, position);
        if (end == std::string_view::npos) {
            return fail(error, "Krita colorset XML contains an unterminated attribute.");
        }
        std::string decoded;
        if (!decode_xml_entities(body.substr(position, end - position), decoded)) {
            return fail(error, "Krita colorset XML contains an unsupported entity.");
        }
        if (!tag.attributes.emplace(std::move(attribute), std::move(decoded)).second
            || tag.attributes.size() > kMaximumXmlAttributes) {
            return fail(error, "Krita colorset XML has duplicate or excessive attributes.");
        }
        position = end + 1U;
    }
    return true;
}

bool next_xml_tag(std::string_view xml, std::size_t& position,
                  XmlTag& tag, bool& found, std::string* error) {
    found = false;
    while (position < xml.size()) {
        const std::size_t open = xml.find('<', position);
        if (open == std::string_view::npos) {
            position = xml.size();
            return true;
        }
        if (xml.substr(open, 4U) == "<!--") {
            const std::size_t end = xml.find("-->", open + 4U);
            if (end == std::string_view::npos) {
                return fail(error, "Krita colorset XML contains an unterminated comment.");
            }
            position = end + 3U;
            continue;
        }
        if (xml.substr(open, 2U) == "<?") {
            const std::size_t end = xml.find("?>", open + 2U);
            if (end == std::string_view::npos) {
                return fail(error,
                            "Krita colorset XML contains an unterminated processing instruction.");
            }
            position = end + 2U;
            continue;
        }
        if (xml.substr(open, 2U) == "<!") {
            return fail(error,
                        "Krita colorset XML declarations and custom entities are not supported.");
        }
        const std::size_t close = xml.find('>', open + 1U);
        if (close == std::string_view::npos || close - open > kMaximumXmlTagBytes) {
            return fail(error, "Krita colorset XML contains an unterminated or oversized tag.");
        }
        if (!parse_xml_tag_body(xml.substr(open + 1U, close - open - 1U), tag,
                                error)) {
            return false;
        }
        position = close + 1U;
        found = true;
        return true;
    }
    return true;
}

const std::string* xml_attribute(const XmlTag& tag, const char* name) {
    const auto found = tag.attributes.find(name);
    return found == tag.attributes.end() ? nullptr : &found->second;
}

bool parse_kpl_rgb(const XmlTag& tag, std::string_view bit_depth,
                   PaletteEntry& entry, bool& unsupported,
                   std::string* error) {
    unsupported = false;
    const std::string tag_name = lower_ascii(tag.name);
    if (tag_name != "rgb" && tag_name != "srgb") {
        unsupported = true;
        return fail(error, "Krita palette uses unsupported color space '"
                               + tag.name + "'; only RGB is supported.");
    }
    const std::string* red = xml_attribute(tag, "r");
    const std::string* green = xml_attribute(tag, "g");
    const std::string* blue = xml_attribute(tag, "b");
    if (red == nullptr || green == nullptr || blue == nullptr
        || !parse_double(*red, entry.red) || !parse_double(*green, entry.green)
        || !parse_double(*blue, entry.blue)) {
        return fail(error, "Krita RGB entry has missing or non-finite components.");
    }
    const std::string* alpha = xml_attribute(tag, "a");
    if (alpha != nullptr && !parse_double(*alpha, entry.alpha)) {
        return fail(error, "Krita RGB entry has invalid alpha.");
    }
    const std::string normalized_depth = lower_ascii(std::string(bit_depth));
    const std::string* space_attribute = xml_attribute(tag, "space");
    const std::string* profile_attribute = xml_attribute(tag, "profile");
    const std::string* color_space_attribute = xml_attribute(tag, "colorSpace");
    if (color_space_attribute == nullptr) {
        color_space_attribute = xml_attribute(tag, "colorspace");
    }
    const std::string* selected_space = space_attribute != nullptr
                                            ? space_attribute
                                            : (profile_attribute != nullptr
                                                   ? profile_attribute
                                                   : color_space_attribute);
    const auto conflicts = [selected_space](const std::string* candidate) {
        return selected_space != nullptr && candidate != nullptr
               && lower_ascii(*selected_space) != lower_ascii(*candidate);
    };
    if (conflicts(profile_attribute) || conflicts(color_space_attribute)) {
        return fail(error, "Krita RGB entry has conflicting color-profile declarations.");
    }
    const std::string space = selected_space == nullptr
                                  ? std::string() : lower_ascii(*selected_space);
    if (tag_name == "srgb") {
        entry.source_encoding = ColorEncoding::SRGB;
    } else if (space.empty()) {
        entry.source_encoding = normalized_depth == "f32"
                                    ? ColorEncoding::Linear : ColorEncoding::SRGB;
    } else if (space == "srgb" || space == "srgb-elle-v2-srgbtrc.icc"
               || space == "srgbtrc" || space == "rgb") {
        entry.source_encoding = ColorEncoding::SRGB;
    } else if (space == "linear" || space == "linear-srgb"
               || space == "srgb-elle-v2-g10.icc") {
        entry.source_encoding = ColorEncoding::Linear;
    } else {
        unsupported = true;
        return fail(error, "Krita RGB entry references unsupported profile '"
                               + *selected_space + "'.");
    }
    if (normalized_depth != "f32" && normalized_depth != "u8"
        && normalized_depth != "u16" && !normalized_depth.empty()) {
        unsupported = true;
        return fail(error, "Krita RGB entry uses unsupported bit depth '"
                               + std::string(bit_depth) + "'.");
    }
    if (entry.source_encoding == ColorEncoding::SRGB
        && (entry.red < 0.0 || entry.red > 1.0 || entry.green < 0.0
            || entry.green > 1.0 || entry.blue < 0.0 || entry.blue > 1.0)) {
        return fail(error,
                    "Normalized Krita sRGB values must be within [0, 1].");
    }
    return validate_entry(entry, error);
}

struct PositionedEntry {
    PaletteEntry entry;
    std::optional<std::pair<std::size_t, std::size_t>> position;
    std::size_t xml_order = 0U;
};

bool parse_kpl_xml(std::string_view xml, PaletteDocument& document,
                   PaletteIoSummary& summary, std::string* error) {
    if (xml.empty() || xml.size() > kMaximumKplXmlBytes || has_nul(xml)
        || has_forbidden_text_control(xml) || !valid_utf8(xml)) {
        return fail(error,
                    "Krita colorset.xml is invalid UTF-8, contains NUL, or exceeds 16 MiB.");
    }
    std::size_t cursor = 0U;
    XmlTag tag;
    bool found = false;
    if (!next_xml_tag(xml, cursor, tag, found, error) || !found
        || tag.closing || lower_ascii(tag.name) != "colorset") {
        return fail(error, "Krita palette XML must begin with a ColorSet element.");
    }
    if (const std::string* name = xml_attribute(tag, "name")) document.name = *name;
    if (const std::string* columns = xml_attribute(tag, "columns")) {
        unsigned long long parsed = 0U;
        if (!parse_unsigned(*columns, parsed) || parsed > kMaximumPaletteEntries) {
            return fail(error, "Krita palette column count is invalid.");
        }
        if (parsed == 0U) document.columns.reset();
        else document.columns = static_cast<std::size_t>(parsed);
    }

    std::vector<PositionedEntry> entries;
    bool closed_root = false;
    while (next_xml_tag(xml, cursor, tag, found, error) && found) {
        const std::string lower_name = lower_ascii(tag.name);
        if (tag.closing && lower_name == "colorset") {
            closed_root = true;
            break;
        }
        if (tag.closing || lower_name != "colorsetentry") continue;
        ++summary.scanned;
        PositionedEntry positioned;
        positioned.xml_order = summary.scanned - 1U;
        if (const std::string* name = xml_attribute(tag, "name")) {
            positioned.entry.name = *name;
        }
        const std::string bit_depth = xml_attribute(tag, "bitdepth") == nullptr
                                          ? std::string()
                                          : *xml_attribute(tag, "bitdepth");
        bool has_color = false;
        bool saw_color = false;
        bool unsupported_color = false;
        bool closed_entry = tag.self_closing;
        while (!closed_entry
               && next_xml_tag(xml, cursor, tag, found, error) && found) {
            const std::string child = lower_ascii(tag.name);
            if (tag.closing && child == "colorsetentry") {
                closed_entry = true;
                break;
            }
            if (tag.closing) continue;
            if (child == "rgb" || child == "srgb") {
                if (saw_color) {
                    return fail(error, "Krita palette entry contains multiple colors.");
                }
                saw_color = true;
                bool rgb_unsupported = false;
                std::string rgb_error;
                if (!parse_kpl_rgb(tag, bit_depth, positioned.entry,
                                   rgb_unsupported, &rgb_error)) {
                    if (!rgb_unsupported) return fail(error, rgb_error);
                    ++summary.unsupported;
                    unsupported_color = true;
                    continue;
                }
                has_color = true;
            } else if (child == "position") {
                const std::string* row = xml_attribute(tag, "row");
                const std::string* column = xml_attribute(tag, "column");
                unsigned long long parsed_row = 0U;
                unsigned long long parsed_column = 0U;
                if (row == nullptr || column == nullptr
                    || !parse_unsigned(*row, parsed_row)
                    || !parse_unsigned(*column, parsed_column)
                    || parsed_row > kMaximumPaletteEntries
                    || parsed_column > kMaximumPaletteEntries) {
                    return fail(error, "Krita palette Position is invalid.");
                }
                positioned.position = std::make_pair(
                    static_cast<std::size_t>(parsed_row),
                    static_cast<std::size_t>(parsed_column));
            } else if (child == "cmyk" || child == "lab" || child == "xyz"
                       || child == "gray" || child == "ycbcr") {
                if (saw_color) {
                    return fail(error, "Krita palette entry contains multiple colors.");
                }
                saw_color = true;
                unsupported_color = true;
                ++summary.unsupported;
            }
        }
        if (!closed_entry) return fail(error, "Krita ColorSetEntry is not closed.");
        if (!has_color) {
            if (!unsupported_color) ++summary.skipped;
            continue;
        }
        entries.push_back(std::move(positioned));
        if (entries.size() > kMaximumPaletteEntries) {
            return fail(error, "Krita palette exceeds the one-million-entry limit.");
        }
    }
    if (error != nullptr && !error->empty()) return false;
    if (!closed_root) return fail(error, "Krita ColorSet element is not closed.");

    std::stable_sort(entries.begin(), entries.end(), [](const PositionedEntry& left,
                                                        const PositionedEntry& right) {
        if (!left.position.has_value() || !right.position.has_value()) {
            return left.position.has_value() && !right.position.has_value();
        }
        return std::tie(left.position->first, left.position->second, left.xml_order)
               < std::tie(right.position->first, right.position->second, right.xml_order);
    });
    for (PositionedEntry& positioned : entries) {
        positioned.entry.source_order = positioned.xml_order;
        document.entries.push_back(std::move(positioned.entry));
    }
    if (summary.unsupported > 0U) {
        add_warning(summary,
                    "Unsupported Krita color-space or profile entries were ignored.");
    }
    return ensure_import_has_entries(document, summary, error);
}

bool safe_archive_path(std::string_view path) {
    if (path.empty() || path.size() > kMaximumPaletteTextLineBytes
        || path.front() == '/' || path.front() == '\\'
        || path.find('\\') != std::string_view::npos || has_nul(path)) {
        return false;
    }
    if (path.back() == '/') {
        path.remove_suffix(1U);
        if (path.empty()) return false;
    }
    std::size_t position = 0U;
    while (position < path.size()) {
        const std::size_t slash = path.find('/', position);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        const std::string_view component = path.substr(position, end - position);
        if (component.empty() || component == "." || component == ".."
            || component.find(':') != std::string_view::npos) {
            return false;
        }
        position = slash == std::string_view::npos ? path.size() : slash + 1U;
    }
    return true;
}

struct ZipReaderGuard {
    void* handle = mz_zip_reader_create();
    ~ZipReaderGuard() {
        if (handle != nullptr) {
            (void)mz_zip_reader_close(handle);
            mz_zip_reader_delete(&handle);
        }
    }
};

struct ZipWriterGuard {
    void* handle = mz_zip_writer_create();
    bool open = false;
    ~ZipWriterGuard() {
        if (handle != nullptr) {
            if (open) (void)mz_zip_writer_close(handle);
            mz_zip_writer_delete(&handle);
        }
    }
};

struct MemoryStreamGuard {
    void* handle = mz_stream_mem_create();
    bool open = false;
    ~MemoryStreamGuard() {
        if (handle != nullptr) {
            if (open) (void)mz_stream_mem_close(handle);
            mz_stream_mem_delete(&handle);
        }
    }
};

struct TemporaryCleanup {
    fs::path path;
    bool active = true;
    ~TemporaryCleanup() {
        if (active && !path.empty()) {
            std::error_code ignored;
            fs::remove(path, ignored);
        }
    }
    void dismiss() { active = false; }
};

bool read_current_zip_entry(void* reader, std::size_t size,
                            std::string& destination, std::string* error) {
    destination.assign(size, '\0');
    if (mz_zip_reader_entry_open(reader) != MZ_OK) {
        return fail(error, "Could not open Krita palette ZIP entry.");
    }
    std::size_t offset = 0U;
    while (offset < destination.size()) {
        const std::size_t bounded = (std::min)(
            destination.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<int32_t>::max)()));
        const int32_t count = mz_zip_reader_entry_read(
            reader, destination.data() + offset, static_cast<int32_t>(bounded));
        if (count <= 0) {
            (void)mz_zip_reader_entry_close(reader);
            return fail(error, "Krita palette ZIP entry ended before its declared size.");
        }
        offset += static_cast<std::size_t>(count);
    }
    std::array<char, 1U> extra{};
    if (mz_zip_reader_entry_read(reader, extra.data(), 1) != 0
        || mz_zip_reader_entry_close(reader) != MZ_OK) {
        return fail(error, "Krita palette ZIP entry failed CRC or size validation.");
    }
    return true;
}

bool read_kpl_xml(const std::string& path, std::string& xml, std::string* error) {
    std::string archive;
    if (!read_file(path, archive, error)) return false;
    ZipReaderGuard reader;
    if (reader.handle == nullptr) return fail(error, "Could not allocate ZIP reader.");
    if (mz_zip_reader_open_buffer(
            reader.handle,
            reinterpret_cast<const std::uint8_t*>(archive.data()),
            static_cast<std::int32_t>(archive.size()), 0) != MZ_OK) {
        return fail(error, "Could not open Krita KPL ZIP.");
    }
    void* zip_handle = nullptr;
    uint32_t central_disk = 0U;
    if (mz_zip_reader_get_zip_handle(reader.handle, &zip_handle) != MZ_OK
        || mz_zip_get_disk_number_with_cd(zip_handle, &central_disk) != MZ_OK
        || central_disk != 0U) {
        return fail(error, "Multi-disk Krita palette ZIPs are not supported.");
    }
    std::set<std::string> folded_paths;
    std::size_t entries = 0U;
    std::size_t total_bytes = 0U;
    std::string mimetype;
    bool found_xml = false;
    int32_t result = mz_zip_reader_goto_first_entry(reader.handle);
    if (result != MZ_OK) return fail(error, "Krita palette ZIP is empty.");
    while (result == MZ_OK) {
        mz_zip_file* info = nullptr;
        if (mz_zip_reader_entry_get_info(reader.handle, &info) != MZ_OK
            || info == nullptr || info->filename == nullptr) {
            return fail(error, "Krita palette ZIP has malformed entry metadata.");
        }
        if (++entries > kMaximumKplArchiveEntries) {
            return fail(error, "Krita palette ZIP exceeds the 64-entry limit.");
        }
        const std::string archive_path(info->filename, info->filename_size);
        if (std::strlen(info->filename) != info->filename_size
            || !safe_archive_path(archive_path)) {
            return fail(error, "Krita palette ZIP contains an unsafe entry path.");
        }
        if (!folded_paths.insert(lower_ascii(archive_path)).second) {
            return fail(error, "Krita palette ZIP contains duplicate or case-colliding paths.");
        }
        if ((info->flag & MZ_ZIP_FLAG_ENCRYPTED) != 0U || info->aes_version != 0U) {
            return fail(error, "Encrypted Krita palette ZIP entries are not supported.");
        }
        if (info->disk_number != 0U
            || mz_zip_attrib_is_symlink(info->external_fa, info->version_madeby) == MZ_OK) {
            return fail(error, "Krita palette ZIP contains a link or multi-disk entry.");
        }
        const bool directory = mz_zip_reader_entry_is_dir(reader.handle) == MZ_OK;
        std::uint32_t unix_attributes = 0U;
        if (!directory
            && mz_zip_attrib_convert(MZ_HOST_SYSTEM(info->version_madeby),
                                     info->external_fa, MZ_HOST_SYSTEM_UNIX,
                                     &unix_attributes) == MZ_OK) {
            const std::uint32_t type = unix_attributes & 0170000U;
            if (type != 0U && type != 0100000U) {
                return fail(error, "Krita palette ZIP contains a special-file entry.");
            }
        }
        if (!directory) {
            if (info->compression_method != MZ_COMPRESS_METHOD_STORE
                && info->compression_method != MZ_COMPRESS_METHOD_DEFLATE) {
                return fail(error, "Krita palette ZIP uses unsupported compression.");
            }
            if (info->uncompressed_size < 0
                || static_cast<std::uint64_t>(info->uncompressed_size)
                       > kMaximumKplExpandedBytes) {
                return fail(error, "Krita palette ZIP entry is too large.");
            }
            const std::size_t size = static_cast<std::size_t>(info->uncompressed_size);
            if (size > kMaximumKplExpandedBytes - total_bytes) {
                return fail(error, "Krita palette ZIP exceeds the 64 MiB expanded limit.");
            }
            if (info->compressed_size <= 0 && size != 0U) {
                return fail(error, "Krita palette ZIP entry has invalid compressed size.");
            }
            const std::uint64_t minimum_compressed =
                (static_cast<std::uint64_t>(size) + kMaximumKplCompressionRatio - 1U)
                / kMaximumKplCompressionRatio;
            if (info->compressed_size > 0
                && static_cast<std::uint64_t>(info->compressed_size)
                       < minimum_compressed) {
                return fail(error,
                            "Krita palette ZIP entry exceeds the compression-ratio limit.");
            }
            const std::string folded = lower_ascii(archive_path);
            if (folded == "colorset.xml") {
                if (found_xml || size == 0U || size > kMaximumKplXmlBytes) {
                    return fail(error, "Krita palette ZIP has duplicate or oversized colorset.xml.");
                }
                if (!read_current_zip_entry(reader.handle, size, xml, error)) return false;
                found_xml = true;
            } else if (folded == "mimetype") {
                if (!read_current_zip_entry(reader.handle, size, mimetype, error)) return false;
            }
            total_bytes += size;
        }
        result = mz_zip_reader_goto_next_entry(reader.handle);
    }
    if (result != MZ_END_OF_LIST) {
        return fail(error, "Could not enumerate the complete Krita palette ZIP.");
    }
    if (!found_xml) return fail(error, "Krita palette ZIP is missing colorset.xml.");
    if (!mimetype.empty()
        && trim_view(mimetype) != "application/x-krita-palette") {
        return fail(error, "Krita palette ZIP has an unsupported mimetype.");
    }
    return true;
}

std::string unique_suffix() {
    static std::atomic<std::uint64_t> counter{0U};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
    const std::uint64_t process = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    const std::uint64_t process = static_cast<std::uint64_t>(getpid());
#endif
    return std::to_string(process) + "-" + std::to_string(now) + "-"
           + std::to_string(counter.fetch_add(1U, std::memory_order_relaxed));
}

bool prepare_output_path(const std::string& path, bool overwrite,
                         fs::path& destination, fs::path& temporary,
                         std::string* error) {
    if (path.empty() || has_nul(path)) {
        return fail(error, "Palette output path is empty or contains a NUL byte.");
    }
    destination = detail::path_from_utf8(path);
    fs::path parent = destination.has_parent_path() ? destination.parent_path()
                                                    : fs::path(".");
    std::error_code code;
    const fs::file_status parent_status = fs::status(parent, code);
    if (code || !fs::is_directory(parent_status)) {
        return fail(error, "Palette output directory does not exist or is not a directory.");
    }
    const fs::file_status status = fs::symlink_status(destination, code);
    const bool exists = !code && status.type() != fs::file_type::not_found;
    if (code && code != std::errc::no_such_file_or_directory) {
        return fail(error, "Could not inspect palette output destination.");
    }
    if (exists && fs::is_directory(status)) {
        return fail(error, "Palette output destination is a directory.");
    }
    if (exists && !overwrite) {
        return fail(error, "Palette output already exists and overwrite is disabled.");
    }
    temporary = parent / (".pvt-palette-" + unique_suffix() + ".tmp");
    return true;
}

bool install_output(const fs::path& temporary, const fs::path& destination,
                    bool overwrite, std::string* error) {
#if defined(_WIN32)
    DWORD failure = ERROR_SUCCESS;
    if (!detail::install_windows_temporary(temporary, destination, overwrite, &failure)) {
        return fail(error, "Could not atomically install palette output (Windows error "
                               + std::to_string(failure) + ").");
    }
#else
    if (overwrite) {
        if (::rename(temporary.c_str(), destination.c_str()) != 0) {
            return fail(error, "Could not atomically replace palette output: "
                                   + std::generic_category().message(errno));
        }
    } else {
        if (::link(temporary.c_str(), destination.c_str()) != 0) {
            return fail(error, "Could not install palette output without replacing it: "
                                   + std::generic_category().message(errno));
        }
        if (::unlink(temporary.c_str()) != 0) {
            return fail(error,
                        "Palette output was installed, but its temporary name could not be removed.");
        }
    }
    fs::path parent = destination.has_parent_path() ? destination.parent_path()
                                                    : fs::path(".");
#  if defined(O_DIRECTORY)
    constexpr int directory_flag = O_DIRECTORY;
#  else
    constexpr int directory_flag = 0;
#  endif
    const int directory = ::open(parent.c_str(), O_RDONLY | directory_flag);
    if (directory >= 0) {
        (void)::fsync(directory);
        (void)::close(directory);
    }
#endif
    return true;
}

bool write_atomic_bytes(const std::string& path, std::string_view bytes,
                        bool overwrite, std::string* error) {
    fs::path destination;
    fs::path temporary;
    if (!prepare_output_path(path, overwrite, destination, temporary, error)) return false;
    TemporaryCleanup cleanup{temporary};
#if defined(_WIN32)
    HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return fail(error, "Could not exclusively create temporary palette output.");
    }
    std::size_t offset = 0U;
    bool written = true;
    while (offset < bytes.size()) {
        const DWORD request = static_cast<DWORD>((std::min)(
            bytes.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD count = 0U;
        if (WriteFile(handle, bytes.data() + offset, request, &count, nullptr) == 0
            || count == 0U) {
            written = false;
            break;
        }
        offset += static_cast<std::size_t>(count);
    }
    written = written && FlushFileBuffers(handle) != 0;
    const bool closed = CloseHandle(handle) != 0;
    if (!written || !closed) return fail(error, "Could not write complete palette output.");
#else
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#  if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#  endif
#  if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#  endif
    const int descriptor = ::open(temporary.c_str(), flags, 0666);
    if (descriptor < 0) {
        return fail(error, "Could not exclusively create temporary palette output: "
                               + std::generic_category().message(errno));
    }
    std::size_t offset = 0U;
    bool written = true;
    while (offset < bytes.size()) {
        const std::size_t request = (std::min)(
            bytes.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<ssize_t>::max)()));
        const ssize_t count = ::write(descriptor, bytes.data() + offset, request);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            written = false;
            break;
        }
        offset += static_cast<std::size_t>(count);
    }
    int sync_result = 0;
    do {
        sync_result = ::fsync(descriptor);
    } while (sync_result != 0 && errno == EINTR);
    const int close_result = ::close(descriptor);
    if (!written || sync_result != 0 || close_result != 0) {
        return fail(error, "Could not write and sync complete palette output.");
    }
#endif
    if (!install_output(temporary, destination, overwrite, error)) {
        return false;
    }
    cleanup.dismiss();
    return true;
}

std::string make_kpl_xml(const PaletteDocument& source, PaletteIoSummary& summary) {
    const std::size_t columns = source.columns.value_or(
        (std::min)(source.entries.size(), static_cast<std::size_t>(16U)));
    std::ostringstream xml;
    xml.imbue(std::locale::classic());
    xml << std::setprecision(17);
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<ColorSet name=\"" << escape_xml(source.name)
        << "\" version=\"1.0\" readonly=\"false\" columns=\""
        << columns << "\">\n";
    for (std::size_t index = 0U; index < source.entries.size(); ++index) {
        const PaletteEntry& entry = source.entries[index];
        const std::array<float, 4U> component{{
            static_cast<float>(entry.red), static_cast<float>(entry.green),
            static_cast<float>(entry.blue), static_cast<float>(entry.alpha)}};
        if (static_cast<double>(component[0U]) != entry.red
            || static_cast<double>(component[1U]) != entry.green
            || static_cast<double>(component[2U]) != entry.blue
            || static_cast<double>(component[3U]) != entry.alpha) {
            summary.precision_lost = true;
        }
        xml << "  <ColorSetEntry name=\"" << escape_xml(entry.name)
            << "\" id=\"" << (index + 1U)
            << "\" bitdepth=\"F32\" spot=\"false\">\n";
        xml << "    <RGB";
        if (entry.source_encoding == ColorEncoding::SRGB) {
            xml << " space=\"sRGB-elle-V2-srgbtrc.icc\"";
        }
        xml << " r=\"" << component[0U] << "\" g=\"" << component[1U]
            << "\" b=\"" << component[2U] << "\" a=\"" << component[3U]
            << "\" />\n"
            << "    <Position row=\"" << (index / columns)
            << "\" column=\"" << (index % columns) << "\" />\n"
            << "  </ColorSetEntry>\n";
    }
    xml << "</ColorSet>\n";
    return xml.str();
}

bool write_kpl(const std::string& path, const PaletteDocument& source,
               bool overwrite, PaletteIoSummary& summary, std::string* error) {
    MemoryStreamGuard stream;
    if (stream.handle == nullptr
        || mz_stream_mem_open(stream.handle, "palette.kpl",
                              MZ_OPEN_MODE_CREATE | MZ_OPEN_MODE_WRITE) != MZ_OK) {
        return fail(error, "Could not allocate in-memory Krita ZIP output.");
    }
    stream.open = true;
    ZipWriterGuard writer;
    if (writer.handle == nullptr) return fail(error, "Could not allocate Krita ZIP writer.");
    if (mz_zip_writer_open(writer.handle, stream.handle, 0) != MZ_OK) {
        return fail(error, "Could not create in-memory Krita KPL ZIP.");
    }
    writer.open = true;
    const std::string mimetype = "application/x-krita-palette";
    mz_zip_writer_set_compress_method(writer.handle, MZ_COMPRESS_METHOD_STORE);
    mz_zip_writer_set_compress_level(writer.handle, MZ_COMPRESS_LEVEL_DEFAULT);
    mz_zip_file info{};
    info.filename = "mimetype";
    info.filename_size = static_cast<std::uint16_t>(std::strlen(info.filename));
    info.uncompressed_size = static_cast<std::int64_t>(mimetype.size());
    if (mz_zip_writer_add_buffer(writer.handle, const_cast<char*>(mimetype.data()),
                                 static_cast<std::int32_t>(mimetype.size()), &info) != MZ_OK) {
        return fail(error, "Could not write Krita KPL mimetype entry.");
    }
    const std::string xml = make_kpl_xml(source, summary);
    if (xml.size() > kMaximumKplXmlBytes
        || xml.size() > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)())) {
        return fail(error, "Krita colorset.xml exceeds the 16 MiB output limit.");
    }
    // Store colorset.xml as well as mimetype.  Palette XML is bounded and
    // small; avoiding extreme deflate ratios ensures every file we export also
    // passes the importer's archive-bomb policy.
    mz_zip_writer_set_compress_method(writer.handle, MZ_COMPRESS_METHOD_STORE);
    mz_zip_writer_set_compress_level(writer.handle, MZ_COMPRESS_LEVEL_DEFAULT);
    info = {};
    info.filename = "colorset.xml";
    info.filename_size = static_cast<std::uint16_t>(std::strlen(info.filename));
    info.uncompressed_size = static_cast<std::int64_t>(xml.size());
    if (mz_zip_writer_add_buffer(writer.handle, const_cast<char*>(xml.data()),
                                 static_cast<std::int32_t>(xml.size()), &info) != MZ_OK
        || mz_zip_writer_close(writer.handle) != MZ_OK) {
        return fail(error, "Could not finalize Krita KPL ZIP.");
    }
    writer.open = false;
    const void* buffer = nullptr;
    int32_t length = 0;
    mz_stream_mem_get_buffer_length(stream.handle, &length);
    if (length <= 0 || mz_stream_mem_get_buffer(stream.handle, &buffer) != MZ_OK
        || buffer == nullptr) {
        return fail(error, "Could not read completed in-memory Krita KPL ZIP.");
    }
    if (summary.precision_lost) {
        add_warning(summary, "Double components were rounded to Krita F32 precision.");
    }
    return write_atomic_bytes(
        path, std::string_view(static_cast<const char*>(buffer),
                               static_cast<std::size_t>(length)),
        overwrite, error);
}

std::FILE* open_binary_input(const fs::path& path) {
#if defined(_WIN32)
    std::FILE* file = nullptr;
    return _wfopen_s(&file, path.wstring().c_str(), L"rb") == 0 ? file : nullptr;
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

using Pixel = std::array<double, 4U>;

std::array<std::uint64_t, 4U> canonical_pixel_key(const Pixel& pixel) {
    std::array<std::uint64_t, 4U> key{};
    for (std::size_t channel = 0U; channel < pixel.size(); ++channel) {
        const double normalized = pixel[channel] == 0.0 ? 0.0 : pixel[channel];
        static_assert(sizeof(normalized) == sizeof(key[channel]),
                      "Palette keys require 64-bit doubles");
        std::memcpy(&key[channel], &normalized, sizeof(normalized));
    }
    return key;
}

bool import_image_pixels(const std::vector<Pixel>& pixels,
                         std::size_t columns,
                         ColorEncoding encoding,
                         PaletteDocument& document,
                         PaletteIoSummary& summary,
                         std::string* error) {
    std::set<std::array<std::uint64_t, 4U>> seen;
    document.columns = columns;
    for (std::size_t index = 0U; index < pixels.size(); ++index) {
        ++summary.scanned;
        const Pixel& pixel = pixels[index];
        if (!std::isfinite(pixel[0U]) || !std::isfinite(pixel[1U])
            || !std::isfinite(pixel[2U]) || !std::isfinite(pixel[3U])) {
            return fail(error, "Palette image contains NaN or infinity.");
        }
        if (pixel[3U] < 0.0 || pixel[3U] > 1.0) {
            return fail(error, "Palette image alpha is outside [0, 1].");
        }
        if (pixel[3U] == 0.0) {
            ++summary.transparent_ignored;
            continue;
        }
        if (!seen.insert(canonical_pixel_key(pixel)).second) {
            ++summary.duplicates_ignored;
            continue;
        }
        PaletteEntry entry;
        entry.red = pixel[0U];
        entry.green = pixel[1U];
        entry.blue = pixel[2U];
        entry.alpha = pixel[3U];
        entry.source_encoding = encoding;
        entry.source_order = index;
        document.entries.push_back(entry);
        if (document.entries.size() > kMaximumPaletteEntries) {
            return fail(error, "Palette image exceeds the one-million-color limit.");
        }
    }
    summary.accepted = document.entries.size();
    if (summary.transparent_ignored != 0U) {
        add_warning(summary, "Fully transparent image pixels were ignored.");
    }
    if (summary.duplicates_ignored != 0U) {
        add_warning(summary,
                    "Duplicate decoded RGBA image pixels were ignored after their first occurrence.");
    }
    return ensure_import_has_entries(document, summary, error);
}

bool import_png(const std::string& path, PaletteDocument& document,
                PaletteIoSummary& summary, std::string* error) {
    fs::path native;
    std::uintmax_t file_size = 0U;
    if (!inspect_import_path(path, native, file_size, error)) return false;
    (void)file_size;
    std::FILE* file = open_binary_input(native);
    if (file == nullptr) return fail(error, "Could not open PNG palette image.");
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_stdio(&png, file) == 0) {
        const std::string message = png.message;
        std::fclose(file);
        return fail(error, "Could not read PNG palette metadata: " + message);
    }
    std::size_t pixel_count = 0U;
    std::size_t components = 0U;
    if (png.width == 0U || png.height == 0U
        || !checked_multiply(static_cast<std::size_t>(png.width),
                             static_cast<std::size_t>(png.height), pixel_count)
        || pixel_count > kMaximumPaletteEntries
        || !checked_multiply(pixel_count, 4U, components)) {
        png_image_free(&png);
        std::fclose(file);
        return fail(error, "PNG palette dimensions exceed the one-million-pixel limit.");
    }
    png.format = PNG_FORMAT_LINEAR_RGB_ALPHA;
    std::vector<png_uint_16> decoded(components);
    if (png_image_finish_read(&png, nullptr, decoded.data(), 0, nullptr) == 0) {
        const std::string message = png.message;
        png_image_free(&png);
        std::fclose(file);
        return fail(error, "Could not decode PNG palette image: " + message);
    }
    const std::size_t width = static_cast<std::size_t>(png.width);
    png_image_free(&png);
    std::fclose(file);
    std::vector<Pixel> pixels(pixel_count);
    constexpr double scale = 1.0 / 65535.0;
    for (std::size_t pixel = 0U; pixel < pixel_count; ++pixel) {
        for (std::size_t channel = 0U; channel < 4U; ++channel) {
            pixels[pixel][channel] =
                static_cast<double>(decoded[pixel * 4U + channel]) * scale;
        }
    }
    // libpng's simplified linear format is the canonical decoded value used
    // for exact deduplication; alpha remains coverage rather than gamma-coded.
    return import_image_pixels(pixels, width, ColorEncoding::Linear,
                               document, summary, error);
}

bool read_u32(const std::vector<unsigned char>& bytes, std::size_t& position,
              std::uint32_t& value) {
    if (position > bytes.size() || bytes.size() - position < 4U) return false;
    value = static_cast<std::uint32_t>(bytes[position])
            | (static_cast<std::uint32_t>(bytes[position + 1U]) << 8U)
            | (static_cast<std::uint32_t>(bytes[position + 2U]) << 16U)
            | (static_cast<std::uint32_t>(bytes[position + 3U]) << 24U);
    position += 4U;
    return true;
}

bool read_u64(const std::vector<unsigned char>& bytes, std::size_t& position,
              std::uint64_t& value) {
    if (position > bytes.size() || bytes.size() - position < 8U) return false;
    value = 0U;
    for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
        value |= static_cast<std::uint64_t>(bytes[position++]) << shift;
    }
    return true;
}

bool read_c_string(const std::vector<unsigned char>& bytes, std::size_t& position,
                   std::string& value, std::size_t maximum) {
    value.clear();
    while (position < bytes.size() && bytes[position] != 0U) {
        if (value.size() >= maximum) return false;
        value.push_back(static_cast<char>(bytes[position++]));
    }
    if (position >= bytes.size()) return false;
    ++position;
    return true;
}

std::int32_t signed_u32(std::uint32_t value) {
    std::int32_t result = 0;
    static_assert(sizeof(result) == sizeof(value), "EXR coordinates require 32 bits");
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

float float_u32(std::uint32_t value) {
    float result = 0.0F;
    static_assert(sizeof(result) == sizeof(value), "EXR FLOAT requires 32 bits");
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

struct ExrChannelInfo {
    std::string name;
    int rgba_index = -1;
};

struct ExrHeaderInfo {
    std::vector<ExrChannelInfo> channels;
    std::uint8_t compression = 0U;
    std::int32_t minimum_x = 0;
    std::int32_t minimum_y = 0;
    std::int32_t maximum_x = -1;
    std::int32_t maximum_y = -1;
    std::uint8_t line_order = 0U;
    bool has_channels = false;
    bool has_compression = false;
    bool has_data_window = false;
};

bool parse_exr_channels(const std::vector<unsigned char>& bytes,
                        std::size_t begin, std::size_t size,
                        ExrHeaderInfo& header, std::string* error) {
    if (header.has_channels) return fail(error, "OpenEXR has duplicate channels attributes.");
    const std::size_t end = begin + size;
    std::size_t position = begin;
    std::set<std::string> names;
    while (position < end) {
        if (bytes[position] == 0U) {
            ++position;
            if (position != end) return fail(error, "OpenEXR channel list has trailing data.");
            header.has_channels = true;
            return true;
        }
        std::string name;
        const std::size_t name_start = position;
        while (position < end && bytes[position] != 0U && position - name_start <= 255U) {
            name.push_back(static_cast<char>(bytes[position++]));
        }
        if (position >= end || name.empty() || position - name_start > 255U) {
            return fail(error, "OpenEXR channel list is malformed.");
        }
        ++position;
        if (end - position < 16U) return fail(error, "OpenEXR channel record is truncated.");
        std::size_t field = position;
        std::uint32_t pixel_type = 0U;
        std::uint32_t x_sampling = 0U;
        std::uint32_t y_sampling = 0U;
        if (!read_u32(bytes, field, pixel_type)) return false;
        field += 4U; // pLinear plus three reserved bytes.
        if (!read_u32(bytes, field, x_sampling) || !read_u32(bytes, field, y_sampling)) {
            return fail(error, "OpenEXR channel record is truncated.");
        }
        position = field;
        if (pixel_type != 2U) {
            return fail(error, "OpenEXR palette channels must all be 32-bit FLOAT.");
        }
        if (x_sampling != 1U || y_sampling != 1U) {
            return fail(error, "Subsampled OpenEXR palette channels are not supported.");
        }
        const std::string folded = lower_ascii(name);
        if (!names.insert(folded).second) return fail(error, "OpenEXR has duplicate channels.");
        ExrChannelInfo channel;
        channel.name = name;
        if (folded == "r") channel.rgba_index = 0;
        else if (folded == "g") channel.rgba_index = 1;
        else if (folded == "b") channel.rgba_index = 2;
        else if (folded == "a") channel.rgba_index = 3;
        header.channels.push_back(std::move(channel));
        if (header.channels.size() > 64U) return fail(error, "OpenEXR has too many channels.");
    }
    return fail(error, "OpenEXR channel list has no terminator.");
}

bool parse_exr_header(const std::vector<unsigned char>& bytes,
                      std::size_t& position, ExrHeaderInfo& header,
                      std::string* error) {
    std::uint32_t magic = 0U;
    std::uint32_t version = 0U;
    if (!read_u32(bytes, position, magic) || !read_u32(bytes, position, version)
        || magic != 20000630U) {
        return fail(error, "Input is not an OpenEXR file.");
    }
    if ((version & 0xffU) != 2U || (version & 0xffffff00U) != 0U) {
        return fail(error,
                    "Only single-part, non-tiled OpenEXR version 2 scanline images are supported.");
    }
    std::set<std::string> attributes;
    while (position < bytes.size()) {
        if (bytes[position] == 0U) {
            ++position;
            break;
        }
        std::string name;
        std::string type;
        if (!read_c_string(bytes, position, name, 255U)
            || !read_c_string(bytes, position, type, 255U)) {
            return fail(error, "OpenEXR header has an invalid attribute name or type.");
        }
        if (!attributes.insert(name).second) {
            return fail(error, "OpenEXR header has duplicate attributes.");
        }
        std::uint32_t size = 0U;
        if (!read_u32(bytes, position, size) || size > bytes.size() - position) {
            return fail(error, "OpenEXR header attribute exceeds the file bounds.");
        }
        const std::size_t begin = position;
        if (name == "channels") {
            if (type != "chlist"
                || !parse_exr_channels(bytes, begin, size, header, error)) return false;
        } else if (name == "compression") {
            if (type != "compression" || size != 1U || header.has_compression) {
                return fail(error, "OpenEXR compression attribute is malformed.");
            }
            header.compression = bytes[begin];
            header.has_compression = true;
        } else if (name == "dataWindow") {
            if (type != "box2i" || size != 16U || header.has_data_window) {
                return fail(error, "OpenEXR dataWindow attribute is malformed.");
            }
            std::size_t field = begin;
            std::uint32_t values[4]{};
            for (std::uint32_t& value : values) {
                if (!read_u32(bytes, field, value)) return false;
            }
            header.minimum_x = signed_u32(values[0U]);
            header.minimum_y = signed_u32(values[1U]);
            header.maximum_x = signed_u32(values[2U]);
            header.maximum_y = signed_u32(values[3U]);
            header.has_data_window = true;
        } else if (name == "lineOrder") {
            if (type != "lineOrder" || size != 1U) {
                return fail(error, "OpenEXR lineOrder attribute is malformed.");
            }
            header.line_order = bytes[begin];
        }
        position += size;
    }
    if (!header.has_channels || !header.has_compression || !header.has_data_window) {
        return fail(error, "OpenEXR is missing channels, compression, or dataWindow.");
    }
    std::array<bool, 3U> rgb{{false, false, false}};
    for (const ExrChannelInfo& channel : header.channels) {
        if (channel.rgba_index >= 0 && channel.rgba_index < 3) {
            rgb[static_cast<std::size_t>(channel.rgba_index)] = true;
        }
    }
    if (!rgb[0U] || !rgb[1U] || !rgb[2U]) {
        return fail(error, "OpenEXR palette image must contain R, G, and B FLOAT channels.");
    }
    if (header.line_order != 0U) {
        return fail(error, "Only increasing-Y OpenEXR scanline order is supported.");
    }
    if (header.compression != 0U && header.compression != 2U
        && header.compression != 3U) {
        return fail(error,
                    "OpenEXR palette compression is unsupported; use NO_COMPRESSION, ZIPS, or ZIP.");
    }
    return true;
}

bool decode_exr_zip(const unsigned char* source, std::size_t source_size,
                    std::vector<unsigned char>& decoded, std::string* error) {
    if (source_size == decoded.size()) {
        std::copy_n(source, source_size, decoded.begin());
        return true;
    }
    uLongf decoded_size = static_cast<uLongf>(decoded.size());
    const int result = ::uncompress(decoded.data(), &decoded_size, source,
                                    static_cast<uLong>(source_size));
    if (result != Z_OK || decoded_size != decoded.size()) {
        return fail(error, "Could not inflate OpenEXR ZIP scanline block.");
    }
    for (std::size_t index = 1U; index < decoded.size(); ++index) {
        decoded[index] = static_cast<unsigned char>(
            static_cast<int>(decoded[index - 1U]) + static_cast<int>(decoded[index]) - 128);
    }
    std::vector<unsigned char> interleaved(decoded.size());
    const unsigned char* even = decoded.data();
    const unsigned char* odd = decoded.data() + (decoded.size() + 1U) / 2U;
    for (std::size_t index = 0U; index < interleaved.size(); ++index) {
        interleaved[index] = (index & 1U) == 0U ? *even++ : *odd++;
    }
    decoded.swap(interleaved);
    return true;
}

bool import_exr(const std::string& path, PaletteDocument& document,
                PaletteIoSummary& summary, std::string* error) {
    std::string raw;
    if (!read_file(path, raw, error)) return false;
    std::vector<unsigned char> bytes(raw.begin(), raw.end());
    std::size_t position = 0U;
    ExrHeaderInfo header;
    if (!parse_exr_header(bytes, position, header, error)) return false;
    const std::int64_t width_signed = static_cast<std::int64_t>(header.maximum_x)
                                      - header.minimum_x + 1;
    const std::int64_t height_signed = static_cast<std::int64_t>(header.maximum_y)
                                       - header.minimum_y + 1;
    if (width_signed <= 0 || height_signed <= 0) {
        return fail(error, "OpenEXR dataWindow dimensions are invalid.");
    }
    const std::size_t width = static_cast<std::size_t>(width_signed);
    const std::size_t height = static_cast<std::size_t>(height_signed);
    std::size_t pixel_count = 0U;
    if (!checked_multiply(width, height, pixel_count)
        || pixel_count > kMaximumPaletteEntries) {
        return fail(error, "OpenEXR palette exceeds the one-million-pixel limit.");
    }
    std::size_t decoded_samples = 0U;
    std::size_t total_decoded_bytes = 0U;
    if (!checked_multiply(pixel_count, header.channels.size(), decoded_samples)
        || !checked_multiply(decoded_samples, sizeof(float), total_decoded_bytes)
        || total_decoded_bytes > kMaximumPaletteDecodedBytes) {
        return fail(error, "OpenEXR palette exceeds the 64 MiB decoded-channel limit.");
    }
    const std::size_t lines_per_block = header.compression == 3U ? 16U : 1U;
    const std::size_t chunk_count = (height + lines_per_block - 1U) / lines_per_block;
    std::vector<std::uint64_t> offsets(chunk_count);
    for (std::uint64_t& offset : offsets) {
        if (!read_u64(bytes, position, offset) || offset > bytes.size()) {
            return fail(error, "OpenEXR scanline offset table is truncated or invalid.");
        }
    }
    std::vector<Pixel> pixels(pixel_count, Pixel{{0.0, 0.0, 0.0, 1.0}});
    std::vector<bool> rows_seen(height, false);
    for (const std::uint64_t chunk_offset : offsets) {
        std::size_t chunk = static_cast<std::size_t>(chunk_offset);
        std::uint32_t encoded_y = 0U;
        std::uint32_t encoded_size = 0U;
        if (!read_u32(bytes, chunk, encoded_y) || !read_u32(bytes, chunk, encoded_size)
            || encoded_size > bytes.size() - chunk) {
            return fail(error, "OpenEXR scanline block is outside the file bounds.");
        }
        const std::int32_t y = signed_u32(encoded_y);
        if (y < header.minimum_y || y > header.maximum_y) {
            return fail(error, "OpenEXR scanline block has an out-of-range Y coordinate.");
        }
        const std::size_t first_row = static_cast<std::size_t>(y - header.minimum_y);
        const std::size_t row_count = (std::min)(lines_per_block, height - first_row);
        std::size_t samples = 0U;
        std::size_t decoded_bytes = 0U;
        if (!checked_multiply(row_count, header.channels.size(), samples)
            || !checked_multiply(samples, width, samples)
            || !checked_multiply(samples, sizeof(float), decoded_bytes)) {
            return fail(error, "OpenEXR scanline decoded size overflows.");
        }
        std::vector<unsigned char> decoded(decoded_bytes);
        if (header.compression == 0U) {
            if (encoded_size != decoded_bytes) {
                return fail(error, "Uncompressed OpenEXR scanline size is invalid.");
            }
            std::copy_n(bytes.data() + chunk, decoded_bytes, decoded.begin());
        } else if (!decode_exr_zip(bytes.data() + chunk, encoded_size, decoded, error)) {
            return false;
        }
        std::size_t decoded_position = 0U;
        for (std::size_t row = 0U; row < row_count; ++row) {
            const std::size_t destination_row = first_row + row;
            if (rows_seen[destination_row]) {
                return fail(error, "OpenEXR scanline blocks overlap.");
            }
            rows_seen[destination_row] = true;
            for (const ExrChannelInfo& channel : header.channels) {
                for (std::size_t x = 0U; x < width; ++x) {
                    std::uint32_t bits = 0U;
                    bits = static_cast<std::uint32_t>(decoded[decoded_position])
                           | (static_cast<std::uint32_t>(decoded[decoded_position + 1U]) << 8U)
                           | (static_cast<std::uint32_t>(decoded[decoded_position + 2U]) << 16U)
                           | (static_cast<std::uint32_t>(decoded[decoded_position + 3U]) << 24U);
                    decoded_position += 4U;
                    if (channel.rgba_index >= 0) {
                        pixels[destination_row * width + x]
                              [static_cast<std::size_t>(channel.rgba_index)] =
                                  static_cast<double>(float_u32(bits));
                    }
                }
            }
        }
        if (decoded_position != decoded.size()) {
            return fail(error, "OpenEXR scanline block has trailing decoded data.");
        }
    }
    if (std::find(rows_seen.begin(), rows_seen.end(), false) != rows_seen.end()) {
        return fail(error, "OpenEXR scanline table does not cover every row.");
    }
    return import_image_pixels(pixels, width, ColorEncoding::Linear,
                               document, summary, error);
}

void append_exr_u32(std::vector<unsigned char>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<unsigned char>(value & 0xffU));
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<unsigned char>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<unsigned char>((value >> 24U) & 0xffU));
}

void append_exr_u64(std::vector<unsigned char>& bytes, std::uint64_t value) {
    for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<unsigned char>((value >> shift) & 0xffU));
    }
}

void append_exr_float(std::vector<unsigned char>& bytes, float value) {
    std::uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    append_exr_u32(bytes, bits);
}

void append_exr_c_string(std::vector<unsigned char>& bytes, const char* value) {
    bytes.insert(bytes.end(), value, value + std::strlen(value));
    bytes.push_back(0U);
}

void append_exr_attribute(std::vector<unsigned char>& header,
                          const char* name, const char* type,
                          const std::vector<unsigned char>& value) {
    append_exr_c_string(header, name);
    append_exr_c_string(header, type);
    append_exr_u32(header, static_cast<std::uint32_t>(value.size()));
    header.insert(header.end(), value.begin(), value.end());
}

bool write_png_palette(const std::string& path, std::size_t width,
                       std::size_t height, const PaletteDocument& source,
                       bool overwrite, PaletteIoSummary& summary,
                       std::string* error) {
    std::size_t pixel_count = 0U;
    std::size_t component_count = 0U;
    if (!checked_multiply(width, height, pixel_count)
        || !checked_multiply(pixel_count, 4U, component_count)) {
        return fail(error, "PNG palette image buffer size overflows.");
    }
    std::vector<unsigned char> rgba(component_count, 0U);
    for (std::size_t index = 0U; index < source.entries.size(); ++index) {
        const Encoded8 encoded = encode_srgb8(source.entries[index], summary);
        for (std::size_t channel = 0U; channel < 4U; ++channel) {
            rgba[index * 4U + channel] =
                static_cast<unsigned char>(encoded.component[channel]);
        }
    }
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    png.width = static_cast<png_uint_32>(width);
    png.height = static_cast<png_uint_32>(height);
    png.format = PNG_FORMAT_RGBA;
    png_alloc_size_t encoded_size = 0U;
    if (png_image_write_to_memory(&png, nullptr, &encoded_size, 0,
                                  rgba.data(), 0, nullptr) == 0
        || encoded_size == 0U || encoded_size > kMaximumPaletteFileBytes) {
        const std::string message = png.message;
        png_image_free(&png);
        return fail(error, "Could not encode PNG palette image: " + message);
    }
    std::vector<unsigned char> encoded(static_cast<std::size_t>(encoded_size));
    if (png_image_write_to_memory(&png, encoded.data(), &encoded_size, 0,
                                  rgba.data(), 0, nullptr) == 0) {
        const std::string message = png.message;
        png_image_free(&png);
        return fail(error, "Could not encode PNG palette image: " + message);
    }
    png_image_free(&png);
    const std::string_view bytes(reinterpret_cast<const char*>(encoded.data()),
                                 static_cast<std::size_t>(encoded_size));
    return write_atomic_bytes(path, bytes, overwrite, error);
}

bool write_exr_palette(const std::string& path, std::size_t width,
                       std::size_t height, const PaletteDocument& source,
                       bool overwrite, PaletteIoSummary& summary,
                       std::string* error) {
    struct Channel {
        const char* name;
        std::size_t index;
    };
    const std::array<Channel, 4U> channels{{
        {"A", 3U}, {"B", 2U}, {"G", 1U}, {"R", 0U}}};
    std::size_t pixel_count = 0U;
    if (!checked_multiply(width, height, pixel_count)) {
        return fail(error, "OpenEXR palette image dimensions overflow.");
    }
    std::vector<std::array<float, 4U>> pixels(
        pixel_count, std::array<float, 4U>{{0.0F, 0.0F, 0.0F, 0.0F}});
    for (std::size_t index = 0U; index < source.entries.size(); ++index) {
        const PaletteEntry& entry = source.entries[index];
        const std::array<double, 4U> linear{{
            entry.source_encoding == ColorEncoding::SRGB ? srgb_to_linear(entry.red)
                                                         : entry.red,
            entry.source_encoding == ColorEncoding::SRGB ? srgb_to_linear(entry.green)
                                                         : entry.green,
            entry.source_encoding == ColorEncoding::SRGB ? srgb_to_linear(entry.blue)
                                                         : entry.blue,
            entry.alpha}};
        if (entry.source_encoding == ColorEncoding::SRGB) summary.encoding_converted = true;
        for (std::size_t channel = 0U; channel < 4U; ++channel) {
            pixels[index][channel] = static_cast<float>(linear[channel]);
            if (static_cast<double>(pixels[index][channel]) != linear[channel]) {
                summary.precision_lost = true;
            }
        }
    }

    std::vector<unsigned char> header;
    append_exr_u32(header, 20000630U);
    append_exr_u32(header, 2U);
    std::vector<unsigned char> value;
    for (const Channel& channel : channels) {
        append_exr_c_string(value, channel.name);
        append_exr_u32(value, 2U);
        value.insert(value.end(), 4U, 0U);
        append_exr_u32(value, 1U);
        append_exr_u32(value, 1U);
    }
    value.push_back(0U);
    append_exr_attribute(header, "channels", "chlist", value);
    value.assign(1U, 0U);
    append_exr_attribute(header, "compression", "compression", value);
    value.clear();
    append_exr_u32(value, 0U);
    append_exr_u32(value, 0U);
    append_exr_u32(value, static_cast<std::uint32_t>(width - 1U));
    append_exr_u32(value, static_cast<std::uint32_t>(height - 1U));
    append_exr_attribute(header, "dataWindow", "box2i", value);
    append_exr_attribute(header, "displayWindow", "box2i", value);
    value.assign(1U, 0U);
    append_exr_attribute(header, "lineOrder", "lineOrder", value);
    value.clear();
    append_exr_float(value, 1.0F);
    append_exr_attribute(header, "pixelAspectRatio", "float", value);
    value.clear();
    append_exr_float(value, 0.0F);
    append_exr_float(value, 0.0F);
    append_exr_attribute(header, "screenWindowCenter", "v2f", value);
    value.clear();
    append_exr_float(value, 1.0F);
    append_exr_attribute(header, "screenWindowWidth", "float", value);
    header.push_back(0U);

    std::size_t scanline_samples = 0U;
    std::size_t scanline_bytes = 0U;
    if (!checked_multiply(width, channels.size(), scanline_samples)
        || !checked_multiply(scanline_samples, sizeof(float), scanline_bytes)) {
        return fail(error, "OpenEXR palette scanline size overflows.");
    }
    const std::uint64_t first_chunk = static_cast<std::uint64_t>(header.size())
                                      + static_cast<std::uint64_t>(height) * 8U;
    const std::uint64_t chunk_bytes = 8U + static_cast<std::uint64_t>(scanline_bytes);
    std::vector<unsigned char> encoded = header;
    encoded.reserve(static_cast<std::size_t>(first_chunk
                    + static_cast<std::uint64_t>(height) * chunk_bytes));
    for (std::size_t y = 0U; y < height; ++y) {
        append_exr_u64(encoded, first_chunk + static_cast<std::uint64_t>(y) * chunk_bytes);
    }
    for (std::size_t y = 0U; y < height; ++y) {
        append_exr_u32(encoded, static_cast<std::uint32_t>(y));
        append_exr_u32(encoded, static_cast<std::uint32_t>(scanline_bytes));
        for (const Channel& channel : channels) {
            for (std::size_t x = 0U; x < width; ++x) {
                append_exr_float(encoded, pixels[y * width + x][channel.index]);
            }
        }
    }
    const std::string_view bytes(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    return write_atomic_bytes(path, bytes, overwrite, error);
}

bool export_image(const std::string& path, PaletteFormat format,
                  const PaletteDocument& source, bool overwrite,
                  PaletteIoSummary& summary, std::string* error) {
    std::size_t width = source.columns.value_or(
        (std::min)(source.entries.size(), static_cast<std::size_t>(256U)));
    width = (std::min)(width, source.entries.size());
    std::size_t height = (source.entries.size() + width - 1U) / width;
    std::size_t pixel_count = 0U;
    if (!checked_multiply(width, height, pixel_count)
        || pixel_count > kMaximumPaletteEntries) {
        width = source.entries.size();
        height = 1U;
        add_warning(summary,
                    "Image layout was flattened to stay within the one-million-pixel limit.");
    }
    if (width > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())
        || height > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        return fail(error, "Palette image dimensions exceed 32-bit format limits.");
    }
    for (const PaletteEntry& entry : source.entries) {
        if (entry.alpha == 0.0) {
            add_warning(summary,
                        "Fully transparent palette entries are written but will be ignored on image reimport.");
        }
    }
    const bool written = format == PaletteFormat::FloatExrImage
                             ? write_exr_palette(path, width, height, source,
                                                 overwrite, summary, error)
                             : write_png_palette(path, width, height, source,
                                                 overwrite, summary, error);
    if (!written) return false;
    summary.scanned = source.entries.size();
    summary.accepted = source.entries.size();
    summary.names_lost = std::any_of(
        source.entries.begin(), source.entries.end(),
        [](const PaletteEntry& entry) { return !entry.name.empty(); });
    if (summary.names_lost) add_warning(summary, "Palette image formats do not preserve entry names.");
    if (format == PaletteFormat::FloatExrImage && summary.encoding_converted) {
        add_warning(summary, "Encoded sRGB colors were converted to linear RGB for OpenEXR.");
    }
    if (summary.precision_lost) {
        add_warning(summary, format == PaletteFormat::FloatExrImage
                                 ? "Double components were rounded to OpenEXR FLOAT precision."
                                 : "Components were quantized for 8-bit PNG.");
    }
    return true;
}

void note_name_loss(const PaletteDocument& source, PaletteIoSummary& summary) {
    summary.names_lost = std::any_of(
        source.entries.begin(), source.entries.end(),
        [](const PaletteEntry& entry) { return !entry.name.empty(); });
    if (summary.names_lost) add_warning(summary, "This format does not preserve entry names.");
}

void note_alpha_loss(const PaletteDocument& source, PaletteIoSummary& summary) {
    summary.alpha_lost = std::any_of(
        source.entries.begin(), source.entries.end(),
        [](const PaletteEntry& entry) { return entry.alpha != 1.0; });
    if (summary.alpha_lost) add_warning(summary, "This format does not preserve alpha.");
}

std::string gpl_line_text(std::string text, PaletteIoSummary& summary) {
    bool changed = false;
    for (char& raw : text) {
        if (raw == '\n' || raw == '\r') {
            raw = ' ';
            changed = true;
        }
    }
    if (changed) {
        summary.names_lost = true;
        add_warning(summary, "Line breaks in names were replaced for GIMP GPL output.");
    }
    return text;
}

std::string serialize_gpl(const PaletteDocument& source, PaletteIoSummary& summary) {
    std::ostringstream output;
    output << "GIMP Palette\nName: " << gpl_line_text(source.name, summary) << "\n";
    if (source.columns.has_value()) output << "Columns: " << *source.columns << "\n";
    output << "# Exported by Procedural Visualizer Tool\n";
    for (const PaletteEntry& entry : source.entries) {
        const Encoded8 encoded = encode_srgb8(entry, summary);
        output << std::setw(3) << encoded.component[0U] << ' '
               << std::setw(3) << encoded.component[1U] << ' '
               << std::setw(3) << encoded.component[2U];
        if (!entry.name.empty()) output << '\t' << gpl_line_text(entry.name, summary);
        output << '\n';
    }
    note_alpha_loss(source, summary);
    return output.str();
}

std::string serialize_text(const PaletteDocument& source, PaletteIoSummary& summary) {
    std::string output;
    output.reserve(source.entries.size() * 10U);
    for (const PaletteEntry& entry : source.entries) {
        const Encoded8 encoded = encode_srgb8(entry, summary);
        output += hex_color(encoded, encoded.component[3U] != 255U);
        output.push_back('\n');
    }
    note_name_loss(source, summary);
    return output;
}

std::string display_name(const PaletteEntry& entry, std::size_t index) {
    return entry.name.empty() ? "Color " + std::to_string(index + 1U) : entry.name;
}

std::string serialize_css(const PaletteDocument& source, PaletteIoSummary& summary) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "/* Generated by Procedural Visualizer Tool; GIMP-compatible CSS colors. */\n";
    std::set<std::string> used;
    for (std::size_t index = 0U; index < source.entries.size(); ++index) {
        const PaletteEntry& entry = source.entries[index];
        const std::string original = display_name(entry, index);
        const std::string identifier = unique_code_name(original, used, true);
        const Encoded8 encoded = encode_srgb8(entry, summary);
        output << '.' << identifier << " { color: ";
        if (encoded.component[3U] == 255U) {
            output << "rgb(" << encoded.component[0U] << ", "
                   << encoded.component[1U] << ", " << encoded.component[2U] << "); }";
        } else {
            output << "rgba(" << encoded.component[0U] << ", "
                   << encoded.component[1U] << ", " << encoded.component[2U] << ", "
                   << std::setprecision(9)
                   << static_cast<double>(encoded.component[3U]) / 255.0 << "); }";
        }
        output << " /* pvt-name: \"" << escape_css_comment_string(original)
               << "\" */\n";
    }
    return output.str();
}

std::string serialize_python(const PaletteDocument& source, PaletteIoSummary& summary) {
    std::ostringstream output;
    output << "# Generated by Procedural Visualizer Tool; GIMP-compatible Python dictionary.\n"
           << "colors = {\n";
    std::set<std::string> used;
    for (std::size_t index = 0U; index < source.entries.size(); ++index) {
        const PaletteEntry& entry = source.entries[index];
        const std::string original = display_name(entry, index);
        const std::string key = unique_code_name(original, used, false);
        const Encoded8 encoded = encode_srgb8(entry, summary);
        output << "    '" << escape_code_string(key, '\'') << "': '"
               << hex_color(encoded, encoded.component[3U] != 255U)
               << "',  # pvt-name: \"" << escape_code_string(original, '"') << "\"\n";
    }
    output << "}\n";
    return output.str();
}

std::string serialize_php(const PaletteDocument& source, PaletteIoSummary& summary) {
    std::ostringstream output;
    output << "<?php\n/* Generated by Procedural Visualizer Tool; GIMP-compatible PHP dictionary. */\n"
           << "$colors = array(\n";
    std::set<std::string> used;
    for (std::size_t index = 0U; index < source.entries.size(); ++index) {
        const PaletteEntry& entry = source.entries[index];
        const std::string original = display_name(entry, index);
        const std::string key = unique_code_name(original, used, false);
        const Encoded8 encoded = encode_srgb8(entry, summary);
        output << "    '" << escape_code_string(key, '\'') << "' => '"
               << hex_color(encoded, encoded.component[3U] != 255U)
               << "',  // pvt-name: \"" << escape_code_string(original, '"') << "\"\n";
    }
    output << ");\n?>\n";
    return output.str();
}

std::string serialize_java(const PaletteDocument& source, PaletteIoSummary& summary) {
    std::set<std::string> class_names;
    const std::string class_name = unique_code_name(
        source.name.empty() ? "PvtPalette" : source.name, class_names, false);
    std::ostringstream output;
    output << "import java.awt.Color;\nimport java.util.LinkedHashMap;\nimport java.util.Map;\n\n"
           << "// Generated by Procedural Visualizer Tool; GIMP-compatible Java map.\n"
           << "public final class " << class_name << " {\n"
           << "    public final Map<String, Color> colors = new LinkedHashMap<>();\n\n"
           << "    public " << class_name << "() {\n";
    std::set<std::string> used;
    for (std::size_t index = 0U; index < source.entries.size(); ++index) {
        const PaletteEntry& entry = source.entries[index];
        const std::string original = display_name(entry, index);
        const std::string key = unique_code_name(original, used, false);
        const Encoded8 encoded = encode_srgb8(entry, summary);
        output << "        colors.put(\"" << escape_code_string(key, '"')
               << "\", new Color(" << encoded.component[0U] << ", "
               << encoded.component[1U] << ", " << encoded.component[2U];
        if (encoded.component[3U] != 255U) output << ", " << encoded.component[3U];
        output << ")); // pvt-name: \"" << escape_code_string(original, '"') << "\"\n";
    }
    output << "    }\n}\n";
    return output.str();
}

bool import_palette_impl(const std::string& path, PaletteFormat format,
                         PaletteDocument& destination,
                         PaletteIoSummary& summary, std::string* error) {
    if (format == PaletteFormat::Auto) format = format_from_path(path);
    if (format == PaletteFormat::Auto) {
        return fail(error, "Could not infer palette format from the file extension.");
    }
    PaletteDocument candidate;
    candidate.name = document_name_from_path(path);
    PaletteIoSummary candidate_summary;
    bool imported = false;
    if (format == PaletteFormat::PngImage) {
        imported = import_png(path, candidate, candidate_summary, error);
    } else if (format == PaletteFormat::FloatExrImage) {
        imported = import_exr(path, candidate, candidate_summary, error);
    } else if (format == PaletteFormat::KritaKpl) {
        std::string xml;
        imported = read_kpl_xml(path, xml, error)
                   && parse_kpl_xml(xml, candidate, candidate_summary, error);
    } else {
        std::string text;
        if (!read_file(path, text, error)) return false;
        if (has_nul(text) || has_forbidden_text_control(text) || !valid_utf8(text)) {
            return fail(error,
                        "Palette text must be valid UTF-8 without NUL or forbidden controls.");
        }
        switch (format) {
            case PaletteFormat::GimpGpl:
                imported = parse_gpl(text, candidate, candidate_summary, error);
                break;
            case PaletteFormat::CssStylesheet:
                imported = parse_css(text, candidate, candidate_summary, error);
                break;
            case PaletteFormat::PythonDictionary:
                imported = parse_dictionary(text, ":", candidate, candidate_summary, error);
                break;
            case PaletteFormat::PhpDictionary:
                imported = parse_dictionary(text, "=>", candidate, candidate_summary, error);
                break;
            case PaletteFormat::JavaMap:
                imported = parse_java(text, candidate, candidate_summary, error);
                break;
            case PaletteFormat::TextHex:
                imported = parse_text_hex(text, candidate, candidate_summary, error);
                break;
            default:
                return fail(error, "Selected palette import format is unsupported.");
        }
    }
    if (!imported) return false;
    destination = std::move(candidate);
    summary = std::move(candidate_summary);
    return true;
}

bool export_palette_impl(const std::string& path, PaletteFormat format,
                         const PaletteDocument& source, bool overwrite,
                         PaletteIoSummary& summary, std::string* error) {
    if (!validate_document(source, error)) return false;
    if (format == PaletteFormat::Auto) format = format_from_path(path);
    if (format == PaletteFormat::Auto) {
        return fail(error, "Could not infer palette format from the file extension.");
    }
    PaletteIoSummary candidate;
    bool exported = false;
    if (format == PaletteFormat::KritaKpl) {
        exported = write_kpl(path, source, overwrite, candidate, error);
    } else if (format == PaletteFormat::PngImage
               || format == PaletteFormat::FloatExrImage) {
        exported = export_image(path, format, source, overwrite, candidate, error);
    } else {
        std::string text;
        switch (format) {
            case PaletteFormat::GimpGpl: text = serialize_gpl(source, candidate); break;
            case PaletteFormat::CssStylesheet: text = serialize_css(source, candidate); break;
            case PaletteFormat::PythonDictionary: text = serialize_python(source, candidate); break;
            case PaletteFormat::PhpDictionary: text = serialize_php(source, candidate); break;
            case PaletteFormat::JavaMap: text = serialize_java(source, candidate); break;
            case PaletteFormat::TextHex: text = serialize_text(source, candidate); break;
            default: return fail(error, "Selected palette export format is unsupported.");
        }
        if (text.size() > kMaximumPaletteFileBytes) {
            return fail(error, "Serialized palette exceeds the 64 MiB output limit.");
        }
        exported = write_atomic_bytes(path, text, overwrite, error);
    }
    if (!exported) return false;
    if (format == PaletteFormat::KritaKpl) {
        candidate.scanned = source.entries.size();
        candidate.accepted = source.entries.size();
    } else if (format != PaletteFormat::PngImage
               && format != PaletteFormat::FloatExrImage) {
        candidate.scanned = source.entries.size();
        candidate.accepted = source.entries.size();
    }
    summary = std::move(candidate);
    return true;
}

} // namespace

PaletteFormat format_from_path(const std::string& path) {
    if (path.empty() || has_nul(path)) return PaletteFormat::Auto;
    const std::string extension = lower_ascii(
        detail::path_to_utf8(detail::path_from_utf8(path).extension()));
    if (extension == ".gpl") return PaletteFormat::GimpGpl;
    if (extension == ".kpl") return PaletteFormat::KritaKpl;
    if (extension == ".css") return PaletteFormat::CssStylesheet;
    if (extension == ".py") return PaletteFormat::PythonDictionary;
    if (extension == ".php") return PaletteFormat::PhpDictionary;
    if (extension == ".java") return PaletteFormat::JavaMap;
    if (extension == ".txt" || extension == ".hex") return PaletteFormat::TextHex;
    if (extension == ".png") return PaletteFormat::PngImage;
    if (extension == ".exr") return PaletteFormat::FloatExrImage;
    return PaletteFormat::Auto;
}

const char* format_name(PaletteFormat format) {
    switch (format) {
        case PaletteFormat::Auto: return "Automatic";
        case PaletteFormat::GimpGpl: return "GIMP Palette (.gpl)";
        case PaletteFormat::KritaKpl: return "Krita Palette (.kpl)";
        case PaletteFormat::CssStylesheet: return "CSS stylesheet";
        case PaletteFormat::PythonDictionary: return "Python dictionary";
        case PaletteFormat::PhpDictionary: return "PHP dictionary";
        case PaletteFormat::JavaMap: return "Java map";
        case PaletteFormat::TextHex: return "Hex text";
        case PaletteFormat::PngImage: return "PNG palette image";
        case PaletteFormat::FloatExrImage: return "FLOAT OpenEXR palette image";
    }
    return "Unknown";
}

bool import_palette(const std::string& path, PaletteFormat format,
                    PaletteDocument& destination, PaletteIoSummary& summary,
                    std::string* error) {
    clear_error(error);
    try {
        return import_palette_impl(path, format, destination, summary, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "Palette import ran out of memory.");
    } catch (const fs::filesystem_error& exception) {
        return fail(error, "Palette import filesystem error: "
                               + std::string(exception.what()));
    } catch (const std::exception& exception) {
        return fail(error, "Palette import failed: " + std::string(exception.what()));
    } catch (...) {
        return fail(error, "Palette import failed with an unknown exception.");
    }
}

bool export_palette(const std::string& path, PaletteFormat format,
                    const PaletteDocument& source, bool overwrite,
                    PaletteIoSummary& summary, std::string* error) {
    clear_error(error);
    try {
        return export_palette_impl(path, format, source, overwrite, summary, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "Palette export ran out of memory.");
    } catch (const fs::filesystem_error& exception) {
        return fail(error, "Palette export filesystem error: "
                               + std::string(exception.what()));
    } catch (const std::exception& exception) {
        return fail(error, "Palette export failed: " + std::string(exception.what()));
    } catch (...) {
        return fail(error, "Palette export failed with an unknown exception.");
    }
}

} // namespace pvt::palette_io
