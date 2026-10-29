// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo — glu32.dll (OpenGL Utility Library 1.2) on Vitreo's opengl32.
//
// Contracts: Microsoft Learn's glu* reference pages, the GLU 1.2
// specification, and the winoracle measurements of Microsoft's glu32
// (tools/oracle_probe/gl11_state_probe{32,64}.expected, glu-* tags, cited at
// each rule below).

#include "glu32.hpp"
#include "glu32_image.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/opengl32/gl_client_memory.hpp"
#include "winapi/graphics/opengl32/gl_commands.hpp"
#include "winapi/graphics/opengl32/gl_context.hpp"
#include "winapi/graphics/opengl32/gl_display_list.hpp"
#include "winapi/graphics/opengl32/gl_enums.hpp"
#include "winapi/graphics/opengl32/gl_exports.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Every product in this file is rounded before it is added: glu32's x86 and
// x64 arithmetic contracts nothing, and the measured results depend on it
// (gl11_state_probe64.expected fix2-unproject-fused-pivot: the rounded
// product of two non-identity matrices has an exact zero pivot and
// gluUnProject fails; a fused one would not; fix2-project-fused-w: likewise
// gluProject's w). File scope, so gluProject, gluUnProject, the matrix
// product they invert and gluLookAt / gluPerspective's arithmetic all
// follow it.
#pragma clang fp contract(off)

