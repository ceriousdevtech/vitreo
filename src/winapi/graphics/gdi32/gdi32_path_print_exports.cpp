// SPDX-License-Identifier: LGPL-2.1-only

/// @file gdi32_path_print_exports.cpp
/// @brief Path, enhanced-metafile and printing exports.
///
/// The "Audit completions" block, BeginPath .. SetAbortProc: paths,
/// ExtCreatePen, DC pen/brush colours, enhanced-metafile entry points
/// and the print-spooler calls. Reads GdiManager::m_region_objects
/// directly.
///
/// One of the section registrars split out of make_gdi32_exports
/// (gdi32_registration.cpp); see gdi32_registration_internal.hpp. The entries
/// are pushed in their original order.

#include "winapi/graphics/gdi32/gdi32_registration_internal.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace vitreo::winapi {
namespace {

void gdi_log_unimplemented_once(bool& logged, const char* api, const char* why) {
    if (logged) return;
    logged = true;
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED gdi32!%s: %s\n", api, why);
}

}  // namespace

void add_gdi32_path_print_exports(DllExports& exports, GdiManager& mgr) {
    // ======================================================================
    // Audit completions: paths, enhanced metafiles, printing, and the few
    // DC/region queries printing and UI code reaches for.
    //
    // Vitreo's GDI is a live-surface renderer with no path accumulator, no
    // metafile recorder and no print spooler. Each entry below is either the
    // real query or the documented failure — never a success-shaped value for
    // drawing that did not happen.
    // ======================================================================
    constexpr u32 SP_ERROR = 0xFFFFFFFFu;   // StartDoc/StartPage failure
    constexpr u32 RGN_ERROR = 0u;           // GetClipRgn / EqualRgn failure

    // -- Paths (geometry_ops.cpp; contracts from s3gdi_region_probe).
    const std::pair<const char*, gdi_geometry_exports::PathOp> path_ops[] = {
        {"BeginPath", gdi_geometry_exports::PathOp::Begin},
        {"EndPath", gdi_geometry_exports::PathOp::End},
        {"AbortPath", gdi_geometry_exports::PathOp::Abort},
        {"CloseFigure", gdi_geometry_exports::PathOp::CloseFigure},
        {"FillPath", gdi_geometry_exports::PathOp::Fill},
        {"StrokePath", gdi_geometry_exports::PathOp::Stroke},
        {"StrokeAndFillPath", gdi_geometry_exports::PathOp::StrokeAndFill},
        {"FlattenPath", gdi_geometry_exports::PathOp::Flatten},
        {"WidenPath", gdi_geometry_exports::PathOp::Widen},
    };
    for (const auto& [name, op] : path_ops) {
        exports.entries.push_back({name, 0, [&mgr, op = op](Marshaler& m) {
            gdi_geometry_exports::path_op(mgr, m, op);
        }, 4});
    }
    exports.entries.push_back({"SelectClipPath", 0, [&mgr](Marshaler& m) {
        gdi_geometry_exports::select_clip_path(mgr, m);
    }, 8});
    exports.entries.push_back({"PathToRegion", 0, [&mgr](Marshaler& m) {
        gdi_geometry_exports::path_to_region(mgr, m);
    }, 4});
    exports.entries.push_back({"GetPath", 0, [&mgr](Marshaler& m) {
        gdi_geometry_exports::get_path(mgr, m);
    }, 16});
    exports.entries.push_back({"PolylineTo", 0, [&mgr](Marshaler& m) {
        gdi_geometry_exports::poly_points(mgr, m, gdi_geometry_exports::PolyKind::PolylineTo);
    }, 12});
    exports.entries.push_back({"PolyBezier", 0, [&mgr](Marshaler& m) {
        gdi_geometry_exports::poly_points(mgr, m, gdi_geometry_exports::PolyKind::PolyBezier);
    }, 12});
    exports.entries.push_back({"PolyPolygon", 0, [&mgr](Marshaler& m) {
        gdi_geometry_exports::poly_polygon(mgr, m);
    }, 16});
    exports.entries.push_back({"CreatePolyPolygonRgn", 0, [&mgr](Marshaler& m) {
        gdi_geometry_exports::create_polygon_rgn(mgr, m, true);
    }, 16});
    exports.entries.push_back({"GetMapMode", 0, [&mgr](Marshaler& m) {
        gdi_geometry_exports::get_map_mode(mgr, m);
    }, 4});
    exports.entries.push_back({"GetMiterLimit", 0, [&mgr](Marshaler& m) {
        gdi_geometry_exports::get_miter_limit(mgr, m);
    }, 8});
    exports.entries.push_back({"GetArcDirection", 0, [&mgr](Marshaler& m) {
        gdi_geometry_exports::get_arc_direction(mgr, m);
    }, 4});
    exports.entries.push_back({"OffsetClipRgn", 0, [&mgr](Marshaler& m) {
        gdi_geometry_exports::offset_clip_rgn(mgr, m);
    }, 12});
    exports.entries.push_back({"PolyBezierTo", 0, [&mgr](Marshaler& m) { gdi_geometry_exports::poly_points(mgr, m, gdi_geometry_exports::PolyKind::PolyBezierTo); }, 12});
    exports.entries.push_back({"SetMiterLimit", 0, [&mgr](Marshaler& m) { gdi_geometry_exports::set_miter_limit(mgr, m); }, 12});
    exports.entries.push_back({"SetArcDirection", 0, [&mgr](Marshaler& m) { gdi_geometry_exports::set_arc_direction(mgr, m); }, 8});
    exports.entries.push_back({"ExtCreatePen", 0, [&mgr](Marshaler& m) {
        // ExtCreatePen(iPenStyle, cWidth, const LOGBRUSH*, cStyle, pstyle)
        GdiManager::ExtPenInfo info;
        info.style = m.arg32(0);
        info.width = m.arg32(1);
        const GuestAddr lb = m.arg_ptr(2);
        const u32 count = m.arg32(3);
        const GuestAddr entries = m.arg_ptr(4);
        if (lb == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
        info.brush_style = m.read_u32(lb).value_or(0xFFFFFFFFu);
        info.color = m.read_u32(lb + 4).value_or(0);
        info.hatch = m.cpu_is_64bit() ? m.memory().read_u64(lb + 8).value_or(0)
                                      : m.read_u32(lb + 8).value_or(0);
        if (count > 16 || (count != 0 && entries == 0) || (count == 0 && entries != 0)) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        for (u32 i = 0; i < count; ++i) info.style_entries.push_back(m.read_u32(entries + i * 4).value_or(0));
        u32 error = 0;
        const Handle h = mgr.create_ext_pen(std::move(info), &error);
        if (h == 0 && error != 0) set_last_error(error);
        m.set_return(static_cast<u64>(h));
    }, 20});
    exports.entries.push_back({"SetDCPenColor", 0, [&mgr](Marshaler& m) {
        m.set_return(static_cast<u64>(mgr.set_dc_pen_color(static_cast<Handle>(m.arg(0)),
                                                           m.arg32(1))));
    }, 8});
    exports.entries.push_back({"GetDCBrushColor", 0, [&mgr](Marshaler& m) {
        const u32 prev = mgr.set_dc_brush_color(static_cast<Handle>(m.arg(0)), 0);
        if (prev != 0xFFFFFFFFu) (void)mgr.set_dc_brush_color(static_cast<Handle>(m.arg(0)), prev);
        m.set_return(static_cast<u64>(prev));
    }, 4});
    exports.entries.push_back({"GetDCPenColor", 0, [&mgr](Marshaler& m) {
        const u32 prev = mgr.set_dc_pen_color(static_cast<Handle>(m.arg(0)), 0);
        if (prev != 0xFFFFFFFFu) (void)mgr.set_dc_pen_color(static_cast<Handle>(m.arg(0)), prev);
        m.set_return(static_cast<u64>(prev));
    }, 4});
    exports.entries.push_back({"CreateDIBPatternBrushPt", 0, [&mgr](Marshaler& m) {
        m.set_return(static_cast<u64>(mgr.create_dib_pattern_brush(m.arg_ptr(0), m.arg32(1))));
    }, 8});
    exports.entries.push_back({"CancelDC", 0, [&mgr](Marshaler& m) {
        // Nothing long-running to cancel on a live DC: TRUE; a bad DC is
        // FALSE with the error untouched ("CancelDC(NULL)=0").
        m.set_return(mgr.has_dc(static_cast<Handle>(m.arg(0))) ? 1u : 0u);
    }, 4});

    // -- Regions.
    exports.entries.push_back({"EqualRgn", 0, [&mgr](Marshaler& m) {
        const auto a = static_cast<Handle>(m.arg(0));
        const auto b = static_cast<Handle>(m.arg(1));
        auto lhs = mgr.m_region_objects.find(a);
        auto rhs = mgr.m_region_objects.find(b);
        if (lhs == mgr.m_region_objects.end() ||
            rhs == mgr.m_region_objects.end()) {
            m.set_return(RGN_ERROR);
            return;
        }
        // Vitreo models regions as their bounding rectangle, so identical
        // bounds is identical region.
        const bool equal = ClipRect::combine(lhs->second, rhs->second, 3).is_empty();
        m.set_return(equal ? 1u : 0u);
    }, 8});
    exports.entries.push_back({"GetClipRgn", 0, [&mgr](Marshaler& m) { gdi_geometry_exports::get_clip_rgn(mgr, m); }, 8});

    // -- Enhanced metafiles. Recording needs a command journal; every entry
    // point reports the documented failure so a caller falls back to drawing
    // directly.
    exports.entries.push_back({"CreateEnhMetaFileW", 0, [](Marshaler& m) {
        // Recording needs a GDI command journal Vitreo does not have: NULL
        // with ERROR_CALL_NOT_IMPLEMENTED (Windows records here).
        static bool logged = false;
        gdi_log_unimplemented_once(logged, "CreateEnhMetaFileW", "no metafile recorder");
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        m.set_return(0u);
    }, 16});
    exports.entries.push_back({"CloseEnhMetaFile", 0, [](Marshaler& m) {
        // No metafile DC can exist (CreateEnhMetaFileW fails), so there is
        // nothing to close.
        (void)m.arg(0);
        m.set_return(0u);
    }, 4});
    exports.entries.push_back({"DeleteEnhMetaFile", 0, [&mgr](Marshaler& m) {
        m.set_return(mgr.delete_enh_metafile(static_cast<Handle>(m.arg(0))) ? 1u : 0u);
    }, 4});
    exports.entries.push_back({"GetEnhMetaFileBits", 0, [&mgr](Marshaler& m) {
        // (hemf, nSize, lpbBuffer): the size with a NULL buffer, else the
        // bytes copied; 0 for an unknown handle or a short buffer.
        const auto* bytes = mgr.enh_metafile(static_cast<Handle>(m.arg(0)));
        const u32 size = m.arg32(1);
        const GuestAddr out = m.arg_ptr(2);
        if (bytes == nullptr) { m.set_return(0u); return; }
        if (out == 0) { m.set_return(static_cast<u32>(bytes->size())); return; }
        if (size < bytes->size() || m.memory().write_bytes(out, bytes->data(), bytes->size()).is_err()) {
            m.set_return(0u);
            return;
        }
        m.set_return(static_cast<u32>(bytes->size()));
    }, 12});
    exports.entries.push_back({"SetEnhMetaFileBits", 0, [&mgr](Marshaler& m) {
        const u32 size = m.arg32(0);
        const GuestAddr data = m.arg_ptr(1);
        if (data == 0 || size == 0 || size > (64u << 20)) { m.set_return(0u); return; }
        std::vector<u8> bytes(size);
        if (m.memory().read_bytes(data, bytes.data(), size).is_err()) { m.set_return(0u); return; }
        m.set_return(static_cast<u64>(mgr.set_enh_metafile_bits(bytes)));
    }, 8});
    exports.entries.push_back({"GetEnhMetaFileHeader", 0, [&mgr](Marshaler& m) {
        // (hemf, nSize, lpemh): the header record's size ("header=108"), or
        // the bytes of it copied; 0 with the error untouched for NULL.
        const auto* bytes = mgr.enh_metafile(static_cast<Handle>(m.arg(0)));
        const u32 size = m.arg32(1);
        const GuestAddr out = m.arg_ptr(2);
        if (bytes == nullptr) { m.set_return(0u); return; }
        u32 header = 0;
        std::memcpy(&header, bytes->data() + 4, 4);
        if (out == 0) { m.set_return(header); return; }
        const u32 n = std::min(size, header);
        (void)m.memory().write_bytes(out, bytes->data(), n);
        m.set_return(n);
    }, 12});
    exports.entries.push_back({"PlayEnhMetaFile", 0, [&mgr](Marshaler& m) {
        // Playback needs an interpreter for every EMR_* record: FALSE with
        // ERROR_CALL_NOT_IMPLEMENTED for a real metafile.
        if (mgr.enh_metafile(static_cast<Handle>(m.arg(1))) == nullptr) { m.set_return(0u); return; }
        static bool logged = false;
        gdi_log_unimplemented_once(logged, "PlayEnhMetaFile", "no EMF record interpreter");
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        m.set_return(0u);
    }, 12});
    exports.entries.push_back({"PlayEnhMetaFileRecord", 0, [](Marshaler& m) {
        static bool logged = false;
        gdi_log_unimplemented_once(logged, "PlayEnhMetaFileRecord", "no EMF record interpreter");
        (void)m.arg(0);
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        m.set_return(0u);
    }, 16});
    exports.entries.push_back({"EnumEnhMetaFile", 0, [&mgr](Marshaler& m) {
        // (hdc, hemf, proc, data, rect): metafile_ops.cpp.
        m.set_return(static_cast<u32>(mgr.enum_enh_metafile(static_cast<Handle>(m.arg(0)),
                                                            static_cast<Handle>(m.arg(1)), m.arg_ptr(2),
                                                            m.arg(3), m.cpu_is_64bit())));
    }, 20});
    exports.entries.push_back({"GdiComment", 0, [&mgr](Marshaler& m) {
        // Outside a metafile DC the comment is accepted and ignored
        // ("GdiComment(mem)=1").
        m.set_return(mgr.has_dc(static_cast<Handle>(m.arg(0))) ? 1u : 0u);
    }, 12});

    // -- Printing. There is no spooler behind a Vitreo DC.
    exports.entries.push_back({"StartDocW", 0, [](Marshaler& m) {
        (void)m.arg(0); (void)m.arg_ptr(1);
        VITREO_TRACE("[gdi32] StartDocW -> SP_ERROR (no print spooler)\n");
        m.set_return(SP_ERROR);
    }, 8});
    exports.entries.push_back({"EndDoc", 0, [](Marshaler& m) {
        (void)m.arg(0); m.set_return(SP_ERROR);
    }, 4});
    exports.entries.push_back({"StartPage", 0, [](Marshaler& m) {
        (void)m.arg(0); m.set_return(SP_ERROR);
    }, 4});
    exports.entries.push_back({"EndPage", 0, [](Marshaler& m) {
        (void)m.arg(0); m.set_return(SP_ERROR);
    }, 4});
    // int AbortDoc(HDC hdc). Learn: stops the current print job; SP_ERROR on
    // failure. StartDoc never starts a job here (no print spooler), so no DC
    // ever has one to abort: every call fails the way Windows fails for a DC
    // without a job -- SP_ERROR with ERROR_INVALID_HANDLE, for screen, memory,
    // NULL and bogus DCs alike (shlwapi_regsec_shell_probe abortdoc_*).
    exports.entries.push_back({"AbortDoc", 0, [](Marshaler& m) {
        (void)m.arg(0);
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(SP_ERROR);
    }, 4});
    exports.entries.push_back({"SetAbortProc", 0, [](Marshaler& m) {
        (void)m.arg(0); (void)m.arg_ptr(1);
        m.set_return(SP_ERROR);
    }, 8});
}

}  // namespace vitreo::winapi
