/// @file test_gdi_vimage_blit.cpp
/// @brief WS7 routing: every GDI export that reaches a vImage kernel gives
/// the guest exactly what the software path gives. Each scenario runs twice
/// on a fresh GdiManager and guest memory, once with WS7 forced off and once
/// forced on; the guest bytes, return values and surface pixels must match,
/// and the kernel counters show the fast path ran (or did not).

#include <gtest/gtest.h>
#include <winapi/graphics/gdi32/gdi32.hpp>
#include <winapi/graphics/gdi32/bitmap_codec.hpp>
#include <winapi/graphics/gdi32/gdi_vimage.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
namespace accel = vitreo::winapi::gdi::accel;
using accel::Kernel;

namespace {

constexpr GuestAddr kMem = 0x01000000;       // 32 MiB of guest scratch
constexpr usize kMemSize = 0x02000000;
constexpr GuestAddr kBmi = kMem + 0x100;
constexpr GuestAddr kBmi2 = kMem + 0x800;
constexpr GuestAddr kPpv = kMem + 0x80;
constexpr GuestAddr kBits = kMem + 0x10000;  // up to ~31 MiB of pixel data
constexpr u32 kSrcCopy = 0x00CC0020;
constexpr u32 kDstInvert = 0x00550009;
constexpr u32 kSrcPaint = 0x00EE0086;

struct Fmt {
    u16 bpp = 32;
    u32 compression = gdi::kBiRgb;
    std::array<u32, 3> masks{};
    u32 colors = 0;  ///< palette entries written after the header
};

/// One GdiManager over its own guest memory: an arm of an A/B run.
struct World {
    HandleTable handles;
    GuestMemory memory;
    std::unique_ptr<GdiManager> mgr;
    std::mt19937 rng{0x5737};
    std::vector<u8> out;  ///< everything the scenario observed, compared A vs B

    World() {
        EXPECT_TRUE(memory.map_region(kMem, kMemSize, MemoryProtection::ReadWrite).is_ok());
        EXPECT_TRUE(memory.map_region(GdiManager::GDI_DIB_SCRATCH_BASE,
                                      GdiManager::GDI_DIB_SCRATCH_SIZE,
                                      MemoryProtection::ReadWrite).is_ok());
        mgr = std::make_unique<GdiManager>(handles);
        mgr->set_guest_memory(&memory);
    }

    void random_guest(GuestAddr at, usize n) {
        std::vector<u8> b(n);
        for (auto& v : b) v = static_cast<u8>(rng() & 0xFFu);
        (void)memory.write_bytes(at, b.data(), n);
    }

    /// BITMAPINFOHEADER (+ masks or a random colour table) at @p at.
    void write_bmi(GuestAddr at, i32 w, i32 h, const Fmt& f, bool pal_words = false) {
        (void)memory.write_u32(at + 0, 40);
        (void)memory.write_u32(at + 4, static_cast<u32>(w));
        (void)memory.write_u32(at + 8, static_cast<u32>(h));
        (void)memory.write_u16(at + 12, 1);
        (void)memory.write_u16(at + 14, f.bpp);
        (void)memory.write_u32(at + 16, f.compression);
        (void)memory.write_u32(at + 20, 0);
        (void)memory.write_u32(at + 24, 0);
        (void)memory.write_u32(at + 28, 0);
        (void)memory.write_u32(at + 32, f.colors);
        (void)memory.write_u32(at + 36, 0);
        if (f.compression == gdi::kBiBitfields) {
            for (u32 i = 0; i < 3; ++i) (void)memory.write_u32(at + 40 + i * 4, f.masks[i]);
        }
        for (u32 i = 0; i < f.colors; ++i) {
            if (pal_words) {
                (void)memory.write_u16(at + 40 + i * 2, static_cast<u16>(rng() % 20u));
            } else {
                (void)memory.write_u32(at + 40 + i * 4, rng() & 0x00FFFFFFu);
            }
        }
    }

    struct Dib {
        Handle dc = 0, bmp = 0;
        GuestAddr bits = 0;
        usize bytes = 0;
    };
    Dib make_dib(i32 w, i32 h, const Fmt& f) {
        write_bmi(kBmi, w, h, f);
        Dib d;
        d.bmp = mgr->create_dib_section(0, kBmi, 0, kPpv, false);
        EXPECT_NE(d.bmp, 0u);
        d.bits = memory.read_u32(kPpv).value_or(0);
        d.bytes = static_cast<usize>(gdi::dib_stride(w, f.bpp)) * static_cast<usize>(std::abs(h));
        random_guest(d.bits, d.bytes);
        d.dc = mgr->create_compatible_dc(0);
        (void)mgr->select_object(d.dc, d.bmp);
        return d;
    }

    struct Ddb {
        Handle dc = 0, bmp = 0;
    };
    /// A DDB with random content (CreateBitmap with initial bits).
    Ddb make_ddb(i32 w, i32 h, u32 bpp) {
        random_guest(kBits, gdi::ddb_stride(w, bpp) * static_cast<usize>(h));
        Ddb d;
        d.bmp = mgr->create_bitmap(w, h, 1, bpp, kBits);
        EXPECT_NE(d.bmp, 0u);
        d.dc = mgr->create_compatible_dc(0);
        (void)mgr->select_object(d.dc, d.bmp);
        return d;
    }

