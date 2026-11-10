// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d12_raytracing.hpp
/// @brief DXR acceleration structures in the D3D12 frontend: the d3d12.h
/// constants, the build inputs as read from guest memory, and the Vitreo
/// acceleration-structure blob a structure's GPU VA range holds.
///
/// DXR (DirectX-Specs Raytracing.md, "Acceleration structure properties")
/// makes an acceleration structure an opaque, implementation-defined blob
/// at a 256-byte-aligned GPU VA, reachable only through the dedicated APIs
/// (build, copy, postbuild info, a shader's SRV). Vitreo's blob is the
/// structure's build input in one canonical form -- what serialization,
/// deserialization, tools visualization and updates need -- and the backend
/// acceleration structure (graphics::BackendAccelerationStructure) the
/// frontend maps the VA to:
///
///   0    header (kRtBlobHeaderBytes): u32 magic kRtBlobMagic, u32 version
///        kRtBlobVersion, u32 type (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_
///        TYPE), u32 build flags, u32 count (geometries or instances),
///        u32 0, u64 blob bytes
///   256  bottom level: count geometry records {u32 type, u32 flags,
///        u32 primitive count, u32 0}; from kRtBlobHeaderBytes + 16 * count
///        rounded up to 256, each geometry's canonical data
///        (graphics::kRtTriangleBytes per triangle, kRtBoxBytes per box),
///        each rounded up to 16 bytes
///   256  top level: count u64 bottom-level VAs, then count 64-byte
///        D3D12_RAYTRACING_INSTANCE_DESCs as the build read them
///
/// The whole blob rounds up to 256 bytes: ResultDataMaxSizeInBytes,
/// CURRENT_SIZE and COMPACTED_SIZE.

#include "graphics/contracts/backend_raytracing.hpp"
#include "winapi/graphics/d3d12/d3d12_constants.hpp"

#include <vitreo/common.hpp>

#include <memory>
#include <vector>

namespace vitreo::winapi::d3d12 {

// d3d12.h (Windows SDK 10.0.26100 / Agility), DXR enums and constants
// (D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS is in d3d12_constants.hpp).
inline constexpr u32 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL = 0;
inline constexpr u32 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL = 1;
inline constexpr u32 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY = 2;
inline constexpr u32 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE = 0x1;
inline constexpr u32 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION = 0x2;
inline constexpr u32 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE = 0x4;
inline constexpr u32 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD = 0x8;
inline constexpr u32 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY = 0x10;
inline constexpr u32 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE = 0x20;
inline constexpr u32 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_OMM_UPDATE = 0x40;
inline constexpr u32 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DISABLE_OMMS = 0x80;
inline constexpr u32 D3D12_ELEMENTS_LAYOUT_ARRAY = 0;
inline constexpr u32 D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS = 1;
inline constexpr u32 D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES = 0;
inline constexpr u32 D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS = 1;
inline constexpr u32 D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES = 2;
inline constexpr u32 D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE = 0x1;
inline constexpr u32 D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION = 0x2;
inline constexpr u32 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE = 0;
inline constexpr u32 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TOOLS_VISUALIZATION = 1;
inline constexpr u32 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION = 2;
inline constexpr u32 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE = 3;
inline constexpr u32 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE = 0;
inline constexpr u32 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT = 1;
inline constexpr u32 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_VISUALIZATION_DECODE_FOR_TOOLS = 2;
inline constexpr u32 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE = 3;
inline constexpr u32 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE = 4;
inline constexpr u32 D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE = 0;
inline constexpr u32 D3D12_SERIALIZED_DATA_APPLICATION_SPECIFIC_DRIVER_STATE = 1;
inline constexpr u64 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT = 256;
inline constexpr u64 D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT = 16;
inline constexpr u64 D3D12_RAYTRACING_TRANSFORM3X4_BYTE_ALIGNMENT = 16;
inline constexpr u64 D3D12_RAYTRACING_AABB_BYTE_ALIGNMENT = 8;
inline constexpr u32 D3D12_RAYTRACING_TIER_1_0 = 10;
inline constexpr u32 D3D12_RAYTRACING_TIER_1_1 = 11;
/// sizeof(D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER(1)):
/// a 32-byte driver matching identifier, then three UINT64s.
inline constexpr u64 kRtSerializedHeaderBytes = 56;
/// sizeof(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_TOOLS_VISUALIZATION_HEADER).
inline constexpr u64 kRtVisualizationHeaderBytes = 8;

inline constexpr u32 kRtBlobMagic = 0x53415256u;  // "VRAS"
inline constexpr u32 kRtBlobVersion = 1;
inline constexpr u64 kRtBlobHeaderBytes = 256;
inline constexpr u64 kRtBlobGeometryRecordBytes = 16;

/// D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER of Vitreo blobs:
/// DriverOpaqueGUID {7e1f3c52-5a8e-4b77-9d31-0c6a2f4b9e18}, then the
/// versioning bytes ("VRAS", kRtBlobVersion, zeros).
inline constexpr u8 kRtDriverMatchingIdentifier[32] = {
    0x52, 0x3c, 0x1f, 0x7e, 0x8e, 0x5a, 0x77, 0x4b, 0x9d, 0x31, 0x0c,
    0x6a, 0x2f, 0x4b, 0x9e, 0x18, 'V',  'R',  'A',  'S',  kRtBlobVersion,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0};

[[nodiscard]] constexpr u64 rt_align(u64 value, u64 alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

/// D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS, read from guest
/// memory (the geometry descriptions are CPU data, copied at the call).
struct RtInputs {
    u32 type{0};
    u32 flags{0};
    u32 count{0};
    u32 layout{0};
    /// Top level: D3D12_GPU_VIRTUAL_ADDRESS InstanceDescs.
    u64 instance_descs{0};
    struct Geometry {
        u32 type{0};
        u32 flags{0};
        u64 transform{0};
        u32 index_format{0};
        u32 vertex_format{0};
        u32 index_count{0};
        u32 vertex_count{0};
        u64 index_buffer{0};
        u64 vertex_buffer{0};
        u64 vertex_stride{0};
        u64 aabb_count{0};
        u64 aabbs{0};
        u64 aabb_stride{0};
        /// Triangles (or boxes) the geometry describes.
        [[nodiscard]] u32 primitives() const {
            if (type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS) {
                return static_cast<u32>(aabb_count);
            }
            return (index_buffer != 0 ? index_count : vertex_count) / 3u;
        }
        [[nodiscard]] bool boxes() const {
            return type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
        }
    };
    std::vector<Geometry> geometries;
};

/// Where a structure's pieces sit in its blob and scratch, from its inputs
/// and the backend's sizes.
struct RtLayout {
    u64 blob_bytes{0};
    /// Bottom level: each geometry's data offset in the blob.
    std::vector<u64> geometry_offsets;
    /// Scratch: top-level instance records at 0, the backend's scratch at
    /// backend_scratch_offset.
    u64 backend_scratch_offset{0};
    u64 scratch_bytes{0};
    u64 update_scratch_bytes{0};
    graphics::BackendRtSizes backend;
};

/// A built acceleration structure, by the VA its blob starts at.
struct AccelerationStructure {
    std::shared_ptr<graphics::BackendAccelerationStructure> backend;
    bool top_level{false};
    u32 flags{0};
    u64 blob_bytes{0};
    u32 count{0};
    struct Geometry {
        bool boxes{false};
        u32 flags{0};
        u32 primitives{0};
        u64 offset{0};
    };
    std::vector<Geometry> geometries;
};

}  // namespace vitreo::winapi::d3d12
