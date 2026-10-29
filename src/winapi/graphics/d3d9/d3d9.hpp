// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d9.hpp
/// @brief D3D9 (d3d9.dll) guest COM frontend and thunk definitions.
///
/// The frontend owns only Windows-visible behavior: guest COM object memory,
/// vtables, interface identity, reference counting, argument validation, and
/// the D3D9 state that a guest can read back. All GPU work goes through the
/// shared `graphics::GraphicsBackendDevice` contract that the DirectX, Vulkan,
/// and OpenGL frontends share; there is no D3D9-private backend.

#include <vitreo/common.hpp>
#include "graphics/contracts/backend_device.hpp"
#include "winapi/core/thunk_registry.hpp"
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::graphics {
struct FFPipelineState;
}

namespace vitreo::winapi {

class Marshaler;
struct D3d9DrawSnapshot;
struct D3d9ClearSnapshot;
struct D3d9ColorFillSnapshot;
class D3d9SubmitQueue;

// =============================================================================
// D3D9 Constants
// =============================================================================

constexpr u32 D3D_SDK_VERSION = 32;

constexpr u32 D3D_OK = 0;
constexpr u32 D3DERR_INVALIDCALL = 0x8876086C;
constexpr u32 D3DERR_NOTAVAILABLE = 0x8876086A;
constexpr u32 D3DERR_NOTFOUND_D3D = 0x88760866;
constexpr u32 D3DERR_MOREDATA = 0x88760867;
constexpr u32 D3DERR_DEVICELOST = 0x88760868;
constexpr u32 D3DERR_DEVICENOTRESET = 0x88760869;
constexpr u32 D3DERR_OUTOFVIDEOMEMORY = 0x8876017C;
constexpr u32 E_NOINTERFACE_D3D = 0x80004002;
constexpr u32 E_POINTER_D3D = 0x80004003;
constexpr u32 E_NOTIMPL_D3D = 0x80004001;
constexpr u32 E_INVALIDARG_D3D = 0x80070057;
constexpr u32 E_OUTOFMEMORY_D3D = 0x8007000E;

/// SetPrivateData flag: buffer at pData holds an IUnknown* (sizeof pointer).
/// Vitreo AddRefs known D3D9 ComObjects; unknown guest pointers → INVALIDCALL.
constexpr u32 D3DSPD_IUNKNOWN = 1;

constexpr u32 D3DDEVTYPE_HAL = 1;
constexpr u32 D3DFMT_UNKNOWN = 0;
constexpr u32 D3DFMT_A8R8G8B8 = 21;
constexpr u32 D3DFMT_X8R8G8B8 = 22;
constexpr u32 D3DFMT_A8P8 = 40; // 8-bit palette index + 8-bit alpha
constexpr u32 D3DFMT_P8 = 41;   // 8-bit palette index
constexpr u32 D3DFMT_INDEX16 = 101;
constexpr u32 D3DFMT_INDEX32 = 102;
constexpr u32 D3DMULTISAMPLE_NONE = 0;
constexpr u32 D3DMULTISAMPLE_NONMASKABLE = 1;
constexpr u32 D3DPRESENTFLAG_LOCKABLE_BACKBUFFER = 0x1;
constexpr u32 D3DSWAPEFFECT_DISCARD = 1;
constexpr u32 D3DPOOL_DEFAULT = 0;
constexpr u32 D3DPOOL_MANAGED = 1;
constexpr u32 D3DPOOL_SYSTEMMEM = 2;
constexpr u32 D3DPOOL_SCRATCH = 3;

constexpr u32 D3DCLEAR_TARGET = 0x1;
constexpr u32 D3DCLEAR_ZBUFFER = 0x2;
constexpr u32 D3DCLEAR_STENCIL = 0x4;

constexpr u32 D3DBACKBUFFER_TYPE_MONO = 0;

/// D3DRESOURCETYPE
constexpr u32 D3DRTYPE_SURFACE = 1;
constexpr u32 D3DRTYPE_VOLUME = 2;
constexpr u32 D3DRTYPE_TEXTURE = 3;
constexpr u32 D3DRTYPE_VOLUMETEXTURE = 4;
constexpr u32 D3DRTYPE_CUBETEXTURE = 5;
constexpr u32 D3DRTYPE_VERTEXBUFFER = 6;
constexpr u32 D3DRTYPE_INDEXBUFFER = 7;

constexpr u32 D3DCUBE_FACE_COUNT = 6;

/// Lock flags whose Windows-visible effect the frontend must honor.
constexpr u32 D3DLOCK_READONLY = 0x10;
constexpr u32 D3DLOCK_DISCARD = 0x2000;
constexpr u32 D3DLOCK_NOOVERWRITE = 0x1000;

/// IDirect3DDevice9::ProcessVertices — skip copying elements the vertex
/// operation does not touch (Microsoft Learn).
constexpr u32 D3DPV_DONOTCOPYDATA = 0x00000001;

/// IDirect3DDevice9::SetStreamSourceFreq — indexed / instance stream flags
/// (d3d9types.h). Low 30 bits carry the instance count or step rate.
constexpr u32 D3DSTREAMSOURCE_INDEXEDDATA = 1u << 30;
constexpr u32 D3DSTREAMSOURCE_INSTANCEDATA = 2u << 30;

// -----------------------------------------------------------------------------
// D3DCAPS9 field offsets
// -----------------------------------------------------------------------------
//
// D3DCAPS9 is 76 four-byte fields (304 bytes). Its tail is easy to miscount
// because two of the last fields are embedded structures rather than DWORDs:
// StretchRectFilterCaps at 244, D3DVSHADERCAPS2_0 at 248 (16 bytes), and
// D3DPSHADERCAPS2_0 at 264 (20 bytes). Writing an adapter-group field past
// offset 244 therefore lands inside the shader caps and leaves the intended
// field zero, so these offsets are named rather than written inline.

constexpr usize D3DCAPS9_SIZE = 304;
constexpr usize D3DCAPS9_OFFSET_DEVICE_TYPE = 0;
constexpr usize D3DCAPS9_OFFSET_ADAPTER_ORDINAL = 4;
constexpr usize D3DCAPS9_OFFSET_CAPS = 8;
constexpr usize D3DCAPS9_OFFSET_CAPS2 = 12;
constexpr usize D3DCAPS9_OFFSET_CAPS3 = 16;
constexpr usize D3DCAPS9_OFFSET_PRESENTATION_INTERVALS = 20;
constexpr usize D3DCAPS9_OFFSET_CURSOR_CAPS = 24;
constexpr usize D3DCAPS9_OFFSET_DEV_CAPS = 28;
constexpr usize D3DCAPS9_OFFSET_PRIMITIVE_MISC_CAPS = 32;
constexpr usize D3DCAPS9_OFFSET_RASTER_CAPS = 36;
constexpr usize D3DCAPS9_OFFSET_ZCMP_CAPS = 40;
constexpr usize D3DCAPS9_OFFSET_SRC_BLEND_CAPS = 44;
constexpr usize D3DCAPS9_OFFSET_DEST_BLEND_CAPS = 48;
constexpr usize D3DCAPS9_OFFSET_ALPHA_CMP_CAPS = 52;
constexpr usize D3DCAPS9_OFFSET_SHADE_CAPS = 56;
constexpr usize D3DCAPS9_OFFSET_TEXTURE_CAPS = 60;
constexpr usize D3DCAPS9_OFFSET_TEXTURE_FILTER_CAPS = 64;
constexpr usize D3DCAPS9_OFFSET_CUBE_TEXTURE_FILTER_CAPS = 68;
constexpr usize D3DCAPS9_OFFSET_VOLUME_TEXTURE_FILTER_CAPS = 72;
constexpr usize D3DCAPS9_OFFSET_TEXTURE_ADDRESS_CAPS = 76;
constexpr usize D3DCAPS9_OFFSET_VOLUME_TEXTURE_ADDRESS_CAPS = 80;
constexpr usize D3DCAPS9_OFFSET_LINE_CAPS = 84;
constexpr usize D3DCAPS9_OFFSET_MAX_TEXTURE_WIDTH = 88;
constexpr usize D3DCAPS9_OFFSET_MAX_TEXTURE_HEIGHT = 92;
constexpr usize D3DCAPS9_OFFSET_MAX_VOLUME_EXTENT = 96;
constexpr usize D3DCAPS9_OFFSET_MAX_TEXTURE_REPEAT = 100;
constexpr usize D3DCAPS9_OFFSET_MAX_TEXTURE_ASPECT_RATIO = 104;
constexpr usize D3DCAPS9_OFFSET_MAX_ANISOTROPY = 108;
constexpr usize D3DCAPS9_OFFSET_MAX_VERTEX_W = 112;
constexpr usize D3DCAPS9_OFFSET_GUARD_BAND_LEFT = 116;
constexpr usize D3DCAPS9_OFFSET_GUARD_BAND_TOP = 120;
constexpr usize D3DCAPS9_OFFSET_GUARD_BAND_RIGHT = 124;
constexpr usize D3DCAPS9_OFFSET_GUARD_BAND_BOTTOM = 128;
constexpr usize D3DCAPS9_OFFSET_EXTENTS_ADJUST = 132;
constexpr usize D3DCAPS9_OFFSET_STENCIL_CAPS = 136;
constexpr usize D3DCAPS9_OFFSET_FVF_CAPS = 140;
constexpr usize D3DCAPS9_OFFSET_TEXTURE_OP_CAPS = 144;
constexpr usize D3DCAPS9_OFFSET_MAX_TEXTURE_BLEND_STAGES = 148;
constexpr usize D3DCAPS9_OFFSET_MAX_SIMULTANEOUS_TEXTURES = 152;
constexpr usize D3DCAPS9_OFFSET_VERTEX_PROCESSING_CAPS = 156;
constexpr usize D3DCAPS9_OFFSET_MAX_ACTIVE_LIGHTS = 160;
constexpr usize D3DCAPS9_OFFSET_MAX_USER_CLIP_PLANES = 164;
constexpr usize D3DCAPS9_OFFSET_MAX_VERTEX_BLEND_MATRICES = 168;
constexpr usize D3DCAPS9_OFFSET_MAX_VERTEX_BLEND_MATRIX_INDEX = 172;
constexpr usize D3DCAPS9_OFFSET_MAX_POINT_SIZE = 176;
constexpr usize D3DCAPS9_OFFSET_MAX_PRIMITIVE_COUNT = 180;
constexpr usize D3DCAPS9_OFFSET_MAX_VERTEX_INDEX = 184;
constexpr usize D3DCAPS9_OFFSET_MAX_STREAMS = 188;
constexpr usize D3DCAPS9_OFFSET_MAX_STREAM_STRIDE = 192;
constexpr usize D3DCAPS9_OFFSET_VERTEX_SHADER_VERSION = 196;
constexpr usize D3DCAPS9_OFFSET_MAX_VERTEX_SHADER_CONST = 200;
constexpr usize D3DCAPS9_OFFSET_PIXEL_SHADER_VERSION = 204;
constexpr usize D3DCAPS9_OFFSET_PIXEL_SHADER_1X_MAX_VALUE = 208;
constexpr usize D3DCAPS9_OFFSET_DEV_CAPS2 = 212;
constexpr usize D3DCAPS9_OFFSET_MAX_NPATCH_TESSELLATION_LEVEL = 216;
constexpr usize D3DCAPS9_OFFSET_MASTER_ADAPTER_ORDINAL = 224;
constexpr usize D3DCAPS9_OFFSET_ADAPTER_ORDINAL_IN_GROUP = 228;
constexpr usize D3DCAPS9_OFFSET_NUMBER_OF_ADAPTERS_IN_GROUP = 232;
constexpr usize D3DCAPS9_OFFSET_DECL_TYPES = 236;
constexpr usize D3DCAPS9_OFFSET_NUM_SIMULTANEOUS_RTS = 240;
constexpr usize D3DCAPS9_OFFSET_STRETCH_RECT_FILTER_CAPS = 244;
constexpr usize D3DCAPS9_OFFSET_VS20_CAPS = 248;
constexpr usize D3DCAPS9_OFFSET_PS20_CAPS = 264;
constexpr usize D3DCAPS9_OFFSET_VERTEX_TEXTURE_FILTER_CAPS = 284;
constexpr usize D3DCAPS9_OFFSET_MAX_VSHADER_INSTRUCTIONS_EXECUTED = 288;
constexpr usize D3DCAPS9_OFFSET_MAX_PSHADER_INSTRUCTIONS_EXECUTED = 292;
constexpr usize D3DCAPS9_OFFSET_MAX_VERTEX_SHADER_30_INSTRUCTION_SLOTS = 296;
constexpr usize D3DCAPS9_OFFSET_MAX_PIXEL_SHADER_30_INSTRUCTION_SLOTS = 300;

/// Fill a zeroed `D3DCAPS9` buffer with the HAL capabilities Vitreo reports.
/// @p device_type and @p adapter become `DeviceType` / `AdapterOrdinal`.
void fill_d3d9_device_caps(std::array<u8, D3DCAPS9_SIZE>& caps,
                           u32 device_type, u32 adapter) noexcept;

/// Raster sample count for a D3DMULTISAMPLE_TYPE + quality pair. NONE is 1.
/// NONMASKABLE quality indexes the supported 2/4/8 modes in that order.
[[nodiscard]] unsigned d3d9_raster_sample_count(
    u32 multi_sample_type, u32 multi_sample_quality,
    const graphics::GraphicsBackendDevice* backend = nullptr) noexcept;

/// Whether CheckDeviceMultiSampleType / CreateDevice may accept @p type.
[[nodiscard]] bool d3d9_multisample_supported(
    const graphics::GraphicsBackendDevice* backend,
    u32 multi_sample_type) noexcept;

/// Quality levels written through CheckDeviceMultiSampleType's last argument.
[[nodiscard]] u32 d3d9_multisample_quality_levels(
    const graphics::GraphicsBackendDevice* backend,
    u32 multi_sample_type) noexcept;

/// D3DVIEWPORT9: X, Y, Width, Height, MinZ, MaxZ.
constexpr usize D3DVIEWPORT9_SIZE = 24;

// =============================================================================
// Interface / method description
// =============================================================================

/// Every COM interface the D3D9 frontend maps into guest memory.
enum class D3D9InterfaceId : u32 {
    Direct3D9 = 0,
    Device,
    SwapChain,
    Surface,
    Texture,
    VertexBuffer,
    IndexBuffer,
    VertexShader,
    PixelShader,
    StateBlock,
    VertexDeclaration,
    CubeTexture,
    Query,
    VolumeTexture,
    Volume,
    Count,
};

constexpr usize D3D9_INTERFACE_COUNT =
    static_cast<usize>(D3D9InterfaceId::Count);

struct D3D9MethodDesc {
    const char* name;
    /// x86 stdcall callee-pop byte count, including the implicit `this`.
    u16 stdcall_bytes;
};

/// Methods of @p iface in vtable order.
[[nodiscard]] const D3D9MethodDesc* d3d9_interface_methods(
    D3D9InterfaceId iface, usize& count) noexcept;

/// Number of vtable slots in @p iface.
[[nodiscard]] usize d3d9_interface_method_count(D3D9InterfaceId iface) noexcept;

/// Short interface name used in export names and logs ("Device", "Surface").
[[nodiscard]] const char* d3d9_interface_name(D3D9InterfaceId iface) noexcept;

/// Which pipeline-state table a `record_d3d9_state_use()` sample came from.
enum class D3D9StateKind : u32 {
    RenderState = 0,
    TextureStageState = 1,
    SamplerState = 2,
};

/// VITREO_D3D9_STATE_COVERAGE=1: record every distinct (kind, index, value)
/// a guest sets, and dump the set at exit. The vtable-slot coverage dump says
/// *that* a guest called SetRenderState; this says *which* states and values,
/// which is what an oracle scene has to reproduce to be meaningful.
void record_d3d9_state_use(D3D9StateKind kind, u32 index, u32 value) noexcept;

/// VITREO_D3D9_STATE_COVERAGE=1: record one guest vertex declaration, as a
/// printable signature, and dump the distinct set alongside the state tables.
/// Vertex formats are the one part of the draw path that no render-state dump
/// can reach, and a mistranslated element type displaces geometry without
/// touching any state a guest sets.
void record_d3d9_vertex_declaration(const std::string& signature) noexcept;

/// True when the state-coverage probe is on. Callers use it to skip building
/// a signature string on a path that runs thousands of times per load.
[[nodiscard]] bool d3d9_state_coverage_enabled_for_declarations() noexcept;

/// Synthetic export name backing one vtable slot, e.g. "_D3D9_Device_Clear".
[[nodiscard]] std::string d3d9_method_export_name(D3D9InterfaceId iface,
                                                  usize slot);

// Kept for the IDirect3D9 factory interface, whose slots are addressed
// directly by the existing registry wiring and tests.
constexpr usize D3D9_METHOD_COUNT = 17;

// =============================================================================
// D3D9Manager
// =============================================================================

/// Implements the guest-visible D3D9 COM object graph over Vitreo's shared
/// graphics backend.
///
/// Every method either performs the real Windows-visible effect or fails
/// honestly. Two failure codes are used deliberately and mean different
/// things:
///
/// * `D3DERR_INVALIDCALL` — the guest's arguments or device state are invalid,
///   exactly as Windows would report.
/// * `E_NOTIMPL_D3D` — the arguments are valid but Vitreo has no
///   implementation of that contract yet. Each such return logs the specific
///   missing contract. No method returns success without the side effect.
class D3D9Manager {
public:
    using MethodThunks = std::array<GuestAddr, D3D9_METHOD_COUNT>;
    using InterfaceThunks = std::vector<GuestAddr>;
    using DisplaySizeResolver = std::function<bool(u32&, u32&)>;
    /// Resolve a guest HWND to its client-area size (GetClientRect width/
    /// height). Used when windowed BackBufferWidth/Height are 0.
    using ClientSizeResolver = std::function<bool(u32 /*hwnd*/, u32& /*width*/,
                                                  u32& /*height*/)>;
    /// Resolve a guest top-level HWND to the native presentation view the
    /// shared backend can attach a swap chain to. Returns nullptr when the
    /// window has no presentable native view.
    using PresentationViewResolver = std::function<void*(u32)>;
    /// Optional AddRef for a guest IUnknown* that is not a Vitreo D3D9 object.
    /// Composition root wires this to ComCoreManager::guest_iunknown_addref.
    /// Return 0 to reject (Set/GetPrivateData → D3DERR_INVALIDCALL).
    using GuestIUnknownAddRefFn = std::function<u32(u32 /*punk*/)>;
    /// Optional Release for a guest IUnknown* that is not a Vitreo D3D9 object.
    using GuestIUnknownReleaseFn = std::function<void(u32 /*punk*/)>;
    /// Drive the host window into/out of borderless fullscreen covering the
    /// display (same contract DirectDraw exclusive mode uses).
    using FullscreenFn = std::function<void(u32 /*hwnd*/, bool /*enable*/)>;
    /// Publish the exclusive-mode logical resolution into user32/gdi metrics.
    using DisplayModeFn =
        std::function<void(u32 /*hwnd*/, u32 /*width*/, u32 /*height*/,
                           u32 /*bits*/)>;
    /// Show/hide the host OS cursor (CocoaBridge::set_cursor_visible).
    using CursorVisibleFn = std::function<void(bool /*visible*/)>;
    /// Warp the host cursor in guest screen coordinates (SetCursorPos space).
    using CursorPositionFn = std::function<void(i32 /*x*/, i32 /*y*/)>;
    /// Apply a SetCursorProperties bitmap on the host. Pixels are tightly
    /// packed top-down RGBA8. Return false to reject the call (D3DERR_INVALIDCALL).
    using CursorImageFn = std::function<bool(
        u32 /*width*/, u32 /*height*/, u32 /*hotspot_x*/, u32 /*hotspot_y*/,
        const u8* /*rgba*/, usize /*byte_count*/)>;

