// SPDX-License-Identifier: LGPL-2.1-only
/// @file metal_tiled.mm
/// @brief D3D11.2 tiled resources on Metal 4 placement sparse resources
///        (see metal_tiled.hpp).

#import <Metal/Metal.h>

#include "graphics/metal/metal_tiled.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_submit_queue.hpp"
#include "core/memory/host_memory_floor.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vitreo::graphics {

namespace {

/// Tiles per placement heap: a pool is allocated 4 MiB at a time, as its
/// tiles are first mapped. (D3D leaves the breakdown of a pool into
/// allocations to the driver -- ResizeTilePool remarks.)
constexpr u64 kGranuleTiles = 64;

/// One tile's update, resolved: a region of one tile, and where it goes.
struct TileOp {
    u32 level{0};
    u32 slice{0};
    u32 x{0};
    u32 y{0};
    u64 heap_offset{0};
};

struct Parked {
    void* object{nullptr};  ///< retained
    u64 gpu_gen{0};
    bool resident{false};   ///< remove from the residency set when freed
};

std::mutex g_mutex;
std::vector<Parked> g_parked;
std::vector<std::pair<std::shared_ptr<MetalTileMappingBatch>, u64>> g_batches;
std::map<std::pair<u32, unsigned>, std::pair<unsigned, unsigned>> g_shapes;
std::unordered_set<const void*> g_self_aliased;

}  // namespace

struct MetalTileMappingBatch {
    id resource{nil};
    bool texture{false};
    struct Group {
        id<MTLHeap> heap{nil};  ///< nil: the group unmaps
        std::vector<TileOp> ops;
    };
    std::vector<Group> groups;
};

class MetalTilePool final : public BackendTilePool {
public:
    explicit MetalTilePool(void* device) : m_device(device) {}

    ~MetalTilePool() override {
        std::lock_guard<std::mutex> lock(m_mu);
        const u64 gen = MetalSubmitQueue::instance().recording_generation();
        for (auto& [index, granule] : m_granules) {
            if (granule.heap != nullptr) {
                MetalTiled::instance().release_after(granule.heap, gen, true);
                granule.heap = nullptr;
            }
        }
    }

    bool reference(u64 tile) override {
        std::lock_guard<std::mutex> lock(m_mu);
        Granule& granule = m_granules[tile / kGranuleTiles];
        if (granule.heap == nullptr) {
            granule.heap = allocate_heap();
            if (granule.heap == nullptr) {
                if (granule.refs == 0) m_granules.erase(tile / kGranuleTiles);
                return false;
            }
        }
        ++granule.refs;
        return true;
    }

    void unreference(u64 tile) override {
        std::lock_guard<std::mutex> lock(m_mu);
        const u64 index = tile / kGranuleTiles;
        auto it = m_granules.find(index);
        if (it == m_granules.end() || it->second.refs == 0) return;
        --it->second.refs;
        if (it->second.refs == 0 && index * kGranuleTiles >= m_size) {
            release(it);
        }
    }

    void set_size(u64 tiles) override {
        std::lock_guard<std::mutex> lock(m_mu);
        m_size = tiles;
        for (auto it = m_granules.begin(); it != m_granules.end();) {
            if (it->second.refs == 0 && it->first * kGranuleTiles >= m_size) {
                it = release(it);
            } else {
                ++it;
            }
        }
    }

    /// The heap holding pool tile @p tile (which a mapping references) and
    /// the tile's offset in it, in tiles.
    [[nodiscard]] void* heap_for(u64 tile, u64& offset) {
        std::lock_guard<std::mutex> lock(m_mu);
        auto it = m_granules.find(tile / kGranuleTiles);
        if (it == m_granules.end()) return nullptr;
        offset = tile % kGranuleTiles;
        return it->second.heap;
    }

private:
    struct Granule {
        void* heap{nullptr};  ///< id<MTLHeap>, retained
        u64 refs{0};
    };
    using Granules = std::unordered_map<u64, Granule>;

