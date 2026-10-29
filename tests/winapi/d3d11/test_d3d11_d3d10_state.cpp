// SPDX-License-Identifier: LGPL-2.1-only
//
// Direct3D 10 queries, blobs/profiles, state blocks, emulation and the x86 frames.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_d3d10.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D10DeviceTest, QueriesPredicatesAndCounters) {
    const GuestAddr device = create10(false);
    // [obj] "CreateQuery(8) hr=0x80070057", predicates only 5 and 7.
    words(DESC, {8, 0});
    ASSERT_TRUE(memory.write_u64(OBJ_OUT, 0x5A5A5A50ull).is_ok());
    EXPECT_EQ(vcall(device, kD3D10CreateQuery, {DESC, OBJ_OUT}), kEInvalidArg);
    EXPECT_EQ(pointer(OBJ_OUT), 0u);
    words(DESC, {4, 0});
    EXPECT_EQ(vcall(device, kD3D10CreatePredicate, {DESC, OBJ_OUT}),
              kEInvalidArg);
    // "CreateQuery(4) ... size=64", "CreateQuery(0) ... size=4".
    ASSERT_EQ(vcall(device, kD3D10CreateQuery, {DESC, OBJ_OUT}), kSOk);
    const GuestAddr statistics = pointer(OBJ_OUT);
    EXPECT_EQ(vcall(statistics, 10, {}), 64u);
    EXPECT_EQ(qi(statistics, winapi::IID_ID3D10PREDICATE), 0u);
    words(GET, {99, 99});
    vcall(statistics, 11, {GET});
    EXPECT_EQ(word(GET), 4u);
    EXPECT_EQ(word(GET + 4), 0u);
    // "pipeline stats GetData(D3D11 size 88) hr=0x80070057".
    vcall(statistics, 7, {});
    vcall(statistics, 8, {});
    EXPECT_EQ(vcall(statistics, 9, {GET, 88, 0}), kEInvalidArg);
    release(statistics);
    words(DESC, {5, 0});
    ASSERT_EQ(vcall(device, kD3D10CreatePredicate, {DESC, OBJ_OUT}), kSOk);
    const GuestAddr predicate = pointer(OBJ_OUT);
    EXPECT_NE(qi(predicate, winapi::IID_ID3D10PREDICATE), 0u);
    release(predicate);
    EXPECT_EQ(vcall(predicate, 10, {}), 4u);
    release(predicate);
    // [fmt] counters: the well-known ones exist in name only.
    for (const u32 counter : {0u, 0x11u}) {
        words(DESC, {counter, 0});
        ASSERT_TRUE(memory.write_u64(OBJ_OUT, 0x5A5A5A50ull).is_ok());
        EXPECT_EQ(vcall(device, kD3D10CreateCounter, {DESC, OBJ_OUT}),
                  0x887A0004u);
        EXPECT_EQ(pointer(OBJ_OUT), 0x5A5A5A50ull) << "untouched";
    }
    words(DESC, {0x40000000u, 0});
    EXPECT_EQ(vcall(device, kD3D10CreateCounter, {DESC, OBJ_OUT}),
              kEInvalidArg);
    // "CheckCounter(0x0) hr=0 type=0 active=0 name(9)="GPU Idle"
    // units(9)="fraction" desc(91)=...", and "(0x0, 4-byte name) length=9
    // bytes=4750550...".
    words(DESC, {0, 0});
    words(U32S, {99, 99, 64, 64, 128});
    ASSERT_EQ(vcall(device, kD3D10CheckCounter,
                    {DESC, U32S, U32S + 4, NAMES, U32S + 8, NAMES + 64,
                     U32S + 12, NAMES + 128, U32S + 16}),
              kSOk);
    EXPECT_EQ(word(U32S), 0u);
    EXPECT_EQ(word(U32S + 4), 0u);
    EXPECT_EQ(word(U32S + 8), 9u);
    EXPECT_EQ(word(U32S + 12), 9u);
    EXPECT_EQ(word(U32S + 16), 91u);
    std::array<char, 9> name{};
    ASSERT_TRUE(memory.read_bytes(NAMES, reinterpret_cast<u8*>(name.data()), 9)
                    .is_ok());
    EXPECT_STREQ(name.data(), "GPU Idle");
    words(U32S + 8, {4});
    ASSERT_EQ(vcall(device, kD3D10CheckCounter,
                    {DESC, 0, 0, NAMES, U32S + 8, 0, 0, 0, 0}),
              kSOk);
    EXPECT_EQ(word(U32S + 8), 9u);
    ASSERT_TRUE(memory.read_bytes(NAMES, reinterpret_cast<u8*>(name.data()), 4)
                    .is_ok());
    EXPECT_EQ(std::string(name.data(), 3), "GPU");
    EXPECT_EQ(name[3], '\0');
    words(DESC, {0x12, 0});
    EXPECT_EQ(vcall(device, kD3D10CheckCounter,
                    {DESC, 0, 0, NAMES, U32S + 8, 0, 0, 0, 0}),
              kEInvalidArg);
    EXPECT_EQ(release(device), 0u);
}

