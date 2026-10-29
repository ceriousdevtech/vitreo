// SPDX-License-Identifier: LGPL-2.1-only
/// @file ndr_proxy.cpp
/// @brief NdrDll* proxy/stub self-registration.
///
/// The tables MIDL emits, in the layout the guest sees them (P = pointer size):
///
///   ProxyFileInfo
///     +0*P  const PCInterfaceProxyVtblList *pProxyVtblList
///     +1*P  const PCInterfaceStubVtblList  *pStubVtblList
///     +2*P  const char                    **pNamesArray
///     +3*P  const IID                     **pDelegatedIIDs
///     +4*P  IID_BS_LOOKUP_ROUTINE           pIIDLookupRoutine
///     +5*P  unsigned short                  TableSize
///     +5*P+2 unsigned short                 TableVersion
///
///   CInterfaceStubVtbl (its leading CInterfaceStubHeader)
///     +0*P  const IID              *piid
///     +1*P  const MIDL_SERVER_INFO *pServerInfo
///     +2*P  unsigned long           DispatchTableCount
///
/// `pProxyFileList` itself is a NULL-terminated array of `ProxyFileInfo *`.

#include "ndr_proxy.hpp"

#include "winapi/common/guid_text.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u32 S_OK_HR                    = 0x00000000u;
constexpr u32 E_INVALIDARG_HR            = 0x80070057u;
constexpr u32 E_UNEXPECTED_HR            = 0x8000FFFFu;

/// A single proxy DLL declaring more interfaces than this is not something
/// MIDL produces; treat it as a corrupt table rather than looping on it.
constexpr u32 MAX_INTERFACES_PER_FILE = 4096;
/// Likewise for the number of proxy files chained off one DLL.
constexpr u32 MAX_PROXY_FILES = 256;

[[nodiscard]] std::wstring widen(std::string_view text) {
    std::wstring result;
    result.reserve(text.size());
    for (const char c : text) {
        result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return result;
}

/// Read a NUL-terminated ANSI string from guest memory, bounded so a missing
/// terminator cannot walk the address space.
[[nodiscard]] std::string read_ansi(Marshaler& m, GuestAddr addr) {
    constexpr u32 MAX_NAME = 512;
    std::string text;
    for (u32 i = 0; i < MAX_NAME; ++i) {
        auto byte = m.read_u8(addr + i);
        if (!byte.is_ok() || byte.value() == 0) break;
        text.push_back(static_cast<char>(byte.value()));
    }
    return text;
}

/// Set a key's default value, creating the key if needed.
[[nodiscard]] bool set_default_value(capsule::Registry& registry,
                                     const std::wstring& subkey,
                                     const std::wstring& value) {
    auto key = registry.open_key(capsule::HKEY_CLASSES_ROOT, subkey,
                                 /*create=*/true);
    if (key.is_err()) return false;
    const bool ok =
        registry.set_value(key.value(), L"",
                           capsule::RegValue{capsule::RegType::Sz, value}).is_ok();
    registry.close_key(key.value());
    return ok;
}

[[nodiscard]] bool set_named_value(capsule::Registry& registry,
                                   const std::wstring& subkey,
                                   const std::wstring& name,
                                   const std::wstring& value) {
    auto key = registry.open_key(capsule::HKEY_CLASSES_ROOT, subkey,
                                 /*create=*/true);
    if (key.is_err()) return false;
    const bool ok =
        registry.set_value(key.value(), name,
                           capsule::RegValue{capsule::RegType::Sz, value}).is_ok();
    registry.close_key(key.value());
    return ok;
}

/// Remove a key and the subkeys it owns. The capsule registry refuses to drop
/// a key that still has children (matching RegDeleteKey on NT), so children go
/// first.
void remove_key_tree(capsule::Registry& registry, const std::wstring& parent,
                     const std::wstring& name) {
    auto key = registry.open_key(capsule::HKEY_CLASSES_ROOT,
                                 parent.empty() ? name : parent + L"\\" + name,
                                 /*create=*/false);
    if (key.is_ok()) {
        if (auto children = registry.enum_keys(key.value()); children.is_ok()) {
            for (const auto& child : children.value()) {
                remove_key_tree(registry,
                                parent.empty() ? name : parent + L"\\" + name,
                                child);
            }
        }
        registry.close_key(key.value());
    }
    auto parent_key = registry.open_key(
        capsule::HKEY_CLASSES_ROOT, parent.empty() ? std::wstring{} : parent,
        /*create=*/false);
    if (parent_key.is_err()) return;
    (void)registry.delete_key(parent_key.value(), name);
    registry.close_key(parent_key.value());
}

} // namespace

