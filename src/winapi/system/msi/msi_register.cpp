// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_register.cpp
/// @brief Windows Installer registration area: the product-registration
/// standard actions (ProcessComponents, RegisterUser, RegisterProduct,
/// PublishProduct / PublishFeatures / PublishComponents and their Unpublish
/// actions, FindRelatedProducts, RemoveExistingProducts), the installed-state
/// provider the features area consults, and the action registrar.
///
/// Contract: Learn ("Standard Actions Reference" pages of each action, the
/// Upgrade / PublishComponent / Feature / Component / Media tables, "Uninstall
/// Registry Key", "Version", ProductVersion, ALLUSERS) and the measured
/// layout of tools/oracle_probe/msi_registration_run.ps1 (the "Executing op"
/// lines and registry dumps in msi_registration_*.expected).

#include "winapi/system/msi/msi_core.hpp"
#include "winapi/system/msi/msi.hpp"
#include "winapi/system/msi/msi_assembly.hpp"
#include "winapi/system/msi/msi_database.hpp"
#include "winapi/system/msi/msi_engine.hpp"
#include "winapi/system/msi/msi_features.hpp"
#include "winapi/system/msi/msi_files.hpp"
#include "winapi/system/msi/msi_product.hpp"
#include "winapi/system/msi/msi_register.hpp"
#include "winapi/system/msi/msi_register_internal.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "capsule/registry/registry.hpp"
#include "capsule/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <random>

