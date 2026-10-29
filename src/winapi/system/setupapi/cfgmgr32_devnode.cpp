// SPDX-License-Identifier: LGPL-2.1-only

#include "winapi/system/setupapi/cfgmgr32_internal.hpp"

#include <algorithm>
#include <cstring>

namespace vitreo::winapi::setupapi_internal {

namespace {

constexpr u32 CM_LOCATE_DEVNODE_BITS = 0x00000007;
constexpr u32 CM_LOCATE_DEVNODE_NOVALIDATION = 0x00000004;

constexpr u32 CM_REGISTRY_SOFTWARE = 0x00000001;
constexpr u32 CM_REGISTRY_BITS = 0x00000301;
constexpr u32 REG_DISPOSITION_OPEN_ALWAYS = 0x00000000;
constexpr u32 REG_DISPOSITION_BITS = 0x00000001;
constexpr u32 CM_DISABLE_BITS = 0x0000000F;   // POLITE..UI_NOT_OK | PERSIST (Win10)
/// CM_DRP_* is SPDRP_* + 1, CM_DRP_MIN 1 .. CM_DRP_MAX 0x25 (SDK cfgmgr32.h).
constexpr u32 CM_DRP_MAX = 0x25;

/// The instance ID a DEVINST names (any handle the process was given).
[[nodiscard]] std::optional<std::string> devinst_id(u32 devinst) {
    return DevInstTable::instance().id_for(devinst);
}

} // namespace

/// CONFIGRET CM_Locate_DevNode{A,W}(PDEVINST, DEVINSTID, ULONG)
/// s7_cfgmgr_probe locate_*: flags outside CM_LOCATE_DEVNODE_BITS
/// CR_INVALID_FLAG (output untouched); NULL output CR_INVALID_POINTER; NULL or
/// "" is the root; a malformed ID CR_INVALID_DEVICE_ID and a missing one
/// CR_NO_SUCH_DEVNODE, both with *pdnDevInst = 0; with NOVALIDATION any
/// well-formed ID gets a handle.
void cm_locate_devnode(Marshaler& m, const CfgPtr& state, bool wide) {
    const GuestAddr out = m.arg_ptr(0);
    const GuestAddr id_ptr = m.arg_ptr(1);
    const u32 flags = m.arg32(2);
    if ((flags & ~CM_LOCATE_DEVNODE_BITS) != 0) return m.set_return(CR_INVALID_FLAG);
    if (out == 0) return m.set_return(CR_INVALID_POINTER);
    std::string id = ROOT_INSTANCE_ID;
    if (id_ptr != 0) {
        const auto text = read_guest_string(m, id_ptr, wide);
        if (!text) return m.set_return(CR_INVALID_POINTER);
        if (!text->empty()) id = *text;
    }
    if (!valid_instance_id(id)) {
        (void)m.write_u32(out, 0);
        return m.set_return(CR_INVALID_DEVICE_ID);
    }
    const auto nodes = state->nodes();
    const NodeRecord* node = find_node(nodes, id);
    if (node == nullptr && (flags & CM_LOCATE_DEVNODE_NOVALIDATION) == 0) {
        (void)m.write_u32(out, 0);
        return m.set_return(CR_NO_SUCH_DEVNODE);
    }
    const u32 devinst = DevInstTable::instance().handle_for(node ? node->instance_id : id);
    if (m.write_u32(out, devinst).is_err()) return m.set_return(CR_INVALID_POINTER);
    m.set_return(CR_SUCCESS);
}

/// CONFIGRET CM_Get_Device_ID{A,W}(DEVINST, PTCHAR Buffer, ULONG BufferLen, ULONG)
/// s7_cfgmgr_probe getid_*: flags CR_INVALID_FLAG; NULL buffer or length 0
/// CR_INVALID_POINTER; unknown DEVINST CR_INVALID_DEVNODE. The terminator is
/// written only when it fits (an exact-length buffer succeeds without it);
/// a short buffer receives the characters that fit and CR_BUFFER_SMALL.
void cm_get_device_id(Marshaler& m, bool wide) {
    const u32 devinst = m.arg32(0);
    const GuestAddr buffer = m.arg_ptr(1);
    const u32 length = m.arg32(2);
    const u32 flags = m.arg32(3);
    if (flags != 0) return m.set_return(CR_INVALID_FLAG);
    if (buffer == 0 || length == 0) return m.set_return(CR_INVALID_POINTER);
    const auto id = devinst_id(devinst);
    if (!id) return m.set_return(CR_INVALID_DEVNODE);
    const usize count = std::min<usize>(id->size() + 1, length);
    std::vector<u8> bytes;
    if (wide) {
        auto all = utf16_bytes(*id);
        bytes.assign(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(count * 2));
    } else {
        std::string text = *id;
        text.push_back('\0');
        bytes.assign(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(count));
    }
    if (m.memory().write_bytes(buffer, bytes.data(), bytes.size()).is_err()) {
        return m.set_return(CR_INVALID_POINTER);
    }
    m.set_return(length < id->size() ? CR_BUFFER_SMALL : CR_SUCCESS);
}

/// CONFIGRET CM_Get_Device_ID_Size(PULONG, DEVINST, ULONG): characters
/// without the NUL; flags and unknown handles write 0 first
/// (s7_cfgmgr_probe getidsize_*).
void cm_get_device_id_size(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(0);
    const u32 devinst = m.arg32(1);
    const u32 flags = m.arg32(2);
    if (out == 0) return m.set_return(CR_INVALID_POINTER);
    (void)m.write_u32(out, 0);
    if (flags != 0) return m.set_return(CR_INVALID_FLAG);
    const auto id = devinst_id(devinst);
    if (!id) return m.set_return(CR_INVALID_DEVNODE);
    (void)m.write_u32(out, static_cast<u32>(id->size()));
    m.set_return(CR_SUCCESS);
}

/// CONFIGRET CM_Get_{Parent,Child,Sibling}(PDEVINST, DEVINST, ULONG)
/// s7_cfgmgr_probe parent_/child_/sibling_*: NULL output CR_INVALID_POINTER;
/// flags CR_INVALID_FLAG and a zero DEVINST CR_INVALID_DEVNODE, both with the
/// output untouched; otherwise the output is zeroed first; an unknown handle
/// is CR_INVALID_DEVNODE and a missing relative CR_NO_SUCH_DEVNODE.
void cm_get_relative(Marshaler& m, const CfgPtr& state, Relation relation) {
    const GuestAddr out = m.arg_ptr(0);
    const u32 devinst = m.arg32(1);
    const u32 flags = m.arg32(2);
    if (out == 0) return m.set_return(CR_INVALID_POINTER);
    if (flags != 0) return m.set_return(CR_INVALID_FLAG);
    if (devinst == 0) return m.set_return(CR_INVALID_DEVNODE);
    (void)m.write_u32(out, 0);
    const auto id = devinst_id(devinst);
    if (!id) return m.set_return(CR_INVALID_DEVNODE);
    const auto nodes = state->nodes();
    const NodeRecord* node = find_node(nodes, *id);
    if (node == nullptr) return m.set_return(CR_NO_SUCH_DEVNODE);
    const NodeRecord* found = nullptr;
    if (relation == Relation::Parent) {
        found = node->parent.empty() ? nullptr : find_node(nodes, node->parent);
    } else if (relation == Relation::Child) {
        for (const auto& other : nodes) {
            if (iequal(other.parent, node->instance_id)) { found = &other; break; }
        }
    } else if (!node->parent.empty()) {
        bool after = false;
        for (const auto& other : nodes) {
            if (!iequal(other.parent, node->parent)) continue;
            if (after) { found = &other; break; }
            after = &other == node;
        }
    }
    if (found == nullptr) return m.set_return(CR_NO_SUCH_DEVNODE);
    (void)m.write_u32(out, DevInstTable::instance().handle_for(found->instance_id));
    m.set_return(CR_SUCCESS);
}

/// CONFIGRET CM_Get_DevNode_Status(PULONG Status, PULONG Problem, DEVINST, ULONG)
/// s7_cfgmgr_probe status_*: NULL outputs CR_INVALID_POINTER, flags
/// CR_INVALID_FLAG, unknown handle CR_INVALID_DEVNODE (outputs untouched).
void cm_get_devnode_status(Marshaler& m, const CfgPtr& state) {
    const GuestAddr status = m.arg_ptr(0);
    const GuestAddr problem = m.arg_ptr(1);
    const u32 devinst = m.arg32(2);
    const u32 flags = m.arg32(3);
    if (status == 0 || problem == 0) return m.set_return(CR_INVALID_POINTER);
    if (flags != 0) return m.set_return(CR_INVALID_FLAG);
    const auto id = devinst_id(devinst);
    if (!id) return m.set_return(CR_INVALID_DEVNODE);
    const auto nodes = state->nodes();
    const NodeRecord* node = find_node(nodes, *id);
    if (node == nullptr) return m.set_return(CR_NO_SUCH_DEVNODE);
    (void)m.write_u32(status, node->status);
    (void)m.write_u32(problem, node->problem);
    m.set_return(CR_SUCCESS);
}

/// CONFIGRET CM_Get_DevNode_Registry_Property{A,W}(DEVINST, ULONG Property,
///     PULONG Type, PVOID Buffer, PULONG Length, ULONG)
/// s7_cfgmgr_probe drp_*: NULL length CR_INVALID_POINTER; flags
/// CR_INVALID_FLAG; unknown handle CR_INVALID_DEVNODE; a property outside
/// CM_DRP_MIN..CM_DRP_MAX CR_INVALID_PROPERTY (length untouched); an absent
/// value CR_NO_SUCH_VALUE with *Length = 0 and the type untouched; a short
/// buffer CR_BUFFER_SMALL with the needed length.
void cm_get_registry_property(Marshaler& m, const CfgPtr& state, bool wide) {
    const u32 devinst = m.arg32(0);
    const u32 property = m.arg32(1);
    const GuestAddr type_out = m.arg_ptr(2);
    const GuestAddr buffer = m.arg_ptr(3);
    const GuestAddr length_ptr = m.arg_ptr(4);
    const u32 flags = m.arg32(5);
    if (length_ptr == 0) return m.set_return(CR_INVALID_POINTER);
    if (flags != 0) return m.set_return(CR_INVALID_FLAG);
    const auto id = devinst_id(devinst);
    if (!id) return m.set_return(CR_INVALID_DEVNODE);
    if (property < 1 || property > CM_DRP_MAX) return m.set_return(CR_INVALID_PROPERTY);
    const auto length = m.read_u32(length_ptr);
    if (length.is_err()) return m.set_return(CR_INVALID_POINTER);
    const auto nodes = state->nodes();
    const NodeRecord* node = find_node(nodes, *id);
    const auto prop = spdrp_property(property - 1);
    const auto value = node != nullptr && prop ? node_property(nodes, *node, *prop)
                                               : std::nullopt;
    if (!value) {
        (void)m.write_u32(length_ptr, 0);
        return m.set_return(CR_NO_SUCH_VALUE);
    }
    const auto [type, bytes] = registry_encoding(*value, wide);
    if (type_out != 0) (void)m.write_u32(type_out, type);
    (void)m.write_u32(length_ptr, static_cast<u32>(bytes.size()));
    if (buffer == 0 || length.value() < bytes.size()) return m.set_return(CR_BUFFER_SMALL);
    if (m.memory().write_bytes(buffer, bytes.data(), bytes.size()).is_err()) {
        return m.set_return(CR_INVALID_POINTER);
    }
    m.set_return(CR_SUCCESS);
}

/// CONFIGRET CM_Get_DevNode_PropertyW(DEVINST, const DEVPROPKEY*, DEVPROPTYPE*,
///     PBYTE, PULONG Size, ULONG)
/// s7_cfgmgr_probe dnp_*: flags CR_INVALID_FLAG; NULL key CR_FAILURE; NULL
/// type or size, or a NULL buffer with a size, CR_INVALID_POINTER; unknown
/// handle CR_INVALID_DEVNODE; absent CR_NO_SUCH_VALUE with *Size = 0; a
/// short buffer CR_BUFFER_SMALL with type and size written.
void cm_get_devnode_property(Marshaler& m, const CfgPtr& state) {
    const u32 devinst = m.arg32(0);
    const GuestAddr key = m.arg_ptr(1);
    const GuestAddr type_out = m.arg_ptr(2);
    const GuestAddr buffer = m.arg_ptr(3);
    const GuestAddr size_ptr = m.arg_ptr(4);
    const u32 flags = m.arg32(5);
    if (flags != 0) return m.set_return(CR_INVALID_FLAG);
    const auto fmtid = read_guid(m, key);
    const auto pid = key != 0 ? m.read_u32(key + 16) : VResult<u32>::err(Error::InvalidArgument);
    if (!fmtid || pid.is_err()) return m.set_return(CR_FAILURE);
    if (type_out == 0 || size_ptr == 0) return m.set_return(CR_INVALID_POINTER);
    const auto size = m.read_u32(size_ptr);
    if (size.is_err() || (buffer == 0 && size.value() != 0)) return m.set_return(CR_INVALID_POINTER);
    const auto id = devinst_id(devinst);
    if (!id) return m.set_return(CR_INVALID_DEVNODE);
    const auto nodes = state->nodes();
    const NodeRecord* node = find_node(nodes, *id);
    const auto prop = devpkey_property(*fmtid, pid.value());
    const auto value = node != nullptr && prop ? node_property(nodes, *node, *prop)
                                               : std::nullopt;
    if (!value) {
        (void)m.write_u32(size_ptr, 0);
        return m.set_return(CR_NO_SUCH_VALUE);
    }
    const auto [type, bytes] = devprop_encoding(*value);
    (void)m.write_u32(type_out, type);
    (void)m.write_u32(size_ptr, static_cast<u32>(bytes.size()));
    if (size.value() < bytes.size()) return m.set_return(CR_BUFFER_SMALL);
    if (m.memory().write_bytes(buffer, bytes.data(), bytes.size()).is_err()) {
        return m.set_return(CR_INVALID_POINTER);
    }
    m.set_return(CR_SUCCESS);
}

/// CONFIGRET CM_Open_DevNode_Key(DEVINST, REGSAM, ULONG HardwareProfile,
///     REGDISPOSITION, PHKEY, ULONG)
/// s7_cfgmgr_probe odk_*: flags outside CM_REGISTRY_BITS CR_INVALID_FLAG; a
/// disposition other than OpenAlways/OpenExisting CR_INVALID_DATA; NULL
/// output CR_INVALID_POINTER; unknown handle CR_INVALID_DEVNODE; then
/// *phkDevice = NULL; a missing key CR_NO_SUCH_REGISTRY_KEY when opening,
/// CR_ACCESS_DENIED when it would have to be created; write access to an
/// existing key without elevation CR_ACCESS_DENIED.
void cm_open_devnode_key(Marshaler& m, const CfgPtr& state) {
    const u32 devinst = m.arg32(0);
    const u32 sam = m.arg32(1);
    const u32 profile = m.arg32(2);
    const u32 disposition = m.arg32(3);
    const GuestAddr out = m.arg_ptr(4);
    const u32 flags = m.arg32(5);
    if ((flags & ~CM_REGISTRY_BITS) != 0) return m.set_return(CR_INVALID_FLAG);
    if ((disposition & ~REG_DISPOSITION_BITS) != 0) return m.set_return(CR_INVALID_DATA);
    if (out == 0) return m.set_return(CR_INVALID_POINTER);
    const auto id = devinst_id(devinst);
    if (!id) return m.set_return(CR_INVALID_DEVNODE);
    (void)m.write_ptr(out, 0);
    const auto nodes = state->nodes();
    const NodeRecord* node = find_node(nodes, *id);
    std::string path;
    if (node != nullptr && (flags & ~CM_REGISTRY_SOFTWARE) == 0 && profile == 0) {
        if ((flags & CM_REGISTRY_SOFTWARE) != 0) {
            if (!node->driver.empty()) path = std::string(CLASS_KEY) + "\\" + node->driver;
        } else if (!node->parent.empty()) {
            ensure_device_hardware_key(state->registry, *node);
            path = std::string(ENUM_KEY) + "\\" + node->instance_id;
        }
    }
    const bool exists = !path.empty() && registry_key_exists(state->registry, path);
    if (!exists) {
        if (disposition != REG_DISPOSITION_OPEN_ALWAYS) return m.set_return(CR_NO_SUCH_REGISTRY_KEY);
        if (!state->elevated()) return m.set_return(CR_ACCESS_DENIED);
        // Creating a device's software (driver) key assigns it a driver
        // instance: that is driver installation, which Vitreo does not do.
        log_unimplemented_once("cfgmgr32!CM_Open_DevNode_Key", "creating a device key");
        return m.set_return(CR_ACCESS_DENIED);
    }
    if (wants_write_access(sam) && !state->elevated()) return m.set_return(CR_ACCESS_DENIED);
    const u64 key = open_registry_key(state->registry, path, sam);
    if (key == 0) return m.set_return(CR_NO_SUCH_REGISTRY_KEY);
    (void)m.write_ptr(out, key);
    m.set_return(CR_SUCCESS);
}

/// CONFIGRET CM_Disable_DevNode / CM_Enable_DevNode(DEVINST, ULONG)
/// s7_cfgmgr_probe: bad flags CR_INVALID_FLAG, unknown handle
/// CR_INVALID_DEVNODE; the root cannot be disabled (CR_FAILURE) and
/// CM_Enable_DevNode rejects it (CR_INVALID_DEVNODE). Changing a device's
/// state needs the PnP manager: CR_ACCESS_DENIED, what a non-administrator
/// gets.
void cm_change_devnode(Marshaler& m, const CfgPtr& state, bool disable) {
    const u32 devinst = m.arg32(0);
    const u32 flags = m.arg32(1);
    if ((flags & ~(disable ? CM_DISABLE_BITS : 0u)) != 0) return m.set_return(CR_INVALID_FLAG);
    const auto id = devinst_id(devinst);
    if (!id) return m.set_return(CR_INVALID_DEVNODE);
    if (iequal(*id, ROOT_INSTANCE_ID)) {
        return m.set_return(disable ? CR_FAILURE : CR_INVALID_DEVNODE);
    }
    const auto nodes = state->nodes();
    if (find_node(nodes, *id) == nullptr) return m.set_return(CR_NO_SUCH_DEVNODE);
    if (state->elevated()) {
        log_unimplemented_once(disable ? "cfgmgr32!CM_Disable_DevNode" : "cfgmgr32!CM_Enable_DevNode",
                               "changing a device's state (no PnP manager)");
    }
    m.set_return(CR_ACCESS_DENIED);
}

} // namespace vitreo::winapi::setupapi_internal
