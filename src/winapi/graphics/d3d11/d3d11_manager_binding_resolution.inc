// SPDX-License-Identifier: LGPL-2.1-only
// @file d3d11_manager_binding_resolution.inc
// @brief D3D11Manager class body, part 12 of 17: binding resolution (descriptor
//   tables, sampler and SRV resolution, the R4 draw memos, native states).
//
// A fragment of the D3D11Manager definition in d3d11.hpp, which
// includes it inside the class, in this order; not a standalone header.

    /// Mirror one stage's b#/t#/s# bindings into its descriptor tables and point
    /// @p out at them.
    ///
    /// Only the registers @p layout says the shader declares are synced: a
    /// shader reading t0..t11 costs twelve writes, not 128. The table dedupes
    /// unchanged slots itself, so a stable binding set re-publishes nothing.
    ///
    /// Returns false when a table could not be created or a slot could not be
    /// written, which drops the draw. Binding some of a shader's register spaces
    /// and not others would render with whatever the missing ones last held.
    /// @p border_twins: s# whose border twin the shader samples; the twin
    /// goes in sampler-table entry SAMPLER_SLOTS + n (border_twin_mask).
    [[nodiscard]] bool sync_descriptor_tables(
        ContextState& state, d3d11::StageBindings stage,
        const d3d11::ShaderBindingLayout& layout,
        std::array<const graphics::BackendDescriptorTable*,
                   graphics::BACKEND_MAX_DESCRIPTOR_SETS>& out,
        u32 border_twins = 0);

    /// Resolve a t# slot holding a texture SRV. Leaves the outputs untouched
    /// when the slot is empty or unresolvable.
    /// Words a raw/structured buffer view spans (num_elements *
    /// element_bytes / 4), for the translator's out-of-bounds guard; 0 for
    /// anything that is not such a view.
    /// m_sampler_states / m_shader_resource_views lookups through their memos;
    /// null for 0 or an unknown address.
    [[nodiscard]] const SamplerState* find_sampler_state(GuestAddr address) const;
    /// Drop the lookup memo's entry for one erased object. The maps are
    /// node-based, so erasing one element leaves every other memoized
    /// pointer valid; only a bulk erase needs the generation bump.
    void forget_sampler_memo(GuestAddr address) noexcept;
    void forget_srv_memo(GuestAddr address) noexcept;
    [[nodiscard]] const ShaderResourceView* find_shader_resource_view(
        GuestAddr address) const;
    /// The t# entry of the size table: a raw/structured buffer view's words,
    /// or a typed buffer view's element bias and format-alpha bit
    /// (graphics::DxbcTranslationOptions::typed_buffer_bias).
    [[nodiscard]] u32 srv_buffer_size_words(GuestAddr srv_address) const;
    /// The u# entry of the size table: a raw/structured buffer view's words,
    /// a typed buffer view's element bias, or a Texture3D view's W-slice
    /// window (graphics::kVolumeUavWindowSizeShift; 0 for the whole mip).
    [[nodiscard]] u32 uav_buffer_size_words(GuestAddr uav_address) const;
    /// The t# entries of @p stage_srvs' typed buffer views -- their element
    /// biases -- into the size table of the push block @p words: the
    /// geometry, hull and domain kernels, whose blocks carry no view sizes.
    void fill_typed_buffer_biases(std::span<u32> words,
                                  std::span<const GuestAddr> stage_srvs) const;
    void resolve_shader_resource(
        GuestAddr srv_address,
        const graphics::BackendTexture2D*& texture_2d,
        graphics::BackendTextureViewDesc& backend_view,
        const graphics::BackendTexture3D*& texture_3d);
    /// Move a buffer render-target view's window between the buffer and its
    /// proxy texture, in command order: `into_proxy` before a draw or clear
    /// through the view (so blending and unwritten pixels see the buffer),
    /// else after it (the buffer then holds what was rendered). False when
    /// the copy could not be recorded.
    [[nodiscard]] bool sync_buffer_render_target(const RenderTargetView& view,
                                                 bool into_proxy);
    /// A D3D11_RTV_DIMENSION_BUFFER view of @p buffer_address: elements
    /// [first_element, + num_elements) of @p element_bytes each. 0 when it
    /// cannot be created.
    [[nodiscard]] GuestAddr create_buffer_render_target_view(
        GuestAddr device_owner, GuestAddr buffer_address, u32 format,
        u32 first_element, u32 num_elements, u32 element_bytes);
    /// What a Texture3D UAV at u#@p slot binds: its whole mip under the
    /// view's format -- the storage's own, a typed member of a TYPELESS
    /// family, or an R32 cast (functional spec 5.3.9.5) -- through a
    /// backend view. A W-slice window is applied by the translated shader
    /// from the size table (uav_buffer_size_words): Metal views a volume
    /// whole in W. Null, reported once, when neither can express it.
    [[nodiscard]] graphics::BackendTexture3D* volume_uav_binding(
        const UnorderedAccessView& view, Texture2D& texture, u32 slot);
    /// s0..s7 of @p samplers whose state needs the border-twin emulation
    /// (bit n = s#n): BORDER addressing with a colour Metal cannot express.
    [[nodiscard]] u32 border_twin_mask(std::span<const GuestAddr> samplers) const;
    /// The four floats a twinned s# puts in the push block's border member:
    /// its border colour, or for a comparison filter (red,
    /// D3D11_COMPARISON_FUNC, 0, 0) -- the translator compares against the
    /// red itself (DxbcTranslationOptions::border_twin_samplers).
    [[nodiscard]] std::array<float, 4> border_twin_push_colour(
        GuestAddr sampler_address) const;
    /// t0..t31 of @p views holding an R32G32B32 typed buffer view (bit n =
    /// t#n), which the translated shader reads as three R32 texels per
    /// element (DxbcTranslationOptions::rgb32_buffer_srvs). Sets
    /// @p unservable for one at t32 or above, past the variant mask.
    [[nodiscard]] u32 rgb32_buffer_srv_mask(std::span<const GuestAddr> views,
                                            bool& unservable) const;
    /// t0..t31 of @p views holding a TEXTURE2DMS / TEXTURE2DMSARRAY view of
    /// a ONE-sample texture (bit n = t#n), which binds as its 2D mip and is
    /// read through a translator variant
    /// (DxbcTranslationOptions::single_sample_ms_srvs). Sets @p unservable
    /// for one at t32 or above, past the variant mask.
    [[nodiscard]] u32 single_sample_ms_srv_mask(
        std::span<const GuestAddr> views, bool& unservable) const;
    /// The MipLODBias of the sampler state bound at an s# slot (0 when
    /// empty): the push block's member 0, which the translated shader adds.
    [[nodiscard]] float sampler_mip_lod_bias(GuestAddr sampler_address) const;
    /// Bit n set when s#n holds a sampler whose mip filter is POINT: the
    /// push block's member 4 (graphics::kPointMipMaskOffset).
    /// s# whose shader bias carries the point-mip tie nudge (bit per slot).
    [[nodiscard]] u32 point_mip_nudge_mask(
        std::span<const GuestAddr> samplers) const;
    [[nodiscard]] u32 point_mip_sampler_mask(
        std::span<const GuestAddr> samplers) const;
    /// s# (bit n) holding a MINIMUM_ / MAXIMUM_ sampler state, which a
    /// translator variant reduces in the shader
    /// (DxbcTranslationOptions::minmax_samplers).
    [[nodiscard]] u32 minmax_sampler_mask(
        std::span<const GuestAddr> samplers) const;
    /// Push member 13 (graphics::kMinMaxSamplerOffset) for @p samplers.
    void fill_minmax_samplers(std::span<u32> words,
                              std::span<const GuestAddr> samplers) const;
    /// A push block (graphics::kPushBlockBytes, as words) carrying only
    /// @p samplers' LOD biases and point-mip mask: what the geometry and hull
    /// kernels read for their explicit-LOD sampling. No view sizes: those
    /// kernels are translated without the buffer-view bounds guard (their
    /// callers add the constant-buffer limits and the typed-buffer biases,
    /// fill_typed_buffer_biases).
    [[nodiscard]] std::vector<u32> sampler_push_block(
        std::span<const GuestAddr> samplers) const;
    /// Push member 12 (graphics::kConstantBufferLimitOffset) of the push block
    /// @p words: per b# slot the elements a shader may read -- the
    /// *SetConstantBuffers1 window (NumConstants) within the buffer past
    /// FirstConstant, 0 for an empty slot -- tagged with
    /// graphics::kConstantBufferLimitKnown (D3D11.3 7.5 and 5.3.4.3.2; the
    /// translator's DxbcTranslationOptions::guard_constant_buffers).
    void fill_constant_buffer_limits(std::span<u32> words,
                                     std::span<const GuestAddr> buffers,
                                     std::span<const u32> first,
                                     std::span<const u32> count) const;
    /// The byte offset a b# slot binds its buffer at: FirstConstant * 16, or
    /// 0 when that is at or past the buffer's end -- nothing is readable
    /// there (the limit is 0), and a Metal offset past the buffer is invalid.
    [[nodiscard]] u32 constant_buffer_bind_offset(GuestAddr buffer,
                                                  u32 first) const;
    /// What execute_draw derives from one stage's 16 bound s# slots, built
    /// by the functions above (so it cannot drift from them) and memoised:
    /// it is a pure function of the bound addresses and the sampler states,
    /// which D3D11 makes immutable, so it holds while the addresses and
    /// m_sampler_set_generation are unchanged. Seven passes over the 16
    /// slots per stage per draw were ~5% of Satisfactory's RHI thread.
    struct SamplerStageSummary {
        std::array<GuestAddr, d3d11::SAMPLER_SLOTS> bound{};
        u64 generation{0};
        u32 border_twins{0};      ///< border_twin_mask
        u32 minmax{0};            ///< minmax_sampler_mask
        u32 point_mip{0};         ///< point_mip_sampler_mask
        u32 point_mip_nudge{0};   ///< point_mip_nudge_mask
        std::array<float, d3d11::SAMPLER_SLOTS> lod_bias{};  ///< sampler_mip_lod_bias
        /// border_twin_push_colour, for the slots in border_twins.
        std::array<std::array<float, 4>, d3d11::SAMPLER_SLOTS> border_colour{};
        /// fill_minmax_samplers' four words per slot, for the slots in minmax.
        std::array<std::array<u32, 4>, d3d11::SAMPLER_SLOTS> minmax_entry{};
        /// R4 stage-memo: the SamplerSlots stamp this summary is for, when
        /// `stamped` (d3d11_draw_memo.hpp).
        bool stamped{false};
        u64 stamp{0};
    };
    /// The summary for @p bound, from the memo for @p stage (0 pixel,
    /// 1 vertex) or rebuilt into it. Re-fetch at each use: a nested
    /// execute_draw (geometry/tessellation passes) may rebuild the entry.
    [[nodiscard]] const SamplerStageSummary& sampler_stage_summary(
        u32 stage, std::span<const GuestAddr> bound) const;
    /// What execute_draw derives from one stage's bound t# slots: the
    /// R32G32B32 variant mask (rgb32_buffer_srv_mask), the one-sample MS
    /// variant mask (single_sample_ms_srv_mask), whether either met a view
    /// at t32 or above, and the bounds guard's size table
    /// (srv_buffer_size_words). Memoised like SamplerStageSummary: a pure
    /// function of the bound addresses and the views at them, which D3D11
    /// makes immutable and which hold their resources, so it holds while the
    /// addresses and m_srv_set_generation are unchanged. Three lookups per
    /// bound view per stage per draw were part of the ~4-5% of Satisfactory's
    /// RHI thread that the SRV scans cost.
    struct SrvStageSummary {
        std::array<GuestAddr, d3d11::SHADER_RESOURCE_SLOTS> bound{};
        u64 generation{0};
        u32 rgb32{0};
        u32 ms1{0};
        bool rgb32_unservable{false};
        bool ms1_unservable{false};
        std::array<u32, d3d11::SHADER_RESOURCE_SLOTS> buffer_size_words{};
        /// R4 stage-memo: the SrvSlots stamp this summary is for.
        bool stamped{false};
        u64 stamp{0};
        /// R4c: buffer_size_words entries from here on are 0.
        usize size_used{d3d11::SHADER_RESOURCE_SLOTS};
    };
    /// The summary for @p bound, from the memo for @p stage (0 pixel,
    /// 1 vertex) or rebuilt into it. Re-fetch at each use, as for
    /// sampler_stage_summary.
    [[nodiscard]] const SrvStageSummary& srv_stage_summary(
        u32 stage, std::span<const GuestAddr> bound) const;
    // ---- R4 (d3d11_draw_memo.hpp)
    /// stage-memo: the summaries keyed by the binding array's stamp; the
    /// content-compared memo above when the part is off.
    [[nodiscard]] const SrvStageSummary& srv_stage_summary(
        u32 stage, const d3d11::SrvSlots& bound) const;
    [[nodiscard]] const SamplerStageSummary& sampler_stage_summary(
        u32 stage, const d3d11::SamplerSlots& bound) const;
    /// sampler-memo: a stage's flat s# slots as resolve_sampler leaves them
    /// in a fresh BackendDrawCall (and their native states), keyed by the
    /// SamplerSlots stamp.
    struct ResolvedSamplers {
        bool valid{false};
        bool with_native{false};
        u64 stamp{0};
        std::array<graphics::BackendSamplerDesc,
                   graphics::BACKEND_MAX_TEXTURE_SLOTS> descs{};
        std::array<const graphics::BackendSamplerState*,
                   graphics::BACKEND_MAX_TEXTURE_SLOTS> natives{};
    };
    [[nodiscard]] const ResolvedSamplers& resolved_samplers(
        u32 stage, const d3d11::SamplerSlots& bound, bool with_native);
    mutable std::array<ResolvedSamplers, 2> m_resolved_samplers{};
    /// draw_memo::active_mask() for the draw being executed, taken once at
    /// the top of execute_draw.
    u32 m_draw_memo_mask{0};
    [[nodiscard]] bool memo_on(d3d11::draw_memo::Part part) const noexcept {
        return ((m_draw_memo_mask >> static_cast<u32>(part)) & 1u) != 0;
    }
    /// srv-memo: a stage's flat t# slots as the SRV resolvers leave them in
    /// a fresh BackendDrawCall, keyed by the SrvSlots stamp and
    /// m_texture_backend_gen.
    struct ResolvedSrvs {
        bool valid{false};
        u64 stamp{0};
        u64 backend_gen{0};
        static constexpr usize N = graphics::BACKEND_MAX_TEXTURE_SLOTS;
        std::array<const graphics::BackendTexture2D*, N> textures{};
        std::array<graphics::BackendTextureViewDesc, N> views{};
        std::array<const graphics::BackendTexture3D*, N> textures_3d{};
        std::array<const graphics::BackendBuffer*, N> buffers{};
        std::array<unsigned, N> offsets{};
        std::array<const graphics::BackendTextureBuffer*, N> texture_buffers{};
    };
    [[nodiscard]] const ResolvedSrvs& resolved_srvs(u32 stage,
                                                    const d3d11::SrvSlots& bound);
    std::array<ResolvedSrvs, 2> m_resolved_srvs{};
    /// srv-memo: bumped whenever a live texture record's backend object is
    /// replaced (resize_swap_chain_buffers, swap_chain_index_buffer).
    u64 m_texture_backend_gen{1};
    u64 m_r4_srv_hits{0};
    u64 m_r4_srv_misses{0};
    /// om-memo: the attachments of the last draw, keyed by the OM stamps
    /// and m_texture_backend_gen (execute_draw).
    struct OmMemo {
        bool valid{false};
        u64 rt_stamp{0};
        u64 dsv_stamp{0};
        u64 backend_gen{0};
        static constexpr usize N = graphics::BACKEND_MAX_RENDER_TARGETS;
        std::array<graphics::BackendTexture2D*, N> color_targets{};
        std::array<graphics::PixelFormat, N> color_formats{};
        std::array<u32, N> color_mip_slices{};
        std::array<u32, N> color_array_slices{};
        std::array<u32, N> color_array_sizes{};
        std::array<graphics::BackendTexture3D*, N> color_targets_3d{};
        std::array<u32, N> color_depth_planes{};
        std::array<u32, N> color_view_formats{};
        std::array<GuestAddr, N> buffer_target_views{};
        /// The textures whose render-target diagnostics a draw bumps.
        std::array<Texture2D*, N> rt_diag{};
        /// The depth-stencil target's texture, for its draw-pair diagnostic.
        Texture2D* depth_diag{nullptr};
        graphics::BackendTexture2D* depth{nullptr};
        u32 depth_mip_slice{0};
        u32 depth_array_slice{0};
        u32 depth_array_size{1};
        u32 depth_view_flags{0};
    };
    OmMemo m_om_memo{};
    /// vb-memo: the backend buffer of each bound IA slot, keyed by the
    /// vertex-buffer array's stamp.
    struct ResolvedVbs {
        bool valid{false};
        u64 stamp{0};
        usize used{0};
        std::array<graphics::BackendBuffer*, d3d11::VERTEX_BUFFER_SLOTS>
            buffers{};
    };
    [[nodiscard]] const ResolvedVbs& resolved_vertex_buffers(
        const d3d11::StampedSlots<GuestAddr, d3d11::VERTEX_BUFFER_SLOTS>&
            bound);
    ResolvedVbs m_resolved_vbs{};
    u64 m_r4_vb_hits{0};
    u64 m_r4_vb_misses{0};
    /// key-memo: the last draw's pipeline-key inputs and the key mixed from
    /// them (execute_draw).
    struct KeyMemo {
        bool valid{false};
        usize count{0};
        std::array<u64, 512> inputs{};
        u64 key{0};
    };
    KeyMemo m_key_memo{};
    u64 m_r4_key_hits{0};
    u64 m_r4_key_misses{0};
    u64 m_r4_om_hits{0};
    u64 m_r4_om_misses{0};
    /// Engagement counters (draw-thread plain integers, printed by tick).
    mutable u64 m_r4_stage_hits{0};
    mutable u64 m_r4_stage_misses{0};
    u64 m_r4_sampler_hits{0};
    u64 m_r4_sampler_misses{0};
    u64 m_r4_ia_hits{0};
    u64 m_r4_ia_misses{0};
    // ---- end R4
    /// Resolve an s# slot. Independent of every t#. False for an empty slot.
    bool resolve_sampler(GuestAddr sampler_address,
                         graphics::BackendSamplerDesc& backend_sampler,
                         float* shader_lod_bias,
                         const graphics::BackendSamplerState** native = nullptr);
    // ---- R3c native-state (graphics/contracts/delta_record.hpp)
    /// Before @p call is drawn: point it at @p states, keep each sampler
    /// state there only where it was built from exactly the slot's final
    /// desc, and name the depth-stencil state built from exactly the final
    /// depth/stencil fields (from @p dss's variants, or the device's for the
    /// NULL default state).
    void settle_native_states(graphics::BackendDrawCall& call,
                              graphics::BackendNativeStates& states,
                              const DepthStencilState* dss,
                              bool samplers_settled = false);
    /// The backend sampler for an empty s# slot (the default sampler).
    std::shared_ptr<graphics::BackendSamplerState> m_default_sampler_state;
    bool m_default_sampler_state_tried{false};
    /// The NULL depth-stencil state's variants (settle_native_states).
    std::array<std::shared_ptr<graphics::BackendDepthStencilState>, 4>
        m_default_depth_stencil_states{};
    bool m_native_depth_stencil_unsupported{false};
    // ---- end R3c
    /// Resolve a t# slot holding a buffer SRV: a structured/raw buffer, or a
    /// typed buffer, which is a texture-buffer view rather than a plain buffer.
    void resolve_buffer_resource(
        GuestAddr srv_address, const graphics::BackendBuffer*& out_buffer,
        unsigned& out_offset,
        const graphics::BackendTextureBuffer*& out_texture_buffer);
