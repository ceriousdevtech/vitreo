// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_text_layout.cpp
/// @brief GDI+ text layout and the exports built on it: GdipDrawString,
/// GdipMeasureString, GdipMeasureCharacterRanges, GdipDrawDriverString,
/// GdipMeasureDriverString, GdipAddPathString(I).
///
/// The layout model is GDI+'s, measured on Windows 11 with
/// tools/oracle_probe/gdiplus_text_probe.c (identical for every
/// TextRenderingHint on a bitmap, and for x86/x64):
///  - advances are the face's design advances at the em size; a format
///    other than GenericTypographic widens each to round(advance * 1.03)
///    design units and pads the line by 1/6 em at each end, and adds 1/8 em
///    below the block ("t0 def" vs "t0 typo": Arial 20 px "Hello World"
///    113.1217 x 24.8438 vs 103.3594 x 22.3438);
///  - a line is the font's line spacing tall; a lone line of plain text
///    (no tab, break, hotkey processing, trimming, fallback, RTL/vertical)
///    is only the cell (ascent + descent) tall ("t7"/"t8" vs "t0");
///  - the baseline is the line top plus the cell ascent;
///  - lines break after spaces, tabs and hyphens and between CJK
///    characters; a word wider than the line breaks between characters;
///    trailing spaces hang (MeasureTrailingSpaces counts them);
///  - tab stops: the first stop at firstTabOffset + tabs[0], then
///    cumulative, then every multiple of the last interval; with no stops a
///    tab is one design unit wide, except under a NULL format, whose stops
///    are every 4 em ("tabpos set0..7", "t8 def");
///  - with a height limit, trimming None lays out every line and clips the
///    box; LineLimit keeps only whole lines; otherwise the first line is
///    always kept and a later one while enough of it shows ("partial ...");
///  - trimming re-fills the last visible line (by character, by word, or
///    with U+2026 at its nominal advance), codepoints fitted counting what
///    remains; a NoWrap line trims at the layout width;
///  - alignment places each line (Near is the right edge under
///    DirectionRightToLeft); line alignment places the block, a zero
///    layout extent aligning about the origin; the box clips to a non-zero
///    layout extent.

#include "winapi/graphics/gdiplus/gdiplus_text.hpp"
#include "winapi/graphics/gdiplus/gdiplus.hpp"
#include "winapi/graphics/gdiplus/gdiplus_graphics.hpp"
#include "winapi/graphics/gdiplus/gdiplus_geometry.hpp"
#include "winapi/graphics/gdiplus/gdiplus_region.hpp"
#include "winapi/graphics/gdiplus/gdiplus_render.hpp"
#include "winapi/graphics/gdiplus/gdiplus_pen_brush.hpp"
#include "winapi/core/marshaler.hpp"

#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace vitreo::winapi::gdip {

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
/// Non-typographic advance widening (probe "t1 def": Arial 'x' 1024 units
/// -> 1055, 'i' 455 -> 469, 'W' 1933 -> 1991).
constexpr double kAdvanceWidening = 1.03;
constexpr char16_t kEllipsis = 0x2026;

bool is_cjk(u32 c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x20000 && c <= 0x2FFFF);
}

/// One UTF-16 unit of the text after hotkey processing.
struct Unit16 {
    char16_t ch{0};
    u32 cp{0};            ///< full code point (0 for a trailing surrogate)
    i32 source{0};        ///< index in the caller's string
    i32 source_end{0};    ///< one past the last caller unit this unit consumes
    std::shared_ptr<const TextFace> face;
    u16 glyph{0};
    float advance{0};
    bool space{false}, tab{false}, hard_break{false}, control{false};
    bool hotkey{false};   ///< underlined by HotkeyPrefixShow
    bool fallback{false};
};

struct Line {
    usize begin{0}, end{0};     ///< [begin, end) units, trailing spaces and the break included
    std::vector<float> pos;     ///< pen x per unit, relative to the content start
    std::vector<float> adv;     ///< effective advance per unit (tab: its jump)
    float width{0};             ///< content width (trailing spaces hang)
    float full_width{0};        ///< width including trailing spaces
    bool ellipsis{false};
    float ellipsis_x{0};
    float ellipsis_w{0};
    usize consumed{0};          ///< units this line accounts for (codepoints fitted)
};

enum class Fit { Word, Char };

class Layouter {
public:
    Layouter(const std::shared_ptr<const TextFace>& face, float em, const GpStringFormat& format)
        : m_face(face), m_em(em), m_format(format) {
        m_typo = format.typographic;
        m_unit = em / static_cast<float>(face->upem);
    }

