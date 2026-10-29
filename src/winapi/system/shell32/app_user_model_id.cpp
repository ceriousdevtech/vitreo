// SPDX-License-Identifier: LGPL-2.1-only
/// @file app_user_model_id.cpp

#include "winapi/system/shell32/app_user_model_id.hpp"

#include "ipc/hwnd_registry.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/system/shell32/shell_paths.hpp"
#include "winapi/system/shell32/shell_paths_internal.hpp"
#include "winapi/com/ole32/com_core_internal.hpp"
#include "capsule/vfs/vfs.hpp"

#include <atomic>
#include <cstdio>

namespace vitreo::winapi::shell {
namespace {

/// System.Title {F29F85E0-4FF9-1068-AB91-08002B27B3D9},2: a string.
constexpr PropertyFmtid kSummaryFmtid{
    0xE0, 0x85, 0x9F, 0xF2, 0xF9, 0x4F, 0x68, 0x10,
    0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9};

[[nodiscard]] bool is_space(char16_t c) noexcept {
    return c == u' ' || c == u'\t' || c == u'\r' || c == u'\n' || c == u'\v' ||
           c == u'\f';
}

[[nodiscard]] std::u16string trimmed(const std::u16string& text) {
    usize first = 0;
    usize last = text.size();
    while (first < last && is_space(text[first])) ++first;
    while (last > first && is_space(text[last - 1])) --last;
    return text.substr(first, last - first);
}

[[nodiscard]] bool equals_ignoring_ascii_case(const std::u16string& text,
                                              std::u16string_view word) {
    if (text.size() != word.size()) return false;
    for (usize i = 0; i < text.size(); ++i) {
        char16_t a = text[i];
        char16_t b = word[i];
        if (a >= u'A' && a <= u'Z') a = static_cast<char16_t>(a - u'A' + u'a');
        if (b >= u'A' && b <= u'Z') b = static_cast<char16_t>(b - u'A' + u'a');
        if (a != b) return false;
    }
    return true;
}

/// "true"/"false", or an optionally signed decimal integer.
[[nodiscard]] std::optional<bool> parse_boolean(const std::u16string& text) {
    if (equals_ignoring_ascii_case(text, u"true")) return true;
    if (equals_ignoring_ascii_case(text, u"false")) return false;
    usize at = 0;
    if (at < text.size() && (text[at] == u'-' || text[at] == u'+')) ++at;
    if (at == text.size()) return std::nullopt;
    bool nonzero = false;
    for (; at < text.size(); ++at) {
        if (text[at] < u'0' || text[at] > u'9') return std::nullopt;
        nonzero = nonzero || text[at] != u'0';
    }
    return nonzero;
}

} // namespace

ShellPropertyType shell_property_type(const PropertyFmtid& fmtid, u32 pid) noexcept {
    if (fmtid == kAppUserModelFmtid) {
        switch (pid) {
        // RelaunchCommand, RelaunchIconResource, RelaunchDisplayNameResource,
        // ID, VisualElementsManifestHintPath, UninstallCommand,
        // SettingsCommand.
        case 2: case 3: case 4: case 5: case 31: case 37: case 38:
            return ShellPropertyType::String;
        // IsDestListSeparator, ExcludeFromShowInNewInstall, PreventPinning,
        // IsDualMode.
        case 6: case 8: case 9: case 11:
            return ShellPropertyType::Boolean;
        // StartPinOption (UInt32), ToastActivatorCLSID (Guid).
        case 12: case 26:
            return ShellPropertyType::Unconverted;
        default:
            return ShellPropertyType::Unknown;
        }
    }
    if (fmtid == kSummaryFmtid && pid == 2) return ShellPropertyType::String;
    return ShellPropertyType::Unknown;
}

CanonicalShellValue canonical_shell_value(const PropertyFmtid& fmtid, u32 pid,
                                          const std::u16string& stored) {
    CanonicalShellValue value;
    const ShellPropertyType type = shell_property_type(fmtid, pid);
    if (type == ShellPropertyType::Unknown || type == ShellPropertyType::Unconverted) {
        if (type == ShellPropertyType::Unconverted) {
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true)) {
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED shell32!SHGetPropertyStoreForWindow "
                             "IPropertyStore::GetValue: canonical UInt32/Guid "
                             "conversion (pid %u returned as text)\n",
                             pid);
            }
        }
        value.hr = 1;  // S_FALSE
        value.kind = CanonicalShellValue::Kind::String;
        value.text = stored;
        return value;
    }
    const std::u16string text = trimmed(stored);
    const u32 trimmed_hr = text.size() == stored.size() ? 0u : kInplaceTruncated;
    if (text.empty()) {
        value.hr = trimmed_hr;
        return value;
    }
    if (type == ShellPropertyType::String) {
        value.hr = trimmed_hr;
        value.kind = CanonicalShellValue::Kind::String;
        value.text = text;
        return value;
    }
    const auto flag = parse_boolean(text);
    if (!flag) {
        value.hr = kTypeMismatch;
        return value;
    }
    value.hr = trimmed_hr;
    value.kind = CanonicalShellValue::Kind::Boolean;
    value.flag = *flag;
    return value;
}

