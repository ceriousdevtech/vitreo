/// @file test_gl_display_list.cpp
/// @brief OpenGL 1.1 display lists (spec 5.4) through the registered opengl32
///        exports, in both bitnesses: what a list records (decoded values and
///        client memory copied at compile time), the commands that are never
///        compiled, GL_COMPILE_AND_EXECUTE, the nesting limit, glCallLists'
///        offset types with glListBase, list names, errors and sharing.
///
/// Expected values are the spec's, and where the spec leaves a value open or
/// Windows differs, the GDI Generic measurement (tools/oracle_probe/
/// gl11_state_probe64.expected, list-* tags, cited at each assertion).
/// Contexts here have no drawable, so drawing commands change only state; the
/// GPU replay test is test_gl_display_list_metal.cpp.

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <string_view>
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
#include "winapi/graphics/opengl32/gl_display_list.hpp"
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

/// A guest caller of the registered exports (x86 stdcall frame or x64
/// registers, garbage in every unused register and high dword).
class Harness {
public:
    static constexpr GuestAddr MEM = 0x10000;       // 0x10000 bytes, read/write
    static constexpr GuestAddr STACK = 0x80000;
    static constexpr GuestAddr OUT = MEM + 0x100;    // query results
    static constexpr GuestAddr DATA = MEM + 0x1000;  // client arrays, images
    static constexpr GuestAddr SCRATCH = MEM + 0x8000;

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

    u64 call(std::string_view name, std::vector<Arg> args) {
        const std::string n(name);
        auto addr = registry->get_thunk_address("opengl32.dll", n);
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

    u32 err() { return static_cast<u32>(call("glGetError", {})); }
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
    /// x of the model-view translation: the tests count executed glTranslatef(1, 0, 0).
    float mv_x() { return getf(GL_MODELVIEW_MATRIX, 16)[12]; }
    void put_floats(GuestAddr at, std::initializer_list<float> v) {
        u32 k = 0;
        for (float x : v) (void)memory.write_u32(at + 4 * k++, fbits(x));
    }
    void put_bytes(GuestAddr at, std::initializer_list<u8> v) {
        u32 k = 0;
        for (u8 x : v) (void)memory.write_u8(at + k++, x);
    }
    void fill(GuestAddr at, usize n, u8 value) {
        std::vector<u8> b(n, value);
        (void)memory.write_bytes(at, b.data(), b.size());
    }

    void new_list(u32 n, u32 mode = GL_COMPILE) { call("glNewList", {I{n}, I{mode}}); }
    void end_list() { call("glEndList", {}); }
    void call_list(u32 n) { call("glCallList", {I{n}}); }
    void translate_x(float x) { call("glTranslatef", {F{x}, F{0}, F{0}}); }
    void identity() { call("glLoadIdentity", {}); }

    GuestMemory memory;
    cpu::CpuState cpu;
    Opengl32Manager mgr;
    std::unique_ptr<ThunkRegistry> registry;

private:
    bool m_x64;
};

struct CurrentContext {
    explicit CurrentContext(std::shared_ptr<GlShareGroup> group = nullptr) : ctx(GlFramebufferConfig{}, std::move(group)) {
        ctx.on_first_make_current(16, 8);
        set_current_gl_context_for_thread(&ctx);
    }
    ~CurrentContext() { set_current_gl_context_for_thread(nullptr); }
    GlContext ctx;
};

class GlDisplayListTest : public ::testing::TestWithParam<bool> {
protected:
    Harness gl{GetParam()};
    CurrentContext current;
    GlContext& ctx = current.ctx;

    std::vector<float> color() { return gl.getf(GL_CURRENT_COLOR, 4); }
};

std::string bitness_name(const ::testing::TestParamInfo<bool>& info) { return info.param ? "x64" : "x86"; }

using Floats = std::vector<float>;

// ---- CompileCopiesClientDataAtCompileTime -----------------------------------------------

// Spec 5.4: "pointers which are passed as arguments to commands are
// dereferenced when the command is issued. Vertex array pointers are
// dereferenced when the commands ArrayElement, DrawArrays, or DrawElements
// are accumulated into a display list", and client state in effect when a
// command is compiled applies to it.
TEST_P(GlDisplayListTest, CompileCopiesClientDataAtCompileTime) {
    // Vertex arrays (list-arrayelement-dereferenced, list-drawarrays-dereferenced,
    // list-drawelements-dereferenced).
    const GuestAddr colors = Harness::DATA, verts = Harness::DATA + 0x40, idx = Harness::DATA + 0x80;
    gl.put_floats(colors, {0.25f, 0.5f, 0.75f, 1.0f, 0.125f, 0.375f, 0.625f, 0.875f});
    gl.put_floats(verts, {0, 0, 0, 1, 1, 1});
    gl.put_bytes(idx, {0});
    gl.call("glColorPointer", {I{4}, I{GL_FLOAT}, I{0}, P{colors}});
    gl.call("glVertexPointer", {I{3}, I{GL_FLOAT}, I{0}, P{verts}});
    gl.call("glEnableClientState", {I{GL_COLOR_ARRAY}});
    gl.new_list(90);
    gl.call("glBegin", {I{GL_POINTS}});
    gl.call("glArrayElement", {I{1}});
    gl.call("glEnd", {});
    gl.end_list();
    gl.new_list(91);
    gl.call("glDrawArrays", {I{GL_POINTS}, I{0}, I{2}});
    gl.end_list();
    gl.new_list(92);
    gl.call("glDrawElements", {I{GL_POINTS}, I{1}, I{GL_UNSIGNED_BYTE}, P{idx}});
    gl.end_list();
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    // Change the client state and the memory after compiling.
    gl.call("glDisableClientState", {I{GL_COLOR_ARRAY}});
    gl.fill(colors, 32, 0);
    gl.put_bytes(idx, {1});
    gl.call("glColor4f", {F{1}, F{1}, F{1}, F{1}});
    gl.call_list(90);
    EXPECT_EQ(color(), (Floats{0.125f, 0.375f, 0.625f, 0.875f}));   // list-arrayelement-dereferenced
    gl.call("glColor4f", {F{1}, F{1}, F{1}, F{1}});
    gl.call_list(91);
    EXPECT_EQ(color(), (Floats{0.125f, 0.375f, 0.625f, 0.875f}));   // list-drawarrays-dereferenced
    gl.call("glColor4f", {F{1}, F{1}, F{1}, F{1}});
    gl.call_list(92);
    EXPECT_EQ(color(), (Floats{0.25f, 0.5f, 0.75f, 1.0f}));         // list-drawelements-dereferenced
    EXPECT_EQ(gl.err(), GL_NO_ERROR);

    // A vector form's pointer and a matrix are read when compiled.
    const GuestAddr v = Harness::DATA + 0x100;
    gl.put_floats(v, {0.5f, 0.25f, 0.125f, 1.0f});
    gl.put_floats(v + 0x40, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 7, 0, 0, 1});
    gl.new_list(93);
    gl.call("glColor4fv", {P{v}});
    gl.call("glLoadMatrixf", {P{v + 0x40}});
    gl.end_list();
    gl.put_floats(v, {0, 0, 0, 0});
    gl.put_floats(v + 0x40 + 48, {99});
    gl.call_list(93);
    EXPECT_EQ(color(), (Floats{0.5f, 0.25f, 0.125f, 1.0f}));
    EXPECT_EQ(gl.mv_x(), 7.0f);
    gl.identity();

