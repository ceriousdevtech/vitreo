// SPDX-License-Identifier: LGPL-2.1-only
// @file d3d11_manager_object_creation.inc
// @brief D3D11Manager class body, part 7 of 17: object storage and creation
//   (resource and view creators, UAV initial counts, the read/write hazard
//   rules, create_query).
//
// A fragment of the D3D11Manager definition in d3d11.hpp, which
// includes it inside the class, in this order; not a standalone header.

    [[nodiscard]] bool ensure_storage();
    [[nodiscard]] GuestAddr allocate_object(GuestAddr vtable);

    /// Map another object-storage chunk when the current one is full.
    /// @return true if a new chunk is available for bump allocation
    [[nodiscard]] bool grow_object_storage();
    [[nodiscard]] bool write_pointer(GuestAddr address, GuestAddr value);
    [[nodiscard]] bool read_pointer(GuestAddr address, GuestAddr& value) const;
    [[nodiscard]] bool read_guid(GuestAddr address, GuidBytes& guid) const;
    [[nodiscard]] GuestAddr allocate_device(GuestAddr adapter,
                                            u32 feature_level,
                                            u32 creation_flags);
    [[nodiscard]] GuestAddr create_buffer(
        GuestAddr device_owner, const BufferDesc& desc,
        const void* initial_data,
        std::unique_ptr<graphics::BackendBuffer> opened_backend = nullptr,
        u64 opened_nonce = 0);
    /// The backend description of @p desc's storage, without the sharing and
    /// tiling fields create_buffer adds for those buffers.
    [[nodiscard]] static graphics::BackendBufferDesc backend_buffer_desc(
        const BufferDesc& desc);
    /// @p dimension is the D3D11_RESOURCE_DIMENSION the resource reports
    /// (TEXTURE1D resources are height-1 2D textures here): shared storage
    /// records it for every opener. @p opened_backend / @p opened_memory /
    /// @p opened_nonce: an opening of a shared resource (d3d11_sharing.cpp).
    [[nodiscard]] GuestAddr create_texture_2d(
        GuestAddr device_owner, const Texture2DDesc& desc,
        const void* initial_data, u32 initial_row_pitch,
        GuestAddr swap_chain_owner = 0,
        std::unique_ptr<graphics::BackendTexture2D> opened_backend = nullptr,
        u32 dimension = 3,
        std::shared_ptr<graphics::BackendSharedMemory> opened_memory =
            nullptr,
        u64 opened_nonce = 0);
    [[nodiscard]] GuestAddr create_render_target_view(
        GuestAddr device_owner, GuestAddr texture, u32 format,
        u32 view_dimension = 4,
        u32 mip_slice = 0, u32 first_array_slice = 0,
        u32 array_size = 1);
    [[nodiscard]] GuestAddr create_depth_stencil_view(
        GuestAddr device_owner, GuestAddr texture, u32 format,
        u32 view_dimension, u32 flags, u32 mip_slice,
        u32 first_array_slice, u32 array_size);
    [[nodiscard]] GuestAddr create_shader_resource_view(
        GuestAddr device_owner, GuestAddr texture, u32 format,
        u32 view_dimension, u32 most_detailed_mip, u32 mip_levels,
        u32 first_array_slice = 0, u32 array_size = 1);
    /// StructuredBuffer / typed-buffer SRV (D3D11_SRV_DIMENSION_BUFFER and
    /// BUFFEREX). Distinct from the texture path: a buffer view names an
    /// element window, not a mip/array slice.
    [[nodiscard]] GuestAddr create_buffer_shader_resource_view(
        GuestAddr device_owner, GuestAddr buffer_address, u32 format,
        u32 view_dimension, u32 first_element, u32 num_elements,
        u32 element_bytes,
        std::shared_ptr<graphics::BackendTextureBuffer> texture_buffer = {});

    [[nodiscard]] GuestAddr create_unordered_access_view(
        GuestAddr device_owner, GuestAddr texture, u32 format,
        u32 view_dimension, u32 mip_slice, u32 first_w_slice = 0,
        u32 w_size = 0);
    /// RWStructuredBuffer UAV (D3D11_UAV_DIMENSION_BUFFER). Distinct from the
    /// texture path for the same reason the SRV path is: a buffer view names
    /// an element window, not a mip slice.
    [[nodiscard]] GuestAddr create_buffer_unordered_access_view(
        GuestAddr device_owner, GuestAddr buffer_address, u32 format,
        u32 view_dimension, u32 first_element, u32 num_elements,
        u32 element_bytes, u32 buffer_flags,
        std::shared_ptr<graphics::BackendTextureBuffer> texture_buffer = {});
    /// Apply one `pUAVInitialCounts` entry to a bound view.
    ///
    /// Learn (CSSetUnorderedAccessViews): the value is "only relevant for UAVs
    /// that were created with either APPEND or COUNTER", and -1 "indicates to
    /// keep the current offset". Returns false when the write to the counter
    /// failed, which a caller reports rather than swallowing.
    [[nodiscard]] bool apply_uav_initial_count(GuestAddr view_address,
                                               u32 initial_count);
    /// Read entry @p index of a `pUAVInitialCounts` array and apply it to
    /// @p view_address. A null array is the common case and means "keep".
    void apply_initial_count(GuestAddr counts_array, u32 index,
                             GuestAddr view_address);

    // ---- Read/write hazards --------------------------------------------------
    //
    // One subresource may not be read and written by the same pipeline state.
    // The rules below are measured on Windows (d3d11_a05_context_state_probe,
    // d3d11_a05b_bind_rules_probe) and apply identically to every bind path:
    //  * binding an OUTPUT (RTV, the writable planes of a DSV, a CS or OM UAV,
    //    an SO target) unbinds every INPUT of an overlapping subresource: SRVs
    //    on all six stages, vertex buffers (stride and offset zeroed) and the
    //    index buffer (format and offset zeroed);
    //  * binding an input while an overlapping output is bound binds NULL
    //    instead (a VB/IB still records its stride/offset/format);
    //  * a new output unbinds overlapping outputs of the other bind points.
    // Granularity: mip x array slice for 1D/2D textures, mip only for a volume
    // (W slices are not subresources), the whole buffer for a buffer, and per
    // plane for a depth-stencil texture.

    /// The subresources one binding covers. `resource` 0 overlaps nothing.
    struct BoundSubresources {
        GuestAddr resource{0};
        u32 first_mip{0};
        u32 mip_count{0};
        u32 first_slice{0};
        u32 slice_count{0};
        /// kPlaneDepthOrColour and/or kPlaneStencil.
        u32 planes{0};
    };
    static constexpr u32 kPlaneDepthOrColour = 1u;
    static constexpr u32 kPlaneStencil = 2u;
    /// Output bind points, for unbind_outputs_overlapping.
    static constexpr u32 kOutputRenderTargets = 1u;
    static constexpr u32 kOutputMergerUavs = 2u;
    static constexpr u32 kOutputComputeUavs = 4u;
    static constexpr u32 kOutputStreamTargets = 8u;
    [[nodiscard]] static bool subresources_overlap(const BoundSubresources& a,
                                                   const BoundSubresources& b);
    [[nodiscard]] BoundSubresources srv_subresources(GuestAddr view) const;
    [[nodiscard]] BoundSubresources rtv_subresources(GuestAddr view) const;
    [[nodiscard]] BoundSubresources uav_subresources(GuestAddr view) const;
    /// Only the planes the view WRITES: READ_ONLY_DEPTH / READ_ONLY_STENCIL
    /// leave that plane readable by SRVs.
    [[nodiscard]] BoundSubresources dsv_written_subresources(
        GuestAddr view) const;
    [[nodiscard]] static BoundSubresources buffer_subresources(
        GuestAddr buffer);
    /// True when @p read overlaps anything @p state binds for output.
    [[nodiscard]] bool bound_output_overlaps(const ContextState& state,
                                             const BoundSubresources& read) const;
    /// Unbind every input binding of @p state overlapping any of @p written.
    void unbind_inputs_overlapping(ContextState& state,
                                   std::span<const BoundSubresources> written);
    /// Unbind the outputs of the bind points in @p classes that overlap
    /// @p written.
    void unbind_outputs_overlapping(ContextState& state,
                                    const BoundSubresources& written,
                                    u32 classes);
    /// OMSetRenderTargets (NumUAVs 0) and
    /// OMSetRenderTargetsAndUnorderedAccessViews, with Learn's three cases and
    /// the measured validation that drops the whole call.
    void set_output_merger(GuestAddr context, u32 num_rtvs, GuestAddr rtvs,
                           GuestAddr dsv, u32 uav_start, u32 num_uavs,
                           GuestAddr uavs, GuestAddr initial_counts);
    [[nodiscard]] GuestAddr create_query(
        GuestAddr device_owner, u32 query_type, u32 misc_flags);