TEST_F(D3D10DeviceTest, BlobsSignaturesAndProfiles) {
    namespace sh = d3d10_blobs;
    // D3D10CreateBlob: "(0) ... size=0 ptr=obj", "(NULL out)
    // hr=0x8876086c"; QI answers IUnknown and ID3D10Blob only.
    EXPECT_EQ(module(&D3D11Manager::impl_D3D10CreateBlob, {16, 0}),
              0x8876086Cu);
    ASSERT_EQ(module(&D3D11Manager::impl_D3D10CreateBlob, {0, OUT}), kSOk);
    const GuestAddr empty = pointer(OUT);
    EXPECT_NE(vcall(empty, 3, {}), 0u);
    EXPECT_EQ(vcall(empty, 4, {}), 0u);
    EXPECT_EQ(qi(empty, winapi::IID_ID3D10BLOB), empty);
    release(empty);
    u64 hr = 0;
    EXPECT_EQ(qi(empty, winapi::IID_ID3D11DEVICE, &hr), 0u);
    EXPECT_EQ(hr, kENoInterface);
    EXPECT_EQ(release(empty), 0u);

    // [blob] "input signature of vs40 bytes:44584243f0a79f70...": the ISGN
    // chunk in a container of its own, hash and all.
    static constexpr char kInputOfVs40[] =
        "44584243f0a79f70e952e6bdc51b7ba7eaec6d9a01000000740000000100000024000"
        "0004953474e4800000002000000080000003800000000000000000000000300000000"
        "0000000707000041000000000000000000000003000000010000000f0f0000504f534"
        "954494f4e00434f4c4f5200ab";
    static constexpr char kBothOfVs40[] =
        "44584243d6c9fd317e66874982024fabbd48eb7801000000cc0000000200000028000"
        "000780000004953474e480000000200000008000000380000000000000000000000030"
        "00000000000000707000041000000000000000000000003000000010000000f0f00005"
        "04f534954494f4e00434f4c4f5200ab4f53474e4c0000000200000008000000380000"
        "00000000000100000003000000000000000f000000440000000000000000000000030"
        "00000010000000f00000053565f506f736974696f6e00434f4c4f5200abab";
    const auto hex_of = [&](GuestAddr blob) {
        const GuestAddr data = vcall(blob, 3, {});
        const usize size = vcall(blob, 4, {});
        std::vector<u8> bytes(size);
        EXPECT_TRUE(memory.read_bytes(data, bytes.data(), size).is_ok());
        std::string text;
        char pair[3];
        for (const u8 byte : bytes) {
            std::snprintf(pair, sizeof(pair), "%02x", byte);
            text += pair;
        }
        return text;
    };
    ASSERT_TRUE(memory.write_bytes(CODE, sh::g_vs40, sizeof(sh::g_vs40))
                    .is_ok());
    ASSERT_EQ(signature(true, false, {CODE, sizeof(sh::g_vs40), OUT}), kSOk);
    EXPECT_EQ(hex_of(pointer(OUT)), kInputOfVs40);
    release(pointer(OUT));
    ASSERT_EQ(signature(true, true, {CODE, sizeof(sh::g_vs40), OUT}), kSOk);
    EXPECT_EQ(hex_of(pointer(OUT)), kBothOfVs40);
    release(pointer(OUT));
    // "input signature of vs50 hr=0x80004005 out=untouched", "... NULL out
    // hr=0x00000000".
    ASSERT_TRUE(memory.write_bytes(CODE, sh::g_vs50, sizeof(sh::g_vs50))
                    .is_ok());
    ASSERT_TRUE(memory.write_u64(OUT, 0x5A5A5A50ull).is_ok());
    EXPECT_EQ(signature(true, false, {CODE, sizeof(sh::g_vs50), OUT}), kEFail);
    EXPECT_EQ(pointer(OUT), 0x5A5A5A50ull);
    ASSERT_TRUE(memory.write_bytes(CODE, sh::g_vs40, sizeof(sh::g_vs40))
                    .is_ok());
    EXPECT_EQ(signature(true, false, {CODE, sizeof(sh::g_vs40), 0}), kSOk);
    EXPECT_EQ(signature(true, false, {CODE, 16, OUT}), kEFail);

    // "10_1 profiles vs_4_0 gs_4_0 ps_4_0", one static string each.
    const auto text = [&](char stage) {
        const GuestAddr at = profile(stage);
        std::array<char, 8> value{};
        EXPECT_TRUE(memory.read_bytes(at, reinterpret_cast<u8*>(value.data()),
                                      7).is_ok());
        return std::string(value.data());
    };
    EXPECT_EQ(text('v'), "vs_4_0");
    EXPECT_EQ(text('g'), "gs_4_0");
    EXPECT_EQ(text('p'), "ps_4_0");
    EXPECT_EQ(profile('v'), profile('v'));
}

