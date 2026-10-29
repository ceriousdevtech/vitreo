// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_exports.cpp
/// @brief GL 1.1 export handlers: guest arguments -> internal commands.
///
/// Integer arguments narrower than 32 bits (GLbyte, GLshort, GLubyte,
/// GLushort, GLboolean) arrive promoted to a 32-bit slot whose upper bits the
/// caller may leave undefined, so each handler narrows to the declared type.
/// Pointer arguments are read from guest memory; an unreadable array makes the
/// command do nothing (Windows would fault in the caller's address space).
/// The current-value, vertex and vertex-array exports are gl_cmd_vertex.cpp's.

#include "gl_exports.hpp"
#include "gl_client_memory.hpp"
#include "gl_commands.hpp"
#include "gl_pixels.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <cstdio>
#include <cstring>
#include <type_traits>
#include <unordered_map>

namespace vitreo::winapi::gl {

// ---- GlCall ------------------------------------------------------------------

GlExportSlots gl_export_slots(std::string_view kinds) noexcept {
    GlExportSlots s;
    u8 slot = 0;
    for (char k : kinds) {
        if (s.count >= s.x86.size()) break;
        s.x86[s.count++] = slot;
        slot = static_cast<u8>(slot + (k == 'd' ? 2 : 1));
    }
    return s;
}

usize GlCall::slot(usize n) const noexcept {
    return m.cpu_is_64bit() ? n : m_slots.x86[n];
}

u32 GlCall::u(usize n) const { return m.arg32(slot(n)); }
float GlCall::f(usize n) const { return m.arg_f32(slot(n)); }
double GlCall::d(usize n) const { return m.arg_f64(slot(n)); }
GuestAddr GlCall::p(usize n) const {
    const u64 v = m.arg(slot(n));
    return static_cast<GuestAddr>(m.cpu_is_64bit() ? v : (v & 0xFFFFFFFFu));
}
void GlCall::ret(u64 value) { m.set_return(value); }

// ---- Client memory -----------------------------------------------------------

namespace {

/// Rule 3: a host copy that cannot be had is GL_OUT_OF_MEMORY.
bool client_status_ok(GlCall& c, GlClientStatus status) {
    if (status == GlClientStatus::OutOfMemory) c.ctx.record_error(GL_OUT_OF_MEMORY);
    return status == GlClientStatus::Ok;
}

}  // namespace

cmd::GlClientPixels gl_client_pixels(GlCall& c, GuestAddr addr, const GlPixelLayout& layout,
                                     std::vector<u8>& storage) {
    cmd::GlClientPixels px;
    px.unpack = &c.ctx.state.pixel_store.unpack;
    if (addr == 0) {
        px.null_pointer = true;
        return px;
    }
    if (!layout.valid ||
        !client_status_ok(c, gl_read_client(c.m.memory(), addr, layout.span_bytes, MemoryProtection::Read, storage))) {
        px.readable = false;
        return px;
    }
    px.bytes = storage;
    return px;
}

bool gl_read_client_bytes(GlCall& c, GuestAddr addr, usize bytes, MemoryProtection access, std::vector<u8>& out) {
    if (bytes == 0) {
        out.clear();
        return true;
    }
    return client_status_ok(c, gl_read_client(c.m.memory(), addr, bytes, access, out));
}

std::span<u8> GlClientOutput::open(void* user, usize size) {
    auto& o = *static_cast<GlClientOutput*>(user);
    if (o.addr == 0) return {};
    if (!client_status_ok(*o.call, gl_read_client(o.call->m.memory(), o.addr, size, MemoryProtection::ReadWrite,
                                                  o.bytes))) {
        return {};
    }
    return o.bytes;
}

void GlClientOutput::write_back() {
    if (addr == 0 || bytes.empty()) return;
    (void)call->m.memory().write_bytes(addr, bytes.data(), bytes.size());
}

GlDisplayList* gl_recording_list(GlContext& ctx, const GlListRule& rule, u32 arg0) noexcept {
    if (!ctx.compiling_list || rule.never_compiled) return nullptr;
    if (rule.proxy_target != 0 && arg0 == rule.proxy_target) return nullptr;
    return ctx.compiling_list.get();
}

GlDisplayList* GlCall::recording_list() const {
    if (!ctx.compiling_list || m_list_rule.never_compiled) return nullptr;
    return gl_recording_list(ctx, m_list_rule, m_list_rule.proxy_target != 0 ? u(0) : 0);
}

namespace {

using namespace cmd;

// ---- Guest memory ------------------------------------------------------------

/// Reads @p n values into @p out, which the caller sized (a fixed array, or
/// a vector its own argument checks bound).
template <typename T>
bool read_array(GlCall& c, GuestAddr addr, T* out, usize n) {
    if (n == 0) return true;
    if (addr == 0) return false;
    return c.m.memory().read_bytes(addr, reinterpret_cast<Byte*>(out), n * sizeof(T)).is_ok();
}

template <typename T>
bool write_array(GlCall& c, GuestAddr addr, const T* values, usize n) {
    if (n == 0) return true;
    if (addr == 0) return false;
    return c.m.memory().write_bytes(addr, reinterpret_cast<const u8*>(values), n * sizeof(T)).is_ok();
}

/// Writes a query result as GLboolean / GLint / GLfloat / GLdouble (spec 6.1.2).
template <typename T>
void write_query(GlCall& c, GuestAddr addr, const GlQuery& q) {
    std::array<T, 16> out{};
    for (u32 k = 0; k < q.count; ++k) {
        if constexpr (std::is_same_v<T, u8>) out[k] = gl_query_to_boolean(q.v[k]);
        else if constexpr (std::is_same_v<T, i32>) out[k] = gl_query_to_integer(q.kind, q.v[k]);
        else out[k] = static_cast<T>(q.v[k]);
    }
    (void)write_array(c, addr, out.data(), q.count);
}

// ---- Typed argument access -----------------------------------------------------


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
bool gather(GlCall& c, bool vector_form, std::array<T, 4>& raw) {
    if (vector_form) return read_array(c, c.p(0), raw.data(), N);
    for (usize k = 0; k < N; ++k) raw[k] = gl_arg_as<T>(c, k);
    return true;
}

template <typename T>
void load_matrix_cmd(GlCall& c) {
    std::array<T, 16> raw{};
    if (!read_array(c, c.p(0), raw.data(), 16)) return;
    GlMatrix m{};
    for (u32 k = 0; k < 16; ++k) m[k] = static_cast<double>(raw[k]);
    gl_issue<&LoadMatrix>(c, m);
}

template <typename T>
void mult_matrix_cmd(GlCall& c) {
    std::array<T, 16> raw{};
    if (!read_array(c, c.p(0), raw.data(), 16)) return;
    GlMatrix m{};
    for (u32 k = 0; k < 16; ++k) m[k] = static_cast<double>(raw[k]);
    gl_issue<&MultMatrix>(c, m);
}

// ---- {f,i,d}{,v} parameter families --------------------------------------------

/// Value counts per pname for the vector forms (unknown pnames read nothing;
/// the command then reports GL_INVALID_ENUM).
u32 light_count(u32 pname) {
    switch (pname) {
    case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_POSITION: return 4;
    case GL_SPOT_DIRECTION: return 3;
    case GL_SPOT_EXPONENT: case GL_SPOT_CUTOFF: case GL_CONSTANT_ATTENUATION:
    case GL_LINEAR_ATTENUATION: case GL_QUADRATIC_ATTENUATION: return 1;
    default: return 0;
    }
}
u32 material_count(u32 pname) {
    switch (pname) {
    case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_EMISSION: case GL_AMBIENT_AND_DIFFUSE: return 4;
    case GL_COLOR_INDEXES: return 3;
    case GL_SHININESS: return 1;
    default: return 0;
    }
}
u32 light_model_count(u32 pname) {
    return pname == GL_LIGHT_MODEL_AMBIENT ? 4
           : (pname == GL_LIGHT_MODEL_LOCAL_VIEWER || pname == GL_LIGHT_MODEL_TWO_SIDE) ? 1 : 0;
}
u32 fog_count(u32 pname) {
    switch (pname) {
    case GL_FOG_COLOR: return 4;
    case GL_FOG_MODE: case GL_FOG_DENSITY: case GL_FOG_START: case GL_FOG_END: case GL_FOG_INDEX: return 1;
    default: return 0;
    }
}
u32 tex_env_count(u32 pname) {
    return pname == GL_TEXTURE_ENV_COLOR ? 4 : pname == GL_TEXTURE_ENV_MODE ? 1 : 0;
}
u32 tex_gen_count(u32 pname) {
    return (pname == GL_OBJECT_PLANE || pname == GL_EYE_PLANE) ? 4 : pname == GL_TEXTURE_GEN_MODE ? 1 : 0;
}
u32 tex_param_count(u32 pname) {
    switch (pname) {
    case GL_TEXTURE_BORDER_COLOR: return 4;
    case GL_TEXTURE_MIN_FILTER: case GL_TEXTURE_MAG_FILTER: case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T: case GL_TEXTURE_PRIORITY: return 1;
    default: return 0;
    }
}

/// The values of a *v call (pointer at argument @p ptr_arg).
template <typename T>
bool read_values(GlCall& c, usize ptr_arg, u32 count, GlValues& out) {
    std::array<T, 4> raw{};
    if (count > 0 && !read_array(c, c.p(ptr_arg), raw.data(), count)) return false;
    out = GlValues{};
    out.count = static_cast<u8>(count);
    out.from_int = std::is_integral_v<T>;
    for (u32 k = 0; k < count; ++k) out.v[k] = static_cast<double>(raw[k]);
    return true;
}

template <typename T>
GlValues scalar_arg(GlCall& c, usize n) {
    return GlValues::scalar_value(static_cast<double>(gl_arg_as<T>(c, n)), std::is_integral_v<T>);
}

// glFoo{f,i}(enum, T) / glFoo{f,i}v(enum, const T*)
template <typename T, void (*Cmd)(GlContext&, u32, const GlValues&)>
void p1_scalar(GlCall& c) { gl_issue<Cmd>(c, c.u(0), scalar_arg<T>(c, 1)); }
template <typename T, u32 (*Count)(u32), void (*Cmd)(GlContext&, u32, const GlValues&)>
void p1_vector(GlCall& c) {
    GlValues v;
    if (read_values<T>(c, 1, Count(c.u(0)), v)) gl_issue<Cmd>(c, c.u(0), v);
}
// glFoo{f,i,d}(enum, enum, T) / glFoo{f,i,d}v(enum, enum, const T*)
template <typename T, void (*Cmd)(GlContext&, u32, u32, const GlValues&)>
void p2_scalar(GlCall& c) { gl_issue<Cmd>(c, c.u(0), c.u(1), scalar_arg<T>(c, 2)); }
template <typename T, u32 (*Count)(u32), void (*Cmd)(GlContext&, u32, u32, const GlValues&)>
void p2_vector(GlCall& c) {
    GlValues v;
    if (read_values<T>(c, 2, Count(c.u(1)), v)) gl_issue<Cmd>(c, c.u(0), c.u(1), v);
}
// glGetFoo{f,i,d}v(enum, enum, T*)
template <typename T, bool (*Cmd)(GlContext&, u32, u32, GlQuery&)>
void p2_get(GlCall& c) {
    GlQuery q;
    if (Cmd(c.ctx, c.u(0), c.u(1), q)) write_query<T>(c, c.p(2), q);
}

template <typename T>
void get_simple(GlCall& c) {
    GlQuery q;
    if (Get(c.ctx, c.u(0), q)) write_query<T>(c, c.p(1), q);
}

template <typename T>
void get_tex_level(GlCall& c) {
    GlQuery q;
    if (GetTexLevelParameter(c.ctx, c.u(0), c.i(1), c.u(2), q)) write_query<T>(c, c.p(3), q);
}

template <typename T>
void get_map(GlCall& c) {
    std::vector<double> values;
    GlQueryKind kind{};
    if (!GetMap(c.ctx, c.u(0), c.u(1), values, kind)) return;
    std::vector<T> out(values.size());
    for (usize k = 0; k < values.size(); ++k) {
        if constexpr (std::is_same_v<T, i32>) out[k] = gl_query_to_integer(kind, values[k]);
        else out[k] = static_cast<T>(values[k]);
    }
    (void)write_array(c, c.p(2), out.data(), out.size());
}

// ---- Pixel maps (spec 3.6.3 / 4.3.2) -------------------------------------------------

template <typename T>
void pixel_map_cmd(GlCall& c) {
    const u32 map = c.u(0);
    const i32 size = c.i(1);
    std::vector<float> values;
    if (size >= 1 && static_cast<u32>(size) <= kMaxPixelMapTable) {
        std::vector<T> raw(static_cast<usize>(size));
        if (!read_array(c, c.p(2), raw.data(), raw.size())) return;
        values.resize(raw.size());
        const bool index = gl_pixel_map_is_index_valued(map);
        for (usize k = 0; k < raw.size(); ++k) {
            // Colour entries convert per Table 2.6; index entries are values.
            if constexpr (std::is_same_v<T, float>) values[k] = raw[k];
            else values[k] = index ? static_cast<float>(raw[k]) : normalized(raw[k]);
        }
    }
    gl_issue<&PixelMap>(c, map, size, values);
}

template <typename T>
void get_pixel_map_cmd(GlCall& c) {
    const u32 map = c.u(0);
    const std::vector<float>* stored = GetPixelMap(c.ctx, map);
    if (!stored) return;
    const bool index = gl_pixel_map_is_index_valued(map);
    std::vector<T> out(stored->size());
    for (usize k = 0; k < stored->size(); ++k) {
        const double v = (*stored)[k];
        if constexpr (std::is_same_v<T, float>) {
            out[k] = static_cast<float>(v);
        } else {
            constexpr double kMax = std::is_same_v<T, u16> ? 65535.0 : 4294967295.0;
            // Index entries round; colour entries use Table 4.7's unsigned conversion.
            const double x = index ? std::round(v) : std::round(std::clamp(v, 0.0, 1.0) * kMax);
            out[k] = static_cast<T>(std::clamp(x, 0.0, kMax));
        }
    }
    (void)write_array(c, c.p(1), out.data(), out.size());
}

// ---- Evaluator maps (control points gathered with the guest stride) ----------------

template <typename T>
void map1_cmd(GlCall& c) {
    const u32 target = c.u(0);
    const double u1 = gl_arg_as<T>(c, 1);
    const double u2 = gl_arg_as<T>(c, 2);
    const i32 stride = c.i(3);
    const i32 order = c.i(4);
    const u32 k = gl_map_components(target);
    std::vector<float> points;
    if (k > 0 && stride >= static_cast<i32>(k) && order >= 1 && order <= static_cast<i32>(kMaxEvalOrder)) {
        points.resize(static_cast<usize>(order) * k);
        for (i32 n = 0; n < order; ++n) {
            std::array<T, 4> raw{};
            const GuestAddr at = c.p(5) + static_cast<GuestAddr>(n) * static_cast<GuestAddr>(stride) * sizeof(T);
            if (!read_array(c, at, raw.data(), k)) return;
            for (u32 j = 0; j < k; ++j) points[static_cast<usize>(n) * k + j] = static_cast<float>(raw[j]);
        }
    }
    gl_issue<&Map1>(c, target, u1, u2, stride, order, points);
}

template <typename T>
void map2_cmd(GlCall& c) {
    const u32 target = c.u(0);
    const double u1 = gl_arg_as<T>(c, 1);
    const double u2 = gl_arg_as<T>(c, 2);
    const i32 ustride = c.i(3);
    const i32 uorder = c.i(4);
    const double v1 = gl_arg_as<T>(c, 5);
    const double v2 = gl_arg_as<T>(c, 6);
    const i32 vstride = c.i(7);
    const i32 vorder = c.i(8);
    const i32 k = static_cast<i32>(gl_map_components(target));
    const i32 max_order = static_cast<i32>(kMaxEvalOrder);
    std::vector<float> points;
    if (k > 0 && ustride >= k && vstride >= k && uorder >= 1 && uorder <= max_order && vorder >= 1 &&
        vorder <= max_order) {
        points.resize(static_cast<usize>(uorder * vorder * k));
        for (i32 a = 0; a < uorder; ++a) {
            for (i32 bi = 0; bi < vorder; ++bi) {
                std::array<T, 4> raw{};
                const GuestAddr at = c.p(9) + (static_cast<GuestAddr>(a) * static_cast<GuestAddr>(ustride) +
                                               static_cast<GuestAddr>(bi) * static_cast<GuestAddr>(vstride)) *
                                                  sizeof(T);
                if (!read_array(c, at, raw.data(), static_cast<usize>(k))) return;
                for (i32 j = 0; j < k; ++j) {
                    points[static_cast<usize>((a * vorder + bi) * k + j)] = static_cast<float>(raw[static_cast<usize>(j)]);
                }
            }
        }
    }
    gl_issue<&Map2>(c, target, u1, u2, ustride, uorder, v1, v2, vstride, vorder, points);
}

template <typename T, usize N, bool V>
void eval_coord_cmd(GlCall& c) {
    std::array<T, 4> raw{};
    if (!gather<T, N>(c, V, raw)) return;
    if constexpr (N == 1) gl_issue<&EvalCoord1>(c, raw[0]);
    else gl_issue<&EvalCoord2>(c, raw[0], raw[1]);
}

// ---- glGetString -------------------------------------------------------------------

void get_string_cmd(GlCall& c) {
    const u32 name = c.u(0);
    const char* s = GetString(c.ctx, name);
    if (!s) {
        c.ret(0);
        return;
    }
    // One fixed slot per name, so repeated calls return the same pointer
    // (glgetstring-GL_VERSION-stable same-pointer=1).
    const GuestAddr addr = c.string_base() + (name - GL_VENDOR) * kGlStringSlotSize;
    (void)c.m.memory().write_bytes(addr, reinterpret_cast<const u8*>(s), std::strlen(s) + 1);
    c.ret(addr);
}

// ---- Polygon stipple (client memory per the pixel-store modes) ---------------------------

void polygon_stipple_cmd(GlCall& c) {
    // Refused inside Begin/End before reading; a compiled call is checked
    // when it executes (spec 5.4: an erroneous command is still compiled).
    if (!c.recording_list() && c.ctx.reject_inside_begin_end()) return;
    // The stipple is a 32 x 32 BITMAP image under the UNPACK_* modes.
    const GlPixelStoreModes& modes = c.ctx.state.pixel_store.unpack;
    std::vector<u8> storage;
    const GlClientPixels px =
        gl_client_pixels(c, c.p(0), gl_pixel_layout(modes, GL_COLOR_INDEX, GL_BITMAP, 32, 32), storage);
    if (px.null_pointer || !px.readable) return;   // Windows faults; no effect
    gl_issue<&PolygonStipple>(c, gl_unpack_stipple(modes, px.bytes));
}

void get_polygon_stipple_cmd(GlCall& c) {
    std::array<u32, 32> rows{};
    if (!GetPolygonStipple(c.ctx, rows)) return;
    const GlPixelStoreModes& modes = c.ctx.state.pixel_store.pack;
    const GlPixelLayout layout = gl_pixel_layout(modes, GL_COLOR_INDEX, GL_BITMAP, 32, 32);
    if (!layout.valid) return;   // no address range holds it: Windows faults
    // Only the stipple's own bits change (read-modify-write).
    GlClientOutput out{&c, c.p(0), {}};
    if (GlClientOutput::open(&out, layout.span_bytes).size() < layout.span_bytes) return;
    gl_pack_stipple(modes, rows, out.bytes);
    out.write_back();
}

// ---- Texture images and objects (gl_cmd_texture.cpp) ---------------------------

/// The client bytes an unpacking image command reads (eq. 3.8-3.10 under the
/// UNPACK_* modes). Nothing is read when the arguments cannot describe an
/// image the command would accept (the command then records its error).
GlClientPixels client_pixels(GlCall& c, GuestAddr addr, u32 format, u32 type, i32 w, i32 h,
                             std::vector<u8>& storage) {
    constexpr i32 kLargest = static_cast<i32>(kMaxTextureSize) + 2;
    if (addr != 0 && (w <= 0 || h <= 0 || w > kLargest || h > kLargest || !gl_pixel_type_valid(type, true) ||
                      gl_pixel_format_elements(format) == 0)) {
        GlClientPixels px;
        px.unpack = &c.ctx.state.pixel_store.unpack;
        return px;
    }
    return gl_client_pixels(c, addr, gl_pixel_layout(c.ctx.state.pixel_store.unpack, format, type, w, h), storage);
}

void tex_image_1d_cmd(GlCall& c) {
    std::vector<u8> storage;
    const GlClientPixels px = client_pixels(c, c.p(7), c.u(5), c.u(6), c.i(3), 1, storage);
    gl_issue<&TexImage>(c, 1, c.u(0), c.i(1), c.i(2), c.i(3), 1, c.i(4), c.u(5), c.u(6), px);
}

void tex_image_2d_cmd(GlCall& c) {
    std::vector<u8> storage;
    const GlClientPixels px = client_pixels(c, c.p(8), c.u(6), c.u(7), c.i(3), c.i(4), storage);
    gl_issue<&TexImage>(c, 2, c.u(0), c.i(1), c.i(2), c.i(3), c.i(4), c.i(5), c.u(6), c.u(7), px);
}

void tex_sub_image_1d_cmd(GlCall& c) {
    std::vector<u8> storage;
    const GlClientPixels px = client_pixels(c, c.p(6), c.u(4), c.u(5), c.i(3), 1, storage);
    gl_issue<&TexSubImage>(c, 1, c.u(0), c.i(1), c.i(2), 0, c.i(3), 1, c.u(4), c.u(5), px);
}

void tex_sub_image_2d_cmd(GlCall& c) {
    std::vector<u8> storage;
    const GlClientPixels px = client_pixels(c, c.p(8), c.u(6), c.u(7), c.i(4), c.i(5), storage);
    gl_issue<&TexSubImage>(c, 2, c.u(0), c.i(1), c.i(2), c.i(3), c.i(4), c.i(5), c.u(6), c.u(7), px);
}

void get_tex_image_cmd(GlCall& c) {
    GlClientOutput out{&c, c.p(4), {}};
    if (!GetTexImage(c.ctx, c.u(0), c.i(1), c.u(2), c.u(3), &GlClientOutput::open, &out)) return;
    out.write_back();
}

/// The n names at @p addr (n < 0: none; the command records the error),
/// read only once the guest range is known to hold them.
bool read_names(GlCall& c, i32 n, GuestAddr addr, std::vector<u32>& names) {
    names.clear();
    if (n <= 0) return true;
    std::vector<u8> bytes;
    if (!gl_read_client_bytes(c, addr, static_cast<usize>(n) * sizeof(u32), MemoryProtection::Read, bytes)) {
        return false;
    }
    if (!gl_try_resize(names, static_cast<usize>(n))) {
        c.ctx.record_error(GL_OUT_OF_MEMORY);
        return false;
    }
    std::memcpy(names.data(), bytes.data(), bytes.size());
    return true;
}

void gen_textures_cmd(GlCall& c) {
    // The n names go to the client's array: it must be writable before n
    // names are made (Windows faults in the caller otherwise; no effect).
    const i32 n = c.i(0);
    if (n > 0 && !gl_client_range_accessible(c.m.memory(), c.p(1), static_cast<usize>(n) * sizeof(u32),
                                             MemoryProtection::ReadWrite)) {
        (void)c.ctx.reject_inside_begin_end();
        return;
    }
    std::vector<u32> names;
    GenTextures(c.ctx, n, names);
    (void)write_array(c, c.p(1), names.data(), names.size());
}

void delete_textures_cmd(GlCall& c) {
    std::vector<u32> names;
    if (!read_names(c, c.i(0), c.p(1), names)) {
        (void)c.ctx.reject_inside_begin_end();   // spec 2.6.3 still applies
        return;
    }
    gl_issue<&DeleteTextures>(c, c.i(0), std::span<const u32>(names));
}

void are_textures_resident_cmd(GlCall& c) {
    std::vector<u32> names;
    if (!read_names(c, c.i(0), c.p(1), names)) {
        (void)c.ctx.reject_inside_begin_end();
        c.ret(0);
        return;
    }
    std::vector<u8> residences;
    const GlResidency r = AreTexturesResident(c.ctx, c.i(0), names, residences);
    if (r.write_residences) (void)write_array(c, c.p(2), residences.data(), residences.size());
    c.ret(r.all_resident ? 1 : 0);
}

void prioritize_textures_cmd(GlCall& c) {
    std::vector<u32> names;
    std::vector<u8> bytes;
    const bool names_read = read_names(c, c.i(0), c.p(1), names) &&
                            gl_read_client_bytes(c, c.p(2), names.size() * sizeof(float), MemoryProtection::Read,
                                                 bytes);
    std::vector<float> priorities(names_read ? names.size() : 0u, 0.0f);
    if (names_read && !bytes.empty()) std::memcpy(priorities.data(), bytes.data(), bytes.size());
    if (!names_read) {
        (void)c.ctx.reject_inside_begin_end();
        return;
    }
    gl_issue<&PrioritizeTextures>(c, c.i(0), names, priorities);
}

/// glCallLists: the offsets are read from client memory when issued (and so
/// when compiled); nothing is read when n <= 0 or the type is invalid (the
/// command then records its error).
void call_lists_cmd(GlCall& c) {
    const i32 n = c.i(0);
    const u32 type = c.u(1);
    // Scratch reused across calls, trimmed after (gl_client_memory.hpp rule 4).
    thread_local std::vector<u8> bytes;
    thread_local std::vector<u32> offsets;
    offsets.clear();
    const u32 size = gl_call_lists_type_bytes(type);
    if (n > 0 && size != 0) {
        // Nothing of n's size is allocated before the guest range is known
        // to hold the n offsets (final review, Finding 3 / Task 12 M1).
        const bool read =
            gl_read_client_bytes(c, c.p(2), static_cast<usize>(n) * size, MemoryProtection::Read, bytes);
        if (read) gl_decode_call_lists(type, bytes, n, offsets);
        gl_trim_scratch(bytes);
        if (!read) return;   // unreadable: no effect (Windows faults)
    }
    gl_issue<&CallLists>(c, n, type, std::span<const u32>(offsets));
    gl_trim_scratch(offsets);
}

using Entry = GlHandlerEntry;

// clang-format off
const Entry kHandlers[] = {
    // ---- errors, strings, queries, enables ----
    {"glGetError", [](GlCall& c) { c.ret(GetError(c.ctx)); }},
    {"glGetString", &get_string_cmd},
    {"glGetBooleanv", &get_simple<u8>},
    {"glGetIntegerv", &get_simple<i32>},
    {"glGetFloatv", &get_simple<float>},
    {"glGetDoublev", &get_simple<double>},
    {"glGetPointerv", [](GlCall& c) {
        GuestAddr value = 0;
        if (GetPointer(c.ctx, c.u(0), value)) (void)c.m.write_ptr(c.p(1), value);
    }},
    {"glEnable", [](GlCall& c) { gl_issue<&Enable>(c, c.u(0)); }},
    {"glDisable", [](GlCall& c) { gl_issue<&Disable>(c, c.u(0)); }},
    {"glIsEnabled", [](GlCall& c) { c.ret(IsEnabled(c.ctx, c.u(0)) ? 1 : 0); }},

    // ---- transformation ----
    {"glMatrixMode", [](GlCall& c) { gl_issue<&MatrixMode>(c, c.u(0)); }},
    {"glLoadIdentity", [](GlCall& c) { gl_issue<&LoadIdentity>(c); }},
    {"glLoadMatrixf", &load_matrix_cmd<float>},
    {"glLoadMatrixd", &load_matrix_cmd<double>},
    {"glMultMatrixf", &mult_matrix_cmd<float>},
    {"glMultMatrixd", &mult_matrix_cmd<double>},
    {"glRotatef", [](GlCall& c) { gl_issue<&Rotate>(c, c.f(0), c.f(1), c.f(2), c.f(3)); }},
    {"glRotated", [](GlCall& c) { gl_issue<&Rotate>(c, c.d(0), c.d(1), c.d(2), c.d(3)); }},
    {"glScalef", [](GlCall& c) { gl_issue<&Scale>(c, c.f(0), c.f(1), c.f(2)); }},
    {"glScaled", [](GlCall& c) { gl_issue<&Scale>(c, c.d(0), c.d(1), c.d(2)); }},
    {"glTranslatef", [](GlCall& c) { gl_issue<&Translate>(c, c.f(0), c.f(1), c.f(2)); }},
    {"glTranslated", [](GlCall& c) { gl_issue<&Translate>(c, c.d(0), c.d(1), c.d(2)); }},
    {"glFrustum", [](GlCall& c) { gl_issue<&Frustum>(c, c.d(0), c.d(1), c.d(2), c.d(3), c.d(4), c.d(5)); }},
    {"glOrtho", [](GlCall& c) { gl_issue<&Ortho>(c, c.d(0), c.d(1), c.d(2), c.d(3), c.d(4), c.d(5)); }},
    {"glPushMatrix", [](GlCall& c) { gl_issue<&PushMatrix>(c); }},
    {"glPopMatrix", [](GlCall& c) { gl_issue<&PopMatrix>(c); }},
    {"glViewport", [](GlCall& c) { gl_issue<&Viewport>(c, c.i(0), c.i(1), c.i(2), c.i(3)); }},
    {"glDepthRange", [](GlCall& c) { gl_issue<&DepthRange>(c, c.d(0), c.d(1)); }},
    {"glClipPlane", [](GlCall& c) {
        std::array<double, 4> eq{};
        if (read_array(c, c.p(1), eq.data(), 4)) gl_issue<&ClipPlane>(c, c.u(0), eq);
    }},
    {"glGetClipPlane", [](GlCall& c) {
        std::array<double, 4> eq{};
        if (GetClipPlane(c.ctx, c.u(0), eq)) (void)write_array(c, c.p(1), eq.data(), 4);
    }},

    // ---- lighting, colouring, fog ----
    {"glShadeModel", [](GlCall& c) { gl_issue<&ShadeModel>(c, c.u(0)); }},
    {"glLightf", &p2_scalar<float, &Light>},
    {"glLighti", &p2_scalar<i32, &Light>},
    {"glLightfv", &p2_vector<float, &light_count, &Light>},
    {"glLightiv", &p2_vector<i32, &light_count, &Light>},
    {"glGetLightfv", &p2_get<float, &GetLight>},
    {"glGetLightiv", &p2_get<i32, &GetLight>},
    {"glLightModelf", &p1_scalar<float, &LightModel>},
    {"glLightModeli", &p1_scalar<i32, &LightModel>},
    {"glLightModelfv", &p1_vector<float, &light_model_count, &LightModel>},
    {"glLightModeliv", &p1_vector<i32, &light_model_count, &LightModel>},
    {"glMaterialf", &p2_scalar<float, &Material>},
    {"glMateriali", &p2_scalar<i32, &Material>},
    {"glMaterialfv", &p2_vector<float, &material_count, &Material>},
    {"glMaterialiv", &p2_vector<i32, &material_count, &Material>},
    {"glGetMaterialfv", &p2_get<float, &GetMaterial>},
    {"glGetMaterialiv", &p2_get<i32, &GetMaterial>},
    {"glColorMaterial", [](GlCall& c) { gl_issue<&ColorMaterial>(c, c.u(0), c.u(1)); }},
    {"glFogf", &p1_scalar<float, &Fog>},
    {"glFogi", &p1_scalar<i32, &Fog>},
    {"glFogfv", &p1_vector<float, &fog_count, &Fog>},
    {"glFogiv", &p1_vector<i32, &fog_count, &Fog>},

    // ---- rasterization ----
    {"glPointSize", [](GlCall& c) { gl_issue<&PointSize>(c, c.f(0)); }},
    {"glLineWidth", [](GlCall& c) { gl_issue<&LineWidth>(c, c.f(0)); }},
    {"glLineStipple", [](GlCall& c) { gl_issue<&LineStipple>(c, c.i(0), static_cast<u16>(c.u(1))); }},
    {"glCullFace", [](GlCall& c) { gl_issue<&CullFace>(c, c.u(0)); }},
    {"glFrontFace", [](GlCall& c) { gl_issue<&FrontFace>(c, c.u(0)); }},
    {"glPolygonMode", [](GlCall& c) { gl_issue<&PolygonMode>(c, c.u(0), c.u(1)); }},
    {"glPolygonOffset", [](GlCall& c) { gl_issue<&PolygonOffset>(c, c.f(0), c.f(1)); }},
    {"glPolygonStipple", &polygon_stipple_cmd},
    {"glGetPolygonStipple", &get_polygon_stipple_cmd},

    // ---- per-fragment and framebuffer control ----
    {"glScissor", [](GlCall& c) { gl_issue<&Scissor>(c, c.i(0), c.i(1), c.i(2), c.i(3)); }},
    {"glAlphaFunc", [](GlCall& c) { gl_issue<&AlphaFunc>(c, c.u(0), c.f(1)); }},
    {"glStencilFunc", [](GlCall& c) { gl_issue<&StencilFunc>(c, c.u(0), c.i(1), c.u(2)); }},
    {"glStencilOp", [](GlCall& c) { gl_issue<&StencilOp>(c, c.u(0), c.u(1), c.u(2)); }},
    {"glDepthFunc", [](GlCall& c) { gl_issue<&DepthFunc>(c, c.u(0)); }},
    {"glBlendFunc", [](GlCall& c) { gl_issue<&BlendFunc>(c, c.u(0), c.u(1)); }},
    {"glLogicOp", [](GlCall& c) { gl_issue<&LogicOp>(c, c.u(0)); }},
    {"glDrawBuffer", [](GlCall& c) { gl_issue<&DrawBuffer>(c, c.u(0)); }},
    {"glReadBuffer", [](GlCall& c) { gl_issue<&ReadBuffer>(c, c.u(0)); }},
    {"glColorMask", [](GlCall& c) { gl_issue<&ColorMask>(c, c.b(0), c.b(1), c.b(2), c.b(3)); }},
    {"glDepthMask", [](GlCall& c) { gl_issue<&DepthMask>(c, c.b(0)); }},
    {"glStencilMask", [](GlCall& c) { gl_issue<&StencilMask>(c, c.u(0)); }},
    {"glIndexMask", [](GlCall& c) { gl_issue<&IndexMask>(c, c.u(0)); }},
    {"glClearColor", [](GlCall& c) { gl_issue<&ClearColor>(c, c.f(0), c.f(1), c.f(2), c.f(3)); }},
    {"glClearIndex", [](GlCall& c) { gl_issue<&ClearIndex>(c, c.f(0)); }},
    {"glClearDepth", [](GlCall& c) { gl_issue<&ClearDepth>(c, c.d(0)); }},
    {"glClearStencil", [](GlCall& c) { gl_issue<&ClearStencil>(c, c.i(0)); }},
    {"glClear", [](GlCall& c) { gl_issue<&Clear>(c, c.u(0)); }},
    {"glFlush", [](GlCall& c) { gl_issue<&Flush>(c); }},
    {"glFinish", [](GlCall& c) { gl_issue<&Finish>(c); }},
    {"glClearAccum", [](GlCall& c) { gl_issue<&ClearAccum>(c, c.f(0), c.f(1), c.f(2), c.f(3)); }},
    {"glAccum", [](GlCall& c) { gl_issue<&Accum>(c, c.u(0), c.f(1)); }},
    {"glHint", [](GlCall& c) { gl_issue<&Hint>(c, c.u(0), c.u(1)); }},

    // ---- pixel state ----
    {"glPixelStoref", [](GlCall& c) { gl_issue<&PixelStore>(c, c.u(0), c.f(1), false); }},
    {"glPixelStorei", [](GlCall& c) { gl_issue<&PixelStore>(c, c.u(0), c.i(1), true); }},
    {"glPixelTransferf", [](GlCall& c) { gl_issue<&PixelTransfer>(c, c.u(0), c.f(1), false); }},
    {"glPixelTransferi", [](GlCall& c) { gl_issue<&PixelTransfer>(c, c.u(0), c.i(1), true); }},
    {"glPixelZoom", [](GlCall& c) { gl_issue<&PixelZoom>(c, c.f(0), c.f(1)); }},
    {"glPixelMapfv", &pixel_map_cmd<float>},
    {"glPixelMapuiv", &pixel_map_cmd<u32>},
    {"glPixelMapusv", &pixel_map_cmd<u16>},
    {"glGetPixelMapfv", &get_pixel_map_cmd<float>},
    {"glGetPixelMapuiv", &get_pixel_map_cmd<u32>},
    {"glGetPixelMapusv", &get_pixel_map_cmd<u16>},

    // ---- texture environment, generation, object parameters ----
    {"glTexEnvf", &p2_scalar<float, &TexEnv>},
    {"glTexEnvi", &p2_scalar<i32, &TexEnv>},
    {"glTexEnvfv", &p2_vector<float, &tex_env_count, &TexEnv>},
    {"glTexEnviv", &p2_vector<i32, &tex_env_count, &TexEnv>},
    {"glGetTexEnvfv", &p2_get<float, &GetTexEnv>},
    {"glGetTexEnviv", &p2_get<i32, &GetTexEnv>},
    {"glTexGend", &p2_scalar<double, &TexGen>},
    {"glTexGenf", &p2_scalar<float, &TexGen>},
    {"glTexGeni", &p2_scalar<i32, &TexGen>},
    {"glTexGendv", &p2_vector<double, &tex_gen_count, &TexGen>},
    {"glTexGenfv", &p2_vector<float, &tex_gen_count, &TexGen>},
    {"glTexGeniv", &p2_vector<i32, &tex_gen_count, &TexGen>},
    {"glGetTexGendv", &p2_get<double, &GetTexGen>},
    {"glGetTexGenfv", &p2_get<float, &GetTexGen>},
    {"glGetTexGeniv", &p2_get<i32, &GetTexGen>},
    {"glTexParameterf", &p2_scalar<float, &TexParameter>},
    {"glTexParameteri", &p2_scalar<i32, &TexParameter>},
    {"glTexParameterfv", &p2_vector<float, &tex_param_count, &TexParameter>},
    {"glTexParameteriv", &p2_vector<i32, &tex_param_count, &TexParameter>},
    {"glGetTexParameterfv", &p2_get<float, &GetTexParameter>},
    {"glGetTexParameteriv", &p2_get<i32, &GetTexParameter>},
    {"glGetTexLevelParameterfv", &get_tex_level<float>},
    {"glGetTexLevelParameteriv", &get_tex_level<i32>},
    {"glTexImage1D", &tex_image_1d_cmd},
    {"glTexImage2D", &tex_image_2d_cmd},
    {"glTexSubImage1D", &tex_sub_image_1d_cmd},
    {"glTexSubImage2D", &tex_sub_image_2d_cmd},
    {"glCopyTexImage1D", [](GlCall& c) {
        gl_issue<&CopyTexImage>(c, 1, c.u(0), c.i(1), c.u(2), c.i(3), c.i(4), c.i(5), 1, c.i(6)); }},
    {"glCopyTexImage2D", [](GlCall& c) {
        gl_issue<&CopyTexImage>(c, 2, c.u(0), c.i(1), c.u(2), c.i(3), c.i(4), c.i(5), c.i(6), c.i(7)); }},
    {"glCopyTexSubImage1D", [](GlCall& c) {
        gl_issue<&CopyTexSubImage>(c, 1, c.u(0), c.i(1), c.i(2), 0, c.i(3), c.i(4), c.i(5), 1); }},
    {"glCopyTexSubImage2D", [](GlCall& c) {
        gl_issue<&CopyTexSubImage>(c, 2, c.u(0), c.i(1), c.i(2), c.i(3), c.i(4), c.i(5), c.i(6), c.i(7)); }},
    {"glGetTexImage", &get_tex_image_cmd},
    {"glGenTextures", &gen_textures_cmd},
    {"glDeleteTextures", &delete_textures_cmd},
    {"glBindTexture", [](GlCall& c) { gl_issue<&BindTexture>(c, c.u(0), c.u(1)); }},
    {"glIsTexture", [](GlCall& c) { c.ret(IsTexture(c.ctx, c.u(0)) ? 1 : 0); }},
    {"glAreTexturesResident", &are_textures_resident_cmd},
    {"glPrioritizeTextures", &prioritize_textures_cmd},

    // ---- attribute stacks ----
    {"glPushAttrib", [](GlCall& c) { gl_issue<&PushAttrib>(c, c.u(0)); }},
    {"glPopAttrib", [](GlCall& c) { gl_issue<&PopAttrib>(c); }},
    {"glPushClientAttrib", [](GlCall& c) { gl_issue<&PushClientAttrib>(c, c.u(0)); }},
    {"glPopClientAttrib", [](GlCall& c) { gl_issue<&PopClientAttrib>(c); }},

    // ---- evaluators ----
    {"glMap1f", &map1_cmd<float>},
    {"glMap1d", &map1_cmd<double>},
    {"glMap2f", &map2_cmd<float>},
    {"glMap2d", &map2_cmd<double>},
    {"glMapGrid1f", [](GlCall& c) { gl_issue<&MapGrid1>(c, c.i(0), c.f(1), c.f(2)); }},
    {"glMapGrid1d", [](GlCall& c) { gl_issue<&MapGrid1>(c, c.i(0), c.d(1), c.d(2)); }},
    {"glMapGrid2f", [](GlCall& c) { gl_issue<&MapGrid2>(c, c.i(0), c.f(1), c.f(2), c.i(3), c.f(4), c.f(5)); }},
    {"glMapGrid2d", [](GlCall& c) { gl_issue<&MapGrid2>(c, c.i(0), c.d(1), c.d(2), c.i(3), c.d(4), c.d(5)); }},
    {"glGetMapiv", &get_map<i32>},
    {"glGetMapfv", &get_map<float>},
    {"glGetMapdv", &get_map<double>},
    {"glEvalCoord1f", &eval_coord_cmd<float, 1, false>},
    {"glEvalCoord1fv", &eval_coord_cmd<float, 1, true>},
    {"glEvalCoord1d", &eval_coord_cmd<double, 1, false>},
    {"glEvalCoord1dv", &eval_coord_cmd<double, 1, true>},
    {"glEvalCoord2f", &eval_coord_cmd<float, 2, false>},
    {"glEvalCoord2fv", &eval_coord_cmd<float, 2, true>},
    {"glEvalCoord2d", &eval_coord_cmd<double, 2, false>},
    {"glEvalCoord2dv", &eval_coord_cmd<double, 2, true>},
    {"glEvalPoint1", [](GlCall& c) { gl_issue<&EvalPoint1>(c, c.i(0)); }},
    {"glEvalPoint2", [](GlCall& c) { gl_issue<&EvalPoint2>(c, c.i(0), c.i(1)); }},
    {"glEvalMesh1", [](GlCall& c) { gl_issue<&EvalMesh1>(c, c.u(0), c.i(1), c.i(2)); }},
    {"glEvalMesh2", [](GlCall& c) { gl_issue<&EvalMesh2>(c, c.u(0), c.i(1), c.i(2), c.i(3), c.i(4)); }},

    // ---- selection, feedback, list base ----
    {"glRenderMode", [](GlCall& c) { c.ret(static_cast<u32>(RenderMode(c.ctx, c.u(0)))); }},
    {"glSelectBuffer", [](GlCall& c) { gl_issue<&SelectBuffer>(c, c.i(0), c.p(1)); }},
    {"glFeedbackBuffer", [](GlCall& c) { gl_issue<&FeedbackBuffer>(c, c.i(0), c.u(1), c.p(2)); }},
    {"glInitNames", [](GlCall& c) { gl_issue<&InitNames>(c); }},
    {"glLoadName", [](GlCall& c) { gl_issue<&LoadName>(c, c.u(0)); }},
    {"glPushName", [](GlCall& c) { gl_issue<&PushName>(c, c.u(0)); }},
    {"glPopName", [](GlCall& c) { gl_issue<&PopName>(c); }},
    {"glPassThrough", [](GlCall& c) { gl_issue<&PassThrough>(c, c.f(0)); }},

    // ---- display lists (gl_display_list.cpp) ----
    {"glListBase", [](GlCall& c) { gl_issue<&ListBase>(c, c.u(0)); }},
    {"glNewList", [](GlCall& c) { gl_issue<&NewList>(c, c.u(0), c.u(1)); }},
    {"glEndList", [](GlCall& c) { gl_issue<&EndList>(c); }},
    {"glCallList", [](GlCall& c) { gl_issue<&CallList>(c, c.u(0)); }},
    {"glCallLists", &call_lists_cmd},
    {"glGenLists", [](GlCall& c) { c.ret(GenLists(c.ctx, c.i(0))); }},
    {"glDeleteLists", [](GlCall& c) { gl_issue<&DeleteLists>(c, c.u(0), c.i(1)); }},
    {"glIsList", [](GlCall& c) { c.ret(IsList(c.ctx, c.u(0)) ? 1 : 0); }},
};
// clang-format on

const std::unordered_map<std::string_view, GlHandler>& handler_map() {
    static const auto* map = [] {
        auto* m = new std::unordered_map<std::string_view, GlHandler>();
        for (const Entry& e : kHandlers) m->emplace(e.first, e.second);
        for (const Entry& e : gl_vertex_export_handlers()) m->emplace(e.first, e.second);
        for (const Entry& e : gl_pixel_export_handlers()) m->emplace(e.first, e.second);
        return m;
    }();
    return *map;
}

}  // namespace

GlHandler gl_export_handler(std::string_view name) {
    const auto& map = handler_map();
    const auto it = map.find(name);
    return it == map.end() ? nullptr : it->second;
}

std::vector<std::string_view> gl_export_handler_names() {
    std::vector<std::string_view> names;
    for (const Entry& e : kHandlers) names.push_back(e.first);
    for (const Entry& e : gl_vertex_export_handlers()) names.push_back(e.first);
    for (const Entry& e : gl_pixel_export_handlers()) names.push_back(e.first);
    return names;
}

}  // namespace vitreo::winapi::gl
