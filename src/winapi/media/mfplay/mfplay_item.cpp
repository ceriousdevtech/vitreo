// SPDX-License-Identifier: LGPL-2.1-only

/// @file mfplay_item.cpp
/// @brief IMFPMediaItem. Return codes come from Learn and
/// mfplay_probe{32,64}.expected (see mfplay.hpp).

#include "winapi/media/mfplay/mfplay_internal.hpp"

#include <algorithm>

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

void clear_pv(Marshaler& m, GuestAddr pv) {
    const std::array<Byte, 24> zero{};
    (void)m.memory().write_bytes(pv, zero.data(), propvariant_size(m.cpu_is_64bit()));
}

void write_pv_64(Marshaler& m, GuestAddr pv, u16 vt, u64 value) {
    clear_pv(m, pv);
    (void)m.memory().write_u16(pv, vt);
    (void)m.memory().write_u64(pv + 8, value);
}

} // namespace

u32 MfPlayManager::dispatch_item(u32 slot, Marshaler& m) {
    ItemPtr it = find_item(m.arg_ptr(0));
    if (!it) return E_POINTER;  // not an object this DLL handed out
    return item_methods(slot, m, it);
}

u32 MfPlayManager::item_methods(u32 slot, Marshaler& m, const ItemPtr& it) {
    PlayerPtr owner = it->owner.lock();
    // Learn Shutdown: the player's items are invalidated with it (measured:
    // everything but Get/SetUserData returns MF_E_SHUTDOWN).
    const bool shut = !owner || owner->shut_down;
    const GuestAddr a1 = m.arg_ptr(1);
    switch (slot) {
        case I_QueryInterface: {
            const GuestAddr ppv = m.arg_ptr(2);
            if (ppv == 0) return E_POINTER;
            Guid iid;
            if (read_guid(m, a1, iid) && (iid == IID_IUnknown || iid == IID_IMFPMediaItem)) {
                item_add_ref(it);
                (void)m.write_ptr(ppv, it->cell);
                return S_OK;
            }
            (void)m.write_ptr(ppv, 0);
            return E_NOINTERFACE;
        }
        case I_AddRef: {
            std::lock_guard<std::mutex> lock(m_mutex);
            return ++it->refs;
        }
        case I_Release: {
            u32 left = 0;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (it->refs == 0) return 0;
                left = --it->refs;
            }
            if (left == 0) destroy_item(it);
            return left;
        }
        case I_GetMediaPlayer: {
            if (a1 == 0) return E_POINTER;
            (void)m.write_ptr(a1, 0);
            if (shut) return MF_E_SHUTDOWN;
            {
                std::lock_guard<std::recursive_mutex> lock(owner->mutex);
                ++owner->refs;
            }
            (void)m.write_ptr(a1, owner->cell);
            return S_OK;
        }
        case I_GetURL: {
            if (a1 == 0) return E_POINTER;
            (void)m.write_ptr(a1, 0);
            if (shut) return MF_E_SHUTDOWN;
            if (!m_host.task_alloc) return E_OUTOFMEMORY;
            // Freed by the caller with CoTaskMemFree (Learn).
            const GuestAddr s = m_host.task_alloc((it->url.size() + 1) * 2);
            if (s == 0) return E_OUTOFMEMORY;
            for (usize i = 0; i <= it->url.size(); ++i) {
                (void)m.memory().write_u16(s + i * 2, i < it->url.size() ? static_cast<u16>(it->url[i]) : 0);
            }
            (void)m.write_ptr(a1, s);
            return S_OK;
        }
        case I_GetObject:
            if (a1 == 0) return E_POINTER;
            (void)m.write_ptr(a1, 0);
            if (shut) return MF_E_SHUTDOWN;
            return MF_E_NOT_FOUND;  // every item here comes from a URL (measured)
        case I_GetUserData:
            if (a1 == 0) return E_POINTER;
            (void)m.write_ptr(a1, it->user_data);
            return S_OK;
        case I_SetUserData:
            it->user_data = a1;
            return S_OK;
        case I_GetStartStopPosition: {
            const GuestAddr start_type = a1, start_pv = m.arg_ptr(2);
            const GuestAddr stop_type = m.arg_ptr(3), stop_pv = m.arg_ptr(4);
            // Each GUID/PROPVARIANT pair is both NULL or both set; all NULL is
            // E_POINTER (measured).
            if ((start_type == 0) != (start_pv == 0) || (stop_type == 0) != (stop_pv == 0) ||
                (start_type == 0 && stop_type == 0)) {
                return E_POINTER;
            }
            if (shut) return MF_E_SHUTDOWN;
            const auto put = [&](GuestAddr type, GuestAddr pv, const std::optional<i64>& v) {
                if (type == 0) return;
                // Unset: GUID_NULL and VT_EMPTY; set: MFP_POSITIONTYPE_100NS
                // (also all-zero) and VT_I8 (measured).
                (void)m.write_struct<Guid>(type, MFP_POSITIONTYPE_100NS);
                if (v) write_pv_64(m, pv, VT_I8, static_cast<u64>(*v));
                else clear_pv(m, pv);
            };
            put(start_type, start_pv, it->start);
            put(stop_type, stop_pv, it->stop);
            return S_OK;
        }
        case I_SetStartStopPosition: {
            const GuestAddr start_type = a1, start_pv = m.arg_ptr(2);
            const GuestAddr stop_type = m.arg_ptr(3), stop_pv = m.arg_ptr(4);
            if ((start_type == 0) != (start_pv == 0) || (stop_type == 0) != (stop_pv == 0) ||
                (start_type == 0 && stop_type == 0)) {
                return E_POINTER;
            }
            if (shut) return MF_E_SHUTDOWN;
            // A NULL pair leaves that end unchanged; VT_EMPTY clears it; only
            // MFP_POSITIONTYPE_100NS with VT_I8 sets it (Learn, measured).
            const auto parse = [&](GuestAddr type, GuestAddr pv, std::optional<i64> current,
                                   std::optional<i64>& out) -> u32 {
                out = current;
                if (type == 0) return S_OK;
                Guid g;
                if (!read_guid(m, type, g) || !(g == MFP_POSITIONTYPE_100NS)) return E_INVALIDARG;
                const u16 vt = m.memory().read_u16(pv).value_or(0xFFFF);
                if (vt == VT_EMPTY) {
                    out.reset();
                    return S_OK;
                }
                if (vt != VT_I8) return E_INVALIDARG;
                out = static_cast<i64>(m.memory().read_u64(pv + 8).value_or(0));
                return S_OK;
            };
            std::optional<i64> start, stop;
            if (u32 hr = parse(start_type, start_pv, it->start, start); hr != S_OK) return hr;
            if (u32 hr = parse(stop_type, stop_pv, it->stop, stop); hr != S_OK) return hr;
            // Learn: below zero, past the duration, or stop before start.
            const auto bad = [&](const std::optional<i64>& v) {
                return v && (*v < 0 || (it->duration > 0 && *v > it->duration));
            };
            if (bad(start) || bad(stop) || (start && stop && *stop < *start)) return MF_E_OUT_OF_RANGE;
            it->start = start;
            it->stop = stop;
            return S_OK;
        }
        case I_HasVideo:
        case I_HasAudio: {
            const GuestAddr has = a1, selected = m.arg_ptr(2);
            if (has != 0) (void)m.write_u32(has, 0);
            if (selected != 0) (void)m.write_u32(selected, 0);
            if (shut) return MF_E_SHUTDOWN;
            const bool want_video = slot == I_HasVideo;
            bool any = false, any_selected = false;
            for (usize i = 0; i < it->stream_is_video.size(); ++i) {
                if (it->stream_is_video[i] != want_video) continue;
                any = true;
                any_selected = any_selected || it->selected[i];
            }
            if (has != 0) (void)m.write_u32(has, any ? 1u : 0u);
            if (selected != 0) (void)m.write_u32(selected, any_selected ? 1u : 0u);
            return S_OK;
        }
        case I_IsProtected:
            if (a1 == 0) return E_POINTER;
            if (shut) return MF_E_SHUTDOWN;
            // Measured: S_FALSE with FALSE for unprotected content. Protected
            // content never opens here.
            (void)m.write_u32(a1, 0);
            return S_FALSE;
        case I_GetDuration: {
            const GuestAddr pv = m.arg_ptr(2);
            if (pv == 0) return E_POINTER;
            clear_pv(m, pv);
            if (shut) return MF_E_SHUTDOWN;
            Guid g;
            if (!read_guid(m, a1, g) || !(g == MFP_POSITIONTYPE_100NS)) return E_INVALIDARG;
            if (it->duration <= 0) return MF_E_ATTRIBUTENOTFOUND;
            // Measured VT_UI8 (Learn says VT_I8); the whole content.
            write_pv_64(m, pv, VT_UI8, static_cast<u64>(it->duration));
            return S_OK;
        }
        case I_GetNumberOfStreams:
            if (a1 == 0) return E_POINTER;
            (void)m.write_u32(a1, 0);
            if (shut) return MF_E_SHUTDOWN;
            (void)m.write_u32(a1, static_cast<u32>(it->stream_is_video.size()));
            return S_OK;
        case I_GetStreamSelection: {
            const GuestAddr out = m.arg_ptr(2);
            if (out == 0) return E_POINTER;
            (void)m.write_u32(out, 0);
            if (shut) return MF_E_SHUTDOWN;
            const u32 index = m.arg32(1);
            if (index >= it->selected.size()) return E_INVALIDARG;  // measured
            (void)m.write_u32(out, it->selected[index] ? 1u : 0u);
            return S_OK;
        }
        case I_SetStreamSelection: {
            if (shut) return MF_E_SHUTDOWN;
            const u32 index = m.arg32(1);
            if (index >= it->selected.size()) return E_INVALIDARG;
            // Learn: takes effect at the next SetMediaItem.
            it->selected[index] = m.arg32(2) != 0;
            return S_OK;
        }
        case I_GetStreamAttribute: {
            const u32 index = m.arg32(1);
            const GuestAddr key = m.arg_ptr(2);
            const GuestAddr pv = m.arg_ptr(3);
            if (pv == 0) return E_POINTER;
            clear_pv(m, pv);
            if (shut) return MF_E_SHUTDOWN;
            if (index >= it->stream_is_video.size()) return E_INVALIDARG;
            Guid g;
            if (!read_guid(m, key, g)) return E_POINTER;
            const bool video = it->stream_is_video[index];
            if (g == MF_MT_MAJOR_TYPE) {
                if (!m_host.task_alloc) return E_OUTOFMEMORY;
                const GuestAddr clsid = m_host.task_alloc(16);  // PropVariantClear frees puuid
                if (clsid == 0) return E_OUTOFMEMORY;
                (void)m.write_struct<Guid>(clsid, video ? MFMediaType_Video : MFMediaType_Audio);
                clear_pv(m, pv);
                (void)m.memory().write_u16(pv, VT_CLSID);
                (void)m.write_ptr(pv + 8, clsid);
                return S_OK;
            }
            if (g == MF_MT_FRAME_SIZE && video) {
                write_pv_64(m, pv, VT_UI8, (u64{it->video_width} << 32) | it->video_height);
                return S_OK;
            }
            MFPLAY_UNIMPLEMENTED("IMFPMediaItem::GetStreamAttribute",
                                 "only MF_MT_MAJOR_TYPE and MF_MT_FRAME_SIZE are reported");
            return MF_E_ATTRIBUTENOTFOUND;
        }
        case I_GetPresentationAttribute: {
            const GuestAddr pv = m.arg_ptr(2);
            if (pv == 0) return E_POINTER;
            clear_pv(m, pv);
            if (shut) return MF_E_SHUTDOWN;
            Guid g;
            if (!read_guid(m, a1, g)) return E_POINTER;
            if (g == MF_PD_DURATION && it->duration > 0) {
                write_pv_64(m, pv, VT_UI8, static_cast<u64>(it->duration));  // measured VT_UI8
                return S_OK;
            }
            if (g == MF_PD_TOTAL_FILE_SIZE) {
                write_pv_64(m, pv, VT_UI8, it->file_size);
                return S_OK;
            }
            MFPLAY_UNIMPLEMENTED("IMFPMediaItem::GetPresentationAttribute",
                                 "only MF_PD_DURATION and MF_PD_TOTAL_FILE_SIZE are reported");
            return MF_E_ATTRIBUTENOTFOUND;
        }
        case I_GetCharacteristics:
            if (a1 == 0) return E_POINTER;
            if (shut) return MF_E_SHUTDOWN;
            // A file: seekable and pausable, not live (measured 0x6 for a WAV).
            (void)m.write_u32(a1, MFP_MEDIAITEM_CAN_SEEK | MFP_MEDIAITEM_CAN_PAUSE);
            return S_OK;
        case I_SetStreamSink:
            if (shut) return MF_E_SHUTDOWN;
            // NULL restores the default renderer: S_OK for any index (measured).
            if (m.arg_ptr(2) == 0) return S_OK;
            MFPLAY_UNIMPLEMENTED("IMFPMediaItem::SetStreamSink", "custom IMFStreamSink / IMFActivate sinks");
            return E_NOTIMPL;
        case I_GetMetadata:
            if (a1 == 0) return E_POINTER;
            (void)m.write_ptr(a1, 0);
            if (shut) return MF_E_SHUTDOWN;
            MFPLAY_UNIMPLEMENTED("IMFPMediaItem::GetMetadata", "no metadata property store");
            return E_NOTIMPL;
        default:
            return E_NOTIMPL;
    }
}

} // namespace vitreo::winapi
