// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file d3d11_test_metal_typed_buffer.hpp
/// @brief The fixture D3D11TypedBufferOffsetTest: typed buffer views at
///        element offsets Metal cannot alias directly.
///
/// Split from the former test_d3d11_metal.cpp; shared by
/// test_d3d11_metal_typed_buffer_*.cpp.

#include "d3d11_test_metal_fixture.hpp"

#include "graphics/contracts/delta_record.hpp"
#include "graphics/contracts/slim_record.hpp"
#include "../../graphics/typed_buffer_offset_shaders.h"

#include <array>
#include <initializer_list>
#include <vector>

namespace vitreo::winapi::d3d11_test {

// ---------------------------------------------------------------------------
// Typed buffer views at element offsets Metal cannot alias directly.
//
// Learn (D3D11_BUFFER_SRV / D3D11_BUFFER_UAV): FirstElement is any element;
// the element size comes from the view format. UE's landscape reads its
// per-section LOD arrays as R32_FLOAT Buffer<float> views at FirstElement
// 270k (byte 1080k: 8 bytes past a 16-byte boundary). D3D11.3 functional
// spec: every command on the immediate context observes every earlier write
// in submission order -- Map (DISCARD / NO_OVERWRITE), UpdateSubresource(1),
// CopySubresourceRegion, a compute UAV write -- including two draws one
// pass apart. Learn (ld): an address past the view returns 0 in every
// component the format has and the default (0, alpha 1) in the others;
// bufinfo returns NumElements. Shaders: typed_buffer_offset_shaders.h (fxc).
//
// Run as production records draws: delta-record and pass-fold pinned on.
class D3D11TypedBufferOffsetTest : public D3D11MetalTest {
protected:
    void SetUp() override {
        graphics::delta_record::force_all_for_test(
            graphics::delta_record::Force::On);
        graphics::slim_record::force_all_for_test(
            graphics::slim_record::Force::On);
        D3D11MetalTest::SetUp();
    }
    void TearDown() override {
        graphics::delta_record::force_all_for_test(
            graphics::delta_record::Force::None);
        graphics::slim_record::force_all_for_test(
            graphics::slim_record::Force::None);
    }

    static constexpr u32 kRgba32Float = 2;   // DXGI_FORMAT_R32G32B32A32_FLOAT
    static constexpr u32 kRgba8Unorm = 28;   // DXGI_FORMAT_R8G8B8A8_UNORM
    static constexpr u32 kR32Typeless = 39;  // DXGI_FORMAT_R32_TYPELESS
    static constexpr u32 kR32Float = 41;     // DXGI_FORMAT_R32_FLOAT
    static constexpr u32 kR32Uint = 42;      // DXGI_FORMAT_R32_UINT
    static constexpr u32 kR16Float = 54;     // DXGI_FORMAT_R16_FLOAT
    /// The landscape arrays: four sub-arrays of 270 R32_FLOAT elements.
    static constexpr u32 kSection = 270;
    static constexpr u32 kElements = 4 * kSection + 8;
    /// The render target: 16 x 2 R32G32B32A32_FLOAT.
    static constexpr u32 kWidth = 16;
    static constexpr u32 kHeight = 2;

