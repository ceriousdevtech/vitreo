// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_raytracing.mm
/// @brief BackendRaytracing on Metal (metal_raytracing.hpp).

#import <Metal/Metal.h>

#include "graphics/metal/metal_raytracing.hpp"
#include "graphics/metal/metal_backend_buffer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace vitreo::graphics {

namespace {

// The passes' programs. Every guest buffer is bound at a 16-byte-aligned
// offset and addressed in bytes from there (`base`), so a vertex or index
// at any offset D3D allows (2-byte aligned for 16-bit formats) is read
// with byte loads that need no alignment.
constexpr const char* kRtKernels = R"MSL(
#include <metal_stdlib>
using namespace metal;

static uint ld_u8(device const uchar* p, ulong at) { return p[at]; }
static uint ld_u16(device const uchar* p, ulong at) {
    return uint(p[at]) | (uint(p[at + 1]) << 8);
}
static uint ld_u32(device const uchar* p, ulong at) {
    return uint(p[at]) | (uint(p[at + 1]) << 8) | (uint(p[at + 2]) << 16) |
           (uint(p[at + 3]) << 24);
}
static float ld_f32(device const uchar* p, ulong at) { return as_type<float>(ld_u32(p, at)); }
static float ld_f16(device const uchar* p, ulong at) {
    return float(as_type<half>(ushort(ld_u16(p, at))));
}
static float snorm(int v, float m) { return max(float(v) / m, -1.0f); }

struct TriangleParams {
    ulong vertex_base;
    ulong vertex_stride;
    ulong index_base;
    ulong transform_base;
    ulong dst_base;
    uint vertex_format;
    uint index_size;
    uint has_transform;
    uint count;
};

static float3 vertex_at(device const uchar* vb, constant TriangleParams& p, uint index) {
    const ulong at = p.vertex_base + ulong(index) * p.vertex_stride;
    switch (p.vertex_format) {
    case 0: return float3(ld_f32(vb, at), ld_f32(vb, at + 4), ld_f32(vb, at + 8));
    case 1: return float3(ld_f32(vb, at), ld_f32(vb, at + 4), 0.0f);
    case 2: return float3(ld_f16(vb, at), ld_f16(vb, at + 2), 0.0f);
    case 3: return float3(ld_f16(vb, at), ld_f16(vb, at + 2), ld_f16(vb, at + 4));
    case 4: return float3(snorm(short(ld_u16(vb, at)), 32767.0f),
                          snorm(short(ld_u16(vb, at + 2)), 32767.0f), 0.0f);
    case 5: return float3(snorm(short(ld_u16(vb, at)), 32767.0f),
                          snorm(short(ld_u16(vb, at + 2)), 32767.0f),
                          snorm(short(ld_u16(vb, at + 4)), 32767.0f));
    case 6: return float3(float(ld_u16(vb, at)) / 65535.0f,
                          float(ld_u16(vb, at + 2)) / 65535.0f, 0.0f);
    case 7: return float3(float(ld_u16(vb, at)) / 65535.0f,
                          float(ld_u16(vb, at + 2)) / 65535.0f,
                          float(ld_u16(vb, at + 4)) / 65535.0f);
    case 8: {
        const uint w = ld_u32(vb, at);
        return float3(float(w & 1023u) / 1023.0f, float((w >> 10) & 1023u) / 1023.0f,
                      float((w >> 20) & 1023u) / 1023.0f);
    }
    case 9: return float3(float(ld_u8(vb, at)) / 255.0f, float(ld_u8(vb, at + 1)) / 255.0f,
                          float(ld_u8(vb, at + 2)) / 255.0f);
    case 10: return float3(float(ld_u8(vb, at)) / 255.0f, float(ld_u8(vb, at + 1)) / 255.0f,
                           0.0f);
    case 11: return float3(snorm(char(ld_u8(vb, at)), 127.0f),
                           snorm(char(ld_u8(vb, at + 1)), 127.0f),
                           snorm(char(ld_u8(vb, at + 2)), 127.0f));
    default: return float3(snorm(char(ld_u8(vb, at)), 127.0f),
                           snorm(char(ld_u8(vb, at + 1)), 127.0f), 0.0f);
    }
}

kernel void vitreo_rt_decode_triangles(device const uchar* vb [[buffer(0)]],
                                       device const uchar* ib [[buffer(1)]],
                                       device const uchar* tb [[buffer(2)]],
                                       device uchar* dst [[buffer(3)]],
                                       constant TriangleParams& p [[buffer(4)]],
                                       uint tid [[thread_position_in_grid]]) {
    if (tid >= p.count) return;
    float m[12];
    if (p.has_transform != 0) {
        for (uint k = 0; k < 12; ++k) m[k] = ld_f32(tb, p.transform_base + k * 4);
    }
    device float* out = (device float*)(dst + p.dst_base + ulong(tid) * 36);
    for (uint corner = 0; corner < 3; ++corner) {
        const uint linear = tid * 3 + corner;
        uint index = linear;
        if (p.index_size == 2) index = ld_u16(ib, p.index_base + ulong(linear) * 2);
        else if (p.index_size == 4) index = ld_u32(ib, p.index_base + ulong(linear) * 4);
        float3 v = vertex_at(vb, p, index);
        if (p.has_transform != 0) {
            v = float3(m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3],
                       m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7],
                       m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11]);
        }
        out[corner * 3 + 0] = v.x;
        out[corner * 3 + 1] = v.y;
        out[corner * 3 + 2] = v.z;
    }
}

