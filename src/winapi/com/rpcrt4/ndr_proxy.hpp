// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file ndr_proxy.hpp
/// @brief NdrDll* — self-registration for MIDL-generated COM proxy/stub DLLs.
///
/// A MIDL-built proxy/stub DLL (`*ps.dll`) implements `DllRegisterServer` by
/// handing rpcrt4 a table describing the interfaces it can marshal. rpcrt4
/// turns that table into the `HKCR\Interface` and `HKCR\CLSID` entries COM
/// later looks up.
///
/// A missing-export stub returns 0xFFFFFFFF and, because it pops no stdcall
/// arguments, leaves the caller's stack 12 bytes out of balance. The
/// following `ret` then jumps to whatever that misaligned slot holds.

#include <vitreo/common.hpp>
#include "capsule/registry/registry.hpp"
#include "winapi/core/thunk_registry.hpp"

#include <mutex>
#include <unordered_set>

namespace vitreo::winapi {

class Marshaler;
class ModuleManager;

/// Registers the interfaces a MIDL proxy/stub DLL declares, and hands out the
/// guest `CStdPSFactoryBuffer` as an `IPSFactoryBuffer` via NdrDllGetClassObject.
///
/// The tables live in guest memory and are read through the Marshaler; every
/// pointer is followed only after the structure that contains it has been read
/// successfully, so a malformed table produces an error rather than a fault.
class NdrProxyRegistrar {
public:
    NdrProxyRegistrar(capsule::Registry& registry, ModuleManager& modules,
                      ThunkRegistry& thunks) noexcept
        : m_registry{registry}, m_modules{modules}, m_thunks{thunks} {}

    NdrProxyRegistrar(const NdrProxyRegistrar&) = delete;
    NdrProxyRegistrar& operator=(const NdrProxyRegistrar&) = delete;

    /// NdrDllRegisterProxy(HMODULE hDll, const ProxyFileInfo **pProxyFileList,
    ///                     const CLSID *pclsid)
    [[nodiscard]] u32 register_proxy(Marshaler& m);

    /// NdrDllUnregisterProxy(HMODULE, const ProxyFileInfo **, const CLSID *)
    [[nodiscard]] u32 unregister_proxy(Marshaler& m);

    /// NdrDllGetClassObject(REFCLSID, REFIID, void **ppv,
    ///                      const ProxyFileInfo **, const CLSID *,
    ///                      CStdPSFactoryBuffer *)
    ///
    /// Learn: returns S_OK and an IPSFactoryBuffer. Vitreo initializes the
    /// guest `CStdPSFactoryBuffer` the proxy DLL passes in and points its
    /// vtable at Vitreo_PSFactory_* thunks. CreateProxy/CreateStub still fail
    /// honestly (E_NOTIMPL) — there is no NDR wire engine yet.
    [[nodiscard]] u32 get_class_object(Marshaler& m);

    /// NdrDllCanUnloadNow(CStdPSFactoryBuffer *) — S_OK when no outstanding
    /// factory references remain.
    [[nodiscard]] u32 can_unload_now(Marshaler& m);

    // IPSFactoryBuffer vtable entry points (This = CStdPSFactoryBuffer*).
    void psfactory_query_interface(Marshaler& m);
    void psfactory_add_ref(Marshaler& m);
    void psfactory_release(Marshaler& m);
    void psfactory_create_proxy(Marshaler& m);
    void psfactory_create_stub(Marshaler& m);

private:
    /// One interface as described by a proxy file's stub vtable.
    struct ProxyInterface {
        std::array<u8, 16> iid{};
        u32                method_count{0};
        std::string        name;
    };

    /// Walk the NULL-terminated ProxyFileInfo list and collect every interface
    /// it declares. Returns false when the table cannot be read.
    [[nodiscard]] bool collect_interfaces(Marshaler& m,
                                          GuestAddr proxy_file_list,
                                          std::vector<ProxyInterface>& out);

    /// Build (once) the guest IPSFactoryBufferVtbl of Vitreo thunk addresses.
    [[nodiscard]] GuestAddr ensure_psfactory_vtable(Marshaler& m);

    [[nodiscard]] bool guid_equal(Marshaler& m, GuestAddr a, GuestAddr b) const;
    [[nodiscard]] bool guid_is(Marshaler& m, GuestAddr ptr,
                               const std::array<u8, 16>& expected) const;

    capsule::Registry& m_registry;
    ModuleManager&     m_modules;
    ThunkRegistry&     m_thunks;

    std::mutex   m_mutex;
    GuestAddr    m_psfactory_vtable{0};
    /// Guest CStdPSFactoryBuffer addresses that still hold a live refcount.
    std::unordered_set<GuestAddr> m_live_factories;
};

} // namespace vitreo::winapi
