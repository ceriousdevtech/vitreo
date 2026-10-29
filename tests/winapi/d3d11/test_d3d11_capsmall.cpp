// SPDX-License-Identifier: LGPL-2.1-only
//
// Small optional capabilities, mappable default buffers, ClearView and markers.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

namespace capsmall_blobs {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../../oracle_data/d3d11_capsmall_shaders.h"
#pragma clang diagnostic pop
}  // namespace capsmall_blobs

#include "d3d11_test_surface.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

// ---------------------------------------------------------------------------
// Optional capabilities WARP reports at FL 11_0 (d3d11_capsmall_probe):
// CheckFeatureSupport sizes, SHADER_MIN_PRECISION_SUPPORT, MARKER_SUPPORT and
// the markers, OPTIONS1 ClearViewAlsoSupportsDepthOnlyFormats /
// MapOnDefaultBuffers, OPTIONS2 UnifiedMemoryArchitecture. Every expected
// value is a line of tools/oracle_probe/d3d11_capsmall_probe.expected
// (Windows, WARP; x86 = x64) or the Learn page it names.
// ---------------------------------------------------------------------------



class D3D11CapsmallTest : public D3D11SurfaceTest {
protected:
    static constexpr GuestAddr PAGE = 0x200000;
    static constexpr GuestAddr INIT = PAGE;             // subresource data
    static constexpr GuestAddr BYTES = PAGE + 0x100;    // initial contents
    static constexpr GuestAddr RECTS = PAGE + 0x300;
    static constexpr GuestAddr COLOR4 = PAGE + 0x380;
    static constexpr GuestAddr MAPPED = PAGE + 0x3C0;   // MAPPED_SUBRESOURCE
    static constexpr GuestAddr CODE = PAGE + 0x1000;    // shader bytecode
    static constexpr u32 kDoNotWait = 0x100000;         // D3D11_MAP_FLAG

    void SetUp() override {
        D3D11SurfaceTest::SetUp();
        ASSERT_TRUE(memory.map_region(PAGE, 0x4000,
                                      MemoryProtection::ReadWrite).is_ok());
    }
    /// CreateBuffer({64, DEFAULT, bind, cpu, misc, stride}), initial data
    /// words 0x11110000 + i when @p init. Returns the HRESULT; the buffer is
    /// at OUT.
    u32 create_default_buffer(u32 bind, u32 cpu_access, u32 misc = 0,
                              u32 stride = 0, bool init = true, u32 bytes = 64,
                              GuestAddr on_device = 0) {
        write_words(DESC, {bytes, 0, bind, cpu_access, misc, stride});
        for (u32 i = 0; i < bytes / 4; ++i) {
            EXPECT_TRUE(memory.write_u32(BYTES + 4 * i, 0x11110000u + i).is_ok());
        }
        EXPECT_TRUE(memory.write_u64(INIT, BYTES).is_ok());
        EXPECT_TRUE(memory.write_u64(INIT + 8, 0).is_ok());
        EXPECT_TRUE(memory.write_u64(OUT, 0).is_ok());
        cpu.gpr.rsp = STACK_BASE + 0x800;
        set_arg(0, on_device != 0 ? on_device : device);
        set_arg(1, DESC);
        set_arg(2, init ? INIT : 0);
        set_arg(3, OUT);
        d3d11.impl_DeviceMethod(marshaler, 3);
        return static_cast<u32>(cpu.gpr.rax);
    }
    /// Map(resource, 0, type, flags, MAPPED) on @p on_context.
    u32 map(GuestAddr resource, u32 type, u32 flags = 0, u32 sub = 0,
            GuestAddr on_context = 0) {
        EXPECT_TRUE(memory.write_u64(MAPPED, POISON).is_ok());
        cpu.gpr.rsp = STACK_BASE + 0x800;
        set_arg(0, on_context != 0 ? on_context : context);
        set_arg(1, resource);
        set_arg(2, sub);
        set_arg(3, type);
        set_arg(4, flags);
        set_arg(5, MAPPED);
        d3d11.impl_ContextMethod(marshaler, 14);
        return static_cast<u32>(cpu.gpr.rax);
    }
    void unmap(GuestAddr resource) { context_call(15, {resource, 0}); }
    [[nodiscard]] GuestAddr mapped_data() {
        return memory.read_u64(MAPPED).value();
    }
    [[nodiscard]] u32 feature(u32 id, u32 size, GuestAddr at) {
        for (u32 i = 0; i < 0x80; i += 4) {
            EXPECT_TRUE(memory.write_u32(at + i, 0xCDCDCDCDu).is_ok());
        }
        if (id == 2 || id == 3) EXPECT_TRUE(memory.write_u32(at, 28).is_ok());
        return device_call(33, {id, at, size});
    }
    [[nodiscard]] u32 word(GuestAddr at) { return memory.read_u32(at).value(); }
};