struct BoxParams {
    ulong src_base;
    ulong stride;
    ulong dst_base;
    uint count;
};

kernel void vitreo_rt_decode_boxes(device const uchar* src [[buffer(0)]],
                                   device uchar* dst [[buffer(3)]],
                                   constant BoxParams& p [[buffer(4)]],
                                   uint tid [[thread_position_in_grid]]) {
    if (tid >= p.count) return;
    const ulong at = p.src_base + ulong(tid) * p.stride;
    float b[6];
    for (uint k = 0; k < 6; ++k) b[k] = ld_f32(src, at + k * 4);
    // DXR "Degenerate primitives": inverted bounds on any axis make the box
    // the point at the centre of its input bounds. An inactive box (MinX NaN)
    // compares false and keeps its NaN.
    if (b[0] > b[3] || b[1] > b[4] || b[2] > b[5]) {
        for (uint k = 0; k < 3; ++k) {
            const float c = (b[k] + b[k + 3]) * 0.5f;
            b[k] = c;
            b[k + 3] = c;
        }
    }
    device float* out = (device float*)(dst + p.dst_base + ulong(tid) * 24);
    for (uint k = 0; k < 6; ++k) out[k] = b[k];
}

struct MemoryRange {
    ulong va;
    ulong size;
    device const uchar* base;
};

struct GatherParams {
    ulong src_base;
    ulong dst_base;
    uint count;
    uint pointers;
    uint ranges;
};

kernel void vitreo_rt_gather_instances(device const uchar* src [[buffer(0)]],
                                       device const MemoryRange* ranges [[buffer(1)]],
                                       device uchar* dst [[buffer(3)]],
                                       constant GatherParams& p [[buffer(4)]],
                                       uint tid [[thread_position_in_grid]]) {
    if (tid >= p.count) return;
    device const uchar* from = src;
    ulong at = p.src_base + ulong(tid) * 64;
    bool found = true;
    if (p.pointers != 0) {
        const ulong va = ulong(ld_u32(src, p.src_base + ulong(tid) * 8)) |
                         (ulong(ld_u32(src, p.src_base + ulong(tid) * 8 + 4)) << 32);
        found = false;
        uint lo = 0, hi = p.ranges;
        while (lo < hi) {
            const uint mid = (lo + hi) / 2;
            if (ranges[mid].va + ranges[mid].size <= va) lo = mid + 1;
            else hi = mid;
        }
        if (lo < p.ranges && ranges[lo].va <= va && va + 64 <= ranges[lo].va + ranges[lo].size) {
            from = ranges[lo].base;
            at = va - ranges[lo].va;
            found = true;
        }
    }
    device uint* desc = (device uint*)(dst + p.dst_base + ulong(p.count) * 8 + ulong(tid) * 64);
    for (uint k = 0; k < 16; ++k) desc[k] = found ? ld_u32(from, at + k * 4) : 0u;
    device uint* va_out = (device uint*)(dst + p.dst_base + ulong(tid) * 8);
    va_out[0] = desc[14];
    va_out[1] = desc[15];
}

struct BottomLevel {
    ulong va;
    ulong resource_id;
};

struct ConvertParams {
    ulong src_base;
    ulong dst_base;
    ulong inactive_id;
    uint count;
    uint bottom_levels;
};

