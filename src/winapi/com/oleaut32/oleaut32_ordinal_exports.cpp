// SPDX-License-Identifier: LGPL-2.1-only
/// @file oleaut32_ordinal_exports.cpp
/// @brief OLEAUT32 exports that are neither BSTR/VARIANT/SAFEARRAY core nor
///        wire marshaling: OleCreateFontIndirect, the type-library entry
///        thunks and the ordinal-only exports (VarUI4FromStr, VarBstrCat,
///        VarBstrCmp, LoadRegTypeLib).
///
/// Split from oleaut32.cpp by responsibility.

#include "oleaut32.hpp"
#include "winapi/com/oleaut32/oleaut32_internal2.hpp"
#include "winapi/com/oleaut32/oleaut32_internal.hpp"
#include "winapi/com/oleaut32/oleaut32_shared_internal.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace vitreo::winapi {

u32 OleAut32Manager::ole_create_font_indirect() {
    return E_NOTIMPL;
}

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

void oleaut32::impl_OleCreateFontIndirect(Marshaler& m, OleAut32Manager& mgr) {
    u32 rc = mgr.ole_create_font_indirect();
    m.set_return(rc);
}

void oleaut32::impl_RegisterTypeLib(Marshaler& m, OleAut32Manager& mgr) {
    u32 rc = mgr.register_type_lib(m);
    m.set_return(rc);
}

void oleaut32::impl_LoadTypeLib(Marshaler& m, OleAut32Manager& mgr) {
    u32 rc = mgr.load_type_lib(m);
    m.set_return(rc);
}

// =============================================================================
// Ordinal-only OLEAUT32 exports.
//
// Callers import these from OLEAUT32 by ordinal. A fail-thunk that returns
// 0xFFFFFFFF is a bogus non-NULL pointer for SafeArrayCreateVector and an
// unwritten out-parameter for LoadRegTypeLib. Either fault deadlocks init
// on the loader lock. These implement the Windows contracts, or fail honestly.
// =============================================================================

// Result/HRESULT constants used by the ordinal exports below.
static constexpr u32 DISP_E_OVERFLOW         = 0x8002000A;
static constexpr u32 TYPE_E_LIBNOTREGISTERED = 0x8002801D;
// VarBstrCmp returns one of these (typed as HRESULT).
static constexpr u32 VARCMP_LT = 0;
static constexpr u32 VARCMP_EQ = 1;
static constexpr u32 VARCMP_GT = 2;

// Read a BSTR (length-prefixed UTF-16; pointer is to the first char, the
// 4-byte byte-length prefix lives at ptr-4) into a UTF-16 string. A NULL BSTR
// is treated as the empty string (Windows convention for VarBstrCmp).
static std::u16string read_bstr_u16(Marshaler& m, GuestAddr bstr) {
    std::u16string s;
    if (bstr == 0) return s;
    u32 byte_len = m.read_u32(bstr - 4).value_or(0);
    if (byte_len == 0) return s;
    u32 n = byte_len / 2;
    s.resize(n);
    for (u32 i = 0; i < n; ++i) {
        s[i] = static_cast<char16_t>(m.read_u16(bstr + i * 2).value_or(0));
    }
    return s;
}

// Read a NUL-terminated UTF-16 string.
static std::u16string read_wide_z(Marshaler& m, GuestAddr psz, u32 max_chars = 4096) {
    std::u16string s;
    if (psz == 0) return s;
    for (u32 i = 0; i < max_chars; ++i) {
        u16 ch = m.read_u16(psz + i * 2).value_or(0);
        if (ch == 0) break;
        s.push_back(static_cast<char16_t>(ch));
    }
    return s;
}

