// SPDX-License-Identifier: LGPL-2.1-only
/// @file oleaut32_safearray_access.cpp
/// @brief OLEAUT32 SAFEARRAY introspection and element access:
///        bounds, lock/unlock, data access, Get/PutElement, PtrOfIndex, vartype.
///
/// Split from oleaut32.cpp by responsibility. Lifecycle is in
/// oleaut32_safearray.cpp.

#include "oleaut32.hpp"
#include "winapi/com/oleaut32/oleaut32_internal2.hpp"
#include "winapi/com/oleaut32/oleaut32_internal.hpp"
#include "winapi/com/oleaut32/oleaut32_shared_internal.hpp"
#include "winapi/com/oleaut32/oleaut32_safearray_internal.hpp"
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

u32 OleAut32Manager::safe_array_get_dim(Marshaler& m) {
    const GuestAddr psa = m.arg_ptr(0);
    if (psa == 0) return 0;
    return m.read_u16(psa).value_or(0);
}

u32 OleAut32Manager::safe_array_get_elemsize(Marshaler& m) {
    const GuestAddr psa = m.arg_ptr(0);
    if (psa == 0) return 0;
    return m.read_u32(psa + 4).value_or(0);
}

u32 OleAut32Manager::safe_array_get_ubound(Marshaler& m) {
    const GuestAddr psa = m.arg_ptr(0);
    const u32 n_dim = static_cast<u32>(m.arg(1));
    const GuestAddr out = m.arg_ptr(2);
    SaHeader hdr{};
    if (!read_sa_header(m, psa, hdr) || out == 0) return OA_E_INVALIDARG;
    if (n_dim == 0 || n_dim > hdr.cDims) return DISP_E_BADINDEX;
    const GuestAddr bound = sa_bound_addr(psa, hdr, n_dim - 1);
    const i64 c_elements = m.read_u32(bound).value_or(0);
    const i64 lbound = static_cast<i32>(m.read_u32(bound + 4).value_or(0));
    const i64 ubound = lbound + c_elements - 1;
    if (ubound > 0x7FFFFFFFll || ubound < -0x80000000ll) {
        // Learn: DISP_E_OVERFLOW; Windows stores -1 (probe ubound_ovf*).
        (void)m.write_u32(out, 0xFFFFFFFFu);
        return DISP_E_OVERFLOW_HR;
    }
    (void)m.write_u32(out, static_cast<u32>(static_cast<i32>(ubound)));
    return S_OK;
}

u32 OleAut32Manager::safe_array_get_lbound(Marshaler& m) {
    const GuestAddr psa = m.arg_ptr(0);
    const u32 n_dim = static_cast<u32>(m.arg(1));
    const GuestAddr out = m.arg_ptr(2);
    SaHeader hdr{};
    if (!read_sa_header(m, psa, hdr) || out == 0) return OA_E_INVALIDARG;
    if (n_dim == 0 || n_dim > hdr.cDims) return DISP_E_BADINDEX;
    const GuestAddr bound = sa_bound_addr(psa, hdr, n_dim - 1);
    (void)m.write_u32(out, m.read_u32(bound + 4).value_or(0));
    return S_OK;
}

u32 OleAut32Manager::safe_array_lock(Marshaler& m) {
    const GuestAddr psa = m.arg_ptr(0);
    SaHeader hdr{};
    if (!read_sa_header(m, psa, hdr)) return OA_E_INVALIDARG;
    if (hdr.cLocks == 0xFFFFFFFFu) return OA_E_UNEXPECTED;
    (void)m.write_u32(psa + 8, hdr.cLocks + 1);
    return S_OK;
}

u32 OleAut32Manager::safe_array_unlock(Marshaler& m) {
    const GuestAddr psa = m.arg_ptr(0);
    SaHeader hdr{};
    if (!read_sa_header(m, psa, hdr)) return OA_E_INVALIDARG;
    // Learn: E_UNEXPECTED "The array could not be unlocked" (probe unlock_zero).
    if (hdr.cLocks == 0) return OA_E_UNEXPECTED;
    (void)m.write_u32(psa + 8, hdr.cLocks - 1);
    return S_OK;
}

u32 OleAut32Manager::safe_array_access_data(Marshaler& m) {
    const GuestAddr psa = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    SaHeader hdr{};
    if (!read_sa_header(m, psa, hdr) || out == 0) return OA_E_INVALIDARG;
    if (hdr.cLocks == 0xFFFFFFFFu) return OA_E_UNEXPECTED;
    (void)m.write_u32(psa + 8, hdr.cLocks + 1);
    (void)m.write_ptr(out, hdr.pvData);
    return S_OK;
}

u32 OleAut32Manager::safe_array_unaccess_data(Marshaler& m) {
    return safe_array_unlock(m);
}

u32 OleAut32Manager::safe_array_ptr_of_index(Marshaler& m) {
    const GuestAddr psa = m.arg_ptr(0);
    const GuestAddr indices = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(2);
    SaHeader hdr{};
    if (!read_sa_header(m, psa, hdr) || indices == 0 || out == 0) {
        return OA_E_INVALIDARG;
    }
    u64 offset = 0;
    if (!sa_element_offset(m, psa, hdr, indices, offset)) return DISP_E_BADINDEX;
    (void)m.write_ptr(out, hdr.pvData + offset);
    return S_OK;
}

