// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file metal_backend_descriptor_table.hpp
/// @brief Tier-2 argument-buffer descriptor tables
///
/// Private to vitreo_graphics_metal, split out of metal_device.mm: included
/// only by that target's .mm files, after <Metal/Metal.h> (the layering
/// check keeps Apple framework imports out of headers).

#include <vitreo/common.hpp>
#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_backend_buffer.hpp"
#include "graphics/metal/metal_backend_state.hpp"
#include "graphics/metal/metal_backend_texture_2d.hpp"
#include "graphics/metal/metal_backend_texture_3d.hpp"
#include "graphics/metal/metal_submit_queue.hpp"
#include "graphics/metal/metal_raytracing.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vitreo::graphics {

inline void retain_cs_handle(void* object) {
    if (object != nullptr) {
        // Every draw handle is an id<MTL*>. The ARC bridge retains through
        // objc_retain directly; CFRetain first runs its toll-free-bridging
        // test (CF_IS_OBJC), ~1.4% of the D3D11 render thread at ~15 retains a
        // draw. MetalSubmitQueue::release_draw is the matching release.
        (void)CFBridgingRetain((__bridge id)object);
    }
}

/// A Tier-2 argument buffer holding one descriptor set.
///
/// Slot N is descriptor N, at byte `N * MSL_DESCRIPTOR_SLOT_BYTES`. Metal 3
/// descriptors are all 8 bytes — `MTLResourceID` for a texture or sampler, a
/// `MTLGPUAddress` for a buffer — so the class of a slot does not move the next
/// one, and one flat word array serves all three.
///
/// Writes land in a CPU-side shadow, never in GPU memory a draw might be reading.
/// `publish()` snapshots the shadow into a buffer the command stream owns, so a
/// binding changed after a draw was recorded cannot reach back into it. A clean
/// table republishes nothing, which is the common case: D3D binding state is
/// mostly stable across draws.
class MetalBackendDescriptorTable final : public BackendDescriptorTable {
public:
    MetalBackendDescriptorTable(void* device,
                                const BackendDescriptorTableDesc& desc)
        : m_device(device), m_desc(desc) {
        if (desc.slot_count == 0) return;
        if (desc.persistent) {
            // The argument buffer draws bind, written in place. Shared
            // storage: a CPU write is visible to the next command buffer.
            @autoreleasepool {
                id<MTLBuffer> buffer = [(__bridge id<MTLDevice>)device
                    newBufferWithLength:static_cast<NSUInteger>(desc.slot_count) *
                                        sizeof(u64)
                                options:MTLResourceStorageModeShared];
                if (buffer == nil) return;
                std::memset([buffer contents], 0, [buffer length]);
                m_live_words = static_cast<u64*>([buffer contents]);
                m_live = (__bridge_retained void*)buffer;
            }
            m_log = std::make_shared<MetalResidentLog>();
        }
        m_words.assign(desc.slot_count, 0ull);
        m_resources.assign(desc.slot_count, nullptr);
        m_storage.assign(desc.slot_count, nullptr);
        m_buffer_cache.assign(desc.slot_count, {});
    }

    ~MetalBackendDescriptorTable() override {
        if (m_live != nullptr) CFRelease(m_live);
        for (void* slot : m_resources) {
            if (slot != nullptr) CFRelease(slot);
        }
        for (void* storage : m_storage) {
            if (storage != nullptr) CFRelease(storage);
        }
        for (const Pooled& pooled : m_pool) {
            if (pooled.buffer != nullptr) CFRelease(pooled.buffer);
        }
    }

    [[nodiscard]] bool valid() const noexcept override {
        return m_device != nullptr && !m_words.empty();
    }
    [[nodiscard]] unsigned slot_count() const noexcept override {
        return static_cast<unsigned>(m_words.size());
    }

