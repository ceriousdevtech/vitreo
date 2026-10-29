// SPDX-License-Identifier: LGPL-2.1-only

/// @file mfplay_player.cpp
/// @brief IMFPMediaPlayer. Return codes and the event each call raises come
/// from Learn and mfplay_probe{32,64}.expected (see mfplay.hpp).

#include "winapi/media/mfplay/mfplay_internal.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>
#include <thread>

namespace vitreo::winapi {

using namespace mfplay;

namespace {

bool read_guid(Marshaler& m, GuestAddr at, Guid& out) {
    if (at == 0) return false;
    auto r = m.read_struct<Guid>(at);
    if (r.is_err()) return false;
    out = r.value();
    return true;
}

void write_f32(Marshaler& m, GuestAddr at, float v) {
    (void)m.memory().write_u32(at, std::bit_cast<u32>(v));
}

float read_f32(Marshaler& m, GuestAddr at) {
    return std::bit_cast<float>(m.memory().read_u32(at).value_or(0));
}

/// PropVariantInit on the caller's PROPVARIANT.
void clear_pv(Marshaler& m, GuestAddr pv) {
    const std::array<Byte, 24> zero{};
    (void)m.memory().write_bytes(pv, zero.data(), propvariant_size(m.cpu_is_64bit()));
}

void write_pv_64(Marshaler& m, GuestAddr pv, u16 vt, u64 value) {
    clear_pv(m, pv);
    (void)m.memory().write_u16(pv, vt);
    (void)m.memory().write_u64(pv + 8, value);
}

/// Linear amplitude to the backend's hundredths of a decibel.
i32 amplitude_to_hundredth_db(double gain) {
    if (gain <= 0.0) return -10000;
    return static_cast<i32>(std::clamp<long>(std::lround(2000.0 * std::log10(gain)), -10000, 0));
}

u32 f32_bits(float v) { return std::bit_cast<u32>(v); }

} // namespace

u32 MfPlayManager::dispatch_player(u32 slot, Marshaler& m) {
    PlayerPtr p = find_player(m.arg_ptr(0));
    if (!p) return E_POINTER;  // not an object this DLL handed out
    if (slot <= P_Release) return player_unknown(slot, m, p);
    if (slot <= P_GetState) return player_transport(slot, m, p);
    if (slot <= P_GetMediaItem) return player_items(slot, m, p);
    if (slot <= P_SetMute) return player_audio(slot, m, p);
    if (slot <= P_GetBorderColor) return player_video(slot, m, p);
    return player_effects(slot, m, p);
}

// ---------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------

u32 MfPlayManager::player_unknown(u32 slot, Marshaler& m, const PlayerPtr& p) {
    switch (slot) {
        case P_QueryInterface: {
            const GuestAddr ppv = m.arg_ptr(2);
            if (ppv == 0) return E_POINTER;
            Guid iid;
            if (!read_guid(m, m.arg_ptr(1), iid)) {
                (void)m.write_ptr(ppv, 0);
                return E_POINTER;
            }
            if (iid == IID_IUnknown || iid == IID_IMFPMediaPlayer) {
                std::lock_guard<std::recursive_mutex> lock(p->mutex);
                ++p->refs;
                (void)m.write_ptr(ppv, p->cell);
                return S_OK;
            }
            if (iid == IID_IPropertyStore) {
                MFPLAY_UNIMPLEMENTED("IMFPMediaPlayer::QueryInterface",
                                     "IPropertyStore (media source configuration properties)");
            }
            (void)m.write_ptr(ppv, 0);
            return E_NOINTERFACE;
        }
        case P_AddRef: {
            // Measured: the first AddRef on a new player answers 3 -- the
            // player holds one reference on itself until it is shut down.
            std::lock_guard<std::recursive_mutex> lock(p->mutex);
            ++p->refs;
            return p->refs + (p->shut_down ? 0u : 1u);
        }
        case P_Release: {
            u32 left = 0;
            {
                std::lock_guard<std::recursive_mutex> lock(p->mutex);
                if (p->refs == 0) return 0;
                left = --p->refs;
                if (left != 0) return left + (p->shut_down ? 0u : 1u);
            }
            // The last reference shuts the player down (Learn Shutdown).
            shutdown_player(p, false);
            release_player_cell(p);
            return 0;
        }
        default:
            return E_NOTIMPL;
    }
}

// ---------------------------------------------------------------------------
// Transport, position, rate, state
// ---------------------------------------------------------------------------

void MfPlayManager::play(const PlayerPtr& p) {
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (!p->current || !p->current->media) {
        post_event(p, Event{EventType::Play, MF_E_INVALIDREQUEST, p->state, nullptr, 0});
        return;
    }
    apply_audio(*p);
    if (p->current->media->run() == audio::MediaStartResult::Failed) {
        post_event(p, Event{EventType::Play, E_FAIL, p->state, p->current, 0});
        return;
    }
    p->state = State::Playing;
    post_event(p, Event{EventType::Play, S_OK, State::Playing, p->current, 0});
}

u32 MfPlayManager::player_transport(u32 slot, Marshaler& m, const PlayerPtr& p) {
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    switch (slot) {
        case P_Play:
            if (p->shut_down) return MF_E_SHUTDOWN;
            play(p);
            return S_OK;
        case P_Pause:
            if (p->shut_down) return MF_E_SHUTDOWN;
            if (!p->current || !p->current->media) {
                post_event(p, Event{EventType::Pause, MF_E_INVALIDREQUEST, p->state, nullptr, 0});
                return S_OK;
            }
            p->current->media->pause();
            p->state = State::Paused;
            present_frame(*p, true);  // a paused player shows the current frame
            post_event(p, Event{EventType::Pause, S_OK, State::Paused, p->current, 0});
            return S_OK;
        case P_Stop: {
            if (p->shut_down) return MF_E_SHUTDOWN;
            // Measured: Stop with no item completes with S_OK.
            if (!p->current || !p->current->media) {
                post_event(p, Event{EventType::Stop, S_OK, p->state, nullptr, 0});
                return S_OK;
            }
            p->current->media->stop();
            (void)p->current->media->seek_100ns(p->current->start.value_or(0));
            p->state = State::Stopped;
            // Learn (MFPlay guide): no video is displayed after Stop.
            if (p->video_shown && m_host.hide_video) m_host.hide_video(p->video_hwnd);
            p->video_shown = false;
            post_event(p, Event{EventType::Stop, S_OK, State::Stopped, p->current, 0});
            return S_OK;
        }
        case P_FrameStep:
            if (p->shut_down) return MF_E_SHUTDOWN;
            if (!p->current || !p->current->has_video) {
                post_event(p, Event{EventType::FrameStep, MF_E_INVALIDREQUEST, p->state, nullptr, 0});
                return S_OK;
            }
            // Learn: MF_E_UNSUPPORTED_RATE when the source cannot frame step.
            MFPLAY_UNIMPLEMENTED("IMFPMediaPlayer::FrameStep", "the host media backend cannot step one frame");
            post_event(p, Event{EventType::FrameStep, MF_E_UNSUPPORTED_RATE, p->state, p->current, 0});
            return S_OK;
        case P_SetPosition: {
            const GuestAddr type = m.arg_ptr(1);
            const GuestAddr pv = m.arg_ptr(2);
            if (pv == 0 || type == 0) return E_POINTER;
            if (p->shut_down) return MF_E_SHUTDOWN;
            Guid g;
            if (!read_guid(m, type, g) || !(g == MFP_POSITIONTYPE_100NS)) return E_INVALIDARG;
            if (m.memory().read_u16(pv).value_or(0) != VT_I8) return E_INVALIDARG;
            const i64 value = static_cast<i64>(m.memory().read_u64(pv + 8).value_or(0));
            if (value < 0) return MF_E_INVALID_POSITION;  // measured, synchronous
            if (!p->current || !p->current->media) {
                post_event(p, Event{EventType::PositionSet, MF_E_INVALIDREQUEST, p->state, nullptr, 0});
                return S_OK;
            }
            Item& item = *p->current;
            const i64 start = item.start.value_or(0);
            const i64 end = item.stop.value_or(item.duration);
            u32 hr = S_OK;
            if ((item.duration > 0 && start + value > end) || !item.media->seek_100ns(start + value)) {
                hr = MF_E_INVALID_POSITION;
            } else if (p->state == State::Paused) {
                present_frame(*p, true);  // Learn: a paused player refreshes the frame
            }
            post_event(p, Event{EventType::PositionSet, hr, p->state, p->current, 0});
            return S_OK;
        }
        case P_GetPosition: {
            const GuestAddr pv = m.arg_ptr(2);
            if (pv == 0) return E_POINTER;
            clear_pv(m, pv);  // measured: VT_EMPTY on every failure
            if (p->shut_down) return MF_E_SHUTDOWN;
            Guid g;
            if (!read_guid(m, m.arg_ptr(1), g) || !(g == MFP_POSITIONTYPE_100NS)) return E_INVALIDARG;
            if (!p->current || !p->current->media) return MF_E_INVALIDREQUEST;
            // Learn: relative to the item's start time.
            const i64 pos = p->current->media->position_100ns() - p->current->start.value_or(0);
            write_pv_64(m, pv, VT_I8, static_cast<u64>(std::max<i64>(pos, 0)));
            return S_OK;
        }
        case P_GetDuration: {
            const GuestAddr pv = m.arg_ptr(2);
            if (pv == 0) return E_POINTER;
            if (p->shut_down) return MF_E_SHUTDOWN;
            Guid g;
            if (!read_guid(m, m.arg_ptr(1), g) || !(g == MFP_POSITIONTYPE_100NS)) return E_INVALIDARG;
            // Measured: the PROPVARIANT is left untouched with no item.
            if (!p->current) return MF_E_INVALIDREQUEST;
            if (p->current->duration <= 0) return MF_E_ATTRIBUTENOTFOUND;  // Learn: no duration
            // Learn: VT_UI8, the playback window after start/stop times.
            const i64 len = p->current->stop.value_or(p->current->duration) - p->current->start.value_or(0);
            write_pv_64(m, pv, VT_UI8, static_cast<u64>(std::max<i64>(len, 0)));
            return S_OK;
        }
        case P_SetRate: {
            if (p->shut_down) return MF_E_SHUTDOWN;
            const float rate = m.arg_f32(1);
            if (rate == 0.0f || std::isnan(rate)) return MF_E_OUT_OF_RANGE;
            if (!p->current) {
                // Measured with no item: 1.0 is accepted, nothing else.
                const bool one = rate == 1.0f;
                post_event(p, Event{EventType::RateSet, one ? S_OK : MF_E_UNSUPPORTED_RATE, p->state,
                                    nullptr, one ? f32_bits(1.0f) : 0u});
                return S_OK;
            }
            if (rate < 0.0f) {
                // GetSupportedRates(FALSE) has no reverse range.
                post_event(p, Event{EventType::RateSet, MF_E_UNSUPPORTED_RATE, p->state, p->current, 0});
                return S_OK;
            }
            // Learn: the nearest supported rate is set and reported in flRate.
            if (rate != 1.0f) {
                MFPLAY_UNIMPLEMENTED("IMFPMediaPlayer::SetRate",
                                     "rates other than 1.0 (the host media backend has no rate control)");
            }
            p->rate = 1.0f;
            post_event(p, Event{EventType::RateSet, S_OK, p->state, p->current, f32_bits(1.0f)});
            return S_OK;
        }
        case P_GetRate: {
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) return E_POINTER;
            if (p->shut_down) return MF_E_SHUTDOWN;
            write_f32(m, out, p->rate);
            return S_OK;
        }
        case P_GetSupportedRates: {
            const bool forward = m.arg32(1) != 0;
            const GuestAddr slowest = m.arg_ptr(2);
            const GuestAddr fastest = m.arg_ptr(3);
            if (p->shut_down) return MF_E_SHUTDOWN;
            if (!p->current || !forward) return MF_E_UNSUPPORTED_RATE;  // outputs untouched (measured)
            if (slowest == 0 || fastest == 0) return E_POINTER;
            write_f32(m, slowest, 1.0f);
            write_f32(m, fastest, 1.0f);
            return S_OK;
        }
        case P_GetState: {
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) return E_POINTER;
            (void)m.write_u32(out, static_cast<u32>(p->reported_state));
            return S_OK;
        }
        default:
            return E_NOTIMPL;
    }
}

