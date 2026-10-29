// SPDX-License-Identifier: LGPL-2.1-only
// @file d3d11_manager_swap_chain_sharing.inc
// @brief D3D11Manager class body, part 8 of 17: swap-chain creation and sharing
//   (DXGI resource facets, keyed mutexes, shared resources, presentation
//   queries, find_device, add_ref).
//
// A fragment of the D3D11Manager definition in d3d11.hpp, which
// includes it inside the class, in this order; not a standalone header.

    /// Which factory entry point a swap chain comes from: the validation
    /// rules differ (DXGI_SWAP_CHAIN_DESC vs DESC1 fields, composition).
    enum class SwapChainApi { Legacy, Hwnd, Composition };
    /// Every creation rule DXGI applies to a description, as the HRESULT it
    /// returns (measured, dxgi_a11_contracts_probe / _swapchain_rules_probe).
    /// @p desc has its zero Width/Height already taken from the window.
    [[nodiscard]] u32 validate_swap_chain_desc(
        const SwapChainDesc& desc, SwapChainApi api, u32 stereo) const;
    /// Validate and create; returns S_OK or DXGI_STATUS_OCCLUDED with
    /// @p created set, else the failure HRESULT. @p factory and
    /// @p restrict_output are retained by the chain when non-zero.
    [[nodiscard]] u32 create_swap_chain(
        GuestAddr device_interface, SwapChainDesc desc, SwapChainApi api,
        u32 stereo, GuestAddr factory, GuestAddr restrict_output,
        GuestAddr& created);
    [[nodiscard]] bool create_swap_chain_backend(SwapChain& swap_chain);
    /// Hand the chain's colour space, background, rotation and HDR metadata
    /// to its (new) backend.
    void apply_swap_chain_presentation_state(SwapChain& chain);
    /// Allocate buffers 1..BufferCount-1 of a ring that rotates (the flip
    /// effects and SEQUENTIAL) plus the MSAA resolve target.
    [[nodiscard]] bool create_swap_chain_ring(SwapChain& swap_chain,
                                              Texture2D& back_buffer);
    /// The window's client size; nullopt when @p hwnd is not a window or no
    /// window services are wired.
    [[nodiscard]] std::optional<std::pair<u32, u32>> window_client_size(
        u64 hwnd) const;
    /// False only when the window services positively say @p hwnd is not a
    /// window.
    [[nodiscard]] bool window_exists(u64 hwnd) const;
    /// References to the back buffer(s) held OUTSIDE the chain: GetBuffer
    /// references and views with a public reference. Bindings in a device
    /// context do not count (measured).
    [[nodiscard]] bool swap_chain_buffers_referenced(const SwapChain& chain);
    /// Unbind every view of @p texture from @p context (all contexts when 0),
    /// render targets only when @p render_targets_only.
    void unbind_texture_views(GuestAddr texture, GuestAddr context,
                              bool render_targets_only);
    /// IDXGISwapChain::ResizeBuffers / ResizeBuffers1 body.
    [[nodiscard]] u32 resize_swap_chain_buffers(GuestAddr self, u32 count,
                                                u32 width, u32 height,
                                                u32 format, u32 flags);
    /// The offered flag of the resource behind an IDXGIResource pointer (a
    /// texture's or buffer's DXGI facet); null when it is not one.
    [[nodiscard]] bool* dxgi_resource_offered_flag(GuestAddr resource);
    /// IDXGIDevice::QueryResourceResidency's DXGI_RESIDENCY for the buffer or
    /// texture behind @p resource (the resource itself or any of its DXGI
    /// facets); nullopt when it is not one.
    [[nodiscard]] std::optional<u32> dxgi_resource_residency(
        GuestAddr resource) const;
    /// IDXGIResource methods on a buffer's facet.
    void buffer_dxgi_resource_method(Marshaler& m, usize slot,
                                     GuestAddr buffer_address);
    void buffer_keyed_mutex_method(Marshaler& m, usize slot,
                                   GuestAddr buffer_address);
    /// GetDesc1 of a view of a planar video texture: the PlaneSlice its
    /// format names, at @p offset_2d (TEXTURE2D) / @p offset_2d_array.
    void write_view_plane_slice(GuestAddr desc_out, GuestAddr texture,
                                u32 format, u32 view_dimension, bool uav,
                                u32 offset_2d, u32 offset_2d_array);
    /// IDXGIResource1::CreateSubresourceSurface objects: surface -> the
    /// texture and subresource it stands for. Each holds a texture reference.
    struct SubresourceSurface {
        GuestAddr texture{0};
        u32 subresource{0};
        u32 ref_count{1};
        PrivateDataStore private_data;
    };
    std::unordered_map<GuestAddr, SubresourceSurface> m_subresource_surfaces;
    /// Keyed-mutex state of this process's shared resources, by storage id,
    /// so every opening attaches the same state once.
    std::unordered_map<u64, std::weak_ptr<SharedKeyedMutex>>
        m_keyed_mutex_states;
    /// Attach a KEYEDMUTEX resource to its keyed-mutex state: shared by
    /// every opening of storage @p storage_id / @p nonce in every process
    /// (0: the resource has no shared storage -- a state only it reaches).
    void attach_keyed_mutex(std::shared_ptr<SharedKeyedMutex>& state,
                            u32& opening, u64 storage_id, u64 nonce);
    void attach_keyed_mutex(Texture2D& texture);
    /// IDXGIKeyedMutex::AcquireSync / ReleaseSync bodies.
    [[nodiscard]] u32 keyed_mutex_acquire(
        const std::shared_ptr<SharedKeyedMutex>& state, u32 opening, u64 key,
        u32 timeout_ms);
    [[nodiscard]] u32 keyed_mutex_release(
        const std::shared_ptr<SharedKeyedMutex>& state, u32 opening,
        u64 key);
    u32 m_next_keyed_mutex_opening{0};
    void subresource_surface_method(Marshaler& m, usize slot, GuestAddr self);
    /// The process handle table, where IDXGIResource1::CreateSharedHandle
    /// creates its NT handles (winapi/kernel32/shared_resource_handle.hpp).
    HandleTable* m_handle_table{nullptr};
    /// IDXGIResource::GetSharedHandle / IDXGIResource1::CreateSharedHandle
    /// bodies (d3d11_sharing.cpp).
    [[nodiscard]] u32 get_shared_handle(Marshaler& m, Texture2D& texture);
    [[nodiscard]] u32 get_shared_handle(Marshaler& m, Buffer& buffer);
    [[nodiscard]] u32 create_shared_handle(Marshaler& m, Texture2D& texture);
    /// The id another device or process opens @p texture by (its storage
    /// exported on first use); 0 when it has no shared storage.
    [[nodiscard]] u64 export_shared_texture(Texture2D& texture);
    /// D3D11_OPTIONS4.ExtendedNV12SharedTextureSupported at @p feature_level.
    [[nodiscard]] bool extended_nv12_sharing(u32 feature_level) const;
    /// Synchronization points of shared resources (D3D11 Flush, Present,
    /// ReleaseSync): publish what this process wrote to them.
    [[nodiscard]] std::shared_ptr<graphics::GpuCompletion>
    publish_shared_writes();
    /// A GPU copy between @p texture (a shared STAGING resource's memory)
    /// and a GPU texture location; false when the backend cannot.
    [[nodiscard]] bool copy_shared_staging(
        Texture2D& staging, u32 subresource, bool to_staging,
        const graphics::BackendTextureLocation& gpu, u32 x, u32 y, u32 z,
        u32 width, u32 height, u32 depth);
    /// Before the CPU reads or writes a shared STAGING resource (Map, CPU
    /// copies): its GPU copies and other devices' published writes are in.
    void acquire_shared_staging(Texture2D& texture);
    /// CopySubresourceRegion of a planar video format (NV12 / P010 / P016):
    /// the luma box and the chroma samples it covers. Null on success, else
    /// why the copy was dropped.
    [[nodiscard]] const char* copy_planar_region(
        Texture2D& destination, u32 dst_sub, u32 dst_x, u32 dst_y,
        Texture2D& source, u32 src_sub, u32 left, u32 top, u32 width,
        u32 height);
    /// Create the frontend object of a 3D texture over an opened (or new)
    /// backend: the tail of CreateTexture3D.
    [[nodiscard]] GuestAddr create_texture_3d_object(
        GuestAddr device_owner, const Texture3DDesc& desc,
        std::unique_ptr<graphics::BackendTexture3D> backend,
        std::shared_ptr<graphics::BackendSharedMemory> memory, u64 nonce,
        bool opening);
    /// ID3D11Device::OpenSharedResource / Device1::OpenSharedResource1 /
    /// OpenSharedResourceByName bodies.
    enum class SharedHandleKind : u8 { Kmt, Nt, Name };
    [[nodiscard]] u32 open_shared_resource(Marshaler& m, GuestAddr self,
                                           SharedHandleKind kind);
    /// Open the shared storage (@p surface_id, @p nonce; nonce 0 = any) as a
    /// new resource object of the device @p self answering @p iid.
    [[nodiscard]] u32 open_shared_storage(GuestAddr self, u32 surface_id,
                                          u64 nonce, GuestAddr iid_address,
                                          GuestAddr resource_out);
    /// The storage id a guest shared handle (KMT value or NT handle of this
    /// process) names, or 0.
    [[nodiscard]] u32 shared_storage_of_handle(u64 handle) const;
    /// What a shareable resource stores with its storage for every opener:
    /// its dimension, the creator's description and a nonce (Windows hands
    /// every opening the creator's description, misc flags included).
    struct SharedResourceDesc {
        /// D3D11_RESOURCE_DIMENSION: 1 buffer, 2 Texture1D, 3 Texture2D,
        /// 4 Texture3D.
        u32 dimension{3};
        Texture2DDesc texture{};
        u32 depth{1};
        BufferDesc buffer{};
        u64 nonce{0};
        /// The host process that created the resource
        /// (RESTRICT_SHARED_RESOURCE: only it opens the resource).
        u32 creator_pid{0};
    };
    [[nodiscard]] static std::vector<u8> shared_resource_metadata(
        const SharedResourceDesc& desc);
    [[nodiscard]] static bool parse_shared_resource_metadata(
        const std::vector<u8>& bytes, SharedResourceDesc& desc);
    [[nodiscard]] static std::vector<u8> shared_texture_metadata(
        const Texture2DDesc& desc, u64 nonce, u32 dimension = 3);
    [[nodiscard]] static bool parse_shared_texture_metadata(
        const std::vector<u8>& bytes, Texture2DDesc& desc, u64& nonce);
    [[nodiscard]] static u64 new_shared_nonce();
    /// Present1's DXGI_PRESENT_PARAMETERS rules (dirty / scroll rects).
    [[nodiscard]] u32 validate_present_parameters(const SwapChain& chain,
                                                  GuestAddr params) const;
    /// DXGI_FRAME_STATISTICS for @p chain into @p stats; returns the HRESULT
    /// GetFrameStatistics(Media) answers.
    [[nodiscard]] u32 write_frame_statistics(const SwapChain& chain,
                                             GuestAddr stats);
    /// The display refresh period in virtual QPC ticks.
    [[nodiscard]] u64 qpc_refresh_period() const;
    /// GetBuffer(n > 0): the chain-owned texture for buffer n, created on
    /// first use from the ring allocation.
    [[nodiscard]] GuestAddr swap_chain_index_buffer(GuestAddr self, u32 index);
    void release_swap_chain_index_buffers(SwapChain& chain);
    [[nodiscard]] bool swap_chain_color_space_supported(
        const SwapChain& chain, u32 color_space) const;
    [[nodiscard]] static graphics::BackendColorSpace
    swap_chain_backend_color_space(u32 color_space);
    /// Configure a bound composition chain's layer: the visual's transform
    /// combined with IDXGISwapChain2::SetMatrixTransform.
    [[nodiscard]] bool apply_composition_layer(SwapChain& chain);
    /// DXGI_SWAP_CHAIN_DESC1 into @p desc (Scaling to desc1_scaling).
    [[nodiscard]] bool read_swap_chain_desc1(GuestAddr address,
                                             SwapChainDesc& desc,
                                             u32& stereo) const;
    /// Release the frame-latency semaphore of a dead chain.
    void close_swap_chain_latency_object(SwapChain& swap_chain);
    /// Create the frame-latency semaphore on first use.
    [[nodiscard]] bool ensure_swap_chain_latency_object(SwapChain& swap_chain);
    [[nodiscard]] Device* find_device(GuestAddr interface_address,
                                      InterfaceType* type = nullptr);
    [[nodiscard]] u32 add_ref(GuestAddr interface_address);
