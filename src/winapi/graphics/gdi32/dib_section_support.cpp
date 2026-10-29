// SPDX-License-Identifier: LGPL-2.1-only

/// @file gdi32_dib.cpp
/// @brief Device-independent bitmaps.
///
/// CreateDIBSection, SetDIBits / GetDIBits, StretchDIBits, and the header
/// parsing they share. A DIB is a self-describing pixel buffer: a header giving
/// dimensions, bit depth and compression, an optional palette, then rows.
///
/// Two things about that layout are easy to get wrong and are handled here.
/// Rows are bottom-up unless the height is negative, so a positive height means
/// the first row in memory is the *last* row on screen. And in a BI_RGB 32bpp
/// DIB the fourth byte of each pixel is reserved, not alpha — carrying it
/// through as alpha renders every frame black.
///
/// Split out of gdi32.cpp. Closed in both directions once the ternary raster
/// operation codes moved to gdi32_internal.hpp, which is where the bitmap half
/// of the file needed them from too.

#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/graphics/gdi32/bitmap_codec.hpp"
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

/// @file dib_section_support.cpp
/// @brief DIB Section Support
///
/// Closed cut from gdi32_dib.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

namespace {

/// One pre-main environment lookup. The DIB syncs below run on every blit, and
/// getenv locks and walks the whole environment block each call.
const bool g_gdi_diag = std::getenv("VITREO_GDI_DIAG") != nullptr;


} // namespace

// =============================================================================
// DIB Section Support
// =============================================================================

GuestAddr GdiManager::dib_scratch_alloc(u32 size) noexcept {
    ApiLock api_lock(*this);
    size = (size + 3) & ~3u; // 4-byte align
    if (size == 0) size = 4;

    // Reuse a freed block first (first fit). Without this the region is a pure
    // bump allocator: an app that creates a DIB per frame burns through it and
    // every subsequent CreateDIBSection returns 0, blanking the window.
    for (auto it = m_dib_scratch_free.begin(); it != m_dib_scratch_free.end(); ++it) {
        if (it->second < size) continue;
        const GuestAddr addr = it->first;
        if (it->second == size) {
            m_dib_scratch_free.erase(it);
        } else {
            it->first += size;
            it->second -= size;
        }
        return addr;
    }

    if (m_dib_scratch_offset + size > GDI_DIB_SCRATCH_SIZE) {
        // Too big for (or too little left in) the fixed arena. A full-screen
        // 32-bpp DIB is 14 MB at 2560x1440 and 33 MB at 4K, so the arena can
        // never satisfy one; give it a dedicated mapping rather than failing
        // CreateDIBSection, which a caller reads as "no video mode available".
        if (!m_guest_memory) return 0;
        const u32 rounded = (size + 0xFFFFu) & ~0xFFFFu;
        const GuestAddr base =
            m_guest_memory->find_free_gap(0x63000000, rounded);
        if (base == 0 ||
            m_guest_memory->map_region(base, rounded,
                                       MemoryProtection::ReadWrite).is_err()) {
            VITREO_TRACE("[gdi] DIB allocation of %u bytes failed: no free "
                         "region\n", size);
            return 0;
        }
        m_dib_large_blocks.emplace(base, rounded);
        VITREO_TRACE("[gdi] DIB block %u bytes mapped at 0x%llx (outside the "
                     "scratch arena)\n", size,
                     static_cast<unsigned long long>(base));
        return base;
    }
    GuestAddr addr = GDI_DIB_SCRATCH_BASE + m_dib_scratch_offset;
    m_dib_scratch_offset += size;
    return addr;
}

