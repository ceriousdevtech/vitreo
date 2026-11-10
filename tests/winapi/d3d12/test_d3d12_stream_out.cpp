// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_d3d12_stream_out.cpp
/// @brief D3D12 stream output at feature level 11_0.
///
/// Expected component order is the rule measured by
/// tools/oracle_probe/d3d11_stream_out_probe.c and stated in
/// graphics/shaders/stream_out_repack.hpp: a triangle strip's triangle n is
/// (n, n+1, n+2) when n is even and (n, n+2, n+1) when n is odd, and a
/// primitive that does not fit a target is dropped whole. The vertex shader
/// below is vs_4_0 `mov o1.xyzw, v0.xxxx` (SV_VertexID broadcast), so each
/// streamed TEXCOORD0 component is that vertex id's bits.

#include "winapi/graphics/d3d12/d3d12.hpp"
#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/d3d12/d3d12_root_signature.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/gs_passthrough.hpp"
#include "graphics/shaders/ia_resolve.hpp"
#include "graphics/shaders/indirect_count.hpp"
#include "graphics/shaders/stream_out_repack.hpp"
#include "graphics/shaders/tessellator.hpp"

#include "../../graphics/tessellation_shaders.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {
namespace {

using namespace d3d12;

constexpr GuestAddr kData = 0x40000;
constexpr usize kDataSize = 0x80000;
constexpr GuestAddr kStack = kData + kDataSize - 0x1000;
constexpr GuestAddr kIid = kData + 0x100;
constexpr GuestAddr kOut = kData + 0x200;
constexpr GuestAddr kOut2 = kData + 0x240;
constexpr GuestAddr kDesc = kData + 0x400;
constexpr GuestAddr kDesc2 = kData + 0x800;
constexpr GuestAddr kArray = kData + 0xC00;

/// Bits of an immediate float, shared by the domain-shader bytecode and the
/// bytes the repack must leave in the stream-output buffer.
[[nodiscard]] u32 float_bits(float value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/// `mov o0.xyzw, l(1, 2, 3, 4)` — the domain shader this file streams.
[[nodiscard]] std::array<u32, 4> domain_output_words() {
    return {float_bits(1.f), float_bits(2.f), float_bits(3.f), float_bits(4.f)};
}

/// Host memory the stream-output kernels' C++ models write, so a Map of an
/// upload buffer sees the same bytes the GPU path would leave.
class HostBuffer final : public graphics::BackendBuffer {
public:
    HostBuffer(usize size, const void* initial) : m_size(std::max<usize>(size, 1)) {
        const usize alloc = (m_size + 0xFFFFu) & ~usize{0xFFFFu};
        m_storage.reset(static_cast<u8*>(std::aligned_alloc(0x10000, alloc)));
        std::memset(m_storage.get(), 0, alloc);
        m_desc.byte_width = size == 0 ? 1 : size;
        if (initial != nullptr && size != 0) {
            std::memcpy(m_storage.get(), initial, size);
        }
    }
    [[nodiscard]] bool valid() const noexcept override { return m_storage != nullptr; }
    [[nodiscard]] const graphics::BackendBufferDesc& desc() const noexcept override {
        return m_desc;
    }
    [[nodiscard]] bool update(usize offset, const void* data, usize length,
                              graphics::BufferWrite) override {
        if (data == nullptr || offset + length > m_size) return false;
        std::memcpy(m_storage.get() + offset, data, length);
        return true;
    }
    [[nodiscard]] u8* bytes() { return m_storage.get(); }
    [[nodiscard]] u32* words(unsigned offset) {
        return reinterpret_cast<u32*>(m_storage.get() + offset);
    }

private:
    graphics::BackendBufferDesc m_desc{};
    std::unique_ptr<u8, decltype(&std::free)> m_storage{nullptr, &std::free};
    usize m_size{0};
};

class SoBackend final : public graphics::GraphicsBackendDevice {
public:
    class Heap final : public graphics::BackendHeap {
    public:
        explicit Heap(u64 size, bool cpu)
            : m_buffer(static_cast<usize>(size), nullptr), m_size(size), m_cpu(cpu) {}
        [[nodiscard]] bool valid() const noexcept override { return true; }
        [[nodiscard]] u64 size() const noexcept override { return m_size; }
        [[nodiscard]] void* host_pointer() const noexcept override {
            return m_cpu ? const_cast<HostBuffer&>(m_buffer).bytes() : nullptr;
        }
        [[nodiscard]] graphics::BackendBuffer* buffer() noexcept override {
            return &m_buffer;
        }

    private:
        HostBuffer m_buffer;
        u64 m_size{0};
        bool m_cpu{false};
    };

    class Texture final : public graphics::BackendTexture2D {
    public:
        explicit Texture(const graphics::BackendTexture2DDesc& desc) : m_desc(desc) {}
        [[nodiscard]] bool valid() const noexcept override { return true; }
        [[nodiscard]] const graphics::BackendTexture2DDesc& desc()
            const noexcept override {
            return m_desc;
        }
        [[nodiscard]] bool clear_render_target(const std::array<float, 4>&) override {
            return true;
        }
        [[nodiscard]] bool clear_depth_stencil(float, u8) override { return true; }
        [[nodiscard]] bool update(unsigned, const void*, unsigned) override { return true; }

    private:
        graphics::BackendTexture2DDesc m_desc;
    };

    class Swap final : public graphics::BackendSwapChain {
    public:
        explicit Swap(const graphics::BackendSwapChainDesc& desc) : m_desc(desc) {}
        [[nodiscard]] bool valid() const noexcept override { return true; }
        [[nodiscard]] const graphics::BackendSwapChainDesc& desc()
            const noexcept override {
            return m_desc;
        }
        [[nodiscard]] bool clear_back_buffer(const std::array<float, 4>&) override {
            return true;
        }
        [[nodiscard]] bool present(unsigned, unsigned) override { return true; }
        [[nodiscard]] bool resize(unsigned, unsigned, graphics::PixelFormat,
                                  unsigned) override {
            return true;
        }
        [[nodiscard]] std::unique_ptr<graphics::BackendRenderPass> begin_render_pass(
            bool, bool, float,
            const std::array<graphics::BackendTexture2D*,
                             graphics::BACKEND_MAX_RENDER_TARGETS>&) override {
            return nullptr;
        }

    private:
        graphics::BackendSwapChainDesc m_desc;
    };

    class Pipeline final : public graphics::BackendPipeline {
    public:
        [[nodiscard]] bool valid() const noexcept override { return true; }
    };
    class Compute final : public graphics::BackendComputePipeline {
    public:
        explicit Compute(std::string entry) : entry(std::move(entry)) {}
        [[nodiscard]] bool valid() const noexcept override { return true; }
        std::string entry;
    };
    class Pass final : public graphics::BackendRenderPass {
    public:
        explicit Pass(SoBackend& owner) : m_owner(owner) {}
        [[nodiscard]] bool valid() const noexcept override { return true; }
        [[nodiscard]] bool draw(const graphics::BackendDrawCall& call) override {
            ++m_owner.draws;
            m_owner.last_capture = call.vertex_capture_buffer != nullptr;
            if (call.vertex_capture_buffer != nullptr &&
                call.indirect.buffer == nullptr) {
                m_owner.fill_capture(call);
            } else if (call.vertex_capture_buffer != nullptr) {
                m_owner.fill_relative_capture(call);
            }
            return true;
        }
        [[nodiscard]] bool end() override { return true; }

    private:
        SoBackend& m_owner;
    };

    class Fence final : public graphics::BackendFence {
    public:
        explicit Fence(u64 initial) : m_value(initial) {}
        [[nodiscard]] bool valid() const noexcept override { return true; }
        [[nodiscard]] u64 completed_value() const override { return m_value; }
        [[nodiscard]] bool signal(u64 value) override {
            m_value = value;
            return true;
        }
        [[nodiscard]] bool wait_gpu(u64) override { return true; }
        [[nodiscard]] bool signal_cpu(u64 value) override {
            m_value = value;
            return true;
        }
        [[nodiscard]] bool wait_cpu(u64 value, u64) override {
            return m_value >= value;
        }

    private:
        u64 m_value{0};
    };

    unsigned draws{0};
    bool last_capture{false};

    [[nodiscard]] bool available() const noexcept override { return true; }
    [[nodiscard]] bool supports_fences() const noexcept override { return true; }
    [[nodiscard]] std::unique_ptr<graphics::BackendFence> create_fence(
        u64 initial) override {
        return std::make_unique<Fence>(initial);
    }
    [[nodiscard]] std::string backend_device_name() const override { return "SO"; }
    [[nodiscard]] void* native_device() const noexcept override { return nullptr; }
    [[nodiscard]] void* native_command_queue() const noexcept override {
        return nullptr;
    }
    [[nodiscard]] bool supports_placement_heaps() const noexcept override {
        return true;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendHeap> create_heap(
        const graphics::BackendHeapDesc& desc) override {
        return std::make_unique<Heap>(desc.size, desc.cpu_visible);
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendBuffer> create_buffer(
        const graphics::BackendBufferDesc& desc, const void* initial) override {
        return std::make_unique<HostBuffer>(desc.byte_width, initial);
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendTexture2D> create_texture_2d(
        const graphics::BackendTexture2DDesc& desc, const void*, unsigned) override {
        return std::make_unique<Texture>(desc);
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendSwapChain> create_swap_chain(
        const graphics::BackendSwapChainDesc& desc) override {
        return std::make_unique<Swap>(desc);
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendPipeline> create_pipeline(
        const graphics::BackendPipelineDesc&) override {
        return std::make_unique<Pipeline>();
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendComputePipeline>
    create_compute_pipeline(const graphics::BackendComputePipelineDesc& desc) override {
        return std::make_unique<Compute>(desc.compute_entry_point);
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendRenderPass>
    create_render_pass_standalone(const graphics::BackendRenderPassDesc&) override {
        return std::make_unique<Pass>(*this);
    }
    [[nodiscard]] bool copy_buffer(graphics::BackendBuffer* dst,
                                   const graphics::BackendBuffer* src) override {
        if (dst == nullptr || src == nullptr) return false;
        if (dst->desc().byte_width != src->desc().byte_width) return false;
        return copy_buffer_region(dst, 0, src, 0, dst->desc().byte_width);
    }
    [[nodiscard]] bool copy_buffer_region(graphics::BackendBuffer* dst, u64 dst_offset,
                                          const graphics::BackendBuffer* src,
                                          u64 src_offset, u64 bytes) override {
        if (dst == nullptr || src == nullptr || bytes == 0) return false;
        auto* d = static_cast<HostBuffer*>(dst);
        auto* s = static_cast<HostBuffer*>(const_cast<graphics::BackendBuffer*>(src));
        if (dst_offset + bytes > d->desc().byte_width ||
            src_offset + bytes > s->desc().byte_width) {
            return false;
        }
        std::memcpy(d->bytes() + dst_offset, s->bytes() + src_offset,
                    static_cast<usize>(bytes));
        return true;
    }
    [[nodiscard]] bool copy_texture_2d(graphics::BackendTexture2D*,
                                       const graphics::BackendTexture2D*) override {
        return true;
    }
    [[nodiscard]] bool generate_mips(graphics::BackendTexture2D*) override {
        return false;
    }
    void flush() override {}
    [[nodiscard]] bool wait_idle() override { return true; }

    [[nodiscard]] bool dispatch(const graphics::BackendDispatchCall& call) override {
        if (call.pipeline == nullptr) return false;
        const auto& entry =
            static_cast<const Compute*>(call.pipeline)->entry;
        if (entry == graphics::kStreamOutPrepareEntryPoint) {
            run_prepare(call);
        } else if (entry == graphics::kStreamOutRepackEntryPoint) {
            run_repack(call);
        } else if (entry == graphics::kStreamOutCompactEntryPoint) {
            run_compact(call);
        } else if (entry == graphics::tess::kTessPatchEntryPoint) {
            run_tess_patch(call);
        } else if (entry == graphics::tess::kTessScanEntryPoint) {
            run_tess_scan(call);
        } else if (entry == graphics::kIndirectCountEntryPoint) {
            run_indirect_count(call);
        } else if (entry == graphics::kIaResolveEntryPoint) {
            run_ia_resolve(call);
        } else if (entry == "main0") {
            // Hull writes the factor buffer (binding 5). Domain reads patch
            // constants at binding 4 and uses the fixed domain thread count.
            // A geometry shader binds neither.
            constexpr unsigned base = graphics::BACKEND_GS_EXPANSION_BUFFER_BASE;
            if (call.buffers[base + 5] != nullptr) {
                run_constant_hull(call);
            } else if (call.threads_per_group_x == graphics::kDomainComputeThreads &&
                       call.buffers[base + 4] != nullptr) {
                run_constant_domain(call);
            } else {
                run_point_geometry(call);
            }
        }
        return true;
    }

private:
    [[nodiscard]] static HostBuffer* host(const graphics::BackendBuffer* buffer) {
        return static_cast<HostBuffer*>(const_cast<graphics::BackendBuffer*>(buffer));
    }

    /// vs_4_0 used by these tests: o0 = (0,0,0,1), o1 = VertexID.xxxx,
    /// o2.xy = VertexID. The capture record is those registers in ascending
    /// order (dxbc_to_spirv_vertex_capture.cpp).
    void fill_capture(const graphics::BackendDrawCall& call) {
        auto* buffer = host(call.vertex_capture_buffer);
        if (buffer == nullptr || call.vertex_count == 0) return;
        constexpr u32 kRegs = 3;
        u32* base = buffer->words(call.vertex_capture_buffer_offset);
        for (u32 i = 0; i < call.vertex_count; ++i) {
            const u32 vid = call.first_vertex + i;
            u32* rec = base + static_cast<usize>(vid) * kRegs * 4u;
            rec[0] = rec[1] = rec[2] = 0;
            rec[3] = 0x3f800000u;
            rec[4] = rec[5] = rec[6] = rec[7] = vid;
            rec[8] = rec[9] = vid;
            rec[10] = rec[11] = 0;
        }
    }

    /// The relative capture of a GPU-resolved draw (capture_draw_relative):
    /// the argument block {vertices, instances, first vertex, first
    /// instance} from the indirect buffer, records keyed vertex - first +
    /// instance x stride behind the 16-byte {stride, capacity} header.
    void fill_relative_capture(const graphics::BackendDrawCall& call) {
        auto* buffer = host(call.vertex_capture_buffer);
        auto* args = host(call.indirect.buffer);
        if (buffer == nullptr || args == nullptr || call.index_buffer != nullptr) {
            return;
        }
        const u32* a = args->words(call.indirect.offset);
        u32* base = buffer->words(call.vertex_capture_buffer_offset);
        const u32 stride = base[0];
        const u32 capacity = base[1];
        constexpr u32 kRegs = 3;
        for (u32 j = 0; j < a[1]; ++j) {
            for (u32 i = 0; i < a[0]; ++i) {
                const u32 vid = a[2] + i;
                const u32 key = i + j * stride;
                if (i >= stride || key >= capacity) continue;
                u32* rec = base + 4u + static_cast<usize>(key) * kRegs * 4u;
                rec[0] = rec[1] = rec[2] = 0;
                rec[3] = 0x3f800000u;
                rec[4] = rec[5] = rec[6] = rec[7] = vid;
                rec[8] = rec[9] = vid;
                rec[10] = rec[11] = 0;
            }
        }
    }

    void run_indirect_count(const graphics::BackendDispatchCall& call) {
        graphics::IndirectCountParams params{};
        if (call.constants == nullptr || call.constants_size < sizeof(params)) return;
        std::memcpy(&params, call.constants, sizeof(params));
        auto* source = host(call.buffers[graphics::INDIRECT_COUNT_SOURCE_INDEX]);
        auto* count = host(call.buffers[graphics::INDIRECT_COUNT_COUNT_INDEX]);
        auto* dest = host(call.buffers[graphics::INDIRECT_COUNT_DEST_INDEX]);
        if (source == nullptr || dest == nullptr) return;
        const u32* src = source->words(static_cast<unsigned>(
            call.buffer_offsets[graphics::INDIRECT_COUNT_SOURCE_INDEX]));
        std::vector<u32> words(src, src + static_cast<usize>(params.max_count) *
                                              params.stride_words);
        const u32 n = count != nullptr
                          ? count->words(static_cast<unsigned>(call.buffer_offsets
                                [graphics::INDIRECT_COUNT_COUNT_INDEX]))[0]
                          : 0u;
        const auto out = graphics::indirect_count_model(params, words, n);
        std::memcpy(dest->words(0), out.data(), out.size() * sizeof(u32));
    }

    /// vitreo_ia_resolve of a non-indexed draw, through its C++ model.
    void run_ia_resolve(const graphics::BackendDispatchCall& call) {
        graphics::IaResolveParams params{};
        if (call.constants == nullptr || call.constants_size < sizeof(params)) return;
        std::memcpy(&params, call.constants, sizeof(params));
        auto* args = host(call.buffers[graphics::IA_RESOLVE_ARGS_INDEX]);
        auto* ids = host(call.buffers[graphics::IA_RESOLVE_IDS_INDEX]);
        auto* out = host(call.buffers[graphics::IA_RESOLVE_OUT_INDEX]);
        auto* capture = host(call.buffers[graphics::IA_RESOLVE_CAPTURE_INDEX]);
        if (args == nullptr || ids == nullptr || out == nullptr ||
            capture == nullptr || params.indexed != 0) {
            return;
        }
        std::array<u32, 5> a{};
        std::memcpy(a.data(),
                    args->words(static_cast<unsigned>(
                        call.buffer_offsets[graphics::IA_RESOLVE_ARGS_INDEX])),
                    16);
        std::vector<u32> keys;
        const auto block = graphics::ia_resolve_model(params, a, {}, keys);
        std::memcpy(out->words(0), block.data(), sizeof(block));
        std::memcpy(ids->words(0), keys.data(), keys.size() * sizeof(u32));
        capture->words(0)[0] = block[graphics::IA_RESOLVED_KEY_STRIDE];
        capture->words(0)[1] = params.record_capacity;
    }

    /// A point geometry shader copies the captured vertex's o0 and o1 into
    /// one emitted vertex and publishes a count of 1. That is the gs_4_0
    /// this file builds (mov o0, v[0][0]; mov o1, v[0][1]; emit).
    void run_point_geometry(const graphics::BackendDispatchCall& call) {
        constexpr unsigned base = graphics::BACKEND_GS_EXPANSION_BUFFER_BASE;
        auto* in = host(call.buffers[base + 0]);
        auto* expanded = host(call.buffers[base + 1]);
        auto* counter = host(call.buffers[base + 2]);
        auto* ids = host(call.buffers[base + 3]);
        if (in == nullptr || expanded == nullptr || counter == nullptr ||
            ids == nullptr || call.group_count_x == 0) {
            return;
        }
        const u32 invocations = call.group_count_x;
        const u32 per_vertex =
            static_cast<u32>(expanded->desc().byte_width / (invocations * 16u));
        if (per_vertex == 0) return;
        const u32* control = counter->words(0);
        const u32 limit =
            (control[graphics::kGsControlFlags] & graphics::kGsFlagInvocationLimit) != 0
                ? control[graphics::kGsControlInvocationLimit]
                : invocations;
        for (u32 i = 0; i < invocations; ++i) {
            if (i >= limit) {
                counter->words(0)[graphics::kGsInvocationCountBase + i] = 0u;
                continue;
            }
            const u32 vid = ids->words(call.buffer_offsets[base + 3])[i];
            const u32* src = in->words(call.buffer_offsets[base + 0]) +
                             static_cast<usize>(vid) * 3u * 4u;
            u32* dst = expanded->words(0) +
                       static_cast<usize>(i) * per_vertex * 4u;
            std::memcpy(dst, src, std::min(per_vertex, 3u) * 16u);
            counter->words(0)[graphics::kGsInvocationCountBase + i] = 1u;
        }
    }

    void run_prepare(const graphics::BackendDispatchCall& call) {
        graphics::SoPrepareParams params{};
        if (call.constants == nullptr || call.constants_size < sizeof(params)) return;
        std::memcpy(&params, call.constants, sizeof(params));
        std::array<u32*, graphics::BACKEND_MAX_STREAM_OUT_TARGETS> filled{};
        for (u32 slot = 0; slot < filled.size(); ++slot) {
            if ((params.slot_mask & (1u << slot)) == 0) continue;
            auto* buffer = host(call.buffers[graphics::SO_PREPARE_FILLED_BASE + slot]);
            if (buffer == nullptr) continue;
            filled[slot] =
                buffer->words(call.buffer_offsets[graphics::SO_PREPARE_FILLED_BASE + slot]);
        }
        auto* op_buffer = host(call.buffers[graphics::SO_PREPARE_OP_INDEX]);
        auto* stats_buffer = host(call.buffers[graphics::SO_PREPARE_STATS_INDEX]);
        auto* count_buffer = host(call.buffers[graphics::SO_PREPARE_COUNT_INDEX]);
        if (op_buffer == nullptr || stats_buffer == nullptr || count_buffer == nullptr) {
            return;
        }
        std::array<u32, graphics::SO_OP_WORDS> op{};
        std::array<u32, graphics::SO_STATS_WORDS> stats{};
        std::memcpy(stats.data(), stats_buffer->words(0), sizeof(stats));
        graphics::stream_out_prepare_model(
            params,
            *count_buffer->words(call.buffer_offsets[graphics::SO_PREPARE_COUNT_INDEX]),
            filled, op, stats);
        std::memcpy(op_buffer->words(0), op.data(), sizeof(op));
        std::memcpy(stats_buffer->words(0), stats.data(), sizeof(stats));
    }

    void run_compact(const graphics::BackendDispatchCall& call) {
        graphics::SoCompactParams params{};
        if (call.constants == nullptr || call.constants_size < sizeof(params)) return;
        std::memcpy(&params, call.constants, sizeof(params));
        auto* counts_buffer = host(call.buffers[graphics::SO_COMPACT_COUNTS_INDEX]);
        auto* order_buffer = host(call.buffers[graphics::SO_COMPACT_ORDER_INDEX]);
        auto* total_buffer = host(call.buffers[graphics::SO_COMPACT_TOTAL_INDEX]);
        if (counts_buffer == nullptr || order_buffer == nullptr || total_buffer == nullptr) {
            return;
        }
        std::vector<u32> counts(params.invocations);
        std::memcpy(counts.data(),
                    counts_buffer->words(call.buffer_offsets[graphics::SO_COMPACT_COUNTS_INDEX]),
                    counts.size() * sizeof(u32));
        const auto order = graphics::stream_out_compact_model(params, params.invocations, counts);
        if (!order.empty()) {
            std::memcpy(order_buffer->words(0), order.data(), order.size() * sizeof(u32));
        }
        total_buffer->words(0)[0] = static_cast<u32>(order.size());
    }

    /// The repack kernel (stream_out_repack_msl), on the CPU, so the guest
    /// buffer receives the declared components.
    void run_repack(const graphics::BackendDispatchCall& call) {
        auto* control = host(call.buffers[graphics::SO_REPACK_CONTROL_INDEX]);
        auto* capture = host(call.buffers[graphics::SO_REPACK_CAPTURE_INDEX]);
        auto* op = host(call.buffers[graphics::SO_REPACK_OP_INDEX]);
        auto* indices = host(call.buffers[graphics::SO_REPACK_INDICES_INDEX]);
        if (control == nullptr || capture == nullptr || op == nullptr) return;
        const u32* ctl = control->words(0);
        const u32* opw = op->words(0);
        const u32 written = opw[graphics::SO_OP_VERTEX_COUNT];
        const u32 vpp = ctl[graphics::SO_CTL_VERTS_PER_PRIM];
        if (vpp == 0) return;
        const u32* cap = capture->words(call.buffer_offsets[graphics::SO_REPACK_CAPTURE_INDEX]);
        const u32* idx = indices != nullptr
                             ? indices->words(call.buffer_offsets[graphics::SO_REPACK_INDICES_INDEX])
                             : nullptr;
        for (u32 tid = 0; tid < written; ++tid) {
            const u32 primitive = tid / vpp;
            const u32 k = tid % vpp;
            u32 v = tid;
            if (ctl[graphics::SO_CTL_STRIP] != 0u) {
                const u32 kk = (vpp == 3u && (primitive & 1u) != 0u && k != 0u)
                                   ? 3u - k
                                   : k;
                v = primitive + kk;
            }
            u32 vid = 0;
            const u32 mode = ctl[graphics::SO_CTL_INDEX_MODE];
            if (mode == 0u) {
                vid = ctl[graphics::SO_CTL_FIRST] + v;
            } else if (idx != nullptr) {
                const u32 e = ctl[graphics::SO_CTL_FIRST] + v;
                const u32 index = mode == 1u
                                      ? (idx[e >> 1] >> ((e & 1u) * 16u)) & 0xFFFFu
                                      : idx[e];
                vid = static_cast<u32>(static_cast<i32>(index) +
                                       static_cast<i32>(ctl[graphics::SO_CTL_BASE_VERTEX]));
            }
            const u32 regs = ctl[graphics::SO_CTL_RECORD_VEC4S];
            const u32 entries = ctl[graphics::SO_CTL_ENTRY_COUNT];
            for (u32 n = 0; n < entries; ++n) {
                const u32 at = graphics::SO_CTL_ENTRIES + n * graphics::SO_CTL_ENTRY_WORDS;
                const u32 src = ctl[at + 2];
                if (src == 0xFFFFFFFFu) continue;
                const u32 slot = ctl[at];
                const u32 start = ctl[at + 3] & 0xFFu;
                const u32 count = (ctl[at + 3] >> 8) & 0xFFu;
                auto* target = host(call.buffers[graphics::SO_REPACK_TARGET_BASE + slot]);
                if (target == nullptr || slot >= 4) continue;
                const u32 dst =
                    (opw[graphics::SO_OP_DST_OFFSETS + slot] +
                     tid * ctl[graphics::SO_CTL_STRIDES + slot]) /
                        4u +
                    ctl[at + 1];
                u32* out = target->words(
                    call.buffer_offsets[graphics::SO_REPACK_TARGET_BASE + slot]);
                const u32* record = cap + (static_cast<usize>(vid) * regs + src) * 4u;
                for (u32 c = 0; c < count; ++c) out[dst + c] = record[start + c];
            }
        }
    }

    /// The hull shader below writes edge and inside factors of 1. The C++
    /// tessellator (the same reference the GPU kernel runs) turns integer
    /// factor 1 into one triangle.
    void run_constant_hull(const graphics::BackendDispatchCall& call) {
        constexpr unsigned base = graphics::BACKEND_GS_EXPANSION_BUFFER_BASE;
        auto* factors = host(call.buffers[base + 5]);
        if (factors == nullptr || call.group_count_x == 0) return;
        const u32 one = float_bits(1.f);
        u32* dst = factors->words(call.buffer_offsets[base + 5]);
        const u32 limit = static_cast<u32>(factors->desc().byte_width / 4u);
        for (u32 patch = 0; patch < call.group_count_x; ++patch) {
            const u32 at = patch * graphics::kTessFactorWords;
            if (at + 4u > limit) return;
            for (u32 edge = 0; edge < 4u; ++edge) dst[at + edge] = one;
        }
    }

    /// One vec4 per domain point: the shader's `l(1, 2, 3, 4)`.
    void run_constant_domain(const graphics::BackendDispatchCall& call) {
        constexpr unsigned base = graphics::BACKEND_GS_EXPANSION_BUFFER_BASE;
        auto* records = host(call.buffers[base + 3]);
        if (records == nullptr) return;
        const auto words = domain_output_words();
        u32* dst = records->words(call.buffer_offsets[base + 3]);
        const u32 count = static_cast<u32>(records->desc().byte_width / 16u);
        for (u32 i = 0; i < count; ++i) {
            std::memcpy(dst + static_cast<usize>(i) * 4u, words.data(),
                        sizeof(words));
        }
    }

    /// vitreo_tess_patch, on the CPU reference tessellator.
    void run_tess_patch(const graphics::BackendDispatchCall& call) {
        graphics::tess::TessPatchParams params{};
        if (call.constants == nullptr || call.constants_size < sizeof(params)) return;
        std::memcpy(&params, call.constants, sizeof(params));
        auto* factors = host(call.buffers[graphics::tess::TESS_FACTORS_INDEX]);
        auto* counts = host(call.buffers[graphics::tess::TESS_COUNTS_INDEX]);
        auto* points = host(call.buffers[graphics::tess::TESS_POINTS_INDEX]);
        auto* indices = host(call.buffers[graphics::tess::TESS_INDICES_INDEX]);
        if (factors == nullptr || counts == nullptr || points == nullptr ||
            indices == nullptr) {
            return;
        }
        u32 n = params.patches;
        u32 per_instance = std::max(params.patches_per_instance, 1u);
        if (params.count_mode != 0u) {
            auto* patch_count =
                host(call.buffers[graphics::tess::TESS_PATCH_COUNT_INDEX]);
            if (patch_count == nullptr) return;
            const u32* total = patch_count->words(
                call.buffer_offsets[graphics::tess::TESS_PATCH_COUNT_INDEX]);
            n = std::min(n, total[0] > params.first_patch ? total[0] - params.first_patch
                                                          : 0u);
            per_instance = std::max(total[1], 1u);
        }
        u32* factor_words =
            factors->words(call.buffer_offsets[graphics::tess::TESS_FACTORS_INDEX]);
        u32* count_words =
            counts->words(call.buffer_offsets[graphics::tess::TESS_COUNTS_INDEX]);
        const u32 count_limit = static_cast<u32>(counts->desc().byte_width / 4u);
        auto* primitive_ids =
            host(call.buffers[graphics::tess::TESS_PRIMITIVE_IDS_INDEX]);
        for (u32 tid = 0; tid < params.patches; ++tid) {
            if (2u * tid + 1u >= count_limit) return;
            if (tid >= n) {
                if (params.mode == 0u) {
                    count_words[2u * tid] = 0u;
                    count_words[2u * tid + 1u] = 0u;
                }
                continue;
            }
            float factor[8]{};
            const u32 words = std::min(params.factor_words, 8u);
            const u32* src = factor_words + static_cast<usize>(params.first_patch + tid) *
                                               params.factor_words;
            for (u32 i = 0; i < words; ++i) {
                std::memcpy(&factor[i], &src[i], sizeof(float));
            }
            graphics::tess::TessState state{};
            state.sink.points =
                points->words(call.buffer_offsets[graphics::tess::TESS_POINTS_INDEX]);
            state.sink.indices = indices->words(
                call.buffer_offsets[graphics::tess::TESS_INDICES_INDEX]);
            state.sink.primitive_ids =
                primitive_ids == nullptr
                    ? nullptr
                    : primitive_ids->words(call.buffer_offsets
                                               [graphics::tess::TESS_PRIMITIVE_IDS_INDEX]);
            state.sink.patch_index = params.first_patch + tid;
            state.sink.primitive_id = (params.first_patch + tid) % per_instance;
            state.sink.write_primitive_ids = params.write_primitive_ids;
            state.sink.write = 0u;
            if (params.mode == 1u) {
                state.sink.point_base = count_words[2u * tid];
                state.sink.index_base = count_words[2u * tid + 1u];
                state.sink.write = state.sink.point_base != 0xFFFFFFFFu ? 1u : 0u;
            }
            graphics::tess::tess_patch(state, params.domain, params.partitioning,
                                       params.output_primitive, factor);
            if (params.mode == 0u) {
                count_words[2u * tid] = static_cast<u32>(std::max(state.numPoints, 0));
                count_words[2u * tid + 1u] =
                    static_cast<u32>(std::max(state.numIndices, 0));
            }
        }
    }

    /// vitreo_tess_scan: exclusive prefix of each patch's counts, and the
    /// primitive total the repack's prepare reads.
    void run_tess_scan(const graphics::BackendDispatchCall& call) {
        graphics::tess::TessScanParams params{};
        if (call.constants == nullptr || call.constants_size < sizeof(params)) return;
        std::memcpy(&params, call.constants, sizeof(params));
        auto* counts = host(call.buffers[graphics::tess::TESS_SCAN_COUNTS_INDEX]);
        auto* block = host(call.buffers[graphics::tess::TESS_SCAN_BLOCK_INDEX]);
        auto* args = host(call.buffers[graphics::tess::TESS_SCAN_ARGS_INDEX]);
        if (counts == nullptr || block == nullptr || args == nullptr) return;
        u32* count_words =
            counts->words(call.buffer_offsets[graphics::tess::TESS_SCAN_COUNTS_INDEX]);
        const u32 words = static_cast<u32>(counts->desc().byte_width / 4u);
        std::vector<u32> bases(static_cast<usize>(params.patches) * 2u);
        u32 points = 0;
        u32 indices = 0;
        for (u32 i = 0; i < params.patches; ++i) {
            if (2u * i + 1u >= words) return;
            bases[2u * i] = points;
            bases[2u * i + 1u] = indices;
            points += count_words[2u * i];
            indices += count_words[2u * i + 1u];
        }
        const bool over = points > params.point_capacity ||
                          indices > params.index_capacity;
        const u32 vpp = std::max(params.vertices_per_primitive, 1u);
        for (u32 i = 0; i < params.patches; ++i) {
            count_words[2u * i] = over ? 0xFFFFFFFFu : bases[2u * i];
            count_words[2u * i + 1u] = bases[2u * i + 1u];
        }
        u32* block_words =
            block->words(call.buffer_offsets[graphics::tess::TESS_SCAN_BLOCK_INDEX]);
        block_words[graphics::tess::TESS_BLOCK_POINTS] = over ? 0u : points;
        block_words[graphics::tess::TESS_BLOCK_INDICES] = over ? 0u : indices;
        block_words[graphics::tess::TESS_BLOCK_PRIMITIVES] = over ? 0u : indices / vpp;
        block_words[graphics::tess::TESS_BLOCK_OVERFLOW] = over ? 1u : 0u;
        u32* arg_words =
            args->words(call.buffer_offsets[graphics::tess::TESS_SCAN_ARGS_INDEX]);
        arg_words[graphics::tess::TESS_ARGS_DOMAIN_DISPATCH] =
            over ? 0u : (points + graphics::kDomainComputeThreads - 1u) /
                            graphics::kDomainComputeThreads;
        arg_words[graphics::tess::TESS_ARGS_DOMAIN_DISPATCH + 1u] = 1u;
        arg_words[graphics::tess::TESS_ARGS_DOMAIN_DISPATCH + 2u] = 1u;
        arg_words[graphics::tess::TESS_ARGS_DRAW] = over ? 0u : indices;
        arg_words[graphics::tess::TESS_ARGS_DRAW + 1u] = 1u;
        arg_words[graphics::tess::TESS_ARGS_DRAW + 2u] = 0u;
        arg_words[graphics::tess::TESS_ARGS_DRAW + 3u] = 0u;
        arg_words[graphics::tess::TESS_ARGS_DRAW + 4u] = 0u;
    }
};

class Harness {
public:
    explicit Harness(bool is_64bit) : m_is_64bit(is_64bit) {
        EXPECT_TRUE(memory.map_region(kData, kDataSize, MemoryProtection::ReadWrite).is_ok());
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
        return m_is_64bit ? memory.read_u64(at).value() : memory.read_u32(at).value();
    }
    void write_ptr(GuestAddr at, u64 value) {
        EXPECT_TRUE((m_is_64bit ? memory.write_u64(at, value)
                                : memory.write_u32(at, static_cast<u32>(value)))
                        .is_ok());
    }
    void w32(GuestAddr at, u32 v) { EXPECT_TRUE(memory.write_u32(at, v).is_ok()); }
    void w64(GuestAddr at, u64 v) { EXPECT_TRUE(memory.write_u64(at, v).is_ok()); }
    [[nodiscard]] u32 r32(GuestAddr at) const { return memory.read_u32(at).value(); }

    struct Arg {
        u64 value;
        bool wide{false};
    };
    [[nodiscard]] static Arg wide(u64 v) { return {v, true}; }

    u64 call(const std::string& name, const std::vector<Arg>& args,
             bool wide_return = false) {
        const auto found = by_name.find(name);
        EXPECT_NE(found, by_name.end()) << name;
        if (found == by_name.end()) return ~0ull;
        return invoke(found->second->handler, args, wide_return,
                      found->second->stdcall_bytes, name);
    }

    template <typename Handler>
    u64 invoke(const Handler& handler, const std::vector<Arg>& args, bool wide_return,
               usize stdcall_bytes, const std::string& name) {
        cpu.gpr.rsp = kStack;
        cpu.gpr.rax = 0;
        if (!m_is_64bit) cpu.gpr.rdx = 0;
        if (m_is_64bit) {
            u64* regs[] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
            for (usize i = 0; i < args.size(); ++i) {
                if (i < 4) {
                    *regs[i] = args[i].value;
                } else {
                    EXPECT_TRUE(memory.write_u64(kStack + 0x28 + (i - 4) * 8, args[i].value)
                                    .is_ok());
                }
            }
        } else {
            usize slot = 0;
            for (const Arg& arg : args) {
                EXPECT_TRUE(memory.write_u32(kStack + 4 + slot * 4,
                                             static_cast<u32>(arg.value))
                                .is_ok());
                ++slot;
                if (arg.wide) {
                    EXPECT_TRUE(memory.write_u32(kStack + 4 + slot * 4,
                                                 static_cast<u32>(arg.value >> 32))
                                    .is_ok());
                    ++slot;
                }
            }
            if (!name.empty()) EXPECT_EQ(stdcall_bytes, slot * 4) << name;
        }
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        handler(marshaler);
        if (m_is_64bit) return cpu.gpr.rax;
        if (wide_return) {
            return (cpu.gpr.rax & 0xFFFFFFFFull) | ((cpu.gpr.rdx & 0xFFFFFFFFull) << 32);
        }
        return cpu.gpr.rax & 0xFFFFFFFFull;
    }

    u64 com(GuestAddr object, usize slot, std::vector<Arg> args, bool wide_return = false) {
        const u64 vtable = read_ptr(object);
        const u64 thunk = read_ptr(vtable + slot * ptr_size());
        const auto name = by_thunk.find(thunk);
        EXPECT_NE(name, by_thunk.end());
        if (name == by_thunk.end()) return ~0ull;
        args.insert(args.begin(), Arg{object});
        return call(name->second, args, wide_return);
    }

    GuestAddr iid(const GuidBytes& value) {
        EXPECT_TRUE(memory.write_bytes(kIid, value.data(), value.size()).is_ok());
        return kIid;
    }

    GuestAddr device() {
        EXPECT_EQ(call("D3D12CreateDevice",
                       {{0}, {D3D_FEATURE_LEVEL_11_0}, {iid(IID_ID3D12DEVICE)}, {kOut}}),
                  S_OK);
        return read_ptr(kOut);
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    SoBackend backend;
    std::unique_ptr<D3D12Manager> manager;
    DllExports exports;
    std::unordered_map<std::string, const ExportEntry*> by_name;
    std::unordered_map<u64, std::string> by_thunk;
    bool m_is_64bit;
};

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
    for (const auto& element : elements) {
        push_u32(data, strings_at + static_cast<u32>(strings.size()));
        push_u32(data, element.index);
        push_u32(data, element.system_value);
        push_u32(data, element.component_type);
        push_u32(data, element.reg);
        push_u32(data, element.mask | (element.mask << 8));
        for (const char* c = element.name; *c != 0; ++c) strings.push_back(*c);
        strings.push_back(0);
    }
    while (strings.size() % 4 != 0) strings.push_back(0);
    data.insert(data.end(), strings.begin(), strings.end());
    return data;
}

std::vector<u8> dxbc_stage(const std::vector<u32>& tokens, const std::vector<SigEl>& inputs,
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

/// o0 = (0,0,0,1), o1 = SV_VertexID.xxxx, o2.xy = SV_VertexID.
std::vector<u8> point_vertex_shader() {
    const std::vector<u32> tokens{
        0x00010040u, 31u,       0x04000060u, 0x00101012u, 0u, 6u,         0x04000067u,
        0x001020F2u, 0u,        1u,          0x03000065u, 0x001020F2u,    1u,
        0x03000065u, 0x00102032u, 2u,        0x08000036u, 0x001020F2u,    0u,
        0x00004002u, 0u,        0u,          0u,          0x3F800000u,    0x05000036u,
        0x001020F2u, 1u,        0x00101006u, 0u,          0x05000036u,    0x00102032u,
        2u,          0x00101006u, 0u,        0x0100003Eu,
    };
    return dxbc_stage(tokens, {{"SV_VertexID", 0, 6, 1, 0, 0x1}},
                      {{"SV_Position", 0, 1, 3, 0, 0xF},
                       {"TEXCOORD", 0, 0, 3, 1, 0xF},
                       {"TEXCOORD", 1, 0, 3, 2, 0x3}});
}

std::vector<u8> point_geometry_shader() {
    const std::vector<u32> tokens{
        0x00020040u, 36u,       0x05000061u, 0x002010F2u, 1u, 0u,      1u,
        0x0400005Fu, 0x002010F2u, 1u,        1u,          0x0100085Du, 0x0100085Cu,
        0x04000067u, 0x001020F2u, 0u,        1u,          0x03000065u, 0x001020F2u,
        1u,          0x0200005Eu, 1u,        0x06000036u, 0x001020F2u, 0u,
        0x00201E46u, 0u,        0u,          0x06000036u, 0x001020F2u, 1u,
        0x00201E46u, 0u,        1u,          0x01000013u, 0x0100003Eu,
    };
    return dxbc_stage(tokens,
                      {{"SV_Position", 0, 1, 3, 0, 0xF}, {"TEXCOORD", 0, 0, 3, 1, 0xF}},
                      {{"SV_Position", 0, 1, 3, 0, 0xF}, {"TEXCOORD", 0, 0, 3, 1, 0xF}});
}

std::vector<u8> dxbc_chunks(
    const std::vector<u32>& tokens,
    const std::vector<std::pair<u32, std::vector<u8>>>& chunks) {
    std::vector<u8> code;
    for (const u32 token : tokens) push_u32(code, token);
    std::vector<std::pair<u32, std::vector<u8>>> parts = chunks;
    parts.push_back({0x58454853u, code});
    u32 at = 32u + 4u * static_cast<u32>(parts.size());
    std::vector<u32> offsets;
    for (const auto& chunk : parts) {
        offsets.push_back(at);
        at += 8u + static_cast<u32>(chunk.second.size());
    }
    std::vector<u8> blob;
    push_u32(blob, 0x43425844u);
    for (int i = 0; i < 4; ++i) push_u32(blob, 0u);
    push_u32(blob, 1u);
    push_u32(blob, at);
    push_u32(blob, static_cast<u32>(parts.size()));
    for (const u32 offset : offsets) push_u32(blob, offset);
    for (const auto& chunk : parts) {
        push_u32(blob, chunk.first);
        push_u32(blob, static_cast<u32>(chunk.second.size()));
        blob.insert(blob.end(), chunk.second.begin(), chunk.second.end());
    }
    u8 hash[16]{};
    graphics::dxbc_container_checksum(blob.data(), at, hash);
    std::memcpy(blob.data() + 4, hash, sizeof(hash));
    return blob;
}

/// hs_5_0, integer triangle, max tessellation factor 1, every factor mov'd
/// to 1. The reference tessellator emits one triangle per patch.
std::vector<u8> constant_hull_shader() {
    std::vector<u32> tokens{0x00030050u, 0u, 0x01000071u, 0x01001893u, 0x01001894u,
                            0x01001095u, 0x01000896u, 0x01001897u, 0x02000098u,
                            float_bits(1.f), 0x0100086au};
    const auto fork = [&](u32 reg, u32 name) {
        tokens.insert(tokens.end(),
                      {0x01000073u, 0x04000067u, 0x00102012u, reg, name, 0x05000036u,
                       0x00102012u, reg, 0x00004001u, float_bits(1.f), 0x0100003Eu});
    };
    fork(0u, 0x11u);
    fork(1u, 0x12u);
    fork(2u, 0x13u);
    fork(3u, 0x14u);
    tokens[1] = static_cast<u32>(tokens.size());
    const std::vector<SigEl> factors{
        {"SV_TessFactor", 0, 13, 3, 0, 0x1},
        {"SV_TessFactor", 1, 13, 3, 1, 0x1},
        {"SV_TessFactor", 2, 13, 3, 2, 0x1},
        {"SV_InsideTessFactor", 0, 14, 3, 3, 0x1},
    };
    return dxbc_chunks(tokens, {{0x4E475349u, signature_chunk({})},
                                {0x4E47534Fu, signature_chunk({})},
                                {0x47534350u, signature_chunk(factors)}});
}

/// ds_5_0: `mov o0.xyzw, l(1, 2, 3, 4)` into TEXCOORD0. No SV_Position.
std::vector<u8> constant_domain_shader() {
    const auto out = domain_output_words();
    std::vector<u32> tokens{0x00040050u, 0u,       0x01001893u, 0x01001095u, 0x0100086au,
                            0x03000065u, 0x001020F2u, 0u,       0x08000036u, 0x001020F2u,
                            0u,          0x00004002u, out[0],   out[1],      out[2],
                            out[3],      0x0100003Eu};
    tokens[1] = static_cast<u32>(tokens.size());
    return dxbc_chunks(tokens, {{0x4E475349u, signature_chunk({})},
                                {0x4E47534Fu, signature_chunk({{"TEXCOORD", 0, 0, 3, 0, 0xF}})}});
}

GuestAddr make_root(Harness& h, GuestAddr dev, u32 flags) {
    RootSignatureDesc desc;
    desc.flags = flags;
    std::vector<u8> blob;
    std::string error;
    EXPECT_EQ(serialize_root_signature(desc, ROOT_SIGNATURE_VERSION_1_0, blob, error), S_OK)
        << error;
    EXPECT_TRUE(h.memory.write_bytes(kData + 0x1800, blob.data(), blob.size()).is_ok());
    EXPECT_EQ(h.com(dev, DeviceSlot::CreateRootSignature,
                    {{0},
                     {kData + 0x1800},
                     {blob.size()},
                     {h.iid(IID_ID3D12ROOTSIGNATURE)},
                     {kOut}}),
              S_OK);
    return h.read_ptr(kOut);
}

void put_shader(Harness& h, GuestAddr at, const std::vector<u8>& bytes, GuestAddr pointer,
                GuestAddr length) {
    EXPECT_TRUE(h.memory.write_bytes(at, bytes.data(), bytes.size()).is_ok());
    h.write_ptr(kDesc + pointer, at);
    h.write_ptr(kDesc + length, bytes.size());
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

GuestAddr make_buffer(Harness& h, GuestAddr dev, u64 width) {
    const auto& hp = layout::D3D12_HEAP_PROPERTIES[h.ptr_size() == 8];
    const auto& rd = layout::D3D12_RESOURCE_DESC[h.ptr_size() == 8];
    for (u32 i = 0; i < hp.size; i += 4) h.w32(kDesc + i, 0);
    for (u32 i = 0; i < rd.size; i += 4) h.w32(kDesc2 + i, 0);
    h.w32(kDesc + hp.Type, D3D12_HEAP_TYPE_UPLOAD);
    h.w32(kDesc2 + rd.Dimension, D3D12_RESOURCE_DIMENSION_BUFFER);
    h.w64(kDesc2 + rd.Width, width);
    h.w32(kDesc2 + rd.Height, 1);
    EXPECT_TRUE(h.memory.write_u16(kDesc2 + rd.DepthOrArraySize, 1).is_ok());
    EXPECT_TRUE(h.memory.write_u16(kDesc2 + rd.MipLevels, 1).is_ok());
    h.w32(kDesc2 + rd.SampleDesc_Count, 1);
    h.w32(kDesc2 + rd.Layout, D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
    EXPECT_EQ(h.com(dev, DeviceSlot::CreateCommittedResource,
                    {{kDesc},
                     {D3D12_HEAP_FLAG_NONE},
                     {kDesc2},
                     {D3D12_RESOURCE_STATE_GENERIC_READ},
                     {0},
                     {h.iid(IID_ID3D12RESOURCE)},
                     {kOut}}),
              S_OK);
    return h.read_ptr(kOut);
}

u64 gpu_va(Harness& h, GuestAddr resource) {
    return h.com(resource, ResourceSlot::GetGPUVirtualAddress, {}, true);
}

GuestAddr map_buffer(Harness& h, GuestAddr resource) {
    EXPECT_EQ(h.com(resource, ResourceSlot::Map, {{0}, {0}, {kOut2}}), S_OK);
    return h.read_ptr(kOut2);
}

void write_entry(Harness& h, GuestAddr at, u32 stream, GuestAddr name, u32 index, u8 start,
                 u8 count, u8 slot) {
    const auto& layout = layout::D3D12_SO_DECLARATION_ENTRY[h.ptr_size() == 8];
    for (u32 i = 0; i < layout.size; i += 4) h.w32(at + i, 0);
    h.w32(at + layout.Stream, stream);
    h.write_ptr(at + layout.SemanticName, name);
    h.w32(at + layout.SemanticIndex, index);
    EXPECT_TRUE(h.memory.write_u8(at + layout.StartComponent, start).is_ok());
    EXPECT_TRUE(h.memory.write_u8(at + layout.ComponentCount, count).is_ok());
    EXPECT_TRUE(h.memory.write_u8(at + layout.OutputSlot, slot).is_ok());
}

void set_one_target(Harness& h, GuestAddr list, u64 data_va, u64 size, u64 filled_va) {
    const auto& view = layout::D3D12_STREAM_OUTPUT_BUFFER_VIEW[h.ptr_size() == 8];
    const GuestAddr at = kData + 0x1600;
    h.w64(at + view.BufferLocation, data_va);
    h.w64(at + view.SizeInBytes, size);
    h.w64(at + view.BufferFilledSizeLocation, filled_va);
    EXPECT_EQ(h.com(list, GraphicsCommandListSlot::SOSetTargets, {{0}, {1}, {at}}), S_OK);
}

void draw_list(Harness& h, GuestAddr dev, GuestAddr pso, GuestAddr root, u32 topology,
               u32 vertices, GuestAddr data, u64 data_bytes, GuestAddr filled,
               bool bind_target, u32 draw_after_unbind = 0, u32 instances = 1,
               u32 start_vertex = 0) {
    const auto& ql = layout::D3D12_COMMAND_QUEUE_DESC[h.ptr_size() == 8];
    for (u32 i = 0; i < ql.size; i += 4) h.w32(kDesc + i, 0);
    h.w32(kDesc + ql.Type, D3D12_COMMAND_LIST_TYPE_DIRECT);
    EXPECT_EQ(h.com(dev, DeviceSlot::CreateCommandQueue,
                    {{kDesc}, {h.iid(IID_ID3D12COMMANDQUEUE)}, {kOut}}),
              S_OK);
    const GuestAddr q = h.read_ptr(kOut);
    EXPECT_EQ(h.com(dev, DeviceSlot::CreateCommandAllocator,
                    {{D3D12_COMMAND_LIST_TYPE_DIRECT},
                     {h.iid(IID_ID3D12COMMANDALLOCATOR)},
                     {kOut}}),
              S_OK);
    const GuestAddr alloc = h.read_ptr(kOut);
    EXPECT_EQ(h.com(dev, DeviceSlot::CreateCommandList,
                    {{0},
                     {D3D12_COMMAND_LIST_TYPE_DIRECT},
                     {alloc},
                     {0},
                     {h.iid(IID_ID3D12GRAPHICSCOMMANDLIST)},
                     {kOut}}),
              S_OK);
    const GuestAddr list = h.read_ptr(kOut);
    EXPECT_EQ(h.com(list, GraphicsCommandListSlot::SetPipelineState, {{pso}}), S_OK);
    EXPECT_EQ(h.com(list, GraphicsCommandListSlot::SetGraphicsRootSignature, {{root}}), S_OK);
    EXPECT_EQ(h.com(list, GraphicsCommandListSlot::IASetPrimitiveTopology, {{topology}}),
              S_OK);
    if (bind_target) {
        set_one_target(h, list, gpu_va(h, data), data_bytes, gpu_va(h, filled));
    } else {
        // A null view list unbinds StartSlot. The draw must not append.
        EXPECT_EQ(h.com(list, GraphicsCommandListSlot::SOSetTargets, {{0}, {1}, {0}}),
                  S_OK);
    }
    EXPECT_EQ(h.com(list, GraphicsCommandListSlot::DrawInstanced,
                    {{vertices}, {instances}, {start_vertex}, {0}}),
              S_OK);
    if (draw_after_unbind != 0) {
        EXPECT_EQ(h.com(list, GraphicsCommandListSlot::SOSetTargets, {{0}, {1}, {0}}),
                  S_OK);
        EXPECT_EQ(h.com(list, GraphicsCommandListSlot::DrawInstanced,
                        {{draw_after_unbind}, {1}, {0}, {0}}),
                  S_OK);
    }
    EXPECT_EQ(h.com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    h.write_ptr(kArray, list);
    EXPECT_EQ(h.com(q, CommandQueueSlot::ExecuteCommandLists, {{1}, {kArray}}), S_OK);
}

class D3D12StreamOutputTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        if (!d3d12_enabled()) GTEST_SKIP() << "set VITREO_D3D12=1";
        h = std::make_unique<Harness>(GetParam());
    }
    std::unique_ptr<Harness> h;
};

INSTANTIATE_TEST_SUITE_P(Bitness, D3D12StreamOutputTest, ::testing::Values(true, false),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

TEST_P(D3D12StreamOutputTest, StreamOutputRejectsABadDeclarationAndBadTargets) {
    const GuestAddr dev = h->device();
    const GuestAddr root =
        make_root(*h, dev, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT);
    prime_gfx(*h, root, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    const auto& l = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[GetParam()];
    put_shader(*h, kData + 0x2000, point_vertex_shader(), l.VS_pShaderBytecode,
               l.VS_BytecodeLength);
    const GuestAddr name = kData + 0x3000;
    EXPECT_TRUE(h->memory.write_bytes(name, reinterpret_cast<const u8*>("NOPE"), 5).is_ok());
    write_entry(*h, kData + 0x3100, 0, name, 0, 0, 4, 0);
    h->w32(kData + 0x3200, 16);
    h->write_ptr(kDesc + l.StreamOutput_pSODeclaration, kData + 0x3100);
    h->w32(kDesc + l.StreamOutput_NumEntries, 1);
    h->write_ptr(kDesc + l.StreamOutput_pBufferStrides, kData + 0x3200);
    h->w32(kDesc + l.StreamOutput_NumStrides, 1);
    h->w32(kDesc + l.StreamOutput_RasterizedStream, D3D12_SO_NO_RASTERIZED_STREAM);
    h->w64(kOut, 0x1111);
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              E_INVALIDARG);
    EXPECT_EQ(h->read_ptr(kOut), 0u);

    // Learn: the root signature must allow stream output.
    const GuestAddr bare = make_root(*h, dev, 0);
    h->write_ptr(kDesc + l.pRootSignature, bare);
    EXPECT_TRUE(h->memory
                    .write_bytes(name, reinterpret_cast<const u8*>("TEXCOORD"), 9)
                    .is_ok());
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              E_INVALIDARG);

    h->write_ptr(kDesc + l.pRootSignature, root);
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK);

    // A geometry shader that does not declare stream 1.
    put_shader(*h, kData + 0x4000, point_geometry_shader(), l.GS_pShaderBytecode,
               l.GS_BytecodeLength);
    h->w32(kDesc + l.PrimitiveTopologyType, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);
    write_entry(*h, kData + 0x3100, 1, name, 0, 0, 4, 0);
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              E_INVALIDARG);
    write_entry(*h, kData + 0x3100, 0, name, 0, 0, 4, 0);
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK);

    // Learn (D3D12_GRAPHICS_PIPELINE_STATE_DESC): the hull and domain
    // shaders are both set or both null. The declaration names the domain
    // shader's SV_Position, so the refusal is the missing hull shader.
    h->write_ptr(kDesc + l.GS_pShaderBytecode, 0);
    h->write_ptr(kDesc + l.GS_BytecodeLength, 0);
    put_shader(*h, kData + 0x2000, point_vertex_shader(), l.VS_pShaderBytecode,
               l.VS_BytecodeLength);
    const std::vector<u8> ds(tess_ds_tri, tess_ds_tri + sizeof(tess_ds_tri));
    put_shader(*h, kData + 0x6000, ds, l.DS_pShaderBytecode, l.DS_BytecodeLength);
    h->w32(kDesc + l.PrimitiveTopologyType, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    EXPECT_TRUE(h->memory
                    .write_bytes(name, reinterpret_cast<const u8*>("SV_Position"), 12)
                    .is_ok());
    write_entry(*h, kData + 0x3100, 0, name, 0, 0, 4, 0);
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              E_INVALIDARG);

    EXPECT_EQ(h->com(dev, DeviceSlot::CreateCommandAllocator,
                    {{D3D12_COMMAND_LIST_TYPE_DIRECT},
                     {h->iid(IID_ID3D12COMMANDALLOCATOR)},
                     {kOut}}),
              S_OK);
    const GuestAddr alloc = h->read_ptr(kOut);
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateCommandList,
                    {{0},
                     {D3D12_COMMAND_LIST_TYPE_DIRECT},
                     {alloc},
                     {0},
                     {h->iid(IID_ID3D12GRAPHICSCOMMANDLIST)},
                     {kOut}}),
              S_OK);
    const GuestAddr list = h->read_ptr(kOut);
    const GuestAddr data = make_buffer(*h, dev, 256);
    const GuestAddr counter = make_buffer(*h, dev, 256);
    const auto& view = layout::D3D12_STREAM_OUTPUT_BUFFER_VIEW[GetParam()];
    const GuestAddr at = kData + 0x1600;
    h->w64(at + view.BufferLocation, gpu_va(*h, data) + 1);
    h->w64(at + view.SizeInBytes, 64);
    h->w64(at + view.BufferFilledSizeLocation, gpu_va(*h, counter));
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::SOSetTargets, {{0}, {1}, {at}}), S_OK);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), E_INVALIDARG);

    EXPECT_EQ(h->com(dev, DeviceSlot::CreateCommandAllocator,
                    {{D3D12_COMMAND_LIST_TYPE_DIRECT},
                     {h->iid(IID_ID3D12COMMANDALLOCATOR)},
                     {kOut}}),
              S_OK);
    const GuestAddr alloc2 = h->read_ptr(kOut);
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateCommandList,
                    {{0},
                     {D3D12_COMMAND_LIST_TYPE_DIRECT},
                     {alloc2},
                     {0},
                     {h->iid(IID_ID3D12GRAPHICSCOMMANDLIST)},
                     {kOut}}),
              S_OK);
    const GuestAddr list2 = h->read_ptr(kOut);
    h->w64(at + view.BufferLocation, gpu_va(*h, data));
    h->w64(at + view.BufferFilledSizeLocation, 0x1000);
    EXPECT_EQ(h->com(list2, GraphicsCommandListSlot::SOSetTargets, {{0}, {1}, {at}}), S_OK);
    EXPECT_EQ(h->com(list2, GraphicsCommandListSlot::Close, {}), E_INVALIDARG);

