// SPDX-License-Identifier: LGPL-2.1-only

#include "com_core.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "filter_graph.hpp"
#include "ole_guid_internal.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "winapi/com/ole32/task_memory.hpp"
#include "winapi/com/rpcrt4/rpcrt4.hpp"
#include "capsule/registry/registry.hpp"
#include "capsule/vfs/vfs.hpp"
#include "ipc/ipc_gate.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

/// @file ole_class_marshal_stubs.cpp
/// @brief OLE class and marshal stubs
///
/// Closed cut from com_core.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// ----- OLE class and marshal stubs

void ComCoreManager::impl_ProgIDFromCLSID(Marshaler& m) {
    constexpr u32 REGDB_E_READREGDB = 0x80040150u;
    constexpr u32 REGDB_E_CLASSNOTREG = 0x80040154u;
    const GuestAddr clsid_ptr = m.arg_ptr(0);
    const GuestAddr out_ptr = m.arg_ptr(1);
    if (clsid_ptr == 0 || out_ptr == 0) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    (void)m.write_ptr(out_ptr, 0);
    if (m_registry == nullptr) {
        m.set_return(REGDB_E_READREGDB);
        return;
    }
    auto clsid = m.read_struct<Guid>(clsid_ptr);
    if (clsid.is_err()) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    const std::string guid = format_guid(clsid.value());
    const std::wstring key_path = L"CLSID\\" +
        std::wstring(guid.begin(), guid.end()) + L"\\ProgID";
    auto key = m_registry->open_key(
        capsule::HKEY_CLASSES_ROOT, key_path, false);
    if (key.is_err()) {
        m.set_return(REGDB_E_CLASSNOTREG);
        return;
    }
    auto value = m_registry->query_value(key.value(), L"");
    m_registry->close_key(key.value());
    if (value.is_err()) {
        m.set_return(REGDB_E_CLASSNOTREG);
        return;
    }
    if (value.value().type != capsule::RegType::Sz ||
        !std::holds_alternative<std::wstring>(value.value().data)) {
        m.set_return(REGDB_E_READREGDB);
        return;
    }
    const std::wstring& wide_progid =
        std::get<std::wstring>(value.value().data);
    const std::string progid(wide_progid.begin(), wide_progid.end());
    const GuestAddr buffer = co_task_mem_alloc_guest(
        (progid.size() + 1) * sizeof(u16));
    if (buffer == 0) {
        m.set_return(0x8007000Eu);
        return;
    }
    if (m.write_wide_string(buffer, progid).is_err() ||
        m.write_ptr(out_ptr, buffer).is_err()) {
        co_task_mem_free_guest(buffer);
        m.set_return(REGDB_E_READREGDB);
        return;
    }
    m.set_return(com::S_OK);
}

u32 ComCoreManager::clsid_from_progid_text(const std::u16string& progid, Guid& clsid) {
    if (m_registry == nullptr || progid.empty()) return com::CO_E_CLASSSTRING;
    const std::wstring key_path(progid.begin(), progid.end());
    auto key = m_registry->open_key(
        capsule::HKEY_CLASSES_ROOT, key_path + L"\\CLSID", false);
    if (key.is_err()) return com::CO_E_CLASSSTRING;
    auto value = m_registry->query_value(key.value(), L"");
    m_registry->close_key(key.value());
    if (value.is_err() || value.value().type != capsule::RegType::Sz ||
        !std::holds_alternative<std::wstring>(value.value().data)) {
        return com::CO_E_CLASSSTRING;
    }
    const std::wstring& wide_clsid = std::get<std::wstring>(value.value().data);
    const std::string clsid_text(wide_clsid.begin(), wide_clsid.end());
    return parse_guid_string(clsid_text, clsid) ? com::S_OK : com::CO_E_CLASSSTRING;
}

