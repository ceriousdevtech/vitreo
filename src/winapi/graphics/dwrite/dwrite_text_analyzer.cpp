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

// Learn IDWriteTextAnalyzer::GetGlyphs: clusterMap[i] is the first glyph
// produced for textString[i]. Surrogate pairs share one glyph slot.
struct AnalyzerGlyphRun {
    std::vector<u32> codepoints;
    std::vector<u16> cluster_map;
};

AnalyzerGlyphRun analyzer_glyphs_from_utf16(const std::u16string& text) {
    AnalyzerGlyphRun run;
    run.cluster_map.resize(text.size());
    for (usize index = 0; index < text.size();) {
        const u16 cluster = static_cast<u16>(run.codepoints.size());
        const char16_t lead = text[index];
        if (lead >= 0xd800 && lead <= 0xdbff && index + 1 < text.size()) {
            const char16_t trail = text[index + 1];
            if (trail >= 0xdc00 && trail <= 0xdfff) {
                run.codepoints.push_back(
                    0x10000u + ((static_cast<u32>(lead) - 0xd800u) << 10) +
                    (static_cast<u32>(trail) - 0xdc00u));
                run.cluster_map[index] = cluster;
                run.cluster_map[index + 1] = cluster;
                index += 2;
                continue;
            }
        }
        run.codepoints.push_back(static_cast<u32>(lead));
        run.cluster_map[index] = cluster;
        ++index;
    }
    return run;
}

// Learn DWRITE_SHAPING_GLYPH_PROPERTIES: justification:4, isClusterStart:1.
constexpr u16 kShapingGlyphClusterStart = 0x10;

u32 write_analyzer_glyph_placements(
    Marshaler& m,
    const dwrite::MatchedFont& font,
    GuestAddr glyphs_address,
    usize glyph_count,
    float em_size,
    bool is_sideways,
    GuestAddr advances_address,
    GuestAddr offsets_address) {
    if (!std::isfinite(em_size) || em_size <= 0.0f ||
        font.metrics.design_units_per_em == 0 ||
        (glyph_count != 0 &&
         (glyphs_address == 0 || advances_address == 0 ||
          offsets_address == 0)) ||
        glyph_count > 0x100000) {
        return E_INVALIDARG;
    }

    std::vector<u16> glyphs;
    glyphs.reserve(glyph_count);
    for (usize index = 0; index < glyph_count; ++index) {
        const auto glyph =
            m.read_u16(glyphs_address + index * sizeof(u16));
        if (glyph.is_err() || glyph.value() >= font.glyph_count) {
            return E_INVALIDARG;
        }
        glyphs.push_back(glyph.value());
    }

    auto metrics = dwrite::glyph_metrics(font, glyphs);
    if (!metrics) return DWRITE_E_FILEFORMAT;

    const float scale =
        em_size / static_cast<float>(font.metrics.design_units_per_em);
    for (usize index = 0; index < metrics->size(); ++index) {
        const float advance =
            scale * static_cast<float>(
                        is_sideways ? (*metrics)[index].advance_height
                                    : (*metrics)[index].advance_width);
        u32 advance_bits = 0;
        std::memcpy(&advance_bits, &advance, sizeof(advance_bits));
        if (m.write_u32(
                advances_address + index * sizeof(u32), advance_bits)
                .is_err() ||
            m.write_u32(
                offsets_address + index * (2 * sizeof(u32)), 0)
                .is_err() ||
            m.write_u32(
                offsets_address + index * (2 * sizeof(u32)) + sizeof(u32),
                0)
                .is_err()) {
            return E_POINTER;
        }
    }
    return S_OK;
}

} // namespace

