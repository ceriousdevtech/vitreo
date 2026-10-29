// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_core.cpp
/// @brief GDI+ marshaling helpers and the export table (gdiplus.dll).

#include "winapi/graphics/gdiplus/gdiplus_core.hpp"
#include "winapi/graphics/gdiplus/gdiplus.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>

namespace vitreo::winapi::gdip {

u64 handle_arg(Marshaler& m, usize index) { return m.arg(index); }

float real_arg(Marshaler& m, usize index) { return m.arg_f32(index); }

bool write_handle(Marshaler& m, GuestAddr out, u64 handle) {
    return m.write_ptr(out, handle).is_ok();
}

bool write_status(Marshaler& m, Status s) {
    m.set_return(static_cast<u32>(s));
    return true;
}

bool read_f32(Marshaler& m, GuestAddr at, float& v) {
    const auto r = m.read_u32(at);
    if (r.is_err()) return false;
    const u32 bits = r.value();
    std::memcpy(&v, &bits, sizeof(v));
    return true;
}

bool write_f32(Marshaler& m, GuestAddr at, float v) {
    u32 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return m.write_u32(at, bits).is_ok();
}

bool read_points(Marshaler& m, GuestAddr at, i32 count, std::vector<PointF>& out) {
    if (count < 0) return false;
    out.resize(static_cast<usize>(count));
    for (i32 i = 0; i < count; ++i) {
        if (!read_f32(m, at + static_cast<u64>(i) * 8, out[static_cast<usize>(i)].X) ||
            !read_f32(m, at + static_cast<u64>(i) * 8 + 4, out[static_cast<usize>(i)].Y)) {
            return false;
        }
    }
    return true;
}

bool read_points_i(Marshaler& m, GuestAddr at, i32 count, std::vector<PointF>& out) {
    if (count < 0) return false;
    out.resize(static_cast<usize>(count));
    for (i32 i = 0; i < count; ++i) {
        const auto x = m.read_u32(at + static_cast<u64>(i) * 8);
        const auto y = m.read_u32(at + static_cast<u64>(i) * 8 + 4);
        if (x.is_err() || y.is_err()) return false;
        out[static_cast<usize>(i)] = {static_cast<float>(static_cast<i32>(x.value())),
                                      static_cast<float>(static_cast<i32>(y.value()))};
    }
    return true;
}

bool write_points(Marshaler& m, GuestAddr at, const std::vector<PointF>& pts) {
    for (usize i = 0; i < pts.size(); ++i) {
        if (!write_f32(m, at + i * 8, pts[i].X) || !write_f32(m, at + i * 8 + 4, pts[i].Y)) {
            return false;
        }
    }
    return true;
}

/// GDI+ converts REAL to INT as floor(v + 0.5): -0.5 becomes 0
/// (gdiplus_geometry_probe "pointsI", "xformI_half").
static i32 round_real(float v) { return static_cast<i32>(std::floor(v + 0.5f)); }

bool write_points_i(Marshaler& m, GuestAddr at, const std::vector<PointF>& pts) {
    for (usize i = 0; i < pts.size(); ++i) {
        if (m.write_u32(at + i * 8, static_cast<u32>(round_real(pts[i].X))).is_err() ||
            m.write_u32(at + i * 8 + 4, static_cast<u32>(round_real(pts[i].Y))).is_err()) {
            return false;
        }
    }
    return true;
}

bool read_rect(Marshaler& m, GuestAddr at, RectF& r) {
    return read_f32(m, at, r.X) && read_f32(m, at + 4, r.Y) && read_f32(m, at + 8, r.Width) &&
           read_f32(m, at + 12, r.Height);
}

bool read_rect_i(Marshaler& m, GuestAddr at, RectF& r) {
    std::vector<PointF> two;
    if (!read_points_i(m, at, 2, two)) return false;
    r = {two[0].X, two[0].Y, two[1].X, two[1].Y};
    return true;
}

bool write_rect(Marshaler& m, GuestAddr at, const RectF& r) {
    return write_f32(m, at, r.X) && write_f32(m, at + 4, r.Y) && write_f32(m, at + 8, r.Width) &&
           write_f32(m, at + 12, r.Height);
}

bool write_rect_i(Marshaler& m, GuestAddr at, const RectF& r) {
    return write_points_i(m, at, {{r.X, r.Y}, {r.Width, r.Height}});
}

bool read_rects(Marshaler& m, GuestAddr at, i32 count, std::vector<RectF>& out) {
    if (count < 0) return false;
    out.resize(static_cast<usize>(count));
    for (i32 i = 0; i < count; ++i) {
        if (!read_rect(m, at + static_cast<u64>(i) * 16, out[static_cast<usize>(i)])) return false;
    }
    return true;
}

bool read_rects_i(Marshaler& m, GuestAddr at, i32 count, std::vector<RectF>& out) {
    if (count < 0) return false;
    out.resize(static_cast<usize>(count));
    for (i32 i = 0; i < count; ++i) {
        if (!read_rect_i(m, at + static_cast<u64>(i) * 16, out[static_cast<usize>(i)])) return false;
    }
    return true;
}

bool read_affine(Marshaler& m, GuestAddr at, Affine& a) {
    return read_f32(m, at, a.m11) && read_f32(m, at + 4, a.m12) && read_f32(m, at + 8, a.m21) &&
           read_f32(m, at + 12, a.m22) && read_f32(m, at + 16, a.dx) && read_f32(m, at + 20, a.dy);
}

std::u16string read_wstr(Marshaler& m, GuestAddr at, i32 length) {
    std::u16string out;
    if (at == 0) return out;
    constexpr i32 kMax = 1 << 20;
    const i32 limit = length < 0 ? kMax : length;
    for (i32 i = 0; i < limit; ++i) {
        const auto unit = m.read_u16(at + static_cast<u64>(i) * 2);
        if (unit.is_err()) break;
        if (length < 0 && unit.value() == 0) break;
        out.push_back(static_cast<char16_t>(unit.value()));
    }
    return out;
}

bool write_wstr(Marshaler& m, GuestAddr at, std::u16string_view s, bool terminate) {
    for (usize i = 0; i < s.size(); ++i) {
        if (m.write_u16(at + i * 2, static_cast<u16>(s[i])).is_err()) return false;
    }
    return !terminate || m.write_u16(at + s.size() * 2, 0).is_ok();
}

void log_unimplemented(std::string_view api, std::string_view what) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::string key = std::string(api) + ":" + std::string(what);
    std::lock_guard<std::mutex> lock(mutex);
    if (!seen.insert(key).second) return;
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED gdiplus!%.*s: %.*s\n",
                 static_cast<int>(api.size()), api.data(), static_cast<int>(what.size()),
                 what.data());
}

} // namespace vitreo::winapi::gdip