    D3D9Manager();
    ~D3D9Manager();
    D3D9Manager(const D3D9Manager&) = delete;
    D3D9Manager& operator=(const D3D9Manager&) = delete;

    void set_memory(GuestMemory* mem, bool is_64bit) {
        m_memory = mem;
        m_is_64bit = is_64bit;
    }
    void set_backend(graphics::GraphicsBackendDevice* backend) {
        m_backend = backend;
    }
    void set_display_size_resolver(DisplaySizeResolver resolver) {
        m_display_size_resolver = std::move(resolver);
    }
    void set_presentation_view_resolver(PresentationViewResolver resolver) {
        m_presentation_view_resolver = std::move(resolver);
    }
    /// Wire guest IUnknown AddRef/Release for D3DSPD_IUNKNOWN private data
    /// that is not a Vitreo-managed D3D9 ComObject. Unset → INVALIDCALL for
    /// unknown pointers (Vitreo objects still use the local fast path).
    void set_guest_iunknown_refcount(GuestIUnknownAddRefFn addref,
                                     GuestIUnknownReleaseFn release) {
        m_guest_iunknown_addref = std::move(addref);
        m_guest_iunknown_release = std::move(release);
    }
    void set_client_size_resolver(ClientSizeResolver resolver) {
        m_client_size_resolver = std::move(resolver);
    }
    void set_fullscreen_callback(FullscreenFn cb) {
        m_fullscreen_cb = std::move(cb);
    }
    void set_display_mode_callback(DisplayModeFn cb) {
        m_display_mode_cb = std::move(cb);
    }
    void set_cursor_visible_callback(CursorVisibleFn cb) {
        m_cursor_visible_cb = std::move(cb);
    }
    void set_cursor_position_callback(CursorPositionFn cb) {
        m_cursor_position_cb = std::move(cb);
    }
    void set_cursor_image_callback(CursorImageFn cb) {
        m_cursor_image_cb = std::move(cb);
    }

