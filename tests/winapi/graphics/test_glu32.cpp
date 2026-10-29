/// @file test_glu32.cpp
/// @brief glu32.dll on Vitreo's opengl32, through the registered exports in
///        both bitnesses: strings, the matrix helpers (on the current matrix,
///        and compiled into an open display list), gluProject/gluUnProject,
///        gluScaleImage, gluBuild{1,2}DMipmaps, the quadrics (their GL
///        command sequence against Microsoft glu32's, read back through
///        GL_FEEDBACK on winoracle) with their GLU_ERROR callback, and the
///        tessellator / NURBS exports that fail loudly.
///
/// Expected values are Learn's / the GLU 1.2 specification's, and where those
/// leave a value open, Microsoft glu32's on GDI Generic
/// (tools/oracle_probe/gl11_state_probe64.expected, glu-* tags cited at each
/// assertion; glu32_oracle_feedback.inc carries its glu-fb-* lines). GDI
/// Generic keeps matrices in single precision, so matrix values compare to
/// 1e-6.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/graphics/glu32/glu32.hpp"
#include "winapi/graphics/glu32/glu32_image.hpp"
#include "winapi/graphics/glu32/glu32_quadric.hpp"
#include "winapi/graphics/opengl32/gl_commands.hpp"
#include "winapi/graphics/opengl32/gl_context.hpp"
#include "winapi/graphics/opengl32/gl_enums.hpp"
#include "winapi/graphics/opengl32/opengl32.hpp"

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::gl;

namespace {

u32 fbits(float f) {
    u32 u;
    std::memcpy(&u, &f, 4);
    return u;
}
u64 dbits(double d) {
    u64 u;
    std::memcpy(&u, &d, 8);
    return u;
}

struct I { u32 v; };
struct F { float v; };
struct D { double v; };
struct P { GuestAddr v; };
using Arg = std::variant<I, F, D, P>;

constexpr u32 GLU_INVALID_ENUM = 100900;
constexpr u32 GLU_INVALID_VALUE = 100901;

/// A guest caller of the registered opengl32 and glu32 exports (x86 stdcall
/// frame or x64 registers, garbage in every unused register).
class Harness {
public:
    static constexpr GuestAddr MEM = 0x100000;       // 0x40000 bytes, read/write
    static constexpr GuestAddr STACK = 0x80000;
    static constexpr GuestAddr OUT = MEM + 0x100;    // query results
    static constexpr GuestAddr DATA = MEM + 0x1000;  // client images, matrices
    static constexpr GuestAddr DST = MEM + 0x20000;  // image results
    static constexpr GuestAddr HEAP = MEM + 0x30000; // gluNewQuadric's allocations
    GuestAddr heap_next = HEAP;
    std::vector<GuestAddr> freed;

    explicit Harness(bool x64) : m_x64(x64) {
        EXPECT_TRUE(memory.map_region(MEM, 0x40000, MemoryProtection::ReadWrite).is_ok());
        EXPECT_TRUE(memory.map_region(STACK, 0x2000, MemoryProtection::ReadWrite).is_ok());
        EXPECT_TRUE(memory.map_region(OPENGL32_SCRATCH_BASE, OPENGL32_SCRATCH_SIZE,
                                      MemoryProtection::ReadWrite).is_ok());
        EXPECT_TRUE(memory.map_region(GLU32_SCRATCH_BASE, GLU32_SCRATCH_SIZE, MemoryProtection::ReadWrite).is_ok());
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        registry = std::make_unique<ThunkRegistry>(0x70000000);
        gl.set_thunk_registry(registry.get());
        EXPECT_TRUE(registry->register_dll("opengl32.dll", create_opengl32_thunks(gl)).is_ok());
        EXPECT_TRUE(registry->register_dll("glu32.dll", create_glu32_thunks(glu)).is_ok());
        glu.set_guest_call([this](GuestAddr fn, const u64* args, usize n, bool x64_call) -> u64 {
            callbacks.push_back({fn, n > 0 ? static_cast<u32>(args[0]) : 0u, x64_call});
            return 0;
        });
        // A bump allocator standing in for the process heap.
        glu.set_heap(Glu32Manager::Heap{[this](u64 size) -> GuestAddr {
                                            if (heap_next + size > HEAP + 0x10000) return 0;
                                            const GuestAddr a = heap_next;
                                            heap_next += (size + 15) & ~u64{15};
                                            return a;
                                        },
                                        [this](GuestAddr a) { freed.push_back(a); }});
    }

    u64 call(std::string_view name, std::vector<Arg> args) {
        const std::string n(name);
        const char* dll = n.rfind("glu", 0) == 0 ? "glu32.dll" : "opengl32.dll";
        auto addr = registry->get_thunk_address(dll, n);
        EXPECT_TRUE(addr.is_ok()) << n;
        if (!addr.is_ok()) return ~0ull;
        auto handler = registry->resolve_handler(addr.value());
        EXPECT_TRUE(handler.is_ok()) << n;
        if (!handler.is_ok()) return ~0ull;
        const GuestAddr frame = STACK + 0x1000;
        cpu.set_sp(frame);
        cpu.gpr.rax = 0xCCCCCCCCCCCCCCCCull;
        cpu.gpr.rcx = cpu.gpr.rdx = cpu.gpr.r8 = cpu.gpr.r9 = 0xDDDDDDDDDDDDDDDDull;
        for (auto& x : cpu.sse.xmm) x.u64_[0] = x.u64_[1] = 0xEEEEEEEEEEEEEEEEull;
        if (m_x64) {
            (void)memory.write_u64(frame, 0xDEADBEEF);
            for (usize i = 0; i < args.size(); ++i) {
                u64 bits = 0;
                bool fp = false;
                if (auto* a = std::get_if<I>(&args[i])) bits = 0xABCD000000000000ull | a->v;
                else if (auto* f = std::get_if<F>(&args[i])) { bits = 0x7777777700000000ull | fbits(f->v); fp = true; }
                else if (auto* d = std::get_if<D>(&args[i])) { bits = dbits(d->v); fp = true; }
                else bits = std::get<P>(args[i]).v;
                if (i < 4) {
                    if (fp) cpu.sse.xmm[i].u64_[0] = bits;
                    else (i == 0 ? cpu.gpr.rcx : i == 1 ? cpu.gpr.rdx : i == 2 ? cpu.gpr.r8 : cpu.gpr.r9) = bits;
                } else {
                    (void)memory.write_u64(frame + 0x28 + (i - 4) * 8, bits);
                }
            }
        } else {
            (void)memory.write_u32(frame, 0xDEADBEEF);
            GuestAddr at = frame + 4;
            u32 bytes = 0;
            for (const Arg& arg : args) {
                if (auto* a = std::get_if<I>(&arg)) { (void)memory.write_u32(at, a->v); at += 4; bytes += 4; }
                else if (auto* f = std::get_if<F>(&arg)) { (void)memory.write_u32(at, fbits(f->v)); at += 4; bytes += 4; }
                else if (auto* d = std::get_if<D>(&arg)) { (void)memory.write_u64(at, dbits(d->v)); at += 8; bytes += 8; }
                else { (void)memory.write_u32(at, static_cast<u32>(std::get<P>(arg).v)); at += 4; bytes += 4; }
            }
            EXPECT_EQ(registry->resolve_stdcall_bytes(addr.value()), bytes) << n;
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        (*handler.value())(m);
        return m_x64 ? cpu.gpr.rax : (cpu.gpr.rax & 0xFFFFFFFFull);
    }

    [[nodiscard]] bool x64() const noexcept { return m_x64; }
    u32 err() { return static_cast<u32>(call("glGetError", {})); }
    std::vector<double> getd(u32 pname, u32 n) {
        call("glGetDoublev", {I{pname}, P{OUT}});
        std::vector<double> v(n);
        EXPECT_TRUE(memory.read_bytes(OUT, reinterpret_cast<Byte*>(v.data()), n * 8).is_ok());
        return v;
    }
    std::vector<float> getf(u32 pname, u32 n) {
        call("glGetFloatv", {I{pname}, P{OUT}});
        std::vector<float> v(n);
        EXPECT_TRUE(memory.read_bytes(OUT, reinterpret_cast<Byte*>(v.data()), n * 4).is_ok());
        return v;
    }
    i32 geti(u32 pname) {
        call("glGetIntegerv", {I{pname}, P{OUT}});
        i32 v = 0;
        EXPECT_TRUE(memory.read_bytes(OUT, reinterpret_cast<Byte*>(&v), 4).is_ok());
        return v;
    }
    i32 tex_level(u32 target, i32 level, u32 pname) {
        call("glGetTexLevelParameteriv", {I{target}, I{static_cast<u32>(level)}, I{pname}, P{OUT}});
        i32 v = 0;
        EXPECT_TRUE(memory.read_bytes(OUT, reinterpret_cast<Byte*>(&v), 4).is_ok());
        return v;
    }
    void put(GuestAddr at, const std::vector<u8>& bytes) {
        (void)memory.write_bytes(at, bytes.data(), bytes.size());
    }
    std::vector<u8> bytes(GuestAddr at, usize n) {
        std::vector<u8> v(n);
        EXPECT_TRUE(memory.read_bytes(at, reinterpret_cast<Byte*>(v.data()), n).is_ok());
        return v;
    }
    std::string ansi(GuestAddr at) {
        std::string s;
        for (u32 k = 0; k < 256; ++k) {
            auto b = memory.read_u8(at + k);
            if (!b.is_ok() || b.value() == 0) break;
            s.push_back(static_cast<char>(b.value()));
        }
        return s;
    }
    std::string wide(GuestAddr at) {
        std::string s;
        for (u32 k = 0; k < 256; ++k) {
            auto u = memory.read_u16(at + 2 * k);
            if (!u.is_ok() || u.value() == 0) break;
            s.push_back(u.value() < 0x80 ? static_cast<char>(u.value()) : '?');
        }
        return s;
    }
    void put_matrix(GuestAddr at, const std::array<double, 16>& m) {
        (void)memory.write_bytes(at, reinterpret_cast<const u8*>(m.data()), sizeof(double) * 16);
    }

    struct Callback {
        GuestAddr fn;
        u32 code;
        bool x64;
    };
    std::vector<Callback> callbacks;

    GuestMemory memory;
    cpu::CpuState cpu;
    Opengl32Manager gl;
    Glu32Manager glu;
    std::unique_ptr<ThunkRegistry> registry;

private:
    bool m_x64;
};

struct CurrentContext {
    CurrentContext() : ctx(GlFramebufferConfig{}, nullptr) {
        ctx.on_first_make_current(320, 200);
        set_current_gl_context_for_thread(&ctx);
    }
    ~CurrentContext() { set_current_gl_context_for_thread(nullptr); }
    GlContext ctx;
};

class Glu32Test : public ::testing::TestWithParam<bool> {
protected:
    Harness h{GetParam()};
    CurrentContext current;

