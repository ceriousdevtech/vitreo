// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_string_fcvt_registration.cpp
/// @brief _atoflt / _fcvt / _ecvt and the static conversion buffer they share.
///
/// Split out of crt_string_registration.cpp.

#include "winapi/msvcrt/text/crt_string_registration_internal.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vitreo::winapi {

void add_crt_fcvt_exports(DllExports& exports, CrtMemoryManager& mem_mgr) {

    // ---- Floating conversion helpers (_atoflt / _fcvt / _ecvt) ---------------
    //
    // CRT static-buffer semantics: reuse one guest buffer allocated once.

    auto fcvt_buffer = [&mem_mgr](Marshaler& m, usize needed) -> GuestAddr {
        static GuestAddr s_buf = 0;
        static usize s_cap = 0;
        if (needed + 1 > s_cap) {
            const GuestAddr fresh = mem_mgr.crt_malloc(needed + 1);
            if (fresh == 0) return 0;
            s_buf = fresh;
            s_cap = needed + 1;
        }
        return s_buf;
    };

    // void _atoflt(STRFLT pflt, const char* str)
    // STRFLT: { int sign; int decpt; int flag; char* mantissa }
    exports.entries.push_back({"_atoflt", 0, [&mem_mgr, fcvt_buffer](Marshaler& m) {
        const GuestAddr pflt = m.arg_ptr(0);
        const GuestAddr str_ptr = m.arg_ptr(1);
        if (pflt == 0 || str_ptr == 0) return;
        auto str = m.read_ansi_string(str_ptr);
        if (str.is_err()) return;

        char* end = nullptr;
        const double value = std::strtod(str.value().c_str(), &end);
        int sign = 0;
        int decpt = 0;
        int flag = 0;
        std::string digits;

        if (value < 0.0) {
            sign = 1;
        }
        if (!std::isfinite(value)) {
            flag = 1;
            digits = "0";
            decpt = 0;
        } else {
            char tmp[64];
            std::snprintf(tmp, sizeof(tmp), "%.15e", std::fabs(value));
            // Form: d.dddddde±ee — extract mantissa digits and exponent.
            digits.push_back(tmp[0] == '0' && tmp[1] == '.' ? '0' : tmp[0]);
            for (usize i = 2; tmp[i] != '\0' && tmp[i] != 'e' && tmp[i] != 'E';
                 ++i) {
                if (tmp[i] >= '0' && tmp[i] <= '9') digits.push_back(tmp[i]);
            }
            const char* exp = std::strchr(tmp, 'e');
            if (!exp) exp = std::strchr(tmp, 'E');
            decpt = exp ? std::atoi(exp + 1) + 1 : 1;
        }

        const GuestAddr mant = fcvt_buffer(m, digits.size());
        if (mant == 0) return;
        (void)m.memory().write_bytes(
            mant, reinterpret_cast<const Byte*>(digits.data()), digits.size());
        (void)m.write_u8(mant + digits.size(), 0);

        (void)m.write_u32(pflt + 0, static_cast<u32>(sign));
        (void)m.write_u32(pflt + 4, static_cast<u32>(decpt));
        (void)m.write_u32(pflt + 8, static_cast<u32>(flag));
        if (m.cpu_is_64bit()) {
            (void)m.write_u64(pflt + 16, mant);
        } else {
            (void)m.write_u32(pflt + 12, static_cast<u32>(mant));
        }
        (void)mem_mgr;  // errno unused; historical void return
    }});

    // char* _fcvt(double value, int count, int* dec, int* sign)
    // char* _ecvt(double value, int count, int* dec, int* sign)
    auto convert_digits = [fcvt_buffer](Marshaler& m, bool fcvt_mode) {
        double value = 0.0;
        usize arg_base = 0;
        if (m.cpu_is_64bit() && m.has_cpu()) {
            // Win64: first floating arg arrives in XMM0.
            const u64 bits = m.cpu()->sse.xmm[0].u64_[0];
            std::memcpy(&value, &bits, sizeof(value));
            arg_base = 1;  // count is the first integer arg (RDX)
        } else {
            const u32 lo = static_cast<u32>(m.arg(0));
            const u32 hi = static_cast<u32>(m.arg(1));
            const u64 bits = (static_cast<u64>(hi) << 32) | lo;
            std::memcpy(&value, &bits, sizeof(value));
            arg_base = 2;
        }
        const auto count = static_cast<i32>(m.arg(arg_base));
        const GuestAddr dec_ptr = m.arg_ptr(arg_base + 1);
        const GuestAddr sign_ptr = m.arg_ptr(arg_base + 2);

        const int sign = value < 0.0 ? 1 : 0;
        const double abs_val = std::fabs(value);
        const int digits = count < 0 ? 0 : (count > 32 ? 32 : count);

        char tmp[64];
        int decpt = 0;
        if (fcvt_mode) {
            // _fcvt returns SIGNIFICANT digits only, with `dec` giving where
            // the decimal point falls relative to the start of that string --
            // it is not "%.*f" with the dot deleted. Measured on Windows:
            // 0.5 at 3 decimals is "500"/dec=0, not "0500"; and 1e-5 at 3
            // decimals is ""/dec=-4, because nothing survives the rounding
            // yet the point is still four places left of where digits would
            // have started.
            if (abs_val == 0.0) {
                const std::string zeros(static_cast<usize>(digits), '0');
                std::snprintf(tmp, sizeof(tmp), "%s", zeros.c_str());
                decpt = 0;
            } else {
                char probe[64];
                std::snprintf(probe, sizeof(probe), "%.*e", 17, abs_val);
                const char* pe = std::strchr(probe, 'e');
                const int e10 = (pe ? std::atoi(pe + 1) : 0) + 1;
                decpt = e10;
                const int significant = e10 + digits;
                if (significant <= 0) {
                    tmp[0] = '\0';           // rounded away entirely
                } else {
                    std::snprintf(probe, sizeof(probe), "%.*e",
                                  significant - 1, abs_val);
                    std::string filtered;
                    for (usize i = 0; probe[i] != '\0' && probe[i] != 'e' &&
                                      probe[i] != 'E'; ++i) {
                        if (probe[i] >= '0' && probe[i] <= '9')
                            filtered.push_back(probe[i]);
                    }
                    // Rounding can carry a new leading digit (9.9999999 at 3
                    // decimals becomes 10.000), which both shifts the decimal
                    // point right AND makes the result one digit longer -- the
                    // digit count has to be recomputed from the new exponent,
                    // or the last decimal is dropped.
                    const char* pe2 = std::strchr(probe, 'e');
                    if (pe2 != nullptr) {
                        const int carried = std::atoi(pe2 + 1) + 1;
                        if (carried != e10) {
                            decpt = carried;
                            const int again = carried + digits;
                            if (again <= 0) {
                                filtered.clear();
                            } else {
                                std::snprintf(probe, sizeof(probe), "%.*e",
                                              again - 1, abs_val);
                                filtered.clear();
                                for (usize i = 0; probe[i] != '\0' &&
                                                  probe[i] != 'e' &&
                                                  probe[i] != 'E'; ++i) {
                                    if (probe[i] >= '0' && probe[i] <= '9')
                                        filtered.push_back(probe[i]);
                                }
                            }
                        }
                    }
                    std::snprintf(tmp, sizeof(tmp), "%s", filtered.c_str());
                }
            }
        } else {
            // _ecvt: count total significant digits.
            std::snprintf(tmp, sizeof(tmp), "%.*e", digits > 0 ? digits - 1 : 0,
                          abs_val);
            std::string filtered;
            for (usize i = 0; tmp[i] != '\0' && tmp[i] != 'e' && tmp[i] != 'E';
                 ++i) {
                if (tmp[i] >= '0' && tmp[i] <= '9') filtered.push_back(tmp[i]);
            }
            const char* exp = std::strchr(tmp, 'e');
            if (!exp) exp = std::strchr(tmp, 'E');
            // Zero has no meaningful exponent: Windows reports dec = 0, not
            // the 1 that "0.00000e+00" would otherwise give.
            decpt = (abs_val == 0.0) ? 0
                                     : (exp ? std::atoi(exp + 1) + 1 : 1);
            std::snprintf(tmp, sizeof(tmp), "%s", filtered.c_str());
        }

        if (dec_ptr) (void)m.write_u32(dec_ptr, static_cast<u32>(decpt));
        if (sign_ptr) (void)m.write_u32(sign_ptr, static_cast<u32>(sign));

        const usize len = std::strlen(tmp);
        const GuestAddr buf = fcvt_buffer(m, len);
        if (buf == 0) {
            m.set_return(0);
            return;
        }
        (void)m.memory().write_bytes(
            buf, reinterpret_cast<const Byte*>(tmp), len);
        (void)m.write_u8(buf + len, 0);
        m.set_return(buf);
    };

    exports.entries.push_back({"_fcvt", 0,
        [convert_digits](Marshaler& m) { convert_digits(m, true); }});
    exports.entries.push_back({"_ecvt", 0,
        [convert_digits](Marshaler& m) { convert_digits(m, false); }});
}

} // namespace vitreo::winapi
