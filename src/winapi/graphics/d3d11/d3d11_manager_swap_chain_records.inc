// SPDX-License-Identifier: LGPL-2.1-only
// @file d3d11_manager_swap_chain_records.inc
// @brief D3D11Manager class body, part 6 of 17: swap-chain records and the
//   interface map (SwapChainDesc, SwapChain, ExternalWindowSurface,
//   InterfaceType, Interface).
//
// A fragment of the D3D11Manager definition in d3d11.hpp, which
// includes it inside the class, in this order; not a standalone header.

    struct SwapChainDesc {
        u32 width{0};
        u32 height{0};
        u32 refresh_numerator{0};
        u32 refresh_denominator{0};
        u32 format{0};
        u32 scanline_ordering{0};
        u32 scaling{0};
        u32 sample_count{1};
        u32 sample_quality{0};
        u32 buffer_usage{0};
        u32 buffer_count{0};
        GuestAddr output_window{0};
        u32 windowed{1};
        u32 swap_effect{0};
        u32 flags{0};
        /// DXGI_ALPHA_MODE from DXGI_SWAP_CHAIN_DESC1. Premultiplied/straight
        /// composition surfaces must not be composited onto an opaque host.
        /// A legacy DXGI_SWAP_CHAIN_DESC chain reports IGNORE through
        /// GetDesc1 (measured, dxgi_a11_contracts_probe).
        u32 alpha_mode{0};
        /// DXGI_SWAP_CHAIN_DESC1::Scaling (DXGI_SCALING), distinct from the
        /// mode scaling above (DXGI_MODE_SCALING of the legacy / fullscreen
        /// description). Legacy chains report STRETCH.
        u32 desc1_scaling{0};
    };

    struct SwapChain {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        GuestAddr back_buffer{0};
        // Flip-model swap chains expose one stable guest Texture2D interface
        // while DXGI rotates the physical buffers behind that interface.
        // The current physical buffer lives on Texture2D::backend; these are
        // the remaining buffers advertised by BufferCount.
        std::vector<std::unique_ptr<graphics::BackendTexture2D>>
            flip_back_buffers;
        usize next_flip_back_buffer{0};
        u32 ref_count{1};
        u32 present_count{0};
        u32 pending_sync_interval{0};
        u32 pending_present_flags{0};
        /// IDXGISwapChain::GetFullscreenState: the chain owns the output in
        /// exclusive full-screen.
        bool fullscreen{false};
        /// While fullscreen: the IDXGIOutput GetFullscreenState returns
        /// (held, one reference), the mode the output is in, and whether that
        /// mode was switched to (DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH) or
        /// is the desktop's.
        GuestAddr fullscreen_output{0};
        u32 mode_width{0};
        u32 mode_height{0};
        bool mode_changed{false};
        /// The window rectangle before full-screen, restored on leaving.
        std::optional<std::array<i32, 4>> windowed_rect;
        /// The host window is in a macOS Space for this chain. Losing
        /// application focus relinquishes DXGI full-screen but keeps the
        /// Space (Cmd-Tab leaves a Space like any Mac full-screen app); an
        /// explicit SetFullscreenState(FALSE) or Release leaves it.
        bool host_space{false};
        /// Full-screen was relinquished because the application lost focus:
        /// Present reports DXGI_STATUS_OCCLUDED until focus returns.
        bool occluded_by_focus_loss{false};
        bool composition{false};
        /// The factory that created the chain (IDXGISwapChain::GetParent),
        /// held as a reference for the chain's lifetime.
        GuestAddr factory{0};
        /// CreateSwapChainForHwnd's pRestrictToOutput, held as a reference.
        GuestAddr restrict_output{0};
        /// The HWND named at creation is not a window: DXGI still creates the
        /// chain (measured), and every Present reports DXGI_STATUS_OCCLUDED
        /// because nothing can be shown.
        bool window_missing{false};
        /// A ResizeBuffers that passed validation but failed to create the
        /// new buffers leaves the chain WITHOUT buffers, its description
        /// already updated: GetBuffer and Present fail with INVALID_CALL
        /// until a ResizeBuffers succeeds (measured, the rules probe).
        bool buffers_lost{false};
        /// IDXGISwapChain1::Set/GetBackgroundColor; opaque black by default
        /// (measured).
        std::array<float, 4> background_color{0.0f, 0.0f, 0.0f, 1.0f};
        /// DXGI_MODE_ROTATION of a flip-model chain; IDENTITY by default.
        u32 rotation{1};
        /// IDXGISwapChain2::SetSourceSize; the whole buffer until set, and
        /// reset to it by ResizeBuffers (measured).
        u32 source_width{0};
        u32 source_height{0};
        /// FRAME_LATENCY_WAITABLE_OBJECT chains: the latency (1 by default)
        /// and the semaphore GetFrameLatencyWaitableObject hands duplicates
        /// of. Created on first use.
        u32 maximum_frame_latency{1};
        /// Shared with the GPU-completion callbacks that release it, which
        /// can outlive the chain: `alive` is cleared (and the handle closed)
        /// under `mutex` when the chain dies.
        struct LatencyObject {
            std::mutex mutex;
            u64 handle{0};
            u32 cap{1};
            bool alive{true};
        };
        std::shared_ptr<LatencyObject> latency;
        /// Single-sample target a multisampled (bitblt) back buffer is
        /// resolved into at Present.
        std::unique_ptr<graphics::BackendTexture2D> msaa_resolve;
        /// IDXGISwapChain3::SetColorSpace1.
        u32 color_space{0};
        /// IDXGISwapChain4::SetHDRMetaData (type + the HDR10 bytes).
        u32 hdr_metadata_type{0};
        std::vector<u8> hdr_metadata;
        /// IDXGISwapChain2::SetMatrixTransform (composition chains only).
        std::array<float, 6> matrix_transform{1.0f, 0.0f, 0.0f,
                                              1.0f, 0.0f, 0.0f};
        /// IDXGISwapChain3::GetCurrentBackBufferIndex.
        u32 current_back_buffer_index{0};
        /// Virtual QPC tick of the last counted Present (frame statistics).
        u64 last_present_qpc{0};
        /// GetBuffer(n) textures for n > 0 on the sequential effects (index
        /// n - 1). Each is a read-only view of that buffer of the ring.
        std::vector<GuestAddr> index_buffers;
        /// The DirectComposition visual state last bound, re-applied with
        /// the chain's own matrix transform.
        std::shared_ptr<graphics::BackendCompositionLayerDesc>
            composition_layer;

        bool pending_present{false};
        /// The clip of the DirectComposition visual this chain was bound
        /// to (identity transform only), kept so a present can carry it
        /// across the process boundary. left/top/right/bottom in chain pixels.
        struct CompositionClip {
            bool valid{false};
            float left{0}, top{0}, right{0}, bottom{0};
        } composition_clip;
        SwapChainDesc desc{};
        std::unique_ptr<graphics::BackendSwapChain> backend;
        PrivateDataStore private_data;
    };

    struct ExternalWindowSurface {
        u32 width{0};
        u32 height{0};
        void* native_view{nullptr};
        // Flip-model swap chains rotate through multiple shared back buffers.
        // Keep the CAMetalLayer stable for the HWND and cache each imported
        // IOSurface by handle instead of rebuilding the layer every frame.
        std::unordered_map<
            u64, std::unique_ptr<graphics::BackendTexture2D>> textures;
        std::unique_ptr<graphics::BackendSwapChain> swap_chain;
    };

    enum class InterfaceType {
        Device,
        /// The ID3D10Device1 facet: counted with, and QueryInterface'd as,
        /// the device.
        D3D10Device,
        Context,
        DxgiDevice,
        Annotation,
        VideoContext,
        /// The ID3D11Multithread facets: counted with, and QueryInterface'd
        /// as, the device and the immediate context respectively.
        DeviceMultithread,
        ContextMultithread,
    };

    struct Interface {
        GuestAddr owner{0};
        InterfaceType type{InterfaceType::Device};
    };
