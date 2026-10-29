// SPDX-License-Identifier: LGPL-2.1-only

/// @file ddraw_thunks.cpp
/// @brief Guest-visible ddraw.dll export table (create_ddraw_thunks).

#include "winapi/graphics/ddraw/ddraw.hpp"
#include "winapi/graphics/ddraw/ddraw_internal.hpp"
#include "winapi/core/marshaler.hpp"

#include <array>

namespace vitreo::winapi {

using namespace ddraw_internal;

DllExports create_ddraw_thunks(DirectDrawManager& manager) {
    DllExports exports;
    exports.entries.push_back({"DirectDrawCreate", 1, [&manager](Marshaler& m) {
        m.set_return(manager.direct_draw_create(m.arg_ptr(0), m.arg_ptr(1),
                                                m.arg_ptr(2)));
    }, 12});
    // Learn: DirectDrawCreateEx(GUID*, LPVOID*, REFIID, IUnknown*) — stdcall=16.
    exports.entries.push_back({"DirectDrawCreateEx", 0, [&manager](Marshaler& m) {
        m.set_return(manager.direct_draw_create_ex(
            m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), m.arg_ptr(3)));
    }, 16});
    // Learn: DirectDrawCreateClipper(DWORD, LPDIRECTDRAWCLIPPER*, IUnknown*)
    // — stdcall=12. A caller that resolves it by GetProcAddress and finds
    // nothing has no way to clip a windowed blit at all, so the export must
    // exist alongside DirectDrawCreate.
    exports.entries.push_back({"DirectDrawCreateClipper", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.direct_draw_create_clipper(
                static_cast<u32>(m.arg(0)), m.arg_ptr(1), m.arg_ptr(2)));
        }, 12});
    exports.entries.push_back({"Vitreo_DDraw_CreateClipper", 0,
        [&manager](Marshaler& m) { m.set_return(manager.create_clipper(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2),
            m.arg_ptr(3))); }, 16});
    exports.entries.push_back({"Vitreo_DDClipper_QueryInterface", 0,
        [&manager](Marshaler& m) { m.set_return(manager.clipper_query_interface(
            m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_DDClipper_AddRef", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.clipper_add_ref(m.arg_ptr(0))); }, 4});
    exports.entries.push_back({"Vitreo_DDClipper_Release", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.clipper_release(m.arg_ptr(0))); }, 4});
    exports.entries.push_back({"Vitreo_DDClipper_GetClipList", 0,
        [&manager](Marshaler& m) { m.set_return(manager.clipper_get_clip_list(
            m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), m.arg_ptr(3))); }, 16});
    exports.entries.push_back({"Vitreo_DDClipper_GetHWnd", 0,
        [&manager](Marshaler& m) { m.set_return(manager.clipper_get_hwnd(
            m.arg_ptr(0), m.arg_ptr(1))); }, 8});
    exports.entries.push_back({"Vitreo_DDClipper_Initialize", 0,
        [&manager](Marshaler& m) { m.set_return(manager.clipper_initialize(
            m.arg_ptr(0), m.arg_ptr(1), static_cast<u32>(m.arg(2)))); }, 12});
    exports.entries.push_back({"Vitreo_DDClipper_IsClipListChanged", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.clipper_is_clip_list_changed(
                m.arg_ptr(0), m.arg_ptr(1))); }, 8});
    exports.entries.push_back({"Vitreo_DDClipper_SetClipList", 0,
        [&manager](Marshaler& m) { m.set_return(manager.clipper_set_clip_list(
            m.arg_ptr(0), m.arg_ptr(1), static_cast<u32>(m.arg(2)))); }, 12});
    exports.entries.push_back({"Vitreo_DDClipper_SetHWnd", 0,
        [&manager](Marshaler& m) { m.set_return(manager.clipper_set_hwnd(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)),
            static_cast<u32>(m.arg(2)))); }, 12});
    exports.entries.push_back({"Vitreo_DDSurface_SetClipper", 0,
        [&manager](Marshaler& m) { m.set_return(manager.surface_set_clipper(
            m.arg_ptr(0), m.arg_ptr(1))); }, 8});
    exports.entries.push_back({"Vitreo_DDSurface_GetClipper", 0,
        [&manager](Marshaler& m) { m.set_return(manager.surface_get_clipper(
            m.arg_ptr(0), m.arg_ptr(1))); }, 8});
    exports.entries.push_back({"Vitreo_D3D7_QueryInterface", 0,
        [&manager](Marshaler& m) { m.set_return(manager.query_interface(
            m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_D3D7_AddRef", 0,
        [&manager](Marshaler& m) { m.set_return(manager.add_ref(m.arg_ptr(0))); }, 4});
    exports.entries.push_back({"Vitreo_D3D7_Release", 0,
        [&manager](Marshaler& m) { m.set_return(manager.release(m.arg_ptr(0))); }, 4});
    exports.entries.push_back({"Vitreo_D3D7_EnumDevices", 0,
        [&manager](Marshaler& m) { m.set_return(manager.d3d7_enum_devices(
            m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_D3D7_CreateDevice", 0,
        [&manager](Marshaler& m) { m.set_return(manager.d3d7_create_device(
            m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), m.arg_ptr(3))); }, 16});
    exports.entries.push_back({"Vitreo_D3D7_CreateVertexBuffer", 0,
        [&manager](Marshaler& m) { m.set_return(manager.d3d7_create_vertex_buffer(
            m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2),
            static_cast<u32>(m.arg(3)))); }, 16});
    exports.entries.push_back({"Vitreo_D3D7_EnumZBufferFormats", 0,
        [&manager](Marshaler& m) { m.set_return(manager.d3d7_enum_z_buffer_formats(
            m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), m.arg_ptr(3))); }, 16});
    exports.entries.push_back({"Vitreo_D3D7_EvictManagedTextures", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.d3d7_evict_managed_textures(m.arg_ptr(0))); }, 4});
    // ---- IDirect3DDevice7 -------------------------------------------------
    // Slots with no implementation are registered first from the same table
    // that builds the vtable, so every slot resolves to a thunk with the right
    // stdcall cleanup even when the method itself is unsupported. Getting the
    // byte count wrong would unbalance the guest's stack, which is far worse
    // than the E_NOTIMPL it returns.
    {
        // Slot indices into METHOD_SLOTS_D3DDEV7 with no implementation:
        // the state blocks (22, 23, 39..42), the strided and vertex-buffer
        // draw forms (29..32), ComputeSphereVisibility (33) and Load (43).
        // All of them need the submission half of the draw path, so each
        // fails rather than reporting work it did not do.
        constexpr std::array<u32, 12> unimplemented = {
            22, 23, 29, 30, 31, 32, 33, 39, 40, 41, 42, 43,
        };
        for (const u32 slot : unimplemented) {
            exports.entries.push_back({METHOD_SLOTS_D3DDEV7[slot].name, 0,
                [name = METHOD_SLOTS_D3DDEV7[slot].name](Marshaler& m) {
                    VITREO_TRACE("[d3d7] unsupported method=%s\n", name);
                    m.set_return(ddraw::E_NOTIMPL);
                },
                METHOD_SLOTS_D3DDEV7[slot].stdcall_bytes});
        }
    }
    exports.entries.push_back({"Vitreo_D3D7Dev_QueryInterface", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_query_interface(
            m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_D3D7Dev_AddRef", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_add_ref(m.arg_ptr(0))); }, 4});
    exports.entries.push_back({"Vitreo_D3D7Dev_Release", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_release(m.arg_ptr(0))); }, 4});
    exports.entries.push_back({"Vitreo_D3D7Dev_GetCaps", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_get_caps(
            m.arg_ptr(0), m.arg_ptr(1))); }, 8});
    exports.entries.push_back({"Vitreo_D3D7Dev_EnumTextureFormats", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_enum_texture_formats(
                m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_D3D7Dev_BeginScene", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_begin_scene(m.arg_ptr(0))); }, 4});
    exports.entries.push_back({"Vitreo_D3D7Dev_EndScene", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_end_scene(m.arg_ptr(0))); }, 4});
    exports.entries.push_back({"Vitreo_D3D7Dev_GetDirect3D", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_get_direct3d(
            m.arg_ptr(0), m.arg_ptr(1))); }, 8});
    exports.entries.push_back({"Vitreo_D3D7Dev_SetRenderTarget", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_set_render_target(
            m.arg_ptr(0), m.arg_ptr(1), static_cast<u32>(m.arg(2)))); }, 12});
    exports.entries.push_back({"Vitreo_D3D7Dev_GetRenderTarget", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_get_render_target(
            m.arg_ptr(0), m.arg_ptr(1))); }, 8});
    exports.entries.push_back({"Vitreo_D3D7Dev_Clear", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_clear(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2),
            static_cast<u32>(m.arg(3)), static_cast<u32>(m.arg(4)),
            static_cast<u32>(m.arg(5)), static_cast<u32>(m.arg(6)))); }, 28});
    exports.entries.push_back({"Vitreo_D3D7Dev_SetTransform", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_set_transform(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_D3D7Dev_GetTransform", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_get_transform(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_D3D7Dev_SetViewport", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_set_viewport(
            m.arg_ptr(0), m.arg_ptr(1))); }, 8});
    exports.entries.push_back({"Vitreo_D3D7Dev_MultiplyTransform", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_multiply_transform(
                m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_D3D7Dev_GetViewport", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_get_viewport(
            m.arg_ptr(0), m.arg_ptr(1))); }, 8});
    exports.entries.push_back({"Vitreo_D3D7Dev_SetMaterial", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_set_material(
            m.arg_ptr(0), m.arg_ptr(1))); }, 8});
    exports.entries.push_back({"Vitreo_D3D7Dev_GetMaterial", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_get_material(
            m.arg_ptr(0), m.arg_ptr(1))); }, 8});
    exports.entries.push_back({"Vitreo_D3D7Dev_SetLight", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_set_light(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_D3D7Dev_GetLight", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_get_light(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_D3D7Dev_SetRenderState", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_set_render_state(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)),
            static_cast<u32>(m.arg(2)))); }, 12});
    exports.entries.push_back({"Vitreo_D3D7Dev_GetRenderState", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_get_render_state(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_D3D7Dev_PreLoad", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_pre_load(
            m.arg_ptr(0), m.arg_ptr(1))); }, 8});
    exports.entries.push_back({"Vitreo_D3D7Dev_DrawPrimitive", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_draw_primitive(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)),
            static_cast<u32>(m.arg(2)), m.arg_ptr(3),
            static_cast<u32>(m.arg(4)), static_cast<u32>(m.arg(5)))); }, 24});
    exports.entries.push_back({"Vitreo_D3D7Dev_DrawIndexedPrimitive", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_draw_indexed_primitive(
                m.arg_ptr(0), static_cast<u32>(m.arg(1)),
                static_cast<u32>(m.arg(2)), m.arg_ptr(3),
                static_cast<u32>(m.arg(4)), m.arg_ptr(5),
                static_cast<u32>(m.arg(6)),
                static_cast<u32>(m.arg(7)))); }, 32});
    exports.entries.push_back({"Vitreo_D3D7Dev_SetClipStatus", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_set_clip_status(
            m.arg_ptr(0), m.arg_ptr(1))); }, 8});
    exports.entries.push_back({"Vitreo_D3D7Dev_GetClipStatus", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_get_clip_status(
            m.arg_ptr(0), m.arg_ptr(1))); }, 8});
    exports.entries.push_back({"Vitreo_D3D7Dev_GetTexture", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_get_texture(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_D3D7Dev_SetTexture", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_set_texture(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_D3D7Dev_GetTextureStageState", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_get_texture_stage_state(
                m.arg_ptr(0), static_cast<u32>(m.arg(1)),
                static_cast<u32>(m.arg(2)), m.arg_ptr(3))); }, 16});
    exports.entries.push_back({"Vitreo_D3D7Dev_SetTextureStageState", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_set_texture_stage_state(
                m.arg_ptr(0), static_cast<u32>(m.arg(1)),
                static_cast<u32>(m.arg(2)),
                static_cast<u32>(m.arg(3)))); }, 16});
    exports.entries.push_back({"Vitreo_D3D7Dev_ValidateDevice", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_validate_device(
            m.arg_ptr(0), m.arg_ptr(1))); }, 8});
    exports.entries.push_back({"Vitreo_D3D7Dev_LightEnable", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_light_enable(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)),
            static_cast<u32>(m.arg(2)))); }, 12});
    exports.entries.push_back({"Vitreo_D3D7Dev_GetLightEnable", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_get_light_enable(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_D3D7Dev_SetClipPlane", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_set_clip_plane(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_D3D7Dev_GetClipPlane", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_get_clip_plane(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_D3D7Dev_GetInfo", 0,
        [&manager](Marshaler& m) { m.set_return(manager.device_get_info(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2),
            static_cast<u32>(m.arg(3)))); }, 16});
    exports.entries.push_back({"Vitreo_DDraw7_GetSurfaceFromDC", 0,
        [&manager](Marshaler& m) { m.set_return(manager.get_surface_from_dc(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_DDraw7_RestoreAllSurfaces", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.restore_all_surfaces(m.arg_ptr(0))); }, 4});
    exports.entries.push_back({"Vitreo_DDraw7_TestCooperativeLevel", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.test_cooperative_level(m.arg_ptr(0))); }, 4});
    exports.entries.push_back({"Vitreo_DDraw7_GetDeviceIdentifier", 0,
        [&manager](Marshaler& m) { m.set_return(manager.get_device_identifier(
            m.arg_ptr(0), m.arg_ptr(1), static_cast<u32>(m.arg(2)))); }, 12});
    exports.entries.push_back({"Vitreo_DDraw7_StartModeTest", 0,
        [&manager](Marshaler& m) { m.set_return(manager.start_mode_test(
            m.arg_ptr(0), m.arg_ptr(1), static_cast<u32>(m.arg(2)),
            static_cast<u32>(m.arg(3)))); }, 16});
    exports.entries.push_back({"Vitreo_DDraw7_EvaluateMode", 0,
        [&manager](Marshaler& m) { m.set_return(manager.evaluate_mode(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_DDraw_QueryInterface", 0,
        [&manager](Marshaler& m) { m.set_return(manager.query_interface(
            m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_DDraw_AddRef", 0,
        [&manager](Marshaler& m) { m.set_return(manager.add_ref(m.arg_ptr(0))); }, 4});
    exports.entries.push_back({"Vitreo_DDraw_Release", 0,
        [&manager](Marshaler& m) { m.set_return(manager.release(m.arg_ptr(0))); }, 4});
    for (u32 slot = 3; slot < IDIRECTDRAW_SLOTS; ++slot) {
        if (slot == 4 || slot == 5 || slot == 6 || slot == 8 || slot == 11 ||
            slot == 12 || slot == 17 || slot == 20 || slot == 21 ||
            slot == 22) continue;
        exports.entries.push_back({METHOD_SLOTS[slot].name, 0,
            [name = METHOD_SLOTS[slot].name](Marshaler& m) {
                VITREO_TRACE("[ddraw] unsupported method=%s\n", name);
                m.set_return(ddraw::E_NOTIMPL);
            },
            METHOD_SLOTS[slot].stdcall_bytes});
    }
    exports.entries.push_back({"Vitreo_DDraw_GetCaps", 0,
        [&manager](Marshaler& m) { m.set_return(manager.get_caps(
            m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_DDraw_GetDisplayMode", 0,
        [&manager](Marshaler& m) { m.set_return(manager.get_display_mode(
            m.arg_ptr(0), m.arg_ptr(1))); }, 8});
    exports.entries.push_back({"Vitreo_DDraw_GetVerticalBlankStatus", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.get_vertical_blank_status(
                m.arg_ptr(0), m.arg_ptr(1)));
        }, 8});
    exports.entries.push_back({"Vitreo_DDraw_WaitForVerticalBlank", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.wait_for_vertical_blank(
                m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2)));
        }, 12});
    exports.entries.push_back({"Vitreo_DDraw_EnumDisplayModes", 0,
        [&manager](Marshaler& m) { m.set_return(manager.enum_display_modes(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2),
            m.arg_ptr(3), m.arg_ptr(4))); }, 20});
    exports.entries.push_back({"Vitreo_DDraw_SetCooperativeLevel", 0,
        [&manager](Marshaler& m) { m.set_return(manager.set_cooperative_level(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), static_cast<u32>(m.arg(2)))); }, 12});
    exports.entries.push_back({"Vitreo_DDraw_RestoreDisplayMode", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.restore_display_mode(m.arg_ptr(0)));
        }, 4});
    exports.entries.push_back({"Vitreo_DDraw_SetDisplayMode", 0,
        [&manager](Marshaler& m) { m.set_return(manager.set_display_mode(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), static_cast<u32>(m.arg(2)),
            static_cast<u32>(m.arg(3)))); }, 16});

    // IDirectDraw2::SetDisplayMode(width, height, bpp, refresh_rate, flags).
    // Vitreo presents into a host window, so a requested refresh rate has no
    // hardware to program; 0 ("driver default") is the only honest answer and
    // is what callers pass anyway.
    exports.entries.push_back({"Vitreo_DDraw2_SetDisplayMode", 0,
        [&manager](Marshaler& m) {
            const u32 refresh = static_cast<u32>(m.arg(4));
            if (refresh != 0) {
                VITREO_TRACE("[ddraw] SetDisplayMode: ignoring requested "
                             "refresh rate %u Hz\n", refresh);
            }
            (void)m.arg(5);  // flags (DDSDM_STANDARDVGAMODE)
            m.set_return(manager.set_display_mode(
                m.arg_ptr(0), static_cast<u32>(m.arg(1)),
                static_cast<u32>(m.arg(2)), static_cast<u32>(m.arg(3))));
        }, 24});

    // IDirectDraw2::GetAvailableVidMem(LPDDSCAPS, LPDWORD total, LPDWORD free)
    // Vitreo's surfaces live in ordinary guest memory, so report a fixed pool
    // rather than a hardware figure. Callers use this to size their surface
    // budget; reporting zero would make them refuse to allocate anything.
    exports.entries.push_back({"Vitreo_DDraw2_GetAvailableVidMem", 0,
        [](Marshaler& m) {
            constexpr u32 POOL_BYTES = 64u * 1024u * 1024u;
            (void)m.arg_ptr(1);  // LPDDSCAPS
            const GuestAddr total = m.arg_ptr(2);
            const GuestAddr available = m.arg_ptr(3);
            if (total != 0) (void)m.write_u32(total, POOL_BYTES);
            if (available != 0) (void)m.write_u32(available, POOL_BYTES);
            m.set_return(ddraw::DD_OK);
        }, 16});
    exports.entries.push_back({"Vitreo_DDraw_CreatePalette", 0,
        [&manager](Marshaler& m) { m.set_return(manager.create_palette(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2),
            m.arg_ptr(3), m.arg_ptr(4))); }, 20});
    exports.entries.push_back({"Vitreo_DDraw_CreateSurface", 0,
        [&manager](Marshaler& m) { m.set_return(manager.create_surface(
            m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), m.arg_ptr(3))); }, 16});
    exports.entries.push_back({"Vitreo_DDrawPalette_QueryInterface", 0,
        [&manager](Marshaler& m) { m.set_return(manager.palette_query_interface(
            m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_DDrawPalette_AddRef", 0,
        [&manager](Marshaler& m) { m.set_return(manager.palette_add_ref(m.arg_ptr(0))); }, 4});
    exports.entries.push_back({"Vitreo_DDrawPalette_Release", 0,
        [&manager](Marshaler& m) { m.set_return(manager.palette_release(m.arg_ptr(0))); }, 4});
    exports.entries.push_back({"Vitreo_DDrawPalette_GetCaps", 0,
        [&manager](Marshaler& m) { m.set_return(manager.palette_get_caps(
            m.arg_ptr(0), m.arg_ptr(1))); }, 8});
    exports.entries.push_back({"Vitreo_DDrawPalette_GetEntries", 0,
        [&manager](Marshaler& m) { m.set_return(manager.palette_get_entries(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), static_cast<u32>(m.arg(2)),
            static_cast<u32>(m.arg(3)), m.arg_ptr(4))); }, 20});
    exports.entries.push_back({"Vitreo_DDrawPalette_Initialize", 0,
        [](Marshaler& m) { m.set_return(ddraw::DDERR_INVALIDOBJECT); }, 16});
    exports.entries.push_back({"Vitreo_DDrawPalette_SetEntries", 0,
        [&manager](Marshaler& m) { m.set_return(manager.palette_set_entries(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), static_cast<u32>(m.arg(2)),
            static_cast<u32>(m.arg(3)), m.arg_ptr(4))); }, 20});
    constexpr std::array<u16, IDIRECTDRAWSURFACE_SLOTS> surface_cleanup = {
        12, 4, 4, 8, 8, 24, 16, 24, 12, 12, 16, 12, 12, 8, 8, 8, 12, 8,
        8, 12, 8, 8, 8, 12, 4, 20, 8, 4, 8, 12, 12, 8, 8, 24, 8, 12,
    };
    constexpr std::array<const char*, IDIRECTDRAWSURFACE_SLOTS> surface_names = {
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
        // Implemented below: QI/AddRef/Release, Blt, BltFast, GetSurfaceDesc,
        // IsLost, Lock, Restore, SetColorKey, SetPalette, Unlock.
        if (slot == 0 || slot == 1 || slot == 2 || slot == 5 || slot == 7 ||
            slot == 11 || slot == 12 || slot == 15 || slot == 21 ||
            slot == 22 ||
            slot == 24 || slot == 25 || slot == 27 || slot == 28 ||
            slot == 29 || slot == 31 || slot == 32) continue;
        exports.entries.push_back({surface_names[slot], 0,
            [name = surface_names[slot]](Marshaler& m) {
                VITREO_TRACE("[ddraw] unsupported method=%s\n", name);
                m.set_return(ddraw::E_NOTIMPL);
            },
            surface_cleanup[slot]});
    }
    exports.entries.push_back({"Vitreo_DDSurface_QueryInterface", 0,
        [&manager](Marshaler& m) { m.set_return(manager.surface_query_interface(
            m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2))); }, 12});
    exports.entries.push_back({"Vitreo_DDSurface_AddRef", 0,
        [&manager](Marshaler& m) { m.set_return(manager.surface_add_ref(m.arg_ptr(0))); }, 4});
    exports.entries.push_back({"Vitreo_DDSurface_Release", 0,
        [&manager](Marshaler& m) { m.set_return(manager.surface_release(m.arg_ptr(0))); }, 4});
    exports.entries.push_back({"Vitreo_DDSurface_Blt", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.surface_blt(
                m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), m.arg_ptr(3),
                static_cast<u32>(m.arg(4)), m.arg_ptr(5)));
        }, 24});
    exports.entries.push_back({"Vitreo_DDSurface_BltFast", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.surface_blt_fast(
                m.arg_ptr(0), static_cast<u32>(m.arg(1)),
                static_cast<u32>(m.arg(2)), m.arg_ptr(3), m.arg_ptr(4),
                static_cast<u32>(m.arg(5))));
        }, 24});
    exports.entries.push_back({"Vitreo_DDSurface_SetColorKey", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.surface_set_color_key(
                m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2)));
        }, 12});
    exports.entries.push_back({"Vitreo_DDSurface_GetSurfaceDesc", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.surface_get_surface_desc(
                m.arg_ptr(0), m.arg_ptr(1)));
        }, 8});
    exports.entries.push_back({"Vitreo_DDSurface_IsLost", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.surface_is_lost(m.arg_ptr(0)));
        }, 4});
    exports.entries.push_back({"Vitreo_DDSurface_GetPixelFormat", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.surface_get_pixel_format(
                m.arg_ptr(0), m.arg_ptr(1)));
        }, 8});
    exports.entries.push_back({"Vitreo_DDSurface_GetAttachedSurface", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.surface_get_attached_surface(
                m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2)));
        }, 12});
    exports.entries.push_back({"Vitreo_DDSurface_Flip", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.surface_flip(
                m.arg_ptr(0), m.arg_ptr(1), static_cast<u32>(m.arg(2))));
        }, 12});
    exports.entries.push_back({"Vitreo_DDSurface_Restore", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.surface_restore(m.arg_ptr(0)));
        }, 4});
    exports.entries.push_back({"Vitreo_DDSurface_Lock", 0,
        [&manager](Marshaler& m) {
            const auto caller = m.cpu()
                ? m.read_u32(static_cast<GuestAddr>(m.cpu()->sp())).value_or(0)
                : 0;
            VITREO_TRACE("[ddraw] Surface::Lock caller=0x%08x\n", caller);
            m.set_return(manager.surface_lock(
                m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), static_cast<u32>(m.arg(3)),
                m.arg_ptr(4)));
        }, 20});
    exports.entries.push_back({"Vitreo_DDSurface_SetPalette", 0,
        [&manager](Marshaler& m) { m.set_return(manager.surface_set_palette(
            m.arg_ptr(0), m.arg_ptr(1))); }, 8});
    exports.entries.push_back({"Vitreo_DDSurface_Unlock", 0,
        [&manager](Marshaler& m) {
            const auto caller = m.cpu()
                ? m.read_u32(static_cast<GuestAddr>(m.cpu()->sp())).value_or(0)
                : 0;
            VITREO_TRACE("[ddraw] Surface::Unlock caller=0x%08x\n", caller);
            m.set_return(manager.surface_unlock(m.arg_ptr(0), m.arg_ptr(1)));
        }, 8});
    return exports;
}

} // namespace vitreo::winapi