TEST_F(D3D10DeviceTest, StateBlocksCaptureAndApplyWhatTheMaskNames) {
    namespace sh = d3d10_blobs;
    const GuestAddr device = create10(false);
    u64 hr = 0;
    const GuestAddr vs = shader10(device, kD3D10CreateVertexShader, sh::g_vs40,
                                  sizeof(sh::g_vs40), &hr);
    ASSERT_EQ(hr, kSOk);
    const GuestAddr cb = buffer10(device, 64, 0, 0x4);
    vcall(device, kD3D10VSSetShader, {vs});
    ASSERT_TRUE(memory.write_u64(PTRS, cb).is_ok());
    vcall(device, kD3D10VSSetConstantBuffers, {2, 1, PTRS});
    vcall(device, kD3D10IASetPrimitiveTopology, {1});
    // An all-enabled mask.
    std::array<u8, 76> mask{};
    mask.fill(0xFF);
    ASSERT_TRUE(memory.write_bytes(DESC, mask.data(), mask.size()).is_ok());
    ASSERT_EQ(module(&D3D11Manager::impl_D3D10CreateStateBlock,
                     {device, DESC, OUT}),
              kSOk);
    const GuestAddr block = pointer(OUT);
    // [sb] "state_block: answers:" -- nothing, IUnknown included.
    EXPECT_EQ(qi(block, winapi::IID_IUNKNOWN, &hr), 0u);
    EXPECT_EQ(hr, kENoInterface);
    // "Apply of a never-captured block hr=0x80004005".
    EXPECT_EQ(vcall(block, 4, {}), kEFail);
    // "Capture hr=0 vs count 1->2".
    EXPECT_EQ(add_ref(vs), 2u);
    release(vs);
    EXPECT_EQ(vcall(block, 3, {}), kSOk);
    EXPECT_EQ(add_ref(vs), 3u);
    release(vs);
    vcall(device, kD3D10ClearState, {});
    // "Apply hr=0 vs same=1 cb same=1 topo=1".
    EXPECT_EQ(vcall(block, 4, {}), kSOk);
    ASSERT_TRUE(memory.write_u64(OUT, 0).is_ok());
    vcall(device, kD3D10VSGetShader, {OUT});
    EXPECT_EQ(pointer(OUT), vs);
    release(vs);
    vcall(device, kD3D10VSGetConstantBuffers, {2, 1, GET});
    EXPECT_EQ(pointer(GET), cb);
    release(cb);
    vcall(device, kD3D10IAGetPrimitiveTopology, {GET});
    EXPECT_EQ(word(GET), 1u);
    // "ReleaseAllDeviceObjects hr=0 vs count=1", then Apply is E_FAIL.
    EXPECT_EQ(vcall(block, 5, {}), kSOk);
    vcall(device, kD3D10ClearState, {});
    EXPECT_EQ(add_ref(vs), 2u);
    release(vs);
    EXPECT_EQ(vcall(block, 4, {}), kEFail);
    // A VS-only mask leaves the topology alone ("VS-only block: vs same=1
    // topo=2").
    mask.fill(0);
    mask[0] = 1;
    ASSERT_TRUE(memory.write_bytes(DESC, mask.data(), mask.size()).is_ok());
    ASSERT_EQ(module(&D3D11Manager::impl_D3D10CreateStateBlock,
                     {device, DESC, OUT}),
              kSOk);
    const GuestAddr vs_only = pointer(OUT);
    vcall(device, kD3D10VSSetShader, {vs});
    EXPECT_EQ(vcall(vs_only, 3, {}), kSOk);
    vcall(device, kD3D10ClearState, {});
    vcall(device, kD3D10IASetPrimitiveTopology, {2});
    EXPECT_EQ(vcall(vs_only, 4, {}), kSOk);
    vcall(device, kD3D10VSGetShader, {OUT});
    EXPECT_EQ(pointer(OUT), vs);
    release(vs);
    vcall(device, kD3D10IAGetPrimitiveTopology, {GET});
    EXPECT_EQ(word(GET), 2u);
    ASSERT_TRUE(memory.write_u64(OUT, 0).is_ok());
    EXPECT_EQ(vcall(vs_only, 6, {OUT}), kSOk);
    EXPECT_EQ(pointer(OUT), device);
    release(device);
    EXPECT_EQ(release(vs_only), 0u);
    EXPECT_EQ(release(block), 0u);
    vcall(device, kD3D10ClearState, {});
    EXPECT_EQ(release(vs), 0u);
    EXPECT_EQ(release(cb), 0u);
    EXPECT_EQ(release(device), 0u);
}

