// SPDX-License-Identifier: LGPL-2.1-only
/// @file filter_graph_events.cpp
/// @brief Filter graph event queue, window notification and the 16 ms clock thread,
/// plus IMediaEvent / IMediaEventEx / IMediaEventSink.

#include "filter_graph_internal.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/user32/messages/message_send.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

namespace vitreo::winapi {

void ComCoreManager::ensure_filter_graph_event(FilterGraphState& st) {
    if (st.event_handle != 0) return;
    if (m_sync != nullptr) {
        st.event_handle = m_sync->create_event(true, false, "");
    }
    if (st.event_handle == 0) {
        const u32 sid = find_filter_graph_id_by_iface(st.media_event_iface);
        st.event_handle = kFallbackEventHandleBase | (sid & 0xFFFFu);
    }
}

void ComCoreManager::signal_filter_graph_event(FilterGraphState& st) {
    ensure_filter_graph_event(st);
    if (m_sync != nullptr && st.event_handle != 0) {
        (void)m_sync->set_event(st.event_handle);
    }
}

void ComCoreManager::reset_filter_graph_event(FilterGraphState& st) {
    if (m_sync != nullptr && st.event_handle != 0) {
        (void)m_sync->reset_event(st.event_handle);
    }
}

void ComCoreManager::notify_filter_graph_window(const FilterGraphState& st) {
    if (st.notify_flags == AM_MEDIAEVENT_NONOTIFY) return;
    if (st.notify_hwnd == 0 || m_message_send == nullptr) return;
    (void)m_message_send->post_message(
        static_cast<windowing::HWND>(st.notify_hwnd), st.notify_msg, 0,
        st.notify_instance);
}

void ComCoreManager::refresh_filter_graph_event(FilterGraphState& st) {
    if (st.player == nullptr) return;
    if (st.has_pending_event) {
        signal_filter_graph_event(st);
        return;
    }
    bool queued = false;
    // With a guest renderer the streaming thread owns end of stream: the
    // renderer's own EC_COMPLETE (IMediaEventSink::Notify) is the event.
    if (st.stream != nullptr) return;
    if (st.player->take_completed()) {
        if (st.notify_flags != AM_MEDIAEVENT_NONOTIFY) {
            st.has_pending_event = true;
            st.pending_event_code = EC_COMPLETE;
            st.pending_param1 = 0;
            st.pending_param2 = 0;
            queued = true;
        }
        // Learn SetNotifyFlags: the GetEventHandle event is still signaled
        // when EC_COMPLETE arrives, even if notifications are disabled.
        signal_filter_graph_event(st);
        if (queued) {
            std::fprintf(stderr,
                         "[ole32] IMediaEvent queue EC_COMPLETE handle=0x%x\n",
                         st.event_handle);
            notify_filter_graph_window(st);
        }
    } else if (st.player->failed() && !st.video_renderer_connected) {
        if (st.notify_flags != AM_MEDIAEVENT_NONOTIFY) {
            st.has_pending_event = true;
            st.pending_event_code = EC_ERRORABORT;
            // EC_ERRORABORT lParam1 is an HRESULT (a long): sign-extend it
            // into the LONG_PTR as a 64-bit caller reads it.
            st.pending_param1 = static_cast<u64>(static_cast<i64>(static_cast<i32>(E_FAIL)));
            st.pending_param2 = 0;
            queued = true;
        }
        signal_filter_graph_event(st);
        if (queued) notify_filter_graph_window(st);
    }
}

void ComCoreManager::start_filter_graph_clock() {
    const u32 prev =
        m_filter_graph_running_count.fetch_add(1, std::memory_order_acq_rel);
    if (prev == 0) {
        if (m_filter_graph_clock_thread.joinable()) {
            m_filter_graph_clock_stop.store(true, std::memory_order_release);
            m_filter_graph_clock_cv.notify_all();
            m_filter_graph_clock_thread.join();
        }
        m_filter_graph_clock_stop.store(false, std::memory_order_release);
        m_filter_graph_clock_thread =
            std::thread([this] { filter_graph_clock_loop(); });
    }
}

void ComCoreManager::stop_filter_graph_clock() {
    const u32 prev =
        m_filter_graph_running_count.load(std::memory_order_acquire);
    if (prev == 0) return;
    if (m_filter_graph_running_count.fetch_sub(1, std::memory_order_acq_rel) ==
        1) {
        m_filter_graph_clock_stop.store(true, std::memory_order_release);
        m_filter_graph_clock_cv.notify_all();
        if (m_filter_graph_clock_thread.joinable()) {
            m_filter_graph_clock_thread.join();
        }
    }
}

void ComCoreManager::filter_graph_clock_loop() {
    while (!m_filter_graph_clock_stop.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lock(m_filter_graph_clock_mutex);
            for (auto& [id, st] : m_filter_graphs) {
                (void)id;
                if (st.filter_state != 2) continue;
                present_filter_graph_video(st);
                refresh_filter_graph_event(st);
            }
        }
        std::unique_lock<std::mutex> wait(m_filter_graph_clock_mutex);
        m_filter_graph_clock_cv.wait_for(
            wait, std::chrono::milliseconds(16), [this] {
                return m_filter_graph_clock_stop.load(
                    std::memory_order_acquire);
            });
    }
}