void ComCoreManager::impl_CLSIDFromProgID(Marshaler& m) {
    const GuestAddr progid_ptr = m.arg_ptr(0);
    const GuestAddr clsid_ptr = m.arg_ptr(1);
    if (progid_ptr == 0 || clsid_ptr == 0) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    if (m_registry == nullptr) {
        m.set_return(com::CO_E_CLASSSTRING);
        return;
    }
    auto progid = m.read_wide_string(progid_ptr);
    if (progid.is_err() || progid.value().empty()) {
        m.set_return(com::CO_E_CLASSSTRING);
        return;
    }
    const std::wstring key_path(
        progid.value().begin(), progid.value().end());
    auto key = m_registry->open_key(
        capsule::HKEY_CLASSES_ROOT, key_path + L"\\CLSID", false);
    if (key.is_err()) {
        m.set_return(com::CO_E_CLASSSTRING);
        return;
    }
    auto value = m_registry->query_value(key.value(), L"");
    m_registry->close_key(key.value());
    if (value.is_err() || value.value().type != capsule::RegType::Sz ||
        !std::holds_alternative<std::wstring>(value.value().data)) {
        m.set_return(com::CO_E_CLASSSTRING);
        return;
    }
    const std::wstring& wide_clsid =
        std::get<std::wstring>(value.value().data);
    const std::string clsid_text(wide_clsid.begin(), wide_clsid.end());
    Guid clsid{};
    if (!parse_guid_string(clsid_text, clsid)) {
        m.set_return(com::CO_E_CLASSSTRING);
        return;
    }
    const auto result = m.write_struct<Guid>(clsid_ptr, clsid);
    m.set_return(result.is_ok() ? com::S_OK : com::E_INVALIDARG);
}

void ComCoreManager::impl_CoRegisterClassObject(Marshaler& m) {
    const GuestAddr clsid_ptr = m.arg_ptr(0);
    const GuestAddr class_object = m.arg_ptr(1);
    const u32 context = static_cast<u32>(m.arg(2));
    const u32 flags = static_cast<u32>(m.arg(3));
    const GuestAddr cookie_ptr = m.arg_ptr(4);
    if (clsid_ptr == 0 || class_object == 0 || cookie_ptr == 0 ||
        !m_guest_call) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    auto clsid = m.read_struct<Guid>(clsid_ptr);
    if (clsid.is_err()) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    (void)m.write_u32(cookie_ptr, 0);
    (void)guest_iunknown_addref(m, class_object);
    std::lock_guard lock(m_class_objects_mutex);
    const u32 cookie = m_next_class_object_cookie++;
    const bool suspended = (flags & 0x4u) != 0; // REGCLS_SUSPENDED
    std::unique_ptr<ipc::ComClassRegistration> shared_registration;
    if (!m_capsule_root.empty()) {
        ipc::ComClassId class_id{};
        static_assert(sizeof(class_id) == sizeof(Guid));
        std::memcpy(class_id.data(), &clsid.value(), sizeof(Guid));
        shared_registration = std::make_unique<ipc::ComClassRegistration>(
            m_capsule_root, class_id, cookie, class_object, context, flags,
            m.cpu_is_64bit() ? 8u : 4u, suspended);
    }
    m_class_objects.emplace(cookie, ClassObjectRegistration{
        clsid.value(), class_object, context, flags, suspended,
        std::move(shared_registration),
        // The registering thread's TEB identifies its apartment.
        m.has_cpu() ? static_cast<GuestAddr>(m.cpu_is_64bit()
                                                 ? m.cpu()->seg.gs_desc.base
                                                 : m.cpu()->seg.fs_desc.base)
                    : GuestAddr{0}});
    ipc::ipc_log(
        "CoRegisterClassObject clsid=%s object=0x%llx context=0x%x "
        "flags=0x%x cookie=%u suspended=%d",
        format_guid(clsid.value()).c_str(),
        static_cast<unsigned long long>(class_object), context, flags, cookie,
        suspended ? 1 : 0);
    if (m.write_u32(cookie_ptr, cookie).is_err()) {
        m_class_objects.erase(cookie);
        guest_iunknown_release(m, class_object);
        m.set_return(com::E_INVALIDARG);
        return;
    }
    m.set_return(com::S_OK);
}