// ---------------------------------------------------------------------------
// Media items
// ---------------------------------------------------------------------------

u32 MfPlayManager::create_item_from_url(Marshaler& m, const PlayerPtr& p, GuestAddr url,
                                        bool sync, u64 user_data, GuestAddr out) {
    if (url == 0) {
        if (out != 0) (void)m.write_ptr(out, 0);
        return E_POINTER;
    }
    if (p->shut_down) {
        if (out != 0) (void)m.write_ptr(out, 0);
        return MF_E_SHUTDOWN;
    }
    if (!sync) {
        // Measured: no callback -> MF_E_INVALIDREQUEST, ppMediaItem untouched.
        if (p->callback == 0) return MF_E_INVALIDREQUEST;
        if (out != 0) return E_INVALIDARG;  // Learn: must be NULL when async
    } else if (out == 0) {
        return E_POINTER;
    }
    auto text = m.read_wide_string(url);
    if (text.is_err()) return E_POINTER;
    ItemPtr item = new_item(p, user_data);
    if (!item) return E_OUTOFMEMORY;
    if (sync) {
        const u32 hr = open_url(*item, text.value());
        if (hr != S_OK) {
            destroy_item(item);
            (void)m.write_ptr(out, 0);
            return hr;
        }
        (void)m.write_ptr(out, item->cell);
        return S_OK;
    }
    // Asynchronous: open on a worker, report MEDIAITEM_CREATED (pMediaItem
    // NULL and the error in hrEvent on failure, measured).
    std::thread worker([this, p, item, url_text = text.value(), user_data] {
        const u32 hr = open_url(*item, url_text);
        ItemPtr result = item;
        if (hr != S_OK) {
            destroy_item(item);
            result = nullptr;
        }
        std::lock_guard<std::recursive_mutex> lock(p->mutex);
        Event ev{EventType::MediaItemCreated, hr, p->state, result, user_data};
        ev.owns_item_ref = result != nullptr;
        post_event(p, std::move(ev));
    });
    std::lock_guard<std::mutex> lock(m_mutex);
    m_workers.push_back(std::move(worker));
    return S_OK;
}