    // A pixel image is unpacked with the pixel-store state in effect when it
    // was compiled: a 2x2 RGB UNSIGNED_BYTE image under UNPACK_ALIGNMENT 1
    // (rows of 6 bytes; 8 under the default 4) replays the same texels after
    // the alignment and the client bytes change.
    const GuestAddr img = Harness::DATA + 0x200;
    std::vector<u8> texels(12);
    for (usize k = 0; k < texels.size(); ++k) texels[k] = static_cast<u8>(10 * (k + 1));
    (void)gl.memory.write_bytes(img, texels.data(), texels.size());
    gl.call("glPixelStorei", {I{GL_UNPACK_ALIGNMENT}, I{1}});
    gl.new_list(94);
    gl.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{GL_RGB}, I{2}, I{2}, I{0}, I{GL_RGB}, I{GL_UNSIGNED_BYTE}, P{img}});
    gl.end_list();
    gl.call("glPixelStorei", {I{GL_UNPACK_ALIGNMENT}, I{4}});
    gl.fill(img, 16, 0xEE);
    gl.call_list(94);
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    gl.call("glPixelStorei", {I{GL_PACK_ALIGNMENT}, I{1}});
    gl.fill(Harness::SCRATCH, 32, 0);
    gl.call("glGetTexImage", {I{GL_TEXTURE_2D}, I{0}, I{GL_RGB}, I{GL_UNSIGNED_BYTE}, P{Harness::SCRATCH}});
    std::vector<u8> back(12);
    ASSERT_TRUE(gl.memory.read_bytes(Harness::SCRATCH, back.data(), back.size()).is_ok());
    EXPECT_EQ(back, texels);
    gl.call("glPixelStorei", {I{GL_PACK_ALIGNMENT}, I{4}});

    // The polygon stipple is unpacked with the compile-time LSB_FIRST.
    std::vector<u8> stipple(128);
    for (usize k = 0; k < stipple.size(); ++k) stipple[k] = static_cast<u8>(k == 0 ? 0x01 : 0);
    (void)gl.memory.write_bytes(img, stipple.data(), stipple.size());
    gl.call("glPixelStorei", {I{GL_UNPACK_LSB_FIRST}, I{1}});
    gl.new_list(95);
    gl.call("glPolygonStipple", {P{img}});
    gl.end_list();
    gl.call("glPixelStorei", {I{GL_UNPACK_LSB_FIRST}, I{0}});
    gl.fill(img, 128, 0xFF);
    gl.call_list(95);
    EXPECT_EQ(ctx.state.polygon_stipple.rows[0], 1u);   // bit 0 = pixel 0 under LSB_FIRST
    for (usize y = 1; y < 32; ++y) EXPECT_EQ(ctx.state.polygon_stipple.rows[y], 0u) << y;

    // glCallLists' offsets are read when compiled; the base when executed
    // (list-calllists-compiled offsets-at-compile=1,2 base-at-call=200 x=6).
    for (u32 k = 0; k <= 3; ++k) {
        gl.new_list(200 + k);
        gl.translate_x(static_cast<float>(1u << k));
        gl.end_list();
    }
    gl.put_bytes(idx, {1, 2});
    gl.new_list(240);
    gl.call("glCallLists", {I{2}, I{GL_UNSIGNED_BYTE}, P{idx}});
    gl.end_list();
    gl.put_bytes(idx, {3, 3});
    gl.identity();
    gl.call("glListBase", {I{200}});
    gl.call_list(240);
    EXPECT_EQ(gl.mv_x(), 6.0f);
    gl.call("glListBase", {I{0}});
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
}

// ---- CompileAndExecuteDoesBoth -------------------------------------------------------

