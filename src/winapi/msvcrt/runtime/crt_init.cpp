/// @file crt_init.cpp
/// @brief msvcrt CRT initialization (WIN32-067)

#include "winapi/common/c_specific_handler.hpp"
#include "crt_init.hpp"
#include "crt_data.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"

#include <algorithm>
#include <codecvt>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <cstdlib>
#include <ctime>
#include <locale>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <stdlib.h>  // arc4random_buf
#endif

namespace vitreo::winapi {

namespace {

// Shared CRT signal table (signal / raise / abort / _seh_filter_exe).
std::mutex g_signal_lock;
std::unordered_map<i32, GuestAddr> g_signal_handlers;
// _WRITE_ABORT_MSG (1) | _CALL_REPORTFAULT (2); both default on.
u32 g_abort_behavior = 0x3;

constexpr i32 kSigDfl = 0;
constexpr i32 kSigIgn = 1;
constexpr u32 kEinval = 22;
constexpr u32 kCallReportFault = 0x2;
constexpr u32 kStatusStackBufferOverrun = 0xC0000409u;

[[nodiscard]] bool crt_signal_number_ok(i32 sig) noexcept {
    // SIGINT/ILL/FPE/SEGV/TERM/BREAK/ABRT — signal.h
    static constexpr i32 kValid[] = {2, 4, 8, 11, 15, 21, 22};
    return std::find(std::begin(kValid), std::end(kValid), sig) !=
           std::end(kValid);
}

/// SIG_GET(2)/SIG_SGE(3)/SIG_ACK(4) are not installable handlers.
[[nodiscard]] bool crt_signal_handler_ok(GuestAddr handler) noexcept {
    return handler == kSigDfl || handler == kSigIgn || handler > 4;
}

GuestAddr crt_signal_exchange(i32 sig, GuestAddr handler) {
    std::lock_guard<std::mutex> guard(g_signal_lock);
    auto it = g_signal_handlers.find(sig);
    const GuestAddr previous =
        it != g_signal_handlers.end() ? it->second : GuestAddr{0};
    g_signal_handlers[sig] = handler;
    return previous;
}

/// Map NTSTATUS → CRT signal number, or 0 if unmapped (Learn _seh_filter_*).
[[nodiscard]] i32 crt_exception_to_signal(u32 code) noexcept {
    switch (code) {
        case 0xC0000005u: return 11; // STATUS_ACCESS_VIOLATION → SIGSEGV
        case 0xC000001Du: return 4;  // STATUS_ILLEGAL_INSTRUCTION → SIGILL
        case 0xC0000096u: return 4;  // STATUS_PRIVILEGED_INSTRUCTION → SIGILL
        case 0xC000008Du: // STATUS_FLOAT_DENORMAL_OPERAND
        case 0xC000008Eu: // STATUS_FLOAT_DIVIDE_BY_ZERO
        case 0xC000008Fu: // STATUS_FLOAT_INEXACT_RESULT
        case 0xC0000090u: // STATUS_FLOAT_INVALID_OPERATION
        case 0xC0000091u: // STATUS_FLOAT_OVERFLOW
        case 0xC0000092u: // STATUS_FLOAT_STACK_CHECK
        case 0xC0000093u: // STATUS_FLOAT_UNDERFLOW
            return 8; // SIGFPE
        default:
            return 0;
    }
}

} // namespace




namespace {

/// MSVC keeps rand()'s seed per thread and starts it at 1.
inline u32& crt_rand_seed() noexcept {
    static thread_local u32 seed = 1u;
    return seed;
}



/// MSVC's 32-bit _JUMP_BUFFER field offsets.
struct JmpBuf32 {
    static constexpr u64 EBP = 0;
    static constexpr u64 EBX = 4;
    static constexpr u64 EDI = 8;
    static constexpr u64 ESI = 12;
    static constexpr u64 ESP = 16;
    static constexpr u64 EIP = 20;
    static constexpr u64 REGISTRATION = 24;  ///< SEH chain head at setjmp time
    static constexpr u64 TRY_LEVEL = 28;
};

/// The integer slots of MSVC's x64 _JUMP_BUFFER. The XMM save area that
/// follows is not restored: Vitreo's longjmp resumes at a call boundary,
/// where the Windows x64 ABI already treats XMM6-XMM15 as caller-preserved
/// through the setjmp site.
struct JmpBuf64 {
    static constexpr u64 FRAME = 0;
    static constexpr u64 RBX = 8;
    static constexpr u64 RSP = 16;
    static constexpr u64 RBP = 24;
    static constexpr u64 RSI = 32;
    static constexpr u64 RDI = 40;
    static constexpr u64 R12 = 48;
    static constexpr u64 R13 = 56;
    static constexpr u64 R14 = 64;
    static constexpr u64 R15 = 72;
    static constexpr u64 RIP = 80;
};

} // namespace

CrtInitManager::CrtInitManager() {
    m_argv.push_back("vitreo_guest.exe");
}

void CrtInitManager::initterm(const std::vector<VoidFunc>& funcs) {
    for (const auto& f : funcs) {
        if (f) f();
    }
}

int CrtInitManager::initterm_e(const std::vector<IntFunc>& funcs) {
    for (const auto& f : funcs) {
        if (f) {
            int result = f();
            if (result != 0) return result;
        }
    }
    return 0;
}

void CrtInitManager::set_command_line(const std::string& program, const std::vector<std::string>& args) {
    m_argv.clear();
    m_argv.push_back(program);
    for (const auto& a : args) {
        m_argv.push_back(a);
    }
    m_wargv_dirty = true;
}

int CrtInitManager::get_argc() const noexcept {
    return static_cast<int>(m_argv.size());
}

const std::vector<std::string>& CrtInitManager::get_argv() const noexcept {
    return m_argv;
}

const std::vector<std::wstring>& CrtInitManager::get_wargv() const {
    if (m_wargv_dirty) {
        auto* self = const_cast<CrtInitManager*>(this);
        self->m_wargv.clear();
        for (const auto& s : m_argv) {
            self->m_wargv.emplace_back(s.begin(), s.end());
        }
        self->m_wargv_dirty = false;
    }
    return m_wargv;
}

void CrtInitManager::amsg_exit(int rterrnum) {
    m_last_error_num = rterrnum;
    m_exit_requested = true;
}

bool CrtInitManager::exit_requested() const noexcept { return m_exit_requested; }
int CrtInitManager::last_error_num() const noexcept { return m_last_error_num; }

void CrtInitManager::set_app_type(int app_type) { m_app_type = app_type; }
int CrtInitManager::get_app_type() const noexcept { return m_app_type; }

void CrtInitManager::cexit() { /* no-op: no atexit registry */ }
void CrtInitManager::c_exit() { /* no-op: quick exit */ }

int CrtInitManager::config_thread_locale(int type) {
    int prev = m_thread_locale;
    if (type != 0) {
        m_thread_locale = type;
    }
    return prev;
}

int CrtInitManager::set_new_mode(int mode) {
    int prev = m_new_mode;
    m_new_mode = mode;
    return prev;
}


DllExports create_crt_process_thunks(ProcessManager& pm, CrtDataBlock& crt_data, ThreadManager& tm,
                                    CrtMemoryManager& memory_manager) {
    DllExports exports;

    // void exit(int status)
    // Learn exit: calls, in LIFO order, the functions registered by atexit /
    // _onexit (_crt_atexit for a UCRT EXE), flushes all file buffers, then
    // terminates. crt_ucrt_rt_probe: "exit: crt_atexit_c atexit_b atexit_a
    // -> exit=0x5", exit_flushed "buffered". Vitreo's streams are unbuffered,
    // so every fwrite has already reached the file.
    exports.entries.push_back(ExportEntry{
        "exit", 0,
        [&pm, &memory_manager](Marshaler& m) {
            const auto status = static_cast<u32>(m.arg(0));
            memory_manager.run_atexit_handlers();
            pm.exit_process(status);
        }
    });

    // __declspec(noreturn) void quick_exit(int status)
    // Learn quick_exit: calls, LIFO, the functions registered by
    // at_quick_exit (_crt_at_quick_exit), none registered by atexit/_onexit,
    // then _Exit(status). crt_ucrt_s6_probe "q" rows: lifo "CBA" exit 7 with
    // an atexit function not run; "flush": buffered stream output still
    // reaches the file (Vitreo's streams are unbuffered, so it already has).
    exports.entries.push_back(ExportEntry{
        "quick_exit", 0,
        [&pm, &memory_manager](Marshaler& m) {
            const auto status = static_cast<u32>(m.arg(0));
            memory_manager.run_quick_exit_handlers();
            pm.exit_process(status);
        }
    });

    // void _cexit(void): same cleanup as exit() -- atexit list LIFO, flush --
    // then RETURNS to the caller (crt_ucrt_rt_probe: "cexit: atexit_b
    // atexit_a after_cexit"). _c_exit returns without running them.
    exports.entries.push_back(ExportEntry{
        "_cexit", 7,
        [&memory_manager](Marshaler& m) {
            (void)m;
            memory_manager.run_atexit_handlers();
        }
    });
    exports.entries.push_back(ExportEntry{
        "_c_exit", 8,
        [](Marshaler& m) { (void)m; }
    });

    // _exit — terminate via ProcessManager
    exports.entries.push_back(ExportEntry{
        "_exit", 0,
        [&pm](Marshaler& m) {
            pm.exit_process(static_cast<u32>(m.arg(0)));
        }
    });

    // void __crtTerminateProcess(UINT uExitCode) — VC CRT wrapper around
    // TerminateProcess(GetCurrentProcess(), uExitCode).
    exports.entries.push_back(ExportEntry{
        "__crtTerminateProcess", 0,
        [&pm](Marshaler& m) {
            pm.exit_process(static_cast<u32>(m.arg(0)));
        }
    });

    // ?terminate@@YAXXZ — C++ std::terminate
    exports.entries.push_back(ExportEntry{
        "?terminate@@YAXXZ", 0,
        [&pm](Marshaler& m) {
            (void)m;
            pm.exit_process(3);
        }
    });

    // ---- CRT termination policy --------------------------------------------
    //
    // These carry real state: apps read the value each returns to decide
    // whether their configuration took effect, and a failure-shaped return
    // (the generic missing-export stub's 0xFFFFFFFF) is a meaningful error for
    // every one of them.

    // unsigned _set_abort_behavior(unsigned flags, unsigned mask) -> old flags
    // _WRITE_ABORT_MSG (1) | _CALL_REPORTFAULT (2); both default on.
    exports.entries.push_back(ExportEntry{
        "_set_abort_behavior", 0,
        [](Marshaler& m) {
            const auto flags = static_cast<u32>(m.arg(0));
            const auto mask  = static_cast<u32>(m.arg(1));
            const u32 previous = g_abort_behavior;
            g_abort_behavior = (g_abort_behavior & ~mask) | (flags & mask);
            m.set_return(previous);
        }
    });

    // int _set_error_mode(int mode) -> previous mode, or -1 for an invalid mode
    exports.entries.push_back(ExportEntry{
        "_set_error_mode", 0,
        [](Marshaler& m) {
            static i32 s_error_mode = 0;  // _OUT_TO_DEFAULT
            const auto mode = static_cast<i32>(m.arg(0));
            constexpr i32 REPORT_ERRMODE = 3;  // query without changing
            if (mode == REPORT_ERRMODE) {
                m.set_return(static_cast<u32>(s_error_mode));
                return;
            }
            if (mode < 0 || mode > 2) {
                m.set_return(0xFFFFFFFFu);
                return;
            }
            const i32 previous = s_error_mode;
            s_error_mode = mode;
            m.set_return(static_cast<u32>(previous));
        }
    });

    // void (*signal(int sig, void (*handler)(int)))(int) -> previous handler
    //
    // Learn signal: invalid sig or SIG_ACK/GET/SGE → SIG_ERR and errno EINVAL.
    // Handlers are delivered by raise / abort / _seh_filter_exe.
    exports.entries.push_back(ExportEntry{
        "signal", 0,
        [&memory_manager](Marshaler& m) {
            const auto sig = static_cast<i32>(m.arg(0));
            const GuestAddr handler = m.arg_ptr(1);
            if (!crt_signal_number_ok(sig) || !crt_signal_handler_ok(handler)) {
                memory_manager.set_errno(kEinval);
                // SIG_ERR is (void(*)(int))-1 — full pointer width on x64.
                m.set_return(~u64{0});
                return;
            }
            m.set_return(crt_signal_exchange(sig, handler));
        }
    });

    // int raise(int sig)
    //
    // Oracle (crt_ucrt_rt_probe): custom handler → reset to SIG_DFL, call it,
    // return 0; SIG_IGN → return 0; SIG_DFL → terminate(3); invalid →
    // invalid-parameter + errno EINVAL + return -1.
    exports.entries.push_back(ExportEntry{
        "raise", 0,
        [&pm, &memory_manager](Marshaler& m) {
            const auto sig = static_cast<i32>(m.arg(0));
            if (!crt_signal_number_ok(sig)) {
                memory_manager.set_errno(kEinval);
                (void)memory_manager.invalid_parameter();
                m.set_return(static_cast<u64>(static_cast<i32>(-1)));
                return;
            }
            GuestAddr handler = 0;
            {
                std::lock_guard<std::mutex> guard(g_signal_lock);
                auto it = g_signal_handlers.find(sig);
                if (it != g_signal_handlers.end()) handler = it->second;
                // Reset before invoking (UCRT / ISO C).
                if (handler > 4) g_signal_handlers[sig] = kSigDfl;
            }
            if (handler == kSigIgn) {
                m.set_return(0);
                return;
            }
            if (handler > 4) {
                if (memory_manager.has_guest_caller()) {
                    const u64 args[1] = {static_cast<u64>(static_cast<u32>(sig))};
                    (void)memory_manager.call_guest(handler, args, 1);
                }
                m.set_return(0);
                return;
            }
            // SIG_DFL — terminate (oracle: SIGTERM default also exits 3).
            pm.exit_process(3);
        }
    });

    // int _seh_filter_exe(unsigned long, EXCEPTION_POINTERS*)
    // Overrides the CONTINUE_SEARCH stub in create_crt_init_thunks (last wins).
    // DLL filter stays CONTINUE_SEARCH always (oracle seh_filter_dll_*).
    exports.entries.push_back(ExportEntry{
        "_seh_filter_exe", 0,
        [&memory_manager](Marshaler& m) {
            constexpr i32 kContinueSearch = 0;
            constexpr i32 kContinueExecution = -1;
            const auto code = static_cast<u32>(m.arg(0));
            (void)m.arg(1);
            const i32 sig = crt_exception_to_signal(code);
            if (sig == 0) {
                m.set_return(static_cast<u64>(kContinueSearch));
                return;
            }
            GuestAddr handler = 0;
            {
                std::lock_guard<std::mutex> guard(g_signal_lock);
                auto it = g_signal_handlers.find(sig);
                if (it != g_signal_handlers.end()) handler = it->second;
                if (handler > 4) g_signal_handlers[sig] = kSigDfl;
            }
            if (handler == kSigDfl) {
                m.set_return(static_cast<u64>(kContinueSearch));
                return;
            }
            if (handler == kSigIgn) {
                m.set_return(static_cast<u64>(static_cast<i32>(kContinueExecution)));
                return;
            }
            if (handler > 4 && memory_manager.has_guest_caller()) {
                const u64 args[1] = {static_cast<u64>(static_cast<u32>(sig))};
                (void)memory_manager.call_guest(handler, args, 1);
            }
            m.set_return(static_cast<u64>(static_cast<i32>(kContinueExecution)));
        }
    });

    // terminate_handler set_terminate(terminate_handler) -> previous
    static GuestAddr s_terminate_handler = 0;
    exports.entries.push_back(ExportEntry{
        "set_terminate", 0,
        [](Marshaler& m) {
            const GuestAddr previous = s_terminate_handler;
            s_terminate_handler = m.arg_ptr(0);
            m.set_return(previous);
        }
    });
    exports.entries.push_back(ExportEntry{
        "_get_terminate", 0,
        [](Marshaler& m) { m.set_return(s_terminate_handler); }
    });

    // void terminate(void) — the C++ terminate entry point
    exports.entries.push_back(ExportEntry{
        "terminate", 0,
        [&pm](Marshaler& m) {
            (void)m;
            std::fprintf(stderr, "[crt] terminate() called — terminating guest "
                                 "(handler=0x%llx, not invoked)\n",
                         static_cast<unsigned long long>(s_terminate_handler));
            pm.exit_process(3);
        }
    });

    // void __std_terminate(void) — vcruntime's noreturn terminate entry.
    // MSVC emits an `int 3` right after the call, so returning here leaves the
    // guest spinning on a breakpoint forever.
    exports.entries.push_back(ExportEntry{
        "__std_terminate", 0,
        [&pm](Marshaler& m) {
            (void)m;
            std::fprintf(stderr, "[crt] __std_terminate() called — terminating "
                                 "guest (handler=0x%llx, not invoked)\n",
                         static_cast<unsigned long long>(s_terminate_handler));
            pm.exit_process(3);
        }
    });

    // void _invalid_parameter_noinfo(void) / _noreturn / _invalid_parameter
    //
    // Learn: call the thread-local then process-wide invalid-parameter handler
    // (empty args). The `_noinfo` form may RETURN when a user handler is set;
    // `_noreturn` always follows with `_invoke_watson` (fast-fail 0xC0000409).
    exports.entries.push_back(ExportEntry{
        "_invalid_parameter_noinfo", 0,
        [&memory_manager](Marshaler& /*m*/) {
            (void)memory_manager.invalid_parameter();
        }});
    exports.entries.push_back(ExportEntry{
        "_invalid_parameter_noinfo_noreturn", 0,
        [&memory_manager](Marshaler& /*m*/) {
            if (memory_manager.invalid_parameter()) {
                // Handler returned — still watson (oracle ipn_noreturn_handler).
                memory_manager.invoke_watson();
            }
        }});
    exports.entries.push_back(ExportEntry{
        "_invalid_parameter", 0,
        [&memory_manager](Marshaler& /*m*/) {
            // msvcrt.dll's export returns after the handler, or at once
            // without one (crt_msvcrt_stdio_probe); the UCRT's fast-fails.
            if (memory_manager.invalid_parameter() &&
                CrtDllFlavorScope::current() != CrtDllFlavor::Msvcrt) {
                memory_manager.invoke_watson();
            }
        }});

    // ---- CRT startup helpers ------------------------------------------------

    // _configure_narrow_argv / _configure_wide_argv are registered with the
    // other argv accessors in crt_init_registration.cpp.

    // int _initialize_narrow_environment(void) — the environment block is set
    // up by the environment manager before any guest code runs.
    exports.entries.push_back(ExportEntry{
        "_initialize_narrow_environment", 0,
        [](Marshaler& m) { m.set_return(0); }
    });
    exports.entries.push_back(ExportEntry{
        "_initialize_wide_environment", 0,
        [](Marshaler& m) { m.set_return(0); }
    });

    // int _seh_filter_dll(unsigned code, EXCEPTION_POINTERS*) — the DLL variant
    // never handles the exception itself; it lets the search continue.
    exports.entries.push_back(ExportEntry{
        "_seh_filter_dll", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            (void)m.arg(1);
            m.set_return(0);  // EXCEPTION_CONTINUE_SEARCH
        }
    });