namespace vitreo::winapi {

using namespace glu;

namespace {

constexpr u32 GLU_VERSION = 100800;
constexpr u32 GLU_EXTENSIONS = 100801;
constexpr u32 kGlOutOfMemory = 0x0505;
constexpr double kPi = 3.14159265358979323846;

// ---- Strings ------------------------------------------------------------------------
//
// gluErrorString / gluErrorUnicodeStringEXT (glu-error-*, glu-errorw-*): the
// GL errors and GLU_INVALID_ENUM/VALUE/OUT_OF_MEMORY, the tessellator errors
// 100151-100156 and the NURBS errors 100251-100287 have strings;
// GLU_INCOMPATIBLE_GL_VERSION, GLU_INVALID_OPERATION, 100157, 100158 and
// every other code are NULL. The wide strings equal the ANSI ones. (For
// 100157 / 100158 Windows' gluErrorUnicodeStringEXT returns a pointer into
// unrelated memory while gluErrorString returns NULL; Vitreo returns NULL
// from both.) gluGetString (glu-getstring): GLU_VERSION
// "1.2.2.0 Microsoft Corporation", GLU_EXTENSIONS "GL_EXT_bgra", else NULL.
// Each string has a fixed place in the scratch region, so a pointer is the
// same on every call (glu-getstring-stable, glu-errorw-* stable=1).

struct CodeString {
    u32 code;
    const char* text;
};

constexpr CodeString kErrorStrings[] = {
    {0x0000, "no error"},
    {0x0500, "invalid enumerant"},
    {0x0501, "invalid value"},
    {0x0502, "invalid operation"},
    {0x0503, "stack overflow"},
    {0x0504, "stack underflow"},
    {0x0505, "out of memory"},
    {100900, "invalid enumerant"},
    {100901, "invalid value"},
    {100902, "out of memory"},
    {100151, "missing gluTessBeginPolygon"},
    {100152, "missing gluTessBeginContour"},
    {100153, "missing gluTessEndPolygon"},
    {100154, "missing gluTessEndContour"},
    {100155, "tesselation coordinate too large"},
    {100156, "need combine callback"},
    {100251, "spline order un-supported"},
    {100252, "too few knots"},
    {100253, "valid knot range is empty"},
    {100254, "decreasing knot sequence knot"},
    {100255, "knot multiplicity greater than order of spline"},
    {100256, "endcurve() must follow bgncurve()"},
    {100257, "bgncurve() must precede endcurve()"},
    {100258, "missing or extra geometric data"},
    {100259, "can't draw pwlcurves"},
    {100260, "missing or extra domain data"},
    {100261, "missing or extra domain data"},
    {100262, "endtrim() must precede endsurface()"},
    {100263, "bgnsurface() must precede endsurface()"},
    {100264, "curve of improper type passed as trim curve"},
    {100265, "bgnsurface() must precede bgntrim()"},
    {100266, "endtrim() must follow bgntrim()"},
    {100267, "bgntrim() must precede endtrim()"},
    {100268, "invalid or missing trim curve"},
    {100269, "bgntrim() must precede pwlcurve()"},
    {100270, "pwlcurve referenced twice"},
    {100271, "pwlcurve and nurbscurve mixed"},
    {100272, "improper usage of trim data type"},
    {100273, "nurbscurve referenced twice"},
    {100274, "nurbscurve and pwlcurve mixed"},
    {100275, "nurbssurface referenced twice"},
    {100276, "invalid property"},
    {100277, "endsurface() must follow bgnsurface()"},
    {100278, "intersecting or misoriented trim curves"},
    {100279, "intersecting trim curves"},
    {100280, "UNUSED"},
    {100281, "unconnected trim curves"},
    {100282, "unknown knot error"},
    {100283, "negative vertex count encountered"},
    {100284, "negative byte-stride encounteed"},
    {100285, "unknown type descriptor"},
    {100286, "null control point reference"},
    {100287, "duplicate point on pwlcurve"},
    {GLU_VERSION, "1.2.2.0 Microsoft Corporation"},
    {GLU_EXTENSIONS, "GL_EXT_bgra"},
};
constexpr usize kStringCount = sizeof kErrorStrings / sizeof kErrorStrings[0];

/// Byte offsets of each string in the scratch region: the ANSI copies first,
/// then the UTF-16 copies (2-byte aligned); quadric handles after both.
struct StringLayout {
    std::array<u32, kStringCount> ansi{};
    std::array<u32, kStringCount> wide{};
    u32 end{0};
};

const StringLayout& string_layout() {
    static const StringLayout layout = [] {
        StringLayout l;
        u32 at = 0;
        for (usize i = 0; i < kStringCount; ++i) {
            l.ansi[i] = at;
            at += static_cast<u32>(std::strlen(kErrorStrings[i].text)) + 1;
        }
        at = (at + 1) & ~1u;
        for (usize i = 0; i < kStringCount; ++i) {
            l.wide[i] = at;
            at += 2 * (static_cast<u32>(std::strlen(kErrorStrings[i].text)) + 1);
        }
        l.end = at;
        return l;
    }();
    return layout;
}

std::optional<usize> string_index(u32 code, bool error_codes_only) {
    for (usize i = 0; i < kStringCount; ++i) {
        const u32 c = kErrorStrings[i].code;
        if (error_codes_only && (c == GLU_VERSION || c == GLU_EXTENSIONS)) continue;
        if (c == code) return i;
    }
    return std::nullopt;
}

// ---- Fail-loud families --------------------------------------------------------------

void log_unimplemented_family(std::atomic<bool>& logged, const char* api, const char* detail) {
    if (logged.exchange(true)) return;
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED glu32!%s: %s\n", api, detail);
}

std::atomic<bool> g_tess_logged{false};
std::atomic<bool> g_nurbs_logged{false};

bool is_tess_export(std::string_view n) {
    return n == "gluNewTess" || n == "gluDeleteTess" || n == "gluTessBeginPolygon" ||
           n == "gluTessBeginContour" || n == "gluTessVertex" || n == "gluTessEndContour" ||
           n == "gluTessEndPolygon" || n == "gluTessProperty" || n == "gluTessNormal" ||
           n == "gluTessCallback" || n == "gluGetTessProperty" || n == "gluBeginPolygon" ||
           n == "gluNextContour" || n == "gluEndPolygon";
}

bool is_nurbs_export(std::string_view n) {
    return n == "gluNewNurbsRenderer" || n == "gluDeleteNurbsRenderer" || n == "gluBeginSurface" ||
           n == "gluBeginCurve" || n == "gluEndCurve" || n == "gluEndSurface" || n == "gluBeginTrim" ||
           n == "gluEndTrim" || n == "gluPwlCurve" || n == "gluNurbsCurve" || n == "gluNurbsSurface" ||
           n == "gluLoadSamplingMatrices" || n == "gluNurbsProperty" || n == "gluGetNurbsProperty" ||
           n == "gluNurbsCallback";
}

// ---- GL issue ------------------------------------------------------------------------

/// The spec 5.4 rule of each GL command GLU issues, fixed once.
struct GlRules {
    gl::GlListRule mult_matrix = gl::gl_list_rule("glMultMatrixd");
    gl::GlListRule ortho = gl::gl_list_rule("glOrtho");
    gl::GlListRule translate = gl::gl_list_rule("glTranslatef");
    gl::GlListRule scale = gl::gl_list_rule("glScalef");
    gl::GlListRule begin = gl::gl_list_rule("glBegin");
    gl::GlListRule end = gl::gl_list_rule("glEnd");
    gl::GlListRule normal = gl::gl_list_rule("glNormal3f");
    gl::GlListRule texcoord = gl::gl_list_rule("glTexCoord2f");
    gl::GlListRule vertex = gl::gl_list_rule("glVertex3f");
    gl::GlListRule tex_image_1d = gl::gl_list_rule("glTexImage1D");
    gl::GlListRule tex_image_2d = gl::gl_list_rule("glTexImage2D");
};
const GlRules& rules() {
    static const GlRules r;
    return r;
}

/// Issues command F on @p ctx as opengl32's export of rule @p rule would.
template <auto F, typename... A>
void issue(gl::GlContext& ctx, const gl::GlListRule& rule, u32 arg0, const A&... args) {
    gl::gl_issue_to<F>(ctx, gl::gl_recording_list(ctx, rule, arg0), args...);
}

/// A quadric's GL calls on the current context (none without one).
class GlQuadricSink final : public QuadricSink {
public:
    explicit GlQuadricSink(gl::GlContext* ctx) : m_ctx(ctx) {}
    void begin(u32 mode) override {
        if (m_ctx) issue<&gl::cmd::Begin>(*m_ctx, rules().begin, mode, mode);
    }
    void end() override {
        if (m_ctx) issue<&gl::cmd::End>(*m_ctx, rules().end, 0);
    }
    void normal(float x, float y, float z) override {
        if (m_ctx) issue<&gl::cmd::Normal>(*m_ctx, rules().normal, 0, x, y, z);
    }
    void texcoord(float s, float t) override {
        if (m_ctx) issue<&gl::cmd::TexCoord>(*m_ctx, rules().texcoord, 0, s, t, 0.0f, 1.0f);
    }
    void vertex(float x, float y, float z) override {
        if (m_ctx) issue<&gl::cmd::Vertex>(*m_ctx, rules().vertex, 0, x, y, z, 1.0f);
    }

private:
    gl::GlContext* m_ctx;
};

void multiply_vec(const gl::GlMatrix& m, const std::array<double, 4>& in, std::array<double, 4>& out) {
    for (int r = 0; r < 4; ++r) {
        out[r] = in[0] * m[0 * 4 + r] + in[1] * m[1 * 4 + r] + in[2] * m[2 * 4 + r] + in[3] * m[3 * 4 + r];
    }
}

/// a * b for column-major 4x4 matrices, each element summed over k = 0..3 in
/// order with every product rounded (the file's contraction rule). glu32's
/// own product: opengl32's gl_multiply is the GL matrix stack's and follows
/// that library's compilation.
gl::GlMatrix multiply_matrices(const gl::GlMatrix& a, const gl::GlMatrix& b) {
    gl::GlMatrix r{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                const double product = a[static_cast<usize>(k * 4 + row)] * b[static_cast<usize>(col * 4 + k)];
                sum = sum + product;
            }
            r[static_cast<usize>(col * 4 + row)] = sum;
        }
    }
    return r;
}

bool read_matrix(Marshaler& m, GuestAddr p, gl::GlMatrix& out) {
    if (p == 0) return false;
    return m.memory().read_bytes(p, reinterpret_cast<Byte*>(out.data()), sizeof(double) * 16).is_ok();
}

