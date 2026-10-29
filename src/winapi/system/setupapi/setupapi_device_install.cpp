// SPDX-License-Identifier: LGPL-2.1-only

#include "winapi/system/setupapi/setupapi_internal.hpp"

#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace vitreo::winapi::setupapi_internal {

// ---- device installation ------------------------------------------------------------

/// BOOL SetupDiCreateDeviceInfoW(HDEVINFO, PCWSTR DeviceName, const GUID*,
///     PCWSTR Description, HWND, DWORD CreationFlags, PSP_DEVINFO_DATA)
/// "The caller of this function must be a member of the Administrators
/// group" (Learn). s7_setupapi_probe cdi_* (non-elevated): bad set 6, bad
/// flags 1004, NULL name 0xE0000205, then ERROR_ACCESS_DENIED for every
/// otherwise-valid call. Elevated, the element is created in the set only
/// (it becomes a device when DIF_REGISTERDEVICE registers it).
void create_device_info(Marshaler& m, const StatePtr& state) {
    const u64 handle = m.arg(0);
    const GuestAddr name_ptr = m.arg_ptr(1);
    const GuestAddr class_ptr = m.arg_ptr(2);
    const GuestAddr desc_ptr = m.arg_ptr(3);
    (void)m.arg_ptr(4);
    const u32 flags = m.arg32(5);
    const GuestAddr did = m.arg_ptr(6);
    std::lock_guard lock(state->mutex);
    DevInfoSet* set = state->find_set(handle);
    if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE);
    if ((flags & ~(DICD_GENERATE_ID | DICD_INHERIT_CLASSDRVS)) != 0) {
        return fail(m, error_codes::ERROR_INVALID_FLAGS);
    }
    const auto name = read_guest_string(m, name_ptr, true);
    if (!name) return fail(m, ERROR_INVALID_DEVINST_NAME);
    if (!state->elevated()) return fail(m, error_codes::ERROR_ACCESS_DENIED);
    const auto class_guid = read_guid(m, class_ptr);
    if (!class_guid) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
    if (set->has_class && *class_guid != set->class_guid) return fail(m, ERROR_CLASS_MISMATCH);
    const auto nodes = state->nodes();
    auto id_taken = [&](std::string_view id) {
        if (find_node(nodes, id) != nullptr) return true;
        return std::any_of(set->elements.begin(), set->elements.end(),
                           [id](const SetElement& e) { return iequal(e.instance_id, id); });
    };
    std::string id;
    if ((flags & DICD_GENERATE_ID) != 0) {
        const std::string device = "ROOT\\" + ascii_upper(*name);
        if (name->empty() || name->find('\\') != std::string::npos ||
            !valid_instance_id(device + "\\0000")) {
            return fail(m, ERROR_INVALID_DEVINST_NAME);
        }
        for (u32 n = 0; n < 10000 && id.empty(); ++n) {
            char suffix[8];
            std::snprintf(suffix, sizeof(suffix), "\\%04u", n);
            if (!id_taken(device + suffix)) id = device + suffix;
        }
        if (id.empty()) return fail(m, ERROR_DEVINST_ALREADY_EXISTS);
    } else {
        if (!valid_instance_id(*name)) return fail(m, ERROR_INVALID_DEVINST_NAME);
        if (id_taken(*name)) return fail(m, ERROR_DEVINST_ALREADY_EXISTS);
        id = *name;
    }
    SetElement e;
    e.token = next_token();
    e.instance_id = id;
    e.class_guid = *class_guid;
    e.created = true;
    if (const auto desc = read_guest_string(m, desc_ptr, true); desc && !desc->empty()) {
        e.created_props[0x00] = utf16_bytes(*desc);   // SPDRP_DEVICEDESC
    }
    if ((flags & DICD_INHERIT_CLASSDRVS) != 0 && set->class_drivers) {
        e.class_drivers = set->class_drivers;
    }
    set->elements.push_back(std::move(e));
    if (did != 0) {
        const auto cb = m.read_u32(did);
        if (cb.is_err() || cb.value() != layout(m).devinfo ||
            !write_devinfo(m, did, set->elements.back())) {
            // Learn: the element has been added even so.
            return fail(m, ERROR_INVALID_USER_BUFFER);
        }
    }
    succeed(m);
}

/// BOOL SetupDiSetDeviceRegistryPropertyW(HDEVINFO, PSP_DEVINFO_DATA, DWORD
///     Property, const BYTE*, DWORD Size) — administrators only (Learn).
/// s7_setupapi_probe sdrp_* (non-elevated): bad set 6; bad DeviceInfoData 87;
/// an ordinal past SPDRP_MAXIMUM_PROPERTY 0xE0000209; NULL buffer with a size
/// 1784; then ERROR_ACCESS_DENIED. Elevated, a created element stores the
/// settable properties (Learn's list, plus HARDWAREID/COMPATIBLEIDS for these
/// root-enumerated elements and CLASSGUID); a registered devnode's
/// properties cannot be rewritten here.
namespace {

/// SPDRP_* values whose data is a string or multi-string (SetupAPI.h
/// comments / Learn property pages): the A form hands them in the ANSI code
/// page, the device keeps UTF-16.
[[nodiscard]] bool string_property(u32 property) {
    switch (property) {
        case 0x01: case 0x02: case 0x08: case 0x0C: case 0x0D:
        case 0x11: case 0x12: case 0x18: case 0x1D:
            return true;
        default:
            return false;
    }
}

void set_registry_property_impl(Marshaler& m, const StatePtr& state, bool wide);

}  // namespace

