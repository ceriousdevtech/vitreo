// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d11.hpp
/// @brief Direct3D 11 device-creation and base COM-interface bridge.
///
/// This is the first D3D11 frontend layer above Vitreo's Metal device. It
/// implements device selection, feature-level negotiation, COM identity and
/// lifetime, the immediate-context relationship, texture/render-target
/// resources, and DXGI swap-chain presentation through the shared graphics
/// backend.
///
/// The d3d11:: constants live in d3d11_constants.hpp and the CPU-mapping
/// types in d3d11_cpu_mapping.hpp, both included below. The private
/// surface of D3D11Manager is spread over the d3d11_manager_*.inc
/// fragments, which the class body includes in order.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "winapi/core/guest_com.hpp"
#include "winapi/graphics/d3d11/d3d11_format_caps.hpp"
#include "winapi/graphics/d3d11/d3d11_standard_swizzle.hpp"
#include "winapi/graphics/d3d11/d3d11_draw_memo.hpp"
#include "winapi/graphics/d3d10/d3d10_stdcall.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <optional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <span>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The binding-window and descriptor-set constants are values, not types, so the
// forward declarations below cannot cover them. Including the contract header is
// also what keeps this frontend from transcribing a capacity that could drift
// from the backend's.
#include "graphics/contracts/backend_device.hpp"
#include "graphics/shaders/stream_out_repack.hpp"
#include "graphics/shaders/pipeline_stats.hpp"
#include "graphics/shaders/gs_passthrough.hpp"
#include "winapi/graphics/d3d11/d3d11_transient_ring.hpp"
#include "winapi/graphics/d3d11/d3d11_constants.hpp"
#include "winapi/graphics/d3d11/d3d11_cpu_mapping.hpp"

namespace vitreo {
class GuestMemory;
}

namespace vitreo::graphics {
enum class BackendBlendFactor;
class BackendBuffer;
struct DxbcContainer;
struct DxbcClassLinkage;
struct BackendCompositionLayerDesc;
class BackendPipeline;
class BackendSwapChain;
class BackendCommandList;
class BackendDescriptorTable;
class BackendFence;
struct BackendSamplerDesc;
struct BackendTextureViewDesc;
class BackendTextureBuffer;
class BackendTexture2D;
class BackendTexture3D;
class BackendTilePool;
class BackendTextureBuffer;
class GraphicsBackendDevice;
class GpuCompletion;
enum class BackendBlendFactor;
enum class PixelFormat : u32;
enum class BackendColorSpace : u32;
enum class ShaderStage : u8;
}

namespace vitreo::winapi {

class DxgiManager;
class HandleTable;
class Marshaler;

class SharedKeyedMutex;

/// The argument block of an indirect draw (D3D and Metal spell it the same,
/// graphics::BackendIndirectArgs): a guest DRAWINDIRECT_ARGS buffer, or one
/// of Vitreo's own (DrawAuto's, an emulation pass's). Inactive: a direct draw.
struct IndirectDraw {
    const graphics::BackendBuffer* args{nullptr};
    u32 offset{0};
    [[nodiscard]] bool active() const noexcept { return args != nullptr; }
};

class D3D11Manager {
public:
    /// The device lock every guest entry takes (see m_api_mutex).
    [[nodiscard]] std::recursive_mutex& api_mutex() noexcept {
        return m_api_mutex;
    }
    /// How many times the calling thread holds api_mutex() through guest
    /// entries. A call that must block for another guest thread (a keyed
    /// mutex wait) releases the lock that many times while it waits.
    [[nodiscard]] int& api_lock_depth() noexcept;

    /// Gives the device lock up, for its lifetime, around work that reads and
    /// writes no device-shared state: shader translation into locals, a
    /// Metal pipeline compile from a self-contained description. Only when
    /// the calling thread's one hold is the guest entry it is in
    /// (api_lock_depth() == 1): an ID3D11Multithread::Enter, or an entry
    /// nested in another, keeps the lock, and the work then runs under it.
    /// The caller must not keep a map iterator across the scope: an insert
    /// by another thread can rehash. References to map values survive.
    class PureWorkUnlock {
    public:
        explicit PureWorkUnlock(D3D11Manager& manager, bool allowed = true);
        ~PureWorkUnlock();
        PureWorkUnlock(const PureWorkUnlock&) = delete;
        PureWorkUnlock& operator=(const PureWorkUnlock&) = delete;
        [[nodiscard]] bool released() const noexcept { return m_released; }