bool read_viewport(Marshaler& m, GuestAddr p, std::array<i32, 4>& out) {
    if (p == 0) return false;
    return m.memory().read_bytes(p, reinterpret_cast<Byte*>(out.data()), sizeof(i32) * 4).is_ok();
}

/// The inverse of @p m by Gauss-Jordan elimination with row swaps, treating
/// the 16 values as rows of four in memory order. The row swapped in for
/// column c is the last row below c whose entry in column c is larger in
/// magnitude than the diagonal entry's (not the largest one: only that rule
/// reproduces fix2-unproject-regular bit for bit along with every
/// ext-unproject-* line). False when a pivot is exactly zero. This
/// is glu32's singularity rule: matrices whose determinant is zero but
/// whose elimination meets no exact zero pivot invert to huge values, and
/// only an exact zero pivot fails (ext-unproject-m1 .. -m9; the cofactor
/// determinant would also refuse m1 - m3 and m8).
bool invert_by_elimination(const gl::GlMatrix& m, gl::GlMatrix& out) {
    double a[4][4];
    double b[4][4];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            a[r][c] = m[static_cast<usize>(r * 4 + c)];
            b[r][c] = r == c ? 1.0 : 0.0;
        }
    }
    for (int col = 0; col < 4; ++col) {
        int best = col;
        for (int r = col + 1; r < 4; ++r) {
            if (std::fabs(a[r][col]) > std::fabs(a[col][col])) best = r;
        }
        if (best != col) {
            for (int c = 0; c < 4; ++c) {
                std::swap(a[col][c], a[best][c]);
                std::swap(b[col][c], b[best][c]);
            }
        }
        const double pivot = a[col][col];
        if (pivot == 0.0) return false;
        for (int c = 0; c < 4; ++c) {
            a[col][c] /= pivot;
            b[col][c] /= pivot;
        }
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            const double factor = a[r][col];
            for (int c = 0; c < 4; ++c) {
                a[r][c] -= a[col][c] * factor;
                b[r][c] -= b[col][c] * factor;
            }
        }
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) out[static_cast<usize>(r * 4 + c)] = b[r][c];
    }
    return true;
}

void log_fault_once(const char* api, const char* what) {
    static std::mutex mu;
    static std::vector<std::string> seen;
    std::lock_guard lock(mu);
    for (const auto& s : seen) {
        if (s == api) return;
    }
    seen.emplace_back(api);
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED glu32!%s: %s (Windows faults)\n", api, what);
}

}  // namespace

// ---- Arguments -----------------------------------------------------------------------

/// One marshalled call: arguments by number from the export's kinds string
/// (x86: a double takes two stack slots; x64: argument n is position n, the
/// first four floating-point ones in XMM0-3).
class Glu32Manager::Args {
public:
    Args(Marshaler& marshaler, const gl::GlExportSlots& slots) : m(marshaler), m_slots(slots) {}
    Marshaler& m;
    [[nodiscard]] u32 u(usize n) const { return m.arg32(slot(n)); }
    [[nodiscard]] i32 i(usize n) const { return static_cast<i32>(u(n)); }
    [[nodiscard]] double d(usize n) const { return m.arg_f64(slot(n)); }
    [[nodiscard]] GuestAddr p(usize n) const {
        const u64 v = m.arg(slot(n));
        return static_cast<GuestAddr>(m.cpu_is_64bit() ? v : (v & 0xFFFFFFFFu));
    }
    void ret(u64 v) { m.set_return(v); }

private:
    [[nodiscard]] usize slot(usize n) const noexcept { return m.cpu_is_64bit() ? n : m_slots.x86[n]; }
    const gl::GlExportSlots& m_slots;
};

// ---- Strings -------------------------------------------------------------------------

GuestAddr Glu32Manager::publish_string(Marshaler& m, usize index, bool wide) {
    // No reserved region: never write memory glu32 does not own.
    if (m_scratch_base == 0) return 0;
    const StringLayout& l = string_layout();
    const char* text = kErrorStrings[index].text;
    const usize len = std::strlen(text);
    if (!wide) {
        const GuestAddr at = m_scratch_base + l.ansi[index];
        (void)m.memory().write_bytes(at, reinterpret_cast<const Byte*>(text), len + 1);
        return at;
    }
    const GuestAddr at = m_scratch_base + l.wide[index];
    for (usize k = 0; k <= len; ++k) {
        (void)m.memory().write_u16(at + k * 2, static_cast<u16>(static_cast<u8>(text[k])));
    }
    return at;
}

// const GLubyte* gluErrorString(GLenum errCode)
void Glu32Manager::gluErrorString(Args& a) {
    const auto index = string_index(a.u(0), true);
    a.ret(index ? publish_string(a.m, *index, false) : 0);
}

// const wchar_t* gluErrorUnicodeStringEXT(GLenum errCode)
void Glu32Manager::gluErrorUnicodeStringEXT(Args& a) {
    const auto index = string_index(a.u(0), true);
    a.ret(index ? publish_string(a.m, *index, true) : 0);
}

// const GLubyte* gluGetString(GLenum name)
void Glu32Manager::gluGetString(Args& a) {
    const u32 name = a.u(0);
    if (name != GLU_VERSION && name != GLU_EXTENSIONS) {
        a.ret(0);
        return;
    }
    a.ret(publish_string(a.m, *string_index(name, false), false));
}

// ---- Matrix helpers --------------------------------------------------------------------

// void gluOrtho2D(left, right, bottom, top): glOrtho(..., -1, 1) (Learn), so
// left == right is glOrtho's GL_INVALID_VALUE (glu-ortho2d-left-eq-right).
void Glu32Manager::gluOrtho2D(Args& a) {
    gl::GlContext* ctx = gl::current_gl_context();
    if (ctx == nullptr) return;
    issue<&gl::cmd::Ortho>(*ctx, rules().ortho, 0, a.d(0), a.d(1), a.d(2), a.d(3), -1.0, 1.0);
}

