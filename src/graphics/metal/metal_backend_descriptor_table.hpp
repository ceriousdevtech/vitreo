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

#include <array>
#include <cstring>
#include <mutex>
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
        m_words.assign(desc.slot_count, 0ull);
        m_resources.assign(desc.slot_count, nullptr);
        m_storage.assign(desc.slot_count, nullptr);
    }

    ~MetalBackendDescriptorTable() override {
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
                                  unsigned offset) override {
        if (slot >= m_words.size()) return false;
        if (buffer == nullptr) {
            clear_slot(slot);
            return true;
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
        @autoreleasepool {
            id<MTLBuffer> mtl = (__bridge id<MTLBuffer>)handle;
            // A buffer descriptor is the GPU address, so the slice offset is
            // folded in here rather than bound separately as the flat path does.
            const u64 address = static_cast<u64>(mtl.gpuAddress) +
                                metal->native_offset() + offset;
            return write_slot(slot, address, handle);
        }
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
        @autoreleasepool {
            // A texture buffer is an MTLTexture: a Tier-2 argument buffer
            // holds its resource id exactly as it holds any texture's.
            id<MTLTexture> mtl = (__bridge id<MTLTexture>)handle;
            return write_slot(slot, mtl.gpuResourceID._impl, handle,
                              metal->storage_handle());
        }
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
        if (m_resources[slot] != nullptr) {
            CFRelease(m_resources[slot]);
            m_resources[slot] = nullptr;
        }
        if (m_storage[slot] != nullptr) {
            CFRelease(m_storage[slot]);
            m_storage[slot] = nullptr;
        }
        if (m_words[slot] != 0) {
            m_words[slot] = 0;
            m_dirty = true;
        }
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
        for (Pooled& pooled : m_pool) {
            if (pooled.buffer != m_published) continue;
            pooled.generation =
                MetalSubmitQueue::instance().recording_generation();
            break;
        }
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
            for (Pooled& pooled : m_pool) {
                if (pooled.buffer != m_published) continue;
                pooled.generation = gen;
                break;
            }
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
                                  void* storage = nullptr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (owned != nullptr) CFRetain(owned);
        if (m_resources[slot] != nullptr) CFRelease(m_resources[slot]);
        m_resources[slot] = owned;
        if (storage != nullptr) CFRetain(storage);
        if (m_storage[slot] != nullptr) CFRelease(m_storage[slot]);
        m_storage[slot] = storage;
        if (m_words[slot] != word) {
            m_words[slot] = word;
            m_dirty = true;
        }
        return true;
    }

    /// A slice not referenced by in-flight GPU work, or a fresh one.
    [[nodiscard]] void* acquire_buffer() {
        const u64 completed = MetalSubmitQueue::instance().completed_generation();
        const u64 recording = MetalSubmitQueue::instance().recording_generation();
        for (Pooled& pooled : m_pool) {
            if (pooled.buffer == m_published) continue;
            if (pooled.in_use && pooled.generation > completed) continue;
            pooled.generation = recording;
            pooled.in_use = true;
            return pooled.buffer;
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
    /// For a typed-buffer slot, the retained MTLBuffer its alias texture reads
    /// and writes; declared resident with it so hazards order against it.
    std::vector<void*> m_storage;
    std::vector<Pooled> m_pool;
    void* m_published{nullptr};
    bool m_dirty{true};
    /// publish_for: the generation last published for, and what that
    /// publish made resident (m_resources then m_storage, as publish()).
    u64 m_published_gen{0};
    std::vector<void*> m_published_resident;
};

/// delta-record: publish_descriptor_tables for recording generation @p gen,
/// taking no references (MetalBackendDescriptorTable::publish_for).
[[nodiscard]] inline bool publish_descriptor_tables_for(
    const std::array<const BackendDescriptorTable*,
                     BACKEND_MAX_DESCRIPTOR_SETS>& tables,
    std::array<void*, 4>& out_buffers, std::vector<void*>& out_resident,
    u64 gen) {
    for (unsigned set = 0; set < BACKEND_MAX_DESCRIPTOR_SETS; ++set) {
        if (tables[set] == nullptr) continue;
        auto* table = const_cast<MetalBackendDescriptorTable*>(
            static_cast<const MetalBackendDescriptorTable*>(tables[set]));
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
    std::array<void*, 4>& out_buffers, std::vector<void*>& out_resident) {
    for (unsigned set = 0; set < BACKEND_MAX_DESCRIPTOR_SETS; ++set) {
        if (tables[set] == nullptr) continue;
        auto* table = const_cast<MetalBackendDescriptorTable*>(
            static_cast<const MetalBackendDescriptorTable*>(tables[set]));
        void* const buffer = table->publish(out_resident);
        if (buffer == nullptr) {
            VITREO_TRACE("[metal] descriptor set %u failed to publish\n", set);
            return false;
        }
        out_buffers[set] = buffer;
    }
    return true;
}

} // namespace vitreo::graphics
