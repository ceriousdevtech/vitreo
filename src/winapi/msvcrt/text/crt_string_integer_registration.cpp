// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_string_integer_registration.cpp
/// @brief strtol / strtoul / wcstol ... and the ato* / _wto* wrappers, plus
///        wcstombs, which the export table registers directly after them.
///
/// Split out of crt_string_registration.cpp. The UCRT-measured parser the whole
/// family shares lives in this file's anonymous namespace.

#include "winapi/msvcrt/text/crt_string_registration_internal.hpp"
#include "winapi/msvcrt/text/crt_wctype_internal.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include <functional>

namespace vitreo::winapi {

namespace {

// ---------------------------------------------------------------------------
// The strtol family, as the UCRT implements it (09_crt.md HIGH 11/12).
//
// One parser for strtol/strtoul/wcstol/wcstoul/strto(u)ll/_strto(u)i64/
// _wcsto(u)i64 and the ato*/_wto* wrappers. Behaviour measured by
// tools/oracle_probe/crt_ucrt_str_probe.c against ucrtbase:
//   * *endptr is a full pointer, written BEFORE parameter validation, and
//     points at the string start when no digits were converted;
//   * NULL string / base not in {0, 2..36} -> invalid-parameter handler,
//     errno EINVAL, result 0;
//   * overflow saturates (LONG_MAX/LONG_MIN/ULONG_MAX...) with ERANGE; the
//     unsigned forms NEGATE a "-" input modulo 2^N without ERANGE;
//   * the string is walked in guest memory with no length cap, and wide
//     strings are classified per UTF-16 unit: Unicode decimal digits
//     (fullwidth, Arabic-Indic, Devanagari, ...) are digits, other non-ASCII
//     units end the number (never folded onto a byte).
// ---------------------------------------------------------------------------
enum class CrtIntKind { I32, U32, I64, U64 };

/// Decimal value of a UTF-16 digit per the UCRT's wide digit table, or -1.
[[nodiscard]] int crt_wide_digit(u32 c) noexcept {
    static constexpr u32 kZeros[] = {
        0x0030, 0x0660, 0x06F0, 0x0966, 0x09E6, 0x0A66, 0x0AE6, 0x0B66,
        0x0C66, 0x0CE6, 0x0D66, 0x0E50, 0x0ED0, 0x0F20, 0x1040, 0x17E0,
        0x1810, 0xFF10};
    for (u32 z : kZeros) {
        if (c >= z && c <= z + 9) return static_cast<int>(c - z);
    }
    return -1;
}

struct CrtIntResult {
    u64 bits = 0;          ///< value to return (already saturated/negated)
    bool range_error = false;
    usize consumed = 0;    ///< code units consumed (0 if nothing converted)
};

[[nodiscard]] CrtIntResult crt_parse_integer_units(
    const std::function<i32(usize)>& unit_at, bool wide, int base,
    CrtIntKind kind) {
    CrtIntResult out;
    usize i = 0;
    auto unit = [&](usize k) -> i32 { return unit_at(k); };
    auto is_space = [&](i32 c) {
        if (c < 0) return false;
        if (!wide) return c == ' ' || (c >= 0x09 && c <= 0x0D);
        return (win_wctype_bits(static_cast<u32>(c)) & 0x08u) != 0;
    };
    auto digit_of = [&](i32 c) -> int {
        if (c < 0) return -1;
        int d = wide ? crt_wide_digit(static_cast<u32>(c))
                     : ((c >= '0' && c <= '9') ? c - '0' : -1);
        if (d < 0) {
            if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        }
        return d;
    };
    while (is_space(unit(i))) ++i;
    bool negative = false;
    if (unit(i) == '-' || unit(i) == '+') {
        negative = unit(i) == '-';
        ++i;
    }
    if ((base == 0 || base == 16) && digit_of(unit(i)) == 0 &&
        (unit(i + 1) == 'x' || unit(i + 1) == 'X')) {
        if (digit_of(unit(i + 2)) >= 0 && digit_of(unit(i + 2)) < 16) {
            i += 2;
            base = 16;
        } else {
            // "0x" with no hex digit: the subject sequence is just "0".
            out.consumed = i + 1;
            return out;
        }
    } else if (base == 0) {
        base = digit_of(unit(i)) == 0 ? 8 : 10;
    }
    const bool is_signed = kind == CrtIntKind::I32 || kind == CrtIntKind::I64;
    const bool is64 = kind == CrtIntKind::I64 || kind == CrtIntKind::U64;
    const u64 umax = is64 ? ~u64{0} : u64{0xFFFFFFFFu};
    const u64 pos_limit = is_signed ? (umax >> 1) : umax;
    const u64 neg_limit = is_signed ? (umax >> 1) + 1 : umax;
    const u64 limit = negative ? neg_limit : pos_limit;
    u64 magnitude = 0;
    bool overflow = false;
    usize start = i;
    for (;; ++i) {
        const int d = digit_of(unit(i));
        if (d < 0 || d >= base) break;
        if (!overflow) {
            if (magnitude > (limit - static_cast<u64>(d)) / static_cast<u64>(base)) {
                overflow = true;
            } else {
                magnitude = magnitude * static_cast<u64>(base) + static_cast<u64>(d);
            }
        }
    }
    if (i == start) return out;  // nothing converted: endptr = string start
    out.consumed = i;
    if (overflow) {
        out.range_error = true;
        if (is_signed) out.bits = negative ? (0 - neg_limit) & umax : pos_limit;
        else out.bits = umax;
        return out;
    }
    out.bits = (negative ? (0 - magnitude) : magnitude) & umax;
    return out;
}

/// Parse the guest string at `str` (wide or narrow), store *end_ptr (a full
/// guest pointer; 0 = not requested), and apply the UCRT errno/validation
/// rules. Returns the value bits and whether the call is valid.
[[nodiscard]] u64 crt_strto_integer(Marshaler& m, CrtMemoryManager& mem_mgr,
                                    GuestAddr str, GuestAddr end_ptr,
                                    int base, bool wide, CrtIntKind kind) {
    constexpr u32 EINVAL_VALUE = 22;
    constexpr u32 ERANGE_VALUE = 34;
    if (end_ptr != 0) (void)m.write_ptr(end_ptr, str);
    if (str == 0 || base == 1 || base < 0 || base > 36) {
        mem_mgr.set_errno(EINVAL_VALUE);
        (void)mem_mgr.invalid_parameter();
        return 0;
    }
    auto unit_at = [&m, str, wide](usize k) -> i32 {
        if (wide) {
            auto r = m.memory().read_u16(str + static_cast<GuestAddr>(k * 2));
            return r.is_ok() && r.value() != 0 ? static_cast<i32>(r.value()) : -1;
        }
        auto r = m.memory().read_u8(str + static_cast<GuestAddr>(k));
        return r.is_ok() && r.value() != 0 ? static_cast<i32>(r.value()) : -1;
    };
    const CrtIntResult r = crt_parse_integer_units(unit_at, wide, base, kind);
    if (r.range_error) mem_mgr.set_errno(ERANGE_VALUE);
    if (end_ptr != 0 && r.consumed != 0) {
        (void)m.write_ptr(end_ptr,
                          str + static_cast<GuestAddr>(wide ? r.consumed * 2 : r.consumed));
    }
    return r.bits;
}

/// Store a parsed integer as the function's return value.
void crt_return_integer(Marshaler& m, u64 bits, CrtIntKind kind) {
    if (kind == CrtIntKind::I64 || kind == CrtIntKind::U64) {
        m.set_return_i64(bits);
    } else if (kind == CrtIntKind::I32) {
        // long: sign-extended in RAX so a 64-bit read of EAX's value agrees.
        m.set_return(static_cast<u64>(static_cast<i64>(static_cast<i32>(bits))));
    } else {
        m.set_return(static_cast<u32>(bits));
    }
}

} // namespace

void add_crt_integer_conversion_exports(DllExports& exports,
                                        CrtMemoryManager& mem_mgr) {
    // The strtol family and the ato*/_wto* wrappers share one UCRT-measured
    // parser (crt_strto_integer; 09_crt.md HIGH 11/12, MEDIUM _wtoi, LOW
    // atoi/_wtoi64): full-width endptr, EINVAL + invalid-parameter handler
    // for NULL/bad base, saturation with ERANGE, per-unit wide digits.
    auto add_strto = [&exports, &mem_mgr](const char* name, u16 ordinal,
                                          bool wide, CrtIntKind kind) {
        exports.entries.push_back(
            {name, ordinal, [&mem_mgr, wide, kind](Marshaler& m) {
                const u64 bits = crt_strto_integer(
                    m, mem_mgr, m.arg_ptr(0), m.arg_ptr(1),
                    static_cast<int>(static_cast<i32>(m.arg(2))), wide, kind);
                crt_return_integer(m, bits, kind);
            }});
    };
    add_strto("strtol", 0, false, CrtIntKind::I32);
    add_strto("strtoul", 0, false, CrtIntKind::U32);
    add_strto("wcstol", 0, true, CrtIntKind::I32);
    add_strto("wcstoul", 0, true, CrtIntKind::U32);
    add_strto("_strtoi64", 0, false, CrtIntKind::I64);
    add_strto("strtoll", 0, false, CrtIntKind::I64);
    add_strto("_strtoui64", 0, false, CrtIntKind::U64);
    add_strto("strtoull", 0, false, CrtIntKind::U64);
    add_strto("_wcstoi64", 0, true, CrtIntKind::I64);
    add_strto("wcstoll", 0, true, CrtIntKind::I64);
    add_strto("_wcstoui64", 0, true, CrtIntKind::U64);
    add_strto("wcstoull", 0, true, CrtIntKind::U64);
    // atoi(s) is strtol(s, NULL, 10) with int saturation; the UCRT reports
    // ERANGE on overflow and EINVAL (via the handler) on NULL.
    auto add_ato = [&exports, &mem_mgr](const char* name, u16 ordinal,
                                        bool wide, CrtIntKind kind) {
        exports.entries.push_back(
            {name, ordinal, [&mem_mgr, wide, kind](Marshaler& m) {
                const u64 bits = crt_strto_integer(
                    m, mem_mgr, m.arg_ptr(0), 0, 10, wide, kind);
                crt_return_integer(m, bits, kind);
            }});
    };
    add_ato("atoi", 0, false, CrtIntKind::I32);
    add_ato("atol", 0, false, CrtIntKind::I32);
    add_ato("atoll", 0, false, CrtIntKind::I64);
    add_ato("_atoi64", 9, false, CrtIntKind::I64);
    add_ato("_wtoi", 7, true, CrtIntKind::I32);
    add_ato("_wtol", 8, true, CrtIntKind::I32);
    add_ato("_wtoll", 0, true, CrtIntKind::I64);
    add_ato("_wtoi64", 10, true, CrtIntKind::I64);

    // size_t wcstombs(char* destination, const wchar_t* source, size_t count)
    exports.entries.push_back(
        {"wcstombs", 0, [&mem_mgr](Marshaler& m) {
            constexpr u32 EILSEQ_VALUE = 42;
            const GuestAddr destination = m.arg_ptr(0);
            const GuestAddr source = m.arg_ptr(1);
            const usize count = static_cast<usize>(m.arg(2));
            const u64 fail = static_cast<u64>(static_cast<usize>(-1));
            if (source == 0) {
                mem_mgr.set_errno(EILSEQ_VALUE);
                m.set_return(fail);
                return;
            }
            usize converted = 0;
            for (;; ++converted) {
                auto value = m.read_u16(source + converted * 2);
                if (value.is_err()) {
                    mem_mgr.set_errno(EILSEQ_VALUE);
                    m.set_return(fail);
                    return;
                }
                if (value.value() == 0) {
                    if (destination != 0 && converted < count) {
                        (void)m.write_u8(destination + converted, 0);
                    }
                    m.set_return(static_cast<u64>(converted));
                    return;
                }
                if (value.value() > 0xFF) {
                    mem_mgr.set_errno(EILSEQ_VALUE);
                    m.set_return(fail);
                    return;
                }
                if (destination != 0) {
                    if (converted >= count) {
                        m.set_return(static_cast<u64>(converted));
                        return;
                    }
                    (void)m.write_u8(destination + converted,
                                     static_cast<u8>(value.value()));
                }
            }
        }});
}

} // namespace vitreo::winapi