u32 MfPlayManager::set_media_item(Marshaler& m, const PlayerPtr& p, const ItemPtr& item) {
    (void)m;
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    // Measured: with every stream deselected the topology is refused
    // synchronously and no event follows.
    if (std::none_of(item->selected.begin(), item->selected.end(), [](bool s) { return s; })) {
        return MF_E_TOPO_UNSUPPORTED;
    }
    // Learn InsertEffect: a required effect that cannot be added is a playback
    // error; an optional one is skipped.
    bool required_effect = false;
    for (const auto& e : p->effects) {
        if (e.optional) {
            MFPLAY_UNIMPLEMENTED("IMFPMediaPlayer::SetMediaItem", "optional effects are skipped (no MFT pipeline)");
        } else {
            required_effect = true;
        }
    }
    if (required_effect) {
        MFPLAY_UNIMPLEMENTED("IMFPMediaPlayer::SetMediaItem", "required effects cannot be added (no MFT pipeline)");
        item_add_ref(item);  // pMediaItem stays valid until delivered
        Event ev{EventType::MediaItemSet, MF_E_TOPO_UNSUPPORTED, p->state, item, 0};
        ev.owns_item_ref = true;
        post_event(p, std::move(ev));
        return S_OK;
    }
    ItemPtr previous = p->current;
    if (previous && previous->media) previous->media->stop();
    if (p->video_shown && m_host.hide_video) m_host.hide_video(p->video_hwnd);
    p->video_shown = false;
    p->last_frame = ~u64{0};
    if (previous != item) {
        item_add_ref(item);
        p->current = item;
        if (previous) item_release(previous);
    }
    p->current_visible = false;
    p->state = State::Stopped;
    p->rate = 1.0f;  // Learn: a new item resets the rate
    if (item->media) (void)item->media->seek_100ns(item->start.value_or(0));
    apply_audio(*p);
    post_event(p, Event{EventType::MediaItemSet, S_OK, State::Stopped, item, 0});
    return S_OK;
}

