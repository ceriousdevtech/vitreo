// SPDX-License-Identifier: LGPL-2.1-only

#include "winapi/system/setupapi/setupapi_internal.hpp"

namespace vitreo::winapi::setupapi_internal {

// ---- properties -----------------------------------------------------------------

/// BOOL SetupDiGetDeviceRegistryProperty{A,W}(HDEVINFO, PSP_DEVINFO_DATA,
///     DWORD Property, PDWORD Type, PBYTE Buffer, DWORD Size, PDWORD Required)
/// s7_setupapi_probe gdrp_*: bad set 6; bad DeviceInfoData 87; an ordinal
/// >= SPDRP_MAXIMUM_PROPERTY 0xE0000209; NULL buffer with a size 1784; a
/// property the device does not have ERROR_INVALID_DATA with the outputs
/// untouched; otherwise Type and RequiredSize are written and a short buffer
/// fails 122.
void get_registry_property(Marshaler& m, const StatePtr& state, bool wide) {
    const u64 handle = m.arg(0);
    const GuestAddr did = m.arg_ptr(1);
    const u32 property = m.arg32(2);
    const GuestAddr type_out = m.arg_ptr(3);
    const GuestAddr buffer = m.arg_ptr(4);
    const u32 capacity = m.arg32(5);
    const GuestAddr required_out = m.arg_ptr(6);
    std::lock_guard lock(state->mutex);
    DevInfoSet* set = state->find_set(handle);
    if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE);
    const auto element = resolve_element(m, *set, did);
    if (!element) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
    if (property >= SPDRP_MAXIMUM_PROPERTY) return fail(m, ERROR_INVALID_REG_PROPERTY);
    if (buffer == 0 && capacity != 0) return fail(m, ERROR_INVALID_USER_BUFFER);
    const auto nodes = state->nodes();
    const auto prop = spdrp_property(property);
    const auto value = prop ? element_property(nodes, set->elements[*element], *prop)
                            : std::nullopt;
    if (!value) return fail(m, error_codes::ERROR_INVALID_DATA);
    const auto [type, bytes] = registry_encoding(*value, wide);
    if (type_out != 0) (void)m.write_u32(type_out, type);
    if (required_out != 0) (void)m.write_u32(required_out, static_cast<u32>(bytes.size()));
    if (capacity < bytes.size()) return fail(m, error_codes::ERROR_INSUFFICIENT_BUFFER);
    if (m.memory().write_bytes(buffer, bytes.data(), bytes.size()).is_err()) {
        return fail(m, ERROR_INVALID_USER_BUFFER);
    }
    succeed(m);
}

/// BOOL SetupDiGetDevicePropertyW(HDEVINFO, PSP_DEVINFO_DATA, const
///     DEVPROPKEY*, DEVPROPTYPE*, PBYTE, DWORD Size, PDWORD Required, DWORD Flags)
/// s7_setupapi_probe gdp_*: bad set 6; bad DeviceInfoData 87; Flags != 0
/// 1004 (checked before the key); NULL key ERROR_INVALID_DATA; NULL type or a
/// NULL buffer with a size 1784; a missing property ERROR_NOT_FOUND with the
/// outputs untouched; otherwise type/size written and a short buffer 122.
void get_device_property(Marshaler& m, const StatePtr& state) {
    const u64 handle = m.arg(0);
    const GuestAddr did = m.arg_ptr(1);
    const GuestAddr key = m.arg_ptr(2);
    const GuestAddr type_out = m.arg_ptr(3);
    const GuestAddr buffer = m.arg_ptr(4);
    const u32 capacity = m.arg32(5);
    const GuestAddr required_out = m.arg_ptr(6);
    const u32 flags = m.arg32(7);
    std::lock_guard lock(state->mutex);
    DevInfoSet* set = state->find_set(handle);
    if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE);
    const auto element = resolve_element(m, *set, did);
    if (!element) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
    if (flags != 0) return fail(m, error_codes::ERROR_INVALID_FLAGS);
    const auto fmtid = read_guid(m, key);
    const auto pid = key != 0 ? m.read_u32(key + 16) : VResult<u32>::err(Error::InvalidArgument);
    if (!fmtid || pid.is_err()) return fail(m, error_codes::ERROR_INVALID_DATA);
    if (type_out == 0 || (buffer == 0 && capacity != 0)) return fail(m, ERROR_INVALID_USER_BUFFER);
    const auto nodes = state->nodes();
    const auto prop = devpkey_property(*fmtid, pid.value());
    const auto value = prop ? element_property(nodes, set->elements[*element], *prop)
                            : std::nullopt;
    if (!value) return fail(m, error_codes::ERROR_NOT_FOUND);
    const auto [type, bytes] = devprop_encoding(*value);
    (void)m.write_u32(type_out, type);
    if (required_out != 0) (void)m.write_u32(required_out, static_cast<u32>(bytes.size()));
    if (capacity < bytes.size()) return fail(m, error_codes::ERROR_INSUFFICIENT_BUFFER);
    if (m.memory().write_bytes(buffer, bytes.data(), bytes.size()).is_err()) {
        return fail(m, ERROR_INVALID_USER_BUFFER);
    }
    succeed(m);
}