u32 ComCoreManager::wait_filter_graph_notification(
    FilterGraphState& st, i32 timeout_ms, u32& ev_code, Marshaler* m) {
    const bool infinite = timeout_ms < 0;
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(infinite ? 0 : timeout_ms);
    for (;;) {
        {
            // The renderer's Notify arrives on the streaming thread.
            std::lock_guard<std::mutex> lock(m_filter_graph_clock_mutex);
            refresh_filter_graph_event(st);
            present_filter_graph_video(st, m);
            if (st.has_pending_event) {
                ev_code = st.pending_event_code;
                st.taken_param1 = st.pending_param1;
                st.taken_param2 = st.pending_param2;
                st.has_pending_event = false;
                reset_filter_graph_event(st);
                return com::S_OK;
            }
        }
        if (timeout_ms == 0) {
            ev_code = 0;
            return E_ABORT;
        }
        if (!infinite && std::chrono::steady_clock::now() >= deadline) {
            ev_code = 0;
            return E_ABORT;
        }
        if (m_sync != nullptr && st.event_handle != 0) {
            i64 slice = 16;
            if (!infinite) {
                const auto left =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now())
                        .count();
                if (left <= 0) {
                    ev_code = 0;
                    return E_ABORT;
                }
                slice = std::min<i64>(left, 16);
            }
            (void)m_sync->wait_for_single_object(
                st.event_handle, static_cast<u32>(slice));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }
}

u32 ComCoreManager::filter_graph_media_event_get_event(Marshaler& m) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) {
        return com::E_INVALIDARG;
    }
    const GuestAddr code_ptr = m.arg_ptr(1);
    const GuestAddr p1_ptr = m.arg_ptr(2);
    const GuestAddr p2_ptr = m.arg_ptr(3);
    // GetEvent(long* lEventCode, LONG_PTR* lParam1, LONG_PTR* lParam2,
    // long msTimeout): the params are pointer-width (measured on x64: an
    // IMediaEventSink::Notify LONG_PTR round-trips all 64 bits).
    const i32 timeout_ms = static_cast<i32>(m.arg32(4));
    if (code_ptr == 0 || p1_ptr == 0 || p2_ptr == 0) {
        return E_POINTER;
    }
    ensure_filter_graph_event(*st);
    u32 code = 0;
    const u32 hr = wait_filter_graph_notification(*st, timeout_ms, code, &m);
    if (hr != com::S_OK) {
        (void)m.write_u32(code_ptr, 0);
        (void)m.write_ptr(p1_ptr, 0);
        (void)m.write_ptr(p2_ptr, 0);
        if (st->video_renderer_connected) {
            std::fprintf(stderr,
                         "[ole32] IMediaEvent::GetEvent -> 0x%x timeout=%d\n",
                         hr, timeout_ms);
        }
        return hr;
    }
    (void)m.write_u32(code_ptr, code);
    (void)m.write_ptr(p1_ptr, st->taken_param1);
    (void)m.write_ptr(p2_ptr, st->taken_param2);
    std::fprintf(stderr, "[ole32] IMediaEvent::GetEvent -> code=0x%x\n", code);
    return com::S_OK;
}