u32 OleAut32Manager::safe_array_get_element(Marshaler& m) {
    const GuestAddr psa = m.arg_ptr(0);
    const GuestAddr indices = m.arg_ptr(1);
    const GuestAddr pv = m.arg_ptr(2);
    SaHeader hdr{};
    if (!read_sa_header(m, psa, hdr) || indices == 0 || pv == 0) {
        return OA_E_INVALIDARG;
    }
    u64 offset = 0;
    if (!sa_element_offset(m, psa, hdr, indices, offset)) return DISP_E_BADINDEX;
    const GuestAddr cell = hdr.pvData + offset;

    if ((hdr.fFeatures & FADF_BSTR) != 0) {
        // Caller-owned copy, byte length preserved (probe bstr_odd_roundtrip).
        const GuestAddr src = static_cast<GuestAddr>(m.read_ptr(cell).value_or(0));
        GuestAddr copy = 0;
        if (src != 0) {
            copy = bstr_alloc_bytes(m, src, m.read_u32(src - 4).value_or(0));
            if (copy == 0) return OA_E_OUTOFMEMORY;
        }
        (void)m.write_ptr(pv, copy);
        return S_OK;
    }
    if ((hdr.fFeatures & (FADF_UNKNOWN | FADF_DISPATCH)) != 0) {
        // The out pointer is AddRef'd (probe unk_get: b_add 1 -> 2).
        const GuestAddr obj = static_cast<GuestAddr>(m.read_ptr(cell).value_or(0));
        com_add_ref(m, obj);
        (void)m.write_ptr(pv, obj);
        return S_OK;
    }
    if ((hdr.fFeatures & FADF_VARIANT) != 0) {
        // pv is an uninitialised VARIANT: init it, then VariantCopy.
        u8 zeros[24] = {};
        (void)m.memory().write_bytes(pv, zeros, oleaut_variant_size(m.cpu_is_64bit()));
        return variant_copy_at(m, pv, cell);
    }
    std::vector<u8> buf(hdr.cbElements);
    (void)m.memory().read_bytes(cell, buf.data(), buf.size());
    (void)m.memory().write_bytes(pv, buf.data(), buf.size());
    return S_OK;
}

u32 OleAut32Manager::safe_array_put_element(Marshaler& m) {
    const GuestAddr psa = m.arg_ptr(0);
    const GuestAddr indices = m.arg_ptr(1);
    const GuestAddr pv = m.arg_ptr(2);
    SaHeader hdr{};
    if (!read_sa_header(m, psa, hdr) || indices == 0) return OA_E_INVALIDARG;
    u64 offset = 0;
    if (!sa_element_offset(m, psa, hdr, indices, offset)) return DISP_E_BADINDEX;
    const GuestAddr cell = hdr.pvData + offset;

    if ((hdr.fFeatures & FADF_BSTR) != 0) {
        // pv IS the BSTR ("VT_BSTR ... do not require another level of
        // indirection"); a NULL BSTR is legal. The old element is freed.
        GuestAddr copy = 0;
        if (pv != 0) {
            copy = bstr_alloc_bytes(m, pv, m.read_u32(pv - 4).value_or(0));
            if (copy == 0) return OA_E_OUTOFMEMORY;
        }
        const GuestAddr old = static_cast<GuestAddr>(m.read_ptr(cell).value_or(0));
        (void)m.write_ptr(cell, copy);
        bstr_free(m, old);
        return S_OK;
    }
    if ((hdr.fFeatures & (FADF_UNKNOWN | FADF_DISPATCH)) != 0) {
        // pv IS the interface pointer: AddRef it, Release the one replaced
        // (probe unk_put / unk_overwrite).
        com_add_ref(m, pv);
        const GuestAddr old = static_cast<GuestAddr>(m.read_ptr(cell).value_or(0));
        (void)m.write_ptr(cell, pv);
        com_release(m, old);
        return S_OK;
    }
    if (pv == 0) return OA_E_INVALIDARG;
    if ((hdr.fFeatures & FADF_VARIANT) != 0) {
        // "copied correctly" / old element "cleared correctly": VariantCopy
        // clears the destination first (probe var_put_bstr: a distinct BSTR).
        return variant_copy_at(m, cell, pv);
    }
    std::vector<u8> buf(hdr.cbElements);
    (void)m.memory().read_bytes(pv, buf.data(), buf.size());
    (void)m.memory().write_bytes(cell, buf.data(), buf.size());
    return S_OK;
}

u32 OleAut32Manager::safe_array_get_vartype(Marshaler& m) {
    const GuestAddr psa = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    SaHeader hdr{};
    if (!read_sa_header(m, psa, hdr) || out == 0) return OA_E_INVALIDARG;
    u16 vt = 0;
    if ((hdr.fFeatures & FADF_HAVEVARTYPE) != 0) {
        vt = static_cast<u16>(m.read_u32(psa - 4).value_or(0));
    } else if ((hdr.fFeatures & FADF_RECORD) != 0) {
        vt = kVtRecord;
    } else if ((hdr.fFeatures & FADF_HAVEIID) != 0) {
        vt = (hdr.fFeatures & FADF_DISPATCH) != 0 ? VT_DISPATCH : VT_UNKNOWN;
    } else {
        // Without the hidden type Windows does not guess from the other
        // bits, not even FADF_BSTR (probe vartype_nohave_*).
        return OA_E_INVALIDARG;
    }
    (void)m.write_u16(out, vt);
    return S_OK;
}

} // namespace vitreo::winapi
