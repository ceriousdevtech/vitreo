// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file d3d11_test_backend_texture.hpp
/// @brief Fake backend 2D texture.

#include "d3d11_test_backend_buffers.hpp"

namespace vitreo::winapi::d3d11_test {

class TestBackendTexture final : public graphics::BackendTexture2D {
public:
    TestBackendTexture(graphics::BackendTexture2DDesc desc,
                       BackendState& state)
        : m_desc(desc), m_state(state),
          m_test_handle(0x510000u + state.texture_creations) {
        // Like the Metal backend: a shareable single image of a format with
        // shared storage gets storage every opener sees; anything else is
        // created unshared.
        m_storage = m_desc.shareable &&
                    (state.extended_sharing ||
                     (m_desc.mip_levels == 1 && m_desc.array_size == 1 &&
                      m_desc.sample_count <= 1 &&
                      test_shared_storage_format(m_desc.pixel_format)));
        if (m_storage) {
            m_state.shared_storage[m_test_handle] = m_desc;
        } else {
            m_desc.shared_metadata.clear();
        }
    }
    /// An opening of shared storage @p handle.
    TestBackendTexture(graphics::BackendTexture2DDesc desc,
                       BackendState& state, u64 handle)
        : m_desc(desc), m_state(state), m_test_handle(handle),
          m_storage(true) {}
    static bool test_shared_storage_format(graphics::PixelFormat format) {
        const auto info = graphics::format_table(format);
        const u32 bytes = info.bytes_per_pixel;
        return info.mtl_pixel_format != 0 && !info.is_compressed &&
               !info.is_depth && format != graphics::PixelFormat::Stencil8 &&
               format != graphics::PixelFormat::B5G6R5Unorm &&
               format != graphics::PixelFormat::ABGR4Unorm &&
               format != graphics::PixelFormat::BGR5A1Unorm &&
               format != graphics::PixelFormat::RGB9E5Float &&
               (bytes == 1 || bytes == 2 || bytes == 4 || bytes == 8 ||
                bytes == 16);
    }
    const std::vector<u8>& shared_metadata() const noexcept override {
        return m_desc.shared_metadata;
    }
    bool valid() const noexcept override { return true; }
    const graphics::BackendTexture2DDesc& desc() const noexcept override {
        return m_desc;
    }
    u64 shared_handle() const noexcept override {
        if (m_state.force_shared_handle) {
            return m_state.unique_shared_handles
                ? m_test_handle
                : 0x51FACEu;
        }
        return m_storage ? m_test_handle : 0u;
    }
    bool clear_render_target(
        const std::array<float, 4>& color) override {
        ++m_state.texture_clears;
        m_state.last_texture_clear_mip = 0;
        m_state.last_clear = color;
        return m_desc.render_target;
    }
    bool clear_render_target_level(
        const std::array<float, 4>& color,
        unsigned mip_level, unsigned first_array_slice = 0,
        unsigned array_size = 1) override {
        if (!m_desc.render_target || mip_level >= m_desc.mip_levels ||
            array_size == 0 || first_array_slice >= m_desc.array_size ||
            array_size > m_desc.array_size - first_array_slice) {
            return false;
        }
        ++m_state.texture_clears;
        m_state.last_texture_clear_mip = mip_level;
        m_state.last_texture_clear_slice = first_array_slice;
        m_state.last_texture_clear_array_size = array_size;
        m_state.last_clear = color;
        return true;
    }
    bool clear_render_target_view(const std::array<float, 4>& color,
                                  graphics::PixelFormat view_format,
                                  unsigned mip_level,
                                  unsigned first_array_slice,
                                  unsigned array_size) override {
        m_state.clear_view_formats.push_back(view_format);
        return clear_render_target_level(color, mip_level, first_array_slice,
                                         array_size);
    }
    bool fill_texels(const void* texel, unsigned texel_bytes,
                     unsigned mip_level, unsigned first_array_slice,
                     unsigned array_size, unsigned x, unsigned y,
                     unsigned width, unsigned height) override {
        if (graphics::format_table(m_desc.pixel_format).bytes_per_pixel !=
            texel_bytes) {
            return false;
        }
        const auto* bytes = static_cast<const u8*>(texel);
        m_state.texel_fills.push_back(
            {std::vector<u8>(bytes, bytes + texel_bytes), mip_level, x, y,
             first_array_slice, width, height, array_size, false});
        return true;
    }
    bool discard_storage() override {
        ++m_state.storage_discards;
        return m_desc.cpu_writable;
    }
    bool fill_depth_stencil_texels(u32 depth_bits, bool write_stencil,
                                   u8 stencil, unsigned mip_level,
                                   unsigned first_array_slice,
                                   unsigned array_size, unsigned x, unsigned y,
                                   unsigned width, unsigned height) override {
        // As the Metal backend: blits, so never into a multisampled texture.
        if (!m_desc.depth_stencil || m_desc.sample_count > 1) return false;
        m_state.depth_fills.push_back(
            {depth_bits, write_stencil ? u32{stencil} : ~0u, mip_level,
             first_array_slice, array_size, x, y, width, height});
        return true;
    }
    bool update_depth_stencil_plane(unsigned mip_level, unsigned,
                                    bool stencil, const void* data,
                                    unsigned row_pitch) override {
        if (!graphics::format_table(m_desc.pixel_format).is_depth) {
            return false;
        }
        const u32 w = std::max(1u, m_desc.width >> mip_level);
        const u32 h = std::max(1u, m_desc.height >> mip_level);
        const auto* bytes = static_cast<const u8*>(data);
        if (stencil) {
            m_state.uploaded_stencil_plane.clear();
            for (u32 y = 0; y < h; ++y) {
                m_state.uploaded_stencil_plane.insert(
                    m_state.uploaded_stencil_plane.end(), bytes + y * row_pitch,
                    bytes + y * row_pitch + w);
            }
        } else {
            m_state.uploaded_depth_plane.assign(w * h, 0.0f);
            for (u32 y = 0; y < h; ++y) {
                std::memcpy(m_state.uploaded_depth_plane.data() + y * w,
                            bytes + y * row_pitch, w * sizeof(float));
            }
        }
        return true;
    }
    std::shared_ptr<graphics::BackendReadback> readback_slice_async(
        unsigned mip_level, unsigned) override {
        const auto format = graphics::format_table(m_desc.pixel_format);
        if (!m_state.async_readbacks || format.bytes_per_pixel == 0) {
            return nullptr;
        }
        return std::make_shared<TestReadback>(
            m_state,
            static_cast<usize>(std::max(1u, m_desc.width >> mip_level)) *
                format.bytes_per_pixel,
            std::max(1u, m_desc.height >> mip_level));
    }
    bool clear_depth_stencil(float depth, u8 stencil) override {
        return clear_depth_stencil_level(depth, stencil, 0, 0, 1);
    }
    bool clear_depth_stencil_level(
        float depth, u8 stencil, unsigned mip_level,
        unsigned first_array_slice, unsigned array_size) override {
        return clear_depth_stencil_aspects(
            depth, stencil, true, true, mip_level,
            first_array_slice, array_size);
    }
    bool clear_depth_stencil_aspects(
        float depth, u8 stencil, bool clear_depth, bool clear_stencil,
        unsigned mip_level, unsigned first_array_slice,
        unsigned array_size) override {
        m_state.last_depth_clear_value = depth;
        m_state.last_stencil_clear_value = stencil;
        if (!m_desc.depth_stencil || mip_level >= m_desc.mip_levels ||
            array_size == 0 || first_array_slice >= m_desc.array_size ||
            array_size > m_desc.array_size - first_array_slice) {
            return false;
        }
        ++m_state.depth_clears;
        m_state.last_depth_clear_mip = mip_level;
        m_state.last_depth_clear_slice = first_array_slice;
        m_state.last_depth_clear_array_size = array_size;
        m_state.last_depth_clear_depth = clear_depth;
        m_state.last_depth_clear_stencil = clear_stencil;
        return true;
    }
    bool update(unsigned mip_level, const void* data,
                unsigned row_pitch) override {
        return update_slice(mip_level, 0, data, row_pitch);
    }
    bool update_slice(unsigned mip_level, unsigned slice, const void* data,
                      unsigned row_pitch) override {
        const auto format = graphics::format_table(m_desc.pixel_format);
        if (mip_level >= m_desc.mip_levels ||
            slice >= m_desc.array_size || data == nullptr ||
            format.bytes_per_pixel == 0) {
            return false;
        }
        ++m_state.texture_updates;
        m_state.texture_subresource_updates.push_back({mip_level, slice});
        m_state.texture_subresource_first_bytes.push_back(
            *static_cast<const u8*>(data));
        m_state.last_texture_update_row_pitch = row_pitch;
        const usize size = static_cast<usize>(row_pitch) *
            std::max(1u, m_desc.height >> mip_level);
        const auto* bytes = static_cast<const u8*>(data);
        m_state.last_texture_update.assign(bytes, bytes + size);
        return true;
    }
    bool update_region(unsigned mip_level, unsigned slice, const void* data,
                       unsigned row_pitch, unsigned x, unsigned y,
                       unsigned width, unsigned height) override {
        const auto format = graphics::format_table(m_desc.pixel_format);
        if (mip_level >= m_desc.mip_levels ||
            slice >= m_desc.array_size || data == nullptr ||
            (format.bytes_per_pixel == 0 && !format.is_compressed) ||
            width == 0 || height == 0) {
            return false;
        }
        ++m_state.texture_updates;
        m_state.texture_subresource_updates.push_back({mip_level, slice});
        m_state.texture_subresource_first_bytes.push_back(
            *static_cast<const u8*>(data));
        m_state.last_texture_update_row_pitch = row_pitch;
        m_state.last_texture_update_region = {
            mip_level, slice, x, y, width, height,
        };
        // A compressed region's rows are rows of 4x4 blocks.
        const usize size = static_cast<usize>(row_pitch) *
            (format.is_compressed ? (height + 3u) / 4u : height);
        const auto* bytes = static_cast<const u8*>(data);
        m_state.last_texture_update.assign(bytes, bytes + size);
        return true;
    }
    bool readback(unsigned mip_level, void* data,
                  unsigned row_pitch) override {
        const auto format = graphics::format_table(m_desc.pixel_format);
        const usize row_bytes =
            static_cast<usize>(m_desc.width) * format.bytes_per_pixel;
        const usize source_size = row_bytes * m_desc.height;
        if (mip_level != 0 || data == nullptr || row_pitch < row_bytes ||
            format.bytes_per_pixel == 0 ||
            m_state.native_texture_readback.size() < source_size) {
            return false;
        }
        ++m_state.texture_native_readbacks;
        auto* destination = static_cast<u8*>(data);
        for (u32 row = 0; row < m_desc.height; ++row) {
            std::copy_n(
                m_state.native_texture_readback.data() + row * row_bytes,
                row_bytes, destination + row * row_pitch);
        }
        return true;
    }
    /// Planes that vary per texel so a copy's offsets are visible: depth
    /// (x + 4y) / 16, stencil x + 16y.
    bool readback_depth_stencil_plane(unsigned mip_level, unsigned slice,
                                      bool stencil, void* data,
                                      unsigned row_pitch) override {
        if (!graphics::format_table(m_desc.pixel_format).is_depth ||
            mip_level != 0 || slice >= m_desc.array_size || data == nullptr) {
            return false;
        }
        auto* out = static_cast<u8*>(data);
        for (u32 y = 0; y < m_desc.height; ++y) {
            for (u32 x = 0; x < m_desc.width; ++x) {
                if (stencil) {
                    out[y * row_pitch + x] = static_cast<u8>(x + 16 * y);
                } else {
                    const float d = static_cast<float>(x + 4 * y) / 16.0f;
                    std::memcpy(out + y * row_pitch + x * 4, &d, 4);
                }
            }
        }
        return true;
    }
private:
    graphics::BackendTexture2DDesc m_desc;
    BackendState& m_state;
    u64 m_test_handle{0};
    bool m_storage{false};
};

}  // namespace vitreo::winapi::d3d11_test
