// SPDX-License-Identifier: LGPL-2.1-only
/// @file oleaut32_variant_coerce.cpp
/// @brief OLEAUT32 VARIANT coercion: VariantChangeType(Ex) and DispGetParam.
///
/// Split from oleaut32.cpp by responsibility.

#include "oleaut32.hpp"
#include "winapi/com/oleaut32/oleaut32_internal2.hpp"
#include "winapi/com/oleaut32/oleaut32_internal.hpp"
#include "winapi/com/oleaut32/oleaut32_shared_internal.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/core/marshaler.hpp"
#include <array>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <mutex>

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

namespace {

// VARTYPEs (wtypes.h) the numeric coercion core handles.
constexpr u16 kVtCy = 6, kVtDate = 7, kVtError = 10,
              kVtI8 = 20, kVtUi8 = 21, kVtInt = 22, kVtUint = 23;
constexpr u32 kDispOverflow   = 0x8002000Au;
constexpr u32 kDispBadVarType = 0x80020008u;

/// A VARIANT value on its way between numeric types: an exact integer
/// (signed, or unsigned above INT64_MAX), a floating value, or a currency
/// (int64 scaled by 10,000).
struct Number {
    enum class Kind { Signed, Unsigned, Real, Currency } kind{Kind::Signed};
    i64 s{0};
    u64 u{0};
    double d{0.0};
};

[[nodiscard]] bool numeric_vt(u16 vt) {
    switch (vt) {
    case VT_I1: case VT_UI1: case VT_I2: case VT_UI2: case VT_I4: case VT_UI4:
    case kVtI8: case kVtUi8: case kVtInt: case kVtUint: case VT_R4: case VT_R8:
    case kVtCy: case kVtDate: case VT_BOOL:
        return true;
    default:
        return false;
    }
}

/// VARTYPEs a VARIANT may hold (wtypes.h "V" entries) — anything else is
/// DISP_E_BADVARTYPE (measured: Ints.badvt, vt 0x7F).
[[nodiscard]] bool variant_vt_valid(u16 vt) {
    const u16 base = static_cast<u16>(vt & 0x0FFF);
    if ((vt & 0x8000) != 0 || (vt & 0x1000) != 0) return false;
    if (base <= 14) return true;                 // EMPTY .. DECIMAL
    if (base >= 16 && base <= 23) return true;   // I1 .. UINT
    return base == 36 || base == 37 || base == 38; // RECORD, INT_PTR, UINT_PTR
}

[[nodiscard]] Number read_number(u16 vt, const u8* payload) {
    Number n;
    auto load = [&](auto value) { std::memcpy(&value, payload, sizeof value); return value; };
    switch (vt) {
    case VT_I1: n.s = load(i8{}); break;
    case VT_UI1: n.s = load(u8{}); break;
    case VT_I2: case VT_BOOL: n.s = load(i16{}); break;
    case VT_UI2: n.s = load(u16{}); break;
    case VT_I4: case kVtInt: n.s = load(i32{}); break;
    case VT_UI4: case kVtUint: n.s = load(u32{}); break;
    case kVtI8: n.s = load(i64{}); break;
    case kVtUi8: {
        const u64 v = load(u64{});
        if (v > static_cast<u64>(INT64_MAX)) { n.kind = Number::Kind::Unsigned; n.u = v; }
        else n.s = static_cast<i64>(v);
        break;
    }
    case VT_R4: n.kind = Number::Kind::Real; n.d = load(float{}); break;
    case VT_R8: case kVtDate: n.kind = Number::Kind::Real; n.d = load(double{}); break;
    case kVtCy: n.kind = Number::Kind::Currency; n.s = load(i64{}); break;
    default: break; // VT_EMPTY: zero
    }
    return n;
}

/// Round half to even (the Automation conversions' rounding).
[[nodiscard]] double round_even(double v) { return std::nearbyint(v); }

[[nodiscard]] double as_real(const Number& n) {
    switch (n.kind) {
    case Number::Kind::Signed: return static_cast<double>(n.s);
    case Number::Kind::Unsigned: return static_cast<double>(n.u);
    case Number::Kind::Real: return n.d;
    case Number::Kind::Currency: return static_cast<double>(n.s) / 10000.0;
    }
    return 0.0;
}

/// Integer value of `n` rounded half-to-even into [lo, hi] (as __int128).
[[nodiscard]] bool as_integer(const Number& n, __int128 lo, __int128 hi, __int128& out) {
    __int128 v = 0;
    switch (n.kind) {
    case Number::Kind::Signed: v = n.s; break;
    case Number::Kind::Unsigned: v = static_cast<__int128>(n.u); break;
    case Number::Kind::Real: {
        if (!std::isfinite(n.d)) return false;
        const double r = round_even(n.d);
        if (r < -1.8446744073709552e19 || r > 1.8446744073709552e19) return false;
        v = static_cast<__int128>(r);
        break;
    }
    case Number::Kind::Currency: {
        i64 q = n.s / 10000;
        const i64 rem = n.s % 10000;
        const i64 mag = rem < 0 ? -rem : rem;
        if (mag > 5000 || (mag == 5000 && (q & 1) != 0)) q += rem < 0 ? -1 : 1;
        v = q;
        break;
    }
    }
    if (v < lo || v > hi) return false;
    out = v;
    return true;
}

/// Convert `n` to `target`, writing the payload into `dst` (16 bytes).
/// Returns S_OK or DISP_E_OVERFLOW.
[[nodiscard]] u32 write_number(const Number& n, u16 target, u8* dst) {
    auto store = [&](auto value) { std::memcpy(dst, &value, sizeof value); return S_OK; };
    __int128 v = 0;
    auto range = [&](__int128 lo, __int128 hi) { return as_integer(n, lo, hi, v); };
    switch (target) {
    case VT_I1: return range(-128, 127) ? store(static_cast<i8>(v)) : kDispOverflow;
    case VT_UI1: return range(0, 255) ? store(static_cast<u8>(v)) : kDispOverflow;
    case VT_I2: return range(-32768, 32767) ? store(static_cast<i16>(v)) : kDispOverflow;
    case VT_UI2: return range(0, 65535) ? store(static_cast<u16>(v)) : kDispOverflow;
    case VT_I4: case kVtInt:
        return range(INT32_MIN, INT32_MAX) ? store(static_cast<i32>(v)) : kDispOverflow;
    case VT_UI4: case kVtUint:
        return range(0, UINT32_MAX) ? store(static_cast<u32>(v)) : kDispOverflow;
    case kVtI8: return range(INT64_MIN, INT64_MAX) ? store(static_cast<i64>(v)) : kDispOverflow;
    case kVtUi8: return range(0, static_cast<__int128>(UINT64_MAX)) ? store(static_cast<u64>(v)) : kDispOverflow;
    case VT_BOOL: {
        bool nonzero = false;
        switch (n.kind) {
        case Number::Kind::Signed: case Number::Kind::Currency: nonzero = n.s != 0; break;
        case Number::Kind::Unsigned: nonzero = true; break;
        case Number::Kind::Real: nonzero = n.d != 0.0; break;
        }
        return store(static_cast<i16>(nonzero ? -1 : 0));
    }
    case VT_R4: {
        const double d = as_real(n);
        if (std::isfinite(d) && std::fabs(d) > 3.4028234663852886e38) return kDispOverflow;
        return store(static_cast<float>(d));
    }
    case VT_R8: return store(as_real(n));
    case kVtDate: {
        // Learn DATE: valid range 1 January 100 .. 31 December 9999.
        const double d = as_real(n);
        if (!(d >= -657434.0 && d < 2958466.0)) return kDispOverflow;
        return store(d);
    }
    case kVtCy: {
        if (n.kind == Number::Kind::Currency) return store(n.s);
        if (n.kind == Number::Kind::Real) {
            if (!std::isfinite(n.d)) return kDispOverflow;
            const double scaled = round_even(n.d * 10000.0);
            if (scaled < -9.2233720368547758e18 || scaled >= 9.2233720368547758e18) return kDispOverflow;
            return store(static_cast<i64>(scaled));
        }
        const __int128 base = n.kind == Number::Kind::Unsigned
            ? static_cast<__int128>(n.u) : static_cast<__int128>(n.s);
        const __int128 scaled = base * 10000;
        if (scaled < INT64_MIN || scaled > INT64_MAX) return kDispOverflow;
        return store(static_cast<i64>(scaled));
    }
    default:
        return DISP_E_TYPEMISMATCH;
    }
}

/// Parse an en-US numeric string the way VarParseNumFromStr reads plain
/// decimal text: optional surrounding blanks, sign, digits, '.', exponent.
/// Returns false when anything else is present.
[[nodiscard]] bool parse_number(std::string text, Number& out) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    usize start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
    text = text.substr(start);
    if (text.empty()) return false;
    usize i = 0;
    if (text[i] == '+' || text[i] == '-') ++i;
    usize digits = 0;
    bool real = false;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) { ++i; ++digits; }
    if (i < text.size() && text[i] == '.') {
        real = true; ++i;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) { ++i; ++digits; }
    }
    if (digits == 0) return false;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        real = true; ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
        usize exp_digits = 0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) { ++i; ++exp_digits; }
        if (exp_digits == 0) return false;
    }
    if (i != text.size()) return false;
    if (!real) {
        errno = 0;
        char* end = nullptr;
        const long long v = std::strtoll(text.c_str(), &end, 10);
        if (errno == 0) { out.kind = Number::Kind::Signed; out.s = v; return true; }
    }
    out.kind = Number::Kind::Real;
    out.d = std::strtod(text.c_str(), nullptr);
    return true;
}

