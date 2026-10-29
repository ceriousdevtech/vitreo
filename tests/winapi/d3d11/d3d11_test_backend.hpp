// SPDX-License-Identifier: LGPL-2.1-only
// VITREO_LARGE_FILE_OK: a single backend class; its members are one contract and cannot be split without changing code
#pragma once
/// @file d3d11_test_backend.hpp
/// @brief The fake graphics backend the D3D11 frontend tests drive (TestGraphicsBackend).

#include "d3d11_test_backend_passes.hpp"

namespace vitreo::winapi::d3d11_test {

class TestGraphicsBackend final : public graphics::GraphicsBackendDevice {
public:
    bool available() const noexcept override { return true; }

    /// A timeline that mirrors the real backend's ordering property: `signal`
    /// RECORDS a value without applying it, because on Metal it is encoded into
    /// the command stream. A fake that applied it immediately would let a
    /// frontend bug through.
    class FakeFence final : public graphics::BackendFence {
    public:
        explicit FakeFence(u64 initial) : m_value(initial) {}
        bool valid() const noexcept override { return true; }
        u64 completed_value() const override { return m_value; }
        bool signal(u64 value) override { m_recorded = value; return true; }
        bool wait_gpu(u64) override { return true; }
        bool signal_cpu(u64 value) override {
            if (value < m_value) return false;
            m_value = value;
            return true;
        }
        bool wait_cpu(u64 value, u64) override { return m_value >= value; }
        /// What a `signal` recorded but has not applied.
        [[nodiscard]] u64 recorded() const { return m_recorded; }

    private:
        u64 m_value{0};
        u64 m_recorded{0};
    };

    /// A recording that mirrors the real backend's property: nothing executes
    /// until the list is submitted.
    class FakeCommandList final : public graphics::BackendCommandList {
    public:
        explicit FakeCommandList(unsigned* submissions)
            : m_submissions(submissions) {}
        bool valid() const noexcept override { return true; }
        bool begin_capture() override {
            if (m_capturing || m_closed) return false;
            m_capturing = true;
            return true;
        }
        bool end_capture() override {
            if (!m_capturing) return false;
            m_capturing = false;
            return true;
        }
        bool close() override {
            if (m_closed) return false;
            m_capturing = false;
            m_closed = true;
            return true;
        }
        bool is_closed() const noexcept override { return m_closed; }
        bool reset() override {
            m_closed = false;
            m_ops = 0;
            return true;
        }
        usize operation_count() const noexcept override { return m_ops; }
        void note_operation() { ++m_ops; }
        [[nodiscard]] bool capturing() const { return m_capturing; }
        void note_submitted() {
            if (m_submissions != nullptr) ++*m_submissions;
        }

    private:
        unsigned* m_submissions{nullptr};
        usize m_ops{0};
        bool m_closed{false};
        bool m_capturing{false};
    };

    bool supports_command_lists() const noexcept override { return true; }
    std::unique_ptr<graphics::BackendCommandList> create_command_list()
        override {
        auto list = std::make_unique<FakeCommandList>(&state.list_submissions);
        state.last_command_list = list.get();
        return list;
    }
    bool submit_command_list(graphics::BackendCommandList* list) override {
        if (list == nullptr || !list->is_closed()) return false;
        static_cast<FakeCommandList*>(list)->note_submitted();
        return true;
    }

