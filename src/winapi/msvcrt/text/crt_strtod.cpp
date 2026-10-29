/// @file crt_strtod.cpp
/// @brief strtod / strtof / wcstod / wcstof / atof / _wtof and their _l and
///        long-double spellings.
///
/// Contract: the Windows SDK's UCRT source, ucrt/convert/strtod.cpp, atof.cpp
/// and ucrt/inc/corecrt_internal_strtox.h, verified by
/// tools/oracle_probe/crt_strtod_probe.c. The subject-sequence parser is the
/// shared port in crt_float_parse.hpp (scanf runs the same one over its input
/// adapter), driven here by a string source, which -- like the UCRT's
/// c_string_character_source -- can always restore a saved position. This
/// file also holds that header's out-of-line helpers and to_bits.
///
/// The system msvcrt.dll parses decimal only: no hexadecimal floats, no
/// INF/NAN spellings and no non-ASCII digits; everything else (whitespace,
/// ERANGE rules, rounding) matches the UCRT (crt_strtod_probe, "msvcrt" rows).
/// The flavor comes from the calling DLL (CrtDllFlavorScope).

#include "winapi/msvcrt/text/crt_string.hpp"

#include "core/cpu/state/cpu_state.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/text/crt_float_parse.hpp"
#include "winapi/msvcrt/text/crt_string_internal.hpp"

#include <cfenv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vitreo::winapi {

