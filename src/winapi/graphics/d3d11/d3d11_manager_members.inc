// SPDX-License-Identifier: LGPL-2.1-only
// @file d3d11_manager_members.inc
// @brief D3D11Manager class body, part 16 of 17: the main data members,
//   m_memory through m_arena_pool (declaration order is the constructor's
//   initialisation order).
//
// A fragment of the D3D11Manager definition in d3d11.hpp, which
// includes it inside the class, in this order; not a standalone header.

    GuestMemory& m_memory;
    bool m_is_64bit;
    graphics::GraphicsBackendDevice& m_backend;
    /// gs-ring: the geometry emulation's per-draw buffers
    /// (d3d11_transient_ring.hpp), under the device lock like every draw.
    TransientBufferRing m_transient{m_backend};
    DxgiManager& m_dxgi;
    MethodThunks m_method_thunks{};
    GuestAddr m_region_base{0};
    /// First address past the object arena currently being bump-allocated.
    /// Object storage grows in chunks, so this is not m_region_base +
    /// STORAGE_SIZE once the first chunk fills.
    GuestAddr m_region_end{0};
    bool m_storage_ready{false};
    GuestAddr m_device_vtable{0};
    GuestAddr m_context_vtable{0};
    GuestAddr m_dxgi_device_vtable{0};
    GuestAddr m_buffer_vtable{0};
    GuestAddr m_texture_2d_vtable{0};
    GuestAddr m_dxgi_resource_vtable{0};
    GuestAddr m_keyed_mutex_vtable{0};
    GuestAddr m_shader_resource_view_vtable{0};
    GuestAddr m_unordered_access_view_vtable{0};
    GuestAddr m_render_target_view_vtable{0};
    GuestAddr m_depth_stencil_view_vtable{0};
    GuestAddr m_query_vtable{0};
    GuestAddr m_fence_vtable{0};
    GuestAddr m_command_list_vtable{0};
    GuestAddr m_deferred_context_vtable{0};
    GuestAddr m_swap_chain_vtable{0};
    GuestAddr m_annotation_vtable{0};
    GuestAddr m_swap_chain_media_vtable{0};
    GuestAddr m_vertex_shader_vtable{0};
    GuestAddr m_pixel_shader_vtable{0};
    GuestAddr m_compute_shader_vtable{0};
    GuestAddr m_geometry_shader_vtable{0};
    GuestAddr m_hull_shader_vtable{0};
    GuestAddr m_domain_shader_vtable{0};
    GuestAddr m_dxgi_surface_vtable{0};
    GuestAddr m_device_context_state_vtable{0};
    GuestAddr m_input_layout_vtable{0};
    GuestAddr m_blend_state_vtable{0};
    GuestAddr m_rasterizer_state_vtable{0};
    GuestAddr m_depth_stencil_state_vtable{0};
    GuestAddr m_sampler_state_vtable{0};
    GuestAddr m_video_context_vtable{0};
    GuestAddr m_next_object{0};
    std::vector<GuestAddr> m_free_objects;
    /// Exactly the addresses in m_free_objects, so free_object's "already
    /// free" check is O(1). A linear std::find over the list was ~1% of UE's
    /// RHI thread: it releases thousands of transient buffers a second.
    std::unordered_set<GuestAddr> m_free_object_set;
    /// Interfaces retained by SetPrivateDataInterface, keyed by the owning
    /// interface address and then by GUID.
    ///
    /// Deliberately here rather than inside each record's PrivateDataStore.
    /// There are eighteen release_* paths, and one that forgot to release would
    /// leak a guest reference silently -- no crash, no failing test, just an
    /// object the guest can never free. One map lets a single helper, called
    /// from the one free_object hook, cover every record type.
    bool m_shutting_down{false};
    std::unordered_map<GuestAddr,
                       std::unordered_map<GuidBytes, PrivateInterface, GuidHash>>
        m_private_data_interfaces;
    GuestFunctionCaller m_guest_call;
    std::function<bool(GuestAddr)> m_is_d3d12_device;
    GuestAddr m_next_map_hint{0x64000000ull};
    PresentationViewResolver m_presentation_view_resolver;
    PresentationViewResolver m_foreign_surface_view_resolver;
    ForeignWindowResolver m_foreign_window_resolver;
    WindowPixelsPresenter m_window_pixels_presenter;
    WindowSurfacePresenter m_window_surface_presenter;
    WindowServices m_window_services;
    /// The chain that owns exclusive full-screen (one per process: the
    /// logical display mode is the process's single guest desktop).
    GuestAddr m_exclusive_chain{0};
    /// A full-screen / windowed transition is sending its window messages:
    /// a SetFullscreenState or ResizeTarget re-entered from them reports
    /// DXGI_STATUS_MODE_CHANGE_IN_PROGRESS.
    std::atomic<bool> m_fullscreen_transition{false};
    SyncServices m_sync_services;
    struct PendingForeignSurface {
        u64 hwnd{0};
        u64 shared_handle{0};
        u32 width{0};
        u32 height{0};
        u32 src_x{0};
        u32 src_y{0};
        /// The originating guest's Present SyncInterval, carried to the owner.
        u32 sync_interval{1};
        std::shared_ptr<graphics::GpuCompletion> completion;
    };
    std::mutex m_foreign_present_mutex;
    /// Learn ("Introduction to a device in Direct3D 11", "Threading
    /// Differences between Direct3D Versions"): ID3D11Device methods are
    /// free-threaded, and the runtime serializes them -- and the immediate
    /// context -- with its own lock unless the app creates the device with
    /// D3D11_CREATE_DEVICE_SINGLETHREADED. Every guest entry into this manager
    /// holds this lock (create_d3d11_thunks). UE creates textures on worker
    /// threads while its render thread drives the context; without the lock
    /// a CreateTexture2D rehashing m_textures under a concurrent lookup
    /// crashed the host. Recursive because methods dispatch to each other.
    // DXGI calls this frontend, and this frontend retains DXGI objects.
    // Sharing the owner lock avoids a DXGI <-> D3D11 lock inversion.
    std::recursive_mutex& m_api_mutex;
    /// Set by a guest Draw* entry just before it calls execute_draw, which
    /// takes it at once: that outermost draw alone may give the lock up
    /// around a pipeline compile (draw_compile_may_release_lock). Nested
    /// emulation draws find it clear.
    bool m_draw_may_release_lock{false};
    std::condition_variable m_foreign_present_cv;
    std::unordered_map<u64, PendingForeignSurface> m_pending_foreign_surfaces;
    std::unordered_set<u64> m_foreign_surfaces_in_flight;
    std::jthread m_foreign_present_worker;
    std::unordered_map<u64, ExternalWindowSurface> m_external_surfaces;
    std::unordered_map<GuestAddr, Device> m_devices;
    /// Backend answers format_caps_context() reads; queried once.
    mutable std::optional<d3d11::FormatCapsContext> m_format_caps_base;
    mutable std::once_flag m_format_caps_once;
    std::unordered_map<GuestAddr, Interface> m_interfaces;
    std::unordered_map<GuestAddr, Buffer> m_buffers;
    /// Diagnostic only: what the last buffer destroyed at each object slot
    /// was, and who dropped its final reference (guest return address; 0 is
    /// an internal release such as an unbind). Object slots are reused LIFO,
    /// so a guest that still holds a destroyed buffer names the NEXT object
    /// created there; a rejection can then report what the guest meant.
    struct BufferTombstone {
        BufferDesc desc{};
        GuestAddr final_release_caller{0};
        GuestAddr create_caller{0};
    };
    std::unordered_map<GuestAddr, BufferTombstone> m_buffer_tombstones;
    std::unordered_map<GuestAddr, GuestAddr> m_buffer_create_callers;
    /// Guest caller of the ID3D11Buffer::Release being executed, else 0.
    GuestAddr m_releasing_caller{0};
    std::unordered_map<GuestAddr, Texture2D> m_textures;
    std::unordered_map<GuestAddr, ShaderResourceView>
        m_shader_resource_views;
    std::unordered_map<GuestAddr, UnorderedAccessView>
        m_unordered_access_views;
    std::unordered_map<GuestAddr, RenderTargetView> m_render_target_views;
    std::unordered_map<GuestAddr, DepthStencilView> m_depth_stencil_views;
    std::unordered_map<GuestAddr, Query> m_queries;
    std::unordered_map<GuestAddr, Fence> m_fences;
    std::unordered_map<GuestAddr, CommandList> m_command_lists;
    std::unordered_map<GuestAddr, ClassLinkage> m_class_linkages;
    std::unordered_map<GuestAddr, ClassInstance> m_class_instances;
    GuestAddr m_class_linkage_vtable{0};
    GuestAddr m_class_instance_vtable{0};
    GuestAddr m_multithread_vtable{0};
    std::unordered_map<GuestAddr, SwapChain> m_swap_chains;
    std::unordered_map<GuestAddr, VertexShader> m_vertex_shaders;
    std::unordered_map<const std::string*, u64> m_msl_hashes;
    std::unordered_map<GuestAddr, PixelShader> m_pixel_shaders;
    std::unordered_map<GuestAddr, ComputeShader> m_compute_shaders;
    /// The compute shader whose Dispatch is binding its stage right now (0 for
    /// the GS/HS lowerings that share bind_compute_stage). Diagnostic only:
    /// names a volume UAV's writer for the render-target dump.
    GuestAddr m_diag_binding_shader{0};
    std::unordered_map<GuestAddr, GeometryShader> m_geometry_shaders;
    std::unordered_map<GuestAddr, HullShader> m_hull_shaders;
    std::unordered_map<GuestAddr, DomainShader> m_domain_shaders;
    u32 m_device_removed_cookie{0};
    /// ID3D11Device3::CreateTexture2D1 / CreateTexture3D1 hand their
    /// D3D11_TEXTURE_LAYOUT to the base creators' body (device methods 5 and
    /// 6) through this, which the body takes (and resets) on entry.
    u32 m_pending_texture_layout{0};
    /// ID3DDeviceContextState objects, and which one each context's live
    /// bindings belong to (0 until the first SwapDeviceContextState).
    struct DeviceContextStateObject {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        u32 feature_level{0};
        /// EmulatedInterface was ID3D10Device or ID3D10Device1.
        bool d3d10{false};
        /// The bindings this state holds while it is NOT the context's.
        ContextState stored{};
        PrivateDataStore private_data;
    };
    std::unordered_map<GuestAddr, DeviceContextStateObject>
        m_device_context_states;
    std::unordered_map<GuestAddr, GuestAddr> m_active_context_state;
    /// Stream-out machinery, created on first use.
    std::unique_ptr<graphics::BackendTexture2D> m_stream_out_target;
    /// Zeros bound to an input slot the input layout reads but the guest left
    /// unbound: D3D11.3 8.19.2 treats it as a buffer of size 0, every fetch
    /// out of bounds, which returns 0 (plus the missing-component defaults).
    std::unique_ptr<graphics::BackendBuffer> m_zero_vertex_buffer;
    std::unique_ptr<graphics::BackendComputePipeline> m_stream_out_repack;
    std::unique_ptr<graphics::BackendComputePipeline> m_stream_out_prepare;
    std::unique_ptr<graphics::BackendComputePipeline> m_stream_out_compact;
    std::unique_ptr<graphics::BackendComputePipeline> m_stream_out_drawauto;
    /// Stats record for streaming draws no query is watching.
    std::shared_ptr<graphics::BackendBuffer> m_stream_out_scratch_stats;
    /// Indirect draws through the emulations: the resolve kernel, the
    /// capture capacity (records) it is sized for -- raised when a draw
    /// overflowed it -- and the resolves whose overflow flag is unread.
    std::unique_ptr<graphics::BackendComputePipeline> m_ia_resolve;
    /// PIPELINE_STATISTICS kernels, by entry point (pipeline_stats.hpp).
    std::map<std::string, std::unique_ptr<graphics::BackendComputePipeline>>
        m_stats_kernels;
    /// The software tessellator's passes.
    std::unique_ptr<graphics::BackendComputePipeline> m_tess_patch;
    std::unique_ptr<graphics::BackendComputePipeline> m_tess_scan;
    /// Its batch scratch, which only the GPU writes: kept between draws.
    struct TessScratch {
        std::shared_ptr<graphics::BackendBuffer> counts;
        std::shared_ptr<graphics::BackendBuffer> points;
        std::shared_ptr<graphics::BackendBuffer> indices;
        std::shared_ptr<graphics::BackendBuffer> records;
    };
    TessScratch m_tess_scratch;
    u32 m_indirect_record_capacity{1u << 14};
    struct IndirectFeedback {
        std::shared_ptr<graphics::BackendBuffer> resolved;
        u64 generation{0};
        u32 record_capacity{0};
    };
    std::vector<IndirectFeedback> m_indirect_feedback;
    std::unordered_map<GuestAddr, InputLayout> m_input_layouts;
    std::unordered_map<GuestAddr, BlendState> m_blend_states;
    std::unordered_map<GuestAddr, RasterizerState> m_rasterizer_states;
    std::unordered_map<GuestAddr, DepthStencilState> m_depth_stencil_states;
    std::unordered_map<GuestAddr, SamplerState> m_sampler_states;
    /// Lookup memos for the per-draw binding walk: execute_draw resolves every
    /// bound s# and t# several times (masks, push words, backend bindings), a
    /// hash-map find each. Direct-mapped by address. An entry is valid while
    /// its map's erase generation is unchanged: unordered_map nodes never
    /// move, so a value pointer lives until that element is erased. Guarded,
    /// like the maps, by the device lock.
    struct SamplerMemoEntry {
        GuestAddr address{0};
        u64 generation{0};
        const SamplerState* state{nullptr};
    };
    struct ShaderResourceViewMemoEntry {
        GuestAddr address{0};
        u64 generation{0};
        const ShaderResourceView* view{nullptr};
    };
    static constexpr usize kLookupMemoEntries = 256;
    mutable std::array<SamplerMemoEntry, kLookupMemoEntries> m_sampler_memo{};
    mutable std::array<ShaderResourceViewMemoEntry, kLookupMemoEntries>
        m_srv_memo{};
    /// Bumped by every erase from m_sampler_states / m_shader_resource_views.
    /// Start at 1 so a default memo entry (generation 0) never matches.
    u64 m_sampler_erase_generation{1};
    /// Bumped by every insert into or erase from m_sampler_states; keys
    /// m_sampler_summary (an erased sampler's address can be reused).
    u64 m_sampler_set_generation{1};
    /// Draws and dispatches recorded on the immediate context since the last
    /// early submit (note_immediate_gpu_work).
    u32 m_work_since_submit{0};
    mutable std::array<SamplerStageSummary, 2> m_sampler_summary{};
    /// Bumped by every insert into or erase from m_shader_resource_views;
    /// keys m_srv_summary (a released view's address can be reused).
    u64 m_srv_set_generation{1};
    mutable std::array<SrvStageSummary, 2> m_srv_summary{};
    u64 m_srv_erase_generation{1};
    /// State-object deduplication: canonical description key (family,
    /// device, description bytes) -> the live object, and the number of
    /// unique objects each (device, family) holds.
    std::unordered_map<std::string, GuestAddr> m_state_pool;
    std::map<std::pair<GuestAddr, u8>, u32> m_state_pool_counts;
    std::unordered_map<GuestAddr, ContextState> m_context_states;
    /// Deferred contexts by interface address, and each one's annotation
    /// facet -> the context.
    std::unordered_map<GuestAddr, DeferredContext> m_deferred_contexts;
    std::unordered_map<GuestAddr, GuestAddr> m_deferred_annotations;
    /// Released 64 KiB recording chunks, reused before mapping new ones: a
    /// deferred context typically finishes a list every frame.
    std::vector<GuestAddr> m_arena_pool;