    /// A window surface DC (opaque black like a fresh window; GetDC sizes an
    /// unknown window's framebuffer to its 320x320 fallback).
    Handle make_window_dc(u64 hwnd) { return mgr->get_dc(static_cast<Handle>(hwnd)); }

    /// Start of the call under test: ab() counts kernels from here, so the
    /// setup (CreateBitmap's own decode, say) does not satisfy a check.
    void begin();

    void record(i64 v) {
        const u8* p = reinterpret_cast<const u8*>(&v);
        out.insert(out.end(), p, p + sizeof(v));
    }
    void record_guest(GuestAddr at, usize n) {
        std::vector<u8> b(n);
        (void)memory.read_bytes(at, b.data(), n);
        out.insert(out.end(), b.begin(), b.end());
    }
    void record_surface(Handle dc) {
        (void)mgr->read_device_surface(dc, [&](const GdiManager::ReadOnlyDeviceSurface& s) {
            record(s.width);
            record(s.height);
            out.insert(out.end(), s.rgba,
                       s.rgba + static_cast<usize>(s.width) * static_cast<usize>(s.height) * 4u);
        });
    }
};

u64 runs(Kernel k) { return accel::kernel_runs(k); }

std::array<u64, static_cast<usize>(Kernel::Count)> g_mark{};
bool g_marked = false;

void World::begin() {
    for (usize i = 0; i < g_mark.size(); ++i) g_mark[i] = runs(static_cast<Kernel>(i));
    g_marked = true;
}
u64 all_runs() {
    u64 n = 0;
    for (u8 i = 1; i < static_cast<u8>(Kernel::Count); ++i) n += runs(static_cast<Kernel>(i));
    return n;
}

/// Run @p scenario in both arms; expect identical observations. Returns the
/// kernel counter deltas of the accelerated arm. That arm lifts the 1.2x
/// conversion gate (ScopedIgnoreGate), so every pixel-exact kernel runs and
/// is proved byte-exact, including the rows that ship as software; the
/// shipped verdicts are ClassifyTable.ConversionGate's job.
std::array<u64, static_cast<usize>(Kernel::Count)> ab(const std::string& what,
                                                      const std::function<void(World&)>& scenario) {
    std::vector<u8> soft, fast;
    std::array<u64, static_cast<usize>(Kernel::Count)> before{}, delta{};
    {
        accel::ScopedForce off(false);
        World w;
        const u64 n0 = all_runs();
        scenario(w);
        EXPECT_EQ(all_runs(), n0) << what << ": a kernel ran with WS7 forced off";
        soft = std::move(w.out);
    }
    {
        accel::ScopedForce on(true);
        accel::ScopedIgnoreGate all;
        World w;
        for (usize i = 0; i < before.size(); ++i) before[i] = runs(static_cast<Kernel>(i));
        g_marked = false;
        scenario(w);
        if (g_marked) before = g_mark;
        for (usize i = 0; i < before.size(); ++i) delta[i] = runs(static_cast<Kernel>(i)) - before[i];
        fast = std::move(w.out);
    }
    EXPECT_EQ(soft.size(), fast.size()) << what;
    if (soft != fast) {
        usize first = 0;
        while (first < soft.size() && first < fast.size() && soft[first] == fast[first]) ++first;
        ADD_FAILURE() << what << ": software and vImage arms differ at byte " << first << " of "
                      << soft.size();
    }
    return delta;
}

u64 d(const std::array<u64, static_cast<usize>(Kernel::Count)>& delta, Kernel k) {
    return delta[static_cast<usize>(k)];
}
u64 total(const std::array<u64, static_cast<usize>(Kernel::Count)>& delta) {
    u64 n = 0;
    for (usize i = 1; i < delta.size(); ++i) n += delta[i];
    return n;
}

struct Named {
    const char* name;
    Fmt f;
    Kernel decode;  ///< the decode kernel (E11 and, gate lifted, whole-surface)
    Kernel encode;  ///< whole-surface encode; None when no exact kernel exists
};

std::vector<Named> dib_formats() {
    return {
        {"32", {32, gdi::kBiRgb, {}, 0}, Kernel::Bgrx32, Kernel::Bgrx32},
        {"32bf", {32, gdi::kBiBitfields, {0x00FF0000, 0x0000FF00, 0x000000FF}, 0}, Kernel::Bytes32,
         Kernel::Bytes32},
        {"32bf-rgb", {32, gdi::kBiBitfields, {0x000000FF, 0x0000FF00, 0x00FF0000}, 0},
         Kernel::Bytes32, Kernel::Bytes32},
        {"24", {24, gdi::kBiRgb, {}, 0}, Kernel::Bgr24, Kernel::Bgr24},
        {"8", {8, gdi::kBiRgb, {}, 256}, Kernel::Pal8, Kernel::None},
        {"8-short", {8, gdi::kBiRgb, {}, 37}, Kernel::Pal8, Kernel::None},
        {"4", {4, gdi::kBiRgb, {}, 16}, Kernel::Pal4, Kernel::None},
        {"1", {1, gdi::kBiRgb, {}, 2}, Kernel::Pal1, Kernel::None},
    };
}

}  // namespace

