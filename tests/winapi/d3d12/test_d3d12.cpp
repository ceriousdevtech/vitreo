// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d12.cpp
/// @brief d3d12.dll — the object model, memory, descriptors and submission,
/// driven through the guest-visible vtables on x64 and x86.
///
/// Expected values come from Learn or from Windows measurements
/// (tools/oracle_probe/d12core_facts_probe.expected), never from running this
/// implementation.

#include "winapi/graphics/d3d12/d3d12.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/d3d12/d3d12_root_signature.hpp"
#include "winapi/graphics/dcomp/dcomp.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/gs_passthrough.hpp"
#include "graphics/shaders/pipeline_stats.hpp"

#include "../../graphics/tessellation_shaders.h"
#include "../../oracle_data/tess_ds_cull_shader.h"
#include "../../graphics/uav_counter_shaders.h"
#include "../../oracle_data/d3d12_sm51/d3d12_sm51_shaders.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <initializer_list>
#include <future>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {
namespace {

using namespace d3d12;

// =============================================================================
// A backend with host memory for heaps and recorders for everything else
// =============================================================================

class FakeBackend final : public graphics::GraphicsBackendDevice {
public:
    class FakeBuffer final : public graphics::BackendBuffer {
    public:
        // Page-aligned, as a CPU-visible backend heap's host memory is: the
        // frontend maps it into the guest.
        explicit FakeBuffer(usize size)
            : m_storage(static_cast<u8*>(std::aligned_alloc(
                            0x10000, (size + 0xFFFFu) & ~usize{0xFFFFu})),
                        &std::free),
              m_size(size) {
            std::memset(m_storage.get(), 0, (size + 0xFFFFu) & ~usize{0xFFFFu});
            m_desc.byte_width = size;
        }
        void mark_tiled(bool tiled) { m_desc.tiled = tiled; }
        [[nodiscard]] bool valid() const noexcept override { return true; }
        [[nodiscard]] const graphics::BackendBufferDesc& desc()
            const noexcept override {
            return m_desc;
        }
        [[nodiscard]] bool update(usize offset, const void* data, usize length,
                                  graphics::BufferWrite) override {
            if (offset + length > m_size) return false;
            std::memcpy(m_storage.get() + offset, data, length);
            return true;
        }
        [[nodiscard]] u8* bytes() { return m_storage.get(); }

    private:
        graphics::BackendBufferDesc m_desc{};
        std::unique_ptr<u8, decltype(&std::free)> m_storage;
        usize m_size{0};
    };

    class FakeHeap final : public graphics::BackendHeap {
    public:
        FakeHeap(u64 size, bool cpu) : m_buffer(static_cast<usize>(size)) {
            m_size = size;
            m_cpu = cpu;
        }
        [[nodiscard]] bool valid() const noexcept override { return true; }
        [[nodiscard]] u64 size() const noexcept override { return m_size; }
        [[nodiscard]] void* host_pointer() const noexcept override {
            return m_cpu ? const_cast<FakeBuffer&>(m_buffer).bytes() : nullptr;
        }
        [[nodiscard]] graphics::BackendBuffer* buffer() noexcept override {
            return &m_buffer;
        }

    private:
        FakeBuffer m_buffer;
        u64 m_size{0};
        bool m_cpu{false};
    };

    struct TextureUpload {
        graphics::BackendTexture2D* texture{nullptr};
        std::vector<u8> bytes;
        unsigned pitch{0};
    };
    std::vector<TextureUpload> texture_uploads;

    class FakeTexture final : public graphics::BackendTexture2D {
    public:
        FakeTexture(FakeBackend& owner, const graphics::BackendTexture2DDesc& desc)
            : m_owner(owner), m_desc(desc) {}
        [[nodiscard]] bool valid() const noexcept override { return true; }
        [[nodiscard]] const graphics::BackendTexture2DDesc& desc()
            const noexcept override {
            return m_desc;
        }
        /// A tiled texture asked for one keeps a residency companion (the
        /// tier-2 contract the frontend checks at CreateReservedResource).
        [[nodiscard]] bool has_residency_companion() const noexcept override {
            return m_desc.tiled && m_desc.residency_companion;
        }
        [[nodiscard]] bool clear_render_target(
            const std::array<float, 4>& color) override {
            m_owner.rt_clears.push_back({this, color});
            this->color = color;
            has_color = true;
            return true;
        }
        [[nodiscard]] bool clear_depth_stencil(float depth, u8 stencil) override {
            m_owner.ds_clears.push_back({this, depth, stencil, true, true});
            this->depth = depth;
            this->stencil = stencil;
            has_depth = true;
            has_stencil = true;
            return true;
        }
        [[nodiscard]] bool clear_depth_stencil_aspects(
            float depth, u8 stencil, bool clear_depth, bool clear_stencil,
            unsigned, unsigned, unsigned) override {
            m_owner.ds_clears.push_back(
                {this, depth, stencil, clear_depth, clear_stencil});
            if (clear_depth) {
                this->depth = depth;
                has_depth = true;
            }
            if (clear_stencil) {
                this->stencil = stencil;
                has_stencil = true;
            }
            return true;
        }
        [[nodiscard]] bool update(unsigned level, const void* data,
                                  unsigned row_pitch) override {
            if (level == 0 && data != nullptr && row_pitch != 0 &&
                m_desc.height != 0) {
                const usize bytes =
                    static_cast<usize>(row_pitch) * m_desc.height;
                TextureUpload upload;
                upload.texture = this;
                upload.pitch = row_pitch;
                upload.bytes.assign(static_cast<const u8*>(data),
                                    static_cast<const u8*>(data) + bytes);
                m_owner.texture_uploads.push_back(std::move(upload));
            }
            return true;
        }
        /// Last clear, so a suspend/resume local copy can be checked.
        std::array<float, 4> color{};
        bool has_color{false};
        float depth{0};
        u8 stencil{0};
        bool has_depth{false};
        bool has_stencil{false};

    private:
        FakeBackend& m_owner;
        graphics::BackendTexture2DDesc m_desc;
    };

    class FakeFence final : public graphics::BackendFence {
    public:
        FakeFence(FakeBackend& owner, u64 initial)
            : m_owner(owner), m_value(initial) {}
        [[nodiscard]] bool valid() const noexcept override { return true; }
        [[nodiscard]] u64 completed_value() const override { return m_value; }
        [[nodiscard]] bool signal(u64 value) override {
            m_owner.stream.push_back("signal " + std::to_string(value));
            // The fake "GPU" runs in stream order at once.
            m_value = value;
            return true;
        }
        [[nodiscard]] bool wait_gpu(u64 value) override {
            m_owner.stream.push_back("wait " + std::to_string(value));
            return true;
        }
        [[nodiscard]] bool signal_cpu(u64 value) override {
            m_value = value;
            return true;
        }
        [[nodiscard]] bool wait_cpu(u64 value, u64) override {
            return m_value >= value;
        }

    private:
        FakeBackend& m_owner;
        u64 m_value{0};
    };

    explicit FakeBackend(bool tiled = false) : m_tiled(tiled) {}

    struct TileCall {
        bool texture{false};
        bool has_pool{false};
        bool self_aliased{false};
        std::vector<graphics::BackendTileMapping> mappings;
    };
    std::vector<TileCall> tile_calls;
    /// CopyTiles of a 2D tile: the linear box handed to the backend.
    struct TextureTileCopy {
        graphics::BackendTexture2D* texture{nullptr};
        unsigned level{0};
        unsigned slice{0};
        unsigned x{0};
        unsigned y{0};
        unsigned width{0};
        unsigned height{0};
        u64 offset{0};
        u64 row_pitch{0};
        bool to_texture{false};
    };
    std::vector<TextureTileCopy> texture_tile_copies;

    [[nodiscard]] bool available() const noexcept override { return true; }
    [[nodiscard]] std::string backend_device_name() const override {
        return "Fake";
    }
    [[nodiscard]] void* native_device() const noexcept override {
        return nullptr;
    }
    [[nodiscard]] void* native_command_queue() const noexcept override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendBuffer> create_buffer(
        const graphics::BackendBufferDesc& desc, const void*) override {
        // A 64 GiB reserved buffer is E_OUTOFMEMORY on Windows (a02). The
        // fake stands in for a backend that cannot address one.
        if (desc.tiled && desc.byte_width >= (1ull << 36)) return nullptr;
        auto buffer = std::make_unique<FakeBuffer>(
            desc.byte_width == 0 ? 1 : desc.byte_width);
        buffer->mark_tiled(desc.tiled);
        return buffer;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendTexture2D>
    create_texture_2d(const graphics::BackendTexture2DDesc& desc, const void*,
                      unsigned) override {
        auto texture = std::make_unique<FakeTexture>(*this, desc);
        textures.push_back(texture.get());
        return texture;
    }
    /// A presenter that records what it shows into the stream.
    class FakeSwapChain final : public graphics::BackendSwapChain {
    public:
        FakeSwapChain(FakeBackend& owner, const graphics::BackendSwapChainDesc& d)
            : m_owner(owner), m_desc(d) {}
        [[nodiscard]] bool valid() const noexcept override { return true; }
        [[nodiscard]] const graphics::BackendSwapChainDesc& desc()
            const noexcept override {
            return m_desc;
        }
        [[nodiscard]] bool clear_back_buffer(
            const std::array<float, 4>&) override {
            return true;
        }
        [[nodiscard]] bool present(unsigned, unsigned) override { return true; }
        [[nodiscard]] bool present_texture(graphics::BackendTexture2D* source,
                                           unsigned, unsigned flags) override {
            // The backend contract (MetalSwapChain::present_texture): the
            // frontend consumes the DXGI_PRESENT flags; a present with any
            // is refused.
            if (flags != 0) {
                m_owner.stream.push_back("present refused: flags");
                return false;
            }
            m_owner.stream.push_back(
                "present " + std::to_string(source->desc().width) + "x" +
                std::to_string(source->desc().height));
            return true;
        }
        [[nodiscard]] bool configure_composition_layer(
            const graphics::BackendCompositionLayerDesc& desc) override {
            m_owner.composition_layers.push_back(desc);
            return true;
        }
        [[nodiscard]] bool resize(unsigned width, unsigned height,
                                  graphics::PixelFormat, unsigned) override {
            m_desc.width = width;
            m_desc.height = height;
            return true;
        }
        [[nodiscard]] std::unique_ptr<graphics::BackendRenderPass>
        begin_render_pass(bool, bool, float,
                          const std::array<graphics::BackendTexture2D*,
                                           graphics::BACKEND_MAX_RENDER_TARGETS>&)
            override {
            return nullptr;
        }

    private:
        FakeBackend& m_owner;
        graphics::BackendSwapChainDesc m_desc;
    };
    [[nodiscard]] std::unique_ptr<graphics::BackendSwapChain> create_swap_chain(
        const graphics::BackendSwapChainDesc& desc) override {
        ++swap_chain_creations;
        last_swap_chain = desc;
        return std::make_unique<FakeSwapChain>(*this, desc);
    }
    struct DrawNote {
        u32 vertex_count{0};
        u32 index_count{0};
        u32 first_vertex{0};
        u32 first_instance{0};
        /// The vertex push block's SV_InstanceID bias word (kInstanceIdBiasOffset).
        u32 instance_bias{0};
        bool capture{false};
        bool indexed{false};
        bool indirect{false};
        bool srv0{false};
        bool srv1{false};
        const graphics::BackendBuffer* uav_counter{nullptr};
        unsigned uav_counter_offset{0};
        graphics::BackendPrimitiveTopology topology{
            graphics::BackendPrimitiveTopology::TriangleList};
    };
    struct DispatchNote {
        u32 groups{0};
        bool in0{false};
        bool out1{false};
        bool counter2{false};
        bool ids3{false};
        bool cbv_table{false};
        bool srv_table{false};
        bool sampler_table{false};
        const graphics::BackendBuffer* uav_counter{nullptr};
        unsigned uav_counter_offset{0};
        /// The kernel's entry point, and StatsKernelParams::flags when the
        /// constants are a StatsKernelParams.
        std::string entry;
        u32 stats_flags{0};
    };
    std::vector<DrawNote> draws;
    std::vector<DispatchNote> dispatches;
    struct RtClear {
        graphics::BackendTexture2D* texture{nullptr};
        std::array<float, 4> color{};
    };
    struct DsClear {
        graphics::BackendTexture2D* texture{nullptr};
        float depth{0};
        u8 stencil{0};
        bool depth_on{false};
        bool stencil_on{false};
    };
    /// Textures CreateCommittedResource handed the frontend, in order.
    std::vector<graphics::BackendTexture2D*> textures;
    std::vector<RtClear> rt_clears;
    std::vector<DsClear> ds_clears;
    std::vector<graphics::BackendRenderPassDesc> passes;

    class FakePipeline final : public graphics::BackendPipeline {
    public:
        [[nodiscard]] bool valid() const noexcept override { return true; }
    };
    class FakeCompute final : public graphics::BackendComputePipeline {
    public:
        explicit FakeCompute(std::string entry) : entry(std::move(entry)) {}
        [[nodiscard]] bool valid() const noexcept override { return true; }
        std::string entry;
    };
    class FakePass final : public graphics::BackendRenderPass {
    public:
        explicit FakePass(FakeBackend& owner) : m_owner(owner) {}
        [[nodiscard]] bool valid() const noexcept override { return true; }
        [[nodiscard]] bool draw(const graphics::BackendDrawCall& call) override {
            DrawNote note{};
            note.vertex_count = call.vertex_count;
            note.index_count = call.index_count;
            note.first_vertex = call.first_vertex;
            note.first_instance = call.first_instance;
            if (call.vertex_constants != nullptr &&
                call.vertex_constants_size >=
                    graphics::kInstanceIdBiasOffset + 4u) {
                std::memcpy(&note.instance_bias,
                            static_cast<const u8*>(call.vertex_constants) +
                                graphics::kInstanceIdBiasOffset,
                            4);
            }
            note.capture = call.vertex_capture_buffer != nullptr;
            note.indexed = call.index_buffer != nullptr;
            note.indirect = call.indirect.active();
            note.srv0 = call.vertex_srv_buffers[0] != nullptr;
            note.srv1 = call.vertex_srv_buffers[1] != nullptr;
            note.uav_counter = call.fragment_uav_counters[0];
            note.uav_counter_offset = call.fragment_uav_counter_offsets[0];
            note.topology = call.topology;
            m_owner.draws.push_back(note);
            return true;
        }
        [[nodiscard]] bool end() override { return true; }

    private:
        FakeBackend& m_owner;
    };

    [[nodiscard]] std::unique_ptr<graphics::BackendPipeline> create_pipeline(
        const graphics::BackendPipelineDesc& desc) override {
        pipeline_descs.push_back(desc);
        return std::make_unique<FakePipeline>();
    }
    std::vector<graphics::BackendPipelineDesc> pipeline_descs;
    [[nodiscard]] std::unique_ptr<graphics::BackendComputePipeline>
    create_compute_pipeline(
        const graphics::BackendComputePipelineDesc& desc) override {
        return std::make_unique<FakeCompute>(desc.compute_entry_point);
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendRenderPass>
    create_render_pass_standalone(
        const graphics::BackendRenderPassDesc& desc) override {
        passes.push_back(desc);
        return std::make_unique<FakePass>(*this);
    }
    [[nodiscard]] bool dispatch(const graphics::BackendDispatchCall& call) override {
        DispatchNote note{};
        note.groups = call.group_count_x;
        constexpr unsigned base = graphics::BACKEND_GS_EXPANSION_BUFFER_BASE;
        note.in0 = call.buffers[base + 0] != nullptr;
        note.out1 = call.buffers[base + 1] != nullptr;
        note.counter2 = call.buffers[base + 2] != nullptr;
        note.ids3 = call.buffers[base + 3] != nullptr;
        note.cbv_table = call.descriptor_tables[0] != nullptr;
        note.srv_table = call.descriptor_tables[1] != nullptr;
        note.sampler_table = call.descriptor_tables[2] != nullptr;
        constexpr unsigned counter = graphics::BACKEND_UAV_COUNTER_BUFFER_BASE;
        note.uav_counter = call.buffers[counter];
        note.uav_counter_offset = call.buffer_offsets[counter];
        if (call.pipeline != nullptr) {
            note.entry = static_cast<const FakeCompute*>(call.pipeline)->entry;
        }
        if (call.constants != nullptr &&
            call.constants_size == sizeof(graphics::StatsKernelParams)) {
            graphics::StatsKernelParams params{};
            std::memcpy(&params, call.constants, sizeof(params));
            note.stats_flags = params.flags;
        }
        dispatches.push_back(note);
        return true;
    }
    class FakeTable final : public graphics::BackendDescriptorTable {
    public:
        explicit FakeTable(unsigned slots) : m_slots(slots) {}
        [[nodiscard]] bool valid() const noexcept override { return true; }
        [[nodiscard]] unsigned slot_count() const noexcept override {
            return m_slots;
        }
        [[nodiscard]] bool set_texture(
            unsigned, const graphics::BackendTexture2D*,
            const graphics::BackendTextureViewDesc&) override {
            return true;
        }
        [[nodiscard]] bool set_texture_3d(
            unsigned, const graphics::BackendTexture3D*) override {
            return true;
        }
        [[nodiscard]] bool set_buffer(unsigned, const graphics::BackendBuffer*,
                                      u64) override {
            return true;
        }
        [[nodiscard]] bool set_texture_buffer(
            unsigned, const graphics::BackendTextureBuffer*) override {
            return true;
        }
        [[nodiscard]] bool set_sampler(
            unsigned, const graphics::BackendSamplerDesc&) override {
            return true;
        }
        void clear_slot(unsigned) override {}

    private:
        unsigned m_slots;
    };
    [[nodiscard]] std::unique_ptr<graphics::BackendDescriptorTable>
    create_descriptor_table(
        const graphics::BackendDescriptorTableDesc& desc) override {
        return std::make_unique<FakeTable>(desc.slot_count);
    }
    [[nodiscard]] bool copy_buffer(graphics::BackendBuffer*,
                                   const graphics::BackendBuffer*) override {
        return false;
    }
    /// Successful region copies allowed before the next one fails.
    /// Negative: never fail.
    int buffer_copies_before_failure{-1};

    [[nodiscard]] bool copy_buffer_region(graphics::BackendBuffer* dst,
                                          u64 dst_offset,
                                          const graphics::BackendBuffer* src,
                                          u64 src_offset, u64 bytes) override {
        if (buffer_copies_before_failure == 0) return false;
        if (buffer_copies_before_failure > 0) --buffer_copies_before_failure;
        stream.push_back("copy " + std::to_string(bytes));
        auto* d = static_cast<FakeBuffer*>(dst);
        auto* s = static_cast<FakeBuffer*>(const_cast<graphics::BackendBuffer*>(
            src));
        std::memcpy(d->bytes() + dst_offset, s->bytes() + src_offset,
                    static_cast<usize>(bytes));
        return true;
    }
    [[nodiscard]] bool copy_texture_2d(
        graphics::BackendTexture2D* dst,
        const graphics::BackendTexture2D* src) override {
        // The suspend/resume local copy. Pixel tests read the stored clear.
        auto* d = static_cast<FakeTexture*>(dst);
        auto* s = static_cast<FakeTexture*>(
            const_cast<graphics::BackendTexture2D*>(src));
        if (d == nullptr || s == nullptr) return false;
        d->color = s->color;
        d->has_color = s->has_color;
        d->depth = s->depth;
        d->stencil = s->stencil;
        d->has_depth = s->has_depth;
        d->has_stencil = s->has_stencil;
        return true;
    }
    [[nodiscard]] bool generate_mips(graphics::BackendTexture2D*) override {
        return false;
    }
    void flush() override { ++flushes; }
    /// Copies land in the destination before this returns, so a read-back
    /// wait has already completed. The default wait refuses a backend with
    /// no completion object; that would hide every GPU-written argument.
    [[nodiscard]] bool wait_idle() override {
        flush();
        return true;
    }
    [[nodiscard]] bool supports_texture_sample_count(
        unsigned count) const override {
        return count <= 1 || (m_tiled && count == 4);
    }
    [[nodiscard]] bool supports_tiled_resources() const override {
        return m_tiled;
    }
    [[nodiscard]] bool tiled_texture_tile_shape(
        graphics::PixelFormat format, unsigned samples, unsigned& width,
        unsigned& height) const override {
        // The same standard shapes d3d11::standard_tile_shape compares the
        // backend against (caps), from the storage format's element size.
        width = 0;
        height = 0;
        if (!m_tiled || (samples != 1 && samples != 4)) return false;
        const graphics::FormatInfo info = graphics::format_table(format);
        if (info.is_compressed) {
            if (samples != 1) return false;
            const u32 block = graphics::compressed_block_bytes(format);
            if (block == 8) {
                width = 512;
                height = 256;
                return true;
            }
            if (block == 16) {
                width = 256;
                height = 256;
                return true;
            }
            return false;
        }
        const u32 bytes = info.bytes_per_pixel;
        struct Shape {
            u32 bytes, w1, h1, w4, h4;
        };
        static constexpr Shape kShapes[] = {
            {1, 256, 256, 128, 128}, {2, 256, 128, 128, 64},
            {4, 128, 128, 64, 64},   {8, 128, 64, 64, 32},
            {16, 64, 64, 32, 32},
        };
        for (const Shape& shape : kShapes) {
            if (shape.bytes != bytes) continue;
            width = samples == 1 ? shape.w1 : shape.w4;
            height = samples == 1 ? shape.h1 : shape.h4;
            return true;
        }
        return false;
    }
    class FakeTilePool final : public graphics::BackendTilePool {
    public:
        [[nodiscard]] bool reference(u64 tile) override {
            if (tile >= size) return false;
            ++refs[tile];
            return true;
        }
        void unreference(u64 tile) override {
            const auto it = refs.find(tile);
            if (it == refs.end() || it->second == 0) return;
            if (--it->second == 0) refs.erase(it);
        }
        void set_size(u64 tiles) override { size = tiles; }
        u64 size{0};
        std::map<u64, u32> refs;
    };

    [[nodiscard]] std::unique_ptr<graphics::BackendTilePool> create_tile_pool()
        override {
        if (!m_tiled) return nullptr;
        return std::make_unique<FakeTilePool>();
    }
    [[nodiscard]] bool tiled_texture_layout(
        const graphics::BackendTexture2D* texture,
        graphics::BackendTiledLayout& layout) override {
        if (!m_tiled || texture == nullptr || !texture->desc().tiled) return false;
        unsigned w = 0;
        unsigned h = 0;
        if (!tiled_texture_tile_shape(texture->desc().pixel_format,
                                      std::max(1u, texture->desc().sample_count),
                                      w, h)) {
            return false;
        }
        layout.tile_width = w;
        layout.tile_height = h;
        // Metal packs mips smaller than the tile (a 256 RGBA8 chain's tail
        // starts at or after mip 2; tests/graphics/test_tiled_resources_gpu.cpp).
        // A mip that still covers the tile in both axes stays a standard grid,
        // including a partial edge such as 200 of a 128 tile. GetResourceTiling
        // reports NumPackedMips 0 either way; this tail is only the backend's.
        const auto& d = texture->desc();
        const unsigned mips = std::max(1u, d.mip_levels);
        layout.first_packed_level = mips;
        for (unsigned mip = 0; mip < d.mip_levels; ++mip) {
            const unsigned mw = std::max(1u, mip >= 31 ? 1u : d.width >> mip);
            const unsigned mh = std::max(1u, mip >= 31 ? 1u : d.height >> mip);
            if (mw < w || mh < h) {
                layout.first_packed_level = mip;
                break;
            }
        }
        layout.packed_tiles = layout.first_packed_level < mips ? 1u : 0u;
        return true;
    }
    [[nodiscard]] bool update_tile_mappings(
        graphics::BackendTexture2D* texture, graphics::BackendTilePool* pool,
        const graphics::BackendTileMapping* mappings, usize count,
        bool self_aliased) override {
        if (texture == nullptr || !texture->valid() || !texture->desc().tiled) {
            return false;
        }
        record_tiles(true, pool, mappings, count, self_aliased);
        return true;
    }
    [[nodiscard]] bool update_tile_mappings(
        graphics::BackendBuffer* buffer, graphics::BackendTilePool* pool,
        const graphics::BackendTileMapping* mappings, usize count,
        bool self_aliased) override {
        if (buffer == nullptr || !buffer->valid() || !buffer->desc().tiled) {
            return false;
        }
        record_tiles(false, pool, mappings, count, self_aliased);
        return true;
    }
    void attachment_barrier(graphics::BackendTexture2D* = nullptr) override {
        stream.push_back("attachment barrier");
    }
    [[nodiscard]] bool copy_texture_buffer_region(
        graphics::BackendTexture2D* texture, unsigned level, unsigned slice,
        unsigned x, unsigned y, unsigned width, unsigned height,
        graphics::BackendBuffer* buffer, u64 offset, u64 row_pitch,
        bool to_texture) override {
        if (texture == nullptr || buffer == nullptr || !texture->valid() ||
            !buffer->valid() || width == 0 || height == 0 || row_pitch == 0) {
            return false;
        }
        TextureTileCopy copy{};
        copy.texture = texture;
        copy.level = level;
        copy.slice = slice;
        copy.x = x;
        copy.y = y;
        copy.width = width;
        copy.height = height;
        copy.offset = offset;
        copy.row_pitch = row_pitch;
        copy.to_texture = to_texture;
        texture_tile_copies.push_back(copy);
        return true;
    }
    [[nodiscard]] bool supports_fences() const noexcept override {
        return true;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendFence> create_fence(
        u64 initial) override {
        return std::make_unique<FakeFence>(*this, initial);
    }
    [[nodiscard]] bool supports_placement_heaps() const noexcept override {
        return true;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendHeap> create_heap(
        const graphics::BackendHeapDesc& desc) override {
        ++heaps;
        return std::make_unique<FakeHeap>(desc.size, desc.cpu_visible);
    }
    [[nodiscard]] bool heap_texture_placement(
        const graphics::BackendTexture2DDesc& desc, u64& size,
        u64& alignment) override {
        size = static_cast<u64>(desc.width) * desc.height * 4u;
        alignment = 4096;
        return true;
    }

    std::vector<std::string> stream;
    unsigned flushes{0};
    unsigned heaps{0};
    unsigned swap_chain_creations{0};
    graphics::BackendSwapChainDesc last_swap_chain{};
    std::vector<graphics::BackendCompositionLayerDesc> composition_layers;

private:
    void record_tiles(bool texture, graphics::BackendTilePool* pool,
                      const graphics::BackendTileMapping* mappings, usize count,
                      bool self_aliased) {
        TileCall call{};
        call.texture = texture;
        call.has_pool = pool != nullptr;
        call.self_aliased = self_aliased;
        if (mappings != nullptr) {
            call.mappings.assign(mappings, mappings + count);
        }
        tile_calls.push_back(std::move(call));
    }

    bool m_tiled{false};
};

// =============================================================================
// Calls through the vtables, x64 or x86
// =============================================================================

constexpr GuestAddr kData = 0x40000;
constexpr usize kDataSize = 0x40000;
constexpr GuestAddr kStack = kData + kDataSize - 0x1000;
constexpr GuestAddr kIid = kData + 0x100;
constexpr GuestAddr kOut = kData + 0x200;
constexpr GuestAddr kOut2 = kData + 0x240;
constexpr GuestAddr kDesc = kData + 0x400;
constexpr GuestAddr kDesc2 = kData + 0x800;
constexpr GuestAddr kArray = kData + 0xC00;

class Harness {
public:
    explicit Harness(bool is_64bit, bool tiled = false)
        : m_is_64bit(is_64bit), backend(tiled) {
        EXPECT_TRUE(memory.map_region(kData, kDataSize,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.mode = is_64bit ? cpu::CpuMode::Long : cpu::CpuMode::Protected;
        manager = std::make_unique<D3D12Manager>(memory, is_64bit, backend);
        exports = create_d3d12_thunks(*manager);
        for (const auto& entry : exports.entries) by_name[entry.name] = &entry;
        D3D12Manager::MethodThunks thunks{};
        GuestAddr next = 0x70000000;
        const auto fill = [&](const char* interface_name, auto& table) {
            for (usize slot = 0; slot < table.size(); ++slot) {
                table[slot] = next;
                by_thunk[next] = d3d12_method_export_name(interface_name, slot);
                next += 0x10;
            }
        };
        for (usize table = 0; table < INTERFACE_TABLE_COUNT; ++table) {
            thunks.tables[table].resize(INTERFACE_TABLES[table].method_count);
            fill(INTERFACE_TABLES[table].export_name, thunks.tables[table]);
        }
        fill("Blob", thunks.blob);
        fill("RootSignatureDeserializer", thunks.root_signature_deserializer);
        fill("VersionedRootSignatureDeserializer",
             thunks.versioned_root_signature_deserializer);
        manager->set_method_thunks(thunks);
    }

    [[nodiscard]] u32 ptr_size() const { return m_is_64bit ? 8u : 4u; }
    [[nodiscard]] u64 read_ptr(GuestAddr at) const {
        return m_is_64bit ? memory.read_u64(at).value()
                          : memory.read_u32(at).value();
    }
    void write_ptr(GuestAddr at, u64 value) {
        EXPECT_TRUE((m_is_64bit ? memory.write_u64(at, value)
                                : memory.write_u32(at, static_cast<u32>(value)))
                        .is_ok());
    }
    void w32(GuestAddr at, u32 v) { EXPECT_TRUE(memory.write_u32(at, v).is_ok()); }
    void w64(GuestAddr at, u64 v) { EXPECT_TRUE(memory.write_u64(at, v).is_ok()); }
    [[nodiscard]] u32 r32(GuestAddr at) const { return memory.read_u32(at).value(); }
    [[nodiscard]] u64 r64(GuestAddr at) const { return memory.read_u64(at).value(); }

    /// One argument as the guest passes it: a UINT64 (or 8-byte struct) is
    /// one slot on x64 and two on x86.
    struct Arg {
        u64 value;
        bool wide{false};
    };
    [[nodiscard]] static Arg wide(u64 v) { return {v, true}; }

    /// Call export @p name; the result as the guest sees it (EAX on x86, or
    /// EDX:EAX with @p wide_return).
    u64 call(const std::string& name, const std::vector<Arg>& args,
             bool wide_return = false) {
        const auto found = by_name.find(name);
        EXPECT_NE(found, by_name.end()) << name;
        if (found == by_name.end()) return ~0ull;
        return invoke(found->second->handler, args, wide_return,
                      found->second->stdcall_bytes, name);
    }

    /// Call a DXGI entry point the runtime routes to this manager (a
    /// factory method whose pDevice is a D3D12 queue).
    u64 call_manager(void (D3D12Manager::*entry)(Marshaler&),
                     const std::vector<Arg>& args) {
        return invoke([&](Marshaler& m) { (manager.get()->*entry)(m); }, args,
                      false, 0, "");
    }

    template <typename Handler>
    u64 invoke(const Handler& handler, const std::vector<Arg>& args,
               bool wide_return, usize stdcall_bytes, const std::string& name) {
        cpu.gpr.rsp = kStack;
        cpu.gpr.rax = 0xCCCCCCCCCCCCCCCCull;
        if (!m_is_64bit) cpu.gpr.rdx = 0xCCCCCCCCCCCCCCCCull;
        if (m_is_64bit) {
            u64* regs[] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
            for (usize i = 0; i < args.size(); ++i) {
                if (i < 4) {
                    *regs[i] = args[i].value;
                } else {
                    EXPECT_TRUE(memory.write_u64(kStack + 0x28 + (i - 4) * 8,
                                                 args[i].value).is_ok());
                }
            }
        } else {
            usize slot = 0;
            for (const Arg& a : args) {
                EXPECT_TRUE(memory.write_u32(kStack + 4 + slot * 4,
                                             static_cast<u32>(a.value)).is_ok());
                ++slot;
                if (a.wide) {
                    EXPECT_TRUE(memory.write_u32(
                        kStack + 4 + slot * 4,
                        static_cast<u32>(a.value >> 32)).is_ok());
                    ++slot;
                }
            }
            // The callee pops exactly what the SDK prototype occupies.
            if (!name.empty()) EXPECT_EQ(stdcall_bytes, slot * 4) << name;
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        handler(m);
        if (m_is_64bit) return cpu.gpr.rax;
        if (wide_return) {
            return (cpu.gpr.rax & 0xFFFFFFFFull) |
                   ((cpu.gpr.rdx & 0xFFFFFFFFull) << 32);
        }
        return cpu.gpr.rax & 0xFFFFFFFFull;
    }

    /// Slot @p slot of the COM object at @p object, through its vtable.
    u64 com(GuestAddr object, usize slot, std::vector<Arg> args,
            bool wide_return = false) {
        const u64 vtable = read_ptr(object);
        EXPECT_NE(vtable, 0u) << "object 0x" << std::hex << object;
        const u64 thunk = read_ptr(vtable + slot * ptr_size());
        const auto name = by_thunk.find(thunk);
        EXPECT_NE(name, by_thunk.end()) << "vtable slot " << slot;
        if (name == by_thunk.end()) return ~0ull;
        args.insert(args.begin(), Arg{object});
        return call(name->second, args, wide_return);
    }

    GuestAddr iid(const GuidBytes& value) {
        EXPECT_TRUE(memory.write_bytes(kIid, value.data(), value.size()).is_ok());
        return kIid;
    }

    GuestAddr device(u32 level = D3D_FEATURE_LEVEL_11_0) {
        const u64 hr = call("D3D12CreateDevice",
                            {{0}, {level}, {iid(IID_ID3D12DEVICE)}, {kOut}});
        EXPECT_EQ(hr, S_OK);
        return read_ptr(kOut);
    }

    GuestAddr queue(GuestAddr dev, u32 type = D3D12_COMMAND_LIST_TYPE_DIRECT) {
        const auto& l = layout::D3D12_COMMAND_QUEUE_DESC[m_is_64bit];
        for (u32 i = 0; i < l.size; i += 4) w32(kDesc + i, 0);
        w32(kDesc + l.Type, type);
        EXPECT_EQ(com(dev, DeviceSlot::CreateCommandQueue,
                      {{kDesc}, {iid(IID_ID3D12COMMANDQUEUE)}, {kOut}}),
                  S_OK);
        return read_ptr(kOut);
    }

    GuestAddr allocator(GuestAddr dev, u32 type = D3D12_COMMAND_LIST_TYPE_DIRECT) {
        EXPECT_EQ(com(dev, DeviceSlot::CreateCommandAllocator,
                      {{type}, {iid(IID_ID3D12COMMANDALLOCATOR)}, {kOut}}),
                  S_OK);
        return read_ptr(kOut);
    }

    u64 try_list(GuestAddr dev, GuestAddr alloc, GuestAddr& out,
                 u32 type = D3D12_COMMAND_LIST_TYPE_DIRECT) {
        const u64 hr = com(dev, DeviceSlot::CreateCommandList,
                           {{0}, {type}, {alloc}, {0},
                            {iid(IID_ID3D12GRAPHICSCOMMANDLIST)}, {kOut}});
        out = read_ptr(kOut);
        return hr;
    }

    GuestAddr fence(GuestAddr dev, u64 initial) {
        EXPECT_EQ(com(dev, DeviceSlot::CreateFence,
                      {wide(initial), {0}, {iid(IID_ID3D12FENCE)}, {kOut}}),
                  S_OK);
        return read_ptr(kOut);
    }

    /// CreateCommittedResource of a buffer.
    u64 buffer(GuestAddr dev, u32 heap_type, u64 width, u32 state,
               GuestAddr& out, u32 flags = 0) {
        const auto& hp = layout::D3D12_HEAP_PROPERTIES[m_is_64bit];
        const auto& rd = layout::D3D12_RESOURCE_DESC[m_is_64bit];
        for (u32 i = 0; i < hp.size; i += 4) w32(kDesc + i, 0);
        for (u32 i = 0; i < rd.size; i += 4) w32(kDesc2 + i, 0);
        w32(kDesc + hp.Type, heap_type);
        w32(kDesc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_BUFFER);
        w64(kDesc2 + rd.Width, width);
        w32(kDesc2 + rd.Height, 1);
        EXPECT_TRUE(memory.write_u16(kDesc2 + rd.DepthOrArraySize, 1).is_ok());
        EXPECT_TRUE(memory.write_u16(kDesc2 + rd.MipLevels, 1).is_ok());
        w32(kDesc2 + rd.SampleDesc_Count, 1);
        w32(kDesc2 + rd.Layout, D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
        w32(kDesc2 + rd.Flags, flags);
        const u64 hr = com(dev, DeviceSlot::CreateCommittedResource,
                           {{kDesc}, {0}, {kDesc2}, {state}, {0},
                            {iid(IID_ID3D12RESOURCE)}, {kOut}});
        out = read_ptr(kOut);
        return hr;
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    FakeBackend backend;
    std::unique_ptr<D3D12Manager> manager;
    DllExports exports;
    std::unordered_map<std::string, const ExportEntry*> by_name;
    std::unordered_map<u64, std::string> by_thunk;
    bool m_is_64bit;
};

/// Every D3D12 test runs on both bitnesses.
class D3D12Test : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        if (!d3d12_enabled()) {
            GTEST_SKIP() << "D3D12 explicitly disabled by VITREO_D3D12=0";
        }
        h = std::make_unique<Harness>(GetParam());
    }
    std::unique_ptr<Harness> h;
};

INSTANTIATE_TEST_SUITE_P(Bitness, D3D12Test, ::testing::Values(true, false),
                         [](const auto& info) {
                             return info.param ? "x64" : "x86";
                         });

// =============================================================================
// Module exports and the explicit diagnostic opt-out
// =============================================================================

// Ordinals from the real d3d12.dll's export table (ordinal base 99): the
// SDK's d3d12.lib binds D3D12CreateDevice and D3D12GetDebugInterface BY
// ORDINAL. Stack cleanup from the x86 import library's decorations
// (libd3d12.a: _D3D12CreateDevice@16, _D3D12EnableExperimentalFeatures@16,
// _D3D12GetDebugInterface@8, ...).
TEST(D3D12Exports, OrdinalsAndStdcallBytesMatchTheImportLibrary) {
    FakeBackend backend;
    GuestMemory memory;
    D3D12Manager manager(memory, false, backend);
    const DllExports exports = create_d3d12_thunks(manager);
    const auto find = [&](std::string_view name) -> const ExportEntry* {
        for (const auto& entry : exports.entries) {
            if (entry.name == name) return &entry;
        }
        return nullptr;
    };
    struct Expect {
        const char* name;
        int ordinal;
        int bytes;
    };
    for (const Expect& e : {Expect{"D3D12CreateDevice", 101, 16},
                            Expect{"D3D12GetDebugInterface", 102, 8},
                            Expect{"D3D12CreateRootSignatureDeserializer", 106, 16},
                            Expect{"D3D12CreateVersionedRootSignatureDeserializer",
                                   107, 16},
                            Expect{"D3D12EnableExperimentalFeatures", 109, 16},
                            Expect{"D3D12SerializeRootSignature", 115, 16},
                            Expect{"D3D12SerializeVersionedRootSignature", 116,
                                   12}}) {
        const ExportEntry* entry = find(e.name);
        ASSERT_NE(entry, nullptr) << e.name;
        EXPECT_EQ(entry->ordinal, e.ordinal) << e.name;
        EXPECT_EQ(entry->stdcall_bytes, e.bytes) << e.name;
    }
    // Every vtable slot is an export with the generated cleanup.
    for (usize table = 0; table < INTERFACE_TABLE_COUNT; ++table) {
        const auto& row = INTERFACE_TABLES[table];
        for (usize slot = 0; slot < row.method_count; ++slot) {
            const ExportEntry* entry =
                find(d3d12_method_export_name(row.export_name, slot));
            ASSERT_NE(entry, nullptr) << row.export_name << " " << slot;
            EXPECT_EQ(entry->stdcall_bytes, row.stdcall_bytes[slot]);
        }
    }
}

// A struct passed by value counts its size on the x86 stack: the SDK's
// D3D12_GPU_DESCRIPTOR_HANDLE is { UINT64 ptr }, so
// SetGraphicsRootDescriptorTable(UINT, D3D12_GPU_DESCRIPTOR_HANDLE) pops 4 +
// 4 + 8 bytes, and ClearUnorderedAccessViewUint (GPU handle, CPU handle,
// resource, values, count, rects) 4 + 8 + 5 * 4.
TEST(D3D12Exports, ByValueHandlesCountTheirSize) {
    EXPECT_EQ(GRAPHICS_COMMAND_LIST_STDCALL_BYTES
                  [GraphicsCommandListSlot::SetGraphicsRootDescriptorTable],
              16u);
    EXPECT_EQ(GRAPHICS_COMMAND_LIST_STDCALL_BYTES
                  [GraphicsCommandListSlot::ClearUnorderedAccessViewUint],
              32u);
    // D3D12_CPU_DESCRIPTOR_HANDLE { SIZE_T ptr } is four bytes on x86.
    EXPECT_EQ(DEVICE_STDCALL_BYTES[DeviceSlot::CreateSampler], 12u);
}

TEST(D3D12Gate, DeviceCreationIsAvailableByDefault) {
    if (!d3d12_enabled()) GTEST_SKIP() << "D3D12 explicitly disabled";
    for (bool x64 : {true, false}) {
        Harness h(x64);
        h.write_ptr(kOut, 0);
        EXPECT_EQ(h.call("D3D12CreateDevice",
                         {{0}, {D3D_FEATURE_LEVEL_11_0}, {h.iid(IID_ID3D12DEVICE)}, {kOut}}), S_OK);
        EXPECT_NE(h.read_ptr(kOut), 0u);
    }
}

TEST(D3D12Gate, ExplicitDisableReportsUnsupportedAndClearsOutput) {
    if (d3d12_enabled()) GTEST_SKIP() << "requires VITREO_D3D12=0";
    for (bool x64 : {true, false}) {
        Harness h(x64);
        h.write_ptr(kOut, 0xdeadbeefu);
        EXPECT_EQ(h.call("D3D12CreateDevice",
                         {{0}, {D3D_FEATURE_LEVEL_11_0}, {h.iid(IID_ID3D12DEVICE)}, {kOut}}), DXGI_ERROR_UNSUPPORTED);
        EXPECT_EQ(h.read_ptr(kOut), 0u);
    }
}

// =============================================================================
// Object model (d12core_facts_probe)
// =============================================================================

// Measured (d3d12_a01_device_probe "factory as adapter"): a pointer that is
// not an IDXGIAdapter is E_INVALIDARG and leaves *ppDevice NULL. NULL is the
// default adapter and still creates the device.
TEST_P(D3D12Test, ANonAdapterPointerIsRejected) {
    h->w64(kOut, 0xDEADBEEFull);
    EXPECT_EQ(h->call("D3D12CreateDevice",
                      {{0x1000},
                       {D3D_FEATURE_LEVEL_11_0},
                       {h->iid(IID_ID3D12DEVICE)},
                       {kOut}}),
              E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kOut), 0u);
    EXPECT_NE(h->device(), 0u);
}

// A child holds a reference on its device; D3D12CreateDevice returns the
// existing device (Learn: devices are singletons per adapter).
TEST_P(D3D12Test, ChildrenReferenceTheSingletonDevice) {
    const GuestAddr dev = h->device();
    EXPECT_EQ(h->com(dev, 1, {}), 2u);  // device_addref 2
    EXPECT_EQ(h->com(dev, 2, {}), 1u);  // device_release 1
    const GuestAddr queue = h->queue(dev);
    EXPECT_EQ(h->com(dev, 1, {}), 3u);  // device_addref_with_queue 3
    EXPECT_EQ(h->com(dev, 2, {}), 2u);
    // GetDevice hands the same device back with a reference.
    EXPECT_EQ(h->com(queue, 7, {{h->iid(IID_ID3D12DEVICE)}, {kOut2}}), S_OK);
    EXPECT_EQ(h->read_ptr(kOut2), dev);
    EXPECT_EQ(h->com(dev, 2, {}), 2u);
    // second_device_same 1, second_device_release 2.
    EXPECT_EQ(h->device(), dev);
    EXPECT_EQ(h->com(dev, 2, {}), 2u);
    EXPECT_EQ(h->com(queue, 2, {}), 0u);
    EXPECT_EQ(h->com(dev, 2, {}), 0u);
}

TEST_P(D3D12Test, SetNameIsTheWideDebugNamePrivateData) {
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    const GuestAddr name = kData + 0x3000;
    for (u32 i = 0; i < 4; ++i) {
        ASSERT_TRUE(h->memory.write_u16(name + i * 2, i < 3 ? 'a' + i : 0).is_ok());
    }
    EXPECT_EQ(h->com(queue, 6, {{name}}), S_OK);
    ASSERT_TRUE(h->memory.write_bytes(kDesc, WKPDID_D3DDEBUGOBJECTNAMEW.data(),
                                      16).is_ok());
    h->w32(kOut2, 0);
    // setname_private hr=0 size=8 ("abc" and its terminator, UTF-16).
    EXPECT_EQ(h->com(queue, 3, {{kDesc}, {kOut2}, {0}}), S_OK);
    EXPECT_EQ(h->r32(kOut2), 8u);
}

TEST_P(D3D12Test, CustomHeapPropertiesAreTheCacheCoherentUmaOnes) {
    const GuestAddr dev = h->device();
    const auto& l = layout::D3D12_HEAP_PROPERTIES[GetParam()];
    struct Row {
        u32 type, cpu, pool;
    };
    // custom_heap 1/2/3: DEFAULT NOT_AVAILABLE, UPLOAD/READBACK WRITE_BACK,
    // all L0, node masks 1.
    for (const Row& row : {Row{1, 1, 1}, Row{2, 3, 1}, Row{3, 3, 1}}) {
        EXPECT_EQ(h->com(dev, DeviceSlot::GetCustomHeapProperties,
                         {{kDesc}, {0}, {row.type}}),
                  GetParam() ? kDesc : static_cast<u32>(kDesc));
        EXPECT_EQ(h->r32(kDesc + l.Type), D3D12_HEAP_TYPE_CUSTOM);
        EXPECT_EQ(h->r32(kDesc + l.CPUPageProperty), row.cpu);
        EXPECT_EQ(h->r32(kDesc + l.MemoryPoolPreference), row.pool);
        EXPECT_EQ(h->r32(kDesc + l.CreationNodeMask), 1u);
        EXPECT_EQ(h->r32(kDesc + l.VisibleNodeMask), 1u);
    }
}

// =============================================================================
// Descriptor heaps
// =============================================================================

TEST_P(D3D12Test, DescriptorHeapHandlesUseTheHiddenReturnPointer) {
    const GuestAddr dev = h->device();
    const auto& l = layout::D3D12_DESCRIPTOR_HEAP_DESC[GetParam()];
    for (u32 visible = 0; visible < 2; ++visible) {
        h->w32(kDesc + l.Type, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        h->w32(kDesc + l.NumDescriptors, 16);
        h->w32(kDesc + l.Flags,
               visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : 0u);
        h->w32(kDesc + l.NodeMask, 0);
        ASSERT_EQ(h->com(dev, DeviceSlot::CreateDescriptorHeap,
                         {{kDesc}, {h->iid(IID_ID3D12DESCRIPTORHEAP)}, {kOut}}),
                  S_OK);
        const GuestAddr heap = h->read_ptr(kOut);
        // The CPU handle is a SIZE_T: poison eight bytes and see exactly the
        // pointer width written, and RetVal returned.
        h->w64(kOut2, 0xA5A5A5A5A5A5A5A5ull);
        EXPECT_EQ(h->com(heap, DescriptorHeapSlot::GetCPUDescriptorHandleForHeapStart,
                         {{kOut2}}),
                  GetParam() ? kOut2 : static_cast<u32>(kOut2));
        EXPECT_NE(h->read_ptr(kOut2), 0u);
        if (!GetParam()) EXPECT_EQ(h->r32(kOut2 + 4), 0xA5A5A5A5u);
        // cpu_nonzero=1 gpu_nonzero=1 for both heaps (measured).
        h->w64(kOut2, 0);
        EXPECT_EQ(h->com(heap, DescriptorHeapSlot::GetGPUDescriptorHandleForHeapStart,
                         {{kOut2}}),
                  GetParam() ? kOut2 : static_cast<u32>(kOut2));
        EXPECT_NE(h->r64(kOut2), 0u);
    }
    // rtv_heap_shader_visible 0x80070057.
    h->w32(kDesc + l.Type, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    h->w32(kDesc + l.Flags, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateDescriptorHeap,
                     {{kDesc}, {h->iid(IID_ID3D12DESCRIPTORHEAP)}, {kOut}}),
              E_INVALIDARG);
}

// =============================================================================
// Resources and memory
// =============================================================================

// An UPLOAD buffer is mapped once, persistently: every Map hands out the same
// pointer (map_same_pointer, map_again_same_pointer), a buffer has one
// subresource (map_subresource1 E_INVALIDARG), and the guest's bytes are the
// memory the GPU reads -- no copy between them.
TEST_P(D3D12Test, UploadBuffersMapPersistentlyAndZeroCopy) {
    const GuestAddr dev = h->device();
    GuestAddr buffer = 0;
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 1000,
                        D3D12_RESOURCE_STATE_GENERIC_READ, buffer),
              S_OK);
    EXPECT_EQ(h->com(buffer, ResourceSlot::Map, {{0}, {0}, {0}}), S_OK);
    ASSERT_EQ(h->com(buffer, ResourceSlot::Map, {{0}, {0}, {kOut}}), S_OK);
    const GuestAddr a = h->read_ptr(kOut);
    ASSERT_EQ(h->com(buffer, ResourceSlot::Map, {{0}, {0}, {kOut}}), S_OK);
    EXPECT_EQ(h->read_ptr(kOut), a);
    for (int i = 0; i < 3; ++i) (void)h->com(buffer, ResourceSlot::Unmap, {{0}, {0}});
    ASSERT_EQ(h->com(buffer, ResourceSlot::Map, {{0}, {0}, {kOut}}), S_OK);
    EXPECT_EQ(h->read_ptr(kOut), a);
    EXPECT_EQ(h->com(buffer, ResourceSlot::Map, {{1}, {0}, {kOut}}),
              E_INVALIDARG);
    // The guest's write lands in the backend heap's host memory.
    ASSERT_TRUE(h->memory.write_u32(a + 12, 0x12345678u).is_ok());
    ASSERT_NE(h->backend.heaps, 0u);
    // GPU VA: non-zero and 64 KiB aligned (upload_va_nonzero, va_mod_64k 0).
    const u64 va = h->com(buffer, ResourceSlot::GetGPUVirtualAddress, {}, true);
    EXPECT_NE(va, 0u);
    EXPECT_EQ(va % 65536u, 0u);
    // GetDesc reports the 64 KiB alignment and ROW_MAJOR layout
    // (upload_desc alignment=65536 width=1000 layout=1).
    const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
    EXPECT_EQ(h->com(buffer, ResourceSlot::GetDesc, {{kDesc}}),
              GetParam() ? kDesc : static_cast<u32>(kDesc));
    EXPECT_EQ(h->r64(kDesc + rd.Alignment), 65536u);
    EXPECT_EQ(h->r64(kDesc + rd.Width), 1000u);
    EXPECT_EQ(h->r32(kDesc + rd.Layout), D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
    // upload_heap_props type=2 cpu=0 pool=0 create=1 visible=1 flags=0xc0.
    const auto& hp = layout::D3D12_HEAP_PROPERTIES[GetParam()];
    EXPECT_EQ(h->com(buffer, ResourceSlot::GetHeapProperties,
                     {{kDesc}, {kOut2}}),
              S_OK);
    EXPECT_EQ(h->r32(kDesc + hp.Type), D3D12_HEAP_TYPE_UPLOAD);
    EXPECT_EQ(h->r32(kDesc + hp.CPUPageProperty), 0u);
    EXPECT_EQ(h->r32(kDesc + hp.CreationNodeMask), 1u);
    EXPECT_EQ(h->r32(kOut2), 0xC0u);
}

// GetDesc copies immutable creation metadata. Independent callers must make
// progress while command-list recording/replay holds shared ownership.
TEST_P(D3D12Test, ImmutableQueriesDoNotDrainRecordingOrReplay) {
    const GuestAddr dev = h->device();
    GuestAddr buffer = 0;
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 256,
                        D3D12_RESOURCE_STATE_GENERIC_READ, buffer), S_OK);
    const auto* entry = h->by_name.at(
        d3d12_method_export_name("Resource", ResourceSlot::GetDesc));
    constexpr u64 initial = 0x100000002ull;
    const GuestAddr fence = h->fence(dev, initial);
    const auto* query = h->by_name.at(
        d3d12_method_export_name("Fence", FenceSlot::GetCompletedValue));
    const GuestAddr queue = h->queue(dev);
    ASSERT_EQ(h->com(queue, CommandQueueSlot::GetTimestampFrequency, {{kOut2}}), S_OK);
    const u64 frequency = h->r64(kOut2);
    ASSERT_GT(frequency, 0u);
    const auto* clock = h->by_name.at(
        d3d12_method_export_name("CommandQueue", CommandQueueSlot::GetTimestampFrequency));
    const auto* reason = h->by_name.at(
        d3d12_method_export_name("Device", DeviceSlot::GetDeviceRemovedReason));
    constexpr usize threads = 8;
    std::array<std::future<void>, threads> workers;
    auto recording = std::make_unique<FrontendLock>(
        h->manager->frontend(), FrontendRwLock::Mode::Shared);
    auto replay = std::make_unique<ReplayLock>(h->manager->frontend());
    for (usize t = 0; t < threads; ++t) {
        workers[t] = std::async(std::launch::async, [&, t] {
            cpu::CpuState state;
            state.mode = GetParam() ? cpu::CpuMode::Long : cpu::CpuMode::Protected;
            const GuestAddr stack = kData + 0x20000 + t * 0x1000;
            const GuestAddr out = stack + 0x100;
            const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
            for (usize i = 0; i < 128; ++i) {
                state.gpr.rsp = stack;
                if (GetParam()) {
                    state.gpr.rcx = buffer;
                    state.gpr.rdx = out;
                } else {
                    (void)h->memory.write_u32(stack + 4, static_cast<u32>(buffer));
                    (void)h->memory.write_u32(stack + 8, static_cast<u32>(out));
                }
                Marshaler marshaler(h->memory);
                marshaler.attach_cpu(state);
                entry->handler(marshaler);
                EXPECT_EQ(static_cast<GuestAddr>(state.gpr.rax), out);
                EXPECT_EQ(h->r64(out + rd.Width), 256u);
                EXPECT_EQ(h->r32(out + rd.Dimension), D3D12_RESOURCE_DIMENSION_BUFFER);
                EXPECT_EQ(h->r32(out + rd.Layout), D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
                if (GetParam()) state.gpr.rcx = fence;
                else (void)h->memory.write_u32(stack + 4, static_cast<u32>(fence));
                query->handler(marshaler);
                const u64 completed = GetParam()
                    ? state.gpr.rax
                    : ((state.gpr.rdx & 0xFFFFFFFFull) << 32) |
                          (state.gpr.rax & 0xFFFFFFFFull);
                EXPECT_EQ(completed, initial);
                if (GetParam()) {
                    state.gpr.rcx = queue;
                    state.gpr.rdx = out;
                } else {
                    (void)h->memory.write_u32(stack + 4, static_cast<u32>(queue));
                    (void)h->memory.write_u32(stack + 8, static_cast<u32>(out));
                }
                clock->handler(marshaler);
                EXPECT_EQ(static_cast<u32>(state.gpr.rax), S_OK);
                EXPECT_EQ(h->r64(out), frequency);
                if (GetParam()) state.gpr.rcx = dev;
                else (void)h->memory.write_u32(stack + 4, static_cast<u32>(dev));
                reason->handler(marshaler);
                EXPECT_EQ(static_cast<u32>(state.gpr.rax), S_OK);
            }
        });
    }
    bool completed_while_recording = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (auto& worker : workers) {
        completed_while_recording &= worker.wait_until(deadline) == std::future_status::ready;
    }
    replay.reset();
    recording.reset(); // unblock and join even with the old exclusive getter
    for (auto& worker : workers) worker.get();
    EXPECT_TRUE(completed_while_recording);
}

TEST_P(D3D12Test, DISABLED_RootBufferSpanIndexUnrelatedObjectBenchmark) {
    const GuestAddr dev = h->device();
    GuestAddr buffer = 0;
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 4096,
                       D3D12_RESOURCE_STATE_GENERIC_READ, buffer), S_OK);
    const u64 va = h->com(buffer, ResourceSlot::GetGPUVirtualAddress, {}, true);
    for (u32 i = 0; i != 10000; ++i) ASSERT_NE(h->allocator(dev), 0u);
    auto& frontend = h->manager->frontend();
    ASSERT_EQ(frontend.buffer_words_from_va(va), 4096u / sizeof(u32));
    constexpr u32 iterations = 1000;
    const auto begin = std::chrono::steady_clock::now();
    for (u32 i = 0; i != iterations; ++i) {
        const GuestAddr allocator = h->allocator(dev);
        EXPECT_EQ(h->com(allocator, 2, {}), 0u);
        EXPECT_EQ(frontend.buffer_words_from_va(va), 4096u / sizeof(u32));
    }
    const double us = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - begin).count() / iterations;
    std::printf("[d3d12-span-bench] unrelated_objects=10000 us_per_iteration=%.3f\n", us);
}

TEST_P(D3D12Test, HeapTypesTakeTheMeasuredInitialStates) {
    const GuestAddr dev = h->device();
    GuestAddr out = 0;
    // upload_state_common 0, readback_state_generic_read 0x80070057,
    // readback_state_copy_dest 0.
    EXPECT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 256,
                        D3D12_RESOURCE_STATE_COMMON, out),
              S_OK);
    EXPECT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_READBACK, 256,
                        D3D12_RESOURCE_STATE_GENERIC_READ, out),
              E_INVALIDARG);
    EXPECT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_READBACK, 256,
                        D3D12_RESOURCE_STATE_COPY_DEST, out),
              S_OK);
    // default_map 0x80070057, and *ppData left alone (default_map_out_null 0).
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 256,
                        D3D12_RESOURCE_STATE_COMMON, out),
              S_OK);
    h->write_ptr(kOut2, 1);
    EXPECT_EQ(h->com(out, ResourceSlot::Map, {{0}, {0}, {kOut2}}),
              E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kOut2), 1u);
}

// A placed buffer lies inside its heap at a 64 KiB-aligned offset, and holds
// no COM reference on the heap (heap_addref_with_placed 2 after one create).
TEST_P(D3D12Test, PlacedResourcesLieInsideTheirHeap) {
    const GuestAddr dev = h->device();
    const auto& hd = layout::D3D12_HEAP_DESC[GetParam()];
    for (u32 i = 0; i < hd.size; i += 4) h->w32(kDesc + i, 0);
    h->w64(kDesc + hd.SizeInBytes, 1u << 20);
    h->w32(kDesc + hd.Properties_Type, D3D12_HEAP_TYPE_DEFAULT);
    h->w32(kDesc + hd.Flags, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateHeap,
                     {{kDesc}, {h->iid(IID_ID3D12HEAP)}, {kOut}}),
              S_OK);
    const GuestAddr heap = h->read_ptr(kOut);
    // heap_desc: size 1048576, alignment 65536 (0 in), node masks 1.
    EXPECT_EQ(h->com(heap, HeapSlot::GetDesc, {{kDesc2}}),
              GetParam() ? kDesc2 : static_cast<u32>(kDesc2));
    EXPECT_EQ(h->r64(kDesc2 + hd.Alignment), 65536u);
    EXPECT_EQ(h->r32(kDesc2 + hd.Properties_CreationNodeMask), 1u);
    const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
    const GuestAddr desc = kData + 0x1000;
    for (u32 i = 0; i < rd.size; i += 4) h->w32(desc + i, 0);
    h->w32(desc + rd.Dimension, D3D12_RESOURCE_DIMENSION_BUFFER);
    h->w64(desc + rd.Width, 4096);
    h->w32(desc + rd.Height, 1);
    ASSERT_TRUE(h->memory.write_u16(desc + rd.DepthOrArraySize, 1).is_ok());
    ASSERT_TRUE(h->memory.write_u16(desc + rd.MipLevels, 1).is_ok());
    h->w32(desc + rd.SampleDesc_Count, 1);
    h->w32(desc + rd.Layout, D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
    const auto place = [&](u64 offset) {
        return h->com(dev, DeviceSlot::CreatePlacedResource,
                      {{heap}, Harness::wide(offset), {desc},
                       {D3D12_RESOURCE_STATE_COMMON}, {0},
                       {h->iid(IID_ID3D12RESOURCE)}, {kOut}});
    };
    EXPECT_EQ(place(65536), S_OK);                    // placed_buffer
    EXPECT_EQ(h->com(heap, 1, {}), 2u);               // no reference taken
    EXPECT_EQ(h->com(heap, 2, {}), 1u);
    EXPECT_EQ(place(1000), E_INVALIDARG);             // placed_misaligned
    EXPECT_EQ(place((1u << 20) - 65536), S_OK);       // placed_last_64k
    EXPECT_EQ(place(1u << 20), E_INVALIDARG);         // placed_past_end
}

// =============================================================================
// Fences and queues
// =============================================================================

// A CPU Signal sets the value, even below the current one (fence_initial 7,
// fence_after_lower 3). GetCompletedValue is a UINT64 return: EDX:EAX on x86.
TEST_P(D3D12Test, FenceCpuSignalSetsTheValue) {
    const GuestAddr dev = h->device();
    const GuestAddr fence = h->fence(dev, 7);
    EXPECT_EQ(h->com(fence, FenceSlot::GetCompletedValue, {}, true), 7u);
    EXPECT_EQ(h->com(fence, FenceSlot::Signal, {Harness::wide(3)}), S_OK);
    EXPECT_EQ(h->com(fence, FenceSlot::GetCompletedValue, {}, true), 3u);
    EXPECT_EQ(h->com(fence, FenceSlot::Signal,
                     {Harness::wide(0x100000002ull)}),
              S_OK);
    EXPECT_EQ(h->com(fence, FenceSlot::GetCompletedValue, {}, true),
              0x100000002ull);
}

// A queue that waits for a value no signal has been submitted for holds its
// later work until one is: the backend stream never carries a wait ahead of
// the signal that releases it (d3d12_queue.cpp).
TEST_P(D3D12Test, CrossQueueWaitHoldsWorkUntilTheSignalIsSubmitted) {
    const GuestAddr dev = h->device();
    const GuestAddr direct = h->queue(dev);
    const GuestAddr copy = h->queue(dev, D3D12_COMMAND_LIST_TYPE_COPY);
    const GuestAddr fence = h->fence(dev, 0);
    EXPECT_EQ(h->com(direct, CommandQueueSlot::Wait,
                     {{fence}, Harness::wide(5)}),
              S_OK);
    EXPECT_EQ(h->com(direct, CommandQueueSlot::Signal,
                     {{fence}, Harness::wide(6)}),
              S_OK);
    EXPECT_TRUE(h->backend.stream.empty())
        << "nothing may reach the stream before the signal the wait needs";
    EXPECT_EQ(h->com(copy, CommandQueueSlot::Signal,
                     {{fence}, Harness::wide(5)}),
              S_OK);
    EXPECT_EQ(h->backend.stream,
              (std::vector<std::string>{"signal 5", "wait 5", "signal 6"}));
}

TEST_P(D3D12Test, QueueSignalsPropagateAcrossAllLiveQueues) {
    const GuestAddr dev = h->device();
    const bool benchmark = std::getenv("VITREO_D3D12_QUEUE_BENCH") != nullptr;
    // Allocators are unrelated to fence propagation. They must not change
    // the result or become the work performed for each queue signal.
    for (usize i = 0; i < (benchmark ? 10000u : 64u); ++i) (void)h->allocator(dev);
    const GuestAddr start = h->fence(dev, 0);
    const GuestAddr middle = h->fence(dev, 0);
    const GuestAddr end = h->fence(dev, 0);
    GuestAddr first = h->queue(dev);
    const GuestAddr second = h->queue(dev, D3D12_COMMAND_LIST_TYPE_COPY);
    const auto chain = [&](u64 value) {
        EXPECT_EQ(h->com(second, CommandQueueSlot::Wait,
                         {{middle}, Harness::wide(value)}), S_OK);
        EXPECT_EQ(h->com(second, CommandQueueSlot::Signal,
                         {{end}, Harness::wide(value)}), S_OK);
        EXPECT_EQ(h->com(first, CommandQueueSlot::Wait,
                         {{start}, Harness::wide(value)}), S_OK);
        EXPECT_EQ(h->com(first, CommandQueueSlot::Signal,
                         {{middle}, Harness::wide(value)}), S_OK);
        EXPECT_LT(h->com(end, FenceSlot::GetCompletedValue, {}, true), value);
        EXPECT_EQ(h->com(start, FenceSlot::Signal, {Harness::wide(value)}), S_OK);
        EXPECT_EQ(h->com(end, FenceSlot::GetCompletedValue, {}, true), value);
    };
    chain(1);
    EXPECT_EQ(h->com(first, 2, {}), 0u); // final Release removes its queue
    first = h->queue(dev); // a new identity, possibly at a recycled address
    chain(2);
    if (benchmark) {
        constexpr usize signals = 1000;
        const auto begin = std::chrono::steady_clock::now();
        for (usize i = 0; i < signals; ++i) {
            EXPECT_EQ(h->com(start, FenceSlot::Signal, {Harness::wide(i + 3)}), S_OK);
        }
        const double us = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - begin).count();
        std::printf("[d3d12-queue-bench] %s allocators=10000 signals=%zu us/signal=%.3f\n",
                    GetParam() ? "x64" : "x86", signals, us / signals);
    }
}

// =============================================================================
// Command lists
// =============================================================================

TEST_P(D3D12Test, ListAndAllocatorFollowTheDocumentedStateRules) {
    const GuestAddr dev = h->device();
    const GuestAddr alloc = h->allocator(dev);
    GuestAddr list = 0;
    ASSERT_EQ(h->try_list(dev, alloc, list), S_OK);
    // Learn: an allocator backs one recording list at a time.
    GuestAddr second = 0;
    EXPECT_EQ(h->try_list(dev, alloc, second), E_INVALIDARG);
    // Reset of an allocator with a recording list: E_FAIL.
    EXPECT_EQ(h->com(alloc, CommandAllocatorSlot::Reset, {}), E_FAIL);
    // Reset of a recording list: E_FAIL; Close twice: E_FAIL.
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Reset, {{alloc}, {0}}),
              E_FAIL);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), E_FAIL);
    EXPECT_EQ(h->com(alloc, CommandAllocatorSlot::Reset, {}), S_OK);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Reset, {{alloc}, {0}}),
              S_OK);
    // A type mismatch between allocator and list.
    const GuestAddr compute = h->allocator(dev, D3D12_COMMAND_LIST_TYPE_COMPUTE);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Reset, {{compute}, {0}}),
              E_INVALIDARG);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::GetType, {}),
              D3D12_COMMAND_LIST_TYPE_DIRECT);
}

// A recording method that cannot be honoured records the error Close then
// returns, and such a list can never be reset (Learn, Close / Reset).
TEST_P(D3D12Test, RecordingErrorsSurfaceFromCloseAndReset) {
    const GuestAddr dev = h->device();
    const GuestAddr alloc = h->allocator(dev);
    GuestAddr list = 0;
    ASSERT_EQ(h->try_list(dev, alloc, list), S_OK);
    // CopyBufferRegion from a non-resource: an invalid argument.
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{0x1234}, Harness::wide(0), {0x5678}, Harness::wide(0),
                  Harness::wide(16)});
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), E_INVALIDARG);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Reset, {{alloc}, {0}}),
              E_INVALIDARG);
}

// ID3D12GraphicsCommandList7::Barrier through the list's vtable: the core
// runtime's rules measured on winoracle (a07 enhanced_rules), expected values
// from there.
// DirectX-Specs Enhanced Barriers: AccessBefore is what the work before the
// barrier did (ACCESS_COMMON on a texture: what its LayoutBefore allows).
// Ending RENDER_TARGET / DEPTH_STENCIL_WRITE work is what ResourceBarrier's
// transition out of RENDER_TARGET / DEPTH_WRITE is; the begin half of a
// split barrier and a NO_ACCESS before order nothing.
TEST(D3D12EnhancedBarrierRule, EndsAttachmentWritesLikeALegacyTransition) {
    constexpr u32 RT = D3D12_BARRIER_ACCESS_RENDER_TARGET;
    constexpr u32 DSW = D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
    constexpr u32 SRV = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
    constexpr u32 L_RT = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    constexpr u32 L_DSW = D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
    constexpr u32 L_DSR = D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ;
    constexpr u32 L_SRV = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
    constexpr u32 TEX = D3D12_BARRIER_TYPE_TEXTURE;
    constexpr u32 GLOBAL = D3D12_BARRIER_TYPE_GLOBAL;
    const auto barrier = [](u32 ab, u32 aa, u32 lb, u32 la, u32 sync_after) {
        EnhancedBarrier b;
        b.sync_before = D3D12_BARRIER_SYNC_RENDER_TARGET;
        b.sync_after = sync_after;
        b.access_before = ab;
        b.access_after = aa;
        b.layout_before = lb;
        b.layout_after = la;
        return b;
    };
    constexpr u32 PS = D3D12_BARRIER_SYNC_PIXEL_SHADING;
    constexpr u32 SPLIT = D3D12_BARRIER_SYNC_SPLIT;
    EXPECT_TRUE(ends_attachment_writes(barrier(RT, SRV, L_RT, L_SRV, PS), TEX));
    EXPECT_TRUE(ends_attachment_writes(barrier(DSW, SRV, L_DSW, L_DSR, PS), TEX));
    EXPECT_TRUE(ends_attachment_writes(barrier(D3D12_BARRIER_ACCESS_COMMON, SRV,
                                               L_RT, L_SRV, PS),
                                       TEX));
    EXPECT_TRUE(ends_attachment_writes(barrier(RT, SRV, 0, 0, PS), GLOBAL));
    // The end half of a split barrier (SyncBefore SPLIT) ends them too.
    EnhancedBarrier end = barrier(RT, SRV, L_RT, L_SRV, PS);
    end.sync_before = SPLIT;
    EXPECT_TRUE(ends_attachment_writes(end, TEX));
    EXPECT_FALSE(ends_attachment_writes(barrier(RT, SRV, L_RT, L_SRV, SPLIT), TEX));
    EXPECT_FALSE(ends_attachment_writes(
        barrier(D3D12_BARRIER_ACCESS_NO_ACCESS, SRV, L_RT, L_SRV, PS), TEX));
    EXPECT_FALSE(ends_attachment_writes(barrier(RT, RT, L_RT, L_RT, PS), TEX));
    EXPECT_FALSE(ends_attachment_writes(barrier(SRV, RT, L_SRV, L_RT, PS), TEX));
    // A layout is a texture's: a global barrier's layout words are not read.
    EXPECT_FALSE(ends_attachment_writes(
        barrier(D3D12_BARRIER_ACCESS_COMMON, SRV, L_RT, L_SRV, PS), GLOBAL));
}

// ID3D12GraphicsCommandList7::Barrier reaches the backend's
// attachment_barrier as ResourceBarrier's transition out of RENDER_TARGET
// does: a texture rendered and then read through a heap window (which the
// backend cannot see per draw) must have its texels stored first.
TEST_P(D3D12Test, EnhancedBarrierOutOfRenderTargetEndsTheAttachmentPass) {
    const bool x64 = GetParam();
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    const auto& hp = layout::D3D12_HEAP_PROPERTIES[x64];
    const auto& rd = layout::D3D12_RESOURCE_DESC[x64];
    for (u32 i = 0; i < hp.size; i += 4) h->w32(kDesc + i, 0);
    for (u32 i = 0; i < rd.size; i += 4) h->w32(kDesc2 + i, 0);
    h->w32(kDesc + hp.Type, D3D12_HEAP_TYPE_DEFAULT);
    h->w32(kDesc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_TEXTURE2D);
    h->w64(kDesc2 + rd.Width, 4);
    h->w32(kDesc2 + rd.Height, 4);
    ASSERT_TRUE(h->memory.write_u16(kDesc2 + rd.DepthOrArraySize, 1).is_ok());
    ASSERT_TRUE(h->memory.write_u16(kDesc2 + rd.MipLevels, 1).is_ok());
    h->w32(kDesc2 + rd.Format, DXGI_FORMAT_R8G8B8A8_UNORM);
    h->w32(kDesc2 + rd.SampleDesc_Count, 1);
    h->w32(kDesc2 + rd.Flags, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateCommittedResource,
                     {{kDesc}, {0}, {kDesc2}, {D3D12_RESOURCE_STATE_RENDER_TARGET},
                      {0}, {h->iid(IID_ID3D12RESOURCE)}, {kOut}}),
              S_OK);
    const GuestAddr target = h->read_ptr(kOut);

    const auto& gl = layout::D3D12_BARRIER_GROUP[x64];
    const auto& tb = layout::D3D12_TEXTURE_BARRIER[x64];
    // The backend stream one executed list of one texture barrier leaves.
    const auto run = [&](u32 sync_before, u32 sync_after, u32 access_before,
                         u32 access_after,
                         u32 layout_before, u32 layout_after) {
        const GuestAddr alloc = h->allocator(dev);
        GuestAddr list = 0;
        EXPECT_EQ(h->try_list(dev, alloc, list), S_OK);
        for (u32 i = 0; i < 0x80; i += 4) h->w32(kArray + i, 0);
        h->w32(kArray + tb.SyncBefore, sync_before);
        h->w32(kArray + tb.SyncAfter, sync_after);
        h->w32(kArray + tb.AccessBefore, access_before);
        h->w32(kArray + tb.AccessAfter, access_after);
        h->w32(kArray + tb.LayoutBefore, layout_before);
        h->w32(kArray + tb.LayoutAfter, layout_after);
        h->write_ptr(kArray + tb.pResource, target);
        h->w32(kArray + tb.Subresources_IndexOrFirstMipLevel, 0xFFFFFFFFu);
        h->w32(kDesc2 + gl.Type, D3D12_BARRIER_TYPE_TEXTURE);
        h->w32(kDesc2 + gl.NumBarriers, 1);
        h->write_ptr(kDesc2 + gl.pTextureBarriers, kArray);
        (void)h->com(list, GraphicsCommandListSlot::Barrier, {{1}, {kDesc2}});
        EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
        h->backend.stream.clear();
        h->write_ptr(kArray + 0x100, list);
        (void)h->com(queue, CommandQueueSlot::ExecuteCommandLists,
                     {{1}, {kArray + 0x100}});
        return std::count(h->backend.stream.begin(), h->backend.stream.end(),
                          std::string("attachment barrier"));
    };
    EXPECT_EQ(run(D3D12_BARRIER_SYNC_RENDER_TARGET,
                  D3D12_BARRIER_SYNC_PIXEL_SHADING,
                  D3D12_BARRIER_ACCESS_RENDER_TARGET,
                  D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                  D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                  D3D12_BARRIER_LAYOUT_SHADER_RESOURCE),
              1);
    // Back into RENDER_TARGET: nothing rendered is left to store.
    EXPECT_EQ(run(D3D12_BARRIER_SYNC_PIXEL_SHADING,
                  D3D12_BARRIER_SYNC_RENDER_TARGET,
                  D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                  D3D12_BARRIER_ACCESS_RENDER_TARGET,
                  D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                  D3D12_BARRIER_LAYOUT_RENDER_TARGET),
              0);
}

// Learn D3D12_RESOURCE_ALIASING_BARRIER: either or both resources may be
// NULL. All forms must store pending attachment writes before alias use;
// Metal's tracking across encoders cannot order reads within a kept-open pass.
TEST_P(D3D12Test, AliasingBarriersEndPendingAttachmentWrites) {
    const bool x64 = GetParam();
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    const auto& hd = layout::D3D12_HEAP_DESC[x64];
    const auto& rd = layout::D3D12_RESOURCE_DESC[x64];
    for (u32 i = 0; i < hd.size; i += 4) h->w32(kDesc + i, 0);
    h->w64(kDesc + hd.SizeInBytes, 65536);
    h->w32(kDesc + hd.Properties_Type, D3D12_HEAP_TYPE_DEFAULT);
    h->w32(kDesc + hd.Flags, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateHeap,
        {{kDesc}, {h->iid(IID_ID3D12HEAP)}, {kOut}}), S_OK);
    const GuestAddr heap = h->read_ptr(kOut);
    for (u32 i = 0; i < rd.size; i += 4) h->w32(kDesc2 + i, 0);
    h->w32(kDesc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_BUFFER);
    h->w64(kDesc2 + rd.Width, 256);
    h->w32(kDesc2 + rd.Height, 1);
    ASSERT_TRUE(h->memory.write_u16(kDesc2 + rd.DepthOrArraySize, 1).is_ok());
    ASSERT_TRUE(h->memory.write_u16(kDesc2 + rd.MipLevels, 1).is_ok());
    h->w32(kDesc2 + rd.SampleDesc_Count, 1);
    h->w32(kDesc2 + rd.Layout, D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
    std::array<GuestAddr, 2> aliases{};
    for (auto& resource : aliases) {
        ASSERT_EQ(h->com(dev, DeviceSlot::CreatePlacedResource,
            {{heap}, Harness::wide(0), {kDesc2}, {D3D12_RESOURCE_STATE_COMMON},
             {0}, {h->iid(IID_ID3D12RESOURCE)}, {kOut}}), S_OK);
        resource = h->read_ptr(kOut);
    }
    const auto& b = layout::D3D12_RESOURCE_BARRIER[x64];
    for (u32 sides = 0; sides != 4; ++sides) {
        const GuestAddr allocator = h->allocator(dev);
        GuestAddr list = 0;
        ASSERT_EQ(h->try_list(dev, allocator, list), S_OK);
        for (u32 i = 0; i < b.size; i += 4) h->w32(kArray + i, 0);
        h->w32(kArray + b.Type, D3D12_RESOURCE_BARRIER_TYPE_ALIASING);
        h->write_ptr(kArray + b.Aliasing_pResourceBefore, (sides & 1) ? aliases[0] : 0);
        h->write_ptr(kArray + b.Aliasing_pResourceAfter, (sides & 2) ? aliases[1] : 0);
        (void)h->com(list, GraphicsCommandListSlot::ResourceBarrier, {{1}, {kArray}});
        ASSERT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
        h->backend.stream.clear();
        h->write_ptr(kArray + 0x100, list);
        (void)h->com(queue, CommandQueueSlot::ExecuteCommandLists,
            {{1}, {kArray + 0x100}});
        EXPECT_EQ(std::count(h->backend.stream.begin(), h->backend.stream.end(),
                             std::string("attachment barrier")), 1) << sides;
    }
}

TEST_P(D3D12Test, EnhancedBarriersFollowTheMeasuredRuntimeRules) {
    const bool x64 = GetParam();
    const GuestAddr dev = h->device();
    // OPTIONS12.EnhancedBarriersSupported (D3D12_FEATURE_D3D12_OPTIONS12 = 41).
    for (u32 i = 0; i < 12; i += 4) h->w32(kDesc + i, 0xEEEEEEEE);
    ASSERT_EQ(h->com(dev, DeviceSlot::CheckFeatureSupport, {{41}, {kDesc}, {12}}),
              S_OK);
    EXPECT_EQ(h->r32(kDesc + 4), 1u);

    GuestAddr uav = 0;
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 256, 0, uav,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
              S_OK);
    GuestAddr plain = 0;
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 256, 0, plain), S_OK);

    const auto& gl = layout::D3D12_BARRIER_GROUP[x64];
    const auto& bl = layout::D3D12_BUFFER_BARRIER[x64];
    const auto& gb = layout::D3D12_GLOBAL_BARRIER[x64];
    struct Buf {
        u32 sb, sa, ab, aa;
        GuestAddr r;
        u64 offset, size;
    };
    // Close's answer after one barrier group of @p type in a fresh list.
    const auto close_after = [&](u32 list_type, u32 type, auto&& fill) {
        const GuestAddr alloc = h->allocator(dev, list_type);
        GuestAddr list = 0;
        EXPECT_EQ(h->try_list(dev, alloc, list, list_type), S_OK);
        for (u32 i = 0; i < 0x80; i += 4) h->w32(kArray + i, 0);
        fill(kArray);
        h->w32(kDesc2 + gl.Type, type);
        h->w32(kDesc2 + gl.NumBarriers, 1);
        h->write_ptr(kDesc2 + gl.pBufferBarriers, kArray);
        (void)h->com(list, GraphicsCommandListSlot::Barrier, {{1}, {kDesc2}});
        return h->com(list, GraphicsCommandListSlot::Close, {});
    };
    const auto buffer_close = [&](u32 list_type, const Buf& b) {
        return close_after(list_type, D3D12_BARRIER_TYPE_BUFFER, [&](GuestAddr at) {
            h->w32(at + bl.SyncBefore, b.sb);
            h->w32(at + bl.SyncAfter, b.sa);
            h->w32(at + bl.AccessBefore, b.ab);
            h->w32(at + bl.AccessAfter, b.aa);
            h->write_ptr(at + bl.pResource, b.r);
            h->w64(at + bl.Offset, b.offset);
            h->w64(at + bl.Size, b.size);
        });
    };
    constexpr u32 DIRECT = D3D12_COMMAND_LIST_TYPE_DIRECT;
    constexpr u32 COPY = D3D12_COMMAND_LIST_TYPE_COPY;
    constexpr u32 S_COPY = D3D12_BARRIER_SYNC_COPY;
    constexpr u32 S_CS = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
    constexpr u32 A_CD = D3D12_BARRIER_ACCESS_COPY_DEST;
    constexpr u32 A_UAV = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    constexpr u64 ALL = ~0ull;
    EXPECT_EQ(buffer_close(DIRECT, {S_COPY, S_CS, A_CD, A_UAV, uav, 0, ALL}), S_OK);
    EXPECT_EQ(buffer_close(DIRECT, {S_COPY, S_CS, A_CD, A_UAV, uav, 0, 256}), S_OK);
    EXPECT_EQ(buffer_close(DIRECT, {S_COPY, S_CS, A_CD, A_UAV, uav, 0, 128}),
              E_INVALIDARG);
    EXPECT_EQ(buffer_close(DIRECT, {S_COPY, S_CS, A_CD, A_UAV, uav, 16, ALL}),
              E_INVALIDARG);
    EXPECT_EQ(buffer_close(DIRECT, {S_COPY, S_CS, A_CD, A_UAV, 0, 0, ALL}),
              E_INVALIDARG);
    // UNORDERED_ACCESS needs ALLOW_UNORDERED_ACCESS; SYNC_NONE takes
    // NO_ACCESS only; COPY sync is not a UAV sync, but any compatible bit of
    // an access set passes.
    EXPECT_EQ(buffer_close(DIRECT, {S_COPY, S_CS, A_CD, A_UAV, plain, 0, ALL}),
              E_INVALIDARG);
    EXPECT_EQ(buffer_close(DIRECT, {0, S_CS, 0, A_UAV, uav, 0, ALL}), E_INVALIDARG);
    EXPECT_EQ(buffer_close(DIRECT, {0, S_COPY, D3D12_BARRIER_ACCESS_NO_ACCESS, A_CD,
                                    uav, 0, ALL}),
              S_OK);
    EXPECT_EQ(buffer_close(DIRECT, {S_COPY, S_COPY, A_UAV, A_UAV, uav, 0, ALL}),
              E_INVALIDARG);
    EXPECT_EQ(buffer_close(DIRECT, {S_COPY, S_COPY, A_CD | A_UAV, A_CD, uav, 0, ALL}),
              S_OK);
    EXPECT_EQ(buffer_close(DIRECT, {S_COPY, D3D12_BARRIER_SYNC_SPLIT, A_CD, A_UAV,
                                    uav, 0, ALL}),
              S_OK);
    EXPECT_EQ(buffer_close(DIRECT, {S_COPY, D3D12_BARRIER_SYNC_SPLIT | S_CS, A_CD,
                                    A_UAV, uav, 0, ALL}),
              E_INVALIDARG);
    // A copy list has COPY and ALL syncs and copy accesses only.
    EXPECT_EQ(buffer_close(COPY, {S_COPY, S_COPY, A_CD,
                                  D3D12_BARRIER_ACCESS_COPY_SOURCE, uav, 0, ALL}),
              S_OK);
    EXPECT_EQ(buffer_close(COPY, {S_COPY, S_CS, A_CD, A_UAV, uav, 0, ALL}),
              E_INVALIDARG);
    // A bundle cannot record barriers: E_FAIL.
    EXPECT_EQ(buffer_close(D3D12_COMMAND_LIST_TYPE_BUNDLE,
                           {S_COPY, S_CS, A_CD, A_UAV, uav, 0, ALL}),
              E_FAIL);
    // A global barrier on a compute list cannot name RENDER_TARGET access.
    const auto global_close = [&](u32 list_type, u32 aa) {
        return close_after(list_type, D3D12_BARRIER_TYPE_GLOBAL, [&](GuestAddr at) {
            h->w32(at + gb.SyncBefore, S_CS);
            h->w32(at + gb.SyncAfter, D3D12_BARRIER_SYNC_ALL);
            h->w32(at + gb.AccessBefore, A_UAV);
            h->w32(at + gb.AccessAfter, aa);
        });
    };
    EXPECT_EQ(global_close(DIRECT, D3D12_BARRIER_ACCESS_RENDER_TARGET), S_OK);
    EXPECT_EQ(global_close(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                           D3D12_BARRIER_ACCESS_RENDER_TARGET),
              E_INVALIDARG);
    // An undefined group type.
    EXPECT_EQ(close_after(DIRECT, 3, [](GuestAddr) {}), E_INVALIDARG);
}

// CreateCommittedResource3 / CreatePlacedResource2 take an InitialLayout:
// a buffer is created UNDEFINED only (measured, a07 enhanced_create).
TEST_P(D3D12Test, InitialLayoutCreatorsTakeTheMeasuredLayouts) {
    const bool x64 = GetParam();
    const GuestAddr dev = h->device();
    const auto& hp = layout::D3D12_HEAP_PROPERTIES[x64];
    const auto& rd = layout::D3D12_RESOURCE_DESC1[x64];
    const auto create3 = [&](u32 initial_layout, u32 flags, GuestAddr out) {
        for (u32 i = 0; i < hp.size; i += 4) h->w32(kDesc + i, 0);
        for (u32 i = 0; i < rd.size; i += 4) h->w32(kDesc2 + i, 0);
        h->w32(kDesc + hp.Type, D3D12_HEAP_TYPE_DEFAULT);
        h->w32(kDesc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_BUFFER);
        h->w64(kDesc2 + rd.Width, 256);
        h->w32(kDesc2 + rd.Height, 1);
        EXPECT_TRUE(h->memory.write_u16(kDesc2 + rd.DepthOrArraySize, 1).is_ok());
        EXPECT_TRUE(h->memory.write_u16(kDesc2 + rd.MipLevels, 1).is_ok());
        h->w32(kDesc2 + rd.SampleDesc_Count, 1);
        h->w32(kDesc2 + rd.Layout, D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
        h->w32(kDesc2 + rd.Flags, flags);
        return h->com(dev, DeviceSlot::CreateCommittedResource3,
                      {{kDesc}, {0}, {kDesc2}, {initial_layout}, {0}, {0}, {0},
                       {0}, {h->iid(IID_ID3D12RESOURCE)}, {out}});
    };
    EXPECT_EQ(create3(D3D12_BARRIER_LAYOUT_UNDEFINED, 0, kOut), S_OK);
    EXPECT_NE(h->read_ptr(kOut), 0u);
    h->write_ptr(kOut, 0x1234);
    EXPECT_EQ(create3(D3D12_BARRIER_LAYOUT_COMMON, 0, kOut), E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kOut), 0u);
    EXPECT_EQ(create3(D3D12_BARRIER_LAYOUT_UNDEFINED, 0, 0), S_FALSE);
}

// CreateSharedHandle / OpenSharedHandle of a SHARED fence through the device
// vtable: a new object on one timeline (measured, a04s fence).
TEST_P(D3D12Test, SharedFenceHandleOpensANewObjectOnTheSameTimeline) {
    HandleTable table;
    h->manager->set_handle_table(&table);
    const GuestAddr dev = h->device();
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateFence,
                     {Harness::wide(7), {D3D12_FENCE_FLAG_SHARED},
                      {h->iid(IID_ID3D12FENCE)}, {kOut}}),
              S_OK);
    const GuestAddr fence = h->read_ptr(kOut);
    const GuestAddr plain = h->fence(dev, 0);
    // A non-shared fence, and pHandle NULL: E_INVALIDARG.
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateSharedHandle,
                     {{plain}, {0}, {0x10000000}, {0}, {kOut2}}),
              E_INVALIDARG);
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateSharedHandle,
                     {{fence}, {0}, {0x10000000}, {0}, {0}}),
              E_INVALIDARG);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateSharedHandle,
                     {{fence}, {0}, {0x10000000}, {0}, {kOut2}}),
              S_OK);
    const u64 handle = h->read_ptr(kOut2);
    ASSERT_NE(handle, 0u);
    // The wrong kind is E_NOINTERFACE; ppv NULL E_UNEXPECTED; a bogus
    // handle E_HANDLE.
    EXPECT_EQ(h->com(dev, DeviceSlot::OpenSharedHandle,
                     {{handle}, {h->iid(IID_ID3D12HEAP)}, {kOut}}),
              E_NOINTERFACE);
    EXPECT_EQ(h->com(dev, DeviceSlot::OpenSharedHandle,
                     {{handle}, {h->iid(IID_ID3D12FENCE)}, {0}}),
              E_UNEXPECTED);
    EXPECT_EQ(h->com(dev, DeviceSlot::OpenSharedHandle,
                     {{0x1234}, {h->iid(IID_ID3D12FENCE)}, {kOut}}),
              E_HANDLE);
    ASSERT_EQ(h->com(dev, DeviceSlot::OpenSharedHandle,
                     {{handle}, {h->iid(IID_ID3D12FENCE)}, {kOut}}),
              S_OK);
    const GuestAddr opened = h->read_ptr(kOut);
    EXPECT_NE(opened, fence);
    EXPECT_EQ(h->com(opened, FenceSlot::GetCompletedValue, {}, true), 7u);
    EXPECT_EQ(h->com(fence, FenceSlot::Signal, {Harness::wide(9)}), S_OK);
    EXPECT_EQ(h->com(opened, FenceSlot::GetCompletedValue, {}, true), 9u);
    // A lowering through the opened object is the fence's.
    EXPECT_EQ(h->com(opened, FenceSlot::Signal, {Harness::wide(3)}), S_OK);
    EXPECT_EQ(h->com(fence, FenceSlot::GetCompletedValue, {}, true), 3u);
    EXPECT_EQ(h->com(opened, FenceSlot::GetCreationFlags, {}),
              D3D12_FENCE_FLAG_SHARED);
    // The handle outlives every interface.
    EXPECT_EQ(h->com(fence, FenceSlot::Release, {}), 0u);
    EXPECT_EQ(h->com(opened, FenceSlot::Release, {}), 0u);
    ASSERT_EQ(h->com(dev, DeviceSlot::OpenSharedHandle,
                     {{handle}, {h->iid(IID_ID3D12FENCE)}, {kOut}}),
              S_OK);
    EXPECT_EQ(h->com(h->read_ptr(kOut), FenceSlot::GetCompletedValue, {}, true),
              3u);
}

// The configuration objects and the device's DRED / tools / configuration
// tear-offs, against tools/oracle_probe/d3d12_a01c_config_probe and
// d3d12_a01d_dred_tools_probe (WARP and NVIDIA agree).
TEST_P(D3D12Test, ConfigurationObjectsAnswerAsMeasured) {
    constexpr GuestAddr kClsid = kData + 0x300;
    const auto get_interface = [&](const GuidBytes& clsid, const GuidBytes& iid,
                                   GuestAddr out) {
        EXPECT_TRUE(h->memory.write_bytes(kClsid, clsid.data(), 16).is_ok());
        return h->call("D3D12GetInterface", {{kClsid}, {h->iid(iid)}, {out}});
    };
    // ID3D12SDKConfiguration1: a new object per call.
    ASSERT_EQ(get_interface(CLSID_D3D12SDKCONFIGURATION,
                            IID_ID3D12SDKCONFIGURATION1, kOut),
              S_OK);
    const GuestAddr config = h->read_ptr(kOut);
    ASSERT_EQ(get_interface(CLSID_D3D12SDKCONFIGURATION,
                            IID_ID3D12SDKCONFIGURATION1, kOut2),
              S_OK);
    EXPECT_NE(h->read_ptr(kOut2), config);
    EXPECT_EQ(h->com(h->read_ptr(kOut2), SdkConfigurationSlot::Release, {}), 0u);
    EXPECT_EQ(h->com(config, SdkConfigurationSlot::SetSDKVersion, {{616}, {0}}),
              DXGI_ERROR_INVALID_CALL);
    const GuestAddr path = kData + 0x380;
    EXPECT_TRUE(h->memory.write_bytes(path, reinterpret_cast<const Byte*>(".\\D3D12\\"), 10).is_ok());
    EXPECT_EQ(h->com(config, SdkConfigurationSlot::CreateDeviceFactory,
                     {{617}, {path}, {h->iid(IID_ID3D12DEVICEFACTORY)}, {kOut}}),
              0x887E0003u);  // D3D12_ERROR_INVALID_REDIST
    EXPECT_EQ(h->read_ptr(kOut), 0u);
    EXPECT_EQ(h->com(config, SdkConfigurationSlot::CreateDeviceFactory,
                     {{616}, {path}, {h->iid(IID_ID3D12DEVICE)}, {kOut}}),
              E_NOINTERFACE);
    EXPECT_EQ(h->read_ptr(kOut), 0u);
    EXPECT_EQ(h->com(config, SdkConfigurationSlot::Release, {}), 0u);
    // ID3D12DeviceFactory: flags 0..7, its own DRED state.
    ASSERT_EQ(get_interface(CLSID_D3D12DEVICEFACTORY, IID_ID3D12DEVICEFACTORY,
                            kOut),
              S_OK);
    const GuestAddr factory = h->read_ptr(kOut);
    EXPECT_EQ(h->com(factory, DeviceFactorySlot::SetFlags, {{7}}), S_OK);
    EXPECT_EQ(h->com(factory, DeviceFactorySlot::SetFlags, {{8}}), E_INVALIDARG);
    EXPECT_EQ(h->com(factory, DeviceFactorySlot::GetFlags, {}), 7u);
    EXPECT_EQ(h->com(factory, DeviceFactorySlot::SetFlags, {{0}}), S_OK);
    h->write_ptr(kOut2, 0x1234);
    EXPECT_TRUE(h->memory.write_bytes(kClsid, CLSID_D3D12DEVICEFACTORY.data(),
                                      16).is_ok());
    EXPECT_EQ(h->com(factory, DeviceFactorySlot::GetConfigurationInterface,
                     {{kClsid}, {h->iid(IID_ID3D12DEVICEFACTORY)}, {kOut2}}),
              E_NOINTERFACE);
    EXPECT_EQ(h->read_ptr(kOut2), 0x1234u);
    // The global DRED settings reach D3D12CreateDevice's device.
    ASSERT_EQ(get_interface(CLSID_D3D12DEVICEREMOVEDEXTENDEDDATA,
                            IID_ID3D12DEVICEREMOVEDEXTENDEDDATASETTINGS2, kOut2),
              S_OK);
    const GuestAddr settings = h->read_ptr(kOut2);
    EXPECT_EQ(h->com(settings, DredSettingsSlot::SetAutoBreadcrumbsEnablement,
                     {{D3D12_DRED_ENABLEMENT_FORCED_ON}}),
              0u);
    EXPECT_EQ(h->com(settings, DredSettingsSlot::SetBreadcrumbContextEnablement,
                     {{D3D12_DRED_ENABLEMENT_FORCED_ON}}),
              0u);
    EXPECT_EQ(h->com(settings, DredSettingsSlot::Release, {}), 0u);
    const GuestAddr dev = h->device();
    ASSERT_EQ(h->com(dev, DeviceSlot::QueryInterface,
                     {{h->iid(IID_ID3D12DEVICECONFIGURATION1)}, {kOut}}),
              S_OK);
    const GuestAddr dev_config = h->read_ptr(kOut);
    EXPECT_NE(dev_config, dev);
    EXPECT_EQ(h->com(dev_config, DeviceConfigurationSlot::GetDesc, {{kDesc}}),
              kDesc);
    const auto& cd = layout::D3D12_DEVICE_CONFIGURATION_DESC[GetParam()];
    EXPECT_EQ(h->r32(kDesc + cd.Flags), 0x40000048u);
    EXPECT_EQ(h->r32(kDesc + cd.SDKVersion), 616u);
    // QI(IUnknown) through the tear-off is the device's identity.
    ASSERT_EQ(h->com(dev_config, DeviceConfigurationSlot::QueryInterface,
                     {{h->iid(IID_IUNKNOWN)}, {kOut2}}),
              S_OK);
    EXPECT_EQ(h->read_ptr(kOut2), dev);
    (void)h->com(dev, DeviceSlot::Release, {});
    // A factory device has the factory's (empty) DRED state.
    ASSERT_EQ(h->com(factory, DeviceFactorySlot::CreateDevice,
                     {{0}, {D3D_FEATURE_LEVEL_11_0},
                      {h->iid(IID_ID3D12DEVICECONFIGURATION1)}, {kOut2}}),
              S_OK);
    const GuestAddr factory_dev_config = h->read_ptr(kOut2);
    EXPECT_EQ(h->com(factory_dev_config, DeviceConfigurationSlot::GetDesc,
                     {{kDesc}}),
              kDesc);
    EXPECT_EQ(h->r32(kDesc + cd.Flags), 0x40000000u);
    EXPECT_EQ(h->com(factory_dev_config, DeviceConfigurationSlot::Release, {}),
              0u);
    // DRED: NOT_CURRENTLY_AVAILABLE while live, page faults UNSUPPORTED
    // (not enabled), the empty answer once removed.
    ASSERT_EQ(h->com(dev, DeviceSlot::QueryInterface,
                     {{h->iid(IID_ID3D12DEVICEREMOVEDEXTENDEDDATA2)}, {kOut}}),
              S_OK);
    const GuestAddr dred = h->read_ptr(kOut);
    EXPECT_EQ(h->com(dred, DredSlot::GetAutoBreadcrumbsOutput1, {{kDesc2}}),
              DXGI_ERROR_NOT_CURRENTLY_AVAILABLE);
    EXPECT_EQ(h->com(dred, DredSlot::GetPageFaultAllocationOutput2, {{kDesc2}}),
              DXGI_ERROR_UNSUPPORTED);
    EXPECT_EQ(h->com(dev, DeviceSlot::RemoveDevice, {}), 0u);
    h->write_ptr(kDesc2, 0xCDCD);
    EXPECT_EQ(h->com(dred, DredSlot::GetAutoBreadcrumbsOutput1, {{kDesc2}}),
              S_OK);
    EXPECT_EQ(h->read_ptr(kDesc2), 0u);
    EXPECT_EQ(h->com(dred, DredSlot::GetPageFaultAllocationOutput2, {{kDesc2}}),
              DXGI_ERROR_UNSUPPORTED);
    EXPECT_EQ(h->com(dred, DredSlot::GetDeviceState, {}),
              D3D12_DRED_DEVICE_STATE_UNKNOWN);
    EXPECT_EQ(h->com(factory, DeviceFactorySlot::Release, {}), 0u);
}

TEST_P(D3D12Test, DeviceToolsAndCompatibilityDeviceAnswerAsMeasured) {
    const GuestAddr dev = h->device();
    ASSERT_EQ(h->com(dev, DeviceSlot::QueryInterface,
                     {{h->iid(IID_ID3D12DEVICETOOLS1)}, {kOut}}),
              S_OK);
    const GuestAddr tools = h->read_ptr(kOut);
    EXPECT_EQ(h->com(tools, DeviceToolsSlot::GetApplicationSpecificDriverBlobStatus,
                     {}),
              D3D12_APPLICATION_SPECIFIC_DRIVER_BLOB_UNKNOWN);
    h->write_ptr(kOut2, 0x1234);
    EXPECT_EQ(h->com(tools, DeviceToolsSlot::GetApplicationSpecificDriverState,
                     {{kOut2}}),
              E_FAIL);
    EXPECT_EQ(h->read_ptr(kOut2), 0x1234u);
    // A pending next-allocation address refuses committed buffers until 0.
    EXPECT_EQ(h->com(tools, DeviceToolsSlot::SetNextAllocationAddress,
                     {Harness::wide(0x100000000ull)}),
              0u);
    GuestAddr buffer = 0;
    EXPECT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 65536,
                        D3D12_RESOURCE_STATE_COMMON, buffer),
              E_INVALIDARG);
    EXPECT_EQ(h->com(tools, DeviceToolsSlot::SetNextAllocationAddress,
                     {Harness::wide(0)}),
              0u);
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 65536,
                        D3D12_RESOURCE_STATE_COMMON, buffer),
              S_OK);
    // ID3D12PageableTools::GetAllocation is E_NOTIMPL on Windows.
    ASSERT_EQ(h->com(buffer, ResourceSlot::QueryInterface,
                     {{h->iid(IID_ID3D12PAGEABLETOOLS)}, {kOut}}),
              S_OK);
    EXPECT_EQ(h->com(h->read_ptr(kOut), PageableToolsSlot::GetAllocation,
                     {{kDesc2}}),
              E_NOTIMPL);
    // CreateSharedHeap: SHARED needed, flags 0..7; Reflect gives them back.
    ASSERT_EQ(h->com(dev, DeviceSlot::QueryInterface,
                     {{h->iid(IID_ID3D12COMPATIBILITYDEVICE)}, {kOut}}),
              S_OK);
    const GuestAddr compat = h->read_ptr(kOut);
    const auto& hd = layout::D3D12_HEAP_DESC[GetParam()];
    for (u32 i = 0; i < hd.size; i += 4) h->w32(kDesc + i, 0);
    h->w64(kDesc + hd.SizeInBytes, 65536);
    h->w32(kDesc + hd.Properties_Type, D3D12_HEAP_TYPE_DEFAULT);
    EXPECT_EQ(h->com(compat, CompatibilityDeviceSlot::CreateSharedHeap,
                     {{kDesc}, {0}, {h->iid(IID_ID3D12HEAP)}, {kOut}}),
              E_INVALIDARG);
    h->w32(kDesc + hd.Flags, D3D12_HEAP_FLAG_SHARED);
    EXPECT_EQ(h->com(compat, CompatibilityDeviceSlot::CreateSharedHeap,
                     {{kDesc}, {8}, {h->iid(IID_ID3D12HEAP)}, {kOut}}),
              E_INVALIDARG);
    ASSERT_EQ(h->com(compat, CompatibilityDeviceSlot::CreateSharedHeap,
                     {{kDesc}, {D3D12_COMPATIBILITY_SHARED_FLAG_9_ON_12},
                      {h->iid(IID_ID3D12HEAP)}, {kOut}}),
              S_OK);
    const GuestAddr heap = h->read_ptr(kOut);
    h->w32(kDesc2, 0xCDCDCDCD);
    EXPECT_EQ(h->com(compat, CompatibilityDeviceSlot::ReflectSharedProperties,
                     {{heap},
                      {D3D12_REFELCT_SHARED_PROPERTY_COMPATIBILITY_SHARED_FLAGS},
                      {kDesc2}, {4}}),
              S_OK);
    EXPECT_EQ(h->r32(kDesc2), D3D12_COMPATIBILITY_SHARED_FLAG_9_ON_12);
    EXPECT_EQ(h->com(compat, CompatibilityDeviceSlot::ReflectSharedProperties,
                     {{heap},
                      {D3D12_REFELCT_SHARED_PROPERTY_COMPATIBILITY_SHARED_FLAGS},
                      {kDesc2}, {2}}),
              E_INVALIDARG);
    EXPECT_EQ(h->com(compat, CompatibilityDeviceSlot::ReflectSharedProperties,
                     {{heap}, {D3D12_REFLECT_SHARED_PROPERTY_D3D11_RESOURCE_FLAGS},
                      {kDesc2}, {16}}),
              E_INVALIDARG);
    EXPECT_EQ(h->com(compat, CompatibilityDeviceSlot::ReflectSharedProperties,
                     {{buffer},
                      {D3D12_REFELCT_SHARED_PROPERTY_COMPATIBILITY_SHARED_FLAGS},
                      {kDesc2}, {4}}),
              E_INVALIDARG);  // not shared
}

// CopyBufferRegion replays at ExecuteCommandLists, into the backend, between
// windows of the heaps' buffers: UPLOAD -> DEFAULT -> READBACK round trip.
TEST_P(D3D12Test, CopyBufferRegionRoundTripsThroughTheQueue) {
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    const GuestAddr alloc = h->allocator(dev);
    GuestAddr upload = 0, gpu = 0, readback = 0, list = 0;
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 256,
                        D3D12_RESOURCE_STATE_GENERIC_READ, upload), S_OK);
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 256,
                        D3D12_RESOURCE_STATE_COMMON, gpu), S_OK);
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_READBACK, 256,
                        D3D12_RESOURCE_STATE_COPY_DEST, readback), S_OK);
    ASSERT_EQ(h->com(upload, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr src = h->read_ptr(kOut2);
    for (u32 i = 0; i < 16; ++i) h->w32(src + i * 4, 0xC0DE0000u + i);
    ASSERT_EQ(h->try_list(dev, alloc, list), S_OK);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{gpu}, Harness::wide(16), {upload}, Harness::wide(0),
                  Harness::wide(64)});
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{readback}, Harness::wide(0), {gpu}, Harness::wide(16),
                  Harness::wide(64)});
    ASSERT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    h->write_ptr(kArray, list);
    // Nothing reaches the backend until the list executes.
    EXPECT_TRUE(h->backend.stream.empty());
    (void)h->com(queue, CommandQueueSlot::ExecuteCommandLists,
                 {{1}, {kArray}});
    EXPECT_EQ(h->backend.stream,
              (std::vector<std::string>{"copy 64", "copy 64"}));
    ASSERT_EQ(h->com(readback, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr dst = h->read_ptr(kOut2);
    for (u32 i = 0; i < 16; ++i) {
        EXPECT_EQ(h->r32(dst + i * 4), 0xC0DE0000u + i) << i;
    }
}

// =============================================================================
// DXGI swap chains on a command queue
// =============================================================================

constexpr u64 kHwnd = 0x10044;
constexpr GuestAddr kChainDesc = kData + 0x1000;

/// Window services for one 120x48 window (the a08 probe's client area).
void attach_window(Harness& h) {
    D3D12Manager::PresentationServices services;
    services.presentation_view = [](u64 hwnd) -> void* {
        return hwnd == kHwnd ? reinterpret_cast<void*>(0x1) : nullptr;
    };
    services.client_size = [](u64 hwnd) -> std::optional<std::pair<u32, u32>> {
        if (hwnd != kHwnd) return std::nullopt;
        return std::pair<u32, u32>{120, 48};
    };
    h.manager->set_presentation_services(std::move(services));
}

/// DXGI_SWAP_CHAIN_DESC1 (48 bytes, no pointers).
void write_chain_desc(Harness& h, u32 width, u32 height, u32 count,
                      u32 flags = 0, u32 effect = 4 /* FLIP_DISCARD */,
                      u32 alpha = 0) {
    const u32 values[12] = {width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0,
                            1,     0,      0x20 /* RENDER_TARGET_OUTPUT */,
                            count, 0 /* STRETCH */, effect, alpha, flags};
    for (u32 i = 0; i < 12; ++i) h.w32(kChainDesc + i * 4u, values[i]);
}

u64 create_chain(Harness& h, GuestAddr device_or_queue, GuestAddr& out) {
    h.write_ptr(kOut, 0xBAD0BAD0u);
    const u64 hr = h.call_manager(&D3D12Manager::impl_CreateSwapChainForHwnd,
                                  {{0}, {device_or_queue}, {kHwnd},
                                   {kChainDesc}, {0}, {0}, {kOut}});
    out = h.read_ptr(kOut);
    return hr;
}

// a08 create / buffers: a flip chain on a direct queue; the buffers are
// committed ID3D12Resource textures (flags ALLOW_RENDER_TARGET, heap flags
// 0x8d); a device is not a queue; one flip chain per window.
TEST_P(D3D12Test, SwapChainOnAQueueServesItsBuffersAsResources) {
    attach_window(*h);
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    write_chain_desc(*h, 0, 0, 3);
    GuestAddr chain = 0;
    EXPECT_EQ(create_chain(*h, dev, chain), DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(chain, 0u);
    ASSERT_EQ(create_chain(*h, queue, chain), S_OK);
    ASSERT_NE(chain, 0u);
    GuestAddr second = 0;
    EXPECT_EQ(create_chain(*h, queue, second), E_ACCESSDENIED);
    EXPECT_EQ(second, 0u);
    // 0x0 takes the client area (size==client=1).
    ASSERT_EQ(h->com(chain, SwapChainSlot::GetDesc1, {{kDesc}}), S_OK);
    EXPECT_EQ(h->r32(kDesc + 0), 120u);
    EXPECT_EQ(h->r32(kDesc + 4), 48u);
    EXPECT_EQ(h->r32(kDesc + 28), 3u);
    std::array<GuestAddr, 3> buffers{};
    for (u32 i = 0; i < 3; ++i) {
        ASSERT_EQ(h->com(chain, SwapChainSlot::GetBuffer,
                         {{i}, {h->iid(IID_ID3D12RESOURCE)}, {kOut}}),
                  S_OK);
        buffers[i] = h->read_ptr(kOut);
        ASSERT_NE(buffers[i], 0u);
    }
    EXPECT_NE(buffers[0], buffers[1]);
    EXPECT_NE(buffers[1], buffers[2]);
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetBuffer,
                     {{3}, {h->iid(IID_ID3D12RESOURCE)}, {kOut}}),
              DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(h->read_ptr(kOut), 0u);
    const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
    (void)h->com(buffers[0], ResourceSlot::GetDesc, {{kDesc}});
    EXPECT_EQ(h->r32(kDesc + rd.Dimension), D3D12_RESOURCE_DIMENSION_TEXTURE2D);
    EXPECT_EQ(h->r64(kDesc + rd.Width), 120u);
    EXPECT_EQ(h->r32(kDesc + rd.Format),
              static_cast<u32>(DXGI_FORMAT_R8G8B8A8_UNORM));
    EXPECT_EQ(h->r32(kDesc + rd.Flags), 0x1u);
    const auto& hp = layout::D3D12_HEAP_PROPERTIES[GetParam()];
    ASSERT_EQ(h->com(buffers[0], ResourceSlot::GetHeapProperties,
                     {{kDesc}, {kOut2}}),
              S_OK);
    EXPECT_EQ(h->r32(kDesc + hp.Type), D3D12_HEAP_TYPE_DEFAULT);
    EXPECT_EQ(h->r32(kOut2), 0x8Du);
    // GetDevice answers the queue's device, not the queue.
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetDevice,
                     {{h->iid(IID_ID3D12DEVICE)}, {kOut}}),
              S_OK);
    EXPECT_EQ(h->read_ptr(kOut), dev);
    (void)h->com(dev, DeviceSlot::Release, {});
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetDevice,
                     {{h->iid(IID_ID3D12COMMANDQUEUE)}, {kOut}}),
              E_NOINTERFACE);
    // Each GetBuffer reference is the application's; the chain keeps one.
    for (const GuestAddr buffer : buffers) {
        EXPECT_EQ(h->com(buffer, ResourceSlot::Release, {}), 1u);
    }
    EXPECT_EQ(h->com(chain, SwapChainSlot::Release, {}), 0u);
}

// a08 present: the index walks the ring; TEST moves nothing; a present held
// behind a queue wait is shown after the work that releases it.
TEST_P(D3D12Test, PresentRotatesTheBackBufferInQueueOrder) {
    attach_window(*h);
    const GuestAddr dev = h->device();
    const GuestAddr direct = h->queue(dev);
    const GuestAddr copy = h->queue(dev, D3D12_COMMAND_LIST_TYPE_COPY);
    const GuestAddr fence = h->fence(dev, 0);
    write_chain_desc(*h, 64, 48, 2);
    GuestAddr chain = 0;
    ASSERT_EQ(create_chain(*h, direct, chain), S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetCurrentBackBufferIndex, {}), 0u);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0}}), S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetCurrentBackBufferIndex, {}), 1u);
    EXPECT_EQ(h->backend.stream,
              (std::vector<std::string>{"present 64x48"}));
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0x1 /* TEST */}}),
              S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetCurrentBackBufferIndex, {}), 1u);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{5}, {0}}),
              DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present,
                     {{0}, {0x200 /* ALLOW_TEARING */}}),
              DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0x80000000u}}),
              DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(h->backend.stream.size(), 1u);
    // Held behind a wait no signal has been submitted for.
    EXPECT_EQ(h->com(direct, CommandQueueSlot::Wait,
                     {{fence}, Harness::wide(5)}),
              S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{1}, {0}}), S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetCurrentBackBufferIndex, {}), 0u);
    EXPECT_EQ(h->backend.stream.size(), 1u);
    EXPECT_EQ(h->com(copy, CommandQueueSlot::Signal,
                     {{fence}, Harness::wide(5)}),
              S_OK);
    EXPECT_EQ(h->backend.stream,
              (std::vector<std::string>{"present 64x48", "signal 5", "wait 5",
                                        "present 64x48"}));
    ASSERT_EQ(h->com(chain, SwapChainSlot::GetLastPresentCount, {{kOut2}}),
              S_OK);
    EXPECT_EQ(h->r32(kOut2), 2u);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Release, {}), 0u);
}

// =============================================================================
// Exclusive full-screen (tools/oracle_probe/d3d12_a73_swapchain_fullscreen_probe,
// .winoracle-gpu.expected: RTX 4080 in the desktop user's session, x64 and x86
// identical)
// =============================================================================

/// One 2560x1440 output and a window system that records what the shared
/// exclusive display-mode owner and the host Space are asked for.
struct FullscreenWorld {
    struct Enter {
        u64 hwnd;
        std::array<i32, 4> output;
        u32 width, height;
        bool change_mode;
        bool operator==(const Enter&) const = default;
    };
    std::vector<Enter> enters;
    std::vector<u64> leaves;
    std::vector<std::pair<u64, bool>> host;
    bool app_active{true};
    bool focused{true};
    bool occluded{true};
    u32 client_w{120}, client_h{48};
    std::function<void()> on_enter;
    std::unique_ptr<DxgiManager> dxgi;
};

constexpr u64 kHwnd2 = 0x10088;

void attach_fullscreen_window(Harness& h, FullscreenWorld& w) {
    w.dxgi = std::make_unique<DxgiManager>(h.memory, h.ptr_size() == 8);
    DxgiManager::MethodThunks thunks{};
    static_assert(sizeof(DxgiManager::MethodThunks) % sizeof(GuestAddr) == 0);
    auto* fields = reinterpret_cast<GuestAddr*>(&thunks);
    for (usize i = 0; i < sizeof(thunks) / sizeof(GuestAddr); ++i) {
        fields[i] = 0x72000000 + i * 0x10;
    }
    w.dxgi->set_method_thunks(thunks);
    w.dxgi->set_output_resolver([] {
        dxgi::OutputInfo info{
            .device_name = "\\\\.\\DISPLAY1",
            .left = 0, .top = 0, .right = 2560, .bottom = 1440,
            .monitor = 0xD0001,
            .bits_per_color = 8,
            .refresh_rate_hz = 60.0,
        };
        return std::vector<dxgi::OutputInfo>{info};
    });
    D3D12Manager::PresentationServices services;
    services.presentation_view = [](u64 hwnd) -> void* {
        return hwnd == kHwnd || hwnd == kHwnd2 ? reinterpret_cast<void*>(0x1)
                                               : nullptr;
    };
    services.client_size = [&w](u64 hwnd) -> std::optional<std::pair<u32, u32>> {
        if (hwnd != kHwnd && hwnd != kHwnd2) return std::nullopt;
        return std::pair<u32, u32>{w.client_w, w.client_h};
    };
    services.window_rect = [](u64) -> std::optional<std::array<i32, 4>> {
        return std::array<i32, 4>{40, 40, 680, 400};
    };
    services.dxgi = w.dxgi.get();
    services.exclusive.client_size = services.client_size;
    services.exclusive.window_rect = services.window_rect;
    services.exclusive.enter_exclusive = [&w](u64 hwnd, std::array<i32, 4> output,
                                              u32 width, u32 height,
                                              bool change_mode) {
        w.enters.push_back({hwnd, output, width, height, change_mode});
        w.client_w = width;
        w.client_h = height;
        if (w.on_enter) w.on_enter();
        return true;
    };
    services.exclusive.leave_exclusive =
        [&w](u64 hwnd, bool, std::optional<std::array<i32, 4>>) {
            w.leaves.push_back(hwnd);
            w.client_w = 640;
            w.client_h = 360;
        };
    services.exclusive.host_fullscreen = [&w](u64 hwnd, bool enable) {
        w.host.emplace_back(hwnd, enable);
    };
    services.exclusive.application_active = [&w] { return w.app_active; };
    services.exclusive.window_occluded = [&w](u64) { return w.occluded; };
    services.exclusive.has_keyboard_focus = [&w](u64) {
        return w.app_active && w.focused;
    };
    h.manager->set_presentation_services(std::move(services));
}

/// GetFullscreenState(&fs, &target): {fs, target}.
std::pair<u32, u64> fullscreen_state(Harness& h, GuestAddr chain) {
    h.w32(kOut2, 7);
    h.write_ptr(kOut, 0x1111);
    EXPECT_EQ(h.com(chain, SwapChainSlot::GetFullscreenState, {{kOut2}, {kOut}}),
              S_OK);
    return {h.r32(kOut2), h.read_ptr(kOut)};
}

/// GetDesc's Windowed and GetFullscreenDesc's Windowed.
std::pair<u32, u32> windowed_fields(Harness& h, GuestAddr chain) {
    EXPECT_EQ(h.com(chain, SwapChainSlot::GetDesc, {{kDesc}}), S_OK);
    const u32 desc = h.r32(kDesc + (h.ptr_size() == 8 ? 56u : 48u));
    EXPECT_EQ(h.com(chain, SwapChainSlot::GetFullscreenDesc, {{kDesc}}), S_OK);
    return {desc, h.r32(kDesc + 16)};
}

constexpr u32 kWaitableTearing = 0x840;  // FRAME_LATENCY_WAITABLE | ALLOW_TEARING
constexpr GuestAddr kModeDesc = kData + 0x1300;

// a73 m12_* / rules12 / present12, the UE swap chain (FLIP_DISCARD x3,
// FRAME_LATENCY_WAITABLE_OBJECT | ALLOW_TEARING): FALSE on a windowed chain is
// S_OK; TRUE enters full-screen in the desktop mode (no ALLOW_MODE_SWITCH)
// through the shared owner and a host Space; GetFullscreenState, GetDesc and
// GetFullscreenDesc agree; Present (TEST included) is INVALID_CALL until
// ResizeBuffers, ALLOW_TEARING stays refused in full-screen; a repeated TRUE
// is S_OK and no transition; FALSE with a pTarget is INVALID_CALL while
// full-screen; ResizeTarget keeps the desktop mode, NULL is INVALID_CALL;
// FALSE leaves the Space and again asks for ResizeBuffers.
TEST_P(D3D12Test, WaitableTearingChainGoesFullscreenAsMeasured) {
    FullscreenWorld w;
    attach_fullscreen_window(*h, w);
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    write_chain_desc(*h, 2560, 1440, 3, kWaitableTearing);
    GuestAddr chain = 0;
    ASSERT_EQ(create_chain(*h, queue, chain), S_OK);
    EXPECT_EQ(fullscreen_state(*h, chain), (std::pair<u32, u64>{0, 0}));
    EXPECT_EQ(windowed_fields(*h, chain), (std::pair<u32, u32>{1, 1}));
    EXPECT_EQ(h->com(chain, SwapChainSlot::SetFullscreenState, {{0}, {0}}), S_OK);
    EXPECT_TRUE(w.host.empty());

    ASSERT_EQ(h->com(chain, SwapChainSlot::SetFullscreenState, {{1}, {0}}), S_OK);
    EXPECT_EQ(w.enters, (std::vector<FullscreenWorld::Enter>{
                            {kHwnd, {0, 0, 2560, 1440}, 2560, 1440, false}}));
    EXPECT_EQ(w.host, (std::vector<std::pair<u64, bool>>{{kHwnd, true}}));
    const auto [fs, output] = fullscreen_state(*h, chain);
    EXPECT_EQ(fs, 1u);
    ASSERT_NE(output, 0u);
    EXPECT_TRUE(w.dxgi->is_output_object(output));
    w.dxgi->release_dxgi_object(output);
    EXPECT_EQ(windowed_fields(*h, chain), (std::pair<u32, u32>{0, 0}));
    // GetFullscreenState's out-parameters are both optional.
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetFullscreenState, {{0}, {0}}), S_OK);

    const usize shown = h->backend.stream.size();
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0}}),
              DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0x1 /* TEST */}}),
              DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(h->backend.stream.size(), shown);
    EXPECT_EQ(h->com(chain, SwapChainSlot::ResizeBuffers,
                     {{0}, {0}, {0}, {0}, {kWaitableTearing}}),
              S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0}}), S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{1}, {0}}), S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present,
                     {{0}, {0x200 /* ALLOW_TEARING */}}),
              DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0x1 /* TEST */}}),
              S_OK);

    EXPECT_EQ(h->com(chain, SwapChainSlot::SetFullscreenState, {{1}, {0}}), S_OK);
    EXPECT_EQ(w.enters.size(), 1u) << "TRUE again is no transition";
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0}}), S_OK);
    const GuestAddr adapter = w.dxgi->create_default_adapter();
    const GuestAddr target =
        w.dxgi->create_containing_output(adapter, 0, 0, 10, 10);
    w.dxgi->release_adapter(adapter);
    ASSERT_NE(target, 0u);
    EXPECT_EQ(h->com(chain, SwapChainSlot::SetFullscreenState, {{0}, {target}}),
              DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(fullscreen_state(*h, chain).first, 1u);
    w.dxgi->release_dxgi_object(h->read_ptr(kOut));

    h->w32(kModeDesc + 0, 1280);
    h->w32(kModeDesc + 4, 720);
    EXPECT_EQ(h->com(chain, SwapChainSlot::ResizeTarget, {{kModeDesc}}), S_OK);
    EXPECT_EQ(w.enters.size(), 1u) << "no ALLOW_MODE_SWITCH: the desktop mode";
    EXPECT_EQ(h->com(chain, SwapChainSlot::ResizeTarget, {{0}}),
              DXGI_ERROR_INVALID_CALL);

    ASSERT_EQ(h->com(chain, SwapChainSlot::SetFullscreenState, {{0}, {0}}), S_OK);
    EXPECT_EQ(w.leaves, (std::vector<u64>{kHwnd}));
    EXPECT_EQ(w.host.back(), (std::pair<u64, bool>{kHwnd, false}));
    EXPECT_EQ(fullscreen_state(*h, chain), (std::pair<u32, u64>{0, 0}));
    EXPECT_EQ(windowed_fields(*h, chain), (std::pair<u32, u32>{1, 1}));
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0}}),
              DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(h->com(chain, SwapChainSlot::ResizeBuffers,
                     {{0}, {0}, {0}, {0}, {kWaitableTearing}}),
              S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0}}), S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present,
                     {{0}, {0x200 /* ALLOW_TEARING */}}),
              S_OK);
    // Windowed with a pTarget is accepted (a73 WARP rules12 "FALSE(output0)").
    EXPECT_EQ(h->com(chain, SwapChainSlot::SetFullscreenState, {{0}, {target}}),
              S_OK);
    w.dxgi->release_dxgi_object(target);
    // TRUE then FALSE with no ResizeBuffers between: the buffers are the
    // windowed ones again (a73 present12 resize=0: windowed Present S_OK).
    ASSERT_EQ(h->com(chain, SwapChainSlot::SetFullscreenState, {{1}, {0}}), S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{1}, {0}}),
              DXGI_ERROR_INVALID_CALL);
    ASSERT_EQ(h->com(chain, SwapChainSlot::SetFullscreenState, {{0}, {0}}), S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{1}, {0}}), S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Release, {}), 0u);
    h->manager->set_presentation_services({});
}

// a73: a window that is not foreground cannot go full-screen
// (NOT_CURRENTLY_AVAILABLE); pTarget must be an output, and GetFullscreenState
// then hands back that very object ("passed=1"); a second chain cannot take
// the display mode one holds; another application in the foreground leaves
// the chain full-screen (focus12: GetFullscreenState TRUE) with Present
// DXGI_STATUS_OCCLUDED and DXGI_PRESENT_TEST INVALID_CALL, and a TRUE from
// the background S_OK; back in the foreground Present is S_OK; the last
// Release of a full-screen chain still restores the window and leaves the
// Space.
TEST_P(D3D12Test, FullscreenRefusalsTargetsAndFocusLossAsMeasured) {
    FullscreenWorld w;
    attach_fullscreen_window(*h, w);
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    write_chain_desc(*h, 2560, 1440, 3, kWaitableTearing);
    GuestAddr chain = 0;
    ASSERT_EQ(create_chain(*h, queue, chain), S_OK);
    w.focused = false;
    EXPECT_EQ(h->com(chain, SwapChainSlot::SetFullscreenState, {{1}, {0}}),
              DXGI_ERROR_NOT_CURRENTLY_AVAILABLE);
    EXPECT_TRUE(w.enters.empty());
    EXPECT_TRUE(w.host.empty());
    w.focused = true;
    EXPECT_EQ(h->com(chain, SwapChainSlot::SetFullscreenState, {{1}, {chain}}),
              DXGI_ERROR_INVALID_CALL);
    EXPECT_TRUE(w.enters.empty());

    const GuestAddr adapter = w.dxgi->create_default_adapter();
    const GuestAddr target =
        w.dxgi->create_containing_output(adapter, 0, 0, 10, 10);
    w.dxgi->release_adapter(adapter);
    ASSERT_NE(target, 0u);
    ASSERT_EQ(h->com(chain, SwapChainSlot::SetFullscreenState, {{1}, {target}}),
              S_OK);
    EXPECT_EQ(fullscreen_state(*h, chain), (std::pair<u32, u64>{1, target}));
    w.dxgi->release_dxgi_object(target);

    // One chain of the process holds the display mode.
    h->write_ptr(kOut, 0);
    ASSERT_EQ(h->call_manager(&D3D12Manager::impl_CreateSwapChainForHwnd,
                              {{0}, {queue}, {kHwnd2}, {kChainDesc}, {0}, {0},
                               {kOut}}),
              S_OK);
    const GuestAddr second = h->read_ptr(kOut);
    EXPECT_EQ(h->com(second, SwapChainSlot::SetFullscreenState, {{1}, {0}}),
              DXGI_ERROR_NOT_CURRENTLY_AVAILABLE);
    EXPECT_EQ(h->com(second, SwapChainSlot::Release, {}), 0u);

    EXPECT_EQ(h->com(chain, SwapChainSlot::ResizeBuffers,
                     {{0}, {0}, {0}, {0}, {kWaitableTearing}}),
              S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0}}), S_OK);
    // Another application is foreground; AppKit reports the window hidden.
    w.app_active = false;
    EXPECT_EQ(fullscreen_state(*h, chain).first, 1u);
    w.dxgi->release_dxgi_object(h->read_ptr(kOut));
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0}}),
              DXGI_STATUS_OCCLUDED);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0x1 /* TEST */}}),
              DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(h->com(chain, SwapChainSlot::SetFullscreenState, {{1}, {0}}), S_OK);
    EXPECT_EQ(w.enters.size(), 1u);
    EXPECT_TRUE(w.leaves.empty());
    EXPECT_EQ(w.host, (std::vector<std::pair<u64, bool>>{{kHwnd, true}}))
        << "the Space is kept";
    // The host shows the window again: Present displays before the
    // activation reaches the guest.
    w.occluded = false;
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0}}), S_OK);
    w.app_active = true;
    w.occluded = true;
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0}}), S_OK);

    EXPECT_EQ(h->com(chain, SwapChainSlot::Release, {}), 0u);
    EXPECT_EQ(w.leaves, (std::vector<u64>{kHwnd}));
    EXPECT_EQ(w.host.back(), (std::pair<u64, bool>{kHwnd, false}));
    EXPECT_EQ(w.dxgi->exclusive_fullscreen().owner.load(), 0u);
    w.dxgi->release_dxgi_object(target);
    h->manager->set_presentation_services({});
}

// a73 rules12: Windowed=FALSE creation (CreateSwapChainForHwnd with a
// DXGI_SWAP_CHAIN_FULLSCREEN_DESC) is S_OK and full-screen when the window
// has the focus, DXGI_STATUS_OCCLUDED and windowed when it has not (WARP, a
// disconnected session). A SetFullscreenState re-entered from the
// transition's own window messages is DXGI_STATUS_MODE_CHANGE_IN_PROGRESS
// (Learn), as is ResizeTarget.
TEST_P(D3D12Test, WindowedFalseCreationAndReentryDuringTheTransition) {
    FullscreenWorld w;
    attach_fullscreen_window(*h, w);
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    write_chain_desc(*h, 2560, 1440, 3, kWaitableTearing);
    constexpr GuestAddr kFullscreenDesc = kData + 0x1340;
    const u32 fullscreen_desc[5] = {60, 1, 0, 0, 0 /* Windowed */};
    for (u32 i = 0; i < 5; ++i) h->w32(kFullscreenDesc + i * 4u, fullscreen_desc[i]);
    const auto create_fullscreen = [&](GuestAddr& out) {
        h->write_ptr(kOut, 0);
        const u64 hr = h->call_manager(&D3D12Manager::impl_CreateSwapChainForHwnd,
                                       {{0}, {queue}, {kHwnd}, {kChainDesc},
                                        {kFullscreenDesc}, {0}, {kOut}});
        out = h->read_ptr(kOut);
        return hr;
    };
    GuestAddr chain = 0;
    ASSERT_EQ(create_fullscreen(chain), S_OK);
    EXPECT_EQ(fullscreen_state(*h, chain).first, 1u);
    w.dxgi->release_dxgi_object(h->read_ptr(kOut));
    EXPECT_EQ(windowed_fields(*h, chain), (std::pair<u32, u32>{0, 0}));
    EXPECT_EQ(w.enters.size(), 1u);
    // Created full-screen, its buffers are the full-screen ones.
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{1}, {0}}), S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::SetFullscreenState, {{0}, {0}}), S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Release, {}), 0u);

    w.focused = false;
    ASSERT_EQ(create_fullscreen(chain), DXGI_STATUS_OCCLUDED);
    ASSERT_NE(chain, 0u);
    EXPECT_EQ(fullscreen_state(*h, chain), (std::pair<u32, u64>{0, 0}));
    EXPECT_EQ(windowed_fields(*h, chain), (std::pair<u32, u32>{1, 1}));
    w.focused = true;

    std::vector<u64> inner;
    w.on_enter = [&] {
        inner.push_back(h->com(chain, SwapChainSlot::SetFullscreenState, {{0}, {0}}));
        h->w32(kModeDesc + 0, 1280);
        h->w32(kModeDesc + 4, 720);
        inner.push_back(h->com(chain, SwapChainSlot::ResizeTarget, {{kModeDesc}}));
    };
    EXPECT_EQ(h->com(chain, SwapChainSlot::SetFullscreenState, {{1}, {0}}), S_OK);
    w.on_enter = nullptr;
    EXPECT_EQ(inner, (std::vector<u64>{DXGI_STATUS_MODE_CHANGE_IN_PROGRESS,
                                       DXGI_STATUS_MODE_CHANGE_IN_PROGRESS}));
    EXPECT_EQ(fullscreen_state(*h, chain).first, 1u);
    w.dxgi->release_dxgi_object(h->read_ptr(kOut));
    EXPECT_EQ(h->com(chain, SwapChainSlot::SetFullscreenState, {{0}, {0}}), S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Release, {}), 0u);
    h->manager->set_presentation_services({});
}

// The DXGI_PRESENT flags stay in the frontend: ALLOW_TEARING (a tearing
// chain, SyncInterval 0), DO_NOT_WAIT and RESTART frames reach the window
// like any other. The presenter refuses flags (BackendSwapChain contract);
// passing them through dropped each such frame while Present said S_OK.
TEST_P(D3D12Test, PresentFlagsStayInTheFrontend) {
    attach_window(*h);
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    write_chain_desc(*h, 64, 48, 2, 0x800 /* ALLOW_TEARING */);
    GuestAddr chain = 0;
    ASSERT_EQ(create_chain(*h, queue, chain), S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present,
                     {{0}, {0x200 /* ALLOW_TEARING */}}),
              S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{1}, {0x4 /* RESTART */}}),
              S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0x8 /* DO_NOT_WAIT */}}),
              S_OK);
    EXPECT_EQ(h->backend.stream,
              (std::vector<std::string>{"present 64x48", "present 64x48",
                                        "present 64x48"}));
    EXPECT_EQ(h->com(chain, SwapChainSlot::Release, {}), 0u);
}

// a08 resize: every buffer reference must be released first; zero keeps the
// count and format, 0x0 takes the client area; one buffer, or adding
// ALLOW_TEARING, is E_INVALIDARG. The chain's queue reference goes with it.
TEST_P(D3D12Test, ResizeBuffersNeedsEveryBufferReferenceReleased) {
    attach_window(*h);
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    const u64 queue_refs = h->com(queue, CommandQueueSlot::AddRef, {});
    (void)h->com(queue, CommandQueueSlot::Release, {});
    write_chain_desc(*h, 64, 48, 2);
    GuestAddr chain = 0;
    ASSERT_EQ(create_chain(*h, queue, chain), S_OK);
    EXPECT_EQ(h->com(queue, CommandQueueSlot::AddRef, {}), queue_refs + 1);
    (void)h->com(queue, CommandQueueSlot::Release, {});
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0}}), S_OK);
    ASSERT_EQ(h->com(chain, SwapChainSlot::GetBuffer,
                     {{0}, {h->iid(IID_ID3D12RESOURCE)}, {kOut}}),
              S_OK);
    const GuestAddr held = h->read_ptr(kOut);
    EXPECT_EQ(h->com(chain, SwapChainSlot::ResizeBuffers,
                     {{0}, {32}, {16}, {0}, {0}}),
              DXGI_ERROR_INVALID_CALL);
    (void)h->com(held, ResourceSlot::Release, {});
    EXPECT_EQ(h->com(chain, SwapChainSlot::ResizeBuffers,
                     {{0}, {32}, {16}, {0}, {0}}),
              S_OK);
    ASSERT_EQ(h->com(chain, SwapChainSlot::GetDesc1, {{kDesc}}), S_OK);
    EXPECT_EQ(h->r32(kDesc + 0), 32u);
    EXPECT_EQ(h->r32(kDesc + 4), 16u);
    EXPECT_EQ(h->r32(kDesc + 8), static_cast<u32>(DXGI_FORMAT_R8G8B8A8_UNORM));
    EXPECT_EQ(h->r32(kDesc + 28), 2u);
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetCurrentBackBufferIndex, {}), 0u);
    EXPECT_EQ(h->com(chain, SwapChainSlot::ResizeBuffers,
                     {{0}, {0}, {0}, {0}, {0}}),
              S_OK);
    ASSERT_EQ(h->com(chain, SwapChainSlot::GetDesc1, {{kDesc}}), S_OK);
    EXPECT_EQ(h->r32(kDesc + 0), 120u);
    EXPECT_EQ(h->r32(kDesc + 4), 48u);
    EXPECT_EQ(h->com(chain, SwapChainSlot::ResizeBuffers,
                     {{1}, {32}, {16}, {0}, {0}}),
              E_INVALIDARG);
    EXPECT_EQ(h->com(chain, SwapChainSlot::ResizeBuffers,
                     {{0}, {32}, {16}, {0}, {0x800 /* ALLOW_TEARING */}}),
              E_INVALIDARG);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Release, {}), 0u);
    EXPECT_EQ(h->com(queue, CommandQueueSlot::AddRef, {}), queue_refs);
    (void)h->com(queue, CommandQueueSlot::Release, {});
}

// Learn (CreateSwapChainForComposition): the chain has no HWND; Present
// makes the current buffer available to the DirectComposition visual that
// has it as content. The D3D11 composition chains retain that frame until
// Commit supplies the target, then show it through the presentation-view
// swap chain and the visual's layer. GetCurrentBackBufferIndex still
// advances: each D3D12 buffer is its own resource.
TEST_P(D3D12Test, CompositionPresentShowsThroughTheBoundVisual) {
    attach_window(*h);
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    // DXGI_ALPHA_MODE_PREMULTIPLIED is valid on a composition chain.
    write_chain_desc(*h, 64, 48, 2, 0, 4, 1);
    GuestAddr chain = 0;
    ASSERT_EQ(h->call_manager(&D3D12Manager::impl_CreateSwapChainForComposition,
                              {{0}, {queue}, {kChainDesc}, {0}, {kOut}}),
              S_OK);
    chain = h->read_ptr(kOut);
    ASSERT_NE(chain, 0u);
    h->write_ptr(kOut, 0x111);
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetHwnd, {{kOut}}),
              DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(h->read_ptr(kOut), 0u);

    // IDXGISwapChain2::SetMatrixTransform (dx = 4) applies before the
    // visual's offset. No visual yet: Present succeeds, shows nothing, and
    // the ring moves. DO_NOT_WAIT is not WAS_STILL_DRAWING on an empty queue.
    constexpr GuestAddr kMatrix = kData + 0x1100;
    const float matrix[6] = {1.0f, 0.0f, 0.0f, 1.0f, 4.0f, 0.0f};
    for (u32 i = 0; i < 6; ++i) {
        u32 bits = 0;
        std::memcpy(&bits, &matrix[i], sizeof(bits));
        h->w32(kMatrix + i * 4u, bits);
    }
    EXPECT_EQ(h->com(chain, SwapChainSlot::SetMatrixTransform, {{kMatrix}}),
              S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0x8}}), S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetCurrentBackBufferIndex, {}), 1u);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0x1}}), S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetCurrentBackBufferIndex, {}), 1u);
    EXPECT_TRUE(h->backend.stream.empty());
    EXPECT_EQ(h->backend.swap_chain_creations, 0u);

    DxgiManager dxgi(h->memory, GetParam());
    D3D11Manager d3d11(h->memory, GetParam(), h->backend, dxgi);
    DcompManager dcomp(h->memory, GetParam(), d3d11);
    DcompManager::MethodThunks thunks{};
    GuestAddr thunk = 0x73000000;
    const auto fill = [&](auto& methods) {
        for (GuestAddr& method : methods) {
            method = thunk;
            thunk += 0x10;
        }
    };
    fill(thunks.device);
    fill(thunks.device2);
    fill(thunks.desktop_device);
    fill(thunks.device3);
    fill(thunks.target);
    fill(thunks.visual2);
    fill(thunks.translate_transform);
    fill(thunks.scale_transform);
    fill(thunks.rotate_transform);
    fill(thunks.skew_transform);
    fill(thunks.matrix_transform);
    fill(thunks.rectangle_clip);
    dcomp.set_method_thunks(thunks);
    constexpr u64 kUnhosted = 0x222;
    dcomp.set_window_ownership_resolver([](u64 hwnd) {
        return hwnd == kHwnd || hwnd == kUnhosted
                   ? DcompManager::WindowOwnership::CurrentProcess
                   : DcompManager::WindowOwnership::Invalid;
    });

    constexpr GuestAddr kDcompIid = kData + 0x1200;
    constexpr GuestAddr kDeviceOut = kData + 0x1220;
    constexpr GuestAddr kVisualOut = kData + 0x1230;
    constexpr GuestAddr kTargetOut = kData + 0x1240;
    // IID_IDCOMPOSITIONDESKTOPDEVICE, little-endian layout from
    // tests/winapi/d3d11/d3d11_test_guids.hpp.
    const u8 desktop_iid[16] = {0xFE, 0x33, 0x46, 0x5F, 0x08, 0x1E, 0xB8, 0x4C,
                                0x8C, 0x75, 0xCE, 0x24, 0x33, 0x3F, 0x56, 0x02};
    ASSERT_TRUE(h->memory.write_bytes(kDcompIid, desktop_iid, sizeof(desktop_iid))
                    .is_ok());

    auto dcall = [&](auto&& fn, std::vector<Harness::Arg> args,
                     float xmm1 = 0.0f) {
        h->cpu.sse.xmm[1].f32_[0] = xmm1;
        return h->invoke([&](Marshaler& m) { fn(m); }, args, false, 0, "");
    };
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_DCompositionCreateDevice3(m); },
                    {{0}, {kDcompIid}, {kDeviceOut}}),
              dcomp::S_OK);
    const GuestAddr composition_device = h->read_ptr(kDeviceOut);
    ASSERT_NE(composition_device, 0u);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 6);
              },
                    {{composition_device}, {kVisualOut}}),
              dcomp::S_OK);
    const GuestAddr visual = h->read_ptr(kVisualOut);
    ASSERT_NE(visual, 0u);
    // Scalar OffsetX / OffsetY (the animation overloads are the odd slots).
    const auto farg = [](float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return static_cast<u64>(bits);
    };
    // x64 reads the float from xmm1; x86 reads the stack bits.
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_VisualMethod(m, 4); },
                    {{visual}, {farg(12.0f)}}, 12.0f),
              dcomp::S_OK);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_VisualMethod(m, 6); },
                    {{visual}, {farg(7.0f)}}, 7.0f),
              dcomp::S_OK);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_VisualMethod(m, 15); },
                    {{visual}, {0x1234}}),
              dcomp::E_INVALIDARG);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_VisualMethod(m, 15); },
                    {{visual}, {chain}}),
              dcomp::S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::AddRef, {}), 3u);
    EXPECT_EQ(h->com(chain, SwapChainSlot::Release, {}), 2u);

    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 24);
              },
                    {{composition_device}, {kHwnd}, {1}, {kTargetOut}}),
              dcomp::S_OK);
    const GuestAddr target = h->read_ptr(kTargetOut);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_TargetMethod(m, 3); },
                    {{target}, {visual}}),
              dcomp::S_OK);
    // SetContent does not show the frame. Commit does.
    EXPECT_EQ(h->backend.swap_chain_creations, 0u);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 3);
              },
                    {{composition_device}}),
              dcomp::S_OK);
    EXPECT_EQ(h->backend.swap_chain_creations, 1u);
    EXPECT_TRUE(h->backend.last_swap_chain.premultiplied_alpha);
    EXPECT_EQ(h->backend.stream, (std::vector<std::string>{"present 64x48"}));
    ASSERT_EQ(h->backend.composition_layers.size(), 1u);
    const auto& layer = h->backend.composition_layers.front();
    EXPECT_FLOAT_EQ(layer.transform[0], 1.0f);
    EXPECT_FLOAT_EQ(layer.transform[3], 1.0f);
    // chain translation (4, 0) then visual offset (12, 7).
    EXPECT_FLOAT_EQ(layer.transform[4], 16.0f);
    EXPECT_FLOAT_EQ(layer.transform[5], 7.0f);
    EXPECT_TRUE(layer.above_window_children);
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetCurrentBackBufferIndex, {}), 1u);

    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0}}), S_OK);
    EXPECT_EQ(h->com(chain, SwapChainSlot::GetCurrentBackBufferIndex, {}), 0u);
    EXPECT_EQ(h->backend.stream.size(), 2u);
    EXPECT_EQ(h->backend.swap_chain_creations, 1u);

    // A target the host has no presentation view for cannot be committed,
    // and a later Present fails without moving the ring.
    write_chain_desc(*h, 32, 16, 2, 0, 4, 1);
    GuestAddr unbound = 0;
    ASSERT_EQ(h->call_manager(&D3D12Manager::impl_CreateSwapChainForComposition,
                              {{0}, {queue}, {kChainDesc}, {0}, {kOut}}),
              S_OK);
    unbound = h->read_ptr(kOut);
    ASSERT_NE(unbound, 0u);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_DCompositionCreateDevice3(m); },
                    {{0}, {kDcompIid}, {kDeviceOut}}),
              dcomp::S_OK);
    const GuestAddr other_device = h->read_ptr(kDeviceOut);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 6);
              },
                    {{other_device}, {kVisualOut}}),
              dcomp::S_OK);
    const GuestAddr other_visual = h->read_ptr(kVisualOut);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_VisualMethod(m, 15); },
                    {{other_visual}, {unbound}}),
              dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 24);
              },
                    {{other_device}, {kUnhosted}, {0}, {kTargetOut}}),
              dcomp::S_OK);
    const GuestAddr other_target = h->read_ptr(kTargetOut);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_TargetMethod(m, 3); },
                    {{other_target}, {other_visual}}),
              dcomp::S_OK);
    EXPECT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 3);
              },
                    {{other_device}}),
              dcomp::E_NOTIMPL);
    EXPECT_EQ(h->com(unbound, SwapChainSlot::GetCurrentBackBufferIndex, {}), 0u);
    EXPECT_EQ(h->com(unbound, SwapChainSlot::Present, {{0}, {0}}),
              DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(h->com(unbound, SwapChainSlot::GetCurrentBackBufferIndex, {}), 0u);
    ASSERT_EQ(h->com(unbound, SwapChainSlot::GetLastPresentCount, {{kOut2}}),
              S_OK);
    EXPECT_EQ(h->r32(kOut2), 0u);
}

// Learn (DCompositionCreateDevice2): renderingDevice may be NULL or a
// Direct3D device. D3D12 has no IDXGIDevice, so the pointer is the
// ID3D12Device or the ID3D12CommandQueue, and DCompositionCreateDevice
// accepts those too. The composition device AddRefs that pointer until
// its own Release, the same way it holds a D3D11 rendering device.
// SetContent of the composition swap chain still shows the frame on
// Commit. A target whose HWND has no presentation view still fails
// Commit with E_NOTIMPL.
TEST_P(D3D12Test, CompositionCreateDeviceAcceptsD3D12DeviceOrQueue) {
    attach_window(*h);
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    const GuestAddr copy = h->queue(dev, D3D12_COMMAND_LIST_TYPE_COPY);
    const GuestAddr allocator = h->allocator(dev);
    write_chain_desc(*h, 64, 48, 2, 0, 4, 1);
    ASSERT_EQ(h->call_manager(&D3D12Manager::impl_CreateSwapChainForComposition,
                              {{0}, {queue}, {kChainDesc}, {0}, {kOut}}),
              S_OK);
    const GuestAddr chain = h->read_ptr(kOut);
    ASSERT_NE(chain, 0u);
    // No visual yet: Present keeps the frame and shows nothing.
    EXPECT_EQ(h->com(chain, SwapChainSlot::Present, {{0}, {0}}), S_OK);
    EXPECT_TRUE(h->backend.stream.empty());

    DxgiManager dxgi(h->memory, GetParam());
    D3D11Manager d3d11(h->memory, GetParam(), h->backend, dxgi);
    DcompManager dcomp(h->memory, GetParam(), d3d11);
    DcompManager::MethodThunks thunks{};
    GuestAddr thunk = 0x74000000;
    const auto fill = [&](auto& methods) {
        for (GuestAddr& method : methods) {
            method = thunk;
            thunk += 0x10;
        }
    };
    fill(thunks.device);
    fill(thunks.device2);
    fill(thunks.desktop_device);
    fill(thunks.device3);
    fill(thunks.target);
    fill(thunks.visual2);
    fill(thunks.translate_transform);
    fill(thunks.scale_transform);
    fill(thunks.rotate_transform);
    fill(thunks.skew_transform);
    fill(thunks.matrix_transform);
    fill(thunks.rectangle_clip);
    dcomp.set_method_thunks(thunks);
    constexpr u64 kUnhosted = 0x222;
    dcomp.set_window_ownership_resolver([](u64 hwnd) {
        return hwnd == kHwnd || hwnd == kUnhosted
                   ? DcompManager::WindowOwnership::CurrentProcess
                   : DcompManager::WindowOwnership::Invalid;
    });

    constexpr GuestAddr kDcompIid = kData + 0x1200;
    constexpr GuestAddr kDeviceOut = kData + 0x1220;
    constexpr GuestAddr kVisualOut = kData + 0x1230;
    constexpr GuestAddr kTargetOut = kData + 0x1240;
    // IID bytes are the ones dcomp.cpp matches (IID_IDCompositionDevice and
    // IID_IDCompositionDesktopDevice).
    const u8 device_iid[16] = {0x3A, 0xA9, 0x7E, 0xC3, 0xAA, 0xE7, 0x0D, 0x45,
                               0xB1, 0x6F, 0x97, 0x46, 0xCB, 0x04, 0x07, 0xF3};
    const u8 desktop_iid[16] = {0xFE, 0x33, 0x46, 0x5F, 0x08, 0x1E, 0xB8, 0x4C,
                                0x8C, 0x75, 0xCE, 0x24, 0x33, 0x3F, 0x56, 0x02};
    auto dcall = [&](auto&& fn, std::vector<Harness::Arg> args) {
        return h->invoke([&](Marshaler& m) { fn(m); }, args, false, 0, "");
    };
    auto live = [&](GuestAddr object) {
        const u64 held = h->com(object, 1, {});
        EXPECT_EQ(h->com(object, 2, {}), held - 1);
        return held - 1;
    };
    struct CreateApi {
        void (DcompManager::*entry)(Marshaler&);
        DcompManager::DeviceInterface kind;
        const u8* iid;
    };
    const CreateApi apis[] = {
        {&DcompManager::impl_DCompositionCreateDevice,
         DcompManager::DeviceInterface::Legacy, device_iid},
        {&DcompManager::impl_DCompositionCreateDevice2,
         DcompManager::DeviceInterface::Desktop, desktop_iid},
        {&DcompManager::impl_DCompositionCreateDevice3,
         DcompManager::DeviceInterface::Desktop, desktop_iid},
    };
    const GuestAddr rendering[] = {0, dev, queue, copy};
    for (const CreateApi& api : apis) {
        ASSERT_TRUE(h->memory.write_bytes(kDcompIid, api.iid, 16).is_ok());
        for (const GuestAddr pointer : rendering) {
            const u64 before = pointer == 0 ? 0 : live(pointer);
            ASSERT_EQ(dcall([&](Marshaler& m) { (dcomp.*api.entry)(m); },
                            {{pointer}, {kDcompIid}, {kDeviceOut}}),
                      dcomp::S_OK);
            const GuestAddr composition = h->read_ptr(kDeviceOut);
            ASSERT_NE(composition, 0u);
            if (pointer != 0) EXPECT_EQ(live(pointer), before + 1);
            // Release returns the new reference count.
            EXPECT_EQ(dcall([&](Marshaler& m) {
                          dcomp.impl_DeviceMethod(m, api.kind, 2);
                      },
                            {{composition}}),
                      0u);
            if (pointer != 0) EXPECT_EQ(live(pointer), before);
        }
    }

    ASSERT_TRUE(h->memory.write_bytes(kDcompIid, desktop_iid, 16).is_ok());
    const auto reject = [&](GuestAddr pointer) {
        const u64 before = live(pointer);
        h->write_ptr(kDeviceOut, 0x111);
        EXPECT_EQ(dcall([&](Marshaler& m) {
                      dcomp.impl_DCompositionCreateDevice3(m);
                  },
                        {{pointer}, {kDcompIid}, {kDeviceOut}}),
                  dcomp::E_INVALIDARG);
        EXPECT_EQ(h->read_ptr(kDeviceOut), 0u);
        EXPECT_EQ(live(pointer), before);
    };
    reject(chain);
    reject(allocator);
    h->write_ptr(kDeviceOut, 0x111);
    EXPECT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DCompositionCreateDevice3(m);
              },
                    {{0x1234}, {kDcompIid}, {kDeviceOut}}),
              dcomp::E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kDeviceOut), 0u);

    // DCompositionCreateDevice still answers only IDCompositionDevice.
    const u64 device_refs = live(dev);
    ASSERT_TRUE(h->memory.write_bytes(kDcompIid, desktop_iid, 16).is_ok());
    h->write_ptr(kDeviceOut, 0x111);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_DCompositionCreateDevice(m); },
                    {{dev}, {kDcompIid}, {kDeviceOut}}),
              dcomp::E_NOINTERFACE);
    EXPECT_EQ(h->read_ptr(kDeviceOut), 0u);
    EXPECT_EQ(live(dev), device_refs);

    ASSERT_TRUE(h->memory.write_bytes(kDcompIid, desktop_iid, 16).is_ok());
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DCompositionCreateDevice3(m);
              },
                    {{queue}, {kDcompIid}, {kDeviceOut}}),
              dcomp::S_OK);
    const GuestAddr composition_device = h->read_ptr(kDeviceOut);
    ASSERT_NE(composition_device, 0u);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 6);
              },
                    {{composition_device}, {kVisualOut}}),
              dcomp::S_OK);
    const GuestAddr visual = h->read_ptr(kVisualOut);
    const u64 chain_refs = live(chain);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_VisualMethod(m, 15); },
                    {{visual}, {chain}}),
              dcomp::S_OK);
    EXPECT_EQ(live(chain), chain_refs + 1);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 24);
              },
                    {{composition_device}, {kHwnd}, {1}, {kTargetOut}}),
              dcomp::S_OK);
    const GuestAddr target = h->read_ptr(kTargetOut);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_TargetMethod(m, 3); },
                    {{target}, {visual}}),
              dcomp::S_OK);
    EXPECT_TRUE(h->backend.stream.empty());
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 3);
              },
                    {{composition_device}}),
              dcomp::S_OK);
    EXPECT_EQ(h->backend.swap_chain_creations, 1u);
    EXPECT_TRUE(h->backend.last_swap_chain.premultiplied_alpha);
    EXPECT_EQ(h->backend.stream, (std::vector<std::string>{"present 64x48"}));

    // The same chain is content for a composition device built on the
    // ID3D12Device, not only on the queue that created the chain.
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DCompositionCreateDevice3(m);
              },
                    {{dev}, {kDcompIid}, {kDeviceOut}}),
              dcomp::S_OK);
    const GuestAddr device_composition = h->read_ptr(kDeviceOut);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 6);
              },
                    {{device_composition}, {kVisualOut}}),
              dcomp::S_OK);
    const GuestAddr device_visual = h->read_ptr(kVisualOut);
    const u64 chain_refs_again = live(chain);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_VisualMethod(m, 15); },
                    {{device_visual}, {chain}}),
              dcomp::S_OK);
    EXPECT_EQ(live(chain), chain_refs_again + 1);

    write_chain_desc(*h, 32, 16, 2, 0, 4, 1);
    ASSERT_EQ(h->call_manager(&D3D12Manager::impl_CreateSwapChainForComposition,
                              {{0}, {queue}, {kChainDesc}, {0}, {kOut}}),
              S_OK);
    const GuestAddr unbound = h->read_ptr(kOut);
    ASSERT_NE(unbound, 0u);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 6);
              },
                    {{device_composition}, {kVisualOut}}),
              dcomp::S_OK);
    const GuestAddr unbound_visual = h->read_ptr(kVisualOut);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_VisualMethod(m, 15); },
                    {{unbound_visual}, {unbound}}),
              dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 24);
              },
                    {{device_composition}, {kUnhosted}, {0}, {kTargetOut}}),
              dcomp::S_OK);
    const GuestAddr unbound_target = h->read_ptr(kTargetOut);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_TargetMethod(m, 3); },
                    {{unbound_target}, {unbound_visual}}),
              dcomp::S_OK);
    EXPECT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 3);
              },
                    {{device_composition}}),
              dcomp::E_NOTIMPL);
    EXPECT_EQ(h->com(unbound, SwapChainSlot::GetCurrentBackBufferIndex, {}), 0u);
    EXPECT_EQ(h->com(unbound, SwapChainSlot::Present, {{0}, {0}}),
              DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(h->com(unbound, SwapChainSlot::GetCurrentBackBufferIndex, {}), 0u);
    ASSERT_EQ(h->com(unbound, SwapChainSlot::GetLastPresentCount, {{kOut2}}),
              S_OK);
    EXPECT_EQ(h->r32(kOut2), 0u);
    EXPECT_EQ(h->backend.stream, (std::vector<std::string>{"present 64x48"}));
    EXPECT_EQ(h->backend.swap_chain_creations, 1u);
}

// Learn (IDCompositionDevice::CreateSurface, IDCompositionSurface::BeginDraw):
// the surface stores one of the three documented formats. BeginDraw returns a
// DXGI surface for the update rectangle and an offset of that rectangle's
// upper left. ID2D1DeviceContext is E_INVALIDARG without a Direct2D device.
// Any other IID is E_NOINTERFACE. The first update covers the whole surface.
// Only one surface per device is in Drawing; SuspendDraw lets another
// BeginDraw proceed, and EndDraw publishes the rectangle. Commit before
// EndDraw does not show it. A target HWND with no presentation view still
// fails Commit with E_NOTIMPL.
TEST_P(D3D12Test, CompositionSurfaceBeginDrawPublishesTheUpdate) {
    attach_window(*h);
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);

    DxgiManager dxgi(h->memory, GetParam());
    D3D11Manager d3d11(h->memory, GetParam(), h->backend, dxgi);
    DcompManager dcomp(h->memory, GetParam(), d3d11);
    DcompManager::MethodThunks thunks{};
    GuestAddr thunk = 0x75000000;
    const auto fill = [&](auto& methods) {
        for (GuestAddr& method : methods) {
            method = thunk;
            thunk += 0x10;
        }
    };
    fill(thunks.device);
    fill(thunks.device2);
    fill(thunks.desktop_device);
    fill(thunks.device3);
    fill(thunks.target);
    fill(thunks.visual2);
    fill(thunks.translate_transform);
    fill(thunks.scale_transform);
    fill(thunks.rotate_transform);
    fill(thunks.skew_transform);
    fill(thunks.matrix_transform);
    fill(thunks.rectangle_clip);
    fill(thunks.surface);
    fill(thunks.update_object);
    dcomp.set_method_thunks(thunks);
    constexpr u64 kUnhosted = 0x222;
    dcomp.set_window_ownership_resolver([](u64 hwnd) {
        return hwnd == kHwnd || hwnd == kUnhosted
                   ? DcompManager::WindowOwnership::CurrentProcess
                   : DcompManager::WindowOwnership::Invalid;
    });

    auto exports = create_dcomp_thunks(dcomp);
    const auto find_export = [&](const char* name) -> const ExportEntry* {
        for (const ExportEntry& entry : exports.entries) {
            if (entry.name == name) return &entry;
        }
        return nullptr;
    };
    // x86 stack cleanup from dcomp.h / dxgi.h. BeginDraw is five pointer
    // arguments (20). Scroll is two rectangles and two ints (20). Map is
    // the mapped rect plus flags (12). GetDesc is one pointer (8).
    ASSERT_NE(find_export("_DComp_Surface_3"), nullptr);
    EXPECT_EQ(find_export("_DComp_Surface_3")->stdcall_bytes, 20u);
    EXPECT_EQ(find_export("_DComp_Surface_4")->stdcall_bytes, 4u);
    EXPECT_EQ(find_export("_DComp_Surface_7")->stdcall_bytes, 20u);
    EXPECT_EQ(find_export("_DComp_SurfaceUpdate_8")->stdcall_bytes, 8u);
    EXPECT_EQ(find_export("_DComp_SurfaceUpdate_9")->stdcall_bytes, 12u);

    constexpr GuestAddr kDcompIid = kData + 0x1200;
    constexpr GuestAddr kDeviceOut = kData + 0x1220;
    constexpr GuestAddr kVisualOut = kData + 0x1230;
    constexpr GuestAddr kTargetOut = kData + 0x1240;
    constexpr GuestAddr kSurfaceOut = kData + 0x1260;
    constexpr GuestAddr kUpdateOut = kData + 0x1280;
    constexpr GuestAddr kOffset = kData + 0x12A0;
    constexpr GuestAddr kRect = kData + 0x12C0;
    constexpr GuestAddr kMapped = kData + 0x12E0;
    constexpr GuestAddr kSurfDesc = kData + 0x1300;
    constexpr GuestAddr kQiOut = kData + 0x1320;
    const u8 device_iid[16] = {0x3A, 0xA9, 0x7E, 0xC3, 0xAA, 0xE7, 0x0D, 0x45,
                               0xB1, 0x6F, 0x97, 0x46, 0xCB, 0x04, 0x07, 0xF3};
    const u8 desktop_iid[16] = {0xFE, 0x33, 0x46, 0x5F, 0x08, 0x1E, 0xB8, 0x4C,
                                0x8C, 0x75, 0xCE, 0x24, 0x33, 0x3F, 0x56, 0x02};
    // dcomp.h IDCompositionSurface, dxgi.h IDXGISurface / IDXGISurface1,
    // d2d1_1.h ID2D1DeviceContext.
    const u8 surface_iid[16] = {0x53, 0x49, 0x8A, 0xBB, 0x99, 0x2C, 0x5A, 0x4F,
                                0x96, 0xF5, 0x48, 0x19, 0x02, 0x7F, 0xA3, 0xAC};
    const u8 dxgi_surface_iid[16] = {
        0x6C, 0xB5, 0xFC, 0xCA, 0xC3, 0x6A, 0x89, 0x48,
        0xBF, 0x47, 0x9E, 0x23, 0xBB, 0xD2, 0x60, 0xEC};
    const u8 dxgi_surface1_iid[16] = {
        0x92, 0x30, 0xE6, 0x4A, 0x27, 0x63, 0x1B, 0x4C,
        0x80, 0xAE, 0xBF, 0xE1, 0x2E, 0xA3, 0x2B, 0x86};
    const u8 d2d_context_iid[16] = {
        0x7A, 0xFE, 0xF7, 0xE8, 0x1C, 0x19, 0x6D, 0x46,
        0xAD, 0x95, 0x97, 0x56, 0x78, 0xBD, 0xA9, 0x98};
    const u8 unknown_iid[16] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                                0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11};
    ASSERT_TRUE(h->memory.write_bytes(kDcompIid, desktop_iid, 16).is_ok());

    auto dcall = [&](auto&& fn, std::vector<Harness::Arg> args,
                     float xmm1 = 0.0f) {
        h->cpu.sse.xmm[1].f32_[0] = xmm1;
        return h->invoke([&](Marshaler& m) { fn(m); }, args, false, 0, "");
    };
    auto live = [&](GuestAddr object) {
        const u64 held = h->com(object, 1, {});
        EXPECT_EQ(h->com(object, 2, {}), held - 1);
        return held - 1;
    };
    const auto farg = [](float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return static_cast<u64>(bits);
    };
    auto write_iid = [&](GuestAddr at, const u8* bytes) {
        ASSERT_TRUE(h->memory.write_bytes(at, bytes, 16).is_ok());
    };
    constexpr GuestAddr kDxgiIid = kData + 0x1400;
    constexpr GuestAddr kD2dIid = kData + 0x1420;
    constexpr GuestAddr kUnknownIid = kData + 0x1440;
    constexpr GuestAddr kSurfaceIid = kData + 0x1460;
    write_iid(kDxgiIid, dxgi_surface_iid);
    write_iid(kD2dIid, d2d_context_iid);
    write_iid(kUnknownIid, unknown_iid);
    write_iid(kSurfaceIid, surface_iid);
    constexpr GuestAddr kSurface1Iid = kData + 0x1480;
    write_iid(kSurface1Iid, dxgi_surface1_iid);

    auto make_device = [&](GuestAddr rendering,
                           DcompManager::DeviceInterface kind,
                           const u8* iid) {
        write_iid(kDcompIid, iid);
        EXPECT_EQ(dcall([&](Marshaler& m) {
                      dcomp.impl_DCompositionCreateDevice3(m);
                  },
                        {{rendering}, {kDcompIid}, {kDeviceOut}}),
                  dcomp::S_OK);
        const GuestAddr composition = h->read_ptr(kDeviceOut);
        EXPECT_NE(composition, 0u);
        (void)kind;
        return composition;
    };
    // DCompositionCreateDevice answers only the legacy interface.
    write_iid(kDcompIid, device_iid);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_DCompositionCreateDevice(m); },
                    {{queue}, {kDcompIid}, {kDeviceOut}}),
              dcomp::S_OK);
    const GuestAddr legacy = h->read_ptr(kDeviceOut);
    h->write_ptr(kSurfaceOut, 0x111);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Legacy, 8);
              },
                    {{legacy}, {4}, {4}, {DXGI_FORMAT_B8G8R8A8_UNORM}, {1},
                     {kSurfaceOut}}),
              dcomp::S_OK);
    const GuestAddr legacy_surface = h->read_ptr(kSurfaceOut);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 2); },
                    {{legacy_surface}}),
              0u);
    EXPECT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Legacy, 2);
              },
                    {{legacy}}),
              0u);

    const GuestAddr composition = make_device(
        queue, DcompManager::DeviceInterface::Desktop, desktop_iid);
    h->write_ptr(kSurfaceOut, 0x111);
    EXPECT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 8);
              },
                    {{composition}, {4}, {4}, {DXGI_FORMAT_B8G8R8A8_UNORM},
                     {1}, {0}}),
              dcomp::E_INVALIDARG);
    EXPECT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 8);
              },
                    {{composition}, {0}, {4}, {DXGI_FORMAT_B8G8R8A8_UNORM},
                     {1}, {kSurfaceOut}}),
              dcomp::E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kSurfaceOut), 0u);
    h->write_ptr(kSurfaceOut, 0x111);
    EXPECT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 8);
              },
                    {{composition}, {4}, {4}, {DXGI_FORMAT_R8_UNORM}, {1},
                     {kSurfaceOut}}),
              dcomp::E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kSurfaceOut), 0u);
    h->write_ptr(kSurfaceOut, 0x111);
    // DXGI_ALPHA_MODE_STRAIGHT is not a CreateSurface alpha mode.
    EXPECT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 8);
              },
                    {{composition}, {4}, {4}, {DXGI_FORMAT_R8G8B8A8_UNORM},
                     {2}, {kSurfaceOut}}),
              dcomp::E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kSurfaceOut), 0u);
    for (const u32 alpha : {0u, 1u, 3u}) {
        h->write_ptr(kSurfaceOut, 0);
        ASSERT_EQ(dcall([&](Marshaler& m) {
                      dcomp.impl_DeviceMethod(
                          m, DcompManager::DeviceInterface::Desktop, 8);
                  },
                        {{composition}, {2}, {2},
                         {DXGI_FORMAT_R16G16B16A16_FLOAT}, {alpha},
                         {kSurfaceOut}}),
                  dcomp::S_OK);
        EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 2); },
                        {{h->read_ptr(kSurfaceOut)}}),
                  0u);
    }
    h->write_ptr(kSurfaceOut, 0x111);
    // Learn CreateVirtualSurface: an edge above 16,777,216 fails and the
    // out pointer is cleared.
    EXPECT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 9);
              },
                    {{composition}, {16777217}, {4},
                     {DXGI_FORMAT_B8G8R8A8_UNORM}, {1}, {kSurfaceOut}}),
              dcomp::E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kSurfaceOut), 0u);

    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 8);
              },
                    {{composition}, {8}, {4}, {DXGI_FORMAT_B8G8R8A8_UNORM},
                     {1}, {kSurfaceOut}}),
              dcomp::S_OK);
    const GuestAddr surface = h->read_ptr(kSurfaceOut);
    h->write_ptr(kQiOut, 0);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 0); },
                    {{surface}, {kUnknownIid}, {kQiOut}}),
              dcomp::E_NOINTERFACE);
    EXPECT_EQ(h->read_ptr(kQiOut), 0u);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 0); },
                    {{surface}, {kSurfaceIid}, {kQiOut}}),
              dcomp::S_OK);
    EXPECT_EQ(h->read_ptr(kQiOut), surface);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 2); },
                    {{surface}}),
              1u);

    auto begin = [&](GuestAddr which, GuestAddr rect, GuestAddr iid) {
        h->write_ptr(kUpdateOut, 0x111);
        h->w32(kOffset, 0x11111111u);
        h->w32(kOffset + 4, 0x22222222u);
        return dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 3); },
                     {{which}, {rect}, {iid}, {kUpdateOut}, {kOffset}});
    };
    EXPECT_EQ(begin(surface, 0, 0), dcomp::E_INVALIDARG);
    EXPECT_EQ(begin(surface, 0, kD2dIid), dcomp::E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kUpdateOut), 0u);
    EXPECT_EQ(h->r32(kOffset), 0u);
    EXPECT_EQ(begin(surface, 0, kUnknownIid), dcomp::E_NOINTERFACE);
    EXPECT_EQ(h->read_ptr(kUpdateOut), 0u);
    // First update must cover the surface. (0,0,1,1) does not.
    h->w32(kRect + 0, 0);
    h->w32(kRect + 4, 0);
    h->w32(kRect + 8, 1);
    h->w32(kRect + 12, 1);
    EXPECT_EQ(begin(surface, kRect, kDxgiIid), dcomp::E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kUpdateOut), 0u);
    h->w32(kRect + 0, 0xFFFFFFFFu);
    EXPECT_EQ(begin(surface, kRect, kDxgiIid), dcomp::E_INVALIDARG);

    ASSERT_EQ(begin(surface, 0, kSurface1Iid), dcomp::S_OK);
    const GuestAddr update = h->read_ptr(kUpdateOut);
    EXPECT_NE(update, 0u);
    EXPECT_EQ(h->r32(kOffset), 0u);
    EXPECT_EQ(h->r32(kOffset + 4), 0u);
    EXPECT_EQ(begin(surface, 0, kDxgiIid),
              dcomp::DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED);
    EXPECT_EQ(h->read_ptr(kUpdateOut), 0u);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 8); },
                    {{update}, {kSurfDesc}}),
              dcomp::S_OK);
    EXPECT_EQ(h->r32(kSurfDesc + 0), 8u);
    EXPECT_EQ(h->r32(kSurfDesc + 4), 4u);
    EXPECT_EQ(h->r32(kSurfDesc + 8),
              static_cast<u32>(DXGI_FORMAT_B8G8R8A8_UNORM));
    EXPECT_EQ(h->r32(kSurfDesc + 12), 1u);
    const u64 queue_before = live(queue);
    h->write_ptr(kQiOut, 0x111);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 7); },
                    {{update}, {h->iid(IID_ID3D12COMMANDQUEUE)}, {kQiOut}}),
              dcomp::S_OK);
    EXPECT_EQ(h->read_ptr(kQiOut), queue);
    EXPECT_EQ(live(queue), queue_before + 1);
    (void)h->com(queue, 2, {});
    h->write_ptr(kQiOut, 0x111);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 7); },
                    {{update}, {h->iid(IID_ID3D12DEVICE)}, {kQiOut}}),
              dcomp::E_NOINTERFACE);
    EXPECT_EQ(h->read_ptr(kQiOut), 0u);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 9); },
                    {{update}, {0}, {2u /* DXGI_MAP_WRITE */}}),
              dcomp::E_INVALIDARG);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 9); },
                    {{update}, {kMapped}, {2u /* DXGI_MAP_WRITE */}}),
              dcomp::S_OK);
    EXPECT_EQ(h->r32(kMapped), 8u * 4u);
    const GuestAddr bits = h->read_ptr(kMapped + h->ptr_size());
    EXPECT_NE(bits, 0u);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 9); },
                    {{update}, {kMapped}, {2u /* DXGI_MAP_WRITE */}}),
              dcomp::DXGI_ERROR_INVALID_CALL);
    const u8 pixel[4] = {0x11, 0x22, 0x33, 0x44};
    ASSERT_TRUE(h->memory.write_bytes(bits, pixel, sizeof(pixel)).is_ok());
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 10); },
                    {{update}}),
              dcomp::S_OK);
    // Learn Scroll: a surface with BeginDraw outstanding is still being
    // updated, so Scroll fails.
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 7); },
                    {{surface}, {0}, {0}, {0}, {0}}),
              dcomp::DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED);

    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 6);
              },
                    {{composition}, {kVisualOut}}),
              dcomp::S_OK);
    const GuestAddr visual = h->read_ptr(kVisualOut);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_VisualMethod(m, 4); },
                    {{visual}, {farg(5.0f)}}, 5.0f),
              dcomp::S_OK);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_VisualMethod(m, 6); },
                    {{visual}, {farg(9.0f)}}, 9.0f),
              dcomp::S_OK);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_VisualMethod(m, 15); },
                    {{visual}, {surface}}),
              dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 24);
              },
                    {{composition}, {kHwnd}, {1}, {kTargetOut}}),
              dcomp::S_OK);
    const GuestAddr target = h->read_ptr(kTargetOut);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_TargetMethod(m, 3); },
                    {{target}, {visual}}),
              dcomp::S_OK);
    const usize uploads_before_commit = h->backend.texture_uploads.size();
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 3);
              },
                    {{composition}}),
              dcomp::S_OK);
    EXPECT_EQ(h->backend.texture_uploads.size(), uploads_before_commit);
    EXPECT_TRUE(h->backend.stream.empty());
    EXPECT_EQ(h->backend.swap_chain_creations, 0u);

    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 4); },
                    {{surface}}),
              dcomp::S_OK);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 4); },
                    {{surface}}),
              dcomp::DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 9); },
                    {{update}, {kMapped}, {1u /* DXGI_MAP_READ */}}),
              dcomp::DXGI_ERROR_INVALID_CALL);
    ASSERT_EQ(h->backend.texture_uploads.size(), uploads_before_commit + 1);
    const auto& published = h->backend.texture_uploads[uploads_before_commit];
    EXPECT_EQ(published.pitch, 32u);
    ASSERT_GE(published.bytes.size(), 4u);
    EXPECT_EQ(published.bytes[0], 0x11);
    EXPECT_EQ(published.bytes[1], 0x22);
    EXPECT_EQ(published.bytes[2], 0x33);
    EXPECT_EQ(published.bytes[3], 0x44);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 3);
              },
                    {{composition}}),
              dcomp::S_OK);
    EXPECT_EQ(h->backend.swap_chain_creations, 1u);
    EXPECT_TRUE(h->backend.last_swap_chain.premultiplied_alpha);
    EXPECT_EQ(h->backend.stream, (std::vector<std::string>{"present 8x4"}));
    ASSERT_FALSE(h->backend.composition_layers.empty());
    EXPECT_FLOAT_EQ(h->backend.composition_layers.back().transform[4], 5.0f);
    EXPECT_FLOAT_EQ(h->backend.composition_layers.back().transform[5], 9.0f);
    EXPECT_TRUE(h->backend.composition_layers.back().above_window_children);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 3);
              },
                    {{composition}}),
              dcomp::S_OK);
    EXPECT_EQ(h->backend.swap_chain_creations, 1u);
    EXPECT_EQ(h->backend.stream.size(), 2u);

    // A later update may be a sub-rectangle. The returned surface is that
    // rectangle, so the offset of its upper left is (0, 0), and only those
    // pixels change in the published image.
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 8);
              },
                    {{composition}, {4}, {1}, {DXGI_FORMAT_R8G8B8A8_UNORM},
                     {3}, {kSurfaceOut}}),
              dcomp::S_OK);
    const GuestAddr sub = h->read_ptr(kSurfaceOut);
    ASSERT_EQ(begin(sub, 0, kDxgiIid), dcomp::S_OK);
    const GuestAddr sub_update = h->read_ptr(kUpdateOut);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 9); },
                    {{sub_update}, {kMapped}, {2u /* DXGI_MAP_WRITE */}}),
              dcomp::S_OK);
    const GuestAddr sub_bits = h->read_ptr(kMapped + h->ptr_size());
    std::vector<u8> full(16, 0xFF);
    ASSERT_TRUE(h->memory.write_bytes(sub_bits, full.data(), full.size()).is_ok());
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 4); },
                    {{sub}}),
              dcomp::S_OK);
    h->w32(kRect + 0, 1);
    h->w32(kRect + 4, 0);
    h->w32(kRect + 8, 3);
    h->w32(kRect + 12, 1);
    ASSERT_EQ(begin(sub, kRect, kDxgiIid), dcomp::S_OK);
    EXPECT_EQ(h->r32(kOffset), 0u);
    EXPECT_EQ(h->r32(kOffset + 4), 0u);
    const GuestAddr sub_update2 = h->read_ptr(kUpdateOut);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 8); },
                    {{sub_update2}, {kSurfDesc}}),
              dcomp::S_OK);
    EXPECT_EQ(h->r32(kSurfDesc + 0), 2u);
    EXPECT_EQ(h->r32(kSurfDesc + 4), 1u);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 9); },
                    {{sub_update2}, {kMapped}, {2u /* DXGI_MAP_WRITE */}}),
              dcomp::S_OK);
    const GuestAddr patch_bits = h->read_ptr(kMapped + h->ptr_size());
    const u8 patch[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    ASSERT_TRUE(h->memory.write_bytes(patch_bits, patch, sizeof(patch)).is_ok());
    const usize sub_upload = h->backend.texture_uploads.size();
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 4); },
                    {{sub}}),
              dcomp::S_OK);
    ASSERT_LT(sub_upload, h->backend.texture_uploads.size());
    const auto& patched = h->backend.texture_uploads[sub_upload];
    ASSERT_GE(patched.bytes.size(), 16u);
    EXPECT_EQ(patched.bytes[0], 0xFF);
    EXPECT_EQ(patched.bytes[4], 0xAA);
    EXPECT_EQ(patched.bytes[8], 0xAA);
    EXPECT_EQ(patched.bytes[12], 0xFF);

    auto fresh = [&](u32 alpha) -> GuestAddr {
        EXPECT_EQ(dcall([&](Marshaler& m) {
                      dcomp.impl_DeviceMethod(
                          m, DcompManager::DeviceInterface::Desktop, 8);
                  },
                        {{composition}, {2}, {2}, {DXGI_FORMAT_B8G8R8A8_UNORM},
                         {alpha}, {kSurfaceOut}}),
                  dcomp::S_OK);
        return h->read_ptr(kSurfaceOut);
    };
    const GuestAddr left = fresh(1);
    const GuestAddr right = fresh(1);
    ASSERT_EQ(begin(left, 0, kDxgiIid), dcomp::S_OK);
    EXPECT_EQ(begin(right, 0, kDxgiIid),
              dcomp::DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 5); },
                    {{left}}),
              dcomp::S_OK);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 5); },
                    {{left}}),
              dcomp::DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED);
    EXPECT_EQ(begin(left, 0, kDxgiIid),
              dcomp::DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED);
    ASSERT_EQ(begin(right, 0, kDxgiIid), dcomp::S_OK);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 6); },
                    {{left}}),
              dcomp::DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 4); },
                    {{right}}),
              dcomp::S_OK);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 6); },
                    {{left}}),
              dcomp::S_OK);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 6); },
                    {{left}}),
              dcomp::DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 4); },
                    {{left}}),
              dcomp::S_OK);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 5); },
                    {{left}}),
              dcomp::DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 6); },
                    {{left}}),
              dcomp::DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED);

    // EndDraw of a suspended surface publishes without ResumeDraw.
    const GuestAddr suspended = fresh(0);
    ASSERT_EQ(begin(suspended, 0, kDxgiIid), dcomp::S_OK);
    const GuestAddr suspended_update = h->read_ptr(kUpdateOut);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 9); },
                    {{suspended_update}, {kMapped}, {2u /* DXGI_MAP_WRITE */}}),
              dcomp::S_OK);
    const GuestAddr suspended_bits = h->read_ptr(kMapped + h->ptr_size());
    const u8 mark = 0x5A;
    ASSERT_TRUE(h->memory.write_bytes(suspended_bits, &mark, 1).is_ok());
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 5); },
                    {{suspended}}),
              dcomp::S_OK);
    const usize suspended_upload = h->backend.texture_uploads.size();
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 4); },
                    {{suspended}}),
              dcomp::S_OK);
    ASSERT_LT(suspended_upload, h->backend.texture_uploads.size());
    EXPECT_EQ(h->backend.texture_uploads[suspended_upload].bytes[0], 0x5A);

    // A second device may draw while this one is suspended: the guard is
    // per composition device.
    const GuestAddr other = make_device(
        dev, DcompManager::DeviceInterface::Desktop, desktop_iid);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 8);
              },
                    {{other}, {2}, {2}, {DXGI_FORMAT_R16G16B16A16_FLOAT}, {0},
                     {kSurfaceOut}}),
              dcomp::S_OK);
    const GuestAddr wide = h->read_ptr(kSurfaceOut);
    ASSERT_EQ(begin(left, 0, kDxgiIid), dcomp::S_OK);
    ASSERT_EQ(begin(wide, 0, kDxgiIid), dcomp::S_OK);
    const GuestAddr wide_update = h->read_ptr(kUpdateOut);
    h->write_ptr(kQiOut, 0);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 7); },
                    {{wide_update}, {h->iid(IID_ID3D12DEVICE)}, {kQiOut}}),
              dcomp::S_OK);
    EXPECT_EQ(h->read_ptr(kQiOut), dev);
    (void)h->com(dev, 2, {});
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 8); },
                    {{wide_update}, {kSurfDesc}}),
              dcomp::S_OK);
    EXPECT_EQ(h->r32(kSurfDesc + 8),
              static_cast<u32>(DXGI_FORMAT_R16G16B16A16_FLOAT));
    const usize wide_upload = h->backend.texture_uploads.size();
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 4); },
                    {{wide}}),
              dcomp::S_OK);
    ASSERT_LT(wide_upload, h->backend.texture_uploads.size());
    EXPECT_EQ(h->backend.texture_uploads[wide_upload].bytes.size(), 32u);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 4); },
                    {{left}}),
              dcomp::S_OK);

    // UNSPECIFIED alpha is IGNORE: the presenter does not treat it as
    // premultiplied. topmost=0 is a different target from the one above.
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 6);
              },
                    {{other}, {kVisualOut}}),
              dcomp::S_OK);
    const GuestAddr plain_visual = h->read_ptr(kVisualOut);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_VisualMethod(m, 15); },
                    {{plain_visual}, {wide}}),
              dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 24);
              },
                    {{other}, {kHwnd}, {0}, {kTargetOut}}),
              dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_TargetMethod(m, 3); },
                    {{h->read_ptr(kTargetOut)}, {plain_visual}}),
              dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 3);
              },
                    {{other}}),
              dcomp::S_OK);
    EXPECT_FALSE(h->backend.last_swap_chain.premultiplied_alpha);
    EXPECT_EQ(h->backend.stream.back(), "present 2x2");

    const GuestAddr unhosted_device = make_device(
        queue, DcompManager::DeviceInterface::Desktop, desktop_iid);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 8);
              },
                    {{unhosted_device}, {2}, {2}, {DXGI_FORMAT_B8G8R8A8_UNORM},
                     {1}, {kSurfaceOut}}),
              dcomp::S_OK);
    const GuestAddr unhosted_surface = h->read_ptr(kSurfaceOut);
    ASSERT_EQ(begin(unhosted_surface, 0, kDxgiIid), dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 4); },
                    {{unhosted_surface}}),
              dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 6);
              },
                    {{unhosted_device}, {kVisualOut}}),
              dcomp::S_OK);
    const GuestAddr unhosted_visual = h->read_ptr(kVisualOut);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_VisualMethod(m, 15); },
                    {{unhosted_visual}, {unhosted_surface}}),
              dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 24);
              },
                    {{unhosted_device}, {kUnhosted}, {0}, {kTargetOut}}),
              dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_TargetMethod(m, 3); },
                    {{h->read_ptr(kTargetOut)}, {unhosted_visual}}),
              dcomp::S_OK);
    const unsigned creations = h->backend.swap_chain_creations;
    EXPECT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 3);
              },
                    {{unhosted_device}}),
              dcomp::E_NOTIMPL);
    EXPECT_EQ(h->backend.swap_chain_creations, creations);
}

// Reserved resources: creation from a02, mappings from Learn
// UpdateTileMappings / CopyTileMappings, tiling from the standard shape
// WARP reports plus the backend's own packed tail (D3D11.3 5.9.2.8.5:
// packing is the implementation's; mips of at least one tile never pack).
// Tier 1; volumes (tier 3) stay unimplemented.
class D3D12TiledTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        if (!d3d12_enabled()) {
            GTEST_SKIP() << "D3D12 explicitly disabled by VITREO_D3D12=0";
        }
        h = std::make_unique<Harness>(GetParam(), true);
    }

    void clear_desc(GuestAddr at, u32 bytes) {
        for (u32 i = 0; i < bytes; i += 4) h->w32(at + i, 0);
    }

    /// A buffer description at @p at (a02 reserved buffer rows).
    void buffer_desc(GuestAddr at, u64 width, u32 flags = 0) {
        const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
        clear_desc(at, rd.size);
        h->w32(at + rd.Dimension, D3D12_RESOURCE_DIMENSION_BUFFER);
        h->w64(at + rd.Width, width);
        h->w32(at + rd.Height, 1);
        ASSERT_TRUE(h->memory.write_u16(at + rd.DepthOrArraySize, 1).is_ok());
        ASSERT_TRUE(h->memory.write_u16(at + rd.MipLevels, 1).is_ok());
        h->w32(at + rd.SampleDesc_Count, 1);
        h->w32(at + rd.Layout, D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
        h->w32(at + rd.Flags, flags);
    }

    void texture_desc(GuestAddr at, u32 dimension, u32 width, u32 height,
                      u32 depth_or_array, u32 mips, u32 format, u32 samples,
                      u32 layout_kind, u32 flags) {
        const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
        clear_desc(at, rd.size);
        h->w32(at + rd.Dimension, dimension);
        h->w64(at + rd.Width, width);
        h->w32(at + rd.Height, height);
        ASSERT_TRUE(h->memory.write_u16(at + rd.DepthOrArraySize,
                                        static_cast<u16>(depth_or_array))
                        .is_ok());
        ASSERT_TRUE(h->memory.write_u16(at + rd.MipLevels, static_cast<u16>(mips))
                        .is_ok());
        h->w32(at + rd.Format, format);
        h->w32(at + rd.SampleDesc_Count, samples);
        h->w32(at + rd.Layout, layout_kind);
        h->w32(at + rd.Flags, flags);
    }

    u64 create_reserved(GuestAddr desc, GuestAddr out) {
        h->write_ptr(out, 1);
        return h->com(dev, DeviceSlot::CreateReservedResource,
                      {{desc},
                       {D3D12_RESOURCE_STATE_COMMON},
                       {0},
                       {h->iid(IID_ID3D12RESOURCE)},
                       {out}});
    }

    /// One D3D12_SUBRESOURCE_TILING (SDK: UINT, UINT16, UINT16, UINT; 12 bytes
    /// on both bitnesses).
    struct TileSub {
        u32 w;
        u32 h;
        u32 d;
        u32 start;
    };

    /// GetResourceTiling through the device vtable. Expected numbers are the
    /// a02 resource (dimensions, mip count, format) described with the tile
    /// shape and the no-packed-mip answer WARP measured: every [tiling] line
    /// of d3d12_layout_probe has NumPackedMips 0, and a mip smaller than the
    /// tile is still one tile (rgba8_64, bc1_1024).
    void expect_tiling(const char* label, GuestAddr resource, u32 request, u32 first,
                       u32 tiles, u32 standard_mips, u32 packed_mips, u32 packed_tiles,
                       u32 packed_start, u32 shape_w, u32 shape_h, u32 shape_d,
                       const std::vector<TileSub>& subs) {
        constexpr GuestAddr kTiles = kData + 0x2100;
        constexpr GuestAddr kPacked = kData + 0x2110;
        constexpr GuestAddr kShape = kData + 0x2120;
        constexpr GuestAddr kCount = kData + 0x2130;
        constexpr GuestAddr kSubs = kData + 0x2140;
        for (u32 i = 0; i < 0x200; i += 4) h->w32(kTiles + i, 0xCDCDCDCDu);
        h->w32(kCount, request);
        h->com(dev, DeviceSlot::GetResourceTiling,
               {{resource},
                {kTiles},
                {kPacked},
                {kShape},
                {kCount},
                {first},
                {kSubs}});
        EXPECT_EQ(h->r32(kTiles), tiles) << label;
        EXPECT_EQ(h->memory.read_u8(kPacked).value(), standard_mips) << label;
        EXPECT_EQ(h->memory.read_u8(kPacked + 1).value(), packed_mips) << label;
        // Bytes 2..3 of D3D12_PACKED_MIP_INFO are padding, not a field.
        EXPECT_EQ(h->memory.read_u16(kPacked + 2).value(), 0xCDCDu) << label;
        EXPECT_EQ(h->r32(kPacked + 4), packed_tiles) << label;
        EXPECT_EQ(h->r32(kPacked + 8), packed_start) << label;
        EXPECT_EQ(h->r32(kShape), shape_w) << label;
        EXPECT_EQ(h->r32(kShape + 4), shape_h) << label;
        EXPECT_EQ(h->r32(kShape + 8), shape_d) << label;
        EXPECT_EQ(h->r32(kCount), static_cast<u32>(subs.size())) << label;
        for (u32 i = 0; i < subs.size(); ++i) {
            const GuestAddr e = kSubs + static_cast<u64>(i) * SUBRESOURCE_TILING_SIZE;
            EXPECT_EQ(h->r32(e), subs[i].w) << label << " [" << i << "]";
            EXPECT_EQ(h->memory.read_u16(e + 4).value(), static_cast<u16>(subs[i].h))
                << label;
            EXPECT_EQ(h->memory.read_u16(e + 6).value(), static_cast<u16>(subs[i].d))
                << label;
            EXPECT_EQ(h->r32(e + 8), subs[i].start) << label;
        }
        if (subs.size() < 16) {
            const GuestAddr e =
                kSubs + static_cast<u64>(subs.size()) * SUBRESOURCE_TILING_SIZE;
            EXPECT_EQ(h->r32(e), 0xCDCDCDCDu) << label << " past the report";
        }
    }

    GuestAddr dev{0};
    std::unique_ptr<Harness> h;
};

INSTANTIATE_TEST_SUITE_P(Bitness, D3D12TiledTest, ::testing::Values(true, false),
                         [](const auto& info) {
                             return info.param ? "x64" : "x86";
                         });

TEST_P(D3D12TiledTest, CreateReservedResourceMatchesTheProbe) {
    dev = h->device();
    // Tier 2 on a backend with tiled resources: tiling, mappings (packed
    // tails included), CopyTiles and the shader status instructions are
    // complete. Tier 3 needs the standard 3D tile.
    const auto& opt = layout::D3D12_FEATURE_DATA_D3D12_OPTIONS[GetParam()];
    clear_desc(kDesc, opt.size);
    EXPECT_EQ(h->com(dev, DeviceSlot::CheckFeatureSupport,
                     {{D3D12_FEATURE_D3D12_OPTIONS}, {kDesc}, {opt.size}}),
              S_OK);
    EXPECT_EQ(h->r32(kDesc + opt.TiledResourcesTier), 2u);

    const auto& rd = layout::D3D12_RESOURCE_DESC[GetParam()];
    // a02 "reserved buffer 64K" / "1 byte": S_OK, alignment 65536, width kept.
    buffer_desc(kDesc2, 65536);
    ASSERT_EQ(create_reserved(kDesc2, kOut), S_OK);
    const GuestAddr buffer64 = h->read_ptr(kOut);
    EXPECT_NE(buffer64, 0u);
    EXPECT_EQ(h->com(buffer64, ResourceSlot::GetDesc, {{kDesc}}),
              GetParam() ? kDesc : static_cast<u32>(kDesc));
    EXPECT_EQ(h->r64(kDesc + rd.Alignment), 65536u);
    EXPECT_EQ(h->r64(kDesc + rd.Width), 65536u);
    EXPECT_EQ(h->r32(kDesc + rd.Layout), D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
    // a02 GetGPUVirtualAddress: nonzero and 64 KiB-aligned.
    const u64 va = h->com(buffer64, ResourceSlot::GetGPUVirtualAddress, {}, true);
    EXPECT_NE(va, 0u);
    EXPECT_EQ(va % 65536u, 0u);
    // a02 reserved Map: E_INVALIDARG, ptr NULL (a DEFAULT buffer leaves it).
    h->write_ptr(kOut2, 0x1234);
    EXPECT_EQ(h->com(buffer64, ResourceSlot::Map, {{0}, {0}, {kOut2}}),
              E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kOut2), 0u);
    // a02 GetHeapProperties: E_INVALIDARG, outputs untouched.
    h->w32(kDesc, 0xAAAAAAAAu);
    h->w32(kOut2, 0xdeadu);
    EXPECT_EQ(h->com(buffer64, ResourceSlot::GetHeapProperties, {{kDesc}, {kOut2}}),
              E_INVALIDARG);
    EXPECT_EQ(h->r32(kDesc + layout::D3D12_HEAP_PROPERTIES[GetParam()].Type),
              0xAAAAAAAAu);
    EXPECT_EQ(h->r32(kOut2), 0xdeadu);

    buffer_desc(kDesc2, 1);
    ASSERT_EQ(create_reserved(kDesc2, kOut), S_OK);
    EXPECT_EQ(h->com(h->read_ptr(kOut), ResourceSlot::GetDesc, {{kDesc}}),
              GetParam() ? kDesc : static_cast<u32>(kDesc));
    EXPECT_EQ(h->r64(kDesc + rd.Width), 1u);
    EXPECT_EQ(h->r64(kDesc + rd.Alignment), 65536u);

    // a02 "reserved buffer 64 GiB": E_OUTOFMEMORY, no object.
    buffer_desc(kDesc2, 1ull << 36);
    EXPECT_EQ(create_reserved(kDesc2, kOut), E_OUTOFMEMORY);
    EXPECT_EQ(h->read_ptr(kOut), 0u);

    // Learn: ppvResource null is S_FALSE when the description is valid.
    buffer_desc(kDesc2, 65536);
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateReservedResource,
                     {{kDesc2},
                      {D3D12_RESOURCE_STATE_COMMON},
                      {0},
                      {h->iid(IID_ID3D12RESOURCE)},
                      {0}}),
              S_FALSE);

    // a02 texture rows.
    texture_desc(kDesc2, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 256, 256, 1, 1,
                 DXGI_FORMAT_R8G8B8A8_UNORM, 1,
                 D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE, 0);
    ASSERT_EQ(create_reserved(kDesc2, kOut), S_OK);
    EXPECT_EQ(h->com(h->read_ptr(kOut), ResourceSlot::GetDesc, {{kDesc}}),
              GetParam() ? kDesc : static_cast<u32>(kDesc));
    EXPECT_EQ(h->r64(kDesc + rd.Alignment), 65536u);
    EXPECT_EQ(h->r32(kDesc + rd.Layout), D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE);
    EXPECT_EQ(h->memory.read_u16(kDesc + rd.MipLevels).value(), 1u);

    texture_desc(kDesc2, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 256, 256, 1, 0,
                 DXGI_FORMAT_R8G8B8A8_UNORM, 1,
                 D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE, 0);
    ASSERT_EQ(create_reserved(kDesc2, kOut), S_OK);
    EXPECT_EQ(h->com(h->read_ptr(kOut), ResourceSlot::GetDesc, {{kDesc}}),
              GetParam() ? kDesc : static_cast<u32>(kDesc));
    EXPECT_EQ(h->memory.read_u16(kDesc + rd.MipLevels).value(), 9u);

    texture_desc(kDesc2, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 256, 256, 1, 1,
                 DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_TEXTURE_LAYOUT_UNKNOWN, 0);
    EXPECT_EQ(create_reserved(kDesc2, kOut), E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kOut), 0u);

    texture_desc(kDesc2, D3D12_RESOURCE_DIMENSION_TEXTURE1D, 4096, 1, 1, 1,
                 DXGI_FORMAT_R8G8B8A8_UNORM, 1,
                 D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE, 0);
    EXPECT_EQ(create_reserved(kDesc2, kOut), E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kOut), 0u);

    // a02 array=4 RT and the tier-1 refusal of an array mip that is not a
    // whole tile (64 of a 128 tile). Volumes are tier 3 (a02 succeeds on
    // WARP; this device does not implement them).
    texture_desc(kDesc2, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 256, 256, 4, 1,
                 DXGI_FORMAT_R8G8B8A8_UNORM, 1,
                 D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE,
                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    EXPECT_EQ(create_reserved(kDesc2, kOut), S_OK);
    texture_desc(kDesc2, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 256, 256, 2, 3,
                 DXGI_FORMAT_R8G8B8A8_UNORM, 1,
                 D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE, 0);
    EXPECT_EQ(create_reserved(kDesc2, kOut), E_INVALIDARG);
    texture_desc(kDesc2, D3D12_RESOURCE_DIMENSION_TEXTURE3D, 64, 64, 16, 1,
                 DXGI_FORMAT_R8G8B8A8_UNORM, 1,
                 D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE, 0);
    EXPECT_EQ(create_reserved(kDesc2, kOut), E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kOut), 0u);
}

TEST_P(D3D12TiledTest, GetResourceTilingReportsReservedBuffersAndTexture2D) {
    dev = h->device();
    // a02 "reserved buffer 64K" / "1 byte" / UAV. Probe buf1 (width 1) and
    // the 64 KiB tile: ceil(width / 65536) tiles, packed 0,0,0,0, one
    // subresource. A buffer reports no standard mip count.
    buffer_desc(kDesc2, 65536);
    ASSERT_EQ(create_reserved(kDesc2, kOut), S_OK);
    const GuestAddr buffer64 = h->read_ptr(kOut);
    expect_tiling("buffer 64K", buffer64, 40, 0, 1, 0, 0, 0, 0, 65536, 1, 1,
                  {{1, 1, 1, 0}});
    // pNumTilesForEntireResource is optional; the shape is still written.
    constexpr GuestAddr kShape = kData + 0x2120;
    h->w32(kShape, 0xCDCDCDCDu);
    h->com(dev, DeviceSlot::GetResourceTiling,
           {{buffer64}, {0}, {0}, {kShape}, {0}, {0}, {0}});
    EXPECT_EQ(h->r32(kShape), 65536u);
    EXPECT_EQ(h->r32(kShape + 4), 1u);
    EXPECT_EQ(h->r32(kShape + 8), 1u);

    buffer_desc(kDesc2, 1);
    ASSERT_EQ(create_reserved(kDesc2, kOut), S_OK);
    expect_tiling("buffer 1 byte", h->read_ptr(kOut), 40, 0, 1, 0, 0, 0, 0, 65536, 1,
                  1, {{1, 1, 1, 0}});
    buffer_desc(kDesc2, 65536, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ASSERT_EQ(create_reserved(kDesc2, kOut), S_OK);
    expect_tiling("buffer UAV", h->read_ptr(kOut), 40, 0, 1, 0, 0, 0, 0, 65536, 1, 1,
                  {{1, 1, 1, 0}});

    // a02 "reserved 2D 256x256" mips=1, RGBA8. Shape 128x128x1 (probe
    // rgba8_128_mip1 / rgba8_1024). 256/128 = 2, so 2x2 = 4 tiles, one
    // standard mip, nothing packed.
    texture_desc(kDesc2, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 256, 256, 1, 1,
                 DXGI_FORMAT_R8G8B8A8_UNORM, 1,
                 D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE, 0);
    ASSERT_EQ(create_reserved(kDesc2, kOut), S_OK);
    expect_tiling("rgba8 256 mip1", h->read_ptr(kOut), 40, 0, 4, 1, 0, 0, 0, 128, 128,
                  1, {{2, 2, 1, 0}});

    // a02 mips=0 is stored as 9. Same shape. WARP packs nothing (12 tiles,
    // NumStandardMips 9); packing is the implementation's (D3D11.3
    // 5.9.2.8.5) and this backend packs the mips under one tile into one
    // tail tile (the fake mirrors Metal's firstMipmapInTail 2 /
    // tailSizeInBytes 65536, test_tiled_resources_gpu). Spec rules checked:
    // mip0 (2x2) and mip1 (one full tile) stay standard, the tail follows
    // the standard tiles, and a packed subresource reads D3D12_PACKED_TILE
    // with zero extents (Learn D3D12_SUBRESOURCE_TILING). 4+1+1 = 6.
    texture_desc(kDesc2, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 256, 256, 1, 0,
                 DXGI_FORMAT_R8G8B8A8_UNORM, 1,
                 D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE, 0);
    ASSERT_EQ(create_reserved(kDesc2, kOut), S_OK);
    const GuestAddr chain = h->read_ptr(kOut);
    constexpr TileSub kPacked{0, 0, 0, PACKED_TILE};
    expect_tiling("rgba8 256 chain", chain, 40, 0, 6, 2, 7, 1, 5, 128, 128, 1,
                  {{2, 2, 1, 0},
                   {1, 1, 1, 4},
                   kPacked,
                   kPacked,
                   kPacked,
                   kPacked,
                   kPacked,
                   kPacked,
                   kPacked});
    // A window of the chain: first mip 1, two subresources.
    expect_tiling("rgba8 256 chain window", chain, 2, 1, 6, 2, 7, 1, 5, 128, 128, 1,
                  {{1, 1, 1, 4}, kPacked});

    // a02 array=4, one mip, whole tiles. Per-slice stride is the mip's tile
    // count (probe first99: start 828 = 9 slices * 92 tiles).
    texture_desc(kDesc2, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 256, 256, 4, 1,
                 DXGI_FORMAT_R8G8B8A8_UNORM, 1,
                 D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE,
                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    ASSERT_EQ(create_reserved(kDesc2, kOut), S_OK);
    expect_tiling("rgba8 array4", h->read_ptr(kOut), 40, 0, 16, 1, 0, 0, 0, 128, 128, 1,
                  {{2, 2, 1, 0}, {2, 2, 1, 4}, {2, 2, 1, 8}, {2, 2, 1, 12}});

    // a02 D32F. Probe d32_512 shape 128x128x1 (4-byte element). 256 is 2x2.
    texture_desc(kDesc2, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 256, 256, 1, 1,
                 DXGI_FORMAT_D32_FLOAT, 1, D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE,
                 D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    ASSERT_EQ(create_reserved(kDesc2, kOut), S_OK);
    expect_tiling("d32 256", h->read_ptr(kOut), 40, 0, 4, 1, 0, 0, 0, 128, 128, 1,
                  {{2, 2, 1, 0}});

    // a02 BC1 512 mips=0, desc-after mips=10. Probe bc1_1024 shape 512x256x1.
    // mip0 is 1x2 tiles (WARP then gives each later mip a tile, 11 in all).
    // Here mip1 (256 wide) is under the 512 tile and starts the one-tile
    // tail: 2+1 = 3, NumStandardMips 1, nine packed.
    texture_desc(kDesc2, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 512, 512, 1, 0,
                 DXGI_FORMAT_BC1_UNORM, 1, D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE,
                 0);
    ASSERT_EQ(create_reserved(kDesc2, kOut), S_OK);
    expect_tiling("bc1 512 chain", h->read_ptr(kOut), 40, 0, 3, 1, 9, 1, 2, 512, 256,
                  1,
                  {{1, 2, 1, 0},
                   kPacked,
                   kPacked,
                   kPacked,
                   kPacked,
                   kPacked,
                   kPacked,
                   kPacked,
                   kPacked,
                   kPacked});

    // a02 "x4 RT". Probe ms4_512 shape 64x64x1. 256/64 = 4, so 16 tiles.
    texture_desc(kDesc2, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 256, 256, 1, 1,
                 DXGI_FORMAT_R8G8B8A8_UNORM, 4,
                 D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE,
                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    ASSERT_EQ(create_reserved(kDesc2, kOut), S_OK);
    expect_tiling("rgba8 256 x4", h->read_ptr(kOut), 40, 0, 16, 1, 0, 0, 0, 64, 64, 1,
                  {{4, 4, 1, 0}});

    // A committed resource is not tiled (probe "rt committed": all zero, and
    // the subresource array is left untouched).
    GuestAddr committed = 0;
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 65536, D3D12_RESOURCE_STATE_COMMON,
                        committed),
              S_OK);
    expect_tiling("committed buffer", committed, 40, 0, 0, 0, 0, 0, 0, 0, 0, 0, {});

    // a02 volume and NV12 are not resources this device tiles.
    texture_desc(kDesc2, D3D12_RESOURCE_DIMENSION_TEXTURE3D, 64, 64, 16, 1,
                 DXGI_FORMAT_R8G8B8A8_UNORM, 1,
                 D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE, 0);
    EXPECT_EQ(create_reserved(kDesc2, kOut), E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kOut), 0u);
    texture_desc(kDesc2, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 256, 256, 1, 1,
                 DXGI_FORMAT_NV12, 1, D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE, 0);
    EXPECT_EQ(create_reserved(kDesc2, kOut), E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kOut), 0u);
}

TEST_P(D3D12TiledTest, UpdateAndCopyTileMappingsUseTheTilePool) {
    dev = h->device();
    const GuestAddr queue = h->queue(dev);
    texture_desc(kDesc2, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 256, 256, 1, 1,
                 DXGI_FORMAT_R8G8B8A8_UNORM, 1,
                 D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE, 0);
    ASSERT_EQ(create_reserved(kDesc2, kOut), S_OK);
    const GuestAddr tiled = h->read_ptr(kOut);
    ASSERT_EQ(create_reserved(kDesc2, kOut), S_OK);
    const GuestAddr tiled2 = h->read_ptr(kOut);

    const auto& hd = layout::D3D12_HEAP_DESC[GetParam()];
    clear_desc(kDesc, hd.size);
    h->w64(kDesc + hd.SizeInBytes, 4u * 65536u);
    h->w32(kDesc + hd.Properties_Type, D3D12_HEAP_TYPE_DEFAULT);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateHeap,
                     {{kDesc}, {h->iid(IID_ID3D12HEAP)}, {kOut}}),
              S_OK);
    const GuestAddr heap = h->read_ptr(kOut);

    // One tile at (0, 0) -> heap tile 2. RGBA8's standard tile is 128, so a
    // 256 texture is 2x2 tiles (Learn UpdateTileMappings, sequential range).
    constexpr GuestAddr kCoord = kData + 0x1800;
    constexpr GuestAddr kRegion = kData + 0x1840;
    constexpr GuestAddr kFlag = kData + 0x1880;
    constexpr GuestAddr kOffset = kData + 0x18C0;
    h->w32(kCoord + 0, 0);
    h->w32(kCoord + 4, 0);
    h->w32(kCoord + 8, 0);
    h->w32(kCoord + 12, 0);
    h->w32(kRegion + 0, 1);
    h->w32(kRegion + 4, 1);
    h->w32(kRegion + 8, 1);
    h->w32(kRegion + 12, 1u | (1u << 16));
    h->w32(kFlag, D3D12_TILE_RANGE_FLAG_NONE);
    h->w32(kOffset, 2);
    h->backend.tile_calls.clear();
    EXPECT_EQ(h->com(queue, CommandQueueSlot::UpdateTileMappings,
                     {{tiled},
                      {1},
                      {kCoord},
                      {kRegion},
                      {heap},
                      {1},
                      {kFlag},
                      {kOffset},
                      {0},
                      {D3D12_TILE_MAPPING_FLAG_NONE}}),
              0u);
    ASSERT_EQ(h->backend.tile_calls.size(), 1u);
    EXPECT_TRUE(h->backend.tile_calls[0].texture);
    EXPECT_TRUE(h->backend.tile_calls[0].has_pool);
    EXPECT_FALSE(h->backend.tile_calls[0].self_aliased);
    ASSERT_EQ(h->backend.tile_calls[0].mappings.size(), 1u);
    EXPECT_EQ(h->backend.tile_calls[0].mappings[0].tile.x, 0u);
    EXPECT_EQ(h->backend.tile_calls[0].mappings[0].tile.y, 0u);
    EXPECT_EQ(h->backend.tile_calls[0].mappings[0].pool_tile, 2u);

    // A 2x1 box reused onto heap tile 2 aliases with the tile already there.
    h->w32(kRegion + 0, 2);
    h->w32(kRegion + 8, 2);
    h->w32(kFlag, D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE);
    h->backend.tile_calls.clear();
    EXPECT_EQ(h->com(queue, CommandQueueSlot::UpdateTileMappings,
                     {{tiled},
                      {1},
                      {kCoord},
                      {kRegion},
                      {heap},
                      {1},
                      {kFlag},
                      {kOffset},
                      {0},
                      {D3D12_TILE_MAPPING_FLAG_NONE}}),
              0u);
    ASSERT_EQ(h->backend.tile_calls.size(), 1u);
    EXPECT_TRUE(h->backend.tile_calls[0].self_aliased);
    ASSERT_EQ(h->backend.tile_calls[0].mappings.size(), 1u);
    EXPECT_EQ(h->backend.tile_calls[0].mappings[0].tile.x, 1u);
    EXPECT_EQ(h->backend.tile_calls[0].mappings[0].tile.y, 0u);
    EXPECT_EQ(h->backend.tile_calls[0].mappings[0].pool_tile, 2u);

    // Copy the 2x1 at (0, 0) onto (0, 1) of the other resource. Learn: the
    // source is read before any destination write.
    h->w32(kCoord + 0, 0);
    h->w32(kCoord + 4, 1);  // destination Y = 1; source coordinate follows
    constexpr GuestAddr kSrcCoord = kData + 0x1900;
    h->w32(kSrcCoord + 0, 0);
    h->w32(kSrcCoord + 4, 0);
    h->w32(kSrcCoord + 8, 0);
    h->w32(kSrcCoord + 12, 0);
    h->w32(kRegion + 0, 2);
    h->w32(kRegion + 4, 1);
    h->w32(kRegion + 8, 2);
    h->w32(kRegion + 12, 1u | (1u << 16));
    h->backend.tile_calls.clear();
    EXPECT_EQ(h->com(queue, CommandQueueSlot::CopyTileMappings,
                     {{tiled2},
                      {kCoord},
                      {tiled},
                      {kSrcCoord},
                      {kRegion},
                      {D3D12_TILE_MAPPING_FLAG_NONE}}),
              0u);
    ASSERT_EQ(h->backend.tile_calls.size(), 1u);
    EXPECT_TRUE(h->backend.tile_calls[0].has_pool);
    EXPECT_TRUE(h->backend.tile_calls[0].self_aliased);
    ASSERT_EQ(h->backend.tile_calls[0].mappings.size(), 2u);
    EXPECT_EQ(h->backend.tile_calls[0].mappings[0].tile.x, 0u);
    EXPECT_EQ(h->backend.tile_calls[0].mappings[0].tile.y, 1u);
    EXPECT_EQ(h->backend.tile_calls[0].mappings[1].tile.x, 1u);
    EXPECT_EQ(h->backend.tile_calls[0].mappings[1].tile.y, 1u);
    EXPECT_EQ(h->backend.tile_calls[0].mappings[0].pool_tile, 2u);
    EXPECT_EQ(h->backend.tile_calls[0].mappings[1].pool_tile, 2u);

    // Learn's "clear an entire surface": NULL heap, NULL range, one region.
    h->w32(kFlag, D3D12_TILE_RANGE_FLAG_NULL);
    h->backend.tile_calls.clear();
    EXPECT_EQ(h->com(queue, CommandQueueSlot::UpdateTileMappings,
                     {{tiled2},
                      {1},
                      {0},
                      {0},
                      {0},
                      {1},
                      {kFlag},
                      {0},
                      {0},
                      {D3D12_TILE_MAPPING_FLAG_NONE}}),
              0u);
    ASSERT_EQ(h->backend.tile_calls.size(), 1u);
    EXPECT_FALSE(h->backend.tile_calls[0].has_pool);
    ASSERT_EQ(h->backend.tile_calls[0].mappings.size(), 2u);
    EXPECT_EQ(h->backend.tile_calls[0].mappings[0].pool_tile,
              graphics::BACKEND_TILE_UNMAPPED);

    // A bad flag and a non-reserved resource change nothing.
    const usize before = h->backend.tile_calls.size();
    EXPECT_EQ(h->com(queue, CommandQueueSlot::UpdateTileMappings,
                     {{tiled},
                      {1},
                      {0},
                      {0},
                      {0},
                      {1},
                      {kFlag},
                      {0},
                      {0},
                      {0x10}}),
              0u);
    GuestAddr committed = 0;
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 65536,
                        D3D12_RESOURCE_STATE_COMMON, committed),
              S_OK);
    EXPECT_EQ(h->com(queue, CommandQueueSlot::UpdateTileMappings,
                     {{committed},
                      {1},
                      {0},
                      {0},
                      {0},
                      {1},
                      {kFlag},
                      {0},
                      {0},
                      {D3D12_TILE_MAPPING_FLAG_NONE}}),
              0u);
    EXPECT_EQ(h->backend.tile_calls.size(), before);
    EXPECT_EQ(h->com(dev, DeviceSlot::GetDeviceRemovedReason, {}), S_OK);
}

TEST_P(D3D12TiledTest, PackedTailIsReportedAndMapped) {
    dev = h->device();
    const auto& opt = layout::D3D12_FEATURE_DATA_D3D12_OPTIONS[GetParam()];
    clear_desc(kDesc, opt.size);
    ASSERT_EQ(h->com(dev, DeviceSlot::CheckFeatureSupport,
                     {{D3D12_FEATURE_D3D12_OPTIONS}, {kDesc}, {opt.size}}),
              S_OK);
    EXPECT_EQ(h->r32(kDesc + opt.TiledResourcesTier), 2u);

    // a02 full chain. The backend packs the mips under one 128 tile (mip 2
    // onward) into one tile, which GetResourceTiling reports: tiles 0-3 are
    // mip0, 4 is mip1, 5 is the packed tail (NumStandardMips 2,
    // NumPackedMips 7, StartTileIndexInOverallResource 5).
    texture_desc(kDesc2, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 256, 256, 1, 0,
                 DXGI_FORMAT_R8G8B8A8_UNORM, 1,
                 D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE, 0);
    ASSERT_EQ(create_reserved(kDesc2, kOut), S_OK);
    const GuestAddr chain = h->read_ptr(kOut);
    constexpr TileSub kPacked{0, 0, 0, PACKED_TILE};
    expect_tiling("rgba8 256 chain", chain, 3, 0, 6, 2, 7, 1, 5, 128, 128, 1,
                  {{2, 2, 1, 0}, {1, 1, 1, 4}, kPacked});

    const GuestAddr queue = h->queue(dev);
    const auto& hd = layout::D3D12_HEAP_DESC[GetParam()];
    clear_desc(kDesc, hd.size);
    h->w64(kDesc + hd.SizeInBytes, 16u * 65536u);
    h->w32(kDesc + hd.Properties_Type, D3D12_HEAP_TYPE_DEFAULT);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateHeap,
                     {{kDesc}, {h->iid(IID_ID3D12HEAP)}, {kOut}}),
              S_OK);
    const GuestAddr heap = h->read_ptr(kOut);

    constexpr GuestAddr kCoord = kData + 0x1800;
    constexpr GuestAddr kRegion = kData + 0x1840;
    constexpr GuestAddr kFlag = kData + 0x1880;
    constexpr GuestAddr kOffset = kData + 0x18C0;
    auto write_at = [&](u32 x, u32 y, u32 sub) {
        h->w32(kCoord + 0, x);
        h->w32(kCoord + 4, y);
        h->w32(kCoord + 8, 0);
        h->w32(kCoord + 12, sub);
    };
    auto write_box = [&](u32 tiles, u32 w, u32 height, u32 depth) {
        h->w32(kRegion + 0, tiles);
        h->w32(kRegion + 4, 1);
        h->w32(kRegion + 8, w);
        h->w32(kRegion + 12, height | (depth << 16));
    };
    auto write_run = [&](u32 tiles) {
        h->w32(kRegion + 0, tiles);
        h->w32(kRegion + 4, 0);
        h->w32(kRegion + 8, 0);
        h->w32(kRegion + 12, 0);
    };
    auto map = [&](u32 heap_tile) {
        h->w32(kFlag, D3D12_TILE_RANGE_FLAG_NONE);
        h->w32(kOffset, heap_tile);
        EXPECT_EQ(h->com(queue, CommandQueueSlot::UpdateTileMappings,
                         {{chain},
                          {1},
                          {kCoord},
                          {kRegion},
                          {heap},
                          {1},
                          {kFlag},
                          {kOffset},
                          {0},
                          {D3D12_TILE_MAPPING_FLAG_NONE}}),
                  0u);
    };

    h->backend.tile_calls.clear();
    write_at(0, 0, 0);
    write_box(4, 2, 2, 1);
    map(0);
    ASSERT_EQ(h->backend.tile_calls.size(), 1u);
    ASSERT_EQ(h->backend.tile_calls[0].mappings.size(), 4u);
    const u32 xs[4] = {0, 1, 0, 1};
    const u32 ys[4] = {0, 0, 1, 1};
    for (u32 i = 0; i < 4; ++i) {
        const auto& tile = h->backend.tile_calls[0].mappings[i].tile;
        EXPECT_FALSE(tile.packed) << i;
        EXPECT_EQ(tile.level, 0u) << i;
        EXPECT_EQ(tile.slice, 0u) << i;
        EXPECT_EQ(tile.x, xs[i]) << i;
        EXPECT_EQ(tile.y, ys[i]) << i;
        EXPECT_EQ(h->backend.tile_calls[0].mappings[i].pool_tile, static_cast<u64>(i));
    }

    h->backend.tile_calls.clear();
    write_at(0, 0, 1);
    write_box(1, 1, 1, 1);
    map(4);
    ASSERT_EQ(h->backend.tile_calls.size(), 1u);
    ASSERT_EQ(h->backend.tile_calls[0].mappings.size(), 1u);
    EXPECT_FALSE(h->backend.tile_calls[0].mappings[0].tile.packed);
    EXPECT_EQ(h->backend.tile_calls[0].mappings[0].tile.level, 1u);
    EXPECT_EQ(h->backend.tile_calls[0].mappings[0].tile.x, 0u);
    EXPECT_EQ(h->backend.tile_calls[0].mappings[0].tile.y, 0u);
    EXPECT_EQ(h->backend.tile_calls[0].mappings[0].pool_tile, 4u);

    // Learn D3D12_TILED_RESOURCE_COORDINATE: any packed mip's subresource
    // names the same packed tiles, X is the tile within them. Mip 2 maps the
    // tail's tile 0; mip 3 names that same tile.
    h->backend.tile_calls.clear();
    write_at(0, 0, 2);
    write_run(1);
    map(5);
    ASSERT_EQ(h->backend.tile_calls.size(), 1u);
    ASSERT_EQ(h->backend.tile_calls[0].mappings.size(), 1u);
    {
        const auto& tile = h->backend.tile_calls[0].mappings[0].tile;
        EXPECT_TRUE(tile.packed);
        EXPECT_EQ(tile.slice, 0u);
        EXPECT_EQ(tile.x, 0u);
        EXPECT_EQ(h->backend.tile_calls[0].mappings[0].pool_tile, 5u);
    }
    h->backend.tile_calls.clear();
    write_at(0, 0, 3);
    write_box(1, 1, 1, 1);
    map(7);
    ASSERT_EQ(h->backend.tile_calls.size(), 1u);
    ASSERT_EQ(h->backend.tile_calls[0].mappings.size(), 1u);
    EXPECT_TRUE(h->backend.tile_calls[0].mappings[0].tile.packed);
    EXPECT_EQ(h->backend.tile_calls[0].mappings[0].tile.x, 0u);
    EXPECT_EQ(h->backend.tile_calls[0].mappings[0].pool_tile, 7u);
    // Y and Z must be 0 for a packed coordinate, X inside the tail; a box
    // of the tail spans X only. None of these reach the backend.
    for (const u32 bad : {0u, 1u, 2u}) {
        const usize before = h->backend.tile_calls.size();
        if (bad == 0) {
            write_at(0, 1, 2);
            write_run(1);
        } else if (bad == 1) {
            write_at(1, 0, 2);
            write_run(1);
        } else {
            write_at(0, 0, 2);
            write_box(2, 1, 2, 1);
        }
        map(8);
        EXPECT_EQ(h->backend.tile_calls.size(), before) << bad;
    }
    EXPECT_EQ(h->com(dev, DeviceSlot::GetDeviceRemovedReason, {}), S_OK);

    GuestAddr linear = 0;
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 65536,
                        D3D12_RESOURCE_STATE_GENERIC_READ, linear),
              S_OK);
    constexpr u32 kToLinear =
        D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER;
    auto open_list = [&]() {
        const GuestAddr alloc = h->allocator(dev);
        GuestAddr opened = 0;
        EXPECT_EQ(h->try_list(dev, alloc, opened), S_OK);
        return opened;
    };
    auto copy_one = [&](GuestAddr list) {
        (void)h->com(list, GraphicsCommandListSlot::CopyTiles,
                     {{chain},
                      {kCoord},
                      {kRegion},
                      {linear},
                      Harness::wide(0),
                      {kToLinear}});
    };
    h->backend.texture_tile_copies.clear();
    GuestAddr list = open_list();
    write_at(1, 0, 0);
    write_box(1, 1, 1, 1);
    copy_one(list);
    write_at(0, 0, 1);
    write_box(1, 1, 1, 1);
    copy_one(list);
    ASSERT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    h->write_ptr(kArray, list);
    (void)h->com(queue, CommandQueueSlot::ExecuteCommandLists, {{1}, {kArray}});
    ASSERT_EQ(h->backend.texture_tile_copies.size(), 2u);
    EXPECT_EQ(h->backend.texture_tile_copies[0].level, 0u);
    EXPECT_EQ(h->backend.texture_tile_copies[0].x, 128u);
    EXPECT_EQ(h->backend.texture_tile_copies[0].y, 0u);
    EXPECT_EQ(h->backend.texture_tile_copies[0].width, 128u);
    EXPECT_EQ(h->backend.texture_tile_copies[0].height, 128u);
    EXPECT_EQ(h->backend.texture_tile_copies[0].row_pitch, 512u);
    EXPECT_EQ(h->backend.texture_tile_copies[1].level, 1u);
    EXPECT_EQ(h->backend.texture_tile_copies[1].x, 0u);
    EXPECT_EQ(h->backend.texture_tile_copies[1].y, 0u);
    EXPECT_EQ(h->backend.texture_tile_copies[1].width, 128u);
    EXPECT_EQ(h->backend.texture_tile_copies[1].height, 128u);
    EXPECT_EQ(h->backend.texture_tile_copies[1].row_pitch, 512u);

    // D3D11.3 5.9.2.8.5: CopyTiles cannot reach packed mips, by subresource
    // or by a run from a standard mip into the tail.
    h->backend.texture_tile_copies.clear();
    list = open_list();
    write_at(0, 0, 2);
    write_run(1);
    copy_one(list);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), E_INVALIDARG);
    list = open_list();
    write_at(0, 0, 1);
    write_run(2);
    copy_one(list);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), E_INVALIDARG);
    EXPECT_TRUE(h->backend.texture_tile_copies.empty());

    // The whole resource, NULL: six tiles, the last one the packed tail.
    h->w32(kFlag, D3D12_TILE_RANGE_FLAG_NULL);
    write_at(0, 0, 0);
    write_run(6);
    h->backend.tile_calls.clear();
    EXPECT_EQ(h->com(queue, CommandQueueSlot::UpdateTileMappings,
                     {{chain},
                      {1},
                      {kCoord},
                      {kRegion},
                      {0},
                      {1},
                      {kFlag},
                      {0},
                      {0},
                      {D3D12_TILE_MAPPING_FLAG_NONE}}),
              0u);
    ASSERT_EQ(h->backend.tile_calls.size(), 1u);
    ASSERT_EQ(h->backend.tile_calls[0].mappings.size(), 6u);
    EXPECT_FALSE(h->backend.tile_calls[0].has_pool);
    for (u32 i = 0; i < 6; ++i) {
        const auto& tile = h->backend.tile_calls[0].mappings[i].tile;
        EXPECT_EQ(tile.packed, i == 5) << i;
        if (i < 5) EXPECT_EQ(tile.level, i == 4 ? 1u : 0u) << i;
        EXPECT_EQ(h->backend.tile_calls[0].mappings[i].pool_tile,
                  graphics::BACKEND_TILE_UNMAPPED);
    }
    // Seven would run past the resource.
    write_run(7);
    h->backend.tile_calls.clear();
    (void)h->com(queue, CommandQueueSlot::UpdateTileMappings,
                 {{chain}, {1}, {kCoord}, {kRegion}, {0}, {1}, {kFlag}, {0}, {0},
                  {D3D12_TILE_MAPPING_FLAG_NONE}});
    EXPECT_TRUE(h->backend.tile_calls.empty());

    // A NULL tile is still a valid CopyTiles source (reads zeros).
    list = open_list();
    write_at(0, 0, 1);
    write_box(1, 1, 1, 1);
    copy_one(list);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    EXPECT_EQ(h->com(dev, DeviceSlot::GetDeviceRemovedReason, {}), S_OK);
}

TEST_P(D3D12TiledTest, CopyTilesCopiesMappedBufferAndTextureTiles) {
    dev = h->device();
    const auto& opt = layout::D3D12_FEATURE_DATA_D3D12_OPTIONS[GetParam()];
    clear_desc(kDesc, opt.size);
    ASSERT_EQ(h->com(dev, DeviceSlot::CheckFeatureSupport,
                     {{D3D12_FEATURE_D3D12_OPTIONS}, {kDesc}, {opt.size}}),
              S_OK);
    EXPECT_EQ(h->r32(kDesc + opt.TiledResourcesTier), 2u);

    const GuestAddr queue = h->queue(dev);
    constexpr u64 kTile = 65536;
    buffer_desc(kDesc2, 2 * kTile);
    ASSERT_EQ(create_reserved(kDesc2, kOut), S_OK);
    const GuestAddr reserved = h->read_ptr(kOut);

    const auto& hd = layout::D3D12_HEAP_DESC[GetParam()];
    clear_desc(kDesc, hd.size);
    h->w64(kDesc + hd.SizeInBytes, 4 * kTile);
    h->w32(kDesc + hd.Properties_Type, D3D12_HEAP_TYPE_DEFAULT);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateHeap,
                     {{kDesc}, {h->iid(IID_ID3D12HEAP)}, {kOut}}),
              S_OK);
    const GuestAddr heap = h->read_ptr(kOut);

    GuestAddr upload = 0;
    GuestAddr readback = 0;
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD, kTile + 4,
                        D3D12_RESOURCE_STATE_GENERIC_READ, upload),
              S_OK);
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_READBACK, kTile,
                        D3D12_RESOURCE_STATE_COPY_DEST, readback),
              S_OK);

    constexpr GuestAddr kCoord = kData + 0x1800;
    constexpr GuestAddr kRegion = kData + 0x1840;
    constexpr GuestAddr kFlag = kData + 0x1880;
    constexpr GuestAddr kHeapOff = kData + 0x18C0;
    constexpr GuestAddr kSrcCoord = kData + 0x1900;
    constexpr GuestAddr kBox = kData + 0x1940;
    auto write_coord = [&](GuestAddr at, u32 x, u32 y) {
        h->w32(at + 0, x);
        h->w32(at + 4, y);
        h->w32(at + 8, 0);
        h->w32(at + 12, 0);
    };
    auto write_box = [&](GuestAddr at, u32 tiles, u32 w, u32 height, u32 depth) {
        h->w32(at + 0, tiles);
        h->w32(at + 4, 1);
        h->w32(at + 8, w);
        h->w32(at + 12, height | (depth << 16));
    };
    auto write_run = [&](GuestAddr at, u32 tiles) {
        h->w32(at + 0, tiles);
        h->w32(at + 4, 0);
        h->w32(at + 8, 0);
        h->w32(at + 12, 0);
    };
    auto map_region = [&](GuestAddr resource, GuestAddr coord, GuestAddr region,
                          u32 heap_tile) {
        h->w32(kFlag, D3D12_TILE_RANGE_FLAG_NONE);
        h->w32(kHeapOff, heap_tile);
        EXPECT_EQ(h->com(queue, CommandQueueSlot::UpdateTileMappings,
                         {{resource},
                          {1},
                          {coord},
                          {region},
                          {heap},
                          {1},
                          {kFlag},
                          {kHeapOff},
                          {0},
                          {D3D12_TILE_MAPPING_FLAG_NONE}}),
                  0u);
    };
    auto open_list = [&]() {
        const GuestAddr alloc = h->allocator(dev);
        GuestAddr opened = 0;
        EXPECT_EQ(h->try_list(dev, alloc, opened), S_OK);
        return opened;
    };
    auto copy_tiles = [&](GuestAddr list, GuestAddr tiled, GuestAddr coord,
                          GuestAddr region, GuestAddr buffer, u64 byte_offset,
                          u32 flags) {
        (void)h->com(list, GraphicsCommandListSlot::CopyTiles,
                     {{tiled},
                      {coord},
                      {region},
                      {buffer},
                      Harness::wide(byte_offset),
                      {flags}});
    };
    constexpr u32 kToTiled =
        D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE;
    constexpr u32 kToLinear =
        D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER;

    // Only buffer tile 1 is mapped. Tile 0 stays NULL.
    write_coord(kCoord, 1, 0);
    write_run(kRegion, 1);
    map_region(reserved, kCoord, kRegion, 0);

    h->backend.stream.clear();
    h->backend.texture_tile_copies.clear();

    // A NULL tile is a valid CopyTiles target: mappings are the queue's and
    // can change before the list runs, and the copy itself reads zeros or
    // drops the write (D3D11.3 5.9.4.1 / 5.9.4.2).
    write_coord(kCoord, 0, 0);
    write_run(kRegion, 1);
    GuestAddr list = open_list();
    copy_tiles(list, reserved, kCoord, kRegion, upload, 0, kToTiled);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);

    list = open_list();
    copy_tiles(list, upload, kCoord, kRegion, readback, 0, kToTiled);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), E_INVALIDARG);

    write_coord(kCoord, 1, 0);
    write_run(kRegion, 1);
    list = open_list();
    copy_tiles(list, reserved, kCoord, kRegion, reserved, 0, kToTiled);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), E_INVALIDARG);

    list = open_list();
    copy_tiles(list, reserved, 0, kRegion, upload, 0, kToTiled);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), E_INVALIDARG);

    for (const u32 flags : {D3D12_TILE_COPY_FLAG_NONE, 1u, 6u, 8u}) {
        list = open_list();
        copy_tiles(list, reserved, kCoord, kRegion, upload, 0, flags);
        EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), E_INVALIDARG)
            << flags;
    }
    // One tile occupies 64 KiB of the linear buffer. Offset 1 overruns a
    // buffer of exactly one tile.
    list = open_list();
    copy_tiles(list, reserved, kCoord, kRegion, readback, 1, kToLinear);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), E_INVALIDARG);
    EXPECT_TRUE(h->backend.stream.empty());

    // Learn: any byte offset; a buffer tile is 64 KiB of bytes. The pattern
    // is written at +4 and must come back from reserved tile 1, including
    // the last dword of that tile.
    ASSERT_EQ(h->com(upload, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr src = h->read_ptr(kOut2);
    for (u32 i = 0; i < 16; ++i) h->w32(src + 4 + i * 4, 0xC0DE0000u + i);
    h->w32(src + 4 + 65532, 0x71u);
    list = open_list();
    copy_tiles(list, reserved, kCoord, kRegion, upload, 4, kToTiled);
    copy_tiles(list, reserved, kCoord, kRegion, readback, 0, kToLinear);
    ASSERT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    EXPECT_TRUE(h->backend.stream.empty());
    h->write_ptr(kArray, list);
    (void)h->com(queue, CommandQueueSlot::ExecuteCommandLists, {{1}, {kArray}});
    EXPECT_EQ(h->backend.stream,
              (std::vector<std::string>{"copy 65536", "copy 65536"}));
    ASSERT_EQ(h->com(readback, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr dst = h->read_ptr(kOut2);
    for (u32 i = 0; i < 16; ++i) {
        EXPECT_EQ(h->r32(dst + i * 4), 0xC0DE0000u + i) << i;
    }
    EXPECT_EQ(h->r32(dst + 65532), 0x71u);

    // RGBA8's standard tile is 128 (the tiling probe). A 200-wide mip's
    // right and bottom edge is 72 texels, and the pitch stays a full tile
    // row (Learn, CopyTiles). The two tiles are 64 KiB apart from offset 4.
    texture_desc(kDesc2, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 200, 200, 1, 1,
                 DXGI_FORMAT_R8G8B8A8_UNORM, 1,
                 D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE, 0);
    ASSERT_EQ(create_reserved(kDesc2, kOut), S_OK);
    const GuestAddr texture = h->read_ptr(kOut);
    write_coord(kCoord, 0, 0);
    write_box(kRegion, 4, 2, 2, 1);
    map_region(texture, kCoord, kRegion, 0);
    GuestAddr linear = 0;
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 4 + 2 * kTile,
                        D3D12_RESOURCE_STATE_GENERIC_READ, linear),
              S_OK);
    write_coord(kSrcCoord, 0, 1);
    write_box(kBox, 2, 2, 1, 1);
    list = open_list();
    copy_tiles(list, texture, kSrcCoord, kBox, linear, 4, kToLinear);
    write_coord(kCoord, 1, 1);
    write_box(kRegion, 1, 1, 1, 1);
    copy_tiles(list, texture, kCoord, kRegion, linear, 0,
               D3D12_TILE_COPY_FLAG_NO_HAZARD | kToTiled);
    ASSERT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    EXPECT_TRUE(h->backend.texture_tile_copies.empty());
    h->write_ptr(kArray, list);
    (void)h->com(queue, CommandQueueSlot::ExecuteCommandLists, {{1}, {kArray}});
    ASSERT_EQ(h->backend.texture_tile_copies.size(), 3u);
    const auto& edge0 = h->backend.texture_tile_copies[0];
    EXPECT_EQ(edge0.texture->desc().width, 200u);
    EXPECT_EQ(edge0.level, 0u);
    EXPECT_EQ(edge0.slice, 0u);
    EXPECT_EQ(edge0.x, 0u);
    EXPECT_EQ(edge0.y, 128u);
    EXPECT_EQ(edge0.width, 128u);
    EXPECT_EQ(edge0.height, 72u);
    EXPECT_EQ(edge0.row_pitch, 512u);
    EXPECT_EQ(edge0.offset, 4u);
    EXPECT_FALSE(edge0.to_texture);
    const auto& edge1 = h->backend.texture_tile_copies[1];
    EXPECT_EQ(edge1.x, 128u);
    EXPECT_EQ(edge1.y, 128u);
    EXPECT_EQ(edge1.width, 72u);
    EXPECT_EQ(edge1.height, 72u);
    EXPECT_EQ(edge1.row_pitch, 512u);
    EXPECT_EQ(edge1.offset, 4u + kTile);
    const auto& back = h->backend.texture_tile_copies[2];
    EXPECT_EQ(back.x, 128u);
    EXPECT_EQ(back.y, 128u);
    EXPECT_EQ(back.width, 72u);
    EXPECT_EQ(back.height, 72u);
    EXPECT_EQ(back.row_pitch, 512u);
    EXPECT_EQ(back.offset, 0u);
    EXPECT_TRUE(back.to_texture);

    // 4x RGBA8 tile is 64 texels (tiling probe). A pixel is its samples in
    // index order, so the row is 64 * 4 * 4 bytes (Learn, CopyTiles).
    texture_desc(kDesc2, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 256, 256, 1, 1,
                 DXGI_FORMAT_R8G8B8A8_UNORM, 4,
                 D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE,
                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    ASSERT_EQ(create_reserved(kDesc2, kOut), S_OK);
    const GuestAddr msaa = h->read_ptr(kOut);
    write_coord(kCoord, 1, 0);
    write_box(kRegion, 1, 1, 1, 1);
    map_region(msaa, kCoord, kRegion, 0);
    GuestAddr msaa_linear = 0;
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT, kTile,
                        D3D12_RESOURCE_STATE_COMMON, msaa_linear),
              S_OK);
    list = open_list();
    copy_tiles(list, msaa, kCoord, kRegion, msaa_linear, 0, kToLinear);
    ASSERT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    h->backend.texture_tile_copies.clear();
    h->write_ptr(kArray, list);
    (void)h->com(queue, CommandQueueSlot::ExecuteCommandLists, {{1}, {kArray}});
    ASSERT_EQ(h->backend.texture_tile_copies.size(), 1u);
    const auto& sample = h->backend.texture_tile_copies[0];
    EXPECT_EQ(sample.texture->desc().sample_count, 4u);
    EXPECT_EQ(sample.x, 64u);
    EXPECT_EQ(sample.y, 0u);
    EXPECT_EQ(sample.width, 64u);
    EXPECT_EQ(sample.height, 64u);
    EXPECT_EQ(sample.row_pitch, 1024u);
    EXPECT_EQ(sample.offset, 0u);
    EXPECT_FALSE(sample.to_texture);
}

// Geometry, hull and domain shaders take the same lowering D3D11 submits:
// a vertex capture, a compute kernel, and a pass-through draw. A stream-output
// description with NumEntries set and a null declaration is E_INVALIDARG
// (Learn, D3D12_STREAM_OUTPUT_DESC). Hull without a domain shader is
// E_INVALIDARG (Learn, both stages or neither).

void push_u32(std::vector<u8>& out, u32 word) {
    for (unsigned byte = 0; byte < 4; ++byte) {
        out.push_back(static_cast<u8>((word >> (byte * 8)) & 0xFFu));
    }
}

struct SigEl {
    const char* name;
    u32 index;
    u32 system_value;
    u32 component_type;
    u32 reg;
    u32 mask;
};

std::vector<u8> signature_chunk(const std::vector<SigEl>& elements) {
    std::vector<u8> data;
    push_u32(data, static_cast<u32>(elements.size()));
    push_u32(data, 8u);
    std::vector<u8> strings;
    const u32 strings_at = 8u + 24u * static_cast<u32>(elements.size());
    for (const auto& e : elements) {
        push_u32(data, strings_at + static_cast<u32>(strings.size()));
        push_u32(data, e.index);
        push_u32(data, e.system_value);
        push_u32(data, e.component_type);
        push_u32(data, e.reg);
        push_u32(data, e.mask | (e.mask << 8));
        for (const char* c = e.name; *c != 0; ++c) strings.push_back(*c);
        strings.push_back(0);
    }
    while (strings.size() % 4 != 0) strings.push_back(0);
    data.insert(data.end(), strings.begin(), strings.end());
    return data;
}

std::vector<u8> dxbc_stage(const std::vector<u32>& tokens,
                           const std::vector<SigEl>& inputs,
                           const std::vector<SigEl>& outputs) {
    std::vector<u8> code;
    for (const u32 token : tokens) push_u32(code, token);
    const std::array<std::pair<u32, std::vector<u8>>, 3> chunks{{
        {0x4E475349u, signature_chunk(inputs)},
        {0x4E47534Fu, signature_chunk(outputs)},
        {0x58454853u, code},
    }};
    u32 at = 32u + 4u * static_cast<u32>(chunks.size());
    std::vector<u32> offsets;
    for (const auto& chunk : chunks) {
        offsets.push_back(at);
        at += 8u + static_cast<u32>(chunk.second.size());
    }
    std::vector<u8> blob;
    push_u32(blob, 0x43425844u);
    for (int i = 0; i < 4; ++i) push_u32(blob, 0u);
    push_u32(blob, 1u);
    push_u32(blob, at);
    push_u32(blob, static_cast<u32>(chunks.size()));
    for (const u32 offset : offsets) push_u32(blob, offset);
    for (const auto& chunk : chunks) {
        push_u32(blob, chunk.first);
        push_u32(blob, static_cast<u32>(chunk.second.size()));
        blob.insert(blob.end(), chunk.second.begin(), chunk.second.end());
    }
    u8 hash[16]{};
    graphics::dxbc_container_checksum(blob.data(), at, hash);
    std::memcpy(blob.data() + 4, hash, sizeof(hash));
    return blob;
}

/// vs_4_0: SV_VertexID in, SV_Position and TEXCOORD0 out. No input layout.
std::vector<u8> point_vertex_shader() {
    const std::vector<u32> tokens{
        0x00010040u, 31u,
        0x04000060u, 0x00101012u, 0u, 6u,
        0x04000067u, 0x001020F2u, 0u, 1u,
        0x03000065u, 0x001020F2u, 1u,
        0x03000065u, 0x00102032u, 2u,
        0x08000036u, 0x001020F2u, 0u,
        0x00004002u, 0u, 0u, 0u, 0x3F800000u,
        0x05000036u, 0x001020F2u, 1u, 0x00101006u, 0u,
        0x05000036u, 0x00102032u, 2u, 0x00101006u, 0u,
        0x0100003Eu,
    };
    return dxbc_stage(tokens, {{"SV_VertexID", 0, 6, 1, 0, 0x1}},
                      {{"SV_Position", 0, 1, 3, 0, 0xF},
                       {"TEXCOORD", 0, 0, 3, 1, 0xF},
                       {"TEXCOORD", 1, 0, 3, 2, 0x3}});
}

/// gs_4_0: one point in, one point out, copying position and TEXCOORD.
std::vector<u8> point_geometry_shader() {
    const std::vector<u32> tokens{
        0x00020040u, 36u,
        0x05000061u, 0x002010F2u, 1u, 0u, 1u,
        0x0400005Fu, 0x002010F2u, 1u, 1u,
        0x0100085Du,
        0x0100085Cu,
        0x04000067u, 0x001020F2u, 0u, 1u,
        0x03000065u, 0x001020F2u, 1u,
        0x0200005Eu, 1u,
        0x06000036u, 0x001020F2u, 0u, 0x00201E46u, 0u, 0u,
        0x06000036u, 0x001020F2u, 1u, 0x00201E46u, 0u, 1u,
        0x01000013u,
        0x0100003Eu,
    };
    return dxbc_stage(
        tokens,
        {{"SV_Position", 0, 1, 3, 0, 0xF}, {"TEXCOORD", 0, 0, 3, 1, 0xF}},
        {{"SV_Position", 0, 1, 3, 0, 0xF}, {"TEXCOORD", 0, 0, 3, 1, 0xF}});
}

GuestAddr make_root(Harness& h, GuestAddr dev, u32 flags, bool constant_buffer = false) {
    RootSignatureDesc desc;
    desc.flags = flags;
    if (constant_buffer) {
        // The tri hull shader reads its tessellation factors from b0.
        RootParameter cbv;
        cbv.parameter_type = D3D12_ROOT_PARAMETER_TYPE_CBV;
        cbv.shader_visibility = D3D12_SHADER_VISIBILITY_ALL;
        cbv.shader_register = 0;
        desc.parameters.push_back(cbv);
    }
    std::vector<u8> blob;
    std::string error;
    EXPECT_EQ(serialize_root_signature(desc, ROOT_SIGNATURE_VERSION_1_0, blob,
                                       error),
              S_OK)
        << error;
    const GuestAddr at = kData + 0x1800;
    EXPECT_TRUE(h.memory.write_bytes(at, blob.data(), blob.size()).is_ok());
    EXPECT_EQ(h.com(dev, DeviceSlot::CreateRootSignature,
                    {{0}, {at}, {blob.size()}, {h.iid(IID_ID3D12ROOTSIGNATURE)},
                     {kOut}}),
              S_OK);
    return h.read_ptr(kOut);
}

void prime_gfx(Harness& h, GuestAddr root, u32 topology) {
    const auto& l = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[h.ptr_size() == 8];
    const std::vector<u8> zero(l.size, 0);
    EXPECT_TRUE(h.memory.write_bytes(kDesc, zero.data(), zero.size()).is_ok());
    h.write_ptr(kDesc + l.pRootSignature, root);
    h.w32(kDesc + l.RasterizerState_FillMode, D3D12_FILL_MODE_SOLID);
    h.w32(kDesc + l.RasterizerState_CullMode, D3D12_CULL_MODE_NONE);
    h.w32(kDesc + l.PrimitiveTopologyType, topology);
    h.w32(kDesc + l.SampleDesc_Count, 1);
}

void put_shader(Harness& h, GuestAddr at, const std::vector<u8>& bytes,
                GuestAddr bytecode_field, GuestAddr pointer_field,
                GuestAddr length_field) {
    EXPECT_TRUE(h.memory.write_bytes(at, bytes.data(), bytes.size()).is_ok());
    h.write_ptr(kDesc + pointer_field, at);
    h.write_ptr(kDesc + length_field, bytes.size());
    (void)bytecode_field;
}

TEST_P(D3D12Test, GeometryShaderDrawsThroughTheLoweredPasses) {
    const GuestAddr dev = h->device();
    const GuestAddr root = make_root(*h, dev, 0);
    prime_gfx(*h, root, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);
    const auto& l = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[GetParam()];
    const std::vector<u8> vs = point_vertex_shader();
    const std::vector<u8> gs = point_geometry_shader();
    put_shader(*h, kData + 0x2000, vs, l.VS, l.VS_pShaderBytecode,
               l.VS_BytecodeLength);
    put_shader(*h, kData + 0x3000, gs, l.GS, l.GS_pShaderBytecode,
               l.GS_BytecodeLength);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK);
    const GuestAddr pso = h->read_ptr(kOut);

    // A hull shader without a domain shader is not a pipeline.
    h->write_ptr(kDesc + l.GS_pShaderBytecode, 0);
    h->write_ptr(kDesc + l.GS_BytecodeLength, 0);
    h->write_ptr(kDesc + l.HS_pShaderBytecode, kData + 0x3000);
    h->write_ptr(kDesc + l.HS_BytecodeLength, gs.size());
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              E_INVALIDARG);

    // NumEntries without pSODeclaration is not a stream-output description.
    h->write_ptr(kDesc + l.HS_pShaderBytecode, 0);
    h->write_ptr(kDesc + l.HS_BytecodeLength, 0);
    h->w32(kDesc + l.StreamOutput_NumEntries, 1);
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              E_INVALIDARG);

    const GuestAddr queue = h->queue(dev);
    const GuestAddr alloc = h->allocator(dev);
    GuestAddr list = 0;
    ASSERT_EQ(h->try_list(dev, alloc, list), S_OK);
    (void)h->com(list, GraphicsCommandListSlot::SetPipelineState, {{pso}});
    (void)h->com(list, GraphicsCommandListSlot::SetGraphicsRootSignature,
                 {{root}});
    (void)h->com(list, GraphicsCommandListSlot::IASetPrimitiveTopology,
                 {{D3D_PRIMITIVE_TOPOLOGY_POINTLIST}});
    const auto& vp = layout::D3D12_VIEWPORT[GetParam()];
    const GuestAddr view = kData + 0x1600;
    const auto put_f = [&](u32 field, float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        h->w32(view + field, bits);
    };
    put_f(vp.TopLeftX, 0.0f);
    put_f(vp.TopLeftY, 0.0f);
    put_f(vp.Width, 64.0f);
    put_f(vp.Height, 64.0f);
    put_f(vp.MinDepth, 0.0f);
    put_f(vp.MaxDepth, 1.0f);
    (void)h->com(list, GraphicsCommandListSlot::RSSetViewports, {{1}, {view}});
    (void)h->com(list, GraphicsCommandListSlot::DrawInstanced,
                 {{4}, {1}, {0}, {0}});
    ASSERT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    h->write_ptr(kArray, list);
    (void)h->com(queue, CommandQueueSlot::ExecuteCommandLists, {{1}, {kArray}});

    ASSERT_EQ(h->backend.draws.size(), 2u);
    EXPECT_TRUE(h->backend.draws[0].capture);
    EXPECT_EQ(h->backend.draws[0].vertex_count, 4u);
    EXPECT_EQ(h->backend.draws[0].topology,
              graphics::BackendPrimitiveTopology::PointList);
    ASSERT_EQ(h->backend.dispatches.size(), 1u);
    EXPECT_EQ(h->backend.dispatches[0].groups, 4u);
    EXPECT_TRUE(h->backend.dispatches[0].in0);
    EXPECT_TRUE(h->backend.dispatches[0].out1);
    EXPECT_TRUE(h->backend.dispatches[0].counter2);
    EXPECT_TRUE(h->backend.dispatches[0].ids3);
    EXPECT_FALSE(h->backend.draws[1].capture);
    EXPECT_TRUE(h->backend.draws[1].srv0);
    EXPECT_TRUE(h->backend.draws[1].srv1);
    EXPECT_EQ(h->backend.draws[1].vertex_count, 4u);
    EXPECT_EQ(h->backend.draws[1].topology,
              graphics::BackendPrimitiveTopology::PointList);
    // The pass-through pipeline declares the class of what it draws: Metal
    // refuses a vertex function that writes [[render_target_array_index]]
    // (every pass-through does) when inputPrimitiveTopology is unspecified.
    const auto passthrough = std::find_if(
        h->backend.pipeline_descs.begin(), h->backend.pipeline_descs.end(),
        [](const graphics::BackendPipelineDesc& desc) {
            return desc.vertex_entry_point == graphics::kGsPassthroughEntryPoint;
        });
    ASSERT_NE(passthrough, h->backend.pipeline_descs.end());
    EXPECT_EQ(passthrough->input_primitive_class,
              graphics::BackendPrimitiveClass::Point);
}

void prepare_point_gs_draw(Harness& h, GuestAddr list, GuestAddr pso,
                           GuestAddr root) {
    (void)h.com(list, GraphicsCommandListSlot::SetPipelineState, {{pso}});
    (void)h.com(list, GraphicsCommandListSlot::SetGraphicsRootSignature,
                {{root}});
    (void)h.com(list, GraphicsCommandListSlot::IASetPrimitiveTopology,
                {{D3D_PRIMITIVE_TOPOLOGY_POINTLIST}});
    const auto& vp = layout::D3D12_VIEWPORT[h.ptr_size() == 8];
    const GuestAddr view = kData + 0x1600;
    const auto put_f = [&](u32 field, float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        h.w32(view + field, bits);
    };
    put_f(vp.TopLeftX, 0.0f);
    put_f(vp.TopLeftY, 0.0f);
    put_f(vp.Width, 64.0f);
    put_f(vp.Height, 64.0f);
    put_f(vp.MinDepth, 0.0f);
    put_f(vp.MaxDepth, 1.0f);
    (void)h.com(list, GraphicsCommandListSlot::RSSetViewports, {{1}, {view}});
}

// Learn (DrawInstanced): SV_InstanceID counts from 0 across the draw, and
// StartInstanceLocation only offsets per-instance fetches. An instanced
// geometry draw is captured once for all instances, keyed draw-relative
// (instance x key stride), as ExecuteIndirect's GPU assembly is.
TEST_P(D3D12Test, GeometryShaderCaptureKeepsEachInstancesId) {
    const GuestAddr dev = h->device();
    const GuestAddr root = make_root(*h, dev, 0);
    prime_gfx(*h, root, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);
    const auto& l = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[GetParam()];
    put_shader(*h, kData + 0x2000, point_vertex_shader(), l.VS,
               l.VS_pShaderBytecode, l.VS_BytecodeLength);
    put_shader(*h, kData + 0x3000, point_geometry_shader(), l.GS,
               l.GS_pShaderBytecode, l.GS_BytecodeLength);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK);
    const GuestAddr pso = h->read_ptr(kOut);
    const GuestAddr queue = h->queue(dev);
    const GuestAddr alloc = h->allocator(dev);
    GuestAddr list = 0;
    ASSERT_EQ(h->try_list(dev, alloc, list), S_OK);
    prepare_point_gs_draw(*h, list, pso, root);
    (void)h->com(list, GraphicsCommandListSlot::DrawInstanced,
                 {{2}, {3}, {0}, {5}});
    ASSERT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    h->write_ptr(kArray, list);
    (void)h->com(queue, CommandQueueSlot::ExecuteCommandLists, {{1}, {kArray}});

    // Every instance is captured by one GPU-assembled draw (its argument
    // block {2, 3, 0, 5} read by the resolve kernel, which keys the records
    // draw-relative and leaves SV_InstanceID to the key), expanded by one
    // dispatch and drawn by one pass-through: not three of each.
    ASSERT_EQ(h->backend.draws.size(), 2u);
    EXPECT_TRUE(h->backend.draws[0].capture);
    EXPECT_TRUE(h->backend.draws[0].indirect);
    EXPECT_FALSE(h->backend.draws[0].indexed);
    EXPECT_FALSE(h->backend.draws[1].capture);
    ASSERT_EQ(h->backend.dispatches.size(), 2u);
    EXPECT_EQ(h->backend.dispatches[0].entry, "vitreo_ia_resolve");
}

/// Learn (DrawIndexedInstanced): StartIndexLocation is the first index read
/// and IndexCountPerInstance indexes follow, in the view's format. The
/// format's cut value (0xFFFF / 0xFFFFFFFF) is not a vertex, so a point list
/// emits one point per other index. `words` is the buffer's little-endian
/// contents. A GPU destination is filled by CopyBufferRegion first; a null
/// destination uses the upload buffer itself, which the CPU can read.
bool execute_point_gs_indexed(Harness& h, GuestAddr dev, GuestAddr queue,
                              GuestAddr pso, GuestAddr root, GuestAddr upload,
                              GuestAddr gpu, u32 copy_bytes, u32 view_bytes,
                              u32 format, u32 index_count, u32 start_index) {
    h.backend.draws.clear();
    h.backend.dispatches.clear();
    h.backend.stream.clear();
    const GuestAddr alloc = h.allocator(dev);
    GuestAddr list = 0;
    if (h.try_list(dev, alloc, list) != S_OK) return false;
    prepare_point_gs_draw(h, list, pso, root);
    if (gpu != 0) {
        if (h.com(list, GraphicsCommandListSlot::CopyBufferRegion,
                  {{gpu}, Harness::wide(0), {upload}, Harness::wide(0),
                   Harness::wide(copy_bytes)}) != S_OK) {
            return false;
        }
    }
    const GuestAddr resource = gpu != 0 ? gpu : upload;
    const u64 va =
        h.com(resource, ResourceSlot::GetGPUVirtualAddress, {}, true);
    const auto& ib = layout::D3D12_INDEX_BUFFER_VIEW[h.ptr_size() == 8];
    const GuestAddr view = kData + 0x1700;
    h.w64(view + ib.BufferLocation, va);
    h.w32(view + ib.SizeInBytes, view_bytes);
    h.w32(view + ib.Format, format);
    if (h.com(list, GraphicsCommandListSlot::IASetIndexBuffer, {{view}}) !=
        S_OK) {
        return false;
    }
    if (h.com(list, GraphicsCommandListSlot::DrawIndexedInstanced,
              {{index_count}, {1}, {start_index}, {0}, {0}}) != S_OK) {
        return false;
    }
    if (h.com(list, GraphicsCommandListSlot::Close, {}) != S_OK) return false;
    h.write_ptr(kArray, list);
    (void)h.com(queue, CommandQueueSlot::ExecuteCommandLists, {{1}, {kArray}});
    return true;
}

void expect_lowered_points(const FakeBackend& backend, u32 points) {
    ASSERT_EQ(backend.draws.size(), 2u);
    EXPECT_TRUE(backend.draws[0].capture);
    EXPECT_TRUE(backend.draws[0].indexed);
    EXPECT_EQ(backend.draws[0].index_count, points);
    EXPECT_EQ(backend.draws[0].vertex_count, 0u);
    EXPECT_EQ(backend.draws[0].topology,
              graphics::BackendPrimitiveTopology::PointList);
    ASSERT_EQ(backend.dispatches.size(), 1u);
    EXPECT_EQ(backend.dispatches[0].groups, points);
    EXPECT_FALSE(backend.draws[1].capture);
    EXPECT_FALSE(backend.draws[1].indexed);
    EXPECT_EQ(backend.draws[1].vertex_count, points);
    EXPECT_EQ(backend.draws[1].topology,
              graphics::BackendPrimitiveTopology::PointList);
}

// Learn (DrawIndexedInstanced / D3D12_INDEX_BUFFER_VIEW): the indexes are
// the buffer's bytes at StartIndexLocation, including a DEFAULT heap the
// CPU cannot map. Geometry and tessellation draws share that read. The
// sequences are {5, 6, 7, cut} with StartIndexLocation 1 and a count of 3,
// so the draw reads {6, 7, cut}. A point list keeps the two non-cut
// indexes. The three leading values, or zeros, would keep three. The cut is
// the PSO's IBStripCutValue (d3d12_a61_ia_probe): with DISABLED the max
// index is an ordinary point, so all three are kept.
TEST_P(D3D12Test, GeometryShaderDrawReadsGpuOnlyIndexBuffer) {
    const GuestAddr dev = h->device();
    const GuestAddr root = make_root(*h, dev, 0);
    prime_gfx(*h, root, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);
    const auto& l = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[GetParam()];
    put_shader(*h, kData + 0x2000, point_vertex_shader(), l.VS,
               l.VS_pShaderBytecode, l.VS_BytecodeLength);
    put_shader(*h, kData + 0x3000, point_geometry_shader(), l.GS,
               l.GS_pShaderBytecode, l.GS_BytecodeLength);
    const auto make_pso = [&](u32 cut) {
        h->w32(kDesc + l.IBStripCutValue, cut);
        EXPECT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                         {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
                  S_OK);
        return h->read_ptr(kOut);
    };
    const GuestAddr pso = make_pso(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF);
    const GuestAddr pso32 =
        make_pso(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF);
    const GuestAddr pso_uncut =
        make_pso(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED);
    const GuestAddr queue = h->queue(dev);

    const auto upload = [&](const u32* words, u32 count) {
        GuestAddr resource = 0;
        EXPECT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD, count * 4u,
                            D3D12_RESOURCE_STATE_GENERIC_READ, resource),
                  S_OK);
        EXPECT_EQ(h->com(resource, ResourceSlot::Map, {{0}, {0}, {kOut2}}),
                  S_OK);
        const GuestAddr ptr = h->read_ptr(kOut2);
        for (u32 i = 0; i < count; ++i) h->w32(ptr + i * 4u, words[i]);
        return resource;
    };
    const auto gpu_buffer = [&]() {
        GuestAddr resource = 0;
        EXPECT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 256,
                            D3D12_RESOURCE_STATE_COMMON, resource),
                  S_OK);
        return resource;
    };

    // R16: bytes 05 00 06 00 07 00 FF FF. Start 1 reads 6, 7, 0xFFFF.
    const u32 r16[] = {0x00060005u, 0xFFFF0007u};
    const GuestAddr upload16 = upload(r16, 2);
    ASSERT_TRUE(execute_point_gs_indexed(
        *h, dev, queue, pso, root, upload16, 0, 0, 8, DXGI_FORMAT_R16_UINT, 3,
        1));
    ASSERT_NO_FATAL_FAILURE(expect_lowered_points(h->backend, 2));
    EXPECT_TRUE(h->backend.stream.empty());
    ASSERT_TRUE(execute_point_gs_indexed(
        *h, dev, queue, pso_uncut, root, upload16, 0, 0, 8,
        DXGI_FORMAT_R16_UINT, 3, 1));
    ASSERT_NO_FATAL_FAILURE(expect_lowered_points(h->backend, 3));

    // A 16-bit DEFAULT buffer is assembled on the GPU timeline (the path
    // ExecuteIndirect takes): the resolve kernel reads the indexes, the
    // capture is drawn from its argument block, and the CPU reads nothing
    // back.
    const GuestAddr gpu16 = gpu_buffer();
    ASSERT_TRUE(execute_point_gs_indexed(
        *h, dev, queue, pso, root, upload16, gpu16, 8, 8, DXGI_FORMAT_R16_UINT,
        3, 1));
    ASSERT_EQ(h->backend.draws.size(), 2u);
    EXPECT_TRUE(h->backend.draws[0].capture);
    EXPECT_TRUE(h->backend.draws[0].indexed);
    EXPECT_TRUE(h->backend.draws[0].indirect);
    EXPECT_FALSE(h->backend.draws[1].capture);
    EXPECT_EQ(h->backend.dispatches.size(), 2u);
    // The list's upload copy, then only GPU-timeline copies of the resolved
    // counts (4 bytes): no read-back of the 6 index bytes.
    ASSERT_FALSE(h->backend.stream.empty());
    EXPECT_EQ(h->backend.stream.front(), "copy 8");
    EXPECT_EQ(std::count(h->backend.stream.begin(), h->backend.stream.end(),
                         std::string("copy 6")),
              0);

    // R32: {5, 6, 7, 0xFFFFFFFF}. Start 1 reads 6, 7, 0xFFFFFFFF.
    const u32 r32[] = {5u, 6u, 7u, 0xFFFFFFFFu};
    const GuestAddr upload32 = upload(r32, 4);
    const GuestAddr gpu32 = gpu_buffer();
    ASSERT_TRUE(execute_point_gs_indexed(
        *h, dev, queue, pso32, root, upload32, gpu32, 16, 16,
        DXGI_FORMAT_R32_UINT, 3, 1));
    ASSERT_NO_FATAL_FAILURE(expect_lowered_points(h->backend, 2));
    EXPECT_EQ(h->backend.stream,
              (std::vector<std::string>{"copy 16", "copy 12"}));

    // The bytes are in a 32-bit DEFAULT buffer (read on the CPU), and the
    // read-back copy is refused. The draw stays unimplemented instead of
    // assembling a guessed count.
    h->backend.buffer_copies_before_failure = 1;
    ASSERT_TRUE(execute_point_gs_indexed(
        *h, dev, queue, pso32, root, upload32, gpu32, 16, 16,
        DXGI_FORMAT_R32_UINT, 3, 1));
    EXPECT_TRUE(h->backend.draws.empty());
    EXPECT_TRUE(h->backend.dispatches.empty());
    EXPECT_EQ(h->backend.stream, (std::vector<std::string>{"copy 16"}));
}

TEST_P(D3D12Test, HullAndDomainShadersDrawThroughTheSoftwareTessellator) {
    const GuestAddr dev = h->device();
    const GuestAddr root = make_root(
        *h, dev, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT,
        true);
    prime_gfx(*h, root, D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH);
    const auto& l = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[GetParam()];
    const auto& el = layout::D3D12_INPUT_ELEMENT_DESC[GetParam()];
    const GuestAddr names = kData + 0x1500;
    EXPECT_TRUE(h->memory
                    .write_bytes(names,
                                 reinterpret_cast<const Byte*>("POSITION"), 9)
                    .is_ok());
    EXPECT_TRUE(h->memory
                    .write_bytes(names + 16, reinterpret_cast<const Byte*>("COLOR"),
                                 6)
                    .is_ok());
    const GuestAddr elements = kData + 0x1400;
    auto element = [&](u32 index, GuestAddr name, u32 offset) {
        const GuestAddr at = elements + index * el.size;
        h->write_ptr(at + el.SemanticName, name);
        h->w32(at + el.SemanticIndex, 0);
        h->w32(at + el.Format, DXGI_FORMAT_R32G32B32A32_FLOAT);
        h->w32(at + el.InputSlot, 0);
        h->w32(at + el.AlignedByteOffset, offset);
        h->w32(at + el.InputSlotClass, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA);
        h->w32(at + el.InstanceDataStepRate, 0);
    };
    element(0, names, 0);
    element(1, names + 16, 16);
    h->write_ptr(kDesc + l.InputLayout_pInputElementDescs, elements);
    h->w32(kDesc + l.InputLayout_NumElements, 2);
    put_shader(*h, kData + 0x2000,
               std::vector<u8>(tess_vs_cp, tess_vs_cp + sizeof(tess_vs_cp)), l.VS,
               l.VS_pShaderBytecode, l.VS_BytecodeLength);
    put_shader(*h, kData + 0x4000,
               std::vector<u8>(tess_hs_tri, tess_hs_tri + sizeof(tess_hs_tri)),
               l.HS, l.HS_pShaderBytecode, l.HS_BytecodeLength);
    put_shader(*h, kData + 0x6000,
               std::vector<u8>(tess_ds_tri, tess_ds_tri + sizeof(tess_ds_tri)),
               l.DS, l.DS_pShaderBytecode, l.DS_BytecodeLength);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK)
        << "hull and domain shaders lower with the software tessellator";
    const GuestAddr pso = h->read_ptr(kOut);

    const GuestAddr queue = h->queue(dev);
    const GuestAddr alloc = h->allocator(dev);
    GuestAddr list = 0;
    ASSERT_EQ(h->try_list(dev, alloc, list), S_OK);
    (void)h->com(list, GraphicsCommandListSlot::SetPipelineState, {{pso}});
    (void)h->com(list, GraphicsCommandListSlot::SetGraphicsRootSignature,
                 {{root}});
    (void)h->com(list, GraphicsCommandListSlot::IASetPrimitiveTopology,
                 {{D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST}});
    (void)h->com(list, GraphicsCommandListSlot::DrawInstanced,
                 {{3}, {1}, {0}, {0}});
    ASSERT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    h->write_ptr(kArray, list);
    (void)h->com(queue, CommandQueueSlot::ExecuteCommandLists, {{1}, {kArray}});

    ASSERT_GE(h->backend.draws.size(), 2u);
    EXPECT_TRUE(h->backend.draws.front().capture);
    EXPECT_FALSE(h->backend.draws.back().capture);
    EXPECT_TRUE(h->backend.draws.back().indexed);
    EXPECT_TRUE(h->backend.draws.back().indirect);
    EXPECT_TRUE(h->backend.draws.back().srv0);
    // Hull, tessellator count, scan, tessellator write, domain.
    ASSERT_GE(h->backend.dispatches.size(), 5u);
    EXPECT_EQ(h->backend.dispatches.front().groups, 1u);
    EXPECT_TRUE(h->backend.dispatches.front().in0);
}

// "HSInvocations increments once per patch" (D3D11.3 20.4.7). An indirect
// tessellated draw's patch count is GPU data: vitreo_stats_vertices adds the
// resolved primitive count (patches, every instance) to HSInvocations on the
// GPU, as it does IAPrimitives, instead of flagging it unmeasured.
TEST_P(D3D12Test, IndirectTessellatedDrawCountsHsInvocationsOnTheGpu) {
    const GuestAddr dev = h->device();
    const GuestAddr root = make_root(
        *h, dev, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT,
        true);
    prime_gfx(*h, root, D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH);
    const bool x64 = GetParam();
    const auto& l = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[x64];
    const auto& el = layout::D3D12_INPUT_ELEMENT_DESC[x64];
    const GuestAddr names = kData + 0x1500;
    EXPECT_TRUE(h->memory
                    .write_bytes(names,
                                 reinterpret_cast<const Byte*>("POSITION"), 9)
                    .is_ok());
    EXPECT_TRUE(h->memory
                    .write_bytes(names + 16, reinterpret_cast<const Byte*>("COLOR"),
                                 6)
                    .is_ok());
    const GuestAddr elements = kData + 0x1400;
    for (u32 index = 0; index < 2; ++index) {
        const GuestAddr at = elements + index * el.size;
        h->write_ptr(at + el.SemanticName, names + index * 16u);
        h->w32(at + el.SemanticIndex, 0);
        h->w32(at + el.Format, DXGI_FORMAT_R32G32B32A32_FLOAT);
        h->w32(at + el.InputSlot, 0);
        h->w32(at + el.AlignedByteOffset, index * 16u);
        h->w32(at + el.InputSlotClass, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA);
        h->w32(at + el.InstanceDataStepRate, 0);
    }
    h->write_ptr(kDesc + l.InputLayout_pInputElementDescs, elements);
    h->w32(kDesc + l.InputLayout_NumElements, 2);
    put_shader(*h, kData + 0x2000,
               std::vector<u8>(tess_vs_cp, tess_vs_cp + sizeof(tess_vs_cp)), l.VS,
               l.VS_pShaderBytecode, l.VS_BytecodeLength);
    put_shader(*h, kData + 0x4000,
               std::vector<u8>(tess_hs_tri, tess_hs_tri + sizeof(tess_hs_tri)),
               l.HS, l.HS_pShaderBytecode, l.HS_BytecodeLength);
    put_shader(*h, kData + 0x6000,
               std::vector<u8>(tess_ds_tri, tess_ds_tri + sizeof(tess_ds_tri)),
               l.DS, l.DS_pShaderBytecode, l.DS_BytecodeLength);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK);
    const GuestAddr pso = h->read_ptr(kOut);

    const auto& qh = layout::D3D12_QUERY_HEAP_DESC[x64];
    const GuestAddr qdesc = kData + 0x7000;
    for (u32 i = 0; i < qh.size; i += 4) h->w32(qdesc + i, 0);
    h->w32(qdesc + qh.Type, D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS);
    h->w32(qdesc + qh.Count, 1);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateQueryHeap,
                     {{qdesc}, {h->iid(IID_ID3D12QUERYHEAP)}, {kOut}}),
              S_OK);
    const GuestAddr heap = h->read_ptr(kOut);

    const auto& sig = layout::D3D12_COMMAND_SIGNATURE_DESC[x64];
    const auto& arg = layout::D3D12_INDIRECT_ARGUMENT_DESC[x64];
    const GuestAddr arg_at = kData + 0x5000;
    const GuestAddr sig_at = kData + 0x5100;
    for (u32 i = 0; i < arg.size; i += 4) h->w32(arg_at + i, 0);
    h->w32(arg_at + arg.Type, D3D12_INDIRECT_ARGUMENT_TYPE_DRAW);
    for (u32 i = 0; i < sig.size; i += 4) h->w32(sig_at + i, 0);
    h->w32(sig_at + sig.ByteStride, 16);
    h->w32(sig_at + sig.NumArgumentDescs, 1);
    h->write_ptr(sig_at + sig.pArgumentDescs, arg_at);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateCommandSignature,
                     {{sig_at}, {0}, {h->iid(IID_ID3D12COMMANDSIGNATURE)}, {kOut}}),
              S_OK);
    const GuestAddr signature = h->read_ptr(kOut);
    GuestAddr args = 0;
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 256,
                        D3D12_RESOURCE_STATE_GENERIC_READ, args),
              S_OK);
    ASSERT_EQ(h->com(args, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr mapped = h->read_ptr(kOut2);
    // One patch (3 control points), 2 instances.
    h->w32(mapped + 0, 3);
    h->w32(mapped + 4, 2);
    h->w32(mapped + 8, 0);
    h->w32(mapped + 12, 0);

    const GuestAddr queue = h->queue(dev);
    const GuestAddr alloc = h->allocator(dev);
    GuestAddr list = 0;
    ASSERT_EQ(h->try_list(dev, alloc, list), S_OK);
    (void)h->com(list, GraphicsCommandListSlot::SetPipelineState, {{pso}});
    (void)h->com(list, GraphicsCommandListSlot::SetGraphicsRootSignature,
                 {{root}});
    (void)h->com(list, GraphicsCommandListSlot::IASetPrimitiveTopology,
                 {{D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST}});
    (void)h->com(list, GraphicsCommandListSlot::BeginQuery,
                 {{heap}, {D3D12_QUERY_TYPE_PIPELINE_STATISTICS}, {0}});
    (void)h->com(list, GraphicsCommandListSlot::ExecuteIndirect,
                 {{signature}, {1}, {args}, Harness::wide(0), {0},
                  Harness::wide(0)});
    (void)h->com(list, GraphicsCommandListSlot::EndQuery,
                 {{heap}, {D3D12_QUERY_TYPE_PIPELINE_STATISTICS}, {0}});
    ASSERT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    h->write_ptr(kArray, list);
    const u64 waits_before = d3d12::g_replay_cpu_waits.load();
    (void)h->com(queue, CommandQueueSlot::ExecuteCommandLists, {{1}, {kArray}});
    EXPECT_EQ(d3d12::g_replay_cpu_waits.load(), waits_before);

    u32 vertex_counts = 0;
    for (const auto& dispatch : h->backend.dispatches) {
        if (dispatch.entry != graphics::kStatsVerticesEntryPoint) continue;
        ++vertex_counts;
        EXPECT_NE(dispatch.stats_flags & graphics::STATS_FLAG_ADD_HS_INVOCATIONS,
                  0u);
        EXPECT_NE(dispatch.stats_flags & graphics::STATS_FLAG_ADD_IA_PRIMITIVES,
                  0u);
    }
    EXPECT_EQ(vertex_counts, 1u);
}

// SV_CullDistance on a tessellated domain shader (D3D11.3 15.4.2): the
// PSO is created, and the rasterizing pass is drawn non-indexed over the
// tessellator's index list so each vertex sees its primitive.
TEST_P(D3D12Test, ADomainShadersCullDistancesDrawNonIndexedOverTheIndexList) {
    const GuestAddr dev = h->device();
    const GuestAddr root = make_root(
        *h, dev, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT,
        true);
    prime_gfx(*h, root, D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH);
    const auto& l = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[GetParam()];
    const auto& el = layout::D3D12_INPUT_ELEMENT_DESC[GetParam()];
    const GuestAddr names = kData + 0x1500;
    EXPECT_TRUE(h->memory
                    .write_bytes(names,
                                 reinterpret_cast<const Byte*>("POSITION"), 9)
                    .is_ok());
    EXPECT_TRUE(h->memory
                    .write_bytes(names + 16, reinterpret_cast<const Byte*>("COLOR"),
                                 6)
                    .is_ok());
    const GuestAddr elements = kData + 0x1400;
    auto element = [&](u32 index, GuestAddr name, u32 offset) {
        const GuestAddr at = elements + index * el.size;
        h->write_ptr(at + el.SemanticName, name);
        h->w32(at + el.SemanticIndex, 0);
        h->w32(at + el.Format, DXGI_FORMAT_R32G32B32A32_FLOAT);
        h->w32(at + el.InputSlot, 0);
        h->w32(at + el.AlignedByteOffset, offset);
        h->w32(at + el.InputSlotClass, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA);
        h->w32(at + el.InstanceDataStepRate, 0);
    };
    element(0, names, 0);
    element(1, names + 16, 16);
    h->write_ptr(kDesc + l.InputLayout_pInputElementDescs, elements);
    h->w32(kDesc + l.InputLayout_NumElements, 2);
    put_shader(*h, kData + 0x2000,
               std::vector<u8>(tess_vs_cp, tess_vs_cp + sizeof(tess_vs_cp)), l.VS,
               l.VS_pShaderBytecode, l.VS_BytecodeLength);
    put_shader(*h, kData + 0x4000,
               std::vector<u8>(tess_hs_tri, tess_hs_tri + sizeof(tess_hs_tri)),
               l.HS, l.HS_pShaderBytecode, l.HS_BytecodeLength);
    put_shader(*h, kData + 0x6000,
               std::vector<u8>(tess_ds_tri_cull,
                             tess_ds_tri_cull + sizeof(tess_ds_tri_cull)),
               l.DS, l.DS_pShaderBytecode, l.DS_BytecodeLength);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK)
        << "SV_CullDistance on the domain shader is no longer refused";
    const GuestAddr pso = h->read_ptr(kOut);

    const GuestAddr queue = h->queue(dev);
    const GuestAddr alloc = h->allocator(dev);
    GuestAddr list = 0;
    ASSERT_EQ(h->try_list(dev, alloc, list), S_OK);
    (void)h->com(list, GraphicsCommandListSlot::SetPipelineState, {{pso}});
    (void)h->com(list, GraphicsCommandListSlot::SetGraphicsRootSignature,
                 {{root}});
    (void)h->com(list, GraphicsCommandListSlot::IASetPrimitiveTopology,
                 {{D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST}});
    (void)h->com(list, GraphicsCommandListSlot::DrawInstanced,
                 {{3}, {1}, {0}, {0}});
    ASSERT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    h->write_ptr(kArray, list);
    (void)h->com(queue, CommandQueueSlot::ExecuteCommandLists, {{1}, {kArray}});

    ASSERT_GE(h->backend.draws.size(), 2u);
    EXPECT_TRUE(h->backend.draws.front().capture);
    EXPECT_FALSE(h->backend.draws.back().capture);
    // D3D11.3 15.4.2 needs each primitive's vertices: the records are drawn
    // NON-indexed, the pass-through reading the index list at t1.
    EXPECT_FALSE(h->backend.draws.back().indexed);
    EXPECT_TRUE(h->backend.draws.back().indirect);
    EXPECT_TRUE(h->backend.draws.back().srv0);
    // Hull, tessellator count, scan, tessellator write, domain.
    ASSERT_GE(h->backend.dispatches.size(), 5u);
    EXPECT_EQ(h->backend.dispatches.front().groups, 1u);
    EXPECT_TRUE(h->backend.dispatches.front().in0);
}

// Tier 0 (Learn, D3D12_RENDER_PASS_TIER; DirectX-Specs "Render Passes"):
// BeginRenderPass records Clear*View for CLEAR and one OMSetRenderTargets
// for the surfaces the spec binds (CLEAR, PRESERVE, DISCARD), with
// NO_ACCESS packed out. DISCARD may keep the previous contents. A draw
// inside the pass is what makes the bind visible to the backend.
// Closing without EndRenderPass fails the list, suspending flag included.
// A depth/stencil view whose planes are accessed independently fails the
// list. Learn (Close): E_FAIL if an invalid API was called during
// recording, E_INVALIDARG for an invalid argument. x86 and x64 use the
// layouts in d3d12_layouts.hpp.
TEST_P(D3D12Test, RenderPassRecordsClearsAndBindsAtTier0) {
    const bool is64 = GetParam();
    const GuestAddr dev = h->device();
    const GuestAddr queue = h->queue(dev);
    const GuestAddr alloc = h->allocator(dev);
    const auto texture = [&](u32 format, u32 flags, u32 state) {
        const auto& hp = layout::D3D12_HEAP_PROPERTIES[is64];
        const auto& rd = layout::D3D12_RESOURCE_DESC[is64];
        for (u32 i = 0; i < hp.size; i += 4) h->w32(kDesc + i, 0);
        for (u32 i = 0; i < rd.size; i += 4) h->w32(kDesc2 + i, 0);
        h->w32(kDesc + hp.Type, D3D12_HEAP_TYPE_DEFAULT);
        h->w32(kDesc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_TEXTURE2D);
        h->w64(kDesc2 + rd.Width, 4);
        h->w32(kDesc2 + rd.Height, 4);
        EXPECT_TRUE(h->memory.write_u16(kDesc2 + rd.DepthOrArraySize, 1).is_ok());
        EXPECT_TRUE(h->memory.write_u16(kDesc2 + rd.MipLevels, 1).is_ok());
        h->w32(kDesc2 + rd.Format, format);
        h->w32(kDesc2 + rd.SampleDesc_Count, 1);
        h->w32(kDesc2 + rd.Flags, flags);
        EXPECT_EQ(h->com(dev, DeviceSlot::CreateCommittedResource,
                         {{kDesc}, {0}, {kDesc2}, {state}, {0},
                          {h->iid(IID_ID3D12RESOURCE)}, {kOut}}),
                  S_OK);
        return h->read_ptr(kOut);
    };
    const GuestAddr color_res0 = texture(
        DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    const GuestAddr color_res1 = texture(
        DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    const GuestAddr depth32_res = texture(
        DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const GuestAddr depth24_res = texture(
        DXGI_FORMAT_D24_UNORM_S8_UINT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    ASSERT_GE(h->backend.textures.size(), 4u);
    graphics::BackendTexture2D* color0 = h->backend.textures[0];
    graphics::BackendTexture2D* color1 = h->backend.textures[1];
    graphics::BackendTexture2D* depth32 = h->backend.textures[2];

    const auto heap = [&](u32 type, u32 count) {
        const auto& l = layout::D3D12_DESCRIPTOR_HEAP_DESC[is64];
        for (u32 i = 0; i < l.size; i += 4) h->w32(kDesc + i, 0);
        h->w32(kDesc + l.Type, type);
        h->w32(kDesc + l.NumDescriptors, count);
        EXPECT_EQ(h->com(dev, DeviceSlot::CreateDescriptorHeap,
                         {{kDesc}, {h->iid(IID_ID3D12DESCRIPTORHEAP)}, {kOut}}),
                  S_OK);
        return h->read_ptr(kOut);
    };
    const auto cpu_at = [&](GuestAddr heap_obj) {
        EXPECT_EQ(h->com(heap_obj,
                         DescriptorHeapSlot::GetCPUDescriptorHandleForHeapStart,
                         {{kOut2}}),
                  is64 ? kOut2 : static_cast<u32>(kOut2));
        return h->read_ptr(kOut2);
    };
    const GuestAddr rtv_heap = heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2);
    const GuestAddr dsv_heap = heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2);
    const u64 rtv0 = cpu_at(rtv_heap);
    const u32 rtv_step = static_cast<u32>(h->com(
        dev, DeviceSlot::GetDescriptorHandleIncrementSize,
        {{D3D12_DESCRIPTOR_HEAP_TYPE_RTV}}));
    const u64 rtv1 = rtv0 + rtv_step;
    const u64 dsv32 = cpu_at(dsv_heap);
    const u32 dsv_step = static_cast<u32>(h->com(
        dev, DeviceSlot::GetDescriptorHandleIncrementSize,
        {{D3D12_DESCRIPTOR_HEAP_TYPE_DSV}}));
    const u64 dsv24 = dsv32 + dsv_step;
    (void)h->com(dev, DeviceSlot::CreateRenderTargetView, {{color_res0}, {0}, {rtv0}});
    (void)h->com(dev, DeviceSlot::CreateRenderTargetView, {{color_res1}, {0}, {rtv1}});
    (void)h->com(dev, DeviceSlot::CreateDepthStencilView, {{depth32_res}, {0}, {dsv32}});
    (void)h->com(dev, DeviceSlot::CreateDepthStencilView, {{depth24_res}, {0}, {dsv24}});

    const GuestAddr root = make_root(*h, dev, 0);
    prime_gfx(*h, root, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);
    const auto& pso_l = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[is64];
    put_shader(*h, kData + 0x2000, point_vertex_shader(), pso_l.VS,
               pso_l.VS_pShaderBytecode, pso_l.VS_BytecodeLength);
    put_shader(*h, kData + 0x3000, point_geometry_shader(), pso_l.GS,
               pso_l.GS_pShaderBytecode, pso_l.GS_BytecodeLength);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK);
    const GuestAddr pso = h->read_ptr(kOut);
    const auto& vp = layout::D3D12_VIEWPORT[is64];
    const GuestAddr kView = kData + 0x1600;
    const auto put_f = [&](GuestAddr at, float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        h->w32(at, bits);
    };
    put_f(kView + vp.TopLeftX, 0.0f);
    put_f(kView + vp.TopLeftY, 0.0f);
    put_f(kView + vp.Width, 4.0f);
    put_f(kView + vp.Height, 4.0f);
    put_f(kView + vp.MinDepth, 0.0f);
    put_f(kView + vp.MaxDepth, 1.0f);

    const auto& rt = layout::D3D12_RENDER_PASS_RENDER_TARGET_DESC[is64];
    const auto& ds = layout::D3D12_RENDER_PASS_DEPTH_STENCIL_DESC[is64];
    const GuestAddr kRt = kData + 0x7000;
    const GuestAddr kDs = kData + 0x7800;
    const auto write_rt = [&](u32 index, u64 handle, u32 begin, u32 end,
                              float r, float g, float b, float a) {
        const GuestAddr at = kRt + static_cast<GuestAddr>(index) * rt.size;
        for (u32 i = 0; i < rt.size; i += 4) h->w32(at + i, 0);
        h->write_ptr(at + rt.cpuDescriptor_ptr, handle);
        h->w32(at + rt.BeginningAccess_Type, begin);
        h->w32(at + rt.EndingAccess_Type, end);
        put_f(at + rt.BeginningAccess_Clear_ClearValue_Color + 0, r);
        put_f(at + rt.BeginningAccess_Clear_ClearValue_Color + 4, g);
        put_f(at + rt.BeginningAccess_Clear_ClearValue_Color + 8, b);
        put_f(at + rt.BeginningAccess_Clear_ClearValue_Color + 12, a);
    };
    const auto write_ds = [&](u64 handle, u32 depth_begin, u32 stencil_begin,
                              float depth_clear) {
        for (u32 i = 0; i < ds.size; i += 4) h->w32(kDs + i, 0);
        h->write_ptr(kDs + ds.cpuDescriptor_ptr, handle);
        h->w32(kDs + ds.DepthBeginningAccess_Type, depth_begin);
        h->w32(kDs + ds.StencilBeginningAccess_Type, stencil_begin);
        h->w32(kDs + ds.DepthEndingAccess_Type,
               D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE);
        h->w32(kDs + ds.StencilEndingAccess_Type,
               D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE);
        put_f(kDs + ds.DepthBeginningAccess_Clear_ClearValue_DepthStencil_Depth,
              depth_clear);
    };
    const auto begin = [&](GuestAddr list, u32 n, GuestAddr targets,
                           GuestAddr depth_ptr, u32 flags) {
        (void)h->com(list, GraphicsCommandListSlot::BeginRenderPass,
                     {{n}, {targets}, {depth_ptr}, {flags}});
    };
    const auto end = [&](GuestAddr list) {
        (void)h->com(list, GraphicsCommandListSlot::EndRenderPass, {});
    };
    const auto draw = [&](GuestAddr list) {
        (void)h->com(list, GraphicsCommandListSlot::SetPipelineState, {{pso}});
        (void)h->com(list, GraphicsCommandListSlot::SetGraphicsRootSignature,
                     {{root}});
        (void)h->com(list, GraphicsCommandListSlot::IASetPrimitiveTopology,
                     {{D3D_PRIMITIVE_TOPOLOGY_POINTLIST}});
        (void)h->com(list, GraphicsCommandListSlot::RSSetViewports,
                     {{1}, {kView}});
        (void)h->com(list, GraphicsCommandListSlot::DrawInstanced,
                     {{4}, {1}, {0}, {0}});
    };
    const auto has_pass = [&](graphics::BackendTexture2D* c0,
                              graphics::BackendTexture2D* c1,
                              graphics::BackendTexture2D* depth) {
        for (const auto& pass : h->backend.passes) {
            if (pass.color_targets[0] == c0 && pass.color_targets[1] == c1 &&
                pass.depth_stencil_target == depth) {
                return true;
            }
        }
        return false;
    };
    const auto color_clears = [&](graphics::BackendTexture2D* tex) {
        std::vector<std::array<float, 4>> out;
        for (const auto& clear : h->backend.rt_clears) {
            if (clear.texture == tex) out.push_back(clear.color);
        }
        return out;
    };
    GuestAddr list = 0;
    ASSERT_EQ(h->try_list(dev, alloc, list), S_OK);
    const auto run = [&]() {
        ASSERT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
        h->write_ptr(kArray, list);
        (void)h->com(queue, CommandQueueSlot::ExecuteCommandLists,
                     {{1}, {kArray}});
        ASSERT_EQ(h->com(list, GraphicsCommandListSlot::Reset, {{alloc}, {0}}),
                  S_OK);
    };
    const auto reopen = [&]() {
        GuestAddr next = 0;
        EXPECT_EQ(h->try_list(dev, alloc, next), S_OK);
        list = next;
    };
    const auto fails = [&](u64 hr) {
        EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), hr);
        EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Reset, {{alloc}, {0}}),
                  hr);
        reopen();
    };

    // CLEAR + PRESERVE, depth-only D32 with stencil NO_ACCESS, UAV flag.
    write_rt(0, rtv0, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR,
             D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE, 1.0f, 0.0f, 0.0f,
             1.0f);
    write_ds(dsv32, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR,
             D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS, 0.25f);
    begin(list, 1, kRt, kDs, D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES);
    draw(list);
    end(list);
    run();
    const auto red = color_clears(color0);
    ASSERT_EQ(red.size(), 1u);
    EXPECT_FLOAT_EQ(red[0][0], 1.0f);
    EXPECT_FLOAT_EQ(red[0][1], 0.0f);
    EXPECT_FLOAT_EQ(red[0][2], 0.0f);
    EXPECT_FLOAT_EQ(red[0][3], 1.0f);
    ASSERT_EQ(h->backend.ds_clears.size(), 1u);
    EXPECT_EQ(h->backend.ds_clears[0].texture, depth32);
    EXPECT_FLOAT_EQ(h->backend.ds_clears[0].depth, 0.25f);
    EXPECT_TRUE(h->backend.ds_clears[0].depth_on);
    EXPECT_FALSE(h->backend.ds_clears[0].stencil_on);
    EXPECT_TRUE(has_pass(color0, nullptr, depth32));

    // PRESERVE keeps the cleared colour and still binds the target.
    write_rt(0, rtv0, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE,
             D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE, 0.0f, 0.0f, 0.0f,
             0.0f);
    begin(list, 1, kRt, 0, D3D12_RENDER_PASS_FLAG_NONE);
    draw(list);
    end(list);
    run();
    EXPECT_EQ(color_clears(color0).size(), 1u);
    EXPECT_TRUE(has_pass(color0, nullptr, nullptr));

    // DISCARD may return the previous contents (DirectX-Specs). Bind, no clear.
    write_rt(0, rtv0, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD,
             D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD, 0.0f, 0.0f, 0.0f,
             0.0f);
    begin(list, 1, kRt, 0, D3D12_RENDER_PASS_FLAG_NONE);
    draw(list);
    end(list);
    run();
    EXPECT_EQ(color_clears(color0).size(), 1u);
    EXPECT_TRUE(has_pass(color0, nullptr, nullptr));

    // NO_ACCESS is not bound; the remaining CLEAR target is slot 0.
    write_rt(0, rtv0, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS,
             D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS, 0.0f, 0.0f, 0.0f,
             0.0f);
    write_rt(1, rtv1, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR,
             D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE, 0.0f, 1.0f, 0.0f,
             1.0f);
    begin(list, 2, kRt, 0, D3D12_RENDER_PASS_FLAG_NONE);
    draw(list);
    end(list);
    run();
    const auto green = color_clears(color1);
    ASSERT_EQ(green.size(), 1u);
    EXPECT_FLOAT_EQ(green[0][1], 1.0f);
    EXPECT_EQ(color_clears(color0).size(), 1u);
    EXPECT_TRUE(has_pass(color1, nullptr, nullptr));
    EXPECT_FALSE(has_pass(color0, color1, nullptr));

    // The list is recording again after run(). Close the empty one.
    ASSERT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);

    write_ds(dsv24, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR,
             D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS, 1.0f);
    reopen();
    begin(list, 0, 0, kDs, D3D12_RENDER_PASS_FLAG_NONE);
    fails(E_FAIL);

    write_rt(0, rtv0, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR,
             D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE, 1.0f, 0.0f, 0.0f,
             1.0f);
    begin(list, 1, kRt, 0, D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS);
    fails(E_FAIL);
    begin(list, 1, kRt, 0, D3D12_RENDER_PASS_FLAG_RESUMING_PASS);
    fails(E_FAIL);
    begin(list, 1, kRt, 0, 0x8u);
    fails(E_INVALIDARG);
    begin(list, 9, 0, 0, D3D12_RENDER_PASS_FLAG_NONE);
    fails(E_INVALIDARG);
    begin(list, 0, 0, 0, D3D12_RENDER_PASS_FLAG_NONE);
    fails(E_INVALIDARG);
    begin(list, 1, 0, 0, D3D12_RENDER_PASS_FLAG_NONE);
    fails(E_INVALIDARG);

    end(list);
    fails(E_FAIL);

    begin(list, 1, kRt, 0, D3D12_RENDER_PASS_FLAG_NONE);
    fails(E_FAIL);  // closed inside the pass

    begin(list, 1, kRt, 0, D3D12_RENDER_PASS_FLAG_NONE);
    begin(list, 1, kRt, 0, D3D12_RENDER_PASS_FLAG_NONE);
    end(list);
    fails(E_FAIL);

    begin(list, 1, kRt, 0, D3D12_RENDER_PASS_FLAG_NONE);
    (void)h->com(list, GraphicsCommandListSlot::CopyBufferRegion,
                 {{0}, Harness::wide(0), {0}, Harness::wide(0), Harness::wide(0)});
    end(list);
    fails(E_FAIL);

    const GuestAddr compute_alloc =
        h->allocator(dev, D3D12_COMMAND_LIST_TYPE_COMPUTE);
    GuestAddr compute = 0;
    ASSERT_EQ(h->try_list(dev, compute_alloc, compute,
                          D3D12_COMMAND_LIST_TYPE_COMPUTE),
              S_OK);
    begin(compute, 1, kRt, 0, D3D12_RENDER_PASS_FLAG_NONE);
    EXPECT_EQ(h->com(compute, GraphicsCommandListSlot::Close, {}), E_FAIL);
}

// Learn (render passes, suspending/resuming; D3D12_RENDER_PASS_TIER): tier 0
// emulates the pair. A suspending pass may be resumed in the same
// ExecuteCommandLists, including a pass that is both, and the beginning
// clear is not repeated. The local copy is restored at the resume, so a
// write to that render target across the suspend (the UAV hazard; a clear
// is the same kind of GPU write) does not stick. A resume in a later
// ExecuteCommandLists does not keep that copy. A resume on the same list
// whose views differ fails Close with E_FAIL. Depth and stencil accessed
// independently on one view stays unimplemented. x86 and x64.
TEST_P(D3D12Test, RenderPassSuspendResumeKeepsTheLocalRenderTarget) {
    const bool is64 = GetParam();
    const GuestAddr dev = h->device();
    for (u32 i = 0; i < 12; i += 4) h->w32(kDesc + i, 0xFFFFFFFFu);
    ASSERT_EQ(h->com(dev, DeviceSlot::CheckFeatureSupport,
                     {{D3D12_FEATURE_D3D12_OPTIONS5}, {kDesc}, {12}}),
              S_OK);
    EXPECT_EQ(h->r32(kDesc + 4), 0u);

    const GuestAddr queue = h->queue(dev);
    const GuestAddr alloc = h->allocator(dev);
    const auto texture = [&](u32 format, u32 flags, u32 state) {
        const auto& hp = layout::D3D12_HEAP_PROPERTIES[is64];
        const auto& rd = layout::D3D12_RESOURCE_DESC[is64];
        for (u32 i = 0; i < hp.size; i += 4) h->w32(kDesc + i, 0);
        for (u32 i = 0; i < rd.size; i += 4) h->w32(kDesc2 + i, 0);
        h->w32(kDesc + hp.Type, D3D12_HEAP_TYPE_DEFAULT);
        h->w32(kDesc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_TEXTURE2D);
        h->w64(kDesc2 + rd.Width, 4);
        h->w32(kDesc2 + rd.Height, 4);
        EXPECT_TRUE(h->memory.write_u16(kDesc2 + rd.DepthOrArraySize, 1).is_ok());
        EXPECT_TRUE(h->memory.write_u16(kDesc2 + rd.MipLevels, 1).is_ok());
        h->w32(kDesc2 + rd.Format, format);
        h->w32(kDesc2 + rd.SampleDesc_Count, 1);
        h->w32(kDesc2 + rd.Flags, flags);
        EXPECT_EQ(h->com(dev, DeviceSlot::CreateCommittedResource,
                         {{kDesc}, {0}, {kDesc2}, {state}, {0},
                          {h->iid(IID_ID3D12RESOURCE)}, {kOut}}),
                  S_OK);
        return h->read_ptr(kOut);
    };
    const GuestAddr color_res0 = texture(
        DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    const GuestAddr color_res1 = texture(
        DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    const GuestAddr depth24_res = texture(
        DXGI_FORMAT_D24_UNORM_S8_UINT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    ASSERT_GE(h->backend.textures.size(), 3u);
    auto* color0 = static_cast<FakeBackend::FakeTexture*>(h->backend.textures[0]);
    auto* color1 = static_cast<FakeBackend::FakeTexture*>(h->backend.textures[1]);

    const auto heap = [&](u32 type, u32 count) {
        const auto& l = layout::D3D12_DESCRIPTOR_HEAP_DESC[is64];
        for (u32 i = 0; i < l.size; i += 4) h->w32(kDesc + i, 0);
        h->w32(kDesc + l.Type, type);
        h->w32(kDesc + l.NumDescriptors, count);
        EXPECT_EQ(h->com(dev, DeviceSlot::CreateDescriptorHeap,
                         {{kDesc}, {h->iid(IID_ID3D12DESCRIPTORHEAP)}, {kOut}}),
                  S_OK);
        return h->read_ptr(kOut);
    };
    const auto cpu_at = [&](GuestAddr heap_obj) {
        EXPECT_EQ(h->com(heap_obj,
                         DescriptorHeapSlot::GetCPUDescriptorHandleForHeapStart,
                         {{kOut2}}),
                  is64 ? kOut2 : static_cast<u32>(kOut2));
        return h->read_ptr(kOut2);
    };
    const GuestAddr rtv_heap = heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2);
    const GuestAddr dsv_heap = heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
    const u64 rtv0 = cpu_at(rtv_heap);
    const u32 rtv_step = static_cast<u32>(h->com(
        dev, DeviceSlot::GetDescriptorHandleIncrementSize,
        {{D3D12_DESCRIPTOR_HEAP_TYPE_RTV}}));
    const u64 rtv1 = rtv0 + rtv_step;
    const u64 dsv24 = cpu_at(dsv_heap);
    (void)h->com(dev, DeviceSlot::CreateRenderTargetView, {{color_res0}, {0}, {rtv0}});
    (void)h->com(dev, DeviceSlot::CreateRenderTargetView, {{color_res1}, {0}, {rtv1}});
    (void)h->com(dev, DeviceSlot::CreateDepthStencilView,
                 {{depth24_res}, {0}, {dsv24}});

    const GuestAddr root = make_root(*h, dev, 0);
    prime_gfx(*h, root, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);
    const auto& pso_l = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[is64];
    put_shader(*h, kData + 0x2000, point_vertex_shader(), pso_l.VS,
               pso_l.VS_pShaderBytecode, pso_l.VS_BytecodeLength);
    put_shader(*h, kData + 0x3000, point_geometry_shader(), pso_l.GS,
               pso_l.GS_pShaderBytecode, pso_l.GS_BytecodeLength);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK);
    const GuestAddr pso = h->read_ptr(kOut);
    const auto& vp = layout::D3D12_VIEWPORT[is64];
    const GuestAddr kView = kData + 0x1600;
    const auto put_f = [&](GuestAddr at, float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        h->w32(at, bits);
    };
    put_f(kView + vp.TopLeftX, 0.0f);
    put_f(kView + vp.TopLeftY, 0.0f);
    put_f(kView + vp.Width, 4.0f);
    put_f(kView + vp.Height, 4.0f);
    put_f(kView + vp.MinDepth, 0.0f);
    put_f(kView + vp.MaxDepth, 1.0f);
    const GuestAddr kBlue = kData + 0x1800;
    put_f(kBlue + 0, 0.0f);
    put_f(kBlue + 4, 0.0f);
    put_f(kBlue + 8, 1.0f);
    put_f(kBlue + 12, 1.0f);

    const auto& rt = layout::D3D12_RENDER_PASS_RENDER_TARGET_DESC[is64];
    const auto& ds = layout::D3D12_RENDER_PASS_DEPTH_STENCIL_DESC[is64];
    const GuestAddr kRt = kData + 0x7000;
    const GuestAddr kDs = kData + 0x7800;
    const auto write_rt = [&](u64 handle, u32 begin, float r, float g, float b,
                              float a) {
        for (u32 i = 0; i < rt.size; i += 4) h->w32(kRt + i, 0);
        h->write_ptr(kRt + rt.cpuDescriptor_ptr, handle);
        h->w32(kRt + rt.BeginningAccess_Type, begin);
        h->w32(kRt + rt.EndingAccess_Type,
               D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE);
        put_f(kRt + rt.BeginningAccess_Clear_ClearValue_Color + 0, r);
        put_f(kRt + rt.BeginningAccess_Clear_ClearValue_Color + 4, g);
        put_f(kRt + rt.BeginningAccess_Clear_ClearValue_Color + 8, b);
        put_f(kRt + rt.BeginningAccess_Clear_ClearValue_Color + 12, a);
    };
    const auto draw = [&](GuestAddr list) {
        (void)h->com(list, GraphicsCommandListSlot::SetPipelineState, {{pso}});
        (void)h->com(list, GraphicsCommandListSlot::SetGraphicsRootSignature,
                     {{root}});
        (void)h->com(list, GraphicsCommandListSlot::IASetPrimitiveTopology,
                     {{D3D_PRIMITIVE_TOPOLOGY_POINTLIST}});
        (void)h->com(list, GraphicsCommandListSlot::RSSetViewports,
                     {{1}, {kView}});
        (void)h->com(list, GraphicsCommandListSlot::DrawInstanced,
                     {{4}, {1}, {0}, {0}});
    };
    const auto begin = [&](GuestAddr list, u64 handle_slot, u32 flags) {
        (void)h->com(list, GraphicsCommandListSlot::BeginRenderPass,
                     {{1}, {handle_slot}, {0}, {flags}});
    };
    const auto end = [&](GuestAddr list) {
        (void)h->com(list, GraphicsCommandListSlot::EndRenderPass, {});
    };
    const auto close = [&](GuestAddr list) {
        ASSERT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    };
    const auto passes_of = [&](graphics::BackendTexture2D* tex) {
        u32 n = 0;
        for (const auto& pass : h->backend.passes) {
            if (pass.color_targets[0] == tex) ++n;
        }
        return n;
    };
    const auto clears_of = [&](graphics::BackendTexture2D* tex) {
        std::vector<std::array<float, 4>> out;
        for (const auto& clear : h->backend.rt_clears) {
            if (clear.texture == tex) out.push_back(clear.color);
        }
        return out;
    };
    const auto execute = [&](std::initializer_list<GuestAddr> lists) {
        GuestAddr at = kArray;
        for (GuestAddr list : lists) {
            h->write_ptr(at, list);
            at += h->ptr_size();
        }
        (void)h->com(queue, CommandQueueSlot::ExecuteCommandLists,
                     {{static_cast<u32>(lists.size())}, {kArray}});
    };

    // Three lists, one ExecuteCommandLists: suspend, both, resume.
    write_rt(rtv0, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR, 1.0f, 0.0f,
             0.0f, 1.0f);
    GuestAddr list_a = 0;
    ASSERT_EQ(h->try_list(dev, alloc, list_a), S_OK);
    begin(list_a, kRt, D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS);
    draw(list_a);
    end(list_a);
    close(list_a);
    GuestAddr list_b = 0;
    ASSERT_EQ(h->try_list(dev, alloc, list_b), S_OK);
    begin(list_b, kRt,
          D3D12_RENDER_PASS_FLAG_RESUMING_PASS |
              D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS);
    draw(list_b);
    end(list_b);
    close(list_b);
    GuestAddr list_c = 0;
    ASSERT_EQ(h->try_list(dev, alloc, list_c), S_OK);
    begin(list_c, kRt, D3D12_RENDER_PASS_FLAG_RESUMING_PASS);
    draw(list_c);
    end(list_c);
    close(list_c);
    execute({list_a, list_b, list_c});
    const auto chain = clears_of(color0);
    ASSERT_EQ(chain.size(), 1u);
    EXPECT_FLOAT_EQ(chain[0][0], 1.0f);
    EXPECT_FLOAT_EQ(chain[0][1], 0.0f);
    EXPECT_TRUE(color0->has_color);
    EXPECT_FLOAT_EQ(color0->color[0], 1.0f);
    EXPECT_EQ(passes_of(color0), 3u);

    // Same list: a clear between suspend and resume does not replace the
    // local copy. The resume's own CLEAR is not applied again.
    GuestAddr list = 0;
    ASSERT_EQ(h->try_list(dev, alloc, list), S_OK);
    begin(list, kRt, D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS);
    draw(list);
    end(list);
    (void)h->com(list, GraphicsCommandListSlot::ClearRenderTargetView,
                 {{rtv0}, {kBlue}, {0}, {0}});
    begin(list, kRt, D3D12_RENDER_PASS_FLAG_RESUMING_PASS);
    draw(list);
    end(list);
    close(list);
    execute({list});
    const auto hazard = clears_of(color0);
    ASSERT_EQ(hazard.size(), 3u);
    EXPECT_FLOAT_EQ(hazard[1][0], 1.0f);
    EXPECT_FLOAT_EQ(hazard[2][2], 1.0f);
    EXPECT_FLOAT_EQ(color0->color[0], 1.0f);
    EXPECT_FLOAT_EQ(color0->color[2], 0.0f);
    EXPECT_EQ(passes_of(color0), 5u);

    // A later ExecuteCommandLists does not resume the previous suspend.
    write_rt(rtv1, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR, 0.0f, 1.0f,
             0.0f, 1.0f);
    GuestAddr green_list = 0;
    ASSERT_EQ(h->try_list(dev, alloc, green_list), S_OK);
    begin(green_list, kRt, D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS);
    end(green_list);
    close(green_list);
    execute({green_list});
    ASSERT_TRUE(color1->has_color);
    EXPECT_FLOAT_EQ(color1->color[1], 1.0f);
    GuestAddr blue_list = 0;
    ASSERT_EQ(h->try_list(dev, alloc, blue_list), S_OK);
    (void)h->com(blue_list, GraphicsCommandListSlot::ClearRenderTargetView,
                 {{rtv1}, {kBlue}, {0}, {0}});
    close(blue_list);
    execute({blue_list});
    EXPECT_FLOAT_EQ(color1->color[2], 1.0f);
    write_rt(rtv1, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE, 0.0f,
             1.0f, 0.0f, 1.0f);
    GuestAddr late = 0;
    ASSERT_EQ(h->try_list(dev, alloc, late), S_OK);
    begin(late, kRt, D3D12_RENDER_PASS_FLAG_RESUMING_PASS);
    end(late);
    close(late);
    execute({late});
    EXPECT_FLOAT_EQ(color1->color[2], 1.0f);
    EXPECT_FLOAT_EQ(color1->color[1], 0.0f);
    ASSERT_EQ(clears_of(color1).size(), 2u);

    // Same-list resume whose render target differs: Close returns E_FAIL.
    write_rt(rtv0, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR, 1.0f, 0.0f,
             0.0f, 1.0f);
    GuestAddr bad = 0;
    ASSERT_EQ(h->try_list(dev, alloc, bad), S_OK);
    begin(bad, kRt, D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS);
    end(bad);
    write_rt(rtv1, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR, 1.0f, 0.0f,
             0.0f, 1.0f);
    begin(bad, kRt, D3D12_RENDER_PASS_FLAG_RESUMING_PASS);
    EXPECT_EQ(h->com(bad, GraphicsCommandListSlot::Close, {}), E_FAIL);
    EXPECT_EQ(h->com(bad, GraphicsCommandListSlot::Reset, {{alloc}, {0}}),
              E_FAIL);

    // One depth-stencil view, depth accessed and stencil not: still loud.
    for (u32 i = 0; i < ds.size; i += 4) h->w32(kDs + i, 0);
    h->write_ptr(kDs + ds.cpuDescriptor_ptr, dsv24);
    h->w32(kDs + ds.DepthBeginningAccess_Type,
           D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR);
    h->w32(kDs + ds.StencilBeginningAccess_Type,
           D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS);
    h->w32(kDs + ds.DepthEndingAccess_Type,
           D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE);
    h->w32(kDs + ds.StencilEndingAccess_Type,
           D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS);
    GuestAddr depth_list = 0;
    ASSERT_EQ(h->try_list(dev, alloc, depth_list), S_OK);
    (void)h->com(depth_list, GraphicsCommandListSlot::BeginRenderPass,
                 {{0}, {0}, {kDs}, {D3D12_RENDER_PASS_FLAG_NONE}});
    EXPECT_EQ(h->com(depth_list, GraphicsCommandListSlot::Close, {}), E_FAIL);
}

// gs_arrays (fxc gs_5_1) reads cbv space 1, tex space 7 and smp space 2.
// Those are the same descriptor tables a vertex or pixel draw resolves; the
// expansion buffers stay on the flat set-4 window.
TEST_P(D3D12Test, GeometryShaderDescriptorTablesBindOnTheLoweredDispatch) {
    const GuestAddr dev = h->device();
    RootSignatureDesc rs;
    const auto table = [](u32 type, u32 base, u32 count, u32 space) {
        RootParameter parameter;
        parameter.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.shader_visibility = D3D12_SHADER_VISIBILITY_GEOMETRY;
        RootDescriptorRange range;
        range.range_type = type;
        range.num_descriptors = count;
        range.base_shader_register = base;
        range.register_space = space;
        parameter.ranges.push_back(range);
        return parameter;
    };
    rs.parameters.push_back(table(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 0, 3, 1));
    rs.parameters.push_back(table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 4, 7));
    rs.parameters.push_back(
        table(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 5, 3, 2));
    std::vector<u8> blob;
    std::string error;
    ASSERT_EQ(serialize_root_signature(rs, ROOT_SIGNATURE_VERSION_1_0, blob,
                                       error),
              S_OK)
        << error;
    const GuestAddr blob_at = kData + 0x10000;
    ASSERT_TRUE(h->memory.write_bytes(blob_at, blob.data(), blob.size()).is_ok());
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateRootSignature,
                     {{0}, {blob_at}, {blob.size()},
                      {h->iid(IID_ID3D12ROOTSIGNATURE)}, {kOut}}),
              S_OK);
    const GuestAddr root = h->read_ptr(kOut);
    prime_gfx(*h, root, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    const auto& l = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[GetParam()];
    put_shader(*h, kData + 0x11000, point_vertex_shader(), l.VS,
               l.VS_pShaderBytecode, l.VS_BytecodeLength);
    const std::vector<u8> gs(sm51_gs_arrays,
                             sm51_gs_arrays + sizeof(sm51_gs_arrays));
    put_shader(*h, kData + 0x12000, gs, l.GS, l.GS_pShaderBytecode,
               l.GS_BytecodeLength);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK);
    const GuestAddr pso = h->read_ptr(kOut);
    const GuestAddr queue = h->queue(dev);
    const GuestAddr alloc = h->allocator(dev);
    GuestAddr list = 0;
    ASSERT_EQ(h->try_list(dev, alloc, list), S_OK);
    (void)h->com(list, GraphicsCommandListSlot::SetPipelineState, {{pso}});
    (void)h->com(list, GraphicsCommandListSlot::SetGraphicsRootSignature,
                 {{root}});
    (void)h->com(list, GraphicsCommandListSlot::IASetPrimitiveTopology,
                 {{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST}});
    (void)h->com(list, GraphicsCommandListSlot::DrawInstanced,
                 {{3}, {1}, {0}, {0}});
    ASSERT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    h->write_ptr(kArray, list);
    (void)h->com(queue, CommandQueueSlot::ExecuteCommandLists, {{1}, {kArray}});
    ASSERT_FALSE(h->backend.dispatches.empty());
    const auto& expansion = h->backend.dispatches.front();
    EXPECT_TRUE(expansion.in0);
    EXPECT_TRUE(expansion.out1);
    EXPECT_TRUE(expansion.counter2);
    EXPECT_TRUE(expansion.ids3);
    EXPECT_TRUE(expansion.cbv_table);
    EXPECT_TRUE(expansion.srv_table);
    EXPECT_TRUE(expansion.sampler_table);
}

// Learn (CreateUnorderedAccessView): imm_atomic_alloc moves the counter
// resource named by the UAV descriptor, at CounterOffsetInBytes. A view with
// no counter resource does not invent one. A pipeline that never touches a
// counter is unchanged (GeometryShaderDrawsThroughTheLoweredPasses).
// ID3D12PipelineLibrary1, against tools/oracle_probe/
// d3d12_a11_library_cache_probe (WARP and NVIDIA agree).
TEST_P(D3D12Test, PipelineLibraryStoresLoadsAndSerializesAsMeasured) {
    const GuestAddr dev = h->device();
    RootSignatureDesc rs;
    RootParameter table;
    table.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    table.shader_visibility = D3D12_SHADER_VISIBILITY_ALL;
    RootDescriptorRange range;
    range.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.num_descriptors = 1;
    range.base_shader_register = 1;
    table.ranges.push_back(range);
    rs.parameters.push_back(table);
    std::vector<u8> blob;
    std::string error;
    ASSERT_EQ(serialize_root_signature(rs, ROOT_SIGNATURE_VERSION_1_0, blob,
                                       error),
              S_OK);
    const GuestAddr blob_at = kData + 0x10000;
    ASSERT_TRUE(h->memory.write_bytes(blob_at, blob.data(), blob.size()).is_ok());
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateRootSignature,
                     {{0}, {blob_at}, {blob.size()},
                      {h->iid(IID_ID3D12ROOTSIGNATURE)}, {kOut}}),
              S_OK);
    const GuestAddr root = h->read_ptr(kOut);
    const GuestAddr shader = kData + 0x11000;
    ASSERT_TRUE(h->memory
                    .write_bytes(shader, uav_counter_append_main,
                                 sizeof(uav_counter_append_main))
                    .is_ok());
    const auto& cl = layout::D3D12_COMPUTE_PIPELINE_STATE_DESC[GetParam()];
    for (u32 i = 0; i < cl.size; i += 4) h->w32(kDesc + i, 0);
    h->write_ptr(kDesc + cl.pRootSignature, root);
    h->write_ptr(kDesc + cl.CS_pShaderBytecode, shader);
    h->write_ptr(kDesc + cl.CS_BytecodeLength, sizeof(uav_counter_append_main));
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateComputePipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK);
    const GuestAddr pso = h->read_ptr(kOut);
    const GuestAddr name = kData + 0x12000;
    const GuestAddr other_name = kData + 0x12020;
    for (u32 i = 0; i < 3; ++i) {
        EXPECT_TRUE(h->memory.write_u16(name + i * 2, i < 2 ? u"cs"[i] : 0).is_ok());
        EXPECT_TRUE(h->memory.write_u16(other_name + i * 2, i < 2 ? u"xx"[i] : 0).is_ok());
    }
    EXPECT_EQ(h->com(dev, DeviceSlot::CreatePipelineLibrary,
                     {{0}, {0}, {h->iid(IID_ID3D12PIPELINELIBRARY)}, {0}}),
              S_FALSE);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreatePipelineLibrary,
                     {{0}, {0}, {h->iid(IID_ID3D12PIPELINELIBRARY1)}, {kOut}}),
              S_OK);
    const GuestAddr lib = h->read_ptr(kOut);
    EXPECT_EQ(h->com(lib, PipelineLibrarySlot::StorePipeline, {{name}, {pso}}), S_OK);
    EXPECT_EQ(h->com(lib, PipelineLibrarySlot::StorePipeline, {{name}, {pso}}),
              E_INVALIDARG);
    EXPECT_EQ(h->com(lib, PipelineLibrarySlot::StorePipeline, {{0}, {pso}}),
              E_INVALIDARG);
    // Load hands out the stored object; a missing name or the other kind is
    // E_INVALIDARG with *ppv NULL.
    ASSERT_EQ(h->com(lib, PipelineLibrarySlot::LoadComputePipeline,
                     {{name}, {kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK);
    EXPECT_EQ(h->read_ptr(kOut), pso);
    EXPECT_EQ(h->com(lib, PipelineLibrarySlot::LoadComputePipeline,
                     {{other_name}, {kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)},
                      {kOut}}),
              E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kOut), 0u);
    // Serialize: too small is E_INVALIDARG; the blob reloads.
    const u64 size = h->com(lib, PipelineLibrarySlot::GetSerializedSize, {});
    ASSERT_GT(size, 0u);
    const GuestAddr out_blob = kData + 0x13000;
    EXPECT_EQ(h->com(lib, PipelineLibrarySlot::Serialize, {{out_blob}, {size - 1}}),
              E_INVALIDARG);
    ASSERT_EQ(h->com(lib, PipelineLibrarySlot::Serialize, {{out_blob}, {size}}), S_OK);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreatePipelineLibrary,
                     {{out_blob}, {size}, {h->iid(IID_ID3D12PIPELINELIBRARY1)},
                      {kOut}}),
              S_OK);
    const GuestAddr lib2 = h->read_ptr(kOut);
    EXPECT_EQ(h->com(lib2, PipelineLibrarySlot::StorePipeline, {{name}, {pso}}),
              E_INVALIDARG);
    // A byte flipped in the entries, and a truncated blob: E_INVALIDARG.
    const u8 last = h->memory.read_u8(out_blob + size - 5).value();
    EXPECT_TRUE(h->memory.write_u8(out_blob + size - 5, last ^ 0x5A).is_ok());
    EXPECT_EQ(h->com(dev, DeviceSlot::CreatePipelineLibrary,
                     {{out_blob}, {size}, {h->iid(IID_ID3D12PIPELINELIBRARY1)},
                      {kOut}}),
              E_INVALIDARG);
    EXPECT_EQ(h->com(dev, DeviceSlot::CreatePipelineLibrary,
                     {{out_blob}, {size / 2}, {h->iid(IID_ID3D12PIPELINELIBRARY1)},
                      {kOut}}),
              E_INVALIDARG);
    // ShaderCacheSession: defaults, values, errors.
    const auto& sd = layout::D3D12_SHADER_CACHE_SESSION_DESC[GetParam()];
    for (u32 i = 0; i < sd.size; i += 4) h->w32(kDesc2 + i, 0);
    h->w32(kDesc2 + sd.Identifier, 0x1234);
    h->w32(kDesc2 + sd.MaximumInMemoryCacheSizeBytes, 36864);
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateShaderCacheSession,
                     {{kDesc2}, {h->iid(IID_ID3D12SHADERCACHESESSION)}, {kOut}}),
              E_INVALIDARG);
    h->w32(kDesc2 + sd.MaximumInMemoryCacheSizeBytes, 0);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateShaderCacheSession,
                     {{kDesc2}, {h->iid(IID_ID3D12SHADERCACHESESSION)}, {kOut}}),
              S_OK);
    const GuestAddr session = h->read_ptr(kOut);
    EXPECT_EQ(h->com(session, ShaderCacheSessionSlot::GetDesc, {{kArray}}), kArray);
    EXPECT_EQ(h->r32(kArray + sd.MaximumInMemoryCacheSizeBytes), 1048576u);
    EXPECT_EQ(h->r32(kArray + sd.MaximumInMemoryCacheEntries), 128u);
    const GuestAddr key = kData + 0x12100;
    const GuestAddr value = kData + 0x12200;
    const GuestAddr value_size = kData + 0x12300;
    h->w32(key, 0x6B6579);
    h->w32(value, 0x11223344);
    EXPECT_EQ(h->com(session, ShaderCacheSessionSlot::StoreValue,
                     {{key}, {4}, {value}, {4}}),
              S_OK);
    EXPECT_EQ(h->com(session, ShaderCacheSessionSlot::StoreValue,
                     {{key}, {4}, {value}, {4}}),
              DXGI_ERROR_ALREADY_EXISTS);
    h->w32(value_size, 2);
    EXPECT_EQ(h->com(session, ShaderCacheSessionSlot::FindValue,
                     {{key}, {4}, {kArray}, {value_size}}),
              DXGI_ERROR_MORE_DATA);
    EXPECT_EQ(h->r32(value_size), 4u);
    EXPECT_EQ(h->com(session, ShaderCacheSessionSlot::FindValue,
                     {{key}, {3}, {0}, {value_size}}),
              DXGI_ERROR_NOT_FOUND);
    // Another version while a session is open.
    h->w64(kDesc2 + sd.Version, 2);
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateShaderCacheSession,
                     {{kDesc2}, {h->iid(IID_ID3D12SHADERCACHESESSION)}, {kOut}}),
              DXGI_ERROR_ALREADY_EXISTS);
    // Controls (no developer mode).
    EXPECT_EQ(h->com(dev, DeviceSlot::ShaderCacheControl,
                     {{D3D12_SHADER_CACHE_KIND_FLAG_APPLICATION_MANAGED},
                      {D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR}}),
              DXGI_ERROR_SDK_COMPONENT_MISSING);
    EXPECT_EQ(h->com(dev, DeviceSlot::ShaderCacheControl,
                     {{0}, {D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR}}),
              E_INVALIDARG);
    EXPECT_EQ(h->com(dev, DeviceSlot::SetBackgroundProcessingMode,
                     {{0}, {0}, {0}, {0}}),
              S_OK);
    EXPECT_EQ(h->com(dev, DeviceSlot::SetBackgroundProcessingMode,
                     {{D3D12_BACKGROUND_PROCESSING_MODE_DISABLE_BACKGROUND_WORK},
                      {0}, {0}, {0}}),
              DXGI_ERROR_SDK_COMPONENT_MISSING);
}

// State objects without libraries and lifetime trackers, against
// d3d12_a12_sessions_probe.
TEST_P(D3D12Test, StateObjectsAndLifetimeTrackersAnswerAsMeasured) {
    const GuestAddr dev = h->device();
    const auto& so = layout::D3D12_STATE_OBJECT_DESC[GetParam()];
    const auto& sub = layout::D3D12_STATE_SUBOBJECT[GetParam()];
    h->w32(kDesc + so.Type, D3D12_STATE_OBJECT_TYPE_COLLECTION);
    h->w32(kDesc + so.NumSubobjects, 1);
    h->write_ptr(kDesc + so.pSubobjects, kDesc2);
    h->w32(kDesc2 + sub.Type, D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG);
    h->write_ptr(kDesc2 + sub.pDesc, kArray);
    h->w32(kArray, 0);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateStateObject,
                     {{kDesc}, {h->iid(IID_ID3D12STATEOBJECT)}, {kOut}}),
              S_OK);
    const GuestAddr object = h->read_ptr(kOut);
    ASSERT_EQ(h->com(object, StateObjectSlot::QueryInterface,
                     {{h->iid(IID_ID3D12STATEOBJECTPROPERTIES1)}, {kOut2}}),
              S_OK);
    const GuestAddr props = h->read_ptr(kOut2);
    EXPECT_EQ(h->com(props, StateObjectPropertiesSlot::GetShaderIdentifier,
                     {{kIid}}),
              0u);
    EXPECT_EQ(h->com(props, StateObjectPropertiesSlot::GetPipelineStackSize, {},
                     true),
              0xFFFFFFFFull);
    // Config flags beyond the three, an unknown subobject: E_INVALIDARG.
    h->w32(kArray, 0x80);
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateStateObject,
                     {{kDesc}, {h->iid(IID_ID3D12STATEOBJECT)}, {kOut}}),
              E_INVALIDARG);
    h->w32(kDesc2 + sub.Type, 99);
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateStateObject,
                     {{kDesc}, {h->iid(IID_ID3D12STATEOBJECT)}, {kOut}}),
              E_INVALIDARG);
    // CreateLifetimeTracker: NULL owner E_INVALIDARG, ppv NULL E_UNEXPECTED.
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateLifetimeTracker,
                     {{0}, {h->iid(IID_ID3D12LIFETIMETRACKER)}, {kOut}}),
              E_INVALIDARG);
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateLifetimeTracker,
                     {{kArray}, {h->iid(IID_ID3D12LIFETIMETRACKER)}, {0}}),
              E_UNEXPECTED);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateLifetimeTracker,
                     {{kArray}, {h->iid(IID_ID3D12LIFETIMETRACKER)}, {kOut}}),
              S_OK);
    EXPECT_EQ(h->com(h->read_ptr(kOut), LifetimeTrackerSlot::DestroyOwnedObject,
                     {{object}}),
              E_INVALIDARG);
    // No protected content: CreateProtectedResourceSession is E_INVALIDARG.
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateProtectedResourceSession,
                     {{kArray}, {h->iid(IID_ID3D12PROTECTEDRESOURCESESSION)},
                      {kOut}}),
              E_INVALIDARG);
}

TEST_P(D3D12Test, ComputeShaderUavCounterUsesTheDescriptorsCounterResource) {
    const GuestAddr dev = h->device();
    RootSignatureDesc rs;
    RootParameter table;
    table.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    table.shader_visibility = D3D12_SHADER_VISIBILITY_ALL;
    RootDescriptorRange range;
    range.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.num_descriptors = 1;
    range.base_shader_register = 1;
    table.ranges.push_back(range);
    rs.parameters.push_back(table);
    std::vector<u8> blob;
    std::string error;
    ASSERT_EQ(serialize_root_signature(rs, ROOT_SIGNATURE_VERSION_1_0, blob,
                                       error),
              S_OK)
        << error;
    const GuestAddr blob_at = kData + 0x10000;
    ASSERT_TRUE(h->memory.write_bytes(blob_at, blob.data(), blob.size()).is_ok());
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateRootSignature,
                     {{0}, {blob_at}, {blob.size()},
                      {h->iid(IID_ID3D12ROOTSIGNATURE)}, {kOut}}),
              S_OK);
    const GuestAddr root = h->read_ptr(kOut);
    const GuestAddr shader = kData + 0x11000;
    ASSERT_TRUE(h->memory
                    .write_bytes(shader, uav_counter_append_main,
                                 sizeof(uav_counter_append_main))
                    .is_ok());
    const auto& cl = layout::D3D12_COMPUTE_PIPELINE_STATE_DESC[GetParam()];
    for (u32 i = 0; i < cl.size; i += 4) h->w32(kDesc + i, 0);
    h->write_ptr(kDesc + cl.pRootSignature, root);
    h->write_ptr(kDesc + cl.CS_pShaderBytecode, shader);
    h->write_ptr(kDesc + cl.CS_BytecodeLength, sizeof(uav_counter_append_main));
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateComputePipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK);
    const GuestAddr pso = h->read_ptr(kOut);

    const auto& hd = layout::D3D12_DESCRIPTOR_HEAP_DESC[GetParam()];
    h->w32(kDesc + hd.Type, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    h->w32(kDesc + hd.NumDescriptors, 1);
    h->w32(kDesc + hd.Flags, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    h->w32(kDesc + hd.NodeMask, 0);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateDescriptorHeap,
                     {{kDesc}, {h->iid(IID_ID3D12DESCRIPTORHEAP)}, {kOut}}),
              S_OK);
    const GuestAddr heap = h->read_ptr(kOut);
    h->w64(kOut2, 0);
    ASSERT_EQ(h->com(heap, DescriptorHeapSlot::GetCPUDescriptorHandleForHeapStart,
                     {{kOut2}}),
              GetParam() ? kOut2 : static_cast<u32>(kOut2));
    const u64 cpu = h->read_ptr(kOut2);
    h->w64(kOut2, 0);
    ASSERT_EQ(h->com(heap, DescriptorHeapSlot::GetGPUDescriptorHandleForHeapStart,
                     {{kOut2}}),
              GetParam() ? kOut2 : static_cast<u32>(kOut2));
    const u64 gpu = h->r64(kOut2);

    GuestAddr data = 0;
    GuestAddr counter = 0;
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 256,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, data,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
              S_OK);
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 8192,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, counter,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
              S_OK);
    const auto& ul = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[GetParam()];
    const auto write_uav = [&](GuestAddr counter_resource, u64 counter_offset) {
        for (u32 i = 0; i < ul.size; i += 4) h->w32(kDesc + i, 0);
        h->w32(kDesc + ul.Format, DXGI_FORMAT_UNKNOWN);
        h->w32(kDesc + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
        h->w64(kDesc + ul.Buffer_FirstElement, 0);
        h->w32(kDesc + ul.Buffer_NumElements, 4);
        h->w32(kDesc + ul.Buffer_StructureByteStride, 8);
        h->w64(kDesc + ul.Buffer_CounterOffsetInBytes, counter_offset);
        h->w32(kDesc + ul.Buffer_Flags, 0);
        (void)h->com(dev, DeviceSlot::CreateUnorderedAccessView,
                     {{data}, {counter_resource}, {kDesc}, {cpu}});
    };
    const GuestAddr queue = h->queue(dev);
    const auto dispatch_once = [&]() {
        const GuestAddr alloc = h->allocator(dev);
        GuestAddr list = 0;
        EXPECT_EQ(h->try_list(dev, alloc, list), S_OK);
        if (list == 0) return;
        (void)h->com(list, GraphicsCommandListSlot::SetPipelineState, {{pso}});
        (void)h->com(list, GraphicsCommandListSlot::SetComputeRootSignature,
                     {{root}});
        h->write_ptr(kArray, heap);
        (void)h->com(list, GraphicsCommandListSlot::SetDescriptorHeaps,
                     {{1}, {kArray}});
        (void)h->com(list, GraphicsCommandListSlot::SetComputeRootDescriptorTable,
                     {{0}, Harness::wide(gpu)});
        (void)h->com(list, GraphicsCommandListSlot::Dispatch, {{1}, {1}, {1}});
        EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
        h->write_ptr(kArray, list);
        (void)h->com(queue, CommandQueueSlot::ExecuteCommandLists,
                     {{1}, {kArray}});
    };

    h->backend.dispatches.clear();
    write_uav(0, 0);
    dispatch_once();
    EXPECT_TRUE(h->backend.dispatches.empty());

    h->backend.dispatches.clear();
    write_uav(counter, 4096);
    dispatch_once();
    ASSERT_EQ(h->backend.dispatches.size(), 1u);
    EXPECT_NE(h->backend.dispatches[0].uav_counter, nullptr);
    // Learn: CounterOffsetInBytes is the byte offset of the counter in that
    // resource. D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT is 4096.
    EXPECT_EQ(h->backend.dispatches[0].uav_counter_offset, 4096u);
}

// Learn (D3D12_DRAW_ARGUMENTS): VertexCountPerInstance, InstanceCount,
// StartVertexLocation, StartInstanceLocation. A bare ExecuteIndirect leaves
// those words in the argument buffer, and the lowered geometry draw
// assembles from them on the GPU timeline (vitreo_ia_resolve).
TEST_P(D3D12Test, IndirectGeometryDrawReadsTheArgumentBuffer) {
    const GuestAddr dev = h->device();
    const GuestAddr root = make_root(*h, dev, 0);
    prime_gfx(*h, root, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);
    const auto& l = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[GetParam()];
    put_shader(*h, kData + 0x2000, point_vertex_shader(), l.VS,
               l.VS_pShaderBytecode, l.VS_BytecodeLength);
    put_shader(*h, kData + 0x3000, point_geometry_shader(), l.GS,
               l.GS_pShaderBytecode, l.GS_BytecodeLength);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK);
    const GuestAddr pso = h->read_ptr(kOut);

    const auto& sig = layout::D3D12_COMMAND_SIGNATURE_DESC[GetParam()];
    const auto& arg = layout::D3D12_INDIRECT_ARGUMENT_DESC[GetParam()];
    const GuestAddr arg_at = kData + 0x5000;
    const GuestAddr sig_at = kData + 0x5100;
    for (u32 i = 0; i < arg.size; i += 4) h->w32(arg_at + i, 0);
    h->w32(arg_at + arg.Type, D3D12_INDIRECT_ARGUMENT_TYPE_DRAW);
    for (u32 i = 0; i < sig.size; i += 4) h->w32(sig_at + i, 0);
    h->w32(sig_at + sig.ByteStride, 16);
    h->w32(sig_at + sig.NumArgumentDescs, 1);
    h->write_ptr(sig_at + sig.pArgumentDescs, arg_at);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateCommandSignature,
                     {{sig_at}, {0}, {h->iid(IID_ID3D12COMMANDSIGNATURE)}, {kOut}}),
              S_OK);
    const GuestAddr signature = h->read_ptr(kOut);

    GuestAddr args = 0;
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 256,
                        D3D12_RESOURCE_STATE_GENERIC_READ, args),
              S_OK);
    ASSERT_EQ(h->com(args, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    const GuestAddr mapped = h->read_ptr(kOut2);
    // 5 vertices, 2 instances, starting at vertex 3 of instance 4.
    h->w32(mapped + 0, 5);
    h->w32(mapped + 4, 2);
    h->w32(mapped + 8, 3);
    h->w32(mapped + 12, 4);

    const GuestAddr queue = h->queue(dev);
    const GuestAddr alloc = h->allocator(dev);
    GuestAddr list = 0;
    ASSERT_EQ(h->try_list(dev, alloc, list), S_OK);
    (void)h->com(list, GraphicsCommandListSlot::SetPipelineState, {{pso}});
    (void)h->com(list, GraphicsCommandListSlot::SetGraphicsRootSignature,
                 {{root}});
    (void)h->com(list, GraphicsCommandListSlot::IASetPrimitiveTopology,
                 {{D3D_PRIMITIVE_TOPOLOGY_POINTLIST}});
    (void)h->com(list, GraphicsCommandListSlot::ExecuteIndirect,
                 {{signature}, {1}, {args}, Harness::wide(0), {0},
                  Harness::wide(0)});
    ASSERT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    h->write_ptr(kArray, list);
    const u64 waits_before = d3d12::g_replay_cpu_waits.load();
    (void)h->com(queue, CommandQueueSlot::ExecuteCommandLists, {{1}, {kArray}});

    // The counts stay GPU data: vitreo_ia_resolve reads the argument block
    // (the first dispatch), one capture draw is indirect from the block it
    // resolved, the expansion is sized for the capacity and stops at the
    // resolved count, and one expanded draw follows. Nothing is read back.
    ASSERT_EQ(h->backend.draws.size(), 2u);
    EXPECT_TRUE(h->backend.draws[0].capture);
    EXPECT_TRUE(h->backend.draws[0].indirect);
    EXPECT_FALSE(h->backend.draws[1].capture);
    ASSERT_EQ(h->backend.dispatches.size(), 2u);
    EXPECT_EQ(h->backend.dispatches[0].groups, 1u);
    EXPECT_GT(h->backend.dispatches[1].groups, 5u);
    EXPECT_EQ(d3d12::g_replay_cpu_waits.load(), waits_before)
        << "the indirect geometry draw waited for the GPU";
}

// Learn (D3D12_BUFFER_UAV::CounterOffsetInBytes): a pixel shader's
// imm_atomic_alloc moves the same counter a compute dispatch does, at the
// descriptor's byte offset. Offset 0 stays the start of the resource.
TEST_P(D3D12Test, PixelShaderUavCounterUsesTheDescriptorsCounterOffset) {
    const GuestAddr dev = h->device();
    RootSignatureDesc rs;
    RootParameter table;
    table.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    table.shader_visibility = D3D12_SHADER_VISIBILITY_ALL;
    RootDescriptorRange range;
    range.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.num_descriptors = 1;
    range.base_shader_register = 1;
    table.ranges.push_back(range);
    rs.parameters.push_back(table);
    std::vector<u8> blob;
    std::string error;
    ASSERT_EQ(serialize_root_signature(rs, ROOT_SIGNATURE_VERSION_1_0, blob,
                                       error),
              S_OK)
        << error;
    const GuestAddr blob_at = kData + 0x10000;
    ASSERT_TRUE(h->memory.write_bytes(blob_at, blob.data(), blob.size()).is_ok());
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateRootSignature,
                     {{0}, {blob_at}, {blob.size()},
                      {h->iid(IID_ID3D12ROOTSIGNATURE)}, {kOut}}),
              S_OK);
    const GuestAddr root = h->read_ptr(kOut);
    prime_gfx(*h, root, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);
    const auto& l = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[GetParam()];
    h->w32(kDesc + l.NumRenderTargets, 1);
    h->w32(kDesc + l.RTVFormats, DXGI_FORMAT_R8G8B8A8_UNORM);
    put_shader(*h, kData + 0x12000, point_vertex_shader(), l.VS,
               l.VS_pShaderBytecode, l.VS_BytecodeLength);
    const std::vector<u8> ps(uav_counter_ps_main,
                             uav_counter_ps_main + sizeof(uav_counter_ps_main));
    put_shader(*h, kData + 0x13000, ps, l.PS, l.PS_pShaderBytecode,
               l.PS_BytecodeLength);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK);
    const GuestAddr pso = h->read_ptr(kOut);

    const auto& hd = layout::D3D12_DESCRIPTOR_HEAP_DESC[GetParam()];
    h->w32(kDesc + hd.Type, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    h->w32(kDesc + hd.NumDescriptors, 1);
    h->w32(kDesc + hd.Flags, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    h->w32(kDesc + hd.NodeMask, 0);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateDescriptorHeap,
                     {{kDesc}, {h->iid(IID_ID3D12DESCRIPTORHEAP)}, {kOut}}),
              S_OK);
    const GuestAddr heap = h->read_ptr(kOut);
    h->w64(kOut2, 0);
    ASSERT_EQ(h->com(heap, DescriptorHeapSlot::GetCPUDescriptorHandleForHeapStart,
                     {{kOut2}}),
              GetParam() ? kOut2 : static_cast<u32>(kOut2));
    const u64 cpu = h->read_ptr(kOut2);
    h->w64(kOut2, 0);
    ASSERT_EQ(h->com(heap, DescriptorHeapSlot::GetGPUDescriptorHandleForHeapStart,
                     {{kOut2}}),
              GetParam() ? kOut2 : static_cast<u32>(kOut2));
    const u64 gpu = h->r64(kOut2);

    GuestAddr data = 0;
    GuestAddr counter = 0;
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 256,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, data,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
              S_OK);
    ASSERT_EQ(h->buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 8192,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, counter,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
              S_OK);
    const auto& ul = layout::D3D12_UNORDERED_ACCESS_VIEW_DESC[GetParam()];
    const auto write_uav = [&](GuestAddr counter_resource, u64 counter_offset) {
        for (u32 i = 0; i < ul.size; i += 4) h->w32(kDesc + i, 0);
        h->w32(kDesc + ul.Format, DXGI_FORMAT_UNKNOWN);
        h->w32(kDesc + ul.ViewDimension, D3D12_UAV_DIMENSION_BUFFER);
        h->w64(kDesc + ul.Buffer_FirstElement, 0);
        h->w32(kDesc + ul.Buffer_NumElements, 4);
        h->w32(kDesc + ul.Buffer_StructureByteStride, 4);
        h->w64(kDesc + ul.Buffer_CounterOffsetInBytes, counter_offset);
        h->w32(kDesc + ul.Buffer_Flags, 0);
        (void)h->com(dev, DeviceSlot::CreateUnorderedAccessView,
                     {{data}, {counter_resource}, {kDesc}, {cpu}});
    };
    const GuestAddr queue = h->queue(dev);
    const auto draw_once = [&]() {
        const GuestAddr alloc = h->allocator(dev);
        GuestAddr list = 0;
        EXPECT_EQ(h->try_list(dev, alloc, list), S_OK);
        if (list == 0) return;
        (void)h->com(list, GraphicsCommandListSlot::SetPipelineState, {{pso}});
        (void)h->com(list, GraphicsCommandListSlot::SetGraphicsRootSignature,
                     {{root}});
        h->write_ptr(kArray, heap);
        (void)h->com(list, GraphicsCommandListSlot::SetDescriptorHeaps,
                     {{1}, {kArray}});
        (void)h->com(list, GraphicsCommandListSlot::SetGraphicsRootDescriptorTable,
                     {{0}, Harness::wide(gpu)});
        (void)h->com(list, GraphicsCommandListSlot::IASetPrimitiveTopology,
                     {{D3D_PRIMITIVE_TOPOLOGY_POINTLIST}});
        (void)h->com(list, GraphicsCommandListSlot::DrawInstanced,
                     {{1}, {1}, {0}, {0}});
        EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
        h->write_ptr(kArray, list);
        (void)h->com(queue, CommandQueueSlot::ExecuteCommandLists,
                     {{1}, {kArray}});
    };

    h->backend.draws.clear();
    write_uav(0, 0);
    draw_once();
    EXPECT_TRUE(h->backend.draws.empty());

    h->backend.draws.clear();
    write_uav(counter, 0);
    draw_once();
    ASSERT_EQ(h->backend.draws.size(), 1u);
    EXPECT_NE(h->backend.draws[0].uav_counter, nullptr);
    EXPECT_EQ(h->backend.draws[0].uav_counter_offset, 0u);

    h->backend.draws.clear();
    write_uav(counter, 4096);
    draw_once();
    ASSERT_EQ(h->backend.draws.size(), 1u);
    EXPECT_NE(h->backend.draws[0].uav_counter, nullptr);
    EXPECT_EQ(h->backend.draws[0].uav_counter_offset, 4096u);
}

// Learn (CreateVirtualSurface, Scroll, Trim, Resize): the logical surface
// can be far larger than the pixels BeginDraw has realized. Scroll copies
// scrollRect ∩ clipRect to (that rectangle + offset) ∩ clipRect ∩ surface,
// positive offsets move right and down, and the exposed source is no longer
// shown. Trim keeps only the given rectangles. An edge above 16,777,216
// fails. A realization that is not one rectangle is presented as one layer
// per rectangle.
TEST_P(D3D12Test, CompositionVirtualSurfaceScrollMovesTheRealizedRectangle) {
    attach_window(*h);
    const GuestAddr queue = h->queue(h->device());

    DxgiManager dxgi(h->memory, GetParam());
    D3D11Manager d3d11(h->memory, GetParam(), h->backend, dxgi);
    DcompManager dcomp(h->memory, GetParam(), d3d11);
    DcompManager::MethodThunks thunks{};
    GuestAddr thunk = 0x76000000;
    const auto fill = [&](auto& methods) {
        for (GuestAddr& method : methods) {
            method = thunk;
            thunk += 0x10;
        }
    };
    fill(thunks.device);
    fill(thunks.device2);
    fill(thunks.desktop_device);
    fill(thunks.device3);
    fill(thunks.target);
    fill(thunks.visual2);
    fill(thunks.translate_transform);
    fill(thunks.scale_transform);
    fill(thunks.rotate_transform);
    fill(thunks.skew_transform);
    fill(thunks.matrix_transform);
    fill(thunks.rectangle_clip);
    fill(thunks.surface);
    fill(thunks.update_object);
    dcomp.set_method_thunks(thunks);
    dcomp.set_window_ownership_resolver([](u64 hwnd) {
        return hwnd == kHwnd ? DcompManager::WindowOwnership::CurrentProcess
                             : DcompManager::WindowOwnership::Invalid;
    });

    auto exports = create_dcomp_thunks(dcomp);
    const auto find_export = [&](const char* name) -> const ExportEntry* {
        for (const ExportEntry& entry : exports.entries) {
            if (entry.name == name) return &entry;
        }
        return nullptr;
    };
    // dcomp.h: Resize(this, width, height) and Trim(this, rects, count)
    // are 12 bytes of x86 stack.
    ASSERT_NE(find_export("_DComp_Surface_8"), nullptr);
    ASSERT_NE(find_export("_DComp_Surface_9"), nullptr);
    EXPECT_EQ(find_export("_DComp_Surface_8")->stdcall_bytes, 12u);
    EXPECT_EQ(find_export("_DComp_Surface_9")->stdcall_bytes, 12u);

    constexpr GuestAddr kDcompIid = kData + 0x2100;
    constexpr GuestAddr kDeviceOut = kData + 0x2120;
    constexpr GuestAddr kVisualOut = kData + 0x2140;
    constexpr GuestAddr kTargetOut = kData + 0x2160;
    constexpr GuestAddr kSurfaceOut = kData + 0x2180;
    constexpr GuestAddr kUpdateOut = kData + 0x21A0;
    constexpr GuestAddr kOffset = kData + 0x21C0;
    constexpr GuestAddr kRect = kData + 0x21E0;
    constexpr GuestAddr kRect2 = kRect + 16;
    constexpr GuestAddr kMapped = kData + 0x2220;
    constexpr GuestAddr kSurfDesc = kData + 0x2240;
    constexpr GuestAddr kQiOut = kData + 0x2260;
    constexpr GuestAddr kDxgiIid = kData + 0x2280;
    constexpr GuestAddr kSurfaceIid = kData + 0x22A0;
    constexpr GuestAddr kVirtualIid = kData + 0x22C0;
    const u8 device_iid[16] = {0x3A, 0xA9, 0x7E, 0xC3, 0xAA, 0xE7, 0x0D, 0x45,
                               0xB1, 0x6F, 0x97, 0x46, 0xCB, 0x04, 0x07, 0xF3};
    const u8 desktop_iid[16] = {0xFE, 0x33, 0x46, 0x5F, 0x08, 0x1E, 0xB8, 0x4C,
                                0x8C, 0x75, 0xCE, 0x24, 0x33, 0x3F, 0x56, 0x02};
    const u8 surface_iid[16] = {0x53, 0x49, 0x8A, 0xBB, 0x99, 0x2C, 0x5A, 0x4F,
                                0x96, 0xF5, 0x48, 0x19, 0x02, 0x7F, 0xA3, 0xAC};
    // dcomp.h IDCompositionVirtualSurface.
    const u8 virtual_iid[16] = {0x51, 0x1C, 0x47, 0xAE, 0x53, 0x5F, 0x24, 0x4A,
                                0x8D, 0x3E, 0xD0, 0xC3, 0x9C, 0x30, 0xB3, 0xF0};
    const u8 dxgi_surface_iid[16] = {
        0x6C, 0xB5, 0xFC, 0xCA, 0xC3, 0x6A, 0x89, 0x48,
        0xBF, 0x47, 0x9E, 0x23, 0xBB, 0xD2, 0x60, 0xEC};
    ASSERT_TRUE(h->memory.write_bytes(kDxgiIid, dxgi_surface_iid, 16).is_ok());
    ASSERT_TRUE(h->memory.write_bytes(kSurfaceIid, surface_iid, 16).is_ok());
    ASSERT_TRUE(h->memory.write_bytes(kVirtualIid, virtual_iid, 16).is_ok());

    auto dcall = [&](auto&& fn, std::vector<Harness::Arg> args,
                     float xmm1 = 0.0f) {
        h->cpu.sse.xmm[1].f32_[0] = xmm1;
        return h->invoke([&](Marshaler& m) { fn(m); }, args, false, 0, "");
    };
    const auto farg = [](float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return static_cast<u64>(bits);
    };
    auto write_rect = [&](GuestAddr at, i32 l, i32 t, i32 r, i32 b) {
        h->w32(at + 0, static_cast<u32>(l));
        h->w32(at + 4, static_cast<u32>(t));
        h->w32(at + 8, static_cast<u32>(r));
        h->w32(at + 12, static_cast<u32>(b));
    };
    auto make_desktop = [&]() -> GuestAddr {
        EXPECT_TRUE(h->memory.write_bytes(kDcompIid, desktop_iid, 16).is_ok());
        EXPECT_EQ(dcall([&](Marshaler& m) {
                      dcomp.impl_DCompositionCreateDevice3(m);
                  },
                        {{queue}, {kDcompIid}, {kDeviceOut}}),
                  dcomp::S_OK);
        return h->read_ptr(kDeviceOut);
    };
    const GuestAddr composition = make_desktop();

    auto create_virtual = [&](u32 width, u32 height, u32 format, u32 alpha) {
        h->write_ptr(kSurfaceOut, 0x111);
        return dcall([&](Marshaler& m) {
                   dcomp.impl_DeviceMethod(
                       m, DcompManager::DeviceInterface::Desktop, 9);
               },
                     {{composition}, {width}, {height}, {format}, {alpha},
                      {kSurfaceOut}});
    };

    EXPECT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 9);
              },
                    {{composition}, {4}, {4}, {DXGI_FORMAT_B8G8R8A8_UNORM},
                     {1}, {0}}),
              dcomp::E_INVALIDARG);
    EXPECT_EQ(create_virtual(4, 4, DXGI_FORMAT_R8_UNORM, 1), dcomp::E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kSurfaceOut), 0u);
    h->write_ptr(kSurfaceOut, 0x111);
    EXPECT_EQ(create_virtual(4, 4, DXGI_FORMAT_B8G8R8A8_UNORM, 2),
              dcomp::E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kSurfaceOut), 0u);
    h->write_ptr(kSurfaceOut, 0x111);
    EXPECT_EQ(create_virtual(16777217u, 1, DXGI_FORMAT_B8G8R8A8_UNORM, 1),
              dcomp::E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kSurfaceOut), 0u);
    h->write_ptr(kSurfaceOut, 0x111);
    EXPECT_EQ(create_virtual(1, 16777217u, DXGI_FORMAT_R8G8B8A8_UNORM, 3),
              dcomp::E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kSurfaceOut), 0u);

    // The documented maximum is inclusive, and it does not allocate the
    // logical bitmap.
    const usize textures_before = h->backend.textures.size();
    ASSERT_EQ(create_virtual(16777216u, 1, DXGI_FORMAT_B8G8R8A8_UNORM, 0),
              dcomp::S_OK);
    EXPECT_EQ(h->backend.textures.size(), textures_before);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 2); },
                    {{h->read_ptr(kSurfaceOut)}}),
              0u);
    ASSERT_EQ(create_virtual(0, 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 1),
              dcomp::S_OK);
    EXPECT_EQ(h->backend.textures.size(), textures_before);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 2); },
                    {{h->read_ptr(kSurfaceOut)}}),
              0u);

    ASSERT_EQ(create_virtual(16777216u, 16777216u, DXGI_FORMAT_B8G8R8A8_UNORM, 1),
              dcomp::S_OK);
    const GuestAddr huge = h->read_ptr(kSurfaceOut);
    EXPECT_EQ(h->backend.textures.size(), textures_before);
    h->write_ptr(kQiOut, 0);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 0); },
                    {{huge}, {kVirtualIid}, {kQiOut}}),
              dcomp::S_OK);
    EXPECT_EQ(h->read_ptr(kQiOut), huge);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 2); },
                    {{huge}}),
              1u);
    auto begin = [&](GuestAddr which, GuestAddr rect) {
        h->write_ptr(kUpdateOut, 0x111);
        h->w32(kOffset, 0x11111111u);
        h->w32(kOffset + 4, 0x22222222u);
        return dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 3); },
                     {{which}, {rect}, {kDxgiIid}, {kUpdateOut}, {kOffset}});
    };
    // NULL updates the entire logical surface. That bitmap cannot be stored.
    EXPECT_EQ(begin(huge, 0), dcomp::E_OUTOFMEMORY);
    EXPECT_EQ(h->read_ptr(kUpdateOut), 0u);
    EXPECT_EQ(h->r32(kOffset), 0u);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 4); },
                    {{huge}}),
              dcomp::DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED);
    write_rect(kRect, 1, 2, 3, 4);
    ASSERT_EQ(begin(huge, kRect), dcomp::S_OK);
    EXPECT_EQ(h->r32(kOffset), 0u);
    EXPECT_EQ(h->r32(kOffset + 4), 0u);
    const GuestAddr huge_update = h->read_ptr(kUpdateOut);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 8); },
                    {{huge_update}, {kSurfDesc}}),
              dcomp::S_OK);
    EXPECT_EQ(h->r32(kSurfDesc + 0), 2u);
    EXPECT_EQ(h->r32(kSurfDesc + 4), 2u);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 4); },
                    {{huge}}),
              dcomp::S_OK);
    ASSERT_FALSE(h->backend.texture_uploads.empty());
    EXPECT_EQ(h->backend.texture_uploads.back().pitch, 8u);
    EXPECT_EQ(h->backend.texture_uploads.back().bytes.size(), 16u);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 2); },
                    {{huge}}),
              0u);

    // A normal surface does not answer the virtual-surface IID, and Resize
    // is not a method it can honour.
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 8);
              },
                    {{composition}, {4}, {1}, {DXGI_FORMAT_B8G8R8A8_UNORM},
                     {1}, {kSurfaceOut}}),
              dcomp::S_OK);
    const GuestAddr ordinary = h->read_ptr(kSurfaceOut);
    h->write_ptr(kQiOut, 0x111);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 0); },
                    {{ordinary}, {kVirtualIid}, {kQiOut}}),
              dcomp::E_NOINTERFACE);
    EXPECT_EQ(h->read_ptr(kQiOut), 0u);
    ASSERT_EQ(begin(ordinary, 0), dcomp::S_OK);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 8); },
                    {{ordinary}, {2}, {2}}),
              dcomp::E_INVALIDARG);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 9); },
                    {{h->read_ptr(kUpdateOut)}, {kMapped}, {2u}}),
              dcomp::S_OK);
    const GuestAddr ordinary_bits = h->read_ptr(kMapped + h->ptr_size());
    const u8 row[16] = {0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22,
                        0x33, 0x33, 0x33, 0x33, 0x44, 0x44, 0x44, 0x44};
    ASSERT_TRUE(h->memory.write_bytes(ordinary_bits, row, sizeof(row)).is_ok());
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 4); },
                    {{ordinary}}),
              dcomp::S_OK);
    const usize ordinary_upload = h->backend.texture_uploads.size();
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 7); },
                    {{ordinary}, {0}, {0}, {1}, {0}}),
              dcomp::S_OK);
    ASSERT_EQ(h->backend.texture_uploads.size(), ordinary_upload + 1);
    const auto& scrolled = h->backend.texture_uploads.back();
    ASSERT_EQ(scrolled.bytes.size(), 16u);
    EXPECT_EQ(scrolled.bytes[0], 0x00);
    EXPECT_EQ(scrolled.bytes[4], 0x11);
    EXPECT_EQ(scrolled.bytes[8], 0x22);
    EXPECT_EQ(scrolled.bytes[12], 0x33);

    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 8);
              },
                    {{composition}, {4}, {1}, {DXGI_FORMAT_B8G8R8A8_UNORM},
                     {1}, {kSurfaceOut}}),
              dcomp::S_OK);
    const GuestAddr clipped = h->read_ptr(kSurfaceOut);
    ASSERT_EQ(begin(clipped, 0), dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 9); },
                    {{h->read_ptr(kUpdateOut)}, {kMapped}, {2u}}),
              dcomp::S_OK);
    ASSERT_TRUE(h->memory
                    .write_bytes(h->read_ptr(kMapped + h->ptr_size()), row,
                                 sizeof(row))
                    .is_ok());
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 4); },
                    {{clipped}}),
              dcomp::S_OK);
    write_rect(kRect, 0, 0, 3, 1);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 7); },
                    {{clipped}, {0}, {kRect}, {1}, {0}}),
              dcomp::S_OK);
    ASSERT_GE(h->backend.texture_uploads.back().bytes.size(), 16u);
    EXPECT_EQ(h->backend.texture_uploads.back().bytes[0], 0x00);
    EXPECT_EQ(h->backend.texture_uploads.back().bytes[4], 0x11);
    EXPECT_EQ(h->backend.texture_uploads.back().bytes[8], 0x22);
    EXPECT_EQ(h->backend.texture_uploads.back().bytes[12], 0x44);
    write_rect(kRect, -1, 0, 2, 1);
    const usize before_reject = h->backend.texture_uploads.size();
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 7); },
                    {{clipped}, {kRect}, {0}, {0}, {0}}),
              dcomp::E_INVALIDARG);
    EXPECT_EQ(h->backend.texture_uploads.size(), before_reject);

    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 8);
              },
                    {{composition}, {4}, {1}, {DXGI_FORMAT_B8G8R8A8_UNORM},
                     {3}, {kSurfaceOut}}),
              dcomp::S_OK);
    const GuestAddr leftward = h->read_ptr(kSurfaceOut);
    ASSERT_EQ(begin(leftward, 0), dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 9); },
                    {{h->read_ptr(kUpdateOut)}, {kMapped}, {2u}}),
              dcomp::S_OK);
    ASSERT_TRUE(h->memory
                    .write_bytes(h->read_ptr(kMapped + h->ptr_size()), row,
                                 sizeof(row))
                    .is_ok());
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 4); },
                    {{leftward}}),
              dcomp::S_OK);
    const i64 negative = static_cast<i64>(static_cast<i32>(-1));
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 7); },
                    {{leftward},
                     {0},
                     {0},
                     {static_cast<u64>(negative)},
                     {0}}),
              dcomp::S_OK);
    ASSERT_GE(h->backend.texture_uploads.back().bytes.size(), 16u);
    EXPECT_EQ(h->backend.texture_uploads.back().bytes[0], 0x22);
    EXPECT_EQ(h->backend.texture_uploads.back().bytes[4], 0x33);
    EXPECT_EQ(h->backend.texture_uploads.back().bytes[8], 0x44);
    EXPECT_EQ(h->backend.texture_uploads.back().bytes[12], 0x00);

    ASSERT_EQ(create_virtual(1000, 1000, DXGI_FORMAT_B8G8R8A8_UNORM, 1),
              dcomp::S_OK);
    const GuestAddr sparse = h->read_ptr(kSurfaceOut);
    h->write_ptr(kQiOut, 0);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 0); },
                    {{sparse}, {kSurfaceIid}, {kQiOut}}),
              dcomp::S_OK);
    EXPECT_EQ(h->read_ptr(kQiOut), sparse);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 2); },
                    {{sparse}}),
              1u);
    // Outside the logical surface. A sub-rectangle inside it is valid on
    // the first BeginDraw of a virtual surface.
    write_rect(kRect, 0, 0, 1001, 1);
    EXPECT_EQ(begin(sparse, kRect), dcomp::E_INVALIDARG);
    write_rect(kRect, 10, 20, 14, 21);
    ASSERT_EQ(begin(sparse, kRect), dcomp::S_OK);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 8); },
                    {{sparse}, {8}, {8}}),
              dcomp::DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 7); },
                    {{sparse}, {0}, {0}, {1}, {0}}),
              dcomp::DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED);
    const GuestAddr sparse_update = h->read_ptr(kUpdateOut);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 8); },
                    {{sparse_update}, {kSurfDesc}}),
              dcomp::S_OK);
    EXPECT_EQ(h->r32(kSurfDesc + 0), 4u);
    EXPECT_EQ(h->r32(kSurfDesc + 4), 1u);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 9); },
                    {{sparse_update}, {kMapped}, {2u}}),
              dcomp::S_OK);
    ASSERT_TRUE(h->memory
                    .write_bytes(h->read_ptr(kMapped + h->ptr_size()), row,
                                 sizeof(row))
                    .is_ok());
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 4); },
                    {{sparse}}),
              dcomp::S_OK);
    ASSERT_EQ(h->backend.texture_uploads.back().pitch, 16u);
    ASSERT_EQ(h->backend.texture_uploads.back().bytes.size(), 16u);
    EXPECT_EQ(h->backend.texture_uploads.back().bytes[0], 0x11);

    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 6);
              },
                    {{composition}, {kVisualOut}}),
              dcomp::S_OK);
    const GuestAddr visual = h->read_ptr(kVisualOut);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_VisualMethod(m, 4); },
                    {{visual}, {farg(5.0f)}}, 5.0f),
              dcomp::S_OK);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_VisualMethod(m, 6); },
                    {{visual}, {farg(9.0f)}}, 9.0f),
              dcomp::S_OK);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_VisualMethod(m, 15); },
                    {{visual}, {sparse}}),
              dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 24);
              },
                    {{composition}, {kHwnd}, {1}, {kTargetOut}}),
              dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_TargetMethod(m, 3); },
                    {{h->read_ptr(kTargetOut)}, {visual}}),
              dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 3);
              },
                    {{composition}}),
              dcomp::S_OK);
    EXPECT_EQ(h->backend.stream.back(), "present 4x1");
    EXPECT_TRUE(h->backend.last_swap_chain.premultiplied_alpha);
    ASSERT_FALSE(h->backend.composition_layers.empty());
    EXPECT_FLOAT_EQ(h->backend.composition_layers.back().transform[4], 15.0f);
    EXPECT_FLOAT_EQ(h->backend.composition_layers.back().transform[5], 29.0f);
    const unsigned presents_after_draw = h->backend.swap_chain_creations;

    write_rect(kRect, 10, 20, 14, 21);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 7); },
                    {{sparse}, {kRect}, {0}, {1}, {0}}),
              dcomp::S_OK);
    ASSERT_EQ(h->backend.texture_uploads.back().bytes.size(), 16u);
    EXPECT_EQ(h->backend.texture_uploads.back().bytes[0], 0x11);
    EXPECT_EQ(h->backend.texture_uploads.back().bytes[4], 0x22);
    EXPECT_EQ(h->backend.texture_uploads.back().bytes[8], 0x33);
    EXPECT_EQ(h->backend.texture_uploads.back().bytes[12], 0x44);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 3);
              },
                    {{composition}}),
              dcomp::S_OK);
    EXPECT_EQ(h->backend.stream.back(), "present 4x1");
    EXPECT_EQ(h->backend.swap_chain_creations, presents_after_draw);
    EXPECT_FLOAT_EQ(h->backend.composition_layers.back().transform[4], 16.0f);
    EXPECT_FLOAT_EQ(h->backend.composition_layers.back().transform[5], 29.0f);

    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 9); },
                    {{sparse}, {0}, {1}}),
              dcomp::E_INVALIDARG);
    write_rect(kRect, 12, 20, 14, 21);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 9); },
                    {{sparse}, {kRect}, {1}}),
              dcomp::S_OK);
    ASSERT_EQ(h->backend.texture_uploads.back().bytes.size(), 8u);
    EXPECT_EQ(h->backend.texture_uploads.back().bytes[0], 0x22);
    EXPECT_EQ(h->backend.texture_uploads.back().bytes[4], 0x33);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 3);
              },
                    {{composition}}),
              dcomp::S_OK);
    EXPECT_EQ(h->backend.stream.back(), "present 2x1");
    EXPECT_FLOAT_EQ(h->backend.composition_layers.back().transform[4], 17.0f);

    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 8); },
                    {{sparse}, {13}, {1000}}),
              dcomp::S_OK);
    ASSERT_EQ(h->backend.texture_uploads.back().bytes.size(), 4u);
    EXPECT_EQ(h->backend.texture_uploads.back().bytes[0], 0x22);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 8); },
                    {{sparse}, {16777217u}, {1}}),
              dcomp::E_INVALIDARG);
    ASSERT_EQ(h->backend.texture_uploads.back().bytes.size(), 4u);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 9); },
                    {{sparse}, {0}, {0}}),
              dcomp::S_OK);
    const usize after_trim_all = h->backend.texture_uploads.size();
    const unsigned creations_before_empty =
        h->backend.swap_chain_creations;
    const usize stream_before_empty = h->backend.stream.size();
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 3);
              },
                    {{composition}}),
              dcomp::S_OK);
    EXPECT_EQ(h->backend.texture_uploads.size(), after_trim_all);
    EXPECT_EQ(h->backend.swap_chain_creations, creations_before_empty);
    EXPECT_EQ(h->backend.stream.size(), stream_before_empty);

    ASSERT_EQ(create_virtual(32, 8, DXGI_FORMAT_B8G8R8A8_UNORM, 3),
              dcomp::S_OK);
    const GuestAddr holed = h->read_ptr(kSurfaceOut);
    write_rect(kRect, 0, 0, 4, 1);
    ASSERT_EQ(begin(holed, kRect), dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 9); },
                    {{h->read_ptr(kUpdateOut)}, {kMapped}, {2u}}),
              dcomp::S_OK);
    ASSERT_TRUE(h->memory
                    .write_bytes(h->read_ptr(kMapped + h->ptr_size()), row,
                                 sizeof(row))
                    .is_ok());
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 4); },
                    {{holed}}),
              dcomp::S_OK);
    write_rect(kRect, 0, 0, 1, 1);
    write_rect(kRect2, 3, 0, 4, 1);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 9); },
                    {{holed}, {kRect}, {2}}),
              dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 6);
              },
                    {{composition}, {kVisualOut}}),
              dcomp::S_OK);
    const GuestAddr hole_visual = h->read_ptr(kVisualOut);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_VisualMethod(m, 15); },
                    {{hole_visual}, {holed}}),
              dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 24);
              },
                    {{composition}, {kHwnd}, {0}, {kTargetOut}}),
              dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_TargetMethod(m, 3); },
                    {{h->read_ptr(kTargetOut)}, {hole_visual}}),
              dcomp::S_OK);
    // Two kept rectangles. Commit presents each one; the gap is not a texel.
    EXPECT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 3);
              },
                    {{composition}}),
              dcomp::S_OK);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 9); },
                    {{holed}, {kRect}, {1}}),
              dcomp::S_OK);
    ASSERT_EQ(h->backend.texture_uploads.back().bytes.size(), 4u);
    EXPECT_EQ(h->backend.texture_uploads.back().bytes[0], 0x11);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 3);
              },
                    {{composition}}),
              dcomp::S_OK);
    EXPECT_EQ(h->backend.stream.back(), "present 1x1");

    // Legacy IDCompositionDevice::CreateVirtualSurface is the same slot.
    ASSERT_TRUE(h->memory.write_bytes(kDcompIid, device_iid, 16).is_ok());
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_DCompositionCreateDevice(m); },
                    {{queue}, {kDcompIid}, {kDeviceOut}}),
              dcomp::S_OK);
    const GuestAddr legacy = h->read_ptr(kDeviceOut);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Legacy, 9);
              },
                    {{legacy}, {8}, {8}, {DXGI_FORMAT_R8G8B8A8_UNORM}, {3},
                     {kSurfaceOut}}),
              dcomp::S_OK);
    EXPECT_NE(h->read_ptr(kSurfaceOut), 0u);
    EXPECT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 2); },
                    {{h->read_ptr(kSurfaceOut)}}),
              0u);
    EXPECT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Legacy, 2);
              },
                    {{legacy}}),
              0u);
}

// Learn Trim: pixels outside the kept rectangles are no longer used for
// texturing. A hole is not a texel of the bounding box, so Commit presents
// each kept rectangle and the colour that used to sit in the hole is absent.
TEST_P(D3D12Test, CompositionVirtualSurfaceTrimLeavesTheHoleUndrawn) {
    attach_window(*h);
    const GuestAddr queue = h->queue(h->device());

    DxgiManager dxgi(h->memory, GetParam());
    D3D11Manager d3d11(h->memory, GetParam(), h->backend, dxgi);
    DcompManager dcomp(h->memory, GetParam(), d3d11);
    DcompManager::MethodThunks thunks{};
    GuestAddr thunk = 0x76000000;
    const auto fill = [&](auto& methods) {
        for (GuestAddr& method : methods) {
            method = thunk;
            thunk += 0x10;
        }
    };
    fill(thunks.device);
    fill(thunks.device2);
    fill(thunks.desktop_device);
    fill(thunks.device3);
    fill(thunks.target);
    fill(thunks.visual2);
    fill(thunks.translate_transform);
    fill(thunks.scale_transform);
    fill(thunks.rotate_transform);
    fill(thunks.skew_transform);
    fill(thunks.matrix_transform);
    fill(thunks.rectangle_clip);
    fill(thunks.surface);
    fill(thunks.update_object);
    dcomp.set_method_thunks(thunks);
    dcomp.set_window_ownership_resolver([](u64 hwnd) {
        return hwnd == kHwnd ? DcompManager::WindowOwnership::CurrentProcess
                             : DcompManager::WindowOwnership::Invalid;
    });

    constexpr GuestAddr kDcompIid = kData + 0x3100;
    constexpr GuestAddr kDeviceOut = kData + 0x3120;
    constexpr GuestAddr kVisualOut = kData + 0x3140;
    constexpr GuestAddr kTargetOut = kData + 0x3160;
    constexpr GuestAddr kSurfaceOut = kData + 0x3180;
    constexpr GuestAddr kUpdateOut = kData + 0x31A0;
    constexpr GuestAddr kOffset = kData + 0x31C0;
    constexpr GuestAddr kRect = kData + 0x31E0;
    constexpr GuestAddr kRect2 = kRect + 16;
    constexpr GuestAddr kMapped = kData + 0x3220;
    const u8 desktop_iid[16] = {0xFE, 0x33, 0x46, 0x5F, 0x08, 0x1E, 0xB8, 0x4C,
                                0x8C, 0x75, 0xCE, 0x24, 0x33, 0x3F, 0x56, 0x02};
    const u8 dxgi_surface_iid[16] = {
        0x6C, 0xB5, 0xFC, 0xCA, 0xC3, 0x6A, 0x89, 0x48,
        0xBF, 0x47, 0x9E, 0x23, 0xBB, 0xD2, 0x60, 0xEC};
    ASSERT_TRUE(h->memory.write_bytes(kDcompIid, desktop_iid, 16).is_ok());
    constexpr GuestAddr kDxgiIid = kData + 0x3280;
    ASSERT_TRUE(h->memory.write_bytes(kDxgiIid, dxgi_surface_iid, 16).is_ok());

    auto dcall = [&](auto&& fn, std::vector<Harness::Arg> args,
                     float xmm1 = 0.0f) {
        h->cpu.sse.xmm[1].f32_[0] = xmm1;
        return h->invoke([&](Marshaler& m) { fn(m); }, args, false, 0, "");
    };
    const auto farg = [](float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return static_cast<u64>(bits);
    };
    auto write_rect = [&](GuestAddr at, i32 l, i32 t, i32 r, i32 b) {
        h->w32(at + 0, static_cast<u32>(l));
        h->w32(at + 4, static_cast<u32>(t));
        h->w32(at + 8, static_cast<u32>(r));
        h->w32(at + 12, static_cast<u32>(b));
    };

    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_DCompositionCreateDevice3(m); },
                    {{queue}, {kDcompIid}, {kDeviceOut}}),
              dcomp::S_OK);
    const GuestAddr composition = h->read_ptr(kDeviceOut);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 9);
              },
                    {{composition}, {32}, {8}, {DXGI_FORMAT_B8G8R8A8_UNORM},
                     {1}, {kSurfaceOut}}),
              dcomp::S_OK);
    const GuestAddr surface = h->read_ptr(kSurfaceOut);

    // 4x2 update at (2, 1). Columns x=3 and x=4 are the hole colour 0xA5.
    write_rect(kRect, 2, 1, 6, 3);
    h->write_ptr(kUpdateOut, 0);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 3); },
                    {{surface}, {kRect}, {kDxgiIid}, {kUpdateOut}, {kOffset}}),
              dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_UpdateObjectMethod(m, 9); },
                    {{h->read_ptr(kUpdateOut)}, {kMapped}, {2u}}),
              dcomp::S_OK);
    const GuestAddr bits = h->read_ptr(kMapped + h->ptr_size());
    u8 drawn[32];
    for (u8& byte : drawn) byte = 0xA5;
    const u8 kept[4][4] = {
        {0x11, 0x11, 0x11, 0x11},
        {0x22, 0x22, 0x22, 0x22},
        {0x33, 0x33, 0x33, 0x33},
        {0x44, 0x44, 0x44, 0x44},
    };
    std::memcpy(drawn + 0, kept[0], 4);
    std::memcpy(drawn + 12, kept[1], 4);
    std::memcpy(drawn + 16, kept[2], 4);
    std::memcpy(drawn + 28, kept[3], 4);
    ASSERT_TRUE(h->memory.write_bytes(bits, drawn, sizeof(drawn)).is_ok());
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 4); },
                    {{surface}}),
              dcomp::S_OK);
    ASSERT_FALSE(h->backend.texture_uploads.empty());
    const auto& full = h->backend.texture_uploads.back();
    ASSERT_EQ(full.pitch, 16u);
    ASSERT_EQ(full.bytes.size(), 32u);
    EXPECT_EQ(full.bytes[4], 0xA5);

    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 6);
              },
                    {{composition}, {kVisualOut}}),
              dcomp::S_OK);
    const GuestAddr visual = h->read_ptr(kVisualOut);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_VisualMethod(m, 4); },
                    {{visual}, {farg(7.0f)}}, 7.0f),
              dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_VisualMethod(m, 6); },
                    {{visual}, {farg(4.0f)}}, 4.0f),
              dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_VisualMethod(m, 15); },
                    {{visual}, {surface}}),
              dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 24);
              },
                    {{composition}, {kHwnd}, {1}, {kTargetOut}}),
              dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_TargetMethod(m, 3); },
                    {{h->read_ptr(kTargetOut)}, {visual}}),
              dcomp::S_OK);
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 3);
              },
                    {{composition}}),
              dcomp::S_OK);
    EXPECT_EQ(h->backend.stream.back(), "present 4x2");
    ASSERT_FALSE(h->backend.composition_layers.empty());
    EXPECT_FLOAT_EQ(h->backend.composition_layers.back().transform[4], 9.0f);
    EXPECT_FLOAT_EQ(h->backend.composition_layers.back().transform[5], 5.0f);

    const usize uploads_before_trim = h->backend.texture_uploads.size();
    const usize stream_before_trim = h->backend.stream.size();
    write_rect(kRect, 2, 1, 3, 3);
    write_rect(kRect2, 5, 1, 6, 3);
    ASSERT_EQ(dcall([&](Marshaler& m) { dcomp.impl_SurfaceMethod(m, 9); },
                    {{surface}, {kRect}, {2}}),
              dcomp::S_OK);
    EXPECT_EQ(h->backend.stream.size(), stream_before_trim);
    ASSERT_EQ(h->backend.texture_uploads.size(), uploads_before_trim + 2);
    for (usize index = uploads_before_trim;
         index < h->backend.texture_uploads.size(); ++index) {
        const auto& upload = h->backend.texture_uploads[index];
        EXPECT_EQ(upload.pitch, 4u);
        ASSERT_EQ(upload.bytes.size(), 8u);
        for (u8 byte : upload.bytes) EXPECT_NE(byte, 0xA5);
    }
    const auto& left = h->backend.texture_uploads[uploads_before_trim];
    const auto& right = h->backend.texture_uploads[uploads_before_trim + 1];
    EXPECT_EQ(left.bytes[0], 0x11);
    EXPECT_EQ(left.bytes[4], 0x33);
    EXPECT_EQ(right.bytes[0], 0x22);
    EXPECT_EQ(right.bytes[4], 0x44);

    const unsigned chains_before_commit = h->backend.swap_chain_creations;
    const usize layers_before_commit = h->backend.composition_layers.size();
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 3);
              },
                    {{composition}}),
              dcomp::S_OK);
    ASSERT_EQ(h->backend.stream.size(), stream_before_trim + 2);
    EXPECT_EQ(h->backend.stream[stream_before_trim], "present 1x2");
    EXPECT_EQ(h->backend.stream[stream_before_trim + 1], "present 1x2");
    EXPECT_EQ(h->backend.swap_chain_creations, chains_before_commit + 2);
    ASSERT_EQ(h->backend.composition_layers.size(), layers_before_commit + 2);
    const auto& first = h->backend.composition_layers[layers_before_commit];
    const auto& second =
        h->backend.composition_layers[layers_before_commit + 1];
    EXPECT_FLOAT_EQ(first.transform[4], 9.0f);
    EXPECT_FLOAT_EQ(first.transform[5], 5.0f);
    EXPECT_FLOAT_EQ(second.transform[4], 12.0f);
    EXPECT_FLOAT_EQ(second.transform[5], 5.0f);

    const usize uploads_after_commit = h->backend.texture_uploads.size();
    const unsigned chains_after_commit = h->backend.swap_chain_creations;
    ASSERT_EQ(dcall([&](Marshaler& m) {
                  dcomp.impl_DeviceMethod(
                      m, DcompManager::DeviceInterface::Desktop, 3);
              },
                    {{composition}}),
              dcomp::S_OK);
    EXPECT_EQ(h->backend.texture_uploads.size(), uploads_after_commit);
    EXPECT_EQ(h->backend.swap_chain_creations, chains_after_commit);
    ASSERT_GE(h->backend.stream.size(), 2u);
    EXPECT_EQ(h->backend.stream[h->backend.stream.size() - 2], "present 1x2");
    EXPECT_EQ(h->backend.stream.back(), "present 1x2");
}

}  // namespace
}  // namespace vitreo::winapi
