// SPDX-License-Identifier: LGPL-2.1-only

/// @file gdi32.cpp
/// @brief GDI32 device context stubs (WIN32-063)

#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/graphics/gdi32/default_palettes.hpp"
#include "winapi/common/display_device_identity.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "core/memory/guest_memory.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "ipc/hwnd_registry.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "winapi/graphics/gdi32/palette_contract.inc"
#include <atomic>
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

/// @file palette_operations.cpp
/// @brief Palette Operations
///
/// Closed cut from gdi32.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Palette Operations
// =============================================================================

Handle GdiManager::create_palette(GuestAddr lpLogPalette) {
    ApiLock api_lock(*this);
    if (!m_guest_memory || lpLogPalette == 0) return 0;

    // Read LOGPALETTE header (4 bytes: palVersion u16, palNumEntries u16)
    u8 header[4];
    auto rd = m_guest_memory->read_bytes(lpLogPalette, reinterpret_cast<Byte*>(header), 4);
    if (rd.is_err()) return 0;

    u16 version = static_cast<u16>(header[0] | (header[1] << 8));
    u16 numEntries = static_cast<u16>(header[2] | (header[3] << 8));

    if (version != palette_contract::logical_version || numEntries == 0) {
        VITREO_TRACE("[gdi] CreatePalette: invalid entry count %u\n", numEntries);
        return 0;
    }

    // Read PALETTEENTRY array (4 bytes each: R, G, B, flags)
    std::vector<u8> entryData(numEntries * 4);
    auto pe_rd = m_guest_memory->read_bytes(
        lpLogPalette + 4, reinterpret_cast<Byte*>(entryData.data()),
        static_cast<usize>(numEntries * 4));
    if (pe_rd.is_err()) return 0;

    // Allocate palette handle and store entries
    Handle h = allocate_gdi_handle(kHandleTypeGdiObject);
    PaletteInfo& pal = m_palette_objects[h];
    pal.version = version;
    pal.entries.resize(numEntries);

    for (u16 i = 0; i < numEntries; ++i) {
        pal.entries[i].red   = entryData[i * 4 + 0];
        pal.entries[i].green = entryData[i * 4 + 1];
        pal.entries[i].blue  = entryData[i * 4 + 2];
        pal.entries[i].flags = entryData[i * 4 + 3];
    }

    VITREO_TRACE("[gdi] CreatePalette: handle=0x%x entries=%u\n", h, numEntries);
    return h;
}

Handle GdiManager::create_halftone_palette(Handle hdc) {
    ApiLock api_lock(*this);
    if (m_dc_states.find(hdc) == m_dc_states.end()) {
        return 0;
    }

    Handle h = allocate_gdi_handle(kHandleTypeGdiObject);
    PaletteInfo& pal = m_palette_objects[h];
    pal.version = 0x0300;
    pal.entries.resize(256);

    // Windows HALFTONE palette: 6×6×6 cube (216) followed by a 40-step grey ramp.
    u32 idx = 0;
    for (u32 r = 0; r < 6; ++r) {
        for (u32 g = 0; g < 6; ++g) {
            for (u32 b = 0; b < 6; ++b) {
                pal.entries[idx].red = static_cast<u8>(r * 51);
                pal.entries[idx].green = static_cast<u8>(g * 51);
                pal.entries[idx].blue = static_cast<u8>(b * 51);
                pal.entries[idx].flags = 0;
                ++idx;
            }
        }
    }
    for (u32 i = 0; i < 40; ++i) {
        const u8 gray = static_cast<u8>((i * 255) / 39);
        pal.entries[idx].red = gray;
        pal.entries[idx].green = gray;
        pal.entries[idx].blue = gray;
        pal.entries[idx].flags = 0;
        ++idx;
    }

    VITREO_TRACE("[gdi] CreateHalftonePalette: hdc=0x%x handle=0x%x\n", hdc, h);
    return h;
}

u32 GdiManager::get_dib_color_table(Handle hdc, u32 iStart, u32 cEntries,
                                    GuestAddr prgbq) {
    ApiLock api_lock(*this);
    auto dc_it = m_dc_states.find(hdc);
    if (dc_it == m_dc_states.end() || prgbq == 0 || cEntries == 0) {
        return 0;
    }

    const Handle bitmap = dc_it->second.selected_bitmap_handle;
    auto dib_it = m_dib_sections.find(bitmap);
    if (dib_it == m_dib_sections.end()) {
        return 0;
    }

    const DIBSectionInfo& dib = dib_it->second;
    if (dib.bpp > 8 || dib.color_table.size() < 4) {
        return 0;
    }

    const u32 table_count = static_cast<u32>(dib.color_table.size() / 4);
    if (iStart >= table_count) {
        return 0;
    }
    if (!m_guest_memory) {
        return 0;
    }

    const u32 to_copy = std::min(cEntries, table_count - iStart);
    m_guest_memory->write_bytes(
        prgbq, dib.color_table.data() + static_cast<usize>(iStart) * 4,
        static_cast<usize>(to_copy) * 4);
    VITREO_TRACE("[gdi] GetDIBColorTable: hdc=0x%x start=%u count=%u\n",
                 hdc, iStart, to_copy);
    return to_copy;
}

