// SPDX-License-Identifier: LGPL-2.1-only
//
// Live-run diagnostics: the /tmp/vitreo-draw-dump marker.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "../../oracle_data/d3d11_oracle/d3d11_shaders.h"
#include "draw_dump_shaders.h"

#include "d3d11_test_live_diag.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

// /tmp/vitreo-draw-dump: the marker's contents.
TEST(D3D11DrawDumpConfig, ParsesTheMarkersTokens) {
    const auto config =
        d3d11::parse_draw_dump_config("ps_semantic=WAVE_PARAM_INDEX max=4");
    EXPECT_EQ(config.ps_semantic, "WAVE_PARAM_INDEX");
    EXPECT_EQ(config.max_draws, 4u);
    EXPECT_TRUE(config.ignored.empty());

    // An empty marker: every draw, the default budget.
    const auto any = d3d11::parse_draw_dump_config("");
    EXPECT_TRUE(any.ps_semantic.empty());
    EXPECT_EQ(any.max_draws, d3d11::kDefaultDrawDumpMax);

    // Newlines and commas separate tokens; keys are case-insensitive.
    const auto lines =
        d3d11::parse_draw_dump_config("PS_SEMANTIC=TEXCOORD3\nmax=1,\n");
    EXPECT_EQ(lines.ps_semantic, "TEXCOORD3");
    EXPECT_EQ(lines.max_draws, 1u);
    EXPECT_EQ(d3d11::parse_draw_dump_config("max=0").max_draws, 0u);

    // What is not understood is reported, not guessed.
    const auto bad = d3d11::parse_draw_dump_config(
        "max=lots vs_semantic=X bogus max=99999999999 ps_semantic=ok");
    EXPECT_EQ(bad.ps_semantic, "ok");
    EXPECT_EQ(bad.max_draws, d3d11::kDefaultDrawDumpMax);
    EXPECT_EQ(bad.ignored, "max=lots vs_semantic=X bogus max=99999999999");
}

TEST(D3D11DrawDumpConfig, SemanticMatchesTheInputSignatureByNameOrNameAndIndex) {
    using graphics::DxbcSignatureElement;
    const std::vector<DxbcSignatureElement> signature = {
        DxbcSignatureElement{"SV_Position", 0, 0, 0xF, 3, 1, 0},
        DxbcSignatureElement{"TEXCOORD", 3, 1, 0x3, 3, 0, 0},
        DxbcSignatureElement{"WAVE_PARAM_INDEX", 0, 2, 0x1, 3, 0, 0},
    };
    EXPECT_TRUE(d3d11::signature_names_semantic(signature, "WAVE_PARAM_INDEX"));
    EXPECT_TRUE(d3d11::signature_names_semantic(signature, "wave_param_index"));
    EXPECT_TRUE(d3d11::signature_names_semantic(signature, "WAVE_PARAM_INDEX0"));
    EXPECT_TRUE(d3d11::signature_names_semantic(signature, "TEXCOORD"));
    EXPECT_TRUE(d3d11::signature_names_semantic(signature, "TEXCOORD3"));
    EXPECT_FALSE(d3d11::signature_names_semantic(signature, "TEXCOORD0"));
    EXPECT_FALSE(d3d11::signature_names_semantic(signature, "WAVE_PARAM"));
    EXPECT_FALSE(d3d11::signature_names_semantic(signature, "NORMAL"));
    EXPECT_FALSE(d3d11::signature_names_semantic(signature, ""));
    EXPECT_FALSE(d3d11::signature_names_semantic({}, "TEXCOORD"));
}