void ComCoreManager::impl_CoRevokeClassObject(Marshaler& m) {
    const u32 cookie = static_cast<u32>(m.arg(0));
    GuestAddr class_object = 0;
    {
        std::lock_guard lock(m_class_objects_mutex);
        auto found = m_class_objects.find(cookie);
        if (found == m_class_objects.end()) {
            m.set_return(com::E_INVALIDARG);
            return;
        }
        class_object = found->second.class_object;
        m_class_objects.erase(found);
    }
    guest_iunknown_release(m, class_object);
    m.set_return(com::S_OK);
}

void ComCoreManager::impl_CoRegisterPSClsid(Marshaler& m) {
    const GuestAddr iid_ptr = m.arg_ptr(0);
    const GuestAddr clsid_ptr = m.arg_ptr(1);
    if (iid_ptr == 0 || clsid_ptr == 0) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    auto iid = m.read_struct<Guid>(iid_ptr);
    auto clsid = m.read_struct<Guid>(clsid_ptr);
    if (iid.is_err() || clsid.is_err()) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    std::lock_guard lock(m_proxy_stub_mutex);
    m_proxy_stub_clsids[format_guid(iid.value())] = clsid.value();
    m.set_return(com::S_OK);
}

void ComCoreManager::impl_CoGetPSClsid(Marshaler& m) {
    constexpr u32 REGDB_E_IIDNOTREG = 0x80040155u;
    const GuestAddr iid_ptr = m.arg_ptr(0);
    const GuestAddr clsid_ptr = m.arg_ptr(1);
    if (iid_ptr == 0 || clsid_ptr == 0) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    const std::string iid_text = format_guid(iid.value());
    {
        std::lock_guard lock(m_proxy_stub_mutex);
        auto registered = m_proxy_stub_clsids.find(iid_text);
        if (registered != m_proxy_stub_clsids.end()) {
            const auto result = m.write_struct<Guid>(
                clsid_ptr, registered->second);
            m.set_return(result.is_ok() ? com::S_OK : com::E_INVALIDARG);
            return;
        }
    }

    if (m_registry == nullptr) {
        m.set_return(REGDB_E_IIDNOTREG);
        return;
    }
    const std::wstring key_path = L"Interface\\" +
        std::wstring(iid_text.begin(), iid_text.end()) +
        L"\\ProxyStubClsid32";
    auto key = m_registry->open_key(
        capsule::HKEY_CLASSES_ROOT, key_path, false);
    if (key.is_err()) {
        m.set_return(REGDB_E_IIDNOTREG);
        return;
    }
    auto value = m_registry->query_value(key.value(), L"");
    m_registry->close_key(key.value());
    if (value.is_err() || value.value().type != capsule::RegType::Sz ||
        !std::holds_alternative<std::wstring>(value.value().data)) {
        m.set_return(REGDB_E_IIDNOTREG);
        return;
    }
    const std::wstring& wide_clsid =
        std::get<std::wstring>(value.value().data);
    const std::string clsid_text(wide_clsid.begin(), wide_clsid.end());
    Guid clsid{};
    if (!parse_guid_string(clsid_text, clsid)) {
        m.set_return(REGDB_E_IIDNOTREG);
        return;
    }
    const auto result = m.write_struct<Guid>(clsid_ptr, clsid);
    m.set_return(result.is_ok() ? com::S_OK : com::E_INVALIDARG);
}