    // uintptr_t _beginthread(void (__cdecl* start)(void*), unsigned stack,
    //                        void* arglist) -> handle, or (uintptr_t)-1L
    // Learn: on error returns -1L (all bits on x64) with errno set; a NULL
    // start runs the invalid-parameter handler, errno EINVAL
    // (crt_ucrt_rt_probe: bt_null_start h=0xffffffffffffffff errno=22 ip=1).
    exports.entries.push_back(ExportEntry{
        "_beginthread", 0,
        [&tm, &memory_manager](Marshaler& m) {
            constexpr u32 EINVAL_VALUE = 22;
            constexpr u32 EAGAIN_VALUE = 11;
            const u64 failure = m.cpu_is_64bit() ? ~u64{0} : u64{0xFFFFFFFFu};
            const GuestAddr start = m.arg_ptr(0);
            const auto stack_size = static_cast<u32>(m.arg(1));
            const GuestAddr param = m.arg_ptr(2);
            if (start == 0) {
                memory_manager.set_errno(EINVAL_VALUE);
                (void)memory_manager.invalid_parameter();
                m.set_return(failure);
                return;
            }
            u32 tid_val = 0;
            const Handle h = tm.create_thread(stack_size, start, param, 0,
                                              &tid_val);
            if (h == INVALID_HANDLE || h == 0) {
                memory_manager.set_errno(EAGAIN_VALUE);
                m.set_return(failure);
                return;
            }
            if (auto* gt = tm.get_thread(tid_val)) gt->launched = true;
            tm.run_async(tid_val, start, param);
            m.set_return(static_cast<u64>(h));
        }
    });

