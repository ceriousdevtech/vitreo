// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_cmd_extensions.cpp
/// @brief The extensions GDI Generic advertises and wglGetProcAddress serves:
///        GL_EXT_paletted_texture (glColorTableEXT, glColorSubTableEXT,
///        glGetColorTableEXT, glGetColorTableParameter{iv,fv}EXT),
///        GL_WIN_swap_hint (glAddSwapHintRectWIN) and glDrawRangeElementsWIN;
///        GL_EXT_bgra lives in the pixel pipeline (gl_pixels.cpp).
///
/// Contracts: the EXT_paletted_texture and WIN_swap_hint registry specs, and
/// GDI Generic where they leave a value open or Windows differs
/// (tools/oracle_probe/gl11_state_probe64.expected ext-* tags,
/// swap_hint_probe64.expected hint-* tags).

#include "gl_commands.hpp"
#include "gl_drawable.hpp"
#include "gl_exports.hpp"
#include "gl_pixels.hpp"
#include "gl_texture.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vitreo::winapi::gl {

namespace {

/// The largest colour table: GDI Generic took 65536 entries (4096 and 65536
/// accepted; probed only up to 1024 because those runs destabilised it).
constexpr i32 kMaxColorTableWidth = 65536;

/// ColorTableEXT's internal formats on GDI Generic
/// (ext-pal-colortable-internal-*): RGB / RGBA bases (3, 4, RGB, RGBA and the
/// sized RGB / RGBA formats), not ALPHA, LUMINANCE, INTENSITY, 1 or BGRA.
std::optional<u32> table_base(i32 internal_format) noexcept {
    if (internal_format == 1 || internal_format == 2) return std::nullopt;
    const std::optional<GlTexFormat> f = gl_tex_format(internal_format, true);
    if (!f || (f->base != GL_RGB && f->base != GL_RGBA)) return std::nullopt;
    return f->base;
}

/// The source / destination formats of the table commands
/// (ext-pal-colortable-*-ubyte, ext-pal-getcolortable-*): RGB, RGBA, BGR,
/// BGRA and the single components; not LUMINANCE, LUMINANCE_ALPHA or
/// COLOR_INDEX.
bool table_format_valid(u32 format) noexcept {
    switch (format) {
    case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_RGB: case GL_RGBA: case GL_BGR_EXT:
    case GL_BGRA_EXT:
        return true;
    default: return false;
    }
}

/// The table a target names: the bound 1D / 2D object's, or a proxy's.
GlTextureObject* table_object(GlContext& ctx, u32 target, bool allow_proxy, const GlShareGroup::Lock& held) {
    switch (target) {
    case GL_TEXTURE_1D: return ctx.bound_texture(1, held);
    case GL_TEXTURE_2D: return ctx.bound_texture(2, held);
    case GL_PROXY_TEXTURE_1D: return allow_proxy ? &ctx.proxy_textures[0] : nullptr;
    case GL_PROXY_TEXTURE_2D: return allow_proxy ? &ctx.proxy_textures[1] : nullptr;
    default: return nullptr;
    }
}

bool is_proxy(u32 target) noexcept { return target == GL_PROXY_TEXTURE_1D || target == GL_PROXY_TEXTURE_2D; }

/// A table entry stores 8 bits per component, rounded in single precision
/// (ext-pal-colortable-RGBA-float: 0.9f -> 0xe6, 0.7f -> 0xb3).
std::array<u8, 4> quantize(const GlRgba& c) noexcept {
    std::array<u8, 4> out{};
    for (usize k = 0; k < 4; ++k) {
        const float v = std::clamp(c[k], 0.0f, 1.0f) * 255.0f + 0.5f;
        out[k] = static_cast<u8>(std::floor(v));
    }
    return out;
}

/// An entry component as GDI Generic returns it: v * (1 / 255) in single
/// precision (ext-pal-getcolortable-RGBA-float: 3 -> 0x3c40c0c2).
float entry_component(u8 v) noexcept { return static_cast<float>(v) * (1.0f / 255.0f); }

}  // namespace

