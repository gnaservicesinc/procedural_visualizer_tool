#include "bundle_archive.h"

#include "mz.h"
#include "mz_os.h"
#include "mz_strm.h"
#include "mz_zip.h"
#include "mz_zip_rw.h"

#include "path_utf8.h"
#if defined(_WIN32)
#  include "windows_file_install.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/file.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace pvt::detail {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaximumBundleEntries = 32768U;
constexpr std::size_t kMaximumBundleBytes =
    std::size_t{1024} * 1024U * 1024U;
// Layer setup entries can contain the bounded 8192-sample rich music cache.
// Project metadata itself retains its stricter 4 MiB parser limit.
constexpr std::size_t kMaximumMetadataFileBytes = 8U * 1024U * 1024U;
constexpr std::size_t kMaximumAssetFileBytes =
    std::size_t{512} * 1024U * 1024U;
constexpr std::size_t kMaximumArchivePathBytes = 4096U;
constexpr std::uint64_t kMaximumCompressionRatio = 1000U;

bool path_is_reparse_point(const fs::path& path) {
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
           && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
#else
    (void)path;
    return false;
#endif
}

class Sha256 {
public:
    Sha256()
        : state_{{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                  0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U}} {}

    bool update(const void* data, std::size_t size) {
        if (size > (std::numeric_limits<std::uint64_t>::max)() - total_bytes_) {
            return false;
        }
        const auto* input = static_cast<const unsigned char*>(data);
        total_bytes_ += static_cast<std::uint64_t>(size);
        while (size != 0U) {
            const std::size_t count = (std::min)(size, block_.size() - block_size_);
            std::copy_n(input, count, block_.begin()
                                      + static_cast<std::ptrdiff_t>(block_size_));
            block_size_ += count;
            input += count;
            size -= count;
            if (block_size_ == block_.size()) {
                transform(block_.data());
                block_size_ = 0U;
            }
        }
        return true;
    }

    std::array<unsigned char, 32U> finish() {
        const std::uint64_t bit_length = total_bytes_ * 8U;
        const unsigned char marker = 0x80U;
        (void)update(&marker, 1U);
        const unsigned char zero = 0U;
        while (block_size_ != 56U) {
            (void)update(&zero, 1U);
        }
        std::array<unsigned char, 8U> length{};
        for (std::size_t index = 0U; index < length.size(); ++index) {
            length[length.size() - 1U - index] =
                static_cast<unsigned char>(bit_length >> (index * 8U));
        }
        (void)update(length.data(), length.size());

        std::array<unsigned char, 32U> digest{};
        for (std::size_t word = 0U; word < state_.size(); ++word) {
            for (std::size_t byte = 0U; byte < 4U; ++byte) {
                digest[word * 4U + byte] = static_cast<unsigned char>(
                    state_[word] >> ((3U - byte) * 8U));
            }
        }
        return digest;
    }

private:
    static std::uint32_t rotate_right(std::uint32_t value, unsigned count) {
        return (value >> count) | (value << (32U - count));
    }

    void transform(const unsigned char* block) {
        static constexpr std::array<std::uint32_t, 64U> constants{{
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
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U}};
        std::array<std::uint32_t, 64U> words{};
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
            const std::uint32_t sigma0 = rotate_right(first, 7U)
                                         ^ rotate_right(first, 18U) ^ (first >> 3U);
            const std::uint32_t sigma1 = rotate_right(second, 17U)
                                         ^ rotate_right(second, 19U) ^ (second >> 10U);
            words[index] = words[index - 16U] + sigma0 + words[index - 7U] + sigma1;
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
            const std::uint32_t sum1 = rotate_right(e, 6U) ^ rotate_right(e, 11U)
                                       ^ rotate_right(e, 25U);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temporary1 = h + sum1 + choose
                                             + constants[index] + words[index];
            const std::uint32_t sum0 = rotate_right(a, 2U) ^ rotate_right(a, 13U)
                                       ^ rotate_right(a, 22U);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = sum0 + majority;
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

    std::array<std::uint32_t, 8U> state_{};
    std::array<unsigned char, 64U> block_{};
    std::size_t block_size_ = 0U;
    std::uint64_t total_bytes_ = 0U;
};

std::string hex_digest(const std::array<unsigned char, 32U>& digest) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string destination(digest.size() * 2U, '0');
    for (std::size_t index = 0U; index < digest.size(); ++index) {
        destination[index * 2U] = digits[digest[index] >> 4U];
        destination[index * 2U + 1U] = digits[digest[index] & 0x0fU];
    }
    return destination;
}

bool sha_update_u64(Sha256& hash, std::uint64_t value) {
    std::array<unsigned char, 8U> encoded{};
    for (std::size_t index = 0U; index < encoded.size(); ++index) {
        encoded[encoded.size() - 1U - index] =
            static_cast<unsigned char>(value >> (index * 8U));
    }
    return hash.update(encoded.data(), encoded.size());
}

bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