// The semantic filter selects the right draws and only up to `max`: a pixel
// shader that reads a WAVE_PARAM_INDEX varying is dumped, one that does not is
// not, and the budget is spent on the matching draws alone.
TEST_F(D3D11LiveDiagTest, DrawDumpSelectsByPixelShaderInputSemanticUpToMax) {
    const std::string prefix = pid_prefix("draw-dump");
    // Shaders: the VS passes WAVE_PARAM_INDEX on; one PS reads it, one does not.
    const auto make_shader = [&](usize slot, const unsigned char* blob,
                                 usize size) {
        EXPECT_TRUE(memory.write_bytes(BLOB, blob, size).is_ok());
        EXPECT_TRUE(memory.write_u64(A6_OUT, 0).is_ok());
        EXPECT_EQ(device_call(slot, {BLOB, size, 0, A6_OUT}), d3d11::S_OK);
        return memory.read_u64(A6_OUT).value();
    };
    const GuestAddr vs = make_shader(12, dd_vs_wave, sizeof(dd_vs_wave));
    const GuestAddr ps_wave = make_shader(15, dd_ps_wave, sizeof(dd_ps_wave));
    const GuestAddr ps_plain =
        make_shader(15, dd_ps_plain, sizeof(dd_ps_plain));
    ASSERT_TRUE(vs != 0 && ps_wave != 0 && ps_plain != 0);

    // A 4x4 render target.
    const GuestAddr target =
        tex2d({4, 4, 1, 1, 28, 1, 0, 0, 0x20 /* RTV */, 0, 0});
    const GuestAddr rtv = make_view(9, target, {});
    ASSERT_TRUE(memory.write_u64(A6_ARG, rtv).is_ok());
    context_call(33, {1, A6_ARG, 0});  // OMSetRenderTargets
    context_call(24, {4});             // TRIANGLELIST
    write_words(A6_VIEW, {0, 0, 0x40800000u, 0x40800000u, 0, 0x3F800000u});
    context_call(44, {1, A6_VIEW});    // RSSetViewports(0,0,4,4,0,1)
    context_call(11, {vs, 0, 0});

    const auto draw_with = [&](GuestAddr ps, int times) {
        context_call(9, {ps, 0, 0});
        for (int i = 0; i < times; ++i) {
            const unsigned before = backend.state.draws;
            context_call(13, {3, 0});  // Draw(3, 0)
            EXPECT_EQ(backend.state.draws, before + 1)
                << "the scene must really draw";
        }
    };

    live_diag::draw_dump_marker().set_path(
        dir.write("dump", "ps_semantic=WAVE_PARAM_INDEX max=2"));
    draw_with(ps_plain, 2);  // no WAVE_PARAM_INDEX: not dumped
    EXPECT_TRUE(log.matching("[draw-dump]").empty());
    draw_with(ps_wave, 3);   // three match, the budget is two
    const auto calls = log.matching(" call indexed=");
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0],
              prefix + "draw=1 call indexed=0 count=3 start=0 base=0 "
                       "instances=1 first_instance=0 topology=4 "
                       "ps_semantic=WAVE_PARAM_INDEX max=2");
    EXPECT_NE(calls[1].find("draw=2 call"), std::string::npos);
    // Each dump is complete and names the shader that matched.
    for (const char* what :
         {" shaders vs=0x", " ps_inputs ", " viewports count=1 v0=0,0,4,4,0,1",
          " scissors count=0", " rasterizer ", " blend ", " depth_stencil ",
          " input_layout none", " rtv0 view=0x", " end"}) {
        EXPECT_EQ(log.matching(what).size(), 2u) << what;
    }
    const auto inputs = log.matching(" ps_inputs ");
    EXPECT_NE(inputs[0].find("WAVE_PARAM_INDEX0@v1/mask=0x1"),
              std::string::npos) << inputs[0];
    EXPECT_NE(inputs[0].find("SV_Position0@v0"), std::string::npos);
    EXPECT_TRUE(log.matching("draw=3 ").empty()) << "the budget is two";

    // A draw after the budget is spent, with the plain shader too: nothing.
    const size_t lines = log.lines.size();
    draw_with(ps_plain, 1);
    draw_with(ps_wave, 1);
    EXPECT_EQ(log.lines.size(), lines);

    // Edit the marker: a new budget, and a plain shader is now the one wanted
    // by a name only it lacks nothing of -- SV_Position is in both.
    live_diag::draw_dump_marker().set_path(
        dir.write("dump2", "ps_semantic=sv_position max=3"));
    log.lines.clear();
    draw_with(ps_plain, 2);
    draw_with(ps_wave, 2);
    EXPECT_EQ(log.matching(" call indexed=").size(), 3u)
        << "both shaders take SV_Position; max=3 of the four draws";
    EXPECT_TRUE(log.matching("[draw-dump]").size() > 3u);

    // No marker, no lines.
    live_diag::draw_dump_marker().set_path(dir.missing("absent"));
    log.lines.clear();
    draw_with(ps_wave, 1);
    EXPECT_TRUE(log.lines.empty());
}