void GdiManager::dib_scratch_free(GuestAddr addr, u32 size) noexcept {
    ApiLock api_lock(*this);
    // Dedicated large blocks are unmapped outright rather than returned to the
    // arena's free list, which only tracks arena-relative extents.
    if (auto large = m_dib_large_blocks.find(addr);
        large != m_dib_large_blocks.end()) {
        if (m_guest_memory) {
            (void)m_guest_memory->unmap_region(addr, large->second);
        }
        m_dib_large_blocks.erase(large);
        return;
    }
    if (addr < GDI_DIB_SCRATCH_BASE ||
        addr >= GDI_DIB_SCRATCH_BASE + GDI_DIB_SCRATCH_SIZE) {
        return;
    }
    size = (size + 3) & ~3u;
    if (size == 0) return;

    // If this block sits at the bump tip, simply retract the tip so the
    // common allocate/free-in-order pattern costs nothing.
    if (addr + size == GDI_DIB_SCRATCH_BASE + m_dib_scratch_offset) {
        m_dib_scratch_offset -= size;
        // Absorb any free blocks that are now also at the tip.
        bool merged = true;
        while (merged) {
            merged = false;
            for (auto it = m_dib_scratch_free.begin();
                 it != m_dib_scratch_free.end(); ++it) {
                if (it->first + it->second ==
                    GDI_DIB_SCRATCH_BASE + m_dib_scratch_offset) {
                    m_dib_scratch_offset -= it->second;
                    m_dib_scratch_free.erase(it);
                    merged = true;
                    break;
                }
            }
        }
        return;
    }

    // Insert sorted by address, then coalesce with either neighbour.
    auto pos = std::lower_bound(
        m_dib_scratch_free.begin(), m_dib_scratch_free.end(), addr,
        [](const std::pair<GuestAddr, u32>& b, GuestAddr a) { return b.first < a; });
    auto it = m_dib_scratch_free.insert(pos, {addr, size});

    if (it + 1 != m_dib_scratch_free.end() &&
        it->first + it->second == (it + 1)->first) {
        it->second += (it + 1)->second;
        m_dib_scratch_free.erase(it + 1);
    }
    if (it != m_dib_scratch_free.begin()) {
        auto prev = it - 1;
        if (prev->first + prev->second == it->first) {
            prev->second += it->second;
            m_dib_scratch_free.erase(it);
        }
    }
}

Handle GdiManager::make_dib_section(i32 width, i32 height, bool bottom_up, u16 bpp,
                                    u32 compression, const std::array<u32, 3>& masks,
                                    std::vector<u8> color_table, Handle logical_palette,
                                    std::vector<u16> palette_indices, u32 clr_used,
                                    u32 clr_important, u64 section, u32 offset,
                                    GuestAddr* bits_out, u32* error) {
    ApiLock api_lock(*this);
    constexpr u32 ERROR_NOT_ENOUGH_MEMORY_CODE = 8;
    constexpr u32 ERROR_INVALID_PARAMETER_CODE = 87;
    auto fail = [&](u32 code) {
        if (error) *error = code;
        return Handle{0};
    };
    const u64 stride = gdi::dib_stride(width, bpp);
    const u64 size = stride * static_cast<u64>(height);
    if (size > 0x7FFFFFFFu ||
        static_cast<u64>(width) * static_cast<u64>(height) * 4u > (u64{1} << 31)) {
        return fail(ERROR_NOT_ENOUGH_MEMORY_CODE);
    }

    GuestAddr bits_addr = 0;
    GuestAddr view = 0;
    if (section != 0) {
        // Learn (CreateDIBSection): the bits live at dwOffset in the
        // file-mapping object, which must be DWORD aligned; the caller keeps
        // ownership of the section handle.
        // A misaligned offset fails with the last error untouched; a section
        // that is too small or read-only fails with 87 (s3gdi misc probe).
        if ((offset & 3u) != 0) return fail(0);
        if (!m_map_section_view) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED gdi32!CreateDIBSection: "
                                     "hSection without a section mapper\n");
            }
            return fail(ERROR_INVALID_PARAMETER_CODE);
        }
        const u64 aligned = static_cast<u64>(offset) & ~u64{0xFFFF};
        const auto mapper = m_map_section_view;
        {
            ApiUnlock unlocked(*this);
            view = mapper(section, aligned, size + (offset - aligned));
        }
        if (view == 0) return fail(ERROR_INVALID_PARAMETER_CODE);
        bits_addr = view + static_cast<GuestAddr>(offset - aligned);
    } else {
        bits_addr = dib_scratch_alloc(static_cast<u32>(size));
        if (bits_addr == 0) {
            VITREO_TRACE("[gdi] CreateDIBSection: scratch_alloc failed for %llu bytes\n",
                         static_cast<unsigned long long>(size));
            return fail(ERROR_NOT_ENOUGH_MEMORY_CODE);
        }
        // Windows zero-fills a DIB section's pixel buffer. For 32bpp that
        // means fully transparent black, which is what a skinned app relies
        // on: it draws one element into a fresh DIB and composites it with
        // AlphaBlend, so any untouched pixel must contribute nothing.
        const std::vector<u8> zeros(static_cast<usize>(size), 0);
        (void)m_guest_memory->write_bytes(bits_addr, zeros.data(), zeros.size());
    }

    const Handle h = allocate_gdi_handle(kHandleTypeGdiObject);
    auto bmp = std::make_shared<SoftBitmap>();
    bmp->width = width;
    bmp->height = height;
    bmp->bpp = bpp;
    bmp->pixels.assign(static_cast<usize>(width) * height * 4u, 0);
    m_bitmap_objects[h] = bmp;

    DIBSectionInfo info;
    info.guest_bits = bits_addr;
    info.width = width;
    info.height = height;
    info.bpp = bpp;
    info.stride = static_cast<u32>(stride);
    info.bottom_up = bottom_up;
    info.compression = compression;
    info.masks = masks;
    info.clr_used = clr_used;
    info.clr_important = clr_important;
    info.section = section;
    info.offset = offset;
    info.color_table = std::move(color_table);
    info.logical_palette = logical_palette;
    info.palette_indices = std::move(palette_indices);
    m_dib_sections[h] = std::move(info);
    if (view != 0) {
        m_dib_section_views[h] = view;
        sync_dib_from_guest(h);  // a section arrives with its own contents
    }
    if (bits_out) *bits_out = bits_addr;
    VITREO_TRACE("[gdi] CreateDIBSection: %dx%d bpp=%u comp=%u stride=%llu bits=0x%llx "
                 "section=0x%llx handle=0x%x\n",
                 width, height, bpp, compression, static_cast<unsigned long long>(stride),
                 static_cast<unsigned long long>(bits_addr),
                 static_cast<unsigned long long>(section), h);
    return h;
}

