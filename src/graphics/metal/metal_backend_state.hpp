// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file metal_backend_state.hpp
/// @brief Process-lifetime depth-stencil and sampler states
///
/// Private to vitreo_graphics_metal, split out of metal_device.mm: included
/// only by that target's .mm files, after <Metal/Metal.h> (the layering
/// check keeps Apple framework imports out of headers).

#include <vitreo/common.hpp>
#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_backend_enums.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace vitreo::graphics {

/// A sampler's border twin: the same state with an opaque-white border
/// (BACKEND_BORDER_TWIN_SAMPLER_BASE).
inline BackendSamplerDesc border_twin(BackendSamplerDesc desc) {
    desc.border_color = 0xFFFFFFFFu;
    return desc;
}

class MetalPassStateCache {
public:
    explicit MetalPassStateCache(void* device) : m_device(device) {}
    ~MetalPassStateCache() { release(); }

    void* depth_stencil_state(const BackendDrawCall& call, bool has_depth) {
        if (!has_depth) return nullptr;
        u64 key = 1469598103934665603ull;
        const auto hash = [&](u64 value) {
            key ^= value;
            key *= 1099511628211ull;
        };
        hash(call.depth_test_enabled);
        hash(call.depth_write_enabled);
        hash(static_cast<u64>(call.depth_compare));
        hash(call.stencil_test_enabled);
        hash(call.stencil_read_mask);
        hash(call.stencil_write_mask);
        const auto hash_face = [&](const BackendStencilFaceDesc& face) {
            hash(static_cast<u64>(face.stencil_fail));
            hash(static_cast<u64>(face.depth_fail));
            hash(static_cast<u64>(face.pass));
            hash(static_cast<u64>(face.compare));
        };
        hash_face(call.front_stencil);
        hash_face(call.back_stencil);
        hash(reinterpret_cast<u64>(m_device));   // never share across devices
        {
            std::lock_guard<std::mutex> lk(store_mutex());
            const auto found = depth_store().find(key);
            if (found != depth_store().end()) return found->second;
        }

        MTLDepthStencilDescriptor* descriptor =
            [[MTLDepthStencilDescriptor alloc] init];
        descriptor.depthCompareFunction =
            call.depth_test_enabled ? metal_compare(call.depth_compare)
                                    : MTLCompareFunctionAlways;
        descriptor.depthWriteEnabled = call.depth_write_enabled ? YES : NO;
        if (call.stencil_test_enabled) {
            const auto make_face = [&](const BackendStencilFaceDesc& face) {
                MTLStencilDescriptor* stencil =
                    [[MTLStencilDescriptor alloc] init];
                stencil.stencilCompareFunction = metal_compare(face.compare);
                stencil.stencilFailureOperation =
                    metal_stencil_operation(face.stencil_fail);
                stencil.depthFailureOperation =
                    metal_stencil_operation(face.depth_fail);
                stencil.depthStencilPassOperation =
                    metal_stencil_operation(face.pass);
                stencil.readMask = call.stencil_read_mask;
                stencil.writeMask = call.stencil_write_mask;
                return stencil;
            };
            descriptor.frontFaceStencil = make_face(call.front_stencil);
            descriptor.backFaceStencil = make_face(call.back_stencil);
        }
        id<MTLDepthStencilState> state =
            [(__bridge id<MTLDevice>)m_device
                newDepthStencilStateWithDescriptor:descriptor];
        void* handle = (__bridge_retained void*)state;
        {
            std::lock_guard<std::mutex> lk(store_mutex());
            // Another thread may have built the same state while we were
            // outside the lock; keep the first and drop ours.
            const auto [it, inserted] = depth_store().emplace(key, handle);
            if (!inserted) {
                CFRelease(handle);
                return it->second;
            }
        }
        return handle;
    }

