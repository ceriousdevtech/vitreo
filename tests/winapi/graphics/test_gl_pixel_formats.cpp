/// @file test_gl_pixel_formats.cpp
/// @brief The OpenGL ICD pixel-format list, ChoosePixelFormat's selection rule,
///        the gdi32 pixel-format exports in both bitnesses, and the mapping to
///        the context's framebuffer.
///
/// Expected values are Microsoft's measured behaviour (wgl_contract_probe64.expected;
/// the 32-bit build is identical) translated to Vitreo's own format list: the
/// oracle's 36 generic formats are not Vitreo's formats, so each `choose-*`
/// request class is asserted as "the rule's answer on THIS table", with the
/// probe tag it comes from.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>

#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/graphics/opengl32/gl_enums.hpp"
#include "winapi/graphics/opengl32/gl_pixel_formats.hpp"
#include "winapi/kernel32/error.hpp"

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::gl;

namespace {

constexpr u32 kDb = PFD_DOUBLEBUFFER;
constexpr u32 kWin = PFD_DRAW_TO_WINDOW;
constexpr u32 kGl = PFD_SUPPORT_OPENGL;
constexpr u32 kBmp = PFD_DRAW_TO_BITMAP;
constexpr u32 kGdi = 0x10;

PixelFormatRequest req(u32 flags, u8 color, u8 depth, u8 alpha = 0, u8 accum = 0, u8 stencil = 0,
                       u8 pixel_type = 0) {
    PixelFormatRequest r;
    r.flags = flags;
    r.color_bits = color;
    r.depth_bits = depth;
    r.alpha_bits = alpha;
    r.accum_bits = accum;
    r.stencil_bits = stencil;
    r.pixel_type = pixel_type;
    return r;
}

/// 1-based index of the first format with the given properties.
i32 find_format(bool db, u8 depth, u8 stencil, u8 alpha) {
    const auto& fs = gl_pixel_formats();
    for (usize i = 0; i < fs.size(); ++i) {
        const auto& f = fs[i];
        if (f.double_buffer == db && f.depth_bits == depth && f.stencil_bits == stencil &&
            f.alpha_bits == alpha) {
            return static_cast<i32>(i) + 1;
        }
    }
    return 0;
}

// ---- The rule, written independently of the implementation -------------------
// wgl_contract_probe64.expected header: key levels 1-5 then lowest index, plus
// the stencil tie-break that Vitreo's table needs (unobservable on Windows).
std::array<int, 7> model_key(const PixelFormatRequest& r, const GlPixelFormat& f) {
    const bool bmp_absent = (r.flags & kBmp) != 0;  // no Vitreo format draws to a bitmap
    const bool gdi_absent = (r.flags & kGdi) != 0;  // nor supports GDI
    const int k1 = (bmp_absent ? 1 : 0) + (gdi_absent ? 1 : 0);
    const int k2 = (r.flags & PFD_DOUBLEBUFFER_DONTCARE)
                       ? 0
                       : (((r.flags & kDb) != 0) != ((f.flags & kDb) != 0) ? 1 : 0);
    const int k3 = ((r.flags & kWin) && !(f.flags & kWin)) ? 1 : 0;
    const int wanted_color = (r.color_bits == 4 || r.color_bits == 8 || r.color_bits == 16 ||
                              r.color_bits == 24 || r.color_bits == 32)
                                 ? r.color_bits
                                 : 32;
    const int k4 = (f.color_bits != wanted_color ? 1 : 0) + (r.accum_bits != 0 ? 1 : 0) +
                   (r.pixel_type != 0 ? 1 : 0) + ((r.alpha_bits != 0 && f.alpha_bits == 0) ? 1 : 0);
    const int want_depth = (r.flags & PFD_DEPTH_DONTCARE) ? 0 : r.depth_bits;
    const int k5 = f.depth_bits >= want_depth ? f.depth_bits : 1000 - f.depth_bits;
    const int k6 = f.stencil_bits >= r.stencil_bits ? f.stencil_bits : 1000 - f.stencil_bits;
    return {k1, k2, k3, k4, k5, k6, 0};
}

i32 model_choose(const PixelFormatRequest& r) {
    const auto& fs = gl_pixel_formats();
    i32 best = 1;
    auto best_key = model_key(r, fs[0]);
    for (usize i = 1; i < fs.size(); ++i) {
        const auto k = model_key(r, fs[i]);
        if (k < best_key) { best_key = k; best = static_cast<i32>(i) + 1; }
    }
    return best;
}

// ---- The gdi32 exports through a guest CPU frame, both bitnesses -------------

class PixelFormatExports {
public:
    static constexpr GuestAddr MEM = 0x10000;
    static constexpr GuestAddr STACK = MEM + 0x4000;
    static constexpr GuestAddr PFD = MEM + 0x100;

