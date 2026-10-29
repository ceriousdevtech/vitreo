// SPDX-License-Identifier: LGPL-2.1-only

/// @file gdi32.cpp
/// @brief GDI32 device context stubs (WIN32-063)

#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/common/display_device_identity.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "core/memory/guest_memory.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "ipc/hwnd_registry.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

/// @file getobject_retrieve_information_about_gdi.cpp
/// @brief GetObject — retrieve information about a GDI object
///
/// Closed cut from gdi32.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// GetObject — retrieve information about a GDI object
// =============================================================================

namespace {

inline void put16(u8* b, usize off, u32 v) {
    b[off] = static_cast<u8>(v);
    b[off + 1] = static_cast<u8>(v >> 8);
}
inline void put32(u8* b, usize off, u32 v) {
    for (usize i = 0; i < 4; ++i) b[off + i] = static_cast<u8>(v >> (8 * i));
}
inline void put64(u8* b, usize off, u64 v) {
    for (usize i = 0; i < 8; ++i) b[off + i] = static_cast<u8>(v >> (8 * i));
}

constexpr u32 kErrorInvalidHandle = 6;
constexpr u32 kStampDesignVector = 0x08000000u + 'd' + ('v' << 8);

}  // namespace

i32 GdiManager::get_object(Handle hgdiobj, i32 cb_buffer, GuestAddr lp_object, bool is_64bit,
                           bool wide, u32* error) {
    ApiLock api_lock(*this);
    auto fail = [&](u32 code) {
        if (error) *error = code;
        return 0;
    };
    auto write = [&](const u8* data, usize size) {
        return m_guest_memory &&
               m_guest_memory->write_bytes(lp_object, data, size).is_ok();
    };
    // s3gdi_bitmap_probe "GetObject other types": NULL, regions and DCs set
    // ERROR_INVALID_HANDLE; any other non-object leaves the last error.
    if (hgdiobj == 0) return fail(kErrorInvalidHandle);
    const u32 type = object_type(hgdiobj);
    switch (type) {
    case OBJ_BITMAP_TYPE: {
        auto bmp = bitmap_surface(hgdiobj);
        if (!bmp) return fail(0);
        const auto dib = m_dib_sections.find(hgdiobj);
        const bool is_dib = dib != m_dib_sections.end();
        const usize bitmap_size = is_64bit ? 32 : 24;
        const usize section_size = is_64bit ? 104 : 84;
        if (lp_object == 0) return static_cast<i32>(bitmap_size);
        if (cb_buffer < static_cast<i32>(bitmap_size)) return fail(0);
        const u16 bpp = is_dib ? dib->second.bpp : bmp->bpp;
        u8 buf[104] = {};
        put32(buf, 0, 0);  // bmType
        put32(buf, 4, static_cast<u32>(bmp->width));
        put32(buf, 8, static_cast<u32>(bmp->height));
        put32(buf, 12, is_dib ? dib->second.stride : gdi::ddb_stride(bmp->width, bpp));
        put16(buf, 16, 1);
        put16(buf, 18, bpp);
        const u64 bits = is_dib ? static_cast<u64>(dib->second.guest_bits) : 0;
        if (is_64bit) put64(buf, 24, bits);
        else put32(buf, 20, static_cast<u32>(bits));
        if (!is_dib || cb_buffer < static_cast<i32>(section_size)) {
            return write(buf, bitmap_size) ? static_cast<i32>(bitmap_size) : fail(0);
        }
        // DIBSECTION: the header reports a positive height and the colour
        // count; dsBitfields holds the masks, 555 for a BI_RGB 16bpp DIB and
        // zero for BI_RGB 32bpp (s3gdi_bitmap_probe "dib32 ds", "dib16rgb").
        const auto& d = dib->second;
        const usize h = bitmap_size;
        put32(buf, h + 0, 40);
        put32(buf, h + 4, static_cast<u32>(d.width));
        put32(buf, h + 8, static_cast<u32>(d.height));
        put16(buf, h + 12, 1);
        put16(buf, h + 14, d.bpp);
        // A 16bpp DIB section reports BI_BITFIELDS even when created BI_RGB
        // (s3gdi_bitmap_probe "dib16rgb"); 32bpp keeps what it was created with.
        put32(buf, h + 16, d.bpp == 16 ? gdi::kBiBitfields : d.compression);
        put32(buf, h + 20, d.stride * static_cast<u32>(d.height));
        put32(buf, h + 32, d.bpp <= 8 ? d.clr_used : 0);
        put32(buf, h + 36, d.bpp <= 8 ? d.clr_important : 0);
        const usize bf = h + 40;
        put32(buf, bf + 0, d.masks[0]);
        put32(buf, bf + 4, d.masks[1]);
        put32(buf, bf + 8, d.masks[2]);
        if (is_64bit) {
            put64(buf, 88, d.section);
            put32(buf, 96, d.offset);
        } else {
            put32(buf, 76, static_cast<u32>(d.section));
            put32(buf, 80, d.offset);
        }
        return write(buf, section_size) ? static_cast<i32>(section_size) : fail(0);
    }
    case OBJ_PEN_TYPE: {
        if (m_pending_delete.contains(hgdiobj)) return fail(0);
        const auto& pen = m_pen_objects.at(hgdiobj);
        constexpr i32 kLogPen = 16;
        if (lp_object == 0) return kLogPen;
        if (cb_buffer < kLogPen) return fail(0);
        u8 buf[kLogPen] = {};
        put32(buf, 0, static_cast<u32>(pen.style));
        put32(buf, 4, static_cast<u32>(pen.width));
        put32(buf, 12, pen.color);
        return write(buf, kLogPen) ? kLogPen : fail(0);
    }
    case OBJ_EXTPEN_TYPE: {
        if (m_pending_delete.contains(hgdiobj)) return fail(0);
        const auto& pen = m_extpen_objects.at(hgdiobj);
        const usize head = is_64bit ? 28 : 24;
        const usize size = head + pen.style_entries.size() * 4u;
        if (lp_object == 0) return static_cast<i32>(size);
        if (cb_buffer < static_cast<i32>(size)) return fail(0);  // "extpen cb=LOGPEN"
        std::vector<u8> buf(size, 0);
        put32(buf.data(), 0, pen.style);
        put32(buf.data(), 4, pen.width);
        put32(buf.data(), 8, pen.brush_style);
        put32(buf.data(), 12, pen.color);
        if (is_64bit) {
            put64(buf.data(), 16, pen.hatch);
            put32(buf.data(), 24, static_cast<u32>(pen.style_entries.size()));
        } else {
            put32(buf.data(), 16, static_cast<u32>(pen.hatch));
            put32(buf.data(), 20, static_cast<u32>(pen.style_entries.size()));
        }
        for (usize i = 0; i < pen.style_entries.size(); ++i) {
            put32(buf.data(), head + i * 4u, pen.style_entries[i]);
        }
        return write(buf.data(), size) ? static_cast<i32>(size) : fail(0);
    }
    case OBJ_BRUSH_TYPE: {
        // A brush deleted while selected no longer answers GetObject
        // (s3gdi_bitmap_probe "deferred brush").
        if (m_pending_delete.contains(hgdiobj)) return fail(0);
        const i32 size = is_64bit ? 16 : 12;
        if (lp_object == 0) return size;
        if (cb_buffer < size) return fail(0);
        BrushInfo info{0, 0, 0};
        if (const auto* known = lookup_brush(hgdiobj)) {
            info = *known;
        } else if (u32 color = 0; lookup_brush_color(hgdiobj, &color)) {
            info.color = color;
        } else if (const auto pat = m_brush_patterns.find(hgdiobj); pat != m_brush_patterns.end()) {
            info.style = 3;
        }
        u8 buf[16] = {};
        put32(buf, 0, info.style);
        put32(buf, 4, info.color);
        if (is_64bit) put64(buf, 8, info.hatch);
        else put32(buf, 8, static_cast<u32>(info.hatch));
        return write(buf, static_cast<usize>(size)) ? size : fail(0);
    }
    case OBJ_FONT_TYPE: {
        if (m_pending_delete.contains(hgdiobj)) return fail(0);
        const FontInfo* f = lookup_font(hgdiobj);
        if (!f) return fail(0);
        // GetObjectW answers LOGFONTW (92) for a NULL buffer and copies up
        // to an ENUMLOGFONTEXDVW image (356) otherwise; GetObjectA the same
        // with LOGFONTA (60) / ENUMLOGFONTEXDVA (260) (s3gdi_info_probe
        // "big", s3gdi_bitmap_probe "font cb30").
        const usize base = wide ? 92 : 60;
        const usize full = wide ? 356 : 260;
        if (lp_object == 0) return static_cast<i32>(base);
        if (cb_buffer <= 0) return fail(0);
        std::vector<u8> buf(full, 0);
        put32(buf.data(), 0, static_cast<u32>(f->height));
        put32(buf.data(), 4, static_cast<u32>(f->width));
        put32(buf.data(), 8, static_cast<u32>(f->escapement));
        put32(buf.data(), 12, static_cast<u32>(f->orientation));
        put32(buf.data(), 16, static_cast<u32>(f->lf_weight));
        buf[20] = f->italic ? 1 : 0;
        buf[21] = f->underline;
        buf[22] = f->strike_out;
        buf[23] = f->char_set;
        buf[24] = f->out_precision;
        buf[25] = f->clip_precision;
        buf[26] = f->quality;
        buf[27] = f->pitch_and_family;
        auto put_name = [&](usize offset, const std::u16string& name, usize chars) {
            for (usize i = 0; i < name.size() && i < chars; ++i) {
                if (wide) {
                    put16(buf.data(), offset + i * 2, name[i]);
                } else {
                    buf[offset + i] = name[i] < 0x80 ? static_cast<u8>(name[i]) : u8{'?'};
                }
            }
        };
        put_name(28, f->face_name, 32);
        put_name(base, f->full_name, 64);  // elfFullName
        put32(buf.data(), wide ? 348 : 188, kStampDesignVector);
        const usize n = std::min<usize>(static_cast<usize>(cb_buffer), full);
        return write(buf.data(), n) ? static_cast<i32>(n) : fail(0);
    }
    case OBJ_PAL_TYPE: {
        u16 count = 20;  // DEFAULT_PALETTE: the 20 static colours
        if (const auto pal = m_palette_objects.find(hgdiobj); pal != m_palette_objects.end()) {
            count = static_cast<u16>(pal->second.entries.size());
        }
        if (lp_object == 0) return 2;
        if (cb_buffer < 2) return fail(0);
        u8 buf[2];
        put16(buf, 0, count);
        return write(buf, 2) ? 2 : fail(0);
    }
    case OBJ_REGION_TYPE:
    case OBJ_DC_TYPE:
    case OBJ_MEMDC_TYPE:
        return fail(kErrorInvalidHandle);
    default:
        return fail(0);
    }
}

} // namespace vitreo::winapi