/// VarBstrFromR8-style text: up to 15 significant digits, no trailing
/// zeros (R4: 7 digits).
[[nodiscard]] std::string format_real(double v, int digits) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*G", digits, v);
    return buf;
}

} // namespace

u32 OleAut32Manager::variant_change_type(Marshaler& m) {
    return variant_change_type_ex(m, m.arg_ptr(0), m.arg_ptr(1),
                                  static_cast<u16>(m.arg(2)),
                                  static_cast<u16>(m.arg(3)));
}

u32 OleAut32Manager::variant_change_type_ex(Marshaler& m,
                                            GuestAddr pvarg_dest,
                                            GuestAddr pvar_src,
                                            u16 wFlags,
                                            u16 target_vt) {
    // Ownership (measured, typeinfo_invoke_probe `own` rows, x86 = x64): the
    // result is always the destination's own value -- a same-type BSTR is a
    // new copy, an interface is AddRef'ed, a VT_BYREF source is copied out of
    // its referent (which is never changed). On success the destination's
    // previous content is cleared (in place: the source is); on failure
    // neither is touched. So convert into a fresh temporary first.
    if (pvarg_dest == 0 || pvar_src == 0) return 0x80070057u; // E_INVALIDARG
    const u32 vsz = oleaut_variant_size(m.cpu_is_64bit());
    const GuestAddr result = oa_alloc(m, vsz);
    if (result == 0) return 0x8007000Eu;
    const u32 hr = change_type_core(m, result, pvar_src, wFlags, target_vt);
    if (hr == S_OK) {
        (void)variant_clear_at(m, pvarg_dest);   // in place, this is the source
        std::vector<Byte> bytes(vsz);
        (void)m.memory().read_bytes(result, bytes.data(), vsz);
        (void)m.memory().write_bytes(pvarg_dest, bytes.data(), vsz);
    }
    oa_free(result);
    return hr;
}

