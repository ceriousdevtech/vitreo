// SPDX-License-Identifier: LGPL-2.1-only
/// @file filter_graph_pin_sample.cpp
/// @brief Guest-visible IPin (output pin) and IMediaSample COM objects of the filter
/// graph.

#include "filter_graph_internal.hpp"
#include "winapi/com/ole32/dshow_host.hpp"

#include <array>
#include <string_view>

namespace vitreo::winapi {

GuestAddr ComCoreManager::ensure_graph_output_pin(Marshaler& m,
                                                  FilterGraphState& st) {
    if (st.output_pin != 0) return st.output_pin;
    if (m_thunks == nullptr) return 0;
    static constexpr std::array<std::string_view, 3> core_names{{
        "Vitreo_FilterGraph_QueryInterface",
        "Vitreo_FilterGraph_AddRef",
        "Vitreo_FilterGraph_Release",
    }};
    std::array<GuestAddr, 3> core{};
    if (!load_filter_graph_thunks(m_thunks, core_names, core)) return 0;
    std::array<GuestAddr, 15> pin_methods{};
    if (!load_filter_graph_thunks(m_thunks, kOutputPinMethodNames, pin_methods)) {
        return 0;
    }
    // {vtable*}{18 IPin slots}, pointer-width (strmif.h: IPin has 18).
    const GuestAddr ps = guest_pointer_size(m);
    const GuestAddr block = reserve_com_scratch((1 + 18) * ps + 16u);
    if (block == 0) return 0;
    const GuestAddr obj = (block + 15u) & ~GuestAddr{15u};
    const GuestAddr vtbl = obj + ps;
    std::array<GuestAddr, 18> vtable{};
    vtable[0] = core[0];
    vtable[1] = core[1];
    vtable[2] = core[2];
    for (usize i = 0; i < pin_methods.size(); ++i) {
        vtable[3 + i] = pin_methods[i];
    }
    (void)m.write_ptr(obj, vtbl);
    write_vtable(m, vtbl, vtable.data(), vtable.size());
    st.output_pin = obj;
    st.output_pin_refs = 1;
    const u32 sid = find_filter_graph_id_by_iface(st.graph_builder_iface);
    {
        std::lock_guard<std::mutex> map_lock(m_filter_graph_map_mutex);
    m_iface_to_filter_graph[obj] = {sid, FilterGraphInterfaceKind::OutputPin};
    }
    return obj;
}

GuestAddr ComCoreManager::ensure_graph_media_sample(Marshaler& m,
                                                    FilterGraphState& st,
                                                    u32 bytes) {
    if (bytes == 0) return 0;
    if (st.sample_iface != 0 && st.sample_buffer_bytes >= bytes) {
        st.sample_data_bytes = bytes;
        return st.sample_iface;
    }
    if (m_thunks == nullptr) return 0;
    if (st.sample_buffer != 0) {
        co_task_mem_free_guest(st.sample_buffer);
        st.sample_buffer = 0;
        st.sample_buffer_bytes = 0;
    }
    st.sample_buffer = co_task_mem_alloc_guest(bytes);
    if (st.sample_buffer == 0) return 0;
    st.sample_buffer_bytes = bytes;
    st.sample_data_bytes = bytes;
    if (st.sample_iface != 0) return st.sample_iface;

    static constexpr std::array<std::string_view, 3> core_names{{
        "Vitreo_FilterGraph_QueryInterface",
        "Vitreo_FilterGraph_AddRef",
        "Vitreo_FilterGraph_Release",
    }};
    std::array<GuestAddr, 3> core{};
    if (!load_filter_graph_thunks(m_thunks, core_names, core)) return 0;
    std::array<GuestAddr, 16> sample_methods{};
    if (!load_filter_graph_thunks(m_thunks, kMediaSampleMethodNames,
                                 sample_methods)) {
        return 0;
    }
    // {vtable*}{19 IMediaSample slots}, pointer-width.
    const GuestAddr ps = guest_pointer_size(m);
    const GuestAddr block = reserve_com_scratch((1 + 19) * ps + 16u);
    if (block == 0) return 0;
    const GuestAddr obj = (block + 15u) & ~GuestAddr{15u};
    const GuestAddr vtbl = obj + ps;
    std::array<GuestAddr, 19> vtable{};
    vtable[0] = core[0];
    vtable[1] = core[1];
    vtable[2] = core[2];
    for (usize i = 0; i < sample_methods.size(); ++i) {
        vtable[3 + i] = sample_methods[i];
    }
    (void)m.write_ptr(obj, vtbl);
    write_vtable(m, vtbl, vtable.data(), vtable.size());
    st.sample_iface = obj;
    st.sample_refs = 1;
    const u32 sid = find_filter_graph_id_by_iface(st.graph_builder_iface);
    {
        std::lock_guard<std::mutex> map_lock(m_filter_graph_map_mutex);
    m_iface_to_filter_graph[obj] = {sid, FilterGraphInterfaceKind::MediaSample};
    }
    return obj;
}

u32 ComCoreManager::filter_graph_output_pin(Marshaler& m, u32 op) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    switch (op) {
    case 0: // Connect
        return E_NOTIMPL;
    case 1: // ReceiveConnection
        return E_FAIL;
    case 2: // Disconnect
        return com::S_OK;
    case 3: { // ConnectedTo
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return E_POINTER;
        GuestAddr pin = 0;
        for (const auto& filter : st->guest_filters) {
            if (filter.input_pin != 0) {
                pin = filter.input_pin;
                break;
            }
        }
        // IPin** out: pointer-width. Learn: the returned pin is AddRef'd.
        if (pin != 0) (void)dshow_host().add_ref(m, pin);
        (void)m.write_ptr(out, pin);
        return pin != 0 ? com::S_OK : VFW_E_NOT_CONNECTED;
    }
    case 4: { // ConnectionMediaType(AM_MEDIA_TYPE* pmt)
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return E_POINTER;
        if (st->media_type == 0) return E_FAIL;
        // The caller owns the copy and frees pbFormat with CoTaskMemFree
        // (FreeMediaType), so the format block is duplicated, never shared.
        const AmMediaTypeLayout& layout = am_media_type_layout(m.cpu_is_64bit());
        for (u32 i = 0; i < layout.size; i += 4) {
            auto v = m.read_u32(st->media_type + i);
            (void)m.write_u32(out + i, v.is_ok() ? v.value() : 0);
        }
        const u32 cb = m.read_u32(st->media_type + layout.cb_format).value_or(0);
        GuestAddr copy = 0;
        if (cb != 0 && st->format_block != 0) {
            copy = co_task_mem_alloc_guest(cb);
            if (copy == 0) {
                (void)m.write_ptr(out + layout.pb_format, 0);
                (void)m.write_u32(out + layout.cb_format, 0);
                return E_OUTOFMEMORY;
            }
            for (u32 i = 0; i < cb; ++i) {
                (void)m.write_u8(copy + i,
                                 m.memory().read_u8(st->format_block + i).value_or(0));
            }
        }
        (void)m.write_ptr(out + layout.punk, 0);
        (void)m.write_ptr(out + layout.pb_format, copy);
        return com::S_OK;
    }
    case 5: { // QueryPinInfo(PIN_INFO* pInfo)
        const GuestAddr info = m.arg_ptr(1);
        if (info == 0) return E_POINTER;
        // PIN_INFO.pFilter must be the owning IBaseFilter, AddRef'd. The
        // graph's video source is not a guest-visible IBaseFilter, so there
        // is nothing correct to put there: fail instead of handing out the
        // IGraphBuilder (whose vtable is not IBaseFilter's).
        const PinInfoLayout& layout = pin_info_layout(m.cpu_is_64bit());
        for (u32 i = 0; i < layout.size; i += 4) (void)m.write_u32(info + i, 0);
        VITREO_QUARTZ_UNIMPLEMENTED_ONCE(
            "IPin::QueryPinInfo: the graph's source output pin has no "
            "IBaseFilter object");
        return E_NOTIMPL;
    }
    case 6: { // QueryDirection
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return E_POINTER;
        (void)m.write_u32(out, PINDIR_OUTPUT);
        return com::S_OK;
    }
    case 7: { // QueryId
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return E_POINTER;
        const GuestAddr id = co_task_mem_alloc_guest(16);
        if (id == 0) return E_FAIL;
        (void)m.write_u32(id, static_cast<u32>(u'O'));
        (void)m.write_ptr(out, id);  // LPWSTR*
        return com::S_OK;
    }
    case 8: // QueryAccept
        return com::S_OK;
    case 9: { // EnumMediaTypes
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return E_POINTER;
        (void)m.write_ptr(out, 0);
        return E_NOTIMPL;
    }
    case 10: // QueryInternalConnections
        return E_NOTIMPL;
    case 11: // EndOfStream
    case 12: // BeginFlush
    case 13: // EndFlush
        return com::S_OK;
    case 14: // NewSegment
        return com::S_OK;
    default:
        return E_NOTIMPL;
    }
}