// "sizes": every feature is answered at its structure's size only, a refusal
// leaves the buffer untouched; D3D11_OPTIONS2 is also taken at 36 bytes,
// with a ninth member of 0.
TEST_F(D3D11CapsmallTest, FeatureSizesAreExactExceptOptions2) {
    const u32 sizes[] = {8, 4, 8, 8, 4, 56, 4, 4, 8, 4, 16,
                         4, 4, 16, 32, 4, 8, 4, 4, 4, 8};
    const GuestAddr at = PAGE + 0x400;
    for (u32 f = 0; f < std::size(sizes); ++f) {
        const u32 first = (f == 2 || f == 3) ? 4u : 0u;
        for (const u32 size : {0u, sizes[f] - 1, sizes[f] + 1, sizes[f] + 4,
                               sizes[f] + 8}) {
            if (f == 14 && size == 36) continue;
            EXPECT_EQ(feature(f, size, at), d3d11::E_INVALIDARG)
                << "feature " << f << " size " << size;
            EXPECT_EQ(word(at + first), 0xCDCDCDCDu)
                << "feature " << f << " size " << size << ": untouched";
        }
        EXPECT_EQ(feature(f, sizes[f], at), d3d11::S_OK) << "feature " << f;
    }
    // "f14 size 36 accepted: bytes ... 00 00 00 00 cd cd cd cd".
    ASSERT_EQ(feature(14, 36, at), d3d11::S_OK);
    EXPECT_EQ(word(at + 4), 1u) << "TypedUAVLoadAdditionalFormats";
    EXPECT_EQ(word(at + 32), 0u) << "the ninth member";
    EXPECT_EQ(word(at + 36), 0xCDCDCDCDu);
    EXPECT_EQ(feature(22, 4, at), d3d11::E_INVALIDARG);
}

// "values" FL11_0: feature 8 = 3,3; feature 10 members 2 and 3 = 1; feature
// 12 = 1. Feature 14 member 7 (UnifiedMemoryArchitecture) is not WARP's 1:
// the adapter profile policy (.docs/contracts/dxgi.md) answers as a discrete
// adapter, whatever the host is (Options2MatchesTheDiscreteAdapterRow).
TEST_F(D3D11CapsmallTest, OptionalCapsAtFeatureLevel11) {
    const GuestAddr at = PAGE + 0x400;
    ASSERT_EQ(feature(8, 8, at), d3d11::S_OK);
    EXPECT_EQ(word(at), 3u) << "PixelShaderMinPrecision 10_BIT|16_BIT";
    EXPECT_EQ(word(at + 4), 3u) << "AllOtherShaderStagesMinPrecision";
    ASSERT_EQ(feature(10, 16, at), d3d11::S_OK);
    EXPECT_EQ(word(at + 8), 1u) << "ClearViewAlsoSupportsDepthOnlyFormats";
    EXPECT_EQ(word(at + 12), 1u) << "MapOnDefaultBuffers";
    ASSERT_EQ(feature(12, 4, at), d3d11::S_OK);
    EXPECT_EQ(word(at), 1u) << "MARKER_SUPPORT.Profile";
    // A backend whose command stream takes no markers profiles nothing.
    backend.state.debug_markers_supported = false;
    ASSERT_EQ(feature(12, 4, at), d3d11::S_OK);
    EXPECT_EQ(word(at), 0u);
    backend.state.debug_markers_supported = true;
}

// D3D11_FEATURE_DATA_D3D11_OPTIONS2 at FL 11_0 against a real discrete
// adapter: the RTX 4080 SUPER row of the d3d11_a01 caps probe (user's
// Windows 11 PC, 2026-10-20), "feature 14 size 32: hr=0x00000000
// data=0,1,0,0,1,1,0,0" -- PSSpecifiedStencilRefSupported,
// TypedUAVLoadAdditionalFormats, ROVsSupported, ConservativeRasterizationTier,
// TiledResourcesTier, MapOnDefaultTextures, StandardSwizzle,
// UnifiedMemoryArchitecture. Two members differ on purpose: member 0 is 1
// on a backend that exports the shader stencil (the SV_StencilRef path is
// built, .docs/contracts/d3d11.md "D3D11_OPTIONS2.PSSpecifiedStencilRef
// Supported"), and member 6 is 1 because the standard-swizzle layouts are
// implemented (WARP reports 1, d3d11_texmap_probe; the NVIDIA driver does
// not). UnifiedMemoryArchitecture is 0 whether or not the host GPU has
// unified memory: the adapter profile policy (.docs/contracts/dxgi.md).
TEST_F(D3D11CapsmallTest, Options2MatchesTheDiscreteAdapterRow) {
    const GuestAddr at = PAGE + 0x400;
    constexpr std::array<u32, 8> kRtx4080Super{0, 1, 0, 0, 1, 1, 0, 0};
    // A backend with sparse textures, as Metal on Apple silicon: tier 1
    // (D3D11TiledTest).
    backend.state.tiled_resources = true;
    for (const bool stencil_export : {false, true}) {
        for (const bool unified : {true, false}) {
            backend.state.shader_stencil_export = stencil_export;
            backend.state.unified_memory = unified;
            ASSERT_EQ(feature(14, 32, at), d3d11::S_OK);
            std::array<u32, 8> expected = kRtx4080Super;
            expected[0] = stencil_export ? 1u : 0u;  // Vitreo: stencil ref
            expected[6] = 1u;                         // Vitreo: standard swizzle
            for (u32 member = 0; member < 8; ++member) {
                EXPECT_EQ(word(at + member * 4), expected[member])
                    << "member " << member << " stencil_export "
                    << stencil_export << " unified host " << unified;
            }
        }
    }
    backend.state.shader_stencil_export = false;
    backend.state.unified_memory = false;
    backend.state.tiled_resources = false;
}

