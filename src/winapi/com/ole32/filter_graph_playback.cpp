// SPDX-License-Identifier: LGPL-2.1-only
/// @file filter_graph_playback.cpp
/// @brief Transport and render interfaces of the Filter Graph Manager: RenderFile,
/// IMediaControl, IBasicAudio, IMediaSeeking and IMediaPosition.

#include "capsule/vfs/vfs.hpp"
#include "filter_graph_internal.hpp"
#include "winapi/com/ole32/dshow_host.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace vitreo::winapi {

u32 ComCoreManager::filter_graph_render_file(Marshaler& m) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) {
        return com::E_INVALIDARG;
    }
    const GuestAddr file_ptr = m.arg_ptr(1);
    const GuestAddr playlist_ptr = m.arg_ptr(2);
    if (file_ptr == 0) return E_POINTER;
    if (playlist_ptr != 0) return com::E_INVALIDARG;
    auto guest_path = m.read_wide_string(file_ptr);
    if (guest_path.is_err() || guest_path.value().empty()) {
        return com::E_INVALIDARG;
    }
    if (st->filter_state != 0) return 0x80040224u;  // VFW_E_NOT_STOPPED
    if (m_vfs == nullptr) return VFW_E_NOT_FOUND;
    {
        auto resolved = m_vfs->resolve_host_path(guest_path.value());
        std::error_code exists_error;
        if (resolved.is_err() ||
            !std::filesystem::is_regular_file(resolved.value(), exists_error)) {
            std::fprintf(stderr, "[ole32] RenderFile VFW_E_NOT_FOUND path='%s'\n",
                         guest_path.value().c_str());
            return VFW_E_NOT_FOUND;  // measured: a missing file
        }
        // Intelligent Connect: with filters already in the graph, Windows
        // builds the graph from them (measured, dshow_renderfile_probe).
        std::u16string wide_path;
        for (GuestAddr at = file_ptr; wide_path.size() < 0x8000u; at += 2) {
            const u16 unit = m.memory().read_u16(at).value_or(0);
            if (unit == 0) break;
            wide_path.push_back(static_cast<char16_t>(unit));
        }
        const bool matroska = [&] {
            // EBML magic: WebM / Matroska, which no stock Windows source or
            // splitter handles (measured: an empty graph fails it with
            // VFW_E_UNSUPPORTED_STREAM).
            std::ifstream probe(resolved.value(), std::ios::binary);
            unsigned char magic[4] = {0, 0, 0, 0};
            probe.read(reinterpret_cast<char*>(magic), 4);
            return probe.gcount() == 4 && magic[0] == 0x1A && magic[1] == 0x45 &&
                   magic[2] == 0xDF && magic[3] == 0xA3;
        }();
        if (!st->guest_filters.empty()) {
            const u32 connect_hr = intelligent_render_file(m, *st, wide_path);
            std::fprintf(stderr, "[ole32] RenderFile intelligent connect path='%s' -> 0x%x\n",
                         guest_path.value().c_str(), connect_hr);
            if (static_cast<i32>(connect_hr) >= 0 || matroska) return connect_hr;
            // The graph's filters take nothing: the host decoder stands in
            // for the stock Windows decoders below.
        } else if (matroska) {
            std::fprintf(stderr, "[ole32] RenderFile VFW_E_UNSUPPORTED_STREAM (Matroska/WebM, "
                                 "no filter in the graph) path='%s'\n",
                         guest_path.value().c_str());
            return 0x80040265u;  // VFW_E_UNSUPPORTED_STREAM
        }
    }
    if (m_vfs == nullptr) return VFW_E_NOT_FOUND;
    auto host_path = m_vfs->resolve_host_path(guest_path.value());
    std::error_code file_error;
    if (host_path.is_err() ||
        !std::filesystem::is_regular_file(
            host_path.is_ok() ? host_path.value() : std::string{},
            file_error)) {
        VITREO_TRACE("[ole32] IGraphBuilder::RenderFile not found path='%s'\n",
                     guest_path.value().c_str());
        std::fprintf(stderr,
                     "[ole32] RenderFile VFW_E_NOT_FOUND path='%s'\n",
                     guest_path.value().c_str());
        return VFW_E_NOT_FOUND;
    }

    auto player = audio::create_media_player();
    if (player == nullptr || !player->open(host_path.value())) {
        VITREO_TRACE("[ole32] IGraphBuilder::RenderFile -> "
                     "VFW_E_CANNOT_RENDER path='%s'\n",
                     guest_path.value().c_str());
        std::fprintf(stderr,
                     "[ole32] RenderFile VFW_E_CANNOT_RENDER path='%s'%s\n",
                     guest_path.value().c_str(),
                     player == nullptr
                         ? " (ffmpeg is not installed)"
                         : "");
        return VFW_E_CANNOT_RENDER;
    }
    player->set_volume(st->volume);
    player->set_balance(st->balance);
    graph_diag("render sid=%u replacing_player=%u path='%s'\n",
               find_filter_graph_id_by_iface(m.arg_ptr(0)),
               st->player != nullptr ? 1u : 0u, guest_path.value().c_str());
    // A new source replaces the old one: its streaming thread goes first.
    drop_guest_stream(*st);
    st->player = std::shared_ptr<audio::IMediaPlayer>(std::move(player));
    st->filter_state = 0;
    st->has_pending_event = false;
    st->video_renderer_connected = st->player->has_video();
    st->guest_video_connected = false;
    st->last_video_frame = ~u64{0};
    connect_guest_video_renderer(m, *st);
    u32 native_w = 0;
    u32 native_h = 0;
    st->player->native_video_size(native_w, native_h);
    st->native_video_width = static_cast<i32>(native_w);
    st->native_video_height = static_cast<i32>(native_h);
    st->source_left = 0;
    st->source_top = 0;
    st->source_width = st->native_video_width;
    st->source_height = st->native_video_height;
    st->dest_left = 0;
    st->dest_top = 0;
    st->dest_width = st->native_video_width;
    st->dest_height = st->native_video_height;
    st->default_source = true;
    st->default_dest = true;
    if (st->video_width == 0 && st->native_video_width != 0) {
        st->video_width = st->native_video_width;
        st->video_height = st->native_video_height;
    }
    VITREO_TRACE("[ole32] IGraphBuilder::RenderFile -> S_OK path='%s' "
                 "video_renderer=%u\n",
                 guest_path.value().c_str(),
                 st->video_renderer_connected ? 1u : 0u);
    std::fprintf(stderr,
                 "[ole32] RenderFile S_OK path='%s' video=%u size=%dx%d "
                 "duration=%.2fs\n",
                 guest_path.value().c_str(),
                 st->video_renderer_connected ? 1u : 0u,
                 st->native_video_width, st->native_video_height,
                 static_cast<double>(st->player->duration_100ns()) /
                     10'000'000.0);
    return com::S_OK;
}

