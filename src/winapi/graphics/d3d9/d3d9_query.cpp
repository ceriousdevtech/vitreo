// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_query.cpp
/// @brief IDirect3DQuery9 and its creator.
///
/// CreateQuery (IDirect3DDevice9 vtable slot 118) is routed here from
/// `D3D9Manager::dispatch_device` in d3d9_device_dispatch.cpp.
///
/// Contract: Microsoft Learn IDirect3DDevice9::CreateQuery, IDirect3DQuery9,
/// D3DQUERYTYPE, "Queries (Direct3D 9)"; result layouts from SDK
/// shared\d3d9types.h. Measured on an RTX 4080 HAL device, x86 and x64
/// (tools/oracle_probe/d3d9_query_probe{32,64}.winoracle-gpu.expected):
///
/// - Supported there: VCACHE (4), EVENT (8), OCCLUSION (9), TIMESTAMP (10),
///   TIMESTAMPDISJOINT (11), TIMESTAMPFREQ (12) and the D3D9Ex-only
///   MEMORYPRESSURE (19). Everything else is D3DERR_NOTAVAILABLE, for the
///   NULL support probe and for a real create alike. Support is per driver;
///   Vitreo offers the types it measures honestly. VCACHE describes a
///   post-transform vertex cache (D3DDEVINFO_VCACHE) and MEMORYPRESSURE a
///   D3D9Ex residency budget, neither of which the Metal backend exposes, so
///   both stay NOTAVAILABLE -- an allowed answer for every type.
/// - Issue accepts every flag combination (0, END, BEGIN, BEGIN|END) on
///   every type with D3D_OK; BEGIN only means something to the bracketed
///   types (OCCLUSION, TIMESTAMPDISJOINT).
/// - GetData returns S_FALSE while a bracket is building or a result is
///   outstanding, else S_OK. A query never issued is signaled: S_OK. The
///   result is written for any non-null pData; the driver accepted a size
///   one byte short or four bytes long, so Vitreo writes min(size, result).
/// - Occlusion counts what passed depth/stencil (and alpha test, which
///   kills before the count): a 64x64 target gives 4096 / 2048 / 1024 for a
///   full / half / quarter quad, 0 when depth-rejected or off-screen, and
///   the same count with colour or depth writes disabled. END without BEGIN
///   is a ready 0.

#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <memory>

