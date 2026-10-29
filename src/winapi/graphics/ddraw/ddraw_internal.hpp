// SPDX-License-Identifier: LGPL-2.1-only

/// @file ddraw_internal.hpp
/// @brief Private tables shared by the DirectDraw / Direct3D 7 translation
///        units: vtable method slots, interface GUIDs and device kinds.

#pragma once

#include <vitreo/common.hpp>

#include <array>
#include <cstring>

namespace vitreo::winapi::ddraw_internal {

inline constexpr u32 IDIRECTDRAW_SLOTS = 23;
inline constexpr u32 IDIRECTDRAWPALETTE_SLOTS = 7;
inline constexpr u32 IDIRECTDRAWSURFACE_SLOTS = 36;

struct MethodSlot {
    const char* name;
    u16 stdcall_bytes;
};

inline constexpr std::array<MethodSlot, IDIRECTDRAW_SLOTS> METHOD_SLOTS = {{
    {"Vitreo_DDraw_QueryInterface", 12},
    {"Vitreo_DDraw_AddRef", 4},
    {"Vitreo_DDraw_Release", 4},
    {"Vitreo_DDraw_Compact", 4},
    {"Vitreo_DDraw_CreateClipper", 16},
    {"Vitreo_DDraw_CreatePalette", 16},
    {"Vitreo_DDraw_CreateSurface", 16},
    {"Vitreo_DDraw_DuplicateSurface", 12},
    {"Vitreo_DDraw_EnumDisplayModes", 20},
    {"Vitreo_DDraw_EnumSurfaces", 20},
    {"Vitreo_DDraw_FlipToGDISurface", 4},
    {"Vitreo_DDraw_GetCaps", 12},
    {"Vitreo_DDraw_GetDisplayMode", 8},
    {"Vitreo_DDraw_GetFourCCCodes", 12},
    {"Vitreo_DDraw_GetGDISurface", 8},
    {"Vitreo_DDraw_GetMonitorFrequency", 8},
    {"Vitreo_DDraw_GetScanLine", 8},
    {"Vitreo_DDraw_GetVerticalBlankStatus", 8},
    {"Vitreo_DDraw_Initialize", 8},
    {"Vitreo_DDraw_RestoreDisplayMode", 4},
    {"Vitreo_DDraw_SetCooperativeLevel", 12},
    {"Vitreo_DDraw_SetDisplayMode", 16},
    {"Vitreo_DDraw_WaitForVerticalBlank", 12},
}};

inline constexpr u32 IDIRECTDRAW2_SLOTS = 24;

/// IDirectDraw2 repeats IDirectDraw's layout except that SetDisplayMode takes
/// two extra parameters (refresh rate and flags) and GetAvailableVidMem is
/// appended. The shared slots reuse the IDirectDraw thunks; only the two
/// differing ones need their own entries.
inline constexpr std::array<MethodSlot, IDIRECTDRAW2_SLOTS> METHOD_SLOTS2 = {{
    {"Vitreo_DDraw_QueryInterface", 12},
    {"Vitreo_DDraw_AddRef", 4},
    {"Vitreo_DDraw_Release", 4},
    {"Vitreo_DDraw_Compact", 4},
    {"Vitreo_DDraw_CreateClipper", 16},
    {"Vitreo_DDraw_CreatePalette", 16},
    {"Vitreo_DDraw_CreateSurface", 16},
    {"Vitreo_DDraw_DuplicateSurface", 12},
    {"Vitreo_DDraw_EnumDisplayModes", 20},
    {"Vitreo_DDraw_EnumSurfaces", 20},
    {"Vitreo_DDraw_FlipToGDISurface", 4},
    {"Vitreo_DDraw_GetCaps", 12},
    {"Vitreo_DDraw_GetDisplayMode", 8},
    {"Vitreo_DDraw_GetFourCCCodes", 12},
    {"Vitreo_DDraw_GetGDISurface", 8},
    {"Vitreo_DDraw_GetMonitorFrequency", 8},
    {"Vitreo_DDraw_GetScanLine", 8},
    {"Vitreo_DDraw_GetVerticalBlankStatus", 8},
    {"Vitreo_DDraw_Initialize", 8},
    {"Vitreo_DDraw_RestoreDisplayMode", 4},
    {"Vitreo_DDraw_SetCooperativeLevel", 12},
    {"Vitreo_DDraw2_SetDisplayMode", 24},
    {"Vitreo_DDraw_WaitForVerticalBlank", 12},
    {"Vitreo_DDraw2_GetAvailableVidMem", 16},
}};

/// IDirectDraw7 is IDirectDraw2 plus six methods. The shared slots reuse the
/// existing thunks; SetDisplayMode/GetAvailableVidMem keep the v2 shapes.
inline constexpr u32 IDIRECTDRAW7_SLOTS = 30;
inline constexpr std::array<MethodSlot, IDIRECTDRAW7_SLOTS> METHOD_SLOTS7 = {{
    {"Vitreo_DDraw_QueryInterface", 12},
    {"Vitreo_DDraw_AddRef", 4},
    {"Vitreo_DDraw_Release", 4},
    {"Vitreo_DDraw_Compact", 4},
    {"Vitreo_DDraw_CreateClipper", 16},
    {"Vitreo_DDraw_CreatePalette", 16},
    {"Vitreo_DDraw_CreateSurface", 16},
    {"Vitreo_DDraw_DuplicateSurface", 12},
    {"Vitreo_DDraw_EnumDisplayModes", 20},
    {"Vitreo_DDraw_EnumSurfaces", 20},
    {"Vitreo_DDraw_FlipToGDISurface", 4},
    {"Vitreo_DDraw_GetCaps", 12},
    {"Vitreo_DDraw_GetDisplayMode", 8},
    {"Vitreo_DDraw_GetFourCCCodes", 12},
    {"Vitreo_DDraw_GetGDISurface", 8},
    {"Vitreo_DDraw_GetMonitorFrequency", 8},
    {"Vitreo_DDraw_GetScanLine", 8},
    {"Vitreo_DDraw_GetVerticalBlankStatus", 8},
    {"Vitreo_DDraw_Initialize", 8},
    {"Vitreo_DDraw_RestoreDisplayMode", 4},
    {"Vitreo_DDraw_SetCooperativeLevel", 12},
    {"Vitreo_DDraw2_SetDisplayMode", 24},
    {"Vitreo_DDraw_WaitForVerticalBlank", 12},
    {"Vitreo_DDraw2_GetAvailableVidMem", 16},
    {"Vitreo_DDraw7_GetSurfaceFromDC", 12},
    {"Vitreo_DDraw7_RestoreAllSurfaces", 4},
    {"Vitreo_DDraw7_TestCooperativeLevel", 4},
    {"Vitreo_DDraw7_GetDeviceIdentifier", 12},
    {"Vitreo_DDraw7_StartModeTest", 16},
    {"Vitreo_DDraw7_EvaluateMode", 12},
}};

/// IDirect3D7 (ddraw.dll). Direct3D 7 is fixed-function only, so the device
/// this hands out is described by the shared FFPipelineState rather than a
/// shader frontend.
inline constexpr u32 IDIRECT3D7_SLOTS = 8;
inline constexpr std::array<MethodSlot, IDIRECT3D7_SLOTS> METHOD_SLOTS_D3D7 = {{
    {"Vitreo_D3D7_QueryInterface", 12},
    {"Vitreo_D3D7_AddRef", 4},
    {"Vitreo_D3D7_Release", 4},
    {"Vitreo_D3D7_EnumDevices", 12},
    {"Vitreo_D3D7_CreateDevice", 16},
    {"Vitreo_D3D7_CreateVertexBuffer", 16},
    {"Vitreo_D3D7_EnumZBufferFormats", 16},
    {"Vitreo_D3D7_EvictManagedTextures", 4},
}};

/// IDirect3DDevice7. The slot order is d3d.h's DECLARE_INTERFACE_ block and
/// must not be reordered to match documentation groupings — a wrong index
/// silently dispatches a different method with a different stack frame.
/// stdcall byte counts are `this` plus each argument, all 4 bytes on x86.
inline constexpr u32 IDIRECT3DDEVICE7_SLOTS = 49;
inline constexpr std::array<MethodSlot, IDIRECT3DDEVICE7_SLOTS> METHOD_SLOTS_D3DDEV7 = {{
    {"Vitreo_D3D7Dev_QueryInterface", 12},
    {"Vitreo_D3D7Dev_AddRef", 4},
    {"Vitreo_D3D7Dev_Release", 4},
    {"Vitreo_D3D7Dev_GetCaps", 8},
    {"Vitreo_D3D7Dev_EnumTextureFormats", 12},
    {"Vitreo_D3D7Dev_BeginScene", 4},
    {"Vitreo_D3D7Dev_EndScene", 4},
    {"Vitreo_D3D7Dev_GetDirect3D", 8},
    {"Vitreo_D3D7Dev_SetRenderTarget", 12},
    {"Vitreo_D3D7Dev_GetRenderTarget", 8},
    {"Vitreo_D3D7Dev_Clear", 28},
    {"Vitreo_D3D7Dev_SetTransform", 12},
    {"Vitreo_D3D7Dev_GetTransform", 12},
    {"Vitreo_D3D7Dev_SetViewport", 8},
    {"Vitreo_D3D7Dev_MultiplyTransform", 12},
    {"Vitreo_D3D7Dev_GetViewport", 8},
    {"Vitreo_D3D7Dev_SetMaterial", 8},
    {"Vitreo_D3D7Dev_GetMaterial", 8},
    {"Vitreo_D3D7Dev_SetLight", 12},
    {"Vitreo_D3D7Dev_GetLight", 12},
    {"Vitreo_D3D7Dev_SetRenderState", 12},
    {"Vitreo_D3D7Dev_GetRenderState", 12},
    {"Vitreo_D3D7Dev_BeginStateBlock", 4},
    {"Vitreo_D3D7Dev_EndStateBlock", 8},
    {"Vitreo_D3D7Dev_PreLoad", 8},
    {"Vitreo_D3D7Dev_DrawPrimitive", 24},
    {"Vitreo_D3D7Dev_DrawIndexedPrimitive", 32},
    {"Vitreo_D3D7Dev_SetClipStatus", 8},
    {"Vitreo_D3D7Dev_GetClipStatus", 8},
    {"Vitreo_D3D7Dev_DrawPrimitiveStrided", 24},
    {"Vitreo_D3D7Dev_DrawIndexedPrimitiveStrided", 32},
    {"Vitreo_D3D7Dev_DrawPrimitiveVB", 24},
    {"Vitreo_D3D7Dev_DrawIndexedPrimitiveVB", 32},
    {"Vitreo_D3D7Dev_ComputeSphereVisibility", 24},
    {"Vitreo_D3D7Dev_GetTexture", 12},
    {"Vitreo_D3D7Dev_SetTexture", 12},
    {"Vitreo_D3D7Dev_GetTextureStageState", 16},
    {"Vitreo_D3D7Dev_SetTextureStageState", 16},
    {"Vitreo_D3D7Dev_ValidateDevice", 8},
    {"Vitreo_D3D7Dev_ApplyStateBlock", 8},
    {"Vitreo_D3D7Dev_CaptureStateBlock", 8},
    {"Vitreo_D3D7Dev_DeleteStateBlock", 8},
    {"Vitreo_D3D7Dev_CreateStateBlock", 12},
    {"Vitreo_D3D7Dev_Load", 24},
    {"Vitreo_D3D7Dev_LightEnable", 12},
    {"Vitreo_D3D7Dev_GetLightEnable", 12},
    {"Vitreo_D3D7Dev_SetClipPlane", 12},
    {"Vitreo_D3D7Dev_GetClipPlane", 12},
    {"Vitreo_D3D7Dev_GetInfo", 16},
}};

/// IDirectDrawClipper (ddraw.h). Nine slots: IUnknown's three followed by
/// GetClipList, GetHWnd, Initialize, IsClipListChanged, SetClipList, SetHWnd
/// in that documented order.
inline constexpr u32 IDIRECTDRAWCLIPPER_SLOTS = 9;
inline constexpr std::array<MethodSlot, IDIRECTDRAWCLIPPER_SLOTS> METHOD_SLOTS_CLIPPER = {{
    {"Vitreo_DDClipper_QueryInterface", 12},
    {"Vitreo_DDClipper_AddRef", 4},
    {"Vitreo_DDClipper_Release", 4},
    {"Vitreo_DDClipper_GetClipList", 16},
    {"Vitreo_DDClipper_GetHWnd", 8},
    {"Vitreo_DDClipper_Initialize", 12},
    {"Vitreo_DDClipper_IsClipListChanged", 8},
    {"Vitreo_DDClipper_SetClipList", 12},
    {"Vitreo_DDClipper_SetHWnd", 12},
}};

struct Guid {
    u32 data1;
    u16 data2;
    u16 data3;
    u8 data4[8];
};

inline constexpr Guid IID_IUNKNOWN{
    0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
inline constexpr Guid IID_IDIRECTDRAW{
    0x6C14DB80u, 0xA733, 0x11CE, {0xA5, 0x21, 0, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
inline constexpr Guid IID_IDIRECTDRAWSURFACE{
    0x6C14DB81u, 0xA733, 0x11CE, {0xA5, 0x21, 0, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
inline constexpr Guid IID_IDIRECTDRAWSURFACE2{
    0x57805885u, 0x6EEC, 0x11CF, {0x94, 0x41, 0xA8, 0x23, 0x03, 0xC1, 0x0E, 0x27}};
inline constexpr Guid IID_IDIRECTDRAWSURFACE3{
    0xDA044E00u, 0x69B2, 0x11D0, {0xA1, 0xD5, 0x00, 0xAA, 0x00, 0xB8, 0xDF, 0xBB}};
inline constexpr Guid IID_IDIRECTDRAW2{
    0xB3A6F3E0u, 0x2B43, 0x11CF, {0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56}};

// IID_IDirect3DDevice7 {F5049E79-4861-11D2-A407-00A0C90629A8}
inline constexpr Guid IID_IDIRECT3DDEVICE7{
    0xf5049e79u, 0x4861, 0x11d2,
    {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};

// The device GUIDs Direct3D 7 defines. CreateDevice takes one of these as its
// class id, and EnumDevices reports the subset a driver can serve.
inline constexpr Guid IID_IDIRECT3DHALDEVICE{
    0x84e63de0u, 0x46aa, 0x11cf,
    {0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e}};
inline constexpr Guid IID_IDIRECT3DTNLHALDEVICE{
    0xf5049e78u, 0x4861, 0x11d2,
    {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};
inline constexpr Guid IID_IDIRECT3DRGBDEVICE{
    0xa4665c60u, 0x2673, 0x11cf,
    {0xa3, 0x1a, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56}};

/// Which of the three devices a caller asked for. The distinction is reported
/// back in D3DDEVICEDESC7 (hardware vs. software vertex processing); every one
/// of them is served by the same fixed-function translation underneath.
enum class DeviceKind : u32 { Hal = 0, TnLHal = 1, Rgb = 2 };

inline constexpr Guid IID_IDIRECTDRAWCLIPPER{
    0x6C14DB85u, 0xA733, 0x11CE, {0xA5, 0x21, 0, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};

inline bool guid_equal(const Guid& left, const Guid& right) {
    return std::memcmp(&left, &right, sizeof(Guid)) == 0;
}

} // namespace vitreo::winapi::ddraw_internal
