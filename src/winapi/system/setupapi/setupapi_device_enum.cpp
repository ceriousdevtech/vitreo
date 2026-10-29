// SPDX-License-Identifier: LGPL-2.1-only

#include "winapi/system/setupapi/setupapi_internal.hpp"

namespace vitreo::winapi::setupapi_internal {

// ---- SetupDiGetClassDevs[Ex]{A,W} -----------------------------------------------

/// HDEVINFO SetupDiGetClassDevs[Ex]{A,W}(const GUID*, PCTSTR Enumerator, HWND,
///     DWORD Flags [, HDEVINFO DeviceInfoSet, PCTSTR MachineName, PVOID])
/// s7_setupapi_probe gcd_*: a NULL class without DIGCF_ALLCLASSES is
/// ERROR_INVALID_PARAMETER; an empty Enumerator, or with
/// DIGCF_DEVICEINTERFACE one that is not a device instance ID, is
/// ERROR_INVALID_DATA; an unknown enumerator/instance/class gives a valid
/// empty set; unknown flag bits are ignored. Ex: a remote machine is
/// ERROR_CALL_NOT_IMPLEMENTED, Reserved must be NULL, an existing set is
/// extended and returned (its class must match: ERROR_CLASS_MISMATCH).
void get_class_devs(Marshaler& m, const StatePtr& state, bool wide, bool ex) {
    const GuestAddr guid_ptr = m.arg_ptr(0);
    const GuestAddr enumerator_ptr = m.arg_ptr(1);
    (void)m.arg_ptr(2);  // hwndParent
    const u32 flags = m.arg32(3);
    const u64 invalid = invalid_handle_value(m);
    const u64 existing = ex ? m.arg(4) : 0;
    if (ex) {
        const GuestAddr machine = m.arg_ptr(5);
        const GuestAddr reserved = m.arg_ptr(6);
        if (reserved != 0) return fail(m, error_codes::ERROR_INVALID_PARAMETER, invalid);
        if (machine != 0) {
            const auto name = read_guest_string(m, machine, wide);
            if (!name || !name->empty()) {
                // Remote enumeration was removed from SetupAPI in Windows 8
                // (s7_setupapi_probe gcdex_remote).
                return fail(m, error_codes::ERROR_CALL_NOT_IMPLEMENTED, invalid);
            }
        }
    }
    const bool all_classes = (flags & DIGCF_ALLCLASSES) != 0;
    const bool interfaces = (flags & DIGCF_DEVICEINTERFACE) != 0;
    std::optional<Guid> guid;
    if (!all_classes) {
        guid = read_guid(m, guid_ptr);
        if (!guid) return fail(m, error_codes::ERROR_INVALID_PARAMETER, invalid);
    }
    std::optional<std::string> enumerator;
    if (enumerator_ptr != 0) {
        enumerator = read_guest_string(m, enumerator_ptr, wide);
        if (!enumerator || enumerator->empty() ||
            (interfaces && !valid_instance_id(*enumerator))) {
            return fail(m, error_codes::ERROR_INVALID_DATA, invalid);
        }
    }

    std::lock_guard lock(state->mutex);
    u64 handle = 0;
    DevInfoSet* set = nullptr;
    if (existing != 0) {
        set = state->find_set(existing);
        if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE, invalid);
        if (!interfaces && !all_classes && set->has_class && set->class_guid != *guid) {
            return fail(m, ERROR_CLASS_MISMATCH, invalid);
        }
        handle = existing;
    } else {
        handle = state->create_set();
        set = state->find_set(handle);
        if (!interfaces && !all_classes) {
            set->has_class = true;
            set->class_guid = *guid;
        }
    }
    const auto nodes = state->nodes();
    for (const NodeRecord& node : nodes) {
        const Guid node_class = node.class_guid.value_or(GUID_ZERO);
        if (set->has_class && node_class != set->class_guid) continue;
        if (interfaces) {
            if (enumerator && !iequal(node.instance_id, *enumerator)) continue;
            for (const Guid& g : node.interfaces) {
                if (!all_classes && g != *guid) continue;
                const usize element = add_node_element(*set, node);
                (void)add_interface(*set, element, g, node.instance_id);
            }
        } else {
            if (!all_classes && node_class != *guid) continue;
            if (enumerator && !iequal(first_component(node.instance_id), *enumerator)) continue;
            (void)add_node_element(*set, node);
        }
    }
    succeed(m, handle);
}