// R4 (d3d11_draw_memo.hpp) relies on every restorable binding snapshot
// holding its objects: a memo keyed by a stamp names objects by address.
// A state block's Capture holds the sampler it copied, so after the
// application unbinds and releases it, it is still alive and Apply binds
// that same object again.
TEST_F(D3D10DeviceTest, StateBlockSnapshotsHoldTheSamplersTheyCaptured) {
    constexpr usize kPSSetSamplers = 6, kPSGetSamplers = 42,
                    kCreateSamplerState = 86;
    const GuestAddr device = create10(false);
    words(DESC, {0x15, 3, 3, 3, 0, 1, 0, 0, 0, 0, 0, 0, 0x7F7FFFFFu});
    ASSERT_EQ(vcall(device, kCreateSamplerState, {DESC, OUT}), kSOk);
    const GuestAddr sampler = pointer(OUT);
    ASSERT_TRUE(memory.write_u64(PTRS, sampler).is_ok());
    vcall(device, kPSSetSamplers, {0, 1, PTRS});
    std::array<u8, 76> mask{};
    mask.fill(0xFF);
    ASSERT_TRUE(memory.write_bytes(DESC, mask.data(), mask.size()).is_ok());
    ASSERT_EQ(module(&D3D11Manager::impl_D3D10CreateStateBlock,
                     {device, DESC, OUT}),
              kSOk);
    const GuestAddr block = pointer(OUT);
    EXPECT_EQ(vcall(block, 3, {}), kSOk);  // Capture
    // Unbound and released by the application: only the block holds it.
    ASSERT_TRUE(memory.write_u64(PTRS, 0).is_ok());
    vcall(device, kPSSetSamplers, {0, 1, PTRS});
    // D3D10 counts the runtime's holds in the public count (as the VS in
    // StateBlocksCaptureAndApplyWhatTheMaskNames): 1 left is the block's.
    EXPECT_EQ(release(sampler), 1u) << "alive: the captured block holds it";
    EXPECT_EQ(add_ref(sampler), 2u);
    EXPECT_EQ(release(sampler), 1u);
    EXPECT_EQ(vcall(block, 4, {}), kSOk);  // Apply
    ASSERT_TRUE(memory.write_u64(GET, 0).is_ok());
    vcall(device, kPSGetSamplers, {0, 1, GET});
    EXPECT_EQ(pointer(GET), sampler);
    release(sampler);
    // Dropping the block's hold and the binding frees it.
    EXPECT_EQ(release(block), 0u);
    vcall(device, kD3D10ClearState, {});
    EXPECT_EQ(add_ref(sampler), 0u) << "destroyed once nothing holds it";
    EXPECT_EQ(release(device), 0u);
}