// void gluPerspective(fovy, aspect, zNear, zFar): glMultMatrixd of the Learn
// matrix (f = cot(fovy / 2)); nothing for aspect 0, zNear == zFar or
// sin(fovy / 2) == 0 (glu-perspective-aspect0, -near-eq-far, -fovy0).
void Glu32Manager::gluPerspective(Args& a) {
    const double fovy = a.d(0), aspect = a.d(1), z_near = a.d(2), z_far = a.d(3);
    gl::GlContext* ctx = gl::current_gl_context();
    if (ctx == nullptr) return;
    const double radians = fovy / 2 * kPi / 180;
    const double delta_z = z_far - z_near;
    const double sine = std::sin(radians);
    if (delta_z == 0 || sine == 0 || aspect == 0) return;
    const double cotangent = std::cos(radians) / sine;
    gl::GlMatrix m = gl::gl_identity_matrix();
    m[0] = cotangent / aspect;
    m[5] = cotangent;
    m[10] = -(z_far + z_near) / delta_z;
    m[11] = -1;
    m[14] = -2 * z_near * z_far / delta_z;
    m[15] = 0;
    issue<&gl::cmd::MultMatrix>(*ctx, rules().mult_matrix, 0, m);
}

// void gluLookAt(eye, center, up): the rotation of Learn's remarks
// (f = normalize(center - eye), s = f x up normalised, u = s x f) as a
// single-precision glMultMatrixf, then glTranslated(-eye). Normalising a zero
// vector leaves it zero (glu-lookat-eye-eq-center, -up-parallel, -up-zero).
void Glu32Manager::gluLookAt(Args& a) {
    std::array<double, 9> v{};
    for (usize k = 0; k < 9; ++k) v[k] = a.d(k);
    gl::GlContext* ctx = gl::current_gl_context();
    if (ctx == nullptr) return;
    auto normalize = [](float x[3]) {
        const float r = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        if (r == 0.0f) return;
        x[0] /= r;
        x[1] /= r;
        x[2] /= r;
    };
    auto cross = [](const float p[3], const float q[3], float out[3]) {
        out[0] = p[1] * q[2] - p[2] * q[1];
        out[1] = p[2] * q[0] - p[0] * q[2];
        out[2] = p[0] * q[1] - p[1] * q[0];
    };
    float forward[3] = {static_cast<float>(v[3] - v[0]), static_cast<float>(v[4] - v[1]),
                        static_cast<float>(v[5] - v[2])};
    float up[3] = {static_cast<float>(v[6]), static_cast<float>(v[7]), static_cast<float>(v[8])};
    normalize(forward);
    float side[3];
    cross(forward, up, side);
    normalize(side);
    cross(side, forward, up);
    gl::GlMatrix m = gl::gl_identity_matrix();
    for (int k = 0; k < 3; ++k) {
        m[k * 4 + 0] = side[k];
        m[k * 4 + 1] = up[k];
        m[k * 4 + 2] = -forward[k];
    }
    issue<&gl::cmd::MultMatrix>(*ctx, rules().mult_matrix, 0, m);
    issue<&gl::cmd::Translate>(*ctx, rules().translate, 0, -v[0], -v[1], -v[2]);
}

// void gluPickMatrix(x, y, width, height, GLint viewport[4]): glTranslatef
// then glScalef so the pick region fills the viewport (Learn); nothing for
// width or height <= 0 (glu-pickmatrix-width0, -height-negative).
void Glu32Manager::gluPickMatrix(Args& a) {
    const double x = a.d(0), y = a.d(1), dx = a.d(2), dy = a.d(3);
    const GuestAddr vp_p = a.p(4);
    gl::GlContext* ctx = gl::current_gl_context();
    if (ctx == nullptr) return;
    if (dx <= 0 || dy <= 0) return;
    std::array<i32, 4> vp{};
    if (!read_viewport(a.m, vp_p, vp)) {
        log_fault_once("gluPickMatrix", "unreadable viewport");
        return;
    }
    const float tx = static_cast<float>((vp[2] - 2 * (x - vp[0])) / dx);
    const float ty = static_cast<float>((vp[3] - 2 * (y - vp[1])) / dy);
    const float sx = static_cast<float>(vp[2] / dx);
    const float sy = static_cast<float>(vp[3] / dy);
    issue<&gl::cmd::Translate>(*ctx, rules().translate, 0, static_cast<double>(tx), static_cast<double>(ty), 0.0);
    issue<&gl::cmd::Scale>(*ctx, rules().scale, 0, static_cast<double>(sx), static_cast<double>(sy), 1.0);
}

// int gluProject(objx, objy, objz, model[16], proj[16], viewport[4], *winx,
// *winy, *winz): proj * model * (obj, 1), divided by w and mapped to the
// viewport, z to [0, 1] (Learn). GL_FALSE with the outputs untouched when w
// is 0 (glu-project-w0). Pure arithmetic: no context needed (glu-noctx-project).
void Glu32Manager::gluProject(Args& a) {
    gl::GlMatrix model{}, proj{};
    std::array<i32, 4> vp{};
    if (!read_matrix(a.m, a.p(3), model) || !read_matrix(a.m, a.p(4), proj) || !read_viewport(a.m, a.p(5), vp)) {
        log_fault_once("gluProject", "unreadable matrix or viewport");
        a.ret(0);
        return;
    }
    std::array<double, 4> in{a.d(0), a.d(1), a.d(2), 1.0}, out{};
    multiply_vec(model, in, out);
    multiply_vec(proj, out, in);
    if (in[3] == 0.0) {
        a.ret(0);
        return;
    }
    for (int k = 0; k < 3; ++k) in[k] = in[k] / in[3] * 0.5 + 0.5;
    const double win[3] = {in[0] * vp[2] + vp[0], in[1] * vp[3] + vp[1], in[2]};
    for (int k = 0; k < 3; ++k) (void)a.m.memory().write_bytes(a.p(6 + k), reinterpret_cast<const Byte*>(&win[k]), 8);
    a.ret(1);
}