    bool supports_fences() const noexcept override { return true; }
    std::unique_ptr<graphics::BackendFence> create_fence(
        u64 initial_value) override {
        return std::make_unique<FakeFence>(initial_value);
    }
    bool supports_texture_sample_count(unsigned count) const override {
        return count == 1 || count == 2 || count == 4 || count == 8;
    }
    // -- Tiled resources: the Apple GPU's placement sparse layout, as
    // measured on an M4 Pro (64 KiB pages: the D3D standard shape at 1x and
    // 4x; a mip is in the packed tail once it is no more than half a tile
    // in either dimension).
    bool supports_tiled_resources() const override {
        return state.tiled_resources;
    }
    bool tiled_texture_tile_shape(graphics::PixelFormat format,
                                  unsigned samples, unsigned& width,
                                  unsigned& height) const override {
        if (!state.tiled_resources || (samples != 1 && samples != 4)) {
            return false;
        }
        const auto info = graphics::format_table(format);
        if (info.is_compressed) {
            if (samples != 1) return false;
            const u32 block = graphics::compressed_block_bytes(format);
            width = block == 8 ? 512 : 256;
            height = block == 8 ? 256 : 256;
            return block == 8 || block == 16;
        }
        // Depth32Float_Stencil8 (D24S8 / D32S8X24 storage): 128x128 at 8
        // bytes, not a standard 64-bit shape.
        if (format == graphics::PixelFormat::D32Float_S8Uint) {
            width = samples == 1 ? 128 : 64;
            height = samples == 1 ? 128 : 64;
            return true;
        }
        u32 texels = 65536u / std::max<u32>(1u, info.bytes_per_pixel) / samples;
        u32 w = 1;
        while (w * w < texels) w <<= 1;
        width = w;
        height = texels / w;
        return info.bytes_per_pixel != 0;
    }
    std::unique_ptr<graphics::BackendTilePool> create_tile_pool() override {
        if (!state.tiled_resources) return nullptr;
        return std::make_unique<TestTilePool>(state);
    }
    bool tiled_texture_layout(const graphics::BackendTexture2D* texture,
                              graphics::BackendTiledLayout& layout) override {
        if (texture == nullptr || !texture->desc().tiled) return false;
        const auto& d = texture->desc();
        if (!tiled_texture_tile_shape(d.pixel_format, d.sample_count,
                                      layout.tile_width, layout.tile_height)) {
            return false;
        }
        layout.first_packed_level = d.mip_levels;
        for (unsigned mip = 0; mip < d.mip_levels; ++mip) {
            if (std::max(1u, d.width >> mip) * 2 <= layout.tile_width ||
                std::max(1u, d.height >> mip) * 2 <= layout.tile_height) {
                layout.first_packed_level = mip;
                break;
            }
        }
        layout.packed_tiles = layout.first_packed_level < d.mip_levels ? 2 : 0;
        return true;
    }
    bool record_tile_mappings(const void* resource,
                              const graphics::BackendTileMapping* mappings,
                              usize count, bool self_aliased) {
        ++state.tile_mapping_updates;
        state.last_self_aliased = self_aliased;
        auto& table = state.tile_mappings[resource];
        for (usize i = 0; i < count; ++i) {
            const auto& t = mappings[i].tile;
            const BackendState::TileKey key{t.level, t.slice, t.x, t.y,
                                            t.packed};
            if (mappings[i].pool_tile == graphics::BACKEND_TILE_UNMAPPED) {
                table.erase(key);
            } else {
                table[key] = mappings[i].pool_tile;
            }
        }
        return true;
    }
    bool update_tile_mappings(graphics::BackendTexture2D* texture,
                              graphics::BackendTilePool*,
                              const graphics::BackendTileMapping* mappings,
                              usize count, bool self_aliased) override {
        return record_tile_mappings(texture, mappings, count, self_aliased);
    }
    bool update_tile_mappings(graphics::BackendBuffer* buffer,
                              graphics::BackendTilePool*,
                              const graphics::BackendTileMapping* mappings,
                              usize count, bool self_aliased) override {
        return record_tile_mappings(buffer, mappings, count, self_aliased);
    }
    bool tiled_resource_barrier() override {
        ++state.tiled_barriers;
        return true;
    }
    bool copy_texture_buffer_region(graphics::BackendTexture2D* texture,
                                    unsigned level, unsigned slice,
                                    unsigned x, unsigned y, unsigned width,
                                    unsigned height,
                                    graphics::BackendBuffer* buffer,
                                    u64 offset, u64 row_pitch,
                                    bool to_texture) override {
        if (texture == nullptr || buffer == nullptr) return false;
        state.tile_copies.push_back({texture, level, slice, x, y, width,
                                     height, buffer, offset, row_pitch,
                                     to_texture, {}});
        if (to_texture && buffer->desc().byte_width <= 0x100000) {
            state.tile_copies.back().bytes =
                static_cast<TestBackendBuffer*>(buffer)->live();
        }
        return true;
    }

