// SPDX-License-Identifier: LGPL-2.1-only
//
// Feature level 11_1 and D3D11_FEATURE_DATA_D3D11_OPTIONS.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_refused_shaders.h"
namespace fl111 {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../../oracle_data/d3d11_fl111_shaders.h"
#pragma clang diagnostic pop
}  // namespace fl111
namespace doubles {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../../oracle_data/d3d11_doubles_shaders.h"
#pragma clang diagnostic pop
}  // namespace doubles

#include "d3d11_test_surface.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

// ---------------------------------------------------------------------------
// Feature level 11_1 and D3D11_FEATURE_DATA_D3D11_OPTIONS. Expected values
// are lines of tools/oracle_probe/d3d11_fl111_probe.expected (Windows,
// WARP, a device created with {11_0}) or of the a01 probe's per-level
// "feature 5" rows; the shaders are that probe's fxc output.
// ---------------------------------------------------------------------------

TEST_F(D3D11OptionalCapsTest, D3D11OptionsReportWhatThisDeviceImplements) {
    // Learn D3D11_FEATURE_DATA_D3D11_OPTIONS; WARP at FL 11_0 answers every
    // member TRUE (a01 "feature 5 size 56: ...=1,1,...,1", fl111 "caps
    // 11_0"). The backend decides the members that need the GPU.
    backend.state.framebuffer_fetch = true;
    backend.state.programmable_sample_positions = true;
    auto o = options();
    EXPECT_EQ(o[0], 1u) << "OutputMergerLogicOp: framebuffer fetch";
    EXPECT_EQ(o[1], 1u) << "UAVOnlyRenderingForcedSampleCount: 4x and 8x";
    // Members 2..12 -- 12 is ExtendedDoublesShaderInstructions, TRUE since
    // the double instructions are emulated (WARP: 1; this test expected 0
    // while doubles were refused).
    for (u32 member = 2; member <= 12; ++member) {
        EXPECT_EQ(o[member], 1u) << "member " << member;
    }
    // A GPU without framebuffer fetch or programmable sample positions
    // reports the logic ops and the forced-single-sample member FALSE.
    // MapNoOverwriteOnDynamicBufferSRV (member 9) is not about sample
    // positions: it stays TRUE from FL 10_0 (a01 "feature 5" rows).
    backend.state.framebuffer_fetch = false;
    backend.state.programmable_sample_positions = false;
    o = options();
    EXPECT_EQ(o[0], 0u);
    EXPECT_EQ(o[9], 1u);
    EXPECT_EQ(o[10], 0u);
    EXPECT_EQ(o[7], 1u) << "ConstantBufferOffsetting does not depend on it";
}

class D3D11LogicOpCapsTest : public D3D11OptionalCapsTest {
protected:
    void SetUp() override {
        // Before the device exists: the capability table is read once.
        backend.state.framebuffer_fetch = true;
        D3D11OptionalCapsTest::SetUp();
    }
    bool logic_bit(u32 format) {
        write_words(A6_ARG, {format, 0});
        device_call(33, {3, A6_ARG, 8});  // FORMAT_SUPPORT2
        return (memory.read_u32(A6_ARG + 4).value() & 0x100u) != 0;
    }
};

TEST_F(D3D11LogicOpCapsTest, LogicOpFormatBitOnEveryRenderableUint) {
    // fl111 "caps 11_0: FORMAT_SUPPORT2 logic-op bit: 3=1 12=1 17=1 25=1
    // 30=1 36=1 42=1 50=1 57=1 62=1 43=0 28=0 41=0" (D3D11.3 17.7: "all
    // renderable UINT formats").
    for (const u32 format : {3u, 12u, 17u, 25u, 30u, 36u, 42u, 50u, 57u,
                             62u}) {
        EXPECT_TRUE(logic_bit(format)) << "format " << format;
    }
    for (const u32 format : {43u, 28u, 41u}) {
        EXPECT_FALSE(logic_bit(format)) << "format " << format;
    }
}

TEST_F(D3D11OptionalCapsTest, NoLogicOpFormatBitWithoutTheCap) {
    write_words(A6_ARG, {42, 0});
    device_call(33, {3, A6_ARG, 8});
    EXPECT_EQ(memory.read_u32(A6_ARG + 4).value() & 0x100u, 0u);
}

