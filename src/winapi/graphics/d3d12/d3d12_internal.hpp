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
#include "winapi/graphics/d3d12/d3d12_vtables.hpp"
#include "winapi/core/guest_com.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/shader_types.hpp"

#include <vitreo/common.hpp>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi::d3d12 {

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

inline void d3d12_diag(const char* format, ...) {
    if (!diag_enabled()) return;
    char buffer[768];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    std::fprintf(stderr, "[d3d12-diag] %s\n", buffer);
}

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
};

struct Device final : Object {
    Device() : Object(Kind::Device) {}
    u32 feature_level{0};
    /// GetDeviceRemovedReason; S_OK while the device is usable.
    u32 removed_reason{0};
};

// -----------------------------------------------------------------------------
// Memory, heaps and resources
// -----------------------------------------------------------------------------

/// A block of GPU memory: an ID3D12Heap's, or the implicit heap of a committed
/// resource. Buffers are windows of `backend->buffer()`.
struct Memory {
    std::unique_ptr<graphics::BackendHeap> backend;
    u64 size{0};
    /// GPU virtual address of byte 0 (every buffer VA is va + its offset).
    u64 va{0};
    /// CPU-visible memory: the persistent guest mapping of byte 0.
    GuestAddr guest{0};
    usize guest_size{0};
    bool cpu_visible{false};
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
    std::shared_ptr<Memory> memory;
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
};

struct Resource final : Object {
    Resource() : Object(Kind::Resource) {}
    ResourceDescription desc{};
    HeapProperties heap_properties{};
    u32 heap_flags{0};
    bool committed{false};
    bool placed{false};
    bool reserved{false};
    /// Placed: the ID3D12Heap (no COM reference: see create_placed_resource).
    GuestAddr heap_object{0};
    /// Buffers (and placed textures): the memory and this resource's offset in
    /// it. A buffer's GPU VA is memory->va + offset.
    std::shared_ptr<Memory> memory;
    u64 offset{0};
    std::unique_ptr<graphics::BackendTexture2D> texture;
    std::unique_ptr<graphics::BackendTexture3D> texture3d;
    graphics::PixelFormat pixel_format{graphics::PixelFormat::Unknown};
    u32 initial_state{0};
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
        return is_buffer() && memory ? memory->va + offset : 0;
    }
    [[nodiscard]] graphics::BackendBuffer* buffer() const {
        return is_buffer() && memory && memory->backend
                   ? memory->backend->buffer()
                   : nullptr;
    }
    [[nodiscard]] u32 array_size() const {
        return desc.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                   ? 1u
                   : desc.depth_or_array_size;
    }
    [[nodiscard]] u32 subresource_count() const;
};

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
};

struct PipelineState final : Object {
    PipelineState() : Object(Kind::PipelineState) {}
    /// What GetCachedBlob hands out and a CachedPSO must match: a digest of
    /// the description the pipeline was created from.
    u64 cache_digest{0};
    bool compute{false};
    std::shared_ptr<const RootLayout> root;
    CompiledShader vs, ps, cs;
    GraphicsState graphics{};
    u32 flags{0};
    u32 node_mask{0};
    /// Backend render pipelines by the vertex strides bound at draw time
    /// (D3D12 strides are dynamic; Metal bakes them into the pipeline).
    std::unordered_map<u64, std::unique_ptr<graphics::BackendPipeline>>
        variants;
    std::unique_ptr<graphics::BackendComputePipeline> compute_pipeline;
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
struct SetViewports { std::vector<Viewport> viewports; };
struct SetScissors { std::vector<Rect> rects; };
struct SetRenderTargets { u32 count; std::array<Descriptor, 8> rtvs; bool has_dsv; Descriptor dsv; };
struct SetBlendFactor { std::array<float, 4> factor; };
struct SetStencilRef { u32 front; u32 back; };
struct SetDepthBounds { float min; float max; };
struct Draw { u32 vertex_count, instance_count, start_vertex, start_instance; };
struct DrawIndexed { u32 index_count, instance_count, start_index; i32 base_vertex; u32 start_instance; };
struct Dispatch { u32 x, y, z; };
struct CopyBufferRegion { GuestAddr dst; u64 dst_serial; u64 dst_offset; GuestAddr src; u64 src_serial; u64 src_offset; u64 bytes; };
struct CopyTextureRegion { CopyLocation dst; u32 x, y, z; CopyLocation src; bool has_box; u32 box[6]; };
struct CopyResource { GuestAddr dst; u64 dst_serial; GuestAddr src; u64 src_serial; };
struct ResolveSubresource { GuestAddr dst; u64 dst_serial; u32 dst_sub; GuestAddr src; u64 src_serial; u32 src_sub; u32 format; u32 dst_x{0}; u32 dst_y{0}; bool has_rect{false}; std::array<i32, 4> rect{}; };
struct ClearRenderTarget { Descriptor rtv; std::array<float, 4> color; std::vector<Rect> rects; };
struct ClearDepthStencil { Descriptor dsv; u32 flags; float depth; u8 stencil; std::vector<Rect> rects; };
struct ClearUnorderedAccess { Descriptor uav; bool is_float; std::array<u32, 4> values; std::vector<Rect> rects; };
struct Event { u32 kind; std::string label; };  ///< 0 marker, 1 begin, 2 end
struct ExecuteBundle { std::shared_ptr<const Recording> bundle; };
struct WriteImmediate { std::vector<std::pair<u64, u32>> writes; };
struct Barrier { bool uav; };
struct ClearState { GuestAddr pso; u64 serial; };
struct Query { GuestAddr heap; u64 serial; u32 type; u32 index; bool begin; };
struct ResolveQuery { GuestAddr heap; u64 heap_serial; u32 type; u32 start; u32 count; GuestAddr dst; u64 dst_serial; u64 offset; };
struct SetPredication { GuestAddr buffer; u64 serial; u64 offset; u32 op; };
struct ExecuteIndirect { GuestAddr signature; u64 signature_serial; u32 max_count; GuestAddr args; u64 args_serial; u64 args_offset; GuestAddr count; u64 count_serial; u64 count_offset; };
}  // namespace cmd

