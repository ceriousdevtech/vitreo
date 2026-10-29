/// @file test_gl_assembler.cpp
/// @brief OpenGL 1.1 vertex assembly on the CPU: primitive conversion of the
///        ten glBegin modes into point/line/triangle lists with GL's
///        provoking vertex first (spec 2.6.1, 2.13.7 Table 2.9), incomplete
///        primitives, polygon edge flags (2.6.2), the begin/end command rule
///        (2.6.3) over the whole export table, and client-array validation
///        (2.8) in both bitnesses.
///
/// Expected index lists are written from the spec's vertex numbering; never
/// from a run. No backend device is attached: draws assemble and validate but
/// record nothing (the pixel tests are test_gl_draw.cpp).

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/graphics/opengl32/gl_assembler.hpp"
#include "winapi/graphics/opengl32/gl_commands.hpp"
#include "winapi/graphics/opengl32/gl_context.hpp"
#include "winapi/graphics/opengl32/opengl32.hpp"

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::gl;

namespace {

// ---- Assembler -----------------------------------------------------------------

std::vector<GlBatchVertex> numbered(u32 n) {
    std::vector<GlBatchVertex> v(n);
    for (u32 i = 0; i < n; ++i) {
        v[i] = GlBatchVertex{};
        v[i].pos[0] = static_cast<float>(i);
        v[i].pos[3] = 1.0f;
    }
    return v;
}

struct Emitted {
    GlPrimitiveClass cls;
    std::vector<u32> indices;
    std::vector<u8> edges;
};

Emitted emit(u32 mode, u32 n, std::vector<u8> flags = {}) {
    GlAssembler a;
    const auto v = numbered(n);
    a.emit(mode, v, flags);
    Emitted e;
    e.cls = a.primitive_class();
    e.indices.assign(a.indices().begin(), a.indices().end());
    e.edges.assign(a.triangle_edges().begin(), a.triangle_edges().end());
    return e;
}

using U = std::vector<u32>;

// Spec 2.6.1 (vertex numbering of each primitive) and Table 2.9 (the vertex
// whose colour a flat-shaded primitive takes: the last vertex of each point,
// segment, triangle and quadrilateral; vertex 1 of a polygon). Every emitted
// primitive lists that provoking vertex first, which is the vertex Metal's
// [[flat]] uses. Triangles keep their winding (a rotation of the spec order);
// segments run provoking-first, i.e. reversed.
TEST(GlAssemblerTest, QuadsStripsPolygonsFansLoopsBecomeLists) {
    struct Case {
        u32 mode;
        u32 n;
        GlPrimitiveClass cls;
        U indices;
    };
    const std::vector<Case> cases = {
        // POINTS: point i is vertex i.
        {GL_POINTS, 3, GlPrimitiveClass::Points, U{0, 1, 2}},
        // LINES: segment i is vertices 2i-1, 2i (1-based); provoking 2i.
        {GL_LINES, 4, GlPrimitiveClass::Lines, U{1, 0, 3, 2}},
        // LINE_STRIP: segment i is i, i+1; provoking i+1.
        {GL_LINE_STRIP, 4, GlPrimitiveClass::Lines, U{1, 0, 2, 1, 3, 2}},
        // LINE_LOOP: the strip plus n -> 1, whose provoking vertex is 1.
        {GL_LINE_LOOP, 4, GlPrimitiveClass::Lines, U{1, 0, 2, 1, 3, 2, 0, 3}},
        // TRIANGLES: triangle i is 3i-2, 3i-1, 3i; provoking 3i.
        {GL_TRIANGLES, 6, GlPrimitiveClass::Triangles, U{2, 0, 1, 5, 3, 4}},
        // TRIANGLE_STRIP: i, i+1, i+2 for odd i, i+1, i, i+2 for even i
        // (1-based); provoking i+2.
        {GL_TRIANGLE_STRIP, 5, GlPrimitiveClass::Triangles, U{2, 0, 1, 3, 2, 1, 4, 2, 3}},
        // TRIANGLE_FAN: 1, i+1, i+2; provoking i+2.
        {GL_TRIANGLE_FAN, 5, GlPrimitiveClass::Triangles, U{2, 0, 1, 3, 0, 2, 4, 0, 3}},
        // QUADS: quad i is 4i-3..4i; provoking 4i. Split along the diagonal
        // through the provoking vertex: (4i, 4i-3, 4i-2), (4i, 4i-2, 4i-1).
        {GL_QUADS, 8, GlPrimitiveClass::Triangles, U{3, 0, 1, 3, 1, 2, 7, 4, 5, 7, 5, 6}},
        // QUAD_STRIP: quad i is 2i-1, 2i, 2i+2, 2i+1 in boundary order;
        // provoking 2i+2.
        {GL_QUAD_STRIP, 6, GlPrimitiveClass::Triangles, U{3, 2, 0, 3, 0, 1, 5, 4, 2, 5, 2, 3}},
        // POLYGON: a fan from vertex 1, which is also the provoking vertex.
        {GL_POLYGON, 5, GlPrimitiveClass::Triangles, U{0, 1, 2, 0, 2, 3, 0, 3, 4}},
    };
    for (const Case& c : cases) {
        const Emitted e = emit(c.mode, c.n);
        EXPECT_EQ(e.cls, c.cls) << "mode " << c.mode;
        EXPECT_EQ(e.indices, c.indices) << "mode " << c.mode;
    }
    // A two-vertex loop is two segments: 1 -> 2 and 2 -> 1.
    EXPECT_EQ(emit(GL_LINE_LOOP, 2).indices, (U{1, 0, 0, 1}));
    EXPECT_EQ(gl_primitive_class(0x000A), GlPrimitiveClass::None);   // one past GL_POLYGON
}

// Spec 2.6.1: a primitive whose vertex count is not complete is ignored; the
// complete primitives before it are drawn.
TEST(GlAssemblerTest, IncompletePrimitivesAreDropped) {
    struct Case {
        u32 mode;
        u32 n;
        U indices;
    };
    const std::vector<Case> cases = {
        {GL_POINTS, 0, U{}},
        {GL_LINES, 1, U{}},
        {GL_LINES, 3, U{1, 0}},
        {GL_LINE_STRIP, 1, U{}},
        {GL_LINE_LOOP, 1, U{}},
        {GL_TRIANGLES, 2, U{}},
        {GL_TRIANGLES, 5, U{2, 0, 1}},
        {GL_TRIANGLE_STRIP, 2, U{}},
        {GL_TRIANGLE_FAN, 2, U{}},
        {GL_QUADS, 3, U{}},
        {GL_QUADS, 7, U{3, 0, 1, 3, 1, 2}},
        {GL_QUAD_STRIP, 3, U{}},
        {GL_QUAD_STRIP, 5, U{3, 2, 0, 3, 0, 1}},
        {GL_POLYGON, 2, U{}},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(emit(c.mode, c.n).indices, c.indices) << "mode " << c.mode << " n " << c.n;
    }
}

// Spec 2.6.2: with POLYGON, TRIANGLES or QUADS each vertex starts an edge that
// is boundary when the vertex's edge flag is TRUE; every edge of a strip or a
// fan is boundary. Edges the conversion adds inside a quadrilateral or a
// polygon are never boundary, so glPolygonMode(GL_LINE) (Task 9) draws only
// the primitive's own edges. Bit k of a triangle's mask is the edge from its
// emitted corner k to corner k+1 (mod 3).
TEST(GlAssemblerTest, EdgeFlagsMarkPolygonEdgesInLineMode) {
    // POLYGON 0..4, flags 1,0,1,1,0: edges 0-1 B, 1-2 -, 2-3 B, 3-4 B, 4-0 -.
    // Triangles (0,1,2) (0,2,3) (0,3,4).
    EXPECT_EQ(emit(GL_POLYGON, 5, {1, 0, 1, 1, 0}).edges, (std::vector<u8>{0b001, 0b010, 0b010}));
    // Every flag TRUE: the outline only, never the fan diagonals.
    EXPECT_EQ(emit(GL_POLYGON, 4, {1, 1, 1, 1}).edges, (std::vector<u8>{0b011, 0b110}));
    // TRIANGLES (2,0,1): edges 2-0 (flag of 2), 0-1 (flag of 0), 1-2 (flag of 1).
    EXPECT_EQ(emit(GL_TRIANGLES, 3, {1, 0, 0}).edges, (std::vector<u8>{0b010}));
    EXPECT_EQ(emit(GL_TRIANGLES, 3, {0, 0, 1}).edges, (std::vector<u8>{0b001}));
    EXPECT_EQ(emit(GL_TRIANGLES, 3, {0, 1, 0}).edges, (std::vector<u8>{0b100}));
    // QUADS (3,0,1) (3,1,2): edges 3-0 flag 3, 0-1 flag 0, 1-3 diagonal;
    // 3-1 diagonal, 1-2 flag 1, 2-3 flag 2.
    EXPECT_EQ(emit(GL_QUADS, 4, {1, 1, 1, 1}).edges, (std::vector<u8>{0b011, 0b110}));
    EXPECT_EQ(emit(GL_QUADS, 4, {0, 1, 0, 1}).edges, (std::vector<u8>{0b001, 0b010}));
    // No flags given: every vertex flag is TRUE (the initial value).
    EXPECT_EQ(emit(GL_TRIANGLES, 3).edges, (std::vector<u8>{0b111}));
    // Strips and fans ignore the flags: all their edges are boundary.
    EXPECT_EQ(emit(GL_TRIANGLE_STRIP, 4, {0, 0, 0, 0}).edges, (std::vector<u8>{0b111, 0b111}));
    EXPECT_EQ(emit(GL_TRIANGLE_FAN, 4, {0, 0, 0, 0}).edges, (std::vector<u8>{0b111, 0b111}));
    // QUAD_STRIP (3,2,0) (3,0,1): the outline 3-2, 2-0, 0-1, 1-3 is boundary,
    // the diagonal 0-3 is not, whatever the flags.
    EXPECT_EQ(emit(GL_QUAD_STRIP, 4, {0, 0, 0, 0}).edges, (std::vector<u8>{0b011, 0b110}));
    // Lines and points have no polygon edges.
    EXPECT_TRUE(emit(GL_LINES, 2, {0, 0}).edges.empty());
}

// ---- Export harness (real thunks, guest CPU frames) -------------------------------

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

struct I { u32 v; };
struct F { float v; };
struct D { double v; };
struct P { GuestAddr v; };
using Arg = std::variant<I, F, D, P>;

class Harness {
public:
    static constexpr GuestAddr MEM = 0x10000;       // 0x10000 bytes, read/write
    static constexpr GuestAddr STACK = 0x80000;
    static constexpr GuestAddr SCRATCH = MEM + 0x4000;
    static constexpr GuestAddr END = MEM + 0x10000;  // first unmapped byte

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
    void put_floats(GuestAddr at, std::initializer_list<float> v) {
        u32 k = 0;
        for (float x : v) (void)memory.write_u32(at + 4 * k++, fbits(x));
    }
    void put_bytes(GuestAddr at, std::initializer_list<u8> v) {
        u32 k = 0;
        for (u8 x : v) (void)memory.write_u8(at + k++, x);
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    Opengl32Manager mgr;
    std::unique_ptr<ThunkRegistry> registry;

private:
    bool m_x64;
};

struct CurrentContext {
    CurrentContext() : ctx(GlFramebufferConfig{}) {
        ctx.on_first_make_current(16, 8);
        set_current_gl_context_for_thread(&ctx);
    }
    ~CurrentContext() { set_current_gl_context_for_thread(nullptr); }
    GlContext ctx;
};

class GlVertexCommandTest : public ::testing::TestWithParam<bool> {
protected:
    Harness gl{GetParam()};
    CurrentContext current;
    GlContext& ctx = current.ctx;
};

std::string bitness_name(const ::testing::TestParamInfo<bool>& info) { return info.param ? "x64" : "x86"; }

// ---- CommandsIllegalInsideBeginEnd (Review Focus 5) --------------------------------

struct ExportRow {
    const char* name;
    u32 stack_bytes;
    const char* kinds;
};
#define GL11_EXPORT(name, ordinal, bytes, ret, kinds) {#name, bytes, kinds},
#define GL11_OTHER_EXPORT(name, ordinal, bytes)
const ExportRow kExports[] = {
#include "winapi/graphics/opengl32/gl11_exports.inc"
};
#undef GL11_EXPORT
#undef GL11_OTHER_EXPORT

/// Spec 2.6.3: "Only a subset of GL commands can be used between Begin and
/// End. Those commands are Vertex, Color, Index, Normal, TexCoord, EvalCoord,
/// EvalPoint, Material, and EdgeFlag. Also, it is acceptable to use CallList
/// or CallLists" ...; spec 2.8 adds ArrayElement.
bool allowed_inside_begin_end(std::string_view n) {
    auto starts = [&](std::string_view p) { return n.substr(0, p.size()) == p; };
    auto family = [&](std::string_view p) {
        // glColor3f, glColor4ubv, ... but not glColorMask / glColorPointer.
        return starts(p) && n.size() > p.size() && (n[p.size()] >= '1' && n[p.size()] <= '4');
    };
    if (family("glVertex") || family("glColor") || family("glNormal") || family("glTexCoord") ||
        family("glEvalCoord") || family("glEvalPoint")) {
        return true;
    }
    if (n == "glIndexd" || n == "glIndexdv" || n == "glIndexf" || n == "glIndexfv" || n == "glIndexi" ||
        n == "glIndexiv" || n == "glIndexs" || n == "glIndexsv" || n == "glIndexub" || n == "glIndexubv") {
        return true;
    }
    return starts("glMaterial") || n == "glEdgeFlag" || n == "glEdgeFlagv" || n == "glArrayElement" ||
           n == "glCallList" || n == "glCallLists" || n == "glEnd";
}

// Spec 2.6.3: "If any other GL command is executed between Begin and End, the
// error flag is set to INVALID_OPERATION and the command is ignored." Every
// export of the table that is not in the subset is called inside glBegin with
// plausible arguments (a readable, writable pointer for every pointer); each
// must record INVALID_OPERATION and change neither a queryable state value,
// the open primitive, nor client memory.
TEST_P(GlVertexCommandTest, CommandsIllegalInsideBeginEnd) {
    auto snapshot = [&] {
        std::vector<double> s;
        static const u32 kPnames[] = {
            GL_CURRENT_COLOR, GL_CURRENT_NORMAL, GL_CURRENT_TEXTURE_COORDS, GL_CURRENT_INDEX,
            GL_EDGE_FLAG, GL_MATRIX_MODE, GL_MODELVIEW_MATRIX, GL_PROJECTION_MATRIX, GL_TEXTURE_MATRIX,
            GL_MODELVIEW_STACK_DEPTH, GL_PROJECTION_STACK_DEPTH, GL_VIEWPORT, GL_DEPTH_RANGE,
            GL_SHADE_MODEL, GL_POINT_SIZE, GL_LINE_WIDTH, GL_LINE_STIPPLE_PATTERN, GL_CULL_FACE_MODE,
            GL_FRONT_FACE, GL_POLYGON_MODE, GL_SCISSOR_BOX, GL_ALPHA_TEST_FUNC, GL_STENCIL_FUNC,
            GL_DEPTH_FUNC, GL_BLEND_SRC, GL_BLEND_DST, GL_LOGIC_OP_MODE, GL_DRAW_BUFFER, GL_READ_BUFFER,
            GL_COLOR_WRITEMASK, GL_DEPTH_WRITEMASK, GL_STENCIL_WRITEMASK, GL_COLOR_CLEAR_VALUE,
            GL_DEPTH_CLEAR_VALUE, GL_STENCIL_CLEAR_VALUE, GL_ACCUM_CLEAR_VALUE, GL_INDEX_CLEAR_VALUE,
            GL_UNPACK_ALIGNMENT, GL_PACK_ALIGNMENT, GL_RED_SCALE, GL_ZOOM_X, GL_FOG_DENSITY, GL_FOG_COLOR,
            GL_LIGHT_MODEL_AMBIENT, GL_ATTRIB_STACK_DEPTH, GL_CLIENT_ATTRIB_STACK_DEPTH,
            GL_VERTEX_ARRAY_SIZE, GL_VERTEX_ARRAY_STRIDE, GL_COLOR_ARRAY_SIZE, GL_TEXTURE_BINDING_2D,
            GL_LIST_BASE, GL_LIST_INDEX, GL_NAME_STACK_DEPTH, GL_RENDER_MODE, GL_PERSPECTIVE_CORRECTION_HINT,
            GL_MAP1_GRID_SEGMENTS, GL_POLYGON_OFFSET_FACTOR, GL_PIXEL_MAP_I_TO_I_SIZE,
        };
        for (u32 p : kPnames) {
            GlQuery q;
            EXPECT_TRUE(cmd::Get(ctx, p, q)) << p;
            for (u32 k = 0; k < q.count; ++k) s.push_back(q.v[k]);
        }
        for (u32 c = 0; c < kGlCapCount; ++c) s.push_back(ctx.state.enable.test(static_cast<GlCap>(c)) ? 1 : 0);
        for (const GlClientArray* a : {&ctx.state.vertex_array.vertex, &ctx.state.vertex_array.color,
                                       &ctx.state.vertex_array.normal, &ctx.state.vertex_array.tex_coord,
                                       &ctx.state.vertex_array.index, &ctx.state.vertex_array.edge_flag}) {
            s.push_back(a->enabled ? 1 : 0);
            s.push_back(static_cast<double>(a->pointer));
        }
        {
            const GlShareGroup::Lock held = ctx.share_group->lock();
            s.push_back(static_cast<double>(ctx.share_group->textures(held).size()));
        }
        return s;
    };
    std::vector<u8> pattern(0x2000);
    for (usize k = 0; k < pattern.size(); ++k) pattern[k] = static_cast<u8>(0x40 + (k * 7) % 0x30);
    (void)gl.memory.write_bytes(Harness::SCRATCH, pattern.data(), pattern.size());

    const std::vector<double> before = snapshot();
    u32 checked = 0;
    for (const ExportRow& row : kExports) {
        const std::string_view name = row.name;
        if (allowed_inside_begin_end(name)) continue;
        // glGetError inside Begin/End is ErrorFlagIsStickyUntilRead's
        // (err-geterror-in-glBegin); it is the only way to read the flag.
        if (name == "glGetError") continue;
        std::vector<Arg> args;
        for (const char* k = row.kinds; *k != '\0'; ++k) {
            switch (*k) {
            case 'f': args.push_back(F{0.5f}); break;
            case 'd': args.push_back(D{0.5}); break;
            case 'p': args.push_back(P{Harness::SCRATCH}); break;
            default: args.push_back(I{2}); break;
            }
        }
        ASSERT_EQ(gl.err(), GL_NO_ERROR) << name;
        gl.call("glBegin", {I{GL_TRIANGLES}});
        gl.call("glVertex2i", {I{1}, I{2}});
        ASSERT_EQ(ctx.begin_mode, GL_TRIANGLES);
        gl.call(name, args);
        EXPECT_EQ(ctx.begin_mode, GL_TRIANGLES) << name << " ended the primitive";
        EXPECT_EQ(ctx.immediate_vertices.size(), 1u) << name;
        gl.call("glEnd", {});
        EXPECT_EQ(gl.err(), GL_INVALID_OPERATION) << name;
        EXPECT_EQ(snapshot(), before) << name << " changed state";
        std::vector<u8> now(pattern.size());
        ASSERT_TRUE(gl.memory.read_bytes(Harness::SCRATCH, now.data(), now.size()).is_ok());
        EXPECT_EQ(now, pattern) << name << " wrote client memory";
        ++checked;
    }
    // 336 GL exports, minus the 2.6.3 subset (and glGetError).
    EXPECT_GT(checked, 200u);
}

// ---- glBegin / glEnd / draw errors -------------------------------------------------

// Spec 2.6.1, 2.6.3, 2.8, 2.9 errors, as measured (gl11_state_probe64.expected).
TEST_P(GlVertexCommandTest, BeginEndAndDrawErrorsFollowTheSpec) {
    gl.call("glEnd", {});
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);   // err-glEnd-without-glBegin
    gl.call("glBegin", {I{0x1234}});
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);        // err-bad-enum-glBegin
    EXPECT_FALSE(ctx.inside_begin_end());
    gl.call("glBegin", {I{0x1234}});
    gl.call("glEnd", {});                        // err-bad-enum-glBegin-then-glEnd: the first error stays
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    gl.call("glBegin", {I{GL_POLYGON}});
    gl.call("glBegin", {I{GL_POINTS}});          // err-glBegin-inside-glBegin
    EXPECT_EQ(ctx.begin_mode, GL_POLYGON);
    gl.call("glEnd", {});
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);
    EXPECT_FALSE(ctx.inside_begin_end());
    // err-glVertex-outside-glBegin: no error, and nothing is added.
    gl.call("glVertex3f", {F{0}, F{0}, F{0}});
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    EXPECT_TRUE(ctx.immediate_vertices.empty());
    // err-glArrayElement-neg: no arrays enabled, nothing to read, no error.
    gl.call("glArrayElement", {I{static_cast<u32>(-1)}});
    EXPECT_EQ(gl.err(), GL_NO_ERROR);

    gl.call("glDrawArrays", {I{0x1234}, I{0}, I{1}});
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);        // err-glDrawArrays-bad-mode
    gl.call("glDrawArrays", {I{GL_POINTS}, I{0}, I{static_cast<u32>(-1)}});
    EXPECT_EQ(gl.err(), GL_INVALID_VALUE);       // err-neg-size-glDrawArrays
    gl.call("glDrawElements", {I{GL_POINTS}, I{1}, I{GL_FLOAT}, P{0}});
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);        // err-glDrawElements-bad-type
    gl.call("glDrawElements", {I{GL_TRIANGLES}, I{3}, I{GL_SHORT}, P{Harness::SCRATCH}});
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);        // only UNSIGNED_BYTE/SHORT/INT (spec 2.8)
    gl.call("glDrawElements", {I{0x000A}, I{3}, I{GL_UNSIGNED_BYTE}, P{Harness::SCRATCH}});
    EXPECT_EQ(gl.err(), GL_INVALID_ENUM);
    gl.call("glDrawElements", {I{GL_TRIANGLES}, I{static_cast<u32>(-1)}, I{GL_UNSIGNED_BYTE}, P{Harness::SCRATCH}});
    EXPECT_EQ(gl.err(), GL_INVALID_VALUE);
    // No vertex array: nothing to transfer, no error.
    gl.call("glDrawArrays", {I{GL_TRIANGLES}, I{0}, I{3}});
    EXPECT_EQ(gl.err(), GL_NO_ERROR);

    // glRect = Begin(POLYGON) ... End: not allowed inside Begin/End (2.9), and
    // the vector form reads nothing there.
    gl.call("glBegin", {I{GL_TRIANGLES}});
    gl.call("glRectiv", {P{Harness::END - 4}, P{Harness::END - 4}});
    gl.call("glEnd", {});
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);

    // A vector form whose client memory is unreadable is ignored (Windows
    // would fault in the caller): the current value stays.
    gl.call("glColor4fv", {P{Harness::END - 8}});
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    EXPECT_EQ(ctx.state.current.color, (GlVec4{1, 1, 1, 1}));
}

