/// @file test_gl_context_state.cpp
/// @brief OpenGL 1.1 context state: initial values, errors, queries, attribute
///        stacks and the export marshalling, in both bitnesses.
///
/// Expected values come from the OpenGL 1.1 specification (cited by section or
/// table) or from Microsoft's GDI Generic GL 1.1 measured on Windows 11
/// (gl11_state_probe{32,64}.expected and wgl_contract_probe{32,64}.expected;
/// the two bitnesses gave identical gl11 lines). Measured values are embedded
/// with their probe tag.

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
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
#include "winapi/graphics/opengl32/opengl32.hpp"
#include "winapi/kernel32/error.hpp"

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::gl;

namespace {

// ---- Probe tables -----------------------------------------------------------

struct ProbeGet {
    const char* tag;
    u32 pname;
    char type;   // B / I / F / D: the getter the probe used
    std::vector<double> values;
};
struct ProbeEnabled {
    const char* tag;
    u32 cap;
    int value;
};
struct ProbePointer {
    const char* tag;
    u32 pname;
};
struct ProbeIndexed {
    const char* tag;
    u32 target;
    i32 level;
    u32 pname;
    std::vector<double> values;
};

// The measured initial-state lines, with their probe tags.
#include "winapi/graphics/gl11_state_probe_initial.inc"

/// The context of gl11_state_probe64.expected's setup line: pixel format 3
/// (double-buffered RGBA, 8/8/8/0 colour, 32 depth, 8 stencil, 16/16/16/0
/// accumulation; get-GL_*_BITS), first made current on a 320x200 client.
GlFramebufferConfig oracle_format3() {
    GlFramebufferConfig fb;
    fb.red_bits = fb.green_bits = fb.blue_bits = 8;
    fb.alpha_bits = 0;
    fb.index_bits = 8;          // get-GL_INDEX_BITS 8
    fb.depth_bits = 32;         // get-GL_DEPTH_BITS 32
    fb.stencil_bits = 8;        // get-GL_STENCIL_BITS 8
    fb.accum_red_bits = fb.accum_green_bits = fb.accum_blue_bits = 16;
    fb.accum_alpha_bits = 0;
    fb.double_buffer = true;    // get-GL_DOUBLEBUFFER 1
    return fb;
}

u32 fbits(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}
float bitsf(u32 b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
u64 dbits(double d) {
    u64 b;
    std::memcpy(&b, &d, 8);
    return b;
}
double bitsd(u64 b) {
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}

bool near(double actual, double expected) {
    return std::fabs(actual - expected) <= 1e-6 * std::max(1.0, std::fabs(expected));
}

/// A GL argument of a given kind.
struct I { u32 v; };
struct F { float v; };
struct D { double v; };
struct P { GuestAddr v; };
using Arg = std::variant<I, F, D, P>;

// ---- Fixture: the real exports, called through a guest CPU frame -------------

class GlExportHarness {
public:
    static constexpr GuestAddr MEM = 0x10000;
    static constexpr GuestAddr STACK = 0x80000;
    static constexpr GuestAddr OUT = MEM + 0x1000;   // query output
    static constexpr GuestAddr IN = MEM + 0x3000;    // array input

    explicit GlExportHarness(bool x64) : m_x64(x64) {
        EXPECT_TRUE(memory.map_region(MEM, 0x10000, MemoryProtection::ReadWrite).is_ok());
        EXPECT_TRUE(memory.map_region(STACK, 0x2000, MemoryProtection::ReadWrite).is_ok());
        EXPECT_TRUE(memory.map_region(OPENGL32_SCRATCH_BASE, OPENGL32_SCRATCH_SIZE,
                                      MemoryProtection::ReadWrite).is_ok());
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        registry = std::make_unique<ThunkRegistry>(0x70000000);
        mgr.set_thunk_registry(registry.get());
        EXPECT_TRUE(registry->register_dll("opengl32.dll", create_opengl32_thunks(mgr)).is_ok());
    }

    /// Calls export @p name with @p args laid out per the bitness's ABI;
    /// returns RAX (EAX on x86).
    u64 call(const char* name, std::vector<Arg> args) {
        auto addr = registry->get_thunk_address("opengl32.dll", name);
        EXPECT_TRUE(addr.is_ok()) << name;
        if (!addr.is_ok()) return ~0ull;
        auto handler = registry->resolve_handler(addr.value());
        EXPECT_TRUE(handler.is_ok()) << name;
        if (!handler.is_ok()) return ~0ull;
        const GuestAddr frame = STACK + 0x1000;
        cpu.set_sp(frame);
        // Undefined register/slot contents must not leak into arguments.
        cpu.gpr.rax = 0xCCCCCCCCCCCCCCCCull;
        cpu.gpr.rcx = cpu.gpr.rdx = cpu.gpr.r8 = cpu.gpr.r9 = 0xDDDDDDDDDDDDDDDDull;
        for (auto& x : cpu.sse.xmm) x.u64_[0] = x.u64_[1] = 0xEEEEEEEEEEEEEEEEull;
        if (m_x64) {
            (void)memory.write_u64(frame, 0xDEADBEEF);
            for (usize i = 0; i < args.size(); ++i) {
                u64 bits = 0;
                bool fp = false;
                if (auto* a = std::get_if<I>(&args[i])) bits = 0xABCD000000000000ull | a->v;  // high dword garbage
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

    std::vector<i32> geti(u32 pname, u32 n) {
        fill(0x5A);
        call("glGetIntegerv", {I{pname}, P{OUT}});
        std::vector<i32> out(n);
        for (u32 k = 0; k < n; ++k) out[k] = static_cast<i32>(memory.read_u32(OUT + 4 * k).value());
        return out;
    }
    std::vector<float> getf(u32 pname, u32 n) {
        fill(0x5A);
        call("glGetFloatv", {I{pname}, P{OUT}});
        std::vector<float> out(n);
        for (u32 k = 0; k < n; ++k) out[k] = bitsf(memory.read_u32(OUT + 4 * k).value());
        return out;
    }
    std::vector<double> getd(u32 pname, u32 n) {
        fill(0x5A);
        call("glGetDoublev", {I{pname}, P{OUT}});
        std::vector<double> out(n);
        for (u32 k = 0; k < n; ++k) out[k] = bitsd(memory.read_u64(OUT + 8 * k).value());
        return out;
    }
    std::vector<u8> getb(u32 pname, u32 n) {
        fill(0x5A);
        call("glGetBooleanv", {I{pname}, P{OUT}});
        std::vector<u8> out(n);
        for (u32 k = 0; k < n; ++k) out[k] = memory.read_u8(OUT + k).value();
        return out;
    }
    /// Reads @p n floats a getter wrote at OUT.
    std::vector<double> out_floats(u32 n) {
        std::vector<double> out(n);
        for (u32 k = 0; k < n; ++k) out[k] = bitsf(memory.read_u32(OUT + 4 * k).value());
        return out;
    }
    void fill(u8 byte) {
        std::vector<u8> pattern(0x400, byte);
        (void)memory.write_bytes(OUT, pattern.data(), pattern.size());
    }
    void put_floats(std::initializer_list<float> v, GuestAddr at = IN) {
        u32 k = 0;
        for (float x : v) (void)memory.write_u32(at + 4 * k++, fbits(x));
    }
    void put_ints(std::initializer_list<i32> v, GuestAddr at = IN) {
        u32 k = 0;
        for (i32 x : v) (void)memory.write_u32(at + 4 * k++, static_cast<u32>(x));
    }
    void put_doubles(std::initializer_list<double> v, GuestAddr at = IN) {
        u32 k = 0;
        for (double x : v) (void)memory.write_u64(at + 8 * k++, dbits(x));
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    Opengl32Manager mgr;
    std::unique_ptr<ThunkRegistry> registry;

private:
    bool m_x64;
};

/// A context that is current on this thread for the test's lifetime.
struct CurrentContext {
    explicit CurrentContext(const GlFramebufferConfig& fb = oracle_format3()) : ctx(fb) {
        ctx.on_first_make_current(320, 200);   // setup: client=320x200
        set_current_gl_context_for_thread(&ctx);
    }
    ~CurrentContext() { set_current_gl_context_for_thread(nullptr); }
    GlContext ctx;
};

class GlContextStateTest : public ::testing::TestWithParam<bool> {
protected:
    GlExportHarness gl{GetParam()};
    CurrentContext current;
    GlContext& ctx = current.ctx;
};

std::string bitness_name(const ::testing::TestParamInfo<bool>& info) { return info.param ? "x64" : "x86"; }

// ---- InitialStateMatchesSpecAndOracle ----------------------------------------

TEST_P(GlContextStateTest, InitialStateMatchesSpecAndOracle) {
    // Every get-* line, read with the getter the probe used.
    for (const ProbeGet& row : kProbeGets) {
        const u32 n = static_cast<u32>(row.values.size());
        switch (row.type) {
        case 'B': {
            const auto v = gl.getb(row.pname, n);
            for (u32 k = 0; k < n; ++k) EXPECT_EQ(v[k], row.values[k]) << row.tag << "[" << k << "]";
            break;
        }
        case 'I': {
            const auto v = gl.geti(row.pname, n);
            for (u32 k = 0; k < n; ++k) EXPECT_EQ(v[k], row.values[k]) << row.tag << "[" << k << "]";
            break;
        }
        case 'F': {
            const auto v = gl.getf(row.pname, n);
            for (u32 k = 0; k < n; ++k) EXPECT_TRUE(near(v[k], row.values[k])) << row.tag << "[" << k << "] " << v[k];
            break;
        }
        default: {
            const auto v = gl.getd(row.pname, n);
            for (u32 k = 0; k < n; ++k) EXPECT_TRUE(near(v[k], row.values[k])) << row.tag << "[" << k << "] " << v[k];
            break;
        }
        }
        // The getter writes exactly n elements (spec_n == n for all 238 pnames).
        EXPECT_EQ(gl.err(), 0u) << row.tag;
    }
    for (const ProbeEnabled& row : kProbeEnabled) {
        EXPECT_EQ(gl.call("glIsEnabled", {I{row.cap}}), static_cast<u64>(row.value)) << row.tag;
    }
    for (const ProbePointer& row : kProbePointers) {
        gl.fill(0x5A);
        gl.call("glGetPointerv", {I{row.pname}, P{GlExportHarness::OUT}});
        EXPECT_EQ(GetParam() ? gl.memory.read_u64(GlExportHarness::OUT).value()
                             : gl.memory.read_u32(GlExportHarness::OUT).value(), 0u) << row.tag;
    }
    auto check_indexed = [&](const char* getter, const ProbeIndexed& row, bool three_args) {
        gl.fill(0x5A);
        if (three_args) gl.call(getter, {I{row.target}, I{row.pname}, P{GlExportHarness::OUT}});
        else gl.call(getter, {I{row.target}, I{static_cast<u32>(row.level)}, I{row.pname}, P{GlExportHarness::OUT}});
        const auto v = gl.out_floats(static_cast<u32>(row.values.size()));
        for (usize k = 0; k < v.size(); ++k) EXPECT_TRUE(near(v[k], row.values[k])) << row.tag << "[" << k << "]";
        EXPECT_EQ(gl.err(), 0u) << row.tag;
    };
    for (const auto& row : kProbeLights) check_indexed("glGetLightfv", row, true);
    for (const auto& row : kProbeMaterials) check_indexed("glGetMaterialfv", row, true);
    for (const auto& row : kProbeTexEnv) check_indexed("glGetTexEnvfv", row, true);
    for (const auto& row : kProbeTexGen) check_indexed("glGetTexGenfv", row, true);
    for (const auto& row : kProbeTexParams) check_indexed("glGetTexParameterfv", row, true);
    for (const auto& row : kProbeTexLevels) check_indexed("glGetTexLevelParameterfv", row, false);
    for (const auto& row : kProbeMaps) {
        gl.fill(0x5A);
        gl.call("glGetMapdv", {I{row.target}, I{row.pname}, P{GlExportHarness::OUT}});
        for (usize k = 0; k < row.values.size(); ++k) {
            EXPECT_EQ(bitsd(gl.memory.read_u64(GlExportHarness::OUT + 8 * k).value()), row.values[k]) << row.tag;
        }
    }
    for (const auto& row : kProbePixelMaps) {
        gl.fill(0x5A);
        gl.call("glGetPixelMapfv", {I{row.target}, P{GlExportHarness::OUT}});
        EXPECT_EQ(gl.out_floats(1)[0], row.values[0]) << row.tag;
    }
    // clipplane0..5 n=4 values=0 0 0 0
    for (u32 i = 0; i < 6; ++i) {
        gl.fill(0x5A);
        gl.call("glGetClipPlane", {I{GL_CLIP_PLANE0 + i}, P{GlExportHarness::OUT}});
        for (u32 k = 0; k < 4; ++k) EXPECT_EQ(bitsd(gl.memory.read_u64(GlExportHarness::OUT + 8 * k).value()), 0.0);
    }
    // polygonstipple n=128 bytes=ff...ff
    gl.fill(0x00);
    gl.call("glGetPolygonStipple", {P{GlExportHarness::OUT}});
    for (u32 k = 0; k < 128; ++k) EXPECT_EQ(gl.memory.read_u8(GlExportHarness::OUT + k).value(), 0xFF) << k;
    EXPECT_EQ(gl.memory.read_u8(GlExportHarness::OUT + 128).value(), 0x00);   // nothing past the 32x32 bitmap
    // light8-GL_AMBIENT-out-of-range err=0x0500
    gl.call("glGetLightfv", {I{GL_LIGHT0 + 8}, I{GL_AMBIENT}, P{GlExportHarness::OUT}});
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);

    // get-invalid-*: every getter records INVALID_ENUM and writes nothing.
    for (u32 pname : {0x1234u, 0x0000u, 0xFFFFFFFFu, 0x8073u, 0x846Eu, 0x806Au, 0x84E2u, 0x8872u, 0x0DE2u}) {
        for (const char* getter : {"glGetIntegerv", "glGetFloatv", "glGetDoublev", "glGetBooleanv"}) {
            gl.fill(0x5A);
            gl.call(getter, {I{pname}, P{GlExportHarness::OUT}});
            EXPECT_EQ(gl.err(), GL_INVALID_ENUM) << getter << " " << pname;
            EXPECT_EQ(gl.memory.read_u32(GlExportHarness::OUT).value(), 0x5A5A5A5Au) << getter << " " << pname;
        }
        EXPECT_EQ(gl.call("glIsEnabled", {I{pname}}), 0u);
        EXPECT_EQ(gl.err(), GL_INVALID_ENUM) << pname;
    }
    // get-invalid-gap-0x0C30 (GL_INDEX_MODE): a glGet pname, not an IsEnabled cap.
    EXPECT_EQ(gl.geti(GL_INDEX_MODE, 1)[0], 0);
    EXPECT_EQ(gl.err(), 0u);
    gl.call("glIsEnabled", {I{GL_INDEX_MODE}});
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);
    // getpointer-invalid-0x1234 value=untouched err=0x0500
    gl.fill(0x5A);
    gl.call("glGetPointerv", {I{0x1234}, P{GlExportHarness::OUT}});
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);
    EXPECT_EQ(gl.memory.read_u32(GlExportHarness::OUT).value(), 0x5A5A5A5Au);

    // glGetString: Design 10's strings, stable pointers, NULL + INVALID_ENUM
    // for other names (glgetstring-invalid-0x1234, -zero, -0x1F04). The
    // extensions are GDI Generic's (glgetstring-GL_EXTENSIONS), implemented.
    const std::pair<u32, const char*> kStrings[] = {
        {GL_VENDOR, "Vitreo"}, {GL_RENDERER, "Vitreo Metal"}, {GL_VERSION, "1.1.0"},
        {GL_EXTENSIONS, "GL_WIN_swap_hint GL_EXT_bgra GL_EXT_paletted_texture"}};
    for (const auto& [name, expected] : kStrings) {
        const u64 p = gl.call("glGetString", {I{name}});
        ASSERT_NE(p, 0u) << name;
        std::vector<u8> bytes(std::strlen(expected) + 1);
        ASSERT_TRUE(gl.memory.read_bytes(static_cast<GuestAddr>(p), bytes.data(), bytes.size()).is_ok());
        EXPECT_STREQ(reinterpret_cast<const char*>(bytes.data()), expected);
        EXPECT_EQ(gl.call("glGetString", {I{name}}), p);   // glgetstring-GL_VERSION-stable same-pointer=1
    }
    for (u32 bad : {0x1234u, 0u, 0x1F04u}) {
        EXPECT_EQ(gl.call("glGetString", {I{bad}}), 0u);
        EXPECT_EQ(gl.err(), GL_INVALID_ENUM);
    }
}

// Ledger row 10 (Task 3): a float in an x64 stack slot (argument 4 and up)
// read back. glMapGrid2f(un, u1, u2, vn, v1, v2): v1 and v2 are arguments 4
// and 5, so on x64 they come from the stack, not XMM4/5 (spec 5.1: the grid
// domains and segment counts are queryable as given).
TEST_P(GlContextStateTest, MapGrid2fReadsFloatsFromX64StackSlots) {
    gl.call("glMapGrid2f", {I{10}, F{0.25f}, F{0.75f}, I{20}, F{0.125f}, F{0.875f}});
    EXPECT_EQ(gl.err(), 0u);
    EXPECT_EQ(gl.getf(GL_MAP2_GRID_DOMAIN, 4), (std::vector<float>{0.25f, 0.75f, 0.125f, 0.875f}));
    EXPECT_EQ(gl.getf(GL_MAP2_GRID_SEGMENTS, 2), (std::vector<float>{10, 20}));
}

// ---- ErrorFlagIsStickyUntilRead -----------------------------------------------

TEST_P(GlContextStateTest, ErrorFlagIsStickyUntilRead) {
    // err-none-baseline err=0 then=0
    EXPECT_EQ(gl.err(), 0u);
    EXPECT_EQ(gl.err(), 0u);
    // err-first-error-wins / err-two-errors-sequence: INVALID_VALUE then
    // INVALID_ENUM reads 0x0501, 0, 0.
    gl.call("glLineWidth", {F{0.0f}});
    gl.call("glEnable", {I{0x1234}});
    EXPECT_EQ(gl.err(), GL_INVALID_VALUE);
    EXPECT_EQ(gl.err(), 0u);
    EXPECT_EQ(gl.err(), 0u);
    // A failing call changes no state (spec 2.5).
    gl.call("glLineWidth", {F{-2.0f}});
    EXPECT_EQ(gl.getf(GL_LINE_WIDTH, 1)[0], 1.0f);
    EXPECT_EQ(gl.err(), GL_INVALID_VALUE);
    // err-geterror-in-glBegin: in-begin=0x0502 in-begin-again=0x0502
    // after-glEnd=0x0502 then=0.
    ctx.begin_mode = GL_TRIANGLES;   // glBegin is Task 7
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);
    ctx.begin_mode = kGlOutsideBeginEnd;
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);
    EXPECT_EQ(gl.err(), 0u);
    // err-geterror-in-glBegin-after-bad-call: in-begin=0x0502 after-glEnd=0x0502.
    ctx.begin_mode = GL_TRIANGLES;
    gl.call("glEnable", {I{GL_BLEND}});           // err-glEnable-inside-glBegin
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);
    ctx.begin_mode = kGlOutsideBeginEnd;
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);
    EXPECT_EQ(gl.err(), 0u);
    EXPECT_EQ(gl.call("glIsEnabled", {I{GL_BLEND}}), 0u);   // no other effect
}