Handle GdiManager::select_palette(Handle hdc, Handle hPal, bool /*forceBackground*/) {
    ApiLock api_lock(*this);
    auto dc_it = m_dc_states.find(hdc);
    if (dc_it == m_dc_states.end()) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return 0;
    }
    const auto stock = m_stock_objects.find(DEFAULT_PALETTE_ID);
    if (!m_palette_objects.contains(hPal) &&
        (stock == m_stock_objects.end() || stock->second != hPal)) return 0;

    Handle prev = dc_it->second.selected_palette;
    dc_it->second.selected_palette = hPal;

    VITREO_TRACE("[gdi] SelectPalette: hdc=0x%x hPal=0x%x prev=0x%x\n", hdc, hPal, prev);
    return prev;
}

u32 GdiManager::realize_palette(Handle hdc) {
    ApiLock api_lock(*this);
    if (!m_dc_states.contains(hdc)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return palette_edges_sdk::GDI_ERROR;
    }
    // This runtime exposes a truecolor display (no RC_PALETTE). Windows maps
    // no entries, including when an indexed DIB is selected into a memory DC;
    // its DIB color table is unchanged. See palette_edges{32,64}.expected.
    return 0;
}

bool GdiManager::unrealize_object(Handle object) {
    ApiLock api_lock(*this);
    // Brush unrealization is a documented no-op. The Windows measurements
    // include deleted brush identities; forget that identity on handle reuse.
    const auto type = object_type(object);
    return type == palette_edges_sdk::OBJ_BRUSH ||
           type == palette_edges_sdk::OBJ_PAL ||
           (type == 0 && m_retired_brushes.contains(object));
}

u32 GdiManager::get_palette_entries(Handle hPal, u32 iStart, u32 cEntries, GuestAddr pPE) {
    ApiLock api_lock(*this);
    // If pPE is NULL, return the number of entries in the palette
    auto pal_it = m_palette_objects.find(hPal);
    if (pal_it == m_palette_objects.end()) {
        // DEFAULT_PALETTE: the 20 static colours (Learn, "Default Palette";
        // default_palettes.hpp), peFlags 0.
        const auto stock = m_stock_objects.find(DEFAULT_PALETTE_ID);
        if (stock == m_stock_objects.end() || stock->second != hPal) return 0;
        const u32 count = static_cast<u32>(gdi::kGdiStaticColors.size());
        if (pPE == 0) return count;
        if (iStart >= count || !m_guest_memory) return 0;
        const u32 n = std::min(cEntries, count - iStart);
        std::vector<u8> buf(n * 4);
        for (u32 i = 0; i < n; ++i) {
            const u32 c = gdi::kGdiStaticColors[iStart + i];
            buf[i * 4 + 0] = static_cast<u8>(c >> 16);
            buf[i * 4 + 1] = static_cast<u8>(c >> 8);
            buf[i * 4 + 2] = static_cast<u8>(c);
            buf[i * 4 + 3] = 0;
        }
        if (n != 0) m_guest_memory->write_bytes(pPE, buf.data(), n * 4);
        return n;
    }

    u32 numEntries = static_cast<u32>(pal_it->second.entries.size());
    if (pPE == 0) return numEntries;

    if (iStart >= numEntries) return 0;
    u32 available = numEntries - iStart;
    u32 toCopy = std::min(cEntries, available);

    if (!m_guest_memory || toCopy == 0) return 0;

    // Write PALETTEENTRY array to guest memory
    std::vector<u8> buf(toCopy * 4);
    for (u32 i = 0; i < toCopy; ++i) {
        const auto& e = pal_it->second.entries[iStart + i];
        buf[i * 4 + 0] = e.red;
        buf[i * 4 + 1] = e.green;
        buf[i * 4 + 2] = e.blue;
        buf[i * 4 + 3] = e.flags;
    }
    m_guest_memory->write_bytes(pPE, buf.data(), toCopy * 4);

    VITREO_TRACE("[gdi] GetPaletteEntries: hPal=0x%x start=%u count=%u\n", hPal, iStart, toCopy);
    return toCopy;
}

