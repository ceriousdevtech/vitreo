/// @file if_table.cpp
/// @brief GetIfEntry / GetIfTable: the legacy MIB_IFROW tables.

#include "iphlpapi_internal.hpp"

namespace vitreo::winapi {

namespace {
constexpr usize kIfRowSize = 860;
using IfRow = std::array<u8, kIfRowSize>;
/// MIB_IFROW (ipmib.h; 860 bytes on both bitnesses) of every host interface.
/// Names are the TCP/IP device paths and the description, type, MTU and
/// speed are the adapter's (iphlp_adapters_probe ifrow-*).
std::optional<std::vector<IfRow>> collect_if_rows() {
    const auto snapshot = iphlp::snapshot_host_interfaces(iphlp::SnapshotDetail::Full);
    if (!snapshot.ok) return std::nullopt;
    std::vector<IfRow> rows;
    for (const auto& i : snapshot.interfaces) {
        IfRow row{};
        auto put = [&](usize offset, u64 value) {
            const u32 narrowed = static_cast<u32>(std::min<u64>(value, 0xFFFFFFFFull));
            std::memcpy(row.data() + offset, &narrowed, 4);
        };
        const auto name = tcpip_device_name(i);
        std::memcpy(row.data(), name.data(), std::min<usize>(name.size(), 255) * 2);  // wszName
        put(512, i.index);
        put(516, i.if_type);
        put(520, i.link_mtu);
        put(524, i.link_speed);  // dwSpeed saturates (ifrow-primary speed_eq)
        const usize mac_size = std::min<usize>(i.mac.size(), 8);
        put(528, mac_size);
        std::memcpy(row.data() + 532, i.mac.data(), mac_size);
        put(540, i.admin_status);
        // MIB_IF_OPER_STATUS: OPERATIONAL 5, DISCONNECTED 2, NON_OPERATIONAL 0.
        put(544, i.oper_status == kIfOperStatusUp ? 5u : (i.admin_status == kNET_IF_ADMIN_STATUS_UP ? 2u : 0u));
        // dwLastChange is 0 on current Windows (ifrow-loopback).
        if (!i.loopback) {
            put(552, i.counters.in_octets);
            put(556, i.counters.in_ucast);
            put(560, i.counters.in_nucast);
            put(564, i.counters.in_discards);
            put(568, i.counters.in_errors);
            put(572, i.counters.in_unknown);
            put(576, i.counters.out_octets);
            put(580, i.counters.out_ucast);
            put(584, i.counters.out_nucast);
            put(592, i.counters.out_errors);
        }
        const std::string description = ansi_text(i.description).substr(0, 255);
        put(600, description.size() + 1);  // dwDescrLen counts the NUL
        std::memcpy(row.data() + 604, description.data(), description.size());
        rows.push_back(row);
    }
    return rows;
}
u32 if_row_index(const IfRow& row) {
    u32 index = 0;
    std::memcpy(&index, row.data() + 512, 4);
    return index;
}
} // namespace

void IphlpapiManager::impl_GetIfEntry(Marshaler& m) {
    const GuestAddr output = m.arg_ptr(0);
    const auto index = m.read_u32(output + 512);
    if (!output || index.is_err()) { m.set_return(ERROR_INVALID_PARAMETER); return; }
    const auto rows = collect_if_rows();
    if (!rows) { m.set_return(ERROR_NOT_SUPPORTED); return; }
    for (const auto& row : *rows) {
        if (if_row_index(row) != index.value()) continue;
        m.set_return(m.memory().write_bytes(output, row.data(), row.size()).is_ok()
                         ? NO_ERROR : ERROR_INVALID_PARAMETER);
        return;
    }
    m.set_return(ERROR_NOT_FOUND);
}

void IphlpapiManager::impl_GetIfTable(Marshaler& m) {
    const GuestAddr output = m.arg_ptr(0), size_ptr = m.arg_ptr(1);
    const auto supplied = m.read_u32(size_ptr);
    if (!size_ptr || supplied.is_err()) { m.set_return(ERROR_INVALID_PARAMETER); return; }
    auto rows = collect_if_rows();
    if (!rows) { m.set_return(ERROR_NOT_SUPPORTED); return; }
    if (m.arg(2)) std::sort(rows->begin(), rows->end(),
        [](const auto& a, const auto& b) { return if_row_index(a) < if_row_index(b); });
    const u32 needed = static_cast<u32>(4 + rows->size() * kIfRowSize);
    if (m.write_u32(size_ptr, needed).is_err()) { m.set_return(ERROR_INVALID_PARAMETER); return; }
    if (!output || supplied.value() < needed) { m.set_return(ERROR_INSUFFICIENT_BUFFER); return; }
    std::vector<u8> table(needed);
    const u32 count = static_cast<u32>(rows->size());
    std::memcpy(table.data(), &count, 4);
    for (usize i = 0; i < rows->size(); ++i)
        std::memcpy(table.data() + 4 + i * kIfRowSize, (*rows)[i].data(), kIfRowSize);
    m.set_return(m.memory().write_bytes(output, table.data(), table.size()).is_ok()
                     ? NO_ERROR : ERROR_INVALID_PARAMETER);
}

} // namespace vitreo::winapi