TEST_P(GlDisplayListTest, CompileAndExecuteDoesBoth) {
    // GL_COMPILE does not execute (list-compiled-color-not-executed).
    gl.call("glColor4f", {F{0}, F{0}, F{0}, F{0}});
    gl.new_list(1, GL_COMPILE);
    gl.call("glColor4f", {F{0.5f}, F{0.5f}, F{0.5f}, F{0.5f}});
    EXPECT_EQ(color(), (Floats{0, 0, 0, 0}));
    gl.translate_x(1);
    gl.end_list();
    EXPECT_EQ(gl.mv_x(), 0.0f);
    gl.call_list(1);
    EXPECT_EQ(color(), (Floats{0.5f, 0.5f, 0.5f, 0.5f}));
    EXPECT_EQ(gl.mv_x(), 1.0f);

    // GL_COMPILE_AND_EXECUTE executes each command as it is compiled.
    gl.identity();
    gl.new_list(2, GL_COMPILE_AND_EXECUTE);
    gl.call("glColor4f", {F{0.25f}, F{0.75f}, F{1}, F{1}});
    gl.translate_x(1);
    EXPECT_EQ(color(), (Floats{0.25f, 0.75f, 1, 1}));
    EXPECT_EQ(gl.mv_x(), 1.0f);
    gl.end_list();
    EXPECT_EQ(gl.mv_x(), 1.0f);
    gl.call_list(2);
    EXPECT_EQ(gl.mv_x(), 2.0f);

    // Errors of compiled commands are generated when they execute
    // (list-compile-error-deferred in-list=0 after-endlist=0 callist=INVALID_ENUM;
    // list-compile-and-execute-error in-list=INVALID_ENUM after-endlist=0
    // callist=INVALID_ENUM): a command that fails is still compiled.
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    gl.new_list(60, GL_COMPILE);
    gl.call("glEnable", {I{0x1234}});
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    gl.end_list();
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    gl.call_list(60);
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);
    gl.new_list(61, GL_COMPILE_AND_EXECUTE);
    gl.call("glEnable", {I{0x1234}});
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);
    gl.end_list();
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    gl.call_list(61);
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);
    // A command not allowed inside glBegin, compiled between a compiled
    // glBegin and glEnd, errors only when the list runs
    // (list-compiled-loadidentity-inside-begin after-endlist=0 callist=INVALID_OPERATION).
    gl.new_list(63, GL_COMPILE);
    gl.call("glBegin", {I{GL_POINTS}});
    gl.identity();
    gl.call("glEnd", {});
    gl.end_list();
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    gl.call_list(63);
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);
    // The same inside an executed glBegin under COMPILE_AND_EXECUTE: the
    // rectangle (refused before it reads its vector) is still compiled.
    gl.put_floats(Harness::DATA, {0, 0, 1, 1});
    gl.new_list(64, GL_COMPILE_AND_EXECUTE);
    gl.call("glBegin", {I{GL_POINTS}});
    gl.call("glRectfv", {P{Harness::DATA}, P{Harness::DATA + 8}});
    gl.call("glEnd", {});
    gl.end_list();
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);
    gl.call("glBegin", {I{GL_POINTS}});
    gl.call_list(64);   // glCallList is allowed inside glBegin; the list's glBegin is not
    gl.call("glEnd", {});
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);

    // A list is replaced only at glEndList: under COMPILE_AND_EXECUTE a call
    // of the list being defined runs its previous contents
    // (list-redefine-compile-and-execute during=11 after=640).
    gl.new_list(73);
    gl.translate_x(1);
    gl.end_list();
    gl.identity();
    gl.new_list(73, GL_COMPILE_AND_EXECUTE);
    gl.translate_x(10);
    gl.call_list(73);
    gl.end_list();
    EXPECT_EQ(gl.mv_x(), 11.0f);
    gl.identity();
    gl.call_list(73);
    EXPECT_EQ(gl.mv_x(), 640.0f);
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
}

// ---- NestingLimit64 ------------------------------------------------------------------

TEST_P(GlDisplayListTest, NestingLimit64) {
    EXPECT_EQ(gl.geti(GL_MAX_LIST_NESTING), 64);   // get-GL_MAX_LIST_NESTING
    // A list that calls itself: 64 levels execute, counting the outermost
    // glCallList; deeper calls are ignored without an error
    // (list-nesting-self-call executed-levels=64 err=0).
    gl.new_list(70);
    gl.translate_x(1);
    gl.call_list(70);
    gl.end_list();
    gl.identity();
    gl.call_list(70);
    EXPECT_EQ(gl.mv_x(), 64.0f);
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    // list-nesting-via-outer executed-levels=63
    gl.new_list(71);
    gl.call_list(70);
    gl.end_list();
    gl.identity();
    gl.call_list(71);
    EXPECT_EQ(gl.mv_x(), 63.0f);
    // list-nesting-via-calllists executed-levels=64
    gl.put_bytes(Harness::DATA, {70});
    gl.identity();
    gl.call("glCallLists", {I{1}, I{GL_UNSIGNED_BYTE}, P{Harness::DATA}});
    EXPECT_EQ(gl.mv_x(), 64.0f);
    // list-nesting-self-calllists executed-levels=64
    gl.put_bytes(Harness::DATA, {72});
    gl.new_list(72);
    gl.translate_x(1);
    gl.call("glCallLists", {I{1}, I{GL_UNSIGNED_BYTE}, P{Harness::DATA}});
    gl.end_list();
    gl.identity();
    gl.call_list(72);
    EXPECT_EQ(gl.mv_x(), 64.0f);
    // The depth unwinds: a later call has the whole budget again.
    gl.identity();
    gl.call_list(70);
    EXPECT_EQ(gl.mv_x(), 64.0f);
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
}

// ---- CallListsTypesAndListBase -------------------------------------------------------

