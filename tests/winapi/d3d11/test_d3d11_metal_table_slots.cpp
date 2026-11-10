// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d11_metal_table_slots.cpp
/// @brief Texture views of a stage that binds through a descriptor table,
///        rebound one slot at a time and across ClearState.
///
/// Learn (ID3D11DeviceContext::PSSetShaderResources): the call replaces the
/// views of [StartSlot, StartSlot + NumViews) and no others. Learn
/// (ID3D11DeviceContext::ClearState): every slot returns to its default,
/// which for a shader resource is no view -- and a read of no view returns 0
/// (Learn, ld (sm4 - asm)). The fixture is d3d11_test_metal_typed_buffer.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_metal_typed_buffer.hpp"
#include "../../graphics/table_texture_slots_shaders.h"
#include "../../graphics/typed_buffer_offset_shaders.h"

#include <array>

namespace vitreo::winapi::d3d11_test {
namespace {

class D3D11TableTextureSlotsTest : public D3D11TypedBufferOffsetTest {
protected:
    /// A 1x1 R32_FLOAT texture holding @p value, and its whole-resource view.
    GuestAddr texel_view(float value) {
        const std::array<u32, 11> td{1, 1, 1, 1, kR32Float, 1, 0,
                                     D3D11_USAGE_DEFAULT,
                                     D3D11_BIND_SHADER_RESOURCE, 0, 0};
        // D3D11_SUBRESOURCE_DATA {pSysMem, SysMemPitch, SysMemSlicePitch}.
        const GuestAddr init =
            put(std::array<u64, 2>{put(&value, sizeof(value)), 4});
        GuestAddr out = out_slot();
        EXPECT_EQ(device_call(5, {put(td), init, out}), d3d11::S_OK);
        const GuestAddr texture = memory.read_u64(out).value();
        out = out_slot();
        EXPECT_EQ(device_call(7, {texture, 0, out}), d3d11::S_OK);
        return memory.read_u64(out).value();
    }
    /// PSSetShaderResources(slot, 1, {view}).
    void bind(u32 slot, GuestAddr view) {
        const std::array<u64, 1> list{view};
        context_call(8, {slot, 1, put(list)});
    }
    void bind_two_texture_reader() {
        const GuestAddr ps =
            shader(15, tts_ps_two_textures, sizeof(tts_ps_two_textures));
        ASSERT_NE(ps, 0u);
        bind_target(ps);
    }
    void expect_span(const std::vector<std::array<float, 4>>& texels, u32 x0,
                     u32 width, float near_value, float far_value,
                     const char* what) {
        for (u32 y = 0; y < kHeight; ++y) {
            for (u32 x = x0; x < x0 + width; ++x) {
                const auto& t = texels[y * kWidth + x];
                EXPECT_EQ(t[0], near_value) << what << " t0 at " << x;
                EXPECT_EQ(t[1], far_value) << what << " t9 at " << x;
            }
        }
    }
};

TEST_F(D3D11TableTextureSlotsTest, RebindingOneSlotLeavesTheOthers) {
    const GuestAddr a = texel_view(1.0f);
    const GuestAddr b = texel_view(10.0f);
    const GuestAddr c = texel_view(2.0f);
    const GuestAddr d = texel_view(20.0f);
    bind_two_texture_reader();

    bind(0, a);
    bind(9, b);
    columns(0, 4);
    draw();
    bind(0, c);  // t9 untouched
    columns(4, 4);
    draw();
    bind(9, d);  // t0 untouched
    columns(8, 4);
    draw();
    bind(0, a);  // back to a view the table held before
    columns(12, 4);
    draw();

    const auto texels = read_target();
    expect_span(texels, 0, 4, 1.0f, 10.0f, "a, b");
    expect_span(texels, 4, 4, 2.0f, 10.0f, "c, b");
    expect_span(texels, 8, 4, 2.0f, 20.0f, "c, d");
    expect_span(texels, 12, 4, 1.0f, 20.0f, "a, d");
}

TEST_F(D3D11TableTextureSlotsTest, ClearStateUnbindsEverySlot) {
    const GuestAddr a = texel_view(1.0f);
    const GuestAddr b = texel_view(10.0f);
    const GuestAddr d = texel_view(20.0f);
    bind_two_texture_reader();
    bind(0, a);
    bind(9, b);
    columns(0, 16);
    draw();
    expect_span(read_target(), 0, 16, 1.0f, 10.0f, "before ClearState");

    context_call(110, {});  // ClearState
    bind_two_texture_reader();
    bind(9, d);  // t0 stays at its default: no view
    columns(0, 16);
    draw();
    expect_span(read_target(), 0, 16, 0.0f, 20.0f, "after ClearState");
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