/// BOOL SetupDiGetDeviceInstanceId{A,W}(HDEVINFO, PSP_DEVINFO_DATA, PTSTR,
///     DWORD Size, PDWORD Required) — sizes in characters with the NUL.
/// s7_setupapi_probe gdii*: bad set 6; bad DeviceInfoData 87; RequiredSize is
/// always written; a short (or NULL) buffer 122.
void get_instance_id(Marshaler& m, const StatePtr& state, bool wide) {
    const u64 handle = m.arg(0);
    const GuestAddr did = m.arg_ptr(1);
    const GuestAddr buffer = m.arg_ptr(2);
    const u32 capacity = m.arg32(3);
    const GuestAddr required_out = m.arg_ptr(4);
    std::lock_guard lock(state->mutex);
    DevInfoSet* set = state->find_set(handle);
    if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE);
    const auto element = resolve_element(m, *set, did);
    if (!element) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
    const std::string& id = set->elements[*element].instance_id;
    const u32 needed = static_cast<u32>(id.size()) + 1;
    if (required_out != 0) (void)m.write_u32(required_out, needed);
    if (capacity < needed) return fail(m, error_codes::ERROR_INSUFFICIENT_BUFFER);
    if (buffer == 0) return fail(m, ERROR_INVALID_USER_BUFFER);
    std::vector<u8> bytes;
    if (wide) {
        bytes = utf16_bytes(id);
    } else {
        bytes.assign(id.begin(), id.end());
        bytes.push_back(0);
    }
    if (m.memory().write_bytes(buffer, bytes.data(), bytes.size()).is_err()) {
        return fail(m, ERROR_INVALID_USER_BUFFER);
    }
    succeed(m);
}

// ---- registry keys ---------------------------------------------------------------

/// HKEY SetupDiOpenDevRegKey(HDEVINFO, PSP_DEVINFO_DATA, DWORD Scope,
///     DWORD HwProfile, DWORD KeyType, REGSAM)
/// s7_setupapi_probe odrk_*: bad set 6; NULL/bad DeviceInfoData 87; a Scope
/// other than GLOBAL/CONFIGSPECIFIC or a KeyType other than DEV/DRV
/// (DIREG_BOTH included) 1004; a key the device does not have 0xE0000204;
/// write access without elevation ERROR_ACCESS_DENIED. Failures return
/// pointer-width INVALID_HANDLE_VALUE.
void open_dev_reg_key(Marshaler& m, const StatePtr& state) {
    const u64 handle = m.arg(0);
    const GuestAddr did = m.arg_ptr(1);
    const u32 scope = m.arg32(2);
    (void)m.arg32(3);  // HwProfile
    const u32 key_type = m.arg32(4);
    const u32 sam = m.arg32(5);
    const u64 invalid = invalid_handle_value(m);
    std::lock_guard lock(state->mutex);
    DevInfoSet* set = state->find_set(handle);
    if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE, invalid);
    const auto element = resolve_element(m, *set, did);
    if (!element) return fail(m, error_codes::ERROR_INVALID_PARAMETER, invalid);
    if ((scope != DICS_FLAG_GLOBAL && scope != DICS_FLAG_CONFIGSPECIFIC) ||
        (key_type != DIREG_DEV && key_type != DIREG_DRV)) {
        return fail(m, error_codes::ERROR_INVALID_FLAGS, invalid);
    }
    const auto nodes = state->nodes();
    const NodeRecord* node = element_node(nodes, set->elements[*element]);
    if (node == nullptr || scope == DICS_FLAG_CONFIGSPECIFIC) {
        // Created elements have no keys until registered; Vitreo has a single
        // hardware profile and keeps no per-profile device keys.
        return fail(m, ERROR_KEY_DOES_NOT_EXIST, invalid);
    }
    std::string path;
    if (key_type == DIREG_DEV) {
        if (node->parent.empty()) return fail(m, ERROR_KEY_DOES_NOT_EXIST, invalid);
        ensure_device_hardware_key(state->registry, *node);
        path = std::string(ENUM_KEY) + "\\" + node->instance_id + "\\Device Parameters";
    } else {
        if (node->driver.empty()) return fail(m, ERROR_KEY_DOES_NOT_EXIST, invalid);
        path = std::string(CLASS_KEY) + "\\" + node->driver;
    }
    if (!registry_key_exists(state->registry, path)) {
        return fail(m, ERROR_KEY_DOES_NOT_EXIST, invalid);
    }
    if (wants_write_access(sam) && !state->elevated()) {
        return fail(m, error_codes::ERROR_ACCESS_DENIED, invalid);
    }
    const u64 key = open_registry_key(state->registry, path, sam);
    if (key == 0) return fail(m, ERROR_KEY_DOES_NOT_EXIST, invalid);
    succeed(m, key);
}