class BundleWriteLock final {
public:
    BundleWriteLock() = default;
    BundleWriteLock(const BundleWriteLock&) = delete;
    BundleWriteLock& operator=(const BundleWriteLock&) = delete;

    ~BundleWriteLock() {
#if defined(_WIN32)
        if (handle_ != INVALID_HANDLE_VALUE) {
            (void)UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped_);
            (void)CloseHandle(handle_);
        }
#else
        if (descriptor_ >= 0) {
            (void)::flock(descriptor_, LOCK_UN);
            (void)::close(descriptor_);
        }
#endif
    }

    bool acquire(const fs::path& destination, std::string* error) {
        std::error_code path_error;
        const fs::path absolute =
            fs::absolute(destination, path_error).lexically_normal();
        if (path_error || absolute.empty() || absolute.filename().empty()) {
            return fail(error, "Could not resolve the project save-lock path.");
        }
        const fs::path lock_path = absolute.parent_path()
            / path_from_utf8("." + path_to_utf8(absolute.filename())
                             + ".pvt-save.lock");
#if defined(_WIN32)
        const DWORD existing_attributes = GetFileAttributesW(lock_path.c_str());
        if (existing_attributes != INVALID_FILE_ATTRIBUTES
            && (existing_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            return fail(error, "Project save lock is a reparse point; refusing save.");
        }
        handle_ = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            return fail(error, "Could not open the project save lock (Windows error "
                                   + std::to_string(GetLastError()) + ").");
        }
        BY_HANDLE_FILE_INFORMATION information{};
        if (GetFileInformationByHandle(handle_, &information) == 0
            || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            const DWORD failure = GetLastError();
            (void)CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            return fail(error, "Project save lock failed validation (Windows error "
                                   + std::to_string(failure) + ").");
        }
        if (LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                       0, MAXDWORD, MAXDWORD, &overlapped_) == 0) {
            const DWORD failure = GetLastError();
            (void)CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            return fail(error, "Another process is already saving this project "
                                   "(Windows error " + std::to_string(failure) + ").");
        }
#else
        int flags = O_RDWR | O_CREAT | O_NONBLOCK;
#  if defined(O_CLOEXEC)
        flags |= O_CLOEXEC;
#  endif
#  if defined(O_NOFOLLOW)
        flags |= O_NOFOLLOW;
#  endif
        descriptor_ = ::open(lock_path.c_str(), flags, 0600);
        if (descriptor_ < 0) {
            return fail(error, "Could not open the project save lock: "
                                   + std::generic_category().message(errno) + ".");
        }
        struct stat information{};
        if (::fstat(descriptor_, &information) != 0) {
            const int failure = errno;
            (void)::close(descriptor_);
            descriptor_ = -1;
            return fail(error, "Could not inspect the project save lock: "
                                   + std::generic_category().message(failure) + ".");
        }
        if (!S_ISREG(information.st_mode)) {
            (void)::close(descriptor_);
            descriptor_ = -1;
            return fail(error, "Project save lock is not a regular file.");
        }
        if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            const int failure = errno;
            (void)::close(descriptor_);
            descriptor_ = -1;
            return fail(error, "Another process is already saving this project: "
                                   + std::generic_category().message(failure) + ".");
        }
#endif
        return true;
    }

private:
#if defined(_WIN32)
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped_{};
#else
    int descriptor_ = -1;
#endif
};

std::string lower_ascii(std::string value) {
    for (char& character : value) {
        const unsigned char raw = static_cast<unsigned char>(character);
        if (raw >= 'A' && raw <= 'Z') {
            character = static_cast<char>(raw - 'A' + 'a');
        }
    }
    return value;
}

bool safe_archive_path(const std::string& path) {
    if (path.empty() || path.size() > kMaximumArchivePathBytes
        || !valid_utf8(path) || path.front() == '/' || path.front() == '\\'
        || path.find('\\') != std::string::npos
        || path.find('\0') != std::string::npos) {
        return false;
    }
    std::size_t start = 0U;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end = slash == std::string::npos ? path.size() : slash;
        const std::string_view component(path.data() + start, end - start);
        if (component.empty() || component == "." || component == ".."
            || component.find(':') != std::string_view::npos) {
            return false;
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1U;
        if (start == path.size()) {
            break; // A single trailing slash is a directory marker.
        }
    }
    return true;
}

bool lowercase_sha256_component(std::string_view value) {
    return value.size() == 64U
           && std::all_of(value.begin(), value.end(), [](char character) {
                  return (character >= '0' && character <= '9')
                         || (character >= 'a' && character <= 'f');
              });
}

bool asset_entry_path(std::string_view path) {
    constexpr std::string_view prefix = "assets/";
    if (path.substr(0U, prefix.size()) != prefix) return false;
    const std::string_view relative = path.substr(prefix.size());
    if (lowercase_sha256_component(relative)) return true; // legacy v2
    const std::size_t slash = relative.find('/');
    return slash == 64U
           && lowercase_sha256_component(relative.substr(0U, slash))
           && slash + 1U < relative.size()
           && relative.find('/', slash + 1U) == std::string_view::npos;
}

