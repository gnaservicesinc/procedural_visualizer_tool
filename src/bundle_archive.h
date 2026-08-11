#ifndef PVT_BUNDLE_ARCHIVE_H
#define PVT_BUNDLE_ARCHIVE_H

#include <map>
#include <set>
#include <string>

namespace pvt::detail {

struct BundleFileSet {
    std::string root_name;
    std::map<std::string, std::string> files;
    // In-memory commit metadata, never serialized or included in state
    // digests. Project-format migrations may atomically add or replace only
    // these explicitly verified files inside existing immutable versions.
    std::set<std::string> transactional_updates;
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

// Serializes writers with a sibling advisory lock and performs the expected
// state check while that lock is held. `destination_existed == false` is a
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
