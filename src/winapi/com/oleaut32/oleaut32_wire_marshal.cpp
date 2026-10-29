// SPDX-License-Identifier: LGPL-2.1-only
/// @file oleaut32_wire_marshal.cpp
/// @brief OLEAUT32 NDR user-marshal surface (MS-OAUT wire format) for BSTR,
///        VARIANT and LPSAFEARRAY: *_UserFree/Size/Marshal/Unmarshal.
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

// #286 void BSTR_UserFree(ULONG *pFlags, BSTR *pBstr)
//
// NDR user-marshaling free for BSTR (oaidl.h). Frees *pBstr via SysFreeString
// semantics and clears the caller's pointer. Imported by ordinal. A missing
// export is a GetProcAddress failure during DllMain.
void oleaut32::impl_BSTR_UserFree(Marshaler& m, OleAut32Manager& mgr) {
    oa_log_first("BSTR_UserFree");
    (void)m.arg_ptr(0); // pFlags — RPC wire flags; unused for free
    const GuestAddr pbstr = m.arg_ptr(1);
    if (pbstr == 0) {
        return;
    }
    GuestAddr bstr = 0;
    if (m.cpu_is_64bit()) {
        bstr = static_cast<GuestAddr>(m.read_u64(pbstr).value_or(0));
        (void)m.write_u64(pbstr, 0);
    } else {
        bstr = static_cast<GuestAddr>(m.read_u32(pbstr).value_or(0));
        (void)m.write_u32(pbstr, 0);
    }
    mgr.sys_free_string(m, bstr);
}

// -----------------------------------------------------------------------------
// BSTR NDR user-marshal helpers (#283–#286). Wire body is MS-OAUT
// FLAGGED_WORD_BLOB: ULONG cBytes, ULONG clSize, USHORT asData[clSize].
// Null BSTR: cBytes=0xFFFFFFFF, clSize=0, no asData. Empty: both 0.
// Buffer is 4-byte aligned before the blob ([C706] NDR alignment).
// -----------------------------------------------------------------------------

static GuestAddr bstr_wire_align(GuestAddr p) noexcept {
    return (p + 3u) & ~GuestAddr(3u);
}

static GuestAddr read_bstr_slot(Marshaler& m, GuestAddr pbstr) {
    if (pbstr == 0) return 0;
    if (m.cpu_is_64bit()) {
        return static_cast<GuestAddr>(m.read_u64(pbstr).value_or(0));
    }
    return static_cast<GuestAddr>(m.read_u32(pbstr).value_or(0));
}

static void write_bstr_slot(Marshaler& m, GuestAddr pbstr, GuestAddr bstr) {
    if (pbstr == 0) return;
    if (m.cpu_is_64bit()) (void)m.write_u64(pbstr, bstr);
    else (void)m.write_u32(pbstr, static_cast<u32>(bstr));
}

// Bytes consumed by the FLAGGED_WORD_BLOB body after alignment.
static u32 bstr_wire_body_size(Marshaler& m, GuestAddr bstr) {
    if (bstr == 0) return 8; // null transmitted BSTR
    const u32 c_bytes = m.read_u32(bstr - 4).value_or(0); // SysStringByteLen
    const u32 cl_size = (c_bytes + 1u) / 2u;
    return 8u + cl_size * 2u;
}

// #283 ULONG BSTR_UserSize(ULONG *pFlags, ULONG StartingSize, BSTR *pBstr)
void oleaut32::impl_BSTR_UserSize(Marshaler& m) {
    oa_log_first("BSTR_UserSize");
    (void)m.arg_ptr(0);
    const u32 starting = static_cast<u32>(m.arg(1));
    const GuestAddr bstr = read_bstr_slot(m, m.arg_ptr(2));
    const GuestAddr aligned = bstr_wire_align(static_cast<GuestAddr>(starting));
    const u32 pad = static_cast<u32>(aligned - starting);
    m.set_return(starting + pad + bstr_wire_body_size(m, bstr));
}

// #284 unsigned char* BSTR_UserMarshal(ULONG *pFlags, unsigned char *Buffer, BSTR *pBstr)
void oleaut32::impl_BSTR_UserMarshal(Marshaler& m) {
    oa_log_first("BSTR_UserMarshal");
    (void)m.arg_ptr(0);
    GuestAddr buffer = m.arg_ptr(1);
    const GuestAddr bstr = read_bstr_slot(m, m.arg_ptr(2));
    buffer = bstr_wire_align(buffer);
    if (bstr == 0) {
        (void)m.write_u32(buffer, 0xFFFFFFFFu);
        (void)m.write_u32(buffer + 4, 0);
        m.set_return(buffer + 8);
        return;
    }
    const u32 c_bytes = m.read_u32(bstr - 4).value_or(0);
    const u32 cl_size = (c_bytes + 1u) / 2u;
    (void)m.write_u32(buffer, c_bytes);
    (void)m.write_u32(buffer + 4, cl_size);
    if (cl_size != 0) {
        std::vector<Byte> data(cl_size * 2u, 0);
        const u32 copy = std::min(c_bytes, cl_size * 2u);
        if (copy != 0) {
            (void)m.memory().read_bytes(bstr, data.data(), copy);
        }
        (void)m.memory().write_bytes(buffer + 8, data.data(), cl_size * 2u);
    }
    m.set_return(buffer + 8 + cl_size * 2u);
}

