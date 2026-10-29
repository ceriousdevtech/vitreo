// SPDX-License-Identifier: LGPL-2.1-only

/// @file wow64_redirector.cpp
/// @brief The WOW64 registry redirector over the capsule registry.
///
/// Contract: Learn "Registry Redirector", "Registry Keys Affected by WOW64",
/// "Accessing an Alternate Registry View", measured on Windows 11 26200 by
/// tools/oracle_probe/wow64_registry_probe.c (x86 and x64):
///   tests/oracle_data/wow64_registry_probe{,.x86}.expected  (behaviour)
///   tests/oracle_data/wow64_registry_walk.x86.expected      (which keys of
///       HKLM\SOFTWARE and HKCU\Software the 32-bit view redirects/shares)
///
/// The capsule stores the 64-bit Windows layout: the 32-bit view of a
/// redirected key lives in a WOW6432Node key, exactly as on 64-bit Windows,
/// so the .reg hives need no second store and 32- and 64-bit processes of
/// one capsule see each other's writes. The rules:
///
///  * 32-bit view of HKLM\SOFTWARE\X -> HKLM\SOFTWARE\WOW6432Node\X, except
///    the shared keys (kSharedSoftwareKeys) and SOFTWARE\Classes.
///  * SOFTWARE\Classes (machine and user) is shared, except its CLSID,
///    DirectShow, Interface, Media Type and MediaFoundation subkeys, whose
///    32-bit view is <Classes>\WOW6432Node\<subkey>. TypeLib and AppID are
///    shared (class_TypeLib_* / class_AppID_*).
///  * HKCU\Software outside Classes is shared.
///  * In the 32-bit view an explicit SOFTWARE\WOW6432Node is taken back to
///    SOFTWARE and redirected again, so SOFTWARE\WOW6432Node\X is the 32-bit
///    X and SOFTWARE\WOW6432Node\Microsoft\Ole the shared Ole key
///    (explicit_wow_*, from_s32_microsoft_ole_default); a
///    Classes\WOW6432Node path opens as itself (explicit_classes_wow_32key).
///  * The 64-bit view applies no redirection, but WOW6432Node holds links:
///    WOW6432Node\Classes is Classes\WOW6432Node, and WOW6432Node\<shared
///    key> is the shared key (explicit_wow_classes_64key,
///    explicit_wow_policies_64key, explicit_wow_ole_64key).

#include "capsule/registry/registry.hpp"

#include <algorithm>
#include <cwctype>
#include <initializer_list>