u32 MfPlayManager::player_items(u32 slot, Marshaler& m, const PlayerPtr& p) {
    switch (slot) {
        case P_CreateMediaItemFromURL:
            return create_item_from_url(m, p, m.arg_ptr(1), m.arg32(2) != 0, m.arg_ptr(3), m.arg_ptr(4));
        case P_CreateMediaItemFromObject: {
            const GuestAddr object = m.arg_ptr(1);
            const bool sync = m.arg32(2) != 0;
            const GuestAddr out = m.arg_ptr(4);
            if (object == 0) return E_POINTER;  // measured: ppMediaItem untouched
            if (p->shut_down) return MF_E_SHUTDOWN;
            if (!sync && p->callback == 0) return MF_E_INVALIDREQUEST;
            if (!sync && out != 0) return E_INVALIDARG;
            if (sync && out == 0) return E_POINTER;
            MFPLAY_UNIMPLEMENTED("IMFPMediaPlayer::CreateMediaItemFromObject",
                                 "no IMFMediaSource / IMFByteStream source path");
            if (out != 0) (void)m.write_ptr(out, 0);
            return E_NOTIMPL;
        }
        case P_SetMediaItem: {
            const GuestAddr cell = m.arg_ptr(1);
            if (cell == 0) return E_POINTER;
            if (p->shut_down) return MF_E_SHUTDOWN;
            ItemPtr item = find_item(cell);
            // Learn/measured: an item from another player is E_INVALIDARG.
            if (!item || item->owner.lock() != p) return E_INVALIDARG;
            return set_media_item(m, p, item);
        }
        case P_ClearMediaItem: {
            std::lock_guard<std::recursive_mutex> lock(p->mutex);
            if (p->shut_down) return MF_E_SHUTDOWN;
            ItemPtr cleared = std::move(p->current);
            p->current_visible = false;
            if (cleared && cleared->media) cleared->media->stop();
            if (p->video_shown && m_host.hide_video) m_host.hide_video(p->video_hwnd);
            p->video_shown = false;
            p->state = State::Empty;
            // The player's reference moves into the event (pMediaItem is the
            // cleared item) and is dropped once it is delivered.
            Event ev{EventType::MediaItemCleared, S_OK, State::Empty, cleared, 0};
            ev.owns_item_ref = cleared != nullptr;
            post_event(p, std::move(ev));
            return S_OK;
        }
        case P_GetMediaItem: {
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) return E_POINTER;
            (void)m.write_ptr(out, 0);
            std::lock_guard<std::recursive_mutex> lock(p->mutex);
            if (p->shut_down) return MF_E_SHUTDOWN;
            // Measured MF_E_NOT_FOUND; Learn: not before MEDIAITEM_SET.
            if (!p->current || !p->current_visible) return MF_E_NOT_FOUND;
            item_add_ref(p->current);
            (void)m.write_ptr(out, p->current->cell);
            return S_OK;
        }
        default:
            return E_NOTIMPL;
    }
}

