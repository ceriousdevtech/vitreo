// SPDX-License-Identifier: LGPL-2.1-only
//
// WS5 Task 1: the upper-bound probe that decides whether geometry-shader
// draws are worth moving onto Metal mesh pipelines at all, before anything
// is built (no-disabled-lever rule: probe first, ship default-on or revert).
//
// Hand-written MSL in raw Metal, in the shape of the real lowering
// (d3d11_geometry.cpp execute_geometry_direct, which batches every instance
// of one D3D draw into one capture, one expansion and one draw):
//
//   A (today)  per draw: capture render pass on a 1x1 target (raster off)
//              -> compute expansion (one thread per GS invocation, writes the
//              list vertices and the per-invocation count) -> render pass on
//              the guest target (Load) drawing the expanded list.
//   B          per draw: capture pass on 1x1 -> render pass with a mesh draw
//              that expands in threadgroup memory.
//   C          one render pass for the whole frame: per draw a raster-off
//              capture draw, memoryBarrier(buffers, after: vertex, before:
//              object|mesh), then the mesh draw.
//
// Workload: kDraws draws (64 by default, VITREO_WS5_PROBE_DRAWS overrides;
// 1 is the plan's literal single-draw workload) x 1,152 instances x 1 point;
// the VS writes 2 float4 records; the GS emits a 4-vertex strip (a quad, 6
// list vertices); Max blend into a 256x256 RGBA8 target. Arm A's list draw is
// an indirect draw whose arguments the expansion kernel writes, as today's
// pass C is. GPU time is GPUEndTime - GPUStartTime of one MTLCommandBuffer
// per rep: 3 warm-ups, then 21 interleaved reps per arm (A B C A B C ...;
// VITREO_WS5_PROBE_REPS overrides). Each arm prints min / median / max, and
// the run is flagged NOISY when any arm's max/min exceeds kNoisySpread, so a
// contended run is visible in its own output.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <stdlib.h>  // getloadavg

#if !__has_feature(objc_arc)
#error "test_gs_mesh_probe.mm is ARC"
#endif