    void* sampler_state(const BackendSamplerDesc& desc) {
        u64 key =
            (static_cast<u64>(desc.min_filter) << 0) |
            (static_cast<u64>(desc.mag_filter) << 4) |
            (static_cast<u64>(desc.mip_filter) << 8) |
            (static_cast<u64>(desc.address_u) << 12) |
            (static_cast<u64>(desc.address_v) << 16) |
            (static_cast<u64>(desc.max_anisotropy) << 20) |
            (static_cast<u64>(desc.mipmaps_enabled ? 1 : 0) << 28) |
            (static_cast<u64>(desc.address_w) << 32) |
            (static_cast<u64>(metal_border_color_tag(desc.border_color)) << 36) |
            (static_cast<u64>(desc.comparison_enabled ? 1 : 0) << 40) |
            (static_cast<u64>(desc.comparison) << 41);
        u32 lod_min_bits = 0;
        u32 lod_max_bits = 0;
        static_assert(sizeof(lod_min_bits) == sizeof(desc.lod_min_clamp));
        std::memcpy(&lod_min_bits, &desc.lod_min_clamp, sizeof(lod_min_bits));
        std::memcpy(&lod_max_bits, &desc.lod_max_clamp, sizeof(lod_max_bits));
        key ^= static_cast<u64>(lod_min_bits) * 0x9e3779b185ebca87ull;
        key ^= static_cast<u64>(lod_max_bits) * 0xc2b2ae3d27d4eb4full;
        // Fold in the device: the store is shared, so two devices must not
        // collide on an otherwise identical descriptor.
        key ^= reinterpret_cast<u64>(m_device) * 0x9e3779b97f4a7c15ull;
        // A per-thread front for the store. A draw asks for all 16 sampler
        // slots of both stages, bound or not, and the store's mutex is shared
        // by every recording thread: 32 locked lookups per draw were ~2% of
        // the render thread. Entries never leave the store (release()), so a
        // cached handle stays valid for the life of the process.
        struct FrontEntry {
            u64 key{0};
            void* handle{nullptr};
        };
        static thread_local std::array<FrontEntry, 64> front{};
        FrontEntry& cached = front[static_cast<usize>(
            (key * 0xff51afd7ed558ccdull) >> 58)];
        if (cached.handle != nullptr && cached.key == key) {
            return cached.handle;
        }
        {
            std::lock_guard<std::mutex> lk(store_mutex());
            const auto found = sampler_store().find(key);
            if (found != sampler_store().end()) {
                cached = FrontEntry{key, found->second};
                return found->second;
            }
        }

        MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
        descriptor.minFilter = desc.min_filter == BackendFilter::Linear
                                   ? MTLSamplerMinMagFilterLinear
                                   : MTLSamplerMinMagFilterNearest;
        descriptor.magFilter = desc.mag_filter == BackendFilter::Linear
                                   ? MTLSamplerMinMagFilterLinear
                                   : MTLSamplerMinMagFilterNearest;
        if (!desc.mipmaps_enabled) {
            descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
            descriptor.lodMinClamp = 0.0f;
            descriptor.lodMaxClamp = 0.0f;
        } else {
            descriptor.mipFilter = desc.mip_filter == BackendFilter::Linear
                                       ? MTLSamplerMipFilterLinear
                                       : MTLSamplerMipFilterNearest;
            descriptor.lodMinClamp = std::max(0.0f, desc.lod_min_clamp);
            descriptor.lodMaxClamp = std::max(0.0f, desc.lod_max_clamp);
        }
        descriptor.sAddressMode = metal_address_mode(desc.address_u);
        descriptor.tAddressMode = metal_address_mode(desc.address_v);
        descriptor.rAddressMode = metal_address_mode(desc.address_w);
        descriptor.borderColor = metal_border_color(desc.border_color);
        descriptor.maxAnisotropy = std::max(1u, desc.max_anisotropy);
        // Required for the sampler to have a usable gpuResourceID, which is what
        // a descriptor table stores. Set unconditionally: it is a capability
        // flag, the flat path is unaffected by it, and one cached state serving
        // both binding modes beats two that can drift apart.
        descriptor.supportArgumentBuffers = YES;
        descriptor.compareFunction = desc.comparison_enabled
            ? metal_compare(desc.comparison)
            : MTLCompareFunctionNever;
        id<MTLSamplerState> state =
            [(__bridge id<MTLDevice>)m_device
                newSamplerStateWithDescriptor:descriptor];
        void* handle = (__bridge_retained void*)state;
        {
            std::lock_guard<std::mutex> lk(store_mutex());
            const auto [it, inserted] = sampler_store().emplace(key, handle);
            if (!inserted) {
                CFRelease(handle);
                handle = it->second;
            }
        }
        cached = FrontEntry{key, handle};
        return handle;
    }