    void expect_matrix(u32 pname, const std::vector<double>& want, const char* tag) {
        const auto got = h.getd(pname, 16);
        for (usize k = 0; k < 16; ++k) EXPECT_NEAR(got[k], want[k], 1e-6) << tag << " m[" << k << "]";
    }
    void identity_stacks() {
        h.call("glMatrixMode", {I{GL_PROJECTION}});
        h.call("glLoadIdentity", {});
        h.call("glMatrixMode", {I{GL_MODELVIEW}});
        h.call("glLoadIdentity", {});
    }
};

std::string bitness_name(const ::testing::TestParamInfo<bool>& info) { return info.param ? "x64" : "x86"; }

const std::vector<double> kIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

// ---- ErrorStringMatchesWindows -----------------------------------------------------------

TEST_P(Glu32Test, ErrorStringMatchesWindows) {
    struct Case {
        u32 code;
        const char* text;   // nullptr: NULL
    };
    const Case cases[] = {
        {0x0000, "no error"},                              // glu-error-0x0000
        {0x0500, "invalid enumerant"},                     // glu-error-0x0500
        {0x0501, "invalid value"},
        {0x0502, "invalid operation"},
        {0x0503, "stack overflow"},
        {0x0504, "stack underflow"},
        {0x0505, "out of memory"},
        {100900, "invalid enumerant"},                     // glu-error-0x18A24
        {100901, "invalid value"},
        {100902, "out of memory"},
        {100903, nullptr},                                 // GLU_INCOMPATIBLE_GL_VERSION
        {100904, nullptr},                                 // GLU_INVALID_OPERATION
        {0x0506, nullptr},
        {0x1234, nullptr},
        {100151, "missing gluTessBeginPolygon"},           // glu-error-0x18737
        {100155, "tesselation coordinate too large"},
        {100156, "need combine callback"},
        {100157, nullptr},                                 // glu-error-0x1873D
        {100158, nullptr},
        {100150, nullptr},
        {100251, "spline order un-supported"},             // glu-error-0x1879B
        {100261, "missing or extra domain data"},
        {100280, "UNUSED"},
        {100284, "negative byte-stride encounteed"},
        {100287, "duplicate point on pwlcurve"},           // glu-error-0x187BF
        {100288, nullptr},
    };
    for (const Case& c : cases) {
        const u64 a = h.call("gluErrorString", {I{c.code}});
        const u64 w = h.call("gluErrorUnicodeStringEXT", {I{c.code}});
        if (c.text == nullptr) {
            EXPECT_EQ(a, 0u) << c.code;
            EXPECT_EQ(w, 0u) << c.code;
            continue;
        }
        ASSERT_NE(a, 0u) << c.code;
        ASSERT_NE(w, 0u) << c.code;
        EXPECT_EQ(h.ansi(static_cast<GuestAddr>(a)), c.text) << c.code;
        EXPECT_EQ(h.wide(static_cast<GuestAddr>(w)), c.text) << c.code;   // glu-errorw-* ansi-equal=1
        // glu-errorw-* stable=1: the same pointer every call.
        EXPECT_EQ(h.call("gluErrorString", {I{c.code}}), a);
        EXPECT_EQ(h.call("gluErrorUnicodeStringEXT", {I{c.code}}), w);
    }
    // glu-getstring, glu-getstring-stable.
    const u64 v = h.call("gluGetString", {I{100800}});
    const u64 e = h.call("gluGetString", {I{100801}});
    ASSERT_NE(v, 0u);
    ASSERT_NE(e, 0u);
    EXPECT_EQ(h.ansi(static_cast<GuestAddr>(v)), "1.2.2.0 Microsoft Corporation");
    EXPECT_EQ(h.ansi(static_cast<GuestAddr>(e)), "GL_EXT_bgra");
    EXPECT_EQ(h.call("gluGetString", {I{100800}}), v);
    EXPECT_EQ(h.call("gluGetString", {I{0x1234}}), 0u);
    // No context needed (glu-noctx-strings).
    set_current_gl_context_for_thread(nullptr);
    EXPECT_EQ(h.ansi(static_cast<GuestAddr>(h.call("gluGetString", {I{100800}}))), "1.2.2.0 Microsoft Corporation");
    EXPECT_EQ(h.ansi(static_cast<GuestAddr>(h.call("gluErrorString", {I{0x0500}}))), "invalid enumerant");
}

// ---- PerspectiveMultipliesCurrentMatrix --------------------------------------------------

TEST_P(Glu32Test, PerspectiveMultipliesCurrentMatrix) {
    identity_stacks();
    h.call("glMatrixMode", {I{GL_PROJECTION}});
    h.call("gluPerspective", {D{60}, D{1.5}, D{0.5}, D{100}});
    expect_matrix(GL_PROJECTION_MATRIX,   // glu-perspective-60-1.5-0.5-100
                  {1.1547005176544189, 0, 0, 0, 0, 1.7320507764816284, 0, 0, 0, 0, -1.0100502967834473, -1, 0, 0,
                   -1.0050251483917236, 0},
                  "60-1.5-0.5-100");
    // It multiplies the current matrix (glu-perspective-90-1-1-3-after-translate123).
    h.call("glLoadIdentity", {});
    h.call("glTranslated", {D{1}, D{2}, D{3}});
    h.call("gluPerspective", {D{90}, D{1}, D{1}, D{3}});
    expect_matrix(GL_PROJECTION_MATRIX, {1, 0, 0, 0, 0, 1, 0, 0, -1, -2, -5, -1, 0, 0, -3, 0}, "after-translate");
    // Degenerate arguments change nothing (glu-perspective-aspect0, -near-eq-far, -fovy0).
    for (const auto& args : std::vector<std::array<double, 4>>{{60, 0, 1, 10}, {60, 1, 2, 2}, {0, 1, 1, 10}}) {
        h.call("glLoadIdentity", {});
        h.call("gluPerspective", {D{args[0]}, D{args[1]}, D{args[2]}, D{args[3]}});
        expect_matrix(GL_PROJECTION_MATRIX, kIdentity, "degenerate");
    }
    // glu-perspective-negative-aspect-near: computed as given.
    h.call("glLoadIdentity", {});
    h.call("gluPerspective", {D{45}, D{-2}, D{-1}, D{10}});
    expect_matrix(GL_PROJECTION_MATRIX,
                  {-1.2071068286895752, 0, 0, 0, 0, 2.4142136573791504, 0, 0, 0, 0, -0.81818181276321411, -1, 0, 0,
                   1.8181818723678589, 0},
                  "negative");
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    // Inside glBegin the GL command GLU issues is refused (glu-ortho2d-inside-begin).
    h.call("glLoadIdentity", {});
    h.call("glBegin", {I{GL_POINTS}});
    h.call("gluPerspective", {D{60}, D{1}, D{1}, D{10}});
    h.call("glEnd", {});
    EXPECT_EQ(h.err(), GL_INVALID_OPERATION);
    expect_matrix(GL_PROJECTION_MATRIX, kIdentity, "inside-begin");
}

// ---- LookAtAndOrtho2D --------------------------------------------------------------------

TEST_P(Glu32Test, LookAtAndOrtho2D) {
    identity_stacks();
    h.call("gluLookAt", {D{1}, D{2}, D{3}, D{0}, D{0}, D{0}, D{0}, D{1}, D{0}});
    const std::vector<double> lookat123{0.94868332147598267, -0.16903086006641388, 0.26726123690605164, 0, 0,
                                        0.8451542854309082, 0.53452247381210327, 0, -0.31622779369354248,
                                        -0.50709253549575806, 0.80178368091583252, 0, 5.9604644775390625e-08, 0,
                                        -3.7416572570800781, 1};
    expect_matrix(GL_MODELVIEW_MATRIX, lookat123, "glu-lookat-123-000-010");
    h.call("glLoadIdentity", {});
    h.call("gluLookAt", {D{0.1}, D{-7.3}, D{2.9}, D{4.4}, D{1.25}, D{-3.3}, D{0.3}, D{0.2}, D{1.7}});
    expect_matrix(GL_MODELVIEW_MATRIX,
                  {0.86079341173171997, 0.34181797504425049, -0.37708783149719238, 0, -0.50037878751754761,
                   0.43293732404708862, -0.74979090690612793, 0, -0.093036621809005737, 0.83410179615020752,
                   0.54370802640914917, 0, -3.4690384864807129, 0.70736551284790039, -7.0125179290771484, 1},
                  "glu-lookat-irrational");
    // Degenerate: a zero vector stays zero (glu-lookat-eye-eq-center, -up-parallel, -up-zero).
    h.call("glLoadIdentity", {});
    h.call("gluLookAt", {D{1}, D{2}, D{3}, D{1}, D{2}, D{3}, D{0}, D{1}, D{0}});
    expect_matrix(GL_MODELVIEW_MATRIX, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, "eye-eq-center");
    h.call("glLoadIdentity", {});
    h.call("gluLookAt", {D{0}, D{0}, D{0}, D{0}, D{5}, D{0}, D{0}, D{1}, D{0}});
    expect_matrix(GL_MODELVIEW_MATRIX, {0, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 1}, "up-parallel");
    h.call("glLoadIdentity", {});
    h.call("gluLookAt", {D{0}, D{0}, D{0}, D{0}, D{0}, D{-1}, D{0}, D{0}, D{0}});
    expect_matrix(GL_MODELVIEW_MATRIX, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, "up-zero");

    // gluOrtho2D is glOrtho(..., -1, 1) (glu-ortho2d-0-320-0-200, -m1.5-2.5-3-m7).
    h.call("glMatrixMode", {I{GL_PROJECTION}});
    h.call("glLoadIdentity", {});
    h.call("gluOrtho2D", {D{0}, D{320}, D{0}, D{200}});
    expect_matrix(GL_PROJECTION_MATRIX,
                  {0.0062500000931322575, 0, 0, 0, 0, 0.0099999997764825821, 0, 0, 0, 0, -1, 0, -1, -1, 0, 1},
                  "ortho2d-320-200");
    h.call("glLoadIdentity", {});
    h.call("gluOrtho2D", {D{-1.5}, D{2.5}, D{3}, D{-7}});
    expect_matrix(GL_PROJECTION_MATRIX,
                  {0.5, 0, 0, 0, 0, -0.20000000298023224, 0, 0, 0, 0, -1, 0, -0.25, -0.40000000596046448, 0, 1},
                  "ortho2d-m1.5");
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    h.call("glLoadIdentity", {});
    h.call("gluOrtho2D", {D{1}, D{1}, D{0}, D{200}});   // glu-ortho2d-left-eq-right: glOrtho's error
    EXPECT_EQ(h.err(), GL_INVALID_VALUE);
    expect_matrix(GL_PROJECTION_MATRIX, kIdentity, "left-eq-right");

    // gluPickMatrix (glu-pickmatrix-100-50-5-7-vp10-20-320-200, -width0, -height-negative).
    const GuestAddr vp = Harness::DATA;
    h.put(vp, {10, 0, 0, 0, 20, 0, 0, 0, 0x40, 1, 0, 0, 200, 0, 0, 0});
    h.call("glLoadIdentity", {});
    h.call("gluPickMatrix", {D{100}, D{50}, D{5}, D{7}, P{vp}});
    expect_matrix(GL_PROJECTION_MATRIX, {64, 0, 0, 0, 0, 28.571428298950195, 0, 0, 0, 0, 1, 0, 28, 20, 0, 1},
                  "pickmatrix");
    h.call("glLoadIdentity", {});
    h.call("gluPickMatrix", {D{100}, D{50}, D{0}, D{7}, P{vp}});
    expect_matrix(GL_PROJECTION_MATRIX, kIdentity, "pick-width0");
    h.call("gluPickMatrix", {D{100}, D{50}, D{5}, D{-1}, P{vp}});
    expect_matrix(GL_PROJECTION_MATRIX, kIdentity, "pick-height-negative");
}

// ---- MatrixHelpersCompileIntoDisplayList -------------------------------------------------

// A real glu32 calls opengl32's exports, so a GLU helper inside
// glNewList/glEndList is compiled into the list (spec 5.4;
// glu-lookat-compiled-before-call / -after-call).
TEST_P(Glu32Test, MatrixHelpersCompileIntoDisplayList) {
    identity_stacks();
    h.call("glNewList", {I{300}, I{GL_COMPILE}});
    h.call("gluLookAt", {D{1}, D{2}, D{3}, D{0}, D{0}, D{0}, D{0}, D{1}, D{0}});
    h.call("glEndList", {});
    expect_matrix(GL_MODELVIEW_MATRIX, kIdentity, "lookat-before-call");
    h.call("glCallList", {I{300}});
    const auto mv = h.getd(GL_MODELVIEW_MATRIX, 16);
    EXPECT_NEAR(mv[0], 0.94868332147598267, 1e-6);
    EXPECT_NEAR(mv[14], -3.7416572570800781, 1e-6);

    h.call("glMatrixMode", {I{GL_PROJECTION}});
    h.call("glNewList", {I{301}, I{GL_COMPILE}});
    h.call("gluPerspective", {D{90}, D{1}, D{1}, D{3}});
    h.call("glEndList", {});
    expect_matrix(GL_PROJECTION_MATRIX, kIdentity, "perspective-before-call");
    h.call("glCallList", {I{301}});
    expect_matrix(GL_PROJECTION_MATRIX, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -2, -1, 0, 0, -3, 0}, "perspective-call");
    // GL_COMPILE_AND_EXECUTE runs it as well.
    h.call("glLoadIdentity", {});
    h.call("glNewList", {I{302}, I{GL_COMPILE_AND_EXECUTE}});
    h.call("gluOrtho2D", {D{0}, D{320}, D{0}, D{200}});
    h.call("glEndList", {});
    EXPECT_NEAR(h.getd(GL_PROJECTION_MATRIX, 16)[0], 0.00625, 1e-9);
    EXPECT_EQ(h.err(), GL_NO_ERROR);
}

// ---- ProjectAndUnProject -----------------------------------------------------------------

TEST_P(Glu32Test, ProjectAndUnProject) {
    // glu-project: the probe's matrices (model translates by (0.5, -0.25, -3);
    // proj is GDI's float gluPerspective(60, 1.6, 1, 10), as glGetDoublev read it).
    const GuestAddr mv = Harness::DATA, pr = Harness::DATA + 0x80, vp = Harness::DATA + 0x100;
    const GuestAddr wx = Harness::DATA + 0x200, wy = wx + 8, wz = wx + 16;
    h.put_matrix(mv, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5, -0.25, -3, 1});
    const float f = static_cast<float>(1.0 / std::tan(30.0 * 3.14159265358979323846 / 180.0));
    h.put_matrix(pr, {static_cast<float>(f / 1.6), 0, 0, 0, 0, f, 0, 0, 0, 0, static_cast<float>(-11.0 / 9.0), -1, 0, 0,
                      static_cast<float>(-20.0 / 9.0), 0});
    h.put(vp, {10, 0, 0, 0, 20, 0, 0, 0, 0x40, 1, 0, 0, 200, 0, 0, 0});
    auto readd = [&](GuestAddr at) {
        double d = 0;
        EXPECT_TRUE(h.memory.read_bytes(at, reinterpret_cast<Byte*>(&d), 8).is_ok());
        return d;
    };
    EXPECT_EQ(h.call("gluProject", {D{0.3}, D{0.7}, D{-1.1}, P{mv}, P{pr}, P{vp}, P{wx}, P{wy}, P{wz}}), 1u);
    // proj is rebuilt here from GDI's single-precision arithmetic, not read
    // back bit for bit, hence 1e-4 window units.
    EXPECT_NEAR(readd(wx), 203.79611503787157, 1e-4);
    EXPECT_NEAR(readd(wy), 139.01031340040811, 1e-4);
    EXPECT_NEAR(readd(wz), 0.84010838153885636, 1e-6);
    // glu-unproject-roundtrip
    const GuestAddr ox = Harness::DATA + 0x300, oy = ox + 8, oz = ox + 16;
    EXPECT_EQ(h.call("gluUnProject", {D{readd(wx)}, D{readd(wy)}, D{readd(wz)}, P{mv}, P{pr}, P{vp}, P{ox}, P{oy},
                                      P{oz}}),
              1u);
    EXPECT_NEAR(readd(ox), 0.3, 1e-9);
    EXPECT_NEAR(readd(oy), 0.7, 1e-9);
    EXPECT_NEAR(readd(oz), -1.1, 1e-9);
    // glu-unproject-100-50-0.25
    EXPECT_EQ(h.call("gluUnProject", {D{100}, D{50}, D{0.25}, P{mv}, P{pr}, P{vp}, P{ox}, P{oy}, P{oz}}), 1u);
    EXPECT_NEAR(readd(ox), -1.0214776648391253, 1e-4);
    EXPECT_NEAR(readd(oy), -0.27147770073009492, 1e-4);
    EXPECT_NEAR(readd(oz), 1.7096773479036929, 1e-4);
    // glu-project-w0 / glu-unproject-singular: GL_FALSE, outputs untouched.
    const GuestAddr zero = Harness::DATA + 0x400;
    h.put_matrix(zero, {});
    const double seven = 7;
    for (GuestAddr at : {wx, wy, wz, ox, oy, oz}) (void)h.memory.write_bytes(at, reinterpret_cast<const u8*>(&seven), 8);
    EXPECT_EQ(h.call("gluProject", {D{1}, D{2}, D{3}, P{mv}, P{zero}, P{vp}, P{wx}, P{wy}, P{wz}}), 0u);
    EXPECT_EQ(readd(wx), 7.0);
    EXPECT_EQ(h.call("gluUnProject", {D{1}, D{2}, D{0.5}, P{zero}, P{pr}, P{vp}, P{ox}, P{oy}, P{oz}}), 0u);
    EXPECT_EQ(readd(ox), 7.0);
    // glu-project-identity, and with no context (glu-noctx-project).
    const GuestAddr id = Harness::DATA + 0x500;
    h.put_matrix(id, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    set_current_gl_context_for_thread(nullptr);
    EXPECT_EQ(h.call("gluProject", {D{0.5}, D{-0.5}, D{0.25}, P{id}, P{id}, P{vp}, P{wx}, P{wy}, P{wz}}), 1u);
    EXPECT_DOUBLE_EQ(readd(wx), 250);
    EXPECT_DOUBLE_EQ(readd(wy), 70);
    EXPECT_DOUBLE_EQ(readd(wz), 0.625);
}

// ---- ScaleImageMatchesSpecBoxFilter ------------------------------------------------------

struct ScaleCase {
    const char* tag;          // the glu-scale-* line
    u32 format;
    i32 wi, hi;
    u32 type_in;
    std::vector<u8> in;
    i32 wo, ho;
    u32 type_out;
    std::vector<u8> out;      // the measured dataout bytes (0xCC sentinel where untouched)
    u32 ret{0};
    i32 align{4};             // UNPACK_ALIGNMENT and PACK_ALIGNMENT
};

std::vector<u8> le16(std::initializer_list<u16> v) {
    std::vector<u8> b;
    for (u16 x : v) { b.push_back(static_cast<u8>(x)); b.push_back(static_cast<u8>(x >> 8)); }
    return b;
}
std::vector<u8> le32(std::initializer_list<u32> v) {
    std::vector<u8> b;
    for (u32 x : v) for (int k = 0; k < 4; ++k) b.push_back(static_cast<u8>(x >> (8 * k)));
    return b;
}
std::vector<u8> lef(std::initializer_list<float> v) {
    std::vector<u8> b;
    for (float x : v) for (int k = 0; k < 4; ++k) b.push_back(static_cast<u8>(fbits(x) >> (8 * k)));
    return b;
}

TEST_P(Glu32Test, ScaleImageMatchesSpecBoxFilter) {
    const std::vector<u8> lum4x4{0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 255};
    const std::vector<u8> rgb3x2_align4{10, 20, 30, 40, 50, 60, 70, 80, 90, 0xEE, 0xEE, 0xEE,
                                        100, 110, 120, 130, 140, 150, 160, 170, 180};
    const std::vector<ScaleCase> cases = {
        // GLU 1.2 3.1 / the brief's case: a 2x2 RGBA image magnified to 4x4.
        {"glu-scale-rgba-ubyte-2x2-to-4x4", GL_RGBA, 2, 2, GL_UNSIGNED_BYTE,
         {0, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255}, 4, 4, GL_UNSIGNED_BYTE,
         {0x2f, 0x2f, 0x0f, 0xff, 0x2f, 0x2f, 0x0f, 0xff, 0x8f, 0x0f, 0x2f, 0xff, 0x8f, 0x0f, 0x2f, 0xff,
          0x2f, 0x2f, 0x0f, 0xff, 0x2f, 0x2f, 0x0f, 0xff, 0x8f, 0x0f, 0x2f, 0xff, 0x8f, 0x0f, 0x2f, 0xff,
          0x0f, 0x8f, 0x2f, 0xff, 0x0f, 0x8f, 0x2f, 0xff, 0x2f, 0x2f, 0x8f, 0xff, 0x2f, 0x2f, 0x8f, 0xff,
          0x0f, 0x8f, 0x2f, 0xff, 0x0f, 0x8f, 0x2f, 0xff, 0x2f, 0x2f, 0x8f, 0xff, 0x2f, 0x2f, 0x8f, 0xff}},
        {"glu-scale-lum-ubyte-4x4-to-2x2", GL_LUMINANCE, 4, 4, GL_UNSIGNED_BYTE, lum4x4, 2, 2, GL_UNSIGNED_BYTE,
         {0x28, 0x48, 0xcc, 0xcc, 0xa8, 0xcc, 0xcc, 0xcc}},
        {"glu-scale-lum-ubyte-4x4-to-3x3-align4", GL_LUMINANCE, 4, 4, GL_UNSIGNED_BYTE, lum4x4, 3, 3,
         GL_UNSIGNED_BYTE, {0x14, 0x28, 0x3c, 0xcc, 0x64, 0x78, 0x8c, 0xcc, 0xb4, 0xc8, 0xe5, 0xcc}},
        {"glu-scale-lum-ubyte-4x4-to-5x3-align4", GL_LUMINANCE, 4, 4, GL_UNSIGNED_BYTE, lum4x4, 5, 3,
         GL_UNSIGNED_BYTE,
         {0x14, 0x1b, 0x28, 0x35, 0x3b, 0xcc, 0xcc, 0xcc, 0x65, 0x6b, 0x78, 0x85, 0x8b, 0xcc, 0xcc, 0xcc,
          0xb6, 0xbb, 0xc8, 0xd9, 0xe6, 0xcc}},
        {"glu-scale-lum-ubyte-4x1-to-3x1", GL_LUMINANCE, 4, 1, GL_UNSIGNED_BYTE, lum4x4, 3, 1, GL_UNSIGNED_BYTE,
         {0x04, 0x18, 0x2c, 0xcc}},
        {"glu-scale-lum-ubyte-3x1-to-7x1", GL_LUMINANCE, 3, 1, GL_UNSIGNED_BYTE, lum4x4, 7, 1, GL_UNSIGNED_BYTE,
         {0x09, 0x02, 0x09, 0x10, 0x16, 0x1d, 0x16, 0xcc}},
        {"glu-scale-rgb-ubyte-3x2-to-2x3-align4", GL_RGB, 3, 2, GL_UNSIGNED_BYTE, rgb3x2_align4, 2, 3,
         GL_UNSIGNED_BYTE,
         {0x23, 0x2d, 0x37, 0x4b, 0x55, 0x5f, 0xcc, 0xcc, 0x41, 0x4b, 0x55, 0x69, 0x73, 0x7d, 0xcc, 0xcc,
          0x5f, 0x69, 0x73, 0x87, 0x91, 0x9b, 0xcc, 0xcc}},
        {"glu-scale-rgb-ubyte-3x2-to-2x3-align1", GL_RGB, 3, 2, GL_UNSIGNED_BYTE,
         {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160, 170, 180}, 2, 3, GL_UNSIGNED_BYTE,
         {0x23, 0x2d, 0x37, 0x4b, 0x55, 0x5f, 0x41, 0x4b, 0x55, 0x69, 0x73, 0x7d, 0x5f, 0x69, 0x73, 0x87, 0x91, 0x9b,
          0xcc},
         0, 1},
        // Types and their 16-bit round trip (these lines ran under alignment 1).
        {"glu-scale-type-ubyte-2x1-to-3x1", GL_LUMINANCE, 2, 1, GL_UNSIGNED_BYTE, {0, 255}, 3, 1, GL_UNSIGNED_BYTE,
         {0x2a, 0x7f, 0xd5, 0xcc}, 0, 1},
        {"glu-scale-type-byte-2x1-to-3x1", GL_LUMINANCE, 2, 1, GL_BYTE, {0x80, 0x7f}, 3, 1, GL_BYTE,
         {0x7f, 0x7f, 0x7f, 0xcc}, 0, 1},
        {"glu-scale-type-ushort-2x1-to-3x1", GL_LUMINANCE, 2, 1, GL_UNSIGNED_SHORT, le16({0, 65535}), 3, 1,
         GL_UNSIGNED_SHORT, {0xaa, 0x2a, 0xff, 0x7f, 0x54, 0xd5}, 0, 1},
        {"glu-scale-type-short-2x1-to-3x1", GL_LUMINANCE, 2, 1, GL_SHORT, le16({0x8000, 0x7fff}), 3, 1, GL_SHORT,
         {0x55, 0x15, 0xff, 0x3f, 0xa9, 0x6a}, 0, 1},
        {"glu-scale-type-uint-2x1-to-3x1", GL_LUMINANCE, 2, 1, GL_UNSIGNED_INT, le32({0, 0xffffffffu}), 3, 1,
         GL_UNSIGNED_INT, {0xaa, 0x2a, 0xaa, 0x2a, 0xff, 0x7f, 0xff, 0x7f, 0x54, 0xd5, 0x54, 0xd5}, 0, 1},
        {"glu-scale-type-int-2x1-to-3x1", GL_LUMINANCE, 2, 1, GL_INT, le32({0x80000000u, 0x7fffffffu}), 3, 1, GL_INT,
         {0x55, 0x15, 0x55, 0x15, 0xff, 0xbf, 0xff, 0x3f, 0xaa, 0x6a, 0xaa, 0x6a}, 0, 1},
        {"glu-scale-type-float-2x1-to-3x1", GL_LUMINANCE, 2, 1, GL_FLOAT, lef({0.0f, 1.0f}), 3, 1, GL_FLOAT,
         {0xab, 0xa8, 0x2a, 0x3e, 0x00, 0xff, 0xff, 0x3e, 0xd5, 0x54, 0x55, 0x3f}, 0, 1},
        {"glu-scale-convert-byte-to-ubyte", GL_LUMINANCE, 2, 1, GL_BYTE, {0x80, 0x7f}, 2, 1, GL_UNSIGNED_BYTE,
         {0xfe, 0xff}, 0, 1},
        {"glu-scale-convert-byte-to-short", GL_LUMINANCE, 2, 1, GL_BYTE, {0x80, 0x7f}, 2, 1, GL_SHORT,
         {0x00, 0x7f, 0xfe, 0x7f}, 0, 1},
        {"glu-scale-convert-byte-to-int", GL_LUMINANCE, 2, 1, GL_BYTE, {0x80, 0x7f}, 2, 1, GL_INT,
         {0x00, 0x7f, 0x00, 0x7f, 0xfe, 0x7f, 0xfe, 0x7f}, 0, 1},
        {"glu-scale-convert-byte-to-float", GL_LUMINANCE, 2, 1, GL_BYTE, {0x80, 0x7f}, 2, 1, GL_FLOAT,
         {0xfe, 0x00, 0x7e, 0x3f, 0x00, 0xfd, 0x7f, 0x3f}, 0, 1},
        {"glu-scale-convert-short-to-uint", GL_LUMINANCE, 2, 1, GL_SHORT, le16({0x8000, 0x7fff}), 2, 1,
         GL_UNSIGNED_INT, {0, 0, 0, 0, 0xfe, 0xff, 0xfe, 0xff}, 0, 1},
        {"glu-scale-convert-float-to-int", GL_LUMINANCE, 2, 1, GL_FLOAT, lef({0.0f, 1.0f}), 2, 1, GL_INT,
         {0, 0, 0, 0, 0xff, 0xff, 0xff, 0x7f}, 0, 1},
        {"glu-scale-ubyte-same-size-1-128-254", GL_LUMINANCE, 3, 1, GL_UNSIGNED_BYTE, {1, 128, 254}, 3, 1,
         GL_UNSIGNED_BYTE, {0x01, 0x80, 0xfe}, 0, 1},
        {"glu-scale-byte-same-size-m1-1-m100", GL_LUMINANCE, 3, 1, GL_BYTE, {0xff, 0x01, 0x9c}, 3, 1, GL_BYTE,
         {0x7e, 0x01, 0x1b}, 0, 1},
        {"glu-scale-float-same-size-m0.5-0.3-1.5", GL_LUMINANCE, 3, 1, GL_FLOAT, lef({-0.5f, 0.3f, 1.5f}), 3, 1,
         GL_FLOAT, {0x80, 0x01, 0x00, 0x3f, 0x9a, 0x98, 0x99, 0x3e, 0x00, 0xfd, 0xff, 0x3e}, 0, 1},
        {"glu-scale-int-same-size-m5-123456789-m123456789", GL_LUMINANCE, 3, 1, GL_INT,
         le32({static_cast<u32>(-5), 123456789u, static_cast<u32>(-123456789)}), 3, 1, GL_INT,
         {0xff, 0xff, 0xff, 0x7f, 0x5b, 0x87, 0x5b, 0x07, 0xa4, 0x78, 0xa4, 0x78}, 0, 1},
        // Colour indices keep raw values; GL_BITMAP bits (alignment 1).
        {"glu-scale-colorindex-ubyte-2x1-to-3x1", GL_COLOR_INDEX, 2, 1, GL_UNSIGNED_BYTE, {0, 7}, 3, 1,
         GL_UNSIGNED_BYTE, {0x01, 0x03, 0x05, 0xcc}, 0, 1},
        {"glu-scale-bitmap-8x2-to-4x2", GL_COLOR_INDEX, 8, 2, GL_BITMAP, {0xA5, 0x00}, 4, 2, GL_BITMAP,
         {0x0c, 0x0c, 0xcc}, 0, 1},
        {"glu-scale-bitmap-8x2-to-ubyte-8x2", GL_COLOR_INDEX, 8, 2, GL_BITMAP, {0xA5, 0x00}, 8, 2, GL_UNSIGNED_BYTE,
         {1, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 1},
        {"glu-scale-bitmap-8x2-to-bitmap-8x2", GL_COLOR_INDEX, 8, 2, GL_BITMAP, {0xA5, 0x00}, 8, 2, GL_BITMAP,
         {0xa5, 0x00, 0xcc}, 0, 1},
        {"glu-scale-bitmap-8x1-to-bitmap-12x1", GL_COLOR_INDEX, 8, 1, GL_BITMAP, {0xA5}, 12, 1, GL_BITMAP,
         {0x80, 0x1c, 0xcc}, 0, 1},
        {"glu-scale-colorindex-ubyte-1-2-3-to-bitmap", GL_COLOR_INDEX, 3, 1, GL_UNSIGNED_BYTE, {1, 2, 3}, 3, 1,
         GL_BITMAP, {0xac, 0xcc}, 0, 1},
        {"glu-scale-rgb-bitmap", GL_RGB, 8, 1, GL_BITMAP, {0xA5, 0x00, 0x00}, 8, 1, GL_UNSIGNED_BYTE,
         {0xff, 0x00, 0xff, 0x00}, 0, 1},
        {"glu-scale-rgb-bitmap-to-bitmap", GL_RGB, 8, 1, GL_BITMAP, {0xA5, 0x00, 0x00}, 8, 1, GL_BITMAP,
         {0xa5, 0x00, 0x00, 0xcc}, 0, 1},
        {"glu-scale-lum-ubyte-0-127-128-255-to-bitmap", GL_LUMINANCE, 4, 1, GL_UNSIGNED_BYTE, {0, 127, 128, 255}, 4,
         1, GL_BITMAP, {0x3c, 0xcc}, 0, 1},
        // The exact 2:1 shrink rounds (+2), the box filter truncates.
        {"glu-scale-ushort-2x2-0-0-1-2-to-1x1", GL_LUMINANCE, 2, 2, GL_UNSIGNED_SHORT, le16({0, 0, 1, 2}), 1, 1,
         GL_UNSIGNED_SHORT, {0x01, 0x00, 0xcc}, 0, 1},
        {"glu-scale-ushort-4x2-to-2x1", GL_LUMINANCE, 4, 2, GL_UNSIGNED_SHORT, le16({0, 0, 1, 2, 5, 6, 7, 9}), 2, 1,
         GL_UNSIGNED_SHORT, {0x03, 0x00, 0x05, 0x00}, 0, 1},
        {"glu-scale-ushort-3x1-0-1-2-to-1x1", GL_LUMINANCE, 3, 1, GL_UNSIGNED_SHORT, le16({0, 1, 2}), 1, 1,
         GL_UNSIGNED_SHORT, {0x01, 0x00, 0xcc}, 0, 1},
        // Formats (glu-scale-format-*) and errors.
        {"glu-scale-format-LUMINANCE_ALPHA-1x1-to-2x1", GL_LUMINANCE_ALPHA, 1, 1, GL_UNSIGNED_BYTE, {1, 2}, 2, 1,
         GL_UNSIGNED_BYTE, {1, 2, 1, 2, 0xcc}, 0, 1},
        {"glu-scale-format-BGR_EXT-1x1-to-2x1", 0x80E0, 1, 1, GL_UNSIGNED_BYTE, {1, 2, 3}, 2, 1, GL_UNSIGNED_BYTE,
         {1, 2, 3, 1, 2, 3, 0xcc}, 0, 1},
        {"glu-scale-format-DEPTH_COMPONENT-1x1-to-2x1", GL_DEPTH_COMPONENT, 1, 1, GL_UNSIGNED_BYTE, {1}, 2, 1,
         GL_UNSIGNED_BYTE, {1, 1, 0xcc}, 0, 1},
        {"glu-scale-format-0x1234-1x1-to-2x1", 0x1234, 1, 1, GL_UNSIGNED_BYTE, {1}, 2, 1, GL_UNSIGNED_BYTE,
         {0xcc, 0xcc}, GLU_INVALID_ENUM, 1},
        {"glu-scale-format-RGBA8-1x1-to-2x1", GL_RGBA8, 1, 1, GL_UNSIGNED_BYTE, {1}, 2, 1, GL_UNSIGNED_BYTE,
         {0xcc, 0xcc}, GLU_INVALID_ENUM, 1},
        {"glu-scale-typein-DOUBLE", GL_RGB, 1, 1, GL_DOUBLE, {1}, 1, 1, GL_UNSIGNED_BYTE, {0xcc}, GLU_INVALID_ENUM, 1},
        {"glu-scale-typeout-0x1407-2_BYTES", GL_RGB, 1, 1, GL_UNSIGNED_BYTE, {1}, 1, 1, 0x1407, {0xcc},
         GLU_INVALID_ENUM, 1},
        {"glu-scale-size-win0", GL_RGB, 0, 1, GL_UNSIGNED_BYTE, {1}, 1, 1, GL_UNSIGNED_BYTE, {0xcc}, 0, 1},
        {"glu-scale-size-hout0", GL_RGB, 1, 1, GL_UNSIGNED_BYTE, {1}, 1, 0, GL_UNSIGNED_BYTE, {0xcc}, 0, 1},
        {"glu-scale-size-win-neg", GL_RGB, -1, 1, GL_UNSIGNED_BYTE, {1}, 1, 1, GL_UNSIGNED_BYTE, {0xcc},
         GLU_INVALID_VALUE, 1},
        {"glu-scale-size-hout-neg", GL_RGB, 1, 1, GL_UNSIGNED_BYTE, {1}, 1, -1, GL_UNSIGNED_BYTE, {0xcc},
         GLU_INVALID_VALUE, 1},
        {"glu-scale-size-win0-wout-neg", GL_RGB, 0, 1, GL_UNSIGNED_BYTE, {1}, -1, 1, GL_UNSIGNED_BYTE, {0xcc}, 0, 1},
        {"glu-scale-badformat-and-negative-width", 0x1234, -1, 1, GL_UNSIGNED_BYTE, {1}, 1, 1, GL_UNSIGNED_BYTE,
         {0xcc}, GLU_INVALID_VALUE, 1},
        {"glu-scale-badtypes", GL_RGB, 1, 1, 0x1234, {1}, 1, 1, 0x1234, {0xcc}, GLU_INVALID_ENUM, 1},
    };
    for (const ScaleCase& c : cases) {
        h.call("glPixelStorei", {I{GL_UNPACK_ALIGNMENT}, I{static_cast<u32>(c.align)}});
        h.call("glPixelStorei", {I{GL_PACK_ALIGNMENT}, I{static_cast<u32>(c.align)}});
        h.put(Harness::DATA, c.in);
        h.put(Harness::DST, std::vector<u8>(c.out.size() + 16, 0xCC));
        const u64 r = h.call("gluScaleImage", {I{c.format}, I{static_cast<u32>(c.wi)}, I{static_cast<u32>(c.hi)},
                                               I{c.type_in}, P{Harness::DATA}, I{static_cast<u32>(c.wo)},
                                               I{static_cast<u32>(c.ho)}, I{c.type_out}, P{Harness::DST}});
        EXPECT_EQ(r, c.ret) << c.tag;
        EXPECT_EQ(h.bytes(Harness::DST, c.out.size()), c.out) << c.tag;
        EXPECT_EQ(h.err(), GL_NO_ERROR) << c.tag;
    }
    // The remaining glu-scale-* lines ran under UNPACK/PACK_ALIGNMENT 1.
    h.call("glPixelStorei", {I{GL_UNPACK_ALIGNMENT}, I{1}});
    h.call("glPixelStorei", {I{GL_PACK_ALIGNMENT}, I{1}});

    // Pixel-store modes (glu-scale-lum-ubyte-rowlen4-skip1-1-to-3x2-packrow5-skip1-2).
    h.put(Harness::DATA, lum4x4);
    h.put(Harness::DST, std::vector<u8>(32, 0xCC));
    h.call("glPixelStorei", {I{GL_UNPACK_ROW_LENGTH}, I{4}});
    h.call("glPixelStorei", {I{GL_UNPACK_SKIP_ROWS}, I{1}});
    h.call("glPixelStorei", {I{GL_UNPACK_SKIP_PIXELS}, I{1}});
    h.call("glPixelStorei", {I{GL_PACK_ROW_LENGTH}, I{5}});
    h.call("glPixelStorei", {I{GL_PACK_SKIP_ROWS}, I{1}});
    h.call("glPixelStorei", {I{GL_PACK_SKIP_PIXELS}, I{2}});
    EXPECT_EQ(h.call("gluScaleImage", {I{GL_LUMINANCE}, I{2}, I{2}, I{GL_UNSIGNED_BYTE}, P{Harness::DATA}, I{3},
                                       I{2}, I{GL_UNSIGNED_BYTE}, P{Harness::DST}}),
              0u);
    EXPECT_EQ(h.bytes(Harness::DST, 20),
              (std::vector<u8>{0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x52, 0x58, 0x5d, 0xcc, 0xcc, 0x93, 0x98,
                               0x9d, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc}));
    for (u32 pname : {GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS, GL_PACK_ROW_LENGTH,
                      GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS}) {
        h.call("glPixelStorei", {I{pname}, I{0}});
    }
    // Byte swapping (glu-scale-ushort-unpack-swap-2x1-same / -pack-swap-2x1-same).
    h.put(Harness::DATA, le16({0x1234, 0xABCD}));
    h.call("glPixelStorei", {I{GL_UNPACK_SWAP_BYTES}, I{1}});
    h.call("gluScaleImage", {I{GL_LUMINANCE}, I{2}, I{1}, I{GL_UNSIGNED_SHORT}, P{Harness::DATA}, I{2}, I{1},
                             I{GL_UNSIGNED_SHORT}, P{Harness::DST}});
    EXPECT_EQ(h.bytes(Harness::DST, 4), (std::vector<u8>{0x12, 0x34, 0xab, 0xcd}));
    h.call("glPixelStorei", {I{GL_UNPACK_SWAP_BYTES}, I{0}});
    h.call("glPixelStorei", {I{GL_PACK_SWAP_BYTES}, I{1}});
    h.call("gluScaleImage", {I{GL_LUMINANCE}, I{2}, I{1}, I{GL_UNSIGNED_SHORT}, P{Harness::DATA}, I{2}, I{1},
                             I{GL_UNSIGNED_SHORT}, P{Harness::DST}});
    EXPECT_EQ(h.bytes(Harness::DST, 4), (std::vector<u8>{0x12, 0x34, 0xab, 0xcd}));
    h.call("glPixelStorei", {I{GL_PACK_SWAP_BYTES}, I{0}});
    // Bit offsets (glu-scale-bitmap-unpack-skip3-pack-skip2).
    h.call("glPixelStorei", {I{GL_UNPACK_SKIP_PIXELS}, I{3}});
    h.call("glPixelStorei", {I{GL_PACK_SKIP_PIXELS}, I{2}});
    h.put(Harness::DATA, {0xA5});
    h.put(Harness::DST, {0xCC, 0xCC});
    h.call("gluScaleImage", {I{GL_COLOR_INDEX}, I{5}, I{1}, I{GL_BITMAP}, P{Harness::DATA}, I{5}, I{1}, I{GL_BITMAP},
                             P{Harness::DST}});
    EXPECT_EQ(h.bytes(Harness::DST, 2), (std::vector<u8>{0xca, 0xcc}));
    h.call("glPixelStorei", {I{GL_UNPACK_SKIP_PIXELS}, I{0}});
    h.call("glPixelStorei", {I{GL_PACK_SKIP_PIXELS}, I{0}});

    // Inside glBegin or with no context the pixel-store queries fail: 1285
    // (GL_OUT_OF_MEMORY), nothing written (glu-scale-inside-begin,
    // glu-noctx-scaleimage).
    h.put(Harness::DST, {0xCC, 0xCC, 0xCC, 0xCC});
    h.call("glBegin", {I{GL_POINTS}});
    EXPECT_EQ(h.call("gluScaleImage", {I{GL_LUMINANCE}, I{2}, I{1}, I{GL_UNSIGNED_BYTE}, P{Harness::DATA}, I{4},
                                       I{1}, I{GL_UNSIGNED_BYTE}, P{Harness::DST}}),
              0x0505u);
    h.call("glEnd", {});
    EXPECT_EQ(h.err(), GL_INVALID_OPERATION);
    set_current_gl_context_for_thread(nullptr);
    EXPECT_EQ(h.call("gluScaleImage", {I{GL_LUMINANCE}, I{2}, I{2}, I{GL_UNSIGNED_BYTE}, P{Harness::DATA}, I{4},
                                       I{1}, I{GL_UNSIGNED_BYTE}, P{Harness::DST}}),
              0x0505u);
    EXPECT_EQ(h.bytes(Harness::DST, 4), (std::vector<u8>{0xcc, 0xcc, 0xcc, 0xcc}));
}

// ---- Build2DMipmapsUploadsEveryLevel -----------------------------------------------------

std::vector<u8> hex_bytes(std::string_view s) {
    std::vector<u8> out;
    std::istringstream in{std::string(s)};
    std::string tok;
    while (in >> tok) out.push_back(static_cast<u8>(std::stoul(tok, nullptr, 16)));
    return out;
}

TEST_P(Glu32Test, Build2DMipmapsUploadsEveryLevel) {
    // The probe's source: img[i] = i * 37 + 11, under UNPACK_ALIGNMENT 1.
    std::vector<u8> img(4096 * 4);
    for (usize i = 0; i < img.size(); ++i) img[i] = static_cast<u8>(i * 37 + 11);
    h.put(Harness::DATA, img);
    struct Level {
        i32 w, h;
        const char* bytes;   // glGetTexImage(GL_RGBA, GL_UNSIGNED_BYTE), first 64 bytes
    };
    struct Case {
        const char* tag;
        i32 w, h, comps;
        u32 format;
        std::vector<Level> levels;
    };
    const std::vector<Case> cases = {
        {"glu-build2d-rgb-3x5", 3, 5, 3, GL_RGB,
         {{4, 4, "30 3b 60 ff 60 85 8a ff a0 78 7d ff b0 22 47 ff 73 98 bd ff 7c a1 66 ff 6f 94 59 ff 59 7e a3 ff "
                 "cf 6e 7f ff 79 64 89 ff 6c 91 7c ff b5 c7 66 ff 5f 57 75 ff 88 9a bf ff 7b a0 b2 ff 45 64 5c ff"},
          {2, 2, "60 7e 83 ff 86 6b 70 ff 8c 71 8f ff 78 97 7c ff"},
          {1, 1, "7a 7c 80 ff"}}},
        {"glu-build2d-lum-6x3", 6, 3, 1, GL_LUMINANCE,
         {{8, 4, "36 00 00 ff 3a 00 00 ff 49 00 00 ff 51 00 00 ff 6d 00 00 ff 89 00 00 ff a4 00 00 ff a8 00 00 ff "
                 "99 00 00 ff 49 00 00 ff 29 00 00 ff 44 00 00 ff 60 00 00 ff 7c 00 00 ff 98 00 00 ff ac 00 00 ff"},
          {4, 2, "54 00 00 ff 42 00 00 ff 74 00 00 ff a4 00 00 ff b5 00 00 ff 49 00 00 ff 4e 00 00 ff 87 00 00 ff"},
          {2, 1, "65 00 00 ff 7b 00 00 ff"},
          {1, 1, "70 00 00 ff"}}},
        {"glu-build2d-rgb-12x10", 12, 10, 3, GL_RGB,
         {{16, 8, "28 4d 58 ff 5b 80 a5 ff 9b 73 98 ff b4 26 4b ff 6e 7a 9f ff a8 ad 52 ff 9b a0 45 ff 4e 73 92 ff "
                  "a2 c7 bf ff 55 7a 8c ff 48 6d 7f ff 95 a0 98 ff c2 34 52 ff 8f 67 8c ff 82 a7 7f ff 95 ba 2c ff"},
          {8, 4, "5a 6f 8e ff 87 5c 7e ff 94 92 77 ff 81 86 84 ff 7a 9f 84 ff 87 86 6b ff 87 69 78 ff 6b 90 65 ff "
                 "90 65 8a ff 7d 8c 6e ff 8a 6f 6e ff 71 96 7b ff 71 76 94 ff 7e 63 81 ff 97 99 6e ff 87 86 9b ff"},
          {4, 2, "7c 6f 81 ff 84 87 79 ff 7c 7f 81 ff 84 86 79 ff 80 73 85 ff 88 8a 7d ff 80 73 85 ff 78 8a 7d ff"},
          {2, 1, "82 7d 7f ff 7e 81 7f ff"},
          {1, 1, "80 7f 7f ff"}}},
        // GL_EXT_bgra: blue first in the client image.
        {"glu-build2d-bgra-7x1", 7, 1, 4, 0x80E1,
         {{8, 1, "5c 37 12 81 cd a8 83 22 9f 7a 55 74 40 ab 86 65 52 bd 98 77 83 5e 89 a8 55 30 db 7a c5 a0 7b ea"},
          {4, 1, "95 70 4b 52 6f 92 6d 6c 6a 8d 90 8f 8d 68 ab b2"},
          {2, 1, "82 81 5c 5f 7c 7b 9e a1"},
          {1, 1, "7f 7e 7d 80"}}},
        {"glu-build2d-rgba-1x1", 1, 1, 4, GL_RGBA, {{1, 1, "0b 30 55 7a"}}},
    };
    u32 name = 1;
    for (const Case& c : cases) {
        h.call("glBindTexture", {I{GL_TEXTURE_2D}, I{name++}});
        h.call("glPixelStorei", {I{GL_UNPACK_ALIGNMENT}, I{1}});
        EXPECT_EQ(h.call("gluBuild2DMipmaps", {I{GL_TEXTURE_2D}, I{static_cast<u32>(c.comps)},
                                               I{static_cast<u32>(c.w)}, I{static_cast<u32>(c.h)}, I{c.format},
                                               I{GL_UNSIGNED_BYTE}, P{Harness::DATA}}),
                  0u)
            << c.tag;
        EXPECT_EQ(h.err(), GL_NO_ERROR) << c.tag;
        EXPECT_EQ(h.geti(GL_UNPACK_ALIGNMENT), 1) << c.tag;   // unpack-after=1 0 0 0 0
        h.call("glPixelStorei", {I{GL_PACK_ALIGNMENT}, I{1}});
        for (usize L = 0; L < c.levels.size(); ++L) {
            const Level& lv = c.levels[L];
            const i32 level = static_cast<i32>(L);
            EXPECT_EQ(h.tex_level(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH), lv.w) << c.tag << " level " << L;
            EXPECT_EQ(h.tex_level(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT), lv.h) << c.tag << " level " << L;
            EXPECT_EQ(h.tex_level(GL_TEXTURE_2D, level, GL_TEXTURE_INTERNAL_FORMAT), c.comps) << c.tag << " level " << L;
            const auto want = hex_bytes(lv.bytes);
            h.call("glGetTexImage", {I{GL_TEXTURE_2D}, I{static_cast<u32>(level)}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE},
                                     P{Harness::DST}});
            EXPECT_EQ(h.bytes(Harness::DST, want.size()), want) << c.tag << " level " << L;
        }
        // The level after the last is empty (glu-build2d-* levelN none w=0).
        EXPECT_EQ(h.tex_level(GL_TEXTURE_2D, static_cast<i32>(c.levels.size()), GL_TEXTURE_WIDTH), 0) << c.tag;
        h.call("glPixelStorei", {I{GL_PACK_ALIGNMENT}, I{4}});
    }


    // glu-build2d-rgb-2000x1: 2048 is clamped to GL_MAX_TEXTURE_SIZE, then 11 levels.
    h.call("glBindTexture", {I{GL_TEXTURE_2D}, I{name++}});
    EXPECT_EQ(h.call("gluBuild2DMipmaps", {I{GL_TEXTURE_2D}, I{3}, I{2000}, I{1}, I{GL_RGB}, I{GL_UNSIGNED_BYTE},
                                           P{Harness::DATA}}),
              0u);
    EXPECT_EQ(h.tex_level(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH), 1024);
    EXPECT_EQ(h.tex_level(GL_TEXTURE_2D, 10, GL_TEXTURE_WIDTH), 1);

    // glu-build2d-unpack-align8-row6-skip1-2: the source is read under the
    // client's UNPACK_* modes, which are unchanged afterwards.
    {
        std::vector<u8> src(64);
        for (usize i = 0; i < src.size(); ++i) src[i] = static_cast<u8>(i * 4);
        h.put(Harness::DATA + 0x8000, src);
        h.call("glBindTexture", {I{GL_TEXTURE_2D}, I{name++}});
        h.call("glPixelStorei", {I{GL_UNPACK_ALIGNMENT}, I{8}});
        h.call("glPixelStorei", {I{GL_UNPACK_ROW_LENGTH}, I{6}});
        h.call("glPixelStorei", {I{GL_UNPACK_SKIP_ROWS}, I{1}});
        h.call("glPixelStorei", {I{GL_UNPACK_SKIP_PIXELS}, I{2}});
        EXPECT_EQ(h.call("gluBuild2DMipmaps", {I{GL_TEXTURE_2D}, I{1}, I{2}, I{2}, I{GL_LUMINANCE},
                                               I{GL_UNSIGNED_BYTE}, P{Harness::DATA + 0x8000}}),
                  0u);
        EXPECT_EQ(h.geti(GL_UNPACK_ALIGNMENT), 8);
        EXPECT_EQ(h.geti(GL_UNPACK_ROW_LENGTH), 6);
        EXPECT_EQ(h.geti(GL_UNPACK_SKIP_ROWS), 1);
        EXPECT_EQ(h.geti(GL_UNPACK_SKIP_PIXELS), 2);
        h.call("glPixelStorei", {I{GL_PACK_ALIGNMENT}, I{1}});
        h.call("glGetTexImage", {I{GL_TEXTURE_2D}, I{0}, I{GL_LUMINANCE}, I{GL_UNSIGNED_BYTE}, P{Harness::DST}});
        EXPECT_EQ(h.bytes(Harness::DST, 4), (std::vector<u8>{0x28, 0x2c, 0x48, 0x4c}));
        h.call("glPixelStorei", {I{GL_PACK_ALIGNMENT}, I{4}});
        h.call("glPixelStorei", {I{GL_UNPACK_ALIGNMENT}, I{4}});
        for (u32 p : {GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS}) {
            h.call("glPixelStorei", {I{p}, I{0}});
        }
    }

    // Errors (glu-build2d-err-*): GLU's checks, then glTexImage's.
    struct Err {
        const char* tag;
        u32 target;
        u32 comps;
        i32 w, hgt;
        u32 format, type;
        u32 ret;
        u32 gl_error;
        i32 level0_width;
    };
    const Err errs[] = {
        {"width0", GL_TEXTURE_2D, 3, 0, 4, GL_RGB, GL_UNSIGNED_BYTE, GLU_INVALID_VALUE, GL_NO_ERROR, 0},
        {"height-neg", GL_TEXTURE_2D, 3, 4, -1, GL_RGB, GL_UNSIGNED_BYTE, GLU_INVALID_VALUE, GL_NO_ERROR, 0},
        {"format-bad", GL_TEXTURE_2D, 3, 4, 4, 0x1234, GL_UNSIGNED_BYTE, GLU_INVALID_ENUM, GL_NO_ERROR, 0},
        {"type-bad", GL_TEXTURE_2D, 3, 4, 4, GL_RGB, 0x1234, GLU_INVALID_ENUM, GL_NO_ERROR, 0},
        {"width0-format-bad", GL_TEXTURE_2D, 3, 0, 4, 0x1234, GL_UNSIGNED_BYTE, GLU_INVALID_VALUE, GL_NO_ERROR, 0},
        {"stencil", GL_TEXTURE_2D, 1, 4, 4, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, GLU_INVALID_ENUM, GL_NO_ERROR, 0},
        {"depth", GL_TEXTURE_2D, 1, 4, 4, GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, GLU_INVALID_ENUM, GL_NO_ERROR, 0},
        {"target-1d", GL_TEXTURE_1D, 3, 4, 4, GL_RGB, GL_UNSIGNED_BYTE, 0, GL_INVALID_ENUM, 0},
        {"comps5", GL_TEXTURE_2D, 5, 4, 4, GL_RGB, GL_UNSIGNED_BYTE, 0, GL_INVALID_ENUM, 0},
        {"comps0", GL_TEXTURE_2D, 0, 4, 4, GL_RGB, GL_UNSIGNED_BYTE, 0, GL_INVALID_ENUM, 0},
        {"float", GL_TEXTURE_2D, 3, 4, 4, GL_RGB, GL_FLOAT, 0, GL_NO_ERROR, 4},
        {"byte", GL_TEXTURE_2D, 3, 4, 4, GL_RGB, GL_BYTE, 0, GL_NO_ERROR, 4},
    };
    for (const Err& e : errs) {
        h.call("glBindTexture", {I{GL_TEXTURE_2D}, I{name++}});
        EXPECT_EQ(h.call("gluBuild2DMipmaps", {I{e.target}, I{e.comps}, I{static_cast<u32>(e.w)},
                                               I{static_cast<u32>(e.hgt)}, I{e.format}, I{e.type}, P{Harness::DATA}}),
                  e.ret)
            << e.tag;
        EXPECT_EQ(h.err(), e.gl_error) << e.tag;
        EXPECT_EQ(h.tex_level(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH), e.level0_width) << e.tag;
        while (h.err() != GL_NO_ERROR) {}
    }
    // glu-build2d-inside-begin
    h.call("glBegin", {I{GL_POINTS}});
    EXPECT_EQ(h.call("gluBuild2DMipmaps", {I{GL_TEXTURE_2D}, I{3}, I{4}, I{4}, I{GL_RGB}, I{GL_UNSIGNED_BYTE},
                                           P{Harness::DATA}}),
              0x0505u);
    h.call("glEnd", {});
    EXPECT_EQ(h.err(), GL_INVALID_OPERATION);

    // glu-build2d-compiled: every upload is compiled into the open list.
    h.call("glBindTexture", {I{GL_TEXTURE_2D}, I{name++}});
    h.call("glNewList", {I{301}, I{GL_COMPILE}});
    EXPECT_EQ(h.call("gluBuild2DMipmaps", {I{GL_TEXTURE_2D}, I{3}, I{4}, I{2}, I{GL_RGB}, I{GL_UNSIGNED_BYTE},
                                           P{Harness::DATA}}),
              0u);
    h.call("glEndList", {});
    EXPECT_EQ(h.tex_level(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH), 0);
    h.call("glCallList", {I{301}});
    EXPECT_EQ(h.tex_level(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH), 4);
    EXPECT_EQ(h.tex_level(GL_TEXTURE_2D, 2, GL_TEXTURE_WIDTH), 1);
    EXPECT_EQ(h.err(), GL_NO_ERROR);

    // gluBuild1DMipmaps (glu-build1d-*).
    h.call("glBindTexture", {I{GL_TEXTURE_1D}, I{name++}});
    h.call("glPixelStorei", {I{GL_UNPACK_ALIGNMENT}, I{1}});
    h.call("glPixelStorei", {I{GL_PACK_ALIGNMENT}, I{1}});
    EXPECT_EQ(h.call("gluBuild1DMipmaps", {I{GL_TEXTURE_1D}, I{3}, I{5}, I{GL_RGB}, I{GL_UNSIGNED_BYTE},
                                           P{Harness::DATA}}),
              0u);
    const std::vector<std::pair<i32, const char*>> levels1d = {
        {4, "21 46 6b ff a6 65 8a ff 92 51 76 ff b1 d6 2e ff"}, {2, "64 56 7b ff a1 93 52 ff"}, {1, "83 74 66 ff"}};
    for (usize L = 0; L < levels1d.size(); ++L) {
        EXPECT_EQ(h.tex_level(GL_TEXTURE_1D, static_cast<i32>(L), GL_TEXTURE_WIDTH), levels1d[L].first);
        const auto want = hex_bytes(levels1d[L].second);
        h.call("glGetTexImage", {I{GL_TEXTURE_1D}, I{static_cast<u32>(L)}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE},
                                 P{Harness::DST}});
        EXPECT_EQ(h.bytes(Harness::DST, want.size()), want) << "glu-build1d-rgb-5 level " << L;
    }
    EXPECT_EQ(h.call("gluBuild1DMipmaps", {I{GL_TEXTURE_1D}, I{3}, I{0}, I{GL_RGB}, I{GL_UNSIGNED_BYTE},
                                           P{Harness::DATA}}),
              GLU_INVALID_VALUE);   // glu-build1d-err-width0
    EXPECT_EQ(h.call("gluBuild1DMipmaps", {I{GL_TEXTURE_1D}, I{1}, I{4}, I{GL_STENCIL_INDEX}, I{GL_UNSIGNED_BYTE},
                                           P{Harness::DATA}}),
              GLU_INVALID_ENUM);    // glu-build1d-err-stencil
    // No context (glu-noctx-build2d, -build1d, -build2d-width0).
    set_current_gl_context_for_thread(nullptr);
    EXPECT_EQ(h.call("gluBuild2DMipmaps", {I{GL_TEXTURE_2D}, I{1}, I{2}, I{2}, I{GL_LUMINANCE}, I{GL_UNSIGNED_BYTE},
                                           P{Harness::DATA}}),
              0x0505u);
    EXPECT_EQ(h.call("gluBuild1DMipmaps", {I{GL_TEXTURE_1D}, I{1}, I{2}, I{GL_LUMINANCE}, I{GL_UNSIGNED_BYTE},
                                           P{Harness::DATA}}),
              0x0505u);
    EXPECT_EQ(h.call("gluBuild2DMipmaps", {I{GL_TEXTURE_2D}, I{1}, I{0}, I{2}, I{GL_LUMINANCE}, I{GL_UNSIGNED_BYTE},
                                           P{Harness::DATA}}),
              GLU_INVALID_VALUE);
}

