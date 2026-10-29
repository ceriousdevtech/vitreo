// SPDX-License-Identifier: LGPL-2.1-only
// @file d3d11_manager_resource_records.inc
// @brief D3D11Manager class body, part 1 of 17: device, tile-pool and resource
//   records (GuidBytes, PrivateDataStore, Device, TilePool, TiledState, Buffer,
//   Texture2D and their descs).
//
// A fragment of the D3D11Manager definition in d3d11.hpp, which
// includes it inside the class, in this order; not a standalone header.

private:
    using GuidBytes = std::array<u8, 16>;

    struct GuidHash {
        usize operator()(const GuidBytes& guid) const noexcept;
    };

    using PrivateDataStore =
        std::unordered_map<GuidBytes, std::vector<u8>, GuidHash>;

    struct Device {
        GuestAddr device_interface{0};
        GuestAddr context_interface{0};
        GuestAddr dxgi_device_interface{0};
        GuestAddr annotation_interface{0};
        /// The immediate context's ID3D11VideoContext(1-3) facet
        /// (measured on WARP: the context answers it, the device answers no
        /// ID3D11VideoDevice).
        GuestAddr video_context_interface{0};
        /// ID3D11Multithread / ID3D10Multithread: one facet with the
        /// device's identity and one with the immediate context's (measured:
        /// different pointers, each QI(IUnknown)s to its owner, each shares
        /// its owner's count). Zero on a SINGLETHREADED device, which
        /// refuses both (measured E_NOINTERFACE).
        GuestAddr device_multithread_interface{0};
        GuestAddr context_multithread_interface{0};
        /// SetMultithreadProtected's flag, one per device whichever facet
        /// sets it (measured). FALSE until set (measured) -- TRUE on a
        /// device of D3D10CreateDevice(1) (measured, d3d10_contract_probe).
        bool multithread_protected{false};
        /// ID3D10Device1 facet (owner + 0xE0). QueryInterface answers it
        /// only once `d3d10_interfaces` is set.
        GuestAddr d3d10_device_interface{0};
        /// The device answers ID3D10Device / ID3D10Device1: created by
        /// D3D10CreateDevice(1), or -- measured -- after its first
        /// CreateDeviceContextState call of any kind, even a failing one.
        bool d3d10_interfaces{false};
        /// The immediate context runs a D3D10 context state: the D3D10
        /// device methods are live and the ID3D11DeviceContext ones Learn
        /// lists are disabled (CreateDeviceContextState remarks).
        bool d3d10_active{false};
        /// Created by d3d10.dll's D3D10CreateDevice (the Direct3D 10.0 API):
        /// DXGI 1.0 formats only (measured; FormatCapsContext).
        bool d3d10_api_10_0{false};
        GuestAddr adapter{0};
        /// The device's public count. It includes one reference for every
        /// device child the application holds -- the immediate context among
        /// them (measured: 2 after D3D11CreateDevice with ppImmediateContext,
        /// 1 without) -- see retain_object.
        u32 device_ref_count{0};
        u32 context_ref_count{0};
        u32 feature_level{d3d11::FEATURE_LEVEL_11_0};
        u32 creation_flags{0};
        u32 exception_mode{0};
        i32 gpu_thread_priority{0};
        u32 maximum_frame_latency{3};
        /// ID3D11Device and IDXGIDevice share one store; the immediate
        /// context has its own (measured: NOT_FOUND across the two).
        PrivateDataStore private_data;
        PrivateDataStore context_private_data;
    };