    void shape(const std::u16string& text) {
        // Hotkey prefixes: '&' marks the next character (Show underlines
        // it, Hide does not), "&&" is a literal '&' (Learn HotkeyPrefix).
        const i32 n = static_cast<i32>(text.size());
        for (i32 i = 0; i < n; ++i) {
            Unit16 u;
            u.ch = text[static_cast<usize>(i)];
            u.source = i;
            u.source_end = i + 1;
            if (m_format.hotkey != HotkeyPrefixNone && u.ch == u'&') {
                if (i + 1 < n) {
                    u.ch = text[static_cast<usize>(i + 1)];
                    u.source_end = i + 2;
                    if (u.ch != u'&' && m_format.hotkey == HotkeyPrefixShow) u.hotkey = true;
                    ++i;
                } else {
                    continue;  // a final lone '&' is consumed
                }
            }
            m_units.push_back(u);
        }
        for (usize i = 0; i < m_units.size(); ++i) {
            Unit16& u = m_units[i];
            u32 cp = u.ch;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < m_units.size() && m_units[i + 1].ch >= 0xDC00 &&
                m_units[i + 1].ch < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (m_units[i + 1].ch - 0xDC00u);
            } else if (cp >= 0xDC00 && cp < 0xE000 && i > 0 && m_units[i - 1].cp >= 0x10000) {
                cp = 0;  // trailing half of a pair
            }
            u.cp = cp;
            u.space = u.ch == u' ';
            u.tab = u.ch == u'\t';
            u.hard_break = u.ch == u'\n' || u.ch == u'\r' || u.ch == 0x2028 || u.ch == 0x2029;
            u.control = !u.tab && !u.hard_break && (u.ch < 0x20 || u.ch == 0x7F);
            if (u.tab) m_has_tab = true;
            if (u.hard_break) m_has_break = true;
        }
        // Glyphs and advances, by face (the primary face, then fallbacks).
        std::vector<u32> cps;
        std::vector<usize> idx;
        for (usize i = 0; i < m_units.size(); ++i) {
            const Unit16& u = m_units[i];
            if (u.cp == 0 || u.tab || u.hard_break || u.control) continue;
            cps.push_back(u.cp);
            idx.push_back(i);
        }
        assign_glyphs(m_face, cps, idx, true);
    }

    [[nodiscard]] bool fallback_used() const { return m_fallback; }
    [[nodiscard]] bool has_tab() const { return m_has_tab; }
    [[nodiscard]] bool has_break() const { return m_has_break; }
    [[nodiscard]] float pad() const { return m_typo ? 0.0f : m_em / 6.0f; }
    [[nodiscard]] float extra() const { return m_typo ? 0.0f : m_em / 8.0f; }
    [[nodiscard]] std::vector<Unit16>& units() { return m_units; }
    [[nodiscard]] float unit() const { return m_unit; }

    /// Kern consecutive glyphs of one face by its 'kern' pairs.
    void apply_kerning() {
        for (usize i = 0; i + 1 < m_units.size(); ++i) {
            Unit16& a = m_units[i];
            const Unit16& b = m_units[i + 1];
            if (!a.face || a.face != b.face || a.glyph == 0 || b.glyph == 0) continue;
            const auto it = a.face->kerning.find((static_cast<u32>(a.glyph) << 16) | b.glyph);
            if (it == a.face->kerning.end()) continue;
            double v = it->second;
            if (!m_typo) v = std::round(v * kAdvanceWidening);
            a.advance += static_cast<float>(v) * m_em / static_cast<float>(a.face->upem);
        }
    }

    /// Next tab stop strictly after @p x (relative to the content start).
    [[nodiscard]] float tab_target(float x) const {
        if (!m_format.tabs.empty()) {
            float stop = m_format.first_tab;
            for (float t : m_format.tabs) {
                stop += t;
                if (stop > x) return stop;
            }
            const float interval = m_format.tabs.back();
            if (interval > 0) return std::floor(x / interval + 1.0f) * interval;
            return x + m_unit;
        }
        if (m_format.implicit_default) {
            const float interval = 4.0f * m_em;
            return std::floor(x / interval + 1.0f) * interval;
        }
        return x + m_unit;
    }

    /// Lay out from unit @p start: up to a hard break (consumed), or until
    /// @p max_w overflows.
    Line fit(usize start, float max_w, Fit mode) const {
        Line line;
        line.begin = start;
        float x = 0;
        float content = 0;
        usize last_break = 0;  // 0: no opportunity yet (an opportunity is > start)
        float break_content = 0, break_x = 0;
        usize i = start;
        const bool trailing = (m_format.flags & StringFormatFlagsMeasureTrailingSpaces) != 0;
        for (; i < m_units.size(); ++i) {
            const Unit16& u = m_units[i];
            if (u.hard_break) {
                usize end = i + 1;
                if (u.ch == u'\r' && end < m_units.size() && m_units[end].ch == u'\n') ++end;
                line.end = end;
                line.pos.resize(end - start, x);
                line.adv.resize(end - start, 0);
                line.width = trailing ? x : content;
                line.full_width = x;
                line.consumed = end - start;
                return line;
            }
            float adv = u.tab ? tab_target(x) - x : u.advance;
            if (!u.space && i > start && x + adv > max_w + 1e-4f) {
                usize end = i;
                if (mode == Fit::Word && last_break > start) {
                    end = last_break;
                    content = break_content;
                    x = break_x;
                }
                line.end = end;
                line.pos.resize(end - start);
                line.adv.resize(end - start);
                line.width = trailing ? std::min(x, max_w) : content;
                line.full_width = x;
                line.consumed = end - start;
                return line;
            }
            line.pos.push_back(x);
            line.adv.push_back(adv);
            x += adv;
            if (!u.space) content = x;
            const bool next_space = i + 1 < m_units.size() && m_units[i + 1].space;
            if ((u.space && !next_space) || u.tab || (u.ch == u'-' && !next_space && i + 1 < m_units.size() &&
                                                      !m_units[i + 1].hard_break)) {
                last_break = i + 1;
                break_content = content;
                break_x = x;
            }
            if (is_cjk(u.cp)) {
                last_break = i + 1;
                break_content = content;
                break_x = x;
            }
            if (i + 1 < m_units.size() && is_cjk(m_units[i + 1].cp) && !u.space) {
                last_break = i + 1;
                break_content = content;
                break_x = x;
            }
        }
        line.end = i;
        line.width = trailing ? x : content;
        line.full_width = x;
        line.consumed = i - start;
        return line;
    }

    /// The face's U+2026 at its nominal advance (probe "ellipsis ...":
    /// 2048 design units for Arial, not widened).
    [[nodiscard]] std::pair<u16, float> ellipsis() const {
        auto gids = dwrite::glyph_indices(m_face->font, {kEllipsis});
        u16 g = gids && !gids->empty() ? (*gids)[0] : 0;
        float w = 0;
        if (auto mt = dwrite::glyph_metrics(m_face->font, {g}); mt && !mt->empty()) {
            w = static_cast<float>((*mt)[0].advance_width) * m_unit;
        }
        return {g, w};
    }

