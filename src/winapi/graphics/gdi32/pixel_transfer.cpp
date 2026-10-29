// SPDX-License-Identifier: LGPL-2.1-only

/// @file pixel_transfer.cpp
/// @brief Bitmap objects and the exports that move pixels in and out of them:
/// CreateBitmap, CreateCompatibleBitmap, GetBitmapBits/SetBitmapBits,
/// GetDIBits/SetDIBits.
///
/// Every rule here is measured on Windows 11 by
/// tools/oracle_probe/s3gdi_bitmap_probe (x64 and x86 expected files):
///  - a device-dependent bitmap's depth is nPlanes*nBitCount rounded up to
///    1/4/8/16/24/32, its rows are WORD aligned, and it starts zero-filled;
///  - a zero-sized bitmap is the stock 1x1 monochrome bitmap;
///  - DIB scan lines are numbered from the bottom whatever the DIB's
///    orientation, and a DIB taller than the bitmap holds it at its top;
///  - a colour DIB lands in a monochrome bitmap as white exactly where it
///    equals the DC's background colour.

#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/graphics/gdi32/bitmap_codec.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/error.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace vitreo::winapi {

namespace {

constexpr u32 kErrorInvalidParameter = 87;
constexpr u32 kErrorNotEnoughMemory = 8;
/// GDI surfaces are RGBA8 host memory; refuse anything that could not be
/// held rather than aborting on a failed allocation.
constexpr u64 kMaxSurfaceBytes = u64{1} << 31;

[[nodiscard]] u32 bitfield_masks_for(u16 bpp, std::array<u32, 3>& masks) {
    masks = bpp == 16 ? std::array<u32, 3>{0x7C00, 0x03E0, 0x001F}
                      : std::array<u32, 3>{0x00FF0000, 0x0000FF00, 0x000000FF};
    return gdi::kBiBitfields;
}

/// Copy @p count palette indices of a packed row (1/4/8bpp) unchanged.
void copy_indices(const u8* src, u8* dst, u16 bpp, i32 count) {
    for (i32 x = 0; x < count; ++x) {
        u32 v = 0;
        switch (bpp) {
        case 1: v = (src[x >> 3] >> (7 - (x & 7))) & 1u; break;
        case 4: v = (x & 1) ? (src[x >> 1] & 0x0Fu) : (src[x >> 1] >> 4); break;
        default: v = src[x]; break;
        }
        switch (bpp) {
        case 1:
            if (v) dst[x >> 3] |= static_cast<u8>(0x80u >> (x & 7));
            break;
        case 4:
            dst[x >> 1] |= static_cast<u8>((x & 1) ? v : (v << 4));
            break;
        default:
            dst[x] = static_cast<u8>(v);
            break;
        }
    }
}

}  // namespace

std::shared_ptr<SoftBitmap> GdiManager::bitmap_surface(Handle hbm) {
    ApiLock api_lock(*this);
    if (const auto it = m_bitmap_objects.find(hbm);
        it != m_bitmap_objects.end() && it->second) {
        return it->second;
    }
    if (m_cocoa_bridge && (hbm & 0xFFF00000u) == 0x60000000u) {
        if (auto soft = cocoa_bitmap_to_soft(hbm)) {
            m_bitmap_objects[hbm] = soft;
            return soft;
        }
    }
    return nullptr;
}

u32 GdiManager::object_type(Handle h) const {
    ApiLock api_lock(*this);
    if (h == 0) return 0;
    if (const auto it = m_dc_states.find(h); it != m_dc_states.end()) {
        return it->second.memory_dc ? OBJ_MEMDC_TYPE : OBJ_DC_TYPE;
    }
    if (m_extpen_objects.contains(h)) return OBJ_EXTPEN_TYPE;
    if (m_system_brush_color && system_brush_color(h)) return OBJ_BRUSH_TYPE;
    if (m_pen_objects.contains(h)) return OBJ_PEN_TYPE;
    if (m_brush_info.contains(h) || m_brush_colors.contains(h) ||
        m_brush_patterns.contains(h)) {
        return OBJ_BRUSH_TYPE;
    }
    if (m_bitmap_objects.contains(h)) return OBJ_BITMAP_TYPE;
    if (m_cocoa_bridge && (h & 0xFFF00000u) == 0x60000000u) {
        windowing::CocoaBridge::BitmapDims dims{};
        const auto bridge = m_cocoa_bridge;
        bool found = false;
        {
            ApiUnlock unlocked(*this);
            found = bridge->get_bitmap_dims(h, dims);
        }
        if (found) return OBJ_BITMAP_TYPE;
    }
    if (m_region_objects.contains(h)) return OBJ_REGION_TYPE;
    if (m_palette_objects.contains(h)) return OBJ_PAL_TYPE;
    if (const auto pal = m_stock_objects.find(DEFAULT_PALETTE_ID);
        pal != m_stock_objects.end() && pal->second == h) {
        return OBJ_PAL_TYPE;
    }
    {
        std::lock_guard font_lock(gdi_font_registry_mutex());
        if (gdi_font_registry().contains(h)) return OBJ_FONT_TYPE;
    }
    if (m_metafile_objects.contains(h)) return 9;  // OBJ_METAFILE
    if (m_enh_metafiles.contains(h)) return 13;    // OBJ_ENHMETAFILE
    return 0;
}