kernel void vitreo_rt_convert_instances(device const uchar* src [[buffer(0)]],
                                        device const BottomLevel* blas [[buffer(1)]],
                                        device uchar* dst [[buffer(3)]],
                                        constant ConvertParams& p [[buffer(4)]],
                                        uint tid [[thread_position_in_grid]]) {
    if (tid >= p.count) return;
    const ulong va = ulong(ld_u32(src, p.src_base + ulong(tid) * 8)) |
                     (ulong(ld_u32(src, p.src_base + ulong(tid) * 8 + 4)) << 32);
    const ulong desc = p.src_base + ulong(p.count) * 8 + ulong(tid) * 64;
    float m[12];
    for (uint k = 0; k < 12; ++k) m[k] = ld_f32(src, desc + k * 4);
    const uint id_mask = ld_u32(src, desc + 48);
    const uint contribution_flags = ld_u32(src, desc + 52);
    ulong resource = p.inactive_id;
    uint mask = id_mask >> 24;
    uint lo = 0, hi = p.bottom_levels;
    while (lo < hi) {
        const uint mid = (lo + hi) / 2;
        if (blas[mid].va < va) lo = mid + 1;
        else hi = mid;
    }
    if (va != 0 && lo < p.bottom_levels && blas[lo].va == va) {
        resource = blas[lo].resource_id;
    } else {
        // DXR "Inactive primitives and instances": a NULL (or no) bottom
        // level keeps its InstanceIndex and is never hit.
        mask = 0;
    }
    // MTLIndirectAccelerationStructureInstanceDescriptor: packed column-major
    // 4x3 transform, options, mask, intersection function table offset,
    // user ID, acceleration structure ID.
    device float* t = (device float*)(dst + p.dst_base + ulong(tid) * 72);
    for (uint column = 0; column < 4; ++column) {
        for (uint row = 0; row < 3; ++row) t[column * 3 + row] = m[row * 4 + column];
    }
    device uint* w = (device uint*)(dst + p.dst_base + ulong(tid) * 72 + 48);
    // D3D12_RAYTRACING_INSTANCE_FLAGS bits 0-3 are MTLAccelerationStructure-
    // InstanceOptions bits 0-3 (cull disable, counter-clockwise front,
    // opaque, non-opaque).
    w[0] = (contribution_flags >> 24) & 0xFu;
    w[1] = mask;
    w[2] = contribution_flags & 0xFFFFFFu;
    w[3] = id_mask & 0xFFFFFFu;
    w[4] = uint(resource);
    w[5] = uint(resource >> 32);
}
)MSL";

struct TriangleParams {
    u64 vertex_base, vertex_stride, index_base, transform_base, dst_base;
    u32 vertex_format, index_size, has_transform, count;
};
struct BoxParams {
    u64 src_base, stride, dst_base;
    u32 count, pad;
};
struct MemoryRangeRecord {
    u64 va, size, base;
};
struct GatherParams {
    u64 src_base, dst_base;
    u32 count, pointers, ranges, pad;
};
struct BottomLevelRecord {
    u64 va, resource_id;
};
struct ConvertParams {
    u64 src_base, dst_base, inactive_id;
    u32 count, bottom_levels;
};

static_assert(sizeof(MTLIndirectAccelerationStructureInstanceDescriptor) == 72);
static_assert(offsetof(MTLIndirectAccelerationStructureInstanceDescriptor, options) == 48);
static_assert(offsetof(MTLIndirectAccelerationStructureInstanceDescriptor, mask) == 52);
static_assert(offsetof(MTLIndirectAccelerationStructureInstanceDescriptor,
                       intersectionFunctionTableOffset) == 56);
static_assert(offsetof(MTLIndirectAccelerationStructureInstanceDescriptor, userID) == 60);
static_assert(offsetof(MTLIndirectAccelerationStructureInstanceDescriptor,
                       accelerationStructureID) == 64);

/// A guest buffer as a pass binds it: the MTLBuffer, the 16-byte-aligned
/// binding offset and the byte distance from there to the wanted offset.
struct Bound {
    id<MTLBuffer> buffer{nil};
    NSUInteger offset{0};
    u64 base{0};
};

bool bind(const BackendBuffer* buffer, u64 offset, bool write, Bound& out) {
    if (buffer == nullptr || !buffer->valid()) return false;
    auto* metal = const_cast<MetalBackendBuffer*>(static_cast<const MetalBackendBuffer*>(buffer));
    if (write) metal->note_gpu_write();
    void* handle = metal->native_handle();
    if (handle == nullptr) return false;
    metal->note_gpu_bind();
    const u64 at = offset + metal->native_offset();
    out.buffer = (__bridge id<MTLBuffer>)handle;
    out.offset = static_cast<NSUInteger>(at & ~u64{15});
    out.base = at & 15u;
    return true;
}

/// Bottom-level residency: every live bottom level in one log.
struct BottomLevels {
    std::mutex mutex;
    std::shared_ptr<MetalResidentLog> log = std::make_shared<MetalResidentLog>();
    std::unordered_set<void*> live;
    usize dead_logged{0};

