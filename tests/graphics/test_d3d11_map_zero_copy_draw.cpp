// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d11_map_zero_copy_draw.cpp
/// @brief WS6: WRITE_DISCARD renames of DYNAMIC vertex, index and constant
///        buffers read through the render path (draws, not dispatches), and
///        a DISCARD past the ring's budget.
///
/// Learn (D3D11_MAP_WRITE_DISCARD): the GPU keeps reading the previous
/// contents for work already queued, so every draw of a frame must see the
/// bytes written by the DISCARD just before it, even when every draw is
/// queued before any runs. Each test runs twice: arm A with the ring off (the
/// shadow path, VITREO_WS6=0), arm B with it on.
///
/// The scene: an N x 1 R32_UINT target, draw i covers only column i (a quad
/// from FScreenVertex {float2 Position, float2 UV}: lut_vs_screen_vertex maps
/// Position [0,1]^2 to the viewport), and vtfb_ps_uint writes cb0[0].x.

#include <gtest/gtest.h>

#include "graphics/metal/metal_submit_queue.hpp"
#include "winapi/graphics/d3d11/d3d11_buffer_ring.hpp"

#include "../winapi/d3d11/d3d11_test_metal_vt_feedback.hpp"
#include "../winapi/d3d11/volume_lut_shaders.h"
#include "../winapi/d3d11/vt_feedback_shaders.h"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

namespace vitreo::winapi::d3d11_test {

class D3D11MapZeroCopyDrawTest : public VtFeedbackTest {
protected:
    static constexpr u32 kColumns = 8;
    static constexpr u32 kR32G32Float = 16;  // DXGI_FORMAT_R32G32_FLOAT

    void TearDown() override {
        if (hold) (void)hold->signal_cpu(~0ull >> 1);
        graphics::MetalSubmitQueue::instance().drain();
        d3d11_ws6_override_for_test(std::nullopt);
        VtFeedbackTest::TearDown();
    }

    void both_arms(const std::function<void(bool ring)>& body) {
        for (const bool ring : {false, true}) {
            SCOPED_TRACE(ring ? "arm B (zero-copy ring)" : "arm A (shadow)");
            d3d11_ws6_override_for_test(ring);
            body(ring);
            if (hold) {
                (void)hold->signal_cpu(~0ull >> 1);
                hold.reset();
            }
            graphics::MetalSubmitQueue::instance().drain();
        }
    }
    /// Everything recorded from here waits until the gate opens.
    void gate() {
        auto& backend = graphics::MetalDevice::instance();
        ASSERT_TRUE(backend.supports_fences());
        hold = backend.create_fence(0);
        ASSERT_TRUE(hold && hold->valid());
        ASSERT_TRUE(hold->wait_gpu(1));
    }
    void open_gate() {
        ASSERT_TRUE(hold);
        ASSERT_TRUE(hold->signal_cpu(1));
    }

