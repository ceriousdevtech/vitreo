#pragma once

/// @file d3d9_test_backend_resources.hpp
/// @brief Recording backend resources (buffer, textures, pipeline) behind the
///        mock graphics backend the D3D9 frontend tests run against.

#include <vitreo/common.hpp>
#include <graphics/contracts/backend_device.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace vitreo::winapi::d3d9_test {

/// What the frontend asked the shared backend to do. The point of most of
/// these tests is that a D3D9 call produced the real backend side effect, not
/// merely a success HRESULT.
struct BackendRecord {
    unsigned swap_chains_created{0};
    unsigned presents{0};
    unsigned back_buffer_clears{0};
    unsigned texture_render_target_clears{0};
    unsigned texture_updates{0};
    unsigned buffer_updates{0};
    unsigned pipelines_created{0};
    unsigned render_passes_begun{0};
    unsigned render_passes_ended{0};
    unsigned depth_clearing_passes{0};
    float last_depth_clear{0.0f};
    unsigned draws{0};
    unsigned standalone_passes{0};
    unsigned last_swapchain_color_target_count{0};
    bool last_standalone_had_depth{false};
    graphics::PixelFormat last_standalone_depth_format{graphics::PixelFormat::Unknown};
    bool last_standalone_cleared_depth{false};
    unsigned last_standalone_color_slice{0};
    graphics::BackendPipelineDesc last_pipeline_desc{};
    std::vector<graphics::BackendDrawCall> draw_calls;
    std::vector<const void*> draw_vertex_constant_pointers;
    std::vector<const void*> draw_fragment_constant_pointers;
    /// Copies of each draw's constants: the pointers in `draw_calls` are only
    /// valid during the draw, so anything asserted later must be kept here.
    std::vector<std::vector<u8>> draw_vertex_constants;
    std::vector<std::vector<u8>> draw_fragment_constants;
    /// Set to make the next create_pipeline fail.
    bool fail_pipeline{false};
    std::array<float, 4> last_clear{};
    graphics::BackendSwapChainDesc last_swap_chain_desc{};
    graphics::BackendTexture2DDesc last_texture_desc{};
    graphics::BackendTexture3DDesc last_texture_3d_desc{};
    std::vector<u8> last_buffer_update;
    usize last_buffer_update_offset{0};
    graphics::BufferWrite last_buffer_update_mode{};
    /// Draws the command-stream worker has finished, and how many had when
    /// the last buffer update reached the backend. A draw waits
    /// draw_delay_ms first, standing in for a worker that is still
    /// recording when the API thread moves on.
    std::atomic<unsigned> draws_finished{0};
    unsigned draws_finished_at_buffer_update{0};
    std::atomic<unsigned> draw_delay_ms{0};
    std::vector<u8> last_texture_update;
    unsigned last_texture_update_level{0};
    unsigned last_texture_update_slice{0};
    std::vector<u8> last_texture_3d_update;
    unsigned last_texture_3d_update_level{0};
    unsigned last_texture_3d_update_row_pitch{0};
    unsigned last_texture_3d_update_slice_pitch{0};
    unsigned texture_3d_creates{0};
    unsigned texture_3d_updates{0};
    unsigned texture_3d_mip_generations{0};
    unsigned texture_2d_mip_generations{0};
    /// Set to make the next create_swap_chain fail.
    bool fail_swap_chain{false};
    unsigned texture_region_copies{0};
    unsigned last_region_dst_x{0};
    unsigned last_region_dst_y{0};
    unsigned last_region_src_x{0};
    unsigned last_region_src_y{0};
    unsigned last_region_width{0};
    unsigned last_region_height{0};
    unsigned last_region_src_slice{0};
    unsigned last_region_dst_slice{0};
    unsigned last_clear_slice{0};
    unsigned last_readback_slice{0};
    unsigned texture_stretches{0};
    unsigned last_stretch_dst_x{0};
    unsigned last_stretch_dst_y{0};
    unsigned last_stretch_dst_width{0};
    unsigned last_stretch_dst_height{0};
    unsigned last_stretch_src_x{0};
    unsigned last_stretch_src_y{0};
    unsigned last_stretch_src_width{0};
    unsigned last_stretch_src_height{0};
    unsigned last_stretch_src_slice{0};
    unsigned last_stretch_dst_slice{0};
    graphics::BackendFilter last_stretch_filter{
        graphics::BackendFilter::Nearest};
};