    // ---- errno message tables ----------------------------------------------
    //
    // `strerror` returns a pointer into a *per-thread* TLS buffer (Learn).
    // Message text for 0..144 matches UCRT (crt_ucrt_rt_probe.expected);
    // codes outside that range print "Unknown error". `__sys_errlist` /
    // `__sys_nerr` still expose the classic 0..42 POSIX subset (see LOW).
    static const char* const ERRNO_MESSAGES[] = {
        "No error",
        "Operation not permitted",
        "No such file or directory",
        "No such process",
        "Interrupted function call",
        "Input/output error",
        "No such device or address",
        "Arg list too long",
        "Exec format error",
        "Bad file descriptor",
        "No child processes",
        "Resource temporarily unavailable",
        "Not enough space",
        "Permission denied",
        "Bad address",
        "Unknown error",
        "Resource device",
        "File exists",
        "Improper link",
        "No such device",
        "Not a directory",
        "Is a directory",
        "Invalid argument",
        "Too many open files in system",
        "Too many open files",
        "Inappropriate I/O control operation",
        "Unknown error",
        "File too large",
        "No space left on device",
        "Invalid seek",
        "Read-only file system",
        "Too many links",
        "Broken pipe",
        "Domain error",
        "Result too large",
        "Unknown error",
        "Resource deadlock avoided",
        "Unknown error",
        "Filename too long",
        "No locks available",
        "Function not implemented",
        "Directory not empty",
        "Illegal byte sequence",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "address in use",
        "address not available",
        "address family not supported",
        "connection already in progress",
        "bad message",
        "operation canceled",
        "connection aborted",
        "connection refused",
        "connection reset",
        "destination address required",
        "host unreachable",
        "identifier removed",
        "operation in progress",
        "already connected",
        "too many symbolic link levels",
        "message size",
        "network down",
        "network reset",
        "network unreachable",
        "no buffer space",
        "no message available",
        "no link",
        "no message",
        "no protocol option",
        "no stream resources",
        "not a stream",
        "not connected",
        "state not recoverable",
        "not a socket",
        "not supported",
        "operation not supported",
        "Unknown error",
        "value too large",
        "owner dead",
        "protocol error",
        "protocol not supported",
        "wrong protocol type",
        "stream timeout",
        "timed out",
        "text file busy",
        "operation would block",
        "Unknown error",
        "Unknown error",
        "Unknown error",
        "Unknown error",
    };
    constexpr i32 STRERROR_MSG_COUNT =
        static_cast<i32>(sizeof(ERRNO_MESSAGES) / sizeof(ERRNO_MESSAGES[0]));
    // Classic `_sys_nerr` / `_sys_errlist` size (indices 0..42).
    constexpr i32 SYS_NERR_VALUE = 43;