// The dump carries the bytes and slices of every bound constant buffer, the
// shader-resource view list, and the state objects.
TEST_F(D3D11LiveDiagTest, DrawDumpCarriesConstantBufferSlicesViewsAndState) {
    const auto make_shader = [&](usize slot, const unsigned char* blob,
                                 usize size) {
        EXPECT_TRUE(memory.write_bytes(BLOB, blob, size).is_ok());
        EXPECT_TRUE(memory.write_u64(A6_OUT, 0).is_ok());
        EXPECT_EQ(device_call(slot, {BLOB, size, 0, A6_OUT}), d3d11::S_OK);
        return memory.read_u64(A6_OUT).value();
    };
    const GuestAddr vs = make_shader(12, dd_vs_wave, sizeof(dd_vs_wave));
    const GuestAddr ps = make_shader(15, dd_ps_wave, sizeof(dd_ps_wave));
    const GuestAddr target =
        tex2d({4, 4, 1, 1, 28, 1, 0, 0, 0x20, 0, 0});
    const GuestAddr rtv = make_view(9, target, {});
    ASSERT_TRUE(memory.write_u64(A6_ARG, rtv).is_ok());
    context_call(33, {1, A6_ARG, 0});
    context_call(24, {4});
    write_words(A6_VIEW, {0, 0, 0x40800000u, 0x40800000u, 0, 0x3F800000u});
    context_call(44, {1, A6_VIEW});
    context_call(11, {vs, 0, 0});
    context_call(9, {ps, 0, 0});

    // A 512-byte constant buffer of 0,1,2,.. bytes (31 constants of 16 bytes,
    // 32 in all): the pixel stage binds the window of constants 16..31 of it
    // (PSSetConstantBuffers1: FirstConstant 16, NumConstants 16 -- both
    // multiples of 16), the vertex stage the whole of a second buffer.
    std::vector<u8> pattern(512);
    for (usize i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<u8>(i * 7 + 3);
    }
    const GuestAddr big =
        buffer({512, 0, 0x4 /* CB */, 0, 0, 0}, init_data(pattern, 0));
    std::vector<u8> small_pattern(64);
    for (usize i = 0; i < small_pattern.size(); ++i) {
        small_pattern[i] = static_cast<u8>(0xF0 - i);
    }
    const GuestAddr small =
        buffer({64, 0, 0x4, 0, 0, 0}, init_data(small_pattern, 0));
    ASSERT_NE(big, 0u);
    ASSERT_NE(small, 0u);
    constexpr GuestAddr PTRS_AT = A6 + 0x1C00;
    constexpr GuestAddr WINDOW_AT = A6 + 0x1D00;
    ASSERT_TRUE(memory.write_u64(PTRS_AT, big).is_ok());
    write_words(WINDOW_AT, {16, 16});
    // PSSetConstantBuffers1(StartSlot 1, 1, buffers, first, count)
    context_call(context_slot("PSSetConstantBuffers1"),
                 {1, 1, PTRS_AT, WINDOW_AT, WINDOW_AT + 4});
    ASSERT_TRUE(memory.write_u64(PTRS_AT + 8, small).is_ok());
    context_call(context_slot("VSSetConstantBuffers"), {0, 1, PTRS_AT + 8});

    // Two shader-resource views: a texture at t2 of the pixel stage, a typed
    // buffer window at t5 of the vertex stage.
    const GuestAddr texture =
        tex2d({8, 8, 4, 1, 28, 1, 0, 0, 0x8 /* SRV */, 0, 0});
    // D3D11_SHADER_RESOURCE_VIEW_DESC: R8G8B8A8_UNORM, TEXTURE2D (4),
    // MostDetailedMip 1, MipLevels 2.
    const GuestAddr texture_srv = make_view(7, texture, {28, 4, 1, 2});
    const GuestAddr srv_buffer = buffer({256, 0, 0x8, 0, 0, 0});
    // BUFFER (1): FirstElement 4, NumElements 16 of R32_UINT (42).
    const GuestAddr buffer_srv = make_view(7, srv_buffer, {42, 1, 4, 16});
    ASSERT_NE(texture_srv, 0u);
    ASSERT_NE(buffer_srv, 0u);
    ASSERT_TRUE(memory.write_u64(PTRS_AT + 16, texture_srv).is_ok());
    context_call(8, {2, 1, PTRS_AT + 16});  // PSSetShaderResources(2, 1)
    ASSERT_TRUE(memory.write_u64(PTRS_AT + 24, buffer_srv).is_ok());
    context_call(25, {5, 1, PTRS_AT + 24});  // VSSetShaderResources(5, 1)

    // The stencil reference, and a scissor.
    write_words(A6_VIEW, {1, 1, 3, 3});
    context_call(45, {1, A6_VIEW});  // RSSetScissorRects

    live_diag::draw_dump_marker().set_path(dir.write("dump", "max=1"));
    context_call(13, {3, 0});

    const auto only = [&](std::string_view what) {
        const auto lines = log.matching(what);
        EXPECT_EQ(lines.size(), 1u) << what;
        return lines.empty() ? std::string() : lines[0];
    };
    // The pixel window: constants 16..31 = bytes 256..511 of the buffer.
    {
        const std::string line = only(" ps_cb slot=1 ");
        EXPECT_NE(line.find("first_constant=16 num_constants=16 "
                            "buffer_bytes=512 window=256..512 "
                            "shadow_bytes=256 hex="),
                  std::string::npos) << line;
        std::string expected;
        for (usize i = 256; i < 512; ++i) {
            if (i != 256 && i % 4 == 0) expected += ' ';
            char byte[4];
            std::snprintf(byte, sizeof(byte), "%02x", pattern[i]);
            expected += byte;
        }
        EXPECT_EQ(line.substr(line.find("hex=") + 4), expected);
    }
    // The vertex buffer: the whole of it (a plain VSSetConstantBuffers).
    {
        const std::string line = only(" vs_cb slot=0 ");
        EXPECT_NE(line.find("first_constant=0 num_constants=4096 "
                            "buffer_bytes=64 window=0..64 shadow_bytes=64 "
                            "hex="),
                  std::string::npos) << line;
        std::string expected;
        for (usize i = 0; i < 64; ++i) {
            if (i != 0 && i % 4 == 0) expected += ' ';
            char byte[4];
            std::snprintf(byte, sizeof(byte), "%02x", small_pattern[i]);
            expected += byte;
        }
        EXPECT_EQ(line.substr(line.find("hex=") + 4), expected);
    }
    EXPECT_TRUE(log.matching(" ps_cb slot=0 ").empty())
        << "only bound slots are dumped";
    {
        const std::string line = only(" ps_srv slot=2 ");
        EXPECT_NE(line.find("kind=texture"), std::string::npos) << line;
        EXPECT_NE(line.find("format=28 dimension=4 mips=1+2 slices=0+"),
                  std::string::npos) << line;
        EXPECT_NE(line.find("resource_size=8x8 mips=4"), std::string::npos)
            << line;
    }
    {
        const std::string line = only(" vs_srv slot=5 ");
        EXPECT_NE(line.find("kind=buffer"), std::string::npos) << line;
        EXPECT_NE(line.find("format=42"), std::string::npos) << line;
        EXPECT_NE(line.find("first_element=4 num_elements=16 element_bytes=4"),
                  std::string::npos) << line;
    }
    EXPECT_NE(only(" scissors count=1").find("s0=1,1,3,3"), std::string::npos);
}

