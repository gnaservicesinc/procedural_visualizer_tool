#ifndef PVT_BUNDLE_ARCHIVE_H
#define PVT_BUNDLE_ARCHIVE_H

#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace pvt::detail {

struct BundleFileSet {
    std::string root_name;
    std::map<std::string, std::string> files;
    // In-memory commit metadata, never serialized or included in state
    // digests. Project-format migrations and the explicitly mutable current
    // snapshot in partial-history mode may atomically add or replace only
    // these narrowly recognized files inside existing version directories.
    std::set<std::string> transactional_updates;
    // Verified redundant content-addressed assets and obsolete full/delta
    // forms of human-readable version payloads may be removed after their
    // replacement and root controls are durable.
    std::set<std::string> transactional_removals;
    // Immutable version directories deliberately retired by the persisted
    // revision policy. They are removed only after the new root metadata and
    // current pointer are durable.
    std::set<std::uint64_t> retired_versions;
    // ZIP deflate level, 0..9. This write policy is intentionally excluded
    // from the semantic file-set digest.
    int zip_compression_level = 6;
    bool force_zip_recompression = false;
    // POSIX human-editable directory bundles expose current as a relative
    // symlink to the numeric version directory. Windows uses the checked text
    // record, which remains present in memory for checksums and ZIP portability.
    bool current_as_relative_symlink = false;
    std::uint64_t current_symlink_version = 0U;
    bool from_zip = false;
};

bool read_bundle_file_set(const std::string& path,
                          BundleFileSet& destination,
                          std::string* error);

// Writes either a ZIP (path ending in .zip, case-insensitive) or an unpacked
// directory. ZIP replacement and root metadata files are installed atomically;
// an unpacked new version directory is committed before the current pointer.
bool write_bundle_file_set(const std::string& path,
                           const BundleFileSet& files,
                           std::string* error);

// Serializes writers with a transient sibling advisory lock, removes its exact
// file identity after a completed transaction, and performs the expected state
// check while that lock is held. `destination_existed == false` is a
// compare-and-create operation; otherwise `expected_state_digest` must match
// the complete file-set digest currently on disk. This is the application save
// path. Unchanged entries from an existing validated ZIP are copied in their
// compressed form before the complete temporary archive is read back and
// compared. The unchecked overload above remains useful for isolated tests.
bool write_bundle_file_set_if_unchanged(
    const std::string& path,
    const BundleFileSet& files,
    bool destination_existed,
    const std::string& expected_state_digest,
    std::string* error);

bool path_is_zip_bundle(const std::string& path);
bool sha256_hex(const std::string& bytes,
                std::string& destination,
                std::string* error);
bool bundle_file_set_digest(const BundleFileSet& files,
                            std::string& destination,
                            std::string* error);
bool valid_utf8(const std::string& text);

} // namespace pvt::detail

#endif