public:
    /// DIAGNOSTIC (d3d11_rt_dump.cpp): the render-target dump the
    /// /tmp/vitreo-rt-dump marker asks for, written into @p dir now (created
    /// if missing); it also opens the transfer log. False when its stats.txt
    /// cannot be created. Public for the tests.
    bool dump_render_targets_to(const std::string& dir);
    // Public so d3d11_tiled.cpp's file-local helpers can walk them; they are
    // plain data only D3D11Manager creates.
    /// A tile pool (D3D11_RESOURCE_MISC_TILE_POOL; d3d11_tiled.cpp): memory
    /// in 64 KiB tiles that tiled resources map. Every tiled resource mapped
    /// into it holds it, so its memory outlives the pool buffer while
    /// mappings remain (measured: a released pool's tiles still read back).
    struct TilePool {
        std::unique_ptr<graphics::BackendTilePool> backend;
        /// ResizeTilePool's size in tiles; GetDesc's ByteWidth follows it
        /// (measured). New mappings stay below it.
        u64 tiles{0};
    };

    /// A tiled resource's tiles (D3D11_RESOURCE_MISC_TILED; d3d11_tiled.cpp),
    /// numbered as GetResourceTiling reports them: slice after slice, each
    /// slice's non-packed mips row-major mip after mip, then its packed tiles.
    struct TiledState {
        /// A buffer is one row of BACKEND_TILE_BYTES-byte tiles.
        bool buffer{false};
        u32 tile_width{0};   ///< texels (a buffer: bytes)
        u32 tile_height{1};
        u32 mip_levels{1};
        u32 array_size{1};
        u32 standard_mips{1};
        u32 packed_tiles{0};  ///< per slice
        u32 tiles_per_slice{0};
        u32 total_tiles{0};
        /// Per non-packed mip: its first tile within a slice, and its extent
        /// in tiles.
        std::vector<u32> mip_first;
        std::vector<u32> mip_tiles_x;
        std::vector<u32> mip_tiles_y;
        /// The one pool the tiles map into (null until the first mapping).
        std::shared_ptr<TilePool> pool;
        /// Mapped tiles: tile -> pool tile. Everything else is NULL-mapped.
        std::unordered_map<u32, u64> mapped;
        /// How many of this resource's tiles show each pool tile, and how
        /// many pool tiles two or more do (the resource aliases itself).
        std::unordered_map<u64, u32> pool_tile_uses;
        u32 shared_pool_tiles{0};
        TiledState() = default;
        TiledState(const TiledState&) = delete;
        TiledState& operator=(const TiledState&) = delete;
        /// Drops this resource's references to its pool tiles.
        ~TiledState();
    };