    [[nodiscard]] bool set_texture(unsigned slot,
                                   const BackendTexture2D* texture,
                                   const BackendTextureViewDesc& view) override {
        if (slot >= m_words.size()) return false;
        if (texture == nullptr) {
            clear_slot(slot);
            return true;
        }
        const auto* metal = static_cast<const MetalBackendTexture2D*>(texture);
        if (!metal->valid()) return false;
        // Resolve the mip/slice/format window through the same helper the flat
        // path uses, so a table-bound SRV sees exactly what a flat-bound one
        // would. A view is a distinct MTLTexture with its own resource id.
        void* handle = metal->native_shader_handle(view, view.mip_levels != 1);
        if (handle == nullptr) return false;
        @autoreleasepool {
            id<MTLTexture> mtl = (__bridge id<MTLTexture>)handle;
            return write_slot(slot, mtl.gpuResourceID._impl, handle);
        }
    }

    [[nodiscard]] bool set_residency_texture(
        unsigned slot, const BackendTexture2D* texture,
        const BackendTextureViewDesc& view, unsigned samples) override {
        if (slot >= m_words.size()) return false;
        BackendTextureViewDesc window = view;
        // The companion's own float format, read unswizzled.
        window.pixel_format = PixelFormat::Unknown;
        window.swizzle = BackendViewSwizzle::Identity;
        window.component_mapping = 0;
        const MetalBackendTexture2D* source =
            texture != nullptr
                ? static_cast<const MetalBackendTexture2D*>(texture)
                      ->residency_companion()
                : nullptr;
        if (source == nullptr) {
            // Not tiled: a 1x1 texture of 1.0 in the view's shape.
            using Dim = BackendTextureViewDimension;
            const bool ms = samples > 1u;
            Dim dimension = view.dimension;
            if (dimension == Dim::Automatic) dimension = ms ? Dim::Texture2DMS : Dim::Texture2D;
            source = MetalBackendTexture2D::residency_dummy(dimension, samples);
            if (source == nullptr) return false;
            window = BackendTextureViewDesc{};
            window.dimension = dimension;
            window.mip_levels = 1;
            window.array_size =
                dimension == Dim::TextureCube || dimension == Dim::TextureCubeArray
                    ? 6u
                    : dimension == Dim::Texture2DMSArray ? 2u : 1u;
        }
        return set_texture(slot, source, window);
    }

    [[nodiscard]] bool set_residency_texture_3d(unsigned slot) override {
        return set_texture_3d(slot, MetalBackendTexture2D::residency_dummy_3d());
    }

    [[nodiscard]] bool set_texture_3d(unsigned slot,
                                      const BackendTexture3D* texture) override {
        if (slot >= m_words.size()) return false;
        if (texture == nullptr) {
            clear_slot(slot);
            return true;
        }
        const auto* metal = static_cast<const MetalBackendTexture3D*>(texture);
        if (!metal->valid()) return false;
        void* handle = metal->native_handle();
        if (handle == nullptr) return false;
        @autoreleasepool {
            id<MTLTexture> mtl = (__bridge id<MTLTexture>)handle;
            return write_slot(slot, mtl.gpuResourceID._impl, handle);
        }
    }

    [[nodiscard]] bool set_buffer(unsigned slot, const BackendBuffer* buffer,
                                  u64 offset) override {
        if (slot >= m_words.size()) return false;
        if (buffer == nullptr) {
            clear_slot(slot);
            return true;
        }
        // Metal buffer offsets are NSUInteger, which is pointer-width. On
        // arm64 that is 64 bits, so a byte past 4 GiB is a real binding. A
        // value that does not fit must not be truncated onto a low address;
        // the slot is left unchanged and the caller clears it.
        if constexpr (sizeof(NSUInteger) < sizeof(u64)) {
            if (offset > static_cast<u64>(~NSUInteger{0})) return false;
        }
        const auto* metal = static_cast<const MetalBackendBuffer*>(buffer);
        if (!metal->valid()) return false;
        // Tell the buffer it is about to be read by the GPU, exactly as the flat
        // bind path does. MetalBuffer renames its allocation on the next update
        // when it knows GPU work references the current one; without this it
        // overwrites in place, and every draw already queued reads the newest
        // contents instead of the ones it was recorded with.
        //
        // Found by the flat-vs-table differential on the `stress` scene, which
        // issues sixteen draws each preceded by UpdateSubresource on one
        // constant buffer: the table path drew one triangle where the flat path
        // drew sixteen. Nothing else in the corpus moved, and no unit test could
        // have seen it -- it needs several draws in one frame over one buffer.
        metal->note_gpu_bind();
        void* handle = metal->native_handle();
        if (handle == nullptr) return false;
        const usize gpu_offset = metal->native_offset();
        // Same MTLBuffer and same slice as the descriptor already stored.
        // gpuAddress does not change for a given handle, so the recorded
        // word is still right. note_gpu_bind still ran: a later Map(DISCARD)
        // must rename instead of overwriting this draw.
        if (buffer_slot_unchanged(slot, handle, nullptr, gpu_offset, offset)) {
            return true;
        }
        @autoreleasepool {
            id<MTLBuffer> mtl = (__bridge id<MTLBuffer>)handle;
            // A buffer descriptor is the GPU address, so the slice offset is
            // folded in here rather than bound separately as the flat path does.
            // A sum that does not fit in the 64-bit GPU address must not wrap.
            const u64 base = static_cast<u64>(mtl.gpuAddress);
            const u64 slice = static_cast<u64>(gpu_offset);
            if (slice > ~u64{0} - base || offset > ~u64{0} - (base + slice)) {
                return false;
            }
            const u64 address = base + slice + offset;
            if (!write_slot(slot, address, handle)) return false;
        }
        remember_buffer_slot(slot, handle, nullptr, gpu_offset, offset);
        return true;
    }