u32 ComCoreManager::guest_iunknown_addref(Marshaler& m, GuestAddr punk) {
    if (punk == 0 || !m_guest_call) return 0;
    auto vtable_r = m.read_ptr(punk);
    if (!vtable_r.is_ok() || vtable_r.value() == 0) return 0;
    const GuestAddr vtable = static_cast<GuestAddr>(vtable_r.value());
    // IUnknown::AddRef is slot 1 (offset 4 on 32-bit).
    auto method_r = m.read_ptr(vtable + (m.cpu_is_64bit() ? 8u : 4u));
    if (!method_r.is_ok() || method_r.value() == 0) return 0;
    const u64 args[1] = {static_cast<u64>(punk)};
    return static_cast<u32>(m_guest_call(
        static_cast<GuestAddr>(method_r.value()), args, 1, m.cpu_is_64bit()));
}

void ComCoreManager::guest_iunknown_release(Marshaler& m, GuestAddr punk) {
    if (punk == 0 || !m_guest_call) return;
    auto vtable_r = m.read_ptr(punk);
    if (!vtable_r.is_ok() || vtable_r.value() == 0) return;
    const GuestAddr vtable = static_cast<GuestAddr>(vtable_r.value());
    // IUnknown::Release is slot 2 (offset 8 on 32-bit).
    auto method_r = m.read_ptr(vtable + (m.cpu_is_64bit() ? 16u : 8u));
    if (!method_r.is_ok() || method_r.value() == 0) return;
    const u64 args[1] = {static_cast<u64>(punk)};
    (void)m_guest_call(static_cast<GuestAddr>(method_r.value()), args, 1,
                       m.cpu_is_64bit());
}

u32 ComCoreManager::guest_iunknown_qi(Marshaler& m, GuestAddr punk,
                                      GuestAddr iid_ptr, GuestAddr out_ptr) {
    if (punk == 0 || iid_ptr == 0 || out_ptr == 0 || !m_guest_call) {
        return com::E_INVALIDARG;
    }
    auto vtable_r = m.read_ptr(punk);
    if (!vtable_r.is_ok() || vtable_r.value() == 0) return com::E_INVALIDARG;
    const GuestAddr vtable = static_cast<GuestAddr>(vtable_r.value());
    // IUnknown::QueryInterface is slot 0.
    auto method_r = m.read_ptr(vtable);
    if (!method_r.is_ok() || method_r.value() == 0) return com::E_INVALIDARG;
    const u64 args[3] = {static_cast<u64>(punk), static_cast<u64>(iid_ptr),
                         static_cast<u64>(out_ptr)};
    return static_cast<u32>(m_guest_call(
        static_cast<GuestAddr>(method_r.value()), args, 3, m.cpu_is_64bit()));
}

void ComCoreManager::impl_CoMarshalInterThreadInterfaceInStream(Marshaler& m) {
    // HRESULT CoMarshalInterThreadInterfaceInStream(REFIID riid, LPUNKNOWN pUnk,
    //                                               LPSTREAM *ppStm)
    //
    // Same-process stand-in: create an empty IStream and remember (iid, punk)
    // keyed by that stream. CoGetInterfaceAndReleaseStream reconstitutes via
    // QI. This is a same-process handoff, not a real OBJREF.
    const GuestAddr riid_ptr = m.arg_ptr(0);
    const GuestAddr punk = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);

    if (riid_ptr == 0 || punk == 0 || out_ptr == 0) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    (void)m.write_ptr(out_ptr, 0);

    auto iid_r = m.read_struct<Guid>(riid_ptr);
    if (!iid_r.is_ok()) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    if (!m_guest_call) {
        VITREO_TRACE("[ole32] CoMarshalInterThreadInterfaceInStream -> "
                     "E_NOTIMPL (no guest_call)\n");
        m.set_return(0x80004001u); // E_NOTIMPL
        return;
    }

    const GuestAddr stream = create_memory_stream(m, 0, 0);
    if (stream == 0) {
        m.set_return(0x8007000Eu); // E_OUTOFMEMORY
        return;
    }

    (void)guest_iunknown_addref(m, punk);

    {
        std::lock_guard lock(m_interthread_mutex);
        m_interthread_marshals[stream] =
            InterThreadMarshal{iid_r.value(), punk};
    }

    if (m.write_ptr(out_ptr, stream).is_err()) {
        guest_iunknown_release(m, punk);
        std::lock_guard lock(m_interthread_mutex);
        m_interthread_marshals.erase(stream);
        m.set_return(com::E_INVALIDARG);
        return;
    }

    VITREO_TRACE("[ole32] CoMarshalInterThreadInterfaceInStream("
                 "punk=0x%08x) -> stream=0x%08x\n",
                 static_cast<unsigned>(punk), static_cast<unsigned>(stream));
    m.set_return(com::S_OK);
}