namespace crt_float {

int wide_character_to_digit(u32 c) {
    static constexpr u32 kZeros[] = {0x0030, 0x0660, 0x06F0, 0x0966, 0x09E6, 0x0A66,
                                     0x0AE6, 0x0B66, 0x0C66, 0x0CE6, 0x0D66, 0x0E50,
                                     0x0ED0, 0x0F20, 0x1040, 0x17E0, 0x1810, 0xFF10};
    for (u32 zero : kZeros) {
        if (c >= zero && c < zero + 10) return static_cast<int>(c - zero);
    }
    return -1;
}

unsigned parse_digit(u32 c, bool wide) {
    if (wide) {
        const int v = wide_character_to_digit(c);
        if (v >= 0) return static_cast<unsigned>(v);
    } else if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return ~0u;
}

bool is_space(u32 c, bool wide) {
    if (c == ' ' || (c >= 0x09 && c <= 0x0D)) return true;
    if (!wide) return false;
    return c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

Bits to_bits(Result result, const FpString& fp, bool is_double, unsigned rc) {
    Bits out;
    const u64 sign_bit = is_double ? (1ULL << 63) : (1ULL << 31);
    const u64 sign = fp.negative ? sign_bit : 0;
    const u64 exp_all = is_double ? 0x7FF0000000000000ULL : 0x7F800000ULL;
    const u64 mant_all = is_double ? 0x000FFFFFFFFFFFFFULL : 0x007FFFFFULL;
    const u64 quiet_bit = is_double ? (1ULL << 51) : (1ULL << 22);
    switch (result) {
    case Result::NoDigits:      out.no_digits = true; return out;  // always +0
    case Result::Zero:          out.bits = sign; return out;
    case Result::Infinity:      out.bits = sign | exp_all; return out;
    case Result::QNan:          out.bits = sign | exp_all | mant_all; return out;
    case Result::SNan:          out.bits = sign | exp_all | 1; return out;
    case Result::Indeterminate: out.bits = sign_bit | exp_all | quiet_bit; return out;
    case Result::Overflow:      out.bits = sign | exp_all; out.range_error = true; return out;
    case Result::Underflow:     out.bits = sign; out.range_error = true; return out;
    case Result::DecimalDigits:
    case Result::HexadecimalDigits: break;
    }
    std::string digits;
    digits.reserve(fp.mantissa.size());
    for (char d : fp.mantissa) {
        digits.push_back(static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10));
    }
    std::string text = fp.negative ? "-" : "";
    if (result == Result::HexadecimalDigits) {
        // 0x0.<digits>p<exponent>: the exponent is already base 2.
        text += "0x0." + digits + "p" + std::to_string(fp.exponent);
    } else {
        text += "0." + digits + "e" + std::to_string(fp.exponent);
    }
    static constexpr int kModes[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    const int saved = std::fegetround();
    std::fesetround(kModes[rc & 3]);
    if (is_double) {
        const double v = std::strtod(text.c_str(), nullptr);
        std::memcpy(&out.bits, &v, sizeof(v));
        out.range_error = std::isinf(v) || v == 0.0;
    } else {
        const float v = std::strtof(text.c_str(), nullptr);
        u32 b = 0;
        std::memcpy(&b, &v, sizeof(v));
        out.bits = b;
        out.range_error = std::isinf(v) || v == 0.0f;
    }
    std::fesetround(saved);
    return out;
}

} // namespace crt_float

namespace {

constexpr u32 kEINVAL = 22;
constexpr u32 kERANGE = 34;

/// c_string_character_source: can always step back to a saved position.
struct StringSource {
    const std::vector<u32>& s;
    usize p{0};
    u32 get() {
        const u32 c = p < s.size() ? s[p] : 0;
        ++p;
        return c;
    }
    void unget(u32) { --p; }
    usize save_state() const { return p; }
    bool restore_state(usize state) {
        p = state;
        return true;
    }
};

/// Floating-point return: XMM0 on x64, ST(0) on x86 (float and double alike).
void return_float(Marshaler& m, u64 bits, bool is_double) {
    if (!m.has_cpu()) return;
    if (m.cpu_is_64bit()) {
        m.cpu()->sse.xmm[0].u64_[0] = bits;
        m.cpu()->sse.xmm[0].u64_[1] = 0;
        return;
    }
    double value = 0.0;
    if (is_double) {
        std::memcpy(&value, &bits, sizeof(value));
    } else {
        float f = 0.0f;
        const u32 b = static_cast<u32>(bits);
        std::memcpy(&f, &b, sizeof(f));
        value = static_cast<double>(f);
    }
    m.cpu()->fpu.push(cpu::Float80(value));
}

std::vector<u32> read_units(Marshaler& m, GuestAddr p, bool wide) {
    std::vector<u32> s;
    const usize n = wide ? guest_wcslen(m.memory(), p) : guest_strlen(m.memory(), p);
    s.reserve(n);
    for (usize i = 0; i < n; ++i) {
        s.push_back(wide ? m.memory().read_u16(p + i * 2).value_or(0)
                         : m.memory().read_u8(p + i).value_or(0));
    }
    return s;
}

/// strtod family (end pointer) and atof family (no end pointer, no ERANGE).
void strtod_export(Marshaler& m, CrtMemoryManager& mm, bool wide, bool is_double,
                   bool has_end) {
    const GuestAddr str = m.arg_ptr(0);
    const GuestAddr end_ptr = has_end ? m.arg_ptr(1) : 0;
    if (end_ptr != 0) (void)m.write_ptr(end_ptr, str);
    if (str == 0) {
        mm.set_errno(kEINVAL);
        (void)mm.invalid_parameter();
        return_float(m, 0, is_double);
        return;
    }
    const crt_float::Syntax syntax{wide, CrtDllFlavorScope::current() != CrtDllFlavor::Msvcrt};
    const std::vector<u32> units = read_units(m, str, wide);
    StringSource source{units};
    crt_float::FpString fp;
    const crt_float::Result parsed = crt_float::parse_from_source(source, syntax, fp);
    unsigned rc = 0;
    if (m.has_cpu()) rc = (m.cpu()->sse.mxcsr >> 13) & 3;
    const crt_float::Bits value = crt_float::to_bits(parsed, fp, is_double, rc);
    if (end_ptr != 0) {
        (void)m.write_ptr(end_ptr, str + static_cast<GuestAddr>(source.p * (wide ? 2 : 1)));
    }
    if (has_end && value.range_error) mm.set_errno(kERANGE);
    return_float(m, value.bits, is_double);
}

} // namespace

void add_crt_strtod_exports(DllExports& exports, CrtMemoryManager& mm) {
    struct Row {
        const char* name;
        bool wide;
        bool is_double;
        bool has_end;
    };
    // The _l forms take a trailing _locale_t; Vitreo's only locale is "C".
    // long double is double on Windows.
    static constexpr Row kRows[] = {
        {"strtod", false, true, true},    {"_strtod_l", false, true, true},
        {"strtold", false, true, true},   {"_strtold_l", false, true, true},
        {"strtof", false, false, true},   {"_strtof_l", false, false, true},
        {"wcstod", true, true, true},     {"_wcstod_l", true, true, true},
        {"wcstold", true, true, true},    {"_wcstold_l", true, true, true},
        {"wcstof", true, false, true},    {"_wcstof_l", true, false, true},
        {"atof", false, true, false},     {"_atof_l", false, true, false},
        {"_wtof", true, true, false},     {"_wtof_l", true, true, false},
    };
    for (const Row& row : kRows) {
        const Row r = row;
        exports.entries.push_back({r.name, 0, [r, &mm](Marshaler& m) {
            strtod_export(m, mm, r.wide, r.is_double, r.has_end);
        }, 0});
    }
}

} // namespace vitreo::winapi
