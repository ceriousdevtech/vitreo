// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_register_com.cpp
/// @brief Registration-area actions for COM classes, ProgIds, extensions,
/// verbs, MIME types, AppIDs, type libraries, fonts, self-registering
/// modules, shortcuts, COM+ applications and assemblies.
///
/// Contract: Learn (the Class, ProgId, Extension, Verb, MIME, AppId, TypeLib,
/// Font, SelfReg, Shortcut, MsiShortcutProperty, Complus, MsiAssembly tables
/// and the Register*/Unregister*, CreateShortcuts, SelfRegModules actions),
/// [MS-SHLLINK] and [MS-PROPSTORE] for the .lnk files, the OpenType `name`
/// table for font titles, and the measured registry layout of
/// tools/oracle_probe/msi_registration_run.ps1 (msi_registration_machine_x86
/// / _user_x86 / _machine_x64 / _ext2 .expected).

#include "winapi/system/msi/msi_core.hpp"
#include "winapi/system/msi/msi.hpp"
#include "winapi/system/msi/msi_engine.hpp"
#include "winapi/system/msi/msi_features.hpp"
#include "winapi/system/msi/msi_files.hpp"
#include "winapi/system/msi/msi_product.hpp"
#include "winapi/system/msi/msi_register_internal.hpp"
#include "winapi/com/propsys/propsys.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "capsule/registry/registry.hpp"
#include "capsule/vfs/vfs.hpp"

#include <algorithm>
#include <memory>
#include <array>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

namespace vitreo::winapi::msi {

using namespace reg_detail;

namespace {

// ---- registry helpers -------------------------------------------------------------------

/// Writes in the 64-bit view: the keys below are 64-bit-layout paths (a
/// 32-bit component's CLSID names Classes\WOW6432Node itself, clsid_key).
struct Writer {
    capsule::Registry& r;
    u64 hive;

