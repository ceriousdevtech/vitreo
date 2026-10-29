// SPDX-License-Identifier: LGPL-2.1-only

/// @file ddraw.cpp
/// @brief DirectDrawManager core: vtable storage, object lookup, creation and
///        IUnknown lifetime.

#include "winapi/graphics/ddraw/ddraw.hpp"
#include "winapi/graphics/ddraw/ddraw_internal.hpp"
#include "graphics/contracts/presentation_state.hpp"
#include "core/memory/guest_memory.hpp"

#include <array>

namespace vitreo::winapi {

using namespace ddraw_internal;

namespace {

constexpr usize STORAGE_SIZE = 0x10000;

} // namespace

VoidResult DirectDrawManager::write_pointer(GuestAddr address, GuestAddr value) {
    return m_is_64bit ? m_memory->write_u64(address, value)
                     : m_memory->write_u32(address, static_cast<u32>(value));
}

bool DirectDrawManager::ensure_storage() {
    if (m_region_base != 0) return true;
    if (!m_memory || !m_thunks) return false;

    const GuestAddr base = m_memory->find_free_gap(0x61000000, STORAGE_SIZE);
    if (base == 0 ||
        m_memory->map_region(base, STORAGE_SIZE, MemoryProtection::ReadWrite).is_err()) {
        return false;
    }

    const usize scale = m_is_64bit ? 2 : 1;
    const usize pointer_size = 4 * scale;
    m_region_base = base;
    m_vtable = base;
    m_vtable2 = base + 0x100 * scale;
    m_palette_vtable = base + 0x200 * scale;
    m_vtable7 = base + 0x300 * scale;
    m_d3d7_vtable = base + 0x600 * scale;
    // 49 slots is 0xC4 bytes, so the device vtable owns 0x700..0x7C4.
    m_d3d7_device_vtable = base + 0x700 * scale;
    m_clipper_vtable = base + 0x800 * scale;
    m_surface_vtable = base + 0x400 * scale;
    m_next_object = base + 0x1000 * scale;
    for (u32 slot = 0; slot < IDIRECTDRAW_SLOTS; ++slot) {
        auto thunk = m_thunks->get_thunk_address("ddraw.dll", METHOD_SLOTS[slot].name);
        if (thunk.is_err() ||
            write_pointer(m_vtable + slot * pointer_size, thunk.value()).is_err()) {
            return false;
        }
    }
    for (u32 slot = 0; slot < IDIRECTDRAW2_SLOTS; ++slot) {
        auto thunk = m_thunks->get_thunk_address("ddraw.dll",
                                                 METHOD_SLOTS2[slot].name);
        if (thunk.is_err() ||
            write_pointer(m_vtable2 + slot * pointer_size, thunk.value()).is_err()) {
            return false;
        }
    }
    for (u32 slot = 0; slot < IDIRECTDRAW7_SLOTS; ++slot) {
        auto thunk = m_thunks->get_thunk_address("ddraw.dll",
                                                 METHOD_SLOTS7[slot].name);
        if (thunk.is_err() ||
            write_pointer(m_vtable7 + slot * pointer_size, thunk.value()).is_err()) {
            return false;
        }
    }
    for (u32 slot = 0; slot < IDIRECT3D7_SLOTS; ++slot) {
        auto thunk = m_thunks->get_thunk_address("ddraw.dll",
                                                 METHOD_SLOTS_D3D7[slot].name);
        if (thunk.is_err() ||
            write_pointer(m_d3d7_vtable + slot * pointer_size, thunk.value()).is_err()) {
            return false;
        }
    }
    for (u32 slot = 0; slot < IDIRECT3DDEVICE7_SLOTS; ++slot) {
        auto thunk = m_thunks->get_thunk_address(
            "ddraw.dll", METHOD_SLOTS_D3DDEV7[slot].name);
        if (thunk.is_err() ||
            write_pointer(m_d3d7_device_vtable + slot * pointer_size, thunk.value()).is_err()) {
            return false;
        }
    }
    for (u32 slot = 0; slot < IDIRECTDRAWCLIPPER_SLOTS; ++slot) {
        auto thunk = m_thunks->get_thunk_address(
            "ddraw.dll", METHOD_SLOTS_CLIPPER[slot].name);
        if (thunk.is_err() ||
            write_pointer(m_clipper_vtable + slot * pointer_size, thunk.value()).is_err()) {
            return false;
        }
    }
    constexpr std::array<const char*, IDIRECTDRAWPALETTE_SLOTS> palette_methods = {
        "Vitreo_DDrawPalette_QueryInterface", "Vitreo_DDrawPalette_AddRef",
        "Vitreo_DDrawPalette_Release", "Vitreo_DDrawPalette_GetCaps",
        "Vitreo_DDrawPalette_GetEntries", "Vitreo_DDrawPalette_Initialize",
        "Vitreo_DDrawPalette_SetEntries",
    };
    for (u32 slot = 0; slot < IDIRECTDRAWPALETTE_SLOTS; ++slot) {
        auto thunk = m_thunks->get_thunk_address("ddraw.dll", palette_methods[slot]);
        if (thunk.is_err() ||
            write_pointer(m_palette_vtable + slot * pointer_size, thunk.value()).is_err()) {
            return false;
        }
    }
    constexpr std::array<const char*, IDIRECTDRAWSURFACE_SLOTS> surface_methods = {
        "Vitreo_DDSurface_QueryInterface", "Vitreo_DDSurface_AddRef",
        "Vitreo_DDSurface_Release", "Vitreo_DDSurface_AddAttachedSurface",
        "Vitreo_DDSurface_AddOverlayDirtyRect", "Vitreo_DDSurface_Blt",
        "Vitreo_DDSurface_BltBatch", "Vitreo_DDSurface_BltFast",
        "Vitreo_DDSurface_DeleteAttachedSurface", "Vitreo_DDSurface_EnumAttachedSurfaces",
        "Vitreo_DDSurface_EnumOverlayZOrders", "Vitreo_DDSurface_Flip",
        "Vitreo_DDSurface_GetAttachedSurface", "Vitreo_DDSurface_GetBltStatus",
        "Vitreo_DDSurface_GetCaps", "Vitreo_DDSurface_GetClipper",
        "Vitreo_DDSurface_GetColorKey", "Vitreo_DDSurface_GetDC",
        "Vitreo_DDSurface_GetFlipStatus", "Vitreo_DDSurface_GetOverlayPosition",
        "Vitreo_DDSurface_GetPalette", "Vitreo_DDSurface_GetPixelFormat",
        "Vitreo_DDSurface_GetSurfaceDesc", "Vitreo_DDSurface_Initialize",
        "Vitreo_DDSurface_IsLost", "Vitreo_DDSurface_Lock",
        "Vitreo_DDSurface_ReleaseDC", "Vitreo_DDSurface_Restore",
        "Vitreo_DDSurface_SetClipper", "Vitreo_DDSurface_SetColorKey",
        "Vitreo_DDSurface_SetOverlayPosition", "Vitreo_DDSurface_SetPalette",
        "Vitreo_DDSurface_Unlock", "Vitreo_DDSurface_UpdateOverlay",
        "Vitreo_DDSurface_UpdateOverlayDisplay", "Vitreo_DDSurface_UpdateOverlayZOrder",
    };
    for (u32 slot = 0; slot < IDIRECTDRAWSURFACE_SLOTS; ++slot) {
        auto thunk = m_thunks->get_thunk_address("ddraw.dll", surface_methods[slot]);
        if (thunk.is_err() ||
            write_pointer(m_surface_vtable + slot * pointer_size, thunk.value()).is_err()) {
            return false;
        }
    }
    return true;
}

DirectDrawManager::Object* DirectDrawManager::find_object(GuestAddr object) {
    auto it = m_objects.find(object);
    if (it != m_objects.end()) return &it->second;
    // An interface alias is a second pointer onto the same object.
    const auto alias = m_interface_aliases.find(object);
    if (alias == m_interface_aliases.end()) return nullptr;
    it = m_objects.find(alias->second);
    return it == m_objects.end() ? nullptr : &it->second;
}

const DirectDrawManager::Object* DirectDrawManager::find_object(GuestAddr object) const {
    auto it = m_objects.find(object);
    if (it != m_objects.end()) return &it->second;
    const auto alias = m_interface_aliases.find(object);
    if (alias == m_interface_aliases.end()) return nullptr;
    it = m_objects.find(alias->second);
    return it == m_objects.end() ? nullptr : &it->second;
}

GuestAddr DirectDrawManager::d3d7_interface_for(GuestAddr object) {
    if (!m_memory || !ensure_storage()) return 0;
    // One alias per object, so repeated QueryInterface/GetDirect3D calls hand
    // back the same pointer -- a caller is entitled to compare them.
    for (const auto& [alias, owner] : m_interface_aliases) {
        if (owner == object) return alias;
    }
    const GuestAddr alias = m_next_object;
    m_next_object += 0x20;
    if (write_pointer(alias, m_d3d7_vtable).is_err()) {
        return 0;
    }
    m_interface_aliases.emplace(alias, object);
    return alias;
}

DirectDrawManager::PaletteObject* DirectDrawManager::find_palette(GuestAddr palette) {
    auto it = m_palettes.find(palette);
    return it == m_palettes.end() ? nullptr : &it->second;
}

DirectDrawManager::SurfaceObject* DirectDrawManager::find_surface(GuestAddr surface) {
    auto it = m_surfaces.find(surface);
    return it == m_surfaces.end() ? nullptr : &it->second;
}

u32 DirectDrawManager::direct_draw_create(GuestAddr guid, GuestAddr output,
                                           GuestAddr outer) {
    if (!m_memory || output == 0) return ddraw::E_INVALIDARG;
    (void)write_pointer(output, 0);
    if (outer != 0) return ddraw::CLASS_E_NOAGGREGATION;
    if (guid != 0) return ddraw::DDERR_INVALIDDIRECTDRAWGUID;
    if (!ensure_storage()) return 0x8007000Eu;

    const GuestAddr object = m_next_object;
    m_next_object += 0x20;
    if (write_pointer(object, m_vtable).is_err() ||
        write_pointer(output, object).is_err()) {
        return 0x8007000Eu;
    }
    m_objects.emplace(object, Object{});
    VITREO_TRACE("[ddraw] DirectDrawCreate -> object=0x%llx\n",
                 static_cast<unsigned long long>(object));
    return ddraw::DD_OK;
}


u32 DirectDrawManager::direct_draw_create_ex(GuestAddr guid, GuestAddr output,
                                             GuestAddr iid, GuestAddr outer) {
    constexpr Guid IID_IDIRECTDRAW7_LOCAL{
        0x15e65ec0u, 0x3b9c, 0x11d2,
        {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};
    if (!m_memory || output == 0 || iid == 0) return ddraw::E_INVALIDARG;
    (void)write_pointer(output, 0);
    if (outer != 0) return ddraw::CLASS_E_NOAGGREGATION;
    if (guid != 0) return ddraw::DDERR_INVALIDDIRECTDRAWGUID;

    Guid requested{};
    if (m_memory->read_bytes(iid, reinterpret_cast<Byte*>(&requested),
                             sizeof(requested)).is_err()) {
        return ddraw::E_INVALIDARG;
    }
    // Learn DirectDrawCreateEx: the IID must be IID_IDirectDraw7; anything
    // else is DDERR_INVALIDPARAMS rather than E_NOINTERFACE.
    if (!guid_equal(requested, IID_IDIRECTDRAW7_LOCAL)) {
        return ddraw::DDERR_INVALIDPARAMS;
    }
    if (!ensure_storage()) return 0x8007000Eu;

    const GuestAddr object = m_next_object;
    m_next_object += 0x20;
    if (write_pointer(object, m_vtable7).is_err() ||
        write_pointer(output, object).is_err()) {
        return 0x8007000Eu;
    }
    m_objects.emplace(object, Object{});
    VITREO_TRACE("[ddraw] DirectDrawCreateEx -> IDirectDraw7 object=0x%llx\n",
                 static_cast<unsigned long long>(object));
    return ddraw::DD_OK;
}

u32 DirectDrawManager::query_interface(GuestAddr object, GuestAddr iid,
                                        GuestAddr output) {
    if (!m_memory || output == 0) return ddraw::E_INVALIDARG;
    (void)write_pointer(output, 0);
    if (!find_object(object)) return ddraw::DDERR_INVALIDOBJECT;
    Guid requested{};
    if (iid == 0 || m_memory->read_bytes(
            iid, reinterpret_cast<Byte*>(&requested), sizeof(requested)).is_err()) {
        return ddraw::E_INVALIDARG;
    }
    // SDL 1.2's DirectX backend asks for IDirectDraw2 immediately after
    // DirectDrawCreate and gives up on the whole video driver if it is
    // refused, which is what pushed it onto windib.
    VITREO_TRACE("[ddraw] QueryInterface iid=%08x-%04x-%04x\n",
                 requested.data1, requested.data2, requested.data3);
    constexpr Guid IID_IDIRECTDRAW7_QI{
        0x15e65ec0u, 0x3b9c, 0x11d2,
        {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};
    // IID_IDirect3D7 {F5049E77-4861-11D2-A407-00A0C90629A8}
    constexpr Guid IID_IDIRECT3D7{
        0xf5049e77u, 0x4861, 0x11d2,
        {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};
    const bool want_v2 = guid_equal(requested, IID_IDIRECTDRAW2);
    const bool want_v7 = guid_equal(requested, IID_IDIRECTDRAW7_QI);
    const bool want_d3d7 = guid_equal(requested, IID_IDIRECT3D7);
    if (want_d3d7) {
        // Direct3D 7 shares the DirectDraw object's lifetime; the interface
        // difference is the vtable the caller is handed.
        // Overwriting this object's vtable would revoke the IDirectDraw7
        // interface the caller still holds: its next SetCooperativeLevel would
        // index slot 20 of an 8-slot IDirect3D7 vtable and call through zero.
        const GuestAddr d3d7 = d3d7_interface_for(object);
        if (d3d7 == 0) return ddraw::E_INVALIDARG;
        (void)write_pointer(output, d3d7);
        (void)add_ref(object);
        VITREO_TRACE("[d3d7] QueryInterface IID_IDirect3D7 -> object=0x%llx\n",
                     static_cast<unsigned long long>(object));
        return ddraw::DD_OK;
    }
    if (!guid_equal(requested, IID_IUNKNOWN) &&
        !guid_equal(requested, IID_IDIRECTDRAW) && !want_v2 && !want_v7) {
        return ddraw::E_NOINTERFACE;
    }
    // The two interfaces differ in vtable shape, so switching the object's
    // vtable pointer is what actually hands back the requested interface.
    if (want_v2 &&
        write_pointer(object, m_vtable2).is_err()) {
        return ddraw::E_INVALIDARG;
    }
    if (want_v7) {
        if (!ensure_storage()) return ddraw::E_INVALIDARG;
        if (write_pointer(object, m_vtable7).is_err()) {
            return ddraw::E_INVALIDARG;
        }
    }
    (void)write_pointer(output, object);
    (void)add_ref(object);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::add_ref(GuestAddr object) {
    Object* state = find_object(object);
    return state ? ++state->ref_count : 0;
}

u32 DirectDrawManager::release(GuestAddr object) {
    Object* state = find_object(object);
    if (!state) return 0;
    const u32 result = --state->ref_count;
    if (result == 0) m_objects.erase(object);
    return result;
}

u32 DirectDrawManager::cooperative_flags(GuestAddr object) const {
    const Object* state = find_object(object);
    return state ? state->cooperative_flags : 0;
}

u32 DirectDrawManager::display_width(GuestAddr object) const {
    return find_object(object) ? m_presentation.display_mode().width : 0;
}

u32 DirectDrawManager::display_height(GuestAddr object) const {
    return find_object(object) ? m_presentation.display_mode().height : 0;
}

u32 DirectDrawManager::display_bpp(GuestAddr object) const {
    return find_object(object) ? m_presentation.display_mode().bits_per_pixel : 0;
}

} // namespace vitreo::winapi