    Granules::iterator release(Granules::iterator it) {
        if (it->second.heap != nullptr) {
            // Mapped tiles may still be read by recorded work: the heap goes
            // once everything recorded so far has retired.
            MetalTiled::instance().release_after(
                it->second.heap,
                MetalSubmitQueue::instance().recording_generation(), true);
        }
        return m_granules.erase(it);
    }

    void* allocate_heap() {
        const u64 bytes = kGranuleTiles * BACKEND_TILE_BYTES;
        if (!vitreo::memory::host_can_allocate_gpu(
                bytes, MetalDevice::instance().gpu_working_set(),
                "GPU tile pool")) {
            return nullptr;
        }
        if (@available(macOS 26.0, *)) {
            id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
            MTLHeapDescriptor* desc = [MTLHeapDescriptor new];
            desc.type = MTLHeapTypePlacement;
            desc.storageMode = MTLStorageModePrivate;
            desc.size = static_cast<NSUInteger>(bytes);
            desc.maxCompatiblePlacementSparsePageSize = MTLSparsePageSize64;
            id<MTLHeap> heap = [device newHeapWithDescriptor:desc];
            if (heap == nil) return nullptr;
            heap.label = @"vitreo tile pool";
            void* retained = (__bridge_retained void*)heap;
            MetalTiled::instance().make_resident(retained);
            return retained;
        }
        return nullptr;
    }

    void* m_device{nullptr};
    std::mutex m_mu;
    Granules m_granules;
    u64 m_size{0};
};

namespace {

/// CopyTiles / UpdateTiles of a multisampled tiled texture. A blit cannot
/// move multisampled texels, so a kernel reads every sample and a
/// per-sample fragment function ([[sample_id]]) writes them, in the D3D
/// linear tile layout: "each pixel's samples in sample-index order before
/// ... the next pixel", rows `row` bytes apart (Learn, CopyTiles remarks).
/// Colour moves through an unsigned-integer view of the same texel size (the
/// bits verbatim), depth through its value (D16 as its 16-bit code).
constexpr const char* kTileMsaaSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct TileArgs {
    uint width; uint height; uint samples; uint bpp;
    uint row; uint stride; uint x0; uint y0;
    uint offset; uint depth16; uint pad0; uint pad1;
};
static void put(device uchar* p, uint4 v, uint bpp) {
    if (bpp == 1) { p[0] = uchar(v.x); return; }
    if (bpp == 2) { *(device ushort*)p = ushort(v.x); return; }
    device uint* q = (device uint*)p;
    q[0] = v.x;
    if (bpp >= 8) q[1] = v.y;
    if (bpp == 16) { q[2] = v.z; q[3] = v.w; }
}
static uint4 get(const device uchar* p, uint bpp) {
    if (bpp == 1) return uint4(p[0], 0, 0, 0);
    if (bpp == 2) return uint4(*(const device ushort*)p, 0, 0, 0);
    const device uint* q = (const device uint*)p;
    if (bpp == 4) return uint4(q[0], 0, 0, 0);
    if (bpp == 8) return uint4(q[0], q[1], 0, 0);
    return uint4(q[0], q[1], q[2], q[3]);
}
kernel void tile_read_ms(texture2d_ms<uint, access::read> src [[texture(0)]],
                         constant TileArgs& a [[buffer(0)]],
                         device uchar* dst [[buffer(1)]],
                         uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= a.width || gid.y >= a.height) return;
    for (uint s = 0; s < a.samples; ++s)
        put(dst + a.offset + gid.y * a.row + gid.x * a.stride + s * a.bpp,
            src.read(gid + uint2(a.x0, a.y0), s), a.bpp);
}
kernel void tile_read_ms_depth(depth2d_ms<float, access::read> src [[texture(0)]],
                               constant TileArgs& a [[buffer(0)]],
                               device uchar* dst [[buffer(1)]],
                               uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= a.width || gid.y >= a.height) return;
    for (uint s = 0; s < a.samples; ++s) {
        float d = src.read(gid + uint2(a.x0, a.y0), s);
        uint bits = a.depth16 != 0 ? uint(rint(saturate(d) * 65535.0))
                                   : as_type<uint>(d);
        put(dst + a.offset + gid.y * a.row + gid.x * a.stride + s * a.bpp,
            uint4(bits, 0, 0, 0), a.bpp);
    }
}
struct VOut { float4 pos [[position]]; };
vertex VOut tile_vs(uint vid [[vertex_id]]) {
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
static const device uchar* texel(VOut in, uint s, constant TileArgs& a,
                                 const device uchar* src) {
    uint2 xy = uint2(in.pos.xy) - uint2(a.x0, a.y0);
    return src + a.offset + xy.y * a.row + xy.x * a.stride + s * a.bpp;
}
fragment uint4 tile_write_ms(VOut in [[stage_in]], uint s [[sample_id]],
                             constant TileArgs& a [[buffer(0)]],
                             const device uchar* src [[buffer(2)]]) {
    return get(texel(in, s, a, src), a.bpp);
}
struct DepthOut { float depth [[depth(any)]]; };
fragment DepthOut tile_write_ms_depth(VOut in [[stage_in]], uint s [[sample_id]],
                                      constant TileArgs& a [[buffer(0)]],
                                      const device uchar* src [[buffer(2)]]) {
    DepthOut o;
    uint bits = get(texel(in, s, a, src), a.bpp).x;
    o.depth = a.depth16 != 0 ? float(bits) / 65535.0 : as_type<float>(bits);
    return o;
}
)MSL";