bool NdrProxyRegistrar::collect_interfaces(Marshaler& m,
                                           GuestAddr proxy_file_list,
                                           std::vector<ProxyInterface>& out) {
    const u32 P = m.cpu_is_64bit() ? 8u : 4u;

    for (u32 file_index = 0; file_index < MAX_PROXY_FILES; ++file_index) {
        auto file_ptr = m.read_ptr(proxy_file_list +
                                   static_cast<GuestAddr>(file_index) * P);
        if (!file_ptr.is_ok()) return false;
        const GuestAddr info = static_cast<GuestAddr>(file_ptr.value());
        if (info == 0) return true; // NULL terminator: the list is complete.

        auto stub_list  = m.read_ptr(info + P);
        auto names      = m.read_ptr(info + 2 * P);
        auto table_size = m.read_u16(info + 5 * P);
        if (!stub_list.is_ok() || !names.is_ok() || !table_size.is_ok()) {
            return false;
        }
        if (table_size.value() > MAX_INTERFACES_PER_FILE) return false;

        const GuestAddr stubs = static_cast<GuestAddr>(stub_list.value());
        const GuestAddr name_array = static_cast<GuestAddr>(names.value());
        if (stubs == 0) continue;

        for (u32 i = 0; i < table_size.value(); ++i) {
            auto stub_vtbl = m.read_ptr(stubs + static_cast<GuestAddr>(i) * P);
            if (!stub_vtbl.is_ok()) return false;
            const GuestAddr vtbl = static_cast<GuestAddr>(stub_vtbl.value());
            if (vtbl == 0) continue;

            auto iid_ptr = m.read_ptr(vtbl);
            auto methods = m.read_u32(vtbl + 2 * P);
            if (!iid_ptr.is_ok() || !methods.is_ok()) return false;

            ProxyInterface entry;
            if (m.memory().read_bytes(static_cast<GuestAddr>(iid_ptr.value()),
                                      reinterpret_cast<Byte*>(entry.iid.data()),
                                      entry.iid.size()).is_err()) {
                return false;
            }
            entry.method_count = methods.value();
            if (name_array != 0) {
                if (auto name_ptr =
                        m.read_ptr(name_array + static_cast<GuestAddr>(i) * P);
                    name_ptr.is_ok() && name_ptr.value() != 0) {
                    entry.name =
                        read_ansi(m, static_cast<GuestAddr>(name_ptr.value()));
                }
            }
            out.push_back(std::move(entry));
        }
    }
    // More chained files than MIDL can plausibly emit: the pointer we followed
    // was not a proxy file list.
    return false;
}

