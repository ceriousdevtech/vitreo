/// @file now_playing_mpnowplaying.mm
/// @brief macOS MPNowPlayingInfoCenter backend for the now-playing contract.
///
/// MPNowPlayingInfoCenter is the same idea as the Windows system media
/// transport controls: the process publishes what it is playing and the shell
/// displays it. It is not privacy-gated, so unlike the Bluetooth bridge there
/// is no TCC hazard here — but it only surfaces for a process that is a real
/// app bundle, which a relaunched Vitreo guest is.

#include "hardware/contracts/now_playing.hpp"

#import <Foundation/Foundation.h>
#import <MediaPlayer/MediaPlayer.h>

namespace vitreo::hardware {
namespace {

NSString* to_ns(const std::string& text) {
    if (text.empty()) return nil;
    return [NSString stringWithUTF8String:text.c_str()];
}

MPNowPlayingPlaybackState to_playback_state(NowPlayingStatus status) {
    switch (status) {
    case NowPlayingStatus::Playing: return MPNowPlayingPlaybackStatePlaying;
    case NowPlayingStatus::Paused: return MPNowPlayingPlaybackStatePaused;
    case NowPlayingStatus::Stopped: return MPNowPlayingPlaybackStateStopped;
    case NowPlayingStatus::Changing:
        // Windows uses Changing while a track swap is in flight; the item is
        // still the app's, so keep it playing rather than tearing it down.
        return MPNowPlayingPlaybackStatePlaying;
    case NowPlayingStatus::Closed:
    default:
        return MPNowPlayingPlaybackStateStopped;
    }
}

class MPNowPlayingHost final : public NowPlayingHost {
public:
    [[nodiscard]] bool update(const NowPlayingInfo& info) override {
        @autoreleasepool {
            MPNowPlayingInfoCenter* center =
                [MPNowPlayingInfoCenter defaultCenter];
            if (center == nil) return false;

            NSMutableDictionary* entry = [NSMutableDictionary dictionary];
            if (NSString* title = to_ns(info.title)) {
                entry[MPMediaItemPropertyTitle] = title;
            }
            if (NSString* artist = to_ns(info.artist)) {
                entry[MPMediaItemPropertyArtist] = artist;
            }
            if (NSString* album = to_ns(info.album_artist)) {
                // The Windows field is the album ARTIST; macOS shows the album
                // artist under its own key and the album title separately, so
                // publish it as the album artist only.
                entry[MPMediaItemPropertyAlbumArtist] = album;
            }
            if (info.has_timeline) {
                entry[MPMediaItemPropertyPlaybackDuration] = @(info.duration_seconds);
                entry[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(info.elapsed_seconds);
                entry[MPNowPlayingInfoPropertyPlaybackRate] = @(info.rate);
            }
            center.nowPlayingInfo = entry;
            center.playbackState = to_playback_state(info.status);
            return true;
        }
    }

    [[nodiscard]] bool clear() override {
        @autoreleasepool {
            MPNowPlayingInfoCenter* center =
                [MPNowPlayingInfoCenter defaultCenter];
            if (center == nil) return false;
            center.nowPlayingInfo = nil;
            center.playbackState = MPNowPlayingPlaybackStateStopped;
            return true;
        }
    }
};

} // namespace

std::unique_ptr<NowPlayingHost> make_platform_now_playing_host() {
    return std::make_unique<MPNowPlayingHost>();
}

} // namespace vitreo::hardware