/// IMemAllocator::Commit (slot 5) / Decommit (slot 6) on the renderer's
/// allocator: CBaseOutputPin::Active / Inactive, i.e. the source going
/// Paused from Stopped and back to Stopped.
u32 ComCoreManager::set_stream_allocator_committed(Marshaler& m,
                                                   const FilterGraphStream* s,
                                                   bool commit) {
    if (s == nullptr || s->allocator == 0) return com::S_OK;
    const u64 args[] = {s->allocator};
    return guest_vtable_call(m, s->allocator, commit ? 5u : 6u, args, 1);
}

bool ComCoreManager::seek_graph(Marshaler& m, FilterGraphState& st, i64 position) {
    FilterGraphStream* s = st.stream.get();
    bool active = false;
    if (s != nullptr) {
        std::lock_guard<std::mutex> lock(s->mutex);
        active = s->thread_alive;
    }
    if (!active) return st.player->seek_100ns(position);
    // Learn "Flushing Data": BeginFlush downstream (a blocked Receive
    // returns and the renderer discards), reposition, EndFlush, then the
    // source delivers again from the new position (a new preroll in
    // Paused) with the discontinuity flag set.
    const u64 pin[] = {s->input_pin};
    (void)guest_vtable_call(m, s->input_pin, 15, pin, 1);  // IPin::BeginFlush
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        ++s->flushes;
        ++s->generation;
        s->prerolled = false;
        s->halted = true;  // nothing is sent while flushing
        s->eos_sent = false;
        s->discontinuity = true;
        s->last_frame = ~u64{0};
    }
    const bool ok = st.player->seek_100ns(position);
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->segment_start = st.player->position_100ns();
    }
    (void)guest_vtable_call(m, s->input_pin, 16, pin, 1);  // IPin::EndFlush
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->halted = false;
        ++s->generation;
    }
    s->cv.notify_all();
    return ok;
}

