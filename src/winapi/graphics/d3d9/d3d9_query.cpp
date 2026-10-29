// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_query.cpp
/// @brief IDirect3DQuery9 and its creator.
///
/// CreateQuery (IDirect3DDevice9 vtable slot 118) is routed here from
/// `D3D9Manager::dispatch_device` in d3d9_device_dispatch.cpp.

#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>

#include <memory>

namespace vitreo::winapi {

/// IDirect3DDevice9 vtable slot 118: CreateQuery.
void D3D9Manager::dispatch_device_query(Marshaler& m, usize slot,
                                        u32 self) {
    switch (slot) {
    case 118: { // CreateQuery
        // Learn: CreateQuery(Type, ppQuery). ppQuery may be NULL to probe
        // support — return D3D_OK when supported, D3DERR_NOTAVAILABLE when not.
        // Only D3DQUERYTYPE_EVENT (8) is implemented. Occlusion (9) and other
        // query types stay NOTAVAILABLE: the Metal backend has no honest
        // visibility/pixel-count result, and faking 0 or max DWORD would
        // mislead occlusion culling.
        constexpr u32 D3DQUERYTYPE_EVENT = 8;
        const u32 type = static_cast<u32>(m.arg(1));
        const GuestAddr out = m.arg_ptr(2);

        if (type != D3DQUERYTYPE_EVENT) {
            if (out != 0) (void)m.write_u32(out, 0);
            VITREO_TRACE("[d3d9] IDirect3DDevice9::CreateQuery(type=%u) -> "
                         "D3DERR_NOTAVAILABLE\n",
                         type);
            m.set_return(D3DERR_NOTAVAILABLE);
            return;
        }
        if (out == 0) {
            // Support probe for EVENT (Learn: ppQuery may be NULL).
            VITREO_TRACE("[d3d9] CreateQuery(EVENT) support probe -> D3D_OK\n");
            m.set_return(D3D_OK);
            return;
        }

        const u32 object = alloc_com_object(D3D9InterfaceId::Query, self);
        if (object == 0) {
            m.set_return(E_OUTOFMEMORY_D3D);
            return;
        }
        ComObject* query_entry = find(object, D3D9InterfaceId::Query);
        if (query_entry == nullptr) {
            destroy_object(object);
            m.set_return(E_OUTOFMEMORY_D3D);
            return;
        }
        query_entry->query = std::make_unique<QueryState>();
        query_entry->query->type = type;
        if (m.write_u32(out, object).is_err()) {
            destroy_object(object);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        VITREO_TRACE("[d3d9] CreateQuery(EVENT) -> 0x%08x\n", object);
        m.set_return(D3D_OK);
        return;
    }
    }
}

void D3D9Manager::dispatch_query(Marshaler& m, usize slot, u32 self) {
    if (dispatch_unknown(m, slot, self, D3D9InterfaceId::Query)) return;

    ComObject* entry = find(self, D3D9InterfaceId::Query);
    if (entry == nullptr || !entry->query) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    QueryState& query = *entry->query;

    constexpr u32 D3DQUERYTYPE_EVENT = 8;
    constexpr u32 D3DISSUE_END = 1u << 0;
    constexpr u32 D3DISSUE_BEGIN = 1u << 1;
    constexpr u32 S_FALSE = 0x00000001u;

    switch (slot) {
    case 3: { // GetDevice
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0 || entry->device == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        (void)add_ref(entry->device);
        if (m.write_u32(out, entry->device).is_err()) {
            (void)release(entry->device);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 4: // GetType
        m.set_return(static_cast<u64>(query.type));
        return;
    case 5: // GetDataSize
        // EVENT reports sizeof(BOOL).
        m.set_return(query.type == D3DQUERYTYPE_EVENT ? 4u : 0u);
        return;
    case 6: { // Issue
        const u32 flags = static_cast<u32>(m.arg(1));
        if (query.type == D3DQUERYTYPE_EVENT) {
            // EVENT only accepts D3DISSUE_END.
            if ((flags & D3DISSUE_BEGIN) != 0 || (flags & D3DISSUE_END) == 0) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            query.issued = true;
            m.set_return(D3D_OK);
            return;
        }
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    case 7: { // GetData
        const GuestAddr data = m.arg_ptr(1);
        const u32 size = static_cast<u32>(m.arg(2));
        (void)m.arg(3);  // dwGetDataFlags — FLUSH is a no-op for CPU-side EVENT

        if (query.type != D3DQUERYTYPE_EVENT) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (!query.issued) {
            // No Issue(END) yet — not ready.
            m.set_return(S_FALSE);
            return;
        }
        // Vitreo's command stream is host-synchronous relative to the guest:
        // once Issue(END) returns, the event has completed.
        if (data != 0) {
            if (size < 4 || m.write_u32(data, 1u /* TRUE */).is_err()) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
        }
        query.issued = false;
        m.set_return(D3D_OK);
        return;
    }
    default:
        m.set_return(E_NOTIMPL_D3D);
        return;
    }
}

} // namespace vitreo::winapi