u32 OleAut32Manager::change_type_core(Marshaler& m,
                                      GuestAddr pvarg_dest,
                                      GuestAddr pvar_src,
                                      u16 wFlags,
                                      u16 target_vt) {
    const u32 vsz = oleaut_variant_size(m.cpu_is_64bit());
    u8 src_buf[24] = {};
    (void)m.memory().read_bytes(pvar_src, src_buf, vsz);
    u16 src_vt = static_cast<u16>(src_buf[0] | (src_buf[1] << 8));

    // Read source value from offset 8
    auto read_src_i32 = [&]() -> i32 {
        return static_cast<i32>(src_buf[8] | (src_buf[9] << 8) |
               (src_buf[10] << 16) | (src_buf[11] << 24));
    };
    auto read_src_u32 = [&]() -> u32 {
        return src_buf[8] | (src_buf[9] << 8) |
               (src_buf[10] << 16) | (src_buf[11] << 24);
    };
    auto read_src_i16 = [&]() -> i16 {
        return static_cast<i16>(src_buf[8] | (src_buf[9] << 8));
    };
    auto read_src_r8 = [&]() -> double {
        double v;
        std::memcpy(&v, &src_buf[8], 8);
        return v;
    };
    auto read_src_r4 = [&]() -> float {
        float v;
        std::memcpy(&v, &src_buf[8], 4);
        return v;
    };

    VITREO_TRACE("[oleaut32] VariantChangeType dest=0x%x src=0x%x srcVT=%u targetVT=%u\n",
                 (unsigned)pvarg_dest, (unsigned)pvar_src, src_vt, target_vt);

    if (!variant_vt_valid(src_vt) || !variant_vt_valid(target_vt)) {
        return kDispBadVarType;
    }
    // Measured: VT_ARRAY, VT_BYREF and VT_VARIANT targets are refused, same
    // type included; any source but VT_NULL converts to VT_EMPTY.
    if ((target_vt & (0x2000 | VT_BYREF)) != 0 || target_vt == VT_VARIANT) {
        return DISP_E_TYPEMISMATCH;
    }
    if (target_vt == VT_EMPTY) {
        u16 vt = src_vt;
        if (vt == (VT_BYREF | VT_VARIANT)) {
            const GuestAddr inner = static_cast<GuestAddr>(m.read_ptr(pvar_src + 8).value_or(0));
            vt = inner == 0 ? 0 : m.read_u16(inner).value_or(0);
        }
        if ((vt & VT_TYPEMASK) == VT_NULL) return DISP_E_TYPEMISMATCH;
        return S_OK;   // the zeroed result is VT_EMPTY
    }

    // Same type: the destination's own copy (VariantCopy semantics).
    if (src_vt == target_vt) {
        return variant_copy_at(m, pvarg_dest, pvar_src);
    }

    // VT_BYREF|T → dereference, then coerce the pointed-to value.
    // Script engines pass [in,out] longs as VT_BYREF|VT_I4.
    if ((src_vt & VT_BYREF) != 0) {
        const u16 base_vt = static_cast<u16>(src_vt & VT_TYPEMASK);
        const GuestAddr ptr =
            static_cast<GuestAddr>(m.read_ptr(pvar_src + 8).value_or(0));
        if (ptr == 0) {
            VITREO_TRACE("[oleaut32]   -> DISP_E_TYPEMISMATCH (null BYREF)\n");
            return DISP_E_TYPEMISMATCH;
        }
        if (base_vt == VT_VARIANT) {
            return change_type_core(m, pvarg_dest, ptr, wFlags, target_vt);
        }
        // Materialise a non-BYREF view of the referent and convert that; the
        // conversion copies (BSTR) or AddRefs (interfaces) what it keeps.
        struct Temp {
            OleAut32Manager& owner; GuestAddr at;
            ~Temp() { if (at != 0) owner.oa_free(at); }
        } holder{*this, oa_alloc(m, vsz)};
        const GuestAddr tmp = holder.at;
        if (tmp == 0) return 0x8007000Eu; // E_OUTOFMEMORY
        u8 tmp_buf[24]{};
        tmp_buf[0] = static_cast<u8>(base_vt & 0xFF);
        tmp_buf[1] = static_cast<u8>(base_vt >> 8);
        if (base_vt == VT_UI1 || base_vt == VT_I1) {
            // Single-byte types: only the low byte is meaningful.
            tmp_buf[8] = m.read_u8(ptr).value_or(0);
        } else if (base_vt == VT_I2 || base_vt == VT_BOOL || base_vt == VT_UI2) {
            const u16 v = m.read_u16(ptr).value_or(0);
            tmp_buf[8] = static_cast<u8>(v & 0xFF);
            tmp_buf[9] = static_cast<u8>(v >> 8);
        } else if (base_vt == VT_R8 || base_vt == kVtCy || base_vt == kVtDate ||
                   base_vt == kVtI8 || base_vt == kVtUi8) {
            u8 eight[8]{};
            (void)m.memory().read_bytes(ptr, eight, 8);
            std::memcpy(&tmp_buf[8], eight, 8);
        } else if (base_vt == VT_BSTR || base_vt == VT_UNKNOWN ||
                   base_vt == VT_DISPATCH) {
            (void)m.memory().write_bytes(tmp, tmp_buf, vsz);
            (void)m.write_u16(tmp, base_vt);
            (void)m.write_ptr(tmp + 8, static_cast<GuestAddr>(
                m.read_ptr(ptr).value_or(0)));
            return change_type_core(m, pvarg_dest, tmp, wFlags, target_vt);
        } else if (base_vt == VT_I4 || base_vt == VT_UI4 || base_vt == VT_R4 ||
                   base_vt == kVtInt || base_vt == kVtUint || base_vt == kVtError) {
            const u32 v = m.read_u32(ptr).value_or(0);
            tmp_buf[8]  = static_cast<u8>(v);
            tmp_buf[9]  = static_cast<u8>(v >> 8);
            tmp_buf[10] = static_cast<u8>(v >> 16);
            tmp_buf[11] = static_cast<u8>(v >> 24);
        } else {
            VITREO_TRACE("[oleaut32]   -> DISP_E_TYPEMISMATCH (BYREF base=%u)\n",
                         base_vt);
            return DISP_E_TYPEMISMATCH;
        }
        (void)m.memory().write_bytes(tmp, tmp_buf, vsz);
        return change_type_core(m, pvarg_dest, tmp, wFlags, target_vt);
    }


    // Numeric, currency, date and Boolean conversions: one table with the
    // Automation range checks (DISP_E_OVERFLOW) and half-to-even rounding.
    {
        const bool src_numeric = numeric_vt(src_vt) || src_vt == VT_EMPTY;
        if (numeric_vt(target_vt) && (src_numeric || src_vt == VT_BSTR)) {
            Number number = read_number(src_vt, &src_buf[8]);
            if (src_vt == VT_BSTR) {
                GuestAddr bstr = 0;
                std::memcpy(&bstr, &src_buf[8], oleaut_ptr_size(m.cpu_is_64bit()));
                std::string text;
                if (bstr != 0) {
                    const u32 bytes = m.read_u32(bstr - 4).value_or(0);
                    for (u32 i = 0; i + 1 < bytes; i += 2) {
                        const u16 ch = m.read_u16(bstr + i).value_or(0);
                        text.push_back(ch < 0x80 ? static_cast<char>(ch) : '\x7f');
                    }
                }
                std::string lower;
                for (char c : text) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                if (target_vt == VT_BOOL && (lower == "true" || lower == "false")) {
                    number = Number{};
                    number.s = lower == "true" ? -1 : 0;
                } else if (!parse_number(text, number)) {
                    return DISP_E_TYPEMISMATCH;
                }
            }
            u8 out[24] = {};
            out[0] = static_cast<u8>(target_vt);
            out[1] = static_cast<u8>(target_vt >> 8);
            const u32 hr = write_number(number, target_vt, &out[8]);
            if (hr != S_OK) return hr;
            (void)m.memory().write_bytes(pvarg_dest, out, oleaut_variant_size(m.cpu_is_64bit()));
            return S_OK;
        }
        // Measured (typeinfo_invoke_probe `vct` rows, x86 = x64): VT_NULL
        // converts only to VT_NULL; VT_EMPTY to DECIMAL is a zero DECIMAL.
        if (src_vt == VT_NULL && target_vt != VT_NULL) return DISP_E_TYPEMISMATCH;
        if (src_vt == VT_EMPTY && target_vt == 14) {
            u8 out[24] = {};
            out[0] = 14;
            (void)m.memory().write_bytes(pvarg_dest, out, oleaut_variant_size(m.cpu_is_64bit()));
            return S_OK;
        }
        if (target_vt == VT_BSTR && src_vt != VT_BOOL &&
            (numeric_vt(src_vt) || src_vt == VT_EMPTY)) {
            const Number number = read_number(src_vt, &src_buf[8]);
            std::string text;
            if (src_vt == kVtDate) {
                static std::once_flag once;
                std::call_once(once, [] {
                    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED oleaut32!VariantChangeType: "
                                         "VT_DATE to VT_BSTR\n");
                });
                return DISP_E_TYPEMISMATCH;
            }
            if (src_vt == VT_EMPTY) text.clear();
            else if (src_vt == VT_R4) text = format_real(number.d, 7);
            else if (src_vt == VT_R8) text = format_real(number.d, 15);
            else if (src_vt == kVtCy) text = format_real(static_cast<double>(number.s) / 10000.0, 19);
            else if (number.kind == Number::Kind::Unsigned) text = std::to_string(number.u);
            else text = std::to_string(number.s);
            std::vector<Byte> utf16;
            for (char c : text) { utf16.push_back(static_cast<Byte>(c)); utf16.push_back(0); }
            const GuestAddr bstr = allocate_bstr_bytes(m, utf16);
            if (bstr == 0) return 0x8007000Eu;
            u8 out[24] = {};
            out[0] = static_cast<u8>(VT_BSTR);
            std::memcpy(&out[8], &bstr, oleaut_ptr_size(m.cpu_is_64bit()));
            (void)m.memory().write_bytes(pvarg_dest, out, oleaut_variant_size(m.cpu_is_64bit()));
            return S_OK;
        }
        if (src_vt == VT_DISPATCH && target_vt == VT_UNKNOWN) {
            // Measured (typeinfo_invoke_probe Unk.fromdispatch): the object
            // is asked for IID_IUnknown.
            GuestAddr punk = 0;
            std::memcpy(&punk, &src_buf[8], oleaut_ptr_size(m.cpu_is_64bit()));
            GuestAddr unk = 0;
            if (punk != 0) {
                static constexpr std::array<u8, 16> kIidUnknown{
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
                unk = com_query_interface(m, punk, kIidUnknown);
                if (unk == 0) return 0x80004002u; // E_NOINTERFACE
            }
            u8 out[24] = {};
            out[0] = static_cast<u8>(VT_UNKNOWN);
            std::memcpy(&out[8], &unk, oleaut_ptr_size(m.cpu_is_64bit()));
            (void)m.memory().write_bytes(pvarg_dest, out, oleaut_variant_size(m.cpu_is_64bit()));
            return S_OK;
        }
    }

    // Prepare destination: zero it, set target vt
    u8 dst_buf[24] = {};
    dst_buf[0] = static_cast<u8>(target_vt & 0xFF);
    dst_buf[1] = static_cast<u8>(target_vt >> 8);

    auto write_dst_i32 = [&](i32 v) {
        dst_buf[8]  = static_cast<u8>(v);
        dst_buf[9]  = static_cast<u8>(v >> 8);
        dst_buf[10] = static_cast<u8>(v >> 16);
        dst_buf[11] = static_cast<u8>(v >> 24);
    };
    auto write_dst_u32 = [&](u32 v) {
        dst_buf[8]  = static_cast<u8>(v);
        dst_buf[9]  = static_cast<u8>(v >> 8);
        dst_buf[10] = static_cast<u8>(v >> 16);
        dst_buf[11] = static_cast<u8>(v >> 24);
    };
    auto write_dst_r8 = [&](double v) {
        std::memcpy(&dst_buf[8], &v, 8);
    };
    auto write_dst_i16 = [&](i16 v) {
        dst_buf[8] = static_cast<u8>(v & 0xFF);
        dst_buf[9] = static_cast<u8>((v >> 8) & 0xFF);
    };

    // Helper: read BSTR content as UTF-8 from a guest BSTR pointer
    auto read_bstr_utf8 = [&](GuestAddr bstr_addr) -> std::string {
        if (bstr_addr == 0) return {};
        u32 byte_len = m.read_u32(bstr_addr - 4).value_or(0);
        if (byte_len == 0) return {};
        std::string result;
        for (u32 i = 0; i < byte_len; i += 2) {
            u16 ch = m.read_u16(bstr_addr + i).value_or(0);
            if (ch < 0x80) result += static_cast<char>(ch);
        }
        return result;
    };

    // Helper: allocate BSTR from a narrow string
    auto alloc_bstr_from_str = [&](const std::string& s) -> GuestAddr {
        std::vector<Byte> utf16(s.size() * 2);
        for (usize i = 0; i < s.size(); ++i) {
            utf16[i * 2] = static_cast<Byte>(static_cast<u8>(s[i]));
        }
        return allocate_bstr_bytes(m, utf16);
    };
    // Pointer payloads (BSTR, interface pointers) are pointer-sized.
    auto write_dst_ptr = [&](GuestAddr v) {
        const u32 n = oleaut_ptr_size(m.cpu_is_64bit());
        for (u32 i = 0; i < n; ++i) {
            dst_buf[8 + i] = static_cast<u8>(v >> (8 * i));
        }
    };

    bool ok = false;

    switch (src_vt) {
    case VT_BSTR: {
        GuestAddr bstr_addr = 0;
        std::memcpy(&bstr_addr, &src_buf[8], oleaut_ptr_size(m.cpu_is_64bit()));
        std::string str = read_bstr_utf8(bstr_addr);
        if (target_vt == VT_I4 || target_vt == VT_UI4) {
            char* end = nullptr;
            long val = std::strtol(str.c_str(), &end, 10);
            write_dst_i32(static_cast<i32>(val));
            ok = true;
        } else if (target_vt == VT_R8) {
            char* end = nullptr;
            double val = std::strtod(str.c_str(), &end);
            write_dst_r8(val);
            ok = true;
        } else if (target_vt == VT_BOOL) {
            char* end = nullptr;
            long val = std::strtol(str.c_str(), &end, 10);
            write_dst_i16(val != 0 ? static_cast<i16>(-1) : static_cast<i16>(0));
            ok = true;
        }
        break;
    }
    case VT_I4:
    case VT_UI4: {
        i32 val = read_src_i32();
        if (target_vt == VT_R8) {
            write_dst_r8(static_cast<double>(val));
            ok = true;
        } else if (target_vt == VT_R4) {
            float f = static_cast<float>(val);
            std::memcpy(&dst_buf[8], &f, 4);
            ok = true;
        } else if (target_vt == VT_BSTR) {
            std::string s = std::to_string(val);
            const GuestAddr bstr = alloc_bstr_from_str(s);
            write_dst_ptr(bstr);
            ok = (bstr != 0);
        } else if (target_vt == VT_BOOL) {
            write_dst_i16(val != 0 ? static_cast<i16>(-1) : static_cast<i16>(0));
            ok = true;
        } else if (target_vt == VT_I2) {
            write_dst_i16(static_cast<i16>(val));
            ok = true;
        }
        break;
    }
    case VT_R8: {
        double val = read_src_r8();
        if (target_vt == VT_I4 || target_vt == VT_UI4) {
            write_dst_i32(static_cast<i32>(val));
            ok = true;
        } else if (target_vt == VT_BSTR) {
            std::string s = std::to_string(val);
            const GuestAddr bstr = alloc_bstr_from_str(s);
            write_dst_ptr(bstr);
            ok = (bstr != 0);
        } else if (target_vt == VT_BOOL) {
            write_dst_i16(val != 0.0 ? static_cast<i16>(-1) : static_cast<i16>(0));
            ok = true;
        }
        break;
    }
    case VT_R4: {
        float val = read_src_r4();
        if (target_vt == VT_R8) {
            write_dst_r8(static_cast<double>(val));
            ok = true;
        } else if (target_vt == VT_I4) {
            write_dst_i32(static_cast<i32>(val));
            ok = true;
        }
        break;
    }
    case VT_BOOL: {
        i16 val = read_src_i16();
        if (target_vt == VT_I2) {
            // VARIANT_BOOL is already an i16; script engines (ISRT) coerce
            // BOOL→I2 when reading typed locals.
            write_dst_i16(val);
            ok = true;
        } else if (target_vt == VT_I4 || target_vt == VT_UI4) {
            write_dst_i32(val != 0 ? 1 : 0);
            ok = true;
        } else if (target_vt == VT_R8) {
            write_dst_r8(val != 0 ? 1.0 : 0.0);
            ok = true;
        } else if (target_vt == VT_BSTR) {
            // Check VARIANT_ALPHABOOL (0x02) in wFlags
            std::string s;
            if (wFlags & 0x02) {
                // VARIANT_ALPHABOOL: use "True"/"False"
                s = val != 0 ? "True" : "False";
            } else {
                // Default OLE convention: "#TRUE#"/"#FALSE#"
                s = val != 0 ? "#TRUE#" : "#FALSE#";
            }
            const GuestAddr bstr = alloc_bstr_from_str(s);
            write_dst_ptr(bstr);
            ok = (bstr != 0);
        }
        break;
    }
    case VT_I2: {
        i16 val = read_src_i16();
        if (target_vt == VT_I4 || target_vt == VT_UI4) {
            write_dst_i32(static_cast<i32>(val));
            ok = true;
        } else if (target_vt == VT_R8) {
            write_dst_r8(static_cast<double>(val));
            ok = true;
        } else if (target_vt == VT_BOOL) {
            write_dst_i16(val != 0 ? static_cast<i16>(-1) : static_cast<i16>(0));
            ok = true;
        }
        break;
    }
    case VT_UI1: {
        // Unsigned byte — always fits in I2/I4/UI4/R*/BOOL/BSTR.
        // Script engines coerce typed locals UI1→I4.
        const u8 val = src_buf[8];
        if (target_vt == VT_I4 || target_vt == VT_UI4) {
            write_dst_i32(static_cast<i32>(val));
            ok = true;
        } else if (target_vt == VT_I2 || target_vt == VT_UI2) {
            write_dst_i16(static_cast<i16>(val));
            ok = true;
        } else if (target_vt == VT_R8) {
            write_dst_r8(static_cast<double>(val));
            ok = true;
        } else if (target_vt == VT_R4) {
            float f = static_cast<float>(val);
            std::memcpy(&dst_buf[8], &f, 4);
            ok = true;
        } else if (target_vt == VT_BOOL) {
            write_dst_i16(val != 0 ? static_cast<i16>(-1) : static_cast<i16>(0));
            ok = true;
        } else if (target_vt == VT_BSTR) {
            const GuestAddr bstr = alloc_bstr_from_str(std::to_string(val));
            write_dst_ptr(bstr);
            ok = (bstr != 0);
        } else if (target_vt == VT_I1) {
            dst_buf[8] = val;
            ok = true;
        }
        break;
    }
    case VT_I1: {
        const i8 val = static_cast<i8>(src_buf[8]);
        if (target_vt == VT_I4 || target_vt == VT_UI4) {
            write_dst_i32(static_cast<i32>(val));
            ok = true;
        } else if (target_vt == VT_I2) {
            write_dst_i16(static_cast<i16>(val));
            ok = true;
        } else if (target_vt == VT_UI1) {
            dst_buf[8] = static_cast<u8>(val);
            ok = true;
        } else if (target_vt == VT_R8) {
            write_dst_r8(static_cast<double>(val));
            ok = true;
        } else if (target_vt == VT_BOOL) {
            write_dst_i16(val != 0 ? static_cast<i16>(-1) : static_cast<i16>(0));
            ok = true;
        } else if (target_vt == VT_BSTR) {
            const GuestAddr bstr = alloc_bstr_from_str(std::to_string(val));
            write_dst_ptr(bstr);
            ok = (bstr != 0);
        }
        break;
    }
    case VT_UI2: {
        const u16 val = static_cast<u16>(read_src_i16());
        if (target_vt == VT_I4 || target_vt == VT_UI4) {
            write_dst_i32(static_cast<i32>(val));
            ok = true;
        } else if (target_vt == VT_I2) {
            write_dst_i16(static_cast<i16>(val));
            ok = true;
        } else if (target_vt == VT_R8) {
            write_dst_r8(static_cast<double>(val));
            ok = true;
        } else if (target_vt == VT_BOOL) {
            write_dst_i16(val != 0 ? static_cast<i16>(-1) : static_cast<i16>(0));
            ok = true;
        } else if (target_vt == VT_BSTR) {
            const GuestAddr bstr = alloc_bstr_from_str(std::to_string(val));
            write_dst_ptr(bstr);
            ok = (bstr != 0);
        }
        break;
    }
    case VT_EMPTY:
    case VT_NULL:
        // EMPTY/NULL conversion to scalar Automation values.
        // Script engines coerce EMPTY→NULL after reading a substitution
        // value. Rejecting it surfaces as DISP_E_TYPEMISMATCH (0x80020005).
        if (target_vt == VT_EMPTY || target_vt == VT_NULL) {
            ok = true;
        } else if (target_vt == VT_I4 || target_vt == VT_UI4) {
            write_dst_i32(0);
            ok = true;
        } else if (target_vt == VT_R8) {
            write_dst_r8(0.0);
            ok = true;
        } else if (target_vt == VT_BSTR) {
            const GuestAddr bstr = alloc_bstr_from_str("");
            write_dst_ptr(bstr);
            ok = (bstr != 0);
        } else if (target_vt == VT_BOOL) {
            write_dst_i16(0);
            ok = true;
        }
        break;
    case VT_DISPATCH:
    case VT_UNKNOWN: {
        // Measured (`own` rows): a non-interface target from VT_DISPATCH
        // reads the object's DISPID_VALUE (IDispatch::Invoke, PROPERTYGET,
        // no arguments) and converts that; a NULL VT_DISPATCH and any
        // VT_UNKNOWN are DISP_E_TYPEMISMATCH.
        GuestAddr punk = 0;
        std::memcpy(&punk, &src_buf[8], oleaut_ptr_size(m.cpu_is_64bit()));
        if (target_vt != VT_DISPATCH && target_vt != VT_UNKNOWN) {
            if (src_vt != VT_DISPATCH || punk == 0 || !m_guest_call) break;
            const u32 ps = oleaut_ptr_size(m.cpu_is_64bit());
            const GuestAddr block = oa_alloc(m, 16 + 16 + vsz);
            if (block == 0) return 0x8007000Eu;
            const GuestAddr iid_null = block, params = block + 16, value = block + 32;
            const GuestAddr vtbl = static_cast<GuestAddr>(m.read_ptr(punk).value_or(0));
            const GuestAddr invoke = vtbl == 0 ? 0 : static_cast<GuestAddr>(
                m.read_ptr(vtbl + 6 * ps).value_or(0));
            u32 hr = DISP_E_TYPEMISMATCH;
            if (invoke != 0) {
                const u64 args[9] = {punk, 0 /*DISPID_VALUE*/, iid_null, 0 /*lcid*/,
                                     2 /*DISPATCH_PROPERTYGET*/, params, value, 0, 0};
                hr = static_cast<u32>(m_guest_call(invoke, args, 9, m.cpu_is_64bit()));
                if (hr == S_OK) hr = change_type_core(m, pvarg_dest, value, wFlags, target_vt);
                (void)variant_clear_at(m, value);
            }
            oa_free(block);
            return hr;
        }
        if (src_vt == VT_DISPATCH && target_vt == VT_UNKNOWN) {
            write_dst_ptr(punk);
            ok = true;
            if (punk != 0 && pvarg_dest != pvar_src && m_guest_call) {
                auto vtable_r = m.read_ptr(static_cast<GuestAddr>(punk));
                if (vtable_r.is_ok() && vtable_r.value() != 0) {
                    auto addref_r = m.read_ptr(
                        static_cast<GuestAddr>(vtable_r.value()) +
                        oleaut_ptr_size(m.cpu_is_64bit()));
                    if (addref_r.is_ok() && addref_r.value() != 0) {
                        const u64 a[1] = {static_cast<u64>(punk)};
                        (void)m_guest_call(
                            static_cast<GuestAddr>(addref_r.value()), a, 1,
                            m.cpu_is_64bit());
                    }
                }
            }
        } else if (src_vt == VT_UNKNOWN && target_vt == VT_DISPATCH) {
            if (punk == 0) {
                write_dst_ptr(0);
                ok = true;
            } else if (m_guest_call) {
                // QI for IID_IDispatch ({00020400-0000-0000-C000-000000000046}).
                constexpr std::array<u8, 16> iid_dispatch{
                    0x00, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
                const GuestAddr iid = scratch_alloc(16);
                const GuestAddr out = scratch_alloc(8);
                if (iid != 0 && out != 0 &&
                    m.memory()
                        .write_bytes(iid, iid_dispatch.data(), 16)
                        .is_ok()) {
                    (void)m.write_ptr(out, 0);
                    auto vtable_r = m.read_ptr(static_cast<GuestAddr>(punk));
                    if (vtable_r.is_ok() && vtable_r.value() != 0) {
                        auto qi_r = m.read_ptr(
                            static_cast<GuestAddr>(vtable_r.value()));
                        if (qi_r.is_ok() && qi_r.value() != 0) {
                            const u64 a[3] = {static_cast<u64>(punk),
                                              static_cast<u64>(iid),
                                              static_cast<u64>(out)};
                            const u32 hr = static_cast<u32>(m_guest_call(
                                static_cast<GuestAddr>(qi_r.value()), a, 3,
                                m.cpu_is_64bit()));
                            if (hr == S_OK) {
                                write_dst_ptr(static_cast<GuestAddr>(
                                    m.read_ptr(out).value_or(0)));
                                ok = true;
                            } else {
                                // Measured (typeinfo_invoke_probe
                                // InDual.wrongunk): E_NOINTERFACE.
                                return 0x80004002u;
                            }
                        }
                    }
                }
            }
        }
        break;
    }
    default:
        break;
    }

    if (!ok) {
        VITREO_TRACE("[oleaut32]   -> DISP_E_TYPEMISMATCH (srcVT=%u -> targetVT=%u)\n",
                     src_vt, target_vt);
        return DISP_E_TYPEMISMATCH;
    }

    (void)m.memory().write_bytes(pvarg_dest, dst_buf, vsz);
    VITREO_TRACE("[oleaut32]   -> S_OK\n");
    return S_OK;
}

u32 OleAut32Manager::disp_get_param(Marshaler& m) {
    // HRESULT DispGetParam(DISPPARAMS*, UINT position, VARTYPE vtTarg,
    //                      VARIANT* pvarResult, UINT* puArgErr)
    constexpr u32 DISP_E_PARAMNOTFOUND = 0x80020004u;
    constexpr u32 E_INVALIDARG_HR      = 0x80070057u;

    const GuestAddr pdispparams = m.arg_ptr(0);
    const u32       position    = static_cast<u32>(m.arg(1));
    const u16       vt_targ     = static_cast<u16>(m.arg(2));
    const GuestAddr pvar_result = m.arg_ptr(3);
    const GuestAddr pu_arg_err  = m.arg_ptr(4);

    auto fail = [&](u32 hr, u32 arg_index) -> u32 {
        if (pu_arg_err != 0) (void)m.write_u32(pu_arg_err, arg_index);
        VITREO_TRACE("[oleaut32] DispGetParam pos=%u vt=%u -> 0x%08x\n",
                     position, vt_targ, hr);
        return hr;
    };

    if (pdispparams == 0 || pvar_result == 0) {
        return fail(E_INVALIDARG_HR, 0);
    }

    // DISPPARAMS: rgvarg*, rgdispidNamedArgs*, cArgs, cNamedArgs.
    // Pointers are 4 bytes on x86 and 8 on x64; UINT fields stay 4 bytes.
    const u32 ptr_size = oleaut_ptr_size(m.cpu_is_64bit());
    const u32 variant_size = oleaut_variant_size(m.cpu_is_64bit());
    const GuestAddr rgvarg =
        static_cast<GuestAddr>(m.read_ptr(pdispparams).value_or(0));
    const GuestAddr rgdispid = static_cast<GuestAddr>(
        m.read_ptr(pdispparams + ptr_size).value_or(0));
    const u32 c_args =
        static_cast<u32>(m.read_u32(pdispparams + 2u * ptr_size).value_or(0));
    const u32 c_named =
        static_cast<u32>(m.read_u32(pdispparams + 2u * ptr_size + 4).value_or(0));

    u32 pos = 0;
    if (position < c_args) {
        pos = c_args - position - 1;
    } else {
        // Named-arg lookup: DISPID == position.
        bool found = false;
        for (u32 i = 0; i < c_named; ++i) {
            if (rgdispid == 0) break;
            const i32 id =
                static_cast<i32>(m.read_u32(rgdispid + i * 4).value_or(0));
            if (static_cast<u32>(id) == position) {
                pos = i;
                found = true;
                break;
            }
        }
        if (!found) return fail(DISP_E_PARAMNOTFOUND, position);
    }

    if (c_args > 0 && rgvarg == 0) return fail(E_INVALIDARG_HR, pos);

    const GuestAddr src = rgvarg + static_cast<GuestAddr>(pos) * variant_size;

    // Coercion re-enters variant_change_type_ex(dest, src, flags=0, vtTarg) directly with
    // explicit guest addresses. Known gap: its VT_BOOL->VT_BSTR path reads wFlags from
    // m.arg(2) instead of its (unnamed) wFlags parameter, so under DispGetParam it sees
    // vtTarg there. Fixing that changes behaviour, so it is left as a separate follow-up.

    // Clear destination first (VariantInit).
    {
        u8 zeros[24]{};
        (void)m.memory().write_bytes(pvar_result, zeros, variant_size);
    }

    const u16 src_vt =
        static_cast<u16>(m.read_u16(src).value_or(0));
    if (src_vt == vt_targ) {
        u8 buf[24]{};
        (void)m.memory().read_bytes(src, buf, variant_size);
        (void)m.memory().write_bytes(pvar_result, buf, variant_size);
        VITREO_TRACE("[oleaut32] DispGetParam pos=%u vt=%u (same) -> S_OK\n",
                     position, vt_targ);
        return S_OK;
    }

    u32 hr = variant_change_type_ex(m, pvar_result, src, 0, vt_targ);
    if (hr != S_OK) {
        if (pu_arg_err != 0) (void)m.write_u32(pu_arg_err, pos);
        VITREO_TRACE("[oleaut32] DispGetParam pos=%u srcVT=%u -> vt=%u "
                     "hr=0x%08x\n",
                     position, src_vt, vt_targ, hr);
        return hr;
    }
    VITREO_TRACE("[oleaut32] DispGetParam pos=%u srcVT=%u -> vt=%u S_OK\n",
                 position, src_vt, vt_targ);
    return S_OK;
}

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

void oleaut32::impl_VariantChangeType(Marshaler& m, OleAut32Manager& mgr) {
    u32 rc = mgr.variant_change_type(m);
    m.set_return(rc);
}

} // namespace vitreo::winapi
