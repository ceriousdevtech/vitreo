// SPDX-License-Identifier: LGPL-2.1-only
// @file d3d11_manager_object_lifetime.inc
// @brief D3D11Manager class body, part 9 of 17: view clears, COM lifetime,
//   private data, desc marshaling, staging transfers, the release_* functions
//   and class linkage.
//
// A fragment of the D3D11Manager definition in d3d11.hpp, which
// includes it inside the class, in this order; not a standalone header.

    /// ClearUnorderedAccessViewUint / ClearUnorderedAccessViewFloat.
    /// @param view_address  The UAV interface
    /// @param values        The four clear values, as raw 32-bit patterns
    /// @param is_float_form true for the Float entry point (diagnostic only:
    ///                      the stored bits are the same either way)
    /// @return true if the resource behind the view was cleared
    /// Write @p element repeated over @p span bytes at @p begin on the GPU
    /// timeline (@p span a multiple of its size), ordered after the queued
    /// work that still reads or writes the destination: a backend fill when
    /// the element is one repeated byte (every zero clear) and the backend
    /// takes the range, else a copy from a transient pattern buffer. A CPU
    /// memcpy into the buffer's live storage lands before that work runs
    /// (measured: Satisfactory's FFT bloom read a UAV counter cleared
    /// "before" the reduction that filled it and produced an all-NaN
    /// spectrum).
    [[nodiscard]] bool fill_buffer_on_gpu(Buffer& buffer, usize begin,
                                          std::span<const u8> element,
                                          usize span);
    /// @p element repeated over the @p span bytes at @p dst (@p span a
    /// multiple of its size), in O(log) copies rather than one per element.
    static void replicate_element(u8* dst, usize span,
                                  std::span<const u8> element) noexcept;
    [[nodiscard]] bool clear_unordered_access_view(
        GuestAddr view_address, const std::array<u32, 4>& values,
        bool is_float_form);
    /// Store one encoded texel over a view's window of `texture`: mip `mip`,
    /// array slices (or, for a volume, W slices) `first_slice` ..
    /// `first_slice + slice_count - 1`, and optionally only the ClearView
    /// rectangle `rect` = {left, top, right, bottom}. `method` names the D3D
    /// entry point for a fail-loudly line.
    [[nodiscard]] bool fill_texture_view_texels(
        Texture2D& texture, const u8* texel, u32 texel_bytes, u32 mip,
        u32 first_slice, u32 slice_count, const std::array<u32, 4>* rect,
        const char* method);
    [[nodiscard]] u32 release(GuestAddr interface_address);
    void destroy_if_unreferenced(GuestAddr owner);
    [[nodiscard]] u32 query_interface(Marshaler& m,
                                      GuestAddr interface_address);
    [[nodiscard]] u32 select_feature_level(GuestAddr levels,
                                           u32 level_count,
                                           bool& invalid) const;
    /// The parameter rules D3D11CreateDevice and
    /// D3D11CreateDeviceAndSwapChain share: SDK version, creation flags,
    /// driver type against pAdapter and Software, and the feature-level
    /// array. Returns S_OK with the selected level, or the failing HRESULT.
    [[nodiscard]] u32 validate_device_request(
        GuestAddr adapter, u32 driver_type, GuestAddr software, u32 flags,
        GuestAddr levels, u32 level_count, u32 sdk_version,
        u32& selected_level) const;
    /// Whether the PE image at @p module exports a Direct3D 10+ user-mode
    /// driver entry point (OpenAdapter10 / OpenAdapter10_2), which is what
    /// D3D_DRIVER_TYPE_SOFTWARE loads from its Software module.
    [[nodiscard]] bool module_exports_user_mode_driver(GuestAddr module) const;
    /// Get/SetPrivateData[Interface] of any object or facet (the contract is
    /// at set_private_data_interface in d3d11.cpp). @p owner is the object
    /// the store belongs to -- its primary interface -- and keys its
    /// interface entries; 0 means the interface the call came in on, which
    /// is right for every object's own vtable and wrong for a facet.
    [[nodiscard]] u32 set_private_data(
        Marshaler& m, PrivateDataStore& private_data, GuestAddr owner = 0);
    [[nodiscard]] u32 get_private_data(
        Marshaler& m, PrivateDataStore& private_data, GuestAddr owner = 0);
    [[nodiscard]] bool read_buffer_desc(GuestAddr address,
                                        BufferDesc& desc) const;
    [[nodiscard]] bool write_buffer_desc(GuestAddr address,
                                         const BufferDesc& desc);
    /// `SetPrivateDataInterface`, shared by the device and every device child.
    [[nodiscard]] u32 set_private_data_interface(
        Marshaler& m, PrivateDataStore& private_data, GuestAddr owner = 0);
    /// Release every guest interface retained against @p address and forget
    /// them. Called from free_object, after the record has already been erased,
    /// because a guest Release may re-enter this manager.
    using PrivateInterface = std::shared_ptr<GuestAddr>;
    [[nodiscard]] u32 guest_reference(GuestAddr address, usize slot);
    [[nodiscard]] PrivateInterface own_private_interface(GuestAddr address);
    void release_private_data_interfaces(GuestAddr address);

    [[nodiscard]] bool read_texture_desc(GuestAddr address,
                                         Texture2DDesc& desc) const;
    [[nodiscard]] bool write_texture_desc(GuestAddr address,
                                          const Texture2DDesc& desc);
    [[nodiscard]] bool read_swap_chain_desc(GuestAddr address,
                                            SwapChainDesc& desc) const;
    [[nodiscard]] bool write_swap_chain_desc(GuestAddr address,
                                             const SwapChainDesc& desc);
    [[nodiscard]] graphics::PixelFormat backend_format(u32 dxgi_format) const;
    [[nodiscard]] graphics::PixelFormat backend_texture_format(
        u32 dxgi_format, u32 bind_flags) const;
    /// Record a GPU -> staging copy of one source level into `destination`'s
    /// subresource `dst_sub` without waiting for it (the box: element origin
    /// and extent in each). False when the backend cannot defer it.
    [[nodiscard]] bool defer_staging_readback(
        Texture2D& destination, u32 dst_sub, Texture2D& source, u32 src_mip,
        u32 src_slice, u32 src_ex, u32 src_ey, u32 dst_ex, u32 dst_ey,
        u32 elements_w, u32 elements_h);
    /// True when every deferred copy into the subresource has executed.
    [[nodiscard]] bool staging_readbacks_ready(Texture2D& texture, u32 sub);
    /// Wait for and fold in every deferred copy into the subresource (every
    /// subresource when `sub` is ~0u), before its shadow is read or written.
    void resolve_staging_readbacks(Texture2D& texture, u32 sub = ~0u);
    /// UpdateSubresource into a volume or block-compressed texture: the box
    /// only, reading nothing past it.
    [[nodiscard]] bool update_texture_box(Texture2D& texture, u32 subresource,
                                          GuestAddr box_address, GuestAddr data,
                                          u32 row_pitch, u32 depth_pitch);
    /// CopySubresourceRegion between two textures. Null on success, else
    /// why the call was dropped, for the diagnostic line.
    [[nodiscard]] const char* copy_texture_region(
        Texture2D& destination, u32 dst_sub, u32 dst_x, u32 dst_y, u32 dst_z,
        Texture2D& source, u32 src_sub, const std::array<u32, 6>* box);
    [[nodiscard]] bool copy_texture_resource(Texture2D& destination,
                                             Texture2D& source);
    /// One subresource of a packed depth+stencil texture in its guest-visible
    /// staging layout (4 or 8 bytes a texel), read from the host's two planes.
    /// The inverse of read_packed_depth_stencil: split a guest-layout packed
    /// depth+stencil level (D24_UNORM_S8_UINT words, or D32_FLOAT_S8X24
    /// pairs) into the host's depth and stencil planes of `destination`.
    [[nodiscard]] bool write_packed_depth_stencil(Texture2D& destination,
                                                  u32 mip, u32 slice,
                                                  const u8* in,
                                                  usize in_pitch);
    [[nodiscard]] bool read_packed_depth_stencil(const Texture2D& source,
                                                 u32 mip, u32 slice, u8* out,
                                                 usize out_pitch);
    /// Read the pixels one D3D11_SUBRESOURCE_DATA record names (record
    /// @p index of the array at @p initial_data; 12 bytes on x86, 16 on x64)
    /// into a tight @p row_bytes x @p rows x @p depth layout. Learn
    /// D3D11_SUBRESOURCE_DATA: only the bytes the subresource occupies are
    /// read, so a pitch smaller than a row (rows overlap) is honoured and a
    /// last row ending at the end of its allocation is not over-read. Fails
    /// (E_INVALIDARG) for a NULL pSysMem, or a zero pitch the dimension uses.
    [[nodiscard]] bool read_subresource_data(GuestAddr initial_data, u32 index,
                                             usize row_bytes, u32 rows,
                                             u32 depth, bool uses_row_pitch,
                                             bool uses_slice_pitch,
                                             std::vector<u8>& tight) const;
    /// Write one tight subresource of initial data into a newly created
    /// texture: the CPU copy of a STAGING texture, both planes of a packed
    /// depth+stencil one, else the backend texture.
    [[nodiscard]] bool upload_initial_subresource(Texture2D& texture, u32 mip,
                                                  u32 slice, const u8* data,
                                                  usize row_bytes, u32 rows);
    [[nodiscard]] u32 release_buffer(GuestAddr address);
    [[nodiscard]] u32 release_texture(GuestAddr address);
    [[nodiscard]] u32 release_shader_resource_view(GuestAddr address);
    [[nodiscard]] u32 release_unordered_access_view(GuestAddr address);
    [[nodiscard]] u32 release_render_target_view(GuestAddr address);
    [[nodiscard]] u32 release_depth_stencil_view(GuestAddr address);
    [[nodiscard]] u32 release_query(GuestAddr address);
    [[nodiscard]] u32 release_fence(GuestAddr address);
    [[nodiscard]] u32 release_command_list(GuestAddr address);
    [[nodiscard]] u32 release_class_linkage(GuestAddr address);
    [[nodiscard]] u32 release_class_instance(GuestAddr address);
    /// ID3D11Device::CreateClassLinkage and the linkage's two creators.
    void create_class_linkage(Marshaler& m, GuestAddr self);
    /// The pClassLinkage argument of every Create*Shader: NULL, or a live
    /// linkage of @p device_owner.
    [[nodiscard]] bool valid_class_linkage(GuestAddr device_owner,
                                           GuestAddr linkage) const;
    /// Create*Shader's answer for bytecode that declares interfaces
    /// (requires_class_linkage): E_INVALIDARG without a linkage (measured,
    /// d3d11_classlink_probe [create]), S_OK with one.
    [[nodiscard]] u32 refuse_interface_shader(GuestAddr linkage,
                                              const char* method);
    /// A shader created with a linkage registers its IFCE class types and
    /// class instances with it, in chunk order (measured: InstanceId and
    /// TypeId number the names a linkage has seen, from shaders and from
    /// GetClassInstance / CreateClassInstance alike).
    void register_class_linkage(GuestAddr linkage,
                                const graphics::DxbcContainer& container);
    /// Records @p bytes' interfaces on a new shader object.
    void attach_shader_linkage(ShaderLinkage& linkage,
                               const std::vector<u8>& bytes,
                               const graphics::DxbcContainer& container);
    /// The *SetShader stages in ContextBindings::linked order.
    enum class LinkedStage : u8 {
        Vertex = 0, Pixel, Compute, Geometry, Hull, Domain,
    };
    [[nodiscard]] GuestAddr& stage_shader_field(ContextBindings& state,
                                                LinkedStage stage);
    /// Calls @p f with @p shader's record in @p stage's map; false when it
    /// has none (d3d11_class_instance_binding.cpp).
    template <typename F>
    bool visit_stage_record(LinkedStage stage, GuestAddr shader, F&& f);
    [[nodiscard]] ShaderLinkage* stage_shader_linkage(LinkedStage stage,
                                                      GuestAddr shader);
    [[nodiscard]] bool retain_stage_shader(LinkedStage stage, GuestAddr shader);
    void release_stage_shader(LinkedStage stage, GuestAddr shader);
    /// Every *SetShader(pShader, ppClassInstances, NumClassInstances): binds
    /// @p shader, or -- when its interfaces and the instances disagree
    /// (Learn: "the shader will get disabled"; measured) -- nothing.
    void set_stage_shader(GuestAddr context, LinkedStage stage,
                          GuestAddr shader, GuestAddr instances, u32 count);
    /// Releases what the stage's binding holds and clears it.
    void unbind_stage_shader(ContextBindings& state, LinkedStage stage);
    /// *GetShader(ppShader, ppClassInstances, pNumClassInstances).
    void get_stage_shader(Marshaler& m, const ContextBindings& state,
                          LinkedStage stage);
    /// The specialisation of @p shader for @p key (built on first use), or
    /// 0 when it cannot be built (logged).
    [[nodiscard]] GuestAddr specialise_shader(LinkedStage stage,
                                              GuestAddr shader,
                                              const std::vector<u32>& key);
    /// Destroys the specialisations a shader object owns.
    void destroy_specialisations(LinkedStage stage, ShaderLinkage& linkage);