struct TileArgs {
    u32 width, height, samples, bpp, row, stride, x0, y0, offset, depth16,
        pad0, pad1;
};

/// Compiled once; pipelines by name / attachment format and sample count.
class TileMsaaPrograms {
public:
    static TileMsaaPrograms& instance() {
        static TileMsaaPrograms* programs = new TileMsaaPrograms();
        return *programs;
    }
    void* compute(void* device, const char* name) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!load(device)) return nullptr;
        void*& slot = m_compute[name];
        if (slot == nullptr) {
            @autoreleasepool {
                id<MTLLibrary> library = (__bridge id<MTLLibrary>)m_library;
                id<MTLFunction> fn = [library
                    newFunctionWithName:[NSString stringWithUTF8String:name]];
                NSError* error = nil;
                id<MTLComputePipelineState> pso =
                    fn != nil ? [(__bridge id<MTLDevice>)device
                                    newComputePipelineStateWithFunction:fn
                                                                  error:&error]
                              : nil;
                if (pso != nil) slot = (__bridge_retained void*)pso;
            }
        }
        return slot;
    }
    void* render(void* device, bool depth, u32 format, u32 samples) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!load(device)) return nullptr;
        void*& slot = m_render[(static_cast<u64>(depth) << 40) |
                               (static_cast<u64>(format) << 8) | samples];
        if (slot == nullptr) {
            @autoreleasepool {
                id<MTLLibrary> library = (__bridge id<MTLLibrary>)m_library;
                MTLRenderPipelineDescriptor* desc =
                    [[MTLRenderPipelineDescriptor alloc] init];
                desc.vertexFunction = [library newFunctionWithName:@"tile_vs"];
                desc.fragmentFunction = [library
                    newFunctionWithName:depth ? @"tile_write_ms_depth"
                                              : @"tile_write_ms"];
                desc.rasterSampleCount = samples;
                if (depth) {
                    desc.depthAttachmentPixelFormat =
                        static_cast<MTLPixelFormat>(format);
                } else {
                    desc.colorAttachments[0].pixelFormat =
                        static_cast<MTLPixelFormat>(format);
                }
                NSError* error = nil;
                id<MTLRenderPipelineState> pso =
                    desc.vertexFunction != nil && desc.fragmentFunction != nil
                        ? [(__bridge id<MTLDevice>)device
                              newRenderPipelineStateWithDescriptor:desc
                                                             error:&error]
                        : nil;
                if (pso != nil) slot = (__bridge_retained void*)pso;
            }
        }
        return slot;
    }
    /// Always pass, write depth.
    void* depth_state(void* device) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_depth_state == nullptr) {
            @autoreleasepool {
                MTLDepthStencilDescriptor* desc =
                    [[MTLDepthStencilDescriptor alloc] init];
                desc.depthCompareFunction = MTLCompareFunctionAlways;
                desc.depthWriteEnabled = YES;
                id<MTLDepthStencilState> state =
                    [(__bridge id<MTLDevice>)device
                        newDepthStencilStateWithDescriptor:desc];
                if (state != nil) {
                    m_depth_state = (__bridge_retained void*)state;
                }
            }
        }
        return m_depth_state;
    }