// ---- Quadrics: errors, the callback and current values -----------------------------------

TEST_P(Glu32Test, QuadricErrorsGoToTheCallback) {
    const GuestAddr q = static_cast<GuestAddr>(h.call("gluNewQuadric", {}));
    ASSERT_NE(q, 0u);   // glu-newquadric non-null=1
    // No callback: silent, no GL error (glu-quadric-err-no-callback).
    h.call("gluQuadricDrawStyle", {P{q}, I{0x1234}});
    EXPECT_TRUE(h.callbacks.empty());
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    const GuestAddr cb = 0x401000;
    h.call("gluQuadricCallback", {P{q}, I{glu::GLU_ERROR}, P{cb}});
    struct Case {
        const char* tag;
        std::string_view api;
        std::vector<Arg> args;
        u32 code;   // 0: no callback
    };
    const std::vector<Case> cases = {
        {"drawstyle-bad", "gluQuadricDrawStyle", {P{q}, I{0x1234}}, GLU_INVALID_ENUM},
        {"normals-bad", "gluQuadricNormals", {P{q}, I{glu::GLU_LINE}}, GLU_INVALID_ENUM},
        {"orientation-bad", "gluQuadricOrientation", {P{q}, I{glu::GLU_FLAT}}, GLU_INVALID_ENUM},
        {"texture-2", "gluQuadricTexture", {P{q}, I{2}}, 0},
        {"callback-which-bad", "gluQuadricCallback", {P{q}, I{0x1234}, P{cb}}, GLU_INVALID_ENUM},
        {"sphere-radius-neg", "gluSphere", {P{q}, D{-1}, I{8}, I{8}}, GLU_INVALID_VALUE},
        {"sphere-slices1", "gluSphere", {P{q}, D{1}, I{1}, I{8}}, GLU_INVALID_VALUE},
        {"sphere-stacks1", "gluSphere", {P{q}, D{1}, I{8}, I{1}}, 0},
        {"sphere-stacks0", "gluSphere", {P{q}, D{1}, I{8}, I{0}}, GLU_INVALID_VALUE},
        {"sphere-radius0", "gluSphere", {P{q}, D{0}, I{8}, I{8}}, 0},
        {"cylinder-base-neg", "gluCylinder", {P{q}, D{-1}, D{1}, D{1}, I{8}, I{1}}, GLU_INVALID_VALUE},
        {"cylinder-top-neg", "gluCylinder", {P{q}, D{1}, D{-1}, D{1}, I{8}, I{1}}, GLU_INVALID_VALUE},
        {"cylinder-height-neg", "gluCylinder", {P{q}, D{1}, D{1}, D{-1}, I{8}, I{1}}, GLU_INVALID_VALUE},
        {"cylinder-slices1", "gluCylinder", {P{q}, D{1}, D{1}, D{1}, I{1}, I{1}}, GLU_INVALID_VALUE},
        {"cylinder-stacks0", "gluCylinder", {P{q}, D{1}, D{1}, D{1}, I{8}, I{0}}, GLU_INVALID_VALUE},
        {"disk-inner-neg", "gluDisk", {P{q}, D{-1}, D{1}, I{8}, I{1}}, GLU_INVALID_VALUE},
        {"disk-outer-lt-inner", "gluDisk", {P{q}, D{1}, D{0.5}, I{8}, I{1}}, GLU_INVALID_VALUE},
        {"disk-outer-eq-inner", "gluDisk", {P{q}, D{1}, D{1}, I{8}, I{1}}, 0},
        {"disk-slices1", "gluDisk", {P{q}, D{0}, D{1}, I{1}, I{1}}, GLU_INVALID_VALUE},
        {"disk-loops0", "gluDisk", {P{q}, D{0}, D{1}, I{8}, I{0}}, GLU_INVALID_VALUE},
        {"pdisk-sweep-0", "gluPartialDisk", {P{q}, D{0}, D{1}, I{8}, I{1}, D{0}, D{0}}, 0},
        {"pdisk-sweep-720", "gluPartialDisk", {P{q}, D{0}, D{1}, I{8}, I{1}, D{0}, D{720}}, 0},
        {"pdisk-sweep-m720", "gluPartialDisk", {P{q}, D{0}, D{1}, I{8}, I{1}, D{0}, D{-720}}, 0},
        {"pdisk-slices1", "gluPartialDisk", {P{q}, D{0}, D{1}, I{1}, I{1}, D{0}, D{90}}, GLU_INVALID_VALUE},
        {"pdisk-loops0", "gluPartialDisk", {P{q}, D{0}, D{1}, I{8}, I{0}, D{0}, D{90}}, GLU_INVALID_VALUE},
        {"pdisk-inner-neg", "gluPartialDisk", {P{q}, D{-1}, D{1}, I{8}, I{1}, D{0}, D{90}}, GLU_INVALID_VALUE},
    };
    for (const Case& c : cases) {
        h.callbacks.clear();
        h.call(c.api, c.args);
        if (c.code == 0) {
            EXPECT_TRUE(h.callbacks.empty()) << "glu-quadric-err-" << c.tag;
        } else {
            ASSERT_EQ(h.callbacks.size(), 1u) << "glu-quadric-err-" << c.tag;
            EXPECT_EQ(h.callbacks[0].fn, cb) << c.tag;
            EXPECT_EQ(h.callbacks[0].code, c.code) << c.tag;
            EXPECT_EQ(h.callbacks[0].x64, h.x64()) << c.tag;
        }
        EXPECT_EQ(h.err(), GL_NO_ERROR) << c.tag;   // gl-err=0x0000
    }
    // A refused setter keeps the old value (glu-quadric-bad-style-keeps-point).
    h.call("gluQuadricDrawStyle", {P{q}, I{glu::GLU_POINT}});
    h.call("gluQuadricDrawStyle", {P{q}, I{0x1234}});
    EXPECT_EQ(h.glu.quadric_state(q)->draw_style, glu::GLU_POINT);
    EXPECT_TRUE(h.glu.quadric_state(q)->texture);   // texture-2 turned it on
    // A NULL callback clears it (-drawstyle-bad-after-null-callback).
    h.call("gluQuadricCallback", {P{q}, I{glu::GLU_ERROR}, P{0}});
    h.callbacks.clear();
    h.call("gluQuadricDrawStyle", {P{q}, I{0x1234}});
    EXPECT_TRUE(h.callbacks.empty());
    // With no context the callback still runs (glu-noctx-sphere-error).
    h.call("gluQuadricCallback", {P{q}, I{glu::GLU_ERROR}, P{cb}});
    set_current_gl_context_for_thread(nullptr);
    h.call("gluSphere", {P{q}, D{-1}, I{8}, I{8}});
    ASSERT_EQ(h.callbacks.size(), 1u);
    EXPECT_EQ(h.callbacks[0].code, GLU_INVALID_VALUE);
    h.call("gluDeleteQuadric", {P{q}});
    EXPECT_FALSE(h.glu.quadric_state(q).has_value());
}

