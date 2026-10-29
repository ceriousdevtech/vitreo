// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_context.cpp
/// @brief Thread context queries: IGlobalOptions, CoGetObjectContext,
/// CoGetContextToken, CoGetApartmentType and IComThreadingInfo.

#include "com_core.hpp"
#include "ole_guid_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "com_core_internal.hpp"

namespace vitreo::winapi {

namespace {

// Microsoft Learn / objidl.h — the three views CoGetObjectContext answers.
constexpr Guid IID_ICOMTHREADINGINFO{
    0x000001CEu, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr Guid IID_ICONTEXTCALLBACK{
    0x000001DAu, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr Guid IID_IOBJCONTEXT{
    0x000001C6u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
// objidl.h APTTYPE / THDTYPE.
constexpr u32 APTTYPE_STA = 0u;
constexpr u32 APTTYPE_MTA = 1u;
constexpr u32 APTTYPE_MAINSTA = 3u;
constexpr u32 APTTYPE_CURRENT = 0xFFFFFFFFu;  // (APTTYPE)-1
constexpr u32 APTTYPEQUALIFIER_NONE = 0u;
constexpr u32 APTTYPEQUALIFIER_IMPLICIT_MTA = 1u;
constexpr u32 THDTYPE_BLOCKMESSAGES = 0u;
constexpr u32 THDTYPE_PROCESSMESSAGES = 1u;

} // namespace

u32 ComCoreManager::global_options_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (iid_ptr == 0 || out_ptr == 0) return com::E_INVALIDARG;
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    auto found = m_global_options_refs.find(object);
    if (found == m_global_options_refs.end() ||
        (!guid_equal(iid.value(), IID_IUNKNOWN) &&
         !guid_equal(iid.value(), IID_IGLOBALOPTIONS))) {
        (void)m.write_ptr(out_ptr, 0);
        return E_NOINTERFACE;
    }
    ++found->second;
    if (m.write_ptr(out_ptr, object).is_err()) {
        --found->second;
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::global_options_add_ref(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_global_options_refs.find(m.arg_ptr(0));
    if (found == m_global_options_refs.end()) return 0;
    return ++found->second;
}

u32 ComCoreManager::global_options_release(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto found = m_global_options_refs.find(m.arg_ptr(0));
    if (found == m_global_options_refs.end()) return 0;
    const u32 refs = --found->second;
    if (refs == 0) m_global_options_refs.erase(found);
    return refs;
}

u32 ComCoreManager::global_options_set(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const u32 property = static_cast<u32>(m.arg(1));
    const u64 value = m.arg(2);
    std::lock_guard lock(m_com_mutex);
    if (!m_global_options_refs.contains(object)) return E_NOINTERFACE;
    // COMGLB_APPID (2) has pointer-to-GUID value semantics and is kept
    // unsupported until its owned storage contract is implemented.
    if (property == 0 || property == 2 || property > 5) {
        return com::E_INVALIDARG;
    }
    m_global_options_values[property] = value;
    VITREO_TRACE("[ole32] IGlobalOptions::Set property=%u value=0x%llx\n",
                 property, static_cast<unsigned long long>(value));
    return com::S_OK;
}

u32 ComCoreManager::global_options_query(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const u32 property = static_cast<u32>(m.arg(1));
    const GuestAddr value_ptr = m.arg_ptr(2);
    if (value_ptr == 0) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    if (!m_global_options_refs.contains(object)) return E_NOINTERFACE;
    if (property == 0 || property == 2 || property > 5) {
        return com::E_INVALIDARG;
    }
    const auto found = m_global_options_values.find(property);
    const u64 value = found == m_global_options_values.end() ? 0 : found->second;
    // Query(GLOBALOPT_PROPERTIES, ULONG_PTR* pdwValue): pointer-width.
    if (m.write_ptr(value_ptr, value).is_err()) {
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

// ============================================================================
// CoGetObjectContext — the calling thread's COM context object.
//
// Microsoft Learn "CoGetObjectContext": returns the context of the calling
// thread's apartment. E_INVALIDARG when ppv is null, CO_E_NOTINITIALIZED when
// the thread never entered an apartment, E_NOINTERFACE when the context does
// not implement the requested interface. Every answer IComThreadingInfo gives
// is something the per-thread ApartmentState here already knows, so this view
// is exact rather than approximated; IContextCallback/IObjContext would have
// to marshal a call into another context and are refused honestly.
// ============================================================================
u32 ComCoreManager::co_get_object_context(Marshaler& m) {
    const GuestAddr iid_ptr = m.arg_ptr(0);
    const GuestAddr ppv_ptr = m.arg_ptr(1);
    if (ppv_ptr == 0) return com::E_INVALIDARG;
    (void)m.write_ptr(ppv_ptr, 0);
    if (iid_ptr == 0) return com::E_INVALIDARG;
    auto iid_r = m.read_struct<Guid>(iid_ptr);
    if (iid_r.is_err()) return com::E_INVALIDARG;
    const Guid& iid = iid_r.value();

    const GuestAddr teb = guest_teb(m);
    if (m_thunks == nullptr) return com::E_NOTIMPL;

    std::lock_guard lock(m_com_mutex);
    auto apt = m_apartments.find(teb);
    if (apt == m_apartments.end() || apt->second.init_count == 0) {
        return CO_E_NOTINITIALIZED;
    }

    if (!guid_equal(iid, IID_IUNKNOWN) &&
        !guid_equal(iid, IID_ICOMTHREADINGINFO)) {
        VITREO_TRACE(
            "[ole32] CoGetObjectContext refused iid=%s: only IUnknown and "
            "IComThreadingInfo are implemented (IContextCallback/IObjContext "
            "would have to dispatch into another apartment)\n",
            format_guid(iid).c_str());
        return E_NOINTERFACE;
    }

    ThreadingInfoState& state = m_threading_infos[teb];
    if (state.object == 0 && !materialize_threading_info_locked(m, teb)) {
        return com::E_NOTIMPL;
    }
    ++state.refs;
    if (m.write_ptr(ppv_ptr, state.object).is_err()) {
        --state.refs;
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

// ULONG_PTR-out CoGetContextToken: the calling thread's context object, as an
// opaque token the caller passes back to CoGetObjectContext-style APIs.
//
// Microsoft Learn "CoGetContextToken": E_INVALIDARG when the pointer is null,
// CO_E_NOTINITIALIZED when the thread never entered an apartment, otherwise
// S_OK and a token. The token is the context object itself -- combase returns
// the IObjContext, and the per-thread object materialised here is that same
// context seen through IComThreadingInfo, so the identity is right even though
// only two of its interfaces are implemented.
//
// This was an unimplemented-export stub returning 0xFFFFFFFF and leaving the
// out-parameter untouched, i.e. handing the caller an uninitialised token
// alongside a failure code no documented path produces.
u32 ComCoreManager::co_get_context_token(Marshaler& m) {
    const GuestAddr token_out = m.arg_ptr(0);
    if (token_out == 0) return com::E_INVALIDARG;
    const GuestAddr teb = guest_teb(m);
    if (m_thunks == nullptr) return com::E_NOTIMPL;

    std::lock_guard lock(m_com_mutex);
    auto apt = m_apartments.find(teb);
    if (apt == m_apartments.end() || apt->second.init_count == 0) {
        return CO_E_NOTINITIALIZED;
    }
    ThreadingInfoState& state = m_threading_infos[teb];
    if (state.object == 0 && !materialize_threading_info_locked(m, teb)) {
        return com::E_NOTIMPL;
    }
    // A token is a borrowed reference: CoGetContextToken explicitly does not
    // AddRef, which is why callers must not Release what it hands back.
    if (m.write_ptr(token_out, state.object).is_err()) {
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

/// Build the per-thread COM context object (vtable + slots) for @p teb.
/// Caller holds m_com_mutex. Returns false if it could not be created.
bool ComCoreManager::materialize_threading_info_locked(Marshaler& m,
                                                       GuestAddr teb) {
    ThreadingInfoState& state = m_threading_infos[teb];
    if (state.object != 0) return true;
    {
        const char* names[] = {
            "Vitreo_ComThreadingInfo_QueryInterface",
            "Vitreo_ComThreadingInfo_AddRef",
            "Vitreo_ComThreadingInfo_Release",
            "Vitreo_ComThreadingInfo_GetCurrentApartmentType",
            "Vitreo_ComThreadingInfo_GetCurrentThreadType",
            "Vitreo_ComThreadingInfo_GetCurrentLogicalThreadId",
            "Vitreo_ComThreadingInfo_SetCurrentLogicalThreadId",
        };
        constexpr usize kSlots = sizeof(names) / sizeof(names[0]);
        const GuestAddr pointer_size = m.cpu_is_64bit() ? 8u : 4u;
        const GuestAddr object = reserve_com_scratch((1 + kSlots) * pointer_size);
        if (object == 0) return false;
        const GuestAddr vtable = object + pointer_size;
        if (m.write_ptr(object, vtable).is_err()) return false;
        for (usize i = 0; i < kSlots; ++i) {
            auto fn = m_thunks->get_thunk_address("ole32.dll", names[i]);
            if (fn.is_err() ||
                m.write_ptr(vtable + static_cast<GuestAddr>(i) * pointer_size,
                            fn.value())
                    .is_err()) {
                return false;
            }
        }
        state.object = object;
        state.teb = teb;
        // Windows hands every thread its own logical thread id. Derive a
        // stable one from the TEB so repeated calls agree.
        state.logical_thread_id = Guid{
            static_cast<u32>(teb & 0xFFFFFFFFu),
            static_cast<u16>((teb >> 32) & 0xFFFFu),
            0x4000u,
            {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};
        m_threading_info_iface_to_teb[object] = teb;
    }
    return true;
}

// Microsoft Learn "CoGetApartmentType": S_OK with both out params filled;
// E_INVALIDARG when either pointer is null; CO_E_NOTINITIALIZED when the
// thread has not entered an apartment, in which case combase still reports
// APTTYPE_CURRENT / APTTYPEQUALIFIER_NONE.
u32 ComCoreManager::co_get_apartment_type(Marshaler& m) {
    const GuestAddr type_out = m.arg_ptr(0);
    const GuestAddr qualifier_out = m.arg_ptr(1);
    if (type_out == 0 || qualifier_out == 0) return com::E_INVALIDARG;

    const GuestAddr teb = guest_teb(m);
    std::lock_guard lock(m_com_mutex);
    auto apt = m_apartments.find(teb);
    if (apt == m_apartments.end() || apt->second.init_count == 0) {
        // Learn APTTYPEQUALIFIER_IMPLICIT_MTA: a thread that never initialized
        // COM is implicitly in the MTA while the process MTA exists.
        if (process_mta_alive_locked()) {
            if (m.write_u32(type_out, APTTYPE_MTA).is_err() ||
                m.write_u32(qualifier_out, APTTYPEQUALIFIER_IMPLICIT_MTA).is_err()) {
                return com::E_INVALIDARG;
            }
            return com::S_OK;
        }
        (void)m.write_u32(type_out, APTTYPE_CURRENT);
        (void)m.write_u32(qualifier_out, APTTYPEQUALIFIER_NONE);
        return CO_E_NOTINITIALIZED;
    }
    u32 type = APTTYPE_MTA;
    if (apt->second.sta) {
        type = (m_main_sta_teb == 0 || m_main_sta_teb == teb) ? APTTYPE_MAINSTA
                                                              : APTTYPE_STA;
        if (m_main_sta_teb == 0) m_main_sta_teb = teb;
    }
    if (m.write_u32(type_out, type).is_err() ||
        m.write_u32(qualifier_out, APTTYPEQUALIFIER_NONE).is_err()) {
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::com_threading_info_query_interface(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (iid_ptr == 0 || out_ptr == 0) return com::E_INVALIDARG;
    auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    auto teb_it = m_threading_info_iface_to_teb.find(object);
    if (teb_it == m_threading_info_iface_to_teb.end() ||
        (!guid_equal(iid.value(), IID_IUNKNOWN) &&
         !guid_equal(iid.value(), IID_ICOMTHREADINGINFO))) {
        (void)m.write_ptr(out_ptr, 0);
        return E_NOINTERFACE;
    }
    auto& state = m_threading_infos[teb_it->second];
    ++state.refs;
    if (m.write_ptr(out_ptr, object).is_err()) {
        --state.refs;
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::com_threading_info_add_ref(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto teb_it = m_threading_info_iface_to_teb.find(m.arg_ptr(0));
    if (teb_it == m_threading_info_iface_to_teb.end()) return 0;
    return ++m_threading_infos[teb_it->second].refs;
}

u32 ComCoreManager::com_threading_info_release(Marshaler& m) {
    std::lock_guard lock(m_com_mutex);
    auto teb_it = m_threading_info_iface_to_teb.find(m.arg_ptr(0));
    if (teb_it == m_threading_info_iface_to_teb.end()) return 0;
    auto& state = m_threading_infos[teb_it->second];
    if (state.refs == 0) return 0;
    // The context object itself is owned by the apartment, so its guest
    // memory stays valid at zero references — only the count is reported.
    return --state.refs;
}

u32 ComCoreManager::com_threading_info_get_apartment_type(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    auto teb_it = m_threading_info_iface_to_teb.find(object);
    if (teb_it == m_threading_info_iface_to_teb.end()) return E_NOINTERFACE;
    auto apt = m_apartments.find(teb_it->second);
    if (apt == m_apartments.end() || apt->second.init_count == 0) {
        return CO_E_NOTINITIALIZED;
    }
    // The first STA in the process is the main STA, exactly as on Windows.
    u32 type = APTTYPE_MTA;
    if (apt->second.sta) {
        type = (m_main_sta_teb == 0 || m_main_sta_teb == teb_it->second)
                   ? APTTYPE_MAINSTA
                   : APTTYPE_STA;
        if (m_main_sta_teb == 0) m_main_sta_teb = teb_it->second;
    }
    if (m.write_u32(out, type).is_err()) return com::E_INVALIDARG;
    return com::S_OK;
}

u32 ComCoreManager::com_threading_info_get_thread_type(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    auto teb_it = m_threading_info_iface_to_teb.find(object);
    if (teb_it == m_threading_info_iface_to_teb.end()) return E_NOINTERFACE;
    auto apt = m_apartments.find(teb_it->second);
    if (apt == m_apartments.end() || apt->second.init_count == 0) {
        return CO_E_NOTINITIALIZED;
    }
    // An STA pumps messages while it waits; an MTA thread blocks.
    const u32 type = apt->second.sta ? THDTYPE_PROCESSMESSAGES
                                     : THDTYPE_BLOCKMESSAGES;
    if (m.write_u32(out, type).is_err()) return com::E_INVALIDARG;
    return com::S_OK;
}

u32 ComCoreManager::com_threading_info_get_logical_thread_id(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    auto teb_it = m_threading_info_iface_to_teb.find(object);
    if (teb_it == m_threading_info_iface_to_teb.end()) return E_NOINTERFACE;
    if (m.write_struct(out, m_threading_infos[teb_it->second].logical_thread_id)
            .is_err()) {
        return com::E_INVALIDARG;
    }
    return com::S_OK;
}

u32 ComCoreManager::com_threading_info_set_logical_thread_id(Marshaler& m) {
    const GuestAddr object = m.arg_ptr(0);
    const GuestAddr in = m.arg_ptr(1);
    if (in == 0) return com::E_INVALIDARG;
    auto guid = m.read_struct<Guid>(in);
    if (guid.is_err()) return com::E_INVALIDARG;
    std::lock_guard lock(m_com_mutex);
    auto teb_it = m_threading_info_iface_to_teb.find(object);
    if (teb_it == m_threading_info_iface_to_teb.end()) return E_NOINTERFACE;
    m_threading_infos[teb_it->second].logical_thread_id = guid.value();
    return com::S_OK;
}

} // namespace vitreo::winapi
