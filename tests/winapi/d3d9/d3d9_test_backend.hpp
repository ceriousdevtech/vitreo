#pragma once

/// @file d3d9_test_backend.hpp
/// @brief The mock graphics backend (render pass, swap chain and device) the
///        D3D9 frontend tests run against, and the float-bits helper.

#include "d3d9_test_backend_resources.hpp"

#include <vitreo/common.hpp>
#include <graphics/contracts/backend_device.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace vitreo::winapi::d3d9_test {

class TestBackendRenderPass final : public graphics::BackendRenderPass {
public:
    TestBackendRenderPass(BackendRecord& record, bool has_depth)
        : m_record(record), m_has_depth(has_depth) {
        ++m_record.render_passes_begun;
    }
    bool valid() const noexcept override { return m_open; }
    bool draw(const graphics::BackendDrawCall& call) override {
        if (!m_open || call.pipeline == nullptr ||
            call.vertex_buffer == nullptr) {
            return false;
        }
        // Mirror Metal: depth testing requires a depth attachment.
        if (call.depth_test_enabled && !m_has_depth) {
            return false;
        }
        if (const unsigned delay = m_record.draw_delay_ms.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        ++m_record.draws;
        m_record.draw_calls.push_back(call);
        m_record.draw_vertex_constant_pointers.push_back(
            call.vertex_constants);
        m_record.draw_fragment_constant_pointers.push_back(
            call.fragment_constants);
        const auto copy = [](const void* data, usize size) {
            const auto* bytes = static_cast<const u8*>(data);
            return data != nullptr ? std::vector<u8>(bytes, bytes + size)
                                   : std::vector<u8>{};
        };
        m_record.draw_vertex_constants.push_back(
            copy(call.vertex_constants, call.vertex_constants_size));
        m_record.draw_fragment_constants.push_back(
            copy(call.fragment_constants, call.fragment_constants_size));
        m_record.draws_finished.fetch_add(1);
        return true;
    }
    bool end() override {
        if (!m_open) return false;
        m_open = false;
        ++m_record.render_passes_ended;
        return true;
    }

private:
    BackendRecord& m_record;
    bool m_has_depth{false};
    bool m_open{true};
};

class TestBackendSwapChain final : public graphics::BackendSwapChain {
public:
    TestBackendSwapChain(graphics::BackendSwapChainDesc desc,
                         BackendRecord& record)
        : m_desc(desc), m_record(record) {}
    bool valid() const noexcept override { return true; }
    const graphics::BackendSwapChainDesc& desc() const noexcept override {
        return m_desc;
    }
    bool clear_back_buffer(const std::array<float, 4>& color) override {
        ++m_record.back_buffer_clears;
        m_record.last_clear = color;
        return true;
    }
    bool present(unsigned, unsigned) override {
        ++m_record.presents;
        return true;
    }
    std::unique_ptr<graphics::BackendRenderPass> begin_render_pass(
        bool, bool clear_depth, float depth_clear_value,
        const std::array<graphics::BackendTexture2D*,
                         graphics::BACKEND_MAX_RENDER_TARGETS>&
            color_targets) override {
        m_record.last_swapchain_color_target_count = 1;
        for (unsigned slot = 1; slot < color_targets.size(); ++slot) {
            if (color_targets[slot] != nullptr) {
                m_record.last_swapchain_color_target_count = slot + 1;
            }
        }
        if (clear_depth) {
            ++m_record.depth_clearing_passes;
            m_record.last_depth_clear = depth_clear_value;
        }
        return std::make_unique<TestBackendRenderPass>(m_record,
                                                       m_desc.depth_stencil);
    }
    bool resize(unsigned width, unsigned height,
                graphics::PixelFormat format, unsigned count) override {
        m_desc.width = width;
        m_desc.height = height;
        m_desc.pixel_format = format;
        m_desc.buffer_count = count;
        return true;
    }
    bool readback_texture_rgba(graphics::BackendTexture2D* /*source*/,
                               std::vector<u8>& rgba) override {
        // Expose the last clear as a solid front-buffer image for
        // GetFrontBufferData contract tests.
        const usize bytes =
            static_cast<usize>(m_desc.width) * m_desc.height * 4u;
        rgba.assign(bytes, 0);
        const auto& c = m_record.last_clear;
        const u8 px[4] = {
            static_cast<u8>(std::lround(std::clamp(c[0], 0.0f, 1.0f) * 255.0f)),
            static_cast<u8>(std::lround(std::clamp(c[1], 0.0f, 1.0f) * 255.0f)),
            static_cast<u8>(std::lround(std::clamp(c[2], 0.0f, 1.0f) * 255.0f)),
            static_cast<u8>(std::lround(std::clamp(c[3], 0.0f, 1.0f) * 255.0f)),
        };
        for (usize i = 0; i < bytes; i += 4) {
            rgba[i + 0] = px[0];
            rgba[i + 1] = px[1];
            rgba[i + 2] = px[2];
            rgba[i + 3] = px[3];
        }
        return true;
    }

private:
    graphics::BackendSwapChainDesc m_desc;
    BackendRecord& m_record;
};

class TestGraphicsBackend final : public graphics::GraphicsBackendDevice {
public:
    BackendRecord record;

