#pragma once
/// @file now_playing.hpp
/// @brief Host "now playing" contract.
///
/// Windows.Media.SystemMediaTransportControls is how a Windows app tells the
/// OS what it is playing, so the shell can show the track and offer transport
/// buttons. macOS has the same concept in MPNowPlayingInfoCenter, so a guest's
/// metadata belongs there rather than being accumulated and dropped.
///
/// The interface keeps the WinRT layer free of any platform header; the macOS
/// backend lives behind make_platform_now_playing_host().

#include <vitreo/common.hpp>

#include <memory>
#include <string>

namespace vitreo::hardware {

/// Mirrors Windows.Media.MediaPlaybackStatus, which is what the guest sets
/// (windows.media.h: Closed 0, Changing 1, Stopped 2, Playing 3, Paused 4).
enum class NowPlayingStatus : u32 {
    Closed = 0,
    Changing = 1,
    Stopped = 2,
    Playing = 3,
    Paused = 4,
};

struct NowPlayingInfo {
    std::string title;
    std::string artist;
    std::string album_artist;
    /// SystemMediaTransportControlsDisplayUpdater.AppMediaId — the app's own
    /// identifier for the item; carried for hosts that can use it.
    std::string app_media_id;
    NowPlayingStatus status = NowPlayingStatus::Closed;
    /// The timeline ISystemMediaTransportControls2::UpdateTimelineProperties
    /// last set, relative to its StartTime; has_timeline is false until then.
    bool has_timeline = false;
    double elapsed_seconds = 0;
    double duration_seconds = 0;
    /// The rate the host advances elapsed time at; 0 while not playing.
    double rate = 0;
};

class NowPlayingHost {
public:
    virtual ~NowPlayingHost() = default;

    /// Publishes @p info as the process's current item. Returns false when the
    /// host has no now-playing surface at all.
    [[nodiscard]] virtual bool update(const NowPlayingInfo& info) = 0;

    /// Withdraws the process's item, as ClearAll() does on Windows.
    [[nodiscard]] virtual bool clear() = 0;
};

/// Never null: a platform with no implementation returns a host that reports
/// no now-playing surface.
[[nodiscard]] std::unique_ptr<NowPlayingHost> make_platform_now_playing_host();

} // namespace vitreo::hardware
