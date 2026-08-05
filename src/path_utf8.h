#ifndef PVT_PATH_UTF8_H
#define PVT_PATH_UTF8_H

#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

namespace pvt {
namespace detail {

// C++20 changed filesystem's UTF-8 character type from char to char8_t. Keep
// the public API byte-oriented while preserving the standard UTF-8 conversion
// on Windows in both language modes.
inline std::filesystem::path path_from_utf8(std::string_view utf8) {
#if defined(__cpp_lib_char8_t) && __cpp_lib_char8_t >= 201907L
    std::u8string converted(utf8.size(), u8'\0');
    if (!utf8.empty()) {
        std::memcpy(converted.data(), utf8.data(), utf8.size());
    }
    return std::filesystem::path(converted);
#else
    return std::filesystem::u8path(utf8.begin(), utf8.end());
#endif
}

inline std::string path_to_utf8(const std::filesystem::path& path) {
    const auto converted = path.u8string();
#if defined(__cpp_lib_char8_t) && __cpp_lib_char8_t >= 201907L
    std::string utf8(converted.size(), '\0');
    if (!converted.empty()) {
        std::memcpy(utf8.data(), converted.data(), converted.size());
    }
    return utf8;
#else
    return converted;
#endif
}

} // namespace detail
} // namespace pvt

#endif
