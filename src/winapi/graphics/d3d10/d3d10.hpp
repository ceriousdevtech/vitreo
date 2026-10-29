#pragma once
/// @file d3d10.hpp
/// @brief d3d10.dll / d3d10_1.dll -- the guest-visible Direct3D 10 surface.
///
/// On Windows 8 and later, Direct3D 10 is layered on the Direct3D 11 runtime:
/// D3D10CreateDevice(1) creates a D3D11 device whose immediate context runs a
/// D3D10 context state (ID3D11Device1::CreateDeviceContextState with
/// EmulatedInterface __uuidof(ID3D10Device1)), and every D3D10 object is the
/// D3D11 object itself -- it answers the D3D10, D3D11 and DXGI interfaces
/// with one COM identity (measured, tools/oracle_probe/d3d10_contract_probe).
/// Vitreo does the same: the D3D10 interfaces are facets of D3D11Manager's
/// objects, and their methods run D3D11Manager's implementation
/// (d3d10_device.cpp). This header holds the module-level constants and the
/// opt-out knob; the exports are in d3d10.cpp.

#include "winapi/graphics/d3d10/d3d10_stdcall.hpp"

#include <vitreo/common.hpp>

#include <array>

namespace vitreo::winapi::d3d10 {

/// d3d10.h / d3d10_1.h: the SDK versions D3D10CreateDevice and
/// D3D10CreateDevice1 require (measured: any other value is E_INVALIDARG).
inline constexpr u32 SDK_VERSION = 29;
inline constexpr u32 SDK_VERSION_10_1 = 0x20;

/// D3D10_DRIVER_TYPE (d3d10.h). Not D3D_DRIVER_TYPE: there is no UNKNOWN,
/// and every value is one lower.
inline constexpr u32 DRIVER_TYPE_HARDWARE = 0;
inline constexpr u32 DRIVER_TYPE_REFERENCE = 1;
inline constexpr u32 DRIVER_TYPE_NULL = 2;
inline constexpr u32 DRIVER_TYPE_SOFTWARE = 3;
inline constexpr u32 DRIVER_TYPE_WARP = 5;

/// D3D10_FEATURE_LEVEL1 (d3d10_1.h).
inline constexpr u32 FEATURE_LEVEL_9_1 = 0x9100u;
inline constexpr u32 FEATURE_LEVEL_9_2 = 0x9200u;
inline constexpr u32 FEATURE_LEVEL_9_3 = 0x9300u;
inline constexpr u32 FEATURE_LEVEL_10_0 = 0xA000u;
inline constexpr u32 FEATURE_LEVEL_10_1 = 0xA100u;

/// D3D10GetVersion / D3D10CoreGetVersion: a 64-bit value (EDX:EAX on x86),
/// read from the disassembly of d3d10core.dll on Windows ("mov
/// rax,0A000100041770h"); d3d10.dll's export jumps to it.
inline constexpr u64 RUNTIME_VERSION = 0x000A000100041770ull;

/// D3D10_STATE_BLOCK_MASK (d3d10effect.h): 76 bytes, one bit per state
/// entry. kStateMaskRanges[D3D10_DEVICE_STATE_TYPES] is the byte offset of
/// the type's bits and how many entries it has (measured,
/// d3d10_contract_probe [sb]: EnableCapture of each type's entry 0 sets the
/// bit at that byte; one past the last entry is E_INVALIDARG). Type 0 and
/// types above 24 do not exist.
inline constexpr usize STATE_BLOCK_MASK_BYTES = 76;
struct StateMaskRange {
    u32 offset;
    u32 entries;
};
inline constexpr std::array<StateMaskRange, 25> kStateMaskRanges{{
    {0, 0},
    {74, 1},    // D3D10_DST_SO_BUFFERS
    {68, 1},    // D3D10_DST_OM_RENDER_TARGETS
    {69, 1},    // D3D10_DST_OM_DEPTH_STENCIL_STATE
    {70, 1},    // D3D10_DST_OM_BLEND_STATE
    {0, 1},     // D3D10_DST_VS
    {1, 16},    // D3D10_DST_VS_SAMPLERS
    {3, 128},   // D3D10_DST_VS_SHADER_RESOURCES
    {19, 14},   // D3D10_DST_VS_CONSTANT_BUFFERS
    {21, 1},    // D3D10_DST_GS
    {22, 16},   // D3D10_DST_GS_SAMPLERS
    {24, 128},  // D3D10_DST_GS_SHADER_RESOURCES
    {40, 14},   // D3D10_DST_GS_CONSTANT_BUFFERS
    {42, 1},    // D3D10_DST_PS
    {43, 16},   // D3D10_DST_PS_SAMPLERS
    {45, 128},  // D3D10_DST_PS_SHADER_RESOURCES
    {61, 14},   // D3D10_DST_PS_CONSTANT_BUFFERS
    {63, 16},   // D3D10_DST_IA_VERTEX_BUFFERS
    {65, 1},    // D3D10_DST_IA_INDEX_BUFFER
    {66, 1},    // D3D10_DST_IA_INPUT_LAYOUT
    {67, 1},    // D3D10_DST_IA_PRIMITIVE_TOPOLOGY
    {71, 1},    // D3D10_DST_RS_VIEWPORTS
    {72, 1},    // D3D10_DST_RS_SCISSOR_RECTS
    {73, 1},    // D3D10_DST_RS_RASTERIZER_STATE
    {75, 1},    // D3D10_DST_PREDICATION
}};
/// Whether entry @p index of state type @p type is set in @p mask.
[[nodiscard]] inline bool state_mask_bit(const u8* mask, u32 type,
                                         u32 index) {
    if (type == 0 || type >= kStateMaskRanges.size()) return false;
    const StateMaskRange range = kStateMaskRanges[type];
    if (index >= range.entries) return false;
    return (mask[range.offset + index / 8] & (1u << (index % 8))) != 0;
}

/// Whether this process serves D3D10 devices. On by default; `VITREO_D3D10=0`
/// turns device creation off, which then answers DXGI_ERROR_UNSUPPORTED the
/// way a machine without a D3D10-capable adapter does (and
/// IDXGIAdapter::CheckInterfaceSupport agrees).
[[nodiscard]] bool d3d10_enabled();

} // namespace vitreo::winapi::d3d10