u32 ComCoreManager::guest_graph_pause(Marshaler& m, FilterGraphState& st,
                                      bool from_running) {
    const std::vector<GuestAddr> filters = guest_filter_list(st);
    FilterGraphStream* s = st.stream.get();
    const auto fail_to_stopped = [&](u32 hr) {
        (void)call_guest_filters_state(m, filters, 4);
        if (!from_running) (void)set_stream_allocator_committed(m, s, false);
        stop_guest_stream(st);
        st.player->stop();
        st.filter_state = 0;
        return hr;
    };
    if (!from_running && s != nullptr) {
        const u32 commit = set_stream_allocator_committed(m, s, true);
        if (static_cast<i32>(commit) < 0) return fail_to_stopped(commit);
    }
    // Renderers first, then the source (the streaming thread).
    const u32 hr = call_guest_filters_state(m, filters, 5);
    if (static_cast<i32>(hr) < 0) return fail_to_stopped(hr);
    if (s != nullptr) {
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            s->run_pending = false;
        }
        if (!start_guest_stream(m, st, 1)) return fail_to_stopped(E_FAIL);
    }
    if (hr == com::S_FALSE &&
        query_guest_filters_state(m, filters, 0) == VFW_S_STATE_INTERMEDIATE) {
        return com::S_FALSE;
    }
    return com::S_OK;
}

u32 ComCoreManager::filter_graph_media_control_run(Marshaler& m) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) {
        return com::E_INVALIDARG;
    }
    if (st->filter_state == 2) return com::S_OK;
    // Guest filters and no host decoder: the application built the graph
    // itself (IPin::Connect between its own filters); the filters play it.
    if (!st->intelligent && st->player == nullptr && !st->guest_filters.empty()) st->intelligent = true;
    if (st->intelligent) return intelligent_run(m, *st);
    if (st->player == nullptr) return VFW_E_WRONG_STATE;
    graph_diag("run sid=%u\n", find_filter_graph_id_by_iface(m.arg_ptr(0)));

    if (!st->guest_filters.empty()) {
        const std::vector<GuestAddr> filters = guest_filter_list(*st);
        bool cueing = false;
        if (st->filter_state == 0) {
            st->player->pause();
            st->filter_state = 1;
            const u32 pause_hr = guest_graph_pause(m, *st, false);
            if (static_cast<i32>(pause_hr) < 0) return pause_hr;
            cueing = pause_hr == com::S_FALSE;
        } else {
            cueing = query_guest_filters_state(m, filters, 0) == VFW_S_STATE_INTERMEDIATE;
        }
        // Measured: the renderers are counted at Run (IAMFilterMiscFlags).
        st->renderer_count = count_guest_renderers(m, filters);
        st->renderer_completions = 0;
        // The graph renders the file's audio itself; on Windows that stream
        // has a DirectSound Renderer, counted like any IS_RENDERER filter.
        st->host_audio_renderer_pending = st->player->has_audio();
        if (st->host_audio_renderer_pending) ++st->renderer_count;
        if (st->video_auto_show != 0) st->video_visible = -1;
        ensure_filter_graph_event(*st);
        FilterGraphStream* s = st->stream.get();
        if (cueing && s != nullptr) {
            // Measured: Run returns S_FALSE while the renderers cue, and
            // their Run comes later from another thread.
            bool launch = false;
            {
                std::lock_guard<std::mutex> lock(s->mutex);
                s->filters = filters;
                s->run_pending = true;
                if (!s->run_worker_alive) {
                    s->run_worker_alive = true;
                    launch = true;
                }
            }
            bool started = !launch;
            if (launch && m_thunks != nullptr && m_audio_activation_thread) {
                auto entry = m_thunks->get_thunk_address(
                    "ole32.dll", "Vitreo_FilterGraph_RunWorker", true);
                started = entry.is_ok() &&
                          m_audio_activation_thread(entry.value(), s->filter_graph_id);
            }
            if (!started) {
                {
                    std::lock_guard<std::mutex> lock(s->mutex);
                    s->run_worker_alive = false;
                    s->run_pending = false;
                }
                VITREO_QUARTZ_UNIMPLEMENTED_ONCE(
                    "IMediaControl::Run: no guest thread for the deferred Run");
                return E_FAIL;
            }
            st->filter_state = 2;
            start_filter_graph_clock();
            std::fprintf(stderr, "[ole32] IMediaControl::Run -> S_FALSE (cueing)\n");
            return com::S_FALSE;
        }
        const audio::MediaStartResult result = st->player->run();
        if (result == audio::MediaStartResult::Failed) return E_FAIL;
        const u32 hr = call_guest_filters_state(m, filters, 6);
        st->filter_state = 2;
        if (s != nullptr) (void)start_guest_stream(m, *st, 2);
        start_filter_graph_clock();
        refresh_filter_graph_event(*st);
        std::fprintf(stderr, "[ole32] IMediaControl::Run -> 0x%x video=%u\n", hr,
                     st->video_renderer_connected ? 1u : 0u);
        return hr;
    }

    const audio::MediaStartResult result = st->player->run();
    if (result == audio::MediaStartResult::Failed) return E_FAIL;
    st->filter_state = 2;  // State_Running
    // Learn IVideoWindow AutoShow: the renderer shows the window when the
    // graph runs. get_Visible must then return OATRUE (-1).
    if (st->video_auto_show != 0) {
        st->video_visible = -1;
    }
    ensure_filter_graph_event(*st);
    start_filter_graph_clock();
    present_filter_graph_video(*st, &m);
    refresh_filter_graph_event(*st);
    // Learn IMediaControl::Run: S_FALSE means filters are still transitioning.
    // After a successful RenderFile the graph is running unless the player
    // Failed. Returning S_FALSE here makes callers that test `hr == S_OK`
    // tear the graph down (video stills, audio-only music).
    const u32 hr = com::S_OK;
    std::fprintf(stderr, "[ole32] IMediaControl::Run -> 0x%x video=%u\n", hr,
                 st->video_renderer_connected ? 1u : 0u);
    return hr;
}

