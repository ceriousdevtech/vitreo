#pragma once
/// @file oleaut32_safearray_internal.hpp
/// @brief SAFEARRAY descriptor constants and accessors shared by
///        oleaut32_safearray.cpp and oleaut32_safearray_access.cpp.
///
/// Private to the oleaut32 implementation. Everything sits in one anonymous
/// namespace, exactly the scope and linkage these names had inside
/// oleaut32.cpp, so no symbol is exported and none can clash with
/// oleaut32.hpp or typelib.hpp. The accessors are inline so the per-element
/// get/put paths still inline them.

#include "winapi/com/oleaut32/oleaut32_internal.hpp"
#include "winapi/core/marshaler.hpp"

#include <vitreo/common.hpp>

namespace vitreo::winapi {

namespace {

constexpr u32 DISP_E_BADINDEX       = 0x8002000Bu;
constexpr u32 DISP_E_ARRAYISLOCKED  = 0x8002000Du;
constexpr u32 DISP_E_BADVARTYPE     = 0x80020008u;
constexpr u32 DISP_E_OVERFLOW_HR    = 0x8002000Au;
constexpr u32 OA_E_INVALIDARG       = 0x80070057u;
constexpr u32 OA_E_OUTOFMEMORY      = 0x8007000Eu;
constexpr u32 OA_E_UNEXPECTED       = 0x8000FFFFu;

constexpr u16 FADF_AUTO        = 0x0001;
constexpr u16 FADF_STATIC      = 0x0002;
constexpr u16 FADF_EMBEDDED    = 0x0004;
constexpr u16 FADF_FIXEDSIZE   = 0x0010;
constexpr u16 FADF_RECORD      = 0x0020;
constexpr u16 FADF_HAVEIID     = 0x0040;
constexpr u16 FADF_HAVEVARTYPE = 0x0080;
constexpr u16 FADF_BSTR        = 0x0100;
constexpr u16 FADF_UNKNOWN     = 0x0200;
constexpr u16 FADF_DISPATCH    = 0x0400;
constexpr u16 FADF_VARIANT     = 0x0800;
constexpr u16 FADF_CREATEVECTOR = 0x2000;

/// Bytes reserved in front of every descriptor for the hidden IID / VARTYPE.
constexpr u32 kSaHiddenSize = 16;

struct SaHeader {
    u16 cDims{};
    u16 fFeatures{};
    u32 cbElements{};
    u32 cLocks{};
    GuestAddr pvData{};
    u32 bounds_off{};
};

[[nodiscard]] inline bool read_sa_header(Marshaler& m, GuestAddr psa, SaHeader& out) {
    if (psa == 0) return false;
    const bool is64 = m.cpu_is_64bit();
    auto c = m.read_u16(psa + 0);
    if (c.is_err()) return false;
    out.cDims      = c.value();
    out.fFeatures  = m.read_u16(psa + 2).value_or(0);
    out.cbElements = m.read_u32(psa + 4).value_or(0);
    out.cLocks     = m.read_u32(psa + 8).value_or(0);
    out.bounds_off = oleaut_sa_bounds_off(is64);
    out.pvData     = static_cast<GuestAddr>(
        m.read_ptr(psa + oleaut_sa_pvdata_off(is64)).value_or(0));
    return true;
}

/// Stored bound for caller-order dimension `i` (0-based).
[[nodiscard]] inline GuestAddr sa_bound_addr(GuestAddr psa, const SaHeader& hdr,
                                      u32 caller_dim) noexcept {
    return psa + hdr.bounds_off +
           static_cast<GuestAddr>(hdr.cDims - 1u - caller_dim) * 8u;
}

[[nodiscard]] inline u64 sa_total_cells(Marshaler& m, GuestAddr psa,
                                 const SaHeader& hdr) {
    u64 cells = 1;
    for (u32 i = 0; i < hdr.cDims; ++i) {
        cells *= m.read_u32(psa + hdr.bounds_off + i * 8u).value_or(0);
        if (cells == 0) return 0;
    }
    return cells;
}

/// Resolve rgIndices[] to a byte offset within pvData (rgIndices[0] fastest).
[[nodiscard]] inline bool sa_element_offset(Marshaler& m, GuestAddr psa,
                                     const SaHeader& hdr, GuestAddr indices,
                                     u64& out_offset) {
    u64 cell = 0;
    u64 stride = 1;
    for (u32 i = 0; i < hdr.cDims; ++i) {
        const i64 idx = static_cast<i32>(m.read_u32(indices + i * 4).value_or(0));
        const GuestAddr bound = sa_bound_addr(psa, hdr, i);
        const u32 c_elements = m.read_u32(bound).value_or(0);
        const i64 lbound = static_cast<i32>(m.read_u32(bound + 4).value_or(0));
        if (idx < lbound || idx >= lbound + static_cast<i64>(c_elements)) {
            return false;
        }
        cell += static_cast<u64>(idx - lbound) * stride;
        stride *= c_elements;
    }
    out_offset = cell * hdr.cbElements;
    return true;
}

} // namespace

} // namespace vitreo::winapi
