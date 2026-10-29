/// @file dhcpcsvc.cpp
/// @brief dhcpcsvc.dll: DHCP client API over the host's cached DHCPv4 lease.

#include "iphlpapi_internal.hpp"

namespace vitreo::winapi {

namespace {

constexpr u32 kDhcpcapiVersionV2 = 2;       // dhcpcsdk.h DHCPCAPI_VERSION_V2 (dhcp-init version=0x2)
constexpr u32 kDhcpcapiRequestPersistent = 0x01;
constexpr u32 kDhcpcapiRequestSynchronous = 0x02;
constexpr u32 kDhcpcapiRequestMask = 0x0F;
constexpr u32 kErrorMoreData = 234;
constexpr usize kMaxDhcpAdapterName = 256;  // Learn: "Must be under 256 characters"

void dhcp_unimplemented(const std::string& what) {
    static std::mutex mutex;
    static std::unordered_set<std::string> reported;
    std::lock_guard lock(mutex);
    if (reported.insert(what).second)
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED dhcpcsvc!DhcpRequestParams: %s\n", what.c_str());
}

void impl_DhcpRequestParams(Marshaler& m) {
    // DWORD DhcpRequestParams(DWORD Flags, LPVOID Reserved, LPWSTR AdapterName,
    //   LPDHCPCAPI_CLASSID ClassId, DHCPCAPI_PARAMS_ARRAY SendParams,
    //   DHCPCAPI_PARAMS_ARRAY RecdParams, LPBYTE Buffer, LPDWORD pSize,
    //   LPWSTR RequestIdStr)
    // https://learn.microsoft.com/en-us/windows/win32/api/dhcpcsdk/nf-dhcpcsdk-dhcprequestparams
    // The two DHCPCAPI_PARAMS_ARRAY values are passed by value: two DWORD
    // slots each on x86 (44-byte frame, iphlp_misc_probe x86-frame), and, being
    // 16-byte structures, by hidden reference on x64.
    const bool wide = m.cpu_is_64bit();
    const u32 flags = m.arg32(0);
    const GuestAddr reserved = m.arg_ptr(1);
    const GuestAddr adapter_name = m.arg_ptr(2);
    (void)m.arg_ptr(3);  // ClassId: only sent with a DHCP-INFORM
    u32 recd_count = 0;
    GuestAddr recd_params = 0;
    GuestAddr buffer = 0, size_ptr = 0;
    if (wide) {
        const GuestAddr recd = m.arg_ptr(5);
        recd_count = recd ? m.read_u32(recd).value_or(0) : 0;
        recd_params = recd ? m.memory().read_u64(recd + 8).value_or(0) : 0;
        buffer = m.arg_ptr(6);
        size_ptr = m.arg_ptr(7);
        (void)m.arg_ptr(8);
    } else {
        recd_count = m.arg32(6);
        recd_params = m.arg32(7);
        buffer = m.arg32(8);
        size_ptr = m.arg32(9);
        (void)m.arg32(10);
    }
    // Argument checks precede the adapter lookup and keep the last error
    // (iphlp_misc_probe dhcp-request-null-adapter/-null-size/-no-flags/-bad-flags).
    const auto have = read_size(m, size_ptr);
    if (!have || adapter_name == 0 || reserved != 0 || (flags & ~kDhcpcapiRequestMask) ||
        !(flags & (kDhcpcapiRequestSynchronous | kDhcpcapiRequestPersistent))) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    const auto name = m.read_wide_string(adapter_name, kMaxDhcpAdapterName + 1);
    if (name.is_err() || name.value().size() > kMaxDhcpAdapterName) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    // The lookup clears the last error; an unknown adapter is
    // ERROR_INVALID_PARAMETER (dhcp-request-missing-adapter le=0x0).
    winapi::set_last_error(NO_ERROR);
    std::string wanted = name.value();
    for (auto& c : wanted) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    const auto snapshot = iphlp::snapshot_host_interfaces(iphlp::SnapshotDetail::Full);
    const iphlp::HostInterface* adapter = nullptr;
    for (const auto& iface : snapshot.interfaces)
        if (iphlp::format_guid_braced(iface.guid) == wanted) adapter = &iface;
    if (!adapter) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    if (flags & kDhcpcapiRequestPersistent) {
        dhcp_unimplemented("DHCPCAPI_REQUEST_PERSISTENT (per-boot persistent requests)");
        m.set_return(ERROR_NOT_SUPPORTED);
        return;
    }
    if (!adapter->dhcp_enabled || !adapter->lease_obtained) {
        dhcp_unimplemented("adapter without a host DHCPv4 lease");
        m.set_return(ERROR_NOT_SUPPORTED);
        return;
    }
    const usize param_size = wide ? 32 : 20;  // DHCPCAPI_PARAMS (iphlp_misc_probe sizes)
    const usize data_off = wide ? 16 : 12, bytes_off = wide ? 24 : 16;
    struct Want { GuestAddr entry; u8 option; };
    std::vector<Want> wants;
    usize need = 0;
    for (u32 i = 0; i < recd_count; ++i) {
        const GuestAddr entry = recd_params + i * param_size;
        const auto option = m.read_u32(entry + 4);
        const auto vendor = m.read_u32(entry + 8);
        if (recd_params == 0 || option.is_err() || vendor.is_err() || option.value() > 255) {
            m.set_return(ERROR_INVALID_PARAMETER);
            return;
        }
        // The cache answers standard options the lease carried; anything else
        // needs the DHCP-INFORM exchange Windows would send.
        const auto cached = adapter->dhcp_options.find(static_cast<u8>(option.value()));
        if (vendor.value() != 0 || cached == adapter->dhcp_options.end()) {
            dhcp_unimplemented("DHCP-INFORM for an option absent from the host lease (option " +
                               std::to_string(option.value()) + (vendor.value() ? ", vendor" : "") + ")");
            m.set_return(ERROR_NOT_SUPPORTED);
            return;
        }
        wants.push_back({entry, static_cast<u8>(option.value())});
        need += cached->second.size();
    }
    if (buffer == 0 || *have < need) {
        (void)m.write_u32(size_ptr, static_cast<u32>(need));
        m.set_return(kErrorMoreData);
        return;
    }
    usize used = 0;
    for (const auto& want : wants) {
        const auto& data = adapter->dhcp_options.at(want.option);
        const GuestAddr at = buffer + used;
        if ((!data.empty() && m.memory().write_bytes(at, data.data(), data.size()).is_err()) ||
            m.write_ptr(want.entry + data_off, data.empty() ? 0 : at).is_err() ||
            m.write_u32(want.entry + bytes_off, static_cast<u32>(data.size())).is_err()) {
            m.set_return(ERROR_INVALID_PARAMETER);
            return;
        }
        used += data.size();
    }
    (void)m.write_u32(size_ptr, static_cast<u32>(used));
    m.set_return(NO_ERROR);
}

}  // namespace

DllExports create_dhcpcsvc_thunks() {
    DllExports exports;
    // DWORD DhcpCApiInitialize(LPDWORD Version): Windows reports
    // DHCPCAPI_VERSION_V2 and accepts NULL (iphlp_misc_probe dhcp-init*).
    exports.entries.push_back({"DhcpCApiInitialize", 0, [](Marshaler& m) {
        const GuestAddr version = m.arg_ptr(0);
        if (version) (void)m.write_u32(version, kDhcpcapiVersionV2);
        m.set_return(NO_ERROR);
    }, 4});
    // VOID DhcpCApiCleanup(void).
    exports.entries.push_back({"DhcpCApiCleanup", 0, [](Marshaler& m) { m.set_return(0u); }, 0});
    exports.entries.push_back({"DhcpRequestParams", 0, [](Marshaler& m) { impl_DhcpRequestParams(m); }, 44});
    return exports;
}

} // namespace vitreo::winapi
