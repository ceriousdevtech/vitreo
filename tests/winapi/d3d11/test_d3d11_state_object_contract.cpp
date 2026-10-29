// SPDX-License-Identifier: LGPL-2.1-only
//
// Area 04 state-object contracts.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_surface.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

class D3D11StateObjectContractTest : public D3D11SurfaceTest {
protected:
    u32 state_call(void (D3D11Manager::*method)(Marshaler&, usize),
                   GuestAddr object, usize slot,
                   std::initializer_list<u64> args) {
        set_arg(0, object);
        usize i = 1;
        for (const u64 a : args) set_arg(i++, a);
        (d3d11.*method)(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    }
    u32 release(void (D3D11Manager::*method)(Marshaler&, usize),
                GuestAddr object) {
        return state_call(method, object, 2, {});
    }
    /// D3D11_BLEND_DESC with every target at the Learn default.
    void blend_default() {
        write_words(DESC, {0, 0});
        for (u32 t = 0; t < 8; ++t) blend_target(t, {0, 2, 1, 1, 2, 1, 1, 0xF});
    }
    void blend_target(u32 target, std::initializer_list<u32> words) {
        write_words(DESC + 8 + target * 32, words);
    }
    /// D3D11_DEPTH_STENCIL_DESC defaults (the masks are two bytes at 16).
    void ds_default() {
        write_words(DESC, {1, 1, 2, 0, 0xFFFF, 1, 1, 1, 8, 1, 1, 1, 8});
    }
    void rs_default() { write_words(DESC, {3, 3, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0}); }
    void smp_default() {
        write_words(DESC, {0x15, 3, 3, 3, 0, 1, 1, 0x3F800000, 0x3F800000,
                           0x3F800000, 0x3F800000, 0xFF7FFFFF, 0x7F7FFFFF});
    }
    GuestAddr created() { return memory.read_u64(OUT).value(); }
};

// A04-D2/D4/D5: DepthFunc is validated only when depth is enabled, the write
// mask always.
TEST_F(D3D11StateObjectContractTest, DepthFuncIsJudgedOnlyWithDepthOn) {
    write_words(DESC, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    EXPECT_EQ(device_call(21, {DESC, OUT}), d3d11::S_OK) << "zero-filled";
    for (const u32 func : {0u, 9u}) {
        write_words(DESC, {0, 1, func});
        EXPECT_EQ(device_call(21, {DESC, OUT}), d3d11::S_OK) << func;
        write_words(DESC, {1, 1, func});
        EXPECT_EQ(device_call(21, {DESC, OUT}), d3d11::E_INVALIDARG) << func;
    }
    write_words(DESC, {0, 2, 2});
    EXPECT_EQ(device_call(21, {DESC, OUT}), d3d11::E_INVALIDARG);
}

// A04-B3/B5/B7/B9/B10/B11/C7: the D3D11_BLEND_DESC rules.
TEST_F(D3D11StateObjectContractTest, BlendDescriptionRules) {
    const auto blend = [&](u32 target, std::initializer_list<u32> words) {
        blend_default();
        blend_target(target, words);
        return device_call(20, {DESC, OUT});
    };
    // Accepted factor sets with BlendEnable (P44-47).
    for (u32 f = 0; f <= 20; ++f) {
        const bool color = (f >= 1 && f <= 11) || (f >= 14 && f <= 19);
        const bool alpha = color && f != 3 && f != 4 && f != 9 && f != 10 &&
                           f != 16 && f != 17;
        EXPECT_EQ(blend(0, {1, f, 1, 1, 2, 1, 1, 0xF}) == d3d11::S_OK, color)
            << "src " << f;
        EXPECT_EQ(blend(0, {1, 2, 1, 1, f, 1, 1, 0xF}) == d3d11::S_OK, alpha)
            << "srca " << f;
    }
    for (u32 op = 0; op <= 7; ++op) {
        EXPECT_EQ(blend(0, {1, 2, 1, op, 2, 1, 1, 0xF}) == d3d11::S_OK,
                  op >= 1 && op <= 5) << op;
    }
    // Disabled: nothing but the mask is judged (P29-43).
    EXPECT_EQ(blend(0, {0, 0, 20, 6, 0, 20, 0, 0xF}), d3d11::S_OK);
    // Write masks above 0xF (P60-67); the padding bytes are not the mask.
    EXPECT_EQ(blend(0, {0, 2, 1, 1, 2, 1, 1, 0x10}), d3d11::E_INVALIDARG);
    EXPECT_EQ(blend(0, {0, 2, 1, 1, 2, 1, 1, 0xCDCDCD0F}), d3d11::S_OK);
    // Dual source (P51-59): op ADD/SUB/REV_SUB per pair, RT0 only, alone.
    EXPECT_EQ(blend(0, {1, 16, 1, 4, 2, 1, 1, 0xF}), d3d11::E_INVALIDARG);
    EXPECT_EQ(blend(0, {1, 16, 1, 3, 2, 1, 1, 0xF}), d3d11::S_OK);
    EXPECT_EQ(blend(0, {1, 2, 1, 5, 18, 1, 1, 0xF}), d3d11::S_OK);
    EXPECT_EQ(blend(0, {1, 2, 1, 1, 18, 1, 4, 0xF}), d3d11::E_INVALIDARG);
    blend_default();
    write_words(DESC, {0, 1});
    blend_target(0, {1, 16, 1, 1, 2, 1, 1, 0xF});
    blend_target(1, {1, 2, 1, 1, 2, 1, 1, 0xF});
    EXPECT_EQ(device_call(20, {DESC, OUT}), d3d11::E_INVALIDARG);
    blend_target(1, {0, 2, 1, 1, 2, 1, 1, 0xF});
    EXPECT_EQ(device_call(20, {DESC, OUT}), d3d11::S_OK);
    // Independent targets are judged only with IndependentBlendEnable.
    blend_target(3, {0, 2, 1, 1, 2, 1, 1, 0x30});
    EXPECT_EQ(device_call(20, {DESC, OUT}), d3d11::E_INVALIDARG);
    write_words(DESC, {0, 0});
    EXPECT_EQ(device_call(20, {DESC, OUT}), d3d11::S_OK);
    // Validated before a NULL ppState answers S_FALSE (P10).
    blend_default();
    blend_target(0, {1, 0, 1, 1, 2, 1, 1, 0xF});
    EXPECT_EQ(device_call(20, {DESC, 0}), d3d11::E_INVALIDARG);
    blend_default();
    EXPECT_EQ(device_call(20, {DESC, 0}), 1u);
    // CreateBlendState1 applies the same rules (P108-109).
    write_words(DESC, {0, 0});
    for (u32 t = 0; t < 8; ++t) {
        write_words(DESC + 8 + t * 40, {0, 0, 2, 1, 1, 2, 1, 1, 4, 0xF});
    }
    write_words(DESC + 8, {1, 0, 2, 1, 1, 3, 1, 1, 4, 0xF});
    EXPECT_EQ(device_call(45, {DESC, OUT}), d3d11::E_INVALIDARG);
    write_words(DESC + 8, {0, 0, 2, 1, 1, 2, 1, 1, 4, 0x10});
    EXPECT_EQ(device_call(45, {DESC, OUT}), d3d11::E_INVALIDARG);
}

// A04-B13-B15/C10/D8-D10/R5/S12-S14: GetDesc reports the canonical
// description.
TEST_F(D3D11StateObjectContractTest, GetDescReportsTheCanonicalDescription) {
    // Blend: BOOLs 1, RT[1..7] = RT[0] without independent blend, a disabled
    // target ONE/ZERO/ADD (P74-78, P83).
    blend_default();
    write_words(DESC, {2, 0});
    blend_target(0, {5, 5, 6, 1, 2, 1, 1, 0x7});
    blend_target(1, {1, 9, 2, 5, 2, 1, 1, 0x1});
    ASSERT_EQ(device_call(20, {DESC, OUT}), d3d11::S_OK);
    const GuestAddr blend = created();
    state_call(&D3D11Manager::impl_BlendStateMethod, blend, 7, {OUT2});
    EXPECT_EQ(memory.read_u32(OUT2).value(), 1u) << "AlphaToCoverage";
    for (const u32 t : {0u, 1u, 7u}) {
        const GuestAddr rt = OUT2 + 8 + t * 32;
        EXPECT_EQ(memory.read_u32(rt).value(), 1u) << t;
        EXPECT_EQ(memory.read_u32(rt + 4).value(), 5u) << t;
        EXPECT_EQ(memory.read_u32(rt + 8).value(), 6u) << t;
        EXPECT_EQ(memory.read_u8(rt + 28).value(), 0x7u) << t;
    }
    blend_default();
    blend_target(0, {0, 7, 1, 3, 2, 1, 1, 0xF});
    ASSERT_EQ(device_call(20, {DESC, OUT}), d3d11::S_OK);
    state_call(&D3D11Manager::impl_BlendStateMethod, created(), 7, {OUT2});
    EXPECT_EQ(memory.read_u32(OUT2 + 12).value(), 2u) << "SrcBlend ONE";
    EXPECT_EQ(memory.read_u32(OUT2 + 20).value(), 1u) << "BlendOp ADD";

    // Depth-stencil (P163-166).
    write_words(DESC, {0, 0, 0, 0, 0x340F, 1, 1, 7, 8, 1, 1, 1, 1});
    ASSERT_EQ(device_call(21, {DESC, OUT}), d3d11::S_OK);
    state_call(&D3D11Manager::impl_DepthStencilStateMethod, created(), 7,
               {OUT2});
    EXPECT_EQ(memory.read_u32(OUT2 + 4).value(), 1u) << "write ALL";
    EXPECT_EQ(memory.read_u32(OUT2 + 8).value(), 2u) << "func LESS";
    EXPECT_EQ(memory.read_u8(OUT2 + 16).value(), 0xFFu);
    EXPECT_EQ(memory.read_u8(OUT2 + 17).value(), 0xFFu);
    EXPECT_EQ(memory.read_u32(OUT2 + 28).value(), 1u) << "front pass KEEP";
    EXPECT_EQ(memory.read_u32(OUT2 + 48).value(), 8u) << "back func ALWAYS";
    write_words(DESC, {2, 0, 7, 3, 0x3412, 4, 8, 3, 3, 6, 1, 1, 6});
    ASSERT_EQ(device_call(21, {DESC, OUT}), d3d11::S_OK);
    state_call(&D3D11Manager::impl_DepthStencilStateMethod, created(), 7,
               {OUT2});
    EXPECT_EQ(memory.read_u32(OUT2).value(), 1u);
    EXPECT_EQ(memory.read_u32(OUT2 + 12).value(), 1u);
    EXPECT_EQ(memory.read_u8(OUT2 + 16).value(), 0x12u);
    EXPECT_EQ(memory.read_u32(OUT2 + 20).value(), 4u);

    // Rasterizer BOOLs (P193).
    write_words(DESC, {2, 2, 2, 0xFFFFFFF9u, 0xC0200000u, 0x3FC00000u, 3, 4,
                       5, 6});
    ASSERT_EQ(device_call(22, {DESC, OUT}), d3d11::S_OK);
    state_call(&D3D11Manager::impl_RasterizerStateMethod, created(), 7,
               {OUT2});
    for (const u32 offset : {8u, 24u, 28u, 32u, 36u}) {
        EXPECT_EQ(memory.read_u32(OUT2 + offset).value(), 1u) << offset;
    }
    EXPECT_EQ(memory.read_u32(OUT2 + 12).value(), 0xFFFFFFF9u);

    // Sampler (P313, P318, P320).
    write_words(DESC, {0x4, 1, 2, 5, 0xBFA00000u, 7, 5, 0x3F000000u,
                       0x3E800000u, 0x3E000000u, 0x3F400000u, 0x3F000000u,
                       0x41100000u});
    ASSERT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK);
    state_call(&D3D11Manager::impl_SamplerStateMethod, created(), 7, {OUT2});
    EXPECT_EQ(memory.read_u32(OUT2 + 20).value(), 0u) << "MaxAnisotropy";
    EXPECT_EQ(memory.read_u32(OUT2 + 24).value(), 1u) << "NEVER";
    EXPECT_EQ(memory.read_u32(OUT2 + 28).value(), 0u) << "border";
    EXPECT_EQ(memory.read_u32(OUT2 + 16).value(), 0xBFA00000u);
    write_words(DESC, {0x55, 3, 3, 3, 0, 9, 5});
    ASSERT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK);
    state_call(&D3D11Manager::impl_SamplerStateMethod, created(), 7, {OUT2});
    EXPECT_EQ(memory.read_u32(OUT2 + 20).value(), 9u);
    write_words(DESC, {0x15, 4, 4, 4, 0, 4, 0, 0x3F000000u});
    ASSERT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK);
    state_call(&D3D11Manager::impl_SamplerStateMethod, created(), 7, {OUT2});
    EXPECT_EQ(memory.read_u32(OUT2 + 28).value(), 0x3F000000u) << "BORDER";
    EXPECT_EQ(memory.read_u32(OUT2 + 24).value(), 1u);
}

