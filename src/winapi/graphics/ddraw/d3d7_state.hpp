// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d7_state.hpp
/// @brief IDirect3DDevice7 fixed-function state machine.
///
/// Direct3D 7 has no programmable stages: every draw is described entirely by
/// transforms, lights, a material, render states and texture-stage combiners.
/// That is the same description `graphics::FFPipelineState` already carries for
/// the D3D9 fixed-function pipeline, so this file keeps the guest-visible D3D7
/// state and translates it into that shared structure. The MSL generator in
/// `graphics/shaders/ff_emulation.hpp` is the single shader source for both.
///
/// The translation is mostly an identity map rather than a remap, because
/// D3D8/9 inherited D3D7's fixed-function contract wholesale: D3DRENDERSTATE_*
/// and D3DTSS_* keep the same numeric values in D3D9's D3DRS_*/D3DTSS_*, and
/// D3DLIGHT7/D3DMATERIAL7/D3DVIEWPORT7 are byte-identical to their D3D9
/// successors. The places where the two genuinely differ (the WORLD transform
/// index, the SPECULARENABLE default) are called out at their use sites.

#include <vitreo/common.hpp>
#include "graphics/shaders/ff_emulation.hpp"
#include "graphics/shaders/ff_uniforms.hpp"

#include <array>
#include <map>
#include <unordered_map>