bool GdiManager::object_is_selected(Handle obj) const {
    ApiLock api_lock(*this);
    for (const auto& [dc, state] : m_dc_states) {
        if (state.selected_bitmap_handle == obj || state.selected_brush == obj ||
            state.selected_pen == obj || state.selected_font == obj ||
            state.selected_palette == obj) {
            return true;
        }
    }
    return false;
}

std::vector<u32> GdiManager::dc_logical_palette(Handle hdc) const {
    ApiLock api_lock(*this);
    std::vector<u32> out;
    const auto dc = m_dc_states.find(hdc);
    if (dc != m_dc_states.end()) {
        const auto pal = m_palette_objects.find(dc->second.selected_palette);
        if (pal != m_palette_objects.end()) {
            for (const auto& e : pal->second.entries) {
                out.push_back((static_cast<u32>(e.red) << 16) |
                              (static_cast<u32>(e.green) << 8) | e.blue);
            }
            return out;
        }
    }
    // DEFAULT_PALETTE: the 20 static colours, the first and last ten entries
    // of the 8bpp system table (default_palettes.hpp).
    out.assign(gdi::kGdiStaticColors.begin(), gdi::kGdiStaticColors.end());
    return out;
}

void GdiManager::quantize_ddb_rows(SoftBitmap& bmp, i32 y0, i32 y1) const {
    ApiLock api_lock(*this);
    if (bmp.bpp == 32 || bmp.width <= 0) return;
    const gdi::PixelFormat format = gdi::ddb_pixel_format(bmp.bpp);
    std::vector<u8> packed(gdi::ddb_stride(bmp.width, bmp.bpp) + 4u);
    y0 = std::max(y0, 0);
    y1 = std::min(y1, bmp.height);
    for (i32 y = y0; y < y1; ++y) {
        u8* row = bmp.pixels.data() + static_cast<usize>(y) * bmp.width * 4u;
        std::fill(packed.begin(), packed.end(), u8{0});
        gdi::encode_row(format, row, packed.data(), bmp.width);
        gdi::decode_row(format, packed.data(), row, bmp.width);
    }
    bmp.dirty = true;
}

Handle GdiManager::create_bitmap(i32 width, i32 height, u32 planes, u32 bits_per_pixel,
                                 GuestAddr bits, u32* error) {
    ApiLock api_lock(*this);
    auto fail = [&](u32 code) {
        if (error) *error = code;
        return Handle{0};
    };
    if (width < 0 || height < 0 || planes > 32 || bits_per_pixel > 32) {
        return fail(kErrorInvalidParameter);
    }
    const u16 depth = gdi::round_ddb_bpp(planes * bits_per_pixel);
    if (depth == 0) return fail(kErrorInvalidParameter);
    if (width == 0 || height == 0) return m_stock_objects[DEFAULT_BITMAP_ID];

    const u64 stride = ((static_cast<u64>(width) * depth + 15u) / 16u) * 2u;
    if (stride * static_cast<u64>(height) > 0x7FFFFFFFu) {
        return fail(kErrorInvalidParameter);
    }
    if (static_cast<u64>(width) * static_cast<u64>(height) * 4u > kMaxSurfaceBytes) {
        return fail(kErrorNotEnoughMemory);
    }

    auto bmp = std::make_shared<SoftBitmap>();
    bmp->width = width;
    bmp->height = height;
    bmp->bpp = depth;
    // Windows zero-fills a new bitmap (s3gdi_bitmap_probe "initial contents").
    bmp->pixels.assign(static_cast<usize>(width) * height * 4u, 0);
    if (bits != 0 && m_guest_memory) {
        std::vector<u8> packed(static_cast<usize>(stride) * height);
        if (m_guest_memory->read_bytes(bits, packed.data(), packed.size()).is_err()) {
            return fail(kErrorInvalidParameter);
        }
        const gdi::PixelFormat format = gdi::ddb_pixel_format(depth);
        for (i32 y = 0; y < height; ++y) {
            gdi::decode_row(format, packed.data() + static_cast<usize>(y) * stride,
                            bmp->pixels.data() + static_cast<usize>(y) * width * 4u, width);
        }
        if (static_cast<u64>(width) * depth != stride * 8u) bmp->ddb_raw = std::move(packed);
    }
    const Handle h = allocate_gdi_handle(kHandleTypeGdiObject);
    m_bitmap_objects[h] = std::move(bmp);
    return h;
}