    void add(void* handle) {
        std::lock_guard<std::mutex> lock(mutex);
        live.insert(handle);
        CFRetain(handle);
        log->append(handle);
    }
    void remove(void* handle) {
        std::lock_guard<std::mutex> lock(mutex);
        if (live.erase(handle) == 0) return;
        ++dead_logged;
        // The log keeps a retired bottom level alive; once they outnumber
        // the live ones, a fresh log of the live ones replaces it (draws
        // already recorded keep the old one).
        if (dead_logged > 16 && dead_logged > live.size()) {
            auto fresh = std::make_shared<MetalResidentLog>();
            for (void* h : live) {
                CFRetain(h);
                fresh->append(h);
            }
            log = std::move(fresh);
            dead_logged = 0;
        }
    }
    MetalResidentToken token() {
        std::lock_guard<std::mutex> lock(mutex);
        MetalResidentToken t;
        t.log = log;
        t.count = log->size();
        t.writable = false;
        return t;
    }
    std::vector<id<MTLResource>> snapshot() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<id<MTLResource>> out;
        out.reserve(live.size());
        for (void* h : live) out.push_back((__bridge id<MTLResource>)h);
        return out;
    }
};

BottomLevels& bottom_levels() {
    static BottomLevels* levels = new BottomLevels();
    return *levels;
}

/// Objects a recorded pass needs until its command buffer completes.
struct Keep {
    std::vector<id> objects;
};

void keep_until_complete(id<MTLCommandBuffer> cb, std::shared_ptr<Keep> keep) {
    [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
        (void)keep;
    }];
}

class MetalRaytracing final : public BackendRaytracing {
public:
    explicit MetalRaytracing(id<MTLDevice> device) : m_device(device) {}

    bool sizes(const BackendRtBuild& build, BackendRtSizes& out) override {
        @autoreleasepool {
            MTLAccelerationStructureDescriptor* desc = describe(build, nullptr);
            if (desc == nil) return false;
            const MTLAccelerationStructureSizes s =
                [m_device accelerationStructureSizesWithDescriptor:desc];
            out.structure = s.accelerationStructureSize;
            out.build_scratch = s.buildScratchBufferSize;
            out.update_scratch = (build.flags & kRtBuildAllowUpdate) != 0
                                     ? s.refitScratchBufferSize
                                     : 0u;
            return true;
        }
    }

    u64 instance_record_bytes() const override {
        return sizeof(MTLIndirectAccelerationStructureInstanceDescriptor);
    }

    std::shared_ptr<BackendAccelerationStructure> create(u64 size, bool top_level) override {
        @autoreleasepool {
            id<MTLAccelerationStructure> as =
                [m_device newAccelerationStructureWithSize:static_cast<NSUInteger>(
                                                               std::max<u64>(size, 1))];
            if (as == nil) return nullptr;
            void* handle = (__bridge_retained void*)as;
            auto out = std::make_shared<MetalAccelerationStructure>(handle, size, top_level);
            CFRelease(handle);
            return out;
        }
    }