    private:
        D3D11Manager& m_manager;
        bool m_released{false};
    };
    /// Test hook: run on the releasing thread inside every PureWorkUnlock
    /// that gave the lock up. nullptr clears it.
    static void set_pure_work_unlock_hook_for_test(void (*hook)(void*),
                                                   void* context) noexcept;

    /// R4 (d3d11_draw_memo.hpp): memo hits and misses, for tests and
    /// diagnostics.
    struct DrawMemoCounters {
        u64 stage_hits{0}, stage_misses{0};
        u64 sampler_hits{0}, sampler_misses{0};
        u64 ia_hits{0}, ia_misses{0};
        u64 srv_hits{0}, srv_misses{0};
        u64 om_hits{0}, om_misses{0};
        u64 key_hits{0}, key_misses{0};
        u64 vb_hits{0}, vb_misses{0};
    };
    [[nodiscard]] DrawMemoCounters draw_memo_counters() const noexcept {
        return DrawMemoCounters{m_r4_stage_hits,   m_r4_stage_misses,
                                m_r4_sampler_hits, m_r4_sampler_misses,
                                m_r4_ia_hits,      m_r4_ia_misses,
                                m_r4_srv_hits,     m_r4_srv_misses,
                                m_r4_om_hits,      m_r4_om_misses,
                                m_r4_key_hits,     m_r4_key_misses,
                                m_r4_vb_hits,      m_r4_vb_misses};
    }

    /// gs-ring (graphics/contracts/slim_record.hpp): how the geometry
    /// emulation's transient buffers were served, for tests and diagnostics.
    [[nodiscard]] const TransientBufferRing::Stats& transient_ring_stats()
        const noexcept {
        return m_transient.stats();
    }

    using PresentationViewResolver = std::function<void*(u64 hwnd)>;
    using ForeignWindowResolver = std::function<bool(u64 hwnd)>;
    using WindowPixelsPresenter = std::function<bool(
        u64 hwnd, const std::vector<u8>& rgba, u32 width, u32 height)>;
    /// `sync_interval` is the originating guest's Present SyncInterval,
    /// carried to the owner process because the layer lives there.
    /// width/height are the PRESENTED size; src_x/src_y locate that
    /// rectangle inside the shared texture (a DirectComposition clip on a
    /// pooled swap chain), zero when the whole surface is presented.
    using WindowSurfacePresenter =
        std::function<bool(u64 hwnd, u64 shared_handle, u32 width, u32 height,
                           u32 sync_interval, u32 src_x, u32 src_y)>;