// int gluUnProject(winx, winy, winz, model[16], proj[16], viewport[4], *objx,
// *objy, *objz): the inverse of gluProject through (proj * model)^-1 (Learn);
// GL_FALSE, outputs untouched, when the elimination meets a zero pivot or w
// is 0 (glu-unproject-singular, ext-unproject-*).
void Glu32Manager::gluUnProject(Args& a) {
    gl::GlMatrix model{}, proj{};
    std::array<i32, 4> vp{};
    if (!read_matrix(a.m, a.p(3), model) || !read_matrix(a.m, a.p(4), proj) || !read_viewport(a.m, a.p(5), vp)) {
        log_fault_once("gluUnProject", "unreadable matrix or viewport");
        a.ret(0);
        return;
    }
    gl::GlMatrix inverse{};
    if (!invert_by_elimination(multiply_matrices(proj, model), inverse)) {
        a.ret(0);
        return;
    }
    std::array<double, 4> in{a.d(0), a.d(1), a.d(2), 1.0}, out{};
    in[0] = (in[0] - vp[0]) / vp[2];
    in[1] = (in[1] - vp[1]) / vp[3];
    for (int k = 0; k < 3; ++k) in[k] = in[k] * 2 - 1;
    multiply_vec(inverse, in, out);
    if (out[3] == 0.0) {
        a.ret(0);
        return;
    }
    for (int k = 0; k < 3; ++k) {
        const double o = out[k] / out[3];
        (void)a.m.memory().write_bytes(a.p(6 + k), reinterpret_cast<const Byte*>(&o), 8);
    }
    a.ret(1);
}

// ---- Images --------------------------------------------------------------------------

namespace {

PixelModes modes_of(const gl::GlPixelStoreModes& s) {
    return PixelModes{s.swap_bytes, s.lsb_first, s.row_length, s.skip_rows, s.skip_pixels, s.alignment};
}

/// The pixel-store modes GLU reads with glGetIntegerv; null (and
/// GL_INVALID_OPERATION recorded inside glBegin/glEnd) when the queries fail,
/// which glu32 reports as GL_OUT_OF_MEMORY (glu-scale-inside-begin,
/// glu-noctx-scaleimage: 1285).
gl::GlContext* context_for_queries() {
    gl::GlContext* ctx = gl::current_gl_context();
    if (ctx == nullptr) return nullptr;
    if (ctx->reject_inside_begin_end()) return nullptr;
    return ctx;
}

/// gluScaleImage / gluBuild*DMipmaps keep their working images on the host
/// (HostBuffer) under opengl32's client-memory policy (gl_client_memory.hpp):
/// a client image whose span cannot exist (image_span nullopt) or that is not
/// committed guest memory with the access needed (read for the source, read
/// and write for the destination) is unreadable before anything of its size
/// is allocated; a host allocation that fails is GLU_OUT_OF_MEMORY, as
/// glu32's failed malloc is (Learn: "There is not enough memory to complete
/// the operation").
enum class ClientRead { Ok, Unreadable, NoMemory };

/// Reads the @p span client bytes at @p p into @p out.
ClientRead read_client(Marshaler& m, GuestAddr p, std::optional<usize> span, MemoryProtection access,
                       HostBuffer<u8>& out) {
    if (!span) return ClientRead::Unreadable;   // no address range holds the image
    const usize n = *span;
    if (n != 0 && (p == 0 || !gl::gl_client_range_accessible(m.memory(), p, n, access))) {
        return ClientRead::Unreadable;
    }
    if (!out.allocate(n)) return ClientRead::NoMemory;
    if (n == 0) return ClientRead::Ok;
    return m.memory().read_bytes(p, reinterpret_cast<Byte*>(out.data()), n).is_ok() ? ClientRead::Ok
                                                                                      : ClientRead::Unreadable;
}

/// A buffer of width * height * comps components (each written before it is
/// read); false when it cannot be had: a working image larger than the
/// guest's user address space (glu32's malloc runs inside it and can never
/// satisfy that) or one the host refuses.
bool allocate_image(Marshaler& m, HostBuffer<u16>& image, i32 width, i32 height, u32 comps) {
    const std::optional<usize> count = component_count(width, height, comps);
    if (!count || *count > gl::gl_working_limit(&m.memory()) / sizeof(u16)) return false;
    return image.allocate(*count);
}

}  // namespace

// int gluScaleImage(format, widthin, heightin, typein, datain, widthout,
// heightout, typeout, dataout) (Learn; GLU 1.2 3.1). Order measured
// (glu-scale-size-*, -badformat-and-*): any size 0 -> 0 with nothing written;
// then a negative size -> GLU_INVALID_VALUE; then format / types ->
// GLU_INVALID_ENUM; then the pixel-store queries.
void Glu32Manager::gluScaleImage(Args& a) {
    const u32 format = a.u(0);
    const i32 wi = a.i(1), hi = a.i(2);
    const u32 type_in = a.u(3);
    const GuestAddr data_in = a.p(4);
    const i32 wo = a.i(5), ho = a.i(6);
    const u32 type_out = a.u(7);
    const GuestAddr data_out = a.p(8);
    if (wi == 0 || hi == 0 || wo == 0 || ho == 0) {
        a.ret(0);
        return;
    }
    if (wi < 0 || hi < 0 || wo < 0 || ho < 0) {
        a.ret(GLU_INVALID_VALUE);
        return;
    }
    if (!legal_format(format) || !legal_type(type_in) || !legal_type(type_out)) {
        a.ret(GLU_INVALID_ENUM);
        return;
    }
    gl::GlContext* ctx = context_for_queries();
    if (ctx == nullptr) {
        a.ret(kGlOutOfMemory);
        return;
    }
    const u32 comps = format_components(format);
    const PixelModes unpack = modes_of(ctx->state.pixel_store.unpack);
    const PixelModes pack = modes_of(ctx->state.pixel_store.pack);
    {
        HostBuffer<u8> src, dst;
        const ClientRead in =
            read_client(a.m, data_in, image_span(unpack, wi, hi, format, type_in), MemoryProtection::Read, src);
        const ClientRead out = in == ClientRead::Ok ? read_client(a.m, data_out, image_span(pack, wo, ho, format, type_out),
                                                                  MemoryProtection::ReadWrite, dst)
                                                    : in;
        if (out == ClientRead::Unreadable) {
            log_fault_once("gluScaleImage", "unreadable client image");
            a.ret(kGlOutOfMemory);
            return;
        }
        HostBuffer<u16> before, after;
        if (out == ClientRead::NoMemory || !allocate_image(a.m, before, wi, hi, comps) ||
            !allocate_image(a.m, after, wo, ho, comps)) {
            a.ret(GLU_OUT_OF_MEMORY);
            return;
        }
        unpack_image(unpack, wi, hi, format, type_in, src.data(), before.data());
        if (!box_filter_scale(comps, wi, hi, before.data(), wo, ho, after.data())) {
            a.ret(GLU_OUT_OF_MEMORY);
            return;
        }
        pack_image(pack, wo, ho, format, type_out, after.data(), dst.data());
        if (a.m.memory().write_bytes(data_out, reinterpret_cast<const Byte*>(dst.data()), dst.size()).is_err()) {
            log_fault_once("gluScaleImage", "unwritable client image");
        }
    }
    a.ret(0);
}