namespace vitreo::winapi {

namespace {

constexpr u32 D3DQUERYTYPE_EVENT = 8;
constexpr u32 D3DQUERYTYPE_OCCLUSION = 9;
constexpr u32 D3DQUERYTYPE_TIMESTAMP = 10;
constexpr u32 D3DQUERYTYPE_TIMESTAMPDISJOINT = 11;
constexpr u32 D3DQUERYTYPE_TIMESTAMPFREQ = 12;
constexpr u32 D3DISSUE_END = 1u << 0;
constexpr u32 D3DISSUE_BEGIN = 1u << 1;
constexpr u32 D3DGETDATA_FLUSH = 1u << 0;
constexpr u32 S_FALSE_D3D = 0x00000001u;
/// GpuTimestamp::gpu_time_ns is host-clock nanoseconds, so TIMESTAMP ticks
/// are nanoseconds and TIMESTAMPFREQ is 1 GHz.
constexpr u64 kTimestampFrequency = 1'000'000'000ull;

/// GetDataSize: sizeof the result (d3d9types.h): BOOL for EVENT and
/// TIMESTAMPDISJOINT, DWORD for OCCLUSION, UINT64 for TIMESTAMP(FREQ).
[[nodiscard]] u32 result_size(u32 type) {
    switch (type) {
    case D3DQUERYTYPE_EVENT:
    case D3DQUERYTYPE_OCCLUSION:
    case D3DQUERYTYPE_TIMESTAMPDISJOINT:
        return 4;
    case D3DQUERYTYPE_TIMESTAMP:
    case D3DQUERYTYPE_TIMESTAMPFREQ:
        return 8;
    default:
        return 0;
    }
}

} // namespace

/// IDirect3DDevice9 vtable slot 118: CreateQuery.
void D3D9Manager::dispatch_device_query(Marshaler& m, usize slot,
                                        u32 self) {
    switch (slot) {
    case 118: { // CreateQuery
        // Learn: CreateQuery(Type, ppQuery). ppQuery may be NULL to probe
        // support — D3D_OK when supported, D3DERR_NOTAVAILABLE when not.
        const u32 type = static_cast<u32>(m.arg(1));
        const GuestAddr out = m.arg_ptr(2);

        bool supported = false;
        switch (type) {
        case D3DQUERYTYPE_EVENT:
        case D3DQUERYTYPE_TIMESTAMP:
        case D3DQUERYTYPE_TIMESTAMPDISJOINT:
            // Each needs a GPU completion point in the command stream.
            supported = m_backend != nullptr && m_backend->available() &&
                        m_backend->record_timestamp() != nullptr;
            break;
        case D3DQUERYTYPE_TIMESTAMPFREQ:
            supported = true;
            break;
        case D3DQUERYTYPE_OCCLUSION:
            supported = m_backend != nullptr && m_backend->available() &&
                        m_backend->create_occlusion_counter() != nullptr;
            break;
        default:
            break;
        }
        if (!supported) {
            VITREO_TRACE("[d3d9] IDirect3DDevice9::CreateQuery(type=%u) -> "
                         "D3DERR_NOTAVAILABLE\n",
                         type);
            m.set_return(D3DERR_NOTAVAILABLE);
            return;
        }
        if (out == 0) {
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
        if (m.write_ptr(out, object).is_err()) {
            destroy_object(object);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        VITREO_TRACE("[d3d9] CreateQuery(type=%u) -> 0x%08x\n", type, object);
        m.set_return(D3D_OK);
        return;
    }
    }
}

void D3D9Manager::restart_occlusion_interval(DeviceState& state) {
    // The open pass counts into the interval it was created with; end it so
    // the next draw opens a pass counting into the new one.
    if (state.render_pass) flush_render_pass(state);
    state.occlusion_interval.reset();
    if (state.counting_queries.empty() || m_backend == nullptr) return;
    state.occlusion_interval = m_backend->create_occlusion_counter();
    for (const u32 id : state.counting_queries) {
        ComObject* entry = find(id, D3D9InterfaceId::Query);
        if (entry == nullptr || !entry->query) continue;
        // A null interval (no counter available) is kept as a hole that
        // GetData reports as a failure, never as a count of zero.
        entry->query->intervals.push_back(state.occlusion_interval);
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
    ComObject* device_entry = find(entry->device, D3D9InterfaceId::Device);
    DeviceState* device =
        device_entry != nullptr ? device_entry->device_state.get() : nullptr;

    switch (slot) {
    case 3: { // GetDevice
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0 || entry->device == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        (void)add_ref(entry->device);
        if (m.write_ptr(out, entry->device).is_err()) {
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
        m.set_return(result_size(query.type));
        return;
    case 6: { // Issue
        const u32 flags = static_cast<u32>(m.arg(1));
        if (device == nullptr) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const bool bracketed = query.type == D3DQUERYTYPE_OCCLUSION ||
                               query.type == D3DQUERYTYPE_TIMESTAMPDISJOINT;
        if ((flags & D3DISSUE_BEGIN) != 0 && bracketed) {
            // BEGIN on a building query restarts its bracket.
            query.intervals.clear();
            query.end_point.reset();
            query.phase = QueryState::Phase::Building;
            if (query.type == D3DQUERYTYPE_OCCLUSION) {
                auto& counting = device->counting_queries;
                if (std::find(counting.begin(), counting.end(), self) ==
                    counting.end()) {
                    counting.push_back(self);
                }
                restart_occlusion_interval(*device);
            }
        }
        if ((flags & D3DISSUE_END) == 0) {
            m.set_return(D3D_OK);
            return;
        }
        query.flushed = false;
        switch (query.type) {
        case D3DQUERYTYPE_OCCLUSION: {
            auto& counting = device->counting_queries;
            const auto found = std::find(counting.begin(), counting.end(), self);
            if (found == counting.end()) {
                // END without BEGIN brackets nothing: a ready zero.
                query.intervals.clear();
                query.value = 0;
                query.phase = QueryState::Phase::Signaled;
                break;
            }
            counting.erase(found);
            restart_occlusion_interval(*device);
            query.phase = QueryState::Phase::Issued;
            break;
        }
        case D3DQUERYTYPE_TIMESTAMPFREQ:
            query.value = kTimestampFrequency;
            query.phase = QueryState::Phase::Signaled;
            break;
        case D3DQUERYTYPE_EVENT:
        case D3DQUERYTYPE_TIMESTAMP:
        case D3DQUERYTYPE_TIMESTAMPDISJOINT:
            // Everything the frontend recorded before this call must be in
            // the backend stream ahead of the point.
            d3d9_cs_flush();
            query.end_point =
                m_backend != nullptr ? m_backend->record_timestamp() : nullptr;
            query.phase = QueryState::Phase::Issued;
            break;
        default:
            break;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 7: { // GetData
        const GuestAddr data = m.arg_ptr(1);
        const u32 size = static_cast<u32>(m.arg(2));
        const u32 flags = static_cast<u32>(m.arg(3));
        if (query.phase == QueryState::Phase::Building) {
            m.set_return(S_FALSE_D3D);
            return;
        }
        if (query.phase == QueryState::Phase::Issued) {
            bool ready = true;
            bool failed = false;
            if (query.type == D3DQUERYTYPE_OCCLUSION) {
                for (const auto& interval : query.intervals) {
                    if (!interval) {
                        failed = true;
                        continue;
                    }
                    const u64 generation = interval->recorded_generation();
                    if (generation != 0 &&
                        !m_backend->generation_complete(generation)) {
                        ready = false;
                    }
                }
            } else if (!query.end_point) {
                failed = true;
            } else {
                ready = query.end_point->ready();
            }
            if (failed) {
                VITREO_TRACE("[d3d9] IDirect3DQuery9::GetData(type=%u) -> "
                             "D3DERR_INVALIDCALL (no GPU measurement)\n",
                             query.type);
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            if (!ready) {
                // D3DGETDATA_FLUSH submits; so does the first poll without
                // it, as a driver does on its own, so a caller spinning
                // without the flag still sees the result arrive.
                if ((flags & D3DGETDATA_FLUSH) != 0 || !query.flushed) {
                    if (device != nullptr && device->render_pass &&
                        query.type == D3DQUERYTYPE_OCCLUSION) {
                        flush_render_pass(*device);
                    }
                    d3d9_cs_flush();
                    m_backend->flush();
                    query.flushed = true;
                }
                m.set_return(S_FALSE_D3D);
                return;
            }
            switch (query.type) {
            case D3DQUERYTYPE_OCCLUSION: {
                u64 samples = 0;
                for (const auto& interval : query.intervals) {
                    samples += interval->samples();
                }
                query.value = std::min<u64>(samples, 0xFFFFFFFFull);
                query.intervals.clear();
                break;
            }
            case D3DQUERYTYPE_EVENT:
                query.value = 1;  // TRUE
                break;
            case D3DQUERYTYPE_TIMESTAMP:
                query.value = query.end_point->gpu_time_ns();
                break;
            case D3DQUERYTYPE_TIMESTAMPDISJOINT:
                // One clock for the whole stream: never disjoint.
                query.value = 0;  // FALSE
                break;
            default:
                break;
            }
            query.end_point.reset();
            query.phase = QueryState::Phase::Signaled;
        } else if (query.type == D3DQUERYTYPE_EVENT) {
            query.value = 1;  // a never-issued event is signaled
        } else if (query.type == D3DQUERYTYPE_TIMESTAMPFREQ) {
            query.value = kTimestampFrequency;
        }
        if (data != 0 && size != 0) {
            const u32 bytes = std::min(size, result_size(query.type));
            if (m.memory()
                    .write_bytes(data, reinterpret_cast<const Byte*>(&query.value),
                                 bytes)
                    .is_err()) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
        }
        m.set_return(D3D_OK);
        return;
    }
    default:
        m.set_return(E_NOTIMPL_D3D);
        return;
    }
}

} // namespace vitreo::winapi