private:
    void assign_glyphs(const std::shared_ptr<const TextFace>& face, const std::vector<u32>& cps,
                       const std::vector<usize>& idx, bool primary) {
        if (cps.empty()) return;
        auto gids = dwrite::glyph_indices(face->font, cps);
        std::vector<u16> glyphs = gids ? *gids : std::vector<u16>(cps.size(), 0);
        glyphs.resize(cps.size(), 0);
        std::vector<u32> missing_cps;
        std::vector<usize> missing_idx;
        const bool may_fall_back = primary && (m_format.flags & StringFormatFlagsNoFontFallback) == 0;
        for (usize k = 0; k < cps.size(); ++k) {
            if (glyphs[k] == 0 && may_fall_back && cps[k] > 0x20) {
                missing_cps.push_back(cps[k]);
                missing_idx.push_back(idx[k]);
            }
        }
        auto metrics = dwrite::glyph_metrics(face->font, glyphs);
        const float unit = m_em / static_cast<float>(face->upem);
        for (usize k = 0; k < cps.size(); ++k) {
            Unit16& u = m_units[idx[k]];
            u.face = face;
            u.glyph = glyphs[k];
            double units = metrics && k < metrics->size() ? (*metrics)[k].advance_width : 0;
            if (!m_typo) units = std::round(units * kAdvanceWidening);
            u.advance = static_cast<float>(units) * unit;
        }
        // Font fallback for what the face lacks (StringFormatFlagsNoFontFallback
        // turns it off), one host fallback face per character run.
        for (usize k = 0; k < missing_cps.size();) {
            std::u16string run;
            usize j = k;
            auto append = [&](u32 c) {
                if (c >= 0x10000) {
                    run.push_back(static_cast<char16_t>(0xD800 + ((c - 0x10000) >> 10)));
                    run.push_back(static_cast<char16_t>(0xDC00 + ((c - 0x10000) & 0x3FF)));
                } else {
                    run.push_back(static_cast<char16_t>(c));
                }
            };
            append(missing_cps[k]);
            auto match = dwrite::map_system_font(run, face->font.family_name, face->font.weight, 5,
                                                 face->font.style, u"");
            ++j;
            if (match && match->has_font) {
                const std::string key = std::string(match->font.postscript_name.begin(),
                                                    match->font.postscript_name.end());
                auto& cached = m_fallback_faces[key];
                if (!cached) cached = text_face_from(match->font, 0);
                assign_glyphs(cached, {missing_cps[k]}, {missing_idx[k]}, false);
                m_units[missing_idx[k]].fallback = true;
                m_fallback = true;
            }
            k = j;
        }
    }

    std::shared_ptr<const TextFace> m_face;
    float m_em;
    const GpStringFormat& m_format;
    bool m_typo{false};
    float m_unit{0};
    std::vector<Unit16> m_units;
    bool m_has_tab{false}, m_has_break{false}, m_fallback{false};
    std::map<std::string, std::shared_ptr<const TextFace>> m_fallback_faces;
};

}  // namespace

