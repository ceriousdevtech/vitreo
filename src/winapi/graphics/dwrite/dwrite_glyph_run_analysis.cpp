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
void DWriteManager::impl_CreateGlyphRunAnalysis(Marshaler& m) {
    create_glyph_run_analysis(m, /*factory2_form=*/false);
}

void DWriteManager::impl_CreateGlyphRunAnalysis2(Marshaler& m) {
    create_glyph_run_analysis(m, /*factory2_form=*/true);
}

// IDWriteFactory::CreateGlyphRunAnalysis and its IDWriteFactory2 overload
// build the same object from the same glyph run; only the argument list
// differs. Learn (dwrite_2.h): the version 2 form drops pixelsPerDip — the
// caller folds that scale into emSize and the transform — and inserts
// gridFitMode and antialiasMode between measuringMode and the baseline
// origin. Skia calls the version 2 form whenever it can reach IDWriteFactory2,
// so leaving that slot unimplemented pushes every text run down the outline
// fallback instead of the antialiased raster path.
void DWriteManager::create_glyph_run_analysis(Marshaler& m,
                                              bool factory2_form) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr run_address = m.arg_ptr(1);
    const float pixels_per_dip =
        factory2_form ? 1.0f : read_float_argument(m, 2);
    const GuestAddr transform_address = m.arg_ptr(factory2_form ? 2 : 3);
    const u32 rendering_mode = static_cast<u32>(m.arg(factory2_form ? 3 : 4));
    const u32 measuring_mode = static_cast<u32>(m.arg(factory2_form ? 4 : 5));
    // DWRITE_GRID_FIT_MODE: DEFAULT 0, DISABLED 1, ENABLED 2.
    // DWRITE_TEXT_ANTIALIAS_MODE: CLEARTYPE 0, GRAYSCALE 1.
    const u32 grid_fit_mode =
        factory2_form ? static_cast<u32>(m.arg(5)) : 0;
    const u32 antialias_mode =
        factory2_form ? static_cast<u32>(m.arg(6)) : 0;
    const float baseline_x = read_float_argument(m, factory2_form ? 7 : 6);
    const float baseline_y = read_float_argument(m, factory2_form ? 8 : 7);
    const GuestAddr out = m.arg_ptr(factory2_form ? 9 : 8);
    if (factory2_form && (grid_fit_mode > 2 || antialias_mode > 1)) {
        if (out != 0) (void)m.write_ptr(out, 0);
        m.set_return(E_INVALIDARG);
        return;
    }
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (m.write_ptr(out, 0).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory || run_address == 0 ||
        !std::isfinite(pixels_per_dip) || pixels_per_dip <= 0 ||
        !std::isfinite(baseline_x) || !std::isfinite(baseline_y) ||
        rendering_mode < 1 || rendering_mode > 5 || measuring_mode > 2) {
        m.set_return(E_INVALIDARG);
        return;
    }

    const usize pointer_size = m_is_64bit ? 8 : 4;
    const GuestAddr font_face_address =
        read_pointer(run_address).value_or(0);
    const usize em_offset = pointer_size;
    const usize count_offset = em_offset + sizeof(u32);
    const usize indices_offset = count_offset + sizeof(u32);
    const usize advances_offset = indices_offset + pointer_size;
    const usize offsets_offset = advances_offset + pointer_size;
    const usize sideways_offset = offsets_offset + pointer_size;
    const usize bidi_offset = sideways_offset + sizeof(u32);
    const auto em_bits = m.read_u32(run_address + em_offset);
    const auto count_value = m.read_u32(run_address + count_offset);
    const auto indices_value = read_pointer(run_address + indices_offset);
    const auto advances_value = read_pointer(run_address + advances_offset);
    const auto offsets_value = read_pointer(run_address + offsets_offset);
    const auto sideways_value = m.read_u32(run_address + sideways_offset);
    const auto bidi_value = m.read_u32(run_address + bidi_offset);
    const auto face = m_objects.find(font_face_address);
    if (em_bits.is_err() || count_value.is_err() || indices_value.is_err() ||
        advances_value.is_err() || offsets_value.is_err() ||
        sideways_value.is_err() || bidi_value.is_err() ||
        face == m_objects.end() ||
        face->second.type != ObjectType::FontFace) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const float font_em_size = float_from_bits(em_bits.value());
    const usize count = count_value.value();
    if (!std::isfinite(font_em_size) || font_em_size <= 0 ||
        count > 0x100000 ||
        (count != 0 && indices_value.value() == 0)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (sideways_value.value() != 0) {
        m.set_return(E_NOTIMPL);
        return;
    }

    dwrite::GlyphRunRasterParams params{};
    params.font = face->second.font;
    params.font_em_size = font_em_size;
    params.pixels_per_dip = pixels_per_dip;
    params.baseline_origin_x = baseline_x;
    params.baseline_origin_y = baseline_y;
    params.bidi_level = bidi_value.value();
    if (transform_address != 0) {
        for (usize index = 0; index < params.transform.size(); ++index) {
            const auto bits =
                m.read_u32(transform_address + index * sizeof(u32));
            if (bits.is_err()) {
                m.set_return(E_INVALIDARG);
                return;
            }
            params.transform[index] = float_from_bits(bits.value());
            if (!std::isfinite(params.transform[index])) {
                m.set_return(E_INVALIDARG);
                return;
            }
        }
    }
    params.glyph_indices.reserve(count);
    if (advances_value.value() != 0) params.glyph_advances.reserve(count);
    if (offsets_value.value() != 0) params.glyph_offsets.reserve(count);
    for (usize index = 0; index < count; ++index) {
        const auto glyph = m.read_u16(
            indices_value.value() + index * sizeof(u16));
        if (glyph.is_err() || glyph.value() >= face->second.font.glyph_count) {
            m.set_return(E_INVALIDARG);
            return;
        }
        params.glyph_indices.push_back(glyph.value());
        if (advances_value.value() != 0) {
            const auto bits = m.read_u32(
                advances_value.value() + index * sizeof(u32));
            if (bits.is_err()) {
                m.set_return(E_INVALIDARG);
                return;
            }
            const float advance = float_from_bits(bits.value());
            if (!std::isfinite(advance)) {
                m.set_return(E_INVALIDARG);
                return;
            }
            params.glyph_advances.push_back(advance);
        }
        if (offsets_value.value() != 0) {
            const auto advance_bits = m.read_u32(
                offsets_value.value() + index * 2 * sizeof(u32));
            const auto ascender_bits = m.read_u32(
                offsets_value.value() +
                (index * 2 + 1) * sizeof(u32));
            if (advance_bits.is_err() || ascender_bits.is_err()) {
                m.set_return(E_INVALIDARG);
                return;
            }
            const dwrite::GlyphOffset offset{
                float_from_bits(advance_bits.value()),
                float_from_bits(ascender_bits.value()),
            };
            if (!std::isfinite(offset.advance_offset) ||
                !std::isfinite(offset.ascender_offset)) {
                m.set_return(E_INVALIDARG);
                return;
            }
            params.glyph_offsets.push_back(offset);
        }
    }

    const bool antialias = rendering_mode != 1;
    auto raster = dwrite::rasterize_glyph_run(params, antialias);
    if (!raster) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    const GuestAddr analysis = create_object(ObjectType::GlyphRunAnalysis);
    auto object = m_objects.find(analysis);
    if (analysis == 0 || object == m_objects.end()) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    // Which texture type this analysis can answer follows from the bytes per
    // pixel it produces, not from whether it antialiases. ClearType is the
    // only DWRITE_TEXTURE_CLEARTYPE_3x1 case; an aliased rendering mode and
    // grayscale antialiasing alike yield one byte per pixel and are read back
    // as DWRITE_TEXTURE_ALIASED_1x1.
    constexpr u32 ANTIALIAS_MODE_GRAYSCALE = 1;
    const bool cleartype_texture =
        antialias && antialias_mode != ANTIALIAS_MODE_GRAYSCALE;
    if (cleartype_texture) {
        object->second.cleartype_3x1_texture = std::move(*raster);
    } else {
        object->second.aliased_1x1_texture = std::move(*raster);
    }
    if (m.write_ptr(out, analysis).is_err()) {
        m_objects.erase(analysis);
        m.set_return(E_POINTER);
        return;
    }
    static std::atomic<u32> glyph_analysis_diag_count{0};
    if (dwrite_diag_enabled() &&
        glyph_analysis_diag_count.fetch_add(1, std::memory_order_relaxed) <
            256) {
        const auto& stored = cleartype_texture
            ? object->second.cleartype_3x1_texture
            : object->second.aliased_1x1_texture;
        const std::string family = utf16_to_utf8(
            reinterpret_cast<const u16*>(
                face->second.font.family_name.data()),
            static_cast<int>(face->second.font.family_name.size()));
        const std::string postscript = utf16_to_utf8(
            reinterpret_cast<const u16*>(
                face->second.font.postscript_name.data()),
            static_cast<int>(face->second.font.postscript_name.size()));
        const u16 first_glyph = params.glyph_indices.empty()
            ? 0 : params.glyph_indices.front();
        const float first_advance = params.glyph_advances.empty()
            ? std::numeric_limits<float>::quiet_NaN()
            : params.glyph_advances.front();
        std::fprintf(
            stderr,
            "[dwrite-diag] CreateGlyphRunAnalysis family='%s' ps='%s' "
            "glyphs=%zu first=%u em=%g ppd=%g advance=%g "
            "baseline=(%g,%g) mode=%u measure=%u gridfit=%u aa=%u "
            "bounds=(%d,%d)-(%d,%d) "
            "-> 0x%llx\n",
            family.c_str(), postscript.c_str(), count, first_glyph,
            font_em_size, pixels_per_dip, first_advance, baseline_x,
            baseline_y, rendering_mode, measuring_mode, grid_fit_mode,
            antialias_mode, stored.left, stored.top,
            stored.right, stored.bottom,
            static_cast<unsigned long long>(analysis));
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_AnalysisGetAlphaTextureBounds(Marshaler& m) {
    const auto object = m_objects.find(m.arg_ptr(0));
    const u32 texture_type = static_cast<u32>(m.arg(1));
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (object == m_objects.end() ||
        object->second.type != ObjectType::GlyphRunAnalysis ||
        texture_type > 1) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto& raster = texture_type == 0
        ? object->second.aliased_1x1_texture
        : object->second.cleartype_3x1_texture;
    const std::array<i32, 4> bounds{
        raster.left, raster.top, raster.right, raster.bottom};
    for (usize index = 0; index < bounds.size(); ++index) {
        if (m.write_u32(
                out + index * sizeof(u32),
                static_cast<u32>(bounds[index])).is_err()) {
            m.set_return(E_POINTER);
            return;
        }
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_AnalysisCreateAlphaTexture(Marshaler& m) {
    const auto object = m_objects.find(m.arg_ptr(0));
    const u32 texture_type = static_cast<u32>(m.arg(1));
    const GuestAddr bounds_address = m.arg_ptr(2);
    const GuestAddr output = m.arg_ptr(3);
    const usize buffer_size = static_cast<usize>(m.arg(4));
    if (bounds_address == 0 || (buffer_size != 0 && output == 0)) {
        m.set_return(E_POINTER);
        return;
    }
    if (object == m_objects.end() ||
        object->second.type != ObjectType::GlyphRunAnalysis ||
        texture_type > 1) {
        m.set_return(E_INVALIDARG);
        return;
    }
    std::array<i32, 4> bounds{};
    for (usize index = 0; index < bounds.size(); ++index) {
        const auto value =
            m.read_u32(bounds_address + index * sizeof(u32));
        if (value.is_err()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        bounds[index] = static_cast<i32>(value.value());
    }
    const i64 width = static_cast<i64>(bounds[2]) - bounds[0];
    const i64 height = static_cast<i64>(bounds[3]) - bounds[1];
    const usize channels = texture_type == 0 ? 1 : 3;
    if (width < 0 || height < 0 || width > 16384 || height > 16384 ||
        static_cast<u64>(width) * static_cast<u64>(height) >
            std::numeric_limits<usize>::max() / channels) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const usize required = static_cast<usize>(width * height) * channels;
    if (buffer_size < required) {
        m.set_return(E_NOT_SUFFICIENT_BUFFER);
        return;
    }
    const auto& raster = texture_type == 0
        ? object->second.aliased_1x1_texture
        : object->second.cleartype_3x1_texture;
    const i64 raster_width =
        static_cast<i64>(raster.right) - raster.left;
    const i64 raster_height =
        static_cast<i64>(raster.bottom) - raster.top;
    for (i64 y = 0; y < height; ++y) {
        for (i64 x = 0; x < width; ++x) {
            const i64 source_x = bounds[0] + x - raster.left;
            const i64 source_y = bounds[1] + y - raster.top;
            u8 alpha = 0;
            if (source_x >= 0 && source_y >= 0 &&
                source_x < raster_width && source_y < raster_height &&
                !raster.alpha.empty()) {
                alpha = raster.alpha[static_cast<usize>(
                    source_y * raster_width + source_x)];
            }
            const usize destination =
                static_cast<usize>(y * width + x) * channels;
            for (usize channel = 0; channel < channels; ++channel) {
                if (m.write_u8(output + destination + channel, alpha).is_err()) {
                    m.set_return(E_POINTER);
                    return;
                }
            }
        }
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_AnalysisGetAlphaBlendParams(Marshaler& m) {
    const auto analysis = m_objects.find(m.arg_ptr(0));
    const auto params = m_objects.find(m.arg_ptr(1));
    const GuestAddr gamma_out = m.arg_ptr(2);
    const GuestAddr contrast_out = m.arg_ptr(3);
    const GuestAddr clear_type_out = m.arg_ptr(4);
    if (gamma_out == 0 || contrast_out == 0 || clear_type_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (analysis == m_objects.end() ||
        analysis->second.type != ObjectType::GlyphRunAnalysis ||
        params == m_objects.end() ||
        params->second.type != ObjectType::RenderingParams) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto write_float = [&](GuestAddr address, float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return m.write_u32(address, bits).is_ok();
    };
    if (!write_float(gamma_out, params->second.gamma) ||
        !write_float(contrast_out, params->second.enhanced_contrast) ||
        !write_float(clear_type_out, params->second.clear_type_level)) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

} // namespace vitreo::winapi