u32 ComCoreManager::filter_graph_media_sample(Marshaler& m, u32 op) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    switch (op) {
    case 0: { // GetPointer
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return E_POINTER;
        (void)m.write_ptr(out, st->sample_buffer);  // BYTE**
        return st->sample_buffer != 0 ? com::S_OK : E_FAIL;
    }
    case 1: // GetSize
        return st->sample_buffer_bytes;
    // Times and flags follow quartz's CMediaSample, measured on Windows 11
    // x86 and x64 (tools/oracle_probe/dshow_sample_probe{32,64}.expected).
    case 2: { // GetTime(REFERENCE_TIME* pTimeStart, REFERENCE_TIME* pTimeEnd)
        const GuestAddr start = m.arg_ptr(1);
        const GuestAddr end = m.arg_ptr(2);
        if (start == 0 || end == 0) return E_POINTER;
        if (!st->sample_start_valid) return VFW_E_SAMPLE_TIME_NOT_SET;
        (void)m.write_u64(start, static_cast<u64>(st->sample_start));
        if (!st->sample_end_valid) {
            (void)m.write_u64(end, static_cast<u64>(st->sample_start + 1));
            return VFW_S_NO_STOP_TIME;
        }
        (void)m.write_u64(end, static_cast<u64>(st->sample_end));
        return com::S_OK;
    }
    case 3: { // SetTime(REFERENCE_TIME* pTimeStart, REFERENCE_TIME* pTimeEnd)
        const GuestAddr start = m.arg_ptr(1);
        const GuestAddr end = m.arg_ptr(2);
        // A NULL start invalidates both times.
        st->sample_start_valid = start != 0;
        st->sample_end_valid = start != 0 && end != 0;
        if (start != 0) st->sample_start = static_cast<i64>(m.memory().read_u64(start).value_or(0));
        if (st->sample_end_valid) st->sample_end = static_cast<i64>(m.memory().read_u64(end).value_or(0));
        return com::S_OK;
    }
    case 4: // IsSyncPoint
        return st->sample_sync_point ? com::S_OK : com::S_FALSE;
    case 5: // SetSyncPoint(BOOL)
        st->sample_sync_point = m.arg32(1) != 0;
        return com::S_OK;
    case 6: // IsPreroll
        return st->sample_preroll ? com::S_OK : com::S_FALSE;
    case 7: // SetPreroll(BOOL)
        st->sample_preroll = m.arg32(1) != 0;
        return com::S_OK;
    case 8: // GetActualDataLength
        return st->sample_data_bytes;
    case 9: { // SetActualDataLength(long)
        const u32 length = m.arg32(1);
        if (length > st->sample_buffer_bytes) return VFW_E_BUFFER_OVERFLOW;
        st->sample_data_bytes = length;
        return com::S_OK;
    }
    case 10: { // GetMediaType(AM_MEDIA_TYPE** ppMediaType)
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return E_POINTER;
        (void)m.write_ptr(out, 0);
        return com::S_FALSE;
    }
    case 11: // SetMediaType(AM_MEDIA_TYPE*)
        // A dynamic format change the graph's source never makes; a renderer
        // does not call this on an upstream sample.
        VITREO_QUARTZ_UNIMPLEMENTED_ONCE(
            "IMediaSample::SetMediaType: dynamic format change on the graph's sample");
        return E_NOTIMPL;
    case 12: // IsDiscontinuity
        return st->sample_discontinuity ? com::S_OK : com::S_FALSE;
    case 13: // SetDiscontinuity(BOOL)
        st->sample_discontinuity = m.arg32(1) != 0;
        return com::S_OK;
    case 14: { // GetMediaTime(LONGLONG* pTimeStart, LONGLONG* pTimeEnd)
        const GuestAddr start = m.arg_ptr(1);
        const GuestAddr end = m.arg_ptr(2);
        if (start == 0 || end == 0) return E_POINTER;
        if (!st->sample_media_time_valid) return VFW_E_MEDIA_TIME_NOT_SET;
        (void)m.write_u64(start, static_cast<u64>(st->sample_media_start));
        (void)m.write_u64(end, static_cast<u64>(st->sample_media_end));
        return com::S_OK;
    }
    case 15: { // SetMediaTime(LONGLONG* pTimeStart, LONGLONG* pTimeEnd)
        const GuestAddr start = m.arg_ptr(1);
        const GuestAddr end = m.arg_ptr(2);
        st->sample_media_time_valid = start != 0;
        if (start != 0) {
            st->sample_media_start = static_cast<i64>(m.memory().read_u64(start).value_or(0));
            st->sample_media_end = end != 0
                ? static_cast<i64>(m.memory().read_u64(end).value_or(0))
                : st->sample_media_start;
        }
        return com::S_OK;
    }
    default:
        return E_NOTIMPL;
    }
}

} // namespace vitreo::winapi