private:
    bool load(void* device) {
        if (m_library != nullptr) return true;
        if (m_failed) return false;
        @autoreleasepool {
            NSError* error = nil;
            id<MTLLibrary> library = [(__bridge id<MTLDevice>)device
                newLibraryWithSource:[NSString
                                         stringWithUTF8String:kTileMsaaSource]
                             options:nil
                               error:&error];
            if (library == nil) {
                m_failed = true;
                std::fprintf(stderr,
                             "[metal] tiled multisample copy shaders did not "
                             "compile: %s\n",
                             error ? error.localizedDescription.UTF8String
                                   : "unknown");
                return false;
            }
            m_library = (__bridge_retained void*)library;
        }
        return true;
    }
    std::mutex m_mutex;
    void* m_library{nullptr};
    bool m_failed{false};
    std::unordered_map<std::string, void*> m_compute;
    std::unordered_map<u64, void*> m_render;
    void* m_depth_state{nullptr};
};

}  // namespace

bool MetalTiled::copy_multisampled(void* texture, unsigned slice,
                                   u32 texel_bytes,
                                   bool depth16, unsigned x, unsigned y,
                                   unsigned width, unsigned height,
                                   void* buffer, usize buffer_offset,
                                   u64 offset, u64 row_pitch,
                                   bool to_texture) {
    // Plain compute and render work: no Metal 4 feature (ClearView
    // rectangles on a multisampled depth view use it too).
    void* device = MetalDevice::instance().device_ptr();
    if (device == nullptr || texture == nullptr || buffer == nullptr ||
        width == 0 || height == 0 || offset > UINT32_MAX ||
        row_pitch > UINT32_MAX) {
        return false;
    }
    auto& programs = TileMsaaPrograms::instance();
    auto& queue = MetalSubmitQueue::instance();
    @autoreleasepool {
        id<MTLTexture> tex = (__bridge id<MTLTexture>)texture;
        const MTLPixelFormat format = tex.pixelFormat;
        const bool depth = format == MTLPixelFormatDepth32Float ||
                           format == MTLPixelFormatDepth16Unorm;
        TileArgs args{};
        args.width = width;
        args.height = height;
        args.samples = static_cast<u32>(tex.sampleCount);
        args.bpp = texel_bytes;
        args.row = static_cast<u32>(row_pitch);
        args.stride = texel_bytes * args.samples;
        args.x0 = x;
        args.y0 = y;
        args.offset = static_cast<u32>(offset);
        args.depth16 = depth16 ? 1u : 0u;
        // One slice as a plain 2D multisample texture; colour through an
        // unsigned view of the texel's size, the bits verbatim (the op
        // retains the view).
        MTLPixelFormat view_format = format;
        if (!depth) {
            switch (texel_bytes) {
            case 1: view_format = MTLPixelFormatR8Uint; break;
            case 2: view_format = MTLPixelFormatR16Uint; break;
            case 4: view_format = MTLPixelFormatR32Uint; break;
            case 8: view_format = MTLPixelFormatRG32Uint; break;
            case 16: view_format = MTLPixelFormatRGBA32Uint; break;
            default: return false;
            }
        }
        if (slice >= tex.arrayLength) return false;
        id<MTLTexture> view = tex;
        if (!depth || tex.textureType != MTLTextureType2DMultisample) {
            view = [tex newTextureViewWithPixelFormat:view_format
                                          textureType:MTLTextureType2DMultisample
                                               levels:NSMakeRange(0, 1)
                                               slices:NSMakeRange(slice, 1)];
            if (view == nil) return false;
        }
        void* source = (__bridge void*)view;
        if (!to_texture) {
            void* pipeline = programs.compute(
                device, depth ? "tile_read_ms_depth" : "tile_read_ms");
            if (pipeline == nullptr) return false;
            CsCompute compute{};
            compute.pipeline = pipeline;
            compute.buffers[1] = buffer;
            compute.buffer_off[1] = buffer_offset;
            compute.textures[0] = source;
            compute.constants.resize(sizeof(args));
            std::memcpy(compute.constants.data(), &args, sizeof(args));
            compute.tx = 8;
            compute.ty = 8;
            compute.tz = 1;
            compute.gx = (width + 7u) / 8u;
            compute.gy = (height + 7u) / 8u;
            compute.gz = 1;
            queue.record_compute(std::move(compute));
            return true;
        }
        void* pipeline = programs.render(
            device, depth,
            static_cast<u32>(depth ? format : view.pixelFormat), args.samples);
        void* state = depth ? programs.depth_state(device) : nullptr;
        if (pipeline == nullptr || (depth && state == nullptr)) return false;
        CsBeginPass begin{};
        if (depth) {
            begin.depth = source;
        } else {
            begin.colors[0] = source;
            begin.color_count = 1;
        }
        begin.render_target_array_length = 1;  // preserve: the rest stays
        const u64 pass = queue.record_begin_pass(begin);
        CsDraw draw{};
        draw.pipeline = pipeline;
        CFRetain(pipeline);
        draw.depth_stencil = state;
        if (state != nullptr) CFRetain(state);
        draw.ps_const.resize(sizeof(args));
        std::memcpy(draw.ps_const.data(), &args, sizeof(args));
        draw.ps_cbuffer[0] = buffer;
        CFRetain(buffer);
        draw.ps_cbuffer_off[0] = buffer_offset;
        // The viewport is the box: the covering triangle rasterises its
        // pixels only.
        draw.viewport = {static_cast<float>(x), static_cast<float>(y),
                         static_cast<float>(width),
                         static_cast<float>(height), 0.0f, 1.0f};
        draw.primitive = MTLPrimitiveTypeTriangle;
        draw.vertex_count = 3;
        draw.instance_count = 1;
        queue.record_draw(std::move(draw), pass);
        queue.record_end_pass(pass);
    }
    return true;
}