    [[nodiscard]] bool available() const noexcept override { return true; }
    [[nodiscard]] bool supports_texture_sample_count(
        unsigned count) const override {
        return count == 1 || count == 2 || count == 4 || count == 8;
    }
    [[nodiscard]] std::string backend_device_name() const override {
        return "test-gpu";
    }
    [[nodiscard]] void* native_device() const noexcept override {
        return nullptr;
    }
    [[nodiscard]] void* native_command_queue() const noexcept override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendBuffer>
    create_buffer(const graphics::BackendBufferDesc& desc,
                  const void*) override {
        return std::make_unique<TestBackendBuffer>(desc, record);
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendTexture2D>
    create_texture_2d(const graphics::BackendTexture2DDesc& desc,
                      const void*, unsigned) override {
        record.last_texture_desc = desc;
        return std::make_unique<TestBackendTexture>(desc, record);
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendTexture3D>
    create_texture_3d(const graphics::BackendTexture3DDesc& desc,
                      const void*, unsigned, unsigned) override {
        ++record.texture_3d_creates;
        record.last_texture_3d_desc = desc;
        return std::make_unique<TestBackendTexture3D>(desc, record);
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendPipeline>
    create_pipeline(const graphics::BackendPipelineDesc& desc) override {
        if (record.fail_pipeline) return nullptr;
        ++record.pipelines_created;
        record.last_pipeline_desc = desc;
        return std::make_unique<TestBackendPipeline>(desc);
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendComputePipeline>
    create_compute_pipeline(const graphics::BackendComputePipelineDesc&) override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendRenderPass>
    create_render_pass_standalone(const graphics::BackendRenderPassDesc& desc) override {
        if (desc.color_targets[0] == nullptr) return nullptr;
        ++record.standalone_passes;
        record.last_standalone_had_depth = desc.depth_stencil_target != nullptr;
        record.last_standalone_depth_format = desc.depth_stencil_target
            ? desc.depth_stencil_target->desc().pixel_format : graphics::PixelFormat::Unknown;
        record.last_standalone_cleared_depth = desc.clear_depth;
        record.last_standalone_color_slice = desc.color_slices[0];
        return std::make_unique<TestBackendRenderPass>(
            record, desc.depth_stencil_target != nullptr);
    }
    [[nodiscard]] bool dispatch(const graphics::BackendDispatchCall&) override {
        return false;
    }
    [[nodiscard]] bool copy_buffer(graphics::BackendBuffer*,
                                   const graphics::BackendBuffer*) override {
        return false;
    }
    [[nodiscard]] bool copy_texture_2d(
        graphics::BackendTexture2D*,
        const graphics::BackendTexture2D*) override {
        return false;
    }
    [[nodiscard]] bool copy_texture_2d_region(
        graphics::BackendTexture2D* dst, unsigned dst_x, unsigned dst_y,
        const graphics::BackendTexture2D* src, unsigned src_x, unsigned src_y,
        unsigned width, unsigned height, unsigned dst_mip,
        unsigned dst_slice, unsigned src_mip,
        unsigned src_slice) override {
        if (dst == nullptr || src == nullptr || width == 0 || height == 0) {
            return false;
        }
        ++record.texture_region_copies;
        record.last_region_dst_x = dst_x;
        record.last_region_dst_y = dst_y;
        record.last_region_src_x = src_x;
        record.last_region_src_y = src_y;
        record.last_region_width = width;
        record.last_region_height = height;
        record.last_region_dst_slice = dst_slice;
        record.last_region_src_slice = src_slice;
        (void)dst_mip;
        (void)src_mip;
        return true;
    }
    [[nodiscard]] bool stretch_texture_2d_region(
        graphics::BackendTexture2D* dst, unsigned dst_x, unsigned dst_y,
        unsigned dst_width, unsigned dst_height,
        const graphics::BackendTexture2D* src, unsigned src_x, unsigned src_y,
        unsigned src_width, unsigned src_height,
        graphics::BackendFilter filter, unsigned dst_slice,
        unsigned src_slice) override {
        if (dst == nullptr || src == nullptr || dst_width == 0 ||
            dst_height == 0 || src_width == 0 || src_height == 0) {
            return false;
        }
        ++record.texture_stretches;
        record.last_stretch_dst_x = dst_x;
        record.last_stretch_dst_y = dst_y;
        record.last_stretch_dst_width = dst_width;
        record.last_stretch_dst_height = dst_height;
        record.last_stretch_src_x = src_x;
        record.last_stretch_src_y = src_y;
        record.last_stretch_src_width = src_width;
        record.last_stretch_src_height = src_height;
        record.last_stretch_dst_slice = dst_slice;
        record.last_stretch_src_slice = src_slice;
        record.last_stretch_filter = filter;
        return true;
    }
    [[nodiscard]] bool generate_mips(
        graphics::BackendTexture2D* texture) override {
        if (texture == nullptr || !texture->valid() ||
            texture->desc().mip_levels <= 1) {
            return false;
        }
        ++record.texture_2d_mip_generations;
        return true;
    }
    [[nodiscard]] bool generate_mips(
        graphics::BackendTexture3D* texture) override {
        if (texture == nullptr || !texture->valid() ||
            texture->desc().mip_levels <= 1) {
            return false;
        }
        ++record.texture_3d_mip_generations;
        return true;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendSwapChain>
    create_swap_chain(const graphics::BackendSwapChainDesc& desc) override {
        if (record.fail_swap_chain) return nullptr;
        ++record.swap_chains_created;
        record.last_swap_chain_desc = desc;
        return std::make_unique<TestBackendSwapChain>(desc, record);
    }
    void flush() override {}
};

/// Reinterpret a float as the DWORD form D3D9 state getters report.
[[nodiscard]] inline u32 test_f32_bits(float value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

} // namespace vitreo::winapi::d3d9_test