// int gluBuild1DMipmaps(target, components, width, format, type, data)
// int gluBuild2DMipmaps(target, components, width, height, format, type, data)
// (Learn; GLU 1.2 3.2). Measured (glu-build2d-*, glu-build1d-*): width /
// height < 1 -> GLU_INVALID_VALUE; a format or type gluScaleImage refuses,
// or GL_STENCIL_INDEX / GL_DEPTH_COMPONENT -> GLU_INVALID_ENUM; a bad target
// or component count is glTexImage's error and the call still returns 0.
// Level 0 is the image scaled to the nearest power of two in each direction,
// clamped to GL_MAX_TEXTURE_SIZE (2000 -> 1024); each next level halves the
// previous one (a size already 1 stays 1) down to 1x1. The source is read
// under the UNPACK_* modes, which are unchanged afterwards; the levels are
// uploaded as GL_UNSIGNED_SHORT components under their own tight layout
// (alignment 2), and a list being compiled receives every upload
// (glu-build2d-compiled).
i32 Glu32Manager::build_mipmaps(Args& a, u32 dimensions) {
    const usize f = dimensions == 2 ? 1 : 0;   // argument shift after height
    const u32 target = a.u(0);
    const i32 components = a.i(1);
    const i32 width = a.i(2);
    const i32 height = dimensions == 2 ? a.i(3) : 1;
    const u32 format = a.u(3 + f);
    const u32 type = a.u(4 + f);
    const GuestAddr data = a.p(5 + f);
    if (width < 1 || height < 1) return static_cast<i32>(GLU_INVALID_VALUE);
    if (!legal_format(format) || !legal_type(type) || format == 0x1901 /*GL_STENCIL_INDEX*/ ||
        format == 0x1902 /*GL_DEPTH_COMPONENT*/) {
        return static_cast<i32>(GLU_INVALID_ENUM);
    }
    gl::GlContext* ctx = context_for_queries();
    if (ctx == nullptr) return static_cast<i32>(kGlOutOfMemory);   // glu-noctx-build2d, -inside-begin
    gl::GlQuery max_size;
    (void)gl::cmd::Get(*ctx, gl::GL_MAX_TEXTURE_SIZE, max_size);
    const i32 max_texture = gl::gl_query_to_integer(max_size.kind, max_size.v[0]);
    auto fit = [max_texture](i32 v) {
        const i64 p = nearest_power(v);
        return p > max_texture ? max_texture : static_cast<i32>(p);
    };
    i32 new_w = fit(width);
    i32 new_h = dimensions == 2 ? fit(height) : 1;
    return build_levels(a, dimensions, target, components, width, height, format, type, data, new_w, new_h, *ctx);
}

i32 Glu32Manager::build_levels(Args& a, u32 dimensions, u32 target, i32 components, i32 width, i32 height,
                               u32 format, u32 type, GuestAddr data, i32 new_w, i32 new_h, gl::GlContext& gl_ctx) {
    gl::GlContext* ctx = &gl_ctx;
    const u32 comps = format_components(format);
    const PixelModes unpack = modes_of(ctx->state.pixel_store.unpack);
    HostBuffer<u8> src;
    const ClientRead read =
        read_client(a.m, data, image_span(unpack, width, height, format, type), MemoryProtection::Read, src);
    if (read == ClientRead::Unreadable) {
        log_fault_once(dimensions == 2 ? "gluBuild2DMipmaps" : "gluBuild1DMipmaps", "unreadable client image");
        return static_cast<i32>(kGlOutOfMemory);
    }
    HostBuffer<u16> image;
    if (read == ClientRead::NoMemory || !allocate_image(a.m, image, width, height, comps)) {
        return static_cast<i32>(GLU_OUT_OF_MEMORY);
    }
    unpack_image(unpack, width, height, format, type, src.data(), image.data());
    src = HostBuffer<u8>{};
    i32 image_w = width, image_h = height;

    auto log2_of = [](i32 v) {
        i32 n = 0;
        while (v > 1) {
            v >>= 1;
            ++n;
        }
        return n;
    };
    const i32 levels = std::max(log2_of(new_w), log2_of(new_h));
    static const gl::GlPixelStoreModes kUpload{false, false, 0, 0, 0, 2};
    const gl::GlListRule& rule = dimensions == 2 ? rules().tex_image_2d : rules().tex_image_1d;
    HostBuffer<u16> scaled;
    for (i32 level = 0; level <= levels; ++level) {
        if (image_w != new_w || image_h != new_h) {
            // A failure part-way leaves the levels already uploaded, as
            // glu32's per-level malloc does.
            if (!allocate_image(a.m, scaled, new_w, new_h, comps) ||
                !box_filter_scale(comps, image_w, image_h, image.data(), new_w, new_h, scaled.data())) {
                return static_cast<i32>(GLU_OUT_OF_MEMORY);
            }
            image.swap(scaled);
            image_w = new_w;
            image_h = new_h;
        }
        gl::cmd::GlClientPixels px;
        px.bytes = std::span<const u8>(reinterpret_cast<const u8*>(image.data()), image.size() * sizeof(u16));
        px.unpack = &kUpload;
        issue<&gl::cmd::TexImage>(*ctx, rule, target, dimensions, target, level, components, image_w, image_h, 0,
                                  format, gl::GL_UNSIGNED_SHORT, px);
        if (new_w > 1) new_w /= 2;
        if (new_h > 1) new_h /= 2;
    }
    return 0;
}

