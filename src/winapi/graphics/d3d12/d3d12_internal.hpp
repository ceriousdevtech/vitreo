#pragma once
/// @file d3d12_internal.hpp
/// @brief The d3d12.dll implementation shared by its translation units: the
/// object model, every object's state, and the Frontend that owns them.
///
/// Nothing here is typed from memory. Vtable orders, stdcall byte counts, IIDs
/// and slot names come from d3d12_vtables.hpp; enum values, #defines and
/// HRESULTs from d3d12_constants.hpp; guest struct offsets (x86 and x64) from
/// d3d12_layouts.hpp. All three are generated from the SDK headers and checked
/// in CTest (tools/audit_d3d11_vtables.py, tools/gen_d3d12_layouts.py).
///
///   * memory: an ID3D12Heap is a BackendHeap; a placed or committed buffer is
///     a window (heap buffer, offset) of one, with a stable GPU VA; a
///     CPU-visible heap is mapped into the guest once, so Map is zero-copy.
///   * descriptors: host-side records in a descriptor heap; CPU handles are
///     addresses in a reserved, never-dereferenced guest range.
///   * command lists record a frontend command stream; ExecuteCommandLists
///     translates it into backend work, resolving root signatures and
///     descriptor tables THEN -- D3D12 lets an application rewrite a
///     descriptor after recording and before execution.

#include "winapi/graphics/d3d12/d3d12.hpp"
#include "winapi/graphics/d3d12/d3d12_caps.hpp"
#include "winapi/graphics/d3d12/d3d12_constants.hpp"
#include "winapi/graphics/d3d12/d3d12_layouts.hpp"
#include "winapi/graphics/d3d12/d3d12_lock.hpp"
#include "winapi/graphics/d3d12/d3d12_raytracing.hpp"
#include "winapi/graphics/d3d12/d3d12_shader_cache_disk.hpp"
#include "winapi/graphics/d3d12/d3d12_vtables.hpp"
#include "winapi/core/guest_com.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/common/frame_profile.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/mesh_stage.hpp"
#include "graphics/shaders/pipeline_stats.hpp"
#include "graphics/shaders/dxil_raytracing.hpp"
#include "graphics/shaders/raster_emulation_types.hpp"
#include "graphics/shaders/sampler_feedback_layout.hpp"
#include "graphics/shaders/shader_types.hpp"

#include <vitreo/common.hpp>

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace vitreo {
class GuestMemory;
}
namespace vitreo::winapi {
class HandleTable;
class VirtualMemoryManager;
}

#include <atomic>