// Spec 2.6.2 / 2.7: every glVertex takes the current colour, normal, texture
// coordinates and edge flag at the time it is issued.
TEST_P(GlVertexCommandTest, VertexSnapshotsTheCurrentValues) {
    gl.call("glBegin", {I{GL_TRIANGLES}});
    gl.call("glColor3ub", {I{255}, I{0}, I{0}});
    gl.call("glTexCoord2i", {I{3}, I{4}});
    gl.call("glNormal3i", {I{0}, I{0}, I{0x7FFFFFFF}});
    gl.call("glEdgeFlag", {I{0}});
    gl.call("glVertex2i", {I{1}, I{2}});
    gl.call("glColor3ub", {I{0}, I{255}, I{0}});
    gl.call("glEdgeFlag", {I{1}});
    gl.call("glVertex4i", {I{5}, I{6}, I{7}, I{2}});
    ASSERT_EQ(ctx.immediate_vertices.size(), 2u);
    const GlBatchVertex& a = ctx.immediate_vertices[0];
    const GlBatchVertex& b = ctx.immediate_vertices[1];
    EXPECT_EQ((std::vector<float>{a.pos[0], a.pos[1], a.pos[2], a.pos[3]}), (std::vector<float>{1, 2, 0, 1}));
    EXPECT_EQ((std::vector<float>{a.color[0], a.color[1], a.color[2], a.color[3]}),
              (std::vector<float>{1, 0, 0, 1}));
    EXPECT_EQ((std::vector<float>{a.tex[0], a.tex[1], a.tex[2], a.tex[3]}), (std::vector<float>{3, 4, 0, 1}));
    EXPECT_EQ(a.normal[2], 1.0f);
    EXPECT_EQ((std::vector<float>{b.pos[0], b.pos[1], b.pos[2], b.pos[3]}), (std::vector<float>{5, 6, 7, 2}));
    EXPECT_EQ(b.color[1], 1.0f);
    EXPECT_EQ(ctx.immediate_edge_flags, (std::vector<u8>{0, 1}));
    gl.call("glEnd", {});
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    EXPECT_TRUE(ctx.immediate_vertices.empty());
}

