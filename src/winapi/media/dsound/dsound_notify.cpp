/// @file dsound_notify.cpp
/// @brief Render position notifications (host poller) and IDirectSoundNotify.

#include "winapi/media/dsound/dsound_internal.hpp"

#include <chrono>

namespace vitreo::winapi {
// ---------------------------------------------------------------------------
// Render position notifications (host poller)
// ---------------------------------------------------------------------------

void DirectSoundManager::wake_notify_thread_locked() {
    if (!m_notify_thread.joinable() && !m_notify_stop) {
        m_notify_thread = std::thread([this] { notify_thread_loop(); });
    }
    m_notify_wake.notify_all();
}

void DirectSoundManager::notify_thread_loop() {
    std::unique_lock lock(m_mutex);
    while (!m_notify_stop) {
        bool active = false;
        for (auto& [address, state] : m_buffers) {
            (void)address;
            if (state.notifications.empty() || !state.voice) continue;
            const bool playing = state.voice->is_playing();
            const u32 cursor = state.voice->play_cursor();
            if (state.was_playing) {
                // The cursor moved from last_cursor to cursor, wrapping at
                // most once between two polls of a looping ring.
                if (cursor >= state.last_cursor) {
                    signal_crossed(state.notifications, state.last_cursor, cursor);
                } else {
                    signal_crossed(state.notifications, state.last_cursor, state.bytes);
                    signal_crossed(state.notifications, 0, cursor);
                }
                if (!playing) {
                    // A non-looping buffer that played to its end stopped
                    // itself: DSBPN_OFFSETSTOP fires (Learn DSBPOSITIONNOTIFY).
                    for (const auto& position : state.notifications) {
                        if (position.offset == dsound::DSBPN_OFFSETSTOP) signal(position.event);
                    }
                }
            }
            state.last_cursor = cursor;
            state.was_playing = playing;
            active = active || playing;
        }
        if (active) {
            m_notify_wake.wait_for(lock, std::chrono::milliseconds(5));
        } else {
            m_notify_wake.wait(lock);
        }
    }
}

// ---------------------------------------------------------------------------
// IDirectSoundNotify (capture and render buffers)
// ---------------------------------------------------------------------------

u32 DirectSoundManager::set_positions(std::vector<NotifyPosition>& list, u32 buffer_bytes,
                                      u32 count, GuestAddr positions) {
    // Measured (audio_capture_contract_probe "ds notify"): an empty set is
    // accepted and clears; offsets must be inside the buffer or
    // DSBPN_OFFSETSTOP; events must be non-NULL (not otherwise validated);
    // order is free; a repeated offset is refused.
    if (count == 0) {
        list.clear();
        return dsound::DS_OK;
    }
    if (positions == 0 || count > dsound::DSBNOTIFICATIONS_MAX || !m_memory) {
        return dsound::E_INVALIDARG;
    }
    const u32 stride = m_x64 ? 16u : 8u;  // { DWORD dwOffset; HANDLE hEventNotify; }
    std::vector<NotifyPosition> parsed;
    parsed.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        const GuestAddr entry = positions + static_cast<GuestAddr>(i) * stride;
        auto offset = m_memory->read_u32(entry);
        const u64 event = m_x64 ? m_memory->read_u64(entry + 8).value_or(0)
                                : m_memory->read_u32(entry + 4).value_or(0);
        if (offset.is_err()) return dsound::E_INVALIDARG;
        if (offset.value() != dsound::DSBPN_OFFSETSTOP && offset.value() >= buffer_bytes) {
            return dsound::E_INVALIDARG;
        }
        if (event == 0) return dsound::E_INVALIDARG;
        for (const auto& seen : parsed) {
            if (seen.offset == offset.value()) return dsound::E_INVALIDARG;
        }
        parsed.push_back(NotifyPosition{offset.value(), event});
    }
    list = std::move(parsed);
    return dsound::DS_OK;
}

u32 DirectSoundManager::notify_query_interface(GuestAddr notify, GuestAddr iid,
                                               GuestAddr output) {
    std::lock_guard lock(m_mutex);  // recursive: the callees lock again
    auto facet = m_facets.find(notify);
    if (facet == m_facets.end()) return dsound::E_NOINTERFACE;
    const GuestAddr owner = facet->second;
    if (m_cbuffers.count(owner) != 0) return cbuffer_query_interface(notify, iid, output);
    return buffer_query_interface(owner, iid, output);
}

u32 DirectSoundManager::notify_add_ref(GuestAddr notify) {
    std::lock_guard lock(m_mutex);
    if (auto* capture = find_cbuffer(notify)) return ++capture->refs;
    if (auto* render = find_buffer(notify)) return ++render->ref_count;
    return 0;
}

u32 DirectSoundManager::notify_release(GuestAddr notify) {
    {
        std::lock_guard lock(m_mutex);
        if (!find_cbuffer(notify)) return buffer_release(notify);
    }
    return cbuffer_release(notify);
}

u32 DirectSoundManager::notify_set_positions(GuestAddr notify, u32 count,
                                             GuestAddr positions) {
    std::lock_guard lock(m_mutex);
    if (auto* capture = find_cbuffer(notify)) {
        // Measured: refused while capturing, whatever the arguments.
        if (capture->capturing) return dsound::DSERR_INVALIDCALL;
        return set_positions(capture->notifications, capture->bytes, count, positions);
    }
    if (auto* render = find_buffer(notify)) {
        // Learn (SetNotificationPositions): the buffer must be stopped.
        if (render->voice && render->voice->is_playing()) return dsound::DSERR_INVALIDCALL;
        return set_positions(render->notifications, render->bytes, count, positions);
    }
    return dsound::E_INVALIDARG;
}

} // namespace vitreo::winapi