namespace vitreo::winapi::d3d12 {

/// Times command-list replay blocked the CPU on the GPU (a readback copy
/// and its wait). Tests read it to prove a path never waits.
inline std::atomic<u64> g_replay_cpu_waits{0};

// Stream-output limits the generated constants header does not emit.
// mingw-w64 13.0.0 include/d3d12.h:
//   D3D12_SO_NO_RASTERIZED_STREAM (0xffffffff)
//   D3D12_SO_STREAM_COUNT (4)
//   D3D12_SO_OUTPUT_COMPONENT_COUNT (128)
//   D3D12_SO_BUFFER_MAX_STRIDE_IN_BYTES (2048)
// D3D12_SO_BUFFER_SLOT_COUNT is already in d3d12_constants.hpp.
inline constexpr u32 D3D12_SO_NO_RASTERIZED_STREAM = 0xffffffffu;
inline constexpr u32 D3D12_SO_STREAM_COUNT = 4u;
inline constexpr u32 D3D12_SO_OUTPUT_COMPONENT_COUNT = 128u;
inline constexpr u32 D3D12_SO_BUFFER_MAX_STRIDE_IN_BYTES = 2048u;

// =============================================================================
// Diagnostics
// =============================================================================

/// VITREO_D3D12_DIAG: log device creation, capability queries and every
/// failure path a guest reaches. Diagnostic only.
[[nodiscard]] inline bool diag_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("VITREO_D3D12_DIAG");
        return value != nullptr && value[0] != '\0' &&
               std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

/// VITREO_D3D12_DUMP_REFUSED=<dir>: every refused pipeline creation writes
/// its description, shader blobs and refusal reason into <dir>
/// (PipelineRefusalScope). Diagnostic only; nullptr when unset.
[[nodiscard]] const char* refused_pipeline_dump_dir();
/// VITREO_D3D12_DUMP_PSOS=<dir>: every pipeline creation that succeeds
/// writes the same dump into <dir>, with every guest range the creation read
/// (`.mem.bin`), for tests/tools/d3d12_pso_replay.cpp. Without the variable,
/// the marker file /tmp/vitreo-d3d12-dump-psos turns it on (its first line
/// the directory; empty: /tmp/vitreo-d3d12-psos). Diagnostic only; nullptr
/// when off.
[[nodiscard]] const char* created_pipeline_dump_dir();

/// True while a pipeline creation runs on this thread (PipelineRefusalScope):
/// every d3d12_diag and unimplemented line is then also kept as a reason the
/// pipeline may be refused for.
[[nodiscard]] bool pipeline_refusal_recording();
/// Keep @p text as one reason of the pipeline creation on this thread.
void note_pipeline_refusal(const char* text);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
inline void d3d12_diag(const char* format, ...) {
    const bool record = pipeline_refusal_recording();
    if (!diag_enabled() && !record) return;
    char buffer[768];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (record) note_pipeline_refusal(buffer);
    if (diag_enabled()) std::fprintf(stderr, "[d3d12-diag] %s\n", buffer);
}

/// A refused pipeline-creation step that is D3D12 validation (the documented
/// or measured failure of the description): its reason, @p format, is logged
/// under VITREO_D3D12_DIAG and kept for the refusal report. Returns @p hr.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
u32 refuse(u32 hr, const char* format, ...);

/// The one-time `[vitreo] UNIMPLEMENTED d3d12!<api>: <case>` line every
/// unimplementable case prints where it is called. Once per distinct
/// (api, case) text.
void unimplemented(const char* api, const char* what);

// =============================================================================
// Small shared helpers
// =============================================================================

/// FNV-1a, 64-bit: blob identity (root signatures, cached pipelines).
[[nodiscard]] inline u64 fnv1a(const void* data, usize size,
                               u64 hash = 0xcbf29ce484222325ull) {
    const auto* bytes = static_cast<const u8*>(data);
    for (usize i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

[[nodiscard]] inline bool guid_equal(const GuidBytes& a, const GuidBytes& b) {
    return std::memcmp(a.data(), b.data(), a.size()) == 0;
}

struct GuidHash {
    usize operator()(const GuidBytes& guid) const noexcept {
        u64 low = 0;
        u64 high = 0;
        std::memcpy(&low, guid.data(), 8);
        std::memcpy(&high, guid.data() + 8, 8);
        return static_cast<usize>(low ^ (high * 0x9E3779B97F4A7C15ull));
    }
};

/// WKPDID_D3DDebugObjectNameW {4cca5fd8-921f-42c8-8566-70caf2a9b741}: the
/// private-data GUID ID3D12Object::SetName stores under (Learn, SetName).
inline constexpr GuidBytes WKPDID_D3DDEBUGOBJECTNAMEW{
    0xD8, 0x5F, 0xCA, 0x4C, 0x1F, 0x92, 0xC8, 0x42,
    0x85, 0x66, 0x70, 0xCA, 0xF2, 0xA9, 0xB7, 0x41};

/// WKPDID_D3D12UniqueObjectId {1b39de15-ec04-4bae-ba4d-8cef79fc04c1}
/// (Windows SDK 10.0.26100.0 um/d3dcommon.h; not in mingw-w64): a read-only
/// 8-byte id the runtime gives every object (measured, a01 privdata: size 8).
inline constexpr GuidBytes WKPDID_D3D12UNIQUEOBJECTID{
    0x15, 0xDE, 0x39, 0x1B, 0x04, 0xEC, 0xAE, 0x4B,
    0xBA, 0x4D, 0x8C, 0xEF, 0x79, 0xFC, 0x04, 0xC1};

/// TIMESTAMP query ticks per second: GPU completion times are host-clock
/// nanoseconds (graphics::GpuTimestamp::gpu_time_ns), as D3D11's are.
inline constexpr u64 TIMESTAMP_FREQUENCY_HZ = 1000000000ull;
/// The host clock GPU completion times are in (mach_absolute_time, in ns):
/// GetClockCalibration's GPU half.
[[nodiscard]] u64 host_gpu_clock_ns();

/// How long a blocking `SetEventOnCompletion(value, nullptr)` waits before it
/// gives up. A value the guest will never signal must not wedge the thread
/// forever with no diagnostic.
inline constexpr u64 FENCE_BLOCKING_WAIT_TIMEOUT_NS = 5ull * 1000000000ull;

/// Read a `UINT64` (or an 8-byte struct such as D3D12_GPU_DESCRIPTOR_HANDLE)
/// passed by value: one argument slot on x64, two on x86. @p next_index is
/// where the following parameter starts.
[[nodiscard]] inline u64 read_arg_u64(const Marshaler& m, bool is_64bit,
                                      usize index, usize& next_index) {
    if (is_64bit) {
        next_index = index + 1;
        return m.arg(index);
    }
    next_index = index + 2;
    return static_cast<u64>(m.arg32(index)) |
           (static_cast<u64>(m.arg32(index + 1)) << 32);
}

/// Index of the interface table whose export name is @p name (constexpr, so a
/// misspelt name fails the build).
[[nodiscard]] constexpr usize table_index(const char* name) {
    for (usize index = 0; index < INTERFACE_TABLE_COUNT; ++index) {
        const char* a = INTERFACE_TABLES[index].export_name;
        const char* b = name;
        while (*a != '\0' && *a == *b) {
            ++a;
            ++b;
        }
        if (*a == '\0' && *b == '\0') return index;
    }
    return INTERFACE_TABLE_COUNT;
}

// =============================================================================
// Objects
// =============================================================================

enum class Kind : u8 {
    Device,
    CommandQueue,
    CommandAllocator,
    Fence,
    CommandList,
    Heap,
    Resource,
    DescriptorHeap,
    QueryHeap,
    CommandSignature,
    RootSignature,
    PipelineState,
    SwapChain,
    // Configuration objects D3D12GetInterface hands out: IUnknown-derived,
    // not ID3D12Object (d3d12_config.cpp).
    SdkConfiguration,
    DeviceFactory,
    DredSettings,
    // Device children of d3d12_library.cpp.
    PipelineLibrary,
    ShaderCacheSession,
    // d3d12_state_object.cpp.
    StateObject,
    LifetimeTracker,
};

/// The vtable (interface table) each kind of object is served with.
[[nodiscard]] constexpr usize table_for(Kind kind) {
    switch (kind) {
    case Kind::Device: return table_index("Device");
    case Kind::CommandQueue: return table_index("CommandQueue");
    case Kind::CommandAllocator: return table_index("CommandAllocator");
    case Kind::Fence: return table_index("Fence");
    case Kind::CommandList: return table_index("GraphicsCommandList");
    case Kind::Heap: return table_index("Heap");
    case Kind::Resource: return table_index("Resource");
    case Kind::DescriptorHeap: return table_index("DescriptorHeap");
    case Kind::QueryHeap: return table_index("QueryHeap");
    case Kind::CommandSignature: return table_index("CommandSignature");
    case Kind::RootSignature: return table_index("RootSignature");
    case Kind::PipelineState: return table_index("PipelineState");
    case Kind::SwapChain: return table_index("SwapChain");
    case Kind::SdkConfiguration: return table_index("SdkConfiguration");
    case Kind::DeviceFactory: return table_index("DeviceFactory");
    case Kind::DredSettings: return table_index("DredSettings");
    case Kind::PipelineLibrary: return table_index("PipelineLibrary");
    case Kind::ShaderCacheSession: return table_index("ShaderCacheSession");
    case Kind::StateObject: return table_index("StateObject");
    case Kind::LifetimeTracker: return table_index("LifetimeTracker");
    }
    return INTERFACE_TABLE_COUNT;
}

/// ID3D12Object private data: bytes and interfaces share one GUID namespace
/// (Learn, ID3D12Object::SetPrivateDataInterface).
struct PrivateData {
    std::unordered_map<GuidBytes, std::vector<u8>, GuidHash> bytes;
    std::unordered_map<GuidBytes, GuestAddr, GuidHash> interfaces;
};

struct Object {
    explicit Object(Kind object_kind) : kind(object_kind) {}
    virtual ~Object() = default;
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

    Kind kind;
    /// The interface pointer handed to the guest.
    GuestAddr address{0};
    /// The owning ID3D12Device (0 for the device itself).
    GuestAddr device{0};
    u32 ref_count{1};
    /// Never reused, so a descriptor or recorded command naming an object by
    /// address can tell it from a later object at the same address.
    u64 serial{0};
    PrivateData private_data;
    /// The ID3DDestructionNotifier tear-off (0 until first asked for): a
    /// second interface pointer of this object, sharing its reference count.
    GuestAddr notifier{0};
    /// RegisterDestructionCallback entries, run in registration order once
    /// the object is destroyed.
    struct DestructionCallback {
        u32 id{0};
        GuestAddr function{0};
        GuestAddr data{0};
    };
    std::vector<DestructionCallback> destruction_callbacks;
    u32 next_callback_id{1};
    /// Other tear-offs (DRED, tools, configuration, compatibility, pageable
    /// tools), by interface table: second pointers of this object sharing
    /// its reference count, made on first QueryInterface.
    std::vector<std::pair<usize, GuestAddr>> tearoff_pointers;
};

/// The DRED settings one ID3D12DeviceRemovedExtendedDataSettings2 edits:
/// the process's global state, or a device factory's own (Learn,
/// ID3D12DeviceFactory; measured, a01d factory_state).
struct DredConfig {
    u32 auto_breadcrumbs{0};  ///< D3D12_DRED_ENABLEMENT
    u32 page_fault{0};
    u32 watson{0};
    u32 breadcrumb_context{0};
    bool markers_only{false};
};

struct Device final : Object {
    Device() : Object(Kind::Device) {}
    u32 feature_level{0};
    /// GetDeviceRemovedReason; S_OK while the device is usable. Atomic: read
    /// by shared-lock calls (d3d12_lock.hpp); it leaves S_OK once, under
    /// Frontend::m_remove_mutex.
    std::atomic<u32> removed_reason{0};
    /// D3D12_DEVICE_FLAGS of the configuration the device was created with
    /// (ID3D12DeviceConfiguration::GetDesc).
    u32 configuration_flags{0};
    /// ID3D12DeviceTools::SetNextAllocationAddress (0: none pending).
    u64 next_allocation_va{0};
};

struct SdkConfiguration final : Object {
    SdkConfiguration() : Object(Kind::SdkConfiguration) {}
};

struct DeviceFactory final : Object {
    DeviceFactory() : Object(Kind::DeviceFactory) {}
    u32 flags{0};  ///< D3D12_DEVICE_FACTORY_FLAGS
    DredConfig dred{};
};

struct PipelineLibrary final : Object {
    PipelineLibrary() : Object(Kind::PipelineLibrary) {}
    struct Entry {
        std::u16string name;
        u64 digest{0};  ///< PipelineState::cache_digest
        bool compute{false};
        bool from_stream{false};
        /// The stored or first loaded pipeline (no reference: measured,
        /// StorePipeline leaves the count alone), handed out while alive.
        GuestAddr live{0};
        u64 live_serial{0};
    };
    std::vector<Entry> entries;
};

/// The values of one shader cache identifier, shared by its sessions.
struct ShaderCacheStore {
    u64 version{0};
    u32 open_sessions{0};
    bool delete_on_destroy{false};
    std::map<std::vector<u8>, std::vector<u8>> values;
    /// D3D12_SHADER_CACHE_MODE_DISK: the file the values persist in
    /// (d3d12_shader_cache_disk.hpp); null for a memory cache.
    std::unique_ptr<ShaderCacheFile> file;
};

struct ShaderCacheSession final : Object {
    ShaderCacheSession() : Object(Kind::ShaderCacheSession) {}
    GuidBytes identifier{};
    u32 mode{0};
    u32 flags{0};
    u32 memory_bytes{0};
    u32 memory_entries{0};
    u32 file_bytes{0};
    u64 version{0};
    std::shared_ptr<ShaderCacheStore> store;
};

struct LifetimeTracker final : Object {
    LifetimeTracker() : Object(Kind::LifetimeTracker) {}
    /// The ID3D12LifetimeOwner (no reference: measured).
    GuestAddr owner{0};
};

struct DredSettings final : Object {
    DredSettings() : Object(Kind::DredSettings) {}
    /// The factory whose settings these are; 0 for the global state.
    GuestAddr factory{0};
};

// -----------------------------------------------------------------------------
// Memory, heaps and resources
// -----------------------------------------------------------------------------

/// A block of GPU memory: an ID3D12Heap's, or the implicit heap of a committed
/// resource. Buffers are windows of `backend->buffer()`.
struct Memory {
    /// Shared with the aliases OpenSharedHandle makes (below).
    std::shared_ptr<graphics::BackendHeap> backend;
    /// An alias: the same bytes under another GPU VA, as an opened shared
    /// heap or resource has (measured, a04s: the opened buffer's VA
    /// differs). Keeps the source alive; the guest mapping is the source's.
    std::shared_ptr<Memory> source;
    u64 size{0};
    /// GPU virtual address of byte 0 (every buffer VA is va + its offset).
    u64 va{0};
    /// CPU-visible memory: the persistent guest mapping of byte 0.
    GuestAddr guest{0};
    usize guest_size{0};
    bool cpu_visible{false};
    /// The guest mapping is a write-watched allocation of the process's
    /// virtual memory (D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH).
    bool write_watched{false};
    /// The guest mapping is the guest's own memory (an existing heap): never
    /// unmapped here.
    bool external_guest{false};
    /// D3D12_HEAP_TYPE_UPLOAD: only the CPU writes it, so its bytes are what
    /// the GPU reads (Frontend::read_upload_va).
    bool upload{false};
};

/// D3D12_HEAP_PROPERTIES.
struct HeapProperties {
    u32 type{0};
    u32 cpu_page_property{0};
    u32 memory_pool_preference{0};
    u32 creation_node_mask{0};
    u32 visible_node_mask{0};
};

struct Heap final : Object {
    Heap() : Object(Kind::Heap) {}
    u64 size{0};
    HeapProperties properties{};
    u64 alignment{0};
    u32 flags{0};
    /// D3D12_COMPATIBILITY_SHARED_FLAGS (ID3D12CompatibilityDevice).
    u32 compatibility_flags{0};
    std::shared_ptr<Memory> memory;
    /// Tile-pool memory UpdateTileMappings points reserved resources at.
    /// Not the placement heap's bytes: a placed resource in this heap does
    /// not alias a reserved mapping of the same tile index.
    std::shared_ptr<graphics::BackendTilePool> tile_pool;
    u64 tile_count{0};
};

/// One reserved resource's tiles. Indexes follow the backend's tiled layout
/// (D3D11 TiledState): buffers are a row of 64 KiB tiles; a texture's
/// non-packed mips are boxes of tile_width x tile_height, then each slice's
/// packed tail. Absent from `mapped` means NULL (no memory).
struct ReservedTiles {
    struct Binding {
        std::shared_ptr<graphics::BackendTilePool> pool;
        u64 pool_tile{0};
    };
    ~ReservedTiles() {
        for (const auto& [index, binding] : mapped) {
            (void)index;
            if (binding.pool) binding.pool->unreference(binding.pool_tile);
        }
    }
    ReservedTiles() = default;
    ReservedTiles(const ReservedTiles&) = delete;
    ReservedTiles& operator=(const ReservedTiles&) = delete;

    bool buffer{false};
    u32 tile_width{1};
    u32 tile_height{1};
    u32 mip_levels{1};
    u32 array_size{1};
    u32 standard_mips{0};
    u32 packed_tiles{0};
    u32 tiles_per_slice{0};
    u32 total_tiles{0};
    std::vector<u32> mip_first;
    std::vector<u32> mip_tiles_x;
    std::vector<u32> mip_tiles_y;
    std::map<u32, Binding> mapped;
    /// How many of this resource's tiles show one pool tile. Two or more
    /// makes the resource self-aliased (the backend fences blits on it).
    std::map<std::pair<graphics::BackendTilePool*, u64>, u32> uses;
    u32 shared_tiles{0};
    /// One bit per tile, 1 = mapped, as the queue has applied the mappings
    /// (UpdateTileMappings / CopyTileMappings run in queue order, after the
    /// work submitted before them): the buffer residency a shader's status
    /// reads (tiled resources tier 2, DxbcTranslationOptions::tiled_status).
    std::shared_ptr<std::vector<u32>> resident =
        std::make_shared<std::vector<u32>>();
};

/// What one reserved tile should show after a mapping update. A null pool
/// unmaps it.
struct TileAssignment {
    std::shared_ptr<graphics::BackendTilePool> pool;
    u64 pool_tile{graphics::BACKEND_TILE_UNMAPPED};
};

struct TiledLayout;
struct Resource;
/// The tiling GetResourceTiling reports for a reserved resource this device
/// created (d3d12_queue.cpp): the standard tile shape and the backend's own
/// standard-mip count and packed tail. False for anything else.
[[nodiscard]] bool reserved_tiled_layout(const Resource& resource, TiledLayout& layout);

/// GPU virtual address of a reserved buffer. The sparse buffer is not a
/// window of a placement heap, so it has its own VA span.
struct ReservedVa {
    u64 va{0};
    /// Whole tiles of address space (the sparse allocation).
    u64 bytes{0};
    /// D3D12_RESOURCE_DESC::Width: the bytes the resource owns.
    u64 resource_bytes{0};
    graphics::BackendBuffer* buffer{nullptr};
    /// ReservedTiles::resident of the buffer (a root descriptor's status).
    std::shared_ptr<std::vector<u32>> resident;
};

/// D3D12_RESOURCE_DESC1 (the D3D12_RESOURCE_DESC superset).
struct ResourceDescription {
    u32 dimension{0};
    u64 alignment{0};
    u64 width{0};
    u32 height{0};
    u32 depth_or_array_size{0};
    u32 mip_levels{0};
    u32 format{0};
    u32 sample_count{0};
    u32 sample_quality{0};
    u32 layout{0};
    u32 flags{0};
    /// D3D12_MIP_REGION (RESOURCE_DESC1 only).
    u32 mip_region[3]{};
    /// A buffer created with D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_
    /// STRUCTURE: GetDesc no longer names the flag (measured, a02), but the
    /// acceleration-structure barrier accesses still need it.
    bool acceleration_structure{false};
};

/// DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE (189) /
/// SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE (190), dxgiformat.h.
inline constexpr u32 kFeedbackMinMipFormat = 189;
inline constexpr u32 kFeedbackRegionUsedFormat = 190;
[[nodiscard]] inline bool is_feedback_format(u32 f) {
    return f == kFeedbackMinMipFormat || f == kFeedbackRegionUsedFormat;
}

struct Resource final : Object {
    Resource() : Object(Kind::Resource) {}
    ResourceDescription desc{};
    HeapProperties heap_properties{};
    u32 heap_flags{0};
    /// D3D12_COMPATIBILITY_SHARED_FLAGS (ID3D12CompatibilityDevice).
    u32 compatibility_flags{0};
    bool committed{false};
    bool placed{false};
    bool reserved{false};
    /// Placed: the ID3D12Heap (no COM reference: see create_placed_resource).
    GuestAddr heap_object{0};
    /// Buffers (and placed textures): the memory and this resource's offset in
    /// it. A buffer's GPU VA is memory->va + offset.
    std::shared_ptr<Memory> memory;
    u64 offset{0};
    /// Reserved buffer: sparse address space, no placement-heap window.
    std::unique_ptr<graphics::BackendBuffer> tiled_buffer;
    std::shared_ptr<ReservedVa> reserved_va;
    std::unique_ptr<ReservedTiles> tiles;
    std::shared_ptr<graphics::BackendTexture2D> texture;
    std::shared_ptr<graphics::BackendTexture3D> texture3d;
    graphics::PixelFormat pixel_format{graphics::PixelFormat::Unknown};
    u32 initial_state{0};
    /// A sampler feedback map (formats 189 / 190): no backend texture; its
    /// opaque storage (graphics/shaders/sampler_feedback_layout.hpp) lives
    /// in `memory` at `offset`, like a buffer's, and its UAVs are raw views
    /// of it.
    bool feedback_map{false};
    graphics::sampler_feedback::Geometry feedback{};
    /// Outstanding Map calls (D3D12 maps are reference counted).
    u32 map_count{0};
    /// A swap chain back buffer: the chain that owns it (IDXGISwapChain).
    GuestAddr swap_chain{0};
    /// Typed buffer views (Buffer<T> / RWBuffer<T>), created on first use and
    /// owned here so they die with the resource.
    std::map<std::tuple<u32, u64, u32, bool, u16>,
             std::unique_ptr<graphics::BackendTextureBuffer>>
        typed_views;

    [[nodiscard]] bool is_buffer() const {
        return desc.dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    }
    [[nodiscard]] u64 gpu_va() const {
        if (!is_buffer()) return 0;
        if (reserved_va) return reserved_va->va;
        return memory ? memory->va + offset : 0;
    }
    [[nodiscard]] graphics::BackendBuffer* buffer() const {
        if (feedback_map) return memory && memory->backend ? memory->backend->buffer() : nullptr;
        if (!is_buffer()) return nullptr;
        if (tiled_buffer) return tiled_buffer.get();
        return memory && memory->backend ? memory->backend->buffer() : nullptr;
    }
    [[nodiscard]] u32 array_size() const {
        return desc.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                   ? 1u
                   : desc.depth_or_array_size;
    }
    [[nodiscard]] u32 subresource_count() const;
};

/// One D3D12_GLOBAL_BARRIER, D3D12_TEXTURE_BARRIER or D3D12_BUFFER_BARRIER
/// as an app wrote it (fields a kind does not have stay 0).
struct EnhancedBarrier {
    u32 sync_before{0};
    u32 sync_after{0};
    u32 access_before{0};
    u32 access_after{0};
    u32 layout_before{0};
    u32 layout_after{0};
    u32 index_or_first_mip{0};
    u32 num_mips{0};
    u32 first_slice{0};
    u32 num_slices{0};
    u32 first_plane{0};
    u32 num_planes{0};
    u32 flags{0};
    u64 offset{0};
    u64 size{0};
};

/// The runtime's checks of enhanced barriers and initial layouts
/// (d3d12_barrier.cpp): nullptr when valid, else why not.
[[nodiscard]] const char* barrier_sync_access_violation(const EnhancedBarrier& b,
                                                        u32 list_type);
[[nodiscard]] const char* buffer_barrier_violation(const EnhancedBarrier& b,
                                                   const Resource* r,
                                                   u32 list_type);
[[nodiscard]] const char* texture_barrier_violation(const EnhancedBarrier& b,
                                                    const Resource* r,
                                                    u32 list_type);
[[nodiscard]] const char* initial_layout_violation(const ResourceDescription& d,
                                                   u32 layout);
/// Whether a valid enhanced barrier of D3D12_BARRIER_TYPE @p type ends
/// render-target or depth-stencil writes (d3d12_barrier.cpp): the
/// enhanced-barrier form of ResourceBarrier's transition out of
/// RENDER_TARGET / DEPTH_WRITE, after which the backend's
/// attachment_barrier must make the rendered texels visible.
[[nodiscard]] bool ends_attachment_writes(const EnhancedBarrier& b, u32 type);

// -----------------------------------------------------------------------------
// Descriptors
// -----------------------------------------------------------------------------

/// D3D12_SAMPLER_DESC2 (the superset of D3D12_SAMPLER_DESC and of a static
/// sampler), as an app wrote it.
struct SamplerState {
    u32 filter{0};
    u32 address_u{0};
    u32 address_v{0};
    u32 address_w{0};
    float mip_lod_bias{0.0f};
    u32 max_anisotropy{0};
    u32 comparison_func{0};
    std::array<u32, 4> border{};  ///< float bits, or uints with UINT_BORDER
    float min_lod{0.0f};
    float max_lod{0.0f};
    u32 flags{0};
};

enum class DescriptorType : u8 { None, Cbv, Srv, Uav, Rtv, Dsv, Sampler };

/// One descriptor-heap record: a view description as Create*View saw it. The
/// resource is named by address and serial, never by pointer, so a record left
/// behind by a released resource resolves to nothing instead of freed memory.
struct Descriptor {
    DescriptorType type{DescriptorType::None};
    /// True when the view was created with a null description (the
    /// resource's own format and dimension).
    bool default_view{false};
    u32 dimension{0};
    u32 format{0};
    GuestAddr resource{0};
    u64 resource_serial{0};
    GuestAddr counter{0};
    u64 counter_serial{0};
    u64 counter_offset{0};
    u32 component_mapping{0};
    u32 flags{0};
    /// Buffer views: FirstElement / NumElements / StructureByteStride. A CBV
    /// keeps its GPU VA in `first_element` and SizeInBytes in `num_elements`.
    u64 first_element{0};
    u32 num_elements{0};
    u32 structure_stride{0};
    /// Texture views: mip and slice windows (W slices for Texture3D UAV/RTV).
    u32 most_detailed_mip{0};
    u32 mip_levels{0};
    u32 first_slice{0};
    u32 array_size{0};
    u32 plane_slice{0};
    float min_lod{0.0f};
    SamplerState sampler{};
};

/// Resource binding tier 2 (Learn, "Hardware Tiers": a stage's SRVs reach
/// the whole heap): a shader-visible CBV/SRV/UAV heap as the GPU reads it.
/// `table` is a persistent backend table of records.size() + 1 slots, slot k
/// heap descriptor k and the last one always null (the sentinel a heap
/// window's out-of-range element reads, DxbcTranslationOptions::heap_window).
/// `side` holds kDescriptorHeapWindowSideWords uints per slot: the view
/// entry Replayer::bind_view returns, and 1 for a null descriptor. A write
/// to the heap marks its slot dirty; the replay resolves the dirty slots
/// before a draw that reads the window, so a draw costs O(1) in the heap's
/// size. Every CBV, SRV and UAV record is resolved as its own view type; an
/// empty (never written) or sampler record is null in the window.
///
/// `aux`: the table has 2 * (count + 1) slots, slot count + 1 + k holding
/// heap slot k's auxiliary descriptor (graphics::kDescriptorHeapWindowAux
/// Binding: a texture view's residency companion, a UAV's counter). Made
/// by the first draw of a shader that reads it; the table it replaces is
/// kept in `retired` (draws recorded before still name it).
/// `residency_id[k]` is heap slot k's buffer residency record id (side
/// word graphics::kDescriptorHeapWindowResidencyWord), `residency_slot[r -
/// 1]` the slot of id r (~0u: free).
struct HeapWindowMirror {
    std::unique_ptr<graphics::BackendDescriptorTable> table;
    std::unique_ptr<graphics::BackendHeap> side;
    u32* side_words{nullptr};
    u32 count{0};
    bool aux{false};
    std::vector<std::unique_ptr<graphics::BackendDescriptorTable>> retired;
    std::vector<u8> dirty;
    std::vector<u32> dirty_list;
    /// Slots holding an acceleration-structure SRV, re-resolved when the
    /// VA -> structure map changes (Frontend::m_rt_generation).
    std::vector<u32> rt_slots;
    u64 rt_generation{0};
    std::vector<u32> residency_id;
    std::vector<u32> residency_slot;
};

struct DescriptorHeap final : Object {
    DescriptorHeap() : Object(Kind::DescriptorHeap) {}
    u32 type{0};
    u32 flags{0};
    u32 node_mask{0};
    GuestAddr cpu_base{0};
    usize cpu_reserved{0};
    /// 0 unless shader visible.
    u64 gpu_base{0};
    std::vector<Descriptor> records;
    /// The heap window mirror, made by the first replay that reads it.
    /// window_mutex orders descriptor writes (any thread, Learn: descriptor
    /// creation is free-threaded) against the replay's resolve.
    std::mutex window_mutex;
    std::unique_ptr<HeapWindowMirror> window;
    /// After a write of @p record: its window slot needs resolving.
    /// Bumped by every record write (Create*View, CreateSampler,
    /// CopyDescriptors*): a replay that copied records before can tell
    /// whether any changed since (ReplayResources::sampler_memo).
    std::atomic<u64> generation{0};
    /// Store @p count records from @p values at @p first (records of this
    /// heap). Descriptor writes run under the shared frontend lock
    /// (d3d12_lock.hpp): a shader-visible heap's records are written under
    /// window_mutex, which the replay's window resolve reads them under, so
    /// a resolve never reads a slot mid-write (the slot may be written again
    /// while an unrelated list executes). Other heaps are never read by a
    /// replay; their writers own their slots (Learn: one descriptor is
    /// never written from two threads at once).
    void write(Descriptor* first, const Descriptor* values, usize count) {
        if (gpu_base == 0) {
            for (usize k = 0; k < count; ++k) first[k] = values[k];
            generation.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::lock_guard<std::mutex> lock(window_mutex);
        for (usize k = 0; k < count; ++k) {
            first[k] = values[k];
            mark_dirty_locked(first + k);
        }
        generation.fetch_add(1, std::memory_order_relaxed);
    }
    void write(Descriptor* record, const Descriptor& value) {
        write(record, &value, 1);
    }

private:
    void mark_dirty_locked(const Descriptor* record) {
        if (window == nullptr || record < records.data()) return;
        const usize index = static_cast<usize>(record - records.data());
        if (index >= records.size() || index >= window->dirty.size() ||
            window->dirty[index] != 0) {
            return;
        }
        window->dirty[index] = 1;
        window->dirty_list.push_back(static_cast<u32>(index));
    }

public:
};

// -----------------------------------------------------------------------------
// Root signatures and pipelines
// -----------------------------------------------------------------------------

/// A root signature resolved for binding: every descriptor range carries its
/// absolute offset from the start of its table.
struct BindingRange {
    u32 type{0};  ///< D3D12_DESCRIPTOR_RANGE_TYPE
    u32 count{0};  ///< UINT_MAX = unbounded
    u32 base_register{0};
    u32 space{0};
    u32 flags{0};
    u32 table_offset{0};
};

struct BindingParameter {
    u32 type{0};  ///< D3D12_ROOT_PARAMETER_TYPE
    u32 visibility{0};
    std::vector<BindingRange> ranges;
    u32 shader_register{0};
    u32 space{0};
    u32 num_32bit_values{0};
    u32 flags{0};
};

struct BindingSampler {
    SamplerState state{};
    u32 shader_register{0};
    u32 space{0};
    u32 visibility{0};
};

struct RootLayout {
    u32 flags{0};
    std::vector<BindingParameter> parameters;
    std::vector<BindingSampler> static_samplers;
};

struct RootSignature final : Object {
    RootSignature() : Object(Kind::RootSignature) {}
    /// FNV-1a of `blob`: CreateRootSignature of an identical blob returns
    /// this object again (measured, a05 pso "twice from one blob").
    u64 blob_hash{0};
    std::shared_ptr<const RootLayout> layout;
    std::vector<u8> blob;
    u32 node_mask{0};
};

struct PipelineState;

/// ID3D12StateObject (d3d12_state_object.cpp): a raytracing pipeline or a
/// collection, as its subobjects describe it, and for a pipeline the
/// megakernel it compiles to (graphics/shaders/dxil_raytracing.hpp).
/// The binding form of a bare serialized root signature (a DXIL library's
/// root-signature subobject), validated as CreateRootSignature validates a
/// blob; null with @p error set when it is refused (d3d12_pipeline.cpp).
[[nodiscard]] std::shared_ptr<const RootLayout> root_layout_from_part(const u8* part, usize size, u32& error);

/// Bytes 0..3 of a Vitreo shader identifier ("VRSI"); its index is at 8.
inline constexpr u32 kRtIdentifierMagic = 0x49535256u;

struct StateObject final : Object {
    StateObject() : Object(Kind::StateObject) {}
    ~StateObject() override;
    u32 type{0};
    u32 flags{0};
    struct Library {
        std::vector<u8> bytes;
        std::vector<std::pair<std::string, std::string>> exports;
    };
    std::vector<Library> libraries;
    std::vector<graphics::DxilRtProgramDesc::HitGroup> hit_groups;
    bool has_shader_config{false};
    u32 max_payload{0};
    u32 max_attributes{0};
    bool has_pipeline_config{false};
    u32 max_recursion{0};
    u32 pipeline_flags{0};
    std::shared_ptr<const RootLayout> global_root;
    bool has_global_root{false};
    /// Local root signature of each export (shader or hit group).
    std::map<std::string, std::shared_ptr<const RootLayout>> local_roots;
    /// Every export: its DXIL shader kind (hit groups: 0).
    std::map<std::string, u32> kinds;
    /// Identifier index of each raygen / miss / callable / hit group,
    /// unique device-wide and kept by AddToStateObject children.
    std::map<std::string, u32> indices;
    /// 32 bytes per index entry, in guest memory (GetShaderIdentifier).
    GuestAddr identifiers{0};
    usize identifier_bytes{0};
    std::map<std::string, u32> identifier_slot;
    /// Global root signature of each shader export.
    std::map<std::string, std::shared_ptr<const RootLayout>> global_roots;
    std::shared_ptr<PipelineState> kernel;
    /// The megakernel's local root descriptors (dxil_raytracing.hpp): by
    /// identifier index, (view array k, offset of the VA after the
    /// identifier) of each one its record carries; the arrays' kinds.
    std::map<u32, std::vector<std::pair<u32, u32>>> record_views;
    std::vector<graphics::DxilRtProgram::LocalView> local_views;
    u64 stack_size{0};
};

/// Register usage of one translated shader, per register class.
struct ShaderRegisters {
    u32 cbv_slots{0};
    u32 srv_slots{0};
    u32 sampler_slots{0};
    u32 uav_slots{0};
};

struct CompiledShader {
    bool present{false};
    std::string msl;
    ShaderRegisters registers{};
    graphics::BackendMathMode math_mode{graphics::BackendMathMode::Fast};
    bool preserve_invariance{false};
    u32 threads_x{1};
    u32 threads_y{1};
    u32 threads_z{1};
    /// Vertex shader input signature (register, semantic, component type).
    struct Input {
        std::string semantic;
        u32 semantic_index{0};
        u32 register_num{0};
        u32 component_type{0};
        /// D3D_NAME of a system value (SV_VertexID ...); 0 for a semantic
        /// the input assembler must supply.
        u32 system_value{0};
    };
    std::vector<Input> inputs;
    /// Pixel shader: bit n set when it writes SV_Target n.
    u32 target_mask{0};
    /// It writes SV_RenderTargetArrayIndex (D3D_NAME 4) itself.
    bool writes_layer{false};
    /// Pixel shader: each v#'s interpolation mode
    /// (DxbcTranslationResult::pixel_input_interpolation).
    std::array<u8, 32> input_interpolation{};
    /// Pixel shader: the v# registers GetAttributeAtVertex reads
    /// (DxbcTranslationResult::per_vertex_inputs).
    u32 per_vertex_inputs{0};
    /// Pixel shader: the SV_Barycentrics pass C delivers
    /// (DxbcTranslationResult::raster_barycentrics).
    u32 raster_barycentrics{0};
    /// Pixel shader: translated with RasterEmulationOptions::edges_in_buffer
    /// (its inputs and pass C's edge data exceed Metal's 124 components).
    bool raster_edges_in_buffer{false};
    std::vector<u8> bytecode;
    /// Every binding the shader declares (DXBC dcl_*), for the pipeline's
    /// root-signature coverage check.
    std::vector<graphics::DxbcBindingDeclaration> declarations;
    /// Parallel to `declarations`: for a constant buffer, the 32-bit values
    /// its RDEF variables span (what root constants bound to it must hold);
    /// 0 when unknown (reflection stripped) or not a constant buffer.
    std::vector<u32> cb_values;
    /// Shader model 5.1: the descriptor-table translation, whose slots are
    /// these runs of (space, register) per set (b#, t#, s#, u#); a 5.0
    /// shader's slot is its register in space 0.
    bool table_mode{false};
    struct Run {
        u32 space{0};
        u32 first_register{0};
        u32 count{0};
        u32 first_slot{0};
    };
    std::array<std::vector<Run>, 4> runs;
    /// table_mode: the CBV-table slot of the sampler-state records
    /// (DxbcTranslationOptions::sampler_state_buffer), ~0u when none.
    u32 sampler_state_slot{~0u};
    /// table_mode: CBV-table slots of the raw/structured view-size buffers
    /// (DxbcTranslationOptions::view_size_buffer), ~0u when the stage has
    /// no such SRVs or UAVs.
    u32 srv_view_size_slot{~0u};
    u32 uav_view_size_slot{~0u};
    /// table_mode: CBV-table slot of the constant-buffer vector counts
    /// (DxbcTranslationOptions::constant_buffer_limits), ~0u when the stage
    /// declares no constant buffers.
    u32 cbv_limit_slot{~0u};
    /// CBV-table slots of the null-descriptor flags
    /// (DxbcTranslationOptions::null_descriptors), ~0u when the stage has
    /// no texture or typed-buffer views to query.
    u32 srv_null_slot{~0u};
    u32 uav_null_slot{~0u};
    /// table_mode: CBV-table slots of the typed-buffer element-bias buffers
    /// (DxbcTranslationOptions::typed_buffer_bias_buffer), ~0u when the
    /// stage declares no Buffer<T> or RWBuffer<T>.
    u32 srv_typed_bias_slot{~0u};
    u32 uav_typed_bias_slot{~0u};
    /// table_mode: the t#, u# and b# arrays that read the shader-visible
    /// CBV/SRV/UAV heap itself (resource binding tiers 2 and 3,
    /// DxbcTranslationOptions::heap_window) instead of a copy in their
    /// class's table: every unbounded one, every one longer than
    /// kHeapWindowMinArray, and the SM 6.6 heap-indexed ones. They are in
    /// no run; the replay binds the heap's argument buffer as descriptor set
    /// BACKEND_DESCRIPTOR_SET_HEAP_WINDOW, the params words (limit, then one
    /// base per segment) at CBV slot params_slot and the heap's side buffer
    /// at CBV slot side_slot.
    struct HeapWindow {
        /// One piece of a window array a root-signature range supplies:
        /// the base word holds the first heap slot of the table bound at
        /// root parameter `parameter`, plus `table_offset`. `parameter`
        /// kHeapStart: heap slot 0 (SM 6.6 ResourceDescriptorHeap).
        struct Segment {
            u32 parameter{0};
            u32 table_offset{0};
            u32 base_word{0};
        };
        static constexpr u32 kHeapStart = ~0u;
        bool present{false};
        /// A u# window: draws declare the heap's resources written.
        bool writes{false};
        u32 params_slot{~0u};
        u32 side_slot{~0u};
        /// Tiled status: the heap's buffer residency records (by the side
        /// record's id word) at this CBV slot. ~0u when absent.
        u32 residency_slot{~0u};
        /// The shader reads the window's auxiliary region (residency
        /// companions, UAV counters): the heap's mirror keeps one.
        bool aux{false};
        std::vector<Segment> segments;
    };
    HeapWindow heap_window;
    /// The translation has typed UAV atomics (MSL 3.1 texture atomics; a
    /// view read as raw memory runs them on its buffer words).
    bool typed_uav_atomics{false};
    /// (descriptor set, first binding, count) of each float typed UAV range
    /// declared as a uint image for its atomics (DxbcTranslationOptions::
    /// typed_uav_uint_views): those slots take an R32_UINT view.
    std::vector<std::array<u32, 3>> uint_view_uavs;
    [[nodiscard]] bool uint_view_uav(u32 set, u32 slot) const {
        for (const auto& r : uint_view_uavs) {
            if (r[0] == set && slot >= r[1] && slot - r[1] < r[2]) return true;
        }
        return false;
    }
    /// Tiled resources tier 2 (DxbcTranslationOptions::tiled_status), set
    /// when the shader uses a status instruction: SRV-table slot of t# slot
    /// k's residency companion is srv_companion_base + k and of u# slot k's
    /// uav_companion_base + k; sampler-table slot of s# slot k's status
    /// twin is status_sampler_base + k; CBV-table slots of the buffer
    /// residency records. ~0u when absent.
    bool tiled_status{false};
    u32 srv_companion_base{~0u};
    u32 uav_companion_base{~0u};
    u32 status_sampler_base{~0u};
    /// The s# slots that have a status twin (the real sampler slots).
    u32 status_sampler_count{0};
    u32 srv_residency_slot{~0u};
    u32 uav_residency_slot{~0u};
    /// The declared D3D10_SB_RESOURCE_DIMENSION of each SRV / UAV table
    /// slot (0 where none): the shape of a companion slot's texture.
    std::vector<u8> srv_slot_dimension;
    std::vector<u8> uav_slot_dimension;
    /// 1 for each SRV table slot declared as a RaytracingAccelerationStructure
    /// (dxbc_vitreo_ext.hpp kOpDclAccelerationStructure).
    std::vector<u8> srv_slot_acceleration;
    /// A raytracing megakernel's reserved bindings (space
    /// graphics::kDxilRtSpace): the shader tables (what 0..3), the dispatch
    /// constants (4), the blob of each element of an acceleration-structure
    /// range (5: `count` elements from SRV slot `as_slot`), a local view
    /// array (6: array `as_slot`, `count` elements), the local translation
    /// buffer (7) and a local static sampler (8: rt_samplers[as_slot]).
    struct RtReserved {
        u32 set{0};
        u32 slot{0};
        u32 what{0};
        u32 as_slot{0};
        u32 count{1};
    };
    std::vector<RtReserved> rt_reserved;
    /// The local static samplers of a megakernel (what 8).
    std::vector<SamplerState> rt_samplers;
    /// imm_atomic_alloc / imm_atomic_consume, in binding order. Entry k is
    /// descriptor set SPIRV_UAV_COUNTER_DESCRIPTOR_SET binding k and names
    /// u# `uav_counter_registers[k]` in `uav_counter_spaces[k]`. Empty when
    /// the shader does not move a counter.
    std::vector<u32> uav_counter_registers;
    std::vector<u32> uav_counter_spaces;
    /// The table slot of (@p set, @p space, @p reg); ~0u when the shader
    /// does not declare it.
    [[nodiscard]] u32 slot_of(u32 set, u32 space, u32 reg) const {
        if (!table_mode) return space == 0 ? reg : ~0u;
        for (const Run& run : runs[set]) {
            if (run.space == space && reg >= run.first_register &&
                reg - run.first_register < run.count) {
                return run.first_slot + (reg - run.first_register);
            }
        }
        return ~0u;
    }
};

/// D3D12_INPUT_ELEMENT_DESC as the PSO holds it.
struct InputElement {
    std::string semantic;
    u32 semantic_index{0};
    u32 format{0};
    u32 input_slot{0};
    u32 aligned_offset{0};
    u32 slot_class{0};
    u32 step_rate{0};
};

struct RenderTargetBlend {
    bool blend_enable{false};
    bool logic_op_enable{false};
    u32 src_blend{0}, dest_blend{0}, blend_op{0};
    u32 src_blend_alpha{0}, dest_blend_alpha{0}, blend_op_alpha{0};
    u32 logic_op{0};
    u32 write_mask{0};
};

struct GraphicsState {
    bool alpha_to_coverage{false};
    bool independent_blend{false};
    std::array<RenderTargetBlend, 8> blend{};
    u32 sample_mask{0};
    // D3D12_RASTERIZER_DESC
    u32 fill_mode{0};
    u32 cull_mode{0};
    bool front_ccw{false};
    /// D3D12_RASTERIZER_DESC INT DepthBias; DESC1/2 carry a FLOAT.
    float depth_bias{0.0f};
    float depth_bias_clamp{0.0f};
    float slope_scaled_depth_bias{0.0f};
    bool depth_clip{true};
    bool multisample{false};
    /// D3D12_LINE_RASTERIZATION_MODE_QUADRILATERAL_NARROW (RASTERIZER2):
    /// quadrilateral lines 1.0 wide instead of 1.4.
    bool narrow_quad_line{false};
    bool antialiased_line{false};
    u32 forced_sample_count{0};
    u32 conservative{0};
    // D3D12_DEPTH_STENCIL_DESC1
    bool depth_enable{false};
    u32 depth_write_mask{0};
    u32 depth_func{0};
    bool stencil_enable{false};
    u8 stencil_read_mask{0xFF};
    u8 stencil_write_mask{0xFF};
    struct StencilFace {
        u32 fail{0}, depth_fail{0}, pass{0}, func{0};
    } front{}, back{};
    bool depth_bounds_test{false};
    std::vector<InputElement> input_layout;
    u32 strip_cut{0};
    u32 topology_type{0};
    u32 num_render_targets{0};
    std::array<u32, 8> rtv_formats{};
    u32 dsv_format{0};
    u32 sample_count{1};
    u32 sample_quality{0};
    /// Backend vertex stream of each D3D12 input slot (0xFF: not read).
    std::array<u8, 32> slot_stream{};
    std::array<u8, graphics::BACKEND_MAX_VERTEX_BUFFERS> stream_slot{};
    u32 stream_count{0};
    /// The layout reads more slots than Metal has vertex buffer indices:
    /// every draw fetches through the vertex-pull table (stream s = slot s).
    bool vertex_pulling{false};
};

/// One D3D12_SO_DECLARATION_ENTRY after it has been checked against the
/// producing shader. `output_register` is the o# the repack reads;
/// `dst_dword` is where those components sit in the output vertex.
struct StreamOutputEntry {
    u32 stream{0};
    u32 slot{0};
    u32 start_component{0};
    u32 component_count{0};
    u32 dst_dword{0};
    u32 output_register{0};
    bool gap{false};
};

/// D3D12_STREAM_OUTPUT_DESC kept by the pipeline. `registers` is the
/// producing stage's output registers in the capture/expansion record
/// order (ascending o#). `stream_slots[s]` is the buffers stream s writes.
struct StreamOutput {
    bool active{false};
    std::vector<StreamOutputEntry> entries;
    std::array<u32, D3D12_SO_BUFFER_SLOT_COUNT> strides{};
    std::array<u32, D3D12_SO_STREAM_COUNT> stream_slots{};
    u32 rasterized_stream{0};
    std::vector<u32> registers;
};

struct PipelineState final : Object {
    PipelineState() : Object(Kind::PipelineState) {}
    /// What GetCachedBlob hands out and a CachedPSO must match: a digest of
    /// the description the pipeline was created from.
    u64 cache_digest{0};
    bool compute{false};
    /// Made by ID3D12Device2::CreatePipelineState (a stream): a pipeline
    /// library keeps the two paths apart (a11 library: a stream-made PSO
    /// does not load through LoadComputePipeline, nor a desc-made one
    /// through LoadPipeline).
    bool from_stream{false};
    std::shared_ptr<const RootLayout> root;
    CompiledShader vs, ps, cs;
    GraphicsState graphics{};
    u32 flags{0};
    u32 node_mask{0};
    /// Backend render pipelines by the vertex strides bound at draw time
    /// (D3D12 strides are dynamic; Metal bakes them into the pipeline).
    std::unordered_map<u64, std::unique_ptr<graphics::BackendPipeline>>
        variants;
    /// The plain draws' render pipeline with strides from the draw (built at
    /// creation, Frontend::precompile_draw_functions; null where the backend
    /// has no dynamic strides), and the input streams it reads (bit s). A
    /// draw binding one of those with stride 0 (a constant stream) or not a
    /// multiple of 4 uses `variants`.
    std::unique_ptr<graphics::BackendPipeline> dynamic_variant;
    u32 dynamic_stream_mask{0};
    std::unique_ptr<graphics::BackendComputePipeline> compute_pipeline;
    /// Vertex shader translated with capture_vertex_outputs_to_buffer: the
    /// input records a lowered geometry or hull kernel reads. Empty when
    /// this pipeline has neither stage.
    std::string capture_msl;
    std::vector<u32> capture_registers;
    graphics::BackendMathMode capture_math{graphics::BackendMathMode::Relaxed};
    std::unordered_map<u64, std::unique_ptr<graphics::BackendPipeline>>
        capture_variants;
    /// Geometry shader lowered to a compute expansion plus the pass-through
    /// vertex shader that rasterizes it (the D3D11 three-pass path).
    struct GeometryProgram {
        bool active{false};
        CompiledShader kernel;
        std::string passthrough_msl;
        u32 input_primitive{0};
        u32 output_topology{0};
        u32 max_output_vertices{0};
        u32 instance_count{1};
        u32 stream_mask{1};
        u32 output_record_vec4s{0};
        u32 vertices_per_invocation{0};
        std::shared_ptr<graphics::BackendComputePipeline> pipeline;
        std::unique_ptr<graphics::BackendPipeline> passthrough;
    } geometry;
    /// Hull and domain shaders lowered to the software tessellator
    /// (emulate_tessellation + domain_to_compute).
    struct TessellationProgram {
        bool active{false};
        CompiledShader hull;
        CompiledShader domain_stage;
        std::string passthrough_msl;
        /// SV_CullDistance: the pass-through is drawn non-indexed over the
        /// tessellator's index list, bound at its t1 (passthrough_for).
        bool passthrough_through_index_list{false};
        u32 tess_domain{0};
        u32 partitioning{0};
        u32 output_primitive{0};
        u32 input_control_points{0};
        u32 output_control_points{0};
        float max_tess_factor{64.f};
        u32 cp_record_vec4s{1};
        u32 pc_record_vec4s{1};
        std::vector<u32> domain_records;
        std::shared_ptr<graphics::BackendComputePipeline> hull_pipeline;
        std::shared_ptr<graphics::BackendComputePipeline> domain_pipeline;
        std::unique_ptr<graphics::BackendPipeline> passthrough;
    } tessellation;
    StreamOutput stream_output;
    /// A mesh shader pipeline (DirectX-Specs "Mesh Shader";
    /// d3d12_pipeline_mesh.cpp): the amplification and mesh shaders, what
    /// the mesh shader stages for the pixel shader, and the Metal object +
    /// mesh pipeline built at creation.
    struct MeshProgram {
        bool active{false};
        CompiledShader as;
        CompiledShader ms;
        graphics::MeshStageLayout ms_layout{};
        graphics::MeshStageLayout as_layout{};
        u32 payload_words{0};
        std::unique_ptr<graphics::BackendPipeline> pipeline;
        /// The PIPELINE_STATISTICS variant (Frontend::mesh_statistics_pipeline):
        /// both shaders count into a record, the pixel shader is the
        /// counting one.
        std::unique_ptr<graphics::BackendPipeline> statistics_pipeline;
        bool statistics_tried{false};
        /// ViewInstancingTier 3 (Frontend::mesh_view_pipeline), by
        /// MeshViewPipeline kind and [statistics]; tried once each.
        std::array<std::array<std::unique_ptr<graphics::BackendPipeline>, 2>, 5> view_pipelines;
        std::array<std::array<bool, 2>, 5> view_tried{};
    } mesh;
    /// PIPELINE_STATISTICS variants, built while a statistics query is open
    /// (Frontend::statistics_variant): the vertex shader also writes its
    /// outputs draw-relative into the draw's capture buffer (the clipper's
    /// input, CPrimitives) and the pixel shader counts PSInvocations into
    /// the interval's record. Keyed like `variants`.
    std::unordered_map<u64, std::unique_ptr<graphics::BackendPipeline>>
        statistics_variants;
    /// The vertex shader's captured registers in record order, and where
    /// SV_Position / SV_ClipDistance sit among them (invalid: CPrimitives
    /// cannot be counted for this pipeline).
    std::vector<u32> statistics_capture_registers;
    graphics::StatsClipLayout statistics_clip{};
    /// The pixel shader translated with count_pixel_invocations
    /// (Frontend::statistics_fragment); statistics_pixels false when that
    /// translation failed and PSInvocations cannot be counted.
    std::string statistics_fragment_msl;
    bool statistics_fragment_tried{false};
    bool statistics_pixels{false};
    /// Pass-through pipelines with the counting pixel shader.
    std::unique_ptr<graphics::BackendPipeline> geometry_statistics_passthrough;
    std::unique_ptr<graphics::BackendPipeline> domain_statistics_passthrough;
    /// The geometry shader's (and, without one, the domain shader's) clip
    /// layout per stream, over its expansion records.
    std::array<graphics::StatsClipLayout, 4> lowered_clip{};
    /// Target-independent or conservative rasterization
    /// (graphics/shaders/raster_emulation.hpp): the pixel shader was
    /// translated for it, and every draw goes through a vertex capture (or
    /// the lowered stages) to the raster pass C -- `vertex_msl` for a
    /// pipeline with no geometry or tessellation stage, else the
    /// geometry / tessellation program's passthrough_msl.
    /// A vertex shader writing SV_CullDistance with no geometry or
    /// tessellation stage after it (Metal has no cull distances): every draw
    /// captures the vertex shader's outputs and draws them through
    /// `vertex_msl`, a pass C that reads each primitive's records through
    /// the assembled record ids and discards a primitive whose cull distance
    /// is negative at all its vertices (D3D11.3 15.4.2).
    struct VertexCullProgram {
        bool active{false};
        std::string vertex_msl;
        std::unique_ptr<graphics::BackendPipeline> pipeline;
    } vertex_cull;
    struct RasterProgram {
        bool active{false};
        graphics::RasterEmulationOptions ps{};
        std::string vertex_msl;
        std::unique_ptr<graphics::BackendPipeline> pipeline;
        /// Pass C with the counting pixel shader (statistics_fragment), per
        /// pass-C pipeline slot it stands in for, while a
        /// PIPELINE_STATISTICS query is open.
        std::map<const void*, std::unique_ptr<graphics::BackendPipeline>>
            statistics;
        /// Where SV_Position / SV_ClipDistance sit in the vertex capture's
        /// records (capture_registers), for the clipper counts of a draw of
        /// the vertex shader's own triangles; invalid without SV_Position.
        graphics::StatsClipLayout clip{};
        /// The rasterized primitive pass C draws (RasterPrimitive), set by the
        /// lowering; the pixel shader is translated again when it differs
        /// from ps.primitive.
        graphics::RasterPrimitive primitive{graphics::RasterPrimitive::Triangle};
        /// ps.edges_in_buffer: each pass-C vertex MSL's edge kernel
        /// (generate_raster_emulation_edge_kernel_msl), and its pipeline.
        std::unordered_map<std::string, std::string> edge_kernel_msl;
        std::unordered_map<std::string, std::shared_ptr<graphics::BackendComputePipeline>>
            edge_kernels;
    } raster;
    /// GetAttributeAtVertex (CompiledShader::per_vertex_inputs) with no
    /// geometry, tessellation or raster-emulation stage: every draw captures
    /// the vertex shader and draws this pass-through over the assembled
    /// triangles' record ids (gs_passthrough.hpp, per_vertex_registers).
    std::string per_vertex_msl;
    std::unique_ptr<graphics::BackendPipeline> per_vertex_pipeline;
    /// The same with the counting pixel shader (PIPELINE_STATISTICS).
    std::unique_ptr<graphics::BackendPipeline> per_vertex_statistics_pipeline;
    /// An adjacency topology with no geometry shader (Frontend::
    /// ensure_vertex_replay, built at the first such draw): the vertex
    /// capture's pass-through over the non-adjacent vertices' record ids.
    std::string adjacency_msl;
    std::unique_ptr<graphics::BackendPipeline> adjacency_pipeline;
    std::unique_ptr<graphics::BackendPipeline> adjacency_statistics_pipeline;
    bool adjacency_tried{false};
    /// Vertices of its primitive: 1 point, 2 line, 3 triangle.
    u32 per_vertex_primitive{0};
    /// View instancing (D3D12_VIEW_INSTANCING_DESC; DirectX-Specs "View
    /// Instancing"): 0 when disabled, else the view count (1..4). Draws
    /// loop over the views (ViewInstancingTier 2, draw-level looping).
    u32 view_instance_count{0};
    u32 view_instancing_flags{0};
    /// Each view's (ViewportArrayIndex, RenderTargetArrayIndex).
    std::array<std::pair<u32, u32>, 4> view_locations{};
    /// The SV_ViewID this pipeline's shaders were translated with.
    u32 view_id{0};
    /// Views 1..count-1 (view 0 is this pipeline).
    std::array<std::unique_ptr<PipelineState>, 4> views;
    /// ViewInstancingTier 3 (DirectX-Specs "View Instancing": "the
    /// implementation of view instancing always occurs at the first shader
    /// stage that references SV_ViewID", or the rasterizer): where the
    /// per-view work starts (kViLoop: the whole draw per view). Views share
    /// the value of view 0.
    u32 vi_first{0};
};

/// Frontend::mesh_view_pipeline kinds.
inline constexpr u32 kMeshViewAsCapture = 0;     ///< AS capture-only (+ MS, PS)
inline constexpr u32 kMeshViewStub = 1;          ///< AS replay + this MS + PS
inline constexpr u32 kMeshViewStubCapture = 2;   ///< AS replay (base) + MS capture + PS
inline constexpr u32 kMeshViewMsCapture = 3;     ///< MS capture + PS (no AS)
inline constexpr u32 kMeshViewReplay = 4;        ///< MS output replay + PS

/// PipelineState::vi_first: the first stage that runs once per view.
inline constexpr u32 kViLoop = 0;        ///< not instanced, or the VS / AS
inline constexpr u32 kViHull = 1;        ///< VS once; HS.. per view
inline constexpr u32 kViDomain = 2;      ///< VS, HS, tessellator once
inline constexpr u32 kViGeometry = 3;    ///< VS (, HS, DS) once
inline constexpr u32 kViRaster = 4;      ///< every shader before the PS once
inline constexpr u32 kViMesh = 5;        ///< AS once; MS.. per view
inline constexpr u32 kViMeshRaster = 6;  ///< AS and MS once

/// What one PIPELINE_STATISTICS[1] or SO_STATISTICS_STREAMn query counted
/// between its BeginQuery and EndQuery: the CPU's share (counter order of
/// D3D12_QUERY_DATA_PIPELINE_STATISTICS1), the GPU records the draws added
/// to (graphics/shaders/pipeline_stats.hpp, STATS_RECORD_WORDS each), the
/// argument blocks of indirect dispatches, and the stream-output records
/// (graphics::SO_STATS_WORDS each). Read once the GPU work has completed.
struct StatisticsResult {
    u32 type{0};
    std::array<u64, 14> cpu{};
    std::vector<std::shared_ptr<graphics::BackendBuffer>> records;
    struct DispatchArguments {
        std::shared_ptr<graphics::BackendBuffer> arguments;
        u64 threads_per_group{0};
    };
    std::vector<DispatchArguments> dispatch_arguments;
    /// Mesh draws' records (graphics/shaders/mesh_stage.hpp kMeshStatWords).
    /// With rasterization disabled (no pixel shader, depth and stencil
    /// off) nothing reaches the clipper: `clipper` false.
    struct MeshRecord {
        std::shared_ptr<graphics::BackendBuffer> record;
        bool clipper{true};
    };
    std::vector<MeshRecord> mesh_records;
    std::vector<std::shared_ptr<graphics::BackendBuffer>> so_records;
    /// Bit c: counter c could not be counted (named once when it was set).
    u32 unmeasured{0};
};

struct QueryHeap final : Object {
    QueryHeap() : Object(Kind::QueryHeap) {}
    u32 type{0};
    u32 count{0};
    u32 node_mask{0};
    /// What the last EndQuery of each index left, replaced when a later
    /// execution ends it again (a resolve reads whatever is there then).
    struct Slot {
        std::shared_ptr<graphics::GpuOcclusionCounter> occlusion;
        std::shared_ptr<graphics::GpuTimestamp> timestamp;
        std::shared_ptr<const StatisticsResult> statistics;
    };
    std::vector<Slot> slots;
};

struct CommandSignature final : Object {
    CommandSignature() : Object(Kind::CommandSignature) {}
    u32 byte_stride{0};
    u32 node_mask{0};
    struct Argument {
        u32 type{0};
        u32 slot{0};
        u32 root_index{0};
        u32 dest_offset{0};
        u32 num_values{0};
    };
    std::vector<Argument> arguments;
    GuestAddr root_signature{0};
    /// The root signature's layout (null when no argument changes one).
    std::shared_ptr<const RootLayout> root;
};

// -----------------------------------------------------------------------------
// Command lists
// -----------------------------------------------------------------------------

struct VertexBufferView {
    u64 va{0};
    u32 size{0};
    u32 stride{0};
};

struct Viewport {
    float x{0}, y{0}, width{0}, height{0}, min_depth{0}, max_depth{0};
};

struct Rect {
    i32 left{0}, top{0}, right{0}, bottom{0};
};

/// D3D12_TEXTURE_COPY_LOCATION.
struct CopyLocation {
    GuestAddr resource{0};
    u64 serial{0};
    u32 type{0};
    u32 subresource{0};
    u64 offset{0};
    u32 format{0};
    u32 width{0}, height{0}, depth{0}, row_pitch{0};
};

struct Recording;

namespace cmd {
struct SetPipelineState { GuestAddr pso; u64 serial; };
struct SetRootSignature { bool compute; GuestAddr root; u64 serial; };
struct SetDescriptorHeaps { std::array<GuestAddr, 2> heaps; std::array<u64, 2> serials; };
struct SetRootTable { bool compute; u32 index; u64 handle; };
struct SetRootConstants { bool compute; u32 index; u32 offset; std::vector<u32> values; };
struct SetRootView { bool compute; u32 index; u32 type; u64 va; };
struct SetTopology { u32 topology; };
struct SetVertexBuffers { u32 start; std::vector<VertexBufferView> views; };
struct SetIndexBuffer { u64 va; u32 size; u32 format; };
/// SOSetTargets from StartSlot. A view with `bound` false unbinds that slot.
/// `data_va` / `filled_va` are guest GPU virtual addresses; the filled-size
/// value is a 32-bit byte count at `filled_va` (Learn, BufferFilledSize).
struct SetStreamOutput {
    u32 start{0};
    struct View {
        bool bound{false};
        u64 data_va{0};
        u64 size{0};
        u64 filled_va{0};
    };
    std::vector<View> views;
};
struct SetViewports { std::vector<Viewport> viewports; };
struct SetScissors { std::vector<Rect> rects; };
struct SetRenderTargets { u32 count; std::array<Descriptor, 8> rtvs; bool has_dsv; Descriptor dsv; };
struct SetBlendFactor { std::array<float, 4> factor; };
struct SetStencilRef { u32 front; u32 back; };
/// SetViewInstanceMask: bit i enables view instance i.
struct SetViewInstanceMask { u32 mask; };
struct SetDepthBounds { float min; float max; };
struct Draw { u32 vertex_count, instance_count, start_vertex, start_instance; };
struct DrawIndexed { u32 index_count, instance_count, start_index; i32 base_vertex; u32 start_instance; };
struct Dispatch { u32 x, y, z; };
/// ID3D12GraphicsCommandList6::DispatchMesh.
struct DispatchMesh { u32 x, y, z; };
struct CopyBufferRegion { GuestAddr dst; u64 dst_serial; u64 dst_offset; GuestAddr src; u64 src_serial; u64 src_offset; u64 bytes; };
struct CopyTextureRegion { CopyLocation dst; u32 x, y, z; CopyLocation src; bool has_box; u32 box[6]; };
struct CopyResource { GuestAddr dst; u64 dst_serial; GuestAddr src; u64 src_serial; };
/// One tile of CopyTiles. A buffer tile is @p bytes at @p tiled_offset.
/// A texture tile is the texel box, with @p row_pitch the full tile row
/// (Learn, CopyTiles: linear within each 64 KiB tile). @p buffer_offset is
/// into the linear buffer, resource-relative.
struct CopyTile {
    u32 index{0};
    u64 buffer_offset{0};
    u64 tiled_offset{0};
    u64 bytes{0};
    u32 level{0};
    u32 slice{0};
    u32 x{0};
    u32 y{0};
    u32 width{0};
    u32 height{0};
    u64 row_pitch{0};
    bool texture{false};
};
struct CopyTiles {
    GuestAddr tiled{0};
    u64 tiled_serial{0};
    GuestAddr buffer{0};
    u64 buffer_serial{0};
    bool to_tiled{false};
    std::vector<CopyTile> tiles;
};
struct ResolveSubresource { GuestAddr dst; u64 dst_serial; u32 dst_sub; GuestAddr src; u64 src_serial; u32 src_sub; u32 format; u32 dst_x{0}; u32 dst_y{0}; bool has_rect{false}; std::array<i32, 4> rect{}; u32 mode{0x3u}; };
struct ClearRenderTarget { Descriptor rtv; std::array<float, 4> color; std::vector<Rect> rects; };
struct ClearDepthStencil { Descriptor dsv; u32 flags; float depth; u8 stencil; std::vector<Rect> rects; };
struct ClearUnorderedAccess { Descriptor uav; bool is_float; std::array<u32, 4> values; std::vector<Rect> rects; };
struct Event { u32 kind; std::string label; };  ///< 0 marker, 1 begin, 2 end
struct ExecuteBundle { std::shared_ptr<const Recording> bundle; };
struct WriteImmediate { std::vector<std::pair<u64, u32>> writes; };
/// `attachments`: a transition out of RENDER_TARGET or DEPTH_WRITE
/// (GraphicsBackendDevice::attachment_barrier at replay).
struct Barrier {
    bool uav;
    bool attachments;
    /// Empty means unconditional (aliases, enhanced barriers, or unknown
    /// storage). Weak native-resource ownership avoids keeping dead guest
    /// allocations alive until a command list is reset. Replay falls back
    /// to an unconditional boundary if any referenced texture has expired.
    std::vector<std::weak_ptr<graphics::BackendTexture2D>> attachment_textures{};
};
struct ClearState { GuestAddr pso; u64 serial; };
struct Query { GuestAddr heap; u64 serial; u32 type; u32 index; bool begin; };
struct ResolveQuery { GuestAddr heap; u64 heap_serial; u32 type; u32 start; u32 count; GuestAddr dst; u64 dst_serial; u64 offset; };
struct SetPredication { GuestAddr buffer; u64 serial; u64 offset; u32 op; };
struct ExecuteIndirect { GuestAddr signature; u64 signature_serial; u32 max_count; GuestAddr args; u64 args_serial; u64 args_offset; GuestAddr count; u64 count_serial; u64 count_offset; };
/// One surface of a suspending or resuming render pass. Identity is the
/// view (resource, subresource, format, flags) plus the access types.
/// Clear values are compared only for a CLEAR beginning: Learn requires
/// the suspending and resuming passes to name identical views and access.
struct RenderPassSurface {
    GuestAddr resource{0};
    u64 serial{0};
    u32 mip{0};
    u32 slice{0};
    u32 array_size{0};
    u32 format{0};
    u32 flags{0};
    u32 beginning{0};
    u32 ending{0};
    u32 stencil_beginning{0};
    u32 stencil_ending{0};
    std::array<float, 4> clear{};
    u8 stencil_clear{0};
    bool depth_stencil{false};
};

[[nodiscard]] inline bool same_render_pass_surface(const RenderPassSurface& a,
                                                   const RenderPassSurface& b) {
    if (a.resource != b.resource || a.serial != b.serial || a.mip != b.mip ||
        a.slice != b.slice || a.array_size != b.array_size ||
        a.format != b.format || a.flags != b.flags ||
        a.beginning != b.beginning || a.ending != b.ending ||
        a.depth_stencil != b.depth_stencil ||
        a.stencil_beginning != b.stencil_beginning ||
        a.stencil_ending != b.stencil_ending) {
        return false;
    }
    if (!a.depth_stencil &&
        a.beginning == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR &&
        a.clear != b.clear) {
        return false;
    }
    if (a.depth_stencil &&
        a.beginning == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR &&
        a.clear[0] != b.clear[0]) {
        return false;
    }
    if (a.depth_stencil &&
        a.stencil_beginning == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR &&
        a.stencil_clear != b.stencil_clear) {
        return false;
    }
    return true;
}

[[nodiscard]] inline bool same_render_pass(
    const std::vector<RenderPassSurface>& a, bool a_uav,
    const std::vector<RenderPassSurface>& b, bool b_uav) {
    if (a_uav != b_uav || a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (!same_render_pass_surface(a[i], b[i])) return false;
    }
    return true;
}

/// End of a SUSPENDING_PASS. Replay copies each surface aside so a later
/// RESUMING_PASS in the same ExecuteCommandLists can put that local copy
/// back. A UAV write to one of these surfaces in between is overwritten by
/// that restore (Learn / DirectX-Specs: no GPU work belongs between the
/// pair; the local render-target copy is what the resume continues from).
struct RenderPassSuspend {
    std::vector<RenderPassSurface> surfaces;
    bool allow_uav{false};
};

/// BuildRaytracingAccelerationStructure: the VAs, the inputs as read at
/// the call, and the postbuild descs to emit after it ({DestBuffer,
/// InfoType}).
struct BuildAccelerationStructure {
    u64 dest{0};
    u64 source{0};
    u64 scratch{0};
    RtInputs inputs;
    std::vector<std::pair<u64, u32>> postbuild;
};
struct CopyAccelerationStructure { u64 dest; u64 source; u32 mode; };
struct EmitAccelerationStructurePostbuild { u64 dest; u32 type; std::vector<u64> sources; };
struct SetPipelineState1 { GuestAddr object; u64 serial; };
struct DispatchRays {
    u64 raygen{0}, raygen_size{0};
    u64 miss{0}, miss_size{0}, miss_stride{0};
    u64 hit{0}, hit_size{0}, hit_stride{0};
    u64 callable{0}, callable_size{0}, callable_stride{0};
    u32 width{0}, height{0}, depth{0};
};

/// Start of a RESUMING_PASS, recorded ahead of the beginning clears.
/// @p clear_count is how many following clear commands replay skips when
/// the suspended pass matches: the clear already happened on the first
/// segment, and the local copy is the contents the resume draws on.
struct RenderPassResume {
    std::vector<RenderPassSurface> surfaces;
    bool allow_uav{false};
    u32 clear_count{0};
};
}  // namespace cmd

using Command = std::variant<
    cmd::SetPipelineState, cmd::SetRootSignature, cmd::SetDescriptorHeaps,
    cmd::SetRootTable, cmd::SetRootConstants, cmd::SetRootView,
    cmd::SetTopology, cmd::SetVertexBuffers, cmd::SetIndexBuffer,
    cmd::SetStreamOutput,
    cmd::SetViewports, cmd::SetScissors, cmd::SetRenderTargets,
    cmd::SetBlendFactor, cmd::SetStencilRef, cmd::SetViewInstanceMask,
    cmd::SetDepthBounds, cmd::Draw,
    cmd::DrawIndexed, cmd::Dispatch, cmd::CopyBufferRegion,
    cmd::CopyTextureRegion, cmd::CopyResource, cmd::CopyTiles,
    cmd::ResolveSubresource,
    cmd::ClearRenderTarget, cmd::ClearDepthStencil,
    cmd::ClearUnorderedAccess, cmd::Event, cmd::ExecuteBundle,
    cmd::WriteImmediate, cmd::Barrier, cmd::ClearState, cmd::Query,
    cmd::ResolveQuery, cmd::SetPredication, cmd::ExecuteIndirect,
    cmd::RenderPassSuspend, cmd::RenderPassResume,
    cmd::DispatchMesh,
    cmd::BuildAccelerationStructure, cmd::CopyAccelerationStructure,
    cmd::EmitAccelerationStructurePostbuild, cmd::SetPipelineState1, cmd::DispatchRays>;

/// A closed command list's commands. Immutable once closed and shared with
/// every execution that captured it, so a list reset and re-recorded while an
/// earlier submission is still waiting on a fence replays what was submitted.
struct Recording {
    u32 type{0};
    std::vector<Command> commands;
    /// The pipeline state passed to CreateCommandList / Reset.
    GuestAddr initial_pso{0};
    u64 initial_pso_serial{0};
};

struct CommandAllocator final : Object {
    CommandAllocator() : Object(Kind::CommandAllocator) {}
    u32 type{0};
    /// Lists currently recording against this allocator (Learn: Reset fails
    /// while one is; an allocator backs one recording list at a time).
    /// Atomic: list Reset/Close run under the shared frontend lock.
    std::atomic<u32> recording_lists{0};
};

struct CommandList final : Object {
    CommandList() : Object(Kind::CommandList) {}
    GuestAddr allocator{0};
    u32 type{0};
    u32 node_mask{0};
    bool closed{false};
    /// The first error hit while recording (Learn, Close: "E_FAIL if ... an
    /// invalid API was called during command list recording"), reported by
    /// Close and, since such a list can never be reset, by Reset.
    u32 record_error{0};
    std::shared_ptr<Recording> recording;
    /// BeginRenderPass .. EndRenderPass (Tier 0: the pass is recorded as
    /// the clears, bindings and resolves it stands for). A suspending pass
    /// keeps its surfaces so a resuming pass on this list can be checked
    /// against them while recording; the local copy itself is taken when
    /// the list executes.
    bool in_render_pass{false};
    bool render_pass_suspending{false};
    bool render_pass_allow_uav{false};
    std::vector<cmd::RenderPassSurface> render_pass_surfaces;
    bool render_pass_suspend_open{false};
    bool render_pass_suspend_uav{false};
    std::vector<cmd::RenderPassSurface> render_pass_suspend_surfaces;
    std::vector<cmd::ResolveSubresource> render_pass_resolves;
    /// Query indices begun and not yet ended, by heap and index.
    std::vector<std::pair<GuestAddr, u32>> open_queries;
    /// The descriptor heaps SetDescriptorHeaps last bound while recording
    /// (CBV_SRV_UAV, SAMPLER), and whether it was called: a bundle that sets
    /// heaps must be executed where the same heaps are set (Learn, "Creating
    /// and recording command lists and bundles"; measured, a06 bundles).
    std::array<GuestAddr, 2> heaps{};
    bool heaps_set{false};
};

/// The value of a fence: one per CreateFence, shared by every ID3D12Fence
/// OpenSharedHandle makes of it (measured, a04s fence: a signal or a
/// lowering through either object is seen through the other) and kept alive
/// by its NT handles.
struct FenceTimeline {
    FenceTimeline() = default;
    FenceTimeline(const FenceTimeline&) = delete;
    FenceTimeline& operator=(const FenceTimeline&) = delete;
    /// Stops the GPU event bridge watching the timeline (d3d12_queue.cpp).
    ~FenceTimeline();
    /// Shared so a blocking wait that drops the frontend lock keeps the
    /// timeline alive if the fence is released meanwhile.
    std::shared_ptr<graphics::BackendFence> backend;
    /// The highest value a signal has been SUBMITTED for, from a queue or the
    /// CPU. A queue wait on a value at or below it can be recorded as a GPU
    /// wait: the signal is ahead of it in the one backend queue.
    u64 submitted{0};
    /// Timelines replaced by a lowering Signal, kept while GPU work recorded
    /// against them may still run.
    std::vector<std::shared_ptr<graphics::BackendFence>> retired;
    /// Bumped by every lowering, read without the frontend lock by a thread
    /// blocked in SetEventOnCompletion(value, NULL) on the old timeline.
    std::atomic<u64> generation{0};
};

struct Fence final : Object {
    Fence() : Object(Kind::Fence) {}
    u32 flags{0};
    std::shared_ptr<FenceTimeline> timeline{std::make_shared<FenceTimeline>()};
};

struct CommandQueue final : Object {
    CommandQueue() : Object(Kind::CommandQueue) {}
    u32 type{0};
    i32 priority{0};
    u32 flags{0};
    u32 node_mask{0};
    /// Work submitted while this queue waits on a fence value no signal has
    /// been submitted for yet. Recording such a wait into the single backend
    /// queue would deadlock it (the signal would be queued behind the wait),
    /// so the queue holds its later work here until the signal is submitted.
    struct Pending {
        enum class Op : u8 { Execute, Signal, Wait, Present, TileMappings };
        Op op{Op::Execute};
        std::vector<std::shared_ptr<const Recording>> recordings;
        /// TileMappings: run after the ops queued ahead of it, so a mapping
        /// submitted while this queue waits on a fence stays behind that
        /// wait in the one backend stream.
        std::function<void()> tile_mappings;
        GuestAddr fence{0};
        u64 fence_serial{0};
        u64 value{0};
        /// Present: the chain, the buffer shown (resource address +
        /// serial), SyncInterval and flags.
        GuestAddr chain{0};
        u64 chain_serial{0};
        GuestAddr buffer{0};
        u64 buffer_serial{0};
        u32 sync_interval{0};
        u32 present_flags{0};
    };
    std::deque<Pending> pending;
};

/// IDXGISwapChain4 on an ID3D12CommandQueue (d3d12_swapchain.cpp): flip
/// model only, its buffers ID3D12Resource objects, Present ordered on the
/// queue.
struct SwapChain final : Object {
    SwapChain() : Object(Kind::SwapChain) {}
    GuestAddr queue{0};
    GuestAddr factory{0};
    GuestAddr restrict_output{0};
    u64 hwnd{0};
    bool composition{false};
    /// HWND of the DirectComposition target whose committed visual has this
    /// chain as content. Zero until that Commit. Not OutputWindow: GetHwnd
    /// stays invalid and GetDesc keeps a null window (Learn,
    /// CreateSwapChainForComposition).
    GuestAddr composition_hwnd{0};
    std::shared_ptr<graphics::BackendCompositionLayerDesc> composition_layer;
    /// The buffer Present made available before a visual was bound. Commit
    /// shows this one; a later Present replaces it. D3D12 names buffers by
    /// index, so the retained frame is this resource, not "buffer 0".
    struct DeferredPresent {
        GuestAddr buffer{0};
        u64 serial{0};
        u32 sync_interval{0};
        u32 flags{0};
    };
    std::optional<DeferredPresent> deferred_present;
    /// show sets this when a bound composition present cannot be shown.
    /// present turns it into DXGI_ERROR_INVALID_CALL and does not advance.
    bool composition_present_failed{false};
    /// DXGI_SWAP_CHAIN_DESC1.
    u32 width{0}, height{0}, format{0}, stereo{0}, buffer_usage{0};
    u32 buffer_count{0}, scaling{0}, swap_effect{0}, alpha_mode{0}, flags{0};
    /// DXGI_SWAP_CHAIN_FULLSCREEN_DESC.
    u32 refresh_numerator{0}, refresh_denominator{0}, scanline_ordering{0};
    u32 fullscreen_scaling{0};
    /// Exclusive full-screen, the state machine shared with the D3D11
    /// chains (winapi/graphics/dxgi/dxgi_exclusive_fullscreen.hpp).
    dxgi::ExclusiveState exclusive;
    /// The buffers (ID3D12Resource), each holding the chain's reference.
    std::vector<GuestAddr> buffers;
    u32 current{0};
    u32 present_count{0};
    u64 last_present_qpc{0};
    std::shared_ptr<graphics::BackendSwapChain> backend;
    /// One GPU point per present whose frame the GPU has not finished: the
    /// DXGI present queue DO_NOT_WAIT tests.
    std::deque<std::shared_ptr<graphics::GpuTimestamp>> frames_in_flight;
    std::array<float, 4> background{0.0f, 0.0f, 0.0f, 1.0f};
    u32 rotation{1};  // DXGI_MODE_ROTATION_IDENTITY
    u32 source_width{0}, source_height{0};
    u32 max_frame_latency{1};
    /// FRAME_LATENCY_WAITABLE_OBJECT: the semaphore, shared with the GPU
    /// completion callbacks that release it (they may outlive the chain).
    struct Latency {
        std::mutex mutex;
        u64 handle{0};
        u32 cap{1};
        bool alive{true};
    };
    std::shared_ptr<Latency> latency;
    u32 color_space{0};
    std::array<float, 6> matrix{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    u32 hdr_metadata_type{0};
    std::vector<u8> hdr_metadata;
};

// =============================================================================
// The frontend
// =============================================================================

class Frontend {
public:
    Frontend(GuestMemory& memory, bool is_64bit,
             graphics::GraphicsBackendDevice& backend);
    ~Frontend();

    void set_method_thunks(const D3D12Manager::MethodThunks& thunks);
    void set_guest_function_caller(GuestFunctionCaller caller);

    // Exports (the root-signature ones are d3d12_root_signature.cpp's).
    void D3D12CreateDevice(Marshaler& m);
    void D3D12GetDebugInterface(Marshaler& m);
    void D3D12EnableExperimentalFeatures(Marshaler& m);
    void D3D12GetInterface(Marshaler& m);

    /// Every vtable slot of every interface arrives here.
    void method(Marshaler& m, usize table, usize slot);

    [[nodiscard]] bool is_device(GuestAddr address);
    [[nodiscard]] bool is_command_queue(GuestAddr address);

    // -- object model (d3d12_objects.cpp) -----------------------------------
    template <typename T>
    [[nodiscard]] T* find(GuestAddr address, Kind kind) {
        const auto entry = m_objects.find(address);
        if (entry == m_objects.end() || entry->second->kind != kind) {
            return nullptr;
        }
        return static_cast<T*>(entry->second.get());
    }
    /// @p address only if it still names the object recorded with @p serial.
    template <typename T>
    [[nodiscard]] T* find_serial(GuestAddr address, u64 serial, Kind kind) {
        T* object = find<T>(address, kind);
        return object != nullptr && object->serial == serial ? object : nullptr;
    }
    /// Register @p object under a fresh guest interface, or 0 when the pool is
    /// exhausted (the object is then destroyed).
    [[nodiscard]] GuestAddr publish(std::unique_ptr<Object> object);
    void add_ref(Object& object) { ++object.ref_count; }
    /// Returns the new count; destroys the object at zero.
    u32 release(GuestAddr address);
    /// A child's reference on its device (see create paths).
    void add_device_ref(GuestAddr device);

    [[nodiscard]] bool write_pointer(GuestAddr address, GuestAddr value);
    [[nodiscard]] bool read_pointer(GuestAddr address, GuestAddr& value) const;
    [[nodiscard]] bool read_guid(GuestAddr address, GuidBytes& out) const;
    [[nodiscard]] u32 read_u32(GuestAddr address) const;
    [[nodiscard]] u64 read_u64(GuestAddr address) const;
    [[nodiscard]] float read_f32(GuestAddr address) const;

    /// Hand @p object to the guest through (riid, ppv) the way every Create*
    /// does: E_NOINTERFACE (object destroyed) when riid is not one it
    /// answers, else the pointer with the creation reference.
    [[nodiscard]] u32 return_new_object(std::unique_ptr<Object> object,
                                        GuestAddr iid_address,
                                        GuestAddr out_address);
    [[nodiscard]] bool answers(const Object& object, const GuidBytes& iid);

    GuestMemory& m_memory;
    const bool m_is_64bit;
    graphics::GraphicsBackendDevice& m_backend;
    /// The frontend lock: every object, map and pool below; the lock map is
    /// d3d12_lock.hpp's. Taken through FrontendLock only (recursive per
    /// thread: a guest callback made under the lock may call back in).
    FrontendRwLock m_lock;
    /// Every replay and the queue state it reads and writes: held for the
    /// whole of an ExecuteCommandLists, which runs under the shared lock
    /// (exclusive holders need not take it: they exclude every replay).
    std::mutex m_replay_mutex;
    /// Device removal, CPU/queue fence timeline replacement, and the
    /// read-only GetCompletedValue snapshot (all may run under shared ownership).
    std::mutex m_remove_mutex;
    /// Present and tile-mapping operations queued behind a fence wait on
    /// any queue: while one is, queue Signal / Wait (which may run it) take
    /// the exclusive lock (Frontend::method).
    std::atomic<u32> m_exclusive_queue_ops{0};
    /// Times the calling thread holds the frontend lock (FrontendUnlock).
    [[nodiscard]] u32 lock_depth() const { return m_lock.depth(); }
    /// Pipeline builds running now, and the most that have run at once
    /// (build_pipeline_unlocked): what the narrowed lock buys, for tests and
    /// diagnostics.
    std::atomic<u32> m_builds_in_flight{0};
    std::atomic<u32> m_builds_in_flight_peak{0};
    GuestFunctionCaller m_guest_call;

    std::array<std::vector<GuestAddr>, INTERFACE_TABLE_COUNT> m_thunks{};
    std::array<GuestAddr, INTERFACE_TABLE_COUNT> m_vtables{};
    std::vector<GuestAddr> m_object_chunks;
    GuestAddr m_next_object{0};
    GuestAddr m_object_limit{0};
    std::vector<GuestAddr> m_free_objects;
    bool m_vtables_ready{false};
    u64 m_next_serial{1};
    std::unordered_map<GuestAddr, std::unique_ptr<Object>> m_objects;
    /// Live queue identities only. Fence propagation must not walk every
    /// resource/allocator/PSO; serials distinguish recycled guest addresses.
    std::vector<std::pair<GuestAddr, u64>> m_command_queues;
    /// Objects released so far: with m_next_serial, a generation that moves
    /// whenever m_objects gains or loses an object.
    u64 m_objects_released{0};
    /// buffer_words_from_va's index: the GPU VA span [start, end) of every
    /// buffer resource, per Memory, valid while m_buffer_spans_generation is
    /// the object generation. A root CBV / SRV / UAV names only a GPU VA, so
    /// its size is the buffer around that VA; without the index each lookup
    /// walked every live object (every draw, every root view).
    mutable std::unordered_map<const Memory*, std::vector<std::pair<u64, u64>>>
        m_buffer_spans;
    mutable u64 m_buffer_spans_generation{~u64{0}};
    // -- swap chains (d3d12_swapchain.cpp) ------------------------------------
    D3D12Manager::PresentationServices m_presentation{};
    D3D12Manager::SectionServices m_sections{};
    D3D12Manager::ShaderCacheDirectories m_shader_cache_dirs{};
    enum class SwapChainApi : u8 { Legacy, Hwnd, Composition };
    void create_swap_chain(Marshaler& m, SwapChainApi api);
    void swap_chain_method(Marshaler& m, SwapChain& chain, usize slot);
    /// Validation of a DXGI_SWAP_CHAIN_DESC1 for a D3D12 chain (flip model).
    [[nodiscard]] u32 validate_swap_chain(const SwapChain& chain,
                                          SwapChainApi api) const;
    /// (Re)create the chain's buffers at its current size / format / count.
    /// SetFullscreenState(TRUE, pTarget) and the Windowed=FALSE creation:
    /// may release the frontend lock for the transition, so the caller
    /// re-finds the chain afterwards.
    [[nodiscard]] u32 enter_fullscreen(GuestAddr chain, GuestAddr target);
    /// Leave exclusive full-screen; @p leave_host_space also leaves the
    /// macOS Space. May release the frontend lock like enter_fullscreen.
    void leave_fullscreen(GuestAddr chain, bool leave_host_space);
    /// ResizeTarget on a full-screen chain.
    [[nodiscard]] u32 resize_fullscreen_target(GuestAddr chain, u32 width,
                                               u32 height);
    /// Runs @p transition with the frontend and DXGI API locks released.
    void run_unlocked(const std::function<void()>& transition);
    [[nodiscard]] u32 create_swap_chain_buffers(SwapChain& chain);
    void release_swap_chain_buffers(SwapChain& chain);
    [[nodiscard]] u32 present(SwapChain& chain, u32 sync_interval, u32 flags,
                              GuestAddr dirty_rects, u32 dirty_count);
    /// Show @p buffer of @p chain now (in queue order).
    void show(SwapChain& chain, GuestAddr buffer, u64 buffer_serial,
              u32 sync_interval, u32 flags, bool immediate_present = false);
    /// IDCompositionDevice::Commit bound @p address into the visual tree of
    /// @p hwnd. False when it is not a composition chain or the presenter
    /// cannot take the layer (Commit then returns E_NOTIMPL, as D3D11).
    [[nodiscard]] bool bind_composition_swap_chain(
        GuestAddr address, GuestAddr hwnd,
        const graphics::BackendCompositionLayerDesc& layer);
    [[nodiscard]] bool ensure_composition_backend(SwapChain& chain);
    [[nodiscard]] bool apply_composition_layer(SwapChain& chain);
    [[nodiscard]] bool present_swap_chain_texture(
        SwapChain& chain, graphics::BackendTexture2D* texture,
        u32 sync_interval, u32 flags);
    /// ID3DBlob creation (D3D12Manager::create_blob), for GetCachedBlob.
    std::function<GuestAddr(const u8*, usize)> m_create_blob;
    /// Tear-off interface pointers (ID3DDestructionNotifier) -> owner.
    std::unordered_map<GuestAddr, GuestAddr> m_tearoffs;
    /// ID3DDestructionNotifier slots on the tear-off of @p owner.
    void notifier_method(Marshaler& m, Object& owner, usize slot);
    /// QueryInterface's answer for @p iid on @p object: the object's own
    /// pointer, its tear-off, or 0 (E_NOINTERFACE). Takes the reference.
    [[nodiscard]] GuestAddr query_interface(Object& object,
                                            const GuidBytes& iid);
    /// The tear-off table @p object answers @p iid through, or
    /// INTERFACE_TABLE_COUNT when it is not a tear-off interface.
    [[nodiscard]] static usize tearoff_table(const Object& object,
                                             const GuidBytes& iid);
    /// @p object's tear-off pointer for @p table, made on first use (0 when
    /// the pool is exhausted). No reference is taken.
    [[nodiscard]] GuestAddr tearoff(Object& object, usize table);
    /// A slot of a tear-off (other than ID3DDestructionNotifier) of @p owner.
    void tearoff_method(Marshaler& m, Object& owner, usize table, usize slot);

    // -- configuration and tools (d3d12_config.cpp) -------------------------
    /// D3D12CreateDevice and ID3D12DeviceFactory::CreateDevice: @p factory
    /// is null for the export (which serves the per-adapter singleton).
    [[nodiscard]] u32 create_device(GuestAddr adapter, u32 level,
                                    GuestAddr iid_address, GuestAddr out,
                                    DeviceFactory* factory);
    /// D3D12_DEVICE_FLAGS a device created under @p dred reports.
    [[nodiscard]] static u32 device_configuration_flags(const DredConfig& dred);
    /// Slots of the objects D3D12GetInterface hands out.
    void sdk_configuration_method(Marshaler& m, SdkConfiguration& config,
                                  usize slot);
    void device_factory_method(Marshaler& m, DeviceFactory& factory,
                               usize slot);
    void dred_settings_method(Marshaler& m, DredSettings& settings,
                              usize slot);
    /// D3D12GetInterface / GetConfigurationInterface for @p clsid: a new
    /// object for (riid, ppv), or the documented refusal. @p factory is the
    /// device factory asking (null: the export).
    [[nodiscard]] u32 get_configuration_interface(const GuidBytes& clsid,
                                                  GuestAddr iid_address,
                                                  GuestAddr out,
                                                  DeviceFactory* factory);
    [[nodiscard]] DredConfig& dred_config_of(const DredSettings& settings);
    // -- pipeline libraries and shader caches (d3d12_library.cpp) ------------
    void create_pipeline_library(Marshaler& m, Device& device);
    void pipeline_library_method(Marshaler& m, PipelineLibrary& library,
                                 usize slot);
    void pipeline_library_load(Marshaler& m, PipelineLibrary& library,
                               int kind);
    [[nodiscard]] u64 library_adapter_key() const;
    [[nodiscard]] std::vector<u8> serialize_library(
        const PipelineLibrary& library) const;
    [[nodiscard]] u32 deserialize_library(const std::vector<u8>& blob,
                                          PipelineLibrary& library) const;
    /// A NUL-terminated guest UTF-16 string; false when NULL or unreadable.
    [[nodiscard]] bool read_wide_name(GuestAddr address,
                                      std::u16string& out) const;
    void create_shader_cache_session(Marshaler& m, Device& device);
    void shader_cache_session_method(Marshaler& m, ShaderCacheSession& session,
                                     usize slot);
    void release_shader_cache_session(ShaderCacheSession& session);
    /// D3D12_SHADER_CACHE_MODE_DISK: opens @p store's file (d3d12_library.cpp).
    [[nodiscard]] u32 open_disk_shader_cache(const ShaderCacheSession& session,
                                             ShaderCacheStore& store);
    /// DRIVER_VERSIONED caches' file-name tag: this device and macOS build.
    [[nodiscard]] std::string shader_cache_driver_tag() const;
    [[nodiscard]] u32 shader_cache_control(u32 kinds, u32 control);
    [[nodiscard]] u32 set_background_processing_mode(u32 mode, u32 action,
                                                     GuestAddr event,
                                                     GuestAddr further);
    std::map<GuidBytes, std::shared_ptr<ShaderCacheStore>> m_shader_caches;

    // -- lifetime trackers (d3d12_state_object.cpp) --------------------------
    void create_lifetime_tracker(Marshaler& m, Device& device);
    void lifetime_tracker_method(Marshaler& m, LifetimeTracker& tracker,
                                 usize slot);
    // Device tear-offs.
    void dred_method(Marshaler& m, Device& device, usize slot);
    void device_tools_method(Marshaler& m, Device& device, usize slot);
    void device_configuration_method(Marshaler& m, Object& owner, usize slot);
    void compatibility_device_method(Marshaler& m, Device& device, usize slot);
    void pageable_tools_method(Marshaler& m, Object& owner, usize slot);
    /// The process's global DRED state (D3D12GetInterface's settings).
    DredConfig m_dred{};
    /// ID3D12DeviceConfiguration::SerializeVersionedRootSignature and
    /// CreateVersionedRootSignatureDeserializer: the D3D12Manager exports,
    /// reading their parameters after `this`.
    std::function<void(Marshaler&)> m_serialize_versioned_root_signature;
    std::function<void(Marshaler&)> m_create_versioned_deserializer;
    GuestAddr m_device{0};
    /// Set by the destructor: guest memory may no longer exist.
    bool m_tearing_down{false};

    [[nodiscard]] bool ensure_vtables();
    [[nodiscard]] GuestAddr allocate_interface(usize table);
    void free_interface(GuestAddr address);
    void destroy(std::unique_ptr<Object> object);

    // IUnknown / ID3D12Object / ID3D12DeviceChild, slots 0-7.
    [[nodiscard]] bool common_method(Marshaler& m, usize slot, Object& object);
    [[nodiscard]] u32 get_private_data(Marshaler& m, Object& object);
    [[nodiscard]] u32 set_private_data(Marshaler& m, Object& object);
    [[nodiscard]] u32 set_private_data_interface(Marshaler& m, Object& object);
    [[nodiscard]] u32 set_name(Marshaler& m, Object& object);
    void drop_private_interfaces(Object& object);

    // -- device (d3d12_device.cpp) -----------------------------------------
    void device_method(Marshaler& m, Device& device, usize slot);
    /// What this device implements (d3d12_caps.hpp): only the tiers this
    /// frontend serves are switched on.
    [[nodiscard]] const CapsContext& caps();
    /// The resource-layout rules for this device, with the backend's
    /// placement-heap texture requirement.
    [[nodiscard]] const LayoutCaps& layout();
    /// Bytes and alignment the backend needs to place a texture of @p desc
    /// in a heap. False when it cannot be placed.
    [[nodiscard]] bool texture_placement(const ResourceDescription& desc,
                                         u64& size, u64& alignment);
    std::unique_ptr<CapsContext> m_caps;
    std::unique_ptr<LayoutCaps> m_layout;

    // -- resources and heaps (d3d12_resource.cpp) --------------------------
    [[nodiscard]] bool read_heap_properties(GuestAddr address,
                                            HeapProperties& out) const;
    void write_heap_properties(GuestAddr address, const HeapProperties& p);
    [[nodiscard]] bool read_resource_desc(GuestAddr address, bool desc1,
                                          ResourceDescription& out) const;
    void write_resource_desc(GuestAddr address, const ResourceDescription& desc,
                             bool desc1);
    [[nodiscard]] HeapProperties resolve_custom(const HeapProperties& p) const;
    [[nodiscard]] static constexpr bool gpu_upload_heaps() {
        return GPU_UPLOAD_HEAP_SUPPORTED;
    }
    [[nodiscard]] bool cpu_visible(const HeapProperties& p) const;
    /// A heap of @p size bytes (zero-filled, as Windows' are), with a GPU VA
    /// range and, when CPU-visible, its persistent guest mapping.
    [[nodiscard]] std::shared_ptr<Memory> allocate_memory(u64 size,
                                                          bool cpu_visible,
                                                          bool buffers_only,
                                                          bool write_watch = false);
    /// kernel32's virtual memory: write-watched heap mappings register there
    /// so GetWriteWatch sees them.
    VirtualMemoryManager* m_vmm{nullptr};
    /// ID3D12Device3::OpenExistingHeapFromAddress(1) (d3d12_resource.cpp).
    void open_existing_heap(Marshaler& m, Device& device, bool sized);
    /// The heap over [@p address, +@p size) of guest memory, committed
    /// read-write: CUSTOM / WRITE_BACK / L0, SHARED | SHARED_CROSS_ADAPTER |
    /// ALLOW_SHADER_ATOMICS. @p release runs once the GPU is done with it
    /// (or at once on failure).
    void open_existing_heap_over(Marshaler& m, Device& device, const char* api,
                                 GuestAddr address, u64 size, usize riid_index,
                                 GuestAddr out, std::function<void()> release);
    /// ID3D12Device3::OpenExistingHeapFromFileMapping (d3d12_resource.cpp).
    void open_existing_heap_from_mapping(Marshaler& m, Device& device);
    void retire_memory(Memory* memory);
    /// The same bytes as @p source under a new GPU VA (an opened shared heap
    /// or resource).
    [[nodiscard]] std::shared_ptr<Memory> alias_memory(
        const std::shared_ptr<Memory>& source);

    // -- shared NT handles (d3d12_sharing.cpp) ------------------------------
    /// What one shared heap, committed resource or fence carries to the
    /// objects OpenSharedHandle makes of it.
    struct SharedPayload;
    /// The process handle table (kernel32's), where the NT handles live.
    HandleTable* m_handle_table{nullptr};
    /// Live payloads by their nonce (SharedResourceObject::nonce).
    std::unordered_map<u64, std::weak_ptr<SharedPayload>> m_shared;
    u64 m_next_shared_nonce{1};
    void create_shared_handle(Marshaler& m);
    void open_shared_handle(Marshaler& m, Device& device);
    void open_shared_handle_by_name(Marshaler& m);
    /// Frontend teardown: payloads NT handles still hold let go of the
    /// frontend's memory.
    void detach_shared_payloads();
    /// VA -> (buffer resource memory, offset in it). False when unmapped.
    [[nodiscard]] bool resolve_va(u64 va, u64 bytes, graphics::BackendBuffer*& buffer,
                                  u64& offset);
    /// Words from @p va to the end of the buffer resource that contains it
    /// (the heap's end when no resource does). 0 when @p va is unmapped.
    /// A root SRV/UAV has no NumElements; the resource is the view.
    [[nodiscard]] u32 buffer_words_from_va(u64 va) const;
    /// @p variant 0 CreateHeap, 1 CreateHeap1, 2 ID3D12CompatibilityDevice::
    /// CreateSharedHeap.
    void create_heap(Marshaler& m, Device& device, int variant);
    /// @p variant 0..3 CreateCommittedResource..3, 4 ID3D12CompatibilityDevice::
    /// CreateSharedResource.
    void create_committed_resource(Marshaler& m, Device& device,
                                   int variant);
    void create_placed_resource(Marshaler& m, Device& device, int variant);
    void create_reserved_resource(Marshaler& m, Device& device, int variant);
    /// The backend tile grid of a reserved resource. False (and an
    /// UNIMPLEMENTED line) when the backend cannot describe it.
    [[nodiscard]] bool build_reserved_tiles(Resource& resource, const char* api);
    /// A 64 KiB-aligned GPU VA for a reserved buffer, registered for
    /// resolve_va. The buffer object must already exist.
    void assign_reserved_va(Resource& resource);
    [[nodiscard]] const ReservedVa* reserved_span(u64 va) const;
    /// The heap's tile pool, created on first mapping and sized to the
    /// heap. Null when the backend has no tile pools.
    [[nodiscard]] std::shared_ptr<graphics::BackendTilePool> heap_tile_pool(
        Heap& heap);
    /// A sampler feedback map's storage: @p memory at @p offset (placed), or
    /// new memory (committed, @p memory null); header written, data zero.
    void create_feedback_view(Marshaler& m);
    /// ResolveSubresourceRegion ENCODE / DECODE_SAMPLER_FEEDBACK: validates
    /// and records (d3d12_replay_feedback.cpp).
    void record_feedback_transcode(CommandList& list, Recording& rec, Marshaler& m,
                                   Resource& d, u64 dst_serial, Resource& s,
                                   u64 src_serial, u32 mode);
    [[nodiscard]] u32 create_feedback_map(Resource& resource,
                                          std::shared_ptr<Memory> memory,
                                          u64 offset);
    [[nodiscard]] u32 create_resource_backend(Resource& resource,
                                              const graphics::BackendHeap* heap,
                                              u64 heap_offset,
                                              bool cpu_visible);
    /// The CreateCommittedResource / CreatePlacedResource rules for @p desc
    /// in a heap of @p props (measured, a02 buffers / textures / placed):
    /// E_INVALIDARG with the reason logged, or S_OK. Normalises @p desc the
    /// way GetDesc reports it. @p clear_value is the guest D3D12_CLEAR_VALUE
    /// (0 for none); initial states are checked for committed resources only.
    [[nodiscard]] u32 validate_resource_desc(ResourceDescription& desc,
                                             const HeapProperties& props,
                                             u32 initial_state,
                                             GuestAddr clear_value,
                                             bool committed, const char* api);
    /// The enhanced-barrier creators' extra parameters (Device10
    /// CreateCommittedResource3 / CreatePlacedResource2 /
    /// CreateReservedResource2): the InitialLayout and the castable-format
    /// list (@p count entries at @p formats; a NULL array is no list,
    /// measured). E_INVALIDARG with the reason logged, or S_OK.
    [[nodiscard]] u32 layout_creation_error(const ResourceDescription& desc,
                                            u32 initial_layout, u32 count,
                                            GuestAddr formats, const char* api);
    /// D3D12_HEAP_FLAGS rules of CreateHeap (@p heap_object) and of a
    /// committed resource's implicit heap.
    [[nodiscard]] u32 heap_flags_error(u32 flags, const HeapProperties& props,
                                       bool heap_object, const char* api);
    void heap_method(Marshaler& m, Heap& heap, usize slot);
    void resource_method(Marshaler& m, Resource& resource, usize slot);
    /// ID3D12Resource::WriteToSubresource / ReadFromSubresource.
    void cpu_subresource_copy(Marshaler& m, Resource& resource, bool write);
    std::map<u64, Memory*> m_va_ranges;
    std::map<u64, ReservedVa*> m_reserved_vas;
    /// ResolveQueryData's ordering fence (d3d12_replay.cpp resolve_query):
    /// the CPU writes the results after the GPU has run the queries and
    /// signals it; the copy into the destination waits for it on the GPU.
    std::shared_ptr<graphics::BackendFence> m_resolve_fence;
    u64 m_resolve_value{0};
    [[nodiscard]] graphics::BackendFence* resolve_fence() {
        if (!m_resolve_fence) m_resolve_fence = m_backend.create_fence(0);
        return m_resolve_fence.get();
    }
    u64 m_next_va{0x0000010000000000ull};
    GuestAddr m_guest_map_hint{0x20000000ull};

    // -- descriptors (d3d12_descriptor.cpp) ---------------------------------
    [[nodiscard]] u32 descriptor_increment() const { return 32u; }
    void create_descriptor_heap(Marshaler& m, Device& device);
    void descriptor_heap_method(Marshaler& m, DescriptorHeap& heap, usize slot);
    /// The record a CPU descriptor handle names, or null.
    [[nodiscard]] Descriptor* cpu_descriptor(GuestAddr handle,
                                             DescriptorHeap** heap = nullptr);
    /// The heap and first index a GPU descriptor handle names, or null.
    [[nodiscard]] DescriptorHeap* gpu_descriptor(u64 handle, u32& index);
    void create_view(Marshaler& m, DescriptorType type);
    void create_sampler(Marshaler& m, bool desc2);
    void copy_descriptors(Marshaler& m);
    void copy_descriptors_simple(Marshaler& m);
    [[nodiscard]] bool read_sampler(GuestAddr address, bool desc2,
                                    SamplerState& out) const;
    std::map<GuestAddr, DescriptorHeap*> m_cpu_heaps;
    std::map<u64, DescriptorHeap*> m_gpu_heaps;
    u64 m_next_gpu_handle{0x0000200000000000ull};

    // -- pipelines (d3d12_pipeline.cpp) ------------------------------------
    void create_root_signature(Marshaler& m, Device& device);
    /// The three PSO creators. @p first is the index of the desc parameter
    /// (riid and ppv follow it). With @p capture, the built pipeline is moved
    /// there instead of being handed out (ID3D12PipelineLibrary's Load*).
    void create_graphics_pipeline_state(
        Marshaler& m, Device& device, usize first = 1,
        std::unique_ptr<PipelineState>* capture = nullptr);
    void create_compute_pipeline_state(
        Marshaler& m, Device& device, usize first = 1,
        std::unique_ptr<PipelineState>* capture = nullptr);
    void create_pipeline_state(
        Marshaler& m, Device& device, usize first = 1,
        std::unique_ptr<PipelineState>* capture = nullptr);
    /// The guest addresses of a pipeline's parts, from a desc struct or a
    /// pipeline state stream (0: absent).
    struct PipelineSource {
        GuestAddr root{0};
        GuestAddr vs{0}, ps{0}, ds{0}, hs{0}, gs{0}, cs{0}, as{0}, ms{0};
        GuestAddr so_declaration{0};
        u32 stream_output_entries{0};
        GuestAddr so_strides{0};
        u32 so_num_strides{0};
        u32 so_rasterized_stream{0};
        GuestAddr cached_blob{0};
        GuestAddr cached_size{0};
        /// D3D12_VIEW_INSTANCING_DESC.
        bool view_instancing_present{false};
        u32 view_instances{0};
        GuestAddr view_locations{0};
        u32 view_flags{0};
        /// `root` resolved under the frontend lock (resolve_pipeline_root),
        /// so build_pipeline reads no frontend object: the layout, and the
        /// blob hash pipeline_digest mixes in when it names a live root
        /// signature.
        bool root_resolved{false};
        bool root_live{false};
        std::shared_ptr<const RootLayout> root_layout;
        u64 root_blob_hash{0};
    };
    /// One pipeline creation's refusal report (d3d12_pipeline_refusal.cpp).
    /// Open for the whole Create*PipelineState call: when the call returns a
    /// failure HRESULT, its recorded reasons (every d3d12_diag / refuse /
    /// unimplemented line of the call) are logged in one
    /// `<api>: refused 0x... : <reasons>` VITREO_D3D12_DIAG line, and with
    /// VITREO_D3D12_DUMP_REFUSED the description, the stream, the root
    /// signature blob and every shader blob are written beside the reason.
    class PipelineRefusalScope {
    public:
        PipelineRefusalScope(Frontend& frontend, Marshaler& m, const char* api);
        ~PipelineRefusalScope();
        PipelineRefusalScope(const PipelineRefusalScope&) = delete;
        PipelineRefusalScope& operator=(const PipelineRefusalScope&) = delete;
        /// The D3D12_*_PIPELINE_STATE_DESC / D3D12_PIPELINE_STATE_STREAM_DESC.
        GuestAddr desc{0};
        u64 desc_size{0};
        /// A pipeline state stream's subobject bytes.
        GuestAddr stream{0};
        u64 stream_size{0};
        /// The shaders and root signature, once the description is read.
        const PipelineSource* src{nullptr};
        /// D3D12_INPUT_LAYOUT_DESC, once read (VITREO_D3D12_DUMP_PSOS).
        GuestAddr input_elements{0};
        u32 input_element_count{0};

    private:
        /// One guest range a dump records (encode_regions).
        static constexpr u32 kRegionData = 0;
        static constexpr u32 kRegionPointers = 1;  // holds guest pointers
        struct Region {
            GuestAddr at;
            u32 kind;
            std::vector<u8> bytes;
        };
        void dump(u32 hr, const std::string& reasons, const char* dir);
        void collect_regions(const std::vector<u8>& desc_bytes,
                             const std::vector<u8>& stream_bytes,
                             std::vector<Region>& regions) const;
        [[nodiscard]] std::vector<u8> encode_regions(
            const std::vector<Region>& regions) const;
        Frontend& m_frontend;
        Marshaler& m_m;
        const char* m_api;
        /// The enclosing scope's reasons (a library Load* runs a creator).
        std::string m_saved;
    };
    /// Build the render pipeline @p pso's plain draws use, with the strides
    /// taken from each draw (PipelineState::dynamic_variant); where the
    /// backend cannot, compile the shader functions its first plain draw
    /// will form one from (GraphicsBackendDevice::
    /// precompile_pipeline_functions). Outside the lock.
    void precompile_draw_functions(PipelineState& pso);
    /// Look up @p src.root (under the lock) for build_pipeline.
    void resolve_pipeline_root(PipelineSource& src);
    /// Phases (b) and (c) of a pipeline creation: build @p pso outside the
    /// frontend lock when this thread holds it once (else under it), then
    /// relock. E_INVALIDARG when the device it was created on died meanwhile.
    [[nodiscard]] u32 build_pipeline_unlocked(PipelineSource& src,
                                              PipelineState& pso,
                                              const char* api);
    /// Read the view instancing description into @p pso.
    [[nodiscard]] u32 read_view_instancing(const PipelineSource& src,
                                           PipelineState& pso,
                                           const char* api);
    /// Build views 1.. of a view-instanced @p pso (PipelineState::views).
    [[nodiscard]] u32 build_view_pipelines(const PipelineSource& src,
                                           PipelineState& pso,
                                           const char* api);
    /// The translation parameters of the pipeline this thread is building.
    /// Per thread: pipelines are built outside the frontend lock
    /// (create_*_pipeline_state), several at once, and a draw-time variant
    /// translates on the queue's thread meanwhile.
    struct BuildScratch {
        /// The SV_ViewID the shaders compile_shader translates are built for.
        u32 view_id{0};
        /// PipelineState::vi_first of the view-instanced pipeline being
        /// built (its views take view 0's).
        u32 vi_first{0};
        /// The mesh layout's ps_primitive_split for the pixel shader being
        /// translated (DxbcTranslationOptions::pixel_primitive_split).
        std::array<u8, 32> primitive_split{};
    };
    [[nodiscard]] static BuildScratch& build_scratch();
    /// The first stage of @p src that reads SV_ViewID (kVi*).
    [[nodiscard]] u32 view_instancing_split(const PipelineSource& src, bool mesh);
    /// The vertex capture and the pass-through an adjacency draw with no
    /// geometry shader draws through (PipelineState::adjacency_msl).
    [[nodiscard]] u32 ensure_vertex_replay(PipelineState& pso);
    /// Compile the shaders, resolve the root signature and build the backend
    /// pipeline of @p pso (whose graphics state, flags and `compute` are
    /// already read).
    [[nodiscard]] u32 build_pipeline(const PipelineSource& src,
                                     PipelineState& pso, const char* api);
    /// D3D12_STREAM_OUTPUT_DESC. S_OK stores it on @p pso. A declaration
    /// the shader cannot stream is E_INVALIDARG. Domain-shader stream
    /// output, which the shared repack cannot express, is E_NOTIMPL.
    [[nodiscard]] u32 accept_stream_output(const PipelineSource& src,
                                           PipelineState& pso, const char* api);
    [[nodiscard]] bool shader_present(GuestAddr bytecode_struct);
    /// The digest GetCachedBlob hands out for @p pso.
    [[nodiscard]] u64 pipeline_digest(const PipelineState& pso,
                                      const PipelineSource& src);
    /// Whether @p root binds every register @p shader declares, for a stage
    /// of @p visibility (D3D12_SHADER_VISIBILITY; ALL for compute).
    [[nodiscard]] static bool root_covers(const RootLayout& root,
                                          const CompiledShader& shader,
                                          u32 visibility);
    [[nodiscard]] u32 read_input_layout(Marshaler& m, GuestAddr elements,
                                        u32 element_count,
                                        GraphicsState& state);
    [[nodiscard]] u32 read_shader_bytes(GuestAddr bytecode_struct,
                                        std::vector<u8>& out);
    /// A DXIL (shader model 6) container in @p bytes is replaced by its
    /// shader model 5.1 lowering (graphics/shaders/dxil_to_dxbc.hpp), so
    /// every later stage reads DXBC. E_INVALIDARG for an unsigned or
    /// malformed container, or one using what the lowering refuses.
    [[nodiscard]] u32 lower_dxil(std::vector<u8>& bytes);
    /// Translate @p bytecode (empty: no shader). @p root, when known, sizes
    /// SM 5.1 unbounded arrays.
    /// The output-merger logic op a pixel shader variant performs itself
    /// (DxbcTranslationOptions::logic_op): Metal has none.
    struct LogicOpVariant {
        /// The one op every target uses when `per_target` is false.
        u32 op{0};
        u32 targets{0};
        std::array<std::array<u32, 4>, 8> component_masks{};
        /// IndependentBlendEnable named different D3D12_LOGIC_OP values.
        /// `ops[n]` is target n's op; the shader applies it to o#n only.
        bool per_target{false};
        std::array<u32, 8> ops{};
    };
    [[nodiscard]] u32 logic_op_variant(const GraphicsState& state,
                                       LogicOpVariant& out, const char* api);
    /// Options a draw-time variant adds to compile_shader's translation.
    struct ShaderVariantOptions {
        /// Fragment: count PSInvocations into the statistics record
        /// (DxbcTranslationOptions::count_pixel_invocations).
        bool count_pixel_invocations{false};
        /// Fragment behind a mesh shader: SV_PrimitiveID is the mesh
        /// shader's per-primitive output
        /// (DxbcTranslationOptions::primitive_id_varying).
        bool primitive_id_varying{false};
        /// Fragment: the pipeline rasterizes lines
        /// (DxbcTranslationOptions::line_barycentrics).
        bool line_barycentrics{false};
        /// Vertex: rasterize as usual and also write every output,
        /// draw-relative, into the capture buffer
        /// (capture_rasterized_vertex_outputs + capture_draw_relative).
        bool capture_rasterized{false};
        /// Vertex input specialisation (as pipeline_variant's).
        u32 vertex_input_unpack_uint1010102{0};
        std::array<u8, 32> vertex_input_component_types{};
        bool vertex_pulling{false};
        std::array<graphics::VertexPullElement, 32> vertex_pull{};
        /// Out: the captured registers in record order.
        std::vector<u32>* capture_registers{nullptr};
        /// Fragment: the SV_Target locations with a colour attachment
        /// (color_output_mask); the others are not written
        /// (MslCompileOptions::fragment_output_mask).
        u32 color_output_mask{0xFFFFFFFFu};
    };
    [[nodiscard]] u32 compile_shader(std::vector<u8> bytecode,
                                     graphics::ShaderStage stage,
                                     CompiledShader& out, const char* api,
                                     const RootLayout* root,
                                     u32 visibility_stage,
                                     const LogicOpVariant* logic = nullptr,
                                     const ShaderVariantOptions* variant =
                                         nullptr,
                                     const graphics::RasterEmulationOptions*
                                         raster = nullptr,
                                     const graphics::MeshStageLayout* mesh =
                                         nullptr,
                                     u32 mesh_payload_words = 0);
    /// The amplification and mesh shaders of a mesh pipeline (DXIL only),
    /// validated and compiled against the pixel shader's input signature
    /// (d3d12_pipeline_mesh.cpp).
    [[nodiscard]] u32 compile_mesh_stages(const PipelineSource& src,
                                          PipelineState& pso,
                                          std::vector<u8> ms_bytes,
                                          const std::vector<u8>& ps_bytes,
                                          const char* api);
    /// The Metal object + mesh pipeline of a compiled mesh pipeline.
    [[nodiscard]] u32 finish_mesh_pipeline(PipelineState& pso, const char* api);
    /// Its PIPELINE_STATISTICS variant, built on first use; null when it
    /// cannot be built.
    [[nodiscard]] graphics::BackendPipeline* mesh_statistics_pipeline(PipelineState& pso);
    /// ViewInstancingTier 3 variants of a mesh pipeline (kMeshView*), the
    /// counting one with @p statistics; null when one cannot be built.
    [[nodiscard]] graphics::BackendPipeline* mesh_view_pipeline(PipelineState& pso, u32 kind,
                                                                bool statistics);
    /// @p object: -1 the pipeline's amplification shader decides, 0 none,
    /// 1 @p as_msl; @p payload_bytes 0: the amplification payload's.
    [[nodiscard]] std::unique_ptr<graphics::BackendPipeline> make_mesh_pipeline(
        const PipelineState& pso, const std::string& ms_msl, const std::string& as_msl,
        const std::string& fragment_msl, u64 cache_key, int object = -1,
        u32 payload_bytes = 0);
    [[nodiscard]] u32 finish_graphics_pipeline(PipelineState& pso,
                                               const char* api);
    [[nodiscard]] u32 finish_compute_pipeline(PipelineState& pso,
                                              const char* api);
    [[nodiscard]] graphics::BackendPipeline* pipeline_variant(
        PipelineState& pso,
        const std::array<u32, graphics::BACKEND_MAX_VERTEX_BUFFERS>& strides,
        bool statistics = false);
    /// pipeline_variant's pipeline for @p strides, not cached; @p dynamic
    /// builds it with BackendPipelineDesc::dynamic_vertex_strides (strides
    /// unused) and records the streams it reads in pso.dynamic_stream_mask.
    [[nodiscard]] std::unique_ptr<graphics::BackendPipeline> make_render_variant(
        PipelineState& pso,
        const std::array<u32, graphics::BACKEND_MAX_VERTEX_BUFFERS>& strides,
        bool statistics, bool dynamic);
    /// vitreo_stats_* (graphics/shaders/pipeline_stats.hpp) and
    /// vitreo_ia_resolve (ia_resolve.hpp), built on first use; null when the
    /// backend cannot build them.
    [[nodiscard]] graphics::BackendComputePipeline* statistics_kernel(
        const char* entry_point);
    [[nodiscard]] graphics::BackendComputePipeline* ia_resolve_kernel();
    std::unordered_map<std::string,
                       std::unique_ptr<graphics::BackendComputePipeline>>
        m_stats_kernels;
    std::unique_ptr<graphics::BackendComputePipeline> m_ia_resolve;
    /// vitreo_ia_cut_rewrite (ia_cut_rewrite.hpp), built on first use.
    [[nodiscard]] graphics::BackendComputePipeline* ia_cut_kernel();
    std::unique_ptr<graphics::BackendComputePipeline> m_ia_cut;
    /// The kernel that splits an indirect DispatchMesh into device-staging
    /// chunks (d3d12_replay_mesh.cpp).
    std::unique_ptr<graphics::BackendComputePipeline> m_mesh_chunk_resolve;
    /// Capacities of a GPU-assembled statistics draw: captured vertex
    /// records (an indexed draw's index values are unknown to the CPU) and
    /// primitives (an indirect draw's count). A draw past either is flagged
    /// unmeasured by the kernels; the resolve it left raises them for the
    /// next draw once its GPU work has completed.
    u32 m_stats_record_capacity{1u << 16};
    u32 m_stats_primitive_capacity{1u << 16};
    struct StatsFeedback {
        std::shared_ptr<graphics::BackendBuffer> resolved;
        u64 generation{0};
    };
    std::vector<StatsFeedback> m_stats_feedback;
    /// Vertex records a GPU-assembled geometry / tessellation draw
    /// (ExecuteIndirect) captures; its primitives are bounded by the same
    /// and by the passes' size limit. A draw needing more is dropped whole
    /// (vitreo_ia_resolve's overflow flag), and the resolve it left raises
    /// this for the next draws once its GPU work has completed
    /// (poll_lowered_feedback). The CPU never waits for it.
    u32 m_lowered_record_capacity{1u << 16};
    std::vector<StatsFeedback> m_lowered_feedback;
    /// Retire m_lowered_feedback entries whose GPU work has completed.
    void poll_lowered_feedback();
    /// vitreo_indirect_count (indirect_count.hpp), built on first use.
    [[nodiscard]] graphics::BackendComputePipeline* indirect_count_kernel();
    std::unique_ptr<graphics::BackendComputePipeline> m_indirect_count;
    /// A capture header of {0, 0}: a statistics vertex shader drawn with no
    /// clipper plan writes no record through it.
    std::unique_ptr<graphics::BackendBuffer> m_stats_null_capture;
    /// The word a heap window UAV without a counter resource binds in its
    /// auxiliary counter slot (Replayer::resolve_heap_window_slot).
    std::unique_ptr<graphics::BackendBuffer> m_window_counter_scratch;
    /// Geometry, hull and domain stages, lowered the way the D3D11 frontend
    /// lowers them. S_OK when the pipeline has none, or when each present
    /// stage compiled. E_INVALIDARG for a stage that is not that shader;
    /// E_NOTIMPL, with one UNIMPLEMENTED line, when the lowering cannot
    /// draw it.
    [[nodiscard]] u32 lower_shader_stages(const PipelineSource& src,
                                          PipelineState& pso, const char* api);
    /// The capture variant of the vertex shader (rasterization off), keyed
    /// by the bound vertex strides.
    /// @p relative keys records draw-relative behind a 16-byte header
    /// (DxbcTranslationOptions::capture_draw_relative), as the GPU resolve
    /// of an indirect draw lays them out.
    [[nodiscard]] graphics::BackendPipeline* capture_variant(
        PipelineState& pso,
        const std::array<u32, graphics::BACKEND_MAX_VERTEX_BUFFERS>& strides,
        bool relative = false);
    /// The pass-through vertex shader plus the pipeline's pixel shader.
    /// `topology` is what the pass-through draw issues: Metal requires the
    /// pipeline to declare its input primitive class when the vertex
    /// function writes [[render_target_array_index]] or
    /// [[viewport_array_index]], as pass-through shaders do.
    [[nodiscard]] graphics::BackendPipeline* passthrough_variant(
        PipelineState& pso, const std::string& vertex_msl,
        std::unique_ptr<graphics::BackendPipeline>& slot,
        graphics::BackendPrimitiveTopology topology,
        bool statistics = false);
    /// The pixel shader's MSL counting PSInvocations, or its own MSL when
    /// that translation failed (PipelineState::statistics_pixels false).
    [[nodiscard]] const std::string& statistics_fragment(PipelineState& pso);
    /// The 1x1 target a capture draw rasterizes into (nothing is kept).
    [[nodiscard]] graphics::BackendTexture2D* capture_target();
    /// The software tessellator's two compute kernels.
    [[nodiscard]] bool tessellator_kernels(
        graphics::BackendComputePipeline*& patch,
        graphics::BackendComputePipeline*& scan);
    /// Bytes at a buffer GPU VA. CPU-visible memory is copied; a GPU-only
    /// buffer is read back when the backend can. False when it cannot.
    [[nodiscard]] bool read_buffer_va(u64 va, void* out, usize bytes);
    /// Bytes at a GPU VA in an UPLOAD heap, whose contents only the CPU
    /// writes: what the GPU will read, with no wait. False elsewhere.
    [[nodiscard]] bool read_upload_va(u64 va, void* out, usize bytes);
    void root_signature_method(Marshaler& m, RootSignature& root, usize slot);
    void pipeline_state_method(Marshaler& m, PipelineState& pso, usize slot);
    /// SOSetTargets bindings for the list currently replaying. Each list
    /// starts with none; a bundle inherits the caller's. The filled-size
    /// word is a u32 at `filled_va` inside a buffer.
    struct SoTarget {
        bool bound{false};
        u64 data_va{0};
        u64 size{0};
        u64 filled_va{0};
    };
    std::array<SoTarget, D3D12_SO_BUFFER_SLOT_COUNT> m_so_targets{};
    std::unique_ptr<graphics::BackendComputePipeline> m_so_prepare;
    std::unique_ptr<graphics::BackendComputePipeline> m_so_repack;
    std::unique_ptr<graphics::BackendComputePipeline> m_so_compact;
    std::shared_ptr<graphics::BackendBuffer> m_so_stats;

    // -- command lists (d3d12_command_list.cpp) ------------------------------
    void create_command_allocator(Marshaler& m, Device& device);
    void create_command_list(Marshaler& m, Device& device, bool list1);
    /// ID3D12GraphicsCommandList4::BeginRenderPass (tier 0 recording).
    void begin_render_pass(Marshaler& m, CommandList& list, const char* name);
    void command_allocator_method(Marshaler& m, CommandAllocator& allocator,
                                  usize slot);
    void command_list_method(Marshaler& m, CommandList& list, usize slot);
    void record_error(CommandList& list, u32 error, const char* method,
                      const char* why);
    [[nodiscard]] bool snapshot_cpu_descriptor(GuestAddr handle,
                                               DescriptorType type,
                                               Descriptor& out);

    // -- queues, fences and execution (d3d12_queue.cpp) ---------------------
    void create_command_queue(Marshaler& m, Device& device,
                              bool queue1 = false);
    /// Remove the device (GetDeviceRemovedReason then reports @p reason):
    /// creation fails with DXGI_ERROR_DEVICE_REMOVED and every fence reads
    /// UINT64_MAX (Learn, "Device removed"). The first reason sticks.
    void remove_device(u32 reason, const char* why, Device* which = nullptr);
    [[nodiscard]] bool device_removed() const;
    /// ID3D12Device1::SetEventOnMultipleFenceCompletion.
    void set_event_on_multiple_fences(Marshaler& m);
    void create_fence(Marshaler& m, Device& device);
    void command_queue_method(Marshaler& m, CommandQueue& queue, usize slot);
    void queue_update_tile_mappings(Marshaler& m, CommandQueue& queue);
    void queue_copy_tile_mappings(Marshaler& m, CommandQueue& queue);
    /// Point @p resource's tiles at @p changes (later entries already won).
    /// References pool tiles first; on allocation failure the mappings are
    /// unchanged and the device is removed. Backend updates are ordered with
    /// this queue's pending work.
    void apply_tile_changes(CommandQueue& queue, Resource& resource,
                            const std::map<u32, TileAssignment>& changes);
    void fence_method(Marshaler& m, Fence& fence, usize slot);
    void pump_queue(CommandQueue& queue);
    void pump_all_queues();
    void execute(CommandQueue& queue,
                 const std::vector<std::shared_ptr<const Recording>>& lists);
    [[nodiscard]] bool queue_signal(Fence& fence, u64 value);
    /// Set @p fence to @p value now, lower ones included (measured, a04
    /// "fence CPU Signal(3) (lower)": S_OK completed=3).
    [[nodiscard]] bool cpu_signal(Fence& fence, u64 value);

    // -- replay (d3d12_replay.cpp) -------------------------------------------
    /// Translate one closed list's commands into backend work, in order,
    /// resolving root signatures and descriptor tables now.
    void replay_list(CommandQueue& queue, const Recording& recording);
    /// Drop a render pass suspended during this ExecuteCommandLists and not
    /// resumed by the end of it (Learn: the pair is one ExecuteCommandLists).
    void end_execute_batch();
    /// CopyTiles recorded for a mapped reserved buffer or 2D texture.
    void replay_copy_tiles(const cmd::CopyTiles& command);
    /// Persistent backend objects the replay reuses across lists: descriptor
    /// tables, the root-constant upload ring, a zero buffer.
    std::shared_ptr<struct ReplayResources> m_replay;
    std::unique_ptr<graphics::BackendTexture2D> m_capture_target;
    std::unique_ptr<graphics::BackendComputePipeline> m_tess_patch;
    std::unique_ptr<graphics::BackendComputePipeline> m_tess_scan;

    // -- raytracing (d3d12_raytracing.cpp) -----------------------------------
    /// D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS at @p at. False
    /// with @p why for inputs the call must refuse; @p unimplemented set
    /// when they are valid but use a part Vitreo lacks (opacity micromaps).
    [[nodiscard]] bool read_rt_inputs(GuestAddr at, RtInputs& out,
                                      const char*& why, bool& unimplemented);
    /// The blob and scratch layout of @p inputs; false when the backend
    /// has no ray tracing or cannot size them.
    [[nodiscard]] bool rt_layout(const RtInputs& inputs, RtLayout& out);
    void prebuild_info(Marshaler& m);
    void check_driver_matching_identifier(Marshaler& m);
    /// The command-list DXR methods; false when @p slot is not one.
    [[nodiscard]] bool record_raytracing(Marshaler& m, CommandList& list,
                                         usize slot, const char* name);
    /// The structure whose blob starts at @p va, or null.
    [[nodiscard]] AccelerationStructure* acceleration_structure_at(u64 va);
    std::map<u64, AccelerationStructure> m_acceleration_structures;
    /// Bumped whenever the backend object a VA maps to changes (a heap
    /// window re-resolves its acceleration-structure slots then).
    u64 m_rt_generation{1};
    /// Every guest GPU-memory range, for a pass that dereferences VAs the
    /// app wrote (ARRAY_OF_POINTERS instance descriptions).
    [[nodiscard]] std::vector<graphics::BackendRtMemoryRange> rt_memory_ranges() const;

    // -- state objects (d3d12_state_object.cpp) -------------------------------
    /// CreateStateObject (@p grow 0) and AddToStateObject.
    void create_state_object(Marshaler& m, Device& device, bool add);
    void state_object_properties_method(Marshaler& m, StateObject& so, usize slot);
    /// Identifier indices handed out so far (device-wide).
    u32 m_next_identifier{1};

    // Query heaps / command signatures (d3d12_device.cpp).
    void create_query_heap(Marshaler& m, Device& device);
    void create_command_signature(Marshaler& m, Device& device);
    void simple_child_method(Marshaler& m, Object& object, usize slot);
};


/// Takes the frontend lock (d3d12_lock.hpp) in @p mode, exclusive unless
/// told otherwise, and profiles the wait (VITREO_D3D12_PROFILE).
class FrontendLock {
public:
    explicit FrontendLock(Frontend& frontend,
                          FrontendRwLock::Mode mode = FrontendRwLock::Mode::Exclusive)
        : m_frontend(frontend) {
        if (graphics::frame_profile::enabled()) [[unlikely]] {
            const u64 start = graphics::frame_profile::now_ns();
            if (m_frontend.m_lock.lock(mode)) {
                graphics::frame_profile::add(
                    graphics::frame_profile::Time::LockWait,
                    graphics::frame_profile::now_ns() - start);
                graphics::frame_profile::add(
                    graphics::frame_profile::Counter::LockContended);
            }
        } else {
            (void)m_frontend.m_lock.lock(mode);
        }
    }
    ~FrontendLock() { m_frontend.m_lock.unlock(); }
    FrontendLock(const FrontendLock&) = delete;
    FrontendLock& operator=(const FrontendLock&) = delete;

private:
    Frontend& m_frontend;
};

/// Releases the frontend lock for its scope -- only when the calling thread
/// (which must hold it) holds it exactly once, in either mode. Held more
/// deeply (a guest callback made under the lock re-entered the frontend),
/// the outer frame still uses the objects it looked up, so the lock stays
/// held and released() is false. Everything the scope touches must be the
/// caller's own or separately synchronized. The hold is retaken in the mode
/// it had.
class FrontendUnlock {
public:
    explicit FrontendUnlock(Frontend& frontend) : m_frontend(frontend) {
        m_mode = m_frontend.m_lock.release_if_outermost();
    }
    ~FrontendUnlock() {
        if (m_mode == FrontendRwLock::Mode::None) return;
        if (graphics::frame_profile::enabled()) [[unlikely]] {
            const u64 start = graphics::frame_profile::now_ns();
            if (m_frontend.m_lock.reacquire(m_mode)) {
                graphics::frame_profile::add(
                    graphics::frame_profile::Time::LockWait,
                    graphics::frame_profile::now_ns() - start);
                graphics::frame_profile::add(
                    graphics::frame_profile::Counter::LockContended);
            }
        } else {
            (void)m_frontend.m_lock.reacquire(m_mode);
        }
    }
    [[nodiscard]] bool released() const noexcept {
        return m_mode != FrontendRwLock::Mode::None;
    }
    FrontendUnlock(const FrontendUnlock&) = delete;
    FrontendUnlock& operator=(const FrontendUnlock&) = delete;

private:
    Frontend& m_frontend;
    FrontendRwLock::Mode m_mode{FrontendRwLock::Mode::None};
};

/// m_replay_mutex for a whole ExecuteCommandLists, its wait profiled as a
/// lock wait.
class ReplayLock {
public:
    explicit ReplayLock(Frontend& frontend) : m_lock(frontend.m_replay_mutex, std::defer_lock) {
        if (m_lock.try_lock()) return;
        const bool profile = graphics::frame_profile::enabled();
        const u64 start = profile ? graphics::frame_profile::now_ns() : 0;
        m_lock.lock();
        if (profile) {
            graphics::frame_profile::add(graphics::frame_profile::Time::LockWait,
                                         graphics::frame_profile::now_ns() - start);
            graphics::frame_profile::add(graphics::frame_profile::Counter::LockContended);
        }
    }

private:
    std::unique_lock<std::mutex> m_lock;
};

/// The D3D12_COMPATIBILITY_SHARED_FLAGS rules of ID3D12CompatibilityDevice::
/// CreateSharedHeap / CreateSharedResource (d3d12_config.cpp): S_OK, or the
/// failure (a loud one for the flags Vitreo has no interop for).
[[nodiscard]] u32 compatibility_flags_error(u32 flags, u32 heap_flags,
                                            const char* api);

/// A DXGI *_TYPELESS format (its textures are viewed as typed members).
[[nodiscard]] bool texture_is_typeless(u32 dxgi_format);

/// The DXGI_FORMAT -> backend storage format of a texture resource.
[[nodiscard]] graphics::PixelFormat texture_format_for(u32 dxgi_format,
                                                       u32 resource_flags);

/// Bytes per element of a DXGI format for buffer views and copies; 0 when the
/// format has none (block compressed formats report their block size through
/// format_block_info).
[[nodiscard]] u32 dxgi_element_bytes(u32 dxgi_format);
/// Words 1-7 of a raw typed buffer entry (graphics::kTypedBufferRaw) for a
/// view of @p dxgi_format whose element 0 is at @p device_address, with an
/// SRV's @p component_mapping (D3D12 encoding): false for a format the raw
/// path has no layout for (d3d12_typed_raw.cpp).
[[nodiscard]] bool typed_raw_layout(u32 dxgi_format, u64 device_address,
                                    u32 num_elements, u32 component_mapping,
                                    std::array<u32, 7>& words);
/// The element bytes of a typed buffer view of @p dxgi_format the raw path
/// can read, 0 when it has no layout for the format.
[[nodiscard]] u32 typed_raw_element_bytes(u32 dxgi_format);

/// Block extent and bytes of @p dxgi_format (1x1 for uncompressed).
struct FormatBlock {
    u32 width{1};
    u32 height{1};
    u32 bytes{0};
};
[[nodiscard]] FormatBlock format_block_info(u32 dxgi_format);

}  // namespace vitreo::winapi::d3d12