// A04-B17-B20/C9/D12/R7/R9/S16: identical canonical descriptions share one
// object and its private data; the final Release removes it.
TEST_F(D3D11StateObjectContractTest, IdenticalDescriptionsShareOneObject) {
    constexpr GuestAddr TAG = OUT2 + 0x10;
    constexpr GuestAddr VALUE = OUT2 + 0x20;
    constexpr GuestAddr SIZE = OUT2 + 0x24;
    write_words(TAG, {0x0a04a04b, 0x43211234, 0x0011049a, 0x55443322});
    const auto blend = [&] {
        blend_default();
        blend_target(0, {1, 5, 6, 1, 2, 1, 1, 0xF});
        EXPECT_EQ(device_call(20, {DESC, OUT}), d3d11::S_OK);
        return created();
    };
    const GuestAddr a = blend();
    const GuestAddr b = blend();
    EXPECT_EQ(a, b);
    write_words(VALUE, {0xA04B1E0D});
    EXPECT_EQ(state_call(&D3D11Manager::impl_BlendStateMethod, a, 5,
                         {TAG, 4, VALUE}), d3d11::S_OK);
    write_words(VALUE, {0});
    write_words(SIZE, {4});
    EXPECT_EQ(state_call(&D3D11Manager::impl_BlendStateMethod, b, 4,
                         {TAG, SIZE, VALUE}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(VALUE).value(), 0xA04B1E0Du);
    EXPECT_EQ(release(&D3D11Manager::impl_BlendStateMethod, b), 1u);
    EXPECT_EQ(release(&D3D11Manager::impl_BlendStateMethod, a), 0u);
    const GuestAddr c = blend();
    write_words(SIZE, {4});
    EXPECT_EQ(state_call(&D3D11Manager::impl_BlendStateMethod, c, 4,
                         {TAG, SIZE, VALUE}), 0x887A0002u) << "a new object";
    // RT1..7 are not part of the key without independent blend, disabled
    // factors are not, and CreateBlendState1 with a disabled LogicOp is the
    // same object (P79-80, P84, P111-112).
    blend_default();
    blend_target(0, {1, 5, 6, 1, 2, 1, 1, 0xF});
    blend_target(4, {1, 9, 9, 3, 2, 1, 1, 0x1});
    ASSERT_EQ(device_call(20, {DESC, OUT}), d3d11::S_OK);
    EXPECT_EQ(created(), c);
    write_words(DESC, {0, 0});
    for (u32 t = 0; t < 8; ++t) {
        write_words(DESC + 8 + t * 40, {1, 0, 5, 6, 1, 2, 1, 1, 1, 0xF});
    }
    ASSERT_EQ(device_call(45, {DESC, OUT}), d3d11::S_OK);
    EXPECT_EQ(created(), c) << "Blend1 shares the pool";

    // Rasterizer: BOOL 2 == 1, and every version shares one pool (P195,
    // P200-201).
    rs_default();
    write_words(DESC + 28, {1});
    ASSERT_EQ(device_call(22, {DESC, OUT}), d3d11::S_OK);
    const GuestAddr rs = created();
    write_words(DESC + 28, {2});
    ASSERT_EQ(device_call(22, {DESC, OUT}), d3d11::S_OK);
    EXPECT_EQ(created(), rs);
    write_words(DESC + 28, {1, 0, 0, 0, 0});
    ASSERT_EQ(device_call(46, {DESC, OUT}), d3d11::S_OK);
    EXPECT_EQ(created(), rs);
    ASSERT_EQ(device_call(56, {DESC, OUT}), d3d11::S_OK);
    EXPECT_EQ(created(), rs);

    // Depth-stencil and sampler: ignored fields dedup (P164, P167, P314-316),
    // floats compare bitwise (P323).
    write_words(DESC, {0, 1, 8, 0});
    ASSERT_EQ(device_call(21, {DESC, OUT}), d3d11::S_OK);
    const GuestAddr ds = created();
    write_words(DESC, {0, 1, 2, 0});
    ASSERT_EQ(device_call(21, {DESC, OUT}), d3d11::S_OK);
    EXPECT_EQ(created(), ds);
    smp_default();
    write_words(DESC + 20, {7});
    ASSERT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK);
    const GuestAddr smp = created();
    write_words(DESC + 20, {3});
    ASSERT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK);
    EXPECT_EQ(created(), smp);
    write_words(DESC + 16, {0x80000000u});
    ASSERT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK);
    EXPECT_NE(created(), smp) << "-0.0 is not +0.0";
}

