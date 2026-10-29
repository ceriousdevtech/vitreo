// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file d3d11_test_backend_buffers.hpp
/// @brief Fake backend resources: tile pool, volume/typed-buffer textures, buffers, shared memory, readback.

#include "d3d11_test_backend_state.hpp"

namespace vitreo::winapi::d3d11_test {

/// A tile pool that counts references per pool tile (BackendTilePool).
class TestTilePool final : public graphics::BackendTilePool {
public:
    explicit TestTilePool(BackendState& state) : m_state(state) {}
    ~TestTilePool() override {
        m_state.tile_pool_refs.erase(this);
        m_state.tile_pool_sizes.erase(this);
    }
    bool reference(u64 tile) override {
        if (m_state.tile_pool_out_of_memory) return false;
        ++m_state.tile_pool_refs[this][tile];
        return true;
    }
    void unreference(u64 tile) override {
        auto& refs = m_state.tile_pool_refs[this];
        auto it = refs.find(tile);
        if (it != refs.end() && --it->second == 0) refs.erase(it);
    }
    void set_size(u64 tiles) override { m_state.tile_pool_sizes[this] = tiles; }

private:
    BackendState& m_state;
};

class TestBackendTexture3D final : public graphics::BackendTexture3D {
public:
    explicit TestBackendTexture3D(graphics::BackendTexture3DDesc desc,
                                  BackendState* state = nullptr)
        : m_desc(desc), m_state(state) {}
    bool update_region(unsigned mip_level, const void* data,
                       unsigned row_pitch, unsigned slice_pitch, unsigned x,
                       unsigned y, unsigned z, unsigned width,
                       unsigned height, unsigned depth) override {
        if (m_state == nullptr || data == nullptr) return false;
        m_state->volume_region_updates.push_back(
            {mip_level, x, y, z, width, height, depth, row_pitch,
             slice_pitch});
        const auto* bytes = static_cast<const u8*>(data);
        m_state->last_volume_update.assign(
            bytes, bytes + static_cast<usize>(slice_pitch) * depth);
        return true;
    }
    bool fill_texels(const void* texel, unsigned texel_bytes,
                     unsigned mip_level, unsigned x, unsigned y, unsigned z,
                     unsigned width, unsigned height,
                     unsigned depth) override {
        if (m_state == nullptr ||
            graphics::format_table(m_desc.pixel_format).bytes_per_pixel !=
                texel_bytes) {
            return false;
        }
        const auto* bytes = static_cast<const u8*>(texel);
        m_state->texel_fills.push_back(
            {std::vector<u8>(bytes, bytes + texel_bytes), mip_level, x, y, z,
             width, height, depth, true});
        return true;
    }
    bool valid() const noexcept override { return true; }
    const graphics::BackendTexture3DDesc& desc() const noexcept override {
        return m_desc;
    }
    /// As the Metal backend: a same-size format or a mip window is a view
    /// (kept by the volume); anything else is refused.
    graphics::BackendTexture3D* view(graphics::PixelFormat format,
                                     unsigned first_mip,
                                     unsigned mip_count) override {
        if (format == graphics::PixelFormat::Unknown) {
            format = m_desc.pixel_format;
        }
        if (mip_count == 0 || first_mip >= m_desc.mip_levels ||
            mip_count > m_desc.mip_levels - first_mip ||
            graphics::format_table(format).bytes_per_pixel !=
                graphics::format_table(m_desc.pixel_format).bytes_per_pixel) {
            return nullptr;
        }
        if (format == m_desc.pixel_format && first_mip == 0 &&
            mip_count == m_desc.mip_levels) {
            return this;
        }
        graphics::BackendTexture3DDesc view_desc = m_desc;
        view_desc.pixel_format = format;
        view_desc.width = std::max(1u, m_desc.width >> first_mip);
        view_desc.height = std::max(1u, m_desc.height >> first_mip);
        view_desc.depth = std::max(1u, m_desc.depth >> first_mip);
        view_desc.mip_levels = mip_count;
        m_views.push_back(
            std::make_unique<TestBackendTexture3D>(view_desc, m_state));
        return m_views.back().get();
    }
    bool update(unsigned, const void*, unsigned, unsigned) override {
        return true;
    }
    /// 32-bit texels that vary per (mip, w, y, x) so a readback's slice and
    /// row addressing is visible: mip*1000 + w*100 + y*10 + x.
    bool readback_w_slice(unsigned mip_level, unsigned w_slice, void* data,
                          unsigned row_pitch) override {
        const auto format = graphics::format_table(m_desc.pixel_format);
        const u32 w = std::max(1u, m_desc.width >> mip_level);
        const u32 h = std::max(1u, m_desc.height >> mip_level);
        if (format.bytes_per_pixel != 4 || data == nullptr ||
            row_pitch < w * 4 || mip_level >= m_desc.mip_levels ||
            w_slice >= std::max(1u, m_desc.depth >> mip_level)) {
            return false;
        }
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const u32 value = mip_level * 1000u + w_slice * 100u + y * 10u + x;
                std::memcpy(static_cast<u8*>(data) + y * row_pitch + x * 4,
                            &value, sizeof(value));
            }
        }
        return true;
    }
