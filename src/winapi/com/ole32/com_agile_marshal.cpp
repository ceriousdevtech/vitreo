// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_agile_marshal.cpp
/// @brief Free-threaded marshaler and agile references (RoGetAgileReference).

#include "com_core.hpp"
#include "ole_guid_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "com_core_internal.hpp"

#include <cstdio>

namespace vitreo::winapi {

// ---------------------------------------------------------------------------
// ole32 completions: free-threaded marshaler, agile references, PROPVARIANT
// copy, and the HGLOBAL view of a memory stream.
// ---------------------------------------------------------------------------

namespace {
// {00000003-0000-0000-C000-000000000046}
constexpr Guid IID_IMARSHAL_FTM{0x00000003, 0x0000, 0x0000,
                                {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
// CLSID_InProcFreeMarshaler {0000033A-0000-0000-C000-000000000046}
constexpr Guid CLSID_INPROC_FREE_MARSHALER{0x0000033A, 0x0000, 0x0000,
                                           {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr u32 MSHCTX_INPROC = 3, MSHCTX_CROSSCTX = 4;
constexpr u32 MSHLFLAGS_NORMAL = 0, MSHLFLAGS_TABLEWEAK = 2;
/// In-process FTM packet: {u32 mshlflags; u32 reserved; u64 pointer}.
/// Produced and consumed only by this marshaler (its unmarshal class is
/// CLSID_InProcFreeMarshaler), so the layout is private to it.
constexpr u32 kFtmPacketSize = 16;
} // namespace

u32 ComCoreManager::co_create_free_threaded_marshaler(Marshaler& m) {
    const GuestAddr outer = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    (void)m.write_ptr(out, 0);
    // Both objects come from the locked, bounds-checked builtin allocator.
    static constexpr std::string_view kInner[] = {
        "Vitreo_FreeThreadedMarshaler_QueryInterface",
        "Vitreo_FreeThreadedMarshaler_AddRef",
        "Vitreo_FreeThreadedMarshaler_Release",
    };
    static constexpr std::string_view kMarshal[] = {
        "Vitreo_FtmMarshal_QueryInterface", "Vitreo_FtmMarshal_AddRef",
        "Vitreo_FtmMarshal_Release", "Vitreo_FtmMarshal_GetUnmarshalClass",
        "Vitreo_FtmMarshal_GetMarshalSizeMax", "Vitreo_FtmMarshal_MarshalInterface",
        "Vitreo_FtmMarshal_UnmarshalInterface", "Vitreo_FtmMarshal_ReleaseMarshalData",
        "Vitreo_FtmMarshal_DisconnectObject",
    };
    const GuestAddr object = allocate_builtin_object(m, kInner);
    const GuestAddr marshal = allocate_builtin_object(m, kMarshal);
    if (object == 0 || marshal == 0) return 0x8007000Eu; // E_OUTOFMEMORY
    {
        std::lock_guard lock(m_com_mutex);
        m_ftm_refs[object] = 1;
        m_ftm_outer[object] = outer;
        m_ftm_marshal[object] = marshal;
        m_ftm_marshal_owner[marshal] = object;
    }
    return m.write_ptr(out, object).is_ok() ? com::S_OK : com::E_INVALIDARG;
}

u32 ComCoreManager::ftm_query_interface(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(2);
    if (iid_ptr == 0 || out == 0) return com::E_INVALIDARG;
    auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    GuestAddr marshal = 0, outer = 0;
    {
        std::lock_guard lock(m_com_mutex);
        auto found = m_ftm_refs.find(self);
        if (found == m_ftm_refs.end()) {
            (void)m.write_ptr(out, 0);
            return E_NOINTERFACE;
        }
        if (guid_equal(iid.value(), IID_IUNKNOWN)) {
            ++found->second;
            return m.write_ptr(out, self).is_ok() ? com::S_OK : com::E_INVALIDARG;
        }
        if (!guid_equal(iid.value(), IID_IMARSHAL_FTM)) {
            (void)m.write_ptr(out, 0);
            return E_NOINTERFACE;
        }
        marshal = m_ftm_marshal[self];
        outer = m_ftm_outer[self];
        if (outer == 0) ++found->second; // IMarshal's IUnknown is the FTM itself
    }
    // The returned IMarshal is AddRef'd through the controlling unknown.
    if (outer != 0) guest_iunknown_add_ref(m, outer);
    return m.write_ptr(out, marshal).is_ok() ? com::S_OK : com::E_INVALIDARG;
}

u32 ComCoreManager::ftm_add_ref(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_ftm_refs.find(m.arg_ptr(0));
    return found == m_ftm_refs.end() ? 0 : ++found->second;
}

u32 ComCoreManager::ftm_release(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_ftm_refs.find(m.arg_ptr(0));
    if (found == m_ftm_refs.end()) return 0;
    const u32 refs = --found->second;
    if (refs == 0) {
        m_ftm_outer.erase(found->first);
        if (auto mk = m_ftm_marshal.find(found->first); mk != m_ftm_marshal.end()) {
            m_ftm_marshal_owner.erase(mk->second);
            m_ftm_marshal.erase(mk);
        }
        m_ftm_refs.erase(found);
    }
    return refs;
}

u32 ComCoreManager::ftm_marshal_method(Marshaler& m, u32 slot) {
    const GuestAddr self = m.arg_ptr(0);
    GuestAddr inner = 0, outer = 0;
    {
        std::lock_guard lock(m_com_mutex);
        auto it = m_ftm_marshal_owner.find(self);
        if (it == m_ftm_marshal_owner.end()) return com::E_INVALIDARG;
        inner = it->second;
        outer = m_ftm_outer[inner];
    }
    const GuestAddr ptr = m.cpu_is_64bit() ? 8u : 4u;
    auto call_stream = [&](GuestAddr stream, u32 vslot, GuestAddr buf, u32 cb) -> u32 {
        // ISequentialStream::Read (slot 3) / Write (slot 4)
        const GuestAddr vtbl = static_cast<GuestAddr>(m.read_ptr(stream).value_or(0));
        const GuestAddr fn = vtbl == 0 ? 0 : static_cast<GuestAddr>(
            m.read_ptr(vtbl + vslot * ptr).value_or(0));
        if (fn == 0 || !m_guest_call) return com::E_INVALIDARG;
        const u64 args[4] = {stream, buf, cb, 0};
        return static_cast<u32>(m_guest_call(fn, args, 4, m.cpu_is_64bit()));
    };
    auto scratch = [&]() -> GuestAddr {
        // A packet-sized guest buffer on the COM scratch arena.
        return reserve_com_scratch(32);
    };
    switch (slot) {
    case 0: { // QueryInterface -> controlling unknown (or the FTM itself)
        if (outer != 0) {
            return guest_query_interface(m, outer, m.arg_ptr(1), m.arg_ptr(2));
        }
        auto iid = m.read_struct<Guid>(m.arg_ptr(1));
        if (iid.is_err() || m.arg_ptr(2) == 0) return com::E_INVALIDARG;
        if (guid_equal(iid.value(), IID_IUNKNOWN) ||
            guid_equal(iid.value(), IID_IMARSHAL_FTM)) {
            std::lock_guard lock(m_com_mutex);
            ++m_ftm_refs[inner];
            (void)m.write_ptr(m.arg_ptr(2),
                              guid_equal(iid.value(), IID_IUNKNOWN) ? inner : self);
            return com::S_OK;
        }
        (void)m.write_ptr(m.arg_ptr(2), 0);
        return E_NOINTERFACE;
    }
    case 1: // AddRef
        if (outer != 0) { guest_iunknown_add_ref(m, outer); return 1; }
        { std::lock_guard lock(m_com_mutex); return ++m_ftm_refs[inner]; }
    case 2: // Release
        if (outer != 0) { guest_iunknown_release(m, outer); return 1; }
        {
            std::lock_guard lock(m_com_mutex);
            auto f = m_ftm_refs.find(inner);
            if (f == m_ftm_refs.end()) return 0;
            const u32 left = --f->second;
            if (left == 0) {
                m_ftm_outer.erase(inner);
                m_ftm_marshal_owner.erase(self);
                m_ftm_marshal.erase(inner);
                m_ftm_refs.erase(f);
            }
            return left;
        }
    case 3:   // GetUnmarshalClass(riid, pv, dwDestContext, pvDestContext, mshlflags, pCid)
    case 4:   // GetMarshalSizeMax(riid, pv, dwDestContext, pvDestContext, mshlflags, pSize)
    case 5: { // MarshalInterface(pStm, riid, pv, dwDestContext, pvDestContext, mshlflags)
        const u32 context = static_cast<u32>(m.arg(slot == 5 ? 4 : 3) & 0xFFFFFFFFu);
        if (context != MSHCTX_INPROC && context != MSHCTX_CROSSCTX) {
            // Out-of-process contexts go to the standard marshaler, which
            // needs cross-process COM Vitreo does not provide.
            std::fprintf(stderr,
                "[vitreo] UNIMPLEMENTED ole32!FreeThreadedMarshaler: "
                "destination context %u (standard marshaling)\n", context);
            return com::E_NOTIMPL;
        }
        if (slot == 3) {
            const GuestAddr cid = m.arg_ptr(6);
            if (cid == 0) return com::E_POINTER;
            return m.memory().write_bytes(cid, reinterpret_cast<const Byte*>(
                       &CLSID_INPROC_FREE_MARSHALER), 16).is_ok()
                ? com::S_OK : com::E_INVALIDARG;
        }
        if (slot == 4) {
            const GuestAddr size = m.arg_ptr(6);
            if (size == 0) return com::E_POINTER;
            return m.write_u32(size, kFtmPacketSize).is_ok() ? com::S_OK
                                                            : com::E_INVALIDARG;
        }
        const GuestAddr stream = m.arg_ptr(1);
        const GuestAddr pv = m.arg_ptr(3);
        const u32 flags = m.arg32(6);
        const GuestAddr buf = scratch();
        if (stream == 0 || buf == 0) return com::E_INVALIDARG;
        // The packet carries the interface pointer itself; a strong marshal
        // holds a reference until it is unmarshaled or released.
        if (flags != MSHLFLAGS_TABLEWEAK) guest_iunknown_add_ref(m, pv);
        (void)m.write_u32(buf, flags);
        (void)m.write_u32(buf + 4, 0);
        (void)m.write_u64(buf + 8, pv);
        const u32 hr = call_stream(stream, 4, buf, kFtmPacketSize);
        if (hr != com::S_OK && flags != MSHLFLAGS_TABLEWEAK) guest_iunknown_release(m, pv);
        return hr;
    }
    case 6:   // UnmarshalInterface(pStm, riid, ppv)
    case 7: { // ReleaseMarshalData(pStm)
        const GuestAddr stream = m.arg_ptr(1);
        const GuestAddr buf = scratch();
        if (stream == 0 || buf == 0) return com::E_INVALIDARG;
        const u32 hr = call_stream(stream, 3, buf, kFtmPacketSize);
        if (hr != com::S_OK) return hr;
        const u32 flags = m.read_u32(buf).value_or(0);
        const GuestAddr pv = static_cast<GuestAddr>(m.read_u64(buf + 8).value_or(0));
        if (slot == 7) {
            if (flags != MSHLFLAGS_TABLEWEAK) guest_iunknown_release(m, pv);
            return com::S_OK;
        }
        const GuestAddr out = m.arg_ptr(3);
        if (out == 0) return com::E_POINTER;
        const u32 qhr = guest_query_interface(m, pv, m.arg_ptr(2), out);
        // A NORMAL marshal is consumed by its one unmarshal.
        if (flags == MSHLFLAGS_NORMAL) guest_iunknown_release(m, pv);
        return qhr;
    }
    case 8: // DisconnectObject(dwReserved): nothing is connected remotely.
        return com::S_OK;
    default:
        return com::E_NOTIMPL;
    }
}

u32 ComCoreManager::ro_get_agile_reference(Marshaler& m) {
    // RoGetAgileReference(options, riid, pUnk, ppAgileReference)
    const GuestAddr unknown = m.arg_ptr(2);
    const GuestAddr out = m.arg_ptr(3);
    if (out == 0) return com::E_POINTER;
    (void)m.write_ptr(out, 0);
    if (unknown == 0) return com::E_INVALIDARG;
    if (m_thunks == nullptr) return com::E_NOTIMPL;

    const char* names[] = {
        "Vitreo_AgileReference_QueryInterface",
        "Vitreo_AgileReference_AddRef",
        "Vitreo_AgileReference_Release",
        "Vitreo_AgileReference_Resolve",
    };
    const GuestAddr pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const GuestAddr object = reserve_com_scratch(
        pointer_size + 4 * pointer_size);
    if (object == 0) return E_OUTOFMEMORY;
    const GuestAddr vtable = object + pointer_size;
    if (m.write_ptr(object, vtable).is_err()) return com::E_INVALIDARG;
    for (usize i = 0; i < 4; ++i) {
        auto fn = m_thunks->get_thunk_address("ole32.dll", names[i]);
        if (fn.is_err() ||
            m.write_ptr(vtable + static_cast<GuestAddr>(i) * pointer_size,
                        fn.value()).is_err()) {
            return com::E_NOTIMPL;
        }
    }
    {
        std::lock_guard lock(m_com_mutex);
        m_agile_refs[object] = 1;
        m_agile_targets[object] = unknown;
    }
    return m.write_ptr(out, object).is_ok() ? com::S_OK : com::E_INVALIDARG;
}

u32 ComCoreManager::agile_query_interface(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    auto found = m_agile_refs.find(self);
    if (found == m_agile_refs.end()) {
        (void)m.write_ptr(out, 0);
        return E_NOINTERFACE;
    }
    ++found->second;
    return m.write_ptr(out, self).is_ok() ? com::S_OK : com::E_INVALIDARG;
}

u32 ComCoreManager::agile_add_ref(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_agile_refs.find(m.arg_ptr(0));
    return found == m_agile_refs.end() ? 0 : ++found->second;
}

u32 ComCoreManager::agile_release(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_agile_refs.find(m.arg_ptr(0));
    if (found == m_agile_refs.end()) return 0;
    const u32 refs = --found->second;
    if (refs == 0) {
        m_agile_targets.erase(found->first);
        m_agile_refs.erase(found);
    }
    return refs;
}

u32 ComCoreManager::agile_resolve(Marshaler& m) {
    // IAgileReference::Resolve(riid, ppvObjectReference)
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) return com::E_POINTER;
    (void)m.write_ptr(out, 0);
    std::lock_guard lock(m_com_mutex);
    auto target = m_agile_targets.find(self);
    if (target == m_agile_targets.end()) return E_NOINTERFACE;
    // One address space: the original pointer is valid in every apartment.
    return m.write_ptr(out, target->second).is_ok() ? com::S_OK
                                                    : com::E_INVALIDARG;
}

} // namespace vitreo::winapi
