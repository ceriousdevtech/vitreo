#pragma once
/// @file msi_files.hpp
/// @brief Windows Installer files area: the File/Media model of a package,
/// source resolution, the file versioning rules, and the file actions
/// (InstallFiles, RemoveFiles, MoveFiles, DuplicateFiles,
/// RemoveDuplicateFiles, CreateFolders, RemoveFolders, PatchFiles,
/// BindImage, ResolveSource).
///
/// What other areas consume:
///   * features (FileCost / CostFinalize): `file_rows_of_component` for file
///     costs, and `key_file_supersedes_package` — Windows sets a component's
///     action to Null when its key file already on disk wins over the
///     package's copy (a higher version, or a versioned file over an
///     unversioned one; probe msi_files_probe: C_vdown, C_uv "Action: Null").
///   * engine (`[#file]`, `[!file]` formatting): `file_target_path`.
///   * productops (MsiGetFileHash): `compute_file_hash`.
///
/// Every action that changes the system schedules one ScriptOp per action
/// (with a rollback and a commit) through ActionContext::schedule, so the
/// work happens when InstallFinalize runs the script, as Windows does.
///
/// Contract sources: Microsoft Learn (File, Media, MsiFileHash, RemoveFile,
/// MoveFile, DuplicateFile, CreateFolder, BindImage, Patch tables; the
/// actions above; File Versioning Rules, Default File Versioning, Replacing
/// Existing Files, Companion Files, REINSTALLMODE, SHORTFILENAMES, SourceDir,
/// Word Count Summary, Cabinet data type, Windows Installer Error Messages)
/// and tools/oracle_probe/msi_files_probe (Windows 11, x86 and x64).
/// Contract notes and gaps: .docs/contracts/msi/files.md.

#include <vitreo/common.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vitreo::capsule { class Vfs; }