TEST_P(GlDisplayListTest, CallListsTypesAndListBase) {
    // List 200 + k translates x by 2^k, so x names the lists that ran.
    for (u32 k = 0; k <= 20; ++k) {
        gl.new_list(200 + k);
        gl.translate_x(static_cast<float>(1u << k));
        gl.end_list();
    }
    const GuestAddr a = Harness::DATA;
    struct Case {
        const char* tag;
        u32 base;
        i32 n;
        u32 type;
        std::vector<u8> bytes;
        float x;
    };
    auto le32 = [](u32 v) {
        return std::vector<u8>{static_cast<u8>(v), static_cast<u8>(v >> 8), static_cast<u8>(v >> 16),
                               static_cast<u8>(v >> 24)};
    };
    auto f32 = [&](float f) { return le32(fbits(f)); };
    const Case cases[] = {
        {"list-calllists-ubyte-1-2-3", 200, 3, GL_UNSIGNED_BYTE, {1, 2, 3}, 14},
        {"list-calllists-byte-minus1-base205", 205, 1, GL_BYTE, {0xFF}, 16},
        {"list-calllists-short-6-minus1-base200", 200, 2, GL_SHORT, {6, 0, 0xFF, 0xFF}, 64},
        {"list-calllists-ushort-7", 200, 1, GL_UNSIGNED_SHORT, {7, 0}, 128},
        {"list-calllists-int-8", 200, 1, GL_INT, le32(8), 256},
        {"list-calllists-uint-9", 200, 1, GL_UNSIGNED_INT, le32(9), 512},
        {"list-calllists-float-10.7", 200, 1, GL_FLOAT, f32(10.7f), 1024},
        // GDI Generic floors GL_FLOAT offsets (the 1.1 text says truncate).
        {"list-calllists-float-minus0.5-base211", 211, 1, GL_FLOAT, f32(-0.5f), 1024},
        {"list-calllists-float-minus1.5-base212", 212, 1, GL_FLOAT, f32(-1.5f), 1024},
        {"list-calllists-float-minus0.25-base213", 213, 1, GL_FLOAT, f32(-0.25f), 4096},
        {"list-calllists-float-minus2-base216", 216, 1, GL_FLOAT, f32(-2.0f), 16384},
        {"list-calllists-float-minus0-base217", 217, 1, GL_FLOAT, f32(-0.0f), 131072},
        {"list-calllists-float-2.5", 200, 1, GL_FLOAT, f32(2.5f), 4},
        {"list-calllists-float-3.999", 200, 1, GL_FLOAT, f32(3.999f), 8},
        {"list-calllists-float-4.5", 200, 1, GL_FLOAT, f32(4.5f), 16},
        {"list-calllists-2bytes-00-0c", 200, 1, GL_2_BYTES, {0x00, 0x0C}, 4096},
        {"list-calllists-2bytes-01-02-base-wrap-to-213", static_cast<u32>(213 - 258), 1, GL_2_BYTES, {0x01, 0x02}, 8192},
        {"list-calllists-3bytes-00-00-0e", 200, 1, GL_3_BYTES, {0, 0, 14}, 16384},
        {"list-calllists-4bytes-00-00-00-0f", 200, 1, GL_4_BYTES, {0, 0, 0, 15}, 32768},
        {"list-calllists-4bytes-ff-ff-ff-ff-base217", 217, 1, GL_4_BYTES, {0xFF, 0xFF, 0xFF, 0xFF}, 65536},
        {"list-calllists-4bytes-00-00-01-00-base-wrap-to-218", static_cast<u32>(218 - 256), 1, GL_4_BYTES,
         {0x00, 0x00, 0x01, 0x00}, 262144},
        {"list-calllists-ubyte-undefined-mixed-1-99-2", 200, 3, GL_UNSIGNED_BYTE, {1, 99, 2}, 6},
        // An offset reaching list 0 is skipped silently (list-calllists-offset-to-list-zero err=0).
        {"list-calllists-offset-to-list-zero", 0, 1, GL_UNSIGNED_BYTE, {0}, 0},
    };
    for (const Case& c : cases) {
        (void)gl.memory.write_bytes(a, c.bytes.data(), c.bytes.size());
        gl.identity();
        gl.call("glListBase", {I{c.base}});
        gl.call("glCallLists", {I{static_cast<u32>(c.n)}, I{c.type}, P{a}});
        EXPECT_EQ(gl.err(), GL_NO_ERROR) << c.tag;
        EXPECT_EQ(gl.mv_x(), c.x) << c.tag;
    }
    EXPECT_EQ(gl.geti(GL_LIST_BASE), 0);   // the last case's base
    gl.call("glListBase", {I{0}});

    // Errors (list-err-*): n < 0 first, then n = 0 does nothing (even with a
    // bad type, and with a NULL pointer), then the type.
    gl.call("glCallLists", {I{1}, I{0x1234}, P{a}});
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);                       // list-err-calllists-bad-type
    gl.call("glCallLists", {I{static_cast<u32>(-1)}, I{0x1234}, P{0}});
    EXPECT_EQ(gl.err(), GL_INVALID_VALUE);                      // list-err-calllists-neg-bad-type
    gl.call("glCallLists", {I{static_cast<u32>(-1)}, I{GL_UNSIGNED_BYTE}, P{0}});
    EXPECT_EQ(gl.err(), GL_INVALID_VALUE);                      // err-neg-size-glCallLists
    gl.call("glCallLists", {I{0}, I{0x1234}, P{0}});
    EXPECT_EQ(gl.err(), GL_NO_ERROR);                           // list-err-calllists-zero-bad-type
    gl.call("glCallLists", {I{0}, I{GL_UNSIGNED_BYTE}, P{0}});
    EXPECT_EQ(gl.err(), GL_NO_ERROR);                           // list-err-calllists-zero-null
    // glCallList(0) is INVALID_VALUE (spec 5.4; list-err-calllist-zero), an
    // undefined list does nothing (err-glCallList-undefined).
    gl.call_list(0);
    EXPECT_EQ(gl.err(), GL_INVALID_VALUE);
    gl.call_list(0x7FFFFF);
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    // Both are allowed inside glBegin (spec 2.6.3; list-err-calllist(s)-inside-begin err=0).
    gl.put_bytes(a, {50});
    gl.call("glBegin", {I{GL_POINTS}});
    gl.call_list(50);
    gl.call("glCallLists", {I{1}, I{GL_UNSIGNED_BYTE}, P{a}});
    gl.call("glEnd", {});
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    // Compiled, their errors appear when the list runs
    // (list-calllist-zero-compiled after-endlist=0 callist=INVALID_VALUE;
    // list-calllists-errors-compiled after-endlist=0 callist=INVALID_VALUE).
    gl.new_list(105);
    gl.call_list(0);
    gl.end_list();
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    gl.call_list(105);
    EXPECT_EQ(gl.err(), GL_INVALID_VALUE);
    gl.new_list(106);
    gl.call("glCallLists", {I{static_cast<u32>(-1)}, I{GL_UNSIGNED_BYTE}, P{a}});
    gl.call("glCallLists", {I{1}, I{0x1234}, P{a}});
    gl.end_list();
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    gl.call_list(106);
    EXPECT_EQ(gl.err(), GL_INVALID_VALUE);
    gl.call("glCallLists", {I{1}, I{0x1234}, P{a}});   // clear: the second error of 106 was dropped
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);

    // glListBase is compiled (list-compiled-listbase after-endlist=0 after-callist=7)
    // and GL_LIST_BIT saves it.
    gl.new_list(84);
    gl.call("glListBase", {I{7}});
    gl.end_list();
    EXPECT_EQ(gl.geti(GL_LIST_BASE), 0);
    gl.call_list(84);
    EXPECT_EQ(gl.geti(GL_LIST_BASE), 7);
    gl.call("glPushAttrib", {I{GL_LIST_BIT}});
    gl.call("glListBase", {I{9}});
    gl.call("glPopAttrib", {});
    EXPECT_EQ(gl.geti(GL_LIST_BASE), 7);
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
}

// ---- List names and list command errors ------------------------------------------------

