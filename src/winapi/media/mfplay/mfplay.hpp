// SPDX-License-Identifier: LGPL-2.1-only

/// @file mfplay.hpp
/// @brief mfplay.dll: the Media Foundation simple player (MFPCreateMediaPlayer,
/// IMFPMediaPlayer, IMFPMediaItem, IMFPMediaPlayerCallback events).
///
/// Contract sources:
///  - Exports: SDK 10.0.26100 Lib/um/{x86,x64}/mfplay.lib (MFPCreateMediaPlayer,
///    x86 @24) and the real DLL's export table (DllCanUnloadNow,
///    DllGetClassObject, DllRegisterServer, DllUnregisterServer,
///    MFPCreateMediaPlayer, MFPCreateMediaPlayerEx; the WOW64 build pops 0x1c
///    for the undocumented Ex).
///  - Interfaces, vtable order, IIDs, enums and event structs: SDK mfplay.h.
///    HRESULTs: mferror.h / winerror.h. Attribute GUIDs: mfapi.h / mfidl.h.
///  - Behaviour: Microsoft Learn (mfplay.h reference and the MFPlay guide).
///  - Everything Learn leaves open: tools/oracle_probe/mfplay_probe.c recorded on
///    Windows (tests/oracle_data/mfplay_probe{32,64}.expected, x86 == x64).
///    The oracle has no audio endpoint, so every SetMediaItem there fails
///    (MF_E_CANNOT_CREATE_SINK, or MF_E_TOPO_UNSUPPORTED with nothing selected);
///    the probe pins the parameter, no-item, shutdown and event-delivery rules,
///    and the success path follows Learn.
///
/// Measured rules worth knowing before reading the code:
///  - Calls that Learn calls asynchronous return S_OK and report the operation's
///    result in the event's hrEvent, even with no media item (Play/Pause/
///    FrameStep/SetPosition: MF_E_INVALIDREQUEST in the event; Stop and
///    ClearMediaItem: S_OK). SetRate with no item: RATE_SET S_OK/1.0 for 1.0,
///    MF_E_UNSUPPORTED_RATE/0.0 otherwise.
///  - Events are never delivered inside the call. Without
///    MFP_OPTION_FREE_THREADED_CALLBACK they arrive on the creating thread from
///    its message loop (the player owns one hidden window on that thread, also
///    created without a callback); with it they arrive on another thread and
///    no window exists.
///  - MFPCreateMediaPlayer with a URL opens it synchronously (a missing file
///    fails the call); fStartPlayback TRUE then raises MEDIAITEM_SET and PLAY,
///    fStartPlayback FALSE raises nothing and sets nothing.
///  - After Shutdown: GetState (SHUTDOWN), GetVideoWindow, volume, balance,
///    mute, RemoveAllEffects and the item's Get/SetUserData still work; the
///    rest returns MF_E_SHUTDOWN.
///
/// Playback runs on Vitreo's host media backend (audio::IMediaPlayer) and video
/// is presented into the hWnd's client area through the injected presenter.
/// What that backend cannot do fails with the documented HRESULT and a one-time
/// `[vitreo] UNIMPLEMENTED mfplay.dll!<api>: <case>` line:
///  - playback rates other than 1.0 (GetSupportedRates reports [1, 1] forward,
///    reverse MF_E_UNSUPPORTED_RATE; SetRate clamps to 1.0 as Learn documents);
///  - FrameStep on a video item (FRAME_STEP with MF_E_UNSUPPORTED_RATE);
///  - GetIdealVideoSize (E_NOTIMPL);
///  - CreateMediaItemFromObject (E_NOTIMPL): no IMFMediaSource/IMFByteStream
///    exists to read from;
///  - IMFPMediaItem::SetStreamSink with a sink, GetMetadata (E_NOTIMPL);
///  - InsertEffect is recorded, but a required effect makes the next
///    SetMediaItem fail (MF_E_TOPO_UNSUPPORTED) and an optional one is skipped,
///    as Learn documents for an effect that cannot be added;
///  - network URLs (MF_E_UNSUPPORTED_SCHEME);
///  - QueryInterface(IPropertyStore) on the player (E_NOINTERFACE);
///  - MFPCreateMediaPlayerEx (undocumented; E_NOTIMPL).