// A04-B25/D15/R12/S17/S19: 4096 unique objects per family, one of them the
// runtime's own default, which outlives the application's last Release.
TEST_F(D3D11StateObjectContractTest, UniqueObjectLimitCountsTheRuntimeDefault) {
    std::vector<GuestAddr> made;
    u32 first_fail = 0;
    u32 fail_hr = 0;
    for (u32 i = 0; i < 4100 && first_fail == 0; ++i) {
        smp_default();
        const float lod = static_cast<float>(i);
        u32 bits = 0;
        std::memcpy(&bits, &lod, 4);
        write_words(DESC + 44, {bits});
        const u32 hr = device_call(23, {DESC, OUT});
        if (hr != d3d11::S_OK) {
            first_fail = i;
            fail_hr = hr;
        } else {
            made.push_back(created());
        }
    }
    EXPECT_EQ(made.size(), 4095u);
    EXPECT_EQ(first_fail, 4095u);
    EXPECT_EQ(fail_hr, 0x887C0001u);
    // An existing description still returns its object at the limit.
    smp_default();
    write_words(DESC + 44, {0});
    ASSERT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK);
    EXPECT_EQ(created(), made.front());
    (void)release(&D3D11Manager::impl_SamplerStateMethod, created());
    for (const GuestAddr object : made) {
        (void)release(&D3D11Manager::impl_SamplerStateMethod, object);
    }
    smp_default();
    write_words(DESC + 44, {0x47C35000u});  // 100000.0
    EXPECT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK) << "after release";

    // The Learn default desc: the application's last Release leaves the
    // runtime's object, private data and all (P325).
    constexpr GuestAddr TAG = OUT2 + 0x10;
    constexpr GuestAddr VALUE = OUT2 + 0x20;
    constexpr GuestAddr SIZE = OUT2 + 0x24;
    write_words(TAG, {1, 2, 3, 4});
    smp_default();
    ASSERT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK);
    const GuestAddr runtime_default = created();
    write_words(VALUE, {0xA0450000u});
    EXPECT_EQ(state_call(&D3D11Manager::impl_SamplerStateMethod,
                         runtime_default, 5, {TAG, 4, VALUE}), d3d11::S_OK);
    EXPECT_EQ(release(&D3D11Manager::impl_SamplerStateMethod,
                      runtime_default), 0u);
    ASSERT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK);
    EXPECT_EQ(created(), runtime_default);
    write_words(SIZE, {4});
    EXPECT_EQ(state_call(&D3D11Manager::impl_SamplerStateMethod,
                         runtime_default, 4, {TAG, SIZE, VALUE}),
              d3d11::S_OK);
}