/// BOOL SetupDiEnumDeviceInfo(HDEVINFO, DWORD MemberIndex, PSP_DEVINFO_DATA)
/// s7_setupapi_probe: bad set 6; the index is checked first (259 even with a
/// NULL or mis-sized output); NULL output 87; wrong cbSize 1784.
void enum_device_info(Marshaler& m, const StatePtr& state) {
    const u64 handle = m.arg(0);
    const u32 index = m.arg32(1);
    const GuestAddr data = m.arg_ptr(2);
    std::lock_guard lock(state->mutex);
    DevInfoSet* set = state->find_set(handle);
    if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE);
    if (index >= set->elements.size()) return fail(m, error_codes::ERROR_NO_MORE_ITEMS);
    if (data == 0) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
    const auto cb = m.read_u32(data);
    if (cb.is_err() || cb.value() != layout(m).devinfo) return fail(m, ERROR_INVALID_USER_BUFFER);
    if (!write_devinfo(m, data, set->elements[index])) return fail(m, ERROR_INVALID_USER_BUFFER);
    succeed(m);
}

/// BOOL SetupDiEnumDeviceInterfaces(HDEVINFO, PSP_DEVINFO_DATA, const GUID*,
///     DWORD MemberIndex, PSP_DEVICE_INTERFACE_DATA)
/// s7_setupapi_probe enumif_* / disp_iface: bad set 6; a DeviceInfoData that
/// is mis-sized or not from this set 87; NULL class GUID 87 (before the
/// index); past the last match 259 (before the output checks); NULL output
/// 87; wrong cbSize 1784. Only the set's interfaces are enumerated — a
/// setup-class set has none (disp_class enumif 259).
void enum_device_interfaces(Marshaler& m, const StatePtr& state) {
    const u64 handle = m.arg(0);
    const GuestAddr did = m.arg_ptr(1);
    const GuestAddr guid_ptr = m.arg_ptr(2);
    const u32 index = m.arg32(3);
    const GuestAddr out = m.arg_ptr(4);
    std::lock_guard lock(state->mutex);
    DevInfoSet* set = state->find_set(handle);
    if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE);
    std::optional<usize> element;
    if (did != 0) {
        element = resolve_element(m, *set, did);
        if (!element) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
    }
    const auto guid = read_guid(m, guid_ptr);
    if (!guid) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
    u32 seen = 0;
    const SetInterface* match = nullptr;
    for (const SetInterface& iface : set->interfaces) {
        if (iface.guid != *guid) continue;
        if (element && iface.element != *element) continue;
        if (seen++ == index) {
            match = &iface;
            break;
        }
    }
    if (match == nullptr) return fail(m, error_codes::ERROR_NO_MORE_ITEMS);
    if (out == 0) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
    const auto cb = m.read_u32(out);
    if (cb.is_err() || cb.value() != layout(m).iface) return fail(m, ERROR_INVALID_USER_BUFFER);
    if (!write_interface_data(m, out, *match)) return fail(m, ERROR_INVALID_USER_BUFFER);
    succeed(m);
}

/// BOOL SetupDiGetDeviceInterfaceDetail{A,W}(HDEVINFO,
///     PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA,
///     DWORD DetailSize, PDWORD RequiredSize, PSP_DEVINFO_DATA)
/// s7_setupapi_probe detw_* / deta_*: bad set 6; interface data NULL,
/// mis-sized or not from this set 87. The detail buffer: NULL needs size 0,
/// otherwise size >= offsetof(DevicePath)+sizeof(TCHAR) and cbSize ==
/// sizeof (W 8/6, A 8/5 on x64/x86), else 1784 with RequiredSize untouched.
/// Then RequiredSize = 4 + (path+1)*sizeof(TCHAR); the path is written when
/// it fits; DeviceInfoData is filled (1784 if mis-sized, after the path was
/// written); a NULL or short buffer finally fails 122.
void get_device_interface_detail(Marshaler& m, const StatePtr& state, bool wide) {
    const u64 handle = m.arg(0);
    const GuestAddr iface_ptr = m.arg_ptr(1);
    const GuestAddr detail = m.arg_ptr(2);
    const u32 detail_size = m.arg32(3);
    const GuestAddr required_out = m.arg_ptr(4);
    const GuestAddr did = m.arg_ptr(5);
    const Layout lay = layout(m);
    std::lock_guard lock(state->mutex);
    DevInfoSet* set = state->find_set(handle);
    if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE);
    const auto iface_index = resolve_interface(m, *set, iface_ptr);
    if (!iface_index) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
    const u32 char_size = wide ? 2u : 1u;
    const u32 fixed_cb = lay.is64 ? 8u : (wide ? 6u : 5u);
    if (detail == 0) {
        if (detail_size != 0) return fail(m, ERROR_INVALID_USER_BUFFER);
    } else {
        const auto cb = m.read_u32(detail);
        if (detail_size < 4 + char_size || cb.is_err() || cb.value() != fixed_cb) {
            return fail(m, ERROR_INVALID_USER_BUFFER);
        }
    }
    const SetInterface& iface = set->interfaces[*iface_index];
    std::vector<u8> path_bytes;
    if (wide) {
        path_bytes = utf16_bytes(iface.path);
    } else {
        path_bytes.assign(iface.path.begin(), iface.path.end());
        path_bytes.push_back(0);
    }
    const u32 required = 4 + static_cast<u32>(path_bytes.size());
    if (required_out != 0) (void)m.write_u32(required_out, required);
    const bool fits = detail != 0 && detail_size >= required;
    if (fits && m.memory().write_bytes(detail + 4, path_bytes.data(),
                                       path_bytes.size()).is_err()) {
        return fail(m, ERROR_INVALID_USER_BUFFER);
    }
    if (did != 0) {
        const auto cb = m.read_u32(did);
        if (cb.is_err() || cb.value() != lay.devinfo ||
            !write_devinfo(m, did, set->elements[iface.element])) {
            return fail(m, ERROR_INVALID_USER_BUFFER);
        }
    }
    if (!fits) return fail(m, error_codes::ERROR_INSUFFICIENT_BUFFER);
    succeed(m);
}

