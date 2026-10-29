// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file metal_shared_resource.hpp
/// @brief Shared resources of every shape: a canonical store in IOSurface
///        memory, per-process local Metal resources, and the publication
///        protocol that moves writes between them.
///
/// A single-image colour texture is shared zero-copy: its Metal texture
/// lives on an IOSurface every process opens (metal_texture.mm). Every other
/// shared resource -- mipmapped, array, cube, multisampled, block-compressed
/// or depth textures, volumes, buffers, STAGING memory -- has no IOSurface
/// form (an IOSurface is one image; measured, at most 16-63 planes), so:
///
///  * its CANONICAL STORE is one IOSurface used as plain memory: a header
///    page (the publication counters every process sees) and the payload
///    holding every subresource at offsets both sides compute from the
///    resource's description (shared_store_layout);
///  * each process keeps a LOCAL Metal resource (full mips / slices /
///    samples) shared by every opening in that process -- all of a process's
///    devices record into one queue, so they need no copies between them;
///  * the producer PUSHES the local resource into the store at its
///    synchronization points (Flush, Present, ReleaseSync, destruction, the
///    first export) as GPU blits on its queue, and PUBLISHES a new version
///    once that work has completed; a consumer PULLS a newer version into its
///    local resource with GPU blits recorded before its next operation.
///
/// Buffers and shared CPU memory need no copies at all: their local storage
/// IS the store's memory (an MTLBuffer over the IOSurface pages), and only
/// the publication counters order the processes.
///
/// D3D11's visibility rule is the contract being implemented (Learn,
/// ID3D11Device::OpenSharedResource: "If a shared texture is updated on one
/// device ID3D11DeviceContext::Flush must be called on that device";
/// D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX: every device "must call
/// IDXGIKeyedMutex::AcquireSync before they issue any rendering commands to
/// the surface. When those devices finish rendering, they must call
/// IDXGIKeyedMutex::ReleaseSync"). Measured on Windows
/// (tools/oracle_probe/d3d11_share_ers_probe): every such shape round-trips
/// between two devices and two processes in both directions.

#include <vitreo/common.hpp>
#include "graphics/contracts/backend_device.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vitreo::graphics {

class MetalTexture;

/// What a store holds.
enum class SharedStoreKind : u32 {
    Texture2D = 1,
    Texture3D = 2,
    Buffer = 3,
    Memory = 4,
};

/// The backend description every opener rebuilds a store's resource from,
/// plus the creating API's own bytes (opaque here).
struct SharedStoreRecord {
    static constexpr u32 kMagic = 0x53535356u;  // 'VSSS'
    static constexpr u32 kVersion = 1u;
    SharedStoreKind kind{SharedStoreKind::Memory};
    u32 pixel_format{0};  ///< graphics::PixelFormat
    u32 width{0};         ///< texels, or bytes for Buffer / Memory
    u32 height{1};
    u32 depth{1};
    u32 mip_levels{1};
    u32 array_size{1};    ///< slices; a cube counts faces
    u32 sample_count{1};
    u32 flags{0};
    std::vector<u8> frontend;

    static constexpr u32 kCube = 1u;
    static constexpr u32 kRenderTarget = 2u;
    static constexpr u32 kShaderResource = 4u;
    static constexpr u32 kUnorderedAccess = 8u;
    static constexpr u32 kDepthStencil = 16u;
    static constexpr u32 kPixelFormatView = 32u;
    static constexpr u32 kVertexBuffer = 64u;
    static constexpr u32 kIndexBuffer = 128u;
    static constexpr u32 kConstantBuffer = 256u;
    static constexpr u32 kStreamOutput = 512u;
    static constexpr u32 kCpuWritable = 1024u;

    [[nodiscard]] std::vector<u8> encode() const;
    [[nodiscard]] static bool decode(const u8* bytes, usize size,
                                     SharedStoreRecord& out);
};

/// One subresource (or one sample / aspect of it) in a store's payload.
struct SharedStoreRegion {
    u32 level{0};
    u32 slice{0};   ///< array slice / cube face (0 for a volume)
    u32 sample{0};
    /// Which local texture: 0, or 1 for the chroma plane of a planar video
    /// format (NV12 / P010).
    u32 plane{0};
    u32 width{1};   ///< texels
    u32 height{1};
    u32 depth{1};
    usize offset{0};
    usize bytes_per_row{0};
    usize bytes_per_image{0};
    /// MTLBlitOption: the depth or stencil half of a combined format.
    u32 blit_options{0};
};