Handle GdiManager::create_dib_section(Handle hdc, GuestAddr bmi_addr, u32 usage,
                                      GuestAddr ppvBits, bool guest_is_64bit,
                                      u64 section, u32 offset, u32* error) {
    ApiLock api_lock(*this);
    constexpr u32 ERROR_INVALID_PARAMETER_CODE = 87;
    // Learn: on failure *ppvBits is NULL (s3gdi_bitmap_probe "cds": every
    // failure writes NULL). ppvBits is VOID**, so x64 writes eight bytes.
    auto write_bits = [&](GuestAddr value) {
        if (ppvBits == 0 || !m_guest_memory) return true;
        return guest_is_64bit
            ? m_guest_memory->write_u64(ppvBits, static_cast<u64>(value)).is_ok()
            : m_guest_memory->write_u32(ppvBits, static_cast<u32>(value)).is_ok();
    };
    auto fail = [&](u32 code) {
        (void)write_bits(0);
        if (error) *error = code;
        return Handle{0};
    };
    if (!m_guest_memory) return fail(0);
    gdi::DibHeader hdr;
    if (!gdi::read_dib_header(*m_guest_memory, bmi_addr, hdr)) return fail(0);
    if (hdr.width <= 0 || hdr.height == 0) return fail(0);
    switch (hdr.bpp) {
    case 1: case 4: case 8: case 24:
        if (hdr.compression != gdi::kBiRgb) return fail(0);
        break;
    case 16: case 32:
        if (hdr.compression == gdi::kBiBitfields) {
            if ((hdr.masks[0] | hdr.masks[1] | hdr.masks[2]) == 0) {
                return fail(ERROR_INVALID_PARAMETER_CODE);
            }
        } else if (hdr.compression != gdi::kBiRgb) {
            return fail(0);
        }
        break;
    default:
        return fail(0);
    }
    if (usage > gdi::kDibPalColors) return fail(ERROR_INVALID_PARAMETER_CODE);

    gdi::PixelFormat format;
    if (!gdi::dib_pixel_format(*m_guest_memory, hdr, usage, dc_logical_palette(hdc), format)) {
        return fail(ERROR_INVALID_PARAMETER_CODE);
    }
    std::vector<u8> color_table;
    for (const u32 c : format.palette) {
        color_table.push_back(static_cast<u8>(c));
        color_table.push_back(static_cast<u8>(c >> 8));
        color_table.push_back(static_cast<u8>(c >> 16));
        color_table.push_back(0);
    }
    std::array<u32, 3> masks{};
    if (hdr.compression == gdi::kBiBitfields) {
        masks = hdr.masks;
    } else if (hdr.bpp == 16) {
        masks = {0x7C00, 0x03E0, 0x001F};  // dsBitfields of a BI_RGB 16bpp DIB
    }
    const u32 entries = static_cast<u32>(format.palette.size());
    GuestAddr bits_addr = 0;
    const Handle h = make_dib_section(hdr.width, hdr.abs_height(), !hdr.top_down(), hdr.bpp,
                                      hdr.compression, masks, std::move(color_table), 0, {},
                                      entries, entries, section, offset, &bits_addr, error);
    if (h == 0) {
        (void)write_bits(0);
        return 0;
    }
    if (!write_bits(bits_addr)) {
        VITREO_TRACE("[gdi] CreateDIBSection: ppvBits 0x%llx unwritable\n",
                     static_cast<unsigned long long>(ppvBits));
    }
    return h;
}