std::optional<std::u16string> window_app_user_model_id(u32 hwnd) {
    for (const auto& property : ipc::hwnd_registry_shell_properties(hwnd)) {
        if (property.fmtid != kAppUserModelFmtid || property.pid != kAppUserModelIdPid) {
            continue;
        }
        const CanonicalShellValue value =
            canonical_shell_value(property.fmtid, property.pid, property.text);
        if (value.kind != CanonicalShellValue::Kind::String) return std::nullopt;
        return value.text;
    }
    return std::nullopt;
}

std::string effective_app_user_model_id(
    u32 hwnd, const std::optional<std::u16string>& process_explicit_id,
    std::string_view image_path) {
    const auto utf8 = [](const std::u16string& text) {
        return utf16_to_utf8(reinterpret_cast<const u16*>(text.data()),
                             static_cast<int>(text.size()));
    };
    if (hwnd != 0) {
        if (auto window_id = window_app_user_model_id(hwnd)) {
            return "id:" + utf8(*window_id);
        }
    }
    if (process_explicit_id && !process_explicit_id->empty()) {
        return "id:" + utf8(*process_explicit_id);
    }
    // Windows paths compare without regard to case.
    std::string image(image_path);
    for (char& c : image) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (c == '/') c = '\\';
    }
    return "image:" + image;
}

namespace {

constexpr u32 kShardPidl = 1, kShardPathA = 2, kShardPathW = 3, kShardShellItem = 8;
constexpr i32 kCsidlRecent = 0x0008;

void report_unimplemented_once(std::atomic_flag& flag, const char* what) {
    if (!flag.test_and_set()) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED shell32!SHAddToRecentDocs: %s\n", what);
    }
}

std::string lower_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

bool is_absolute_path(const std::string& path) {
    return (path.size() >= 3 && path[1] == ':' && (path[2] == '\\' || path[2] == '/')) ||
           (path.size() >= 2 && (path[0] == '\\' || path[0] == '/') &&
            (path[1] == '\\' || path[1] == '/'));
}

/// The leaf of a Windows path and the directory holding it.
void split_path(std::string path, std::string& parent, std::string& leaf) {
    while (path.size() > 3 && (path.back() == '\\' || path.back() == '/')) path.pop_back();
    const auto cut = path.find_last_of("\\/");
    if (cut == std::string::npos) {
        parent.clear();
        leaf = path;
        return;
    }
    leaf = path.substr(cut + 1);
    parent = path.substr(0, cut);
    if (parent.size() == 2 && parent[1] == ':') parent.push_back('\\');
}

bool ensure_directory(capsule::Vfs& vfs, const std::string& dir) {
    if (vfs.stat(dir).is_ok()) return true;
    std::string current;
    usize start = 0;
    while (start <= dir.size()) {
        const usize next = dir.find('\\', start);
        const std::string part = dir.substr(start, next == std::string::npos ? std::string::npos
                                                                              : next - start);
        current += part;
        if (current.size() > 2 && vfs.stat(current).is_err() && vfs.create_dir(current).is_err()) {
            return false;
        }
        if (next == std::string::npos) break;
        current.push_back('\\');
        start = next + 1;
    }
    return vfs.stat(dir).is_ok();
}

/// A Shell Link ([MS-SHLLINK] 2.1 header, then Unicode StringData) naming
/// `target`, in the same form IShellLink's IPersistFile::Save writes.
std::vector<u8> shell_link_bytes(const std::string& target, const std::string& working_dir,
                                 u32 attributes) {
    std::vector<u8> out;
    auto u16le = [&](u16 v) { out.push_back(static_cast<u8>(v)); out.push_back(static_cast<u8>(v >> 8)); };
    auto u32le = [&](u32 v) { for (u32 s = 0; s < 32; s += 8) out.push_back(static_cast<u8>(v >> s)); };
    auto string_data = [&](const std::string& text) {
        const std::vector<u16> wide = utf8_to_utf16(text);
        u16le(static_cast<u16>(std::min<usize>(wide.size(), 0xFFFF)));
        for (usize i = 0; i < wide.size() && i < 0xFFFF; ++i) u16le(wide[i]);
    };
    constexpr u32 kHasRelativePath = 0x00000008u, kHasWorkingDir = 0x00000010u,
                  kIsUnicode = 0x00000080u;
    u32le(0x4Cu);                                       // HeaderSize
    const auto* clsid = reinterpret_cast<const u8*>(&CLSID_SHELLLINK);
    out.insert(out.end(), clsid, clsid + sizeof(CLSID_SHELLLINK));
    u32le(kIsUnicode | kHasRelativePath | (working_dir.empty() ? 0u : kHasWorkingDir));
    u32le(attributes);                                  // FileAttributes
    for (int i = 0; i < 6; ++i) u32le(0);               // Creation/Access/WriteTime
    u32le(0);                                           // FileSize
    u32le(0);                                           // IconIndex
    u32le(1);                                           // ShowCommand SW_SHOWNORMAL
    u16le(0); u16le(0); u32le(0); u32le(0);             // HotKey, Reserved1-3
    string_data(target);
    if (!working_dir.empty()) string_data(working_dir);
    u32le(0);                                           // TerminalBlock
    return out;
}