    /// ID3D11Device::CreateBuffer (D3D11_BUFFER_DESC, optional pSysMem).
    GuestAddr buffer(u32 bytes, u32 usage, u32 bind, u32 cpu, u32 misc,
                     const void* init) {
        const std::array<u32, 6> bd{bytes, usage, bind, cpu, misc, 0};
        GuestAddr data = 0;
        if (init != nullptr) {
            data = put(std::array<u64, 2>{put(init, bytes), 0});
        }
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(3, {put(bd), data, out}), d3d11::S_OK);
        return memory.read_u64(out).value();
    }
    /// D3D11_SRV_DIMENSION_BUFFER view {FirstElement, NumElements}.
    GuestAddr srv(GuestAddr resource, u32 format, u32 first, u32 count) {
        const std::array<u32, 6> desc{format, D3D11_SRV_DIMENSION_BUFFER,
                                      first, count, 0, 0};
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(7, {resource, put(desc), out}), d3d11::S_OK)
            << "fmt " << format << " first " << first;
        return memory.read_u64(out).value();
    }
    /// D3D11_UAV_DIMENSION_BUFFER view {FirstElement, NumElements, Flags}.
    GuestAddr uav(GuestAddr resource, u32 format, u32 first, u32 count,
                  u32 flags = 0) {
        const std::array<u32, 5> desc{format, D3D11_UAV_DIMENSION_BUFFER,
                                      first, count, flags};
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(8, {resource, put(desc), out}), d3d11::S_OK)
            << "fmt " << format << " first " << first;
        return memory.read_u64(out).value();
    }
    /// Map(@p type) a buffer and write @p bytes at @p offset into it.
    void map_write(GuestAddr resource, u32 type, u32 offset, const void* bytes,
                   usize size) {
        const GuestAddr mapped = put(std::array<u64, 2>{0, 0});
        ASSERT_EQ(context_call(14, {resource, 0, type, 0, mapped}),
                  d3d11::S_OK);
        const GuestAddr data = memory.read_u64(mapped).value();
        ASSERT_NE(data, 0u);
        ASSERT_TRUE(memory.write_bytes(data + offset,
                                       static_cast<const Byte*>(bytes), size)
                        .is_ok());
        context_call(15, {resource, 0});  // Unmap
    }
    /// Copy @p resource (a buffer of @p bytes) to a STAGING twin and read it.
    std::vector<u32> read_buffer(GuestAddr resource, u32 bytes) {
        const GuestAddr staging = buffer(bytes, D3D11_USAGE_STAGING, 0,
                                         D3D11_CPU_ACCESS_READ, 0, nullptr);
        context_call(47, {staging, resource});  // CopyResource
        const GuestAddr mapped = put(std::array<u64, 2>{0, 0});
        EXPECT_EQ(context_call(14, {staging, 0, D3D11_MAP_READ, 0, mapped}),
                  d3d11::S_OK);
        std::vector<u32> words(bytes / 4, 0xEEEEEEEEu);
        const GuestAddr data = memory.read_u64(mapped).value();
        EXPECT_TRUE(memory.read_bytes(data, reinterpret_cast<Byte*>(
                                                words.data()),
                                      bytes).is_ok());
        context_call(15, {staging, 0});
        return words;
    }

    /// The kWidth x kHeight R32G32B32A32_FLOAT target, cleared to -1, its
    /// RTV bound with @p vertex_shader (VSFull when 0) and @p ps, no input
    /// layout, @p topology, no culling.
    void bind_target(GuestAddr ps, GuestAddr vertex_shader = 0,
                     u32 topology = 4 /* TRIANGLELIST */) {
        const std::array<u32, 11> td{kWidth, kHeight, 1, 1, kRgba32Float, 1, 0,
                                     D3D11_USAGE_DEFAULT,
                                     D3D11_BIND_RENDER_TARGET, 0, 0};
        GuestAddr out = out_slot();
        ASSERT_EQ(device_call(5, {put(td), 0, out}), d3d11::S_OK);
        target = memory.read_u64(out).value();
        out = out_slot();
        ASSERT_EQ(device_call(9, {target, 0, out}), d3d11::S_OK);
        const GuestAddr rtv = memory.read_u64(out).value();
        const std::array<float, 4> sentinel{-1, -1, -1, -1};
        context_call(50, {rtv, put(sentinel)});  // ClearRenderTargetView
        vs = vertex_shader != 0
                 ? vertex_shader
                 : shader(12, tbo_vs_full, sizeof(tbo_vs_full));
        ASSERT_NE(vs, 0u);
        ASSERT_NE(ps, 0u);
        // D3D11_RASTERIZER_DESC: SOLID, CULL_NONE, depth clip on.
        const std::array<u32, 10> rs{3, 1, 0, 0, 0, 0, 1, 0, 0, 0};
        out = out_slot();
        ASSERT_EQ(device_call(22, {put(rs), out}), d3d11::S_OK);
        context_call(43, {memory.read_u64(out).value()});  // RSSetState
        context_call(11, {vs, 0, 0});   // VSSetShader
        context_call(9, {ps, 0, 0});    // PSSetShader
        context_call(17, {0});          // IASetInputLayout(NULL)
        context_call(24, {topology});   // IASetPrimitiveTopology
        const std::array<u64, 1> rtvs{rtv};
        context_call(33, {1, put(rtvs), 0});  // OMSetRenderTargets
    }
    /// RSSetViewports: columns [x, x + width) of the target.
    void columns(float x, float width) {
        const std::array<float, 6> vp{x, 0, width, kHeight, 0, 1};
        context_call(44, {1, put(vp)});
    }
    void ps_views(std::initializer_list<GuestAddr> views) {
        std::vector<u64> list(views);
        context_call(8, {0, list.size(), put(list.data(), list.size() * 8)});
    }
    void draw() { context_call(13, {3, 0}); }  // Draw(3, 0)
    /// The target's texels, row-major, four floats each.
    std::vector<std::array<float, 4>> read_target() {
        const std::array<u32, 11> td{kWidth, kHeight, 1, 1, kRgba32Float, 1, 0,
                                     D3D11_USAGE_STAGING, 0,
                                     D3D11_CPU_ACCESS_READ, 0};
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(5, {put(td), 0, out}), d3d11::S_OK);
        const GuestAddr staging = memory.read_u64(out).value();
        context_call(47, {staging, target});
        const GuestAddr mapped = put(std::array<u64, 2>{0, 0});
        EXPECT_EQ(context_call(14, {staging, 0, D3D11_MAP_READ, 0, mapped}),
                  d3d11::S_OK);
        const GuestAddr data = memory.read_u64(mapped).value();
        const u32 pitch = memory.read_u32(mapped + 8).value();
        std::vector<std::array<float, 4>> texels(kWidth * kHeight);
        for (u32 y = 0; y < kHeight; ++y) {
            EXPECT_TRUE(memory.read_bytes(
                data + y * pitch,
                reinterpret_cast<Byte*>(texels[y * kWidth].data()),
                kWidth * 16).is_ok());
        }
        context_call(15, {staging, 0});
        return texels;
    }
    /// Columns [x0, x0 + 8) must hold (base + first + x % 8, 0, 0, 1): an
    /// R32_FLOAT view's element, its missing components 0 and alpha 1.
    void expect_columns(const std::vector<std::array<float, 4>>& texels,
                        u32 x0, float base, u32 first, const char* what) {
        for (u32 y = 0; y < kHeight; ++y) {
            for (u32 x = x0; x < x0 + 8; ++x) {
                const auto& t = texels[y * kWidth + x];
                const float want = base + static_cast<float>(first + x % 8);
                EXPECT_EQ(t[0], want) << what << " pixel " << x << "," << y;
                EXPECT_EQ(t[1], 0.0f) << what << " pixel " << x << "," << y;
                EXPECT_EQ(t[2], 0.0f) << what << " pixel " << x << "," << y;
                EXPECT_EQ(t[3], 1.0f) << what << " pixel " << x << "," << y;
            }
        }
    }
    /// kElements floats, element e = base + e.
    static std::vector<float> ramp(float base) {
        std::vector<float> values(kElements);
        for (u32 e = 0; e < kElements; ++e) {
            values[e] = base + static_cast<float>(e);
        }
        return values;
    }

    GuestAddr vs{0};
    GuestAddr target{0};
};

}  // namespace vitreo::winapi::d3d11_test
