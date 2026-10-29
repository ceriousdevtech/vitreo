#pragma once
/// @file gdi32_internal.hpp
/// @brief Shared surface between gdi32.cpp and gdi32_registration.cpp.
///
/// Private to the gdi32 implementation. This was file-local before the export
/// table was split out.

#include "winapi/graphics/gdi32/gdi32.hpp"

#include <vitreo/common.hpp>

#include <unordered_map>

namespace vitreo::winapi {

/// Font metrics kept per HFONT.
///
/// GDI hands the guest an opaque handle and expects to answer GetObject and
/// text-measurement calls about it later, so the description has to outlive
/// the call that created it. Keyed by handle rather than held in the manager
/// because stock fonts exist before any manager instance does.
[[nodiscard]] std::unordered_map<Handle, GdiManager::FontInfo>& gdi_font_registry();
// Always acquired after the owner lock; registry scopes never call an owner.
[[nodiscard]] std::mutex& gdi_font_registry_mutex();

// Ternary raster operation codes. The low byte is an index and the middle two
// bytes encode the boolean expression itself, which is why these are not
// sequential — a guest may pass any of the 256 defined ROPs, and these six are
// the ones with a fast path.
inline constexpr u32 SRCCOPY     = 0x00CC0020;  // D = S
inline constexpr u32 SRCPAINT    = 0x00EE0086;  // D = D | S
inline constexpr u32 SRCAND      = 0x008800C6;  // D = D & S
inline constexpr u32 SRCINVERT   = 0x00660046;  // D = D ^ S
inline constexpr u32 NOTSRCCOPY  = 0x00330008;  // D = ~S
inline constexpr u32 MERGEPAINT  = 0x00BB0226;  // D = ~S | D

class Marshaler;

/// Exports that answer from the DC's selected font itself rather than from the
/// DC's pixels. Each returns the value the export returns, GDI_ERROR included.
namespace gdi_font_data {

[[nodiscard]] u32 get_font_data(GdiManager& mgr, Marshaler& m);
[[nodiscard]] u32 get_glyph_indices(GdiManager& mgr, Marshaler& m, bool wide);
[[nodiscard]] u32 get_glyph_outline(GdiManager& mgr, Marshaler& m);
[[nodiscard]] u32 get_char_widths_i(GdiManager& mgr, Marshaler& m, bool abc);

} // namespace gdi_font_data

/// A font's 'name' table string @p id (Windows platform, US English first).
[[nodiscard]] std::u16string font_name_string(const dwrite::MatchedFont& font, u32 id);
/// An OpenType table's bytes, empty when absent.
[[nodiscard]] std::vector<u8> font_table_bytes(const dwrite::MatchedFont& font, u32 id);

/// The text exports (text_exports.cpp): marshal the guest arguments, apply
/// the measured failure contracts and call the text engine.
namespace gdi_text_exports {

void text_out(GdiManager& mgr, Marshaler& m, bool wide);
void ext_text_out(GdiManager& mgr, Marshaler& m, bool wide);
void get_text_extent_point(GdiManager& mgr, Marshaler& m, bool wide);
void get_text_extent_ex_point(GdiManager& mgr, Marshaler& m, bool wide);
void get_text_extent_point_i(GdiManager& mgr, Marshaler& m);
void get_text_metrics(GdiManager& mgr, Marshaler& m, bool wide);
void get_char_width(GdiManager& mgr, Marshaler& m, bool wide);
void get_char_abc_widths(GdiManager& mgr, Marshaler& m, bool wide);
void get_char_abc_widths_float(GdiManager& mgr, Marshaler& m, bool wide);
void get_text_face(GdiManager& mgr, Marshaler& m, bool wide);
void set_text_character_extra(GdiManager& mgr, Marshaler& m);
void get_text_character_extra(GdiManager& mgr, Marshaler& m);
void set_text_align(GdiManager& mgr, Marshaler& m);
void get_text_align(GdiManager& mgr, Marshaler& m);
void add_font_mem_resource_ex(GdiManager& mgr, Marshaler& m);
void remove_font_mem_resource_ex(GdiManager& mgr, Marshaler& m);
/// AddFontResource[Ex]A/W; @p ex says whether fl/pdv follow the name.
void add_font_resource(GdiManager& mgr, Marshaler& m, bool wide, bool ex);
void remove_font_resource(GdiManager& mgr, Marshaler& m, bool wide, bool ex);

} // namespace gdi_text_exports

/// Path, polygon, transform and clip exports (geometry_exports.cpp).
namespace gdi_geometry_exports {

enum class PolyKind { Polyline, PolylineTo, Polygon, PolyBezier, PolyBezierTo };
enum class PathOp { Begin, End, Abort, CloseFigure, Fill, Stroke, StrokeAndFill, Flatten, Widen };

void poly_points(GdiManager& mgr, Marshaler& m, PolyKind kind);
void poly_polygon(GdiManager& mgr, Marshaler& m);
void path_op(GdiManager& mgr, Marshaler& m, PathOp op);
void select_clip_path(GdiManager& mgr, Marshaler& m);
void path_to_region(GdiManager& mgr, Marshaler& m);
void get_path(GdiManager& mgr, Marshaler& m);
void convert_points(GdiManager& mgr, Marshaler& m, bool to_logical);
void set_map_mode(GdiManager& mgr, Marshaler& m);
void get_map_mode(GdiManager& mgr, Marshaler& m);
void set_extent(GdiManager& mgr, Marshaler& m, bool viewport);
void set_graphics_mode(GdiManager& mgr, Marshaler& m);
void set_world_transform(GdiManager& mgr, Marshaler& m);
void get_world_transform(GdiManager& mgr, Marshaler& m);
void modify_world_transform(GdiManager& mgr, Marshaler& m);
void set_miter_limit(GdiManager& mgr, Marshaler& m);
void get_miter_limit(GdiManager& mgr, Marshaler& m);
void set_arc_direction(GdiManager& mgr, Marshaler& m);
void get_arc_direction(GdiManager& mgr, Marshaler& m);
void get_clip_rgn(GdiManager& mgr, Marshaler& m);
void offset_clip_rgn(GdiManager& mgr, Marshaler& m);
void create_polygon_rgn(GdiManager& mgr, Marshaler& m, bool poly);

} // namespace gdi_geometry_exports

} // namespace vitreo::winapi
