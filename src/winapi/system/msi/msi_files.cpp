// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_files.cpp
/// @brief Windows Installer engine area: file actions (install/remove/move/
/// duplicate/folders/patch/bind), media, cabinets and source resolution.
///
/// Every action here reads its tables when the sequencer runs it and
/// schedules ONE ScriptOp that does the work when InstallFinalize runs the
/// script; each op journals what it changed (created/replaced/removed files
/// and folders) so its rollback restores the machine and its commit drops
/// the backups. Replaced and removed files are moved into
/// `<drive>:\Config.Msi\` (the rollback store Windows uses) rather than held
/// in memory.
///
/// Message protocol (tools/oracle_probe/msi_files_probe*.expected, measured
/// on Windows 11, identical for x86 and x64 clients):
///   * InstallFiles / MoveFiles / DuplicateFiles: Progress{1,1,0,0} once,
///     then per file ActionData{[1]=file name, [6]=size, [9]=directory with
///     trailing '\'} (9 fields) followed by Progress{2,size,0,0}. Files the
///     versioning rules keep produce no message.
///   * RemoveFiles / RemoveDuplicateFiles: per group (RemoveFile matches,
///     File-table files, folders) Progress{1,175000,1,0}, then per item a
///     2-field Progress{2,0} and ActionData{[1]=name,[9]=directory} (a folder
///     item: a 1-field ActionData with the folder path).
///   * CreateFolders / RemoveFolders: a 1-field ActionData with the folder
///     path, no progress.
///   * BindImage schedules nothing (Windows 11 executes no bind op).

#include "winapi/system/msi/msi_files.hpp"
#include "winapi/system/msi/msi_assembly.hpp"

#include "winapi/system/msi/msi_core.hpp"
#include "winapi/system/msi/msi.hpp"
#include "winapi/system/msi/msi_engine.hpp"
#include "winapi/system/msi/msi_features.hpp"
#include "winapi/system/msi/msi_package.hpp"
#include "winapi/system/version/version.hpp"
#include "capsule/vfs/vfs.hpp"
#include "core/compression/cabinet.hpp"

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