    /// Publish the guest-callable thunk address of every slot of @p iface.
    /// A vtable is only mapped once all of its slots are known.
    void set_interface_thunks(D3D9InterfaceId iface, InterfaceThunks thunks);

    /// Convenience overload for the IDirect3D9 factory interface.
    void set_method_thunks(MethodThunks thunks);

    [[nodiscard]] u32 direct3d_create9(u32 sdk_version);

    /// Dispatch one vtable slot. `m.arg_ptr(0)` is the guest `this`.
    void impl_method(Marshaler& m, D3D9InterfaceId iface, usize slot);

    /// Wait until queued D3D9 draws have been built into the open pass.
    void d3d9_cs_flush();
    /// Same wait; `clear_pipelines` drops the worker PSO cache (Reset/dtor).
    void d3d9_cs_drain(bool clear_pipelines = false);

    // Retained name for the IDirect3D9 factory interface.
    void impl_D3D9Method(Marshaler& m, usize slot) {
        impl_method(m, D3D9InterfaceId::Direct3D9, slot);
    }

    // -------------------------------------------------------------------------
    // Guest COM object model
    // -------------------------------------------------------------------------
    //
    // The per-object state types are defined in the internal
    // `d3d9_state.hpp`, which the dispatch translation units share.

    struct SurfaceState;
    struct TextureState;
    struct VolumeState;
    struct BufferState;
    struct ShaderState;
    struct DeviceState;
    struct SwapChainState;
    struct StateBlockState;
    struct VertexDeclarationState;
    struct QueryState;