    GuestAddr dynamic(u32 bytes, u32 bind) {
        return buffer(bytes, D3D11_USAGE_DYNAMIC, bind, D3D11_CPU_ACCESS_WRITE,
                      0, 0, nullptr);
    }
    /// Map(WRITE_DISCARD) @p resource, @p bytes at offset 0, Unmap.
    void discard_write(GuestAddr resource, const void* bytes, usize size) {
        const Mapped m = map(resource, 0, D3D11_MAP_WRITE_DISCARD);
        ASSERT_EQ(m.hr, d3d11::S_OK);
        ASSERT_NE(m.data, 0u);
        ASSERT_TRUE(memory.write_bytes(m.data, static_cast<const Byte*>(bytes),
                                       size).is_ok());
        unmap(resource, 0);
    }
    /// Column @p i's quad, strip order.
    static std::array<float, 16> quad(u32 i) {
        const float x0 = static_cast<float>(i) / kColumns;
        const float x1 = static_cast<float>(i + 1) / kColumns;
        return {x0, 0, 0, 0, x1, 0, 0, 0, x0, 1, 0, 0, x1, 1, 0, 0};
    }
    /// The target, its pipeline and the input layout; @p cb at PS b0.
    GuestAddr scene(GuestAddr cb) {
        const GuestAddr vs_object =
            shader(12, lut_vs_screen_vertex, sizeof(lut_vs_screen_vertex));
        const GuestAddr ps = shader(15, vtfb_ps_uint, sizeof(vtfb_ps_uint));
        EXPECT_TRUE(vs_object != 0 && ps != 0);
        const GuestAddr target =
            texture(kColumns, 1, 1, 1, kR32Uint, D3D11_USAGE_DEFAULT,
                    D3D11_BIND_RENDER_TARGET, 0);
        const GuestAddr rtv = render_target_view(target, kR32Uint, 0);
        const std::array<float, 4> zero{0, 0, 0, 0};
        context_call(50, {rtv, put(zero)});  // ClearRenderTargetView
        const std::array<u32, 10> rs{3, 1, 0, 0, 0, 0, 1, 0, 0, 0};
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(22, {put(rs), out}), d3d11::S_OK);
        context_call(43, {memory.read_u64(out).value()});  // RSSetState
        viewport(0, 0, kColumns, 1);
        context_call(11, {vs_object, 0, 0});  // VSSetShader
        context_call(9, {ps, 0, 0});          // PSSetShader
        context_call(17, {input_layout({{"ATTRIBUTE", 0, kR32G32Float, 0, 0, 0},
                                        {"ATTRIBUTE", 1, kR32G32Float, 8, 0, 0}},
                                       lut_vs_screen_vertex,
                                       sizeof(lut_vs_screen_vertex))});
        context_call(24, {5});  // IASetPrimitiveTopology TRIANGLESTRIP
        const std::array<u64, 1> rtvs{rtv};
        context_call(33, {1, put(rtvs), 0});  // OMSetRenderTargets
        const std::array<u64, 1> cbs{cb};
        context_call(16, {0, 1, put(cbs)});  // PSSetConstantBuffers
        return target;
    }
    void bind_vertex_buffer(GuestAddr vb) {
        const std::array<u64, 1> buffers{vb};
        const std::array<u32, 1> strides{16};
        const std::array<u32, 1> offsets{0};
        context_call(18, {0, 1, put(buffers), put(strides), put(offsets)});
    }
    void expect_columns(GuestAddr target, u32 base) {
        context_call(33, {0, 0, 0});  // unbind the target before the copy
        const auto texels =
            read_uint_level(target, kR32Uint, kColumns, 1, 1, 0);
        ASSERT_EQ(texels.size(), kColumns);
        for (u32 i = 0; i < kColumns; ++i) {
            EXPECT_EQ(texels[i], base + i)
                << "draw " << i << " read another DISCARD's bytes";
        }
    }

    std::shared_ptr<graphics::BackendFence> hold;
};

// A DYNAMIC vertex buffer and constant buffer, both DISCARDed before every
// draw of the frame and bound once: each draw reads its own vertices and
// constants although none runs until all are queued.
TEST_F(D3D11MapZeroCopyDrawTest, DiscardBeforeEachDrawRenamesVertexAndConstantBuffers) {
    both_arms([&](bool) {
        const GuestAddr vb = dynamic(64, D3D11_BIND_VERTEX_BUFFER);
        const GuestAddr cb = dynamic(16, D3D11_BIND_CONSTANT_BUFFER);
        const GuestAddr target = scene(cb);
        bind_vertex_buffer(vb);
        gate();
        for (u32 i = 0; i < kColumns; ++i) {
            const auto vertices = quad(i);
            discard_write(vb, vertices.data(), sizeof(vertices));
            const std::array<u32, 4> constants{1000 + i, 0, 0, 0};
            discard_write(cb, constants.data(), sizeof(constants));
            context_call(13, {4, 0});  // Draw(4, 0)
        }
        open_gate();
        expect_columns(target, 1000);
    });
}

// A DYNAMIC index buffer DISCARDed before every DrawIndexed picks each
// draw's quad out of one vertex buffer.
TEST_F(D3D11MapZeroCopyDrawTest, DiscardBeforeEachDrawIndexedRenamesTheIndexBuffer) {
    both_arms([&](bool) {
        const GuestAddr vb = dynamic(64 * kColumns, D3D11_BIND_VERTEX_BUFFER);
        const GuestAddr ib = dynamic(16, D3D11_BIND_INDEX_BUFFER);
        const GuestAddr cb = dynamic(16, D3D11_BIND_CONSTANT_BUFFER);
        const GuestAddr target = scene(cb);
        std::vector<float> all;
        for (u32 i = 0; i < kColumns; ++i) {
            const auto q = quad(i);
            all.insert(all.end(), q.begin(), q.end());
        }
        discard_write(vb, all.data(), all.size() * sizeof(float));
        bind_vertex_buffer(vb);
        context_call(19, {ib, kR16Uint, 0});  // IASetIndexBuffer R16_UINT
        gate();
        for (u32 i = 0; i < kColumns; ++i) {
            const std::array<u16, 8> indices{
                static_cast<u16>(4 * i), static_cast<u16>(4 * i + 1),
                static_cast<u16>(4 * i + 2), static_cast<u16>(4 * i + 3),
                0, 0, 0, 0};
            discard_write(ib, indices.data(), sizeof(indices));
            const std::array<u32, 4> constants{2000 + i, 0, 0, 0};
            discard_write(cb, constants.data(), sizeof(constants));
            context_call(12, {4, 0, 0});  // DrawIndexed(4, 0, 0)
        }
        open_gate();
        expect_columns(target, 2000);
    });
}