    EXPECT_EQ(h->com(dev, DeviceSlot::CreateCommandAllocator,
                    {{D3D12_COMMAND_LIST_TYPE_DIRECT},
                     {h->iid(IID_ID3D12COMMANDALLOCATOR)},
                     {kOut}}),
              S_OK);
    const GuestAddr alloc3 = h->read_ptr(kOut);
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateCommandList,
                    {{0},
                     {D3D12_COMMAND_LIST_TYPE_DIRECT},
                     {alloc3},
                     {0},
                     {h->iid(IID_ID3D12GRAPHICSCOMMANDLIST)},
                     {kOut}}),
              S_OK);
    const GuestAddr list3 = h->read_ptr(kOut);
    EXPECT_EQ(h->com(list3, GraphicsCommandListSlot::SOSetTargets, {{3}, {2}, {at}}), S_OK);
    EXPECT_EQ(h->com(list3, GraphicsCommandListSlot::Close, {}), E_INVALIDARG);

    EXPECT_EQ(h->com(dev, DeviceSlot::CreateCommandAllocator,
                    {{D3D12_COMMAND_LIST_TYPE_DIRECT},
                     {h->iid(IID_ID3D12COMMANDALLOCATOR)},
                     {kOut}}),
              S_OK);
    const GuestAddr alloc4 = h->read_ptr(kOut);
    EXPECT_EQ(h->com(dev, DeviceSlot::CreateCommandList,
                    {{0},
                     {D3D12_COMMAND_LIST_TYPE_DIRECT},
                     {alloc4},
                     {0},
                     {h->iid(IID_ID3D12GRAPHICSCOMMANDLIST)},
                     {kOut}}),
              S_OK);
    const GuestAddr list4 = h->read_ptr(kOut);
    h->w64(at + view.BufferFilledSizeLocation, gpu_va(*h, counter));
    EXPECT_EQ(h->com(list4, GraphicsCommandListSlot::SOSetTargets, {{0}, {1}, {at}}), S_OK);
    EXPECT_EQ(h->com(list4, GraphicsCommandListSlot::SOSetTargets, {{0}, {1}, {0}}), S_OK);
    EXPECT_EQ(h->com(list4, GraphicsCommandListSlot::Close, {}), S_OK);
}

