#pragma once
/// @file audio_bridge.hpp
/// @brief Platform-agnostic audio playback interface

#include <vitreo/common.hpp>
#include <memory>

namespace vitreo::windowing {

/// Abstract audio playback interface.
/// Platform-specific implementations (macOS, Linux, etc.) provide concrete behavior.
class AudioBridge {
public:
    virtual ~AudioBridge() = default;

    /// Play WAV audio data.
    /// @param data Pointer to WAV file data (including RIFF header)
    /// @param size Size of the WAV data in bytes
    /// @param async If true, play asynchronously and return immediately
    virtual void play_wav(const u8* data, u32 size, bool async) = 0;

    /// Stop any currently playing audio.
    virtual void stop() = 0;
};

/// Create the platform-specific audio bridge implementation.
/// @return Owning pointer to the audio bridge
std::unique_ptr<AudioBridge> create_audio_bridge();

} // namespace vitreo::windowing