/// The payload layout of a texture store: every level of every slice,
/// sample and aspect, 256-byte aligned. Both the producer and every opener
/// compute it from the same record, so it is never stored. Empty when the
/// format has no copyable form.
[[nodiscard]] std::vector<SharedStoreRegion> shared_store_layout(
    const SharedStoreRecord& record, usize& payload_bytes);

/// One canonical store: an IOSurface every process maps as memory.
class MetalSharedStore {
public:
    struct Header;

    [[nodiscard]] static std::shared_ptr<MetalSharedStore> create(
        void* device, const SharedStoreRecord& record, usize payload_bytes);
    /// The store the global id names, or null when it names no store (a
    /// zero-copy shared texture's surface, or nothing).
    [[nodiscard]] static std::shared_ptr<MetalSharedStore> open(
        void* device, u32 surface_id);
    ~MetalSharedStore();
    MetalSharedStore(const MetalSharedStore&) = delete;
    MetalSharedStore& operator=(const MetalSharedStore&) = delete;

    [[nodiscard]] u32 id() const noexcept { return m_id; }
    [[nodiscard]] const SharedStoreRecord& record() const noexcept {
        return m_record;
    }
    [[nodiscard]] u8* payload() const noexcept { return m_payload; }
    [[nodiscard]] usize payload_size() const noexcept {
        return m_payload_size;
    }
    /// id<MTLBuffer> over the payload (offset 0 = payload byte 0).
    [[nodiscard]] void* payload_buffer() const noexcept { return m_buffer; }

    /// Publication. begin_publish() claims the next version for work this
    /// process has just submitted; finish_publish() makes it visible once
    /// that work has completed.
    [[nodiscard]] u64 begin_publish();
    void finish_publish(u64 version);
    [[nodiscard]] u64 published() const noexcept;
    /// Wait (bounded) until every publication another process has begun has
    /// completed. This process's own publications are ordered by its queue.
    void wait_foreign_pending();

private:
    MetalSharedStore() = default;

    void* m_surface{nullptr};  // IOSurfaceRef
    void* m_buffer{nullptr};   // id<MTLBuffer>, retained
    Header* m_header{nullptr};
    u8* m_payload{nullptr};
    usize m_payload_size{0};
    u32 m_id{0};
    SharedStoreRecord m_record;
};

/// This process's side of one mirrored or memory-backed shared resource,
/// shared by every opening of it in the process.
class MetalSharedLocal
    : public std::enable_shared_from_this<MetalSharedLocal> {
public:
    MetalSharedLocal(void* device, SharedStoreRecord record);
    ~MetalSharedLocal();
    MetalSharedLocal(const MetalSharedLocal&) = delete;
    MetalSharedLocal& operator=(const MetalSharedLocal&) = delete;

    [[nodiscard]] const SharedStoreRecord& record() const noexcept {
        return m_record;
    }
    /// The local Metal texture (texture kinds); `plane1` is the chroma plane
    /// of a planar video format.
    std::shared_ptr<MetalTexture> texture;
    std::shared_ptr<MetalTexture> plane1;
    /// Buffer / Memory kinds: the store's payload buffer, which IS the local
    /// storage (set by attach_store / export_store).
    /// The store, once exported (the creator allocates it lazily: a texture
    /// nobody ever takes a handle to never pays for one) or opened.
    [[nodiscard]] std::shared_ptr<MetalSharedStore> store() const;
    /// Allocate the store (creator) and publish the current contents.
    /// Returns the store id, 0 on failure.
    [[nodiscard]] u32 export_store();
    /// Attach an opened store (another process's): the next acquire pulls.
    void attach_store(std::shared_ptr<MetalSharedStore> store);
    /// Buffers and CPU memory: allocate the store at creation (it IS their
    /// storage). False on failure.
    [[nodiscard]] bool create_store();

    /// Called by the registry, under its lock.
    void mark_dirty() noexcept { m_dirty = true; }
    [[nodiscard]] bool dirty() const noexcept { return m_dirty; }
    /// Record the push of the local texture into the store and claim a
    /// version. Returns 0 when nothing was pushed.
    [[nodiscard]] u64 push_locked();
    /// Pull a newer published version into the local texture.
    void pull_locked();

private:
    /// Multisampled textures cannot be blitted to or from memory: every
    /// sample moves through a compute read (push) or a per-sample render
    /// (pull). False when this device cannot (logged once).
    [[nodiscard]] bool record_msaa_push();
    [[nodiscard]] bool record_msaa_pull();
    /// A cached view of the local texture: an unsigned-integer view of the
    /// same texel size (colour), or the stencil plane (X32_Stencil8).
    [[nodiscard]] void* msaa_view(bool stencil);

    void* m_device{nullptr};
    SharedStoreRecord m_record;
    std::shared_ptr<MetalSharedStore> m_store;
    void* m_color_view{nullptr};    // retained id<MTLTexture>
    void* m_stencil_view{nullptr};  // retained id<MTLTexture>
    std::vector<SharedStoreRegion> m_regions;
    usize m_payload_bytes{0};
    bool m_dirty{false};
    /// The store version the local texture holds.
    u64 m_version{0};
    friend class MetalSharedRegistry;
};

