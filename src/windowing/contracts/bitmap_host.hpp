#pragma once

#include <vitreo/common.hpp>

#include <vector>

namespace vitreo::windowing {

/// Host bitmap registration used by USER32 resource loading.
class BitmapHost {
public:
    virtual ~BitmapHost() = default;

    [[nodiscard]] virtual u32 register_bitmap_bmp(
        const std::vector<u8>& bmp_file) = 0;

    /// An OEM bitmap (LoadBitmap(NULL, OBM_*)) drawn by the host with its own
    /// controls at the shape Windows defines for that id (bit depth included);
    /// Windows' artwork is never copied. 0 when the host does not draw @p id.
    [[nodiscard]] virtual u32 register_oem_bitmap(u32 id) { (void)id; return 0; }

    /// A checkbox or radio-button glyph as the host draws one (DrawFrameControl
    /// DFC_BUTTON): straight-alpha RGBA, top row first, @p width x @p height.
    /// False when the host has no renderer.
    struct ButtonGlyph {
        bool radio = false;
        bool checked = false;
        bool mixed = false;    // indeterminate (3-state checked)
        bool pushed = false;
        bool enabled = true;
    };
    [[nodiscard]] virtual bool render_button_glyph(ButtonGlyph glyph, i32 width, i32 height,
                                                   std::vector<u8>& rgba) {
        (void)glyph; (void)width; (void)height; (void)rgba;
        return false;
    }
};

} // namespace vitreo::windowing