#pragma once

#include <vitreo/common.hpp>
#include "audio/contracts/media_player.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"

#include <array>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {

namespace mfplay {

// SDK mfplay.h
inline constexpr u32 MFP_OPTION_FREE_THREADED_CALLBACK = 0x1;

enum class State : u32 { Empty = 0, Stopped = 1, Playing = 2, Paused = 3, Shutdown = 4 };

enum class EventType : u32 {
    Play = 0, Pause = 1, Stop = 2, PositionSet = 3, RateSet = 4, MediaItemCreated = 5,
    MediaItemSet = 6, FrameStep = 7, MediaItemCleared = 8, Mf = 9, Error = 10,
    PlaybackEnded = 11, AcquireUserCredential = 12,
};

// _MFP_MEDIAITEM_CHARACTERISTICS
inline constexpr u32 MFP_MEDIAITEM_CAN_SEEK = 0x2;
inline constexpr u32 MFP_MEDIAITEM_CAN_PAUSE = 0x4;

// evr.h MFVideoAspectRatioMode
inline constexpr u32 MFVideoARMode_PreservePicture = 0x1;
inline constexpr u32 MFVideoARMode_Mask = 0x7;

/// IMFPMediaPlayer vtable (SDK mfplay.h order).
enum PlayerSlot : u32 {
    P_QueryInterface = 0, P_AddRef, P_Release, P_Play, P_Pause, P_Stop, P_FrameStep,
    P_SetPosition, P_GetPosition, P_GetDuration, P_SetRate, P_GetRate, P_GetSupportedRates,
    P_GetState, P_CreateMediaItemFromURL, P_CreateMediaItemFromObject, P_SetMediaItem,
    P_ClearMediaItem, P_GetMediaItem, P_GetVolume, P_SetVolume, P_GetBalance, P_SetBalance,
    P_GetMute, P_SetMute, P_GetNativeVideoSize, P_GetIdealVideoSize, P_SetVideoSourceRect,
    P_GetVideoSourceRect, P_SetAspectRatioMode, P_GetAspectRatioMode, P_GetVideoWindow,
    P_UpdateVideo, P_SetBorderColor, P_GetBorderColor, P_InsertEffect, P_RemoveEffect,
    P_RemoveAllEffects, P_Shutdown,
    PLAYER_SLOT_COUNT
};

/// IMFPMediaItem vtable (SDK mfplay.h order).
enum ItemSlot : u32 {
    I_QueryInterface = 0, I_AddRef, I_Release, I_GetMediaPlayer, I_GetURL, I_GetObject,
    I_GetUserData, I_SetUserData, I_GetStartStopPosition, I_SetStartStopPosition,
    I_HasVideo, I_HasAudio, I_IsProtected, I_GetDuration, I_GetNumberOfStreams,
    I_GetStreamSelection, I_SetStreamSelection, I_GetStreamAttribute,
    I_GetPresentationAttribute, I_GetCharacteristics, I_SetStreamSink, I_GetMetadata,
    ITEM_SLOT_COUNT
};

/// IMFPMediaPlayerCallback::OnMediaPlayerEvent
inline constexpr u32 CALLBACK_SLOT_ON_EVENT = 3;

/// Private message the hidden event window receives (WM_APP range).
inline constexpr u32 EVENT_WINDOW_MESSAGE = 0xB0A1;
inline constexpr u32 DESTROY_WINDOW_MESSAGE = 0xB0A2;

} // namespace mfplay

