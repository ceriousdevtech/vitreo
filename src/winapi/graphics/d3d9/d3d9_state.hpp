// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d9_state.hpp
/// @brief Guest-visible D3D9 object state, shared by the frontend's
///        per-interface dispatch translation units.
///
/// Everything here exists because a guest can observe it: D3D9 state setters
/// are paired with getters, so storing state accurately *is* implementing the
/// contract, not caching. GPU-side objects are referenced through the shared
/// `graphics::GraphicsBackendDevice` types only.

#include "winapi/graphics/d3d9/d3d9.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "graphics/shaders/ff_emulation.hpp"
#include "graphics/shaders/sm3_to_msl.hpp"

#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

// =============================================================================
// Structure sizes and offsets (32-bit guest layout)
// =============================================================================

/// D3DPRESENT_PARAMETERS: 14 four-byte fields.
constexpr usize D3DPRESENT_PARAMETERS_SIZE = 56;
constexpr usize PP_BACK_BUFFER_WIDTH = 0;
constexpr usize PP_BACK_BUFFER_HEIGHT = 4;
constexpr usize PP_BACK_BUFFER_FORMAT = 8;
constexpr usize PP_BACK_BUFFER_COUNT = 12;
constexpr usize PP_MULTI_SAMPLE_TYPE = 16;
constexpr usize PP_MULTI_SAMPLE_QUALITY = 20;
constexpr usize PP_SWAP_EFFECT = 24;
constexpr usize PP_DEVICE_WINDOW = 28;
constexpr usize PP_WINDOWED = 32;
constexpr usize PP_ENABLE_AUTO_DEPTH_STENCIL = 36;
constexpr usize PP_AUTO_DEPTH_STENCIL_FORMAT = 40;
constexpr usize PP_FLAGS = 44;
constexpr usize PP_FULLSCREEN_REFRESH_RATE = 48;
constexpr usize PP_PRESENTATION_INTERVAL = 52;

/// D3DSURFACE_DESC: Format, Type, Usage, Pool, MultiSampleType,
/// MultiSampleQuality, Width, Height.
constexpr usize D3DSURFACE_DESC_SIZE = 32;

/// D3DVOLUME_DESC: Format, Type, Usage, Pool, Width, Height, Depth.
constexpr usize D3DVOLUME_DESC_SIZE = 28;

/// D3DLOCKED_BOX: RowPitch, SlicePitch, pBits (32-bit guest).
constexpr usize D3DLOCKED_BOX_SIZE = 12;

/// D3DVERTEXBUFFER_DESC: Format, Type, Usage, Pool, Size, FVF.
constexpr usize D3DVERTEXBUFFER_DESC_SIZE = 24;

/// D3DINDEXBUFFER_DESC: Format, Type, Usage, Pool, Size.
constexpr usize D3DINDEXBUFFER_DESC_SIZE = 20;

/// D3DDEVICE_CREATION_PARAMETERS: AdapterOrdinal, DeviceType, hFocusWindow,
/// BehaviorFlags.
constexpr usize D3DDEVICE_CREATION_PARAMETERS_SIZE = 16;

/// D3DDISPLAYMODE: Width, Height, RefreshRate, Format.
constexpr usize D3DDISPLAYMODE_SIZE = 16;

/// D3DRASTER_STATUS: BOOL InVBlank, UINT ScanLine.
constexpr usize D3DRASTER_STATUS_SIZE = 8;

/// D3DGAMMARAMP: WORD red[256], green[256], blue[256].
constexpr usize D3DGAMMARAMP_CHANNEL_ENTRIES = 256;
constexpr usize D3DGAMMARAMP_SIZE =
    D3DGAMMARAMP_CHANNEL_ENTRIES * 3 * sizeof(u16);

/// Fill a D3DGAMMARAMP with the identity mapping (WORD i → (i << 8) | i).
inline void d3d9_fill_identity_gamma_ramp(
    std::array<u16, D3DGAMMARAMP_CHANNEL_ENTRIES * 3>& ramp) noexcept {
    for (u32 i = 0; i < D3DGAMMARAMP_CHANNEL_ENTRIES; ++i) {
        const u16 value = static_cast<u16>((i << 8) | i);
        ramp[i] = value;
        ramp[D3DGAMMARAMP_CHANNEL_ENTRIES + i] = value;
        ramp[D3DGAMMARAMP_CHANNEL_ENTRIES * 2 + i] = value;
    }
}

