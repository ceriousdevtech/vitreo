// SPDX-License-Identifier: LGPL-2.1-only
// ID2D1RenderTarget::DrawText / DrawTextLayout / DrawGlyphRun and the text
// rendering-params state (d2d1.h). DirectWrite formats the text; glyph
// coverage is DirectWrite's grayscale/aliased rasterization
// (dwrite::rasterize_glyph_run), composited with the brush as Direct2D
// composites its glyph textures. Learn: D2D1_TEXT_ANTIALIAS_MODE remarks,
// DrawTextLayout, D2D1_DRAW_TEXT_OPTIONS.
#include "d2d1_capabilities_contract.inc"
#include "d2d1_internal.hpp"
#include "d2d1_text_contract.inc"
#include "graphics/common/path_geometry_math.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/dwrite/dwrite.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
namespace vitreo::winapi {
using namespace d2d1_contract;
using namespace d2d1_text_contract;

struct D2DManager::TextRun {
  dwrite::MatchedFont font;
  u32 simulations{0};
  float size{0};
  float x{0}, y{0};  // baseline origin, DIPs, before the target transform
  std::vector<u16> glyphs;
  std::vector<float> advances;
  std::vector<dwrite::GlyphOffset> offsets;
  u32 bidi_level{0};
  bool sideways{false};
  GuestAddr brush{0};
};

namespace {

void text_gap(const char *method, const char *what) {
  static std::mutex mutex;
  static std::set<std::string> once;
  std::lock_guard lock(mutex);
  if (once.insert(std::string(method) + what).second)
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED d2d1.dll!ID2D1RenderTarget::%s: %s\n",
                 method, what);
}

float bits_float(u64 bits) {
  float value = 0;
  const u32 low = static_cast<u32>(bits);
  std::memcpy(&value, &low, sizeof(value));
  return value;
}

/// D2D1_POINT_2F passed by value: two stack slots on x86, one packed
/// register/slot on x64. Returns the next argument index.
usize read_point(Marshaler &m, usize index, float &x, float &y) {
  if (m.cpu_is_64bit()) {
    const u64 packed = m.arg(index);
    x = bits_float(packed);
    y = bits_float(packed >> 32);
    return index + 1;
  }
  x = bits_float(m.arg(index));
  y = bits_float(m.arg(index + 1));
  return index + 2;
}

bool has_color_glyph_tables(const dwrite::MatchedFont &font) {
  for (const char *tag : {"COLR", "sbix", "CBDT", "SVG "}) {
    const u32 value = static_cast<u32>(tag[0]) | static_cast<u32>(tag[1]) << 8 |
                      static_cast<u32>(tag[2]) << 16 | static_cast<u32>(tag[3]) << 24;
    if (dwrite::font_table(font, value)) return true;
  }
  return false;
}

}  // namespace

