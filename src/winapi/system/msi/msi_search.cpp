// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_search.cpp
/// @brief Windows Installer search area: AppSearch, CCPSearch, RMCCPSearch
/// with every locator, the .ini, environment, ODBC and service actions, and
/// AllocateRegistrySpace (the Registry-table actions are in msi_registry.cpp).
///
/// Contract: msi_search.hpp lists the Learn pages; the measured behaviour
/// comes from tools/oracle_probe/msi_search_probe.c (searches) and
/// msi_search_install_probe.c (.ini and environment), Windows 11, both
/// bitnesses, .expected beside them. Gaps: .docs/contracts/msi/search.md.

#include "winapi/system/msi/msi_search.hpp"
#include "winapi/system/msi/msi_search_internal.hpp"

#include "capsule/registry/registry.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/advapi32/services/scm.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/system/time.hpp"
#include "winapi/system/msi/msi.hpp"
#include "winapi/system/msi/msi_engine.hpp"
#include "winapi/system/msi/msi_files.hpp"
#include "winapi/system/msi/msi_register.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace vitreo::winapi::msi {

using capsule::RegHandle;
using capsule::RegType;
using capsule::RegValue;

// =====================================================================================
// Shared helpers (msi_search_internal.hpp)
// =====================================================================================

namespace search_detail {

std::wstring widen(std::string_view utf8) {
    std::wstring out;
    out.reserve(utf8.size());
    for (usize i = 0; i < utf8.size();) {
        const u8 c = static_cast<u8>(utf8[i]);
        u32 cp = c;
        usize len = 1;
        if (c >= 0xF0 && i + 3 < utf8.size()) {
            cp = ((c & 0x07u) << 18) | ((static_cast<u8>(utf8[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<u8>(utf8[i + 2]) & 0x3Fu) << 6) | (static_cast<u8>(utf8[i + 3]) & 0x3Fu);
            len = 4;
        } else if (c >= 0xE0 && i + 2 < utf8.size()) {
            cp = ((c & 0x0Fu) << 12) | ((static_cast<u8>(utf8[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<u8>(utf8[i + 2]) & 0x3Fu);
            len = 3;
        } else if (c >= 0xC0 && i + 1 < utf8.size()) {
            cp = ((c & 0x1Fu) << 6) | (static_cast<u8>(utf8[i + 1]) & 0x3Fu);
            len = 2;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(cp));
        }
        i += len;
    }
    return out;
}

std::string narrow(std::wstring_view wide) {
    std::string out;
    out.reserve(wide.size());
    for (usize i = 0; i < wide.size(); ++i) {
        u32 cp = static_cast<u32>(wide[i]) & 0xFFFFu;
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < wide.size()) {
            const u32 lo = static_cast<u32>(wide[i + 1]) & 0xFFFFu;
            if (lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::u16string to_u16(std::string_view utf8) {
    const std::wstring w = widen(utf8);
    return std::u16string(w.begin(), w.end());
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::optional<i64> parse_int(std::string_view s) {
    if (s.empty()) return std::nullopt;
    bool negative = false;
    usize i = 0;
    if (s[0] == '-' || s[0] == '+') {
        negative = s[0] == '-';
        i = 1;
    }
    if (i >= s.size()) return std::nullopt;
    i64 v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return std::nullopt;
        v = v * 10 + (s[i] - '0');
    }
    return negative ? -v : v;
}

TableView::TableView(Session& session, const char* name) {
    if (Package* p = session.package()) m_table = p->table(name);
}

std::string TableView::cell(usize row, std::string_view column) const {
    if (m_table == nullptr || row >= m_table->rows.size()) return {};
    const auto col = m_table->column_index(column);
    if (!col || *col >= m_table->rows[row].size()) return {};
    return m_table->rows[row][*col];
}

ComponentAction component_action(Session& session, const std::string& component) {
    const auto& resolve = session.item_states().component_action;
    if (!resolve) return ComponentAction::None;
    const std::optional<i32> state = resolve(component);
    if (!state) return ComponentAction::None;
    // INSTALLSTATE_LOCAL 3 / SOURCE 4 install; ABSENT 2 removes.
    if (*state == 3 || *state == 4) return ComponentAction::Install;
    if (*state == 2) return ComponentAction::Remove;
    return ComponentAction::None;
}

bool component_is_64bit(Session& session, const std::string& component) {
    TableView t(session, "Component");
    for (usize i = 0; i < t.size(); ++i) {
        if (t.cell(i, "Component") == component)
            return (t.integer(i, "Attributes").value_or(0) & 0x0100) != 0;  // msidbComponentAttributes64bit
    }
    return false;
}

bool per_machine(Session& session) {
    // ALLUSERS=1 is a per-machine install (Learn "ALLUSERS"); the engine
    // resolves ALLUSERS=2 to 1 or clears it before the execute sequence.
    const std::string all = session.property("ALLUSERS");
    return all == "1" || all == "2";
}

bool write_root(Session& session, i64 root, RegHandle& hive, std::wstring& prefix) {
    prefix.clear();
    switch (root) {
        case -1:
            hive = per_machine(session) ? capsule::HKEY_LOCAL_MACHINE : capsule::HKEY_CURRENT_USER;
            return true;
        case 0:
            hive = per_machine(session) ? capsule::HKEY_LOCAL_MACHINE : capsule::HKEY_CURRENT_USER;
            prefix = L"Software\\Classes";
            return true;
        case 1: hive = capsule::HKEY_CURRENT_USER; return true;
        case 2: hive = capsule::HKEY_LOCAL_MACHINE; return true;
        case 3: hive = capsule::HKEY_USERS; return true;
        default: return false;
    }
}

std::wstring hive_name(RegHandle hive) {
    switch (hive) {
        case capsule::HKEY_CLASSES_ROOT: return L"HKEY_CLASSES_ROOT";
        case capsule::HKEY_CURRENT_USER: return L"HKEY_CURRENT_USER";
        case capsule::HKEY_LOCAL_MACHINE: return L"HKEY_LOCAL_MACHINE";
        case capsule::HKEY_USERS: return L"HKEY_USERS";
        default: return L"";
    }
}

bool key_exists(capsule::Registry& reg, RegHandle hive, const std::wstring& path) {
    auto k = reg.open_key(hive, path, false);
    if (k.is_err()) return false;
    reg.close_key(k.value());
    return true;
}

std::optional<KeySnapshot> snapshot_key(capsule::Registry& reg, RegHandle hive, const std::wstring& path) {
    auto k = reg.open_key(hive, path, false);
    if (k.is_err()) return std::nullopt;
    KeySnapshot snap;
    const auto slash = path.find_last_of(L'\\');
    snap.name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (auto names = reg.enum_values(k.value()); names.is_ok()) {
        for (const auto& n : names.value()) {
            if (auto v = reg.query_value(k.value(), n); v.is_ok()) snap.values.emplace_back(n, v.value());
        }
    }
    std::vector<std::wstring> subkeys;
    if (auto subs = reg.enum_keys(k.value()); subs.is_ok()) subkeys = subs.value();
    reg.close_key(k.value());
    for (const auto& sub : subkeys) {
        if (auto child = snapshot_key(reg, hive, path + L"\\" + sub)) snap.children.push_back(std::move(*child));
    }
    return snap;
}

void restore_key(capsule::Registry& reg, RegHandle hive, const std::wstring& path, const KeySnapshot& snap) {
    auto k = reg.open_key(hive, path, true);
    if (k.is_err()) return;
    for (const auto& [name, value] : snap.values) (void)reg.set_value(k.value(), name, value);
    reg.close_key(k.value());
    for (const auto& child : snap.children) restore_key(reg, hive, path + L"\\" + child.name, child);
}

bool delete_key_tree(capsule::Registry& reg, RegHandle hive, const std::wstring& path) {
    auto k = reg.open_key(hive, path, false);
    if (k.is_err()) return false;
    std::vector<std::wstring> subkeys;
    if (auto subs = reg.enum_keys(k.value()); subs.is_ok()) subkeys = subs.value();
    reg.close_key(k.value());
    for (const auto& sub : subkeys) delete_key_tree(reg, hive, path + L"\\" + sub);
    const auto slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) return reg.delete_key(hive, path).is_ok();
    auto parent = reg.open_key(hive, path.substr(0, slash), false);
    if (parent.is_err()) return false;
    const bool ok = reg.delete_key(parent.value(), path.substr(slash + 1)).is_ok();
    reg.close_key(parent.value());
    return ok;
}

void prune_empty_keys(capsule::Registry& reg, RegHandle hive, std::wstring path) {
    while (!path.empty()) {
        const auto slash = path.find_last_of(L'\\');
        if (slash == std::wstring::npos) return;  // never a key directly below the hive
        auto k = reg.open_key(hive, path, false);
        if (k.is_err()) return;
        const auto values = reg.enum_values(k.value());
        const auto subs = reg.enum_keys(k.value());
        reg.close_key(k.value());
        if (values.is_err() || subs.is_err() || !values.value().empty() || !subs.value().empty()) return;
        auto parent = reg.open_key(hive, path.substr(0, slash), false);
        if (parent.is_err()) return;
        const bool ok = reg.delete_key(parent.value(), path.substr(slash + 1)).is_ok();
        reg.close_key(parent.value());
        if (!ok) return;
        path.resize(slash);
    }
}

std::vector<std::wstring> missing_keys(capsule::Registry& reg, RegHandle hive, const std::wstring& path) {
    std::vector<std::wstring> missing;
    usize pos = 0;
    while (pos <= path.size()) {
        const usize slash = path.find(L'\\', pos);
        const std::wstring prefix = path.substr(0, slash);
        if (!prefix.empty() && !key_exists(reg, hive, prefix)) missing.push_back(prefix);
        if (slash == std::wstring::npos) break;
        pos = slash + 1;
    }
    return missing;
}

i32 OpEnv::message(u32 type, const Record& record) {
    if (session != nullptr && session->engine() != nullptr) return session->engine()->message(type, record);
    if (services.ui != nullptr && session != nullptr) return services.ui->message(*session, type, record);
    return 0;
}

namespace {
Record make_record(const std::vector<std::string>& fields, std::optional<u32> number) {
    const u32 offset = number ? 1u : 0u;
    Record r(static_cast<u32>(fields.size()) + offset);
    if (number) {
        r.fields[1].kind = Record::Kind::Integer;
        r.fields[1].integer = static_cast<i32>(*number);
    }
    for (usize i = 0; i < fields.size(); ++i) {
        auto& f = r.fields[i + 1 + offset];
        f.kind = Record::Kind::String;
        f.text = to_u16(fields[i]);
    }
    return r;
}
}  // namespace

void action_data(OpEnv& env, const std::vector<std::string>& fields) {
    (void)env.message(kMsgActionData, make_record(fields, std::nullopt));
}

i32 error_message(OpEnv& env, u32 error, const std::vector<std::string>& fields) {
    return env.message(kMsgError, make_record(fields, error));
}

namespace {
thread_local std::vector<ScriptOp>* t_collect = nullptr;
}  // namespace

void schedule(ActionContext& ctx, ScriptOp op) {
    if (t_collect != nullptr) t_collect->push_back(std::move(op));
    else ctx.schedule(std::move(op));
}

bool require_script(ActionContext& ctx, std::string_view action) {
    if (t_collect != nullptr || ctx.scripting()) return true;
    // Engine contract (msi_engine.hpp): a system-changing action outside
    // InstallInitialize..InstallFinalize fails with 1603.
    (void)action;
    return false;
}

}  // namespace search_detail

using namespace search_detail;

namespace {

constexpr i32 kIdIgnore = 5;  // IDIGNORE from the Error dialog / external UI

// =====================================================================================
// File system
// =====================================================================================

[[nodiscard]] std::string strip_slash(std::string path) {
    while (path.size() > 3 && (path.back() == '\\')) path.pop_back();
    if (path.size() == 2 && path[1] == ':') path.push_back('\\');
    return path;
}

[[nodiscard]] std::string with_slash(std::string path) {
    if (path.empty() || path.back() != '\\') path.push_back('\\');
    return path;
}

[[nodiscard]] bool is_absolute(std::string_view p) {
    return (p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':') ||
           (p.size() >= 2 && p[0] == '\\' && p[1] == '\\');
}

[[nodiscard]] std::optional<capsule::FileInfo> stat_path(capsule::Vfs* vfs, const std::string& path) {
    if (vfs == nullptr || path.empty()) return std::nullopt;
    // Windows Installer paths use backslashes; a forward slash names no
    // directory (probe DR_FWDSLASH).
    if (path.find('/') != std::string::npos) return std::nullopt;
    auto st = vfs->stat(strip_slash(path));
    if (st.is_err()) return std::nullopt;
    return st.value();
}

[[nodiscard]] bool is_dir(const capsule::FileInfo& info) {
    return (static_cast<u32>(info.attributes) & static_cast<u32>(capsule::FileAttributes::Directory)) != 0;
}

[[nodiscard]] bool directory_exists(capsule::Vfs* vfs, const std::string& path) {
    const auto st = stat_path(vfs, path);
    return st && is_dir(*st);
}

[[nodiscard]] bool file_exists(capsule::Vfs* vfs, const std::string& path) {
    const auto st = stat_path(vfs, path);
    return st && !is_dir(*st);
}

[[nodiscard]] std::string parent_of(const std::string& path) {
    const std::string p = strip_slash(path);
    if (p.size() <= 3) return with_slash(p);  // "C:\" is its own parent (probe FN_DRIVEROOT)
    const auto slash = p.find_last_of('\\');
    if (slash == std::string::npos) return {};
    return with_slash(p.substr(0, slash));
}

[[nodiscard]] std::optional<std::vector<u8>> read_file(capsule::Vfs* vfs, const std::string& path) {
    if (vfs == nullptr) return std::nullopt;
    auto opened = vfs->open(path, capsule::AccessMode::Read, capsule::ShareMode::Read,
                            capsule::CreationDisposition::OpenExisting);
    if (opened.is_err()) return std::nullopt;
    std::vector<u8> bytes;
    std::array<u8, 64 * 1024> chunk{};
    for (;;) {
        auto n = vfs->read(opened.value(), chunk);
        if (n.is_err() || n.value() == 0) break;
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n.value()));
    }
    (void)vfs->close(opened.value());
    return bytes;
}

[[nodiscard]] bool write_file(capsule::Vfs* vfs, const std::string& path, const std::vector<u8>& bytes) {
    if (vfs == nullptr) return false;
    auto opened = vfs->open(path, capsule::AccessMode::Write, capsule::ShareMode::None,
                            capsule::CreationDisposition::CreateAlways);
    if (opened.is_err()) return false;
    usize done = 0;
    bool ok = true;
    while (done < bytes.size()) {
        auto n = vfs->write(opened.value(), std::span<const u8>(bytes).subspan(done));
        if (n.is_err() || n.value() == 0) {
            ok = false;
            break;
        }
        done += n.value();
    }
    (void)vfs->close(opened.value());
    return ok;
}

// =====================================================================================
// Signatures
// =====================================================================================

[[nodiscard]] std::optional<SignatureFilter> load_signature(Session& session, const std::string& sig) {
    TableView t(session, "Signature");
    for (usize i = 0; i < t.size(); ++i) {
        if (t.cell(i, "Signature") != sig) continue;
        SignatureFilter f;
        const std::string name = t.cell(i, "FileName");
        const auto bar = name.find('|');
        if (bar == std::string::npos) {
            f.file_name = name;
        } else {
            f.short_name = name.substr(0, bar);
            f.file_name = name.substr(bar + 1);
        }
        f.min_version = parse_file_version(t.cell(i, "MinVersion"));
        f.max_version = parse_file_version(t.cell(i, "MaxVersion"));
        if (auto v = t.integer(i, "MinSize")) f.min_size = static_cast<u64>(*v);
        if (auto v = t.integer(i, "MaxSize")) f.max_size = static_cast<u64>(*v);
        if (auto v = t.integer(i, "MinDate")) f.min_date = static_cast<u32>(*v);
        if (auto v = t.integer(i, "MaxDate")) f.max_date = static_cast<u32>(*v);
        const std::string langs = t.cell(i, "Languages");
        usize pos = 0;
        while (pos < langs.size()) {
            const usize comma = langs.find(',', pos);
            const std::string item = langs.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (auto v = parse_int(item)) f.languages.push_back(static_cast<u16>(*v));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        return f;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<FileFacts> file_facts(capsule::Vfs* vfs, const std::string& path) {
    if (vfs == nullptr) return std::nullopt;
    ExistingFile e = probe_existing_file(*vfs, path);
    if (!e.exists) return std::nullopt;
    FileFacts f;
    if (auto st = stat_path(vfs, path)) f.size = st->size;
    // The Signature table compares the modification time in local time
    // (probe s_date_local_* match exactly, the UTC packing does not).
    TimeManager time;
    u64 local = 0;
    u16 dos_date = 0, dos_time = 0;
    if (time.file_time_to_local_file_time(e.modified, local) &&
        time.file_time_to_dos_date_time(local, dos_date, dos_time))
        f.local_dos_time = (static_cast<u32>(dos_date) << 16) | dos_time;
    f.version = e.info.version;
    f.languages = e.info.languages;
    return f;
}

}  // namespace

bool signature_matches(const SignatureFilter& filter, const FileFacts& file) {
    if (filter.min_size && file.size < *filter.min_size) return false;
    if (filter.max_size && file.size > *filter.max_size) return false;
    if (filter.min_date && file.local_dos_time < *filter.min_date) return false;
    if (filter.max_date && file.local_dos_time > *filter.max_date) return false;
    // An unversioned file compares as version 0 (probe: MinVersion 1.0 does
    // not match it, MaxVersion 99.0 does).
    const u64 version = file.version.value_or(0);
    if (filter.max_version && version > *filter.max_version) return false;
    if (filter.min_version) {
        if (version < *filter.min_version) return false;
        if (version == *filter.min_version) {
            // Languages count only at a version equal to MinVersion (Learn,
            // probe s_k_*): every listed language must be the file's; a null
            // Languages matches only a file without a language.
            if (filter.languages.empty()) {
                const bool has_language = std::any_of(file.languages.begin(), file.languages.end(),
                                                      [](u16 l) { return l != 0; });
                if (has_language) return false;
            } else {
                for (const u16 want : filter.languages) {
                    if (std::find(file.languages.begin(), file.languages.end(), want) == file.languages.end())
                        return false;
                }
            }
        }
    }
    return true;
}

namespace {

/// @p dir (trailing '\') holds a file matching @p f: its path spelled with
/// the Signature FileName (probe S_CASE).
[[nodiscard]] std::optional<std::string> match_in_dir(capsule::Vfs* vfs, const std::string& dir,
                                                      const SignatureFilter& f) {
    for (const std::string* name : {&f.file_name, &f.short_name}) {
        if (name->empty()) continue;
        const std::string candidate = with_slash(dir) + *name;
        if (!file_exists(vfs, candidate)) continue;
        const auto facts = file_facts(vfs, candidate);
        if (facts && signature_matches(f, *facts)) return with_slash(dir) + f.file_name;
    }
    return std::nullopt;
}

/// Breadth-first search below @p dir to @p depth levels.
[[nodiscard]] std::optional<std::string> search_tree(capsule::Vfs* vfs, const std::string& dir,
                                                     const SignatureFilter& f, i64 depth) {
    std::deque<std::pair<std::string, i64>> queue{{with_slash(dir), 0}};
    while (!queue.empty()) {
        auto [current, level] = queue.front();
        queue.pop_front();
        if (auto hit = match_in_dir(vfs, current, f)) return hit;
        if (level >= depth || vfs == nullptr) continue;
        auto entries = vfs->list_dir(strip_slash(current));
        if (entries.is_err()) continue;
        for (const auto& e : entries.value()) {
            if (e.name == "." || e.name == ".." || !is_dir(e.info)) continue;
            queue.emplace_back(with_slash(current + e.name), level + 1);
        }
    }
    return std::nullopt;
}

/// A path read from the registry or an .ini file: surrounding quotes and
/// anything after the closing quote dropped (probe *_FILEQUOTED,
/// D_DIRQUOTED).
[[nodiscard]] std::string clean_path(std::string text) {
    if (!text.empty() && text[0] == '"') {
        const auto close = text.find('"', 1);
        text = text.substr(1, close == std::string::npos ? std::string::npos : close - 1);
    }
    return text;
}

class Searcher {
public:
    Searcher(Session& session, Services& services) : m_session(session), m_services(services) {}

    std::optional<std::string> resolve(const std::string& sig, int depth = 0) {
        if (depth > 32) return std::nullopt;  // a Parent cycle
        const std::optional<SignatureFilter> filter = load_signature(m_session, sig);
        const SignatureFilter* f = filter ? &*filter : nullptr;
        if (auto r = comp_locator(sig, f)) return r;
        if (auto r = reg_locator(sig, f)) return r;
        if (auto r = ini_locator(sig, f)) return r;
        if (auto r = dr_locator(sig, f, depth)) return r;
        return std::nullopt;
    }

    /// RMCCPSearch resolution: relative DrLocator paths would be searched on
    /// the removable volume CCP_DRIVE names; every other locator resolves as
    /// usual (probe rm-* lines).
    bool removable_mode{false};

private:
    capsule::Vfs* vfs() { return m_services.vfs; }

    /// Interpret a located path by locator type (shared by the registry,
    /// .ini and component locators; probe D_/FN_/FS_/DS_ and I_ lines).
    std::optional<std::string> located(const std::string& path, u32 type, const SignatureFilter* f) {
        if (path.empty()) return std::nullopt;
        if (f != nullptr) {
            if (type == kLocatorTypeDirectory) return match_in_dir(vfs(), path, *f);
            // File (and raw) locator with a signature: the value is the file.
            if (!file_exists(vfs(), path)) return std::nullopt;
            const std::string leaf = path.substr(path.find_last_of('\\') + 1);
            if (!iequals(leaf, f->file_name) && !iequals(leaf, f->short_name)) return std::nullopt;
            const auto facts = file_facts(vfs(), path);
            if (!facts || !signature_matches(*f, *facts)) return std::nullopt;
            return path;
        }
        if (type == kLocatorTypeDirectory) {
            if (!directory_exists(vfs(), path)) return std::nullopt;
            return with_slash(strip_slash(path));
        }
        // File locator without a signature: the directory holding the path.
        const std::string parent = parent_of(path);
        if (parent.empty() || !directory_exists(vfs(), parent)) return std::nullopt;
        return parent;
    }

    std::optional<std::string> comp_locator(const std::string& sig, const SignatureFilter* f) {
        TableView t(m_session, "CompLocator");
        for (usize i = 0; i < t.size(); ++i) {
            if (t.cell(i, "Signature_") != sig) continue;
            const u32 type = static_cast<u32>(t.integer(i, "Type").value_or(kLocatorTypeFileName));
            const auto key_path = installed_key_path(t.cell(i, "ComponentId"));
            if (!key_path) return std::nullopt;
            if (type == kLocatorTypeDirectory) {
                if (f != nullptr) return match_in_dir(vfs(), *key_path, *f);
                if (!directory_exists(vfs(), *key_path)) return std::nullopt;
                return with_slash(strip_slash(*key_path));
            }
            if (!file_exists(vfs(), *key_path)) return std::nullopt;
            if (f == nullptr) return parent_of(*key_path);
            // With a signature the key file is tested against the filters,
            // not the FileName (probe C_FILE_BADSIG).
            const auto facts = file_facts(vfs(), *key_path);
            if (!facts || !signature_matches(*f, *facts)) return std::nullopt;
            return *key_path;
        }
        return std::nullopt;
    }

    /// The local key path of an installed component (MsiLocateComponent):
    /// the first client's registered key path, over every context, from the
    /// installed-products database (msi_register.hpp). A registry key path
    /// ("02:\...") or source form is not a file or directory location.
    std::optional<std::string> installed_key_path(const std::string& component_id) {
        if (m_services.registry == nullptr || component_id.size() != 38) return std::nullopt;
        const std::string sid = m_services.manager != nullptr ? narrow(m_services.manager->user_sid()) : std::string{};
        const ProductDatabase db(*m_services.registry, sid);
        for (const auto& [instance, path] : db.component_clients(component_id, kContextAll)) {
            (void)instance;
            if (path.size() < 3 || path[1] != ':' || !std::isalpha(static_cast<unsigned char>(path[0]))) continue;
            return path;
        }
        return std::nullopt;
    }

    std::optional<std::string> reg_locator(const std::string& sig, const SignatureFilter* f) {
        TableView t(m_session, "RegLocator");
        capsule::Registry* reg = m_services.registry;
        for (usize i = 0; i < t.size(); ++i) {
            if (t.cell(i, "Signature_") != sig) continue;
            if (reg == nullptr) return std::nullopt;
            RegHandle hive = 0;
            switch (t.integer(i, "Root").value_or(-99)) {
                case 0: hive = capsule::HKEY_CLASSES_ROOT; break;
                case 1: hive = capsule::HKEY_CURRENT_USER; break;
                case 2: hive = capsule::HKEY_LOCAL_MACHINE; break;
                case 3: hive = capsule::HKEY_USERS; break;
                default: return std::nullopt;
            }
            // A null Type reads as 0, a directory (probe T_NULL, T_NULL_NOSIG),
            // although Learn says 1. msidbLocatorType64bit (0x10) searches the
            // 64-bit registry view; without it the installer searches the
            // 32-bit view (Learn, "RegLocator Table").
            const u32 raw_type = static_cast<u32>(t.integer(i, "Type").value_or(0));
            const u32 type = raw_type & 0x0F;
            const capsule::RegView view = (raw_type & 0x10) != 0 ? capsule::RegView::Native64
                                                                 : capsule::RegView::Wow32;
            const std::wstring key = widen(m_session.format(t.cell(i, "Key")));
            const std::wstring name = widen(m_session.format(t.cell(i, "Name")));
            auto k = reg->open_key(reg->view_root(hive, view), key, false);
            if (k.is_err()) return std::nullopt;
            auto v = reg->query_value(k.value(), name);
            reg->close_key(k.value());
            if (v.is_err()) return std::nullopt;
            const RegValue& value = v.value();
            const auto expand = [this](const std::string& n) { return m_session.environment(n); };
            if (type == kLocatorTypeRawValue && f == nullptr) return format_raw_registry_value(value, expand);
            // Only string values name a path (probe D_DIRDWORD).
            if (value.type != RegType::Sz && value.type != RegType::ExpandSz) return std::nullopt;
            auto text = format_raw_registry_value(value, expand);
            if (!text) return std::nullopt;
            std::string path = *text;
            if (value.type == RegType::Sz && path.rfind("##", 0) == 0) path.erase(0, 1);
            return located(clean_path(path), type == kLocatorTypeDirectory ? kLocatorTypeDirectory
                                                                           : kLocatorTypeFileName, f);
        }
        return std::nullopt;
    }

    std::optional<std::string> ini_locator(const std::string& sig, const SignatureFilter* f) {
        TableView t(m_session, "IniLocator");
        for (usize i = 0; i < t.size(); ++i) {
            if (t.cell(i, "Signature_") != sig) continue;
            // The .ini file is in the Windows folder (Learn "IniLocator Table").
            const std::string windows = m_session.property("WindowsFolder");
            if (windows.empty()) {
                log_unimplemented("AppSearch", "IniLocator without a WindowsFolder property");
                return std::nullopt;
            }
            const auto bytes = read_file(vfs(), with_slash(windows) + t.cell(i, "FileName"));
            if (!bytes) return std::nullopt;
            const IniDocument doc = IniDocument::parse(std::string(bytes->begin(), bytes->end()));
            auto value = doc.get(t.cell(i, "Section"), t.cell(i, "Key"));
            if (!value || value->empty()) return std::nullopt;
            const i64 field = t.integer(i, "Field").value_or(0);
            if (field > 0) {
                // The n-th comma-separated field, untrimmed (probe I_RAW_F2).
                std::string_view rest = *value;
                for (i64 n = 1;; ++n) {
                    const usize comma = rest.find(',');
                    if (n == field) {
                        value = std::string(rest.substr(0, comma));
                        break;
                    }
                    if (comma == std::string_view::npos) return std::nullopt;
                    rest.remove_prefix(comma + 1);
                }
                if (value->empty()) return std::nullopt;
            }
            // A null Type is 1, a file (probe I_TYPE_NULL).
            const u32 type = static_cast<u32>(t.integer(i, "Type").value_or(kLocatorTypeFileName)) & 0x0F;
            if (type == kLocatorTypeRawValue && f == nullptr) return value;
            return located(clean_path(*value), type == kLocatorTypeDirectory ? kLocatorTypeDirectory
                                                                             : kLocatorTypeFileName, f);
        }
        return std::nullopt;
    }

    std::optional<std::string> dr_locator(const std::string& sig, const SignatureFilter* f, int depth) {
        TableView t(m_session, "DrLocator");
        for (usize i = 0; i < t.size(); ++i) {
            if (t.cell(i, "Signature_") != sig) continue;
            const std::string parent = t.cell(i, "Parent");
            // Path is formatted (probe DR_FMT, DR_ENVFMT).
            const std::string path = m_session.format(t.cell(i, "Path"));
            const i64 levels = t.integer(i, "Depth").value_or(0);
            std::vector<std::string> bases;
            if (!parent.empty()) {
                auto p = resolve(parent, depth + 1);
                if (!p) continue;
                // A parent found as a file contributes its directory
                // (probe DR_PARENT_ISFILE); an absolute Path below a parent
                // names nothing (probe DR_CHILDABS).
                std::string dir = (p->back() == '\\') ? *p : parent_of(*p);
                if (is_absolute(path)) continue;
                bases.push_back(path.empty() ? dir : with_slash(dir) + path);
            } else if (is_absolute(path)) {
                bases.push_back(path);
            } else if (removable_mode) {
                // RMCCPSearch: relative paths name a location on the
                // removable volume in CCP_DRIVE. Every Vitreo volume is a
                // fixed drive, so no removable volume holds them (probe
                // rm-relative: a fixed CCP_DRIVE finds nothing).
                continue;
            } else if (vfs() != nullptr) {
                // All fixed drives, in drive order (probe DR_RELATIVE,
                // DR_NOPATH = "C:\").
                std::vector<char> drives = vfs()->mounted_drives();
                std::sort(drives.begin(), drives.end());
                for (const char d : drives) bases.push_back(std::string(1, d) + ":\\" + path);
            }
            for (const auto& base : bases) {
                if (f == nullptr) {
                    if (directory_exists(vfs(), base)) return with_slash(strip_slash(base));
                    continue;
                }
                if (!directory_exists(vfs(), base)) continue;  // a file Path is no search root (DR_FILE_AT_PATH)
                if (auto hit = search_tree(vfs(), base, *f, levels)) return hit;
            }
        }
        return std::nullopt;
    }

    Session& m_session;
    Services& m_services;
};

// =====================================================================================
// AppSearch / CCPSearch / RMCCPSearch
// =====================================================================================

/// Which searches ran in this session (the client's are carried to the
/// server session: Learn, the execute sequence skips a search the UI
/// sequence already ran).
struct SearchState {
    bool app_search_ran{false}, ccp_search_ran{false}, rmccp_search_ran{false};
    bool skip_app_search{false}, skip_ccp_search{false}, skip_rmccp_search{false};
};

void install_handoff_hook() {
    static std::once_flag once;
    std::call_once(once, [] {
        add_execute_handoff_hook([](Session& client, Session& server) {
            const SearchState& c = client.state<SearchState>();
            SearchState& s = server.state<SearchState>();
            s.skip_app_search = c.app_search_ran;
            s.skip_ccp_search = c.ccp_search_ran;
            s.skip_rmccp_search = c.rmccp_search_ran;
        });
    });
}

/// True (and CCP_Success = 1) when a CCPSearch signature is found.
bool ccp_search(ActionContext& ctx, bool removable) {
    Session& session = ctx.session();
    TableView t(session, "CCPSearch");
    Searcher searcher(session, ctx.services());
    searcher.removable_mode = removable;
    for (usize i = 0; i < t.size(); ++i) {
        if (searcher.resolve(t.cell(i, "Signature_"))) {
            session.set_property("CCP_Success", "1");
            return true;
        }
    }
    return false;
}

}  // namespace

std::optional<std::string> resolve_signature(Session& session, Services& services, const std::string& signature) {
    Searcher searcher(session, services);
    return searcher.resolve(signature);
}

u32 action_app_search(ActionContext& ctx) {
    Session& session = ctx.session();
    SearchState& st = session.state<SearchState>();
    if (st.skip_app_search) return kActionSuccess;
    st.app_search_ran = true;
    TableView t(session, "AppSearch");
    Searcher searcher(session, ctx.services());
    OpEnv env(ctx);
    // Rows in table order; each found signature sets the property, a miss
    // leaves it as it was (probe PRESET, TWOSIG, TWOSIGMISS).
    for (usize i = 0; i < t.size(); ++i) {
        const std::string property = t.cell(i, "Property");
        const std::string signature = t.cell(i, "Signature_");
        action_data(env, {property, signature});
        if (auto found = searcher.resolve(signature)) session.set_property(property, *found);
    }
    return kActionSuccess;
}

u32 action_ccp_search(ActionContext& ctx) {
    SearchState& st = ctx.session().state<SearchState>();
    if (st.skip_ccp_search) return kActionSuccess;
    st.ccp_search_ran = true;
    (void)ccp_search(ctx, false);
    return kActionSuccess;
}

u32 action_rmccp_search(ActionContext& ctx) {
    Session& session = ctx.session();
    SearchState& st = session.state<SearchState>();
    if (st.skip_rmccp_search) return kActionSuccess;
    st.rmccp_search_ran = true;
    // Without CCP_DRIVE, and when no qualifying product is found (Windows
    // would prompt for the removable volume), the action fails with 1603
    // (probe "RMCCPSearch no-drive", rm-missing).
    if (session.property("CCP_DRIVE").empty()) return kActionFailure;
    return ccp_search(ctx, true) ? kActionSuccess : kActionFailure;
}

// =====================================================================================
// .ini files
// =====================================================================================

namespace {
[[nodiscard]] std::string trim(std::string_view s) {
    usize b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return std::string(s.substr(b, e - b));
}
[[nodiscard]] std::optional<std::string> section_name(const std::string& line) {
    const std::string t = trim(line);
    if (t.size() < 2 || t.front() != '[') return std::nullopt;
    const auto close = t.find(']');
    if (close == std::string::npos) return std::nullopt;
    return trim(std::string_view(t).substr(1, close - 1));
}
[[nodiscard]] std::optional<std::pair<std::string, std::string>> key_value(const std::string& line) {
    const std::string t = trim(line);
    if (t.empty() || t[0] == ';' || t[0] == '[') return std::nullopt;
    const auto eq = t.find('=');
    if (eq == std::string::npos) return std::nullopt;
    return std::make_pair(trim(std::string_view(t).substr(0, eq)), trim(std::string_view(t).substr(eq + 1)));
}
}  // namespace

IniDocument IniDocument::parse(const std::string& text) {
    IniDocument doc;
    usize pos = 0;
    while (pos < text.size()) {
        const usize nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        doc.m_lines.push_back(Line{std::move(line)});
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return doc;
}

std::string IniDocument::serialize() const {
    std::string out;
    for (const auto& l : m_lines) {
        out += l.text;
        out += "\r\n";
    }
    return out;
}

std::optional<usize> IniDocument::find_section(const std::string& section) const {
    for (usize i = 0; i < m_lines.size(); ++i) {
        if (auto name = section_name(m_lines[i].text); name && iequals(*name, section)) return i;
    }
    return std::nullopt;
}

std::optional<usize> IniDocument::find_key(usize section_line, const std::string& key) const {
    for (usize i = section_line + 1; i < m_lines.size(); ++i) {
        if (section_name(m_lines[i].text)) break;
        if (auto kv = key_value(m_lines[i].text); kv && iequals(kv->first, key)) return i;
    }
    return std::nullopt;
}

std::optional<std::string> IniDocument::get(const std::string& section, const std::string& key) const {
    const auto s = find_section(section);
    if (!s) return std::nullopt;
    const auto k = find_key(*s, key);
    if (!k) return std::nullopt;
    std::string value = key_value(m_lines[*k].text)->second;
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\'')))
        value = value.substr(1, value.size() - 2);
    return value;
}

void IniDocument::set(const std::string& section, const std::string& key, const std::string& value) {
    const auto s = find_section(section);
    if (!s) {
        // A new section goes at the end of the file (probe [NewSec]).
        m_lines.push_back(Line{"[" + section + "]"});
        m_lines.push_back(Line{key + "=" + value});
        return;
    }
    if (const auto k = find_key(*s, key)) {
        m_lines[*k].text = key_value(m_lines[*k].text)->first + "=" + value;
        return;
    }
    // A new key follows the section's last key (probe k_addnew).
    usize insert_at = *s + 1;
    for (usize i = *s + 1; i < m_lines.size(); ++i) {
        if (section_name(m_lines[i].text)) break;
        if (key_value(m_lines[i].text)) insert_at = i + 1;
    }
    m_lines.insert(m_lines.begin() + static_cast<std::ptrdiff_t>(insert_at), Line{key + "=" + value});
}

bool IniDocument::remove(const std::string& section, const std::string& key) {
    const auto s = find_section(section);
    if (!s) return false;
    const auto k = find_key(*s, key);
    if (!k) return false;
    m_lines.erase(m_lines.begin() + static_cast<std::ptrdiff_t>(*k));
    // A section left without keys goes too (Learn "RemoveIniFile Table";
    // probe [Other]).
    usize end = *s + 1;
    bool has_key = false;
    for (; end < m_lines.size(); ++end) {
        if (section_name(m_lines[end].text)) break;
        if (key_value(m_lines[end].text)) has_key = true;
    }
    if (!has_key)
        m_lines.erase(m_lines.begin() + static_cast<std::ptrdiff_t>(*s),
                      m_lines.begin() + static_cast<std::ptrdiff_t>(end));
    return true;
}

namespace {

enum IniMode : i64 { kIniAddLine = 0, kIniCreateLine = 1, kIniRemoveLine = 2, kIniAddTag = 3, kIniRemoveTag = 4 };

struct IniOp {
    std::string file;  ///< full path
    std::string file_name, folder;
    std::string section, key, value;
    i64 mode{kIniAddLine};
};

[[nodiscard]] std::vector<std::string> split_tags(const std::string& v) {
    std::vector<std::string> tags;
    usize pos = 0;
    for (;;) {
        const usize comma = v.find(',', pos);
        tags.push_back(v.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return tags;
}

u32 run_ini_op(OpEnv& env, const IniOp& op, std::function<void()>& undo) {
    capsule::Vfs* vfs = env.services.vfs;
    if (vfs == nullptr) {
        log_unimplemented("WriteIniValues", "no capsule VFS wired into the engine");
        return kActionFailure;
    }
    action_data(env, {op.file, op.section, op.key, op.value});
    const auto prior = read_file(vfs, op.file);
    IniDocument doc = IniDocument::parse(prior ? std::string(prior->begin(), prior->end()) : std::string());
    const std::optional<std::string> current = doc.get(op.section, op.key);
    switch (op.mode) {
        case kIniAddLine: doc.set(op.section, op.key, op.value); break;
        case kIniCreateLine:
            if (!current) doc.set(op.section, op.key, op.value);
            break;
        case kIniAddTag:
            // The tag is appended even when the entry already has it
            // (probe k_tag2 "a,b" + "b" = "a,b,b").
            doc.set(op.section, op.key, current && !current->empty() ? *current + "," + op.value : op.value);
            break;
        case kIniRemoveLine:
            if (!current) return kActionSuccess;
            doc.remove(op.section, op.key);
            break;
        case kIniRemoveTag: {
            if (!current) return kActionSuccess;
            std::vector<std::string> tags = split_tags(*current);
            const usize before = tags.size();
            tags.erase(std::remove_if(tags.begin(), tags.end(),
                                      [&](const std::string& t) { return iequals(trim(t), op.value); }),
                       tags.end());
            if (tags.size() == before) return kActionSuccess;
            if (tags.empty()) {
                doc.remove(op.section, op.key);
            } else {
                std::string joined;
                for (usize i = 0; i < tags.size(); ++i) joined += (i ? "," : "") + tags[i];
                doc.set(op.section, op.key, joined);
            }
            break;
        }
        default: return kActionSuccess;
    }
    const std::string text = doc.serialize();
    if (!write_file(vfs, op.file, std::vector<u8>(text.begin(), text.end()))) {
        // Learn Error 1913: Could not update the .ini file [2][3].
        if (error_message(env, 1913, {op.folder, op.file_name}) == kIdIgnore) return kActionSuccess;
        return kActionFailure;
    }
    undo = [vfs, file = op.file, prior] {
        if (prior) (void)write_file(vfs, file, *prior);
        else if (vfs != nullptr) (void)vfs->remove(file);
    };
    return kActionSuccess;
}

void schedule_ini(ActionContext& ctx, const char* action, IniOp op) {
    auto undo = std::make_shared<std::function<void()>>();
    ScriptOp script;
    script.action = action;
    script.run = [env = OpEnv(ctx), op, undo]() mutable -> u32 { return run_ini_op(env, op, *undo); };
    script.rollback = [undo] {
        if (*undo) (*undo)();
    };
    schedule(ctx, std::move(script));
}

/// IniFile / RemoveIniFile rows as ops; @p remap turns an IniFile mode into
/// its uninstall counterpart.
void schedule_ini_rows(ActionContext& ctx, const char* action, const char* table, ComponentAction gate,
                       bool uninstall) {
    Session& session = ctx.session();
    TableView t(session, table);
    for (usize i = 0; i < t.size(); ++i) {
        if (component_action(session, t.cell(i, "Component_")) != gate) continue;
        IniOp op;
        op.file_name = t.cell(i, "FileName");
        const std::string dir_property = t.cell(i, "DirProperty");
        op.folder = dir_property.empty() ? session.property("WindowsFolder") : session.property(dir_property);
        if (op.folder.empty()) continue;
        op.folder = with_slash(op.folder);
        op.file = op.folder + op.file_name;
        op.section = session.format(t.cell(i, "Section"));
        op.key = session.format(t.cell(i, "Key"));
        op.value = session.format(t.cell(i, "Value"));
        op.mode = t.integer(i, "Action").value_or(kIniAddLine);
        if (uninstall) {
            // Uninstall removes what IniFile wrote: lines for add/create,
            // the tag for add-tag (msi_search_install_probe "uninstalled").
            if (op.mode == kIniAddLine || op.mode == kIniCreateLine) op.mode = kIniRemoveLine;
            else if (op.mode == kIniAddTag) op.mode = kIniRemoveTag;
            else continue;
        }
        schedule_ini(ctx, action, std::move(op));
    }
}

}  // namespace

u32 action_write_ini_values(ActionContext& ctx) {
    if (!require_script(ctx, "WriteIniValues")) return kActionFailure;
    schedule_ini_rows(ctx, "WriteIniValues", "IniFile", ComponentAction::Install, false);
    return kActionSuccess;
}

u32 action_remove_ini_values(ActionContext& ctx) {
    if (!require_script(ctx, "RemoveIniValues")) return kActionFailure;
    schedule_ini_rows(ctx, "RemoveIniValues", "IniFile", ComponentAction::Remove, true);
    schedule_ini_rows(ctx, "RemoveIniValues", "RemoveIniFile", ComponentAction::Install, false);
    return kActionSuccess;
}

// =====================================================================================
// Environment
// =====================================================================================

namespace {

constexpr wchar_t kSystemEnvironment[] = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";

struct EnvRow {
    std::string name;
    bool set{false}, create{false}, remove_on_uninstall{false}, remove{false}, system{false};
    std::optional<std::string> value;  ///< the part to set (delimiter and [~] removed)
    bool append{false}, prepend{false};
    std::string delimiter;
};

[[nodiscard]] EnvRow parse_env_row(Session& session, const std::string& raw_name, const std::string& raw_value,
                                   bool value_null) {
    EnvRow r;
    usize i = 0;
    for (; i < raw_name.size(); ++i) {
        const char c = raw_name[i];
        if (c == '=') r.set = true;
        else if (c == '+') r.create = true;
        else if (c == '-') r.remove_on_uninstall = true;
        else if (c == '!') r.remove = true;
        else if (c == '*') r.system = true;
        else break;
    }
    r.name = raw_name.substr(i);
    if (value_null) return r;
    std::string v = session.format(raw_value);
    // "[~];Value" appends, "Value;[~]" prepends; the character next to
    // [~] is the delimiter (msi_search_install_probe script ops).
    if (!v.empty() && v.front() == '\0') {
        r.append = true;
        v.erase(0, 1);
        if (!v.empty()) {
            r.delimiter = v.substr(0, 1);
            v.erase(0, 1);
        }
    } else if (!v.empty() && v.back() == '\0') {
        r.prepend = true;
        v.pop_back();
        if (!v.empty()) {
            r.delimiter = v.substr(v.size() - 1);
            v.pop_back();
        }
    }
    r.value = v;
    return r;
}

[[nodiscard]] std::vector<std::string> split_by(const std::string& s, const std::string& delim) {
    std::vector<std::string> parts;
    if (delim.empty()) {
        parts.push_back(s);
        return parts;
    }
    usize pos = 0;
    for (;;) {
        const usize at = s.find(delim, pos);
        parts.push_back(s.substr(pos, at == std::string::npos ? std::string::npos : at - pos));
        if (at == std::string::npos) break;
        pos = at + delim.size();
    }
    return parts;
}

[[nodiscard]] std::string join_by(const std::vector<std::string>& parts, const std::string& delim) {
    std::string out;
    for (usize i = 0; i < parts.size(); ++i) {
        if (i) out += delim;
        out += parts[i];
    }
    return out;
}

struct EnvOp {
    EnvRow row;
    bool uninstall{false};
};

u32 run_env_op(OpEnv& env, const EnvOp& op, std::function<void()>& undo) {
    capsule::Registry* reg = env.services.registry;
    if (reg == nullptr) {
        log_unimplemented("WriteEnvironmentStrings", "no capsule registry wired into the engine");
        return kActionFailure;
    }
    const EnvRow& r = op.row;
    action_data(env, {r.name, r.value.value_or(""), op.uninstall || r.remove ? "remove" : "set"});
    const RegHandle hive = r.system ? capsule::HKEY_LOCAL_MACHINE : capsule::HKEY_CURRENT_USER;
    const std::wstring key = r.system ? std::wstring(kSystemEnvironment) : std::wstring(L"Environment");
    auto opened = reg->open_key(hive, key, true);
    if (opened.is_err()) {
        if (error_message(env, 1924, {r.name}) == kIdIgnore) return kActionSuccess;
        return kActionFailure;
    }
    const RegHandle h = opened.value();
    const std::wstring wname = widen(r.name);
    std::optional<RegValue> prior;
    if (auto q = reg->query_value(h, wname); q.is_ok()) prior = q.value();
    std::optional<std::string> current;
    if (prior) {
        if (const auto* s = std::get_if<std::wstring>(&prior->data)) current = narrow(*s);
    }
    const std::string part = r.value.value_or("");

    std::optional<std::string> result = current;  // nullopt = delete
    if (op.uninstall) {
        if (r.append || r.prepend) {
            if (!current) {
                reg->close_key(h);
                return kActionSuccess;
            }
            auto items = split_by(*current, r.delimiter);
            items.erase(std::remove(items.begin(), items.end(), part), items.end());
            result = items.empty() ? std::nullopt : std::optional<std::string>(join_by(items, r.delimiter));
        } else if (current && (part.empty() || iequals(*current, part))) {
            result = std::nullopt;
        }
    } else if (r.remove) {
        // '!' removes only a variable whose value matches (an empty Value
        // matches any): probe DEL / DELANY / DELMISMATCH.
        if (current && (part.empty() || iequals(*current, part))) result = std::nullopt;
    } else if (!r.value) {
        result = std::nullopt;  // a blank Value removes the variable (probe NULLSET)
    } else if (r.create && current) {
        // '+' leaves an existing variable alone (probe PLUS).
    } else if (r.append || r.prepend) {
        std::vector<std::string> items;
        if (current && !current->empty()) items = split_by(*current, r.delimiter);
        items.erase(std::remove(items.begin(), items.end(), part), items.end());
        if (r.append) items.push_back(part);
        else items.insert(items.begin(), part);
        result = join_by(items, r.delimiter);
    } else {
        result = part;
    }

    bool ok = true;
    if (result != current || (result && !prior)) {
        if (result) {
            // A value with '%' is stored expandable, as is one that already
            // was (probe EXP type 2, APP type 1).
            const bool expand = result->find('%') != std::string::npos ||
                                (prior && prior->type == RegType::ExpandSz);
            ok = reg->set_value(h, wname, RegValue{expand ? RegType::ExpandSz : RegType::Sz, widen(*result)})
                     .is_ok();
        } else if (prior) {
            ok = reg->delete_value(h, wname).is_ok();
        }
    }
    reg->close_key(h);
    if (!ok) {
        if (error_message(env, 1924, {r.name}) == kIdIgnore) return kActionSuccess;
        return kActionFailure;
    }
    undo = [reg, hive, key, wname, prior] {
        auto k = reg->open_key(hive, key, true);
        if (k.is_err()) return;
        if (prior) (void)reg->set_value(k.value(), wname, *prior);
        else (void)reg->delete_value(k.value(), wname);
        reg->close_key(k.value());
    };
    return kActionSuccess;
}

void schedule_env_rows(ActionContext& ctx, const char* action, bool uninstall) {
    Session& session = ctx.session();
    TableView t(session, "Environment");
    for (usize i = 0; i < t.size(); ++i) {
        const ComponentAction a = component_action(session, t.cell(i, "Component_"));
        if (a != (uninstall ? ComponentAction::Remove : ComponentAction::Install)) continue;
        EnvOp op;
        op.uninstall = uninstall;
        op.row = parse_env_row(session, t.cell(i, "Name"), t.cell(i, "Value"), t.null(i, "Value"));
        if (op.row.name.empty()) continue;
        if (uninstall) {
            // Uninstall undoes what the row set; '+' (create-only) rows and
            // '!' rows without '-' are left alone (msi_search_install_probe:
            // PLUSNEW stays, APP/PRE/EXP/SYS revert with or without '-').
            if (op.row.create) continue;
            if (op.row.remove && !op.row.remove_on_uninstall) continue;
            if (op.row.remove) op.uninstall = false;  // "!-": remove on uninstall too
        }
        auto undo = std::make_shared<std::function<void()>>();
        ScriptOp script;
        script.action = action;
        script.run = [env = OpEnv(ctx), op, undo]() mutable -> u32 { return run_env_op(env, op, *undo); };
        script.rollback = [undo] {
            if (*undo) (*undo)();
        };
        schedule(ctx, std::move(script));
    }
}

}  // namespace

u32 action_write_environment_strings(ActionContext& ctx) {
    if (!require_script(ctx, "WriteEnvironmentStrings")) return kActionFailure;
    schedule_env_rows(ctx, "WriteEnvironmentStrings", false);
    return kActionSuccess;
}

u32 action_remove_environment_strings(ActionContext& ctx) {
    if (!require_script(ctx, "RemoveEnvironmentStrings")) return kActionFailure;
    schedule_env_rows(ctx, "RemoveEnvironmentStrings", true);
    return kActionSuccess;
}

// =====================================================================================
// ODBC
// =====================================================================================

namespace {

/// Rows of the ODBC tables whose component is in @p gate.
[[nodiscard]] usize odbc_rows(Session& session, ComponentAction gate) {
    usize n = 0;
    for (const char* table : {"ODBCDriver", "ODBCTranslator", "ODBCDataSource"}) {
        TableView t(session, table);
        for (usize i = 0; i < t.size(); ++i)
            if (component_action(session, t.cell(i, "Component_")) == gate) ++n;
    }
    return n;
}

}  // namespace

u32 action_install_odbc(ActionContext& ctx) {
    if (!require_script(ctx, "InstallODBC")) return kActionFailure;
    if (odbc_rows(ctx.session(), ComponentAction::Install) == 0) return kActionSuccess;
    // Installing a driver, translator or data source goes through the ODBC
    // installer DLL (odbccp32: SQLInstallDriverEx / SQLConfigDataSource),
    // which Vitreo does not provide.
    log_unimplemented("InstallODBC", "ODBC driver/translator/data source (no odbccp32 driver manager)");
    OpEnv env(ctx);
    (void)error_message(env, 1918, {"120", "ODBC is not available", ""});
    return kActionFailure;
}

u32 action_remove_odbc(ActionContext& ctx) {
    if (!require_script(ctx, "RemoveODBC")) return kActionFailure;
    if (odbc_rows(ctx.session(), ComponentAction::Remove) == 0) return kActionSuccess;
    log_unimplemented("RemoveODBC", "ODBC driver/translator/data source (no odbccp32 driver manager)");
    OpEnv env(ctx);
    (void)error_message(env, 1917, {"120", "ODBC is not available", ""});
    return kActionFailure;
}

u32 action_set_odbc_folders(ActionContext& ctx) {
    // SetODBCFolders moves a new driver's directory to where an existing
    // driver of the same name is installed. Vitreo has no ODBC driver
    // manager, so no driver is installed and every new driver keeps its
    // authored directory: nothing to change.
    (void)ctx;
    return kActionSuccess;
}

// =====================================================================================
// Services
// =====================================================================================

namespace {

std::atomic<ScmManager*> g_scm{nullptr};

constexpr u32 kServiceNoChange = 0xFFFFFFFFu;
constexpr u32 kErrorControlVital = 0x8000;  // msidbServiceInstallErrorControlVital

[[nodiscard]] std::wstring service_key(const std::wstring& name) {
    return L"SYSTEM\\CurrentControlSet\\Services\\" + name;
}

/// The SCM to drive: the process's own, else one over the session registry.
class ScmAccess {
public:
    explicit ScmAccess(Services& services) {
        if (ScmManager* scm = g_scm.load()) {
            m_scm = scm;
        } else if (services.registry != nullptr && services.vfs != nullptr) {
            m_local = std::make_unique<ScmManager>(*services.registry, *services.vfs);
            m_scm = m_local.get();
        }
    }
    [[nodiscard]] ScmManager* get() { return m_scm; }

private:
    ScmManager* m_scm{nullptr};
    std::unique_ptr<ScmManager> m_local;
};

[[nodiscard]] std::vector<std::wstring> split_nul(const std::string& s) {
    std::vector<std::wstring> out;
    usize pos = 0;
    while (pos < s.size()) {
        const usize nul = s.find('\0', pos);
        const std::string item = s.substr(pos, nul == std::string::npos ? std::string::npos : nul - pos);
        if (!item.empty()) out.push_back(widen(item));
        if (nul == std::string::npos) break;
        pos = nul + 1;
    }
    return out;
}

struct ServiceInstallOp {
    ServiceConfig config;
    std::string display_for_error;
    std::wstring load_order_group;
    std::vector<std::wstring> depend_services, depend_groups;
    std::optional<std::wstring> description;  ///< nullopt = keep; "" = erase ("[~]")
    bool vital{false};
    bool unsupported_type{false};
    bool has_password{false};
};

/// Snapshot-based undo of a change to one service's database entry.
std::function<void()> service_undo(capsule::Registry* reg, const std::wstring& name) {
    auto snap = snapshot_key(*reg, capsule::HKEY_LOCAL_MACHINE, service_key(name));
    return [reg, name, snap] {
        delete_key_tree(*reg, capsule::HKEY_LOCAL_MACHINE, service_key(name));
        if (snap) restore_key(*reg, capsule::HKEY_LOCAL_MACHINE, service_key(name), *snap);
    };
}

u32 run_service_install(OpEnv& env, const ServiceInstallOp& op, std::function<void()>& undo) {
    capsule::Registry* reg = env.services.registry;
    ScmAccess access(env.services);
    ScmManager* scm = access.get();
    const std::string name = narrow(op.config.name);
    auto fail = [&]() -> u32 {
        // Learn Error 1923: Service '[2]' ([3]) could not be installed.
        const i32 answer = error_message(env, 1923, {op.display_for_error, name});
        if (!op.vital || answer == kIdIgnore) return kActionSuccess;
        return kActionFailure;
    };
    if (scm == nullptr || reg == nullptr) {
        log_unimplemented("InstallServices", "no capsule registry/VFS wired into the engine");
        return fail();
    }
    if (op.unsupported_type) return fail();  // driver types: Learn "unsupported"
    if (op.has_password) {
        // A capsule has one principal (advapi32 SCM): services run as it,
        // whatever account and password CreateService recorded.
        log_unimplemented("InstallServices", "service logon account password (single capsule principal)");
    }
    undo = service_undo(reg, op.config.name);
    u32 error = 0;
    const bool ok = scm->service_exists(op.config.name) ? scm->change_config(op.config, error)
                                                        : scm->create_service(op.config, error);
    if (!ok) return fail();
    // The parts CreateService's registry entry carries beyond ServiceConfig
    // (Learn "HKLM\SYSTEM\CurrentControlSet\Services Registry Tree").
    if (auto k = reg->open_key(capsule::HKEY_LOCAL_MACHINE, service_key(op.config.name), false); k.is_ok()) {
        const RegHandle h = k.value();
        if (!op.load_order_group.empty()) (void)reg->set_value(h, L"Group", RegValue{RegType::Sz, op.load_order_group});
        if (!op.depend_services.empty())
            (void)reg->set_value(h, L"DependOnService", RegValue{RegType::MultiSz, op.depend_services});
        if (!op.depend_groups.empty())
            (void)reg->set_value(h, L"DependOnGroup", RegValue{RegType::MultiSz, op.depend_groups});
        reg->close_key(h);
    }
    if (op.description) {
        if (!scm->change_description(op.config.name, op.description, error)) {
            // Learn Error 1930: the description could not be changed.
            (void)error_message(env, 1930, {op.display_for_error, name});
        }
    }
    return kActionSuccess;
}

[[nodiscard]] std::string component_key_file(Session& session, const std::string& component) {
    TableView t(session, "Component");
    for (usize i = 0; i < t.size(); ++i) {
        if (t.cell(i, "Component") != component) continue;
        const std::string key = t.cell(i, "KeyPath");
        if (key.empty()) return {};
        return file_target_path(session, key);
    }
    return {};
}

/// Services that depend on @p name (DependOnService), for a stop that must
/// stop them first (Learn "ServiceControl Table").
std::vector<std::wstring> dependents(ScmManager& scm, capsule::Registry& reg, const std::wstring& name) {
    std::vector<std::wstring> out;
    for (const auto& svc : scm.enumerate_services()) {
        auto k = reg.open_key(capsule::HKEY_LOCAL_MACHINE, service_key(svc.name), false);
        if (k.is_err()) continue;
        if (auto v = reg.query_value(k.value(), L"DependOnService"); v.is_ok()) {
            if (const auto* list = std::get_if<std::vector<std::wstring>>(&v.value().data)) {
                for (const auto& d : *list) {
                    if (iequals(narrow(d), narrow(name))) {
                        out.push_back(svc.name);
                        break;
                    }
                }
            }
        }
        reg.close_key(k.value());
    }
    return out;
}

bool running(ScmManager& scm, const std::wstring& name) {
    const u32 state = scm.query_status(name).current_state;
    return state != service_constants::SERVICE_STOPPED;
}

bool stop_service(ScmManager& scm, capsule::Registry& reg, const std::wstring& name,
                  std::vector<std::wstring>& stopped, int depth = 0) {
    if (depth > 16 || !scm.service_exists(name) || !running(scm, name)) return true;
    for (const auto& dep : dependents(scm, reg, name)) {
        if (!stop_service(scm, reg, dep, stopped, depth + 1)) return false;
    }
    ServiceStatus status;
    u32 error = 0;
    if (!scm.control_service(name, service_constants::SERVICE_CONTROL_STOP, status, error) &&
        error != error_codes::ERROR_SERVICE_NOT_ACTIVE)
        return false;
    stopped.push_back(name);
    return true;
}

struct ServiceControlOp {
    std::wstring name;
    std::vector<std::wstring> arguments;
    enum class Kind : u8 { Start, Stop, Delete } kind{Kind::Start};
};

u32 run_service_control(OpEnv& env, const ServiceControlOp& op, std::function<void()>& undo) {
    capsule::Registry* reg = env.services.registry;
    ScmAccess access(env.services);
    ScmManager* scm = access.get();
    const std::string name = narrow(op.name);
    if (scm == nullptr || reg == nullptr) {
        log_unimplemented("StartServices", "no capsule registry/VFS wired into the engine");
        return kActionFailure;
    }
    std::string display = name;
    if (auto cfg = scm->query_config(op.name); cfg && !cfg->display_name.empty()) display = narrow(cfg->display_name);
    action_data(env, {display, name});
    switch (op.kind) {
        case ServiceControlOp::Kind::Start: {
            if (running(*scm, op.name)) return kActionSuccess;
            u32 error = 0;
            if (!scm->start_service(op.name, op.arguments, error) &&
                error != error_codes::ERROR_SERVICE_ALREADY_RUNNING) {
                // Learn Error 1920: Service '[2]' ([3]) failed to start.
                if (error_message(env, 1920, {display, name}) == kIdIgnore) return kActionSuccess;
                return kActionFailure;
            }
            ScmManager* process_scm = g_scm.load();
            undo = [process_scm, reg, n = op.name] {
                if (process_scm == nullptr) return;
                std::vector<std::wstring> stopped;
                (void)stop_service(*process_scm, *reg, n, stopped);
            };
            return kActionSuccess;
        }
        case ServiceControlOp::Kind::Stop: {
            std::vector<std::wstring> stopped;
            if (!stop_service(*scm, *reg, op.name, stopped)) {
                // Learn Error 1921: Service '[2]' ([3]) could not be stopped.
                if (error_message(env, 1921, {display, name}) == kIdIgnore) return kActionSuccess;
                return kActionFailure;
            }
            ScmManager* process_scm = g_scm.load();
            undo = [process_scm, stopped] {
                if (process_scm == nullptr) return;
                for (auto it = stopped.rbegin(); it != stopped.rend(); ++it) {
                    u32 e = 0;
                    (void)process_scm->start_service(*it, {}, e);
                }
            };
            return kActionSuccess;
        }
        case ServiceControlOp::Kind::Delete: {
            if (!scm->service_exists(op.name)) return kActionSuccess;
            std::vector<std::wstring> stopped;
            if (!stop_service(*scm, *reg, op.name, stopped)) {
                if (error_message(env, 1921, {display, name}) == kIdIgnore) return kActionSuccess;
                return kActionFailure;
            }
            undo = service_undo(reg, op.name);
            u32 error = 0;
            if (!scm->delete_service(op.name, error)) {
                // Learn Error 1922: Service '[2]' ([3]) could not be deleted.
                if (error_message(env, 1922, {display, name}) == kIdIgnore) return kActionSuccess;
                return kActionFailure;
            }
            return kActionSuccess;
        }
    }
    return kActionFailure;
}

/// ServiceControl rows whose Event bits ask for @p install_bit on a
/// component being installed or @p uninstall_bit on one being removed.
void schedule_service_control(ActionContext& ctx, const char* action, u32 install_bit, u32 uninstall_bit,
                              ServiceControlOp::Kind kind) {
    Session& session = ctx.session();
    TableView t(session, "ServiceControl");
    for (usize i = 0; i < t.size(); ++i) {
        const ComponentAction a = component_action(session, t.cell(i, "Component_"));
        const u32 events = static_cast<u32>(t.integer(i, "Event").value_or(0));
        const bool wanted = (a == ComponentAction::Install && (events & install_bit) != 0) ||
                            (a == ComponentAction::Remove && (events & uninstall_bit) != 0);
        if (!wanted) continue;
        ServiceControlOp op;
        op.kind = kind;
        op.name = widen(session.format(t.cell(i, "Name")));
        if (op.name.empty()) continue;
        op.arguments = split_nul(session.format(t.cell(i, "Arguments")));
        auto undo = std::make_shared<std::function<void()>>();
        ScriptOp script;
        script.action = action;
        script.run = [env = OpEnv(ctx), op, undo]() mutable -> u32 { return run_service_control(env, op, *undo); };
        script.rollback = [undo] {
            if (*undo) (*undo)();
        };
        schedule(ctx, std::move(script));
    }
}

}  // namespace

void set_service_control_manager(ScmManager* scm) { g_scm.store(scm); }
ScmManager* service_control_manager() { return g_scm.load(); }

u32 collect_script_ops(ActionContext& ctx, const ActionFn& action, std::vector<ScriptOp>& ops) {
    std::vector<ScriptOp>* saved = search_detail::t_collect;
    search_detail::t_collect = &ops;
    const u32 rc = action(ctx);
    search_detail::t_collect = saved;
    return rc;
}

u32 action_install_services(ActionContext& ctx) {
    if (!require_script(ctx, "InstallServices")) return kActionFailure;
    Session& session = ctx.session();
    TableView t(session, "ServiceInstall");
    for (usize i = 0; i < t.size(); ++i) {
        const std::string component = t.cell(i, "Component_");
        if (component_action(session, component) != ComponentAction::Install) continue;
        ServiceInstallOp op;
        op.config.name = widen(session.format(t.cell(i, "Name")));
        op.config.display_name = widen(session.format(t.cell(i, "DisplayName")));
        op.display_for_error = narrow(op.config.display_name.empty() ? op.config.name : op.config.display_name);
        op.config.service_type = static_cast<u32>(t.integer(i, "ServiceType").value_or(0));
        op.config.start_type = static_cast<u32>(t.integer(i, "StartType").value_or(0));
        const u32 error_control = static_cast<u32>(t.integer(i, "ErrorControl").value_or(0));
        op.vital = (error_control & kErrorControlVital) != 0;
        op.config.error_control = error_control & ~kErrorControlVital;
        // The component's key file is the service executable (Learn
        // "ServiceInstall Table"); Arguments follow it.
        const std::string exe = component_key_file(session, component);
        const std::string args = session.format(t.cell(i, "Arguments"));
        std::string image = "\"" + exe + "\"";
        if (!args.empty()) image += " " + args;
        op.config.image_path = widen(image);
        op.config.object_name = widen(session.format(t.cell(i, "StartName")));
        op.has_password = !t.null(i, "Password");
        op.load_order_group = widen(session.format(t.cell(i, "LoadOrderGroup")));
        for (const auto& dep : split_nul(session.format(t.cell(i, "Dependencies")))) {
            if (!dep.empty() && dep[0] == L'+') op.depend_groups.push_back(dep.substr(1));
            else op.depend_services.push_back(dep);
        }
        if (!t.null(i, "Description")) {
            const std::string d = session.format(t.cell(i, "Description"));
            op.description = (d == std::string(1, '\0')) ? std::wstring() : widen(d);
        }
        const u32 kind = op.config.service_type & 0xFF;
        op.unsupported_type = kind != service_constants::SERVICE_WIN32_OWN_PROCESS &&
                              kind != service_constants::SERVICE_WIN32_SHARE_PROCESS;
        if (exe.empty() || op.config.name.empty()) op.unsupported_type = true;
        auto undo = std::make_shared<std::function<void()>>();
        ScriptOp script;
        script.action = "InstallServices";
        script.run = [env = OpEnv(ctx), op, undo]() mutable -> u32 { return run_service_install(env, op, *undo); };
        script.rollback = [undo] {
            if (*undo) (*undo)();
        };
        schedule(ctx, std::move(script));
    }
    return kActionSuccess;
}

u32 action_start_services(ActionContext& ctx) {
    if (!require_script(ctx, "StartServices")) return kActionFailure;
    schedule_service_control(ctx, "StartServices", 0x001, 0x010, ServiceControlOp::Kind::Start);
    return kActionSuccess;
}

u32 action_stop_services(ActionContext& ctx) {
    if (!require_script(ctx, "StopServices")) return kActionFailure;
    schedule_service_control(ctx, "StopServices", 0x002, 0x020, ServiceControlOp::Kind::Stop);
    return kActionSuccess;
}

u32 action_delete_services(ActionContext& ctx) {
    if (!require_script(ctx, "DeleteServices")) return kActionFailure;
    schedule_service_control(ctx, "DeleteServices", 0x008, 0x080, ServiceControlOp::Kind::Delete);
    return kActionSuccess;
}

namespace {

struct ServiceConfigOp {
    std::wstring name;
    bool failure_actions{false};
    // MsiServiceConfig
    u32 config_type{0};
    std::string argument;
    // MsiServiceConfigFailureActions
    ServiceFailureActionsUpdate update;
};

u32 run_service_config(OpEnv& env, const ServiceConfigOp& op, std::function<void()>& undo) {
    capsule::Registry* reg = env.services.registry;
    ScmAccess access(env.services);
    ScmManager* scm = access.get();
    const std::string name = narrow(op.name);
    if (scm == nullptr || reg == nullptr) {
        log_unimplemented("MsiConfigureServices", "no capsule registry/VFS wired into the engine");
        return kActionFailure;
    }
    // Learn Error 1939: Service '[2]' ([3]) could not be configured.
    auto fail = [&]() -> u32 {
        if (error_message(env, 1939, {name, name}) == kIdIgnore) return kActionSuccess;
        return kActionFailure;
    };
    if (!scm->service_exists(op.name)) return fail();
    undo = service_undo(reg, op.name);
    u32 error = 0;
    if (op.failure_actions) return scm->change_failure_actions(op.name, op.update, error) ? kActionSuccess : fail();
    auto k = reg->open_key(capsule::HKEY_LOCAL_MACHINE, service_key(op.name), false);
    if (k.is_err()) return fail();
    const RegHandle h = k.value();
    bool ok = true;
    const auto dword = [&](const wchar_t* value_name, u32 v) {
        ok = reg->set_value(h, value_name, RegValue{RegType::Dword, v}).is_ok();
    };
    // ChangeServiceConfig2 info levels (winsvc.h) and the registry values
    // the SCM keeps them in (advapi32 scm_database query_config2).
    switch (op.config_type) {
        case 3:  // SERVICE_CONFIG_DELAYED_AUTO_START_INFO
            dword(L"DelayedAutostart", parse_int(op.argument).value_or(0) != 0 ? 1u : 0u);
            break;
        case 4:  // SERVICE_CONFIG_FAILURE_ACTIONS_FLAG
            dword(L"FailureActionsOnNonCrashFailures", parse_int(op.argument).value_or(0) != 0 ? 1u : 0u);
            break;
        case 5:  // SERVICE_CONFIG_SERVICE_SID_INFO
            dword(L"ServiceSidType", static_cast<u32>(parse_int(op.argument).value_or(0)));
            break;
        case 6:  // SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO
            ok = reg->set_value(h, L"RequiredPrivileges", RegValue{RegType::MultiSz, split_nul(op.argument)}).is_ok();
            break;
        case 7:  // SERVICE_CONFIG_PRESHUTDOWN_INFO; empty = the 3-minute default
            if (op.argument.empty()) (void)reg->delete_value(h, L"PreshutdownTimeout");
            else dword(L"PreshutdownTimeout", static_cast<u32>(parse_int(op.argument).value_or(0)));
            break;
        default: ok = false; break;
    }
    reg->close_key(h);
    return ok ? kActionSuccess : fail();
}

[[nodiscard]] bool config_event_applies(Session& session, const std::string& component, u32 events) {
    const ComponentAction a = component_action(session, component);
    if (a == ComponentAction::Remove) return (events & 2u) != 0;  // msidbServiceConfigEventUninstall
    if (a != ComponentAction::Install) return false;
    // Install vs reinstall: a component already installed is reinstalled.
    bool installed = false;
    if (const auto& resolve = session.item_states().component_installed) {
        if (auto s = resolve(component)) installed = *s == 3 || *s == 4;
    }
    return (events & (installed ? 4u : 1u)) != 0;
}

void schedule_config(ActionContext& ctx, ServiceConfigOp op) {
    auto undo = std::make_shared<std::function<void()>>();
    ScriptOp script;
    script.action = "MsiConfigureServices";
    script.run = [env = OpEnv(ctx), op, undo]() mutable -> u32 { return run_service_config(env, op, *undo); };
    script.rollback = [undo] {
        if (*undo) (*undo)();
    };
    schedule(ctx, std::move(script));
}

}  // namespace

u32 action_msi_configure_services(ActionContext& ctx) {
    if (!require_script(ctx, "MsiConfigureServices")) return kActionFailure;
    Session& session = ctx.session();
    {
        TableView t(session, "MsiServiceConfig");
        for (usize i = 0; i < t.size(); ++i) {
            if (!config_event_applies(session, t.cell(i, "Component_"),
                                      static_cast<u32>(t.integer(i, "Event").value_or(0))))
                continue;
            ServiceConfigOp op;
            op.name = widen(session.format(t.cell(i, "Name")));
            op.config_type = static_cast<u32>(t.integer(i, "ConfigType").value_or(0));
            op.argument = session.format(t.cell(i, "Argument"));
            schedule_config(ctx, std::move(op));
        }
    }
    TableView t(session, "MsiServiceConfigFailureActions");
    for (usize i = 0; i < t.size(); ++i) {
        if (!config_event_applies(session, t.cell(i, "Component_"),
                                  static_cast<u32>(t.integer(i, "Event").value_or(0))))
            continue;
        ServiceConfigOp op;
        op.failure_actions = true;
        op.name = widen(session.format(t.cell(i, "Name")));
        // A null ResetPeriod never resets the failure count (INFINITE).
        op.update.reset_period_seconds = static_cast<u32>(t.integer(i, "ResetPeriod").value_or(0xFFFFFFFF));
        // "" keeps the current text, "[~]" deletes it (Learn).
        for (auto [column, target] : {std::pair{"RebootMessage", &op.update.reboot_message},
                                      std::pair{"Command", &op.update.command}}) {
            if (t.null(i, column)) continue;
            const std::string v = session.format(t.cell(i, column));
            *target = (v == std::string(1, '\0')) ? std::wstring() : widen(v);
        }
        if (!t.null(i, "Actions")) {
            std::vector<ServiceFailureAction> actions;
            const auto types = split_nul(session.format(t.cell(i, "Actions")));
            const auto delays = split_nul(session.format(t.cell(i, "DelayActions")));
            for (usize a = 0; a < types.size(); ++a) {
                ServiceFailureAction fa;
                fa.type = static_cast<u32>(parse_int(narrow(types[a])).value_or(0));
                if (a < delays.size()) fa.delay_ms = static_cast<u32>(parse_int(narrow(delays[a])).value_or(0));
                actions.push_back(fa);
            }
            op.update.actions = std::move(actions);
        }
        schedule_config(ctx, std::move(op));
    }
    return kActionSuccess;
}

u32 action_allocate_registry_space(ActionContext& ctx) {
    // AVAILABLEFREEREG KB of free registry space must exist (Learn). The
    // capsule registry has no size quota, so any requirement is met.
    OpEnv env(ctx);
    action_data(env, {ctx.session().property("AVAILABLEFREEREG")});
    return kActionSuccess;
}

void register_search_actions(ActionRegistry& r) {
    install_handoff_hook();
    r.add("AppSearch", action_app_search);
    r.add("CCPSearch", action_ccp_search);
    r.add("RMCCPSearch", action_rmccp_search);
    r.add("WriteRegistryValues", action_write_registry_values);
    r.add("RemoveRegistryValues", action_remove_registry_values);
    r.add("WriteIniValues", action_write_ini_values);
    r.add("RemoveIniValues", action_remove_ini_values);
    r.add("WriteEnvironmentStrings", action_write_environment_strings);
    r.add("RemoveEnvironmentStrings", action_remove_environment_strings);
    r.add("InstallODBC", action_install_odbc);
    r.add("RemoveODBC", action_remove_odbc);
    r.add("SetODBCFolders", action_set_odbc_folders);
    r.add("InstallServices", action_install_services);
    r.add("StartServices", action_start_services);
    r.add("StopServices", action_stop_services);
    r.add("DeleteServices", action_delete_services);
    r.add("MsiConfigureServices", action_msi_configure_services);
    r.add("AllocateRegistrySpace", action_allocate_registry_space);
}

}  // namespace vitreo::winapi::msi
