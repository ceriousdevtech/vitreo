/// @file crt_printf_registration.cpp
/// @brief String-target printf exports: UCRT `__stdio_common_vs*printf*` and
///        the legacy msvcrt.dll sprintf family, all on crt_printf's processor.
///
/// UCRT termination and return rules follow ucrt/stdio/output.cpp
/// (common_vsprintf, common_vsprintf_s, common_vsnprintf_s); the msvcrt ones
/// are measured per export by tools/oracle_probe/crt_msvcrt_printf_probe.c,
/// because the system msvcrt.dll differs from the UCRT in several of them
/// (errno ERANGE on truncation, where _snprintf_c/_vsnprintf_c put the NUL,
/// _snprintf_s's extra terminator at max_count). A failed validation reaches
/// CrtMemoryManager::invalid_parameter, which does not fast-fail inside an
/// msvcrt.dll export (msvcrt exports no handler setter; measured).

#include "winapi/msvcrt/io/crt_printf.hpp"

#include "winapi/core/thunk_registry.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"

#include <limits>

namespace vitreo::winapi {

namespace {

using namespace crt_printf;

/// Arguments common to every formatted-output call.
struct Call {
    Marshaler& m;
    CrtMemoryManager& mm;
    Flavor flavor;
    bool wide;
    Processor processor{Processor::Standard};
    u64 options{0};
};

Request make_request(const Call& c) {
    return crt_printf::make_request(c.mm, c.flavor, c.processor, c.options, c.wide);
}

void report_invalid(const Call& c, i32 error) {
    crt_printf::report_invalid(c.mm, c.flavor, error);
}

struct Formatted {
    int result;
    usize used;
    bool truncated;  // the processor stopped because the buffer was full
};

/// Runs the processor into @p buffer (capacity @p count) and stores what it
/// produced; termination is the caller's rule.
Formatted run(const Call& c, GuestAddr buffer, usize count, bool continue_count,
              GuestAddr format, ArgCursor args) {
    Target target;
    target.capacity = count;
    target.continue_count = continue_count;
    const Request request = make_request(c);
    const int r = crt_printf::format(c.m, request, format, args, target);
    if (buffer != 0) store_units(c.m, buffer, target.stored, c.wide);
    const bool full = target.stored.size() == count;
    return {r, target.stored.size(), full && r < 0 && !continue_count};
}

usize truncate_value(const Marshaler& m) {
    return m.cpu_is_64bit() ? std::numeric_limits<u64>::max()
                            : std::numeric_limits<u32>::max();
}

// ---- UCRT (ucrt/stdio/output.cpp) ---------------------------------------------

/// common_vsprintf.
int ucrt_vsprintf(const Call& c, GuestAddr buffer, usize count, GuestAddr format,
                  ArgCursor args) {
    if (format == 0 || (count != 0 && buffer == 0)) {
        report_invalid(c, kEINVAL);
        return -1;
    }
    const bool standard = (c.options & kStandardSnprintfBehavior) != 0;
    const Formatted f = run(c, buffer, count, standard || buffer == 0, format, args);
    const int result = f.result;
    if (buffer == 0) return result;
    if (c.options & kLegacyVsprintfNullTermination) {
        if (count == 0 && result != 0) return -1;
        if (f.used != count) store_nul(c.m, buffer, f.used, c.wide);
        else if (result >= 0 && static_cast<usize>(result) > count) return -1;
        return result;
    }
    if (standard) {
        if (count == 0) return result;
        if (result < 0) store_nul(c.m, buffer, 0, c.wide);
        else if (f.used == count) store_nul(c.m, buffer, count - 1, c.wide);
        else store_nul(c.m, buffer, f.used, c.wide);
        return result;
    }
    if (count == 0) return -1;
    if (f.used == count) {
        store_nul(c.m, buffer, count - 1, c.wide);
        return -2;
    }
    store_nul(c.m, buffer, f.used, c.wide);
    return result;
}

/// common_vsprintf_s.
int ucrt_vsprintf_s(Call c, GuestAddr buffer, usize count, GuestAddr format,
                    ArgCursor args) {
    if (format == 0 || buffer == 0 || count == 0) {
        report_invalid(c, kEINVAL);
        return -1;
    }
    c.processor = Processor::Validating;
    const int result = ucrt_vsprintf(c, buffer, count, format, args);
    if (result < 0) store_nul(c.m, buffer, 0, c.wide);
    if (result == -2) {
        report_invalid(c, kERANGE);
        return -1;
    }
    return result;
}

/// common_vsnprintf_s.
int ucrt_vsnprintf_s(Call c, GuestAddr buffer, usize count, usize max_count,
                     GuestAddr format, ArgCursor args) {
    if (format == 0) {
        report_invalid(c, kEINVAL);
        return -1;
    }
    if (max_count == 0 && buffer == 0 && count == 0) return 0;
    if (buffer == 0 || count == 0) {
        report_invalid(c, kEINVAL);
        return -1;
    }
    c.processor = Processor::Validating;
    const u32 saved_errno = c.mm.get_errno();
    int result = -1;
    if (count > max_count) {
        result = ucrt_vsprintf(c, buffer, max_count + 1, format, args);
        if (result == -2) {
            if (c.mm.get_errno() == static_cast<u32>(kERANGE)) c.mm.set_errno(saved_errno);
            return -1;
        }
    } else {
        result = ucrt_vsprintf(c, buffer, count, format, args);
        store_nul(c.m, buffer, count - 1, c.wide);
        if (result == -2 && max_count == truncate_value(c.m)) {
            if (c.mm.get_errno() == static_cast<u32>(kERANGE)) c.mm.set_errno(saved_errno);
            return -1;
        }
    }
    if (result < 0) {
        store_nul(c.m, buffer, 0, c.wide);
        if (result == -2) report_invalid(c, kERANGE);
        return -1;
    }
    return result;
}

// ---- msvcrt.dll (measured, crt_msvcrt_printf_probe) -------------------------------

/// _snprintf / _vsnprintf / vsnprintf / _snwprintf / _vsnwprintf, and sprintf
/// (count = SIZE_MAX). Exact fit stores no terminator; any result that could
/// not be terminated sets errno ERANGE.
int msvcrt_snprintf(const Call& c, GuestAddr buffer, usize count,
                    GuestAddr format, ArgCursor args) {
    if (format == 0 || (buffer == 0 && count != 0)) {
        report_invalid(c, kEINVAL);
        return -1;
    }
    const Formatted f = run(c, buffer, count, buffer == 0, format, args);
    if (buffer == 0) return f.result;
    if (f.used == count) c.mm.set_errno(static_cast<u32>(kERANGE));
    if (count == 0) return f.result != 0 ? -1 : 0;
    if (f.used != count) store_nul(c.m, buffer, f.used, c.wide);
    return f.result;
}

/// _snprintf_c / _swprintf_c: no room for the terminator is a failure that
/// empties the string at [0] (also when count is 0).
int msvcrt_snprintf_c(const Call& c, GuestAddr buffer, usize count,
                      GuestAddr format, ArgCursor args) {
    if (format == 0 || (buffer == 0 && count != 0)) {
        report_invalid(c, kEINVAL);
        return -1;
    }
    const Formatted f = run(c, buffer, count, buffer == 0, format, args);
    if (buffer == 0) return f.result;
    if (f.used == count) {
        store_nul(c.m, buffer, 0, c.wide);
        c.mm.set_errno(static_cast<u32>(kERANGE));
        return -1;
    }
    store_nul(c.m, buffer, f.used, c.wide);
    return f.result;
}

/// _vsnprintf_c / _vswprintf_c: truncates with the terminator at count-1.
int msvcrt_vsnprintf_c(const Call& c, GuestAddr buffer, usize count,
                       GuestAddr format, ArgCursor args) {
    if (format == 0 || (buffer == 0 && count != 0)) {
        report_invalid(c, kEINVAL);
        return -1;
    }
    const Formatted f = run(c, buffer, count, buffer == 0, format, args);
    if (buffer == 0) return f.result;
    if (f.used == count) {
        if (count != 0) store_nul(c.m, buffer, count - 1, c.wide);
        c.mm.set_errno(static_cast<u32>(kERANGE));
        return -1;
    }
    store_nul(c.m, buffer, f.used, c.wide);
    return f.result;
}

/// sprintf_s / vsprintf_s / swprintf_s / vswprintf_s: the UCRT _s rules with
/// msvcrt's handler-less validation, except that a format error empties the
/// string at [0] without first terminating what was produced (measured
/// "[%n]" leaves "\0ZZZ").
int msvcrt_sprintf_s(Call c, GuestAddr buffer, usize count, GuestAddr format,
                     ArgCursor args) {
    if (format == 0 || buffer == 0 || count == 0) {
        report_invalid(c, kEINVAL);
        return -1;
    }
    c.processor = Processor::Validating;
    const Formatted f = run(c, buffer, count, false, format, args);
    if (f.used == count) {  // no room left for the terminator
        store_nul(c.m, buffer, count - 1, c.wide);
        store_nul(c.m, buffer, 0, c.wide);
        report_invalid(c, kERANGE);
        return -1;
    }
    if (f.result < 0) {
        store_nul(c.m, buffer, 0, c.wide);
        return -1;
    }
    store_nul(c.m, buffer, f.used, c.wide);
    return f.result;
}

/// _snprintf_s family. Narrow truncation leaves errno ERANGE; the wide
/// functions leave errno alone (measured asymmetry). With room past
/// max_count the formatter gets max_count characters and a terminator is
/// stored at [max_count] as well.
int msvcrt_snprintf_s(Call c, GuestAddr buffer, usize count, usize max_count,
                      GuestAddr format, ArgCursor args) {
    if (format == 0) {
        report_invalid(c, kEINVAL);
        return -1;
    }
    if (max_count == 0 && buffer == 0 && count == 0) return 0;
    if (buffer == 0 || count == 0) {
        report_invalid(c, kEINVAL);
        return -1;
    }
    c.processor = Processor::Validating;
    const u32 saved_errno = c.mm.get_errno();
    auto truncation_errno = [&]() {
        c.mm.set_errno(c.wide ? saved_errno : static_cast<u32>(kERANGE));
    };
    if (count > max_count) {
        const Formatted f = run(c, buffer, max_count, false, format, args);
        store_nul(c.m, buffer, max_count, c.wide);
        if (f.truncated) {
            truncation_errno();
            return -1;
        }
        if (f.result < 0) {
            store_nul(c.m, buffer, 0, c.wide);
            return -1;
        }
        store_nul(c.m, buffer, f.used, c.wide);
        return f.result;
    }
    const Formatted f = run(c, buffer, count, false, format, args);
    store_nul(c.m, buffer, count - 1, c.wide);
    if (f.used == count) {  // no room left for the terminator
        if (max_count == truncate_value(c.m)) {
            truncation_errno();
            return -1;
        }
        store_nul(c.m, buffer, 0, c.wide);
        report_invalid(c, kERANGE);
        return -1;
    }
    if (f.result < 0) {
        store_nul(c.m, buffer, 0, c.wide);
        return -1;
    }
    store_nul(c.m, buffer, f.used, c.wide);
    return f.result;
}

/// _scprintf / _vscprintf / _scwprintf / _vscwprintf.
int count_only(const Call& c, GuestAddr format, ArgCursor args) {
    if (format == 0) {
        report_invalid(c, kEINVAL);
        return -1;
    }
    return run(c, 0, 0, true, format, args).result;
}

/// _vsprintf_p: positional, truncation terminates at count-1 and fails with
/// ERANGE; a format error leaves the buffer as the processor left it.
int msvcrt_vsprintf_p(Call c, GuestAddr buffer, usize count, GuestAddr format,
                      ArgCursor args) {
    if (format == 0 || (buffer == 0 && count != 0)) {
        report_invalid(c, kEINVAL);
        return -1;
    }
    c.processor = Processor::Positional;
    const Formatted f = run(c, buffer, count, buffer == 0, format, args);
    if (buffer == 0) return f.result;
    if (f.used == count) {
        if (count != 0) store_nul(c.m, buffer, count - 1, c.wide);
        c.mm.set_errno(static_cast<u32>(kERANGE));
        return -1;
    }
    if (f.result < 0) return -1;
    store_nul(c.m, buffer, f.used, c.wide);
    return f.result;
}

void ret(Marshaler& m, int value) { m.set_return(static_cast<u32>(value)); }

} // namespace

namespace crt_printf {

Request make_request(CrtMemoryManager& mm, Flavor flavor, Processor processor,
                     u64 options, bool wide) {
    Request r;
    r.flavor = flavor;
    r.processor = processor;
    r.options = options;
    r.wide = wide;
    r.count_output_enabled = flavor == Flavor::Ucrt && printf_count_output_enabled();
    r.two_digit_exponent =
        flavor == Flavor::Msvcrt && (msvcrt_output_format() & 1u) != 0;
    CrtMemoryManager* p = &mm;
    r.set_errno = [p](i32 e) { p->set_errno(static_cast<u32>(e)); };
    // The handler (or, for the UCRT, the fast-fail) is decided by the DLL the
    // export belongs to (CrtDllFlavorScope), not by the formatter flavor.
    r.invalid_parameter = [p]() { (void)p->invalid_parameter(); };
    return r;
}

void report_invalid(CrtMemoryManager& mm, Flavor flavor, i32 error) {
    (void)flavor;
    mm.set_errno(static_cast<u32>(error));
    (void)mm.invalid_parameter();
}

} // namespace crt_printf

DllExports create_crt_printf_thunks(CrtMemoryManager& mm) {
    DllExports exports;
    auto add = [&exports](const char* name, std::function<void(Marshaler&)> fn) {
        exports.entries.push_back({name, 0, std::move(fn), 0});
    };

    // ---- UCRT __stdio_common_* (corecrt_stdio_config.h options) ----------------
    // `unsigned __int64 _Options` occupies two slots on x86.
    auto options_of = [](Marshaler& m) -> u64 {
        return m.cpu_is_64bit() ? m.arg(0)
                                : (m.arg(0) & 0xFFFFFFFFULL) | (m.arg(1) << 32);
    };
    auto base_of = [](Marshaler& m) -> usize { return m.cpu_is_64bit() ? 1 : 2; };

    for (const bool wide : {false, true}) {
        // int __stdio_common_vsprintf(u64 options, char* buffer, size_t count,
        //                             const char* format, _locale_t, va_list)
        add(wide ? "__stdio_common_vswprintf" : "__stdio_common_vsprintf",
            [&mm, wide, options_of, base_of](Marshaler& m) {
                const usize b = base_of(m);
                Call c{m, mm, Flavor::Ucrt, wide, Processor::Standard, options_of(m)};
                ret(m, ucrt_vsprintf(c, m.arg_ptr(b), static_cast<usize>(m.arg(b + 1)),
                                     m.arg_ptr(b + 2), ArgCursor::va_list_at(m.arg_ptr(b + 4))));
            });
        add(wide ? "__stdio_common_vswprintf_p" : "__stdio_common_vsprintf_p",
            [&mm, wide, options_of, base_of](Marshaler& m) {
                const usize b = base_of(m);
                Call c{m, mm, Flavor::Ucrt, wide, Processor::Positional, options_of(m)};
                ret(m, ucrt_vsprintf(c, m.arg_ptr(b), static_cast<usize>(m.arg(b + 1)),
                                     m.arg_ptr(b + 2), ArgCursor::va_list_at(m.arg_ptr(b + 4))));
            });
        add(wide ? "__stdio_common_vswprintf_s" : "__stdio_common_vsprintf_s",
            [&mm, wide, options_of, base_of](Marshaler& m) {
                const usize b = base_of(m);
                Call c{m, mm, Flavor::Ucrt, wide, Processor::Validating, options_of(m)};
                ret(m, ucrt_vsprintf_s(c, m.arg_ptr(b), static_cast<usize>(m.arg(b + 1)),
                                       m.arg_ptr(b + 2), ArgCursor::va_list_at(m.arg_ptr(b + 4))));
            });
        // int __stdio_common_vsnprintf_s(u64 options, char* buffer,
        //     size_t count, size_t max_count, const char* format, _locale_t,
        //     va_list)
        add(wide ? "__stdio_common_vsnwprintf_s" : "__stdio_common_vsnprintf_s",
            [&mm, wide, options_of, base_of](Marshaler& m) {
                const usize b = base_of(m);
                Call c{m, mm, Flavor::Ucrt, wide, Processor::Validating, options_of(m)};
                ret(m, ucrt_vsnprintf_s(c, m.arg_ptr(b), static_cast<usize>(m.arg(b + 1)),
                                        static_cast<usize>(m.arg(b + 2)), m.arg_ptr(b + 3),
                                        ArgCursor::va_list_at(m.arg_ptr(b + 5))));
            });
    }

    // int _set_printf_count_output(int enable) / _get_printf_count_output(void)
    // (UCRT only; msvcrt.dll exports neither and never allows %n.)
    add("_set_printf_count_output", [](Marshaler& m) {
        const bool previous =
            exchange_printf_count_output(static_cast<u32>(m.arg(0)) != 0);
        m.set_return(previous ? 1u : 0u);
    });
    add("_get_printf_count_output", [](Marshaler& m) {
        m.set_return(printf_count_output_enabled() ? 1u : 0u);
    });

    // unsigned _set_output_format(unsigned format) / _get_output_format(void)
    // (msvcrt.dll only; Learn _set_output_format). Only _TWO_DIGIT_EXPONENT
    // (1) is defined; any other bit is rejected with EINVAL and leaves the
    // format alone, returning it (measured: set(3) after set(1) returns 1).
    add("_set_output_format", [&mm](Marshaler& m) {
        const u32 format = static_cast<u32>(m.arg(0));
        if ((format & ~1u) != 0) {
            mm.set_errno(static_cast<u32>(kEINVAL));
            m.set_return(msvcrt_output_format());
            return;
        }
        m.set_return(exchange_msvcrt_output_format(format));
    });
    add("_get_output_format", [](Marshaler& m) {
        m.set_return(msvcrt_output_format());
    });

    // ---- msvcrt.dll -----------------------------------------------------------
    // Every signature below is the msvcrt.dll export's (Learn: sprintf,
    // _snprintf, sprintf_s, _snprintf_s, _scprintf, _vsprintf_p families);
    // `swprintf` / `vswprintf` / `_swprintf` / `_vswprintf` are msvcrt's
    // pre-ISO forms without a count (the probe calls them that way).
    for (const bool wide : {false, true}) {
        auto call = [&mm, wide](Marshaler& m) {
            return Call{m, mm, Flavor::Msvcrt, wide};
        };
        // Unbounded: sprintf(buffer, format, ...) / vsprintf(buffer, format, va)
        auto unbounded = [call](Marshaler& m, bool va) {
            ret(m, msvcrt_snprintf(call(m), m.arg_ptr(0), std::numeric_limits<usize>::max(),
                                   m.arg_ptr(1), va ? ArgCursor::va_list_at(m.arg_ptr(2))
                                                    : ArgCursor::call_args(2)));
        };
        if (wide) {
            add("swprintf", [unbounded](Marshaler& m) { unbounded(m, false); });
            add("_swprintf", [unbounded](Marshaler& m) { unbounded(m, false); });
            add("vswprintf", [unbounded](Marshaler& m) { unbounded(m, true); });
            add("_vswprintf", [unbounded](Marshaler& m) { unbounded(m, true); });
        } else {
            add("sprintf", [unbounded](Marshaler& m) { unbounded(m, false); });
            add("vsprintf", [unbounded](Marshaler& m) { unbounded(m, true); });
        }

        // (buffer, count, format, ...) / (buffer, count, format, va)
        auto counted = [call](auto rule) {
            return [call, rule](Marshaler& m, bool va) {
                ret(m, rule(call(m), m.arg_ptr(0), static_cast<usize>(m.arg(1)),
                            m.arg_ptr(2), va ? ArgCursor::va_list_at(m.arg_ptr(3))
                                             : ArgCursor::call_args(3)));
            };
        };
        auto legacy = counted([](const Call& c, GuestAddr b, usize n, GuestAddr f, ArgCursor a) {
            return msvcrt_snprintf(c, b, n, f, a);
        });
        auto sn_c = counted([](const Call& c, GuestAddr b, usize n, GuestAddr f, ArgCursor a) {
            return msvcrt_snprintf_c(c, b, n, f, a);
        });
        auto vsn_c = counted([](const Call& c, GuestAddr b, usize n, GuestAddr f, ArgCursor a) {
            return msvcrt_vsnprintf_c(c, b, n, f, a);
        });
        auto secure = counted([](const Call& c, GuestAddr b, usize n, GuestAddr f, ArgCursor a) {
            return msvcrt_sprintf_s(c, b, n, f, a);
        });
        auto positional = counted([](const Call& c, GuestAddr b, usize n, GuestAddr f, ArgCursor a) {
            return msvcrt_vsprintf_p(c, b, n, f, a);
        });
        if (wide) {
            add("_snwprintf", [legacy](Marshaler& m) { legacy(m, false); });
            add("_vsnwprintf", [legacy](Marshaler& m) { legacy(m, true); });
            add("_swprintf_c", [sn_c](Marshaler& m) { sn_c(m, false); });
            add("_vswprintf_c", [vsn_c](Marshaler& m) { vsn_c(m, true); });
            add("swprintf_s", [secure](Marshaler& m) { secure(m, false); });
            add("vswprintf_s", [secure](Marshaler& m) { secure(m, true); });
        } else {
            add("_snprintf", [legacy](Marshaler& m) { legacy(m, false); });
            add("_vsnprintf", [legacy](Marshaler& m) { legacy(m, true); });
            add("vsnprintf", [legacy](Marshaler& m) { legacy(m, true); });
            add("_snprintf_c", [sn_c](Marshaler& m) { sn_c(m, false); });
            add("_vsnprintf_c", [vsn_c](Marshaler& m) { vsn_c(m, true); });
            add("sprintf_s", [secure](Marshaler& m) { secure(m, false); });
            add("vsprintf_s", [secure](Marshaler& m) { secure(m, true); });
            add("_vsprintf_p", [positional](Marshaler& m) { positional(m, true); });
        }

        // (buffer, count, max_count, format, ...) / (..., va)
        auto snprintf_s = [call](Marshaler& m, bool va) {
            ret(m, msvcrt_snprintf_s(call(m), m.arg_ptr(0), static_cast<usize>(m.arg(1)),
                                     static_cast<usize>(m.arg(2)), m.arg_ptr(3),
                                     va ? ArgCursor::va_list_at(m.arg_ptr(4))
                                        : ArgCursor::call_args(4)));
        };
        add(wide ? "_snwprintf_s" : "_snprintf_s", [snprintf_s](Marshaler& m) { snprintf_s(m, false); });
        add(wide ? "_vsnwprintf_s" : "_vsnprintf_s", [snprintf_s](Marshaler& m) { snprintf_s(m, true); });

        // (format, ...) / (format, va)
        auto counting = [call](Marshaler& m, bool va) {
            ret(m, count_only(call(m), m.arg_ptr(0),
                              va ? ArgCursor::va_list_at(m.arg_ptr(1))
                                 : ArgCursor::call_args(1)));
        };
        add(wide ? "_scwprintf" : "_scprintf", [counting](Marshaler& m) { counting(m, false); });
        add(wide ? "_vscwprintf" : "_vscprintf", [counting](Marshaler& m) { counting(m, true); });
    }
    return exports;
}

} // namespace vitreo::winapi