void Glu32Manager::gluBuild1DMipmaps(Args& a) { a.ret(static_cast<u32>(build_mipmaps(a, 1))); }
void Glu32Manager::gluBuild2DMipmaps(Args& a) { a.ret(static_cast<u32>(build_mipmaps(a, 2))); }

// ---- Quadrics ------------------------------------------------------------------------

std::optional<QuadricState> Glu32Manager::quadric_state(GuestAddr handle) const {
    std::lock_guard lock(m_mutex);
    auto it = m_quadrics.find(handle);
    if (it == m_quadrics.end()) return std::nullopt;
    return it->second;
}

template <typename Fn>
bool Glu32Manager::with_quadric(GuestAddr handle, Fn&& fn) {
    std::lock_guard lock(m_mutex);
    auto it = m_quadrics.find(handle);
    if (it == m_quadrics.end()) return false;
    fn(it->second);
    return true;
}

// GLU 1.2 4.2: an error calls the GLU_ERROR callback with the code; there is
// no GL error and no other report (glu-quadric-err-no-callback,
// -drawstyle-bad gl-err=0). The callback is void CALLBACK fn(GLenum).
void Glu32Manager::quadric_error(Args& a, GuestAddr callback, u32 error) {
    if (callback == 0 || !m_guest_call) return;
    const u64 args[1] = {error};
    (void)m_guest_call(callback, args, 1, a.m.cpu_is_64bit());
}

// GLUquadric* gluNewQuadric(void): a process-heap allocation (no context
// needed, glu-noctx-newquadric), as many as the heap holds; NULL only when
// the allocation fails (Learn: "If there is not enough memory to allocate
// the object, the return value is 0").
void Glu32Manager::gluNewQuadric(Args& a) {
    const GuestAddr handle = m_heap.alloc ? m_heap.alloc(kQuadricBytes) : 0;
    if (handle == 0) {
        std::fprintf(stderr, "[vitreo] glu32!gluNewQuadric: the process heap has no room for a quadric\n");
        a.ret(0);
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        m_quadrics[handle] = QuadricState{};
    }
    a.ret(handle);
}

// void gluDeleteQuadric(GLUquadric*): frees a live quadric; any other pointer
// (NULL, freed, guest garbage) is left alone.
void Glu32Manager::gluDeleteQuadric(Args& a) {
    const GuestAddr handle = a.p(0);
    {
        std::lock_guard lock(m_mutex);
        if (m_quadrics.erase(handle) == 0) return;
    }
    if (m_heap.free) m_heap.free(handle);
}

// The setters keep the old value and report GLU_INVALID_ENUM for a token
// they do not take (glu-quadric-err-*-bad, -bad-style-keeps-point).
void Glu32Manager::gluQuadricDrawStyle(Args& a) {
    const u32 v = a.u(1);
    const bool ok = v == GLU_POINT || v == GLU_LINE || v == GLU_FILL || v == GLU_SILHOUETTE;
    GuestAddr cb = 0;
    (void)with_quadric(a.p(0), [&](QuadricState& q) {
        if (ok) q.draw_style = v;
        cb = q.error_callback;
    });
    if (!ok) quadric_error(a, cb, GLU_INVALID_ENUM);
}

void Glu32Manager::gluQuadricNormals(Args& a) {
    const u32 v = a.u(1);
    const bool ok = v == GLU_SMOOTH || v == GLU_FLAT || v == GLU_NONE;
    GuestAddr cb = 0;
    (void)with_quadric(a.p(0), [&](QuadricState& q) {
        if (ok) q.normals = v;
        cb = q.error_callback;
    });
    if (!ok) quadric_error(a, cb, GLU_INVALID_ENUM);
}

void Glu32Manager::gluQuadricOrientation(Args& a) {
    const u32 v = a.u(1);
    const bool ok = v == GLU_OUTSIDE || v == GLU_INSIDE;
    GuestAddr cb = 0;
    (void)with_quadric(a.p(0), [&](QuadricState& q) {
        if (ok) q.orientation = v;
        cb = q.error_callback;
    });
    if (!ok) quadric_error(a, cb, GLU_INVALID_ENUM);
}

// GLboolean: any non-zero value turns texture coordinates on, without an
// error (glu-quadric-err-texture-2).
void Glu32Manager::gluQuadricTexture(Args& a) {
    const bool v = (a.u(1) & 0xFFu) != 0;
    (void)with_quadric(a.p(0), [&](QuadricState& q) { q.texture = v; });
}

// void gluQuadricCallback(qobj, which, fn): GLU_ERROR is the only callback
// (Learn); another `which` is GLU_INVALID_ENUM to the current callback
// (glu-quadric-err-callback-which-bad). A NULL fn clears it
// (-drawstyle-bad-after-null-callback).
void Glu32Manager::gluQuadricCallback(Args& a) {
    const u32 which = a.u(1);
    const GuestAddr fn = a.p(2);
    GuestAddr cb = 0;
    (void)with_quadric(a.p(0), [&](QuadricState& q) {
        if (which == GLU_ERROR) q.error_callback = fn;
        cb = q.error_callback;
    });
    if (which != GLU_ERROR) quadric_error(a, cb, GLU_INVALID_ENUM);
}