// =============================================================================
// Task 4: DIB section sync, Get/SetDIBits, DDB conversion
// =============================================================================

TEST(DibSectionSync, TopDownAndBottomUpRoundTrip) {
    for (const auto& n : dib_formats()) {
        for (const bool top_down : {true, false}) {
            for (const i32 w : {1, 3, 33, 641}) {
                const std::string what = std::string(n.name) + (top_down ? " td" : " bu") +
                                         " w=" + std::to_string(w);
                const auto delta = ab(what, [&](World& world) {
                    const auto dib = world.make_dib(w, top_down ? -7 : 7, n.f);
                    world.begin();
                    world.record(world.mgr->pat_blt(dib.dc, 0, 0, 1, 1, kDstInvert));
                    world.record_guest(dib.bits, dib.bytes);
                    world.record_surface(dib.dc);
                });
                if (n.decode == Kernel::Bytes32 && w * 7 < accel::min_pixels(Kernel::Bytes32)) {
                    EXPECT_EQ(total(delta), 0u) << what;
                } else if (n.encode != Kernel::None) {
                    // One decode and one encode (same kernel id).
                    EXPECT_GE(d(delta, n.decode), 2u) << what;
                } else {
                    EXPECT_GE(d(delta, n.decode), 1u) << what;  // indexed encode is software
                }
            }
        }
    }
}

TEST(DibSectionSync, Ineligible16bppUsesSoftware) {
    const Fmt f565{16, gdi::kBiBitfields, {0xF800, 0x07E0, 0x001F}, 0};
    const Fmt f555{16, gdi::kBiRgb, {}, 0};
    for (const Fmt& f : {f565, f555}) {
        const auto delta = ab("16bpp", [&](World& world) {
            const auto dib = world.make_dib(33, -5, f);
            world.begin();
            world.record(world.mgr->pat_blt(dib.dc, 0, 0, 1, 1, kDstInvert));
            world.record_guest(dib.bits, dib.bytes);
            world.record_surface(dib.dc);
        });
        EXPECT_EQ(total(delta), 0u);
    }
}

TEST(SetDibBits, MatchesSoftware) {
    for (const u32 ddb_bpp : {32u, 24u, 1u}) {
        for (const auto& n : dib_formats()) {
            for (const bool top_down : {true, false}) {
                for (const i32 dib_w : {50, 20}) {
                    for (const u32 start : {0u, 2u}) {
                        for (const u32 lines : {1u, 99u}) {
                            const std::string what = std::string(n.name) + " ddb" +
                                                     std::to_string(ddb_bpp) +
                                                     (top_down ? " td" : " bu") + " w" +
                                                     std::to_string(dib_w) + " s" +
                                                     std::to_string(start) + " l" +
                                                     std::to_string(lines);
                            const auto delta = ab(what, [&](World& world) {
                                const auto ddb = world.make_ddb(37, 11, ddb_bpp);
                                const i32 dib_h = 13;
                                world.write_bmi(kBmi2, dib_w, top_down ? -dib_h : dib_h, n.f);
                                world.random_guest(kBits, gdi::dib_stride(dib_w, n.f.bpp) * 13u);
                                const Handle dc = world.mgr->create_compatible_dc(0);
                                world.begin();
                                world.record(world.mgr->set_dib_bits(dc, ddb.bmp, start, lines,
                                                                     kBits, kBmi2, 0));
                                world.record_surface(ddb.dc);
                            });
                            // Scan 0 of a 13-line DIB is bitmap row 12, below
                            // the 11-row bitmap: that band copies nothing.
                            const bool empty_band = start == 0 && lines == 1;
                            if (ddb_bpp == 1) {
                                EXPECT_EQ(d(delta, n.decode), 0u) << what;
                            } else if (empty_band) {
                                EXPECT_EQ(d(delta, n.decode), 0u) << what;
                            } else {
                                EXPECT_GE(d(delta, n.decode), 1u) << what;
                            }
                            if (ddb_bpp == 24) EXPECT_GE(d(delta, Kernel::ClearFourth), 1u) << what;
                        }
                    }
                }
            }
        }
    }
}

TEST(SetDibBits, PalColorsStaysSoftware) {
    const Fmt f8{8, gdi::kBiRgb, {}, 256};
    const auto delta = ab("pal colors", [&](World& world) {
        const auto ddb = world.make_ddb(37, 11, 32);
        world.write_bmi(kBmi2, 37, 11, f8, true);
        world.random_guest(kBits, gdi::dib_stride(37, 8) * 11u);
        const Handle dc = world.mgr->create_compatible_dc(0);
        world.begin();
        world.record(world.mgr->set_dib_bits(dc, ddb.bmp, 0, 11, kBits, kBmi2, 1));
        world.record_surface(ddb.dc);
    });
    EXPECT_EQ(d(delta, Kernel::Pal8), 0u);
}

