// SPDX-License-Identifier: LGPL-2.1-only
/// @file oleaut32_safearray.cpp
/// @brief OLEAUT32 SAFEARRAY lifecycle: create, vector, destroy, copy, redim.
///
/// Split from oleaut32.cpp by responsibility. Element access and
/// introspection are in oleaut32_safearray_access.cpp.

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

// =============================================================================
// SAFEARRAY
//
// Layout and semantics measured on Windows (tools/oracle_probe/
// oleaut32_safearray_probe.c, .expected / .x86.expected):
//  - the descriptor stores rgsabound REVERSED: psa->rgsabound[cDims-1-i] is
//    the caller's rgsabound[i], so dimension nDim (1-based, caller order) is
//    psa->rgsabound[cDims-nDim];
//  - rgIndices[i] pairs with the caller's rgsabound[i] and rgIndices[0] varies
//    fastest (column-major);
//  - fFeatures is FADF_HAVEVARTYPE (+ the hidden VARTYPE dword at psa-4) plus
//    FADF_BSTR / FADF_VARIANT, or FADF_HAVEIID|FADF_UNKNOWN / FADF_DISPATCH
//    (the IID in the 16 bytes before the descriptor). Never FADF_FIXEDSIZE.
// =============================================================================

namespace {

/// Element size for a SafeArrayCreate base type; 0 = type rejected (NULL).
/// Table measured on Windows 10 (probe create_vt for vt 0..0x4a and the
/// VT_ARRAY/VT_BYREF/VT_VECTOR/0xFFFF forms, which are all rejected).
[[nodiscard]] u32 sa_element_size(u16 vt, bool is64) noexcept {
    switch (vt) {
    case 2:  /*VT_I2*/      case 11: /*VT_BOOL*/  case 18: /*VT_UI2*/  return 2;
    case 3:  /*VT_I4*/      case 4:  /*VT_R4*/    case 10: /*VT_ERROR*/
    case 19: /*VT_UI4*/     case 22: /*VT_INT*/   case 23: /*VT_UINT*/ return 4;
    case 5:  /*VT_R8*/      case 6:  /*VT_CY*/    case 7:  /*VT_DATE*/
    case 20: /*VT_I8*/      case 21: /*VT_UI8*/                          return 8;
    case 16: /*VT_I1*/      case 17: /*VT_UI1*/                          return 1;
    case 14: /*VT_DECIMAL*/                                              return 16;
    case 8:  /*VT_BSTR*/    case 9:  /*VT_DISPATCH*/ case 13: /*VT_UNKNOWN*/
    case 37: /*VT_INT_PTR*/ case 38: /*VT_UINT_PTR*/  return is64 ? 8u : 4u;
    case 12: /*VT_VARIANT*/ return is64 ? 24u : 16u;
    default: return 0;
    }
}

} // namespace

