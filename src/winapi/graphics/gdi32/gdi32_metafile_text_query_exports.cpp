// SPDX-License-Identifier: LGPL-2.1-only

/// @file gdi32_metafile_text_query_exports.cpp
/// @brief Metafile, text-extent and pixel-format-query exports.
///
/// CopyMetaFileW .. GetStretchBltMode: metafile calls, GetTextExtent*
/// variants, CreateDCW, DescribePixelFormat, escapes and bitmap bits.
///
/// One of the section registrars split out of make_gdi32_exports
/// (gdi32_registration.cpp); see gdi32_registration_internal.hpp. The entries
/// are pushed in their original order.

#include "winapi/graphics/gdi32/gdi32_registration_internal.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/graphics/gdi32/pixel_format_api.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <vector>

namespace vitreo::winapi {

void add_gdi32_metafile_text_query_exports(DllExports& exports, GdiManager& mgr) {
    // ---- CopyMetaFileW(hmfSrc, lpFileName) ----
    exports.entries.push_back({"CopyMetaFileW", 0, [](Marshaler& m) {
        (void)m.arg(0); (void)m.arg(1);
        m.set_return(0); // not supported
    }, 8});

    // ---- SetMetaFileBitsEx(cbBuffer, lpData) ----
    exports.entries.push_back({"SetMetaFileBitsEx", 0, [&mgr](Marshaler& m) {
        const auto size = static_cast<usize>(m.arg(0));
        const GuestAddr data = m.arg_ptr(1);
        if (size == 0 || data == 0) {
            m.set_return(0);
            return;
        }
        std::vector<u8> bytes(size);
        if (!m.memory().read_bytes(
                data, reinterpret_cast<Byte*>(bytes.data()), size).is_ok()) {
            m.set_return(0);
            return;
        }
        const Handle hmf = mgr.set_metafile_bits(bytes);
        VITREO_TRACE(
            "[gdi] SetMetaFileBitsEx(size=%zu, data=0x%llx) -> 0x%x\n",
            size, static_cast<unsigned long long>(data), hmf);
        m.set_return(hmf);
    }, 8});

    // ---- DeleteMetaFile(hmf) ----
    exports.entries.push_back({"DeleteMetaFile", 0, [&mgr](Marshaler& m) {
        const auto hmf = static_cast<Handle>(m.arg(0));
        const bool deleted = mgr.delete_metafile(hmf);
        VITREO_TRACE(
            "[gdi] DeleteMetaFile(hmf=0x%x) -> %s\n",
            hmf, deleted ? "TRUE" : "FALSE");
        m.set_return(deleted ? 1 : 0);
    }, 4});

    // ---- PlayMetaFile(hdc, hmf) ----
    exports.entries.push_back({"PlayMetaFile", 0, [&mgr](Marshaler& m) {
        const auto hdc = static_cast<Handle>(m.arg(0));
        const auto hmf = static_cast<Handle>(m.arg(1));
        VITREO_TRACE(
            "[gdi] PlayMetaFile(hdc=0x%x, hmf=0x%x) -> FALSE "
            "(%s)\n",
            hdc, hmf,
            mgr.has_metafile(hmf)
                ? "record playback unsupported"
                : "invalid metafile handle");
        m.set_return(0);
    }, 8});

    // ---- GetTextExtentPoint32A(hdc, lpString, c, lpSize) ----
    // Measures with the DC's selected font. Callers size a dialog font by
    // measuring candidates.
    exports.entries.push_back({"GetTextExtentPoint32A", 0, [&mgr](Marshaler& m) { gdi_text_exports::get_text_extent_point(mgr, m, false); }, 16});

    // ---- GetTextExtentExPointA/W(hdc, str, c, maxExtent, lpnFit, lpnDx, lpSize)
    exports.entries.push_back({"GetTextExtentExPointA", 0, [&mgr](Marshaler& m) {
        gdi_text_exports::get_text_extent_ex_point(mgr, m, false);
    }, 28});
    exports.entries.push_back({"GetTextExtentExPointW", 0, [&mgr](Marshaler& m) {
        gdi_text_exports::get_text_extent_ex_point(mgr, m, true);
    }, 28});
    exports.entries.push_back({"GetTextExtentPointW", 0, [&mgr](Marshaler& m) {
        gdi_text_exports::get_text_extent_point(mgr, m, true);
    }, 16});
    exports.entries.push_back({"SetTextCharacterExtra", 0, [&mgr](Marshaler& m) {
        gdi_text_exports::set_text_character_extra(mgr, m);
    }, 8});
    exports.entries.push_back({"GetTextCharacterExtra", 0, [&mgr](Marshaler& m) {
        gdi_text_exports::get_text_character_extra(mgr, m);
    }, 4});
    exports.entries.push_back({"GetTextAlign", 0, [&mgr](Marshaler& m) {
        gdi_text_exports::get_text_align(mgr, m);
    }, 4});

    // BOOL GetTextExtentPointI(HDC, LPWORD pgiIn, int cgi, LPSIZE psize)
    exports.entries.push_back({"GetTextExtentPointI", 0, [&mgr](Marshaler& m) { gdi_text_exports::get_text_extent_point_i(mgr, m); }, 16});

    // ---- CreateDCW(lpszDriver, lpszDevice, lpszOutput, lpInitData) ----
    exports.entries.push_back({"CreateDCW", 0, [&mgr](Marshaler& m) {
        // Only the display is a device here: "DISPLAY" or \\.\DISPLAYn. A
        // printer name is ERROR_INVALID_PRINTER_NAME, as for a printer
        // Windows does not have ("CreateDCW(NoSuchPrinter)=NULL err=1801").
        const int kind = gdi_display_device(m, m.arg_ptr(0), true);
        if (kind <= 0) {
            if (kind < 0) set_last_error(error_codes::ERROR_INVALID_PRINTER_NAME);
            m.set_return(0u);
            return;
        }
        m.set_return(static_cast<u64>(mgr.create_ic_w()));
    }, 16});

    // ---- DescribePixelFormat(hdc, iPixelFormat, nBytes, ppfd) ----
    // https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-describepixelformat
    exports.entries.push_back({"DescribePixelFormat", 0, [&mgr](Marshaler& m) { pixel_format_describe(mgr, m); }, 16});

    // ---- GetPixelFormat(hdc) ----
    exports.entries.push_back({"GetPixelFormat", 0, [&mgr](Marshaler& m) { pixel_format_get(mgr, m); }, 4});

    // ---- GetICMProfileW(hdc, pBufSize, pszFilename) ----
    exports.entries.push_back({"GetICMProfileW", 0, [](Marshaler& m) {
        // pBufSize is IN/OUT: the caller passes its buffer size in CHARACTERS
        // and Windows writes back the size it needs. Leaving it untouched
        // hands the caller back whatever it initialised the variable with,
        // and leaving pszFilename untouched hands back an unterminated
        // buffer. Callers poison both with 0xaa before the call, which is
        // how a path read out of one becomes a wild pointer.
        (void)m.arg(0);
        const GuestAddr buf_size_ptr = m.arg_ptr(1);
        const GuestAddr filename_ptr = m.arg_ptr(2);
        if (buf_size_ptr == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        const u32 in_chars = m.memory().read_u32(buf_size_ptr).value_or(0);
        // Vitreo reports no colour profile for its displays, so the required
        // size is zero and the name is the empty string.
        (void)m.write_u32(buf_size_ptr, 0u);
        if (filename_ptr != 0 && in_chars >= 1) {
            (void)m.write_u16(filename_ptr, 0u);
        }
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        m.set_return(0);
    }, 12});

    // ---- ExtEscape(hdc, iEscape, cjInput, lpInData, cjOutput, lpOutData) ----
    exports.entries.push_back({"ExtEscape", 0, [](Marshaler& m) {
        (void)m.arg(0); (void)m.arg(1); (void)m.arg(2);
        (void)m.arg_ptr(3); (void)m.arg(4); (void)m.arg_ptr(5);
        m.set_return(0); // unsupported escape
    }, 24});

    // ---- GetBitmapBits(hbm, cb, lpvBits) ----
    exports.entries.push_back({"GetBitmapBits", 0, [&mgr](Marshaler& m) {
        const auto hbm = static_cast<Handle>(m.arg(0));
        const i32 n = mgr.get_bitmap_bits(hbm, static_cast<i32>(m.arg32(1)), m.arg_ptr(2));
        // A non-bitmap sets ERROR_INVALID_HANDLE on x64 and leaves the last
        // error on x86 (s3gdi_bitmap_probe "gbb not-bitmap").
        if (n == 0 && m.cpu_is_64bit() && mgr.object_type(hbm) != OBJ_BITMAP_TYPE) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
        }
        m.set_return(static_cast<u64>(static_cast<u32>(n)));
    }, 12});
    exports.entries.push_back({"SetBitmapBits", 0, [&mgr](Marshaler& m) {
        const i32 n = mgr.set_bitmap_bits(static_cast<Handle>(m.arg(0)), m.arg32(1), m.arg_ptr(2));
        m.set_return(static_cast<u64>(static_cast<u32>(n)));
    }, 12});

    // ---- SetStretchBltMode(hdc, mode) — returns previous (= current) mode ----
    exports.entries.push_back({"SetStretchBltMode", 0, [&mgr](Marshaler& m) {
        m.set_return(static_cast<u32>(mgr.set_stretch_blt_mode(static_cast<Handle>(m.arg(0)),
                                                               static_cast<i32>(m.arg32(1)))));
    }, 8});
    exports.entries.push_back({"GetStretchBltMode", 0, [&mgr](Marshaler& m) {
        m.set_return(static_cast<u32>(mgr.get_stretch_blt_mode(static_cast<Handle>(m.arg(0)))));
    }, 4});
}

}  // namespace vitreo::winapi