    struct MethodThunks {
        std::array<GuestAddr, d3d11::DEVICE_METHOD_COUNT> device{};
        std::array<GuestAddr, d3d11::CONTEXT_METHOD_COUNT> context{};
        std::array<GuestAddr, d3d11::DXGI_DEVICE_METHOD_COUNT> dxgi_device{};
        std::array<GuestAddr, d3d11::BUFFER_METHOD_COUNT> buffer{};
        std::array<GuestAddr, d3d11::TEXTURE_2D_METHOD_COUNT> texture_2d{};
        std::array<GuestAddr, d3d11::DXGI_RESOURCE_METHOD_COUNT>
            dxgi_resource{};
        std::array<GuestAddr, d3d11::KEYED_MUTEX_METHOD_COUNT> keyed_mutex{};
        std::array<GuestAddr, d3d11::SHADER_RESOURCE_VIEW_METHOD_COUNT>
            shader_resource_view{};
        std::array<GuestAddr, d3d11::UNORDERED_ACCESS_VIEW_METHOD_COUNT>
            unordered_access_view{};
        std::array<GuestAddr, d3d11::RENDER_TARGET_VIEW_METHOD_COUNT>
            render_target_view{};
        std::array<GuestAddr, d3d11::DEPTH_STENCIL_VIEW_METHOD_COUNT>
            depth_stencil_view{};
        std::array<GuestAddr, d3d11::QUERY_METHOD_COUNT> query{};
        std::array<GuestAddr, d3d11::FENCE_METHOD_COUNT> fence{};
        std::array<GuestAddr, d3d11::COMMAND_LIST_METHOD_COUNT> command_list{};
        std::array<GuestAddr, d3d11::SWAP_CHAIN_METHOD_COUNT> swap_chain{};
        std::array<GuestAddr, d3d11::SWAP_CHAIN_MEDIA_METHOD_COUNT>
            swap_chain_media{};
        std::array<GuestAddr, d3d11::ANNOTATION_METHOD_COUNT> annotation{};
        std::array<GuestAddr, d3d11::VERTEX_SHADER_METHOD_COUNT> vertex_shader{};
        std::array<GuestAddr, d3d11::PIXEL_SHADER_METHOD_COUNT> pixel_shader{};
        std::array<GuestAddr, d3d11::COMPUTE_SHADER_METHOD_COUNT>
            compute_shader{};
        std::array<GuestAddr, d3d11::GEOMETRY_SHADER_METHOD_COUNT>
            geometry_shader{};
        std::array<GuestAddr, d3d11::HULL_SHADER_METHOD_COUNT> hull_shader{};
        std::array<GuestAddr, d3d11::DOMAIN_SHADER_METHOD_COUNT>
            domain_shader{};
        std::array<GuestAddr, d3d11::DXGI_SURFACE_METHOD_COUNT> dxgi_surface{};
        std::array<GuestAddr, d3d11::DEVICE_CONTEXT_STATE_METHOD_COUNT>
            device_context_state{};
        std::array<GuestAddr, d3d11::INPUT_LAYOUT_METHOD_COUNT> input_layout{};
        std::array<GuestAddr, d3d11::BLEND_STATE_METHOD_COUNT> blend_state{};
        std::array<GuestAddr, d3d11::RASTERIZER_STATE_METHOD_COUNT> rasterizer_state{};
        std::array<GuestAddr, d3d11::DEPTH_STENCIL_STATE_METHOD_COUNT> depth_stencil_state{};
        std::array<GuestAddr, d3d11::SAMPLER_STATE_METHOD_COUNT> sampler_state{};
        std::array<GuestAddr, d3d11::VIDEO_CONTEXT_METHOD_COUNT> video_context{};
        std::array<GuestAddr, d3d11::MULTITHREAD_METHOD_COUNT> multithread{};
        std::array<GuestAddr, d3d11::CLASS_LINKAGE_METHOD_COUNT>
            class_linkage{};
        std::array<GuestAddr, d3d11::CLASS_INSTANCE_METHOD_COUNT>
            class_instance{};
        // The D3D10 facets of the same objects
        // (winapi/graphics/d3d10/d3d10_device.cpp). Optional as a set, like
        // the keyed mutex's: a harness that wires none gets D3D11 alone.
        std::array<GuestAddr, d3d10::DEVICE_METHOD_COUNT> d3d10_device{};
        std::array<GuestAddr, d3d10::BUFFER_METHOD_COUNT> d3d10_buffer{};
        std::array<GuestAddr, d3d10::TEXTURE_METHOD_COUNT> d3d10_texture{};
        std::array<GuestAddr, d3d10::SHADER_RESOURCE_VIEW_METHOD_COUNT>
            d3d10_shader_resource_view{};
        std::array<GuestAddr, d3d10::RENDER_TARGET_VIEW_METHOD_COUNT>
            d3d10_render_target_view{};
        std::array<GuestAddr, d3d10::DEPTH_STENCIL_VIEW_METHOD_COUNT>
            d3d10_depth_stencil_view{};
        std::array<GuestAddr, d3d10::BLEND_STATE_METHOD_COUNT>
            d3d10_blend_state{};
        std::array<GuestAddr, d3d10::DEPTH_STENCIL_STATE_METHOD_COUNT>
            d3d10_depth_stencil_state{};
        std::array<GuestAddr, d3d10::RASTERIZER_STATE_METHOD_COUNT>
            d3d10_rasterizer_state{};
        std::array<GuestAddr, d3d10::SAMPLER_STATE_METHOD_COUNT>
            d3d10_sampler_state{};
        std::array<GuestAddr, d3d10::DEVICE_CHILD_METHOD_COUNT>
            d3d10_device_child{};
        std::array<GuestAddr, d3d10::QUERY_METHOD_COUNT> d3d10_query{};
        std::array<GuestAddr, d3d10::BLOB_METHOD_COUNT> d3d10_blob{};
        std::array<GuestAddr, d3d10::STATE_BLOCK_METHOD_COUNT>
            d3d10_state_block{};
    };