u32 NdrProxyRegistrar::register_proxy(Marshaler& m) {
    const GuestAddr module     = m.arg_ptr(0);
    const GuestAddr file_list  = m.arg_ptr(1);
    const GuestAddr clsid_ptr  = m.arg_ptr(2);
    if (file_list == 0 || clsid_ptr == 0) {
        VITREO_TRACE("[rpcrt4] NdrDllRegisterProxy: NULL proxy list or CLSID "
                     "-> E_INVALIDARG\n");
        return E_INVALIDARG_HR;
    }

    std::array<u8, 16> proxy_clsid{};
    if (m.memory().read_bytes(clsid_ptr,
                              reinterpret_cast<Byte*>(proxy_clsid.data()),
                              proxy_clsid.size()).is_err()) {
        return E_INVALIDARG_HR;
    }

    // COM resolves the proxy through InprocServer32, so without the module's
    // path there is nothing worth writing.
    const ModuleInfo* info = m_modules.get_module_info_by_handle(module);
    if (info == nullptr || info->path.empty()) {
        VITREO_TRACE("[rpcrt4] NdrDllRegisterProxy: no path for hDll=0x%08x "
                     "-> E_UNEXPECTED\n", static_cast<unsigned>(module));
        return E_UNEXPECTED_HR;
    }

    std::vector<ProxyInterface> interfaces;
    if (!collect_interfaces(m, file_list, interfaces)) {
        VITREO_TRACE("[rpcrt4] NdrDllRegisterProxy: unreadable proxy file "
                     "list at 0x%08x (module=%s) -> E_INVALIDARG\n",
                     static_cast<unsigned>(file_list), info->name.c_str());
        return E_INVALIDARG_HR;
    }

    const std::string clsid_text = format_guid_braced(proxy_clsid);
    const std::wstring clsid_key = L"CLSID\\" + widen(clsid_text);

    // The proxy DLL is its own in-process server for the PSFactoryBuffer class.
    bool ok = set_default_value(m_registry, clsid_key, L"PSFactoryBuffer");
    ok = set_default_value(m_registry, clsid_key + L"\\InprocServer32",
                           widen(info->path)) && ok;
    ok = set_named_value(m_registry, clsid_key + L"\\InprocServer32",
                         L"ThreadingModel", L"Both") && ok;

    for (const auto& entry : interfaces) {
        const std::string iid_text = format_guid_braced(entry.iid);
        const std::wstring iid_key = L"Interface\\" + widen(iid_text);

        ok = set_default_value(m_registry, iid_key, widen(entry.name)) && ok;
        // NumMethods is the interface's dispatch-table size as the proxy DLL
        // itself declares it; it is the only method count available without
        // decoding the vtables.
        char methods[16];
        std::snprintf(methods, sizeof(methods), "%u", entry.method_count);
        ok = set_default_value(m_registry, iid_key + L"\\NumMethods",
                               widen(methods)) && ok;
        ok = set_default_value(m_registry, iid_key + L"\\ProxyStubClsid32",
                               widen(clsid_text)) && ok;

        VITREO_TRACE("[rpcrt4] NdrDllRegisterProxy: interface %s \"%s\" "
                     "methods=%u -> proxy %s\n",
                     iid_text.c_str(), entry.name.c_str(), entry.method_count,
                     clsid_text.c_str());
    }

    VITREO_TRACE("[rpcrt4] NdrDllRegisterProxy module=%s path=\"%s\" "
                 "clsid=%s interfaces=%zu -> %s\n",
                 info->name.c_str(), info->path.c_str(), clsid_text.c_str(),
                 interfaces.size(), ok ? "S_OK" : "E_UNEXPECTED");
    return ok ? S_OK_HR : E_UNEXPECTED_HR;
}

u32 NdrProxyRegistrar::unregister_proxy(Marshaler& m) {
    const GuestAddr file_list = m.arg_ptr(1);
    const GuestAddr clsid_ptr = m.arg_ptr(2);
    if (file_list == 0 || clsid_ptr == 0) return E_INVALIDARG_HR;

    std::array<u8, 16> proxy_clsid{};
    if (m.memory().read_bytes(clsid_ptr,
                              reinterpret_cast<Byte*>(proxy_clsid.data()),
                              proxy_clsid.size()).is_err()) {
        return E_INVALIDARG_HR;
    }

    std::vector<ProxyInterface> interfaces;
    if (!collect_interfaces(m, file_list, interfaces)) return E_INVALIDARG_HR;

    for (const auto& entry : interfaces) {
        remove_key_tree(m_registry, L"Interface",
                        widen(format_guid_braced(entry.iid)));
    }
    const std::string clsid_text = format_guid_braced(proxy_clsid);
    remove_key_tree(m_registry, L"CLSID", widen(clsid_text));

    VITREO_TRACE("[rpcrt4] NdrDllUnregisterProxy clsid=%s interfaces=%zu -> "
                 "S_OK\n", clsid_text.c_str(), interfaces.size());
    return S_OK_HR;
}

