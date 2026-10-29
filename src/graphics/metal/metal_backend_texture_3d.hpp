// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file metal_backend_texture_3d.hpp
/// @brief The backend volume texture
///
/// Private to vitreo_graphics_metal, split out of metal_device.mm: included
/// only by that target's .mm files, after <Metal/Metal.h> (the layering
/// check keeps Apple framework imports out of headers).

#include <vitreo/common.hpp>
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/format_converter.hpp"
#include "graphics/metal/metal_shared_resource.hpp"
#include "graphics/metal/metal_texture.hpp"

#include <array>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace vitreo::graphics {

class MetalBackendTexture3D final : public BackendTexture3D {
public:
    [[nodiscard]] static bool describe(const BackendTexture3DDesc& desc,
                                       bool has_initial_data,
                                       TextureDesc& metal_desc);

    MetalBackendTexture3D(void* device, void* command_queue,
                          BackendTexture3DDesc desc,
                          const void* initial_data,
                          unsigned initial_row_pitch,
                          unsigned initial_slice_pitch);

    bool valid() const noexcept override {
        return m_texture != nullptr && m_texture->handle() != nullptr;
    }

    const BackendTexture3DDesc& desc() const noexcept override {
        return m_desc;
    }

    bool readback_w_slice(unsigned mip_level, unsigned w_slice, void* data,
                          unsigned row_pitch) override;

    void zero_initialize(const FormatInfo& format);

    bool clear_render_target_slices(const std::array<float, 4>& color,
                                    unsigned mip_level,
                                    unsigned first_w_slice,
                                    unsigned w_size) override;

    bool update(unsigned mip_level, const void* data, unsigned row_pitch,
                unsigned slice_pitch) override;

    bool update_region(unsigned mip_level, const void* data,
                       unsigned row_pitch, unsigned slice_pitch, unsigned x,
                       unsigned y, unsigned z, unsigned width,
                       unsigned height, unsigned depth) override;

    bool fill_texels(const void* texel, unsigned texel_bytes,
                     unsigned mip_level, unsigned x, unsigned y, unsigned z,
                     unsigned width, unsigned height,
                     unsigned depth) override;

    [[nodiscard]] void* native_handle() const noexcept {
        if (m_view_handle != nullptr) return m_view_handle;
        return m_texture != nullptr ? m_texture->handle() : nullptr;
    }

    BackendTexture3D* view(PixelFormat format, unsigned first_mip,
                           unsigned mip_count) override;

    ~MetalBackendTexture3D() override;

    /// Another opening in this process of the mirrored volume @p local.
    MetalBackendTexture3D(void* device, void* command_queue,
                          std::shared_ptr<MetalSharedLocal> local,
                          BackendTexture3DDesc desc)
        : m_desc(std::move(desc))
        , m_command_queue(command_queue)
        , m_texture(local->texture)
        , m_device(device)
        , m_shared(std::move(local)) {
        m_desc.shareable = true;
    }

    [[nodiscard]] static SharedStoreRecord shared_record_for(
        const BackendTexture3DDesc& desc);

    [[nodiscard]] static BackendTexture3DDesc desc_from_record(
        const SharedStoreRecord& record);

    [[nodiscard]] const std::shared_ptr<MetalSharedLocal>& shared_local()
        const noexcept {
        return m_shared;
    }
    [[nodiscard]] const std::shared_ptr<MetalTexture>& local_texture()
        const noexcept {
        return m_texture;
    }
    void adopt_shared(std::shared_ptr<MetalSharedLocal> local) {
        m_shared = std::move(local);
        m_desc.shareable = true;
        m_desc.shared_metadata = m_shared->record().frontend;
    }

    u64 shared_handle() const noexcept override {
        if (!m_shared) return 0u;
        const auto store = m_shared->store();
        return store ? store->id() : 0u;
    }
    u64 export_shared() override {
        return m_shared ? m_shared->export_store() : 0u;
    }

private:
    /// A view() of @p parent: binds @p view_handle (retained, released
    /// with this object) and keeps the parent's storage alive.
    MetalBackendTexture3D(const MetalBackendTexture3D& parent,
                          void* view_handle, BackendTexture3DDesc desc)
        : m_desc(std::move(desc))
        , m_command_queue(parent.m_command_queue)
        , m_texture(parent.m_texture)
        , m_device(parent.m_device)
        , m_view_handle(view_handle) {}

    [[nodiscard]] void* fill_view(unsigned mip, unsigned texel_bytes);

    BackendTexture3DDesc m_desc;
    void* m_command_queue{nullptr};
    std::shared_ptr<MetalTexture> m_texture;
    void* m_device{nullptr};
    std::shared_ptr<MetalSharedLocal> m_shared;
    std::unordered_map<u32, void*> m_fill_views;
    /// Set on a view() object: the Metal texture view it binds.
    void* m_view_handle{nullptr};
    /// view() objects of this volume, by (format, first mip, mip count).
    std::mutex m_views_mutex;
    std::unordered_map<u64, std::unique_ptr<MetalBackendTexture3D>> m_views;
};

} // namespace vitreo::graphics
