// SPDX-License-Identifier: LGPL-2.1-only
// @file d3d11_manager_draw_emulation.inc
// @brief D3D11Manager class body, part 13 of 17: draws and their emulation
//   (execute_draw, geometry and tessellation emulation, vertex capture).
//
// A fragment of the D3D11Manager definition in d3d11.hpp, which
// includes it inside the class, in this order; not a standalone header.

    /// Build BackendPipelineDesc from bound state and submit a draw. Void
    /// Draw* APIs still return to the guest even when submission fails; logs
    /// record the reason (no fabricated GPU success).
    ///
    /// A non-zero @p indirect_args names an ID3D11Buffer whose contents supply
    /// the counts instead; @p count, @p start_location, @p base_vertex,
    /// @p instance_count and @p first_instance are then ignored.
    /// A stream-out capture draw: execute_draw runs the bound vertex shader's
    /// capture variant with rasterization off, into @p target (a private 1x1
    /// target, since a streaming draw commonly has no render target at all),
    /// writing each vertex's outputs to @p buffer at @p offset.
    /// A draw execute_draw issues on the guest's behalf, rather than the
    /// guest's own:
    ///  - Capture: the bound vertex shader's capture variant, rasterization
    ///    off, into `target` (private 1x1: a streaming draw commonly binds no
    ///    render target), writing each vertex's outputs to `buffer`+`offset`.
    ///  - GeometryOutput: pass C of the geometry emulation -- `vertex_msl`
    ///    (the generated pass-through shader) reads the expanded records from
    ///    `buffer` as its t0, with the guest's pixel shader, targets and
    ///    states, drawing a `topology` list whose vertex count comes from
    ///    `indirect_args`.
    struct DrawVariant {
        enum class Kind : u8 { Capture, GeometryOutput, Tessellated };
        Kind kind{Kind::Capture};
        graphics::BackendBuffer* buffer{nullptr};
        unsigned offset{0};
        graphics::BackendTexture2D* target{nullptr};
        /// Capture of an indexed draw: these 32-bit index values instead of
        /// the bound index buffer (the draw's own values minus strip cuts).
        graphics::BackendBuffer* capture_indices{nullptr};
        u32 capture_index_count{0};
        const std::string* vertex_msl{nullptr};
        /// Capture of instance k of an instanced draw, drawn alone at
        /// StartInstanceLocation + k: the SV_InstanceID the shader must see.
        u32 instance_id_bias{0};
        /// Capture of an indirect draw, all instances at once: records keyed
        /// draw-relative behind the header the GPU resolve wrote
        /// (DxbcTranslationOptions::capture_draw_relative).
        bool capture_relative{false};
        /// GeometryOutput: the expansion's per-invocation vertex counts (the
        /// pass-through's t1; null for dense records), and how many vertex
        /// slots -- or, with `index_buffer`, indices -- to draw.
        graphics::BackendBuffer* counts{nullptr};
        unsigned counts_offset{0};
        u32 vertex_count{0};
        /// GeometryOutput of dense records (tessellated): the tessellator's
        /// 32-bit connectivity, and the GPU-written argument block of the
        /// draw (indexed when `index_buffer` is set).
        graphics::BackendBuffer* index_buffer{nullptr};
        unsigned index_offset{0};
        IndirectDraw indirect{};
        /// GeometryOutput: the shader writes SV_ViewportArrayIndex.
        bool viewport_array{false};
        /// Tessellated (11.17): the patch draw of the hull kernel's output --
        /// factors, the control-point / control / patch-constant buffers the
        /// domain shader reads (bound at 22 + binding 1 / 2 / 4), and the
        /// pipeline's tessellator state.
        graphics::BackendBuffer* tess_factors{nullptr};
        graphics::BackendBuffer* tess_control_points{nullptr};
        graphics::BackendBuffer* tess_control{nullptr};
        graphics::BackendBuffer* tess_patch_constants{nullptr};
        u32 patch_count{0};
        u32 patch_control_points{0};
        graphics::BackendTessellationPartition tess_partition{
            graphics::BackendTessellationPartition::Integer};
        bool tess_clockwise{true};
        u32 tess_max_factor{64};
        graphics::BackendPrimitiveTopology topology{
            graphics::BackendPrimitiveTopology::TriangleList};
        /// GeometryOutput drawing lines as rectangles
        /// (graphics::generate_line_quad_vertex_msl): its LineQuadParams,
        /// bound at the pass's t2; Metal triangles per D3D primitive (2 a
        /// line, 6 a wireframe triangle; 0 = not such a pass), which
        /// SV_PrimitiveID is divided by; and whether the pixel shader
        /// scales o0.a by the alpha-antialiased line's coverage.
        graphics::BackendBuffer* line_params{nullptr};
        u32 line_quad_divisor{0};
        bool line_coverage_alpha{false};
        /// Set once the draw reached the backend. Nothing downstream may run
        /// over a capture or an expansion that never happened.
        bool submitted{false};
    };
    /// @p host_indirect: an argument block of Vitreo's own (DrawAuto, an
    /// emulation pass) in place of @p indirect_args / @p indirect_offset.
    /// Whether a guest draw on @p context may compile its pipeline with the
    /// device lock given up (PureWorkUnlock). The immediate context is not
    /// thread-safe by contract (Learn, ID3D11DeviceContext), so only device
    /// methods can run meanwhile -- unless ID3D11Multithread protection is
    /// on, which makes each context call atomic, or the manager drives a
    /// second device, whose immediate context shares this manager's draw
    /// memos. VITREO_D3D11_DRAW_UNLOCK=0 keeps the lock (A/B).
    [[nodiscard]] bool draw_compile_may_release_lock(GuestAddr context);
    void execute_draw(GuestAddr context, bool indexed, u32 count,
                      u32 start_location, i32 base_vertex,
                      u32 instance_count = 1, u32 first_instance = 0,
                      GuestAddr indirect_args = 0, u32 indirect_offset = 0,
                      DrawVariant* variant = nullptr,
                      const IndirectDraw& host_indirect = {});
    /// What /tmp/vitreo-draw-dump needs of the call being made.
    struct DrawDumpCall {
        bool indexed{false};
        u32 count{0};
        u32 start_location{0};
        i32 base_vertex{0};
        u32 instance_count{1};
        u32 first_instance{0};
        GuestAddr indirect_args{0};
        u32 indirect_offset{0};
    };
    /// /tmp/vitreo-draw-dump (d3d11_draw_dump.cpp, DIAGNOSTIC, log only):
    /// when the marker's contents select this draw (its pixel shader's input
    /// signature names `ps_semantic`, within the `max` budget), write its
    /// inputs to the log as `[draw-dump]` lines. execute_draw calls it only
    /// while the marker is present.
    void log_draw_dump(const ContextState& state, const DrawDumpCall& call);
    // ---- P0 state census (VITREO_D3D11_STATE_CENSUS=1, DIAGNOSTIC;
    // d3d11_state_census.cpp). execute_draw calls these only when
    // d3d11_census::enabled; they read state and count, nothing else.
    /// A guest draw entered execute_draw (no emulation variant).
    void census_guest_draw(GuestAddr context, const ContextState& state,
                           bool indexed, bool indirect);
    /// A draw is about to reach the backend (@p emulation: an emulation
    /// pass's own draw).
    void census_backend_draw(GuestAddr context, bool emulation,
                             u64 pipeline_key,
                             const graphics::BackendRenderPassDesc& pass,
                             const graphics::BackendDrawCall& call);
    /// Each device and its SetMultithreadProtected flag. Device lock held.
    [[nodiscard]] std::vector<std::pair<GuestAddr, bool>>
    census_device_protection() const;
    // ---- end P0 state census
    /// The geometry emulation's pass B and C input: records of the stage
    /// before (a vertex capture, a domain shader's output) and the
    /// assembled primitives as 32-bit record indices, `vertices_per_primitive`
    /// each.
    struct GeometryInput {
        graphics::BackendBuffer* records{nullptr};
        u32 records_offset{0};
        graphics::BackendBuffer* ids{nullptr};
        u32 ids_offset{0};
        u32 vertices_per_primitive{1};
        /// Primitives: exact, or -- with `primitive_count`, a GPU word --
        /// the most there can be (the buffers are sized for it).
        u64 max_primitives{0};
        const graphics::BackendBuffer* primitive_count{nullptr};
        u32 primitive_count_offset{0};
        /// Nonzero: primitives per draw instance, SV_PrimitiveID restarting
        /// with each (D3D11.3 8.17); or that count as a GPU word.
        u32 primitives_per_instance{0};
        const graphics::BackendBuffer* primitives_per_instance_count{nullptr};
        u32 primitives_per_instance_offset{0};
        /// SV_PrimitiveID from a table in `ids`, at this word offset (a
        /// tessellated primitive carries its patch's id, 11.7.9.1).
        bool primitive_id_table{false};
        u32 primitive_id_table_offset{0};
    };
    /// The expansion kernel of @p geometry_shader for a record layout, built
    /// on first use; null when it cannot be.
    [[nodiscard]] std::shared_ptr<graphics::BackendComputePipeline>
    geometry_kernel_for(GuestAddr geometry_shader,
                        const std::vector<u32>& record_layout,
                        u32 minmax_samplers = 0);
    /// Passes B and C: expand @p in, stream each stream the declaration
    /// names, and draw the rasterized one. False (with a diagnostic) when
    /// the work could not be recorded.
    [[nodiscard]] bool geometry_expand(
        GuestAddr context, GuestAddr geometry_shader,
        const std::shared_ptr<graphics::BackendComputePipeline>& kernel,
        const GeometryInput& in,
        const std::shared_ptr<graphics::BackendBuffer>& stats);
    /// An indirect draw's input assembly and vertex capture, resolved on the
    /// GPU timeline (graphics/shaders/ia_resolve.hpp): `resolved` holds the
    /// IA_RESOLVED_* words, `ids` the assembled record keys, `capture` the
    /// draw-relative records behind their header.
    struct IndirectCapture {
        std::shared_ptr<graphics::BackendBuffer> resolved;
        std::shared_ptr<graphics::BackendBuffer> ids;
        std::shared_ptr<graphics::BackendBuffer> capture;
        u32 record_capacity{0};
        u32 primitive_capacity{0};
    };
    /// Resolve and capture an indirect draw for @p vpp-vertex primitives
    /// assembled as @p assembly (0 list, 1 strip, 2 strip with adjacency).
    /// @p max_primitives bounds what the caller's passes can hold. False,
    /// with a diagnostic, when the work could not be recorded.
    [[nodiscard]] bool capture_indirect(GuestAddr context, bool indexed,
                                        const IndirectDraw& indirect, u32 vpp,
                                        u32 assembly, u32 record_vec4s,
                                        u64 max_primitives,
                                        IndirectCapture& out);
    /// Read the overflow flags of completed indirect resolves and raise the
    /// capacity a dropped draw asked for (never waits).
    void poll_indirect_feedback();
    /// An indirect draw through a geometry program: the counts resolved on
    /// the GPU timeline.
    void execute_geometry_indirect(
        GuestAddr context, bool indexed, const IndirectDraw& indirect,
        const std::shared_ptr<graphics::BackendComputePipeline>& kernel,
        const std::vector<u32>& record_layout,
        const std::shared_ptr<graphics::BackendBuffer>& stats);
    /// A direct draw through a geometry program, every instance at once: one
    /// draw-relative capture, one expansion over (primitive x instance), one
    /// draw -- three passes per draw instead of three per instance. Primitive
    /// assembly runs on the CPU through graphics::ia_resolve_model, the C++
    /// form of the indirect path's GPU resolve, so both paths key, order and
    /// number primitives identically. False, with a diagnostic, when the
    /// work could not be recorded.
    [[nodiscard]] bool execute_geometry_direct(
        GuestAddr context, bool indexed, u32 count, u32 start_location,
        i32 base_vertex, u32 instance_count, u32 first_instance,
        const std::shared_ptr<graphics::BackendComputePipeline>& kernel,
        const std::vector<u32>& record_layout,
        const std::shared_ptr<graphics::BackendBuffer>& stats);
    /// A direct draw's vertices captured for an emulation pass: records
    /// keyed by vertex id, `record_bytes` per instance, and the primitives
    /// assembled as record ids. prepare_direct_capture sizes and allocates
    /// it; capture_direct_instance records one instance's capture draw.
    struct DirectCapture {
        // Transient (gs-ring): back to m_transient when the draw is done.
        TransientBufferRing::Buffer records;
        TransientBufferRing::Buffer ids;
        TransientBufferRing::Buffer capture_indices;
        u32 capture_index_count{0};
        u64 record_bytes{0};
        u32 primitives{0};
    };
    /// Null when @p out is ready (with no primitives, nothing to draw), else
    /// why the draw cannot be captured.
    [[nodiscard]] const char* prepare_direct_capture(
        const ContextState& state, bool indexed, u32 count,
        u32 start_location, i32 base_vertex, u32 instance_count, u32 vpp,
        graphics::PrimitiveAssembly assembly, u32 record_vec4s,
        DirectCapture& out);
    [[nodiscard]] bool capture_direct_instance(
        GuestAddr context, bool indexed, u32 count, u32 start_location,
        i32 base_vertex, u32 first_instance, u32 instance,
        const DirectCapture& capture);
    /// The rasterizer state's line rendering (Learn D3D11_RASTERIZER_DESC):
    /// 0 aliased, 1 alpha-antialiased (AntialiasedLineEnable alone), 2
    /// quadrilateral (MultisampleEnable).
    [[nodiscard]] u32 line_rasterization_mode(const ContextState& state) const;
    /// The LineQuadParams of a line-quad pass C in @p mode over
    /// @p primitives primitives; null when it cannot be allocated.
    [[nodiscard]] TransientBufferRing::Buffer line_quad_params(
        const ContextState& state, u32 mode, u32 primitives);
    /// A direct draw of lines, or of triangles in WIREFRAME fill, in a line
    /// mode other than aliased, with no geometry program or tessellation:
    /// the vertex shader's capture, then pass C drawing each line as the
    /// rectangle D3D rasterizes (graphics::generate_line_quad_vertex_msl).
    /// False, having drawn nothing, when it cannot be served that way.
    [[nodiscard]] bool execute_line_quads(GuestAddr context, bool indexed,
                                          u32 count, u32 start_location,
                                          i32 base_vertex, u32 instance_count,
                                          u32 first_instance, u32 mode);
    /// A draw with a geometry PROGRAM bound: capture, expand, draw (11.16).
    void execute_geometry(GuestAddr context, bool indexed, u32 count,
                          u32 start_location, i32 base_vertex,
                          u32 instance_count, u32 first_instance,
                          const IndirectDraw& indirect);
    /// ID3D11Device::CreateGeometryShader (device slot 13).
    void create_geometry_shader(Marshaler& m, GuestAddr self);
    /// ID3D11Device::CreateHullShader / CreateDomainShader (slots 16 / 17).
    void create_tessellation_shader(Marshaler& m, GuestAddr self, bool hull);
    /// HS/DS SetShader, Get*, and the resource slots (context 59-66, 97-104).
    void context_tessellation_method(Marshaler& m, GuestAddr self, usize slot);
    /// A draw with a hull and domain shader bound: capture, hull kernel,
    /// patch draw (11.17; .docs/design-tessellation-emulation.md).
    void execute_tessellation(GuestAddr context, bool indexed, u32 count,
                              u32 start_location, i32 base_vertex,
                              u32 instance_count, u32 first_instance,
                              const IndirectDraw& indirect);
    /// Lower a geometry PROGRAM and fill @p shader's program fields (11.16).
    /// S_OK, or the HRESULT creation returns with @p why.
    /// @p rasterized_stream: the stream pass C draws (0-3), or
    /// D3D11_SO_NO_RASTERIZED_STREAM.
    /// CreateHullShader / CreateDomainShader's program pieces from @p bytes
    /// (also a class-linkage specialisation's): S_OK, or E_NOTIMPL and
    /// @p why when the tessellation emulation cannot run the program.
    [[nodiscard]] u32 build_tessellation_program(
        const std::vector<u8>& bytes, const graphics::DxbcContainer& container,
        bool hull, HullShader& hull_shader, DomainShader& domain_shader,
        VertexShader& domain_vertex, bool& hardware_domain, std::string& why);
    [[nodiscard]] u32 build_geometry_program(
        const std::vector<u8>& bytes, const graphics::DxbcContainer& container,
        u32 rasterized_stream, GeometryShader& shader, std::string& why);
    /// Lower a geometry shader to its expansion kernel for one vertex-shader
    /// record layout. False, with the reason logged, when it cannot be.
    [[nodiscard]] bool compile_geometry_kernel(
        const std::vector<u8>& bytecode,
        const std::vector<u32>& input_record, std::string& msl_out,
        d3d11::ShaderBindingLayout* layout_out, bool tess_software = false,
        u32 minmax_samplers = 0);
    /// A domain shader lowered to the software tessellator's compute kernel;
    /// @p records receives its record layout.
    [[nodiscard]] bool compile_domain_kernel(
        const std::vector<u8>& bytecode, std::string& msl_out,
        d3d11::ShaderBindingLayout* layout_out, std::vector<u32>& records,
        std::string& why, u32 minmax_samplers = 0);
    /// A kernel cache key: the record layout, then (when @p minmax_samplers
    /// is not 0) a word no register number takes, carrying the MINIMUM_ /
    /// MAXIMUM_ sampler mask of the variant (DxbcTranslationOptions::
    /// minmax_samplers).
    [[nodiscard]] static std::vector<u32> kernel_variant_key(
        std::vector<u32> record_layout, u32 minmax_samplers) {
        if (minmax_samplers != 0) {
            record_layout.push_back(0x4D4D0000u | (minmax_samplers & 0xFFFFu));
        }
        return record_layout;
    }
    /// Tessellation on the software tessellator (the exact path): every
    /// domain and output primitive, stream output of the domain shader, a
    /// geometry shader after it, and indirect draws.
    void execute_tessellation_software(GuestAddr context, bool indexed,
                                       u32 count, u32 start_location,
                                       i32 base_vertex, u32 instance_count,
                                       u32 first_instance,
                                       const IndirectDraw& indirect);
    /// A domain shader lowered to a post-tessellation vertex function.
    [[nodiscard]] bool compile_domain_shader(
        const std::vector<u8>& bytecode, std::string& msl_out,
        d3d11::ShaderBindingLayout* layout_out, std::string& why);
    /// Which register each captured vec4 of @p vs holds; worked out once.
    [[nodiscard]] bool ensure_vertex_capture(VertexShader& vs);
    /// How far a draw's vertex ids reach, which sizes its capture, and how the
    /// expansion/repack resolves them. Returns nullptr, or why it cannot.
    struct CaptureExtent {
        u32 index_mode{0};  ///< 0 non-indexed, 1 16-bit, 2 32-bit
        u32 first{0};       ///< first vertex, or first index ELEMENT
        graphics::BackendBuffer* index_backend{nullptr};
        u64 max_vertex{0};
    };
    /// The index values an indexed draw reads (or first..first+count for a
    /// non-indexed one), from the index buffer's CPU copy. @p extent comes
    /// from capture_extent. False when the data is not readable.
    [[nodiscard]] bool draw_indices(const ContextState& state, bool indexed,
                                    u32 count, const CaptureExtent& extent,
                                    std::vector<u32>& out);
    [[nodiscard]] const char* capture_extent(const ContextState& state,
                                             bool indexed, u32 count,
                                             u32 start_location,
                                             i32 base_vertex,
                                             CaptureExtent& out);