    [[nodiscard]] bool set_texture_buffer(
        unsigned slot, const BackendTextureBuffer* view) override {
        if (slot >= m_words.size()) return false;
        if (view == nullptr) {
            clear_slot(slot);
            return true;
        }
        if (!view->valid()) return false;
        // The same resolution the flat path performs: rebuild the alias if the
        // buffer was renamed. And, as set_buffer, tell the buffer the GPU is
        // about to read it so a later DISCARD renames it rather than
        // overwriting what this draw was recorded with. A view with an
        // element bias (BackendTextureBuffer::element_bias) is the frontend's
        // to apply, as on the flat path.
        auto* metal = const_cast<MetalBackendTextureBuffer*>(
            static_cast<const MetalBackendTextureBuffer*>(view));
        void* handle = metal->native_shader_handle();
        if (handle == nullptr) return false;
        metal->note_gpu_bind();
        void* storage = metal->storage_handle();
        if (buffer_slot_unchanged(slot, handle, storage, 0, 0)) return true;
        @autoreleasepool {
            // A texture buffer is an MTLTexture: a Tier-2 argument buffer
            // holds its resource id exactly as it holds any texture's.
            id<MTLTexture> mtl = (__bridge id<MTLTexture>)handle;
            if (!write_slot(slot, mtl.gpuResourceID._impl, handle, storage)) {
                return false;
            }
        }
        remember_buffer_slot(slot, handle, storage, 0, 0);
        return true;
    }

    [[nodiscard]] bool set_sampler(unsigned slot,
                                   const BackendSamplerDesc& sampler) override {
        if (slot >= m_words.size()) return false;
        MetalPassStateCache cache(m_device);
        void* state = cache.sampler_state(sampler);
        if (state == nullptr) return false;
        @autoreleasepool {
            id<MTLSamplerState> mtl = (__bridge id<MTLSamplerState>)state;
            // The cache owns its states for the process lifetime, so this slot
            // borrows rather than retains; passing null as the owned handle
            // keeps it out of the residency list, which samplers do not need.
            return write_slot(slot, mtl.gpuResourceID._impl, nullptr);
        }
    }

    void clear_slot(unsigned slot) override {
        if (slot >= m_words.size()) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        mark_acceleration_structure(slot, false);
        if (m_resources[slot] != nullptr) {
            resident_changed(m_resources[slot], nullptr);
            CFRelease(m_resources[slot]);
            m_resources[slot] = nullptr;
        }
        if (m_storage[slot] != nullptr) {
            resident_changed(m_storage[slot], nullptr);
            CFRelease(m_storage[slot]);
            m_storage[slot] = nullptr;
        }
        if (m_words[slot] != 0) {
            m_words[slot] = 0;
            m_dirty = true;
            if (m_live_words != nullptr) m_live_words[slot] = 0;
        }
        forget_buffer_slot(slot);
    }