void D2DManager::draw_text(Marshaler &m, usize slot) {
  const GuestAddr self = m.arg(0);
  const auto target_of = [&]() -> Resource * {
    auto it = m_resources.find(self);
    if (it == m_resources.end() || it->second.brush || it->second.bitmap || !it->second.target)
      return nullptr;
    return &it->second;
  };

  if (slot == target_SetTextRenderingParams || slot == target_GetTextRenderingParams) {
    // COM references are taken and dropped outside m_mutex (DirectWrite
    // objects re-enter DirectWrite, which may be inside a D2D callback).
    if (slot == target_SetTextRenderingParams) {
      const GuestAddr params = m.arg(1);
      if (params && m_guest_interface_call) (void)m_guest_interface_call(m, params, 1, nullptr, 0);
      GuestAddr old = 0;
      {
        std::lock_guard lock(m_mutex);
        Resource *resource = target_of();
        if (!resource) {
          (void)m.raise_access_violation(false, self);
          return;
        }
        old = std::exchange(resource->target->text_rendering_params, params);
      }
      if (old && m_guest_interface_call) (void)m_guest_interface_call(m, old, 2, nullptr, 0);
      return;
    }
    GuestAddr params = 0;
    {
      std::lock_guard lock(m_mutex);
      Resource *resource = target_of();
      if (!resource) {
        (void)m.raise_access_violation(false, self);
        return;
      }
      params = resource->target->text_rendering_params;
    }
    if (params && m_guest_interface_call) (void)m_guest_interface_call(m, params, 1, nullptr, 0);
    (void)write_pointer(m, m.arg(1), params);
    return;
  }

  // Everything DirectWrite contributes is gathered before m_mutex.
  DWriteManager::TextDrawing drawing;
  u32 format_hr = S_OK;
  GuestAddr default_brush = 0;
  u32 options = 0;
  float origin_x = 0, origin_y = 0;
  float box_width = 0, box_height = 0;
  std::vector<TextRun> glyph_runs;
  if (!m_dwrite) {
    format_hr = E_NOTIMPL;
  } else if (slot == target_DrawTextLayout) {
    // (D2D1_POINT_2F origin, IDWriteTextLayout*, ID2D1Brush*, options)
    usize i = read_point(m, 1, origin_x, origin_y);
    const GuestAddr layout = m.arg(i++);
    default_brush = m.arg(i++);
    options = static_cast<u32>(m.arg(i));
    if (!layout) {
      (void)m.raise_access_violation(false, layout);
      return;
    }
    format_hr = m_dwrite->text_drawing(layout, drawing);
  } else if (slot == target_DrawText) {
    // (string, length, IDWriteTextFormat*, D2D1_RECT_F const*, brush,
    //  options, DWRITE_MEASURING_MODE)
    const GuestAddr string = m.arg(1);
    const u32 length = static_cast<u32>(m.arg(2));
    const GuestAddr format = m.arg(3);
    const GuestAddr rect = m.arg(4);
    default_brush = m.arg(5);
    options = static_cast<u32>(m.arg(6));
    const u32 measuring = static_cast<u32>(m.arg(7));
    std::array<float, 4> r{};
    std::u16string text(length, u'\0');
    if (!format || !rect || !read_data(m, rect, r.data(), sizeof(r)) ||
        (length && !read_data(m, string, text.data(), text.size() * 2))) {
      if (!format || !rect) (void)m.raise_access_violation(false, format ? rect : format);
      return;
    }
    origin_x = r[0];
    origin_y = r[1];
    box_width = std::max(0.f, r[2] - r[0]);
    box_height = std::max(0.f, r[3] - r[1]);
    if (measuring != DWRITE_MEASURING_MODE_NATURAL) {
      // GDI-classic/natural measuring needs the GDI-compatible layout.
      text_gap("DrawText", "DWRITE_MEASURING_MODE_GDI_CLASSIC/GDI_NATURAL");
      format_hr = E_NOTIMPL;
    } else {
      format_hr = m_dwrite->text_drawing_for_format(format, std::move(text), box_width,
                                                    box_height, drawing);
    }
  } else {
    // DrawGlyphRun(D2D1_POINT_2F baselineOrigin, DWRITE_GLYPH_RUN const*,
    //              ID2D1Brush*, DWRITE_MEASURING_MODE)
    usize i = read_point(m, 1, origin_x, origin_y);
    const GuestAddr run = m.arg(i++);
    default_brush = m.arg(i++);
    if (!run) {
      (void)m.raise_access_violation(false, run);
      return;
    }
    using namespace dwrite_text_contract;
    const usize b = m_is_64bit ? 1 : 0, w = m_is_64bit ? 8 : 4;
    TextRun text;
    GuestAddr face = 0, indices = 0, advances = 0, offsets = 0;
    u32 size_bits = 0, count = 0, sideways = 0, bidi = 0;
    bool ok = read_data(m, run + offset_GLYPH_RUN_fontFace[b], &face, w) &&
              read_data(m, run + offset_GLYPH_RUN_fontEmSize[b], &size_bits, 4) &&
              read_data(m, run + offset_GLYPH_RUN_glyphCount[b], &count, 4) &&
              read_data(m, run + offset_GLYPH_RUN_glyphIndices[b], &indices, w) &&
              read_data(m, run + offset_GLYPH_RUN_glyphAdvances[b], &advances, w) &&
              read_data(m, run + offset_GLYPH_RUN_glyphOffsets[b], &offsets, w) &&
              read_data(m, run + offset_GLYPH_RUN_isSideways[b], &sideways, 4) &&
              read_data(m, run + offset_GLYPH_RUN_bidiLevel[b], &bidi, 4);
    if (!ok) return;
    std::memcpy(&text.size, &size_bits, 4);
    text.x = origin_x;
    text.y = origin_y;
    text.bidi_level = bidi;
    text.sideways = sideways != 0;
    text.glyphs.resize(count);
    if (count && (!indices || !read_data(m, indices, text.glyphs.data(), count * 2))) return;
    if (advances) {
      text.advances.resize(count);
      if (count && !read_data(m, advances, text.advances.data(), count * 4)) return;
    }
    if (offsets) {
      text.offsets.resize(count);
      if (count && !read_data(m, offsets, text.offsets.data(), count * 8)) return;
    }
    if (!m_dwrite->font_face_description(face, text.font, text.simulations)) {
      text_gap("DrawGlyphRun", "font face not created by this process's DirectWrite");
      format_hr = E_NOTIMPL;
    }
    glyph_runs.push_back(std::move(text));
  }

  GuestAddr params = 0;
  {
    std::lock_guard lock(m_mutex);
    Resource *resource = target_of();
    if (!resource) {
      (void)m.raise_access_violation(false, self);
      return;
    }
    params = resource->target->text_rendering_params;
  }
  const std::optional<u32> rendering_mode =
      params && m_dwrite ? m_dwrite->rendering_params_mode(params) : std::nullopt;

  std::lock_guard lock(m_mutex);
  Resource *resource = target_of();
  if (!resource) {
    (void)m.raise_access_violation(false, self);
    return;
  }
  auto &t = *resource->target;
  if (!t.drawing || !t.dc) {
    t.latch(D2DERR_WRONG_STATE);
    return;
  }
  if (t.error != S_OK) return;
  const auto brush_ok = [&](GuestAddr brush) {
    const auto it = m_resources.find(brush);
    if (it == m_resources.end() || !it->second.brush) {
      (void)m.raise_access_violation(false, brush);
      return false;
    }
    if (it->second.factory != resource->factory) {
      t.latch(D2DERR_WRONG_FACTORY);
      return false;
    }
    if (it->second.target != resource->target) {
      t.latch(D2DERR_WRONG_RESOURCE_DOMAIN);
      return false;
    }
    return true;
  };
  if (!brush_ok(default_brush)) return;
  if (format_hr != S_OK) {
    t.latch(format_hr);
    return;
  }
  // Learn DrawTextLayout: drawing effects that are not ID2D1Resource are
  // ignored; ID2D1Resource effects that are not brushes put the target in
  // an error state.
  const auto effect_brush = [&](GuestAddr effect, GuestAddr &brush) -> bool {
    brush = default_brush;
    if (!effect) return true;
    const auto it = m_resources.find(effect);
    if (it != m_resources.end()) {
      if (!it->second.brush) {
        t.latch(E_INVALIDARG);
        return false;
      }
      if (!brush_ok(effect)) return false;
      brush = effect;
      return true;
    }
    if (m_geometries.contains(effect) || m_factories.contains(effect)) {
      t.latch(E_INVALIDARG);
      return false;
    }
    return true;
  };
  if (slot != target_DrawGlyphRun) {
    if (!drawing.inline_objects.empty()) {
      // IDWriteInlineObject::Draw needs a Direct2D IDWriteTextRenderer the
      // guest can call; Vitreo has no guest-visible renderer object.
      text_gap(slot == target_DrawText ? "DrawText" : "DrawTextLayout",
               "inline objects (IDWriteInlineObject::Draw)");
      t.latch(E_NOTIMPL);
      return;
    }
    for (usize i = 0; i < drawing.layout.runs.size(); ++i) {
      const auto &run = drawing.layout.runs[i];
      if (run.glyphs.empty()) continue;
      TextRun text;
      text.font = run.font;
      text.simulations = run.simulations;
      text.size = run.size;
      text.x = origin_x + run.baseline_x;
      text.y = origin_y + run.baseline_y;
      text.glyphs = run.glyphs;
      text.advances = run.advances;
      text.offsets = run.offsets;
      text.bidi_level = run.bidi_level;
      if (!effect_brush(drawing.run_effects[i], text.brush)) return;
      glyph_runs.push_back(std::move(text));
    }
    if (slot == target_DrawTextLayout) {
      box_width = drawing.max_width;
      box_height = drawing.max_height;
    }
  } else {
    glyph_runs.front().brush = default_brush;
  }

  // Clip to the layout box (D2D1_DRAW_TEXT_OPTIONS_CLIP).
  const bool clip_option = (options & D2D1_DRAW_TEXT_OPTIONS_CLIP) != 0 && slot != target_DrawGlyphRun;
  if (clip_option) {
    if (t.transform[1] != 0 || t.transform[2] != 0) {
      text_gap("DrawTextLayout", "D2D1_DRAW_TEXT_OPTIONS_CLIP under a rotating transform");
      t.latch(E_NOTIMPL);
      return;
    }
    // The layout rectangle: origin to origin + (maxWidth, maxHeight).
    const float width = box_width, height = box_height;
    const float sx = t.transform[0] * t.dpi_x / dip_screen_dpi, sy = t.transform[3] * t.dpi_y / dip_screen_dpi;
    const float dx = t.transform[4] * t.dpi_x / dip_screen_dpi, dy = t.transform[5] * t.dpi_y / dip_screen_dpi;
    std::array<float, 4> box{origin_x * sx + dx, origin_y * sy + dy, (origin_x + width) * sx + dx,
                             (origin_y + height) * sy + dy};
    if (box[0] > box[2]) std::swap(box[0], box[2]);
    if (box[1] > box[3]) std::swap(box[1], box[3]);
    if (!t.clips.empty()) {
      const auto &c = t.clips.back();
      box = {std::max(box[0], c[0]), std::max(box[1], c[1]), std::min(box[2], c[2]), std::min(box[3], c[3])};
    }
    t.clips.push_back(box);
  }
  render_text(m, t, *resource, default_brush, glyph_runs, options | (rendering_mode ? (*rendering_mode << 8) : 0));
  if (t.error == S_OK) {
    for (const auto &d : drawing.decorations) {
      GuestAddr brush = default_brush;
      if (!effect_brush(d.effect, brush)) break;
      const float top = origin_y + d.baseline_y + d.offset;
      const float left = origin_x + d.x;
      const graphics::PathElement rectangle[] = {
          {graphics::PathVerb::Move, {left, top}},
          {graphics::PathVerb::Line, {left + d.width, top}},
          {graphics::PathVerb::Line, {left + d.width, top + d.thickness}},
          {graphics::PathVerb::Line, {left, top + d.thickness}},
          {graphics::PathVerb::Close, {}}};
      auto transform = t.transform;
      for (usize i : {usize(0), usize(2), usize(4)}) transform[i] *= t.dpi_x / dip_screen_dpi;
      for (usize i : {usize(1), usize(3), usize(5)}) transform[i] *= t.dpi_y / dip_screen_dpi;
      std::vector<graphics::PathEdge> edges;
      if (!graphics::path_edges(rectangle, false, 0, transform, edges)) continue;
      const auto &b = m_resources.at(brush);
      const float alpha = std::clamp(b.color[3], 0.f, 1.f) * std::clamp(b.opacity, 0.f, 1.f);
      fill_device_edges(t, edges,
                        {std::clamp(b.color[0], 0.f, 1.f) * alpha, std::clamp(b.color[1], 0.f, 1.f) * alpha,
                         std::clamp(b.color[2], 0.f, 1.f) * alpha, alpha},
                        false, t.antialias == D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }
  }
  if (clip_option) t.clips.pop_back();
}

void D2DManager::render_text(Marshaler &m, Target &t, Resource &resource, GuestAddr default_brush,
                             const std::vector<TextRun> &runs, u32 options) {
  (void)m;
  (void)resource;
  (void)default_brush;
  const u32 rendering_mode = options >> 8;
  options &= 0xFF;
  const char *method = "DrawTextLayout";
  // Learn D2D1_TEXT_ANTIALIAS_MODE: DEFAULT is ClearType, downgraded to
  // aliased by an ALIASED rendering mode, and to grayscale by an OUTLINE
  // rendering mode or a target with alpha that is not D2D1_ALPHA_MODE_IGNORE.
  u32 mode = t.text_antialias;
  if (mode == D2D1_TEXT_ANTIALIAS_MODE_DEFAULT) {
    if (rendering_mode == DWRITE_RENDERING_MODE_ALIASED) mode = D2D1_TEXT_ANTIALIAS_MODE_ALIASED;
    else if (rendering_mode == DWRITE_RENDERING_MODE_OUTLINE || t.alpha != D2D1_ALPHA_MODE_IGNORE)
      mode = D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE;
    else mode = D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE;
  }
  if (mode == D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE) {
    text_gap(method, "ClearType (subpixel) text antialiasing");
    t.latch(E_NOTIMPL);
    return;
  }
  const bool antialias = mode != D2D1_TEXT_ANTIALIAS_MODE_ALIASED;
  auto transform = t.transform;
  for (usize i : {usize(0), usize(2), usize(4)}) transform[i] *= t.dpi_x / dip_screen_dpi;
  for (usize i : {usize(1), usize(3), usize(5)}) transform[i] *= t.dpi_y / dip_screen_dpi;
  std::array<i32, 4> clip{0, 0, i32(t.width), i32(t.height)};
  if (!t.clips.empty()) {
    const auto &c = t.clips.back();
    const auto edge = [](float x, u32 size) { return i32(std::ceil(std::clamp(x - .5f, 0.f, float(size)))); };
    clip = {edge(c[0], t.width), edge(c[1], t.height), edge(c[2], t.width), edge(c[3], t.height)};
  }
  for (const TextRun &run : runs) {
    if (run.glyphs.empty()) continue;
    if (run.sideways) {
      text_gap(method, "sideways glyph runs");
      t.latch(E_NOTIMPL);
      return;
    }
    if (run.simulations != 0) {
      text_gap(method, "DWRITE_FONT_SIMULATIONS (emboldened/oblique faces)");
      t.latch(E_NOTIMPL);
      return;
    }
    if ((options & D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT) && has_color_glyph_tables(run.font)) {
      text_gap(method, "D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT color glyphs");
      t.latch(E_NOTIMPL);
      return;
    }
    // Vertical snapping of the baseline to a device pixel (Learn
    // D2D1_DRAW_TEXT_OPTIONS_NONE), for transforms that keep rows rows.
    float baseline_y = run.y;
    if (!(options & D2D1_DRAW_TEXT_OPTIONS_NO_SNAP) && transform[1] == 0 && transform[3] != 0) {
      const float device = transform[3] * run.y + transform[5];
      baseline_y = (std::round(device) - transform[5]) / transform[3];
    }
    dwrite::GlyphRunRasterParams params;
    params.font = run.font;
    params.font_em_size = run.size;
    params.pixels_per_dip = 1;
    params.transform = transform;
    params.baseline_origin_x = run.x;
    params.baseline_origin_y = baseline_y;
    params.glyph_indices = run.glyphs;
    params.glyph_advances = run.advances;
    params.glyph_offsets = run.offsets;
    params.bidi_level = run.bidi_level;
    const auto mask = dwrite::rasterize_glyph_run(params, antialias);
    if (!mask) {
      text_gap(method, "glyph rasterization failed");
      t.latch(E_NOTIMPL);
      return;
    }
    const i32 width = mask->right - mask->left, height = mask->bottom - mask->top;
    if (width <= 0 || height <= 0) continue;
    const auto &brush = m_resources.at(run.brush);
    const float alpha = std::clamp(brush.color[3], 0.f, 1.f) * std::clamp(brush.opacity, 0.f, 1.f);
    if (alpha == 0) continue;
    const std::array<float, 4> premultiplied{std::clamp(brush.color[0], 0.f, 1.f) * alpha,
                                             std::clamp(brush.color[1], 0.f, 1.f) * alpha,
                                             std::clamp(brush.color[2], 0.f, 1.f) * alpha, alpha};
    if (t.gpu) {
      auto image = std::make_shared<Bitmap>();
      image->width = static_cast<u32>(width);
      image->height = static_cast<u32>(height);
      image->bytes.resize(usize(width) * height * 4);
      for (usize i = 0; i < mask->alpha.size(); ++i) {
        const float coverage = mask->alpha[i] / 255.f;
        for (usize c = 0; c < 4; ++c)
          image->bytes[i * 4 + c] = static_cast<u8>(std::nearbyint(premultiplied[c] * coverage * 255.f));
      }
      graphics::BackendTexture2DDesc desc{};
      desc.width = image->width;
      desc.height = image->height;
      desc.shader_resource = true;
      desc.pixel_format = graphics::PixelFormat::RGBA8Unorm;
      image->texture = m_backend ? m_backend->create_texture_2d(desc, image->bytes.data(), image->width * 4) : nullptr;
      if (!image->texture || !image->texture->valid()) {
        t.latch(D2DERR_RECREATE_TARGET);
        return;
      }
      t.pending_bitmaps.push_back(image);
      if (!t.gpu->draw_bitmap(*image->texture,
                              {float(mask->left), float(mask->top), float(mask->right), float(mask->bottom)},
                              {0, 0, 1, 1}, 1, false, false, clip)) {
        t.latch(D2DERR_RECREATE_TARGET);
        return;
      }
      t.dirty = true;
      continue;
    }
    const auto quantize = [](float x) { return std::nearbyint(std::clamp(x, 0.f, 1.f) * 255.f) / 255.f; };
    for (i32 row = 0; row < height; ++row) {
      const i32 y = mask->top + row;
      if (y < clip[1] || y >= clip[3]) continue;
      for (i32 column = 0; column < width; ++column) {
        const i32 x = mask->left + column;
        if (x < clip[0] || x >= clip[2]) continue;
        const float coverage = mask->alpha[usize(row) * width + column] / 255.f;
        if (coverage == 0) continue;
        float *pixel = t.pixels.get() + (usize(y) * t.width + x) * 4;
        const float a = quantize(alpha) * coverage;
        for (usize c = 0; c < 4; ++c)
          pixel[c] = quantize(quantize(premultiplied[c]) * coverage + quantize(pixel[c]) * (1 - a));
      }
    }
    t.dirty = true;
  }
}

}  // namespace vitreo::winapi
