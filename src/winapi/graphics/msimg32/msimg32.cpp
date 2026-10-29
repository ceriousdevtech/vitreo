#include "msimg32.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include <cstdio>

namespace vitreo::winapi {

void Msimg32Manager::impl_TransparentBlt(Marshaler& m) {
    // BOOL TransparentBlt(hdcDest, xDest, yDest, wDest, hDest,
    //                     hdcSrc, xSrc, ySrc, wSrc, hSrc, crTransparent)
    if (m_gdi == nullptr) { m.set_return(0); return; }
    const bool ok = m_gdi->transparent_blt(
        static_cast<Handle>(m.arg(0)), static_cast<i32>(m.arg32(1)), static_cast<i32>(m.arg32(2)),
        static_cast<i32>(m.arg32(3)), static_cast<i32>(m.arg32(4)), static_cast<Handle>(m.arg(5)),
        static_cast<i32>(m.arg32(6)), static_cast<i32>(m.arg32(7)), static_cast<i32>(m.arg32(8)),
        static_cast<i32>(m.arg32(9)), m.arg32(10));
    m.set_return(ok ? 1u : 0u);
}

void Msimg32Manager::impl_AlphaBlend(Marshaler& m) {
    // BOOL AlphaBlend(HDC hdcDest, int xDest, int yDest, int wDest, int hDest,
    //                 HDC hdcSrc, int xSrc, int ySrc, int wSrc, int hSrc,
    //                 BLENDFUNCTION ftn)
    // BLENDFUNCTION is 4 bytes by value: BlendOp, BlendFlags,
    // SourceConstantAlpha, AlphaFormat. Every rejection is FALSE with
    // ERROR_INVALID_PARAMETER (tools/oracle_probe/s3gdi_blit_probe "AB").
    if (m_gdi == nullptr) {
        set_last_error(87u);
        m.set_return(0);
        return;
    }
    const u32 blend = m.arg32(10);
    const u32 error = m_gdi->alpha_blend_checked(
        static_cast<Handle>(m.arg(0)), static_cast<i32>(m.arg32(1)), static_cast<i32>(m.arg32(2)),
        static_cast<i32>(m.arg32(3)), static_cast<i32>(m.arg32(4)), static_cast<Handle>(m.arg(5)),
        static_cast<i32>(m.arg32(6)), static_cast<i32>(m.arg32(7)), static_cast<i32>(m.arg32(8)),
        static_cast<i32>(m.arg32(9)), static_cast<u8>(blend & 0xFF),
        static_cast<u8>((blend >> 16) & 0xFF), static_cast<u8>((blend >> 24) & 0xFF));
    if (error != 0 && error != 0xFFFFFFFFu) set_last_error(error);
    m.set_return(error == 0 ? 1u : 0u);
}

void Msimg32Manager::impl_GradientFill(Marshaler& m) {
    // BOOL GradientFill(hdc, pVertex, nVertex, pMesh, nMesh, ulMode)
    if (m_gdi == nullptr) { set_last_error(87u); m.set_return(0); return; }
    const u32 result = m_gdi->gradient_fill(static_cast<Handle>(m.arg(0)), m.arg_ptr(1),
                                            m.arg32(2), m.arg_ptr(3), m.arg32(4), m.arg32(5));
    if (result != 0 && result != 0xFFFFFFFFu) set_last_error(result);
    m.set_return(result == 0 ? 1u : 0u);
}

DllExports create_msimg32_thunks(Msimg32Manager& mgr) {
    DllExports exports;
    exports.entries.reserve(2);

    exports.entries.push_back(ExportEntry{
        "TransparentBlt", 0,
        [&mgr](Marshaler& m) { mgr.impl_TransparentBlt(m); },
        44
    });

    exports.entries.push_back(ExportEntry{
        "AlphaBlend", 0,
        [&mgr](Marshaler& m) { mgr.impl_AlphaBlend(m); },
        44
    });

    // GradientFill(hdc, pVertex, nVertex, pMesh, nMesh, ulMode) -> BOOL
    exports.entries.push_back(ExportEntry{
        "GradientFill", 0,
        [&mgr](Marshaler& m) { mgr.impl_GradientFill(m); },
        24
    });

    return exports;
}

} // namespace vitreo::winapi
