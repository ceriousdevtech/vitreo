// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_init_registration.cpp
/// @brief Export table for msvcrt's startup and process-exit surface.
///
/// Composition only: maps export names to handlers and records each one's
/// stack-cleanup contract. msvcrt is cdecl, so most entries pop zero bytes and
/// the guest cleans its own stack.
///
/// Split out of crt_init.cpp, which was 1,149 lines and seven-eighths this table.
///
/// The export inventory this produces is verified byte-for-byte by
/// tools/architecture/check_export_inventory.sh.
///
/// VITREO_LARGE_FILE_OK: declarative ABI table — one entry per export. Plan
/// section 4.1 exempts these because length here tracks how many CRT functions
/// Vitreo implements, not how much logic one file carries.

#include <unistd.h>
#include "crt_init.hpp"
#include "crt_data.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/system/crt_environment.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/context64.hpp"
#include <algorithm>
#include <strings.h>
#include <codecvt>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <cstdlib>
#include <ctime>
#include <locale>
#include <stdlib.h>  // arc4random_buf

namespace vitreo::winapi {

// =============================================================================
// Thunk Registration
// =============================================================================


namespace {

// ---------------------------------------------------------------------------
// argv construction (09_crt.md HIGH 5, MEDIUM tokenizer, LOW argv mode).
//
// Learn "Parsing C command-line arguments" plus the Windows runs of
// tools/oracle_probe/crt_ucrt_rt_probe.c (argv / argv_unexpanded / argv2_wild
// / argv0 / wargv lines):
//   * argv[0] is the program name: a leading quote runs to the next quote
//     with no escape processing, otherwise it ends at space/tab;
//   * arguments are separated by spaces/tabs outside quotes;
//   * 2n backslashes + '"' -> n backslashes, the quote toggles quoting;
//     2n+1 backslashes + '"' -> n backslashes and a literal '"';
//     backslashes not followed by '"' are literal;
//   * inside quotes, '""' is a literal '"' and quoting continues;
//   * _crt_argv_no_arguments (0): argc 0, argv NULL;
//   * _crt_argv_expanded_arguments (2): every argument after argv[0] holding
//     '*' or '?' -- quoted or not -- is replaced by the matching names
//     (directory prefix kept, "." and ".." skipped, sorted); a pattern with
//     no match stays literal.
// The table is built at call time with the caller's pointer width: the old
// fixed block wrote 4-byte slots, so x64 argv[0] was (slot1<<32)|slot0.
// ---------------------------------------------------------------------------
template <class Str>
std::vector<Str> parse_command_line(const Str& cmd) {
    using Ch = typename Str::value_type;
    std::vector<Str> out;
    usize i = 0;
    const usize n = cmd.size();
    auto blank = [](Ch c) { return c == Ch(' ') || c == Ch('\t'); };
    // Program name.
    {
        Str name;
        if (i < n && cmd[i] == Ch('"')) {
            ++i;
            while (i < n && cmd[i] != Ch('"')) name.push_back(cmd[i++]);
            if (i < n) ++i;
        } else {
            while (i < n && !blank(cmd[i])) name.push_back(cmd[i++]);
        }
        out.push_back(std::move(name));
    }
    for (;;) {
        while (i < n && blank(cmd[i])) ++i;
        if (i >= n) break;
        Str cur;
        bool quoted = false;
        while (i < n) {
            const Ch c = cmd[i];
            if (!quoted && blank(c)) break;
            if (c == Ch('\\')) {
                usize k = 0;
                while (i + k < n && cmd[i + k] == Ch('\\')) ++k;
                if (i + k < n && cmd[i + k] == Ch('"')) {
                    cur.append(k / 2, Ch('\\'));
                    if (k % 2 == 1) {
                        cur.push_back(Ch('"'));
                        i += k + 1;
                    } else {
                        i += k;  // the quote is handled next iteration
                    }
                } else {
                    cur.append(k, Ch('\\'));
                    i += k;
                }
                continue;
            }
            if (c == Ch('"')) {
                if (quoted && i + 1 < n && cmd[i + 1] == Ch('"')) {
                    cur.push_back(Ch('"'));
                    i += 2;
                    continue;
                }
                quoted = !quoted;
                ++i;
                continue;
            }
            cur.push_back(c);
            ++i;
        }
        out.push_back(std::move(cur));
    }
    return out;
}

bool wildcard_match(const std::string& pattern, const std::string& name) {
    auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    };
    usize p = 0, s = 0, star = std::string::npos, mark = 0;
    while (s < name.size()) {
        if (p < pattern.size() &&
            (pattern[p] == '?' || lower(pattern[p]) == lower(name[s]))) {
            ++p; ++s;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = s;
        } else if (star != std::string::npos) {
            p = star + 1;
            s = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

std::vector<std::string> expand_wildcards(const CrtDataBlock& crt_data,
                                          std::vector<std::string> args) {
    const auto& lister = crt_data.directory_lister();
    std::vector<std::string> out;
    for (usize a = 0; a < args.size(); ++a) {
        const std::string& arg = args[a];
        if (a == 0 || arg.find_first_of("*?") == std::string::npos || !lister) {
            out.push_back(arg);
            continue;
        }
        const usize cut = arg.find_last_of("\\/:");
        const std::string prefix = cut == std::string::npos ? "" : arg.substr(0, cut + 1);
        const std::string pattern = arg.substr(prefix.size());
        std::vector<std::string> matches;
        if (auto names = lister(prefix)) {
            for (const auto& name : *names) {
                if (name == "." || name == "..") continue;
                if (wildcard_match(pattern, name)) matches.push_back(prefix + name);
            }
        }
        if (matches.empty()) {
            out.push_back(arg);
            continue;
        }
        std::sort(matches.begin(), matches.end(),
                  [](const std::string& x, const std::string& y) {
                      return strcasecmp(x.c_str(), y.c_str()) < 0;
                  });
        out.insert(out.end(), matches.begin(), matches.end());
    }
    return out;
}

/// The command line as UTF-16 units (Latin-1 widening of the ANSI text, the
/// same conversion _wcmdln is built with).
std::u16string wide_command_line(const CrtDataBlock& crt_data) {
    const std::string& narrow = crt_data.command_line();
    std::u16string wide;
    wide.reserve(narrow.size());
    for (char c : narrow) wide.push_back(static_cast<unsigned char>(c));
    return wide;
}

std::vector<std::string> narrow_args(const CrtDataBlock& crt_data, int mode) {
    if (mode == 0) return {};
    auto args = parse_command_line(crt_data.command_line());
    if (mode == 2) args = expand_wildcards(crt_data, std::move(args));
    return args;
}

std::vector<std::wstring> wide_args(const CrtDataBlock& crt_data, int mode) {
    std::vector<std::wstring> out;
    if (mode == 0) return out;
    std::vector<std::u16string> parsed = parse_command_line(wide_command_line(crt_data));
    if (mode == 2) {
        // Expansion works on the (Latin-1) narrow spelling of each argument.
        std::vector<std::string> narrow;
        for (const auto& a : parsed) {
            std::string n8;
            for (char16_t c : a) n8.push_back(static_cast<char>(c & 0xFF));
            narrow.push_back(std::move(n8));
        }
        narrow = expand_wildcards(crt_data, std::move(narrow));
        parsed.clear();
        for (const auto& a : narrow) {
            std::u16string w;
            for (char c : a) w.push_back(static_cast<unsigned char>(c));
            parsed.push_back(std::move(w));
        }
    }
    for (const auto& a : parsed) out.emplace_back(a.begin(), a.end());
    return out;
}

/// Build __argv (and __argc) for the calling image's bitness. Returns the
/// published table (0 for mode 0).
GuestAddr publish_narrow_argv(Marshaler& m, const CrtDataBlock& crt_data,
                              CrtEnvironmentManager& env_mgr, int mode) {
    const bool w64 = m.cpu_is_64bit();
    const auto args = narrow_args(crt_data, mode);
    GuestAddr table = 0;
    if (mode != 0) table = env_mgr.build_narrow_string_table(args, w64);
    (void)m.write_u32(crt_data.argc_addr(), static_cast<u32>(args.size()));
    (void)m.write_ptr(crt_data.argv_var_addr(), table);
    return table;
}

GuestAddr publish_wide_argv(Marshaler& m, const CrtDataBlock& crt_data,
                            CrtEnvironmentManager& env_mgr, int mode) {
    const bool w64 = m.cpu_is_64bit();
    const auto args = wide_args(crt_data, mode);
    GuestAddr table = 0;
    if (mode != 0) table = env_mgr.build_wide_string_table(args, w64);
    (void)m.write_u32(crt_data.argc_addr(), static_cast<u32>(args.size()));
    (void)m.write_ptr(crt_data.wargv_var_addr(), table);
    return table;
}

/// argv as the fixed block (x86 default) or a previous publish left it.
std::vector<std::wstring> data_block_args(Marshaler& m,
                                          const CrtDataBlock& crt_data) {
    (void)m;
    return wide_args(crt_data, 1);
}
} // namespace

DllExports create_crt_init_thunks(
    CrtInitManager& mgr, CrtDataBlock& crt_data,
    CrtEnvironmentManager& env_mgr) {
    DllExports exports;
    exports.entries.reserve(20);

    // int* __p___argc(void)
    //
    // The UCRT accessor returns a stable pointer to writable CRT process
    // state. Returning the mapped data cell (rather than an integer value)
    // is required because callers immediately dereference the result.
    exports.entries.push_back(ExportEntry{
        "__p___argc", 0,
        [&crt_data](Marshaler& m) {
            m.set_return(crt_data.argc_addr());
        },
        0
    });

    // char*** __p___argv(void) — returns &__argv so callers can read/write
    // the `char**` process argv pointer.
    exports.entries.push_back(ExportEntry{
        "__p___argv", 0,
        [&crt_data, &env_mgr](Marshaler& m) {
            // The fixed data block still seeds a 4-byte argv table. Upgrade it
            // to pointer width on first x64 touch — but never overwrite a table
            // (or NULL) that _configure_narrow_argv already published
            // (crt_ucrt_rt_probe argv0 / argv2_wild).
            if (m.cpu_is_64bit()) {
                const auto cur = m.read_u64(crt_data.argv_var_addr());
                if (cur.is_ok() &&
                    cur.value() == crt_data.argv_table_addr()) {
                    (void)publish_narrow_argv(m, crt_data, env_mgr, 1);
                }
            }
            m.set_return(crt_data.argv_var_addr());
        },
        0
    });

    // wchar_t*** __p___wargv(void) — returns &__wargv.
    //
    // The missing-export stub returned 0xFFFFFFFF and every wide-char CRT
    // startup dereferences the result immediately: pwsh.exe died on
    // `mov rbx,[rax]` with rax=0xffffffff, 601 instructions in. The cell has
    // to exist AND point at a real table, so build the table on first use —
    // unless _configure_wide_argv(0) intentionally left argv NULL / argc 0.
    exports.entries.push_back(ExportEntry{
        "__p___wargv", 0,
        [&mgr, &crt_data, &env_mgr](Marshaler& m) {
            (void)mgr;
            const GuestAddr cell = crt_data.wargv_var_addr();
            const bool w64 = m.cpu_is_64bit();
            u64 current = 0;
            if (w64) {
                const auto v = m.read_u64(cell);
                if (v.is_ok()) current = v.value();
            } else {
                const auto v = m.read_u32(cell);
                if (v.is_ok()) current = v.value();
            }
            if (current == 0) {
                const u32 argc =
                    m.read_u32(crt_data.argc_addr()).value_or(0);
                // Mode 0: argc 0 and argv NULL — leave the NULL alone.
                if (argc != 0) {
                    const GuestAddr table = env_mgr.build_wide_string_table(
                        data_block_args(m, crt_data), w64);
                    (void)m.write_ptr(cell, table);
                }
            }
            m.set_return(cell);
        },
        0
    });

    // wchar_t** _get_initial_wide_environment(void)
    // char**    _get_initial_narrow_environment(void)
    //
    // UCRT startup calls these to seed __winitenv/__initenv. They return the
    // ARRAY, not the variable cell.
    exports.entries.push_back(ExportEntry{
        "_get_initial_wide_environment", 0,
        [&env_mgr](Marshaler& m) {
            m.set_return(env_mgr.wenviron_array_addr(m.cpu_is_64bit()));
        },
        0
    });
    exports.entries.push_back(ExportEntry{
        "_get_initial_narrow_environment", 0,
        [&env_mgr](Marshaler& m) {
            m.set_return(env_mgr.environ_array_addr(m.cpu_is_64bit()));
        },
        0
    });

    // char*** __p___initenv(void) / wchar_t*** __p___winitenv(void)
    //
    // CRT startup writes the environment block it was handed through these
    // accessors, so they must return the address of a writable cell. The
    // missing-export stub returned 0xFFFFFFFF, which every mingw-built image
    // stores through before it reaches main.
    exports.entries.push_back(ExportEntry{
        "__p___initenv", 0,
        [&crt_data](Marshaler& m) { m.set_return(crt_data.initenv_addr()); },
        0
    });
    exports.entries.push_back(ExportEntry{
        "__p___winitenv", 0,
        [&crt_data](Marshaler& m) { m.set_return(crt_data.winitenv_addr()); },
        0
    });

    exports.entries.push_back(ExportEntry{
        "_initterm", 1,
        [&mgr](Marshaler& m) {
            // void _initterm(void(**first)(void), void(**last)(void))
            // In 32-bit mode, each table entry is a 4-byte function pointer.
            GuestAddr first = m.arg_ptr(0);
            GuestAddr last  = m.arg_ptr(1);
            const usize ptr_size = m.cpu_is_64bit() ? 8u : 4u;
            if (!mgr.has_initterm_callback()) {
                // Never report success for initializers that did not run.
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED msvcrt!_initterm: no guest-call "
                    "callback installed; %llu initializers not run\n",
                    static_cast<unsigned long long>(
                        last > first ? (last - first) / ptr_size : 0));
                return;
            }
            for (GuestAddr p = first; p < last; p += ptr_size) {
                GuestAddr func_ptr = 0;
                if (ptr_size == 8) {
                    auto rv = m.read_u64(p);
                    if (rv.is_ok()) func_ptr = static_cast<GuestAddr>(rv.value());
                } else {
                    auto rv = m.read_u32(p);
                    if (rv.is_ok()) func_ptr = rv.value();
                }
                if (func_ptr != 0) {
                    VITREO_TRACE( "[init] calling guest initializer at 0x%x\n",
                                 static_cast<unsigned>(func_ptr));
                    mgr.call_guest_init(func_ptr);
                }
            }
        }
    });
    exports.entries.push_back(ExportEntry{
        "_initterm_e", 2,
        [&mgr](Marshaler& m) {
            // int _initterm_e(int(**first)(void), int(**last)(void))
            // Walk table of int(*)(void) pointers, call each non-null one.
            // If any returns non-zero, stop and return that error code.
            GuestAddr first = m.arg_ptr(0);
            GuestAddr last  = m.arg_ptr(1);
            const usize ptr_size = m.cpu_is_64bit() ? 8u : 4u;
            std::fprintf(stderr, "[init] _initterm_e: first=0x%x last=0x%x ptr_size=%zu callback=%s\n",
                         static_cast<unsigned>(first), static_cast<unsigned>(last),
                         ptr_size, mgr.has_initterm_e_callback() ? "yes" : "no");
            if (!mgr.has_initterm_e_callback()) {
                m.set_return(0);
                return;
            }
            for (GuestAddr p = first; p < last; p += ptr_size) {
                GuestAddr func_ptr = 0;
                if (ptr_size == 8) {
                    auto rv = m.read_u64(p);
                    if (rv.is_ok()) func_ptr = static_cast<GuestAddr>(rv.value());
                } else {
                    auto rv = m.read_u32(p);
                    if (rv.is_ok()) func_ptr = rv.value();
                }
                if (func_ptr != 0) {
                    std::fprintf(stderr, "[init] _initterm_e calling guest initializer at 0x%x\n",
                                 static_cast<unsigned>(func_ptr));
                    u64 ret = mgr.call_guest_init_e(func_ptr);
                    if (ret != 0) {
                        std::fprintf(stderr,
                            "[init] _initterm_e: initializer at 0x%x returned error %llu\n",
                            static_cast<unsigned>(func_ptr), static_cast<unsigned long long>(ret));
                        m.set_return(static_cast<u64>(ret));
                        return;
                    }
                }
            }
            m.set_return(0);
        }
    });
    // int _configure_narrow_argv(_crt_argv_mode mode) /
    // int _configure_wide_argv(_crt_argv_mode mode) -- UCRT startup builds
    // __argc/__argv (__wargv) from the command line: 0 = no arguments,
    // 1 = unexpanded, 2 = wildcards expanded. Returns 0 (errno_t) on success.
    exports.entries.push_back(ExportEntry{
        "_configure_narrow_argv", 0,
        [&crt_data, &env_mgr](Marshaler& m) {
            const int mode = static_cast<int>(static_cast<i32>(m.arg(0)));
            if (mode < 0 || mode > 2) {
                m.set_return(22u);  // EINVAL
                return;
            }
            (void)publish_narrow_argv(m, crt_data, env_mgr, mode);
            m.set_return(0);
        }
    });
    exports.entries.push_back(ExportEntry{
        "_configure_wide_argv", 0,
        [&crt_data, &env_mgr](Marshaler& m) {
            const int mode = static_cast<int>(static_cast<i32>(m.arg(0)));
            if (mode < 0 || mode > 2) {
                m.set_return(22u);  // EINVAL
                return;
            }
            (void)publish_wide_argv(m, crt_data, env_mgr, mode);
            m.set_return(0);
        }
    });
    // int __getmainargs(int* pArgc, char*** pArgv, char*** pEnv,
    //                   int doWildCard, _startupinfo* startInfo)
    // Hands back the real argv/environment arrays, pointer-width per bitness
    // (a null argv sends mingw's CRT to _amsg_exit(_RT_SPACEARG)).
    exports.entries.push_back(ExportEntry{
        "__getmainargs", 3,
        [&crt_data, &env_mgr](Marshaler& m) {
            const GuestAddr p_argc = m.arg_ptr(0);
            const GuestAddr p_argv = m.arg_ptr(1);
            const GuestAddr p_env  = m.arg_ptr(2);
            const int mode = static_cast<u32>(m.arg(3)) != 0 ? 2 : 1;
            const bool w64 = m.cpu_is_64bit();
            const GuestAddr table = publish_narrow_argv(m, crt_data, env_mgr, mode);
            if (p_argc != 0) {
                (void)m.write_u32(p_argc, m.read_u32(crt_data.argc_addr()).value_or(0));
            }
            if (p_argv != 0) (void)m.write_ptr(p_argv, table);
            if (p_env != 0) {
                (void)m.write_ptr(p_env, env_mgr.environ_array_addr(w64));
            }
            m.set_return(0);
        }
    });
    exports.entries.push_back(ExportEntry{
        "__wgetmainargs", 4,
        [&crt_data, &env_mgr](Marshaler& m) {
            // int __wgetmainargs(int* pArgc, wchar_t*** pArgv, wchar_t*** pEnv,
            //                    int doWildCard, _startupinfo* startInfo)
            const GuestAddr p_argc = m.arg_ptr(0);
            const GuestAddr p_argv = m.arg_ptr(1);
            const GuestAddr p_env  = m.arg_ptr(2);
            const int mode = static_cast<u32>(m.arg(3)) != 0 ? 2 : 1;
            const bool w64 = m.cpu_is_64bit();
            const GuestAddr table = publish_wide_argv(m, crt_data, env_mgr, mode);
            if (p_argc != 0) {
                (void)m.write_u32(p_argc, m.read_u32(crt_data.argc_addr()).value_or(0));
            }
            if (p_argv != 0) (void)m.write_ptr(p_argv, table);
            if (p_env != 0) {
                (void)m.write_ptr(p_env, env_mgr.wenviron_array_addr(w64));
            }
            // Measured (msvcrt_exception_initenv_probe): __wgetmainargs also
            // stores the environment in __winitenv; __getmainargs leaves
            // __initenv alone.
            (void)m.write_ptr(crt_data.winitenv_addr(), env_mgr.wenviron_array_addr(w64));
            m.set_return(0);
        }
    });
    exports.entries.push_back(ExportEntry{
        "_amsg_exit", 5,
        [&mgr](Marshaler& m) {
            u32 rterrnum = static_cast<u32>(m.arg(0));
            mgr.amsg_exit(static_cast<int>(rterrnum));
        }
    });
    exports.entries.push_back(ExportEntry{
        "__set_app_type", 6,
        [&mgr](Marshaler& m) {
            u32 app_type = static_cast<u32>(m.arg(0));
            mgr.set_app_type(static_cast<int>(app_type));
        }
    });
    // UCRT name; same contract as __set_app_type.
    // https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/set-app-type
    exports.entries.push_back(ExportEntry{
        "_set_app_type", 0,
        [&mgr](Marshaler& m) {
            mgr.set_app_type(static_cast<int>(m.arg(0)));
        }
    });
    // errno_t _set_fmode(int mode) — _O_TEXT=0x4000, _O_BINARY=0x8000.
    // https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/set-fmode
    exports.entries.push_back(ExportEntry{
        "_set_fmode", 0,
        [&crt_data](Marshaler& m) {
            const int mode = static_cast<int>(m.arg(0));
            constexpr int kOText = 0x4000;
            constexpr int kOBinary = 0x8000;
            constexpr int kOWText = 0x10000;
            if (mode != kOText && mode != kOBinary && mode != kOWText) {
                m.set_return(22); // EINVAL
                return;
            }
            (void)m.write_u32(crt_data.fmode_addr(), static_cast<u32>(mode));
            m.set_return(0);
        }
    });
    // char* _get_narrow_winmain_command_line(void)
    // https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/get-narrow-winmain-command-line
    exports.entries.push_back(ExportEntry{
        "_get_narrow_winmain_command_line", 0,
        [&crt_data](Marshaler& m) {
            m.set_return(crt_data.base() + CrtDataBlock::CMDLINE_STR_OFFSET);
        }
    });
    // wchar_t* _get_wide_winmain_command_line(void), zero arguments, cdecl.
    exports.entries.push_back(ExportEntry{
        "_get_wide_winmain_command_line", 0,
        [&crt_data](Marshaler& m) {
            GuestAddr cursor = m.read_ptr(crt_data.wcmdln_addr()).value_or(0);
            if (cursor == 0) {
                m.set_return(crt_data.base() + CrtDataBlock::EMPTY_WSTRING_OFFSET);
                return;
            }
            // Windows UCRT oracle: quotes toggle the program-name region;
            // backslashes do not escape them here. Skip all U+0001..U+0020
            // separators, preserving the remaining command line verbatim.
            bool quoted = false;
            for (;;) {
                auto ch = m.read_u16(cursor);
                if (ch.is_err()) { m.set_return(0); return; }
                if (ch.value() == 0 || (!quoted && ch.value() <= u' ')) break;
                if (ch.value() == u'"') quoted = !quoted;
                cursor += sizeof(u16);
            }
            for (;;) {
                auto ch = m.read_u16(cursor);
                if (ch.is_err()) { m.set_return(0); return; }
                if (ch.value() == 0 || ch.value() > u' ') break;
                cursor += sizeof(u16);
            }
            m.set_return(cursor);
        }
    });

    // int __crtGetShowWindowMode(void) — CRT helper over GetStartupInfo.
    // STARTF_USESHOWWINDOW is clear in Vitreo's STARTUPINFO, so SW_SHOWDEFAULT.
    exports.entries.push_back(ExportEntry{
        "__crtGetShowWindowMode", 0,
        [](Marshaler& m) {
            constexpr u32 kSwShowDefault = 10;
            m.set_return(kSwShowDefault);
        }
    });
    // LPTOP_LEVEL_EXCEPTION_FILTER __crtSetUnhandledExceptionFilter(...)
    exports.entries.push_back(ExportEntry{
        "__crtSetUnhandledExceptionFilter", 0,
        [](Marshaler& m) {
            const u64 previous = set_unhandled_exception_filter_guest(m.arg(0));
            m.set_return(previous);
        }
    });
    // int _ismbblead(unsigned int c) — C locale has no lead bytes.
    // https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/ismbblead-ismbblead-l
    exports.entries.push_back(ExportEntry{
        "_ismbblead", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            m.set_return(0);
        }
    });
    exports.entries.push_back(ExportEntry{
        "_seh_filter_exe", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            (void)m.arg(1);
            m.set_return(0); // EXCEPTION_CONTINUE_SEARCH
        }
    });
    exports.entries.push_back(ExportEntry{
        "_register_thread_local_exe_atexit_callback", 0,
        [](Marshaler& m) { (void)m.arg(0); }
    });
    // void __crtCapturePreviousContext(CONTEXT* ctx) — VC2013+ CRT helper
    // over RtlCaptureContext. x64 CONTEXT layout only; x86 callers zero the
    // record themselves before the call.
    exports.entries.push_back(ExportEntry{
        "__crtCapturePreviousContext", 0,
        [](Marshaler& m) {
            if (m.cpu_is_64bit()) {
                write_context64(m, m.arg_ptr(0));
            }
        }
    });
    // LONG __crtUnhandledException(EXCEPTION_POINTERS*) — CRT forwards to
    // UnhandledExceptionFilter; with no filter installed, continue search.
    exports.entries.push_back(ExportEntry{
        "__crtUnhandledException", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            m.set_return(0); // EXCEPTION_CONTINUE_SEARCH
        }
    });
    // void __crt_debugger_hook(int) — breakpoint site when a debugger is
    // attached; no-op otherwise.
    exports.entries.push_back(ExportEntry{
        "__crt_debugger_hook", 0,
        [](Marshaler& m) { (void)m.arg(0); }
    });
    // _cexit / _c_exit live with exit() in create_crt_process_thunks: they
    // run the atexit list owned by CrtMemoryManager.
    exports.entries.push_back(ExportEntry{
        // Learn (_getpid): the calling process's identifier — the same value
        // GetCurrentProcessId returns, which is what the guest sees elsewhere.
        "_getpid", 0,
        [](Marshaler& m) {
            m.set_return(static_cast<u64>(::getpid()));
        },
        0});
    exports.entries.push_back(ExportEntry{
        "_configthreadlocale", 9,
        [&mgr](Marshaler& m) {
            u32 type = static_cast<u32>(m.arg(0));
            int result = mgr.config_thread_locale(static_cast<int>(type));
            m.set_return(static_cast<u64>(result));
        }
    });
    exports.entries.push_back(ExportEntry{
        "_set_new_mode", 10,
        [&mgr](Marshaler& m) {
            u32 mode = static_cast<u32>(m.arg(0));
            int prev = mgr.set_new_mode(static_cast<int>(mode));
            m.set_return(static_cast<u64>(prev));
        }
    });

    return exports;
}

} // namespace vitreo::winapi