Handle GdiManager::create_runtime_dib_section(i32 width, i32 height, bool bottom_up, u16 bpp,
                                              GuestAddr* bits_out) {
    if (width <= 0 || height <= 0 || (bpp != 32 && bpp != 1)) return 0;
    std::vector<u8> color_table;
    u32 entries = 0;
    if (bpp == 1) {
        color_table = {0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0};
        entries = 2;
    }
    return make_dib_section(width, height, bottom_up, bpp, gdi::kBiRgb, {}, std::move(color_table), 0, {},
                            entries, entries, 0, 0, bits_out, nullptr);
}

void GdiManager::sync_dib_from_guest(Handle bitmap_handle) {
    ApiLock api_lock(*this);
    auto dib_it = m_dib_sections.find(bitmap_handle);
    if (dib_it == m_dib_sections.end() || !m_guest_memory) return;
    auto bmp_it = m_bitmap_objects.find(bitmap_handle);
    if (bmp_it == m_bitmap_objects.end() || !bmp_it->second) return;
    auto& dib = dib_it->second;
    auto& bmp = bmp_it->second;

    const u32 pixel_data_size = dib.stride * static_cast<u32>(dib.height);
    // The DIB bits live in guest memory, so a fully mapped range can be read in
    // place. Copying them into a fresh vector on every blit cost a heap
    // allocation, a zero-fill and a full-surface memcpy per frame.
    std::vector<u8> staging;
    const u8* pixel_data = m_guest_memory->host_ptr(dib.guest_bits, pixel_data_size);
    if (pixel_data == nullptr) {
        staging.resize(pixel_data_size);
        if (m_guest_memory->read_bytes(dib.guest_bits, staging.data(), pixel_data_size).is_err()) {
            return;
        }
        pixel_data = staging.data();
    }
    if (g_gdi_diag) {
        usize probe = 0;
        for (; probe + 3 < pixel_data_size; probe += 4) {
            if ((pixel_data[probe] | pixel_data[probe + 1] | pixel_data[probe + 2]) != 0) break;
        }
        VITREO_TRACE("[gdi-diag] sync_dib bmp=0x%x bpp=%u first non-black byte %zu of %u\n",
                     bitmap_handle, dib.bpp, probe, pixel_data_size);
    }

    const gdi::PixelFormat format = dib_pixel_format_of(dib);
    const i32 copy_w = std::min(dib.width, bmp->width);
    const i32 copy_h = std::min(dib.height, bmp->height);
    if (copy_w <= 0 || copy_h <= 0) return;
    bmp->dirty = true;
    for (i32 row = 0; row < copy_h; ++row) {
        // For bottom-up DIBs buffer row 0 is the bottom scan line.
        const i32 src_row = dib.bottom_up ? (dib.height - 1 - row) : row;
        gdi::decode_row(format, pixel_data + static_cast<usize>(src_row) * dib.stride,
                        bmp->pixels.data() + static_cast<usize>(row) * bmp->width * 4u, copy_w);
    }
}