/// BOOL SetupDiDestroyDeviceInfoList(HDEVINFO): unknown handle 6
/// (destroy2/destroy_null/destroy_inv); success sets ERROR_SUCCESS.
void destroy_device_info_list(Marshaler& m, const StatePtr& state) {
    const u64 handle = m.arg(0);
    std::lock_guard lock(state->mutex);
    if (state->sets.erase(handle) == 0) return fail(m, error_codes::ERROR_INVALID_HANDLE);
    succeed(m);
}

/// HDEVINFO SetupDiCreateDeviceInfoList[Ex{A,W}](const GUID* ClassGuid, HWND
///     [, PCTSTR MachineName, PVOID Reserved])
/// An empty set, with the class when one is given (cdil, cdil_class; the
/// window is not validated: cdil_badhwnd). Ex: a remote machine is
/// ERROR_CALL_NOT_IMPLEMENTED (cdil_remote), Reserved must be NULL.
void create_device_info_list(Marshaler& m, const StatePtr& state, bool ex, bool wide) {
    const GuestAddr guid_ptr = m.arg_ptr(0);
    (void)m.arg_ptr(1);
    const u64 invalid = invalid_handle_value(m);
    if (ex) {
        const GuestAddr machine = m.arg_ptr(2);
        if (m.arg_ptr(3) != 0) return fail(m, error_codes::ERROR_INVALID_PARAMETER, invalid);
        if (machine != 0) {
            const auto name = read_guest_string(m, machine, wide);
            if (!name || !name->empty()) {
                return fail(m, error_codes::ERROR_CALL_NOT_IMPLEMENTED, invalid);
            }
        }
    }
    std::optional<Guid> guid;
    if (guid_ptr != 0) {
        guid = read_guid(m, guid_ptr);
        if (!guid) return fail(m, error_codes::ERROR_INVALID_PARAMETER, invalid);
    }
    std::lock_guard lock(state->mutex);
    const u64 handle = state->create_set();
    DevInfoSet* set = state->find_set(handle);
    if (guid && *guid != GUID_ZERO) {
        set->has_class = true;
        set->class_guid = *guid;
    }
    succeed(m, handle);
}