Handle GdiManager::create_compatible_bitmap(Handle hdc, i32 width, i32 height) {
    ApiLock api_lock(*this);
    const auto dc = m_dc_states.find(hdc);
    if (dc == m_dc_states.end() || width < 0 || height < 0) return 0;
    if (width == 0 || height == 0) return m_stock_objects[DEFAULT_BITMAP_ID];

    // Learn (CreateCompatibleBitmap): with a DIB section selected the result
    // is a DIB section of the same format - bottom-up, colour table kept
    // (s3gdi_bitmap_probe "ccb dib-selected", "ccb dib8-selected").
    const Handle selected = dc->second.selected_bitmap_handle;
    if (const auto proto = m_dib_sections.find(selected); proto != m_dib_sections.end()) {
        const DIBSectionInfo p = proto->second;
        const u32 entries = static_cast<u32>(p.color_table.size() / 4u);
        return make_dib_section(width, height, true, p.bpp, p.compression, p.masks,
                                p.color_table, p.logical_palette, p.palette_indices,
                                p.bpp <= 8 ? entries : 0, p.bpp <= 8 ? entries : 0,
                                0, 0, nullptr, nullptr);
    }

    // A memory DC is compatible with the bitmap selected into it - the 1x1
    // monochrome default until the caller selects another - and every other
    // DC with the 32bpp display (s3gdi_bitmap_probe "ccb fresh-memdc").
    u16 depth = 32;
    if (dc->second.memory_dc && dc->second.bitmap) depth = dc->second.bitmap->bpp;
    if (static_cast<u64>(width) * static_cast<u64>(height) * 4u > kMaxSurfaceBytes) return 0;

    auto bmp = std::make_shared<SoftBitmap>();
    bmp->width = width;
    bmp->height = height;
    bmp->bpp = depth;
    bmp->pixels.assign(static_cast<usize>(width) * height * 4u, 0);
    const Handle h = allocate_gdi_handle(kHandleTypeGdiObject);
    m_bitmap_objects[h] = std::move(bmp);
    VITREO_TRACE("[gdi] CreateCompatibleBitmap(hdc=0x%x %dx%d) bpp=%u -> 0x%x\n", hdc,
                 width, height, depth, h);
    return h;
}

i32 GdiManager::get_bitmap_bits(Handle hbm, i32 cb, GuestAddr out, u32* error) {
    ApiLock api_lock(*this);
    auto bmp = bitmap_surface(hbm);
    if (!bmp || !m_guest_memory) {
        if (error) *error = 0;
        return 0;
    }
    const auto dib = m_dib_sections.find(hbm);
    const u16 bpp = dib != m_dib_sections.end() ? dib->second.bpp : bmp->bpp;
    const u32 stride = gdi::ddb_stride(bmp->width, bpp);
    const u32 total = stride * static_cast<u32>(bmp->height);
    if (out == 0) return static_cast<i32>(total);
    if (cb <= 0) return 0;
    const u32 n = std::min(static_cast<u32>(cb), total);

    std::vector<u8> packed(total, 0);
    if (dib != m_dib_sections.end()) {
        // A DIB section answers with its own bytes, re-laid as WORD-aligned
        // rows top row first (s3gdi_bitmap_probe "gbb dib24 bu").
        const auto& d = dib->second;
        for (i32 y = 0; y < d.height; ++y) {
            const i32 src_row = d.bottom_up ? d.height - 1 - y : y;
            (void)m_guest_memory->read_bytes(
                d.guest_bits + static_cast<GuestAddr>(src_row) * d.stride,
                packed.data() + static_cast<usize>(y) * stride, stride);
        }
    } else {
        if (bmp->ddb_raw.size() == packed.size()) packed = bmp->ddb_raw;
        const gdi::PixelFormat format = gdi::ddb_pixel_format(bpp);
        for (i32 y = 0; y < bmp->height; ++y) {
            gdi::encode_row(format, bmp->pixels.data() + static_cast<usize>(y) * bmp->width * 4u,
                            packed.data() + static_cast<usize>(y) * stride, bmp->width);
        }
    }
    if (m_guest_memory->write_bytes(out, packed.data(), n).is_err()) return 0;
    return static_cast<i32>(n);
}

