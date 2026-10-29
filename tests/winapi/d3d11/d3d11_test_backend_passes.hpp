// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file d3d11_test_backend_passes.hpp
/// @brief Fake backend pipelines, render pass and swap chain.

#include "d3d11_test_backend_texture.hpp"

namespace vitreo::winapi::d3d11_test {

class TestBackendPipeline final : public graphics::BackendPipeline {
public:
    explicit TestBackendPipeline(graphics::BackendPipelineDesc desc)
        : m_desc(std::move(desc)) {}
    bool valid() const noexcept override { return true; }
    const graphics::BackendPipelineDesc& desc() const { return m_desc; }
private:
    graphics::BackendPipelineDesc m_desc;
};

class TestBackendComputePipeline final
    : public graphics::BackendComputePipeline {
public:
    explicit TestBackendComputePipeline(std::string entry = {})
        : entry(std::move(entry)) {}
    bool valid() const noexcept override { return true; }
    std::string entry;
};

class TestBackendRenderPass final : public graphics::BackendRenderPass {
public:
    explicit TestBackendRenderPass(BackendState& state) : m_state(state) {}
    bool valid() const noexcept override { return m_open; }
    bool draw(const graphics::BackendDrawCall& call) override {
        // R3c: every native state a draw names was built from exactly the
        // desc it is paired with.
        const graphics::BackendNativeStates no_states{};
        const graphics::BackendNativeStates& states =
            call.native_states != nullptr ? *call.native_states : no_states;
        if (states.depth_stencil != nullptr) {
            ++m_state.draws_naming_depth_stencil_state;
            if (!graphics::same_depth_stencil_desc(
                    states.depth_stencil->desc(),
                    graphics::depth_stencil_desc_of(call))) {
                ++m_state.native_state_mismatches;
            }
        }
        m_state.last_depth_stencil_state = states.depth_stencil;
        bool names_sampler = false;
        for (u32 i = 0; i < graphics::BACKEND_MAX_TEXTURE_SLOTS; ++i) {
            if (states.samplers[i] != nullptr) {
                names_sampler = true;
                if (!graphics::same_sampler_desc(states.samplers[i]->desc(),
                                                 call.samplers[i])) {
                    ++m_state.native_state_mismatches;
                }
            }
            if (states.vertex_samplers[i] != nullptr &&
                !graphics::same_sampler_desc(states.vertex_samplers[i]->desc(),
                                             call.vertex_samplers[i])) {
                ++m_state.native_state_mismatches;
            }
        }
        if (names_sampler) ++m_state.draws_naming_sampler_state;
        if (!m_open) return false;
        if (m_state.before_draw) m_state.before_draw();
        ++m_state.draws;
        if (call.indirect.buffer != nullptr) ++m_state.indirect_draws;
        m_state.last_draw_topology = call.topology;
        m_state.last_draw_vertex_srv0 = call.vertex_srv_buffers[0];
        m_state.last_draw_vertex_buffers = call.vertex_buffers;
        m_state.last_draw_vertex_buffer_offsets = call.vertex_buffer_offsets;
        m_state.last_draw_vertex_srv1 = call.vertex_srv_buffers[1];
        m_state.last_draw_vertex_srv1_bytes =
            call.vertex_srv_buffers[1] != nullptr
                ? static_cast<const TestBackendBuffer*>(
                      call.vertex_srv_buffers[1])->contents()
                : std::vector<u8>{};
        m_state.last_draw_vertex_srv2_bytes =
            call.vertex_srv_buffers[2] != nullptr
                ? static_cast<const TestBackendBuffer*>(
                      call.vertex_srv_buffers[2])->contents()
                : std::vector<u8>{};
        m_state.last_draw_vertex_count = call.vertex_count;
        m_state.last_draw_patch_count = call.patch_count;
        m_state.last_draw_patch_control_points = call.patch_control_points;
        m_state.last_draw_tess_factors = call.tessellation_factors;
        if (call.vertex_capture_buffer != nullptr) {
            ++m_state.capture_draws;
            m_state.last_capture_buffer = call.vertex_capture_buffer;
            m_state.last_capture_offset = call.vertex_capture_buffer_offset;
            m_state.last_capture_vertex_count =
                call.index_buffer != nullptr ? call.index_count
                                             : call.vertex_count;
            m_state.last_capture_first_instance = call.first_instance;
            m_state.last_capture_buffer_bytes =
                call.vertex_capture_buffer->desc().byte_width;
            m_state.last_capture_vertex_offsets = call.vertex_buffer_offsets;
            m_state.last_capture_call = call;
            if (call.pipeline != nullptr) {
                m_state.last_capture_pipeline_desc =
                    static_cast<const TestBackendPipeline*>(call.pipeline)
                        ->desc();
            }
            const auto& bytes = static_cast<const TestBackendBuffer*>(
                                    call.vertex_capture_buffer)->live();
            m_state.last_capture_header.assign(
                bytes.begin(),
                bytes.begin() + std::min<usize>(bytes.size(), 16u));
        }
        m_state.last_draw_mipmaps_enabled = call.samplers[0].mipmaps_enabled;
        m_state.last_draw_comparison_enabled =
            call.samplers[0].comparison_enabled;
        m_state.last_draw_sampler_comparison = call.samplers[0].comparison;
        m_state.last_draw_min_filter = call.samplers[0].min_filter;
        m_state.last_draw_mag_filter = call.samplers[0].mag_filter;
        m_state.last_draw_mip_filter = call.samplers[0].mip_filter;
        m_state.last_draw_max_anisotropy = call.samplers[0].max_anisotropy;
        m_state.last_draw_address_u = call.samplers[0].address_u;
        m_state.last_draw_address_v = call.samplers[0].address_v;
        m_state.last_draw_address_w = call.samplers[0].address_w;
        m_state.last_draw_sampler_border_color = call.samplers[0].border_color;
        m_state.last_draw_fill_mode = call.fill_mode;
        m_state.last_draw_cull_mode = call.cull_mode;
        m_state.last_draw_depth_clip_enabled = call.depth_clip_enabled;
        m_state.last_draw_depth_bias = call.depth_bias;
        m_state.last_draw_depth_bias_slope_scale =
            call.depth_bias_slope_scale;
        m_state.last_draw_depth_bias_clamp = call.depth_bias_clamp;
        m_state.last_draw_scissor_enabled = call.scissor_enabled;
        m_state.last_draw_call = call;
        m_state.last_draw_index_bytes.clear();
        if (call.index_buffer != nullptr) {
            m_state.last_draw_index_bytes =
                static_cast<const TestBackendBuffer*>(call.index_buffer)
                    ->contents();
        }
        for (unsigned slot = 0; slot < graphics::BACKEND_MAX_VERTEX_BUFFERS;
             ++slot) {
            m_state.last_draw_vertex_stream_bytes[slot].clear();
            m_state.last_draw_vertex_stream_widths[slot] =
                call.vertex_buffers[slot] != nullptr
                    ? call.vertex_buffers[slot]->desc().byte_width
                    : 0u;
            if (call.vertex_buffers[slot] != nullptr) {
                m_state.last_draw_vertex_stream_bytes[slot] =
                    static_cast<const TestBackendBuffer*>(
                        call.vertex_buffers[slot])->contents();
            }
        }
        if (call.pipeline != nullptr) {
            m_state.last_draw_pipeline_desc =
                static_cast<const TestBackendPipeline*>(call.pipeline)->desc();
        }
        m_state.last_draw_lod_min_clamp = call.samplers[0].lod_min_clamp;
        m_state.last_draw_lod_max_clamp = call.samplers[0].lod_max_clamp;
        m_state.last_draw_lod_bias = call.samplers[0].lod_bias;
        m_state.last_draw_fragment_constants_size =
            call.fragment_constants_size;
        const auto copy_constants = [](const void* data, usize size) {
            const auto* bytes = static_cast<const u8*>(data);
            return data != nullptr ? std::vector<u8>(bytes, bytes + size)
                                   : std::vector<u8>{};
        };
        m_state.last_draw_vertex_constants =
            copy_constants(call.vertex_constants, call.vertex_constants_size);
        m_state.last_draw_fragment_constants = copy_constants(
            call.fragment_constants, call.fragment_constants_size);
        if (call.fragment_constants != nullptr &&
            call.fragment_constants_size >= sizeof(float)) {
            std::memcpy(&m_state.last_draw_shader_lod_bias,
                        call.fragment_constants, sizeof(float));
        }
        m_state.last_draw_depth_write_enabled = call.depth_write_enabled;
        m_state.last_draw_depth_test_enabled = call.depth_test_enabled;
        m_state.last_draw_depth_compare = call.depth_compare;
        for (unsigned slot = 0;
             slot < graphics::BACKEND_MAX_TEXTURE_SLOTS; ++slot) {
            m_state.last_draw_uav_texture_bound[slot] =
                call.fragment_uav_textures[slot] != nullptr ||
                call.fragment_uav_textures_3d[slot] != nullptr;
            m_state.last_draw_uav_buffer_bound[slot] =
                call.fragment_uav_buffers[slot] != nullptr;
        }
        m_state.last_draw_uav_texture_buffers =
            call.fragment_uav_texture_buffers;
        m_state.last_draw_texture_bound = call.textures[0] != nullptr;
        m_state.last_draw_vertex_texture_bound =
            call.vertex_textures[0] != nullptr;
        m_state.last_draw_stencil_enabled = call.stencil_test_enabled;
        m_state.last_draw_stencil_read_mask = call.stencil_read_mask;
        m_state.last_draw_stencil_write_mask = call.stencil_write_mask;
        m_state.last_draw_stencil_reference = call.stencil_reference;
        m_state.last_draw_front_stencil = call.front_stencil;
        m_state.last_draw_back_stencil = call.back_stencil;
        m_state.last_texture_views = call.texture_views;
        m_state.last_vertex_texture_views = call.vertex_texture_views;
        m_state.last_vertex_sampler = call.vertex_samplers[0];
        return true;
    }
    bool end() override {
        if (!m_open) return false;
        m_open = false;
        return true;
    }
private:
    BackendState& m_state;
    bool m_open{true};
};

class TestBackendSwapChain final : public graphics::BackendSwapChain {
public:
    TestBackendSwapChain(graphics::BackendSwapChainDesc desc,
                         BackendState& state)
        : m_desc(desc), m_state(state) {}
    bool valid() const noexcept override { return true; }
    const graphics::BackendSwapChainDesc& desc() const noexcept override {
        return m_desc;
    }
    bool clear_back_buffer(
        const std::array<float, 4>& color) override {
        ++m_state.back_buffer_clears;
        m_state.last_clear = color;
        return true;
    }
    bool present(unsigned sync_interval, unsigned flags) override {
        // The backend contract: SyncInterval 0..4, no DXGI flags (the
        // frontend consumes them).
        if (sync_interval > 4 || flags != 0) return false;
        ++m_state.presents;
        m_state.last_present_sync = sync_interval;
        return true;
    }
    bool present_would_block() const override {
        return m_state.present_would_block;
    }
    bool set_color_space(graphics::BackendColorSpace space) override {
        m_state.last_color_space = space;
        return true;
    }
    bool readback_texture_rgba(graphics::BackendTexture2D* source,
                               std::vector<u8>& rgba) override {
        if (source == nullptr || !source->valid() ||
            m_state.readback_rgba.empty()) {
            rgba.clear();
            return false;
        }
        ++m_state.texture_readbacks;
        rgba = m_state.readback_rgba;
        return true;
    }
    bool configure_composition_layer(
        const graphics::BackendCompositionLayerDesc& desc) override {
        m_state.composition_layers.push_back(desc);
        return true;
    }
    std::unique_ptr<graphics::BackendRenderPass> begin_render_pass(
        bool, bool, float,
        const std::array<graphics::BackendTexture2D*,
                         graphics::BACKEND_MAX_RENDER_TARGETS>&) override {
        return std::make_unique<TestBackendRenderPass>(m_state);
    }
    bool resize(unsigned width, unsigned height,
                graphics::PixelFormat format,
                unsigned buffer_count) override {
        if (width == 0 || height == 0 || buffer_count == 0 ||
            format == graphics::PixelFormat::Unknown) {
            return false;
        }
        m_desc.width = width;
        m_desc.height = height;
        m_desc.pixel_format = format;
        m_desc.buffer_count = buffer_count;
        ++m_state.resizes;
        return true;
    }
private:
    graphics::BackendSwapChainDesc m_desc;
    BackendState& m_state;
};

}  // namespace vitreo::winapi::d3d11_test