/// Media Foundation simple player objects and their event delivery.
class MfPlayManager {
public:
    /// Host services. Unset hooks make the dependent operation fail with its
    /// documented HRESULT rather than pretend.
    struct Host {
        /// Run a guest function on the calling guest thread.
        std::function<u64(GuestAddr function, const u64* args, usize count)> call_guest;
        /// Run a guest function on Vitreo's MFPlay service thread (called from
        /// the manager's own delivery thread; blocks until it returns).
        std::function<u64(GuestAddr function, const u64* args, usize count)> call_guest_service;
        /// Create the hidden window, on the calling guest thread, whose window
        /// procedure is @p wndproc. Returns 0 on failure.
        std::function<u64(GuestAddr wndproc)> create_event_window;
        /// DestroyWindow on the calling guest thread.
        std::function<void(u64 hwnd)> destroy_window;
        /// DefWindowProcW on the calling guest thread.
        std::function<u64(u64 hwnd, u32 msg, u64 wparam, u64 lparam)> def_window_proc;
        /// PostMessageW from any host thread.
        std::function<bool(u64 hwnd, u32 msg, u64 wparam, u64 lparam)> post_message;
        std::function<u32()> current_thread_id;
        /// Windows path (UTF-8) -> host path, or nullopt when unmapped.
        std::function<std::optional<std::string>(const std::string& windows_path)> resolve_path;
        std::function<std::unique_ptr<audio::IMediaPlayer>()> create_media;
        std::function<void(u64 hwnd, const u8* rgba, i32 width, i32 height)> present_video;
        std::function<void(u64 hwnd)> hide_video;
        /// Client size of @p hwnd; false when unknown.
        std::function<bool(u64 hwnd, i32& width, i32& height)> client_size;
        /// Guest memory the manager keeps for its objects (never freed).
        std::function<GuestAddr(u64 size)> alloc_guest;
        /// CoTaskMemAlloc (GetURL, PROPVARIANT payloads the caller frees).
        std::function<GuestAddr(u64 size)> task_alloc;
    };

    MfPlayManager();
    ~MfPlayManager();
    MfPlayManager(const MfPlayManager&) = delete;
    MfPlayManager& operator=(const MfPlayManager&) = delete;

    void set_thunk_registry(ThunkRegistry* thunks) noexcept { m_thunks = thunks; }
    void set_memory(GuestMemory* memory) noexcept { m_memory = memory; }
    void set_host(Host host);
    /// The manager's own clock thread drives tick(); tests drive it by hand.
    void set_clock_enabled(bool enabled) noexcept { m_clock_enabled = enabled; }

    /// Export names of the per-slot vtable thunks and the event window procedure.
    [[nodiscard]] static std::string player_thunk_name(u32 slot);
    [[nodiscard]] static std::string item_thunk_name(u32 slot);
    [[nodiscard]] static const char* wndproc_thunk_name() { return "Vitreo_MFPlay_EventWndProc"; }
    /// x86 stdcall bytes, from the SDK prototypes.
    [[nodiscard]] static u16 player_slot_stdcall_bytes(u32 slot);
    [[nodiscard]] static u16 item_slot_stdcall_bytes(u32 slot);

    // ---- exports ----------------------------------------------------------
    /// MFPCreateMediaPlayer(pwszURL, fStartPlayback, creationOptions,
    /// pCallback, hWnd, ppMediaPlayer).
    void create_media_player(Marshaler& m);
    [[nodiscard]] u32 dispatch_player(u32 slot, Marshaler& m);
    [[nodiscard]] u32 dispatch_item(u32 slot, Marshaler& m);
    /// The hidden window's procedure (HWND, UINT, WPARAM, LPARAM).
    [[nodiscard]] u64 event_window_proc(Marshaler& m);

    /// Players and items alive (tests).
    [[nodiscard]] usize live_player_count() const;
    [[nodiscard]] usize live_item_count() const;
    /// Advance playback bookkeeping once (end of media, video frames). The
    /// manager's clock thread calls this; tests call it directly.
    void tick();
    /// Join pending asynchronous item creations and wait until every queued
    /// free-threaded event has been delivered (tests).
    void drain_service_queue();

private:
    struct Player;
    struct Item;
    struct Event;
    using PlayerPtr = std::shared_ptr<Player>;
    using ItemPtr = std::shared_ptr<Item>;