// #277 HRESULT VarUI4FromStr(OLECHAR* strIn, LCID, ULONG dwFlags, ULONG* pulOut)
void oleaut32::impl_VarUI4FromStr(Marshaler& m) {
    oa_log_first("VarUI4FromStr");
    GuestAddr strIn = m.arg_ptr(0);
    GuestAddr pulOut = m.arg_ptr(3);
    std::u16string s = read_wide_z(m, strIn);
    size_t i = 0;
    while (i < s.size() && (s[i] == u' ' || s[i] == u'\t')) ++i;
    bool any = false, overflow = false;
    unsigned long long acc = 0;
    for (; i < s.size(); ++i) {
        char16_t c = s[i];
        if (c < u'0' || c > u'9') break;
        any = true;
        acc = acc * 10ull + static_cast<unsigned>(c - u'0');
        if (acc > 0xFFFFFFFFull) overflow = true;
    }
    if (!any) { if (pulOut) (void)m.write_u32(pulOut, 0); m.set_return(DISP_E_TYPEMISMATCH); return; }
    if (overflow) { if (pulOut) (void)m.write_u32(pulOut, 0); m.set_return(DISP_E_OVERFLOW); return; }
    if (pulOut) (void)m.write_u32(pulOut, static_cast<u32>(acc));
    m.set_return(S_OK);
}

// #313 HRESULT VarBstrCat(BSTR bstrLeft, BSTR bstrRight, LPBSTR pbstrOut)
void oleaut32::impl_VarBstrCat(Marshaler& m, OleAut32Manager& mgr) {
    oa_log_first("VarBstrCat");
    constexpr u32 kEInvalidArg = 0x80070057u;
    constexpr u32 kEOutOfMemory = 0x8007000Eu;
    const GuestAddr left = m.arg_ptr(0);
    const GuestAddr right = m.arg_ptr(1);
    const GuestAddr out_ptr = m.arg_ptr(2);
    if (out_ptr == 0) {
        m.set_return(kEInvalidArg);
        return;
    }
    std::u16string ls = read_bstr_u16(m, left);
    std::u16string rs = read_bstr_u16(m, right);
    if ((ls.size() + rs.size()) * 2 > kBstrMaxBytes) {
        (void)m.write_ptr(out_ptr, 0);
        m.set_return(kEOutOfMemory);
        return;
    }
    std::vector<Byte> bytes((ls.size() + rs.size()) * 2);
    for (usize i = 0; i < ls.size(); ++i) {
        const u16 ch = static_cast<u16>(ls[i]);
        bytes[i * 2] = static_cast<Byte>(ch & 0xFF);
        bytes[i * 2 + 1] = static_cast<Byte>((ch >> 8) & 0xFF);
    }
    for (usize i = 0; i < rs.size(); ++i) {
        const u16 ch = static_cast<u16>(rs[i]);
        const usize o = (ls.size() + i) * 2;
        bytes[o] = static_cast<Byte>(ch & 0xFF);
        bytes[o + 1] = static_cast<Byte>((ch >> 8) & 0xFF);
    }
    const GuestAddr bstr = mgr.allocate_bstr_bytes(m, bytes);
    if (bstr == 0) {
        (void)m.write_ptr(out_ptr, 0);
        m.set_return(kEOutOfMemory);
        return;
    }
    if (m.write_ptr(out_ptr, bstr).is_err()) {
        mgr.sys_free_string(m, bstr);
        m.set_return(kEInvalidArg);
        return;
    }
    m.set_return(S_OK);
}

// #314 HRESULT VarBstrCmp(BSTR bstrLeft, BSTR bstrRight, LCID, ULONG) -> VARCMP_*
void oleaut32::impl_VarBstrCmp(Marshaler& m) {
    oa_log_first("VarBstrCmp");
    std::u16string ls = read_bstr_u16(m, m.arg_ptr(0));
    std::u16string rs = read_bstr_u16(m, m.arg_ptr(1));
    int c = ls.compare(rs);  // ordinal UTF-16 comparison (lcid/flags ignored)
    m.set_return(c < 0 ? VARCMP_LT : (c > 0 ? VARCMP_GT : VARCMP_EQ));
}

// #162 HRESULT LoadRegTypeLib(REFGUID, WORD, WORD, LCID, ITypeLib** pptlib)
// Implemented on OleAut32Manager — looks up HKCR\TypeLib after RegisterTypeLib.
void oleaut32::impl_LoadRegTypeLib(Marshaler& m, OleAut32Manager& mgr) {
    oa_log_first("LoadRegTypeLib");
    m.set_return(mgr.load_reg_type_lib(m));
}

} // namespace vitreo::winapi