TEST_P(D3D12StreamOutputTest, StreamOutputWritesDeclaredComponentsAndFilledSize) {
    const GuestAddr dev = h->device();
    const GuestAddr root =
        make_root(*h, dev, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT);
    prime_gfx(*h, root, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    const auto& l = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[GetParam()];
    put_shader(*h, kData + 0x2000, point_vertex_shader(), l.VS_pShaderBytecode,
               l.VS_BytecodeLength);
    const GuestAddr name = kData + 0x3000;
    EXPECT_TRUE(h->memory
                    .write_bytes(name, reinterpret_cast<const u8*>("TEXCOORD"), 9)
                    .is_ok());
    write_entry(*h, kData + 0x3100, 0, name, 0, 0, 4, 0);
    h->w32(kData + 0x3200, 16);
    h->write_ptr(kDesc + l.StreamOutput_pSODeclaration, kData + 0x3100);
    h->w32(kDesc + l.StreamOutput_NumEntries, 1);
    h->write_ptr(kDesc + l.StreamOutput_pBufferStrides, kData + 0x3200);
    h->w32(kDesc + l.StreamOutput_NumStrides, 1);
    h->w32(kDesc + l.StreamOutput_RasterizedStream, D3D12_SO_NO_RASTERIZED_STREAM);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK);
    const GuestAddr pso = h->read_ptr(kOut);

    const GuestAddr data = make_buffer(*h, dev, 256);
    const GuestAddr filled = make_buffer(*h, dev, 256);
    h->backend.draws = 0;
    draw_list(*h, dev, pso, root, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 6, data, 64, filled,
              true);
    // Two triangles need 96 bytes. 64 holds one (48) and not a second, so the
    // second primitive is dropped whole and the filled size is 48.
    const GuestAddr data_map = map_buffer(*h, data);
    const GuestAddr fill_map = map_buffer(*h, filled);
    EXPECT_EQ(h->r32(fill_map), 48u);
    for (u32 vertex = 0; vertex < 3; ++vertex) {
        for (u32 component = 0; component < 4; ++component) {
            EXPECT_EQ(h->r32(data_map + (vertex * 4u + component) * 4u), vertex)
                << vertex << " " << component;
        }
    }
    for (u32 byte = 48; byte < 64; ++byte) {
        EXPECT_EQ(h->memory.read_u8(data_map + byte).value(), 0u) << byte;
    }
    EXPECT_EQ(h->backend.draws, 1u);
    EXPECT_TRUE(h->backend.last_capture);

    // A 4-vertex triangle strip is two triangles: (0,1,2) and (1,3,2).
    const GuestAddr strip_data = make_buffer(*h, dev, 256);
    const GuestAddr strip_fill = make_buffer(*h, dev, 256);
    // The strip is followed, on the same list, by a null SOSetTargets and
    // another draw. The null list unbinds slot 0, so the second draw does
    // not append.
    draw_list(*h, dev, pso, root, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, 4, strip_data,
              256, strip_fill, true, 3);
    const GuestAddr strip_map = map_buffer(*h, strip_data);
    const GuestAddr strip_fill_map = map_buffer(*h, strip_fill);
    const u32 strip_ids[] = {0, 1, 2, 1, 3, 2};
    EXPECT_EQ(h->r32(strip_fill_map), 96u);
    for (u32 vertex = 0; vertex < 6; ++vertex) {
        for (u32 component = 0; component < 4; ++component) {
            EXPECT_EQ(h->r32(strip_map + (vertex * 4u + component) * 4u),
                      strip_ids[vertex]);
        }
    }

    // RasterizedStream 0 still writes the stream and also rasterizes.
    prime_gfx(*h, root, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    put_shader(*h, kData + 0x2000, point_vertex_shader(), l.VS_pShaderBytecode,
               l.VS_BytecodeLength);
    h->write_ptr(kDesc + l.StreamOutput_pSODeclaration, kData + 0x3100);
    h->w32(kDesc + l.StreamOutput_NumEntries, 1);
    h->write_ptr(kDesc + l.StreamOutput_pBufferStrides, kData + 0x3200);
    h->w32(kDesc + l.StreamOutput_NumStrides, 1);
    h->w32(kDesc + l.StreamOutput_RasterizedStream, 0);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK);
    const GuestAddr raster = h->read_ptr(kOut);
    const GuestAddr raster_data = make_buffer(*h, dev, 256);
    const GuestAddr raster_fill = make_buffer(*h, dev, 256);
    h->backend.draws = 0;
    draw_list(*h, dev, raster, root, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3,
              raster_data, 256, raster_fill, true);
    const GuestAddr raster_map = map_buffer(*h, raster_data);
    const GuestAddr raster_fill_map = map_buffer(*h, raster_fill);
    EXPECT_EQ(h->r32(raster_fill_map), 48u);
    EXPECT_EQ(h->r32(raster_map), 0u);
    EXPECT_EQ(h->r32(raster_map + 16), 1u);
    EXPECT_EQ(h->backend.draws, 2u);
    EXPECT_FALSE(h->backend.last_capture);
}

