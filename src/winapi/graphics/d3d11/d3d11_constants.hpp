// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d11_constants.hpp
/// @brief The d3d11:: constants of the Direct3D 11 frontend: HRESULTs,
/// driver types, device-creation flags, feature levels, interface method
/// counts, binding-slot counts and ShaderBindingLayout.
///
/// Split out of d3d11.hpp, which includes it.

#include <vitreo/common.hpp>
#include "graphics/contracts/backend_device.hpp"
#include "winapi/graphics/d3d11/d3d11_draw_memo.hpp"

#include <array>

namespace vitreo::winapi {

namespace d3d11 {

constexpr u32 S_OK = 0;
constexpr u32 E_NOTIMPL = 0x80004001u;
constexpr u32 E_NOINTERFACE = 0x80004002u;
constexpr u32 E_POINTER = 0x80004003u;
constexpr u32 E_INVALIDARG = 0x80070057u;
constexpr u32 E_FAIL = 0x80004005u;
constexpr u32 E_OUTOFMEMORY = 0x8007000Eu;
constexpr u32 DXGI_ERROR_UNSUPPORTED = 0x887A0004u;
constexpr u32 DXGI_ERROR_SDK_COMPONENT_MISSING = 0x887A002Du;
// winerror.h: the swap-chain results beyond INVALID_CALL.
constexpr u32 DXGI_STATUS_OCCLUDED = 0x087A0001u;
constexpr u32 DXGI_STATUS_MODE_CHANGE_IN_PROGRESS = 0x087A0008u;
constexpr u32 DXGI_ERROR_FRAME_STATISTICS_DISJOINT = 0x887A000Bu;
constexpr u32 DXGI_ERROR_NOT_CURRENTLY_AVAILABLE = 0x887A0022u;
constexpr u32 E_ACCESSDENIED = 0x80070005u;
constexpr u32 DSV_READ_ONLY_DEPTH = 0x1u;
constexpr u32 DSV_READ_ONLY_STENCIL = 0x2u;

constexpr u32 SDK_VERSION = 7;
// d3dcommon.h D3D_DRIVER_TYPE. REFERENCE and SOFTWARE were swapped here
// (2 and 4 are REFERENCE and SOFTWARE, in that order) and NULL was missing.
constexpr u32 DRIVER_TYPE_UNKNOWN = 0;
constexpr u32 DRIVER_TYPE_HARDWARE = 1;
constexpr u32 DRIVER_TYPE_REFERENCE = 2;
constexpr u32 DRIVER_TYPE_NULL = 3;
constexpr u32 DRIVER_TYPE_SOFTWARE = 4;
constexpr u32 DRIVER_TYPE_WARP = 5;

constexpr u32 CREATE_DEVICE_SINGLETHREADED = 0x1;
/// D3D11_FEATURE_DATA_THREADING::DriverCommandLists. FALSE: command lists
/// are the runtime's own recordings (d3d11_deferred.cpp), with the deferred
/// UpdateSubresource source-offset behaviour Learn documents for that case.
constexpr bool kDriverCommandLists = false;
constexpr u32 CREATE_DEVICE_DEBUG = 0x2;
/// D3D11_CREATE_DEVICE_FLAG bits the runtime creates a device with and echoes
/// through GetCreationFlags, measured on Windows
/// (d3d11_a01_device_caps_probe "flags 0x..." lines): SINGLETHREADED 0x1,
/// PREVENT_INTERNAL_THREADING_OPTIMIZATIONS 0x8, BGRA_SUPPORT 0x20,
/// PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY 0x80, DISABLE_GPU_TIMEOUT
/// 0x100, VIDEO_SUPPORT 0x800.
constexpr u32 CREATE_DEVICE_KEPT_FLAGS = 0x000009A9u;
/// Bits the runtime accepts and drops (GetCreationFlags does not report
/// them): 0x1000, 0x8000000, 0x40000000, 0x80000000. Every other bit --
/// DEBUG, SWITCH_TO_REF, DEBUGGABLE and the undefined ones -- asks for an SDK
/// layer and answers DXGI_ERROR_SDK_COMPONENT_MISSING without one.
constexpr u32 CREATE_DEVICE_DROPPED_FLAGS = 0xC8001000u;
/// HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND): D3D_DRIVER_TYPE_SOFTWARE with a
/// module that exports no user-mode driver entry points (measured).
constexpr u32 HRESULT_PROC_NOT_FOUND = 0x8007007Fu;

constexpr u32 FEATURE_LEVEL_9_1 = 0x9100;
constexpr u32 FEATURE_LEVEL_9_2 = 0x9200;
constexpr u32 FEATURE_LEVEL_9_3 = 0x9300;
constexpr u32 FEATURE_LEVEL_10_0 = 0xA000;
constexpr u32 FEATURE_LEVEL_10_1 = 0xA100;
constexpr u32 FEATURE_LEVEL_11_0 = 0xB000;
constexpr u32 FEATURE_LEVEL_11_1 = 0xB100;
constexpr u32 FEATURE_LEVEL_12_0 = 0xC000;
constexpr u32 FEATURE_LEVEL_12_1 = 0xC100;
constexpr u32 FEATURE_LEVEL_12_2 = 0xC200;

/// ID3D11Device5 method count (Device1=50 + Device2–5 extensions).
constexpr usize DEVICE_METHOD_COUNT = 69;
/// ID3D11DeviceContext4 method count, from the SDK vtable: Context1 ends at 133
/// with DiscardView1, Context2 adds ten (UpdateTileMappings..EndEvent, 134-143),
/// Context3 adds three (Flush1, Set/GetHardwareProtectionState, 144-146) and
/// Context4 adds Signal and Wait at 147 and 148.
///
/// This was 147, which is Context3's count. QI accepts the Context4 IID, so a
/// guest could obtain the interface and find slots 147 and 148 null in its
/// vtable -- a call through either one jumped to address zero.
constexpr usize CONTEXT_METHOD_COUNT = 149;
// IDXGIDevice (7..9) + IDXGIDevice1 (10..13) + IDXGIDevice2 (14..16) +
// IDXGIDevice3::Trim (17) + IDXGIDevice4 Offer/ReclaimResources1 (18, 19):
// dxgi1_5.h IDXGIDevice4Vtbl. QueryInterface answers IDXGIDevice4.
constexpr usize DXGI_DEVICE_METHOD_COUNT = 20;
constexpr usize BUFFER_METHOD_COUNT = 11;
// ID3D11Texture2D1 / ID3D11Texture3D1 (d3d11_3.h): GetDesc1 at slot 11. The
// one table serves every texture dimension; a Texture1D has no `1` form, so
// its slot 11 is simply never reached through QueryInterface.
constexpr usize TEXTURE_2D_METHOD_COUNT = 12;
// IDXGIResource (0..11) + IDXGIResource1::CreateSubresourceSurface /
// CreateSharedHandle (12..13).
constexpr usize DXGI_RESOURCE_METHOD_COUNT = 14;
/// IDXGIKeyedMutex: IUnknown + IDXGIObject + GetDevice + Acquire/ReleaseSync.
constexpr usize KEYED_MUTEX_METHOD_COUNT = 10;
// A vtable must be as long as the newest interface QueryInterface answers for
// the object: QI handing out ID3D11ShaderResourceView1 with a 9-slot table left
// GetDesc1 (slot 9) null, and the guest branched to address zero. The counts
// below are the `1` interfaces from d3d11_3.h (views, ID3D11Query1) and
// d3d11_1.h (ID3D11BlendState1); tools/audit_d3d11_vtables.py checks each one
// against the SDK vtable it is registered with.
constexpr usize SHADER_RESOURCE_VIEW_METHOD_COUNT = 10;
constexpr usize UNORDERED_ACCESS_VIEW_METHOD_COUNT = 10;
constexpr usize RENDER_TARGET_VIEW_METHOD_COUNT = 10;
constexpr usize DEPTH_STENCIL_VIEW_METHOD_COUNT = 9;
constexpr usize QUERY_METHOD_COUNT = 10;
/// ID3D11Fence: IUnknown + ID3D11DeviceChild + CreateSharedHandle,
/// GetCompletedValue, SetEventOnCompletion.
constexpr usize FENCE_METHOD_COUNT = 10;
/// ID3D11CommandList: IUnknown + ID3D11DeviceChild + GetContextFlags.
constexpr usize COMMAND_LIST_METHOD_COUNT = 8;
// IDXGISwapChain (0..17) + IDXGISwapChain1 (18..28) + IDXGISwapChain2
// (29..35) + IDXGISwapChain3 (36..39) + IDXGISwapChain4::SetHDRMetaData (40):
// dxgi1_5.h IDXGISwapChain4Vtbl. QueryInterface answers IDXGISwapChain4.
constexpr usize SWAP_CHAIN_METHOD_COUNT = 41;
/// IDXGISwapChainMedia (dxgi1_3.h): IUnknown + GetFrameStatisticsMedia,
/// SetPresentDuration, CheckPresentDurationSupport. A facet of the chain.
constexpr usize SWAP_CHAIN_MEDIA_METHOD_COUNT = 6;
constexpr usize ANNOTATION_METHOD_COUNT = 7;
constexpr usize VERTEX_SHADER_METHOD_COUNT = 7;
constexpr usize PIXEL_SHADER_METHOD_COUNT = 7;
constexpr usize COMPUTE_SHADER_METHOD_COUNT = 7;
// ID3D11GeometryShader: IUnknown + ID3D11DeviceChild, no methods of its own.
constexpr usize GEOMETRY_SHADER_METHOD_COUNT = 7;
// ID3D11HullShader / ID3D11DomainShader: likewise.
constexpr usize HULL_SHADER_METHOD_COUNT = 7;
constexpr usize DOMAIN_SHADER_METHOD_COUNT = 7;
// IDXGISurface2 (a 2D texture's surface facet) and ID3DDeviceContextState.
constexpr usize DXGI_SURFACE_METHOD_COUNT = 14;
constexpr usize DEVICE_CONTEXT_STATE_METHOD_COUNT = 7;
constexpr usize INPUT_LAYOUT_METHOD_COUNT = 7;
constexpr usize BLEND_STATE_METHOD_COUNT = 9;
// ID3D11RasterizerState2 extends the base interface with GetDesc1/GetDesc2.
constexpr usize RASTERIZER_STATE_METHOD_COUNT = 10;
constexpr usize DEPTH_STENCIL_STATE_METHOD_COUNT = 8;
constexpr usize SAMPLER_STATE_METHOD_COUNT = 8;
/// ID3D11VideoContext3 (d3d11_4.h): IUnknown, ID3D11DeviceChild, the 58
/// ID3D11VideoContext methods, 14 of VideoContext1, 4 of VideoContext2 and 2
/// of VideoContext3 (tools/audit_d3d11_vtables.py --emit
/// VIDEO_CONTEXT_STDCALL_BYTES).
constexpr usize VIDEO_CONTEXT_METHOD_COUNT = 85;
/// ID3D11Multithread (d3d11_4.h): IUnknown + Enter, Leave,
/// SetMultithreadProtected, GetMultithreadProtected. ID3D10Multithread
/// (d3d10.h) has the same seven slots in the same order, so one table serves
/// both, as one pointer does on Windows (measured, a12 refmodel [mt]).
constexpr usize MULTITHREAD_METHOD_COUNT = 7;
/// ID3D11ClassLinkage: IUnknown + ID3D11DeviceChild + GetClassInstance,
/// CreateClassInstance.
constexpr usize CLASS_LINKAGE_METHOD_COUNT = 9;
/// ID3D11ClassInstance: IUnknown + ID3D11DeviceChild + GetClassLinkage,
/// GetDesc, GetInstanceName, GetTypeName.
constexpr usize CLASS_INSTANCE_METHOD_COUNT = 11;

// Per-stage register-space sizes, from Microsoft Learn "Resource Limits
// (Direct3D 11)" for feature level 11. These are the sizes a guest may address,
// and they are independent of how many of them this backend can bind in one
// draw: a binding must be RECORDED even when the draw cannot serve it, because
// the matching Get* call has to return it.
//
// D3D11_COMMONSHADER_INPUT_RESOURCE_REGISTER_COUNT.
constexpr usize SHADER_RESOURCE_SLOTS = 128;
// D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT. Learn: 15 hardware slots,
// one set aside for the immediate constant buffer, leaving 14 to the API.
constexpr usize CONSTANT_BUFFER_SLOTS = 14;
// D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT.
constexpr usize SAMPLER_SLOTS = 16;
// D3D11_PS_CS_UAV_REGISTER_COUNT. This is the D3D11.0 value and already what
// the backend serves; D3D11.1 raises it to 64 via D3D11_1_UAV_SLOT_COUNT, which
// is a separate contract this frontend does not yet claim.
constexpr usize UAV_SLOTS = 8;
// D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT. Every slot is recorded and read
// back; the draw maps the slots an input layout uses onto the backend's
// BACKEND_MAX_VERTEX_BUFFERS streams (InputLayout::backend_stream).
constexpr usize VERTEX_BUFFER_SLOTS = 32;
// R4 (d3d11_draw_memo.hpp): every stage's t# and s# arrays re-stamp on each
// change, so draw-time derivations can be keyed by the stamp.
using SrvSlots = StampedSlots<GuestAddr, SHADER_RESOURCE_SLOTS>;
using SamplerSlots = StampedSlots<GuestAddr, SAMPLER_SLOTS>;
// D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT: the most constants a shader can
// address in one b# register. It is also the NumConstants that
// *GetConstantBuffers1 reports for a slot bound as a whole buffer -- by the
// D3D11.0 *SetConstantBuffers, by *SetConstantBuffers1 with NULL windows, with
// a NULL buffer, or never bound (measured, d3d11_a05_context_state_probe).
constexpr u32 WHOLE_CONSTANT_WINDOW = 4096;
inline constexpr std::array<u32, CONSTANT_BUFFER_SLOTS> kWholeConstantWindows =
    [] {
        std::array<u32, CONSTANT_BUFFER_SLOTS> windows{};
        windows.fill(WHOLE_CONSTANT_WINDOW);
        return windows;
    }();

/// D3D11_BUFFER_UAV_FLAG. RAW makes this a byte-addressed RWByteAddressBuffer
/// and is a separate shader contract; APPEND and COUNTER each attach a hidden
/// counter, and Learn says the two are alternatives, not a combination.
constexpr u32 BUFFER_UAV_FLAG_RAW = 0x1;
constexpr u32 BUFFER_UAV_FLAG_APPEND = 0x2;
constexpr u32 BUFFER_UAV_FLAG_COUNTER = 0x4;

/// Which stage's binding state a descriptor-table sync mirrors. Only the three
/// stages this frontend can execute; hull/domain/geometry have no shader to bind
/// for, so they have no tables.
enum class StageBindings : u32 { Vertex = 0, Pixel = 1, Compute = 2, Count };

/// How a compiled shader expects its registers to be bound.
struct ShaderBindingLayout {
    /// Highest declared register + 1, per D3D register space. Zero when the
    /// shader declares none.
    u32 srv_slots{0};
    u32 constant_buffer_slots{0};
    u32 sampler_slots{0};
    /// True when a space exceeds the backend's flat capacity, so this shader
    /// reads descriptor tables. Both modes want the same Metal buffer indices,
    /// so a stage is in exactly one of them.
    bool uses_descriptor_tables{false};
    /// The u# of each hidden counter the shader moves, in binding order
    /// (DxbcToSpirvResult::uav_counter_registers). Fixed-size because the
    /// layout is copied on every draw.
    std::array<u8, graphics::BACKEND_MAX_UAV_COUNTER_BINDINGS>
        uav_counter_registers{};
    u8 uav_counter_count{0};
    /// t# / u# registers by declared shape (DxbcBindingDeclaration::
    /// dimension): bit n set in `*_flat` for a Texture1D/Texture2D
    /// declaration, in `*_arrayed` for Texture1DArray/Texture2DArray. Read by
    /// fit_view_to_declaration.
    std::array<u64, SHADER_RESOURCE_SLOTS / 64> srv_flat{};
    std::array<u64, SHADER_RESOURCE_SLOTS / 64> srv_arrayed{};
    u64 uav_flat{0};
    u64 uav_arrayed{0};
};

/// Fits @p view, the backend view of the texture SRV (or, @p uav, the UAV)
/// bound at register @p slot, to the shape the shader declares there.
/// Measured (tools/oracle_probe/d3d11_array_window_probe [srv] / [uav]): a
/// Texture2D declaration over a TEXTURE2DARRAY view reads element 0 of the
/// view -- its FirstArraySlice -- under the view's mip window; a
/// Texture2DArray declaration over a TEXTURE2D view sees one element (Load
/// of any other index returns 0, GetDimensions reports 1). Metal reads an
/// image through a declaration of another type as garbage, so the view is
/// re-typed to the declaration. False (logged once) for a pairing with no
/// measured behaviour -- a cube or multisample view under a 1D/2D(-array)
/// declaration -- which the caller leaves unbound.
[[nodiscard]] bool fit_view_to_declaration(
    const ShaderBindingLayout& layout, bool uav, u32 slot,
    graphics::BackendTextureViewDesc& view);

} // namespace d3d11

} // namespace vitreo::winapi