    bool supports_framebuffer_fetch() const override {
        return state.framebuffer_fetch;
    }
    bool supports_programmable_sample_positions() const override {
        return state.programmable_sample_positions;
    }
    bool supports_shader_stencil_export() const override {
        return state.shader_stencil_export;
    }
    bool has_unified_memory() const override { return state.unified_memory; }
    bool supports_debug_markers() const override {
        return state.debug_markers_supported;
    }
    void push_debug_group(const std::string& label) override {
        state.debug_markers.push_back("push:" + label);
    }
    void pop_debug_group() override { state.debug_markers.push_back("pop"); }
    void insert_debug_signpost(const std::string& label) override {
        state.debug_markers.push_back("sign:" + label);
    }
    std::string backend_device_name() const override {
        return "Shared Test Metal GPU";
    }
    void* native_device() const noexcept override {
        return reinterpret_cast<void*>(1);
    }
    void* native_command_queue() const noexcept override {
        return reinterpret_cast<void*>(2);
    }
    // R3c native states: plain desc holders.
    struct TestSamplerState final : graphics::BackendSamplerState {
        explicit TestSamplerState(const graphics::BackendSamplerDesc& d)
            : value(d) {}
        const graphics::BackendSamplerDesc& desc() const noexcept override {
            return value;
        }
        graphics::BackendSamplerDesc value;
    };
    struct TestDepthStencilState final : graphics::BackendDepthStencilState {
        explicit TestDepthStencilState(const graphics::BackendDepthStencilDesc& d)
            : value(d) {}
        const graphics::BackendDepthStencilDesc& desc() const noexcept override {
            return value;
        }
        graphics::BackendDepthStencilDesc value;
    };
    std::unique_ptr<graphics::BackendSamplerState> create_sampler_state(
        const graphics::BackendSamplerDesc& desc) override {
        ++state.sampler_states_created;
        return std::make_unique<TestSamplerState>(desc);
    }
    std::unique_ptr<graphics::BackendDepthStencilState>
    create_depth_stencil_state(
        const graphics::BackendDepthStencilDesc& desc) override {
        ++state.depth_stencil_states_created;
        return std::make_unique<TestDepthStencilState>(desc);
    }
    std::unique_ptr<graphics::BackendBuffer> create_buffer(
        const graphics::BackendBufferDesc& desc,
        const void* initial_data) override {
        if (state.on_create_buffer) state.on_create_buffer();
        ++state.buffer_creations;
        state.last_buffer_desc = desc;
        state.last_buffer_initial_data.clear();
        if (initial_data != nullptr) {
            const auto* bytes = static_cast<const u8*>(initial_data);
            state.last_buffer_initial_data.assign(
                bytes, bytes + desc.byte_width);
        }
        auto buffer = std::make_unique<TestBackendBuffer>(
            desc, state, state.last_buffer_initial_data);
        state.created_buffers.push_back(buffer.get());
        if (desc.tiled) state.last_tiled_object = buffer.get();
        return buffer;
    }
    std::unique_ptr<graphics::BackendTexture2D> create_texture_2d(
        const graphics::BackendTexture2DDesc& desc,
        const void*, unsigned) override {
        ++state.texture_creations;
        state.last_texture_desc = desc;
        auto texture = std::make_unique<TestBackendTexture>(desc, state);
        if (desc.tiled) state.last_tiled_object = texture.get();
        return texture;
    }
    std::unique_ptr<graphics::BackendTexture3D> create_texture_3d(
        const graphics::BackendTexture3DDesc& desc, const void*, unsigned,
        unsigned) override {
        state.last_texture_3d_desc = desc;
        ++state.texture_3d_creations;
        return std::make_unique<TestBackendTexture3D>(desc, &state);
    }
    std::unique_ptr<graphics::BackendTextureBuffer> create_texture_buffer(
        const graphics::BackendBuffer&, graphics::PixelFormat pixel_format,
        unsigned first_element, unsigned num_elements,
        bool shader_write = false) override {
        state.texture_buffer_creations.push_back(
            {pixel_format, first_element, num_elements, shader_write});
        return std::make_unique<TestBackendTextureBuffer>();
    }
    bool supports_shared_texture_2d(
        graphics::PixelFormat format) const noexcept override {
        return TestBackendTexture::test_shared_storage_format(format);
    }
    bool supports_shared_texture(
        graphics::PixelFormat format) const noexcept override {
        return state.extended_sharing
            ? graphics::format_table(format).mtl_pixel_format != 0
            : TestBackendTexture::test_shared_storage_format(format);
    }
    bool supports_extended_sharing() const noexcept override {
        return state.extended_sharing;
    }
    std::shared_ptr<graphics::BackendSharedMemory> create_shared_memory(
        usize bytes, std::vector<u8> metadata) override {
        if (!state.extended_sharing) return nullptr;
        const u64 id = ++state.next_shared_id;
        auto memory = std::make_shared<TestSharedMemory>(
            id, bytes, std::move(metadata), state);
        state.shared_memories[id] = memory;
        return memory;
    }
    SharedOpening open_shared_resource(u64 handle) override {
        SharedOpening opening;
        if (const auto memory = state.shared_memories.find(handle);
            memory != state.shared_memories.end()) {
            opening.kind = SharedOpening::Kind::Memory;
            opening.memory = memory->second;
            opening.metadata = memory->second->shared_metadata();
            return opening;
        }
        if (const auto buffer = state.shared_buffers.find(handle);
            buffer != state.shared_buffers.end()) {
            opening.kind = SharedOpening::Kind::Buffer;
            opening.buffer = std::make_unique<TestBackendBuffer>(
                buffer->second, state, std::vector<u8>{}, handle);
            opening.metadata = buffer->second.shared_metadata;
            return opening;
        }
        return GraphicsBackendDevice::open_shared_resource(handle);
    }
    std::shared_ptr<graphics::GpuCompletion> publish_shared_writes() override {
        ++state.shared_publications;
        return nullptr;
    }
    void acquire_shared_writes() override { ++state.shared_acquires; }
    std::shared_ptr<void> retain_shared_texture(u64 handle) override {
        return state.shared_storage.contains(handle)
            ? std::make_shared<u64>(handle)
            : nullptr;
    }
    std::unique_ptr<graphics::BackendTexture2D> open_shared_texture_2d(
        u64 handle) override {
        if (handle == 0) return nullptr;
        ++state.shared_texture_opens;
        if (const auto stored = state.shared_storage.find(handle);
            stored != state.shared_storage.end()) {
            return std::make_unique<TestBackendTexture>(stored->second, state,
                                                        handle);
        }
        graphics::BackendTexture2DDesc desc{};
        desc.width = 348;
        desc.height = 646;
        desc.pixel_format = graphics::PixelFormat::BGRA8Unorm;
        desc.render_target = true;
        desc.shareable = true;
        return std::make_unique<TestBackendTexture>(desc, state);
    }
    std::unique_ptr<graphics::BackendPipeline> create_pipeline(
        const graphics::BackendPipelineDesc& desc) override {
        ++state.pipeline_creations;
        state.last_pipeline_fragment_empty = desc.fragment_shader_msl.empty();
        state.last_pipeline_vertex_msl = desc.vertex_shader_msl;
        state.last_pipeline_fragment_msl = desc.fragment_shader_msl;
        state.last_pipeline_attributes = desc.vertex_attributes;
        state.last_pipeline_strides = desc.vertex_buffer_strides;
        state.last_pipeline_attachmentless = desc.attachmentless;
        if (desc.tessellation) {
            ++state.tessellation_pipelines;
            state.last_tessellation_partition = desc.tessellation_partition;
        }
        if (!desc.rasterization_enabled) ++state.rasterization_disabled_pipelines;
        return std::make_unique<TestBackendPipeline>(desc);
    }
    std::unique_ptr<graphics::BackendComputePipeline> create_compute_pipeline(
        const graphics::BackendComputePipelineDesc& desc) override {
        ++state.compute_pipeline_creations;
        state.last_compute_pipeline_has_msl = !desc.compute_shader_msl.empty();
        state.last_compute_msl = desc.compute_shader_msl;
        state.last_compute_entry_is_main0 = desc.compute_entry_point == "main0";
        return std::make_unique<TestBackendComputePipeline>(
            desc.compute_entry_point);
    }
    std::unique_ptr<graphics::BackendRenderPass> create_render_pass_standalone(
        const graphics::BackendRenderPassDesc& desc) override {
        state.last_pass_color_targets = static_cast<unsigned>(std::count_if(
            desc.color_targets.begin(), desc.color_targets.end(),
            [](const auto* target) { return target != nullptr; }));
        state.last_pass_has_depth = desc.depth_stencil_target != nullptr;
        state.last_pass_color0 = desc.color_targets[0];
        state.last_pass_attachmentless_width = desc.attachmentless_width;
        state.last_pass_attachmentless_height = desc.attachmentless_height;
        state.last_pass_attachmentless_samples = desc.attachmentless_samples;
        state.last_pass_center_sample_positions = desc.center_sample_positions;
        state.last_pass_depth_level = desc.depth_level;
        state.last_pass_depth_slice = desc.depth_slice;
        state.last_pass_depth_array_size = desc.depth_array_size;
        state.last_pass_volume_target0 = desc.color_targets_3d[0] != nullptr;
        state.last_pass_color0_desc = desc.color_targets[0] != nullptr
            ? desc.color_targets[0]->desc()
            : graphics::BackendTexture2DDesc{};
        state.last_pass_depth_plane0 = desc.color_depth_planes[0];
        state.last_pass_color_array_size0 = desc.color_array_sizes[0];
        state.pass_occlusion_counters.push_back(desc.occlusion_counter);
        if (desc.occlusion_counter != nullptr) {
            if (state.before_counted_pass) state.before_counted_pass();
            // The pass joins the recording open now.
            auto* counter =
                static_cast<TestOcclusionCounter*>(desc.occlusion_counter);
            if (state.generations) {
                counter->generation =
                    std::max(counter->generation, state.recording_gen);
            }
        }
        return std::make_unique<TestBackendRenderPass>(state);
    }
    bool dispatch(const graphics::BackendDispatchCall& call) override {
        if (call.pipeline == nullptr || !call.pipeline->valid()) return false;
        ++state.dispatches;
        state.dispatched_entries.push_back(
            static_cast<const TestBackendComputePipeline*>(call.pipeline)
                ->entry);
        if (call.indirect.buffer != nullptr) ++state.indirect_dispatches;
        state.last_dispatch_groups = {
            call.group_count_x, call.group_count_y, call.group_count_z};
        state.last_dispatch_threads = {
            call.threads_per_group_x, call.threads_per_group_y,
            call.threads_per_group_z};
        state.last_dispatch_has_uav = call.textures[8] != nullptr;
        state.last_dispatch_has_uav_3d = call.textures_3d[8] != nullptr;
        state.last_dispatch_uav_3d_format =
            call.textures_3d[8] != nullptr
                ? call.textures_3d[8]->desc().pixel_format
                : graphics::PixelFormat::Unknown;
        state.last_dispatch_uav_3d_mips =
            call.textures_3d[8] != nullptr
                ? call.textures_3d[8]->desc().mip_levels
                : 0u;
        state.last_dispatch_uav_mip = call.texture_views[8].most_detailed_mip;
        state.last_dispatch_has_srv = call.textures[0] != nullptr;
        state.last_dispatch_has_sampler = call.sampler_bound[0];
        state.last_dispatch_has_constant_buffer = call.buffers[2] != nullptr;
        state.last_dispatch_buffers = call.buffers;
        for (usize i = 0; i < call.buffers.size(); ++i) {
            state.last_dispatch_buffer_bytes[i] =
                call.buffers[i] != nullptr
                    ? static_cast<const TestBackendBuffer*>(call.buffers[i])
                          ->live()
                    : std::vector<u8>{};
        }
        state.last_dispatch_buffer_offsets = call.buffer_offsets;
        state.last_dispatch_texture_buffers = call.texture_buffers;
        state.last_dispatch_constants.assign(call.constants_size / 4u, 0u);
        if (call.constants != nullptr && call.constants_size >= 4u) {
            std::memcpy(state.last_dispatch_constants.data(), call.constants,
                        state.last_dispatch_constants.size() * 4u);
        }
        // Every buffer in this backend is a TestBackendBuffer (no RTTI here).
        // A repack is the only dispatch that binds both of these indices.
        if (const auto* control = static_cast<const TestBackendBuffer*>(
                call.buffers[graphics::SO_REPACK_CONTROL_INDEX]);
            control != nullptr && call.buffers[graphics::SO_REPACK_CAPTURE_INDEX] &&
            call.pipeline != nullptr &&
            call.buffers[graphics::SO_REPACK_TARGET_BASE] != nullptr) {
            std::vector<u32> words(control->contents().size() / 4);
            std::memcpy(words.data(), control->contents().data(),
                        words.size() * 4);
            state.repack_controls.push_back(std::move(words));
        }
        if (state.live_buffers) run_accounting_model(call);
        return true;
    }
    /// The stream-output accounting kernels, by their C++ models.
    void run_accounting_model(const graphics::BackendDispatchCall& call) {
        const auto& entry =
            static_cast<const TestBackendComputePipeline*>(call.pipeline)->entry;
        const auto buffer = [&](unsigned index) {
            return static_cast<const TestBackendBuffer*>(call.buffers[index]);
        };
        const auto words = [&](unsigned index) {
            return buffer(index)->live_words(call.buffer_offsets[index]);
        };
        if (entry == graphics::kStreamOutPrepareEntryPoint) {
            graphics::SoPrepareParams params{};
            std::memcpy(&params, call.constants, sizeof(params));
            std::array<u32*, graphics::BACKEND_MAX_STREAM_OUT_TARGETS> filled{};
            for (u32 s = 0; s < filled.size(); ++s) {
                if ((params.slot_mask & (1u << s)) != 0) {
                    filled[s] = words(graphics::SO_PREPARE_FILLED_BASE + s);
                }
            }
            std::array<u32, graphics::SO_OP_WORDS> op{};
            std::array<u32, graphics::SO_STATS_WORDS> stats{};
            u32* stats_words = words(graphics::SO_PREPARE_STATS_INDEX);
            std::memcpy(stats.data(), stats_words, sizeof(stats));
            graphics::stream_out_prepare_model(
                params, *words(graphics::SO_PREPARE_COUNT_INDEX), filled, op,
                stats);
            std::memcpy(words(graphics::SO_PREPARE_OP_INDEX), op.data(),
                        sizeof(op));
            std::memcpy(stats_words, stats.data(), sizeof(stats));
            state.prepare_params.push_back(params);
            state.prepare_ops.push_back(op);
        } else if (entry == graphics::kStreamOutDrawAutoEntryPoint) {
            std::array<u32, 2> constants{};
            std::memcpy(constants.data(), call.constants, sizeof(constants));
            const u32 count = graphics::stream_out_draw_auto_count(
                *words(graphics::SO_DRAWAUTO_FILLED_INDEX), constants[0],
                constants[1]);
            u32* args = words(graphics::SO_DRAWAUTO_ARGS_INDEX);
            args[0] = count;
            args[1] = 1;
            args[2] = args[3] = 0;
            state.drawauto_counts.push_back(count);
        }
    }
    bool copy_buffer(graphics::BackendBuffer* dst,
                     const graphics::BackendBuffer* src) override {
        if (dst == nullptr || src == nullptr || !dst->valid() ||
            !src->valid()) {
            return false;
        }
        if (dst->desc().byte_width != src->desc().byte_width) return false;
        ++state.buffer_copies;
        return true;
    }
    bool copy_buffer_texture_row(graphics::BackendBuffer* buffer, u64 offset,
                                 graphics::BackendTexture2D* texture,
                                 unsigned width, bool to_texture) override {
        if (buffer == nullptr || texture == nullptr || width == 0) {
            return false;
        }
        state.buffer_texture_row_copies.push_back(
            {offset, width, to_texture, texture->desc().pixel_format,
             state.draws});
        return true;
    }
    bool copy_buffer_region(graphics::BackendBuffer* dst, u64 dst_offset,
                            const graphics::BackendBuffer* src, u64 src_offset,
                            u64 bytes) override {
        if (dst == nullptr || src == nullptr || bytes == 0) return false;
        if (dst_offset > dst->desc().byte_width ||
            bytes > dst->desc().byte_width - dst_offset) {
            return false;
        }
        if (src_offset > src->desc().byte_width ||
            bytes > src->desc().byte_width - src_offset) {
            return false;
        }
        // Every buffer this backend hands out is a TestBackendBuffer.
        const auto* test_src = static_cast<const TestBackendBuffer*>(src);
        state.buffer_region_copies.push_back(
            {dst_offset, src_offset, bytes, src->desc().byte_width,
             test_src->contents()});
        std::memcpy(static_cast<TestBackendBuffer*>(dst)->live().data() +
                        dst_offset,
                    test_src->live().data() + src_offset, bytes);
        return true;
    }
    // The contract's refusals (backend_device.hpp): out of range, empty, or
    // not four-byte granular.
    bool fill_buffer_region(graphics::BackendBuffer* dst, u64 offset,
                            u64 bytes, u8 value) override {
        if (dst == nullptr || bytes == 0) return false;
        if (offset > dst->desc().byte_width ||
            bytes > dst->desc().byte_width - offset) {
            return false;
        }
        if ((offset % 4) != 0 || (bytes % 4) != 0) return false;
        state.buffer_fills.push_back({offset, bytes, value});
        std::memset(static_cast<TestBackendBuffer*>(dst)->live().data() +
                        offset,
                    value, bytes);
        return true;
    }
    bool copy_texture_2d(graphics::BackendTexture2D* dst,
                         const graphics::BackendTexture2D* src) override {
        if (dst == nullptr || src == nullptr || !dst->valid() ||
            !src->valid()) {
            return false;
        }
        ++state.texture_copies;
        return true;
    }
    bool copy_texture_3d(graphics::BackendTexture3D* dst,
                         const graphics::BackendTexture3D* src) override {
        if (dst == nullptr || src == nullptr || !dst->valid() ||
            !src->valid() || dst == src) {
            return false;
        }
        ++state.texture_3d_copies;
        return true;
    }
    bool resolve_texture_2d(
        graphics::BackendTexture2D* dst, unsigned dst_mip,
        unsigned dst_slice, const graphics::BackendTexture2D* src,
        unsigned src_mip, unsigned src_slice,
        graphics::PixelFormat resolve_format) override {
        if (dst == nullptr || src == nullptr || !dst->valid() ||
            !src->valid()) {
            return false;
        }
        ++state.texture_resolves;
        state.last_copy_dst_mip = dst_mip;
        state.last_copy_dst_slice = dst_slice;
        state.last_copy_src_mip = src_mip;
        state.last_copy_src_slice = src_slice;
        state.last_resolve_format = resolve_format;
        return true;
    }
    bool copy_texture_2d_region(
        graphics::BackendTexture2D* dst, unsigned dst_x, unsigned dst_y,
        const graphics::BackendTexture2D* src, unsigned src_x, unsigned src_y,
        unsigned width, unsigned height, unsigned dst_mip,
        unsigned dst_slice, unsigned src_mip,
        unsigned src_slice) override {
        if (dst == nullptr || src == nullptr || !dst->valid() ||
            !src->valid()) {
            return false;
        }
        ++state.texture_region_copies;
        state.copy_regions.push_back(
            {dst_x, dst_y, src_x, src_y, width, height});
        state.last_copy_dst_mip = dst_mip;
        state.last_copy_dst_slice = dst_slice;
        state.last_copy_src_mip = src_mip;
        state.last_copy_src_slice = src_slice;
        state.last_copy_width = width;
        state.last_copy_height = height;
        state.copied_subresources.push_back(
            {dst_mip, dst_slice, src_mip, src_slice});
        return true;
    }
    bool generate_mips(graphics::BackendTexture2D* texture) override {
        if (texture == nullptr || !texture->valid() ||
            texture->desc().mip_levels <= 1) {
            return false;
        }
        ++state.mip_generations;
        return true;
    }
    bool generate_mips_window(graphics::BackendTexture2D* texture,
                              unsigned first_mip, unsigned mip_count,
                              unsigned first_slice,
                              unsigned slice_count) override {
        if (texture == nullptr) return false;
        state.mip_windows.push_back(
            {first_mip, mip_count, first_slice, slice_count});
        return true;
    }
    bool copy_texture_elements(const graphics::BackendTextureLocation& dst,
                               const graphics::BackendTextureLocation& src,
                               unsigned width, unsigned height,
                               unsigned depth) override {
        if ((dst.texture_2d == nullptr && dst.texture_3d == nullptr) ||
            (src.texture_2d == nullptr && src.texture_3d == nullptr)) {
            return false;
        }
        state.element_copies.push_back(
            {dst.mip, dst.slice, dst.x, dst.y, dst.z, src.mip, src.slice,
             src.x, src.y, src.z, width, height, depth});
        return true;
    }
    std::unique_ptr<graphics::BackendSwapChain> create_swap_chain(
        const graphics::BackendSwapChainDesc& desc) override {
        ++state.swap_chain_creations;
        state.last_swap_chain_view = desc.native_view;
        return std::make_unique<TestBackendSwapChain>(desc, state);
    }
    [[nodiscard]] bool wants_early_submit() const override {
        return state.gpu_wants_work;
    }
    void flush() override {
        ++state.flushes;
        // A flush submits the open recording, so the next one gets a new
        // generation (MetalSubmitQueue::submit).
        if (state.generations) ++state.recording_gen;
    }
    std::shared_ptr<graphics::GpuTimestamp> record_timestamp() override {
        auto point = std::make_shared<TestGpuPoint>();
        state.gpu_points.push_back(point);
        return point;
    }
    std::shared_ptr<graphics::GpuOcclusionCounter> create_occlusion_counter()
        override {
        auto counter = std::make_shared<TestOcclusionCounter>();
        state.occlusion_counters.push_back(counter);
        return counter;
    }
    u64 recording_generation() override {
        return state.generations ? state.recording_gen : 0;
    }
    /// Every op here joins the recording open when it is made, so the one
    /// just recorded is in recording_gen -- including after a test's
    /// before_draw "other thread" moved it on.
    u64 last_recorded_generation() override {
        return state.generations ? state.recording_gen : 0;
    }
    bool generation_complete(u64 generation) override {
        return state.generations && generation != 0 &&
               state.completed_gen >= generation;
    }
    bool wait_generation(u64 generation) override {
        if (!state.generations) return wait_idle();
        state.generation_waits.push_back(generation);
        if (generation >= state.recording_gen) ++state.recording_gen;
        state.completed_gen = std::max(state.completed_gen, generation);
        return true;
    }
    std::shared_ptr<graphics::GpuCompletion> flush_completion() override {
        ++state.flushes;
        if (state.generations) ++state.recording_gen;
        struct Wait final : graphics::GpuCompletion {
            BackendState* state{nullptr};
            bool wait() override {
                ++state->wait_idles;
                if (state->wait_idle_delay.count() > 0) {
                    std::this_thread::sleep_for(state->wait_idle_delay);
                }
                return state->wait_idle_ok;
            }
        };
        auto wait = std::make_shared<Wait>();
        wait->state = &state;
        return wait;
    }
    bool wait_idle() override {
        ++state.wait_idles;
        if (state.wait_idle_delay.count() > 0) {
            std::this_thread::sleep_for(state.wait_idle_delay);
        }
        return state.wait_idle_ok;
    }

    BackendState state;
};

}  // namespace vitreo::winapi::d3d11_test