TEST(SetDibBits, IntoDibSection) {
    for (const auto& n : dib_formats()) {
        const auto delta = ab(n.name, [&](World& world) {
            const auto dib = world.make_dib(29, 9, Fmt{32, gdi::kBiRgb, {}, 0});
            world.write_bmi(kBmi2, 29, -9, n.f);
            world.random_guest(kBits, gdi::dib_stride(29, n.f.bpp) * 9u);
            const Handle dc = world.mgr->create_compatible_dc(0);
            world.begin();
            world.record(world.mgr->set_dib_bits(dc, dib.bmp, 0, 9, kBits, kBmi2, 0));
            world.record_guest(dib.bits, dib.bytes);
        });
        EXPECT_GE(d(delta, n.decode), 1u) << n.name;
    }
}

TEST(GetDibBits, PaddingAndTailZero) {
    const Fmt f32{32, gdi::kBiRgb, {}, 0};
    const Fmt f24{24, gdi::kBiRgb, {}, 0};
    const Fmt f32bf{32, gdi::kBiBitfields, {0x00FF0000, 0x0000FF00, 0x000000FF}, 0};
    for (const Fmt& f : {f32, f24, f32bf}) {
        for (const bool top_down : {true, false}) {
            for (const u32 start : {0u, 3u}) {
                for (const u32 lines : {99u, 4u}) {
                    for (const i32 dib_w : {50, 20}) {
                        const std::string what = std::to_string(f.bpp) + (top_down ? " td" : " bu") +
                                                 " s" + std::to_string(start) + " l" +
                                                 std::to_string(lines) + " w" + std::to_string(dib_w);
                        const auto delta = ab(what, [&](World& world) {
                            const auto ddb = world.make_ddb(37, 11, 32);
                            const i32 dib_h = 13;
                            world.write_bmi(kBmi2, dib_w, top_down ? -dib_h : dib_h, f);
                            const usize bytes = gdi::dib_stride(dib_w, f.bpp) * 13u;
                            world.random_guest(kBits, bytes);  // pre-dirtied
                            const Handle dc = world.mgr->create_compatible_dc(0);
                            world.begin();
                            world.record(world.mgr->get_dib_bits(dc, ddb.bmp, start, lines, kBits,
                                                                 kBmi2, 0));
                            world.record_guest(kBits, bytes);
                            world.record_guest(kBmi2, 64);
                        });
                        EXPECT_GE(d(delta, f.bpp == 24 ? Kernel::Bgr24
                                           : f.compression == gdi::kBiBitfields ? Kernel::Bytes32
                                                                                : Kernel::Bgrx32),
                                  1u)
                            << what;
                    }
                }
            }
        }
    }
}

TEST(GetDibBits, FromDibSection) {
    const auto delta = ab("dib section", [&](World& world) {
        const Fmt bf{32, gdi::kBiBitfields, {0x000000FF, 0x0000FF00, 0x00FF0000}, 0};
        const auto dib = world.make_dib(41, 9, bf);
        world.write_bmi(kBmi2, 41, -9, Fmt{32, gdi::kBiBitfields, {0x00FF0000, 0x0000FF00, 0x000000FF}, 0});
        world.random_guest(kBits, gdi::dib_stride(41, 32) * 9u);
        const Handle dc = world.mgr->create_compatible_dc(0);
        world.begin();
        world.record(world.mgr->get_dib_bits(dc, dib.bmp, 0, 9, kBits, kBmi2, 0));
        world.record_guest(kBits, gdi::dib_stride(41, 32) * 9u);
    });
    EXPECT_GE(d(delta, Kernel::Bytes32), 2u);  // section sync decode + GetDIBits encode
}

TEST(GetDibBits, FromDibSection24To32) {
    const auto delta = ab("dib section 24->32", [&](World& world) {
        const auto dib = world.make_dib(41, 9, Fmt{24, gdi::kBiRgb, {}, 0});
        world.write_bmi(kBmi2, 41, -9, Fmt{32, gdi::kBiRgb, {}, 0});
        world.random_guest(kBits, gdi::dib_stride(41, 32) * 9u);
        const Handle dc = world.mgr->create_compatible_dc(0);
        world.begin();
        world.record(world.mgr->get_dib_bits(dc, dib.bmp, 0, 9, kBits, kBmi2, 0));
        world.record_guest(kBits, gdi::dib_stride(41, 32) * 9u);
    });
    EXPECT_GE(d(delta, Kernel::Bgr24), 1u);   // section sync decode
    EXPECT_GE(d(delta, Kernel::Bgrx32), 1u);  // GetDIBits encode
}

TEST(DdbQuantize, Bpp24) {
    for (const i32 w : {1, 3, 37, 640}) {
        const auto delta = ab("q24 w=" + std::to_string(w), [&](World& world) {
            const auto ddb = world.make_ddb(w, 9, 24);
            world.begin();
            world.record(world.mgr->pat_blt(ddb.dc, 0, 0, 1, 1, kDstInvert));
            world.record_surface(ddb.dc);
            const u32 bytes = gdi::ddb_stride(w, 24) * 9u;
            world.random_guest(kBits, bytes);
            world.record(world.mgr->get_bitmap_bits(ddb.bmp, static_cast<i32>(bytes), kBits));
            world.record_guest(kBits, bytes);
        });
        EXPECT_GE(d(delta, Kernel::ClearFourth), 1u);
        EXPECT_GE(d(delta, Kernel::Bgr24), 1u);  // GetBitmapBits encode
    }
}