MetalTiled& MetalTiled::instance() {
    static MetalTiled tiled;
    return tiled;
}

bool MetalTiled::available() {
    static std::once_flag once;
    std::call_once(once, [this] { init(); });
    return m_available;
}

void MetalTiled::init() {
    m_initialised = true;
    if (@available(macOS 26.0, *)) {
        auto& metal = MetalDevice::instance();
        id<MTLDevice> device = (__bridge id<MTLDevice>)metal.device_ptr();
        id<MTLCommandQueue> queue =
            (__bridge id<MTLCommandQueue>)metal.command_queue_ptr();
        if (device == nil || queue == nil) return;
        // Placement sparse resources and the Metal 4 queue that maps them;
        // a 64 KiB sparse page is the D3D tile.
        if (![device supportsFamily:MTLGPUFamilyMetal4] ||
            ![device respondsToSelector:@selector(newMTL4CommandQueue)] ||
            [device sparseTileSizeInBytesForSparsePageSize:
                        MTLSparsePageSize64] != BACKEND_TILE_BYTES) {
            return;
        }
        id<MTL4CommandQueue> queue4 = [device newMTL4CommandQueue];
        id<MTLSharedEvent> event = [device newSharedEvent];
        MTLResidencySetDescriptor* residency_desc =
            [MTLResidencySetDescriptor new];
        residency_desc.label = @"vitreo tile pools";
        NSError* error = nil;
        id<MTLResidencySet> residency =
            [device newResidencySetWithDescriptor:residency_desc error:&error];
        if (queue4 == nil || event == nil || residency == nil) return;
        [queue addResidencySet:residency];
        [queue4 addResidencySet:residency];
        m_device = (__bridge void*)device;
        m_queue4 = (__bridge_retained void*)queue4;
        m_event = (__bridge_retained void*)event;
        m_residency = (__bridge_retained void*)residency;
        m_available = true;
    }
}