// Learn (DrawInstanced) and D3D11.3 8.16/8.17: SV_VertexID of a
// non-indexed draw counts from StartVertexLocation and restarts with each
// instance; stream output appends whole primitives in draw order, instance by
// instance. An instanced draw through a geometry shader is expanded once for
// every instance (the GPU-assembled path), so its records must keep that
// order: 3 points x 2 instances from vertex 4 write 4, 5, 6, 4, 5, 6.
TEST_P(D3D12StreamOutputTest, InstancedGeometryDrawStreamsEachInstanceInOrder) {
    const GuestAddr dev = h->device();
    const GuestAddr root =
        make_root(*h, dev, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT);
    prime_gfx(*h, root, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);
    const auto& l = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[GetParam()];
    put_shader(*h, kData + 0x2000, point_vertex_shader(), l.VS_pShaderBytecode,
               l.VS_BytecodeLength);
    put_shader(*h, kData + 0x4000, point_geometry_shader(), l.GS_pShaderBytecode,
               l.GS_BytecodeLength);
    const GuestAddr name = kData + 0x3000;
    EXPECT_TRUE(h->memory
                    .write_bytes(name, reinterpret_cast<const u8*>("TEXCOORD"), 9)
                    .is_ok());
    write_entry(*h, kData + 0x3100, 0, name, 0, 0, 4, 0);
    h->w32(kData + 0x3200, 16);
    h->write_ptr(kDesc + l.StreamOutput_pSODeclaration, kData + 0x3100);
    h->w32(kDesc + l.StreamOutput_NumEntries, 1);
    h->write_ptr(kDesc + l.StreamOutput_pBufferStrides, kData + 0x3200);
    h->w32(kDesc + l.StreamOutput_NumStrides, 1);
    h->w32(kDesc + l.StreamOutput_RasterizedStream, D3D12_SO_NO_RASTERIZED_STREAM);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK);
    const GuestAddr pso = h->read_ptr(kOut);
    const GuestAddr data = make_buffer(*h, dev, 256);
    const GuestAddr filled = make_buffer(*h, dev, 256);
    draw_list(*h, dev, pso, root, D3D_PRIMITIVE_TOPOLOGY_POINTLIST, 3, data, 256, filled,
              true, 0, 2, 4);
    const GuestAddr data_map = map_buffer(*h, data);
    const GuestAddr fill_map = map_buffer(*h, filled);
    EXPECT_EQ(h->r32(fill_map), 96u);
    const u32 expected[6] = {4, 5, 6, 4, 5, 6};
    for (u32 record = 0; record < 6; ++record) {
        EXPECT_EQ(h->r32(data_map + record * 16u), expected[record]) << record;
    }
}

