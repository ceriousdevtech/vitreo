// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_raytracing.cpp
/// @brief DXR acceleration structures: the build inputs, their sizes
/// (GetRaytracingAccelerationStructurePrebuildInfo), the driver matching
/// identifier, and the command-list methods that record builds, copies and
/// postbuild info (replayed in d3d12_replay_raytracing.cpp). Contract:
/// DirectX-Specs Raytracing.md (BuildRaytracingAccelerationStructure and
/// its structures, "Acceleration structure properties"), d3d12_raytracing.hpp
/// for the blob layout.

#include "winapi/graphics/d3d12/d3d12_internal.hpp"

#include <algorithm>

namespace vitreo::winapi::d3d12 {

namespace {

/// DXR VertexFormat (D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC: the tier 1.0
/// list and the tier 1.1 additions) as the backend decodes it.
[[nodiscard]] bool rt_vertex_format(u32 format, graphics::BackendRtVertexFormat& out) {
    using F = graphics::BackendRtVertexFormat;
    switch (format) {
    case DXGI_FORMAT_R32G32B32_FLOAT: out = F::Float3; return true;
    case DXGI_FORMAT_R32G32_FLOAT: out = F::Float2; return true;
    case DXGI_FORMAT_R16G16_FLOAT: out = F::Half2; return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: out = F::Half4; return true;
    case DXGI_FORMAT_R16G16_SNORM: out = F::Snorm16x2; return true;
    case DXGI_FORMAT_R16G16B16A16_SNORM: out = F::Snorm16x4; return true;
    case DXGI_FORMAT_R16G16_UNORM: out = F::Unorm16x2; return true;
    case DXGI_FORMAT_R16G16B16A16_UNORM: out = F::Unorm16x4; return true;
    case DXGI_FORMAT_R10G10B10A2_UNORM: out = F::Unorm10x3; return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM: out = F::Unorm8x4; return true;
    case DXGI_FORMAT_R8G8_UNORM: out = F::Unorm8x2; return true;
    case DXGI_FORMAT_R8G8B8A8_SNORM: out = F::Snorm8x4; return true;
    case DXGI_FORMAT_R8G8_SNORM: out = F::Snorm8x2; return true;
    default: return false;
    }
}

/// Component size of a vertex format (the VertexBuffer address and stride
/// alignment, D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE rules).
[[nodiscard]] u32 rt_vertex_component_bytes(u32 format) {
    switch (format) {
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return 4;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8_SNORM:
        return 1;
    default:
        return 2;
    }
}

}  // namespace

bool Frontend::read_rt_inputs(GuestAddr at, RtInputs& out, const char*& why,
                              bool& unimplemented_part) {
    unimplemented_part = false;
    why = nullptr;
    if (at == 0) {
        why = "null inputs";
        return false;
    }
    const auto& l = layout::D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS[m_is_64bit];
    out = {};
    out.type = read_u32(at + l.Type);
    out.flags = read_u32(at + l.Flags);
    out.count = read_u32(at + l.NumDescs);
    out.layout = read_u32(at + l.DescsLayout);
    if (out.layout != D3D12_ELEMENTS_LAYOUT_ARRAY &&
        out.layout != D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS) {
        why = "DescsLayout is not a D3D12_ELEMENTS_LAYOUT";
        return false;
    }
    if (out.type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
        out.instance_descs = read_u64(at + l.InstanceDescs);
        if (out.layout == D3D12_ELEMENTS_LAYOUT_ARRAY &&
            out.instance_descs % D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT != 0) {
            why = "InstanceDescs is not 16-byte aligned";
            return false;
        }
        if (out.layout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS && out.instance_descs % 8 != 0) {
            why = "InstanceDescs pointer array is not 8-byte aligned";
            return false;
        }
        return true;
    }
    if (out.type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY) {
        unimplemented_part = true;
        why = "opacity micromap arrays";
        return false;
    }
    if (out.type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL) {
        why = "Type is not a buildable acceleration structure type";
        return false;
    }
    GuestAddr descs = 0;
    if (!read_pointer(at + l.pGeometryDescs, descs) || (out.count != 0 && descs == 0)) {
        why = "no geometry descs";
        return false;
    }
    const auto& g = layout::D3D12_RAYTRACING_GEOMETRY_DESC[m_is_64bit];
    out.geometries.resize(out.count);
    for (u32 index = 0; index < out.count; ++index) {
        GuestAddr desc = descs + static_cast<GuestAddr>(index) * g.size;
        if (out.layout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS &&
            (!read_pointer(descs + index * (m_is_64bit ? 8u : 4u), desc) || desc == 0)) {
            why = "null geometry desc pointer";
            return false;
        }
        RtInputs::Geometry& geometry = out.geometries[index];
        geometry.type = read_u32(desc + g.Type);
        geometry.flags = read_u32(desc + g.Flags);
        if ((geometry.flags & ~(D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE |
                                D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION)) != 0) {
            why = "unknown D3D12_RAYTRACING_GEOMETRY_FLAGS";
            return false;
        }
        if (geometry.type == D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES) {
            unimplemented_part = true;
            why = "opacity micromap triangles";
            return false;
        }
        if (geometry.type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS) {
            geometry.aabb_count = read_u64(desc + g.AABBs_AABBCount);
            geometry.aabbs = read_u64(desc + g.AABBs_AABBs_StartAddress);
            geometry.aabb_stride = read_u64(desc + g.AABBs_AABBs_StrideInBytes);
            if (geometry.aabbs % D3D12_RAYTRACING_AABB_BYTE_ALIGNMENT != 0 ||
                geometry.aabb_stride % D3D12_RAYTRACING_AABB_BYTE_ALIGNMENT != 0) {
                why = "AABBs address or stride is not 8-byte aligned";
                return false;
            }
            if (geometry.aabb_count > 0xFFFFFFFFull) {
                why = "AABBCount exceeds the primitive limit";
                return false;
            }
            continue;
        }
        if (geometry.type != D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES) {
            why = "Type is not a D3D12_RAYTRACING_GEOMETRY_TYPE";
            return false;
        }
        geometry.transform = read_u64(desc + g.Triangles_Transform3x4);
        geometry.index_format = read_u32(desc + g.Triangles_IndexFormat);
        geometry.vertex_format = read_u32(desc + g.Triangles_VertexFormat);
        geometry.index_count = read_u32(desc + g.Triangles_IndexCount);
        geometry.vertex_count = read_u32(desc + g.Triangles_VertexCount);
        geometry.index_buffer = read_u64(desc + g.Triangles_IndexBuffer);
        geometry.vertex_buffer = read_u64(desc + g.Triangles_VertexBuffer_StartAddress);
        geometry.vertex_stride = read_u64(desc + g.Triangles_VertexBuffer_StrideInBytes) & 0xFFFFFFFFull;
        graphics::BackendRtVertexFormat format{};
        if (!rt_vertex_format(geometry.vertex_format, format)) {
            why = "VertexFormat is not a raytracing vertex format";
            return false;
        }
        const u32 component = rt_vertex_component_bytes(geometry.vertex_format);
        if (geometry.vertex_buffer % component != 0 || geometry.vertex_stride % component != 0) {
            why = "VertexBuffer address or stride is not component aligned";
            return false;
        }
        if (geometry.transform % D3D12_RAYTRACING_TRANSFORM3X4_BYTE_ALIGNMENT != 0) {
            why = "Transform3x4 is not 16-byte aligned";
            return false;
        }
        if (geometry.index_buffer == 0) {
            if (geometry.index_format != DXGI_FORMAT_UNKNOWN || geometry.index_count != 0) {
                why = "IndexFormat / IndexCount without an IndexBuffer";
                return false;
            }
        } else if (geometry.index_format == DXGI_FORMAT_R32_UINT ||
                   geometry.index_format == DXGI_FORMAT_R16_UINT) {
            const u32 size = geometry.index_format == DXGI_FORMAT_R32_UINT ? 4u : 2u;
            if (geometry.index_buffer % size != 0) {
                why = "IndexBuffer is not aligned to its format";
                return false;
            }
        } else {
            why = "IndexFormat is not R32_UINT or R16_UINT";
            return false;
        }
    }
    return true;
}

bool Frontend::rt_layout(const RtInputs& inputs, RtLayout& out) {
    graphics::BackendRaytracing* rt = m_backend.raytracing();
    if (rt == nullptr) return false;
    out = {};
    graphics::BackendRtBuild build;
    build.flags = inputs.flags & 0x1Fu;
    if (inputs.type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
        build.top_level = true;
        // An empty top level is built from one inactive record.
        build.instance_count = std::max(inputs.count, 1u);
        if (!rt->sizes(build, out.backend)) return false;
        out.blob_bytes = rt_align(kRtBlobHeaderBytes +
                                      static_cast<u64>(inputs.count) * (8 + graphics::kRtInstanceDescBytes),
                                  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
        const u64 records = rt_align(static_cast<u64>(build.instance_count) * rt->instance_record_bytes(),
                                     D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
        out.backend_scratch_offset = records;
        out.scratch_bytes = records + rt_align(std::max<u64>(out.backend.build_scratch, 1), 256);
        if ((inputs.flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE) != 0) {
            out.update_scratch_bytes = records + rt_align(std::max<u64>(out.backend.update_scratch, 1), 256);
        }
        return true;
    }
    u64 at = rt_align(kRtBlobHeaderBytes + kRtBlobGeometryRecordBytes * inputs.geometries.size(),
                      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    for (const RtInputs::Geometry& g : inputs.geometries) {
        graphics::BackendRtGeometry geometry;
        geometry.boxes = g.boxes();
        geometry.opaque = (g.flags & D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE) != 0;
        geometry.no_duplicate_any_hit =
            (g.flags & D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION) != 0;
        geometry.primitive_count = g.primitives();
        build.geometries.push_back(geometry);
        out.geometry_offsets.push_back(at);
        at += rt_align(static_cast<u64>(geometry.primitive_count) *
                           (geometry.boxes ? graphics::kRtBoxBytes : graphics::kRtTriangleBytes),
                       16);
    }
    if (build.geometries.empty()) {
        // An empty bottom level is one degenerate triangle (no hits).
        graphics::BackendRtGeometry none;
        none.primitive_count = 1;
        build.geometries.push_back(none);
    }
    if (!rt->sizes(build, out.backend)) return false;
    out.blob_bytes = rt_align(at, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    out.backend_scratch_offset = 0;
    out.scratch_bytes = rt_align(std::max<u64>(out.backend.build_scratch, 1), 256);
    if ((inputs.flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE) != 0) {
        out.update_scratch_bytes = rt_align(std::max<u64>(out.backend.update_scratch, 1), 256);
    }
    return true;
}

void Frontend::prebuild_info(Marshaler& m) {
    // void GetRaytracingAccelerationStructurePrebuildInfo(
    //     const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *pDesc,
    //     D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *pInfo).
    // Only CPU data is read; GPU VAs are not dereferenced.
    const GuestAddr info = m.arg_ptr(2);
    m.set_return(0);
    if (info == 0) return;
    const auto& l = layout::D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO[m_is_64bit];
    u64 result = 0, scratch = 0, update = 0;
    RtInputs inputs;
    const char* why = nullptr;
    bool missing = false;
    RtLayout layout;
    if (!read_rt_inputs(m.arg_ptr(1), inputs, why, missing)) {
        if (missing) {
            unimplemented("ID3D12Device5::GetRaytracingAccelerationStructurePrebuildInfo", why);
        } else {
            d3d12_diag("GetRaytracingAccelerationStructurePrebuildInfo: %s", why);
        }
    } else if (!rt_layout(inputs, layout)) {
        unimplemented("ID3D12Device5::GetRaytracingAccelerationStructurePrebuildInfo",
                      "the backend has no acceleration structures");
    } else {
        result = layout.blob_bytes;
        scratch = layout.scratch_bytes;
        update = layout.update_scratch_bytes;
    }
    (void)m_memory.write_u64(info + l.ResultDataMaxSizeInBytes, result);
    (void)m_memory.write_u64(info + l.ScratchDataSizeInBytes, scratch);
    (void)m_memory.write_u64(info + l.UpdateScratchDataSizeInBytes, update);
}

void Frontend::check_driver_matching_identifier(Marshaler& m) {
    // D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS CheckDriverMatchingIdentifier(
    //     D3D12_SERIALIZED_DATA_TYPE, const
    //     D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER *). A device with
    // raytracing recognises the acceleration-structure type and answers for
    // the identifier (Raytracing.md, CheckDriverMatchingIdentifier).
    const u32 type = m.arg32(1);
    const GuestAddr identifier = m.arg_ptr(2);
    if (type != D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE ||
        m_backend.raytracing() == nullptr) {
        m.set_return(D3D12_DRIVER_MATCHING_IDENTIFIER_UNSUPPORTED_TYPE);
        return;
    }
    u8 bytes[32] = {};
    if (identifier == 0 || !m_memory.read_bytes(identifier, bytes, sizeof(bytes)).is_ok()) {
        m.set_return(D3D12_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED);
        return;
    }
    if (std::memcmp(bytes, kRtDriverMatchingIdentifier, 16) != 0) {
        m.set_return(D3D12_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED);
        return;
    }
    m.set_return(std::memcmp(bytes + 16, kRtDriverMatchingIdentifier + 16, 16) == 0
                     ? D3D12_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE
                     : D3D12_DRIVER_MATCHING_IDENTIFIER_INCOMPATIBLE_VERSION);
}

AccelerationStructure* Frontend::acceleration_structure_at(u64 va) {
    const auto found = m_acceleration_structures.find(va);
    return found != m_acceleration_structures.end() ? &found->second : nullptr;
}

std::vector<graphics::BackendRtMemoryRange> Frontend::rt_memory_ranges() const {
    std::vector<graphics::BackendRtMemoryRange> out;
    for (const auto& [va, memory] : m_va_ranges) {
        if (memory == nullptr || memory->backend == nullptr || memory->backend->buffer() == nullptr) {
            continue;
        }
        out.push_back({va, memory->size, memory->backend->buffer(), 0});
    }
    for (const auto& [va, span] : m_reserved_vas) {
        if (span == nullptr || span->buffer == nullptr) continue;
        out.push_back({va, span->bytes, span->buffer, 0});
    }
    return out;
}

bool Frontend::record_raytracing(Marshaler& m, CommandList& list, usize slot,
                                 const char* name) {
    using S = GraphicsCommandListSlot;
    Recording& rec = *list.recording;
    const std::string api = std::string("ID3D12GraphicsCommandList4::") + name;
    if (slot != S::BuildRaytracingAccelerationStructure &&
        slot != S::EmitRaytracingAccelerationStructurePostbuildInfo &&
        slot != S::CopyRaytracingAccelerationStructure) {
        return false;
    }
    if (m_backend.raytracing() == nullptr) {
        unimplemented(api.c_str(), "the backend has no acceleration structures");
        record_error(list, E_INVALIDARG, name, "raytracing");
        return true;
    }
    const auto read_postbuild = [&](GuestAddr at, u64& dest, u32& type) {
        const auto& l =
            layout::D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC[m_is_64bit];
        dest = read_u64(at + l.DestBuffer);
        type = read_u32(at + l.InfoType);
        if (type > D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE) {
            return false;
        }
        // Natural alignment of the output struct (UINT64 members).
        return dest != 0 && dest % 8 == 0;
    };
    if (slot == S::BuildRaytracingAccelerationStructure) {
        // (const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *pDesc,
        //  UINT NumPostbuildInfoDescs,
        //  const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *).
        const GuestAddr desc = m.arg_ptr(1);
        const u32 postbuild_count = m.arg32(2);
        const GuestAddr postbuild = m.arg_ptr(3);
        if (desc == 0 || (postbuild_count != 0 && postbuild == 0)) {
            record_error(list, E_INVALIDARG, name, "null description");
            return true;
        }
        const auto& l = layout::D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC[m_is_64bit];
        cmd::BuildAccelerationStructure build;
        build.dest = read_u64(desc + l.DestAccelerationStructureData);
        build.source = read_u64(desc + l.SourceAccelerationStructureData);
        build.scratch = read_u64(desc + l.ScratchAccelerationStructureData);
        const char* why = nullptr;
        bool missing = false;
        if (!read_rt_inputs(desc + l.Inputs, build.inputs, why, missing)) {
            if (missing) unimplemented(api.c_str(), why);
            record_error(list, E_INVALIDARG, name, why);
            return true;
        }
        const bool update =
            (build.inputs.flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE) != 0;
        const u64 align = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
        if (build.dest == 0 || build.dest % align != 0 || build.scratch == 0 ||
            build.scratch % align != 0 || (update && (build.source == 0 || build.source % align != 0)) ||
            (!update && build.source != 0)) {
            record_error(list, E_INVALIDARG, name, "misaligned or missing address");
            return true;
        }
        std::vector<u32> seen;
        for (u32 index = 0; index < postbuild_count; ++index) {
            u64 dest = 0;
            u32 type = 0;
            const auto& p =
                layout::D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC[m_is_64bit];
            if (!read_postbuild(postbuild + index * p.size, dest, type) ||
                std::find(seen.begin(), seen.end(), type) != seen.end()) {
                record_error(list, E_INVALIDARG, name, "invalid postbuild info desc");
                return true;
            }
            seen.push_back(type);
            build.postbuild.emplace_back(dest, type);
        }
        rec.commands.emplace_back(std::move(build));
        return true;
    }
    if (slot == S::EmitRaytracingAccelerationStructurePostbuildInfo) {
        // (const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *,
        //  UINT NumSourceAccelerationStructures,
        //  const D3D12_GPU_VIRTUAL_ADDRESS *pSourceAccelerationStructureData).
        const GuestAddr desc = m.arg_ptr(1);
        const u32 count = m.arg32(2);
        const GuestAddr sources = m.arg_ptr(3);
        cmd::EmitAccelerationStructurePostbuild emit{};
        if (desc == 0 || (count != 0 && sources == 0) || !read_postbuild(desc, emit.dest, emit.type)) {
            record_error(list, E_INVALIDARG, name, "invalid postbuild info desc");
            return true;
        }
        for (u32 index = 0; index < count; ++index) {
            const u64 va = read_u64(sources + index * 8u);
            if (va % D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT != 0) {
                record_error(list, E_INVALIDARG, name, "misaligned acceleration structure");
                return true;
            }
            emit.sources.push_back(va);
        }
        rec.commands.emplace_back(std::move(emit));
        return true;
    }
    // CopyRaytracingAccelerationStructure(D3D12_GPU_VIRTUAL_ADDRESS Dest,
    //     D3D12_GPU_VIRTUAL_ADDRESS Source, COPY_MODE Mode).
    usize next = 0;
    const u64 dest = read_arg_u64(m, m_is_64bit, 1, next);
    const u64 source = read_arg_u64(m, m_is_64bit, next, next);
    const u32 mode = m.arg32(next);
    const u64 align = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
    if (mode > D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE || dest == 0 ||
        source == 0 || dest % align != 0 || source % align != 0) {
        record_error(list, E_INVALIDARG, name, "invalid copy");
        return true;
    }
    rec.commands.emplace_back(cmd::CopyAccelerationStructure{dest, source, mode});
    return true;
}

}  // namespace vitreo::winapi::d3d12
