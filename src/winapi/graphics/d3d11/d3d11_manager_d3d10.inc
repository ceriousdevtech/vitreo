// SPDX-License-Identifier: LGPL-2.1-only
// @file d3d11_manager_d3d10.inc
// @brief D3D11Manager class body, part 17 of 17: the Direct3D 10 facets (D3D10
//   kinds, vtables and forwarding, blobs, state blocks).
//
// A fragment of the D3D11Manager definition in d3d11.hpp, which
// includes it inside the class, in this order; not a standalone header.

    // -- Direct3D 10 (winapi/graphics/d3d10/d3d10_device.cpp) ---------------
    //
    // A D3D10 interface is a facet of the D3D11 object: its own vtable at a
    // fixed offset inside the object's slot, counted with the object, one
    // COM identity (measured, d3d10_contract_probe [qi]). The D3D10 methods
    // run the D3D11 implementation: they rebuild the call the D3D11 thunk
    // would have received -- `this` the D3D11 object, every D3D10 interface
    // pointer translated to its object, every D3D10-only structure converted
    // -- and translate what comes back.

    /// The D3D10 vtable a facet carries.
    enum class D3D10Kind : u8 {
        Buffer,
        Texture,
        ShaderResourceView,
        RenderTargetView,
        DepthStencilView,
        BlendState,
        DepthStencilState,
        RasterizerState,
        SamplerState,
        /// Input layouts and vertex / geometry / pixel shaders.
        DeviceChild,
        Query,
    };
    /// A device child's D3D10 facet sits at object + 0x38 (the slot is 0x40
    /// bytes, the DXGI facets use +0x20..+0x30); the device's at + 0xE0.
    static constexpr GuestAddr kD3D10FacetOffset = 0x38;
    static constexpr GuestAddr kD3D10DeviceFacetOffset = 0xE0;
    /// Facet -> the object it belongs to (device facets live in
    /// m_interfaces instead).
    std::unordered_map<GuestAddr, GuestAddr> m_d3d10_facets;
    GuestAddr m_d3d10_device_vtable{0};
    GuestAddr m_d3d10_buffer_vtable{0};
    GuestAddr m_d3d10_texture_vtable{0};
    GuestAddr m_d3d10_shader_resource_view_vtable{0};
    GuestAddr m_d3d10_render_target_view_vtable{0};
    GuestAddr m_d3d10_depth_stencil_view_vtable{0};
    GuestAddr m_d3d10_blend_state_vtable{0};
    GuestAddr m_d3d10_depth_stencil_state_vtable{0};
    GuestAddr m_d3d10_rasterizer_state_vtable{0};
    GuestAddr m_d3d10_sampler_state_vtable{0};
    GuestAddr m_d3d10_device_child_vtable{0};
    GuestAddr m_d3d10_query_vtable{0};
    GuestAddr m_d3d10_blob_vtable{0};
    GuestAddr m_d3d10_state_block_vtable{0};
    /// Every D3D10 vtable is written (the thunks were wired).
    bool m_d3d10_ready{false};

    /// The D3D10 vtable object @p owner's facet carries; false when the
    /// object has no D3D10 interface (a UAV, a compute shader, ...).
    [[nodiscard]] bool d3d10_kind_of(GuestAddr owner, D3D10Kind& kind) const;
    [[nodiscard]] GuestAddr d3d10_vtable_for(D3D10Kind kind) const;
    /// The D3D10 facet of @p owner, created on first use; 0 for 0 or an
    /// object with no D3D10 interface.
    [[nodiscard]] GuestAddr d3d10_facet_of(GuestAddr owner);
    /// The object behind a D3D10 interface pointer; any other pointer
    /// unchanged.
    [[nodiscard]] GuestAddr d3d10_owner_of(GuestAddr iface) const;
    /// QueryInterface of a D3D11 object for a D3D10 IID: the facet when
    /// the object answers @p iid, else 0 (measured graphs, d3d10_device.cpp).
    [[nodiscard]] GuestAddr d3d10_interface_for(GuestAddr owner,
                                                const GuidBytes& iid);
    /// free_object: forget @p address's D3D10 facet.
    void d3d10_forget_facet(GuestAddr address);
    /// The device's D3D10 facet, as ID3D10DeviceChild::GetDevice and
    /// QueryInterface hand it out: 0 while the device does not answer it.
    [[nodiscard]] GuestAddr d3d10_device_facet(GuestAddr device_owner) const;

    /// Dispatch @p slot of @p method on @p self as if the guest had called
    /// it with @p args (after `this`), through a synthetic call frame in
    /// this thread's scratch arena. The D3D10 device methods and facets run
    /// the D3D11 implementation this way. Returns RAX.
    using MethodDispatch = void (D3D11Manager::*)(Marshaler&, usize);
    u64 d3d10_forward(MethodDispatch method, usize slot, GuestAddr self,
                      std::initializer_list<u64> args);
    /// This thread's guest scratch memory for synthetic frames and the
    /// structures they point at: bump allocated, released by
    /// D3D10ScratchScope (calls nest; guest callbacks may re-enter).
    struct D3D10Scratch {
        GuestAddr base{0};
        usize used{0};
    };
    std::unordered_map<std::thread::id, D3D10Scratch> m_d3d10_scratch;
    [[nodiscard]] GuestAddr d3d10_scratch(usize bytes);
    [[nodiscard]] usize d3d10_scratch_mark();
    void d3d10_scratch_release(usize mark);
    /// > 0 while a D3D10 method runs the D3D11 implementation: the
    /// ID3D11DeviceContext methods a D3D10 state disables stay reachable.
    u32 m_d3d10_forwarding{0};
    /// Copy @p count interface pointers at @p array to scratch with each
    /// D3D10 pointer translated to its object (0 for a NULL array).
    [[nodiscard]] GuestAddr d3d10_objects_in(GuestAddr array, u32 count);
    /// Rewrite @p count object pointers at @p array (a getter's output) to
    /// their D3D10 facets.
    void d3d10_objects_out(GuestAddr array, u32 count);

    /// The ID3D11DeviceContext slots a D3D10 context state disables on the
    /// immediate context (Learn, CreateDeviceContextState remarks).
    [[nodiscard]] static bool d3d11_context_slot_disabled_by_d3d10(usize slot);
    /// Run a disabled getter: against an empty binding set, which is what
    /// Windows reports (measured: NULL objects, blend factor 1, mask ~0,
    /// no viewports).
    void d3d10_run_on_empty_state(GuestAddr context,
                                  const std::function<void()>& call);
    /// D3D11CreateDeviceAndSwapChain's chain, for the D3D10 entry points:
    /// the DXGI_SWAP_CHAIN_DESC at @p desc_address, parented to @p adapter's
    /// factory. Returns S_OK / DXGI_STATUS_OCCLUDED with @p swap_chain set,
    /// or the failure.
    [[nodiscard]] u32 create_device_swap_chain(GuestAddr device_interface,
                                               GuestAddr desc_address,
                                               GuestAddr adapter,
                                               GuestAddr& swap_chain);
    /// D3D10CreateDevice(1) and the AndSwapChain(1) variants.
    void d3d10_create_device(Marshaler& m, bool api_10_1,
                             bool with_swap_chain);
    /// The context-type device methods (ID3D10Device slots 3..62, ClearState,
    /// Flush): forwarded to the immediate context. Returns false for a slot
    /// it does not own.
    [[nodiscard]] bool d3d10_context_method(Marshaler& m, usize slot,
                                            Device& device);
    /// The Create* device methods.
    void d3d10_create_method(Marshaler& m, usize slot, Device& device);
    /// Common ID3D10DeviceChild slots 0-6 of a facet; @p method is the D3D11
    /// object's own dispatcher.
    void d3d10_child_method(Marshaler& m, usize slot, GuestAddr owner,
                            MethodDispatch method);
    /// ID3D10Buffer / ID3D10Texture*::Map.
    void d3d10_map(Marshaler& m, GuestAddr owner, bool texture);

    /// ID3D10Blob objects (D3D10CreateBlob, the signature blobs).
    struct D3D10Blob {
        GuestAddr data{0};
        usize size{0};
        usize mapped{0};
        u32 ref_count{1};
    };
    std::unordered_map<GuestAddr, D3D10Blob> m_d3d10_blobs;
    /// A new blob of @p size bytes (copied from @p bytes when non-null);
    /// 0 when out of memory.
    [[nodiscard]] GuestAddr d3d10_create_blob(const u8* bytes, usize size);
    /// The static profile strings D3D10Get*ShaderProfile return.
    GuestAddr m_d3d10_profiles{0};

    /// ID3D10StateBlock (D3D10CreateStateBlock): a mask and the device
    /// state Capture read through the D3D10 getters, holding the
    /// references those getters took (measured: Capture AddRefs, Release-
    /// AllDeviceObjects releases).
    struct D3D10StateBlock {
        GuestAddr device_owner{0};
        std::array<u8, 76> mask{};
        bool captured{false};
        /// The masked bindings Capture read (everything unmasked zero), each
        /// object holding one public reference.
        std::shared_ptr<ContextBindings> state;
        u32 ref_count{1};
    };
    std::unordered_map<GuestAddr, D3D10StateBlock> m_d3d10_state_blocks;
    /// The device that made device child @p address (0: not a child).
    [[nodiscard]] GuestAddr device_owner_of(GuestAddr address);
    /// One public reference taken / dropped on every object @p bindings
    /// names (retain_object / release_object).
    void retain_bound_objects(const ContextBindings& bindings);
    void release_bound_objects(const ContextBindings& bindings);
    void d3d10_state_block_capture(D3D10StateBlock& block, GuestAddr device);
    void d3d10_state_block_apply(const D3D10StateBlock& block,
                                 GuestAddr device);
    void d3d10_state_block_release_objects(D3D10StateBlock& block);