[[nodiscard]] inline bool d3d9_gamma_ramp_is_identity(
    const std::array<u16, D3DGAMMARAMP_CHANNEL_ENTRIES * 3>& ramp) noexcept {
    for (u32 i = 0; i < D3DGAMMARAMP_CHANNEL_ENTRIES; ++i) {
        const u16 value = static_cast<u16>((i << 8) | i);
        if (ramp[i] != value ||
            ramp[D3DGAMMARAMP_CHANNEL_ENTRIES + i] != value ||
            ramp[D3DGAMMARAMP_CHANNEL_ENTRIES * 2 + i] != value) {
            return false;
        }
    }
    return true;
}

/// Pack D3DGAMMARAMP into 256 tightly packed RGBA8 texels (R=red[i]>>8).
inline void d3d9_gamma_ramp_to_rgba8(
    const std::array<u16, D3DGAMMARAMP_CHANNEL_ENTRIES * 3>& ramp,
    std::array<u8, D3DGAMMARAMP_CHANNEL_ENTRIES * 4>& out) noexcept {
    for (u32 i = 0; i < D3DGAMMARAMP_CHANNEL_ENTRIES; ++i) {
        out[i * 4 + 0] = static_cast<u8>(ramp[i] >> 8);
        out[i * 4 + 1] =
            static_cast<u8>(ramp[D3DGAMMARAMP_CHANNEL_ENTRIES + i] >> 8);
        out[i * 4 + 2] =
            static_cast<u8>(ramp[D3DGAMMARAMP_CHANNEL_ENTRIES * 2 + i] >> 8);
        out[i * 4 + 3] = 255;
    }
}

// =============================================================================
// Cull mode
// =============================================================================

/// D3DCULL — Learn D3DRS_CULLMODE. The value names the winding that is
/// *discarded*, not the winding that faces front.
/// https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dcull
constexpr u32 D3DCULL_NONE = 1;
constexpr u32 D3DCULL_CW = 2;
constexpr u32 D3DCULL_CCW = 3;

/// A D3DRS_CULLMODE value translated into the backend's two independent
/// settings. Metal (and the shared backend contract) split culling into "which
/// winding is the front face" and "which face is discarded"; D3D9 folds both
/// into one enum, so the two must always be produced together.
struct D3D9CullState {
    graphics::BackendCullMode cull_mode{graphics::BackendCullMode::None};
    bool front_counter_clockwise{false};
};

/// Translate D3DRS_CULLMODE into a (front face, culled face) pair.
///
/// Vitreo's canonical D3D9 front face is clockwise, matching D3D9's own
/// left-handed screen-space convention: neither the clip-space Y axis nor the
/// framebuffer origin differs between D3D9 and Metal, so a triangle's screen
/// winding survives the translation unchanged and needs no compensating flip.
/// With CW defined as front, D3DCULL_CW ("discard clockwise triangles")
/// discards the front face and D3DCULL_CCW discards the back face.
[[nodiscard]] inline D3D9CullState translate_d3d9_cull_mode(u32 mode) noexcept {
    D3D9CullState result{};
    result.front_counter_clockwise = false;
    switch (mode) {
    case D3DCULL_CW:
        result.cull_mode = graphics::BackendCullMode::Front;
        break;
    case D3DCULL_CCW:
        result.cull_mode = graphics::BackendCullMode::Back;
        break;
    default:
        // D3DCULL_NONE and any out-of-range value the guest may have set.
        result.cull_mode = graphics::BackendCullMode::None;
        break;
    }
    return result;
}

/// D3DMATERIAL9: four D3DCOLORVALUEs plus Power.
constexpr usize D3DMATERIAL9_SIZE = 68;

/// D3DLIGHT9.
constexpr usize D3DLIGHT9_SIZE = 104;

/// D3DLIGHT_DIRECTIONAL — Learn D3DLIGHTTYPE.
constexpr u32 D3DLIGHT_DIRECTIONAL = 3;