i32 GdiManager::set_bitmap_bits(Handle hbm, u32 cb, GuestAddr in, u32* error) {
    ApiLock api_lock(*this);
    auto bmp = bitmap_surface(hbm);
    if (!bmp || in == 0 || !m_guest_memory) {
        if (error) *error = 0;
        return 0;
    }
    const auto dib = m_dib_sections.find(hbm);
    const u16 bpp = dib != m_dib_sections.end() ? dib->second.bpp : bmp->bpp;
    const u32 stride = gdi::ddb_stride(bmp->width, bpp);
    const u32 total = stride * static_cast<u32>(bmp->height);
    const u32 n = std::min(cb, total);
    if (n == 0) return 0;

    std::vector<u8> incoming(n);
    if (m_guest_memory->read_bytes(in, incoming.data(), n).is_err()) return 0;

    if (dib != m_dib_sections.end()) {
        const auto& d = dib->second;
        for (i32 y = 0; y < d.height; ++y) {
            const u32 offset = static_cast<u32>(y) * stride;
            if (offset >= n) break;
            const u32 bytes = std::min(stride, n - offset);
            const i32 dst_row = d.bottom_up ? d.height - 1 - y : y;
            (void)m_guest_memory->write_bytes(
                d.guest_bits + static_cast<GuestAddr>(dst_row) * d.stride,
                incoming.data() + offset, bytes);
        }
        sync_dib_from_guest(hbm);
        return static_cast<i32>(n);
    }

    const gdi::PixelFormat format = gdi::ddb_pixel_format(bpp);
    std::vector<u8> packed(total, 0);
    if (bmp->ddb_raw.size() == packed.size()) packed = bmp->ddb_raw;
    for (i32 y = 0; y < bmp->height; ++y) {
        gdi::encode_row(format, bmp->pixels.data() + static_cast<usize>(y) * bmp->width * 4u,
                        packed.data() + static_cast<usize>(y) * stride, bmp->width);
    }
    std::memcpy(packed.data(), incoming.data(), n);
    if (static_cast<u64>(bmp->width) * bpp != static_cast<u64>(stride) * 8u) bmp->ddb_raw = packed;
    for (i32 y = 0; y < bmp->height; ++y) {
        gdi::decode_row(format, packed.data() + static_cast<usize>(y) * stride,
                        bmp->pixels.data() + static_cast<usize>(y) * bmp->width * 4u,
                        bmp->width);
    }
    bmp->dirty = true;
    return static_cast<i32>(n);
}