TEST(BitmapBits, SetAndGetMatchSoftware) {
    for (const u32 bpp : {32u, 24u, 16u, 8u}) {
        const auto delta = ab("bitmap bits " + std::to_string(bpp), [&](World& world) {
            const auto ddb = world.make_ddb(33, 7, bpp);
            const u32 bytes = gdi::ddb_stride(33, bpp) * 7u;
            world.random_guest(kBits, bytes);
            world.begin();
            world.record(world.mgr->set_bitmap_bits(ddb.bmp, bytes - 5, kBits));
            world.record_surface(ddb.dc);
            world.random_guest(kBits, bytes);
            world.record(world.mgr->get_bitmap_bits(ddb.bmp, static_cast<i32>(bytes), kBits));
            world.record_guest(kBits, bytes);
        });
        if (bpp == 32) EXPECT_GE(d(delta, Kernel::Bgrx32), 3u);
        if (bpp == 24) EXPECT_GE(d(delta, Kernel::Bgr24), 3u);
        // 8bpp decodes through Pal8; its encode (nearest palette) and all
        // of 16bpp stay software.
        if (bpp == 8) EXPECT_EQ(d(delta, Kernel::Pal8), total(delta));
        if (bpp == 8) EXPECT_GE(d(delta, Kernel::Pal8), 1u);
        if (bpp == 16) EXPECT_EQ(total(delta), 0u) << bpp;
    }
}

TEST(KillSwitch, EnvZeroIsSoftware) {
    // VITREO_WS7 is read once per process, so a test cannot flip the
    // environment after the first enabled() call; ScopedForce(false) is the
    // same switch, and every row then classifies as software.
    accel::ScopedForce off(false);
    EXPECT_FALSE(accel::enabled());
    gdi::PixelFormat f;
    f.bpp = 32;
    accel::Request r;
    r.format = &f;
    r.pixels = 1 << 20;
    EXPECT_EQ(accel::classify(r).kernel, Kernel::None);
}

// =============================================================================
// Task 5: BitBlt/StretchBlt, StretchDIBits/SetDIBitsToDevice, AlphaBlend
// =============================================================================

TEST(StretchBltFast, SrccopyUnscaled) {
    struct Case { i32 dx, dy, sx, sy, w, h; };
    const Case cases[] = {{0, 0, 0, 0, 50, 40}, {13, 7, 3, 2, 30, 20}, {-5, -3, 0, 0, 30, 20},
                          {20, 10, 40, 30, 50, 40}, {0, 0, -4, -6, 61, 43}, {7, 3, 5, 9, 1, 1}};
    for (const auto& c : cases) {
        for (const bool stretch_api : {false, true}) {
            const std::string what = std::to_string(c.dx) + "," + std::to_string(c.dy) + " src " +
                                     std::to_string(c.sx) + "," + std::to_string(c.sy) +
                                     (stretch_api ? " StretchBlt" : " BitBlt");
            const auto delta = ab(what, [&](World& world) {
                const auto src = world.make_ddb(61, 43, 32);
                const auto dst = world.make_ddb(50, 40, 32);
                const bool ok = stretch_api
                                    ? world.mgr->stretch_blt(dst.dc, c.dx, c.dy, c.w, c.h, src.dc, c.sx,
                                                             c.sy, c.w, c.h, kSrcCopy)
                                    : world.mgr->bit_blt(dst.dc, c.dx, c.dy, c.w, c.h, src.dc, c.sx,
                                                         c.sy, kSrcCopy);
                world.record(ok);
                world.record_surface(dst.dc);
            });
            EXPECT_GE(d(delta, Kernel::CopyRgba), 1u) << what;
        }
    }
}

TEST(StretchBltFast, IntoDibSection) {
    const auto delta = ab("ddb -> dib", [&](World& world) {
        const auto src = world.make_ddb(61, 43, 32);
        const auto dib = world.make_dib(50, 40, Fmt{24, gdi::kBiRgb, {}, 0});
        world.begin();
        world.record(world.mgr->bit_blt(dib.dc, 3, 4, 40, 30, src.dc, 1, 2, kSrcCopy));
        world.record_guest(dib.bits, dib.bytes);
    });
    EXPECT_GE(d(delta, Kernel::CopyRgba), 1u);
}