bool MetalTiled::tile_shape(u32 mtl_pixel_format, unsigned samples,
                            unsigned& width, unsigned& height) {
    width = 0;
    height = 0;
    if (!available() || mtl_pixel_format == 0 || samples == 0) return false;
    const auto key = std::make_pair(mtl_pixel_format, samples);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_shapes.find(key);
        if (it != g_shapes.end()) {
            width = it->second.first;
            height = it->second.second;
            return width != 0;
        }
    }
    unsigned w = 0;
    unsigned h = 0;
    if (@available(macOS 26.0, *)) {
        @autoreleasepool {
            id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
            const MTLTextureType type = samples > 1
                                            ? MTLTextureType2DMultisample
                                            : MTLTextureType2D;
            const auto format = static_cast<MTLPixelFormat>(mtl_pixel_format);
            if (samples == 1 || [device supportsTextureSampleCount:samples]) {
                const MTLSize size = [device
                    sparseTileSizeWithTextureType:type
                                      pixelFormat:format
                                      sampleCount:samples
                                   sparsePageSize:MTLSparsePageSize64];
                if (size.width != 0 && size.height != 0 && size.depth == 1) {
                    // The shape is only an answer if such a texture exists:
                    // make one (it is address space, nothing is allocated).
                    MTLTextureDescriptor* desc = [MTLTextureDescriptor new];
                    desc.textureType = type;
                    desc.pixelFormat = format;
                    desc.width = size.width;
                    desc.height = size.height;
                    desc.sampleCount = samples;
                    desc.storageMode = MTLStorageModePrivate;
                    desc.usage = samples > 1 ? MTLTextureUsageRenderTarget
                                             : MTLTextureUsageShaderRead;
                    desc.placementSparsePageSize = MTLSparsePageSize64;
                    if ([device newTextureWithDescriptor:desc] != nil) {
                        w = static_cast<unsigned>(size.width);
                        h = static_cast<unsigned>(size.height);
                    }
                }
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_shapes[key] = {w, h};
    width = w;
    height = h;
    return w != 0;
}

void* MetalTiled::new_sparse_buffer(u64 bytes) {
    if (!available() || bytes == 0) return nullptr;
    const u64 rounded =
        (bytes + BACKEND_TILE_BYTES - 1) / BACKEND_TILE_BYTES *
        BACKEND_TILE_BYTES;
    if (@available(macOS 26.0, *)) {
        id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
        if (rounded > [device maxBufferLength]) return nullptr;
        id<MTLBuffer> buffer =
            [device newBufferWithLength:static_cast<NSUInteger>(rounded)
                                options:MTLResourceStorageModePrivate
                placementSparsePageSize:MTLSparsePageSize64];
        if (buffer == nil) return nullptr;
        buffer.label = @"vitreo tiled buffer";
        return (__bridge_retained void*)buffer;
    }
    return nullptr;
}

bool MetalTiled::texture_tail(void* texture, unsigned& first_packed_level,
                              unsigned& packed_tiles) {
    id<MTLTexture> tex = (__bridge id<MTLTexture>)texture;
    if (tex == nil || !tex.isSparse) return false;
    const NSUInteger mips = tex.mipmapLevelCount;
    const NSUInteger first = tex.firstMipmapInTail;
    const NSUInteger tail = tex.tailSizeInBytes;
    if (first >= mips || tail == 0) {
        first_packed_level = static_cast<unsigned>(mips);
        packed_tiles = 0;
        return true;
    }
    first_packed_level = static_cast<unsigned>(first);
    packed_tiles = static_cast<unsigned>(
        (tail + BACKEND_TILE_BYTES - 1) / BACKEND_TILE_BYTES);
    return true;
}

std::unique_ptr<BackendTilePool> MetalTiled::create_pool() {
    if (!available()) return nullptr;
    return std::make_unique<MetalTilePool>(m_device);
}

bool MetalTiled::self_aliased(const void* resource) {
    if (resource == nullptr ||
        m_self_aliased_count.load(std::memory_order_acquire) == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_self_aliased.contains(resource);
}

void MetalTiled::forget(const void* resource) {
    if (resource == nullptr ||
        m_self_aliased_count.load(std::memory_order_acquire) == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_self_aliased.erase(resource) != 0) {
        m_self_aliased_count.fetch_sub(1, std::memory_order_release);
    }
}

bool MetalTiled::record_update(void* resource, bool texture,
                               unsigned first_packed_level,
                               BackendTilePool* pool,
                               const BackendTileMapping* mappings,
                               usize count, bool self_aliased) {
    if (!available() || resource == nullptr) return false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const bool listed = g_self_aliased.contains(resource);
        if (self_aliased && !listed) {
            g_self_aliased.insert(resource);
            m_self_aliased_count.fetch_add(1, std::memory_order_release);
        } else if (!self_aliased && listed) {
            g_self_aliased.erase(resource);
            m_self_aliased_count.fetch_sub(1, std::memory_order_release);
        }
    }
    if (count == 0 || mappings == nullptr) return true;
    collect();
    auto batch = std::make_shared<MetalTileMappingBatch>();
    batch->resource = (__bridge id)resource;
    batch->texture = texture;
    std::unordered_map<void*, usize> group_of;
    auto* metal_pool = static_cast<MetalTilePool*>(pool);
    for (usize i = 0; i < count; ++i) {
        const BackendTileMapping& mapping = mappings[i];
        void* heap = nullptr;
        TileOp op{};
        if (mapping.pool_tile != BACKEND_TILE_UNMAPPED) {
            if (metal_pool == nullptr) return false;
            heap = metal_pool->heap_for(mapping.pool_tile, op.heap_offset);
            if (heap == nullptr) return false;  // not referenced
        }
        if (texture) {
            // Metal addresses the packed tail as tiles of the level
            // firstMipmapInTail, in a one-tile-high row
            // (MTL4UpdateSparseTextureMappingOperation::textureRegion).
            op.level = mapping.tile.packed ? first_packed_level
                                           : mapping.tile.level;
            op.slice = mapping.tile.slice;
            op.x = mapping.tile.x;
            op.y = mapping.tile.packed ? 0u : mapping.tile.y;
        } else {
            op.x = mapping.tile.x;
        }
        auto [it, inserted] = group_of.emplace(heap, batch->groups.size());
        if (inserted) {
            batch->groups.push_back({});
            batch->groups.back().heap = (__bridge id<MTLHeap>)heap;
        }
        batch->groups[it->second].ops.push_back(op);
    }
    if (batch->groups.empty()) return true;
    metal_record_tile_mapping(std::move(batch));
    return true;
}

void MetalTiled::encode(const std::shared_ptr<MetalTileMappingBatch>& batch,
                        u64 wait_value, u64 signal_value, u64 gpu_gen) {
    if (!batch || !m_available) return;
    if (@available(macOS 26.0, *)) {
        @autoreleasepool {
            id<MTL4CommandQueue> queue4 =
                (__bridge id<MTL4CommandQueue>)m_queue4;
            id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)m_event;
            [queue4 waitForEvent:event value:wait_value];
            for (const auto& group : batch->groups) {
                const MTLSparseTextureMappingMode mode =
                    group.heap != nil ? MTLSparseTextureMappingModeMap
                                      : MTLSparseTextureMappingModeUnmap;
                if (batch->texture) {
                    std::vector<MTL4UpdateSparseTextureMappingOperation> ops(
                        group.ops.size());
                    for (usize i = 0; i < ops.size(); ++i) {
                        const TileOp& op = group.ops[i];
                        ops[i].mode = mode;
                        ops[i].textureRegion = MTLRegionMake2D(op.x, op.y, 1, 1);
                        ops[i].textureLevel = op.level;
                        ops[i].textureSlice = op.slice;
                        ops[i].heapOffset = op.heap_offset;
                    }
                    [queue4 updateTextureMappings:(id<MTLTexture>)batch->resource
                                             heap:group.heap
                                       operations:ops.data()
                                            count:ops.size()];
                } else {
                    std::vector<MTL4UpdateSparseBufferMappingOperation> ops(
                        group.ops.size());
                    for (usize i = 0; i < ops.size(); ++i) {
                        const TileOp& op = group.ops[i];
                        ops[i].mode = mode;
                        ops[i].bufferRange = NSMakeRange(op.x, 1);
                        ops[i].heapOffset = op.heap_offset;
                    }
                    [queue4 updateBufferMappings:(id<MTLBuffer>)batch->resource
                                            heap:group.heap
                                      operations:ops.data()
                                           count:ops.size()];
                }
            }
            [queue4 signalEvent:event value:signal_value];
        }
    }
    // The Metal 4 queue does not retain what it maps: keep the resource and
    // the heaps until the generation that contains the update retires.
    std::lock_guard<std::mutex> lock(g_mutex);
    g_batches.emplace_back(batch, gpu_gen);
}

void MetalTiled::make_resident(void* heap) {
    if (@available(macOS 26.0, *)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        id<MTLResidencySet> residency =
            (__bridge id<MTLResidencySet>)m_residency;
        [residency addAllocation:(__bridge id<MTLHeap>)heap];
        [residency commit];
    }
}

void MetalTiled::release_after(void* object, u64 gpu_gen, bool resident) {
    if (object == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_parked.push_back({object, gpu_gen, resident});
    }
    collect();
}

void MetalTiled::collect() {
    const u64 done = MetalSubmitQueue::instance().completed_generation();
    std::vector<Parked> freed;
    std::vector<std::shared_ptr<MetalTileMappingBatch>> retired;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_parked.begin(); it != g_parked.end();) {
            if (it->gpu_gen <= done) {
                freed.push_back(*it);
                it = g_parked.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = g_batches.begin(); it != g_batches.end();) {
            if (it->second <= done) {
                retired.push_back(std::move(it->first));
                it = g_batches.erase(it);
            } else {
                ++it;
            }
        }
        if (@available(macOS 26.0, *)) {
            bool changed = false;
            id<MTLResidencySet> residency =
                (__bridge id<MTLResidencySet>)m_residency;
            for (const Parked& parked : freed) {
                if (parked.resident && residency != nil) {
                    [residency removeAllocation:(__bridge id<MTLHeap>)
                                                    parked.object];
                    changed = true;
                }
            }
            if (changed) [residency commit];
        }
    }
    for (const Parked& parked : freed) CFRelease(parked.object);
}

}  // namespace vitreo::graphics