    auto errno_text = [](i32 code) -> const char* {
        if (code >= 0 && code < STRERROR_MSG_COUNT) return ERRNO_MESSAGES[code];
        return "Unknown error";
    };

    // char* strerror(int errnum) — per-thread TLS buffer (94 bytes, UCRT).
    exports.entries.push_back(ExportEntry{
        "strerror", 0,
        [&memory_manager, errno_text](Marshaler& m) {
            static std::mutex s_lock;
            static std::unordered_map<u32, GuestAddr> s_bufs;
            const auto code = static_cast<i32>(m.arg(0));
            const char* text = errno_text(code);
            const u32 tid = ThreadManager::get_current_thread_id_static();
            GuestAddr buffer = 0;
            {
                std::lock_guard<std::mutex> guard(s_lock);
                auto& slot = s_bufs[tid];
                if (slot == 0) slot = memory_manager.crt_malloc(94);
                buffer = slot;
            }
            if (buffer == 0 ||
                m.write_ansi_string(buffer, text).is_err()) {
                m.set_return(0);
                return;
            }
            m.set_return(buffer);
        }
    });

    // errno_t strerror_s(char* buffer, size_t size, int errnum)
    // Learn: truncation is success (return 0); NULL/size0 → EINVAL + errno +
    // invalid-parameter handler.
    exports.entries.push_back(ExportEntry{
        "strerror_s", 0,
        [&memory_manager, errno_text](Marshaler& m) {
            constexpr u32 EINVAL_VALUE = 22;
            const GuestAddr buffer = m.arg_ptr(0);
            const auto size = static_cast<usize>(m.arg(1));
            const auto code = static_cast<i32>(m.arg(2));
            if (buffer == 0 || size == 0) {
                memory_manager.set_errno(EINVAL_VALUE);
                (void)memory_manager.invalid_parameter();
                m.set_return(EINVAL_VALUE);
                return;
            }
            const char* text = errno_text(code);
            const usize length = std::strlen(text);
            if (length + 1 > size) {
                for (usize i = 0; i + 1 < size; ++i) {
                    (void)m.write_u8(buffer + static_cast<GuestAddr>(i),
                                     static_cast<u8>(text[i]));
                }
                (void)m.write_u8(buffer + static_cast<GuestAddr>(size - 1), 0);
                m.set_return(0u); // truncation is not an error
                return;
            }
            m.set_return(m.write_ansi_string(buffer, text).is_ok()
                ? 0u : EINVAL_VALUE);
        }
    });