TEST_P(D3D12StreamOutputTest, StreamOutputGeometryIndirectAndStreamSubobject) {
    const GuestAddr dev = h->device();
    const GuestAddr root =
        make_root(*h, dev, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT);
    prime_gfx(*h, root, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);
    const auto& l = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[GetParam()];
    put_shader(*h, kData + 0x2000, point_vertex_shader(), l.VS_pShaderBytecode,
               l.VS_BytecodeLength);
    put_shader(*h, kData + 0x4000, point_geometry_shader(), l.GS_pShaderBytecode,
               l.GS_BytecodeLength);
    const GuestAddr name = kData + 0x3000;
    EXPECT_TRUE(h->memory
                    .write_bytes(name, reinterpret_cast<const u8*>("TEXCOORD"), 9)
                    .is_ok());
    write_entry(*h, kData + 0x3100, 0, name, 0, 0, 4, 0);
    h->w32(kData + 0x3200, 16);
    h->write_ptr(kDesc + l.StreamOutput_pSODeclaration, kData + 0x3100);
    h->w32(kDesc + l.StreamOutput_NumEntries, 1);
    h->write_ptr(kDesc + l.StreamOutput_pBufferStrides, kData + 0x3200);
    h->w32(kDesc + l.StreamOutput_NumStrides, 1);
    h->w32(kDesc + l.StreamOutput_RasterizedStream, D3D12_SO_NO_RASTERIZED_STREAM);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK);
    const GuestAddr pso = h->read_ptr(kOut);
    const GuestAddr data = make_buffer(*h, dev, 256);
    const GuestAddr filled = make_buffer(*h, dev, 256);
    draw_list(*h, dev, pso, root, D3D_PRIMITIVE_TOPOLOGY_POINTLIST, 3, data, 256, filled,
              true);
    const GuestAddr data_map = map_buffer(*h, data);
    const GuestAddr fill_map = map_buffer(*h, filled);
    EXPECT_EQ(h->r32(fill_map), 48u);
    for (u32 vertex = 0; vertex < 3; ++vertex) {
        EXPECT_EQ(h->r32(data_map + vertex * 16u), vertex);
    }

    // Pipeline-state stream subobject, then ExecuteIndirect of one triangle.
    h->write_ptr(kDesc + l.GS_pShaderBytecode, 0);
    h->write_ptr(kDesc + l.GS_BytecodeLength, 0);
    h->w32(kDesc + l.PrimitiveTopologyType, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    const bool x64 = GetParam();
    auto align = [](u64 v, u64 a) { return (v + a - 1) / a * a; };
    const u64 pointer = x64 ? 8u : 4u;
    const GuestAddr stream = kData + 0x7000;
    u64 cursor = 0;
    const auto sub_root = [&](GuestAddr value) {
        h->w32(stream + cursor, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE);
        const u64 at = align(cursor + 4, pointer);
        h->write_ptr(stream + at, value);
        cursor = align(at + pointer, pointer);
    };
    const auto sub_vs = [&]() {
        h->w32(stream + cursor, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS);
        const u64 at = align(cursor + 4, pointer);
        const auto& bc = layout::D3D12_SHADER_BYTECODE[x64];
        h->write_ptr(stream + at + bc.pShaderBytecode, kData + 0x2000);
        h->write_ptr(stream + at + bc.BytecodeLength, point_vertex_shader().size());
        cursor = align(at + bc.size, pointer);
    };
    const auto sub_so = [&]() {
        h->w32(stream + cursor, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT);
        const u64 at = align(cursor + 4, pointer);
        const auto& so = layout::D3D12_STREAM_OUTPUT_DESC[x64];
        h->write_ptr(stream + at + so.pSODeclaration, kData + 0x3100);
        h->w32(stream + at + so.NumEntries, 1);
        h->write_ptr(stream + at + so.pBufferStrides, kData + 0x3200);
        h->w32(stream + at + so.NumStrides, 1);
        h->w32(stream + at + so.RasterizedStream, D3D12_SO_NO_RASTERIZED_STREAM);
        cursor = align(at + so.size, pointer);
    };
    const auto sub_topo = [&]() {
        h->w32(stream + cursor, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY);
        const u64 at = align(cursor + 4, 4);
        h->w32(stream + at, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        cursor = align(at + 4, pointer);
    };
    sub_root(root);
    sub_vs();
    sub_so();
    sub_topo();
    const auto& sd = layout::D3D12_PIPELINE_STATE_STREAM_DESC[x64];
    const GuestAddr desc = kData + 0x7800;
    h->w64(desc + sd.SizeInBytes, cursor);
    h->write_ptr(desc + sd.pPipelineStateSubobjectStream, stream);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreatePipelineState,
                     {{desc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK);
    const GuestAddr stream_pso = h->read_ptr(kOut);

    const auto& arg = layout::D3D12_INDIRECT_ARGUMENT_DESC[x64];
    const auto& sig = layout::D3D12_COMMAND_SIGNATURE_DESC[x64];
    const GuestAddr arg_at = kData + 0x7900;
    const GuestAddr sig_at = kData + 0x7A00;
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
    const GuestAddr args = make_buffer(*h, dev, 256);
    const GuestAddr args_map = map_buffer(*h, args);
    h->w32(args_map + 0, 3);
    h->w32(args_map + 4, 1);
    h->w32(args_map + 8, 0);
    h->w32(args_map + 12, 0);

    const auto& ql = layout::D3D12_COMMAND_QUEUE_DESC[x64];
    for (u32 i = 0; i < ql.size; i += 4) h->w32(kDesc + i, 0);
    h->w32(kDesc + ql.Type, D3D12_COMMAND_LIST_TYPE_DIRECT);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateCommandQueue,
                     {{kDesc}, {h->iid(IID_ID3D12COMMANDQUEUE)}, {kOut}}),
              S_OK);
    const GuestAddr queue = h->read_ptr(kOut);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateCommandAllocator,
                     {{D3D12_COMMAND_LIST_TYPE_DIRECT},
                      {h->iid(IID_ID3D12COMMANDALLOCATOR)},
                      {kOut}}),
              S_OK);
    const GuestAddr alloc = h->read_ptr(kOut);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateCommandList,
                     {{0},
                      {D3D12_COMMAND_LIST_TYPE_DIRECT},
                      {alloc},
                      {0},
                      {h->iid(IID_ID3D12GRAPHICSCOMMANDLIST)},
                      {kOut}}),
              S_OK);
    const GuestAddr list = h->read_ptr(kOut);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::SetPipelineState, {{stream_pso}}), S_OK);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::SetGraphicsRootSignature, {{root}}),
              S_OK);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::IASetPrimitiveTopology,
                     {{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST}}),
              S_OK);
    const GuestAddr indirect_data = make_buffer(*h, dev, 256);
    const GuestAddr indirect_fill = make_buffer(*h, dev, 256);
    set_one_target(*h, list, gpu_va(*h, indirect_data), 256, gpu_va(*h, indirect_fill));
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::ExecuteIndirect,
                     {{signature},
                      {1},
                      {args},
                      Harness::wide(0),
                      {0},
                      Harness::wide(0)}),
              S_OK);
    ASSERT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    h->write_ptr(kArray, list);
    EXPECT_EQ(h->com(queue, CommandQueueSlot::ExecuteCommandLists, {{1}, {kArray}}), S_OK);
    const GuestAddr out_map = map_buffer(*h, indirect_data);
    const GuestAddr out_fill = map_buffer(*h, indirect_fill);
    EXPECT_EQ(h->r32(out_fill), 48u);
    EXPECT_EQ(h->r32(out_map), 0u);
    EXPECT_EQ(h->r32(out_map + 16), 1u);
    EXPECT_EQ(h->r32(out_map + 32), 2u);
}