TEST(StretchBltFast, PartialCoverageClip) {
    const auto simple = ab("simple clip", [&](World& world) {
        const auto src = world.make_ddb(61, 43, 32);
        const auto dst = world.make_ddb(50, 40, 32);
        world.begin();
        world.record(world.mgr->intersect_clip_rect(dst.dc, 5, 4, 30, 20));
        world.record(world.mgr->bit_blt(dst.dc, 2, 1, 45, 35, src.dc, 3, 3, kSrcCopy));
        world.record_surface(dst.dc);
    });
    EXPECT_GE(d(simple, Kernel::CopyRgba), 1u);
    const auto complex = ab("complex clip", [&](World& world) {
        const auto src = world.make_ddb(61, 43, 32);
        const auto dst = world.make_ddb(50, 40, 32);
        const Handle a = world.mgr->create_rect_rgn(2, 2, 10, 10);
        const Handle b = world.mgr->create_rect_rgn(20, 15, 40, 30);
        world.begin();
        world.record(world.mgr->select_clip_rgn(dst.dc, a));
        world.record(world.mgr->ext_select_clip_rgn(dst.dc, b, 2 /* RGN_OR */));
        world.record(world.mgr->bit_blt(dst.dc, 0, 0, 50, 40, src.dc, 3, 3, kSrcCopy));
        world.record_surface(dst.dc);
    });
    EXPECT_EQ(d(complex, Kernel::CopyRgba), 0u);
}

TEST(StretchBltFast, SelfScroll) {
    const i32 offsets[] = {-7, -1, 0, 1, 7};
    for (const i32 dx : offsets) {
        for (const i32 dy : offsets) {
            if (dx == 0 && dy == 0) continue;
            const std::string what = "scroll " + std::to_string(dx) + "," + std::to_string(dy);
            const auto delta = ab(what, [&](World& world) {
                const auto s = world.make_ddb(64, 48, 32);
                world.begin();
                world.record(world.mgr->bit_blt(s.dc, 10 + dx, 9 + dy, 37, 29, s.dc, 10, 9, kSrcCopy));
                world.record_surface(s.dc);
            });
            EXPECT_GE(d(delta, Kernel::CopyRgba), 1u) << what;
        }
    }
}

TEST(StretchBltFast, WindowSurfaceAlpha) {
    const auto delta = ab("window", [&](World& world) {
        const auto src = world.make_ddb(61, 43, 32);  // random fourth bytes
        const Handle win = world.make_window_dc(0x10010);
        world.begin();
        world.record(world.mgr->bit_blt(win, 4, 3, 40, 30, src.dc, 0, 0, kSrcCopy));
        world.record_surface(win);
        (void)world.mgr->read_device_surface(win, [&](const GdiManager::ReadOnlyDeviceSurface& s) {
            for (i32 y = 3; y < 33; ++y) {
                for (i32 x = 4; x < 44; ++x) {
                    EXPECT_EQ(s.rgba[(static_cast<usize>(y) * static_cast<usize>(s.width) +
                                      static_cast<usize>(x)) * 4 + 3], 0xFF);
                }
            }
        });
    });
    EXPECT_GE(d(delta, Kernel::CopyRgba), 1u);
}

TEST(StretchBltFast, Ineligible) {
    auto expect_none = [](const std::string& what, const std::function<void(World&)>& f) {
        const auto delta = ab(what, f);
        EXPECT_EQ(d(delta, Kernel::CopyRgba), 0u) << what;
    };
    expect_none("srcpaint", [](World& world) {
        const auto src = world.make_ddb(61, 43, 32);
        const auto dst = world.make_ddb(50, 40, 32);
        world.begin();
        world.record(world.mgr->bit_blt(dst.dc, 1, 1, 30, 30, src.dc, 0, 0, kSrcPaint));
        world.record_surface(dst.dc);
    });
    for (i32 mode = 1; mode <= 4; ++mode) {
        for (const bool grow : {true, false}) {
            expect_none("stretch mode " + std::to_string(mode) + (grow ? " x2" : " x1/2"),
                        [&](World& world) {
                            const auto src = world.make_ddb(61, 43, 32);
                            const auto dst = world.make_ddb(50, 40, 32);
                            world.begin();
                            world.record(world.mgr->set_stretch_blt_mode(dst.dc, mode));
                            const i32 sw = grow ? 12 : 48, sh = grow ? 10 : 40;
                            world.record(world.mgr->stretch_blt(dst.dc, 0, 0, 24, 20, src.dc, 1, 1, sw,
                                                                sh, kSrcCopy));
                            world.record_surface(dst.dc);
                        });
        }
    }
    expect_none("mirrored", [](World& world) {
        const auto src = world.make_ddb(61, 43, 32);
        const auto dst = world.make_ddb(50, 40, 32);
        world.begin();
        world.record(world.mgr->stretch_blt(dst.dc, 30, 0, -30, 20, src.dc, 0, 0, 30, 20, kSrcCopy));
        world.record_surface(dst.dc);
    });
    expect_none("mono source", [](World& world) {
        const auto src = world.make_ddb(61, 43, 1);
        const auto dst = world.make_ddb(50, 40, 32);
        world.begin();
        world.record(world.mgr->bit_blt(dst.dc, 0, 0, 30, 20, src.dc, 0, 0, kSrcCopy));
        world.record_surface(dst.dc);
    });
    expect_none("mono destination", [](World& world) {
        const auto src = world.make_ddb(61, 43, 32);
        const auto dst = world.make_ddb(50, 40, 1);
        world.begin();
        world.record(world.mgr->bit_blt(dst.dc, 0, 0, 30, 20, src.dc, 0, 0, kSrcCopy));
        world.record_surface(dst.dc);
    });
    expect_none("MM_LOMETRIC", [](World& world) {
        const auto src = world.make_ddb(61, 43, 32);
        const auto dst = world.make_ddb(50, 40, 32);
        world.begin();
        world.record(world.mgr->set_map_mode(dst.dc, 2));
        world.record(world.mgr->bit_blt(dst.dc, 0, 0, 30, 20, src.dc, 0, 0, kSrcCopy));
        world.record_surface(dst.dc);
    });
}