    // int* __sys_nerr(void) — pointer to the table's entry count
    exports.entries.push_back(ExportEntry{
        "__sys_nerr", 0,
        [&memory_manager](Marshaler& m) {
            static GuestAddr s_count = 0;
            if (s_count == 0) {
                s_count = memory_manager.crt_malloc(4);
                if (s_count != 0) {
                    (void)m.write_u32(s_count,
                                      static_cast<u32>(SYS_NERR_VALUE));
                }
            }
            m.set_return(s_count);
        }
    });

    // char*** __sys_errlist(void) — pointer to the array of message pointers
    exports.entries.push_back(ExportEntry{
        "__sys_errlist", 0,
        [&memory_manager](Marshaler& m) {
            static GuestAddr s_table = 0;
            if (s_table != 0) {
                m.set_return(s_table);
                return;
            }
            const usize ptr_size = m.cpu_is_64bit() ? 8 : 4;
            // _sys_errlist[_sys_nerr] is the documented "Unknown error"
            // slot (crt_ucrt_rt_probe: sys_errlist42/43), so the table has
            // _sys_nerr + 1 entries.
            constexpr i32 kEntries = SYS_NERR_VALUE + 1;
            const GuestAddr table = memory_manager.crt_malloc(
                static_cast<usize>(kEntries) * ptr_size);
            if (table == 0) {
                m.set_return(0);
                return;
            }
            for (i32 i = 0; i < kEntries; ++i) {
                const char* text = ERRNO_MESSAGES[i];
                const GuestAddr string =
                    memory_manager.crt_malloc(std::strlen(text) + 1);
                if (string != 0) {
                    (void)m.write_ansi_string(string, text);
                }
                (void)m.write_ptr(
                    table + static_cast<GuestAddr>(static_cast<usize>(i) * ptr_size),
                    string);
            }
            s_table = table;
            m.set_return(s_table);
        }
    });

    exports.entries.push_back(ExportEntry{
        "set_new_handler", 0,
        [](Marshaler& m) {
            static GuestAddr s_cpp_new_handler = 0;
            const GuestAddr previous = s_cpp_new_handler;
            s_cpp_new_handler = m.arg_ptr(0);
            m.set_return(previous);
        }
    });

    // __C_specific_handler: the x64 SEH scope-table handler, shared with
    // ntdll (winapi/common/c_specific_handler.cpp). The 32-bit CRT tables do
    // not list it, so apply_*_windows_ordinals drops it for x86 guests.
    exports.entries.push_back(make_c_specific_handler_export());

    // _XcptFilter — exception filter (EXCEPTION_EXECUTE_HANDLER)
    exports.entries.push_back(ExportEntry{
        "_XcptFilter", 0,
        [](Marshaler& m) { (void)m.arg(0); m.set_return(1); }
    });

    // _commode — global variable (return 0)
    exports.entries.push_back(ExportEntry{
        "_commode", 0,
        [](Marshaler& m) { m.set_return(0); }
    });

    // _fmode — global variable (return 0)
    exports.entries.push_back(ExportEntry{
        "_fmode", 0,
        [](Marshaler& m) { m.set_return(0); }
    });

    // __p__commode — return pointer to _commode variable
    exports.entries.push_back(ExportEntry{
        "__p__commode", 0,
        [&crt_data](Marshaler& m) { m.set_return(crt_data.commode_addr()); }
    });

    // __p__fmode — return pointer to _fmode variable
    exports.entries.push_back(ExportEntry{
        "__p__fmode", 0,
        [&crt_data](Marshaler& m) { m.set_return(crt_data.fmode_addr()); }
    });

    // _except_handler4_common — x86 SEH handler (return ExceptionContinueSearch)
    exports.entries.push_back(ExportEntry{
        "_except_handler4_common", 0,
        [](Marshaler& m) { (void)m; m.set_return(1); }
    });