TEST_P(D3D12StreamOutputTest, StreamOutputDomainShaderWritesItsOutputs) {
    // Integer tessellation factor 1 is one triangle (D3D11 fixed-function
    // tessellator, the minimum-factor case). A 3-control-point patch list of
    // 6 vertices is two patches, so two triangles, six vertices. Each vertex
    // is the domain shader's l(1, 2, 3, 4), copied as raw words. A target
    // that holds one triangle drops the next whole primitive
    // (d3d11_stream_out_probe; Learn, BufferFilledSize is the byte count).
    const GuestAddr dev = h->device();
    const GuestAddr root =
        make_root(*h, dev, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT);
    prime_gfx(*h, root, D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH);
    const auto& l = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[GetParam()];
    put_shader(*h, kData + 0x2000, point_vertex_shader(), l.VS_pShaderBytecode,
               l.VS_BytecodeLength);
    put_shader(*h, kData + 0x5000, constant_hull_shader(), l.HS_pShaderBytecode,
               l.HS_BytecodeLength);
    put_shader(*h, kData + 0x6000, constant_domain_shader(), l.DS_pShaderBytecode,
               l.DS_BytecodeLength);
    const GuestAddr name = kData + 0x7000;
    EXPECT_TRUE(h->memory
                    .write_bytes(name, reinterpret_cast<const u8*>("TEXCOORD"), 9)
                    .is_ok());
    write_entry(*h, kData + 0x7100, 0, name, 0, 0, 4, 0);
    h->w32(kData + 0x7200, 16);
    h->write_ptr(kDesc + l.StreamOutput_pSODeclaration, kData + 0x7100);
    h->w32(kDesc + l.StreamOutput_NumEntries, 1);
    h->write_ptr(kDesc + l.StreamOutput_pBufferStrides, kData + 0x7200);
    h->w32(kDesc + l.StreamOutput_NumStrides, 1);
    h->w32(kDesc + l.StreamOutput_RasterizedStream, D3D12_SO_NO_RASTERIZED_STREAM);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK);
    const GuestAddr pso = h->read_ptr(kOut);
    const auto words = domain_output_words();
    const auto expect_vertices = [&](GuestAddr mapped, u32 vertices) {
        for (u32 vertex = 0; vertex < vertices; ++vertex) {
            for (u32 component = 0; component < 4u; ++component) {
                EXPECT_EQ(h->r32(mapped + (vertex * 4u + component) * 4u),
                          words[component])
                    << vertex << " " << component;
            }
        }
    };

    const GuestAddr data = make_buffer(*h, dev, 256);
    const GuestAddr filled = make_buffer(*h, dev, 256);
    h->backend.draws = 0;
    draw_list(*h, dev, pso, root, D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST, 6,
              data, 256, filled, true);
    const GuestAddr data_map = map_buffer(*h, data);
    const GuestAddr fill_map = map_buffer(*h, filled);
    EXPECT_EQ(h->r32(fill_map), 96u);
    expect_vertices(data_map, 6);
    EXPECT_EQ(h->backend.draws, 1u);
    EXPECT_TRUE(h->backend.last_capture);

    const GuestAddr small = make_buffer(*h, dev, 256);
    const GuestAddr small_fill = make_buffer(*h, dev, 256);
    h->backend.draws = 0;
    draw_list(*h, dev, pso, root, D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST, 6,
              small, 64, small_fill, true);
    const GuestAddr small_map = map_buffer(*h, small);
    const GuestAddr small_fill_map = map_buffer(*h, small_fill);
    EXPECT_EQ(h->r32(small_fill_map), 48u);
    expect_vertices(small_map, 3);
    for (u32 byte = 48; byte < 64; ++byte) {
        EXPECT_EQ(h->memory.read_u8(small_map + byte).value(), 0u) << byte;
    }
    EXPECT_EQ(h->backend.draws, 1u);
}