TEST_P(GlDisplayListTest, ListNamesFollowTheOracle) {
    // list-genlists-sequence gen1=1 gen3=2 after-newlist20-gen2=5 after-delete2-gen1=2
    // gen2=7 gen15=21 islist20=1 islist1=1: first fit from 1.
    EXPECT_EQ(gl.call("glGenLists", {I{1}}), 1u);
    EXPECT_EQ(gl.call("glGenLists", {I{3}}), 2u);
    gl.new_list(20);
    gl.end_list();
    EXPECT_EQ(gl.call("glGenLists", {I{2}}), 5u);
    gl.call("glDeleteLists", {I{2}, I{1}});
    EXPECT_EQ(gl.call("glGenLists", {I{1}}), 2u);
    EXPECT_EQ(gl.call("glGenLists", {I{2}}), 7u);
    EXPECT_EQ(gl.call("glGenLists", {I{15}}), 21u);
    EXPECT_EQ(gl.call("glIsList", {I{20}}), 1u);
    EXPECT_EQ(gl.call("glIsList", {I{1}}), 1u);   // a generated name is an (empty) list
    EXPECT_EQ(gl.call("glIsList", {I{36}}), 0u);
    // list-deletelists-1-100 islist1=0 islist20=0 islist40=0; list-genlists-after-delete-all gen2=1.
    gl.call("glDeleteLists", {I{1}, I{100}});
    EXPECT_EQ(gl.call("glIsList", {I{1}}), 0u);
    EXPECT_EQ(gl.call("glIsList", {I{20}}), 0u);
    EXPECT_EQ(gl.call("glIsList", {I{35}}), 0u);
    EXPECT_EQ(gl.call("glGenLists", {I{2}}), 1u);
    gl.call("glDeleteLists", {I{1}, I{10}});
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    // glGenLists(0) returns 0 without an error (err-glGenLists-zero); a
    // negative count is INVALID_VALUE (err-neg-size-glGenLists).
    EXPECT_EQ(gl.call("glGenLists", {I{0}}), 0u);
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    EXPECT_EQ(gl.call("glGenLists", {I{static_cast<u32>(-1)}}), 0u);
    EXPECT_EQ(gl.err(), GL_INVALID_VALUE);
    // A huge block is a range of names, not one entry per name.
    EXPECT_EQ(gl.call("glGenLists", {I{0x40000000u}}), 1u);
    EXPECT_EQ(gl.call("glIsList", {I{0x3FFFFFFFu}}), 1u);
    EXPECT_EQ(gl.call("glIsList", {I{0x40000001u}}), 0u);
    gl.call("glDeleteLists", {I{0x100}, I{0x10}});
    EXPECT_EQ(gl.call("glIsList", {I{0x0FF}}), 1u);
    EXPECT_EQ(gl.call("glIsList", {I{0x100}}), 0u);
    EXPECT_EQ(gl.call("glIsList", {I{0x10F}}), 0u);
    EXPECT_EQ(gl.call("glIsList", {I{0x110}}), 1u);
    EXPECT_EQ(gl.call("glGenLists", {I{0x10}}), 0x100u);
    // Names run out at 2^32 - 1.
    EXPECT_EQ(gl.call("glGenLists", {I{0x7FFFFFFFu}}), 0x40000001u);
    EXPECT_EQ(gl.call("glGenLists", {I{0x7FFFFFFFu}}), 0u);
    EXPECT_EQ(gl.call("glGenLists", {I{0x40000000u}}), 0xC0000000u);
    EXPECT_EQ(gl.call("glGenLists", {I{1}}), 0u);
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    EXPECT_EQ(gl.call("glIsList", {I{0xFFFFFFFFu}}), 1u);
    gl.call("glDeleteLists", {I{1}, I{0x7FFFFFFF}});
    gl.call("glDeleteLists", {I{0x80000000u}, I{0x7FFFFFFF}});
    EXPECT_EQ(gl.call("glIsList", {I{0x3FFFFFFFu}}), 0u);
    EXPECT_EQ(gl.call("glIsList", {I{0xFFFFFFFEu}}), 0u);
    EXPECT_EQ(gl.call("glIsList", {I{0xFFFFFFFFu}}), 1u);
    gl.call("glDeleteLists", {I{0xFFFFFFFFu}, I{1}});
    EXPECT_TRUE(ctx.share_group->lists(ctx.share_group->lock()).empty());
    // Deleting: range 0 does nothing, list 0 and names past 2^32-1 are ignored
    // (list-err-deletelists-zero-range, -from-zero, -wrap: no error);
    // err-neg-size-glDeleteLists INVALID_VALUE.
    gl.new_list(1);
    gl.end_list();
    gl.new_list(0xFFFFFFFFu);
    gl.end_list();
    gl.call("glDeleteLists", {I{1}, I{0}});
    EXPECT_EQ(gl.call("glIsList", {I{1}}), 1u);
    gl.call("glDeleteLists", {I{0}, I{3}});
    EXPECT_EQ(gl.call("glIsList", {I{1}}), 0u);
    gl.call("glDeleteLists", {I{0xFFFFFFFFu}, I{4}});
    EXPECT_EQ(gl.call("glIsList", {I{0xFFFFFFFFu}}), 0u);
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    gl.call("glDeleteLists", {I{1}, I{static_cast<u32>(-1)}});
    EXPECT_EQ(gl.err(), GL_INVALID_VALUE);
    // The list being compiled is not a name until glEndList
    // (list-genlists-during-compile-of-1 gen1=1 islist1-during=1 islist1-after=1).
    gl.new_list(1);
    EXPECT_EQ(gl.call("glGenLists", {I{1}}), 1u);
    EXPECT_EQ(gl.call("glIsList", {I{1}}), 1u);
    gl.translate_x(1);
    gl.end_list();
    gl.identity();
    gl.call_list(1);
    EXPECT_EQ(gl.mv_x(), 1.0f);
    gl.call("glDeleteLists", {I{1}, I{5}});
    // list-delete-while-compiling islist-during=0 islist-after=1 callist-x=5.
    gl.new_list(74);
    gl.translate_x(1);
    gl.end_list();
    gl.new_list(74);
    gl.translate_x(5);
    gl.call("glDeleteLists", {I{74}, I{1}});
    EXPECT_EQ(gl.call("glIsList", {I{74}}), 0u);
    gl.end_list();
    EXPECT_EQ(gl.call("glIsList", {I{74}}), 1u);
    gl.identity();
    gl.call_list(74);
    EXPECT_EQ(gl.mv_x(), 5.0f);
    // glIsList(0) is FALSE without an error (err-glIsList-zero).
    EXPECT_EQ(gl.call("glIsList", {I{0}}), 0u);
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
}