using Command = std::variant<
    cmd::SetPipelineState, cmd::SetRootSignature, cmd::SetDescriptorHeaps,
    cmd::SetRootTable, cmd::SetRootConstants, cmd::SetRootView,
    cmd::SetTopology, cmd::SetVertexBuffers, cmd::SetIndexBuffer,
    cmd::SetViewports, cmd::SetScissors, cmd::SetRenderTargets,
    cmd::SetBlendFactor, cmd::SetStencilRef, cmd::SetDepthBounds, cmd::Draw,
    cmd::DrawIndexed, cmd::Dispatch, cmd::CopyBufferRegion,
    cmd::CopyTextureRegion, cmd::CopyResource, cmd::ResolveSubresource,
    cmd::ClearRenderTarget, cmd::ClearDepthStencil,
    cmd::ClearUnorderedAccess, cmd::Event, cmd::ExecuteBundle,
    cmd::WriteImmediate, cmd::Barrier, cmd::ClearState, cmd::Query,
    cmd::ResolveQuery, cmd::SetPredication, cmd::ExecuteIndirect>;

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
    u32 recording_lists{0};
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
    /// the clears, bindings and resolves it stands for).
    bool in_render_pass{false};
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

struct Fence final : Object {
    Fence() : Object(Kind::Fence) {}
    u32 flags{0};
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
        enum class Op : u8 { Execute, Signal, Wait, Present };
        Op op{Op::Execute};
        std::vector<std::shared_ptr<const Recording>> recordings;
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
    /// DXGI_SWAP_CHAIN_DESC1.
    u32 width{0}, height{0}, format{0}, stereo{0}, buffer_usage{0};
    u32 buffer_count{0}, scaling{0}, swap_effect{0}, alpha_mode{0}, flags{0};
    /// DXGI_SWAP_CHAIN_FULLSCREEN_DESC.
    u32 refresh_numerator{0}, refresh_denominator{0}, scanline_ordering{0};
    u32 fullscreen_scaling{0};
    bool windowed{true};
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
    std::recursive_mutex m_mutex;
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
    // -- swap chains (d3d12_swapchain.cpp) ------------------------------------
    D3D12Manager::PresentationServices m_presentation{};
    enum class SwapChainApi : u8 { Legacy, Hwnd, Composition };
    void create_swap_chain(Marshaler& m, SwapChainApi api);
    void swap_chain_method(Marshaler& m, SwapChain& chain, usize slot);
    /// Validation of a DXGI_SWAP_CHAIN_DESC1 for a D3D12 chain (flip model).
    [[nodiscard]] u32 validate_swap_chain(const SwapChain& chain,
                                          SwapChainApi api) const;
    /// (Re)create the chain's buffers at its current size / format / count.
    [[nodiscard]] u32 create_swap_chain_buffers(SwapChain& chain);
    void release_swap_chain_buffers(SwapChain& chain);
    [[nodiscard]] u32 present(SwapChain& chain, u32 sync_interval, u32 flags,
                              GuestAddr dirty_rects, u32 dirty_count);
    /// Show @p buffer of @p chain now (in queue order).
    void show(SwapChain& chain, GuestAddr buffer, u64 buffer_serial,
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
                                                          bool buffers_only);
    void retire_memory(Memory* memory);
    /// VA -> (buffer resource memory, offset in it). False when unmapped.
    [[nodiscard]] bool resolve_va(u64 va, u64 bytes, graphics::BackendBuffer*& buffer,
                                  u64& offset);
    void create_heap(Marshaler& m, Device& device, bool heap1);
    void create_committed_resource(Marshaler& m, Device& device,
                                   int variant);
    void create_placed_resource(Marshaler& m, Device& device, int variant);
    void create_reserved_resource(Marshaler& m, Device& device, int variant);
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
    /// D3D12_HEAP_FLAGS rules of CreateHeap (@p heap_object) and of a
    /// committed resource's implicit heap.
    [[nodiscard]] u32 heap_flags_error(u32 flags, const HeapProperties& props,
                                       bool heap_object, const char* api);
    void heap_method(Marshaler& m, Heap& heap, usize slot);
    void resource_method(Marshaler& m, Resource& resource, usize slot);
    /// ID3D12Resource::WriteToSubresource / ReadFromSubresource.
    void cpu_subresource_copy(Marshaler& m, Resource& resource, bool write);
    std::map<u64, Memory*> m_va_ranges;
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
    void create_graphics_pipeline_state(Marshaler& m, Device& device);
    void create_compute_pipeline_state(Marshaler& m, Device& device);
    void create_pipeline_state(Marshaler& m, Device& device);
    /// The guest addresses of a pipeline's parts, from a desc struct or a
    /// pipeline state stream (0: absent).
    struct PipelineSource {
        GuestAddr root{0};
        GuestAddr vs{0}, ps{0}, ds{0}, hs{0}, gs{0}, cs{0}, as{0}, ms{0};
        u32 stream_output_entries{0};
        GuestAddr cached_blob{0};
        GuestAddr cached_size{0};
        u32 view_instances{0};
    };
    /// Compile the shaders, resolve the root signature and build the backend
    /// pipeline of @p pso (whose graphics state, flags and `compute` are
    /// already read).
    [[nodiscard]] u32 build_pipeline(const PipelineSource& src,
                                     PipelineState& pso, const char* api);
    [[nodiscard]] bool shader_present(GuestAddr bytecode_struct);
    /// The digest GetCachedBlob hands out for @p pso.
    [[nodiscard]] u64 pipeline_digest(const PipelineState& pso,
                                      GuestAddr root_address);
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
    /// Translate @p bytecode (empty: no shader). @p root, when known, sizes
    /// SM 5.1 unbounded arrays.
    /// The output-merger logic op a pixel shader variant performs itself
    /// (DxbcTranslationOptions::logic_op): Metal has none.
    struct LogicOpVariant {
        u32 op{0};
        u32 targets{0};
        std::array<std::array<u32, 4>, 8> component_masks{};
    };
    [[nodiscard]] u32 logic_op_variant(const GraphicsState& state,
                                       LogicOpVariant& out, const char* api);
    [[nodiscard]] u32 compile_shader(std::vector<u8> bytecode,
                                     graphics::ShaderStage stage,
                                     CompiledShader& out, const char* api,
                                     const RootLayout* root,
                                     u32 visibility_stage,
                                     const LogicOpVariant* logic = nullptr);
    [[nodiscard]] u32 finish_graphics_pipeline(PipelineState& pso,
                                               const char* api);
    [[nodiscard]] u32 finish_compute_pipeline(PipelineState& pso,
                                              const char* api);
    [[nodiscard]] graphics::BackendPipeline* pipeline_variant(
        PipelineState& pso,
        const std::array<u32, graphics::BACKEND_MAX_VERTEX_BUFFERS>& strides);
    void root_signature_method(Marshaler& m, RootSignature& root, usize slot);
    void pipeline_state_method(Marshaler& m, PipelineState& pso, usize slot);

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
    void remove_device(u32 reason, const char* why);
    [[nodiscard]] bool device_removed() const;
    /// ID3D12Device1::SetEventOnMultipleFenceCompletion.
    void set_event_on_multiple_fences(Marshaler& m);
    void create_fence(Marshaler& m, Device& device);
    void command_queue_method(Marshaler& m, CommandQueue& queue, usize slot);
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
    /// Persistent backend objects the replay reuses across lists: descriptor
    /// tables, the root-constant upload ring, a zero buffer.
    std::shared_ptr<struct ReplayResources> m_replay;

    // Query heaps / command signatures (d3d12_device.cpp).
    void create_query_heap(Marshaler& m, Device& device);
    void create_command_signature(Marshaler& m, Device& device);
    void simple_child_method(Marshaler& m, Object& object, usize slot);
};

/// A DXGI *_TYPELESS format (its textures are viewed as typed members).
[[nodiscard]] bool texture_is_typeless(u32 dxgi_format);

/// The DXGI_FORMAT -> backend storage format of a texture resource.
[[nodiscard]] graphics::PixelFormat texture_format_for(u32 dxgi_format,
                                                       u32 resource_flags);

/// Bytes per element of a DXGI format for buffer views and copies; 0 when the
/// format has none (block compressed formats report their block size through
/// format_block_info).
[[nodiscard]] u32 dxgi_element_bytes(u32 dxgi_format);

/// Block extent and bytes of @p dxgi_format (1x1 for uncompressed).
struct FormatBlock {
    u32 width{1};
    u32 height{1};
    u32 bytes{0};
};
[[nodiscard]] FormatBlock format_block_info(u32 dxgi_format);

}  // namespace vitreo::winapi::d3d12
