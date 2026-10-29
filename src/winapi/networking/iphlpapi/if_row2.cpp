/// @file if_row2.cpp
/// @brief GetIfEntry2 / GetIfTable2 / FreeMibTable: MIB_IF_ROW2 images.

#include "iphlpapi_internal.hpp"

namespace vitreo::winapi {

namespace {

/// MIB_IF_ROW2 image of one host interface (if_row2_contract.inc offsets).
[[nodiscard]] std::array<u8, kIfRow2Size> if_row2_image(const iphlp::HostInterface& i) {
    std::array<u8, kIfRow2Size> row{};
    auto put32 = [&](u32 off, u64 v) { const u32 n = static_cast<u32>(v); std::memcpy(row.data() + off, &n, 4); };
    auto put64 = [&](u32 off, u64 v) { std::memcpy(row.data() + off, &v, 8); };
    auto text = [&](u32 off, const std::string& utf8) {
        const auto w = utf8_to_utf16(utf8);
        const usize n = std::min<usize>(w.size(), kIF_MAX_STRING_SIZE);
        std::memcpy(row.data() + off, w.data(), n * 2);
    };
    put64(kIfRow2InterfaceLuid, i.luid);
    put32(kIfRow2InterfaceIndex, i.index);
    std::memcpy(row.data() + kIfRow2InterfaceGuid, i.guid.data(), 16);
    // Alias is the adapter's friendly name (iphlp_adapters_probe ift2
    // alias_eq_friendlyname=1, description_eq=1).
    text(kIfRow2Alias, i.friendly_name);
    text(kIfRow2Description, i.description);
    const usize mac_len = std::min<usize>(i.mac.size(), kIF_MAX_PHYS_ADDRESS_LENGTH);
    put32(kIfRow2PhysicalAddressLength, mac_len);
    std::memcpy(row.data() + kIfRow2PhysicalAddress, i.mac.data(), mac_len);
    // The host reports only the address in use; a factory address the host
    // replaced (e.g. a private Wi-Fi address) is not exposed.
    std::memcpy(row.data() + kIfRow2PermanentPhysicalAddress, i.mac.data(), mac_len);
    put32(kIfRow2Mtu, i.link_mtu);
    put32(kIfRow2Type, i.if_type);
    put32(kIfRow2TunnelType, i.tunnel_type);
    put32(kIfRow2MediaType, i.media_type);
    put32(kIfRow2PhysicalMediumType, i.physical_medium);
    put32(kIfRow2AccessType, i.access_type);
    put32(kIfRow2DirectionType, kNET_IF_DIRECTION_SENDRECEIVE);
    row[kIfRow2InterfaceAndOperStatusFlags] = i.status_flags;
    put32(kIfRow2OperStatus, i.oper_status);
    put32(kIfRow2AdminStatus, i.admin_status);
    put32(kIfRow2MediaConnectState, i.media_connect_state);
    std::memcpy(row.data() + kIfRow2NetworkGuid, i.network_guid.data(), 16);
    put32(kIfRow2ConnectionType, i.connection_type);
    put64(kIfRow2TransmitLinkSpeed, i.link_speed);
    put64(kIfRow2ReceiveLinkSpeed, i.link_speed);
    // Counters the host driver keeps. Windows' software loopback reports none
    // (if_row2_contract_probe), and the byte-class split (unicast/multicast/
    // broadcast octets), OutDiscards and OutQLen are counters the host does
    // not keep: they read 0, as Windows reports counters a miniport does not
    // supply.
    if (!i.loopback) {
        put64(kIfRow2InOctets, i.counters.in_octets);
        put64(kIfRow2InUcastPkts, i.counters.in_ucast);
        put64(kIfRow2InNUcastPkts, i.counters.in_nucast);
        put64(kIfRow2InDiscards, i.counters.in_discards);
        put64(kIfRow2InErrors, i.counters.in_errors);
        put64(kIfRow2InUnknownProtos, i.counters.in_unknown);
        put64(kIfRow2OutOctets, i.counters.out_octets);
        put64(kIfRow2OutUcastPkts, i.counters.out_ucast);
        put64(kIfRow2OutNUcastPkts, i.counters.out_nucast);
        put64(kIfRow2OutErrors, i.counters.out_errors);
    }
    return row;
}

} // namespace

void IphlpapiManager::impl_GetIfEntry2(Marshaler& m) {
    // https://learn.microsoft.com/en-us/windows/win32/api/netioapi/nf-netioapi-getifentry2
    const GuestAddr row = m.arg_ptr(0);
    if (row == 0 || !m.memory().guest_can_write(row, kIfRow2Size)) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const auto luid_value = m.memory().read_u64(row + kIfRow2InterfaceLuid);
    const auto index_value = m.read_u32(row + kIfRow2InterfaceIndex);
    if (!luid_value.is_ok() || !index_value.is_ok()) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    // Windows clears the last error before validating the selectors
    // (ip_interface_contract_probe row-zero/row-invalid-* last-error=0).
    winapi::set_last_error(NO_ERROR);
    const u64 luid = luid_value.value();
    const u32 index = index_value.value();
    if (luid == 0 && index == 0) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const auto snapshot = iphlp::snapshot_host_interfaces(iphlp::SnapshotDetail::Full);
    // Learn requires LUID precedence even when the supplied index is valid.
    const auto* iface = luid != 0 ? snapshot.by_luid(luid) : snapshot.by_index(index);
    if (!iface) {
        m.set_return(ERROR_FILE_NOT_FOUND);
        return;
    }
    const auto image = if_row2_image(*iface);
    m.set_return(m.memory().write_bytes(row, image.data(), image.size()).is_ok()
                 ? NO_ERROR : ERROR_INVALID_PARAMETER);
}

void IphlpapiManager::impl_GetIfTable2(Marshaler& m) {
    // NETIO_STATUS GetIfTable2(PMIB_IF_TABLE2 *Table)
    // https://learn.microsoft.com/en-us/windows/win32/api/netioapi/nf-netioapi-getiftable2
    // The table is allocated for the caller and released with FreeMibTable.
    const GuestAddr out = m.arg_ptr(0);
    const bool wide = m.cpu_is_64bit();
    if (out == 0 || !m.memory().guest_can_write(out, wide ? 8 : 4)) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    if (!m_heap) {
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED iphlpapi!GetIfTable2: no process heap is configured for the table\n");
        });
        m.set_return(ERROR_NOT_ENOUGH_MEMORY_CODE);
        return;
    }
    const auto snapshot = iphlp::snapshot_host_interfaces(iphlp::SnapshotDetail::Full);
    if (!snapshot.ok) { m.set_return(ERROR_NOT_ENOUGH_MEMORY_CODE); return; }
    const AdapterAbi& L = adapter_abi(m);
    const usize header = L.MIB_IF_TABLE2_Table;
    std::vector<u8> image(header + snapshot.interfaces.size() * kIfRow2Size, 0);
    const u32 count = static_cast<u32>(snapshot.interfaces.size());
    std::memcpy(image.data(), &count, 4);
    for (usize i = 0; i < snapshot.interfaces.size(); ++i) {
        const auto row = if_row2_image(snapshot.interfaces[i]);
        std::memcpy(image.data() + header + i * kIfRow2Size, row.data(), row.size());
    }
    const u64 table = m_heap->heap_alloc(m_heap->get_process_heap(), 0, image.size());
    if (table == 0) { m.set_return(ERROR_NOT_ENOUGH_MEMORY_CODE); return; }
    if (m.memory().write_bytes(table, image.data(), image.size()).is_err() ||
        m.write_ptr(out, table).is_err()) {
        (void)m_heap->heap_free(m_heap->get_process_heap(), 0, table);
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    winapi::set_last_error(NO_ERROR);  // iphlp_adapters_probe ift2 le=0x0
    m.set_return(NO_ERROR);
}

void IphlpapiManager::impl_FreeMibTable(Marshaler& m) {
    // VOID FreeMibTable(PVOID Memory): releases a table the IP Helper
    // functions allocated (GetIfTable2).
    const GuestAddr memory = m.arg_ptr(0);
    if (memory && m_heap) (void)m_heap->heap_free(m_heap->get_process_heap(), 0, memory);
    m.set_return(0u);
}

} // namespace vitreo::winapi
