// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_register_api.cpp
/// @brief msi.dll installed-product query exports (registration area):
/// product / feature / component / qualified-component / patch queries,
/// enumeration, Provide*/Locate/UseFeature and the source-list functions,
/// answered from the installed-products database (msi_register.hpp).
///
/// Contract: the Microsoft Learn page of each function (Msi.h, SDK
/// 10.0.26100) and tools/oracle_probe/msi_registration_api_probe.c, run on
/// Windows 11 after per-machine and per-user installs of the registration
/// test package (msi_registration_*.expected). Where the probe and Learn
/// differ the probe wins and the contract doc says so.

#include "winapi/system/msi/msi_core.hpp"
#include "winapi/system/msi/msi.hpp"
#include "winapi/system/msi/msi_assembly.hpp"
#include "winapi/system/msi/msi_engine.hpp"
#include "winapi/system/msi/msi_product.hpp"
#include "winapi/system/msi/msi_register.hpp"
#include "winapi/system/msi/msi_register_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "capsule/vfs/vfs.hpp"
#include "capsule/registry/registry.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace vitreo::winapi::msi {

namespace {

constexpr u32 kOk = 0, kFileNotFound = 2, kInvalidHandle = 6, kInvalidParameter = 87, kCallNotImplemented = 120,
              kMoreData = 234, kNoMoreItems = 259, kInstallFailure = 1603, kUnknownProduct = 1605,
              kUnknownFeature = 1606, kUnknownComponent = 1607, kUnknownProperty = 1608, kIndexAbsent = 1611,
              kBadUsername = 2202, kUnknownPatch = 1647, kFunctionFailed = 1627;
constexpr i32 kStInvalidArg = -2, kStUnknown = -1, kStMoreData = -3, kStAbsent = 2, kStLocal = 3, kStSource = 4;
// INSTALLMODE (Msi.h).
constexpr i32 kModeNoDetectionAny = -4, kModeNoSourceResolution = -3, kModeNoDetection = -2, kModeExisting = -1,
              kModeDefault = 0;
constexpr u32 kCodePatch = 0x40000000u, kSourceNetwork = 1, kSourceUrl = 2, kSourceMedia = 4;
// USERINFOSTATE (Msi.h).
constexpr i32 kUserMoreData = -3, kUserInvalidArg = -2, kUserUnknown = -1, kUserAbsent = 0, kUserPresent = 1;

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::string acp_to_utf8(const std::string& acp) {
    if (acp.empty()) return {};
    const int n = StringConverter::multi_byte_to_wide_char(0, 0, acp.data(), static_cast<int>(acp.size()), nullptr, 0);
    std::vector<u16> w(static_cast<usize>(n > 0 ? n : 0));
    if (n > 0) StringConverter::multi_byte_to_wide_char(0, 0, acp.data(), static_cast<int>(acp.size()), w.data(), n);
    return utf16_to_utf8(w.data(), static_cast<int>(w.size()));
}

std::u16string to_u16(std::string_view s) {
    const std::vector<u16> v = utf8_to_utf16(s);
    return std::u16string(v.begin(), v.end());
}

/// The marshalling every export shares: A strings are CP_ACP, W UTF-16;
/// character counts are in the encoding's units (bytes for A).
struct Io {
    Marshaler& m;
    bool wide;

    [[nodiscard]] std::optional<std::string> str(usize arg) const {
        const GuestAddr p = m.arg_ptr(arg);
        if (p == 0) return std::nullopt;
        if (wide) {
            auto s = m.read_wide_string(p);
            if (s.is_err()) return std::nullopt;
            return s.value();
        }
        auto s = m.read_ansi_string(p);
        if (s.is_err()) return std::nullopt;
        return acp_to_utf8(s.value());
    }
    /// The units @p value occupies in the guest encoding.
    [[nodiscard]] std::vector<u16> units(const std::string& value) const {
        if (wide) return utf8_to_utf16(value);
        const std::string b = utf8_to_acp(value);
        return std::vector<u16>(b.begin(), b.end());
    }
    void write_units(GuestAddr buf, const std::vector<u16>& u, usize count) const {
        for (usize k = 0; k < count; ++k) {
            if (wide) (void)m.write_u16(buf + k * 2, u[k]);
            else (void)m.write_u8(buf + k, static_cast<u8>(u[k]));
        }
        if (wide) (void)m.write_u16(buf + count * 2, 0);
        else (void)m.write_u8(buf + count, 0);
    }
    /// A fixed 39-character GUID / feature buffer.
    void fixed(GuestAddr buf, const std::string& value) const {
        if (buf == 0) return;
        const auto u = units(value);
        write_units(buf, u, u.size());
    }

    enum class Overflow { Untouched, Truncate };
    /// The installed-product buffer convention (probe "info-size" lines):
    /// *pcch is the capacity including the terminator and becomes the length
    /// without it; a null buffer is a size query (and a null pcch with it an
    /// existence check); a buffer without pcch is ERROR_INVALID_PARAMETER; a
    /// short buffer is ERROR_MORE_DATA and is left untouched (Truncate: filled
    /// with what fits, the MsiGetProductProperty / MsiGetFeatureInfo rule).
    [[nodiscard]] u32 put(GuestAddr buf, GuestAddr pcch, const std::string& value,
                          Overflow overflow = Overflow::Untouched) const {
        if (pcch == 0) return buf == 0 ? kOk : kInvalidParameter;
        auto cap = m.read_u32(pcch);
        if (cap.is_err()) return kInvalidParameter;
        const auto u = units(value);
        const u32 need = static_cast<u32>(u.size());
        (void)m.write_u32(pcch, need);
        if (buf == 0) return kOk;
        if (need + 1 > cap.value()) {
            if (overflow == Overflow::Truncate && cap.value() > 0) write_units(buf, u, cap.value() - 1);
            return kMoreData;
        }
        write_units(buf, u, u.size());
        return kOk;
    }
};

/// A product/component/patch code argument: nullopt when absent or not a
/// registry-format GUID (ERROR_INVALID_PARAMETER / INSTALLSTATE_INVALIDARG).
std::optional<std::string> guid_arg(const Io& io, usize arg) {
    auto s = io.str(arg);
    if (!s || !is_guid(*s)) return std::nullopt;
    return upper(*s);
}

bool single_context(u32 c) { return c == kContextUserManaged || c == kContextUserUnmanaged || c == kContextMachine; }

/// The szUserSid / dwContext rules of the per-instance *Ex functions:
/// exactly one context, the machine context with a null SID, never
/// S-1-5-18, S-1-1-0 or "" (probe infoex lines). @p sid is "" for the caller.
bool instance_sid_ok(const Io& io, usize sid_arg, u32 context, std::string& sid, bool allow_everyone = false) {
    const GuestAddr p = io.m.arg_ptr(sid_arg);
    if (!single_context(context) && !(allow_everyone && (context & ~kContextAll) == 0 && context != 0)) return false;
    if (p == 0) { sid.clear(); return true; }
    if (context == kContextMachine) return false;
    auto s = io.str(sid_arg);
    if (!s || s->empty()) return false;
    sid = upper(*s);
    if (sid == kMachineSid) return false;
    if (sid == kEveryoneSid && !allow_everyone) return false;
    return true;
}

/// The enumeration SID rules (EnumProductsEx / ComponentsEx / ClientsEx /
/// PatchesEx / GetComponentPathEx): any non-empty combination of the three
/// contexts, machine-only with a null SID, S-1-5-18 rejected.
bool enum_sid_ok(const Io& io, usize sid_arg, u32 contexts, std::string& sid) {
    if (contexts == 0 || (contexts & ~kContextAll) != 0) return false;
    const GuestAddr p = io.m.arg_ptr(sid_arg);
    if (p == 0) { sid.clear(); return true; }
    if (contexts == kContextMachine) return false;
    auto s = io.str(sid_arg);
    if (!s || s->empty()) return false;
    sid = upper(*s);
    return sid != kMachineSid;
}

/// The SID output of the Ex enumerators: the owning user, "" for the machine.
u32 put_sid(const Io& io, GuestAddr buf, GuestAddr pcch, const ProductInstance& inst) {
    if (pcch == 0) return buf == 0 ? kOk : kInvalidParameter;
    return io.put(buf, pcch, inst.context == kContextMachine ? std::string{} : inst.sid);
}

struct Api {
    MsiManager& mgr;

    [[nodiscard]] std::optional<ProductDatabase> db() const { return product_database(mgr); }

    /// Does the key file / folder behind a registered key path exist?
    [[nodiscard]] bool key_path_present(const std::string& key_path) const {
        if (key_path.size() >= 3 && std::isdigit(static_cast<unsigned char>(key_path[0])) &&
            std::isdigit(static_cast<unsigned char>(key_path[1])) && key_path[2] == ':')
            return true;  // registry key path: the registration is the evidence
        capsule::Vfs* vfs = mgr.vfs();
        if (vfs == nullptr) return true;
        std::string path = key_path;
        if (path.size() > 3 && path.back() == '\\') path.pop_back();
        return vfs->stat(path).is_ok();
    }