void set_registry_property(Marshaler& m, const StatePtr& state) {
    set_registry_property_impl(m, state, true);
}

void set_registry_property_a(Marshaler& m, const StatePtr& state) {
    set_registry_property_impl(m, state, false);
}

namespace {

void set_registry_property_impl(Marshaler& m, const StatePtr& state, bool wide) {
    const u64 handle = m.arg(0);
    const GuestAddr did = m.arg_ptr(1);
    const u32 property = m.arg32(2);
    const GuestAddr buffer = m.arg_ptr(3);
    const u32 size = m.arg32(4);
    std::lock_guard lock(state->mutex);
    DevInfoSet* set = state->find_set(handle);
    if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE);
    const auto element = resolve_element(m, *set, did);
    if (!element) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
    if (property >= SPDRP_MAXIMUM_PROPERTY) return fail(m, ERROR_INVALID_REG_PROPERTY);
    if (buffer == 0 && size != 0) return fail(m, ERROR_INVALID_USER_BUFFER);
    if (!state->elevated()) return fail(m, error_codes::ERROR_ACCESS_DENIED);
    SetElement& e = set->elements[*element];
    if (!e.created) {
        log_unimplemented_once("setupapi!SetupDiSetDeviceRegistryPropertyW",
                               "writing a property of a registered device");
        return fail(m, error_codes::ERROR_ACCESS_DENIED);
    }
    static constexpr u32 kSettable[] = {
        0x01, 0x02, 0x08, 0x0A, 0x0C, 0x0D, 0x11, 0x12, 0x17, 0x18, 0x1A, 0x1D, 0x21};
    if (std::find(std::begin(kSettable), std::end(kSettable), property) == std::end(kSettable)) {
        return fail(m, ERROR_INVALID_REG_PROPERTY);
    }
    if (buffer == 0) {
        e.created_props.erase(property);
        return succeed(m);
    }
    std::vector<u8> bytes(size);
    if (m.memory().read_bytes(buffer, bytes.data(), bytes.size()).is_err()) {
        return fail(m, ERROR_INVALID_USER_BUFFER);
    }
    if (!wide && string_property(property)) {
        // Each ANSI character (embedded NULs of a multi-string included)
        // becomes one UTF-16 unit run through the ANSI code page.
        const int n = StringConverter::multi_byte_to_wide_char(
            0, 0, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), nullptr, 0);
        std::vector<u16> units(static_cast<usize>(std::max(n, 0)));
        if (n > 0) {
            (void)StringConverter::multi_byte_to_wide_char(
                0, 0, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()),
                units.data(), n);
        }
        bytes.resize(units.size() * 2);
        std::memcpy(bytes.data(), units.data(), bytes.size());
    }
    if (property == 0x08) {   // SPDRP_CLASSGUID: the element changes class
        std::vector<u16> units(bytes.size() / 2);
        std::memcpy(units.data(), bytes.data(), units.size() * 2);
        while (!units.empty() && units.back() == 0) units.pop_back();
        const auto guid = parse_guid_string(
            utf16_to_utf8(units.data(), static_cast<int>(units.size())));
        if (!guid) return fail(m, error_codes::ERROR_INVALID_DATA);
        e.class_guid = *guid;
        (void)m.memory().write_bytes(did + 4, guid->data(), 16);
        return succeed(m);
    }
    e.created_props[property] = std::move(bytes);
    succeed(m);
}

}  // namespace