namespace vitreo::winapi::d3d7 {

// =============================================================================
// D3D7 enumerations
// =============================================================================

/// D3DTRANSFORMSTATETYPE. Note WORLD is 1 here; D3D9 renumbered it to 256 and
/// left VIEW/PROJECTION/TEXTURE0 alone, so transform indices are the one part
/// of the fixed-function contract that must be translated rather than passed
/// through.
namespace ts {
constexpr u32 WORLD = 1;
constexpr u32 VIEW = 2;
constexpr u32 PROJECTION = 3;
constexpr u32 WORLD1 = 4;
constexpr u32 WORLD2 = 5;
constexpr u32 WORLD3 = 6;
constexpr u32 TEXTURE0 = 16;
constexpr u32 TEXTURE7 = 23;
} // namespace ts

/// D3DRENDERSTATETYPE. Only the states that survived into D3D8/9 are named;
/// the DX3-era ones (TEXTUREHANDLE, ROP2, PLANEMASK, ...) are still accepted
/// and returned by Get, they simply do not describe anything the shared
/// pipeline state can express.
namespace rs {
constexpr u32 ZENABLE = 7;
constexpr u32 FILLMODE = 8;
constexpr u32 SHADEMODE = 9;
constexpr u32 ZWRITEENABLE = 14;
constexpr u32 ALPHATESTENABLE = 15;
constexpr u32 LASTPIXEL = 16;
constexpr u32 SRCBLEND = 19;
constexpr u32 DESTBLEND = 20;
constexpr u32 CULLMODE = 22;
constexpr u32 ZFUNC = 23;
constexpr u32 ALPHAREF = 24;
constexpr u32 ALPHAFUNC = 25;
constexpr u32 DITHERENABLE = 26;
constexpr u32 ALPHABLENDENABLE = 27;
constexpr u32 FOGENABLE = 28;
constexpr u32 SPECULARENABLE = 29;
constexpr u32 FOGCOLOR = 34;
constexpr u32 FOGTABLEMODE = 35;
constexpr u32 FOGSTART = 36;
constexpr u32 FOGEND = 37;
constexpr u32 FOGDENSITY = 38;
constexpr u32 EDGEANTIALIAS = 40;
constexpr u32 COLORKEYENABLE = 41;
constexpr u32 ZBIAS = 47;
constexpr u32 RANGEFOGENABLE = 48;
constexpr u32 STENCILENABLE = 52;
constexpr u32 STENCILFAIL = 53;
constexpr u32 STENCILZFAIL = 54;
constexpr u32 STENCILPASS = 55;
constexpr u32 STENCILFUNC = 56;
constexpr u32 STENCILREF = 57;
constexpr u32 STENCILMASK = 58;
constexpr u32 STENCILWRITEMASK = 59;
constexpr u32 TEXTUREFACTOR = 60;
constexpr u32 WRAP0 = 128;
constexpr u32 WRAP7 = 135;
constexpr u32 CLIPPING = 136;
constexpr u32 LIGHTING = 137;
constexpr u32 EXTENTS = 138;
constexpr u32 AMBIENT = 139;
constexpr u32 FOGVERTEXMODE = 140;
constexpr u32 COLORVERTEX = 141;
constexpr u32 LOCALVIEWER = 142;
constexpr u32 NORMALIZENORMALS = 143;
constexpr u32 DIFFUSEMATERIALSOURCE = 145;
constexpr u32 SPECULARMATERIALSOURCE = 146;
constexpr u32 AMBIENTMATERIALSOURCE = 147;
constexpr u32 EMISSIVEMATERIALSOURCE = 148;
constexpr u32 VERTEXBLEND = 151;
constexpr u32 CLIPPLANEENABLE = 152;
constexpr u32 MAX = 153;
} // namespace rs

/// D3DTEXTURESTAGESTATETYPE. D3D9 moved 12..21 to sampler state but kept the
/// surviving members at the same indices, so the numbering below is shared.
namespace tss {
constexpr u32 COLOROP = 1;
constexpr u32 COLORARG1 = 2;
constexpr u32 COLORARG2 = 3;
constexpr u32 ALPHAOP = 4;
constexpr u32 ALPHAARG1 = 5;
constexpr u32 ALPHAARG2 = 6;
constexpr u32 BUMPENVMAT00 = 7;
constexpr u32 BUMPENVMAT01 = 8;
constexpr u32 BUMPENVMAT10 = 9;
constexpr u32 BUMPENVMAT11 = 10;
constexpr u32 TEXCOORDINDEX = 11;
constexpr u32 ADDRESS = 12;
constexpr u32 ADDRESSU = 13;
constexpr u32 ADDRESSV = 14;
constexpr u32 BORDERCOLOR = 15;
constexpr u32 MAGFILTER = 16;
constexpr u32 MINFILTER = 17;
constexpr u32 MIPFILTER = 18;
constexpr u32 MIPMAPLODBIAS = 19;
constexpr u32 MAXMIPLEVEL = 20;
constexpr u32 MAXANISOTROPY = 21;
constexpr u32 BUMPENVLSCALE = 22;
constexpr u32 BUMPENVLOFFSET = 23;
constexpr u32 TEXTURETRANSFORMFLAGS = 24;
constexpr u32 MAX = 25;
} // namespace tss

/// D3DCLEAR_* flags accepted by IDirect3DDevice7::Clear.
namespace clear_flag {
constexpr u32 TARGET = 0x1;
constexpr u32 ZBUFFER = 0x2;
constexpr u32 STENCIL = 0x4;
} // namespace clear_flag

/// D3DPRIMITIVETYPE.
namespace prim {
constexpr u32 POINTLIST = 1;
constexpr u32 LINELIST = 2;
constexpr u32 LINESTRIP = 3;
constexpr u32 TRIANGLELIST = 4;
constexpr u32 TRIANGLESTRIP = 5;
constexpr u32 TRIANGLEFAN = 6;
} // namespace prim

/// The device supports eight simultaneously active lights and eight texture
/// blending stages, which is what `FFPipelineState` carries and what
/// D3DDEVICEDESC7 reports back through GetCaps.
constexpr u32 MAX_ACTIVE_LIGHTS = 8;
constexpr u32 MAX_TEXTURE_STAGES = 8;
/// D3DMAXUSERCLIPPLANES.
constexpr u32 MAX_CLIP_PLANES = 6;

// =============================================================================
// State
// =============================================================================

/// A D3D matrix in its guest memory order (row-major, m[row][column]).
using Mat4 = std::array<float, 16>;

[[nodiscard]] Mat4 identity_matrix();

/// D3DVIEWPORT7.
struct Viewport {
    u32 x{0};
    u32 y{0};
    u32 width{0};
    u32 height{0};
    float min_z{0.0f};
    float max_z{1.0f};
};

/// One light slot. D3DLIGHT7 is field-for-field the same shape as D3DLIGHT9,
/// so the shared FFLight is the storage rather than an intermediate copy;
/// only the enable bit is tracked separately because SetLight and LightEnable
/// are independent calls in D3D7.
struct LightSlot {
    graphics::FFLight light{};
    bool enabled{false};
};

/// Guest-visible IDirect3DDevice7 state.
///
/// Render and texture-stage states are kept as the raw DWORDs the guest set so
/// GetRenderState / GetTextureStageState return exactly what was written —
/// including the DX3-era and sampler states that no longer describe anything
/// in the shared pipeline description.
struct DeviceState {
    /// Keyed by D3DTRANSFORMSTATETYPE, so WORLD..WORLD3 and TEXTURE0..7 all
    /// live here under their D3D7 indices.
    std::unordered_map<u32, Mat4> transforms;
    Viewport viewport{};
    bool viewport_set{false};