std::size_t entry_size_limit(std::string_view path) {
    return asset_entry_path(path) ? kMaximumAssetFileBytes
                                  : kMaximumMetadataFileBytes;
}

bool validate_file_set(const BundleFileSet& files, std::string* error) {
    if (files.root_name.empty() || files.root_name.find('/') != std::string::npos
        || !safe_archive_path(files.root_name)) {
        return fail(error, "Bundle root name is invalid.");
    }
    if (files.files.empty() || files.files.size() > kMaximumBundleEntries) {
        return fail(error, "Bundle has no files or exceeds the 32768-entry limit.");
    }
    std::set<std::string> folded_paths;
    std::size_t total_bytes = 0U;
    for (const auto& entry : files.files) {
        if (entry.first.empty() || entry.first.back() == '/'
            || !safe_archive_path(entry.first)
            || files.root_name.size() + 1U + entry.first.size()
                   > kMaximumArchivePathBytes
            || !folded_paths.insert(lower_ascii(entry.first)).second) {
            return fail(error, "Bundle contains an unsafe or case-colliding path.");
        }
        if (entry.second.size() > entry_size_limit(entry.first)) {
            return fail(error,
                        asset_entry_path(entry.first)
                            ? "Bundle asset exceeds the 512 MiB file limit."
                            : "Bundle non-asset entry exceeds the 8 MiB file limit.");
        }
        if (total_bytes > kMaximumBundleBytes - entry.second.size()) {
            return fail(error, "Bundle exceeds the 1 GiB expanded-size limit.");
        }
        total_bytes += entry.second.size();
    }
    return true;
}

std::string unique_suffix() {
    static std::atomic<std::uint64_t> counter{0U};
    const std::uint64_t sequence = counter.fetch_add(1U, std::memory_order_relaxed);
    std::uint64_t entropy = 0U;
    try {
        std::random_device random;
        entropy = (static_cast<std::uint64_t>(random()) << 32U)
                  ^ static_cast<std::uint64_t>(random());
    } catch (...) {
        // A counter plus a high-resolution timestamp remains collision-resistant
        // within this process when an OS entropy provider is unavailable.
        entropy = static_cast<std::uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
    }
    const std::uint64_t value = entropy ^ sequence;
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

bool read_regular_file(const fs::path& path,
                       std::size_t maximum_bytes,
                       std::string& bytes,
                       std::string* error) {
    std::error_code status_error;
    const fs::file_status status = fs::symlink_status(path, status_error);
    if (status_error || fs::is_symlink(status) || path_is_reparse_point(path)
        || !fs::is_regular_file(status)) {
        return fail(error, "Bundle entry is not a regular file: '"
                               + path_to_utf8(path) + "'.");
    }
    const std::uintmax_t size = fs::file_size(path, status_error);
    if (status_error || size > maximum_bytes) {
        return fail(error, "Bundle entry is unreadable or exceeds its size limit: '"
                               + path_to_utf8(path) + "'.");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return fail(error, "Could not open bundle entry for reading: '"
                               + path_to_utf8(path) + "'.");
    }
    bytes.assign(static_cast<std::size_t>(size), '\0');
    if (!bytes.empty()) {
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    if (!input || input.peek() != std::char_traits<char>::eof()) {
        return fail(error, "I/O error while reading bundle entry: '"
                               + path_to_utf8(path) + "'.");
    }
    return true;
}

#if defined(_WIN32)
bool flush_path(const fs::path& path) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const bool ok = FlushFileBuffers(handle) != 0;
    (void)CloseHandle(handle);
    return ok;
}

bool replace_file(const fs::path& temporary, const fs::path& destination,
                  std::string* error) {
    DWORD failure = ERROR_SUCCESS;
    if (!install_windows_temporary(temporary, destination, true, &failure)) {
        return fail(error, "Could not atomically install bundle file (Windows error "
                               + std::to_string(failure) + ").");
    }
    return true;
}
#else
bool flush_path(const fs::path& path) {
    const std::string native = path.string();
    const int descriptor = ::open(native.c_str(), O_RDONLY);
    if (descriptor < 0) {
        return false;
    }
    int result = 0;
    do {
        result = ::fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    const bool ok = result == 0;
    (void)::close(descriptor);
    return ok;
}

bool replace_file(const fs::path& temporary, const fs::path& destination,
                  std::string* error) {
    if (::rename(temporary.string().c_str(), destination.string().c_str()) != 0) {
        return fail(error, "Could not atomically install bundle file: "
                               + std::generic_category().message(errno) + ".");
    }
    return true;
}
#endif

bool write_atomic_file(const fs::path& destination,
                       const std::string& bytes,
                       std::string* error) {
    fs::path directory = destination.parent_path();
    if (directory.empty()) {
        directory = fs::path(".");
    }
    const fs::path temporary =
        directory / (".pvt-bundle-file-" + unique_suffix() + ".tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return fail(error, "Could not create temporary bundle file.");
        }
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return fail(error, "Could not write temporary bundle file.");
        }
    }
    if (!flush_path(temporary)) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return fail(error, "Could not flush temporary bundle file.");
    }
    if (!replace_file(temporary, destination, error)) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    (void)flush_path(directory);
    return true;
}