// "values" FL10_0: MapOnDefaultBuffers 0 ("at least D3D_FEATURE_LEVEL_11_0",
// Learn) and "mapdef at lower feature levels": every CPU access refused;
// ClearViewAlsoSupportsDepthOnlyFormats, min precision and markers as at
// 11_0.
TEST_F(D3D11CapsmallTest, MapOnDefaultBuffersNeedsFeatureLevel11) {
    ASSERT_TRUE(memory.write_u32(LEVELS, d3d11::FEATURE_LEVEL_10_0).is_ok());
    prepare_create();
    set_arg(4, LEVELS);
    set_arg(5, 1);
    d3d11.impl_D3D11CreateDevice(marshaler);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr device10 = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr at = PAGE + 0x400;
    const auto feature10 = [&](u32 id, u32 size) {
        for (u32 i = 0; i < 0x40; i += 4) {
            EXPECT_TRUE(memory.write_u32(at + i, 0xCDCDCDCDu).is_ok());
        }
        set_arg(0, device10);
        set_arg(1, id);
        set_arg(2, at);
        set_arg(3, size);
        d3d11.impl_DeviceMethod(marshaler, 33);
        return static_cast<u32>(cpu.gpr.rax);
    };
    ASSERT_EQ(feature10(10, 16), d3d11::S_OK);
    EXPECT_EQ(word(at + 8), 1u);
    EXPECT_EQ(word(at + 12), 0u);
    ASSERT_EQ(feature10(8, 8), d3d11::S_OK);
    EXPECT_EQ(word(at), 3u);
    // OPTIONS2 UnifiedMemoryArchitecture is 0 at this level too, on a
    // unified-memory host (adapter profile policy, .docs/contracts/dxgi.md).
    backend.state.unified_memory = true;
    ASSERT_EQ(feature10(14, 32), d3d11::S_OK);
    EXPECT_EQ(word(at + 28), 0u) << "UnifiedMemoryArchitecture at FL 10_0";
    backend.state.unified_memory = false;
    for (const u32 cpu_access : {0x10000u, 0x20000u, 0x30000u}) {
        EXPECT_EQ(create_default_buffer(0x8, cpu_access, 0, 0, true, 64,
                                        device10),
                  d3d11::E_INVALIDARG);
    }
}