u32 ComCoreManager::filter_graph_media_control_pause(Marshaler& m) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    if (!st->intelligent && st->player == nullptr && !st->guest_filters.empty()) st->intelligent = true;
    if (st->intelligent) {
        // Measured: Paused -> Pause is GetState(0) only.
        if (st->filter_state == 1) {
            return query_guest_filters_state(m, guest_filter_list(*st), 0) ==
                           VFW_S_STATE_INTERMEDIATE
                       ? com::S_FALSE
                       : com::S_OK;
        }
        const bool from_running = st->filter_state == 2;
        st->filter_state = 1;
        return intelligent_pause(m, *st, from_running);
    }
    if (st->player == nullptr) return VFW_E_WRONG_STATE;
    const bool guest = !st->guest_filters.empty();
    // A caller re-issues Pause on an inactive graph every frame.
    // Re-entering the player for a state that is not changing takes the voice
    // mutex and can run CoreAudio's *synchronous* AudioQueueStop, which waits
    // on the audio callback thread — on the UI thread that is a visible stall.
    // Pausing an already-paused graph is defined to succeed, so answer without
    // touching the pipeline. With guest filters the FGM asks them GetState(0)
    // (measured).
    if (st->filter_state == 1) {
        if (guest) {
            return query_guest_filters_state(m, guest_filter_list(*st), 0) ==
                           VFW_S_STATE_INTERMEDIATE
                       ? com::S_FALSE
                       : com::S_OK;
        }
        return st->player->ready() ? com::S_OK : com::S_FALSE;
    }
    if (st->video_renderer_connected) {
        std::fprintf(stderr, "[ole32] IMediaControl::Pause video=1\n");
    }
    graph_diag("pause sid=%u\n", find_filter_graph_id_by_iface(m.arg_ptr(0)));
    const bool was_running = st->filter_state == 2;
    st->player->pause();
    st->filter_state = 1;
    if (was_running) {
        stop_filter_graph_clock();
    }
    if (guest) return guest_graph_pause(m, *st, was_running);
    present_filter_graph_video(*st, &m);
    return st->player->ready() ? com::S_OK : com::S_FALSE;
}