    /// One element of a vertex as an FVF or declaration describes it.
    struct FvfElement {
        u32 usage{0};
        u32 usage_index{0};
        u32 offset{0};
        graphics::BackendVertexFormat format{
            graphics::BackendVertexFormat::Float4};
        /// D3DVERTEXELEMENT9.Stream; FVF layouts always use stream 0.
        u32 stream{0};
    };

private:
    struct ComObject {
        D3D9InterfaceId iface{D3D9InterfaceId::Direct3D9};
        u32 ref_count{1};
        u32 vtable{0};
        /// Owning device object address (0 for IDirect3D9 itself).
        u32 device{0};
        /// Application private data keyed by GUID.
        /// D3DSPD_IUNKNOWN entries store a guest IUnknown* and own one ref
        /// until replace, FreePrivateData, or object destroy.
        struct PrivateDataEntry {
            u32 flags{0};
            std::vector<u8> bytes;
        };
        std::map<std::array<u8, 16>, PrivateDataEntry> private_data;
        std::unique_ptr<SurfaceState> surface;
        std::unique_ptr<TextureState> texture;
        std::unique_ptr<VolumeState> volume;
        std::unique_ptr<BufferState> buffer;
        std::unique_ptr<ShaderState> shader;
        std::unique_ptr<DeviceState> device_state;
        /// Set for chains from CreateAdditionalSwapChain; implicit chain uses
        /// DeviceState presentation fields instead.
        std::unique_ptr<SwapChainState> swap_chain_state;
        std::unique_ptr<StateBlockState> state_block;
        std::unique_ptr<VertexDeclarationState> vertex_declaration;
        std::unique_ptr<QueryState> query;
    };

