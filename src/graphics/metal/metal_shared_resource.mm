// SPDX-License-Identifier: LGPL-2.1-only
/// @file metal_shared_resource.mm
/// @brief Canonical stores, local resources and publication for shared
///        resources of every shape (see metal_shared_resource.hpp).

#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>

#include "graphics/metal/metal_shared_resource.hpp"

#include "graphics/contracts/format_converter.hpp"
#include "graphics/metal/metal_submit_queue.hpp"
#include "graphics/metal/metal_texture.hpp"

#include <libproc.h>
#include <sys/proc.h>
#include <os/os_sync_wait_on_address.h>
#include <sys/proc_info.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

// MetalSharedStore has a member named id(); inside its members the
// Objective-C type needs another name.
typedef id<MTLDevice> MetalDeviceObject;
typedef id<MTLBuffer> MetalBufferObject;

namespace vitreo::graphics {

/// The first page of every store. Zero-filled memory (a fresh IOSurface) is
/// the initial state: nothing pending, nothing published.
struct MetalSharedStore::Header {
    u32 magic;
    u32 layout;
    std::atomic<u64> pending;    ///< last version a process began publishing
    std::atomic<u64> published;  ///< highest version whose work completed
    std::atomic<u32> pending_pid;
    std::atomic<u32> wake;       ///< waiters park here (shared address)
    u64 payload_bytes;
};

namespace {

static_assert(std::atomic<u64>::is_always_lock_free,
              "the publication counters live in memory other processes map");

constexpr u32 kHeaderMagic = 0x48535356u;  // 'VSSH'
/// Header page and row pitch of the IOSurface: 16 KiB, the largest host page
/// size, so the payload starts page-aligned for newBufferWithBytesNoCopy on
/// every Mac.
constexpr usize kPage = 16384;
/// How long a consumer waits for another process's publication before it
/// proceeds with what the store holds (a producer that never completes its
/// work would otherwise stall every consumer forever).
constexpr auto kForeignWait = std::chrono::seconds(5);

CFStringRef store_record_key() { return CFSTR("VitreoSharedStore"); }

usize round_up(usize value, usize align) {
    return (value + align - 1) / align * align;
}

bool process_alive(u32 pid) {
    if (pid == 0) return false;
    proc_bsdinfo info{};
    const int n = ::proc_pidinfo(static_cast<pid_t>(pid), PROC_PIDTBSDINFO, 0,
                                 &info, static_cast<int>(sizeof(info)));
    if (n == static_cast<int>(sizeof(info))) return info.pbi_status != SZOMB;
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

void unimplemented_once(const std::string& what) {
    static std::mutex mutex;
    static std::unordered_set<std::string> logged;
    std::lock_guard lock(mutex);
    if (logged.insert(what).second) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED %s\n", what.c_str());
    }
}

/// Waits for submitted work off the recording threads, then publishes it.
/// One per process, never joined (as SharedKeyedMutex's worker).
class PublishWorker {
public:
    using Task = std::function<void()>;
    static PublishWorker& instance() {
        static PublishWorker* worker = new PublishWorker();
        return *worker;
    }
    void post(Task task) {
        {
            std::lock_guard lock(m_mutex);
            m_tasks.push_back(std::move(task));
        }
        m_ready.notify_one();
    }

private:
    PublishWorker() {
        std::thread([this] {
            for (;;) {
                Task task;
                {
                    std::unique_lock lock(m_mutex);
                    m_ready.wait(lock, [this] { return !m_tasks.empty(); });
                    task = std::move(m_tasks.front());
                    m_tasks.pop_front();
                }
                task();
            }
        }).detach();
    }
    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<Task> m_tasks;
};

/// Ops the registry itself records must not re-enter it.
thread_local int t_registry_depth = 0;

struct RegistryScope {
    RegistryScope() { ++t_registry_depth; }
    ~RegistryScope() { --t_registry_depth; }
};

/// Bytes of one texel of @p format in a store (a depth+stencil format is
/// stored as its two aspects, see shared_store_layout).
struct StoredFormat {
    u32 texel_bytes{0};   ///< uncompressed
    u32 block_bytes{0};   ///< block-compressed (4x4 blocks)
    u32 stencil_bytes{0}; ///< a second, stencil aspect
    u32 depth_option{0};  ///< MTLBlitOption of the first aspect
};

StoredFormat stored_format(PixelFormat format) {
    const FormatInfo info = format_table(format);
    StoredFormat out{};
    if (info.mtl_pixel_format == 0) return out;
    if (info.is_compressed) {
        out.block_bytes = compressed_block_bytes(format);
        return out;
    }
    switch (info.mtl_pixel_format) {
    case MTLPixelFormatDepth32Float_Stencil8:
    case MTLPixelFormatDepth24Unorm_Stencil8:
        out.texel_bytes = 4;
        out.stencil_bytes = 1;
        out.depth_option = MTLBlitOptionDepthFromDepthStencil;
        return out;
    case MTLPixelFormatDepth32Float:
        out.texel_bytes = 4;
        return out;
    case MTLPixelFormatDepth16Unorm:
        out.texel_bytes = 2;
        return out;
    case MTLPixelFormatStencil8:
        out.texel_bytes = 1;
        return out;
    default:
        out.texel_bytes = info.bytes_per_pixel;
        return out;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Record

std::vector<u8> SharedStoreRecord::encode() const {
    const u32 header[12] = {kMagic,       kVersion,
                            static_cast<u32>(kind), pixel_format,
                            width,        height,
                            depth,        mip_levels,
                            array_size,   sample_count,
                            flags,        static_cast<u32>(frontend.size())};
    std::vector<u8> bytes(sizeof(header) + frontend.size());
    std::memcpy(bytes.data(), header, sizeof(header));
    if (!frontend.empty()) {
        std::memcpy(bytes.data() + sizeof(header), frontend.data(),
                    frontend.size());
    }
    return bytes;
}

bool SharedStoreRecord::decode(const u8* bytes, usize size,
                               SharedStoreRecord& out) {
    u32 header[12] = {};
    if (bytes == nullptr || size < sizeof(header)) return false;
    std::memcpy(header, bytes, sizeof(header));
    if (header[0] != kMagic || header[1] != kVersion ||
        size != sizeof(header) + header[11]) {
        return false;
    }
    out.kind = static_cast<SharedStoreKind>(header[2]);
    out.pixel_format = header[3];
    out.width = header[4];
    out.height = header[5];
    out.depth = header[6];
    out.mip_levels = header[7];
    out.array_size = header[8];
    out.sample_count = header[9];
    out.flags = header[10];
    out.frontend.assign(bytes + sizeof(header), bytes + size);
    return true;
}

std::vector<SharedStoreRegion> shared_store_layout(
    const SharedStoreRecord& record, usize& payload_bytes) {
    std::vector<SharedStoreRegion> regions;
    payload_bytes = 0;
    if (record.kind == SharedStoreKind::Buffer ||
        record.kind == SharedStoreKind::Memory) {
        payload_bytes = record.width;
        return regions;
    }
    const auto format = static_cast<PixelFormat>(record.pixel_format);
    if (record.pixel_format >= static_cast<u32>(PixelFormat::Count)) {
        return regions;
    }
    const StoredFormat stored = stored_format(format);
    if (stored.texel_bytes == 0 && stored.block_bytes == 0) return regions;
    const bool planar = is_planar_format(format);
    const bool volume = record.kind == SharedStoreKind::Texture3D;
    const u32 slices = volume ? 1u : std::max(1u, record.array_size);
    const u32 samples = std::max(1u, record.sample_count);
    usize at = 0;
    const auto add = [&](u32 level, u32 slice, u32 sample, u32 w, u32 h,
                         u32 d, u32 element, bool blocks, u32 options,
                         u32 plane = 0) {
        SharedStoreRegion r{};
        r.level = level;
        r.slice = slice;
        r.sample = sample;
        r.plane = plane;
        r.width = w;
        r.height = h;
        r.depth = d;
        const u32 columns = blocks ? (w + 3u) / 4u : w;
        const u32 rows = blocks ? (h + 3u) / 4u : h;
        r.bytes_per_row = static_cast<usize>(columns) * element;
        r.bytes_per_image = r.bytes_per_row * rows;
        r.blit_options = options;
        r.offset = at;
        at = round_up(at + r.bytes_per_image * d, 256);
        regions.push_back(r);
    };
    // D3D subresource order: slice-major, then level; each sample and each
    // aspect of a subresource after the other.
    for (u32 slice = 0; slice < slices; ++slice) {
        for (u32 level = 0; level < std::max(1u, record.mip_levels);
             ++level) {
            const u32 w = std::max(1u, record.width >> level);
            const u32 h = std::max(1u, record.height >> level);
            const u32 d = volume ? std::max(1u, record.depth >> level) : 1u;
            for (u32 sample = 0; sample < samples; ++sample) {
                if (stored.block_bytes != 0) {
                    add(level, slice, sample, w, h, d, stored.block_bytes,
                        true, 0);
                    continue;
                }
                add(level, slice, sample, w, h, d, stored.texel_bytes, false,
                    stored.depth_option);
                if (planar) {
                    // The chroma plane: half size, twice the texel.
                    add(level, slice, sample, std::max(1u, w / 2u),
                        std::max(1u, h / 2u), d, 2u * stored.texel_bytes,
                        false, 0, 1);
                }
                if (stored.stencil_bytes != 0) {
                    add(level, slice, sample, w, h, d, stored.stencil_bytes,
                        false, MTLBlitOptionStencilFromDepthStencil);
                }
            }
        }
    }
    payload_bytes = at;
    return regions;
}

// ---------------------------------------------------------------------------
// Store

std::shared_ptr<MetalSharedStore> MetalSharedStore::create(
    void* device, const SharedStoreRecord& record, usize payload_bytes) {
    MetalDeviceObject mtl_device = (__bridge MetalDeviceObject)device;
    if (mtl_device == nil) return nullptr;
    const usize payload = round_up(std::max<usize>(payload_bytes, 1), kPage);
    const usize total = kPage + payload;
    @autoreleasepool {
        NSDictionary* properties = @{
            (__bridge NSString*)kIOSurfaceWidth : @(kPage),
            (__bridge NSString*)kIOSurfaceHeight : @(total / kPage),
            (__bridge NSString*)kIOSurfaceBytesPerElement : @1,
            (__bridge NSString*)kIOSurfaceBytesPerRow : @(kPage),
            (__bridge NSString*)kIOSurfaceAllocSize : @(total),
            // Stores are opened by their global id in any Vitreo host
            // process of the capsule, as zero-copy shared textures are.
            (__bridge NSString*)kIOSurfaceIsGlobal : @YES,
        };
        IOSurfaceRef surface =
            IOSurfaceCreate((__bridge CFDictionaryRef)properties);
        if (surface == nullptr) return nullptr;
        const std::vector<u8> encoded = record.encode();
        CFDataRef data = CFDataCreate(kCFAllocatorDefault, encoded.data(),
                                      static_cast<CFIndex>(encoded.size()));
        if (data == nullptr) {
            CFRelease(surface);
            return nullptr;
        }
        IOSurfaceSetValue(surface, store_record_key(), data);
        CFRelease(data);
        auto* base = static_cast<u8*>(IOSurfaceGetBaseAddress(surface));
        if (base == nullptr || IOSurfaceGetAllocSize(surface) < total) {
            CFRelease(surface);
            return nullptr;
        }
        MetalBufferObject buffer = [mtl_device
            newBufferWithBytesNoCopy:base + kPage
                              length:payload
                             options:MTLResourceStorageModeShared
                         deallocator:nil];
        if (buffer == nil) {
            CFRelease(surface);
            return nullptr;
        }
        auto store = std::shared_ptr<MetalSharedStore>(new MetalSharedStore());
        store->m_surface = surface;
        store->m_buffer = (__bridge_retained void*)buffer;
        store->m_header = new (base) Header{};
        store->m_header->magic = kHeaderMagic;
        store->m_header->layout = SharedStoreRecord::kVersion;
        store->m_header->payload_bytes = payload;
        store->m_payload = base + kPage;
        store->m_payload_size = payload;
        store->m_id = IOSurfaceGetID(surface);
        store->m_record = record;
        return store;
    }
}

std::shared_ptr<MetalSharedStore> MetalSharedStore::open(void* device,
                                                         u32 surface_id) {
    MetalDeviceObject mtl_device = (__bridge MetalDeviceObject)device;
    if (mtl_device == nil || surface_id == 0) return nullptr;
    @autoreleasepool {
        IOSurfaceRef surface = IOSurfaceLookup(surface_id);
        if (surface == nullptr) return nullptr;
        SharedStoreRecord record{};
        bool ok = false;
        if (CFTypeRef value = IOSurfaceCopyValue(surface, store_record_key())) {
            if (CFGetTypeID(value) == CFDataGetTypeID()) {
                CFDataRef data = static_cast<CFDataRef>(value);
                ok = SharedStoreRecord::decode(
                    CFDataGetBytePtr(data),
                    static_cast<usize>(CFDataGetLength(data)), record);
            }
            CFRelease(value);
        }
        auto* base = static_cast<u8*>(IOSurfaceGetBaseAddress(surface));
        auto* header = reinterpret_cast<Header*>(base);
        if (!ok || base == nullptr || header->magic != kHeaderMagic ||
            IOSurfaceGetAllocSize(surface) < kPage + header->payload_bytes) {
            CFRelease(surface);
            return nullptr;
        }
        const usize payload = header->payload_bytes;
        MetalBufferObject buffer = [mtl_device
            newBufferWithBytesNoCopy:base + kPage
                              length:payload
                             options:MTLResourceStorageModeShared
                         deallocator:nil];
        if (buffer == nil) {
            CFRelease(surface);
            return nullptr;
        }
        auto store = std::shared_ptr<MetalSharedStore>(new MetalSharedStore());
        store->m_surface = surface;
        store->m_buffer = (__bridge_retained void*)buffer;
        store->m_header = header;
        store->m_payload = base + kPage;
        store->m_payload_size = payload;
        store->m_id = surface_id;
        store->m_record = std::move(record);
        return store;
    }
}

MetalSharedStore::~MetalSharedStore() {
    // The buffer aliases the surface's pages: it goes first.
    if (m_buffer != nullptr) CFRelease(m_buffer);
    if (m_surface != nullptr) CFRelease(static_cast<IOSurfaceRef>(m_surface));
}

u64 MetalSharedStore::begin_publish() {
    const u64 version =
        m_header->pending.fetch_add(1, std::memory_order_acq_rel) + 1;
    m_header->pending_pid.store(static_cast<u32>(::getpid()),
                                std::memory_order_release);
    return version;
}

void MetalSharedStore::finish_publish(u64 version) {
    u64 seen = m_header->published.load(std::memory_order_acquire);
    while (seen < version &&
           !m_header->published.compare_exchange_weak(
               seen, version, std::memory_order_acq_rel)) {
    }
    m_header->wake.fetch_add(1, std::memory_order_acq_rel);
    (void)os_sync_wake_by_address_all(&m_header->wake, sizeof(u32),
                                      OS_SYNC_WAKE_BY_ADDRESS_SHARED);
}

u64 MetalSharedStore::published() const noexcept {
    return m_header->published.load(std::memory_order_acquire);
}

void MetalSharedStore::wait_foreign_pending() {
    const u32 self = static_cast<u32>(::getpid());
    const auto deadline = std::chrono::steady_clock::now() + kForeignWait;
    for (;;) {
        const u32 seen = m_header->wake.load(std::memory_order_acquire);
        const u64 pending = m_header->pending.load(std::memory_order_acquire);
        const u64 published =
            m_header->published.load(std::memory_order_acquire);
        if (published >= pending) return;
        const u32 pid = m_header->pending_pid.load(std::memory_order_acquire);
        // This process's own publication is ordered by its queue.
        if (pid == self) return;
        if (!process_alive(pid)) {
            // A publisher that exited completed nothing further; what it
            // left in the store is all there is.
            finish_publish(pending);
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            unimplemented_once(
                "d3d11!shared resource: another process never completed a "
                "publication; proceeding with the store's current contents");
            return;
        }
        (void)os_sync_wait_on_address_with_timeout(
            &m_header->wake, seen, sizeof(u32),
            OS_SYNC_WAIT_ON_ADDRESS_SHARED, OS_CLOCK_MACH_ABSOLUTE_TIME,
            10'000'000);
    }
}

// ---------------------------------------------------------------------------
// Local

MetalSharedLocal::MetalSharedLocal(void* device, SharedStoreRecord record)
    : m_device(device), m_record(std::move(record)) {
    m_regions = shared_store_layout(m_record, m_payload_bytes);
}

MetalSharedLocal::~MetalSharedLocal() {
    struct ViewRelease {
        void** views[2];
        ~ViewRelease() {
            for (void** view : views) {
                if (*view != nullptr) CFRelease(*view);
                *view = nullptr;
            }
        }
    } release_views{{&m_color_view, &m_stencil_view}};
    // Destruction is a synchronization point: what this process wrote last
    // reaches the store for every other opening.
    std::vector<std::pair<std::shared_ptr<MetalSharedStore>, u64>> versions;
    {
        RegistryScope scope;
        if (m_dirty && m_store) {
            const u64 version = push_locked();
            if (version != 0) versions.emplace_back(m_store, version);
        }
    }
    MetalSharedRegistry::instance().remove(this);
    if (!versions.empty()) {
        const u64 generation = MetalSubmitQueue::instance().submit();
        struct Completion final : GpuCompletion {
            explicit Completion(u64 g) : generation(g) {}
            bool wait() override {
                return MetalSubmitQueue::instance().wait_for_generation(
                    generation);
            }
            u64 generation;
        };
        MetalSharedRegistry::publish_after(
            std::make_shared<Completion>(generation), std::move(versions));
    }
}

std::shared_ptr<MetalSharedStore> MetalSharedLocal::store() const {
    auto& registry = MetalSharedRegistry::instance();
    std::lock_guard lock(registry.m_mutex);
    return m_store;
}

u32 MetalSharedLocal::export_store() {
    auto& registry = MetalSharedRegistry::instance();
    std::vector<std::pair<std::shared_ptr<MetalSharedStore>, u64>> versions;
    u32 id = 0;
    {
        std::lock_guard lock(registry.m_mutex);
        RegistryScope scope;
        if (m_store) return m_store->id();
        const bool texture_kind =
            m_record.kind == SharedStoreKind::Texture2D ||
            m_record.kind == SharedStoreKind::Texture3D;
        if (texture_kind && m_regions.empty()) return 0;
        m_store = MetalSharedStore::create(m_device, m_record,
                                           m_payload_bytes);
        if (!m_store) return 0;
        id = m_store->id();
        registry.bind_store(this, id);
        // The first export publishes the contents as they stand.
        m_dirty = true;
        const u64 version = push_locked();
        if (version != 0) versions.emplace_back(m_store, version);
    }
    if (!versions.empty()) {
        const u64 generation = MetalSubmitQueue::instance().submit();
        struct Completion final : GpuCompletion {
            explicit Completion(u64 g) : generation(g) {}
            bool wait() override {
                return MetalSubmitQueue::instance().wait_for_generation(
                    generation);
            }
            u64 generation;
        };
        MetalSharedRegistry::publish_after(
            std::make_shared<Completion>(generation), std::move(versions));
    }
    return id;
}

bool MetalSharedLocal::create_store() {
    auto& registry = MetalSharedRegistry::instance();
    std::lock_guard lock(registry.m_mutex);
    if (m_store) return true;
    m_store = MetalSharedStore::create(m_device, m_record, m_payload_bytes);
    if (!m_store) return false;
    registry.bind_store(this, m_store->id());
    return true;
}

void MetalSharedLocal::attach_store(std::shared_ptr<MetalSharedStore> store) {
    auto& registry = MetalSharedRegistry::instance();
    std::lock_guard lock(registry.m_mutex);
    m_store = std::move(store);
    if (m_store) registry.bind_store(this, m_store->id());
    // Version 0: the first acquire pulls whatever has been published.
    m_version = 0;
}

u64 MetalSharedLocal::push_locked() {
    if (!m_store) return 0;
    m_dirty = false;
    if (m_record.kind == SharedStoreKind::Texture2D ||
        m_record.kind == SharedStoreKind::Texture3D) {
        if (m_record.sample_count > 1) {
            if (!record_msaa_push()) return 0;
            const u64 version = m_store->begin_publish();
            m_version = version;
            return version;
        }
        auto& queue = MetalSubmitQueue::instance();
        void* buffer = m_store->payload_buffer();
        for (const auto& r : m_regions) {
            const auto& tex = r.plane == 0 ? texture : plane1;
            if (!tex || tex->handle() == nullptr) return 0;
            queue.record_download_texture(
                tex->handle(), r.level, r.slice, 0, 0, 0, r.width, r.height,
                r.depth, buffer, r.bytes_per_row,
                r.depth > 1 ? r.bytes_per_image : 0, r.offset,
                r.blit_options);
        }
    }
    // Buffers and memory ARE the store: publishing is the version alone.
    const u64 version = m_store->begin_publish();
    m_version = version;
    return version;
}

void MetalSharedLocal::pull_locked() {
    if (!m_store) return;
    const u64 published = m_store->published();
    if (published <= m_version) return;
    m_version = published;
    // Buffers and memory are the store itself: nothing moves, and writes of
    // this process still owed a publication stay owed.
    if (m_record.kind != SharedStoreKind::Texture2D &&
        m_record.kind != SharedStoreKind::Texture3D) {
        return;
    }
    // The pull replaces the whole local texture. Writes this process made
    // since its last push and another process's newer version were not
    // ordered against each other (an application race); the published
    // version wins.
    m_dirty = false;
    if (m_record.sample_count > 1) {
        (void)record_msaa_pull();
        return;
    }
    auto& queue = MetalSubmitQueue::instance();
    void* buffer = m_store->payload_buffer();
    for (const auto& r : m_regions) {
        const auto& tex = r.plane == 0 ? texture : plane1;
        if (!tex || tex->handle() == nullptr) return;
        queue.record_upload_texture(buffer, tex->handle(), r.bytes_per_row,
                                    r.depth > 1 ? r.bytes_per_image : 0,
                                    r.width, r.height, r.depth, r.level,
                                    r.slice, 0, 0, 0, r.blit_options,
                                    r.offset);
    }
}

// ---------------------------------------------------------------------------
// Multisampled contents

namespace {

/// Kernels and shaders that move every sample of a multisampled texture
/// between its local texture and a store: a blit cannot (Metal copies only
/// single-sample textures to and from buffers), so the push reads each
/// sample in a compute kernel and the pull renders each sample with a
/// per-sample fragment function ([[sample_id]]). Colour moves through an
/// unsigned-integer view of the same texel size (the bits verbatim); depth
/// through its float bits ([[depth(any)]]); stencil through the stencil
/// plane view and shader stencil export ([[stencil]]).
constexpr const char* kMsaaCopySource = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct MsArgs {
    uint width; uint height; uint samples; uint bpp;
    uint row; uint slice; uint pad0; uint pad1;
    uint offsets[32];
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
kernel void push_ms(texture2d_ms<uint, access::read> src [[texture(0)]],
                    constant MsArgs& a [[buffer(0)]],
                    device uchar* dst [[buffer(1)]],
                    uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= a.width || gid.y >= a.height) return;
    for (uint s = 0; s < a.samples; ++s)
        put(dst + a.offsets[s] + gid.y * a.row + gid.x * a.bpp,
            src.read(gid, s), a.bpp);
}
kernel void push_ms_array(texture2d_ms_array<uint, access::read> src [[texture(0)]],
                          constant MsArgs& a [[buffer(0)]],
                          device uchar* dst [[buffer(1)]],
                          uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= a.width || gid.y >= a.height) return;
    for (uint s = 0; s < a.samples; ++s)
        put(dst + a.offsets[s] + gid.y * a.row + gid.x * a.bpp,
            src.read(gid, a.slice, s), a.bpp);
}
kernel void push_ms_depth(depth2d_ms<float, access::read> src [[texture(0)]],
                          constant MsArgs& a [[buffer(0)]],
                          device uchar* dst [[buffer(1)]],
                          uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= a.width || gid.y >= a.height) return;
    for (uint s = 0; s < a.samples; ++s)
        put(dst + a.offsets[s] + gid.y * a.row + gid.x * 4,
            uint4(as_type<uint>(src.read(gid, s)), 0, 0, 0), 4);
}
kernel void push_ms_depth_array(depth2d_ms_array<float, access::read> src [[texture(0)]],
                                constant MsArgs& a [[buffer(0)]],
                                device uchar* dst [[buffer(1)]],
                                uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= a.width || gid.y >= a.height) return;
    for (uint s = 0; s < a.samples; ++s)
        put(dst + a.offsets[s] + gid.y * a.row + gid.x * 4,
            uint4(as_type<uint>(src.read(gid, a.slice, s)), 0, 0, 0), 4);
}
struct VOut { float4 pos [[position]]; };
vertex VOut pull_vs(uint vid [[vertex_id]]) {
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
static const device uchar* texel(VOut in, uint s, constant MsArgs& a,
                                 const device uchar* src) {
    uint2 xy = uint2(in.pos.xy);
    return src + a.offsets[s] + xy.y * a.row + xy.x * a.bpp;
}
fragment uint4 pull_ms(VOut in [[stage_in]], uint s [[sample_id]],
                       constant MsArgs& a [[buffer(0)]],
                       const device uchar* src [[buffer(2)]]) {
    return get(texel(in, s, a, src), a.bpp);
}
struct DepthOut { float depth [[depth(any)]]; };
fragment DepthOut pull_ms_depth(VOut in [[stage_in]], uint s [[sample_id]],
                                constant MsArgs& a [[buffer(0)]],
                                const device uchar* src [[buffer(2)]]) {
    DepthOut o;
    o.depth = as_type<float>(get(texel(in, s, a, src), 4).x);
    return o;
}
struct StencilOut { uint stencil [[stencil]]; };
fragment StencilOut pull_ms_stencil(VOut in [[stage_in]], uint s [[sample_id]],
                                    constant MsArgs& a [[buffer(0)]],
                                    const device uchar* src [[buffer(2)]]) {
    StencilOut o;
    o.stencil = get(texel(in, s, a, src), 1).x;
    return o;
}
)MSL";

struct MsArgs {
    u32 width, height, samples, bpp, row, slice, pad0, pad1;
    u32 offsets[32];
};

class MsaaCopyPrograms {
public:
    static MsaaCopyPrograms& instance() {
        static MsaaCopyPrograms* programs = new MsaaCopyPrograms();
        return *programs;
    }
    /// A retained compute pipeline for @p name, or null.
    void* compute(void* device, const char* name) {
        std::lock_guard lock(m_mutex);
        if (!load(device)) return nullptr;
        auto& slot = m_compute[name];
        if (slot == nullptr) {
            @autoreleasepool {
                id<MTLLibrary> library = (__bridge id<MTLLibrary>)m_library;
                id<MTLFunction> fn = [library
                    newFunctionWithName:[NSString stringWithUTF8String:name]];
                id<MTLDevice> dev = (__bridge id<MTLDevice>)device;
                NSError* error = nil;
                id<MTLComputePipelineState> pso =
                    fn ? [dev newComputePipelineStateWithFunction:fn
                                                            error:&error]
                       : nil;
                if (pso != nil) slot = (__bridge_retained void*)pso;
            }
        }
        return slot;
    }
    /// A retained render pipeline writing @p fragment into attachments of
    /// @p color / @p depth / @p stencil (MTLPixelFormat, 0 = none) at
    /// @p samples samples, or null.
    void* render(void* device, const char* fragment, u32 color, u32 depth,
                 u32 stencil, u32 samples) {
        std::lock_guard lock(m_mutex);
        if (!load(device)) return nullptr;
        const std::string key = std::string(fragment) + ":" +
                                std::to_string(color) + ":" +
                                std::to_string(depth) + ":" +
                                std::to_string(stencil) + ":" +
                                std::to_string(samples);
        auto& slot = m_render[key];
        if (slot == nullptr) {
            @autoreleasepool {
                id<MTLLibrary> library = (__bridge id<MTLLibrary>)m_library;
                MTLRenderPipelineDescriptor* desc =
                    [[MTLRenderPipelineDescriptor alloc] init];
                desc.vertexFunction = [library newFunctionWithName:@"pull_vs"];
                desc.fragmentFunction = [library
                    newFunctionWithName:[NSString
                                            stringWithUTF8String:fragment]];
                desc.rasterSampleCount = samples;
                if (color != 0) {
                    desc.colorAttachments[0].pixelFormat =
                        static_cast<MTLPixelFormat>(color);
                }
                desc.depthAttachmentPixelFormat =
                    static_cast<MTLPixelFormat>(depth);
                desc.stencilAttachmentPixelFormat =
                    static_cast<MTLPixelFormat>(stencil);
                id<MTLDevice> dev = (__bridge id<MTLDevice>)device;
                NSError* error = nil;
                id<MTLRenderPipelineState> pso =
                    desc.vertexFunction && desc.fragmentFunction
                        ? [dev newRenderPipelineStateWithDescriptor:desc
                                                              error:&error]
                        : nil;
                if (pso != nil) slot = (__bridge_retained void*)pso;
            }
        }
        return slot;
    }
    /// Depth-stencil states: always pass; write depth (@p stencil false) or
    /// replace stencil from the shader (@p stencil true).
    void* depth_stencil_state(void* device, bool stencil) {
        std::lock_guard lock(m_mutex);
        void*& slot = stencil ? m_stencil_state : m_depth_state;
        if (slot == nullptr) {
            @autoreleasepool {
                MTLDepthStencilDescriptor* desc =
                    [[MTLDepthStencilDescriptor alloc] init];
                desc.depthCompareFunction = MTLCompareFunctionAlways;
                desc.depthWriteEnabled = !stencil;
                if (stencil) {
                    MTLStencilDescriptor* face =
                        [[MTLStencilDescriptor alloc] init];
                    face.stencilCompareFunction = MTLCompareFunctionAlways;
                    face.depthStencilPassOperation =
                        MTLStencilOperationReplace;
                    face.depthFailureOperation = MTLStencilOperationReplace;
                    face.stencilFailureOperation = MTLStencilOperationReplace;
                    face.readMask = 0xFF;
                    face.writeMask = 0xFF;
                    desc.frontFaceStencil = face;
                    desc.backFaceStencil = face;
                }
                id<MTLDevice> dev = (__bridge id<MTLDevice>)device;
                id<MTLDepthStencilState> state =
                    [dev newDepthStencilStateWithDescriptor:desc];
                if (state != nil) slot = (__bridge_retained void*)state;
            }
        }
        return slot;
    }

private:
    bool load(void* device) {
        if (m_library != nullptr) return true;
        if (m_failed) return false;
        @autoreleasepool {
            id<MTLDevice> dev = (__bridge id<MTLDevice>)device;
            NSError* error = nil;
            id<MTLLibrary> library = [dev
                newLibraryWithSource:[NSString
                                         stringWithUTF8String:kMsaaCopySource]
                             options:nil
                               error:&error];
            if (library == nil) {
                m_failed = true;
                std::fprintf(stderr,
                             "[metal] shared multisample copy shaders did not "
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
    std::unordered_map<std::string, void*> m_render;
    void* m_depth_state{nullptr};
    void* m_stencil_state{nullptr};
};

void retain_handle(void* handle) {
    if (handle != nullptr) CFRetain(handle);
}

/// The regions of a multisampled store grouped by (slice, aspect): the
/// per-sample offsets one kernel invocation or one draw needs.
struct MsaaGroup {
    u32 slice{0};
    u32 options{0};  ///< MTLBlitOption of the aspect (stencil or not)
    u32 width{0};
    u32 height{0};
    usize row{0};
    u32 bpp{0};
    std::vector<usize> offsets;
};

std::vector<MsaaGroup> msaa_groups(const std::vector<SharedStoreRegion>& regions) {
    std::vector<MsaaGroup> groups;
    for (const auto& r : regions) {
        MsaaGroup* group = nullptr;
        for (auto& g : groups) {
            if (g.slice == r.slice && g.options == r.blit_options) group = &g;
        }
        if (group == nullptr) {
            groups.push_back({});
            group = &groups.back();
            group->slice = r.slice;
            group->options = r.blit_options;
            group->width = r.width;
            group->height = r.height;
            group->row = r.bytes_per_row;
            group->bpp = r.width != 0
                             ? static_cast<u32>(r.bytes_per_row / r.width)
                             : 0u;
        }
        group->offsets.push_back(r.offset);
    }
    return groups;
}

bool msaa_args(const MsaaGroup& group, MsArgs& args) {
    if (group.offsets.size() > std::size(args.offsets)) return false;
    args = {};
    args.width = group.width;
    args.height = group.height;
    args.samples = static_cast<u32>(group.offsets.size());
    args.bpp = group.bpp;
    args.row = static_cast<u32>(group.row);
    args.slice = group.slice;
    for (usize i = 0; i < group.offsets.size(); ++i) {
        if (group.offsets[i] > UINT32_MAX) return false;
        args.offsets[i] = static_cast<u32>(group.offsets[i]);
    }
    return true;
}

}  // namespace

void* MetalSharedLocal::msaa_view(bool stencil) {
    void*& slot = stencil ? m_stencil_view : m_color_view;
    if (slot != nullptr) return slot;
    if (!texture || texture->handle() == nullptr) return nullptr;
    @autoreleasepool {
        id<MTLTexture> tex = (__bridge id<MTLTexture>)texture->handle();
        MTLPixelFormat format = MTLPixelFormatInvalid;
        if (stencil) {
            format = MTLPixelFormatX32_Stencil8;
        } else {
            switch (stored_format(static_cast<PixelFormat>(
                        m_record.pixel_format)).texel_bytes) {
            case 1: format = MTLPixelFormatR8Uint; break;
            case 2: format = MTLPixelFormatR16Uint; break;
            case 4: format = MTLPixelFormatR32Uint; break;
            case 8: format = MTLPixelFormatRG32Uint; break;
            case 16: format = MTLPixelFormatRGBA32Uint; break;
            default: return nullptr;
            }
        }
        id<MTLTexture> view =
            [tex newTextureViewWithPixelFormat:format
                                   textureType:tex.textureType
                                        levels:NSMakeRange(0, 1)
                                        slices:NSMakeRange(0, tex.arrayLength)];
        if (view == nil) return nullptr;
        slot = (__bridge_retained void*)view;
    }
    return slot;
}

bool MetalSharedLocal::record_msaa_push() {
    if (!texture || texture->handle() == nullptr || !m_store) return false;
    const FormatInfo info =
        format_table(static_cast<PixelFormat>(m_record.pixel_format));
    const bool depth_format = info.is_depth;
    const bool array = m_record.array_size > 1;
    auto& programs = MsaaCopyPrograms::instance();
    auto& queue = MetalSubmitQueue::instance();
    for (const MsaaGroup& group : msaa_groups(m_regions)) {
        const bool stencil_aspect =
            group.options == MTLBlitOptionStencilFromDepthStencil;
        const bool depth_aspect = depth_format && !stencil_aspect;
        void* source = depth_aspect ? texture->handle()
                                    : msaa_view(stencil_aspect);
        const char* kernel =
            depth_aspect ? (array ? "push_ms_depth_array" : "push_ms_depth")
                         : (array ? "push_ms_array" : "push_ms");
        void* pipeline = programs.compute(m_device, kernel);
        MsArgs args{};
        if (source == nullptr || pipeline == nullptr ||
            !msaa_args(group, args)) {
            unimplemented_once(
                "d3d11!shared resource: multisampled contents of this format "
                "across processes");
            return false;
        }
        CsCompute compute{};
        compute.pipeline = pipeline;
        compute.buffers[1] = m_store->payload_buffer();
        compute.textures[0] = source;
        compute.constants.resize(sizeof(args));
        std::memcpy(compute.constants.data(), &args, sizeof(args));
        compute.tx = 8;
        compute.ty = 8;
        compute.tz = 1;
        compute.gx = (group.width + 7u) / 8u;
        compute.gy = (group.height + 7u) / 8u;
        compute.gz = 1;
        queue.record_compute(std::move(compute));
    }
    return true;
}

bool MetalSharedLocal::record_msaa_pull() {
    if (!texture || texture->handle() == nullptr || !m_store) return false;
    const FormatInfo info =
        format_table(static_cast<PixelFormat>(m_record.pixel_format));
    const u32 samples = std::max(1u, m_record.sample_count);
    const bool combined = info.mtl_pixel_format ==
                              MTLPixelFormatDepth32Float_Stencil8 ||
                          info.mtl_pixel_format ==
                              MTLPixelFormatDepth24Unorm_Stencil8;
    auto& programs = MsaaCopyPrograms::instance();
    auto& queue = MetalSubmitQueue::instance();
    const auto groups = msaa_groups(m_regions);
    for (const MsaaGroup& group : groups) {
        const bool stencil_aspect =
            group.options == MTLBlitOptionStencilFromDepthStencil;
        const bool depth_aspect = info.is_depth && !stencil_aspect;
        MsArgs args{};
        if (!msaa_args(group, args)) return false;
        CsBeginPass begin{};
        void* pipeline = nullptr;
        void* state = nullptr;
        if (info.is_depth) {
            begin.depth = texture->handle();
            begin.depth_slice = group.slice;
            begin.stencil = combined;
            begin.render_target_array_length = 1;
            const u32 stencil_format = combined ? info.mtl_pixel_format : 0u;
            pipeline = programs.render(
                m_device, depth_aspect ? "pull_ms_depth" : "pull_ms_stencil",
                0, info.mtl_pixel_format, stencil_format, samples);
            state = programs.depth_stencil_state(m_device, stencil_aspect);
        } else {
            void* view = msaa_view(false);
            if (view == nullptr) return false;
            begin.colors[0] = view;
            begin.color_count = 1;
            begin.color_slices[0] = group.slice;
            begin.render_target_array_length = 1;
            const u32 view_format = static_cast<u32>(
                ((__bridge id<MTLTexture>)view).pixelFormat);
            pipeline = programs.render(m_device, "pull_ms", view_format, 0,
                                       0, samples);
        }
        if (pipeline == nullptr) {
            unimplemented_once(
                "d3d11!shared resource: multisampled contents of this format "
                "across processes");
            return false;
        }
        // record_begin_pass retains the attachments itself.
        const u64 pass = queue.record_begin_pass(begin);
        CsDraw draw{};
        draw.pipeline = pipeline;
        retain_handle(pipeline);
        draw.depth_stencil = state;
        retain_handle(state);
        draw.ps_const.resize(sizeof(args));
        std::memcpy(draw.ps_const.data(), &args, sizeof(args));
        draw.ps_cbuffer[0] = m_store->payload_buffer();
        retain_handle(draw.ps_cbuffer[0]);
        draw.viewport = {0.0f, 0.0f, static_cast<float>(group.width),
                         static_cast<float>(group.height), 0.0f, 1.0f};
        draw.primitive = MTLPrimitiveTypeTriangle;
        draw.vertex_count = 3;
        draw.instance_count = 1;
        queue.record_draw(std::move(draw), pass);
        queue.record_end_pass(pass);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Registry

MetalSharedRegistry& MetalSharedRegistry::instance() {
    static MetalSharedRegistry* registry = new MetalSharedRegistry();
    return *registry;
}

void MetalSharedRegistry::add(const std::shared_ptr<MetalSharedLocal>& local,
                              void* handle) {
    std::lock_guard lock(m_mutex);
    if (handle == nullptr) return;
    m_by_handle[handle] = local;
    m_handles_of[local.get()].push_back(handle);
}

void MetalSharedRegistry::remove(MetalSharedLocal* local) {
    std::lock_guard lock(m_mutex);
    if (const auto handles = m_handles_of.find(local);
        handles != m_handles_of.end()) {
        for (void* handle : handles->second) {
            const auto found = m_by_handle.find(handle);
            if (found == m_by_handle.end()) continue;
            const auto held = found->second.lock();
            if (held == nullptr || held.get() == local) {
                m_by_handle.erase(found);
            }
        }
        m_handles_of.erase(handles);
    }
    if (const auto store = m_store_of.find(local); store != m_store_of.end()) {
        const auto found = m_by_store.find(store->second);
        if (found != m_by_store.end()) {
            const auto held = found->second.lock();
            if (held == nullptr || held.get() == local) m_by_store.erase(found);
        }
        m_store_of.erase(store);
    }
    if (m_stored.erase(local) != 0) {
        m_live.store(static_cast<u32>(m_stored.size()),
                     std::memory_order_relaxed);
    }
}

void MetalSharedRegistry::bind_store(MetalSharedLocal* local, u32 store_id) {
    // Every opening in this process must find this local, never open the
    // store a second time: a second MTLBuffer over the same pages is an
    // alias Metal's hazard tracking cannot see, so work on the two would
    // not be ordered (measured: a same-process opening's first copy read the
    // creator's update before it had executed).
    std::lock_guard lock(m_mutex);
    std::weak_ptr<MetalSharedLocal> weak = local->weak_from_this();
    if (weak.expired()) return;
    // Only resources with a store take part in publication: one nobody has
    // exported yet has no reader to publish to.
    m_stored[local] = weak;
    m_live.store(static_cast<u32>(m_stored.size()), std::memory_order_relaxed);
    m_store_of[local] = store_id;
    m_by_store[store_id] = std::move(weak);
}

std::vector<std::shared_ptr<MetalSharedLocal>>
MetalSharedRegistry::stored_locked() {
    std::vector<std::shared_ptr<MetalSharedLocal>> locals;
    locals.reserve(m_stored.size());
    for (const auto& [raw, weak] : m_stored) {
        (void)raw;
        if (auto local = weak.lock()) locals.push_back(std::move(local));
    }
    return locals;
}

std::shared_ptr<MetalSharedLocal> MetalSharedRegistry::find_store(u32 id) {
    std::lock_guard lock(m_mutex);
    const auto found = m_by_store.find(id);
    return found == m_by_store.end() ? nullptr : found->second.lock();
}

MetalSharedLocal* MetalSharedRegistry::local_for_texture_locked(
    void* handle) {
    for (int depth = 0; handle != nullptr && depth < 4; ++depth) {
        const auto found = m_by_handle.find(handle);
        if (found != m_by_handle.end()) {
            auto local = found->second.lock();
            return local.get();
        }
        // A view (render-target / UAV / format cast) writes its parent. A
        // buffer (or a buffer's texture alias) is not a view of a texture.
        id object = (__bridge id)handle;
        if (![object conformsToProtocol:@protocol(MTLTexture)]) return nullptr;
        id<MTLTexture> texture = (id<MTLTexture>)object;
        handle = (__bridge void*)texture.parentTexture;
    }
    return nullptr;
}

void MetalSharedRegistry::note_written_texture(void* handle) {
    // The registry's own pushes and pulls write the store and the local
    // texture to bring them level; they are not writes of the application.
    if (empty() || handle == nullptr || t_registry_depth != 0) return;
    std::lock_guard lock(m_mutex);
    if (MetalSharedLocal* local = local_for_texture_locked(handle)) {
        local->mark_dirty();
    }
}

void MetalSharedRegistry::note_written_buffer(void* handle) {
    if (empty() || handle == nullptr || t_registry_depth != 0) return;
    std::lock_guard lock(m_mutex);
    const auto found = m_by_handle.find(handle);
    if (found == m_by_handle.end()) return;
    if (auto local = found->second.lock()) local->mark_dirty();
}

void MetalSharedRegistry::note_written_resource(void* handle) {
    // A resource reached through an argument buffer (descriptor table): a
    // texture, a view of one, or a buffer.
    note_written_texture(handle);
}

void MetalSharedRegistry::acquire() {
    if (empty() || t_registry_depth != 0) return;
    RegistryScope scope;
    // Strong references outlive the lock: a local whose last owner lets go
    // meanwhile is destroyed after it, not while the maps are walked.
    std::vector<std::shared_ptr<MetalSharedLocal>> locals;
    std::lock_guard lock(m_mutex);
    locals = stored_locked();
    for (const auto& local : locals) {
        if (!local->m_store) continue;
        local->m_store->wait_foreign_pending();
        local->pull_locked();
    }
}

std::vector<std::pair<std::shared_ptr<MetalSharedStore>, u64>>
MetalSharedRegistry::push_dirty() {
    std::vector<std::pair<std::shared_ptr<MetalSharedStore>, u64>> versions;
    if (empty()) return versions;
    RegistryScope scope;
    std::vector<std::shared_ptr<MetalSharedLocal>> locals;
    std::lock_guard lock(m_mutex);
    locals = stored_locked();
    for (const auto& local : locals) {
        if (!local->m_dirty || !local->m_store) continue;
        const u64 version = local->push_locked();
        if (version != 0) versions.emplace_back(local->m_store, version);
    }
    return versions;
}

void MetalSharedRegistry::publish_after(
    std::shared_ptr<GpuCompletion> completion,
    std::vector<std::pair<std::shared_ptr<MetalSharedStore>, u64>> versions) {
    if (versions.empty()) return;
    if (!completion) {
        for (auto& [store, version] : versions) store->finish_publish(version);
        return;
    }
    PublishWorker::instance().post(
        [completion = std::move(completion),
         versions = std::move(versions)] {
            (void)completion->wait();
            for (const auto& [store, version] : versions) {
                store->finish_publish(version);
            }
        });
}

// ---------------------------------------------------------------------------
// Shared CPU memory

MetalSharedMemory::MetalSharedMemory(std::shared_ptr<MetalSharedLocal> local,
                                     std::vector<u8> metadata)
    : m_local(std::move(local)), m_metadata(std::move(metadata)) {}

u64 MetalSharedMemory::shared_handle() const noexcept {
    const auto store = m_local->store();
    return store ? store->id() : 0u;
}

u8* MetalSharedMemory::data() noexcept {
    const auto store = m_local->store();
    return store ? store->payload() : nullptr;
}

usize MetalSharedMemory::size() const noexcept {
    return m_local->record().width;
}

bool MetalSharedMemory::acquire() {
    const auto store = m_local->store();
    if (!store) return false;
    store->wait_foreign_pending();
    return true;
}

void MetalSharedMemory::note_written() {
    auto& registry = MetalSharedRegistry::instance();
    std::lock_guard lock(registry.m_mutex);
    m_local->mark_dirty();
}

}  // namespace vitreo::graphics
