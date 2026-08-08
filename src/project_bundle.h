#ifndef PVT_PROJECT_BUNDLE_H
#define PVT_PROJECT_BUNDLE_H

#include "procedural_visualizer_tool.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pvt {

constexpr std::uint32_t kProjectBundleFormatVersion = 1;

struct BundleVersionInfo {
    std::uint64_t number = 0;
    std::string uuid;
    std::string parent_digest;
    std::string reason;
    std::string saved_utc;
    std::string saved_with_version;
    std::string metadata_digest;
    std::size_t layer_count = 0;
    bool indexed = true;
    bool valid = true;
    bool externally_modified = false;
    // True when the exact version tree no longer matches the last observed
    // digest recorded by an explicit save.
    bool changed_since_recorded = false;
    std::string integrity_message;
};

struct BundleDiffEntry {
    std::string field;
    std::string before;
    std::string after;
};

struct BundleSaveReport {
    std::string path;
    std::uint64_t version = 0;
    bool created_version = false;
    bool validated_only = false;
    bool wrote_zip = false;
    bool promoted_external_change = false;
};

struct ProjectDocument {
    ProjectConfig project;
    std::string source_path;
    std::string imported_from_path;
    std::string bundle_root_name;
    std::string first_created_utc;
    std::string last_opened_utc;
    std::string last_saved_utc;
    std::string created_with_version;
    std::string last_changed_with_version;
    std::string loaded_snapshot_digest;
    // Digest of the exact on-disk bundle state seen at load/save time. It is
    // used to reject stale writes when another process advances or edits the
    // bundle while this document remains open.
    std::string loaded_bundle_state_digest;
    std::uint64_t current_version = 0;
    std::vector<BundleVersionInfo> versions;
    bool source_is_zip = false;
    bool legacy_import = false;
    bool dirty = true;
    bool externally_modified = false;
    bool newer_program_version = false;
};

ProjectDocument default_project_document();

// Legacy imports never associate the returned document with the .pvt path, so
// a later project save cannot overwrite the imported setup.
bool import_legacy_setup(const std::string& path,
                         ProjectDocument& destination,
                         std::string* error = nullptr);

// Accepts a ZIP archive or an unpacked bundle directory. Load is transactional
// and read-only; checksum-valid current is tried first, then numeric versions
// from highest to lowest. Valid externally edited data is loaded dirty.
bool load_project_document(const std::string& path,
                           ProjectDocument& destination,
                           std::string* error = nullptr);

bool load_project_version(const ProjectDocument& document,
                          std::uint64_t version,
                          ProjectConfig& destination,
                          std::string* error = nullptr);

bool diff_project_versions(const ProjectDocument& document,
                           std::uint64_t before,
                           std::uint64_t after,
                           std::vector<BundleDiffEntry>& destination,
                           std::string* error = nullptr);

// Explicitly changes the bundle's current pointer without altering an immutable
// version. This may rewrite an outer ZIP atomically.
bool make_project_version_current(ProjectDocument& document,
                                  std::uint64_t version,
                                  BundleSaveReport* report = nullptr,
                                  std::string* error = nullptr);

// Creates a new highest-numbered version copied from the selected snapshot.
bool revert_project_as_new(ProjectDocument& document,
                           std::uint64_t version,
                           BundleSaveReport* report = nullptr,
                           std::string* error = nullptr);

// Save and Save-As share this operation. A clean document performs full bundle
// validation and does not add a version. Changed or externally edited data is
// appended as a new immutable version.
bool save_project_document(ProjectDocument& document,
                           const std::string& path,
                           BundleSaveReport* report = nullptr,
                           std::string* error = nullptr);

bool validate_project_bundle(const std::string& path,
                             std::vector<BundleVersionInfo>* versions = nullptr,
                             std::string* error = nullptr);

std::string portable_project_filename(const std::string& project_name);

} // namespace pvt

#endif