bool layout_text(const std::u16string& text, const std::shared_ptr<const TextFace>& face, float em, i32 style,
                 const RectF& layout_in, const GpStringFormat& format, LayoutPurpose purpose, TextLayout& out) {
    const bool for_ranges = purpose == LayoutPurpose::Ranges;
    out = TextLayout{};
    if (!face) return false;
    out.em = em;
    const bool vertical = (format.flags & StringFormatFlagsDirectionVertical) != 0;
    const bool rtl = (format.flags & StringFormatFlagsDirectionRightToLeft) != 0;
    const bool nowrap = (format.flags & StringFormatFlagsNoWrap) != 0;
    const bool line_limit = (format.flags & StringFormatFlagsLineLimit) != 0;
    out.vertical = vertical;
    // A negative extent is no extent (probe "negrect").
    RectF layout = layout_in;
    layout.Width = std::max(0.0f, layout.Width);
    layout.Height = std::max(0.0f, layout.Height);
    // Vertical text lays out in a frame turned 90 degrees: along = height.
    const float along = vertical ? layout.Height : layout.Width;
    const float across = vertical ? layout.Width : layout.Height;

    Layouter lay(face, em, format);
    lay.shape(text);
    auto& units = lay.units();
    float pad = lay.pad();
    float max_w = 0, wrap_w = 0;
    std::vector<Line> lines;
    auto break_lines = [&] {
        max_w = along > 0 ? std::max(along - 2 * pad, 0.0f) : kInf;
        wrap_w = nowrap ? kInf : max_w;
        lines.clear();
        for (usize start = 0; start < units.size();) {
            Line line = lay.fit(start, wrap_w, Fit::Word);
            if (line.end <= start) break;
            start = line.end;
            lines.push_back(std::move(line));
        }
    };
    break_lines();

    const float unit = lay.unit();
    const float cell = static_cast<float>(face->win_ascent + face->win_descent) * unit;
    const float ascent = static_cast<float>(face->win_ascent) * unit;
    const float spacing = static_cast<float>(face->line_spacing) * unit;
    const bool fits_one = lines.size() == 1 && !(nowrap && lines[0].width > max_w + 1e-4f);
    const bool simple = fits_one && !lay.has_tab() && !lay.has_break() && format.hotkey == HotkeyPrefixNone &&
                        !rtl && !vertical && !lay.fallback_used() && !for_ranges && purpose != LayoutPurpose::Path;
    if (!simple) {
        // The full layout path kerns (the face's kern pairs, widened like
        // advances) and pads by round(upem / 6) design units: "Wo\nWo"
        // 37.1875 vs "Wo"-free simple widths; "ab\nab" 29.5703 vs 29.5768.
        lay.apply_kerning();
        if (pad > 0) pad = std::round(static_cast<float>(face->upem) / 6.0f) * unit;
        break_lines();
    }
    const float lh = simple ? cell : spacing;

    // Visible lines.
    usize visible = lines.size();
    if (across > 0 && !lines.empty()) {
        if (line_limit) {
            visible = 0;
            while (visible < lines.size() && static_cast<float>(visible + 1) * lh <= across + 1e-4f) ++visible;
        } else if (format.trimming != StringTrimmingNone) {
            // A line shows while at least a quarter of its height is inside
            // ("partial-scan": Arial 20 px keeps line 2 from h = 29, line 3
            // from h = 52; Segoe UI from 33.5; Courier New 40 px needs
            // h >= 11.5 for even the first line).
            visible = 0;
            while (visible < lines.size() &&
                   across - static_cast<float>(visible) * lh >= lh * 0.25f - 1e-4f) {
                ++visible;
            }
        }
    }
    const bool hidden_text = visible < lines.size();
    if (lines.size() > visible) lines.resize(visible);

    // Trimming.
    const i32 trimming = format.trimming;
    auto trim_line = [&](Line& line, bool wrapped) {
        const float original = line.width;
        const float ew = lay.ellipsis().second;
        Line t;
        switch (trimming) {
        case StringTrimmingCharacter: t = lay.fit(line.begin, max_w, Fit::Char); break;
        case StringTrimmingWord: t = lay.fit(line.begin, max_w, Fit::Word); break;
        case StringTrimmingEllipsisCharacter: t = lay.fit(line.begin, std::max(0.0f, max_w - ew), Fit::Char); break;
        case StringTrimmingEllipsisWord: t = lay.fit(line.begin, std::max(0.0f, max_w - ew), Fit::Word); break;
        case StringTrimmingEllipsisPath: {
            // Keep the start, the ellipsis and the longest tail that fits.
            // GDI+ keeps the whole string as fitted (probe "trim=5 ... cp=43").
            t = lay.fit(line.begin, std::max(0.0f, max_w - ew), Fit::Char);
            t.consumed = units.size() - line.begin;
            break;
        }
        default: return;
        }
        const bool ellipsis = trimming >= StringTrimmingEllipsisCharacter;
        // A hard break ends the paragraph: the trimmed line stops before it.
        if (t.end > t.begin && units[t.end - 1].hard_break) {
            usize e = t.end;
            while (e > t.begin && units[e - 1].hard_break) --e;
            t.pos.resize(e - t.begin);
            t.adv.resize(e - t.begin);
            t.end = e;
            if (trimming != StringTrimmingEllipsisPath) t.consumed = e - t.begin;
        }
        if (ellipsis) {
            t.ellipsis = true;
            t.ellipsis_x = t.width;
            t.ellipsis_w = ew;
            t.width += ew;
        }
        // Wrapped lines keep their box width when an ellipsis shortens them
        // (probe "trim=4 w100h24": cp=4, width 95.9766 = the untrimmed line).
        if (wrapped && ellipsis) t.width = std::max(t.width, original);
        line = std::move(t);
    };
    if (trimming != StringTrimmingNone && !lines.empty()) {
        if (nowrap && along > 0) {
            for (Line& line : lines) {
                if (line.width > max_w + 1e-4f) trim_line(line, false);
            }
        } else if (hidden_text) {
            trim_line(lines.back(), true);
        }
    }

    // Codepoints fitted, in caller units.
    i32 fitted = 0;
    if (!lines.empty()) {
        const Line& last = lines.back();
        const usize end_unit = std::min(units.size(), last.begin + last.consumed);
        fitted = end_unit == 0 ? 0 : units[end_unit - 1].source_end;
        if (end_unit == units.size()) fitted = static_cast<i32>(text.size());
    }
    out.codepoints_fitted = fitted;
    out.lines_filled = static_cast<i32>(lines.size());
    out.line_height = lh;

    // Block geometry in the (along, across) frame.
    float block_w = 0;
    for (const Line& line : lines) block_w = std::max(block_w, line.width + 2 * pad);
    if (lines.empty()) block_w = 0;
    const float block_h = static_cast<float>(lines.size()) * lh + lay.extra();
    i32 align = format.align;
    if (rtl && !vertical) align = align == StringAlignmentNear ? StringAlignmentFar
                                   : align == StringAlignmentFar ? StringAlignmentNear : align;
    auto place = [](i32 a, float origin, float extent, float size) {
        if (a == StringAlignmentCenter) return extent > 0 ? origin + (extent - size) / 2 : origin - size / 2;
        if (a == StringAlignmentFar) return extent > 0 ? origin + extent - size : origin - size;
        return origin;
    };
    const float u_origin = 0, v_origin = 0;
    const float top0 = place(format.line_align, v_origin, across, block_h);
    float bx0 = kInf, bx1 = -kInf;
    std::vector<float> line_x(lines.size());
    for (usize k = 0; k < lines.size(); ++k) {
        const float w = lines[k].width + 2 * pad;
        line_x[k] = place(align, u_origin, along, w);
        bx0 = std::min(bx0, line_x[k]);
        bx1 = std::max(bx1, line_x[k] + w);
    }
    if (lines.empty()) bx0 = bx1 = place(align, u_origin, along, 0);
    float bw = std::max(bx1 - bx0, block_w);
    float bh = block_h;
    if (along > 0) bw = std::min(bw, along);
    if (across > 0) bh = std::min(bh, across);

    // Frame -> world.
    const float v_extent = across > 0 ? across : block_h;
    auto to_world = [&](float u, float v) -> PointF {
        if (!vertical) return {layout.X + u, layout.Y + v};
        return {layout.X + v_extent - v, layout.Y + u};
    };
    if (!vertical) {
        out.bounds = {layout.X + bx0, layout.Y + top0, bw, bh};
    } else {
        const PointF a = to_world(bx0, top0 + bh);
        out.bounds = {a.X, a.Y, bh, bw};
    }

    // Glyphs, cells, decorations.
    out.cells.assign(text.size(), {});
    for (usize k = 0; k < lines.size(); ++k) {
        const Line& line = lines[k];
        const float top = top0 + static_cast<float>(k) * lh;
        const float base_v = top + ascent;
        const float x0 = line_x[k] + pad;
        const usize count = std::min(line.pos.size(), line.end - line.begin);
        for (usize j = 0; j < count; ++j) {
            const Unit16& u = units[line.begin + j];
            const float x = x0 + line.pos[j];
            const float a = line.adv[j];
            for (i32 s = u.source; s < u.source_end && s < static_cast<i32>(out.cells.size()); ++s) {
                auto& c = out.cells[static_cast<usize>(s)];
                c.visible = true;
                c.x0 = x;
                c.x1 = x + a;
                c.top = top;
                c.bottom = top + lh;
            }
            if (u.face && u.glyph != 0 && !u.space) {
                const PointF p = to_world(x, base_v);
                out.glyphs.push_back({u.face, u.glyph, p.X, p.Y, u.advance, u.source});
            }
            if (u.hotkey && u.face) {
                const float s = unit;
                const float th = std::max(1.0f, static_cast<float>(face->underline_thickness) * s);
                const float y = base_v - static_cast<float>(face->underline_position) * s - th / 2;
                if (!vertical) out.decorations.push_back({{layout.X + x, layout.Y + y, a, th}});
            }
        }
        if (line.ellipsis) {
            const auto [eg, ew] = lay.ellipsis();
            const PointF p = to_world(x0 + line.ellipsis_x, base_v);
            if (eg != 0) out.glyphs.push_back({face, eg, p.X, p.Y, ew, -1});
        }
        const float content_w = line.ellipsis ? line.ellipsis_x + line.ellipsis_w : line.width;
        auto decorate = [&](i32 pos_units, i32 size_units) {
            const float th = std::max(1.0f, static_cast<float>(size_units) * unit);
            const float y = base_v - static_cast<float>(pos_units) * unit - th / 2;
            if (!vertical) {
                out.decorations.push_back({{layout.X + x0, layout.Y + y, content_w, th}});
            } else {
                const PointF p = to_world(x0, y + th);
                out.decorations.push_back({{p.X, p.Y, th, content_w}});
            }
        };
        if (style & FontStyleUnderline) decorate(face->underline_position, face->underline_thickness);
        if (style & FontStyleStrikeout) decorate(face->strikeout_position, face->strikeout_size);
    }
    return true;
}

