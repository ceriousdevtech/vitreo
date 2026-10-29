// SPDX-License-Identifier: LGPL-2.1-only

#include "winapi/system/setupapi/setupapi_internal.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "winapi/common/display_device_identity.hpp"

#include <cstdio>
#include <cstring>

namespace vitreo::winapi::setupapi_internal {

// ---- the device tree ----------------------------------------------------------

namespace {

/// The root devnode's status, measured (s7_cfgmgr_probe status_root,
/// s7_setupapi_probe gdp_root_status): DN_NO_SHOW_IN_DM | DN_NT_DRIVER |
/// DN_NT_ENUMERATOR | DN_STARTED | DN_DRIVER_LOADED.
constexpr u32 ROOT_STATUS = DN_NO_SHOW_IN_DM | DN_NT_DRIVER | DN_NT_ENUMERATOR |
                            DN_STARTED | DN_DRIVER_LOADED;
/// A running root-enumerated device with its driver loaded.
constexpr u32 ROOT_ENUMERATED_STARTED = DN_ROOT_ENUMERATED | DN_DRIVER_LOADED |
                                        DN_STARTED | DN_NT_ENUMERATOR | DN_NT_DRIVER;
/// Base container of the computer itself (internal devices and the root):
/// s7_setupapi_probe gdrp_root_prop p=24.
constexpr char COMPUTER_CONTAINER_ID[] = "{00000000-0000-0000-FFFF-FFFFFFFFFFFF}";

} // namespace

[[nodiscard]] std::string display_hardware_id() {
    const auto& identity = display_device_identity::adapter_identity();
    if (!identity.has_pci_identity()) {
        return display_device_identity::HARDWARE_ID_A;
    }
    std::array<char, 96> id{};
    std::snprintf(
        id.data(), id.size(),
        "PCI\\VEN_%04X&DEV_%04X&SUBSYS_%08X&REV_%02X",
        identity.vendor_id.value_or(0), identity.device_id.value_or(0),
        identity.subsystem_id.value_or(0), identity.revision.value_or(0));
    return id.data();
}

/// The devnodes that exist right now. The display adapter is present only
/// while the host exposes a Metal device (an empty name means none).
[[nodiscard]] std::vector<NodeRecord> device_nodes(const std::string& display_name) {
    std::vector<NodeRecord> nodes;
    NodeRecord root;
    root.instance_id = ROOT_INSTANCE_ID;
    root.enumerator = "HTREE";
    root.container_id = COMPUTER_CONTAINER_ID;
    root.status = ROOT_STATUS;
    nodes.push_back(std::move(root));
    if (!display_name.empty()) {
        NodeRecord display;
        display.instance_id = display_device_identity::INSTANCE_ID_A;
        display.parent = ROOT_INSTANCE_ID;
        display.class_guid = GUID_DEVCLASS_DISPLAY;
        display.class_name = "Display";
        display.device_desc = display_name;
        display.friendly_name = display_name;
        display.hardware_ids = {display_hardware_id()};
        display.mfg = "Apple Inc.";
        display.enumerator = "ROOT";
        display.driver = guid_string(GUID_DEVCLASS_DISPLAY) + "\\0000";
        display.container_id = COMPUTER_CONTAINER_ID;
        display.status = ROOT_ENUMERATED_STARTED;
        display.interfaces = {GUID_DEVINTERFACE_DISPLAY_ADAPTER,
                              GUID_DISPLAY_DEVICE_ARRIVAL};
        nodes.push_back(std::move(display));
    }
    return nodes;
}

[[nodiscard]] const NodeRecord* find_node(const std::vector<NodeRecord>& nodes,
                                          std::string_view id) {
    for (const auto& node : nodes) {
        if (iequal(node.instance_id, id)) return &node;
    }
    return nullptr;
}

/// Symbolic link of a device interface: "\\?\" + instance ID with '\' as '#',
/// '#', and the interface class GUID in lower case. SetupDi reports the whole
/// path in lower case, CM_Get_Device_Interface_List keeps the instance ID's
/// case (s7_setupapi_probe detw_full all_lower=1, s7_cfgmgr_probe ifl_disp
/// lower=0; both tails are the lower-case GUID).
[[nodiscard]] std::string interface_path(std::string_view instance_id,
                                         const Guid& interface_guid,
                                         bool lower_case) {
    std::string path = "\\\\?\\";
    for (const char c : instance_id) path.push_back(c == '\\' ? '#' : c);
    path += '#';
    path += guid_string(interface_guid);
    return lower_case ? ascii_lower(path) : path;
}

// ---- device properties --------------------------------------------------------

[[nodiscard]] std::optional<PropValue> string_prop(const std::string& text) {
    if (text.empty()) return std::nullopt;
    PropValue v;
    v.text = text;
    return v;
}

[[nodiscard]] std::optional<PropValue> node_property(
    const std::vector<NodeRecord>& nodes, const NodeRecord& node, Prop prop) {
    PropValue v;
    switch (prop) {
        case Prop::DeviceDesc: return string_prop(node.device_desc);
        case Prop::HardwareIds:
            if (node.hardware_ids.empty()) return std::nullopt;
            v.kind = PropValue::Kind::List;
            v.list = node.hardware_ids;
            return v;
        case Prop::CompatibleIds: return std::nullopt;
        case Prop::Service: return std::nullopt;
        case Prop::Class: return string_prop(node.class_name);
        case Prop::ClassGuid:
            if (!node.class_guid) return std::nullopt;
            v.kind = PropValue::Kind::GuidValue;
            v.guid = *node.class_guid;
            return v;
        case Prop::Driver: return string_prop(node.driver);
        case Prop::ConfigFlags: return std::nullopt;
        case Prop::Mfg: return string_prop(node.mfg);
        case Prop::FriendlyName: return string_prop(node.friendly_name);
        case Prop::EnumeratorName: return string_prop(node.enumerator);
        case Prop::InstanceId: return string_prop(node.instance_id);
        case Prop::Parent: return string_prop(node.parent);
        case Prop::Children: {
            for (const auto& other : nodes) {
                if (iequal(other.parent, node.instance_id)) v.list.push_back(other.instance_id);
            }
            if (v.list.empty()) return std::nullopt;
            v.kind = PropValue::Kind::List;
            return v;
        }
        case Prop::DevNodeStatus:
            v.kind = PropValue::Kind::Dword;
            v.dword = node.status;
            return v;
        case Prop::ProblemCode:
            v.kind = PropValue::Kind::Dword;
            v.dword = node.problem;
            return v;
        case Prop::BaseContainerId: return string_prop(node.container_id);
    }
    return std::nullopt;
}

/// SPDRP_* ordinal -> property (SetupAPI.h). CM_DRP_* is SPDRP_* + 1.
[[nodiscard]] std::optional<Prop> spdrp_property(u32 spdrp) {
    switch (spdrp) {
        case 0x00: return Prop::DeviceDesc;
        case 0x01: return Prop::HardwareIds;
        case 0x02: return Prop::CompatibleIds;
        case 0x04: return Prop::Service;
        case 0x07: return Prop::Class;
        case 0x08: return Prop::ClassGuid;
        case 0x09: return Prop::Driver;
        case 0x0A: return Prop::ConfigFlags;
        case 0x0B: return Prop::Mfg;
        case 0x0C: return Prop::FriendlyName;
        case 0x16: return Prop::EnumeratorName;
        case 0x24: return Prop::BaseContainerId;
        default: return std::nullopt;
    }
}

/// DEVPKEY_* (devpkey.h) -> property.
[[nodiscard]] std::optional<Prop> devpkey_property(const Guid& fmtid, u32 pid) {
    // DEVPKEY_Device_* {a45c254e-df1c-4efd-8020-67d146a850e0}
    static constexpr Guid kDevice{0x4E, 0x25, 0x5C, 0xA4, 0x1C, 0xDF, 0xFD, 0x4E,
                                  0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0};
    // DEVPKEY_Device_InstanceId {78c34fc8-104a-4aca-9ea4-524d52996e57},256
    static constexpr Guid kInstance{0xC8, 0x4F, 0xC3, 0x78, 0x4A, 0x10, 0xCA, 0x4A,
                                    0x9E, 0xA4, 0x52, 0x4D, 0x52, 0x99, 0x6E, 0x57};
    // DEVPKEY_Device_DevNodeStatus/ProblemCode/Parent/Children
    // {4340a6c5-93fa-4706-972c-7b648008a5a7}
    static constexpr Guid kNode{0xC5, 0xA6, 0x40, 0x43, 0xFA, 0x93, 0x06, 0x47,
                                0x97, 0x2C, 0x7B, 0x64, 0x80, 0x08, 0xA5, 0xA7};
    if (fmtid == kDevice) {
        switch (pid) {
            case 2: return Prop::DeviceDesc;
            case 3: return Prop::HardwareIds;
            case 4: return Prop::CompatibleIds;
            case 6: return Prop::Service;
            case 9: return Prop::Class;
            case 10: return Prop::ClassGuid;
            case 11: return Prop::Driver;
            case 12: return Prop::ConfigFlags;
            case 13: return Prop::Mfg;
            case 14: return Prop::FriendlyName;
            case 24: return Prop::EnumeratorName;
            default: return std::nullopt;
        }
    }
    if (fmtid == kInstance && pid == 256) return Prop::InstanceId;
    if (fmtid == kNode) {
        switch (pid) {
            case 2: return Prop::DevNodeStatus;
            case 3: return Prop::ProblemCode;
            case 8: return Prop::Parent;
            case 9: return Prop::Children;
            default: return std::nullopt;
        }
    }
    return std::nullopt;
}

/// Registry (SPDRP / CM_DRP) encoding: REG_SZ / REG_MULTI_SZ / REG_DWORD, the
/// class GUID as its REG_SZ string. Wide or ANSI characters.
[[nodiscard]] std::pair<u32, std::vector<u8>> registry_encoding(
    const PropValue& v, bool wide) {
    auto chars = [wide](std::string_view text) {
        if (wide) return utf16_bytes(text);
        std::vector<u8> bytes(text.begin(), text.end());
        bytes.push_back(0);
        return bytes;
    };
    switch (v.kind) {
        case PropValue::Kind::String: return {REG_SZ, chars(v.text)};
        case PropValue::Kind::GuidValue: return {REG_SZ, chars(guid_string(v.guid))};
        case PropValue::Kind::Dword: {
            std::vector<u8> bytes(4);
            std::memcpy(bytes.data(), &v.dword, 4);
            return {REG_DWORD, bytes};
        }
        case PropValue::Kind::List: {
            std::vector<u8> bytes;
            for (const auto& item : v.list) {
                const auto one = chars(item);
                bytes.insert(bytes.end(), one.begin(), one.end());
            }
            bytes.resize(bytes.size() + (wide ? 2 : 1), 0);
            return {REG_MULTI_SZ, bytes};
        }
    }
    return {REG_SZ, {}};
}

/// Unified-property (DEVPKEY) encoding: DEVPROP_TYPE_* with UTF-16 strings.
[[nodiscard]] std::pair<u32, std::vector<u8>> devprop_encoding(const PropValue& v) {
    switch (v.kind) {
        case PropValue::Kind::String: return {DEVPROP_TYPE_STRING, utf16_bytes(v.text)};
        case PropValue::Kind::List: return {DEVPROP_TYPE_STRING_LIST, utf16_multi_bytes(v.list)};
        case PropValue::Kind::Dword: {
            std::vector<u8> bytes(4);
            std::memcpy(bytes.data(), &v.dword, 4);
            return {DEVPROP_TYPE_UINT32, bytes};
        }
        case PropValue::Kind::GuidValue:
            return {DEVPROP_TYPE_GUID, std::vector<u8>(v.guid.begin(), v.guid.end())};
    }
    return {DEVPROP_TYPE_STRING, {}};
}

// ---- registry helpers -----------------------------------------------------------

[[nodiscard]] std::wstring widen(std::string_view text) {
    return std::wstring(text.begin(), text.end());
}

/// Write access to a device or class key: KEY_SET_VALUE, KEY_CREATE_SUB_KEY,
/// KEY_CREATE_LINK, DELETE, WRITE_DAC, WRITE_OWNER, GENERIC_WRITE or
/// GENERIC_ALL. Those keys are writable by administrators only
/// (s7_setupapi_probe odrk_dev_all / ocrk_disp_all: ERROR_ACCESS_DENIED).
[[nodiscard]] bool wants_write_access(u32 sam) {
    constexpr u32 kWriteBits = 0x0002u | 0x0004u | 0x0020u | 0x00010000u |
                               0x00040000u | 0x00080000u | 0x40000000u | 0x10000000u;
    return (sam & kWriteBits) != 0;
}

[[nodiscard]] bool registry_key_exists(RegistryThunks* registry, const std::string& path) {
    if (registry == nullptr) return false;
    u64 handle = 0;
    if (registry->reg_open_key_ex_w(HKEY_LOCAL_MACHINE_VALUE, widen(path), 0, 0x20019u,
                                    &handle) != error_codes::ERROR_SUCCESS) {
        return false;
    }
    (void)registry->reg_close_key(handle);
    return true;
}

[[nodiscard]] u64 open_registry_key(RegistryThunks* registry, const std::string& path,
                                    u32 sam) {
    if (registry == nullptr) return 0;
    u64 handle = 0;
    if (registry->reg_open_key_ex_w(HKEY_LOCAL_MACHINE_VALUE, widen(path), 0, sam,
                                    &handle) != error_codes::ERROR_SUCCESS) {
        return 0;
    }
    return handle;
}

void set_registry_value(RegistryThunks* registry, u64 key, const wchar_t* name,
                        u32 type, const std::vector<u8>& data) {
    (void)registry->reg_set_value_ex_w(key, name, type, data.data(),
                                       static_cast<u32>(data.size()));
}

/// The PnP manager keeps every device's hardware key at Enum\<instance ID>
/// (with a "Device Parameters" subkey) holding its identification values.
/// Vitreo's modelled devices get theirs the first time a caller opens one,
/// so the key a guest opens agrees with what the SetupDi/CM calls report.
/// The root devnode has no hardware key (s7_cfgmgr_probe odk_root_hw).
void ensure_device_hardware_key(RegistryThunks* registry, const NodeRecord& node) {
    if (registry == nullptr || node.parent.empty()) return;
    const std::string path = std::string(ENUM_KEY) + "\\" + node.instance_id;
    u64 key = 0;
    u32 disposition = 0;
    if (registry->reg_create_key_ex_w(HKEY_LOCAL_MACHINE_VALUE, widen(path), &key,
                                      &disposition) != error_codes::ERROR_SUCCESS) {
        return;
    }
    if (!node.device_desc.empty()) {
        set_registry_value(registry, key, L"DeviceDesc", REG_SZ, utf16_bytes(node.device_desc));
    }
    if (!node.hardware_ids.empty()) {
        set_registry_value(registry, key, L"HardwareID", REG_MULTI_SZ,
                           utf16_multi_bytes(node.hardware_ids));
    }
    if (node.class_guid) {
        set_registry_value(registry, key, L"Class", REG_SZ, utf16_bytes(node.class_name));
        set_registry_value(registry, key, L"ClassGUID", REG_SZ,
                           utf16_bytes(guid_string(*node.class_guid)));
    }
    if (!node.driver.empty()) {
        set_registry_value(registry, key, L"Driver", REG_SZ, utf16_bytes(node.driver));
    }
    if (!node.mfg.empty()) {
        set_registry_value(registry, key, L"Mfg", REG_SZ, utf16_bytes(node.mfg));
    }
    u64 parameters = 0;
    if (registry->reg_create_key_ex_w(key, L"Device Parameters", &parameters,
                                      &disposition) == error_codes::ERROR_SUCCESS) {
        (void)registry->reg_close_key(parameters);
    }
    (void)registry->reg_close_key(key);
}

std::function<std::string()> default_display_name_provider(
    std::function<std::string()> provider) {
    if (provider) return provider;
    return [] {
        const auto* backend = graphics::active_backend_device();
        return (backend != nullptr && backend->available())
            ? backend->backend_device_name()
            : std::string{};
    };
}

} // namespace vitreo::winapi::setupapi_internal
