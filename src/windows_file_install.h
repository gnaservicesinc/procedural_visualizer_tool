#ifndef PVT_WINDOWS_FILE_INSTALL_H
#define PVT_WINDOWS_FILE_INSTALL_H

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>

namespace pvt {
namespace detail {

inline bool windows_path_missing(DWORD code) {
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}

inline bool windows_path_appeared(DWORD code) {
    return code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS;
}

// Installs a closed, flushed sibling temporary. ReplaceFileW is essential for
// regular-file overwrites because it carries forward the destination's DACL and
// other metadata. Reparse points deliberately use MoveFileExW so the directory
// entry is replaced rather than opening or modifying the link target.
inline bool install_windows_temporary(const std::filesystem::path& temporary,
                                      const std::filesystem::path& destination, bool overwrite,
                                      DWORD* failure) {
    if (!overwrite) {
        if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != 0) {
            return true;
        }
        *failure = GetLastError();
        return false;
    }

    // Retry when a competing writer creates or removes the destination between
    // inspection and installation. The actual replacement operation remains
    // atomic; the bound prevents hostile directory churn from spinning forever.
    for (unsigned int attempt = 0; attempt < 8U; ++attempt) {
        const DWORD attributes = GetFileAttributesW(destination.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD inspect_error = GetLastError();
            if (!windows_path_missing(inspect_error)) {
                *failure = inspect_error;
                return false;
            }
            if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != 0) {
                return true;
            }
            const DWORD move_error = GetLastError();
            if (windows_path_appeared(move_error)) {
                continue;
            }
            *failure = move_error;
            return false;
        }

        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            if (MoveFileExW(temporary.c_str(), destination.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
                return true;
            }
            const DWORD move_error = GetLastError();
            if (windows_path_missing(move_error)) {
                continue;
            }
            *failure = move_error;
            return false;
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
            *failure = ERROR_ACCESS_DENIED;
            return false;
        }

        if (ReplaceFileW(destination.c_str(), temporary.c_str(), nullptr, 0U, nullptr, nullptr) !=
            0) {
            return true;
        }
        const DWORD replace_error = GetLastError();
        if (windows_path_missing(replace_error)) {
            continue;
        }
        *failure = replace_error;
        return false;
    }

    *failure = ERROR_RETRY;
    return false;
}

} // namespace detail
} // namespace pvt

#endif

#endif