/// HKEY SetupDiOpenClassRegKey(const GUID* ClassGuid, REGSAM)
/// NULL opens HKLM\SYSTEM\CurrentControlSet\Control\Class; a class that is not
/// installed (GUID_NULL included) fails ERROR_INVALID_CLASS; write access
/// without elevation ERROR_ACCESS_DENIED (s7_setupapi_probe ocrk_*). Never
/// creates a key for a class that is not installed.
void open_class_reg_key(Marshaler& m, const StatePtr& state) {
    const GuestAddr guid_ptr = m.arg_ptr(0);
    const u32 sam = m.arg32(1);
    const u64 invalid = invalid_handle_value(m);
    std::string path = CLASS_KEY;
    if (guid_ptr != 0) {
        const auto guid = read_guid(m, guid_ptr);
        if (!guid) return fail(m, ERROR_INVALID_USER_BUFFER, invalid);
        path += "\\" + guid_string(*guid);
        const SetupClass* installed = find_setup_class(*guid);
        if (installed != nullptr) ensure_class_key(state->registry, *installed);
        if ((installed == nullptr && !registry_key_exists(state->registry, path)) ||
            *guid == GUID_ZERO) {
            return fail(m, ERROR_INVALID_CLASS, invalid);
        }
    }
    if (wants_write_access(sam) && !state->elevated()) {
        return fail(m, error_codes::ERROR_ACCESS_DENIED, invalid);
    }
    const u64 key = open_registry_key(state->registry, path, sam);
    if (key == 0) {
        if (state->registry == nullptr) {
            log_unimplemented_once("setupapi!SetupDiOpenClassRegKey", "no registry attached");
        }
        return fail(m, ERROR_INVALID_CLASS, invalid);
    }
    succeed(m, key);
}

/// HKEY SetupDiOpenDeviceInterfaceRegKey(HDEVINFO, PSP_DEVICE_INTERFACE_DATA,
///     DWORD Reserved, REGSAM): Vitreo's interfaces have no interface keys
/// (setupapi_interface_key_probe: the display interface answers
/// 0xE0000204); bad cbSize/interface or Reserved != 0 is 87, a bad set 6.
void open_interface_reg_key(Marshaler& m, const StatePtr& state) {
    const u64 handle = m.arg(0);
    const GuestAddr iface = m.arg_ptr(1);
    const u32 reserved = m.arg32(2);
    (void)m.arg32(3);
    const u64 invalid = invalid_handle_value(m);
    std::lock_guard lock(state->mutex);
    DevInfoSet* set = state->find_set(handle);
    if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE, invalid);
    if (reserved != 0 || !resolve_interface(m, *set, iface)) {
        return fail(m, error_codes::ERROR_INVALID_PARAMETER, invalid);
    }
    fail(m, ERROR_KEY_DOES_NOT_EXIST, invalid);
}

} // namespace vitreo::winapi::setupapi_internal