// A04-R3/R1-2/R1-3/R2-2/R2-4/R8/R10: the rasterizer rules, the latest-version
// interfaces, SetPrivateDataInterface.
TEST_F(D3D11StateObjectContractTest, RasterizerRulesAndInterfaces) {
    rs_default();
    write_words(DESC + 16, {0x7FC00000u});  // DepthBiasClamp NaN
    EXPECT_EQ(device_call(22, {DESC, OUT}), d3d11::E_INVALIDARG);
    rs_default();
    write_words(DESC + 20, {0x7FC00000u});  // SlopeScaledDepthBias NaN
    EXPECT_EQ(device_call(22, {DESC, OUT}), d3d11::E_INVALIDARG);
    rs_default();
    write_words(DESC + 16, {0x7F800000u, 0xFF800000u});  // +Inf, -Inf
    EXPECT_EQ(device_call(22, {DESC, OUT}), d3d11::S_OK);
    // ForcedSampleCount at FL 11_0: 0, 1, 4, 8; 1 excludes MultisampleEnable.
    for (u32 forced = 0; forced <= 32; ++forced) {
        for (const u32 ms : {0u, 1u}) {
            rs_default();
            write_words(DESC + 32, {ms, 0, forced});
            const bool valid = forced == 0 || (forced == 1 && ms == 0) ||
                               forced == 4 || forced == 8;
            EXPECT_EQ(device_call(46, {DESC, OUT}),
                      valid ? d3d11::S_OK : d3d11::E_INVALIDARG)
                << forced << " ms=" << ms;
        }
    }
    // ConservativeRaster ON is E_INVALIDARG at tier 0; DESC2 FSC 8 works.
    rs_default();
    write_words(DESC + 44, {1});
    EXPECT_EQ(device_call(56, {DESC, OUT}), d3d11::E_INVALIDARG);
    write_words(DESC + 40, {8, 0});
    ASSERT_EQ(device_call(56, {DESC, OUT}), d3d11::S_OK);
    state_call(&D3D11Manager::impl_RasterizerStateMethod, created(), 9,
               {OUT2});
    EXPECT_EQ(memory.read_u32(OUT2 + 40).value(), 8u);

    // A base-created object answers RasterizerState1/2 (P196-199).
    rs_default();
    ASSERT_EQ(device_call(22, {DESC, OUT}), d3d11::S_OK);
    const GuestAddr base = created();
    // d3d11_1.h IID_ID3D11RasterizerState1.
    constexpr Guid IID_ID3D11RASTERIZERSTATE1{
        0x1217D7A6u, 0x5039u, 0x418Cu,
        {0xB0, 0x42, 0x9C, 0xBE, 0x25, 0x6A, 0xFD, 0x6E}};
    for (const Guid& iid :
         {IID_ID3D11RASTERIZERSTATE1, IID_ID3D11RASTERIZERSTATE2}) {
        ASSERT_TRUE(memory.write_bytes(OUT2 + 0x40,
                                       reinterpret_cast<const Byte*>(&iid),
                                       sizeof(iid)).is_ok());
        EXPECT_EQ(state_call(&D3D11Manager::impl_RasterizerStateMethod, base,
                             0, {OUT2 + 0x40, OUT2}), d3d11::S_OK);
        EXPECT_EQ(memory.read_u64(OUT2).value(), base);
        (void)release(&D3D11Manager::impl_RasterizerStateMethod, base);
    }
    // SetPrivateDataInterface (slot 6) AddRefs the interface through its
    // guest vtable and releases it on clear (P331), as on every other child.
    constexpr GuestAddr TAG = OUT2 + 0x60;
    constexpr GuestAddr GUEST_VTABLE = OUT2 + 0x80;
    constexpr GuestAddr GUEST_IFACE = OUT2 + 0xA0;
    constexpr GuestAddr GUEST_ADDREF = 0x72000010;
    constexpr GuestAddr GUEST_RELEASE = 0x72000020;
    write_words(TAG, {5, 6, 7, 8});
    ASSERT_TRUE(memory.write_u64(GUEST_VTABLE + 8, GUEST_ADDREF).is_ok());
    ASSERT_TRUE(memory.write_u64(GUEST_VTABLE + 16, GUEST_RELEASE).is_ok());
    ASSERT_TRUE(memory.write_u64(GUEST_IFACE, GUEST_VTABLE).is_ok());
    std::vector<GuestAddr> calls;
    d3d11.set_guest_function_caller(
        [&calls](GuestAddr function, const u64*, usize) -> u64 {
            calls.push_back(function);
            return 1;
        });
    EXPECT_EQ(state_call(&D3D11Manager::impl_RasterizerStateMethod, base, 6,
                         {TAG, GUEST_IFACE}), d3d11::S_OK);
    EXPECT_EQ(calls, (std::vector<GuestAddr>{GUEST_ADDREF}));
    EXPECT_EQ(state_call(&D3D11Manager::impl_RasterizerStateMethod, base, 6,
                         {TAG, 0}), d3d11::S_OK);
    EXPECT_EQ(calls, (std::vector<GuestAddr>{GUEST_ADDREF, GUEST_RELEASE}));
}