namespace vitreo::winapi::msi {

class Session;
class ActionContext;

// msidbFileAttributes (MsiDefs.h).
enum : u32 {
    kFileReadOnly = 0x000001,
    kFileHidden = 0x000002,
    kFileSystem = 0x000004,
    kFileVital = 0x000200,
    kFileChecksum = 0x000400,
    kFilePatchAdded = 0x001000,
    kFileNoncompressed = 0x002000,
    kFileCompressed = 0x004000,
};

/// Word Count summary property (PID 15) bits for an installation package.
struct SourceMode {
    bool short_names{false};  ///< bit 0: the source tree uses short names
    bool compressed{false};   ///< bit 1: files are in cabinets by default
    bool admin_image{false};  ///< bit 2: administrative image
    bool no_elevation{false}; ///< bit 3: elevated privileges not required
};
/// The package's Word Count (0 when the summary stream lacks it, as Learn
/// specifies).
[[nodiscard]] SourceMode source_mode(Session& session);

/// One File-table row.
struct FileRow {
    std::string key;
    std::string component;
    std::string short_name;  ///< short half of `short|long` (or the only name)
    std::string long_name;   ///< long half (or the only name)
    u64 size{0};
    std::string version;     ///< Version column (a version, a companion parent key, or empty)
    std::string language;    ///< Language column (comma-separated LANGIDs)
    u32 attributes{0};
    u32 sequence{0};
};

/// File rows in File-table order. Cached per session; reloaded when the
/// File table's row count changes (temporary rows a custom action added).
[[nodiscard]] const std::vector<FileRow>& file_rows(Session& session);
[[nodiscard]] const FileRow* find_file_row(Session& session, const std::string& key);
[[nodiscard]] std::vector<const FileRow*> file_rows_of_component(Session& session,
                                                                 const std::string& component);

/// The name a file is installed under: the short half when SHORTFILENAMES is
/// set, else the long half (Learn: SHORTFILENAMES).
[[nodiscard]] std::string target_file_name(Session& session, const FileRow& row);
/// Full installed path of a File key ([#file]): the component's directory
/// plus target_file_name. Empty when the file or its directory is unknown.
[[nodiscard]] std::string file_target_path(Session& session, const std::string& key);

/// Install the `[#file]` / `[!file]` resolver (Session::item_states().file_path)
/// when none is set: the installed path of a File key whose component is
/// installed locally ([!file] is the same path: the capsule volumes have no
/// 8.3 names, so GetShortPathName returns the long path, as Windows does on a
/// volume with short-name generation disabled). Idempotent; the features
/// area's CostInitialize should call it, and every file action does.
void install_file_state_resolvers(Session& session);

// ---- versioning ----------------------------------------------------------------

/// "a.b.c.d" (missing parts 0, each part <= 65535) as MS:LS, or nullopt when
/// the text is not a version (then it may be a companion-file parent key).
[[nodiscard]] std::optional<u64> parse_file_version(const std::string& text);

/// MsiGetFileHash: the MD5 of the bytes as four little-endian DWORDs.
[[nodiscard]] std::array<u32, 4> compute_file_hash(std::span<const u8> bytes);

/// What is known about one side of a versioning comparison.
struct VersionedFile {
    std::optional<u64> version;
    std::vector<u16> languages;  ///< empty = no language
};
/// The file already at the target location.
struct ExistingFile {
    bool exists{false};
    VersionedFile info;
    u64 created{0};   ///< FILETIME
    u64 modified{0};  ///< FILETIME
    std::optional<std::array<u32, 4>> hash;  ///< filled on demand
    std::vector<u8> bytes;  ///< contents (read for the version resource)
};
/// REINSTALLMODE file letters (Learn: REINSTALLMODE; default "omus").
struct ReinstallMode {
    bool p{false}, o{false}, e{false}, d{false}, c{false}, a{false};
};
[[nodiscard]] ReinstallMode parse_reinstall_mode(const std::string& text);

/// Read version, languages and times of an existing file through the VFS.
[[nodiscard]] ExistingFile probe_existing_file(capsule::Vfs& vfs, const std::string& path);

/// The default file versioning rules for one non-companion file: true when
/// the package copy is to be installed over @p existing. @p package_hash is
/// the MsiFileHash row of an unversioned file (Learn: the table "can only be
/// used with unversioned files"). @p checksum_attribute is
/// msidbFileAttributesChecksum (REINSTALLMODE=c repairs only those).
[[nodiscard]] bool should_install_file(const VersionedFile& package, ExistingFile& existing,
                                       const std::optional<std::array<u32, 4>>& package_hash,
                                       bool checksum_attribute, const ReinstallMode& mode);

/// Companion files (Learn: Companion Files, File Versioning Rules): the
/// decision of a file whose Version column names @p parent. @p parent_package
/// and @p parent_existing describe the companion parent.
[[nodiscard]] bool should_install_companion(const VersionedFile& parent_package,
                                            ExistingFile& parent_existing,
                                            const ExistingFile& companion_existing,
                                            const ReinstallMode& mode);

/// True when the key file a component would install is beaten by the copy
/// already on disk, which makes Windows set the component's action Null:
/// the existing key file is versioned and the package's is unversioned, or
/// both are versioned and the existing version is higher (REINSTALLMODE
/// `a`/`d`/`e` relax this as for any file). For CostFinalize (features).
[[nodiscard]] bool key_file_supersedes_package(Session& session, capsule::Vfs& vfs,
                                               const std::string& component);

/// PE CheckSum of @p image as CheckSumMappedFile computes it (the field at
/// OptionalHeader.CheckSum is skipped), and whether the stored one matches.
/// nullopt when @p image is not a PE file.
[[nodiscard]] std::optional<bool> pe_checksum_valid(std::span<const u8> image);

/// MSI wildcard match (`*` any run, `?` exactly one character), case
/// insensitive, as RemoveFile.FileName and MoveFile.SourceName use it.
[[nodiscard]] bool wildcard_match(std::string_view pattern, std::string_view name);

// ---- actions (registered by register_file_actions) ----------------------------------
u32 action_install_files(ActionContext& ctx);
u32 action_remove_files(ActionContext& ctx);
u32 action_move_files(ActionContext& ctx);
u32 action_duplicate_files(ActionContext& ctx);
u32 action_remove_duplicate_files(ActionContext& ctx);
u32 action_create_folders(ActionContext& ctx);
u32 action_remove_folders(ActionContext& ctx);
u32 action_patch_files(ActionContext& ctx);
u32 action_bind_image(ActionContext& ctx);
u32 action_resolve_source(ActionContext& ctx);

}  // namespace vitreo::winapi::msi