TEST_P(Glu32Test, QuadricDrawsLeaveCurrentValuesAndCompile) {
    const GuestAddr q = static_cast<GuestAddr>(h.call("gluNewQuadric", {}));
    ASSERT_NE(q, 0u);
    // glu-quadric-sphere-after: a textured smooth sphere leaves its last
    // vertex's normal and texture coordinates current.
    h.call("gluQuadricTexture", {P{q}, I{1}});
    h.call("glNormal3f", {F{9}, F{9}, F{9}});
    h.call("glTexCoord4f", {F{9}, F{9}, F{9}, F{9}});
    h.call("gluSphere", {P{q}, D{0.5}, I{4}, I{3}});
    const auto n = h.getf(GL_CURRENT_NORMAL, 3);
    const auto t = h.getf(GL_CURRENT_TEXTURE_COORDS, 4);
    EXPECT_NEAR(n[0], 0, 1e-6);
    EXPECT_NEAR(n[1], 0.866025, 1e-6);
    EXPECT_NEAR(n[2], -0.5, 1e-6);
    EXPECT_NEAR(t[0], 0, 1e-6);
    EXPECT_NEAR(t[1], 0.333333, 1e-6);
    EXPECT_EQ(t[2], 0.0f);
    EXPECT_EQ(t[3], 1.0f);
    // glu-quadric-sphere-none-notex-after: neither is touched.
    h.call("glNormal3f", {F{9}, F{9}, F{9}});
    h.call("glTexCoord4f", {F{9}, F{9}, F{9}, F{9}});
    h.call("gluQuadricNormals", {P{q}, I{glu::GLU_NONE}});
    h.call("gluQuadricTexture", {P{q}, I{0}});
    h.call("gluSphere", {P{q}, D{0.5}, I{4}, I{3}});
    EXPECT_EQ(h.getf(GL_CURRENT_NORMAL, 3), (std::vector<float>{9, 9, 9}));
    EXPECT_EQ(h.getf(GL_CURRENT_TEXTURE_COORDS, 4), (std::vector<float>{9, 9, 9, 9}));
    // Compiled into a list, a quadric's commands run at glCallList
    // (glu-quadric-disk-compiled): a smooth disk's one glNormal.
    h.call("gluQuadricNormals", {P{q}, I{glu::GLU_SMOOTH}});
    h.call("gluQuadricOrientation", {P{q}, I{glu::GLU_INSIDE}});
    h.call("glNewList", {I{302}, I{GL_COMPILE}});
    h.call("gluDisk", {P{q}, D{0}, D{0.5}, I{5}, I{1}});
    h.call("glEndList", {});
    EXPECT_EQ(h.getf(GL_CURRENT_NORMAL, 3), (std::vector<float>{9, 9, 9}));
    h.call("glCallList", {I{302}});
    EXPECT_EQ(h.getf(GL_CURRENT_NORMAL, 3), (std::vector<float>{0, 0, -1}));
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    h.call("gluDeleteQuadric", {P{q}});
}