// A04-S5/S7/S8/S9: sampler MaxAnisotropy 0..16 for every filter, MinLOD >
// MaxLOD legal, NaN LODs refused, MipLODBias within [-16, 15.99].
TEST_F(D3D11StateObjectContractTest, SamplerRules) {
    for (const u32 filter : {0x55u, 0xD5u, 0x15u, 0x0u}) {
        for (const u32 aniso : {0u, 1u, 16u, 17u, 0xFFFFFFFFu}) {
            smp_default();
            write_words(DESC, {filter});
            write_words(DESC + 20, {aniso});
            EXPECT_EQ(device_call(23, {DESC, OUT}),
                      aniso <= 16 ? d3d11::S_OK : d3d11::E_INVALIDARG)
                << filter << " " << aniso;
        }
    }
    smp_default();
    write_words(DESC + 44, {0x40A00000u, 0x3F800000u});  // 5 > 1
    EXPECT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK);
    write_words(DESC + 44, {0x7FC00000u, 0x3F800000u});
    EXPECT_EQ(device_call(23, {DESC, OUT}), d3d11::E_INVALIDARG);
    write_words(DESC + 44, {0x3F800000u, 0x7FC00000u});
    EXPECT_EQ(device_call(23, {DESC, OUT}), d3d11::E_INVALIDARG);
    write_words(DESC + 44, {0xFF800000u, 0x7F800000u});  // -Inf, +Inf
    EXPECT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK);
    const std::array<std::pair<u32, u32>, 7> biases{{
        {0xC1800000u, d3d11::S_OK},          // -16.0
        {0x417FD70Au, d3d11::S_OK},          // 15.99
        {0xC1840000u, d3d11::E_INVALIDARG},  // -16.5
        {0x41800000u, d3d11::E_INVALIDARG},  // 16.0
        {0x42C80000u, d3d11::E_INVALIDARG},  // 100
        {0x7FC00000u, d3d11::E_INVALIDARG},  // NaN
        {0x7F800000u, d3d11::E_INVALIDARG},  // +Inf
    }};
    for (const auto& [bias, hr] : biases) {
        smp_default();
        write_words(DESC + 16, {bias});
        EXPECT_EQ(device_call(23, {DESC, OUT}), hr) << std::hex << bias;
    }
}