TEST_P(GlDisplayListTest, NewListAndEndListErrors) {
    // GL_LIST_INDEX / GL_LIST_MODE (list-index-mode during-index=31 during-mode=0x1301
    // after-index=0 after-mode=0x1301; list-mode-compile 0x1300 0x1300; initial 0, get-GL_LIST_MODE).
    EXPECT_EQ(gl.geti(GL_LIST_MODE), 0);
    gl.new_list(31, GL_COMPILE_AND_EXECUTE);
    EXPECT_EQ(gl.geti(GL_LIST_INDEX), 31);
    EXPECT_EQ(gl.geti(GL_LIST_MODE), static_cast<i32>(GL_COMPILE_AND_EXECUTE));
    gl.end_list();
    EXPECT_EQ(gl.geti(GL_LIST_INDEX), 0);
    EXPECT_EQ(gl.geti(GL_LIST_MODE), static_cast<i32>(GL_COMPILE_AND_EXECUTE));
    gl.new_list(31, GL_COMPILE);
    EXPECT_EQ(gl.geti(GL_LIST_MODE), static_cast<i32>(GL_COMPILE));
    gl.end_list();
    EXPECT_EQ(gl.geti(GL_LIST_MODE), static_cast<i32>(GL_COMPILE));
    EXPECT_EQ(gl.err(), GL_NO_ERROR);

    // glNewList checks glBegin, then the mode, then an open list, then n = 0.
    gl.new_list(0);
    EXPECT_EQ(gl.err(), GL_INVALID_VALUE);                   // err-bad-value-glNewList-zero
    gl.new_list(0, 0x1234);
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);                    // list-err-newlist-zero-bad-mode
    gl.new_list(902, 0x1234);
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);                    // err-glNewList-bad-mode
    EXPECT_EQ(gl.geti(GL_LIST_INDEX), 0);
    gl.end_list();
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);               // err-glEndList-without-glNewList
    gl.call("glBegin", {I{GL_POINTS}});
    gl.new_list(0, 0x1234);
    gl.call("glEnd", {});
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);               // list-err-newlist-zero-bad-mode-inside-begin
    gl.call("glBegin", {I{GL_POINTS}});
    gl.new_list(1);
    gl.call("glEnd", {});
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);               // err-glNewList-inside-glBegin
    EXPECT_EQ(gl.geti(GL_LIST_INDEX), 0);
    // While a list is open: a second glNewList fails and the first keeps
    // compiling (err-glNewList-nested; list-err-newlist-zero-while-compiling
    // INVALID_OPERATION; list-err-newlist-bad-mode-while-compiling and
    // list-newlist-errors-while-compiling INVALID_ENUM).
    gl.new_list(50);
    gl.translate_x(1);
    gl.new_list(51);
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);
    gl.new_list(0);
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);
    gl.new_list(51, 0x1234);
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);
    gl.new_list(0, 0x1234);
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);
    EXPECT_EQ(gl.geti(GL_LIST_INDEX), 50);
    gl.translate_x(1);
    gl.end_list();
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    EXPECT_EQ(gl.call("glIsList", {I{51}}), 0u);
    gl.identity();
    gl.call_list(50);
    EXPECT_EQ(gl.mv_x(), 2.0f);
    // glEndList inside an executed glBegin fails and the list stays open
    // (list-endlist-inside-executed-begin err-after-glEnd=INVALID_OPERATION
    // index-after-glEnd=104 second-endlist-err=0 islist104=1).
    gl.new_list(104, GL_COMPILE_AND_EXECUTE);
    gl.call("glBegin", {I{GL_POINTS}});
    gl.end_list();
    gl.call("glEnd", {});
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);
    EXPECT_EQ(gl.geti(GL_LIST_INDEX), 104);
    gl.end_list();
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    EXPECT_EQ(gl.call("glIsList", {I{104}}), 1u);
    // A compiled (not executed) glBegin does not open a primitive
    // (list-endlist-after-compiled-begin endlist-err=0 index-after=0 islist53=1).
    gl.new_list(53);
    gl.call("glBegin", {I{GL_POINTS}});
    gl.end_list();
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    EXPECT_EQ(gl.geti(GL_LIST_INDEX), 0);
    EXPECT_EQ(gl.call("glIsList", {I{53}}), 1u);
    // The other list commands are refused inside glBegin (list-err-*-inside-begin).
    for (const char* name : {"glGenLists", "glDeleteLists", "glIsList", "glListBase", "glEndList"}) {
        gl.call("glBegin", {I{GL_POINTS}});
        const u64 r = name == std::string_view("glDeleteLists") ? gl.call(name, {I{1}, I{1}})
                      : name == std::string_view("glEndList")   ? gl.call(name, {})
                                                                : gl.call(name, {I{1}});
        gl.call("glEnd", {});
        EXPECT_EQ(gl.err(), GL_INVALID_OPERATION) << name;
        if (name != std::string_view("glDeleteLists") && name != std::string_view("glEndList")) EXPECT_EQ(r, 0u) << name;
    }
    gl.call("glBegin", {I{GL_POINTS}});
    gl.call("glGenLists", {I{static_cast<u32>(-1)}});
    gl.call("glEnd", {});
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);   // list-err-genlists-neg-inside-begin
    EXPECT_EQ(gl.geti(GL_LIST_BASE), 0);
}

// ---- ListsSharedAcrossSharedContexts --------------------------------------------------

TEST_P(GlDisplayListTest, ListsSharedAcrossSharedContexts) {
    // A second context in the same share group (wglShareLists, Task 5).
    GlContext other(GlFramebufferConfig{}, ctx.share_group);
    auto on_other = [&](auto&& fn) {
        set_current_gl_context_for_thread(&other);
        fn();
        set_current_gl_context_for_thread(&ctx);
    };
    // list-shared share=1 islist-in-b=1 callist-in-b-x=3 gen-a=1 gen-b=2
    // islist-in-a-after-b-delete=0.
    gl.new_list(5);
    gl.translate_x(3);
    gl.end_list();
    EXPECT_EQ(gl.call("glGenLists", {I{1}}), 1u);
    on_other([&] {
        EXPECT_EQ(gl.call("glIsList", {I{5}}), 1u);
        gl.identity();
        gl.call_list(5);
        EXPECT_EQ(gl.mv_x(), 3.0f);
        EXPECT_EQ(gl.call("glGenLists", {I{1}}), 2u);
        gl.call("glDeleteLists", {I{5}, I{1}});
    });
    EXPECT_EQ(gl.call("glIsList", {I{5}}), 0u);
    // A list being compiled in one context is not visible in the other until glEndList.
    gl.new_list(6);
    gl.translate_x(2);
    on_other([&] { EXPECT_EQ(gl.call("glIsList", {I{6}}), 0u); });
    gl.end_list();
    on_other([&] {
        EXPECT_EQ(gl.call("glIsList", {I{6}}), 1u);
        gl.identity();
        gl.call_list(6);
        EXPECT_EQ(gl.mv_x(), 2.0f);
    });
    // wglShareLists counts lists as objects (share-after-gentextures-dst).
    EXPECT_TRUE(ctx.share_group->has_objects());
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
}