u32 ComCoreManager::filter_graph_media_event_get_event_handle(Marshaler& m) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return E_POINTER;
    ensure_filter_graph_event(*st);
    (void)m.write_ptr(out, st->event_handle);  // OAEVENT* (LONG_PTR)
    std::fprintf(stderr, "[ole32] IMediaEvent::GetEventHandle -> 0x%x\n",
                 st->event_handle);
    return com::S_OK;
}

u32 ComCoreManager::filter_graph_media_event_set_notify_window(Marshaler& m) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    // Learn: hwnd NULL stops notify. Any other handle is stored; window
    // validity is enforced when posting (invalid hwnd simply does not deliver).
    // SetNotifyWindow(OAHWND hwnd, long lMsg, LONG_PTR lInstanceData):
    // OAHWND and LONG_PTR are pointer-width, lMsg is 32 bits.
    st->notify_hwnd = m.cpu_is_64bit() ? m.arg(1) : (m.arg(1) & 0xFFFFFFFFull);
    st->notify_msg = m.arg32(2);
    st->notify_instance =
        m.cpu_is_64bit() ? m.arg(3) : (m.arg(3) & 0xFFFFFFFFull);
    std::fprintf(stderr,
                 "[ole32] IMediaEventEx::SetNotifyWindow hwnd=0x%llx msg=0x%x\n",
                 static_cast<unsigned long long>(st->notify_hwnd),
                 st->notify_msg);
    return com::S_OK;
}

u32 ComCoreManager::filter_graph_media_event_set_notify_flags(Marshaler& m) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    const u32 flags = m.arg32(1);
    if (flags != 0 && flags != AM_MEDIAEVENT_NONOTIFY) {
        return com::E_INVALIDARG;
    }
    st->notify_flags = flags;
    if (flags == AM_MEDIAEVENT_NONOTIFY) {
        st->has_pending_event = false;
        reset_filter_graph_event(*st);
    }
    return com::S_OK;
}

u32 ComCoreManager::filter_graph_media_event_get_notify_flags(Marshaler& m) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return E_POINTER;
    (void)m.write_u32(out, st->notify_flags);
    return com::S_OK;
}

void ComCoreManager::report_filter_graph_event_locked(FilterGraphState& st, u32 code, u64 p1,
                                                      u64 p2) {
    if (code == EC_COMPLETE) {
        std::fprintf(stderr,
                     "[ole32] EC_COMPLETE report hr=0x%llx state=%u renderers=%u completed=%u "
                     "default=%d\n",
                     static_cast<unsigned long long>(p1), st.filter_state, st.renderer_count,
                     st.renderer_completions, st.complete_default_cancelled ? 0 : 1);
    }
    if (code == EC_COMPLETE && !st.complete_default_cancelled) {
        // Default handling, measured (dshow_complete_probe, x86 and x64):
        // only while running; one EC_COMPLETE reaches the application once
        // the renderers counted at Run (IAMFilterMiscFlags IS_RENDERER) have
        // sent as many; lParam1 is the renderer's HRESULT and lParam2 is 0.
        // Further reports until the next Run are dropped.
        if (st.filter_state != 2 || st.renderer_count == 0 ||
            st.renderer_completions >= st.renderer_count) {
            return;
        }
        if (++st.renderer_completions < st.renderer_count) return;
        p2 = 0;
        st.seek_completed = true;  // IMediaSeeking: the position is now the stop time
    }
    if (st.notify_flags != AM_MEDIAEVENT_NONOTIFY) {
        st.has_pending_event = true;
        st.pending_event_code = code;
        st.pending_param1 = p1;
        st.pending_param2 = p2;
    }
    signal_filter_graph_event(st);
    notify_filter_graph_window(st);
}