    /// Deliberately does NOT free the cached states.
    ///
    /// These objects outlive the pass that first asked for them. A draw
    /// recorded now is encoded later on the submit thread, so releasing them
    /// when a render pass is destroyed leaves the queue holding freed
    /// MTLDepthStencilState pointers -- which crashes inside
    /// setDepthStencilState:, on the submit thread, far from the pass that
    /// caused it. Observed with a 32-bit D3D9 title.
    ///
    /// The store is shared and keyed purely by state contents, so nothing here
    /// is pass-specific in the first place; per-pass ownership was both unsafe
    /// and wasteful (identical states rebuilt for every pass). The states are
    /// small and bounded by the number of distinct state combinations a guest
    /// uses, so holding them for the process lifetime is the right trade.
    void release() {}

private:
    // Shared across every cache instance. A mutex is required because passes
    // are created on guest threads while the submit thread runs concurrently;
    // an unsynchronised unordered_map hands back torn pointers.
    static std::mutex& store_mutex() {
        static std::mutex mu;
        return mu;
    }
    static std::unordered_map<u64, void*>& depth_store() {
        static std::unordered_map<u64, void*> store;
        return store;
    }
    static std::unordered_map<u64, void*>& sampler_store() {
        static std::unordered_map<u64, void*> store;
        return store;
    }

    void* m_device{nullptr};
};

/// native-state (delta_record.hpp, R3c): a sampler built once. Its native
/// state and border twin come from MetalPassStateCache's process-lifetime
/// store, so a draw recorded with them may outlive this object.
class MetalBackendSamplerState final : public BackendSamplerState {
public:
    MetalBackendSamplerState(void* device, const BackendSamplerDesc& desc)
        : m_desc(desc) {
        MetalPassStateCache cache(device);
        m_native = cache.sampler_state(desc);
        m_twin = cache.sampler_state(border_twin(desc));
    }
    [[nodiscard]] const BackendSamplerDesc& desc() const noexcept override {
        return m_desc;
    }
    [[nodiscard]] bool valid() const noexcept {
        return m_native != nullptr && m_twin != nullptr;
    }
    [[nodiscard]] void* native() const noexcept { return m_native; }
    [[nodiscard]] void* twin() const noexcept { return m_twin; }

private:
    BackendSamplerDesc m_desc{};
    void* m_native{nullptr};
    void* m_twin{nullptr};
};

/// native-state (R3c): a depth-stencil state built once, from the same
/// process-lifetime store a draw's fields are looked up in.
class MetalBackendDepthStencilState final : public BackendDepthStencilState {
public:
    MetalBackendDepthStencilState(void* device,
                                  const BackendDepthStencilDesc& desc)
        : m_desc(desc) {
        MetalPassStateCache cache(device);
        BackendDrawCall fields{};
        apply_depth_stencil_desc(fields, desc);
        m_native = cache.depth_stencil_state(fields, /*has_depth=*/true);
    }
    [[nodiscard]] const BackendDepthStencilDesc& desc() const noexcept override {
        return m_desc;
    }
    [[nodiscard]] bool valid() const noexcept { return m_native != nullptr; }
    [[nodiscard]] void* native() const noexcept { return m_native; }

private:
    BackendDepthStencilDesc m_desc{};
    void* m_native{nullptr};
};

} // namespace vitreo::graphics