    // The floating-point environment (_controlfp, _control87, _controlfp_s,
    // __control87_2, _statusfp, _statusfp2, _clearfp, _fpreset,
    // __fpe_flt_rounds) lives in crt_fpenv.cpp.
    add_crt_fpenv_exports(exports);

    // VC2005+ CRT pointer obfuscation. When kernel32 EncodePointer is unavailable
    // the CRT returns the pointer unchanged; Vitreo mirrors that identity so
    // atexit / SEH function pointers survive encode→decode round-trips.
    exports.entries.push_back(ExportEntry{
        "_encode_pointer", 0,
        [](Marshaler& m) { m.set_return(m.arg(0)); }
    });
    exports.entries.push_back(ExportEntry{
        "_decode_pointer", 0,
        [](Marshaler& m) { m.set_return(m.arg(0)); }
    });
    exports.entries.push_back(ExportEntry{
        "_encoded_null", 0,
        [](Marshaler& m) { m.set_return(0); }
    });

    // void _invoke_watson(...); MSVC invalid-parameter / assert path.
    exports.entries.push_back(ExportEntry{
        "_invoke_watson", 0,
        [&memory_manager](Marshaler& /*m*/) {
            // Learn: __fastfail(FAST_FAIL_INVALID_ARG) where available; the
            // process exit status is 0xC0000409 (crt_ucrt_rt_probe:
            // "watson: -> exit=0xc0000409").
            memory_manager.invoke_watson();
        }
    });

    // abort — Learn: if a SIGABRT handler is installed, raise it first; then
    // with _CALL_REPORTFAULT (default) terminate via fast-fail 0xC0000409,
    // else _exit(3). Measured: crt_ucrt_rt_probe abort / abort_handler /
    // abort_noreport.
    exports.entries.push_back(ExportEntry{
        "abort", 0,
        [&pm, &memory_manager](Marshaler& /*m*/) {
            constexpr i32 kSigAbrt = 22;
            GuestAddr handler = 0;
            {
                std::lock_guard<std::mutex> guard(g_signal_lock);
                auto it = g_signal_handlers.find(kSigAbrt);
                if (it != g_signal_handlers.end()) handler = it->second;
                if (handler > 4) g_signal_handlers[kSigAbrt] = kSigDfl;
            }
            if (handler > 4 && memory_manager.has_guest_caller()) {
                const u64 args[1] = {static_cast<u64>(kSigAbrt)};
                (void)memory_manager.call_guest(handler, args, 1);
            }
            if ((g_abort_behavior & kCallReportFault) != 0) {
                pm.exit_process(kStatusStackBufferOverrun);
            } else {
                pm.exit_process(3);
            }
        }
    });

    // rand / srand
    exports.entries.push_back(ExportEntry{
        "srand", 0,
        [](Marshaler& m) {
            crt_rand_seed() = static_cast<u32>(m.arg(0));
        }
    });
    exports.entries.push_back(ExportEntry{
        "rand", 0,
        [](Marshaler& m) {
            // MSVC's rand() is a specified LCG, not an implementation detail:
            //   seed = seed * 214013 + 2531011;  return (seed >> 16) & 0x7fff;
            // with a per-thread seed that starts at 1. Delegating to the host
            // std::rand() produced a DIFFERENT SEQUENCE for the same srand()
            // value -- 24 of 24 probed values differed from Windows -- so any
            // guest that seeds deterministically and expects Microsoft's
            // sequence (procedural generation, shuffles, replays) diverged.
            u32& seed = crt_rand_seed();
            seed = seed * 214013u + 2531011u;
            m.set_return(static_cast<u64>((seed >> 16) & 0x7FFFu));
        }
    });
    // errno_t rand_s(unsigned int* randomValue) — cryptographically stronger
    // than rand(); MSVC backs this with RtlGenRandom. Null out → EINVAL.
    exports.entries.push_back(ExportEntry{
        "rand_s", 0,
        [](Marshaler& m) {
            GuestAddr out = m.arg_ptr(0);
            if (out == 0) {
                m.set_return(22u);  // EINVAL
                return;
            }
            u32 value = 0;
            arc4random_buf(&value, sizeof(value));
            if (m.write_u32(out, value).is_err()) {
                m.set_return(22u);
                return;
            }
            m.set_return(0u);
        }});

    // time(time_t* t)
    exports.entries.push_back(ExportEntry{
        "time", 0,
        [](Marshaler& m) {
            GuestAddr out_ptr = m.arg_ptr(0);
            std::time_t now = std::time(nullptr);
            if (out_ptr != 0) {
                (void)m.write_u32(out_ptr, static_cast<u32>(now));
            }
            m.set_return(static_cast<u64>(static_cast<u32>(now)));
        }
    });

    // uintptr_t _beginthreadex(void* security, unsigned stack_size,
    //     unsigned (__stdcall* start)(void*), void* arglist,
    //     unsigned initflag, unsigned* thrdaddr)
    //
    // Learn (beginthread-beginthreadex): returns a handle usable with the
    // synchronization APIs (so SuspendThread/ResumeThread must treat it like
    // a CreateThread handle), or 0 on error with errno set; a NULL start
    // address runs the invalid-parameter handler and sets errno EINVAL.
    // crt_ucrt_rt_probe.expected: bte_suspended / bte_running_suspend /
    // bte_null_start h=0x0 errno=22 ip=1.
    exports.entries.push_back(ExportEntry{
        "_beginthreadex", 0,
        [&tm, &memory_manager](Marshaler& m) {
            constexpr u32 EINVAL_VALUE = 22;
            constexpr u32 EAGAIN_VALUE = 11;
            (void)m.arg(0); // security
            const u32 stack_size = static_cast<u32>(m.arg(1));
            const GuestAddr start = m.arg_ptr(2);
            const GuestAddr param = m.arg_ptr(3);
            const u32 flags = static_cast<u32>(m.arg(4));
            const GuestAddr tid_ptr = m.arg_ptr(5);
            if (start == 0) {
                memory_manager.set_errno(EINVAL_VALUE);
                (void)memory_manager.invalid_parameter();
                m.set_return(0);
                return;
            }

            u32 tid_val = 0;
            const Handle h = tm.create_thread(stack_size, start, param, flags, &tid_val);
            if (h == INVALID_HANDLE || h == 0) {
                memory_manager.set_errno(EAGAIN_VALUE);
                m.set_return(0);
                return;
            }
            if (tid_ptr != 0) {
                (void)m.write_u32(tid_ptr, tid_val);
            }
            if (!(flags & CREATE_SUSPENDED)) {
                // Same as CreateThread: mark launched so ResumeThread never
                // starts the routine a second time and SuspendThread parks it.
                if (auto* gt = tm.get_thread(tid_val)) gt->launched = true;
                tm.run_async(tid_val, start, param);
            }
            m.set_return(static_cast<u64>(h));
        }
    });