namespace cmd {

void ColorTable(GlContext& ctx, u32 target, i32 internal_format, i32 width, u32 format, u32 type,
                const GlClientPixels& pixels) {
    if (ctx.reject_inside_begin_end()) return;   // ext-pal-colortable-inside-begin
    if (target != GL_TEXTURE_1D && target != GL_TEXTURE_2D && !is_proxy(target)) {
        ctx.record_error(GL_INVALID_ENUM);       // ext-pal-colortable-target-0x1234 / -TEXTURE_ENV
        return;
    }
    const std::optional<u32> base = table_base(internal_format);
    if (!base) { ctx.record_error(GL_INVALID_ENUM); return; }
    // The width is a power of two (ext-pal-colortable-width-*: 0, 3, 5, 255,
    // 257 and -1 are INVALID_VALUE).
    if (width <= 0 || width > kMaxColorTableWidth || (width & (width - 1)) != 0) {
        ctx.record_error(GL_INVALID_VALUE);
        return;
    }
    if (!table_format_valid(format) || !gl_pixel_type_valid(type, false)) {
        ctx.record_error(GL_INVALID_ENUM);
        return;
    }
    std::vector<GlRgba> groups;
    if (!is_proxy(target)) {
        if (pixels.null_pointer || !pixels.readable) {
            gl_log_unimplemented_once("glColorTableEXT", "unreadable client table (Windows faults)");
            return;
        }
        gl_unpack_rgba(ctx, gl_unpack_modes(ctx, pixels), format, type, width, 1, pixels.bytes, groups);
        if (groups.size() != static_cast<usize>(width)) return;
    }
    const GlShareGroup::Lock held = ctx.share_group->lock();
    GlTextureObject* t = table_object(ctx, target, true, held);
    if (t == nullptr) return;
    GlColorTable& table = t->color_table;
    table.internal_format = static_cast<u32>(internal_format);
    table.base = *base;
    table.width = width;
    if (is_proxy(target)) return;   // a proxy keeps the state only (ext-pal-proxy-rgb-512)
    table.entries.resize(static_cast<usize>(width));
    for (i32 k = 0; k < width; ++k) table.entries[static_cast<usize>(k)] = quantize(groups[static_cast<usize>(k)]);
    gl_lookup_all_indices(*t);
}

void ColorSubTable(GlContext& ctx, u32 target, i32 start, i32 count, u32 format, u32 type,
                   const GlClientPixels& pixels) {
    if (ctx.reject_inside_begin_end()) return;
    // Proxies have no entries to replace (ext-pal-colorsubtable-proxy).
    if (target != GL_TEXTURE_1D && target != GL_TEXTURE_2D) { ctx.record_error(GL_INVALID_ENUM); return; }
    if (!table_format_valid(format) || !gl_pixel_type_valid(type, false)) {
        ctx.record_error(GL_INVALID_ENUM);
        return;
    }
    i32 width = 0;
    {
        const GlShareGroup::Lock held = ctx.share_group->lock();
        width = table_object(ctx, target, false, held)->color_table.width;
    }
    // start + count past the table is INVALID_VALUE (ext-pal-colorsubtable-6-3,
    // -9-0). GDI Generic accepts a negative start or count and writes outside
    // its table; Vitreo refuses them with INVALID_VALUE.
    if (start < 0 || count < 0 || static_cast<i64>(start) + count > width) {
        ctx.record_error(GL_INVALID_VALUE);
        return;
    }
    if (count == 0) return;
    if (pixels.null_pointer || !pixels.readable) {
        gl_log_unimplemented_once("glColorSubTableEXT", "unreadable client table (Windows faults)");
        return;
    }
    std::vector<GlRgba> groups;
    gl_unpack_rgba(ctx, gl_unpack_modes(ctx, pixels), format, type, count, 1, pixels.bytes, groups);
    if (groups.size() != static_cast<usize>(count)) return;
    const GlShareGroup::Lock held = ctx.share_group->lock();
    GlTextureObject* t = table_object(ctx, target, false, held);
    GlColorTable& table = t->color_table;
    if (static_cast<i64>(start) + count > static_cast<i64>(table.entries.size())) return;   // redefined meanwhile
    for (i32 k = 0; k < count; ++k) {
        table.entries[static_cast<usize>(start + k)] = quantize(groups[static_cast<usize>(k)]);
    }
    gl_lookup_all_indices(*t);
}

bool GetColorTable(GlContext& ctx, u32 target, u32 format, u32 type, GlClientBuffer client, void* user) {
    if (ctx.reject_inside_begin_end()) return false;
    // A proxy has no contents (ext-pal-getcolortable-target-PROXY_*).
    if (target != GL_TEXTURE_1D && target != GL_TEXTURE_2D) { ctx.record_error(GL_INVALID_ENUM); return false; }
    if (!table_format_valid(format) || !gl_pixel_type_valid(type, false)) {
        ctx.record_error(GL_INVALID_ENUM);
        return false;
    }
    std::vector<GlRgba> groups;
    {
        const GlShareGroup::Lock held = ctx.share_group->lock();
        const GlColorTable& table = table_object(ctx, target, false, held)->color_table;
        groups.reserve(table.entries.size());
        // All four components as stored (ext-pal-getcolortable-RGBA-ubyte of
        // an RGB table keeps the source alpha).
        for (const auto& e : table.entries) {
            groups.push_back({entry_component(e[0]), entry_component(e[1]), entry_component(e[2]),
                              entry_component(e[3])});
        }
    }
    const i32 width = static_cast<i32>(groups.size());
    const GlPixelStoreModes& pack = ctx.state.pixel_store.pack;
    const GlPixelLayout layout = gl_pixel_layout(pack, format, type, width, 1);
    const std::span<u8> out = layout.valid ? client(user, layout.span_bytes) : std::span<u8>{};
    if (!layout.valid || out.size() < layout.span_bytes) {
        gl_log_unimplemented_once("glGetColorTableEXT", "unwritable client table (Windows faults)");
        return false;
    }
    gl_pack_rgba(pack, format, type, width, 1, groups, out);
    return true;
}

bool GetColorTableParameter(GlContext& ctx, u32 target, u32 pname, GlQuery& out) {
    if (ctx.reject_inside_begin_end()) return false;   // ext-pal-getparam-inside-begin
    const GlShareGroup::Lock held = ctx.share_group->lock();
    const GlTextureObject* t = table_object(ctx, target, true, held);
    if (t == nullptr) { ctx.record_error(GL_INVALID_ENUM); return false; }
    const GlColorTable& table = t->color_table;
    switch (pname) {
    case GL_COLOR_TABLE_FORMAT_EXT: out.set(GlQueryKind::Integer, {static_cast<double>(table.internal_format)}); break;
    case GL_COLOR_TABLE_WIDTH_EXT: out.set(GlQueryKind::Integer, {static_cast<double>(table.width)}); break;
    // Every component reports 8 bits, whatever the format (ext-pal-*-RED_SIZE ..
    // -ALPHA_SIZE); LUMINANCE / INTENSITY sizes are INVALID_ENUM.
    case GL_COLOR_TABLE_RED_SIZE_EXT: case GL_COLOR_TABLE_GREEN_SIZE_EXT: case GL_COLOR_TABLE_BLUE_SIZE_EXT:
    case GL_COLOR_TABLE_ALPHA_SIZE_EXT:
        out.set(GlQueryKind::Integer, {8.0});
        break;
    default: ctx.record_error(GL_INVALID_ENUM); return false;
    }
    return true;
}

void AddSwapHintRect(GlContext& ctx, i32 x, i32 y, i32 width, i32 height) {
    // Inside glBegin: INVALID_OPERATION and no rectangle (hint-inside-begin,
    // hint-refused-hint-white-over-black: the next swap is whole).
    if (ctx.reject_inside_begin_end()) return;
    // A zero or negative size adds nothing, without an error
    // (hint-zero-size-*, hint-negative-width-*).
    if (width <= 0 || height <= 0) return;
    if (ctx.drawable) ctx.drawable->add_swap_hint(x, y, width, height);
}

}  // namespace cmd