// ---------------------------------------------------------------------------
// Volume, balance, mute (usable after Shutdown, measured)
// ---------------------------------------------------------------------------

void MfPlayManager::apply_audio(Player& p) {
    if (!p.current || !p.current->media) return;
    Item& item = *p.current;
    bool audio_selected = false;
    for (usize i = 0; i < item.stream_is_video.size(); ++i) {
        if (!item.stream_is_video[i] && item.selected[i]) audio_selected = true;
    }
    // A deselected audio stream is not rendered.
    const bool silent = p.mute != 0 || !audio_selected;
    item.media->set_volume(silent ? -10000 : amplitude_to_hundredth_db(p.volume));
    // Balance -1 (left only) .. +1 (right only): attenuate the other channel
    // linearly. The backend's pan is DirectSound's: negative silences right.
    i32 pan = 0;
    if (p.balance < 0.0f) pan = amplitude_to_hundredth_db(1.0 + p.balance);
    else if (p.balance > 0.0f) pan = -amplitude_to_hundredth_db(1.0 - p.balance);
    item.media->set_balance(pan);
}

u32 MfPlayManager::player_audio(u32 slot, Marshaler& m, const PlayerPtr& p) {
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    switch (slot) {
        case P_GetVolume:
        case P_GetBalance: {
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) return E_POINTER;
            write_f32(m, out, slot == P_GetVolume ? p->volume : p->balance);
            return S_OK;
        }
        case P_SetVolume: {
            const float v = m.arg_f32(1);
            if (!(v >= 0.0f && v <= 1.0f)) return MF_E_OUT_OF_RANGE;
            p->volume = v;
            apply_audio(*p);
            return S_OK;
        }
        case P_SetBalance: {
            const float v = m.arg_f32(1);
            if (!(v >= -1.0f && v <= 1.0f)) return MF_E_OUT_OF_RANGE;
            p->balance = v;
            apply_audio(*p);
            return S_OK;
        }
        case P_GetMute: {
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) return E_POINTER;
            (void)m.write_u32(out, p->mute);  // the BOOL as given (measured 2 -> 2)
            return S_OK;
        }
        case P_SetMute:
            p->mute = m.arg32(1);
            apply_audio(*p);
            return S_OK;
        default:
            return E_NOTIMPL;
    }
}

