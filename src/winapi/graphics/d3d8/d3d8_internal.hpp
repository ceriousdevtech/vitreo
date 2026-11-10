// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d8_internal.hpp
/// @brief State shared by the D3D8 frontend translation units.

#include "winapi/graphics/d3d8/d3d8.hpp"
#include "winapi/graphics/d3d8/d3d8_contract.hpp"

#include <array>
#include <map>
#include <vector>

namespace vitreo::winapi {

namespace d3d8 {

constexpr u32 S_FALSE = 1;
constexpr u32 D3DERR_NOTAVAILABLE = 0x8876086Au;
constexpr u32 D3DERR_NOTFOUND = 0x88760866u;
constexpr u32 E_NOINTERFACE = 0x80004002u;
constexpr u32 E_POINTER = 0x80004003u;
constexpr u32 E_NOTIMPL = 0x80004001u;
constexpr u32 E_FAIL = 0x80004005u;
constexpr u32 E_OUTOFMEMORY = 0x8007000Eu;

/// @p base plus a generated (non-negative) structure offset.
[[nodiscard]] constexpr GuestAddr at(GuestAddr base, i32 offset) {
    return base + static_cast<u32>(offset);
}

/// The D3D9 SDK version the frontend creates its IDirect3D9 with.
constexpr u32 D3D9_SDK_VERSION = 32;

/// D3D9 slot of a method, resolved by name from the D3D9 vtable tables once.
[[nodiscard]] usize d3d9_slot(D3D9InterfaceId iface, const char* name);

}  // namespace d3d8

using d3d8::at;

#define D3D8_SLOT9(IFACE, NAME)                                               \
    ([]() -> usize {                                                          \
        static const usize slot =                                             \
            ::vitreo::winapi::d3d8::d3d9_slot(D3D9InterfaceId::IFACE, #NAME); \
        return slot;                                                          \
    }())

/// One D3D8 vertex shader handle: the declaration (D3DVSD_* tokens), the
/// optional vs.1.x function, and the D3D9 objects that realise them.
struct D3D8Manager::VertexShader {
    std::vector<u32> declaration;  ///< Including the D3DVSD_END token.
    std::vector<u32> function;     ///< Empty for a fixed-function declaration.
    u32 usage{0};
    u32 declaration9{0};           ///< IDirect3DVertexDeclaration9.
    u32 shader9{0};                ///< IDirect3DVertexShader9 or 0.
    /// D3DVSD_CONST: constant registers loaded when the shader is set.
    std::vector<std::pair<u32, std::array<u32, 4>>> constants;
};

struct D3D8Manager::PixelShader {
    std::vector<u32> function;
    u32 shader9{0};
};

/// D3D8 state the D3D9 state block cannot hold: the index buffer's
/// BaseVertexIndex and the render states D3D9 removed.
struct D3D8Manager::StateBlock {
    u32 block9{0};
    bool has_base_vertex_index{false};
    u32 base_vertex_index{0};
    std::map<u32, u32> render_states8;  ///< D3D8-only render states.
};

struct D3D8Manager::DeviceState {
    u32 direct3d8{0};       ///< The IDirect3D8 that created the device.
    u32 behavior_flags{0};
    /// D3DDEVTYPE the device was created as (REF runs on the HAL device).
    u32 device_type{1};
    u32 base_vertex_index{0};
    /// Render states D3D9 has no slot for (LINEPATTERN, ZVISIBLE,
    /// EDGEANTIALIAS, ZBIAS, SOFTWAREVERTEXPROCESSING, PATCHSEGMENTS):
    /// the value Get returns.
    std::map<u32, u32> render_states8;
    /// Render states and texture-stage types d3d8types.h does not define,
    /// below 256 / 32: retail D3D8 accepts and returns them (measured).
    std::map<u32, u32> unknown_render_states;
    std::map<u32, u32> unknown_stage_states;  ///< key: stage << 8 | type
    std::map<u32, VertexShader> vertex_shaders;
    std::map<u32, PixelShader> pixel_shaders;
    std::map<u32, StateBlock> state_blocks;
    u32 next_vertex_shader{0};
    u32 next_pixel_shader{0};
    u32 next_state_block{0};
    /// Begin/EndStateBlock recording in progress.
    bool recording{false};
    StateBlock recorded{};
    /// A SetCursorProperties succeeded: ShowCursor drives the device cursor.
    /// Before that, ShowCursor neither touches the Win32 cursor nor
    /// remembers the request, and returns FALSE (d3d8_device_cursor_probe
    /// "noprops.*": showing=1, handle unchanged, prev=0 after TRUE).
    bool cursor_set{false};
};

}  // namespace vitreo::winapi