// ---- CallbackFunctionsFailLoudly ---------------------------------------------------------

TEST_P(Glu32Test, CallbackFunctionsFailLoudly) {
    // The tessellator and NURBS renderer: NULL constructors, no-op rest, one
    // log line per family.
    glu32_forget_unimplemented_logs_for_tests();
    testing::internal::CaptureStderr();
    EXPECT_EQ(h.call("gluNewTess", {}), 0u);
    h.call("gluTessBeginPolygon", {P{0x1000}, P{0}});
    h.call("gluTessVertex", {P{0x1000}, P{Harness::DATA}, P{0}});
    h.call("gluTessProperty", {P{0x1000}, I{100140}, D{100130}});
    h.call("gluTessNormal", {P{0x1000}, D{0}, D{0}, D{1}});
    h.call("gluBeginPolygon", {P{0x1000}});
    h.call("gluDeleteTess", {P{0x1000}});
    // gluGetTessProperty leaves *value alone.
    h.put(Harness::DATA, {1, 2, 3, 4, 5, 6, 7, 8});
    h.call("gluGetTessProperty", {P{0x1000}, I{100140}, P{Harness::DATA}});
    EXPECT_EQ(h.bytes(Harness::DATA, 8), (std::vector<u8>{1, 2, 3, 4, 5, 6, 7, 8}));
    EXPECT_EQ(h.call("gluNewNurbsRenderer", {}), 0u);
    h.call("gluNurbsProperty", {P{0x1000}, I{100200}, F{1}});
    h.call("gluNurbsCurve", {P{0x1000}, I{4}, P{Harness::DATA}, I{3}, P{Harness::DATA}, I{2}, I{0x0DB7}});
    h.call("gluNurbsSurface", {P{0x1000}, I{4}, P{Harness::DATA}, I{4}, P{Harness::DATA}, I{3}, I{12},
                               P{Harness::DATA}, I{2}, I{2}, I{0x0DB7}});
    h.call("gluGetNurbsProperty", {P{0x1000}, I{100200}, P{Harness::DATA}});
    EXPECT_EQ(h.bytes(Harness::DATA, 8), (std::vector<u8>{1, 2, 3, 4, 5, 6, 7, 8}));
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_EQ(log,
              "[vitreo] UNIMPLEMENTED glu32!gluNewTess: the GLU tessellator is not implemented\n"
              "[vitreo] UNIMPLEMENTED glu32!gluNewNurbsRenderer: the GLU NURBS renderer is not implemented\n");
    EXPECT_EQ(h.err(), GL_NO_ERROR);
}

