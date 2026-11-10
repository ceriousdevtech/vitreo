// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_replay_raytracing.cpp
/// @brief Replay of the DXR acceleration-structure commands: builds and
/// updates (the blob of d3d12_raytracing.hpp, then the backend structure),
/// the copy modes and postbuild info. Contract: DirectX-Specs
/// Raytracing.md (BuildRaytracingAccelerationStructure,
/// CopyRaytracingAccelerationStructure,
/// EmitRaytracingAccelerationStructurePostbuildInfo).

#include "winapi/graphics/d3d12/d3d12_replay_internal.hpp"

#include <algorithm>

namespace vitreo::winapi::d3d12 {

namespace {

/// The tools-visualization size of @p as (Raytracing.md,
/// D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_TOOLS_VISUALIZATION_HEADER
/// followed by the instance or geometry descs, then the decoded data).
[[nodiscard]] u64 visualization_bytes(const AccelerationStructure& as, u64 geometry_desc_bytes) {
    if (as.top_level) {
        return kRtVisualizationHeaderBytes + static_cast<u64>(as.count) * graphics::kRtInstanceDescBytes;
    }
    u64 at = rt_align(kRtVisualizationHeaderBytes + geometry_desc_bytes * as.geometries.size(), 16);
    for (const auto& g : as.geometries) {
        at += rt_align(static_cast<u64>(g.primitives) *
                           (g.boxes ? graphics::kRtBoxBytes : graphics::kRtTriangleBytes),
                       16);
    }
    return at;
}

[[nodiscard]] u64 serialized_bytes(const AccelerationStructure& as) {
    return kRtSerializedHeaderBytes + (as.top_level ? 8ull * as.count : 0ull) + as.blob_bytes;
}

void put32(std::vector<u8>& out, u64 at, u32 value) { std::memcpy(out.data() + at, &value, 4); }
void put64(std::vector<u8>& out, u64 at, u64 value) { std::memcpy(out.data() + at, &value, 8); }

}  // namespace

bool Replayer::write_va(u64 va, const void* data, usize bytes) {
    const auto* from = static_cast<const u8*>(data);
    for (usize done = 0; done < bytes;) {
        const usize n = std::min<usize>(bytes - done, 64u * 1024u);
        graphics::BackendBuffer* staging = nullptr;
        u64 staging_offset = 0;
        graphics::BackendBuffer* target = nullptr;
        u64 offset = 0;
        if (!upload(from + done, n, staging, staging_offset) ||
            !f.resolve_va(va + done, n, target, offset) ||
            !f.m_backend.copy_buffer_region(target, offset, staging, staging_offset, n)) {
            return false;
        }
        done += n;
    }
    return true;
}

bool Replayer::bind_acceleration_structure(graphics::BackendDescriptorTable& t,
                                           unsigned slot, u64 va) {
    graphics::BackendRaytracing* rt = f.m_backend.raytracing();
    const AccelerationStructure* as = f.acceleration_structure_at(va);
    const graphics::BackendAccelerationStructure* structure =
        as != nullptr && as->top_level && as->backend ? as->backend.get() : nullptr;
    if (structure == nullptr && rt != nullptr) structure = rt->empty_top_level();
    if (structure == nullptr || !t.set_acceleration_structure(slot, structure)) {
        t.clear_slot(slot);
        return false;
    }
    return as != nullptr;
}

void Replayer::dispatch_rays(ReplayState& st, const cmd::DispatchRays& c) {
    // Raytracing.md (DispatchRays): a zero dimension launches nothing; the
    // raytracing pipeline is SetPipelineState1's.
    if (c.width == 0 || c.height == 0 || c.depth == 0) return;
    StateObject* so = f.find_serial<StateObject>(st.state_object, st.state_object_serial, Kind::StateObject);
    if (so == nullptr || !so->kernel) {
        d3d12_diag("DispatchRays without a raytracing pipeline");
        return;
    }
    u32 words[graphics::kRtConstWords] = {};
    words[graphics::kRtConstWidth] = c.width;
    words[graphics::kRtConstHeight] = c.height;
    words[graphics::kRtConstDepth] = c.depth;
    words[graphics::kRtConstMissStride] = static_cast<u32>(c.miss_stride);
    words[graphics::kRtConstHitStride] = static_cast<u32>(c.hit_stride);
    words[graphics::kRtConstCallableStride] = static_cast<u32>(c.callable_stride);
    // A table of stride 0 is one record used for every index.
    const auto records = [](u64 size, u64 stride) -> u32 {
        if (size == 0) return 0;
        return stride == 0 ? 0xFFFFFFFFu : static_cast<u32>(std::min<u64>(size / stride, 0xFFFFFFFFu));
    };
    words[graphics::kRtConstMissCount] = records(c.miss_size, c.miss_stride);
    words[graphics::kRtConstHitCount] = records(c.hit_size, c.hit_stride);
    words[graphics::kRtConstCallableCount] = records(c.callable_size, c.callable_stride);
    RtDispatchBindings bindings;
    bindings.tables = {c.raygen_size != 0 ? c.raygen : 0u, c.miss_size != 0 ? c.miss : 0u,
                       c.hit_size != 0 ? c.hit : 0u, c.callable_size != 0 ? c.callable : 0u};
    // The bound heaps, for local descriptor tables (handle - start) /
    // increment (dxil_raytracing.hpp).
    if (st.heaps[0] != nullptr) words[graphics::kRtConstViewHeap] = static_cast<u32>(st.heaps[0]->gpu_base);
    if (st.heaps[1] != nullptr) words[graphics::kRtConstSamplerHeap] = static_cast<u32>(st.heaps[1]->gpu_base);
    words[graphics::kRtConstViewIncrement] = f.descriptor_increment();
    words[graphics::kRtConstSamplerIncrement] = f.descriptor_increment();
    if (!so->local_views.empty() && !resolve_local_views(*so, c, words, bindings)) return;
    if (!upload(words, sizeof(words), bindings.constants, bindings.constants_offset)) return;
    PipelineState* saved = st.pso;
    st.pso = so->kernel.get();
    rt_dispatch_ = &bindings;
    dispatch(st, cmd::Dispatch{(c.width + 7u) / 8u, (c.height + 7u) / 8u, c.depth});
    rt_dispatch_ = nullptr;
    st.pso = saved;
}

bool Replayer::resolve_local_views(const StateObject& so, const cmd::DispatchRays& c, u32* words,
                                   RtDispatchBindings& b) {
    // Raytracing.md ("Shader record"): a record's local root descriptors
    // are GPU VAs after its identifier, laid out by the local root
    // signature of the export the identifier names. Read the records as
    // the GPU leaves them (everything recorded before this DispatchRays),
    // bind each distinct VA as an element of its view array, and tell the
    // kernel which element each record names.
    const usize views = so.local_views.size();
    struct Table {
        u64 start, size, stride, count;
    };
    const auto count_of = [](u64 size, u64 stride) -> u64 {
        if (size == 0) return 0;
        return stride == 0 ? 1 : size / stride;
    };
    const Table tables[4] = {{c.raygen, c.raygen_size, 0, c.raygen_size != 0 ? 1u : 0u},
                             {c.miss, c.miss_size, c.miss_stride, count_of(c.miss_size, c.miss_stride)},
                             {c.hit, c.hit_size, c.hit_stride, count_of(c.hit_size, c.hit_stride)},
                             {c.callable, c.callable_size, c.callable_stride,
                              count_of(c.callable_size, c.callable_stride)}};
    static constexpr u32 kBaseWords[4] = {graphics::kRtConstRecordBaseRaygen, graphics::kRtConstRecordBaseMiss,
                                          graphics::kRtConstRecordBaseHit, graphics::kRtConstRecordBaseCallable};
    u64 records = 0;
    for (u32 t = 0; t < 4; ++t) {
        words[kBaseWords[t]] = static_cast<u32>(records);
        records += tables[t].count;
    }
    if (records * views > 0x3FFFFFFFull) {
        unimplemented("ID3D12GraphicsCommandList4::DispatchRays", "local root descriptors in more than 2^30 records");
        return false;
    }
    std::vector<u32> translation(std::max<usize>(static_cast<usize>(records * views), 1u), 0u);
    b.local_views.assign(views, std::vector<u64>(1, 0u));
    std::vector<std::map<u64, u32>> elements(views);
    for (u32 t = 0; t < 4; ++t) {
        const Table& table = tables[t];
        if (table.count == 0) continue;
        // One record of a stride-0 table is the whole range (at most the
        // 4096-byte maximum stride).
        const u64 record_bytes = table.stride != 0 ? table.stride : std::min<u64>(table.size, 4096u);
        const u64 bytes = (table.count - 1) * table.stride + record_bytes;
        std::vector<u8> data(static_cast<usize>(bytes));
        if (!read_va(table.start, data.data(), data.size())) {
            d3d12_diag("DispatchRays: the shader records could not be read");
            return false;
        }
        for (u64 r = 0; r < table.count; ++r) {
            const u8* record = data.data() + r * table.stride;
            u32 magic = 0, index = 0;
            std::memcpy(&magic, record, 4);
            std::memcpy(&index, record + 8, 4);
            if (magic != kRtIdentifierMagic) continue;  // a NULL or foreign identifier runs nothing
            const auto found = so.record_views.find(index);
            if (found == so.record_views.end()) continue;
            for (const auto& [k, offset] : found->second) {
                if (32u + offset + 8u > record_bytes || k >= views) continue;
                u64 va = 0;
                std::memcpy(&va, record + 32 + offset, 8);
                u32 element = 0;
                if (va != 0) {
                    auto [it, fresh] = elements[k].emplace(va, static_cast<u32>(b.local_views[k].size()));
                    if (fresh) {
                        if (b.local_views[k].size() >= graphics::kRtLocalViewSlots) {
                            unimplemented("ID3D12GraphicsCommandList4::DispatchRays",
                                          "more than 4095 distinct local root descriptor VAs of one register");
                            return false;
                        }
                        b.local_views[k].push_back(va);
                    }
                    element = it->second;
                }
                translation[static_cast<usize>((words[kBaseWords[t]] + r) * views + k)] = element;
            }
        }
    }
    b.translation_words = static_cast<u32>(translation.size());
    return upload(translation.data(), translation.size() * 4u, b.translation, b.translation_offset);
}

bool Replayer::copy_va(u64 dst, u64 src, u64 bytes) {
    if (bytes == 0) return true;
    graphics::BackendBuffer* to = nullptr;
    graphics::BackendBuffer* from = nullptr;
    u64 to_offset = 0, from_offset = 0;
    return f.resolve_va(dst, bytes, to, to_offset) && f.resolve_va(src, bytes, from, from_offset) &&
           f.m_backend.copy_buffer_region(to, to_offset, from, from_offset, bytes);
}

bool Replayer::read_va(u64 va, void* out, usize bytes) {
    graphics::BackendBuffer* source = nullptr;
    u64 offset = 0;
    if (bytes == 0 || !f.resolve_va(va, bytes, source, offset)) return false;
    graphics::BackendBuffer* staging = nullptr;
    u64 staging_offset = 0;
    u8* host = nullptr;
    std::unique_ptr<graphics::BackendHeap> large;
    if (bytes <= CONSTANT_CHUNK_BYTES / 4) {
        if (!reserve(bytes, staging, staging_offset, host)) return false;
    } else {
        graphics::BackendHeapDesc desc{};
        desc.size = (bytes + 0xFFFFu) & ~u64{0xFFFFu};
        desc.cpu_visible = true;
        desc.buffers_only = true;
        large = f.m_backend.create_heap(desc);
        if (!large || large->buffer() == nullptr || large->host_pointer() == nullptr) return false;
        staging = large->buffer();
        host = static_cast<u8*>(large->host_pointer());
    }
    if (!f.m_backend.copy_buffer_region(staging, staging_offset, source, offset, bytes)) {
        return false;
    }
    graphics::frame_profile::WaitScope profile_wait("replay:raytracing-readback");
    if (auto completion = f.m_backend.flush_completion()) {
        if (!completion->wait()) return false;
    } else if (!f.m_backend.wait_idle()) {
        return false;
    }
    std::memcpy(out, host, bytes);
    return true;
}

graphics::BackendBuffer* Replayer::transient_buffer(u64 bytes) {
    graphics::BackendHeapDesc desc{};
    desc.size = rt_align(std::max<u64>(bytes, 256), 64u * 1024u);
    desc.cpu_visible = false;
    desc.buffers_only = true;
    ReplayResources::Transient transient;
    transient.heap = f.m_backend.create_heap(desc);
    if (!transient.heap || transient.heap->buffer() == nullptr) return nullptr;
    transient.generation = f.m_backend.recording_generation();
    graphics::BackendBuffer* buffer = transient.heap->buffer();
    rr.transient_heaps.push_back(std::move(transient));
    return buffer;
}

AccelerationStructure* Replayer::place_structure(u64 va, bool top_level, u64 backend_bytes,
                                                 const graphics::BackendAccelerationStructure* keep_off) {
    graphics::BackendRaytracing* rt = f.m_backend.raytracing();
    AccelerationStructure& record = f.m_acceleration_structures[va];
    if (record.backend && record.backend->top_level() == top_level &&
        record.backend->size() >= backend_bytes && record.backend.get() != keep_off) {
        return &record;
    }
    record.backend = rt->create(backend_bytes, top_level);
    ++f.m_rt_generation;
    if (!record.backend) {
        f.m_acceleration_structures.erase(va);
        return nullptr;
    }
    return &record;
}

bool Replayer::build_bottom_geometry(const RtInputs::Geometry& g, graphics::BackendBuffer* blob,
                                     u64 at, graphics::BackendRtGeometry& out) {
    graphics::BackendRaytracing* rt = f.m_backend.raytracing();
    out = {};
    out.boxes = g.boxes();
    out.opaque = (g.flags & D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE) != 0;
    out.no_duplicate_any_hit =
        (g.flags & D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION) != 0;
    out.primitive_count = g.primitives();
    out.data = blob;
    out.offset = at;
    if (out.primitive_count == 0) return true;
    if (out.boxes) {
        graphics::BackendRtBoxSource src;
        src.stride = g.aabb_stride;
        src.count = out.primitive_count;
        const u64 bytes = (src.count - 1ull) * src.stride + graphics::kRtBoxBytes;
        graphics::BackendBuffer* boxes = nullptr;
        if (!f.resolve_va(g.aabbs, bytes, boxes, src.offset)) return false;
        src.boxes = boxes;
        return rt->decode_boxes(src, blob, at);
    }
    graphics::BackendRtTriangleSource src;
    graphics::BackendRtVertexFormat format{};
    switch (g.vertex_format) {
    case DXGI_FORMAT_R32G32B32_FLOAT: format = graphics::BackendRtVertexFormat::Float3; break;
    case DXGI_FORMAT_R32G32_FLOAT: format = graphics::BackendRtVertexFormat::Float2; break;
    case DXGI_FORMAT_R16G16_FLOAT: format = graphics::BackendRtVertexFormat::Half2; break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: format = graphics::BackendRtVertexFormat::Half4; break;
    case DXGI_FORMAT_R16G16_SNORM: format = graphics::BackendRtVertexFormat::Snorm16x2; break;
    case DXGI_FORMAT_R16G16B16A16_SNORM: format = graphics::BackendRtVertexFormat::Snorm16x4; break;
    case DXGI_FORMAT_R16G16_UNORM: format = graphics::BackendRtVertexFormat::Unorm16x2; break;
    case DXGI_FORMAT_R16G16B16A16_UNORM: format = graphics::BackendRtVertexFormat::Unorm16x4; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM: format = graphics::BackendRtVertexFormat::Unorm10x3; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM: format = graphics::BackendRtVertexFormat::Unorm8x4; break;
    case DXGI_FORMAT_R8G8_UNORM: format = graphics::BackendRtVertexFormat::Unorm8x2; break;
    case DXGI_FORMAT_R8G8B8A8_SNORM: format = graphics::BackendRtVertexFormat::Snorm8x4; break;
    default: format = graphics::BackendRtVertexFormat::Snorm8x2; break;
    }
    src.vertex_format = format;
    src.vertex_stride = g.vertex_stride;
    src.triangle_count = out.primitive_count;
    static constexpr u32 kVertexBytes[] = {12, 8, 4, 8, 4, 8, 4, 8, 4, 4, 2, 4, 2};
    const u64 vertices = g.vertex_count != 0 ? g.vertex_count : 1u;
    const u64 vertex_bytes =
        (vertices - 1ull) * src.vertex_stride + kVertexBytes[static_cast<u32>(format)];
    graphics::BackendBuffer* vb = nullptr;
    if (!f.resolve_va(g.vertex_buffer, vertex_bytes, vb, src.vertex_offset)) return false;
    src.vertices = vb;
    if (g.index_buffer != 0) {
        graphics::BackendBuffer* ib = nullptr;
        src.index_size = g.index_format == DXGI_FORMAT_R32_UINT ? 4u : 2u;
        if (!f.resolve_va(g.index_buffer, static_cast<u64>(g.index_count) * src.index_size, ib,
                          src.index_offset)) {
            return false;
        }
        src.indices = ib;
    }
    if (g.transform != 0) {
        graphics::BackendBuffer* tb = nullptr;
        if (!f.resolve_va(g.transform, 48, tb, src.transform_offset)) return false;
        src.transform = tb;
    }
    return rt->decode_triangles(src, blob, at);
}

void Replayer::build_acceleration_structure(const cmd::BuildAccelerationStructure& c) {
    graphics::BackendRaytracing* rt = f.m_backend.raytracing();
    const RtInputs& in = c.inputs;
    RtLayout layout;
    if (rt == nullptr || !f.rt_layout(in, layout)) {
        d3d12_diag("BuildRaytracingAccelerationStructure: the backend cannot size it");
        return;
    }
    const bool top = in.type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    const bool update = (in.flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE) != 0;
    graphics::BackendBuffer* blob = nullptr;
    u64 blob_offset = 0;
    graphics::BackendBuffer* scratch = nullptr;
    u64 scratch_offset = 0;
    if (!f.resolve_va(c.dest, layout.blob_bytes, blob, blob_offset) ||
        !f.resolve_va(c.scratch, update ? layout.update_scratch_bytes : layout.scratch_bytes,
                      scratch, scratch_offset)) {
        d3d12_diag("BuildRaytracingAccelerationStructure: destination or scratch is not "
                   "mapped for the sizes GetRaytracingAccelerationStructurePrebuildInfo reports");
        return;
    }
    std::shared_ptr<graphics::BackendAccelerationStructure> source;
    u32 flags = in.flags & 0x1Fu;
    if (update) {
        AccelerationStructure* from = f.acceleration_structure_at(c.source);
        // Raytracing.md "Acceleration structure update constraints": the same
        // type and counts as the source, which allowed updates.
        if (from == nullptr || !from->backend || from->top_level != top ||
            (from->flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE) == 0 ||
            from->count != in.count) {
            d3d12_diag("BuildRaytracingAccelerationStructure: PERFORM_UPDATE without a matching "
                       "source built with ALLOW_UPDATE");
            return;
        }
        source = from->backend;
        flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    }

    // The blob's header and, for a bottom level, its geometry records.
    const u64 head_bytes =
        top ? kRtBlobHeaderBytes : rt_align(kRtBlobHeaderBytes + kRtBlobGeometryRecordBytes * in.count, 16);
    std::vector<u8> head(head_bytes, 0);
    put32(head, 0, kRtBlobMagic);
    put32(head, 4, kRtBlobVersion);
    put32(head, 8, in.type);
    put32(head, 12, flags);
    put32(head, 16, in.count);
    put64(head, 24, layout.blob_bytes);
    for (u32 k = 0; !top && k < in.count; ++k) {
        const u64 at = kRtBlobHeaderBytes + kRtBlobGeometryRecordBytes * k;
        put32(head, at, in.geometries[k].type);
        put32(head, at + 4, in.geometries[k].flags);
        put32(head, at + 8, in.geometries[k].primitives());
    }
    if (!write_va(c.dest, head.data(), head.size())) {
        d3d12_diag("BuildRaytracingAccelerationStructure: header write refused");
        return;
    }

    graphics::BackendRtBuild build;
    build.top_level = top;
    build.flags = flags;
    if (top) {
        graphics::BackendRtInstanceSource src;
        src.count = in.count;
        src.pointers = in.layout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS;
        if (in.count != 0) {
            graphics::BackendBuffer* descs = nullptr;
            const u64 bytes = static_cast<u64>(in.count) * (src.pointers ? 8u : graphics::kRtInstanceDescBytes);
            if (!f.resolve_va(in.instance_descs, bytes, descs, src.offset)) {
                d3d12_diag("BuildRaytracingAccelerationStructure: InstanceDescs not mapped");
                return;
            }
            src.buffer = descs;
            if (src.pointers) src.memory = f.rt_memory_ranges();
            if (!rt->gather_instances(src, blob, blob_offset + kRtBlobHeaderBytes)) {
                d3d12_diag("BuildRaytracingAccelerationStructure: gather refused");
                return;
            }
        }
        std::vector<graphics::BackendRtBottomLevel> levels;
        for (const auto& [va, as] : f.m_acceleration_structures) {
            if (!as.top_level && as.backend) levels.push_back({va, as.backend.get()});
        }
        graphics::BackendBuffer* from = blob;
        u64 from_offset = blob_offset + kRtBlobHeaderBytes;
        if (in.count == 0) {
            // An empty top level: one inactive record (NULL bottom level).
            const u8 zeros[8 + graphics::kRtInstanceDescBytes] = {};
            if (!upload(zeros, sizeof(zeros), from, from_offset)) return;
        }
        build.instance_count = std::max(in.count, 1u);
        if (!rt->convert_instances(from, from_offset, build.instance_count, levels, scratch,
                                   scratch_offset)) {
            d3d12_diag("BuildRaytracingAccelerationStructure: instance conversion refused");
            return;
        }
        build.instances = scratch;
        build.instances_offset = scratch_offset;
    } else {
        for (u32 k = 0; k < in.count; ++k) {
            graphics::BackendRtGeometry geometry;
            if (!build_bottom_geometry(in.geometries[k], blob, blob_offset + layout.geometry_offsets[k],
                                       geometry)) {
                d3d12_diag("BuildRaytracingAccelerationStructure: geometry %u inputs not mapped", k);
                return;
            }
            build.geometries.push_back(geometry);
        }
        if (build.geometries.empty()) {
            // An empty bottom level: one degenerate triangle, the header's
            // zero bytes at 64 (no intersections).
            graphics::BackendRtGeometry none;
            none.primitive_count = 1;
            none.opaque = true;
            none.data = blob;
            none.offset = blob_offset + 64;
            build.geometries.push_back(none);
        }
    }

    AccelerationStructure* record = nullptr;
    if (update && c.source == c.dest) {
        record = f.acceleration_structure_at(c.dest);
    } else {
        record = place_structure(c.dest, top, layout.backend.structure, source.get());
    }
    if (record == nullptr || !record->backend) {
        unimplemented("ID3D12GraphicsCommandList4::BuildRaytracingAccelerationStructure",
                      "the backend could not allocate the structure");
        return;
    }
    if (!rt->build(*record->backend, build, scratch, scratch_offset + layout.backend_scratch_offset,
                   source.get())) {
        d3d12_diag("BuildRaytracingAccelerationStructure: the backend refused the build");
        return;
    }
    record->top_level = top;
    record->flags = flags;
    record->blob_bytes = layout.blob_bytes;
    record->count = in.count;
    record->geometries.clear();
    for (u32 k = 0; !top && k < in.count; ++k) {
        record->geometries.push_back({in.geometries[k].boxes(), in.geometries[k].flags,
                                      in.geometries[k].primitives(), layout.geometry_offsets[k]});
    }
    for (const auto& [dest, type] : c.postbuild) emit_postbuild(dest, type, {c.dest});
}

void Replayer::emit_postbuild(u64 dest, u32 type, const std::vector<u64>& sources) {
    const u64 desc_bytes = layout::D3D12_RAYTRACING_GEOMETRY_DESC[1].size;
    const u64 stride = type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION ? 16u : 8u;
    std::vector<u8> out(sources.size() * stride, 0);
    for (usize i = 0; i < sources.size(); ++i) {
        const AccelerationStructure* as = f.acceleration_structure_at(sources[i]);
        if (as == nullptr) {
            d3d12_diag("EmitRaytracingAccelerationStructurePostbuildInfo: 0x%llx is no "
                       "acceleration structure", static_cast<unsigned long long>(sources[i]));
            continue;
        }
        switch (type) {
        case D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE:
        case D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE:
            put64(out, i * stride, as->blob_bytes);
            break;
        case D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TOOLS_VISUALIZATION:
            put64(out, i * stride, visualization_bytes(*as, desc_bytes));
            break;
        default:
            put64(out, i * stride, serialized_bytes(*as));
            put64(out, i * stride + 8, as->top_level ? as->count : 0u);
            break;
        }
    }
    if (!write_va(dest, out.data(), out.size())) {
        d3d12_diag("EmitRaytracingAccelerationStructurePostbuildInfo: DestBuffer is not mapped");
    }
}

void Replayer::copy_acceleration_structure(const cmd::CopyAccelerationStructure& c) {
    graphics::BackendRaytracing* rt = f.m_backend.raytracing();
    if (rt == nullptr) return;
    if (c.mode == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE) {
        deserialize_acceleration_structure(c);
        return;
    }
    const AccelerationStructure* found = f.acceleration_structure_at(c.source);
    if (found == nullptr || !found->backend) {
        d3d12_diag("CopyRaytracingAccelerationStructure: 0x%llx is no acceleration structure",
                   static_cast<unsigned long long>(c.source));
        return;
    }
    const AccelerationStructure src = *found;
    switch (c.mode) {
    case D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE:
    case D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT: {
        // COMPACT: the blob is already as small as it gets (CURRENT_SIZE ==
        // COMPACTED_SIZE), so both modes copy it whole.
        if (!copy_va(c.dest, c.source, src.blob_bytes)) {
            d3d12_diag("CopyRaytracingAccelerationStructure: destination not mapped");
            return;
        }
        AccelerationStructure* to =
            place_structure(c.dest, src.top_level, src.backend->size(), src.backend.get());
        if (to == nullptr || !rt->copy(*to->backend, *src.backend)) {
            d3d12_diag("CopyRaytracingAccelerationStructure: the backend refused the copy");
            return;
        }
        auto backend = to->backend;
        *to = src;
        to->backend = std::move(backend);
        return;
    }
    case D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE: {
        std::vector<u8> head(kRtSerializedHeaderBytes, 0);
        std::memcpy(head.data(), kRtDriverMatchingIdentifier, sizeof(kRtDriverMatchingIdentifier));
        put64(head, 32, serialized_bytes(src));
        put64(head, 40, src.blob_bytes);
        // HEADER1: NumBottomLevelAccelerationStructurePointersAfterHeader,
        // then HeaderPostambleType BOTTOM_LEVEL_POINTERS (0).
        put32(head, 48, src.top_level ? src.count : 0u);
        const u64 pointers = src.top_level ? 8ull * src.count : 0ull;
        if (!write_va(c.dest, head.data(), head.size()) ||
            !copy_va(c.dest + kRtSerializedHeaderBytes, c.source + kRtBlobHeaderBytes, pointers) ||
            !copy_va(c.dest + kRtSerializedHeaderBytes + pointers, c.source, src.blob_bytes)) {
            d3d12_diag("CopyRaytracingAccelerationStructure: serialize destination not mapped");
        }
        return;
    }
    default: {
        // VISUALIZATION_DECODE_FOR_TOOLS: the build inputs back, transforms
        // folded and positions float32 (Raytracing.md "Tools support").
        const u64 desc_bytes = layout::D3D12_RAYTRACING_GEOMETRY_DESC[1].size;
        std::vector<u8> head;
        if (src.top_level) {
            head.assign(kRtVisualizationHeaderBytes, 0);
            put32(head, 0, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL);
            put32(head, 4, src.count);
            if (!write_va(c.dest, head.data(), head.size()) ||
                !copy_va(c.dest + kRtVisualizationHeaderBytes,
                         c.source + kRtBlobHeaderBytes + 8ull * src.count,
                         graphics::kRtInstanceDescBytes * src.count)) {
                d3d12_diag("CopyRaytracingAccelerationStructure: visualization not mapped");
            }
            return;
        }
        const auto& g = layout::D3D12_RAYTRACING_GEOMETRY_DESC[1];
        u64 data = rt_align(kRtVisualizationHeaderBytes + desc_bytes * src.geometries.size(), 16);
        head.assign(data, 0);
        put32(head, 0, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL);
        put32(head, 4, static_cast<u32>(src.geometries.size()));
        std::vector<std::pair<u64, u64>> copies;  // (dest offset, blob offset)
        for (usize k = 0; k < src.geometries.size(); ++k) {
            const auto& geometry = src.geometries[k];
            const u64 at = kRtVisualizationHeaderBytes + desc_bytes * k;
            const u64 bytes = static_cast<u64>(geometry.primitives) *
                              (geometry.boxes ? graphics::kRtBoxBytes : graphics::kRtTriangleBytes);
            put32(head, at + g.Type,
                  geometry.boxes ? D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS
                                 : D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES);
            put32(head, at + g.Flags, geometry.flags);
            if (geometry.boxes) {
                put64(head, at + g.AABBs_AABBCount, geometry.primitives);
                put64(head, at + g.AABBs_AABBs_StartAddress, c.dest + data);
                put64(head, at + g.AABBs_AABBs_StrideInBytes, graphics::kRtBoxBytes);
            } else {
                put32(head, at + g.Triangles_VertexFormat, DXGI_FORMAT_R32G32B32_FLOAT);
                put32(head, at + g.Triangles_VertexCount, geometry.primitives * 3u);
                put64(head, at + g.Triangles_VertexBuffer_StartAddress, c.dest + data);
                put64(head, at + g.Triangles_VertexBuffer_StrideInBytes, 12);
            }
            copies.emplace_back(data, geometry.offset);
            data += rt_align(bytes, 16);
        }
        bool ok = write_va(c.dest, head.data(), head.size());
        for (usize k = 0; ok && k < copies.size(); ++k) {
            const auto& geometry = src.geometries[k];
            ok = copy_va(c.dest + copies[k].first, c.source + copies[k].second,
                         static_cast<u64>(geometry.primitives) *
                             (geometry.boxes ? graphics::kRtBoxBytes : graphics::kRtTriangleBytes));
        }
        if (!ok) d3d12_diag("CopyRaytracingAccelerationStructure: visualization not mapped");
        return;
    }
    }
}

void Replayer::deserialize_acceleration_structure(const cmd::CopyAccelerationStructure& c) {
    graphics::BackendRaytracing* rt = f.m_backend.raytracing();
    // The serialized header (and the pointers the app patched), as the GPU
    // leaves them: the source is data the app wrote, so this reads it back.
    u8 head[kRtSerializedHeaderBytes] = {};
    if (!read_va(c.source, head, sizeof(head)) ||
        std::memcmp(head, kRtDriverMatchingIdentifier, sizeof(kRtDriverMatchingIdentifier)) != 0) {
        d3d12_diag("CopyRaytracingAccelerationStructure DESERIALIZE: not a Vitreo serialized "
                   "acceleration structure (CheckDriverMatchingIdentifier)");
        return;
    }
    u32 pointer_count = 0;
    std::memcpy(&pointer_count, head + 48, 4);
    const u64 blob_va = c.source + kRtSerializedHeaderBytes + 8ull * pointer_count;
    u8 blob_head[32] = {};
    if (!read_va(blob_va, blob_head, sizeof(blob_head))) return;
    u32 magic = 0, type = 0, flags = 0, count = 0;
    u64 blob_bytes = 0;
    std::memcpy(&magic, blob_head, 4);
    std::memcpy(&type, blob_head + 8, 4);
    std::memcpy(&flags, blob_head + 12, 4);
    std::memcpy(&count, blob_head + 16, 4);
    std::memcpy(&blob_bytes, blob_head + 24, 8);
    const bool top = type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    if (magic != kRtBlobMagic || (top && count != pointer_count)) {
        d3d12_diag("CopyRaytracingAccelerationStructure DESERIALIZE: corrupt blob");
        return;
    }
    RtInputs inputs;
    inputs.type = type;
    inputs.flags = flags;
    inputs.count = count;
    if (!top) {
        std::vector<u8> table(kRtBlobGeometryRecordBytes * count);
        if (count != 0 && !read_va(blob_va + kRtBlobHeaderBytes, table.data(), table.size())) return;
        inputs.geometries.resize(count);
        for (u32 k = 0; k < count; ++k) {
            u32 record[3] = {};
            std::memcpy(record, table.data() + kRtBlobGeometryRecordBytes * k, 12);
            inputs.geometries[k].type = record[0];
            inputs.geometries[k].flags = record[1];
            if (inputs.geometries[k].boxes()) inputs.geometries[k].aabb_count = record[2];
            else inputs.geometries[k].vertex_count = record[2] * 3u;
        }
    }
    RtLayout layout;
    if (!f.rt_layout(inputs, layout) || layout.blob_bytes != blob_bytes ||
        !copy_va(c.dest, blob_va, blob_bytes) ||
        (top && !copy_va(c.dest + kRtBlobHeaderBytes, c.source + kRtSerializedHeaderBytes,
                         8ull * count))) {
        d3d12_diag("CopyRaytracingAccelerationStructure DESERIALIZE: destination not mapped");
        return;
    }
    graphics::BackendBuffer* blob = nullptr;
    u64 blob_offset = 0;
    graphics::BackendBuffer* scratch = transient_buffer(layout.scratch_bytes);
    if (scratch == nullptr || !f.resolve_va(c.dest, blob_bytes, blob, blob_offset)) return;
    graphics::BackendRtBuild build;
    build.top_level = top;
    build.flags = flags;
    if (top) {
        std::vector<graphics::BackendRtBottomLevel> levels;
        for (const auto& [va, as] : f.m_acceleration_structures) {
            if (!as.top_level && as.backend) levels.push_back({va, as.backend.get()});
        }
        graphics::BackendBuffer* from = blob;
        u64 from_offset = blob_offset + kRtBlobHeaderBytes;
        if (count == 0) {
            const u8 zeros[8 + graphics::kRtInstanceDescBytes] = {};
            if (!upload(zeros, sizeof(zeros), from, from_offset)) return;
        }
        build.instance_count = std::max(count, 1u);
        if (!rt->convert_instances(from, from_offset, build.instance_count, levels, scratch, 0)) return;
        build.instances = scratch;
        build.instances_offset = 0;
    } else {
        for (u32 k = 0; k < count; ++k) {
            graphics::BackendRtGeometry geometry;
            geometry.boxes = inputs.geometries[k].boxes();
            geometry.opaque = (inputs.geometries[k].flags & D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE) != 0;
            geometry.no_duplicate_any_hit = (inputs.geometries[k].flags &
                                             D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION) != 0;
            geometry.primitive_count = inputs.geometries[k].primitives();
            geometry.data = blob;
            geometry.offset = blob_offset + layout.geometry_offsets[k];
            build.geometries.push_back(geometry);
        }
        if (build.geometries.empty()) {
            graphics::BackendRtGeometry none;
            none.primitive_count = 1;
            none.opaque = true;
            none.data = blob;
            none.offset = blob_offset + 64;
            build.geometries.push_back(none);
        }
    }
    AccelerationStructure* record = place_structure(c.dest, top, layout.backend.structure, nullptr);
    if (record == nullptr ||
        !rt->build(*record->backend, build, scratch, layout.backend_scratch_offset, nullptr)) {
        d3d12_diag("CopyRaytracingAccelerationStructure DESERIALIZE: the backend refused the build");
        return;
    }
    record->top_level = top;
    record->flags = flags;
    record->blob_bytes = blob_bytes;
    record->count = count;
    record->geometries.clear();
    for (u32 k = 0; !top && k < count; ++k) {
        record->geometries.push_back({inputs.geometries[k].boxes(), inputs.geometries[k].flags,
                                      inputs.geometries[k].primitives(), layout.geometry_offsets[k]});
    }
}

}  // namespace vitreo::winapi::d3d12