GuestAddr OleAut32Manager::sa_create(Marshaler& m, u16 vt, u32 c_dims,
                                     GuestAddr rgsabound) {
    const bool is64 = m.cpu_is_64bit();
    const u32 cb = sa_element_size(vt, is64);
    if (cb == 0 || c_dims == 0 || c_dims > 0xFFFFu || rgsabound == 0) {
        return 0;
    }
    std::vector<std::pair<u32, i32>> bounds(c_dims);
    u64 cells = 1;
    bool empty = false;
    for (u32 i = 0; i < c_dims; ++i) {
        const auto c = m.read_u32(rgsabound + i * 8);
        const auto l = m.read_u32(rgsabound + i * 8 + 4);
        if (c.is_err() || l.is_err()) return 0;
        bounds[i] = {c.value(), static_cast<i32>(l.value())};
        if (c.value() == 0) empty = true;
        if (!empty) {
            cells *= c.value();
            // The data size is a ULONG product on Windows: an array whose
            // byte size does not fit is refused (probe huge_wrap).
            if (cells * cb > 0xFFFFFFFFull) return 0;
        }
    }
    if (empty) cells = 0;

    const u32 bounds_off = oleaut_sa_bounds_off(is64);
    const GuestAddr block = oa_alloc(m, kSaHiddenSize + bounds_off + c_dims * 8ull);
    if (block == 0) return 0;
    const GuestAddr psa = block + kSaHiddenSize;
    // Windows allocates the data block even for an empty array (probe
    // zero_dim / vector_empty_bstr: pvData is non-NULL).
    const GuestAddr data = oa_alloc(m, cells * cb);
    if (data == 0) {
        oa_free(block);
        return 0;
    }

    u16 features = FADF_HAVEVARTYPE;
    if (vt == VT_BSTR) features |= FADF_BSTR;
    if (vt == VT_VARIANT) features |= FADF_VARIANT;
    if (vt == VT_UNKNOWN) features = FADF_HAVEIID | FADF_UNKNOWN;
    if (vt == VT_DISPATCH) features = FADF_HAVEIID | FADF_DISPATCH;

    (void)m.write_u16(psa + 0, static_cast<u16>(c_dims));
    (void)m.write_u16(psa + 2, features);
    (void)m.write_u32(psa + 4, cb);
    (void)m.write_u32(psa + 8, 0);
    (void)m.write_ptr(psa + oleaut_sa_pvdata_off(is64), data);
    for (u32 i = 0; i < c_dims; ++i) {
        const GuestAddr slot = psa + bounds_off + (c_dims - 1u - i) * 8u;
        (void)m.write_u32(slot, bounds[i].first);
        (void)m.write_u32(slot + 4, static_cast<u32>(bounds[i].second));
    }
    if ((features & FADF_HAVEIID) != 0) {
        // IID_IUnknown {00000000-0000-0000-C000-000000000046} /
        // IID_IDispatch {00020400-0000-0000-C000-000000000046}.
        const std::array<u8, 16> iid{
            0x00, static_cast<u8>(vt == VT_DISPATCH ? 0x04 : 0x00),
            static_cast<u8>(vt == VT_DISPATCH ? 0x02 : 0x00), 0x00,
            0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x46};
        (void)m.memory().write_bytes(psa - 16, iid.data(), iid.size());
    } else {
        (void)m.write_u32(psa - 4, vt);
    }
    return psa;
}

GuestAddr OleAut32Manager::sa_create_vector(Marshaler& m, u16 vt, i32 lbound,
                                            u32 count) {
    // SafeArrayCreateVector allocates descriptor and data as ONE block, the
    // data directly after the single bound (probe vector: pvData - psa ==
    // 0x20 on x64, 0x18 on x86) and adds FADF_CREATEVECTOR. Interface and
    // record vectors are left to SafeArrayCreateVectorEx.
    const bool is64 = m.cpu_is_64bit();
    const u32 cb = sa_element_size(vt, is64);
    if (cb == 0) return 0;
    const u64 data_bytes = static_cast<u64>(count) * cb;
    if (data_bytes > 0xFFFFFFFFull) return 0;
    const u32 desc = oleaut_sa_bounds_off(is64) + 8;
    const GuestAddr block = oa_alloc(m, kSaHiddenSize + desc + data_bytes);
    if (block == 0) return 0;
    const GuestAddr psa = block + kSaHiddenSize;
    u16 features = FADF_HAVEVARTYPE | FADF_CREATEVECTOR;
    if (vt == VT_BSTR) features |= FADF_BSTR;
    if (vt == VT_VARIANT) features |= FADF_VARIANT;
    if (vt == VT_UNKNOWN) features = FADF_HAVEIID | FADF_UNKNOWN | FADF_CREATEVECTOR;
    if (vt == VT_DISPATCH) features = FADF_HAVEIID | FADF_DISPATCH | FADF_CREATEVECTOR;
    (void)m.write_u16(psa + 0, 1);
    (void)m.write_u16(psa + 2, features);
    (void)m.write_u32(psa + 4, cb);
    (void)m.write_u32(psa + 8, 0);
    (void)m.write_ptr(psa + oleaut_sa_pvdata_off(is64), psa + desc);
    (void)m.write_u32(psa + oleaut_sa_bounds_off(is64), count);
    (void)m.write_u32(psa + oleaut_sa_bounds_off(is64) + 4, static_cast<u32>(lbound));
    if ((features & FADF_HAVEIID) != 0) {
        const std::array<u8, 16> iid{
            0x00, static_cast<u8>(vt == VT_DISPATCH ? 0x04 : 0x00),
            static_cast<u8>(vt == VT_DISPATCH ? 0x02 : 0x00), 0x00,
            0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x46};
        (void)m.memory().write_bytes(psa - 16, iid.data(), iid.size());
    } else {
        (void)m.write_u32(psa - 4, vt);
    }
    return psa;
}

