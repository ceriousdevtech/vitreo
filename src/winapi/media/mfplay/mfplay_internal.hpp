// SPDX-License-Identifier: LGPL-2.1-only

/// @file mfplay_internal.hpp
/// @brief Host state behind the IMFPMediaPlayer / IMFPMediaItem objects, the
/// HRESULTs and GUIDs they use, and guest-memory helpers shared by the
/// mfplay translation units.

#pragma once

#include "winapi/media/mfplay/mfplay.hpp"

#include "core/memory/guest_memory.hpp"

#include <atomic>
#include <cstdio>
#include <string_view>

namespace vitreo::winapi {

namespace mfplay {

// winerror.h / mferror.h
inline constexpr u32 S_OK = 0;
inline constexpr u32 S_FALSE = 1;
inline constexpr u32 E_NOTIMPL = 0x80004001u;
inline constexpr u32 E_NOINTERFACE = 0x80004002u;
inline constexpr u32 E_POINTER = 0x80004003u;
inline constexpr u32 E_FAIL = 0x80004005u;
inline constexpr u32 E_OUTOFMEMORY = 0x8007000Eu;
inline constexpr u32 E_INVALIDARG = 0x80070057u;
inline constexpr u32 HRESULT_FILE_NOT_FOUND = 0x80070002u;   // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
inline constexpr u32 HRESULT_PATH_NOT_FOUND = 0x80070003u;   // HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)
inline constexpr u32 HRESULT_INVALID_WINDOW_HANDLE = 0x80070578u; // ERROR_INVALID_WINDOW_HANDLE
inline constexpr u32 CLASS_E_CLASSNOTAVAILABLE = 0x80040111u;
inline constexpr u32 SELFREG_E_CLASS = 0x80040201u;
inline constexpr u32 MF_E_INVALIDREQUEST = 0xC00D36B2u;
inline constexpr u32 MF_E_UNSUPPORTED_SCHEME = 0xC00D36C3u;
inline constexpr u32 MF_E_UNSUPPORTED_BYTESTREAM_TYPE = 0xC00D36C4u;
inline constexpr u32 MF_E_INVALIDINDEX = 0xC00D36BFu;
inline constexpr u32 MF_E_UNSUPPORTED_RATE = 0xC00D36D0u;
inline constexpr u32 MF_E_NOT_FOUND = 0xC00D36D5u;
inline constexpr u32 MF_E_INVALID_POSITION = 0xC00D36E5u;
inline constexpr u32 MF_E_ATTRIBUTENOTFOUND = 0xC00D36E6u;
inline constexpr u32 MF_E_OUT_OF_RANGE = 0xC00D3702u;
inline constexpr u32 MF_E_SHUTDOWN = 0xC00D3E85u;
inline constexpr u32 MF_E_TOPO_UNSUPPORTED = 0xC00D5214u;

// propidl.h VARTYPE
inline constexpr u16 VT_EMPTY = 0;
inline constexpr u16 VT_I8 = 20;
inline constexpr u16 VT_UI8 = 21;
inline constexpr u16 VT_CLSID = 72;

struct Guid {
    u32 data1{0};
    u16 data2{0};
    u16 data3{0};
    std::array<u8, 8> data4{};
    [[nodiscard]] bool operator==(const Guid& o) const noexcept {
        return data1 == o.data1 && data2 == o.data2 && data3 == o.data3 && data4 == o.data4;
    }
};
static_assert(sizeof(Guid) == 16);

constexpr Guid guid(u32 a, u16 b, u16 c, u8 d0, u8 d1, u8 d2, u8 d3, u8 d4, u8 d5,
                    u8 d6, u8 d7) {
    return Guid{a, b, c, {d0, d1, d2, d3, d4, d5, d6, d7}};
}

// unknwn.h / mfplay.h
inline constexpr Guid IID_IUnknown = guid(0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);
inline constexpr Guid IID_IMFPMediaPlayer =
    guid(0xA714590A, 0x58AF, 0x430a, 0x85, 0xBF, 0x44, 0xF5, 0xEC, 0x83, 0x8D, 0x85);
inline constexpr Guid IID_IMFPMediaItem =
    guid(0x90EB3E6B, 0xECBF, 0x45cc, 0xB1, 0xDA, 0xC6, 0xFE, 0x3E, 0xA7, 0x0D, 0x57);
// propsys.h
inline constexpr Guid IID_IPropertyStore =
    guid(0x886d8eeb, 0x8cf2, 0x4446, 0x8d, 0x02, 0xcd, 0xba, 0x1d, 0xbd, 0xcf, 0x99);
// mfplay.h: EXTERN_GUID(MFP_POSITIONTYPE_100NS, 0, ...) -- the all-zero GUID.
inline constexpr Guid MFP_POSITIONTYPE_100NS{};
// mfidl.h
inline constexpr Guid MF_PD_DURATION =
    guid(0x6c990d33, 0xbb8e, 0x477a, 0x85, 0x98, 0x0d, 0x5d, 0x96, 0xfc, 0xd8, 0x8a);
inline constexpr Guid MF_PD_TOTAL_FILE_SIZE =
    guid(0x6c990d34, 0xbb8e, 0x477a, 0x85, 0x98, 0x0d, 0x5d, 0x96, 0xfc, 0xd8, 0x8a);
// mfapi.h
inline constexpr Guid MF_MT_MAJOR_TYPE =
    guid(0x48eba18e, 0xf8c9, 0x4687, 0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f);
inline constexpr Guid MF_MT_FRAME_SIZE =
    guid(0x1652c33d, 0xd6b2, 0x4012, 0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d);
inline constexpr Guid MFMediaType_Audio =
    guid(0x73647561, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
inline constexpr Guid MFMediaType_Video =
    guid(0x73646976, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);

/// One `[vitreo] UNIMPLEMENTED mfplay.dll!<api>: <case>` line per site.
#define MFPLAY_UNIMPLEMENTED(api, why)                                                   \
    do {                                                                                  \
        static std::atomic<bool> fired_{false};                                           \
        if (!fired_.exchange(true)) {                                                     \
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED mfplay.dll!%s: %s\n", api, why); \
        }                                                                                 \
    } while (0)

/// PROPVARIANT: vt at +0, the union at +8; 24 bytes on x64, 16 on x86.
inline u32 propvariant_size(bool is64) { return is64 ? 24u : 16u; }

} // namespace mfplay

struct MfPlayManager::Event {
    mfplay::EventType type{mfplay::EventType::Play};
    u32 hr{0};
    mfplay::State state{mfplay::State::Empty};
    ItemPtr item;     ///< pMediaItem (null when the event has none)
    u64 extra{0};     ///< dwUserData (MEDIAITEM_CREATED) or float bits (RATE_SET)
    /// The event holds one reference on @ref item, dropped once delivered (or
    /// dropped): MEDIAITEM_CREATED's new item, MEDIAITEM_CLEARED's old one.
    bool owns_item_ref{false};
};

struct MfPlayManager::Item {
    GuestAddr cell{0};
    u32 refs{1};
    std::weak_ptr<Player> owner;
    std::string url_utf8;
    std::u16string url;
    u64 user_data{0};
    std::unique_ptr<audio::IMediaPlayer> media;
    bool has_audio{false};
    bool has_video{false};
    u32 video_width{0};
    u32 video_height{0};
    i64 duration{0};
    u64 file_size{0};
    /// Streams in index order: true = video. Video comes first.
    std::vector<bool> stream_is_video;
    std::vector<bool> selected;
    std::optional<i64> start;
    std::optional<i64> stop;
};

struct MfPlayManager::Player {
    GuestAddr cell{0};
    GuestAddr event_buffer{0};
    u32 refs{1};
    /// MFPCreateMediaPlayer with ppMediaPlayer NULL: alive until playback ends.
    bool fire_and_forget{false};
    bool shut_down{false};
    bool free_threaded{false};
    u32 options{0};
    GuestAddr callback{0};
    u64 video_hwnd{0};
    u64 event_hwnd{0};
    u32 creator_thread{0};

    std::recursive_mutex mutex;
    ItemPtr current;
    /// GetMediaItem shows the item once MEDIAITEM_SET is delivered (Learn).
    bool current_visible{false};
    mfplay::State state{mfplay::State::Empty};
    /// What GetState reports: the state of the last delivered event (Learn:
    /// GetState does not reflect a pending operation).
    mfplay::State reported_state{mfplay::State::Empty};
    float rate{1.0f};
    float volume{1.0f};
    float balance{0.0f};
    u32 mute{0};
    std::array<float, 4> source_rect{0.0f, 0.0f, 1.0f, 1.0f};
    u32 aspect_mode{mfplay::MFVideoARMode_PreservePicture};
    u32 border_color{0};
    struct Effect { GuestAddr punk; bool optional; };
    std::vector<Effect> effects;
    u64 last_frame{~u64{0}};
    bool video_shown{false};

    std::deque<Event> window_queue;
    bool in_callback{false};
};

} // namespace vitreo::winapi