    [[nodiscard]] bool set_acceleration_structure(
        unsigned slot, const BackendAccelerationStructure* structure) override {
        if (slot >= m_words.size()) return false;
        if (structure == nullptr) {
            clear_slot(slot);
            return true;
        }
        const auto* metal = static_cast<const MetalAccelerationStructure*>(structure);
        if (!metal->valid()) return false;
        return write_slot(slot, metal->resource_id(), metal->native_handle(), nullptr, true);
    }

    /// Some slot holds an acceleration structure: a draw binding this table
    /// declares every live bottom level resident too
    /// (metal_rt_bottom_level_token), since a top level reaches them.
    [[nodiscard]] bool holds_acceleration_structures() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_acceleration_structures != 0;
    }

    [[nodiscard]] bool persistent() const noexcept { return m_live != nullptr; }

    void declare_shader_writes() override {
        m_shader_writes.store(true, std::memory_order_relaxed);
    }

    /// A persistent table's view for one draw: its own argument buffer,
    /// retained for the stream, and the token naming what it reaches.
    [[nodiscard]] void* publish_persistent(MetalResidentToken& token) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_live == nullptr) return nullptr;
        token.log = m_log;
        token.count = m_log->size();
        token.writable = m_shader_writes.load(std::memory_order_relaxed);
        retain_cs_handle(m_live);
        return m_live;
    }

    /// One draw's view of the table: the MTLBuffer to bind, or nullptr when
    /// it cannot be published. The resources the table reaches are appended
    /// to @p resident -- the encoder must mark them resident, because an
    /// argument buffer does not imply residency the way a direct binding
    /// does (without useResource the GPU faults or reads nothing). The
    /// buffer and every appended resource are retained for the stream, under
    /// this table's lock, so a slot rewritten concurrently cannot release one
    /// first. Appending in place: a per-draw snapshot vector was allocated,
    /// copied and freed for every table of every draw.
    [[nodiscard]] void* publish(std::vector<void*>& resident) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_words.empty() || m_device == nullptr) return nullptr;
        if (m_dirty || m_published == nullptr) {
            void* buffer = acquire_buffer();
            if (buffer == nullptr) return nullptr;
            @autoreleasepool {
                id<MTLBuffer> mtl = (__bridge id<MTLBuffer>)buffer;
                std::memcpy([mtl contents], m_words.data(),
                            m_words.size() * sizeof(u64));
            }
            m_published = buffer;
            m_dirty = false;
        }
        // Re-tag on EVERY publish, not only when the contents changed. A clean
        // table hands the same slice to draw after draw, and if its generation
        // stayed at the one it was first acquired at, the pool would judge it
        // retired while a much more recent draw still referenced it and hand it
        // out to be overwritten.
        retag_published(MetalSubmitQueue::instance().recording_generation());
        resident.reserve(resident.size() + m_resources.size() +
                         m_storage.size());
        for (void* slot : m_resources) {
            if (slot == nullptr) continue;
            resident.push_back(slot);
            retain_cs_handle(slot);
        }
        for (void* storage : m_storage) {
            if (storage == nullptr) continue;
            resident.push_back(storage);
            retain_cs_handle(storage);
        }
        retain_cs_handle(m_published);
        return m_published;
    }

    /// delta-record: publish() for recording generation @p gen without
    /// taking references -- the delta record takes them on what a draw
    /// carries. A clean table already published for @p gen hands back the
    /// same buffer and residency without touching its pool (its slice is
    /// already tagged @p gen, which is what publish's re-tag guarantees).
    [[nodiscard]] void* publish_for(u64 gen, std::vector<void*>& resident) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_words.empty() || m_device == nullptr) return nullptr;
        if (m_dirty || m_published == nullptr || m_published_gen != gen) {
            if (m_dirty || m_published == nullptr) {
                void* buffer = acquire_buffer();
                if (buffer == nullptr) return nullptr;
                @autoreleasepool {
                    id<MTLBuffer> mtl = (__bridge id<MTLBuffer>)buffer;
                    std::memcpy([mtl contents], m_words.data(),
                                m_words.size() * sizeof(u64));
                }
                m_published = buffer;
                m_dirty = false;
            }
            retag_published(gen);
            m_published_gen = gen;
            m_published_resident.clear();
            for (void* slot : m_resources) {
                if (slot != nullptr) m_published_resident.push_back(slot);
            }
            for (void* storage : m_storage) {
                if (storage != nullptr) m_published_resident.push_back(storage);
            }
        }
        resident.insert(resident.end(), m_published_resident.begin(),
                        m_published_resident.end());
        return m_published;
    }

