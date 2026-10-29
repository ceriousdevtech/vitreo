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
void DWriteManager::impl_FaceTryGetFontTable(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const u32 open_type_tag = static_cast<u32>(m.arg(1));
    const GuestAddr table_data_out = m.arg_ptr(2);
    const GuestAddr table_size_out = m.arg_ptr(3);
    const GuestAddr table_context_out = m.arg_ptr(4);
    const GuestAddr exists_out = m.arg_ptr(5);
    if (table_data_out == 0 || table_size_out == 0 ||
        table_context_out == 0 || exists_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (m.write_ptr(table_data_out, 0).is_err() ||
        m.write_u32(table_size_out, 0).is_err() ||
        m.write_ptr(table_context_out, 0).is_err() ||
        m.write_u32(exists_out, 0).is_err()) {
        m.set_return(E_POINTER);
        return;
    }

    const auto face = m_objects.find(self);
    if (face == m_objects.end() ||
        face->second.type != ObjectType::FontFace) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto bytes = dwrite::font_table(face->second.font, open_type_tag);
    if (!bytes) {
        if (dwrite_diag_enabled()) {
            dwrite_diag_printf(
                         "[dwrite-diag] TryGetFontTable face=0x%llx "
                         "tag=0x%08x -> DWRITE_E_FILEFORMAT\n",
                         static_cast<unsigned long long>(self),
                         open_type_tag);
        }
        m.set_return(DWRITE_E_FILEFORMAT);
        return;
    }
    if (bytes->empty()) {
        if (dwrite_diag_enabled()) {
            dwrite_diag_printf(
                         "[dwrite-diag] TryGetFontTable face=0x%llx "
                         "tag=0x%08x -> S_OK exists=FALSE\n",
                         static_cast<unsigned long long>(self),
                         open_type_tag);
        }
        m.set_return(S_OK);
        return;
    }
    if (bytes->size() > std::numeric_limits<u32>::max()) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }

    const usize mapped_size = align_up(bytes->size(), GUEST_PAGE_SIZE);
    const GuestAddr table_address =
        m_memory.find_free_gap(0x68000000ull, mapped_size);
    if (table_address == 0 ||
        m_memory.map_region(
            table_address, mapped_size,
            MemoryProtection::ReadWrite).is_err()) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    if (m_memory.write_bytes(
            table_address,
            reinterpret_cast<const Byte*>(bytes->data()),
            bytes->size()).is_err() ||
        m_memory.protect_region(
            table_address, mapped_size,
            MemoryProtection::Read).is_err()) {
        (void)m_memory.unmap_region(table_address, mapped_size);
        m.set_return(E_OUTOFMEMORY);
        return;
    }

    m_font_tables.emplace(
        table_address, FontTableMapping{self, mapped_size});
    if (m.write_ptr(table_data_out, table_address).is_err() ||
        m.write_u32(
            table_size_out, static_cast<u32>(bytes->size())).is_err() ||
        m.write_ptr(table_context_out, table_address).is_err() ||
        m.write_u32(exists_out, 1).is_err()) {
        m_font_tables.erase(table_address);
        (void)m_memory.unmap_region(table_address, mapped_size);
        m.set_return(E_POINTER);
        return;
    }
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] TryGetFontTable face=0x%llx tag=0x%08x "
                     "-> S_OK exists=TRUE data=0x%llx size=%zu "
                     "context=0x%llx\n",
                     static_cast<unsigned long long>(self), open_type_tag,
                     static_cast<unsigned long long>(table_address),
                     bytes->size(),
                     static_cast<unsigned long long>(table_address));
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_FaceReleaseFontTable(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr context = m.arg_ptr(1);
    const auto table = m_font_tables.find(context);
    if (table != m_font_tables.end() && table->second.face == self) {
        if (dwrite_diag_enabled()) {
            dwrite_diag_printf(
                         "[dwrite-diag] ReleaseFontTable face=0x%llx "
                         "context=0x%llx size=%zu\n",
                         static_cast<unsigned long long>(self),
                         static_cast<unsigned long long>(context),
                         table->second.mapped_size);
        }
        (void)m_memory.unmap_region(context, table->second.mapped_size);
        m_font_tables.erase(table);
    }
    // IDWriteFontFace::ReleaseFontTable returns void.
    m.set_return(0);
}