    explicit PixelFormatExports(bool x64) : m_x64(x64), handles(), mgr(handles) {
        EXPECT_TRUE(memory.map_region(MEM, 0x8000, MemoryProtection::ReadWrite).is_ok());
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        exports = make_gdi32_exports(mgr);
    }

    u64 call(const char* name, std::vector<u64> args) {
        auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                               [&](const ExportEntry& e) { return e.name == name; });
        EXPECT_NE(it, exports.entries.end()) << name;
        if (it == exports.entries.end()) return ~0ull;
        const GuestAddr frame = STACK + 0x800;
        cpu.set_sp(frame);
        set_last_error(0xDEAD);  // the "untouched" sentinel of the probe
        if (m_x64) {
            (void)memory.write_u64(frame, 0xDEADBEEF);
            for (usize i = 0; i < args.size(); ++i) {
                if (i == 0) cpu.gpr.rcx = args[i];
                else if (i == 1) cpu.gpr.rdx = args[i];
                else if (i == 2) cpu.gpr.r8 = args[i];
                else if (i == 3) cpu.gpr.r9 = args[i];
            }
        } else {
            (void)memory.write_u32(frame, 0xDEADBEEF);
            for (usize i = 0; i < args.size(); ++i) {
                (void)memory.write_u32(frame + 4 + 4 * static_cast<GuestAddr>(i),
                                       static_cast<u32>(args[i]));
            }
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        it->handler(m);
        return m_x64 ? cpu.gpr.rax : (cpu.gpr.rax & 0xFFFFFFFFull);
    }

    void write_request(u32 flags, u8 type, u8 color, u8 alpha, u8 accum, u8 depth, u8 stencil) {
        std::array<u8, 40> d{};
        d[0] = 40; d[2] = 1;
        for (int i = 0; i < 4; ++i) d[4 + i] = static_cast<u8>(flags >> (8 * i));
        d[8] = type; d[9] = color; d[16] = alpha; d[18] = accum; d[23] = depth; d[24] = stencil;
        (void)memory.write_bytes(PFD, d.data(), d.size());
    }

    Handle new_dc() { return mgr.get_dc(0); }
    u32 last_error() const { return get_last_error(); }

    GuestMemory memory;
    cpu::CpuState cpu;
    HandleTable handles;
    GdiManager mgr;
    DllExports exports;

private:
    bool m_x64;
};

class PixelFormatBothBitness : public ::testing::TestWithParam<bool> {};

}  // namespace

// ---- The list ----------------------------------------------------------------