    /// The Windows directory the installer resolves WindowsFolder to.
    [[nodiscard]] static std::string windows_dir() { return default_machine_state(nullptr, L"").windows_dir; }

    /// MsiGetComponentPath core: state + path of @p component for @p inst.
    /// A global assembly's key path ('<' / '>') stands for its file in the
    /// assembly cache; when the cache does not hold it the component is
    /// absent and @p path stays empty (probe msi_assembly "comp" lines: state
    /// 2, nothing written).
    [[nodiscard]] i32 component_path(ProductDatabase& d, const ProductInstance& inst, const std::string& component,
                                     std::string& path, const char* api = "MsiGetComponentPath") const {
        const auto kp = d.component_key_path(inst, component);
        if (!kp) return kStUnknown;
        if (is_assembly_key_path(*kp)) {
            const i32 st = d.component_state(inst, component);
            capsule::Vfs* vfs = mgr.vfs();
            const auto file = vfs ? resolve_assembly_key_path(*vfs, windows_dir(), *kp, api) : std::nullopt;
            path = file.value_or(std::string{});
            return file || st != kStLocal ? st : kStAbsent;
        }
        path = *kp;
        const i32 st = d.component_state(inst, component);
        if (st == kStLocal && !key_path_present(*kp)) return kStAbsent;
        return st;
    }

    /// The component a '<' descriptor names: its feature's only (first) component.
    [[nodiscard]] static std::string descriptor_component(ProductDatabase& d, const ProductInstance& inst,
                                                         const std::string& feature) {
        const auto list = d.feature_component_codes(inst, feature);
        return list.empty() ? std::string{} : list.front();
    }

    /// MsiProvideComponent core (Learn "MsiProvideComponent", the
    /// INSTALLMODE table; probe provide lines). Every mode counts a use.
    u32 provide(ProductDatabase& d, const ProductInstance& inst, const std::string& feature,
                const std::string& component, i32 mode, std::string& path, const char* api) {
        const i32 fs = d.feature_state(inst, feature);
        if (fs == kStUnknown) return kUnknownFeature;
        d.record_feature_use(inst, feature);
        const bool available = fs == kStLocal || fs == kStSource;
        if (mode == kModeExisting || mode == kModeNoDetection || mode == kModeNoSourceResolution) {
            if (!available) return kFileNotFound;
            if (mode == kModeNoSourceResolution && fs == kStSource) return 1612;  // ERROR_INSTALL_SOURCE_ABSENT
            const i32 cs = component_path(d, inst, component, path);
            if (cs == kStUnknown) return kUnknownComponent;
            if (mode == kModeExisting && cs != kStLocal && cs != kStSource) return kFileNotFound;
            if (mode != kModeExisting && d.component_state(inst, component) == kStAbsent) return kFileNotFound;
            return kOk;
        }
        // INSTALLMODE_DEFAULT / REINSTALLMODE flags: provide when present,
        // otherwise install on demand through MsiConfigureFeature /
        // MsiReinstallFeature, which this area does not drive.
        if (mode == kModeDefault && available) {
            const i32 cs = component_path(d, inst, component, path);
            if (cs == kStLocal || cs == kStSource) return kOk;
        }
        log_unimplemented(api, mode == kModeDefault ? "install-on-demand of an absent feature/component"
                                                    : "REINSTALLMODE flags (reinstall before providing)");
        return kInstallFailure;
    }

    /// MsiProvideAssembly's provide step: the INSTALLMODE rules of provide()
    /// with the assembly cache in place of the key file of a global assembly
    /// (measured: INSTALLMODE_EXISTING -> ERROR_FILE_NOT_FOUND when the cache
    /// lacks it, the NODETECTION modes -> ERROR_INSTALL_FAILURE; a private
    /// assembly's key file is not checked by the NODETECTION modes).
    u32 provide_assembly(ProductDatabase& d, const ProductInstance& inst, const std::string& feature,
                         const std::string& component, i32 mode, std::string& path, const char* api) {
        const i32 fs = d.feature_state(inst, feature);
        if (fs == kStUnknown) return kUnknownFeature;
        d.record_feature_use(inst, feature);
        const bool available = fs == kStLocal || fs == kStSource;
        const auto kp = d.component_key_path(inst, component);
        if (!kp) return kUnknownComponent;
        const bool global = is_assembly_key_path(*kp);
        const i32 cs = component_path(d, inst, component, path, api);
        if (mode == kModeExisting) return available && (cs == kStLocal || cs == kStSource) && !path.empty() ? kOk : kFileNotFound;
        if (mode == kModeNoDetection || mode == kModeNoSourceResolution || mode == kModeNoDetectionAny) {
            if (!available) return kFileNotFound;
            if (mode == kModeNoSourceResolution && fs == kStSource) return 1612;  // ERROR_INSTALL_SOURCE_ABSENT
            if (global && path.empty()) return kInstallFailure;
            if (!global) path = *kp;
            return kOk;
        }
        if (mode == kModeDefault && available && (cs == kStLocal || cs == kStSource) && !path.empty()) return kOk;
        log_unimplemented(api, mode == kModeDefault ? "install-on-demand / repair of an absent or missing assembly"
                                                    : "REINSTALLMODE flags (reinstall before providing)");
        return kInstallFailure;
    }
};

void reg_pair(HandlerMap& map, const char* base, std::function<void(Io)> fn) {
    for (const bool wide : {false, true}) {
        map[std::string(base) + (wide ? "W" : "A")] = [fn, wide](Marshaler& m) { fn(Io{m, wide}); };
    }
}

}  // namespace