void ComCoreManager::impl_CoGetInterfaceAndReleaseStream(Marshaler& m) {
    // HRESULT CoGetInterfaceAndReleaseStream(LPSTREAM pStm, REFIID riid,
    //                                        LPVOID *ppv)
    const GuestAddr stream = m.arg_ptr(0);
    const GuestAddr riid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);

    if (stream == 0 || riid_ptr == 0 || out_ptr == 0) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    (void)m.write_ptr(out_ptr, 0);

    InterThreadMarshal entry{};
    {
        std::lock_guard lock(m_interthread_mutex);
        auto it = m_interthread_marshals.find(stream);
        if (it == m_interthread_marshals.end()) {
            VITREO_TRACE("[ole32] CoGetInterfaceAndReleaseStream("
                         "stream=0x%08x) -> E_INVALIDARG (unknown stream)\n",
                         static_cast<unsigned>(stream));
            // Still release the stream object if it is one of ours.
            guest_iunknown_release(m, stream);
            m.set_return(com::E_INVALIDARG);
            return;
        }
        entry = it->second;
        m_interthread_marshals.erase(it);
    }

    const u32 hr = guest_iunknown_qi(m, entry.punk, riid_ptr, out_ptr);
    // Drop the marshal-time AddRef regardless of QI outcome.
    guest_iunknown_release(m, entry.punk);
    // Release the stream (API contract).
    guest_iunknown_release(m, stream);

    auto out_r = m.read_ptr(out_ptr);
    VITREO_TRACE("[ole32] CoGetInterfaceAndReleaseStream(stream=0x%08x) "
                 "-> hr=0x%08x iface=0x%08x\n",
                 static_cast<unsigned>(stream), hr,
                 static_cast<unsigned>(out_r.value_or(0)));
    m.set_return(hr);
}

void ComCoreManager::impl_CoReleaseMarshalData(Marshaler& m) {
    constexpr u32 STG_E_INVALIDPOINTER = 0x80030009u;
    const GuestAddr stream = m.arg_ptr(0);
    if (stream == 0) {
        m.set_return(STG_E_INVALIDPOINTER);
        return;
    }

    GuestAddr punk = 0;
    {
        std::lock_guard lock(m_interthread_mutex);
        auto found = m_interthread_marshals.find(stream);
        if (found == m_interthread_marshals.end()) {
            VITREO_TRACE("[ole32] CoReleaseMarshalData(stream=0x%08x) "
                         "-> E_INVALIDARG (unknown packet)\n",
                         static_cast<unsigned>(stream));
            m.set_return(com::E_INVALIDARG);
            return;
        }
        punk = found->second.punk;
        m_interthread_marshals.erase(found);
    }
    guest_iunknown_release(m, punk);
    VITREO_TRACE("[ole32] CoReleaseMarshalData(stream=0x%08x) -> S_OK\n",
                 static_cast<unsigned>(stream));
    m.set_return(com::S_OK);
}

void ComCoreManager::impl_OleDuplicateData(Marshaler& m) {
    (void)m.arg(0); (void)m.arg(1); (void)m.arg(2);
    VITREO_TRACE("[ole32] OleDuplicateData -> 0\n");
    m.set_return(0u);
}

