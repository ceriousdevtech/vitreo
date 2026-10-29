// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/graphics/dwrite/dwrite_internal.hpp"

#include "capsule/vfs/vfs.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/graphics/dwrite/font_backend.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace vitreo::winapi {
namespace {

u32 write_face_glyph_metrics(
    Marshaler& m,
    const dwrite::MatchedFont& font,
    GuestAddr glyphs_address,
    usize count,
    GuestAddr metrics_address) {
    if ((count != 0 && (glyphs_address == 0 || metrics_address == 0)) ||
        count > 0x100000) {
        return E_INVALIDARG;
    }

    std::vector<u16> glyphs;
    glyphs.reserve(count);
    for (usize index = 0; index < count; ++index) {
        const auto glyph =
            m.read_u16(glyphs_address + index * sizeof(u16));
        if (glyph.is_err() || glyph.value() >= font.glyph_count) {
            return E_INVALIDARG;
        }
        glyphs.push_back(glyph.value());
    }

    auto metrics = dwrite::glyph_metrics(font, glyphs);
    if (!metrics) {
        return DWRITE_E_FILEFORMAT;
    }
    constexpr usize METRIC_SIZE = 7 * sizeof(u32);
    for (usize index = 0; index < metrics->size(); ++index) {
        const auto& metric = (*metrics)[index];
        const GuestAddr out = metrics_address + index * METRIC_SIZE;
        const std::array<u32, 7> fields{
            static_cast<u32>(metric.left_side_bearing),
            metric.advance_width,
            static_cast<u32>(metric.right_side_bearing),
            static_cast<u32>(metric.top_side_bearing),
            metric.advance_height,
            static_cast<u32>(metric.bottom_side_bearing),
            static_cast<u32>(metric.vertical_origin_y),
        };
        for (usize field = 0; field < fields.size(); ++field) {
            if (m.write_u32(out + field * sizeof(u32), fields[field]).is_err()) {
                return E_POINTER;
            }
        }
    }
    static std::atomic<u32> glyph_metric_diag_count{0};
    if (dwrite_diag_enabled() && !metrics->empty() &&
        glyph_metric_diag_count.fetch_add(1, std::memory_order_relaxed) <
            256) {
        const std::string family = utf16_to_utf8(
            reinterpret_cast<const u16*>(font.family_name.data()),
            static_cast<int>(font.family_name.size()));
        const std::string postscript = utf16_to_utf8(
            reinterpret_cast<const u16*>(font.postscript_name.data()),
            static_cast<int>(font.postscript_name.size()));
        const auto& metric = metrics->front();
        std::fprintf(
            stderr,
            "[dwrite-diag] glyph metrics family='%s' ps='%s' "
            "units=%u count=%zu first=%u advance=%u lsb=%d rsb=%d\n",
            family.c_str(), postscript.c_str(),
            font.metrics.design_units_per_em, count,
            glyphs.front(), metric.advance_width,
            metric.left_side_bearing, metric.right_side_bearing);
    }
    return S_OK;
}

} // namespace

void DWriteManager::impl_FaceGetDesignGlyphMetrics(Marshaler& m) {
    const auto face = m_objects.find(m.arg_ptr(0));
    if (face == m_objects.end() ||
        face->second.type != ObjectType::FontFace) {
        m.set_return(E_INVALIDARG);
        return;
    }
    m.set_return(write_face_glyph_metrics(
        m, face->second.font, m.arg_ptr(1),
        static_cast<usize>(m.arg(2)), m.arg_ptr(3)));
}