// The indirect-argument block has to fit the buffer, and the offset has to be
// DWORD aligned: Windows drops the call otherwise. Every row of the WARP
// measurement (tools/oracle_probe/d3d11_indirect_args_bounds_probe.expected)
// is replayed: a call WARP ran reaches the backend, one it dropped does not.
TEST_F(D3D11LiveDiagTest, IndirectArgumentBlocksFollowTheMeasuredBounds) {
    const GuestAddr vs = [&] {
        EXPECT_TRUE(memory.write_bytes(BLOB, dd_vs_wave,
                                       sizeof(dd_vs_wave)).is_ok());
        EXPECT_TRUE(memory.write_u64(A6_OUT, 0).is_ok());
        EXPECT_EQ(device_call(12, {BLOB, sizeof(dd_vs_wave), 0, A6_OUT}),
                  d3d11::S_OK);
        return memory.read_u64(A6_OUT).value();
    }();
    ASSERT_NE(vs, 0u);
    context_call(11, {vs, 0, 0});
    context_call(24, {4});  // TRIANGLELIST
    // 16 R16_UINT indices for the indexed rows.
    const GuestAddr indices = buffer({32, 0, 0x2 /* IB */, 0, 0, 0});
    context_call(19, {indices, 57 /* R16_UINT */, 0});  // IASetIndexBuffer
    const GuestAddr cs = compute_shader(cs_lifetime, sizeof(cs_lifetime));
    ASSERT_NE(cs, 0u);
    context_call(69, {cs, 0, 0});  // CSSetShader

    std::ifstream in(std::string(VITREO_ORACLE_PROBE_DIR) +
                     "/d3d11_indirect_args_bounds_probe.expected");
    ASSERT_TRUE(in.is_open());
    std::string row;
    usize rows = 0;
    usize ran_rows = 0;
    while (std::getline(in, row)) {
        const auto width_at = row.find("ByteWidth=");
        const auto offset_at = row.find("offset=");
        if (width_at == std::string::npos || offset_at == std::string::npos) {
            continue;
        }
        std::string call;
        std::istringstream(row) >> call;
        const u32 width = static_cast<u32>(
            std::stoul(row.substr(width_at + 10)));
        const u32 offset = static_cast<u32>(
            std::stoul(row.substr(offset_at + 7)));
        const auto invocations_at = row.find("Invocations=");
        ASSERT_NE(invocations_at, std::string::npos) << row;
        const bool ran = std::stoull(row.substr(invocations_at + 12)) != 0;
        // CSInvocations / VSInvocations: the value after the first one.
        const GuestAddr args = make_indirect_args(
            std::vector<u32>(width / 4, 0));
        ASSERT_NE(args, 0u) << row;
        const unsigned draws = backend.state.indirect_draws;
        const unsigned dispatches = backend.state.dispatches;
        if (call == "DrawInstancedIndirect") {
            context_call(40, {args, offset});
        } else if (call == "DrawIndexedInstancedIndirect") {
            context_call(39, {args, offset});
        } else {
            ASSERT_EQ(call, "DispatchIndirect") << row;
            context_call(42, {args, offset});
        }
        const bool reached = call == "DispatchIndirect"
                                 ? backend.state.dispatches == dispatches + 1
                                 : backend.state.indirect_draws == draws + 1;
        EXPECT_EQ(reached, ran) << "WARP " << (ran ? "ran" : "dropped")
                                << ": " << row;
        ++rows;
        ran_rows += ran ? 1 : 0;
    }
    EXPECT_EQ(rows, 26u) << "every measured row";
    EXPECT_EQ(ran_rows, 8u) << "the in-bounds, aligned rows";
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