private:
    struct Pooled {
        void* buffer{nullptr};
        /// Recording generation this slice was last published at. It may be
        /// reused once the queue reports that generation complete.
        u64 generation{0};
        bool in_use{false};
    };

    [[nodiscard]] bool write_slot(unsigned slot, u64 word, void* owned,
                                  void* storage = nullptr,
                                  bool acceleration_structure = false) {
        std::lock_guard<std::mutex> lock(m_mutex);
        mark_acceleration_structure(slot, acceleration_structure);
        // The same resource and word are already published. Another
        // retain/release pair would only bounce the refcount; a later draw
        // that still names this slot must not pay that on every call.
        if (m_resources[slot] == owned && m_storage[slot] == storage) {
            if (m_words[slot] != word) {
                m_words[slot] = word;
                m_dirty = true;
                if (m_live_words != nullptr) m_live_words[slot] = word;
            }
            return true;
        }
        forget_buffer_slot(slot);
        resident_changed(m_resources[slot], owned);
        resident_changed(m_storage[slot], storage);
        if (owned != nullptr) CFRetain(owned);
        if (m_resources[slot] != nullptr) CFRelease(m_resources[slot]);
        m_resources[slot] = owned;
        if (storage != nullptr) CFRetain(storage);
        if (m_storage[slot] != nullptr) CFRelease(m_storage[slot]);
        m_storage[slot] = storage;
        if (m_words[slot] != word) {
            m_words[slot] = word;
            m_dirty = true;
            if (m_live_words != nullptr) m_live_words[slot] = word;
        }
        return true;
    }

    /// Caller holds m_mutex.
    void mark_acceleration_structure(unsigned slot, bool on) {
        if (m_acceleration_slots.size() <= slot) {
            if (!on) return;
            m_acceleration_slots.resize(m_words.size(), 0);
        }
        if ((m_acceleration_slots[slot] != 0) == on) return;
        m_acceleration_slots[slot] = on ? 1 : 0;
        if (on) ++m_acceleration_structures;
        else --m_acceleration_structures;
    }

    /// Persistent tables: a slot stopped naming @p before and names @p after.
    /// The log gains a handle the first time any slot names it. Handles no
    /// slot names stay logged (declaring a resource the shader cannot reach
    /// is harmless) until they outnumber the live ones; then a fresh log of
    /// the live handles replaces it, and draws already recorded keep theirs.
    /// Caller holds m_mutex.
    void resident_changed(void* before, void* after) {
        if (m_log == nullptr || before == after) return;
        if (after != nullptr) {
            u32& refs = m_live_refs[after];
            if (refs++ == 0) {
                if (m_logged.insert(after).second) {
                    CFRetain(after);
                    m_log->append(after);
                } else if (m_dead_logged != 0) {
                    --m_dead_logged;
                }
            }
        }
        if (before != nullptr) {
            const auto it = m_live_refs.find(before);
            if (it != m_live_refs.end() && --it->second == 0) {
                m_live_refs.erase(it);
                ++m_dead_logged;
            }
        }
        if (m_dead_logged > kResidentLogSlack &&
            m_dead_logged > m_live_refs.size()) {
            auto fresh = std::make_shared<MetalResidentLog>();
            m_logged.clear();
            for (const auto& [handle, refs] : m_live_refs) {
                (void)refs;
                CFRetain(handle);
                fresh->append(handle);
                m_logged.insert(handle);
            }
            m_log = std::move(fresh);
            m_dead_logged = 0;
        }
    }

    /// The published slice's generation, which every publish re-tags. The
    /// published slice is the newest acquired, the back of the pool.
    void retag_published(u64 generation) {
        if (!m_pool.empty() && m_pool.back().buffer == m_published) {
            m_pool.back().generation = generation;
            return;
        }
        for (Pooled& pooled : m_pool) {
            if (pooled.buffer != m_published) continue;
            pooled.generation = generation;
            break;
        }
    }

    /// A slice not referenced by in-flight GPU work, or a fresh one.
    ///
    /// The pool is a FIFO in acquisition order, so its generations ascend
    /// front to back (a re-tag only raises the back's): the front is the only
    /// slice that can have retired first. A table republished every draw keeps
    /// frames of slices in flight; scanning them all from the front made each
    /// publish O(slices in flight).
    [[nodiscard]] void* acquire_buffer() {
        const u64 completed = MetalSubmitQueue::instance().completed_generation();
        const u64 recording = MetalSubmitQueue::instance().recording_generation();
        if (!m_pool.empty()) {
            Pooled front = m_pool.front();
            if (front.buffer != m_published &&
                (!front.in_use || front.generation <= completed)) {
                m_pool.pop_front();
                front.generation = recording;
                front.in_use = true;
                m_pool.push_back(front);
                return front.buffer;
            }
        }
        @autoreleasepool {
            id<MTLBuffer> buffer = [(__bridge id<MTLDevice>)m_device
                newBufferWithLength:m_words.size() * sizeof(u64)
                            options:MTLResourceStorageModeShared];
            if (buffer == nil) return nullptr;
            void* handle = (__bridge_retained void*)buffer;
            m_pool.push_back({handle, recording, true});
            return handle;
        }
    }

    void* m_device{nullptr};
    BackendDescriptorTableDesc m_desc{};
    mutable std::mutex m_mutex;
    /// One 8-byte descriptor per slot, in register order.
    std::vector<u64> m_words;
    /// Retained id<MTLResource> per slot, for residency. Null for an empty slot
    /// and for samplers, which the shared cache owns and which need no
    /// residency call.
    std::vector<void*> m_resources;
    /// Last buffer identity written into each slot. The recording thread is
    /// the only writer. A match means the stored GPU address is still the
    /// one set_buffer / set_texture_buffer would write.
    struct BufferSlotCache {
        void* handle{nullptr};
        void* storage{nullptr};
        usize gpu_offset{0};
        u64 user_offset{0};
        bool valid{false};
    };
    std::vector<BufferSlotCache> m_buffer_cache;

    [[nodiscard]] bool buffer_slot_unchanged(unsigned slot, void* handle,
                                             void* storage, usize gpu_offset,
                                             u64 user_offset) const {
        if (slot >= m_buffer_cache.size()) return false;
        const BufferSlotCache& slot_cache = m_buffer_cache[slot];
        return slot_cache.valid && slot_cache.handle == handle &&
               slot_cache.storage == storage &&
               slot_cache.gpu_offset == gpu_offset &&
               slot_cache.user_offset == user_offset;
    }
    void remember_buffer_slot(unsigned slot, void* handle, void* storage,
                              usize gpu_offset, u64 user_offset) {
        if (slot >= m_buffer_cache.size()) return;
        m_buffer_cache[slot] = BufferSlotCache{handle, storage, gpu_offset,
                                               user_offset, true};
    }
    void forget_buffer_slot(unsigned slot) {
        if (slot < m_buffer_cache.size()) m_buffer_cache[slot].valid = false;
    }
    /// For a typed-buffer slot, the retained MTLBuffer its alias texture reads
    /// and writes; declared resident with it so hazards order against it.
    std::vector<void*> m_storage;
    std::deque<Pooled> m_pool;
    void* m_published{nullptr};
    bool m_dirty{true};
    /// publish_for: the generation last published for, and what that
    /// publish made resident (m_resources then m_storage, as publish()).
    u64 m_published_gen{0};
    std::vector<void*> m_published_resident;
    /// BackendDescriptorTableDesc::persistent: the argument buffer itself
    /// (retained) and its contents, which every write updates in place.
    void* m_live{nullptr};
    u64* m_live_words{nullptr};
    /// Slots holding an acceleration structure (set_acceleration_structure).
    std::vector<u8> m_acceleration_slots;
    u32 m_acceleration_structures{0};
    /// What the slots reach, for draws' residency tokens (resident_changed).
    std::shared_ptr<MetalResidentLog> m_log;
    /// declare_shader_writes: tokens ask for read and write usage.
    std::atomic<bool> m_shader_writes{false};
    std::unordered_map<void*, u32> m_live_refs;
    std::unordered_set<void*> m_logged;
    usize m_dead_logged{0};
    static constexpr usize kResidentLogSlack = 4096;
};