TEST_P(GlContextStateTest, CommandsOutsideTheBeginEndSubsetFailInsideBegin) {
    // Spec 2.6.3; measured: err-glEnable/-glMatrixMode/-glViewport/
    // -glGetIntegerv/-glPushMatrix-inside-glBegin = INVALID_OPERATION,
    // err-glColor-inside-glBegin = no error.
    const std::vector<std::pair<const char*, std::vector<Arg>>> rejected = {
        {"glEnable", {I{GL_FOG}}}, {"glMatrixMode", {I{GL_PROJECTION}}},
        {"glViewport", {I{0}, I{0}, I{1}, I{1}}}, {"glGetIntegerv", {I{GL_VIEWPORT}, P{GlExportHarness::OUT}}},
        {"glPushMatrix", {}}, {"glPushAttrib", {I{GL_ALL_ATTRIB_BITS}}}, {"glLineWidth", {F{2}}},
        {"glFogf", {I{GL_FOG_DENSITY}, F{2}}}, {"glHint", {I{GL_FOG_HINT}, I{GL_NICEST}}},
        {"glPixelStorei", {I{GL_UNPACK_ALIGNMENT}, I{1}}}, {"glEnableClientState", {I{GL_VERTEX_ARRAY}}},
        {"glGetString", {I{GL_VENDOR}}}, {"glTexEnvi", {I{GL_TEXTURE_ENV}, I{GL_TEXTURE_ENV_MODE}, I{GL_DECAL}}},
    };
    for (const auto& [name, args] : rejected) {
        ctx.begin_mode = GL_QUADS;
        gl.call(name, args);
        ctx.begin_mode = kGlOutsideBeginEnd;
        EXPECT_EQ(gl.err(), GL_INVALID_OPERATION) << name;
    }
    EXPECT_EQ(gl.call("glIsEnabled", {I{GL_FOG}}), 0u);
    EXPECT_EQ(gl.geti(GL_MATRIX_MODE, 1)[0], static_cast<i32>(GL_MODELVIEW));
    EXPECT_EQ(gl.geti(GL_ATTRIB_STACK_DEPTH, 1)[0], 0);
    EXPECT_EQ(gl.geti(GL_UNPACK_ALIGNMENT, 1)[0], 4);
    // Allowed: Color, Normal, TexCoord, Index, EdgeFlag, Material.
    ctx.begin_mode = GL_QUADS;
    gl.call("glColor3f", {F{0.5f}, F{0.25f}, F{1}});
    gl.call("glNormal3f", {F{1}, F{0}, F{0}});
    gl.call("glTexCoord2f", {F{0.5f}, F{0.5f}});
    gl.call("glIndexi", {I{3}});
    gl.call("glEdgeFlag", {I{0}});
    gl.call("glMaterialf", {I{GL_FRONT}, I{GL_SHININESS}, F{10}});
    ctx.begin_mode = kGlOutsideBeginEnd;
    EXPECT_EQ(gl.err(), 0u);
    EXPECT_EQ(gl.getf(GL_CURRENT_COLOR, 4), (std::vector<float>{0.5f, 0.25f, 1, 1}));
    EXPECT_EQ(gl.getb(GL_EDGE_FLAG, 1)[0], 0);
}

// ---- InvalidEnumsRecordInvalidEnum --------------------------------------------