void DWriteManager::impl_FaceGetGdiCompatibleGlyphMetrics(Marshaler& m) {
    const auto face = m_objects.find(m.arg_ptr(0));
    const float em_size = read_float_argument(m, 1);
    const float pixels_per_dip = read_float_argument(m, 2);
    const GuestAddr transform_address = m.arg_ptr(3);
    const GuestAddr glyphs_address = m.arg_ptr(5);
    const usize count = static_cast<usize>(m.arg(6));
    const GuestAddr metrics_address = m.arg_ptr(7);
    const u32 is_sideways = static_cast<u32>(m.arg(8));
    if (face == m_objects.end() ||
        face->second.type != ObjectType::FontFace ||
        !std::isfinite(em_size) || em_size <= 0 ||
        !std::isfinite(pixels_per_dip) || pixels_per_dip <= 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (is_sideways != 0) {
        m.set_return(E_NOTIMPL);
        return;
    }
    if (transform_address != 0) {
        for (usize index = 0; index < 6; ++index) {
            const auto bits =
                m.read_u32(transform_address + index * sizeof(u32));
            if (bits.is_err() ||
                !std::isfinite(float_from_bits(bits.value()))) {
                m.set_return(E_INVALIDARG);
                return;
            }
        }
    }
    m.set_return(write_face_glyph_metrics(
        m, face->second.font, glyphs_address, count, metrics_address));
}

void DWriteManager::impl_FaceGetGlyphIndices(Marshaler& m) {
    const auto face = m_objects.find(m.arg_ptr(0));
    const GuestAddr codepoints_address = m.arg_ptr(1);
    const usize count = static_cast<usize>(m.arg(2));
    const GuestAddr glyphs_address = m.arg_ptr(3);
    if (face == m_objects.end() ||
        face->second.type != ObjectType::FontFace ||
        (count != 0 && (codepoints_address == 0 || glyphs_address == 0)) ||
        count > 0x100000) {
        m.set_return(E_INVALIDARG);
        return;
    }

    std::vector<u32> codepoints;
    codepoints.reserve(count);
    for (usize index = 0; index < count; ++index) {
        const auto codepoint =
            m.read_u32(codepoints_address + index * sizeof(u32));
        if (codepoint.is_err()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        codepoints.push_back(codepoint.value());
    }
    auto glyphs = dwrite::glyph_indices(face->second.font, codepoints);
    if (!glyphs) {
        m.set_return(DWRITE_E_FILEFORMAT);
        return;
    }
    usize mapped = 0;
    for (usize index = 0; index < glyphs->size(); ++index) {
        if (m.write_u16(
                glyphs_address + index * sizeof(u16),
                (*glyphs)[index]).is_err()) {
            m.set_return(E_POINTER);
            return;
        }
        if ((*glyphs)[index] != 0) ++mapped;
    }
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
            "[dwrite-diag] GetGlyphIndices face=0x%llx count=%zu mapped=%zu\n",
            static_cast<unsigned long long>(m.arg_ptr(0)), count, mapped);
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_FaceGetMetrics1(Marshaler& m) {
    // DWRITE_FONT_METRICS1 = DWRITE_FONT_METRICS + glyph box / sub/super + BOOL.
    impl_FaceGetMetrics(m);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return;
    constexpr usize BASE_SIZE = 10 * sizeof(u16);
    for (usize i = 0; i < 12; ++i) {
        (void)m.write_u16(out + BASE_SIZE + i * sizeof(u16), 0);
    }
    (void)m.write_u32(out + BASE_SIZE + 12 * sizeof(u16), 0); // hasTypographicMetrics
}

void DWriteManager::impl_FaceGetCaretMetrics(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return;
    // Vertical caret: slopeRise=1, slopeRun=0, offset=0.
    (void)m.write_u16(out, 1);
    (void)m.write_u16(out + sizeof(u16), 0);
    (void)m.write_u16(out + 2 * sizeof(u16), 0);
}

void DWriteManager::impl_FaceGetUnicodeRanges(Marshaler& m) {
    const GuestAddr actual_out = m.arg_ptr(3);
    if (actual_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (m.write_u32(actual_out, 0).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_FaceGetDesignGlyphAdvances(Marshaler& m) {
    const auto face = m_objects.find(m.arg_ptr(0));
    const usize count = static_cast<usize>(m.arg(1));
    const GuestAddr glyphs_address = m.arg_ptr(2);
    const GuestAddr advances_address = m.arg_ptr(3);
    const u32 is_sideways = static_cast<u32>(m.arg(4));
    if (face == m_objects.end() ||
        face->second.type != ObjectType::FontFace ||
        (count != 0 && (glyphs_address == 0 || advances_address == 0)) ||
        count > 0x100000) {
        m.set_return(E_INVALIDARG);
        return;
    }
    std::vector<u16> glyphs;
    glyphs.reserve(count);
    for (usize index = 0; index < count; ++index) {
        const auto glyph =
            m.read_u16(glyphs_address + index * sizeof(u16));
        if (glyph.is_err() || glyph.value() >= face->second.font.glyph_count) {
            m.set_return(E_INVALIDARG);
            return;
        }
        glyphs.push_back(glyph.value());
    }
    auto metrics = dwrite::glyph_metrics(face->second.font, glyphs);
    if (!metrics) {
        m.set_return(DWRITE_E_FILEFORMAT);
        return;
    }
    for (usize index = 0; index < metrics->size(); ++index) {
        const i32 advance = is_sideways != 0
            ? static_cast<i32>((*metrics)[index].advance_height)
            : static_cast<i32>((*metrics)[index].advance_width);
        if (m.write_u32(
                advances_address + index * sizeof(u32),
                static_cast<u32>(advance)).is_err()) {
            m.set_return(E_POINTER);
            return;
        }
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_FaceGetKerningPairAdjustments(Marshaler& m) {
    const usize count = static_cast<usize>(m.arg(1));
    const GuestAddr adjustments = m.arg_ptr(3);
    if (count != 0 && adjustments == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    for (usize index = 0; index < count; ++index) {
        if (m.write_u32(adjustments + index * sizeof(u32), 0).is_err()) {
            m.set_return(E_POINTER);
            return;
        }
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_FaceGetRecommendedRenderingMode(Marshaler& m) {
    // Prefer NATURAL_SYMMETRIC — matches the common Skia/DirectWrite UI choice
    // for ClearType-quality grayscale/LCD masks.
    constexpr u32 DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC = 5;
    const GuestAddr out = m.arg_ptr(5);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (m.write_u32(out, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_FaceGetRecommendedRenderingMode1(Marshaler& m) {
    constexpr u32 DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC = 5;
    const GuestAddr out = m.arg_ptr(8);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (m.write_u32(out, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_FaceGetRecommendedRenderingMode2(Marshaler& m) {
    constexpr u32 DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC = 5;
    constexpr u32 DWRITE_GRID_FIT_MODE_DEFAULT = 0;
    const GuestAddr mode_out = m.arg_ptr(9);
    const GuestAddr grid_out = m.arg_ptr(10);
    if (mode_out == 0 || grid_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (m.write_u32(mode_out, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC)
            .is_err() ||
        m.write_u32(grid_out, DWRITE_GRID_FIT_MODE_DEFAULT).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

} // namespace vitreo::winapi