TEST(GlPixelFormats, FormatsAreIcdWindowFormatsOnly) {
    const auto& fs = gl_pixel_formats();
    ASSERT_FALSE(fs.empty());
    for (usize i = 0; i < fs.size(); ++i) {
        const auto& f = fs[i];
        SCOPED_TRACE(i + 1);
        // Design 8: no generic, draw-to-bitmap or colour-index formats.
        EXPECT_EQ(f.flags & PFD_GENERIC_FORMAT, 0u);
        EXPECT_EQ(f.flags & PFD_DRAW_TO_BITMAP, 0u);
        EXPECT_EQ(f.flags & PFD_NEED_PALETTE, 0u);
        EXPECT_EQ(f.flags & PFD_STEREO, 0u);
        EXPECT_EQ(f.flags & (PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_SUPPORT_COMPOSITION),
                  PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_SUPPORT_COMPOSITION);
        EXPECT_EQ(f.double_buffer, (f.flags & PFD_DOUBLEBUFFER) != 0);
        // Design 3: SwapBuffers exchanges back and front, and a double-buffered
        // format says so (PFD_SWAP_EXCHANGE); a single-buffered one has no swap.
        EXPECT_EQ((f.flags & PFD_SWAP_EXCHANGE) != 0, f.double_buffer);
        EXPECT_EQ(f.flags & PFD_SWAP_COPY, 0u);
        EXPECT_EQ(f.flags, f.double_buffer ? 0x00008225u : 0x00008024u);
        EXPECT_EQ(f.color_bits, 32);
        EXPECT_EQ(f.red_bits, 8);
        EXPECT_EQ(f.green_bits, 8);
        EXPECT_EQ(f.blue_bits, 8);
        EXPECT_TRUE(f.depth_bits == 0 || f.depth_bits == 16 || f.depth_bits == 24);
        EXPECT_TRUE(f.stencil_bits == 0 || f.stencil_bits == 8);
        const auto d = gl_pixel_format_descriptor(f);
        EXPECT_EQ(d[8], PFD_TYPE_RGBA);          // pixel type
        EXPECT_EQ(d[18], 0);                     // cAccumBits
        EXPECT_EQ(d[25], 0);                     // cAuxBuffers
    }
    // Both buffer modes exist, and no two formats are identical.
    EXPECT_NE(find_format(false, 24, 8, 8), 0);
    EXPECT_NE(find_format(true, 24, 8, 8), 0);
    for (usize i = 0; i < fs.size(); ++i) {
        for (usize j = i + 1; j < fs.size(); ++j) {
            EXPECT_NE(gl_pixel_format_descriptor(fs[i]), gl_pixel_format_descriptor(fs[j]));
        }
    }
    EXPECT_EQ(gl_pixel_format(0), nullptr);
    EXPECT_EQ(gl_pixel_format(static_cast<i32>(fs.size()) + 1), nullptr);
    EXPECT_EQ(gl_pixel_format(-1), nullptr);
    EXPECT_EQ(gl_pixel_format(1), &fs[0]);
}

// ---- ChoosePixelFormat ---------------------------------------------------------

TEST(GlPixelFormats, ChooseMatchesOracleRule) {
    // The rule applied to Vitreo's table, over a sweep of requests.
    for (const u32 flags : {0u, kWin | kGl, kWin | kGl | kDb, kWin | kGl | kDb | kGdi,
                            kWin | kGl | kBmp, PFD_DOUBLEBUFFER_DONTCARE | kWin,
                            PFD_DOUBLEBUFFER_DONTCARE | kDb | kWin, PFD_DEPTH_DONTCARE | kDb | kWin,
                            PFD_STEREO | kWin | kGl, kDb}) {
        for (const u8 color : {0, 1, 4, 8, 12, 16, 24, 32, 48}) {
            for (const u8 depth : {0, 8, 16, 17, 24, 25, 32, 64}) {
                for (const u8 alpha : {0, 8}) {
                    for (const u8 accum : {0, 64}) {
                        for (const u8 stencil : {0, 8}) {
                            for (const u8 type : {0, 1}) {
                                const auto r = req(flags, color, depth, alpha, accum, stencil, type);
                                ASSERT_EQ(choose_gl_pixel_format(r), model_choose(r))
                                    << "flags=" << flags << " color=" << int(color)
                                    << " depth=" << int(depth) << " alpha=" << int(alpha);
                            }
                        }
                    }
                }
            }
        }
    }
}