TEST_F(D3D10DeviceTest, D3D10EmulationOnAD3D11Device) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    // [emu] "D3D11 device QI(ID3D10Device1) hr=0x80004002", then any
    // CreateDeviceContextState -- a refused one too -- turns the D3D10
    // interfaces on.
    u64 hr = 0;
    EXPECT_EQ(qi(device, winapi::IID_ID3D10DEVICE1, &hr), 0u);
    EXPECT_EQ(hr, kENoInterface);
    const GuidBytes& d3d10_1 = winapi::IID_ID3D10DEVICE1;
    ASSERT_TRUE(memory.write_bytes(IID_AT + 0x20, d3d10_1.data(), 16).is_ok());
    words(U32S, {0x1234});
    EXPECT_EQ(vcall(device, 47, {0, U32S, 1, 7, IID_AT + 0x20, GET, OUT}),
              kEInvalidArg);
    const GuestAddr d10 = qi(device, winapi::IID_ID3D10DEVICE1);
    ASSERT_NE(d10, 0u);
    EXPECT_EQ(qi(device, winapi::IID_ID3D10DEVICE), d10);
    release(d10);
    // Inactive: "GetFeatureLevel=0xb000", setters ignored, getters empty.
    EXPECT_EQ(vcall(d10, kD3D10GetFeatureLevel, {}), 0xB000u);
    vcall(d10, kD3D10IASetPrimitiveTopology, {2});
    vcall(d10, kD3D10IAGetPrimitiveTopology, {GET});
    EXPECT_EQ(word(GET), 0u);
    vcall(context, 24, {5});
    vcall(d10, kD3D10IAGetPrimitiveTopology, {GET});
    EXPECT_EQ(word(GET), 0u);
    // A D3D10 state: "CreateDeviceContextState(ID3D10Device1) hr=0
    // chosen=0xa100", then "swapped in: GetFeatureLevel=0xa100 d3d11
    // level=0xa100".
    words(U32S, {0xA100});
    ASSERT_EQ(vcall(device, 47, {0, U32S, 1, 7, IID_AT + 0x20, GET, OUT}),
              kSOk);
    EXPECT_EQ(word(GET), 0xA100u);
    const GuestAddr state = pointer(OUT);
    ASSERT_TRUE(memory.write_u64(PTRS, 0).is_ok());
    vcall(context, 131, {state, PTRS});
    const GuestAddr original = pointer(PTRS);
    EXPECT_EQ(vcall(d10, kD3D10GetFeatureLevel, {}), 0xA100u);
    EXPECT_EQ(vcall(device, 37, {}), 0xA100u);
    vcall(d10, kD3D10IASetPrimitiveTopology, {4});
    vcall(d10, kD3D10IAGetPrimitiveTopology, {GET});
    EXPECT_EQ(word(GET), 4u);
    vcall(context, 83, {GET});
    EXPECT_EQ(word(GET), 0u) << "the D3D11 context is off";
    // Swapped back: the D3D11 state's own bindings ("d3d11 topology=5
    // d3d10 get=0").
    vcall(context, 131, {original, PTRS});
    EXPECT_EQ(pointer(PTRS), state);
    vcall(context, 83, {GET});
    EXPECT_EQ(word(GET), 5u);
    vcall(d10, kD3D10IAGetPrimitiveTopology, {GET});
    EXPECT_EQ(word(GET), 0u);
    EXPECT_EQ(vcall(device, 37, {}), 0xB000u);
    release(state);
    release(state);
    release(original);
    release(d10);
    release(context);
    EXPECT_EQ(release(device), 0u);
}

