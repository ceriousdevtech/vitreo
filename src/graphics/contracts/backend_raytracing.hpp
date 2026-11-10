// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file backend_raytracing.hpp
/// @brief Backend ray-tracing contract: acceleration structures and the GPU
/// passes a frontend needs to feed them from guest memory.
///
/// D3D12 (DXR) treats an acceleration structure as an opaque blob at a GPU
/// virtual address. The backend's structures are objects of their own; the
/// frontend keeps the blob (its geometry in one canonical layout, below) in
/// the guest's memory and maps each address to a backend object. Every GPU
/// step that reads guest memory -- decoding vertex formats, gathering
/// instance descriptions, converting them to the backend's instance records
/// -- is a pass recorded here, in order with the rest of the stream.
///
/// Canonical geometry (what BackendRtGeometry::data holds):
///  - triangles: primitive_count records of three float3 positions, x, y, z
///    (kRtTriangleBytes each), no index buffer: primitive i is record i;
///  - boxes: primitive_count records of {min x, y, z, max x, y, z} floats
///    (kRtBoxBytes each).
/// Instance descriptions are D3D12_RAYTRACING_INSTANCE_DESC (64 bytes: a
/// row-major 3x4 float transform, InstanceID:24 | InstanceMask:8,
/// InstanceContributionToHitGroupIndex:24 | Flags:8, the bottom level's GPU
/// VA), with the bottom-level VAs also in a separate array.

#include "graphics/contracts/backend_buffer.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace vitreo::graphics {

inline constexpr u64 kRtTriangleBytes = 36;
inline constexpr u64 kRtBoxBytes = 24;
inline constexpr u64 kRtInstanceDescBytes = 64;

class BackendAccelerationStructure {
public:
    virtual ~BackendAccelerationStructure() = default;
    [[nodiscard]] virtual bool valid() const noexcept = 0;
    [[nodiscard]] virtual bool top_level() const noexcept = 0;
    /// Bytes of backend storage.
    [[nodiscard]] virtual u64 size() const noexcept = 0;
};

/// D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS bits the backend
/// honours (ALLOW_UPDATE .. MINIMIZE_MEMORY have the same values).
enum : u32 {
    kRtBuildAllowUpdate = 0x1,
    kRtBuildAllowCompaction = 0x2,
    kRtBuildPreferFastTrace = 0x4,
    kRtBuildPreferFastBuild = 0x8,
    kRtBuildMinimizeMemory = 0x10,
};

/// One geometry of a bottom level, in the canonical layout above.
struct BackendRtGeometry {
    bool boxes{false};
    /// D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE.
    bool opaque{false};
    /// D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION.
    bool no_duplicate_any_hit{false};
    u32 primitive_count{0};
    const BackendBuffer* data{nullptr};
    u64 offset{0};
};

struct BackendRtBuild {
    bool top_level{false};
    u32 flags{0};
    /// Bottom level.
    std::vector<BackendRtGeometry> geometries;
    /// Top level: instance_count backend instance records
    /// (rt_convert_instances' output).
    u32 instance_count{0};
    const BackendBuffer* instances{nullptr};
    u64 instances_offset{0};
};

struct BackendRtSizes {
    u64 structure{0};
    u64 build_scratch{0};
    u64 update_scratch{0};
};

/// D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC VertexFormat, decoded to float
/// (UNORM / SNORM per D3D's conversion rules; two-component formats read z
/// as 0; a fourth component is ignored).
enum class BackendRtVertexFormat : u32 {
    Float3 = 0,
    Float2,
    Half2,
    Half4,
    Snorm16x2,
    Snorm16x4,
    Unorm16x2,
    Unorm16x4,
    Unorm10x3,  ///< R10G10B10A2_UNORM: x, y, z; A2 ignored
    Unorm8x4,
    Unorm8x2,
    Snorm8x4,
    Snorm8x2,
};