TEST(GlPixelFormats, ChooseRequestClasses) {
    constexpr u32 base = kWin | kGl | kDb;  // 0x25, the probe's baseline request
    const i32 sb24 = find_format(false, 24, 0, 0), db24 = find_format(true, 24, 0, 0);
    const i32 db16 = find_format(true, 16, 0, 0), db0 = find_format(true, 0, 0, 0);
    const i32 db24a = find_format(true, 24, 0, 8);

    // choose-baseline (0x25, depth 24): a double-buffered 24-bit format.
    EXPECT_EQ(choose_gl_pixel_format(req(base, 32, 24)), db24);
    // choose-depth-16: the smallest depth that reaches it.
    EXPECT_EQ(choose_gl_pixel_format(req(base, 32, 16)), db16);
    // choose-depth-more (64 asked, 32 offered on Windows): the largest available.
    EXPECT_EQ(choose_gl_pixel_format(req(base, 32, 64)), db24);
    EXPECT_EQ(choose_gl_pixel_format(req(base, 32, 32)), db24);   // choose-depth-32
    // choose-depth-0: the smallest depth, 0.
    EXPECT_EQ(choose_gl_pixel_format(req(base, 32, 0)), db0);
    // choose-depth-dontcare / -24 / -64: DEPTH_DONTCARE makes the requested depth 0.
    EXPECT_EQ(choose_gl_pixel_format(req(base | PFD_DEPTH_DONTCARE, 32, 0)), db0);
    EXPECT_EQ(choose_gl_pixel_format(req(base | PFD_DEPTH_DONTCARE, 32, 24)), db0);
    EXPECT_EQ(choose_gl_pixel_format(req(base | PFD_DEPTH_DONTCARE, 32, 64)), db0);
    // choose-single-buffer / choose-single-no-db: no DB asked prefers single buffering.
    EXPECT_EQ(choose_gl_pixel_format(req(kWin | kGl, 32, 24)), sb24);
    // choose-doublebuffer-dontcare(-with-db): the double-buffer term is skipped,
    // the lowest index among equals wins (a single-buffered format here).
    EXPECT_EQ(choose_gl_pixel_format(req(PFD_DOUBLEBUFFER_DONTCARE | kWin | kGl, 32, 24)), sb24);
    EXPECT_EQ(choose_gl_pixel_format(req(PFD_DOUBLEBUFFER_DONTCARE | base, 32, 24)), sb24);
    // choose-window-only / choose-no-flags: flags that cannot discriminate fall to depth.
    EXPECT_EQ(choose_gl_pixel_format(req(kWin, 32, 24)), sb24);
    EXPECT_EQ(choose_gl_pixel_format(req(0, 32, 24)), sb24);
    // choose-draw-to-bitmap(-db) / choose-support-gdi(-single) / -no-gl: no format has
    // the bit, so level 1 is equal everywhere and the rest decides; Windows' generic
    // formats answered 1 because they carry BMP/GDI, Vitreo's carry neither.
    EXPECT_EQ(choose_gl_pixel_format(req(kBmp | kGl, 32, 24)), sb24);
    EXPECT_EQ(choose_gl_pixel_format(req(kBmp | kGl | kDb, 32, 24)), db24);
    EXPECT_EQ(choose_gl_pixel_format(req(kWin | kGl | kDb | kGdi, 32, 24)), db24);
    EXPECT_EQ(choose_gl_pixel_format(req(kWin | kGdi, 32, 24)), sb24);
    // choose-color-8/16/24/32 and choose-sweep-color: a colour depth no format has
    // costs every format the same, so the rest decides.
    for (const u8 color : {0, 4, 8, 16, 24, 32, 48}) {
        EXPECT_EQ(choose_gl_pixel_format(req(base, color, 24)), db24) << int(color);
    }
    // choose-alpha-8 / choose-sweep-alpha-db: any alpha 1..16 means "want alpha".
    for (const u8 alpha : {1, 8, 16}) {
        EXPECT_EQ(choose_gl_pixel_format(req(base, 32, 24, alpha)), db24a) << int(alpha);
    }
    // choose-accum-* / choose-sweep-accum: no format has accumulation; equal everywhere.
    EXPECT_EQ(choose_gl_pixel_format(req(base, 32, 24, 0, 64)), db24);
    EXPECT_EQ(choose_gl_pixel_format(req(base, 32, 24, 0, 33)), db24);
    // choose-colorindex-8: no colour-index format; equal everywhere, rest decides.
    EXPECT_EQ(choose_gl_pixel_format(req(base, 8, 24, 0, 0, 0, 1)), db24);
    // choose-prio-colorindex-vs-alpha8: the alpha term outweighs the equal pixel-type term.
    EXPECT_EQ(choose_gl_pixel_format(req(base, 8, 24, 8, 0, 0, 1)), db24a);
    // choose-prio-db-vs-color24: a double-buffer request beats a colour mismatch.
    EXPECT_EQ(choose_gl_pixel_format(req(base, 24, 24)), db24);
    // choose-all-zero / choose-zero-but-nsize: the rule has no failure; smallest depth.
    EXPECT_EQ(choose_gl_pixel_format(req(0, 0, 0)), find_format(false, 0, 0, 0));
    // choose-stereo / choose-aux / choose-layer: not carried, no effect.
    EXPECT_EQ(choose_gl_pixel_format(req(base | PFD_STEREO, 32, 24)), db24);
    // choose-exact: every format's own descriptor gets the same index back.
    const auto& fs = gl_pixel_formats();
    for (usize i = 0; i < fs.size(); ++i) {
        const auto r = req(fs[i].flags, fs[i].color_bits, fs[i].depth_bits, fs[i].alpha_bits, 0,
                           fs[i].stencil_bits);
        EXPECT_EQ(choose_gl_pixel_format(r), static_cast<i32>(i) + 1) << (i + 1);
    }
}