u32 ComCoreManager::filter_graph_media_control_stop(Marshaler& m) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    if (!st->intelligent && st->player == nullptr && !st->guest_filters.empty()) st->intelligent = true;
    if (st->intelligent) return intelligent_stop(m, *st);
    if (st->player == nullptr) return VFW_E_WRONG_STATE;
    graph_diag("stop sid=%u\n", find_filter_graph_id_by_iface(m.arg_ptr(0)));
    const bool was_running = st->filter_state == 2;
    const bool was_active = st->filter_state != 0;
    u32 hr = com::S_OK;
    if (!st->guest_filters.empty() && was_active) {
        const std::vector<GuestAddr> filters = guest_filter_list(*st);
        FilterGraphStream* s = st->stream.get();
        if (s != nullptr) {
            {
                std::lock_guard<std::mutex> lock(s->mutex);
                s->target = 0;
                s->run_pending = false;
                ++s->generation;
                ++s->flushes;
            }
            s->cv.notify_all();
        }
        // Measured: Running -> Pause then Stop; a filter's S_FALSE is the
        // result. The renderers' Stop releases a Receive blocked in Paused;
        // Decommit releases a GetBuffer.
        if (was_running) (void)call_guest_filters_state(m, filters, 5);
        hr = call_guest_filters_state(m, filters, 4);
        (void)set_stream_allocator_committed(m, s, false);
        stop_guest_stream(*st);
    }
    st->player->stop();
    st->filter_state = 0;
    st->renderer_completions = 0;
    if (was_running) {
        stop_filter_graph_clock();
    }
    std::fprintf(stderr, "[ole32] IMediaControl::Stop\n");
    hide_filter_graph_video(*st);
    return hr;
}

u32 ComCoreManager::filter_graph_media_control_get_state(Marshaler& m) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) {
        return com::E_INVALIDARG;
    }
    // Learn IMediaControl::GetState(LONG msTimeout, OAFilterState *pfs).
    // Timeout is a value, not a pointer. A caller that polls with
    // GetState(0, &state) must not get E_POINTER.
    const i32 timeout_ms = static_cast<i32>(m.arg32(1));
    const GuestAddr state_ptr = m.arg_ptr(2);
    if (state_ptr == 0) {
        return E_POINTER;
    }
    if (!st->guest_filters.empty()) {
        // Measured: the FGM polls the filters' GetState with 0 / 10 ms
        // timeouts until none is intermediate or msTimeout has passed
        // (INFINITE: forever); *pfs is the state being entered.
        const std::vector<GuestAddr> filters = guest_filter_list(*st);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
        u32 step = 0;
        u32 hr = com::S_OK;
        for (;;) {
            hr = query_guest_filters_state(m, filters, step);
            if (hr != VFW_S_STATE_INTERMEDIATE || timeout_ms == 0) break;
            if (timeout_ms > 0 && std::chrono::steady_clock::now() >= deadline) break;
            step = step == 0 ? 10u : 0u;
        }
        refresh_filter_graph_event(*st);
        (void)m.write_u32(state_ptr, st->filter_state);
        return hr;
    }
    present_filter_graph_video(*st, &m);
    refresh_filter_graph_event(*st);
    (void)m.write_u32(state_ptr, st->filter_state);
    u32 hr = com::S_OK;
    if (st->filter_state == 2 && st->player != nullptr &&
        (st->video_renderer_connected || !st->player->failed())) {
        // Video renderer connected, or audio-only after a successful
        // RenderFile: the graph is running. A video-only file can fail the
        // audio decoder without aborting the graph.
        hr = com::S_OK;
    } else if (st->player != nullptr && st->player->failed() &&
               !st->video_renderer_connected) {
        hr = E_FAIL;
    }
    if (st->video_renderer_connected || hr != com::S_OK) {
        std::fprintf(stderr,
                     "[ole32] IMediaControl::GetState -> 0x%x state=%u\n", hr,
                     st->filter_state);
    }
    return hr;
}

u32 ComCoreManager::filter_graph_basic_audio_get_volume(Marshaler& m) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    const GuestAddr out_ptr = m.arg_ptr(1);
    if (st == nullptr || out_ptr == 0) {
        return st == nullptr ? com::E_INVALIDARG : E_POINTER;
    }
    (void)m.write_u32(out_ptr, static_cast<u32>(st->volume));
    return com::S_OK;
}