TEST_F(D3D11OptionalCapsTest, DoubleShadersAreCreatedAtFeatureLevel11_0) {
    // d3d11_doubles_probe.expected: WARP at FL 11_0 creates every shader of
    // the double differential ("== dbin: CreateComputeShader(1520 bytes)
    // hr=0x00000000" ...): the SM5 set under dcl_globalFlags
    // enableDoublePrecisionFloatOps (SFI0 D3D_SHADER_REQUIRES_DOUBLES),
    // the 11.1 set (ddiv dfma drcp dtoi dtou itod utod) under
    // enable11_1DoubleExtensions, and the patched _sat / |x| forms.
    for (const auto& shader : doubles::g_dbl_shaders) {
        EXPECT_NE(compute_shader(shader.dxbc, shader.dxbc_size), 0u)
            << shader.name;
        EXPECT_EQ(last_hr, d3d11::S_OK) << shader.name;
    }
    // The graphics stages (vs_5_0 / ps_5_0 fxc output of the same file).
    struct Stage {
        u32 slot;  // CreateVertexShader 12, CreatePixelShader 15
        const unsigned char* blob;
        usize size;
    };
    const Stage stages[] = {
        {12u, doubles::g_dbl_vs_dbl_dxbc, sizeof(doubles::g_dbl_vs_dbl_dxbc)},
        {15u, doubles::g_dbl_ps_dbl_dxbc, sizeof(doubles::g_dbl_ps_dbl_dxbc)},
    };
    for (const Stage& stage : stages) {
        ASSERT_TRUE(memory.write_bytes(BLOB, stage.blob, stage.size).is_ok());
        ASSERT_TRUE(memory.write_u64(A6_OUT, 0).is_ok());
        EXPECT_EQ(device_call(stage.slot, {BLOB, stage.size, 0, A6_OUT}),
                  d3d11::S_OK) << "slot " << stage.slot;
        EXPECT_NE(memory.read_u64(A6_OUT).value(), 0u) << "slot " << stage.slot;
    }
    // CheckFeatureSupport(D3D11_FEATURE_DOUBLES), a01 "feature 1 size 4:
    // hr=0x00000000 data=1".
    ASSERT_TRUE(memory.write_u32(A6_ARG, 0xCDCDCDCDu).is_ok());
    EXPECT_EQ(device_call(33, {1, A6_ARG, 4}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(A6_ARG).value(), 1u);
    EXPECT_EQ(options()[12], 1u) << "ExtendedDoublesShaderInstructions";
}

TEST_F(D3D11OptionalCapsTest, ElevenOneUavShadersNeedFeatureLevel11_1) {
    // fl111 "fl111 at 11_0: vs_uav hr=0x80070057 cs_u63 hr=0x80070057
    // ps_u60 hr=0x80070057": u63 / u60 need the 64 UAV slots of FL 11_1
    // (SFI0 D3D_SHADER_REQUIRES_64_UAVS), refused with a NULL out-pointer.
    EXPECT_EQ(compute_shader(fl111::g_cs_u63, sizeof(fl111::g_cs_u63)), 0u);
    EXPECT_EQ(last_hr, d3d11::E_INVALIDARG);
    ASSERT_TRUE(memory.write_bytes(BLOB, fl111::g_ps_u60,
                                   sizeof(fl111::g_ps_u60)).is_ok());
    ASSERT_TRUE(memory.write_u64(A6_OUT, 0xABABABABull).is_ok());
    EXPECT_EQ(device_call(15, {BLOB, sizeof(fl111::g_ps_u60), 0, A6_OUT}),
              d3d11::E_INVALIDARG);
    EXPECT_EQ(memory.read_u64(A6_OUT).value(), 0u);
    // msad needs only SAD4ShaderInstructions, which FL 11_0 reports
    // ("msad create: hr=0x00000000").
    EXPECT_NE(compute_shader(fl111::g_cs_msad, sizeof(fl111::g_cs_msad)), 0u);
    EXPECT_EQ(last_hr, d3d11::S_OK);
}

TEST_F(D3D11OptionalCapsTest, ConstantBufferWindowsBoundEveryRead) {
    // D3D11.3 5.3.4.3.2 / 7.5: the shader sees NumConstants elements from
    // FirstConstant, and a read past the window or the buffer returns 0.
    // The translated shader reads its limit per b# slot from the push block;
    // each expected count is the number of non-zero elements the same
    // window read on WARP (fl111 "cbwin cs ..." lines).
    const GuestAddr cs =
        compute_shader(fl111::g_cs_cbwin, sizeof(fl111::g_cs_cbwin));
    ASSERT_NE(cs, 0u);
    context_call(69, {cs, 0, 0});
    const GuestAddr out = buffer({64 * 16, 0, 0x80, 0, 0x40, 16});
    ASSERT_TRUE(memory.write_u64(PTRS, make_view(8, out, {})).is_ok());
    context_call(68, {0, 1, PTRS, 0});
    const GuestAddr big = buffer({128 * 16, 0, 0x4, 0, 0, 0});
    const GuestAddr small = buffer({16 * 16, 0, 0x4, 0, 0, 0});
    constexpr usize kLimit = graphics::kConstantBufferLimitOffset / 4;
    const auto dispatch = [&](GuestAddr cb, bool set1, u32 first, u32 num) {
        ASSERT_TRUE(memory.write_u64(PTRS + 8, cb).is_ok());
        if (set1) {
            write_words(WINDOW, {first, num});
            context_call(124, {0, 1, PTRS + 8, WINDOW, WINDOW + 4});
        } else {
            context_call(71, {0, 1, PTRS + 8});  // CSSetConstantBuffers
        }
        context_call(41, {1, 1, 1});
        ASSERT_GT(backend.state.last_dispatch_constants.size(), kLimit);
    };
    const auto limit = [&] {
        return backend.state.last_dispatch_constants[kLimit];
    };
    const auto offset = [&] {
        return backend.state
            .last_dispatch_buffer_offsets[graphics::BACKEND_CONSTANT_BUFFER_BASE];
    };
    constexpr u32 kKnown = graphics::kConstantBufferLimitKnown;
    dispatch(big, false, 0, 0);  // "plain 128": all 64 the shader reads
    EXPECT_EQ(limit(), kKnown | 128u);
    EXPECT_EQ(offset(), 0u);
    dispatch(big, true, 16, 16);  // "set1 16+16": 16..31, then 0
    EXPECT_EQ(limit(), kKnown | 16u);
    EXPECT_EQ(offset(), 16u * 16u);
    dispatch(big, true, 32, 48);  // "set1 32+48": 32..79, then 0
    EXPECT_EQ(limit(), kKnown | 48u);
    dispatch(big, true, 112, 32);  // "112+32 (past end)": 112..127
    EXPECT_EQ(limit(), kKnown | 16u);
    dispatch(big, true, 128, 16);  // "128+16 (at end)": nothing
    EXPECT_EQ(limit(), kKnown | 0u);
    EXPECT_EQ(offset(), 0u) << "a Metal offset past the buffer is invalid";
    dispatch(big, true, 4096, 16);  // "4096+16": nothing
    EXPECT_EQ(limit(), kKnown | 0u);
    EXPECT_EQ(offset(), 0u);
    dispatch(big, true, 16, 0);  // "16+0": nothing
    EXPECT_EQ(limit(), kKnown | 0u);
    dispatch(small, true, 0, 4096);  // "small16 0+4096": 0..15
    EXPECT_EQ(limit(), kKnown | 16u);
    dispatch(small, false, 0, 0);  // "plain small16": 0..15
    EXPECT_EQ(limit(), kKnown | 16u);
    dispatch(0, false, 0, 0);  // "NULL": nothing
    EXPECT_EQ(limit(), kKnown | 0u);
}

TEST_F(D3D11OptionalCapsTest, MapNoOverwriteTakesEveryDynamicBuffer) {
    // Learn D3D11_MAP: the 11.1 runtime maps dynamic constant buffers and
    // dynamic buffer SRVs WRITE_NO_OVERWRITE on a device reporting
    // MapNoOverwriteOnDynamicConstantBuffer / ...BufferSRV (fl111
    // "nooverwrite cb/srv/typed-srv/raw-srv/vb/ib: hr=0x00000000", even as
    // the first map of the buffer).
    const auto map = [&](GuestAddr resource) {
        context_call(14, {resource, 0, 5 /*WRITE_NO_OVERWRITE*/, 0, A6_OUT});
        const u32 hr = static_cast<u32>(cpu.gpr.rax);
        if (hr == d3d11::S_OK) context_call(15, {resource, 0});
        return hr;
    };
    EXPECT_EQ(map(buffer({256, 2, 0x4, 0x10000, 0, 0})), d3d11::S_OK);
    EXPECT_EQ(map(buffer({256, 2, 0x8, 0x10000, 0x40, 16})), d3d11::S_OK);
    EXPECT_EQ(map(buffer({256, 2, 0x8, 0x10000, 0, 0})), d3d11::S_OK);
    EXPECT_EQ(map(buffer({256, 2, 0x8, 0x10000, 0x20, 0})), d3d11::S_OK);
    EXPECT_EQ(map(buffer({256, 2, 0x1 | 0x8, 0x10000, 0, 0})), d3d11::S_OK);
    EXPECT_EQ(map(buffer({256, 2, 0x2, 0x10000, 0, 0})), d3d11::S_OK);
    // Not a DYNAMIC resource: still refused (Learn: "must have been
    // created with write access").
    EXPECT_EQ(map(buffer({256, 0, 0x8, 0, 0, 0})), d3d11::E_INVALIDARG);
}

TEST_F(D3D11OptionalCapsTest, CopyFlagsFollowTheMeasuredRules) {
    // fl111 "copyflags update1/copy1: 0=++ 1=++ 2=++ 3=.. 4=.+ 5=.+ 6=.+
    // 7=.. 8=.. ... 80000000=..": UpdateSubresource1 takes only 0,
    // NO_OVERWRITE and DISCARD; CopySubresourceRegion1 also takes bit 2, but
    // never NO_OVERWRITE with DISCARD, and no other bit.
    const GuestAddr dst = buffer({64, 0, 0x8, 0, 0, 0});
    const GuestAddr src = buffer({64, 0, 0x8, 0, 0, 0});
    write_words(A6_ARG, {16, 0, 0, 32, 1, 1});
    const std::array<u32, 11> flags{0, 1, 2, 3, 4, 5, 6, 7, 8, 0x100,
                                    0x80000000u};
    const std::array<bool, 11> update_ok{true, true, true, false, false,
                                         false, false, false, false, false,
                                         false};
    const std::array<bool, 11> copy_ok{true, true, true, false, true, true,
                                       true, false, false, false, false};
    for (usize i = 0; i < flags.size(); ++i) {
        const unsigned updates = backend.state.buffer_updates;
        context_call(116, {dst, 0, A6_ARG, A6_DATA, 0, 0, flags[i]});
        EXPECT_EQ(backend.state.buffer_updates != updates, update_ok[i])
            << "UpdateSubresource1 flags 0x" << std::hex << flags[i];
        const usize copies = backend.state.buffer_region_copies.size();
        context_call(115, {dst, 0, 40, 0, 0, src, 0, A6_ARG, flags[i]});
        EXPECT_EQ(backend.state.buffer_region_copies.size() != copies,
                  copy_ok[i])
            << "CopySubresourceRegion1 flags 0x" << std::hex << flags[i];
    }
}

class D3D11OutputMergerCapsTest : public D3D11FixedFunctionTest {
protected:
    /// CreateBlendState1 with RT[0]'s desc: blend, logic op, op; the other
    /// targets copy it.
    u32 blend1(u32 blend, u32 logic, u32 op, u32 independent = 0) {
        write_words(BIG, {0, independent});
        for (u32 t = 0; t < 8; ++t) {
            write_words(BIG + 8 + t * 40,
                        {blend, logic, 2, 1, 1, 2, 1, 1, op, 0xF});
        }
        EXPECT_TRUE(memory.write_u64(OUT, 0xABABABABull).is_ok());
        return device_call(45, {BIG, OUT});
    }
    /// A 16x16 target of @p format and @p samples bound alone, viewport 0
    /// over it.
    void bind_target_samples(u32 format, u32 samples) {
        write_words(DESC, {16, 16, 1, 1, format, samples, 0, 0, 0x20, 0, 0});
        ASSERT_EQ(device_call(5, {DESC, 0, OUT}), d3d11::S_OK);
        ASSERT_EQ(device_call(9, {memory.read_u64(OUT).value(), 0, OUT2}),
                  d3d11::S_OK);
        ASSERT_TRUE(memory.write_u64(PTRS, memory.read_u64(OUT2).value())
                        .is_ok());
        context_call(33, {1, PTRS, 0});
        write_words(DESC, {0, 0, 0x41800000u, 0x41800000u, 0, 0x3F800000u});
        context_call(44, {1, DESC});
    }
    const std::string& fragment_msl() const {
        return backend.state.last_draw_pipeline_desc.fragment_shader_msl;
    }
};

TEST_F(D3D11OutputMergerCapsTest, LogicOpsNeedTheCapAndFollowTheBlendRules) {
    // Learn D3D11_RENDER_TARGET_BLEND_DESC1: LogicOpEnable needs
    // OutputMergerLogicOp, and excludes BlendEnable; D3D11.3 17.7 also
    // excludes IndependentBlendEnable (fl111 "logic create": 0..15 S_OK,
    // 16, +BlendEnable, +IndependentBlendEnable 0x80070057).
    backend.state.framebuffer_fetch = false;
    EXPECT_EQ(blend1(0, 1, 10), d3d11::E_INVALIDARG);
    backend.state.framebuffer_fetch = true;
    for (u32 op = 0; op < 16; ++op) {
        EXPECT_EQ(blend1(0, 1, op), d3d11::S_OK) << "op " << op;
    }
    EXPECT_EQ(blend1(0, 1, 16), d3d11::E_INVALIDARG);
    EXPECT_EQ(blend1(1, 1, 10), d3d11::E_INVALIDARG);
    EXPECT_EQ(blend1(0, 1, 10, 1), d3d11::E_INVALIDARG);
}

TEST_F(D3D11OutputMergerCapsTest, LogicOpDrawsReadTheirUintTargets) {
    // D3D11.3 17.7: the op combines the shader output with the target, on
    // UINT targets only. The pixel shader becomes a framebuffer-fetch
    // variant reading render target 0 (the bits WARP measured, fl111
    // "logic r32", are the GPU tests' business: Fl111Gpu).
    backend.state.framebuffer_fetch = true;
    use_shaders(fl111::g_vs_full, sizeof(fl111::g_vs_full), fl111::g_ps_logic,
                sizeof(fl111::g_ps_logic));
    ASSERT_EQ(blend1(0, 1, 10), d3d11::S_OK);
    context_call(35, {memory.read_u64(OUT).value(), 0, 0xFFFFFFFFu});
    context_call(24, {4});
    bind_target(42);  // R32_UINT
    context_call(13, {3, 0});
    ASSERT_EQ(backend.state.draws, 1u);
    EXPECT_NE(fragment_msl().find("vitreo_logic_dst0"), std::string::npos)
        << fragment_msl();
    // RGBA8_UNORM: not a logic-op format, the shader is the plain one.
    bind_target(28);
    context_call(13, {3, 0});
    ASSERT_EQ(backend.state.draws, 2u);
    EXPECT_EQ(fragment_msl().find("vitreo_logic_dst0"), std::string::npos);
    // A multisampled target runs the fetching shader once per sample; a
    // pixel shader with UAV side effects still draws, its u# accesses made
    // by the pixel's own invocation only (Windows
    // d3d11_ps_uav_msaa_probe "logic": one invocation per covered pixel;
    // PsUavMsaaGpu on real Metal).
    use_shaders(fl111::g_vs_full, sizeof(fl111::g_vs_full),
                fl111::g_ps_cov_uav, sizeof(fl111::g_ps_cov_uav));
    bind_target_samples(42, 4);
    context_call(13, {3, 0});
    ASSERT_EQ(backend.state.draws, 3u);
    EXPECT_NE(fragment_msl().find("vitreo_uav_leader"), std::string::npos)
        << fragment_msl();
}

TEST_F(D3D11OutputMergerCapsTest, ForcedSingleSampleOnAMultisampledTarget) {
    // D3D11.3 3.5.6.2: ForcedSampleCount 1 over a 4x target rasterizes the
    // pixel centre and writes every sample (fl111 "forced rt forced=1
    // samples=4": 101.101.101.101 where the centre is covered). The pass
    // puts every sample at the centre; the pixel shader's rasterizer has
    // one sample.
    backend.state.programmable_sample_positions = true;
    use_shaders(fl111::g_vs_tri, sizeof(fl111::g_vs_tri), fl111::g_ps_cov_rt,
                sizeof(fl111::g_ps_cov_rt));
    context_call(24, {4});
    bind_target_samples(42, 4);
    // D3D11_RASTERIZER_DESC1: SOLID, NONE, ..., DepthClipEnable, ...,
    // ForcedSampleCount.
    write_words(DESC, {3, 1, 0, 0, 0, 0, 1, 0, 0, 0, 1});
    ASSERT_EQ(device_call(46, {DESC, OUT}), d3d11::S_OK);
    const GuestAddr forced_one = memory.read_u64(OUT).value();
    context_call(43, {forced_one});
    context_call(13, {3, 0});
    ASSERT_EQ(backend.state.draws, 1u);
    EXPECT_TRUE(backend.state.last_pass_center_sample_positions);
    EXPECT_EQ(push_word<u32>(backend.state.last_draw_fragment_constants,
                             graphics::kRasterizerSampleCountOffset),
              1u);
    // Without a forced count the 4x target rasterizes its own samples.
    context_call(43, {0});
    context_call(13, {3, 0});
    EXPECT_FALSE(backend.state.last_pass_center_sample_positions);
    EXPECT_EQ(push_word<u32>(backend.state.last_draw_fragment_constants,
                             graphics::kRasterizerSampleCountOffset),
              4u);
}

namespace live_diag_tests {

/// "data=1,0,1,..." of the oracle row that starts with @p row_start.
std::vector<u32> oracle_row(const char* file, const std::string& row_start) {
    std::ifstream in(std::string(VITREO_ORACLE_PROBE_DIR) + "/" + file);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(row_start, 0) != 0) continue;
        const auto at = line.find("data=");
        if (at == std::string::npos) continue;
        std::vector<u32> values;
        std::stringstream numbers(line.substr(at + 5));
        std::string number;
        while (std::getline(numbers, number, ',')) {
            values.push_back(static_cast<u32>(std::stoul(number, nullptr, 16)));
        }
        return values;
    }
    ADD_FAILURE() << file << ": no row starting \"" << row_start << "\"";
    return {};
}

}  // namespace live_diag_tests

