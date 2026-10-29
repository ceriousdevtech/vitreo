// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_cmd_vertex.cpp
/// @brief glBegin / glEnd and glVertex (OpenGL 1.1 spec 2.6), glRect (2.9),
///        glArrayElement / glDrawArrays / glDrawElements (2.8), and the
///        exports of every current-value, vertex, rectangle and vertex-array
///        command. The current-value and array-pointer commands themselves
///        are gl_cmd_vertex_state.cpp's (they only set state).
///
/// Vector forms read client memory. Commands spec 2.6.3 forbids inside
/// glBegin/glEnd check that first and read nothing there. A vector whose
/// memory is unreadable makes the command do nothing (Windows would fault in
/// the caller) and is logged once per command family. Arrays are validated
/// over the whole byte range a draw implies (GuestMemory::is_mapped and read
/// protection) before anything is read; an invalid range is
/// GL_INVALID_OPERATION and the command has no effect.

#include "winapi/graphics/opengl32/gl_backend_draw.hpp"
#include "winapi/graphics/opengl32/gl_cmd_internal.hpp"
#include "winapi/graphics/opengl32/gl_exports.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_set>

namespace vitreo::winapi::gl {

namespace {

GlBatchVertex snapshot(const GlContext& ctx, float x, float y, float z, float w) {
    const GlCurrentState& c = ctx.state.current;
    GlBatchVertex v{};
    v.pos[0] = x;
    v.pos[1] = y;
    v.pos[2] = z;
    v.pos[3] = w;
    for (usize k = 0; k < 4; ++k) v.color[k] = c.color[k];
    for (usize k = 0; k < 3; ++k) v.normal[k] = c.normal[k];
    for (usize k = 0; k < 4; ++k) v.tex[k] = c.tex_coords[k];
    v.line_distance = 0.0f;
    return v;
}

// ---- Client arrays (spec 2.8, Table 2.4) ---------------------------------------------

u32 type_bytes(u32 type) {
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: return 4;
    case GL_DOUBLE: return 8;
    default: return 0;
    }
}

template <typename T>
T load(const u8* p, u32 k) {
    T v;
    std::memcpy(&v, p + k * sizeof(T), sizeof(T));
    return v;
}

/// Component @p k as its value (vertex, texture coordinate, index).
double raw_component(const u8* p, u32 type, u32 k) {
    switch (type) {
    case GL_BYTE: return load<i8>(p, k);
    case GL_UNSIGNED_BYTE: return load<u8>(p, k);
    case GL_SHORT: return load<i16>(p, k);
    case GL_UNSIGNED_SHORT: return load<u16>(p, k);
    case GL_INT: return load<i32>(p, k);
    case GL_UNSIGNED_INT: return load<u32>(p, k);
    case GL_FLOAT: return load<float>(p, k);
    case GL_DOUBLE: return load<double>(p, k);
    default: return 0.0;
    }
}

/// Component @p k converted per spec Table 2.6 (colours, normals).
float normalized_component(const u8* p, u32 type, u32 k) {
    switch (type) {
    case GL_BYTE: return gl_byte_to_float(load<i8>(p, k));
    case GL_UNSIGNED_BYTE: return gl_ubyte_to_float(load<u8>(p, k));
    case GL_SHORT: return gl_short_to_float(load<i16>(p, k));
    case GL_UNSIGNED_SHORT: return gl_ushort_to_float(load<u16>(p, k));
    case GL_INT: return gl_int_to_float(load<i32>(p, k));
    case GL_UNSIGNED_INT: return gl_uint_to_float(load<u32>(p, k));
    default: return static_cast<float>(raw_component(p, type, k));
    }
}

/// One enabled array resolved to host memory for elements [lo, hi].
struct ArrayView {
    bool on{false};
    const u8* base{nullptr};   ///< element lo
    i64 lo{0};
    i64 stride{0};
    u32 size{0};
    u32 type{0};
    [[nodiscard]] const u8* element(i64 i) const { return base + (i - lo) * stride; }
};

/// Validates the bytes elements [lo, hi] of @p a occupy (spec 2.8: element i
/// starts at pointer + i * stride; stride 0 means tightly packed).
bool map_array(const GuestMemory& memory, const GlClientArray& a, u32 size, u32 type, i64 lo, i64 hi,
               ArrayView& out) {
    out = ArrayView{};
    if (!a.enabled) return true;
    const i64 element_bytes = static_cast<i64>(size) * type_bytes(type);
    if (element_bytes == 0 || a.pointer == 0) return false;
    const i64 stride = a.stride != 0 ? a.stride : element_bytes;
    // Checked: an index near 2^32 times a stride near 2^31 does not fit i64
    // (final review, Finding 11); a range that cannot exist is unreadable.
    i64 offset = 0, span = 0, start = 0, length = 0;
    if (__builtin_mul_overflow(lo, stride, &offset) ||
        __builtin_add_overflow(static_cast<i64>(a.pointer), offset, &start) ||
        __builtin_mul_overflow(hi - lo, stride, &span) || __builtin_add_overflow(span, element_bytes, &length)) {
        return false;
    }
    if (start <= 0 || length <= 0) return false;
    const auto addr = static_cast<GuestAddr>(start);
    const auto len = static_cast<usize>(length);
    if (!memory.is_mapped(addr, len) || !memory.has_protection(addr, len, MemoryProtection::Read)) return false;
    const Byte* host = memory.host_ptr(addr, len);
    if (host == nullptr) return false;
    out.on = true;
    out.base = reinterpret_cast<const u8*>(host);
    out.lo = lo;
    out.stride = stride;
    out.size = size;
    out.type = type;
    return true;
}

struct Arrays {
    ArrayView edge, tex, color, index, normal, vertex;
};

bool map_arrays(const GlContext& ctx, const GuestMemory& memory, i64 lo, i64 hi, Arrays& out) {
    const GlVertexArrayState& va = ctx.state.vertex_array;
    // The edge-flag array holds GLboolean (one byte) elements.
    return map_array(memory, va.edge_flag, 1, GL_UNSIGNED_BYTE, lo, hi, out.edge) &&
           map_array(memory, va.tex_coord, static_cast<u32>(va.tex_coord.size), va.tex_coord.type, lo, hi, out.tex) &&
           map_array(memory, va.color, static_cast<u32>(va.color.size), va.color.type, lo, hi, out.color) &&
           map_array(memory, va.index, 1, va.index.type, lo, hi, out.index) &&
           map_array(memory, va.normal, 3, va.normal.type, lo, hi, out.normal) &&
           map_array(memory, va.vertex, static_cast<u32>(va.vertex.size), va.vertex.type, lo, hi, out.vertex);
}

using E = cmd::GlArrayElements;

/// Starts @p out for @p count elements of the arrays @p a enables.
void begin_elements(const Arrays& a, i64 count, E& out) {
    out.clear();
    out.arrays = static_cast<u8>((a.edge.on ? E::kEdge : 0) | (a.tex.on ? E::kTex : 0) | (a.color.on ? E::kColor : 0) |
                                 (a.index.on ? E::kIndex : 0) | (a.normal.on ? E::kNormal : 0) |
                                 (a.vertex.on ? E::kVertex : 0));
    out.floats_per_element = (a.edge.on ? 1u : 0u) + (a.tex.on ? 4u : 0u) + (a.color.on ? 4u : 0u) +
                             (a.index.on ? 1u : 0u) + (a.normal.on ? 3u : 0u) + (a.vertex.on ? 4u : 0u);
    out.count = static_cast<u32>(count);
    out.values.reserve(static_cast<usize>(count) * out.floats_per_element);
}

/// Appends element @p i: the values ArrayElement(i) passes on (spec 2.8,
/// Table 2.6 for colours and normals, missing components 0, 0, 0, 1).
void append_element(const Arrays& a, i64 i, E& out) {
    std::vector<float>& v = out.values;
    if (a.edge.on) v.push_back(*a.edge.element(i) != 0 ? 1.0f : 0.0f);
    if (a.tex.on) {
        const u8* p = a.tex.element(i);
        GlVec4 t{0, 0, 0, 1};
        for (u32 k = 0; k < a.tex.size; ++k) t[k] = static_cast<float>(raw_component(p, a.tex.type, k));
        v.insert(v.end(), t.begin(), t.end());
    }
    if (a.color.on) {
        const u8* p = a.color.element(i);
        GlVec4 c{0, 0, 0, 1};
        for (u32 k = 0; k < a.color.size; ++k) c[k] = normalized_component(p, a.color.type, k);
        v.insert(v.end(), c.begin(), c.end());
    }
    if (a.index.on) v.push_back(static_cast<float>(raw_component(a.index.element(i), a.index.type, 0)));
    if (a.normal.on) {
        const u8* p = a.normal.element(i);
        for (u32 k = 0; k < 3; ++k) v.push_back(normalized_component(p, a.normal.type, k));
    }
    if (a.vertex.on) {
        const u8* p = a.vertex.element(i);
        GlVec4 x{0, 0, 0, 1};
        for (u32 k = 0; k < a.vertex.size; ++k) x[k] = static_cast<float>(raw_component(p, a.vertex.type, k));
        v.insert(v.end(), x.begin(), x.end());
    }
}

/// Spec 2.8: ArrayElement is EdgeFlagv, TexCoord, Color, Index, Normal, then
/// Vertex, each for an enabled array. The vertex goes to @p vertices (a
/// draw), or through cmd::Vertex (glArrayElement) when null.
void transfer(GlContext& ctx, const E& e, u32 element, std::vector<GlBatchVertex>* vertices,
              std::vector<u8>* flags) {
    const float* p = e.values.data() + static_cast<usize>(element) * e.floats_per_element;
    if (e.arrays & E::kEdge) cmd::EdgeFlag(ctx, *p++ != 0.0f);
    if (e.arrays & E::kTex) {
        cmd::TexCoord(ctx, p[0], p[1], p[2], p[3]);
        p += 4;
    }
    if (e.arrays & E::kColor) {
        cmd::Color(ctx, p[0], p[1], p[2], p[3]);
        p += 4;
    }
    if (e.arrays & E::kIndex) cmd::Index(ctx, *p++);
    if (e.arrays & E::kNormal) {
        cmd::Normal(ctx, p[0], p[1], p[2]);
        p += 3;
    }
    if (e.arrays & E::kVertex) {
        if (vertices != nullptr) {
            vertices->push_back(snapshot(ctx, p[0], p[1], p[2], p[3]));
            flags->push_back(ctx.state.current.edge_flag ? 1 : 0);
        } else {
            cmd::Vertex(ctx, p[0], p[1], p[2], p[3]);
        }
    }
}

/// glDrawArrays / glDrawElements: Begin(mode), ArrayElement for each element,
/// End() (spec 2.8), assembled without touching the immediate-mode list. The
/// scratch vectors keep their capacity, so a draw (a replayed one too)
/// allocates nothing once they have grown.
void draw_elements(GlContext& ctx, const E& e, u32 mode) {
    if (!(e.arrays & E::kVertex)) {
        // No vertex array: the current values still change, nothing is drawn.
        for (u32 k = 0; k < e.count; ++k) transfer(ctx, e, k, nullptr, nullptr);
        return;
    }
    thread_local std::vector<GlBatchVertex> vertices;
    thread_local std::vector<u8> flags;
    vertices.clear();
    flags.clear();
    for (u32 k = 0; k < e.count; ++k) transfer(ctx, e, k, &vertices, &flags);
    gl_draw_primitives(ctx, mode, vertices, flags);
}

}  // namespace

