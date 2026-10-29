#pragma once
/// @file crt_printf.hpp
/// @brief The printf output processor shared by every CRT formatted-output
///        export (UCRT `__stdio_common_*` and the legacy msvcrt.dll family).
///
/// Contract sources:
/// - UCRT: the Windows SDK's own UCRT source (10.0.26100.0
///   `ucrt/inc/corecrt_internal_stdio_output.h`, `ucrt/stdio/output.cpp`,
///   `ucrt/convert/cvt.cpp`, `cfout.cpp`, `_fptostr.cpp`) and the option bits
///   in `corecrt_stdio_config.h`. Verified byte-for-byte by
///   tools/oracle_probe/crt_printf_probe.c (x86 and x64 .expected).
/// - msvcrt.dll: measured, tools/oracle_probe/crt_msvcrt_printf_probe.c. The
///   system msvcrt runs the same processor with legacy rules: 17 significant
///   digits, 3-digit exponents, 1.#INF-style special values, legacy rounding,
///   `%a` defaulting to 6 hex digits, `F`/`N` ignored, `I`/`z`/`t` only when a
///   conversion they size follows, `hh` meaning `h`, `%#p` prefixed with 0X,
///   and `%Z` wide only with `w`.
///
/// Everything a guest can observe is modelled: the exact characters, where the
/// processor stops (a full buffer or a failed conversion ends the walk, so no
/// later argument is read), errno, and which failures reach the
/// invalid-parameter handler.

#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"

#include <vitreo/common.hpp>

#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace vitreo::winapi::crt_printf {

// corecrt_stdio_config.h (Windows SDK 10.0.26100.0).
inline constexpr u64 kLegacyVsprintfNullTermination = 1ULL << 0;
inline constexpr u64 kStandardSnprintfBehavior = 1ULL << 1;
inline constexpr u64 kLegacyWideSpecifiers = 1ULL << 2;
inline constexpr u64 kLegacyMsvcrtCompatibility = 1ULL << 3;
inline constexpr u64 kLegacyThreeDigitExponents = 1ULL << 4;
inline constexpr u64 kStandardRounding = 1ULL << 5;

inline constexpr i32 kEINVAL = 22;
inline constexpr i32 kERANGE = 34;
inline constexpr i32 kEILSEQ = 42;

/// Which DLL's formatter: ucrtbase's (options decide the legacy behaviours) or
/// the system msvcrt.dll's (always legacy, with its own differences).
enum class Flavor : u8 { Ucrt, Msvcrt };

/// UCRT's three processor bases: the plain functions, the `_s` functions
/// (format validation), and the `_p` functions (positional parameters, which
/// also validate).
enum class Processor : u8 { Standard, Validating, Positional };

/// Walks a guest variadic argument list. Either a `va_list` (a pointer into
/// guest memory) or the export's own argument slots from a given index on.
/// Slots are 4 bytes on x86 and 8 on x64; on x86 a 64-bit integer or a double
/// occupies two slots.
class ArgCursor {
public:
    static ArgCursor va_list_at(GuestAddr va) { return ArgCursor(true, va, 0); }
    static ArgCursor call_args(usize first_index) {
        return ArgCursor(false, 0, first_index);
    }

    u32 next_int32(Marshaler& m) { return static_cast<u32>(take(m)); }
    u64 next_pointer(Marshaler& m) {
        const u64 v = take(m);
        return m.cpu_is_64bit() ? v : (v & 0xFFFFFFFFULL);
    }
    u64 next_int64(Marshaler& m) {
        if (m.cpu_is_64bit()) return take(m);
        const u64 lo = take(m) & 0xFFFFFFFFULL;
        const u64 hi = take(m) & 0xFFFFFFFFULL;
        return lo | (hi << 32);
    }
    u64 next_double_bits(Marshaler& m) { return next_int64(m); }

private:
    ArgCursor(bool va, GuestAddr base, usize first)
        : m_va(va), m_base(base), m_first(first) {}
    u64 take(Marshaler& m);

    bool m_va;
    GuestAddr m_base;
    usize m_first;
    usize m_index{0};
};

/// UCRT's string_output_adapter: stores at most `capacity` characters. When it
/// is full, `continue_count` decides whether the count keeps growing (C99
/// snprintf, counting calls) or the processor stops with -1.
struct Target {
    usize capacity{std::numeric_limits<usize>::max()};
    bool continue_count{true};
    std::vector<u16> stored;   // characters actually stored (bytes for narrow)
};

struct Request {
    Flavor flavor{Flavor::Ucrt};
    Processor processor{Processor::Standard};
    u64 options{0};
    bool wide{false};            // wchar_t format and output
    bool count_output_enabled{false};  // _set_printf_count_output (UCRT only)
    /// msvcrt.dll `_set_output_format(_TWO_DIGIT_EXPONENT)`: 2-digit exponents.
    bool two_digit_exponent{false};
    /// errno writer (the calling guest thread's errno).
    std::function<void(i32)> set_errno;
    /// Reports a failed parameter validation (errno is already EINVAL):
    /// CrtMemoryManager::invalid_parameter, whose no-handler policy follows
    /// the calling DLL (CrtDllFlavorScope).
    std::function<void()> invalid_parameter;
};

/// Runs the processor over the NUL-terminated guest format at @p format.
/// @return the UCRT processor result: characters written, or -1.
int format(Marshaler& m, const Request& request, GuestAddr format,
           ArgCursor args, Target& out);

/// The process-wide `_set_printf_count_output` switch (UCRT). %n is refused
/// (EINVAL) unless it is on; it starts off.
bool printf_count_output_enabled() noexcept;
bool exchange_printf_count_output(bool enabled) noexcept;

/// msvcrt.dll's process-wide `_set_output_format` value (_TWO_DIGIT_EXPONENT
/// is 1); starts 0.
u32 msvcrt_output_format() noexcept;
u32 exchange_msvcrt_output_format(u32 format) noexcept;

/// Write @p units to guest memory at @p buffer as narrow bytes or UTF-16.
void store_units(Marshaler& m, GuestAddr buffer, const std::vector<u16>& units,
                 bool wide);
/// Write one NUL character at index @p index of a narrow or wide buffer.
void store_nul(Marshaler& m, GuestAddr buffer, usize index, bool wide);

} // namespace vitreo::winapi::crt_printf

namespace vitreo::winapi {
class CrtMemoryManager;

namespace crt_printf {
/// The Request a CRT export builds: errno and, for the UCRT, the
/// invalid-parameter handler, wired to @p memory_manager.
Request make_request(CrtMemoryManager& memory_manager, Flavor flavor,
                     Processor processor, u64 options, bool wide);
/// A failed parameter check: errno, then (UCRT only) the handler.
void report_invalid(CrtMemoryManager& memory_manager, Flavor flavor, i32 error);
} // namespace crt_printf

/// String-target printf exports (UCRT __stdio_common_vs*printf* and the
/// msvcrt.dll sprintf family). Registered after the other CRT tables.
DllExports create_crt_printf_thunks(CrtMemoryManager& memory_manager);
} // namespace vitreo::winapi