// ---- Fix round 1: allocation, scratch, bounds, gluUnProject's singularity rule ------------

// gluNewQuadric allocates from the process heap (Learn: NULL only when memory
// runs out), so more than any fixed table holds can be live; gluDeleteQuadric
// frees exactly the live ones.
TEST_P(Glu32Test, QuadricsComeFromTheHeap) {
    std::vector<GuestAddr> live;
    for (int k = 0; k < 600; ++k) {
        const GuestAddr q = static_cast<GuestAddr>(h.call("gluNewQuadric", {}));
        ASSERT_NE(q, 0u) << k;
        live.push_back(q);
    }
    std::sort(live.begin(), live.end());
    EXPECT_EQ(std::adjacent_find(live.begin(), live.end()), live.end());   // all distinct
    h.call("gluDeleteQuadric", {P{live[0]}});
    ASSERT_EQ(h.freed.size(), 1u);
    EXPECT_EQ(h.freed[0], live[0]);
    // A pointer that is not a live quadric is left alone (freed, NULL, garbage).
    h.call("gluDeleteQuadric", {P{live[0]}});
    h.call("gluDeleteQuadric", {P{0}});
    h.call("gluDeleteQuadric", {P{0x12345678}});
    EXPECT_EQ(h.freed.size(), 1u);
    h.call("gluQuadricDrawStyle", {P{0x12345678}, I{glu::GLU_LINE}});
    h.call("gluSphere", {P{0x12345678}, D{1}, I{8}, I{8}});
    EXPECT_EQ(h.err(), GL_NO_ERROR);
    // An exhausted heap: NULL.
    h.heap_next = Harness::HEAP + 0x10000;
    EXPECT_EQ(h.call("gluNewQuadric", {}), 0u);
}