// ---- gdi32 exports -------------------------------------------------------------

TEST_P(PixelFormatBothBitness, DescribeWritesFullDescriptorBothBitnesses) {
    PixelFormatExports h(GetParam());
    const Handle hdc = h.new_dc();
    const auto count = static_cast<u64>(gl_pixel_formats().size());
    for (usize i = 0; i < gl_pixel_formats().size(); ++i) {
        std::array<u8, 48> poison{};
        poison.fill(0xAB);
        (void)h.memory.write_bytes(h.PFD, poison.data(), poison.size());
        // describe-nbytes-48: ret = count, 40 bytes written, nSize 40.
        EXPECT_EQ(h.call("DescribePixelFormat", {hdc, i + 1, 48, h.PFD}), count);
        const auto expect = gl_pixel_format_descriptor(gl_pixel_formats()[i]);
        std::array<u8, 48> got{};
        for (usize b = 0; b < 48; ++b) got[b] = h.memory.read_u8(h.PFD + b).value();
        EXPECT_TRUE(std::equal(expect.begin(), expect.end(), got.begin())) << (i + 1);
        EXPECT_EQ(got[40], 0xAB);  // nothing past the 40-byte struct
        EXPECT_EQ(h.memory.read_u16(h.PFD).value(), 40);       // nSize
        EXPECT_EQ(h.memory.read_u16(h.PFD + 2).value(), 1);    // nVersion
        EXPECT_EQ(h.memory.read_u32(h.PFD + 4).value(), gl_pixel_formats()[i].flags);
        // pf-list: shifts 16/8/0/0 for the 8/8/8 BGRA layout.
        EXPECT_EQ(h.memory.read_u8(h.PFD + 11).value(), 16);
        EXPECT_EQ(h.memory.read_u8(h.PFD + 13).value(), 8);
        EXPECT_EQ(h.memory.read_u8(h.PFD + 15).value(), 0);
        // cAlphaShift is 0 for alpha 0 AND alpha 8: wgl_contract_probe64.expected
        // pf-list idx 1 (alpha 0) and idx 5-8 (rgba=8/8/8/8) both print shift=16/8/0/0.
        EXPECT_EQ(h.memory.read_u8(h.PFD + 17).value(), 0);
    }
}

