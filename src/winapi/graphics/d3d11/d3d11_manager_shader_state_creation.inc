// SPDX-License-Identifier: LGPL-2.1-only
// @file d3d11_manager_shader_state_creation.inc
// @brief D3D11Manager class body, part 11 of 17: shader and state-object
//   creation (state descs and pools, their release, shader translation to MSL).
//
// A fragment of the D3D11Manager definition in d3d11.hpp, which
// includes it inside the class, in this order; not a standalone header.

    /// A vertex, pixel or compute shader's translation to MSL, made before
    /// its object is: it reads only the bytecode and the backend's fixed
    /// capabilities, so Create*Shader runs it under a PureWorkUnlock.
    struct ShaderTranslation {
        bool translated{false};
        std::string msl;
        d3d11::ShaderBindingLayout layout{};
        u32 workgroup_size_x{0};
        u32 workgroup_size_y{0};
        u32 workgroup_size_z{0};
    };
    /// @p stage Vertex, Fragment or Compute.
    [[nodiscard]] ShaderTranslation translate_shader(
        const u8* bytecode, usize size, graphics::ShaderStage stage);
    /// @param hidden a class-linkage specialisation (ShaderLinkage): no
    ///   application reference, so it does not hold the device either.
    /// @param translation translate_shader's result for this bytecode, or
    ///   nullptr to translate here.
    [[nodiscard]] GuestAddr create_vertex_shader(
        GuestAddr device_owner, const void* bytecode, usize bytecode_length,
        bool hidden = false, ShaderTranslation* translation = nullptr);
    [[nodiscard]] GuestAddr create_pixel_shader(
        GuestAddr device_owner, const void* bytecode, usize bytecode_length,
        bool hidden = false, ShaderTranslation* translation = nullptr);
    [[nodiscard]] GuestAddr create_compute_shader(
        GuestAddr device_owner, const void* bytecode, usize bytecode_length,
        bool hidden = false, ShaderTranslation* translation = nullptr);
    [[nodiscard]] GuestAddr create_input_layout(
        GuestAddr device_owner, const std::vector<InputLayoutElement>& elements);
    [[nodiscard]] GuestAddr create_blend_state(
        GuestAddr device_owner, const BlendStateDesc& desc);
    [[nodiscard]] GuestAddr create_rasterizer_state(
        GuestAddr device_owner, const RasterizerStateDesc& desc);
    [[nodiscard]] GuestAddr create_depth_stencil_state(
        GuestAddr device_owner, const DepthStencilStateDesc& desc);
    [[nodiscard]] GuestAddr create_sampler_state(
        GuestAddr device_owner, const SamplerStateDesc& desc);

    /// D3D11_BLEND_DESC (@p desc1 false: 32-byte targets) or
    /// D3D11_BLEND_DESC1 (40-byte targets with LogicOpEnable/LogicOp) read
    /// from guest memory. RenderTargetWriteMask is a UINT8; the three padding
    /// bytes after it are not part of the description (measured,
    /// d3d11_a04_state_objects_probe "blend rt0 mask padding garbage").
    [[nodiscard]] bool read_blend_desc(GuestAddr address, bool desc1,
                                       BlendStateDesc& desc) const;
    /// The D3D11_BLEND_DESC(1) rules (see the definition). @p logic_ops is
    /// the device's OutputMergerLogicOp.
    [[nodiscard]] static bool blend_desc_valid(const BlendStateDesc& desc,
                                               bool logic_ops);
    /// The D3D11_RASTERIZER_DESC2 rules for a device at @p feature_level
    /// (DESC and DESC1 are DESC2 with the later fields 0).
    [[nodiscard]] static bool rasterizer_desc_valid(
        const RasterizerStateDesc& desc, u32 feature_level);

    /// The description Windows keeps for a state object -- what GetDesc
    /// reports and deduplication compares: every BOOL made 0 or 1 and every
    /// field the enable flags switch off reset to its default (measured,
    /// d3d11_a04_state_objects_probe "* getdesc").
    [[nodiscard]] static BlendStateDesc canonical_blend_desc(
        const BlendStateDesc& desc);
    [[nodiscard]] static RasterizerStateDesc canonical_rasterizer_desc(
        const RasterizerStateDesc& desc);
    [[nodiscard]] static DepthStencilStateDesc canonical_depth_stencil_desc(
        const DepthStencilStateDesc& desc);
    [[nodiscard]] static SamplerStateDesc canonical_sampler_desc(
        const SamplerStateDesc& desc);

    /// Deduplicating state-object creation from a validated description.
    /// Returns S_OK with @p state set -- the device's existing object for the
    /// same canonical description, AddRef'd, or a new one -- or
    /// D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS once the family already
    /// holds D3D11_REQ_*_OBJECT_COUNT_PER_DEVICE unique objects, or E_FAIL.
    /// Blend and Blend1 share one pool, and so do Rasterizer, 1 and 2: each
    /// family's description is its latest version.
    [[nodiscard]] u32 acquire_blend_state(GuestAddr device_owner,
                                          const BlendStateDesc& desc,
                                          GuestAddr& state);
    [[nodiscard]] u32 acquire_rasterizer_state(
        GuestAddr device_owner, const RasterizerStateDesc& desc,
        GuestAddr& state);
    [[nodiscard]] u32 acquire_depth_stencil_state(
        GuestAddr device_owner, const DepthStencilStateDesc& desc,
        GuestAddr& state);
    [[nodiscard]] u32 acquire_sampler_state(GuestAddr device_owner,
                                            const SamplerStateDesc& desc,
                                            GuestAddr& state);
    /// The pool lookup and unique-object limit the four acquire_* share.
    /// Returns S_OK with @p existing set on a hit (not yet AddRef'd), S_OK
    /// with 0 when a new object may be made, or the limit error.
    [[nodiscard]] u32 state_pool_lookup(u8 family, const std::string& key,
                                        const std::string& default_key,
                                        GuestAddr device_owner,
                                        GuestAddr& existing) const;
    void state_pool_insert(u8 family, GuestAddr device_owner,
                           const std::string& key, GuestAddr state);
    void state_pool_erase(u8 family, GuestAddr device_owner,
                          const std::string& key);
    [[nodiscard]] u32 release_vertex_shader(GuestAddr address);
    [[nodiscard]] u32 release_pixel_shader(GuestAddr address);
    [[nodiscard]] u32 release_compute_shader(GuestAddr address);
    [[nodiscard]] u32 release_geometry_shader(GuestAddr address);
    [[nodiscard]] u32 release_hull_shader(GuestAddr address);
    [[nodiscard]] u32 release_domain_shader(GuestAddr address);
    [[nodiscard]] u32 release_input_layout(GuestAddr address);
    [[nodiscard]] u32 release_blend_state(GuestAddr address);
    [[nodiscard]] u32 release_rasterizer_state(GuestAddr address);
    [[nodiscard]] u32 release_depth_stencil_state(GuestAddr address);
    [[nodiscard]] u32 release_sampler_state(GuestAddr address);
    /// @param layout_out When non-null, receives the register spaces the shader
    ///   declares and whether it was compiled to read descriptor tables.
    /// @param border_twin_samplers s0..s7 needing the border-twin emulation
    ///   (DxbcTranslationOptions::border_twin_samplers).
    /// The bytecode declares interfaces (fcall/dcl_interface): creation
    /// needs an ID3D11ClassLinkage (refuse_interface_shader).
    [[nodiscard]] static bool requires_class_linkage(
        const std::vector<u8>& bytes);
    /// True when the device behind @p device_interface is below feature
    /// level 11_1 and @p bytecode needs one of its UAV features: a vertex,
    /// hull, domain or geometry program declaring a UAV (Learn
    /// dcl_uav_typed/raw/structured: UAVs reach those stages only at 11_1;
    /// tools/oracle_probe/d3d11_stage_uav_probe.expected), or any program
    /// whose SFI0 flags require UAVs at every stage or the 64 UAV slots
    /// (D3D_SHADER_REQUIRES_64_UAVS: a u8..u63 register; measured E_INVALIDARG
    /// for a cs_5_0 u63 and a ps_5_0 u60 at 11_0, d3d11_fl111_probe "fl111
    /// at 11_0"). The runtime refuses such a shader with E_INVALIDARG and a
    /// NULL out-pointer.
    /// Why Create*Shader refuses @p bytecode on the device behind
    /// @p device_interface: not DXBC, a stage outside @p stages (empty: any),
    /// or a shader model above the feature level; nullptr when accepted.
    [[nodiscard]] const char* shader_bytecode_refused(
        GuestAddr device_interface, const u8* bytecode, usize size,
        std::initializer_list<graphics::ShaderStage> stages) const;
    [[nodiscard]] bool uav_requirements_unavailable(
        GuestAddr device_interface, const u8* bytecode, usize size) const;
    /// D3D11_FEATURE_DATA_D3D11_OPTIONS2.PSSpecifiedStencilRefSupported on a
    /// device of @p feature_level: whether a pixel shader may write
    /// SV_StencilRef (CreatePixelShader refuses one otherwise).
    [[nodiscard]] bool ps_specified_stencil_ref_supported(
        u32 feature_level) const;
    /// The output-merger logic op a pixel-shader variant applies itself
    /// (DxbcTranslationOptions::logic_op / logic_op_targets /
    /// logic_op_component_masks).
    struct LogicOpVariant {
        u32 op{0};
        u32 targets{0};
        std::array<std::array<u32, 4>, 8> component_masks{};
    };
    [[nodiscard]] bool compile_shader_to_msl(
        const u8* bytecode, usize size, bool is_vertex, std::string& msl_out,
        bool dual_source_blending = false,
        d3d11::ShaderBindingLayout* layout_out = nullptr,
        u32 border_twin_samplers = 0, bool apply_sample_mask = false,
        u32 rgb32_buffer_srvs = 0,
        const LogicOpVariant* logic_op = nullptr,
        bool count_pixel_invocations = false,
        u32 single_sample_ms_srvs = 0, bool line_coverage_alpha = false,
        u32 line_quad_primitive_divisor = 0, u32 minmax_samplers = 0);
    [[nodiscard]] bool compile_compute_shader_to_msl(
        const u8* bytecode, usize size, std::string& msl_out,
        u32& workgroup_size_x, u32& workgroup_size_y,
        u32& workgroup_size_z, d3d11::ShaderBindingLayout* layout_out = nullptr,
        u32 border_twin_samplers = 0, u32 rgb32_buffer_srvs = 0,
        u32 single_sample_ms_srvs = 0, u32 minmax_samplers = 0);