// ---------------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------------

u32 MfPlayManager::player_video(u32 slot, Marshaler& m, const PlayerPtr& p) {
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (slot == P_GetVideoWindow) {
        // Measured: the hWnd given at creation, even after Shutdown.
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return E_POINTER;
        (void)m.write_ptr(out, p->video_hwnd);
        return S_OK;
    }
    if (p->shut_down) return MF_E_SHUTDOWN;
    // Measured: SetVideoSourceRect without a video window fails with
    // ERROR_INVALID_WINDOW_HANDLE before any other check.
    if (slot == P_SetVideoSourceRect && p->video_hwnd == 0) return HRESULT_INVALID_WINDOW_HANDLE;
    // Learn/measured: MF_E_INVALIDREQUEST with no item or an item without video.
    if (!p->current || !p->current->has_video) return MF_E_INVALIDREQUEST;
    const Item& item = *p->current;
    const GuestAddr a1 = m.arg_ptr(1);
    switch (slot) {
        case P_GetNativeVideoSize: {
            const GuestAddr ar = m.arg_ptr(2);
            if (a1 == 0 && ar == 0) return E_POINTER;
            const u32 w = item.video_width, h = item.video_height;
            if (a1 != 0) {
                (void)m.write_u32(a1, w);
                (void)m.write_u32(a1 + 4, h);
            }
            if (ar != 0) {
                // Picture aspect ratio; the backend reports square pixels.
                const u32 g = std::max<u32>(std::gcd(w, h), 1);
                (void)m.write_u32(ar, w / g);
                (void)m.write_u32(ar + 4, h / g);
            }
            return S_OK;
        }
        case P_GetIdealVideoSize:
            MFPLAY_UNIMPLEMENTED("IMFPMediaPlayer::GetIdealVideoSize", "no EVR mixer to report ideal sizes");
            return E_NOTIMPL;
        case P_SetVideoSourceRect: {
            if (a1 == 0) return E_POINTER;
            std::array<float, 4> r{};
            for (u32 i = 0; i < 4; ++i) r[i] = read_f32(m, a1 + i * 4);
            if (!(r[0] >= 0.0f && r[0] < r[2] && r[2] <= 1.0f && r[1] >= 0.0f && r[1] < r[3] &&
                  r[3] <= 1.0f)) {
                return E_INVALIDARG;
            }
            p->source_rect = r;
            if (p->state == State::Playing || p->state == State::Paused) present_frame(*p, true);
            return S_OK;
        }
        case P_GetVideoSourceRect:
            if (a1 == 0) return E_POINTER;
            for (u32 i = 0; i < 4; ++i) write_f32(m, a1 + i * 4, p->source_rect[i]);
            return S_OK;
        case P_SetAspectRatioMode: {
            const u32 mode = m.arg32(1);
            if ((mode & ~MFVideoARMode_Mask) != 0) return E_INVALIDARG;
            p->aspect_mode = mode;
            if (p->state == State::Playing || p->state == State::Paused) present_frame(*p, true);
            return S_OK;
        }
        case P_GetAspectRatioMode:
            if (a1 == 0) return E_POINTER;
            (void)m.write_u32(a1, p->aspect_mode);
            return S_OK;
        case P_UpdateVideo:
            // Learn: repaint the current frame and fit it to the window.
            if (p->state == State::Playing || p->state == State::Paused) present_frame(*p, true);
            return S_OK;
        case P_SetBorderColor:
            p->border_color = m.arg32(1);
            if (p->state == State::Playing || p->state == State::Paused) present_frame(*p, true);
            return S_OK;
        case P_GetBorderColor:
            if (a1 == 0) return E_POINTER;
            (void)m.write_u32(a1, p->border_color);
            return S_OK;
        default:
            return E_NOTIMPL;
    }
}