private:

    struct BufferDesc {
        u32 byte_width{0};
        u32 usage{0};
        u32 bind_flags{0};
        u32 cpu_access_flags{0};
        u32 misc_flags{0};
        u32 structure_byte_stride{0};
    };

    struct Buffer {
        GuestAddr interface_address{0};
        /// IDXGIResource facet (object + 0x20): every ID3D11Resource is also
        /// a DXGI resource, buffers included (measured).
        GuestAddr dxgi_resource_interface{0};
        /// IDXGISurface facet (object + 0x30), as a texture's (measured: a
        /// buffer is a ByteWidth x 1 DXGI_FORMAT_UNKNOWN surface).
        GuestAddr dxgi_surface_interface{0};
        GuestAddr device_owner{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        /// DXGI_RESOURCE_PRIORITY_NORMAL until set (measured).
        u32 eviction_priority{0x78000000u};
        /// IDXGIDevice2::OfferResources state.
        bool offered{false};
        BufferDesc desc{};
        std::unique_ptr<graphics::BackendBuffer> backend;
        std::vector<u8> shadow;
        /// The GPU wrote this buffer (a copy into it) after `shadow` was last
        /// true to it. A CPU read -- Map with READ access -- must first pull
        /// the GPU's contents back; nothing else reads a staging buffer.
        bool gpu_newer{false};
        /// Last submission generation whose GPU work reads or writes this
        /// buffer (copies to or from it, stream output into it); see
        /// note_gpu_use. Map waits for exactly that generation, not for the
        /// whole queue. kGpuUseUntracked: a use the backend could not place
        /// on its timeline, so the next wait is a full wait_idle().
        u64 gpu_use_gen{0};
        /// DIAGNOSTIC (transfer log): the frame (m_diag_frame) of the last
        /// GPU copy into this STAGING buffer, ~0 for none.
        u64 diag_copy_frame{~u64{0}};
        std::vector<u8> map_readback;
        GuestAddr mapped_address{0};
        usize mapped_size{0};
        /// The shadow_version the mapped pages hold. A CPU write that goes
        /// to the shadow alone (UpdateSubresource of a mappable DEFAULT
        /// buffer) leaves them behind, and the next Map rewrites them.
        u64 mapped_pages_version{~u64{0}};
        u32 mapped_type{0};
        bool mapped{false};
        /// D3D's BufferFilledSize: where the next stream-out write lands and
        /// what DrawAuto counts (D3D11.3 14.4). A 4-byte word the GPU owns,
        /// created on first use (0: never streamed to): the counts that
        /// advance it are GPU-sourced (geometry-shader emits, tessellation,
        /// indirect and DrawAuto draws), so it is only ever moved on the GPU
        /// timeline -- by vitreo_so_prepare, and by SOSetTargets' explicit
        /// offset as a copy recorded in command order.
        std::shared_ptr<graphics::BackendBuffer> so_filled;
        /// Bumped by every CPU write to `shadow` (UpdateSubresource, Unmap,
        /// a Map(READ) refresh). Keys `index_scans`; a GPU write instead sets
        /// gpu_newer, which makes the shadow unusable for a scan at all.
        u64 shadow_version{0};
        /// What an index range of this buffer holds, for input-assembly
        /// fix-ups a draw needs to know on the CPU (see scan_index_range).
        struct IndexScan {
            u64 version{0};
            u32 first{0};
            u32 count{0};
            bool wide{false};
            u32 max_index{0};
            bool has_cut{false};
        };
        std::vector<IndexScan> index_scans;
        PrivateDataStore private_data;
        /// IDXGIKeyedMutex facet (object + 0x28) of a KEYEDMUTEX buffer, and
        /// its shared state (as Texture2D's).
        GuestAddr keyed_mutex_interface{0};
        std::shared_ptr<SharedKeyedMutex> keyed_mutex;
        u32 keyed_mutex_opening{0};
        /// The creator's identity for its shared storage; 0 when not shared.
        u64 shared_nonce{0};
        /// Opened from a shared handle (an opened STAGING buffer does not
        /// Map, measured).
        bool shared_opening{false};
        /// D3D11_RESOURCE_MISC_TILE_POOL: the pool (no `backend`, no
        /// `shadow`).
        std::shared_ptr<TilePool> tile_pool;
        /// D3D11_RESOURCE_MISC_TILED: its tiles (a `backend` of address
        /// space only, no `shadow` -- a tiled buffer may be 4 GiB).
        std::unique_ptr<TiledState> tiled;
    };

    struct Texture2DDesc {
        u32 width{0};
        u32 height{0};
        u32 mip_levels{1};
        u32 array_size{1};
        u32 format{0};
        u32 sample_count{1};
        u32 sample_quality{0};
        u32 usage{0};
        u32 bind_flags{0};
        u32 cpu_access_flags{0};
        u32 misc_flags{0};
    };

    struct Texture3DDesc {
        u32 width{0};
        u32 height{0};
        u32 depth{0};
        u32 mip_levels{1};
        u32 format{0};
        u32 usage{0};
        u32 bind_flags{0};
        u32 cpu_access_flags{0};
        u32 misc_flags{0};
    };

    /// DIAGNOSTIC (d3d11_rt_dump.cpp): one shader combination that drew
    /// into a render target, how often, and what its first draw bound.
    struct DiagDrawPair {
        GuestAddr vs{0};
        GuestAddr gs{0};
        GuestAddr ps{0};
        u32 draws{0};
        /// The VS and PS stages' bound t# views (resource, format, window,
        /// typed-buffer element bias) and b# slices (buffer, first
        /// constant, count), as text.
        std::string bindings;
    };

    struct Texture2D {
        struct SubresourceStorage {
            usize offset{0};
            usize size{0};
            u32 row_pitch{0};
            u32 width{0};
            u32 height{0};
            /// Volume subresources: W extent and the byte stride between W
            /// slices (D3D11_MAPPED_SUBRESOURCE::DepthPitch). 2D leaves depth 1
            /// and slice_pitch 0 (DepthPitch is then the subresource size).
            u32 depth{1};
            usize slice_pitch{0};
            GuestAddr mapped_address{0};
            usize mapped_size{0};
            u32 mapped_type{0};
            bool mapped{false};
            /// Map on a DEFAULT texture with CPU access (MapOnDefaultTextures):
            /// WriteToSubresource changed the CPU image since the Map, so the
            /// Unmap stores it into the GPU texture.
            bool cpu_dirty{false};
            /// STAGING only: GPU -> staging copies recorded on the command
            /// timeline and not yet folded into the shadow, oldest first.
            /// Each read the whole source level; the box lands at dst.
            struct PendingReadback {
                std::shared_ptr<graphics::BackendReadback> readback;
                usize level_row_bytes{0};
                u32 level_rows{0};
                u32 elem{0};
                u32 src_ex{0}, src_ey{0};
                u32 dst_ex{0}, dst_ey{0};
                u32 elements_w{0}, elements_h{0};
            };
            std::vector<PendingReadback> pending;
        };

        GuestAddr interface_address{0};
        GuestAddr dxgi_resource_interface{0};
        /// IDXGISurface2 facet (object + 0x30). Zero for a volume or 1D
        /// texture, which are not DXGI surfaces.
        GuestAddr dxgi_surface_interface{0};
        GuestAddr keyed_mutex_interface{0};
        GuestAddr device_owner{0};
        GuestAddr swap_chain_owner{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        /// DXGI_RESOURCE_PRIORITY_NORMAL until set (measured).
        u32 eviction_priority{0x78000000u};
        /// IDXGIDevice2::OfferResources state.
        bool offered{false};
        Texture2DDesc desc{};
        Texture3DDesc volume_desc{};
        /// D3D11_TEXTURE_LAYOUT as created (CreateTexture2D1 /
        /// CreateTexture3D1; 0 from the base creators), stored verbatim --
        /// GetDesc1 reports any value it was created with (measured) -- and
        /// deciding what a Map exposes (mapped_image_layout).
        u32 layout{0};
        bool volume{false};
        /// D3D11_RESOURCE_DIMENSION_TEXTURE1D. Backed by a height-1
        /// BackendTexture2D rather than a Metal 1D texture: MTLTextureType1D
        /// supports neither mipmaps nor use as a render target, while a D3D11
        /// Texture1D supports both, so a height-1 2D texture is the mapping
        /// that keeps the guest-visible feature set intact. Every Map, update
        /// and copy path then works unchanged; only the entry points that must
        /// report the resource's dimension look at this flag.
        bool linear{false};
        /// SetResourceMinLOD's value, kept only for a resource created with
        /// D3D11_RESOURCE_MISC_RESOURCE_CLAMP (Learn; any other reads 0 --
        /// measured, a06 probe LOD lines).
        float min_lod{0.0f};
        /// Diagnostic write census for d3d11_rt_dump.cpp: how the resource
        /// was written, so an unwritten target reads "never targeted" vs
        /// "targeted, GPU work lost".
        u32 diag_rt_draws{0};
        u32 diag_clears{0};
        u32 diag_clear_failures{0};
        u32 diag_copy_dst{0};
        /// DIAGNOSTIC (transfer log): the frame (m_diag_frame) of the last
        /// GPU copy into this STAGING texture, ~0 for none.
        u64 diag_copy_frame{~u64{0}};
        u32 diag_uav_binds{0};
        u32 diag_uav_drops{0};
        /// Last shader bound with this texture as a UAV (guest address of the
        /// compute or pixel shader object) and whether it was compute, so the
        /// render-target dump can save the writer's DXBC for an offline replay.
        GuestAddr diag_uav_writer{0};
        /// Last pixel shader bound when this texture was a render target,
        /// and the vertex and geometry shaders of that draw (a volume LUT is
        /// routed to its W slices by the GS's SV_RenderTargetArrayIndex).
        GuestAddr diag_rt_writer{0};
        GuestAddr diag_rt_writer_vs{0};
        GuestAddr diag_rt_writer_gs{0};
        /// Every distinct (VS, GS, PS) that drew into this texture as a
        /// render target since the previous render-target dump, with the
        /// t# views and b# slices its first such draw bound
        /// (note_diag_draw_pair). Filled only while a dump has armed it.
        std::vector<DiagDrawPair> diag_draw_pairs;
        u32 diag_draw_pairs_dropped{0};
        bool diag_uav_writer_compute{false};
        /// Inputs of the last dispatch that bound this texture as a UAV:
        /// t0/t1 resource descriptions and the first cb0 words, so a dumped
        /// UAV target can be re-run with its real inputs on Windows.
        std::string diag_uav_note;
        std::unique_ptr<graphics::BackendTexture2D> backend;
        std::unique_ptr<graphics::BackendTexture3D> backend_3d;
        StagingBytes shadow;
        /// A shared STAGING resource: the memory `shadow` is attached to
        /// (d3d11_sharing.cpp). GPU copies into and out of it are GPU blits
        /// against this memory, so every opening sees them.
        std::shared_ptr<graphics::BackendSharedMemory> shared_memory;
        /// Last submission generation whose GPU copy reads or writes
        /// shared_memory (0: none); Map waits for it.
        u64 shared_memory_gen{0};
        /// Opened from a shared handle rather than created: an opened
        /// STAGING resource does not Map (measured E_OUTOFMEMORY,
        /// d3d11_share_ers_probe).
        bool shared_opening{false};
        /// Reused staging buffer for Unmap read-back, mirroring Buffer's.
        /// This used to be a `std::vector<u8>` constructed inside Unmap, so
        /// every frame upload paid a multi-megabyte malloc AND a zero-fill of
        /// memory that was overwritten on the very next line. Measured on a
        /// Battle.net session: ID3D11DeviceContext::Unmap was 30,358 calls at
        /// 255 us each = 7.8 s of guest-thread time.
        std::vector<u8> map_scratch;
        std::vector<SubresourceStorage> subresources;
        u32 row_pitch{0};
        /// IDXGIKeyedMutex state (KEYEDMUTEX resources): one per shared
        /// resource, shared by every opening in every process
        /// (winapi/graphics/common/shared_keyed_mutex.hpp).
        std::shared_ptr<SharedKeyedMutex> keyed_mutex;
        /// This opening's identity in keyed_mutex (unique in the process).
        u32 keyed_mutex_opening{0};
        /// The creator's identity for its shared storage (the storage id is
        /// recycled once the storage is gone); 0 when not shared.
        u64 shared_nonce{0};
        /// A swap chain's buffer n > 0 of a sequential effect: readable only
        /// (Learn GetBuffer), so GetUsage reports DXGI_USAGE_READ_ONLY.
        bool read_only_buffer{false};
        PrivateDataStore private_data;
        /// D3D11_RESOURCE_MISC_TILED: its tiles (d3d11_tiled.cpp).
        std::unique_ptr<TiledState> tiled;
    };