namespace vitreo::capsule {

namespace {

bool ieq(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    }
    return true;
}

constexpr std::wstring_view kWow = L"WOW6432Node";

/// Shared keys below HKLM\SOFTWARE: the 32-bit view opens the 64-bit key
/// itself. Every key the oracle walk classified "shared" (depth 6), which
/// includes every key Learn's Windows 7+ table lists as shared.
const std::vector<std::vector<std::wstring>>& shared_software_keys() {
    static const std::vector<std::vector<std::wstring>> keys = [] {
        const wchar_t* const list[] = {
            L"Clients",
            L"Policies",
            L"RegisteredApplications",
            L"Microsoft\\Cellular",
            L"Microsoft\\COM3",
            L"Microsoft\\Cryptography\\Calais\\Current",
            L"Microsoft\\Cryptography\\Calais\\Readers",
            L"Microsoft\\Cryptography\\Services",
            L"Microsoft\\CTF\\SystemShared",
            L"Microsoft\\CTF\\TIP",
            L"Microsoft\\DeviceReg",
            L"Microsoft\\DFS",
            L"Microsoft\\Driver Signing",
            L"Microsoft\\EnterpriseCertificates",
            L"Microsoft\\EventSystem",
            L"Microsoft\\FingerKB",
            L"Microsoft\\FuzzyDS",
            L"Microsoft\\Input",
            L"Microsoft\\LanguageOverlay",
            L"Microsoft\\Messaging",
            L"Microsoft\\MSMQ",
            L"Microsoft\\MTF",
            L"Microsoft\\MTFFuzzyFactors",
            L"Microsoft\\MTFInputType",
            L"Microsoft\\MTFKeyboardMappings",
            L"Microsoft\\Non-Driver Signing",
            L"Microsoft\\Notepad\\DefaultFonts",
            L"Microsoft\\Ole",
            L"Microsoft\\Phone",
            L"Microsoft\\Pim",
            L"Microsoft\\Poom",
            L"Microsoft\\Ras",
            L"Microsoft\\Rpc",
            L"Microsoft\\SecurityManager",
            L"Microsoft\\SEMgr",
            L"Microsoft\\Shell",
            L"Microsoft\\Software\\Microsoft\\Shared Tools\\Msinfo",
            L"Microsoft\\SystemCertificates",
            L"Microsoft\\TermServLicensing",
            L"Microsoft\\Transaction Server",
            L"Microsoft\\Unified Store",
            L"Microsoft\\UserData",
            L"Microsoft\\Windows Search",
            L"Microsoft\\XAML",
            L"Microsoft\\Windows\\CurrentVersion\\App Paths",
            L"Microsoft\\Windows\\CurrentVersion\\Control Panel\\Cursors\\Schemes",
            L"Microsoft\\Windows\\CurrentVersion\\Control Panel\\Theme",
            L"Microsoft\\Windows\\CurrentVersion\\Control Panel\\ThemeVolatile",
            L"Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\\Folder",
            L"Microsoft\\Windows\\CurrentVersion\\Explorer\\AutoplayHandlers",
            L"Microsoft\\Windows\\CurrentVersion\\Explorer\\DriveIcons",
            L"Microsoft\\Windows\\CurrentVersion\\Explorer\\KindMap",
            L"Microsoft\\Windows\\CurrentVersion\\Group Policy",
            L"Microsoft\\Windows\\CurrentVersion\\Parental Controls",
            L"Microsoft\\Windows\\CurrentVersion\\Policies",
            L"Microsoft\\Windows\\CurrentVersion\\PreviewHandlers",
            L"Microsoft\\Windows\\CurrentVersion\\Setup",
            L"Microsoft\\Windows\\CurrentVersion\\Telephony\\Locations",
            L"Microsoft\\Windows NT\\CurrentVersion\\Console",
            L"Microsoft\\Windows NT\\CurrentVersion\\Containers",
            L"Microsoft\\Windows NT\\CurrentVersion\\FontDPI",
            L"Microsoft\\Windows NT\\CurrentVersion\\FontLink",
            L"Microsoft\\Windows NT\\CurrentVersion\\FontMapper",
            L"Microsoft\\Windows NT\\CurrentVersion\\Fonts",
            L"Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes",
            L"Microsoft\\Windows NT\\CurrentVersion\\GRE_Initialize",
            L"Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options",
            L"Microsoft\\Windows NT\\CurrentVersion\\LanguagePack",
            L"Microsoft\\Windows NT\\CurrentVersion\\NetworkCards",
            L"Microsoft\\Windows NT\\CurrentVersion\\Perflib",
            L"Microsoft\\Windows NT\\CurrentVersion\\Ports",
            L"Microsoft\\Windows NT\\CurrentVersion\\Print",
            L"Microsoft\\Windows NT\\CurrentVersion\\ProfileList",
            L"Microsoft\\Windows NT\\CurrentVersion\\Time Zones",
            L"Microsoft\\Windows NT\\CurrentVersion\\Virtualization\\HvSocket",
        };
        std::vector<std::vector<std::wstring>> out;
        for (const wchar_t* entry : list) out.push_back(split_reg_path(entry));
        return out;
    }();
    return keys;
}

/// Subkeys of a Classes key whose 32-bit view is <Classes>\WOW6432Node.
bool redirected_class_subkey(std::wstring_view name) {
    for (std::wstring_view k : {std::wstring_view(L"CLSID"), std::wstring_view(L"DirectShow"),
                                std::wstring_view(L"Interface"), std::wstring_view(L"Media Type"),
                                std::wstring_view(L"MediaFoundation")}) {
        if (ieq(name, k)) return true;
    }
    return false;
}

/// True when path[start..] lies at or below a shared SOFTWARE key.
bool is_shared(const std::vector<std::wstring>& path, usize start) {
    for (const auto& entry : shared_software_keys()) {
        if (path.size() - start < entry.size()) continue;
        bool match = true;
        for (usize i = 0; i < entry.size() && match; ++i) {
            match = ieq(path[start + i], entry[i]);
        }
        if (match) return true;
    }
    return false;
}

bool is_root(const std::vector<std::wstring>& path, std::wstring_view root) {
    return !path.empty() && ieq(path[0], root);
}

bool is_hklm_software(const std::vector<std::wstring>& path) {
    return is_root(path, L"HKEY_LOCAL_MACHINE") && path.size() >= 2 &&
           ieq(path[1], L"SOFTWARE");
}

/// Index of the first component below a Classes root, or 0 when `path` is
/// not below one: HKLM\SOFTWARE\Classes, HKCU\Software\Classes,
/// HKU\<user>\Software\Classes and HKU\<sid>_Classes.
usize classes_base(const std::vector<std::wstring>& path) {
    if ((is_root(path, L"HKEY_LOCAL_MACHINE") || is_root(path, L"HKEY_CURRENT_USER")) &&
        path.size() >= 3 && ieq(path[1], L"Software") && ieq(path[2], L"Classes")) {
        return 3;
    }
    if (is_root(path, L"HKEY_USERS") && path.size() >= 2) {
        constexpr std::wstring_view suffix = L"_Classes";
        const std::wstring& user = path[1];
        if (user.size() > suffix.size() &&
            ieq(std::wstring_view(user).substr(user.size() - suffix.size()), suffix)) {
            return 2;
        }
        if (path.size() >= 4 && ieq(path[2], L"Software") && ieq(path[3], L"Classes")) return 4;
    }
    return 0;
}

}  // namespace

