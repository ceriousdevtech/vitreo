// SPDX-License-Identifier: LGPL-2.1-only
/// @file combase_smtc_display.cpp
/// @brief SMTC DisplayUpdater, MusicDisplayProperties and now-playing publication

#include "winapi/com/combase/combase.hpp"
#include "winapi/com/combase/combase_internal.hpp"

namespace vitreo::winapi {

// ---------------------------------------------------------------------------
// SystemMediaTransportControlsDisplayUpdater / MusicDisplayProperties
//
// This is the metadata half of SMTC: the guest fills in the track, then calls
// Update() to publish it. Vitreo forwards that to the host now-playing surface
// so the Mac shows what the guest is playing, and ClearAll() withdraws it.
//
// Vtable order is MIDL declaration order. Note ClearAll comes BEFORE Update —
// the reference docs list members alphabetically and would invert them.
// ---------------------------------------------------------------------------
namespace {
std::string utf16_to_utf8(const std::u16string& text) {
    std::string out;
    out.reserve(text.size());
    for (usize i = 0; i < text.size(); ++i) {
        char32_t code = text[i];
        if (code >= 0xD800 && code <= 0xDBFF && i + 1 < text.size() &&
            text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
            code = 0x10000 + ((code - 0xD800) << 10) + (text[i + 1] - 0xDC00);
            ++i;
        }
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return out;
}
} // namespace

void CombaseManager::publish_now_playing(const Object& updater) {
    if (m_now_playing_host == nullptr) return;
    hardware::NowPlayingInfo info{};
    auto music = m_objects.find(updater.du_music);
    if (music != m_objects.end()) {
        info.title = utf16_to_utf8(music->second.music_fields[0]);
        info.album_artist = utf16_to_utf8(music->second.music_fields[1]);
        info.artist = utf16_to_utf8(music->second.music_fields[2]);
    }
    info.app_media_id = utf16_to_utf8(updater.du_app_media_id);
    auto owner = m_objects.find(updater.du_owner);
    info.status = owner == m_objects.end()
                      ? hardware::NowPlayingStatus::Closed
                      : static_cast<hardware::NowPlayingStatus>(
                            owner->second.smtc_playback_status);
    if (owner != m_objects.end() && owner->second.smtc_has_timeline) {
        // Position within [StartTime, EndTime], in seconds. The host advances
        // elapsed time itself while playing: at PlaybackRate, or at 1 when the
        // app never set one (it starts at 0 on Windows too).
        const auto& t = owner->second.smtc_timeline;  // start, end, min, max, pos
        info.has_timeline = true;
        info.duration_seconds = static_cast<double>(t[1] - t[0]) / 1e7;
        info.elapsed_seconds = static_cast<double>(t[4] - t[0]) / 1e7;
        const double rate = owner->second.smtc_playback_rate;
        info.rate = info.status == hardware::NowPlayingStatus::Playing ? (rate > 0 ? rate : 1.0)
                                                                       : 0.0;
    }
    VITREO_TRACE(
        "[combase] DisplayUpdater.Update -> host now playing: title='%s' "
        "artist='%s' album_artist='%s' status=%u\n",
        info.title.c_str(), info.artist.c_str(), info.album_artist.c_str(),
        static_cast<unsigned>(info.status));
    (void)m_now_playing_host->update(info);
}

void CombaseManager::impl_DuGetType(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) { m.set_return(E_POINTER); return; }
    auto it = m_objects.find(m.arg_ptr(0));
    if (it == m_objects.end() || it->second.type != ObjectType::DisplayUpdater) {
        m.set_return(E_INVALIDARG);
        return;
    }
    m.set_return(m.write_u32(out, it->second.du_type).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_DuPutType(Marshaler& m) {
    auto it = m_objects.find(m.arg_ptr(0));
    if (it == m_objects.end() || it->second.type != ObjectType::DisplayUpdater) {
        m.set_return(E_INVALIDARG);
        return;
    }
    it->second.du_type = static_cast<u32>(m.arg(1));
    m.set_return(S_OK);
}

void CombaseManager::impl_DuGetAppMediaId(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) { m.set_return(E_POINTER); return; }
    auto it = m_objects.find(m.arg_ptr(0));
    if (it == m_objects.end() || it->second.type != ObjectType::DisplayUpdater) {
        m.set_return(E_INVALIDARG);
        return;
    }
    m.set_return(m.write_ptr(out, create_hstring(it->second.du_app_media_id,
                                                 m.cpu_is_64bit()))
                         .is_ok()
                     ? S_OK
                     : E_POINTER);
}

void CombaseManager::impl_DuPutAppMediaId(Marshaler& m) {
    auto it = m_objects.find(m.arg_ptr(0));
    if (it == m_objects.end() || it->second.type != ObjectType::DisplayUpdater) {
        m.set_return(E_INVALIDARG);
        return;
    }
    std::u16string value;
    if (const GuestAddr text = m.arg_ptr(1); text != 0) {
        auto read = read_hstring(text, m.cpu_is_64bit());
        if (read.has_value()) value = std::move(*read);
    }
    it->second.du_app_media_id = std::move(value);
    m.set_return(S_OK);
}

void CombaseManager::impl_DuGetThumbnail(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) { m.set_return(E_POINTER); return; }
    // No thumbnail is ever stored, and a null RandomAccessStreamReference is
    // the documented "none set" value rather than an error.
    m.set_return(m.write_ptr(out, 0).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_DuPutThumbnail(Marshaler& m) {
    (void)m.arg_ptr(1);
    // Accepting the reference without a way to read its bytes would claim
    // artwork was published; RandomAccessStreamReference is not modelled.
    VITREO_TRACE(
        "[combase] DisplayUpdater.Thumbnail refused: "
        "RandomAccessStreamReference is not modelled\n");
    m.set_return(E_NOTIMPL);
}

void CombaseManager::impl_DuGetMusicProperties(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    auto it = m_objects.find(m.arg_ptr(0));
    if (it == m_objects.end() || it->second.type != ObjectType::DisplayUpdater) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (it->second.du_music == 0) {
        Object music{};
        music.type = ObjectType::MusicDisplayProperties;
        music.ref_count = 1;
        const GuestAddr created =
            allocate_object(std::move(music), m.cpu_is_64bit());
        if (created == 0) { m.set_return(E_OUTOFMEMORY); return; }
        m_objects.at(m.arg_ptr(0)).du_music = created;
    }
    const GuestAddr music = m_objects.at(m.arg_ptr(0)).du_music;
    auto child = m_objects.find(music);
    if (child != m_objects.end()) ++child->second.ref_count;
    m.set_return(m.write_ptr(out, music).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_DuGetVideoProperties(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    m.set_return(E_NOTIMPL);
}

void CombaseManager::impl_DuGetImageProperties(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    m.set_return(E_NOTIMPL);
}

void CombaseManager::impl_DuCopyFromFileAsync(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(3);
    if (out == 0) { m.set_return(E_POINTER); return; }
    (void)m.write_ptr(out, 0);
    // Reading metadata out of a StorageFile needs that runtime class.
    m.set_return(E_NOTIMPL);
}

void CombaseManager::impl_DuClearAll(Marshaler& m) {
    auto it = m_objects.find(m.arg_ptr(0));
    if (it == m_objects.end() || it->second.type != ObjectType::DisplayUpdater) {
        m.set_return(E_INVALIDARG);
        return;
    }
    it->second.du_type = 0;
    it->second.du_app_media_id.clear();
    auto music = m_objects.find(it->second.du_music);
    if (music != m_objects.end()) {
        for (auto& field : music->second.music_fields) field.clear();
    }
    if (m_now_playing_host != nullptr) {
        VITREO_TRACE("[combase] DisplayUpdater.ClearAll -> host now playing "
                     "cleared\n");
        (void)m_now_playing_host->clear();
    }
    m.set_return(S_OK);
}

void CombaseManager::impl_DuUpdate(Marshaler& m) {
    auto it = m_objects.find(m.arg_ptr(0));
    if (it == m_objects.end() || it->second.type != ObjectType::DisplayUpdater) {
        m.set_return(E_INVALIDARG);
        return;
    }
    publish_now_playing(it->second);
    m.set_return(S_OK);
}

u32 CombaseManager::music_get_string(Marshaler& m, u32 field) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return E_POINTER;
    auto it = m_objects.find(m.arg_ptr(0));
    if (it == m_objects.end() ||
        it->second.type != ObjectType::MusicDisplayProperties || field > 2) {
        return E_INVALIDARG;
    }
    return m.write_ptr(out, create_hstring(it->second.music_fields[field],
                                           m.cpu_is_64bit()))
                   .is_ok()
               ? S_OK
               : E_POINTER;
}

u32 CombaseManager::music_put_string(Marshaler& m, u32 field) {
    auto it = m_objects.find(m.arg_ptr(0));
    if (it == m_objects.end() ||
        it->second.type != ObjectType::MusicDisplayProperties || field > 2) {
        return E_INVALIDARG;
    }
    std::u16string value;
    if (const GuestAddr text = m.arg_ptr(1); text != 0) {
        auto read = read_hstring(text, m.cpu_is_64bit());
        if (read.has_value()) value = std::move(*read);
    }
    it->second.music_fields[field] = std::move(value);
    return S_OK;
}

} // namespace vitreo::winapi
