// SPDX-License-Identifier: LGPL-2.1-only
/// @file filter_graph.cpp
/// @brief FilterGraph object core: interface lookup, QueryInterface / AddRef /
/// Release, destruction and the public free-function wrappers. The rest of
/// the DirectShow Filter Graph Manager lives in the sibling filter_graph_*.cpp
/// files (create, thunks, playback, events, video, guest_renderer, pin_sample).

#include "filter_graph.hpp"
#include "filter_graph_internal.hpp"
#include "winapi/com/ole32/dshow_host.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"

#include <mutex>

namespace vitreo::winapi {

std::optional<ComCoreManager::FilterGraphInterfaceBinding>
ComCoreManager::filter_graph_binding(GuestAddr iface) const {
    std::lock_guard<std::mutex> lock(m_filter_graph_map_mutex);
    auto it = m_iface_to_filter_graph.find(iface);
    if (it == m_iface_to_filter_graph.end()) return std::nullopt;
    return it->second;
}

ComCoreManager::FilterGraphState*
ComCoreManager::find_filter_graph_by_iface(GuestAddr iface) {
    // FilterGraphState lives in an unordered_map node: the pointer stays
    // valid across other graphs' inserts and erases.
    std::lock_guard<std::mutex> lock(m_filter_graph_map_mutex);
    auto it = m_iface_to_filter_graph.find(iface);
    if (it == m_iface_to_filter_graph.end()) {
        return nullptr;
    }
    auto st_it = m_filter_graphs.find(it->second.filter_graph_id);
    return st_it == m_filter_graphs.end() ? nullptr : &st_it->second;
}

const ComCoreManager::FilterGraphState*
ComCoreManager::find_filter_graph_by_iface(GuestAddr iface) const {
    std::lock_guard<std::mutex> lock(m_filter_graph_map_mutex);
    auto it = m_iface_to_filter_graph.find(iface);
    if (it == m_iface_to_filter_graph.end()) {
        return nullptr;
    }
    auto st_it = m_filter_graphs.find(it->second.filter_graph_id);
    return st_it == m_filter_graphs.end() ? nullptr : &st_it->second;
}

GuestAddr ComCoreManager::resolve_filter_graph_iface(
    const FilterGraphState& st, const Guid& iid) const {
    if (guid_equal(iid, IID_IUNKNOWN) ||
        guid_equal(iid, IID_IGRAPHBUILDER) ||
        guid_equal(iid, IID_IFILTERGRAPH)) {
        return st.graph_builder_iface;
    }
    if (guid_equal(iid, IID_IMEDIACONTROL)) {
        return st.media_control_iface;
    }
    if (guid_equal(iid, IID_IBASICAUDIO)) {
        return st.basic_audio_iface;
    }
    if (guid_equal(iid, IID_IMEDIAEVENT) ||
        guid_equal(iid, IID_IMEDIAEVENTEX)) {
        return st.media_event_iface;
    }
    if (guid_equal(iid, IID_IMEDIASEEKING)) {
        return st.media_seeking_iface;
    }
    if (guid_equal(iid, IID_IVIDEOWINDOW)) {
        return st.video_window_iface;
    }
    if (guid_equal(iid, IID_IBASICVIDEO)) {
        return st.basic_video_iface;
    }
    if (guid_equal(iid, IID_IMEDIAPOSITION)) {
        return st.media_position_iface;
    }
    if (guid_equal(iid, IID_IMEDIAEVENTSINK)) {
        return st.media_event_sink_iface;
    }
    return 0;
}

u32 ComCoreManager::filter_graph_query_interface(Marshaler& m) {
    const GuestAddr this_ptr = m.arg_ptr(0);
    const GuestAddr riid_ptr = m.arg_ptr(1);
    const GuestAddr ppv_ptr = m.arg_ptr(2);
    if (riid_ptr == 0 || ppv_ptr == 0) {
        return com::E_INVALIDARG;
    }
    (void)m.write_ptr(ppv_ptr, 0);

    auto* st = find_filter_graph_by_iface(this_ptr);
    if (st == nullptr) {
        return E_NOINTERFACE;
    }

    auto iid_r = m.read_struct<Guid>(riid_ptr);
    if (iid_r.is_err()) {
        return com::E_INVALIDARG;
    }

    const auto binding = filter_graph_binding(this_ptr);
    if (binding.has_value()) {
        const auto kind = binding->kind;
        // The pin and the sample count their own references (their
        // Release never touches the graph's count).
        if (kind == FilterGraphInterfaceKind::OutputPin) {
            if (guid_equal(iid_r.value(), IID_IUNKNOWN) ||
                guid_equal(iid_r.value(), IID_IPIN)) {
                std::atomic_ref<u32>(st->output_pin_refs).fetch_add(1);
                (void)m.write_ptr(ppv_ptr, st->output_pin);
                return com::S_OK;
            }
            return E_NOINTERFACE;
        }
        if (kind == FilterGraphInterfaceKind::MediaSample) {
            if (guid_equal(iid_r.value(), IID_IUNKNOWN) ||
                guid_equal(iid_r.value(), IID_IMEDIASAMPLE)) {
                std::atomic_ref<u32>(st->sample_refs).fetch_add(1);
                (void)m.write_ptr(ppv_ptr, st->sample_iface);
                return com::S_OK;
            }
            return E_NOINTERFACE;
        }
    }

    const GuestAddr out = resolve_filter_graph_iface(*st, iid_r.value());
    if (out == 0) {
        const Guid& iid = iid_r.value();
        graph_diag("qi miss iid=%08x-%04x-%04x\n", iid.Data1, iid.Data2,
                   iid.Data3);
        std::fprintf(stderr,
                     "[ole32] FilterGraph QI E_NOINTERFACE iid=%08x-%04x-%04x\n",
                     iid.Data1, iid.Data2, iid.Data3);
        return E_NOINTERFACE;
    }

    st->ref_count++;
    (void)m.write_ptr(ppv_ptr, out);
    return com::S_OK;
}

u32 ComCoreManager::filter_graph_add_ref(Marshaler& m) {
    const GuestAddr this_ptr = m.arg_ptr(0);
    auto* st = find_filter_graph_by_iface(this_ptr);
    if (st == nullptr) {
        return 1;
    }
    const auto binding = filter_graph_binding(this_ptr);
    if (binding.has_value()) {
        if (binding->kind == FilterGraphInterfaceKind::OutputPin) {
            return std::atomic_ref<u32>(st->output_pin_refs).fetch_add(1) + 1;
        }
        if (binding->kind == FilterGraphInterfaceKind::MediaSample) {
            return std::atomic_ref<u32>(st->sample_refs).fetch_add(1) + 1;
        }
    }
    return ++st->ref_count;
}

ComCoreManager::~ComCoreManager() {
    // Stop the WASAPI engine-period thread before anything it signals through
    // (m_sync, the duplicated event handles) can go away.
    m_audio_event_clock.reset();
    m_filter_graph_clock_stop.store(true, std::memory_order_release);
    m_filter_graph_clock_cv.notify_all();
    if (m_filter_graph_clock_thread.joinable()) {
        m_filter_graph_clock_thread.join();
    }
    if (m_sync != nullptr) {
        for (auto& [id, st] : m_filter_graphs) {
            (void)id;
            if (st.event_handle != 0) {
                (void)m_sync->close_handle(st.event_handle);
                st.event_handle = 0;
            }
        }
    }
}

void ComCoreManager::erase_filter_graph(u32 sid, FilterGraphState& st) {
    const bool was_running = st.filter_state == 2;
    st.filter_state = 0;
    if (was_running) {
        stop_filter_graph_clock();
    }
    std::lock_guard<std::mutex> lock(m_filter_graph_clock_mutex);
    std::lock_guard<std::mutex> map_lock(m_filter_graph_map_mutex);
    hide_filter_graph_video(st);
    if (m_sync != nullptr && st.event_handle != 0) {
        (void)m_sync->close_handle(st.event_handle);
        st.event_handle = 0;
    }
    m_iface_to_filter_graph.erase(st.graph_builder_iface);
    m_iface_to_filter_graph.erase(st.media_control_iface);
    m_iface_to_filter_graph.erase(st.basic_audio_iface);
    m_iface_to_filter_graph.erase(st.media_event_iface);
    m_iface_to_filter_graph.erase(st.media_seeking_iface);
    m_iface_to_filter_graph.erase(st.video_window_iface);
    m_iface_to_filter_graph.erase(st.basic_video_iface);
    m_iface_to_filter_graph.erase(st.media_position_iface);
    m_iface_to_filter_graph.erase(st.media_event_sink_iface);
    if (st.output_pin != 0) m_iface_to_filter_graph.erase(st.output_pin);
    if (st.sample_iface != 0) m_iface_to_filter_graph.erase(st.sample_iface);
    if (st.sample_buffer != 0) {
        co_task_mem_free_guest(st.sample_buffer);
        st.sample_buffer = 0;
    }
    m_filter_graphs.erase(sid);
}

u32 ComCoreManager::find_filter_graph_id_by_iface(GuestAddr iface) const {
    std::lock_guard<std::mutex> lock(m_filter_graph_map_mutex);
    auto it = m_iface_to_filter_graph.find(iface);
    return it == m_iface_to_filter_graph.end() ? 0u : it->second.filter_graph_id;
}

u32 ComCoreManager::filter_graph_release(Marshaler& m) {
    const GuestAddr this_ptr = m.arg_ptr(0);
    const auto binding = filter_graph_binding(this_ptr);
    if (!binding.has_value()) {
        return 0;
    }
    const u32 sid = binding->filter_graph_id;
    const auto kind = binding->kind;
    FilterGraphState* st = find_filter_graph_by_iface(this_ptr);
    if (st == nullptr) {
        return 0;
    }

    const auto release_count = [](u32& refs) -> u32 {
        std::atomic_ref<u32> count(refs);
        u32 value = count.load();
        while (value > 0 && !count.compare_exchange_weak(value, value - 1)) {}
        return value > 0 ? value - 1 : 0;
    };
    if (kind == FilterGraphInterfaceKind::OutputPin) {
        return release_count(st->output_pin_refs);
    }
    if (kind == FilterGraphInterfaceKind::MediaSample) {
        return release_count(st->sample_refs);
    }

    return release_filter_graph_ref(m, sid);
}

u32 ComCoreManager::release_filter_graph_ref(Marshaler& m, u32 sid) {
    auto sit = m_filter_graphs.find(sid);
    if (sit == m_filter_graphs.end()) {
        return 0;
    }
    if (sit->second.ref_count > 0) {
        --sit->second.ref_count;
    }
    const u32 rc = sit->second.ref_count;
    // Filters call back into the graph while it tears down (an output pin's
    // OnInpinDisconnect -> IFilterGraph::Disconnect, QueryFilterInfo's
    // AddRef/Release): those must not start a second teardown.
    if (sit->second.releasing) return rc;
    graph_diag("release sid=%u ref=%u player=%u%s\n", sid, rc,
               sit->second.player != nullptr ? 1u : 0u,
               rc == 0 ? " -> destroyed" : "");
    if (rc == 0) {
        // Measured (dshow_state_probe): the final Release stops a graph
        // that is not stopped (renderers' Stop) before it lets go of them.
        FilterGraphState& st = sit->second;
        st.releasing = true;
        if (st.intelligent && st.filter_state != 0) {
            // Renderers first, as IMediaControl::Stop does.
            (void)intelligent_stop(m, st);
        }
        if (st.filter_state != 0 || st.stream != nullptr) {
            const std::vector<GuestAddr> filters = guest_filter_list(st);
            if (st.filter_state == 2) (void)call_guest_filters_state(m, filters, 5);
            if (st.filter_state != 0) {
                if (st.stream != nullptr) {
                    std::lock_guard<std::mutex> lock(st.stream->mutex);
                    st.stream->target = 0;
                    st.stream->run_pending = false;
                    ++st.stream->generation;
                    ++st.stream->flushes;
                }
                (void)call_guest_filters_state(m, filters, 4);
                (void)set_stream_allocator_committed(m, st.stream.get(), false);
            }
            stop_guest_stream(st);
            if (st.player != nullptr && st.filter_state != 0) st.player->stop();
        }
        // The graph is stopped from here on: filters that disconnect their
        // peers through IFilterGraph::Disconnect during teardown (the WebM
        // decoders assert it succeeds) must not get VFW_E_NOT_STOPPED.
        st.filter_state = 0;
        drop_guest_stream(st);
        if (st.intelligent) std::fprintf(stderr, "[quartz] graph %u final Release\n", sid);
        if (st.intelligent || st.clock != 0) {
            // Measured: the last Release disconnects every pin, then sets
            // each filter's sync source back to NULL.
            disconnect_all_pins(m, st);
            for (const GuestAddr f : guest_filter_list(st)) {
                (void)dshow_host().call(m, f, dshow::Iface::BaseFilter, 8, {0});
            }
            if (st.clock != 0) (void)dshow_host().release(m, st.clock);
            st.clock = 0;
        }
        release_guest_filters(m, st);
        erase_filter_graph(sid, st);
    }
    return rc;
}

u32 filter_graph_query_interface(ComCoreManager& mgr, Marshaler& m) {
    return mgr.filter_graph_query_interface(m);
}

u32 filter_graph_add_ref(ComCoreManager& mgr, Marshaler& m) {
    return mgr.filter_graph_add_ref(m);
}

u32 filter_graph_release(ComCoreManager& mgr, Marshaler& m) {
    return mgr.filter_graph_release(m);
}

u32 filter_graph_render_file(ComCoreManager& mgr, Marshaler& m) {
    return mgr.filter_graph_render_file(m);
}

u32 filter_graph_media_control_get_state(ComCoreManager& mgr, Marshaler& m) {
    return mgr.filter_graph_media_control_get_state(m);
}

u32 filter_graph_media_control_pause(ComCoreManager& mgr, Marshaler& m) {
    return mgr.filter_graph_media_control_pause(m);
}

u32 filter_graph_media_control_stop(ComCoreManager& mgr, Marshaler& m) {
    return mgr.filter_graph_media_control_stop(m);
}

u32 filter_graph_basic_audio_get_volume(ComCoreManager& mgr, Marshaler& m) {
    return mgr.filter_graph_basic_audio_get_volume(m);
}

u32 filter_graph_basic_audio_put_volume(ComCoreManager& mgr, Marshaler& m) {
    return mgr.filter_graph_basic_audio_put_volume(m);
}

u32 filter_graph_basic_audio_get_balance(ComCoreManager& mgr, Marshaler& m) {
    return mgr.filter_graph_basic_audio_get_balance(m);
}

u32 filter_graph_basic_audio_put_balance(ComCoreManager& mgr, Marshaler& m) {
    return mgr.filter_graph_basic_audio_put_balance(m);
}

} // namespace vitreo::winapi