i32 GdiManager::get_dib_bits(Handle hdc, Handle hbm, u32 start_scan, u32 scan_lines,
                             GuestAddr bits, GuestAddr bmi, u32 usage, u32* error) {
    ApiLock api_lock(*this);
    auto fail = [&](u32 code) {
        if (error) *error = code;
        return 0;
    };
    if (!m_guest_memory) return fail(0);
    if (!m_dc_states.contains(hdc)) return fail(kErrorInvalidParameter);
    auto bmp = bitmap_surface(hbm);
    if (!bmp) return fail(0);
    if (usage > gdi::kDibPalColors) return fail(0);
    gdi::DibHeader hdr;
    if (!gdi::read_dib_header(*m_guest_memory, bmi, hdr)) return fail(0);

    const auto dib = m_dib_sections.find(hbm);
    const bool is_dib = dib != m_dib_sections.end();
    const u16 src_bpp = is_dib ? dib->second.bpp : bmp->bpp;
    const i32 bw = bmp->width;
    const i32 bh = bmp->height;
    auto& mem = *m_guest_memory;

    // biBitCount 0: describe the bitmap, header only (s3gdi_bitmap_probe
    // "query0", "mono query0", "dib8 query0", "gdib biSize12").
    if (hdr.bpp == 0) {
        if (bits != 0) return fail(0);
        if (hdr.core()) {
            (void)mem.write_u16(bmi + 4, static_cast<u16>(bw));
            (void)mem.write_u16(bmi + 6, static_cast<u16>(bh));
            (void)mem.write_u16(bmi + 8, 1);
            (void)mem.write_u16(bmi + 10, static_cast<u16>(src_bpp == 32 ? 24 : src_bpp));
            return 1;
        }
        u32 compression = gdi::kBiRgb;
        if (src_bpp == 32 ||
            (is_dib && dib->second.compression == gdi::kBiBitfields)) {
            compression = gdi::kBiBitfields;
        }
        u32 clr_used = 0, clr_important = 0;
        if (is_dib && src_bpp <= 8) {
            clr_used = clr_important = static_cast<u32>(dib->second.color_table.size() / 4u);
        } else if (!is_dib && src_bpp == 1) {
            clr_used = 2;
        }
        (void)mem.write_u32(bmi + 4, static_cast<u32>(bw));
        (void)mem.write_u32(bmi + 8, static_cast<u32>(bh));
        (void)mem.write_u16(bmi + 12, 1);
        (void)mem.write_u16(bmi + 14, src_bpp);
        (void)mem.write_u32(bmi + 16, compression);
        (void)mem.write_u32(bmi + 20, gdi::dib_stride(bw, src_bpp) * static_cast<u32>(bh));
        (void)mem.write_u32(bmi + 24, 0);
        (void)mem.write_u32(bmi + 28, 0);
        (void)mem.write_u32(bmi + 32, clr_used);
        (void)mem.write_u32(bmi + 36, clr_important);
        return 1;
    }

    switch (hdr.bpp) {
    case 1: case 4: case 8: case 24:
        if (hdr.compression != gdi::kBiRgb) return fail(0);
        break;
    case 16: case 32:
        if (hdr.compression != gdi::kBiRgb && hdr.compression != gdi::kBiBitfields) return fail(0);
        break;
    default:
        return fail(0);  // "gdib7": 0, header untouched
    }
    // A device-dependent bitmap that is neither monochrome nor display
    // format cannot be read back through a display-compatible DC
    // (s3gdi_bitmap_probe "rgb24/rgb16/pal8 gdib32": 0).
    if (!is_dib && src_bpp != 1 && src_bpp != 32) return fail(0);
    const i32 dib_w = hdr.width;
    const i32 dib_h = hdr.abs_height();
    if (dib_w <= 0 || dib_h == 0) return fail(0);

    gdi::PixelFormat format;
    format.bpp = hdr.bpp;
    bool raw_indices = false;
    if (hdr.bpp <= 8) {
        if (is_dib && dib->second.bpp == hdr.bpp) {
            // Same-depth read of a DIB section keeps its indices and its
            // colour table (s3gdi_bitmap_probe "dib8 gdib8").
            raw_indices = true;
            const auto& table = dib->second.color_table;
            for (usize i = 0; i + 3 < table.size(); i += 4) {
                format.palette.push_back((static_cast<u32>(table[i + 2]) << 16) |
                                         (static_cast<u32>(table[i + 1]) << 8) | table[i]);
            }
        } else {
            format.palette = gdi::ddb_pixel_format(hdr.bpp).palette;
        }
    } else if (hdr.compression == gdi::kBiBitfields) {
        // The caller's masks are replaced by the bitmap's own layout
        // (s3gdi_bitmap_probe "gdib16bf565-in" returns 555 masks).
        format.bitfields = true;
        (void)bitfield_masks_for(hdr.bpp, format.masks);
    }

    const u32 stride = gdi::dib_stride(dib_w, hdr.bpp);
    if (!hdr.core()) {
        (void)mem.write_u32(bmi + 20, stride * static_cast<u32>(dib_h));
    }
    if (hdr.bpp <= 8) {
        const GuestAddr table = hdr.color_table();
        for (u32 i = 0; i < format.palette.size(); ++i) {
            const u32 c = format.palette[i];
            if (usage == gdi::kDibPalColors) {
                (void)mem.write_u16(table + i * 2u, static_cast<u16>(i));
            } else if (hdr.core()) {
                const u8 t[3] = {static_cast<u8>(c), static_cast<u8>(c >> 8),
                                 static_cast<u8>(c >> 16)};
                (void)mem.write_bytes(table + i * 3u, t, 3);
            } else {
                (void)mem.write_u32(table + i * 4u, c);
            }
        }
    } else if (format.bitfields) {
        (void)mem.write_u32(hdr.mask_address(), format.masks[0]);
        (void)mem.write_u32(hdr.mask_address() + 4, format.masks[1]);
        (void)mem.write_u32(hdr.mask_address() + 8, format.masks[2]);
    }
    if (bits == 0) return 1;
    // A start past the DIB answers 1 and writes nothing ("gdib32 bu start3").
    if (start_scan >= static_cast<u32>(dib_h)) return 1;
    const u32 lines = std::min(scan_lines, static_cast<u32>(dib_h) - start_scan);

    if (is_dib) sync_dib_from_guest(hbm);
    const i32 copy_w = std::min(dib_w, bw);
    std::vector<u8> row(stride);
    std::vector<u8> source_row;
    i32 copied = 0;
    for (u32 r = 0; r < lines; ++r) {
        // Scan lines count from the bottom; a top-down buffer holds its band
        // top row first. The bitmap sits at the top of a taller DIB.
        const u32 scan = hdr.top_down() ? start_scan + lines - 1 - r : start_scan + r;
        const i32 y = dib_h - 1 - static_cast<i32>(scan);
        std::fill(row.begin(), row.end(), u8{0});
        if (y >= 0 && y < bh) {
            ++copied;
            if (raw_indices) {
                const auto& d = dib->second;
                const i32 src_row = d.bottom_up ? d.height - 1 - y : y;
                source_row.assign(d.stride, 0);
                (void)mem.read_bytes(d.guest_bits + static_cast<GuestAddr>(src_row) * d.stride,
                                     source_row.data(), d.stride);
                copy_indices(source_row.data(), row.data(), hdr.bpp, copy_w);
            } else {
                gdi::encode_row(format, bmp->pixels.data() + static_cast<usize>(y) * bw * 4u,
                                row.data(), copy_w);
            }
        }
        if (mem.write_bytes(bits + static_cast<GuestAddr>(r) * stride, row.data(), stride).is_err()) {
            return fail(0);
        }
    }
    return copied;
}