u32 GdiManager::set_palette_entries(Handle hPal, u32 iStart, u32 cEntries, GuestAddr pPE) {
    ApiLock api_lock(*this);
    auto pal_it = m_palette_objects.find(hPal);
    if (pal_it == m_palette_objects.end()) return 0;
    if (!m_guest_memory || pPE == 0 || cEntries == 0) return 0;

    u32 numEntries = static_cast<u32>(pal_it->second.entries.size());
    if (iStart >= numEntries) return 0;
    u32 available = numEntries - iStart;
    u32 toSet = std::min(cEntries, available);

    // Read PALETTEENTRY array from guest memory
    std::vector<u8> buf(toSet * 4);
    auto rd = m_guest_memory->read_bytes(pPE, reinterpret_cast<Byte*>(buf.data()),
                                          static_cast<usize>(toSet * 4));
    if (rd.is_err()) return 0;

    for (u32 i = 0; i < toSet; ++i) {
        auto& e = pal_it->second.entries[iStart + i];
        e.red   = buf[i * 4 + 0];
        e.green = buf[i * 4 + 1];
        e.blue  = buf[i * 4 + 2];
        e.flags = buf[i * 4 + 3];
    }

    u32 gdi_nonblack = 0;
    for (u32 i = 0; i < toSet; ++i)
        gdi_nonblack += (buf[i * 4] || buf[i * 4 + 1] || buf[i * 4 + 2]) ? 1 : 0;
    VITREO_TRACE("[gdi] SetPaletteEntries: hPal=0x%x start=%u count=%u nonblack=%u\n",
                 hPal, iStart, toSet, gdi_nonblack);
    return toSet;
}

bool GdiManager::resize_palette(Handle hPal, u32 cEntries) {
    ApiLock api_lock(*this);
    auto pal_it = m_palette_objects.find(hPal);
    if (pal_it == m_palette_objects.end()) return false;
    if (cEntries == 0 || cEntries > 256) return false;

    pal_it->second.entries.resize(cEntries);
    VITREO_TRACE("[gdi] ResizePalette: hPal=0x%x newSize=%u\n", hPal, cEntries);
    return true;
}

u32 GdiManager::get_system_palette_entries(Handle hdc, u32 iStart, u32 cEntries,
                                                   GuestAddr pPE, Marshaler* marshaler) {
    ApiLock api_lock(*this);
    constexpr u32 count = std::size(palette_edges_oracle::system_entries);
    // The measured overflowing count is rejected before HDC or memory access.
    if (cEntries == palette_contract::measured_no_copy_count) return 0;
    if (cEntries > count) {
        static std::atomic_flag warned = ATOMIC_FLAG_INIT;
        if (!warned.test_and_set()) std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED gdi32!GetSystemPaletteEntries: oversized request\n");
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }
    const u32 n = iStart < count ? std::min(cEntries, count - iStart) : 0;
    if (pPE && n && m_guest_memory) {
        const auto* bytes = reinterpret_cast<const Byte*>(
            palette_edges_oracle::system_entries[iStart]);
        const usize size = static_cast<usize>(n) * palette_edges_sdk::PALETTEENTRY_SIZE;
        const bool x64 = marshaler && marshaler->cpu_is_64bit();
        // Windows' x64 copy preflights the whole range; x86 copies entries
        // before a later page fault. Both preserve LastError on a write fault.
        const usize step = x64 ? size : palette_edges_sdk::PALETTEENTRY_SIZE;
        for (usize offset = 0; offset < size; offset += step) {
            if (!m_guest_memory->is_range_writable(pPE + offset, step, x64)) {
                GuestAddr bad = pPE + offset;
                for (usize i = 0; i < step; ++i) {
                    if (!m_guest_memory->is_range_writable(pPE + offset + i, 1, x64)) {
                        bad = pPE + offset + i; break;
                    }
                }
                if (marshaler) marshaler->raise_access_violation(true, bad);
                return 0;
            }
            if (m_guest_memory->write_bytes(pPE + offset, bytes + offset, step).is_err()) {
                if (marshaler) marshaler->raise_access_violation(true, pPE + offset);
                return 0;
            }
        }
    }
    // The truecolor Windows profile writes its static system palette even
    // for a bad HDC, then reports invalid handle; valid DCs return zero.
    if (!m_dc_states.contains(hdc)) set_last_error(error_codes::ERROR_INVALID_HANDLE);
    return 0;
}

u32 GdiManager::set_system_palette_use(Handle /*hdc*/, u32 uUsage) {
    ApiLock api_lock(*this);
    // SYSPAL_STATIC = 1, SYSPAL_NOSTATIC = 2, SYSPAL_NOSTATIC256 = 3
    u32 prev = m_system_palette_use;
    if (uUsage >= 1 && uUsage <= 3) {
        m_system_palette_use = uUsage;
    }
    VITREO_TRACE("[gdi] SetSystemPaletteUse: usage=%u prev=%u\n", uUsage, prev);
    return prev;
}

u32 GdiManager::get_system_palette_use(Handle hdc) const {
    ApiLock api_lock(*this);
    if (m_dc_states.find(hdc) == m_dc_states.end()) return 0;
    VITREO_TRACE("[gdi] GetSystemPaletteUse: hdc=0x%x usage=%u\n",
                 hdc, m_system_palette_use);
    return m_system_palette_use;
}

} // namespace vitreo::winapi