namespace {

/// Release what `count` cells starting at `cells` own, per fFeatures.
void sa_clear_cells(OleAut32Manager& mgr, Marshaler& m, u16 features,
                    u32 cb, GuestAddr cells, u64 count) {
    if (cells == 0) return;
    if ((features & FADF_BSTR) != 0) {
        for (u64 i = 0; i < count; ++i) {
            const GuestAddr slot = cells + i * cb;
            mgr.bstr_free(m, static_cast<GuestAddr>(m.read_ptr(slot).value_or(0)));
            (void)m.write_ptr(slot, 0);
        }
    } else if ((features & (FADF_UNKNOWN | FADF_DISPATCH)) != 0) {
        for (u64 i = 0; i < count; ++i) {
            const GuestAddr slot = cells + i * cb;
            mgr.com_release(m, static_cast<GuestAddr>(m.read_ptr(slot).value_or(0)));
            (void)m.write_ptr(slot, 0);
        }
    } else if ((features & FADF_VARIANT) != 0) {
        for (u64 i = 0; i < count; ++i) {
            (void)mgr.variant_clear_at(m, cells + i * cb);
        }
    } else if ((features & FADF_RECORD) != 0 && count != 0) {
        std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED oleaut32!SafeArrayDestroy: FADF_RECORD "
            "elements not cleared\n");
    }
}

} // namespace

u32 OleAut32Manager::sa_destroy(Marshaler& m, GuestAddr psa) {
    if (psa == 0) return S_OK;
    SaHeader hdr{};
    if (!read_sa_header(m, psa, hdr)) return OA_E_INVALIDARG;
    // Learn: DISP_E_ARRAYISLOCKED, and nothing is touched (probe destroy_locked).
    if (hdr.cLocks != 0) return DISP_E_ARRAYISLOCKED;
    const u64 cells = sa_total_cells(m, psa, hdr);
    sa_clear_cells(*this, m, hdr.fFeatures, hdr.cbElements, hdr.pvData, cells);
    const bool owned_data = (hdr.fFeatures & (FADF_STATIC | FADF_EMBEDDED)) == 0 &&
                            (hdr.fFeatures & FADF_CREATEVECTOR) == 0;
    if (owned_data && hdr.pvData != 0) {
        oa_free(hdr.pvData);
    }
    (void)m.write_ptr(psa + oleaut_sa_pvdata_off(m.cpu_is_64bit()), 0);
    if ((hdr.fFeatures & (FADF_AUTO | FADF_STATIC | FADF_EMBEDDED)) == 0) {
        oa_free(psa - kSaHiddenSize);
    }
    return S_OK;
}

GuestAddr OleAut32Manager::safe_array_create(Marshaler& m) {
    return sa_create(m, static_cast<u16>(m.arg(0)),
                     static_cast<u32>(m.arg(1)), m.arg_ptr(2));
}

u32 OleAut32Manager::safe_array_destroy(Marshaler& m) {
    return sa_destroy(m, m.arg_ptr(0));
}