    bool decode_triangles(const BackendRtTriangleSource& src, const BackendBuffer* dst,
                          u64 dst_offset) override {
        if (src.triangle_count == 0) return true;
        Bound vb, ib, tb, out;
        if (!bind(src.vertices, src.vertex_offset, false, vb) ||
            !bind(dst, dst_offset, true, out)) {
            return false;
        }
        if (src.indices != nullptr && !bind(src.indices, src.index_offset, false, ib)) return false;
        if (src.transform != nullptr && !bind(src.transform, src.transform_offset, false, tb)) {
            return false;
        }
        TriangleParams p{};
        p.vertex_base = vb.base;
        p.vertex_stride = src.vertex_stride;
        p.index_base = ib.base;
        p.transform_base = tb.base;
        p.dst_base = out.base;
        p.vertex_format = static_cast<u32>(src.vertex_format);
        p.index_size = src.indices != nullptr ? src.index_size : 0u;
        p.has_transform = src.transform != nullptr ? 1u : 0u;
        p.count = src.triangle_count;
        id<MTLComputePipelineState> pso = pipeline("vitreo_rt_decode_triangles");
        if (pso == nil) return false;
        if (ib.buffer == nil) ib = vb;
        if (tb.buffer == nil) tb = vb;
        MetalSubmitQueue::instance().record_encode([=](void* command_buffer) {
            id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pso];
            [enc setBuffer:vb.buffer offset:vb.offset atIndex:0];
            [enc setBuffer:ib.buffer offset:ib.offset atIndex:1];
            [enc setBuffer:tb.buffer offset:tb.offset atIndex:2];
            [enc setBuffer:out.buffer offset:out.offset atIndex:3];
            [enc setBytes:&p length:sizeof(p) atIndex:4];
            dispatch(enc, pso, p.count);
            [enc endEncoding];
        });
        return true;
    }

    bool decode_boxes(const BackendRtBoxSource& src, const BackendBuffer* dst,
                      u64 dst_offset) override {
        if (src.count == 0) return true;
        Bound in, out;
        if (!bind(src.boxes, src.offset, false, in) || !bind(dst, dst_offset, true, out)) {
            return false;
        }
        BoxParams p{};
        p.src_base = in.base;
        p.stride = src.stride;
        p.dst_base = out.base;
        p.count = src.count;
        id<MTLComputePipelineState> pso = pipeline("vitreo_rt_decode_boxes");
        if (pso == nil) return false;
        MetalSubmitQueue::instance().record_encode([=](void* command_buffer) {
            id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pso];
            [enc setBuffer:in.buffer offset:in.offset atIndex:0];
            [enc setBuffer:out.buffer offset:out.offset atIndex:3];
            [enc setBytes:&p length:sizeof(p) atIndex:4];
            dispatch(enc, pso, p.count);
            [enc endEncoding];
        });
        return true;
    }

    bool gather_instances(const BackendRtInstanceSource& src, const BackendBuffer* dst,
                          u64 dst_offset) override {
        if (src.count == 0) return true;
        Bound in, out;
        if (!bind(src.buffer, src.offset, false, in) || !bind(dst, dst_offset, true, out)) {
            return false;
        }
        GatherParams p{};
        p.src_base = in.base;
        p.dst_base = out.base;
        p.count = src.count;
        p.pointers = src.pointers ? 1u : 0u;
        std::vector<MemoryRangeRecord> ranges;
        auto keep = std::make_shared<Keep>();
        if (src.pointers) {
            std::vector<BackendRtMemoryRange> sorted = src.memory;
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) { return a.va < b.va; });
            for (const BackendRtMemoryRange& range : sorted) {
                Bound b;
                if (!bind(range.buffer, range.offset, false, b)) continue;
                ranges.push_back({range.va, range.size,
                                  static_cast<u64>(b.buffer.gpuAddress) + b.offset + b.base});
                keep->objects.push_back(b.buffer);
            }
        }
        p.ranges = static_cast<u32>(ranges.size());
        if (ranges.empty()) ranges.push_back({});
        id<MTLComputePipelineState> pso = pipeline("vitreo_rt_gather_instances");
        if (pso == nil) return false;
        MetalSubmitQueue::instance().record_encode([=](void* command_buffer) {
            id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pso];
            [enc setBuffer:in.buffer offset:in.offset atIndex:0];
            id<MTLBuffer> ranges_buffer =
                [m_device newBufferWithBytes:ranges.data()
                                      length:ranges.size() * sizeof(MemoryRangeRecord)
                                     options:MTLResourceStorageModeShared];
            [enc setBuffer:ranges_buffer offset:0 atIndex:1];
            [enc setBuffer:out.buffer offset:out.offset atIndex:3];
            [enc setBytes:&p length:sizeof(p) atIndex:4];
            for (id object : keep->objects) {
                [enc useResource:(id<MTLResource>)object usage:MTLResourceUsageRead];
            }
            dispatch(enc, pso, p.count);
            [enc endEncoding];
            keep_until_complete(cb, keep);
        });
        return true;
    }

    bool convert_instances(const BackendBuffer* src, u64 src_offset, u32 count,
                           const std::vector<BackendRtBottomLevel>& levels,
                           const BackendBuffer* dst, u64 dst_offset) override {
        if (count == 0) return true;
        Bound in, out;
        if (!bind(src, src_offset, false, in) || !bind(dst, dst_offset, true, out)) {
            return false;
        }
        std::vector<BottomLevelRecord> table;
        table.reserve(levels.size() + 1);
        for (const BackendRtBottomLevel& level : levels) {
            const auto* as = static_cast<const MetalAccelerationStructure*>(level.structure);
            if (as == nullptr || !as->valid() || as->top_level()) continue;
            table.push_back({level.va, as->resource_id()});
        }
        std::sort(table.begin(), table.end(),
                  [](const auto& a, const auto& b) { return a.va < b.va; });
        ConvertParams p{};
        p.src_base = in.base;
        p.dst_base = out.base;
        p.count = count;
        p.bottom_levels = static_cast<u32>(table.size());
        MetalAccelerationStructure* inactive = inactive_bottom_level();
        if (inactive == nullptr) return false;
        p.inactive_id = inactive->resource_id();
        if (table.empty()) table.push_back({});
        id<MTLComputePipelineState> pso = pipeline("vitreo_rt_convert_instances");
        if (pso == nil) return false;
        MetalSubmitQueue::instance().record_encode([=](void* command_buffer) {
            id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pso];
            [enc setBuffer:in.buffer offset:in.offset atIndex:0];
            id<MTLBuffer> levels_buffer =
                [m_device newBufferWithBytes:table.data()
                                      length:table.size() * sizeof(BottomLevelRecord)
                                     options:MTLResourceStorageModeShared];
            [enc setBuffer:levels_buffer offset:0 atIndex:1];
            [enc setBuffer:out.buffer offset:out.offset atIndex:3];
            [enc setBytes:&p length:sizeof(p) atIndex:4];
            dispatch(enc, pso, p.count);
            [enc endEncoding];
        });
        return true;
    }

    bool build(BackendAccelerationStructure& dst, const BackendRtBuild& build,
               const BackendBuffer* scratch, u64 scratch_offset,
               const BackendAccelerationStructure* update_source) override {
        auto* target = static_cast<MetalAccelerationStructure*>(&dst);
        const auto* source = static_cast<const MetalAccelerationStructure*>(update_source);
        if (!target->valid() || target->top_level() != build.top_level) return false;
        if (source != nullptr && (!source->valid() || source->top_level() != build.top_level)) {
            return false;
        }
        auto keep = std::make_shared<Keep>();
        MTLAccelerationStructureDescriptor* desc = nil;
        @autoreleasepool {
            desc = describe(build, keep.get());
        }
        if (desc == nil) return false;
        Bound scratch_bound;
        const bool has_scratch = scratch != nullptr;
        if (has_scratch && !bind(scratch, scratch_offset, true, scratch_bound)) return false;
        id<MTLAccelerationStructure> to = (__bridge id<MTLAccelerationStructure>)target->native_handle();
        id<MTLAccelerationStructure> from =
            source != nullptr ? (__bridge id<MTLAccelerationStructure>)source->native_handle() : nil;
        keep->objects.push_back(to);
        if (from != nil) keep->objects.push_back(from);
        const bool top = build.top_level;
        MetalSubmitQueue::instance().record_encode([=](void* command_buffer) {
            id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            if (top) {
                for (id<MTLResource> level : bottom_levels().snapshot()) {
                    [enc useResource:level usage:MTLResourceUsageRead];
                }
            }
            if (from != nil) {
                [enc refitAccelerationStructure:from
                                     descriptor:desc
                                    destination:to
                                  scratchBuffer:has_scratch ? scratch_bound.buffer : nil
                            scratchBufferOffset:scratch_bound.offset + scratch_bound.base];
            } else {
                [enc buildAccelerationStructure:to
                                     descriptor:desc
                                  scratchBuffer:scratch_bound.buffer
                            scratchBufferOffset:scratch_bound.offset + scratch_bound.base];
            }
            [enc endEncoding];
            keep_until_complete(cb, keep);
        });
        return true;
    }

    bool copy(BackendAccelerationStructure& dst, const BackendAccelerationStructure& src) override {
        auto* to_as = static_cast<MetalAccelerationStructure*>(&dst);
        const auto* from_as = static_cast<const MetalAccelerationStructure*>(&src);
        if (!to_as->valid() || !from_as->valid() || to_as->top_level() != from_as->top_level() ||
            to_as->size() < from_as->size()) {
            return false;
        }
        id<MTLAccelerationStructure> to = (__bridge id<MTLAccelerationStructure>)to_as->native_handle();
        id<MTLAccelerationStructure> from =
            (__bridge id<MTLAccelerationStructure>)from_as->native_handle();
        const bool top = from_as->top_level();
        MetalSubmitQueue::instance().record_encode([=](void* command_buffer) {
            id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            if (top) {
                for (id<MTLResource> level : bottom_levels().snapshot()) {
                    [enc useResource:level usage:MTLResourceUsageRead];
                }
            }
            [enc copyAccelerationStructure:from toAccelerationStructure:to];
            [enc endEncoding];
        });
        return true;
    }

    const BackendAccelerationStructure* empty_top_level() override {
        MetalAccelerationStructure* inactive = inactive_bottom_level();
        if (inactive == nullptr) return nullptr;
        std::lock_guard<std::mutex> lock(m_inactive_mutex);
        if (m_empty_top) return m_empty_top.get();
        BackendRtBuild build;
        build.top_level = true;
        build.instance_count = 1;
        BackendRtSizes s;
        if (!sizes(build, s)) return nullptr;
        auto as = create(s.structure, true);
        if (!as) return nullptr;
        @autoreleasepool {
            MTLIndirectAccelerationStructureInstanceDescriptor record{};
            record.mask = 0;
            record.accelerationStructureID = inactive->native_handle() != nullptr
                ? ((__bridge id<MTLAccelerationStructure>)inactive->native_handle()).gpuResourceID
                : MTLResourceID{};
            id<MTLBuffer> instances = [m_device newBufferWithBytes:&record length:sizeof(record)
                                                           options:MTLResourceStorageModeShared];
            id<MTLBuffer> scratch = [m_device newBufferWithLength:std::max<u64>(s.build_scratch, 16)
                                                          options:MTLResourceStorageModePrivate];
            MTLInstanceAccelerationStructureDescriptor* desc =
                [MTLInstanceAccelerationStructureDescriptor descriptor];
            desc.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeIndirect;
            desc.instanceCount = 1;
            desc.instanceDescriptorBuffer = instances;
            id<MTLAccelerationStructure> to =
                (__bridge id<MTLAccelerationStructure>)static_cast<MetalAccelerationStructure*>(
                    as.get())->native_handle();
            id<MTLAccelerationStructure> level =
                (__bridge id<MTLAccelerationStructure>)inactive->native_handle();
            MetalSubmitQueue::instance().record_encode([=](void* command_buffer) {
                id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
                id<MTLAccelerationStructureCommandEncoder> enc =
                    [cb accelerationStructureCommandEncoder];
                [enc useResource:level usage:MTLResourceUsageRead];
                [enc buildAccelerationStructure:to descriptor:desc scratchBuffer:scratch
                            scratchBufferOffset:0];
                [enc endEncoding];
            });
        }
        m_empty_top = std::static_pointer_cast<MetalAccelerationStructure>(as);
        return m_empty_top.get();
    }

