/// @file interface_convert.cpp
/// @brief Interface index / LUID / GUID / NDIS-name conversions, if_nametoindex and
///        if_indextoname.

#include "iphlpapi_internal.hpp"

namespace vitreo::winapi {

namespace {

[[nodiscard]] std::optional<u64> read_luid(Marshaler& m, GuestAddr at) {
    if (at == 0) return std::nullopt;
    const auto value = m.memory().read_u64(at);
    if (value.is_err()) return std::nullopt;
    return value.value();
}

} // namespace

void IphlpapiManager::impl_ConvertInterfaceIndexToLuid(Marshaler& m) {
    // NETIO_STATUS ConvertInterfaceIndexToLuid(NET_IFINDEX, PNET_LUID)
    const u32 index = m.arg32(0);
    const GuestAddr luid_ptr = m.arg_ptr(1);
    if (!luid_ptr) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const auto snapshot = iphlp::snapshot_host_interfaces(iphlp::SnapshotDetail::Identity);
    const auto* iface = snapshot.by_index(index);
    const u64 luid = iface ? iface->luid : 0;
    // A missing index zeroes the output (if_row2_contract_probe index-missing-luid=0).
    if (!m.memory().write_u64(luid_ptr, luid).is_ok()) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    m.set_return(iface ? NO_ERROR : ERROR_FILE_NOT_FOUND);
}

void IphlpapiManager::impl_ConvertInterfaceLuidToIndex(Marshaler& m) {
    const GuestAddr luid_ptr = m.arg_ptr(0);
    const GuestAddr index_ptr = m.arg_ptr(1);
    const auto luid = read_luid(m, luid_ptr);
    if (!luid || !index_ptr) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const auto snapshot = iphlp::snapshot_host_interfaces(iphlp::SnapshotDetail::Identity);
    const auto* iface = snapshot.by_luid(*luid);
    if (!iface) {
        (void)m.write_u32(index_ptr, 0xFFFFFFFFu); // NET_IFINDEX_UNSPECIFIED
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    (void)m.write_u32(index_ptr, iface->index);
    m.set_return(NO_ERROR);
}

void IphlpapiManager::impl_ConvertInterfaceLuidToNameW(Marshaler& m) {
    // NETIO_STATUS ConvertInterfaceLuidToNameW(const NET_LUID*, PWSTR, SIZE_T)
    // A pure conversion: "<type>_<NetLuidIndex>" for any LUID, existing or
    // not; a buffer shorter than the name is ERROR_NOT_ENOUGH_MEMORY
    // (iphlp_adapters_probe name-loopback*, name-missing-luid, name-type).
    const auto luid = read_luid(m, m.arg_ptr(0));
    const GuestAddr name_ptr = m.arg_ptr(1);
    const u64 length = m.cpu_is_64bit() ? m.arg(2) : m.arg32(2);
    if (!luid || !name_ptr) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const std::string name = iphlp::ndis_interface_name(*luid);
    if (name.size() + 1 > length) {
        m.set_return(ERROR_NOT_ENOUGH_MEMORY_CODE);
        return;
    }
    std::vector<u8> bytes((name.size() + 1) * 2, 0);
    for (usize i = 0; i < name.size(); ++i) bytes[i * 2] = static_cast<u8>(name[i]);
    m.set_return(m.memory().write_bytes(name_ptr, bytes.data(), bytes.size()).is_ok()
                 ? NO_ERROR : ERROR_INVALID_PARAMETER);
}

void IphlpapiManager::impl_ConvertInterfaceNameToLuidW(Marshaler& m) {
    // NETIO_STATUS ConvertInterfaceNameToLuidW(const WCHAR*, PNET_LUID)
    // Parses the NDIS name without requiring the interface to exist; a name
    // that is not "<type>_<index>" is ERROR_INVALID_NAME with the output
    // zeroed (iphlp_adapters_probe name-to-luid-*).
    const GuestAddr name_ptr = m.arg_ptr(0);
    const GuestAddr luid_ptr = m.arg_ptr(1);
    if (!name_ptr || !luid_ptr) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const auto luid = iphlp::parse_ndis_interface_name(read_guest_wide(m, name_ptr, kIF_MAX_STRING_SIZE + 1));
    (void)m.memory().write_u64(luid_ptr, luid ? *luid : 0);
    m.set_return(luid ? NO_ERROR : ERROR_INVALID_NAME_CODE);
}

void IphlpapiManager::impl_if_nametoindex(Marshaler& m) {
    // NET_IFINDEX if_nametoindex(PCSTR InterfaceName)
    // https://learn.microsoft.com/en-us/windows/win32/api/netioapi/nf-netioapi-if_nametoindex
    // The name is the NDIS interface name (iphlp_adapters_probe if_nametoindex
    // loopback_0=1, name-roundtrip); anything else is 0.
    const GuestAddr name_ptr = m.arg_ptr(0);
    if (!name_ptr) {
        m.set_return(0u);
        return;
    }
    std::string name;
    for (u32 i = 0; i < kIF_MAX_STRING_SIZE + 1; ++i) {
        const auto ch = m.read_u8(name_ptr + i);
        if (ch.is_err() || ch.value() == 0) break;
        name.push_back(static_cast<char>(ch.value()));
    }
    const auto luid = iphlp::parse_ndis_interface_name(name);
    if (!luid) { m.set_return(0u); return; }
    const auto snapshot = iphlp::snapshot_host_interfaces(iphlp::SnapshotDetail::Identity);
    const auto* iface = snapshot.by_luid(*luid);
    m.set_return(iface ? iface->index : 0u);
}

void IphlpapiManager::impl_if_indextoname(Marshaler& m) {
    // PCHAR if_indextoname(NET_IFINDEX InterfaceIndex, PCHAR InterfaceName)
    // https://learn.microsoft.com/en-us/windows/win32/api/netioapi/nf-netioapi-if_indextoname
    // The buffer holds at least IF_NAMESIZE characters; the result is the NDIS
    // name ("loopback_0"), NULL for a missing index (if_indextoname probe line).
    const u32 index = m.arg32(0);
    const GuestAddr name_ptr = m.arg_ptr(1);
    if (!name_ptr) { m.set_return(0u); return; }
    const auto snapshot = iphlp::snapshot_host_interfaces(iphlp::SnapshotDetail::Identity);
    const auto* iface = snapshot.by_index(index);
    if (!iface) { m.set_return(0u); return; }
    const std::string name = iphlp::ndis_interface_name(iface->luid);
    if (m.memory().write_bytes(name_ptr, reinterpret_cast<const u8*>(name.c_str()), name.size() + 1).is_err()) {
        m.set_return(0u);
        return;
    }
    m.set_return(static_cast<u64>(name_ptr));
}

void IphlpapiManager::impl_ConvertInterfaceLuidToGuid(Marshaler& m) {
    const auto luid = read_luid(m, m.arg_ptr(0));
    const GuestAddr guid_ptr = m.arg_ptr(1);
    if (!luid || !guid_ptr) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const auto snapshot = iphlp::snapshot_host_interfaces(iphlp::SnapshotDetail::Identity);
    const auto* iface = snapshot.by_luid(*luid);
    if (!iface) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    // The GUID is the adapter's AdapterName in every other view
    // (iphlp_adapters_probe gaa-identity luid_to_guid, ift2 guid_eq_adaptername).
    m.set_return(m.memory().write_bytes(guid_ptr, iface->guid.data(), 16).is_ok()
                 ? NO_ERROR : ERROR_INVALID_PARAMETER);
}

void IphlpapiManager::impl_ConvertInterfaceGuidToLuid(Marshaler& m) {
    // https://learn.microsoft.com/en-us/windows/win32/api/netioapi/nf-netioapi-convertinterfaceguidtoluid
    const GuestAddr guid_ptr = m.arg_ptr(0);
    const GuestAddr luid_ptr = m.arg_ptr(1);
    if (guid_ptr == 0 || luid_ptr == 0) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    // Windows leaves the output untouched for null pointer parameters, but
    // zeroes it when a supplied GUID does not identify a local interface.
    // tools/oracle_probe/ip_interface_contract_probe{32,64}.expected.
    std::array<u8, 16> guid{};
    if (!m.memory().read_bytes(guid_ptr, guid.data(), guid.size()).is_ok() ||
        !m.memory().write_u64(luid_ptr, 0).is_ok()) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    winapi::set_last_error(NO_ERROR);
    const auto snapshot = iphlp::snapshot_host_interfaces(iphlp::SnapshotDetail::Identity);
    const auto* iface = snapshot.by_guid(guid);
    if (!iface) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    m.set_return(m.memory().write_u64(luid_ptr, iface->luid).is_ok() ? NO_ERROR : ERROR_INVALID_PARAMETER);
}

} // namespace vitreo::winapi