namespace {

/// Append the outline of @p glyph (em @p em, origin at @p origin; rotated
/// a quarter turn clockwise for vertical text; then @p shape) to @p path.
void append_glyph(GpPath& path, const TextFace& face, u16 glyph, float em, PointF origin, bool vertical,
                  const Affine* shape) {
    dwrite::GlyphOutlineParams params;
    params.font = face.font;
    params.em_size = std::fabs(em);
    params.glyph_indices = {glyph};
    params.glyph_advances = {0.0f};
    auto outline = dwrite::glyph_run_outline(params);
    if (!outline) return;
    // Simulated oblique: a 1/3 em-per-em slant, as DirectWrite's simulation.
    const float skew = face.simulated_italic ? 0.33f : 0.0f;
    auto map = [&](dwrite::OutlinePoint p) -> PointF {
        float x = p.x - p.y * skew;
        float y = p.y;
        if (shape != nullptr) {
            const PointF t = shape->apply({x, y});
            x = t.X;
            y = t.Y;
        }
        if (vertical) return {origin.X - y, origin.Y + x};
        return {origin.X + x, origin.Y + y};
    };
    for (const auto& fig : outline->figures) {
        if (fig.segments.empty()) continue;
        path.points.push_back(map(fig.start));
        path.types.push_back(PathPointTypeStart);
        for (const auto& seg : fig.segments) {
            if (seg.kind == dwrite::OutlineSegmentKind::Line) {
                path.points.push_back(map(seg.line));
                path.types.push_back(PathPointTypeLine);
            } else {
                path.points.push_back(map(seg.bezier.control1));
                path.points.push_back(map(seg.bezier.control2));
                path.points.push_back(map(seg.bezier.end));
                path.types.insert(path.types.end(), 3, PathPointTypeBezier);
            }
        }
        // Glyph contours are always closed.
        path.types.back() = static_cast<u8>(path.types.back() | PathPointTypeCloseSubpath);
    }
}

void append_rect(GpPath& path, const RectF& r) {
    path.points.push_back({r.X, r.Y});
    path.points.push_back({r.X + r.Width, r.Y});
    path.points.push_back({r.X + r.Width, r.Y + r.Height});
    path.points.push_back({r.X, r.Y + r.Height});
    path.types.push_back(PathPointTypeStart);
    path.types.push_back(PathPointTypeLine);
    path.types.push_back(PathPointTypeLine);
    path.types.push_back(static_cast<u8>(PathPointTypeLine | PathPointTypeCloseSubpath));
}

/// The text a Gdip* string argument names: @p length units, NUL-terminated
/// for -1. False for any other negative length.
bool read_text(Marshaler& m, GuestAddr at, i32 length, std::u16string& out) {
    if (length < -1) return false;
    out = length == 0 ? std::u16string{} : read_wstr(m, at, length);
    return true;
}

/// Fill @p path with @p brush through @p g's renderer, clipped to @p clip
/// (world) when given, antialiased per the TextRenderingHint.
Status render_text_path(GdiplusManager& mgr, Marshaler& m, GpGraphics& g, const GpPath& path,
                        const GpBrush& brush, const RectF* clip) {
    RenderScope scope(mgr, m, g);
    if (scope.status() != Status::Ok) return scope.status();
    CGContextRef cg = scope.cg();
    if (cg == nullptr || path.points.empty()) return Status::Ok;
    CGContextSaveGState(cg);
    // TextRenderingHintSingleBitPerPixel(GridFit) draw aliased; the others
    // (SystemDefault, AntiAlias*, ClearTypeGridFit) antialiased.
    const i32 hint = g.state.text_rendering_hint;
    const bool aa = hint != 1 && hint != 2;
    CGContextSetShouldAntialias(cg, aa);
    CGContextSetAllowsAntialiasing(cg, aa);
    if (clip != nullptr) CGContextClipToRect(cg, CGRectMake(clip->X, clip->Y, clip->Width, clip->Height));
    CGMutablePathRef cgpath = path_to_cg(path);
    const Status s = fill_path(scope, cgpath, false, brush);
    CGPathRelease(cgpath);
    CGContextRestoreGState(cg);
    if (s != Status::Ok) return s;
    return scope.commit();
}

GpPath layout_path(const TextLayout& layout) {
    GpPath path;
    path.fill_mode = FillModeWinding;
    for (const PlacedGlyph& g : layout.glyphs) {
        const usize before = path.types.size();
        append_glyph(path, *g.face, g.glyph, layout.em, {g.x, g.baseline}, layout.vertical, nullptr);
        // Each glyph's figures end with a path marker (probe "pathstring
        // '3'": H's contour and i's second contour end 0xA1).
        if (path.types.size() > before) {
            path.types.back() = static_cast<u8>(path.types.back() | PathPointTypePathMarker);
        }
    }
    for (const TextDecoration& d : layout.decorations) append_rect(path, d.rect);
    return path;
}

struct DriverGlyph {
    u16 glyph{0};
    PointF pos;
    float advance{0};
};

/// Driver-string glyphs: cmap lookup or raw glyph indices, positions per
/// glyph or (RealizedAdvance) only the first.
std::vector<DriverGlyph> driver_glyphs(const std::vector<u16>& text, const TextFace& face, float em, i32 flags,
                                       const std::vector<PointF>& positions) {
    std::vector<u16> glyphs = text;
    if (flags & DriverStringOptionsCmapLookup) {
        std::vector<u32> cps(text.begin(), text.end());
        auto g = dwrite::glyph_indices(face.font, cps);
        if (g) glyphs = *g;
        glyphs.resize(text.size(), 0);
    }
    auto metrics = dwrite::glyph_metrics(face.font, glyphs);
    const float unit = em / static_cast<float>(face.upem);
    std::vector<DriverGlyph> out;
    PointF pen = positions.empty() ? PointF{} : positions[0];
    const bool realized = (flags & DriverStringOptionsRealizedAdvance) != 0;
    const bool vertical = (flags & DriverStringOptionsVertical) != 0;
    for (usize i = 0; i < glyphs.size(); ++i) {
        DriverGlyph d;
        d.glyph = glyphs[i];
        d.advance = metrics && i < metrics->size() ? static_cast<float>((*metrics)[i].advance_width) * unit : 0;
        d.pos = realized ? pen : (i < positions.size() ? positions[i] : pen);
        if (vertical) pen.Y = d.pos.Y + d.advance; else pen.X = d.pos.X + d.advance;
        out.push_back(d);
    }
    return out;
}

/// Shared argument handling of the driver-string exports.
struct DriverArgs {
    GpGraphics* g{nullptr};
    GpFont* font{nullptr};
    std::shared_ptr<const TextFace> face;
    std::vector<u16> text;
    std::vector<PointF> positions;
    i32 flags{0};
    bool has_matrix{false};
    Affine matrix;
};

}  // namespace