namespace {

constexpr uint32_t kInstances = 1152;
constexpr uint32_t kMaxDraws = 64;
constexpr double kNoisySpread = 1.5;
constexpr uint32_t kTarget = 256;
constexpr uint32_t kVpi = 6;           // point -> 4-vertex strip -> 2 tris
constexpr uint32_t kK = 256 / kVpi;    // invocations per mesh threadgroup (42)
constexpr uint32_t kRecVec4 = 2;
constexpr uint32_t kRecordBytes = kInstances * kRecVec4 * 16;
constexpr uint32_t kExpandBytes = kInstances * kVpi * kRecVec4 * 16;

const char* kProbeMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct DrawArgs { uint draw; uint instances; float half_size; };

// The VS: one record of 2 float4 per (vertex, instance), captured raster-off.
vertex void vs_capture(uint vid [[vertex_id]], uint iid [[instance_id]],
                       device float4* rec [[buffer(0)]],
                       constant DrawArgs& a [[buffer(1)]]) {
    uint i = iid;
    uint d = a.draw;
    float x = (float((i * 7u + d * 13u) % 48u) + 0.5) / 48.0 * 2.0 - 1.0;
    float y = (float((i / 48u + d * 5u) % 24u) + 0.5) / 24.0 * 2.0 - 1.0;
    rec[i * 2u + 0u] = float4(x, y, 0.5, 1.0);
    uint h = (i * 2654435761u) ^ (d * 40503u);
    rec[i * 2u + 1u] = float4(float(h & 255u) / 255.0,
                              float((h >> 8) & 255u) / 255.0,
                              float((h >> 16) & 255u) / 255.0, 1.0);
}

// The GS body, shared by the compute expansion and the mesh: point -> quad.
inline void gs_emit(float4 p, float4 c, float s, thread float4* o) {
    float4 v0 = p + float4(-s, -s, 0, 0);
    float4 v1 = p + float4( s, -s, 0, 0);
    float4 v2 = p + float4(-s,  s, 0, 0);
    float4 v3 = p + float4( s,  s, 0, 0);
    // Strip 0 1 2 3 -> list (0 1 2) (2 1 3), the translator's strip expansion.
    o[0] = v0; o[1] = c; o[2] = v1; o[3] = c; o[4] = v2; o[5] = c;
    o[6] = v2; o[7] = c; o[8] = v1; o[9] = c; o[10] = v3; o[11] = c;
}

kernel void cs_expand(uint gid [[thread_position_in_grid]],
                      const device float4* rec [[buffer(0)]],
                      device float4* out [[buffer(1)]],
                      device uint* counts [[buffer(2)]],
                      constant DrawArgs& a [[buffer(3)]],
                      device uint* args [[buffer(4)]]) {
    // MTLDrawPrimitivesIndirectArguments for pass C (vertexCount,
    // instanceCount, vertexStart, baseInstance).
    if (gid == 0u) {
        args[0] = a.instances * 6u; args[1] = 1u; args[2] = 0u; args[3] = 0u;
    }
    if (gid >= a.instances) return;
    float4 o[12];
    gs_emit(rec[gid * 2u], rec[gid * 2u + 1u], a.half_size, o);
    for (uint n = 0; n < 12u; ++n) out[gid * 12u + n] = o[n];
    counts[gid] = 6u;
}

struct VOut {
    float4 pos [[position]];
    float4 color [[user(locn0)]];
};

// Pass C: the expanded list, unwritten slots culled (w = 0 is clipped).
vertex VOut vs_list(uint vid [[vertex_id]],
                    const device float4* out [[buffer(0)]],
                    const device uint* counts [[buffer(1)]]) {
    VOut r;
    if (vid % 6u >= counts[vid / 6u]) {
        r.pos = float4(0.0); r.color = float4(0.0);
        return r;
    }
    r.pos = out[vid * 2u]; r.color = out[vid * 2u + 1u];
    return r;
}

struct FIn { float4 color [[user(locn0)]]; };
fragment float4 fs_color(FIn in [[stage_in]]) { return in.color; }

struct Payload { uint base_group; };
struct POut { bool culled [[primitive_culled]]; };
using GsMesh = metal::mesh<VOut, POut, 252, 84, topology::triangle>;

[[object]] void os_launch(object_data Payload& payload [[payload]],
                          mesh_grid_properties mgp,
                          uint otg [[threadgroup_position_in_grid]],
                          constant uint& total [[buffer(0)]]) {
    payload.base_group = otg * 1024u;
    mgp.set_threadgroups_per_grid(uint3(min(1024u, total - otg * 1024u), 1, 1));
}

[[mesh]] void ms_gs(GsMesh m,
                    const object_data Payload& payload [[payload]],
                    uint3 tg [[threadgroup_position_in_grid]],
                    uint tid [[thread_index_in_threadgroup]],
                    const device float4* rec [[buffer(0)]],
                    constant DrawArgs& a [[buffer(1)]]) {
    threadgroup float4 scratch[42 * 12];
    threadgroup uint counts[42];
    uint inv = (payload.base_group + tg.x) * 42u + tid;
    counts[tid] = 0u;
    if (inv < a.instances) {
        float4 o[12];
        gs_emit(rec[inv * 2u], rec[inv * 2u + 1u], a.half_size, o);
        for (uint n = 0; n < 12u; ++n) scratch[tid * 12u + n] = o[n];
        counts[tid] = 6u;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint j = tid; j < 252u; j += 42u) {
        if (j % 6u < counts[j / 6u]) {
            VOut v;
            v.pos = scratch[j * 2u];
            v.color = scratch[j * 2u + 1u];
            m.set_vertex(j, v);
        }
        m.set_index(j, uchar(j));
    }
    for (uint p = tid; p < 84u; p += 42u) {
        POut q;
        q.culled = (p * 3u + 2u) % 6u >= counts[(p * 3u) / 6u];
        m.set_primitive(p, q);
    }
    if (tid == 0u) m.set_primitive_count(84u);
}
)MSL";

struct DrawArgs {
    uint32_t draw;
    uint32_t instances;
    float half_size;
};