// =============================================================================
// NdrDllGetClassObject / IPSFactoryBuffer
// =============================================================================

namespace {

constexpr u32 CLASS_E_CLASSNOTAVAILABLE_HR = 0x80040111u;
constexpr u32 E_NOINTERFACE_HR             = 0x80004002u;
constexpr u32 E_NOTIMPL_HR                 = 0x80004001u;
constexpr u32 E_OUTOFMEMORY_HR             = 0x8007000Eu;
constexpr u32 E_POINTER_HR                 = 0x80004003u;
constexpr u32 S_FALSE_HR                   = 0x00000001u;

/// IID_IUnknown {00000000-0000-0000-C000-000000000046}
constexpr std::array<u8, 16> kIID_IUnknown = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
/// IID_IPSFactoryBuffer {D5F569D0-593B-101A-B569-08002B2DBF7A}
constexpr std::array<u8, 16> kIID_IPSFactoryBuffer = {
    0xD0, 0x69, 0xF5, 0xD5, 0x3B, 0x59, 0x1A, 0x10,
    0xB5, 0x69, 0x08, 0x00, 0x2B, 0x2D, 0xBF, 0x7A};

} // namespace

bool NdrProxyRegistrar::guid_equal(Marshaler& m, GuestAddr a,
                                   GuestAddr b) const {
    std::array<u8, 16> left{};
    std::array<u8, 16> right{};
    if (m.memory().read_bytes(a, reinterpret_cast<Byte*>(left.data()), 16)
            .is_err() ||
        m.memory().read_bytes(b, reinterpret_cast<Byte*>(right.data()), 16)
            .is_err()) {
        return false;
    }
    return left == right;
}

bool NdrProxyRegistrar::guid_is(Marshaler& m, GuestAddr ptr,
                                const std::array<u8, 16>& expected) const {
    std::array<u8, 16> got{};
    if (m.memory().read_bytes(ptr, reinterpret_cast<Byte*>(got.data()), 16)
            .is_err()) {
        return false;
    }
    return got == expected;
}

GuestAddr NdrProxyRegistrar::ensure_psfactory_vtable(Marshaler& m) {
    std::lock_guard lock(m_mutex);
    if (m_psfactory_vtable != 0) return m_psfactory_vtable;

    static constexpr std::array<std::string_view, 5> kMethods{{
        "Vitreo_PSFactory_QueryInterface",
        "Vitreo_PSFactory_AddRef",
        "Vitreo_PSFactory_Release",
        "Vitreo_PSFactory_CreateProxy",
        "Vitreo_PSFactory_CreateStub",
    }};

    std::array<GuestAddr, kMethods.size()> methods{};
    for (usize i = 0; i < kMethods.size(); ++i) {
        auto thunk = m_thunks.get_thunk_address("rpcrt4.dll", kMethods[i]);
        if (thunk.is_err()) return 0;
        methods[i] = thunk.value();
    }

    const u32 pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const usize bytes = static_cast<usize>(pointer_size) * methods.size();
    const usize page = align_up(bytes, static_cast<usize>(0x1000));
    const GuestAddr base =
        m.memory().find_free_gap(winapi::SYNTHETIC_MODULE_BASE_X86, page);
    if (base == 0) return 0;
    if (m.memory()
            .map_region(base, page, MemoryProtection::ReadWrite)
            .is_err()) {
        return 0;
    }
    for (usize i = 0; i < methods.size(); ++i) {
        if (m.write_ptr(base + static_cast<GuestAddr>(i) * pointer_size,
                        methods[i])
                .is_err()) {
            return 0;
        }
    }
    m_psfactory_vtable = base;
    return m_psfactory_vtable;
}