void register_text_layout(GdiplusManager& mgr, HandlerMap& map) {
    // ---- MeasureString ----
    map["GdipMeasureString"] = [&mgr](Marshaler& m) {
        GpGraphics* g = mgr.objects().get<GpGraphics>(handle_arg(m, 0));
        const GuestAddr str = m.arg_ptr(1);
        const i32 length = static_cast<i32>(m.arg32(2));
        GpFont* font = mgr.objects().get<GpFont>(handle_arg(m, 3));
        const GuestAddr rect_at = m.arg_ptr(4);
        const u64 fh = handle_arg(m, 5);
        const GuestAddr bb = m.arg_ptr(6);
        const GuestAddr cp_out = m.arg_ptr(7);
        const GuestAddr lines_out = m.arg_ptr(8);
        if (g == nullptr || str == 0 || font == nullptr || rect_at == 0 || bb == 0) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        const GpStringFormat* format = fh != 0 ? mgr.objects().get<GpStringFormat>(fh) : &implicit_default_format();
        if (format == nullptr) { write_status(m, Status::InvalidParameter); return; }
        std::u16string text;
        // A length below -1 is OutOfMemory on Windows (probe "measure len-2").
        if (!read_text(m, str, length, text)) { write_status(m, Status::OutOfMemory); return; }
        RectF rect;
        if (!read_rect(m, rect_at, rect)) { write_status(m, Status::InvalidParameter); return; }
        TextLayout layout;
        if (text.empty()) {
            layout.bounds = {0, 0, 0, 0};  // probe "measure len0" / "t5"
        } else {
            const auto face = family_face(*font->family_object, font->style);
            if (!layout_text(text, face, font_em_world(*font, *g), font->style, rect, *format, LayoutPurpose::Draw, layout)) {
                write_status(m, Status::FontStyleNotFound);
                return;
            }
        }
        write_rect(m, bb, layout.bounds);
        if (cp_out != 0) (void)m.write_u32(cp_out, static_cast<u32>(layout.codepoints_fitted));
        if (lines_out != 0) (void)m.write_u32(lines_out, static_cast<u32>(layout.lines_filled));
        write_status(m, Status::Ok);
    };

    // ---- DrawString ----
    map["GdipDrawString"] = [&mgr](Marshaler& m) {
        GpGraphics* g = mgr.objects().get<GpGraphics>(handle_arg(m, 0));
        const GuestAddr str = m.arg_ptr(1);
        const i32 length = static_cast<i32>(m.arg32(2));
        GpFont* font = mgr.objects().get<GpFont>(handle_arg(m, 3));
        const GuestAddr rect_at = m.arg_ptr(4);
        const u64 fh = handle_arg(m, 5);
        GpBrush* brush = mgr.objects().get<GpBrush>(handle_arg(m, 6));
        if (g == nullptr || str == 0 || font == nullptr || rect_at == 0 || brush == nullptr) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        if (const Status s = graphics_usable(g); s != Status::Ok) { write_status(m, s); return; }
        const GpStringFormat* format = fh != 0 ? mgr.objects().get<GpStringFormat>(fh) : &implicit_default_format();
        if (format == nullptr) { write_status(m, Status::InvalidParameter); return; }
        std::u16string text;
        if (!read_text(m, str, length, text)) { write_status(m, Status::OutOfMemory); return; }
        RectF rect;
        if (!read_rect(m, rect_at, rect)) { write_status(m, Status::InvalidParameter); return; }
        if (text.empty()) { write_status(m, Status::Ok); return; }
        const auto face = family_face(*font->family_object, font->style);
        TextLayout layout;
        if (!layout_text(text, face, font_em_world(*font, *g), font->style, rect, *format, LayoutPurpose::Draw, layout)) {
            write_status(m, Status::FontStyleNotFound);
            return;
        }
        if (face->simulated_bold) log_unimplemented("GdipDrawString", "simulated bold (family has no bold face)");
        const GpPath path = layout_path(layout);
        // The layout rectangle clips unless NoClip, in each direction it
        // has an extent (probe "IIII clipped 30x20" / "noclip").
        RectF clip = rect;
        const bool do_clip = (format->flags & StringFormatFlagsNoClip) == 0 && (rect.Width > 0 || rect.Height > 0);
        if (rect.Width <= 0) { clip.X = -1e7f; clip.Width = 2e7f; }
        if (rect.Height <= 0) { clip.Y = -1e7f; clip.Height = 2e7f; }
        write_status(m, render_text_path(mgr, m, *g, path, *brush, do_clip ? &clip : nullptr));
    };

    // ---- MeasureCharacterRanges ----
    map["GdipMeasureCharacterRanges"] = [&mgr](Marshaler& m) {
        GpGraphics* g = mgr.objects().get<GpGraphics>(handle_arg(m, 0));
        const GuestAddr str = m.arg_ptr(1);
        const i32 length = static_cast<i32>(m.arg32(2));
        GpFont* font = mgr.objects().get<GpFont>(handle_arg(m, 3));
        const GuestAddr rect_at = m.arg_ptr(4);
        GpStringFormat* format = mgr.objects().get<GpStringFormat>(handle_arg(m, 5));
        const i32 region_count = static_cast<i32>(m.arg32(6));
        const GuestAddr regions_at = m.arg_ptr(7);
        // A NULL format faults on Windows (probe "err 31"): refused here.
        if (g == nullptr || str == 0 || font == nullptr || rect_at == 0 || format == nullptr ||
            regions_at == 0 || length == 0) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        std::u16string text;
        if (!read_text(m, str, length, text)) { write_status(m, Status::OutOfMemory); return; }
        RectF rect;
        if (!read_rect(m, rect_at, rect)) { write_status(m, Status::InvalidParameter); return; }
        // Fewer regions than ranges, or a range past the text, is
        // InvalidParameter (probe "count-mismatch", "range-beyond-text").
        if (region_count < static_cast<i32>(format->ranges.size())) { write_status(m, Status::InvalidParameter); return; }
        const i32 n = static_cast<i32>(text.size());
        std::vector<std::pair<i32, i32>> spans;
        for (const CharacterRange& r : format->ranges) {
            i32 first = r.first, len = r.length;
            if (len < 0) { first += len; len = -len; }
            if (first < 0 || first + len > n) { write_status(m, Status::InvalidParameter); return; }
            spans.push_back({first, first + len});
        }
        const usize ptr = m.cpu_is_64bit() ? 8 : 4;
        std::vector<GpRegion*> regions;
        for (usize i = 0; i < spans.size(); ++i) {
            const auto h = m.read_ptr(regions_at + i * ptr);
            GpRegion* r = h.is_ok() ? mgr.objects().get<GpRegion>(h.value()) : nullptr;
            if (r == nullptr) { write_status(m, Status::InvalidParameter); return; }
            regions.push_back(r);
        }
        const auto face = family_face(*font->family_object, font->style);
        TextLayout layout;
        if (!layout_text(text, face, font_em_world(*font, *g), font->style, rect, *format, LayoutPurpose::Ranges, layout)) {
            write_status(m, Status::FontStyleNotFound);
            return;
        }
        const bool clip = (format->flags & StringFormatFlagsNoClip) == 0;
        RectF bounds = rect;
        bounds.Width = std::max(0.0f, bounds.Width);
        bounds.Height = std::max(0.0f, bounds.Height);
        for (usize i = 0; i < spans.size(); ++i) {
            // One rectangle per line the range touches, clipped to the
            // layout rectangle unless NoClip (probe "ranges ...": a zero
            // layout rectangle gives empty regions).
            std::map<float, RectF> per_line;
            for (i32 s = spans[i].first; s < spans[i].second; ++s) {
                const auto& c = layout.cells[static_cast<usize>(s)];
                if (!c.visible) continue;
                auto it = per_line.find(c.top);
                if (it == per_line.end()) {
                    per_line[c.top] = {c.x0, c.top, c.x1 - c.x0, c.bottom - c.top};
                } else {
                    RectF& r = it->second;
                    const float x0 = std::min(r.X, c.x0), x1 = std::max(r.X + r.Width, c.x1);
                    r.X = x0;
                    r.Width = x1 - x0;
                }
            }
            GpRegion result;
            result.kind = GpRegion::Kind::Empty;
            for (auto& [top, r] : per_line) {
                (void)top;
                RectF piece = r;
                if (layout.vertical) {
                    // Cells are in the turned frame: map them to world.
                    const float extent = rect.Width > 0 ? rect.Width : layout.line_height * layout.lines_filled;
                    piece = {rect.X + extent - (r.Y + r.Height), rect.Y + r.X, r.Height, r.Width};
                } else {
                    piece.X += rect.X;
                    piece.Y += rect.Y;
                }
                if (clip) {
                    const float x0 = std::max(piece.X, bounds.X), y0 = std::max(piece.Y, bounds.Y);
                    const float x1 = std::min(piece.X + piece.Width, bounds.X + bounds.Width);
                    const float y1 = std::min(piece.Y + piece.Height, bounds.Y + bounds.Height);
                    if (x1 <= x0 || y1 <= y0) continue;
                    piece = {x0, y0, x1 - x0, y1 - y0};
                }
                if (piece.Width <= 0 || piece.Height <= 0) continue;
                GpRegion leaf;
                leaf.kind = GpRegion::Kind::Rect;
                leaf.rect = piece;
                region_combine(result, leaf, CombineModeUnion);
            }
            *regions[i] = std::move(result);
        }
        write_status(m, Status::Ok);
    };

    // ---- Driver strings ----
    auto driver_args = [&mgr](Marshaler& m, bool draw, DriverArgs& a) -> Status {
        // Measure: (g, text, length, font, positions, flags, matrix, bbox)
        // Draw:    (g, text, length, font, brush, positions, flags, matrix)
        a.g = mgr.objects().get<GpGraphics>(handle_arg(m, 0));
        const GuestAddr text = m.arg_ptr(1);
        const i32 length = static_cast<i32>(m.arg32(2));
        a.font = mgr.objects().get<GpFont>(handle_arg(m, 3));
        const usize base = draw ? 5 : 4;
        const GuestAddr positions = m.arg_ptr(base);
        a.flags = static_cast<i32>(m.arg32(base + 1));
        const u64 matrix = handle_arg(m, base + 2);
        if (a.g == nullptr || text == 0 || a.font == nullptr || positions == 0 || length < -1) {
            return Status::InvalidParameter;
        }
        if (matrix != 0) {
            GpMatrix* mx = mgr.objects().get<GpMatrix>(matrix);
            if (mx == nullptr) return Status::InvalidParameter;
            a.has_matrix = true;
            a.matrix = mx->m;
        }
        i32 count = length;
        if (count == -1) {
            count = 0;
            while (count < (1 << 20)) {
                const auto u = m.read_u16(text + static_cast<u64>(count) * 2);
                if (u.is_err() || u.value() == 0) break;
                ++count;
            }
        }
        for (i32 i = 0; i < count; ++i) {
            const auto u = m.read_u16(text + static_cast<u64>(i) * 2);
            if (u.is_err()) return Status::InvalidParameter;
            a.text.push_back(u.value());
        }
        const i32 npos = (a.flags & DriverStringOptionsRealizedAdvance) ? std::min(count, 1) : count;
        if (!read_points(m, positions, npos, a.positions)) return Status::InvalidParameter;
        a.face = family_face(*a.font->family_object, a.font->style);
        if (!a.face) return Status::FontStyleNotFound;
        return Status::Ok;
    };
    map["GdipMeasureDriverString"] = [&mgr, driver_args](Marshaler& m) {
        const GuestAddr bb = m.arg_ptr(7);
        DriverArgs a;
        Status s = driver_args(m, false, a);
        if (s == Status::Ok && bb == 0) s = Status::InvalidParameter;
        if (s != Status::Ok) { write_status(m, s); return; }
        const float em = font_em_world(*a.font, *a.g);
        const auto glyphs = driver_glyphs(a.text, *a.face, em, a.flags, a.positions);
        const float unit = em / static_cast<float>(a.face->upem);
        const float asc = static_cast<float>(a.face->win_ascent) * unit;
        const float desc = static_cast<float>(a.face->win_descent) * unit;
        const bool vertical = (a.flags & DriverStringOptionsVertical) != 0;
        // The union of each glyph's cell: [x, x + advance] x [y - ascent,
        // y + descent]; vertical: centred on x, [y, y + advance] (probe
        // "driver measure flags=1/3/5/7").
        float x0 = kInf, y0 = kInf, x1 = -kInf, y1 = -kInf;
        for (const DriverGlyph& d : glyphs) {
            RectF cellr = vertical ? RectF{d.pos.X - (asc + desc) / 2, d.pos.Y, asc + desc, d.advance}
                                   : RectF{d.pos.X, d.pos.Y - asc, d.advance, asc + desc};
            PointF corners[4] = {{cellr.X, cellr.Y}, {cellr.X + cellr.Width, cellr.Y},
                                 {cellr.X, cellr.Y + cellr.Height}, {cellr.X + cellr.Width, cellr.Y + cellr.Height}};
            for (PointF c : corners) {
                if (a.has_matrix) {
                    const PointF rel = a.matrix.apply({c.X - d.pos.X, c.Y - d.pos.Y});
                    c = {d.pos.X + rel.X - a.matrix.dx, d.pos.Y + rel.Y - a.matrix.dy};
                }
                x0 = std::min(x0, c.X);
                y0 = std::min(y0, c.Y);
                x1 = std::max(x1, c.X);
                y1 = std::max(y1, c.Y);
            }
        }
        RectF box = glyphs.empty() ? RectF{0, 0, 0, 0} : RectF{x0, y0, x1 - x0, y1 - y0};
        write_rect(m, bb, box);
        write_status(m, Status::Ok);
    };
    map["GdipDrawDriverString"] = [&mgr, driver_args](Marshaler& m) {
        GpBrush* brush = mgr.objects().get<GpBrush>(handle_arg(m, 4));
        DriverArgs a;
        Status s = driver_args(m, true, a);
        if (s == Status::Ok && brush == nullptr) s = Status::InvalidParameter;
        if (s != Status::Ok) { write_status(m, s); return; }
        if (const Status u = graphics_usable(a.g); u != Status::Ok) { write_status(m, u); return; }
        const float em = font_em_world(*a.font, *a.g);
        const auto glyphs = driver_glyphs(a.text, *a.face, em, a.flags, a.positions);
        const bool vertical = (a.flags & DriverStringOptionsVertical) != 0;
        GpPath path;
        path.fill_mode = FillModeWinding;
        Affine shape = a.matrix;
        shape.dx = shape.dy = 0;  // the matrix shapes each glyph about its origin
        for (const DriverGlyph& d : glyphs) {
            append_glyph(path, *a.face, d.glyph, em, d.pos, vertical, a.has_matrix ? &shape : nullptr);
        }
        write_status(m, render_text_path(mgr, m, *a.g, path, *brush, nullptr));
    };

    // ---- AddPathString ----
    auto add_path_string = [&mgr](Marshaler& m, bool integer) {
        GpPath* path = mgr.objects().get<GpPath>(handle_arg(m, 0));
        const GuestAddr str = m.arg_ptr(1);
        const i32 length = static_cast<i32>(m.arg32(2));
        auto family = mgr.objects().share<GpFontFamily>(handle_arg(m, 3));
        const i32 style = static_cast<i32>(m.arg32(4));
        const float em = real_arg(m, 5);
        const GuestAddr rect_at = m.arg_ptr(6);
        const u64 fh = handle_arg(m, 7);
        if (path == nullptr || str == 0 || !family || rect_at == 0 || length < -1) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        // emSize 0 is GenericError (probe "pathstring em0").
        if (em == 0 || !std::isfinite(em)) { write_status(m, Status::GenericError); return; }
        if (em < 0) {
            log_unimplemented("GdipAddPathString", "negative emSize");
            write_status(m, Status::NotImplemented);
            return;
        }
        const GpStringFormat* format = fh != 0 ? mgr.objects().get<GpStringFormat>(fh) : &implicit_default_format();
        if (format == nullptr) { write_status(m, Status::InvalidParameter); return; }
        RectF rect;
        const bool ok = integer ? read_rect_i(m, rect_at, rect) : read_rect(m, rect_at, rect);
        if (!ok) { write_status(m, Status::InvalidParameter); return; }
        std::u16string text;
        (void)read_text(m, str, length, text);
        if (text.empty()) { write_status(m, Status::Ok); return; }
        const auto face = family_face(*family, style);
        TextLayout layout;
        if (!layout_text(text, face, em, style, rect, *format, LayoutPurpose::Path, layout)) {
            write_status(m, Status::FontStyleNotFound);
            return;
        }
        GpPath glyphs = layout_path(layout);
        if (glyphs.points.empty()) { write_status(m, Status::Ok); return; }
        path->points.insert(path->points.end(), glyphs.points.begin(), glyphs.points.end());
        path->types.insert(path->types.end(), glyphs.types.begin(), glyphs.types.end());
        path->new_figure = true;
        write_status(m, Status::Ok);
    };
    map["GdipAddPathString"] = [add_path_string](Marshaler& m) { add_path_string(m, false); };
    map["GdipAddPathStringI"] = [add_path_string](Marshaler& m) { add_path_string(m, true); };
}

}  // namespace vitreo::winapi::gdip