std::vector<std::wstring> Registry::redirect_path(std::vector<std::wstring> path, bool wow32) {
    if (!wow32) {
        // 64-bit view: the links WOW6432Node carries.
        if (is_hklm_software(path) && path.size() >= 3 && ieq(path[2], kWow)) {
            if (path.size() >= 4 && ieq(path[3], L"Classes")) {
                path[2] = L"Classes";
                path[3] = std::wstring(kWow);
            } else if (is_shared(path, 3)) {
                path.erase(path.begin() + 2);
            }
        }
        return path;
    }

    // Back to the logical path: an explicit SOFTWARE\WOW6432Node is the
    // 32-bit view of SOFTWARE itself, so its shared keys and Classes resolve
    // as from SOFTWARE (explicit_wow_ole_32key, explicit_wow_classes_32key).
    // A Classes\WOW6432Node key is not redirected again: it opens as itself
    // (explicit_classes_wow_32key, hkcr_wow_clsid_32key).
    if (is_hklm_software(path) && path.size() >= 3 && ieq(path[2], kWow)) {
        path.erase(path.begin() + 2);
    }

    const auto redirect_classes = [&path](usize base) {
        if (path.size() > base && redirected_class_subkey(path[base])) {
            path.insert(path.begin() + static_cast<std::ptrdiff_t>(base), std::wstring(kWow));
        }
    };
    if (is_hklm_software(path)) {
        if (path.size() >= 3 && ieq(path[2], L"Classes")) {
            redirect_classes(3);
        } else if (!is_shared(path, 2)) {
            path.insert(path.begin() + 2, std::wstring(kWow));
        }
    } else if (const usize base = classes_base(path); base != 0) {
        redirect_classes(base);
    }
    return path;
}

void Registry::set_wow64_process(bool wow64) {
    std::lock_guard lock(m_mutex);
    m_wow64_process = wow64;
    if (auto hkcr = m_merged_handles.find(HKEY_CLASSES_ROOT); hkcr != m_merged_handles.end()) {
        hkcr->second.wow32 = wow64;
    }
}

