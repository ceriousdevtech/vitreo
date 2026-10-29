// SPDX-License-Identifier: LGPL-2.1-only
// @file d3d11_manager_state_records.inc
// @brief D3D11Manager class body, part 4 of 17: pipeline-state records (blend,
//   rasterizer, depth-stencil, sampler) and context binding state
//   (ContextBindings, ContextState).
//
// A fragment of the D3D11Manager definition in d3d11.hpp, which
// includes it inside the class, in this order; not a standalone header.

    struct BlendStateDesc {
        u32 alpha_to_coverage_enable{0};
        u32 independent_blend_enable{0};
        std::array<u32, 8> blend_enable{};
        std::array<u32, 8> src_blend{};
        std::array<u32, 8> dest_blend{};
        std::array<u32, 8> blend_op{};
        std::array<u32, 8> src_blend_alpha{};
        std::array<u32, 8> dest_blend_alpha{};
        std::array<u32, 8> blend_op_alpha{};
        std::array<u32, 8> render_target_write_mask{};
        /// D3D11_RENDER_TARGET_BLEND_DESC1::LogicOpEnable and LogicOp,
        /// reported by GetDesc1. Windows reports NOOP for a state made by
        /// CreateBlendState, and for any target whose logic op is disabled --
        /// measured. LogicOpEnable is always FALSE in a created state: this
        /// device has no logic ops, so CreateBlendState1 refuses it.
        std::array<u32, 8> logic_op_enable{};
        std::array<u32, 8> logic_op{4, 4, 4, 4, 4, 4, 4, 4};
    };

    /// The deduplication bookkeeping every state object carries (Learn
    /// CreateBlendState / CreateDepthStencilState / CreateRasterizerState /
    /// CreateSamplerState: an identical description returns the existing
    /// object). `pool_key` is the object's entry in m_state_pool, erased by
    /// its final Release. `runtime_default` marks the object whose
    /// description is the family's default: the runtime holds one of those
    /// per device from creation on, so it outlives the application's last
    /// Release and counts toward the 4096-object limit whether or not the
    /// application ever made it (measured, d3d11_a04_state_objects_probe
    /// "smp release 1,0 then recreate", "limit *: made=4095").
    struct StatePoolEntry {
        std::string pool_key;
        bool runtime_default{false};
    };
    /// The four deduplicated families; Blend/Blend1 and Rasterizer/1/2 each
    /// share one.
    enum StateFamily : u8 {
        STATE_FAMILY_BLEND = 0,
        STATE_FAMILY_RASTERIZER = 1,
        STATE_FAMILY_DEPTH_STENCIL = 2,
        STATE_FAMILY_SAMPLER = 3,
    };

    struct BlendState {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        BlendStateDesc desc{};
        PrivateDataStore private_data;
        StatePoolEntry pool;
    };

    struct RasterizerStateDesc {
        u32 fill_mode{0};
        u32 cull_mode{0};
        u32 front_counter_clockwise{0};
        i32 depth_bias{0};
        float depth_bias_clamp{0.0f};
        float slope_scaled_depth_bias{0.0f};
        u32 depth_clip_enable{0};
        u32 scissor_enable{0};
        u32 multisample_enable{0};
        u32 antialiased_line_enable{0};
        u32 forced_sample_count{0};
        u32 conservative_raster{0};
    };

    struct RasterizerState {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        RasterizerStateDesc desc{};
        PrivateDataStore private_data;
        StatePoolEntry pool;
    };

    struct DepthStencilStateDesc {
        u32 depth_enable{0};
        u32 depth_write_mask{0};
        u32 depth_func{0};
        u32 stencil_enable{0};
        u8 stencil_read_mask{0xFF};
        u8 stencil_write_mask{0xFF};
        // Front face stencil ops
        u32 front_stencil_fail_op{0};
        u32 front_stencil_depth_fail_op{0};
        u32 front_stencil_pass_op{0};
        u32 front_stencil_func{0};
        // Back face stencil ops
        u32 back_stencil_fail_op{0};
        u32 back_stencil_depth_fail_op{0};
        u32 back_stencil_pass_op{0};
        u32 back_stencil_func{0};
    };

    struct DepthStencilState {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        DepthStencilStateDesc desc{};
        PrivateDataStore private_data;
        StatePoolEntry pool;
        // ---- R3c native-state (graphics/contracts/delta_record.hpp)
        /// The backend states a draw through this object resolved to, one
        /// per distinct set of draw fields (read-only DSV masking makes up
        /// to four), built on first use (native_depth_stencil_state).
        mutable std::array<std::shared_ptr<graphics::BackendDepthStencilState>,
                           4>
            backend_states{};
        // ---- end R3c
    };

    struct SamplerStateDesc {
        u32 filter{0};
        u32 address_u{0};
        u32 address_v{0};
        u32 address_w{0};
        float mip_lod_bias{0.0f};
        u32 max_anisotropy{1};
        u32 comparison_func{0};
        float border_color[4]{};
        float min_lod{0.0f};
        float max_lod{3.402823466e+38f}; // FLT_MAX
    };

    struct SamplerState {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        SamplerStateDesc desc{};
        PrivateDataStore private_data;
        StatePoolEntry pool;
        // ---- R3c native-state (graphics/contracts/delta_record.hpp)
        /// The backend sampler built from what resolve_sampler makes of
        /// desc, on the object's first draw; null until then or when the
        /// backend has none.
        mutable std::shared_ptr<graphics::BackendSamplerState> backend_state;
        mutable bool backend_state_tried{false};
        // ---- end R3c
    };

    /// A context's binding state: everything ClearState resets. Plain data,
    /// so a copy is a snapshot -- a deferred context's FinishCommandList(TRUE)
    /// carries one into the next command list (DeferredRecording::initial).
    /// A copy holds no references by itself; hold_bindings takes them.
    struct ContextBindings {
        GuestAddr vertex_shader{0};
        GuestAddr pixel_shader{0};
        GuestAddr compute_shader{0};
        GuestAddr geometry_shader{0};
        /// *SetShader with class instances (D3D11.3 7.19.6), per LinkedStage:
        /// the application's interface shader and the instances it was bound
        /// with. The stage's shader field above then holds the
        /// specialisation that runs; the binding's references are on the
        /// application's shader and on each instance (runtime references,
        /// as measured: AddRef/Release counts do not move).
        struct LinkedBinding {
            GuestAddr shader{0};
            std::vector<GuestAddr> instances;
        };
        std::array<LinkedBinding, 6> linked{};
        /// Geometry-stage registers. The geometry shader runs as a compute
        /// kernel, so these bind at the compute windows during expansion.
        std::array<GuestAddr, d3d11::CONSTANT_BUFFER_SLOTS>
            geometry_constant_buffers{};
        std::array<u32, d3d11::CONSTANT_BUFFER_SLOTS> geometry_constant_first{};
        std::array<u32, d3d11::CONSTANT_BUFFER_SLOTS> geometry_constant_count =
            d3d11::kWholeConstantWindows;
        d3d11::SrvSlots
            geometry_shader_resources{};
        d3d11::SamplerSlots geometry_samplers{};
        /// Hull- and domain-stage registers and shaders (11.17). The hull
        /// shader runs as a compute kernel; the domain shader is the patch
        /// draw's vertex function.
        GuestAddr hull_shader{0};
        GuestAddr domain_shader{0};
        std::array<GuestAddr, d3d11::CONSTANT_BUFFER_SLOTS>
            hull_constant_buffers{};
        std::array<u32, d3d11::CONSTANT_BUFFER_SLOTS> hull_constant_first{};
        std::array<u32, d3d11::CONSTANT_BUFFER_SLOTS> hull_constant_count =
            d3d11::kWholeConstantWindows;
        d3d11::SrvSlots
            hull_shader_resources{};
        d3d11::SamplerSlots hull_samplers{};
        std::array<GuestAddr, d3d11::CONSTANT_BUFFER_SLOTS>
            domain_constant_buffers{};
        std::array<u32, d3d11::CONSTANT_BUFFER_SLOTS> domain_constant_first{};
        std::array<u32, d3d11::CONSTANT_BUFFER_SLOTS> domain_constant_count =
            d3d11::kWholeConstantWindows;
        d3d11::SrvSlots
            domain_shader_resources{};
        d3d11::SamplerSlots domain_samplers{};
        /// SOSetTargets. Binding does not reset a buffer's filled size unless
        /// an explicit offset is given; see Buffer::so_filled.
        std::array<GuestAddr, 4> stream_out_targets{};
        /// NumBuffers of the last SOSetTargets through the D3D10 device:
        /// ID3D10Device::SOGetTargets reports offset 0xFFFFFFFF for those
        /// slots and 0 for the rest (measured; D3D11 keeps no offsets).
        u32 d3d10_stream_out_count{0};
        /// ID3D11DeviceContext3::SetHardwareProtectionState, stored and
        /// reported back -- measured.
        bool hardware_protection{false};
        GuestAddr input_layout{0};
        GuestAddr blend_state{0};
        GuestAddr rasterizer_state{0};
        GuestAddr depth_stencil_state{0};
        // Sized to the D3D11 register spaces, not to what one draw can bind.
        // A guest that sets t100 and reads it back must get it, and the
        // arrays used to be sized to the backend's flat capacity, so every
        // binding above slot 7 (or 3, for a constant buffer) was silently
        // dropped by the bounds check in the setter -- a wrong render with no
        // diagnostic. Shaders that declare registers past the flat capacity are
        // bound through descriptor tables instead; see `descriptor_tables`.
        std::array<GuestAddr, d3d11::CONSTANT_BUFFER_SLOTS>
            vertex_constant_buffers{};
        std::array<GuestAddr, d3d11::CONSTANT_BUFFER_SLOTS>
            pixel_constant_buffers{};
        d3d11::SrvSlots
            vertex_shader_resources{};
        d3d11::SamplerSlots vertex_samplers{};
        d3d11::SrvSlots
            pixel_shader_resources{};
        d3d11::SamplerSlots pixel_samplers{};
        std::array<GuestAddr, d3d11::CONSTANT_BUFFER_SLOTS>
            compute_constant_buffers{};
        /// ID3D11DeviceContext1's per-slot constant window, in CONSTANTS (16
        /// bytes each), as *SetConstantBuffers1 supplies it. The plain
        /// *SetConstantBuffers resets a slot's window to the whole buffer, which
        /// is what D3D11.0 means and what *GetConstantBuffers1 reports: first
        /// constant 0 and a count of WHOLE_CONSTANT_WINDOW (4096). A count of
        /// 0 is a real, empty window (measured, d3d11_a05_context_state_probe).
        std::array<u32, d3d11::CONSTANT_BUFFER_SLOTS>
            vertex_constant_first{};
        std::array<u32, d3d11::CONSTANT_BUFFER_SLOTS>
            vertex_constant_count = d3d11::kWholeConstantWindows;
        std::array<u32, d3d11::CONSTANT_BUFFER_SLOTS>
            pixel_constant_first{};
        std::array<u32, d3d11::CONSTANT_BUFFER_SLOTS>
            pixel_constant_count = d3d11::kWholeConstantWindows;
        std::array<u32, d3d11::CONSTANT_BUFFER_SLOTS>
            compute_constant_first{};
        std::array<u32, d3d11::CONSTANT_BUFFER_SLOTS>
            compute_constant_count = d3d11::kWholeConstantWindows;
        d3d11::SrvSlots
            compute_shader_resources{};
        d3d11::SamplerSlots compute_samplers{};
        std::array<GuestAddr, d3d11::UAV_SLOTS>
            compute_unordered_access_views{};
        /// UAVs bound through OMSetRenderTargetsAndUnorderedAccessViews.
        /// Learn: for the graphics pipeline these share the u# register space
        /// with the render targets, so the array is indexed by the absolute
        /// u# slot (UAVStartSlot + i), not by a per-call offset.
        std::array<GuestAddr, d3d11::UAV_SLOTS>
            output_merger_unordered_access_views{};
        // All 32 IA slots (D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT). These
        // used to be sized to the backend's 8 streams, so IASetVertexBuffers
        // silently dropped slots 8-31 and IAGetVertexBuffers read them back
        // NULL. The draw maps the slots its input layout uses onto backend
        // streams instead (InputLayout::backend_stream).
        /// R4c vb-memo: stamped (d3d11_draw_memo.hpp).
        d3d11::StampedSlots<GuestAddr, d3d11::VERTEX_BUFFER_SLOTS>
            vertex_buffers{};
        std::array<u32, d3d11::VERTEX_BUFFER_SLOTS> vertex_buffer_strides{};
        std::array<u32, d3d11::VERTEX_BUFFER_SLOTS> vertex_buffer_offsets{};
        GuestAddr index_buffer{0};
        u32 index_buffer_format{0};
        u32 index_buffer_offset{0};
        u32 primitive_topology{0};
        /// R4 (d3d11_draw_memo.hpp): stamped, for the attachment memo.
        d3d11::StampedSlots<GuestAddr, 8> render_targets{};
        d3d11::StampedValue<GuestAddr> depth_stencil_view{};
        std::array<float, 4> blend_factor{1.0f, 1.0f, 1.0f, 1.0f};
        u32 sample_mask{0xFFFFFFFFu};
        u32 stencil_ref{0};
        /// Every viewport and scissor rect bound, up to the pipeline's 16
        /// (D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE). Draws
        /// use entry 0 unless the vertex shader writes SV_ViewportArrayIndex,
        /// which rasterizes through the whole array. RSGetViewports and
        /// RSGetScissorRects report all of them, and a context with none
        /// bound reports zero (both measured, d3d11_rs_state_probe).
        u32 viewport_count{0};
        std::array<std::array<float, 6>, 16> viewports{{
            {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f}}};
        u32 scissor_rect_count{0};
        std::array<std::array<i32, 4>, 16> scissor_rects{};
        /// SetPredication state. Learn: a NULL predicate means no predication
        /// and the value is still "preserved for GetPredication", so the two are
        /// stored independently.
        GuestAddr predicate{0};
        u32 predicate_value{0};
    };

    /// A context: its bindings, plus what belongs to the context itself and
    /// survives ClearState, SwapDeviceContextState and ExecuteCommandList.
    struct ContextState : ContextBindings {
        std::unordered_map<u64, std::unique_ptr<graphics::BackendPipeline>>
            pipelines;
        /// Descriptor tables mirroring the bindings above, for shaders whose
        /// register spaces exceed the backend's flat capacity. Indexed by
        /// [stage][descriptor set], created on first use: a title that never
        /// binds past the flat capacity never allocates one.
        std::array<std::array<std::unique_ptr<graphics::BackendDescriptorTable>,
                              graphics::BACKEND_MAX_DESCRIPTOR_SETS>,
                   static_cast<usize>(d3d11::StageBindings::Count)>
            descriptor_tables{};
        /// OCCLUSION* and PIPELINE_STATISTICS queries building on this
        /// context: every draw / dispatch is counted into each of them.
        std::vector<GuestAddr> counting_queries;
        /// The open occlusion interval: the GPU counter draws count into
        /// until the next Begin or End of an occlusion-type query here.
        /// Null until a draw needs it.
        std::shared_ptr<graphics::GpuOcclusionCounter> occlusion_interval;
        /// The open PIPELINE_STATISTICS interval: the GPU record
        /// (graphics::STATS_RECORD_WORDS) the counting shaders and kernels
        /// add to until the next Begin or End of a statistics query here.
        std::shared_ptr<graphics::BackendBuffer> statistics_interval;
    };