// With no scratch region the string exports return NULL instead of writing
// memory glu32 does not own.
TEST_P(Glu32Test, NoScratchRegionMeansNullStrings) {
    h.glu.set_scratch_base(0);
    EXPECT_EQ(h.call("gluErrorString", {I{0x0500}}), 0u);
    EXPECT_EQ(h.call("gluErrorUnicodeStringEXT", {I{0x0500}}), 0u);
    EXPECT_EQ(h.call("gluGetString", {I{100800}}), 0u);
}

// Size alone never refuses an image: Windows' glu32 mallocs what the image
// needs and reports GLU_OUT_OF_MEMORY only when malloc fails. Vitreo's one
// rule of its own is that a client image must be mapped guest memory before
// anything of its size is allocated; an unmapped one is the GL_OUT_OF_MEMORY
// glu32 returns when it cannot read its pixel-store state (glu-scale-inside-
// begin: 1285), so these huge, unmapped images get that and no host
// allocation of their size is attempted.
TEST_P(Glu32Test, HugeUnmappedImagesAllocateNothing) {
    EXPECT_EQ(h.call("gluScaleImage", {I{GL_RGBA}, I{60000}, I{60000}, I{GL_UNSIGNED_BYTE}, P{Harness::DATA}, I{2},
                                       I{2}, I{GL_UNSIGNED_BYTE}, P{Harness::DST}}),
              0x0505u);
    EXPECT_EQ(h.call("gluScaleImage", {I{GL_RGBA}, I{2}, I{2}, I{GL_UNSIGNED_BYTE}, P{Harness::DATA}, I{60000},
                                       I{60000}, I{GL_UNSIGNED_BYTE}, P{Harness::DST}}),
              0x0505u);
    EXPECT_EQ(h.call("gluBuild2DMipmaps", {I{GL_TEXTURE_2D}, I{4}, I{60000}, I{60000}, I{GL_RGBA},
                                           I{GL_UNSIGNED_BYTE}, P{Harness::DATA}}),
              0x0505u);
}

// Final review, Finding 2: the GLU image layout is checked arithmetic. With
// opengl32's wrapping modes (ALIGNMENT 4, ROW_LENGTH 2^31 - 1, SKIP_ROWS
// 2^30, SKIP_PIXELS 2^30 - 100) the 101 x 1 RGBA FLOAT span wrapped to a few
// bytes and pack_image wrote through data + ~2^64; no client range can hold
// that image, so it is unreadable (GL_OUT_OF_MEMORY) and nothing is read or
// written, on the pack side (gluScaleImage's output) and the unpack side
// (its input, gluBuild2DMipmaps' source).
TEST_P(Glu32Test, WrappingImageLayoutTouchesNoMemory) {
    const auto wrap = [&](bool pack) {
        h.call("glPixelStorei", {I{pack ? GL_PACK_ALIGNMENT : GL_UNPACK_ALIGNMENT}, I{4}});
        h.call("glPixelStorei", {I{pack ? GL_PACK_ROW_LENGTH : GL_UNPACK_ROW_LENGTH}, I{0x7FFFFFFF}});
        h.call("glPixelStorei", {I{pack ? GL_PACK_SKIP_ROWS : GL_UNPACK_SKIP_ROWS}, I{1u << 30}});
        h.call("glPixelStorei", {I{pack ? GL_PACK_SKIP_PIXELS : GL_UNPACK_SKIP_PIXELS}, I{(1u << 30) - 100}});
    };
    const glu::PixelModes modes{false, false, 0x7FFFFFFF, 1 << 30, (1 << 30) - 100, 4};
    EXPECT_EQ(glu::image_span(modes, 101, 1, GL_RGBA, GL_FLOAT), std::nullopt);
    EXPECT_EQ(glu::image_span(modes, 100, 1, GL_RGBA, GL_FLOAT), std::nullopt);
    const glu::PixelModes bits{false, false, 0x7FFFFFFF, 0x7FFFFFFF, 0, 8};
    EXPECT_EQ(glu::image_span(bits, 8, 8, GL_COLOR_INDEX, GL_BITMAP), std::nullopt);
    EXPECT_EQ(glu::image_span(glu::PixelModes{}, 0, 0, GL_RGBA, GL_FLOAT), std::optional<usize>(0));

    std::vector<u8> sentinel(0x1000, 0xAB);
    ASSERT_TRUE(h.memory.write_bytes(Harness::DST, sentinel.data(), sentinel.size()).is_ok());
    wrap(true);
    EXPECT_EQ(h.call("gluScaleImage", {I{GL_RGBA}, I{1}, I{1}, I{GL_FLOAT}, P{Harness::DATA}, I{101}, I{1},
                                       I{GL_FLOAT}, P{Harness::DST}}),
              0x0505u);
    std::vector<u8> back(sentinel.size());
    ASSERT_TRUE(h.memory.read_bytes(Harness::DST, back.data(), back.size()).is_ok());
    EXPECT_EQ(back, sentinel);
    wrap(false);
    EXPECT_EQ(h.call("gluScaleImage", {I{GL_RGBA}, I{101}, I{1}, I{GL_FLOAT}, P{Harness::DATA}, I{1}, I{1},
                                       I{GL_FLOAT}, P{Harness::DST}}),
              0x0505u);
    EXPECT_EQ(h.call("gluBuild2DMipmaps", {I{GL_TEXTURE_2D}, I{4}, I{101}, I{1}, I{GL_RGBA}, I{GL_FLOAT},
                                           P{Harness::DATA}}),
              0x0505u);
    ASSERT_TRUE(h.memory.read_bytes(Harness::DST, back.data(), back.size()).is_ok());
    EXPECT_EQ(back, sentinel);
}

// A client image is read from committed guest memory with the access it
// needs, across adjacent allocations as Windows reads them: the output of
// gluScaleImage must be writable, so a read-only one is unreadable
// (GL_OUT_OF_MEMORY) and stays as it was.
TEST_P(Glu32Test, ReadOnlyOutputImageIsUnwritable) {
    constexpr GuestAddr kReadOnly = 0x300000;
    ASSERT_TRUE(h.memory.map_region(kReadOnly, 0x1000, MemoryProtection::Read).is_ok());
    const std::vector<u8> in{10, 20, 30, 40};
    ASSERT_TRUE(h.memory.write_bytes(Harness::DATA, in.data(), in.size()).is_ok());
    EXPECT_EQ(h.call("gluScaleImage", {I{GL_RGBA}, I{1}, I{1}, I{GL_UNSIGNED_BYTE}, P{Harness::DATA}, I{1},
                                       I{1}, I{GL_UNSIGNED_BYTE}, P{kReadOnly}}),
              0x0505u);
}

// The working images are allocated without aborting: an allocation the host
// cannot satisfy reports failure (gluScaleImage / gluBuild*DMipmaps then
// return GLU_OUT_OF_MEMORY), and a component count past the address space is
// refused before any allocation. Elements are not initialised (each is
// written before it is read).
TEST(Glu32Image, HostAllocationFailureIsReported) {
    using glu::HostBuffer;
    using glu::component_count;
    HostBuffer<u16> huge;
    EXPECT_FALSE(huge.allocate(SIZE_MAX / 4));
    EXPECT_EQ(huge.size(), 0u);
    HostBuffer<u16> small;
    ASSERT_TRUE(small.allocate(6));
    EXPECT_EQ(small.size(), 6u);
    EXPECT_EQ(component_count(3, 2, 4), std::optional<usize>(24));
    EXPECT_EQ(component_count(0x7FFFFFFF, 0x7FFFFFFF, 4), std::nullopt);
}

// gluBuild*DMipmaps' level-0 size (glu-build2d-* level0 lines): the nearest
// power of two, the larger one at a midpoint.
TEST(Glu32Image, NearestPowerOfTwo) {
    using glu::nearest_power;
    EXPECT_EQ(nearest_power(1), 1);
    EXPECT_EQ(nearest_power(3), 4);
    EXPECT_EQ(nearest_power(5), 4);
    EXPECT_EQ(nearest_power(6), 8);
    EXPECT_EQ(nearest_power(10), 8);
    EXPECT_EQ(nearest_power(12), 16);
}

