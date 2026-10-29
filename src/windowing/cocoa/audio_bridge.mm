/// @file audio_bridge.mm
/// @brief macOS audio playback implementation using AVFoundation

#include "windowing/contracts/audio_bridge.hpp"

#include <memory>
#include <cstdio>

#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>

namespace vitreo::windowing {

class MacOSAudioBridge final : public AudioBridge {
public:
    MacOSAudioBridge() = default;
    ~MacOSAudioBridge() override {
        stop();
    }

    void play_wav(const u8* data, u32 size, bool async) override {
        @autoreleasepool {
            NSData* ns_data = [NSData dataWithBytes:data length:size];

            NSError* error = nil;
            AVAudioPlayer* player = [[AVAudioPlayer alloc] initWithData:ns_data error:&error];
            if (!player || error) {
                VITREO_TRACE( "[audio] AVAudioPlayer init failed: %s\n",
                             error ? [[error localizedDescription] UTF8String] : "nil");
                return;
            }

            // Stop any previous playback
            if (m_current_player && [m_current_player isPlaying]) {
                [m_current_player stop];
            }
            m_current_player = player;

            [player play];

            if (!async) {
                // Block until playback completes (SND_SYNC)
                while ([player isPlaying]) {
                    [[NSRunLoop currentRunLoop] runUntilDate:
                        [NSDate dateWithTimeIntervalSinceNow:0.01]];
                }
            }
        }
    }

    void stop() override {
        @autoreleasepool {
            if (m_current_player && [m_current_player isPlaying]) {
                [m_current_player stop];
            }
            m_current_player = nil;
        }
    }

private:
    AVAudioPlayer* m_current_player = nil;
};

std::unique_ptr<AudioBridge> create_audio_bridge() {
    return std::make_unique<MacOSAudioBridge>();
}

} // namespace vitreo::windowing