bool Registry::wow64_process() const {
    std::lock_guard lock(m_mutex);
    return m_wow64_process;
}

bool Registry::handle_is_wow32(RegHandle handle) const {
    if (const auto merged = m_merged_handles.find(handle); merged != m_merged_handles.end()) {
        return handle == HKEY_CLASSES_ROOT ? m_wow64_process : merged->second.wow32;
    }
    const auto info = m_handle_info.find(handle);
    if (info == m_handle_info.end() || info->second.view == RegView::Default) {
        return m_wow64_process;
    }
    return info->second.view == RegView::Wow32;
}

RegNode* Registry::root_node(const std::wstring& name) const {
    if (ieq(name, m_hklm->name)) return m_hklm.get();
    if (ieq(name, m_hkcu->name)) return m_hkcu.get();
    if (ieq(name, m_hku->name)) return m_hku.get();
    return nullptr;
}

RegNode* Registry::walk_physical(const Path& path, bool create) {
    if (path.empty()) return nullptr;
    RegNode* root = root_node(path[0]);
    if (root == nullptr) return nullptr;
    return walk_path(root, Path(path.begin() + 1, path.end()), create);
}

std::wstring Registry::journal_path(const Path& path) {
    std::wstring out;
    for (const auto& part : path) {
        if (!out.empty()) out.push_back(L'\\');
        out += part;
    }
    return out;
}

std::wstring Registry::path_string(const Path& path) {
    std::wstring out;
    for (const auto& part : path) {
        if (!out.empty()) out.push_back(L'\\');
        out += part;
    }
    std::transform(out.begin(), out.end(), out.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return out;
}

void Registry::forget_handles_to(RegNode* node) {
    for (auto handle = m_merged_handles.begin(); handle != m_merged_handles.end();) {
        const auto& binding = handle->second;
        if (handle->first != HKEY_CLASSES_ROOT &&
            (binding.user == node || binding.machine == node)) {
            m_handles.erase(handle->first);
            handle = m_merged_handles.erase(handle);
        } else {
            ++handle;
        }
    }
    for (auto handle = m_handles.begin(); handle != m_handles.end();) {
        if (handle->second == node) {
            m_handle_info.erase(handle->first);
            handle = m_handles.erase(handle);
        } else {
            ++handle;
        }
    }
}

void Registry::add_link_children(
    const Path& path, std::map<std::wstring, std::wstring, RegCaseInsensitive>& names) {
    // Only HKLM\SOFTWARE\WOW6432Node[\...] carries links.
    if (!is_hklm_software(path) || path.size() < 3 || !ieq(path[2], kWow)) return;
    Path logical(path);
    logical.erase(logical.begin() + 2);  // HKLM\SOFTWARE\<rest>
    if (logical.size() == 2) {
        Path classes32{path[0], path[1], L"Classes", std::wstring(kWow)};
        if (walk_physical(classes32, false) != nullptr) names.try_emplace(L"Classes", L"Classes");
    }
    for (const auto& entry : shared_software_keys()) {
        if (entry.size() != logical.size() - 1) continue;
        bool parent_match = true;
        for (usize i = 0; i + 1 < entry.size() && parent_match; ++i) {
            parent_match = ieq(entry[i], logical[2 + i]);
        }
        if (!parent_match) continue;
        Path target(logical);
        target.push_back(entry.back());
        if (RegNode* node = walk_physical(target, false)) names.try_emplace(node->name, node->name);
    }
}

RegHandle Registry::view_root(RegHandle root, RegView view) {
    std::lock_guard lock(m_mutex);
    if (view == RegView::Default) return root;
    const auto key = std::make_pair(root, view);
    if (const auto found = m_view_roots.find(key); found != m_view_roots.end()) {
        return found->second;
    }
    auto opened = open_key(root, L"", false, view);
    if (opened.is_err()) return root;
    m_view_roots.emplace(key, opened.value());
    return opened.value();
}

}  // namespace vitreo::capsule