    void set(const std::wstring& key, std::wstring_view name, const capsule::RegValue& v) const {
        auto k = r.open_key(r.view_root(hive, capsule::RegView::Native64), key, true);
        if (k.is_err()) return;
        (void)r.set_value(k.value(), name, v);
        r.close_key(k.value());
    }
    void sz(const std::wstring& key, std::wstring_view name, std::string_view value) const {
        set(key, name, capsule::RegValue{capsule::RegType::Sz, widen(value)});
    }
    void multi(const std::wstring& key, std::wstring_view name, std::string_view value) const {
        set(key, name, capsule::RegValue{capsule::RegType::MultiSz, std::vector<std::wstring>{widen(value)}});
    }
    void touch(const std::wstring& key) const {
        auto k = r.open_key(r.view_root(hive, capsule::RegView::Native64), key, true);
        if (k.is_ok()) r.close_key(k.value());
    }
    void del_tree(const std::wstring& key) const {
        auto k = r.open_key(r.view_root(hive, capsule::RegView::Native64), key, false);
        if (k.is_err()) return;
        auto children = r.enum_keys(k.value());
        r.close_key(k.value());
        if (children.is_ok())
            for (const auto& c : children.value()) del_tree(key + L"\\" + c);
        const usize slash = key.rfind(L'\\');
        if (slash == std::wstring::npos) return;
        auto parent = r.open_key(r.view_root(hive, capsule::RegView::Native64), key.substr(0, slash), false);
        if (parent.is_err()) return;
        (void)r.delete_key(parent.value(), key.substr(slash + 1));
        r.close_key(parent.value());
    }
};

/// The Classes root an install writes to: HKLM\SOFTWARE\Classes per-machine,
/// HKCU\Software\Classes per-user (measured).
std::pair<u64, std::wstring> classes_root(Session& s) {
    if (!s.property("ALLUSERS").empty()) return {capsule::HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes"};
    return {capsule::HKEY_CURRENT_USER, L"Software\\Classes"};
}

/// CLSID keys of a 32-bit component live in the WOW64 view
/// (Classes\WOW6432Node\CLSID, measured); ProgIds, extensions, MIME and
/// FileType keys are shared; AppID is written to both views (measured).
std::wstring clsid_key(Session& s, const std::string& component, const std::string& clsid) {
    const ComponentInfo* c = find_component(s, component);
    const bool x64 = c != nullptr && c->is_64bit();
    return classes_root(s).second + (x64 ? L"" : L"\\WOW6432Node") + L"\\CLSID\\" + widen(upper(clsid));
}

std::string icon_path(Session& s, const std::string& icon, const std::string& index) {
    std::string w = s.property("WindowsFolder");
    if (w.empty()) w = "C:\\Windows\\";
    if (w.back() != '\\') w.push_back('\\');
    return w + "Installer\\" + s.property("ProductCode") + "\\" + icon + "," + (index.empty() ? "0" : index);
}

std::string file_name_of(const std::string& path) { return path.substr(path.find_last_of('\\') + 1); }

bool is_local_server(const std::string& context) {
    return context == "LocalServer" || context == "LocalServer32";
}

// ---- classes / ProgIds / extensions / MIME -------------------------------------------------

/// The ProgId rows by name.
std::map<std::string, usize> progid_index(Rows& rows) {
    std::map<std::string, usize> out;
    for (usize i = 0; i < rows.size(); ++i) out[rows.get(i, "ProgId")] = i;
    return out;
}

/// The version-independent ProgId of @p progid (the row naming it as parent).
std::string vi_progid(Rows& rows, const std::string& progid) {
    for (usize i = 0; i < rows.size(); ++i)
        if (!progid.empty() && rows.get(i, "ProgId_Parent") == progid) return rows.get(i, "ProgId");
    return {};
}

u32 class_info(ActionContext& ctx, bool reg) {
    if (!require_script(ctx)) return kActionFailure;
    Session& s = ctx.session();
    capsule::Registry* registry = ctx.services().registry;
    if (registry == nullptr) return kActionFailure;
    const auto [hive, root] = classes_root(s);
    Rows rows(s, "Class"), progids(s, "ProgId"), appids(s, "AppId");
    for (usize i = 0; i < rows.size(); ++i) {
        const std::string comp = rows.get(i, "Component_");
        if (reg ? !component_installing(s, comp) : !component_removing(s, comp)) continue;
        const std::string clsid = upper(rows.get(i, "CLSID")), context = rows.get(i, "Context");
        const std::wstring key = clsid_key(s, comp, clsid);
        const std::string ftm = rows.get(i, "FileTypeMask");
        const std::string appid = upper(rows.get(i, "AppId_"));
        Writer w{*registry, hive};
        if (!reg) {
            schedule(ctx, "UnregisterClassInfo", [=]() {
                w.del_tree(key);
                if (!ftm.empty()) w.del_tree(root + L"\\FileType\\" + widen(clsid));
                if (!appid.empty()) {
                    w.del_tree(root + L"\\AppID\\" + widen(appid));
                    w.del_tree(root + L"\\WOW6432Node\\AppID\\" + widen(appid));
                }
            });
            continue;
        }
        std::string file = component_key_file(s, comp);
        if (file.empty()) {
            log_unimplemented("RegisterClassInfo", "class server component whose KeyPath is not a file");
            return kActionFailure;
        }
        const u32 attrs = static_cast<u32>(std::strtoul(rows.get(i, "Attributes").c_str(), nullptr, 10));
        if (attrs & 1) file = file_name_of(file);  // msidbClassAttributesRelativePath
        const std::string arg = s.format(rows.get(i, "Argument"));
        const std::string feature = rows.get(i, "Feature_");
        const std::string desc = session_descriptor(s, feature, comp);
        std::string server = file, server_multi = desc;
        if (is_local_server(context) && !arg.empty()) {
            server += " " + arg;
            server_multi += " " + arg;
        }
        const std::string description = rows.get(i, "Description");
        const std::string progid = rows.get(i, "ProgId_Default");
        const std::string vi = vi_progid(progids, progid);
        const std::string icon = rows.get(i, "Icon_");
        const std::string icon_value = icon.empty() ? std::string{} : icon_path(s, icon, rows.get(i, "IconIndex"));
        const std::string handler = rows.get(i, "DefInprocHandler");
        if (!handler.empty() && handler != "1" && handler != "2" && handler != "3") {
            log_unimplemented("RegisterClassInfo", "DefInprocHandler naming a handler CLSID");
            return kActionFailure;
        }
        // AppId table row of this class (Learn "AppId Table").
        std::map<std::string, std::string> app_values;
        if (!appid.empty())
            for (usize a = 0; a < appids.size(); ++a) {
                if (upper(appids.get(a, "AppId")) != appid) continue;
                for (const char* col : {"RemoteServerName", "LocalService", "ServiceParameters", "DllSurrogate"})
                    if (const std::string v = s.format(appids.get(a, col)); !v.empty()) app_values[col] = v;
                if (appids.get(a, "ActivateAtStorage") == "1") app_values["ActivateAtStorage"] = "Y";
                if (appids.get(a, "RunAsInteractiveUser") == "1") app_values["RunAs"] = "Interactive User";
            }
        const std::wstring ctx_key = key + L"\\" + widen(context);
        schedule(ctx, "RegisterClassInfo",
                 [=]() {
                     if (!description.empty()) w.sz(key, L"", description);
                     if (!appid.empty()) w.sz(key, L"AppID", appid);
                     w.sz(ctx_key, L"", server);
                     if (!desc.empty()) w.multi(ctx_key, widen(context), server_multi);
                     if (!progid.empty()) w.sz(key + L"\\ProgID", L"", progid);
                     if (!vi.empty()) w.sz(key + L"\\VersionIndependentProgID", L"", vi);
                     if (!icon_value.empty()) w.sz(key + L"\\DefaultIcon", L"", icon_value);
                     if (handler == "1" || handler == "3") w.sz(key + L"\\InprocHandler", L"", "ole2.dll");
                     if (handler == "2" || handler == "3") w.sz(key + L"\\InprocHandler32", L"", "ole32.dll");
                     // FileTypeMask: ';'-separated masks -> FileType\{clsid}\0..n.
                     usize n = 0, p = 0;
                     while (!ftm.empty() && p <= ftm.size()) {
                         const usize semi = ftm.find(';', p);
                         w.sz(root + L"\\FileType\\" + widen(clsid) + L"\\" + widen(std::to_string(n++)), L"",
                              ftm.substr(p, semi - p));
                         if (semi == std::string::npos) break;
                         p = semi + 1;
                     }
                     if (!appid.empty())
                         for (const std::wstring& base : {root + L"\\AppID\\", root + L"\\WOW6432Node\\AppID\\"}) {
                             w.touch(base + widen(appid));
                             for (const auto& [n2, v] : app_values) w.sz(base + widen(appid), widen(n2), v);
                         }
                 },
                 [=]() { w.del_tree(key); });
    }
    return kActionSuccess;
}

/// Is the ProgId attached to a class or extension that this session
/// registers (reg) / removes (!reg)?
bool progid_in_use(Session& s, Rows& progids, usize row, bool reg) {
    std::string name = progids.get(row, "ProgId");
    std::string cls = progids.get(row, "Class_");
    // A version-independent ProgId follows its parent.
    if (const std::string parent = progids.get(row, "ProgId_Parent"); !parent.empty()) {
        for (usize i = 0; i < progids.size(); ++i)
            if (progids.get(i, "ProgId") == parent) {
                name = parent;
                cls = progids.get(i, "Class_");
            }
    }
    auto wanted = [&](const std::string& comp) { return reg ? component_installing(s, comp) : component_removing(s, comp); };
    Rows classes(s, "Class");
    for (usize i = 0; i < classes.size(); ++i)
        if (!cls.empty() && upper(classes.get(i, "CLSID")) == upper(cls) && wanted(classes.get(i, "Component_"))) return true;
    Rows exts(s, "Extension");
    for (usize i = 0; i < exts.size(); ++i)
        if (exts.get(i, "ProgId_") == name && wanted(exts.get(i, "Component_"))) return true;
    return false;
}

u32 progid_info(ActionContext& ctx, bool reg) {
    if (!require_script(ctx)) return kActionFailure;
    Session& s = ctx.session();
    capsule::Registry* registry = ctx.services().registry;
    if (registry == nullptr) return kActionFailure;
    const auto [hive, root] = classes_root(s);
    Rows rows(s, "ProgId");
    const auto index = progid_index(rows);
    for (usize i = 0; i < rows.size(); ++i) {
        if (!progid_in_use(s, rows, i, reg)) continue;
        const std::string name = rows.get(i, "ProgId");
        const std::wstring key = root + L"\\" + widen(name);
        Writer w{*registry, hive};
        if (!reg) {
            schedule(ctx, "UnregisterProgIdInfo", [=]() { w.del_tree(key); });
            continue;
        }
        const std::string description = rows.get(i, "Description"), parent = rows.get(i, "ProgId_Parent");
        std::string cls = upper(rows.get(i, "Class_"));
        if (!parent.empty())
            if (auto it = index.find(parent); it != index.end()) cls = upper(rows.get(it->second, "Class_"));
        const std::string icon = rows.get(i, "Icon_");
        const std::string icon_value = icon.empty() ? std::string{} : icon_path(s, icon, rows.get(i, "IconIndex"));
        schedule(ctx, "RegisterProgIdInfo",
                 [=]() {
                     if (!description.empty()) w.sz(key, L"", description);
                     else w.touch(key);
                     if (!cls.empty()) w.sz(key + L"\\CLSID", L"", cls);
                     if (!parent.empty()) w.sz(key + L"\\CurVer", L"", parent);
                     if (!icon_value.empty()) w.sz(key + L"\\DefaultIcon", L"", icon_value);
                 },
                 [=]() { w.del_tree(key); });
    }
    return kActionSuccess;
}

u32 extension_info(ActionContext& ctx, bool reg) {
    if (!require_script(ctx)) return kActionFailure;
    Session& s = ctx.session();
    capsule::Registry* registry = ctx.services().registry;
    if (registry == nullptr) return kActionFailure;
    const auto [hive, root] = classes_root(s);
    Rows rows(s, "Extension"), verbs(s, "Verb");
    for (usize i = 0; i < rows.size(); ++i) {
        const std::string comp = rows.get(i, "Component_");
        if (reg ? !component_installing(s, comp) : !component_removing(s, comp)) continue;
        const std::string ext = rows.get(i, "Extension"), progid = rows.get(i, "ProgId_");
        const std::string mime = rows.get(i, "MIME_");
        const std::wstring ext_key = root + L"\\." + widen(ext);
        // Verbs live on the ProgId when there is one, else on the extension.
        const std::wstring verb_root = (progid.empty() ? ext_key : root + L"\\" + widen(progid)) + L"\\shell";
        Writer w{*registry, hive};
        std::vector<std::pair<i64, usize>> ordered;
        for (usize v = 0; v < verbs.size(); ++v)
            if (verbs.get(v, "Extension_") == ext) {
                const std::string seq = verbs.get(v, "Sequence");
                ordered.emplace_back(seq.empty() ? INT64_MAX : std::strtoll(seq.c_str(), nullptr, 10), v);
            }
        std::sort(ordered.begin(), ordered.end());
        if (!reg) {
            std::vector<std::wstring> verb_keys;
            for (const auto& [seq, v] : ordered) verb_keys.push_back(verb_root + L"\\" + widen(verbs.get(v, "Verb")));
            schedule(ctx, "UnregisterExtensionInfo", [=]() {
                for (const auto& k : verb_keys) w.del_tree(k);
                w.del_tree(ext_key);
            });
            continue;
        }
        const std::string file = component_key_file(s, comp);
        const std::string desc = session_descriptor(s, rows.get(i, "Feature_"), comp);
        struct VerbOut {
            std::wstring key;
            std::string command, line, multi;
        };
        std::vector<VerbOut> outs;
        for (const auto& [seq, v] : ordered) {
            const std::string arg = s.format(verbs.get(v, "Argument"));
            VerbOut o;
            o.key = verb_root + L"\\" + widen(verbs.get(v, "Verb"));
            o.command = s.format(verbs.get(v, "Command"));
            o.line = "\"" + file + "\"" + (arg.empty() ? "" : " " + arg);
            o.multi = desc + (arg.empty() ? "" : " " + arg);
            outs.push_back(o);
        }
        const std::string default_verb = ordered.empty() ? std::string{} : verbs.get(ordered.front().second, "Verb");
        schedule(ctx, "RegisterExtensionInfo",
                 [=]() {
                     if (!progid.empty()) {
                         w.sz(ext_key, L"", progid);
                         w.touch(ext_key + L"\\" + widen(progid) + L"\\ShellNew");  // measured
                     } else {
                         w.touch(ext_key);
                     }
                     if (!mime.empty()) w.sz(ext_key, L"Content Type", mime);
                     if (!default_verb.empty()) w.sz(verb_root, L"", default_verb);
                     for (const VerbOut& o : outs) {
                         if (!o.command.empty()) w.sz(o.key, L"", o.command);
                         w.sz(o.key + L"\\command", L"", o.line);
                         if (!o.multi.empty()) w.multi(o.key + L"\\command", L"command", o.multi);
                     }
                 },
                 [=]() {
                     for (const VerbOut& o : outs) w.del_tree(o.key);
                     w.del_tree(ext_key);
                 });
    }
    return kActionSuccess;
}

u32 mime_info(ActionContext& ctx, bool reg) {
    if (!require_script(ctx)) return kActionFailure;
    Session& s = ctx.session();
    capsule::Registry* registry = ctx.services().registry;
    if (registry == nullptr) return kActionFailure;
    const auto [hive, root] = classes_root(s);
    Rows rows(s, "MIME"), exts(s, "Extension");
    for (usize i = 0; i < rows.size(); ++i) {
        const std::string ext = rows.get(i, "Extension_");
        bool wanted = false;
        for (usize e = 0; e < exts.size(); ++e)
            if (exts.get(e, "Extension") == ext) {
                const std::string comp = exts.get(e, "Component_");
                wanted = wanted || (reg ? component_installing(s, comp) : component_removing(s, comp));
            }
        if (!wanted) continue;
        const std::wstring key = root + L"\\MIME\\Database\\Content Type\\" + widen(rows.get(i, "ContentType"));
        const std::string clsid = upper(rows.get(i, "CLSID"));
        Writer w{*registry, hive};
        if (!reg) {
            schedule(ctx, "UnregisterMIMEInfo", [=]() { w.del_tree(key); });
            continue;
        }
        schedule(ctx, "RegisterMIMEInfo",
                 [=]() {
                     w.sz(key, L"Extension", "." + ext);
                     if (!clsid.empty()) w.sz(key, L"CLSID", clsid);
                 },
                 [=]() { w.del_tree(key); });
    }
    return kActionSuccess;
}

// ---- type libraries, COM+, assemblies --------------------------------------------------------

/// Rows of @p table whose Component_ this session installs / removes.
bool any_rows_for(Session& s, const char* table, const char* column, bool reg) {
    Rows rows(s, table);
    for (usize i = 0; i < rows.size(); ++i) {
        const std::string comp = rows.get(i, column);
        if (reg ? component_installing(s, comp) : component_removing(s, comp)) return true;
    }
    return false;
}

u32 unimplemented_table(ActionContext& ctx, const char* action, const char* table, const char* column, bool reg,
                        const char* what) {
    if (!any_rows_for(ctx.session(), table, column, reg)) return kActionSuccess;
    if (!require_script(ctx)) return kActionFailure;
    log_unimplemented(action, what);
    return kActionFailure;
}

// ---- fonts ------------------------------------------------------------------------------------

/// The full font name (name ID 4) of an sfnt font, preferring the Windows
/// platform / US English record (OpenType spec, `name` table).
std::optional<std::string> font_full_name(const std::vector<u8>& f, bool& cff) {
    auto u16be = [&](usize o) -> u32 { return o + 2 <= f.size() ? (u32{f[o]} << 8 | f[o + 1]) : 0; };
    auto u32be = [&](usize o) -> u32 { return o + 4 <= f.size() ? (u16be(o) << 16 | u16be(o + 2)) : 0; };
    if (f.size() < 12) return std::nullopt;
    const u32 version = u32be(0);
    cff = version == 0x4F54544F;  // 'OTTO'
    if (version != 0x00010000 && version != 0x74727565 && !cff) return std::nullopt;
    const u32 tables = u16be(4);
    usize name = 0;
    for (u32 t = 0; t < tables; ++t)
        if (u32be(12 + t * 16) == 0x6E616D65) name = u32be(12 + t * 16 + 8);  // 'name'
    if (name == 0) return std::nullopt;
    const u32 count = u16be(name + 2), strings = name + u16be(name + 4);
    std::optional<std::string> best;
    int best_rank = -1;
    for (u32 r = 0; r < count; ++r) {
        const usize rec = name + 6 + r * 12;
        const u32 platform = u16be(rec), encoding = u16be(rec + 2), lang = u16be(rec + 4), id = u16be(rec + 6);
        const u32 len = u16be(rec + 8), off = u16be(rec + 10);
        if (id != 4 || strings + off + len > f.size()) continue;
        int rank = -1;
        std::string text;
        if (platform == 3 && (encoding == 1 || encoding == 0)) {
            rank = lang == 0x409 ? 3 : 2;
            std::vector<u16> u;
            for (u32 k = 0; k + 1 < len; k += 2) u.push_back(static_cast<u16>(u16be(strings + off + k)));
            text = utf16_to_utf8(u.data(), static_cast<int>(u.size()));
        } else if (platform == 1 && encoding == 0) {
            rank = 1;
            text.assign(reinterpret_cast<const char*>(f.data() + strings + off), len);
        }
        if (rank > best_rank && !text.empty()) {
            best_rank = rank;
            best = text;
        }
    }
    return best;
}

std::optional<std::vector<u8>> read_all(capsule::Vfs& vfs, const std::string& path) {
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

u32 fonts(ActionContext& ctx, bool reg) {
    Session& s = ctx.session();
    Rows rows(s, "Font");
    if (rows.size() == 0) return kActionSuccess;
    if (!require_script(ctx)) return kActionFailure;
    capsule::Registry* registry = ctx.services().registry;
    capsule::Vfs* vfs = ctx.services().vfs;
    if (registry == nullptr || vfs == nullptr) return kActionFailure;
    const std::wstring key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts";
    std::string fonts_dir = s.property("FontsFolder");
    for (usize i = 0; i < rows.size(); ++i) {
        const FileRow* file = find_file_row(s, rows.get(i, "File_"));
        if (file == nullptr) continue;
        if (reg ? !component_installing(s, file->component) : !component_removing(s, file->component)) continue;
        const std::string path = file_target_path(s, file->key);
        // The value data: the file name when the font sits in FontsFolder,
        // else its full path (measured "vprobe1.ttf").
        std::string dir = path.substr(0, path.find_last_of('\\') + 1);
        const bool in_fonts = !fonts_dir.empty() && upper(dir) == upper(fonts_dir);
        const std::string data = in_fonts ? file_name_of(path) : path;
        const std::string table_title = rows.get(i, "FontTitle");
        // Null FontTitle: the font's own full name + " (TrueType)" (measured:
        // arial.ttf -> "Arial (TrueType)"), read from the installed file when
        // the op runs — InstallFiles' op, earlier in the same script, has
        // copied it by then (a compressed package has no loose source copy to
        // read while the script is generated).
        auto title_of = [vfs, path, table_title, reg]() -> std::optional<std::string> {
            if (!table_title.empty()) return table_title;
            const std::optional<std::vector<u8>> bytes = read_all(*vfs, path);
            bool cff = false;
            const auto full = bytes ? font_full_name(*bytes, cff) : std::nullopt;
            if (!full) {
                log_unimplemented(reg ? "RegisterFonts" : "UnregisterFonts",
                                  "font title of a file that is not a readable TrueType/OpenType font");
                return std::nullopt;
            }
            // CFF-outline OpenType fonts are titled "(OpenType)"; that suffix
            // was not measured (no such font on the oracle): registration.md.
            return *full + (cff ? " (OpenType)" : " (TrueType)");
        };
        auto remove_value = [key, &r = *registry](const std::string& title) {
            auto k = r.open_key(r.view_root(capsule::HKEY_LOCAL_MACHINE, capsule::RegView::Native64), key, false);
            if (k.is_ok()) {
                (void)r.delete_value(k.value(), widen(title));
                r.close_key(k.value());
            }
        };
        auto written = std::make_shared<std::string>();
        Writer w{*registry, capsule::HKEY_LOCAL_MACHINE};
        ScriptOp op;
        op.action = reg ? "RegisterFonts" : "UnregisterFonts";
        op.run = [=]() -> u32 {
            const auto title = title_of();
            if (!title) return kActionFailure;
            if (reg) {
                w.sz(key, widen(*title), data);
                *written = *title;
            } else {
                remove_value(*title);
            }
            return kActionSuccess;
        };
        if (reg) op.rollback = [=]() { if (!written->empty()) remove_value(*written); };
        ctx.schedule(std::move(op));
    }
    return kActionSuccess;
}

// ---- self-registration ----------------------------------------------------------------------------

/// SelfRegModules / SelfUnregModules: DllRegisterServer / DllUnregisterServer
/// of each SelfReg file, run through regsvr32 of the module's bitness in the
/// guest (the custom-action host's process facility).
u32 self_reg(ActionContext& ctx, bool reg) {
    Session& s = ctx.session();
    Rows rows(s, "SelfReg");
    std::vector<std::pair<std::string, bool>> modules;
    for (usize i = 0; i < rows.size(); ++i) {
        const FileRow* file = find_file_row(s, rows.get(i, "File_"));
        if (file == nullptr) continue;
        if (reg ? !component_installing(s, file->component) : !component_removing(s, file->component)) continue;
        const ComponentInfo* c = find_component(s, file->component);
        modules.emplace_back(file_target_path(s, file->key), c != nullptr && c->is_64bit());
    }
    if (modules.empty()) return kActionSuccess;
    if (!require_script(ctx)) return kActionFailure;
    CustomActionHost* host = ctx.services().custom_actions;
    if (host == nullptr) {
        log_unimplemented(reg ? "SelfRegModules" : "SelfUnregModules", "no guest process to load the modules in");
        return kActionFailure;
    }
    const std::string sys32 = s.property("SystemFolder"), sys64 = s.property("System64Folder");
    for (const auto& [path, x64] : modules) {
        const std::string exe = (x64 && !sys64.empty() ? sys64 : sys32) + "regsvr32.exe";
        const std::string line = "\"" + exe + "\" /s " + (reg ? "" : "/u ") + "\"" + path + "\"";
        ScriptOp op;
        op.action = reg ? "SelfRegModules" : "SelfUnregModules";
        auto services = std::make_shared<Services>(ctx.services());
        Session* session = &ctx.session();
        op.run = [host, exe, line, path, reg, services, session]() -> u32 {
            u32 code = 0;
            const std::vector<u16> a = utf8_to_utf16(exe), l = utf8_to_utf16(line);
            const bool ran = host->run_process(std::u16string(a.begin(), a.end()), std::u16string(l.begin(), l.end()), {},
                                               true, code);
            if (ran && code == 0) return kActionSuccess;
            // Error 1904 / 1910 ("Module ... failed to (un)register"):
            // reported, and the install continues as Windows' silent UI
            // ignores it.
            Record rec(2);
            rec.fields[1].kind = Record::Kind::Integer;
            rec.fields[1].integer = reg ? 1904 : 1910;
            const std::vector<u16> p = utf8_to_utf16(path);
            rec.fields[2].kind = Record::Kind::String;
            rec.fields[2].text = std::u16string(p.begin(), p.end());
            ActionContext later(*session, *services);
            (void)later.message(kMsgError, rec);
            return kActionSuccess;
        };
        ctx.schedule(std::move(op));
    }
    return kActionSuccess;
}

// ---- shortcuts --------------------------------------------------------------------------------------

/// A [MS-SHLLINK] Shell Link: header, StringData (name, relative path =
/// target, working dir, arguments, icon location), and the extra data blocks
/// (EXP_DARWIN_LINK for an advertised shortcut, the property store for
/// MsiShortcutProperty rows), terminated.
struct LinkProps {
    PropertyKey key;
    std::u16string value;  ///< stored VT_LPWSTR
};

/// PROPERTYKEY fmtid as its 16 bytes in memory order (Data1..3 little endian).
std::array<u8, 16> fmtid_bytes(const PropertyKey& k) {
    std::array<u8, 16> b{};
    for (int i = 0; i < 4; ++i) b[i] = static_cast<u8>(k.data1 >> (8 * i));
    for (int i = 0; i < 2; ++i) b[4 + i] = static_cast<u8>(k.data2 >> (8 * i));
    for (int i = 0; i < 2; ++i) b[6 + i] = static_cast<u8>(k.data3 >> (8 * i));
    std::copy(k.data4.begin(), k.data4.end(), b.begin() + 8);
    return b;
}

std::vector<u8> build_link(const std::string& target, const std::string& args, const std::string& workdir,
                           const std::string& description, const std::string& icon, i32 icon_index, u32 show_cmd,
                           u16 hotkey, const std::string& darwin, const std::vector<LinkProps>& props) {
    std::vector<u8> out;
    auto u16le = [&](u32 v) { out.push_back(static_cast<u8>(v)); out.push_back(static_cast<u8>(v >> 8)); };
    auto u32le = [&](u32 v) { for (int k = 0; k < 32; k += 8) out.push_back(static_cast<u8>(v >> k)); };
    auto string_data = [&](const std::string& s) {
        const std::vector<u16> w = utf8_to_utf16(s);
        u16le(static_cast<u32>(w.size()));
        for (u16 c : w) u16le(c);
    };
    constexpr u32 kHasName = 0x4, kHasRelativePath = 0x8, kHasWorkingDir = 0x10, kHasArguments = 0x20,
                  kHasIconLocation = 0x40, kIsUnicode = 0x80, kHasDarwinId = 0x1000;
    u32 flags = kIsUnicode | kHasRelativePath;
    if (!description.empty()) flags |= kHasName;
    if (!workdir.empty()) flags |= kHasWorkingDir;
    if (!args.empty()) flags |= kHasArguments;
    if (!icon.empty()) flags |= kHasIconLocation;
    if (!darwin.empty()) flags |= kHasDarwinId;
    static constexpr u8 kClsid[16] = {0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
                                      0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};  // CLSID_ShellLink
    u32le(0x4C);
    out.insert(out.end(), std::begin(kClsid), std::end(kClsid));
    u32le(flags);
    u32le(0);                            // FileAttributes
    for (int k = 0; k < 24; ++k) out.push_back(0);  // Creation/Access/Write times
    u32le(0);                            // FileSize
    u32le(static_cast<u32>(icon_index));
    u32le(show_cmd);
    u16le(hotkey);
    u16le(0);
    u32le(0);
    u32le(0);
    if (!description.empty()) string_data(description);
    string_data(target);
    if (!workdir.empty()) string_data(workdir);
    if (!args.empty()) string_data(args);
    if (!icon.empty()) string_data(icon);
    if (!darwin.empty()) {
        // EXP_DARWIN_LINK (2.5.3): size 0x314, signature 0xA0000006, ANSI[260], Unicode[520 bytes].
        u32le(0x314);
        u32le(0xA0000006);
        const std::string ansi = utf8_to_acp(darwin);
        for (usize k = 0; k < 260; ++k) out.push_back(k < ansi.size() && k < 259 ? static_cast<u8>(ansi[k]) : 0);
        const std::vector<u16> w = utf8_to_utf16(darwin);
        for (usize k = 0; k < 260; ++k) u16le(k < w.size() && k < 259 ? w[k] : 0);
    }
    if (!props.empty()) {
        // PropertyStoreDataBlock (2.5.7) holding [MS-PROPSTORE] serialized
        // storages, one per format id, values VT_LPWSTR.
        std::map<std::array<u8, 16>, std::vector<const LinkProps*>> by_fmtid;
        for (const auto& p : props) by_fmtid[fmtid_bytes(p.key)].push_back(&p);
        std::vector<u8> store;
        auto s32 = [&](std::vector<u8>& v, u32 x) { for (int k = 0; k < 32; k += 8) v.push_back(static_cast<u8>(x >> k)); };
        for (const auto& [fmtid, list] : by_fmtid) {
            std::vector<u8> body;
            for (const LinkProps* p : list) {
                std::vector<u8> value;
                const std::u16string& w = p->value;
                value.push_back(0x1F); value.push_back(0);  // VT_LPWSTR
                value.push_back(0); value.push_back(0);     // padding
                s32(value, static_cast<u32>(w.size() + 1));
                for (u16 c : w) { value.push_back(static_cast<u8>(c)); value.push_back(static_cast<u8>(c >> 8)); }
                value.push_back(0); value.push_back(0);
                while (value.size() % 4) value.push_back(0);
                s32(body, static_cast<u32>(4 + 4 + 1 + value.size()));
                s32(body, p->key.pid);
                body.push_back(0);
                body.insert(body.end(), value.begin(), value.end());
            }
            s32(body, 0);  // end of values
            std::vector<u8> storage;
            s32(storage, static_cast<u32>(4 + 4 + 16 + body.size()));
            s32(storage, 0x53505331);  // "1SPS"
            storage.insert(storage.end(), fmtid.begin(), fmtid.end());
            storage.insert(storage.end(), body.begin(), body.end());
            store.insert(store.end(), storage.begin(), storage.end());
        }
        s32(store, 0);  // end of storages
        u32le(static_cast<u32>(8 + store.size()));
        u32le(0xA0000009);
        out.insert(out.end(), store.begin(), store.end());
    }
    u32le(0);  // TerminalBlock
    return out;
}

std::string long_name(const std::string& name) {
    const usize bar = name.find('|');
    return bar == std::string::npos ? name : name.substr(bar + 1);
}

/// A 1946 warning to raise when the shortcut is written: the formatted
/// PropertyKey and the HRESULT whose low word the record carries.
struct PropertyWarning {
    std::u16string key_text;
    u32 hr{0};
};

/// One MsiShortcutProperty row, the way Windows Installer applies it
/// (tools/oracle_probe/msi_shortcut_property.expected). PropertyKey is
/// Formatted and resolved by propsys!PSGetPropertyKeyFromName: a canonical
/// name, a legacy name, or "{fmtid} pid" (Learn "MsiShortcutProperty Table",
/// "PSGetPropertyKeyFromName"). A key that does not resolve is warning 1946
/// with that HRESULT, and the shortcut is still created. The Formatted value
/// is set as a string and coerced to the property's schema type
/// (PSCoerceToCanonicalValue): for a plain string property leading and
/// trailing whitespace goes and an empty result sets nothing; a key the
/// schema does not describe keeps the string as it is, empty included.
/// Returns false when the row needs a coercion Vitreo does not implement.
bool shortcut_property(Session& s, const std::string& key_cell, const std::string& value_cell,
                       std::vector<LinkProps>& props, std::vector<PropertyWarning>& warnings) {
    constexpr u16 kVtLpwstr = 31;  // VT_LPWSTR (wtypes.h)
    const std::vector<u16> key16 = utf8_to_utf16(s.format(key_cell));
    const std::u16string key_text(key16.begin(), key16.end());
    const PropertyKeyLookup found = property_key_from_name(key_text);
    if (found.hr != 0) {
        warnings.push_back({key_text, found.hr});
        return true;
    }
    const std::vector<u16> value16 = utf8_to_utf16(s.format(value_cell));
    std::u16string value(value16.begin(), value16.end());
    const std::optional<PropertySchema> schema = property_schema(found.key);
    const bool by_key_string = !key_text.empty() && key_text.front() == u'{';
    if (schema && schema->registered) {
        if (schema->vt != kVtLpwstr || schema->tree || schema->enumerated) {
            log_unimplemented("CreateShortcuts",
                              "MsiShortcutProperty value coercion to a non-string, tree or enumerated property");
            return false;
        }
        // Measured: spaces, tabs, CR and LF are trimmed; inner runs stay.
        auto blank = [](char16_t c) { return c == u' ' || c == u'\t' || c == u'\r' || c == u'\n'; };
        usize b = 0, e = value.size();
        while (b < e && blank(value[b])) ++b;
        while (e > b && blank(value[e - 1])) --e;
        value = value.substr(b, e - b);
        if (value.empty()) return true;  // VT_EMPTY: nothing is set
    } else if (!schema && !by_key_string) {
        // A legacy name whose key propkey.h does not define: Windows has a
        // registered description for it (it resolved the name) whose type
        // Vitreo does not know.
        log_unimplemented("CreateShortcuts", "MsiShortcutProperty legacy name of a property outside propkey.h");
        return false;
    }
    // Learn: a property set twice on one shortcut is set "in an unspecified
    // order"; one value per key is stored, the later row's.
    props.erase(std::remove_if(props.begin(), props.end(), [&](const LinkProps& p) { return p.key == found.key; }),
                props.end());
    props.push_back({found.key, std::move(value)});
    return true;
}

u32 shortcuts(ActionContext& ctx, bool create) {
    Session& s = ctx.session();
    Rows rows(s, "Shortcut"), sprops(s, "MsiShortcutProperty");
    if (rows.size() == 0) return kActionSuccess;
    if (!require_script(ctx)) return kActionFailure;
    capsule::Vfs* vfs = ctx.services().vfs;
    if (vfs == nullptr) return kActionFailure;
    const bool advt_disabled = !s.property("DISABLEADVTSHORTCUTS").empty();
    for (usize i = 0; i < rows.size(); ++i) {
        const std::string comp = rows.get(i, "Component_"), target_cell = rows.get(i, "Target");
        const bool advertised = find_feature(s, target_cell) != nullptr;
        bool wanted;
        if (advertised) {
            const i32 fs = feature_final_state(s, target_cell);
            const bool present = fs == install_state::kLocal || fs == install_state::kSource || fs == install_state::kAdvertised;
            const FeatureInfo* f = find_feature(s, target_cell);
            const bool was = f && (f->installed == install_state::kLocal || f->installed == install_state::kSource ||
                                   f->installed == install_state::kAdvertised);
            wanted = create ? present : (was && !present);
        } else {
            wanted = create ? component_installing(s, comp) : component_removing(s, comp);
        }
        if (!wanted) continue;
        std::string dir = target_path(s, rows.get(i, "Directory_"));
        if (dir.empty()) continue;
        if (dir.back() != '\\') dir.push_back('\\');
        const std::string name = rows.get(i, "Name");
        const std::string file_part = !s.property("SHORTFILENAMES").empty() ? name.substr(0, name.find('|')) : long_name(name);
        const std::string lnk = dir + file_part + ".lnk";
        if (!create) {
            schedule(ctx, "RemoveShortcuts", [vfs, lnk]() { (void)vfs->remove(lnk); });
            continue;
        }
        std::string target = advertised ? component_key_file(s, comp) : s.format(target_cell);
        std::string darwin;
        if (advertised && !advt_disabled) darwin = session_descriptor(s, target_cell, comp);
        const std::string args = s.format(rows.get(i, "Arguments"));
        const std::string desc = s.format(rows.get(i, "Description"));
        const std::string wk = rows.get(i, "WkDir");
        std::string workdir = wk.empty() ? std::string{} : target_path(s, wk);
        if (workdir.empty() && !wk.empty()) workdir = s.property(wk);
        const std::string icon = rows.get(i, "Icon_");
        const std::string icon_file = icon.empty() ? std::string{} : icon_path(s, icon, "").substr(0, icon_path(s, icon, "").rfind(','));
        const i32 icon_index = std::atoi(rows.get(i, "IconIndex").c_str());
        const std::string show = rows.get(i, "ShowCmd");
        const u32 show_cmd = show.empty() ? 1u : static_cast<u32>(std::strtoul(show.c_str(), nullptr, 10));
        const u16 hotkey = static_cast<u16>(std::strtoul(rows.get(i, "Hotkey").c_str(), nullptr, 10));
        std::vector<LinkProps> props;
        std::vector<PropertyWarning> warnings;
        for (usize p = 0; p < sprops.size(); ++p) {
            if (sprops.get(p, "Shortcut_") != rows.get(i, "Shortcut")) continue;
            if (!shortcut_property(s, sprops.get(p, "PropertyKey"), sprops.get(p, "PropVariantValue"), props,
                                   warnings))
                return kActionFailure;
        }
        const std::vector<u8> bytes =
            build_link(target, args, workdir, desc, icon_file, icon_index, show_cmd, hotkey, darwin, props);
        // Error 1946 is a warning raised while the shortcut is written (the
        // execution script, measured); the install continues and the
        // shortcut is created without that property.
        const std::vector<u16> lnk_name = utf8_to_utf16(file_part + ".lnk");
        auto services = std::make_shared<Services>(ctx.services());
        Session* session = &ctx.session();
        ScriptOp op;
        op.action = "CreateShortcuts";
        op.run = [vfs, lnk, bytes, warnings, lnk_name, services, session]() -> u32 {
            for (const PropertyWarning& w : warnings) {
                Record rec(4);
                rec.fields[1].kind = Record::Kind::Integer;
                rec.fields[1].integer = 1946;
                rec.fields[2].kind = Record::Kind::String;
                rec.fields[2].text = w.key_text;
                rec.fields[3].kind = Record::Kind::String;
                rec.fields[3].text = std::u16string(lnk_name.begin(), lnk_name.end());
                rec.fields[4].kind = Record::Kind::Integer;
                rec.fields[4].integer = static_cast<i32>(w.hr & 0xFFFFu);
                ActionContext later(*session, *services);
                (void)later.message(kMsgWarning, rec);
            }
            for (usize k = 3; k < lnk.size(); ++k)
                if (lnk[k] == '\\') (void)vfs->create_dir(lnk.substr(0, k));
            auto h = vfs->open(lnk, capsule::AccessMode::Write, capsule::ShareMode::None,
                               capsule::CreationDisposition::CreateAlways);
            if (h.is_err()) return kActionSuccess;
            (void)vfs->write(h.value(), std::span<const u8>(bytes.data(), bytes.size()));
            (void)vfs->close(h.value());
            return kActionSuccess;
        };
        op.rollback = [vfs, lnk]() { (void)vfs->remove(lnk); };
        ctx.schedule(std::move(op));
    }
    return kActionSuccess;
}

}  // namespace

void register_com_and_shell_actions(ActionRegistry& r) {
    r.add("RegisterClassInfo", [](ActionContext& c) { return class_info(c, true); });
    r.add("UnregisterClassInfo", [](ActionContext& c) { return class_info(c, false); });
    r.add("RegisterProgIdInfo", [](ActionContext& c) { return progid_info(c, true); });
    r.add("UnregisterProgIdInfo", [](ActionContext& c) { return progid_info(c, false); });
    r.add("RegisterExtensionInfo", [](ActionContext& c) { return extension_info(c, true); });
    r.add("UnregisterExtensionInfo", [](ActionContext& c) { return extension_info(c, false); });
    r.add("RegisterMIMEInfo", [](ActionContext& c) { return mime_info(c, true); });
    r.add("UnregisterMIMEInfo", [](ActionContext& c) { return mime_info(c, false); });
    r.add("RegisterFonts", [](ActionContext& c) { return fonts(c, true); });
    r.add("UnregisterFonts", [](ActionContext& c) { return fonts(c, false); });
    r.add("SelfRegModules", [](ActionContext& c) { return self_reg(c, true); });
    r.add("SelfUnregModules", [](ActionContext& c) { return self_reg(c, false); });
    r.add("CreateShortcuts", [](ActionContext& c) { return shortcuts(c, true); });
    r.add("RemoveShortcuts", [](ActionContext& c) { return shortcuts(c, false); });
    // Type libraries need the interfaces of the .tlb registered with them
    // (Interface\{IID}); no type-library reader exists here, so a package
    // that registers one fails at the action rather than half-registered.
    r.add("RegisterTypeLibraries", [](ActionContext& c) {
        return unimplemented_table(c, "RegisterTypeLibraries", "TypeLib", "Component_", true,
                                   "type library registration (no .tlb reader for its interfaces)");
    });
    r.add("UnregisterTypeLibraries", [](ActionContext& c) {
        return unimplemented_table(c, "UnregisterTypeLibraries", "TypeLib", "Component_", false,
                                   "type library unregistration (no .tlb reader for its interfaces)");
    });
    r.add("RegisterComPlus", [](ActionContext& c) {
        return unimplemented_table(c, "RegisterComPlus", "Complus", "Component_", true, "COM+ application install (no COM+ catalog)");
    });
    r.add("UnregisterComPlus", [](ActionContext& c) {
        return unimplemented_table(c, "UnregisterComPlus", "Complus", "Component_", false, "COM+ application removal (no COM+ catalog)");
    });
    // MsiPublishAssemblies / MsiUnpublishAssemblies: msi_assembly.cpp.
}

}  // namespace vitreo::winapi::msi
