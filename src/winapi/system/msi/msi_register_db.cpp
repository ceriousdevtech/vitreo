// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_register_db.cpp
/// @brief The installed-products database (msi_register.hpp): where Windows
/// Installer records products, features, components, qualified components,
/// upgrade codes, source lists and usage, and the queries over it.
///
/// Layout and value formats: tools/oracle_probe/msi_registration_run.ps1
/// (machine/user/x64/upgrade scenarios, msi_registration_*.expected) and
/// msi_product_state_probe.c (MsiQueryProductState over every key mix).

#include "winapi/system/msi/msi_register.hpp"
#include "winapi/system/msi/msi.hpp"
#include "winapi/system/msi/msi_product.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "capsule/registry/registry.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <set>

namespace vitreo::winapi::msi {

namespace {

constexpr i32 kStateUnknown = -1, kStateBroken = 0, kStateAdvertised = 1, kStateAbsent = 2, kStateLocal = 3,
              kStateSource = 4, kStateDefault = 5;
constexpr u32 kErrSuccess = 0, kErrUnknownProperty = 1608;

const std::wstring kInstallerKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Installer";

std::wstring widen(std::string_view s) {
    const std::vector<u16> u = utf8_to_utf16(s);
    return std::wstring(u.begin(), u.end());
}

std::string narrow(std::wstring_view w) {
    std::vector<u16> u;
    u.reserve(w.size());
    for (const wchar_t c : w) u.push_back(static_cast<u16>(c));
    return utf16_to_utf8(u.data(), static_cast<int>(u.size()));
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

/// A value as text: strings verbatim (REG_EXPAND_SZ unexpanded), REG_DWORD /
/// REG_QWORD in decimal, REG_MULTI_SZ as its items each followed by NUL.
std::optional<std::string> value_text(const capsule::RegValue& v) {
    if (const auto* s = std::get_if<std::wstring>(&v.data)) return narrow(*s);
    if (const auto* d = std::get_if<u32>(&v.data)) return std::to_string(*d);
    if (const auto* q = std::get_if<u64>(&v.data)) return std::to_string(*q);
    if (const auto* m = std::get_if<std::vector<std::wstring>>(&v.data)) {
        // The REG_MULTI_SZ data as one string: each item and its terminator
        // (probe: Clients ":" reads back as 2 characters).
        std::string out;
        for (const auto& item : *m) {
            out += narrow(item);
            out.push_back('\0');
        }
        return out;
    }
    if (v.type == capsule::RegType::None) return std::string{};
    return std::nullopt;
}

/// RAII-free helpers over the capsule registry. The installer database is
/// written by the 64-bit Windows Installer service in the 64-bit layout
/// (32-bit packages name SOFTWARE\WOW6432Node explicitly), so every path is
/// opened in the 64-bit view whatever the calling process's bitness.
struct Reg {
    capsule::Registry& r;

    [[nodiscard]] bool exists(u64 hive, const std::wstring& path) const {
        auto k = r.open_key(r.view_root(hive, capsule::RegView::Native64), path, false);
        if (k.is_err()) return false;
        r.close_key(k.value());
        return true;
    }
    [[nodiscard]] std::optional<capsule::RegValue> get(u64 hive, const std::wstring& path, std::wstring_view name) const {
        auto k = r.open_key(r.view_root(hive, capsule::RegView::Native64), path, false);
        if (k.is_err()) return std::nullopt;
        auto v = r.query_value(k.value(), name);
        r.close_key(k.value());
        if (v.is_err()) return std::nullopt;
        return v.value();
    }
    [[nodiscard]] std::optional<std::string> text(u64 hive, const std::wstring& path, std::wstring_view name) const {
        auto v = get(hive, path, name);
        if (!v) return std::nullopt;
        return value_text(*v);
    }
    void set(u64 hive, const std::wstring& path, std::wstring_view name, const capsule::RegValue& value) const {
        auto k = r.open_key(r.view_root(hive, capsule::RegView::Native64), path, true);
        if (k.is_err()) return;
        (void)r.set_value(k.value(), name, value);
        r.close_key(k.value());
    }
    void set_sz(u64 hive, const std::wstring& path, std::wstring_view name, std::string_view value) const {
        set(hive, path, name, capsule::RegValue{capsule::RegType::Sz, widen(value)});
    }
    void touch(u64 hive, const std::wstring& path) const {
        auto k = r.open_key(r.view_root(hive, capsule::RegView::Native64), path, true);
        if (k.is_ok()) r.close_key(k.value());
    }
    void del_value(u64 hive, const std::wstring& path, std::wstring_view name) const {
        auto k = r.open_key(r.view_root(hive, capsule::RegView::Native64), path, false);
        if (k.is_err()) return;
        (void)r.delete_value(k.value(), name);
        r.close_key(k.value());
    }
    [[nodiscard]] std::vector<std::wstring> keys(u64 hive, const std::wstring& path) const {
        auto k = r.open_key(r.view_root(hive, capsule::RegView::Native64), path, false);
        if (k.is_err()) return {};
        auto names = r.enum_keys(k.value());
        r.close_key(k.value());
        return names.is_ok() ? names.value() : std::vector<std::wstring>{};
    }
    [[nodiscard]] std::vector<std::wstring> values(u64 hive, const std::wstring& path) const {
        auto k = r.open_key(r.view_root(hive, capsule::RegView::Native64), path, false);
        if (k.is_err()) return {};
        auto names = r.enum_values(k.value());
        r.close_key(k.value());
        return names.is_ok() ? names.value() : std::vector<std::wstring>{};
    }
    /// RegDeleteTree of @p path (the key itself included).
    void del_tree(u64 hive, const std::wstring& path) const {
        for (const std::wstring& child : keys(hive, path)) del_tree(hive, path + L"\\" + child);
        const usize slash = path.rfind(L'\\');
        if (slash == std::wstring::npos) return;
        auto parent = r.open_key(r.view_root(hive, capsule::RegView::Native64), path.substr(0, slash), false);
        if (parent.is_err()) return;
        (void)r.delete_key(parent.value(), path.substr(slash + 1));
        r.close_key(parent.value());
    }
    /// Delete @p path when it has neither values nor subkeys, then its
    /// parents up to (not including) @p stop, the way Windows removes a
    /// product's keys once their last value goes.
    void prune(u64 hive, std::wstring path, const std::wstring& stop) const {
        while (path.size() > stop.size() && exists(hive, path) && keys(hive, path).empty() &&
               values(hive, path).empty()) {
            del_tree(hive, path);
            const usize slash = path.rfind(L'\\');
            if (slash == std::wstring::npos) return;
            path.resize(slash);
        }
    }
};

/// The user SIDs that have any per-user data: UserData\<SID> (minus the
/// machine) and Managed\<SID>.
std::vector<std::string> known_user_sids(Reg reg) {
    std::set<std::string> sids;
    for (const auto& s : reg.keys(capsule::HKEY_LOCAL_MACHINE, kInstallerKey + L"\\UserData"))
        if (upper(narrow(s)) != kMachineSid) sids.insert(upper(narrow(s)));
    for (const auto& s : reg.keys(capsule::HKEY_LOCAL_MACHINE, kInstallerKey + L"\\Managed"))
        sids.insert(upper(narrow(s)));
    return {sids.begin(), sids.end()};
}

}  // namespace

// ---- GUID text forms -------------------------------------------------------------------

std::string pack_guid(std::string_view guid) {
    if (!is_guid(guid)) return {};
    std::string hex;
    for (const char c : guid)
        if (std::isxdigit(static_cast<unsigned char>(c))) hex.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    std::reverse(hex.begin(), hex.begin() + 8);
    std::reverse(hex.begin() + 8, hex.begin() + 12);
    std::reverse(hex.begin() + 12, hex.begin() + 16);
    for (usize i = 16; i < 32; i += 2) std::swap(hex[i], hex[i + 1]);
    return hex;
}

std::string unpack_guid(std::string_view packed) {
    if (packed.size() != 32) return {};
    std::string hex;
    for (const char c : packed) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return {};
        hex.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    // The packing is an involution.
    std::reverse(hex.begin(), hex.begin() + 8);
    std::reverse(hex.begin() + 8, hex.begin() + 12);
    std::reverse(hex.begin() + 12, hex.begin() + 16);
    for (usize i = 16; i < 32; i += 2) std::swap(hex[i], hex[i + 1]);
    std::string out = "{";
    for (usize i = 0; i < 32; ++i) {
        if (i == 8 || i == 12 || i == 16 || i == 20) out.push_back('-');
        out.push_back(hex[i]);
    }
    return out + "}";
}

// ---- ProductDatabase -------------------------------------------------------------------

ProductDatabase::ProductDatabase(capsule::Registry& registry, std::string user_sid)
    : m_registry(registry), m_user_sid(upper(user_sid)) {}

ProductDatabase::Keys ProductDatabase::keys(u32 context, std::string_view sid_in) const {
    Keys k;
    const std::string sid = sid_in.empty() ? m_user_sid : upper(sid_in);
    switch (context) {
        case kContextMachine:
            k.advertised_hive = capsule::HKEY_LOCAL_MACHINE;
            k.advertised_root = L"SOFTWARE\\Classes\\Installer";
            k.user_data = kInstallerKey + L"\\UserData\\" + widen(kMachineSid);
            break;
        case kContextUserManaged:
            k.advertised_hive = capsule::HKEY_LOCAL_MACHINE;
            k.advertised_root = kInstallerKey + L"\\Managed\\" + widen(sid) + L"\\Installer";
            k.user_data = kInstallerKey + L"\\UserData\\" + widen(sid);
            break;
        case kContextUserUnmanaged:
            if (sid == m_user_sid) {
                k.advertised_hive = capsule::HKEY_CURRENT_USER;
                k.advertised_root = L"Software\\Microsoft\\Installer";
            } else {
                k.advertised_hive = capsule::HKEY_USERS;
                k.advertised_root = widen(sid) + L"\\Software\\Microsoft\\Installer";
            }
            k.user_data = kInstallerKey + L"\\UserData\\" + widen(sid);
            break;
        default: break;
    }
    return k;
}

i32 ProductDatabase::instance_state(const ProductInstance& inst) const {
    const auto k = keys(inst.context, inst.sid);
    const std::wstring p = widen(pack_guid(inst.product_code));
    if (p.empty() || k.advertised_root.empty()) return kStateUnknown;
    Reg reg{m_registry};
    const bool published = reg.exists(k.advertised_hive, k.advertised_root + L"\\Products\\" + p);
    const bool installed =
        reg.get(capsule::HKEY_LOCAL_MACHINE, k.user_data + L"\\Products\\" + p + L"\\InstallProperties", L"WindowsInstaller")
            .has_value();
    if (published) return installed ? kStateDefault : kStateAdvertised;
    return installed ? kStateAbsent : kStateUnknown;
}

std::vector<ProductInstance> ProductDatabase::instances(u32 contexts, std::string_view sid_in,
                                                        std::string_view product) const {
    Reg reg{m_registry};
    std::vector<std::string> sids;
    const std::string sid = upper(sid_in);
    if (sid.empty()) sids = {m_user_sid};
    else if (sid == kEveryoneSid) {
        sids = known_user_sids(reg);
        if (std::find(sids.begin(), sids.end(), m_user_sid) == sids.end()) sids.insert(sids.begin(), m_user_sid);
    } else sids = {sid};
    const std::string want = product.empty() ? std::string{} : pack_guid(product);
    std::vector<ProductInstance> out;
    auto scan = [&](u32 context, const std::string& s) {
        const auto k = keys(context, s);
        for (const auto& name : reg.keys(k.advertised_hive, k.advertised_root + L"\\Products")) {
            const std::string packed = upper(narrow(name));
            if (!want.empty() && packed != want) continue;
            const std::string code = unpack_guid(packed);
            if (code.empty()) continue;
            out.push_back(ProductInstance{code, context, context == kContextMachine ? std::string{} : s});
        }
    };
    if (contexts & kContextUserManaged)
        for (const auto& s : sids) scan(kContextUserManaged, s);
    if (contexts & kContextUserUnmanaged)
        for (const auto& s : sids) scan(kContextUserUnmanaged, s);
    if (contexts & kContextMachine) scan(kContextMachine, {});
    return out;
}

bool ProductDatabase::malformed_products(u32 contexts, std::string_view sid) const {
    Reg reg{m_registry};
    std::vector<std::pair<u32, std::string>> scans;
    const std::string s = sid.empty() ? m_user_sid : upper(sid);
    if (contexts & kContextUserManaged) scans.emplace_back(kContextUserManaged, s);
    if (contexts & kContextUserUnmanaged) scans.emplace_back(kContextUserUnmanaged, s);
    if (contexts & kContextMachine) scans.emplace_back(kContextMachine, std::string{});
    for (const auto& [c, who] : scans) {
        if (who == kEveryoneSid) continue;
        const auto k = keys(c, who);
        for (const auto& name : reg.keys(k.advertised_hive, k.advertised_root + L"\\Products"))
            if (unpack_guid(narrow(name)).empty()) return true;
    }
    return false;
}

std::optional<ProductInstance> ProductDatabase::visible(std::string_view product) const {
    if (!is_guid(product)) return std::nullopt;
    for (const u32 c : {kContextUserManaged, kContextUserUnmanaged, kContextMachine}) {
        ProductInstance inst{upper(product), c, c == kContextMachine ? std::string{} : m_user_sid};
        const i32 s = instance_state(inst);
        if (s == kStateDefault || s == kStateAdvertised) return inst;
    }
    return std::nullopt;
}

std::optional<ProductInstance> ProductDatabase::installed(std::string_view product) const {
    if (auto v = visible(product)) return v;
    if (!is_guid(product)) return std::nullopt;
    for (const u32 c : {kContextUserUnmanaged, kContextMachine}) {
        ProductInstance inst{upper(product), c, c == kContextMachine ? std::string{} : m_user_sid};
        if (instance_state(inst) == kStateAbsent) return inst;
    }
    return std::nullopt;
}

i32 ProductDatabase::product_state(std::string_view product) const {
    if (!is_guid(product)) return -2;  // INSTALLSTATE_INVALIDARG
    if (auto v = visible(product)) return instance_state(*v);
    // Installed for this user or the machine but not published to them, or
    // installed for another user: ABSENT (msi_product_state_probe).
    if (installed(product)) return kStateAbsent;
    Reg reg{m_registry};
    const std::wstring p = widen(pack_guid(product));
    for (const auto& s : reg.keys(capsule::HKEY_LOCAL_MACHINE, kInstallerKey + L"\\UserData"))
        if (reg.get(capsule::HKEY_LOCAL_MACHINE,
                    kInstallerKey + L"\\UserData\\" + s + L"\\Products\\" + p + L"\\InstallProperties",
                    L"WindowsInstaller"))
            return kStateAbsent;
    return kStateUnknown;
}

std::optional<std::string> ProductDatabase::install_property(const ProductInstance& inst, std::string_view name) const {
    const auto k = keys(inst.context, inst.sid);
    return Reg{m_registry}.text(capsule::HKEY_LOCAL_MACHINE,
                                k.user_data + L"\\Products\\" + widen(pack_guid(inst.product_code)) + L"\\InstallProperties",
                                widen(name));
}

std::optional<std::string> ProductDatabase::product_key_value(const ProductInstance& inst, std::string_view name) const {
    const auto k = keys(inst.context, inst.sid);
    return Reg{m_registry}.text(k.advertised_hive, k.advertised_root + L"\\Products\\" + widen(pack_guid(inst.product_code)),
                                widen(name));
}

std::optional<std::string> ProductDatabase::source_list_value(const ProductInstance& inst, std::string_view sub,
                                                              std::string_view name) const {
    const auto k = keys(inst.context, inst.sid);
    std::wstring path = k.advertised_root + L"\\Products\\" + widen(pack_guid(inst.product_code)) + L"\\SourceList";
    if (!sub.empty()) path += L"\\" + widen(sub);
    return Reg{m_registry}.text(k.advertised_hive, path, widen(name));
}

std::string ProductDatabase::local_package(const ProductInstance& inst) const {
    return install_property(inst, "LocalPackage").value_or(std::string{});
}

u32 ProductDatabase::product_info(const ProductInstance& inst, std::string_view prop, std::string& out,
                                  bool extended) const {
    // The names MsiGetProductInfo(Ex) accept, case-sensitively, and where each
    // is read (msi_registration machine/user .api lines: an unlisted name is
    // ERROR_UNKNOWN_PROPERTY even when InstallProperties holds it, e.g.
    // Comments; a listed one that is unset is "").
    struct Map {
        const char* name;
        enum Where { Product, Source, Media, Installed, PackageCode, State } where;
        const char* value;
    };
    static constexpr Map kMap[] = {
        {"PackageName", Map::Source, "PackageName"},
        {"Transforms", Map::Product, "Transforms"},
        {"Language", Map::Product, "Language"},
        {"ProductName", Map::Product, "ProductName"},
        {"AssignmentType", Map::Product, "Assignment"},
        {"InstanceType", Map::Product, "InstanceType"},
        {"AuthorizedLUAApp", Map::Product, "AuthorizedLUAApp"},
        {"PackageCode", Map::PackageCode, "PackageCode"},
        {"Version", Map::Product, "Version"},
        {"ProductIcon", Map::Product, "ProductIcon"},
        {"Clients", Map::Product, "Clients"},
        {"AdvertiseFlags", Map::Product, "AdvertiseFlags"},
        {"DeploymentFlags", Map::Product, "DeploymentFlags"},
        // GetProductInfo returns the raw LastUsedSource for both names;
        // MsiSourceListGetInfo splits it (probe "info LastUsedType").
        {"LastUsedSource", Map::Source, "LastUsedSource"},
        {"LastUsedType", Map::Source, "LastUsedSource"},
        {"MediaPackagePath", Map::Media, "MediaPackagePath"},
        {"DiskPrompt", Map::Media, "DiskPrompt"},
        {"InstalledProductName", Map::Installed, "DisplayName"},
        {"VersionString", Map::Installed, "DisplayVersion"},
        {"HelpLink", Map::Installed, "HelpLink"},
        {"HelpTelephone", Map::Installed, "HelpTelephone"},
        {"InstallLocation", Map::Installed, "InstallLocation"},
        {"InstallSource", Map::Installed, "InstallSource"},
        {"InstallDate", Map::Installed, "InstallDate"},
        {"Publisher", Map::Installed, "Publisher"},
        {"LocalPackage", Map::Installed, "LocalPackage"},
        {"URLInfoAbout", Map::Installed, "URLInfoAbout"},
        {"URLUpdateInfo", Map::Installed, "URLUpdateInfo"},
        {"VersionMinor", Map::Installed, "VersionMinor"},
        {"VersionMajor", Map::Installed, "VersionMajor"},
        {"ProductID", Map::Installed, "ProductID"},
        {"RegCompany", Map::Installed, "RegCompany"},
        {"RegOwner", Map::Installed, "RegOwner"},
        {"InstalledLanguage", Map::Installed, "Language"},
        {"WindowsInstaller", Map::Installed, "WindowsInstaller"},
        {"State", Map::State, ""},
    };
    const Map* m = nullptr;
    for (const Map& e : kMap)
        if (prop == e.name) m = &e;
    if (m == nullptr || (m->where == Map::State && !extended)) return kErrUnknownProperty;
    const i32 state = instance_state(inst);
    switch (m->where) {
        case Map::State:
            out = std::to_string(state == kStateDefault ? kStateDefault : kStateAdvertised);
            return kErrSuccess;
        case Map::Product: out = product_key_value(inst, m->value).value_or(""); return kErrSuccess;
        case Map::PackageCode: out = unpack_guid(product_key_value(inst, m->value).value_or("")); return kErrSuccess;
        case Map::Source: out = source_list_value(inst, "", m->value).value_or(""); return kErrSuccess;
        case Map::Media: out = source_list_value(inst, "Media", m->value).value_or(""); return kErrSuccess;
        case Map::Installed:
            // Learn: an installed-only property of an advertised product is
            // ERROR_UNKNOWN_PROPERTY.
            if (state != kStateDefault && state != kStateAbsent) return kErrUnknownProperty;
            out = install_property(inst, m->value).value_or("");
            return kErrSuccess;
    }
    return kErrUnknownProperty;
}

// ---- features --------------------------------------------------------------------------

std::vector<std::pair<std::string, std::string>> ProductDatabase::features(const ProductInstance& inst) const {
    Reg reg{m_registry};
    const auto k = keys(inst.context, inst.sid);
    const std::wstring path = k.advertised_root + L"\\Features\\" + widen(pack_guid(inst.product_code));
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& name : reg.values(k.advertised_hive, path)) {
        std::string parent = reg.text(k.advertised_hive, path, name).value_or("");
        if (!parent.empty() && parent[0] == '\x06') parent.erase(0, 1);  // absent marker
        out.emplace_back(narrow(name), parent);
    }
    return out;
}

i32 ProductDatabase::feature_state(const ProductInstance& inst, std::string_view feature) const {
    Reg reg{m_registry};
    const auto k = keys(inst.context, inst.sid);
    const std::wstring p = widen(pack_guid(inst.product_code));
    const auto published = reg.text(k.advertised_hive, k.advertised_root + L"\\Features\\" + p, widen(feature));
    if (!published) return kStateUnknown;
    if (!published->empty() && (*published)[0] == '\x06') return kStateAbsent;
    const auto installed = reg.text(capsule::HKEY_LOCAL_MACHINE, k.user_data + L"\\Products\\" + p + L"\\Features",
                                    widen(feature));
    if (!installed) return kStateAdvertised;
    // Components: 20-character compressed codes up to an optional "\x02parent".
    const std::string list = installed->substr(0, installed->find('\x02'));
    i32 state = kStateLocal;
    bool any_source = false;
    for (usize i = 0; i + 20 <= list.size(); i += 20) {
        const std::vector<u16> u = utf8_to_utf16(list.substr(i, 20));
        const auto code = decompress_guid(std::u16string(u.begin(), u.end()));
        if (!code) return kStateBroken;
        const i32 cs = component_state(inst, *code);
        if (cs == kStateUnknown || cs == kStateAbsent) return kStateBroken;
        if (cs == kStateSource) any_source = true;
    }
    if (any_source) state = kStateSource;
    return state;
}

std::vector<std::string> ProductDatabase::feature_component_codes(const ProductInstance& inst,
                                                                 std::string_view feature) const {
    const auto k = keys(inst.context, inst.sid);
    const auto installed = Reg{m_registry}.text(
        capsule::HKEY_LOCAL_MACHINE, k.user_data + L"\\Products\\" + widen(pack_guid(inst.product_code)) + L"\\Features",
        widen(feature));
    std::vector<std::string> out;
    if (!installed) return out;
    const std::string list = installed->substr(0, installed->find('\x02'));
    for (usize i = 0; i + 20 <= list.size(); i += 20) {
        const std::vector<u16> u = utf8_to_utf16(list.substr(i, 20));
        if (auto code = decompress_guid(std::u16string(u.begin(), u.end()))) out.push_back(upper(*code));
    }
    return out;
}

std::map<std::string, i32> ProductDatabase::feature_states(const ProductInstance& inst) const {
    std::map<std::string, i32> out;
    for (const auto& [f, parent] : features(inst)) out[f] = feature_state(inst, f);
    return out;
}

// ---- components ------------------------------------------------------------------------

std::optional<std::string> ProductDatabase::component_key_path(const ProductInstance& inst,
                                                               std::string_view component) const {
    const auto k = keys(inst.context, inst.sid);
    return Reg{m_registry}.text(capsule::HKEY_LOCAL_MACHINE,
                                k.user_data + L"\\Components\\" + widen(pack_guid(component)),
                                widen(pack_guid(inst.product_code)));
}

i32 ProductDatabase::component_state(const ProductInstance& inst, std::string_view component) const {
    const auto kp = component_key_path(inst, component);
    if (!kp) return kStateUnknown;
    if (kp->empty()) return kStateAbsent;
    // "NN:\..." is a local registry key path, "NN" alone (two digits) a
    // run-from-source registry form, "NN?" with a source directory prefix a
    // source file; a plain path is local.
    const std::string& s = *kp;
    const bool digits = s.size() >= 2 && std::isdigit(static_cast<unsigned char>(s[0])) &&
                        std::isdigit(static_cast<unsigned char>(s[1]));
    if (digits && (s.size() == 2 || s[2] != ':')) return kStateSource;
    return kStateLocal;
}

std::vector<std::pair<ProductInstance, std::string>> ProductDatabase::component_clients(std::string_view component,
                                                                                        u32 contexts,
                                                                                        std::string_view sid) const {
    Reg reg{m_registry};
    std::vector<std::pair<ProductInstance, std::string>> out;
    const std::wstring c = widen(pack_guid(component));
    if (c.empty()) return out;
    auto scan = [&](u32 context, const std::string& s) {
        const auto k = keys(context, s);
        const std::wstring path = k.user_data + L"\\Components\\" + c;
        for (const auto& name : reg.values(capsule::HKEY_LOCAL_MACHINE, path)) {
            const std::string code = unpack_guid(narrow(name));
            if (code.empty()) continue;  // the shared-component refcount values
            ProductInstance inst{code, context, context == kContextMachine ? std::string{} : s};
            // A user's UserData holds both managed and unmanaged instances:
            // the advertised key says which.
            if (context != kContextMachine) {
                const i32 st = instance_state(inst);
                if (st != kStateDefault && st != kStateAdvertised) continue;
            }
            out.emplace_back(inst, reg.text(capsule::HKEY_LOCAL_MACHINE, path, name).value_or(""));
        }
    };
    std::vector<std::string> sids;
    const std::string want = upper(sid);
    if (want.empty()) sids = {m_user_sid};
    else if (want == kEveryoneSid) sids = known_user_sids(reg);
    else sids = {want};
    if (contexts & kContextUserManaged)
        for (const auto& s : sids) scan(kContextUserManaged, s);
    if (contexts & kContextUserUnmanaged)
        for (const auto& s : sids) scan(kContextUserUnmanaged, s);
    if (contexts & kContextMachine) scan(kContextMachine, {});
    return out;
}

std::vector<ProductDatabase::RegisteredComponent> ProductDatabase::components(u32 contexts, std::string_view sid) const {
    Reg reg{m_registry};
    std::vector<RegisteredComponent> out;
    std::vector<std::string> sids;
    const std::string want = upper(sid);
    if (want.empty()) sids = {m_user_sid};
    else if (want == kEveryoneSid) sids = known_user_sids(reg);
    else sids = {want};
    auto scan = [&](u32 context, const std::string& s) {
        const auto k = keys(context, s);
        for (const auto& name : reg.keys(capsule::HKEY_LOCAL_MACHINE, k.user_data + L"\\Components")) {
            const std::string code = unpack_guid(narrow(name));
            if (code.empty()) continue;
            if (!component_clients(code, context, context == kContextMachine ? std::string_view{} : s).empty())
                out.push_back(RegisteredComponent{code, context, context == kContextMachine ? std::string{} : s});
        }
    };
    if (contexts & kContextUserManaged)
        for (const auto& s : sids) scan(kContextUserManaged, s);
    if (contexts & kContextUserUnmanaged)
        for (const auto& s : sids) scan(kContextUserUnmanaged, s);
    if (contexts & kContextMachine) scan(kContextMachine, {});
    return out;
}

std::vector<std::string> ProductDatabase::related_products(std::string_view upgrade_code) const {
    Reg reg{m_registry};
    std::vector<std::string> out;
    const std::wstring u = widen(pack_guid(upgrade_code));
    if (u.empty()) return out;
    std::set<std::string> seen;
    auto add = [&](u64 hive, const std::wstring& path) {
        for (const auto& name : reg.values(hive, path)) {
            const std::string code = unpack_guid(narrow(name));
            if (!code.empty() && seen.insert(code).second) out.push_back(code);
        }
    };
    // Visible publications first (per-user, then machine), the machine-wide
    // map last; Windows lists a product once.
    const auto mine = keys(kContextUserUnmanaged, {});
    add(capsule::HKEY_LOCAL_MACHINE, keys(kContextUserManaged, {}).advertised_root + L"\\UpgradeCodes\\" + u);
    add(mine.advertised_hive, mine.advertised_root + L"\\UpgradeCodes\\" + u);
    add(capsule::HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\Installer\\UpgradeCodes\\" + u);
    // Only products the caller can see count (MsiEnumRelatedProducts).
    std::vector<std::string> visible_codes;
    for (const auto& code : out)
        if (visible(code)) visible_codes.push_back(code);
    return visible_codes;
}

std::string ProductDatabase::product_from_package_code(std::string_view package_code) const {
    const std::string packed = pack_guid(package_code);
    if (packed.empty()) return {};
    for (const auto& inst : instances(kContextAll, kEveryoneSid))
        if (upper(product_key_value(inst, "PackageCode").value_or("")) == packed) return inst.product_code;
    return {};
}

// ---- writes ----------------------------------------------------------------------------

void ProductDatabase::register_product(const ProductInstance& inst, const std::map<std::string, std::string>& strings,
                                       const std::map<std::string, u32>& dwords,
                                       const std::map<std::string, std::string>& expand_strings, bool package_64bit) {
    Reg reg{m_registry};
    const auto k = keys(inst.context, inst.sid);
    const std::wstring base = k.user_data + L"\\Products\\" + widen(pack_guid(inst.product_code));
    const std::wstring arp = std::wstring(package_64bit ? L"SOFTWARE" : L"SOFTWARE\\WOW6432Node") +
                             L"\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + widen(inst.product_code);
    for (const std::wstring& path : {base + L"\\InstallProperties", arp}) {
        for (const auto& [n, v] : strings) reg.set_sz(capsule::HKEY_LOCAL_MACHINE, path, widen(n), v);
        for (const auto& [n, v] : expand_strings)
            reg.set(capsule::HKEY_LOCAL_MACHINE, path, widen(n), capsule::RegValue{capsule::RegType::ExpandSz, widen(v)});
        for (const auto& [n, v] : dwords)
            reg.set(capsule::HKEY_LOCAL_MACHINE, path, widen(n), capsule::RegValue{capsule::RegType::Dword, v});
    }
    // LocalPackage lives only in InstallProperties (measured: absent from the ARP key).
    reg.del_value(capsule::HKEY_LOCAL_MACHINE, arp, L"LocalPackage");
    reg.set_sz(capsule::HKEY_LOCAL_MACHINE, base + L"\\Patches", L"AllPatches", "");
    reg.touch(capsule::HKEY_LOCAL_MACHINE, base + L"\\Usage");
}

void ProductDatabase::unregister_product(const ProductInstance& inst, std::string_view upgrade_code, bool package_64bit) {
    Reg reg{m_registry};
    const auto k = keys(inst.context, inst.sid);
    reg.del_tree(capsule::HKEY_LOCAL_MACHINE, k.user_data + L"\\Products\\" + widen(pack_guid(inst.product_code)));
    reg.del_tree(capsule::HKEY_LOCAL_MACHINE, std::wstring(package_64bit ? L"SOFTWARE" : L"SOFTWARE\\WOW6432Node") +
                                                  L"\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" +
                                                  widen(inst.product_code));
    if (!upgrade_code.empty()) {
        const std::wstring path = kInstallerKey + L"\\UpgradeCodes\\" + widen(pack_guid(upgrade_code));
        reg.del_value(capsule::HKEY_LOCAL_MACHINE, path, widen(pack_guid(inst.product_code)));
        reg.prune(capsule::HKEY_LOCAL_MACHINE, path, kInstallerKey + L"\\UpgradeCodes");
    }
}

void ProductDatabase::register_user(const ProductInstance& inst, std::string_view owner, std::string_view company,
                                    std::string_view product_id) {
    Reg reg{m_registry};
    const auto k = keys(inst.context, inst.sid);
    const std::wstring path = k.user_data + L"\\Products\\" + widen(pack_guid(inst.product_code)) + L"\\InstallProperties";
    reg.set_sz(capsule::HKEY_LOCAL_MACHINE, path, L"RegOwner", owner);
    reg.set_sz(capsule::HKEY_LOCAL_MACHINE, path, L"RegCompany", company);
    reg.set_sz(capsule::HKEY_LOCAL_MACHINE, path, L"ProductID", product_id);
}

void ProductDatabase::publish_product(const ProductInstance& inst, const Publication& pub) {
    Reg reg{m_registry};
    const auto k = keys(inst.context, inst.sid);
    const std::wstring p = widen(pack_guid(inst.product_code));
    const std::wstring key = k.advertised_root + L"\\Products\\" + p;
    const u64 h = k.advertised_hive;
    auto dword = [&](const wchar_t* n, u32 v) { reg.set(h, key, n, capsule::RegValue{capsule::RegType::Dword, v}); };
    // ProductPublish (measured value set and types).
    reg.set_sz(h, key, L"ProductName", pub.product_name);
    reg.set_sz(h, key, L"PackageCode", pack_guid(pub.package_code));
    dword(L"Language", pub.language);
    dword(L"Version", pub.version);
    dword(L"Assignment", pub.assignment);
    dword(L"AdvertiseFlags", 388);
    dword(L"InstanceType", 0);
    dword(L"AuthorizedLUAApp", 0);
    dword(L"DeploymentFlags", 3);
    reg.set(h, key, L"Clients", capsule::RegValue{capsule::RegType::MultiSz, std::vector<std::wstring>{L":"}});
    if (!pub.product_icon.empty()) reg.set_sz(h, key, L"ProductIcon", pub.product_icon);
    // SourceListPublish / SourceListRegisterLastUsed.
    const std::wstring sl = key + L"\\SourceList";
    reg.set_sz(h, sl, L"PackageName", pub.package_name);
    reg.set(h, sl, L"LastUsedSource",
            capsule::RegValue{capsule::RegType::ExpandSz, widen("n;1;" + pub.last_used_source)});
    for (usize i = 0; i < pub.net_sources.size(); ++i)
        reg.set(h, sl + L"\\Net", widen(std::to_string(i + 1)),
                capsule::RegValue{capsule::RegType::ExpandSz, widen(pub.net_sources[i])});
    for (usize i = 0; i < pub.url_sources.size(); ++i)
        reg.set(h, sl + L"\\URL", widen(std::to_string(i + 1)),
                capsule::RegValue{capsule::RegType::ExpandSz, widen(pub.url_sources[i])});
    for (const auto& [disk, text] : pub.media) reg.set_sz(h, sl + L"\\Media", widen(std::to_string(disk)), text);
    if (!pub.media_package_path.empty()) reg.set_sz(h, sl + L"\\Media", L"MediaPackagePath", pub.media_package_path);
    if (!pub.disk_prompt.empty()) reg.set_sz(h, sl + L"\\Media", L"DiskPrompt", pub.disk_prompt);
    // UpgradeCodePublish: the context's own map and the machine-wide one.
    if (!pack_guid(pub.upgrade_code).empty()) {
        const std::wstring u = widen(pack_guid(pub.upgrade_code));
        reg.set_sz(h, k.advertised_root + L"\\UpgradeCodes\\" + u, p, "");
        reg.set_sz(capsule::HKEY_LOCAL_MACHINE, kInstallerKey + L"\\UpgradeCodes\\" + u, p, "");
    }
}

void ProductDatabase::unpublish_product(const ProductInstance& inst, std::string_view upgrade_code) {
    Reg reg{m_registry};
    const auto k = keys(inst.context, inst.sid);
    const std::wstring p = widen(pack_guid(inst.product_code));
    reg.del_tree(k.advertised_hive, k.advertised_root + L"\\Products\\" + p);
    reg.del_tree(k.advertised_hive, k.advertised_root + L"\\Features\\" + p);
    if (!pack_guid(upgrade_code).empty()) {
        const std::wstring u = widen(pack_guid(upgrade_code));
        const std::wstring own = k.advertised_root + L"\\UpgradeCodes\\" + u;
        reg.del_value(k.advertised_hive, own, p);
        reg.prune(k.advertised_hive, own, k.advertised_root + L"\\UpgradeCodes");
        const std::wstring machine = kInstallerKey + L"\\UpgradeCodes\\" + u;
        reg.del_value(capsule::HKEY_LOCAL_MACHINE, machine, p);
        reg.prune(capsule::HKEY_LOCAL_MACHINE, machine, kInstallerKey + L"\\UpgradeCodes");
    }
}

void ProductDatabase::publish_feature(const ProductInstance& inst, std::string_view feature, std::string_view parent,
                                      bool absent, const std::vector<std::string>& components) {
    Reg reg{m_registry};
    const auto k = keys(inst.context, inst.sid);
    const std::wstring p = widen(pack_guid(inst.product_code));
    std::string published = absent ? std::string("\x06") : std::string{};
    published += parent;
    reg.set_sz(k.advertised_hive, k.advertised_root + L"\\Features\\" + p, widen(feature), published);
    std::string list;
    for (const auto& c : components) list += compress_guid(c);
    if (!parent.empty()) list += std::string("\x02") + std::string(parent);
    reg.set_sz(capsule::HKEY_LOCAL_MACHINE, k.user_data + L"\\Products\\" + p + L"\\Features", widen(feature), list);
}

void ProductDatabase::unpublish_feature(const ProductInstance& inst, std::string_view feature) {
    Reg reg{m_registry};
    const auto k = keys(inst.context, inst.sid);
    const std::wstring p = widen(pack_guid(inst.product_code));
    const std::wstring adv = k.advertised_root + L"\\Features\\" + p;
    reg.del_value(k.advertised_hive, adv, widen(feature));
    reg.prune(k.advertised_hive, adv, k.advertised_root + L"\\Features");
    const std::wstring ud = k.user_data + L"\\Products\\" + p + L"\\Features";
    reg.del_value(capsule::HKEY_LOCAL_MACHINE, ud, widen(feature));
    reg.prune(capsule::HKEY_LOCAL_MACHINE, ud, k.user_data + L"\\Products\\" + p);
}

void ProductDatabase::register_component(const ProductInstance& inst, std::string_view component,
                                         std::string_view key_path) {
    const auto k = keys(inst.context, inst.sid);
    Reg{m_registry}.set_sz(capsule::HKEY_LOCAL_MACHINE, k.user_data + L"\\Components\\" + widen(pack_guid(component)),
                           widen(pack_guid(inst.product_code)), key_path);
}

void ProductDatabase::unregister_component(const ProductInstance& inst, std::string_view component) {
    Reg reg{m_registry};
    const auto k = keys(inst.context, inst.sid);
    const std::wstring path = k.user_data + L"\\Components\\" + widen(pack_guid(component));
    reg.del_value(capsule::HKEY_LOCAL_MACHINE, path, widen(pack_guid(inst.product_code)));
    reg.prune(capsule::HKEY_LOCAL_MACHINE, path, k.user_data + L"\\Components");
}

void ProductDatabase::publish_qualified_component(const ProductInstance& inst, std::string_view category,
                                                  std::string_view qualifier, std::string_view descriptor,
                                                  std::string_view app_data) {
    Reg reg{m_registry};
    const auto k = keys(inst.context, inst.sid);
    const std::wstring path = k.advertised_root + L"\\Components\\" + widen(pack_guid(category));
    // One REG_MULTI_SZ per qualifier: an entry per publishing product
    // (descriptor followed directly by AppData), newest last.
    std::vector<std::wstring> entries;
    if (auto v = reg.get(k.advertised_hive, path, widen(qualifier)))
        if (auto* m = std::get_if<std::vector<std::wstring>>(&v->data)) entries = *m;
    const std::wstring d = widen(descriptor);
    std::erase_if(entries, [&](const std::wstring& e) { return e.compare(0, d.size(), d) == 0; });
    entries.push_back(d + widen(app_data));
    reg.set(k.advertised_hive, path, widen(qualifier), capsule::RegValue{capsule::RegType::MultiSz, entries});
}

void ProductDatabase::unpublish_qualified_component(const ProductInstance& inst, std::string_view category,
                                                    std::string_view qualifier, std::string_view descriptor) {
    Reg reg{m_registry};
    const auto k = keys(inst.context, inst.sid);
    const std::wstring path = k.advertised_root + L"\\Components\\" + widen(pack_guid(category));
    auto v = reg.get(k.advertised_hive, path, widen(qualifier));
    if (!v) return;
    std::vector<std::wstring> entries;
    if (auto* m = std::get_if<std::vector<std::wstring>>(&v->data)) entries = *m;
    const std::wstring d = widen(descriptor);
    std::erase_if(entries, [&](const std::wstring& e) { return e.compare(0, d.size(), d) == 0; });
    if (entries.empty()) reg.del_value(k.advertised_hive, path, widen(qualifier));
    else reg.set(k.advertised_hive, path, widen(qualifier), capsule::RegValue{capsule::RegType::MultiSz, entries});
    reg.prune(k.advertised_hive, path, k.advertised_root + L"\\Components");
}

std::vector<ProductDatabase::Qualifier> ProductDatabase::qualifiers(std::string_view category) const {
    Reg reg{m_registry};
    std::vector<Qualifier> out;
    const std::wstring c = widen(pack_guid(category));
    if (c.empty()) return out;
    std::set<std::string> seen;
    for (const u32 ctx : {kContextUserManaged, kContextUserUnmanaged, kContextMachine}) {
        const auto k = keys(ctx, {});
        const std::wstring path = k.advertised_root + L"\\Components\\" + c;
        for (const auto& name : reg.values(k.advertised_hive, path)) {
            const std::string q = narrow(name);
            if (!seen.insert(q).second) continue;
            auto v = reg.get(k.advertised_hive, path, name);
            const auto* m = v ? std::get_if<std::vector<std::wstring>>(&v->data) : nullptr;
            if (m == nullptr || m->empty()) continue;
            const std::string entry = narrow(m->front());
            const std::vector<u16> u = utf8_to_utf16(entry);
            Descriptor d;
            if (decompose_descriptor(std::u16string(u.begin(), u.end()), d) != 0) continue;
            const std::u16string head(u.begin(), u.begin() + static_cast<std::ptrdiff_t>(d.consumed));
            out.push_back(Qualifier{q, utf16_to_utf8(reinterpret_cast<const u16*>(head.data()), static_cast<int>(head.size())),
                                    utf16_to_utf8(u.data() + d.consumed, static_cast<int>(u.size() - d.consumed))});
        }
    }
    return out;
}

void ProductDatabase::record_feature_use(const ProductInstance& inst, std::string_view feature) {
    Reg reg{m_registry};
    const auto k = keys(inst.context, inst.sid);
    const std::wstring path = k.user_data + L"\\Products\\" + widen(pack_guid(inst.product_code)) + L"\\Usage";
    auto [count, date] = feature_usage(inst, feature);
    // Usage\<feature>: REG_DWORD, high word the MS-DOS date of the last use,
    // low word the use count (MsiGetFeatureUsage).
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    const u16 dos = static_cast<u16>(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
    (void)date;
    reg.set(capsule::HKEY_LOCAL_MACHINE, path, widen(feature),
            capsule::RegValue{capsule::RegType::Dword, (static_cast<u32>(dos) << 16) | ((count + 1) & 0xFFFF)});
}

std::pair<u32, u16> ProductDatabase::feature_usage(const ProductInstance& inst, std::string_view feature) const {
    const auto k = keys(inst.context, inst.sid);
    auto v = Reg{m_registry}.get(capsule::HKEY_LOCAL_MACHINE,
                                 k.user_data + L"\\Products\\" + widen(pack_guid(inst.product_code)) + L"\\Usage",
                                 widen(feature));
    if (!v) return {0, 0};
    const u32* d = std::get_if<u32>(&v->data);
    if (d == nullptr) return {0, 0};
    return {*d & 0xFFFF, static_cast<u16>(*d >> 16)};
}

void ProductDatabase::set_source_list_value(const ProductInstance& inst, std::string_view sub, std::string_view name,
                                            std::string_view value, bool expand) {
    const auto k = keys(inst.context, inst.sid);
    std::wstring path = k.advertised_root + L"\\Products\\" + widen(pack_guid(inst.product_code)) + L"\\SourceList";
    if (!sub.empty()) path += L"\\" + widen(sub);
    Reg{m_registry}.set(k.advertised_hive, path, widen(name),
                        capsule::RegValue{expand ? capsule::RegType::ExpandSz : capsule::RegType::Sz, widen(value)});
}

void ProductDatabase::delete_source_list_value(const ProductInstance& inst, std::string_view sub, std::string_view name) {
    const auto k = keys(inst.context, inst.sid);
    std::wstring path = k.advertised_root + L"\\Products\\" + widen(pack_guid(inst.product_code)) + L"\\SourceList";
    if (!sub.empty()) path += L"\\" + widen(sub);
    Reg{m_registry}.del_value(k.advertised_hive, path, widen(name));
}

std::vector<std::pair<std::string, std::string>> ProductDatabase::source_list_values(const ProductInstance& inst,
                                                                                     std::string_view sub) const {
    Reg reg{m_registry};
    const auto k = keys(inst.context, inst.sid);
    std::wstring path = k.advertised_root + L"\\Products\\" + widen(pack_guid(inst.product_code)) + L"\\SourceList";
    if (!sub.empty()) path += L"\\" + widen(sub);
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& n : reg.values(k.advertised_hive, path))
        out.emplace_back(narrow(n), reg.text(k.advertised_hive, path, n).value_or(""));
    return out;
}

std::optional<ProductDatabase> product_database(MsiManager& manager) {
    if (manager.registry() == nullptr) return std::nullopt;
    return ProductDatabase(*manager.registry(), narrow(manager.user_sid()));
}

}  // namespace vitreo::winapi::msi
