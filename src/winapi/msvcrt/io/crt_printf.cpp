/// @file crt_printf.cpp
/// @brief The CRT printf output processor (see crt_printf.hpp for sources).

#include "winapi/msvcrt/io/crt_printf.hpp"

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/msvcrt/text/crt_string_internal.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace vitreo::winapi::crt_printf {

namespace {

std::atomic<bool> g_count_output{false};
std::atomic<u32> g_msvcrt_output_format{0};

// ---------------------------------------------------------------------------
// Floating point: digit generation and the %e/%f/%g/%a layouts (ucrt/convert)
// ---------------------------------------------------------------------------

enum class FpClass : u8 { Finite, Infinity, QuietNan, SignalingNan, Indeterminate };
enum class Rounding : u8 { Legacy, Standard };

FpClass classify(u64 bits) {
    const u64 exponent = (bits >> 52) & 0x7FF;
    const u64 mantissa = bits & ((1ULL << 52) - 1);
    const bool sign = (bits >> 63) != 0;
    if (exponent != 0x7FF) return FpClass::Finite;
    if (mantissa == 0) return FpClass::Infinity;
    // __acrt_fp_classify: the default NaN (sign set, only the quiet bit) is
    // "indeterminate", printed nan(ind).
    if (sign && mantissa == (1ULL << 51)) return FpClass::Indeterminate;
    if (mantissa & (1ULL << 51)) return FpClass::QuietNan;
    return FpClass::SignalingNan;
}

/// The value's digits as UCRT's __acrt_fltout leaves them: value =
/// 0.mantissa x 10^decpt; `trailing` when nonzero digits exist past the ones
/// generated.
struct Strflt {
    bool negative{false};
    int decpt{0};
    std::string mantissa;
    bool trailing{false};
};

/// Exact decimal expansion of a positive finite nonzero double: significant
/// digits without trailing zeros, and the decimal point position. A double has
/// at most 767 significant decimal digits, so "%.767e" is exact.
void exact_digits(double value, std::string& digits, int& decpt) {
    static thread_local char buf[1024];
    std::snprintf(buf, sizeof(buf), "%.767e", value);
    digits.clear();
    const char* p = buf;
    digits.push_back(*p++);
    if (*p == '.') ++p;
    while (*p >= '0' && *p <= '9') digits.push_back(*p++);
    int exponent = 0;
    if (*p == 'e' || *p == 'E') exponent = std::atoi(p + 1);
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    decpt = exponent + 1;
}

/// __acrt_fltout. @p precision is fltout's own argument (the generator makes
/// precision+1 digits, or k+precision+1 for fixed notation when k >= 0).
Strflt fltout(u64 bits, u64 precision, bool fixed, usize mantissa_cap,
              Flavor flavor, bool flush_denormals) {
    Strflt f;
    f.negative = (bits >> 63) != 0;
    const u64 exponent = (bits >> 52) & 0x7FF;
    const u64 mantissa = bits & ((1ULL << 52) - 1);
    if (exponent == 0 && (mantissa == 0 || flush_denormals)) {
        f.decpt = 0;
        f.mantissa = "0";
        return f;
    }
    switch (classify(bits)) {
    case FpClass::Infinity:      f.decpt = 1; f.mantissa = "1#INF";  f.trailing = true; return f;
    case FpClass::QuietNan:      f.decpt = 1; f.mantissa = "1#QNAN"; return f;
    case FpClass::SignalingNan:  f.decpt = 1; f.mantissa = "1#SNAN"; return f;
    case FpClass::Indeterminate: f.decpt = 1; f.mantissa = "1#IND";  return f;
    case FpClass::Finite: break;
    }
    double value = 0.0;
    const u64 positive = bits & ~(1ULL << 63);
    std::memcpy(&value, &positive, sizeof(value));
    std::string digits;
    int decpt = 0;
    exact_digits(value, digits, decpt);
    if (flavor == Flavor::Msvcrt) {
        // msvcrt.dll generates 17 significant digits, rounded to nearest with
        // ties to even, then zeros (0.1 with %.20f -> 0.10000000000000001000;
        // 348389360662118.125 -> 3.4838936066211812e+014; measured).
        if (digits.size() > 17) {
            bool up = digits[17] > '5';
            if (digits[17] == '5') {
                const bool beyond = digits.find_first_not_of('0', 18) != std::string::npos;
                up = beyond || ((digits[16] - '0') % 2) != 0;
            }
            digits.resize(17);
            if (up) {
                int i = 16;
                while (i >= 0 && digits[static_cast<usize>(i)] == '9') {
                    digits[static_cast<usize>(i)] = '0';
                    --i;
                }
                if (i < 0) {
                    digits.insert(digits.begin(), '1');
                    digits.pop_back();
                    ++decpt;
                } else {
                    digits[static_cast<usize>(i)] += 1;
                }
            }
            while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
        }
        f.decpt = decpt;
        f.mantissa = digits;
        f.trailing = false;
        return f;
    }
    const u64 conv_precision = precision + 1;
    u64 required = conv_precision;
    if (decpt >= 0 && fixed && precision <= 0x7FFFFFFFULL) {
        required = static_cast<u64>(decpt) + conv_precision;
    }
    u64 n = std::min<u64>(required, mantissa_cap);
    n = std::min<u64>(n, digits.size());
    if (n == 0) n = 1;  // the leading digit is always generated
    f.decpt = decpt;
    f.mantissa = digits.substr(0, static_cast<usize>(n));
    f.trailing = digits.size() > n;
    return f;
}

bool check_trailing(const std::string& mantissa, usize from, bool trailing) {
    if (trailing) return true;
    while (from < mantissa.size() && mantissa[from] == '0') ++from;
    return from < mantissa.size();
}

/// MXCSR.RC: 0 nearest, 1 down, 2 up, 3 toward zero (fegetround).
bool should_round_up(const Strflt& f, usize next, Rounding rounding,
                     unsigned rc) {
    const char c = next < f.mantissa.size() ? f.mantissa[next] : '\0';
    if (rounding == Rounding::Legacy) return c >= '5';
    switch (rc) {
    case 0:
        if (c > '5') return true;
        if (c < '5') return false;
        if (check_trailing(f.mantissa, next + 1, f.trailing)) return true;
        if (next == 0) return false;
        return (f.mantissa[next - 1] % 2) != 0;
    case 2: return check_trailing(f.mantissa, next, f.trailing) && !f.negative;
    case 1: return check_trailing(f.mantissa, next, f.trailing) && f.negative;
    default: return false;
    }
}

/// __acrt_fp_strflt_to_string: @p digits digits of the mantissa (zero padded),
/// rounded on the next one; carrying into a new leading digit bumps decpt.
std::string strflt_to_string(Strflt& f, i64 digits, Rounding rounding,
                             unsigned rc) {
    std::string buf = "0";
    usize next = 0;
    for (i64 d = digits; d > 0; --d) {
        buf.push_back(next < f.mantissa.size() ? f.mantissa[next++] : '0');
    }
    if (digits >= 0 && should_round_up(f, next, rounding, rc)) {
        usize i = buf.size() - 1;
        while (buf[i] == '9') {
            buf[i] = '0';
            --i;
        }
        buf[i] = static_cast<char>(buf[i] + 1);
    }
    if (buf[0] == '1') {
        ++f.decpt;
    } else {
        buf.erase(0, 1);
    }
    return buf;
}

std::string exponent_text(const Strflt& f, bool capitals, unsigned min_digits) {
    std::string e = capitals ? "E+000" : "e+000";
    if (f.mantissa.empty() || f.mantissa[0] != '0') {
        int exp = f.decpt - 1;
        if (exp < 0) {
            exp = -exp;
            e[1] = '-';
        }
        e[2] = static_cast<char>('0' + (exp / 100) % 10);
        e[3] = static_cast<char>('0' + (exp / 10) % 10);
        e[4] = static_cast<char>('0' + exp % 10);
    }
    if (min_digits == 2 && e[2] == '0') e.erase(2, 1);
    return e;
}

/// fp_format_e_internal. For %e, @p r holds precision+1 digits; for %g
/// (g_fmt) it holds `precision` significant digits.
std::string e_internal(const Strflt& f, const std::string& r, int precision,
                       bool capitals, unsigned min_exp, bool g_fmt) {
    std::string out;
    if (f.negative) out.push_back('-');
    out.push_back(r.empty() ? '0' : r[0]);
    if (precision > 0) {
        out.push_back('.');
        const usize count = static_cast<usize>(g_fmt ? precision - 1 : precision);
        for (usize i = 0; i < count; ++i) {
            out.push_back(1 + i < r.size() ? r[1 + i] : '0');
        }
    }
    out += exponent_text(f, capitals, min_exp);
    return out;
}

/// fp_format_f_internal. @p r is the rounded digit string.
std::string f_internal(const Strflt& f, std::string r, int precision,
                       bool g_fmt) {
    const int g_magnitude = f.decpt - 1;
    if (g_fmt && g_magnitude == precision) {
        r = r.substr(0, static_cast<usize>(std::max(g_magnitude, 0))) + "0";
    }
    usize pos = 0;
    if (f.decpt <= 0) {
        const bool is_zero = f.decpt == 0 && !f.mantissa.empty() &&
                             f.mantissa[0] == '0';
        if (!g_fmt || !is_zero) {
            r.insert(r.begin(), '0');
        } else if (r.empty()) {
            r = "0";
        } else {
            r[0] = '0';
        }
        pos = 1;
    } else {
        pos = static_cast<usize>(f.decpt);
        if (r.size() < pos) r.append(pos - r.size(), '0');
    }
    if (precision > 0) {
        r.insert(pos, ".");
        if (f.decpt < 0) {
            const int zeros = (g_fmt || -f.decpt < precision) ? -f.decpt : precision;
            r.insert(pos + 1, static_cast<usize>(zeros), '0');
        }
    }
    return f.negative ? "-" + r : r;
}

struct FloatContext {
    Flavor flavor;
    u64 options;
    bool two_digit_exponent;
    Rounding rounding;
    unsigned rc;
    bool flush;
    usize mantissa_cap;
};

std::string format_e(u64 bits, int precision, bool capitals, unsigned min_exp,
                     const FloatContext& c) {
    Strflt f = fltout(bits, static_cast<u64>(precision) + 1, false,
                      c.mantissa_cap, c.flavor, c.flush);
    const std::string r = strflt_to_string(f, static_cast<i64>(precision) + 1,
                                           c.rounding, c.rc);
    return e_internal(f, r, precision, capitals, min_exp, false);
}

std::string format_f(u64 bits, int precision, const FloatContext& c) {
    Strflt f = fltout(bits, static_cast<u64>(precision), true, c.mantissa_cap,
                      c.flavor, c.flush);
    const std::string r = strflt_to_string(
        f, static_cast<i64>(precision) + f.decpt, c.rounding, c.rc);
    return f_internal(f, r, precision, false);
}

std::string format_g(u64 bits, int precision, bool capitals, unsigned min_exp,
                     const FloatContext& c) {
    Strflt f = fltout(bits, static_cast<u64>(precision), true, c.mantissa_cap,
                      c.flavor, c.flush);
    const int magnitude_before = f.decpt - 1;
    std::string r = strflt_to_string(f, precision, c.rounding, c.rc);
    const bool round_expansion = magnitude_before < f.decpt - 1;
    const int magnitude = f.decpt - 1;
    if (magnitude < -4 || magnitude >= precision) {
        return e_internal(f, r, precision, capitals, min_exp, true);
    }
    if (round_expansion && !r.empty()) r.pop_back();
    return f_internal(f, r, precision, true);
}

/// fp_format_a (the "0x" prefix is added by the processor).
std::string format_a(u64 bits, int precision, bool capitals, unsigned min_exp,
                     const FloatContext& c) {
    if (precision < 0) precision = 0;
    const u64 exponent = (bits >> 52) & 0x7FF;
    if (exponent == 0x7FF) {
        std::string s = format_e(bits, precision, false, min_exp, c);
        const usize e = s.rfind('e');
        if (e != std::string::npos) {
            s[e] = capitals ? 'P' : 'p';
            s.resize(std::min(s.size(), e + 3));
        }
        return s;
    }
    const u64 mantissa = bits & ((1ULL << 52) - 1);
    const int hexadd = (capitals ? 'A' : 'a') - '9' - 1;
    std::string out;
    if (bits >> 63) out.push_back('-');
    i64 debias = 1023;
    if (exponent == 0) {
        out.push_back('0');
        debias = mantissa == 0 ? 0 : debias - 1;
    } else {
        out.push_back('1');
    }
    const usize point = out.size();
    out.push_back('.');
    int remaining = precision;
    if (mantissa > 0) {
        int maskpos = 52 - 4;
        while (maskpos >= 0 && remaining > 0) {
            int digit = static_cast<int>((mantissa >> maskpos) & 0xF) + '0';
            if (digit > '9') digit += hexadd;
            out.push_back(static_cast<char>(digit));
            maskpos -= 4;
            --remaining;
        }
        if (maskpos >= 0) {
            const unsigned digit = static_cast<unsigned>((mantissa >> maskpos) & 0xF);
            bool up = false;
            if (c.flavor == Flavor::Msvcrt) {
                // msvcrt.dll rounds up only above half: 0x1.8 -> 0x1 and
                // 0x1.08 -> 0x1.0, 0x1.c -> 0x2 (measured).
                up = digit > 8;
            } else if (c.rounding == Rounding::Legacy) {
                up = digit >= 8;
            } else if (c.rc == 0) {
                if (digit > 8) up = true;
                else if (digit == 8) {
                    const u64 below = mantissa & ((1ULL << maskpos) - 1);
                    if (below != 0) up = true;
                    else {
                        const unsigned left = maskpos != 52 - 4
                            ? static_cast<unsigned>((mantissa >> (maskpos + 4)) & 0xF)
                            : (exponent == 0 ? 0u : 1u);
                        up = (left % 2) == 1;
                    }
                }
            } else if (c.rc == 2) {
                up = digit != 0 && !(bits >> 63);
            } else if (c.rc == 1) {
                up = digit != 0 && (bits >> 63);
            }
            if (up) {
                usize p = out.size() - 1;
                while (out[p] == 'f' || out[p] == 'F') {
                    out[p] = '0';
                    --p;
                }
                if (p != point) {
                    if (out[p] == '9') out[p] = static_cast<char>(out[p] + 1 + hexadd);
                    else out[p] = static_cast<char>(out[p] + 1);
                } else {
                    out[p - 1] = static_cast<char>(out[p - 1] + 1);
                }
            }
        }
    }
    for (; remaining > 0; --remaining) out.push_back('0');
    if (precision == 0) out.erase(point, 1);
    out.push_back(capitals ? 'P' : 'p');
    i64 e = static_cast<i64>(exponent) - debias;
    if (e >= 0) {
        out.push_back('+');
    } else {
        out.push_back('-');
        e = -e;
    }
    out += std::to_string(e);
    (void)min_exp;
    return out;
}

/// __acrt_fp_format.
std::string fp_format(u64 bits, char type, int precision,
                      const FloatContext& c) {
    const bool capitals = type == 'A' || type == 'E' || type == 'F' || type == 'G';
    const bool legacy_specials = c.flavor == Flavor::Msvcrt ||
                                 (c.options & kLegacyMsvcrtCompatibility) != 0;
    if (!legacy_specials) {
        const FpClass cls = classify(bits);
        if (cls != FpClass::Finite) {
            std::string s = (bits >> 63) ? "-" : "";
            static const char* const kStrings[4][2] = {
                {"INF", "inf"}, {"NAN", "nan"},
                {"NAN(SNAN)", "nan(snan)"}, {"NAN(IND)", "nan(ind)"}};
            const int row = cls == FpClass::Infinity ? 0
                          : cls == FpClass::QuietNan ? 1
                          : cls == FpClass::SignalingNan ? 2 : 3;
            s += kStrings[row][capitals ? 0 : 1];
            return s;
        }
    }
    const unsigned min_exp = c.flavor == Flavor::Msvcrt
        ? (c.two_digit_exponent ? 2 : 3)
        : ((c.options & kLegacyThreeDigitExponents) != 0 ? 3 : 2);
    switch (type) {
    case 'a': case 'A': return format_a(bits, precision, capitals, min_exp, c);
    case 'e': case 'E': return format_e(bits, precision, capitals, min_exp, c);
    case 'f': case 'F': return format_f(bits, precision, c);
    default:            return format_g(bits, precision, capitals, min_exp, c);
    }
}

/// force_decimal_point: '#' with precision 0.
void force_decimal_point(std::string& s) {
    usize i = 0;
    if (s.empty()) return;
    if (std::tolower(static_cast<unsigned char>(s[0])) != 'e') {
        do {
            ++i;
        } while (i < s.size() && s[i] >= '0' && s[i] <= '9');
    }
    if (i < s.size() && std::tolower(static_cast<unsigned char>(s[i])) == 'x') i += 2;
    if (i > s.size()) i = s.size();
    s.insert(i, ".");
}

/// crop_zeroes: %g without '#'.
void crop_zeroes(std::string& s) {
    const usize point = s.find('.');
    if (point == std::string::npos) return;
    usize stop = point + 1;
    while (stop < s.size() && s[stop] != 'e' && s[stop] != 'E') ++stop;
    usize last = stop;  // one past the last kept character
    while (last > point + 1 && s[last - 1] == '0') --last;
    if (last == point + 1) last = point;  // drop the '.' too
    s.erase(last, stop - last);
}

// ---------------------------------------------------------------------------
// The format state machine (corecrt_internal_stdio_output.h)
// ---------------------------------------------------------------------------

enum class State : u8 { Normal, Percent, Flag, Width, Dot, Precision, Size, Type, Invalid };
enum class Cls : u8 { Other, Percent, Dot, Star, Zero, Digit, FlagChar, SizeChar, TypeChar };
enum class Length : u8 { None, hh, h, l, ll, j, z, t, L, I, I32, I64, w, T };

enum Flag : unsigned {
    FL_SIGN = 0x01, FL_SIGNSP = 0x02, FL_LEFT = 0x04, FL_LEADZERO = 0x08,
    FL_SIGNED = 0x10, FL_ALTERNATE = 0x20, FL_NEGATIVE = 0x40, FL_FORCEOCTAL = 0x80,
};

bool is_integer_type(u32 c) {
    return c == 'd' || c == 'i' || c == 'o' || c == 'u' || c == 'x' || c == 'X';
}

State next_state(State prev, Cls cls, bool validating) {
    using S = State;
    if (validating && prev == S::Invalid) return S::Normal;
    const bool mid = prev == S::Percent || prev == S::Flag || prev == S::Width ||
                     prev == S::Dot || prev == S::Precision || prev == S::Size;
    switch (cls) {
    case Cls::Other:
        if (validating && mid) return S::Invalid;
        return S::Normal;
    case Cls::Percent:
        if (prev == S::Normal || prev == S::Type) return S::Percent;
        if (validating && prev != S::Percent) return S::Invalid;
        return S::Normal;
    case Cls::Dot:
        if (prev == S::Percent || prev == S::Flag || prev == S::Width) return S::Dot;
        if (validating && (prev == S::Dot || prev == S::Precision || prev == S::Size)) return S::Invalid;
        return S::Normal;
    case Cls::Star:
        if (prev == S::Percent || prev == S::Flag) return S::Width;
        if (prev == S::Dot) return S::Precision;
        if (validating && (prev == S::Width || prev == S::Precision || prev == S::Size)) return S::Invalid;
        return S::Normal;
    case Cls::Zero:
        if (prev == S::Percent || prev == S::Flag) return S::Flag;
        if (prev == S::Width) return S::Width;
        if (prev == S::Dot || prev == S::Precision) return S::Precision;
        if (validating && prev == S::Size) return S::Invalid;
        return S::Normal;
    case Cls::Digit:
        if (prev == S::Percent || prev == S::Flag || prev == S::Width) return S::Width;
        if (prev == S::Dot || prev == S::Precision) return S::Precision;
        if (validating && prev == S::Size) return S::Invalid;
        return S::Normal;
    case Cls::FlagChar:
        if (prev == S::Percent || prev == S::Flag) return S::Flag;
        if (validating && mid) return S::Invalid;
        return S::Normal;
    case Cls::SizeChar:
        if (mid) return S::Size;
        return S::Normal;
    case Cls::TypeChar:
        if (mid) return S::Type;
        return S::Normal;
    }
    return S::Normal;
}

usize to_integer_size(Length length, bool is64, Flavor flavor) {
    switch (length) {
    case Length::None: return 4;
    case Length::hh:   return 1;
    case Length::h:    return 2;
    case Length::l:    return 4;
    case Length::ll:   return 8;
    case Length::j:    return 8;
    case Length::z:
    case Length::t:
    case Length::I:    return is64 ? 8 : 4;
    case Length::I32:  return 4;
    case Length::I64:  return 8;
    // msvcrt.dll accepts L and w on integers as plain int (measured %Ld, %wd).
    case Length::L:
    case Length::w:    return flavor == Flavor::Msvcrt ? 4 : 0;
    default:           return 0;
    }
}

enum class ArgType : u8 { Unused, Int32, Int64, Pointer, Real64 };

struct Parameter {
    ArgType actual{ArgType::Unused};
    u32 format_type{0};
    Length length{Length::None};
    ArgCursor cursor{ArgCursor::call_args(0)};
};

constexpr int kArgMax = 100;  // _ARGMAX

class OutputProcessor {
public:
    OutputProcessor(Marshaler& m, const Request& r, std::vector<u32> fmt,
              ArgCursor args, Target& out)
        : m_(m), r_(r), fmt_(std::move(fmt)), args_(args), out_(out),
          is64_(m.cpu_is_64bit()) {
        validating_ = r.processor != crt_printf::Processor::Standard;
        positional_ = r.processor == crt_printf::Processor::Positional;
        if (m.has_cpu()) {
            const u32 mxcsr = m.cpu()->sse.mxcsr;
            fc_.rc = (mxcsr >> 13) & 3;
            fc_.flush = (mxcsr & 0x8000u) != 0;
        }
        fc_.flavor = r.flavor;
        fc_.options = r.options;
        fc_.two_digit_exponent = r.two_digit_exponent;
        fc_.rounding = (r.flavor == Flavor::Ucrt && (r.options & kStandardRounding))
                           ? Rounding::Standard : Rounding::Legacy;
    }

    int run() {
        pass_ = 0;
        while (advance_pass()) {
            state_ = State::Normal;
            while (pos_ < fmt_.size()) {
                const u32 c = fmt_[pos_++];
                if (c == 0) break;
                if (written_ < 0) break;
                fc_char_ = c;
                state_ = next_state(state_, classify(c), validating_);
                if (!begin_character()) return -1;
                if (state_ == State::Invalid) {
                    fail_invalid();
                    return -1;
                }
                bool ok = true;
                switch (state_) {
                case State::Normal:    ok = case_normal(); break;
                case State::Percent:   ok = case_percent(); break;
                case State::Flag:      ok = case_flag(); break;
                case State::Width:     ok = case_width(); break;
                case State::Dot:       precision_ = 0; break;
                case State::Precision: ok = case_precision(); break;
                case State::Size:      ok = case_size(); break;
                case State::Type:      ok = case_type(); break;
                case State::Invalid:   break;
                }
                if (!ok) return -1;
            }
            if (!end_of_format()) return -1;
        }
        return written_;
    }

private:
    // ---- output adapter ------------------------------------------------------
    void write_char(u16 c) {
        if (out_.stored.size() >= out_.capacity) {
            if (out_.continue_count) ++written_;
            else written_ = -1;
            return;
        }
        ++written_;
        out_.stored.push_back(c);
    }
    void write_string(const u16* s, int len) {
        if (len <= 0) return;
        if (out_.stored.size() >= out_.capacity) {
            if (out_.continue_count) written_ += len;
            else written_ = -1;
            return;
        }
        const usize space = out_.capacity - out_.stored.size();
        const usize n = std::min(space, static_cast<usize>(len));
        out_.stored.insert(out_.stored.end(), s, s + n);
        if (out_.continue_count) written_ += len;
        else if (n != static_cast<usize>(len)) written_ = -1;
        else written_ += static_cast<int>(n);
    }
    void write_repeat(u16 c, int count) {
        for (int i = 0; i < count; ++i) {
            write_char(c);
            if (written_ == -1) break;
        }
    }

    void fail_invalid() {
        if (r_.set_errno) r_.set_errno(kEINVAL);
        if (r_.invalid_parameter) r_.invalid_parameter();
    }
    void set_errno(i32 e) {
        if (r_.set_errno) r_.set_errno(e);
    }

    u32 peek(usize offset = 0) const {
        const usize i = pos_ + offset;
        return i < fmt_.size() ? fmt_[i] : 0;
    }

    Cls classify(u32 c) const {
        if (c < ' ' || c > 'z') return Cls::Other;
        switch (c) {
        case ' ': case '#': case '+': case '-': return Cls::FlagChar;
        case '%': return Cls::Percent;
        case '*': return Cls::Star;
        case '.': return Cls::Dot;
        case '0': return Cls::Zero;
        case 'F': case 'L': case 'N': case 'h': case 'j': case 'l': case 'w':
            return Cls::SizeChar;
        case 'T':
            return r_.flavor == Flavor::Ucrt ? Cls::SizeChar : Cls::Other;
        case 'I':
            if (r_.flavor == Flavor::Msvcrt) {
                // msvcrt: I sizes only I32/I64 or a following integer type;
                // otherwise it is an ordinary character (%Ic prints "Ic").
                const bool ok = (peek() == '3' && peek(1) == '2') ||
                                (peek() == '6' && peek(1) == '4') ||
                                is_integer_type(peek());
                return ok ? Cls::SizeChar : Cls::Other;
            }
            return Cls::SizeChar;
        case 'z': case 't':
            if (r_.flavor == Flavor::Msvcrt) {
                return is_integer_type(peek()) ? Cls::SizeChar : Cls::Other;
            }
            return Cls::SizeChar;
        case 'n':
            return validating_ ? Cls::Other : Cls::TypeChar;
        case 'A': case 'C': case 'E': case 'G': case 'S': case 'X': case 'Z':
        case 'a': case 'c': case 'd': case 'e': case 'f': case 'g': case 'i':
        case 'o': case 'p': case 's': case 'u': case 'x':
            return Cls::TypeChar;
        default:
            if (c >= '1' && c <= '9') return Cls::Digit;
            return Cls::Other;
        }
    }

    // ---- positional parameters ---------------------------------------------
    // Pass 1 scans types (and, for a non-positional format, produces the
    // output); pass 2 outputs a positional format.
    bool advance_pass() {
        ++pass_;
        if (!positional_) return pass_ == 1;
        if (pass_ == 3) return false;
        if (pass_ == 2 && mode_ == Mode::Nonpositional) return false;
        if (pass_ == 2) {
            mode_ = Mode::Unknown;
            max_index_ = -1;
            type_index_ = -1;
            width_ = 0;
            precision_ = 0;
            pos_ = 0;
        }
        return true;
    }
    enum class Mode : u8 { Unknown, Nonpositional, Positional };
    bool scan_pass() const { return positional_ && pass_ == 1; }
    bool should_format() const {
        return !positional_ || pass_ != 1 || mode_ == Mode::Nonpositional;
    }

    // strtol(format, &end, 10) over the format units.
    long parse_long(usize from, usize& end) const {
        usize i = from;
        long long v = 0;
        bool any = false;
        while (i < fmt_.size() && fmt_[i] >= '0' && fmt_[i] <= '9') {
            v = v * 10 + static_cast<long long>(fmt_[i] - '0');
            if (v > 0x7FFFFFFFLL) v = 0x80000000LL;  // saturate (LONG_MAX+1 marks ERANGE)
            ++i;
            any = true;
        }
        end = any ? i : from;
        return static_cast<long>(v);
    }

    bool begin_character() {
        if (!positional_) return true;
        if (state_ != State::Percent || peek() == '%') return true;
        if (mode_ == Mode::Unknown) {
            const u32 c = peek();
            if (c < '0' || c > '9') {
                mode_ = Mode::Nonpositional;
            } else {
                usize end = 0;
                const long v = parse_long(pos_, end);
                if (v > 0 && end < fmt_.size() && fmt_[end] == '$') {
                    if (pass_ == 1) {
                        for (auto& p : params_) p = Parameter{};
                    }
                    mode_ = Mode::Positional;
                } else {
                    mode_ = Mode::Nonpositional;
                }
            }
        }
        if (mode_ != Mode::Positional) return true;
        usize end = 0;
        type_index_ = static_cast<int>(parse_long(pos_, end)) - 1;
        const bool dollar = end < fmt_.size() && fmt_[end] == '$';
        pos_ = end + 1;
        if (pass_ != 1) return true;
        if (!(type_index_ >= 0 && dollar && type_index_ < kArgMax)) {
            fail_invalid();
            return false;
        }
        max_index_ = std::max(max_index_, type_index_);
        return true;
    }

    static bool is_string_spec(u32 c) { return c == 's' || c == 'S'; }
    static bool is_char_spec(u32 c) { return c == 'c' || c == 'C'; }
    static bool is_integral_spec(u32 c) {
        return is_integer_type(c) || c == '*';
    }

    bool reappearance_consistent(const Parameter& p, ArgType actual, u32 type,
                                 Length length) const {
        const bool old_ptr = p.format_type == 'p';
        const bool new_ptr = type == 'p';
        if (old_ptr || new_ptr) return old_ptr == new_ptr;
        const bool old_str = is_string_spec(p.format_type), new_str = is_string_spec(type);
        const bool old_chr = is_char_spec(p.format_type), new_chr = is_char_spec(type);
        if (old_str || new_str || old_chr || new_chr) {
            if (old_str != new_str || old_chr != new_chr) return false;
            return is_wide_spec(p.format_type, p.length) == is_wide_spec(type, length);
        }
        const bool old_int = is_integral_spec(p.format_type), new_int = is_integral_spec(type);
        if (old_int || new_int) {
            if (old_int != new_int) return false;
            if ((p.length == Length::I) != (length == Length::I)) return false;
            return to_integer_size(p.length, is64_, r_.flavor) ==
                   to_integer_size(length, is64_, r_.flavor);
        }
        return p.actual == actual;
    }

    bool store_parameter(int index, ArgType actual, u32 type, Length length) {
        Parameter& p = params_[static_cast<usize>(index)];
        if (p.actual == ArgType::Unused) {
            p.actual = actual;
            p.format_type = type;
            p.length = length;
            return true;
        }
        if (!reappearance_consistent(p, actual, type, length)) {
            fail_invalid();
            return false;
        }
        return true;
    }

    bool end_of_format() {
        if (validating_ && !(state_ == State::Normal || state_ == State::Type)) {
            fail_invalid();
            return false;
        }
        if (!positional_ || mode_ != Mode::Positional || pass_ != 1) return true;
        // Lay out every positional parameter along the va_list.
        ArgCursor walk = args_;
        for (int i = 0; i <= max_index_; ++i) {
            Parameter& p = params_[static_cast<usize>(i)];
            p.cursor = walk;
            switch (p.actual) {
            case ArgType::Int32:   (void)walk.next_int32(m_); break;
            case ArgType::Int64:   (void)walk.next_int64(m_); break;
            case ArgType::Pointer: (void)walk.next_pointer(m_); break;
            case ArgType::Real64:  (void)walk.next_double_bits(m_); break;
            default:
                fail_invalid();  // "Missing position in the format string"
                return false;
            }
        }
        return true;
    }

    /// extract_argument_from_va_list: read (or, in the positional scan, type
    /// check) the current specifier's argument.
    bool extract(ArgType type, u64& value) {
        if (!positional_ || mode_ == Mode::Nonpositional) {
            value = read(args_, type);
            return true;
        }
        if (!(type_index_ >= 0 && type_index_ < kArgMax)) {
            fail_invalid();
            return false;
        }
        if (pass_ == 1) {
            return store_parameter(type_index_, type, fc_char_, length_);
        }
        ArgCursor c = params_[static_cast<usize>(type_index_)].cursor;
        value = read(c, type);
        return true;
    }
    u64 read(ArgCursor& c, ArgType type) {
        switch (type) {
        case ArgType::Int32:   return c.next_int32(m_);
        case ArgType::Int64:   return c.next_int64(m_);
        case ArgType::Pointer: return c.next_pointer(m_);
        case ArgType::Real64:  return c.next_double_bits(m_);
        default:               return 0;
        }
    }

    // '*' width / precision
    bool star_value(int& value) {
        if (!positional_ || mode_ == Mode::Nonpositional) {
            value = static_cast<int>(args_.next_int32(m_));
            return true;
        }
        usize end = 0;
        const int index = static_cast<int>(parse_long(pos_, end)) - 1;
        const bool dollar = end < fmt_.size() && fmt_[end] == '$';
        pos_ = end + 1;
        if (pass_ == 1) {
            if (!(index >= 0 && dollar && index < kArgMax)) {
                fail_invalid();
                return false;
            }
            max_index_ = std::max(max_index_, index);
            return store_parameter(index, ArgType::Int32, fc_char_, length_);
        }
        ArgCursor c = params_[static_cast<usize>(index)].cursor;
        value = static_cast<int>(c.next_int32(m_));
        return true;
    }

    // ---- states --------------------------------------------------------------
    bool case_normal() {
        if (positional_) {
            if (pass_ == 1 && mode_ == Mode::Positional) return true;
            if (pass_ == 2 && mode_ == Mode::Unknown) return true;
        }
        // "C" locale: no DBCS lead bytes in a narrow format.
        write_char(static_cast<u16>(fc_char_));
        return true;
    }

    bool case_percent() {
        width_ = 0;
        suppress_ = false;
        flags_ = 0;
        precision_ = -1;
        length_ = Length::None;
        return true;
    }

    bool case_flag() {
        switch (fc_char_) {
        case '-': flags_ |= FL_LEFT; break;
        case '+': flags_ |= FL_SIGN; break;
        case ' ': flags_ |= FL_SIGNSP; break;
        case '#': flags_ |= FL_ALTERNATE; break;
        case '0': flags_ |= FL_LEADZERO; break;
        default: break;
        }
        return true;
    }

    bool parse_int(int& result) {
        usize end = 0;
        const long v = parse_long(pos_ - 1, end);
        if (v >= 0x80000000L || static_cast<long long>(v) > 0x7FFFFFFFLL) {
            return false;  // ERANGE: fails without the handler, errno unchanged
        }
        result = static_cast<int>(v);
        if (end < pos_) return false;
        pos_ = end;
        return true;
    }

    bool case_width() {
        if (fc_char_ != '*') return parse_int(width_);
        int w = 0;
        if (!star_value(w)) return false;
        if (!should_format()) return true;
        width_ = w;
        if (width_ < 0) {
            flags_ |= FL_LEFT;
            width_ = -width_;
        }
        return true;
    }

    bool case_precision() {
        if (fc_char_ != '*') return parse_int(precision_);
        int p = 0;
        if (!star_value(p)) return false;
        if (!should_format()) return true;
        precision_ = p < 0 ? -1 : p;
        return true;
    }

    bool case_size() {
        const bool compat = r_.flavor == Flavor::Msvcrt ||
                            (r_.options & kLegacyMsvcrtCompatibility) != 0;
        if (fc_char_ == 'F') {
            if (!compat) {
                state_ = State::Type;
                return case_type();
            }
            return true;
        }
        if (fc_char_ == 'N') {
            if (!compat) {
                state_ = State::Invalid;
                fail_invalid();
                return false;
            }
            return true;
        }
        if (length_ != Length::None) {
            fail_invalid();
            return false;
        }
        switch (fc_char_) {
        case 'h':
            if (peek() == 'h') {
                ++pos_;
                // msvcrt.dll reads hh as h (measured %hhd of 300 -> 300).
                length_ = r_.flavor == Flavor::Msvcrt ? Length::h : Length::hh;
            } else {
                length_ = Length::h;
            }
            return true;
        case 'I':
            if (peek() == '3' && peek(1) == '2') {
                pos_ += 2;
                length_ = Length::I32;
            } else if (peek() == '6' && peek(1) == '4') {
                pos_ += 2;
                length_ = Length::I64;
            } else if (is_integer_type(peek())) {
                length_ = Length::I;
            }
            return true;
        case 'l':
            if (peek() == 'l') {
                ++pos_;
                length_ = Length::ll;
            } else {
                length_ = Length::l;
            }
            return true;
        case 'L': length_ = Length::L; return true;
        case 'j': length_ = Length::j; return true;
        case 't': length_ = Length::t; return true;
        case 'z': length_ = Length::z; return true;
        case 'w': length_ = Length::w; return true;
        case 'T': length_ = Length::T; return true;
        default: return true;
        }
    }

    bool is_wide_spec(u32 type, Length length) const {
        if (r_.flavor == Flavor::Msvcrt && (type == 'Z')) {
            // msvcrt.dll: %Z is an ANSI_STRING unless 'w' (measured %lZ, %Z in
            // the wide family).
            return length == Length::w;
        }
        switch (length) {
        case Length::l: case Length::w: return true;
        case Length::h: return false;
        default: break;
        }
        if (length == Length::T) return r_.wide;
        const bool naturally_wide = r_.wide &&
            (r_.flavor == Flavor::Msvcrt || (r_.options & kLegacyWideSpecifiers) != 0);
        const bool natural_width = type == 'c' || type == 's';
        return naturally_wide == natural_width;
    }

    // ---- type cases ------------------------------------------------------------
    bool case_type() {
        str_.clear();
        wide_str_ = false;
        bool ok = false;
        switch (fc_char_) {
        case 'C': case 'c': ok = type_c(); break;
        case 'Z': ok = type_Z(); break;
        case 'S': case 's': ok = type_s(); break;
        case 'A': case 'E': case 'F': case 'G':
        case 'a': case 'e': case 'f': case 'g': ok = type_float(); break;
        case 'd': case 'i': flags_ |= FL_SIGNED; ok = type_integer(10, false); break;
        case 'u': ok = type_integer(10, false); break;
        case 'o':
            if (flags_ & FL_ALTERNATE) flags_ |= FL_FORCEOCTAL;
            ok = type_integer(8, false);
            break;
        case 'X': ok = type_integer(16, true); break;
        case 'x': ok = type_integer(16, false); break;
        case 'p': ok = type_p(); break;
        case 'n': ok = type_n(); break;
        default: ok = true; break;
        }
        if (!ok) return false;
        if (positional_ && mode_ == Mode::Positional && pass_ == 1) return true;
        if (suppress_) return true;

        u16 prefix[3] = {};
        int prefix_len = 0;
        if (flags_ & FL_SIGNED) {
            if (flags_ & FL_NEGATIVE) prefix[prefix_len++] = '-';
            else if (flags_ & FL_SIGN) prefix[prefix_len++] = '+';
            else if (flags_ & FL_SIGNSP) prefix[prefix_len++] = ' ';
        }
        const u32 t = fc_char_;
        const bool int_0x = (t == 'x' || t == 'X') && (flags_ & FL_ALTERNATE);
        const bool float_0x = t == 'a' || t == 'A';
        // msvcrt.dll: %#p prints a 0X prefix (measured).
        const bool ptr_0x = r_.flavor == Flavor::Msvcrt && t == 'p' &&
                            (flags_ & FL_ALTERNATE);
        if (int_0x || float_0x || ptr_0x) {
            prefix[prefix_len++] = '0';
            prefix[prefix_len++] = (t == 'X' || t == 'A' || t == 'p') ? 'X' : 'x';
        }
        const int length = static_cast<int>(str_.size());
        const int padding = width_ - length - prefix_len;
        if (!(flags_ & (FL_LEFT | FL_LEADZERO))) write_repeat(' ', padding);
        write_string(prefix, prefix_len);
        if ((flags_ & FL_LEADZERO) && !(flags_ & FL_LEFT)) write_repeat('0', padding);
        write_stored_string();
        if (written_ >= 0 && (flags_ & FL_LEFT)) write_repeat(' ', padding);
        return true;
    }

    /// write_stored_string_tchar: converts between the string's width and the
    /// output's width through the "C" locale.
    void write_stored_string() {
        if (str_.empty()) return;
        if (!r_.wide) {
            if (!wide_str_) {
                write_string(str_.data(), static_cast<int>(str_.size()));
                return;
            }
            for (u16 wc : str_) {
                // wctomb, "C" locale: only U+0000..U+00FF have a byte.
                if (wc > 0xFF) {
                    set_errno(kEILSEQ);
                    written_ = -1;
                    return;
                }
                const u16 b = wc;
                write_string(&b, 1);
            }
            return;
        }
        if (wide_str_) {
            write_string(str_.data(), static_cast<int>(str_.size()));
            return;
        }
        for (u16 b : str_) {
            // mbtowc, "C" locale: every byte maps to U+00xx; a NUL byte
            // converts to length 0, which the processor treats as failure.
            if (b == 0) {
                written_ = -1;
                return;
            }
            write_char(b);
        }
    }

    bool type_c() {
        const bool wide_char = is_wide_spec(fc_char_, length_);
        u64 v = 0;
        if (!extract(ArgType::Int32, v)) return false;
        if (!should_format()) return true;
        const u16 unit = static_cast<u16>(v);
        if (!r_.wide) {
            if (wide_char) {
                if (unit > 0xFF) {
                    set_errno(kEILSEQ);
                    suppress_ = true;
                    return true;
                }
                str_.push_back(unit);
            } else {
                str_.push_back(static_cast<u16>(unit & 0xFF));
            }
            wide_str_ = false;
            return true;
        }
        str_.push_back(wide_char ? unit : static_cast<u16>(unit & 0xFF));
        wide_str_ = true;
        return true;
    }

    /// Read at most @p max units of a NUL-terminated guest string.
    void read_guest_string(GuestAddr p, bool wide, usize max) {
        auto& mem = m_.memory();
        if (max == static_cast<usize>(0x7FFFFFFF)) {
            const usize n = wide ? guest_wcslen(mem, p) : guest_strlen(mem, p);
            str_.reserve(n);
            for (usize i = 0; i < n; ++i) {
                if (wide) str_.push_back(mem.read_u16(p + i * 2).value_or(0));
                else str_.push_back(mem.read_u8(p + i).value_or(0));
            }
            return;
        }
        for (usize i = 0; i < max; ++i) {
            u16 unit = 0;
            if (wide) {
                auto r = mem.read_u16(p + i * 2);
                if (r.is_err()) break;
                unit = r.value();
            } else {
                auto r = mem.read_u8(p + i);
                if (r.is_err()) break;
                unit = r.value();
            }
            if (unit == 0) break;
            str_.push_back(unit);
        }
    }

    void set_null_string() {
        static const char kNull[] = "(null)";
        for (const char* p = kNull; *p; ++p) str_.push_back(static_cast<u16>(*p));
    }

    bool type_s() {
        u64 p = 0;
        if (!extract(ArgType::Pointer, p)) return false;
        if (!should_format()) return true;
        const usize max = precision_ == -1 ? static_cast<usize>(0x7FFFFFFF)
                                           : static_cast<usize>(precision_);
        const bool wide = is_wide_spec(fc_char_, length_);
        wide_str_ = wide;
        if (p == 0) {
            set_null_string();
            if (str_.size() > max) str_.resize(max);
            return true;
        }
        read_guest_string(static_cast<GuestAddr>(p), wide, max);
        return true;
    }

    bool type_Z() {
        u64 p = 0;
        if (!extract(ArgType::Pointer, p)) return false;
        if (!should_format()) return true;
        GuestAddr buffer = 0;
        u16 length = 0;
        if (p != 0) {
            length = m_.memory().read_u16(static_cast<GuestAddr>(p)).value_or(0);
            const GuestAddr field = static_cast<GuestAddr>(p) + (is64_ ? 8 : 4);
            buffer = is64_ ? m_.memory().read_u64(field).value_or(0)
                           : m_.memory().read_u32(field).value_or(0);
        }
        if (p == 0 || buffer == 0) {
            set_null_string();
            wide_str_ = false;
            return true;
        }
        if (is_wide_spec(fc_char_, length_)) {
            wide_str_ = true;
            const usize n = length / 2;
            for (usize i = 0; i < n; ++i) {
                str_.push_back(m_.memory().read_u16(buffer + i * 2).value_or(0));
            }
        } else {
            wide_str_ = false;
            for (usize i = 0; i < length; ++i) {
                str_.push_back(m_.memory().read_u8(buffer + i).value_or(0));
            }
        }
        return true;
    }

    bool type_float() {
        flags_ |= FL_SIGNED;
        if (positional_ && mode_ == Mode::Positional && pass_ == 1) {
            if (!(type_index_ >= 0 && type_index_ < kArgMax)) {
                fail_invalid();
                return false;
            }
            return store_parameter(type_index_, ArgType::Real64, fc_char_, length_);
        }
        if (!should_format()) return true;
        const u32 t = fc_char_;
        if (precision_ < 0) {
            if (t == 'a' || t == 'A') precision_ = r_.flavor == Flavor::Msvcrt ? 6 : 13;
            else precision_ = 6;
        } else if (precision_ == 0 && (t == 'g' || t == 'G')) {
            precision_ = 1;
        }
        // formatting_buffer: (_CVTBUFSIZE + precision) chars, 1024-byte member
        // buffer below that; an allocation failure caps the precision.
        constexpr i64 kCvtBufSize = 309 + 40;
        i64 count = kCvtBufSize + precision_;
        if (count * 2 > (i64{1} << 28)) {
            precision_ = static_cast<int>(512 - kCvtBufSize);
            count = 512;
        }
        if (count < 512) count = 512;
        fc_.mantissa_cap = static_cast<usize>(count - 1);
        u64 bits = 0;
        if (!extract(ArgType::Real64, bits)) return false;
        std::string s = fp_format(bits, static_cast<char>(t), precision_, fc_);
        if ((flags_ & FL_ALTERNATE) && precision_ == 0) force_decimal_point(s);
        if ((t == 'g' || t == 'G') && !(flags_ & FL_ALTERNATE)) crop_zeroes(s);
        usize start = 0;
        if (!s.empty() && s[0] == '-') {
            flags_ |= FL_NEGATIVE;
            start = 1;
        }
        if (start < s.size()) {
            const char c0 = s[start];
            if (c0 == 'i' || c0 == 'I' || c0 == 'n' || c0 == 'N') {
                flags_ &= ~FL_LEADZERO;
                fc_char_ = 's';
            }
        }
        for (usize i = start; i < s.size(); ++i) str_.push_back(static_cast<u8>(s[i]));
        wide_str_ = false;
        // In the wide processor the formatted digits are already wide chars.
        if (r_.wide) wide_str_ = true;
        return true;
    }

    bool type_p() {
        precision_ = is64_ ? 16 : 8;
        // msvcrt.dll honours a 64-bit size on %p (x86 %llp/%I64p/%jp read 8
        // bytes; measured); the UCRT always reads a pointer.
        const bool wide64 = r_.flavor == Flavor::Msvcrt &&
                            to_integer_size(length_, is64_, r_.flavor) == 8;
        length_ = (is64_ || wide64) ? Length::I64 : Length::I32;
        return type_integer(16, true);
    }

    bool type_integer(unsigned radix, bool capitals) {
        const usize size = to_integer_size(length_, is64_, r_.flavor);
        u64 raw = 0;
        i64 number = 0;
        const bool is_signed = (flags_ & FL_SIGNED) != 0;
        switch (size) {
        case 1:
            if (!extract(ArgType::Int32, raw)) return false;
            number = is_signed ? static_cast<i8>(raw) : static_cast<i64>(static_cast<u8>(raw));
            break;
        case 2:
            if (!extract(ArgType::Int32, raw)) return false;
            number = is_signed ? static_cast<i16>(raw) : static_cast<i64>(static_cast<u16>(raw));
            break;
        case 4:
            if (!extract(ArgType::Int32, raw)) return false;
            number = is_signed ? static_cast<i32>(raw) : static_cast<i64>(static_cast<u32>(raw));
            break;
        case 8:
            if (!extract(ArgType::Int64, raw)) return false;
            number = static_cast<i64>(raw);
            break;
        default:
            fail_invalid();  // "Invalid integer length modifier"
            return false;
        }
        if (!should_format()) return true;
        u64 value = 0;
        if (is_signed && number < 0) {
            value = static_cast<u64>(-(number + 1)) + 1;
            flags_ |= FL_NEGATIVE;
        } else {
            value = static_cast<u64>(number);
        }
        if (precision_ < 0) {
            precision_ = 1;
        } else {
            flags_ &= ~FL_LEADZERO;
        }
        if (value == 0) flags_ &= ~FL_ALTERNATE;
        if (size != 8) value &= 0xFFFFFFFFULL;
        std::vector<u16> digits;
        int prec = precision_;
        while (prec > 0 || value != 0) {
            --prec;
            unsigned d = static_cast<unsigned>(value % radix);
            value /= radix;
            u16 ch = static_cast<u16>(d < 10 ? '0' + d : (capitals ? 'A' : 'a') + d - 10);
            digits.push_back(ch);
        }
        str_.assign(digits.rbegin(), digits.rend());
        if ((flags_ & FL_FORCEOCTAL) && (str_.empty() || str_[0] != '0')) {
            str_.insert(str_.begin(), '0');
        }
        wide_str_ = r_.wide;
        return true;
    }

    bool type_n() {
        u64 p = 0;
        if (!extract(ArgType::Pointer, p)) return false;
        if (!should_format()) return true;
        if (!r_.count_output_enabled) {
            fail_invalid();  // "'n' format specifier disabled"
            return false;
        }
        const GuestAddr a = static_cast<GuestAddr>(p);
        switch (to_integer_size(length_, is64_, r_.flavor)) {
        case 1: (void)m_.memory().write_u8(a, static_cast<u8>(written_)); break;
        case 2: (void)m_.memory().write_u16(a, static_cast<u16>(written_)); break;
        case 4: (void)m_.memory().write_u32(a, static_cast<u32>(written_)); break;
        case 8: (void)m_.memory().write_u64(a, static_cast<u64>(static_cast<i64>(written_))); break;
        default:
            fail_invalid();
            return false;
        }
        suppress_ = true;
        return true;
    }

    Marshaler& m_;
    const Request& r_;
    std::vector<u32> fmt_;
    ArgCursor args_;
    Target& out_;
    bool is64_;
    bool validating_{false};
    bool positional_{false};
    FloatContext fc_{Flavor::Ucrt, 0, false, Rounding::Legacy, 0, false, 511};

    usize pos_{0};
    int pass_{0};
    int written_{0};
    State state_{State::Normal};
    u32 fc_char_{0};
    unsigned flags_{0};
    int width_{0};
    int precision_{-1};
    Length length_{Length::None};
    bool suppress_{false};
    std::vector<u16> str_;
    bool wide_str_{false};

    Mode mode_{Mode::Unknown};
    int max_index_{-1};
    int type_index_{-1};
    std::vector<Parameter> params_{static_cast<usize>(kArgMax)};
};

} // namespace

u64 ArgCursor::take(Marshaler& m) {
    const usize i = m_index++;
    if (!m_va) return m.arg(m_first + i);
    if (m.cpu_is_64bit()) return m.memory().read_u64(m_base + i * 8).value_or(0);
    return m.memory().read_u32(m_base + i * 4).value_or(0);
}

int format(Marshaler& m, const Request& request, GuestAddr format_addr,
           ArgCursor args, Target& out) {
    std::vector<u32> fmt;
    auto& mem = m.memory();
    const usize n = request.wide ? guest_wcslen(mem, format_addr)
                                 : guest_strlen(mem, format_addr);
    fmt.reserve(n + 1);
    for (usize i = 0; i < n; ++i) {
        fmt.push_back(request.wide ? mem.read_u16(format_addr + i * 2).value_or(0)
                                   : mem.read_u8(format_addr + i).value_or(0));
    }
    fmt.push_back(0);
    OutputProcessor processor(m, request, std::move(fmt), args, out);
    return processor.run();
}

bool printf_count_output_enabled() noexcept { return g_count_output.load(); }

bool exchange_printf_count_output(bool enabled) noexcept {
    return g_count_output.exchange(enabled);
}

u32 msvcrt_output_format() noexcept { return g_msvcrt_output_format.load(); }

u32 exchange_msvcrt_output_format(u32 format) noexcept {
    return g_msvcrt_output_format.exchange(format);
}

void store_units(Marshaler& m, GuestAddr buffer, const std::vector<u16>& units,
                 bool wide) {
    if (units.empty()) return;
    if (wide) {
        std::vector<Byte> bytes(units.size() * 2);
        for (usize i = 0; i < units.size(); ++i) {
            bytes[i * 2] = static_cast<Byte>(units[i] & 0xFF);
            bytes[i * 2 + 1] = static_cast<Byte>(units[i] >> 8);
        }
        (void)m.memory().write_bytes(buffer, bytes.data(), bytes.size());
        return;
    }
    std::vector<Byte> bytes(units.size());
    for (usize i = 0; i < units.size(); ++i) bytes[i] = static_cast<Byte>(units[i]);
    (void)m.memory().write_bytes(buffer, bytes.data(), bytes.size());
}

void store_nul(Marshaler& m, GuestAddr buffer, usize index, bool wide) {
    if (wide) (void)m.memory().write_u16(buffer + index * 2, 0);
    else (void)m.memory().write_u8(buffer + index, 0);
}

} // namespace vitreo::winapi::crt_printf