    [[nodiscard]] bool ensure_storage();
    [[nodiscard]] bool map_vtable(D3D9InterfaceId iface);
    [[nodiscard]] u32 alloc_com_object(D3D9InterfaceId iface, u32 device);
    friend class D3d9SubmitQueue;

    [[nodiscard]] ComObject* find(u32 object, D3D9InterfaceId iface);
    [[nodiscard]] ComObject* find_any(u32 object);
    [[nodiscard]] u32 add_ref(u32 object);
    [[nodiscard]] u32 release(u32 object);
    void destroy_object(u32 object);

    // -------------------------------------------------------------------------
    // Per-interface dispatch (one translation unit each)
    // -------------------------------------------------------------------------

    void dispatch_direct3d9(Marshaler& m, usize slot, u32 self);
    void dispatch_device(Marshaler& m, usize slot, u32 self);

    /// IDirect3DDevice9 slot groups `dispatch_device` routes to, one
    /// translation unit each (d3d9_device_*.cpp, d3d9_state_block.cpp,
    /// d3d9_vertex_input.cpp, d3d9_query.cpp).
    void dispatch_device_display(Marshaler& m, usize slot, DeviceState& state);
    void dispatch_device_swap_chain(Marshaler& m, usize slot, u32 self,
                                    DeviceState& state);
    void dispatch_device_create(Marshaler& m, usize slot, u32 self);
    void dispatch_device_scene(Marshaler& m, usize slot, DeviceState& state);
    void dispatch_device_state(Marshaler& m, usize slot, DeviceState& state);
    void dispatch_device_state_blocks(Marshaler& m, usize slot, u32 self,
                                      DeviceState& state);
    void dispatch_device_texture_state(Marshaler& m, usize slot,
                                       DeviceState& state);
    void dispatch_device_vertex_input(Marshaler& m, usize slot, u32 self,
                                      DeviceState& state);
    void dispatch_device_shaders(Marshaler& m, usize slot, u32 self,
                                 DeviceState& state);
    void dispatch_device_query(Marshaler& m, usize slot, u32 self);