u32 NdrProxyRegistrar::get_class_object(Marshaler& m) {
    const GuestAddr rclsid = m.arg_ptr(0);
    const GuestAddr riid = m.arg_ptr(1);
    const GuestAddr ppv = m.arg_ptr(2);
    const GuestAddr proxy_file_list = m.arg_ptr(3);
    const GuestAddr pclsid = m.arg_ptr(4);
    const GuestAddr factory = m.arg_ptr(5);

    if (ppv != 0) (void)m.write_ptr(ppv, 0);

    if (rclsid == 0 || riid == 0 || ppv == 0 || proxy_file_list == 0 ||
        factory == 0) {
        return E_INVALIDARG_HR;
    }

    // Learn / MIDL: when PROXY_CLSID is omitted the factory CLSID defaults to
    // the first interface IID, and CoGetClassObject may pass an interface IID
    // as rclsid. Accept an exact pclsid match, or rclsid present as an IID in
    // the proxy file list.
    bool clsid_ok = (pclsid != 0 && guid_equal(m, rclsid, pclsid));
    if (!clsid_ok) {
        std::vector<ProxyInterface> interfaces;
        if (collect_interfaces(m, proxy_file_list, interfaces)) {
            for (const auto& entry : interfaces) {
                if (guid_is(m, rclsid, entry.iid)) {
                    clsid_ok = true;
                    break;
                }
            }
        }
    }
    if (!clsid_ok) {
        std::array<u8, 16> got{};
        (void)m.memory().read_bytes(rclsid, reinterpret_cast<Byte*>(got.data()),
                                    16);
        VITREO_TRACE("[rpcrt4] NdrDllGetClassObject: class %s not in proxy "
                     "list -> CLASS_E_CLASSNOTAVAILABLE\n",
                     format_guid_braced(got).c_str());
        return CLASS_E_CLASSNOTAVAILABLE_HR;
    }

    const GuestAddr vtbl = ensure_psfactory_vtable(m);
    if (vtbl == 0) return E_OUTOFMEMORY_HR;

    const u32 pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    // CStdPSFactoryBuffer: lpVtbl, RefCount, pProxyFileList
    if (m.write_ptr(factory, vtbl).is_err()) return E_UNEXPECTED_HR;
    if (m.write_u32(factory + pointer_size, 0).is_err()) return E_UNEXPECTED_HR;
    if (m.write_ptr(factory + pointer_size * 2, proxy_file_list).is_err()) {
        return E_UNEXPECTED_HR;
    }

    {
        std::lock_guard lock(m_mutex);
        m_live_factories.insert(factory);
    }

    if (!guid_is(m, riid, kIID_IUnknown) &&
        !guid_is(m, riid, kIID_IPSFactoryBuffer)) {
        VITREO_TRACE("[rpcrt4] NdrDllGetClassObject: unsupported IID -> "
                     "E_NOINTERFACE\n");
        std::lock_guard lock(m_mutex);
        m_live_factories.erase(factory);
        return E_NOINTERFACE_HR;
    }

    if (m.write_ptr(ppv, factory).is_err()) return E_UNEXPECTED_HR;
    if (m.write_u32(factory + pointer_size, 1).is_err()) return E_UNEXPECTED_HR;

    VITREO_TRACE("[rpcrt4] NdrDllGetClassObject: factory=0x%llx -> S_OK\n",
                 static_cast<unsigned long long>(factory));
    return S_OK_HR;
}

