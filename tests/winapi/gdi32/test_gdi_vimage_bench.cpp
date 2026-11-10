/// @file test_gdi_vimage_bench.cpp
/// @brief WS7 gate microbenchmark: every eligible combination at the API
/// level, software (ScopedForce(false)) against vImage (ScopedForce(true)),
/// median of 5 interleaved reps at 1920x1080, plus the 1x1..1024x1024
/// crossover sweep. Whole-surface conversion rows are timed with the 1.2x
/// gate lifted (ScopedIgnoreGate), so rows that ship as software are
/// re-measured too; only shipped rows must clear 1.2x. Skipped unless
/// VITREO_WS7_BENCH is set; run it on an otherwise idle machine:
///
///   VITREO_WS7_BENCH=1 build/tests/vitreo_winapi_tests --gtest_filter='GdiVimageBench.*'

#include <gtest/gtest.h>
#include <winapi/graphics/gdi32/gdi32.hpp>
#include <winapi/graphics/gdi32/bitmap_codec.hpp>
#include <winapi/graphics/gdi32/gdi_vimage.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
namespace accel = vitreo::winapi::gdi::accel;
using accel::Kernel;

namespace {

constexpr GuestAddr kMem = 0x01000000;
constexpr usize kMemSize = 0x02000000;
constexpr GuestAddr kBmi = kMem + 0x100;
constexpr GuestAddr kBmi2 = kMem + 0x800;
constexpr GuestAddr kPpv = kMem + 0x80;
constexpr GuestAddr kBits = kMem + 0x10000;
constexpr u32 kSrcCopy = 0x00CC0020;
constexpr u32 kDstInvert = 0x00550009;

struct Fmt {
    u16 bpp = 32;
    u32 compression = gdi::kBiRgb;
    std::array<u32, 3> masks{};
    u32 colors = 0;
};

struct World {
    HandleTable handles;
    GuestMemory memory;
    std::unique_ptr<GdiManager> mgr;
    std::mt19937 rng{0x5737};

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
    void write_bmi(GuestAddr at, i32 w, i32 h, const Fmt& f) {
        (void)memory.write_u32(at + 0, 40);
        (void)memory.write_u32(at + 4, static_cast<u32>(w));
        (void)memory.write_u32(at + 8, static_cast<u32>(h));
        (void)memory.write_u16(at + 12, 1);
        (void)memory.write_u16(at + 14, f.bpp);
        (void)memory.write_u32(at + 16, f.compression);
        for (u32 off = 20; off < 40; off += 4) (void)memory.write_u32(at + off, 0);
        (void)memory.write_u32(at + 32, f.colors);
        if (f.compression == gdi::kBiBitfields) {
            for (u32 i = 0; i < 3; ++i) (void)memory.write_u32(at + 40 + i * 4, f.masks[i]);
        }
        for (u32 i = 0; i < f.colors; ++i) (void)memory.write_u32(at + 40 + i * 4, rng() & 0xFFFFFFu);
    }
    Handle dib_dc(i32 w, i32 h, const Fmt& f) {
        write_bmi(kBmi, w, h, f);
        const Handle bmp = mgr->create_dib_section(0, kBmi, 0, kPpv, false);
        EXPECT_NE(bmp, 0u);
        const GuestAddr bits = memory.read_u32(kPpv).value_or(0);
        random_guest(bits, static_cast<usize>(gdi::dib_stride(w, f.bpp)) * static_cast<usize>(std::abs(h)));
        const Handle dc = mgr->create_compatible_dc(0);
        (void)mgr->select_object(dc, bmp);
        return dc;
    }
    std::pair<Handle, Handle> ddb(i32 w, i32 h, u32 bpp) {
        random_guest(kBits, gdi::ddb_stride(w, bpp) * static_cast<usize>(h));
        const Handle bmp = mgr->create_bitmap(w, h, 1, bpp, kBits);
        const Handle dc = mgr->create_compatible_dc(0);
        (void)mgr->select_object(dc, bmp);
        return {dc, bmp};
    }
    Handle window_dc(u64 hwnd, i32 w, i32 h) {
        const Handle dc = mgr->get_dc(static_cast<Handle>(hwnd));
        mgr->ensure_framebuffer_size(hwnd, w, h);
        return dc;
    }
};

/// One benchmarked operation at side length @p n (n x n, or 1920x1080 for
/// n == 0). setup builds the world; op is timed.
/// The classify() request a whole-surface conversion row is gated by.
struct Probe {
    accel::Op op;
    accel::Site site;
    Fmt f;
    bool bottom_up;
};

struct Bench {
    const char* name;
    std::vector<Kernel> kernels;  ///< kernels this row exercises
    std::function<std::function<void()>(World&, i32 w, i32 h)> make;
    std::optional<Probe> probe{};  ///< set for gated conversion rows
    /// Held in both arms: the other direction of a two-way operation.
    std::optional<std::pair<accel::Op, bool>> pin{};
};

accel::Request request_of(const Probe& p, const gdi::PixelFormat& pf, i64 pixels) {
    accel::Request r;
    r.op = p.op;
    r.site = p.site;
    r.bottom_up = p.bottom_up;
    r.format = &pf;
    r.pixels = pixels;
    return r;
}

gdi::PixelFormat format_of(const Fmt& f) {
    gdi::PixelFormat pf;
    pf.bpp = f.bpp;
    pf.bitfields = f.compression == gdi::kBiBitfields;
    pf.masks = f.masks;
    return pf;
}

/// True when the row's operation uses vImage at w x h with the shipped gate.
bool shipped(const Bench& b, i64 pixels) {
    if (!b.probe) return true;
    const gdi::PixelFormat pf = format_of(b.probe->f);
    accel::ScopedForce on(true);
    return accel::classify(request_of(*b.probe, pf, pixels)).kernel != Kernel::None;
}

double ns_per_op(const std::function<void()>& op, double min_ms) {
    using clock = std::chrono::steady_clock;
    op();  // warm
    u64 n = 0;
    const auto t0 = clock::now();
    auto t1 = t0;
    do {
        op();
        ++n;
        t1 = clock::now();
    } while (std::chrono::duration<double, std::milli>(t1 - t0).count() < min_ms);
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(n);
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

struct AbResult {
    double sw = 0;               ///< software median, ns/op
    double vi = 0;               ///< vImage median, ns/op
    bool kernels_ran = true;     ///< every kernel in Bench::kernels ran in each vImage rep
};

/// Median-of-5 interleaved A/B. In every vImage rep, each kernel the row
/// names must have advanced its run counter, so a row that silently falls
/// back to software cannot pass as a measured kernel.
AbResult measure(const Bench& b, i32 w, i32 h, double min_ms) {
    World world;
    const auto op = b.make(world, w, h);
    std::optional<accel::ScopedForceOp> pin;
    if (b.pin) pin.emplace(b.pin->first, b.pin->second);
    std::vector<double> sw, vi;
    bool ran = true;
    for (int rep = 0; rep < 5; ++rep) {
        for (int arm = 0; arm < 2; ++arm) {
            const bool fast = (arm == 0) == (rep % 2 == 1);
            accel::ScopedForce force(fast);
            std::vector<u64> before;
            for (Kernel k : b.kernels) before.push_back(accel::kernel_runs(k));
            (fast ? vi : sw).push_back(ns_per_op(op, min_ms));
            if (fast) {
                for (usize i = 0; i < b.kernels.size(); ++i) {
                    if (accel::kernel_runs(b.kernels[i]) == before[i]) ran = false;
                }
            }
        }
    }
    return {median(sw), median(vi), ran};
}

/// True when every kernel of @p b is at or above its baked threshold at n x n.
bool above_thresholds(const Bench& b, i32 n) {
    if (b.probe) return shipped(b, static_cast<i64>(n) * n);
    for (Kernel k : b.kernels) {
        if (static_cast<i64>(n) * n < accel::min_pixels(k)) return false;
    }
    return true;
}

std::vector<Bench> benches() {
    const Fmt f32{32, gdi::kBiRgb, {}, 0};
    const Fmt f32bf{32, gdi::kBiBitfields, {0x00FF0000, 0x0000FF00, 0x000000FF}, 0};
    const Fmt f24{24, gdi::kBiRgb, {}, 0};
    const Fmt f8{8, gdi::kBiRgb, {}, 256};
    const Fmt f4{4, gdi::kBiRgb, {}, 16};
    const Fmt f1{1, gdi::kBiRgb, {}, 2};
    std::vector<Bench> out;
    auto sync = [](Fmt f, bool bottom_up) {
        return [f, bottom_up](World& world, i32 w, i32 h) -> std::function<void()> {
            const Handle dc = world.dib_dc(w, bottom_up ? h : -h, f);
            GdiManager* mgr = world.mgr.get();
            return [mgr, dc] { (void)mgr->pat_blt(dc, 0, 0, 1, 1, kDstInvert); };
        };
    };
    out.push_back({"Sync32Bitfields", {Kernel::Bytes32}, sync(f32bf, true)});
    using accel::Op;
    using accel::Site;
    struct Conv {
        const char* stem;
        Fmt f;
        Kernel k;
    };
    const Conv decodes[] = {{"32", f32, Kernel::Bgrx32}, {"24", f24, Kernel::Bgr24},
                            {"8", f8, Kernel::Pal8},     {"4", f4, Kernel::Pal4},
                            {"1", f1, Kernel::Pal1}};
    const Conv encodes[] = {{"32", f32, Kernel::Bgrx32}, {"24", f24, Kernel::Bgr24}};
    // Names must outlive the rows (a deque never moves its elements).
    static std::deque<std::string> names;
    auto name = [](std::string n) {
        names.push_back(std::move(n));
        return names.back().c_str();
    };
    // DIB section sync, decode direction alone: BitBlt from the section's
    // DC re-syncs the whole section from guest memory, then copies 1 pixel.
    auto sync_decode = [](Fmt f, bool bottom_up) {
        return [f, bottom_up](World& world, i32 w, i32 h) -> std::function<void()> {
            const Handle src = world.dib_dc(w, bottom_up ? h : -h, f);
            const Handle dst = world.ddb(1, 1, 32).first;
            GdiManager* mgr = world.mgr.get();
            return [mgr, src, dst] { (void)mgr->bit_blt(dst, 0, 0, 1, 1, src, 0, 0, kSrcCopy); };
        };
    };
    for (const auto& c : decodes) {
        for (const bool bu : {false, true}) {
            out.push_back({name(std::string("SyncDecode") + c.stem + (bu ? "BottomUp" : "TopDown")),
                           {c.k}, sync_decode(c.f, bu), Probe{Op::Decode, Site::DibSection, c.f, bu}});
        }
    }
    // Encode direction: PatBlt 1x1 re-syncs from and to the guest; the
    // decode is held on software in both arms, so the ratio is diluted by
    // it (a lower bound on the encode alone).
    for (const auto& c : encodes) {
        for (const bool bu : {false, true}) {
            out.push_back({name(std::string("SyncEncode") + c.stem + (bu ? "BottomUp" : "TopDown")),
                           {c.k}, sync(c.f, bu), Probe{Op::Encode, Site::DibSection, c.f, bu},
                           std::pair{Op::Decode, false}});
        }
    }
    auto set_dibits = [](Fmt f, bool bottom_up = true) {
        return [f, bottom_up](World& world, i32 w, i32 h) -> std::function<void()> {
            const auto [dc, bmp] = world.ddb(w, h, 32);
            (void)dc;
            world.write_bmi(kBmi2, w, bottom_up ? h : -h, f);
            world.random_guest(kBits, gdi::dib_stride(w, f.bpp) * static_cast<usize>(h));
            const Handle hdc = world.mgr->create_compatible_dc(0);
            GdiManager* mgr = world.mgr.get();
            const Handle target = bmp;
            const u32 lines = static_cast<u32>(h);
            return [mgr, hdc, target, lines] {
                (void)mgr->set_dib_bits(hdc, target, 0, lines, kBits, kBmi2, 0);
            };
        };
    };
    out.push_back({"SetDIBits32Bitfields", {Kernel::Bytes32}, set_dibits(f32bf)});
    for (const auto& c : decodes) {
        for (const bool bu : {false, true}) {
            out.push_back({name(std::string("SetDIBits") + c.stem + (bu ? "BottomUp" : "TopDown")),
                           {c.k}, set_dibits(c.f, bu), Probe{Op::Decode, Site::DibBits, c.f, bu}});
        }
    }
    auto get_dibits = [](Fmt f, bool bottom_up = true) {
        return [f, bottom_up](World& world, i32 w, i32 h) -> std::function<void()> {
            const auto [dc, bmp] = world.ddb(w, h, 32);
            (void)dc;
            const Handle hdc = world.mgr->create_compatible_dc(0);
            GdiManager* mgr = world.mgr.get();
            World* wp = &world;
            const Handle source = bmp;
            const u32 lines = static_cast<u32>(h);
            const i32 bh = bottom_up ? h : -h;
            return [mgr, wp, hdc, source, lines, f, w, bh] {
                wp->write_bmi(kBmi2, w, bh, f);  // GetDIBits rewrites the header
                (void)mgr->get_dib_bits(hdc, source, 0, lines, kBits, kBmi2, 0);
            };
        };
    };
    out.push_back({"GetDIBits32Bitfields", {Kernel::Bytes32}, get_dibits(f32bf)});
    for (const auto& c : encodes) {
        for (const bool bu : {false, true}) {
            out.push_back({name(std::string("GetDIBits") + c.stem + (bu ? "BottomUp" : "TopDown")),
                           {c.k}, get_dibits(c.f, bu), Probe{Op::Encode, Site::DibBits, c.f, bu}});
        }
    }
    // DDB rows (top-down by definition): CreateBitmap decodes its initial
    // bits; GetBitmapBits encodes.
    for (const auto& c : decodes) {
        const u32 bpp = c.f.bpp;
        out.push_back({name(std::string("CreateBitmap") + c.stem), {c.k},
                       [bpp](World& world, i32 w, i32 h) -> std::function<void()> {
                           world.random_guest(kBits, gdi::ddb_stride(w, bpp) * static_cast<usize>(h));
                           GdiManager* mgr = world.mgr.get();
                           return [mgr, w, h, bpp] {
                               const Handle b = mgr->create_bitmap(w, h, 1, bpp, kBits);
                               (void)mgr->delete_object(b);
                           };
                       },
                       Probe{Op::Decode, Site::DdbBits, c.f, false}});
    }
    for (const auto& c : encodes) {
        const u32 bpp = c.f.bpp;
        out.push_back({name(std::string("GetBitmapBits") + c.stem), {c.k},
                       [bpp](World& world, i32 w, i32 h) -> std::function<void()> {
                           const Handle bmp = world.ddb(w, h, bpp).second;
                           const i32 bytes = static_cast<i32>(gdi::ddb_stride(w, bpp)) * h;
                           GdiManager* mgr = world.mgr.get();
                           return [mgr, bmp, bytes] { (void)mgr->get_bitmap_bits(bmp, bytes, kBits); };
                       },
                       Probe{Op::Encode, Site::DdbBits, c.f, false}});
    }
    // A 1x1 PatBlt on a 24bpp DDB re-quantizes the whole surface.
    out.push_back({"Quantize24", {Kernel::ClearFourth},
                   [](World& world, i32 w, i32 h) -> std::function<void()> {
                       const Handle dc = world.ddb(w, h, 24).first;
                       GdiManager* mgr = world.mgr.get();
                       return [mgr, dc] { (void)mgr->pat_blt(dc, 0, 0, 1, 1, kDstInvert); };
                   },
                   Probe{Op::Quantize, Site::DdbBits, f24, false}});
    out.push_back({"BitBltMem", {Kernel::CopyRgba},
                   [](World& world, i32 w, i32 h) -> std::function<void()> {
                       const Handle s = world.ddb(w, h, 32).first;
                       const Handle d = world.ddb(w, h, 32).first;
                       GdiManager* mgr = world.mgr.get();
                       return [mgr, s, d, w, h] { (void)mgr->bit_blt(d, 0, 0, w, h, s, 0, 0, kSrcCopy); };
                   }});
    out.push_back({"BitBltWindow", {Kernel::CopyRgba},
                   [](World& world, i32 w, i32 h) -> std::function<void()> {
                       const Handle s = world.ddb(w, h, 32).first;
                       const Handle d = world.window_dc(0x10040, w, h);
                       GdiManager* mgr = world.mgr.get();
                       return [mgr, s, d, w, h] { (void)mgr->bit_blt(d, 0, 0, w, h, s, 0, 0, kSrcCopy); };
                   }});
    out.push_back({"BitBltScroll", {Kernel::CopyRgba},
                   [](World& world, i32 w, i32 h) -> std::function<void()> {
                       const Handle s = world.ddb(w + 1, h + 1, 32).first;
                       GdiManager* mgr = world.mgr.get();
                       return [mgr, s, w, h] { (void)mgr->bit_blt(s, 0, 0, w, h, s, 1, 1, kSrcCopy); };
                   }});
    auto stretch_dibits = [](Fmt f, bool bottom_up) {
        return [f, bottom_up](World& world, i32 w, i32 h) -> std::function<void()> {
            const Handle d = world.ddb(w, h, 32).first;
            world.write_bmi(kBmi2, w, bottom_up ? h : -h, f);
            world.random_guest(kBits, gdi::dib_stride(w, f.bpp) * static_cast<usize>(h));
            GdiManager* mgr = world.mgr.get();
            GuestMemory* mem = &world.memory;
            return [mgr, mem, d, w, h] {
                Marshaler m(*mem);
                (void)mgr->stretch_dib_bits(m, d, 0, 0, w, h, 0, 0, w, h, kBits, kBmi2, 0, kSrcCopy);
            };
        };
    };
    out.push_back({"StretchDIBits32TopDown", {Kernel::Bgrx32}, stretch_dibits(f32, false)});
    out.push_back({"StretchDIBits32BottomUp", {Kernel::Bgrx32}, stretch_dibits(f32, true)});
    out.push_back({"StretchDIBits24TopDown", {Kernel::Bgr24}, stretch_dibits(f24, false)});
    out.push_back({"StretchDIBits24BottomUp", {Kernel::Bgr24}, stretch_dibits(f24, true)});
    out.push_back({"StretchDIBits8TopDown", {Kernel::Pal8}, stretch_dibits(f8, false)});
    out.push_back({"StretchDIBits8BottomUp", {Kernel::Pal8}, stretch_dibits(f8, true)});
    out.push_back({"StretchDIBits32BfBottomUp", {Kernel::Bytes32}, stretch_dibits(f32bf, true)});
    out.push_back({"StretchDIBits4BottomUp", {Kernel::Pal4}, stretch_dibits(f4, true)});
    out.push_back({"StretchDIBits1BottomUp", {Kernel::Pal1}, stretch_dibits(f1, true)});
    auto alpha = [](u8 format, u8 ca) {
        return [format, ca](World& world, i32 w, i32 h) -> std::function<void()> {
            const Handle s = world.ddb(w, h, 32).first;
            const Handle d = world.ddb(w, h, 32).first;
            GdiManager* mgr = world.mgr.get();
            return [mgr, s, d, w, h, format, ca] {
                (void)mgr->alpha_blend_checked(d, 0, 0, w, h, s, 0, 0, w, h, 0, ca, format);
            };
        };
    };
    out.push_back({"AlphaBlendPremul", {Kernel::AlphaPremul}, alpha(1, 255)});
    out.push_back({"AlphaBlendPremulConst", {Kernel::AlphaPremulConst}, alpha(1, 200)});
    out.push_back({"AlphaBlendConst", {Kernel::AlphaConst}, alpha(0, 200)});
    return out;
}

/// The sweep runs 1x1 .. 1024x1024: the memory-bound whole-surface
/// conversions cross over far above 256x256, if at all.
constexpr i32 kSweepMax = 1024;

bool bench_enabled() { return environment_flag_enabled("VITREO_WS7_BENCH"); }

}  // namespace

TEST(GdiVimageBench, Gate1080p) {
    if (!bench_enabled()) GTEST_SKIP() << "set VITREO_WS7_BENCH=1";
    std::printf("WS7 1920x1080, median of 5 interleaved reps (gate lifted; ship = classify)\n");
    std::printf("%-26s | %12s | %12s | %7s | %s\n", "name", "sw ns/op", "vimage ns/op", "ratio",
                "ship");
    for (const auto& b : benches()) {
        const bool ships = shipped(b, i64{1920} * 1080);
        AbResult r;
        {
            accel::ScopedIgnoreGate all;
            r = measure(b, 1920, 1080, 50.0);
        }
        std::printf("%-26s | %12.0f | %12.0f | %7.2f | %s\n", b.name, r.sw, r.vi, r.sw / r.vi,
                    ships ? "vimage" : "software");
        std::fflush(stdout);
        EXPECT_TRUE(r.kernels_ran) << b.name << ": vImage arm did not run its kernel";
        if (ships) EXPECT_GE(r.sw / r.vi, 1.2) << b.name << ": ships but is below the WS7 1.2x gate";
    }
}

TEST(GdiVimageBench, Crossover) {
    if (!bench_enabled()) GTEST_SKIP() << "set VITREO_WS7_BENCH=1";
    // Measures the kernels below their baked thresholds too, so the sweep
    // can find the crossover; GdiVimageBench.CrossoverBaked re-runs it with
    // the thresholds in force.
    accel::ScopedIgnoreCrossover ignore;
    accel::ScopedIgnoreGate all;
    std::printf("WS7 crossover sweep (n x n), median of 5 interleaved reps\n");
    std::printf("%-26s | %5s | %10s | %10s | %6s\n", "name", "n", "sw ns", "vimage ns", "ratio");
    for (const auto& b : benches()) {
        for (i32 n = 1; n <= kSweepMax; n *= 2) {
            const AbResult r = measure(b, n, n, 10.0);
            std::printf("%-26s | %5d | %10.0f | %10.0f | %6.2f\n", b.name, n, r.sw, r.vi, r.sw / r.vi);
            EXPECT_TRUE(r.kernels_ran) << b.name << " n=" << n << ": vImage arm did not run its kernel";
        }
        std::fflush(stdout);
    }
}

TEST(GdiVimageBench, CrossoverBaked) {
    if (!bench_enabled()) GTEST_SKIP() << "set VITREO_WS7_BENCH=1";
    std::printf("WS7 sweep with the baked thresholds (below them both arms are software)\n");
    std::printf("%-26s | %5s | %10s | %10s | %6s\n", "name", "n", "sw ns", "vimage ns", "ratio");
    for (const auto& b : benches()) {
        for (i32 n = 1; n <= kSweepMax; n *= 2) {
            const AbResult r = measure(b, n, n, 10.0);
            std::printf("%-26s | %5d | %10.0f | %10.0f | %6.2f\n", b.name, n, r.sw, r.vi, r.sw / r.vi);
            if (above_thresholds(b, n)) {
                EXPECT_TRUE(r.kernels_ran) << b.name << " n=" << n << ": kernel did not run above its threshold";
            }
        }
        std::fflush(stdout);
    }
}