    // void _endthreadex(unsigned retcode) — __declspec(noreturn). Learn:
    // reclaims the thread's CRT resources and calls ExitThread; control never
    // returns to the caller (crt_ucrt_rt_probe: endthreadex exit=7
    // after_end_ran=0).
    exports.entries.push_back(ExportEntry{
        "_endthreadex", 0,
        [&tm](Marshaler& m) {
            tm.exit_current_thread_noreturn(m, static_cast<u32>(m.arg(0)));
        }
    });
    // void _endthread(void) — same, exit code 0.
    exports.entries.push_back(ExportEntry{
        "_endthread", 0,
        [&tm](Marshaler& m) { tm.exit_current_thread_noreturn(m, 0); }
    });

    // int atexit(void (*func)(void))
    //
    // Same registration list as _onexit; the two differ only in their return
    // convention — atexit reports 0 for success where _onexit hands back the
    // function pointer.
    exports.entries.push_back(ExportEntry{
        "atexit", 0,
        [&memory_manager](Marshaler& m) {
            const GuestAddr func = m.arg_ptr(0);
            const GuestAddr registered = memory_manager.register_onexit(func);
            m.set_return(registered != 0 ? 0u : 0xFFFFFFFFu);
        }
    });

    // void _assert(const char* message, const char* file, unsigned line)
    //
    // Windows reports the failed assertion and then aborts the process. The
    // report is the whole point of the API, so it must reach the user before
    // the process dies — a silent return would let a guest continue past a
    // condition its own code declared impossible.
    exports.entries.push_back(ExportEntry{
        "_assert", 0,
        [&pm](Marshaler& m) {
            auto message = m.read_ansi_string(m.arg_ptr(0));
            auto file = m.read_ansi_string(m.arg_ptr(1));
            const auto line = static_cast<u32>(m.arg(2));
            std::fprintf(stderr,
                         "[crt] Assertion failed: %s, file %s, line %u\n",
                         message.is_ok() ? message.value().c_str() : "<unreadable>",
                         file.is_ok() ? file.value().c_str() : "<unreadable>",
                         line);
            std::fflush(stderr);
            pm.exit_process(3);  // abort()'s exit code
        }
    });

    // void _wassert(const wchar_t* expr, const wchar_t* file, unsigned line)
    //
    // Wide sibling of _assert; same abort-after-report contract.
    exports.entries.push_back(ExportEntry{
        "_wassert", 0,
        [&pm](Marshaler& m) {
            auto message = m.read_wide_string(m.arg_ptr(0));
            auto file = m.read_wide_string(m.arg_ptr(1));
            const auto line = static_cast<u32>(m.arg(2));
            std::fprintf(stderr,
                         "[crt] Assertion failed: %s, file %s, line %u\n",
                         message.is_ok() ? message.value().c_str() : "<unreadable>",
                         file.is_ok() ? file.value().c_str() : "<unreadable>",
                         line);
            std::fflush(stderr);
            pm.exit_process(3);
        }
    });

    // ---- setjmp / longjmp ---------------------------------------------------
    //
    // MSVC's 32-bit _JUMP_BUFFER, in the order the CRT writes it:
    //   +0 Ebp, +4 Ebx, +8 Edi, +12 Esi, +16 Esp, +20 Eip,
    //   +24 Registration (SEH chain head), +28 TryLevel, ...
    // The x64 buffer is _SETJMP_FLOAT128-based and much larger; only the
    // integer slots Vitreo restores are written here.
    //
    // These cannot be expressed as ordinary thunks that return normally:
    // longjmp resumes at the setjmp call site with a different stack. It sets
    // cpu.thunk_control_transfer so the dispatcher skips its simulated RET.
    const auto do_setjmp = [](Marshaler& m) {
        const GuestAddr buf = m.arg_ptr(0);
        if (buf == 0 || !m.has_cpu()) {
            m.set_return(0);
            return;
        }
        auto* cpu = m.cpu();

        if (m.cpu_is_64bit()) {
            // The return address sits at [rsp]; the guest's rsp after the
            // call returns is rsp+8.
            auto ret = m.read_u64(cpu->gpr.rsp);
            if (ret.is_err()) {
                m.set_return(0);
                return;
            }
            (void)m.write_u64(buf + JmpBuf64::FRAME, cpu->gpr.rbp);
            (void)m.write_u64(buf + JmpBuf64::RBX, cpu->gpr.rbx);
            (void)m.write_u64(buf + JmpBuf64::RSP, cpu->gpr.rsp + 8);
            (void)m.write_u64(buf + JmpBuf64::RBP, cpu->gpr.rbp);
            (void)m.write_u64(buf + JmpBuf64::RSI, cpu->gpr.rsi);
            (void)m.write_u64(buf + JmpBuf64::RDI, cpu->gpr.rdi);
            (void)m.write_u64(buf + JmpBuf64::R12, cpu->gpr.r12);
            (void)m.write_u64(buf + JmpBuf64::R13, cpu->gpr.r13);
            (void)m.write_u64(buf + JmpBuf64::R14, cpu->gpr.r14);
            (void)m.write_u64(buf + JmpBuf64::R15, cpu->gpr.r15);
            (void)m.write_u64(buf + JmpBuf64::RIP, ret.value());
            m.set_return(0);
            return;
        }

        auto ret = m.read_u32(static_cast<GuestAddr>(cpu->gpr.rsp));
        if (ret.is_err()) {
            m.set_return(0);
            return;
        }
        // cdecl: the caller pops the argument, so the resumed esp is the
        // value just past the return address.
        (void)m.write_u32(buf + JmpBuf32::EBP, static_cast<u32>(cpu->gpr.rbp));
        (void)m.write_u32(buf + JmpBuf32::EBX, static_cast<u32>(cpu->gpr.rbx));
        (void)m.write_u32(buf + JmpBuf32::EDI, static_cast<u32>(cpu->gpr.rdi));
        (void)m.write_u32(buf + JmpBuf32::ESI, static_cast<u32>(cpu->gpr.rsi));
        (void)m.write_u32(buf + JmpBuf32::ESP,
                          static_cast<u32>(cpu->gpr.rsp + 4));
        (void)m.write_u32(buf + JmpBuf32::EIP, ret.value());

        // Capture the SEH chain head so longjmp can restore it; without this
        // a jump out of a __try leaves fs:[0] pointing at a dead frame.
        u32 registration = 0xFFFFFFFFu;
        auto head = m.read_u32(
            static_cast<GuestAddr>(cpu->seg.fs_desc.base + 0));
        if (head.is_ok()) registration = head.value();
        (void)m.write_u32(buf + JmpBuf32::REGISTRATION, registration);
        (void)m.write_u32(buf + JmpBuf32::TRY_LEVEL, 0xFFFFFFFFu);
        m.set_return(0);
    };