void ComCoreManager::forward_basic_audio(Marshaler& m, FilterGraphState& st, u32 slot, u32 value) {
    static constexpr Guid kIidBasicAudio{0x56A868B3u, 0x0AD4u, 0x11CEu,
                                         {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
    for (const GuestAddr f : guest_filter_list(st)) {
        const GuestAddr audio_iface = dshow_host().query(m, f, kIidBasicAudio);
        if (audio_iface == 0) continue;
        (void)dshow_host().call(m, audio_iface, dshow::Iface::BasicAudio, slot, {value});
        (void)dshow_host().release(m, audio_iface);
    }
}

u32 ComCoreManager::filter_graph_basic_audio_put_volume(Marshaler& m) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) {
        return com::E_INVALIDARG;
    }
    const i32 volume = static_cast<i32>(m.arg32(1));
    if (volume < -10000 || volume > 0) return com::E_INVALIDARG;
    st->volume = volume;
    graph_diag("put_volume sid=%u volume=%d player=%u\n",
               find_filter_graph_id_by_iface(m.arg_ptr(0)), volume,
               st->player != nullptr ? 1u : 0u);
    if (st->player != nullptr) st->player->set_volume(volume);
    // Learn IBasicAudio on the Filter Graph Manager: forwarded to the audio
    // renderers in the graph.
    if (st->intelligent) forward_basic_audio(m, *st, 7, static_cast<u32>(volume));
    return com::S_OK;
}

u32 ComCoreManager::filter_graph_basic_audio_get_balance(Marshaler& m) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    const GuestAddr out_ptr = m.arg_ptr(1);
    if (st == nullptr || out_ptr == 0) {
        return st == nullptr ? com::E_INVALIDARG : E_POINTER;
    }
    (void)m.write_u32(out_ptr, static_cast<u32>(st->balance));
    return com::S_OK;
}

u32 ComCoreManager::filter_graph_basic_audio_put_balance(Marshaler& m) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) {
        return com::E_INVALIDARG;
    }
    const i32 balance = static_cast<i32>(m.arg32(1));
    if (balance < -10000 || balance > 10000) return com::E_INVALIDARG;
    st->balance = balance;
    if (st->player != nullptr) st->player->set_balance(balance);
    if (st->intelligent) forward_basic_audio(m, *st, 9, static_cast<u32>(balance));
    return com::S_OK;
}

u32 ComCoreManager::filter_graph_media_seeking_get_duration(Marshaler& m) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    const GuestAddr out = m.arg_ptr(1);
    if (st == nullptr) return com::E_INVALIDARG;
    if (st->intelligent) {
        // Learn: the Filter Graph Manager forwards IMediaSeeking to the
        // renderers, which pass it upstream to the splitter.
        VITREO_QUARTZ_UNIMPLEMENTED_ONCE(
            "IMediaSeeking/IMediaPosition on a graph built from its filters: forwarding to "
            "the renderers");
        return E_NOTIMPL;
    }
    if (out == 0) return E_POINTER;
    const i64 duration =
        st->player != nullptr ? st->player->duration_100ns() : 0;
    (void)m.write_u64(out, static_cast<u64>(duration));
    std::fprintf(stderr, "[ole32] IMediaSeeking::GetDuration -> %lld video=%u\n",
                 static_cast<long long>(duration),
                 st->video_renderer_connected ? 1u : 0u);
    return com::S_OK;
}

u32 ComCoreManager::filter_graph_media_seeking_get_current_position(
    Marshaler& m) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    const GuestAddr out = m.arg_ptr(1);
    if (st == nullptr) return com::E_INVALIDARG;
    if (st->intelligent) {
        // Learn: the Filter Graph Manager forwards IMediaSeeking to the
        // renderers, which pass it upstream to the splitter.
        VITREO_QUARTZ_UNIMPLEMENTED_ONCE(
            "IMediaSeeking/IMediaPosition on a graph built from its filters: forwarding to "
            "the renderers");
        return E_NOTIMPL;
    }
    if (out == 0) return E_POINTER;
    const i64 position =
        st->player != nullptr ? st->player->position_100ns() : 0;
    (void)m.write_u64(out, static_cast<u64>(position));
    if (st->video_renderer_connected) {
        std::fprintf(stderr,
                     "[ole32] IMediaSeeking::GetCurrentPosition -> %lld\n",
                     static_cast<long long>(position));
    }
    return com::S_OK;
}

