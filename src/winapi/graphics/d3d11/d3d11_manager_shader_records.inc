// SPDX-License-Identifier: LGPL-2.1-only
// @file d3d11_manager_shader_records.inc
// @brief D3D11Manager class body, part 3 of 17: shader records (signatures,
//   ShaderLinkage, the shader stages, stream-out declarations, InputLayout).
//
// A fragment of the D3D11Manager definition in d3d11.hpp, which
// includes it inside the class, in this order; not a standalone header.

    struct ShaderSignatureElement {
        std::string semantic_name;
        u32 semantic_index{0};
        u32 register_num{0};
        u8 component_type{3};
    };

    /// Dynamic shader linkage (D3D11.3 7.19) on a shader object. A program
    /// that declares interfaces runs only as a specialisation for the class
    /// instances *SetShader bound (set_stage_shader): a hidden object of the
    /// same kind, built from `bytecode` plus the binding once per binding and
    /// owned by this object (destroyed with it; it holds no public
    /// reference, and nothing hands its address to the application).
    struct ShaderLinkage {
        /// The program's IFCE chunk; null for a program without interfaces.
        std::shared_ptr<const graphics::DxbcClassLinkage> classes;
        std::vector<u8> bytecode;
        /// Specialisations by binding (per slot: table, this[4]).
        std::map<std::vector<u32>, GuestAddr> specialisations;
    };

    struct VertexShader {
        /// R4 ia-memo: process-unique, so a memo keyed by it never meets a
        /// later shader at a recycled address.
        u64 serial{d3d11::draw_memo::next_serial()};
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        std::vector<u8> bytecode;
        std::string msl_source;
        /// std::hash of msl_source (set once, at creation), worked out at the
        /// first draw; 0 = not yet. Every draw keys its pipeline on it.
        u64 msl_source_hash{0};
        /// DXBC ISGN: SemanticName+Index → register, which becomes the MSL
        /// [[attribute(n)]] / SPIR-V Location. CreateInputLayout must bind
        /// by this mapping, not by element-array order.
        std::vector<ShaderSignatureElement> input_signature;
        /// OSGN declares SV_ViewportArrayIndex: its draws carry the whole
        /// viewport/scissor array instead of entry 0.
        bool writes_viewport_index{false};
        d3d11::ShaderBindingLayout binding_layout{};
        /// Stream-out capture layout, worked out on first use: which output
        /// register each captured vec4 holds. 0 = not tried, 1 = ready,
        /// 2 = this shader cannot be captured (reason already logged).
        u8 capture_state{0};
        std::vector<u32> capture_registers;
        ShaderLinkage linkage;
        /// Where the capture records hold SV_Position and the clip distances
        /// (PIPELINE_STATISTICS CPrimitives); set with capture_registers.
        graphics::StatsClipLayout stats_clip{};
        /// Pass C drawing this shader's captured lines (key 2; 4 with
        /// adjacency) or wireframe triangles (3) as the rectangles D3D
        /// rasterizes (graphics::generate_line_quad_vertex_msl), built on
        /// first use; empty when its outputs cannot be drawn so.
        std::map<u32, std::string> line_quad_msl;
        PrivateDataStore private_data;
    };

    struct PixelShader {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        std::vector<u8> bytecode;
        std::string msl_source;
        /// As VertexShader::msl_source_hash.
        u64 msl_source_hash{0};
        std::string dual_source_msl;
        bool dual_source_compile_attempted{false};
        /// Variants, keyed by {border-twin mask | 0x10000 for dual-source |
        /// 0x20000 for the SampleMask-writing form, with the
        /// rgb32_buffer_srv_mask in the high 32 bits; the
        /// single_sample_ms_srv_mask | 1 << 32 for the line-coverage form}.
        /// An empty string records a failed translation.
        std::map<std::pair<u64, u64>, std::string> border_variants;
        /// Output-merger logic-op variants (DxbcTranslationOptions::logic_op),
        /// keyed by the border_variants key, the op and each target's
        /// component masks (logic_variant_key). An empty string records a
        /// failed translation.
        std::map<std::string, std::string> logic_variants;
        d3d11::ShaderBindingLayout binding_layout{};
        /// [earlydepthstencil] with UAV side effects at pixel frequency: on
        /// a multisampled target those are lost where the early test rejects
        /// a pixel's lowest covered sample (Draw logs it once).
        bool early_depth_uav_writes{false};
        /// Writes SV_Coverage (oMask), which disables alpha-to-coverage
        /// (D3D11.3 17.18).
        bool writes_coverage{false};
        /// Writes SV_Depth (or a conservative form), which the translated
        /// shader clamps to the viewport depth range (D3D11.3 15.6).
        bool writes_depth{false};
        ShaderLinkage linkage;
        PrivateDataStore private_data;
    };

    struct ComputeShader {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        std::vector<u8> bytecode;
        std::string msl_source;
        u32 workgroup_size_x{1};
        u32 workgroup_size_y{1};
        u32 workgroup_size_z{1};
        /// Variants by {border-twin mask, with the rgb32_buffer_srv_mask in
        /// the high 32 bits; the single_sample_ms_srv_mask}; empty =
        /// translation failed.
        std::map<std::pair<u64, u64>, std::string> border_variants;
        /// Compiled pipelines by the same key (null: the variant did not
        /// compile). Built once per variant, as draws are: building one per
        /// Dispatch was a Metal library and pipeline state on every call.
        std::map<std::pair<u64, u64>,
                 std::shared_ptr<graphics::BackendComputePipeline>>
            pipelines;
        d3d11::ShaderBindingLayout binding_layout{};
        ShaderLinkage linkage;
        PrivateDataStore private_data;
    };

    /// One validated D3D11_SO_DECLARATION_ENTRY.
    struct StreamOutDeclEntry {
        /// D3D11_SO_DECLARATION_ENTRY::Stream: the geometry-shader stream
        /// (0-3) the entry reads.
        u32 stream{0};
        u32 slot{0};
        /// Offset of the first component within the slot's vertex, in DWORDs.
        u32 dst_dword{0};
        /// A NULL SemanticName: skips `component_count` DWORDs, writes nothing.
        bool gap{false};
        /// Output register (o#) the semantic resolved to.
        u32 output_register{0};
        u32 start_component{0};
        u32 component_count{0};
    };

    /// ID3D11HullShader: lowered to a per-patch compute kernel, compiled per
    /// vertex-shader record layout like a geometry program's.
    struct HullShader {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        std::vector<u8> bytecode;
        u32 domain{0};
        u32 partitioning{0};
        u32 output_primitive{0};
        u32 input_control_points{0};
        u32 output_control_points{0};
        float max_tess_factor{64.0f};
        u32 cp_record_vec4s{0};
        u32 pc_record_vec4s{0};
        d3d11::ShaderBindingLayout binding_layout{};
        std::map<std::vector<u32>,
                 std::shared_ptr<graphics::BackendComputePipeline>>
            kernels;
        /// The same kernel writing raw float32 factors for the software
        /// tessellator, per vertex-shader record layout.
        std::map<std::vector<u32>,
                 std::shared_ptr<graphics::BackendComputePipeline>>
            software_kernels;
        ShaderLinkage linkage;
        PrivateDataStore private_data;
    };
    /// ID3D11DomainShader. Two ways to run it (.docs/design-tessellation-
    /// emulation.md): as the post-tessellation vertex function of Metal's
    /// tessellator (`vertex_key` is the hidden VertexShader entry, set when
    /// the domain is tri or quad), and as a compute kernel over the software
    /// tessellator's points, writing records of `record_registers` that the
    /// pass-through rasterizes and stream output / a geometry shader read.
    struct DomainShader {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        u32 domain{0};
        u32 input_control_points{0};
        GuestAddr vertex_key{0};
        std::string compute_msl;
        d3d11::ShaderBindingLayout compute_layout{};
        std::vector<u32> record_registers;
        std::shared_ptr<graphics::BackendComputePipeline> compute_kernel;
        bool compute_kernel_built{false};
        /// The bytecode, and the compute kernel's variants reducing through
        /// MINIMUM_ / MAXIMUM_ samplers, per s# mask (built on first use).
        std::vector<u8> bytecode;
        std::map<u32, std::shared_ptr<graphics::BackendComputePipeline>>
            minmax_kernels;
        /// Empty without an SV_Position (such a domain shader only streams).
        std::string passthrough_msl;
        bool writes_viewport_index{false};
        ShaderLinkage linkage;
        /// Where its records hold SV_Position and the clip distances
        /// (PIPELINE_STATISTICS CPrimitives).
        graphics::StatsClipLayout stats_clip{};
        PrivateDataStore private_data;
    };

    /// ID3D11GeometryShader. Two kinds share the interface:
    ///  - CreateGeometryShaderWithStreamOutput over VERTEX bytecode: a
    ///    pass-through stage whose whole job is stream output (`program` false);
    ///  - CreateGeometryShader: a geometry PROGRAM, run by the compute
    ///    emulation in .docs/design-geometry-shader-emulation.md;
    ///  - CreateGeometryShaderWithStreamOutput over GEOMETRY bytecode: a
    ///    program that also streams its expansion (both field sets).
    struct GeometryShader {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        bool program{false};
        std::vector<u8> bytecode;
        /// Vertices per input primitive: 1, 2, 3, or 4 / 6 with adjacency.
        u32 input_primitive{0};
        /// D3D10_SB_PRIMITIVE_TOPOLOGY of the output: 1 point list,
        /// 3 line strip, 5 triangle strip. Expanded to independent primitives.
        u32 output_topology{0};
        u32 max_output_vertices{0};
        /// [instance(n)]: invocations per input primitive.
        u32 instance_count{1};
        /// vec4s per expanded vertex record (output registers), and which
        /// o# register each one holds (ascending).
        u32 output_record_vec4s{0};
        std::vector<u32> output_record_registers;
        /// Pass C: draws the expanded records.
        std::string passthrough_msl;
        /// The rasterized stream's record layout as pass C reads it, and
        /// the pass C that draws its lines -- or its triangles in
        /// WIREFRAME fill -- as the rectangles D3D rasterizes
        /// (graphics::generate_line_quad_vertex_msl), built on first use.
        std::vector<graphics::GsPassthroughOutput> passthrough_outputs;
        std::string line_quad_msl;
        /// OSGN declares SV_ViewportArrayIndex: pass C carries the whole
        /// viewport/scissor array.
        bool writes_viewport_index{false};
        d3d11::ShaderBindingLayout binding_layout{};
        /// The expansion kernel depends on the vertex shader's record layout,
        /// so it is built per layout. A null entry records a failed build.
        std::map<std::vector<u32>,
                 std::shared_ptr<graphics::BackendComputePipeline>>
            kernels;
        std::vector<StreamOutDeclEntry> stream_out;
        std::array<u32, 4> stream_out_strides{};
        /// Slots each stream's declaration writes (bit n = slot n; a slot
        /// takes one stream only). A stream writes NOTHING when one of its
        /// slots has no buffer bound; the other streams are unaffected
        /// (measured, d3d11_stream_out_probe / d3d11_gsso_probe).
        std::array<u32, 4> stream_out_slots{};
        /// The stream that is rasterized: 0-3, or
        /// D3D11_SO_NO_RASTERIZED_STREAM. CreateGeometryShader rasterizes
        /// stream 0 (D3D11.3 13.5.2). A stream the shader does not produce
        /// (any but 0 of a vertex shader) rasterizes nothing.
        u32 rasterized_stream{0};
        /// Streams the program declares (dcl_stream; bit 0 alone without).
        u32 stream_mask{1};
        ShaderLinkage linkage;
        /// Per stream: where the expansion records hold SV_Position and the
        /// clip distances (PIPELINE_STATISTICS CPrimitives), worked out on
        /// first use.
        bool stats_clip_ready{false};
        std::array<graphics::StatsClipLayout, 4> stats_clip{};
        PrivateDataStore private_data;
    };

    struct InputLayoutElement {
        std::string semantic_name;
        u32 semantic_index{0};
        u32 format{0};
        u32 input_slot{0};
        u32 aligned_byte_offset{0};
        u32 input_slot_class{0};
        u32 instance_data_step_rate{0};
    };

    struct InputLayout {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        std::vector<InputLayoutElement> elements;
        /// Which backend input stream serves each of the 32 IA slots, or
        /// kNoBackendStream. A layout may name any slot 0-31, but the backend
        /// has BACKEND_MAX_VERTEX_BUFFERS streams: slots below that keep their
        /// own stream, and a higher slot takes a stream no lower slot of THIS
        /// layout uses. Fixed at creation, so every draw with the layout maps
        /// identically (the pipeline cache keys on the stream indices).
        static constexpr u8 kNoBackendStream = 0xFF;
        std::array<u8, d3d11::VERTEX_BUFFER_SLOTS> backend_stream{};
        /// The inverse: the IA slot each backend stream reads, or
        /// kNoBackendStream for a stream the layout leaves unused.
        std::array<u8, graphics::BACKEND_MAX_VERTEX_BUFFERS> stream_slot{};
        PrivateDataStore private_data;
        // ---- R4 ia-memo (d3d11_draw_memo.hpp)
        /// What execute_draw made of the elements against the input
        /// signature of the vertex shader with `vs_serial`.
        struct IaMemo {
            bool valid{false};
            u64 vs_serial{0};
            bool unservable{false};
            u32 unservable_slot{0};
            std::vector<graphics::BackendVertexAttribute> attributes;
            std::array<bool, graphics::BACKEND_MAX_VERTEX_BUFFERS>
                per_instance{};
            std::array<unsigned, graphics::BACKEND_MAX_VERTEX_BUFFERS>
                step_rates{};
            std::array<unsigned, graphics::BACKEND_MAX_VERTEX_BUFFERS>
                inferred{};
        };
        IaMemo ia_memo;
        // ---- end R4
    };