    const auto do_longjmp = [](Marshaler& m) {
        const GuestAddr buf = m.arg_ptr(0);
        auto value = static_cast<u32>(m.arg(1));
        // longjmp(env, 0) must still make setjmp return non-zero.
        if (value == 0) value = 1;
        if (buf == 0 || !m.has_cpu()) {
            m.set_return(value);
            return;
        }
        auto* cpu = m.cpu();

        if (m.cpu_is_64bit()) {
            auto rip = m.read_u64(buf + JmpBuf64::RIP);
            auto rsp = m.read_u64(buf + JmpBuf64::RSP);
            if (rip.is_err() || rsp.is_err() || rip.value() == 0) {
                std::fprintf(stderr,
                             "[crt] longjmp: unreadable jmp_buf at 0x%llx\n",
                             static_cast<unsigned long long>(buf));
                m.set_return(value);
                return;
            }
            const auto load = [&](u64 offset, u64& reg) {
                auto v = m.read_u64(buf + offset);
                if (v.is_ok()) reg = v.value();
            };
            load(JmpBuf64::RBX, cpu->gpr.rbx);
            load(JmpBuf64::RBP, cpu->gpr.rbp);
            load(JmpBuf64::RSI, cpu->gpr.rsi);
            load(JmpBuf64::RDI, cpu->gpr.rdi);
            load(JmpBuf64::R12, cpu->gpr.r12);
            load(JmpBuf64::R13, cpu->gpr.r13);
            load(JmpBuf64::R14, cpu->gpr.r14);
            load(JmpBuf64::R15, cpu->gpr.r15);
            cpu->gpr.rsp = rsp.value();
            cpu->rip = rip.value();
            cpu->gpr.rax = value;
            cpu->thunk_control_transfer = true;
            return;
        }

        auto eip = m.read_u32(buf + JmpBuf32::EIP);
        auto esp = m.read_u32(buf + JmpBuf32::ESP);
        if (eip.is_err() || esp.is_err() || eip.value() == 0) {
            std::fprintf(stderr,
                         "[crt] longjmp: unreadable jmp_buf at 0x%llx\n",
                         static_cast<unsigned long long>(buf));
            m.set_return(value);
            return;
        }
        const auto load = [&](u64 offset, u64& reg) {
            auto v = m.read_u32(buf + offset);
            if (v.is_ok()) reg = v.value();
        };
        load(JmpBuf32::EBP, cpu->gpr.rbp);
        load(JmpBuf32::EBX, cpu->gpr.rbx);
        load(JmpBuf32::EDI, cpu->gpr.rdi);
        load(JmpBuf32::ESI, cpu->gpr.rsi);

        // Unwind the SEH chain back to what was current at setjmp time.
        auto registration = m.read_u32(buf + JmpBuf32::REGISTRATION);
        if (registration.is_ok() && registration.value() != 0) {
            (void)m.write_u32(
                static_cast<GuestAddr>(cpu->seg.fs_desc.base + 0),
                registration.value());
        }

        cpu->gpr.rsp = esp.value();
        cpu->rip = eip.value();
        cpu->gpr.rax = value;
        cpu->thunk_control_transfer = true;
    };

    // Every spelling the CRT exports for the same operation. <setjmp.h> maps
    // `setjmp` to `_setjmp` (or `_setjmpex`, which additionally records the
    // unwind context), and MSVC's compiler intrinsics reach vcruntime's
    // `__intrinsic_setjmp` / `__intrinsic_setjmpex` instead. They all save the
    // same jmp_buf, so they all take this implementation.
    //
    // Leaving one of them to the missing-export stub is silently catastrophic
    // rather than merely absent: the stub returns 0xFFFFFFFF, and a non-zero
    // return from setjmp means "control arrived here via longjmp". Every
    // caller then takes its error path on the *first* pass. libpng is built
    // this way, so an unimplemented `__intrinsic_setjmp` made every single PNG
    // decode report a corrupt header without reading a byte.
    for (const char* name : {"_setjmp", "setjmp", "_setjmp3", "_setjmpex",
                             "__intrinsic_setjmp", "__intrinsic_setjmpex"}) {
        exports.entries.push_back(ExportEntry{name, 0, do_setjmp});
    }
    for (const char* name : {"longjmp", "_longjmpex"}) {
        exports.entries.push_back(ExportEntry{name, 0, do_longjmp});
    }

    return exports;
}

} // namespace vitreo::winapi