/// delta-record: publish_descriptor_tables for recording generation @p gen,
/// taking no references (MetalBackendDescriptorTable::publish_for).
[[nodiscard]] inline bool publish_descriptor_tables_for(
    const std::array<const BackendDescriptorTable*,
                     BACKEND_MAX_DESCRIPTOR_SETS>& tables,
    std::array<void*, BACKEND_MAX_DESCRIPTOR_SETS>& out_buffers,
    std::vector<void*>& out_resident,
    u64 gen, unsigned first_set = 0,
    unsigned last_set = BACKEND_MAX_DESCRIPTOR_SETS) {
    for (unsigned set = first_set; set < last_set &&
                                   set < BACKEND_MAX_DESCRIPTOR_SETS; ++set) {
        if (tables[set] == nullptr) continue;
        auto* table = const_cast<MetalBackendDescriptorTable*>(
            static_cast<const MetalBackendDescriptorTable*>(tables[set]));
        // A persistent table's residency is a token, which a delta record
        // does not carry: such a draw is recorded whole. So is one that
        // reaches acceleration structures (their bottom levels' token).
        if (table->persistent() || table->holds_acceleration_structures()) return false;
        void* const buffer = table->publish_for(gen, out_resident);
        if (buffer == nullptr) {
            VITREO_TRACE("[metal] descriptor set %u failed to publish\n", set);
            return false;
        }
        out_buffers[set] = buffer;
    }
    return true;
}