// Past the ring's budget, with every slice of the class leased by a live
// buffer: CreateBuffer still succeeds on the ring (Windows has no such
// limit), and a DISCARD of a buffer queued work still reads waits for that
// work and reuses its slice, never failing and never writing under it.
TEST_F(D3D11MapZeroCopyDrawTest, CreateAndDiscardPastTheBudgetWaitInsteadOfFailing) {
    d3d11_ws6_override_for_test(true);
    const GuestAddr first = dynamic(65536, D3D11_BIND_CONSTANT_BUFFER);
    auto ring = D3D11Ws6TestPeer::ring(d3d11);
    ASSERT_NE(ring, nullptr);
    ASSERT_NE(D3D11Ws6TestPeer::guest(d3d11, first), 0u);
    ring->set_budgets_for_test(0, 0);
    // 64 KiB slices: 16 per slab. Fill whole slabs, so none is free.
    std::vector<GuestAddr> cbs{first};
    const usize per_slab = D3D11BufferRing::kSlabBytes / 65536;
    while (cbs.size() % per_slab != 0 || cbs.size() < 3 * per_slab) {
        cbs.push_back(dynamic(65536, D3D11_BIND_CONSTANT_BUFFER));
        ASSERT_NE(cbs.back(), 0u) << "CreateBuffer past the budget failed";
        ASSERT_NE(D3D11Ws6TestPeer::guest(d3d11, cbs.back()), 0u)
            << "a live buffer was pushed off the ring by the budget";
    }
    const GuestAddr target = scene(cbs[0]);
    const GuestAddr vb = dynamic(64, D3D11_BIND_VERTEX_BUFFER);
    bind_vertex_buffer(vb);
    const auto q0 = quad(0);
    discard_write(vb, q0.data(), sizeof(q0));
    const std::array<u32, 4> c0{3000, 0, 0, 0};
    discard_write(cbs[0], c0.data(), sizeof(c0));
    gate();
    // Draw(4, 0): column 0 reads cbs[0]. No Flush: the Metal flush waits
    // for completion, which the closed gate would never give.
    context_call(13, {4, 0});
    const GuestAddr before = D3D11Ws6TestPeer::guest(d3d11, cbs[0]);
    std::atomic<bool> released{false};
    std::thread opener([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        released.store(true);
        (void)hold->signal_cpu(1);
    });
    const Mapped m = map(cbs[0], 0, D3D11_MAP_WRITE_DISCARD);
    const bool waited = released.load();
    opener.join();
    ASSERT_EQ(m.hr, d3d11::S_OK) << "a DISCARD past the budget must not fail";
    EXPECT_TRUE(waited) << "the slice was handed back before the draw ran";
    EXPECT_EQ(m.data, before) << "nothing free: the buffer's own slice";
    EXPECT_GE(ring->stats().in_place, 1u);
    // The next frame's draw sees the new constants.
    const std::array<u32, 4> c1{4000, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(m.data, reinterpret_cast<const Byte*>(c1.data()),
                                   sizeof(c1)).is_ok());
    unmap(cbs[0], 0);
    const auto q1 = quad(1);
    discard_write(vb, q1.data(), sizeof(q1));
    context_call(13, {4, 0});
    context_call(33, {0, 0, 0});
    const auto texels = read_uint_level(target, kR32Uint, kColumns, 1, 1, 0);
    ASSERT_EQ(texels.size(), kColumns);
    EXPECT_EQ(texels[0], 3000u) << "the queued draw saw the later DISCARD";
    EXPECT_EQ(texels[1], 4000u);
}

}  // namespace vitreo::winapi::d3d11_test