// ---- Exports (served by wglGetProcAddress) ------------------------------------------------

namespace {

using namespace cmd;

/// The client bytes a table command reads: @p count groups of @p format /
/// @p type under the UNPACK_* modes.
GlClientPixels table_pixels(GlCall& c, GuestAddr addr, u32 format, u32 type, i32 count, std::vector<u8>& storage) {
    if (count <= 0 || count > kMaxColorTableWidth || !table_format_valid(format) || !gl_pixel_type_valid(type, false)) {
        GlClientPixels px;
        px.unpack = &c.ctx.state.pixel_store.unpack;
        return px;
    }
    return gl_client_pixels(c, addr, gl_pixel_layout(c.ctx.state.pixel_store.unpack, format, type, count, 1),
                            storage);
}

void color_table_cmd(GlCall& c) {
    std::vector<u8> storage;
    const GlClientPixels px = table_pixels(c, c.p(5), c.u(3), c.u(4), c.i(2), storage);
    // A proxy table is executed at once, as glTexImage with a proxy target is (spec 5.4).
    if (is_proxy(c.u(0))) {
        ColorTable(c.ctx, c.u(0), c.i(1), c.i(2), c.u(3), c.u(4), px);
        return;
    }
    gl_issue<&ColorTable>(c, c.u(0), c.i(1), c.i(2), c.u(3), c.u(4), px);
}

void color_sub_table_cmd(GlCall& c) {
    std::vector<u8> storage;
    const GlClientPixels px = table_pixels(c, c.p(5), c.u(3), c.u(4), c.i(2), storage);
    gl_issue<&ColorSubTable>(c, c.u(0), c.i(1), c.i(2), c.u(3), c.u(4), px);
}

void get_color_table_cmd(GlCall& c) {
    GlClientOutput out{&c, c.p(3), {}};
    if (!GetColorTable(c.ctx, c.u(0), c.u(1), c.u(2), &GlClientOutput::open, &out)) return;
    out.write_back();
}

template <bool Float>
void get_color_table_parameter_cmd(GlCall& c) {
    GlQuery q;
    if (!GetColorTableParameter(c.ctx, c.u(0), c.u(1), q)) return;
    const GuestAddr out = c.p(2);
    if (out == 0) return;
    if constexpr (Float) {
        const float v = static_cast<float>(q.v[0]);
        (void)c.m.memory().write_bytes(out, reinterpret_cast<const u8*>(&v), 4);
    } else {
        const i32 v = gl_query_to_integer(q.kind, q.v[0]);
        (void)c.m.memory().write_bytes(out, reinterpret_cast<const u8*>(&v), 4);
    }
}

void add_swap_hint_rect_cmd(GlCall& c) {
    // Not compiled: GL_COMPILE drops it, GL_COMPILE_AND_EXECUTE executes it
    // (hint-swap-right-after-compiling-*, -after-compile-and-execute-*,
    // -after-list-call-*).
    if (c.ctx.compiling_list && c.ctx.list_mode == GL_COMPILE && !c.ctx.inside_begin_end()) return;
    AddSwapHintRect(c.ctx, c.i(0), c.i(1), c.i(2), c.i(3));
}

// clang-format off
const GlExtensionEntry kExtensions[] = {
    {"glAddSwapHintRectWIN", "iiii", &add_swap_hint_rect_cmd},
    {"glColorTableEXT", "iiiiip", &color_table_cmd},
    {"glColorSubTableEXT", "iiiiip", &color_sub_table_cmd},
    {"glGetColorTableEXT", "iiip", &get_color_table_cmd},
    {"glGetColorTableParameterivEXT", "iip", &get_color_table_parameter_cmd<false>},
    {"glGetColorTableParameterfvEXT", "iip", &get_color_table_parameter_cmd<true>},
    {"glDrawRangeElementsWIN", "iiiiip", nullptr},   // gl_cmd_vertex.cpp
};
// clang-format on

}  // namespace

std::span<const GlExtensionEntry> gl_extension_entries() {
    static const std::vector<GlExtensionEntry> entries = [] {
        std::vector<GlExtensionEntry> v(std::begin(kExtensions), std::end(kExtensions));
        for (GlExtensionEntry& e : v) {
            if (e.handler == nullptr) e.handler = gl_draw_range_elements_handler();
        }
        return v;
    }();
    return entries;
}

}  // namespace vitreo::winapi::gl