/// Publish one stage's descriptor tables into the command stream.
///
/// Returns false when a table was supplied but could not be published, which the
/// caller turns into a dropped draw: binding some of a shader's register spaces
/// and not others would render with whatever the missing ones last held.
[[nodiscard]] inline bool publish_descriptor_tables(
    const std::array<const BackendDescriptorTable*,
                     BACKEND_MAX_DESCRIPTOR_SETS>& tables,
    std::array<void*, BACKEND_MAX_DESCRIPTOR_SETS>& out_buffers,
    std::vector<void*>& out_resident,
    unsigned first_set = 0,
    unsigned last_set = BACKEND_MAX_DESCRIPTOR_SETS,
    std::vector<MetalResidentToken>* out_tokens = nullptr) {
    for (unsigned set = first_set; set < last_set &&
                                   set < BACKEND_MAX_DESCRIPTOR_SETS; ++set) {
        if (tables[set] == nullptr) continue;
        auto* table = const_cast<MetalBackendDescriptorTable*>(
            static_cast<const MetalBackendDescriptorTable*>(tables[set]));
        if (table->persistent()) {
            // A heap window: its own buffer, and one token for everything
            // it reaches. A caller that cannot carry the token drops the
            // draw rather than bind resources it never made resident.
            MetalResidentToken token;
            void* const live =
                out_tokens != nullptr ? table->publish_persistent(token) : nullptr;
            if (live == nullptr) {
                VITREO_TRACE("[metal] descriptor set %u failed to publish\n",
                             set);
                return false;
            }
            out_tokens->push_back(std::move(token));
            out_buffers[set] = live;
            if (table->holds_acceleration_structures()) {
                out_tokens->push_back(metal_rt_bottom_level_token());
            }
            continue;
        }
        void* const buffer = table->publish(out_resident);
        if (buffer == nullptr) {
            VITREO_TRACE("[metal] descriptor set %u failed to publish\n", set);
            return false;
        }
        out_buffers[set] = buffer;
        if (table->holds_acceleration_structures()) {
            // A top level's bottom levels are resident only by declaration.
            if (out_tokens == nullptr) return false;
            out_tokens->push_back(metal_rt_bottom_level_token());
        }
    }
    return true;
}

} // namespace vitreo::graphics
