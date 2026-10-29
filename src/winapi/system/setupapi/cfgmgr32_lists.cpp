// SPDX-License-Identifier: LGPL-2.1-only

#include "winapi/system/setupapi/cfgmgr32_internal.hpp"

#include <algorithm>
#include <variant>

namespace vitreo::winapi::setupapi_internal {

namespace {

constexpr u32 CM_GETIDLIST_FILTER_ENUMERATOR = 0x00000001;
constexpr u32 CM_GETIDLIST_FILTER_SERVICE = 0x00000002;
constexpr u32 CM_GETIDLIST_FILTER_EJECTRELATIONS = 0x00000004;
constexpr u32 CM_GETIDLIST_FILTER_REMOVALRELATIONS = 0x00000008;
constexpr u32 CM_GETIDLIST_FILTER_POWERRELATIONS = 0x00000010;
constexpr u32 CM_GETIDLIST_FILTER_BUSRELATIONS = 0x00000020;
constexpr u32 CM_GETIDLIST_FILTER_TRANSPORTRELATIONS = 0x00000080;
constexpr u32 CM_GETIDLIST_FILTER_CLASS = 0x00000200;
constexpr u32 CM_GETIDLIST_FILTER_BITS = 0x100003FF;

constexpr u32 CM_GET_DEVICE_INTERFACE_LIST_BITS = 0x00000001;

/// The CM_Get_Device_ID_List filter: the IDs selected, or a CONFIGRET.
/// s7_cfgmgr_probe idl_*: NONE/PRESENT lists every devnode; ENUMERATOR takes
/// "Enumerator" or "Enumerator\DeviceID" (a full instance ID is
/// CR_NO_SUCH_VALUE, NULL CR_INVALID_POINTER); CLASS takes a GUID string
/// (CR_INVALID_DATA otherwise, and it wins over ENUMERATOR); SERVICE matches
/// nothing (no service is modelled); the relations take an instance ID and
/// only bus relations exist here (CR_NO_SUCH_VALUE otherwise).
[[nodiscard]] std::variant<std::vector<std::string>, u32> device_id_list(
    const std::vector<NodeRecord>& nodes, const std::optional<std::string>& filter,
    u32 flags) {
    std::vector<std::string> ids;
    if ((flags & CM_GETIDLIST_FILTER_CLASS) != 0) {
        if (!filter) return CR_INVALID_POINTER;
        const auto guid = parse_guid_string(*filter);
        if (!guid) return CR_INVALID_DATA;
        for (const auto& node : nodes) {
            if (node.class_guid && *node.class_guid == *guid) ids.push_back(node.instance_id);
        }
        return ids;
    }
    constexpr u32 kRelations = CM_GETIDLIST_FILTER_EJECTRELATIONS |
        CM_GETIDLIST_FILTER_REMOVALRELATIONS | CM_GETIDLIST_FILTER_POWERRELATIONS |
        CM_GETIDLIST_FILTER_BUSRELATIONS | CM_GETIDLIST_FILTER_TRANSPORTRELATIONS;
    if ((flags & CM_GETIDLIST_FILTER_ENUMERATOR) != 0) {
        if (!filter) return CR_INVALID_POINTER;
        const usize slashes = static_cast<usize>(std::count(filter->begin(), filter->end(), '\\'));
        if (filter->empty() || slashes > 1) return CR_NO_SUCH_VALUE;
        for (const auto& node : nodes) {
            const std::string_view id = node.instance_id;
            const std::string prefix = slashes == 0 ? first_component(id)
                                                    : std::string(id.substr(0, id.rfind('\\')));
            if (iequal(prefix, *filter)) ids.push_back(node.instance_id);
        }
        return ids;
    }
    if ((flags & CM_GETIDLIST_FILTER_SERVICE) != 0) {
        if (!filter) return CR_INVALID_POINTER;
        return ids;
    }
    if ((flags & kRelations) != 0) {
        if (!filter) return CR_INVALID_POINTER;
        const NodeRecord* node = find_node(nodes, *filter);
        if (node == nullptr || (flags & kRelations) != CM_GETIDLIST_FILTER_BUSRELATIONS) {
            return CR_NO_SUCH_VALUE;
        }
        for (const auto& other : nodes) {
            if (iequal(other.parent, node->instance_id)) ids.push_back(other.instance_id);
        }
        if (ids.empty()) return CR_NO_SUCH_VALUE;
        return ids;
    }
    for (const auto& node : nodes) ids.push_back(node.instance_id);
    return ids;
}

[[nodiscard]] u32 multi_sz_chars(const std::vector<std::string>& ids) {
    u32 chars = 1;
    for (const auto& id : ids) chars += static_cast<u32>(id.size()) + 1;
    return chars;
}

[[nodiscard]] std::vector<u8> multi_sz_bytes(const std::vector<std::string>& items, bool wide) {
    if (wide) return utf16_multi_bytes(items);
    std::vector<u8> bytes;
    for (const auto& item : items) {
        bytes.insert(bytes.end(), item.begin(), item.end());
        bytes.push_back(0);
    }
    bytes.push_back(0);
    return bytes;
}

/// Write a MULTI_SZ list; a short buffer gets an empty string and
/// CR_BUFFER_SMALL (s7_cfgmgr_probe idl_htree_small_written, ifl_disp_small).
void write_multi_sz(Marshaler& m, GuestAddr buffer, u32 length_chars,
                    const std::vector<std::string>& items, bool wide) {
    const auto bytes = multi_sz_bytes(items, wide);
    const u32 chars = static_cast<u32>(bytes.size() / (wide ? 2 : 1));
    if (length_chars < chars) {
        (void)(wide ? m.write_u16(buffer, 0) : m.memory().write_u8(buffer, 0));
        return m.set_return(CR_BUFFER_SMALL);
    }
    if (m.memory().write_bytes(buffer, bytes.data(), bytes.size()).is_err()) {
        return m.set_return(CR_INVALID_POINTER);
    }
    m.set_return(CR_SUCCESS);
}

/// Interfaces of a class, optionally of one device, in CM's path form.
[[nodiscard]] std::vector<std::string> interface_list(const std::vector<NodeRecord>& nodes,
                                                      const Guid& guid,
                                                      const std::optional<std::string>& device) {
    std::vector<std::string> paths;
    for (const auto& node : nodes) {
        if (device && !iequal(node.instance_id, *device)) continue;
        for (const Guid& g : node.interfaces) {
            if (g == guid) paths.push_back(interface_path(node.instance_id, g, false));
        }
    }
    return paths;
}

} // namespace

/// CONFIGRET CM_Get_Device_ID_List_Size{A,W}(PULONG, PCTSTR Filter, ULONG):
/// characters including the final NUL (1 for an empty list). A failing
/// filter writes 0 first; bad flags leave the output untouched.
void cm_get_device_id_list_size(Marshaler& m, const CfgPtr& state, bool wide) {
    const GuestAddr out = m.arg_ptr(0);
    const GuestAddr filter_ptr = m.arg_ptr(1);
    const u32 flags = m.arg32(2);
    if (out == 0) return m.set_return(CR_INVALID_POINTER);
    if ((flags & ~CM_GETIDLIST_FILTER_BITS) != 0) return m.set_return(CR_INVALID_FLAG);
    const auto filter = read_guest_string(m, filter_ptr, wide);
    const auto result = device_id_list(state->nodes(), filter, flags);
    if (const u32* cr = std::get_if<u32>(&result)) {
        (void)m.write_u32(out, 0);
        return m.set_return(*cr);
    }
    (void)m.write_u32(out, multi_sz_chars(std::get<std::vector<std::string>>(result)));
    m.set_return(CR_SUCCESS);
}

/// CONFIGRET CM_Get_Device_ID_List{A,W}(PCTSTR Filter, PTCHAR Buffer,
///     ULONG BufferLen, ULONG): NULL buffer or zero length CR_INVALID_POINTER.
void cm_get_device_id_list(Marshaler& m, const CfgPtr& state, bool wide) {
    const GuestAddr filter_ptr = m.arg_ptr(0);
    const GuestAddr buffer = m.arg_ptr(1);
    const u32 length = m.arg32(2);
    const u32 flags = m.arg32(3);
    if (buffer == 0 || length == 0) return m.set_return(CR_INVALID_POINTER);
    if ((flags & ~CM_GETIDLIST_FILTER_BITS) != 0) return m.set_return(CR_INVALID_FLAG);
    const auto filter = read_guest_string(m, filter_ptr, wide);
    const auto result = device_id_list(state->nodes(), filter, flags);
    if (const u32* cr = std::get_if<u32>(&result)) return m.set_return(*cr);
    write_multi_sz(m, buffer, length, std::get<std::vector<std::string>>(result), wide);
}

/// CONFIGRET CM_Get_Device_Interface_List_Size{A,W}(PULONG, LPGUID,
///     DEVINSTID, ULONG) and CM_Get_Device_Interface_List{A,W}(LPGUID,
///     DEVINSTID, PZZTSTR, ULONG, ULONG). s7_cfgmgr_probe ifl_*: flags outside
///     CM_GET_DEVICE_INTERFACE_LIST_BITS CR_INVALID_FLAG; NULL output
///     CR_INVALID_POINTER; NULL class GUID CR_FAILURE; an unknown device
///     gives an empty list.
void cm_get_interface_list_size(Marshaler& m, const CfgPtr& state, bool wide) {
    const GuestAddr out = m.arg_ptr(0);
    const GuestAddr guid_ptr = m.arg_ptr(1);
    const GuestAddr device_ptr = m.arg_ptr(2);
    const u32 flags = m.arg32(3);
    if ((flags & ~CM_GET_DEVICE_INTERFACE_LIST_BITS) != 0) return m.set_return(CR_INVALID_FLAG);
    if (out == 0) return m.set_return(CR_INVALID_POINTER);
    const auto guid = read_guid(m, guid_ptr);
    if (!guid) return m.set_return(CR_FAILURE);
    const auto device = read_guest_string(m, device_ptr, wide);
    (void)m.write_u32(out, multi_sz_chars(interface_list(state->nodes(), *guid, device)));
    m.set_return(CR_SUCCESS);
}

void cm_get_interface_list(Marshaler& m, const CfgPtr& state, bool wide) {
    const GuestAddr guid_ptr = m.arg_ptr(0);
    const GuestAddr device_ptr = m.arg_ptr(1);
    const GuestAddr buffer = m.arg_ptr(2);
    const u32 length = m.arg32(3);
    const u32 flags = m.arg32(4);
    if ((flags & ~CM_GET_DEVICE_INTERFACE_LIST_BITS) != 0) return m.set_return(CR_INVALID_FLAG);
    if (buffer == 0 || length == 0) return m.set_return(CR_INVALID_POINTER);
    const auto guid = read_guid(m, guid_ptr);
    if (!guid) return m.set_return(CR_FAILURE);
    const auto device = read_guest_string(m, device_ptr, wide);
    write_multi_sz(m, buffer, length, interface_list(state->nodes(), *guid, device), wide);
}

} // namespace vitreo::winapi::setupapi_internal