    D3D11Manager(GuestMemory& memory, bool is_64bit,
                 graphics::GraphicsBackendDevice& backend,
                 DxgiManager& dxgi_manager);
    ~D3D11Manager();

    D3D11Manager(const D3D11Manager&) = delete;
    D3D11Manager& operator=(const D3D11Manager&) = delete;

    void set_method_thunks(MethodThunks thunks);
    /// Supply the host->guest call used to AddRef/Release guest-owned
    /// interfaces. Without it SetPrivateDataInterface refuses rather than
    /// holding a reference it never took. See winapi/core/guest_com.hpp.
    void set_guest_function_caller(GuestFunctionCaller caller);
    /// Recognises ID3D12Device objects of Vitreo's own d3d12.dll frontend,
    /// which D3D11On12CreateDevice would have to wrap.
    void set_d3d12_device_query(std::function<bool(GuestAddr)> query) {
        m_is_d3d12_device = std::move(query);
    }
    void set_presentation_view_resolver(PresentationViewResolver resolver);
    void set_foreign_window_presenter(ForeignWindowResolver resolver,
                                      WindowPixelsPresenter pixel_presenter,
                                      WindowSurfacePresenter surface_presenter = {});
    void set_foreign_surface_view_resolver(PresentationViewResolver resolver);

    /// What DXGI needs of the HWND a swap chain presents to. Unset members
    /// mean no window system is attached (unit tests without one).
    struct WindowServices {
        /// Client-area size; nullopt when @p hwnd is not a window.
        std::function<std::optional<std::pair<u32, u32>>(u64 hwnd)>
            client_size;
        /// Resize the window so its CLIENT area is width x height, as
        /// SetWindowPos would (IDXGISwapChain::ResizeTarget).
        std::function<bool(u64 hwnd, u32 width, u32 height)> resize_client;
        /// Window rectangle in desktop coordinates: left, top, right,
        /// bottom (IDXGISwapChain::GetContainingOutput).
        std::function<std::optional<std::array<i32, 4>>(u64 hwnd)>
            window_rect;
        /// Exclusive full-screen, the display-mode owner DirectDraw and D3D9
        /// share (User32ExtManager::enter_exclusive_display): with
        /// @p change_mode publish width x height as the display mode and
        /// size the window to it; otherwise the window covers @p output
        /// (left, top, right, bottom) in the desktop mode.
        std::function<bool(u64 hwnd, std::array<i32, 4> output, u32 width,
                           u32 height, bool change_mode)>
            enter_exclusive;
        /// Leave it: restore the desktop mode when @p mode_changed, then the
        /// window rectangle when @p restore is set.
        std::function<void(u64 hwnd, bool mode_changed,
                           std::optional<std::array<i32, 4>> restore)>
            leave_exclusive;
        /// Host presentation of exclusive mode: a macOS Space
        /// (CocoaBridge::set_window_fullscreen).
        std::function<void(u64 hwnd, bool enable)> host_fullscreen;
        /// Whether the guest application is the active (foreground) one.
        std::function<bool()> application_active;
        /// Whether the host reports the window entirely occluded (a Space
        /// the user left, miniaturized). Unset: no host visibility signal.
        std::function<bool(u64 hwnd)> window_occluded;
        /// Whether @p hwnd or one of its descendants has keyboard focus
        /// (GetFocus) in the active application.
        std::function<bool(u64 hwnd)> has_keyboard_focus;
    };
    void set_window_services(WindowServices services);