using namespace live_diag_tests;

// MapNoOverwriteOnDynamicBufferSRV (D3D11_OPTIONS member 9) is a promise that
// Map(WRITE_NO_OVERWRITE) takes a dynamic buffer bound as a shader resource
// (D3D11.3 5.6.1.2: "required to be supported for all D3D10+ hardware"). That
// is a property of the feature level, not of the GPU's sample positions
// (which decide member 10). Expected: the a01 "feature 5" rows of WARP
// (members 0..13; WARP answers member 9 = 1 at 11_0, 10_1 and 10_0 and 0 at
// 9_x), 64- and 32-bit.
TEST_F(D3D11ManagerTest, MapNoOverwriteOnDynamicBufferSrvFollowsTheFeatureLevel) {
    struct Level {
        u32 level;
        const char* row;  // the a01 row prefix; "" is the main 11_0 device
    };
    const Level levels[] = {
        {d3d11::FEATURE_LEVEL_11_0, ""},
        {d3d11::FEATURE_LEVEL_10_1, "FLa100 "},
        {d3d11::FEATURE_LEVEL_10_0, "FLa000 "},
        {d3d11::FEATURE_LEVEL_9_3, "FL9300 "},
        {d3d11::FEATURE_LEVEL_9_1, "FL9100 "},
    };
    ASSERT_FALSE(backend.state.programmable_sample_positions)
        << "this test backend has no programmable sample positions";
    for (const Level& entry : levels) {
        ASSERT_TRUE(memory.write_u32(LEVELS, entry.level).is_ok());
        prepare_create();
        set_arg(4, LEVELS);
        set_arg(5, 1);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_D3D11CreateDevice(marshaler);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
        const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
        constexpr GuestAddr DATA = DATA_BASE + 0x800;
        set_arg(0, device);
        set_arg(1, 5);  // D3D11_FEATURE_D3D11_OPTIONS
        set_arg(2, DATA);
        set_arg(3, 56);
        d3d11.impl_DeviceMethod(marshaler, 33);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
        for (const char* file : {"d3d11_a01_device_caps_probe64.expected",
                                 "d3d11_a01_device_caps_probe32.expected"}) {
            const auto row = oracle_row(
                file, std::string(entry.row) + "feature 5 size 56:");
            ASSERT_EQ(row.size(), 14u) << file << " " << entry.row;
            EXPECT_EQ(memory.read_u32(DATA + 9 * 4).value(), row[9])
                << "MapNoOverwriteOnDynamicBufferSRV at level 0x" << std::hex
                << entry.level << " vs " << file;
        }
    }
}