private:
    graphics::BackendTexture3DDesc m_desc;
    BackendState* m_state{nullptr};
    std::vector<std::unique_ptr<TestBackendTexture3D>> m_views;
};

class TestBackendTextureBuffer final : public graphics::BackendTextureBuffer {
public:
    bool valid() const noexcept override { return true; }
};

class TestBackendBuffer final : public graphics::BackendBuffer {
public:
    TestBackendBuffer(graphics::BackendBufferDesc desc,
                      BackendState& state, std::vector<u8> contents = {},
                      u64 shared_id = 0)
        : m_desc(desc), m_state(state), m_contents(std::move(contents)),
          m_shared_id(shared_id) {
        if (m_desc.shareable && m_shared_id == 0 && state.extended_sharing) {
            m_shared_id = ++state.next_shared_id;
            state.shared_buffers[m_shared_id] = m_desc;
        }
    }
    u64 shared_handle() const noexcept override { return m_shared_id; }
    const std::vector<u8>& shared_metadata() const noexcept override {
        return m_desc.shared_metadata;
    }
    bool valid() const noexcept override { return true; }
    /// Initial data, for tests that inspect what a buffer was created with.
    [[nodiscard]] const std::vector<u8>& contents() const { return m_contents; }
    /// What the buffer holds now (BackendState::live_buffers).
    [[nodiscard]] std::vector<u8>& live() const {
        if (m_live.size() < m_desc.byte_width) {
            m_live = m_contents;
            m_live.resize(m_desc.byte_width, 0);
        }
        return m_live;
    }
    [[nodiscard]] u32* live_words(usize offset) const {
        return reinterpret_cast<u32*>(live().data() + offset);
    }
    const graphics::BackendBufferDesc& desc() const noexcept override {
        return m_desc;
    }
    bool update(usize offset, const void* data, usize length,
                graphics::BufferWrite mode) override {
        if (data == nullptr || offset > m_desc.byte_width ||
            length > m_desc.byte_width - offset) {
            return false;
        }
        ++m_state.buffer_updates;
        m_state.last_buffer_update_mode = mode;
        m_state.last_buffer_update_offset = offset;
        const auto* bytes = static_cast<const u8*>(data);
        m_state.last_buffer_update.assign(bytes, bytes + length);
        std::memcpy(live().data() + offset, data, length);
        return true;
    }
    bool read(usize offset, void* data, usize length) override {
        if (data == nullptr || offset > m_desc.byte_width ||
            length > m_desc.byte_width - offset) {
            return false;
        }
        ++m_state.buffer_reads;
        if (m_state.live_buffers) {
            std::memcpy(data, live().data() + offset, length);
            return true;
        }
        auto* bytes = static_cast<u8*>(data);
        for (usize i = 0; i < length; ++i) {
            bytes[i] = static_cast<u8>(0xC0u ^ (offset + i));
        }
        return true;
    }
private:
    graphics::BackendBufferDesc m_desc;
    BackendState& m_state;
    std::vector<u8> m_contents;
    mutable std::vector<u8> m_live;
    u64 m_shared_id{0};
};

/// Shared CPU memory of the extended-sharing mode: one byte vector every
/// opening of the id maps.
class TestSharedMemory final : public graphics::BackendSharedMemory {
public:
    TestSharedMemory(u64 id, usize bytes, std::vector<u8> metadata,
                     BackendState& state)
        : m_id(id), m_bytes(bytes), m_metadata(std::move(metadata)),
          m_state(state) {}
    u64 shared_handle() const noexcept override { return m_id; }
    u8* data() noexcept override { return m_bytes.data(); }
    usize size() const noexcept override { return m_bytes.size(); }
    const std::vector<u8>& shared_metadata() const noexcept override {
        return m_metadata;
    }
    bool acquire() override {
        ++m_state.shared_acquires;
        return true;
    }
    void note_written() override { ++m_state.shared_publications; }

private:
    u64 m_id;
    std::vector<u8> m_bytes;
    std::vector<u8> m_metadata;
    BackendState& m_state;
};

class TestReadback final : public graphics::BackendReadback {
public:
    TestReadback(BackendState& state, usize row_bytes, u32 rows)
        : m_state(state), m_row_bytes(row_bytes), m_rows(rows) {}
    bool ready() override { return m_state.readbacks_ready; }
    bool wait() override {
        ++m_state.readback_waits;
        m_state.readbacks_ready = true;
        return true;
    }
    bool read(void* data, unsigned row_pitch) override {
        if (!m_state.readbacks_ready || row_pitch < m_row_bytes) return false;
        for (u32 y = 0; y < m_rows; ++y) {
            std::memset(static_cast<u8*>(data) + y * row_pitch, 0x5A,
                        m_row_bytes);
        }
        return true;
    }
private:
    BackendState& m_state;
    usize m_row_bytes{0};
    u32 m_rows{0};
};

}  // namespace vitreo::winapi::d3d11_test
