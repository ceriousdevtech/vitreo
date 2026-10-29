/// @file test_gl_pixels.cpp
/// @brief OpenGL 1.1 pixel rectangles without a GPU: the current raster
///        position (spec 2.12) through every glRasterPos form, glBitmap's
///        raster-position update (3.7), the errors of glDrawPixels /
///        glCopyPixels / glReadPixels / glBitmap, and the CPU unpack and pack
///        paths for index, stencil and depth groups (3.6.4, 4.3.2).
///
/// Expected values come from the spec's formulas; where Windows defines what
/// the spec leaves open, the GDI Generic value is cited by its probe tag
/// (tools/oracle_probe/gl11_state_probe64.expected). Export-level tests run
/// x86 and x64 through the registered thunks with a real guest frame (x64:
/// floats in XMM0-3, garbage in unused registers).

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <variant>
#include <vector>

#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/graphics/opengl32/gl_commands.hpp"
#include "winapi/graphics/opengl32/gl_context.hpp"
#include "winapi/graphics/opengl32/gl_exports.hpp"
#include "winapi/graphics/opengl32/gl_pixels.hpp"
#include "winapi/graphics/opengl32/opengl32.hpp"

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::gl;

namespace {

u32 fbits(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}
u64 dbits(double d) {
    u64 b;
    std::memcpy(&b, &d, 8);
    return b;
}
float bitsf(u32 b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

struct I { u32 v; };
struct F { float v; };
struct D { double v; };
struct P { GuestAddr v; };
using Arg = std::variant<I, F, D, P>;

/// The real exports, called through a guest CPU frame (the ABI of the bitness).
class Harness {
public:
    static constexpr GuestAddr MEM = 0x10000;
    static constexpr GuestAddr STACK = 0x80000;
    static constexpr GuestAddr OUT = MEM + 0x1000;
    static constexpr GuestAddr IN = MEM + 0x3000;

    explicit Harness(bool x64) : m_x64(x64) {
        EXPECT_TRUE(memory.map_region(MEM, 0x10000, MemoryProtection::ReadWrite).is_ok());
        EXPECT_TRUE(memory.map_region(STACK, 0x2000, MemoryProtection::ReadWrite).is_ok());
        EXPECT_TRUE(memory.map_region(OPENGL32_SCRATCH_BASE, OPENGL32_SCRATCH_SIZE,
                                      MemoryProtection::ReadWrite).is_ok());
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        registry = std::make_unique<ThunkRegistry>(0x70000000);
        mgr.set_thunk_registry(registry.get());
        EXPECT_TRUE(registry->register_dll("opengl32.dll", create_opengl32_thunks(mgr)).is_ok());
    }

    u64 call(const char* name, std::vector<Arg> args) {
        auto addr = registry->get_thunk_address("opengl32.dll", name);
        EXPECT_TRUE(addr.is_ok()) << name;
        if (!addr.is_ok()) return ~0ull;
        auto handler = registry->resolve_handler(addr.value());
        EXPECT_TRUE(handler.is_ok()) << name;
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
            u16 bytes = 0;
            for (const Arg& arg : args) {
                if (auto* a = std::get_if<I>(&arg)) { (void)memory.write_u32(at, a->v); at += 4; bytes += 4; }
                else if (auto* f = std::get_if<F>(&arg)) { (void)memory.write_u32(at, fbits(f->v)); at += 4; bytes += 4; }
                else if (auto* d = std::get_if<D>(&arg)) { (void)memory.write_u64(at, dbits(d->v)); at += 8; bytes += 8; }
                else { (void)memory.write_u32(at, static_cast<u32>(std::get<P>(arg).v)); at += 4; bytes += 4; }
            }
            EXPECT_EQ(registry->resolve_stdcall_bytes(addr.value()), bytes) << name;
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        (*handler.value())(m);
        return m_x64 ? cpu.gpr.rax : (cpu.gpr.rax & 0xFFFFFFFFull);
    }

    u32 err() { return static_cast<u32>(call("glGetError", {})); }
    std::vector<float> getf(u32 pname, u32 n) {
        call("glGetFloatv", {I{pname}, P{OUT}});
        std::vector<float> out(n);
        for (u32 k = 0; k < n; ++k) out[k] = bitsf(memory.read_u32(OUT + 4 * k).value());
        return out;
    }
    bool valid() {
        (void)memory.write_u8(OUT, 0x5A);
        call("glGetBooleanv", {I{GL_CURRENT_RASTER_POSITION_VALID}, P{OUT}});
        return memory.read_u8(OUT).value() != 0;
    }
    i32 geti(u32 pname) {
        call("glGetIntegerv", {I{pname}, P{OUT}});
        return static_cast<i32>(memory.read_u32(OUT).value());
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    Opengl32Manager mgr;
    std::unique_ptr<ThunkRegistry> registry;

private:
    bool m_x64;
};

/// The probe's context: pixel format 3 (32 depth bits, 8 stencil, no alpha), 320 x 200.
GlFramebufferConfig format3() {
    GlFramebufferConfig fb;
    fb.index_bits = 8;
    fb.depth_bits = 32;
    fb.stencil_bits = 8;
    fb.accum_red_bits = fb.accum_green_bits = fb.accum_blue_bits = 16;
    return fb;
}

struct CurrentContext {
    explicit CurrentContext(const GlFramebufferConfig& fb = format3()) : ctx(fb) {
        ctx.on_first_make_current(320, 200);
        set_current_gl_context_for_thread(&ctx);
    }
    ~CurrentContext() { set_current_gl_context_for_thread(nullptr); }
    GlContext ctx;
};

class GlPixelsExportTest : public ::testing::TestWithParam<bool> {
protected:
    Harness gl{GetParam()};
    CurrentContext current;
    GlContext& ctx = current.ctx;

    void ortho() {
        gl.call("glMatrixMode", {I{GL_PROJECTION}});
        gl.call("glLoadIdentity", {});
        gl.call("glOrtho", {D{0}, D{320}, D{0}, D{200}, D{-1}, D{1}});
        gl.call("glMatrixMode", {I{GL_MODELVIEW}});
        gl.call("glLoadIdentity", {});
    }
    void expect4(u32 pname, std::array<double, 4> want, const char* tag, double tol = 1e-5) {
        const auto v = gl.getf(pname, 4);
        for (u32 k = 0; k < 4; ++k) EXPECT_NEAR(v[k], want[k], tol) << tag << " [" << k << "]";
    }
    float get1(u32 pname) { return gl.getf(pname, 1)[0]; }
};

std::string bitness_name(const ::testing::TestParamInfo<bool>& info) { return info.param ? "x64" : "x86"; }

// ---- Spec 2.12: transformed, clipped as a point, window coordinates ------------------

TEST_P(GlPixelsExportTest, RasterPosTransformsAndClips) {
    ortho();
    // rasterpos-ortho-10.25-20.75-0.5: x_w, y_w per the viewport map; z_w =
    // (z_d + 1) / 2 with z_d = -0.5 (glOrtho negates z), so 0.25; w_c = 1.
    // GDI Generic reports x 10.2539062: it quantises window x/y to a
    // fixed-point grid with a bias, which the spec does not ask for.
    gl.call("glColor4f", {F{0.25f}, F{0.5f}, F{0.75f}, F{1.0f}});
    gl.call("glTexCoord4f", {F{1}, F{2}, F{3}, F{2}});
    gl.call("glRasterPos3f", {F{10.25f}, F{20.75f}, F{0.5f}});
    EXPECT_EQ(gl.err(), 0u);
    EXPECT_TRUE(gl.valid());
    expect4(GL_CURRENT_RASTER_POSITION, {10.25, 20.75, 0.25, 1}, "ortho position");
    // GL_CURRENT_RASTER_DISTANCE is the eye z on GDI Generic (0.5 here).
    EXPECT_NEAR(get1(GL_CURRENT_RASTER_DISTANCE), 0.5, 1e-6);
    expect4(GL_CURRENT_RASTER_COLOR, {0.25, 0.5, 0.75, 1}, "ortho colour");
    // The texture coordinates are stored undivided (tex=1,2,3,2).
    expect4(GL_CURRENT_RASTER_TEXTURE_COORDS, {1, 2, 3, 2}, "ortho tex");

    // rasterpos-color-out-of-range-index5: the colour is clamped (2.13.6);
    // the RGBA-mode colour index keeps its initial 1.
    gl.call("glColor4f", {F{2.0f}, F{-1.0f}, F{0.5f}, F{1.5f}});
    gl.call("glIndexi", {I{5}});
    gl.call("glRasterPos2i", {I{3}, I{4}});
    expect4(GL_CURRENT_RASTER_POSITION, {3, 4, 0.5, 1}, "2i position");
    expect4(GL_CURRENT_RASTER_COLOR, {1, 0, 0.5, 1}, "clamped colour");
    EXPECT_EQ(gl.geti(GL_CURRENT_RASTER_INDEX), 1);
    EXPECT_NEAR(get1(GL_CURRENT_RASTER_DISTANCE), 0.0, 1e-6);

    // rasterpos-texture-matrix-scale-2-3-4-tex-1-1-1-2.
    gl.call("glMatrixMode", {I{GL_TEXTURE}});
    gl.call("glScalef", {F{2}, F{3}, F{4}});
    gl.call("glMatrixMode", {I{GL_MODELVIEW}});
    gl.call("glTexCoord4f", {F{1}, F{1}, F{1}, F{2}});
    gl.call("glRasterPos2s", {I{0xFFFF0003u}, I{0x12340004u}});   // shorts 3, 4 with garbage high halves
    expect4(GL_CURRENT_RASTER_TEXTURE_COORDS, {2, 3, 4, 2}, "texture matrix");
    expect4(GL_CURRENT_RASTER_POSITION, {3, 4, 0.5, 1}, "2s position");
    gl.call("glMatrixMode", {I{GL_TEXTURE}});
    gl.call("glLoadIdentity", {});
    gl.call("glMatrixMode", {I{GL_MODELVIEW}});
    gl.call("glTexCoord4f", {F{0}, F{0}, F{0}, F{1}});
    gl.call("glColor4f", {F{1}, F{1}, F{1}, F{1}});

    // rasterpos-frustum-eye-3-4-neg12: clip = P (3, 4, -12, 1) with
    // glFrustum(-1, 1, -1, 1, 1, 100): x_c = 3, y_c = 4, z_c = (101 * 12 - 200)
    // / 99, w_c = 12; window = ((x_c / w_c + 1) 160, (y_c / w_c + 1) 100,
    // (z_c / w_c + 1) / 2). Distance: the eye z, -12.
    gl.call("glMatrixMode", {I{GL_PROJECTION}});
    gl.call("glLoadIdentity", {});
    gl.call("glFrustum", {D{-1}, D{1}, D{-1}, D{1}, D{1}, D{100}});
    gl.call("glMatrixMode", {I{GL_MODELVIEW}});
    gl.call("glRasterPos3d", {D{3}, D{4}, D{-12}});
    const double zc = (101.0 * 12.0 - 200.0) / 99.0;
    expect4(GL_CURRENT_RASTER_POSITION, {(3.0 / 12 + 1) * 160, (4.0 / 12 + 1) * 100, (zc / 12 + 1) / 2, 12},
            "frustum position", 1e-4);
    EXPECT_NEAR(get1(GL_CURRENT_RASTER_DISTANCE), -12.0, 1e-5);

    // rasterpos-culled-behind-eye: the valid bit clears; the position and its
    // data keep the last valid values (GDI Generic), colour and texture too.
    gl.call("glColor4f", {F{0.5f}, F{0.5f}, F{0.5f}, F{0.5f}});
    gl.call("glTexCoord4f", {F{5}, F{6}, F{7}, F{1}});
    gl.call("glRasterPos3f", {F{0}, F{0}, F{1}});
    EXPECT_EQ(gl.err(), 0u);
    EXPECT_FALSE(gl.valid());
    expect4(GL_CURRENT_RASTER_POSITION, {(3.0 / 12 + 1) * 160, (4.0 / 12 + 1) * 100, (zc / 12 + 1) / 2, 12},
            "culled keeps position", 1e-4);
    expect4(GL_CURRENT_RASTER_COLOR, {1, 1, 1, 1}, "culled keeps colour");
    expect4(GL_CURRENT_RASTER_TEXTURE_COORDS, {0, 0, 0, 1}, "culled keeps tex");
    EXPECT_NEAR(get1(GL_CURRENT_RASTER_DISTANCE), -12.0, 1e-5);
    // bitmap-invalid-rasterpos-move: ignored, not moved, no error (3.7).
    gl.call("glBitmap", {I{0}, I{0}, F{0}, F{0}, F{5}, F{5}, P{0}});
    EXPECT_EQ(gl.err(), 0u);
    EXPECT_FALSE(gl.valid());
    expect4(GL_CURRENT_RASTER_POSITION, {(3.0 / 12 + 1) * 160, (4.0 / 12 + 1) * 100, (zc / 12 + 1) / 2, 12},
            "invalid not moved", 1e-4);

    gl.call("glColor4f", {F{1}, F{1}, F{1}, F{1}});
    gl.call("glTexCoord4f", {F{0}, F{0}, F{0}, F{1}});
    ortho();
    gl.call("glRasterPos2f", {F{400}, F{10}});
    EXPECT_FALSE(gl.valid()) << "rasterpos-culled-right-of-volume";
    // rasterpos-on-volume-corner-320-200: the boundary is inside (-w <= x <= w).
    gl.call("glRasterPos2d", {D{320}, D{200}});
    EXPECT_TRUE(gl.valid());
    expect4(GL_CURRENT_RASTER_POSITION, {320, 200, 0.5, 1}, "corner");
    // rasterpos-w0.
    gl.call("glRasterPos4f", {F{1}, F{1}, F{0}, F{0}});
    EXPECT_FALSE(gl.valid());
    // rasterpos-user-clip-plane-culled / -kept: -x + 50 >= 0.
    const GuestAddr plane = Harness::IN;
    for (u32 k = 0; k < 4; ++k) (void)gl.memory.write_u64(plane + 8 * k, dbits(std::array<double, 4>{-1, 0, 0, 50}[k]));
    gl.call("glClipPlane", {I{GL_CLIP_PLANE0}, P{plane}});
    gl.call("glEnable", {I{GL_CLIP_PLANE0}});
    gl.call("glRasterPos2f", {F{60}, F{10}});
    EXPECT_FALSE(gl.valid());
    gl.call("glRasterPos2f", {F{40}, F{10}});
    EXPECT_TRUE(gl.valid());
    expect4(GL_CURRENT_RASTER_POSITION, {40, 10, 0.5, 1}, "kept by the plane");
    gl.call("glDisable", {I{GL_CLIP_PLANE0}});

    // rasterpos-lit-directional-diffuse: 2.13.1 with one directional light
    // (diffuse (1, 0.5, 0.25)), normal (0, 0, 1), material diffuse (0.5, 0.5,
    // 0.5, 0.75), no ambient: c = d_cm d_cli (n . VP = 1), alpha d_cm's.
    cmd::LightModel(ctx, GL_LIGHT_MODEL_AMBIENT, GlValues::vector(std::array<double, 4>{0, 0, 0, 1}, false));
    cmd::Light(ctx, GL_LIGHT0, GL_AMBIENT, GlValues::vector(std::array<double, 4>{0, 0, 0, 1}, false));
    cmd::Light(ctx, GL_LIGHT0, GL_DIFFUSE, GlValues::vector(std::array<double, 4>{1, 0.5, 0.25, 1}, false));
    cmd::Light(ctx, GL_LIGHT0, GL_POSITION, GlValues::vector(std::array<double, 4>{0, 0, 1, 0}, false));
    cmd::Material(ctx, GL_FRONT_AND_BACK, GL_DIFFUSE, GlValues::vector(std::array<double, 4>{0.5, 0.5, 0.5, 0.75}, false));
    gl.call("glEnable", {I{GL_LIGHT0}});
    gl.call("glEnable", {I{GL_LIGHTING}});
    gl.call("glNormal3f", {F{0}, F{0}, F{1}});
    gl.call("glRasterPos2f", {F{5}, F{5}});
    expect4(GL_CURRENT_RASTER_COLOR, {0.5, 0.25, 0.125, 0.75}, "lit");
    gl.call("glDisable", {I{GL_LIGHTING}});

    // rasterpos-texgen-object-s-eq-x-at-7: OBJECT_LINEAR s = x.
    cmd::TexGen(ctx, GL_S, GL_TEXTURE_GEN_MODE, GlValues::scalar_value(GL_OBJECT_LINEAR, true));
    cmd::TexGen(ctx, GL_S, GL_OBJECT_PLANE, GlValues::vector(std::array<double, 4>{1, 0, 0, 0}, false));
    gl.call("glEnable", {I{GL_TEXTURE_GEN_S}});
    gl.call("glRasterPos2f", {F{7}, F{5}});
    expect4(GL_CURRENT_RASTER_TEXTURE_COORDS, {7, 0, 0, 1}, "texgen");
    gl.call("glDisable", {I{GL_TEXTURE_GEN_S}});

    // The vector forms read the guest array; 4 components divide by w.
    for (u32 k = 0; k < 4; ++k) (void)gl.memory.write_u32(Harness::IN + 4 * k, fbits(std::array<float, 4>{20, 40, 0, 2}[k]));
    gl.call("glRasterPos4fv", {P{Harness::IN}});
    expect4(GL_CURRENT_RASTER_POSITION, {10, 20, 0.5, 2}, "4fv");
    for (u32 k = 0; k < 3; ++k) (void)gl.memory.write_u32(Harness::IN + 4 * k, std::array<u32, 3>{6, 8, 0}[k]);
    gl.call("glRasterPos3iv", {P{Harness::IN}});
    expect4(GL_CURRENT_RASTER_POSITION, {6, 8, 0.5, 1}, "3iv");
    (void)gl.memory.write_u16(Harness::IN, 9);
    (void)gl.memory.write_u16(Harness::IN + 2, 11);
    gl.call("glRasterPos2sv", {P{Harness::IN}});
    expect4(GL_CURRENT_RASTER_POSITION, {9, 11, 0.5, 1}, "2sv");
    for (u32 k = 0; k < 2; ++k) (void)gl.memory.write_u64(Harness::IN + 8 * k, dbits(std::array<double, 2>{12.5, 13.5}[k]));
    gl.call("glRasterPos2dv", {P{Harness::IN}});
    expect4(GL_CURRENT_RASTER_POSITION, {12.5, 13.5, 0.5, 1}, "2dv");

    // rasterpos-inside-begin.
    gl.call("glBegin", {I{GL_POINTS}});
    gl.call("glRasterPos2i", {I{1}, I{1}});
    gl.call("glEnd", {});
    EXPECT_EQ(gl.err(), static_cast<u32>(GL_INVALID_OPERATION));
    expect4(GL_CURRENT_RASTER_POSITION, {12.5, 13.5, 0.5, 1}, "not changed inside begin");
}

// ---- Spec 3.7: the raster position advances by (xmove, ymove) ------------------------

TEST_P(GlPixelsExportTest, BitmapAdvancesRasterPos) {
    ortho();
    gl.call("glRasterPos2i", {I{10}, I{10}});
    // bitmap-null-zero-size-move / bitmap-after-move-5.5-neg2.25: a 0 x 0
    // bitmap with a NULL pointer only moves the raster position.
    gl.call("glBitmap", {I{0}, I{0}, F{0}, F{0}, F{5.5f}, F{-2.25f}, P{0}});
    EXPECT_EQ(gl.err(), 0u);
    expect4(GL_CURRENT_RASTER_POSITION, {15.5, 7.75, 0.5, 1}, "moved");
    // bitmap-8x2-move: the bits do not change the move.
    (void)gl.memory.write_u16(Harness::IN, 0xFFFF);
    gl.call("glRasterPos2i", {I{10}, I{10}});
    gl.call("glBitmap", {I{8}, I{2}, F{0.5f}, F{0.5f}, F{8}, F{0}, P{Harness::IN}});
    EXPECT_EQ(gl.err(), 0u);
    expect4(GL_CURRENT_RASTER_POSITION, {18, 10, 0.5, 1}, "8x2 moved");
    // bitmap-neg-width / bitmap-after-neg-width: INVALID_VALUE, not moved.
    gl.call("glBitmap", {I{static_cast<u32>(-1)}, I{1}, F{0}, F{0}, F{1}, F{1}, P{0}});
    EXPECT_EQ(gl.err(), static_cast<u32>(GL_INVALID_VALUE));
    expect4(GL_CURRENT_RASTER_POSITION, {18, 10, 0.5, 1}, "neg width not moved");
    // bitmap-inside-begin.
    gl.call("glBegin", {I{GL_POINTS}});
    gl.call("glBitmap", {I{0}, I{0}, F{0}, F{0}, F{1}, F{1}, P{0}});
    gl.call("glEnd", {});
    EXPECT_EQ(gl.err(), static_cast<u32>(GL_INVALID_OPERATION));
    expect4(GL_CURRENT_RASTER_POSITION, {18, 10, 0.5, 1}, "inside begin not moved");
    // The z and w of the raster position are unchanged by the move.
    gl.call("glRasterPos4f", {F{20}, F{20}, F{0.5f}, F{2}});
    gl.call("glBitmap", {I{0}, I{0}, F{0}, F{0}, F{-3}, F{4}, P{0}});
    expect4(GL_CURRENT_RASTER_POSITION, {7, 14, 0.375, 2}, "z and w kept");
}

// ---- Errors (probe lines drawpixels-*, copypixels-*, readpixels-*) ----------------------

TEST_P(GlPixelsExportTest, PixelErrorsMatchTheOracle) {
    ortho();
    const GuestAddr px = Harness::IN;
    gl.call("glRasterPos2i", {I{0}, I{0}});
    struct Case {
        const char* tag;
        const char* api;
        std::vector<Arg> args;
        u32 err;
    };
    const auto draw = [&](i32 w, i32 h, u32 f, u32 t) {
        return std::vector<Arg>{I{static_cast<u32>(w)}, I{static_cast<u32>(h)}, I{f}, I{t}, P{px}};
    };
    const auto read = [&](i32 w, i32 h, u32 f, u32 t) {
        return std::vector<Arg>{I{0}, I{0}, I{static_cast<u32>(w)}, I{static_cast<u32>(h)}, I{f}, I{t}, P{Harness::OUT}};
    };
    const auto copy = [&](i32 w, i32 h, u32 t) {
        return std::vector<Arg>{I{0}, I{0}, I{static_cast<u32>(w)}, I{static_cast<u32>(h)}, I{t}};
    };
    const Case cases[] = {
        {"drawpixels-rgba-ubyte-ok", "glDrawPixels", draw(1, 1, GL_RGBA, GL_UNSIGNED_BYTE), 0},
        {"drawpixels-color-index-rgba-mode", "glDrawPixels", draw(1, 1, GL_COLOR_INDEX, GL_UNSIGNED_BYTE), 0},
        {"drawpixels-color-index-bitmap", "glDrawPixels", draw(8, 1, GL_COLOR_INDEX, GL_BITMAP), 0},
        {"drawpixels-stencil-bitmap", "glDrawPixels", draw(8, 1, GL_STENCIL_INDEX, GL_BITMAP), 0},
        {"drawpixels-rgba-bitmap", "glDrawPixels", draw(8, 1, GL_RGBA, GL_BITMAP), GL_INVALID_ENUM},
        {"drawpixels-depth-bitmap", "glDrawPixels", draw(8, 1, GL_DEPTH_COMPONENT, GL_BITMAP), GL_INVALID_ENUM},
        {"drawpixels-depth-float", "glDrawPixels", draw(1, 1, GL_DEPTH_COMPONENT, GL_FLOAT), 0},
        {"drawpixels-stencil-float", "glDrawPixels", draw(1, 1, GL_STENCIL_INDEX, GL_FLOAT), 0},
        {"drawpixels-bad-format", "glDrawPixels", draw(1, 1, 0x1234, GL_UNSIGNED_BYTE), GL_INVALID_ENUM},
        {"drawpixels-bad-type", "glDrawPixels", draw(1, 1, GL_RGBA, 0x1234), GL_INVALID_ENUM},
        {"drawpixels-bad-format-and-type", "glDrawPixels", draw(1, 1, 0x1234, 0x1234), GL_INVALID_ENUM},
        {"drawpixels-neg-and-bad-format", "glDrawPixels", draw(-1, 1, 0x1234, GL_UNSIGNED_BYTE), GL_INVALID_VALUE},
        {"err-bad-value-glDrawPixels-neg", "glDrawPixels", draw(-1, 1, GL_RGBA, GL_UNSIGNED_BYTE), GL_INVALID_VALUE},
        {"drawpixels-zero-size", "glDrawPixels", draw(0, 0, GL_RGBA, GL_UNSIGNED_BYTE), 0},
        {"drawpixels-format-1", "glDrawPixels", draw(1, 1, 1, GL_UNSIGNED_BYTE), GL_INVALID_ENUM},
        {"drawpixels-format-luminance-alpha", "glDrawPixels", draw(1, 1, GL_LUMINANCE_ALPHA, GL_FLOAT), 0},
        {"copypixels-color-ok", "glCopyPixels", copy(1, 1, GL_COLOR), 0},
        {"copypixels-depth-ok", "glCopyPixels", copy(1, 1, GL_DEPTH), 0},
        {"copypixels-stencil-ok", "glCopyPixels", copy(1, 1, GL_STENCIL), 0},
        {"copypixels-type-rgba", "glCopyPixels", copy(1, 1, GL_RGBA), GL_INVALID_ENUM},
        {"err-bad-enum-glCopyPixels-type", "glCopyPixels", copy(1, 1, 0x1234), GL_INVALID_ENUM},
        {"err-bad-value-glCopyPixels-neg", "glCopyPixels", copy(-1, 1, GL_COLOR), GL_INVALID_VALUE},
        {"readpixels-color-index-rgba-mode", "glReadPixels", read(1, 1, GL_COLOR_INDEX, GL_UNSIGNED_BYTE),
         GL_INVALID_OPERATION},
        {"readpixels-stencil-bitmap", "glReadPixels", read(8, 1, GL_STENCIL_INDEX, GL_BITMAP), 0},
        {"readpixels-depth-bitmap", "glReadPixels", read(8, 1, GL_DEPTH_COMPONENT, GL_BITMAP), GL_INVALID_OPERATION},
        {"readpixels-color-index-bitmap", "glReadPixels", read(8, 1, GL_COLOR_INDEX, GL_BITMAP), GL_INVALID_OPERATION},
        {"err-bad-operation-glReadPixels-bitmap-type", "glReadPixels", read(1, 1, GL_RGBA, GL_BITMAP),
         GL_INVALID_OPERATION},
        {"readpixels-bad-format-and-type", "glReadPixels", read(1, 1, 0x1234, 0x1234), GL_INVALID_ENUM},
        {"err-bad-enum-glReadPixels-format", "glReadPixels", read(1, 1, 0x1234, GL_UNSIGNED_BYTE), GL_INVALID_ENUM},
        {"err-bad-enum-glReadPixels-type", "glReadPixels", read(1, 1, GL_RGBA, 0x1234), GL_INVALID_ENUM},
        {"readpixels-neg-and-bad-format", "glReadPixels", read(-1, 1, 0x1234, GL_UNSIGNED_BYTE), GL_INVALID_VALUE},
        {"err-neg-size-glReadPixels", "glReadPixels", read(-1, 1, GL_RGBA, GL_UNSIGNED_BYTE), GL_INVALID_VALUE},
        {"readpixels-format-1", "glReadPixels", read(1, 1, 1, GL_UNSIGNED_BYTE), GL_INVALID_ENUM},
        {"err-neg-size-glBitmap", "glBitmap",
         {I{static_cast<u32>(-1)}, I{1}, F{0}, F{0}, F{0}, F{0}, P{0}}, GL_INVALID_VALUE},
    };
    for (const Case& c : cases) {
        gl.call(c.api, c.args);
        EXPECT_EQ(gl.err(), c.err) << c.tag;
        EXPECT_EQ(gl.err(), 0u) << c.tag << " (then)";
    }
    // drawpixels-invalid-rasterpos(-bad-format): the arguments are checked
    // first; a valid call with an invalid raster position is silent.
    gl.call("glRasterPos2f", {F{1000}, F{0}});
    gl.call("glDrawPixels", draw(1, 1, GL_RGBA, GL_UNSIGNED_BYTE));
    EXPECT_EQ(gl.err(), 0u);
    gl.call("glDrawPixels", draw(1, 1, 0x1234, GL_UNSIGNED_BYTE));
    EXPECT_EQ(gl.err(), static_cast<u32>(GL_INVALID_ENUM));
    // *-inside-begin.
    for (const char* api : {"glDrawPixels", "glReadPixels", "glCopyPixels"}) {
        gl.call("glBegin", {I{GL_POINTS}});
        if (std::string_view(api) == "glDrawPixels") gl.call(api, draw(1, 1, GL_RGBA, GL_UNSIGNED_BYTE));
        else if (std::string_view(api) == "glReadPixels") gl.call(api, read(1, 1, GL_RGBA, GL_UNSIGNED_BYTE));
        else gl.call(api, copy(1, 1, GL_COLOR));
        gl.call("glEnd", {});
        EXPECT_EQ(gl.err(), static_cast<u32>(GL_INVALID_OPERATION)) << api;
    }
}

// A format without depth or stencil buffers: every GDI Generic format has
// both (wgl_contract_probe64.expected pf-list), so these follow the spec:
// ReadPixels of a missing buffer is INVALID_OPERATION (4.3.2), CopyPixels
// reads as ReadPixels does (4.3.3; Learn glCopyPixels), and DrawPixels of
// STENCIL_INDEX without a stencil buffer is INVALID_OPERATION (Learn
// glDrawPixels), after the size and enum checks. DrawPixels of
// DEPTH_COMPONENT without a depth buffer is an error in no source.
TEST(GlPixelsTest, MissingDepthOrStencilBufferIsInvalidOperation) {
    GlFramebufferConfig fb;
    fb.depth_bits = 0;
    fb.stencil_bits = 0;
    GlContext ctx(fb);
    ctx.on_first_make_current(64, 64);
    std::vector<u8> bytes(64, 0);
    cmd::GlClientPixels px;
    px.bytes = bytes;
    const auto expect = [&](u32 want, const char* what) { EXPECT_EQ(ctx.take_error(), want) << what; };
    cmd::DrawPixels(ctx, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, px);
    expect(GL_NO_ERROR, "draw depth: no error is defined");
    cmd::DrawPixels(ctx, 1, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, px);
    expect(GL_INVALID_OPERATION, "draw stencil");
    cmd::DrawPixels(ctx, -1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, px);
    expect(GL_INVALID_VALUE, "size first");
    cmd::DrawPixels(ctx, 1, 1, GL_STENCIL_INDEX, 0x1234, px);
    expect(GL_INVALID_ENUM, "type before buffer");
    cmd::DrawPixels(ctx, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    expect(GL_NO_ERROR, "colour is fine");
    cmd::CopyPixels(ctx, 0, 0, 1, 1, GL_DEPTH);
    expect(GL_INVALID_OPERATION, "copy depth");
    cmd::CopyPixels(ctx, 0, 0, 1, 1, GL_STENCIL);
    expect(GL_INVALID_OPERATION, "copy stencil");
    cmd::CopyPixels(ctx, 0, 0, 1, 1, GL_COLOR);
    expect(GL_NO_ERROR, "copy colour");
    const cmd::GlClientBuffer client = [](void*, usize) -> std::span<u8> { return {}; };
    EXPECT_FALSE(cmd::ReadPixels(ctx, 0, 0, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, client, nullptr));
    expect(GL_INVALID_OPERATION, "read depth");
    EXPECT_FALSE(cmd::ReadPixels(ctx, 0, 0, 1, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, client, nullptr));
    expect(GL_INVALID_OPERATION, "read stencil");
    // An invalid raster position does not hide the error (the buffer check
    // is an argument check).
    cmd::RasterPos(ctx, 1000, 0, 0, 1);
    EXPECT_FALSE(ctx.state.current.raster_position_valid);
    cmd::DrawPixels(ctx, 1, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, px);
    expect(GL_INVALID_OPERATION, "draw stencil, invalid raster position");
}

// ---- The CPU paths (spec 3.6.4 unpack, 4.3.2 pack) ------------------------------------

GlPixelStoreModes tight() { return GlPixelStoreModes{false, false, 0, 0, 0, 1}; }

TEST(GlPixelsTest, StencilAndDepthUnpackFollowTheTransferPath) {
    GlContext ctx(format3());
    // Stencil: indices are not converted to floating point; shift 2, offset
    // 3: 0x5A -> 0x16B; then MAP_STENCIL looks S_TO_S up at index & (n - 1).
    const std::vector<u8> ub{0x5A, 0x01};
    std::vector<double> idx;
    ctx.state.pixel_mode.index_shift = 2;
    ctx.state.pixel_mode.index_offset = 3;
    gl_unpack_stencil(ctx, tight(), GL_UNSIGNED_BYTE, 2, 1, ub, idx);
    ASSERT_EQ(idx.size(), 2u);
    EXPECT_EQ(idx[0], 0x16B);
    EXPECT_EQ(idx[1], 7);
    // A right shift drops the fraction of an integer index (zero-filled).
    ctx.state.pixel_mode.index_shift = -3;
    ctx.state.pixel_mode.index_offset = 0;
    gl_unpack_stencil(ctx, tight(), GL_UNSIGNED_BYTE, 2, 1, ub, idx);
    EXPECT_EQ(idx[0], 0x5A >> 3);
    EXPECT_EQ(idx[1], 0);
    ctx.state.pixel_mode.index_shift = 0;
    ctx.pixel_maps[GL_PIXEL_MAP_S_TO_S - GL_PIXEL_MAP_I_TO_I] = {10, 11, 12, 13};
    ctx.state.pixel_mode.map_stencil = true;
    gl_unpack_stencil(ctx, tight(), GL_UNSIGNED_BYTE, 2, 1, ub, idx);
    EXPECT_EQ(idx[0], 12);   // 0x5A & 3 = 2
    EXPECT_EQ(idx[1], 11);
    ctx.state.pixel_mode.map_stencil = false;
    // BITMAP stencil groups under LSB_FIRST and SKIP_PIXELS.
    GlPixelStoreModes lsb = tight();
    lsb.lsb_first = true;
    lsb.skip_pixels = 1;
    const std::vector<u8> bits{0b00000110};
    gl_unpack_stencil(ctx, lsb, GL_BITMAP, 3, 1, bits, idx);
    ASSERT_EQ(idx.size(), 3u);
    EXPECT_EQ(idx[0], 1);
    EXPECT_EQ(idx[1], 1);
    EXPECT_EQ(idx[2], 0);

    // Depth: Table 2.6 conversion, DEPTH_SCALE / DEPTH_BIAS, clamp.
    const std::vector<u8> us{0xFF, 0xFF, 0x00, 0x80};   // 65535, 32768
    std::vector<float> depth;
    ctx.state.pixel_mode.depth_scale = 0.5f;
    ctx.state.pixel_mode.depth_bias = 0.25f;
    gl_unpack_depth(ctx, tight(), GL_UNSIGNED_SHORT, 2, 1, us, depth);
    ASSERT_EQ(depth.size(), 2u);
    EXPECT_FLOAT_EQ(depth[0], 0.75f);
    EXPECT_FLOAT_EQ(depth[1], static_cast<float>(32768.0 / 65535.0 * 0.5 + 0.25));
    ctx.state.pixel_mode.depth_scale = 4.0f;
    ctx.state.pixel_mode.depth_bias = -1.0f;
    gl_unpack_depth(ctx, tight(), GL_UNSIGNED_SHORT, 2, 1, us, depth);
    EXPECT_EQ(depth[0], 1.0f);   // 3 clamped
    EXPECT_EQ(depth[1], 1.0f);
    ctx.state.pixel_mode.depth_bias = -4.0f;
    gl_unpack_depth(ctx, tight(), GL_UNSIGNED_SHORT, 2, 1, us, depth);
    EXPECT_EQ(depth[0], 0.0f);
    // Signed and float types, and SWAP_BYTES.
    ctx.state.pixel_mode.depth_scale = 1.0f;
    ctx.state.pixel_mode.depth_bias = 0.0f;
    float f = 0.375f;
    std::vector<u8> fb(4);
    std::memcpy(fb.data(), &f, 4);
    std::reverse(fb.begin(), fb.end());
    GlPixelStoreModes swap = tight();
    swap.swap_bytes = true;
    gl_unpack_depth(ctx, swap, GL_FLOAT, 1, 1, fb, depth);
    EXPECT_EQ(depth[0], 0.375f);
    const std::vector<u8> sb{0x7F};   // byte 127 -> (2 * 127 + 1) / 255 = 1
    gl_unpack_depth(ctx, tight(), GL_BYTE, 1, 1, sb, depth);
    EXPECT_FLOAT_EQ(depth[0], 1.0f);
}

TEST(GlPixelsTest, ColorIndexGroupsBecomeRgbaThroughTheMaps) {
    GlContext ctx(format3());
    // Index arithmetic first (shift 1, offset 1: 3 -> 7), then I_TO_* at
    // round(i) & (n - 1) (3.6.4 "Color Index Lookup").
    ctx.pixel_maps[GL_PIXEL_MAP_I_TO_R - GL_PIXEL_MAP_I_TO_I] = {0.0f, 0.25f, 0.5f, 0.75f};
    ctx.pixel_maps[GL_PIXEL_MAP_I_TO_G - GL_PIXEL_MAP_I_TO_I] = {1.0f, 0.0f};
    ctx.pixel_maps[GL_PIXEL_MAP_I_TO_B - GL_PIXEL_MAP_I_TO_I] = {0.125f};
    ctx.pixel_maps[GL_PIXEL_MAP_I_TO_A - GL_PIXEL_MAP_I_TO_I] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1.0f};
    ctx.state.pixel_mode.index_shift = 1;
    ctx.state.pixel_mode.index_offset = 1;
    const std::vector<u8> bytes{3, 0};
    std::vector<GlRgba> out;
    gl_unpack_rgba(ctx, tight(), GL_COLOR_INDEX, GL_UNSIGNED_BYTE, 2, 1, bytes, out);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], (GlRgba{0.75f, 0.0f, 0.125f, 1.0f}));   // 7
    EXPECT_EQ(out[1], (GlRgba{0.25f, 0.0f, 0.125f, 0.5f}));   // 1
}

TEST(GlPixelsTest, ReadPixelsPacksIndicesAndDepth) {
    // readpixels-stencil-shift2-offset3-ubyte / -byte: 0x16B masked by
    // Table 4.6 (2^8 - 1, 2^7 - 1) -> 0x6B; FLOAT stores the index
    // (readpixels-stencil-float: 90).
    std::vector<u8> out(8, 0xAB);
    const std::vector<double> idx{0x16B};
    gl_pack_indices(tight(), GL_UNSIGNED_BYTE, 1, 1, idx, out);
    EXPECT_EQ(out[0], 0x6B);
    EXPECT_EQ(out[1], 0xAB);
    gl_pack_indices(tight(), GL_BYTE, 1, 1, idx, out);
    EXPECT_EQ(out[0], 0x6B);
    GlPixelStoreModes a4 = tight();
    a4.alignment = 4;
    gl_pack_indices(a4, GL_SHORT, 1, 1, std::vector<double>{0x1FFFF}, out);
    EXPECT_EQ(out[0], 0xFF);
    EXPECT_EQ(out[1], 0x7F);
    gl_pack_indices(a4, GL_UNSIGNED_INT, 1, 1, std::vector<double>{0x5A}, out);
    EXPECT_EQ((std::vector<u8>(out.begin(), out.begin() + 4)), (std::vector<u8>{0x5A, 0, 0, 0}));
    gl_pack_indices(a4, GL_FLOAT, 1, 1, std::vector<double>{90}, out);
    float f;
    std::memcpy(&f, out.data(), 4);
    EXPECT_EQ(f, 90.0f);
    // readpixels-stencil-bitmap-8: index & 1 per bit, MSB first; with
    // PACK_LSB_FIRST and PACK_SKIP_PIXELS only the addressed bits change.
    std::vector<u8> bits(2, 0xAB);
    gl_pack_indices(tight(), GL_BITMAP, 8, 1, std::vector<double>(8, 0x5A), bits);
    EXPECT_EQ(bits[0], 0x00);
    EXPECT_EQ(bits[1], 0xAB);
    bits.assign(2, 0x00);
    GlPixelStoreModes lsb = tight();
    lsb.lsb_first = true;
    lsb.skip_pixels = 6;
    gl_pack_indices(lsb, GL_BITMAP, 3, 1, std::vector<double>{1, 0, 3}, bits);
    EXPECT_EQ(bits[0], 0x40);   // bit 6
    EXPECT_EQ(bits[1], 0x01);   // bit 8 = byte 1 bit 0
    // Depth: clamped, Table 4.7 (round(0.5 * 65535) = 32768; GDI Generic's
    // 32-bit fixed-point depth of 0.5 reads 0x7fff in readpixels-depth-ushort).
    std::vector<u8> d(8, 0xAB);
    gl_pack_rgba(a4, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 2, 1,
                 std::vector<GlRgba>{{0.5f, 0, 0, 0}, {2.0f, 0, 0, 0}}, d);
    EXPECT_EQ((std::vector<u8>(d.begin(), d.begin() + 4)), (std::vector<u8>{0x00, 0x80, 0xFF, 0xFF}));
    // Groups marked absent keep their client bytes (readpixels-outside-*).
    std::vector<u8> rgba(8, 0xAB);
    gl_pack_rgba(tight(), GL_RGBA, GL_UNSIGNED_BYTE, 2, 1, std::vector<GlRgba>{{1, 0, 0, 1}, {0, 1, 0, 1}}, rgba,
                 std::vector<u8>{0, 1});
    EXPECT_EQ(rgba, (std::vector<u8>{0xAB, 0xAB, 0xAB, 0xAB, 0x00, 0xFF, 0x00, 0xFF}));
}

// Final review, Finding 1: the layout is checked arithmetic. The reviewer's
// values (UNPACK_ALIGNMENT 4, ROW_LENGTH 2^31 - 1, SKIP_ROWS 2^30,
// SKIP_PIXELS 2^30 - 100, RGBA FLOAT, 101 x 1) wrapped first_byte to -1600
// and the span to 16 (100 x 1: span 0, a read through nullptr - 1600); no
// address range can hold that image, so the layout is invalid and neither
// the readers nor the packers touch a byte.
TEST(GlPixelsTest, OverflowingLayoutIsInvalidAndTouchesNothing) {
    const GlPixelStoreModes wrap{false, false, 0x7FFFFFFF, 1 << 30, (1 << 30) - 100, 4};
    EXPECT_FALSE(gl_pixel_layout(wrap, GL_RGBA, GL_FLOAT, 101, 1).valid);
    EXPECT_FALSE(gl_pixel_layout(wrap, GL_RGBA, GL_FLOAT, 100, 1).valid);
    EXPECT_FALSE(gl_pixel_layout(wrap, GL_STENCIL_INDEX, GL_FLOAT, 1, 1).valid);
    // BITMAP: SKIP_ROWS (2^31 - 1) * k (2^28) fits 64 bits but is past any
    // user address space (128 TiB).
    const GlPixelStoreModes bits{false, false, 0x7FFFFFFF, 0x7FFFFFFF, 0, 8};
    EXPECT_FALSE(gl_pixel_layout(bits, GL_COLOR_INDEX, GL_BITMAP, 8, 8).valid);
    // A huge but representable span is a valid layout: only the mapping
    // decides whether it is client memory (gl_client_memory.hpp rule 2).
    const GlPixelStoreModes wide{false, false, 0x7FFFFFFF, 0, 0, 4};
    const GlPixelLayout big = gl_pixel_layout(wide, GL_RGBA, GL_UNSIGNED_BYTE, 1024, 1024);
    EXPECT_TRUE(big.valid);
    EXPECT_EQ(big.span_bytes, usize{1023} * 4u * 0x7FFFFFFFu + 4096u);

    GlContext ctx(format3());
    const std::vector<u8> sixteen(16, 0x3F);
    std::vector<GlRgba> groups{{1, 1, 1, 1}};
    gl_unpack_rgba(ctx, wrap, GL_RGBA, GL_FLOAT, 101, 1, sixteen, groups);
    EXPECT_TRUE(groups.empty());
    gl_unpack_rgba(ctx, wrap, GL_RGBA, GL_FLOAT, 100, 1, {}, groups);
    EXPECT_TRUE(groups.empty());
    std::vector<double> idx{1.0};
    gl_unpack_stencil(ctx, wrap, GL_FLOAT, 101, 1, sixteen, idx);
    EXPECT_TRUE(idx.empty());
    gl_unpack_color_indices(ctx, wrap, GL_FLOAT, 101, 1, sixteen, idx);
    EXPECT_TRUE(idx.empty());
    std::vector<float> depth{1.0f};
    gl_unpack_depth(ctx, wrap, GL_FLOAT, 101, 1, sixteen, depth);
    EXPECT_TRUE(depth.empty());
    std::vector<u8> out(16, 0xAB);
    gl_pack_rgba(wrap, GL_RGBA, GL_FLOAT, 101, 1, std::vector<GlRgba>(101, GlRgba{1, 0, 0, 1}), out);
    gl_pack_indices(wrap, GL_FLOAT, 101, 1, std::vector<double>(101, 7.0), out);
    gl_pack_indices(bits, GL_BITMAP, 8, 8, std::vector<double>(64, 1.0), out);
    EXPECT_EQ(out, std::vector<u8>(16, 0xAB));
}

// The sub-rectangle glReadPixels packs (the part of the rectangle inside the
// window) lands where the full image's groups lie, BITMAP bits included.
TEST(GlPixelsTest, RegionLayoutPlacesGroupsAsTheFullImage) {
    const GlPixelStoreModes m{false, false, 7, 2, 3, 4};
    GlPixelStoreModes r{};
    usize off = 0;
    ASSERT_TRUE(gl_pixel_region_layout(m, GL_RGB, GL_UNSIGNED_BYTE, 5, 4, 2, 1, r, off));
    const GlPixelLayout full = gl_pixel_layout(m, GL_RGB, GL_UNSIGNED_BYTE, 5, 4);
    EXPECT_EQ(off, full.first_byte + full.row_bytes + 2u * 3u);
    EXPECT_EQ(gl_pixel_layout(r, GL_RGB, GL_UNSIGNED_BYTE, 3, 3).row_bytes, full.row_bytes);
    ASSERT_TRUE(gl_pixel_region_layout(m, GL_STENCIL_INDEX, GL_BITMAP, 12, 4, 6, 0, r, off));
    const GlPixelLayout bfull = gl_pixel_layout(m, GL_STENCIL_INDEX, GL_BITMAP, 12, 4);
    EXPECT_EQ(off, bfull.first_byte + 1u);   // SKIP_PIXELS 3 + 6 = bit 9: byte 1, bit 1
    EXPECT_EQ(r.skip_pixels, 1);
    EXPECT_FALSE(gl_pixel_region_layout(m, GL_RGB, GL_UNSIGNED_BYTE, 5, 4, 5, 0, r, off));
}

INSTANTIATE_TEST_SUITE_P(Bitness, GlPixelsExportTest, ::testing::Values(false, true), bitness_name);

}  // namespace