namespace vitreo::winapi::msi {

namespace {

namespace cab = vitreo::compression::cabinet;

// MessageBox button sets and results the error protocol uses (WinUser.h).
constexpr u32 kMbAbortRetryIgnore = 0x2;
constexpr u32 kMbRetryCancel = 0x5;
constexpr i32 kIdCancel = 2;
constexpr i32 kIdAbort = 3;
constexpr i32 kIdRetry = 4;
constexpr i32 kIdIgnore = 5;

// Windows Installer error numbers (Learn: Windows Installer Error Messages).
constexpr i32 kErrDirIsFile = 1301;       // Cannot create the file '[2]'. A directory with this name already exists.
constexpr i32 kErrReadFile = 1305;        // Error reading from File: [2]; System error code: [3]
constexpr i32 kErrSourceMissing = 1308;   // Source file not found: [2]
constexpr i32 kErrCreateDest = 1310;      // Error attempting to create the destination file: [3]. System error code: [2]
constexpr i32 kErrCabinetMissing = 1311;  // Could not locate source file cabinet: [2].
constexpr i32 kErrFileIsDir = 1312;       // Cannot create the directory '[2]'. A file with this name already exists.
constexpr i32 kErrCreateDir = 1317;       // An error occurred while attempting to create the directory: [2]
constexpr i32 kErrNotInCabinet = 1334;    // The file '[2]' cannot be installed because the file cannot be found in cabinet file '[3]'.
constexpr i32 kErrCabinetCorrupt = 1335;  // The cabinet file '[2]' required for this installation is corrupt and cannot be used.

// Progress record values (Learn: "Adding Custom Actions to the ProgressBar",
// INSTALLMESSAGE_PROGRESS) and the per-item tick the removal actions
// announce (measured).
constexpr i32 kProgressActionInfo = 1;
constexpr i32 kProgressReport = 2;
constexpr i32 kRemoveTicksPerItem = 175000;

// ---- small text helpers -------------------------------------------------------

u64 to_u64(const std::string& text) {
    u64 v = 0;
    for (char c : text) {
        if (c < '0' || c > '9') break;
        v = v * 10 + static_cast<u64>(c - '0');
    }
    return v;
}

i64 to_i64(const std::string& text) {
    if (!text.empty() && text[0] == '-') return -static_cast<i64>(to_u64(text.substr(1)));
    return static_cast<i64>(to_u64(text));
}

char lower_ascii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lower(std::string s) {
    for (char& c : s) c = lower_ascii(c);
    return s;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (lower_ascii(a[i]) != lower_ascii(b[i])) return false;
    return true;
}

std::string with_slash(std::string path) {
    if (!path.empty() && path.back() != '\\') path.push_back('\\');
    return path;
}

std::string without_slash(std::string path) {
    while (path.size() > 3 && path.back() == '\\') path.pop_back();
    return path;
}

std::string parent_dir(const std::string& path) {
    const std::string p = without_slash(path);
    const usize slash = p.find_last_of('\\');
    return slash == std::string::npos ? std::string{} : p.substr(0, slash + 1);
}

std::string leaf_name(const std::string& path) {
    const std::string p = without_slash(path);
    const usize slash = p.find_last_of('\\');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

/// A full path is drive-rooted (`X:\`) or UNC (`\\server\share`) (Learn:
/// MoveFiles "does not act on entries ... that do not evaluate to a full
/// path").
bool is_full_path(const std::string& p) {
    if (p.size() >= 3 && p[1] == ':' && p[2] == '\\') return true;
    return p.size() >= 2 && p[0] == '\\' && p[1] == '\\';
}

/// `short|long` → (short, long); a single name serves as both.
std::pair<std::string, std::string> split_file_name(const std::string& cell) {
    const usize bar = cell.find('|');
    if (bar == std::string::npos) return {cell, cell};
    return {cell.substr(0, bar), cell.substr(bar + 1)};
}

std::u16string utf16(const std::string& s) {
    std::u16string out;
    out.reserve(s.size());
    for (usize i = 0; i < s.size();) {
        const u8 c = static_cast<u8>(s[i]);
        u32 cp = c;
        usize n = 1;
        if (c >= 0xF0) { cp = c & 0x07; n = 4; }
        else if (c >= 0xE0) { cp = c & 0x0F; n = 3; }
        else if (c >= 0xC0) { cp = c & 0x1F; n = 2; }
        if (i + n > s.size()) { cp = 0xFFFD; n = 1; }
        else for (usize k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<u8>(s[i + k]) & 0x3F);
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::vector<u16> parse_languages(const std::string& text) {
    std::vector<u16> langs;
    std::string cur;
    auto flush = [&] {
        std::string t;
        for (char c : cur) if (c != ' ') t.push_back(c);
        if (!t.empty()) langs.push_back(static_cast<u16>(to_u64(t)));
        cur.clear();
    };
    for (char c : text) {
        if (c == ',') flush(); else cur.push_back(c);
    }
    flush();
    std::sort(langs.begin(), langs.end());
    langs.erase(std::unique(langs.begin(), langs.end()), langs.end());
    return langs;
}

/// Cell of @p row in column @p name, "" when the column or cell is absent.
struct Columns {
    const Table* table{nullptr};
    std::vector<std::optional<usize>> index;
    Columns(const Table* t, std::initializer_list<const char*> names) : table(t) {
        for (const char* n : names) index.push_back(t ? t->column_index(n) : std::nullopt);
    }
    [[nodiscard]] const std::string& cell(const std::vector<std::string>& row, usize which) const {
        static const std::string empty;
        const auto& i = index[which];
        if (!i.has_value() || *i >= row.size()) return empty;
        return row[*i];
    }
};

// ---- records --------------------------------------------------------------------

void set_text(Record& r, u32 field, const std::string& text) {
    r.fields[field].kind = Record::Kind::String;
    r.fields[field].text = utf16(text);
}

void set_int(Record& r, u32 field, i64 value) {
    r.fields[field].kind = Record::Kind::Integer;
    r.fields[field].integer = static_cast<i32>(value);
}

Record file_action_data(const std::string& name, std::optional<u64> size, const std::string& dir) {
    Record r(9);
    set_text(r, 1, name);
    if (size.has_value()) set_int(r, 6, static_cast<i64>(*size));
    set_text(r, 9, with_slash(dir));
    return r;
}

Record folder_action_data(const std::string& folder) {
    Record r(1);
    set_text(r, 1, with_slash(folder));
    return r;
}

Record progress4(i32 a, i64 b, i32 c, i32 d) {
    Record r(4);
    set_int(r, 1, a);
    set_int(r, 2, b);
    set_int(r, 3, c);
    set_int(r, 4, d);
    return r;
}

Record progress2(i32 a, i64 b) {
    Record r(2);
    set_int(r, 1, a);
    set_int(r, 2, b);
    return r;
}

// ---- script-time environment ----------------------------------------------------

/// What a ScriptOp needs when InstallFinalize runs it. The session and the
/// services outlive the script (the engine owns both for the whole install).
struct OpEnv {
    Session* session{nullptr};
    Services* services{nullptr};
    [[nodiscard]] capsule::Vfs* vfs() const { return services ? services->vfs : nullptr; }
    i32 send(u32 type, const Record& r) const {
        ActionContext ctx(*session, *services);
        return ctx.message(type, r);
    }
    /// Progress/ActionData: IDCANCEL from the handler cancels the install.
    [[nodiscard]] bool tell(u32 type, const Record& r) const { return send(type, r) != kIdCancel; }
};

OpEnv env_of(ActionContext& ctx) { return OpEnv{&ctx.session(), &ctx.services()}; }

enum class ErrorChoice { Retry, Ignore, Fail, Cancel };

/// Report an install error through the message chain: vital files get
/// Retry/Cancel, others Abort/Retry/Ignore (Learn: File table,
/// msidbFileAttributesVital). No handler answer (quiet UI) is a failure, as
/// the probe measured for both kinds (1603).
ErrorChoice report_error(const OpEnv& env, bool vital, const Record& rec) {
    const i32 r = env.send(kMsgError | (vital ? kMbRetryCancel : kMbAbortRetryIgnore), rec);
    if (r == kIdRetry) return ErrorChoice::Retry;
    if (r == kIdIgnore && !vital) return ErrorChoice::Ignore;
    if (r == kIdCancel) return ErrorChoice::Cancel;
    (void)kIdAbort;
    return ErrorChoice::Fail;
}

Record error_record(i32 code, std::initializer_list<std::string> args) {
    Record r(static_cast<u32>(args.size() + 1));
    set_int(r, 1, code);
    u32 f = 2;
    for (const std::string& a : args) set_text(r, f++, a);
    return r;
}

u32 win32_error_of(capsule::VfsError e) {
    switch (e) {
        case capsule::VfsError::FileNotFound: return 2;
        case capsule::VfsError::PathNotFound: return 3;
        case capsule::VfsError::AccessDenied: return 5;
        case capsule::VfsError::SharingViolation: return 32;
        case capsule::VfsError::DiskFull: return 112;
        case capsule::VfsError::PathAlreadyExists: return 183;
        case capsule::VfsError::DirectoryNotEmpty: return 145;
        default: return 1117;  // ERROR_IO_DEVICE
    }
}

// ---- VFS helpers ------------------------------------------------------------------

std::optional<capsule::FileInfo> stat_path(capsule::Vfs& vfs, const std::string& path) {
    auto st = vfs.stat(without_slash(path));
    if (st.is_err()) return std::nullopt;
    return st.value();
}

bool is_directory(const capsule::FileInfo& info) {
    return capsule::has_attribute(info.attributes, capsule::FileAttributes::Directory);
}

using BytesResult = Result<std::vector<u8>, capsule::VfsError>;
using VoidResult = Result<void, capsule::VfsError>;

BytesResult read_all(capsule::Vfs& vfs, const std::string& path) {
    auto opened = vfs.open(path, capsule::AccessMode::Read, capsule::ShareMode::Read,
                           capsule::CreationDisposition::OpenExisting);
    if (opened.is_err()) return BytesResult::err(opened.error());
    std::vector<u8> bytes;
    if (auto st = vfs.stat_handle(opened.value()); st.is_ok()) bytes.reserve(st.value().size);
    std::vector<u8> chunk(1 << 20);
    for (;;) {
        auto n = vfs.read(opened.value(), chunk);
        if (n.is_err()) { (void)vfs.close(opened.value()); return BytesResult::err(n.error()); }
        if (n.value() == 0) break;
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n.value()));
    }
    (void)vfs.close(opened.value());
    return BytesResult::ok(std::move(bytes));
}

VoidResult write_all(capsule::Vfs& vfs, const std::string& path, std::span<const u8> bytes) {
    auto opened = vfs.open(path, capsule::AccessMode::Write, capsule::ShareMode::None,
                           capsule::CreationDisposition::CreateAlways);
    if (opened.is_err()) return VoidResult::err(opened.error());
    usize done = 0;
    while (done < bytes.size()) {
        auto n = vfs.write(opened.value(), bytes.subspan(done));
        if (n.is_err()) { (void)vfs.close(opened.value()); return VoidResult::err(n.error()); }
        if (n.value() == 0) { (void)vfs.close(opened.value()); return VoidResult::err(capsule::VfsError::WriteError); }
        done += n.value();
    }
    (void)vfs.close(opened.value());
    return VoidResult::ok();
}

bool dir_is_empty(capsule::Vfs& vfs, const std::string& dir) {
    auto list = vfs.list_dir(without_slash(dir));
    if (list.is_err()) return false;
    for (const auto& e : list.value())
        if (e.name != "." && e.name != "..") return false;
    return true;
}

// ---- the rollback journal ----------------------------------------------------------

/// What one ScriptOp changed, so its rollback can undo it newest first and
/// its commit can drop the backups.
struct Journal {
    enum class Kind : u8 { CreatedFile, ReplacedFile, RemovedFile, CreatedDir, RemovedDir };
    struct Entry {
        Kind kind;
        std::string path;
        std::string backup;  ///< Config.Msi copy for Replaced/Removed
        u32 attributes{0};   ///< RemovedDir/RemovedFile: attributes to restore
    };
    capsule::Vfs* vfs{nullptr};
    std::vector<Entry> entries;
    std::set<std::string> known_dirs;  ///< lower-case directories known to exist

    /// Move @p path into the rollback store; false when it could not be moved.
    bool stash(const std::string& path, std::string& backup) {
        const std::string drive = (path.size() >= 2 && path[1] == ':') ? path.substr(0, 2) : std::string("C:");
        const std::string store = drive + "\\Config.Msi";
        if (!stat_path(*vfs, store).has_value()) {
            (void)vfs->create_dir(store);
            (void)vfs->set_attributes(store, 0x2 | 0x4);  // hidden | system, as Windows creates it
        }
        static std::atomic<u64> counter{0};
        static const u64 seed = static_cast<u64>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        char name[64];
        std::snprintf(name, sizeof name, "\\%08llx%08llx.rbf",
                      static_cast<unsigned long long>(seed & 0xFFFFFFFFull),
                      static_cast<unsigned long long>(counter.fetch_add(1)));
        backup = store + name;
        return vfs->rename(path, backup).is_ok();
    }
    void drop_file(const std::string& path) {
        (void)vfs->set_attributes(path, 0x80);  // FILE_ATTRIBUTE_NORMAL: a read-only file can be deleted
        (void)vfs->remove(path);
    }

    void rollback() {
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            switch (it->kind) {
                case Kind::CreatedFile: drop_file(it->path); break;
                case Kind::ReplacedFile:
                    drop_file(it->path);
                    (void)vfs->rename(it->backup, it->path);
                    break;
                case Kind::RemovedFile: (void)vfs->rename(it->backup, it->path); break;
                case Kind::CreatedDir: (void)vfs->remove(without_slash(it->path)); break;
                case Kind::RemovedDir: (void)vfs->create_dir(without_slash(it->path)); break;
            }
        }
        entries.clear();
    }
    void commit() {
        for (const Entry& e : entries)
            if (!e.backup.empty()) drop_file(e.backup);
        entries.clear();
    }

    /// Create @p dir and any missing parent, journaling each created level.
    capsule::VfsError ensure_dir(const std::string& dir) {
        const std::string d = without_slash(dir);
        if (d.empty()) return capsule::VfsError::InvalidPath;
        const std::string key = lower(d);
        if (known_dirs.count(key) != 0) return capsule::VfsError::None;
        if (auto st = stat_path(*vfs, d)) {
            if (!is_directory(*st)) return capsule::VfsError::NotADirectory;
            known_dirs.insert(key);
            return capsule::VfsError::None;
        }
        const std::string parent = parent_dir(d);
        if (!parent.empty() && without_slash(parent) != d) {
            if (const auto e = ensure_dir(parent); e != capsule::VfsError::None) return e;
        }
        auto made = vfs->create_dir(d);
        if (made.is_err() && made.error() != capsule::VfsError::PathAlreadyExists) return made.error();
        if (made.is_ok()) entries.push_back(Entry{Kind::CreatedDir, d, {}, 0});
        known_dirs.insert(key);
        return capsule::VfsError::None;
    }

    /// Put @p bytes at @p path, stashing a file already there.
    capsule::VfsError place(const std::string& path, std::span<const u8> bytes, u32 attributes) {
        std::string backup;
        bool replaced = false;
        if (auto st = stat_path(*vfs, path)) {
            if (is_directory(*st)) return capsule::VfsError::NotAFile;
            if (!stash(path, backup)) return capsule::VfsError::SharingViolation;
            replaced = true;
        }
        auto w = write_all(*vfs, path, bytes);
        if (w.is_err()) {
            drop_file(path);
            if (replaced) (void)vfs->rename(backup, path);
            return w.error();
        }
        entries.push_back(Entry{replaced ? Kind::ReplacedFile : Kind::CreatedFile, path, backup, 0});
        if ((attributes & (kFileReadOnly | kFileHidden | kFileSystem)) != 0)
            (void)vfs->set_attributes(path, attributes & (kFileReadOnly | kFileHidden | kFileSystem));
        return capsule::VfsError::None;
    }

    /// Remove a file (into the rollback store). False when it is not there.
    bool remove_file(const std::string& path) {
        auto st = stat_path(*vfs, path);
        if (!st.has_value() || is_directory(*st)) return false;
        std::string backup;
        if (!stash(path, backup)) return false;
        entries.push_back(Entry{Kind::RemovedFile, path, backup, static_cast<u32>(st->attributes)});
        return true;
    }

    /// Remove an empty folder. False when absent or not empty.
    bool remove_empty_dir(const std::string& dir) {
        const std::string d = without_slash(dir);
        auto st = stat_path(*vfs, d);
        if (!st.has_value() || !is_directory(*st) || !dir_is_empty(*vfs, d)) return false;
        if (vfs->remove(d).is_err()) return false;
        known_dirs.erase(lower(d));
        entries.push_back(Entry{Kind::RemovedDir, d, {}, 0});
        return true;
    }
};

/// Schedule @p body as one op of @p action with its journal's rollback and a
/// commit op that drops the rollback store's copies. Outside InstallInitialize
/// .. InstallFinalize a system change is refused with 1603, as Windows does
/// (msi_engine.hpp; MsiDoAction(InstallFiles) from MsiOpenPackage).
u32 schedule_with_journal(ActionContext& ctx, const std::string& action,
                          std::function<u32(const OpEnv&, Journal&)> body) {
    if (!ctx.scripting()) return kActionFailure;
    auto journal = std::make_shared<Journal>();
    const OpEnv env = env_of(ctx);
    ScriptOp op;
    op.action = action;
    op.run = [env, journal, action, body = std::move(body)]() -> u32 {
        // The script announces each action it executes (measured: an
        // ActionStart per action while InstallFinalize runs the script).
        if (Engine* e = env.session->engine()) e->action_start(action);
        capsule::Vfs* vfs = env.vfs();
        if (vfs == nullptr) {
            log_unimplemented("file actions", "no VFS in the install services");
            return kActionFailure;
        }
        journal->vfs = vfs;
        return body(env, *journal);
    };
    op.rollback = [journal]() { if (journal->vfs) journal->rollback(); };
    ctx.schedule(std::move(op));
    ScriptOp commit;
    commit.action = action;
    commit.commit_only = true;
    commit.run = [journal]() -> u32 {
        if (journal->vfs) journal->commit();
        return kActionSuccess;
    };
    ctx.schedule(std::move(commit));
    return kActionSuccess;
}

// ---- component states ------------------------------------------------------------

bool installing(const ComponentInfo& c) {
    return c.action == install_state::kLocal || c.action == install_state::kSource;
}

bool installing_local(const ComponentInfo& c) { return c.action == install_state::kLocal; }

/// Being removed from the machine, or leaving the local disk for
/// run-from-source (Learn: RemoveFiles, RemoveFolders).
bool removing(const ComponentInfo& c) {
    if (c.action == install_state::kAbsent || c.action == install_state::kRemoved) return true;
    return c.action == install_state::kSource && c.installed == install_state::kLocal;
}

const ComponentInfo* component(Session& s, const std::string& name) { return find_component(s, name); }

// ---- per-session file model ---------------------------------------------------------

struct MediaRow {
    u32 disk_id{0};
    u32 last_sequence{0};
    std::string disk_prompt;
    std::string cabinet;
    std::string volume_label;
    std::string source;  ///< Source column (a property naming the cabinet's location)
};

struct FileModel {
    const Table* table{nullptr};
    usize row_count{~usize{0}};
    std::vector<FileRow> rows;
    std::unordered_map<std::string, usize> index;  ///< key -> rows
    std::unordered_map<std::string, std::vector<usize>> by_component;
    bool mode_loaded{false};
    SourceMode mode;
};

FileModel& model(Session& session) {
    FileModel& m = session.state<FileModel>();
    Package* pkg = session.package();
    const Table* t = pkg ? pkg->table("File") : nullptr;
    if (t == m.table && t != nullptr && t->rows.size() == m.row_count) return m;
    m.table = t;
    m.rows.clear();
    m.index.clear();
    m.by_component.clear();
    m.row_count = t ? t->rows.size() : 0;
    if (t == nullptr) return m;
    const Columns c(t, {"File", "Component_", "FileName", "FileSize", "Version", "Language", "Attributes", "Sequence"});
    for (const auto& row : t->rows) {
        FileRow f;
        f.key = c.cell(row, 0);
        f.component = c.cell(row, 1);
        std::tie(f.short_name, f.long_name) = split_file_name(c.cell(row, 2));
        f.size = to_u64(c.cell(row, 3));
        f.version = c.cell(row, 4);
        f.language = c.cell(row, 5);
        f.attributes = static_cast<u32>(to_u64(c.cell(row, 6)));
        f.sequence = static_cast<u32>(to_u64(c.cell(row, 7)));
        m.index[f.key] = m.rows.size();
        m.by_component[f.component].push_back(m.rows.size());
        m.rows.push_back(std::move(f));
    }
    return m;
}

std::vector<MediaRow> media_rows(Session& session) {
    std::vector<MediaRow> media;
    const Table* t = session.package() ? session.package()->table("Media") : nullptr;
    if (t == nullptr) return media;
    const Columns c(t, {"DiskId", "LastSequence", "DiskPrompt", "Cabinet", "VolumeLabel", "Source"});
    for (const auto& row : t->rows) {
        MediaRow m;
        m.disk_id = static_cast<u32>(to_u64(c.cell(row, 0)));
        m.last_sequence = static_cast<u32>(to_u64(c.cell(row, 1)));
        m.disk_prompt = c.cell(row, 2);
        m.cabinet = c.cell(row, 3);
        m.volume_label = c.cell(row, 4);
        m.source = c.cell(row, 5);
        media.push_back(std::move(m));
    }
    // Learn (Media table): a disk holds the sequences above the previous
    // disk's LastSequence up to its own, so order by LastSequence.
    std::stable_sort(media.begin(), media.end(),
                     [](const MediaRow& a, const MediaRow& b) { return a.last_sequence < b.last_sequence; });
    return media;
}

/// Index of the media row holding @p sequence, or -1.
i32 media_for(const std::vector<MediaRow>& media, u32 sequence) {
    for (usize i = 0; i < media.size(); ++i)
        if (sequence <= media[i].last_sequence) return static_cast<i32>(i);
    return -1;
}

std::map<std::string, std::array<u32, 4>> file_hashes(Session& session) {
    std::map<std::string, std::array<u32, 4>> hashes;
    const Table* t = session.package() ? session.package()->table("MsiFileHash") : nullptr;
    if (t == nullptr) return hashes;
    const Columns c(t, {"File_", "HashPart1", "HashPart2", "HashPart3", "HashPart4"});
    for (const auto& row : t->rows) {
        std::array<u32, 4> h{};
        for (usize i = 0; i < 4; ++i) h[i] = static_cast<u32>(to_i64(c.cell(row, i + 1)));
        hashes[c.cell(row, 0)] = h;
    }
    return hashes;
}

VersionedFile package_info(const FileRow& row) {
    VersionedFile v;
    v.version = parse_file_version(row.version);
    v.languages = parse_languages(row.language);
    return v;
}

/// Parent key when the Version column names another File row (Learn:
/// Companion Files), else empty.
std::string companion_parent(Session& session, const FileRow& row) {
    if (row.version.empty() || parse_file_version(row.version).has_value()) return {};
    return find_file_row(session, row.version) != nullptr ? row.version : std::string{};
}

/// SourceDir, resolving it from the package location when ResolveSource has
/// not run (Learn: SourceDir "Default Value: the directory that contains the
/// installation package").
std::string source_dir(Session& session) {
    std::string s = session.property("SourceDir");
    if (s.empty()) s = session.property("SOURCEDIR");
    if (s.empty()) {
        std::string db = session.property("OriginalDatabase");
        if (db.empty()) db = session.property("DATABASE");
        s = parent_dir(db);
    }
    return with_slash(s);
}

/// Property value as a directory, or empty when it is not a full path.
std::string directory_property(Session& session, const std::string& name) {
    if (name.empty()) return {};
    std::string v = session.property(name);
    if (!is_full_path(v)) return {};
    return with_slash(v);
}

// ---- cabinets ------------------------------------------------------------------------

/// One cabinet's bytes: an embedded stream (borrowed from the package) or an
/// external file (owned).
struct CabinetImage {
    std::vector<u8> owned;
    std::span<const u8> bytes;
};

}  // namespace

// ==== public helpers =====================================================================

SourceMode source_mode(Session& session) {
    FileModel& m = session.state<FileModel>();
    if (m.mode_loaded) return m.mode;
    m.mode_loaded = true;
    // Summary information property set ([MS-OLEPS]): the stream starts with
    // a 28-byte header and one (FMTID, offset) pair; the section holds
    // (PID, offset) pairs; PID_WORDCOUNT (15) is a VT_I4.
    const std::vector<u8>* s = session.package() ? session.package()->stream("\x05SummaryInformation") : nullptr;
    i64 word_count = 0;
    auto rd32 = [&](usize at) -> std::optional<u32> {
        if (s == nullptr || at + 4 > s->size()) return std::nullopt;
        return static_cast<u32>((*s)[at]) | (static_cast<u32>((*s)[at + 1]) << 8) |
               (static_cast<u32>((*s)[at + 2]) << 16) | (static_cast<u32>((*s)[at + 3]) << 24);
    };
    if (auto section = rd32(28 + 16)) {
        const auto count = rd32(*section + 4);
        for (u32 i = 0; count && i < *count && i < 1024; ++i) {
            const auto pid = rd32(*section + 8 + i * 8);
            const auto off = rd32(*section + 12 + i * 8);
            if (!pid || !off || *pid != 15) continue;
            const auto type = rd32(*section + *off);
            const auto value = rd32(*section + *off + 4);
            if (type && value && ((*type & 0xFFFF) == 3 || (*type & 0xFFFF) == 2)) {
                word_count = (*type & 0xFFFF) == 2 ? static_cast<i16>(*value & 0xFFFF)
                                                   : static_cast<i32>(*value);
            }
        }
    }
    m.mode.short_names = (word_count & 1) != 0;
    m.mode.compressed = (word_count & 2) != 0;
    m.mode.admin_image = (word_count & 4) != 0;
    m.mode.no_elevation = (word_count & 8) != 0;
    return m.mode;
}

const std::vector<FileRow>& file_rows(Session& session) { return model(session).rows; }

const FileRow* find_file_row(Session& session, const std::string& key) {
    FileModel& m = model(session);
    const auto it = m.index.find(key);
    return it == m.index.end() ? nullptr : &m.rows[it->second];
}

std::vector<const FileRow*> file_rows_of_component(Session& session, const std::string& comp) {
    FileModel& m = model(session);
    std::vector<const FileRow*> out;
    if (const auto it = m.by_component.find(comp); it != m.by_component.end())
        for (usize i : it->second) out.push_back(&m.rows[i]);
    return out;
}

std::string target_file_name(Session& session, const FileRow& row) {
    return session.property("SHORTFILENAMES").empty() ? row.long_name : row.short_name;
}

std::string file_target_path(Session& session, const std::string& key) {
    const FileRow* row = find_file_row(session, key);
    if (row == nullptr) return {};
    const std::string dir = component_directory(session, row->component);
    if (dir.empty()) return {};
    return with_slash(dir) + target_file_name(session, *row);
}

void install_file_state_resolvers(Session& session) {
    ItemStateResolvers& st = session.item_states();
    if (st.file_path) return;
    Session* s = &session;
    // Learn (Formatted): [#filekey] is the full path of the file when its
    // component is installed (local or source: the run-from-source path);
    // absent or unknown is the empty string.
    st.file_path = [s](const std::string& key, bool short_path) -> std::optional<std::string> {
        (void)short_path;
        const FileRow* row = find_file_row(*s, key);
        if (row == nullptr) return std::nullopt;
        const ComponentInfo* c = find_component(*s, row->component);
        if (c == nullptr) return std::nullopt;
        const i32 state = (c->action == install_state::kLocal || c->action == install_state::kSource) ? c->action
                                                                                                    : c->installed;
        if (state == install_state::kLocal) return file_target_path(*s, key);
        if (state == install_state::kSource) {
            const SourceMode mode = source_mode(*s);
            std::string dir = mode.compressed ? source_dir(*s) : source_path(*s, c->directory);
            if (dir.empty()) return std::nullopt;
            return with_slash(dir) + (mode.short_names ? row->short_name : row->long_name);
        }
        return std::nullopt;
    };
}

std::optional<u64> parse_file_version(const std::string& text) {
    if (text.empty()) return std::nullopt;
    u64 parts[4] = {0, 0, 0, 0};
    usize part = 0;
    bool digit = false;
    for (char c : text) {
        if (c == '.') {
            if (!digit || ++part > 3) return std::nullopt;
            digit = false;
            continue;
        }
        if (c < '0' || c > '9') return std::nullopt;
        parts[part] = parts[part] * 10 + static_cast<u64>(c - '0');
        if (parts[part] > 65535) return std::nullopt;
        digit = true;
    }
    if (!digit) return std::nullopt;
    return (parts[0] << 48) | (parts[1] << 32) | (parts[2] << 16) | parts[3];
}

std::array<u32, 4> compute_file_hash(std::span<const u8> bytes) {
    // MsiGetFileHash is MD5 with the digest read as four little-endian
    // DWORDs (measured: Installer.FileHash of "MARK:heq_old;" =
    // 2b26589f ce950772 1ee01dab fa434cb6, md5 9f58262b720795ce...).
    u8 digest[CC_MD5_DIGEST_LENGTH];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CC_MD5_CTX ctx;
    CC_MD5_Init(&ctx);
    usize done = 0;
    while (done < bytes.size()) {
        const usize n = std::min<usize>(bytes.size() - done, 1u << 30);
        CC_MD5_Update(&ctx, bytes.data() + done, static_cast<CC_LONG>(n));
        done += n;
    }
    CC_MD5_Final(digest, &ctx);
#pragma clang diagnostic pop
    std::array<u32, 4> out{};
    for (usize i = 0; i < 4; ++i)
        out[i] = static_cast<u32>(digest[i * 4]) | (static_cast<u32>(digest[i * 4 + 1]) << 8) |
                 (static_cast<u32>(digest[i * 4 + 2]) << 16) | (static_cast<u32>(digest[i * 4 + 3]) << 24);
    return out;
}

ReinstallMode parse_reinstall_mode(const std::string& text) {
    ReinstallMode m;
    // Learn: REINSTALLMODE "By default ... is omus"; options are
    // case-insensitive and order-independent.
    const std::string t = text.empty() ? std::string("omus") : lower(text);
    for (char c : t) {
        switch (c) {
            case 'p': m.p = true; break;
            case 'o': m.o = true; break;
            case 'e': m.e = true; break;
            case 'd': m.d = true; break;
            case 'c': m.c = true; break;
            case 'a': m.a = true; break;
            default: break;
        }
    }
    return m;
}

ExistingFile probe_existing_file(capsule::Vfs& vfs, const std::string& path) {
    ExistingFile f;
    const auto st = stat_path(vfs, path);
    if (!st.has_value() || is_directory(*st)) return f;
    f.exists = true;
    f.created = st->creation_time;
    f.modified = st->last_write_time;
    auto bytes = read_all(vfs, path);
    if (bytes.is_err()) return f;
    f.bytes = std::move(bytes.value());
    f.info.version = fixed_file_version(f.bytes);
    if (f.info.version.has_value()) {
        u32 error = 0;
        const std::vector<u8> block = version_resource_from_image(f.bytes, error);
        if (!block.empty()) {
            VersionManager vm;
            if (auto q = vm.ver_query_value(block, "\\VarFileInfo\\Translation")) {
                for (u32 i = 0; i + 4 <= q->length; i += 4) {
                    const usize at = q->value_offset + i;
                    if (at + 2 > block.size()) break;
                    f.info.languages.push_back(static_cast<u16>(block[at] | (block[at + 1] << 8)));
                }
                std::sort(f.info.languages.begin(), f.info.languages.end());
                f.info.languages.erase(std::unique(f.info.languages.begin(), f.info.languages.end()),
                                       f.info.languages.end());
            }
        }
    }
    return f;
}

namespace {

/// Equal versions: install unless the languages already on disk cover the
/// package's (Learn: Replacing Existing Files examples FileA/G/H/I/J, "Preserve
/// Superset Languages", "Both Files Have a Version": a language replaces no
/// language, none does not replace none; measured lkeep/lrepl: any differing
/// set is replaced).
bool languages_favor_package(const std::vector<u16>& pkg, const std::vector<u16>& disk) {
    if (pkg.empty()) return false;
    if (disk.empty()) return true;
    return !std::includes(disk.begin(), disk.end(), pkg.begin(), pkg.end());
}

}  // namespace

bool should_install_file(const VersionedFile& pkg, ExistingFile& existing,
                         const std::optional<std::array<u32, 4>>& package_hash,
                         bool checksum_attribute, const ReinstallMode& mode) {
    if (!existing.exists) return true;  // every letter installs a missing file
    if (mode.a) return true;
    if (mode.c && checksum_attribute) {
        if (auto ok = pe_checksum_valid(existing.bytes); ok.has_value() && !*ok) return true;
    }
    if (!mode.o && !mode.e && !mode.d) return false;  // p (and c alone): only missing files
    const auto& pv = pkg.version;
    const auto& dv = existing.info.version;
    if (pv.has_value() && dv.has_value()) {
        if (*pv != *dv) {
            if (mode.d) return true;
            return *pv > *dv;  // o and e install over an older version
        }
        if (mode.e) return true;
        return languages_favor_package(pkg.languages, existing.info.languages);
    }
    if (pv.has_value()) return true;   // Versioned Files Win
    if (dv.has_value()) return false;  // the versioned file on disk wins
    // Neither versioned: a matching MsiFileHash means the same bytes are
    // already there; otherwise Nonversioned Files are User Data — a file
    // modified after it was created is kept (measured heq/hne/hsame,
    // umod/usame/uold).
    if (package_hash.has_value()) {
        if (!existing.hash.has_value()) existing.hash = compute_file_hash(existing.bytes);
        if (*existing.hash == *package_hash) return false;
    }
    return !(existing.modified > existing.created);
}

bool should_install_companion(const VersionedFile& parent_package, ExistingFile& parent_existing,
                              const ExistingFile& companion_existing, const ReinstallMode& mode) {
    if (should_install_file(parent_package, parent_existing, std::nullopt, false, mode)) return true;
    if (!companion_existing.exists) {
        // The parent is kept; a missing companion is still installed when
        // the parent on disk is the same version and language (Learn: File
        // Versioning Rules, "Nonversioned Files Using Companion Files").
        if (!parent_existing.exists) return true;
        return parent_existing.info.version == parent_package.version &&
               parent_existing.info.languages == parent_package.languages;
    }
    // ... and with o or e an existing companion is reinstalled when the
    // parent's version equals the one on the machine.
    return (mode.o || mode.e) && parent_existing.exists &&
           parent_existing.info.version.has_value() &&
           parent_existing.info.version == parent_package.version;
}

bool key_file_supersedes_package(Session& session, capsule::Vfs& vfs, const std::string& comp) {
    const ComponentInfo* c = find_component(session, comp);
    if (c == nullptr || c->key_path.empty() || c->registry_key_path() || c->odbc_key_path()) return false;
    const FileRow* key = find_file_row(session, c->key_path);
    if (key == nullptr || key->component != comp) return false;
    const std::string dir = component_directory(session, comp);
    if (dir.empty()) return false;
    ExistingFile disk = probe_existing_file(vfs, with_slash(dir) + target_file_name(session, *key));
    if (!disk.exists || !disk.info.version.has_value()) return false;
    const VersionedFile pkg = package_info(*key);
    const ReinstallMode mode = parse_reinstall_mode(session.property("REINSTALLMODE"));
    if (should_install_file(pkg, disk, std::nullopt, (key->attributes & kFileChecksum) != 0, mode)) return false;
    return !pkg.version.has_value() || *disk.info.version > *pkg.version;
}

std::optional<bool> pe_checksum_valid(std::span<const u8> image) {
    auto rd16 = [&](usize at) -> u32 { return static_cast<u32>(image[at]) | (static_cast<u32>(image[at + 1]) << 8); };
    auto rd32 = [&](usize at) -> u32 { return rd16(at) | (rd16(at + 2) << 16); };
    if (image.size() < 0x40 || image[0] != 'M' || image[1] != 'Z') return std::nullopt;
    const u32 pe = rd32(0x3C);
    if (static_cast<u64>(pe) + 0x5C + 4 > image.size() || rd32(pe) != 0x00004550) return std::nullopt;
    const usize checksum_at = pe + 0x58;  // OptionalHeader.CheckSum (same offset in PE32 and PE32+)
    const u32 stored = rd32(checksum_at);
    // CheckSumMappedFile: 16-bit one's-complement-style folding sum of the
    // image with the CheckSum field taken as zero, plus the file length.
    u64 sum = 0;
    for (usize i = 0; i + 1 < image.size(); i += 2) {
        if (i == checksum_at || i == checksum_at + 2) continue;
        sum += rd16(i);
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    if (image.size() & 1) {
        sum += image.back();
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    sum = (sum & 0xFFFF) + (sum >> 16);
    const u32 computed = static_cast<u32>(sum + image.size());
    return stored == computed;
}

bool wildcard_match(std::string_view pattern, std::string_view name) {
    usize p = 0, n = 0, star = std::string_view::npos, mark = 0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || lower_ascii(pattern[p]) == lower_ascii(name[n]))) {
            ++p; ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = n;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            n = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

// ==== InstallFiles ========================================================================

namespace {

struct FileJob {
    std::string key;
    std::string name;  ///< installed name
    std::vector<std::string> dirs;  ///< target directories (the component's, then isolated copies)
    std::string source_path;  ///< loose file: full source path
    u64 size{0};
    u32 attributes{0};
    u32 sequence{0};
    usize order{0};       ///< File-table index (stable tie-break)
    i32 media{-1};        ///< index into media; -1 none
    bool compressed{false};
    VersionedFile pkg;
    std::optional<std::array<u32, 4>> hash;
    // Companion files: the parent's package view and installed path.
    bool companion{false};
    VersionedFile parent_pkg;
    std::string parent_path;
};

struct InstallPlan {
    std::vector<FileJob> jobs;
    std::vector<MediaRow> media;
    std::vector<std::string> media_dirs;  ///< per media row: where an external cabinet lives
    ReinstallMode mode;
};

/// Run @p write until it succeeds, the user ignores it, or it fails.
/// Returns kActionSuccess, kActionFailure or kActionUserExit; @p skipped is
/// set when the user chose Ignore.
template <typename Fn>
u32 with_retry(const OpEnv& env, bool vital, Fn&& attempt, bool& skipped) {
    skipped = false;
    for (;;) {
        std::optional<Record> err = attempt();
        if (!err.has_value()) return kActionSuccess;
        switch (report_error(env, vital, *err)) {
            case ErrorChoice::Retry: continue;
            case ErrorChoice::Ignore: skipped = true; return kActionSuccess;
            case ErrorChoice::Cancel: return kActionUserExit;
            case ErrorChoice::Fail: return kActionFailure;
        }
    }
}

std::optional<Record> place_error(capsule::VfsError e, const std::string& path) {
    if (e == capsule::VfsError::None) return std::nullopt;
    if (e == capsule::VfsError::NotAFile) return error_record(kErrDirIsFile, {path});
    if (e == capsule::VfsError::NotADirectory) return error_record(kErrFileIsDir, {parent_dir(path)});
    return error_record(kErrCreateDest, {std::to_string(win32_error_of(e)), path});
}

/// Versioning decision for one job against one target directory.
bool job_wants_install(const OpEnv& env, const InstallPlan& plan, const FileJob& job, const std::string& path) {
    ExistingFile disk = probe_existing_file(*env.vfs(), path);
    if (job.companion) {
        ExistingFile parent = probe_existing_file(*env.vfs(), job.parent_path);
        return should_install_companion(job.parent_pkg, parent, disk, plan.mode);
    }
    return should_install_file(job.pkg, disk, job.hash, (job.attributes & kFileChecksum) != 0, plan.mode);
}

/// Install @p bytes for @p job into every target directory that wants it.
u32 deliver(const OpEnv& env, Journal& journal, const FileJob& job, const std::vector<std::string>& dirs,
            std::span<const u8> bytes) {
    const bool vital = (job.attributes & kFileVital) != 0;
    for (const std::string& dir : dirs) {
        if (!env.tell(kMsgActionData, file_action_data(job.name, job.size, dir))) return kActionUserExit;
        const std::string path = with_slash(dir) + job.name;
        bool skipped = false;
        const u32 rc = with_retry(env, vital, [&]() -> std::optional<Record> {
            const auto made = journal.ensure_dir(dir);
            if (made != capsule::VfsError::None) {
                if (made == capsule::VfsError::NotADirectory) return error_record(kErrFileIsDir, {dir});
                return error_record(kErrCreateDir, {dir});
            }
            return place_error(journal.place(path, bytes, job.attributes), path);
        }, skipped);
        if (rc != kActionSuccess) return rc;
        if (!env.tell(kMsgProgress, progress4(kProgressReport, static_cast<i64>(job.size), 0, 0)))
            return kActionUserExit;
    }
    return kActionSuccess;
}

/// The directories of @p job the versioning rules want written now.
std::vector<std::string> wanted_dirs(const OpEnv& env, const InstallPlan& plan, const FileJob& job) {
    std::vector<std::string> out;
    for (const std::string& d : job.dirs)
        if (job_wants_install(env, plan, job, with_slash(d) + job.name)) out.push_back(d);
    return out;
}

u32 install_loose(const OpEnv& env, Journal& journal, const InstallPlan& plan, const FileJob& job) {
    const std::vector<std::string> dirs = wanted_dirs(env, plan, job);
    if (dirs.empty()) return kActionSuccess;
    std::vector<u8> bytes;
    bool skipped = false;
    const u32 rc = with_retry(env, (job.attributes & kFileVital) != 0, [&]() -> std::optional<Record> {
        auto read = read_all(*env.vfs(), job.source_path);
        if (read.is_err()) {
            if (read.error() == capsule::VfsError::FileNotFound || read.error() == capsule::VfsError::PathNotFound)
                return error_record(kErrSourceMissing, {job.source_path});
            return error_record(kErrReadFile, {job.source_path, std::to_string(win32_error_of(read.error()))});
        }
        bytes = std::move(read.value());
        return std::nullopt;
    }, skipped);
    if (rc != kActionSuccess || skipped) return rc;
    return deliver(env, journal, job, dirs, bytes);
}

/// Install the jobs [first, last) that share one cabinet, decoding each
/// cabinet folder once (a folder is one compression stream: slicing files
/// out of a decoded folder is the only way to avoid re-decoding it per file).
u32 install_from_cabinet(const OpEnv& env, Journal& journal, const InstallPlan& plan,
                         const std::vector<const FileJob*>& jobs, i32 media_index) {
    const MediaRow& media = plan.media[static_cast<usize>(media_index)];
    // Decide first: a cabinet whose files are all kept is never opened.
    std::vector<std::pair<const FileJob*, std::vector<std::string>>> work;
    for (const FileJob* job : jobs) {
        auto dirs = wanted_dirs(env, plan, *job);
        if (!dirs.empty()) work.emplace_back(job, std::move(dirs));
    }
    if (work.empty()) return kActionSuccess;

    CabinetImage image;
    std::optional<cab::Cabinet> cabinet;
    const bool embedded = media.cabinet.front() == '#';
    const std::string stream_name = embedded ? media.cabinet.substr(1) : std::string{};
    const std::string cab_path = embedded ? std::string{} : plan.media_dirs[static_cast<usize>(media_index)] + media.cabinet;
    bool skipped = false;
    u32 rc = with_retry(env, true, [&]() -> std::optional<Record> {
        if (embedded) {
            // Learn (Cabinet data type): a '#' cabinet is a stream of the
            // package, named case-sensitively by the rest of the cell.
            const std::vector<u8>* s = env.session->package()->stream(stream_name);
            if (s == nullptr) return error_record(kErrCabinetMissing, {stream_name});
            image.bytes = std::span<const u8>(s->data(), s->size());
        } else {
            auto read = read_all(*env.vfs(), cab_path);
            if (read.is_err()) return error_record(kErrCabinetMissing, {media.cabinet});
            image.owned = std::move(read.value());
            image.bytes = std::span<const u8>(image.owned.data(), image.owned.size());
        }
        auto parsed = cab::Cabinet::parse(image.bytes);
        if (parsed.is_err()) return error_record(kErrCabinetCorrupt, {media.cabinet});
        cabinet.emplace(std::move(parsed.value()));
        return std::nullopt;
    }, skipped);
    if (rc != kActionSuccess) return rc;

    std::unordered_map<std::string, const cab::FileEntry*> entries;
    for (const cab::FileEntry& e : cabinet->files()) entries.emplace(e.name, &e);

    usize decoded_folder = ~usize{0};
    std::vector<u8> folder;
    for (auto& [job, dirs] : work) {
        const bool vital = (job->attributes & kFileVital) != 0;
        std::span<const u8> bytes;
        rc = with_retry(env, vital, [&]() -> std::optional<Record> {
            const auto it = entries.find(job->key);
            if (it == entries.end()) return error_record(kErrNotInCabinet, {job->name, media.cabinet});
            const cab::FileEntry& e = *it->second;
            if (e.spans_cabinets()) {
                log_unimplemented("InstallFiles", "file continued across cabinets (CFFILE iFolder 0xFFFD-0xFFFF)");
                return error_record(kErrCabinetCorrupt, {media.cabinet});
            }
            if (e.folder_index != decoded_folder) {
                folder.clear();
                decoded_folder = ~usize{0};
                auto data = cabinet->extract_folder(e.folder_index);
                if (data.is_err()) return error_record(kErrCabinetCorrupt, {media.cabinet});
                folder = std::move(data.value());
                decoded_folder = e.folder_index;
            }
            auto slice = cab::Cabinet::slice_file(e, std::span<const u8>(folder.data(), folder.size()));
            if (slice.is_err()) return error_record(kErrCabinetCorrupt, {media.cabinet});
            bytes = slice.value();
            return std::nullopt;
        }, skipped);
        if (rc != kActionSuccess) return rc;
        if (skipped) continue;
        rc = deliver(env, journal, *job, dirs, bytes);
        if (rc != kActionSuccess) return rc;
    }
    return kActionSuccess;
}

u32 run_install_plan(const OpEnv& env, Journal& journal, const InstallPlan& plan) {
    if (!env.tell(kMsgProgress, progress4(kProgressActionInfo, 1, 0, 0))) return kActionUserExit;
    // Sequence order (Learn: File table, the order of installation and of
    // the files in the cabinets); consecutive files of one cabinet share an
    // opened cabinet.
    std::vector<const FileJob*> ordered;
    for (const FileJob& j : plan.jobs) ordered.push_back(&j);
    std::stable_sort(ordered.begin(), ordered.end(), [](const FileJob* a, const FileJob* b) {
        return a->sequence != b->sequence ? a->sequence < b->sequence : a->order < b->order;
    });
    for (usize i = 0; i < ordered.size();) {
        const FileJob* job = ordered[i];
        if (!job->compressed) {
            if (const u32 rc = install_loose(env, journal, plan, *job); rc != kActionSuccess) return rc;
            ++i;
            continue;
        }
        usize end = i;
        std::vector<const FileJob*> group;
        while (end < ordered.size() && ordered[end]->compressed && ordered[end]->media == job->media)
            group.push_back(ordered[end++]);
        if (const u32 rc = install_from_cabinet(env, journal, plan, group, job->media); rc != kActionSuccess)
            return rc;
        i = end;
    }
    return kActionSuccess;
}

/// LockPermissions / MsiLockPermissionsEx rows for @p table: the capsule
/// VFS keeps no security descriptors, so they cannot be applied.
void note_lock_permissions(Session& session, const char* table, const char* action) {
    for (const char* t : {"LockPermissions", "MsiLockPermissionsEx"}) {
        const Table* lp = session.package() ? session.package()->table(t) : nullptr;
        if (lp == nullptr) continue;
        const Columns c(lp, {"Table"});
        for (const auto& row : lp->rows)
            if (c.cell(row, 0) == table) {
                log_unimplemented(action, std::string(t) + " ACLs (the capsule VFS keeps no security descriptors)");
                return;
            }
    }
}

}  // namespace

u32 action_install_files(ActionContext& ctx) {
    Session& s = ctx.session();
    install_file_state_resolvers(s);
    // A system-changing action outside InstallInitialize..InstallFinalize is
    // 1603 even with nothing to do (msi_engine_session_probe).
    if (!ctx.scripting()) return kActionFailure;
    if (s.package() == nullptr) return kActionFailure;
    auto plan = std::make_shared<InstallPlan>();
    plan->media = media_rows(s);
    plan->mode = parse_reinstall_mode(s.property("REINSTALLMODE"));
    const SourceMode mode = source_mode(s);
    const std::string root = source_dir(s);
    for (const MediaRow& m : plan->media) {
        // Learn (Media.Source): a patch's cabinet location property; empty
        // means SourceDir. External cabinets sit at the source root.
        std::string where = m.source.empty() ? root : with_slash(s.property(m.source));
        if (where.empty() || where == "\\") where = root;
        plan->media_dirs.push_back(where);
    }
    const auto hashes = file_hashes(s);
    note_lock_permissions(s, "File", "InstallFiles");
    const std::vector<FileRow>& rows = file_rows(s);
    // Global assemblies (msi_assembly.hpp): staged during the script and
    // committed into the assembly cache after it, never copied to the
    // component's directory.
    std::map<std::string, AssemblyInstall> assemblies;
    for (usize i = 0; i < rows.size(); ++i) {
        const FileRow& row = rows[i];
        const ComponentInfo* c = component(s, row.component);
        // Learn (InstallFiles): only files whose components resolve to the
        // local state are copied.
        if (c == nullptr || !installing_local(*c)) continue;
        auto ga = assemblies.find(row.component);
        if (ga == assemblies.end())
            ga = assemblies.emplace(row.component, plan_assembly_install(s, ctx.services().vfs, row.component)).first;
        if (ga->second.kind == AssemblyInstall::Kind::Unsupported) return kActionFailure;
        if (ga->second.kind == AssemblyInstall::Kind::AlreadyCached) continue;
        const bool staged = ga->second.kind == AssemblyInstall::Kind::Stage;
        FileJob job;
        job.key = row.key;
        job.name = target_file_name(s, row);
        const std::string dir = staged ? ga->second.staging : component_directory(s, row.component);
        if (dir.empty()) continue;
        job.dirs.push_back(with_slash(dir));
        for (const std::string& app : staged ? std::vector<std::string>{} : c->isolated_into) {
            const std::string iso = component_directory(s, app);
            if (!iso.empty()) job.dirs.push_back(with_slash(iso));
        }
        job.size = row.size;
        job.attributes = row.attributes;
        job.sequence = row.sequence;
        job.order = i;
        job.pkg = package_info(row);
        if (!job.pkg.version.has_value()) {
            if (const auto h = hashes.find(row.key); h != hashes.end()) job.hash = h->second;
        }
        if (const std::string parent = companion_parent(s, row); !parent.empty()) {
            const FileRow* p = find_file_row(s, parent);
            job.companion = true;
            job.parent_pkg = package_info(*p);
            job.parent_path = file_target_path(s, parent);
        }
        job.media = media_for(plan->media, row.sequence);
        // Learn (File.Attributes, Word Count Summary): the Compressed /
        // Noncompressed bits override the package's source type.
        bool compressed = mode.compressed;
        if (row.attributes & kFileNoncompressed) compressed = false;
        else if (row.attributes & kFileCompressed) compressed = true;
        if (compressed && (job.media < 0 || plan->media[static_cast<usize>(job.media)].cabinet.empty()))
            compressed = false;
        job.compressed = compressed;
        if (!compressed) {
            const std::string src_name = mode.short_names ? row.short_name : row.long_name;
            // A compressed package installs uncompressed files only from the
            // source root (Learn: Word Count Summary; measured: "Source for
            // file 'loose.txt' is uncompressed, at '<SourceDir>'").
            std::string src_dir = mode.compressed ? root : source_path(s, c->directory);
            if (src_dir.empty()) src_dir = root;
            job.source_path = with_slash(src_dir) + src_name;
        }
        plan->jobs.push_back(std::move(job));
    }
    if (plan->jobs.empty()) return kActionSuccess;
    const u32 rc = schedule_with_journal(ctx, "InstallFiles", [plan](const OpEnv& env, Journal& journal) {
        return run_install_plan(env, journal, *plan);
    });
    if (rc != kActionSuccess) return rc;
    for (const auto& [comp, ga] : assemblies)
        if (ga.kind == AssemblyInstall::Kind::Stage) schedule_assembly_commit(ctx, comp, ga.staging);
    return kActionSuccess;
}

// ==== RemoveFiles ========================================================================

namespace {

struct RemovePattern {
    std::string dir;
    std::string pattern;  ///< long name / wildcard; empty = the folder itself
};

struct RemovePlan {
    std::vector<RemovePattern> patterns;                       ///< RemoveFile rows with a FileName
    std::vector<std::pair<std::string, std::string>> files;    ///< File-table (dir, name)
    std::vector<std::string> folders;                          ///< RemoveFile rows without a FileName
    std::vector<std::string> cleanup;                          ///< removed components' directories
    std::set<std::string> stop;                                ///< directories never removed by cleanup
};

std::vector<std::string> sorted_matches(capsule::Vfs& vfs, const std::string& dir, const std::string& pattern,
                                        bool files_only) {
    std::vector<std::string> out;
    auto list = vfs.list_dir(without_slash(dir));
    if (list.is_err()) return out;
    for (const auto& e : list.value()) {
        if (e.name == "." || e.name == "..") continue;
        if (files_only && is_directory(e.info)) continue;
        if (wildcard_match(pattern, e.name)) out.push_back(e.name);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    return out;
}

u32 run_remove_plan(const OpEnv& env, Journal& journal, const RemovePlan& plan) {
    capsule::Vfs& vfs = *env.vfs();
    std::vector<std::pair<std::string, std::string>> matched;
    for (const RemovePattern& p : plan.patterns)
        for (const std::string& n : sorted_matches(vfs, p.dir, p.pattern, true)) matched.emplace_back(p.dir, n);
    auto remove_group = [&](const std::vector<std::pair<std::string, std::string>>& group) -> u32 {
        if (group.empty()) return kActionSuccess;
        if (!env.tell(kMsgProgress, progress4(kProgressActionInfo, kRemoveTicksPerItem, 1, 0))) return kActionUserExit;
        for (const auto& [dir, name] : group) {
            if (!env.tell(kMsgProgress, progress2(kProgressReport, 0))) return kActionUserExit;
            if (!env.tell(kMsgActionData, file_action_data(name, std::nullopt, dir))) return kActionUserExit;
            (void)journal.remove_file(with_slash(dir) + name);
        }
        return kActionSuccess;
    };
    if (const u32 rc = remove_group(matched); rc != kActionSuccess) return rc;
    if (const u32 rc = remove_group(plan.files); rc != kActionSuccess) return rc;
    if (!plan.folders.empty()) {
        if (!env.tell(kMsgProgress, progress4(kProgressActionInfo, kRemoveTicksPerItem, 1, 0))) return kActionUserExit;
        for (const std::string& f : plan.folders) {
            if (!env.tell(kMsgProgress, progress2(kProgressReport, 0))) return kActionUserExit;
            if (!env.tell(kMsgActionData, folder_action_data(f))) return kActionUserExit;
            (void)journal.remove_empty_dir(f);
        }
    }
    // Folders the installer created are removed when they become empty
    // (Learn: CreateFolder table remarks), up to the tree's root folders.
    for (const std::string& d : plan.cleanup) {
        std::string cur = with_slash(d);
        while (!cur.empty() && plan.stop.count(lower(cur)) == 0 && journal.remove_empty_dir(cur)) cur = parent_dir(cur);
    }
    return kActionSuccess;
}

}  // namespace

u32 action_remove_files(ActionContext& ctx) {
    Session& s = ctx.session();
    install_file_state_resolvers(s);
    // A system-changing action outside InstallInitialize..InstallFinalize is
    // 1603 even with nothing to do (msi_engine_session_probe).
    if (!ctx.scripting()) return kActionFailure;
    if (s.package() == nullptr) return kActionFailure;
    auto plan = std::make_shared<RemovePlan>();
    if (const Table* t = s.package()->table("RemoveFile")) {
        const Columns c(t, {"FileKey", "Component_", "FileName", "DirProperty", "InstallMode"});
        for (const auto& row : t->rows) {
            const ComponentInfo* comp = component(s, c.cell(row, 1));
            if (comp == nullptr) continue;
            const u32 install_mode = static_cast<u32>(to_u64(c.cell(row, 4)));
            // Learn (RemoveFile.InstallMode): 1 on install (local or
            // source), 2 on removal, 3 both.
            const bool on_install = (install_mode & 1) != 0 && installing(*comp);
            const bool on_remove = (install_mode & 2) != 0 &&
                                   (comp->action == install_state::kAbsent || comp->action == install_state::kRemoved);
            if (!on_install && !on_remove) continue;
            const std::string dir = directory_property(s, c.cell(row, 3));
            if (dir.empty()) continue;
            const std::string name = c.cell(row, 2);
            // Measured: of a `short|long` FileName only the long name is removed.
            if (name.empty()) plan->folders.push_back(dir);
            else plan->patterns.push_back(RemovePattern{dir, split_file_name(name).second});
        }
    }
    FeatureModel& fm = feature_model(s);
    for (const std::string& name : fm.component_order) {
        const ComponentInfo* comp = component(s, name);
        if (comp == nullptr || !removing(*comp)) continue;
        if (comp->attributes & kComponentPermanent) continue;  // Learn: never removed on uninstall
        // A global assembly leaves the assembly cache at commit, once no
        // product publishes it (msi_assembly.hpp); its component directory
        // holds none of its files.
        if (schedule_assembly_removal(ctx, name)) continue;
        const std::string dir = component_directory(s, name);
        if (dir.empty()) continue;
        bool any = false;
        for (const FileRow* f : file_rows_of_component(s, name)) {
            plan->files.emplace_back(with_slash(dir), target_file_name(s, *f));
            any = true;
        }
        if (any) plan->cleanup.push_back(with_slash(dir));
    }
    // Cleanup never climbs into a root of the Directory tree (TARGETDIR and
    // the predefined folders it holds).
    for (const auto& [key, d] : fm.directories)
        if (d.parent.empty() || d.parent == fm.target_root) plan->stop.insert(lower(with_slash(d.target)));
    if (plan->patterns.empty() && plan->files.empty() && plan->folders.empty()) return kActionSuccess;
    return schedule_with_journal(ctx, "RemoveFiles", [plan](const OpEnv& env, Journal& journal) {
        return run_remove_plan(env, journal, *plan);
    });
}

// ==== MoveFiles ========================================================================

namespace {

struct MoveEntry {
    std::string source_dir;   ///< with SourceName: the folder; else empty
    std::string source_file;  ///< without SourceName: the full file path
    std::string pattern;      ///< SourceName (long name, wildcards)
    std::string dest_dir;
    std::string dest_name;    ///< empty = keep the source name
    bool move{false};
};

struct CopyItem {
    std::string from;
    std::string to_dir;
    std::string to_name;
    bool move{false};
};

u32 run_copy_items(const OpEnv& env, Journal& journal, const std::vector<CopyItem>& items) {
    if (items.empty()) return kActionSuccess;
    if (!env.tell(kMsgProgress, progress4(kProgressActionInfo, 1, 0, 0))) return kActionUserExit;
    for (const CopyItem& it : items) {
        auto bytes = read_all(*env.vfs(), it.from);
        if (bytes.is_err()) {
            bool skipped = false;
            const u32 rc = with_retry(env, false, [&]() -> std::optional<Record> {
                bytes = read_all(*env.vfs(), it.from);
                if (bytes.is_ok()) return std::nullopt;
                return error_record(kErrSourceMissing, {it.from});
            }, skipped);
            if (rc != kActionSuccess) return rc;
            if (skipped) continue;
        }
        const u64 size = bytes.value().size();
        if (!env.tell(kMsgActionData, file_action_data(it.to_name, size, it.to_dir))) return kActionUserExit;
        const std::string to = with_slash(it.to_dir) + it.to_name;
        bool skipped = false;
        const u32 rc = with_retry(env, false, [&]() -> std::optional<Record> {
            if (journal.ensure_dir(it.to_dir) != capsule::VfsError::None) return error_record(kErrCreateDir, {it.to_dir});
            return place_error(journal.place(to, bytes.value(), 0), to);
        }, skipped);
        if (rc != kActionSuccess) return rc;
        if (!skipped && it.move) (void)journal.remove_file(it.from);
        if (!env.tell(kMsgProgress, progress4(kProgressReport, static_cast<i64>(size), 0, 0))) return kActionUserExit;
    }
    return kActionSuccess;
}

}  // namespace

u32 action_move_files(ActionContext& ctx) {
    Session& s = ctx.session();
    install_file_state_resolvers(s);
    // A system-changing action outside InstallInitialize..InstallFinalize is
    // 1603 even with nothing to do (msi_engine_session_probe).
    if (!ctx.scripting()) return kActionFailure;
    if (s.package() == nullptr) return kActionFailure;
    const Table* t = s.package()->table("MoveFile");
    if (t == nullptr) return kActionSuccess;
    auto entries = std::make_shared<std::vector<MoveEntry>>();
    const bool short_names = !s.property("SHORTFILENAMES").empty();
    const Columns c(t, {"FileKey", "Component_", "SourceName", "DestName", "SourceFolder", "DestFolder", "Options"});
    for (const auto& row : t->rows) {
        const ComponentInfo* comp = component(s, c.cell(row, 1));
        // Learn (MoveFiles): components installed locally or run from source.
        if (comp == nullptr || !installing(*comp)) continue;
        MoveEntry e;
        e.dest_dir = directory_property(s, c.cell(row, 5));
        if (e.dest_dir.empty()) continue;  // measured mv_nodst: nothing done
        const std::string folder_value = s.property(c.cell(row, 4));
        if (!is_full_path(folder_value)) continue;
        e.pattern = c.cell(row, 2);
        if (e.pattern.empty()) e.source_file = without_slash(folder_value);
        else e.source_dir = with_slash(folder_value);
        const auto [sn, ln] = split_file_name(c.cell(row, 3));
        e.dest_name = short_names ? sn : ln;
        e.move = (to_u64(c.cell(row, 6)) & 1) != 0;
        entries->push_back(std::move(e));
    }
    if (entries->empty()) return kActionSuccess;
    return schedule_with_journal(ctx, "MoveFiles", [entries](const OpEnv& env, Journal& journal) {
        std::vector<CopyItem> items;
        for (const MoveEntry& e : *entries) {
            if (!e.source_file.empty()) {
                if (!stat_path(*env.vfs(), e.source_file).has_value()) continue;
                items.push_back(CopyItem{e.source_file, e.dest_dir,
                                         e.dest_name.empty() ? leaf_name(e.source_file) : e.dest_name, e.move});
                continue;
            }
            bool first = true;
            for (const std::string& n : sorted_matches(*env.vfs(), e.source_dir, e.pattern, true)) {
                // Measured (mv_xls: "*.xls" -> renamed.xls): the first match
                // takes DestName, the other matches keep their own names.
                const std::string name = (first && !e.dest_name.empty()) ? e.dest_name : n;
                first = false;
                items.push_back(CopyItem{e.source_dir + n, e.dest_dir, name, e.move});
            }
        }
        return run_copy_items(env, journal, items);
    });
}

// ==== DuplicateFiles / RemoveDuplicateFiles ============================================

namespace {

struct DuplicateEntry {
    std::string original;
    std::string dest_dir;
    std::string dest_name;
    u64 size{0};
};

/// DuplicateFile rows for components passing @p want, resolved to paths.
std::vector<DuplicateEntry> duplicate_entries(Session& s, bool (*want)(const ComponentInfo&)) {
    std::vector<DuplicateEntry> out;
    const Table* t = s.package() ? s.package()->table("DuplicateFile") : nullptr;
    if (t == nullptr) return out;
    const bool short_names = !s.property("SHORTFILENAMES").empty();
    const Columns c(t, {"FileKey", "Component_", "File_", "DestName", "DestFolder"});
    for (const auto& row : t->rows) {
        const ComponentInfo* comp = component(s, c.cell(row, 1));
        if (comp == nullptr || !want(*comp)) continue;
        const FileRow* file = find_file_row(s, c.cell(row, 2));
        if (file == nullptr) continue;
        const std::string original = file_target_path(s, file->key);
        if (original.empty()) continue;
        DuplicateEntry e;
        e.original = original;
        // Learn (DuplicateFile.DestFolder): a property holding the full
        // path; one that does not resolve does nothing (measured dup3: a
        // null DestFolder and DestName duplicate nothing).
        const std::string folder = c.cell(row, 4);
        e.dest_dir = folder.empty() ? parent_dir(original) : directory_property(s, folder);
        if (e.dest_dir.empty()) continue;
        const auto [sn, ln] = split_file_name(c.cell(row, 3));
        e.dest_name = (short_names ? sn : ln);
        if (e.dest_name.empty()) e.dest_name = leaf_name(original);
        if (iequals(with_slash(e.dest_dir) + e.dest_name, original)) continue;  // same file: no action
        e.size = file->size;
        out.push_back(std::move(e));
    }
    return out;
}

}  // namespace

u32 action_duplicate_files(ActionContext& ctx) {
    Session& s = ctx.session();
    install_file_state_resolvers(s);
    // A system-changing action outside InstallInitialize..InstallFinalize is
    // 1603 even with nothing to do (msi_engine_session_probe).
    if (!ctx.scripting()) return kActionFailure;
    if (s.package() == nullptr) return kActionFailure;
    auto entries = std::make_shared<std::vector<DuplicateEntry>>(duplicate_entries(s, installing_local));
    if (entries->empty()) return kActionSuccess;
    return schedule_with_journal(ctx, "DuplicateFiles", [entries](const OpEnv& env, Journal& journal) {
        std::vector<CopyItem> items;
        for (const DuplicateEntry& e : *entries) items.push_back(CopyItem{e.original, e.dest_dir, e.dest_name, false});
        return run_copy_items(env, journal, items);
    });
}

u32 action_remove_duplicate_files(ActionContext& ctx) {
    Session& s = ctx.session();
    install_file_state_resolvers(s);
    // A system-changing action outside InstallInitialize..InstallFinalize is
    // 1603 even with nothing to do (msi_engine_session_probe).
    if (!ctx.scripting()) return kActionFailure;
    if (s.package() == nullptr) return kActionFailure;
    auto entries = std::make_shared<std::vector<DuplicateEntry>>(
        duplicate_entries(s, [](const ComponentInfo& c) {
            return c.action == install_state::kAbsent || c.action == install_state::kRemoved ||
                   (c.action == install_state::kSource && c.installed == install_state::kLocal);
        }));
    if (entries->empty()) return kActionSuccess;
    return schedule_with_journal(ctx, "RemoveDuplicateFiles", [entries](const OpEnv& env, Journal& journal) -> u32 {
        if (!env.tell(kMsgProgress, progress4(kProgressActionInfo, kRemoveTicksPerItem, 1, 0))) return kActionUserExit;
        for (const DuplicateEntry& e : *entries) {
            if (!env.tell(kMsgProgress, progress2(kProgressReport, 0))) return kActionUserExit;
            if (!env.tell(kMsgActionData, file_action_data(e.dest_name, std::nullopt, e.dest_dir))) return kActionUserExit;
            (void)journal.remove_file(with_slash(e.dest_dir) + e.dest_name);
        }
        return kActionSuccess;
    });
}

// ==== CreateFolders / RemoveFolders ======================================================

namespace {

std::vector<std::string> create_folder_paths(Session& s, bool (*want)(const ComponentInfo&)) {
    std::vector<std::string> out;
    const Table* t = s.package() ? s.package()->table("CreateFolder") : nullptr;
    if (t == nullptr) return out;
    const Columns c(t, {"Directory_", "Component_"});
    for (const auto& row : t->rows) {
        const ComponentInfo* comp = component(s, c.cell(row, 1));
        if (comp == nullptr || !want(*comp)) continue;
        const std::string dir = target_path(s, c.cell(row, 0));
        if (!dir.empty()) out.push_back(with_slash(dir));
    }
    return out;
}

}  // namespace

u32 action_create_folders(ActionContext& ctx) {
    Session& s = ctx.session();
    install_file_state_resolvers(s);
    // A system-changing action outside InstallInitialize..InstallFinalize is
    // 1603 even with nothing to do (msi_engine_session_probe).
    if (!ctx.scripting()) return kActionFailure;
    if (s.package() == nullptr) return kActionFailure;
    // Learn (CreateFolders): for components that run locally and from source.
    auto folders = std::make_shared<std::vector<std::string>>(create_folder_paths(s, installing));
    if (folders->empty()) return kActionSuccess;
    note_lock_permissions(s, "CreateFolder", "CreateFolders");
    return schedule_with_journal(ctx, "CreateFolders", [folders](const OpEnv& env, Journal& journal) -> u32 {
        for (const std::string& f : *folders) {
            if (!env.tell(kMsgActionData, folder_action_data(f))) return kActionUserExit;
            bool skipped = false;
            const u32 rc = with_retry(env, false, [&]() -> std::optional<Record> {
                const auto e = journal.ensure_dir(f);
                if (e == capsule::VfsError::None) return std::nullopt;
                if (e == capsule::VfsError::NotADirectory) return error_record(kErrFileIsDir, {f});
                return error_record(kErrCreateDir, {f});
            }, skipped);
            if (rc != kActionSuccess) return rc;
        }
        return kActionSuccess;
    });
}

u32 action_remove_folders(ActionContext& ctx) {
    Session& s = ctx.session();
    install_file_state_resolvers(s);
    // A system-changing action outside InstallInitialize..InstallFinalize is
    // 1603 even with nothing to do (msi_engine_session_probe).
    if (!ctx.scripting()) return kActionFailure;
    if (s.package() == nullptr) return kActionFailure;
    auto folders = std::make_shared<std::vector<std::string>>(create_folder_paths(s, removing));
    if (folders->empty()) return kActionSuccess;
    return schedule_with_journal(ctx, "RemoveFolders", [folders](const OpEnv& env, Journal& journal) -> u32 {
        for (const std::string& f : *folders) {
            if (!env.tell(kMsgActionData, folder_action_data(f))) return kActionUserExit;
            (void)journal.remove_empty_dir(f);  // Learn: removed only if empty
        }
        return kActionSuccess;
    });
}

// ==== PatchFiles / BindImage / ResolveSource ==============================================

u32 action_patch_files(ActionContext& ctx) {
    Session& s = ctx.session();
    install_file_state_resolvers(s);
    // A system-changing action outside InstallInitialize..InstallFinalize is
    // 1603 even with nothing to do (msi_engine_session_probe).
    if (!ctx.scripting()) return kActionFailure;
    const Table* t = s.package() ? s.package()->table("Patch") : nullptr;
    if (t == nullptr) return kActionSuccess;
    const Columns c(t, {"File_"});
    for (const auto& row : t->rows) {
        const FileRow* f = find_file_row(s, c.cell(row, 0));
        const ComponentInfo* comp = f ? component(s, f->component) : nullptr;
        if (comp == nullptr || !installing_local(*comp)) continue;
        // Applying a Patch-table delta needs the PatchAPI (mspatcha) delta
        // engine, which Vitreo does not have.
        log_unimplemented("PatchFiles", "binary delta patching of installed files (mspatcha)");
        return kActionFailure;
    }
    return kActionSuccess;
}

u32 action_bind_image(ActionContext& ctx) {
    // Measured (msi_files_probe, a PE and a non-PE BindImage row on Windows
    // 11 x64): BindImage logs its ActionStart and schedules no bind op; the
    // installed images are byte-identical to the package's.
    (void)ctx;
    return kActionSuccess;
}

u32 action_resolve_source(ActionContext& ctx) {
    Session& s = ctx.session();
    install_file_state_resolvers(s);
    // Measured: ResolveSource sets SourceDir and SOURCEDIR to the package's
    // folder (trailing '\') and SourcedirProduct to the ProductCode.
    std::string dir = s.property("SourceDir");
    if (dir.empty()) {
        std::string db = s.property("OriginalDatabase");
        if (db.empty()) db = s.property("DATABASE");
        dir = parent_dir(db);
        if (dir.empty()) {
            log_unimplemented("ResolveSource", "source lookup through the product's registered source list");
            return kActionFailure;
        }
        s.set_property("SourceDir", with_slash(dir));
    }
    if (s.property("SOURCEDIR").empty()) s.set_property("SOURCEDIR", with_slash(dir));
    if (s.property("SourcedirProduct").empty()) s.set_property("SourcedirProduct", s.property("ProductCode"));
    return kActionSuccess;
}

void register_file_actions(ActionRegistry& r) {
    r.add("InstallFiles", action_install_files);
    r.add("RemoveFiles", action_remove_files);
    r.add("MoveFiles", action_move_files);
    r.add("DuplicateFiles", action_duplicate_files);
    r.add("RemoveDuplicateFiles", action_remove_duplicate_files);
    r.add("CreateFolders", action_create_folders);
    r.add("RemoveFolders", action_remove_folders);
    r.add("PatchFiles", action_patch_files);
    r.add("BindImage", action_bind_image);
    r.add("ResolveSource", action_resolve_source);
}

}  // namespace vitreo::winapi::msi
