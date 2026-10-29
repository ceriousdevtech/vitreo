/// @file crt_math.cpp
/// @brief msvcrt math functions (WIN32-083)

#include "winapi/msvcrt/math/crt_math.hpp"
#include "winapi/msvcrt/math/crt_math_exact.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "core/math/x87_transcendental.hpp"
#include "winapi/core/marshaler.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace vitreo::winapi {

namespace {

/// x87's "indefinite" QNaN, which has the sign bit SET.
///
/// Every invalid operation on Windows yields this exact bit pattern --
/// measured for acos(2), asin(2), log(-1), log10(-1), sqrt(-1), pow(-1,0.5)
/// and fmod(1,0) alike, and for raw 0/0 and inf-inf. Vitreo's arithmetic
/// already produced it; the CRT math thunks handed back the host's POSITIVE
/// default NaN (7ff8...) instead, so a guest comparing the raw bits, hashing
/// a result, or serialising one saw a different value than on Windows.
[[nodiscard]] inline double x87_indefinite() noexcept {
    const u64 bits = 0xFFF8000000000000ull;
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}

[[nodiscard]] inline float x87_indefinite_f() noexcept {
    const u32 bits = 0xFFC00000u;
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

/// Give a freshly-produced NaN the x87 sign. A NaN that came IN is propagated
/// untouched -- only newly generated invalid results take the indefinite.
[[nodiscard]] inline double win_nan(double result, double a,
                                    double b = 0.0) noexcept {
    if (!std::isnan(result)) return result;
    if (std::isnan(a) || std::isnan(b)) return result;
    return x87_indefinite();
}

[[nodiscard]] inline float win_nanf(float result, float a,
                                    float b = 0.0f) noexcept {
    if (!std::isnan(result)) return result;
    if (std::isnan(a) || std::isnan(b)) return result;
    return x87_indefinite_f();
}

// corecrt_math.h / Learn "Math Error Constants"
constexpr int kMathDomain = 1;     // _DOMAIN
constexpr int kMathSing = 2;       // _SING
constexpr int kMathOverflow = 3;   // _OVERFLOW
constexpr int kMathUnderflow = 4;  // _UNDERFLOW
constexpr u32 kEdome = 33;         // EDOM
constexpr u32 kErange = 34;        // ERANGE

} // namespace

GuestAddr CrtMathManager::matherr_scratch(CrtMemoryManager& mem) {
    if (m_matherr_scratch == 0) {
        // x64 struct _exception is 40 bytes; x86 is 32. Over-allocate once.
        m_matherr_scratch = mem.crt_malloc(40);
    }
    return m_matherr_scratch;
}


// =============================================================================
// Trigonometric
// =============================================================================

f64 CrtMathManager::crt_sin(f64 x) { return std::sin(x); }
f64 CrtMathManager::crt_cos(f64 x) { return std::cos(x); }
f64 CrtMathManager::crt_tan(f64 x) { return exact::tan(x); }
f64 CrtMathManager::crt_asin(f64 x) { return exact::asin(x); }
f64 CrtMathManager::crt_acos(f64 x) { return exact::acos(x); }
f64 CrtMathManager::crt_atan(f64 x) { return exact::atan(x); }
f64 CrtMathManager::crt_atan2(f64 y, f64 x) { return std::atan2(y, x); }

// =============================================================================
// Power / Exponential
// =============================================================================

f64 CrtMathManager::crt_sqrt(f64 x) { return std::sqrt(x); }
f64 CrtMathManager::crt_pow(f64 x, f64 y) { return std::pow(x, y); }
f64 CrtMathManager::crt_exp(f64 x) { return std::exp(x); }
f64 CrtMathManager::crt_log(f64 x) { return std::log(x); }
f64 CrtMathManager::crt_log10(f64 x) { return std::log10(x); }
f64 CrtMathManager::crt_ldexp(f64 x, i32 exp) { return std::ldexp(x, exp); }

f64 CrtMathManager::crt_frexp(f64 x, i32& exp) {
    return std::frexp(x, &exp);
}

// =============================================================================
// Rounding
// =============================================================================

f64 CrtMathManager::crt_floor(f64 x) { return std::floor(x); }
f64 CrtMathManager::crt_ceil(f64 x) { return std::ceil(x); }
f64 CrtMathManager::crt_fmod(f64 x, f64 y) { return std::fmod(x, y); }

f64 CrtMathManager::crt_modf(f64 x, f64& iptr) {
    return std::modf(x, &iptr);
}

// =============================================================================
// Utility
// =============================================================================

f64 CrtMathManager::crt_fabs(f64 x) { return std::fabs(x); }
f64 CrtMathManager::crt_hypot(f64 x, f64 y) { return std::hypot(x, y); }
f64 CrtMathManager::crt_copysign(f64 x, f64 y) { return std::copysign(x, y); }
i32 CrtMathManager::crt_isnan(f64 x) { return std::isnan(x) ? 1 : 0; }
i32 CrtMathManager::crt_finite(f64 x) { return std::isfinite(x) ? 1 : 0; }

// =============================================================================
// Float variants
// =============================================================================

f32 CrtMathManager::crt_sinf(f32 x) { return std::sin(x); }
f32 CrtMathManager::crt_cosf(f32 x) { return std::cos(x); }
f32 CrtMathManager::crt_sqrtf(f32 x) { return std::sqrt(x); }
f32 CrtMathManager::crt_powf(f32 x, f32 y) { return std::pow(x, y); }
f32 CrtMathManager::crt_fabsf(f32 x) { return std::fabs(x); }
f32 CrtMathManager::crt_floorf(f32 x) { return std::floor(x); }
f32 CrtMathManager::crt_ceilf(f32 x) { return std::ceil(x); }

// =============================================================================
// Thunk exports
// =============================================================================

DllExports create_crt_math_thunks(CrtMathManager& mgr, CrtMemoryManager& mem) {
    DllExports exports;

    // --- Floating-point argument and return marshalling ----------------------
    //
    // The two calling conventions disagree completely about where a floating
    // -point value lives, so every helper below has to branch on guest
    // bitness:
    //
    //   x86 cdecl : arguments are pushed on the stack, so a double occupies
    //               TWO 32-bit slots (lo then hi) and a float occupies one.
    //               The result comes back on the x87 stack in ST(0).
    //   x64       : the first four floating-point arguments arrive in XMM0-3
    //               and the result is returned in XMM0. The x87 stack is not
    //               involved at all.
    //
    // Indices below are always expressed in x86 stack slots, which is what the
    // call sites use: a two-double function reads slots 0 and 2. That makes the
    // x64 register index simply slot/2 -- and because the x64 integer and
    // floating-point argument positions are shared, the same slot/2 mapping
    // also converts the trailing int/pointer slot of ldexp, frexp and modf.
    const auto arg_slot = [](const Marshaler& m, usize i) -> usize {
        return m.cpu_is_64bit() ? i / 2 : i;
    };

    auto read_dbl = [](Marshaler& m, usize i) -> double {
        u64 bits = 0;
        if (m.cpu_is_64bit()) {
            if (!m.has_cpu()) return 0.0;
            bits = m.cpu()->sse.xmm[i / 2].u64_[0];
        } else {
            const u32 lo = static_cast<u32>(m.arg(i));
            const u32 hi = static_cast<u32>(m.arg(i + 1));
            bits = (static_cast<u64>(hi) << 32) | lo;
        }
        double val;
        std::memcpy(&val, &bits, 8);
        return val;
    };

    auto push_dbl = [](Marshaler& m, double result) {
        if (!m.has_cpu()) return;
        if (m.cpu_is_64bit()) {
            u64 bits = 0;
            std::memcpy(&bits, &result, sizeof(bits));
            m.cpu()->sse.xmm[0].u64_[0] = bits;
            m.cpu()->sse.xmm[0].u64_[1] = 0;
            return;
        }
        m.cpu()->fpu.push(cpu::Float80(result));
    };

    auto read_flt = [](Marshaler& m, usize i) -> float {
        u32 bits = 0;
        if (m.cpu_is_64bit()) {
            if (!m.has_cpu()) return 0.0f;
            bits = m.cpu()->sse.xmm[i].u32_[0];
        } else {
            bits = static_cast<u32>(m.arg(i));
        }
        float val;
        std::memcpy(&val, &bits, 4);
        return val;
    };

    // A float result is returned in the LOW 32 bits of XMM0, not as a double:
    // widening it first would hand the caller a bit pattern it never reads
    // correctly. On x86 the x87 stack carries the value, as for a double.
    auto push_flt = [](Marshaler& m, float result) {
        if (!m.has_cpu()) return;
        if (m.cpu_is_64bit()) {
            u32 bits = 0;
            std::memcpy(&bits, &result, sizeof(bits));
            m.cpu()->sse.xmm[0].u64_[0] = bits;
            m.cpu()->sse.xmm[0].u64_[1] = 0;
            return;
        }
        m.cpu()->fpu.push(cpu::Float80(result));
    };

    // --- _matherr / errno (UCRT, measured by crt_matherr_probe) ---------------
    //
    // When no user handler is installed via __setusermatherr, domain/range
    // errors only set errno (EDOM or ERANGE). With a handler: call it with a
    // guest struct _exception; return 0 → set errno; nonzero → leave errno
    // alone and, for _DOMAIN only (oracle), adopt e->retval. _UNDERFLOW calls
    // the handler but never sets errno. Some exports (pow, exp2) set errno
    // without ever calling _matherr.
    auto ensure_matherr_name = [&mgr, &mem](Marshaler& m,
                                            const char* name) -> GuestAddr {
        auto& names = mgr.matherr_names();
        if (auto it = names.find(name); it != names.end()) return it->second;
        const usize len = std::strlen(name) + 1;
        const GuestAddr addr = mem.crt_malloc(len);
        if (addr == 0) return 0;
        (void)m.memory().write_bytes(addr, reinterpret_cast<const u8*>(name),
                                     len);
        names.emplace(name, addr);
        return addr;
    };

    auto write_exception = [](Marshaler& m, GuestAddr base, bool is64, int type,
                              GuestAddr name, double a1, double a2,
                              double retval) {
        (void)m.write_u32(base, static_cast<u32>(type));
        if (is64) {
            (void)m.write_u64(base + 8, name);
            u64 b1 = 0, b2 = 0, br = 0;
            std::memcpy(&b1, &a1, 8);
            std::memcpy(&b2, &a2, 8);
            std::memcpy(&br, &retval, 8);
            (void)m.write_u64(base + 16, b1);
            (void)m.write_u64(base + 24, b2);
            (void)m.write_u64(base + 32, br);
        } else {
            (void)m.write_u32(base + 4, static_cast<u32>(name));
            u64 b1 = 0, b2 = 0, br = 0;
            std::memcpy(&b1, &a1, 8);
            std::memcpy(&b2, &a2, 8);
            std::memcpy(&br, &retval, 8);
            (void)m.write_u32(base + 8, static_cast<u32>(b1));
            (void)m.write_u32(base + 12, static_cast<u32>(b1 >> 32));
            (void)m.write_u32(base + 16, static_cast<u32>(b2));
            (void)m.write_u32(base + 20, static_cast<u32>(b2 >> 32));
            (void)m.write_u32(base + 24, static_cast<u32>(br));
            (void)m.write_u32(base + 28, static_cast<u32>(br >> 32));
        }
    };

    auto read_exception_retval = [](Marshaler& m, GuestAddr base,
                                    bool is64) -> double {
        u64 br = 0;
        if (is64) {
            br = m.memory().read_u64(base + 32).value_or(0);
        } else {
            const u32 lo = m.memory().read_u32(base + 24).value_or(0);
            const u32 hi = m.memory().read_u32(base + 28).value_or(0);
            br = (static_cast<u64>(hi) << 32) | lo;
        }
        double v = 0.0;
        std::memcpy(&v, &br, 8);
        return v;
    };

    auto raise_math = [&mgr, &mem, ensure_matherr_name, write_exception,
                       read_exception_retval](Marshaler& m, int type,
                                              const char* name, double a1,
                                              double a2,
                                              double retval) -> double {
        double out = retval;
        int handled = 0;
        const GuestAddr handler = mgr.user_matherr();
        if (handler != 0 && mem.has_guest_caller()) {
            const GuestAddr scratch = mgr.matherr_scratch(mem);
            const GuestAddr name_addr = ensure_matherr_name(m, name);
            if (scratch != 0 && name_addr != 0) {
                const bool is64 = m.cpu_is_64bit();
                write_exception(m, scratch, is64, type, name_addr, a1, a2,
                                retval);
                const u64 args[1] = {scratch};
                handled = static_cast<int>(mem.call_guest(handler, args, 1));
                if (handled != 0) {
                    // Oracle: only _DOMAIN adopts a modified e->retval
                    // (sqrt(-1)→42); _SING/_OVERFLOW keep the computed value.
                    if (type == kMathDomain) {
                        out = read_exception_retval(m, scratch, is64);
                    }
                }
            }
        }
        if (handled == 0) {
            if (type == kMathDomain) {
                mem.set_errno(kEdome);
            } else if (type == kMathSing || type == kMathOverflow) {
                mem.set_errno(kErange);
            }
            // _UNDERFLOW: handler may run, but errno stays unset (oracle).
        }
        return out;
    };

    /// errno-only path for exports that never call _matherr (pow, exp2).
    auto set_math_errno = [&mem](int type) {
        if (type == kMathDomain) mem.set_errno(kEdome);
        else if (type == kMathSing || type == kMathOverflow) mem.set_errno(kErange);
    };


    // NOTE on sin/cos/tan at large arguments, measured not assumed:
    // Windows' 32-bit CRT computes these accurately below |x| ~= 90112 and
    // switches to raw x87 (with its 66-bit truncated pi) above it. Verified on
    // real x86 silicon: past that bound CRT sin returns FSIN's exact bits,
    // truncated low bits and all. Against 260-bit references Windows is the
    // WRONG side there and libm is right, and the switch-over point is an
    // empirically probed number rather than a documented one -- so matching it
    // would trade real accuracy for fidelity to an undocumented magic bound.
    // Left accurate deliberately; see the x87 note below for where the
    // hardware semantics ARE reproduced.
    //
    // The _CI* entries are different: those ARE the x87 stack intrinsics, so
    // they take the hardware's semantics unconditionally.

    // --- Single-arg double functions (arg at arg(0)/arg(1), return in ST(0)) ---
    exports.entries.push_back({"sin", 1, [&mgr, read_dbl, push_dbl](Marshaler& m) {
        (void)mgr; push_dbl(m, std::sin(read_dbl(m, 0)));
    }});
    exports.entries.push_back({"cos", 2, [&mgr, read_dbl, push_dbl](Marshaler& m) {
        (void)mgr; push_dbl(m, std::cos(read_dbl(m, 0)));
    }});
    exports.entries.push_back({"tan", 3, [&mgr, read_dbl, push_dbl](Marshaler& m) {
        (void)mgr; push_dbl(m, exact::tan(read_dbl(m, 0)));
    }});
    exports.entries.push_back({"asin", 4, [&mgr, read_dbl, push_dbl, raise_math](Marshaler& m) {
        (void)mgr;
        const double x = read_dbl(m, 0);
        double r = win_nan(exact::asin(x), x);
        if (!std::isnan(x) && std::fabs(x) > 1.0) {
            r = raise_math(m, kMathDomain, "asin", x, 0.0, x87_indefinite());
        }
        push_dbl(m, r);
    }});
    exports.entries.push_back({"acos", 5, [&mgr, read_dbl, push_dbl, raise_math](Marshaler& m) {
        (void)mgr;
        const double x = read_dbl(m, 0);
        double r = win_nan(exact::acos(x), x);
        if (!std::isnan(x) && std::fabs(x) > 1.0) {
            r = raise_math(m, kMathDomain, "acos", x, 0.0, x87_indefinite());
        }
        push_dbl(m, r);
    }});
    exports.entries.push_back({"atan", 6, [&mgr, read_dbl, push_dbl](Marshaler& m) {
        (void)mgr; push_dbl(m, exact::atan(read_dbl(m, 0)));
    }});
    // The hyperbolics existed only as the _CI* x87-stack intrinsics, so a
    // guest importing the ordinary cdecl entry (api-ms-win-crt-math-l1-1-0)
    // hit the missing-export stub and got 0xFFFFFFFF reinterpreted as a
    // double. Measured against Windows by tests/crt_oracle/crt_surface_runner:
    // cosh/sinh/tanh accounted for 55 of 105 value divergences.
    exports.entries.push_back({"cosh", 0, [&mgr, read_dbl, push_dbl](Marshaler& m) {
        (void)mgr; push_dbl(m, std::cosh(read_dbl(m, 0)));
    }});
    exports.entries.push_back({"sinh", 0, [&mgr, read_dbl, push_dbl](Marshaler& m) {
        (void)mgr; push_dbl(m, std::sinh(read_dbl(m, 0)));
    }});
    exports.entries.push_back({"tanh", 0, [&mgr, read_dbl, push_dbl](Marshaler& m) {
        (void)mgr; push_dbl(m, std::tanh(read_dbl(m, 0)));
    }});

    // div/ldiv return an 8-byte struct { int quot; int rem; }, which the
    // 32-bit MSVC ABI returns in EDX:EAX -- quot at offset 0 (EAX), rem at
    // offset 4 (EDX). Both were missing entirely, so a guest calling them got
    // the 0xFFFFFFFF stub in EAX and stale EDX. C99 truncates toward zero,
    // which is what the host operators already do.
    auto emit_div = [](Marshaler& m, long long numer, long long denom) {
        if (denom == 0) {          // Windows raises; do not fabricate a value
            m.set_return_i64(0);   // and do not divide by zero here
            return;
        }
        const long long q = numer / denom;
        const long long r = numer % denom;
        m.set_return_i64((static_cast<u64>(static_cast<u32>(r)) << 32) |
                         static_cast<u64>(static_cast<u32>(q)));
    };
    exports.entries.push_back({"div", 0, [emit_div](Marshaler& m) {
        emit_div(m, static_cast<i32>(m.arg(0)), static_cast<i32>(m.arg(1)));
    }});
    exports.entries.push_back({"ldiv", 0, [emit_div](Marshaler& m) {
        emit_div(m, static_cast<i32>(m.arg(0)), static_cast<i32>(m.arg(1)));
    }});

    // --- Two-arg double functions (first: arg(0)/arg(1), second: arg(2)/arg(3)) ---
    exports.entries.push_back({"atan2", 7, [&mgr, read_dbl, push_dbl](Marshaler& m) {
        (void)mgr;
        push_dbl(m, win_nan(std::atan2(read_dbl(m, 0), read_dbl(m, 2)),
                            read_dbl(m, 0), read_dbl(m, 2)));
    }});
    exports.entries.push_back({"sqrt", 8, [&mgr, read_dbl, push_dbl, raise_math](Marshaler& m) {
        (void)mgr;
        const double x = read_dbl(m, 0);
        double r = win_nan(std::sqrt(x), x);
        // Negative (not -0) → _DOMAIN. Oracle: sqrt(-0) is quiet.
        if (!std::isnan(x) && x < 0.0) {
            r = raise_math(m, kMathDomain, "sqrt", x, 0.0, x87_indefinite());
        }
        push_dbl(m, r);
    }});
    exports.entries.push_back({"pow", 9, [&mgr, read_dbl, push_dbl, set_math_errno](Marshaler& m) {
        (void)mgr;
        const double x = read_dbl(m, 0);
        const double y = read_dbl(m, 2);
        double r = win_nan(std::pow(x, y), x, y);
        // Oracle: pow never calls _matherr; it only sets errno.
        if (!std::isnan(x) && !std::isnan(y)) {
            if (x < 0.0 && y != std::floor(y)) {
                r = x87_indefinite();
                set_math_errno(kMathDomain);
            } else if (x == 0.0 && y < 0.0) {
                r = (std::signbit(x) ? -1.0 : 1.0) *
                    std::numeric_limits<double>::infinity();
                set_math_errno(kMathSing);
            }
        }
        push_dbl(m, r);
    }});
    exports.entries.push_back({"exp", 10, [&mgr, read_dbl, push_dbl, raise_math](Marshaler& m) {
        (void)mgr;
        const double x = read_dbl(m, 0);
        double r = std::exp(x);
        if (std::isfinite(x) && std::isinf(r)) {
            r = raise_math(m, kMathOverflow, "exp", x, 0.0, r);
        } else if (std::isfinite(x) && r == 0.0 && x < 0.0) {
            r = raise_math(m, kMathUnderflow, "exp", x, 0.0, r);
        }
        push_dbl(m, r);
    }});
    exports.entries.push_back({"log", 11, [&mgr, read_dbl, push_dbl, raise_math](Marshaler& m) {
        (void)mgr;
        const double x = read_dbl(m, 0);
        double r = win_nan(std::log(x), x);
        if (!std::isnan(x)) {
            if (x < 0.0) {
                r = raise_math(m, kMathDomain, "log", x, 0.0, x87_indefinite());
            } else if (x == 0.0) {
                r = raise_math(m, kMathSing, "log", x, 0.0,
                               -std::numeric_limits<double>::infinity());
            }
        }
        push_dbl(m, r);
    }});
    exports.entries.push_back({"log10", 12, [&mgr, read_dbl, push_dbl, raise_math](Marshaler& m) {
        (void)mgr;
        const double x = read_dbl(m, 0);
        double r = win_nan(std::log10(x), x);
        if (!std::isnan(x)) {
            if (x < 0.0) {
                r = raise_math(m, kMathDomain, "log10", x, 0.0, x87_indefinite());
            } else if (x == 0.0) {
                r = raise_math(m, kMathSing, "log10", x, 0.0,
                               -std::numeric_limits<double>::infinity());
            }
        }
        push_dbl(m, r);
    }});

    // ldexp(double x, int exp) — x at arg(0)/arg(1), exp at arg(2)
    exports.entries.push_back({"ldexp", 13, [&mgr, read_dbl, push_dbl, arg_slot](Marshaler& m) {
        (void)mgr;
        double x = read_dbl(m, 0);
        i32 exp = static_cast<i32>(m.arg(arg_slot(m, 2)));
        push_dbl(m, std::ldexp(x, exp));
    }});

    // frexp(double x, int* exp) — x at arg(0)/arg(1), exp_ptr at arg(2)
    exports.entries.push_back({"frexp", 14, [&mgr, read_dbl, push_dbl, arg_slot](Marshaler& m) {
        (void)mgr;
        double x = read_dbl(m, 0);
        GuestAddr exp_ptr = m.arg_ptr(arg_slot(m, 2));
        int exp_val = 0;
        double result = std::frexp(x, &exp_val);
        push_dbl(m, result);
        if (exp_ptr) {
            (void)m.write_u32(exp_ptr, static_cast<u32>(static_cast<i32>(exp_val)));
        }
    }});

    exports.entries.push_back({"floor", 15, [&mgr, read_dbl, push_dbl](Marshaler& m) {
        (void)mgr; push_dbl(m, std::floor(read_dbl(m, 0)));
    }});
    exports.entries.push_back({"ceil", 16, [&mgr, read_dbl, push_dbl](Marshaler& m) {
        (void)mgr; push_dbl(m, std::ceil(read_dbl(m, 0)));
    }});
    exports.entries.push_back({"fmod", 17, [&mgr, read_dbl, push_dbl](Marshaler& m) {
        (void)mgr;
        push_dbl(m, win_nan(std::fmod(read_dbl(m, 0), read_dbl(m, 2)),
                            read_dbl(m, 0), read_dbl(m, 2)));
    }});

    // modf(double x, double* iptr) — x at arg(0)/arg(1), iptr at arg(2)
    exports.entries.push_back({"modf", 18, [&mgr, read_dbl, push_dbl, arg_slot](Marshaler& m) {
        (void)mgr;
        double x = read_dbl(m, 0);
        GuestAddr iptr = m.arg_ptr(arg_slot(m, 2));
        double int_part = 0.0;
        double result = std::modf(x, &int_part);
        push_dbl(m, result);
        if (iptr) {
            u64 bits;
            std::memcpy(&bits, &int_part, 8);
            (void)m.write_u32(iptr, static_cast<u32>(bits));
            (void)m.write_u32(iptr + 4, static_cast<u32>(bits >> 32));
        }
    }});

    exports.entries.push_back({"fabs", 19, [&mgr, read_dbl, push_dbl](Marshaler& m) {
        (void)mgr; push_dbl(m, std::fabs(read_dbl(m, 0)));
    }});
    exports.entries.push_back({"_hypot", 20, [&mgr, read_dbl, push_dbl](Marshaler& m) {
        (void)mgr; push_dbl(m, std::hypot(read_dbl(m, 0), read_dbl(m, 2)));
    }});
    exports.entries.push_back({"_copysign", 21, [&mgr, read_dbl, push_dbl](Marshaler& m) {
        (void)mgr; push_dbl(m, std::copysign(read_dbl(m, 0), read_dbl(m, 2)));
    }});

    // _isnan and _finite return int in EAX (not FPU)
    exports.entries.push_back({"_isnan", 22, [&mgr, read_dbl](Marshaler& m) {
        (void)mgr; m.set_return(std::isnan(read_dbl(m, 0)) ? 1u : 0u);
    }});
    exports.entries.push_back({"_finite", 23, [&mgr, read_dbl](Marshaler& m) {
        (void)mgr; m.set_return(std::isfinite(read_dbl(m, 0)) ? 1u : 0u);
    }});

    // int _fpclass(double x) — MSVC float.h classification bits.
    exports.entries.push_back({"_fpclass", 0, [&mgr, read_dbl](Marshaler& m) {
        (void)mgr;
        constexpr int kSNan = 0x0001;
        constexpr int kQNan = 0x0002;
        constexpr int kNInf = 0x0004;
        constexpr int kNN = 0x0008;
        constexpr int kND = 0x0010;
        constexpr int kNZ = 0x0020;
        constexpr int kPZ = 0x0040;
        constexpr int kPD = 0x0080;
        constexpr int kPN = 0x0100;
        constexpr int kPInf = 0x0200;
        const double x = read_dbl(m, 0);
        int cls = kPN;
        switch (std::fpclassify(x)) {
            case FP_NAN: {
                // MSVC: signaling NaN vs quiet — bit 51 clear ⇒ signaling on
                // IEEE754 binary64 (heuristic matching CRT).
                u64 bits = 0;
                std::memcpy(&bits, &x, sizeof(bits));
                cls = ((bits & (1ull << 51)) == 0) ? kSNan : kQNan;
                break;
            }
            case FP_INFINITE:
                cls = std::signbit(x) ? kNInf : kPInf;
                break;
            case FP_ZERO:
                cls = std::signbit(x) ? kNZ : kPZ;
                break;
            case FP_SUBNORMAL:
                cls = std::signbit(x) ? kND : kPD;
                break;
            case FP_NORMAL:
            default:
                cls = std::signbit(x) ? kNN : kPN;
                break;
        }
        m.set_return(static_cast<u32>(cls));
    }});

    // --- Float (f32) variants — arg is single float, return in ST(0) ---
    exports.entries.push_back({"sinf", 24, [read_flt, push_flt](Marshaler& m) {
        const float x = read_flt(m, 0);
        push_flt(m, win_nanf(std::sin(x), x));
    }});
    exports.entries.push_back({"cosf", 25, [read_flt, push_flt](Marshaler& m) {
        const float x = read_flt(m, 0);
        push_flt(m, win_nanf(std::cos(x), x));
    }});
    exports.entries.push_back({"sqrtf", 26, [&mgr, read_flt, push_flt, raise_math](Marshaler& m) {
        (void)mgr;
        const float x = read_flt(m, 0);
        float r = win_nanf(std::sqrt(x), x);
        if (!std::isnan(x) && x < 0.0f) {
            // Exception args are promoted to double (oracle).
            const double out = raise_math(m, kMathDomain, "sqrtf",
                                          static_cast<double>(x), 0.0,
                                          x87_indefinite());
            r = static_cast<float>(out);
            if (std::isnan(out)) r = x87_indefinite_f();
        }
        push_flt(m, r);
    }});
    exports.entries.push_back({"powf", 27, [&mgr, read_flt, push_flt, set_math_errno](Marshaler& m) {
        (void)mgr;
        const float x = read_flt(m, 0);
        const float y = read_flt(m, 1);
        float r = win_nanf(std::pow(x, y), x, y);
        if (!std::isnan(x) && !std::isnan(y)) {
            if (x < 0.0f && y != std::floor(y)) {
                r = x87_indefinite_f();
                set_math_errno(kMathDomain);
            } else if (x == 0.0f && y < 0.0f) {
                r = std::numeric_limits<float>::infinity();
                set_math_errno(kMathSing);
            }
        }
        push_flt(m, r);
    }});
    exports.entries.push_back({"fabsf", 28, [&mgr, read_flt, push_flt](Marshaler& m) {
        (void)mgr; push_flt(m, std::fabs(read_flt(m, 0)));
    }});
    exports.entries.push_back({"floorf", 29, [&mgr, read_flt, push_flt](Marshaler& m) {
        (void)mgr; push_flt(m, std::floor(read_flt(m, 0)));
    }});
    exports.entries.push_back({"ceilf", 30, [&mgr, read_flt, push_flt](Marshaler& m) {
        (void)mgr; push_flt(m, std::ceil(read_flt(m, 0)));
    }});

    // __setusermatherr — install user `_matherr` (Learn / corecrt_math.h).
    // Returning the previous handler is not part of the documented contract
    // (void return); we only store the new one.
    exports.entries.push_back(ExportEntry{
        "__setusermatherr", 0,
        [&mgr](Marshaler& m) {
            (void)mgr.exchange_user_matherr(m.arg_ptr(0));
        }
    });

    // _CIacos — intrinsic acos, reads ST(0), overwrites with result
    exports.entries.push_back(ExportEntry{
        "_CIacos", 0,
        [](Marshaler& m) {
            if (m.has_cpu()) {
                double val = m.cpu()->fpu.get_st(0).to_double();
                double result = win_nan(exact::acos(val), val);
                m.cpu()->fpu.set_st(0, cpu::Float80(result));
            }
        }
    });

    // _CIpow — intrinsic pow with exponent in ST(0) and base in ST(1).
    exports.entries.push_back(ExportEntry{
        "_CIpow", 0,
        [](Marshaler& m) {
            if (m.has_cpu()) {
                const double exponent = m.cpu()->fpu.get_st(0).to_double();
                const double base = m.cpu()->fpu.get_st(1).to_double();
                (void)m.cpu()->fpu.pop();
                m.cpu()->fpu.set_st(0, cpu::Float80(
                    win_nan(std::pow(base, exponent), base, exponent)));
            }
        }
    });

    // --- The remaining _CI* FPU intrinsics -----------------------------------
    //
    // MSVC's /Oi math intrinsics take their arguments on the x87 stack and
    // leave the result in ST(0): one-argument forms replace ST(0), two-argument
    // forms take the *second* argument in ST(0) and the first in ST(1), then
    // pop. There is no stack argument and no EAX result.
    struct CiUnary {
        const char* name;
        double (*fn)(double);
    };
    static constexpr CiUnary CI_UNARY[] = {
        {"_CIcosh",  [](double x) { return std::cosh(x);  }},
        {"_CIsinh",  [](double x) { return std::sinh(x);  }},
        {"_CItanh",  [](double x) { return std::tanh(x);  }},
        {"_CIlog",   [](double x) { return std::log(x);   }},
        {"_CIsin",   [](double x) { return win_nan(math::x87_trig_out_of_range(x)
                                       ? std::sin(x) : math::x87_sin(x), x); }},
        {"_CIcos",   [](double x) { return win_nan(math::x87_trig_out_of_range(x)
                                       ? std::cos(x) : math::x87_cos(x), x); }},
        {"_CIsqrt",  [](double x) { return std::sqrt(x);  }},
        {"_CIlog10", [](double x) { return win_nan(std::log10(x), x); }},
        {"_CIatan",  [](double x) { return exact::atan(x);  }},
        {"_CIexp",   [](double x) { return std::exp(x);   }},
        {"_CItan",   [](double x) { return win_nan(math::x87_trig_out_of_range(x)
                                       ? exact::tan(x) : math::x87_tan(x), x); }},
        {"_CIasin",  [](double x) { return exact::asin(x);  }},
    };
    for (const auto& op : CI_UNARY) {
        exports.entries.push_back(ExportEntry{
            op.name, 0,
            [fn = op.fn](Marshaler& m) {
                if (!m.has_cpu()) return;
                const double x = m.cpu()->fpu.get_st(0).to_double();
                // The indefinite-NaN sign is applied once here, at the single
                // dispatch point, rather than in each table entry -- an entry
                // added later cannot then forget it.
                m.cpu()->fpu.set_st(0, cpu::Float80(win_nan(fn(x), x)));
            }
        });
    }

    struct CiBinary {
        const char* name;
        double (*fn)(double, double);
    };
    static constexpr CiBinary CI_BINARY[] = {
        {"_CIatan2", [](double y, double x) { return win_nan(std::atan2(y, x), y, x); }},
        {"_CIfmod",  [](double x, double y) { return win_nan(std::fmod(x, y), x, y);  }},
    };
    for (const auto& op : CI_BINARY) {
        exports.entries.push_back(ExportEntry{
            op.name, 0,
            [fn = op.fn](Marshaler& m) {
                if (!m.has_cpu()) return;
                const double second = m.cpu()->fpu.get_st(0).to_double();
                const double first  = m.cpu()->fpu.get_st(1).to_double();
                (void)m.cpu()->fpu.pop();
                m.cpu()->fpu.set_st(0, cpu::Float80(fn(first, second)));
            }
        });
    }

    // --- _libm_sse2_*_precise ------------------------------------------------
    //
    // The SSE2 math helpers the x86 CRT calls for /fp:precise code. Unlike the
    // _CI* intrinsics they use XMM registers: the argument arrives in XMM0
    // (a second argument, where there is one, in XMM1) and the result is
    // returned in XMM0 — the x87 stack is not involved.
    auto read_xmm_double = [](Marshaler& m, usize index) -> double {
        if (!m.has_cpu()) return 0.0;
        const u64 bits = m.cpu()->sse.xmm[index].u64_[0];
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    auto write_xmm0_double = [](Marshaler& m, double value) {
        if (!m.has_cpu()) return;
        u64 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        m.cpu()->sse.xmm[0].u64_[0] = bits;
        m.cpu()->sse.xmm[0].u64_[1] = 0;
    };

    // --- float-suffixed math (UCRT) ------------------------------------------
    //
    // The `f` variants take and return a *float*, which the MS x64 ABI passes
    // in the low 32 bits of XMM0 and returns the same way. Without them the
    // missing-export stub answers 0xFFFFFFFF in EAX and leaves XMM0 untouched,
    // so the caller reads whatever the register happened to hold — a silent
    // wrong number rather than a failure. UE5 calls logf/truncf constantly.
    //
    // On x86 the same functions are cdecl: the argument arrives on the stack
    // and the result comes back on the x87 stack, so both paths are needed.
    struct FloatUnary {
        const char* name;
        float (*fn)(float);
    };
    static constexpr FloatUnary FLOAT_UNARY[] = {
        {"logbf",  [](float x) { return std::logb(x);  }},
        {"truncf", [](float x) { return std::trunc(x); }},
        {"sinhf",  [](float x) { return std::sinh(x);  }},
        {"coshf",  [](float x) { return std::cosh(x);  }},
        {"tanhf",  [](float x) { return std::tanh(x);  }},
        {"tanf",   [](float x) { return std::tan(x);   }},
        {"atanf",  [](float x) { return std::atan(x);  }},
        {"cbrtf",  [](float x) { return std::cbrt(x);  }},
        {"roundf", [](float x) { return std::round(x); }},
        {"rintf",  [](float x) { return std::rint(x);  }},
        {"nearbyintf", [](float x) { return std::nearbyint(x); }},
        {"log1pf", [](float x) { return std::log1p(x); }},
        // Inverse hyperbolics. Domain-checked variants registered below.
        {"asinhf", [](float x) { return std::asinh(x); }},
    };
    for (const auto& op : FLOAT_UNARY) {
        exports.entries.push_back(ExportEntry{
            op.name, 0,
            [fn = op.fn, read_flt, push_flt](Marshaler& m) {
                push_flt(m, fn(read_flt(m, 0)));
            }
        });
    }

    // Capture raise_math by value so the ExportEntry lambdas do not dangle
    // after create_crt_math_thunks returns.
    auto raise_flt = [raise_math](Marshaler& m, int type, const char* name,
                                  float x, float y, float retval_f) -> float {
        const double out = raise_math(m, type, name, static_cast<double>(x),
                                      static_cast<double>(y),
                                      static_cast<double>(retval_f));
        if (std::isnan(out) && std::isnan(static_cast<double>(retval_f))) {
            return x87_indefinite_f();
        }
        return static_cast<float>(out);
    };

    exports.entries.push_back({"logf", 0, [read_flt, push_flt, raise_flt](Marshaler& m) {
        const float x = read_flt(m, 0);
        float r = win_nanf(std::log(x), x);
        if (!std::isnan(x)) {
            if (x < 0.0f) {
                r = raise_flt(m, kMathDomain, "logf", x, 0.0f, x87_indefinite_f());
            } else if (x == 0.0f) {
                r = raise_flt(m, kMathSing, "logf", x, 0.0f,
                              -std::numeric_limits<float>::infinity());
            }
        }
        push_flt(m, r);
    }});
    exports.entries.push_back({"log10f", 0, [read_flt, push_flt, raise_flt](Marshaler& m) {
        const float x = read_flt(m, 0);
        float r = win_nanf(std::log10(x), x);
        if (!std::isnan(x)) {
            if (x < 0.0f) {
                r = raise_flt(m, kMathDomain, "log10f", x, 0.0f, x87_indefinite_f());
            } else if (x == 0.0f) {
                r = raise_flt(m, kMathSing, "log10f", x, 0.0f,
                              -std::numeric_limits<float>::infinity());
            }
        }
        push_flt(m, r);
    }});
    exports.entries.push_back({"asinf", 0, [read_flt, push_flt, raise_flt](Marshaler& m) {
        const float x = read_flt(m, 0);
        float r = win_nanf(std::asin(x), x);
        if (!std::isnan(x) && std::fabs(x) > 1.0f) {
            r = raise_flt(m, kMathDomain, "asinf", x, 0.0f, x87_indefinite_f());
        }
        push_flt(m, r);
    }});
    exports.entries.push_back({"acosf", 0, [read_flt, push_flt, raise_flt](Marshaler& m) {
        const float x = read_flt(m, 0);
        float r = win_nanf(std::acos(x), x);
        if (!std::isnan(x) && std::fabs(x) > 1.0f) {
            r = raise_flt(m, kMathDomain, "acosf", x, 0.0f, x87_indefinite_f());
        }
        push_flt(m, r);
    }});
    exports.entries.push_back({"expf", 0, [read_flt, push_flt, raise_flt](Marshaler& m) {
        const float x = read_flt(m, 0);
        float r = std::exp(x);
        if (std::isfinite(x) && std::isinf(r)) {
            r = raise_flt(m, kMathOverflow, "expf", x, 0.0f, r);
        } else if (std::isfinite(x) && r == 0.0f && x < 0.0f) {
            r = raise_flt(m, kMathUnderflow, "expf", x, 0.0f, r);
        }
        push_flt(m, r);
    }});
    // Oracle: expm1f reports as "expf" in _matherr.
    exports.entries.push_back({"expm1f", 0, [read_flt, push_flt, raise_flt](Marshaler& m) {
        const float x = read_flt(m, 0);
        float r = std::expm1(x);
        if (std::isfinite(x) && std::isinf(r)) {
            r = raise_flt(m, kMathOverflow, "expf", x, 0.0f,
                          std::numeric_limits<float>::infinity());
        }
        push_flt(m, r);
    }});
    exports.entries.push_back({"exp2f", 0, [read_flt, push_flt, set_math_errno](Marshaler& m) {
        const float x = read_flt(m, 0);
        float r = std::exp2(x);
        if (std::isfinite(x) && std::isinf(r)) {
            set_math_errno(kMathOverflow);
        } else if (std::isfinite(x) && r == 0.0f && x < 0.0f) {
            set_math_errno(kMathUnderflow);
        }
        push_flt(m, r);
    }});
    exports.entries.push_back({"acoshf", 0, [read_flt, push_flt, raise_flt](Marshaler& m) {
        const float x = read_flt(m, 0);
        float r = win_nanf(std::acosh(x), x);
        if (!std::isnan(x) && x < 1.0f) {
            r = raise_flt(m, kMathDomain, "acoshf", x, 0.0f, x87_indefinite_f());
        }
        push_flt(m, r);
    }});
    exports.entries.push_back({"atanhf", 0, [read_flt, push_flt, raise_flt](Marshaler& m) {
        const float x = read_flt(m, 0);
        float r = win_nanf(std::atanh(x), x);
        if (!std::isnan(x)) {
            if (std::fabs(x) > 1.0f) {
                r = raise_flt(m, kMathDomain, "atanhf", x, 0.0f, x87_indefinite_f());
            } else if (std::fabs(x) == 1.0f) {
                r = raise_flt(m, kMathSing, "atanhf", x, 0.0f,
                              std::copysign(std::numeric_limits<float>::infinity(), x));
            }
        }
        push_flt(m, r);
    }});

    // int ilogbf(float) — the unbiased exponent as an int. Distinct from the
    // float-unary family above because the RESULT is an integer.
    exports.entries.push_back(ExportEntry{
        "ilogbf", 0,
        [read_flt](Marshaler& m) {
            m.set_return(static_cast<u64>(
                static_cast<u32>(std::ilogb(read_flt(m, 0)))));
        }
    });
    exports.entries.push_back(ExportEntry{
        "ilogb", 0,
        [](Marshaler& m) {
            const u64 bits = m.arg(0);
            double d = 0.0;
            std::memcpy(&d, &bits, sizeof(d));
            m.set_return(static_cast<u64>(static_cast<u32>(std::ilogb(d))));
        }
    });

    struct FloatBinary {
        const char* name;
        float (*fn)(float, float);
    };
    static constexpr FloatBinary FLOAT_BINARY[] = {
        {"fmodf",      [](float a, float b) { return std::fmod(a, b);  }},
        {"atan2f",     [](float a, float b) { return std::atan2(a, b); }},
        {"hypotf",     [](float a, float b) { return std::hypot(a, b); }},
        {"fdimf",      [](float a, float b) { return std::fdim(a, b);  }},
    };
    // nextafter family (Learn nextafter...). Measured x86 == x64 in
    // tests/oracle_data/nextafter_probe{32,64}.expected: a NaN argument comes
    // back unchanged (x first), equal arguments return y, and errno = ERANGE
    // whenever the result is subnormal, zero or infinite. long double is
    // double on MSVC, so the *l and nexttoward forms are the double ones.
    auto step_dbl = [&mem](double x, double y) -> double {
        if (std::isnan(x)) return x;
        if (std::isnan(y)) return y;
        if (x == y) return y;
        const double r = std::nextafter(x, y);
        if (std::isinf(r) || r == 0.0 || std::fpclassify(r) == FP_SUBNORMAL) mem.set_errno(kErange);
        return r;
    };
    auto step_flt = [&mem](float x, double y) -> float {
        if (std::isnan(x)) return x;
        if (std::isnan(y)) return static_cast<float>(y);
        if (static_cast<double>(x) == y) return static_cast<float>(y);
        const float r = std::nexttoward(x, static_cast<long double>(y));
        if (std::isinf(r) || r == 0.0f || std::fpclassify(r) == FP_SUBNORMAL) mem.set_errno(kErange);
        return r;
    };
    // nextafterf / _nextafterf (x64-only export): the nextafter contract on
    // floats (nextafter_probe{32,64}.expected).
    for (const char* name : {"nextafterf", "_nextafterf"}) {
        exports.entries.push_back({name, 0, [read_flt, push_flt, step_flt](Marshaler& m) {
            push_flt(m, step_flt(read_flt(m, 0), static_cast<double>(read_flt(m, 1))));
        }});
    }
    for (const auto& op : FLOAT_BINARY) {
        exports.entries.push_back(ExportEntry{
            op.name, 0,
            [fn = op.fn, read_flt, push_flt](Marshaler& m) {
                push_flt(m, fn(read_flt(m, 0), read_flt(m, 1)));
            }
        });
    }

    /// float modff(float, float*) - returns the fractional part and stores the
    /// integral part through the pointer. The pointer follows a single float,
    /// so it is stack slot 1 on x86 and integer slot 1 (RDX) on x64: the same
    /// index either way.
    exports.entries.push_back(ExportEntry{
        "modff", 0,
        [read_flt, push_flt](Marshaler& m) {
            float integral = 0.0f;
            const float fractional = std::modf(read_flt(m, 0), &integral);
            const GuestAddr out = m.arg_ptr(1);
            if (out != 0) {
                u32 bits = 0;
                std::memcpy(&bits, &integral, sizeof(bits));
                (void)m.write_u32(out, bits);
            }
            push_flt(m, fractional);
        }
    });

    struct SseUnary {
        const char* name;
        double (*fn)(double);
    };
    static constexpr SseUnary SSE_UNARY[] = {
        {"_libm_sse2_acos_precise",  [](double x) { return exact::acos(x);  }},
        {"_libm_sse2_asin_precise",  [](double x) { return exact::asin(x);  }},
        {"_libm_sse2_atan_precise",  [](double x) { return exact::atan(x);  }},
        {"_libm_sse2_cos_precise",   [](double x) { return std::cos(x);   }},
        {"_libm_sse2_sin_precise",   [](double x) { return std::sin(x);   }},
        {"_libm_sse2_tan_precise",   [](double x) { return exact::tan(x);   }},
        {"_libm_sse2_exp_precise",   [](double x) { return std::exp(x);   }},
        {"_libm_sse2_log_precise",   [](double x) { return std::log(x);   }},
        {"_libm_sse2_log10_precise", [](double x) { return std::log10(x); }},
        {"_libm_sse2_sqrt_precise",  [](double x) { return std::sqrt(x);  }},
    };
    for (const auto& op : SSE_UNARY) {
        exports.entries.push_back(ExportEntry{
            op.name, 0,
            [fn = op.fn, read_xmm_double, write_xmm0_double](Marshaler& m) {
                write_xmm0_double(m, fn(read_xmm_double(m, 0)));
            }
        });
    }
    exports.entries.push_back(ExportEntry{
        "_libm_sse2_pow_precise", 0,
        [read_xmm_double, write_xmm0_double](Marshaler& m) {
            write_xmm0_double(m, std::pow(read_xmm_double(m, 0),
                                          read_xmm_double(m, 1)));
        }
    });

    // --- C99 additions the UCRT exports --------------------------------------
    exports.entries.push_back({"acosh", 0, [read_dbl, push_dbl, raise_math](Marshaler& m) {
        const double x = read_dbl(m, 0);
        double r = win_nan(std::acosh(x), x);
        if (!std::isnan(x) && x < 1.0) {
            r = raise_math(m, kMathDomain, "acosh", x, 0.0, x87_indefinite());
        }
        push_dbl(m, r);
    }});
    exports.entries.push_back({"asinh", 0, [read_dbl, push_dbl](Marshaler& m) {
        push_dbl(m, std::asinh(read_dbl(m, 0)));
    }});
    exports.entries.push_back({"atanh", 0, [read_dbl, push_dbl, raise_math](Marshaler& m) {
        const double x = read_dbl(m, 0);
        double r = win_nan(std::atanh(x), x);
        if (!std::isnan(x)) {
            if (std::fabs(x) > 1.0) {
                r = raise_math(m, kMathDomain, "atanh", x, 0.0, x87_indefinite());
            } else if (std::fabs(x) == 1.0) {
                r = raise_math(m, kMathSing, "atanh", x, 0.0,
                               std::copysign(std::numeric_limits<double>::infinity(), x));
            }
        }
        push_dbl(m, r);
    }});
    exports.entries.push_back({"cbrt", 0, [read_dbl, push_dbl](Marshaler& m) {
        push_dbl(m, std::cbrt(read_dbl(m, 0)));
    }});
    // Oracle: expm1 overflow reports as name "exp".
    exports.entries.push_back({"expm1", 0, [read_dbl, push_dbl, raise_math](Marshaler& m) {
        const double x = read_dbl(m, 0);
        double r = std::expm1(x);
        if (std::isfinite(x) && std::isinf(r)) {
            r = raise_math(m, kMathOverflow, "exp", x, 0.0,
                           std::numeric_limits<double>::infinity());
        }
        push_dbl(m, r);
    }});
    exports.entries.push_back({"log1p", 0, [read_dbl, push_dbl, raise_math](Marshaler& m) {
        const double x = read_dbl(m, 0);
        double r = win_nan(std::log1p(x), x);
        if (!std::isnan(x)) {
            if (x < -1.0) {
                r = raise_math(m, kMathDomain, "log1p", x, 0.0, x87_indefinite());
            } else if (x == -1.0) {
                r = raise_math(m, kMathSing, "log1p", x, 0.0,
                               -std::numeric_limits<double>::infinity());
            }
        }
        push_dbl(m, r);
    }});
    exports.entries.push_back({"log2", 0, [read_dbl, push_dbl, raise_math](Marshaler& m) {
        const double x = read_dbl(m, 0);
        double r = win_nan(std::log2(x), x);
        if (!std::isnan(x)) {
            if (x < 0.0) {
                r = raise_math(m, kMathDomain, "log2", x, 0.0, x87_indefinite());
            } else if (x == 0.0) {
                r = raise_math(m, kMathSing, "log2", x, 0.0,
                               -std::numeric_limits<double>::infinity());
            }
        }
        push_dbl(m, r);
    }});
    // Learn (exp2): errno only — oracle shows no _matherr for exp2(1024).
    exports.entries.push_back({"exp2", 0, [read_dbl, push_dbl, set_math_errno](Marshaler& m) {
        const double x = read_dbl(m, 0);
        double r = std::exp2(x);
        if (std::isfinite(x) && std::isinf(r)) {
            set_math_errno(kMathOverflow);
        } else if (std::isfinite(x) && r == 0.0 && x < 0.0) {
            set_math_errno(kMathUnderflow);
        }
        push_dbl(m, r);
    }});
    exports.entries.push_back({"round", 0, [read_dbl, push_dbl](Marshaler& m) {
        push_dbl(m, std::round(read_dbl(m, 0)));
    }});
    // long lround(double) / long long llround(double) — integer return in EAX/EDX:EAX
    exports.entries.push_back({"lround", 0, [read_dbl](Marshaler& m) {
        m.set_return(static_cast<u64>(static_cast<u32>(
            static_cast<long>(std::lround(read_dbl(m, 0))))));
    }});
    exports.entries.push_back({"llround", 0, [read_dbl](Marshaler& m) {
        const long long v = std::llround(read_dbl(m, 0));
        // x86 returns long long in EDX:EAX (set_return_i64); x64 uses RAX.
        m.set_return_i64(static_cast<u64>(v));
    }});
    // long lrint(double) — round to nearest integer (current rounding mode).
    exports.entries.push_back({"lrint", 0, [read_dbl](Marshaler& m) {
        m.set_return(static_cast<u64>(static_cast<u32>(
            static_cast<long>(std::lrint(read_dbl(m, 0))))));
    }});
    exports.entries.push_back({"lrintf", 0, [read_flt](Marshaler& m) {
        m.set_return(static_cast<u64>(static_cast<u32>(
            static_cast<long>(std::lrintf(read_flt(m, 0))))));
    }});
    // double nan(const char* tagp) — quiet NaN; UCRT ignores tagp
    // (oracle: bits 0x7ff8000000000000).
    exports.entries.push_back({"nan", 0, [push_dbl](Marshaler& m) {
        (void)m.arg(0);
        const u64 bits = 0x7FF8000000000000ull;
        double d;
        std::memcpy(&d, &bits, sizeof(d));
        push_dbl(m, d);
    }});
    exports.entries.push_back({"nanf", 0, [push_flt](Marshaler& m) {
        (void)m.arg(0);
        const u32 bits = 0x7FC00000u;
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        push_flt(m, f);
    }});
    exports.entries.push_back({"trunc", 0, [read_dbl, push_dbl](Marshaler& m) {
        push_dbl(m, std::trunc(read_dbl(m, 0)));
    }});
    exports.entries.push_back({"copysign", 0, [read_dbl, push_dbl](Marshaler& m) {
        push_dbl(m, std::copysign(read_dbl(m, 0), read_dbl(m, 2)));
    }});
    for (const char* name : {"nextafter", "_nextafter", "nextafterl", "nexttoward", "nexttowardl"}) {
        exports.entries.push_back({name, 0, [read_dbl, push_dbl, step_dbl](Marshaler& m) {
            push_dbl(m, step_dbl(read_dbl(m, 0), read_dbl(m, 2)));
        }});
    }
    // float nexttowardf(float, long double): x86 stack slots 0 and 1-2; x64
    // XMM0 and XMM1.
    exports.entries.push_back({"nexttowardf", 0, [read_flt, push_flt, step_flt](Marshaler& m) {
        double y = 0.0;
        if (m.cpu_is_64bit()) {
            if (!m.has_cpu()) return;
            const u64 bits = m.cpu()->sse.xmm[1].u64_[0];
            std::memcpy(&y, &bits, sizeof(y));
        } else {
            const u64 bits = (static_cast<u64>(static_cast<u32>(m.arg(2))) << 32) |
                             static_cast<u32>(m.arg(1));
            std::memcpy(&y, &bits, sizeof(y));
        }
        push_flt(m, step_flt(read_flt(m, 0), y));
    }});
    exports.entries.push_back({"hypot", 0, [read_dbl, push_dbl](Marshaler& m) {
        push_dbl(m, std::hypot(read_dbl(m, 0), read_dbl(m, 2)));
    }});
    exports.entries.push_back({"remainder", 0, [read_dbl, push_dbl](Marshaler& m) {
        push_dbl(m, std::remainder(read_dbl(m, 0), read_dbl(m, 2)));
    }});
    // float forms: XMM0 in / low-32 XMM0 out on x64 (Learn log2f/copysignf).
    // Returning a double via push_dbl left the low 32 bits as garbage for
    // callers that read float (09_crt.md HIGH 1).
    exports.entries.push_back({"log2f", 0, [read_flt, push_flt, raise_flt](Marshaler& m) {
        const float x = read_flt(m, 0);
        float r = win_nanf(std::log2(x), x);
        if (!std::isnan(x)) {
            if (x < 0.0f) {
                r = raise_flt(m, kMathDomain, "log2f", x, 0.0f, x87_indefinite_f());
            } else if (x == 0.0f) {
                r = raise_flt(m, kMathSing, "log2f", x, 0.0f,
                              -std::numeric_limits<float>::infinity());
            }
        }
        push_flt(m, r);
    }});
    exports.entries.push_back({"copysignf", 0, [read_flt, push_flt](Marshaler& m) {
        push_flt(m, std::copysign(read_flt(m, 0), read_flt(m, 1)));
    }});

    // --- Classification ------------------------------------------------------
    //
    // _dclass/_fdclass/_ldclass return the C99 FP_* category. The UCRT's values
    // differ from the host's <cmath> macros, so they are named explicitly.
    constexpr i32 CRT_FP_NAN       = 2;   // _FP_NAN
    constexpr i32 CRT_FP_INFINITE  = 1;   // _FP_INFINITE
    constexpr i32 CRT_FP_ZERO      = 0;   // _FP_ZERO
    constexpr i32 CRT_FP_SUBNORMAL = -2;  // _FP_SUBNORMAL
    constexpr i32 CRT_FP_NORMAL    = -1;  // _FP_NORMAL
    auto classify = [=](double value) -> i32 {
        switch (std::fpclassify(value)) {
            case FP_NAN:       return CRT_FP_NAN;
            case FP_INFINITE:  return CRT_FP_INFINITE;
            case FP_ZERO:      return CRT_FP_ZERO;
            case FP_SUBNORMAL: return CRT_FP_SUBNORMAL;
            default:           return CRT_FP_NORMAL;
        }
    };
    exports.entries.push_back({"_dclass", 0, [read_dbl, classify](Marshaler& m) {
        m.set_return(static_cast<u32>(classify(read_dbl(m, 0))));
    }});
    exports.entries.push_back({"_ldclass", 0, [read_dbl, classify](Marshaler& m) {
        // long double is the same 64-bit double in MSVC.
        m.set_return(static_cast<u32>(classify(read_dbl(m, 0))));
    }});
    exports.entries.push_back({"_fdclass", 0, [read_flt, classify](Marshaler& m) {
        m.set_return(static_cast<u32>(
            classify(static_cast<double>(read_flt(m, 0)))));
    }});

    // Microsoft floating-point primitives: _dtest receives a POINTER,
    // unlike _dclass. Both return the SDK FP_* classification values.
    // Oracle: tools/oracle_probe/crt_compare_probe{32,64}.expected.
    for (const char* name : {"_dtest", "_ldtest"}) {
        exports.entries.push_back({name, 0, [classify](Marshaler& m) {
            const auto bits = m.read_u64(m.arg_ptr(0));
            if (bits.is_err()) return; // guest memory access is diagnosed by Marshaler
            double value;
            const u64 raw = bits.value();
            std::memcpy(&value, &raw, sizeof(value));
            m.set_return(static_cast<u32>(classify(value)));
        }});
    }
    exports.entries.push_back({"_fdtest", 0, [classify](Marshaler& m) {
        const auto bits = m.read_u32(m.arg_ptr(0));
        if (bits.is_err()) return;
        float value;
        const u32 raw = bits.value();
        std::memcpy(&value, &raw, sizeof(value));
        m.set_return(static_cast<u32>(classify(value)));
    }});
    // Classification uses the source bits so even signaling NaNs are not
    // accidentally quieted by the host's floating-point comparisons.
    const auto fp_bits = [](double value) { u64 bits; std::memcpy(&bits, &value, 8); return bits; };
    const auto is_nan_bits = [](u64 bits) { return (bits & 0x7fffffffffffffffull) > 0x7ff0000000000000ull; };
    const auto input_exceptions = [is_nan_bits](u64 bits) -> u16 {
        if ((bits & 0x7ff0000000000000ull) == 0 && (bits & 0xfffffffffffffull) != 0)
            return cpu::FPU_SW_DE;
        if (is_nan_bits(bits) && !(bits & 0x8000000000000ull)) return cpu::FPU_SW_IE;
        return 0;
    };
    // _FP_LT/_FP_EQ/_FP_GT values are recorded by crt_compare_probe.
    for (const char* name : {"_dpcomp", "_ldpcomp"}) {
        exports.entries.push_back({name, 0, [=](Marshaler& m) {
            const double a = read_dbl(m, 0), b = read_dbl(m, 2);
            const u64 ab = fp_bits(a), bb = fp_bits(b);
            const bool unordered = is_nan_bits(ab) || is_nan_bits(bb);
            if (m.has_cpu()) {
                if (!m.cpu_is_64bit()) m.cpu()->fpu.record_exceptions(input_exceptions(ab) | input_exceptions(bb));
                else if (!unordered && ((input_exceptions(ab) | input_exceptions(bb)) & cpu::FPU_SW_DE))
                    m.cpu()->sse.mxcsr |= cpu::MXCSR_DE;
            }
            m.set_return(unordered ? 0u : a < b ? 1u : a == b ? 2u : 4u);
        }});
    }
    // Actual UCRT chooses its second operand on equality/unordered. x86
    // loads the first operand through x87, while x64 uses integer bit tests.
    exports.entries.push_back({"fmax", 0, [=](Marshaler& m) {
        const double a = read_dbl(m, 0), b = read_dbl(m, 2);
        const u64 ab = fp_bits(a), bb = fp_bits(b);
        u64 result = !is_nan_bits(ab) && !is_nan_bits(bb) && a > b ? ab : bb;
        if (!m.cpu_is_64bit()) {
            if (m.has_cpu()) m.cpu()->fpu.record_exceptions(input_exceptions(ab));
            if (is_nan_bits(result)) result |= 0x8000000000000ull;
        }
        double value; std::memcpy(&value, &result, 8);
        push_dbl(m, value);
    }});

    // fmin / fminl / fmaxl (double; long double is double) and fminf / fmaxf.
    // Learn: a NaN operand yields the other operand; two NaNs yield a NaN.
    // Measured by tests/winapi/msvcrt/oracle/crt_ucrt_s5_probe: both NaN
    // returns the SECOND operand; zeros are ordered (-0 < +0), unlike fmax.
    // x64 raises only DE, and only when neither operand is a NaN. x86 loads
    // both operands through x87: DE for any denormal, IE for any SNaN, and a
    // returned SNaN comes back quieted.
    struct MinMaxFormat { u64 sign, exponent, quiet; };
    const auto minmax_pick = [](u64 ab, u64 bb, const MinMaxFormat& f, bool want_max,
                                const auto& less) -> u64 {
        const auto is_nan = [&f](u64 v) { return (v & ~f.sign) > f.exponent; };
        if (is_nan(ab)) return bb;
        if (is_nan(bb)) return ab;
        const bool a_neg = (ab & f.sign) != 0;
        if ((ab & ~f.sign) == 0 && (bb & ~f.sign) == 0) {  // +-0 pair
            return want_max ? (a_neg ? bb : ab) : (a_neg ? ab : bb);
        }
        const bool a_less = less(ab, bb);
        return want_max ? (a_less ? bb : ab) : (a_less ? ab : bb);
    };
    const auto minmax_finish = [](Marshaler& m, u64 ab, u64 bb, u64 result,
                                  const MinMaxFormat& f) -> u64 {
        const auto is_nan = [&f](u64 v) { return (v & ~f.sign) > f.exponent; };
        const auto is_denormal = [&f](u64 v) {
            return (v & f.exponent) == 0 && (v & ~f.sign) != 0;
        };
        const auto is_snan = [&](u64 v) { return is_nan(v) && (v & f.quiet) == 0; };
        if (m.cpu_is_64bit()) {
            if (m.has_cpu() && !is_nan(ab) && !is_nan(bb) &&
                (is_denormal(ab) || is_denormal(bb))) {
                m.cpu()->sse.mxcsr |= cpu::MXCSR_DE;
            }
            return result;
        }
        if (m.has_cpu()) {
            u16 flags = 0;
            if (is_denormal(ab) || is_denormal(bb)) flags |= cpu::FPU_SW_DE;
            if (is_snan(ab) || is_snan(bb)) flags |= cpu::FPU_SW_IE;
            m.cpu()->fpu.record_exceptions(flags);
        }
        return is_snan(result) ? (result | f.quiet) : result;
    };
    constexpr MinMaxFormat kDoubleFormat{0x8000000000000000ull, 0x7ff0000000000000ull,
                                         0x0008000000000000ull};
    constexpr MinMaxFormat kFloatFormat{0x80000000u, 0x7f800000u, 0x00400000u};
    for (const auto& [name, is_max] : {std::pair{"fmin", false}, std::pair{"fminl", false},
                                       std::pair{"fmaxl", true}}) {
        exports.entries.push_back({name, 0, [=, want_max = is_max](Marshaler& m) {
            const double a = read_dbl(m, 0), b = read_dbl(m, 2);
            const u64 ab = fp_bits(a), bb = fp_bits(b);
            const auto less = [a, b](u64, u64) { return a < b; };
            const u64 result = minmax_finish(
                m, ab, bb, minmax_pick(ab, bb, kDoubleFormat, want_max, less), kDoubleFormat);
            double value; std::memcpy(&value, &result, 8);
            push_dbl(m, value);
        }});
    }
    for (const auto& [name, is_max] : {std::pair{"fminf", false}, std::pair{"fmaxf", true}}) {
        exports.entries.push_back({name, 0, [=, want_max = is_max](Marshaler& m) {
            const float a = read_flt(m, 0), b = read_flt(m, 1);
            u32 a32, b32; std::memcpy(&a32, &a, 4); std::memcpy(&b32, &b, 4);
            const auto less = [a, b](u64, u64) { return a < b; };
            const u32 result = static_cast<u32>(minmax_finish(
                m, a32, b32, minmax_pick(a32, b32, kFloatFormat, want_max, less), kFloatFormat));
            float value; std::memcpy(&value, &result, 4);
            push_flt(m, value);
        }});
    }

    // int _dsign(double) / _fdsign(float) — non-zero when the sign bit is set
    for (const char* name : {"_dsign", "_ldsign"}) {
        exports.entries.push_back({name, 0, [read_dbl](Marshaler& m) {
            m.set_return(std::signbit(read_dbl(m, 0)) ? 0x8000u : 0u);
        }});
    }
    exports.entries.push_back({"_fdsign", 0, [read_flt](Marshaler& m) {
        m.set_return(std::signbit(read_flt(m, 0)) ? 0x8000u : 0u);
    }});

    // double lgamma(double) / float lgammaf(float) / long double lgammal
    // (UCRT math; long double is double). Special cases measured by
    // tools/oracle_probe/crt_s6_missing_probe: poles (+-0 and the negative
    // integers) return +inf with errno ERANGE; +-inf return +inf with errno
    // untouched; a NaN comes back unchanged (payload kept). A finite result
    // that overflows is +inf with ERANGE. Finite values come from the host
    // libm: the UCRT's lgamma is not correctly rounded and its algorithm is
    // not published, so results can differ from Windows in the last bits
    // (measured: 5 of 23 double samples by 1 ULP, lgamma(-2.5) by 10 ULP; 3 of
    // 23 float samples by 1 ULP). Recorded as an open fidelity gap.
    auto lgamma_special = [](double x, double& result) -> int {
        if (std::isnan(x)) { result = x; return 0; }
        if (std::isinf(x)) { result = std::numeric_limits<double>::infinity(); return 0; }
        if (x == 0.0 || (x < 0.0 && std::trunc(x) == x)) {
            result = std::numeric_limits<double>::infinity();
            return kMathSing;
        }
        return -1;
    };
    for (const char* name : {"lgamma", "lgammal"}) {
        exports.entries.push_back({name, 0,
            [read_dbl, push_dbl, set_math_errno, lgamma_special](Marshaler& m) {
                const double x = read_dbl(m, 0);
                double r = 0.0;
                const int special = lgamma_special(x, r);
                if (special < 0) {
                    r = std::lgamma(x);
                    if (std::isinf(r)) set_math_errno(kMathOverflow);
                } else if (special != 0) {
                    set_math_errno(special);
                }
                push_dbl(m, r);
            }});
    }
    exports.entries.push_back({"lgammaf", 0,
        [read_flt, push_flt, set_math_errno, lgamma_special](Marshaler& m) {
            const float x = read_flt(m, 0);
            double special_result = 0.0;
            const int special = lgamma_special(static_cast<double>(x), special_result);
            float r = 0.0f;
            if (special < 0) {
                r = std::lgammaf(x);
                if (std::isinf(r)) set_math_errno(kMathOverflow);
            } else {
                r = std::isnan(x) ? x : static_cast<float>(special_result);
                if (special != 0) set_math_errno(special);
            }
            push_flt(m, r);
        }});

    // x86 compiler helpers (msvcrt.dll exports them for x86 only), measured by
    // tools/oracle_probe/crt_s6_missing_probe (x86 rows):
    // _ftol / _ftol2: pop ST(0), truncate toward zero to __int64 in EDX:EAX;
    // NaN, infinity and out-of-range give the integer indefinite
    // 0x8000000000000000. The conversion is from the 80-bit value itself.
    auto ftol64 = [](const cpu::Float80& v) -> u64 {
        constexpr u64 kIndefinite = 0x8000000000000000ULL;
        const u32 exponent = v.exponent_sign & 0x7FFFu;
        const bool negative = (v.exponent_sign & 0x8000u) != 0;
        if (exponent == 0x7FFFu) return kIndefinite;
        if (v.mantissa == 0 || exponent < 16383u) return 0;
        const u32 e = exponent - 16383u;
        if (e > 63) return kIndefinite;
        const u64 magnitude = v.mantissa >> (63u - e);
        // 2^63 and above do not fit; -2^63 is the indefinite's own bit pattern.
        if (magnitude > 0x7FFFFFFFFFFFFFFFULL) return kIndefinite;
        return negative ? (~magnitude + 1) : magnitude;
    };
    for (const char* name : {"_ftol", "_ftol2"}) {
        exports.entries.push_back(ExportEntry{name, 0, [ftol64](Marshaler& m) {
            if (!m.has_cpu()) return;
            const cpu::Float80 value = m.cpu()->fpu.get_st(0);
            (void)m.cpu()->fpu.pop();
            m.set_return_i64(ftol64(value));
        }});
    }
    // _ftol2_sse: pop ST(0) as a double, then CVTTSD2SI to a 32-bit int in
    // EAX (0x80000000 when it does not fit or is NaN); EDX is left as it was.
    exports.entries.push_back(ExportEntry{"_ftol2_sse", 0, [](Marshaler& m) {
        if (!m.has_cpu()) return;
        const double value = m.cpu()->fpu.get_st(0).to_double();
        (void)m.cpu()->fpu.pop();
        u32 result = 0x80000000u;
        if (!std::isnan(value)) {
            const double t = std::trunc(value);
            if (t >= -2147483648.0 && t <= 2147483647.0) {
                result = static_cast<u32>(static_cast<i32>(t));
            }
        }
        m.cpu()->gpr.rax = (m.cpu()->gpr.rax & ~0xFFFFFFFFULL) | result;
    }});

    // L3-K in-chain class (winapi/core/host_in_chain.hpp). Every export above
    // computes and returns -- no wait, no raise, no control transfer -- and
    // the only guest code any of them can run is the user `_matherr`
    // installed with __setusermatherr, called through run_guest_callback
    // (raise_math, CrtMemoryManager::call_guest; Learn, _matherr: the math
    // routines call it when they detect an error). That is AGuestCallback.
    for (ExportEntry& entry : exports.entries) {
        if (entry.direct.fn == nullptr) {
            entry.in_chain = InChainClass::AGuestCallback;
        }
    }

    return exports;
}

} // namespace vitreo::winapi