bool write_new_directory_tree(const fs::path& destination,
                              const BundleFileSet& files,
                              std::string* error) {
    const fs::path parent = destination.parent_path().empty()
                                ? fs::path(".") : destination.parent_path();
    const fs::path staging =
        parent / (".pvt-bundle-dir-" + unique_suffix() + ".tmp");
    std::error_code filesystem_error;
    if (!fs::create_directory(staging, filesystem_error) || filesystem_error) {
        return fail(error, "Could not create temporary bundle directory.");
    }
    bool success = true;
    for (const auto& entry : files.files) {
        const fs::path target = staging / path_from_utf8(entry.first);
        fs::create_directories(target.parent_path(), filesystem_error);
        if (filesystem_error) {
            success = false;
            break;
        }
        std::ofstream output(target, std::ios::binary | std::ios::trunc);
        output.write(entry.second.data(),
                     static_cast<std::streamsize>(entry.second.size()));
        output.flush();
        if (!output || !flush_path(target)) {
            success = false;
            break;
        }
    }
    if (!success || !flush_path(staging)) {
        fs::remove_all(staging, filesystem_error);
        return fail(error, "Could not complete temporary bundle directory.");
    }
    fs::rename(staging, destination, filesystem_error);
    if (filesystem_error) {
        fs::remove_all(staging, filesystem_error);
        return fail(error, "Could not atomically install new bundle directory.");
    }
    (void)flush_path(parent);
    return true;
}

bool top_level_numeric(std::string_view path, std::string& number) {
    const std::size_t slash = path.find('/');
    const std::string_view component = path.substr(0U, slash);
    if (component.empty()) {
        return false;
    }
    for (const char character : component) {
        if (character < '0' || character > '9') {
            return false;
        }
    }
    number.assign(component);
    return true;
}

bool update_existing_directory(const fs::path& destination,
                               const BundleFileSet& desired,
                               std::string* error) {
    BundleFileSet existing;
    if (!read_bundle_file_set(path_to_utf8(destination), existing, error)) {
        return false;
    }

    std::set<std::string> new_versions;
    std::vector<const std::pair<const std::string, std::string>*> new_assets;
    for (const auto& entry : desired.files) {
        const auto old = existing.files.find(entry.first);
        if (old != existing.files.end()) {
            if (old->second != entry.second
                && entry.first != "metadata.txt"
                && entry.first != "metadata.sha256"
                && entry.first != "current") {
                return fail(error, "Save would overwrite immutable bundle entry '"
                                       + entry.first + "'.");
            }
            continue;
        }
        if (entry.first == "metadata.txt" || entry.first == "metadata.sha256"
            || entry.first == "current") {
            continue; // Repair a missing root control file atomically below.
        }
        if (asset_entry_path(entry.first)) {
            new_assets.push_back(&entry);
            continue;
        }
        std::string version;
        if (!top_level_numeric(entry.first, version)) {
            return fail(error, "Save introduced an unexpected root entry '"
                                   + entry.first + "'.");
        }
        new_versions.insert(version);
    }
    if (new_versions.size() > 1U) {
        return fail(error, "A save may commit at most one new version directory.");
    }

    // Assets use collision-safe digest directories with the original readable
    // filename. Install them before the new version so a crash can leave only
    // harmless unreferenced bytes, never a committed manifest whose dependency
    // is absent.
    if (!new_assets.empty()) {
        const fs::path assets_directory = destination / "assets";
        std::error_code filesystem_error;
        const fs::file_status assets_status =
            fs::symlink_status(assets_directory, filesystem_error);
        if (filesystem_error) {
            filesystem_error.clear();
            if (!fs::create_directory(assets_directory, filesystem_error)
                || filesystem_error) {
                return fail(error, "Could not create bundle asset directory.");
            }
        } else if (!fs::is_directory(assets_status)
                   || fs::is_symlink(assets_status)) {
            return fail(error, "Bundle asset path is not a regular directory.");
        }
        for (const auto* entry : new_assets) {
            const fs::path target =
                destination / path_from_utf8(entry->first);
            const fs::path target_parent = target.parent_path();
            if (target_parent != assets_directory) {
                const fs::file_status parent_status =
                    fs::symlink_status(target_parent, filesystem_error);
                if (filesystem_error) {
                    filesystem_error.clear();
                    if (!fs::create_directory(target_parent, filesystem_error)
                        || filesystem_error) {
                        return fail(error,
                                    "Could not create readable bundle asset identity directory.");
                    }
                } else if (!fs::is_directory(parent_status)
                           || fs::is_symlink(parent_status)) {
                    return fail(error,
                                "Bundle asset identity path is not a regular directory.");
                }
            }
            const fs::file_status target_status =
                fs::symlink_status(target, filesystem_error);
            if (!filesystem_error && fs::exists(target_status)) {
                return fail(error, "Bundle asset appeared during save; refusing overwrite.");
            }
            filesystem_error.clear();
            if (!write_atomic_file(target, entry->second, error)) {
                return false;
            }
            (void)flush_path(target_parent);
        }
        (void)flush_path(assets_directory);
    }

    if (!new_versions.empty()) {
        const std::string version = *new_versions.begin();
        const fs::path final_version = destination / path_from_utf8(version);
        const fs::path parent = destination.parent_path().empty()
                                    ? fs::path(".") : destination.parent_path();
        const fs::path staging =
            parent / path_from_utf8(".pvt-version-" + version + "-"
                                    + unique_suffix() + ".tmp");
        std::error_code filesystem_error;
        if (!fs::create_directory(staging, filesystem_error) || filesystem_error) {
            return fail(error, "Could not create temporary version directory.");
        }
        bool success = true;
        const std::string prefix = version + "/";
        for (const auto& entry : desired.files) {
            if (entry.first.compare(0U, prefix.size(), prefix) != 0) {
                continue;
            }
            const fs::path target = staging / path_from_utf8(entry.first.substr(prefix.size()));
            std::ofstream output(target, std::ios::binary | std::ios::trunc);
            output.write(entry.second.data(),
                         static_cast<std::streamsize>(entry.second.size()));
            output.flush();
            if (!output || !flush_path(target)) {
                success = false;
                break;
            }
        }
        if (!success || !flush_path(staging)) {
            fs::remove_all(staging, filesystem_error);
            return fail(error, "Could not complete new version directory.");
        }
        fs::rename(staging, final_version, filesystem_error);
        if (filesystem_error) {
            fs::remove_all(staging, filesystem_error);
            return fail(error, "Could not atomically commit new version directory.");
        }
        (void)flush_path(destination);
    }

    for (const std::string_view root_file :
         {std::string_view("metadata.txt"), std::string_view("metadata.sha256"),
          std::string_view("current")}) {
        const auto found = desired.files.find(std::string(root_file));
        if (found == desired.files.end()) {
            return fail(error, "Bundle save is missing required root metadata.");
        }
        if (!write_atomic_file(destination / path_from_utf8(found->first),
                               found->second, error)) {
            return false;
        }
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
            if (open) {
                (void)mz_zip_writer_close(handle);
            }
            mz_zip_writer_delete(&handle);
        }
    }
};