/// Learn LightEnable defaults for a light that was never SetLight:
/// Type=DIRECTIONAL, Diffuse=(1,1,1,0), Direction=(0,0,1), all else zero.
/// https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-lightenable
[[nodiscard]] inline std::array<u8, D3DLIGHT9_SIZE> default_d3dlight9() noexcept {
    std::array<u8, D3DLIGHT9_SIZE> light{};
    const auto write_u32 = [&](usize offset, u32 value) {
        std::memcpy(light.data() + offset, &value, sizeof(value));
    };
    const auto write_f32 = [&](usize offset, float value) {
        std::memcpy(light.data() + offset, &value, sizeof(value));
    };
    write_u32(0, D3DLIGHT_DIRECTIONAL);
    write_f32(4, 1.0f);   // Diffuse.r
    write_f32(8, 1.0f);   // Diffuse.g
    write_f32(12, 1.0f);  // Diffuse.b
    write_f32(16, 0.0f);  // Diffuse.a
    write_f32(72, 1.0f);  // Direction.z  (Position/Direction xyz zero except z=1)
    return light;
}

/// D3DCLIPSTATUS9: ClipUnion + ClipIntersection (two DWORDs).
constexpr usize D3DCLIPSTATUS9_SIZE = 8;

constexpr usize D3D9_MAX_TEXTURE_STAGES = 8;
constexpr usize D3D9_MAX_SAMPLERS = 16;
/// vs_3_0 exposes four sampler stages at D3DDMAPSAMPLER + 1 through + 4.
constexpr u32 D3D9_VERTEX_TEXTURE_SAMPLER_FIRST = 257;
constexpr usize D3D9_VERTEX_TEXTURE_SAMPLER_COUNT = 4;
constexpr usize D3D9_MAX_STREAMS = 16;
constexpr usize D3D9_MAX_CLIP_PLANES = 6;
constexpr usize D3D9_MAX_SHADER_CONST_F = 256;
/// VS/PS integer constant slots (four ints each). SM 2.0/3.0 expose 16.
constexpr usize D3D9_MAX_SHADER_CONST_I = 16;
/// VS/PS boolean constant slots. SM 2.0/3.0 expose 16.
constexpr usize D3D9_MAX_SHADER_CONST_B = 16;