// d3d12.h: SizeInBytes is UINT64. Learn ("Stream Output Counters"): the
// filled-size value is a 32-bit byte count of the vertices that fit, and a
// primitive that does not fit is dropped whole. 4 GiB + 32 truncated to 32
// bits cannot hold one 48-byte triangle; the UINT64 capacity can.
TEST(StreamOutPrepareModel, CapacityAboveFourGiBKeepsASmallDraw) {
    graphics::SoPrepareParams wide{};
    wide.primitives = 1;
    wide.vertices_per_primitive = 3;
    wide.slot_mask = 1u;
    wide.strides = {16, 0, 0, 0};
    wide.sizes = {0x100000000ull + 32ull, 0, 0, 0};
    std::array<u32, 4> filled{};
    std::array<u32*, 4> ptrs{&filled[0], &filled[1], &filled[2], &filled[3]};
    std::array<u32, graphics::SO_OP_WORDS> op{};
    std::array<u32, graphics::SO_STATS_WORDS> stats{};
    graphics::stream_out_prepare_model(wide, 0, ptrs, op, stats);
    EXPECT_EQ(op[graphics::SO_OP_VERTEX_COUNT], 3u);
    EXPECT_EQ(filled[0], 48u);

    graphics::SoPrepareParams narrow = wide;
    narrow.sizes[0] = 32;
    filled = {};
    op = {};
    stats = {};
    graphics::stream_out_prepare_model(narrow, 0, ptrs, op, stats);
    EXPECT_EQ(op[graphics::SO_OP_VERTEX_COUNT], 0u);
    EXPECT_EQ(filled[0], 0u);
}