TEST_P(PixelFormatBothBitness, DescribeArgumentContract) {
    PixelFormatExports h(GetParam());
    const Handle hdc = h.new_dc();
    const u64 count = gl_pixel_formats().size();
    std::array<u8, 40> poison{};
    poison.fill(0xAB);
    const auto fill = [&] { (void)h.memory.write_bytes(h.PFD, poison.data(), poison.size()); };
    const auto untouched = [&] {
        for (usize b = 0; b < 40; ++b) {
            if (h.memory.read_u8(h.PFD + b).value() != 0xAB) return false;
        }
        return true;
    };
    // describe-index0 / describe-index-count+1: 0 and ERROR_INVALID_PARAMETER (87).
    fill();
    EXPECT_EQ(h.call("DescribePixelFormat", {hdc, 0, 40, h.PFD}), 0u);
    EXPECT_EQ(h.last_error(), 87u);
    EXPECT_EQ(h.call("DescribePixelFormat", {hdc, count + 1, 40, h.PFD}), 0u);
    EXPECT_EQ(h.last_error(), 87u);
    // describe-nbytes-39: 0, 87, struct untouched.
    fill();
    EXPECT_EQ(h.call("DescribePixelFormat", {hdc, 1, 39, h.PFD}), 0u);
    EXPECT_EQ(h.last_error(), 87u);
    EXPECT_TRUE(untouched());
    // describe-nbytes-0: the count, nothing written, last error untouched.
    fill();
    EXPECT_EQ(h.call("DescribePixelFormat", {hdc, 1, 0, h.PFD}), count);
    EXPECT_EQ(h.last_error(), 0xDEADu);
    EXPECT_TRUE(untouched());
    // describe-null-ppfd: the count.
    EXPECT_EQ(h.call("DescribePixelFormat", {hdc, 1, 40, 0}), count);
    EXPECT_EQ(h.last_error(), 0xDEADu);
    // describe-null-hdc: 0 with ERROR_INVALID_HANDLE (6).
    EXPECT_EQ(h.call("DescribePixelFormat", {0, 1, 40, h.PFD}), 0u);
    EXPECT_EQ(h.last_error(), 6u);
}

TEST_P(PixelFormatBothBitness, ChoosePixelFormatExportReadsTheDescriptor) {
    PixelFormatExports h(GetParam());
    const Handle hdc = h.new_dc();
    // choose-baseline shape: window | OpenGL | double buffer, colour 32, depth 24.
    h.write_request(kWin | kGl | kDb, 0, 32, 0, 0, 24, 0);
    EXPECT_EQ(h.call("ChoosePixelFormat", {hdc, h.PFD}),
              static_cast<u64>(find_format(true, 24, 0, 0)));
    EXPECT_EQ(h.last_error(), 0xDEADu);  // choose-*: gle untouched on success
    // choose-nsize-0: nSize and nVersion are not validated.
    h.write_request(kWin | kGl | kDb, 0, 32, 0, 0, 24, 0);
    (void)h.memory.write_u16(h.PFD, 0);
    (void)h.memory.write_u16(h.PFD + 2, 7);
    EXPECT_EQ(h.call("ChoosePixelFormat", {hdc, h.PFD}),
              static_cast<u64>(find_format(true, 24, 0, 0)));
    // choose-alpha-8.
    h.write_request(kWin | kGl | kDb, 0, 32, 8, 0, 24, 0);
    EXPECT_EQ(h.call("ChoosePixelFormat", {hdc, h.PFD}),
              static_cast<u64>(find_format(true, 24, 0, 8)));
    // choose-null-hdc: 0, ERROR_INVALID_HANDLE.
    EXPECT_EQ(h.call("ChoosePixelFormat", {0, h.PFD}), 0u);
    EXPECT_EQ(h.last_error(), 6u);
    // choose-null-ppfd faults on Windows (EXC=0xc0000005); Vitreo keeps its
    // existing behaviour for the NULL pointer: 0 and ERROR_INVALID_PARAMETER.
    EXPECT_EQ(h.call("ChoosePixelFormat", {hdc, 0}), 0u);
    EXPECT_EQ(h.last_error(), 87u);
}