    std::unordered_map<u32, u32> render_states;
    std::array<std::unordered_map<u32, u32>, MAX_TEXTURE_STAGES> stage_states;

    /// D3D7 does not bound the light index a guest may set, only how many may
    /// be enabled at once, so the container is sparse and ordered by index.
    std::map<u32, LightSlot> lights;
    graphics::FFMaterial material{};

    /// IDirectDrawSurface7 bound per stage (0 = none).
    std::array<GuestAddr, MAX_TEXTURE_STAGES> textures{};
    std::array<std::array<float, 4>, MAX_CLIP_PLANES> clip_planes{};

    /// The IDirectDrawSurface7 this device renders into.
    GuestAddr render_target{0};
    bool in_scene{false};

    /// D3DCLIPSTATUS { dwFlags, dwStatus, minx, maxx, miny, maxy, minz, maxz },
    /// stored verbatim so GetClipStatus returns what SetClipStatus was given.
    /// Nothing consumes it: the extents a driver would report come from
    /// clipping the guest actually performed.
    u32 clip_status_flags{0};
    u32 clip_status{0};
    float clip_status_extents[6]{0, 0, 0, 0, 0, 0};

    [[nodiscard]] u32 render_state(u32 which) const;
    [[nodiscard]] float render_state_float(u32 which) const;
    [[nodiscard]] u32 stage_state(u32 stage, u32 which) const;
    [[nodiscard]] const Mat4& transform(u32 which) const;
};

/// Documented D3D7 power-on value for @p which. Anything not listed reads back
/// as zero, which is what an unset state returns.
[[nodiscard]] u32 default_render_state(u32 which);

/// Documented D3D7 power-on value for a texture stage state. Stage 0 differs
/// from the rest: it starts with a MODULATE colour op so a single bound
/// texture draws without any SetTextureStageState call at all.
[[nodiscard]] u32 default_stage_state(u32 stage, u32 which);

// =============================================================================
// Translation to the shared fixed-function shader state
// =============================================================================

/// Build the `FFPipelineState` describing @p s for a draw whose vertices are
/// declared by @p fvf, so the existing fixed-function MSL generator can
/// produce shaders for it.
///
/// @p fvf is D3D7's dwVertexTypeDesc, which uses the same D3DFVF_* bits the
/// generator already consumes.
/// Build the uniform payload `pack_ff_vertex_uniforms` / `pack_ff_fragment_uniforms`
/// expect for this device.
///
/// `state_to_ff` describes what the generated shader *is*; this describes the
/// values it reads each draw. They are separate because the FFPipelineState
/// hash decides shader reuse, so per-draw values must not participate in it.
[[nodiscard]] graphics::FFUniformInputs state_to_ff_uniform_inputs(
    const DeviceState& s);

[[nodiscard]] graphics::FFPipelineState state_to_ff(const DeviceState& s,
                                                    u32 fvf);

} // namespace vitreo::winapi::d3d7