// ExecuteIndirect through a geometry shader with a count buffer: the
// commands, the count and the primitive assembly are GPU data, so replay
// resolves them on the GPU timeline (vitreo_indirect_count,
// vitreo_ia_resolve) and never waits for the GPU. The streamed points are
// the direct draw's: the count (1) stops the second command.
TEST_P(D3D12StreamOutputTest, GeometryExecuteIndirectWithACountBufferNeverWaits) {
    const GuestAddr dev = h->device();
    const GuestAddr root =
        make_root(*h, dev, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT);
    prime_gfx(*h, root, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);
    const auto& l = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[GetParam()];
    put_shader(*h, kData + 0x2000, point_vertex_shader(), l.VS_pShaderBytecode,
               l.VS_BytecodeLength);
    put_shader(*h, kData + 0x4000, point_geometry_shader(), l.GS_pShaderBytecode,
               l.GS_BytecodeLength);
    const GuestAddr name = kData + 0x3000;
    EXPECT_TRUE(h->memory
                    .write_bytes(name, reinterpret_cast<const u8*>("TEXCOORD"), 9)
                    .is_ok());
    write_entry(*h, kData + 0x3100, 0, name, 0, 0, 4, 0);
    h->w32(kData + 0x3200, 16);
    h->write_ptr(kDesc + l.StreamOutput_pSODeclaration, kData + 0x3100);
    h->w32(kDesc + l.StreamOutput_NumEntries, 1);
    h->write_ptr(kDesc + l.StreamOutput_pBufferStrides, kData + 0x3200);
    h->w32(kDesc + l.StreamOutput_NumStrides, 1);
    h->w32(kDesc + l.StreamOutput_RasterizedStream, D3D12_SO_NO_RASTERIZED_STREAM);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateGraphicsPipelineState,
                     {{kDesc}, {h->iid(IID_ID3D12PIPELINESTATE)}, {kOut}}),
              S_OK);
    const GuestAddr pso = h->read_ptr(kOut);

    const bool x64 = GetParam();
    const auto& arg = layout::D3D12_INDIRECT_ARGUMENT_DESC[x64];
    const auto& sig = layout::D3D12_COMMAND_SIGNATURE_DESC[x64];
    const GuestAddr arg_at = kData + 0x7900;
    const GuestAddr sig_at = kData + 0x7A00;
    for (u32 i = 0; i < arg.size; i += 4) h->w32(arg_at + i, 0);
    h->w32(arg_at + arg.Type, D3D12_INDIRECT_ARGUMENT_TYPE_DRAW);
    for (u32 i = 0; i < sig.size; i += 4) h->w32(sig_at + i, 0);
    h->w32(sig_at + sig.ByteStride, 32);
    h->w32(sig_at + sig.NumArgumentDescs, 1);
    h->write_ptr(sig_at + sig.pArgumentDescs, arg_at);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateCommandSignature,
                     {{sig_at}, {0}, {h->iid(IID_ID3D12COMMANDSIGNATURE)}, {kOut}}),
              S_OK);
    const GuestAddr signature = h->read_ptr(kOut);
    // Two commands 32 bytes apart: {3 vertices, 1 instance, 0, 0} each.
    const GuestAddr args = make_buffer(*h, dev, 256);
    const GuestAddr args_map = map_buffer(*h, args);
    for (u32 c = 0; c < 2; ++c) {
        h->w32(args_map + c * 32 + 0, 3);
        h->w32(args_map + c * 32 + 4, 1);
        h->w32(args_map + c * 32 + 8, 0);
        h->w32(args_map + c * 32 + 12, 0);
    }
    const GuestAddr count = make_buffer(*h, dev, 256);
    h->w32(map_buffer(*h, count) + 16, 1);

    const auto& ql = layout::D3D12_COMMAND_QUEUE_DESC[x64];
    for (u32 i = 0; i < ql.size; i += 4) h->w32(kDesc + i, 0);
    h->w32(kDesc + ql.Type, D3D12_COMMAND_LIST_TYPE_DIRECT);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateCommandQueue,
                     {{kDesc}, {h->iid(IID_ID3D12COMMANDQUEUE)}, {kOut}}),
              S_OK);
    const GuestAddr queue = h->read_ptr(kOut);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateCommandAllocator,
                     {{D3D12_COMMAND_LIST_TYPE_DIRECT},
                      {h->iid(IID_ID3D12COMMANDALLOCATOR)},
                      {kOut}}),
              S_OK);
    const GuestAddr alloc = h->read_ptr(kOut);
    ASSERT_EQ(h->com(dev, DeviceSlot::CreateCommandList,
                     {{0},
                      {D3D12_COMMAND_LIST_TYPE_DIRECT},
                      {alloc},
                      {0},
                      {h->iid(IID_ID3D12GRAPHICSCOMMANDLIST)},
                      {kOut}}),
              S_OK);
    const GuestAddr list = h->read_ptr(kOut);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::SetPipelineState, {{pso}}), S_OK);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::SetGraphicsRootSignature, {{root}}),
              S_OK);
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::IASetPrimitiveTopology,
                     {{D3D_PRIMITIVE_TOPOLOGY_POINTLIST}}),
              S_OK);
    const GuestAddr data = make_buffer(*h, dev, 256);
    const GuestAddr filled = make_buffer(*h, dev, 256);
    set_one_target(*h, list, gpu_va(*h, data), 256, gpu_va(*h, filled));
    EXPECT_EQ(h->com(list, GraphicsCommandListSlot::ExecuteIndirect,
                     {{signature},
                      {2},
                      {args},
                      Harness::wide(0),
                      {count},
                      Harness::wide(16)}),
              S_OK);
    ASSERT_EQ(h->com(list, GraphicsCommandListSlot::Close, {}), S_OK);
    h->write_ptr(kArray, list);
    const u64 waits_before = d3d12::g_replay_cpu_waits.load();
    EXPECT_EQ(h->com(queue, CommandQueueSlot::ExecuteCommandLists, {{1}, {kArray}}), S_OK);
    EXPECT_EQ(d3d12::g_replay_cpu_waits.load(), waits_before)
        << "the indirect geometry draw read GPU data back on the CPU";
    const GuestAddr data_map = map_buffer(*h, data);
    const GuestAddr fill_map = map_buffer(*h, filled);
    EXPECT_EQ(h->r32(fill_map), 48u);
    for (u32 vertex = 0; vertex < 3; ++vertex) {
        EXPECT_EQ(h->r32(data_map + vertex * 16u), vertex);
    }
}

}  // namespace
}  // namespace vitreo::winapi