// #285 unsigned char* BSTR_UserUnmarshal(ULONG *pFlags, unsigned char *Buffer, BSTR *pBstr)
void oleaut32::impl_BSTR_UserUnmarshal(Marshaler& m, OleAut32Manager& mgr) {
    oa_log_first("BSTR_UserUnmarshal");
    (void)m.arg_ptr(0);
    GuestAddr buffer = bstr_wire_align(m.arg_ptr(1));
    const GuestAddr pbstr = m.arg_ptr(2);
    const u32 c_bytes = m.read_u32(buffer).value_or(0);
    const u32 cl_size = m.read_u32(buffer + 4).value_or(0);
    buffer += 8;
    if (c_bytes == 0xFFFFFFFFu) {
        write_bstr_slot(m, pbstr, 0);
        m.set_return(buffer);
        return;
    }
    // Allocate via SysAllocStringLen from the wchar payload.
    GuestAddr new_bstr = 0;
    if (cl_size != 0) {
        // sys_alloc_string_len expects a guest OLECHAR* source; point at asData.
        new_bstr = mgr.sys_alloc_string_len(m, buffer, cl_size);
        // sys_alloc_string_len copies cch chars and adds its own NUL; if the
        // wire cBytes is odd, the last byte of the final USHORT may be padding.
        if (new_bstr != 0 && c_bytes < cl_size * 2u) {
            (void)m.write_u32(new_bstr - 4, c_bytes);
        }
    } else {
        // Empty transmitted BSTR → zero-length allocated BSTR (non-NULL).
        new_bstr = mgr.sys_alloc_string_len(m, 0, 0);
    }
    write_bstr_slot(m, pbstr, new_bstr);
    m.set_return(buffer + cl_size * 2u);
}

// -----------------------------------------------------------------------------
// VARIANT (#287–#290) and LPSAFEARRAY (#291–#294) NDR user-marshal helpers.
// Ordinals from NT oleaut32 export tables. ISSetup.dll GetProcAddress-probes
// these during DllMain; missing ordinals abort with a MessageBox.
//
// VARIANT_UserFree → VariantClear (oaidl.h). LPSAFEARRAY_UserFree →
// SafeArrayDestroy on *ppsa and clears the slot. Size/Marshal/Unmarshal handle
// the empty/null wire case; non-null bodies are not yet fully encoded (will
// TRACE on first use so a later COM marshal failure is diagnosable).
// -----------------------------------------------------------------------------

void oleaut32::impl_VARIANT_UserFree(Marshaler& m, OleAut32Manager& mgr) {
    oa_log_first("VARIANT_UserFree");
    (void)m.arg_ptr(0);
    const GuestAddr pvar = m.arg_ptr(1);
    if (pvar == 0) return;
    // VariantClear contract on the pointed-to VARIANT (not a VARIANT**).
    const u16 vt = m.read_u16(pvar).value_or(0);
    if ((vt & VT_TYPEMASK) == VT_BSTR && (vt & VT_BYREF) == 0) {
        GuestAddr bstr = 0;
        if (m.cpu_is_64bit()) {
            bstr = static_cast<GuestAddr>(m.read_u64(pvar + 8).value_or(0));
        } else {
            bstr = static_cast<GuestAddr>(m.read_u32(pvar + 8).value_or(0));
        }
        mgr.sys_free_string(m, bstr);
    }
    u8 zeros[24] = {};
    (void)m.memory().write_bytes(pvar, zeros, oleaut_variant_size(m.cpu_is_64bit()));
}

void oleaut32::impl_VARIANT_UserSize(Marshaler& m) {
    oa_log_first("VARIANT_UserSize");
    (void)m.arg_ptr(0);
    const u32 starting = static_cast<u32>(m.arg(1));
    const GuestAddr pvar = m.arg_ptr(2);
    const GuestAddr aligned = bstr_wire_align(static_cast<GuestAddr>(starting));
    const u32 pad = static_cast<u32>(aligned - starting);
    // Empty VARIANT wire: ULONG vt payload marker (0) — enough for VT_EMPTY.
    // Non-empty types still reserve a 16-byte body so the stub stays aligned.
    u32 body = 4;
    if (pvar != 0) {
        const u16 vt = m.read_u16(pvar).value_or(0);
        if (vt != VT_EMPTY && vt != VT_NULL) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::fprintf(stderr,
                    "[oleaut32] VARIANT_UserSize non-empty vt=%u "
                    "(wire body not fully implemented)\n", vt);
            }
            body = 4 + 16;
        }
    }
    m.set_return(starting + pad + body);
}