u32 ComCoreManager::filter_graph_media_event_sink_notify(Marshaler& m) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    // Notify(long EventCode, LONG_PTR EventParam1, LONG_PTR EventParam2).
    const u32 code = m.arg32(1);
    const u64 p1 = m.cpu_is_64bit() ? m.arg(2) : (m.arg(2) & 0xFFFFFFFFull);
    u64 p2 = m.cpu_is_64bit() ? m.arg(3) : (m.arg(3) & 0xFFFFFFFFull);
    std::lock_guard<std::mutex> lock(m_filter_graph_clock_mutex);
    report_filter_graph_event_locked(*st, code, p1, p2);
    return com::S_OK;
}

u32 ComCoreManager::filter_graph_media_event_default_handling(Marshaler& m,
                                                              bool restore) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    // EC_COMPLETE is the event this graph handles by default; for the rest
    // the call has nothing to change.
    if (m.arg32(1) == EC_COMPLETE) {
        std::lock_guard<std::mutex> lock(m_filter_graph_clock_mutex);
        st->complete_default_cancelled = !restore;
    }
    return com::S_OK;
}

u32 ComCoreManager::filter_graph_media_event_wait_for_completion(Marshaler& m) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) {
        return com::E_INVALIDARG;
    }
    // HRESULT WaitForCompletion(long msTimeout, long *pEvCode)
    // Learn: graph must be running; 0 returns immediately; INFINITE (-1)
    // blocks until EC_COMPLETE / EC_ERRORABORT / EC_USERABORT.
    const i32 timeout_ms = static_cast<i32>(m.arg32(1));
    const GuestAddr ev_code_ptr = m.arg_ptr(2);
    if (ev_code_ptr == 0) {
        return E_POINTER;
    }
    if (st->filter_state != 2) {
        (void)m.write_u32(ev_code_ptr, 0);
        return VFW_E_WRONG_STATE;
    }
    if (timeout_ms != 0) {
        std::fprintf(stderr,
                     "[ole32] IMediaEvent::WaitForCompletion timeout=%d\n",
                     timeout_ms);
    }
    const bool infinite = timeout_ms < 0;
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(infinite ? 0 : timeout_ms);
    for (;;) {
        i32 slice = timeout_ms;
        if (!infinite && timeout_ms != 0) {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now())
                    .count();
            if (left <= 0) {
                (void)m.write_u32(ev_code_ptr, 0);
                return E_ABORT;
            }
            slice = static_cast<i32>(left);
        }
        u32 code = 0;
        const u32 hr = wait_filter_graph_notification(*st, slice, code, &m);
        if (hr != com::S_OK) {
            (void)m.write_u32(ev_code_ptr, 0);
            return hr;
        }
        // Learn WaitForCompletion: discard notifications other than
        // EC_COMPLETE / EC_ERRORABORT / EC_USERABORT.
        if (code == EC_COMPLETE || code == EC_ERRORABORT ||
            code == EC_USERABORT) {
            (void)m.write_u32(ev_code_ptr, code);
            std::fprintf(stderr,
                         "[ole32] IMediaEvent::WaitForCompletion -> S_OK "
                         "code=0x%x\n",
                         code);
            return com::S_OK;
        }
        if (timeout_ms == 0) {
            (void)m.write_u32(ev_code_ptr, 0);
            return E_ABORT;
        }
    }
}

} // namespace vitreo::winapi
