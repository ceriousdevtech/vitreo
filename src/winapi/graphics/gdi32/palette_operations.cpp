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

    if (numEntries == 0 || numEntries > 256) {
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
    if (dc_it == m_dc_states.end()) return 0;

    Handle prev = dc_it->second.selected_palette;
    dc_it->second.selected_palette = hPal;

    VITREO_TRACE("[gdi] SelectPalette: hdc=0x%x hPal=0x%x prev=0x%x\n", hdc, hPal, prev);
    return prev;
}

u32 GdiManager::realize_palette(Handle hdc) {
    ApiLock api_lock(*this);
    auto dc_it = m_dc_states.find(hdc);
    if (dc_it == m_dc_states.end()) return 0;

    Handle hPal = dc_it->second.selected_palette;
    if (hPal == 0) return 0;

    // Look up the palette
    auto pal_it = m_palette_objects.find(hPal);
    if (pal_it == m_palette_objects.end()) {
        // Could be a stock palette
        if (m_stock_handles.count(hPal)) return 0;
        return 0;
    }

    // In true-color mode, we don't actually need to map palette entries.
    // Just return the number of entries as if they were all mapped.
    u32 mapped = static_cast<u32>(pal_it->second.entries.size());
    VITREO_TRACE("[gdi] RealizePalette: hdc=0x%x hPal=0x%x mapped=%u\n", hdc, hPal, mapped);
    return mapped;
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

u32 GdiManager::get_system_palette_entries(Handle /*hdc*/, u32 iStart, u32 cEntries, GuestAddr pPE) {
    ApiLock api_lock(*this);
    // The 256-entry system palette: its static entries (first and last ten)
    // are the default palette's colours (Learn, "System Palette"), and the
    // whole table is the one GetDIBits synthesises for 8bpp
    // (default_palettes.hpp).
    constexpr u32 SYSTEM_PALETTE_SIZE = 256;

    if (pPE == 0) return SYSTEM_PALETTE_SIZE;
    if (iStart >= SYSTEM_PALETTE_SIZE) return 0;
    if (!m_guest_memory) return 0;

    u32 available = SYSTEM_PALETTE_SIZE - iStart;
    u32 toCopy = std::min(cEntries, available);

    std::vector<u8> buf(toCopy * 4);
    for (u32 i = 0; i < toCopy; ++i) {
        const u32 c = gdi::kGdiDefaultPalette8[iStart + i];
        buf[i * 4 + 0] = static_cast<u8>(c >> 16);  // peRed
        buf[i * 4 + 1] = static_cast<u8>(c >> 8);   // peGreen
        buf[i * 4 + 2] = static_cast<u8>(c);        // peBlue
        buf[i * 4 + 3] = 0;                         // peFlags
    }
    m_guest_memory->write_bytes(pPE, buf.data(), toCopy * 4);

    VITREO_TRACE("[gdi] GetSystemPaletteEntries: start=%u count=%u\n", iStart, toCopy);
    return toCopy;
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