    /// IDirect3D9::CreateDevice.
    void create_device(Marshaler& m, u32 factory);

    /// Build the swap chain, back buffer, and depth stencil described by the
    /// present parameters already stored on @p device. Returns false when the
    /// shared backend could not produce a presentable surface.
    [[nodiscard]] bool build_presentation(u32 device);

    /// Reset every state store on @p device to its documented initial value.
    void reset_device_state(u32 device);

    [[nodiscard]] u32 create_surface_object(u32 device, u32 width, u32 height,
                                            u32 format, u32 usage, u32 pool,
                                            int kind, u32 container,
                                            u32 level, u32 face = 0,
                                            u32 multi_sample_type = 0);

    /// IDirect3DDevice9::CreateTexture.
    void create_texture(Marshaler& m, u32 device);
    /// IDirect3DDevice9::CreateVolumeTexture.
    void create_volume_texture(Marshaler& m, u32 device);
    /// IDirect3DDevice9::CreateCubeTexture.
    void create_cube_texture(Marshaler& m, u32 device);
    /// IDirect3DDevice9::CreateVertexBuffer / CreateIndexBuffer.
    void create_buffer(Marshaler& m, u32 device, bool index_buffer);

    /// One mip level of an IDirect3DVolumeTexture9 (IDirect3DVolume9).
    [[nodiscard]] u32 create_volume_object(u32 device, u32 width, u32 height,
                                           u32 depth, u32 format, u32 usage,
                                           u32 pool, u32 container,
                                           u32 level);

    /// IDirect3DDevice9::DrawPrimitive / DrawIndexedPrimitive.
    void draw_primitive(Marshaler& m, u32 self, bool indexed);
    /// IDirect3DDevice9::DrawPrimitiveUP / DrawIndexedPrimitiveUP.
    void draw_primitive_up(Marshaler& m, u32 self, bool indexed);
    /// IDirect3DDevice9::ProcessVertices — CPU FF or subset VS into a dest VB.
    void process_vertices(Marshaler& m, u32 self);
    /// Transient VB/IB from host bytes (refcount 1).
    [[nodiscard]] u32 create_host_buffer(u32 device, bool index_buffer,
                                         u32 length, u32 fvf_or_format,
                                         const void* data);
    /// Transient VB/IB for a user-pointer draw (refcount 1).
    [[nodiscard]] u32 create_up_buffer(u32 device, bool index_buffer,
                                       u32 length, u32 fvf_or_format,
                                       GuestAddr data, Marshaler& m);

    /// Submit the scene's render pass, if one is open.
    void flush_render_pass(DeviceState& state);

    /// Worker entry: pipeline / constant pack / BackendDrawCall. Must not
    /// find() COM objects, read live DeviceState, or wait on the Metal CS.
    void execute_recorded_draw(D3d9DrawSnapshot& snap);
    /// Worker entry: EndPass (record only) then GPU clear. No wait_encoded.
    void execute_recorded_clear(D3d9ClearSnapshot& snap);
    /// Worker entry: EndPass (record only) then GPU ColorFill.
    void execute_recorded_color_fill(D3d9ColorFillSnapshot& snap);
    /// Open the scene pass on T1 (may wait_encoded). Worker must not do this.
    [[nodiscard]] bool ensure_draw_render_pass(DeviceState& state,
                                              D3d9DrawSnapshot& snap);

    /// Resolve the shared-core colour target for a bound render-target
    /// surface. Returns null for the implicit back buffer (caller should use
    /// the swap-chain pass) or when the surface has no backend texture yet.
    [[nodiscard]] graphics::BackendTexture2D*
    resolve_render_target_color(u32 surface, u32 back_buffer_surface);

    /// Depth attachment for the current colour target. Exact-size DS backends
    /// are preferred; when Windows allows a larger DS than the RT, a scratch
    /// depth texture matching the RT size is used (Metal size constraint).
    [[nodiscard]] graphics::BackendTexture2D*
    resolve_depth_attachment(DeviceState& state, u32 color_width,
                             u32 color_height);

    /// Enumerate an FVF's elements in memory order. On failure, @p rejection
    /// names the part Vitreo cannot express.
    [[nodiscard]] static bool enumerate_fvf_elements(
        u32 fvf, std::vector<FvfElement>& elements, u32& stride,
        const char*& rejection);

    /// Vertex layout for the fixed-function pipeline, whose generated shader
    /// numbers attributes in FVF order.
    [[nodiscard]] static bool build_vertex_layout(
        u32 fvf, std::vector<graphics::BackendVertexAttribute>& attributes,
        u32& stride, const char*& rejection);

    /// Translate the device's D3D9 state into the shared fixed-function state
    /// the MSL generator consumes.
    [[nodiscard]] bool build_ff_state(const D3d9DrawSnapshot& snap,
                                      graphics::FFPipelineState& ff) const;