template <typename Draw>
void Glu32Manager::draw_quadric(Args& a, Draw&& draw) {
    QuadricState state;
    if (!with_quadric(a.p(0), [&](QuadricState& q) { state = q; })) return;
    GlQuadricSink sink(gl::current_gl_context());
    const u32 error = draw(state, sink);
    if (error != 0) quadric_error(a, state.error_callback, error);
}

// void gluSphere(qobj, radius, slices, stacks)
void Glu32Manager::gluSphere(Args& a) {
    const double radius = a.d(1);
    const i32 slices = a.i(2), stacks = a.i(3);
    draw_quadric(a, [&](const QuadricState& q, QuadricSink& s) { return draw_sphere(q, radius, slices, stacks, s); });
}

// void gluCylinder(qobj, baseRadius, topRadius, height, slices, stacks)
void Glu32Manager::gluCylinder(Args& a) {
    const double base = a.d(1), top = a.d(2), height = a.d(3);
    const i32 slices = a.i(4), stacks = a.i(5);
    draw_quadric(a, [&](const QuadricState& q, QuadricSink& s) {
        return draw_cylinder(q, base, top, height, slices, stacks, s);
    });
}

// void gluDisk(qobj, innerRadius, outerRadius, slices, loops)
void Glu32Manager::gluDisk(Args& a) {
    const double inner = a.d(1), outer = a.d(2);
    const i32 slices = a.i(3), loops = a.i(4);
    draw_quadric(a, [&](const QuadricState& q, QuadricSink& s) { return draw_disk(q, inner, outer, slices, loops, s); });
}

// void gluPartialDisk(qobj, innerRadius, outerRadius, slices, loops,
// startAngle, sweepAngle)
void Glu32Manager::gluPartialDisk(Args& a) {
    const double inner = a.d(1), outer = a.d(2);
    const i32 slices = a.i(3), loops = a.i(4);
    const double start = a.d(5), sweep = a.d(6);
    draw_quadric(a, [&](const QuadricState& q, QuadricSink& s) {
        return draw_partial_disk(q, inner, outer, slices, loops, start, sweep, s);
    });
}

// ---- Export table --------------------------------------------------------------------

ApiHandler Glu32Manager::handler(std::string_view name) {
    using Impl = void (Glu32Manager::*)(Args&);
    static const std::pair<std::string_view, Impl> kImpl[] = {
        {"gluErrorString", &Glu32Manager::gluErrorString},
        {"gluErrorUnicodeStringEXT", &Glu32Manager::gluErrorUnicodeStringEXT},
        {"gluGetString", &Glu32Manager::gluGetString},
        {"gluOrtho2D", &Glu32Manager::gluOrtho2D},
        {"gluPerspective", &Glu32Manager::gluPerspective},
        {"gluLookAt", &Glu32Manager::gluLookAt},
        {"gluPickMatrix", &Glu32Manager::gluPickMatrix},
        {"gluProject", &Glu32Manager::gluProject},
        {"gluUnProject", &Glu32Manager::gluUnProject},
        {"gluScaleImage", &Glu32Manager::gluScaleImage},
        {"gluBuild1DMipmaps", &Glu32Manager::gluBuild1DMipmaps},
        {"gluBuild2DMipmaps", &Glu32Manager::gluBuild2DMipmaps},
        {"gluNewQuadric", &Glu32Manager::gluNewQuadric},
        {"gluDeleteQuadric", &Glu32Manager::gluDeleteQuadric},
        {"gluQuadricDrawStyle", &Glu32Manager::gluQuadricDrawStyle},
        {"gluQuadricNormals", &Glu32Manager::gluQuadricNormals},
        {"gluQuadricOrientation", &Glu32Manager::gluQuadricOrientation},
        {"gluQuadricTexture", &Glu32Manager::gluQuadricTexture},
        {"gluQuadricCallback", &Glu32Manager::gluQuadricCallback},
        {"gluSphere", &Glu32Manager::gluSphere},
        {"gluCylinder", &Glu32Manager::gluCylinder},
        {"gluDisk", &Glu32Manager::gluDisk},
        {"gluPartialDisk", &Glu32Manager::gluPartialDisk},
    };
    const char* kinds = nullptr;
#define GLU32_EXPORT(n, ordinal, stack_bytes, ret, k) \
    if (name == #n) kinds = k;
#include "glu32_exports.inc"
#undef GLU32_EXPORT
    if (kinds == nullptr) return nullptr;
    const gl::GlExportSlots slots = gl::gl_export_slots(kinds);
    for (const auto& [n, impl] : kImpl) {
        if (n == name) {
            return [this, impl, slots](Marshaler& m) {
                m.set_return(0);
                Args a(m, slots);
                (this->*impl)(a);
            };
        }
    }
    // The tessellator and NURBS: GLU's failure value (NULL / nothing), one
    // log line per family.
    const std::string api(name);
    if (is_tess_export(name)) {
        return [api](Marshaler& m) {
            log_unimplemented_family(g_tess_logged, api.c_str(), "the GLU tessellator is not implemented");
            m.set_return(0);
        };
    }
    if (is_nurbs_export(name)) {
        return [api](Marshaler& m) {
            log_unimplemented_family(g_nurbs_logged, api.c_str(), "the GLU NURBS renderer is not implemented");
            m.set_return(0);
        };
    }
    return nullptr;
}

void glu32_forget_unimplemented_logs_for_tests() {
    g_tess_logged = false;
    g_nurbs_logged = false;
}

DllExports create_glu32_thunks(Glu32Manager& mgr) {
    DllExports exports;
    auto add = [&](const char* name, u16 ordinal, u16 stdcall_bytes) {
        exports.entries.push_back(ExportEntry{std::string(name), ordinal, mgr.handler(name), stdcall_bytes});
    };
#define GLU32_EXPORT(n, ordinal, stack_bytes, ret, kinds) add(#n, ordinal, stack_bytes);
#include "glu32_exports.inc"
#undef GLU32_EXPORT
    if (string_layout().end > GLU32_SCRATCH_SIZE) {
        std::fprintf(stderr, "[vitreo] glu32: string table overruns its scratch region\n");
    }
    return exports;
}

} // namespace vitreo::winapi
