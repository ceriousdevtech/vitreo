// SPDX-License-Identifier: LGPL-2.1-only
//
// Swap-chain creation, sizes, present flags, buffers and resize rules.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_swapchain.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(DxgiSwapChainContractTest, CreateSwapChainValidatesLikeWindows) {
    GuestAddr chain = 0;
    // [swapchain.create] effect x count.
    struct EffectRow { u32 effect, count, hr; };
    for (const EffectRow row : {EffectRow{0, 0, kIC}, EffectRow{0, 16, 0},
                                EffectRow{0, 17, kIC}, EffectRow{1, 16, 0},
                                EffectRow{3, 1, kIC}, EffectRow{3, 2, 0},
                                EffectRow{3, 17, kIC}, EffectRow{4, 1, kIC},
                                EffectRow{2, 2, kIC}, EffectRow{5, 2, kIC}}) {
        Desc d;
        d.effect = row.effect;
        d.count = row.count;
        EXPECT_EQ(create(d, &chain), row.hr)
            << "effect " << row.effect << " count " << row.count;
        if (row.hr == 0) {
            (void)call(chain, 2, {});
        } else {
            EXPECT_EQ(chain, 0u) << "refused chains NULL *ppSwapChain";
        }
    }
    // Formats: bitblt set -> else E_INVALIDARG; flip set -> else INVALID_CALL.
    struct FormatRow { u32 format; u32 bitblt; u32 flip; };
    for (const FormatRow row :
         {FormatRow{28, 0, 0}, FormatRow{29, 0, kIC}, FormatRow{91, 0, kIC},
          FormatRow{24, 0, 0}, FormatRow{10, 0, 0},
          FormatRow{85, d3d11::E_INVALIDARG, kIC},
          FormatRow{2, d3d11::E_INVALIDARG, kIC},
          FormatRow{27, d3d11::E_INVALIDARG, kIC},
          FormatRow{0, d3d11::E_INVALIDARG, kIC},
          FormatRow{45, d3d11::E_INVALIDARG, kIC}}) {
        Desc d;
        d.format = row.format;
        EXPECT_EQ(create(d, &chain), row.bitblt) << "DISCARD fmt " << row.format;
        if (row.bitblt == 0) (void)call(chain, 2, {});
        d.effect = 4;
        d.count = 2;
        EXPECT_EQ(create(d, &chain), row.flip) << "FLIP fmt " << row.format;
        if (row.flip == 0) (void)call(chain, 2, {});
    }
    // Samples: bitblt MSAA is legal; quality past the levels and count 0 are
    // E_INVALIDARG; a flip chain refuses anything but 1.0 (INVALID_CALL).
    {
        Desc d;
        d.samples = 4;
        EXPECT_EQ(create(d, &chain), d3d11::S_OK) << "DISCARD samples=4.0";
        (void)call(chain, 2, {});
        d.quality = 1;
        EXPECT_EQ(create(d, &chain), d3d11::E_INVALIDARG);
        d.samples = 0;
        d.quality = 0;
        EXPECT_EQ(create(d, &chain), d3d11::E_INVALIDARG);
        d.samples = 4;
        d.effect = 4;
        d.count = 2;
        EXPECT_EQ(create(d, &chain), kIC);
    }
    // Usage: CPU access E_INVALIDARG (bitblt) / INVALID_CALL (flip); the flip
    // model refuses SHARED / READ_ONLY / DISCARD_ON_PRESENT.
    {
        Desc d;
        d.usage = 0x21;
        EXPECT_EQ(create(d, &chain), d3d11::E_INVALIDARG);
        d.effect = 4;
        d.count = 2;
        EXPECT_EQ(create(d, &chain), kIC);
        for (const u32 usage : {0xA0u, 0x120u, 0x220u}) {
            d.usage = usage;
            EXPECT_EQ(create(d, &chain), kIC) << "FLIP usage " << usage;
        }
        d.usage = 0;
        EXPECT_EQ(create(d, &chain), d3d11::S_OK) << "FLIP usage=0x0";
        (void)call(chain, 2, {});
    }
    // Flags ([swapchain.create] + [create.flags]).
    struct FlagRow { u32 flags; u32 bitblt; u32 flip; };
    for (const FlagRow row :
         {FlagRow{0x1, kIC, kIC}, FlagRow{0x2, 0, 0}, FlagRow{0x4, 0, 0},
          FlagRow{0x8, 0, 0}, FlagRow{0x10, 0, d3d11::E_INVALIDARG},
          FlagRow{0x20, kIC, kIC}, FlagRow{0x40, kIC, 0},
          FlagRow{0x80, kIC, kIC}, FlagRow{0x100, kIC, kIC},
          FlagRow{0x400, kIC, kIC}, FlagRow{0x800, kIC, 0},
          FlagRow{0x1000, 0, 0}, FlagRow{0x80000000u, kIC, kIC}}) {
        Desc d;
        d.flags = row.flags;
        EXPECT_EQ(create(d, &chain), row.bitblt) << "DISCARD flags " << row.flags;
        if (row.bitblt == 0) (void)call(chain, 2, {});
        d.effect = 4;
        d.count = 2;
        EXPECT_EQ(create(d, &chain), row.flip) << "FLIP flags " << row.flags;
        if (row.flip == 0) (void)call(chain, 2, {});
    }
    {
        Desc d;
        d.format = 28;
        d.flags = 0x4;
        EXPECT_EQ(create(d, &chain), d3d11::E_INVALIDARG)
            << "DISCARD RGBA GDI_COMPATIBLE";
    }
}