TEST_P(GlContextStateTest, InvalidEnumsRecordInvalidEnum) {
    gl.put_floats({1, 1, 1, 1});
    const P in{GlExportHarness::IN};
    const P out{GlExportHarness::OUT};
    const I bad{0x1234};
    // One row per enum parameter of every state-setting or state-query command.
    const std::vector<std::pair<const char*, std::vector<Arg>>> cases = {
        {"glEnable", {bad}},                                    // err-bad-enum-glEnable
        {"glDisable", {bad}},                                   // err-bad-enum-glDisable
        {"glIsEnabled", {bad}},
        {"glGetIntegerv", {bad, out}},                          // err-bad-enum-glGetIntegerv
        {"glMatrixMode", {bad}},                                // err-bad-enum-glMatrixMode
        {"glBlendFunc", {bad, I{GL_ZERO}}},                     // err-bad-enum-glBlendFunc
        {"glBlendFunc", {I{GL_ONE}, I{GL_DST_COLOR}}},          // DST_COLOR is not a destination factor
        {"glBlendFunc", {I{GL_SRC_COLOR}, I{GL_ZERO}}},         // SRC_COLOR is not a source factor
        {"glDepthFunc", {bad}},                                 // err-bad-enum-glDepthFunc
        {"glAlphaFunc", {bad, F{0}}},                           // err-bad-enum-glAlphaFunc
        {"glCullFace", {bad}},                                  // err-bad-enum-glCullFace
        {"glFrontFace", {bad}},                                 // err-bad-enum-glFrontFace
        {"glShadeModel", {bad}},                                // err-bad-enum-glShadeModel
        {"glPolygonMode", {bad, I{GL_LINE}}},                   // err-bad-enum-glPolygonMode
        {"glPolygonMode", {I{GL_FRONT}, bad}},
        {"glHint", {bad, I{GL_NICEST}}},                        // err-bad-enum-glHint
        {"glHint", {I{GL_FOG_HINT}, bad}},
        {"glFogi", {bad, I{0}}},                                // err-bad-enum-glFogi
        {"glFogi", {I{GL_FOG_MODE}, bad}},                      // err-bad-param-glFogi-mode
        {"glFogf", {I{GL_FOG_COLOR}, F{1}}},                    // a vector pname through the scalar form
        {"glFogfv", {bad, in}},
        {"glTexEnvi", {bad, I{GL_TEXTURE_ENV_MODE}, I{GL_DECAL}}},  // err-bad-enum-glTexEnvi
        {"glTexEnvi", {I{GL_TEXTURE_ENV}, bad, I{GL_DECAL}}},
        {"glTexEnvi", {I{GL_TEXTURE_ENV}, I{GL_TEXTURE_ENV_MODE}, bad}},
        {"glTexEnvf", {I{GL_TEXTURE_ENV}, I{GL_TEXTURE_ENV_COLOR}, F{1}}},
        {"glTexParameteri", {bad, I{GL_TEXTURE_MIN_FILTER}, I{GL_LINEAR}}},  // err-bad-enum-glTexParameteri
        {"glTexParameteri", {I{GL_TEXTURE_2D}, bad, I{GL_LINEAR}}},
        {"glTexParameteri", {I{GL_TEXTURE_2D}, I{GL_TEXTURE_MIN_FILTER}, bad}},
        {"glTexParameteri", {I{GL_TEXTURE_2D}, I{GL_TEXTURE_MAG_FILTER}, I{GL_LINEAR_MIPMAP_LINEAR}}},
        {"glTexParameteri", {I{GL_TEXTURE_2D}, I{GL_TEXTURE_WRAP_S}, I{0x812F}}},  // CLAMP_TO_EDGE is GL 1.2
        {"glTexParameteri", {I{GL_TEXTURE_2D}, I{GL_TEXTURE_RESIDENT}, I{1}}},     // read-only
        {"glTexParameteri", {I{GL_PROXY_TEXTURE_2D}, I{GL_TEXTURE_MIN_FILTER}, I{GL_LINEAR}}},
        {"glReadBuffer", {bad}},                                // err-bad-enum-glReadBuffer
        {"glReadBuffer", {I{GL_NONE}}},
        {"glReadBuffer", {I{GL_FRONT_AND_BACK}}},
        {"glDrawBuffer", {bad}},                                // err-bad-enum-glDrawBuffer
        {"glLightfv", {I{GL_LIGHT0 + 8}, I{GL_AMBIENT}, in}},   // err-bad-enum-glLightfv-light
        {"glLightfv", {I{GL_LIGHT0}, bad, in}},                 // err-bad-enum-glLightfv-pname
        {"glLightf", {I{GL_LIGHT0}, I{GL_POSITION}, F{1}}},
        {"glLightModelf", {bad, F{1}}},
        {"glLightModelf", {I{GL_LIGHT_MODEL_AMBIENT}, F{1}}},
        {"glMaterialf", {bad, I{GL_SHININESS}, F{1}}},
        {"glMaterialf", {I{GL_FRONT}, bad, F{1}}},
        {"glMaterialf", {I{GL_FRONT}, I{GL_DIFFUSE}, F{1}}},
        {"glColorMaterial", {bad, I{GL_AMBIENT}}},              // err-glColorMaterial-bad-enum
        {"glColorMaterial", {I{GL_FRONT}, I{GL_SHININESS}}},
        {"glLogicOp", {bad}},                                   // err-glLogicOp-bad-enum
        {"glStencilFunc", {bad, I{0}, I{0xFF}}},                // err-glStencilFunc-bad-enum
        {"glStencilOp", {bad, I{GL_KEEP}, I{GL_KEEP}}},         // err-glStencilOp-bad-enum
        {"glStencilOp", {I{GL_KEEP}, bad, I{GL_KEEP}}},
        {"glStencilOp", {I{GL_KEEP}, I{GL_KEEP}, bad}},
        {"glClipPlane", {I{GL_CLIP_PLANE0 + 6}, in}},           // err-bad-plane-glClipPlane-index6
        {"glGetClipPlane", {I{GL_CLIP_PLANE0 + 6}, out}},
        {"glPixelStorei", {bad, I{1}}},
        {"glPixelTransferf", {bad, F{1}}},                      // err-glPixelTransferf-bad-enum
        {"glPixelMapfv", {bad, I{1}, in}},
        {"glGetPixelMapfv", {bad, out}},
        {"glTexGeni", {bad, I{GL_TEXTURE_GEN_MODE}, I{GL_EYE_LINEAR}}},
        {"glTexGeni", {I{GL_S}, bad, I{GL_EYE_LINEAR}}},
        {"glTexGeni", {I{GL_S}, I{GL_TEXTURE_GEN_MODE}, bad}},
        {"glTexGeni", {I{GL_R}, I{GL_TEXTURE_GEN_MODE}, I{GL_SPHERE_MAP}}},  // spec 2.10.4
        {"glTexGenf", {I{GL_S}, I{GL_EYE_PLANE}, F{1}}},
        {"glVertexPointer", {I{3}, bad, I{0}, in}},             // err-glVertexPointer-bad-type
        {"glVertexPointer", {I{3}, I{GL_UNSIGNED_BYTE}, I{0}, in}},
        {"glNormalPointer", {I{GL_UNSIGNED_BYTE}, I{0}, in}},   // err-glNormalPointer-bad-type
        {"glColorPointer", {I{4}, bad, I{0}, in}},
        {"glIndexPointer", {I{GL_BYTE}, I{0}, in}},
        {"glTexCoordPointer", {I{2}, I{GL_BYTE}, I{0}, in}},
        {"glInterleavedArrays", {bad, I{0}, in}},               // err-glInterleavedArrays-bad-format
        {"glGetLightfv", {I{GL_LIGHT0}, bad, out}},
        {"glGetMaterialfv", {I{GL_FRONT_AND_BACK}, I{GL_AMBIENT}, out}},  // spec 6.1.3: FRONT or BACK
        {"glGetMaterialfv", {I{GL_FRONT}, I{GL_AMBIENT_AND_DIFFUSE}, out}},
        {"glGetTexEnvfv", {bad, I{GL_TEXTURE_ENV_MODE}, out}},
        {"glGetTexEnvfv", {I{GL_TEXTURE_ENV}, bad, out}},
        {"glGetTexGenfv", {bad, I{GL_TEXTURE_GEN_MODE}, out}},
        {"glGetTexGenfv", {I{GL_S}, bad, out}},
        {"glGetTexParameterfv", {bad, I{GL_TEXTURE_MIN_FILTER}, out}},
        {"glGetTexParameterfv", {I{GL_TEXTURE_2D}, bad, out}},
        {"glGetTexLevelParameterfv", {bad, I{0}, I{GL_TEXTURE_WIDTH}, out}},
        {"glGetTexLevelParameterfv", {I{GL_TEXTURE_2D}, I{0}, bad, out}},
        {"glGetMapfv", {bad, I{GL_ORDER}, out}},
        {"glGetMapfv", {I{GL_MAP1_VERTEX_3}, bad, out}},
        {"glGetPointerv", {bad, out}},
        {"glGetString", {bad}},
        {"glRenderMode", {bad}},                                // err-glRenderMode-bad-enum
        {"glFeedbackBuffer", {I{4}, bad, in}},
        {"glMap1f", {bad, F{0}, F{1}, I{3}, I{1}, in}},
        {"glMap2f", {bad, F{0}, F{1}, I{3}, I{1}, F{0}, F{1}, I{3}, I{1}, in}},
        {"glEvalMesh1", {bad, I{0}, I{1}}},                     // err-glEvalMesh1-bad-mode
        {"glAccum", {bad, F{1}}},                               // err-glAccum-bad-op
    };
    for (const auto& [name, args] : cases) {
        gl.call(name, args);
        EXPECT_EQ(gl.err(), GL_INVALID_ENUM) << name;
    }
    // Measured silent cases: err-bad-cap-glEnableClientState and
    // err-glEvalMesh2-bad-mode record nothing.
    gl.call("glEnableClientState", {bad});
    EXPECT_EQ(gl.err(), 0u);
    gl.call("glDisableClientState", {bad});
    EXPECT_EQ(gl.err(), 0u);
    gl.call("glEvalMesh2", {bad, I{0}, I{1}, I{0}, I{1}});
    EXPECT_EQ(gl.err(), 0u);
    // None of the rejected calls changed state.
    EXPECT_EQ(gl.geti(GL_BLEND_SRC, 1)[0], static_cast<i32>(GL_ONE));
    EXPECT_EQ(gl.geti(GL_BLEND_DST, 1)[0], static_cast<i32>(GL_ZERO));
    EXPECT_EQ(gl.geti(GL_FOG_MODE, 1)[0], static_cast<i32>(GL_EXP));
    EXPECT_EQ(gl.geti(GL_VERTEX_ARRAY_TYPE, 1)[0], static_cast<i32>(GL_FLOAT));
}

TEST_P(GlContextStateTest, InvalidValuesRecordInvalidValue) {
    gl.put_floats({1, 1, 1, 1});
    const P in{GlExportHarness::IN};
    const std::vector<std::pair<const char*, std::vector<Arg>>> cases = {
        {"glViewport", {I{0}, I{0}, I{static_cast<u32>(-1)}, I{1}}},      // err-neg-size-glViewport
        {"glScissor", {I{0}, I{0}, I{1}, I{static_cast<u32>(-1)}}},       // err-neg-size-glScissor
        {"glLineWidth", {F{-1}}}, {"glLineWidth", {F{0}}},                 // err-neg-size-/err-zero-glLineWidth
        {"glPointSize", {F{-1}}}, {"glPointSize", {F{0}}},                 // err-neg-size-/err-zero-glPointSize
        {"glPixelStorei", {I{GL_UNPACK_ALIGNMENT}, I{3}}},                 // err-bad-value-glPixelStorei-alignment3
        {"glPixelStorei", {I{GL_UNPACK_ROW_LENGTH}, I{static_cast<u32>(-1)}}},  // -neg-rowlength
        {"glPixelStorei", {I{GL_PACK_SKIP_PIXELS}, I{static_cast<u32>(-1)}}},
        {"glFogf", {I{GL_FOG_DENSITY}, F{-1}}},                            // err-bad-value-glFogf-neg-density
        {"glMapGrid1f", {I{0}, F{0}, F{1}}},                               // err-bad-value-glMapGrid1f-zero
        {"glMapGrid2f", {I{1}, F{0}, F{1}, I{static_cast<u32>(-1)}, F{0}, F{1}}},  // err-glMapGrid2f-neg
        {"glMap1f", {I{GL_MAP1_VERTEX_3}, F{0}, F{1}, I{3}, I{0}, in}},    // err-bad-value-glMap1f-order0
        {"glMap1f", {I{GL_MAP1_VERTEX_3}, F{1}, F{1}, I{3}, I{1}, in}},    // -domain-equal
        {"glMap1f", {I{GL_MAP1_VERTEX_3}, F{0}, F{1}, I{2}, I{1}, in}},    // stride < k
        {"glMap1f", {I{GL_MAP1_VERTEX_3}, F{0}, F{1}, I{3}, I{31}, in}},   // order > MAX_EVAL_ORDER
        {"glMap2f", {I{GL_MAP2_VERTEX_3}, F{0}, F{1}, I{3}, I{0}, F{0}, F{1}, I{3}, I{1}, in}},  // err-glMap2f-bad-order
        {"glSelectBuffer", {I{static_cast<u32>(-1)}, in}},                 // err-bad-value-glSelectBuffer-neg
        {"glFeedbackBuffer", {I{static_cast<u32>(-1)}, I{GL_2D}, in}},     // err-bad-value-glFeedbackBuffer-neg
        {"glLightf", {I{GL_LIGHT0}, I{GL_SPOT_CUTOFF}, F{91}}},            // err-bad-value-glLightf-spotcutoff
        {"glLightf", {I{GL_LIGHT0}, I{GL_SPOT_EXPONENT}, F{129}}},         // err-bad-value-glLightf-spotexponent
        {"glLightf", {I{GL_LIGHT0}, I{GL_LINEAR_ATTENUATION}, F{-1}}},
        {"glMaterialf", {I{GL_FRONT}, I{GL_SHININESS}, F{129}}},           // err-bad-value-glMaterialf-shininess
        {"glPixelMapfv", {I{GL_PIXEL_MAP_R_TO_R}, I{0}, in}},              // err-glPixelMapfv-bad-size
        {"glPixelMapfv", {I{GL_PIXEL_MAP_I_TO_I}, I{3}, in}},              // err-glPixelMapfv-nonpow2-itoi
        {"glPixelMapfv", {I{GL_PIXEL_MAP_R_TO_R}, I{65537}, in}},          // > MAX_PIXEL_MAP_TABLE
        {"glGetTexLevelParameterfv", {I{GL_TEXTURE_2D}, I{static_cast<u32>(-1)}, I{GL_TEXTURE_WIDTH}, P{GlExportHarness::OUT}}},
        {"glGetTexLevelParameterfv", {I{GL_TEXTURE_2D}, I{11}, I{GL_TEXTURE_WIDTH}, P{GlExportHarness::OUT}}},
        {"glVertexPointer", {I{1}, I{GL_FLOAT}, I{0}, in}},                // err-glVertexPointer-bad-size
        {"glVertexPointer", {I{3}, I{GL_FLOAT}, I{static_cast<u32>(-1)}, in}},  // -neg-stride
        {"glColorPointer", {I{2}, I{GL_FLOAT}, I{0}, in}},                 // err-glColorPointer-bad-size
        {"glTexCoordPointer", {I{5}, I{GL_FLOAT}, I{0}, in}},              // err-glTexCoordPointer-bad-size
        {"glEdgeFlagPointer", {I{static_cast<u32>(-1)}, in}},
        {"glInterleavedArrays", {I{GL_V3F}, I{static_cast<u32>(-1)}, in}},
        {"glFrustum", {D{-1}, D{1}, D{-1}, D{1}, D{0}, D{10}}},            // spec 2.10.2: n <= 0
        {"glFrustum", {D{-1}, D{1}, D{-1}, D{1}, D{1}, D{-1}}},
        {"glFrustum", {D{1}, D{1}, D{-1}, D{1}, D{1}, D{10}}},
        {"glFrustum", {D{-1}, D{1}, D{-1}, D{1}, D{2}, D{2}}},
        {"glOrtho", {D{1}, D{1}, D{0}, D{1}, D{0}, D{1}}},                 // l == r
        {"glOrtho", {D{0}, D{1}, D{2}, D{2}, D{0}, D{1}}},
        {"glOrtho", {D{0}, D{1}, D{0}, D{1}, D{3}, D{3}}},
    };
    for (const auto& [name, args] : cases) {
        gl.call(name, args);
        EXPECT_EQ(gl.err(), GL_INVALID_VALUE) << name;
    }
    EXPECT_EQ(gl.geti(GL_VIEWPORT, 4), (std::vector<i32>{0, 0, 320, 200}));
    EXPECT_EQ(gl.getd(GL_PROJECTION_MATRIX, 1)[0], 1.0);
    EXPECT_EQ(gl.geti(GL_PIXEL_MAP_R_TO_R_SIZE, 1)[0], 1);
    EXPECT_EQ(gl.geti(GL_UNPACK_ALIGNMENT, 1)[0], 4);
    // Measured accepted calls: err-glPolygonOffset-ok, err-glClearAccum-ok,
    // err-glIndexMask-ok, err-glPixelMapfv-ok-1, err-glLoadMatrixd-ok.
    gl.call("glPolygonOffset", {F{1}, F{1}});
    gl.call("glClearAccum", {F{0.5f}, F{0}, F{0}, F{0}});
    gl.call("glIndexMask", {I{0xFF}});
    gl.call("glPixelMapfv", {I{GL_PIXEL_MAP_I_TO_I}, I{1}, in});
    gl.put_doubles({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    gl.call("glLoadMatrixd", {in});
    EXPECT_EQ(gl.err(), 0u);
    // DrawBuffer naming a buffer the format lacks: INVALID_OPERATION (spec 4.2.1).
    for (u32 mode : {GL_AUX0, GL_AUX3, GL_RIGHT, GL_FRONT_RIGHT, GL_BACK_RIGHT}) {
        gl.call("glDrawBuffer", {I{mode}});
        EXPECT_EQ(gl.err(), GL_INVALID_OPERATION) << mode;
        gl.call("glReadBuffer", {I{mode}});
        EXPECT_EQ(gl.err(), GL_INVALID_OPERATION) << mode;
    }
    // glAccum: no accumulation buffer in a Vitreo format -> INVALID_OPERATION
    // (spec 4.2.4; Design 11).
    CurrentContext no_accum(GlFramebufferConfig{});
    gl.call("glAccum", {I{GL_LOAD}, F{1}});
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);
    // glRenderMode(GL_SELECT/GL_FEEDBACK) is Phase 2: 0 and INVALID_OPERATION.
    EXPECT_EQ(gl.call("glRenderMode", {I{GL_SELECT}}), 0u);
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);
    EXPECT_EQ(gl.geti(GL_RENDER_MODE, 1)[0], static_cast<i32>(GL_RENDER));
    // glRenderMode-render-to-render ret=0 err=0.
    EXPECT_EQ(gl.call("glRenderMode", {I{GL_RENDER}}), 0u);
    EXPECT_EQ(gl.err(), 0u);
}