void DWriteManager::impl_CreateTextAnalyzer(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr analyzer_out = m.arg_ptr(1);
    if (analyzer_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(analyzer_out, 0);
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr analyzer = create_object(ObjectType::TextAnalyzer);
    if (analyzer == 0 || m.write_ptr(analyzer_out, analyzer).is_err()) {
        if (analyzer != 0) m_objects.erase(analyzer);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] CreateTextAnalyzer factory=0x%llx "
                     "-> S_OK analyzer=0x%llx\n",
                     static_cast<unsigned long long>(self),
                     static_cast<unsigned long long>(analyzer));
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_AnalyzerAnalyzeScript(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr source = m.arg_ptr(1);
    const u32 text_position = static_cast<u32>(m.arg(2));
    const u32 text_length = static_cast<u32>(m.arg(3));
    const GuestAddr sink = m.arg_ptr(4);
    const auto analyzer = m_objects.find(self);
    if (analyzer == m_objects.end() ||
        analyzer->second.type != ObjectType::TextAnalyzer || source == 0 ||
        sink == 0 || !m_guest_call) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (text_length == 0) {
        m.set_return(S_OK);
        return;
    }
    if (!ensure_storage()) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    // DWRITE_SCRIPT_ANALYSIS: UINT16 script + UINT32 shapes (8 bytes with pad).
    // script=1 is the Latin script id used by DirectWrite for common Western text.
    alignas(8) u8 analysis_bytes[8]{};
    analysis_bytes[0] = 1;
    analysis_bytes[1] = 0;
    if (m_memory.write_bytes(
            m_callback_scratch,
            reinterpret_cast<const Byte*>(analysis_bytes),
            sizeof(analysis_bytes)).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    const u64 args[]{
        text_position, text_length,
        static_cast<u64>(m_callback_scratch),
    };
    const auto result = call_guest_method(sink, 3, args, std::size(args));
    if (!result) {
        m.set_return(E_FAIL);
        return;
    }
    m.set_return(static_cast<u32>(*result));
}

void DWriteManager::impl_AnalyzerAnalyzeBidi(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr source = m.arg_ptr(1);
    const u32 text_position = static_cast<u32>(m.arg(2));
    const u32 text_length = static_cast<u32>(m.arg(3));
    const GuestAddr sink = m.arg_ptr(4);
    const auto analyzer = m_objects.find(self);
    if (analyzer == m_objects.end() ||
        analyzer->second.type != ObjectType::TextAnalyzer || source == 0 ||
        sink == 0 || !m_guest_call) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (text_length == 0) {
        m.set_return(S_OK);
        return;
    }
    // SetBidiLevel(textPosition, textLength, explicitLevel, resolvedLevel)
    const u64 args[]{text_position, text_length, 0u, 0u};
    const auto result = call_guest_method(sink, 5, args, std::size(args));
    if (!result) {
        m.set_return(E_FAIL);
        return;
    }
    m.set_return(static_cast<u32>(*result));
}

void DWriteManager::impl_AnalyzerAnalyzeNumberSubstitution(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr source = m.arg_ptr(1);
    const u32 text_position = static_cast<u32>(m.arg(2));
    const u32 text_length = static_cast<u32>(m.arg(3));
    const GuestAddr sink = m.arg_ptr(4);
    (void)text_position;
    (void)text_length;
    const auto analyzer = m_objects.find(self);
    if (analyzer == m_objects.end() ||
        analyzer->second.type != ObjectType::TextAnalyzer || source == 0 ||
        sink == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    // No number substitution ranges for the analyzed span.
    m.set_return(S_OK);
}

void DWriteManager::impl_AnalyzerAnalyzeLineBreakpoints(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr source = m.arg_ptr(1);
    const u32 text_position = static_cast<u32>(m.arg(2));
    const u32 text_length = static_cast<u32>(m.arg(3));
    const GuestAddr sink = m.arg_ptr(4);
    const auto analyzer = m_objects.find(self);
    if (analyzer == m_objects.end() ||
        analyzer->second.type != ObjectType::TextAnalyzer || source == 0 ||
        sink == 0 || !m_guest_call) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (text_length == 0) {
        m.set_return(S_OK);
        return;
    }
    if (text_length > MAX_FALLBACK_TEXT_UNITS) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const usize mapping_size =
        (static_cast<usize>(text_length) + GUEST_PAGE_SIZE - 1) &
        ~(GUEST_PAGE_SIZE - 1);
    const GuestAddr breakpoints =
        m_memory.find_free_gap(0x6B000000ull, mapping_size);
    if (breakpoints == 0 ||
        m_memory.map_region(
            breakpoints, mapping_size, MemoryProtection::ReadWrite).is_err()) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    struct Scratch {
        GuestMemory& memory;
        GuestAddr address;
        usize size;
        ~Scratch() { (void)memory.unmap_region(address, size); }
    } scratch{m_memory, breakpoints, mapping_size};

    // Fetch text via IDWriteTextAnalysisSource::GetTextAtPosition (slot 3)
    // when possible so whitespace gets honest soft-break opportunities.
    std::vector<u8> breakpoint_bytes(text_length, 0);
    // Prefer reading source text into a temporary host buffer via callback.
    // GetTextAtPosition writes WCHAR const** and UINT32* length.
    const GuestAddr text_string_out = m_callback_scratch;
    const GuestAddr text_length_out = m_callback_scratch + 8;
    if (ensure_storage() &&
        write_pointer(text_string_out, 0) &&
        m_memory.write_u32(text_length_out, 0).is_ok()) {
        const u64 get_args[]{text_position, text_string_out, text_length_out};
        const auto get_result =
            call_guest_method(source, 3, get_args, std::size(get_args));
        if (get_result && (static_cast<u32>(*get_result) & 0x80000000u) == 0) {
            const auto text_ptr = read_pointer(text_string_out);
            const auto avail = m_memory.read_u32(text_length_out);
            if (text_ptr.is_ok() && text_ptr.value() != 0 && avail.is_ok()) {
                const u32 copy_units =
                    std::min(text_length, avail.value());
                for (u32 i = 0; i < copy_units; ++i) {
                    const auto ch = m_memory.read_u16(text_ptr.value() + i * 2);
                    if (ch.is_err()) break;
                    const u16 c = ch.value();
                    const bool whitespace =
                        c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D ||
                        c == 0xA0;
                    // breakConditionAfter = CAN_BREAK (1) for whitespace.
                    u8 value = 0;
                    if (whitespace) {
                        value = static_cast<u8>((1u << 2) | (1u << 4));
                    }
                    breakpoint_bytes[i] = value;
                }
            }
        }
    }
    if (m_memory.write_bytes(
            breakpoints, breakpoint_bytes.data(),
            breakpoint_bytes.size()).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    const u64 args[]{text_position, text_length, static_cast<u64>(breakpoints)};
    const auto result = call_guest_method(sink, 4, args, std::size(args));
    if (!result) {
        m.set_return(E_FAIL);
        return;
    }
    m.set_return(static_cast<u32>(*result));
}

void DWriteManager::impl_AnalyzerGetGlyphs(Marshaler& m) {
    const GuestAddr text_address = m.arg_ptr(1);
    const usize text_length = static_cast<usize>(m.arg(2));
    const GuestAddr font_face = m.arg_ptr(3);
    const u32 max_glyph_count = static_cast<u32>(m.arg(12));
    const GuestAddr cluster_map = m.arg_ptr(13);
    const GuestAddr text_props = m.arg_ptr(14);
    const GuestAddr glyph_indices_out = m.arg_ptr(15);
    const GuestAddr glyph_props = m.arg_ptr(16);
    const GuestAddr actual_count_out = m.arg_ptr(17);

    if (actual_count_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (text_length > 0x10000 || max_glyph_count > 0x100000) {
        m.set_return(E_INVALIDARG);
        return;
    }

    const auto face = m_objects.find(font_face);
    if (face == m_objects.end() ||
        face->second.type != ObjectType::FontFace ||
        (text_length != 0 && text_address == 0)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (text_length != 0 &&
        (cluster_map == 0 || text_props == 0 || glyph_indices_out == 0 ||
         glyph_props == 0)) {
        m.set_return(E_POINTER);
        return;
    }

    auto text = read_utf16(text_address, text_length);
    if (text.is_err()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const AnalyzerGlyphRun run = analyzer_glyphs_from_utf16(text.value());
    if (run.codepoints.size() > max_glyph_count) {
        // Learn: HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER); actualGlyphCount
        // is valid only on success.
        m.set_return(E_NOT_SUFFICIENT_BUFFER);
        return;
    }

    auto glyphs = dwrite::glyph_indices(face->second.font, run.codepoints);
    if (!glyphs || glyphs->size() != run.codepoints.size()) {
        m.set_return(DWRITE_E_FILEFORMAT);
        return;
    }

    for (usize index = 0; index < run.cluster_map.size(); ++index) {
        if (m.write_u16(
                cluster_map + index * sizeof(u16),
                run.cluster_map[index])
                .is_err() ||
            m.write_u16(text_props + index * sizeof(u16), 0).is_err()) {
            m.set_return(E_POINTER);
            return;
        }
    }
    usize mapped = 0;
    for (usize index = 0; index < glyphs->size(); ++index) {
        if (m.write_u16(
                glyph_indices_out + index * sizeof(u16), (*glyphs)[index])
                .is_err() ||
            m.write_u16(
                glyph_props + index * sizeof(u16), kShapingGlyphClusterStart)
                .is_err()) {
            m.set_return(E_POINTER);
            return;
        }
        if ((*glyphs)[index] != 0) ++mapped;
    }
    if (m.write_u32(actual_count_out, static_cast<u32>(glyphs->size()))
            .is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
            "[dwrite-diag] TextAnalyzer::GetGlyphs face=0x%llx chars=%zu "
            "glyphs=%zu mapped=%zu -> S_OK\n",
            static_cast<unsigned long long>(font_face), text_length,
            glyphs->size(), mapped);
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_AnalyzerGetGlyphPlacements(Marshaler& m) {
    const GuestAddr font_face = m.arg_ptr(8);
    const float em_size = read_float_argument(m, 9);
    const u32 is_sideways = static_cast<u32>(m.arg(10));
    const usize glyph_count = static_cast<usize>(m.arg(7));
    const GuestAddr glyphs_address = m.arg_ptr(5);
    const GuestAddr advances_address = m.arg_ptr(17);
    const GuestAddr offsets_address = m.arg_ptr(18);

    const auto face = m_objects.find(font_face);
    if (face == m_objects.end() ||
        face->second.type != ObjectType::FontFace) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const u32 hr = write_analyzer_glyph_placements(
        m, face->second.font, glyphs_address, glyph_count, em_size,
        is_sideways != 0, advances_address, offsets_address);
    if (dwrite_diag_enabled() && hr == S_OK) {
        dwrite_diag_printf(
            "[dwrite-diag] TextAnalyzer::GetGlyphPlacements face=0x%llx "
            "glyphs=%zu em=%g -> S_OK\n",
            static_cast<unsigned long long>(font_face), glyph_count,
            static_cast<double>(em_size));
    }
    m.set_return(hr);
}

void DWriteManager::impl_AnalyzerGetGdiCompatibleGlyphPlacements(Marshaler& m) {
    (void)m.arg_ptr(0);
    if (dwrite_diag_enabled()) {
        std::fprintf(
            stderr,
            "[dwrite-diag] TextAnalyzer::GetGdiCompatibleGlyphPlacements "
            "-> E_NOTIMPL\n");
    }
    m.set_return(E_NOTIMPL);
}

} // namespace vitreo::winapi