    // objects.cpp
    [[nodiscard]] bool ensure_vtables(bool is64);
    [[nodiscard]] GuestAddr alloc_block(u32 size);
    void free_block(GuestAddr addr, u32 size);
    [[nodiscard]] PlayerPtr find_player(GuestAddr self);
    [[nodiscard]] ItemPtr find_item(GuestAddr self);
    [[nodiscard]] PlayerPtr find_player_by_window(u64 hwnd);
    [[nodiscard]] ItemPtr new_item(const PlayerPtr& owner, u64 user_data);
    void destroy_item(const ItemPtr& item);
    [[nodiscard]] u32 open_url(Item& item, const std::string& url);
    /// Shut @p p down; guest Releases run on the service thread when
    /// @p service_thread, else on the calling guest thread.
    void shutdown_player(const PlayerPtr& p, bool service_thread);
    void release_player_cell(const PlayerPtr& p);
    void item_add_ref(const ItemPtr& item);
    void item_release(const ItemPtr& item);
    void ensure_clock();

    // events
    void post_event(const PlayerPtr& p, Event ev);
    void deliver(const PlayerPtr& p, const Event& ev, bool service_thread);
    void drain_window_queue(const PlayerPtr& p);
    void service_loop();

    // player.cpp
    [[nodiscard]] u32 player_unknown(u32 slot, Marshaler& m, const PlayerPtr& p);
    [[nodiscard]] u32 player_transport(u32 slot, Marshaler& m, const PlayerPtr& p);
    [[nodiscard]] u32 player_items(u32 slot, Marshaler& m, const PlayerPtr& p);
    [[nodiscard]] u32 player_audio(u32 slot, Marshaler& m, const PlayerPtr& p);
    [[nodiscard]] u32 player_video(u32 slot, Marshaler& m, const PlayerPtr& p);
    [[nodiscard]] u32 player_effects(u32 slot, Marshaler& m, const PlayerPtr& p);
    [[nodiscard]] u32 create_item_from_url(Marshaler& m, const PlayerPtr& p, GuestAddr url,
                                           bool sync, u64 user_data, GuestAddr out);
    [[nodiscard]] u32 set_media_item(Marshaler& m, const PlayerPtr& p, const ItemPtr& item);
    void play(const PlayerPtr& p);
    void apply_audio(Player& p);
    void present_frame(Player& p, bool force);

    // item.cpp
    [[nodiscard]] u32 item_methods(u32 slot, Marshaler& m, const ItemPtr& it);

    ThunkRegistry* m_thunks{nullptr};
    GuestMemory* m_memory{nullptr};
    Host m_host;

    mutable std::mutex m_mutex;
    std::unordered_map<GuestAddr, PlayerPtr> m_players;
    std::unordered_map<GuestAddr, ItemPtr> m_items;
    GuestAddr m_player_vtable{0};
    GuestAddr m_item_vtable{0};
    bool m_is64{false};
    GuestAddr m_chunk{0};
    u32 m_chunk_used{0};
    std::unordered_map<u32, std::vector<GuestAddr>> m_free_blocks;

    // Free-threaded delivery.
    std::mutex m_service_mutex;
    std::condition_variable m_service_wake;
    std::condition_variable m_service_idle;
    std::deque<std::pair<PlayerPtr, std::shared_ptr<Event>>> m_service_queue;
    bool m_service_busy{false};
    bool m_stopping{false};
    std::thread m_service_thread;

    // Playback clock (end of media, video frames) and async item creation.
    std::thread m_clock_thread;
    bool m_clock_enabled{true};
    std::vector<std::thread> m_workers;
};

/// mfplay.dll exports: MFPCreateMediaPlayer, the COM-server entry points, the
/// undocumented MFPCreateMediaPlayerEx, and the object thunks.
DllExports create_mfplay_thunks(MfPlayManager& mgr);

} // namespace vitreo::winapi