// "mapdef" FL11_0 creation: SRV, UAV or SRV|UAV and nothing else (not even
// the binds other usages ignore); misc 0, RAW or STRUCTURED only.
TEST_F(D3D11CapsmallTest, MappableDefaultBufferCreationRules) {
    struct Misc { u32 misc, stride; bool ok; };
    const Misc miscs[] = {{0, 0, true},       {0x20, 0, true},
                          {0x40, 4, true},    {0x10, 0, false},
                          {0x30, 0, false},   {0x2, 0, false},
                          {0x100, 0, false},  {0x80, 0, false},
                          {0x8000, 0, false}, {0x200, 0, false}};
    const u32 good_binds[] = {0x8, 0x80, 0x88};
    const u32 bad_binds[] = {0, 0x108, 0x208, 0x408, 0x9, 0x82, 0x4, 0x10, 0x20};
    for (const u32 cpu_access : {0x10000u, 0x20000u, 0x30000u}) {
        for (const u32 bind : good_binds) {
            for (const Misc& m : miscs) {
                EXPECT_EQ(create_default_buffer(bind, cpu_access, m.misc,
                                                m.stride),
                          m.ok ? d3d11::S_OK : d3d11::E_INVALIDARG)
                    << "bind 0x" << std::hex << bind << " cpu 0x" << cpu_access
                    << " misc 0x" << m.misc;
            }
        }
        for (const u32 bind : bad_binds) {
            EXPECT_EQ(create_default_buffer(bind, cpu_access),
                      d3d11::E_INVALIDARG)
                << "bind 0x" << std::hex << bind;
        }
    }
    // "GetDesc: usage 0 bind 0x8 cpu 0x30000 misc 0x0".
    ASSERT_EQ(create_default_buffer(0x8, 0x30000), d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(OUT).value();
    set_arg(0, buffer);
    set_arg(1, OUT2);
    d3d11.impl_BufferMethod(marshaler, 10);
    EXPECT_EQ(word(OUT2 + 4), 0u);
    EXPECT_EQ(word(OUT2 + 8), 0x8u);
    EXPECT_EQ(word(OUT2 + 12), 0x30000u);
}

// "mapdef" Map rows: READ needs R, WRITE / WRITE_NO_OVERWRITE need W,
// READ_WRITE both, WRITE_DISCARD never; DO_NOT_WAIT with any of them; pitches
// 64; a refused Map leaves pData NULL; subresource 1 and a NULL pMapped are
// refused; so is every Map on a deferred context.
TEST_F(D3D11CapsmallTest, MappableDefaultBufferMapRules) {
    struct Row { u32 cpu; std::array<bool, 5> ok; };  // types 1..5
    const Row rows[] = {
        {0x10000, {false, true, false, false, true}},
        {0x20000, {true, false, false, false, false}},
        {0x30000, {true, true, true, false, true}},
    };
    for (const Row& row : rows) {
        ASSERT_EQ(create_default_buffer(0x8, row.cpu), d3d11::S_OK);
        const GuestAddr buffer = memory.read_u64(OUT).value();
        for (u32 type = 0; type <= 6; ++type) {
            for (const u32 flags : {0u, kDoNotWait}) {
                const bool ok = type >= 1 && type <= 5 && row.ok[type - 1];
                const u32 hr = map(buffer, type, flags);
                EXPECT_EQ(hr, ok ? d3d11::S_OK : d3d11::E_INVALIDARG)
                    << "cpu 0x" << std::hex << row.cpu << " type " << type
                    << " flags 0x" << flags;
                if (ok) {
                    EXPECT_EQ(word(MAPPED + 8), 64u) << "RowPitch";
                    EXPECT_EQ(word(MAPPED + 12), 64u) << "DepthPitch";
                    EXPECT_EQ(word(mapped_data()), 0x11110000u) << "first";
                    unmap(buffer);
                } else {
                    EXPECT_EQ(mapped_data(), 0u) << "pData NULL";
                }
            }
        }
        EXPECT_EQ(map(buffer, row.cpu == 0x20000 ? 1u : 2u, 0, 1),
                  d3d11::E_INVALIDARG) << "subresource 1";
        set_arg(0, context);
        set_arg(1, buffer);
        set_arg(2, 0);
        set_arg(3, row.cpu == 0x20000 ? 1u : 2u);
        set_arg(4, 0);
        set_arg(5, 0);
        d3d11.impl_ContextMethod(marshaler, 14);
        EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "pMapped NULL";
    }
    ASSERT_EQ(create_default_buffer(0x88, 0x30000, 0x20), d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(OUT).value();
    ASSERT_EQ(device_call(27, {0, OUT2}), d3d11::S_OK);  // CreateDeferredContext
    const GuestAddr deferred = memory.read_u64(OUT2).value();
    for (u32 type = 1; type <= 5; ++type) {
        EXPECT_EQ(map(buffer, type, 0, 0, deferred), d3d11::E_INVALIDARG)
            << "deferred Map(type " << type << ")";
    }
    set_arg(0, deferred);
    d3d11.impl_ContextMethod(marshaler, 2);
}

// "W-only Map(WRITE) exposes" / "after partial WRITE": the mapping is the
// buffer's current contents and unwritten bytes survive; "Map twice" gives
// the same pointer; "after UpdateSubresource box [8,16)": a Map after an
// update sees it.
TEST_F(D3D11CapsmallTest, MappableDefaultBufferContents) {
    ASSERT_EQ(create_default_buffer(0x8, 0x10000), d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(OUT).value();
    TestBackendBuffer* gpu = backend.state.created_buffers.back();
    ASSERT_EQ(map(buffer, 2), d3d11::S_OK);
    const GuestAddr data = mapped_data();
    for (u32 i = 0; i < 16; ++i) {
        EXPECT_EQ(word(data + 4 * i), 0x11110000u + i) << "exposed word " << i;
    }
    ASSERT_TRUE(memory.write_u32(data + 12, 0xAAAA0003u).is_ok());
    ASSERT_TRUE(memory.write_u32(data + 36, 0xAAAA0009u).is_ok());
    ASSERT_EQ(map(buffer, 2), d3d11::S_OK);
    EXPECT_EQ(mapped_data(), data) << "Map twice: the same pointer";
    unmap(buffer);
    unmap(buffer);  // not mapped: harmless
    for (u32 i = 0; i < 16; ++i) {
        const u32 expected = i == 3   ? 0xAAAA0003u
                             : i == 9 ? 0xAAAA0009u
                                      : 0x11110000u + i;
        EXPECT_EQ(*gpu->live_words(4 * i), expected) << "word " << i;
    }

    ASSERT_EQ(create_default_buffer(0x8, 0x30000), d3d11::S_OK);
    const GuestAddr rw = memory.read_u64(OUT).value();
    ASSERT_EQ(map(rw, 3), d3d11::S_OK);  // pages allocated and filled
    unmap(rw);
    write_words(RECTS, {0x88880000u, 0x88880001u});
    write_words(DESC, {8, 0, 0, 16, 1, 1});  // D3D11_BOX [8, 16)
    context_call(48, {rw, 0, DESC, RECTS, 0, 0});  // UpdateSubresource
    ASSERT_EQ(map(rw, 1), d3d11::S_OK);
    const std::array<u32, 6> expected{0x11110000u, 0x11110001u, 0x88880000u,
                                      0x88880001u, 0x11110004u, 0x11110005u};
    for (u32 i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(word(mapped_data() + 4 * i), expected[i]) << "word " << i;
    }
    unmap(rw);
}

// "CS-written words", "exposed words 0..7", "after WRITE of words 5, 6": a
// dispatch writing the buffer through a UAV makes the next Map -- READ or
// WRITE -- show the GPU's contents, and a write of the value the CPU last
// saw over a GPU-written one lands. (The fake backend runs no shader; the
// test writes the buffer the way the dispatch would have.)
TEST_F(D3D11CapsmallTest, MappableDefaultBufferSeesUavWrites) {
    backend.state.generations = true;  // a timeline Map can wait on
    ASSERT_EQ(create_default_buffer(0x88, 0x30000, 0x20), d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(OUT).value();
    TestBackendBuffer* gpu = backend.state.created_buffers.back();
    // Raw UAV: R32_TYPELESS, BUFFER, {FirstElement 0, NumElements 16, RAW}.
    write_words(DESC, {39, 1, 0, 16, 1});
    ASSERT_EQ(device_call(8, {buffer, DESC, OUT2}), d3d11::S_OK);
    const GuestAddr uav = memory.read_u64(OUT2).value();
    ASSERT_TRUE(memory.write_bytes(CODE, capsmall_blobs::g_cs_fill,
                                   sizeof(capsmall_blobs::g_cs_fill)).is_ok());
    ASSERT_EQ(device_call(18, {CODE, sizeof(capsmall_blobs::g_cs_fill), 0,
                               OUT2}),
              d3d11::S_OK);
    const GuestAddr shader = memory.read_u64(OUT2).value();
    context_call(69, {shader, 0, 0});  // CSSetShader
    ASSERT_TRUE(memory.write_u64(OUT2, uav).is_ok());
    context_call(68, {0, 1, OUT2, 0});  // CSSetUnorderedAccessViews

    // A GPU write the frontend was not told about stays invisible: the
    // shadow still answers.
    *gpu->live_words(0) = 0x44440000u;
    ASSERT_EQ(map(buffer, 1), d3d11::S_OK);
    EXPECT_EQ(word(mapped_data()), 0x11110000u);
    unmap(buffer);

    context_call(41, {1, 1, 1});  // Dispatch
    const u64 dispatched_in = backend.state.recording_gen;
    for (u32 i = 0; i < 16; ++i) *gpu->live_words(4 * i) = 0x44440000u + 3 * i;
    // "Map(READ, DO_NOT_WAIT) right after the dispatch: S_OK or
    // WAS_STILL_DRAWING" -- here the dispatch has not retired.
    EXPECT_EQ(map(buffer, 1, kDoNotWait), 0x887A000Au);
    ASSERT_EQ(map(buffer, 1), d3d11::S_OK);
    ASSERT_FALSE(backend.state.generation_waits.empty());
    EXPECT_EQ(backend.state.generation_waits.back(), dispatched_in)
        << "waits for the dispatch's submission";
    for (u32 i = 0; i < 16; ++i) {
        EXPECT_EQ(word(mapped_data() + 4 * i), 0x44440000u + 3 * i) << i;
    }
    unmap(buffer);

    context_call(41, {1, 1, 1});
    for (u32 i = 0; i < 16; ++i) *gpu->live_words(4 * i) = 0x55550000u + i;
    ASSERT_EQ(map(buffer, 2), d3d11::S_OK);  // WRITE
    const GuestAddr data = mapped_data();
    for (u32 i = 0; i < 8; ++i) {
        EXPECT_EQ(word(data + 4 * i), 0x55550000u + i) << "exposed " << i;
    }
    // Word 5 back to what the CPU last saw (0x44440000 + 15), word 6 new.
    ASSERT_TRUE(memory.write_u32(data + 20, 0x4444000Fu).is_ok());
    ASSERT_TRUE(memory.write_u32(data + 24, 0x66660006u).is_ok());
    unmap(buffer);
    EXPECT_EQ(*gpu->live_words(16), 0x55550004u);
    EXPECT_EQ(*gpu->live_words(20), 0x4444000Fu);
    EXPECT_EQ(*gpu->live_words(24), 0x66660006u);
    EXPECT_EQ(*gpu->live_words(28), 0x55550007u);
    backend.state.generations = false;
}

// "clearview": ClearView on depth-stencil views of every depth format --
// depth from Color[0] (D16 / D24 UNORM, D32 stored as given), stencil from
// Color[1] (round to nearest, clamped); rects as on render targets; NULL
// rects with a count dropped.
TEST_F(D3D11CapsmallTest, ClearViewOnDepthStencilViews) {
    const auto dsv_of = [&](u32 format, u32 samples = 1, u32 slices = 1) {
        write_words(DESC, {4, 2, 1, slices, format, samples, 0, 0, 0x40, 0, 0});
        EXPECT_EQ(device_call(5, {DESC, 0, OUT}), d3d11::S_OK);
        const GuestAddr texture = memory.read_u64(OUT).value();
        EXPECT_EQ(device_call(10, {texture, 0, OUT2}), d3d11::S_OK);
        return memory.read_u64(OUT2).value();
    };
    const auto clear = [&](GuestAddr view, float depth, float green,
                           GuestAddr rects = 0, u32 count = 0) {
        const std::array<float, 4> color{depth, green, 0.25f, 0.125f};
        EXPECT_TRUE(memory.write_bytes(
            COLOR4, reinterpret_cast<const Byte*>(color.data()), 16).is_ok());
        context_call(132, {view, COLOR4, rects, count});
    };
    const auto bits = [](float f) { return std::bit_cast<u32>(f); };
    auto& fills = backend.state.depth_fills;

    // In range, whole view: a render-pass clear of depth only.
    const GuestAddr d32 = dsv_of(40);
    const unsigned passes = backend.state.depth_clears;
    clear(d32, 0.75f, 0.5f);
    EXPECT_EQ(backend.state.depth_clears, passes + 1);
    EXPECT_EQ(backend.state.last_depth_clear_value, 0.75f);
    EXPECT_TRUE(backend.state.last_depth_clear_depth);
    EXPECT_FALSE(backend.state.last_depth_clear_stencil);
    EXPECT_TRUE(fills.empty());

    // "D32 whole 1.5 / NaN / -0 / -1e-40": stored as given, denormals
    // flushed with their sign; 1e-40 becomes +0 (a pass clear).
    const std::pair<float, u32> stored[] = {
        {1.5f, 0x3FC00000u}, {-0.5f, 0xBF000000u}, {NAN, 0x7FC00000u},
        {INFINITY, 0x7F800000u}, {-0.0f, 0x80000000u},
        {-1e-40f, 0x80000000u}};
    for (const auto& [value, expected] : stored) {
        fills.clear();
        clear(d32, value, 0.5f);
        ASSERT_EQ(fills.size(), 1u) << value;
        EXPECT_EQ(fills[0], (std::array<u32, 9>{expected, ~0u, 0, 0, 1, 0, 0,
                                                 4, 2}))
            << value;
    }
    fills.clear();
    clear(d32, 1e-40f, 0.5f);
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(bits(backend.state.last_depth_clear_value), 0u);

    // "D32 rect (1,0)-(3,1)", "two rects", "(-5,-5)-(2,1)", "(0,0)-(100,100)",
    // "empty", "inverted", "outside", "NULL rects count 2".
    write_words(RECTS, {1, 0, 3, 1,  0, 1, 1, 2,  3, 0, 4, 2,
                        static_cast<u32>(-5), static_cast<u32>(-5), 2, 1,
                        0, 0, 100, 100,  2, 0, 2, 2,  3, 2, 1, 0,
                        10, 10, 20, 20});
    fills.clear();
    clear(d32, 0.75f, 0.5f, RECTS, 1);
    clear(d32, 0.75f, 0.5f, RECTS + 16, 2);
    clear(d32, 0.75f, 0.5f, RECTS + 48, 1);
    clear(d32, 0.75f, 0.5f, RECTS + 64, 1);
    clear(d32, 0.75f, 0.5f, RECTS + 80, 3);   // empty, inverted, outside
    clear(d32, 0.75f, 0.5f, 0, 2);            // dropped
    const u32 v = bits(0.75f);
    ASSERT_EQ(fills.size(), 5u);
    EXPECT_EQ(fills[0], (std::array<u32, 9>{v, ~0u, 0, 0, 1, 1, 0, 2, 1}));
    EXPECT_EQ(fills[1], (std::array<u32, 9>{v, ~0u, 0, 0, 1, 0, 1, 1, 1}));
    EXPECT_EQ(fills[2], (std::array<u32, 9>{v, ~0u, 0, 0, 1, 3, 0, 1, 2}));
    EXPECT_EQ(fills[3], (std::array<u32, 9>{v, ~0u, 0, 0, 1, 0, 0, 2, 1}));
    EXPECT_EQ(fills[4], (std::array<u32, 9>{v, ~0u, 0, 0, 1, 0, 0, 4, 2}));

    // "D16 whole 0.3 / rect 0.75": FLOAT -> UNORM16 in float, round to
    // nearest even (0x4CCC, 0xBFFF).
    const GuestAddr d16 = dsv_of(55);
    clear(d16, 0.3f, 0.5f);
    EXPECT_EQ(static_cast<u32>(std::lround(
                  backend.state.last_depth_clear_value * 65535.0f - 0.25f)),
              0x4CCCu);
    fills.clear();
    clear(d16, 0.75f, 0.5f, RECTS, 1);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0][0], 0xBFFFu);

    // "D24S8 whole 0.75 green 7.9": stencil 8, depth code 0xBFFFFF, both
    // planes cleared; "green 300" 255, "green -3" 0, "0.5" 0.
    const GuestAddr d24 = dsv_of(45);
    clear(d24, 0.75f, 7.9f);
    EXPECT_TRUE(backend.state.last_depth_clear_stencil);
    EXPECT_EQ(backend.state.last_stencil_clear_value, 8u);
    EXPECT_EQ(std::lrint(static_cast<double>(
                  backend.state.last_depth_clear_value) * 16777215.0),
              0xBFFFFF);
    for (const auto& [green, stencil] :
         {std::pair{300.0f, 255u}, std::pair{-3.0f, 0u}, std::pair{0.5f, 0u},
          std::pair{7.0f, 7u}}) {
        clear(d24, 0.75f, green);
        EXPECT_EQ(backend.state.last_stencil_clear_value, stencil) << green;
    }
    // "D24S8 rect 0.75" and "D32S8 whole 1.5": both planes through fills.
    fills.clear();
    clear(d24, 0.75f, 0.5f, RECTS, 1);
    const GuestAddr d32s8 = dsv_of(20);
    clear(d32s8, 1.5f, 300.0f);
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(std::lrint(static_cast<double>(std::bit_cast<float>(fills[0][0])) *
                         16777215.0),
              0xBFFFFF);
    EXPECT_EQ(fills[0][1], 0u);
    EXPECT_EQ(fills[1][0], 0x3FC00000u);
    EXPECT_EQ(fills[1][1], 255u);

    // Array slices 1..2 of 3 with a rect.
    write_words(DESC, {4, 2, 1, 3, 40, 1, 0, 0, 0x40, 0, 0});
    ASSERT_EQ(device_call(5, {DESC, 0, OUT}), d3d11::S_OK);
    const GuestAddr array = memory.read_u64(OUT).value();
    write_words(DESC + 0x40, {40, 4 /* TEXTURE2DARRAY */, 0, 0, 1, 2});
    ASSERT_EQ(device_call(10, {array, DESC + 0x40, OUT2}), d3d11::S_OK);
    const GuestAddr part = memory.read_u64(OUT2).value();
    fills.clear();
    write_words(RECTS + 0x100, {0, 0, 2, 1});
    clear(part, 0.75f, 0.5f, RECTS + 0x100, 1);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0], (std::array<u32, 9>{v, ~0u, 0, 1, 2, 0, 0, 2, 1}));

    // Multisampled: a whole in-range view is a pass clear; a rect has no
    // implementation here (fails loudly, nothing written).
    const GuestAddr msaa = dsv_of(40, 4);
    const unsigned before = backend.state.depth_clears;
    clear(msaa, 0.75f, 0.5f);
    EXPECT_EQ(backend.state.depth_clears, before + 1);
    fills.clear();
    clear(msaa, 0.75f, 0.5f, RECTS, 1);
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(backend.state.depth_clears, before + 1);

    // A render target with NULL rects and a count: dropped too.
    const GuestAddr tex = texture(1, 0x20);
    ASSERT_EQ(device_call(9, {tex, 0, OUT}), d3d11::S_OK);
    const GuestAddr rtv = memory.read_u64(OUT).value();
    const unsigned colour_clears = backend.state.texture_clears;
    clear(rtv, 1.0f, 1.0f, 0, 2);
    EXPECT_EQ(backend.state.texture_clears, colour_clears);
    clear(rtv, 1.0f, 1.0f, 0, 0);
    EXPECT_EQ(backend.state.texture_clears, colour_clears + 1);
}

