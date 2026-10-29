#pragma once
/// @file dnsapi.hpp
/// @brief DNSAPI.DLL — DnsQuery_W / DnsRecordListFree.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vitreo::winapi {

class Marshaler;

class DnsapiManager {
public:
    DnsapiManager() = default;
    ~DnsapiManager();

    void impl_DnsQuery_W(Marshaler& m);
    void impl_DnsRecordListFree(Marshaler& m);
    void impl_DnsFree(Marshaler& m);

    /// DNS_SERVICE_INSTANCE constructors. Layout measured with cl against
    /// Windows SDK 10.0.26100.0 windns.h (x64 size 72, x86 size 40).
    void impl_DnsServiceConstructInstance(Marshaler& m);
    void impl_DnsServiceCopyInstance(Marshaler& m);
    void impl_DnsServiceFreeInstance(Marshaler& m);
    void impl_DnsServiceBrowse(Marshaler& m);
    void impl_DnsServiceBrowseCancel(Marshaler& m);
    void impl_DnsServiceResolve(Marshaler& m);
    void impl_DnsServiceResolveCancel(Marshaler& m);
    void impl_DnsServiceRegister(Marshaler& m);
    void impl_DnsServiceDeRegister(Marshaler& m);
    void impl_DnsServiceRegisterCancel(Marshaler& m);

    /// Nested guest call. Same shape as Kernel32ExtManager::GuestCall.
    /// Unset means a resolve that would need a callback fails instead of
    /// reporting DNS_REQUEST_PENDING with nobody to deliver the instance to.
    using GuestCall = std::function<u64(GuestAddr, const u64*, usize, bool)>;
    void set_guest_call(GuestCall call) { m_guest_call = std::move(call); }

private:
    [[nodiscard]] GuestAddr alloc_guest(Marshaler& m, u32 bytes);
    void free_guest(Marshaler& m, GuestAddr addr);
    [[nodiscard]] GuestAddr build_service_instance(
        Marshaler& m, const std::vector<u16>& name, const std::vector<u16>& host,
        const u8* ip4, const u8* ip6, u16 port, u16 priority, u16 weight,
        const std::vector<std::vector<u16>>& keys,
        const std::vector<std::vector<u16>>& values, u32 interface_index);

    struct LiveQuery {
        void* ref = nullptr;
        GuestAddr instance = 0;
    };

    std::mutex m_mu;
    std::unordered_map<GuestAddr, u32> m_allocs;
    std::unordered_set<u64> m_resolve_cancels;
    std::unordered_map<u64, LiveQuery> m_live;
    GuestAddr m_next_base = 0x6F800000u;
    u64 m_next_cancel = 1;
    GuestCall m_guest_call;
};

DllExports create_dnsapi_thunks(DnsapiManager& mgr);

} // namespace vitreo::winapi