bool read_zip(const std::string& path,
              BundleFileSet& destination,
              std::string* error) {
    ZipReaderGuard reader;
    if (reader.handle == nullptr) {
        return fail(error, "Could not allocate ZIP reader.");
    }
    if (mz_zip_reader_open_file(reader.handle, path.c_str()) != MZ_OK) {
        return fail(error, "Could not open project ZIP.");
    }
    void* zip_handle = nullptr;
    uint32_t central_disk = 0U;
    if (mz_zip_reader_get_zip_handle(reader.handle, &zip_handle) != MZ_OK
        || mz_zip_get_disk_number_with_cd(zip_handle, &central_disk) != MZ_OK
        || central_disk != 0U) {
        return fail(error, "Multi-disk ZIP archives are not supported.");
    }

    BundleFileSet candidate;
    candidate.from_zip = true;
    std::set<std::string> folded_paths;
    std::size_t total_bytes = 0U;
    std::size_t entries = 0U;
    int32_t result = mz_zip_reader_goto_first_entry(reader.handle);
    if (result != MZ_OK) {
        return fail(error, "Project ZIP is empty or has no readable central directory.");
    }
    while (result == MZ_OK) {
        mz_zip_file* info = nullptr;
        if (mz_zip_reader_entry_get_info(reader.handle, &info) != MZ_OK
            || info == nullptr || info->filename == nullptr) {
            return fail(error, "Project ZIP has malformed entry metadata.");
        }
        if (++entries > kMaximumBundleEntries) {
            return fail(error, "Project ZIP exceeds the 32768-entry limit.");
        }
        const std::string archive_path(info->filename, info->filename_size);
        if (archive_path.find('\0') != std::string::npos
            || std::strlen(info->filename) != info->filename_size
            || !safe_archive_path(archive_path)) {
            return fail(error, "Project ZIP contains an unsafe entry path.");
        }
        if ((info->flag & MZ_ZIP_FLAG_ENCRYPTED) != 0U || info->aes_version != 0U) {
            return fail(error, "Encrypted ZIP entries are not supported.");
        }
        if (info->disk_number != 0U) {
            return fail(error, "Multi-disk ZIP entries are not supported.");
        }
        if (mz_zip_attrib_is_symlink(info->external_fa, info->version_madeby) == MZ_OK) {
            return fail(error, "Project ZIP contains a symbolic link.");
        }
        const bool is_directory =
            mz_zip_reader_entry_is_dir(reader.handle) == MZ_OK;
        uint32_t unix_attributes = 0U;
        if (!is_directory
            && mz_zip_attrib_convert(MZ_HOST_SYSTEM(info->version_madeby),
                                     info->external_fa, MZ_HOST_SYSTEM_UNIX,
                                     &unix_attributes) == MZ_OK) {
            const uint32_t type = unix_attributes & 0170000U;
            if (type != 0U && type != 0100000U) {
                return fail(error, "Project ZIP contains a special file entry.");
            }
        }
        if (!is_directory
            && info->compression_method != MZ_COMPRESS_METHOD_STORE
            && info->compression_method != MZ_COMPRESS_METHOD_DEFLATE) {
            return fail(error, "Project ZIP uses an unsupported compression method.");
        }

        std::string normalized = archive_path;
        if (is_directory && !normalized.empty() && normalized.back() == '/') {
            normalized.pop_back();
        }
        const std::size_t slash = normalized.find('/');
        const std::string root = slash == std::string::npos
                                     ? normalized : normalized.substr(0U, slash);
        if (candidate.root_name.empty()) {
            candidate.root_name = root;
        } else if (candidate.root_name != root) {
            return fail(error, "Project ZIP must contain exactly one root directory.");
        }
        if (!is_directory) {
            if (slash == std::string::npos || slash + 1U >= normalized.size()) {
                return fail(error, "Project ZIP file is outside its root directory.");
            }
            const std::string relative = normalized.substr(slash + 1U);
            const std::string folded = lower_ascii(relative);
            if (!folded_paths.insert(folded).second) {
                return fail(error, "Project ZIP contains duplicate or case-colliding paths.");
            }
            const std::size_t maximum_entry_bytes = entry_size_limit(relative);
            if (info->uncompressed_size < 0
                || static_cast<std::uint64_t>(info->uncompressed_size)
                       > maximum_entry_bytes) {
                return fail(error,
                            asset_entry_path(relative)
                                ? "Project ZIP asset exceeds the 512 MiB file limit."
                                : "Project ZIP non-asset entry exceeds the 8 MiB file limit.");
            }
            if (info->compressed_size <= 0 && info->uncompressed_size > 0) {
                return fail(error, "Project ZIP entry has an invalid compressed size.");
            }
            const std::uint64_t expanded =
                static_cast<std::uint64_t>(info->uncompressed_size);
            const std::uint64_t minimum_compressed =
                (expanded + kMaximumCompressionRatio - 1U)
                / kMaximumCompressionRatio;
            if (!asset_entry_path(relative) && info->compressed_size > 0
                && static_cast<std::uint64_t>(info->compressed_size)
                       < minimum_compressed) {
                return fail(error, "Project ZIP entry exceeds the compression-ratio limit.");
            }
            const std::size_t size = static_cast<std::size_t>(info->uncompressed_size);
            if (total_bytes > kMaximumBundleBytes - size) {
                return fail(error, "Project ZIP exceeds the 1 GiB expanded-size limit.");
            }
            std::string bytes(size, '\0');
            if (mz_zip_reader_entry_open(reader.handle) != MZ_OK) {
                return fail(error, "Could not open project ZIP entry.");
            }
            std::size_t offset = 0U;
            while (offset < bytes.size()) {
                const int32_t request = static_cast<int32_t>((std::min)(
                    bytes.size() - offset,
                    static_cast<std::size_t>((std::numeric_limits<int32_t>::max)())));
                const int32_t count = mz_zip_reader_entry_read(
                    reader.handle, bytes.data() + offset, request);
                if (count <= 0) {
                    (void)mz_zip_reader_entry_close(reader.handle);
                    return fail(error, "Project ZIP entry ended before its declared size.");
                }
                offset += static_cast<std::size_t>(count);
            }
            std::array<char, 1U> extra{};
            if (mz_zip_reader_entry_read(reader.handle, extra.data(), 1) != 0
                || mz_zip_reader_entry_close(reader.handle) != MZ_OK) {
                return fail(error, "Project ZIP entry failed CRC or size validation.");
            }
            candidate.files.emplace(relative, std::move(bytes));
            total_bytes += size;
        }
        result = mz_zip_reader_goto_next_entry(reader.handle);
    }
    if (result != MZ_END_OF_LIST) {
        return fail(error, "Could not enumerate the complete project ZIP.");
    }
    if (candidate.root_name.empty() || candidate.files.empty()) {
        return fail(error, "Project ZIP contains no bundle data.");
    }
    destination = std::move(candidate);
    return true;
}