i32 GdiManager::set_dib_bits(Handle hdc, Handle hbm, u32 start_scan, u32 scan_lines,
                             GuestAddr bits_addr, GuestAddr bmi_addr, u32 usage) {
    ApiLock api_lock(*this);
    if (!m_guest_memory || bits_addr == 0) return 0;
    auto bmp = bitmap_surface(hbm);
    if (!bmp) return 0;
    gdi::DibHeader hdr;
    if (!gdi::read_dib_header(*m_guest_memory, bmi_addr, hdr)) return 0;
    if (hdr.compression != gdi::kBiRgb && hdr.compression != gdi::kBiBitfields) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED gdi32!SetDIBits: compression %u "
                                 "(RLE/JPEG/PNG source)\n", hdr.compression);
        }
        return 0;
    }
    if (!gdi::dib_is_decodable(hdr) || usage > gdi::kDibPalColors) return 0;
    gdi::PixelFormat format;
    if (!gdi::dib_pixel_format(*m_guest_memory, hdr, usage, dc_logical_palette(hdc), format)) {
        return 0;
    }
    const i32 dib_w = hdr.width;
    const i32 dib_h = hdr.abs_height();
    if (dib_w <= 0 || dib_h == 0 || start_scan >= static_cast<u32>(dib_h)) return 0;
    const u32 lines = std::min(scan_lines, static_cast<u32>(dib_h) - start_scan);
    if (lines == 0) return 0;

    const bool is_dib = m_dib_sections.contains(hbm);
    if (is_dib) sync_dib_from_guest(hbm);
    const u32 bk = [&] {
        const auto dc = m_dc_states.find(hdc);
        return dc != m_dc_states.end() ? dc->second.bk_color & 0x00FFFFFFu : 0x00FFFFFFu;
    }();
    const bool keeps_fourth_byte = hdr.bpp == 32 && hdr.compression == gdi::kBiRgb;
    const u32 stride = hdr.stride();
    const i32 copy_w = std::min(dib_w, bmp->width);
    std::vector<u8> row(stride);
    std::vector<u8> rgba(static_cast<usize>(dib_w) * 4u);
    for (u32 r = 0; r < lines; ++r) {
        const u32 scan = hdr.top_down() ? start_scan + lines - 1 - r : start_scan + r;
        const i32 y = dib_h - 1 - static_cast<i32>(scan);
        if (y < 0 || y >= bmp->height) continue;
        if (m_guest_memory->read_bytes(bits_addr + static_cast<GuestAddr>(r) * stride, row.data(),
                                       stride).is_err()) {
            return 0;
        }
        gdi::decode_row(format, row.data(), rgba.data(), copy_w);
        u8* dst = bmp->pixels.data() + static_cast<usize>(y) * bmp->width * 4u;
        if (bmp->bpp == 1) {
            // Colour -> monochrome: white exactly where the pixel equals the
            // DC's background colour (s3gdi_bitmap_probe "sdib32->mono").
            for (i32 x = 0; x < copy_w; ++x) {
                const u8* p = rgba.data() + x * 4;
                const u32 rgb = p[0] | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16);
                const bool white = rgb == bk && (!keeps_fourth_byte || p[3] == 0);
                const u8 v = white ? 0xFF : 0x00;
                u8* d = dst + x * 4;
                d[0] = d[1] = d[2] = v;
                d[3] = 0;
            }
        } else {
            std::memcpy(dst, rgba.data(), static_cast<usize>(copy_w) * 4u);
        }
    }
    if (!is_dib) quantize_ddb_rows(*bmp, 0, bmp->height);
    bmp->dirty = true;
    if (is_dib) sync_dib_to_guest(hbm);
    return static_cast<i32>(lines);
}