/// BOOL SetupDiOpenDeviceInfoW(HDEVINFO, PCWSTR DeviceInstanceId, HWND,
///     DWORD OpenFlags, PSP_DEVINFO_DATA)
/// s7_setupapi_probe odi_*: bad set 6; flags other than
/// DIOD_INHERIT_CLASSDRVS|DIOD_CANCEL_REMOVE 1004; NULL or "" opens the root;
/// a malformed ID 0xE0000205; an unknown one 0xE000020B; a set of another
/// class 0xE0000201; the element is added once (case-insensitive) and a
/// mis-sized DeviceInfoData then fails 1784 with the element still added.
void open_device_info(Marshaler& m, const StatePtr& state, bool wide) {
    const u64 handle = m.arg(0);
    const GuestAddr id_ptr = m.arg_ptr(1);
    (void)m.arg_ptr(2);
    const u32 flags = m.arg32(3);
    const GuestAddr did = m.arg_ptr(4);
    std::lock_guard lock(state->mutex);
    DevInfoSet* set = state->find_set(handle);
    if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE);
    if ((flags & ~(DIOD_INHERIT_CLASSDRVS | DIOD_CANCEL_REMOVE)) != 0) {
        return fail(m, error_codes::ERROR_INVALID_FLAGS);
    }
    std::string id = ROOT_INSTANCE_ID;
    if (id_ptr != 0) {
        const auto text = read_guest_string(m, id_ptr, wide);
        if (!text) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
        if (!text->empty()) id = *text;
    }
    if (!valid_instance_id(id)) return fail(m, ERROR_INVALID_DEVINST_NAME);
    const auto nodes = state->nodes();
    const NodeRecord* node = find_node(nodes, id);
    if (node == nullptr) return fail(m, ERROR_NO_SUCH_DEVINST);
    if (set->has_class && node->class_guid.value_or(GUID_ZERO) != set->class_guid) {
        return fail(m, ERROR_CLASS_MISMATCH);
    }
    const usize element = add_node_element(*set, *node);
    if ((flags & DIOD_INHERIT_CLASSDRVS) != 0 && set->class_drivers) {
        set->elements[element].class_drivers = set->class_drivers;
    }
    if (did != 0) {
        const auto cb = m.read_u32(did);
        if (cb.is_err() || cb.value() != layout(m).devinfo ||
            !write_devinfo(m, did, set->elements[element])) {
            return fail(m, ERROR_INVALID_USER_BUFFER);
        }
    }
    succeed(m);
}

/// BOOL SetupDiOpenDeviceInterfaceW(HDEVINFO, PCWSTR DevicePath,
///     DWORD OpenFlags, PSP_DEVICE_INTERFACE_DATA)
/// s7_setupapi_probe openif_*: bad set 6; NULL path 87; flags other than
/// DIODI_NO_ADD 1004; unknown path 0xE0000225 (case-insensitive match); a
/// set of another class 0xE0000201; with DIODI_NO_ADD an interface not
/// already in the set is 0xE0000225; mis-sized output 1784.
void open_device_interface(Marshaler& m, const StatePtr& state, bool wide) {
    const u64 handle = m.arg(0);
    const GuestAddr path_ptr = m.arg_ptr(1);
    const u32 flags = m.arg32(2);
    const GuestAddr out = m.arg_ptr(3);
    std::lock_guard lock(state->mutex);
    DevInfoSet* set = state->find_set(handle);
    if (set == nullptr) return fail(m, error_codes::ERROR_INVALID_HANDLE);
    const auto path = read_guest_string(m, path_ptr, wide);
    if (!path) return fail(m, error_codes::ERROR_INVALID_PARAMETER);
    if ((flags & ~DIODI_NO_ADD) != 0) return fail(m, error_codes::ERROR_INVALID_FLAGS);
    const auto nodes = state->nodes();
    const NodeRecord* node = nullptr;
    Guid guid{};
    for (const auto& candidate : nodes) {
        for (const Guid& g : candidate.interfaces) {
            if (iequal(interface_path(candidate.instance_id, g, true), *path)) {
                node = &candidate;
                guid = g;
            }
        }
    }
    if (node == nullptr) return fail(m, ERROR_NO_SUCH_DEVICE_INTERFACE);
    if (set->has_class && node->class_guid.value_or(GUID_ZERO) != set->class_guid) {
        return fail(m, ERROR_CLASS_MISMATCH);
    }
    std::optional<usize> index;
    for (usize i = 0; i < set->interfaces.size(); ++i) {
        const SetInterface& iface = set->interfaces[i];
        if (iface.guid == guid && iequal(set->elements[iface.element].instance_id,
                                         node->instance_id)) {
            index = i;
        }
    }
    if (!index) {
        if ((flags & DIODI_NO_ADD) != 0) return fail(m, ERROR_NO_SUCH_DEVICE_INTERFACE);
        const usize element = add_node_element(*set, *node);
        index = add_interface(*set, element, guid, node->instance_id);
    }
    if (out != 0) {
        const auto cb = m.read_u32(out);
        if (cb.is_err() || cb.value() != layout(m).iface ||
            !write_interface_data(m, out, set->interfaces[*index])) {
            return fail(m, ERROR_INVALID_USER_BUFFER);
        }
    }
    succeed(m);
}

} // namespace vitreo::winapi::setupapi_internal
