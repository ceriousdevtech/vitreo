#pragma once
/// @file media_player.hpp
/// @brief Frontend-neutral decoded-media playback contract.

#include <vitreo/common.hpp>

#include <memory>
#include <string_view>
#include <vector>

namespace vitreo::audio {

enum class MediaStartResult : u8 {
    Started,
    Pending,
    Failed,
};

/// Host media pipeline used by Windows multimedia frontends such as
/// DirectShow. Paths passed here have already crossed the capsule boundary.
class IMediaPlayer {
public:
    virtual ~IMediaPlayer() = default;

    /// Build a decoder for a host-resolved media file. Decoding may continue
    /// asynchronously after this returns, but false means no usable graph was
    /// constructed and the Windows frontend must report failure.
    [[nodiscard]] virtual bool open(std::string_view host_path) = 0;

    [[nodiscard]] virtual MediaStartResult run() = 0;
    virtual void pause() = 0;
    virtual void stop() = 0;

    [[nodiscard]] virtual bool ready() const = 0;
    [[nodiscard]] virtual bool failed() const = 0;
    [[nodiscard]] virtual bool take_completed() = 0;

    virtual void set_volume(i32 hundredth_db) = 0;
    virtual void set_balance(i32 hundredth_db) = 0;
    /// Flush and position the decoded streams; false reports a failed seek.
    [[nodiscard]] virtual bool seek_100ns(i64 position) = 0;
    [[nodiscard]] virtual i64 position_100ns() const = 0;
    [[nodiscard]] virtual i64 duration_100ns() const = 0;

    /// True after open() when the file has a video stream the decoder can
    /// present. Audio-only graphs stay false.
    [[nodiscard]] virtual bool has_video() const { return false; }
    /// True after open() when the file has an audio stream the player renders
    /// itself (the part DirectShow's RenderFile gives an audio renderer).
    [[nodiscard]] virtual bool has_audio() const { return false; }
    virtual void native_video_size(u32& width, u32& height) const {
        width = 0;
        height = 0;
    }

    /// Copy the frame that should be visible at the current graph clock.
    /// Before Run this is the preroll frame (Pause shows the first picture).
    /// Returns false when no decoded picture is available yet.
    [[nodiscard]] virtual bool copy_video_frame(std::vector<u8>& rgba,
                                                u32& width, u32& height,
                                                u64& frame_index) const {
        (void)rgba;
        (void)width;
        (void)height;
        (void)frame_index;
        return false;
    }
};

/// Creates the best available host media decoder. Returns null when Vitreo
/// was built without a decoder backend.
[[nodiscard]] std::unique_ptr<IMediaPlayer> create_media_player();

} // namespace vitreo::audio
