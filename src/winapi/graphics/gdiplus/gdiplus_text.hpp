#pragma once
/// @file gdiplus_text.hpp
/// @brief GDI+ text objects (GpFontFamily, GpFont, GpFontCollection,
/// GpStringFormat) and the text layout DrawString, MeasureString,
/// MeasureCharacterRanges and AddPathString share.
///
/// Contract sources: Windows SDK GdiPlusFlat.h / GdiPlusEnums.h /
/// GdiPlusStringFormat.h / GdiPlusFontFamily.h / GdiPlusFont.h, the
/// Microsoft Learn FontFamily / Font / FontCollection / StringFormat /
/// Graphics::DrawString / MeasureString pages, and the winoracle probe
/// tools/oracle_probe/gdiplus_text_probe.c (.expected beside it) for every
/// value Learn does not state (layout metrics, padding, tab stops,
/// trimming, defaults, error codes). Notes: .docs/contracts/gdiplus/text.md.
///
/// Fonts are the same host faces GDI realises (dwrite::match_system_font,
/// GDI's FontSubstitutes table), never Windows font files.

#include "winapi/graphics/gdiplus/gdiplus_core.hpp"
#include "winapi/graphics/dwrite/font_backend.hpp"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi::gdip {

struct GpGraphics;

/// GDI+ `FontStyle` bits (GdiPlusEnums.h).
enum FontStyle : i32 {
    FontStyleRegular = 0,
    FontStyleBold = 1,
    FontStyleItalic = 2,
    FontStyleUnderline = 4,
    FontStyleStrikeout = 8,
};

/// GDI+ `StringFormatFlags` (GdiPlusEnums.h).
enum StringFormatFlags : i32 {
    StringFormatFlagsDirectionRightToLeft = 0x00000001,
    StringFormatFlagsDirectionVertical = 0x00000002,
    StringFormatFlagsNoFitBlackBox = 0x00000004,
    StringFormatFlagsDisplayFormatControl = 0x00000020,
    StringFormatFlagsNoFontFallback = 0x00000400,
    StringFormatFlagsMeasureTrailingSpaces = 0x00000800,
    StringFormatFlagsNoWrap = 0x00001000,
    StringFormatFlagsLineLimit = 0x00002000,
    StringFormatFlagsNoClip = 0x00004000,
};

/// GDI+ `StringTrimming`, `HotkeyPrefix`, `StringAlignment`.
enum StringTrimming : i32 {
    StringTrimmingNone = 0,
    StringTrimmingCharacter = 1,
    StringTrimmingWord = 2,
    StringTrimmingEllipsisCharacter = 3,
    StringTrimmingEllipsisWord = 4,
    StringTrimmingEllipsisPath = 5,
};
enum HotkeyPrefix : i32 { HotkeyPrefixNone = 0, HotkeyPrefixShow = 1, HotkeyPrefixHide = 2 };
enum StringAlignment : i32 { StringAlignmentNear = 0, StringAlignmentCenter = 1, StringAlignmentFar = 2 };

/// GDI+ `DriverStringOptions`.
enum DriverStringOptions : i32 {
    DriverStringOptionsCmapLookup = 1,
    DriverStringOptionsVertical = 2,
    DriverStringOptionsRealizedAdvance = 4,
    DriverStringOptionsLimitSubpixel = 8,
};

/// One face of a family with the design metrics GDI+ reports for it.
struct TextFace {
    dwrite::MatchedFont font;
    u32 upem{2048};
    u32 win_ascent{0};      ///< OS/2 usWinAscent (GdipGetCellAscent)
    u32 win_descent{0};     ///< OS/2 usWinDescent (GdipGetCellDescent)
    u32 line_spacing{0};    ///< GdipGetLineSpacing (design units)
    i32 underline_position{0};   ///< post.underlinePosition (up positive)
    i32 underline_thickness{0};
    i32 strikeout_position{0};   ///< OS/2 yStrikeoutPosition (up positive)
    i32 strikeout_size{0};
    bool simulated_bold{false};
    bool simulated_italic{false};
    /// 'kern' format-0 pairs: (left << 16 | right) -> value (design units).
    std::unordered_map<u32, i16> kerning;
};

struct GpFontCollection;

/// A FontFamily. GDI+ hands out ONE object per (collection, family):
/// GdipCreateFontFamilyFromName, the collection's family list and
/// GdipCloneFontFamily all return the same pointer, and
/// GdipDeleteFontFamily leaves it alive (gdiplus_text_probe "family
/// two-creates same clone same", "installed listall ... contains").
struct GpFontFamily {
    std::u16string name;   ///< the family's own name (GdipGetFamilyName)
    /// Faces of a private collection's family; empty for an installed one,
    /// whose styles resolve through the host font matcher.
    std::vector<dwrite::MatchedFont> private_faces;
    /// Resolved faces by (style & 3); filled on first use.
    mutable std::array<std::shared_ptr<const TextFace>, 4> faces;
};