/// BOOL SetupDiCallClassInstaller(DI_FUNCTION, HDEVINFO, PSP_DEVINFO_DATA)
/// https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdicallclassinstaller
/// A 32-bit caller on 64-bit Windows always gets ERROR_IN_WOW64 (Learn;
/// s7_setupapi_probe cci_* x86, even for a bad set). x64 (non-elevated,
/// measured): bad set 6; bad DeviceInfoData 87; DIF_REGISTERDEVICE on an
/// already-registered device succeeds, without a device 87;
/// DIF_INSTALLDEVICE / DIF_PROPERTYCHANGE without a device 87;
/// DIF_SELECTBESTCOMPATDRV with no compatible driver 0xE0000228; a code no
/// installer handles 0xE000020E. Everything that would change the device
/// configuration needs the PnP manager, which Vitreo does not have: it
/// fails ERROR_ACCESS_DENIED, the failure a non-administrator gets.
void call_class_installer(Marshaler& m, const StatePtr& state) {
    const u32 dif = m.arg32(0);
    const u64 handle = m.arg(1);
    const GuestAddr did = m.arg_ptr(2);
    if (!m.cpu_is_64bit()) return fail(m, ERROR_IN_WOW64);
    std::lock_guard lock(state->mutex);
    DevInfoSet* set = state->find_set(handle);
    if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE);
    SetElement* element = nullptr;
    if (did != 0) {
        const auto index = resolve_element(m, *set, did);
        if (!index) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
        element = &set->elements[*index];
    }
    const bool known = dif != 0 && (dif <= DIF_FINISHINSTALL_ACTION || dif == DIF_RESERVED2);
    if (!known) return fail(m, ERROR_DI_DO_DEFAULT);
    switch (dif) {
        case DIF_REGISTERDEVICE:
            if (element == nullptr) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
            if (!element->created) return succeed(m);
            log_unimplemented_once("setupapi!SetupDiCallClassInstaller",
                                   "DIF_REGISTERDEVICE of a new device");
            return fail(m, error_codes::ERROR_ACCESS_DENIED);
        case DIF_SELECTBESTCOMPATDRV:
            if (element == nullptr) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
            if (!element->compat_drivers || element->compat_drivers->empty()) {
                return fail(m, ERROR_NO_COMPAT_DRIVERS);
            }
            break;
        case DIF_INSTALLDEVICE:
        case DIF_PROPERTYCHANGE:
            if (element == nullptr) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
            break;
        default: break;
    }
    if (state->elevated()) {
        char what[64];
        std::snprintf(what, sizeof(what), "DIF 0x%02x (no PnP manager)", dif);
        log_unimplemented_once("setupapi!SetupDiCallClassInstaller", what);
    }
    fail(m, error_codes::ERROR_ACCESS_DENIED);
}

/// BOOL SetupCopyOEMInfW(PCWSTR SourceInfFileName, PCWSTR, DWORD MediaType,
///     DWORD CopyStyle, PWSTR, DWORD, PDWORD, PWSTR*)
/// "A caller of this function is required have administrative privileges,
/// otherwise the function fails" (Learn). s7_setupapi_probe coi_*
/// (non-elevated): NULL source or a media type past SPOST_URL 87; a missing
/// (or relative, not found) file ERROR_FILE_NOT_FOUND; then
/// ERROR_ACCESS_DENIED with every output untouched. Staging a driver package
/// (catalog and signature checks, oemN.inf) is not implemented for elevated
/// callers either.
void copy_oem_inf(Marshaler& m, const StatePtr& state) {
    copy_oem_inf_impl(m, state, true);
}

void copy_oem_inf_a(Marshaler& m, const StatePtr& state) {
    copy_oem_inf_impl(m, state, false);
}

void copy_oem_inf_impl(Marshaler& m, const StatePtr& state, bool wide) {
    const GuestAddr source_ptr = m.arg_ptr(0);
    (void)m.arg_ptr(1);
    const u32 media_type = m.arg32(2);
    for (usize i = 3; i < 8; ++i) (void)m.arg(i);
    const auto source = read_guest_string(m, source_ptr, wide);
    if (!source || media_type > SPOST_URL) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
    if (state->vfs == nullptr || source->empty() || state->vfs->stat(*source).is_err()) {
        return fail(m, error_codes::ERROR_FILE_NOT_FOUND);
    }
    if (state->elevated()) {
        log_unimplemented_once(wide ? "setupapi!SetupCopyOEMInfW" : "setupapi!SetupCopyOEMInfA",
                               "staging a driver package");
    }
    fail(m, error_codes::ERROR_ACCESS_DENIED);
}

/// BOOL SetupGetInfDriverStoreLocationW(PCWSTR FileName, PSP_ALTPLATFORM_INFO,
///     PCWSTR LocaleName, PWSTR, DWORD, PDWORD)
/// Vitreo has no driver store, so no INF is in it. s7_setupapi_probe
/// sgidsl_*: NULL name 87; a full path not in the store ERROR_NOT_FOUND; a
/// bare name not in %windir%\INF ERROR_FILE_NOT_FOUND.
void inf_driver_store_location(Marshaler& m, const StatePtr& state) {
    const GuestAddr name_ptr = m.arg_ptr(0);
    for (usize i = 1; i < 6; ++i) (void)m.arg(i);
    const auto name = read_guest_string(m, name_ptr, true);
    if (!name) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
    if (name->find_first_of("\\/") == std::string::npos) {
        const bool present = state->vfs != nullptr &&
                             state->vfs->stat("C:\\Windows\\INF\\" + *name).is_ok();
        if (!present) return fail(m, error_codes::ERROR_FILE_NOT_FOUND);
    }
    fail(m, error_codes::ERROR_NOT_FOUND);
}

} // namespace vitreo::winapi::setupapi_internal