u32 OleAut32Manager::sa_copy(Marshaler& m, GuestAddr src, GuestAddr& out) {
    out = 0;
    if (src == 0) return S_OK; // probe copy_null: S_OK and *ppsaOut = NULL
    SaHeader hdr{};
    if (!read_sa_header(m, src, hdr)) return OA_E_INVALIDARG;
    const bool is64 = m.cpu_is_64bit();
    const u64 cells = sa_total_cells(m, src, hdr);
    const u64 data_bytes = cells * hdr.cbElements;
    const GuestAddr block =
        oa_alloc(m, kSaHiddenSize + hdr.bounds_off + hdr.cDims * 8ull);
    if (block == 0) return OA_E_OUTOFMEMORY;
    const GuestAddr dst = block + kSaHiddenSize;
    const GuestAddr data = oa_alloc(m, data_bytes);
    if (data == 0) {
        oa_free(block);
        return OA_E_OUTOFMEMORY;
    }
    // Hidden IID / VARTYPE travel with the descriptor.
    std::array<u8, kSaHiddenSize> hidden{};
    (void)m.memory().read_bytes(src - kSaHiddenSize, hidden.data(), hidden.size());
    (void)m.memory().write_bytes(block, hidden.data(), hidden.size());
    // The copy owns its storage: drop the ownership / placement flags.
    const u16 features = static_cast<u16>(
        hdr.fFeatures & ~(FADF_AUTO | FADF_STATIC | FADF_EMBEDDED |
                          FADF_CREATEVECTOR));
    (void)m.write_u16(dst + 0, hdr.cDims);
    (void)m.write_u16(dst + 2, features);
    (void)m.write_u32(dst + 4, hdr.cbElements);
    (void)m.write_u32(dst + 8, 0);
    (void)m.write_ptr(dst + oleaut_sa_pvdata_off(is64), data);
    std::vector<u8> bounds(hdr.cDims * 8u);
    (void)m.memory().read_bytes(src + hdr.bounds_off, bounds.data(), bounds.size());
    (void)m.memory().write_bytes(dst + hdr.bounds_off, bounds.data(), bounds.size());

    for (u64 i = 0; i < cells; ++i) {
        const GuestAddr sc = hdr.pvData + i * hdr.cbElements;
        const GuestAddr dc = data + i * hdr.cbElements;
        if ((hdr.fFeatures & FADF_BSTR) != 0) {
            const GuestAddr s = static_cast<GuestAddr>(m.read_ptr(sc).value_or(0));
            GuestAddr c = 0;
            if (s != 0) {
                c = bstr_alloc_bytes(m, s, m.read_u32(s - 4).value_or(0));
                if (c == 0) { (void)sa_destroy(m, dst); return OA_E_OUTOFMEMORY; }
            }
            (void)m.write_ptr(dc, c);
        } else if ((hdr.fFeatures & (FADF_UNKNOWN | FADF_DISPATCH)) != 0) {
            const GuestAddr o = static_cast<GuestAddr>(m.read_ptr(sc).value_or(0));
            com_add_ref(m, o);
            (void)m.write_ptr(dc, o);
        } else if ((hdr.fFeatures & FADF_VARIANT) != 0) {
            const u32 hr = variant_copy_at(m, dc, sc);
            if (hr != S_OK) { (void)sa_destroy(m, dst); return hr; }
        } else if (hdr.cbElements != 0) {
            std::vector<u8> buf(hdr.cbElements);
            (void)m.memory().read_bytes(sc, buf.data(), buf.size());
            (void)m.memory().write_bytes(dc, buf.data(), buf.size());
        }
    }
    out = dst;
    return S_OK;
}

u32 OleAut32Manager::safe_array_copy(Marshaler& m) {
    const GuestAddr src = m.arg_ptr(0);
    const GuestAddr ppsa_out = m.arg_ptr(1);
    if (ppsa_out == 0) return OA_E_INVALIDARG;
    GuestAddr copy = 0;
    const u32 hr = sa_copy(m, src, copy);
    (void)m.write_ptr(ppsa_out, copy);
    return hr;
}