namespace cmd {

// ---- Reading the arrays (spec 2.8; when issued, spec 5.4) --------------------------------

void gl_read_array_range(const GlContext& ctx, const GuestMemory& memory, i64 first, i64 count, GlArrayElements& out) {
    out.clear();
    if (count <= 0) return;
    Arrays a;
    if (!map_arrays(ctx, memory, first, first + count - 1, a)) {
        out.readable = false;
        return;
    }
    begin_elements(a, count, out);
    for (i64 k = 0; k < count; ++k) append_element(a, first + k, out);
}

void gl_read_array_indices(const GlContext& ctx, const GuestMemory& memory, i64 count, u32 type, GuestAddr indices,
                           GlArrayElements& out) {
    out.clear();
    if (count <= 0) return;
    const usize bytes = static_cast<usize>(count) * type_bytes(type);
    if (bytes == 0 || indices == 0 || !memory.is_mapped(indices, bytes) ||
        !memory.has_protection(indices, bytes, MemoryProtection::Read)) {
        out.readable = false;
        return;
    }
    const u8* list = reinterpret_cast<const u8*>(memory.host_ptr(indices, bytes));
    if (list == nullptr) {
        out.readable = false;
        return;
    }
    auto index_at = [&](i64 k) { return static_cast<i64>(raw_component(list, type, static_cast<u32>(k))); };
    i64 lo = std::numeric_limits<i64>::max(), hi = 0;
    for (i64 k = 0; k < count; ++k) {
        const i64 v = index_at(k);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    Arrays a;
    if (!map_arrays(ctx, memory, lo, hi, a)) {
        out.readable = false;
        return;
    }
    begin_elements(a, count, out);
    for (i64 k = 0; k < count; ++k) append_element(a, index_at(k), out);
}

// ---- Commands -------------------------------------------------------------------------

void Begin(GlContext& ctx, u32 mode) {
    if (ctx.reject_inside_begin_end()) return;
    if (gl_primitive_class(mode) == GlPrimitiveClass::None) {
        ctx.record_error(GL_INVALID_ENUM);
        return;
    }
    ctx.begin_mode = mode;
    ctx.immediate_vertices.clear();
    ctx.immediate_edge_flags.clear();
}

void End(GlContext& ctx) {
    if (!ctx.inside_begin_end()) {
        ctx.record_error(GL_INVALID_OPERATION);
        return;
    }
    const u32 mode = ctx.begin_mode;
    ctx.begin_mode = kGlOutsideBeginEnd;
    gl_draw_primitives(ctx, mode, ctx.immediate_vertices, ctx.immediate_edge_flags);
    ctx.immediate_vertices.clear();
    ctx.immediate_edge_flags.clear();
}

void Vertex(GlContext& ctx, float x, float y, float z, float w) {
    // Spec 2.6.3: outside Begin/End the effect is undefined; Windows records
    // nothing and draws nothing.
    if (!ctx.inside_begin_end()) return;
    ctx.immediate_vertices.push_back(snapshot(ctx, x, y, z, w));
    ctx.immediate_edge_flags.push_back(ctx.state.current.edge_flag ? 1 : 0);
}

void Rect(GlContext& ctx, float x1, float y1, float x2, float y2) {
    if (ctx.reject_inside_begin_end()) return;
    Begin(ctx, GL_POLYGON);
    Vertex(ctx, x1, y1, 0.0f, 1.0f);
    Vertex(ctx, x2, y1, 0.0f, 1.0f);
    Vertex(ctx, x2, y2, 0.0f, 1.0f);
    Vertex(ctx, x1, y2, 0.0f, 1.0f);
    End(ctx);
}

void ArrayElement(GlContext& ctx, const GlArrayElements& elements) {
    // Allowed inside Begin/End (spec 2.6.3, 2.8).
    if (!elements.readable) {
        ctx.record_error(GL_INVALID_OPERATION);
        return;
    }
    if (elements.count == 1) transfer(ctx, elements, 0, nullptr, nullptr);
}

void DrawArrays(GlContext& ctx, u32 mode, i32 count, const GlArrayElements& elements) {
    if (ctx.reject_inside_begin_end()) return;
    if (gl_primitive_class(mode) == GlPrimitiveClass::None) { ctx.record_error(GL_INVALID_ENUM); return; }
    if (count < 0) { ctx.record_error(GL_INVALID_VALUE); return; }
    if (count == 0) return;
    if (!elements.readable) {
        ctx.record_error(GL_INVALID_OPERATION);
        return;
    }
    draw_elements(ctx, elements, mode);
}

void DrawElements(GlContext& ctx, u32 mode, i32 count, u32 type, const GlArrayElements& elements) {
    if (ctx.reject_inside_begin_end()) return;
    if (gl_primitive_class(mode) == GlPrimitiveClass::None) { ctx.record_error(GL_INVALID_ENUM); return; }
    if (count < 0) { ctx.record_error(GL_INVALID_VALUE); return; }
    if (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT) {
        ctx.record_error(GL_INVALID_ENUM);
        return;
    }
    if (count == 0) return;
    if (!elements.readable) {
        ctx.record_error(GL_INVALID_OPERATION);
        return;
    }
    draw_elements(ctx, elements, mode);
}

void DrawRangeElements(GlContext& ctx, u32 mode, u32 start, u32 end, i32 count, u32 type,
                       const GlArrayElements& elements) {
    if (ctx.reject_inside_begin_end()) return;   // ext-dre-inside-begin
    if (end < start) { ctx.record_error(GL_INVALID_VALUE); return; }   // ext-dre-end-lt-start
    // The rest is glDrawElements (ext-dre-count-negative, -bad-mode, -bad-type,
    // -count0); indices outside [start, end] are drawn (ext-dre-index-outside-range).
    DrawElements(ctx, mode, count, type, elements);
}

}  // namespace cmd

// ---- Exports ---------------------------------------------------------------------------

namespace {

using namespace cmd;

void log_unreadable_once(const char* family, GuestAddr addr) {
    static std::mutex mutex;
    static std::unordered_set<std::string>* seen = new std::unordered_set<std::string>();
    std::lock_guard<std::mutex> lock(mutex);
    if (!seen->insert(family).second) return;
    std::fprintf(stderr,
                 "[vitreo] opengl32!%s: client memory at 0x%llx is not readable; the command is ignored "
                 "(logged once)\n",
                 family, static_cast<unsigned long long>(addr));
}

/// Reads @p n values of a vector form; false (logged once) when unreadable.
template <typename T>
bool read_client(GlCall& c, const char* family, GuestAddr addr, T* out, usize n) {
    if (addr != 0 && c.m.memory().read_bytes(addr, reinterpret_cast<Byte*>(out), n * sizeof(T)).is_ok()) {
        return true;
    }
    log_unreadable_once(family, addr);
    return false;
}

/// Spec Table 2.6: integer colour/normal components to floating point.
template <typename T>
float normalized(T v) {
    if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) return static_cast<float>(v);
    else if constexpr (std::is_same_v<T, i8>) return gl_byte_to_float(v);
    else if constexpr (std::is_same_v<T, u8>) return gl_ubyte_to_float(v);
    else if constexpr (std::is_same_v<T, i16>) return gl_short_to_float(v);
    else if constexpr (std::is_same_v<T, u16>) return gl_ushort_to_float(v);
    else if constexpr (std::is_same_v<T, i32>) return gl_int_to_float(v);
    else return gl_uint_to_float(v);
}

template <typename T, usize N>
bool gather(GlCall& c, bool vector_form, const char* family, std::array<T, 4>& raw) {
    if (vector_form) return read_client(c, family, c.p(0), raw.data(), N);
    for (usize k = 0; k < N; ++k) raw[k] = gl_arg_as<T>(c, k);
    return true;
}

template <typename T, usize N, bool V>
void color_cmd(GlCall& c) {
    std::array<T, 4> raw{};
    if (!gather<T, N>(c, V, "glColor*v", raw)) return;
    std::array<float, 4> v{0, 0, 0, 1};
    for (usize k = 0; k < N; ++k) v[k] = normalized(raw[k]);
    gl_issue<&Color>(c, v[0], v[1], v[2], v[3]);
}

template <typename T, bool V>
void normal_cmd(GlCall& c) {
    std::array<T, 4> raw{};
    if (!gather<T, 3>(c, V, "glNormal3*v", raw)) return;
    gl_issue<&Normal>(c, normalized(raw[0]), normalized(raw[1]), normalized(raw[2]));
}

template <typename T, usize N, bool V>
void texcoord_cmd(GlCall& c) {
    std::array<T, 4> raw{};
    if (!gather<T, N>(c, V, "glTexCoord*v", raw)) return;
    std::array<float, 4> v{0, 0, 0, 1};
    for (usize k = 0; k < N; ++k) v[k] = static_cast<float>(raw[k]);
    gl_issue<&TexCoord>(c, v[0], v[1], v[2], v[3]);
}

template <typename T, bool V>
void index_cmd(GlCall& c) {
    std::array<T, 4> raw{};
    if (!gather<T, 1>(c, V, "glIndex*v", raw)) return;
    gl_issue<&Index>(c, static_cast<float>(raw[0]));
}

template <typename T, usize N, bool V>
void vertex_cmd(GlCall& c) {
    // Outside Begin/End a vertex does nothing, so nothing is read; a list
    // records it wherever it is compiled (it may be called inside glBegin).
    if (!c.ctx.inside_begin_end() && !c.recording_list()) return;
    std::array<T, 4> raw{};
    if (!gather<T, N>(c, V, "glVertex*v", raw)) return;
    std::array<float, 4> v{0, 0, 0, 1};
    for (usize k = 0; k < N; ++k) v[k] = static_cast<float>(raw[k]);
    gl_issue<&Vertex>(c, v[0], v[1], v[2], v[3]);
}

template <typename T, bool V>
void rect_cmd(GlCall& c) {
    // Not allowed inside Begin/End (spec 2.6.3): checked before reading; a
    // compiled call is checked when it executes (spec 5.4).
    if (!c.recording_list() && c.ctx.reject_inside_begin_end()) return;
    std::array<T, 4> r{};
    if constexpr (V) {
        if (!read_client(c, "glRect*v", c.p(0), r.data(), 2) || !read_client(c, "glRect*v", c.p(1), r.data() + 2, 2)) {
            return;
        }
    } else {
        for (usize k = 0; k < 4; ++k) r[k] = gl_arg_as<T>(c, k);
    }
    gl_issue<&Rect>(c, static_cast<float>(r[0]), static_cast<float>(r[1]), static_cast<float>(r[2]),
         static_cast<float>(r[3]));
}

// The array commands read the client arrays when issued (spec 2.8; spec 5.4:
// "when the commands ArrayElement, DrawArrays, or DrawElements are
// accumulated into a display list"); nothing is read for arguments the
// command rejects before looking at the arrays.
thread_local GlArrayElements t_elements;   // grown once, reused by every call

void array_element_cmd(GlCall& c) {
    gl_read_array_range(c.ctx, c.m.memory(), c.i(0), 1, t_elements);
    gl_issue<&ArrayElement>(c, t_elements);
}

void draw_arrays_cmd(GlCall& c) {
    const u32 mode = c.u(0);
    const i32 count = c.i(2);
    t_elements.clear();
    if (gl_primitive_class(mode) != GlPrimitiveClass::None && count > 0) {
        gl_read_array_range(c.ctx, c.m.memory(), c.i(1), count, t_elements);
    }
    gl_issue<&DrawArrays>(c, mode, count, t_elements);
}

void draw_elements_cmd(GlCall& c) {
    const u32 mode = c.u(0);
    const i32 count = c.i(1);
    const u32 type = c.u(2);
    t_elements.clear();
    if (gl_primitive_class(mode) != GlPrimitiveClass::None && count > 0 &&
        (type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT)) {
        gl_read_array_indices(c.ctx, c.m.memory(), count, type, c.p(3), t_elements);
    }
    gl_issue<&DrawElements>(c, mode, count, type, t_elements);
}

void draw_range_elements_cmd(GlCall& c) {
    const u32 mode = c.u(0);
    const i32 count = c.i(3);
    const u32 type = c.u(4);
    t_elements.clear();
    if (gl_primitive_class(mode) != GlPrimitiveClass::None && count > 0 && c.u(2) >= c.u(1) &&
        (type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT)) {
        gl_read_array_indices(c.ctx, c.m.memory(), count, type, c.p(5), t_elements);
    }
    gl_issue<&DrawRangeElements>(c, mode, c.u(1), c.u(2), count, type, t_elements);
}

// clang-format off
const GlHandlerEntry kVertexHandlers[] = {
    // ---- begin/end, vertices, rectangles ----
    {"glBegin", [](GlCall& c) { gl_issue<&Begin>(c, c.u(0)); }},
    {"glEnd", [](GlCall& c) { gl_issue<&End>(c); }},
    {"glVertex2d", &vertex_cmd<double, 2, false>}, {"glVertex2dv", &vertex_cmd<double, 2, true>},
    {"glVertex2f", &vertex_cmd<float, 2, false>},  {"glVertex2fv", &vertex_cmd<float, 2, true>},
    {"glVertex2i", &vertex_cmd<i32, 2, false>},    {"glVertex2iv", &vertex_cmd<i32, 2, true>},
    {"glVertex2s", &vertex_cmd<i16, 2, false>},    {"glVertex2sv", &vertex_cmd<i16, 2, true>},
    {"glVertex3d", &vertex_cmd<double, 3, false>}, {"glVertex3dv", &vertex_cmd<double, 3, true>},
    {"glVertex3f", &vertex_cmd<float, 3, false>},  {"glVertex3fv", &vertex_cmd<float, 3, true>},
    {"glVertex3i", &vertex_cmd<i32, 3, false>},    {"glVertex3iv", &vertex_cmd<i32, 3, true>},
    {"glVertex3s", &vertex_cmd<i16, 3, false>},    {"glVertex3sv", &vertex_cmd<i16, 3, true>},
    {"glVertex4d", &vertex_cmd<double, 4, false>}, {"glVertex4dv", &vertex_cmd<double, 4, true>},
    {"glVertex4f", &vertex_cmd<float, 4, false>},  {"glVertex4fv", &vertex_cmd<float, 4, true>},
    {"glVertex4i", &vertex_cmd<i32, 4, false>},    {"glVertex4iv", &vertex_cmd<i32, 4, true>},
    {"glVertex4s", &vertex_cmd<i16, 4, false>},    {"glVertex4sv", &vertex_cmd<i16, 4, true>},
    {"glRectd", &rect_cmd<double, false>}, {"glRectdv", &rect_cmd<double, true>},
    {"glRectf", &rect_cmd<float, false>},  {"glRectfv", &rect_cmd<float, true>},
    {"glRecti", &rect_cmd<i32, false>},    {"glRectiv", &rect_cmd<i32, true>},
    {"glRects", &rect_cmd<i16, false>},    {"glRectsv", &rect_cmd<i16, true>},

    // ---- current values ----
    {"glColor3b", &color_cmd<i8, 3, false>},   {"glColor3bv", &color_cmd<i8, 3, true>},
    {"glColor3d", &color_cmd<double, 3, false>}, {"glColor3dv", &color_cmd<double, 3, true>},
    {"glColor3f", &color_cmd<float, 3, false>}, {"glColor3fv", &color_cmd<float, 3, true>},
    {"glColor3i", &color_cmd<i32, 3, false>},  {"glColor3iv", &color_cmd<i32, 3, true>},
    {"glColor3s", &color_cmd<i16, 3, false>},  {"glColor3sv", &color_cmd<i16, 3, true>},
    {"glColor3ub", &color_cmd<u8, 3, false>},  {"glColor3ubv", &color_cmd<u8, 3, true>},
    {"glColor3ui", &color_cmd<u32, 3, false>}, {"glColor3uiv", &color_cmd<u32, 3, true>},
    {"glColor3us", &color_cmd<u16, 3, false>}, {"glColor3usv", &color_cmd<u16, 3, true>},
    {"glColor4b", &color_cmd<i8, 4, false>},   {"glColor4bv", &color_cmd<i8, 4, true>},
    {"glColor4d", &color_cmd<double, 4, false>}, {"glColor4dv", &color_cmd<double, 4, true>},
    {"glColor4f", &color_cmd<float, 4, false>}, {"glColor4fv", &color_cmd<float, 4, true>},
    {"glColor4i", &color_cmd<i32, 4, false>},  {"glColor4iv", &color_cmd<i32, 4, true>},
    {"glColor4s", &color_cmd<i16, 4, false>},  {"glColor4sv", &color_cmd<i16, 4, true>},
    {"glColor4ub", &color_cmd<u8, 4, false>},  {"glColor4ubv", &color_cmd<u8, 4, true>},
    {"glColor4ui", &color_cmd<u32, 4, false>}, {"glColor4uiv", &color_cmd<u32, 4, true>},
    {"glColor4us", &color_cmd<u16, 4, false>}, {"glColor4usv", &color_cmd<u16, 4, true>},
    {"glNormal3b", &normal_cmd<i8, false>},    {"glNormal3bv", &normal_cmd<i8, true>},
    {"glNormal3d", &normal_cmd<double, false>}, {"glNormal3dv", &normal_cmd<double, true>},
    {"glNormal3f", &normal_cmd<float, false>}, {"glNormal3fv", &normal_cmd<float, true>},
    {"glNormal3i", &normal_cmd<i32, false>},   {"glNormal3iv", &normal_cmd<i32, true>},
    {"glNormal3s", &normal_cmd<i16, false>},   {"glNormal3sv", &normal_cmd<i16, true>},
    {"glTexCoord1d", &texcoord_cmd<double, 1, false>}, {"glTexCoord1dv", &texcoord_cmd<double, 1, true>},
    {"glTexCoord1f", &texcoord_cmd<float, 1, false>},  {"glTexCoord1fv", &texcoord_cmd<float, 1, true>},
    {"glTexCoord1i", &texcoord_cmd<i32, 1, false>},    {"glTexCoord1iv", &texcoord_cmd<i32, 1, true>},
    {"glTexCoord1s", &texcoord_cmd<i16, 1, false>},    {"glTexCoord1sv", &texcoord_cmd<i16, 1, true>},
    {"glTexCoord2d", &texcoord_cmd<double, 2, false>}, {"glTexCoord2dv", &texcoord_cmd<double, 2, true>},
    {"glTexCoord2f", &texcoord_cmd<float, 2, false>},  {"glTexCoord2fv", &texcoord_cmd<float, 2, true>},
    {"glTexCoord2i", &texcoord_cmd<i32, 2, false>},    {"glTexCoord2iv", &texcoord_cmd<i32, 2, true>},
    {"glTexCoord2s", &texcoord_cmd<i16, 2, false>},    {"glTexCoord2sv", &texcoord_cmd<i16, 2, true>},
    {"glTexCoord3d", &texcoord_cmd<double, 3, false>}, {"glTexCoord3dv", &texcoord_cmd<double, 3, true>},
    {"glTexCoord3f", &texcoord_cmd<float, 3, false>},  {"glTexCoord3fv", &texcoord_cmd<float, 3, true>},
    {"glTexCoord3i", &texcoord_cmd<i32, 3, false>},    {"glTexCoord3iv", &texcoord_cmd<i32, 3, true>},
    {"glTexCoord3s", &texcoord_cmd<i16, 3, false>},    {"glTexCoord3sv", &texcoord_cmd<i16, 3, true>},
    {"glTexCoord4d", &texcoord_cmd<double, 4, false>}, {"glTexCoord4dv", &texcoord_cmd<double, 4, true>},
    {"glTexCoord4f", &texcoord_cmd<float, 4, false>},  {"glTexCoord4fv", &texcoord_cmd<float, 4, true>},
    {"glTexCoord4i", &texcoord_cmd<i32, 4, false>},    {"glTexCoord4iv", &texcoord_cmd<i32, 4, true>},
    {"glTexCoord4s", &texcoord_cmd<i16, 4, false>},    {"glTexCoord4sv", &texcoord_cmd<i16, 4, true>},
    {"glIndexd", &index_cmd<double, false>}, {"glIndexdv", &index_cmd<double, true>},
    {"glIndexf", &index_cmd<float, false>},  {"glIndexfv", &index_cmd<float, true>},
    {"glIndexi", &index_cmd<i32, false>},    {"glIndexiv", &index_cmd<i32, true>},
    {"glIndexs", &index_cmd<i16, false>},    {"glIndexsv", &index_cmd<i16, true>},
    {"glIndexub", &index_cmd<u8, false>},    {"glIndexubv", &index_cmd<u8, true>},
    {"glEdgeFlag", [](GlCall& c) { gl_issue<&EdgeFlag>(c, c.b(0)); }},
    {"glEdgeFlagv", [](GlCall& c) {
        u8 flag = 0;
        if (read_client(c, "glEdgeFlagv", c.p(0), &flag, 1)) gl_issue<&EdgeFlag>(c, flag != 0);
    }},

    // ---- client arrays ----
    {"glEnableClientState", [](GlCall& c) { gl_issue<&EnableClientState>(c, c.u(0)); }},
    {"glDisableClientState", [](GlCall& c) { gl_issue<&DisableClientState>(c, c.u(0)); }},
    {"glVertexPointer", [](GlCall& c) { gl_issue<&VertexPointer>(c, c.i(0), c.u(1), c.i(2), c.p(3)); }},
    {"glNormalPointer", [](GlCall& c) { gl_issue<&NormalPointer>(c, c.u(0), c.i(1), c.p(2)); }},
    {"glColorPointer", [](GlCall& c) { gl_issue<&ColorPointer>(c, c.i(0), c.u(1), c.i(2), c.p(3)); }},
    {"glIndexPointer", [](GlCall& c) { gl_issue<&IndexPointer>(c, c.u(0), c.i(1), c.p(2)); }},
    {"glTexCoordPointer", [](GlCall& c) { gl_issue<&TexCoordPointer>(c, c.i(0), c.u(1), c.i(2), c.p(3)); }},
    {"glEdgeFlagPointer", [](GlCall& c) { gl_issue<&EdgeFlagPointer>(c, c.i(0), c.p(1)); }},
    {"glInterleavedArrays", [](GlCall& c) { gl_issue<&InterleavedArrays>(c, c.u(0), c.i(1), c.p(2)); }},
    {"glArrayElement", &array_element_cmd},
    {"glDrawArrays", &draw_arrays_cmd},
    {"glDrawElements", &draw_elements_cmd},
};
// clang-format on

}  // namespace

std::span<const GlHandlerEntry> gl_vertex_export_handlers() { return kVertexHandlers; }
GlHandler gl_draw_range_elements_handler() { return &draw_range_elements_cmd; }

}  // namespace vitreo::winapi::gl