// ---- Client arrays (spec 2.8) ------------------------------------------------------

// Spec 2.8: the arrays are dereferenced when ArrayElement / DrawArrays /
// DrawElements run, not when the pointer is given. ArrayElement transfers the
// enabled arrays' values as the current values (here observable without a
// drawable).
TEST_P(GlVertexCommandTest, ClientArraysReadAtDrawTime) {
    const GuestAddr colors = Harness::MEM + 0x100;
    gl.put_bytes(colors, {255, 0, 0, 255, 0, 255, 0, 255});
    gl.call("glColorPointer", {I{4}, I{GL_UNSIGNED_BYTE}, I{0}, P{colors}});
    gl.call("glEnableClientState", {I{GL_COLOR_ARRAY}});
    gl.put_bytes(colors, {0, 0, 255, 255, 51, 102, 153, 204});   // changed after the pointer
    gl.call("glArrayElement", {I{1}});
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    EXPECT_EQ(ctx.state.current.color, (GlVec4{0.2f, 0.4f, 0.6f, 0.8f}));
    gl.call("glArrayElement", {I{0}});
    EXPECT_EQ(ctx.state.current.color, (GlVec4{0, 0, 1, 1}));

    // Normals (Table 2.6 signed conversion), texture coordinates (not
    // normalised), edge flags; strides honoured.
    const GuestAddr normals = Harness::MEM + 0x200;
    for (u32 k = 0; k < 6; ++k) (void)gl.memory.write_u16(normals + 2 * k, 0);
    (void)gl.memory.write_u16(normals + 8 + 4, 0x7FFF);   // element 1 (stride 8): z = 32767
    gl.call("glNormalPointer", {I{GL_SHORT}, I{8}, P{normals}});
    gl.call("glEnableClientState", {I{GL_NORMAL_ARRAY}});
    const GuestAddr tex = Harness::MEM + 0x300;
    gl.put_floats(tex, {0.25f, 0.5f, 7, 8});
    gl.call("glTexCoordPointer", {I{2}, I{GL_FLOAT}, I{0}, P{tex}});
    gl.call("glEnableClientState", {I{GL_TEXTURE_COORD_ARRAY}});
    const GuestAddr flags = Harness::MEM + 0x400;
    gl.put_bytes(flags, {1, 0});
    gl.call("glEdgeFlagPointer", {I{0}, P{flags}});
    gl.call("glEnableClientState", {I{GL_EDGE_FLAG_ARRAY}});
    gl.call("glArrayElement", {I{1}});
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    // Table 2.6: a short c is (2c + 1) / (2^16 - 1), so 0 is not 0.
    EXPECT_EQ(ctx.state.current.normal, (GlVec3{1.0f / 65535.0f, 1.0f / 65535.0f, 1}));
    EXPECT_EQ(ctx.state.current.tex_coords, (GlVec4{7, 8, 0, 1}));
    EXPECT_FALSE(ctx.state.current.edge_flag);
    EXPECT_EQ(ctx.state.current.color, (GlVec4{0.2f, 0.4f, 0.6f, 0.8f}));

    // Inside Begin/End, ArrayElement with the vertex array enabled adds a
    // vertex built from the elements (vertex last, after the others).
    const GuestAddr verts = Harness::MEM + 0x500;
    gl.put_floats(verts, {1, 2, 3, 4, 5, 6});
    gl.call("glVertexPointer", {I{3}, I{GL_FLOAT}, I{0}, P{verts}});
    gl.call("glEnableClientState", {I{GL_VERTEX_ARRAY}});
    gl.call("glBegin", {I{GL_POINTS}});
    gl.call("glArrayElement", {I{1}});
    ASSERT_EQ(ctx.immediate_vertices.size(), 1u);
    EXPECT_EQ(ctx.immediate_vertices[0].pos[0], 4.0f);
    EXPECT_EQ(ctx.immediate_vertices[0].color[3], 0.8f);
    EXPECT_EQ(ctx.immediate_vertices[0].tex[0], 7.0f);
    EXPECT_EQ(ctx.immediate_edge_flags, (std::vector<u8>{0}));
    gl.call("glEnd", {});
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
}