bool write_file(capsule::Vfs& vfs, const std::string& path, const std::vector<u8>& bytes) {
    auto h = vfs.open(path, capsule::AccessMode::Write, capsule::ShareMode::Read,
                      capsule::CreationDisposition::CreateAlways);
    if (h.is_err()) return false;
    const auto written = vfs.write(h.value(), bytes);
    const auto closed = vfs.close(h.value());
    return written.is_ok() && written.value() == bytes.size() && closed.is_ok();
}

} // namespace

void add_to_recent_docs(Marshaler& m, ShellPathManager& paths, capsule::Vfs& vfs) {
    static std::atomic_flag jump_list_reported = ATOMIC_FLAG_INIT;
    static std::atomic_flag form_reported[kShardShellItem + 1] = {};
    const u32 flags = m.arg32(0);
    const GuestAddr pv = m.arg_ptr(1);
    report_unimplemented_once(jump_list_reported,
                              "Jump List Recent/Frequent usage data (no destination store)");

    std::wstring recent_w;
    if (paths.get_folder_path(kCsidlRecent, recent_w, false, m.cpu_is_64bit()) != 0) return;
    const std::string recent = wstring_to_utf8(recent_w);

    if (pv == 0) {
        // Clear all usage data: the Recent folder's shortcuts.
        auto listing = vfs.list_dir(recent);
        if (listing.is_err()) return;
        for (const auto& entry : listing.value()) {
            const std::string name = lower_ascii(entry.name);
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".lnk") == 0) {
                (void)vfs.remove(recent + "\\" + entry.name);
            }
        }
        return;
    }

    std::string path;
    if (flags == kShardPathW) {
        auto text = m.read_wide_string(pv);
        if (text.is_err()) return;
        path = text.value();
    } else if (flags == kShardPathA) {
        auto text = m.read_ansi_string(pv);
        if (text.is_err()) return;
        const std::string& bytes = text.value();
        std::vector<u16> units(bytes.size() + 1);
        const auto r = codepage::multi_byte_to_wide(
            codepage::ansi_code_page(), 0, reinterpret_cast<const u8*>(bytes.data()),
            bytes.size(), units.data(), units.size());
        if (!r.ok()) return;
        path = utf16_to_utf8(units.data(), static_cast<int>(r.count));
    } else if (flags == kShardPidl) {
        std::wstring wide;
        if (!paths.get_path_from_pidl(pv, wide)) return;
        path = wstring_to_utf8(wide);
    } else if (flags > kShardPathW && flags <= kShardShellItem) {
        report_unimplemented_once(
            form_reported[flags],
            flags == 6 ? "SHARD_LINK (IShellLink jump-list item)"
            : flags == kShardShellItem ? "SHARD_SHELLITEM (IShellItem)"
                                       : "SHARD_APPIDINFO* (AppUserModelID-scoped item)");
        return;
    } else {
        return;   // not a SHARD_* value: Windows ignores it (recent_badflag)
    }
    if (path.empty()) return;
    for (char& c : path) {
        if (c == '/') c = '\\';
    }
    if (!is_absolute_path(path)) {
        std::string cwd = vfs.cwd();
        if (!cwd.empty() && cwd.back() != '\\') cwd.push_back('\\');
        path = cwd + path;
    }

    // Filters (recent_exe, recent_in_temp).
    const std::string lowered = lower_ascii(path);
    if (lowered.size() > 4 && lowered.compare(lowered.size() - 4, 4, ".exe") == 0) return;
    std::string temp = lower_ascii(paths.expand_environment("%TEMP%"));
    while (!temp.empty() && temp.back() == '\\') temp.pop_back();
    if (!temp.empty() && temp != "%temp%" && lowered.rfind(temp + "\\", 0) == 0) return;

    if (!ensure_directory(vfs, recent)) return;
    std::string parent, leaf;
    split_path(path, parent, leaf);
    if (leaf.empty()) return;
    constexpr u32 kAttrDirectory = 0x10u;
    u32 attributes = 0;
    if (auto info = vfs.stat(path); info.is_ok()) {
        attributes = static_cast<u32>(info.value().attributes);
    }
    (void)write_file(vfs, recent + "\\" + leaf + ".lnk",
                     shell_link_bytes(path, parent, attributes));
    // The folder holding the item (recent_dir_link), unless it is a root.
    std::string grand, folder_leaf;
    split_path(parent, grand, folder_leaf);
    if (!folder_leaf.empty() && !(parent.size() <= 3 && parent.size() >= 2 && parent[1] == ':')) {
        (void)write_file(vfs, recent + "\\" + folder_leaf + ".lnk",
                         shell_link_bytes(parent, grand, kAttrDirectory));
    }
}

} // namespace vitreo::winapi::shell