u32 ComCoreManager::filter_graph_media_seeking_get_stop_position(Marshaler& m) {
    return filter_graph_media_seeking_get_duration(m);
}

u32 ComCoreManager::filter_graph_media_seeking_set_positions(Marshaler& m) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    if (st->intelligent) {
        // Learn: the Filter Graph Manager forwards IMediaSeeking to the
        // renderers, which pass it upstream to the splitter.
        VITREO_QUARTZ_UNIMPLEMENTED_ONCE(
            "IMediaSeeking/IMediaPosition on a graph built from its filters: forwarding to "
            "the renderers");
        return E_NOTIMPL;
    }
    if (st->player == nullptr) return VFW_E_WRONG_STATE;
    const GuestAddr current_ptr = m.arg_ptr(1);
    const u32 current_flags = m.arg32(2);
    const u32 current_mode = current_flags & 0x7u;
    if (current_mode == 0) return com::S_FALSE;
    if (current_ptr == 0) return E_POINTER;
    if (current_mode != 1) return E_NOTIMPL;
    const i64 position = static_cast<i64>(
        m.memory().read_u64(current_ptr).value_or(0));
    return seek_graph(m, *st, position) ? com::S_OK : E_FAIL;
}

u32 ComCoreManager::filter_graph_media_position(Marshaler& m, u32 op) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    if (!st->intelligent && st->player == nullptr && !st->guest_filters.empty()) st->intelligent = true;
    if (st->intelligent) return intelligent_position(m, *st, op);

    const auto write_reftime = [&](GuestAddr ptr, double value) -> u32 {
        if (ptr == 0) return E_POINTER;
        u64 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        (void)m.write_u64(ptr, bits);
        return com::S_OK;
    };
    // REFTIME (double) by value: XMM1 on x64 (its RDX slot is undefined),
    // two stack dwords on x86.
    const auto read_reftime_arg = [&]() -> double { return m.arg_f64(1); };
    const auto duration_seconds = [&]() -> double {
        if (st->player == nullptr) return 0.0;
        return static_cast<double>(st->player->duration_100ns()) /
               10'000'000.0;
    };
    const auto position_seconds = [&]() -> double {
        if (st->player == nullptr) return 0.0;
        return static_cast<double>(st->player->position_100ns()) /
               10'000'000.0;
    };

    std::fprintf(stderr, "[ole32] IMediaPosition op=%u\n", op);
    switch (op) {
    case 0: // get_Duration
        return write_reftime(m.arg_ptr(1), duration_seconds());
    case 1: { // put_CurrentPosition
        if (st->player == nullptr) return E_NOTIMPL;
        const double seconds = read_reftime_arg();
        return seek_graph(m, *st, static_cast<i64>(seconds * 10'000'000.0))
                   ? com::S_OK
                   : E_FAIL;
    }
    case 2: // get_CurrentPosition
        return write_reftime(m.arg_ptr(1), position_seconds());
    case 3: // get_StopTime
        return write_reftime(m.arg_ptr(1), duration_seconds());
    case 4: // put_StopTime
        return com::S_OK;
    case 5: // get_PrerollTime
        return write_reftime(m.arg_ptr(1), 0.0);
    case 6: // put_PrerollTime
        return com::S_OK;
    case 7: // put_Rate
        st->media_rate = read_reftime_arg();
        return com::S_OK;
    case 8: // get_Rate
        return write_reftime(m.arg_ptr(1), st->media_rate);
    case 9: { // CanSeekForward
        if (m.arg_ptr(1) == 0) return E_POINTER;
        (void)m.write_u32(m.arg_ptr(1),
                          static_cast<u32>(st->player != nullptr ? OATRUE
                                                                 : OAFALSE));
        return com::S_OK;
    }
    case 10: { // CanSeekBackward
        if (m.arg_ptr(1) == 0) return E_POINTER;
        (void)m.write_u32(m.arg_ptr(1),
                          static_cast<u32>(st->player != nullptr ? OATRUE
                                                                 : OAFALSE));
        return com::S_OK;
    }
    default:
        return E_NOTIMPL;
    }
}

} // namespace vitreo::winapi