// "deferred rect before/after execute": recorded, applied at execution.
TEST_F(D3D11CapsmallTest, ClearViewOnADepthViewFromADeferredContext) {
    write_words(DESC, {4, 2, 1, 1, 40, 1, 0, 0, 0x40, 0, 0});
    ASSERT_EQ(device_call(5, {DESC, 0, OUT}), d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(OUT).value();
    ASSERT_EQ(device_call(10, {texture, 0, OUT2}), d3d11::S_OK);
    const GuestAddr dsv = memory.read_u64(OUT2).value();
    ASSERT_EQ(device_call(27, {0, OUT2}), d3d11::S_OK);
    const GuestAddr deferred = memory.read_u64(OUT2).value();
    write_words(RECTS, {1, 0, 3, 1});
    const std::array<float, 4> color{0.75f, 0.5f, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        COLOR4, reinterpret_cast<const Byte*>(color.data()), 16).is_ok());
    backend.state.depth_fills.clear();
    set_arg(0, deferred);
    set_arg(1, dsv);
    set_arg(2, COLOR4);
    set_arg(3, RECTS);
    set_arg(4, 1);
    d3d11.impl_ContextMethod(marshaler, 132);
    EXPECT_TRUE(backend.state.depth_fills.empty()) << "before execute";
    set_arg(0, deferred);
    set_arg(1, 0);
    set_arg(2, OUT2);
    d3d11.impl_ContextMethod(marshaler, 114);  // FinishCommandList
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr list = memory.read_u64(OUT2).value();
    context_call(58, {list, 0});  // ExecuteCommandList
    ASSERT_EQ(backend.state.depth_fills.size(), 1u);
    EXPECT_EQ(backend.state.depth_fills[0][5], 1u);
    EXPECT_EQ(backend.state.depth_fills[0][7], 2u);
}