class TestBackendBuffer final : public graphics::BackendBuffer {
public:
    TestBackendBuffer(graphics::BackendBufferDesc desc, BackendRecord& record)
        : m_desc(desc), m_record(record) {}
    bool valid() const noexcept override { return true; }
    const graphics::BackendBufferDesc& desc() const noexcept override {
        return m_desc;
    }
    bool update(usize offset, const void* data, usize length,
                graphics::BufferWrite mode) override {
        if (offset + length > m_desc.byte_width) return false;
        ++m_record.buffer_updates;
        m_record.last_buffer_update_offset = offset;
        m_record.last_buffer_update_mode = mode;
        m_record.draws_finished_at_buffer_update =
            m_record.draws_finished.load();
        const auto* bytes = static_cast<const u8*>(data);
        m_record.last_buffer_update.assign(bytes, bytes + length);
        return true;
    }

private:
    graphics::BackendBufferDesc m_desc;
    BackendRecord& m_record;
};

class TestBackendTexture final : public graphics::BackendTexture2D {
public:
    TestBackendTexture(graphics::BackendTexture2DDesc desc,
                       BackendRecord& record)
        : m_desc(desc), m_record(record) {}
    bool valid() const noexcept override { return true; }
    const graphics::BackendTexture2DDesc& desc() const noexcept override {
        return m_desc;
    }
    bool clear_render_target(const std::array<float, 4>& color) override {
        if (!m_desc.render_target) return false;
        ++m_record.texture_render_target_clears;
        m_record.last_clear_slice = 0;
        m_record.last_clear = color;
        m_clear = color;
        m_has_clear = true;
        m_pixels.clear();
        return true;
    }
    bool clear_render_target_level(const std::array<float, 4>& color,
                                   unsigned mip_level,
                                   unsigned first_array_slice,
                                   unsigned array_size) override {
        if (!m_desc.render_target || mip_level != 0 || array_size != 1) {
            return false;
        }
        if (first_array_slice >= m_desc.array_size) return false;
        ++m_record.texture_render_target_clears;
        m_record.last_clear_slice = first_array_slice;
        m_record.last_clear = color;
        m_clear = color;
        m_has_clear = true;
        m_pixels.clear();
        return true;
    }
    bool clear_depth_stencil(float, u8) override {
        return m_desc.depth_stencil;
    }
    bool update(unsigned mip_level, const void* data,
                unsigned row_pitch) override {
        if (data == nullptr) return false;
        ++m_record.texture_updates;
        m_record.last_texture_update_level = mip_level;
        m_record.last_texture_update_slice = 0;
        unsigned level_height = m_desc.height >> mip_level;
        if (level_height == 0) level_height = 1;
        if (graphics::format_table(m_desc.pixel_format).is_compressed)
            level_height = (level_height + 3u) / 4u;
        const auto* bytes = static_cast<const u8*>(data);
        m_record.last_texture_update.assign(
            bytes, bytes + static_cast<usize>(row_pitch) * level_height);
        // Retain level-0 BGRA for GetRenderTargetData contract tests.
        if (mip_level == 0 &&
            m_desc.pixel_format == graphics::PixelFormat::BGRA8Unorm) {
            m_pixels.assign(bytes,
                            bytes + static_cast<usize>(row_pitch) * level_height);
            m_row_pitch = row_pitch;
            m_has_clear = false;
        }
        return true;
    }
    bool update_slice(unsigned mip_level, unsigned slice, const void* data,
                      unsigned row_pitch) override {
        if (data == nullptr || slice >= m_desc.array_size) return false;
        if (!update(mip_level, data, row_pitch)) return false;
        m_record.last_texture_update_slice = slice;
        return true;
    }
    bool readback(unsigned mip_level, void* data,
                  unsigned row_pitch) override {
        if (data == nullptr || mip_level != 0 || row_pitch < m_desc.width * 4u ||
            m_desc.pixel_format != graphics::PixelFormat::BGRA8Unorm) {
            return false;
        }
        auto* out = static_cast<u8*>(data);
        if (m_has_clear) {
            const u8 px[4] = {
                static_cast<u8>(std::lround(
                    std::clamp(m_clear[2], 0.0f, 1.0f) * 255.0f)),
                static_cast<u8>(std::lround(
                    std::clamp(m_clear[1], 0.0f, 1.0f) * 255.0f)),
                static_cast<u8>(std::lround(
                    std::clamp(m_clear[0], 0.0f, 1.0f) * 255.0f)),
                static_cast<u8>(std::lround(
                    std::clamp(m_clear[3], 0.0f, 1.0f) * 255.0f)),
            };
            for (unsigned y = 0; y < m_desc.height; ++y) {
                u8* row = out + static_cast<usize>(y) * row_pitch;
                for (unsigned x = 0; x < m_desc.width; ++x) {
                    std::memcpy(row + static_cast<usize>(x) * 4u, px, 4);
                }
            }
            return true;
        }
        if (m_pixels.empty() || m_row_pitch == 0) return false;
        for (unsigned y = 0; y < m_desc.height; ++y) {
            const u8* src =
                m_pixels.data() + static_cast<usize>(y) * m_row_pitch;
            u8* dst = out + static_cast<usize>(y) * row_pitch;
            std::memcpy(dst, src, static_cast<usize>(m_desc.width) * 4u);
        }
        return true;
    }
    bool readback_slice(unsigned mip_level, unsigned slice, void* data,
                        unsigned row_pitch) override {
        m_record.last_readback_slice = slice;
        return readback(mip_level, data, row_pitch);
    }

private:
    graphics::BackendTexture2DDesc m_desc;
    BackendRecord& m_record;
    std::array<float, 4> m_clear{};
    bool m_has_clear{false};
    std::vector<u8> m_pixels;
    unsigned m_row_pitch{0};
};