    /// Kernel objects DXGI hands to the guest: the frame-latency waitable
    /// object is a semaphore, and each GetFrameLatencyWaitableObject call
    /// returns a new handle to it that the caller closes.
    struct SyncServices {
        std::function<u64(i32 initial, i32 maximum)> create_semaphore;
        std::function<u64(u64 handle)> duplicate_handle;
        /// Release one count unless the semaphore already holds @p cap.
        std::function<void(u64 handle, i32 count, i32 cap)> release_semaphore;
        std::function<void(u64 handle)> close_handle;
    };
    void set_sync_services(SyncServices services);
    /// The process handle table (NT shared-resource handles live there).
    void set_handle_table(HandleTable* table) { m_handle_table = table; }
    /// `sync_interval` is the originating guest's Present SyncInterval; it is
    /// what decides whether this child's layer is display-synced.
    [[nodiscard]] bool present_shared_window_surface(
        u64 hwnd, u64 shared_handle, u32 width, u32 height,
        u32 sync_interval,
        u32 src_x = 0, u32 src_y = 0);

    void impl_D3D11CreateDevice(Marshaler& m);
    /// Learn D3D11On12CreateDevice (d3d11on12.h).
    void impl_D3D11On12CreateDevice(Marshaler& m);
    void impl_D3D11CreateDeviceAndSwapChain(Marshaler& m);
    void impl_CreateSwapChain(Marshaler& m);
    /// Learn IDXGIFactory2::CreateSwapChainForHwnd — builds a swap chain from
    /// DXGI_SWAP_CHAIN_DESC1 (+ optional fullscreen desc) for an HWND.
    void impl_CreateSwapChainForHwnd(Marshaler& m);
    /// Learn IDXGIFactory2::CreateSwapChainForComposition — creates an
    /// initially unbound flip-model chain. DirectComposition binds it to an
    /// HWND when a visual tree containing the chain is committed.
    void impl_CreateSwapChainForComposition(Marshaler& m);

    [[nodiscard]] bool is_device_interface(GuestAddr address) const;
    [[nodiscard]] bool retain_device_reference(GuestAddr address);
    void release_device_interface_reference(GuestAddr address);
    [[nodiscard]] bool is_swap_chain_object(GuestAddr address) const;
    [[nodiscard]] bool retain_swap_chain_reference(GuestAddr address);
    void release_swap_chain_reference(GuestAddr address);
    [[nodiscard]] bool bind_composition_swap_chain(GuestAddr address,
        GuestAddr hwnd,
        const graphics::BackendCompositionLayerDesc& layer);
    [[nodiscard]] bool refresh_window_presentation(GuestAddr hwnd);
    void impl_DeviceMethod(Marshaler& m, usize slot);
    void impl_ContextMethod(Marshaler& m, usize slot);
    void impl_DxgiDeviceMethod(Marshaler& m, usize slot);
    void impl_BufferMethod(Marshaler& m, usize slot);
    void impl_Texture2DMethod(Marshaler& m, usize slot);
    void impl_DxgiResourceMethod(Marshaler& m, usize slot);
    void impl_KeyedMutexMethod(Marshaler& m, usize slot);
    void impl_ShaderResourceViewMethod(Marshaler& m, usize slot);
    void impl_UnorderedAccessViewMethod(Marshaler& m, usize slot);
    void impl_RenderTargetViewMethod(Marshaler& m, usize slot);
    void impl_DepthStencilViewMethod(Marshaler& m, usize slot);
    void impl_QueryMethod(Marshaler& m, usize slot);
    void impl_FenceMethod(Marshaler& m, usize slot);
    void impl_CommandListMethod(Marshaler& m, usize slot);
    /// Release every reference @p context's binding state holds and reset it,
    /// preserving the context's pipeline cache. Shared by ClearState,
    /// FinishCommandList and ExecuteCommandList.
    void clear_context_state(GuestAddr context);
    /// Recycle @p address and release anything retained against it. Every
    /// release_* path funnels here so none of them can forget the second half.
    void free_object(GuestAddr address);
    /// True when @p address is still the identity of a live device child.
    /// A recycled slot must not be handed out again until that child is gone.
    [[nodiscard]] bool object_slot_live(GuestAddr address) const;
    /// The generation impl_ContextMethod's context-record memo is checked
    /// against (m_context_memo); tests watch it move at every invalidation.
    [[nodiscard]] u64 context_lookup_generation() const noexcept {
        return m_context_lookup_generation;
    }
    void impl_SwapChainMethod(Marshaler& m, usize slot);
    void impl_SwapChainMediaMethod(Marshaler& m, usize slot);
    void impl_AnnotationMethod(Marshaler& m, usize slot);
    void impl_VertexShaderMethod(Marshaler& m, usize slot);
    void impl_PixelShaderMethod(Marshaler& m, usize slot);
    void impl_ComputeShaderMethod(Marshaler& m, usize slot);
    void impl_GeometryShaderMethod(Marshaler& m, usize slot);
    void impl_HullShaderMethod(Marshaler& m, usize slot);
    void impl_DomainShaderMethod(Marshaler& m, usize slot);
    void impl_DxgiSurfaceMethod(Marshaler& m, usize slot);
    void impl_DeviceContextStateMethod(Marshaler& m, usize slot);
    void impl_InputLayoutMethod(Marshaler& m, usize slot);
    void impl_BlendStateMethod(Marshaler& m, usize slot);
    void impl_RasterizerStateMethod(Marshaler& m, usize slot);
    void impl_DepthStencilStateMethod(Marshaler& m, usize slot);
    void impl_SamplerStateMethod(Marshaler& m, usize slot);
    void impl_VideoContextMethod(Marshaler& m, usize slot);
    void impl_MultithreadMethod(Marshaler& m, usize slot);
    void impl_ClassLinkageMethod(Marshaler& m, usize slot);
    void impl_ClassInstanceMethod(Marshaler& m, usize slot);