void ComCoreManager::impl_ReleaseStgMedium(Marshaler& m) {
    // void ReleaseStgMedium(STGMEDIUM*)
    // https://learn.microsoft.com/en-us/windows/win32/api/ole2/nf-ole2-releasestgmedium
    // STGMEDIUM: DWORD tymed @0, the handle union @4 (x86) / @8 (x64),
    // pUnkForRelease after it. With pUnkForRelease NULL the receiver owns the
    // medium and it is destroyed per tymed; otherwise only the file name /
    // stream / storage reference is dropped and pUnkForRelease is Released.
    // Measured: tymed reads TYMED_NULL afterwards (s7_ole_probe.expected).
    const GuestAddr medium = m.arg_ptr(0);
    if (medium == 0) return;
    const bool is64 = m.cpu_is_64bit();
    const u32 ptr = is64 ? 8u : 4u;
    const u32 tymed = m.read_u32(medium).value_or(0);
    const GuestAddr handle = static_cast<GuestAddr>(m.read_ptr(medium + ptr).value_or(0));
    const GuestAddr unk = static_cast<GuestAddr>(m.read_ptr(medium + 2 * ptr).value_or(0));
    constexpr u32 TYMED_HGLOBAL = 1, TYMED_FILE = 2, TYMED_ISTREAM = 4,
                  TYMED_ISTORAGE = 8, TYMED_GDI = 16, TYMED_MFPICT = 32,
                  TYMED_ENHMF = 64;
    const bool receiver_owns = unk == 0;
    switch (tymed) {
    case TYMED_HGLOBAL:
        if (receiver_owns && handle != 0 && m_globals != nullptr) {
            (void)m_globals->global_free(handle);
        }
        break;
    case TYMED_GDI:
        if (receiver_owns && handle != 0) {
            (void)call_guest_export(m, "gdi32.dll", "DeleteObject", {handle});
        }
        break;
    case TYMED_ENHMF:
        if (receiver_owns && handle != 0) {
            (void)call_guest_export(m, "gdi32.dll", "DeleteEnhMetaFile", {handle});
        }
        break;
    case TYMED_MFPICT:
        // METAFILEPICT {LONG mm, xExt, yExt; HMETAFILE hMF}: hMF at 12/16.
        if (receiver_owns && handle != 0 && m_globals != nullptr) {
            const u64 pict = m_globals->global_lock(handle);
            if (pict != 0) {
                const u64 hmf = m.read_ptr(pict + (is64 ? 16u : 12u)).value_or(0);
                (void)m_globals->global_unlock(handle);
                if (hmf != 0) {
                    (void)call_guest_export(m, "gdi32.dll", "DeleteMetaFile", {hmf});
                }
            }
            (void)m_globals->global_free(handle);
        }
        break;
    case TYMED_FILE:
        if (handle != 0) {
            if (receiver_owns) {
                (void)call_guest_export(m, "kernel32.dll", "DeleteFileW", {handle});
            }
            co_task_mem_free_guest(handle);
        }
        break;
    case TYMED_ISTREAM:
    case TYMED_ISTORAGE:
        guest_iunknown_release(m, handle);
        break;
    default:
        break;
    }
    guest_iunknown_release(m, unk);
    (void)m.write_u32(medium, 0);  // TYMED_NULL
}

void ComCoreManager::impl_CoInitializeSecurity(Marshaler& m) {
    for (int i = 0; i < 9; i++) (void)m.arg(i);
    VITREO_TRACE("[ole32] CoInitializeSecurity -> S_OK\n");
    m.set_return(com::S_OK);
}

void ComCoreManager::impl_CoSetProxyBlanket(Marshaler& m) {
    for (int i = 0; i < 8; i++) (void)m.arg(i);
    VITREO_TRACE("[ole32] CoSetProxyBlanket -> S_OK\n");
    m.set_return(com::S_OK);
}

} // namespace vitreo::winapi