private:
    /// The descriptor for @p build; with @p keep, its buffers bound (and
    /// kept) for a real build, else counts only.
    MTLAccelerationStructureDescriptor* describe(const BackendRtBuild& build, Keep* keep) {
        MTLAccelerationStructureUsage usage = MTLAccelerationStructureUsageNone;
        if ((build.flags & kRtBuildAllowUpdate) != 0) usage |= MTLAccelerationStructureUsageRefit;
        if ((build.flags & kRtBuildPreferFastBuild) != 0) {
            usage |= MTLAccelerationStructureUsagePreferFastBuild;
        }
        if ((build.flags & kRtBuildMinimizeMemory) != 0) {
            usage |= MTLAccelerationStructureUsageMinimizeMemory;
        }
        if (build.top_level) {
            MTLInstanceAccelerationStructureDescriptor* desc =
                [MTLInstanceAccelerationStructureDescriptor descriptor];
            desc.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeIndirect;
            desc.instanceCount = build.instance_count;
            desc.usage = usage;
            if (keep != nullptr && build.instance_count != 0) {
                Bound b;
                if (!bind(build.instances, build.instances_offset, false, b)) return nil;
                desc.instanceDescriptorBuffer = b.buffer;
                desc.instanceDescriptorBufferOffset = b.offset + b.base;
                keep->objects.push_back(b.buffer);
            }
            return desc;
        }
        NSMutableArray<MTLAccelerationStructureGeometryDescriptor*>* geometries =
            [NSMutableArray arrayWithCapacity:build.geometries.size()];
        for (const BackendRtGeometry& g : build.geometries) {
            Bound b;
            if (keep != nullptr && g.primitive_count != 0) {
                if (!bind(g.data, g.offset, false, b)) return nil;
                keep->objects.push_back(b.buffer);
            }
            MTLAccelerationStructureGeometryDescriptor* geometry = nil;
            if (g.boxes) {
                MTLAccelerationStructureBoundingBoxGeometryDescriptor* boxes =
                    [MTLAccelerationStructureBoundingBoxGeometryDescriptor descriptor];
                boxes.boundingBoxCount = g.primitive_count;
                boxes.boundingBoxStride = kRtBoxBytes;
                if (b.buffer != nil) {
                    boxes.boundingBoxBuffer = b.buffer;
                    boxes.boundingBoxBufferOffset = b.offset + b.base;
                }
                geometry = boxes;
            } else {
                MTLAccelerationStructureTriangleGeometryDescriptor* tris =
                    [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
                tris.triangleCount = g.primitive_count;
                tris.vertexStride = 12;
                tris.vertexFormat = MTLAttributeFormatFloat3;
                if (b.buffer != nil) {
                    tris.vertexBuffer = b.buffer;
                    tris.vertexBufferOffset = b.offset + b.base;
                }
                geometry = tris;
            }
            geometry.opaque = g.opaque;
            geometry.allowDuplicateIntersectionFunctionInvocation = !g.no_duplicate_any_hit;
            [geometries addObject:geometry];
        }
        MTLPrimitiveAccelerationStructureDescriptor* desc =
            [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        desc.geometryDescriptors = geometries;
        desc.usage = usage;
        return desc;
    }

    id<MTLComputePipelineState> pipeline(const char* name) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_library == nil) {
            NSError* error = nil;
            MTLCompileOptions* options = [MTLCompileOptions new];
            options.languageVersion = MTLLanguageVersion3_0;
            m_library = [m_device newLibraryWithSource:@(kRtKernels) options:options error:&error];
            if (m_library == nil) {
                std::fprintf(stderr, "[metal] ray-tracing passes failed to compile: %s\n",
                             error.localizedDescription.UTF8String);
                return nil;
            }
        }
        NSString* key = @(name);
        id<MTLComputePipelineState> found = m_pipelines[key];
        if (found != nil) return found;
        NSError* error = nil;
        id<MTLFunction> fn = [m_library newFunctionWithName:key];
        id<MTLComputePipelineState> pso =
            fn != nil ? [m_device newComputePipelineStateWithFunction:fn error:&error] : nil;
        if (pso == nil) {
            std::fprintf(stderr, "[metal] ray-tracing pass %s has no pipeline\n", name);
            return nil;
        }
        if (m_pipelines == nil) m_pipelines = [NSMutableDictionary dictionary];
        m_pipelines[key] = pso;
        return pso;
    }

    static void dispatch(id<MTLComputeCommandEncoder> enc, id<MTLComputePipelineState> pso,
                         u32 count) {
        const NSUInteger width = std::min<NSUInteger>(pso.maxTotalThreadsPerThreadgroup, 64);
        [enc dispatchThreads:MTLSizeMake(count, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
    }

    /// A bottom level no ray reaches (one triangle at the origin, a point):
    /// what an inactive instance's record names, with mask 0.
    MetalAccelerationStructure* inactive_bottom_level() {
        std::lock_guard<std::mutex> lock(m_inactive_mutex);
        if (m_inactive) return m_inactive.get();
        BackendRtBuild build;
        BackendRtGeometry g;
        g.primitive_count = 1;
        g.opaque = true;
        build.geometries.push_back(g);
        BackendRtSizes s;
        if (!sizes(build, s)) return nullptr;
        auto as = create(s.structure, false);
        if (!as) return nullptr;
        @autoreleasepool {
            const float zeros[9] = {};
            id<MTLBuffer> vb = [m_device newBufferWithBytes:zeros length:sizeof(zeros)
                                                    options:MTLResourceStorageModeShared];
            id<MTLBuffer> scratch = [m_device newBufferWithLength:std::max<u64>(s.build_scratch, 16)
                                                          options:MTLResourceStorageModePrivate];
            MTLAccelerationStructureTriangleGeometryDescriptor* tri =
                [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
            tri.triangleCount = 1;
            tri.vertexStride = 12;
            tri.vertexFormat = MTLAttributeFormatFloat3;
            tri.vertexBuffer = vb;
            tri.opaque = YES;
            MTLPrimitiveAccelerationStructureDescriptor* desc =
                [MTLPrimitiveAccelerationStructureDescriptor descriptor];
            desc.geometryDescriptors = @[ tri ];
            id<MTLAccelerationStructure> to =
                (__bridge id<MTLAccelerationStructure>)static_cast<MetalAccelerationStructure*>(
                    as.get())->native_handle();
            MetalSubmitQueue::instance().record_encode([=](void* command_buffer) {
                id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
                id<MTLAccelerationStructureCommandEncoder> enc =
                    [cb accelerationStructureCommandEncoder];
                [enc buildAccelerationStructure:to descriptor:desc scratchBuffer:scratch
                            scratchBufferOffset:0];
                [enc endEncoding];
            });
        }
        m_inactive = std::static_pointer_cast<MetalAccelerationStructure>(as);
        return m_inactive.get();
    }

    id<MTLDevice> m_device;
    std::mutex m_mutex;
    id<MTLLibrary> m_library{nil};
    NSMutableDictionary<NSString*, id<MTLComputePipelineState>>* m_pipelines{nil};
    std::mutex m_inactive_mutex;
    std::shared_ptr<MetalAccelerationStructure> m_inactive;
    std::shared_ptr<MetalAccelerationStructure> m_empty_top;
};

}  // namespace

MetalAccelerationStructure::MetalAccelerationStructure(void* retained, u64 size, bool top_level)
    : m_handle(retained), m_size(size), m_top_level(top_level) {
    if (m_handle == nullptr) return;
    CFRetain(m_handle);
    id<MTLAccelerationStructure> as = (__bridge id<MTLAccelerationStructure>)m_handle;
    m_resource_id = as.gpuResourceID._impl;
    if (!top_level) bottom_levels().add(m_handle);
}

MetalAccelerationStructure::~MetalAccelerationStructure() {
    if (m_handle == nullptr) return;
    if (!m_top_level) bottom_levels().remove(m_handle);
    CFRelease(m_handle);
}

BackendRaytracing* metal_raytracing(void* device) {
    static std::once_flag once;
    static MetalRaytracing* rt = nullptr;
    std::call_once(once, [&] {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)device;
        if (dev != nil && dev.supportsRaytracing) rt = new MetalRaytracing(dev);
    });
    return rt;
}

MetalResidentToken metal_rt_bottom_level_token() { return bottom_levels().token(); }

}  // namespace vitreo::graphics