TEST_F(DxgiSwapChainContractTest, CreationSizesWindowAndFullscreenRules) {
    GuestAddr chain = 0;
    // Width or Height 0 takes BOTH from the client area.
    Desc d;
    d.width = 0;
    ASSERT_EQ(create(d, &chain), d3d11::S_OK);
    ASSERT_TRUE(call(chain, 12, {SCRATCH}) == d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(SCRATCH).value(), 200u);
    EXPECT_EQ(memory.read_u32(SCRATCH + 4).value(), 150u);
    (void)call(chain, 2, {});
    // Windowed = FALSE: created windowed, DXGI_STATUS_OCCLUDED.
    d = Desc{};
    d.windowed = 0;
    ASSERT_EQ(create(d, &chain), kOccluded);
    ASSERT_NE(chain, 0u);
    ASSERT_EQ(call(chain, 11, {OUT, 0}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OUT).value(), 0u);
    (void)call(chain, 2, {});
    // An HWND that is not a window: created, every Present occluded.
    d = Desc{};
    d.hwnd = kMissingWindow;
    ASSERT_EQ(create(d, &chain), d3d11::S_OK);
    EXPECT_EQ(call(chain, 8, {0, 0}), kOccluded);
    (void)call(chain, 2, {});
    d.width = 0;
    EXPECT_EQ(create(d, &chain), kIC) << "no client area to size from";
    // Past the Metal texture limit: E_OUTOFMEMORY (Windows' allocation
    // failure, measured on x86 ResizeBuffers), never a host abort.
    d = Desc{};
    d.width = d.height = 20000;
    EXPECT_EQ(create(d, &chain), kOutOfMemory);
    // NULL desc / NULL window / the context as the device: INVALID_CALL
    // with *ppSwapChain untouched.
    d = Desc{};
    d.hwnd = 0;
    EXPECT_EQ(create(d, &chain), kIC);
    EXPECT_EQ(chain, 0x1111u);
    d.hwnd = kWindow;
    EXPECT_EQ(create(d, &chain, context), kIC);
    EXPECT_EQ(chain, 0x1111u);
    // A second flip chain on one window is E_ACCESSDENIED; bitblt is not.
    d.effect = 4;
    d.count = 2;
    GuestAddr first = 0;
    ASSERT_EQ(create(d, &first), d3d11::S_OK);
    EXPECT_EQ(create(d, &chain), kAccessDenied);
    d.effect = 0;
    d.count = 1;
    ASSERT_EQ(create(d, &chain), d3d11::S_OK);
    (void)call(chain, 2, {});
    (void)call(first, 2, {});
}