void DWriteManager::impl_FaceGetGlyphRunOutline(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const float em_size = read_float_argument(m, 1);
    const GuestAddr indices_address = m.arg_ptr(2);
    const GuestAddr advances_address = m.arg_ptr(3);
    const GuestAddr offsets_address = m.arg_ptr(4);
    const u32 glyph_count = static_cast<u32>(m.arg(5));
    const u32 is_sideways = static_cast<u32>(m.arg(6));
    const u32 is_right_to_left = static_cast<u32>(m.arg(7));
    const GuestAddr sink = m.arg_ptr(8);

    if (sink == 0) {
        m.set_return(E_POINTER);
        return;
    }
    const auto face = m_objects.find(self);
    if (face == m_objects.end() ||
        face->second.type != ObjectType::FontFace ||
        !std::isfinite(em_size) || em_size <= 0 ||
        glyph_count > 4096 ||
        (glyph_count != 0 && indices_address == 0) ||
        (is_sideways != 0 && is_right_to_left != 0)) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (is_sideways != 0) {
        m.set_return(E_NOTIMPL);
        return;
    }
    if (!m_guest_call) {
        m.set_return(E_NOTIMPL);
        return;
    }

    dwrite::GlyphOutlineParams params{};
    params.font = face->second.font;
    params.em_size = em_size;
    params.is_right_to_left = is_right_to_left != 0;
    params.glyph_indices.reserve(glyph_count);
    if (advances_address != 0) params.glyph_advances.reserve(glyph_count);
    if (offsets_address != 0) params.glyph_offsets.reserve(glyph_count);
    for (u32 index = 0; index < glyph_count; ++index) {
        const auto glyph = m.read_u16(indices_address + index * sizeof(u16));
        if (glyph.is_err() ||
            glyph.value() >= face->second.font.glyph_count) {
            m.set_return(E_INVALIDARG);
            return;
        }
        params.glyph_indices.push_back(glyph.value());
        if (advances_address != 0) {
            const auto bits =
                m.read_u32(advances_address + index * sizeof(u32));
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
        if (offsets_address != 0) {
            const auto advance_bits =
                m.read_u32(offsets_address + index * 2 * sizeof(u32));
            const auto ascender_bits = m.read_u32(
                offsets_address + (index * 2 + 1) * sizeof(u32));
            if (advance_bits.is_err() || ascender_bits.is_err()) {
                m.set_return(E_INVALIDARG);
                return;
            }
            dwrite::GlyphOffset offset{};
            offset.advance_offset = float_from_bits(advance_bits.value());
            offset.ascender_offset = float_from_bits(ascender_bits.value());
            if (!std::isfinite(offset.advance_offset) ||
                !std::isfinite(offset.ascender_offset)) {
                m.set_return(E_INVALIDARG);
                return;
            }
            params.glyph_offsets.push_back(offset);
        }
    }

    auto outline = dwrite::glyph_run_outline(params);
    if (!outline) {
        m.set_return(E_FAIL);
        return;
    }

    const auto pack_point = [](float x, float y) -> u64 {
        u32 x_bits = 0;
        u32 y_bits = 0;
        std::memcpy(&x_bits, &x, sizeof(x_bits));
        std::memcpy(&y_bits, &y, sizeof(y_bits));
        return static_cast<u64>(x_bits) |
               (static_cast<u64>(y_bits) << 32);
    };
    const auto write_point = [&](GuestAddr address, float x, float y) {
        u32 x_bits = 0;
        u32 y_bits = 0;
        std::memcpy(&x_bits, &x, sizeof(x_bits));
        std::memcpy(&y_bits, &y, sizeof(y_bits));
        return m.write_u32(address, x_bits).is_ok() &&
               m.write_u32(address + 4, y_bits).is_ok();
    };

    const u64 fill_args[]{outline->fill_mode};
    if (!call_guest_method(sink, 3, fill_args, std::size(fill_args))) {
        m.set_return(E_FAIL);
        return;
    }

    constexpr usize kPointBytes = 8;
    constexpr usize kBezierBytes = 24;
    for (const auto& figure : outline->figures) {
        if (m_is_64bit) {
            const u64 begin_args[]{
                pack_point(figure.start.x, figure.start.y),
                0, // D2D1_FIGURE_BEGIN_FILLED
            };
            if (!call_guest_method(
                    sink, 5, begin_args, std::size(begin_args))) {
                m.set_return(E_FAIL);
                return;
            }
        } else {
            u32 x_bits = 0;
            u32 y_bits = 0;
            std::memcpy(&x_bits, &figure.start.x, sizeof(x_bits));
            std::memcpy(&y_bits, &figure.start.y, sizeof(y_bits));
            const u64 begin_args[]{x_bits, y_bits, 0};
            if (!call_guest_method(
                    sink, 5, begin_args, std::size(begin_args))) {
                m.set_return(E_FAIL);
                return;
            }
        }

        usize index = 0;
        while (index < figure.segments.size()) {
            if (figure.segments[index].kind ==
                dwrite::OutlineSegmentKind::Line) {
                const usize start = index;
                while (index < figure.segments.size() &&
                       figure.segments[index].kind ==
                           dwrite::OutlineSegmentKind::Line) {
                    ++index;
                }
                const usize count = index - start;
                const usize bytes = count * kPointBytes;
                const usize mapped = align_up(
                    std::max(bytes, usize{1}), GUEST_PAGE_SIZE);
                const GuestAddr buffer =
                    m_memory.find_free_gap(0x6D000000ull, mapped);
                if (buffer == 0 ||
                    m_memory.map_region(
                        buffer, mapped, MemoryProtection::ReadWrite)
                        .is_err()) {
                    m.set_return(E_OUTOFMEMORY);
                    return;
                }
                for (usize point = 0; point < count; ++point) {
                    const auto& line = figure.segments[start + point].line;
                    if (!write_point(
                            buffer + point * kPointBytes, line.x, line.y)) {
                        (void)m_memory.unmap_region(buffer, mapped);
                        m.set_return(E_POINTER);
                        return;
                    }
                }
                const u64 line_args[]{buffer, static_cast<u64>(count)};
                const bool ok = call_guest_method(
                    sink, 6, line_args, std::size(line_args)).has_value();
                (void)m_memory.unmap_region(buffer, mapped);
                if (!ok) {
                    m.set_return(E_FAIL);
                    return;
                }
                continue;
            }

            const usize start = index;
            while (index < figure.segments.size() &&
                   figure.segments[index].kind ==
                       dwrite::OutlineSegmentKind::Bezier) {
                ++index;
            }
            const usize count = index - start;
            const usize bytes = count * kBezierBytes;
            const usize mapped = align_up(
                std::max(bytes, usize{1}), GUEST_PAGE_SIZE);
            const GuestAddr buffer =
                m_memory.find_free_gap(0x6D000000ull, mapped);
            if (buffer == 0 ||
                m_memory.map_region(
                    buffer, mapped, MemoryProtection::ReadWrite)
                    .is_err()) {
                m.set_return(E_OUTOFMEMORY);
                return;
            }
            for (usize bezier = 0; bezier < count; ++bezier) {
                const auto& curve =
                    figure.segments[start + bezier].bezier;
                const GuestAddr base = buffer + bezier * kBezierBytes;
                if (!write_point(
                        base, curve.control1.x, curve.control1.y) ||
                    !write_point(
                        base + 8, curve.control2.x, curve.control2.y) ||
                    !write_point(base + 16, curve.end.x, curve.end.y)) {
                    (void)m_memory.unmap_region(buffer, mapped);
                    m.set_return(E_POINTER);
                    return;
                }
            }
            const u64 bezier_args[]{buffer, static_cast<u64>(count)};
            const bool ok = call_guest_method(
                sink, 7, bezier_args, std::size(bezier_args)).has_value();
            (void)m_memory.unmap_region(buffer, mapped);
            if (!ok) {
                m.set_return(E_FAIL);
                return;
            }
        }

        const u64 end_args[]{figure.closed ? 1u : 0u};
        if (!call_guest_method(sink, 8, end_args, std::size(end_args))) {
            m.set_return(E_FAIL);
            return;
        }
    }

    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] GetGlyphRunOutline face=0x%llx "
                     "em=%g glyphs=%u figures=%zu -> S_OK\n",
                     static_cast<unsigned long long>(self),
                     static_cast<double>(em_size), glyph_count,
                     outline->figures.size());
    }
    m.set_return(S_OK);
}

} // namespace vitreo::winapi