TEST(StretchDibitsFast, MatchesSoftware) {
    struct Case { i32 xd, yd, xs, ys, w, h; };
    const Case cases[] = {{0, 0, 0, 0, 33, 21}, {5, 4, 2, 3, 20, 12}, {-3, -2, 0, 0, 33, 21},
                          {25, 20, 8, 1, 33, 21}, {2, 2, 16, 4, 17, 9}};
    for (const auto& n : dib_formats()) {
        for (const bool top_down : {true, false}) {
            for (const auto& c : cases) {
                for (const bool window : {false, true}) {
                    const std::string what = std::string(n.name) + (top_down ? " td" : " bu") +
                                             " at " + std::to_string(c.xd) + "," +
                                             std::to_string(c.yd) + " src " + std::to_string(c.xs) +
                                             "," + std::to_string(c.ys) + (window ? " window" : "");
                    const auto delta = ab(what, [&](World& world) {
                        Handle dc = 0;
                        if (window) {
                            dc = world.make_window_dc(0x10020);
                        } else {
                            dc = world.make_ddb(40, 30, 32).dc;
                        }
                        world.write_bmi(kBmi2, 33, top_down ? -21 : 21, n.f);
                        world.random_guest(kBits, gdi::dib_stride(33, n.f.bpp) * 21u);
                        Marshaler m(world.memory);
                        world.begin();
                        world.record(world.mgr->stretch_dib_bits(m, dc, c.xd, c.yd, c.w, c.h, c.xs,
                                                                 c.ys, c.w, c.h, kBits, kBmi2, 0,
                                                                 kSrcCopy));
                        world.record_surface(dc);
                    });
                    // Pal4/Pal1 runs that start mid-byte stay software; a
                    // destination clipped at x < 0 starts the run further in.
                    const i32 first = c.xs + std::max(0, -c.xd);
                    const bool aligned = (n.decode != Kernel::Pal4 || first % 2 == 0) &&
                                         (n.decode != Kernel::Pal1 || first % 8 == 0);
                    // Below the kernel's measured crossover the loop runs.
                    const bool big = static_cast<i64>(c.w) * c.h >= accel::min_pixels(n.decode);
                    if (aligned && big) EXPECT_GE(d(delta, n.decode), 1u) << what;
                    if (!big) EXPECT_EQ(d(delta, n.decode), 0u) << what;
                }
            }
        }
    }
}

TEST(StretchDibitsFast, PalColorsAndStretchStaySoftware) {
    const Fmt f8{8, gdi::kBiRgb, {}, 256};
    const auto pal = ab("pal colors", [&](World& world) {
        const auto dst = world.make_ddb(40, 30, 32);
        world.write_bmi(kBmi2, 33, 21, f8, true);
        world.random_guest(kBits, gdi::dib_stride(33, 8) * 21u);
        Marshaler m(world.memory);
        world.begin();
        world.record(world.mgr->stretch_dib_bits(m, dst.dc, 0, 0, 33, 21, 0, 0, 33, 21, kBits,
                                                 kBmi2, 1, kSrcCopy));
        world.record_surface(dst.dc);
    });
    EXPECT_EQ(d(pal, Kernel::Pal8), 0u);
    const auto stretched = ab("stretched", [&](World& world) {
        const auto dst = world.make_ddb(40, 30, 32);
        world.write_bmi(kBmi2, 33, 21, Fmt{32, gdi::kBiRgb, {}, 0});
        world.random_guest(kBits, gdi::dib_stride(33, 32) * 21u);
        Marshaler m(world.memory);
        world.begin();
        world.record(world.mgr->stretch_dib_bits(m, dst.dc, 0, 0, 40, 30, 0, 0, 33, 21, kBits,
                                                 kBmi2, 0, kSrcCopy));
        world.record_surface(dst.dc);
    });
    EXPECT_EQ(d(stretched, Kernel::Bgrx32), 0u);
}