u32 NdrProxyRegistrar::can_unload_now(Marshaler& m) {
    (void)m.arg_ptr(0);
    std::lock_guard lock(m_mutex);
    return m_live_factories.empty() ? S_OK_HR : S_FALSE_HR;
}

void NdrProxyRegistrar::psfactory_query_interface(Marshaler& m) {
    const GuestAddr factory = m.arg_ptr(0);
    const GuestAddr riid = m.arg_ptr(1);
    const GuestAddr ppv = m.arg_ptr(2);
    if (ppv != 0) (void)m.write_ptr(ppv, 0);

    if (factory == 0 || riid == 0 || ppv == 0) {
        m.set_return(E_POINTER_HR);
        return;
    }
    if (!guid_is(m, riid, kIID_IUnknown) &&
        !guid_is(m, riid, kIID_IPSFactoryBuffer)) {
        m.set_return(E_NOINTERFACE_HR);
        return;
    }

    if (m.write_ptr(ppv, factory).is_err()) {
        m.set_return(E_UNEXPECTED_HR);
        return;
    }

    const u32 pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    auto refs = m.read_u32(factory + pointer_size);
    const u32 next = refs.is_ok() ? refs.value() + 1 : 1;
    (void)m.write_u32(factory + pointer_size, next);
    {
        std::lock_guard lock(m_mutex);
        m_live_factories.insert(factory);
    }
    m.set_return(S_OK_HR);
}

void NdrProxyRegistrar::psfactory_add_ref(Marshaler& m) {
    const GuestAddr factory = m.arg_ptr(0);
    if (factory == 0) {
        m.set_return(0);
        return;
    }
    const u32 pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    auto refs = m.read_u32(factory + pointer_size);
    const u32 next = refs.is_ok() ? refs.value() + 1 : 1;
    (void)m.write_u32(factory + pointer_size, next);
    {
        std::lock_guard lock(m_mutex);
        m_live_factories.insert(factory);
    }
    m.set_return(next);
}

void NdrProxyRegistrar::psfactory_release(Marshaler& m) {
    const GuestAddr factory = m.arg_ptr(0);
    if (factory == 0) {
        m.set_return(0);
        return;
    }
    const u32 pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    auto refs = m.read_u32(factory + pointer_size);
    u32 next = 0;
    if (refs.is_ok() && refs.value() > 0) {
        next = refs.value() - 1;
        (void)m.write_u32(factory + pointer_size, next);
    }
    if (next == 0) {
        std::lock_guard lock(m_mutex);
        m_live_factories.erase(factory);
    }
    m.set_return(next);
}

void NdrProxyRegistrar::psfactory_create_proxy(Marshaler& m) {
    // IPSFactoryBuffer::CreateProxy — needs the NDR proxy engine.
    (void)m.arg_ptr(0);
    (void)m.arg_ptr(1);
    (void)m.arg_ptr(2);
    const GuestAddr pp_proxy = m.arg_ptr(3);
    const GuestAddr ppv = m.arg_ptr(4);
    if (pp_proxy != 0) (void)m.write_ptr(pp_proxy, 0);
    if (ppv != 0) (void)m.write_ptr(ppv, 0);
    VITREO_TRACE("[rpcrt4] IPSFactoryBuffer::CreateProxy: NDR proxy engine "
                 "not implemented -> E_NOTIMPL\n");
    m.set_return(E_NOTIMPL_HR);
}

void NdrProxyRegistrar::psfactory_create_stub(Marshaler& m) {
    (void)m.arg_ptr(0);
    (void)m.arg_ptr(1);
    (void)m.arg_ptr(2);
    const GuestAddr pp_stub = m.arg_ptr(3);
    if (pp_stub != 0) (void)m.write_ptr(pp_stub, 0);
    VITREO_TRACE("[rpcrt4] IPSFactoryBuffer::CreateStub: NDR stub engine "
                 "not implemented -> E_NOTIMPL\n");
    m.set_return(E_NOTIMPL_HR);
}

} // namespace vitreo::winapi