    /// Lay out the uniform buffers exactly as the generated MSL declares them.
    void pack_vertex_uniforms(const D3d9DrawSnapshot& snap,
                              const graphics::FFPipelineState& ff,
                              std::vector<u8>& out) const;
    void pack_fragment_uniforms(const D3d9DrawSnapshot& snap,
                                const graphics::FFPipelineState& ff,
                                std::vector<u8>& out) const;

    /// Shared implementation of the four float shader-constant accessors.
    /// Constants are guest-visible state: a setter without a matching getter
    /// would report values the guest never wrote.
    void set_shader_constant_f(Marshaler& m, std::array<float, 1024>& store);
    void get_shader_constant_f(Marshaler& m,
                               const std::array<float, 1024>& store);
    void set_shader_constant_i(Marshaler& m, std::array<i32, 64>& store);
    void get_shader_constant_i(Marshaler& m, const std::array<i32, 64>& store);
    void set_shader_constant_b(Marshaler& m, std::array<u32, 16>& store);
    void get_shader_constant_b(Marshaler& m, const std::array<u32, 16>& store);

    /// Surface transfer APIs (UpdateSurface / UpdateTexture / StretchRect /
    /// ColorFill / GetRenderTargetData / GetFrontBufferData). Prefer host GPU
    /// blit when both sides have backend textures; otherwise copy through
    /// guest lockable memory.
    void update_surface(Marshaler& m, u32 device);
    void update_texture(Marshaler& m, u32 device);
    void get_render_target_data(Marshaler& m, u32 device);
    void get_front_buffer_data(Marshaler& m, u32 device, u32 swap_chain_index,
                               u32 dest_surface);
    /// Shared Device / SwapChain GetFrontBufferData body. `chain` and
    /// `back_buffer_surface` identify the presentable source for this call.
    void get_front_buffer_data(Marshaler& m, DeviceState& state,
                               graphics::BackendSwapChain* chain,
                               u32 back_buffer_surface, u32 dest_surface);

    /// Shared IDirect3DDevice9 / IDirect3DSwapChain9::GetRasterStatus.
    void get_raster_status(Marshaler& m, u32 swap_chain_index, GuestAddr out);
    void stretch_rect(Marshaler& m, u32 device);
    void color_fill(Marshaler& m, u32 device);

    /// Backend colour texture for a surface (own RT/DS backend, or the parent
    /// 2D/cube texture's shared texture). Null when only guest pixels exist.
    [[nodiscard]] graphics::BackendTexture2D*
    resolve_surface_color_backend(SurfaceState& surface);

    /// DeviceState for a device COM object, or null.
    [[nodiscard]] DeviceState* device_state_for(u32 device);

    /// Owning TextureState for a texture-level surface, or null.
    [[nodiscard]] TextureState* texture_state_for_surface(
        const SurfaceState& surface);
    void dispatch_swap_chain(Marshaler& m, usize slot, u32 self);
    void dispatch_surface(Marshaler& m, usize slot, u32 self);
    void dispatch_texture(Marshaler& m, usize slot, u32 self);
    void dispatch_cube_texture(Marshaler& m, usize slot, u32 self);
    void dispatch_volume_texture(Marshaler& m, usize slot, u32 self);
    void dispatch_volume(Marshaler& m, usize slot, u32 self);
    void dispatch_vertex_buffer(Marshaler& m, usize slot, u32 self);
    void dispatch_index_buffer(Marshaler& m, usize slot, u32 self);
    void dispatch_shader(Marshaler& m, usize slot, u32 self, bool pixel);
    void dispatch_state_block(Marshaler& m, usize slot, u32 self);
    void dispatch_vertex_declaration(Marshaler& m, usize slot, u32 self);
    void dispatch_query(Marshaler& m, usize slot, u32 self);

    /// Parse a guest D3DVERTEXELEMENT9 array into @p out (END excluded).
    [[nodiscard]] bool parse_vertex_declaration(
        GuestAddr elements_addr, VertexDeclarationState& out,
        const char*& rejection) const;

    /// IDirect3DDevice9::CreateVertexShader / CreatePixelShader.
    void create_shader(Marshaler& m, u32 device, bool pixel);

    /// Vertex and index buffers share every method but their descriptor.
    void dispatch_buffer(Marshaler& m, usize slot, u32 self,
                         bool index_buffer);

    /// IDirect3DResource9 slots 0..10, shared by every resource interface.
    /// Returns false when the slot is not part of the shared base.
    [[nodiscard]] bool dispatch_resource_base(Marshaler& m, usize slot,
                                              u32 self, u32 resource_type);

    /// SetPrivateData / GetPrivateData / FreePrivateData (slots 4..6).
    void dispatch_private_data(Marshaler& m, usize slot, ComObject& entry);

    /// Release a D3DSPD_IUNKNOWN private-data entry (no-op for raw blobs).
    void release_private_data_iunknown(const ComObject::PrivateDataEntry& data);

    /// Drop all private data on @p entry, Release-ing IUNKNOWN payloads.
    /// Skips Release of @p self to avoid re-entrancy during destroy.
    void clear_private_data(ComObject& entry, u32 self);

    /// AddRef a D3DSPD_IUNKNOWN payload: Vitreo ComObject fast path, else
    /// optional guest callback. Returns 0 when the pointer cannot be retained.
    [[nodiscard]] u32 retain_private_iunknown(u32 punk);

    /// Release a D3DSPD_IUNKNOWN payload: Vitreo ComObject fast path, else
    /// optional guest callback.
    void release_private_iunknown(u32 punk);