void GdiManager::sync_dib_to_guest(Handle bitmap_handle) {
    ApiLock api_lock(*this);
    auto dib_it = m_dib_sections.find(bitmap_handle);
    auto bmp_it = m_bitmap_objects.find(bitmap_handle);
    if (dib_it == m_dib_sections.end() || bmp_it == m_bitmap_objects.end() || !m_guest_memory) {
        return;
    }
    const auto& dib = dib_it->second;
    const auto& bmp = bmp_it->second;
    const u32 pixel_data_size = dib.stride * static_cast<u32>(dib.height);
    // Write straight into guest memory when the range is mapped contiguously.
    std::vector<u8> staging;
    u8* pixel_data = m_guest_memory->host_ptr_mut(dib.guest_bits, pixel_data_size);
    const bool write_back = (pixel_data == nullptr);
    if (write_back) {
        staging.resize(pixel_data_size);
        if (m_guest_memory->read_bytes(dib.guest_bits, staging.data(), pixel_data_size).is_err()) {
            return;
        }
        pixel_data = staging.data();
    }
    const gdi::PixelFormat format = dib_pixel_format_of(dib);
    const i32 copy_w = std::min(dib.width, bmp->width);
    const i32 copy_h = std::min(dib.height, bmp->height);
    for (i32 row = 0; row < copy_h; ++row) {
        const i32 dst_row = dib.bottom_up ? dib.height - 1 - row : row;
        gdi::encode_row(format, bmp->pixels.data() + static_cast<usize>(row) * bmp->width * 4u,
                        pixel_data + static_cast<usize>(dst_row) * dib.stride, copy_w);
    }
    if (write_back) {
        (void)m_guest_memory->write_bytes(dib.guest_bits, pixel_data, pixel_data_size);
    }
}

gdi::PixelFormat GdiManager::dib_pixel_format_of(const DIBSectionInfo& dib) const {
    ApiLock api_lock(*this);
    gdi::PixelFormat format;
    format.bpp = dib.bpp;
    format.bitfields = dib.compression == gdi::kBiBitfields && (dib.bpp == 16 || dib.bpp == 32);
    format.masks = dib.masks;
    if (dib.bpp <= 8) {
        const PaletteInfo* logical = nullptr;
        if (dib.logical_palette != 0) {
            if (const auto it = m_palette_objects.find(dib.logical_palette);
                it != m_palette_objects.end()) {
                logical = &it->second;
            }
        }
        const u32 entries = static_cast<u32>(dib.color_table.size() / 4u);
        for (u32 i = 0; i < entries; ++i) {
            if (logical && i < dib.palette_indices.size() &&
                dib.palette_indices[i] < logical->entries.size()) {
                const auto& e = logical->entries[dib.palette_indices[i]];
                format.palette.push_back((static_cast<u32>(e.red) << 16) |
                                         (static_cast<u32>(e.green) << 8) | e.blue);
                continue;
            }
            const u8* q = dib.color_table.data() + i * 4u;
            format.palette.push_back((static_cast<u32>(q[2]) << 16) |
                                     (static_cast<u32>(q[1]) << 8) | q[0]);
        }
    }
    return format;
}

bool GdiManager::copy_bitmap_rgba(Handle bitmap, BitmapRgbaCopy& out) {
    ApiLock api_lock(*this);
    out = {};
    if (m_dib_sections.find(bitmap) != m_dib_sections.end()) {
        sync_dib_from_guest(bitmap);
    }
    std::shared_ptr<SoftBitmap> bmp = bitmap_surface(bitmap);
    if (!bmp || bmp->width <= 0 || bmp->height <= 0 || bmp->pixels.empty()) {
        return false;
    }
    const usize expected =
        static_cast<usize>(bmp->width) * static_cast<usize>(bmp->height) * 4u;
    if (bmp->pixels.size() < expected) {
        return false;
    }
    out.width = bmp->width;
    out.height = bmp->height;
    out.rgba.assign(bmp->pixels.begin(), bmp->pixels.begin() + static_cast<std::ptrdiff_t>(expected));
    if (const auto dib = m_dib_sections.find(bitmap); dib != m_dib_sections.end()) {
        out.bpp = dib->second.bpp;
        out.from_dib = true;
    } else {
        out.bpp = bmp->bpp;
        out.from_dib = false;
    }
    return true;
}

} // namespace vitreo::winapi
