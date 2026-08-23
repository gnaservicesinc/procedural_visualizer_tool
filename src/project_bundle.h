#ifndef PVT_PROJECT_BUNDLE_H
#define PVT_PROJECT_BUNDLE_H

#include "procedural_visualizer_tool.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pvt {

constexpr std::uint32_t kProjectBundleFormatVersion = 1;
constexpr std::size_t kMaximumProjectAttachmentBytes =
    kMaximumEmbeddedAssetBytes;
constexpr std::size_t kMaximumProjectBundleExpandedBytes =
    kMaximumUiItems;
constexpr std::size_t kMaximumProjectAttachmentReferences = kMaximumUiItems;
inline constexpr const char* kMusicSourceAttachmentId = "music.source";

struct ProjectAttachmentCache;

// One logical use of an embedded file. Bundle storage keeps one physical file
// beneath assets/<sha256>/ for each unique byte identity. Logical references
// retain their own user-facing filename and extension even when names alias the
// same physical object.
// local_path and bundle_path are managed runtime locations and are never
// serialized or included in semantic project digests.
struct ProjectAttachment {
    std::string reference_id;
    std::string sha256;
    std::string basename;
    std::uint64_t size_bytes = 0U;
    std::string local_path;
    std::string bundle_path;
    // A valid direct edit to a readable version-3-or-newer asset is accepted
    // just like replacing that source through the application. Saving promotes
    // it to a new immutable version with fresh identity metadata.
    bool externally_modified = false;
};

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
    bool compacted_storage = false;
    bool wrote_zip = false;
    bool promoted_external_change = false;
};

struct ProjectRecoveryInfo {
    std::size_t preserved_fields = 0U;
    std::size_t rejected_fields = 0U;
    std::vector<std::string> notes;
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
    // Runtime-only, non-security fingerprint of the loaded semantic project.
    // It lets ordinary no-op saves avoid rebuilding the large canonical text
    // representation while the persisted SHA-256 identities remain unchanged.
    std::string loaded_fast_project_fingerprint;
    // Digest of the exact on-disk bundle state seen at load/save time. It is
    // used to reject stale writes when another process advances or edits the
    // bundle while this document remains open.
    std::string loaded_bundle_state_digest;
    std::uint64_t current_version = 0;
    std::vector<BundleVersionInfo> versions;
    std::vector<ProjectAttachment> attachments;
    mutable std::shared_ptr<ProjectAttachmentCache> attachment_cache;
    bool source_is_zip = false;
    bool legacy_import = false;
    bool dirty = true;
    bool externally_modified = false;
    bool newer_program_version = false;
};

ProjectDocument default_project_document();

// Summarizes bounded compatibility data retained by the active snapshot.
// Program version strings alone are deliberately not treated as damage or a
// save risk; only actual repairs or unrecognized records appear here.
ProjectRecoveryInfo project_recovery_info(const ProjectConfig& project);

// Builds a detached, independent document from one current project snapshot.
// The project and every layer receive fresh UUIDs; bundle history, source
// association, and stale-write tokens are not copied. Preserved configuration
// fields are part of the snapshot and do follow it so Save Copy cannot lose
// future-version data. Saving the result creates a new version-0 bundle and
// cannot overwrite the source project through an inherited identity.
bool make_independent_project_copy(const ProjectConfig& project,
                                   ProjectDocument& destination,
                                   std::string* error = nullptr);

// Copies a document's current snapshot and embedded attachments while still
// assigning independent project/layer identities and discarding history.
bool make_independent_project_copy(const ProjectDocument& source,
                                   ProjectDocument& destination,
                                   std::string* error = nullptr);

// Registers a file immediately by content, copying it into a managed cache so
// moving/deleting the original before Save cannot break the project. Reusing
// the same bytes and basename reuses its readable bundle entry. Empty,
// non-portable, special, and symlink sources are rejected transactionally.
bool attach_project_file(ProjectDocument& document,
                         const std::string& reference_id,
                         const std::string& source_path,
                         ProjectAttachment* attached = nullptr,
                         std::string* error = nullptr);
bool detach_project_file(ProjectDocument& document,
                         const std::string& reference_id,
                         std::string* error = nullptr);
const ProjectAttachment* find_project_attachment(
    const ProjectDocument& document,
    const std::string& reference_id);
std::string project_attachment_path(const ProjectDocument& document,
                                    const std::string& reference_id);
std::string surface_obj_attachment_id(const std::string& layer_uuid);
std::string plane_displacement_attachment_id(const std::string& layer_uuid);
std::string environment_map_attachment_id(const std::string& layer_uuid);
std::string starting_image_attachment_id(const std::string& layer_uuid);
std::string layer_music_attachment_id(const std::string& layer_uuid);

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

// Save and Save-As share this operation. A clean document verifies recorded
// state and the current snapshot without decoding every immutable ancestor;
// validate_project_bundle remains the explicit full-history check. Changed or
// externally edited data is appended as a new immutable version.
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