// ext-unproject-*: glu32 inverts by elimination and fails only on an exactly
// zero pivot, so rank-deficient products whose elimination never meets one
// succeed with the coordinates the rounding gives.
TEST_P(Glu32Test, UnProjectSingularityRuleIsGlu32s) {
    struct Case {
        std::array<double, 16> m;
        int ret;
        std::array<double, 3> obj;
    };
    const Case cases[] = {
        {{0.1, 0.7, 0.3, 0, 0.2, 0.11, 0.13, 0, 0.1 + 0.2, 0.7 + 0.11, 0.3 + 0.13, 0, 0, 0, 0, 1}, 1,
         {-14160155107453276, -14160155107453280, 14160155107453276}},
        {{0.1, 0.5, 0.1 * 3, 0, 0.2, 0.9, 0.2 * 3, 0, 0.7, 0.3, 0.7 * 3, 0, 0, 0, 0, 1}, 1,
         {-1.0268207150404734e+17, 57646075230342360, -1801439850948199.8}},
        {{1, 2, 3, 0, 4, 5, 6, 0, 7, 8, 9, 0, 0, 0, 0, 1}, 1,
         {-836382787940234.38, 1672765575880469.2, -836382787940234.88}},
        {{1e-300, 0, 0, 0, 0, 1e-300, 0, 0, 0, 0, 1e-300, 0, 0, 0, 0, 1}, 1,
         {-3.9999999999999995e+299, 1.9999999999999994e+299, -4.9999999999999995e+299}},
        {{1e-170, 0, 0, 0, 0, 1e-170, 0, 0, 0, 0, 1e-170, 0, 0, 0, 0, 1}, 1,
         {-4.0000000000000004e+169, 1.9999999999999995e+169, -5.0000000000000002e+169}},
        {{0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, 1, {0.19999999999999996, -0.40000000000000002, -0.5}},
        {{2, 0, 0, 0, 0, 0, 0, 3, 0, 0, 4, 0, 0, 5, 0, 1}, 1, {-5.0000000000000009, 8.0000000000000018, -3.1250000000000004}},
        {{1, 1, 0, 0, 1, 1 + 1e-15, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, 1, {-540431955284459.94, 540431955284459.56, -0.5}},
        {{0.3, 0.6, 0.9, 0, 0.1, 0.2, 0.3, 0, 0.5, 0.4, 0.3, 0, 0, 0, 0, 1}, 0, {7, 7, 7}},
    };
    const GuestAddr m = Harness::DATA, id = Harness::DATA + 0x80, vp = Harness::DATA + 0x100;
    const GuestAddr ox = Harness::DATA + 0x200, oy = ox + 8, oz = ox + 16;
    h.put_matrix(id, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    h.put(vp, {0, 0, 0, 0, 0, 0, 0, 0, 100, 0, 0, 0, 100, 0, 0, 0});
    auto readd = [&](GuestAddr at) {
        double d = 0;
        EXPECT_TRUE(h.memory.read_bytes(at, reinterpret_cast<Byte*>(&d), 8).is_ok());
        return d;
    };
    int k = 1;
    for (const Case& c : cases) {
        h.put_matrix(m, c.m);
        for (bool as_proj : {false, true}) {   // ext-unproject-mK and ext-unproject-proj-mK agree
            const double seven = 7;
            for (GuestAddr at : {ox, oy, oz}) (void)h.memory.write_bytes(at, reinterpret_cast<const u8*>(&seven), 8);
            const u64 r = h.call("gluUnProject", {D{30}, D{60}, D{0.25}, P{as_proj ? id : m}, P{as_proj ? m : id},
                                                  P{vp}, P{ox}, P{oy}, P{oz}});
            EXPECT_EQ(r, static_cast<u64>(c.ret)) << "m" << k;
            for (int i = 0; i < 3; ++i) {
                const double want = c.obj[static_cast<usize>(i)];
                EXPECT_NEAR(readd(ox + 8 * static_cast<GuestAddr>(i)), want, std::fabs(want) * 1e-9 + 1e-12)
                    << "m" << k << " " << i;
            }
        }
        ++k;
    }
}

// Products of two non-identity matrices are rounded term by term, never
// fused (gl11_state_probe64.expected fix2-*, identical on x86). x = 1 + 2^-30
// and y = 1 + 2^-29: x*x rounds to y, so x*x - y is exactly 0 rounded and
// 2^-60 fused. proj * model below has row 0 (x*x - y, 0, -y*x + x*y, 0):
// rounded, the elimination's first pivot is an exact zero and gluUnProject
// fails; fused, it would invert to ~4.6e17 coordinates. gluProject's w = -y +
// x*x likewise is exactly 0 and the call fails. The regular products are
// compared bit for bit.
TEST_P(Glu32Test, ProjectionProductsAreRoundedNotFused) {
    const double x = 1.0 + 1.0 / 1073741824.0;
    const double y = 1.0 + 1.0 / 536870912.0;
    const GuestAddr a = Harness::DATA, b = Harness::DATA + 0x80, vp = Harness::DATA + 0x100;
    const GuestAddr o = Harness::DATA + 0x200;
    h.put(vp, {0, 0, 0, 0, 0, 0, 0, 0, 100, 0, 0, 0, 100, 0, 0, 0});
    auto reset = [&] {
        const double seven = 7;
        for (int i = 0; i < 3; ++i)
            (void)h.memory.write_bytes(o + 8 * static_cast<GuestAddr>(i), reinterpret_cast<const u8*>(&seven), 8);
    };
    auto out = [&](int i) {
        double d = 0;
        EXPECT_TRUE(h.memory.read_bytes(o + 8 * static_cast<GuestAddr>(i), reinterpret_cast<Byte*>(&d), 8).is_ok());
        return d;
    };
    auto unproject = [&](GuestAddr model, GuestAddr proj) {
        reset();
        return h.call("gluUnProject", {D{30}, D{60}, D{0.25}, P{model}, P{proj}, P{vp}, P{o}, P{o + 8}, P{o + 16}});
    };
    auto project = [&](double ox, double oy, double oz, GuestAddr model, GuestAddr proj) {
        reset();
        return h.call("gluProject", {D{ox}, D{oy}, D{oz}, P{model}, P{proj}, P{vp}, P{o}, P{o + 8}, P{o + 16}});
    };

    const std::array<double, 16> proj = {-y, 0, 0, 0, x, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const std::array<double, 16> model = {1, x, 0, 0, 0, 0, 1, 0, x, y, 0, 0, 0, 0, 0, 1};
    h.put_matrix(a, model);
    h.put_matrix(b, proj);
    EXPECT_EQ(unproject(a, b), 0u);   // fix2-unproject-fused-pivot ret=0 obj=7 7 7
    for (int i = 0; i < 3; ++i) EXPECT_EQ(out(i), 7.0);
    EXPECT_EQ(unproject(b, a), 0u);   // fix2-unproject-fused-pivot-swapped ret=0
    for (int i = 0; i < 3; ++i) EXPECT_EQ(out(i), 7.0);

    const std::array<double, 16> id = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const std::array<double, 16> pw = {1, 0, 0, -y, 0, 1, 0, x, 0, 0, 1, 0, 0, 0, 0, 0};
    h.put_matrix(a, id);
    h.put_matrix(b, pw);
    EXPECT_EQ(project(1, x, 0, a, b), 0u);   // fix2-project-fused-w ret=0 win=7 7 7
    for (int i = 0; i < 3; ++i) EXPECT_EQ(out(i), 7.0);
    EXPECT_EQ(project(1, x, 0, b, a), 0u);   // fix2-project-fused-w-model ret=0
    for (int i = 0; i < 3; ++i) EXPECT_EQ(out(i), 7.0);

    h.put_matrix(a, {0.1, 0.7, 0.3, 0.2, 0.2, 0.11, 0.13, 0.5, 0.9, 0.3, 0.17, 0.4, 0.6, 0.25, 0.35, 1.3});
    h.put_matrix(b, {1.1, 0.3, 0.7, 0.1, 0.9, 1.7, 0.2, 0.3, 0.4, 0.6, 1.9, 0.5, 0.3, 0.8, 0.1, 2.3});
    // fix2-unproject-regular ret=1 obj=0.072036555580440906 -2.7443356925708584 -0.0018021896385825157
    EXPECT_EQ(unproject(a, b), 1u);
    EXPECT_EQ(out(0), 0.072036555580440906);
    EXPECT_EQ(out(1), -2.7443356925708584);
    EXPECT_EQ(out(2), -0.0018021896385825157);
    // fix2-project-regular ret=1 win=80.956442802810955 86.88040943452333 0.75942591520197822
    EXPECT_EQ(project(0.3, -0.7, 0.45, a, b), 1u);
    EXPECT_EQ(out(0), 80.956442802810955);
    EXPECT_EQ(out(1), 86.88040943452333);
    EXPECT_EQ(out(2), 0.75942591520197822);
}

INSTANTIATE_TEST_SUITE_P(Bitness, Glu32Test, ::testing::Values(false, true), bitness_name);

TEST(Glu32Registration, EveryExportHasAHandlerAndTheFamilyLogsOnce) {
    Glu32Manager mgr;
    const DllExports exports = create_glu32_thunks(mgr);
    EXPECT_EQ(exports.entries.size(), 52u);
    for (const auto& e : exports.entries) EXPECT_TRUE(static_cast<bool>(e.handler)) << e.name;
}

// ---- Quadric tessellation against Microsoft glu32 (glu-fb-* feedback) ---------------------

/// The GL calls of one quadric draw, assembled as GL_FEEDBACK reports them:
/// points, line segments (the first of each strip a reset), and triangles
/// (GDI Generic splits quad (v0, v1, v3, v2) of a strip into (v0, v1, v3) and
/// (v3, v2, v0), a fan into (0, i, i + 1); glu-fb-fill-cylinder p0/p1,
/// glu-fb-fill-disk-0-0.5-3-1 p0-p2).
class FeedbackRecorder final : public glu::QuadricSink {
public:
    struct Vertex {
        std::array<float, 3> pos, normal;
        std::array<float, 2> tex;
        /// No glNormal came before this vertex in the draw: its normal is
        /// whatever was current before (the probe's previous draw).
        bool inherited_normal{false};
    };
    struct Prim {
        std::string kind;   // point, line, line-reset, polygon
        std::vector<Vertex> v;
    };
    std::vector<Prim> prims;

    void begin(u32 mode) override {
        m_mode = mode;
        m_verts.clear();
    }
    void end() override {
        const auto& v = m_verts;
        switch (m_mode) {
        case 0x0000:
            for (const auto& x : v) prims.push_back({"point", {x}});
            break;
        case 0x0003:
            for (usize k = 0; k + 1 < v.size(); ++k) prims.push_back({k == 0 ? "line-reset" : "line", {v[k], v[k + 1]}});
            break;
        case 0x0006:
            for (usize k = 1; k + 1 < v.size(); ++k) prims.push_back({"polygon", {v[0], v[k], v[k + 1]}});
            break;
        case 0x0008:
            for (usize k = 0; k + 3 < v.size(); k += 2) {
                prims.push_back({"polygon", {v[k], v[k + 1], v[k + 3]}});
                prims.push_back({"polygon", {v[k + 3], v[k + 2], v[k]}});
            }
            break;
        default:
            ADD_FAILURE() << "unexpected mode " << m_mode;
        }
    }
    void normal(float x, float y, float z) override {
        m_normal = {x, y, z};
        m_normal_set = true;
    }
    void texcoord(float s, float t) override { m_tex = {s, t}; }
    void vertex(float x, float y, float z) override { m_verts.push_back({{x, y, z}, m_normal, m_tex, !m_normal_set}); }

private:
    u32 m_mode{0};
    std::array<float, 3> m_normal{0, 0, 1};
    std::array<float, 2> m_tex{0, 0};
    bool m_normal_set{false};
    std::vector<Vertex> m_verts;
};

struct OraclePrim {
    std::string kind;
    std::vector<FeedbackRecorder::Vertex> v;
};

const std::vector<std::string>& feedback_lines() {
    static const std::vector<std::string> lines = {
#define GLU32_FEEDBACK_LINE(s) s,
#include "glu32_oracle_feedback.inc"
#undef GLU32_FEEDBACK_LINE
    };
    return lines;
}

/// The oracle's primitives for @p tag (its "TAG pK kind | ..." lines), and
/// the prims count of its summary line.
std::vector<OraclePrim> oracle_prims(const std::string& tag, int& summary_prims) {
    std::vector<OraclePrim> out;
    summary_prims = -1;
    for (const std::string& line : feedback_lines()) {
        if (line.compare(0, tag.size() + 1, tag + " ") != 0) continue;
        std::istringstream in(line.substr(tag.size() + 1));
        std::string first;
        in >> first;
        if (first.rfind("floats=", 0) == 0) {
            const auto at = line.find("prims=");
            summary_prims = std::stoi(line.substr(at + 6));
            continue;
        }
        OraclePrim p;
        in >> p.kind;
        std::string tok;
        while (in >> tok) {
            if (tok != "|") break;
            FeedbackRecorder::Vertex v{};
            std::string n, t;
            in >> v.pos[0] >> v.pos[1] >> v.pos[2] >> n >> v.normal[0] >> v.normal[1] >> v.normal[2] >> t >> v.tex[0] >>
                v.tex[1];
            p.v.push_back(v);
        }
        out.push_back(p);
    }
    return out;
}

TEST(Glu32Quadrics, TessellationMatchesMicrosoftGlu32) {
    struct Style {
        const char* name;
        u32 draw_style, normals, orientation;
        bool texture;
    };
    const Style styles[] = {
        {"point", glu::GLU_POINT, glu::GLU_SMOOTH, glu::GLU_OUTSIDE, true},
        {"line", glu::GLU_LINE, glu::GLU_SMOOTH, glu::GLU_OUTSIDE, true},
        {"silhouette", glu::GLU_SILHOUETTE, glu::GLU_SMOOTH, glu::GLU_OUTSIDE, true},
        {"fill", glu::GLU_FILL, glu::GLU_SMOOTH, glu::GLU_OUTSIDE, true},
        {"fill-flat", glu::GLU_FILL, glu::GLU_FLAT, glu::GLU_OUTSIDE, true},
        {"fill-inside", glu::GLU_FILL, glu::GLU_SMOOTH, glu::GLU_INSIDE, true},
        {"line-inside", glu::GLU_LINE, glu::GLU_SMOOTH, glu::GLU_INSIDE, true},
        {"point-inside", glu::GLU_POINT, glu::GLU_SMOOTH, glu::GLU_INSIDE, true},
        {"silhouette-inside", glu::GLU_SILHOUETTE, glu::GLU_SMOOTH, glu::GLU_INSIDE, true},
        {"line-flat", glu::GLU_LINE, glu::GLU_FLAT, glu::GLU_OUTSIDE, true},
        {"fill-none-notex", glu::GLU_FILL, glu::GLU_NONE, glu::GLU_OUTSIDE, false},
    };
    using DrawFn = u32 (*)(const glu::QuadricState&, glu::QuadricSink&);
    struct Shape {
        const char* name;
        DrawFn draw;
    };
    const Shape shapes[] = {
        {"sphere-r0.5-4-3", [](const glu::QuadricState& q, glu::QuadricSink& s) { return glu::draw_sphere(q, 0.5, 4, 3, s); }},
        {"cylinder-0.5-0.25-h0.5-4-2",
         [](const glu::QuadricState& q, glu::QuadricSink& s) { return glu::draw_cylinder(q, 0.5, 0.25, 0.5, 4, 2, s); }},
        {"disk-0.125-0.5-4-2", [](const glu::QuadricState& q, glu::QuadricSink& s) { return glu::draw_disk(q, 0.125, 0.5, 4, 2, s); }},
        {"disk-0-0.5-3-1", [](const glu::QuadricState& q, glu::QuadricSink& s) { return glu::draw_disk(q, 0, 0.5, 3, 1, s); }},
        {"pdisk-0.125-0.5-3-2-30-120",
         [](const glu::QuadricState& q, glu::QuadricSink& s) { return glu::draw_partial_disk(q, 0.125, 0.5, 3, 2, 30, 120, s); }},
        {"pdisk-0-0.5-2-1-0-360",
         [](const glu::QuadricState& q, glu::QuadricSink& s) { return glu::draw_partial_disk(q, 0, 0.5, 2, 1, 0, 360, s); }},
        {"pdisk-0.125-0.5-2-1-90-m90",
         [](const glu::QuadricState& q, glu::QuadricSink& s) { return glu::draw_partial_disk(q, 0.125, 0.5, 2, 1, 90, -90, s); }},
    };
    int compared = 0;
    for (const Style& st : styles) {
        for (const Shape& sh : shapes) {
            const std::string tag = std::string("glu-fb-") + st.name + "-" + sh.name;
            int summary = -1;
            const auto want = oracle_prims(tag, summary);
            ASSERT_GE(summary, 0) << tag << " missing from the fixture";
            ASSERT_EQ(want.size(), static_cast<usize>(summary)) << tag;
            glu::QuadricState q;
            q.draw_style = st.draw_style;
            q.normals = st.normals;
            q.orientation = st.orientation;
            q.texture = st.texture;
            FeedbackRecorder rec;
            EXPECT_EQ(sh.draw(q, rec), 0u) << tag;
            ASSERT_EQ(rec.prims.size(), want.size()) << tag;
            for (usize p = 0; p < want.size(); ++p) {
                EXPECT_EQ(rec.prims[p].kind, want[p].kind) << tag << " p" << p;
                ASSERT_EQ(rec.prims[p].v.size(), want[p].v.size()) << tag << " p" << p;
                for (usize k = 0; k < want[p].v.size(); ++k) {
                    const auto& g = rec.prims[p].v[k];
                    const auto& w = want[p].v[k];
                    for (int c = 0; c < 3; ++c) EXPECT_NEAR(g.pos[c], w.pos[c], 2e-4) << tag << " p" << p << " v" << k;
                    // GLU_FLAT sphere strips set the normal after their first vertex.
                    if (st.normals != glu::GLU_NONE && !g.inherited_normal) {
                        for (int c = 0; c < 3; ++c) {
                            EXPECT_NEAR(g.normal[c], w.normal[c], 2e-3) << tag << " p" << p << " v" << k << " n";
                        }
                    }
                    if (st.texture) {
                        for (int c = 0; c < 2; ++c) {
                            EXPECT_NEAR(g.tex[c], w.tex[c], 2e-3) << tag << " p" << p << " v" << k << " t";
                        }
                    }
                }
            }
            ++compared;
        }
    }
    EXPECT_EQ(compared, 77);
}

// The 239-slice cap (glu-quadric-disk-300-slices-points 478,
// -sphere-300-2-points 717, -sphere-4-300-points 960).
TEST(Glu32Quadrics, SlicesAreCapped) {
    glu::QuadricState q;
    q.draw_style = glu::GLU_POINT;
    FeedbackRecorder a, b, c;
    (void)glu::draw_disk(q, 0, 0.5, 300, 1, a);
    (void)glu::draw_sphere(q, 0.5, 300, 2, b);
    (void)glu::draw_sphere(q, 0.5, 4, 300, c);
    EXPECT_EQ(a.prims.size(), 478u);
    EXPECT_EQ(b.prims.size(), 717u);
    EXPECT_EQ(c.prims.size(), 960u);
}

}  // namespace