TEST(SetDibitsToDeviceFast, Bands) {
    for (const auto& n : dib_formats()) {
        for (const bool top_down : {true, false}) {
            for (const u32 start : {0u, 3u}) {
                for (const u32 lines : {1u, 6u, 99u}) {
                    for (const i32 ysrc : {0, 1}) {
                        const std::string what = std::string(n.name) + (top_down ? " td" : " bu") +
                                                 " s" + std::to_string(start) + " l" +
                                                 std::to_string(lines) + " y" + std::to_string(ysrc);
                        const auto delta = ab(what, [&](World& world) {
                            const auto dst = world.make_ddb(40, 30, 32);
                            world.write_bmi(kBmi2, 33, top_down ? -21 : 21, n.f);
                            world.random_guest(kBits, gdi::dib_stride(33, n.f.bpp) * 21u);
                            world.begin();
                            world.record(world.mgr->set_dib_bits_to_device(
                                dst.dc, 2, 3, 30, 19, 0, ysrc, start, lines, kBits, kBmi2, 0));
                            world.record_surface(dst.dc);
                        });
                        (void)delta;
                    }
                }
            }
        }
    }
    // The fast path ran for a plain full-band call.
    const auto delta = ab("full band", [&](World& world) {
        const auto dst = world.make_ddb(40, 30, 32);
        world.write_bmi(kBmi2, 33, 21, Fmt{24, gdi::kBiRgb, {}, 0});
        world.random_guest(kBits, gdi::dib_stride(33, 24) * 21u);
        world.begin();
        world.record(world.mgr->set_dib_bits_to_device(dst.dc, 0, 0, 33, 21, 0, 0, 0, 21, kBits,
                                                       kBmi2, 0));
        world.record_surface(dst.dc);
    });
    EXPECT_GE(d(delta, Kernel::Bgr24), 1u);
}

TEST(AlphaBlendFast, AllKernels) {
    struct Case { i32 xd, yd, xs, ys, w, h; };
    const Case cases[] = {{0, 0, 0, 0, 30, 20}, {13, 7, 3, 2, 30, 20}, {-5, -3, 1, 1, 30, 20},
                          {35, 30, 0, 0, 30, 20}};
    for (const u8 format : {u8{0}, u8{1}}) {
        for (const u8 ca : {u8{0}, u8{1}, u8{128}, u8{255}}) {
            for (const auto& c : cases) {
                for (const bool window : {false, true}) {
                    const std::string what = "fmt" + std::to_string(format) + " ca" +
                                             std::to_string(ca) + " at " + std::to_string(c.xd) +
                                             "," + std::to_string(c.yd) + (window ? " window" : "");
                    const auto delta = ab(what, [&](World& world) {
                        const auto src = world.make_ddb(61, 43, 32);
                        Handle dc = window ? world.make_window_dc(0x10030)
                                           : world.make_ddb(50, 40, 32).dc;
                        world.begin();
                        world.record(world.mgr->alpha_blend_checked(dc, c.xd, c.yd, c.w, c.h, src.dc,
                                                                    c.xs, c.ys, c.w, c.h, 0, ca,
                                                                    format));
                        world.record_surface(dc);
                    });
                    const Kernel want = format == 0     ? Kernel::AlphaConst
                                        : ca == 255     ? Kernel::AlphaPremul
                                                        : Kernel::AlphaPremulConst;
                    EXPECT_GE(d(delta, want), 1u) << what;
                }
            }
        }
    }
}

TEST(AlphaBlendFast, FallbacksAndErrors) {
    auto expect_none = [](const std::string& what, const std::function<void(World&)>& f) {
        const auto delta = ab(what, f);
        EXPECT_EQ(d(delta, Kernel::AlphaPremul) + d(delta, Kernel::AlphaPremulConst) +
                      d(delta, Kernel::AlphaConst),
                  0u)
            << what;
    };
    expect_none("stretched", [](World& world) {
        const auto src = world.make_ddb(61, 43, 32);
        const auto dst = world.make_ddb(50, 40, 32);
        world.begin();
        world.record(world.mgr->alpha_blend_checked(dst.dc, 0, 0, 40, 30, src.dc, 0, 0, 20, 15, 0,
                                                    200, 1));
        world.record_surface(dst.dc);
    });
    expect_none("same surface", [](World& world) {
        const auto s = world.make_ddb(64, 48, 32);
        world.begin();
        world.record(world.mgr->alpha_blend_checked(s.dc, 0, 0, 20, 20, s.dc, 30, 25, 20, 20, 0,
                                                    255, 1));
        world.record_surface(s.dc);
    });
    expect_none("source overrun (87)", [](World& world) {
        const auto src = world.make_ddb(61, 43, 32);
        const auto dst = world.make_ddb(50, 40, 32);
        world.begin();
        world.record(world.mgr->alpha_blend_checked(dst.dc, 0, 0, 30, 30, src.dc, 50, 30, 30, 30,
                                                    0, 255, 1));
        world.record_surface(dst.dc);
    });
    expect_none("blend op (87)", [](World& world) {
        const auto src = world.make_ddb(61, 43, 32);
        const auto dst = world.make_ddb(50, 40, 32);
        world.begin();
        world.record(world.mgr->alpha_blend_checked(dst.dc, 0, 0, 30, 30, src.dc, 0, 0, 30, 30, 1,
                                                    255, 1));
        world.record_surface(dst.dc);
    });
    expect_none("mono destination", [](World& world) {
        const auto src = world.make_ddb(61, 43, 32);
        const auto dst = world.make_ddb(50, 40, 1);
        world.begin();
        world.record(world.mgr->alpha_blend_checked(dst.dc, 0, 0, 30, 30, src.dc, 0, 0, 30, 30, 0,
                                                    255, 1));
        world.record_surface(dst.dc);
    });
}