bool write_zip(const fs::path& destination,
               const BundleFileSet& files,
               std::string* error) {
    fs::path directory = destination.parent_path();
    if (directory.empty()) {
        directory = fs::path(".");
    }
    const fs::path temporary =
        directory / (".pvt-bundle-archive-" + unique_suffix() + ".tmp");
    ZipWriterGuard writer;
    if (writer.handle == nullptr
        || mz_zip_writer_open_file(writer.handle, path_to_utf8(temporary).c_str(),
                                   0, 0) != MZ_OK) {
        return fail(error, "Could not create temporary project ZIP.");
    }
    writer.open = true;
    mz_zip_writer_set_compress_method(writer.handle, MZ_COMPRESS_METHOD_DEFLATE);
    mz_zip_writer_set_compress_level(writer.handle, MZ_COMPRESS_LEVEL_DEFAULT);
    for (const auto& entry : files.files) {
        const std::string archive_path = files.root_name + "/" + entry.first;
        mz_zip_file info{};
        info.version_madeby = MZ_VERSION_MADEBY;
        info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
        info.modified_date = std::time(nullptr);
        info.external_fa = 0100644U << 16U;
        info.filename = archive_path.c_str();
        info.filename_size = static_cast<std::uint16_t>(archive_path.size());
        if (mz_zip_writer_add_buffer(writer.handle, entry.second.data(),
                                     static_cast<int32_t>(entry.second.size()),
                                     &info) != MZ_OK) {
            (void)mz_zip_writer_close(writer.handle);
            writer.open = false;
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return fail(error, "Could not add an entry to temporary project ZIP.");
        }
    }
    if (mz_zip_writer_close(writer.handle) != MZ_OK) {
        writer.open = false;
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return fail(error, "Could not finalize temporary project ZIP.");
    }
    writer.open = false;
    if (!flush_path(temporary)) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return fail(error, "Could not flush temporary project ZIP.");
    }
    BundleFileSet verified;
    std::string verification_error;
    if (!read_zip(path_to_utf8(temporary), verified, &verification_error)
        || verified.root_name != files.root_name || verified.files != files.files) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return fail(error, "Temporary project ZIP failed verification before install: "
                               + verification_error);
    }
    if (!replace_file(temporary, destination, error)) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    (void)flush_path(directory);
    return true;
}

} // namespace

