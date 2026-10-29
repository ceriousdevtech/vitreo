#pragma once
/// @file gdiplus_codec_ref.hpp
/// @brief The codec area's per-image state handle, embedded in GpImage.
///
/// A decoded image carries more than pixels: its encoded source, the frame
/// dimension and active frame, the property items, an open multi-frame save
/// session, and (for a metafile) the record stream. That state lives in
/// gdip::CodecState (gdiplus_codec.hpp). GdipCloneImage copies a GpImage, and
/// a clone is independent on Windows (editing a property of one does not
/// change the other), so copying a CodecRef deep-copies the state; the save
/// session is never copied (it belongs to the image SaveImageTo* opened it on).

#include <memory>

namespace vitreo::winapi::gdip {

struct CodecState;

class CodecRef {
public:
    CodecRef() = default;
    CodecRef(const CodecRef& other);
    CodecRef& operator=(const CodecRef& other);
    CodecRef(CodecRef&&) noexcept = default;
    CodecRef& operator=(CodecRef&&) noexcept = default;
    ~CodecRef();

    /// Null for an image the codec area never touched (a plain Scan0 bitmap).
    [[nodiscard]] CodecState* get() const noexcept { return m_state.get(); }
    /// The state, created empty on first use.
    [[nodiscard]] CodecState& ensure();
    void reset(std::shared_ptr<CodecState> state) noexcept { m_state = std::move(state); }

private:
    std::shared_ptr<CodecState> m_state;
};

}  // namespace vitreo::winapi::gdip