/// Quad half-size in NDC. 0.03 (about 8x8 pixels at 256x256) is the gate
/// workload; VITREO_WS5_PROBE_HALF_SIZE overrides it for a diagnostic sweep
/// (0 = no coverage: the encoder-overhead-only bound).
uint32_t env_u32(const char* name, uint32_t def, uint32_t lo, uint32_t hi) {
    const char* v = std::getenv(name);
    if (!v) return def;
    const long n = std::atol(v);
    return static_cast<uint32_t>(std::clamp<long>(n, lo, hi));
}

uint32_t probe_draws() { return env_u32("VITREO_WS5_PROBE_DRAWS", kMaxDraws, 1, kMaxDraws); }
uint32_t probe_reps() { return env_u32("VITREO_WS5_PROBE_REPS", 21, 1, 1001); }

float probe_half_size() {
    const char* v = std::getenv("VITREO_WS5_PROBE_HALF_SIZE");
    return v ? static_cast<float>(std::atof(v)) : 0.03f;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

class GsMeshProbe : public ::testing::Test {
protected:
    void SetUp() override {
        // A benchmark: never part of a default sweep (ctest -E "[Oo]racle").
        if (const char* v = std::getenv("VITREO_WS5_PROBE"); v == nullptr || v[0] != '1') {
            GTEST_SKIP() << "the WS5 upper-bound probe runs with VITREO_WS5_PROBE=1";
        }
        device = MTLCreateSystemDefaultDevice();
        if (device == nil) GTEST_SKIP() << "no Metal device";
        if (![device supportsFamily:MTLGPUFamilyApple7]) {
            GTEST_SKIP() << "mesh shading needs MTLGPUFamilyApple7";
        }
        queue = [device newCommandQueue];
        NSError* err = nil;
        lib = [device newLibraryWithSource:@(kProbeMsl) options:nil error:&err];
        ASSERT_NE(lib, nil) << (err ? err.localizedDescription.UTF8String : "");

        MTLRenderPipelineDescriptor* cap1 = [MTLRenderPipelineDescriptor new];
        cap1.vertexFunction = [lib newFunctionWithName:@"vs_capture"];
        cap1.rasterizationEnabled = NO;
        cap1.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        capture_pso = [device newRenderPipelineStateWithDescriptor:cap1 error:&err];
        ASSERT_NE(capture_pso, nil) << err.localizedDescription.UTF8String;

        expand_pso = [device newComputePipelineStateWithFunction:
                                 [lib newFunctionWithName:@"cs_expand"] error:&err];
        ASSERT_NE(expand_pso, nil) << err.localizedDescription.UTF8String;

        MTLRenderPipelineDescriptor* list = [MTLRenderPipelineDescriptor new];
        list.vertexFunction = [lib newFunctionWithName:@"vs_list"];
        list.fragmentFunction = [lib newFunctionWithName:@"fs_color"];
        set_max_blend(list.colorAttachments[0]);
        list_pso = [device newRenderPipelineStateWithDescriptor:list error:&err];
        ASSERT_NE(list_pso, nil) << err.localizedDescription.UTF8String;

        MTLMeshRenderPipelineDescriptor* mesh = [MTLMeshRenderPipelineDescriptor new];
        mesh.objectFunction = [lib newFunctionWithName:@"os_launch"];
        mesh.meshFunction = [lib newFunctionWithName:@"ms_gs"];
        mesh.fragmentFunction = [lib newFunctionWithName:@"fs_color"];
        mesh.payloadMemoryLength = 16;
        mesh.maxTotalThreadsPerObjectThreadgroup = 1;
        mesh.maxTotalThreadsPerMeshThreadgroup = kK;
        set_max_blend(mesh.colorAttachments[0]);
        mesh_pso = [device newRenderPipelineStateWithMeshDescriptor:mesh
                                                            options:MTLPipelineOptionNone
                                                         reflection:nil
                                                              error:&err];
        ASSERT_NE(mesh_pso, nil) << err.localizedDescription.UTF8String;

        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                         width:kTarget height:kTarget mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModeShared;
        target = [device newTextureWithDescriptor:td];
        td.width = td.height = 1;
        td.storageMode = MTLStorageModePrivate;
        one_by_one = [device newTextureWithDescriptor:td];

        records = [device newBufferWithLength:kRecordBytes * kMaxDraws
                                      options:MTLResourceStorageModePrivate];
        expanded = [device newBufferWithLength:kExpandBytes * kMaxDraws
                                       options:MTLResourceStorageModePrivate];
        counts = [device newBufferWithLength:kInstances * 4 * kMaxDraws
                                     options:MTLResourceStorageModePrivate];
        indirect = [device newBufferWithLength:16 * kMaxDraws
                                       options:MTLResourceStorageModePrivate];
        draws = probe_draws();
    }

    static void set_max_blend(MTLRenderPipelineColorAttachmentDescriptor* c) {
        c.pixelFormat = MTLPixelFormatRGBA8Unorm;
        c.blendingEnabled = YES;
        c.rgbBlendOperation = MTLBlendOperationMax;
        c.alphaBlendOperation = MTLBlendOperationMax;
        c.sourceRGBBlendFactor = MTLBlendFactorOne;
        c.destinationRGBBlendFactor = MTLBlendFactorOne;
        c.sourceAlphaBlendFactor = MTLBlendFactorOne;
        c.destinationAlphaBlendFactor = MTLBlendFactorOne;
    }

    MTLRenderPassDescriptor* pass(id<MTLTexture> t, bool clear) {
        MTLRenderPassDescriptor* p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = t;
        p.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
        p.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        p.colorAttachments[0].storeAction = MTLStoreActionStore;
        return p;
    }

    void encode_capture(id<MTLRenderCommandEncoder> e, uint32_t d) {
        DrawArgs a{d, kInstances, probe_half_size()};
        [e setRenderPipelineState:capture_pso];
        [e setVertexBuffer:records offset:kRecordBytes * d atIndex:0];
        [e setVertexBytes:&a length:sizeof(a) atIndex:1];
        [e drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:1
            instanceCount:kInstances];
    }

    void encode_mesh(id<MTLRenderCommandEncoder> e, uint32_t d) {
        DrawArgs a{d, kInstances, probe_half_size()};
        const uint32_t groups = (kInstances + kK - 1) / kK;
        [e setRenderPipelineState:mesh_pso];
        [e setObjectBytes:&groups length:4 atIndex:0];
        [e setMeshBuffer:records offset:kRecordBytes * d atIndex:0];
        [e setMeshBytes:&a length:sizeof(a) atIndex:1];
        [e drawMeshThreadgroups:MTLSizeMake((groups + 1023) / 1024, 1, 1)
            threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
              threadsPerMeshThreadgroup:MTLSizeMake(kK, 1, 1)];
    }

    // One rep: clears the target, draws the frame, returns GPU ms.
    double run(char arm) {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        {   // Clear.
            id<MTLRenderCommandEncoder> e =
                [cb renderCommandEncoderWithDescriptor:pass(target, true)];
            [e endEncoding];
        }
        if (arm == 'C') {
            id<MTLRenderCommandEncoder> e =
                [cb renderCommandEncoderWithDescriptor:pass(target, false)];
            for (uint32_t d = 0; d < draws; ++d) {
                encode_capture(e, d);
                [e memoryBarrierWithScope:MTLBarrierScopeBuffers
                              afterStages:MTLRenderStageVertex
                             beforeStages:MTLRenderStageObject | MTLRenderStageMesh];
                encode_mesh(e, d);
            }
            [e endEncoding];
        } else {
            for (uint32_t d = 0; d < draws; ++d) {
                id<MTLRenderCommandEncoder> c =
                    [cb renderCommandEncoderWithDescriptor:pass(one_by_one, false)];
                encode_capture(c, d);
                [c endEncoding];
                if (arm == 'A') {
                    DrawArgs a{d, kInstances, probe_half_size()};
                    id<MTLComputeCommandEncoder> x = [cb computeCommandEncoder];
                    [x setComputePipelineState:expand_pso];
                    [x setBuffer:records offset:kRecordBytes * d atIndex:0];
                    [x setBuffer:expanded offset:kExpandBytes * d atIndex:1];
                    [x setBuffer:counts offset:kInstances * 4 * d atIndex:2];
                    [x setBytes:&a length:sizeof(a) atIndex:3];
                    [x setBuffer:indirect offset:16 * d atIndex:4];
                    [x dispatchThreads:MTLSizeMake(kInstances, 1, 1)
                        threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                    [x endEncoding];
                    id<MTLRenderCommandEncoder> r =
                        [cb renderCommandEncoderWithDescriptor:pass(target, false)];
                    [r setRenderPipelineState:list_pso];
                    [r setVertexBuffer:expanded offset:kExpandBytes * d atIndex:0];
                    [r setVertexBuffer:counts offset:kInstances * 4 * d atIndex:1];
                    [r drawPrimitives:MTLPrimitiveTypeTriangle
                       indirectBuffer:indirect
                 indirectBufferOffset:16 * d];
                    [r endEncoding];
                } else {
                    id<MTLRenderCommandEncoder> r =
                        [cb renderCommandEncoderWithDescriptor:pass(target, false)];
                    encode_mesh(r, d);
                    [r endEncoding];
                }
            }
        }
        [cb commit];
        [cb waitUntilCompleted];
        EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted)
            << (cb.error ? cb.error.localizedDescription.UTF8String : "");
        return (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
    }

    std::vector<uint32_t> readback() {
        std::vector<uint32_t> px(kTarget * kTarget);
        [target getBytes:px.data() bytesPerRow:kTarget * 4
              fromRegion:MTLRegionMake2D(0, 0, kTarget, kTarget) mipmapLevel:0];
        return px;
    }

    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLLibrary> lib = nil;
    id<MTLRenderPipelineState> capture_pso = nil, list_pso = nil, mesh_pso = nil;
    id<MTLComputePipelineState> expand_pso = nil;
    id<MTLTexture> target = nil, one_by_one = nil;
    id<MTLBuffer> records = nil, expanded = nil, counts = nil, indirect = nil;
    uint32_t draws = kMaxDraws;
};

TEST_F(GsMeshProbe, UpperBound) {
    const char arms[3] = {'A', 'B', 'C'};
    std::vector<uint32_t> pixels[3];
    for (int w = 0; w < 3; ++w) {
        for (char arm : arms) (void)run(arm);
    }
    const uint32_t reps = probe_reps();
    std::vector<double> ms[3];
    for (uint32_t rep = 0; rep < reps; ++rep) {
        for (int a = 0; a < 3; ++a) {
            ms[a].push_back(run(arms[a]));
            pixels[a] = readback();
        }
    }
    double load[3] = {0, 0, 0};
    (void)getloadavg(load, 3);
    const double ma = median(ms[0]), mb = median(ms[1]), mc = median(ms[2]);
    bool noisy = false;
    for (int a = 0; a < 3; ++a) {
        const auto [lo, hi] = std::minmax_element(ms[a].begin(), ms[a].end());
        noisy |= *hi / *lo > kNoisySpread;
    }
    std::printf("[ws5-probe] draws=%u instances=%u reps=%u half_size=%.3f "
                "load1=%.1f%s\n",
                draws, kInstances, reps, probe_half_size(), load[0],
                noisy ? " NOISY" : "");
    std::printf("[ws5-probe] median: A(three-pass)=%.3f ms B(capture pass + "
                "mesh)=%.3f ms C(in-pass capture + barrier + mesh)=%.3f ms "
                "ratio_B=%.2fx ratio_C=%.2fx\n",
                ma, mb, mc, ma / mb, ma / mc);
    for (int a = 0; a < 3; ++a) {
        const auto [lo, hi] = std::minmax_element(ms[a].begin(), ms[a].end());
        std::printf("[ws5-probe] arm %c min=%.3f median=%.3f max=%.3f reps:",
                    arms[a], *lo, median(ms[a]), *hi);
        for (double v : ms[a]) std::printf(" %.3f", v);
        std::printf("\n");
    }
    size_t lit = 0;
    for (uint32_t p : pixels[0]) lit += p != 0;
    if (probe_half_size() > 0.0f) EXPECT_GT(lit, 1000u) << "the frame drew nothing";
    EXPECT_EQ(pixels[0], pixels[1]) << "mesh (B) differs from three-pass";
    EXPECT_EQ(pixels[0], pixels[2]) << "in-pass mesh (C) differs from three-pass";
}

}  // namespace