Handle GdiManager::create_mono_bitmap(i32 width, i32 height, std::span<const u8> bits01) {
    ApiLock api_lock(*this);
    if (width <= 0 || height <= 0 ||
        bits01.size() < static_cast<usize>(width) * static_cast<usize>(height)) {
        return 0;
    }
    auto bmp = std::make_shared<SoftBitmap>();
    bmp->width = width;
    bmp->height = height;
    bmp->bpp = 1;
    bmp->pixels.assign(static_cast<usize>(width) * height * 4u, 0);
    for (usize i = 0; i < static_cast<usize>(width) * height; ++i) {
        if (bits01[i]) std::memset(bmp->pixels.data() + i * 4u, 0xFF, 3);
    }
    const Handle h = allocate_gdi_handle(kHandleTypeGdiObject);
    m_bitmap_objects[h] = std::move(bmp);
    return h;
}

Handle GdiManager::copy_bitmap(Handle src, i32 cx, i32 cy, u32 flags, u32* error) {
    ApiLock api_lock(*this);
    constexpr u32 LR_MONOCHROME = 0x1;
    constexpr u32 LR_COPYRETURNORG = 0x4;
    constexpr u32 LR_COPYDELETEORG = 0x8;
    constexpr u32 LR_CREATEDIBSECTION = 0x2000;
    auto source = bitmap_surface(src);
    if (!source || object_type(src) != OBJ_BITMAP_TYPE) {
        if (error) *error = 6;  // s3gdi_icon_probe "CopyImage icon-as-bitmap": err 6
        return 0;
    }
    const auto dib = m_dib_sections.find(src);
    if (dib != m_dib_sections.end()) sync_dib_from_guest(src);
    const i32 w = cx > 0 ? cx : source->width;
    const i32 h = cy > 0 ? cy : source->height;
    const bool same_size = w == source->width && h == source->height;
    if ((flags & LR_COPYRETURNORG) != 0 && same_size &&
        (flags & (LR_MONOCHROME | LR_CREATEDIBSECTION)) == 0) {
        return src;
    }
    // Resample: shrinking averages the covered block, growing takes the
    // nearest pixel centre (s3gdi_icon_probe "copied-dib-2x2").
    std::vector<u8> rgba(static_cast<usize>(w) * h * 4u, 0);
    const i32 sw = source->width, sh = source->height;
    for (i32 y = 0; y < h; ++y) {
        for (i32 x = 0; x < w; ++x) {
            u8* d = rgba.data() + (static_cast<usize>(y) * w + x) * 4u;
            const i32 x0 = w < sw ? x * sw / w : static_cast<i32>((2 * x + 1) * static_cast<i64>(sw) / (2 * w));
            const i32 x1 = w < sw ? std::max(x0 + 1, (x + 1) * sw / w) : x0 + 1;
            const i32 y0 = h < sh ? y * sh / h : static_cast<i32>((2 * y + 1) * static_cast<i64>(sh) / (2 * h));
            const i32 y1 = h < sh ? std::max(y0 + 1, (y + 1) * sh / h) : y0 + 1;
            u32 sum[4] = {0, 0, 0, 0};
            u32 n = 0;
            for (i32 sy = y0; sy < y1; ++sy) {
                for (i32 sx = x0; sx < x1; ++sx) {
                    const u8* p = source->pixel_at(sx, sy);
                    for (u32 c = 0; c < 4; ++c) sum[c] += p[c];
                    ++n;
                }
            }
            // The average truncates (s3gdi_icon_probe "copied-dib-2x2": alpha
            // 80,ff,80,ff -> bf).
            for (u32 c = 0; c < 4; ++c) d[c] = static_cast<u8>(sum[c] / n);
        }
    }
    Handle out = 0;
    const bool mono = (flags & LR_MONOCHROME) != 0 || (source->bpp == 1 && dib == m_dib_sections.end());
    if (mono) {
        std::vector<u8> bits(static_cast<usize>(w) * h);
        for (usize i = 0; i < bits.size(); ++i) {
            const u8* p = rgba.data() + i * 4u;
            const i32 lum2 = (p[0] - 255) * (p[0] - 255) + (p[1] - 255) * (p[1] - 255) +
                             (p[2] - 255) * (p[2] - 255);
            const i32 lum0 = p[0] * p[0] + p[1] * p[1] + p[2] * p[2];
            bits[i] = lum2 < lum0 ? 1 : 0;
        }
        out = create_mono_bitmap(w, h, bits);
    } else if ((flags & LR_CREATEDIBSECTION) != 0) {
        const u16 bpp = dib != m_dib_sections.end() ? dib->second.bpp : std::max<u16>(source->bpp, 1);
        std::array<u32, 3> masks{};
        std::vector<u8> table;
        u32 compression = gdi::kBiRgb;
        if (dib != m_dib_sections.end()) {
            masks = dib->second.masks;
            table = dib->second.color_table;
            compression = dib->second.compression;
        } else if (bpp <= 8) {
            for (const u32 c : gdi::ddb_pixel_format(bpp).palette) {
                table.push_back(static_cast<u8>(c));
                table.push_back(static_cast<u8>(c >> 8));
                table.push_back(static_cast<u8>(c >> 16));
                table.push_back(0);
            }
        }
        const u32 entries = static_cast<u32>(table.size() / 4u);
        out = make_dib_section(w, h, true, bpp, compression, masks, std::move(table), 0, {},
                               entries, entries, 0, 0, nullptr, error);
        if (out != 0) {
            m_bitmap_objects[out]->pixels = std::move(rgba);
            sync_dib_to_guest(out);
            sync_dib_from_guest(out);
        }
    } else {
        // A DIB section copies to a display-format DDB that keeps its alpha
        // byte (s3gdi_icon_probe "copied-dib").
        auto bmp = std::make_shared<SoftBitmap>();
        bmp->width = w;
        bmp->height = h;
        bmp->bpp = dib != m_dib_sections.end() ? 32 : source->bpp;
        bmp->pixels = std::move(rgba);
        out = allocate_gdi_handle(kHandleTypeGdiObject);
        m_bitmap_objects[out] = std::move(bmp);
    }
    if (out != 0 && (flags & LR_COPYDELETEORG) != 0 && out != src) (void)delete_object(src);
    return out;
}