TEST_P(PixelFormatBothBitness, SetPixelFormatValidatesIndexAndKeepsOnceOnlyRule) {
    PixelFormatExports h(GetParam());
    const Handle hdc = h.new_dc();
    const u64 count = gl_pixel_formats().size();
    h.write_request(0, 0, 0, 0, 0, 0, 0);
    // setpf-index0 / -index-count+1 / -index-neg: 0 and ERROR_INVALID_PARAMETER (87).
    EXPECT_EQ(h.call("SetPixelFormat", {hdc, 0, h.PFD}), 0u);
    EXPECT_EQ(h.last_error(), 87u);
    EXPECT_EQ(h.call("SetPixelFormat", {hdc, count + 1, h.PFD}), 0u);
    EXPECT_EQ(h.last_error(), 87u);
    EXPECT_EQ(h.call("SetPixelFormat", {hdc, 0xFFFFFFFFu, h.PFD}), 0u);
    EXPECT_EQ(h.last_error(), 87u);
    EXPECT_EQ(h.call("GetPixelFormat", {hdc}), 0u);  // getpf-unset
    // setpf-null-ppfd: accepted.
    EXPECT_EQ(h.call("SetPixelFormat", {hdc, 3, 0}), 1u);
    EXPECT_EQ(h.call("GetPixelFormat", {hdc}), 3u);
    // setpf-second-different: 0 and ERROR_INVALID_PIXEL_FORMAT (2000); the first stays.
    EXPECT_EQ(h.call("SetPixelFormat", {hdc, 1, h.PFD}), 0u);
    EXPECT_EQ(h.last_error(), 2000u);
    EXPECT_EQ(h.call("GetPixelFormat", {hdc}), 3u);
    // gfx_media_exports_probe.expected: the same format again is TRUE.
    EXPECT_EQ(h.call("SetPixelFormat", {hdc, 3, h.PFD}), 1u);
    // setpf-null-hdc: ERROR_INVALID_HANDLE.
    EXPECT_EQ(h.call("SetPixelFormat", {0, 1, h.PFD}), 0u);
    EXPECT_EQ(h.last_error(), 6u);
}

// Final review, Finding 7: a pixel format belongs to its window (or, for a
// windowless DC, to the DC). DestroyWindow forgets the window's, DeleteDC a
// memory DC's, so a reused HWND or DC handle value starts without one and
// can take a different format.
TEST(GlPixelFormats, DestroyedWindowsAndDeletedDcsForgetTheirFormat) {
    HandleTable handles;
    GdiManager mgr(handles);
    const i32 count = static_cast<i32>(gl_pixel_formats().size());
    constexpr Handle kWindow = 0x00010A2C;
    const Handle wdc = mgr.get_dc(kWindow);
    ASSERT_EQ(mgr.set_pixel_format(wdc, 3, count), 0u);
    EXPECT_EQ(mgr.pixel_format(wdc), 3);
    mgr.window_destroyed(kWindow);
    EXPECT_EQ(mgr.pixel_format(wdc), 0);
    EXPECT_EQ(mgr.set_pixel_format(wdc, 5, count), 0u);   // a different format on the reused value
    EXPECT_EQ(mgr.pixel_format(wdc), 5);

    const Handle mem = mgr.create_compatible_dc(0);
    ASSERT_NE(mem, 0u);
    ASSERT_EQ(mgr.set_pixel_format(mem, 2, count), 0u);
    EXPECT_EQ(mgr.pixel_format(mem), 2);
    EXPECT_TRUE(mgr.delete_dc(mem));
    EXPECT_EQ(mgr.pixel_format(mem), 0);
    // A window DC's release does not take the window's format with it.
    EXPECT_TRUE(mgr.delete_dc(wdc));
    const Handle again = mgr.get_dc(kWindow);
    EXPECT_EQ(mgr.pixel_format(again), 5);
}

INSTANTIATE_TEST_SUITE_P(Bitness, PixelFormatBothBitness, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

// ---- Format -> framebuffer ---------------------------------------------------

TEST(GlPixelFormats, FramebufferConfigMirrorsTheDescriptor) {
    for (const auto& f : gl_pixel_formats()) {
        const GlFramebufferConfig c = gl_framebuffer_config(f);
        EXPECT_EQ(c.red_bits, f.red_bits);
        EXPECT_EQ(c.green_bits, f.green_bits);
        EXPECT_EQ(c.blue_bits, f.blue_bits);
        EXPECT_EQ(c.alpha_bits, f.alpha_bits);
        EXPECT_EQ(c.depth_bits, f.depth_bits);
        EXPECT_EQ(c.stencil_bits, f.stencil_bits);
        EXPECT_EQ(c.double_buffer, f.double_buffer);
        // gl11_state_probe64.expected get-GL_INDEX_BITS = 8 on a 32-bit RGBA window format.
        EXPECT_EQ(c.index_bits, 8);
        // The descriptor's cAccumBits is 0, so every accumulation bit count is 0.
        EXPECT_EQ(c.accum_red_bits + c.accum_green_bits + c.accum_blue_bits + c.accum_alpha_bits, 0);
        EXPECT_EQ(c.aux_buffers, 0);
        EXPECT_FALSE(c.stereo);
    }
}