// Spec 5.4: "glDeleteLists of a list executing on another thread is safe":
// one thread replays lists in a loop while another deletes and redefines
// them; the replaying thread holds the list it runs, so every replay sees a
// whole list (an old or a new one) and nothing is freed under it.
TEST_P(GlDisplayListTest, DeletingAListExecutingOnAnotherThreadIsSafe) {
    constexpr u32 kLists = 8;
    constexpr u32 kCommands = 400;
    auto define = [&](u32 n) {
        gl.new_list(n);
        for (u32 k = 0; k < kCommands; ++k) gl.translate_x(1);
        gl.end_list();
    };
    for (u32 n = 1; n <= kLists; ++n) define(n);
    std::atomic<bool> stop{false};
    std::atomic<u32> partial{0}, replays{0};
    std::thread runner([&] {
        GlContext other(GlFramebufferConfig{}, ctx.share_group);
        set_current_gl_context_for_thread(&other);
        while (!stop.load()) {
            for (u32 n = 1; n <= kLists; ++n) {
                cmd::LoadIdentity(other);
                cmd::CallList(other, n);
                const float x = static_cast<float>(other.modelview()[12]);
                if (x != 0.0f && x != static_cast<float>(kCommands)) partial.fetch_add(1);
                replays.fetch_add(1);
            }
        }
        set_current_gl_context_for_thread(nullptr);
    });
    for (u32 round = 0; round < 300; ++round) {
        const u32 n = 1 + round % kLists;
        gl.call("glDeleteLists", {I{n}, I{1}});
        define(n);
    }
    while (replays.load() < 200) std::this_thread::yield();
    stop.store(true);
    runner.join();
    EXPECT_EQ(partial.load(), 0u);   // each replay ran a whole list or nothing
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
}

// ---- CommandsNotCompiledExecuteImmediately (table-driven) ---------------------------------

struct ExportRow {
    const char* name;
    const char* kinds;
};
#define GL11_EXPORT(name, ordinal, bytes, ret, kinds) {#name, kinds},
#define GL11_OTHER_EXPORT(name, ordinal, bytes)
const ExportRow kExports[] = {
#include "winapi/graphics/opengl32/gl11_exports.inc"
};
#undef GL11_EXPORT
#undef GL11_OTHER_EXPORT

/// Plausible arguments for any export: small integers, 0.5, and a pointer
/// to readable, writable zeroed memory.
std::vector<Arg> plausible_args(const char* kinds, u32 first_int = 2) {
    std::vector<Arg> args;
    for (const char* k = kinds; *k != '\0'; ++k) {
        switch (*k) {
        case 'f': args.push_back(F{0.5f}); break;
        case 'd': args.push_back(D{0.5}); break;
        case 'p': args.push_back(P{Harness::SCRATCH}); break;
        default: args.push_back(I{args.empty() ? first_int : 2u}); break;
        }
    }
    return args;
}

usize compiled_commands(GlContext& ctx) { return ctx.compiling_list ? ctx.compiling_list->size() : 0; }