    // -- Direct3D 10 / 10.1 (winapi/graphics/d3d10/) ------------------------
    // d3d10.dll and d3d10_1.dll: a D3D10 device is a D3D11 device whose
    // immediate context runs a D3D10 context state, and every D3D10 object
    // is a facet of the D3D11 object (measured: each answers both sets of
    // interfaces, one COM identity) -- see d3d10_device.cpp.
    void impl_D3D10CreateDevice(Marshaler& m);
    void impl_D3D10CreateDevice1(Marshaler& m);
    void impl_D3D10CreateDeviceAndSwapChain(Marshaler& m);
    void impl_D3D10CreateDeviceAndSwapChain1(Marshaler& m);
    void impl_D3D10CreateBlob(Marshaler& m);
    void impl_D3D10CreateStateBlock(Marshaler& m);
    /// D3D10GetInputSignatureBlob (ISGN), D3D10GetOutputSignatureBlob
    /// (OSGN), D3D10GetInputAndOutputSignatureBlob (both).
    void impl_D3D10GetSignatureBlob(Marshaler& m, bool input, bool output);
    /// D3D10Get{Vertex,Geometry,Pixel}ShaderProfile(pDevice).
    void impl_D3D10GetShaderProfile(Marshaler& m, char stage);
    void impl_D3D10DeviceMethod(Marshaler& m, usize slot);
    void impl_D3D10BufferMethod(Marshaler& m, usize slot);
    void impl_D3D10TextureMethod(Marshaler& m, usize slot);
    void impl_D3D10ShaderResourceViewMethod(Marshaler& m, usize slot);
    void impl_D3D10RenderTargetViewMethod(Marshaler& m, usize slot);
    void impl_D3D10DepthStencilViewMethod(Marshaler& m, usize slot);
    void impl_D3D10BlendStateMethod(Marshaler& m, usize slot);
    void impl_D3D10DepthStencilStateMethod(Marshaler& m, usize slot);
    void impl_D3D10RasterizerStateMethod(Marshaler& m, usize slot);
    void impl_D3D10SamplerStateMethod(Marshaler& m, usize slot);
    void impl_D3D10DeviceChildMethod(Marshaler& m, usize slot);
    void impl_D3D10QueryMethod(Marshaler& m, usize slot);
    void impl_D3D10BlobMethod(Marshaler& m, usize slot);
    void impl_D3D10StateBlockMethod(Marshaler& m, usize slot);

#include "winapi/graphics/d3d11/d3d11_manager_resource_records.inc"
#include "winapi/graphics/d3d11/d3d11_manager_view_records.inc"
#include "winapi/graphics/d3d11/d3d11_manager_shader_records.inc"
#include "winapi/graphics/d3d11/d3d11_manager_state_records.inc"
#include "winapi/graphics/d3d11/d3d11_manager_command_records.inc"
#include "winapi/graphics/d3d11/d3d11_manager_swap_chain_records.inc"
#include "winapi/graphics/d3d11/d3d11_manager_object_creation.inc"
#include "winapi/graphics/d3d11/d3d11_manager_swap_chain_sharing.inc"
#include "winapi/graphics/d3d11/d3d11_manager_object_lifetime.inc"
#include "winapi/graphics/d3d11/d3d11_manager_present.inc"
#include "winapi/graphics/d3d11/d3d11_manager_shader_state_creation.inc"
#include "winapi/graphics/d3d11/d3d11_manager_binding_resolution.inc"
#include "winapi/graphics/d3d11/d3d11_manager_draw_emulation.inc"
#include "winapi/graphics/d3d11/d3d11_manager_context_ops.inc"
#include "winapi/graphics/d3d11/d3d11_manager_statistics_stream_out.inc"
#include "winapi/graphics/d3d11/d3d11_manager_members.inc"
#include "winapi/graphics/d3d11/d3d11_manager_d3d10.inc"
};

[[nodiscard]] DllExports create_d3d11_thunks(D3D11Manager& manager);

[[nodiscard]] std::string d3d11_device_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_context_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_dxgi_device_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_buffer_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_texture_2d_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_dxgi_resource_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_keyed_mutex_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_shader_resource_view_method_export_name(
    usize slot);
[[nodiscard]] std::string d3d11_unordered_access_view_method_export_name(
    usize slot);
[[nodiscard]] std::string d3d11_render_target_view_method_export_name(
    usize slot);
[[nodiscard]] std::string d3d11_depth_stencil_view_method_export_name(
    usize slot);
[[nodiscard]] std::string d3d11_query_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_fence_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_command_list_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_swap_chain_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_swap_chain_media_method_export_name(
    usize slot);
[[nodiscard]] std::string d3d11_annotation_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_vertex_shader_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_pixel_shader_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_compute_shader_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_geometry_shader_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_hull_shader_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_domain_shader_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_dxgi_surface_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_device_context_state_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_input_layout_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_blend_state_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_rasterizer_state_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_depth_stencil_state_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_sampler_state_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_video_context_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_multithread_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_class_linkage_method_export_name(usize slot);
[[nodiscard]] std::string d3d11_class_instance_method_export_name(usize slot);

/// The D3D10 facet vtables' hidden exports (registered with d3d11.dll's
/// table, whose device lock they take): "_D3D10_<interface>_<slot>".
[[nodiscard]] std::string d3d10_method_export_name(const char* interface_name,
                                                   usize slot);
/// The D3D10 facet tables, in the order create_d3d11_thunks registers them:
/// interface name, slot count, stdcall table, dispatcher, and the
/// MethodThunks member the resolved addresses go to.
struct D3D10VtableSpec {
    const char* interface_name;
    usize slots;
    const u16* stdcall_bytes;
    void (D3D11Manager::*method)(Marshaler&, usize);
    GuestAddr* (*thunks)(D3D11Manager::MethodThunks&);
};
[[nodiscard]] std::span<const D3D10VtableSpec> d3d10_vtable_specs();
/// d3d10.dll, d3d10_1.dll, d3d10core.dll and d3d10_1core.dll: their exact
/// export names and ordinals (dumpbin on Windows; d3d10_contract_probe
/// [exp]).
[[nodiscard]] DllExports create_d3d10_thunks(D3D11Manager& manager);
[[nodiscard]] DllExports create_d3d10_1_thunks(D3D11Manager& manager);
[[nodiscard]] DllExports create_d3d10core_thunks(bool d3d10_1);

/// Translate a documented D3D11_BLEND value into the shared backend factor.
/// Kept observable for focused contract tests.
[[nodiscard]] graphics::BackendBlendFactor d3d11_backend_blend_factor(
    u32 value);

} // namespace vitreo::winapi