// ---------------------------------------------------------------------------
// Effects, Shutdown
// ---------------------------------------------------------------------------

u32 MfPlayManager::player_effects(u32 slot, Marshaler& m, const PlayerPtr& p) {
    const auto unknown_call = [&](GuestAddr punk, u32 method) {
        if (!m_host.call_guest || m_memory == nullptr) return;
        const u32 ptr = m_is64 ? 8u : 4u;
        const u64 vt = m.read_ptr(punk).value_or(0);
        const u64 fn = m.read_ptr(vt + method * ptr).value_or(0);
        const u64 args[1] = {punk};
        (void)m_host.call_guest(fn, args, 1);
    };
    switch (slot) {
        case P_InsertEffect: {
            const GuestAddr effect = m.arg_ptr(1);
            const bool optional = m.arg32(2) != 0;
            if (effect == 0) return E_POINTER;
            {
                std::lock_guard<std::recursive_mutex> lock(p->mutex);
                if (p->shut_down) return MF_E_SHUTDOWN;
                for (const auto& e : p->effects) {
                    if (e.punk == effect) return MF_E_INVALIDINDEX;  // measured duplicate
                }
                p->effects.push_back(Player::Effect{effect, optional});
            }
            unknown_call(effect, 1);  // AddRef: held until removed
            return S_OK;
        }
        case P_RemoveEffect: {
            const GuestAddr effect = m.arg_ptr(1);
            if (effect == 0) return E_POINTER;
            {
                std::lock_guard<std::recursive_mutex> lock(p->mutex);
                auto it = std::find_if(p->effects.begin(), p->effects.end(),
                                       [&](const Player::Effect& e) { return e.punk == effect; });
                if (it == p->effects.end()) return MF_E_NOT_FOUND;  // Learn
                p->effects.erase(it);
            }
            unknown_call(effect, 2);
            return S_OK;
        }
        case P_RemoveAllEffects: {
            std::vector<Player::Effect> effects;
            {
                std::lock_guard<std::recursive_mutex> lock(p->mutex);
                effects.swap(p->effects);
            }
            for (const auto& e : effects) unknown_call(e.punk, 2);
            return S_OK;
        }
        case P_Shutdown:
            shutdown_player(p, false);  // a second Shutdown is S_OK (measured)
            return S_OK;
        default:
            return E_NOTIMPL;
    }
}

} // namespace vitreo::winapi