TEST_F(D3D10DeviceX86Test, DeviceBuffersMapsViewportsAndStreamOutput) {
    cpu.gpr.rsp = STACK_BASE + 0x800;
    for (usize i = 0; i < 7; ++i) set_arg(i, 0);
    set_arg(4, 0xA100);
    set_arg(5, 0x20);
    set_arg(6, OUT);
    d3d11.impl_D3D10CreateDevice1(marshaler);
    ASSERT_EQ(cpu.gpr.rax & 0xFFFFFFFFu, 0u);
    const u32 device = word(OUT);
    ASSERT_NE(device, 0u);
    EXPECT_EQ(vcall(device, kD3D10GetFeatureLevel, {}), 0xA100u);
    // A dynamic buffer mapped through ID3D10Buffer::Map: a 4-byte pointer.
    words(DESC, {64, 2, 0x1, 0x10000, 0});
    ASSERT_EQ(vcall(device, kD3D10CreateBuffer, {DESC, 0, OUT}), 0u);
    const u32 buffer = word(OUT);
    words(OUT, {0x5A5A5A50u, 0x5A5A5A50u});
    ASSERT_EQ(vcall(buffer, 10, {4, 0, OUT}), 0u);
    EXPECT_NE(word(OUT), 0u);
    EXPECT_EQ(word(OUT + 4), 0x5A5A5A50u) << "one pointer on x86";
    vcall(buffer, 11, {});
    // A staging Texture2D: D3D10_MAPPED_TEXTURE2D is {pData, RowPitch}, 8
    // bytes on x86 -- nothing past it written ("pitch=64").
    words(DESC, {16, 16, 1, 1, 28, 1, 0, 3, 0, 0x20000, 0});
    ASSERT_EQ(vcall(device, kD3D10CreateTexture2D, {DESC, 0, OUT}), 0u);
    const u32 staging = word(OUT);
    words(GET, {0, 0, 0x5A5A5A50u});
    ASSERT_EQ(vcall(staging, 10, {0, 1, 0, GET}), 0u);
    EXPECT_NE(word(GET), 0u);
    EXPECT_EQ(word(GET + 4), 64u);
    EXPECT_EQ(word(GET + 8), 0x5A5A5A50u);
    vcall(staging, 11, {0});
    // Viewports round trip.
    words(DESC, {3, 4, 50, 60, 0, 0x3F800000u});
    vcall(device, kD3D10RSSetViewports, {1, DESC});
    words(OUT, {1});
    vcall(device, kD3D10RSGetViewports, {OUT, GET});
    EXPECT_EQ(word(GET), 3u);
    EXPECT_EQ(word(GET + 8), 50u);
    EXPECT_EQ(word(GET + 20), 0x3F800000u);
    // x86 D3D10_SO_DECLARATION_ENTRY: name(4) index(4) start count slot
    // pad -- 12 bytes.
    ASSERT_TRUE(memory.write_bytes(NAMES, reinterpret_cast<const u8*>(
                                              "SV_Position\0COLOR"),
                                   18).is_ok());
    ASSERT_TRUE(memory.write_bytes(CODE, d3d10_blobs::g_gsso40,
                                   sizeof(d3d10_blobs::g_gsso40)).is_ok());
    words(DESC, {NAMES, 0, 0x00000400u, NAMES + 12, 0, 0x00000400u});
    ASSERT_EQ(vcall(device, kD3D10CreateGeometryShaderWithStreamOutput,
                    {CODE, static_cast<u32>(sizeof(d3d10_blobs::g_gsso40)),
                     DESC, 2, 32, OUT}),
              0u);
    const u32 gso = word(OUT);
    EXPECT_NE(gso, 0u);
    EXPECT_EQ(vcall(device, kD3D10CreateGeometryShaderWithStreamOutput,
                    {CODE, static_cast<u32>(sizeof(d3d10_blobs::g_gsso40)),
                     DESC, 2, 28, OUT}),
              0x80070057u);
    vcall(gso, 2, {});
    vcall(staging, 2, {});
    vcall(buffer, 2, {});
    EXPECT_EQ(vcall(device, 2, {}), 0u);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