// "marker": what the application sees is WARP's with no profiling tool
// (BeginEvent / EndEvent -1, GetStatus and IsAnnotationEnabled FALSE,
// unbalanced EndEvents harmless); the markers themselves reach the backend's
// command stream in order, a deferred context's when its list executes.
TEST_F(D3D11CapsmallTest, MarkersReachTheCommandStream) {
    const auto wide = [&](GuestAddr at, const char* text) {
        u32 i = 0;
        for (; text[i] != 0; ++i) {
            ASSERT_TRUE(memory.write_u16(at + 2 * i, static_cast<u16>(text[i]))
                            .is_ok());
        }
        ASSERT_TRUE(memory.write_u16(at + 2 * i, 0).is_ok());
    };
    wide(RECTS, "outer");
    wide(RECTS + 0x40, "marker");
    const auto annotation_of = [&](GuestAddr ctx) {
        EXPECT_TRUE(memory.write_bytes(
            DESC + 0x100,
            reinterpret_cast<const Byte*>(&IID_ID3DUSERDEFINEDANNOTATION),
            16).is_ok());
        set_arg(0, ctx);
        set_arg(1, DESC + 0x100);
        set_arg(2, OUT2);
        d3d11.impl_ContextMethod(marshaler, 0);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(OUT2).value();
    };
    const auto annotation = [&](GuestAddr facet, usize slot, GuestAddr name) {
        set_arg(0, facet);
        set_arg(1, name);
        d3d11.impl_AnnotationMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    };
    auto& markers = backend.state.debug_markers;
    markers.clear();
    const GuestAddr facet = annotation_of(context);
    EXPECT_EQ(annotation(facet, 6, 0), 0u) << "GetStatus";
    EXPECT_EQ(annotation(facet, 3, RECTS), 0xFFFFFFFFu) << "BeginEvent";
    EXPECT_EQ(annotation(facet, 5, RECTS + 0x40), 0u) << "SetMarker";
    EXPECT_EQ(annotation(facet, 4, 0), 0xFFFFFFFFu) << "EndEvent";
    EXPECT_EQ(annotation(facet, 4, 0), 0xFFFFFFFFu) << "unbalanced";
    EXPECT_EQ(annotation(facet, 3, 0), 0xFFFFFFFFu) << "NULL name";
    context_call(140, {});
    EXPECT_EQ(cpu.gpr.rax, 0u) << "IsAnnotationEnabled";
    context_call(142, {RECTS, 1});   // BeginEventInt
    context_call(141, {RECTS + 0x40, static_cast<u64>(-2)});  // SetMarkerInt
    context_call(143, {});           // EndEvent
    context_call(141, {0, 3});
    EXPECT_EQ(markers, (std::vector<std::string>{
                           "push:outer", "sign:marker", "pop", "pop", "push:",
                           "push:outer (1)", "sign:marker (-2)", "pop",
                           "sign:(3)"}));

    ASSERT_EQ(device_call(27, {0, OUT}), d3d11::S_OK);
    const GuestAddr deferred = memory.read_u64(OUT).value();
    const GuestAddr deferred_facet = annotation_of(deferred);
    markers.clear();
    EXPECT_EQ(annotation(deferred_facet, 3, RECTS), 0xFFFFFFFFu);
    set_arg(0, deferred);
    set_arg(1, RECTS + 0x40);
    set_arg(2, 7);
    d3d11.impl_ContextMethod(marshaler, 141);
    EXPECT_EQ(annotation(deferred_facet, 4, 0), 0xFFFFFFFFu);
    EXPECT_TRUE(markers.empty()) << "recorded, not emitted";
    set_arg(0, deferred);
    set_arg(1, 0);
    set_arg(2, OUT2);
    d3d11.impl_ContextMethod(marshaler, 114);  // FinishCommandList
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    context_call(58, {memory.read_u64(OUT2).value(), 0});
    EXPECT_EQ(markers, (std::vector<std::string>{"push:outer",
                                                 "sign:marker (7)", "pop"}));
}