// Ruling 3: the byte range a draw implies is validated before anything is
// read; a range the guest has not committed records INVALID_OPERATION and the
// command does nothing (no current value changes, no vertex).
TEST_P(GlVertexCommandTest, ClientArrayOutsideCommittedMemoryIsRejected) {
    // Two float2 vertices fit before END; the third crosses it.
    const GuestAddr verts = Harness::END - 16;
    gl.put_floats(verts, {1, 2, 3, 4});
    gl.call("glVertexPointer", {I{2}, I{GL_FLOAT}, I{0}, P{verts}});
    gl.call("glEnableClientState", {I{GL_VERTEX_ARRAY}});
    gl.call("glDrawArrays", {I{GL_POINTS}, I{0}, I{2}});
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    gl.call("glDrawArrays", {I{GL_POINTS}, I{0}, I{3}});
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);
    gl.call("glDrawArrays", {I{GL_POINTS}, I{1}, I{2}});
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);
    // Element -1 of an array at the start of the mapping is below it.
    gl.call("glVertexPointer", {I{2}, I{GL_FLOAT}, I{0}, P{Harness::MEM}});
    gl.call("glDrawArrays", {I{GL_POINTS}, I{static_cast<u32>(-1)}, I{1}});
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);
    gl.call("glVertexPointer", {I{2}, I{GL_FLOAT}, I{0}, P{verts}});
    // A colour array of the same draw that is out of range rejects it too, and
    // no current value is touched.
    const GuestAddr colors = Harness::END - 4;
    gl.call("glColorPointer", {I{4}, I{GL_UNSIGNED_BYTE}, I{0}, P{colors}});
    gl.call("glEnableClientState", {I{GL_COLOR_ARRAY}});
    gl.put_bytes(colors, {0, 0, 0, 0});
    gl.call("glArrayElement", {I{1}});
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);
    EXPECT_EQ(ctx.state.current.color, (GlVec4{1, 1, 1, 1}));
    gl.call("glDrawArrays", {I{GL_POINTS}, I{0}, I{2}});
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);
    EXPECT_EQ(ctx.state.current.color, (GlVec4{1, 1, 1, 1}));
    gl.call("glDisableClientState", {I{GL_COLOR_ARRAY}});

    // glDrawElements: the index array itself, then the arrays up to the
    // largest index.
    const GuestAddr idx = Harness::MEM + 0x600;
    gl.put_bytes(idx, {0, 1, 1});
    gl.call("glDrawElements", {I{GL_LINES}, I{2}, I{GL_UNSIGNED_BYTE}, P{idx}});
    EXPECT_EQ(gl.err(), GL_NO_ERROR);
    gl.put_bytes(idx, {0, 2});
    gl.call("glDrawElements", {I{GL_LINES}, I{2}, I{GL_UNSIGNED_BYTE}, P{idx}});
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);   // index 2 is past END
    gl.call("glDrawElements", {I{GL_LINES}, I{2}, I{GL_UNSIGNED_INT}, P{Harness::END - 4}});
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);   // the index array crosses END
    gl.call("glDrawElements", {I{GL_LINES}, I{2}, I{GL_UNSIGNED_SHORT}, P{0}});
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);   // NULL indices
    // Inside Begin/End an out-of-range ArrayElement adds no vertex.
    gl.call("glBegin", {I{GL_POINTS}});
    gl.call("glArrayElement", {I{5}});
    EXPECT_TRUE(ctx.immediate_vertices.empty());
    gl.call("glEnd", {});
    EXPECT_EQ(gl.err(), GL_INVALID_OPERATION);
}

INSTANTIATE_TEST_SUITE_P(Bitness, GlVertexCommandTest, ::testing::Values(false, true), bitness_name);

}  // namespace