    /// IUnknown slots 0..2 for any interface. Returns false for other slots.
    [[nodiscard]] bool dispatch_unknown(Marshaler& m, usize slot, u32 self,
                                        D3D9InterfaceId iface);

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------

    [[nodiscard]] bool read_guid(GuestAddr address,
                                 std::array<u8, 16>& guid) const;
    [[nodiscard]] bool valid_adapter(u32 adapter, u32 device_type) const;
    [[nodiscard]] bool current_display_mode(std::array<u32, 4>& mode) const;

    /// Resolve BackBufferWidth/Height/Format per D3DPRESENT_PARAMETERS:
    /// windowed zeros take the device/focus window client area; unknown
    /// format (and any still-zero size) falls back to the display mode.
    void resolve_present_back_buffer(bool windowed, u32 window, u32& width,
                                     u32& height, u32& format) const;

    /// Apply or clear exclusive fullscreen for the presentation HWND after
    /// CreateDevice / Reset. Mirrors DirectDraw's cooperative-level path.
    void apply_exclusive_fullscreen(const DeviceState& state);
    void clear_exclusive_fullscreen();

    /// Opaque white 1×1 texture for NULL SetTexture slots (Windows samples
    /// white when a stage/sampler has no texture bound).
    [[nodiscard]] graphics::BackendTexture2D* default_white_texture();

    /// Learn: palettized textures sample through the current device palette.
    /// Expand guest P8/A8P8 indices into the BGRA8 backend when @p force or
    /// the texture's upload serial lags `state.texture_palette_serial`.
    void sync_palettized_texture(TextureState& texture, DeviceState& state,
                                 bool force);

    /// Regenerate AUTOGENMIPMAP sublevels using `auto_gen_filter`.
    /// LINEAR uses the backend blit; POINT uses CPU nearest-neighbor.
    /// @p device / @p container identify the texture COM object so missing
    /// mip level surfaces/volumes can be materialized.
    void generate_autogen_mips(Marshaler& m, TextureState& texture, u32 device,
                               u32 container);

    /// Re-expand every bound palettized texture after palette state changes.
    void sync_bound_palettized_textures(DeviceState& state);

    /// Map @p bytes of guest-writable memory for a lockable resource.
    [[nodiscard]] GuestAddr map_lock_region(usize bytes);
    void unmap_lock_region(GuestAddr base, usize bytes);

    GuestMemory* m_memory = nullptr;
    bool m_is_64bit = false;
    graphics::GraphicsBackendDevice* m_backend = nullptr;
    DisplaySizeResolver m_display_size_resolver;
    ClientSizeResolver m_client_size_resolver;
    PresentationViewResolver m_presentation_view_resolver;
    GuestIUnknownAddRefFn m_guest_iunknown_addref;
    GuestIUnknownReleaseFn m_guest_iunknown_release;
    FullscreenFn m_fullscreen_cb;
    DisplayModeFn m_display_mode_cb;
    CursorVisibleFn m_cursor_visible_cb;
    CursorPositionFn m_cursor_position_cb;
    CursorImageFn m_cursor_image_cb;
    u32 m_fullscreen_hwnd{0};
    u32 m_exclusive_mode_width{0};
    u32 m_exclusive_mode_height{0};
    std::unique_ptr<graphics::BackendTexture2D> m_default_white_texture;

    std::array<InterfaceThunks, D3D9_INTERFACE_COUNT> m_thunks{};
    std::array<GuestAddr, D3D9_INTERFACE_COUNT> m_vtables{};

    GuestAddr m_region_base = 0;
    u32 m_next_obj_addr = 0;
    bool m_storage_ready = false;
    /// Addresses returned by Release/destroy; reused before bumping
    /// `m_next_obj_addr` so create/destroy churn cannot exhaust storage.
    std::vector<u32> m_free_obj_addrs;

    std::unordered_map<u32, ComObject> m_objects;
    std::recursive_mutex m_mutex;

public:
    /// Temporarily drops the manager lock around a call that BLOCKS.
    ///
    /// `impl_method` holds `m_mutex` for the whole of every D3D9 call, and for
    /// Present that call ends in MetalSubmitQueue::submit(), which waits for
    /// vblank / GPU backpressure. Holding the process's only D3D9 lock while
    /// asleep on the GPU stalls every other guest thread that touches D3D9 --
    /// measured at ~84% of wall on the render thread, with the guest pinned to
    /// Reduced host CPU usage in the measured workload.
    ///
    /// Only the blocking call belongs inside this guard, and nothing inside it
    /// may touch manager state: capture what you need first.
    ///
    /// Safe under re-entry. A nested impl_method (a guest callback that calls
    /// back into D3D9) leaves an outer frame still holding the recursive
    /// mutex, so releasing one level simply does not free it -- correct, just
    /// without the benefit.
    class UnlockDuringBlockingCall {
    public:
        UnlockDuringBlockingCall();
        ~UnlockDuringBlockingCall();
        UnlockDuringBlockingCall(const UnlockDuringBlockingCall&) = delete;
        UnlockDuringBlockingCall& operator=(const UnlockDuringBlockingCall&) = delete;
    private:
        std::unique_lock<std::recursive_mutex>* m_lock = nullptr;
    };

private:
    /// The unique_lock `impl_method` is holding on this thread, if any.
    static thread_local std::unique_lock<std::recursive_mutex>* t_active_lock;

};

/// Create d3d9.dll export thunks.
DllExports create_d3d9_thunks(D3D9Manager& mgr);

} // namespace vitreo::winapi