namespace vitreo::winapi {

namespace {

struct ExportRow {
    const char* name;
    u16 ordinal_x86;
    u16 ordinal_x64;
    u16 stdcall_bytes;
};

constexpr ExportRow kExports[] = {
#define GDIP_EXPORT(name, ord86, ord64, bytes) {#name, ord86, ord64, bytes},
#include "winapi/graphics/gdiplus/gdiplus_export_table.inc"
#undef GDIP_EXPORT
};

} // namespace

std::vector<std::string> gdiplus_export_names() {
    std::vector<std::string> names;
    for (const ExportRow& row : kExports) names.emplace_back(row.name);
    return names;
}

gdip::HandlerMap gdiplus_handlers(GdiplusManager& mgr) {
    gdip::HandlerMap map;
    // Each export belongs to exactly one area (gdiplus_<area>.cpp).
    gdip::register_geometry(mgr, map);
    gdip::register_region(mgr, map);
    gdip::register_graphics(mgr, map);
    gdip::register_pen_brush(mgr, map);
    gdip::register_draw(mgr, map);
    gdip::register_text(mgr, map);
    gdip::register_image(mgr, map);
    gdip::register_codec(mgr, map);
    return map;
}

DllExports create_gdiplus_thunks(GdiplusManager& mgr, bool guest_is_64bit) {
    DllExports exports;
    gdip::HandlerMap handlers = gdiplus_handlers(mgr);
    for (const ExportRow& row : kExports) {
        gdip::Handler fn;
        if (auto it = handlers.find(row.name); it != handlers.end()) {
            fn = [&mgr, inner = std::move(it->second)](Marshaler& m) {
                std::lock_guard<std::recursive_mutex> lock(mgr.mutex());
                inner(m);
            };
            handlers.erase(it);
        } else {
            const std::string name = row.name;
            fn = [name](Marshaler& m) {
                gdip::log_unimplemented(name, "export has no implementation");
                m.set_return(static_cast<u32>(gdip::Status::NotImplemented));
            };
        }
        exports.entries.push_back(ExportEntry{
            row.name, guest_is_64bit ? row.ordinal_x64 : row.ordinal_x86, std::move(fn),
            row.stdcall_bytes});
    }
    // What is left is not a gdiplus.dll export: only the internal hooks
    // GdiplusStartupOutput hands out may remain.
    for (auto& [name, fn] : handlers) {
        if (name.rfind("Vitreo_", 0) != 0) {
            std::fprintf(stderr, "[gdiplus] registrar supplied non-export '%s'\n", name.c_str());
            continue;
        }
        exports.entries.push_back(ExportEntry{
            name, 0,
            [&mgr, inner = std::move(fn)](Marshaler& m) {
                std::lock_guard<std::recursive_mutex> lock(mgr.mutex());
                inner(m);
            },
            4});
    }
    return exports;
}

} // namespace vitreo::winapi
