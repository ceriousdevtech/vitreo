// SPDX-License-Identifier: LGPL-2.1-only
// @file d3d11_manager_context_ops.inc
// @brief D3D11Manager class body, part 14 of 17: context operations (the
//   device/context extension methods and the context method families, Map,
//   tiled resources, context state, deferred contexts, resource facets,
//   queries).
//
// A fragment of the D3D11Manager definition in d3d11.hpp, which
// includes it inside the class, in this order; not a standalone header.

    /// Device and context methods beyond the core surface
    /// (d3d11_device_ext.cpp). Return false for a slot they do not serve.
    /// The texture halves of Map/Unmap, shared by the context and by
    /// IDXGISurface::Map/Unmap.
    /// @p want_pointer false: ID3D11DeviceContext::Map with a NULL
    /// pMappedResource, legal only on a DEFAULT texture with CPU access
    /// (map on default; the texture is then read and written through
    /// ID3D11Device3::ReadFromSubresource / WriteToSubresource).
    [[nodiscard]] u32 map_texture(GuestAddr resource, u32 subresource,
                                  u32 map_type, u32 map_flags,
                                  GuestAddr context_owner, GuestAddr& data_out,
                                  u32& row_pitch_out, u32& depth_pitch_out,
                                  bool want_pointer = true);
    void unmap_texture(GuestAddr resource, u32 subresource);

    using MappedImageLayout = D3D11MappedImageLayout;
    [[nodiscard]] MappedImageLayout mapped_image_layout(const Texture2D& texture,
                                                        u32 subresource) const;
    /// The subresource's shadow bytes into (store) or out of (load) a
    /// mapping image of @p layout.
    void store_mapped_image(const Texture2D& texture, u32 subresource,
                            const MappedImageLayout& layout, u8* image) const;
    void load_mapped_image(Texture2D& texture, u32 subresource,
                           const MappedImageLayout& layout, const u8* image);
    /// Map on default: the GPU texture's subresource into the shadow, after
    /// every command recorded before (read), and the shadow into the GPU
    /// texture, in order with them (write).
    [[nodiscard]] bool read_default_subresource(Texture2D& texture,
                                                u32 subresource);
    [[nodiscard]] bool write_default_subresource(Texture2D& texture,
                                                 u32 subresource);
    /// ID3D11Device3::WriteToSubresource (@p write) / ReadFromSubresource.
    void transfer_mapped_subresource(Marshaler& m, bool write);
    [[nodiscard]] u32 map_buffer(GuestAddr resource, u32 subresource,
                                 u32 map_type, u32 map_flags,
                                 GuestAddr context_owner, GuestAddr& data,
                                 u32& pitch);
    void unmap_buffer(GuestAddr resource, u32 subresource);
    [[nodiscard]] bool device_ext_method(Marshaler& m, usize slot,
                                         GuestAddr self);
    // ID3D11Device vtable slots by area; impl_DeviceMethod
    // (d3d11_device_methods.cpp) forwards to these once the device is found.
    /// CreateBuffer, CreateTexture1D/2D/3D (3-6): d3d11_device_resources.cpp.
    void device_resource_method(Marshaler& m, usize slot, GuestAddr self,
                                Device* device);
    /// CreateShaderResourceView (7): d3d11_device_srv.cpp.
    void device_srv_method(Marshaler& m, usize slot, GuestAddr self,
                           Device* device);
    /// CreateUnorderedAccessView (8): d3d11_device_uav.cpp.
    void device_uav_method(Marshaler& m, usize slot, GuestAddr self,
                           Device* device);
    /// CreateRenderTargetView, CreateDepthStencilView (9, 10):
    /// d3d11_device_rtv_dsv.cpp.
    void device_rtv_dsv_method(Marshaler& m, usize slot, GuestAddr self,
                               Device* device);
    /// CheckFormatSupport, CheckMultisampleQualityLevels, CheckFeatureSupport
    /// (29, 30, 33): d3d11_device_caps.cpp.
    void device_caps_method(Marshaler& m, usize slot, Device* device);
    /// CreateInputLayout, Create*Shader, CreateClassLinkage (11-19):
    /// d3d11_device_shaders.cpp.
    void device_shader_method(Marshaler& m, usize slot, GuestAddr self);
    /// CreateBlendState / DepthStencilState / RasterizerState(2) /
    /// SamplerState (20-23, 56): d3d11_device_states.cpp.
    void device_state_method(Marshaler& m, usize slot, GuestAddr self,
                             Device* device);
    [[nodiscard]] bool context_ext_method(Marshaler& m, usize slot,
                                          GuestAddr self);
    /// impl_ContextMethod's arms by family (d3d11_context_<family>.cpp);
    /// impl_ContextMethod routes exactly the slots each one's switch lists.
    void context_constant_buffer_method(Marshaler& m, usize slot,
                                        GuestAddr self);
    void context_shader_stage_method(Marshaler& m, usize slot, GuestAddr self);
    void context_fixed_stage_method(Marshaler& m, usize slot, GuestAddr self);
    void context_draw_method(Marshaler& m, usize slot, GuestAddr self);
    void context_copy_method(Marshaler& m, usize slot);
    void context_cpu_access_method(Marshaler& m, usize slot, GuestAddr self);
    void context_clear_method(Marshaler& m, usize slot);
    void context_getter_method(Marshaler& m, usize slot, GuestAddr self);

    // -- Tiled resources (d3d11_tiled.cpp) ---------------------------------
    /// D3D11_TILED_RESOURCES_TIER at @p feature_level: TIER_1 where the
    /// backend has tiled resources and the level is 11_0 or above (Learn,
    /// "Tier 1": "feature level 11.0 minimum"; measured WARP: 0 at 10_1).
    [[nodiscard]] u32 tiled_resources_tier(u32 feature_level) const;
    /// The tiles of a new tiled buffer / texture (null: cannot be laid out).
    [[nodiscard]] std::unique_ptr<TiledState> tiled_buffer_state(
        u32 byte_width) const;
    [[nodiscard]] std::unique_ptr<TiledState> tiled_texture_state(
        const Texture2DDesc& desc, graphics::BackendTexture2D* backend);
    /// ID3D11Device2::GetResourceTiling (device slot 52).
    void get_resource_tiling(Marshaler& m);
    /// ID3D11DeviceContext2 slots 134-139 on an immediate context (or a
    /// command list's replay).
    [[nodiscard]] u32 update_tile_mappings(Marshaler& m);
    [[nodiscard]] u32 copy_tile_mappings(Marshaler& m);
    void copy_tiles(Marshaler& m);
    void update_tiles(Marshaler& m);
    [[nodiscard]] u32 resize_tile_pool(Marshaler& m);
    void tiled_resource_barrier(Marshaler& m);
    /// A UINT64 argument starting at argument @p index (two stack slots on
    /// x86), and the index of the argument after it.
    [[nodiscard]] u64 arg_u64(Marshaler& m, usize index) const;
    [[nodiscard]] usize after_u64(usize index) const {
        return m_is_64bit ? index + 1 : index + 2;
    }
    void create_blend_state1(Marshaler& m, GuestAddr self);
    void create_device_context_state(Marshaler& m, GuestAddr self);
    void swap_device_context_state(Marshaler& m, GuestAddr self);
    [[nodiscard]] u32 release_device_context_state(GuestAddr address);
    /// Release every object a (detached) ContextState holds.
    void release_bindings(ContextState&& state);

    // -- Deferred contexts and command lists (d3d11_deferred.cpp) ----------
    /// ID3D11Device::CreateDeferredContext (and 1/2/3, same arguments).
    void create_deferred_context(Marshaler& m, GuestAddr self);
    /// The deferred context @p interface_address is, or whose annotation
    /// facet it is; 0 when it is neither.
    [[nodiscard]] GuestAddr deferred_context_of(
        GuestAddr interface_address) const;
    [[nodiscard]] bool is_deferred_context(GuestAddr context) const {
        return m_deferred_contexts.find(context) != m_deferred_contexts.end();
    }
    [[nodiscard]] u32 deferred_add_ref(GuestAddr context);
    [[nodiscard]] u32 deferred_release(GuestAddr context);
    [[nodiscard]] u32 deferred_query_interface(Marshaler& m,
                                               GuestAddr context);
    /// Every ID3D11DeviceContext call on a deferred context comes here first.
    /// True when the call was fully handled (recorded, refused, or answered);
    /// false lets impl_ContextMethod run it against the deferred context's
    /// own state as well -- the state setters, whose getters must see them.
    [[nodiscard]] bool deferred_context_method(Marshaler& m, usize slot,
                                               GuestAddr self);
    /// ID3D11DeviceContext::ExecuteCommandList on an immediate context.
    void execute_command_list(GuestAddr context, GuestAddr list,
                              bool restore_state);
    /// ID3D11DeviceContext::FinishCommandList on a deferred context.
    [[nodiscard]] u32 finish_command_list(Marshaler& m, GuestAddr self);
    /// Dispatch @p call on @p context through a synthetic guest call frame
    /// at @p frame; returns what the method returned.
    u64 replay_call(GuestAddr context, const RecordedCall& call,
                    GuestAddr frame);
    void replay_mapped_upload(GuestAddr context, const RecordedCall& upload,
                              GuestAddr frame);
    /// Record UpdateSubresource(1): the box and the source bytes are copied.
    void record_update_subresource(Marshaler& m, usize slot,
                                   DeferredRecording& recording,
                                   RecordedCall& call);
    /// Map on a deferred context; returns the HRESULT.
    [[nodiscard]] u32 deferred_map(Marshaler& m, GuestAddr self,
                                   DeferredContext& context);
    template <typename F>
    static void visit_bound_objects(const ContextBindings& bindings, F&& f);
    [[nodiscard]] GuestAddr arena_allocate(GuestArena& arena, usize bytes);
    void release_recording(DeferredRecording& recording);
    [[nodiscard]] std::unique_ptr<DeferredRecording> new_recording();
    /// AddRef / Release for any object a recording may name, command lists
    /// included; hold returns false for an address that is no live object.
    [[nodiscard]] bool hold_object(GuestAddr address);
    void unhold_object(GuestAddr address);
    /// Take / drop one reference on every object @p bindings names -- the
    /// same set clear_context_state releases.
    void hold_bindings(const ContextBindings& bindings);
    void unhold_bindings(const ContextBindings& bindings);
    /// ID3D11DeviceContext1::ClearView.
    enum class DebugMarkerKind : u32 { Push = 0, Pop = 1, Signpost = 2 };
    /// A D3D event marker (ID3D11DeviceContext2 BeginEventInt / EndEvent /
    /// SetMarkerInt, ID3DUserDefinedAnnotation BeginEvent / EndEvent /
    /// SetMarker) on @p context: forwarded to the backend's GPU command
    /// stream (a deferred context records it for ExecuteCommandList).
    /// @p label is the guest's LPCWSTR (NULL allowed); @p data, the Int
    /// variants' value, is appended to it.
    void annotation_marker(GuestAddr context, DebugMarkerKind kind,
                           GuestAddr label, const i32* data);
    void emit_debug_marker(DebugMarkerKind kind, const std::string& label);
    /// ClearView's depth-stencil half: @p boxes are the rects clamped to the
    /// non-negative quadrant (none: the whole view, unless @p had_rects).
    void clear_depth_stencil_view_rects(
        const DepthStencilView& dsv, const std::array<float, 4>& color,
        const std::vector<std::array<u32, 4>>& boxes, bool had_rects);
    void clear_view(Marshaler& m, GuestAddr self);
    /// The interface of resource @p texture -- a buffer or a texture of any
    /// dimension -- that answers @p iid (the resource itself or one of its
    /// DXGI facets), or 0. Every facet's QueryInterface uses it.
    [[nodiscard]] GuestAddr texture_interface_for(GuestAddr texture,
                                                  const GuidBytes& iid);
    /// QueryInterface (this, riid, ppv) on resource @p resource or any of its
    /// facets: *ppv NULLed, E_POINTER / E_NOINTERFACE, one public reference.
    [[nodiscard]] u32 resource_query_interface(Marshaler& m,
                                               GuestAddr resource);
    /// The interface of buffer @p address that @p iid names (0: none), as
    /// its own QueryInterface answers.
    [[nodiscard]] GuestAddr buffer_interface_for(GuestAddr address,
                                                 const GuidBytes& iid);
    [[nodiscard]] u32 multisample_quality_levels(u32 dxgi_format,
                                                 u32 sample_count) const;
    /// The host-GPU facts the per-format capability table
    /// (d3d11_format_caps.hpp) answers from, for a device at @p feature_level.
    [[nodiscard]] d3d11::FormatCapsContext format_caps_context(
        u32 feature_level) const;
    /// The context for @p device: its feature level, and the Direct3D 10.0
    /// API's format set when D3D10CreateDevice made it.
    [[nodiscard]] d3d11::FormatCapsContext format_caps_context(
        const Device& device) const;
    /// Whether a stream-output query is building on @p context.
    [[nodiscard]] bool stream_out_queries_building(GuestAddr context) const;
    /// Hand a streaming draw's stats record (graphics::SO_STATS_WORDS, filled
    /// by the GPU accounting) to every stream-output query building on
    /// @p context; their data is final once the recorded work completes.
    void note_stream_out_stats(
        GuestAddr context,
        const std::shared_ptr<graphics::BackendBuffer>& stats);
    /// Queries: ID3D11DeviceContext Begin / End / GetData and the
    /// predication test applied before each operation Learn's
    /// SetPredication page lists (d3d11_context.cpp).
    void query_begin(GuestAddr context, GuestAddr async);
    void query_end(GuestAddr context, GuestAddr async);
    [[nodiscard]] u32 query_get_data(GuestAddr context, GuestAddr async,
                                     GuestAddr data, u32 data_size, u32 flags);
    /// Remove @p query from the counting list of the context building it.
    void stop_counting(Query& query);
    /// True when the operation about to run on @p context must be skipped:
    /// a predicate is set and its data equals PredicateValue.
    [[nodiscard]] bool predicated_off(GuestAddr context);
    /// The occlusion counter a draw on @p context counts into, or null when
    /// no occlusion query is building there.
    [[nodiscard]] graphics::GpuOcclusionCounter* occlusion_counter_for_draw(
        GuestAddr context);