/// Reinterpret a float as the DWORD a guest reads back from a state getter.
[[nodiscard]] inline u32 f32_bits(float value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/// Windows-documented initial value of a render state, or 0.
[[nodiscard]] u32 d3d9_default_render_state(u32 state,
                                            bool auto_depth_stencil) noexcept;

/// Windows-documented initial value of a texture stage state for @p stage.
[[nodiscard]] u32 d3d9_default_texture_stage_state(u32 stage,
                                                   u32 type) noexcept;

/// Windows-documented initial value of a sampler state.
[[nodiscard]] u32 d3d9_default_sampler_state(u32 type) noexcept;

/// Bytes per pixel of a D3DFORMAT Vitreo can represent, or 0.
[[nodiscard]] u32 d3d9_format_bytes_per_pixel(u32 format) noexcept;

/// Shared-core pixel format for a D3DFORMAT, or PixelFormat::Unknown.
[[nodiscard]] graphics::PixelFormat d3d9_backend_format(u32 format) noexcept;

/// True when CheckDevice* / Create* may accept @p format as guest-visible.
[[nodiscard]] bool d3d9_format_supported(u32 format) noexcept;

/// Expand a guest D3DFORMAT lock buffer into tightly packed BGRA8 for upload
/// into a `PixelFormat::BGRA8Unorm` backend texture. Returns false when the
/// format needs no expansion (caller should upload @p src as-is) or on bad
/// dimensions / pitch.
[[nodiscard]] bool d3d9_expand_lock_to_bgra8(
    u32 format, u32 width, u32 height, const u8* src, u32 src_pitch,
    std::vector<u8>& out) noexcept;

/// Expand bump formats whose derived/mixed channels have no native shared
/// representation into tightly packed RGBA32Float.
[[nodiscard]] bool d3d9_expand_special_bump_to_rgba32f(
    u32 format, u32 width, u32 height, const u8* src, u32 src_pitch,
    std::vector<u8>& out) noexcept;

/// Learn: D3DFMT_P8 / D3DFMT_A8P8 — sampled via the device texture palette.
[[nodiscard]] bool d3d9_is_palettized_format(u32 format) noexcept;

/// Expand P8/A8P8 indices through 256 PALETTEENTRY (peRed/Green/Blue/Flags)
/// into tightly packed BGRA8. @p palette_entries may be null (defaults
/// 0xFFFFFFFF per Learn). A8P8 uses per-pixel alpha; P8 uses peFlags as
/// alpha (D3DPTEXTURECAPS_ALPHAPALETTE).
[[nodiscard]] bool d3d9_expand_palettized_to_bgra8(
    u32 format, u32 width, u32 height, const u8* src, u32 src_pitch,
    const u8* palette_entries, std::vector<u8>& out) noexcept;

/// Lock pitch (bytes per row of blocks for DXT) and total byte size for a
/// level. Returns false when the format is unknown.
[[nodiscard]] bool d3d9_surface_storage(u32 format, u32 width, u32 height,
                                        u32& pitch_out,
                                        usize& bytes_out) noexcept;

/// Row pitch, slice pitch, and total bytes for one volume mip level.
[[nodiscard]] bool d3d9_volume_storage(u32 format, u32 width, u32 height,
                                       u32 depth, u32& row_pitch_out,
                                       u32& slice_pitch_out,
                                       usize& bytes_out) noexcept;

// =============================================================================
// Per-object state
// =============================================================================

struct D3D9Manager::SurfaceState {
    enum class Kind { BackBuffer, RenderTarget, DepthStencil, Offscreen,
                      TextureLevel };

    Kind kind{Kind::Offscreen};
    u32 width{0};
    u32 height{0};
    u32 format{D3DFMT_UNKNOWN};
    u32 usage{0};
    u32 pool{D3DPOOL_DEFAULT};
    u32 multi_sample_type{D3DMULTISAMPLE_NONE};
    u32 multi_sample_quality{0};

    /// Texture object owning this level, or 0.
    u32 container{0};
    u32 level{0};
    /// D3DCUBEMAP_FACES index when @p container is a cube texture; else 0.
    u32 face{0};

    /// Guest-visible pixels, mapped for the surface's lifetime when the
    /// surface is lockable. D3D9 hands the application a pointer into surface
    /// memory, so the mapping must outlive a single Lock/Unlock pair.
    GuestAddr pixels{0};
    usize pixel_bytes{0};
    u32 pitch{0};
    bool locked{false};
    u32 lock_flags{0};

    /// Shared-core texture for depth-stencil surfaces (and later for
    /// CreateRenderTarget). Texture-level colour RTs use the parent
    /// texture's backend instead.
    std::unique_ptr<graphics::BackendTexture2D> backend;

    /// Non-owning view of the swap-chain colour attachment when this is
    /// the implicit / additional back buffer. StretchRect and ColorFill
    /// need a GPU dest; the swap chain owns the texture.
    graphics::BackendTexture2D* swap_chain_color{nullptr};
};

struct D3D9Manager::TextureState {
    u32 width{0};
    u32 height{0};
    /// Depth extent for IDirect3DVolumeTexture9; 1 for 2D/cube.
    u32 depth{1};
    u32 levels{1};
    u32 usage{0};
    u32 format{D3DFMT_UNKNOWN};
    u32 pool{D3DPOOL_MANAGED};
    u32 lod{0};
    /// Learn: SetAutoGenFilterType default is D3DTEXF_LINEAR (2).
    u32 auto_gen_filter{2};
    u32 priority{0};
    /// True for IDirect3DCubeTexture9 (six faces, square edge length).
    bool cube{false};
    /// True for IDirect3DVolumeTexture9 (3D mip chain).
    bool volume{false};

    std::unique_ptr<graphics::BackendTexture2D> backend;
    /// GPU volume storage for D3DPOOL_DEFAULT IDirect3DVolumeTexture9.
    std::unique_ptr<graphics::BackendTexture3D> backend_3d;
    /// Surface object addresses per level (2D) or per face*level (cube),
    /// or IDirect3DVolume9 addresses per level when @p volume is set.
    /// Cube indexing: face * levels + level.
    std::vector<u32> level_surfaces;

    /// Device `texture_palette_serial` last applied to @p backend for
    /// palettized formats. Zero means never expanded (or pixels changed).
    u64 palettized_upload_serial{0};
};

struct D3D9Manager::VolumeState {
    u32 width{0};
    u32 height{0};
    u32 depth{0};
    u32 format{D3DFMT_UNKNOWN};
    u32 usage{0};
    u32 pool{D3DPOOL_DEFAULT};
    /// Owning IDirect3DVolumeTexture9, or 0.
    u32 container{0};
    u32 level{0};

    GuestAddr pixels{0};
    usize pixel_bytes{0};
    u32 row_pitch{0};
    u32 slice_pitch{0};
    bool locked{false};
    u32 lock_flags{0};
};

struct D3D9Manager::BufferState {
    bool index_buffer{false};
    u32 length{0};
    u32 usage{0};
    /// FVF for a vertex buffer, D3DFORMAT for an index buffer.
    u32 fvf_or_format{0};
    u32 pool{D3DPOOL_MANAGED};
    u32 priority{0};

    std::unique_ptr<graphics::BackendBuffer> backend;

    /// Guest-visible contents, mapped for the buffer's lifetime. Lock returns
    /// `storage + OffsetToLock`; Unlock pushes the locked range to the backend.
    GuestAddr storage{0};
    bool locked{false};
    u32 lock_offset{0};
    u32 lock_size{0};
    u32 lock_flags{0};
};

struct D3D9Manager::ShaderState {
    bool pixel{false};
    /// The token stream exactly as the guest supplied it, so GetFunction can
    /// return what the application handed in.
    std::vector<u32> bytecode;
    graphics::Sm3Shader translated;
};

/// Captured device state for IDirect3DStateBlock9. Begin/End records only the
/// changes made while recording; CreateStateBlock(D3DSBT_ALL) snapshots the
/// whole device. Apply writes the captured fields back.
struct D3D9Manager::StateBlockState {
    u32 type{0}; // D3DSBT_* 
    bool has_render_states{false};
    std::map<u32, u32> render_states;
    bool has_textures{false};
    std::array<u32, D3D9_MAX_SAMPLERS> textures{};
    std::array<bool, D3D9_MAX_SAMPLERS> texture_included{};
    std::array<u32, D3D9_VERTEX_TEXTURE_SAMPLER_COUNT> vertex_textures{};
    std::array<bool, D3D9_VERTEX_TEXTURE_SAMPLER_COUNT>
        vertex_texture_included{};
    bool has_stage_states{false};
    std::array<std::map<u32, u32>, D3D9_MAX_TEXTURE_STAGES> stage_states;
    bool has_sampler_states{false};
    std::array<std::map<u32, u32>, D3D9_MAX_SAMPLERS> sampler_states;
    std::array<std::map<u32, u32>, D3D9_VERTEX_TEXTURE_SAMPLER_COUNT>
        vertex_sampler_states;
    bool has_transforms{false};
    std::map<u32, std::array<float, 16>> transforms;
    bool has_fvf{false};
    u32 fvf{0};
    bool has_vertex_declaration{false};
    u32 vertex_declaration{0};
    bool has_vertex_shader{false};
    u32 vertex_shader{0};
    bool has_pixel_shader{false};
    u32 pixel_shader{0};
    bool has_vs_const_f{false};
    std::array<float, D3D9_MAX_SHADER_CONST_F * 4> vs_const_f{};
    std::array<bool, D3D9_MAX_SHADER_CONST_F> vs_const_f_included{};
    bool has_ps_const_f{false};
    std::array<float, D3D9_MAX_SHADER_CONST_F * 4> ps_const_f{};
    std::array<bool, D3D9_MAX_SHADER_CONST_F> ps_const_f_included{};
    bool has_viewport{false};
    std::array<u32, 6> viewport{};
    bool has_scissor{false};
    std::array<i32, 4> scissor{};
};

/// One D3DVERTEXELEMENT9 (8 bytes) as stored for GetDeclaration.
struct D3D9Manager::VertexDeclarationState {
    struct RawElement {
        u16 stream{0};
        u16 offset{0};
        u8 type{0};
        u8 method{0};
        u8 usage{0};
        u8 usage_index{0};
    };
    /// Elements exactly as the guest supplied them, including the END marker.
    std::vector<RawElement> raw;
    /// Translated elements for the draw path (END excluded), all streams.
    std::vector<FvfElement> elements;
    /// Bytes covered per stream (max offset + type size); 0 if unused.
    std::array<u32, D3D9_MAX_STREAMS> stream_strides{};
};

struct D3D9Manager::QueryState {
    u32 type{0};          // D3DQUERYTYPE
    bool issued{false};   // Issue(END) seen since last GetData completion
};

/// Per-chain state for IDirect3DSwapChain9 from CreateAdditionalSwapChain.
/// The device's implicit chain keeps living on DeviceState; additional chains
/// own their own present parameters, backend chain, and back buffer.
struct D3D9Manager::SwapChainState {
    std::array<u8, D3DPRESENT_PARAMETERS_SIZE> present_parameters{};
    std::unique_ptr<graphics::BackendSwapChain> swap_chain;
    u32 back_buffer_surface{0};
    u32 depth_stencil_surface{0};

    [[nodiscard]] u32 present_field(usize offset) const noexcept {
        u32 value = 0;
        std::memcpy(&value, present_parameters.data() + offset, sizeof(value));
        return value;
    }
    void set_present_field(usize offset, u32 value) noexcept {
        std::memcpy(present_parameters.data() + offset, &value, sizeof(value));
    }
};

/// Immutable copy of the map-backed device tables a draw needs. T1 freezes
/// this on the first draw after a state change; later draws share the same
/// allocation instead of copying every `std::map` again.
struct D3d9FrozenDrawTables {
    std::map<u32, u32> render_states;
    std::map<u32, std::array<float, 16>> transforms;
    std::array<std::map<u32, u32>, D3D9_MAX_TEXTURE_STAGES> stage_states;
    std::array<std::map<u32, u32>, D3D9_MAX_SAMPLERS> sampler_states;
    std::array<std::map<u32, u32>, D3D9_VERTEX_TEXTURE_SAMPLER_COUNT>
        vertex_sampler_states;
    std::map<u32, std::array<u8, D3DLIGHT9_SIZE>> lights;
    std::map<u32, bool> light_enabled;
};

struct D3D9Manager::DeviceState {
    // -- creation identity ----------------------------------------------------
    u32 adapter{0};
    u32 device_type{D3DDEVTYPE_HAL};
    u32 focus_window{0};
    u32 behavior_flags{0};
    u32 parent_d3d9{0};

    // -- presentation ---------------------------------------------------------
    std::array<u8, D3DPRESENT_PARAMETERS_SIZE> present_parameters{};
    std::unique_ptr<graphics::BackendSwapChain> swap_chain;
    u32 implicit_swap_chain{0};
    /// Additional chains from CreateAdditionalSwapChain, in creation order.
    /// Not enumerated by GetSwapChain / GetNumberOfSwapChains (those cover
    /// CreateDevice implicits only). Access via the COM pointer returned from
    /// CreateAdditionalSwapChain (e.g. IDirect3DSwapChain9::GetBackBuffer).
    std::vector<u32> additional_swap_chains;
    u32 back_buffer_surface{0};
    u32 depth_stencil_surface{0};
    u32 render_target_surface{0};
    std::array<u32, 4> render_target_surfaces{};
    bool lost{false};
    bool in_scene{false};

    /// Implicit-swap-chain D3DGAMMARAMP for Get/SetGammaRamp round-trips.
    /// Non-identity ramps are applied as a present LUT (no host DAC).
    std::array<u16, D3DGAMMARAMP_CHANNEL_ENTRIES * 3> gamma_ramp{};
    /// Last Flags argument from SetGammaRamp (D3DSGR_*); not applied on host.
    u32 gamma_ramp_flags{0};

    // -- hardware cursor (IDirect3DDevice9::SetCursor*) -----------------------
    /// Hotspot relative to the upper-left of the cursor bitmap.
    u32 cursor_hotspot_x{0};
    u32 cursor_hotspot_y{0};
    /// Guest IDirect3DSurface9 held by the device (AddRef'd). Format must be
    /// D3DFMT_A8R8G8B8 per Learn; contents are guest-owned until Reset/destroy.
    u32 cursor_surface{0};
    /// Last SetCursorPosition in virtual-desktop / screen coordinates.
    i32 cursor_x{0};
    i32 cursor_y{0};
    /// Guest-visible ShowCursor state. Starts hidden until ShowCursor(TRUE).
    bool cursor_visible{false};

    // -- viewport / scissor ---------------------------------------------------
    std::array<u32, 6> viewport{}; // X, Y, Width, Height, MinZ, MaxZ bits
    std::array<i32, 4> scissor{};

    // -- state stores ---------------------------------------------------------
    std::map<u32, u32> render_states;
    std::map<u32, std::array<float, 16>> transforms;
    std::array<std::map<u32, u32>, D3D9_MAX_TEXTURE_STAGES> stage_states;
    std::array<std::map<u32, u32>, D3D9_MAX_SAMPLERS> sampler_states;
    std::array<std::map<u32, u32>, D3D9_VERTEX_TEXTURE_SAMPLER_COUNT>
        vertex_sampler_states;
    std::array<u32, D3D9_MAX_SAMPLERS> textures{};
    /// SetTexture/GetTexture state for D3DVERTEXTEXTURESAMPLER0..3. Kept
    /// separate from pixel/fixed-function stages because the numeric ranges
    /// and COM references are independent.
    std::array<u32, D3D9_VERTEX_TEXTURE_SAMPLER_COUNT> vertex_textures{};
    std::array<std::array<float, 4>, D3D9_MAX_CLIP_PLANES> clip_planes{};
    /// Learn D3DCLIPSTATUS9 defaults: ClipUnion=0, ClipIntersection=0xFFFFFFFF.
    u32 clip_status_union{0};
    u32 clip_status_intersection{0xFFFFFFFFu};
    std::array<u8, D3DMATERIAL9_SIZE> material{};
    std::map<u32, std::array<u8, D3DLIGHT9_SIZE>> lights;
    std::map<u32, bool> light_enabled;

    struct StreamSource {
        u32 buffer{0};
        u32 offset{0};
        u32 stride{0};
        u32 frequency{1};
    };
    std::array<StreamSource, D3D9_MAX_STREAMS> streams{};
    u32 indices{0};

    /// When set, DrawPrimitive/DrawIndexedPrimitive read these parameters
    /// instead of the marshaler args (Draw*PrimitiveUP transient bind path).
    struct UpDrawOverride {
        u32 primitive_type{0};
        u32 first_vertex{0};
        u32 index_start{0};
        u32 primitive_count{0};
        i32 base_vertex{0};
    };
    std::optional<UpDrawOverride> up_override;
    u32 fvf{0};
    /// Bound IDirect3DVertexDeclaration9 object, or 0. Mutually exclusive
    /// with a non-zero FVF on Windows; SetFVF / SetVertexDeclaration clear
    /// the other.
    u32 vertex_declaration{0};

    std::array<float, D3D9_MAX_SHADER_CONST_F * 4> vs_const_f{};
    std::array<float, D3D9_MAX_SHADER_CONST_F * 4> ps_const_f{};
    std::array<i32, D3D9_MAX_SHADER_CONST_I * 4> vs_const_i{};
    std::array<i32, D3D9_MAX_SHADER_CONST_I * 4> ps_const_i{};
    std::array<u32, D3D9_MAX_SHADER_CONST_B> vs_const_b{};
    std::array<u32, D3D9_MAX_SHADER_CONST_B> ps_const_b{};

    /// Guest shader objects are not created yet, but the device still has to
    /// remember that one was bound so the fixed-function draw path can refuse
    /// rather than render with the wrong pipeline.
    u32 vertex_shader{0};
    u32 pixel_shader{0};

    /// Pipelines built from the fixed-function state, keyed by the hash of
    /// everything that goes into one. Owned by the device: they reference
    /// shaders generated for its state.
    std::unordered_map<u64, std::unique_ptr<graphics::BackendPipeline>>
        pipelines;

    // Draw submission is synchronous per device. Retain scratch capacity so
    // dense scenes do not allocate layout and uniform vectors for every draw.
    std::vector<FvfElement> draw_elements;
    std::vector<graphics::BackendVertexAttribute> draw_attributes;
    std::vector<u8> draw_vertex_uniforms;
    std::vector<u8> draw_fragment_uniforms;

    /// Bumped when a setter changes PSO inputs (render states, FF lighting,
    /// stream stride/freq, render target). Shader/FVF/decl handles are
    /// compared directly and do not need an epoch.
    u64 draw_pipeline_epoch{1};
    bool vs_const_dirty{true};
    bool ps_const_dirty{true};
    bool ff_uniform_dirty{true};

    struct DrawPipelineReuse {
        bool valid{false};
        u64 epoch{0};
        u32 vertex_shader{0};
        u32 pixel_shader{0};
        u32 fvf{0};
        u32 vertex_declaration{0};
        u32 render_target_surface{0};
        u32 depth_stencil_surface{0};
        std::array<u32, D3D9_MAX_STREAMS> stream_buffers{};
        std::array<u32, D3D9_MAX_STREAMS> stream_strides{};
        std::array<u32, D3D9_MAX_STREAMS> stream_freqs{};
        u64 pipeline_key{0};
        graphics::BackendPipeline* pipeline{nullptr};
        bool programmable_vs{false};
        bool programmable_ps{false};
        bool programmable_pair{false};
        bool need_ff{false};
        bool has_depth{false};
        bool to_back_buffer{false};
        bool ps_alpha_test{false};
        bool ps_vertex_fog{false};
        u32 ps_alpha_func{0};
        u32 ps_alpha_ref{0};
        u32 color_width{0};
        u32 color_height{0};
        u32 color_format{0};
        unsigned instance_count{1};
        std::array<bool, D3D9_MAX_STREAMS> stream_used{};
        std::array<u32, D3D9_MAX_STREAMS> stream_layout_strides{};
        graphics::FFPipelineState ff{};
    } draw_reuse;

    std::shared_ptr<const D3d9FrozenDrawTables> frozen_draw_tables;

    void invalidate_draw_pipeline_reuse() noexcept {
        ++draw_pipeline_epoch;
        draw_reuse.valid = false;
        frozen_draw_tables.reset();
    }

    [[nodiscard]] std::shared_ptr<const D3d9FrozenDrawTables>
    freeze_draw_tables() {
        if (frozen_draw_tables) {
            return frozen_draw_tables;
        }
        auto tables = std::make_shared<D3d9FrozenDrawTables>();
        tables->render_states = render_states;
        tables->transforms = transforms;
        tables->stage_states = stage_states;
        tables->sampler_states = sampler_states;
        tables->vertex_sampler_states = vertex_sampler_states;
        tables->lights = lights;
        tables->light_enabled = light_enabled;
        frozen_draw_tables = tables;
        return tables;
    }

    /// The scene's recording scope. Draws share one pass so that a frame is
    /// one command buffer; EndScene and Present submit it.
    std::unique_ptr<graphics::BackendRenderPass> render_pass;
    /// Non-null when `render_pass` targets a texture render target rather than
    /// the swap-chain back buffer. Cleared by `flush_render_pass`.
    graphics::BackendTexture2D* render_pass_color{nullptr};
    bool depth_pending_clear{false};
    /// True when `depth_pending_clear` applies to the swap-chain attachment
    /// rather than the currently bound offscreen depth surface. A later
    /// texture-RT pass must not consume a back-buffer Clear Z.
    bool depth_pending_clear_back_buffer{false};
    float depth_clear_value{1.0f};
    u64 diagnostic_frame{0};
    u32 diagnostic_draw{0};

    /// Metal requires colour and depth attachments to share dimensions.
    /// Windows allows a depth-stencil surface larger than the colour RT; when
    /// that happens we draw with a scratch depth texture sized to the RT.
    /// Keyed by `(width << 32) | height`.
    std::unordered_map<u64, std::unique_ptr<graphics::BackendTexture2D>>
        rt_depth_scratch;

    u32 software_vertex_processing{0};
    u32 npatch_mode{0}; // float bits

    /// Learn: Texture Palettes — up to 0xFFFF palettes of 256 PALETTEENTRY
    /// (peRed/peGreen/peBlue/peFlags; peFlags is alpha). Defaults 0xFFFFFFFF.
    /// Sparse: only palettes touched by SetPaletteEntries are allocated.
    static constexpr u32 kMaxPaletteNumber = 0xFFFFu;
    static constexpr usize kPaletteEntryCount = 256;
    static constexpr usize kPaletteBytes = kPaletteEntryCount * 4;
    using TexturePalette = std::array<u8, kPaletteBytes>;
    std::unordered_map<u32, TexturePalette> texture_palettes;
    u32 current_texture_palette{0};
    /// Bumped by SetPaletteEntries / SetCurrentTexturePalette so bound
    /// palettized textures re-expand before the next sample.
    u64 texture_palette_serial{1};
    u32 available_texture_mem{0};

    /// Set between BeginStateBlock and EndStateBlock. While true, state
    /// setters also record into `recording_block`.
    bool recording_state_block{false};
    StateBlockState recording_block{};

    [[nodiscard]] u32 back_buffer_width() const noexcept {
        u32 value = 0;
        std::memcpy(&value,
                    present_parameters.data() + PP_BACK_BUFFER_WIDTH,
                    sizeof(value));
        return value;
    }
    [[nodiscard]] u32 back_buffer_height() const noexcept {
        u32 value = 0;
        std::memcpy(&value,
                    present_parameters.data() + PP_BACK_BUFFER_HEIGHT,
                    sizeof(value));
        return value;
    }
    [[nodiscard]] u32 present_field(usize offset) const noexcept {
        u32 value = 0;
        std::memcpy(&value, present_parameters.data() + offset,
                    sizeof(value));
        return value;
    }
    void set_present_field(usize offset, u32 value) noexcept {
        std::memcpy(present_parameters.data() + offset, &value,
                    sizeof(value));
    }
};

} // namespace vitreo::winapi