namespace vitreo::winapi::msi {

namespace reg_detail {

std::wstring widen(std::string_view s) {
    const std::vector<u16> u = utf8_to_utf16(s);
    return std::wstring(u.begin(), u.end());
}

std::string narrow(std::wstring_view w) {
    std::vector<u16> u;
    for (const wchar_t c : w) u.push_back(static_cast<u16>(c));
    return utf16_to_utf8(u.data(), static_cast<int>(u.size()));
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

ProductInstance session_instance(ActionContext& ctx) {
    Session& s = ctx.session();
    const std::string code = upper(s.property("ProductCode"));
    if (!s.property("ALLUSERS").empty()) return ProductInstance{code, kContextMachine, {}};
    std::string sid;
    if (MsiManager* m = ctx.services().manager) sid = narrow(m->user_sid());
    return ProductInstance{code, kContextUserUnmanaged, upper(sid)};
}

std::optional<ProductDatabase> action_database(ActionContext& ctx) {
    if (ctx.services().registry == nullptr) return std::nullopt;
    std::string sid;
    if (MsiManager* m = ctx.services().manager) sid = narrow(m->user_sid());
    return ProductDatabase(*ctx.services().registry, sid);
}

Rows::Rows(Session& session, const char* table) {
    if (Package* p = session.package()) m_table = p->table(table);
}

usize Rows::size() const { return m_table ? m_table->rows.size() : 0; }

std::string Rows::get(usize row, std::string_view column) const {
    if (!m_table || row >= m_table->rows.size()) return {};
    const auto c = m_table->column_index(column);
    if (!c || *c >= m_table->rows[row].size()) return {};
    return m_table->rows[row][*c];
}

i32 component_final_state(Session& session, const std::string& component) {
    const ComponentInfo* c = find_component(session, component);
    if (c == nullptr) return install_state::kUnknown;
    return c->action != install_state::kUnknown ? c->action : c->installed;
}

i32 feature_final_state(Session& session, const std::string& feature) {
    const FeatureInfo* f = find_feature(session, feature);
    if (f == nullptr) return install_state::kUnknown;
    return f->action != install_state::kUnknown ? f->action : f->installed;
}

bool component_installing(Session& session, const std::string& component) {
    const ComponentInfo* c = find_component(session, component);
    return c != nullptr && (c->action == install_state::kLocal || c->action == install_state::kSource);
}

bool component_removing(Session& session, const std::string& component) {
    const ComponentInfo* c = find_component(session, component);
    return c != nullptr && (c->action == install_state::kAbsent || c->action == install_state::kRemoved) &&
           (c->installed == install_state::kLocal || c->installed == install_state::kSource);
}

bool removing_product(Session& session) {
    if (upper(session.property("REMOVE")) == "ALL") return true;
    FeatureModel& m = feature_model(session);
    if (m.features.empty()) return false;
    for (const auto& [name, f] : m.features) {
        const i32 s = f.action != install_state::kUnknown ? f.action : f.installed;
        if (s == install_state::kLocal || s == install_state::kSource || s == install_state::kAdvertised) return false;
    }
    // Every feature ends absent: removal only if the product was installed.
    return !session.property("Installed").empty();
}

bool package_is_64bit(Session& session) {
    Package* p = session.package();
    if (p == nullptr) return false;
    const std::vector<u8>* stream = p->stream("\005SummaryInformation");
    if (stream == nullptr) return false;
    const auto props = parse_summary_information(*stream);
    const auto it = props.find(7);  // PID_TEMPLATE "platform;languages"
    if (it == props.end()) return false;
    const std::string platform = upper(it->second.text.substr(0, it->second.text.find(';')));
    return platform == "X64" || platform == "INTEL64" || platform == "ARM64" || platform == "AMD64";
}

std::string session_descriptor(Session& session, const std::string& feature, const std::string& component) {
    const FeatureInfo* f = find_feature(session, feature);
    const ComponentInfo* c = find_component(session, component);
    const bool single = f != nullptr && f->components.size() == 1;
    return compose_descriptor(session.property("ProductCode"), feature,
                              single || c == nullptr ? std::string_view{} : std::string_view(c->component_id));
}

std::string component_key_file(Session& session, const std::string& component) {
    const ComponentInfo* c = find_component(session, component);
    if (c == nullptr || c->key_path.empty() || c->registry_key_path() || c->odbc_key_path()) return {};
    return file_target_path(session, c->key_path);
}

bool require_script(ActionContext& ctx) { return ctx.scripting(); }

void schedule(ActionContext& ctx, const std::string& action, std::function<void()> run, std::function<void()> rollback) {
    ScriptOp op;
    op.action = action;
    op.run = [run = std::move(run)]() -> u32 {
        run();
        return kActionSuccess;
    };
    op.rollback = std::move(rollback);
    ctx.schedule(std::move(op));
}

}  // namespace reg_detail

using namespace reg_detail;

namespace {

constexpr u32 kUpgradeMigrateFeatures = 1, kUpgradeOnlyDetect = 2, kUpgradeIgnoreRemoveFailure = 4,
              kUpgradeVersionMinInclusive = 256, kUpgradeVersionMaxInclusive = 512,
              kUpgradeLanguagesExclusive = 1024;

/// ProductVersion "major.minor.build[.x]" (Learn "ProductVersion": the
/// fourth field is ignored): the parts, missing ones 0.
std::array<u32, 3> version_parts(const std::string& text) {
    std::array<u32, 3> v{0, 0, 0};
    usize i = 0;
    for (usize part = 0; part < 3 && i <= text.size(); ++part) {
        const usize dot = text.find('.', i);
        v[part] = static_cast<u32>(std::strtoul(text.substr(i, dot - i).c_str(), nullptr, 10));
        if (dot == std::string::npos) break;
        i = dot + 1;
    }
    return v;
}

/// The Version DWORD (measured 1.2.3 -> 16908291 = 0x01020003).
u32 version_dword(const std::string& text) {
    const auto v = version_parts(text);
    return (v[0] & 0xFF) << 24 | (v[1] & 0xFF) << 16 | (v[2] & 0xFFFF);
}

/// Compare two ProductVersion strings field by field (three fields).
int compare_versions(const std::string& a, const std::string& b) {
    const auto x = version_parts(a), y = version_parts(b);
    for (usize i = 0; i < 3; ++i)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

std::string today_yyyymmdd() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char b[16];
    std::snprintf(b, sizeof b, "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return b;
}

std::string with_slash(std::string s) {
    if (!s.empty() && s.back() != '\\') s.push_back('\\');
    return s;
}

std::optional<std::vector<u8>> read_file(capsule::Vfs& vfs, const std::string& path) {
    auto h = vfs.open(path, capsule::AccessMode::Read, capsule::ShareMode::Read, capsule::CreationDisposition::OpenExisting);
    if (h.is_err()) return std::nullopt;
    std::vector<u8> out;
    std::array<u8, 65536> chunk{};
    for (;;) {
        auto n = vfs.read(h.value(), chunk);
        if (n.is_err() || n.value() == 0) break;
        out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n.value()));
    }
    (void)vfs.close(h.value());
    return out;
}

bool write_file(capsule::Vfs& vfs, const std::string& path, std::span<const u8> bytes) {
    for (usize i = 3; i < path.size(); ++i)
        if (path[i] == '\\') (void)vfs.create_dir(path.substr(0, i));
    auto h = vfs.open(path, capsule::AccessMode::Write, capsule::ShareMode::None, capsule::CreationDisposition::CreateAlways);
    if (h.is_err()) return false;
    bool ok = true;
    usize done = 0;
    while (done < bytes.size()) {
        auto n = vfs.write(h.value(), bytes.subspan(done));
        if (n.is_err() || n.value() == 0) { ok = false; break; }
        done += n.value();
    }
    (void)vfs.close(h.value());
    return ok;
}

/// %WINDIR%\Installer, where Windows caches packages and product icons.
std::string installer_cache_dir(Session& s) {
    std::string w = s.property("WindowsFolder");
    if (w.empty()) w = "C:\\Windows\\";
    return with_slash(w) + "Installer\\";
}

/// A fresh cache file name: eight lower-case hex digits + ".msi" (measured
/// "17b3e878.msi"; the digits are not otherwise meaningful).
std::string new_cache_name(capsule::Vfs& vfs, const std::string& dir) {
    std::random_device rd;
    for (;;) {
        char b[16];
        std::snprintf(b, sizeof b, "%08x.msi", static_cast<unsigned>(rd()));
        if (vfs.stat(dir + b).is_err()) return dir + b;
    }
}

/// Cache the Icon-table icons a product publishes (ARPPRODUCTICON, shortcut
/// and class icons) under %WINDIR%\Installer\<ProductCode>\. Returns the path
/// of @p name, or "".
std::string cached_icon_path(Session& s, const std::string& name) {
    if (name.empty()) return {};
    return installer_cache_dir(s) + s.property("ProductCode") + "\\" + name;
}

// ---- ProcessComponents --------------------------------------------------------------

/// The key path ProcessComponents registers (probe ComponentRegister ops):
/// the key file's full path; for a registry key path "RR:\key\name" (or
/// "RR:\key\" for a key-only row) with RR the root (00 HKCR, 01 HKCU, 02 HKLM,
/// 03 HKU; -1 resolves to 02 per-machine / 01 per-user) plus 20 for a 64-bit
/// component (measured 02 / 01 / 22); the component's directory with a
/// trailing '\' when KeyPath is null.
std::optional<std::string> component_key_path(Session& s, const ComponentInfo& c, std::string& why) {
    // A global assembly's key path names the assembly, not a file in the
    // component's directory (probe msi_assembly ComponentRegister ops:
    // "<\Name,Version=...", "<Key.config\policy...", ">Key.dll\Win32...").
    if (!c.key_path.empty() && !c.registry_key_path() && !c.odbc_key_path())
        if (const auto a = package_assembly(s, c.name); a && a->global()) return assembly_key_path(s, *a, c.key_path);
    if (c.key_path.empty()) return with_slash(component_directory(s, c.name));
    if (c.odbc_key_path()) {
        why = "ODBC data source key paths";
        return std::nullopt;
    }
    if (c.registry_key_path()) {
        Rows reg(s, "Registry");
        for (usize i = 0; i < reg.size(); ++i) {
            if (reg.get(i, "Registry") != c.key_path) continue;
            i32 root = std::atoi(reg.get(i, "Root").c_str());
            if (root == -1) root = s.property("ALLUSERS").empty() ? 1 : 2;
            if (root < 0 || root > 3) break;
            if (c.is_64bit()) root += 20;
            char rr[8];
            std::snprintf(rr, sizeof rr, "%02d:", root);
            const std::string key = s.format(reg.get(i, "Key"));
            const std::string name = s.format(reg.get(i, "Name"));
            return std::string(rr) + "\\" + key + "\\" + name;
        }
        why = "registry key path row not found";
        return std::nullopt;
    }
    const std::string file = file_target_path(s, c.key_path);
    if (file.empty()) {
        why = "key file path unresolved";
        return std::nullopt;
    }
    return file;
}

u32 action_process_components(ActionContext& ctx) {
    if (!require_script(ctx)) return kActionFailure;
    Session& s = ctx.session();
    auto db = action_database(ctx);
    if (!db) {
        log_unimplemented("ProcessComponents", "no capsule registry to register components in");
        return kActionFailure;
    }
    const ProductInstance inst = session_instance(ctx);
    FeatureModel& model = feature_model(s);
    capsule::Registry* registry = ctx.services().registry;
    for (const std::string& name : model.component_order) {
        const ComponentInfo& c = model.components.at(name);
        if (c.component_id.empty()) continue;  // Learn: unregistered component
        const std::string code = upper(c.component_id);
        if (c.action == install_state::kLocal) {
            std::string why;
            auto kp = component_key_path(s, c, why);
            if (!kp) {
                log_unimplemented("ProcessComponents", why);
                return kActionFailure;
            }
            const bool shared_dll = (c.attributes & kComponentSharedDllRefCount) != 0 && !c.registry_key_path() &&
                                    !c.key_path.empty();
            const capsule::RegView view = c.is_64bit() ? capsule::RegView::Native64 : capsule::RegView::Wow32;
            schedule(ctx, "ProcessComponents",
                     [=]() {
                         ProductDatabase d(*registry, db->user_sid());
                         d.register_component(inst, code, *kp);
                         if (shared_dll) {
                             // SharedDLLs\<key file> counts the products sharing it.
                             // A component's registry data is in its own
                             // bitness's view (Learn, "Windows Installer on
                             // 64-bit Operating Systems").
                             auto k = registry->open_key(registry->view_root(capsule::HKEY_LOCAL_MACHINE, view),
                                                         L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\SharedDLLs", true);
                             if (k.is_ok()) {
                                 u32 n = 0;
                                 if (auto v = registry->query_value(k.value(), widen(*kp)); v.is_ok())
                                     if (auto* d32 = std::get_if<u32>(&v.value().data)) n = *d32;
                                 (void)registry->set_value(k.value(), widen(*kp),
                                                           capsule::RegValue{capsule::RegType::Dword, n + 1});
                                 registry->close_key(k.value());
                             }
                         }
                     },
                     [=]() { ProductDatabase(*registry, db->user_sid()).unregister_component(inst, code); });
        } else if (c.action == install_state::kSource) {
            // A run-from-source component registers a source-relative key path
            // whose format was not measured.
            log_unimplemented("ProcessComponents", "run-from-source component registration");
            return kActionFailure;
        } else if (c.action == install_state::kAbsent || c.action == install_state::kRemoved) {
            const std::optional<std::string> old = db->component_key_path(inst, code);
            if (!old) continue;
            schedule(ctx, "ProcessComponents",
                     [=]() { ProductDatabase(*registry, db->user_sid()).unregister_component(inst, code); },
                     [=]() { ProductDatabase(*registry, db->user_sid()).register_component(inst, code, *old); });
        }
    }
    return kActionSuccess;
}

// ---- RegisterUser / RegisterProduct ---------------------------------------------------

u32 action_register_user(ActionContext& ctx) {
    if (!require_script(ctx)) return kActionFailure;
    Session& s = ctx.session();
    // UserRegister op only when there is a ProductID to register (probe:
    // nothing is written without ProductID even with USERNAME/COMPANYNAME).
    const std::string pid = s.property("ProductID");
    if (pid.empty() || removing_product(s)) return kActionSuccess;
    auto db = action_database(ctx);
    if (!db) return kActionFailure;
    const ProductInstance inst = session_instance(ctx);
    capsule::Registry* registry = ctx.services().registry;
    const std::string owner = s.property("USERNAME"), company = s.property("COMPANYNAME"), sid = db->user_sid();
    schedule(ctx, "RegisterUser", [=]() { ProductDatabase(*registry, sid).register_user(inst, owner, company, pid); });
    return kActionSuccess;
}

u32 action_register_product(ActionContext& ctx) {
    if (!require_script(ctx)) return kActionFailure;
    Session& s = ctx.session();
    auto db = action_database(ctx);
    capsule::Vfs* vfs = ctx.services().vfs;
    if (!db || vfs == nullptr) {
        log_unimplemented("RegisterProduct", "no capsule registry/VFS");
        return kActionFailure;
    }
    const ProductInstance inst = session_instance(ctx);
    capsule::Registry* registry = ctx.services().registry;
    const std::string sid = db->user_sid();
    const bool x64 = package_is_64bit(s);
    const std::string upgrade = s.property("UpgradeCode");
    if (inst.product_code.empty() || pack_guid(inst.product_code).empty()) {
        log_unimplemented("RegisterProduct", "package without a valid ProductCode");
        return kActionFailure;
    }
    if (removing_product(s)) {
        // ProductUnregister: InstallProperties, Features/Patches/Usage, the
        // ARP entry and the machine-wide upgrade-code mapping; the cached
        // package goes with it.
        const std::string cached = db->local_package(inst);
        schedule(ctx, "RegisterProduct", [=]() {
            ProductDatabase(*registry, sid).unregister_product(inst, upgrade, x64);
            if (!cached.empty()) (void)vfs->remove(cached);
        });
        return kActionSuccess;
    }
    // DatabaseCopy: cache the package under %WINDIR%\Installer.
    std::string source = s.property("OriginalDatabase");
    if (source.empty()) source = s.property("DATABASE");
    auto bytes = read_file(*vfs, source);
    if (!bytes) {
        log_unimplemented("RegisterProduct", "original package unreadable for caching");
        return kActionFailure;
    }
    const std::string existing = db->local_package(inst);
    const std::string cache = existing.empty() ? new_cache_name(*vfs, installer_cache_dir(s)) : existing;

    // ProductRegister (measured value set, types, and that every ARP string
    // is written, "" when its property is unset; HelpLink / Readme become
    // REG_EXPAND_SZ and Size a REG_DWORD only when set).
    std::map<std::string, std::string> strings;
    std::map<std::string, std::string> expand;
    std::map<std::string, u32> dwords;
    const std::string version = s.property("ProductVersion");
    const auto parts = version_parts(version);
    auto prop_or = [&](const char* p) { return s.property(p); };
    strings["AuthorizedCDFPrefix"] = prop_or("ARPAUTHORIZEDCDFPREFIX");
    strings["Comments"] = prop_or("ARPCOMMENTS");
    strings["Contact"] = prop_or("ARPCONTACT");
    strings["DisplayName"] = prop_or("ProductName");
    strings["DisplayVersion"] = version;
    strings["HelpTelephone"] = prop_or("ARPHELPTELEPHONE");
    strings["InstallDate"] = today_yyyymmdd();
    strings["InstallLocation"] = prop_or("ARPINSTALLLOCATION");
    strings["InstallSource"] = with_slash(prop_or("SourceDir"));
    strings["LocalPackage"] = cache;
    strings["Publisher"] = prop_or("Manufacturer");
    strings["URLInfoAbout"] = prop_or("ARPURLINFOABOUT");
    strings["URLUpdateInfo"] = prop_or("ARPURLUPDATEINFO");
    for (const auto& [name, prop] : {std::pair{"HelpLink", "ARPHELPLINK"}, std::pair{"Readme", "ARPREADME"}}) {
        const std::string v = s.property(prop);
        if (v.empty()) strings[name] = "";
        else expand[name] = v;
    }
    if (const std::string sz = s.property("ARPSIZE"); sz.empty()) strings["Size"] = "";
    else dwords["Size"] = static_cast<u32>(std::strtoul(sz.c_str(), nullptr, 10));
    // Learn "Uninstall Registry Key": the ARPNO* / ARPSYSTEMCOMPONENT flags.
    for (const auto& [name, prop] : {std::pair{"NoModify", "ARPNOMODIFY"}, std::pair{"NoRemove", "ARPNOREMOVE"},
                                     std::pair{"NoRepair", "ARPNOREPAIR"},
                                     std::pair{"SystemComponent", "ARPSYSTEMCOMPONENT"}})
        if (!s.property(prop).empty()) dwords[name] = 1;
    if (const std::string icon = s.property("ARPPRODUCTICON"); !icon.empty())
        strings["DisplayIcon"] = cached_icon_path(s, icon);
    const std::string msiexec = "MsiExec.exe /I" + inst.product_code;
    expand["ModifyPath"] = msiexec;
    expand["UninstallString"] = msiexec;
    dwords["Language"] = static_cast<u32>(std::strtoul(s.property("ProductLanguage").c_str(), nullptr, 10));
    dwords["Version"] = version_dword(version);
    dwords["VersionMajor"] = parts[0];
    dwords["VersionMinor"] = parts[1];
    dwords["WindowsInstaller"] = 1;
    // EstimatedSize, in KB, from the components' local costs (FileCost);
    // see registration.md for the measured offset this does not reproduce.
    i64 bytes_local = 0;
    for (const auto& [name, c] : feature_model(s).components)
        if (component_final_state(s, name) == install_state::kLocal) bytes_local += c.local_cost;
    dwords["EstimatedSize"] = static_cast<u32>((bytes_local + 1023) / 1024);

    std::vector<std::pair<std::string, std::vector<u8>>> icons;
    {
        Rows icon_rows(s, "Icon");
        for (usize i = 0; i < icon_rows.size(); ++i) {
            const std::string name = icon_rows.get(i, "Name");
            if (const std::vector<u8>* data = s.package()->stream("Icon." + name))
                icons.emplace_back(cached_icon_path(s, name), *data);
        }
    }
    schedule(ctx, "RegisterProduct",
             [=, bytes = std::move(*bytes)]() {
                 (void)write_file(*vfs, cache, bytes);
                 for (const auto& [path, data] : icons) (void)write_file(*vfs, path, data);
                 ProductDatabase(*registry, sid).register_product(inst, strings, dwords, expand, x64);
             },
             [=]() {
                 ProductDatabase(*registry, sid).unregister_product(inst, {}, x64);
                 if (existing.empty()) (void)vfs->remove(cache);
             });
    return kActionSuccess;
}

// ---- PublishProduct / Features / Components -------------------------------------------

u32 action_publish_product(ActionContext& ctx) {
    if (!require_script(ctx)) return kActionFailure;
    Session& s = ctx.session();
    auto db = action_database(ctx);
    if (!db) return kActionFailure;
    const ProductInstance inst = session_instance(ctx);
    capsule::Registry* registry = ctx.services().registry;
    const std::string sid = db->user_sid();
    const std::string upgrade = s.property("UpgradeCode");
    if (removing_product(s)) {
        schedule(ctx, "PublishProduct", [=]() { ProductDatabase(*registry, sid).unpublish_product(inst, upgrade); });
        return kActionSuccess;
    }
    ProductDatabase::Publication pub;
    pub.product_name = s.property("ProductName");
    pub.package_code = s.property("PackageCode");
    if (pub.package_code.empty())
        if (const std::vector<u8>* si = s.package()->stream("\005SummaryInformation")) {
            const auto props = parse_summary_information(*si);
            if (auto it = props.find(9); it != props.end()) pub.package_code = it->second.text;  // PID_REVNUMBER
        }
    pub.language = static_cast<u32>(std::strtoul(s.property("ProductLanguage").c_str(), nullptr, 10));
    pub.version = version_dword(s.property("ProductVersion"));
    pub.assignment = inst.context == kContextMachine ? 1 : 0;
    pub.upgrade_code = upgrade;
    std::string original = s.property("OriginalDatabase");
    pub.package_name = original.substr(original.find_last_of('\\') + 1);
    pub.last_used_source = with_slash(s.property("SourceDir"));
    pub.net_sources = {pub.last_used_source};
    Rows media(s, "Media");
    for (usize i = 0; i < media.size(); ++i)
        pub.media.emplace_back(static_cast<u32>(std::strtoul(media.get(i, "DiskId").c_str(), nullptr, 10)),
                               media.get(i, "VolumeLabel") + ";" + s.format(media.get(i, "DiskPrompt")));
    if (const std::string icon = s.property("ARPPRODUCTICON"); !icon.empty()) pub.product_icon = cached_icon_path(s, icon);
    schedule(ctx, "PublishProduct", [=]() { ProductDatabase(*registry, sid).publish_product(inst, pub); },
             [=]() { ProductDatabase(*registry, sid).unpublish_product(inst, upgrade); });
    return kActionSuccess;
}

/// FeaturePublish / FeatureUnpublish for every feature (measured: every
/// feature is published, absent ones with the "\x06" flag, each listing all
/// of its registered components).
u32 publish_features(ActionContext& ctx, bool publish) {
    if (!require_script(ctx)) return kActionFailure;
    Session& s = ctx.session();
    auto db = action_database(ctx);
    if (!db) return kActionFailure;
    const bool removing = removing_product(s);
    if (publish == removing) return kActionSuccess;  // Publish on install, Unpublish on removal
    const ProductInstance inst = session_instance(ctx);
    capsule::Registry* registry = ctx.services().registry;
    const std::string sid = db->user_sid();
    FeatureModel& model = feature_model(s);
    for (const std::string& name : model.feature_order) {
        const FeatureInfo& f = model.features.at(name);
        std::vector<std::string> comps;
        for (const std::string& c : f.components)
            if (const ComponentInfo* ci = find_component(s, c); ci && !ci->component_id.empty())
                comps.push_back(upper(ci->component_id));
        const i32 st = feature_final_state(s, name);
        const bool absent = st != install_state::kLocal && st != install_state::kSource && st != install_state::kAdvertised;
        const std::string parent = f.parent;
        if (publish)
            schedule(ctx, "PublishFeatures",
                     [=]() { ProductDatabase(*registry, sid).publish_feature(inst, name, parent, absent, comps); },
                     [=]() { ProductDatabase(*registry, sid).unpublish_feature(inst, name); });
        else
            schedule(ctx, "UnpublishFeatures", [=]() { ProductDatabase(*registry, sid).unpublish_feature(inst, name); },
                     [=]() { ProductDatabase(*registry, sid).publish_feature(inst, name, parent, absent, comps); });
    }
    return kActionSuccess;
}

/// PublishComponents / UnpublishComponents (Learn: the PublishComponent
/// table; probe ComponentPublish: the qualifier maps to the descriptor of
/// (Feature_, Component_) followed by AppData).
u32 publish_components(ActionContext& ctx, bool publish) {
    if (!require_script(ctx)) return kActionFailure;
    Session& s = ctx.session();
    auto db = action_database(ctx);
    if (!db) return kActionFailure;
    const ProductInstance inst = session_instance(ctx);
    capsule::Registry* registry = ctx.services().registry;
    const std::string sid = db->user_sid();
    Rows rows(s, "PublishComponent");
    for (usize i = 0; i < rows.size(); ++i) {
        const std::string category = upper(rows.get(i, "ComponentId")), qualifier = rows.get(i, "Qualifier");
        const std::string component = rows.get(i, "Component_"), feature = rows.get(i, "Feature_");
        const std::string app = rows.get(i, "AppData");
        const i32 fs = feature_final_state(s, feature);
        const bool present = fs == install_state::kLocal || fs == install_state::kSource || fs == install_state::kAdvertised;
        const std::string desc = session_descriptor(s, feature, component);
        if (desc.empty()) continue;
        if (publish && present)
            schedule(ctx, "PublishComponents",
                     [=]() { ProductDatabase(*registry, sid).publish_qualified_component(inst, category, qualifier, desc, app); },
                     [=]() { ProductDatabase(*registry, sid).unpublish_qualified_component(inst, category, qualifier, desc); });
        else if (!publish && !present)
            schedule(ctx, "UnpublishComponents",
                     [=]() { ProductDatabase(*registry, sid).unpublish_qualified_component(inst, category, qualifier, desc); });
    }
    return kActionSuccess;
}

// ---- FindRelatedProducts / RemoveExistingProducts --------------------------------------

/// Is the product a maintenance install of itself (Installed set)? Both
/// upgrade actions are skipped then (probe: "not run in maintenance mode").
bool maintenance(Session& s) { return !s.property("Installed").empty(); }

bool language_matches(const std::string& list, const std::string& lang, bool exclusive) {
    if (list.empty()) return true;
    bool found = false;
    usize i = 0;
    while (i <= list.size()) {
        const usize comma = list.find(',', i);
        if (std::strtoul(list.substr(i, comma - i).c_str(), nullptr, 10) == std::strtoul(lang.c_str(), nullptr, 10))
            found = true;
        if (comma == std::string::npos) break;
        i = comma + 1;
    }
    return exclusive ? !found : found;
}

u32 action_find_related_products(ActionContext& ctx) {
    Session& s = ctx.session();
    if (maintenance(s)) return kActionSuccess;
    auto db = action_database(ctx);
    if (!db) return kActionSuccess;  // nothing installed anywhere
    const ProductInstance self = session_instance(ctx);
    Rows rows(s, "Upgrade");
    for (usize i = 0; i < rows.size(); ++i) {
        const std::string property = rows.get(i, "ActionProperty");
        if (property.empty()) continue;
        const u32 attrs = static_cast<u32>(std::strtoul(rows.get(i, "Attributes").c_str(), nullptr, 10));
        const std::string vmin = rows.get(i, "VersionMin"), vmax = rows.get(i, "VersionMax");
        for (const std::string& code : db->related_products(rows.get(i, "UpgradeCode"))) {
            if (upper(code) == self.product_code) continue;
            // Only products in the install's own context are related
            // (Learn "FindRelatedProducts": per-machine finds per-machine).
            const auto instances = db->instances(self.context, {}, code);
            if (instances.empty()) continue;
            const ProductInstance& other = instances.front();
            const std::string version = db->install_property(other, "DisplayVersion").value_or("");
            if (!vmin.empty()) {
                const int c = compare_versions(version, vmin);
                if (c < 0 || (c == 0 && !(attrs & kUpgradeVersionMinInclusive))) continue;
            }
            if (!vmax.empty()) {
                const int c = compare_versions(version, vmax);
                if (c > 0 || (c == 0 && !(attrs & kUpgradeVersionMaxInclusive))) continue;
            }
            const std::string lang = db->product_key_value(other, "Language").value_or("");
            if (!language_matches(rows.get(i, "Language"), lang, (attrs & kUpgradeLanguagesExclusive) != 0)) continue;
            std::string value = s.property(property);
            if (value.find(code) != std::string::npos) continue;
            s.set_property(property, value.empty() ? code : value + ";" + code);
        }
    }
    return kActionSuccess;
}

/// Run the uninstall of an installed product from its cached package
/// (probe: RemoveExistingProducts runs a nested engine on the LocalPackage
/// with REMOVE and UPGRADINGPRODUCTCODE).
u32 remove_product(ActionContext& ctx, ProductDatabase& db, const std::string& code, const std::string& remove) {
    MsiManager* mgr = ctx.services().manager;
    capsule::Vfs* vfs = ctx.services().vfs;
    const auto inst = db.installed(code);
    if (!inst) return kActionSuccess;  // already gone
    const std::string local = db.local_package(*inst);
    auto bytes = (mgr && vfs && !local.empty()) ? read_file(*vfs, local) : std::nullopt;
    if (!bytes) return kActionFailure;
    auto package = Package::open(std::span<const u8>(bytes->data(), bytes->size()));
    if (!package) return kActionFailure;
    Engine::Options options;
    options.package_path = local;
    options.ui_level = 2;  // INSTALLUILEVEL_NONE
    options.server = true;
    options.allow_system_changes = true;
    options.command_line["REMOVE"] = remove;
    options.command_line["UPGRADINGPRODUCTCODE"] = ctx.session().property("ProductCode");
    if (inst->context == kContextMachine) options.command_line["ALLUSERS"] = "1";
    u32 error = 0;
    auto engine = Engine::open(*mgr, std::make_shared<Package>(std::move(*package)), options, ctx.services(), error);
    if (!engine) return error ? error : kActionFailure;
    return engine->run_top_level("INSTALL");
}

u32 action_remove_existing_products(ActionContext& ctx) {
    Session& s = ctx.session();
    if (maintenance(s)) return kActionSuccess;
    auto db = action_database(ctx);
    if (!db) return kActionSuccess;
    Rows rows(s, "Upgrade");
    for (usize i = 0; i < rows.size(); ++i) {
        const u32 attrs = static_cast<u32>(std::strtoul(rows.get(i, "Attributes").c_str(), nullptr, 10));
        if (attrs & kUpgradeOnlyDetect) continue;
        const std::string list = s.property(rows.get(i, "ActionProperty"));
        std::string remove = s.format(rows.get(i, "Remove"));
        if (remove.empty()) remove = "ALL";
        usize p = 0;
        while (p < list.size()) {
            const usize semi = list.find(';', p);
            const std::string code = list.substr(p, semi - p);
            p = semi == std::string::npos ? list.size() : semi + 1;
            if (!is_guid(code)) continue;
            const u32 rc = remove_product(ctx, *db, code, remove);
            if (rc != kActionSuccess && !(attrs & kUpgradeIgnoreRemoveFailure)) return kActionFailure;
        }
    }
    (void)kUpgradeMigrateFeatures;
    return kActionSuccess;
}

}  // namespace

void install_registration_state_provider() {
    InstalledStateProvider p;
    // Answers from the registry of the engine running the session (never a
    // process-wide one: each MsiManager has its own capsule registry).
    auto database = [](Session& s) -> std::optional<ProductDatabase> {
        Engine* e = s.engine();
        if (e == nullptr || e->services().registry == nullptr) return std::nullopt;
        return ProductDatabase(*e->services().registry, narrow(e->manager().user_sid()));
    };
    p.feature = [database](Session& s, const std::string& product, const std::string& feature) -> i32 {
        auto d = database(s);
        auto inst = d ? d->installed(product) : std::nullopt;
        if (!inst) return install_state::kAbsent;
        const i32 st = d->feature_state(*inst, feature);
        return st == install_state::kUnknown ? install_state::kAbsent : st;
    };
    p.component = [database](Session& s, const std::string& product, const std::string& component) -> i32 {
        auto d = database(s);
        auto inst = d ? d->installed(product) : std::nullopt;
        if (!inst) return install_state::kAbsent;
        const i32 st = d->component_state(*inst, component);
        return st == install_state::kUnknown ? install_state::kAbsent : st;
    };
    p.product_features = [database](Session& s, const std::string& product) -> std::map<std::string, i32> {
        auto d = database(s);
        auto inst = d ? d->installed(product) : std::nullopt;
        return inst ? d->feature_states(*inst) : std::map<std::string, i32>{};
    };
    p.related_products = [database](Session& s, const std::string& upgrade) -> std::vector<std::string> {
        auto d = database(s);
        return d ? d->related_products(upgrade) : std::vector<std::string>{};
    };
    set_installed_state_provider(std::move(p));
}


void register_registration_actions(ActionRegistry& r) {
    install_registration_state_provider();
    r.add("ProcessComponents", action_process_components);
    r.add("RegisterUser", action_register_user);
    r.add("RegisterProduct", action_register_product);
    r.add("PublishProduct", action_publish_product);
    r.add("PublishFeatures", [](ActionContext& c) { return publish_features(c, true); });
    r.add("UnpublishFeatures", [](ActionContext& c) { return publish_features(c, false); });
    r.add("PublishComponents", [](ActionContext& c) { return publish_components(c, true); });
    r.add("UnpublishComponents", [](ActionContext& c) { return publish_components(c, false); });
    r.add("FindRelatedProducts", action_find_related_products);
    r.add("RemoveExistingProducts", action_remove_existing_products);
    register_com_and_shell_actions(r);
    register_assembly_actions(r);  // MsiPublishAssemblies / MsiUnpublishAssemblies (msi_assembly.cpp)
}

}  // namespace vitreo::winapi::msi