Handle GdiManager::create_dib_pattern_brush(GuestAddr packed_dib, u32 usage) {
    ApiLock api_lock(*this);
    if (!m_guest_memory || packed_dib == 0 || usage > gdi::kDibPalColors) return 0;
    gdi::DibHeader hdr;
    if (!gdi::read_dib_header(*m_guest_memory, packed_dib, hdr) || !gdi::dib_is_decodable(hdr)) {
        return 0;
    }
    gdi::PixelFormat format;
    if (!gdi::dib_pixel_format(*m_guest_memory, hdr, usage, {}, format)) return 0;
    const i32 w = hdr.width;
    const i32 h = hdr.abs_height();
    if (w <= 0 || h <= 0) return 0;
    // The bits follow the colour table (or the masks) of the packed DIB.
    const u32 table_bytes = hdr.color_count() * (usage == gdi::kDibPalColors ? 2u : (hdr.core() ? 3u : 4u));
    const GuestAddr bits = hdr.color_table() + table_bytes;
    auto pattern = std::make_shared<SoftBitmap>();
    pattern->width = w;
    pattern->height = h;
    pattern->pixels.assign(static_cast<usize>(w) * h * 4u, 0);
    std::vector<u8> row(hdr.stride());
    for (i32 r = 0; r < h; ++r) {
        if (m_guest_memory->read_bytes(bits + static_cast<GuestAddr>(r) * hdr.stride(), row.data(),
                                       row.size()).is_err()) {
            return 0;
        }
        const i32 y = hdr.top_down() ? r : h - 1 - r;
        gdi::decode_row(format, row.data(),
                        pattern->pixels.data() + static_cast<usize>(y) * w * 4u, w);
    }
    const Handle brush = allocate_gdi_handle(kHandleTypeGdiObject);
    m_brush_patterns[brush] = std::move(pattern);
    // LOGBRUSH of a DIB pattern brush: BS_DIBPATTERNPT, lbColor = usage,
    // lbHatch = the packed DIB (Learn: LOGBRUSH).
    m_brush_info[brush] = BrushInfo{6, usage, packed_dib};
    return brush;
}

}  // namespace vitreo::winapi