TEST_P(GlDisplayListTest, CommandsNotCompiledExecuteImmediately) {
    // The table is the spec 5.4 list; every glGet*/glIs* export is in it, and
    // every entry is a real export.
    std::set<std::string_view> table;
    for (const GlNotCompiled& e : gl_not_compiled_commands()) {
        if (e.proxy_target == 0) table.insert(e.name);
        bool exists = false;
        for (const ExportRow& row : kExports) exists = exists || e.name == row.name;
        EXPECT_TRUE(exists) << e.name;
    }
    for (const ExportRow& row : kExports) {
        const std::string_view n = row.name;
        if (n.substr(0, 5) == "glGet" || n.substr(0, 4) == "glIs" || n.substr(0, 5) == "glAre") {
            EXPECT_EQ(table.count(n), 1u) << n << " is a query: spec 5.4 executes it immediately";
        }
    }
    const char* const kSpec54[] = {
        "glIsList", "glGenLists", "glDeleteLists", "glFeedbackBuffer", "glSelectBuffer", "glRenderMode",
        "glVertexPointer", "glNormalPointer", "glColorPointer", "glIndexPointer", "glTexCoordPointer",
        "glEdgeFlagPointer", "glInterleavedArrays", "glEnableClientState", "glDisableClientState",
        "glPushClientAttrib", "glPopClientAttrib", "glReadPixels", "glPixelStoref", "glPixelStorei",
        "glGenTextures", "glDeleteTextures", "glAreTexturesResident", "glIsTexture", "glFlush", "glFinish",
        "glIsEnabled",
    };
    for (const char* name : kSpec54) EXPECT_EQ(table.count(name), 1u) << name;

    // Every export, called while GL_COMPILE is open: a not-compiled one adds
    // nothing to the list, every other one adds exactly one command (so no
    // compilable handler bypasses the recording seam).
    u32 immediate = 0, compiled = 0;
    for (const ExportRow& row : kExports) {
        const std::string_view name = row.name;
        if (name == "glNewList" || name == "glEndList") continue;   // the list commands themselves
        gl.fill(Harness::SCRATCH, 0x4000, 0);
        gl.new_list(1000);
        ASSERT_TRUE(ctx.compiling_list) << name;
        gl.call(name, plausible_args(row.kinds));
        const usize added = compiled_commands(ctx);
        gl.end_list();
        // Nothing compiled may have executed: no primitive is open, the list closed.
        EXPECT_FALSE(ctx.inside_begin_end()) << name << " executed while compiling";
        if (ctx.inside_begin_end()) gl.call("glEnd", {});
        EXPECT_FALSE(ctx.compiling_list) << name;
        for (int k = 0; k < 4 && gl.err() != GL_NO_ERROR; ++k) {}
        if (table.count(name) != 0) {
            EXPECT_EQ(added, 0u) << name << " was compiled";
            ++immediate;
        } else {
            EXPECT_EQ(added, 1u) << name << " was not compiled exactly once";
            ++compiled;
        }
        gl.call("glDisableClientState", {I{GL_VERTEX_ARRAY}});
        gl.call("glRenderMode", {I{GL_RENDER}});
    }
    EXPECT_EQ(immediate, table.size() - 2);   // minus glNewList, glEndList
    EXPECT_GT(compiled, 250u);
    // The proxy rule: TexImage1D/2D are executed for the proxy target only
    // (list-not-compiled-proxy-teximage proxy-width-after-endlist=64
    // texture2d-width-before=0 after-endlist=0 after-callist=16).
    gl.new_list(82);
    gl.call("glTexImage2D", {I{GL_PROXY_TEXTURE_2D}, I{0}, I{GL_RGBA}, I{64}, I{32}, I{0}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE}, P{0}});
    gl.call("glTexImage1D", {I{GL_PROXY_TEXTURE_1D}, I{0}, I{GL_RGBA}, I{8}, I{0}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE}, P{0}});
    EXPECT_EQ(compiled_commands(ctx), 0u);
    gl.call("glTexImage2D", {I{GL_TEXTURE_2D}, I{0}, I{GL_RGBA}, I{16}, I{8}, I{0}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE}, P{0}});
    gl.call("glTexImage1D", {I{GL_TEXTURE_1D}, I{0}, I{GL_RGBA}, I{4}, I{0}, I{GL_RGBA}, I{GL_UNSIGNED_BYTE}, P{0}});
    EXPECT_EQ(compiled_commands(ctx), 2u);
    gl.end_list();
    auto level_width = [&](u32 target) {
        gl.call("glGetTexLevelParameteriv", {I{target}, I{0}, I{GL_TEXTURE_WIDTH}, P{Harness::OUT}});
        i32 v = 0;
        EXPECT_TRUE(gl.memory.read_bytes(Harness::OUT, reinterpret_cast<Byte*>(&v), 4).is_ok());
        return v;
    };
    EXPECT_EQ(level_width(GL_PROXY_TEXTURE_2D), 64);
    EXPECT_EQ(level_width(GL_PROXY_TEXTURE_1D), 8);
    EXPECT_EQ(level_width(GL_TEXTURE_2D), 0);
    gl.call_list(82);
    EXPECT_EQ(level_width(GL_TEXTURE_2D), 16);
    EXPECT_EQ(level_width(GL_TEXTURE_1D), 4);
    EXPECT_EQ(gl.err(), GL_NO_ERROR);

    // Spot checks of the immediate effect, against the oracle.
    // list-pixelstore-compile in-list=1 after-endlist=1 after-callist=1 (spec:
    // executed at once; see gl_display_list.cpp for the batched case).
    gl.new_list(100);
    gl.call("glPixelStorei", {I{GL_UNPACK_ALIGNMENT}, I{1}});
    EXPECT_EQ(gl.geti(GL_UNPACK_ALIGNMENT), 1);
    gl.call("glPixelStoref", {I{GL_UNPACK_ROW_LENGTH}, F{3.0f}});
    gl.end_list();
    EXPECT_EQ(gl.geti(GL_UNPACK_ROW_LENGTH), 3);
    gl.call("glPixelStorei", {I{GL_UNPACK_ALIGNMENT}, I{4}});
    gl.call_list(100);
    EXPECT_EQ(gl.geti(GL_UNPACK_ALIGNMENT), 4);
    gl.call("glPixelStorei", {I{GL_UNPACK_ROW_LENGTH}, I{0}});
    // list-not-compiled-enableclientstate after-endlist=1 after-callist=0.
    gl.new_list(81);
    gl.call("glEnableClientState", {I{GL_NORMAL_ARRAY}});
    gl.end_list();
    EXPECT_EQ(gl.call("glIsEnabled", {I{GL_NORMAL_ARRAY}}), 1u);
    gl.call("glDisableClientState", {I{GL_NORMAL_ARRAY}});
    gl.call_list(81);
    EXPECT_EQ(gl.call("glIsEnabled", {I{GL_NORMAL_ARRAY}}), 0u);
    // list-not-compiled-vertexpointer-pushclientattrib pointer-set=1 client-depth-in-list=1.
    const i32 depth = gl.geti(GL_CLIENT_ATTRIB_STACK_DEPTH);   // the loop above left one push
    gl.new_list(102);
    gl.call("glVertexPointer", {I{3}, I{GL_FLOAT}, I{0}, P{Harness::DATA}});
    gl.call("glPushClientAttrib", {I{GL_CLIENT_PIXEL_STORE_BIT}});
    EXPECT_EQ(gl.geti(GL_CLIENT_ATTRIB_STACK_DEPTH), depth + 1);
    gl.call("glPopClientAttrib", {});
    gl.end_list();
    EXPECT_EQ(ctx.state.vertex_array.vertex.pointer, Harness::DATA);
    // list-not-compiled-selectbuffer pointer-set=1.
    gl.new_list(103);
    gl.call("glSelectBuffer", {I{4}, P{Harness::DATA}});
    gl.end_list();
    EXPECT_EQ(ctx.selection_buffer, Harness::DATA);
    // list-not-compiled-gentextures names=1,2 istexture-first=0.
    gl.new_list(83);
    gl.call("glGenTextures", {I{2}, P{Harness::OUT}});
    gl.end_list();
    u32 names[2] = {};
    ASSERT_TRUE(gl.memory.read_bytes(Harness::OUT, reinterpret_cast<Byte*>(names), 8).is_ok());
    EXPECT_NE(names[0], 0u);
    EXPECT_NE(names[1], names[0]);
    EXPECT_EQ(gl.call("glIsTexture", {I{names[0]}}), 0u);
    // list-geterror-not-compiled pending-read-in-list=INVALID_ENUM callist=0.
    gl.call("glEnable", {I{0x1234}});
    gl.new_list(62);
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);
    gl.end_list();
    gl.call_list(62);
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    // Queries answer while compiling (err-glGetIntegerv-inside-glNewList).
    gl.new_list(905);
    EXPECT_EQ(gl.geti(GL_LIST_INDEX), 905);
    gl.end_list();
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
}

// A list of 10,000 immediate-mode vertices records one command per call and
// replays them through the same command functions, so replayed inside the
// caller's glBegin (glCallList is allowed there, spec 2.6.3) the list's
// glBegin fails and its vertices and glEnd act on the open primitive.
TEST_P(GlDisplayListTest, ReplayRunsTheRecordedCommandsInOrder) {
    gl.new_list(1);
    gl.call("glBegin", {I{GL_POINTS}});
    for (u32 k = 0; k < 10000; ++k) gl.call("glVertex2i", {I{k}, I{1}});
    gl.call("glEnd", {});
    ASSERT_EQ(compiled_commands(ctx), 10002u);
    gl.end_list();
    gl.call("glBegin", {I{GL_LINES}});
    gl.call_list(1);
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);   // the list's glBegin inside glBegin
    // ...and its glEnd closed the caller's primitive with 10,000 vertices.
    EXPECT_FALSE(ctx.inside_begin_end());
    gl.call("glEnd", {});
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);
}

INSTANTIATE_TEST_SUITE_P(Bitness, GlDisplayListTest, ::testing::Values(false, true), bitness_name);

}  // namespace