/// A triangle geometry's guest inputs, resolved to backend buffers.
struct BackendRtTriangleSource {
    const BackendBuffer* vertices{nullptr};
    u64 vertex_offset{0};
    u64 vertex_stride{0};
    BackendRtVertexFormat vertex_format{BackendRtVertexFormat::Float3};
    /// Null: non-indexed, triangle i is vertices 3i .. 3i+2.
    const BackendBuffer* indices{nullptr};
    u64 index_offset{0};
    /// 2 or 4 (R16_UINT / R32_UINT).
    u32 index_size{0};
    /// Null: no transform. Otherwise a row-major 3x4 float matrix applied
    /// to every position (z = 0 for a two-component format).
    const BackendBuffer* transform{nullptr};
    u64 transform_offset{0};
    u32 triangle_count{0};
};

struct BackendRtBoxSource {
    const BackendBuffer* boxes{nullptr};
    u64 offset{0};
    /// D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE::StrideInBytes (0: every
    /// primitive reads the first box).
    u64 stride{0};
    u32 count{0};
};

/// One guest GPU-memory range, for passes that dereference addresses the
/// app wrote into GPU memory.
struct BackendRtMemoryRange {
    u64 va{0};
    u64 size{0};
    const BackendBuffer* buffer{nullptr};
    u64 offset{0};
};

/// D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS::InstanceDescs.
struct BackendRtInstanceSource {
    const BackendBuffer* buffer{nullptr};
    u64 offset{0};
    /// D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS: count u64 VAs, each naming
    /// one instance description somewhere in `memory`.
    bool pointers{false};
    u32 count{0};
    std::vector<BackendRtMemoryRange> memory;
};

/// A bottom level a top-level build may name, by its guest VA.
struct BackendRtBottomLevel {
    u64 va{0};
    const BackendAccelerationStructure* structure{nullptr};
};

class BackendRaytracing {
public:
    virtual ~BackendRaytracing() = default;

    /// Backend sizes for @p build (counts, kinds and flags only; buffers
    /// may be null).
    [[nodiscard]] virtual bool sizes(const BackendRtBuild& build,
                                     BackendRtSizes& out) = 0;
    /// Bytes of one backend instance record (rt_convert_instances writes
    /// them; BackendRtBuild::instances reads them).
    [[nodiscard]] virtual u64 instance_record_bytes() const = 0;
    [[nodiscard]] virtual std::shared_ptr<BackendAccelerationStructure>
    create(u64 size, bool top_level) = 0;

    /// GPU passes, recorded in stream order.
    /// Decode a triangle geometry into canonical triangles at (dst, offset).
    [[nodiscard]] virtual bool decode_triangles(const BackendRtTriangleSource& src,
                                                const BackendBuffer* dst,
                                                u64 dst_offset) = 0;
    /// Copy boxes into canonical boxes; a box with inverted bounds becomes
    /// the point at its centre (DXR "Degenerate primitives").
    [[nodiscard]] virtual bool decode_boxes(const BackendRtBoxSource& src,
                                            const BackendBuffer* dst,
                                            u64 dst_offset) = 0;
    /// Gather @p src's descriptions into (dst, dst_offset): count u64
    /// bottom-level VAs, then count 64-byte instance descriptions.
    [[nodiscard]] virtual bool gather_instances(const BackendRtInstanceSource& src,
                                                const BackendBuffer* dst,
                                                u64 dst_offset) = 0;
    /// Convert @p count gathered descriptions at (src, src_offset) into
    /// backend instance records at (dst, dst_offset). An instance whose VA
    /// names none of @p bottom_levels (sorted by VA), NULL included, is
    /// inactive: it keeps its index and is never hit.
    [[nodiscard]] virtual bool convert_instances(
        const BackendBuffer* src, u64 src_offset, u32 count,
        const std::vector<BackendRtBottomLevel>& bottom_levels,
        const BackendBuffer* dst, u64 dst_offset) = 0;
    /// Build (or, with @p update_source, refit from it) @p dst.
    [[nodiscard]] virtual bool build(BackendAccelerationStructure& dst,
                                     const BackendRtBuild& build,
                                     const BackendBuffer* scratch,
                                     u64 scratch_offset,
                                     const BackendAccelerationStructure* update_source) = 0;
    [[nodiscard]] virtual bool copy(BackendAccelerationStructure& dst,
                                    const BackendAccelerationStructure& src) = 0;
    /// A top level no ray hits, built once: what a NULL acceleration
    /// structure SRV binds. Null when it cannot be made.
    [[nodiscard]] virtual const BackendAccelerationStructure* empty_top_level() = 0;
};

}  // namespace vitreo::graphics
