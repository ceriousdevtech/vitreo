// SPDX-License-Identifier: LGPL-2.1-only

/// @file ddraw_palette.cpp
/// @brief IDirectDrawPalette: creation, lifetime and palette entry access.

#include "winapi/graphics/ddraw/ddraw.hpp"
#include "winapi/graphics/ddraw/ddraw_internal.hpp"
#include "graphics/contracts/indexed_color_table.hpp"
#include "graphics/contracts/cpu_surface.hpp"
#include "core/memory/guest_memory.hpp"

#include <array>
#include <memory>
#include <utility>

namespace vitreo::winapi {

using namespace ddraw_internal;

u32 DirectDrawManager::create_palette(GuestAddr object, u32 flags,
                                       GuestAddr entries, GuestAddr output,
                                       GuestAddr outer) {
    if (!m_memory || output == 0) return ddraw::E_INVALIDARG;
    (void)m_memory->write_u32(output, 0);
    if (!find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
    if (outer != 0) return ddraw::CLASS_E_NOAGGREGATION;
    constexpr u32 supported_caps = 0x00000004u | 0x00000040u;
    if ((flags & ~supported_caps) != 0 || (flags & 0x00000004u) == 0 || entries == 0) {
        return ddraw::E_INVALIDARG;
    }
    if (!ensure_storage()) return 0x8007000Eu;

    std::array<graphics::PaletteEntry, graphics::IndexedColorTable::ENTRY_COUNT> colors{};
    if (m_memory->read_bytes(entries, reinterpret_cast<Byte*>(colors.data()),
                             sizeof(colors)).is_err()) {
        return ddraw::E_INVALIDARG;
    }
    auto table = std::make_shared<graphics::IndexedColorTable>();
    if (!table->set_entries(0, graphics::IndexedColorTable::ENTRY_COUNT,
                            colors.data())) {
        return ddraw::E_INVALIDARG;
    }

    const GuestAddr palette = m_next_object;
    m_next_object += 0x20;
    if (write_pointer(palette, m_palette_vtable).is_err() ||
        m_memory->write_u32(output, static_cast<u32>(palette)).is_err()) {
        return 0x8007000Eu;
    }
    m_palettes.emplace(palette, PaletteObject{1, flags, std::move(table)});
    VITREO_TRACE("[ddraw] CreatePalette object=0x%llx flags=0x%x -> palette=0x%llx\n",
                 static_cast<unsigned long long>(object), flags,
                 static_cast<unsigned long long>(palette));
    return ddraw::DD_OK;
}

u32 DirectDrawManager::palette_query_interface(GuestAddr palette, GuestAddr iid,
                                                GuestAddr output) {
    if (!m_memory || output == 0) return ddraw::E_INVALIDARG;
    (void)m_memory->write_u32(output, 0);
    if (!find_palette(palette)) return ddraw::DDERR_INVALIDOBJECT;
    Guid requested{};
    if (iid == 0 || m_memory->read_bytes(
            iid, reinterpret_cast<Byte*>(&requested), sizeof(requested)).is_err()) {
        return ddraw::E_INVALIDARG;
    }
    constexpr Guid iid_palette{
        0x6C14DB84u, 0xA733, 0x11CE, {0xA5, 0x21, 0, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
    if (!guid_equal(requested, IID_IUNKNOWN) && !guid_equal(requested, iid_palette)) {
        return ddraw::E_NOINTERFACE;
    }
    (void)m_memory->write_u32(output, static_cast<u32>(palette));
    (void)palette_add_ref(palette);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::palette_add_ref(GuestAddr palette) {
    PaletteObject* state = find_palette(palette);
    return state ? ++state->ref_count : 0;
}

u32 DirectDrawManager::palette_release(GuestAddr palette) {
    PaletteObject* state = find_palette(palette);
    if (!state) return 0;
    const u32 result = --state->ref_count;
    if (result == 0) m_palettes.erase(palette);
    return result;
}

u32 DirectDrawManager::palette_get_caps(GuestAddr palette, GuestAddr caps) {
    PaletteObject* state = find_palette(palette);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    if (!m_memory || caps == 0 || m_memory->write_u32(caps, state->caps).is_err()) {
        return ddraw::E_INVALIDARG;
    }
    return ddraw::DD_OK;
}

u32 DirectDrawManager::palette_get_entries(GuestAddr palette, u32 flags,
                                            u32 start, u32 count,
                                            GuestAddr entries) {
    PaletteObject* state = find_palette(palette);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    const graphics::PaletteEntry* colors = state->colors->entries(start, count);
    if (flags != 0 || entries == 0 || !colors ||
        m_memory->write_bytes(entries, reinterpret_cast<const Byte*>(colors),
                              count * sizeof(*colors)).is_err()) {
        return ddraw::E_INVALIDARG;
    }
    return ddraw::DD_OK;
}

u32 DirectDrawManager::palette_set_entries(GuestAddr palette, u32 flags,
                                            u32 start, u32 count,
                                            GuestAddr entries) {
    PaletteObject* state = find_palette(palette);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    if (flags != 0 || entries == 0 || count > graphics::IndexedColorTable::ENTRY_COUNT) {
        return ddraw::E_INVALIDARG;
    }
    std::array<graphics::PaletteEntry, graphics::IndexedColorTable::ENTRY_COUNT> colors{};
    if (m_memory->read_bytes(entries, reinterpret_cast<Byte*>(colors.data()),
                             count * sizeof(colors[0])).is_err() ||
        !state->colors->set_entries(start, count, colors.data())) {
        return ddraw::E_INVALIDARG;
    }
    u32 dd_nonblack = 0;
    for (u32 i = 0; i < count && i < graphics::IndexedColorTable::ENTRY_COUNT; ++i)
        dd_nonblack += (colors[i].red || colors[i].green || colors[i].blue) ? 1 : 0;
    VITREO_TRACE("[ddraw] Palette::SetEntries palette=0x%llx start=%u count=%u nonblack=%u\n",
                 static_cast<unsigned long long>(palette), start, count, dd_nonblack);
    for (auto& [surface, surface_state] : m_surfaces) {
        if (surface_state.surface->palette() == state->colors) {
            present_surface(surface, surface_state);
        }
    }
    return ddraw::DD_OK;
}

} // namespace vitreo::winapi