TEST_F(D3D11ManagerTest, RegisteredExportsCarryTheirInChainClass) {
    // L3: the in-chain class is registration metadata. create_d3d11_thunks
    // attaches each method's contract class (d3d11_in_chain.hpp), and
    // register_dll carries it into the direct table translated code reads.
    const auto exports = create_d3d11_thunks(d3d11);
    std::map<std::string, InChainClass> by_name;
    for (const ExportEntry& entry : exports.entries) {
        if (entry.direct.fn == nullptr) {
            EXPECT_EQ(entry.in_chain, InChainClass::None) << entry.name;
            continue;
        }
        by_name[entry.name] = entry.in_chain;
    }
    for (usize slot = 0; slot < d3d11::CONTEXT_METHOD_COUNT; ++slot) {
        const auto it = by_name.find(d3d11_context_method_export_name(slot));
        ASSERT_NE(it, by_name.end()) << slot;
        EXPECT_EQ(it->second, d3d11::context_method_in_chain_class(slot))
            << d3d11::CONTEXT_METHOD_NAMES[slot];
    }
    // ID3D11Device5: IUnknown only; creation methods stay on the full path.
    EXPECT_EQ(by_name.at(d3d11_device_method_export_name(1)), InChainClass::A);
    EXPECT_EQ(by_name.at(d3d11_device_method_export_name(2)),
              InChainClass::AGuestComRef);
    EXPECT_EQ(by_name.at(d3d11_device_method_export_name(3)),
              InChainClass::None);  // CreateBuffer
    // A device child (ID3D11Buffer): AddRef A, Release AGuestComRef, and its
    // interface-specific slots unclassified.
    EXPECT_EQ(by_name.at(d3d11_buffer_method_export_name(1)), InChainClass::A);
    EXPECT_EQ(by_name.at(d3d11_buffer_method_export_name(2)),
              InChainClass::AGuestComRef);
    EXPECT_EQ(by_name.at(d3d11_buffer_method_export_name(3)),
              InChainClass::None);

    ThunkRegistry registry(0x70000000);
    ASSERT_TRUE(registry.register_dll("d3d11.dll", create_d3d11_thunks(d3d11))
                    .is_ok());
    const auto direct_of = [&](usize context_slot) {
        const auto stub = registry.get_thunk_address(
            "d3d11.dll", d3d11_context_method_export_name(context_slot));
        EXPECT_TRUE(stub.is_ok()) << context_slot;
        return registry.direct_thunk(
            registry.dispatch_address_for_export(stub.value()));
    };
    const DirectThunk* map = direct_of(14);   // Map
    const DirectThunk* draw = direct_of(12);  // DrawIndexed
    const DirectThunk* bind = direct_of(8);   // PSSetShaderResources
    ASSERT_NE(map, nullptr);
    ASSERT_NE(draw, nullptr);
    ASSERT_NE(bind, nullptr);
    EXPECT_EQ(map->in_chain, InChainClass::None);
    EXPECT_EQ(draw->in_chain, InChainClass::A);
    EXPECT_EQ(bind->in_chain, InChainClass::AGuestComRef);

    // The slot map translated code tests: one byte per slab slot, set
    // exactly for the in-chain classes, for the stub and its gateway alike.
    const u8* slots = registry.in_chain_slot_map();
    u64 count = 0;
    std::memcpy(&count, slots, sizeof(count));
    const auto eligible = [&](usize context_slot) {
        const GuestAddr stub =
            registry
                .get_thunk_address("d3d11.dll",
                                   d3d11_context_method_export_name(context_slot))
                .value();
        const u64 index = (stub - 0x70000000u) >>
                          ThunkRegistry::kInChainSlotShift;
        const u64 gateway_index =
            (stub + ThunkRegistry::STUB_SIZE - 0x70000000u) >>
            ThunkRegistry::kInChainSlotShift;
        EXPECT_EQ(index, gateway_index);
        EXPECT_LT(index, count);
        return index < count ? slots[sizeof(u64) + index] : u8{0xFF};
    };
    EXPECT_EQ(eligible(14), 0u);  // Map
    EXPECT_EQ(eligible(12), 1u);  // DrawIndexed
    EXPECT_EQ(eligible(8), 1u);   // PSSetShaderResources
    // A registry with no in-chain export publishes the zero-slot map.
    ThunkRegistry empty(0x70000000);
    u64 empty_count = 1;
    std::memcpy(&empty_count, empty.in_chain_slot_map(), sizeof(empty_count));
    EXPECT_EQ(empty_count, 0u);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