bool valid_utf8(const std::string& text) {
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
        if (index + continuation >= text.size()) {
            return false;
        }
        for (std::size_t offset = 1U; offset <= continuation; ++offset) {
            const unsigned char next = static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
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

bool path_is_zip_bundle(const std::string& path) {
    const std::string lowered = lower_ascii(path);
    return lowered.size() >= 4U
           && lowered.compare(lowered.size() - 4U, 4U, ".zip") == 0;
}

bool sha256_hex(const std::string& bytes,
                std::string& destination,
                std::string* error) {
    Sha256 hash;
    if (!hash.update(bytes.data(), bytes.size())) {
        return fail(error, "Input is too large for SHA-256 checksum.");
    }
    destination = hex_digest(hash.finish());
    return true;
}

bool bundle_file_set_digest(const BundleFileSet& files,
                            std::string& destination,
                            std::string* error) {
    Sha256 hash;
    static constexpr std::string_view marker = "PVT_BUNDLE_STATE\0";
    if (!hash.update(marker.data(), marker.size())
        || !sha_update_u64(hash, static_cast<std::uint64_t>(files.files.size()))) {
        return fail(error, "Bundle state is too large to checksum.");
    }
    for (const auto& entry : files.files) {
        if (!sha_update_u64(hash, static_cast<std::uint64_t>(entry.first.size()))
            || !hash.update(entry.first.data(), entry.first.size())
            || !sha_update_u64(hash, static_cast<std::uint64_t>(entry.second.size()))
            || !hash.update(entry.second.data(), entry.second.size())) {
            return fail(error, "Bundle state is too large to checksum.");
        }
    }
    destination = hex_digest(hash.finish());
    return true;
}

bool read_bundle_file_set(const std::string& path,
                          BundleFileSet& destination,
                          std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    try {
        if (path.empty() || path.find('\0') != std::string::npos
            || path.size() > kMaximumArchivePathBytes || !valid_utf8(path)) {
            return fail(error, "Bundle path is invalid or overlong.");
        }
        const fs::path native = path_from_utf8(path);
        std::error_code status_error;
        const fs::file_status status = fs::symlink_status(native, status_error);
        if (status_error || fs::is_symlink(status)
            || path_is_reparse_point(native)) {
            return fail(error, "Bundle source is missing or is a symbolic link.");
        }
        if (fs::is_regular_file(status)) {
            return read_zip(path, destination, error);
        }
        if (!fs::is_directory(status)) {
            return fail(error, "Bundle source is neither a ZIP nor a directory.");
        }

        BundleFileSet candidate;
        candidate.root_name = path_to_utf8(native.filename());
        if (candidate.root_name.empty()) {
            candidate.root_name = path_to_utf8(native.parent_path().filename());
        }
        std::set<std::string> folded_paths;
        std::size_t total_bytes = 0U;
        std::size_t entries = 0U;
        fs::recursive_directory_iterator iterator(native, status_error);
        const fs::recursive_directory_iterator end;
        while (!status_error && iterator != end) {
            const fs::path entry_path = iterator->path();
            const fs::file_status entry_status = fs::symlink_status(entry_path, status_error);
            if (status_error || fs::is_symlink(entry_status)
                || path_is_reparse_point(entry_path)) {
                return fail(error, "Unpacked bundle contains a symbolic link or unreadable entry.");
            }
            if (fs::is_directory(entry_status)) {
                ++iterator;
                continue;
            }
            if (!fs::is_regular_file(entry_status) || ++entries > kMaximumBundleEntries) {
                return fail(error, "Unpacked bundle contains a special file or too many entries.");
            }
            const fs::path relative_native = fs::relative(entry_path, native, status_error);
            if (status_error) {
                return fail(error, "Could not resolve unpacked bundle entry path.");
            }
            const std::string relative = path_to_generic_utf8(relative_native);
            if (!safe_archive_path(relative)
                || !folded_paths.insert(lower_ascii(relative)).second) {
                return fail(error, "Unpacked bundle contains an unsafe or colliding path.");
            }
            std::string bytes;
            if (!read_regular_file(entry_path, entry_size_limit(relative),
                                   bytes, error)) {
                return false;
            }
            if (total_bytes > kMaximumBundleBytes - bytes.size()) {
                return fail(error, "Unpacked bundle exceeds the 1 GiB size limit.");
            }
            total_bytes += bytes.size();
            candidate.files.emplace(relative, std::move(bytes));
            ++iterator;
        }
        if (status_error || candidate.files.empty()) {
            return fail(error, "Could not enumerate unpacked bundle or it is empty.");
        }
        destination = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to read project bundle.");
    } catch (const fs::filesystem_error& exception) {
        return fail(error, std::string("Filesystem error while reading bundle: ")
                               + exception.what());
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected error while reading bundle: ")
                               + exception.what());
    }
}

namespace {

bool write_bundle_file_set_impl(const std::string& path,
                                const BundleFileSet& files,
                                const std::optional<bool>& expected_existence,
                                const std::string& expected_state_digest,
                                std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    try {
        if (path.empty() || path.find('\0') != std::string::npos
            || path.size() > kMaximumArchivePathBytes || !valid_utf8(path)) {
            return fail(error, "Bundle destination or root name is invalid.");
        }
        if (!validate_file_set(files, error)) return false;
        const fs::path destination = path_from_utf8(path);
        BundleWriteLock write_lock;
        if (!write_lock.acquire(destination, error)) return false;

        std::error_code status_error;
        const fs::file_status status = fs::symlink_status(destination, status_error);
        const bool exists = !status_error && fs::exists(status);
        if (exists && fs::is_symlink(status)) {
            return fail(error, "Bundle destination is a symbolic link.");
        }
        if (expected_existence) {
            if (exists != *expected_existence) {
                return fail(error, exists
                    ? "Project destination appeared during save; refusing stale overwrite."
                    : "Project destination disappeared during save; refusing stale overwrite.");
            }
            if (exists) {
                if (expected_state_digest.size() != 64U
                    || !std::all_of(expected_state_digest.begin(),
                                    expected_state_digest.end(), [](char character) {
                                        return (character >= '0' && character <= '9')
                                               || (character >= 'a'
                                                   && character <= 'f');
                                    })) {
                    return fail(error, "Expected project state digest is invalid.");
                }
                BundleFileSet observed;
                std::string observed_digest;
                if (!read_bundle_file_set(path, observed, error)
                    || !bundle_file_set_digest(observed, observed_digest, error)) {
                    return false;
                }
                if (observed_digest != expected_state_digest) {
                    return fail(error,
                                "Project changed before commit; refusing stale overwrite.");
                }
            } else if (!expected_state_digest.empty()) {
                return fail(error,
                            "A new project save cannot have an expected state digest.");
            }
        }
        if (path_is_zip_bundle(path)) {
            if (exists && !fs::is_regular_file(status)) {
                return fail(error, "ZIP bundle destination is not a regular file.");
            }
            return write_zip(destination, files, error);
        }
        if (exists) {
            if (!fs::is_directory(status)) {
                return fail(error, "Unpacked bundle destination is not a directory.");
            }
            return update_existing_directory(destination, files, error);
        }
        return write_new_directory_tree(destination, files, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "Not enough memory to save project bundle.");
    } catch (const fs::filesystem_error& exception) {
        return fail(error, std::string("Filesystem error while saving bundle: ")
                               + exception.what());
    } catch (const std::exception& exception) {
        return fail(error, std::string("Unexpected error while saving bundle: ")
                               + exception.what());
    }
}

} // namespace

bool write_bundle_file_set(const std::string& path,
                           const BundleFileSet& files,
                           std::string* error) {
    return write_bundle_file_set_impl(path, files, std::nullopt, {}, error);
}

bool write_bundle_file_set_if_unchanged(
    const std::string& path,
    const BundleFileSet& files,
    bool destination_existed,
    const std::string& expected_state_digest,
    std::string* error) {
    return write_bundle_file_set_impl(path, files, destination_existed,
                                      expected_state_digest, error);
}

} // namespace pvt::detail