// ---- MatrixStackDepthsAndErrors -------------------------------------------------

TEST_P(GlContextStateTest, MatrixStackDepthsAndErrors) {
    struct Stack {
        u32 mode, depth_pname, matrix_pname;
        i32 max_depth;   // get-GL_MAX_*_STACK_DEPTH (spec minimums 32 / 2 / 2)
    };
    const Stack stacks[] = {
        {GL_MODELVIEW, GL_MODELVIEW_STACK_DEPTH, GL_MODELVIEW_MATRIX, 32},
        {GL_PROJECTION, GL_PROJECTION_STACK_DEPTH, GL_PROJECTION_MATRIX, 10},
        {GL_TEXTURE, GL_TEXTURE_STACK_DEPTH, GL_TEXTURE_MATRIX, 10},
    };
    for (const Stack& s : stacks) {
        gl.call("glMatrixMode", {I{s.mode}});
        // err-underflow-glPopMatrix-*: popping the base entry underflows.
        gl.call("glPopMatrix", {});
        EXPECT_EQ(gl.err(), GL_STACK_UNDERFLOW) << s.mode;
        // err-overflow-glPushMatrix-*: max-1 pushes succeed, the next overflows.
        for (i32 k = 1; k < s.max_depth; ++k) {
            gl.call("glPushMatrix", {});
            gl.call("glTranslatef", {F{1}, F{0}, F{0}});
        }
        EXPECT_EQ(gl.err(), 0u) << s.mode;
        EXPECT_EQ(gl.geti(s.depth_pname, 1)[0], s.max_depth);
        gl.call("glPushMatrix", {});
        EXPECT_EQ(gl.err(), GL_STACK_OVERFLOW) << s.mode;
        EXPECT_EQ(gl.geti(s.depth_pname, 1)[0], s.max_depth);
        // Each pop restores the entry below.
        EXPECT_EQ(gl.getd(s.matrix_pname, 16)[12], s.max_depth - 1);
        gl.call("glPopMatrix", {});
        EXPECT_EQ(gl.getd(s.matrix_pname, 16)[12], s.max_depth - 2);
        for (i32 k = 2; k < s.max_depth; ++k) gl.call("glPopMatrix", {});
        EXPECT_EQ(gl.geti(s.depth_pname, 1)[0], 1);
        EXPECT_EQ(gl.getd(s.matrix_pname, 16)[12], 0.0);
        EXPECT_EQ(gl.err(), 0u);
        gl.call("glLoadIdentity", {});
    }
    // The commands multiply the current matrix on the right (spec 2.10.2).
    gl.call("glMatrixMode", {I{GL_PROJECTION}});
    gl.call("glOrtho", {D{0}, D{320}, D{0}, D{200}, D{-1}, D{1}});
    const auto ortho = gl.getd(GL_PROJECTION_MATRIX, 16);
    const double kOrtho[16] = {2.0 / 320, 0, 0, 0, 0, 2.0 / 200, 0, 0, 0, 0, -1, 0, -1, -1, 0, 1};
    for (u32 k = 0; k < 16; ++k) EXPECT_DOUBLE_EQ(ortho[k], kOrtho[k]) << k;
    gl.call("glLoadIdentity", {});
    gl.call("glFrustum", {D{-1}, D{1}, D{-1}, D{1}, D{1}, D{3}});
    const auto frustum = gl.getd(GL_PROJECTION_MATRIX, 16);
    const double kFrustum[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -2, -1, 0, 0, -3, 0};
    for (u32 k = 0; k < 16; ++k) EXPECT_DOUBLE_EQ(frustum[k], kFrustum[k]) << k;
    gl.call("glMatrixMode", {I{GL_MODELVIEW}});
    gl.call("glRotated", {D{90}, D{0}, D{0}, D{2}});   // axis normalised
    const auto rot = gl.getd(GL_MODELVIEW_MATRIX, 16);
    const double kRot[16] = {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (u32 k = 0; k < 16; ++k) EXPECT_NEAR(rot[k], kRot[k], 1e-12) << k;
    gl.call("glScalef", {F{2}, F{3}, F{4}});
    gl.call("glTranslated", {D{1}, D{1}, D{1}});
    const auto m = gl.getd(GL_MODELVIEW_MATRIX, 16);
    // R * S * T applied to the origin: R * (2, 3, 4) = (-3, 2, 4).
    EXPECT_NEAR(m[12], -3, 1e-12);
    EXPECT_NEAR(m[13], 2, 1e-12);
    EXPECT_NEAR(m[14], 4, 1e-12);
    gl.put_floats({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
    gl.call("glLoadMatrixf", {P{GlExportHarness::IN}});
    EXPECT_EQ(gl.getf(GL_MODELVIEW_MATRIX, 16)[4], 5.0f);   // column-major order is kept
    gl.call("glLoadIdentity", {});
    gl.put_doubles({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 8, 9, 1});
    gl.call("glMultMatrixd", {P{GlExportHarness::IN}});
    EXPECT_EQ(gl.getd(GL_MODELVIEW_MATRIX, 16)[13], 8.0);
    EXPECT_EQ(gl.err(), 0u);
}

// ---- GetQueriesConvertTypesPerSpec ---------------------------------------------

/// Spec 6.1.2 / Table 4.7 INT for a colour component.
i32 int_of_color(double c) { return static_cast<i32>(std::floor((4294967295.0 * c - 1.0) / 2.0 + 0.5)); }

TEST_P(GlContextStateTest, GetQueriesConvertTypesPerSpec) {
    // Colours map [-1, 1] to the full integer range; booleans are nonzero.
    gl.call("glColor4f", {F{1}, F{-1}, F{0.5f}, F{0}});
    EXPECT_EQ(gl.geti(GL_CURRENT_COLOR, 4),
              (std::vector<i32>{2147483647, -2147483647 - 1, int_of_color(0.5), 0}));
    EXPECT_EQ(gl.getb(GL_CURRENT_COLOR, 4), (std::vector<u8>{1, 1, 1, 0}));
    EXPECT_EQ(gl.getd(GL_CURRENT_COLOR, 4)[2], 0.5);
    // Normals and DepthRange use the same mapping.
    gl.call("glNormal3f", {F{0}, F{0}, F{-1}});
    EXPECT_EQ(gl.geti(GL_CURRENT_NORMAL, 3), (std::vector<i32>{0, 0, -2147483647 - 1}));
    gl.call("glDepthRange", {D{0.25}, D{2.0}});   // clamped to [0, 1] (spec 2.10.1)
    EXPECT_EQ(gl.geti(GL_DEPTH_RANGE, 2), (std::vector<i32>{int_of_color(0.25), 2147483647}));
    EXPECT_EQ(gl.getd(GL_DEPTH_RANGE, 2), (std::vector<double>{0.25, 1.0}));
    gl.call("glClearDepth", {D{0.3}});
    EXPECT_EQ(gl.getd(GL_DEPTH_CLEAR_VALUE, 1)[0], 0.3);
    EXPECT_EQ(gl.geti(GL_DEPTH_CLEAR_VALUE, 1)[0], int_of_color(0.3));
    // Other floats round to nearest.
    gl.call("glLineWidth", {F{2.5f}});
    EXPECT_EQ(gl.geti(GL_LINE_WIDTH, 1)[0], 3);
    gl.call("glPointSize", {F{3.4f}});
    EXPECT_EQ(gl.geti(GL_POINT_SIZE, 1)[0], 3);
    EXPECT_EQ(gl.getb(GL_POINT_SIZE, 1)[0], 1);
    gl.call("glFogf", {I{GL_FOG_START}, F{-2.6f}});
    EXPECT_EQ(gl.geti(GL_FOG_START, 1)[0], -3);
    // Booleans read as 1/0 and 1.0/0.0; integers convert exactly to floats.
    EXPECT_EQ(gl.geti(GL_DITHER, 1)[0], 1);
    EXPECT_EQ(gl.getf(GL_DEPTH_WRITEMASK, 1)[0], 1.0f);
    EXPECT_EQ(gl.getd(GL_BLEND, 1)[0], 0.0);
    EXPECT_EQ(gl.getf(GL_MATRIX_MODE, 1)[0], 5888.0f);
    EXPECT_EQ(gl.getb(GL_BLEND_DST, 1)[0], 0);   // GL_ZERO
    // Masks keep their bit pattern.
    gl.call("glStencilMask", {I{0xFFFFFFFFu}});
    EXPECT_EQ(gl.geti(GL_STENCIL_WRITEMASK, 1)[0], -1);
    EXPECT_EQ(gl.getd(GL_STENCIL_WRITEMASK, 1)[0], 4294967295.0);
    // Integer colour commands convert per spec Table 2.6.
    gl.call("glColor3b", {I{127}, I{0xFFFFFF80u}, I{0}});   // high bits of the promoted slot ignored
    EXPECT_EQ(gl.getf(GL_CURRENT_COLOR, 4), (std::vector<float>{1, -1, 1.0f / 255, 1}));
    gl.call("glColor4ub", {I{255}, I{0}, I{0x180}, I{51}});
    EXPECT_EQ(gl.getf(GL_CURRENT_COLOR, 4), (std::vector<float>{1, 0, 128.0f / 255, 51.0f / 255}));
    gl.put_ints({0x7FFFFFFF, 0, -2147483647 - 1, 0});
    gl.call("glColor4iv", {P{GlExportHarness::IN}});
    EXPECT_EQ(gl.getf(GL_CURRENT_COLOR, 4),
              (std::vector<float>{1, static_cast<float>(1.0 / 4294967295.0), -1,
                                  static_cast<float>(1.0 / 4294967295.0)}));
    gl.call("glNormal3s", {I{32767}, I{0}, I{0xFFFF8000u}});
    EXPECT_EQ(gl.getf(GL_CURRENT_NORMAL, 3), (std::vector<float>{1, 1.0f / 65535, -1}));
    gl.call("glTexCoord2i", {I{3}, I{static_cast<u32>(-4)}});   // texture coordinates are not normalised
    EXPECT_EQ(gl.getf(GL_CURRENT_TEXTURE_COORDS, 4), (std::vector<float>{3, -4, 0, 1}));
    gl.call("glIndexs", {I{0xFFFF0007u}});
    EXPECT_EQ(gl.getf(GL_CURRENT_INDEX, 1)[0], 7.0f);
    // Indexed getters follow the same rules.
    gl.fill(0);
    gl.call("glGetLightiv", {I{GL_LIGHT0}, I{GL_DIFFUSE}, P{GlExportHarness::OUT}});
    EXPECT_EQ(static_cast<i32>(gl.memory.read_u32(GlExportHarness::OUT).value()), 2147483647);
    gl.call("glGetLightiv", {I{GL_LIGHT0}, I{GL_SPOT_CUTOFF}, P{GlExportHarness::OUT}});
    EXPECT_EQ(gl.memory.read_u32(GlExportHarness::OUT).value(), 180u);
    gl.call("glGetMaterialiv", {I{GL_BACK}, I{GL_AMBIENT}, P{GlExportHarness::OUT}});
    EXPECT_EQ(static_cast<i32>(gl.memory.read_u32(GlExportHarness::OUT).value()), int_of_color(0.2f));
    // Integer-specified light colours use Table 2.6's signed conversion.
    gl.put_ints({0x7FFFFFFF, 0, 0, 0x7FFFFFFF});
    gl.call("glLightiv", {I{GL_LIGHT1}, I{GL_AMBIENT}, P{GlExportHarness::IN}});
    gl.call("glGetLightfv", {I{GL_LIGHT1}, I{GL_AMBIENT}, P{GlExportHarness::OUT}});
    EXPECT_EQ(bitsf(gl.memory.read_u32(GlExportHarness::OUT).value()), 1.0f);
    // Light positions are stored in eye coordinates (spec 2.13.2).
    gl.call("glTranslatef", {F{10}, F{0}, F{0}});
    gl.put_floats({1, 2, 3, 1});
    gl.call("glLightfv", {I{GL_LIGHT2}, I{GL_POSITION}, P{GlExportHarness::IN}});
    gl.call("glGetLightfv", {I{GL_LIGHT2}, I{GL_POSITION}, P{GlExportHarness::OUT}});
    EXPECT_EQ(gl.out_floats(4), (std::vector<double>{11, 2, 3, 1}));
    gl.put_doubles({1, 0, 0, -5});
    gl.call("glClipPlane", {I{GL_CLIP_PLANE1}, P{GlExportHarness::IN}});
    gl.call("glGetClipPlane", {I{GL_CLIP_PLANE1}, P{GlExportHarness::OUT}});
    // (1, 0, 0, -5) M^-1 with M = translate(10, 0, 0): (1, 0, 0, -15).
    EXPECT_EQ(bitsd(gl.memory.read_u64(GlExportHarness::OUT + 24).value()), -15.0);
    EXPECT_EQ(gl.err(), 0u);
}

// ---- Colour material tracking (spec 2.13.3) --------------------------------------

TEST_P(GlContextStateTest, ColorMaterialTracksTheCurrentColor) {
    gl.call("glColor4f", {F{0.1f}, F{0.2f}, F{0.3f}, F{0.4f}});
    gl.call("glColorMaterial", {I{GL_FRONT}, I{GL_EMISSION}});
    gl.call("glGetMaterialfv", {I{GL_FRONT}, I{GL_EMISSION}, P{GlExportHarness::OUT}});
    EXPECT_EQ(gl.out_floats(4), (std::vector<double>{0, 0, 0, 1}));   // disabled: no tracking
    gl.call("glEnable", {I{GL_COLOR_MATERIAL}});
    gl.call("glGetMaterialfv", {I{GL_FRONT}, I{GL_EMISSION}, P{GlExportHarness::OUT}});
    EXPECT_EQ(gl.out_floats(4), (std::vector<double>{0.1f, 0.2f, 0.3f, 0.4f}));
    gl.call("glColor3f", {F{1}, F{0}, F{0}});
    gl.call("glGetMaterialfv", {I{GL_FRONT}, I{GL_EMISSION}, P{GlExportHarness::OUT}});
    EXPECT_EQ(gl.out_floats(4), (std::vector<double>{1, 0, 0, 1}));
    gl.call("glGetMaterialfv", {I{GL_BACK}, I{GL_EMISSION}, P{GlExportHarness::OUT}});
    EXPECT_EQ(gl.out_floats(4), (std::vector<double>{0, 0, 0, 1}));   // back not selected
    gl.call("glColorMaterial", {I{GL_BACK}, I{GL_AMBIENT_AND_DIFFUSE}});
    gl.call("glGetMaterialfv", {I{GL_BACK}, I{GL_DIFFUSE}, P{GlExportHarness::OUT}});
    EXPECT_EQ(gl.out_floats(4), (std::vector<double>{1, 0, 0, 1}));
    EXPECT_EQ(gl.err(), 0u);
}

// ---- Pixel and client-array state ------------------------------------------------------

TEST_P(GlContextStateTest, PixelAndArrayStateCommandsFollowTheSpec) {
    // glPixelStoref: booleans are "!= 0", integers round (spec 3.6.1).
    gl.call("glPixelStoref", {I{GL_UNPACK_ALIGNMENT}, F{2.4f}});
    EXPECT_EQ(gl.geti(GL_UNPACK_ALIGNMENT, 1)[0], 2);
    gl.call("glPixelStoref", {I{GL_PACK_ROW_LENGTH}, F{7.6f}});
    EXPECT_EQ(gl.geti(GL_PACK_ROW_LENGTH, 1)[0], 8);
    gl.call("glPixelStoref", {I{GL_UNPACK_SWAP_BYTES}, F{0.6f}});
    EXPECT_EQ(gl.geti(GL_UNPACK_SWAP_BYTES, 1)[0], 1);
    gl.call("glPixelStorei", {I{GL_UNPACK_SWAP_BYTES}, I{0}});
    gl.call("glPixelStorei", {I{GL_UNPACK_ALIGNMENT}, I{4}});
    gl.call("glPixelTransferi", {I{GL_RED_SCALE}, I{2}});
    gl.call("glPixelTransferf", {I{GL_INDEX_SHIFT}, F{-2.6f}});
    EXPECT_EQ(gl.getf(GL_RED_SCALE, 1)[0], 2.0f);
    EXPECT_EQ(gl.geti(GL_INDEX_SHIFT, 1)[0], -3);
    gl.call("glPixelZoom", {F{2}, F{-1}});
    EXPECT_EQ(gl.getf(GL_ZOOM_Y, 1)[0], -1.0f);

    // Pixel maps: colour entries from uiv/usv use Table 2.6, index entries are
    // values; S_TO_S rounds (spec 3.6.3); reads convert back (Table 4.7).
    gl.put_ints({0, static_cast<i32>(0xFFFFFFFFu)});
    gl.call("glPixelMapuiv", {I{GL_PIXEL_MAP_R_TO_R}, I{2}, P{GlExportHarness::IN}});
    EXPECT_EQ(gl.geti(GL_PIXEL_MAP_R_TO_R_SIZE, 1)[0], 2);
    gl.call("glGetPixelMapfv", {I{GL_PIXEL_MAP_R_TO_R}, P{GlExportHarness::OUT}});
    EXPECT_EQ(gl.out_floats(2), (std::vector<double>{0, 1}));
    gl.call("glGetPixelMapusv", {I{GL_PIXEL_MAP_R_TO_R}, P{GlExportHarness::OUT}});
    EXPECT_EQ(gl.memory.read_u16(GlExportHarness::OUT + 2).value(), 0xFFFF);
    gl.put_floats({1.4f, 2.6f});
    gl.call("glPixelMapfv", {I{GL_PIXEL_MAP_S_TO_S}, I{2}, P{GlExportHarness::IN}});
    gl.call("glGetPixelMapuiv", {I{GL_PIXEL_MAP_S_TO_S}, P{GlExportHarness::OUT}});
    EXPECT_EQ(gl.memory.read_u32(GlExportHarness::OUT).value(), 1u);
    EXPECT_EQ(gl.memory.read_u32(GlExportHarness::OUT + 4).value(), 3u);

    // Polygon stipple through the unpack/pack modes (spec 3.6.4, 4.3.2).
    std::vector<u8> pattern(128);
    for (u32 k = 0; k < 128; ++k) pattern[k] = static_cast<u8>(k * 37 + 1);
    ASSERT_TRUE(gl.memory.write_bytes(GlExportHarness::IN, pattern.data(), pattern.size()).is_ok());
    gl.call("glPixelStorei", {I{GL_UNPACK_LSB_FIRST}, I{1}});
    gl.call("glPolygonStipple", {P{GlExportHarness::IN}});
    gl.call("glPixelStorei", {I{GL_PACK_LSB_FIRST}, I{1}});
    gl.fill(0);
    gl.call("glGetPolygonStipple", {P{GlExportHarness::OUT}});
    for (u32 k = 0; k < 128; ++k) EXPECT_EQ(gl.memory.read_u8(GlExportHarness::OUT + k).value(), pattern[k]) << k;
    // Packed MSB-first the bytes come back bit-reversed.
    gl.call("glPixelStorei", {I{GL_PACK_LSB_FIRST}, I{0}});
    gl.call("glGetPolygonStipple", {P{GlExportHarness::OUT}});
    auto reverse = [](u8 b) { u8 r = 0; for (int k = 0; k < 8; ++k) if (b & (1 << k)) r |= static_cast<u8>(0x80 >> k); return r; };
    EXPECT_EQ(gl.memory.read_u8(GlExportHarness::OUT + 5).value(), reverse(pattern[5]));
    // bit x of row y = pixel (x, y): row 0 byte 0 bit 0 (LSB first).
    EXPECT_EQ(ctx.state.polygon_stipple.rows[0] & 0xFFu, pattern[0]);

    // glInterleavedArrays (spec 2.8, Table 2.5): T2F_C4UB_V3F.
    gl.call("glInterleavedArrays", {I{GL_T2F_C4UB_V3F}, I{0}, P{0x1000}});
    EXPECT_EQ(gl.err(), 0u);
    EXPECT_EQ(gl.call("glIsEnabled", {I{GL_TEXTURE_COORD_ARRAY}}), 1u);
    EXPECT_EQ(gl.call("glIsEnabled", {I{GL_COLOR_ARRAY}}), 1u);
    EXPECT_EQ(gl.call("glIsEnabled", {I{GL_NORMAL_ARRAY}}), 0u);
    EXPECT_EQ(gl.call("glIsEnabled", {I{GL_VERTEX_ARRAY}}), 1u);
    EXPECT_EQ(gl.geti(GL_COLOR_ARRAY_TYPE, 1)[0], static_cast<i32>(GL_UNSIGNED_BYTE));
    EXPECT_EQ(gl.geti(GL_VERTEX_ARRAY_STRIDE, 1)[0], 24);   // c + 5f
    auto pointer = [&](u32 pname) {
        gl.call("glGetPointerv", {I{pname}, P{GlExportHarness::OUT}});
        return gl.memory.read_u32(GlExportHarness::OUT).value();
    };
    EXPECT_EQ(pointer(GL_TEXTURE_COORD_ARRAY_POINTER), 0x1000u);
    EXPECT_EQ(pointer(GL_COLOR_ARRAY_POINTER), 0x1008u);    // 2f
    EXPECT_EQ(pointer(GL_VERTEX_ARRAY_POINTER), 0x100Cu);   // c + 2f
    gl.call("glVertexPointer", {I{2}, I{GL_SHORT}, I{6}, P{0x2000}});
    EXPECT_EQ(gl.geti(GL_VERTEX_ARRAY_SIZE, 1)[0], 2);
    EXPECT_EQ(pointer(GL_VERTEX_ARRAY_POINTER), 0x2000u);
    EXPECT_EQ(gl.err(), 0u);
}

// ---- PushPopRestoresEachGroupExactly (Review Focus 3) ------------------------------

/// One snapshot item: a queryable value and the attribute groups the spec's
/// "Attribute" column names for it (0 = "-": no group).
struct Item {
    std::string label;
    u32 groups;
    bool mutable_here;   // changed by mutate_everything()
    std::function<std::vector<double>(GlContext&)> read;
};

std::vector<double> get(GlContext& ctx, u32 pname) {
    GlQuery q;
    EXPECT_TRUE(cmd::Get(ctx, pname, q)) << pname;
    return std::vector<double>(q.v.begin(), q.v.begin() + q.count);
}

std::vector<Item> server_items() {
    std::vector<Item> items;
    auto add = [&](u32 pname, u32 groups, bool mut = true) {
        items.push_back({"pname " + std::to_string(pname), groups, mut,
                         [pname](GlContext& c) { return get(c, pname); }});
    };
    // Table 6.5 (current).
    for (u32 p : {GL_CURRENT_COLOR, GL_CURRENT_INDEX, GL_CURRENT_TEXTURE_COORDS, GL_CURRENT_NORMAL, GL_EDGE_FLAG})
        add(p, GL_CURRENT_BIT);
    for (u32 p : {GL_CURRENT_RASTER_POSITION, GL_CURRENT_RASTER_DISTANCE, GL_CURRENT_RASTER_COLOR,
                  GL_CURRENT_RASTER_INDEX, GL_CURRENT_RASTER_TEXTURE_COORDS, GL_CURRENT_RASTER_POSITION_VALID})
        add(p, GL_CURRENT_BIT, false);   // set by glRasterPos (Task 11)
    // Table 6.7: matrices "-", viewport, transform.
    for (u32 p : {GL_MODELVIEW_MATRIX, GL_PROJECTION_MATRIX, GL_TEXTURE_MATRIX}) add(p, 0);
    add(GL_VIEWPORT, GL_VIEWPORT_BIT);
    add(GL_DEPTH_RANGE, GL_VIEWPORT_BIT);
    add(GL_MATRIX_MODE, GL_TRANSFORM_BIT);
    for (u32 i = 0; i < 6; ++i) {
        items.push_back({"clip plane " + std::to_string(i), GL_TRANSFORM_BIT, true, [i](GlContext& c) {
                             std::array<double, 4> eq{};
                             EXPECT_TRUE(cmd::GetClipPlane(c, GL_CLIP_PLANE0 + i, eq));
                             return std::vector<double>(eq.begin(), eq.end());
                         }});
    }
    // Table 6.8.
    for (u32 p : {GL_FOG_COLOR, GL_FOG_INDEX, GL_FOG_DENSITY, GL_FOG_START, GL_FOG_END, GL_FOG_MODE})
        add(p, GL_FOG_BIT);
    add(GL_SHADE_MODEL, GL_LIGHTING_BIT);
    // Table 6.9.
    for (u32 p : {GL_COLOR_MATERIAL_PARAMETER, GL_COLOR_MATERIAL_FACE, GL_LIGHT_MODEL_AMBIENT,
                  GL_LIGHT_MODEL_LOCAL_VIEWER, GL_LIGHT_MODEL_TWO_SIDE})
        add(p, GL_LIGHTING_BIT);
    for (u32 face : {GL_FRONT, GL_BACK}) {
        for (u32 p : {GL_AMBIENT, GL_DIFFUSE, GL_SPECULAR, GL_EMISSION, GL_SHININESS, GL_COLOR_INDEXES}) {
            items.push_back({"material", GL_LIGHTING_BIT, true, [face, p](GlContext& c) {
                                 GlQuery q;
                                 EXPECT_TRUE(cmd::GetMaterial(c, face, p, q));
                                 return std::vector<double>(q.v.begin(), q.v.begin() + q.count);
                             }});
        }
    }
    for (u32 l = 0; l < 8; ++l) {
        for (u32 p : {GL_AMBIENT, GL_DIFFUSE, GL_SPECULAR, GL_POSITION, GL_SPOT_DIRECTION, GL_SPOT_EXPONENT,
                      GL_SPOT_CUTOFF, GL_CONSTANT_ATTENUATION, GL_LINEAR_ATTENUATION, GL_QUADRATIC_ATTENUATION}) {
            items.push_back({"light " + std::to_string(l) + " " + std::to_string(p), GL_LIGHTING_BIT, true,
                             [l, p](GlContext& c) {
                                 GlQuery q;
                                 EXPECT_TRUE(cmd::GetLight(c, GL_LIGHT0 + l, p, q));
                                 return std::vector<double>(q.v.begin(), q.v.begin() + q.count);
                             }});
        }
    }
    // Table 6.10.
    add(GL_POINT_SIZE, GL_POINT_BIT);
    for (u32 p : {GL_LINE_WIDTH, GL_LINE_STIPPLE_PATTERN, GL_LINE_STIPPLE_REPEAT}) add(p, GL_LINE_BIT);
    for (u32 p : {GL_CULL_FACE_MODE, GL_FRONT_FACE, GL_POLYGON_MODE, GL_POLYGON_OFFSET_FACTOR,
                  GL_POLYGON_OFFSET_UNITS})
        add(p, GL_POLYGON_BIT);
    items.push_back({"polygon stipple", GL_POLYGON_STIPPLE_BIT, true, [](GlContext& c) {
                         std::array<u32, 32> rows{};
                         EXPECT_TRUE(cmd::GetPolygonStipple(c, rows));
                         return std::vector<double>(rows.begin(), rows.end());
                     }});
    // Table 6.11-6.12 (texture); bindings need glBindTexture (Task 10).
    add(GL_TEXTURE_BINDING_1D, GL_TEXTURE_BIT, false);
    add(GL_TEXTURE_BINDING_2D, GL_TEXTURE_BIT, false);
    for (u32 target : {GL_TEXTURE_1D, GL_TEXTURE_2D}) {
        for (u32 p : {GL_TEXTURE_BORDER_COLOR, GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_TEXTURE_WRAP_S,
                      GL_TEXTURE_WRAP_T, GL_TEXTURE_PRIORITY}) {
            items.push_back({"texparam", GL_TEXTURE_BIT, true, [target, p](GlContext& c) {
                                 GlQuery q;
                                 EXPECT_TRUE(cmd::GetTexParameter(c, target, p, q));
                                 return std::vector<double>(q.v.begin(), q.v.begin() + q.count);
                             }});
        }
    }
    for (u32 p : {GL_TEXTURE_ENV_MODE, GL_TEXTURE_ENV_COLOR}) {
        items.push_back({"texenv", GL_TEXTURE_BIT, true, [p](GlContext& c) {
                             GlQuery q;
                             EXPECT_TRUE(cmd::GetTexEnv(c, GL_TEXTURE_ENV, p, q));
                             return std::vector<double>(q.v.begin(), q.v.begin() + q.count);
                         }});
    }
    for (u32 coord : {GL_S, GL_T, GL_R, GL_Q}) {
        for (u32 p : {GL_TEXTURE_GEN_MODE, GL_OBJECT_PLANE, GL_EYE_PLANE}) {
            items.push_back({"texgen", GL_TEXTURE_BIT, true, [coord, p](GlContext& c) {
                                 GlQuery q;
                                 EXPECT_TRUE(cmd::GetTexGen(c, coord, p, q));
                                 return std::vector<double>(q.v.begin(), q.v.begin() + q.count);
                             }});
        }
    }
    // Table 6.13.
    add(GL_SCISSOR_BOX, GL_SCISSOR_BIT);
    for (u32 p : {GL_ALPHA_TEST_FUNC, GL_ALPHA_TEST_REF, GL_BLEND_SRC, GL_BLEND_DST, GL_LOGIC_OP_MODE})
        add(p, GL_COLOR_BUFFER_BIT);
    for (u32 p : {GL_STENCIL_FUNC, GL_STENCIL_VALUE_MASK, GL_STENCIL_REF, GL_STENCIL_FAIL,
                  GL_STENCIL_PASS_DEPTH_FAIL, GL_STENCIL_PASS_DEPTH_PASS})
        add(p, GL_STENCIL_BUFFER_BIT);
    add(GL_DEPTH_FUNC, GL_DEPTH_BUFFER_BIT);
    // Table 6.14.
    for (u32 p : {GL_DRAW_BUFFER, GL_INDEX_WRITEMASK, GL_COLOR_WRITEMASK, GL_COLOR_CLEAR_VALUE,
                  GL_INDEX_CLEAR_VALUE})
        add(p, GL_COLOR_BUFFER_BIT);
    add(GL_DEPTH_WRITEMASK, GL_DEPTH_BUFFER_BIT);
    add(GL_DEPTH_CLEAR_VALUE, GL_DEPTH_BUFFER_BIT);
    add(GL_STENCIL_WRITEMASK, GL_STENCIL_BUFFER_BIT);
    add(GL_STENCIL_CLEAR_VALUE, GL_STENCIL_BUFFER_BIT);
    add(GL_ACCUM_CLEAR_VALUE, GL_ACCUM_BUFFER_BIT);
    // Table 6.16 (pixel); the maps are "-".
    for (u32 p : {GL_MAP_COLOR, GL_MAP_STENCIL, GL_INDEX_SHIFT, GL_INDEX_OFFSET, GL_RED_SCALE, GL_GREEN_SCALE,
                  GL_BLUE_SCALE, GL_ALPHA_SCALE, GL_DEPTH_SCALE, GL_RED_BIAS, GL_GREEN_BIAS, GL_BLUE_BIAS,
                  GL_ALPHA_BIAS, GL_DEPTH_BIAS, GL_ZOOM_X, GL_ZOOM_Y, GL_READ_BUFFER})
        add(p, GL_PIXEL_MODE_BIT);
    add(GL_PIXEL_MAP_R_TO_R_SIZE, 0);
    // Table 6.17.
    for (u32 p : {GL_MAP1_GRID_DOMAIN, GL_MAP1_GRID_SEGMENTS, GL_MAP2_GRID_DOMAIN, GL_MAP2_GRID_SEGMENTS})
        add(p, GL_EVAL_BIT);
    // Table 6.18.
    for (u32 p : {GL_PERSPECTIVE_CORRECTION_HINT, GL_POINT_SMOOTH_HINT, GL_LINE_SMOOTH_HINT,
                  GL_POLYGON_SMOOTH_HINT, GL_FOG_HINT})
        add(p, GL_HINT_BIT);
    // Table 6.22.
    add(GL_LIST_BASE, GL_LIST_BIT);
    // Enables: "x/enable" -> the x group and ENABLE_BIT (spec Tables 6.7-6.17).
    const std::pair<u32, u32> kEnables[] = {
        {GL_NORMALIZE, GL_TRANSFORM_BIT},   {GL_CLIP_PLANE0, GL_TRANSFORM_BIT}, {GL_CLIP_PLANE1, GL_TRANSFORM_BIT},
        {GL_CLIP_PLANE2, GL_TRANSFORM_BIT}, {GL_CLIP_PLANE3, GL_TRANSFORM_BIT}, {GL_CLIP_PLANE4, GL_TRANSFORM_BIT},
        {GL_CLIP_PLANE5, GL_TRANSFORM_BIT}, {GL_FOG, GL_FOG_BIT},               {GL_LIGHTING, GL_LIGHTING_BIT},
        {GL_COLOR_MATERIAL, GL_LIGHTING_BIT}, {GL_LIGHT0, GL_LIGHTING_BIT},     {GL_LIGHT1, GL_LIGHTING_BIT},
        {GL_LIGHT2, GL_LIGHTING_BIT},       {GL_LIGHT3, GL_LIGHTING_BIT},       {GL_LIGHT4, GL_LIGHTING_BIT},
        {GL_LIGHT5, GL_LIGHTING_BIT},       {GL_LIGHT6, GL_LIGHTING_BIT},       {GL_LIGHT7, GL_LIGHTING_BIT},
        {GL_POINT_SMOOTH, GL_POINT_BIT},    {GL_LINE_SMOOTH, GL_LINE_BIT},      {GL_LINE_STIPPLE, GL_LINE_BIT},
        {GL_CULL_FACE, GL_POLYGON_BIT},     {GL_POLYGON_SMOOTH, GL_POLYGON_BIT},
        {GL_POLYGON_OFFSET_POINT, GL_POLYGON_BIT}, {GL_POLYGON_OFFSET_LINE, GL_POLYGON_BIT},
        {GL_POLYGON_OFFSET_FILL, GL_POLYGON_BIT}, {GL_POLYGON_STIPPLE, GL_POLYGON_BIT},
        {GL_TEXTURE_1D, GL_TEXTURE_BIT},    {GL_TEXTURE_2D, GL_TEXTURE_BIT},    {GL_TEXTURE_GEN_S, GL_TEXTURE_BIT},
        {GL_TEXTURE_GEN_T, GL_TEXTURE_BIT}, {GL_TEXTURE_GEN_R, GL_TEXTURE_BIT}, {GL_TEXTURE_GEN_Q, GL_TEXTURE_BIT},
        {GL_SCISSOR_TEST, GL_SCISSOR_BIT},  {GL_ALPHA_TEST, GL_COLOR_BUFFER_BIT}, {GL_STENCIL_TEST, GL_STENCIL_BUFFER_BIT},
        {GL_DEPTH_TEST, GL_DEPTH_BUFFER_BIT}, {GL_BLEND, GL_COLOR_BUFFER_BIT},  {GL_DITHER, GL_COLOR_BUFFER_BIT},
        {GL_INDEX_LOGIC_OP, GL_COLOR_BUFFER_BIT}, {GL_COLOR_LOGIC_OP, GL_COLOR_BUFFER_BIT},
        {GL_AUTO_NORMAL, GL_EVAL_BIT},
    };
    for (const auto& [cap, group] : kEnables) add(cap, group | GL_ENABLE_BIT);
    for (u32 cap = GL_MAP1_COLOR_4; cap <= GL_MAP1_VERTEX_4; ++cap) add(cap, GL_EVAL_BIT | GL_ENABLE_BIT);
    for (u32 cap = GL_MAP2_COLOR_4; cap <= GL_MAP2_VERTEX_4; ++cap) add(cap, GL_EVAL_BIT | GL_ENABLE_BIT);
    return items;
}

std::vector<Item> client_items() {
    std::vector<Item> items;
    auto add = [&](u32 pname, u32 groups) {
        items.push_back({"pname " + std::to_string(pname), groups, true,
                         [pname](GlContext& c) { return get(c, pname); }});
    };
    for (u32 p = GL_UNPACK_SWAP_BYTES; p <= GL_UNPACK_ALIGNMENT; ++p) add(p, GL_CLIENT_PIXEL_STORE_BIT);
    for (u32 p = GL_PACK_SWAP_BYTES; p <= GL_PACK_ALIGNMENT; ++p) add(p, GL_CLIENT_PIXEL_STORE_BIT);
    for (u32 p : {GL_VERTEX_ARRAY, GL_VERTEX_ARRAY_SIZE, GL_VERTEX_ARRAY_TYPE, GL_VERTEX_ARRAY_STRIDE,
                  GL_NORMAL_ARRAY, GL_NORMAL_ARRAY_TYPE, GL_NORMAL_ARRAY_STRIDE, GL_COLOR_ARRAY,
                  GL_COLOR_ARRAY_SIZE, GL_COLOR_ARRAY_TYPE, GL_COLOR_ARRAY_STRIDE, GL_INDEX_ARRAY,
                  GL_INDEX_ARRAY_TYPE, GL_INDEX_ARRAY_STRIDE, GL_TEXTURE_COORD_ARRAY, GL_TEXTURE_COORD_ARRAY_SIZE,
                  GL_TEXTURE_COORD_ARRAY_TYPE, GL_TEXTURE_COORD_ARRAY_STRIDE, GL_EDGE_FLAG_ARRAY,
                  GL_EDGE_FLAG_ARRAY_STRIDE})
        add(p, GL_CLIENT_VERTEX_ARRAY_BIT);
    for (u32 p = GL_VERTEX_ARRAY_POINTER; p <= GL_EDGE_FLAG_ARRAY_POINTER; ++p) {
        items.push_back({"pointer " + std::to_string(p), GL_CLIENT_VERTEX_ARRAY_BIT, true, [p](GlContext& c) {
                             GuestAddr a = 0;
                             EXPECT_TRUE(cmd::GetPointer(c, p, a));
                             return std::vector<double>{static_cast<double>(a)};
                         }});
    }
    return items;
}

/// Changes every item server_items() marks mutable.
void mutate_server(GlContext& c) {
    cmd::Color(c, 0.1f, 0.2f, 0.3f, 0.4f);
    cmd::Index(c, 5);
    cmd::TexCoord(c, 1, 2, 3, 4);
    cmd::Normal(c, 1, 0, 0);
    cmd::EdgeFlag(c, false);
    for (u32 i = 0; i < 6; ++i) cmd::ClipPlane(c, GL_CLIP_PLANE0 + i, {1.0 + i, 2, 3, 4});
    cmd::Viewport(c, 1, 2, 3, 4);
    cmd::DepthRange(c, 0.25, 0.75);
    cmd::Fog(c, GL_FOG_COLOR, GlValues::vector(std::array<double, 4>{0.5, 0.5, 0.5, 0.5}, false));
    cmd::Fog(c, GL_FOG_INDEX, GlValues::scalar_value(2, false));
    cmd::Fog(c, GL_FOG_DENSITY, GlValues::scalar_value(0.5, false));
    cmd::Fog(c, GL_FOG_START, GlValues::scalar_value(3, false));
    cmd::Fog(c, GL_FOG_END, GlValues::scalar_value(4, false));
    cmd::Fog(c, GL_FOG_MODE, GlValues::scalar_value(GL_LINEAR, true));
    cmd::ShadeModel(c, GL_FLAT);
    cmd::ColorMaterial(c, GL_FRONT, GL_EMISSION);
    cmd::LightModel(c, GL_LIGHT_MODEL_AMBIENT, GlValues::vector(std::array<double, 4>{0.3, 0.3, 0.3, 0.3}, false));
    cmd::LightModel(c, GL_LIGHT_MODEL_LOCAL_VIEWER, GlValues::scalar_value(1, true));
    cmd::LightModel(c, GL_LIGHT_MODEL_TWO_SIDE, GlValues::scalar_value(1, true));
    for (u32 face : {GL_FRONT, GL_BACK}) {
        for (u32 p : {GL_AMBIENT, GL_DIFFUSE, GL_SPECULAR, GL_EMISSION})
            cmd::Material(c, face, p, GlValues::vector(std::array<double, 4>{0.6, 0.6, 0.6, 0.6}, false));
        cmd::Material(c, face, GL_SHININESS, GlValues::scalar_value(5, false));
        cmd::Material(c, face, GL_COLOR_INDEXES, GlValues::vector(std::array<double, 3>{4, 5, 6}, false));
    }
    for (u32 l = 0; l < 8; ++l) {
        const u32 light = GL_LIGHT0 + l;
        for (u32 p : {GL_AMBIENT, GL_DIFFUSE, GL_SPECULAR, GL_POSITION})
            cmd::Light(c, light, p, GlValues::vector(std::array<double, 4>{0.7, 0.6, 0.5, 0.4}, false));
        cmd::Light(c, light, GL_SPOT_DIRECTION, GlValues::vector(std::array<double, 3>{1, 0, 0}, false));
        cmd::Light(c, light, GL_SPOT_EXPONENT, GlValues::scalar_value(2, false));
        cmd::Light(c, light, GL_SPOT_CUTOFF, GlValues::scalar_value(45, false));
        cmd::Light(c, light, GL_CONSTANT_ATTENUATION, GlValues::scalar_value(2, false));
        cmd::Light(c, light, GL_LINEAR_ATTENUATION, GlValues::scalar_value(3, false));
        cmd::Light(c, light, GL_QUADRATIC_ATTENUATION, GlValues::scalar_value(4, false));
    }
    cmd::PointSize(c, 3);
    cmd::LineWidth(c, 2);
    cmd::LineStipple(c, 3, 0xAAAA);
    cmd::CullFace(c, GL_FRONT);
    cmd::FrontFace(c, GL_CW);
    cmd::PolygonMode(c, GL_FRONT, GL_LINE);
    cmd::PolygonOffset(c, 1, 2);
    std::array<u32, 32> rows{};
    rows.fill(0x0F0F0F0Fu);
    cmd::PolygonStipple(c, rows);
    for (u32 target : {GL_TEXTURE_1D, GL_TEXTURE_2D}) {
        cmd::TexParameter(c, target, GL_TEXTURE_BORDER_COLOR,
                          GlValues::vector(std::array<double, 4>{0.5, 0.5, 0.5, 0.5}, false));
        cmd::TexParameter(c, target, GL_TEXTURE_MIN_FILTER, GlValues::scalar_value(GL_NEAREST, true));
        cmd::TexParameter(c, target, GL_TEXTURE_MAG_FILTER, GlValues::scalar_value(GL_NEAREST, true));
        cmd::TexParameter(c, target, GL_TEXTURE_WRAP_S, GlValues::scalar_value(GL_CLAMP, true));
        cmd::TexParameter(c, target, GL_TEXTURE_WRAP_T, GlValues::scalar_value(GL_CLAMP, true));
        cmd::TexParameter(c, target, GL_TEXTURE_PRIORITY, GlValues::scalar_value(0.5, false));
    }
    cmd::TexEnv(c, GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GlValues::scalar_value(GL_DECAL, true));
    cmd::TexEnv(c, GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR,
                GlValues::vector(std::array<double, 4>{0.2, 0.2, 0.2, 0.2}, false));
    for (u32 coord : {GL_S, GL_T, GL_R, GL_Q}) {
        cmd::TexGen(c, coord, GL_TEXTURE_GEN_MODE, GlValues::scalar_value(GL_OBJECT_LINEAR, true));
        cmd::TexGen(c, coord, GL_OBJECT_PLANE, GlValues::vector(std::array<double, 4>{9, 9, 9, 9}, false));
        cmd::TexGen(c, coord, GL_EYE_PLANE, GlValues::vector(std::array<double, 4>{8, 8, 8, 8}, false));
    }
    cmd::Scissor(c, 5, 6, 7, 8);
    cmd::AlphaFunc(c, GL_LESS, 0.5f);
    cmd::BlendFunc(c, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    cmd::LogicOp(c, GL_XOR);
    cmd::StencilFunc(c, GL_EQUAL, 1, 0xF);
    cmd::StencilOp(c, GL_ZERO, GL_INCR, GL_DECR);
    cmd::DepthFunc(c, GL_GREATER);
    cmd::DrawBuffer(c, GL_FRONT);
    cmd::IndexMask(c, 5);
    cmd::ColorMask(c, false, true, false, true);
    cmd::ClearColor(c, 0.1f, 0.2f, 0.3f, 0.4f);
    cmd::ClearIndex(c, 3);
    cmd::DepthMask(c, false);
    cmd::ClearDepth(c, 0.5);
    cmd::StencilMask(c, 3);
    cmd::ClearStencil(c, 2);
    cmd::ClearAccum(c, 0.5f, 0.5f, 0.5f, 0.5f);
    cmd::PixelTransfer(c, GL_MAP_COLOR, 1, true);
    cmd::PixelTransfer(c, GL_MAP_STENCIL, 1, true);
    cmd::PixelTransfer(c, GL_INDEX_SHIFT, 1, true);
    cmd::PixelTransfer(c, GL_INDEX_OFFSET, 1, true);
    for (u32 p : {GL_RED_SCALE, GL_GREEN_SCALE, GL_BLUE_SCALE, GL_ALPHA_SCALE, GL_DEPTH_SCALE, GL_RED_BIAS,
                  GL_GREEN_BIAS, GL_BLUE_BIAS, GL_ALPHA_BIAS, GL_DEPTH_BIAS})
        cmd::PixelTransfer(c, p, 2.5, false);
    cmd::PixelZoom(c, 2, 3);
    cmd::ReadBuffer(c, GL_FRONT);
    const float map[2] = {0.25f, 0.75f};
    cmd::PixelMap(c, GL_PIXEL_MAP_R_TO_R, 2, map);
    cmd::MapGrid1(c, 5, 0.5, 2);
    cmd::MapGrid2(c, 6, 0.5, 2, 7, 0.25, 3);
    for (u32 p : {GL_PERSPECTIVE_CORRECTION_HINT, GL_POINT_SMOOTH_HINT, GL_LINE_SMOOTH_HINT,
                  GL_POLYGON_SMOOTH_HINT, GL_FOG_HINT})
        cmd::Hint(c, p, GL_NICEST);
    cmd::ListBase(c, 7);
    for (u32 i = 0; i < kGlCapCount; ++i) {
        const u32 cap = gl_cap_enum(static_cast<GlCap>(i));
        if (cap == GL_DITHER) cmd::Disable(c, cap); else cmd::Enable(c, cap);
    }
    GlMatrix m = gl_identity_matrix();
    m[12] = 4;
    for (u32 mode : {GL_PROJECTION, GL_TEXTURE, GL_MODELVIEW}) {
        cmd::MatrixMode(c, mode);
        cmd::LoadMatrix(c, m);
    }
    cmd::MatrixMode(c, GL_TEXTURE);
}

void mutate_client(GlContext& c) {
    for (u32 p = GL_UNPACK_SWAP_BYTES; p <= GL_UNPACK_SKIP_PIXELS; ++p) cmd::PixelStore(c, p, 1, true);
    for (u32 p = GL_PACK_SWAP_BYTES; p <= GL_PACK_SKIP_PIXELS; ++p) cmd::PixelStore(c, p, 1, true);
    cmd::PixelStore(c, GL_UNPACK_ALIGNMENT, 8, true);
    cmd::PixelStore(c, GL_PACK_ALIGNMENT, 2, true);
    for (u32 a : {GL_VERTEX_ARRAY, GL_NORMAL_ARRAY, GL_COLOR_ARRAY, GL_INDEX_ARRAY, GL_TEXTURE_COORD_ARRAY,
                  GL_EDGE_FLAG_ARRAY})
        cmd::EnableClientState(c, a);
    cmd::VertexPointer(c, 2, GL_SHORT, 4, 0x100);
    cmd::NormalPointer(c, GL_BYTE, 4, 0x200);
    cmd::ColorPointer(c, 3, GL_UNSIGNED_BYTE, 4, 0x300);
    cmd::IndexPointer(c, GL_SHORT, 4, 0x400);
    cmd::TexCoordPointer(c, 1, GL_INT, 4, 0x500);
    cmd::EdgeFlagPointer(c, 4, 0x600);
}

using Snapshot = std::vector<std::vector<double>>;
Snapshot snapshot(GlContext& c, const std::vector<Item>& items) {
    Snapshot s;
    for (const Item& item : items) s.push_back(item.read(c));
    return s;
}

void check_group(u32 mask, const std::vector<Item>& items, const Snapshot& before, const Snapshot& mutated,
                 const Snapshot& after) {
    for (usize k = 0; k < items.size(); ++k) {
        const bool restored = (items[k].groups & mask) != 0;
        EXPECT_EQ(after[k], restored ? before[k] : mutated[k])
            << "mask 0x" << std::hex << mask << " item " << items[k].label << " (groups 0x" << items[k].groups << ")";
    }
}

TEST(GlAttribStackTest, PushPopRestoresEachGroupExactly) {
    const auto items = server_items();
    // The mutation must change every mutable item, or a group's case proves nothing.
    {
        GlContext c(oracle_format3());
        c.on_first_make_current(320, 200);
        const Snapshot before = snapshot(c, items);
        mutate_server(c);
        const Snapshot mutated = snapshot(c, items);
        for (usize k = 0; k < items.size(); ++k) {
            if (items[k].mutable_here) EXPECT_NE(before[k], mutated[k]) << items[k].label;
        }
        EXPECT_EQ(c.pending_error(), GL_NO_ERROR);
    }
    const u32 kGroups[] = {GL_CURRENT_BIT, GL_POINT_BIT, GL_LINE_BIT, GL_POLYGON_BIT, GL_POLYGON_STIPPLE_BIT,
                           GL_PIXEL_MODE_BIT, GL_LIGHTING_BIT, GL_FOG_BIT, GL_DEPTH_BUFFER_BIT,
                           GL_ACCUM_BUFFER_BIT, GL_STENCIL_BUFFER_BIT, GL_VIEWPORT_BIT, GL_TRANSFORM_BIT,
                           GL_ENABLE_BIT, GL_COLOR_BUFFER_BIT, GL_HINT_BIT, GL_EVAL_BIT, GL_LIST_BIT,
                           GL_TEXTURE_BIT, GL_SCISSOR_BIT, GL_ALL_ATTRIB_BITS,
                           GL_FOG_BIT | GL_SCISSOR_BIT};
    for (u32 mask : kGroups) {
        GlContext c(oracle_format3());
        c.on_first_make_current(320, 200);
        const Snapshot before = snapshot(c, items);
        cmd::PushAttrib(c, mask);
        EXPECT_EQ(get(c, GL_ATTRIB_STACK_DEPTH)[0], 1.0);
        mutate_server(c);
        const Snapshot mutated = snapshot(c, items);
        cmd::PopAttrib(c);
        EXPECT_EQ(c.pending_error(), GL_NO_ERROR) << mask;
        EXPECT_EQ(get(c, GL_ATTRIB_STACK_DEPTH)[0], 0.0);
        check_group(mask, items, before, mutated, snapshot(c, items));
    }
    const auto citems = client_items();
    for (u32 mask : {GL_CLIENT_PIXEL_STORE_BIT, GL_CLIENT_VERTEX_ARRAY_BIT, GL_CLIENT_ALL_ATTRIB_BITS}) {
        GlContext c(oracle_format3());
        const Snapshot before = snapshot(c, citems);
        cmd::PushClientAttrib(c, mask);
        mutate_client(c);
        const Snapshot mutated = snapshot(c, citems);
        for (usize k = 0; k < citems.size(); ++k) EXPECT_NE(before[k], mutated[k]) << citems[k].label;
        cmd::PopClientAttrib(c);
        EXPECT_EQ(c.pending_error(), GL_NO_ERROR);
        check_group(mask, citems, before, mutated, snapshot(c, citems));
        // Server state is not on the client stack and vice versa.
        cmd::PushAttrib(c, GL_ALL_ATTRIB_BITS);
        mutate_client(c);
        const Snapshot client_now = snapshot(c, citems);
        cmd::PopAttrib(c);
        EXPECT_EQ(snapshot(c, citems), client_now);
    }
}

TEST_P(GlContextStateTest, AttribStackDepthsAndErrors) {
    // err-underflow-glPopAttrib / -glPopClientAttrib.
    gl.call("glPopAttrib", {});
    EXPECT_EQ(gl.err(), GL_STACK_UNDERFLOW);
    gl.call("glPopClientAttrib", {});
    EXPECT_EQ(gl.err(), GL_STACK_UNDERFLOW);
    // err-overflow-glPushAttrib ok-pushes=16 depth-after=16 (and the client stack).
    for (int k = 0; k < 16; ++k) {
        gl.call("glPushAttrib", {I{GL_ALL_ATTRIB_BITS}});
        gl.call("glPushClientAttrib", {I{GL_CLIENT_ALL_ATTRIB_BITS}});
    }
    EXPECT_EQ(gl.err(), 0u);
    gl.call("glPushAttrib", {I{GL_ALL_ATTRIB_BITS}});
    EXPECT_EQ(gl.err(), GL_STACK_OVERFLOW);
    gl.call("glPushClientAttrib", {I{GL_CLIENT_ALL_ATTRIB_BITS}});
    EXPECT_EQ(gl.err(), GL_STACK_OVERFLOW);
    EXPECT_EQ(gl.geti(GL_ATTRIB_STACK_DEPTH, 1)[0], 16);
    EXPECT_EQ(gl.geti(GL_CLIENT_ATTRIB_STACK_DEPTH, 1)[0], 16);
    for (int k = 0; k < 16; ++k) {
        gl.call("glPopAttrib", {});
        gl.call("glPopClientAttrib", {});
    }
    EXPECT_EQ(gl.err(), 0u);
    // err-glPushAttrib-bad-mask: unknown bits are accepted silently.
    gl.call("glPushAttrib", {I{0x80000000u}});
    EXPECT_EQ(gl.err(), 0u);
    EXPECT_EQ(gl.geti(GL_ATTRIB_STACK_DEPTH, 1)[0], 1);
    gl.call("glPopAttrib", {});
    // GL_TEXTURE_BIT restores the bound default texture's parameters (spec 6).
    gl.call("glPushAttrib", {I{GL_TEXTURE_BIT}});
    gl.call("glTexParameteri", {I{GL_TEXTURE_2D}, I{GL_TEXTURE_MIN_FILTER}, I{GL_NEAREST}});
    gl.call("glPopAttrib", {});
    gl.fill(0);
    gl.call("glGetTexParameteriv", {I{GL_TEXTURE_2D}, I{GL_TEXTURE_MIN_FILTER}, P{GlExportHarness::OUT}});
    EXPECT_EQ(gl.memory.read_u32(GlExportHarness::OUT).value(), GL_NEAREST_MIPMAP_LINEAR);
    // Name stack outside selection mode: no effect, no error
    // (err-glPushName-render-mode, -glPopName-, -glLoadName-).
    gl.call("glPushName", {I{1}});
    gl.call("glPopName", {});
    gl.call("glPopName", {});
    gl.call("glLoadName", {I{1}});
    gl.call("glInitNames", {});
    EXPECT_EQ(gl.err(), 0u);
    EXPECT_EQ(gl.geti(GL_NAME_STACK_DEPTH, 1)[0], 0);
}

// ---- NoCurrentContextMatchesOracle --------------------------------------------------

class GlNoContextTest : public ::testing::TestWithParam<bool> {};

TEST_P(GlNoContextTest, NoCurrentContextMatchesOracle) {
    GlExportHarness gl(GetParam());
    ASSERT_EQ(current_gl_context(), nullptr);
    set_last_error(0x1234);
    // noctx-glGetError ret=1282 on every call, before and after other calls.
    EXPECT_EQ(gl.call("glGetError", {}), 1282u);
    EXPECT_EQ(gl.call("glGetError", {}), 1282u);
    // noctx-glGetString ret=NULL, noctx-glIsEnabled ret=0.
    EXPECT_EQ(gl.call("glGetString", {I{GL_VENDOR}}), 0u);
    EXPECT_EQ(gl.call("glIsEnabled", {I{GL_DITHER}}), 0u);
    // noctx-glGetIntegerv / -glGetFloatv / -glGetBooleanv / -glGenTextures: untouched=1.
    gl.fill(0x77);
    gl.call("glGetIntegerv", {I{GL_VIEWPORT}, P{GlExportHarness::OUT}});
    gl.call("glGetFloatv", {I{GL_VIEWPORT}, P{GlExportHarness::OUT}});
    gl.call("glGetBooleanv", {I{GL_DITHER}, P{GlExportHarness::OUT}});
    gl.call("glGetDoublev", {I{GL_VIEWPORT}, P{GlExportHarness::OUT}});
    gl.call("glGenTextures", {I{1}, P{GlExportHarness::OUT}});
    for (u32 k = 0; k < 32; ++k) EXPECT_EQ(gl.memory.read_u8(GlExportHarness::OUT + k).value(), 0x77) << k;
    // noctx-glIsTexture ret=0, noctx-glGenLists ret=0, noctx-glClear,
    // noctx-glBegin-glVertex3f-glEnd ret=0.
    EXPECT_EQ(gl.call("glIsTexture", {I{1}}), 0u);
    EXPECT_EQ(gl.call("glGenLists", {I{1}}), 0u);
    EXPECT_EQ(gl.call("glClear", {I{GL_COLOR_BUFFER_BIT}}), 0u);
    EXPECT_EQ(gl.call("glBegin", {I{GL_TRIANGLES}}), 0u);
    EXPECT_EQ(gl.call("glVertex3f", {F{0}, F{0}, F{0}}), 0u);
    EXPECT_EQ(gl.call("glEnd", {}), 0u);
    EXPECT_EQ(gl.call("glRenderMode", {I{GL_RENDER}}), 0u);
    // noctx-glGetError-after ret=1282; gle=untouched throughout.
    EXPECT_EQ(gl.call("glGetError", {}), 1282u);
    EXPECT_EQ(get_last_error(), 0x1234u);
    // A context made current on another thread is not current here
    // (phase=other-thread-ctx-current).
    GlContext other(oracle_format3());
    std::thread([&] { set_current_gl_context_for_thread(&other); }).join();
    EXPECT_EQ(current_gl_context(), nullptr);
    EXPECT_EQ(gl.call("glGetError", {}), 1282u);
}

// ---- Registration ---------------------------------------------------------------------

TEST_P(GlContextStateTest, EveryExportIsRegisteredWithItsHandler) {
    // Every row of gl11_exports.inc resolves with its ordinal and stack size.
    struct Row { const char* name; u16 ordinal; u16 bytes; };
    const Row rows[] = {
#define GL11_EXPORT(name, ordinal, bytes, ret, kinds) {#name, ordinal, bytes},
#define GL11_OTHER_EXPORT(name, ordinal, bytes) {#name, ordinal, bytes},
#include "winapi/graphics/opengl32/gl11_exports.inc"
#undef GL11_EXPORT
#undef GL11_OTHER_EXPORT
    };
    EXPECT_EQ(std::size(rows), 368u);
    for (const Row& row : rows) {
        auto by_name = gl.registry->get_thunk_address("opengl32.dll", row.name);
        ASSERT_TRUE(by_name.is_ok()) << row.name;
        auto by_ordinal = gl.registry->get_thunk_address("opengl32.dll", row.ordinal);
        ASSERT_TRUE(by_ordinal.is_ok()) << row.name;
        EXPECT_EQ(by_ordinal.value(), by_name.value()) << row.name;
        EXPECT_EQ(gl.registry->resolve_stdcall_bytes(by_name.value()), row.bytes) << row.name;
    }
    // Every handler names a real GL 1.1 export.
    for (std::string_view name : gl_export_handler_names()) {
        bool found = false;
        for (const Row& row : rows) found = found || name == row.name;
        EXPECT_TRUE(found) << name;
    }
    // Every GL 1.1 command has its handler (Task 12 implemented the last, the
    // display-list commands); no GL export is a placeholder any more.
    u32 gl_rows = 0;
#define GL11_EXPORT(name, ordinal, bytes, ret, kinds) \
    EXPECT_NE(gl_export_handler(#name), nullptr) << #name; \
    ++gl_rows;
#define GL11_OTHER_EXPORT(name, ordinal, bytes)
#include "winapi/graphics/opengl32/gl11_exports.inc"
#undef GL11_EXPORT
#undef GL11_OTHER_EXPORT
    EXPECT_EQ(gl_rows, 336u);
    EXPECT_EQ(gl.call("glIsList", {I{1}}), 0u);
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
}

// ---- Assertions migrated from test_opengl32.cpp (the replaced no-op recorder) ----

TEST_P(GlContextStateTest, MigratedRecorderAssertionsStillHold) {
    const GuestAddr out = GlExportHarness::OUT;
    // POINT_SIZE: set, reject 0 (INVALID_VALUE), integer query rounds 3.5 -> 4.
    EXPECT_EQ(gl.getd(GL_POINT_SIZE, 1)[0], 1.0);
    gl.call("glPointSize", {F{3.5f}});
    EXPECT_EQ(gl.err(), 0u);
    EXPECT_EQ(gl.getd(GL_POINT_SIZE, 1)[0], 3.5);
    gl.call("glPointSize", {F{0.0f}});
    EXPECT_EQ(gl.err(), GL_INVALID_VALUE);
    EXPECT_EQ(gl.geti(GL_POINT_SIZE, 1)[0], 4);
    EXPECT_EQ(gl.err(), 0u);
    // LOGIC_OP_MODE.
    EXPECT_EQ(gl.geti(GL_LOGIC_OP_MODE, 1)[0], static_cast<i32>(GL_COPY));
    gl.call("glLogicOp", {I{GL_XOR}});
    EXPECT_EQ(gl.geti(GL_LOGIC_OP_MODE, 1)[0], static_cast<i32>(GL_XOR));
    // DRAW_BUFFER: BACK initially; FRONT, NONE accepted; AUX3 is INVALID_OPERATION.
    EXPECT_EQ(gl.geti(GL_DRAW_BUFFER, 1)[0], static_cast<i32>(GL_BACK));
    gl.call("glDrawBuffer", {I{GL_FRONT}});
    EXPECT_EQ(gl.geti(GL_DRAW_BUFFER, 1)[0], static_cast<i32>(GL_FRONT));
    gl.call("glDrawBuffer", {I{GL_AUX3}});
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);
    EXPECT_EQ(gl.geti(GL_DRAW_BUFFER, 1)[0], static_cast<i32>(GL_FRONT));
    gl.call("glDrawBuffer", {I{GL_NONE}});
    EXPECT_EQ(gl.geti(GL_DRAW_BUFFER, 1)[0], 0);
    // Pixel store through glPixelStoref; bad pname INVALID_ENUM.
    gl.call("glPixelStoref", {I{GL_UNPACK_ALIGNMENT}, F{3.0f}});
    EXPECT_EQ(gl.err(), GL_INVALID_VALUE);
    gl.call("glPixelStoref", {I{GL_PACK_ROW_LENGTH}, F{-1.0f}});
    EXPECT_EQ(gl.err(), GL_INVALID_VALUE);
    gl.call("glPixelStoref", {I{0x1234}, F{1.0f}});
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);
    gl.call("glGetDoublev", {I{0x1234}, P{out}});
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);
    // Default texture parameters (Table 6.11) and their setters.
    auto gtpi = [&](u32 target, u32 pname) {
        gl.fill(0xFF);
        gl.call("glGetTexParameteriv", {I{target}, I{pname}, P{out}});
        return gl.memory.read_u32(out).value();
    };
    EXPECT_EQ(gtpi(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER), GL_NEAREST_MIPMAP_LINEAR);
    EXPECT_EQ(gtpi(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER), GL_LINEAR);
    EXPECT_EQ(gtpi(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S), GL_REPEAT);
    EXPECT_EQ(gtpi(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER), GL_NEAREST_MIPMAP_LINEAR);
    gl.put_ints({static_cast<i32>(GL_NEAREST)});
    gl.call("glTexParameteriv", {I{GL_TEXTURE_2D}, I{GL_TEXTURE_MIN_FILTER}, P{GlExportHarness::IN}});
    EXPECT_EQ(gtpi(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER), GL_NEAREST);
    gl.put_ints({static_cast<i32>(GL_NEAREST_MIPMAP_LINEAR)});
    gl.call("glTexParameteriv", {I{GL_TEXTURE_2D}, I{GL_TEXTURE_MAG_FILTER}, P{GlExportHarness::IN}});
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);
    gl.put_floats({0.25f, 0.5f, 0.75f, 2.0f});
    gl.call("glTexParameterfv", {I{GL_TEXTURE_2D}, I{GL_TEXTURE_BORDER_COLOR}, P{GlExportHarness::IN}});
    gl.call("glGetTexParameterfv", {I{GL_TEXTURE_2D}, I{GL_TEXTURE_BORDER_COLOR}, P{out}});
    EXPECT_EQ(gl.out_floats(4), (std::vector<double>{0.25, 0.5, 0.75, 1.0}));   // clamped
    EXPECT_EQ(gtpi(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR), 0x1FFFFFFFu);        // Table 4.7 INT mapping
    gl.put_floats({3.0f});
    gl.call("glTexParameterfv", {I{GL_TEXTURE_2D}, I{GL_TEXTURE_PRIORITY}, P{GlExportHarness::IN}});
    gl.call("glGetTexParameterfv", {I{GL_TEXTURE_2D}, I{GL_TEXTURE_PRIORITY}, P{out}});
    EXPECT_EQ(gl.out_floats(1)[0], 1.0);
    gl.put_floats({static_cast<float>(0x812F)});
    gl.call("glTexParameterfv", {I{GL_TEXTURE_2D}, I{GL_TEXTURE_WRAP_S}, P{GlExportHarness::IN}});
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);                                        // no CLAMP_TO_EDGE in 1.1
    gl.call("glTexParameteri", {I{GL_TEXTURE_2D}, I{GL_TEXTURE_WRAP_S}, I{GL_CLAMP}});
    EXPECT_EQ(gtpi(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S), GL_CLAMP);
    gl.call("glTexParameteriv", {I{0x1234}, I{GL_TEXTURE_MIN_FILTER}, P{GlExportHarness::IN}});
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);
    gl.call("glGetTexParameteriv", {I{GL_TEXTURE_2D}, I{0x1234}, P{out}});
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);
    // Level parameters of an unspecified level (Table 6.11).
    auto gtlpf = [&](u32 target, u32 level, u32 pname) {
        gl.put_floats({-1.0f}, out);
        gl.call("glGetTexLevelParameterfv", {I{target}, I{level}, I{pname}, P{out}});
        return bitsf(gl.memory.read_u32(out).value());
    };
    EXPECT_EQ(gtlpf(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH), 0.0f);
    EXPECT_EQ(gtlpf(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT), 1.0f);
    EXPECT_EQ(gtlpf(GL_TEXTURE_1D, 0, GL_TEXTURE_HEIGHT), 0.0f);
    EXPECT_EQ(gtlpf(GL_PROXY_TEXTURE_2D, 3, GL_TEXTURE_WIDTH), 0.0f);
    EXPECT_EQ(gl.err(), 0u);
}

INSTANTIATE_TEST_SUITE_P(Bitness, GlContextStateTest, ::testing::Values(false, true), bitness_name);
INSTANTIATE_TEST_SUITE_P(Bitness, GlNoContextTest, ::testing::Values(false, true), bitness_name);

}  // namespace