// The same member does not follow the GPU's sample positions: a GPU that has
// them or has not answers alike, and member 10 (the forced single sample) is
// the one that does.
TEST_F(D3D11OptionalCapsTest, MapNoOverwriteMemberIsIndependentOfSamplePositions) {
    backend.state.programmable_sample_positions = false;
    auto o = options();
    EXPECT_EQ(o[9], 1u) << "MapNoOverwriteOnDynamicBufferSRV";
    EXPECT_EQ(o[10], 0u) << "MultisampleRTVWithForcedSampleCountOne";
    backend.state.programmable_sample_positions = true;
    o = options();
    EXPECT_EQ(o[9], 1u);
    EXPECT_EQ(o[10], 1u);
}

// D3D11_FEATURE_DATA_D3D11_OPTIONS2.PSSpecifiedStencilRefSupported follows the
// backend (supports_shader_stencil_export) from shader model 5 (FL 11_0) up,
// and a pixel shader that writes SV_StencilRef is created exactly when the
// cap is on (d3d11_a09b [SV_StencilRef]: WARP refuses it with E_INVALIDARG
// where the cap is 0). The device's own end-to-end proof, that the written
// value reaches the stencil plane, is D3D11MetalTest.
// StencilRefPixelShaderWritesTheStencilBuffer.
TEST_F(D3D11OptionalCapsTest, PSSpecifiedStencilRefFollowsTheBackendsStencilExport) {
    const auto cap = [&] {
        for (u32 i = 0; i < 32; i += 4) {
            EXPECT_TRUE(memory.write_u32(A6_ARG + i, 0xCDCDCDCDu).is_ok());
        }
        EXPECT_EQ(device_call(33, {14, A6_ARG, 32}), d3d11::S_OK);
        return memory.read_u32(A6_ARG).value();
    };
    const auto create_ps = [&] {
        EXPECT_TRUE(memory.write_bytes(BLOB, a09b_ps_stencil_ref,
                                       sizeof(a09b_ps_stencil_ref)).is_ok());
        EXPECT_TRUE(memory.write_u64(A6_OUT, 0xABABABABull).is_ok());
        last_hr = device_call(15, {BLOB, sizeof(a09b_ps_stencil_ref), 0,
                                   A6_OUT});
        return memory.read_u64(A6_OUT).value();
    };
    backend.state.shader_stencil_export = false;
    EXPECT_EQ(cap(), 0u);
    EXPECT_EQ(create_ps(), 0u);
    EXPECT_EQ(last_hr, d3d11::E_INVALIDARG);

    backend.state.shader_stencil_export = true;
    EXPECT_EQ(cap(), 1u);
    EXPECT_NE(create_ps(), 0u) << "the cap is on: the shader is created";
    EXPECT_EQ(last_hr, d3d11::S_OK);

    // Below FL 11_0 (shader model 4) there is no oStencilRef, whatever the
    // GPU does: WARP 0 at 10_1 / 10_0 / 9_x (a01 "feature 14" rows).
    for (const u32 level : {d3d11::FEATURE_LEVEL_10_1,
                            d3d11::FEATURE_LEVEL_10_0,
                            d3d11::FEATURE_LEVEL_9_3}) {
        ASSERT_TRUE(memory.write_u32(LEVELS, level).is_ok());
        prepare_create();
        set_arg(4, LEVELS);
        set_arg(5, 1);
        Marshaler m(memory);
        m.attach_cpu(cpu);
        d3d11.impl_D3D11CreateDevice(m);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
        const GuestAddr other = memory.read_u64(DEVICE_OUT).value();
        set_arg(0, other);
        set_arg(1, 14);
        set_arg(2, A6_ARG);
        set_arg(3, 32);
        d3d11.impl_DeviceMethod(m, 33);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
        EXPECT_EQ(memory.read_u32(A6_ARG).value(), 0u)
            << "level 0x" << std::hex << level;
    }
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
