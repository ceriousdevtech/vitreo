// SPDX-License-Identifier: LGPL-2.1-only
/// @file combase_smtc.cpp
/// @brief Windows.Media.SystemMediaTransportControls (ISystemMediaTransportControls, ...2)

#include "winapi/com/combase/combase.hpp"
#include "winapi/com/combase/combase_internal.hpp"

#include <algorithm>
#include <cstring>

namespace vitreo::winapi {

// ---------------------------------------------------------------------------
// Windows.Media.SystemMediaTransportControls
//
// Vtable order is the MIDL declaration order of ISystemMediaTransportControls
// (properties get-then-put, then the two events), not the alphabetical order
// the reference docs list members in — those disagree, and using the docs'
// order would put every call one or more slots off.
//
// The boolean properties are pure state: Windows lets an app set them and read
// them back, and that round-trip is the whole observable contract until a host
// media-key source exists to act on them.
// ---------------------------------------------------------------------------

void CombaseManager::impl_SmtcGetForWindow(Marshaler& m) {
    // GetForWindow(HWND appWindow, REFIID riid, void** mediaTransportControl)
    const GuestAddr window = m.arg_ptr(1);
    const GuestAddr iid_ptr = m.arg_ptr(2);
    const GuestAddr out = m.arg_ptr(3);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(out, 0);
    if (window == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    bool controls2 = false;
    if (iid_ptr != 0) {
        CombaseGuid iid{};
        if (m_memory.read_bytes(iid_ptr, reinterpret_cast<Byte*>(&iid),
                                sizeof(iid)).is_ok()) {
            controls2 = guid_equal(
                iid, IID_ISYSTEM_MEDIA_TRANSPORT_CONTROLS2);
        }
        if (!controls2 &&
            !guid_equal(iid, IID_ISYSTEM_MEDIA_TRANSPORT_CONTROLS) &&
            !guid_equal(iid, IID_IUNKNOWN) &&
            !guid_equal(iid, IID_IINSPECTABLE)) {
            m.set_return(E_NOINTERFACE);
            return;
        }
    }
    // One controls object per window (smtc_timeline_probe: GetForWindow for
    // ISystemMediaTransportControls2 returns the pointer QueryInterface does).
    // It lives as long as the process, holding one reference of its own.
    const bool is_64bit = m.cpu_is_64bit();
    const u64 key = (static_cast<u64>(window) << 1) | (is_64bit ? 1u : 0u);
    GuestAddr primary = 0;
    if (auto found = m_smtc_by_window.find(key);
        found != m_smtc_by_window.end() && m_objects.contains(found->second)) {
        primary = found->second;
    } else {
        Object controls{};
        controls.type = ObjectType::MediaTransportControls;
        controls.ref_count = 1;
        // smtc_timeline_probe "flag defaults": IsEnabled (bit 0) starts set,
        // every button flag clear.
        controls.smtc_flags = 1u;
        primary = allocate_object(std::move(controls), is_64bit);
        if (primary == 0) {
            m.set_return(E_OUTOFMEMORY);
            return;
        }
        m_smtc_by_window[key] = primary;
    }
    const GuestAddr object = smtc_view(primary, controls2, is_64bit);
    if (object == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    ++m_objects.at(object).ref_count;
    const bool wrote = m.write_ptr(out, object).is_ok();
    VITREO_TRACE(
        "[combase] SystemMediaTransportControls.GetForWindow(hwnd=0x%llx) -> "
        "0x%llx out=0x%llx %s\n",
        static_cast<unsigned long long>(window),
        static_cast<unsigned long long>(object),
        static_cast<unsigned long long>(out), wrote ? "S_OK" : "E_POINTER");
    m.set_return(wrote ? S_OK : E_POINTER);
}

u32 CombaseManager::smtc_get_flag(Marshaler& m, u32 bit) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return E_POINTER;
    auto it = smtc_state(m.arg_ptr(0));
    if (it == m_objects.end() ||
        (it->second.type != ObjectType::MediaTransportControls &&
         it->second.type != ObjectType::MediaTransportControls2)) {
        return E_INVALIDARG;
    }
    const bool set = (it->second.smtc_flags & (1u << bit)) != 0;
    return m.write_u8(out, set ? 1u : 0u).is_ok() ? S_OK : E_POINTER;
}

u32 CombaseManager::smtc_put_flag(Marshaler& m, u32 bit) {
    auto it = smtc_state(m.arg_ptr(0));
    if (it == m_objects.end() ||
        (it->second.type != ObjectType::MediaTransportControls &&
         it->second.type != ObjectType::MediaTransportControls2)) {
        return E_INVALIDARG;
    }
    if ((m.arg(1) & 0xFF) != 0) {
        it->second.smtc_flags |= (1u << bit);
    } else {
        it->second.smtc_flags &= ~(1u << bit);
    }
    return S_OK;
}

void CombaseManager::impl_SmtcGetPlaybackStatus(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) { m.set_return(E_POINTER); return; }
    auto it = smtc_state(m.arg_ptr(0));
    if (it == m_objects.end() ||
        (it->second.type != ObjectType::MediaTransportControls &&
         it->second.type != ObjectType::MediaTransportControls2)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    m.set_return(m.write_u32(out, it->second.smtc_playback_status).is_ok()
                     ? S_OK
                     : E_POINTER);
}

void CombaseManager::impl_SmtcPutPlaybackStatus(Marshaler& m) {
    auto it = smtc_state(m.arg_ptr(0));
    if (it == m_objects.end() ||
        (it->second.type != ObjectType::MediaTransportControls &&
         it->second.type != ObjectType::MediaTransportControls2)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    it->second.smtc_playback_status = static_cast<u32>(m.arg(1));
    republish_now_playing(it->first);
    m.set_return(S_OK);
}

void CombaseManager::impl_SmtcGetDisplayUpdater(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    VITREO_TRACE("[combase] SMTC.get_DisplayUpdater self=0x%llx out=0x%llx\n",
                 static_cast<unsigned long long>(m.arg_ptr(0)),
                 static_cast<unsigned long long>(out));
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    const GuestAddr self = m.arg_ptr(0);
    auto it = m_objects.find(self);
    if (it == m_objects.end() ||
        it->second.type != ObjectType::MediaTransportControls) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (it->second.du_music == 0 && it->second.async_peer == 0) {
        Object updater{};
        updater.type = ObjectType::DisplayUpdater;
        updater.ref_count = 1;
        updater.du_owner = self;
        const GuestAddr created =
            allocate_object(std::move(updater), m.cpu_is_64bit());
        if (created == 0) {
            VITREO_TRACE("[combase] SMTC.get_DisplayUpdater -> E_OUTOFMEMORY "
                         "(object region exhausted)\n");
            m.set_return(E_OUTOFMEMORY);
            return;
        }
        // The controls own one updater for their lifetime, as on Windows.
        m_objects.at(self).async_peer = created;
    }
    const GuestAddr updater = m_objects.at(self).async_peer;
    auto child = m_objects.find(updater);
    if (child != m_objects.end()) ++child->second.ref_count;
    const bool wrote = m.write_ptr(out, updater).is_ok();
    VITREO_TRACE("[combase] SMTC.get_DisplayUpdater -> 0x%llx %s\n",
                 static_cast<unsigned long long>(updater),
                 wrote ? "S_OK" : "E_POINTER");
    m.set_return(wrote ? S_OK : E_POINTER);
}

void CombaseManager::impl_SmtcGetSoundLevel(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) { m.set_return(E_POINTER); return; }
    // SoundLevel::Full = 2. Nothing on this host ducks or mutes the guest.
    m.set_return(m.write_u32(out, 2u).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_SmtcAddEvent(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr handler = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_u64(out, 0);
    auto object = smtc_state(self);
    if (object == m_objects.end() ||
        (object->second.type != ObjectType::MediaTransportControls &&
         object->second.type != ObjectType::MediaTransportControls2) ||
        handler == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (guest_iunknown_ref(m, handler, 1) == 0) {
        m.set_return(E_POINTER);
        return;
    }
    u64 token = object->second.smtc_next_event_token++;
    if (token == 0) token = object->second.smtc_next_event_token++;
    object->second.smtc_event_handlers.emplace_back(token, handler);
    VITREO_TRACE(
        "[combase] SystemMediaTransportControls event registered "
        "handler=0x%llx token=%llu\n",
        static_cast<unsigned long long>(handler),
        static_cast<unsigned long long>(token));
    if (m.write_u64(out, token).is_err()) {
        object->second.smtc_event_handlers.pop_back();
        (void)guest_iunknown_ref(m, handler, 2);
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

void CombaseManager::impl_SmtcRemoveEvent(Marshaler& m) {
    auto object = smtc_state(m.arg_ptr(0));
    if (object == m_objects.end() ||
        (object->second.type != ObjectType::MediaTransportControls &&
         object->second.type != ObjectType::MediaTransportControls2)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const u64 token = m.cpu_is_64bit()
                          ? m.arg(1)
                          : ((m.arg(1) & 0xFFFFFFFFull) |
                             ((m.arg(2) & 0xFFFFFFFFull) << 32));
    auto& handlers = object->second.smtc_event_handlers;
    const auto registration = std::find_if(
        handlers.begin(), handlers.end(),
        [token](const auto& entry) { return entry.first == token; });
    if (registration != handlers.end()) {
        const GuestAddr handler = registration->second;
        handlers.erase(registration);
        (void)guest_iunknown_ref(m, handler, 2);
    }
    m.set_return(S_OK);
}

std::unordered_map<GuestAddr, CombaseManager::Object>::iterator
CombaseManager::smtc_state(GuestAddr self) {
    auto it = m_objects.find(self);
    if (it != m_objects.end() && it->second.type == ObjectType::MediaTransportControls2 &&
        it->second.smtc_peer != 0) {
        if (auto primary = m_objects.find(it->second.smtc_peer); primary != m_objects.end()) {
            return primary;
        }
    }
    return it;
}

GuestAddr CombaseManager::smtc_view(GuestAddr primary, bool v2, bool is_64bit) {
    auto it = m_objects.find(primary);
    if (it == m_objects.end()) return 0;
    if (!v2) return primary;
    if (it->second.smtc_peer != 0 && m_objects.contains(it->second.smtc_peer)) {
        return it->second.smtc_peer;
    }
    // The ISystemMediaTransportControls2 view: its own vtable over the same
    // controls, kept alive (one reference) as long as the controls are.
    Object view{};
    view.type = ObjectType::MediaTransportControls2;
    view.ref_count = 1;
    view.smtc_peer = primary;
    const GuestAddr address = allocate_object(std::move(view), is_64bit);
    if (address == 0) return 0;
    m_objects.at(primary).smtc_peer = address;
    return address;
}

void CombaseManager::republish_now_playing(GuestAddr controls) {
    auto it = m_objects.find(controls);
    if (it == m_objects.end() || it->second.async_peer == 0) return;
    if (auto updater = m_objects.find(it->second.async_peer); updater != m_objects.end()) {
        publish_now_playing(updater->second);
    }
}

bool CombaseManager::is_controls2(GuestAddr self) const {
    auto it = m_objects.find(self);
    return it != m_objects.end() && it->second.type == ObjectType::MediaTransportControls2;
}

void CombaseManager::impl_Smtc2GetPlaybackRate(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (!is_controls2(m.arg_ptr(0))) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto state = smtc_state(m.arg_ptr(0));
    u64 bits = 0;
    static_assert(sizeof(bits) == sizeof(state->second.smtc_playback_rate));
    std::memcpy(&bits, &state->second.smtc_playback_rate, sizeof(bits));
    m.set_return(m.write_u64(out, bits).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_Smtc2PutPlaybackRate(Marshaler& m) {
    if (!is_controls2(m.arg_ptr(0))) {
        m.set_return(E_INVALIDARG);
        return;
    }
    // put_PlaybackRate(DOUBLE): XMM1 on x64, two stack words on x86
    // (smtc_timeline_probe "after puts ... rate=1.5").
    const double rate = m.arg_f64(1);
    const auto state = smtc_state(m.arg_ptr(0));
    state->second.smtc_playback_rate = rate;
    republish_now_playing(state->first);
    m.set_return(S_OK);
}

void CombaseManager::impl_Smtc2GetAutoRepeatMode(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (!is_controls2(m.arg_ptr(0))) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto state = smtc_state(m.arg_ptr(0));
    m.set_return(m.write_u32(out, state->second.smtc_auto_repeat).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_Smtc2PutAutoRepeatMode(Marshaler& m) {
    if (!is_controls2(m.arg_ptr(0))) {
        m.set_return(E_INVALIDARG);
        return;
    }
    // MediaPlaybackAutoRepeatMode: None 0, Track 1, List 2. Windows takes any
    // value and keeps None for one outside the enum (smtc_timeline_probe
    // "put_AutoRepeatMode 7 hr=00000000 now=0").
    const u32 mode = m.arg32(1);
    smtc_state(m.arg_ptr(0))->second.smtc_auto_repeat = mode <= 2 ? mode : 0;
    m.set_return(S_OK);
}

void CombaseManager::impl_Smtc2GetShuffleEnabled(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (!is_controls2(m.arg_ptr(0))) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto state = smtc_state(m.arg_ptr(0));
    m.set_return(m.write_u8(out, state->second.smtc_shuffle ? 1u : 0u).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_Smtc2PutShuffleEnabled(Marshaler& m) {
    if (!is_controls2(m.arg_ptr(0))) {
        m.set_return(E_INVALIDARG);
        return;
    }
    smtc_state(m.arg_ptr(0))->second.smtc_shuffle = (m.arg(1) & 0xFF) != 0;
    m.set_return(S_OK);
}

void CombaseManager::impl_Smtc2UpdateTimelineProperties(Marshaler& m) {
    // UpdateTimelineProperties(ISystemMediaTransportControlsTimelineProperties*)
    // smtc_timeline_probe: S_OK for a TimelineProperties; Windows dereferences
    // the argument unchecked (NULL is an access violation there), so a NULL or
    // foreign object fails here rather than being dereferenced.
    if (!is_controls2(m.arg_ptr(0))) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr props = m.arg_ptr(1);
    if (props == 0) {
        m.set_return(E_POINTER);
        return;
    }
    auto timeline = m_objects.find(props);
    if (timeline == m_objects.end() || timeline->second.type != ObjectType::TimelineProperties) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto state = smtc_state(m.arg_ptr(0));
    state->second.smtc_timeline = timeline->second.timeline;
    state->second.smtc_has_timeline = true;
    republish_now_playing(state->first);
    m.set_return(S_OK);
}

// ---------------------------------------------------------------------------
// Windows.Media.SystemMediaTransportControlsTimelineProperties
//
// Five TimeSpan properties (Int64 100-ns ticks) in windows.media.h order:
// StartTime, EndTime, MinSeekTime, MaxSeekTime, Position. smtc_timeline_probe:
// all start at 0 and any value is kept as given (negative, past the end).
// A TimeSpan travels by value: one register on x64, two stack words on x86.
// ---------------------------------------------------------------------------

u32 CombaseManager::timeline_get(Marshaler& m, u32 field) {
    const GuestAddr out = m.arg_ptr(1);
    auto it = m_objects.find(m.arg_ptr(0));
    if (it == m_objects.end() || it->second.type != ObjectType::TimelineProperties ||
        field >= it->second.timeline.size()) {
        return E_INVALIDARG;
    }
    if (out == 0) return E_POINTER;
    return m.write_u64(out, static_cast<u64>(it->second.timeline[field])).is_ok() ? S_OK
                                                                                   : E_POINTER;
}

u32 CombaseManager::timeline_put(Marshaler& m, u32 field) {
    auto it = m_objects.find(m.arg_ptr(0));
    if (it == m_objects.end() || it->second.type != ObjectType::TimelineProperties ||
        field >= it->second.timeline.size()) {
        return E_INVALIDARG;
    }
    const u64 ticks = m.cpu_is_64bit()
                          ? m.arg(1)
                          : ((m.arg(1) & 0xFFFFFFFFull) | ((m.arg(2) & 0xFFFFFFFFull) << 32));
    it->second.timeline[field] = static_cast<i64>(ticks);
    return S_OK;
}

} // namespace vitreo::winapi
