// SPDX-License-Identifier: LGPL-2.1-only
// @file d3d11_manager_view_records.inc
// @brief D3D11Manager class body, part 2 of 17: view records
//   (ShaderResourceView, UnorderedAccessView, RenderTargetView,
//   DepthStencilView).
//
// A fragment of the D3D11Manager definition in d3d11.hpp, which
// includes it inside the class, in this order; not a standalone header.

    struct ShaderResourceView {
        GuestAddr interface_address{0};
        /// The IDXGISurface facet an SRV answers (object + 0x20; see
        /// srv_query_interface).
        GuestAddr dxgi_surface_interface{0};
        GuestAddr device_owner{0};
        GuestAddr texture{0};
        /// Set instead of `texture` when the view names a buffer resource
        /// (D3D11_SRV_DIMENSION_BUFFER / BUFFEREX) — a StructuredBuffer or
        /// typed buffer SRV. Exactly one of the two is ever non-zero.
        GuestAddr buffer{0};
        /// Buffer views only: element window, in elements of `element_bytes`.
        u32 first_element{0};
        u32 num_elements{0};
        /// Buffer views only: bytes per element. A structured buffer takes it
        /// from the resource's structure byte stride; a typed buffer takes it
        /// from the view's DXGI format.
        u32 element_bytes{0};
        /// Typed buffer views only: the buffer's storage aliased as a texture
        /// of the view's DXGI format, so the hardware decodes each element.
        /// Null for a StructuredBuffer view, which really is a buffer.
        std::shared_ptr<graphics::BackendTextureBuffer> texture_buffer;
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        u32 format{0};
        u32 view_dimension{0};
        u32 most_detailed_mip{0};
        u32 mip_levels{1};
        u32 first_array_slice{0};
        u32 array_size{1};
        /// BUFFEREX views only: D3D11_BUFFEREX_SRV_FLAG (RAW), for GetDesc.
        u32 buffer_flags{0};
        /// TEXTURE2DMS views only: the caller's UnusedField_NothingToDefine,
        /// which Windows' GetDesc echoes back verbatim (measured,
        /// d3d11_view_desc_fill_probe).
        u32 ms_unused_field{0};
        PrivateDataStore private_data;
    };

    struct UnorderedAccessView {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        GuestAddr texture{0};
        /// Set instead of `texture` when the view names a buffer resource
        /// (D3D11_UAV_DIMENSION_BUFFER) — an RWStructuredBuffer. Exactly one
        /// of the two is ever non-zero.
        GuestAddr buffer{0};
        /// Buffer views only: element window, in elements of `element_bytes`.
        u32 first_element{0};
        u32 num_elements{0};
        /// Buffer views only: bytes per element. A structured buffer takes it
        /// from the resource's structure byte stride; a typed buffer takes it
        /// from the view's DXGI format.
        u32 element_bytes{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        u32 format{0};
        u32 view_dimension{0};
        u32 mip_slice{0};
        /// Texture3D views only (D3D11_TEX3D_UAV): the W-slice window of the
        /// selected mip that the view exposes.
        u32 first_w_slice{0};
        u32 w_size{0};
        /// Texture1DArray views only (D3D11_TEX1D_ARRAY_UAV): the slice window,
        /// with an ArraySize of -1 already resolved to the remaining slices.
        u32 first_array_slice{0};
        u32 array_size{1};
        /// Buffer views only: D3D11_BUFFER_UAV_FLAG. RAW views a
        /// RWByteAddressBuffer; APPEND and COUNTER each attach `counter` below.
        u32 buffer_flags{0};
        /// The hidden counter an APPEND or COUNTER buffer UAV carries.
        ///
        /// Learn (CopyStructureCount): "These types of resources have hidden
        /// counters tracking 'how many' records have been written." It belongs
        /// to the *view*, not the resource, and its value survives unbinding,
        /// so it is real GPU storage rather than a number kept here: the shader
        /// mutates it, and `CopyStructureCount` copies it on the GPU timeline
        /// where those mutations are ordered.
        std::unique_ptr<graphics::BackendBuffer> counter;
        /// Typed buffer views only (`RWBuffer<T>`: neither RAW nor
        /// structured): a writable texture view over the buffer's storage.
        /// Learn (RWBuffer): the view's DXGI format decodes and encodes each
        /// element, which is the texture unit's job -- the shader declares a
        /// read_write texture_buffer, not a word array.
        std::shared_ptr<graphics::BackendTextureBuffer> texture_buffer;
        PrivateDataStore private_data;
    };

    struct RenderTargetView {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        GuestAddr texture{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        u32 format{0};
        u32 view_dimension{0};
        u32 mip_slice{0};
        /// Slice window into the resource. A Texture2DArray view stores
        /// FirstArraySlice/ArraySize here; a Texture3D view (view_dimension
        /// D3D11_RTV_DIMENSION_TEXTURE3D) stores FirstWSlice/WSize, which is
        /// the same shape addressing a different axis.
        u32 first_array_slice{0};
        u32 array_size{1};
        /// Set instead of `texture` by a D3D11_RTV_DIMENSION_BUFFER view:
        /// elements [first_element, first_element + num_elements) of the
        /// view format, rendered as a num_elements x 1 target.
        GuestAddr buffer{0};
        u32 first_element{0};
        u32 num_elements{0};
        /// A buffer view's render target: a num_elements x 1 texture of the
        /// view format standing in for the window, filled from the buffer
        /// before and copied back after every draw or clear through the
        /// view (sync_buffer_render_target). Metal's texture aliases of a
        /// buffer are neither hazard-tracked against it nor placeable at
        /// the texel-aligned offsets D3D allows.
        std::shared_ptr<graphics::BackendTexture2D> buffer_proxy;
        PrivateDataStore private_data;
    };

    struct DepthStencilView {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        GuestAddr texture{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        u32 format{0};
        u32 view_dimension{0};
        u32 flags{0};
        u32 mip_slice{0};
        u32 first_array_slice{0};
        u32 array_size{1};
        /// Measured quirk (d3d11_dsv_probe): a DSV created from an explicit
        /// TEXTURE1D desc reports 1 in the last union word of GetDesc; one
        /// from a NULL desc reports 0.
        u32 union_tail{0};
        PrivateDataStore private_data;
    };