/// A Font: exactly the four properties GDI+ stores (family, em size,
/// style, unit); every getter derives from them.
struct GpFont {
    u64 family{0};   ///< the GpFontFamily handle GdipGetFamily returns
    std::shared_ptr<GpFontFamily> family_object;
    float em_size{0};
    i32 style{0};
    i32 unit{0};
};

/// An installed (process-wide singleton) or private font collection.
struct GpFontCollection {
    bool installed{false};
    /// Private collections: families by name, in the order first added.
    struct Family {
        std::u16string name;
        u64 handle{0};  ///< its GpFontFamily
    };
    std::vector<Family> families;
};

struct CharacterRange { i32 first{0}; i32 length{0}; };

/// A StringFormat. A created format starts with StringTrimmingCharacter;
/// the generic typographic one with flags 0x6004 and no trimming
/// (gdiplus_text_probe "fresh", "generictypographic").
struct GpStringFormat {
    i32 flags{0};
    u16 language{0};
    i32 align{StringAlignmentNear};
    i32 line_align{StringAlignmentNear};
    i32 trimming{StringTrimmingCharacter};
    i32 hotkey{HotkeyPrefixNone};
    float first_tab{0};
    std::vector<float> tabs;
    u16 digit_language{0};
    i32 digit_substitute{0};
    std::vector<CharacterRange> ranges;
    /// GenericTypographic (and its clones): no 1/6-em padding and nominal
    /// advances, whatever the flags say (probe: CreateStringFormat(0x6004)
    /// still pads).
    bool typographic{false};
    /// One of the two process-wide generic formats (delete is a no-op).
    bool generic{false};
    /// The format a NULL GpStringFormat* means: as GenericDefault, but its
    /// tab stops default to every 4 em (probe "t8 def" vs "tabpos set0").
    bool implicit_default{false};
};

// ---- Faces -----------------------------------------------------------------

/// @p font with its design metrics read from its tables (head, hhea, OS/2,
/// post); @p style marks bold/italic simulation when the face lacks them.
[[nodiscard]] std::shared_ptr<const TextFace> text_face_from(dwrite::MatchedFont font, i32 style);

/// The face @p family uses for @p style (bold/italic bits), simulated from
/// the regular face when the family has no such member. Null only when no
/// host face backs the family at all.
[[nodiscard]] std::shared_ptr<const TextFace> family_face(const GpFontFamily& family, i32 style);

// ---- Layout ----------------------------------------------------------------

/// One glyph placed by the layout, in WORLD coordinates.
struct PlacedGlyph {
    std::shared_ptr<const TextFace> face;
    u16 glyph{0};
    float x{0};         ///< pen position (left of the advance)
    float baseline{0};
    float advance{0};
    i32 source{0};      ///< index of the source UTF-16 unit it renders
};

/// A decoration (underline, strikeout, hotkey underline) rectangle.
struct TextDecoration { RectF rect; };

struct TextLayout {
    std::vector<PlacedGlyph> glyphs;
    std::vector<TextDecoration> decorations;
    RectF bounds;              ///< MeasureString's bounding box
    i32 codepoints_fitted{0};
    i32 lines_filled{0};
    float em{0};               ///< em size in world units
    float line_height{0};
    /// Per source unit: the cell it occupies [x0, x1] x [top, bottom], for
    /// MeasureCharacterRanges (absent when not laid out / not visible).
    struct Cell { bool visible{false}; float x0{0}, x1{0}, top{0}, bottom{0}; };
    std::vector<Cell> cells;
    bool vertical{false};
};

/// What a layout is for: MeasureCharacterRanges always takes the full
/// (line-spacing) path; AddPathString pads by round(upem / 6) design units
/// (probe "pathstring": 341/2048 em, where MeasureString pads exactly 1/6).
enum class LayoutPurpose { Draw, Ranges, Path };

/// Lay out @p text with @p face at @p em world units in @p layout (WORLD
/// coordinates) under @p format. @p style supplies underline/strikeout.
/// False when no face is available.
bool layout_text(const std::u16string& text, const std::shared_ptr<const TextFace>& face, float em,
                 i32 style, const RectF& layout, const GpStringFormat& format, LayoutPurpose purpose,
                 TextLayout& out);

/// Em size of @p font in @p g's world units (font unit through the page
/// transform; UnitWorld as given).
[[nodiscard]] float font_em_world(const GpFont& font, const GpGraphics& g);

/// Registers DrawString / MeasureString / MeasureCharacterRanges /
/// Draw/MeasureDriverString / AddPathString(I) (gdiplus_text_layout.cpp).
void register_text_layout(GdiplusManager& mgr, HandlerMap& map);

/// The format a NULL GpStringFormat* stands for.
[[nodiscard]] const GpStringFormat& implicit_default_format();

}  // namespace vitreo::winapi::gdip