void register_product_api(MsiManager& mgr, HandlerMap& map) {
    auto api = std::make_shared<Api>(Api{mgr});

    // ---- products ----

    reg_pair(map, "MsiQueryProductState", [api](Io io) {
        auto code = guid_arg(io, 0);
        if (!code) { io.m.set_return(static_cast<u32>(kStInvalidArg)); return; }
        auto d = api->db();
        io.m.set_return(static_cast<u32>(d ? d->product_state(*code) : kStUnknown));
    });

    reg_pair(map, "MsiEnumProducts", [api](Io io) {
        const u32 index = io.m.arg32(0);
        const GuestAddr buf = io.m.arg_ptr(1);
        if (buf == 0) { io.m.set_return(kInvalidParameter); return; }
        auto d = api->db();
        std::vector<std::string> codes;
        if (d) {
            std::set<std::string> seen;
            for (const auto& inst : d->instances(kContextAll))
                if (seen.insert(inst.product_code).second) codes.push_back(inst.product_code);
        }
        if (index >= codes.size()) {
            io.m.set_return(d && d->malformed_products(kContextAll) ? 1610u : kNoMoreItems);
            return;
        }
        io.fixed(buf, codes[index]);
        io.m.set_return(kOk);
    });

    reg_pair(map, "MsiEnumProductsEx", [api](Io io) {
        // (szProductCode, szUserSid, dwContext, dwIndex, szInstalledProductCode[39],
        //  pdwInstalledContext, szSid, pcchSid)
        const u32 contexts = io.m.arg32(2), index = io.m.arg32(3);
        std::string sid;
        if (!enum_sid_ok(io, 1, contexts, sid)) { io.m.set_return(kInvalidParameter); return; }
        std::string filter;
        if (io.m.arg_ptr(0) != 0) {
            auto c = guid_arg(io, 0);
            if (!c) { io.m.set_return(kInvalidParameter); return; }
            filter = *c;
        }
        const GuestAddr sid_buf = io.m.arg_ptr(6), sid_cch = io.m.arg_ptr(7);
        if (sid_buf != 0 && sid_cch == 0) { io.m.set_return(kInvalidParameter); return; }
        auto d = api->db();
        std::vector<ProductInstance> list;
        if (d) list = d->instances(contexts, sid, filter);
        // dwIndex past the end of the enumeration is a caller error; at the
        // end it is ERROR_NO_MORE_ITEMS (probe enumex1 lines).
        if (index > list.size()) { io.m.set_return(kInvalidParameter); return; }
        if (index == list.size()) {
            io.m.set_return(d && d->malformed_products(contexts, sid) ? 1610u : kNoMoreItems);
            return;
        }
        const ProductInstance& inst = list[index];
        io.fixed(io.m.arg_ptr(4), inst.product_code);
        if (io.m.arg_ptr(5) != 0) (void)io.m.write_u32(io.m.arg_ptr(5), inst.context);
        io.m.set_return(put_sid(io, sid_buf, sid_cch, inst));
    });

    reg_pair(map, "MsiEnumRelatedProducts", [api](Io io) {
        auto upgrade = guid_arg(io, 0);
        const GuestAddr buf = io.m.arg_ptr(3);
        if (!upgrade || io.m.arg32(1) != 0 || buf == 0) { io.m.set_return(kInvalidParameter); return; }
        auto d = api->db();
        const auto related = d ? d->related_products(*upgrade) : std::vector<std::string>{};
        const u32 index = io.m.arg32(2);
        if (index >= related.size()) { io.m.set_return(kNoMoreItems); return; }
        io.fixed(buf, related[index]);
        io.m.set_return(kOk);
    });

    reg_pair(map, "MsiGetProductInfo", [api](Io io) {
        auto code = guid_arg(io, 0);
        auto prop = io.str(1);
        if (!code || !prop) { io.m.set_return(kInvalidParameter); return; }
        const GuestAddr buf = io.m.arg_ptr(2), pcch = io.m.arg_ptr(3);
        if (buf != 0 && pcch == 0) { io.m.set_return(kInvalidParameter); return; }
        auto d = api->db();
        auto inst = d ? d->visible(*code) : std::nullopt;
        if (!inst) { io.m.set_return(kUnknownProduct); return; }
        std::string value;
        const u32 rc = d->product_info(*inst, *prop, value, false);
        if (rc != kOk) { io.m.set_return(rc); return; }
        io.m.set_return(io.put(buf, pcch, value));
    });

    reg_pair(map, "MsiGetProductInfoEx", [api](Io io) {
        // (szProductCode, szUserSid, dwContext, szProperty, szValue, pcchValue)
        auto code = guid_arg(io, 0);
        auto prop = io.str(3);
        std::string sid;
        const u32 context = io.m.arg32(2);
        const GuestAddr buf = io.m.arg_ptr(4), pcch = io.m.arg_ptr(5);
        if (!code || !prop || !instance_sid_ok(io, 1, context, sid) || (buf != 0 && pcch == 0)) {
            io.m.set_return(kInvalidParameter);
            return;
        }
        auto d = api->db();
        if (!d) { io.m.set_return(kUnknownProduct); return; }
        const ProductInstance inst{*code, context, context == kContextMachine ? std::string{} : (sid.empty() ? d->user_sid() : sid)};
        const i32 st = d->instance_state(inst);
        if (st != 5 && st != 1) { io.m.set_return(kUnknownProduct); return; }
        std::string value;
        const u32 rc = d->product_info(inst, *prop, value, true);
        if (rc != kOk) { io.m.set_return(rc); return; }
        io.m.set_return(io.put(buf, pcch, value));
    });

    reg_pair(map, "MsiIsProductElevated", [api](Io io) {
        auto code = guid_arg(io, 0);
        const GuestAddr out = io.m.arg_ptr(1);
        if (!code || out == 0) { io.m.set_return(kInvalidParameter); return; }
        (void)io.m.write_u32(out, 0);
        auto d = api->db();
        auto inst = d ? d->visible(*code) : std::nullopt;
        if (!inst) { io.m.set_return(kUnknownProduct); return; }
        // Learn: per-machine and managed per-user instances are elevated.
        (void)io.m.write_u32(out, inst->context == kContextUserUnmanaged ? 0u : 1u);
        io.m.set_return(kOk);
    });

    reg_pair(map, "MsiGetProductCodeFromPackageCode", [api](Io io) {
        auto pkg = guid_arg(io, 0);
        const GuestAddr out = io.m.arg_ptr(1);
        if (!pkg || out == 0) { io.m.set_return(kInvalidParameter); return; }
        auto d = api->db();
        const std::string code = d ? d->product_from_package_code(*pkg) : std::string{};
        if (code.empty()) { io.m.set_return(kUnknownProduct); return; }
        io.fixed(out, code);
        io.m.set_return(kOk);
    });

    reg_pair(map, "MsiGetUserInfo", [api](Io io) {
        auto code = guid_arg(io, 0);
        if (!code) { io.m.set_return(static_cast<u32>(kUserInvalidArg)); return; }
        auto d = api->db();
        auto inst = d ? d->visible(*code) : std::nullopt;
        if (!inst) { io.m.set_return(static_cast<u32>(kUserUnknown)); return; }
        const auto owner = d->install_property(*inst, "RegOwner");
        const auto company = d->install_property(*inst, "RegCompany");
        const auto serial = d->install_property(*inst, "ProductID");
        if (!owner || !serial) {
            // Probe "userinfo" (per-user, nothing registered): the user-name
            // buffer gets "" and its count drops by one; the others are left.
            const GuestAddr ub = io.m.arg_ptr(1), uc = io.m.arg_ptr(2);
            if (uc != 0) {
                auto cap = io.m.read_u32(uc);
                if (cap.is_ok() && cap.value() > 0) {
                    if (ub != 0) io.write_units(ub, {}, 0);
                    (void)io.m.write_u32(uc, cap.value() - 1);
                }
            }
            io.m.set_return(static_cast<u32>(kUserAbsent));
            return;
        }
        i32 state = kUserPresent;
        const std::pair<usize, std::string> outs[] = {{1, *owner}, {3, company.value_or("")}, {5, *serial}};
        for (const auto& [arg, value] : outs) {
            const u32 rc = io.put(io.m.arg_ptr(arg), io.m.arg_ptr(arg + 1), value);
            if (rc == kMoreData) state = kUserMoreData;
            else if (rc == kInvalidParameter) { io.m.set_return(static_cast<u32>(kUserInvalidArg)); return; }
        }
        io.m.set_return(static_cast<u32>(state));
    });

    reg_pair(map, "MsiGetProductProperty", [api](Io io) {
        // (hProduct, szProperty, lpValueBuf, pcchValueBuf): the product
        // database's Property table, truncating on overflow (probe prodprop).
        auto h = api->mgr.handles().get<InstallHandle>(io.m.arg32(0));
        if (!h || !h->engine) { io.m.set_return(kInvalidHandle); return; }
        auto prop = io.str(1);
        const GuestAddr buf = io.m.arg_ptr(2), pcch = io.m.arg_ptr(3);
        if (!prop || (buf != 0 && pcch == 0)) { io.m.set_return(kInvalidParameter); return; }
        std::string value;
        if (Package* pkg = h->engine->session().package()) {
            if (const Table* t = pkg->table("Property")) {
                const auto kc = t->column_index("Property"), vc = t->column_index("Value");
                if (kc && vc)
                    for (const auto& row : t->rows)
                        if (row[*kc] == *prop) value = row[*vc];
            }
        }
        io.m.set_return(io.put(buf, pcch, value, Io::Overflow::Truncate));
    });

    reg_pair(map, "MsiGetProductInfoFromScript", [](Io io) {
        log_unimplemented(io.wide ? "MsiGetProductInfoFromScriptW" : "MsiGetProductInfoFromScriptA",
                          "advertise scripts are not read");
        io.m.set_return(kInstallFailure);
    });

    // ---- features ----

    reg_pair(map, "MsiEnumFeatures", [api](Io io) {
        auto code = guid_arg(io, 0);
        const GuestAddr fbuf = io.m.arg_ptr(2), pbuf = io.m.arg_ptr(3);
        if (!code || fbuf == 0) { io.m.set_return(kInvalidParameter); return; }
        auto d = api->db();
        auto inst = d ? d->visible(*code) : std::nullopt;
        if (!inst) { io.m.set_return(kUnknownProduct); return; }
        const auto list = d->features(*inst);
        const u32 index = io.m.arg32(1);
        if (index >= list.size()) { io.m.set_return(kNoMoreItems); return; }
        const auto& [feature, parent] = list[index];
        if (io.units(feature).size() > kMaxFeatureChars) { io.m.set_return(kMoreData); return; }
        io.fixed(fbuf, feature);
        if (pbuf != 0) {
            auto u = io.units(parent);
            io.write_units(pbuf, u, std::min<usize>(u.size(), kMaxFeatureChars));
        }
        io.m.set_return(kOk);
    });

    reg_pair(map, "MsiQueryFeatureState", [api](Io io) {
        auto code = guid_arg(io, 0);
        auto feature = io.str(1);
        if (!code || !feature) { io.m.set_return(static_cast<u32>(kStInvalidArg)); return; }
        auto d = api->db();
        auto inst = d ? d->visible(*code) : std::nullopt;
        io.m.set_return(static_cast<u32>(inst ? d->feature_state(*inst, *feature) : kStUnknown));
    });

    reg_pair(map, "MsiQueryFeatureStateEx", [api](Io io) {
        // (szProductCode, szUserSid, dwContext, szFeature, pdwState)
        auto code = guid_arg(io, 0);
        auto feature = io.str(3);
        std::string sid;
        const u32 context = io.m.arg32(2);
        if (!code || !feature || !instance_sid_ok(io, 1, context, sid)) { io.m.set_return(kInvalidParameter); return; }
        auto d = api->db();
        if (!d) { io.m.set_return(kUnknownProduct); return; }
        const ProductInstance inst{*code, context, context == kContextMachine ? std::string{} : (sid.empty() ? d->user_sid() : sid)};
        const i32 ps = d->instance_state(inst);
        if (ps != 5 && ps != 1) { io.m.set_return(kUnknownProduct); return; }
        const i32 st = d->feature_state(inst, *feature);
        if (io.m.arg_ptr(4) != 0) (void)io.m.write_u32(io.m.arg_ptr(4), static_cast<u32>(st));
        io.m.set_return(st == kStUnknown ? kUnknownFeature : kOk);
    });

    reg_pair(map, "MsiGetFeatureInfo", [api](Io io) {
        // (hProduct, szFeature, lpAttributes, lpTitleBuf, pcchTitleBuf, lpHelpBuf, pcchHelpBuf)
        auto h = api->mgr.handles().get<InstallHandle>(io.m.arg32(0));
        if (!h || !h->engine) { io.m.set_return(kInvalidHandle); return; }
        auto feature = io.str(1);
        if (!feature) { io.m.set_return(kInvalidParameter); return; }
        Package* pkg = h->engine->session().package();
        const Table* t = pkg ? pkg->table("Feature") : nullptr;
        const std::vector<std::string>* row = nullptr;
        std::optional<usize> ft, fd, fa;
        if (t) {
            const auto fk = t->column_index("Feature");
            ft = t->column_index("Title");
            fd = t->column_index("Description");
            fa = t->column_index("Attributes");
            if (fk)
                for (const auto& r : t->rows)
                    if (r[*fk] == *feature) row = &r;  // case-sensitive (probe finfo featmain)
        }
        if (row == nullptr) { io.m.set_return(kUnknownFeature); return; }
        if (io.m.arg_ptr(2) != 0) {
            // msidbFeatureAttributes -> INSTALLFEATUREATTRIBUTE: each bit doubles;
            // FAVORLOCAL when FavorSource is clear (Learn; probe attrs=1 for 0).
            const u32 a = fa ? static_cast<u32>(std::strtoul((*row)[*fa].c_str(), nullptr, 10)) : 0;
            u32 out = (a & 1) ? 2u : 1u;
            if (a & 2) out |= 4;
            if (a & 4) out |= 8;
            if (a & 8) out |= 16;
            if (a & 32) out |= 32;
            (void)io.m.write_u32(io.m.arg_ptr(2), out);
        }
        u32 rc = kOk;
        const std::pair<usize, std::string> outs[] = {{3, ft ? (*row)[*ft] : ""}, {5, fd ? (*row)[*fd] : ""}};
        for (const auto& [arg, value] : outs) {
            const u32 r = io.put(io.m.arg_ptr(arg), io.m.arg_ptr(arg + 1), value, Io::Overflow::Truncate);
            if (r == kInvalidParameter) { io.m.set_return(r); return; }
            if (r == kMoreData) rc = kMoreData;
        }
        io.m.set_return(rc);
    });

    reg_pair(map, "MsiGetFeatureUsage", [api](Io io) {
        auto code = guid_arg(io, 0);
        auto feature = io.str(1);
        if (!code || !feature) { io.m.set_return(kInvalidParameter); return; }
        auto d = api->db();
        auto inst = d ? d->visible(*code) : std::nullopt;
        if (!inst) { io.m.set_return(kInstallFailure); return; }
        // An unused (or unpublished) feature reports 0 uses on date 0 (probe fusage).
        const auto [count, date] = d->feature_usage(*inst, *feature);
        if (io.m.arg_ptr(2) != 0) (void)io.m.write_u32(io.m.arg_ptr(2), count);
        if (io.m.arg_ptr(3) != 0) (void)io.m.write_u16(io.m.arg_ptr(3), date);
        io.m.set_return(kOk);
    });

    auto use_feature = [api](Io io, bool ex) {
        auto code = guid_arg(io, 0);
        auto feature = io.str(1);
        if (!code || !feature) { io.m.set_return(static_cast<u32>(kStInvalidArg)); return; }
        if (ex) {
            const i32 mode = static_cast<i32>(io.m.arg32(2));
            if ((mode != 0 && mode != kModeNoDetection) || io.m.arg32(3) != 0) {
                io.m.set_return(static_cast<u32>(kStInvalidArg));
                return;
            }
        }
        auto d = api->db();
        auto inst = d ? d->visible(*code) : std::nullopt;
        if (!inst) { io.m.set_return(static_cast<u32>(kStUnknown)); return; }
        const i32 st = d->feature_state(*inst, *feature);
        if (st != kStUnknown) d->record_feature_use(*inst, *feature);
        io.m.set_return(static_cast<u32>(st));
    };
    reg_pair(map, "MsiUseFeature", [use_feature](Io io) { use_feature(io, false); });
    reg_pair(map, "MsiUseFeatureEx", [use_feature](Io io) { use_feature(io, true); });

    // ---- components ----

    reg_pair(map, "MsiEnumComponents", [api](Io io) {
        const GuestAddr buf = io.m.arg_ptr(1);
        if (buf == 0) { io.m.set_return(kInvalidParameter); return; }
        auto d = api->db();
        std::vector<std::string> codes;
        if (d) {
            std::set<std::string> seen;
            for (const auto& c : d->components(kContextAll))
                if (seen.insert(c.component).second) codes.push_back(c.component);
        }
        const u32 index = io.m.arg32(0);
        if (index >= codes.size()) { io.m.set_return(kNoMoreItems); return; }
        io.fixed(buf, codes[index]);
        io.m.set_return(kOk);
    });

    reg_pair(map, "MsiEnumComponentsEx", [api](Io io) {
        // (szUserSid, dwContext, dwIndex, szInstalledComponentCode[39], pdwInstalledContext, szSid, pcchSid)
        std::string sid;
        if (!enum_sid_ok(io, 0, io.m.arg32(1), sid)) { io.m.set_return(kInvalidParameter); return; }
        const GuestAddr sid_buf = io.m.arg_ptr(5), sid_cch = io.m.arg_ptr(6);
        if (sid_buf != 0 && sid_cch == 0) { io.m.set_return(kInvalidParameter); return; }
        auto d = api->db();
        const auto list = d ? d->components(io.m.arg32(1), sid) : std::vector<ProductDatabase::RegisteredComponent>{};
        const u32 index = io.m.arg32(2);
        if (index >= list.size()) { io.m.set_return(kNoMoreItems); return; }
        io.fixed(io.m.arg_ptr(3), list[index].component);
        if (io.m.arg_ptr(4) != 0) (void)io.m.write_u32(io.m.arg_ptr(4), list[index].context);
        io.m.set_return(put_sid(io, sid_buf, sid_cch, ProductInstance{"", list[index].context, list[index].sid}));
    });

    reg_pair(map, "MsiEnumClients", [api](Io io) {
        auto comp = guid_arg(io, 0);
        const GuestAddr buf = io.m.arg_ptr(2);
        if (!comp || buf == 0) { io.m.set_return(kInvalidParameter); return; }
        auto d = api->db();
        std::vector<std::string> codes;
        if (d) {
            std::set<std::string> seen;
            for (const auto& [inst, kp] : d->component_clients(*comp, kContextAll))
                if (seen.insert(inst.product_code).second) codes.push_back(inst.product_code);
        }
        if (codes.empty()) { io.m.set_return(kUnknownComponent); return; }
        const u32 index = io.m.arg32(1);
        if (index >= codes.size()) { io.m.set_return(kNoMoreItems); return; }
        io.fixed(buf, codes[index]);
        io.m.set_return(kOk);
    });

    reg_pair(map, "MsiEnumClientsEx", [api](Io io) {
        // (szComponent, szUserSid, dwContext, dwProductIndex, szProductBuf[39], pdwInstalledContext, szSid, pcchSid)
        auto comp = guid_arg(io, 0);
        std::string sid;
        if (!comp || !enum_sid_ok(io, 1, io.m.arg32(2), sid)) { io.m.set_return(kInvalidParameter); return; }
        const GuestAddr sid_buf = io.m.arg_ptr(6), sid_cch = io.m.arg_ptr(7);
        if (sid_buf != 0 && sid_cch == 0) { io.m.set_return(kInvalidParameter); return; }
        auto d = api->db();
        const auto list = d ? d->component_clients(*comp, io.m.arg32(2), sid)
                            : std::vector<std::pair<ProductInstance, std::string>>{};
        const u32 index = io.m.arg32(3);
        if (index >= list.size()) { io.m.set_return(kNoMoreItems); return; }
        io.fixed(io.m.arg_ptr(4), list[index].first.product_code);
        if (io.m.arg_ptr(5) != 0) (void)io.m.write_u32(io.m.arg_ptr(5), list[index].first.context);
        io.m.set_return(put_sid(io, sid_buf, sid_cch, list[index].first));
    });

    /// Put a component path with the INSTALLSTATE convention: MOREDATA on a
    /// short buffer (probe "comppath small"), INVALIDARG for a buffer without pcch.
    auto put_path = [](const Io& io, GuestAddr buf, GuestAddr pcch, i32 state, const std::string& path) -> i32 {
        if (state == kStUnknown) return state;
        if (state == kStAbsent && path.empty()) return state;  // an assembly the cache does not hold
        const u32 rc = io.put(buf, pcch, path);
        if (rc == kMoreData) return kStMoreData;
        if (rc == kInvalidParameter) return kStInvalidArg;
        return state;
    };

    reg_pair(map, "MsiGetComponentPath", [api, put_path](Io io) {
        auto code = guid_arg(io, 0);
        auto comp = guid_arg(io, 1);
        if (!code || !comp) { io.m.set_return(static_cast<u32>(kStInvalidArg)); return; }
        auto d = api->db();
        auto inst = d ? d->visible(*code) : std::nullopt;
        if (!inst) { io.m.set_return(static_cast<u32>(kStUnknown)); return; }
        std::string path;
        const i32 st = api->component_path(*d, *inst, *comp, path);
        io.m.set_return(static_cast<u32>(put_path(io, io.m.arg_ptr(2), io.m.arg_ptr(3), st, path)));
    });

    reg_pair(map, "MsiGetComponentPathEx", [api, put_path](Io io) {
        // (szProductCode, szComponentCode, szUserSid, dwContext, lpOutPathBuffer, pcchOutPathBuffer)
        auto code = guid_arg(io, 0);
        auto comp = guid_arg(io, 1);
        std::string sid;
        if (!code || !comp || !enum_sid_ok(io, 2, io.m.arg32(3), sid)) {
            io.m.set_return(static_cast<u32>(kStInvalidArg));
            return;
        }
        auto d = api->db();
        if (!d) { io.m.set_return(static_cast<u32>(kStUnknown)); return; }
        for (const auto& inst : d->instances(io.m.arg32(3), sid, *code)) {
            std::string path;
            const i32 st = api->component_path(*d, inst, *comp, path);
            if (st == kStUnknown) continue;
            io.m.set_return(static_cast<u32>(put_path(io, io.m.arg_ptr(4), io.m.arg_ptr(5), st, path)));
            return;
        }
        io.m.set_return(static_cast<u32>(kStUnknown));
    });

    reg_pair(map, "MsiLocateComponent", [api, put_path](Io io) {
        auto comp = guid_arg(io, 0);
        if (!comp) { io.m.set_return(static_cast<u32>(kStInvalidArg)); return; }
        auto d = api->db();
        if (!d) { io.m.set_return(static_cast<u32>(kStUnknown)); return; }
        for (const auto& [inst, kp] : d->component_clients(*comp, kContextAll)) {
            std::string path;
            const i32 st = api->component_path(*d, inst, *comp, path);
            io.m.set_return(static_cast<u32>(put_path(io, io.m.arg_ptr(1), io.m.arg_ptr(2), st, path)));
            return;
        }
        io.m.set_return(static_cast<u32>(kStUnknown));
    });

    reg_pair(map, "MsiQueryComponentState", [api](Io io) {
        // (szProductCode, szUserSid, dwContext, szComponentCode, pdwState)
        auto code = guid_arg(io, 0);
        auto comp = guid_arg(io, 3);
        std::string sid;
        const u32 context = io.m.arg32(2);
        if (!code || !comp || !instance_sid_ok(io, 1, context, sid)) { io.m.set_return(kInvalidParameter); return; }
        auto d = api->db();
        if (!d) { io.m.set_return(kUnknownProduct); return; }
        const ProductInstance inst{*code, context, context == kContextMachine ? std::string{} : (sid.empty() ? d->user_sid() : sid)};
        const i32 ps = d->instance_state(inst);
        if (ps != 5 && ps != 1) { io.m.set_return(kUnknownProduct); return; }
        std::string path;
        const i32 st = api->component_path(*d, inst, *comp, path);
        if (io.m.arg_ptr(4) != 0) (void)io.m.write_u32(io.m.arg_ptr(4), static_cast<u32>(st));
        io.m.set_return(st == kStUnknown ? kUnknownComponent : kOk);
    });

    reg_pair(map, "MsiGetProductCode", [api](Io io) {
        auto comp = guid_arg(io, 0);
        const GuestAddr out = io.m.arg_ptr(1);
        if (!comp || out == 0) { io.m.set_return(kInvalidParameter); return; }
        auto d = api->db();
        const auto clients = d ? d->component_clients(*comp, kContextAll)
                               : std::vector<std::pair<ProductInstance, std::string>>{};
        if (clients.empty()) { io.m.set_return(kUnknownComponent); return; }
        io.fixed(out, clients.front().first.product_code);
        io.m.set_return(kOk);
    });

    // ---- qualified components / provide ----

    reg_pair(map, "MsiEnumComponentQualifiers", [api](Io io) {
        // (szComponent, iIndex, lpQualifierBuf, pcchQualifierBuf, lpApplicationDataBuf, pcchApplicationDataBuf)
        auto cat = guid_arg(io, 0);
        const GuestAddr qb = io.m.arg_ptr(2), qc = io.m.arg_ptr(3), ab = io.m.arg_ptr(4), ac = io.m.arg_ptr(5);
        if (!cat || qb == 0 || qc == 0 || (ab != 0 && ac == 0)) { io.m.set_return(kInvalidParameter); return; }
        auto d = api->db();
        const auto list = d ? d->qualifiers(*cat) : std::vector<ProductDatabase::Qualifier>{};
        if (list.empty()) { io.m.set_return(kUnknownComponent); return; }
        const u32 index = io.m.arg32(1);
        if (index >= list.size()) { io.m.set_return(kNoMoreItems); return; }
        // The qualifier count is reported only when the buffer is short; on
        // success it is left as passed (probe "qualifier 0 ... 128").
        auto cap = io.m.read_u32(qc);
        const auto qu = io.units(list[index].qualifier);
        if (cap.is_err() || qu.size() + 1 > cap.value()) {
            (void)io.m.write_u32(qc, static_cast<u32>(qu.size()));
            io.m.set_return(kMoreData);
            return;
        }
        io.write_units(qb, qu, qu.size());
        io.m.set_return(ab != 0 || ac != 0 ? io.put(ab, ac, list[index].app_data) : kOk);
    });

    auto provide_qualified = [api](Io io, bool ex) {
        // (szCategory, szQualifier, dwInstallMode, [szProduct, dwUnused1, dwUnused2,] lpPathBuf, pcchPathBuf)
        auto cat = guid_arg(io, 0);
        auto qualifier = io.str(1);
        const i32 mode = static_cast<i32>(io.m.arg32(2));
        std::optional<std::string> product;
        if (ex) {
            if (io.m.arg_ptr(3) != 0) {
                product = guid_arg(io, 3);
                if (!product) { io.m.set_return(kInvalidParameter); return; }
            }
            if (io.m.arg32(4) != 0 || io.m.arg32(5) != 0) { io.m.set_return(kInvalidParameter); return; }
        }
        const usize bi = ex ? 6 : 3;
        if (!cat || !qualifier) { io.m.set_return(kInvalidParameter); return; }
        auto d = api->db();
        const auto list = d ? d->qualifiers(*cat) : std::vector<ProductDatabase::Qualifier>{};
        if (list.empty()) { io.m.set_return(kUnknownComponent); return; }
        const ProductDatabase::Qualifier* q = nullptr;
        for (const auto& e : list)
            if (e.qualifier == *qualifier) q = &e;
        if (q == nullptr) { io.m.set_return(kIndexAbsent); return; }
        Descriptor desc;
        if (decompose_descriptor(to_u16(q->descriptor), desc) != kOk) { io.m.set_return(1610); return; }
        if (product && upper(desc.product) != *product) { io.m.set_return(kIndexAbsent); return; }
        auto inst = d->visible(desc.product);
        if (!inst) { io.m.set_return(kUnknownProduct); return; }
        std::string comp = desc.component.empty() ? Api::descriptor_component(*d, *inst, desc.feature) : desc.component;
        std::string path;
        const u32 rc = api->provide(*d, *inst, desc.feature, comp, mode, path,
                                    ex ? "MsiProvideQualifiedComponentEx" : "MsiProvideQualifiedComponent");
        io.m.set_return(rc == kOk ? io.put(io.m.arg_ptr(bi), io.m.arg_ptr(bi + 1), path) : rc);
    };
    reg_pair(map, "MsiProvideQualifiedComponent", [provide_qualified](Io io) { provide_qualified(io, false); });
    reg_pair(map, "MsiProvideQualifiedComponentEx", [provide_qualified](Io io) { provide_qualified(io, true); });

    reg_pair(map, "MsiProvideComponent", [api](Io io) {
        // (szProduct, szFeature, szComponent, dwInstallMode, lpPathBuf, pcchPathBuf)
        auto code = guid_arg(io, 0);
        auto feature = io.str(1);
        auto comp = guid_arg(io, 2);
        if (!code || !feature || !comp) { io.m.set_return(kInvalidParameter); return; }
        auto d = api->db();
        auto inst = d ? d->visible(*code) : std::nullopt;
        if (!inst) { io.m.set_return(kUnknownProduct); return; }
        std::string path;
        const u32 rc = api->provide(*d, *inst, *feature, *comp, static_cast<i32>(io.m.arg32(3)), path, "MsiProvideComponent");
        io.m.set_return(rc == kOk ? io.put(io.m.arg_ptr(4), io.m.arg_ptr(5), path) : rc);
    });

    reg_pair(map, "MsiProvideComponentFromDescriptor", [api](Io io) {
        // (szDescriptor, pszPath, pcchPath, pcchArgsOffset): the path, quoted,
        // followed by the text after the descriptor (probe from-descriptor).
        auto text = io.str(0);
        if (!text) { io.m.set_return(kInvalidParameter); return; }
        Descriptor desc;
        const std::u16string u = to_u16(*text);
        if (decompose_descriptor(u, desc) != kOk) { io.m.set_return(kInvalidParameter); return; }
        auto d = api->db();
        auto inst = d ? d->visible(desc.product) : std::nullopt;
        if (!inst) { io.m.set_return(kUnknownProduct); return; }
        std::string comp = desc.component.empty() ? Api::descriptor_component(*d, *inst, desc.feature) : desc.component;
        std::string path;
        const u32 rc = api->provide(*d, *inst, desc.feature, comp, kModeDefault, path, "MsiProvideComponentFromDescriptor");
        if (rc != kOk) { io.m.set_return(rc); return; }
        const std::u16string rest = u.substr(desc.consumed);
        const std::string tail = utf16_to_utf8(reinterpret_cast<const u16*>(rest.data()), static_cast<int>(rest.size()));
        // Measured: the args offset reads 0 after the call.
        if (io.m.arg_ptr(3) != 0) (void)io.m.write_u32(io.m.arg_ptr(3), 0);
        io.m.set_return(io.put(io.m.arg_ptr(1), io.m.arg_ptr(2), "\"" + path + "\"" + tail));
    });

    reg_pair(map, "MsiQueryFeatureStateFromDescriptor", [api](Io io) {
        auto text = io.str(0);
        Descriptor desc;
        if (!text || decompose_descriptor(to_u16(*text), desc) != kOk) {
            io.m.set_return(static_cast<u32>(kStInvalidArg));
            return;
        }
        auto d = api->db();
        auto inst = d ? d->visible(desc.product) : std::nullopt;
        io.m.set_return(static_cast<u32>(inst ? d->feature_state(*inst, desc.feature) : kStUnknown));
    });

    // MsiProvideAssembly (Learn; probe msi_assembly_provide_probe.c, every
    // line of msi_assembly_*_api32/_api64.expected). The name is looked up
    // among the values of <Installer root>\Assemblies (dwAssemblyInfo 0) or
    // Win32Assemblies (1) \Global or \<szAppContext with '|' for '\'> that
    // MsiPublishAssemblies wrote; its descriptor names the product, feature
    // and component to provide.
    reg_pair(map, "MsiProvideAssembly", [api](Io io) {
        // (szAssemblyName, szAppContext, dwInstallMode, dwAssemblyInfo, lpPathBuf, pcchPathBuf)
        const char* fn = io.wide ? "MsiProvideAssemblyW" : "MsiProvideAssemblyA";
        auto text = io.str(0);
        const u32 info = io.m.arg32(3);
        const i32 mode = static_cast<i32>(io.m.arg32(2));
        if (!text || info > 1 || mode < kModeNoDetectionAny) { io.m.set_return(kInvalidParameter); return; }
        const bool win32 = info == 1;
        // Measured: a name the type's parser rejects (empty, trailing ',' for
        // .NET, an unknown or miscased Win32 attribute) is ERROR_BAD_CONFIGURATION.
        const auto request = win32 ? parse_win32_assembly_name(*text) : parse_net_assembly_name(*text);
        if (!request) { io.m.set_return(1610); return; }
        std::string context = "Global";
        if (io.m.arg_ptr(1) != 0) {
            auto app = io.str(1);
            if (!app) { io.m.set_return(kInvalidParameter); return; }
            context = *app;
            std::replace(context.begin(), context.end(), '\\', '|');
        }
        auto d = api->db();
        if (!d) { io.m.set_return(kUnknownComponent); return; }
        std::vector<std::wstring> descriptors;
        capsule::Registry& reg = d->registry();
        for (const u32 c : {kContextUserManaged, kContextUserUnmanaged, kContextMachine}) {
            const auto keys = d->keys(c, d->user_sid());
            auto k = reg.open_key(reg.view_root(keys.advertised_hive, capsule::RegView::Native64),
                                  keys.advertised_root + (win32 ? L"\\Win32Assemblies\\" : L"\\Assemblies\\") + reg_detail::widen(context),
                                  false);
            if (k.is_err()) continue;
            if (auto names = reg.enum_values(k.value()); names.is_ok()) {
                for (const std::wstring& n : names.value()) {
                    const std::string n8 = reg_detail::narrow(n);
                    const auto have = win32 ? parse_win32_assembly_name(n8) : parse_net_assembly_name(n8);
                    if (!have || !(win32 ? win32_name_matches(*request, *have) : net_name_matches(*request, *have))) continue;
                    if (auto v = reg.query_value(k.value(), n); v.is_ok())
                        if (const auto* list = std::get_if<std::vector<std::wstring>>(&v.value().data)) descriptors = *list;
                    break;
                }
            }
            reg.close_key(k.value());
            if (!descriptors.empty()) break;
        }
        if (descriptors.empty()) { io.m.set_return(kUnknownComponent); return; }
        // The first descriptor is the product that published it last;
        // INSTALLMODE_NODETECTION_ANY accepts any product's (Learn).
        u32 rc = kUnknownComponent;
        std::string path;
        for (const std::wstring& e : descriptors) {
            Descriptor desc;
            if (decompose_descriptor(to_u16(reg_detail::narrow(e)), desc) != kOk) { rc = 1610; break; }
            auto inst = d->visible(desc.product);
            if (!inst) rc = kUnknownProduct;
            else {
                const std::string comp =
                    desc.component.empty() ? Api::descriptor_component(*d, *inst, desc.feature) : desc.component;
                rc = api->provide_assembly(*d, *inst, desc.feature, comp, mode, path, fn);
            }
            if (rc == kOk || mode != kModeNoDetectionAny) break;
        }
        if (rc != kOk) { io.m.set_return(rc); return; }
        // Buffer rules, measured: *pcch becomes the length; the Unicode form
        // succeeds even when the buffer is short (and leaves it untouched),
        // the ANSI form truncates and returns ERROR_MORE_DATA, except for a
        // zero-length buffer, which is a size query in both.
        const GuestAddr buf = io.m.arg_ptr(4), pcch = io.m.arg_ptr(5);
        if (pcch == 0) { io.m.set_return(buf == 0 ? kOk : kInvalidParameter); return; }
        auto cap = io.m.read_u32(pcch);
        if (cap.is_err()) { io.m.set_return(kInvalidParameter); return; }
        const auto u = io.units(path);
        (void)io.m.write_u32(pcch, static_cast<u32>(u.size()));
        if (buf == 0 || cap.value() == 0) { io.m.set_return(kOk); return; }
        if (u.size() + 1 > cap.value()) {
            if (io.wide) { io.m.set_return(kOk); return; }
            // Measured: the first *pcch characters are written (cch 5 ->
            // "C:\WI"); no terminator was seen within the buffer.
            for (u32 k = 0; k < cap.value(); ++k) (void)io.m.write_u8(buf + k, static_cast<u8>(u[k]));
            io.m.set_return(kMoreData);
            return;
        }
        io.write_units(buf, u, u.size());
        io.m.set_return(kOk);
    });

    // ---- patches ----
    // No patch is ever registered (patches are applied by the productops
    // area, which records none: see registration.md). Every query answers
    // for a product with no patches.

    reg_pair(map, "MsiEnumPatches", [api](Io io) {
        auto code = guid_arg(io, 0);
        if (!code || io.m.arg_ptr(2) == 0 || io.m.arg_ptr(3) == 0 || io.m.arg_ptr(4) == 0) {
            io.m.set_return(kInvalidParameter);
            return;
        }
        auto d = api->db();
        io.m.set_return(d && d->visible(*code) ? kNoMoreItems : kUnknownProduct);
    });

    reg_pair(map, "MsiEnumPatchesEx", [api](Io io) {
        // (szProductCode, szUserSid, dwContext, dwFilter, dwIndex, ...)
        std::string sid;
        const u32 filter = io.m.arg32(3);
        if (!enum_sid_ok(io, 1, io.m.arg32(2), sid) || filter == 0 || (filter & ~15u) != 0) {
            io.m.set_return(kInvalidParameter);
            return;
        }
        // A product not installed in the contexts enumerates nothing (probe
        // patchesex for a per-user product queried per-machine: 259).
        if (io.m.arg_ptr(0) != 0 && !guid_arg(io, 0)) { io.m.set_return(kInvalidParameter); return; }
        if (io.m.arg_ptr(8) != 0 && io.m.arg_ptr(9) == 0) { io.m.set_return(kInvalidParameter); return; }
        io.m.set_return(kNoMoreItems);
    });

    reg_pair(map, "MsiGetPatchInfo", [](Io io) {
        auto patch = guid_arg(io, 0);
        auto attr = io.str(1);
        if (!patch || !attr || (io.m.arg_ptr(2) != 0 && io.m.arg_ptr(3) == 0)) { io.m.set_return(kInvalidParameter); return; }
        if (*attr != "LocalPackage") { io.m.set_return(kUnknownProperty); return; }
        io.m.set_return(kUnknownProduct);  // probe patchinfo: an unregistered patch
    });

    reg_pair(map, "MsiGetPatchInfoEx", [api](Io io) {
        // (szPatchCode, szProductCode, szUserSid, dwContext, szProperty, lpValue, pcchValue)
        auto patch = guid_arg(io, 0);
        auto code = guid_arg(io, 1);
        auto prop = io.str(4);
        std::string sid;
        const u32 context = io.m.arg32(3);
        if (!patch || !code || !prop || !instance_sid_ok(io, 2, context, sid) ||
            (io.m.arg_ptr(5) != 0 && io.m.arg_ptr(6) == 0)) {
            io.m.set_return(kInvalidParameter);
            return;
        }
        auto d = api->db();
        if (!d) { io.m.set_return(kUnknownProduct); return; }
        const ProductInstance inst{*code, context, context == kContextMachine ? std::string{} : (sid.empty() ? d->user_sid() : sid)};
        const i32 ps = d->instance_state(inst);
        io.m.set_return(ps == 5 || ps == 1 ? kUnknownPatch : kUnknownProduct);
    });

    // ---- source lists ----

    /// Resolve (code, sid, context, options) of the SourceList*Ex family to
    /// an instance; returns an error code or kOk.
    auto source_instance = [api](const Io& io, u32 context, u32 options, bool allow_everyone, ProductInstance& inst,
                                 std::optional<ProductDatabase>& d) -> u32 {
        auto code = guid_arg(io, 0);
        std::string sid;
        if (!code || !instance_sid_ok(io, 1, context, sid, allow_everyone)) return kInvalidParameter;
        if ((options & kCodePatch) != 0) return kUnknownPatch;  // no patch is registered
        d = api->db();
        if (!d) return kUnknownProduct;
        if (sid == kEveryoneSid) {
            const auto all = d->instances(context, kEveryoneSid, *code);
            if (all.empty()) return kUnknownProduct;
            inst = all.front();
            return kOk;
        }
        inst = ProductInstance{*code, context, context == kContextMachine ? std::string{} : (sid.empty() ? d->user_sid() : sid)};
        const i32 ps = d->instance_state(inst);
        return ps == 5 || ps == 1 ? kOk : kUnknownProduct;
    };

    /// The ordered sources of one type ("Net" / "URL").
    auto sources_of = [](ProductDatabase& d, const ProductInstance& inst, const char* sub) {
        auto values = d.source_list_values(inst, sub);
        std::vector<std::pair<u32, std::string>> out;
        for (const auto& [n, v] : values) {
            const u32 idx = static_cast<u32>(std::strtoul(n.c_str(), nullptr, 10));
            if (idx > 0) out.emplace_back(idx, v);
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    auto rewrite_sources = [](ProductDatabase& d, const ProductInstance& inst, const char* sub,
                              const std::vector<std::pair<u32, std::string>>& old_list,
                              const std::vector<std::string>& new_list) {
        for (const auto& [i, v] : old_list) d.delete_source_list_value(inst, sub, std::to_string(i));
        for (usize i = 0; i < new_list.size(); ++i)
            d.set_source_list_value(inst, sub, std::to_string(i + 1), new_list[i], true);
    };
    auto type_sub = [](u32 options) -> const char* {
        const u32 t = options & 7u;
        if (t == kSourceNetwork) return "Net";
        if (t == kSourceUrl) return "URL";
        return nullptr;
    };

    /// MsiSourceListAddSourceEx semantics (Learn: dwIndex rules).
    auto add_source = [sources_of, rewrite_sources](ProductDatabase& d, const ProductInstance& inst, const char* sub,
                                                    const std::string& source, u32 index) {
        auto old_list = sources_of(d, inst, sub);
        std::vector<std::string> list;
        for (const auto& [i, v] : old_list) list.push_back(v);
        auto same = [&](const std::string& v) { return upper(v) == upper(source); };
        const auto it = std::find_if(list.begin(), list.end(), same);
        const bool existing = it != list.end();
        if (index == 0) {
            if (existing) return;
            list.push_back(source);
        } else {
            if (existing) list.erase(it);
            if (index - 1 < list.size()) list.insert(list.begin() + (index - 1), source);
            else list.push_back(source);
        }
        rewrite_sources(d, inst, sub, old_list, list);
    };

    reg_pair(map, "MsiSourceListAddSourceEx", [source_instance, type_sub, add_source](Io io) {
        // (szProductCodeOrPatchCode, szUserSid, dwContext, dwOptions, szSource, dwIndex)
        const u32 options = io.m.arg32(3);
        const char* sub = type_sub(options);
        auto source = io.str(4);
        if (sub == nullptr || !source || source->empty() || (options & ~(kCodePatch | 7u)) != 0) {
            io.m.set_return(kInvalidParameter);
            return;
        }
        ProductInstance inst;
        std::optional<ProductDatabase> d;
        const u32 rc = source_instance(io, io.m.arg32(2), options, false, inst, d);
        if (rc != kOk) { io.m.set_return(rc); return; }
        add_source(*d, inst, sub, *source, io.m.arg32(5));
        io.m.set_return(kOk);
    });

    reg_pair(map, "MsiSourceListAddMediaDisk", [source_instance](Io io) {
        // (szProductCodeOrPatchCode, szUserSid, dwContext, dwOptions, dwDiskId, szVolumeLabel, szDiskPrompt)
        const u32 options = io.m.arg32(3);
        if ((options & ~kCodePatch) != 0) { io.m.set_return(kInvalidParameter); return; }
        ProductInstance inst;
        std::optional<ProductDatabase> d;
        const u32 rc = source_instance(io, io.m.arg32(2), options, false, inst, d);
        if (rc != kOk) { io.m.set_return(rc); return; }
        d->set_source_list_value(inst, "Media", std::to_string(io.m.arg32(4)),
                                 io.str(5).value_or("") + ";" + io.str(6).value_or(""), false);
        io.m.set_return(kOk);
    });

    reg_pair(map, "MsiSourceListClearMediaDisk", [source_instance](Io io) {
        const u32 options = io.m.arg32(3);
        if ((options & ~kCodePatch) != 0) { io.m.set_return(kInvalidParameter); return; }
        ProductInstance inst;
        std::optional<ProductDatabase> d;
        const u32 rc = source_instance(io, io.m.arg32(2), options, false, inst, d);
        if (rc != kOk) { io.m.set_return(rc); return; }
        d->delete_source_list_value(inst, "Media", std::to_string(io.m.arg32(4)));
        io.m.set_return(kOk);
    });

    /// LastUsedSource is "<type>;<index>;<path>" (probe: "n;1;C:\...\").
    auto forget_last_used_if = [](ProductDatabase& d, const ProductInstance& inst, char type, const std::string* path) {
        const std::string last = d.source_list_value(inst, "", "LastUsedSource").value_or("");
        if (last.empty() || last[0] != type) return;
        if (path != nullptr) {
            const usize second = last.find(';', 2);
            if (second == std::string::npos || upper(last.substr(second + 1)) != upper(*path)) return;
        }
        d.delete_source_list_value(inst, "", "LastUsedSource");
    };

    reg_pair(map, "MsiSourceListClearSource", [source_instance, type_sub, sources_of, rewrite_sources,
                                               forget_last_used_if](Io io) {
        // (szProductCodeOrPatchCode, szUserSid, dwContext, dwOptions, szSource)
        const u32 options = io.m.arg32(3);
        const char* sub = type_sub(options);
        auto source = io.str(4);
        if (sub == nullptr || !source || source->empty()) { io.m.set_return(kInvalidParameter); return; }
        ProductInstance inst;
        std::optional<ProductDatabase> d;
        const u32 rc = source_instance(io, io.m.arg32(2), options, false, inst, d);
        if (rc != kOk) { io.m.set_return(rc); return; }
        auto old_list = sources_of(*d, inst, sub);
        std::vector<std::string> list;
        for (const auto& [i, v] : old_list)
            if (upper(v) != upper(*source)) list.push_back(v);
        if (list.size() != old_list.size()) {
            rewrite_sources(*d, inst, sub, old_list, list);
            forget_last_used_if(*d, inst, sub[0] == 'N' ? 'n' : 'u', &*source);
        }
        io.m.set_return(kOk);  // removed or not found
    });

    reg_pair(map, "MsiSourceListClearAllEx", [source_instance, sources_of, rewrite_sources, forget_last_used_if](Io io) {
        // (szProductCodeOrPatchCode, szUserSid, dwContext, dwOptions)
        const u32 options = io.m.arg32(3), type = options & 7u;
        if (type != kSourceNetwork && type != kSourceUrl && type != kSourceMedia) { io.m.set_return(kInvalidParameter); return; }
        ProductInstance inst;
        std::optional<ProductDatabase> d;
        const u32 rc = source_instance(io, io.m.arg32(2), options, false, inst, d);
        if (rc != kOk) { io.m.set_return(rc); return; }
        const char* sub = type == kSourceNetwork ? "Net" : type == kSourceUrl ? "URL" : "Media";
        rewrite_sources(*d, inst, sub, sources_of(*d, inst, sub), {});
        forget_last_used_if(*d, inst, type == kSourceNetwork ? 'n' : type == kSourceUrl ? 'u' : 'm', nullptr);
        io.m.set_return(kOk);
    });

    reg_pair(map, "MsiSourceListForceResolutionEx", [source_instance](Io io) {
        const u32 options = io.m.arg32(3);
        if ((options & ~kCodePatch) != 0) { io.m.set_return(kInvalidParameter); return; }
        ProductInstance inst;
        std::optional<ProductDatabase> d;
        const u32 rc = source_instance(io, io.m.arg32(2), options, false, inst, d);
        if (rc != kOk) { io.m.set_return(rc); return; }
        d->delete_source_list_value(inst, "", "LastUsedSource");
        io.m.set_return(kOk);
    });

    reg_pair(map, "MsiSourceListEnumSources", [source_instance, type_sub, sources_of](Io io) {
        // (szProductCodeOrPatchCode, szUserSid, dwContext, dwOptions, dwIndex, szSource, pcchSource)
        const u32 options = io.m.arg32(3);
        const char* sub = type_sub(options);
        if (sub == nullptr) { io.m.set_return(kInvalidParameter); return; }
        ProductInstance inst;
        std::optional<ProductDatabase> d;
        const u32 rc = source_instance(io, io.m.arg32(2), options, true, inst, d);
        if (rc != kOk) { io.m.set_return(rc); return; }
        const auto list = sources_of(*d, inst, sub);
        const u32 index = io.m.arg32(4);
        if (index >= list.size()) { io.m.set_return(kNoMoreItems); return; }
        io.m.set_return(io.put(io.m.arg_ptr(5), io.m.arg_ptr(6), list[index].second));
    });

    reg_pair(map, "MsiSourceListEnumMediaDisks", [source_instance](Io io) {
        // (code, sid, ctx, options, dwIndex, pdwDiskId, szVolumeLabel, pcchVolumeLabel, szDiskPrompt, pcchDiskPrompt)
        const u32 options = io.m.arg32(3);
        if ((options & ~kCodePatch) != 0) { io.m.set_return(kInvalidParameter); return; }
        ProductInstance inst;
        std::optional<ProductDatabase> d;
        const u32 rc = source_instance(io, io.m.arg32(2), options, true, inst, d);
        if (rc != kOk) { io.m.set_return(rc); return; }
        std::vector<std::pair<u32, std::string>> disks;
        for (const auto& [n, v] : d->source_list_values(inst, "Media")) {
            if (n.empty() || !std::all_of(n.begin(), n.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
                continue;  // MediaPackagePath / DiskPrompt
            disks.emplace_back(static_cast<u32>(std::strtoul(n.c_str(), nullptr, 10)), v);
        }
        std::sort(disks.begin(), disks.end());
        const u32 index = io.m.arg32(4);
        if (index >= disks.size()) { io.m.set_return(kNoMoreItems); return; }
        const auto& [id, text] = disks[index];
        const usize semi = text.find(';');
        if (io.m.arg_ptr(5) != 0) (void)io.m.write_u32(io.m.arg_ptr(5), id);
        u32 out = io.put(io.m.arg_ptr(6), io.m.arg_ptr(7), text.substr(0, semi));
        const u32 r2 = io.put(io.m.arg_ptr(8), io.m.arg_ptr(9), semi == std::string::npos ? "" : text.substr(semi + 1));
        if (out == kOk) out = r2;
        io.m.set_return(out);
    });

    reg_pair(map, "MsiSourceListGetInfo", [source_instance](Io io) {
        // (code, sid, ctx, options, szProperty, szValue, pcchValue)
        const u32 options = io.m.arg32(3);
        auto prop = io.str(4);
        if ((options & ~kCodePatch) != 0 || !prop || (io.m.arg_ptr(5) != 0 && io.m.arg_ptr(6) == 0)) {
            io.m.set_return(kInvalidParameter);
            return;
        }
        ProductInstance inst;
        std::optional<ProductDatabase> d;
        const u32 rc = source_instance(io, io.m.arg32(2), options, false, inst, d);
        if (rc != kOk) { io.m.set_return(rc); return; }
        std::string value;
        const std::string last = d->source_list_value(inst, "", "LastUsedSource").value_or("");
        if (*prop == "MediaPackagePath" || *prop == "DiskPrompt") value = d->source_list_value(inst, "Media", *prop).value_or("");
        else if (*prop == "PackageName") value = d->source_list_value(inst, "", "PackageName").value_or("");
        else if (*prop == "LastUsedType") value = last.empty() ? "" : last.substr(0, 1);
        else if (*prop == "LastUsedSource") {
            const usize second = last.find(';', last.find(';') + 1);
            value = second == std::string::npos ? "" : last.substr(second + 1);
        } else {
            io.m.set_return(kUnknownProperty);
            return;
        }
        io.m.set_return(io.put(io.m.arg_ptr(5), io.m.arg_ptr(6), value));
    });

    reg_pair(map, "MsiSourceListSetInfo", [source_instance, type_sub, add_source, sources_of](Io io) {
        // (code, sid, ctx, options, szProperty, szValue)
        const u32 options = io.m.arg32(3);
        auto prop = io.str(4);
        auto value = io.str(5);
        if (!prop || !value) { io.m.set_return(kInvalidParameter); return; }
        const bool last = *prop == "LastUsedSource";
        const char* sub = type_sub(options);
        if (last ? sub == nullptr : (options & ~kCodePatch) != 0) { io.m.set_return(kInvalidParameter); return; }
        ProductInstance inst;
        std::optional<ProductDatabase> d;
        const u32 rc = source_instance(io, io.m.arg32(2), options, false, inst, d);
        if (rc != kOk) { io.m.set_return(rc); return; }
        if (*prop == "MediaPackagePath" || *prop == "DiskPrompt") d->set_source_list_value(inst, "Media", *prop, *value, false);
        else if (*prop == "PackageName") d->set_source_list_value(inst, "", "PackageName", *value, false);
        else if (last) {
            // Registers the source if needed, then records it as last used.
            add_source(*d, inst, sub, *value, 0);
            u32 index = 0;
            for (const auto& [i, v] : sources_of(*d, inst, sub))
                if (upper(v) == upper(*value)) index = i;
            d->set_source_list_value(inst, "", "LastUsedSource",
                                     std::string(sub[0] == 'N' ? "n" : "u") + ";" + std::to_string(index) + ";" + *value, true);
        } else {
            io.m.set_return(kUnknownProperty);
            return;
        }
        io.m.set_return(kOk);
    });

    /// The legacy szUserName forms (Learn "MsiSourceListAddSource"): null or
    /// "" is the machine instance; the caller's own name is its per-user
    /// (unmanaged, then managed) instance; another user cannot be resolved.
    auto legacy_instance = [api](const Io& io, ProductInstance& inst, std::optional<ProductDatabase>& d) -> u32 {
        auto code = guid_arg(io, 0);
        if (!code || io.m.arg32(2) != 0) return kInvalidParameter;
        d = api->db();
        if (!d) return kUnknownProduct;
        const std::string user = io.str(1).value_or("");
        if (user.empty()) {
            inst = ProductInstance{*code, kContextMachine, {}};
            return d->instance_state(inst) == 5 || d->instance_state(inst) == 1 ? kOk : kUnknownProduct;
        }
        const MachineState ms = default_machine_state(api->mgr.registry(), api->mgr.user_sid());
        const usize slash = user.rfind('\\');
        if (upper(user.substr(slash == std::string::npos ? 0 : slash + 1)) != upper(ms.logon_user)) return kBadUsername;
        for (const u32 c : {kContextUserUnmanaged, kContextUserManaged}) {
            inst = ProductInstance{*code, c, d->user_sid()};
            const i32 s = d->instance_state(inst);
            if (s == 5 || s == 1) return kOk;
        }
        return kUnknownProduct;
    };

    reg_pair(map, "MsiSourceListAddSource", [legacy_instance, add_source](Io io) {
        auto source = io.str(3);
        ProductInstance inst;
        std::optional<ProductDatabase> d;
        u32 rc = source ? legacy_instance(io, inst, d) : kInvalidParameter;
        if (rc == kOk && source->empty()) rc = kInvalidParameter;
        if (rc == kOk) add_source(*d, inst, "Net", *source, 0);
        io.m.set_return(rc);
    });

    reg_pair(map, "MsiSourceListClearAll", [legacy_instance, sources_of, rewrite_sources, forget_last_used_if](Io io) {
        ProductInstance inst;
        std::optional<ProductDatabase> d;
        const u32 rc = legacy_instance(io, inst, d);
        if (rc == kOk) {
            rewrite_sources(*d, inst, "Net", sources_of(*d, inst, "Net"), {});
            forget_last_used_if(*d, inst, 'n', nullptr);
        }
        io.m.set_return(rc);
    });

    reg_pair(map, "MsiSourceListForceResolution", [legacy_instance](Io io) {
        ProductInstance inst;
        std::optional<ProductDatabase> d;
        const u32 rc = legacy_instance(io, inst, d);
        if (rc == kOk) d->delete_source_list_value(inst, "", "LastUsedSource");
        io.m.set_return(rc);
    });

    // QueryInstanceCount: exported by msi.dll, declared in no SDK header and
    // documented nowhere, so it has no contract to implement.
    map["QueryInstanceCount"] = [](Marshaler& m) {
        log_unimplemented("QueryInstanceCount", "undocumented export (no signature in any SDK header or Learn page)");
        m.set_return(kCallNotImplemented);
    };
    (void)kFunctionFailed;
}

}  // namespace vitreo::winapi::msi