void oleaut32::impl_VARIANT_UserMarshal(Marshaler& m) {
    oa_log_first("VARIANT_UserMarshal");
    (void)m.arg_ptr(0);
    GuestAddr buffer = bstr_wire_align(m.arg_ptr(1));
    const GuestAddr pvar = m.arg_ptr(2);
    const u16 vt = (pvar != 0) ? m.read_u16(pvar).value_or(0) : static_cast<u16>(VT_EMPTY);
    (void)m.write_u32(buffer, vt);
    buffer += 4;
    if (vt != VT_EMPTY && vt != VT_NULL && pvar != 0) {
        u8 raw[16] = {};
        (void)m.memory().read_bytes(pvar, raw, 16);
        (void)m.memory().write_bytes(buffer, raw, 16);
        buffer += 16;
    }
    m.set_return(buffer);
}

void oleaut32::impl_VARIANT_UserUnmarshal(Marshaler& m) {
    oa_log_first("VARIANT_UserUnmarshal");
    (void)m.arg_ptr(0);
    GuestAddr buffer = bstr_wire_align(m.arg_ptr(1));
    const GuestAddr pvar = m.arg_ptr(2);
    const u32 vt = m.read_u32(buffer).value_or(0);
    buffer += 4;
    if (pvar != 0) {
        u8 zeros[16] = {};
        (void)m.memory().write_bytes(pvar, zeros, 16);
        (void)m.write_u16(pvar, static_cast<u16>(vt));
        if (vt != VT_EMPTY && vt != VT_NULL) {
            u8 raw[16] = {};
            (void)m.memory().read_bytes(buffer, raw, 16);
            (void)m.memory().write_bytes(pvar, raw, 16);
            buffer += 16;
        }
    }
    m.set_return(buffer);
}

static GuestAddr read_psa_slot(Marshaler& m, GuestAddr ppsa) {
    if (ppsa == 0) return 0;
    if (m.cpu_is_64bit()) {
        return static_cast<GuestAddr>(m.read_u64(ppsa).value_or(0));
    }
    return static_cast<GuestAddr>(m.read_u32(ppsa).value_or(0));
}

static void write_psa_slot(Marshaler& m, GuestAddr ppsa, GuestAddr psa) {
    if (ppsa == 0) return;
    if (m.cpu_is_64bit()) (void)m.write_u64(ppsa, psa);
    else (void)m.write_u32(ppsa, static_cast<u32>(psa));
}

void oleaut32::impl_LPSAFEARRAY_UserFree(Marshaler& m, OleAut32Manager& mgr) {
    oa_log_first("LPSAFEARRAY_UserFree");
    (void)m.arg_ptr(0);
    const GuestAddr ppsa = m.arg_ptr(1);
    if (ppsa == 0) return;
    const GuestAddr psa = read_psa_slot(m, ppsa);
    (void)mgr.sa_destroy(m, psa);
    write_psa_slot(m, ppsa, 0);
}

void oleaut32::impl_LPSAFEARRAY_UserSize(Marshaler& m) {
    oa_log_first("LPSAFEARRAY_UserSize");
    (void)m.arg_ptr(0);
    const u32 starting = static_cast<u32>(m.arg(1));
    const GuestAddr psa = read_psa_slot(m, m.arg_ptr(2));
    const GuestAddr aligned = bstr_wire_align(static_cast<GuestAddr>(starting));
    const u32 pad = static_cast<u32>(aligned - starting);
    // Null unique: ULONG 0. Non-null: ULONG 1 + reserved stub (not full wire).
    u32 body = 4;
    if (psa != 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                "[oleaut32] LPSAFEARRAY_UserSize non-null "
                "(wire body not fully implemented)\n");
        }
        body = 8;
    }
    m.set_return(starting + pad + body);
}

void oleaut32::impl_LPSAFEARRAY_UserMarshal(Marshaler& m) {
    oa_log_first("LPSAFEARRAY_UserMarshal");
    (void)m.arg_ptr(0);
    GuestAddr buffer = bstr_wire_align(m.arg_ptr(1));
    const GuestAddr psa = read_psa_slot(m, m.arg_ptr(2));
    if (psa == 0) {
        (void)m.write_u32(buffer, 0);
        m.set_return(buffer + 4);
        return;
    }
    (void)m.write_u32(buffer, 1);
    (void)m.write_u32(buffer + 4, 0); // stub payload
    m.set_return(buffer + 8);
}

void oleaut32::impl_LPSAFEARRAY_UserUnmarshal(Marshaler& m) {
    oa_log_first("LPSAFEARRAY_UserUnmarshal");
    (void)m.arg_ptr(0);
    GuestAddr buffer = bstr_wire_align(m.arg_ptr(1));
    const GuestAddr ppsa = m.arg_ptr(2);
    const u32 tag = m.read_u32(buffer).value_or(0);
    buffer += 4;
    if (tag == 0) {
        write_psa_slot(m, ppsa, 0);
        m.set_return(buffer);
        return;
    }
    (void)m.read_u32(buffer); // stub
    buffer += 4;
    write_psa_slot(m, ppsa, 0); // cannot reconstruct array yet
    m.set_return(buffer);
}

} // namespace vitreo::winapi
