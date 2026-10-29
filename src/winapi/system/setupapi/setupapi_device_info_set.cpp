// SPDX-License-Identifier: LGPL-2.1-only

#include "winapi/system/setupapi/setupapi_internal.hpp"

#include "winapi/kernel32/locale/string_conv.hpp"

#include <atomic>
#include <cstring>

namespace vitreo::winapi::setupapi_internal {

// ---- device information sets ----------------------------------------------------

/// Reserved cookies in SP_DEVINFO_DATA / SP_DEVICE_INTERFACE_DATA. Unique
/// process-wide so data from one set is rejected by another
/// (s7_setupapi_probe foreign_did, detw_foreignset: ERROR_INVALID_PARAMETER).
/// Kept below 2^32: ULONG_PTR is 32 bits on x86.
[[nodiscard]] u64 next_token() {
    static std::atomic<u32> counter{0x5E740000u};
    return counter.fetch_add(1);
}

namespace {

/// SP_DEVINFO_DATA / SP_DEVICE_INTERFACE_DATA: Reserved follows the 20-byte
/// head, pointer-aligned at 24 on both architectures.
constexpr GuestAddr kReservedOffset = 24;

} // namespace

[[nodiscard]] std::optional<usize> resolve_element(Marshaler& m, const DevInfoSet& set,
                                                   GuestAddr did) {
    if (did == 0) return std::nullopt;
    const auto cb = m.read_u32(did);
    if (cb.is_err() || cb.value() != layout(m).devinfo) return std::nullopt;
    const auto token = m.read_ptr(did + kReservedOffset);
    if (token.is_err()) return std::nullopt;
    for (usize i = 0; i < set.elements.size(); ++i) {
        if (set.elements[i].token == token.value()) return i;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<usize> resolve_interface(Marshaler& m, const DevInfoSet& set,
                                                     GuestAddr data) {
    if (data == 0) return std::nullopt;
    const auto cb = m.read_u32(data);
    if (cb.is_err() || cb.value() != layout(m).iface) return std::nullopt;
    const auto token = m.read_ptr(data + kReservedOffset);
    if (token.is_err()) return std::nullopt;
    for (usize i = 0; i < set.interfaces.size(); ++i) {
        if (set.interfaces[i].token == token.value()) return i;
    }
    return std::nullopt;
}

/// Fill a caller's SP_DEVINFO_DATA (cbSize already validated).
[[nodiscard]] bool write_devinfo(Marshaler& m, GuestAddr did, const SetElement& e) {
    const u32 devinst = DevInstTable::instance().handle_for(e.instance_id);
    return m.memory().write_bytes(did + 4, e.class_guid.data(), 16).is_ok() &&
           m.write_u32(did + 20, devinst).is_ok() &&
           m.write_ptr(did + kReservedOffset, e.token).is_ok();
}

[[nodiscard]] bool write_interface_data(Marshaler& m, GuestAddr data, const SetInterface& i) {
    return m.memory().write_bytes(data + 4, i.guid.data(), 16).is_ok() &&
           m.write_u32(data + 20, SPINT_ACTIVE).is_ok() &&
           m.write_ptr(data + kReservedOffset, i.token).is_ok();
}

usize add_node_element(DevInfoSet& set, const NodeRecord& node) {
    for (usize i = 0; i < set.elements.size(); ++i) {
        if (iequal(set.elements[i].instance_id, node.instance_id)) return i;
    }
    SetElement e;
    e.token = next_token();
    e.instance_id = node.instance_id;
    e.class_guid = node.class_guid.value_or(GUID_ZERO);
    set.elements.push_back(std::move(e));
    return set.elements.size() - 1;
}

usize add_interface(DevInfoSet& set, usize element, const Guid& guid,
                    const std::string& instance_id) {
    for (usize i = 0; i < set.interfaces.size(); ++i) {
        if (set.interfaces[i].element == element && set.interfaces[i].guid == guid) return i;
    }
    SetInterface iface;
    iface.token = next_token();
    iface.guid = guid;
    iface.element = element;
    iface.path = interface_path(instance_id, guid, true);
    set.interfaces.push_back(std::move(iface));
    return set.interfaces.size() - 1;
}

/// The modelled devnode for an element; created (unregistered) elements have
/// none.
[[nodiscard]] const NodeRecord* element_node(const std::vector<NodeRecord>& nodes,
                                             const SetElement& e) {
    return e.created ? nullptr : find_node(nodes, e.instance_id);
}

/// A property of an element: the devnode's, or for a created element the
/// values it was created and given.
[[nodiscard]] std::optional<PropValue> element_property(
    const std::vector<NodeRecord>& nodes, const SetElement& e, Prop prop) {
    if (!e.created) {
        const NodeRecord* node = element_node(nodes, e);
        if (node == nullptr) return std::nullopt;
        return node_property(nodes, *node, prop);
    }
    PropValue v;
    switch (prop) {
        case Prop::InstanceId: return string_prop(e.instance_id);
        case Prop::ClassGuid:
            if (e.class_guid == GUID_ZERO) return std::nullopt;
            v.kind = PropValue::Kind::GuidValue;
            v.guid = e.class_guid;
            return v;
        case Prop::Class: {
            const SetupClass* c = find_setup_class(e.class_guid);
            return c == nullptr ? std::nullopt : string_prop(c->name);
        }
        case Prop::EnumeratorName: return string_prop(first_component(e.instance_id));
        default: break;
    }
    // Values stored by SetupDiSetDeviceRegistryProperty (UTF-16 REG data).
    for (const auto& [spdrp, bytes] : e.created_props) {
        if (spdrp_property(spdrp) != prop) continue;
        std::vector<u16> units(bytes.size() / 2);
        std::memcpy(units.data(), bytes.data(), units.size() * 2);
        std::vector<std::string> strings;
        std::vector<u16> current;
        for (const u16 unit : units) {
            if (unit == 0) {
                if (current.empty()) break;
                strings.push_back(utf16_to_utf8(current.data(), static_cast<int>(current.size())));
                current.clear();
                continue;
            }
            current.push_back(unit);
        }
        if (!current.empty()) {
            strings.push_back(utf16_to_utf8(current.data(), static_cast<int>(current.size())));
        }
        if (strings.empty()) return std::nullopt;
        if (prop == Prop::HardwareIds || prop == Prop::CompatibleIds) {
            v.kind = PropValue::Kind::List;
            v.list = strings;
        } else {
            v.text = strings.front();
        }
        return v;
    }
    return std::nullopt;
}

} // namespace vitreo::winapi::setupapi_internal