// The runtime's default object lives as long as its device: the device's
// final release frees it, and the pools go with the device.
TEST_F(D3D11StateObjectContractTest, DefaultObjectsGoWithTheirDevice) {
    const GuestAddr first = device;
    const GuestAddr first_context = context;
    create_default_device();
    device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr second_context = memory.read_u64(CONTEXT_OUT).value();
    smp_default();
    ASSERT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK);
    const GuestAddr runtime_default = created();
    EXPECT_EQ(release(&D3D11Manager::impl_SamplerStateMethod,
                      runtime_default), 0u);
    EXPECT_EQ(state_call(&D3D11Manager::impl_SamplerStateMethod,
                         runtime_default, 1, {}), 1u)
        << "alive while the device is";
    EXPECT_EQ(release(&D3D11Manager::impl_SamplerStateMethod,
                      runtime_default), 0u);
    set_arg(0, second_context);
    d3d11.impl_ContextMethod(marshaler, 2);
    set_arg(0, device);
    d3d11.impl_DeviceMethod(marshaler, 2);
    EXPECT_EQ(state_call(&D3D11Manager::impl_SamplerStateMethod,
                         runtime_default, 1, {}), 0u)
        << "freed with its device";
    device = first;
    context = first_context;
    ASSERT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK);
    EXPECT_NE(created(), 0u);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