/// Every live MetalSharedLocal of the process, by store id and by the Metal
/// objects the submit queue sees, so writes recorded against them mark them
/// dirty and newer published versions are pulled before the next operation.
class MetalSharedRegistry {
public:
    static MetalSharedRegistry& instance();

    void add(const std::shared_ptr<MetalSharedLocal>& local, void* handle);
    void remove(MetalSharedLocal* local);
    void bind_store(MetalSharedLocal* local, u32 store_id);
    [[nodiscard]] std::shared_ptr<MetalSharedLocal> find_store(u32 id);

    /// Fast path: no exported or opened shared resource lives in the
    /// process (a shareable texture nobody took a handle to does not count:
    /// its first export publishes its contents as they stand).
    [[nodiscard]] bool empty() const noexcept {
        return m_live.load(std::memory_order_relaxed) == 0;
    }
    /// A recorded operation writes this texture / buffer handle.
    void note_written_texture(void* handle);
    void note_written_buffer(void* handle);
    /// A texture, texture view or buffer (argument-buffer residency).
    void note_written_resource(void* handle);
    /// Pull every newer published version (MetalDevice::
    /// acquire_shared_writes, and before each recorded operation).
    void acquire();
    /// Push every dirty local; returns the stores and the versions claimed,
    /// for publication once the submission completes.
    [[nodiscard]] std::vector<std::pair<std::shared_ptr<MetalSharedStore>, u64>>
    push_dirty();
    /// Publish @p versions once @p completion has completed (a worker
    /// thread waits; the caller does not).
    static void publish_after(
        std::shared_ptr<GpuCompletion> completion,
        std::vector<std::pair<std::shared_ptr<MetalSharedStore>, u64>>
            versions);

private:
    friend class MetalSharedLocal;
    friend class MetalSharedMemory;
    MetalSharedRegistry() = default;
    [[nodiscard]] MetalSharedLocal* local_for_texture_locked(void* handle);

    /// Strong references to every local with a store (under m_mutex).
    [[nodiscard]] std::vector<std::shared_ptr<MetalSharedLocal>>
    stored_locked();

    std::recursive_mutex m_mutex;
    std::unordered_map<void*, std::weak_ptr<MetalSharedLocal>> m_by_handle;
    std::unordered_map<MetalSharedLocal*, std::vector<void*>> m_handles_of;
    std::unordered_map<u32, std::weak_ptr<MetalSharedLocal>> m_by_store;
    std::unordered_map<MetalSharedLocal*, u32> m_store_of;
    /// The locals that have a store: the ones publication concerns.
    std::unordered_map<MetalSharedLocal*, std::weak_ptr<MetalSharedLocal>>
        m_stored;
    std::atomic<u32> m_live{0};
};

/// Shared CPU memory (BackendSharedMemory): a Memory store.
class MetalSharedMemory final : public BackendSharedMemory {
public:
    explicit MetalSharedMemory(std::shared_ptr<MetalSharedLocal> local,
                               std::vector<u8> metadata);
    [[nodiscard]] u64 shared_handle() const noexcept override;
    [[nodiscard]] u8* data() noexcept override;
    [[nodiscard]] usize size() const noexcept override;
    [[nodiscard]] const std::vector<u8>& shared_metadata()
        const noexcept override {
        return m_metadata;
    }
    [[nodiscard]] bool acquire() override;
    void note_written() override;
    [[nodiscard]] std::shared_ptr<MetalSharedStore> store() const {
        return m_local->store();
    }

private:
    std::shared_ptr<MetalSharedLocal> m_local;
    std::vector<u8> m_metadata;
};

}  // namespace vitreo::graphics