TEST_F(DxgiSwapChainContractTest, HwndAndCompositionDesc1Rules) {
    GuestAddr chain = 0;
    Desc d;
    // Scaling: STRETCH everywhere, NONE only on a flip HWND chain.
    EXPECT_EQ(create1(d, 1, 0, false, &chain), kIC) << "DISCARD scaling=1";
    d.effect = 4;
    d.count = 2;
    ASSERT_EQ(create1(d, 1, 3, false, &chain), d3d11::S_OK);
    ASSERT_EQ(call(chain, 18, {SCRATCH}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(SCRATCH + 32).value(), 1u) << "Scaling echoed";
    EXPECT_EQ(memory.read_u32(SCRATCH + 40).value(), 3u) << "AlphaMode echoed";
    (void)call(chain, 2, {});
    EXPECT_EQ(create1(d, 2, 0, false, &chain), kIC) << "ASPECT_RATIO_STRETCH";
    for (const u32 alpha : {1u, 2u, 4u}) {
        EXPECT_EQ(create1(d, 0, alpha, false, &chain), kIC) << "alpha " << alpha;
    }
    // Stereo: no stereo presentation (Windows/WARP accepts; documented).
    EXPECT_EQ(create1(d, 0, 0, false, &chain, 1), kIC);
    // Width 0 on an HWND chain: the client area.
    d.width = 0;
    ASSERT_EQ(create1(d, 0, 0, false, &chain), d3d11::S_OK);
    ASSERT_EQ(call(chain, 18, {SCRATCH}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(SCRATCH).value(), 200u);
    (void)call(chain, 2, {});
    // Composition: FLIP_DISCARD is fine, count 1 / STRAIGHT / sRGB / NONE are
    // not, width 0 is refused with the out-parameter untouched.
    d = Desc{};
    d.effect = 4;
    d.count = 2;
    ASSERT_EQ(create1(d, 0, 1, true, &chain), d3d11::S_OK);
    ASSERT_EQ(call(chain, 18, {SCRATCH}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(SCRATCH + 40).value(), 1u);
    EXPECT_EQ(call(chain, 20, {OUT}), kIC) << "composition GetHwnd";
    EXPECT_EQ(memory.read_u64(OUT).value(), 0u);
    (void)call(chain, 2, {});
    d.count = 1;
    EXPECT_EQ(create1(d, 0, 0, true, &chain), kIC);
    d.count = 2;
    EXPECT_EQ(create1(d, 0, 2, true, &chain), kIC);
    EXPECT_EQ(create1(d, 1, 0, true, &chain), kIC);
    d.format = 29;
    EXPECT_EQ(create1(d, 0, 0, true, &chain), kIC);
    d.format = 87;
    d.width = 0;
    EXPECT_EQ(create1(d, 0, 0, true, &chain), kIC);
    EXPECT_EQ(chain, 0x1111u);
}

TEST_F(DxgiSwapChainContractTest, PresentFlagsAndSyncIntervalsAsOnWindows) {
    GuestAddr blt = 0;
    GuestAddr flip = 0;
    ASSERT_EQ(create(Desc{}, &blt), d3d11::S_OK);
    Desc fd;
    fd.effect = 4;
    fd.count = 2;
    fd.hwnd = 0xBEEF;
    ASSERT_EQ(create(fd, &flip), d3d11::S_OK);
    // SyncInterval 1..4 presents and paces (Learn: "1 through 4").
    for (u32 sync = 0; sync <= 4; ++sync) {
        EXPECT_EQ(call(blt, 8, {sync, 0}), d3d11::S_OK) << sync;
        EXPECT_EQ(backend.state.last_present_sync, sync);
    }
    EXPECT_EQ(call(blt, 8, {5, 0}), kIC);
    // TEST presents nothing and counts nothing.
    const unsigned shown = backend.state.presents;
    const u32 counted = present_count(blt);
    EXPECT_EQ(call(blt, 8, {0, 0x1}), d3d11::S_OK);
    EXPECT_EQ(call(blt, 8, {0, 0x9}), d3d11::S_OK) << "bitblt TEST|DO_NOT_WAIT";
    EXPECT_EQ(backend.state.presents, shown);
    EXPECT_EQ(present_count(blt), counted);
    // Stereo / restrict-to-output / tearing need what the chain lacks.
    for (const u32 flags : {0x10u, 0x20u, 0x40u, 0x200u}) {
        EXPECT_EQ(call(blt, 8, {0, flags}), kIC) << flags;
        EXPECT_EQ(call(flip, 8, {0, flags}), kIC) << flags;
    }
    // Unknown bits: ignored by bitblt, refused by flip.
    EXPECT_EQ(call(blt, 8, {0, 0x80}), d3d11::S_OK);
    EXPECT_EQ(call(flip, 8, {0, 0x80}), kIC);
    EXPECT_EQ(call(flip, 8, {0, 0x80000000u}), kIC);
    // Flip: TEST only alone; DO_NOT_SEQUENCE not with RESTART / DO_NOT_WAIT.
    for (const u32 flags : {0x3u, 0x5u, 0x9u, 0x101u, 0x6u, 0xAu}) {
        EXPECT_EQ(call(flip, 8, {0, flags}), kIC) << flags;
    }
    for (const u32 flags : {0x1u, 0x2u, 0x4u, 0xCu, 0x100u, 0x102u, 0x108u}) {
        EXPECT_EQ(call(flip, 8, {0, flags}), d3d11::S_OK) << flags;
    }
    // DO_NOT_WAIT with a full queue: WAS_STILL_DRAWING, nothing presented.
    backend.state.present_would_block = true;
    const u32 before = present_count(flip);
    EXPECT_EQ(call(flip, 8, {1, 0x8}), kStillDrawing);
    EXPECT_EQ(present_count(flip), before);
    backend.state.present_would_block = false;
    // ALLOW_TEARING with the chain flag and interval 0 only.
    GuestAddr tearing = 0;
    fd.flags = 0x800;
    fd.hwnd = 0xBEE0;
    ASSERT_EQ(create(fd, &tearing), d3d11::S_OK);
    EXPECT_EQ(call(tearing, 8, {0, 0x200}), d3d11::S_OK);
    EXPECT_EQ(call(tearing, 8, {1, 0x200}), kIC);
    EXPECT_EQ(call(tearing, 8, {0, 0x201}), kIC);
    EXPECT_EQ(call(tearing, 17, {0}), kIC) << "GetLastPresentCount(NULL)";
    (void)call(tearing, 2, {});
    (void)call(flip, 2, {});
    (void)call(blt, 2, {});
}

TEST_F(DxgiSwapChainContractTest, FlipPresentUnbindsBufferZero) {
    // Learn Present remarks + measured "RTV of buffer 0 still bound after
    // Present(0,0)": flip unbinds, bitblt does not.
    for (const u32 effect : {0u, 3u}) {
        Desc d;
        d.effect = effect;
        d.count = effect == 0 ? 1 : 2;
        d.hwnd = 0xC000 + effect;
        GuestAddr chain = 0;
        ASSERT_EQ(create(d, &chain), d3d11::S_OK);
        write_iid(winapi::IID_ID3D11TEXTURE2D);
        ASSERT_EQ(call(chain, 9, {0, IID, BUFFER_OUT}), d3d11::S_OK);
        const GuestAddr back = memory.read_u64(BUFFER_OUT).value();
        const GuestAddr rtv = make_rtv(back);
        bind_rtv(rtv);
        ASSERT_EQ(call(chain, 8, {0, 0}), d3d11::S_OK);
        const GuestAddr bound = bound_rtv();
        EXPECT_EQ(bound != 0, effect == 0) << "effect " << effect;
        release_view(bound);
        clear_state();
        release_view(rtv);
        release_texture(back);
        (void)call(chain, 2, {});
    }
}

TEST_F(DxgiSwapChainContractTest, GetBufferInterfacesIndexesAndUsage) {
    GuestAddr blt = 0;
    ASSERT_EQ(create(Desc{}, &blt), d3d11::S_OK);
    // "GetBuffer(0, X)": the texture's DXGI facets and Texture2D1 answer;
    // an IID the texture lacks is E_NOINTERFACE with *ppSurface NULL; a NULL
    // ppSurface is INVALID_CALL.
    for (const GuidBytes& iid :
         {winapi::IID_IDXGISURFACE, winapi::IID_IDXGISURFACE1, winapi::IID_IDXGIRESOURCE,
          winapi::IID_IDXGIRESOURCE1, winapi::IID_ID3D11TEXTURE2D1, winapi::IID_IUNKNOWN}) {
        write_iid(iid);
        ASSERT_TRUE(memory.write_u64(OUT, 1).is_ok());
        EXPECT_EQ(call(blt, 9, {0, IID, OUT}), d3d11::S_OK);
        const GuestAddr got = memory.read_u64(OUT).value();
        EXPECT_NE(got, 0u);
        // Every facet shares the texture's reference count.
        set_arg(0, got);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        if (guid_equal(iid, winapi::IID_IDXGISURFACE) ||
            guid_equal(iid, winapi::IID_IDXGISURFACE1)) {
            d3d11.impl_DxgiSurfaceMethod(marshaler, 2);
        } else if (guid_equal(iid, winapi::IID_IDXGIRESOURCE) ||
                   guid_equal(iid, winapi::IID_IDXGIRESOURCE1)) {
            d3d11.impl_DxgiResourceMethod(marshaler, 2);
        } else {
            d3d11.impl_Texture2DMethod(marshaler, 2);
        }
    }
    for (const GuidBytes& iid : {winapi::IID_ID3D11BUFFER, winapi::IID_ID3D11DEVICE}) {
        write_iid(iid);
        ASSERT_TRUE(memory.write_u64(OUT, 1).is_ok());
        EXPECT_EQ(call(blt, 9, {0, IID, OUT}), d3d11::E_NOINTERFACE);
        EXPECT_EQ(memory.read_u64(OUT).value(), 0u);
    }
    write_iid(winapi::IID_ID3D11TEXTURE2D);
    EXPECT_EQ(call(blt, 9, {0, IID, 0}), kIC);
    EXPECT_EQ(call(blt, 9, {1, IID, OUT}), kIC) << "DISCARD has no buffer 1";
    // Back-buffer usage: RTO | BACK_BUFFER | DISCARD_ON_PRESENT (0x260);
    // its surface's parent is the chain.
    write_iid(winapi::IID_IDXGIRESOURCE);
    ASSERT_EQ(call(blt, 9, {0, IID, OUT}), d3d11::S_OK);
    const GuestAddr resource = memory.read_u64(OUT).value();
    set_arg(0, resource);
    set_arg(1, OUT2);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_DxgiResourceMethod(marshaler, 9);
    EXPECT_EQ(memory.read_u32(OUT2).value(), 0x260u);
    d3d11.impl_DxgiResourceMethod(marshaler, 2);
    write_iid(winapi::IID_IDXGISURFACE1);
    ASSERT_EQ(call(blt, 9, {0, IID, OUT}), d3d11::S_OK);
    const GuestAddr surface = memory.read_u64(OUT).value();
    write_iid(winapi::IID_IDXGISWAPCHAIN);
    set_arg(0, surface);
    set_arg(1, IID);
    set_arg(2, OUT2);
    d3d11.impl_DxgiSurfaceMethod(marshaler, 6);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(OUT2).value(), blt);
    (void)call(blt, 2, {});
    set_arg(0, surface);
    d3d11.impl_DxgiSurfaceMethod(marshaler, 2);
    (void)call(blt, 2, {});

    // SEQUENTIAL / FLIP_SEQUENTIAL x2: buffer 1 exists, is read-only
    // (usage 0x160), buffer 2 does not; FLIP_DISCARD exposes only buffer 0.
    for (const u32 effect : {1u, 3u, 4u}) {
        Desc d;
        d.effect = effect;
        d.count = 2;
        d.hwnd = 0xD000 + effect;
        GuestAddr chain = 0;
        ASSERT_EQ(create(d, &chain), d3d11::S_OK);
        write_iid(winapi::IID_IDXGIRESOURCE);
        const u32 hr = call(chain, 9, {1, IID, OUT});
        if (effect == 4) {
            EXPECT_EQ(hr, kIC);
        } else {
            ASSERT_EQ(hr, d3d11::S_OK) << effect;
            const GuestAddr buffer1 = memory.read_u64(OUT).value();
            set_arg(0, buffer1);
            set_arg(1, OUT2);
            d3d11.impl_DxgiResourceMethod(marshaler, 9);
            EXPECT_EQ(memory.read_u32(OUT2).value(), 0x160u) << effect;
            d3d11.impl_DxgiResourceMethod(marshaler, 2);
            // The ring still rotates with the named buffer in it.
            EXPECT_EQ(call(chain, 8, {0, 0}), d3d11::S_OK);
        }
        EXPECT_EQ(call(chain, 9, {2, IID, OUT}), kIC);
        (void)call(chain, 2, {});
    }
}

TEST_F(DxgiSwapChainContractTest, ResizeBuffersRulesAsMeasured) {
    GuestAddr chain = 0;
    ASSERT_EQ(create(Desc{}, &chain), d3d11::S_OK);
    write_iid(winapi::IID_ID3D11TEXTURE2D);
    ASSERT_EQ(call(chain, 9, {0, IID, BUFFER_OUT}), d3d11::S_OK);
    const GuestAddr back = memory.read_u64(BUFFER_OUT).value();
    // "ResizeBuffers with outstanding GetBuffer ref": INVALID_CALL.
    EXPECT_EQ(call(chain, 13, {0, 100, 80, 0, 0}), kIC);
    const GuestAddr rtv = make_rtv(back);
    release_texture(back);
    // "... with outstanding RTV": INVALID_CALL.
    EXPECT_EQ(call(chain, 13, {0, 100, 80, 0, 0}), kIC);
    bind_rtv(rtv);
    release_view(rtv);
    // "... with RTV bound to context only": S_OK.
    EXPECT_EQ(call(chain, 13, {0, 100, 80, 0, 0}), d3d11::S_OK);
    EXPECT_EQ(bound_rtv(), 0u) << "the dead view is unbound";
    // Zero Width or Height: both from the client area.
    client_w = 176;
    client_h = 132;
    ASSERT_EQ(call(chain, 13, {0, 0, 50, 0, 0}), d3d11::S_OK);
    ASSERT_EQ(call(chain, 12, {SCRATCH}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(SCRATCH).value(), 176u);
    EXPECT_EQ(memory.read_u32(SCRATCH + 4).value(), 132u);
    // Counts: 16 fine, 17 E_INVALIDARG.
    EXPECT_EQ(call(chain, 13, {16, 64, 48, 0, 0}), d3d11::S_OK);
    EXPECT_EQ(call(chain, 13, {17, 64, 48, 0, 0}), d3d11::E_INVALIDARG);
    // Flags: toggling ALLOW_TEARING / WAITABLE is E_INVALIDARG, as is
    // FOREGROUND_LAYER; an unknown bit is INVALID_CALL; GDI is fine.
    EXPECT_EQ(call(chain, 13, {0, 64, 48, 0, 0x800}), d3d11::E_INVALIDARG);
    EXPECT_EQ(call(chain, 13, {0, 64, 48, 0, 0x40}), d3d11::E_INVALIDARG);
    EXPECT_EQ(call(chain, 13, {0, 64, 48, 0, 0x80}), d3d11::E_INVALIDARG);
    EXPECT_EQ(call(chain, 13, {0, 64, 48, 0, 0x80000000u}), kIC);
    EXPECT_EQ(call(chain, 13, {0, 64, 48, 0, 0x4}), d3d11::S_OK);
    // A bitblt chain given a format it cannot use fails AFTER giving up its
    // buffers: the description holds the new format, GetBuffer and Present
    // are INVALID_CALL, and a later good ResizeBuffers recovers
    // (rules probe "DISCARD ResizeBuffers(R32_FLOAT)").
    EXPECT_EQ(call(chain, 13, {0, 32, 32, 41, 0}), d3d11::E_INVALIDARG);
    ASSERT_EQ(call(chain, 12, {SCRATCH}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(SCRATCH + 16).value(), 41u);
    EXPECT_EQ(memory.read_u32(SCRATCH).value(), 32u);
    EXPECT_EQ(call(chain, 9, {0, IID, BUFFER_OUT}), kIC);
    EXPECT_EQ(call(chain, 8, {0, 0}), kIC);
    EXPECT_EQ(call(chain, 13, {0, 0, 0, 0, 0}), d3d11::E_INVALIDARG);
    EXPECT_EQ(call(chain, 13, {0, 64, 48, 87, 0}), d3d11::S_OK);
    EXPECT_EQ(call(chain, 8, {0, 0}), d3d11::S_OK);
    (void)call(chain, 2, {});
    // Flip: count 1 and an sRGB format are E_INVALIDARG with the description
    // unchanged.
    Desc d;
    d.effect = 4;
    d.count = 2;
    ASSERT_EQ(create(d, &chain), d3d11::S_OK);
    EXPECT_EQ(call(chain, 13, {1, 64, 48, 0, 0}), d3d11::E_INVALIDARG);
    EXPECT_EQ(call(chain, 13, {0, 64, 48, 29, 0}), d3d11::E_INVALIDARG);
    ASSERT_EQ(call(chain, 12, {SCRATCH}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(SCRATCH + 16).value(), 87u);
    EXPECT_EQ(memory.read_u32(SCRATCH + 40).value(), 2u);
    // RESTRICT_SHARED_RESOURCE_DRIVER on a flip chain fails late: the
    // description already carries it (rules probe "FLIP_DISCARD
    // ResizeBuffers(flags=0x10)").
    EXPECT_EQ(call(chain, 13, {0, 64, 48, 0, 0x10}), d3d11::E_INVALIDARG);
    ASSERT_EQ(call(chain, 12, {SCRATCH}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(SCRATCH + 64).value(), 0x10u);
    (void)call(chain, 2, {});
}

TEST_F(DxgiSwapChainContractTest, SwapChainMediaFacet) {
    // "QI IDXGISwapChainMedia hr=0x00000000" on every chain. The facet shares
    // the chain's identity and reference count.
    GuestAddr chain = 0;
    Desc d;
    d.effect = 4;
    d.count = 2;
    ASSERT_EQ(create(d, &chain), d3d11::S_OK);
    write_iid(winapi::IID_IDXGISWAPCHAINMEDIA);
    ASSERT_EQ(call(chain, 0, {IID, OUT}), d3d11::S_OK);
    const GuestAddr media = memory.read_u64(OUT).value();
    ASSERT_NE(media, 0u);
    EXPECT_NE(memory.read_u64(media).value(), 0u) << "facet vtable";
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto media_call = [&](usize slot, std::initializer_list<u64> args) {
        set_arg(0, media);
        usize index = 1;
        for (const u64 value : args) set_arg(index++, value);
        d3d11.impl_SwapChainMediaMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    };
    write_iid(winapi::IID_IUNKNOWN);
    ASSERT_EQ(media_call(0, {IID, OUT2}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(OUT2).value(), chain) << "one IUnknown";
    (void)media_call(2, {});
    // No custom present durations: (0, 0); asking for one is refused, 0 is
    // fine.
    ASSERT_TRUE(memory.write_u32(SCRATCH, 7).is_ok());
    ASSERT_TRUE(memory.write_u32(SCRATCH + 4, 7).is_ok());
    EXPECT_EQ(media_call(5, {166667, SCRATCH, SCRATCH + 4}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(SCRATCH).value(), 0u);
    EXPECT_EQ(memory.read_u32(SCRATCH + 4).value(), 0u);
    EXPECT_EQ(media_call(4, {0}), d3d11::S_OK);
    EXPECT_EQ(media_call(4, {166667}), kIC);
    EXPECT_EQ(media_call(3, {0}), kIC);
    EXPECT_EQ(media_call(3, {SCRATCH}),
              d3d11::DXGI_ERROR_FRAME_STATISTICS_DISJOINT);
    (void)media_call(2, {});
    (void)call(chain, 2, {});
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