u32 OleAut32Manager::safe_array_redim(Marshaler& m) {
    // HRESULT SafeArrayRedim(SAFEARRAY* psa, SAFEARRAYBOUND* psaboundNew)
    // Changes the caller's LAST dimension (stored as psa->rgsabound[0]); the
    // data keeps its position, so growth appends zeroed cells and shrinking
    // releases the dropped ones (probe redim2d). Locked or FADF_FIXEDSIZE
    // arrays give DISP_E_ARRAYISLOCKED (probe redim_locked / redim_fixed).
    const GuestAddr psa   = m.arg_ptr(0);
    const GuestAddr bound = m.arg_ptr(1);
    SaHeader hdr{};
    if (psa == 0 || bound == 0 || !read_sa_header(m, psa, hdr) || hdr.cDims == 0) {
        return OA_E_INVALIDARG;
    }
    if (hdr.cLocks != 0 || (hdr.fFeatures & FADF_FIXEDSIZE) != 0) {
        return DISP_E_ARRAYISLOCKED;
    }
    const u32 new_c = m.read_u32(bound).value_or(0);
    const u32 new_l = m.read_u32(bound + 4).value_or(0);
    const GuestAddr slow = psa + hdr.bounds_off; // caller's last dimension
    const u32 old_c = m.read_u32(slow).value_or(0);

    u64 inner = 1; // cells per step of the last dimension
    for (u32 i = 1; i < hdr.cDims; ++i) {
        inner *= m.read_u32(psa + hdr.bounds_off + i * 8u).value_or(0);
    }
    const u64 old_cells = inner * old_c;
    const u64 new_cells = inner * new_c;
    if (new_cells * hdr.cbElements > 0xFFFFFFFFull) return OA_E_OUTOFMEMORY;

    if (new_cells != old_cells) {
        if (new_cells < old_cells) {
            sa_clear_cells(*this, m, hdr.fFeatures, hdr.cbElements,
                           hdr.pvData + new_cells * hdr.cbElements,
                           old_cells - new_cells);
        }
        const GuestAddr neu = oa_alloc(m, new_cells * hdr.cbElements);
        if (neu == 0) return OA_E_OUTOFMEMORY;
        const u64 keep = std::min(old_cells, new_cells) * hdr.cbElements;
        if (keep != 0 && hdr.pvData != 0) {
            std::vector<Byte> buf(static_cast<usize>(keep));
            (void)m.memory().read_bytes(hdr.pvData, buf.data(), buf.size());
            (void)m.memory().write_bytes(neu, buf.data(), buf.size());
        }
        if ((hdr.fFeatures & (FADF_STATIC | FADF_EMBEDDED | FADF_CREATEVECTOR)) == 0) {
            oa_free(hdr.pvData);
        }
        (void)m.write_ptr(psa + oleaut_sa_pvdata_off(m.cpu_is_64bit()), neu);
        if ((hdr.fFeatures & FADF_CREATEVECTOR) != 0) {
            // The data no longer lives inside the vector block.
            (void)m.write_u16(psa + 2,
                static_cast<u16>(hdr.fFeatures & ~FADF_CREATEVECTOR));
        }
    }
    (void)m.write_u32(slow, new_c);
    (void)m.write_u32(slow + 4, new_l);
    return S_OK;
}

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

void oleaut32::impl_SafeArrayCreate(Marshaler& m, OleAut32Manager& mgr) {
    m.set_return(mgr.safe_array_create(m));
}

void oleaut32::impl_SafeArrayDestroy(Marshaler& m, OleAut32Manager& mgr) {
    u32 rc = mgr.safe_array_destroy(m);
    m.set_return(rc);
}

// #411 SAFEARRAY* SafeArrayCreateVector(VARTYPE, LONG lLbound, ULONG cElements)
void oleaut32::impl_SafeArrayCreateVector(Marshaler& m, OleAut32Manager& mgr) {
    m.set_return(mgr.sa_create_vector(m, static_cast<u16>(m.arg(0)),
                                      static_cast<i32>(static_cast<u32>(m.arg(1))),
                                      static_cast<u32>(m.arg(2))));
}

} // namespace vitreo::winapi