class TestBackendTexture3D final : public graphics::BackendTexture3D {
public:
    TestBackendTexture3D(graphics::BackendTexture3DDesc desc,
                         BackendRecord& record)
        : m_desc(desc), m_record(record) {}
    bool valid() const noexcept override { return true; }
    const graphics::BackendTexture3DDesc& desc() const noexcept override {
        return m_desc;
    }
    bool update(unsigned mip_level, const void* data, unsigned row_pitch,
                unsigned slice_pitch) override {
        if (data == nullptr) return false;
        ++m_record.texture_3d_updates;
        m_record.last_texture_3d_update_level = mip_level;
        m_record.last_texture_3d_update_row_pitch = row_pitch;
        m_record.last_texture_3d_update_slice_pitch = slice_pitch;
        unsigned level_height = m_desc.height >> mip_level;
        unsigned level_depth = m_desc.depth >> mip_level;
        if (level_height == 0) level_height = 1;
        if (level_depth == 0) level_depth = 1;
        const auto* bytes = static_cast<const u8*>(data);
        m_record.last_texture_3d_update.assign(
            bytes,
            bytes + static_cast<usize>(slice_pitch) * level_depth);
        return true;
    }

private:
    graphics::BackendTexture3DDesc m_desc;
    BackendRecord& m_record;
};

class TestBackendPipeline final : public graphics::BackendPipeline {
public:
    explicit TestBackendPipeline(graphics::BackendPipelineDesc desc)
        : m_desc(std::move(desc)) {}
    bool valid() const noexcept override { return true; }
    const graphics::BackendPipelineDesc& desc() const noexcept {
        return m_desc;
    }

private:
    graphics::BackendPipelineDesc m_desc;
};

} // namespace vitreo::winapi::d3d9_test
