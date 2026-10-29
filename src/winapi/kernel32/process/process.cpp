/// @file process.cpp
/// @brief ExitProcess and Process/Thread Info implementation (WIN32-031)

#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_handle.hpp"
#include "winapi/system/ntdll/ntdll.hpp"
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace vitreo::winapi {

namespace {
std::atomic<u64> g_unhandled_exception_filter{0};
UnhandledExceptionFilterInvoker g_unhandled_exception_filter_invoker;
} // namespace

u64 unhandled_exception_filter_guest() noexcept {
    return g_unhandled_exception_filter.load(std::memory_order_relaxed);
}

u64 set_unhandled_exception_filter_guest(u64 handler) noexcept {
    return g_unhandled_exception_filter.exchange(handler, std::memory_order_relaxed);
}

void set_unhandled_exception_filter_invoker(
    UnhandledExceptionFilterInvoker invoker) noexcept {
    g_unhandled_exception_filter_invoker = std::move(invoker);
}

// =============================================================================
// ProcessManager — Process Identity
// =============================================================================

u32 ProcessManager::get_current_process() const noexcept {
    return CURRENT_PROCESS_PSEUDO;
}

u32 ProcessManager::get_current_process_id() const noexcept {
    // Each concurrently running guest process has its own Vitreo host process.
    // Using that pid keeps GetCurrentProcessId, Toolhelp, and the capsule
    // process registry consistent even when optional shared IPC is disabled.
    return static_cast<u32>(::getpid());
}

std::optional<ipc::ProcessSnapshotEntry> ProcessManager::find_process(
    u32 pid) const {
    if (!m_process_snapshot_provider) return std::nullopt;
    const auto processes = m_process_snapshot_provider();
    auto it = std::find_if(processes.begin(), processes.end(),
                           [pid](const auto& process) {
                               return process.pid == pid;
                           });
    if (it == processes.end()) return std::nullopt;
    return *it;
}

// =============================================================================
// ProcessManager — Thread Identity
// =============================================================================

u32 ProcessManager::get_current_thread() const noexcept {
    return CURRENT_THREAD_PSEUDO;
}

u32 ProcessManager::get_current_thread_id() const noexcept {
    // Learn: GetCurrentThreadId returns the calling thread's identifier.
    // ThreadManager tracks the per-host-thread guest TID; a process-wide
    // constant would make every guest thread appear as tid 1 and break
    // per-thread affinity checks.
    return ThreadManager::get_current_thread_id_static();
}

// =============================================================================
// ProcessManager — Debugger Detection
// =============================================================================

bool ProcessManager::is_debugger_present() const noexcept {
    return false;
}

// =============================================================================
// ProcessManager — Process Lifetime
// =============================================================================

void ProcessManager::exit_process(u32 exit_code) noexcept {
    VITREO_TRACE("[process] exit_process(code=%u)\n", exit_code);
    m_exit_code = exit_code;
    const bool first = !m_exited.exchange(true, std::memory_order_acq_rel);
    // Wake CriticalSection enter_slow waiters on the first ExitProcess so they
    // are not left parked for the rest of teardown (Learn EnterCriticalSection
    // process-exit escape). begin_shutdown is idempotent.
    if (first && m_exit_wake) {
        m_exit_wake();
    }
}

bool ProcessManager::has_exited() const noexcept {
    return m_exited.load(std::memory_order_acquire);
}

u32 ProcessManager::exit_code() const noexcept {
    return m_exit_code;
}

void ProcessManager::clear_exit_request() noexcept {
    m_exited = false;
    m_exit_code = 0;
}

// =============================================================================
// ProcessManager — Chain-Load
// =============================================================================

void ProcessManager::set_pending_launch(const std::string& host_path,
                                        const std::vector<std::string>& args) {
    m_pending_launch_path = host_path;
    m_pending_launch_args = args;
    VITREO_TRACE("[process] set_pending_launch: path='%s' args=%zu\n",
                 host_path.c_str(), args.size());

    // In our serialized chain-load model, queueing a pending launch is
    // logically equivalent to ExitProcess for the current guest: the runtime
    // tears down the current guest and the outer chain-load loop in
    // execute_pe re-runs with the queued executable.  Without this, a guest
    // that returns from ShellExecuteW/CreateProcessW and then sits in a
    // message pump (a Finish page that stays up after "Run app") would never
    // hand control back to the runtime, and the chain-load would never fire.
    exit_process(0);
}

bool ProcessManager::has_pending_launch() const noexcept {
    return !m_pending_launch_path.empty();
}

std::pair<std::string, std::vector<std::string>> ProcessManager::take_pending_launch() {
    auto result = std::make_pair(std::move(m_pending_launch_path),
                                 std::move(m_pending_launch_args));
    m_pending_launch_path.clear();
    m_pending_launch_args.clear();
    return result;
}

// =============================================================================
// ProcessManager — Startup Info
// =============================================================================

GuestStartupInfo& guest_startup_info() noexcept {
    static GuestStartupInfo info;
    return info;
}

std::string encode_guest_startup_info(const GuestStartupInfo& si) {
    char buf[192];
    std::snprintf(buf, sizeof(buf), "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
                  si.x, si.y, si.x_size, si.y_size, si.x_count_chars,
                  si.y_count_chars, si.fill_attribute, si.flags,
                  static_cast<unsigned>(si.show_window), si.std_input,
                  si.std_output, si.std_error);
    return buf;
}

bool decode_guest_startup_info(std::string_view text, GuestStartupInfo& si) {
    const std::string copy(text);
    unsigned v[12]{};
    if (std::sscanf(copy.c_str(), "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
                    &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                    &v[8], &v[9], &v[10], &v[11]) != 12) {
        return false;
    }
    si.x = v[0]; si.y = v[1]; si.x_size = v[2]; si.y_size = v[3];
    si.x_count_chars = v[4]; si.y_count_chars = v[5];
    si.fill_attribute = v[6]; si.flags = v[7];
    si.show_window = static_cast<u16>(v[8]);
    si.std_input = v[9]; si.std_output = v[10]; si.std_error = v[11];
    return true;
}

void write_guest_startup_info(GuestMemory& memory, GuestAddr addr, bool is_64bit) {
    // STARTUPINFOW (processthreadsapi.h), x64 / x86 offsets:
    //   cb 0/0, lpReserved 8/4, lpDesktop 16/8, lpTitle 24/12, dwX 32/16,
    //   dwY 36/20, dwXSize 40/24, dwYSize 44/28, dwXCountChars 48/32,
    //   dwYCountChars 52/36, dwFillAttribute 56/40, dwFlags 60/44,
    //   wShowWindow 64/48, cbReserved2 66/50, lpReserved2 72/52,
    //   hStdInput 80/56, hStdOutput 88/60, hStdError 96/64.
    // The whole structure is written: a short write leaves lpReserved2 and
    // the std handle fields holding the caller's stack garbage on x64.
    const usize size = is_64bit ? STARTUPINFOW_SIZE_64 : STARTUPINFOW_SIZE_32;
    Byte bytes[STARTUPINFOW_SIZE_64];
    std::memset(bytes, 0, sizeof(bytes));
    const auto put32 = [&](usize off, u32 v) { std::memcpy(bytes + off, &v, 4); };
    const GuestStartupInfo& si = guest_startup_info();
    const usize base = is_64bit ? 32 : 16;
    put32(0, static_cast<u32>(size));
    put32(base + 0, si.x);
    put32(base + 4, si.y);
    put32(base + 8, si.x_size);
    put32(base + 12, si.y_size);
    put32(base + 16, si.x_count_chars);
    put32(base + 20, si.y_count_chars);
    put32(base + 24, si.fill_attribute);
    put32(base + 28, si.flags);
    std::memcpy(bytes + base + 32, &si.show_window, 2);
    // Handles are only meaningful with STARTF_USESTDHANDLES; otherwise the
    // fields stay zero (the documented "call GetStdHandle" state).
    if ((si.flags & STARTF_USESTDHANDLES) != 0) {
        const u32 handles[3] = {si.std_input, si.std_output, si.std_error};
        for (usize i = 0; i < 3; ++i) {
            if (is_64bit) {
                // A stored (HANDLE)-1 is all ones on x64.
                const u64 v = (handles[i] & 0x80000000u) != 0
                    ? static_cast<u64>(static_cast<i64>(static_cast<i32>(handles[i])))
                    : handles[i];
                std::memcpy(bytes + 80 + i * 8, &v, 8);
            } else {
                put32(56 + i * 4, handles[i]);
            }
        }
    }
    (void)memory.write_bytes(addr, bytes, size);
}

void ProcessManager::write_startup_info(Marshaler& m, GuestAddr addr) {
    write_guest_startup_info(m.memory(), addr, m.cpu_is_64bit());
}

// =============================================================================
// Win32 API Thunk Implementations
// =============================================================================

/// True when the bytes immediately before @p address decode as a call, which
/// is what distinguishes a real return address on the stack from an integer,
/// a data pointer, or a stale slot that happens to fall inside an image.
///
/// A large executable's image covers a wide address range, so a module-range
/// test alone reports mostly noise. Checking for the preceding call keeps the
/// x86 encodings that matter here: E8 rel32, and the FF /2 register, memory,
/// and indirect-through-import forms.
[[nodiscard]] static bool preceded_by_call(Marshaler& m, GuestAddr address) {
    const auto byte_at = [&m](GuestAddr at) -> int {
        auto read = m.read_u8(at);
        return read.is_ok() ? static_cast<int>(read.value()) : -1;
    };

    if (address >= 5 && byte_at(address - 5) == 0xE8) return true;

    // FF /2 (call r/m) is 2 to 7 bytes long depending on ModR/M, SIB, and
    // displacement. Accept any length whose ModR/M reg field selects /2.
    for (GuestAddr back = 2; back <= 7; ++back) {
        if (address < back) break;
        if (byte_at(address - back) != 0xFF) continue;
        const int modrm = byte_at(address - back + 1);
        if (modrm < 0) continue;
        if (((modrm >> 3) & 0x7) == 2) return true;
    }
    return false;
}

void log_self_termination_chain(Marshaler& m, const ModuleManager* modules,
                                const char* api) {
    if (!m.has_cpu() || modules == nullptr) return;
    // VITREO_DUMP_ON_EXIT also enables this: it means "explain why the guest
    // exited", and on the native32 path the hostcall ring records nothing (it
    // is only wired into the JIT/interpreter run loop), so this chain is the
    // only account of a clean self-termination available there.
    const bool verbose = std::getenv("VITREO_TRACE") != nullptr ||
                         std::getenv("VITREO_DUMP_ON_EXIT") != nullptr;

    // Enough slots to cross the several small frames a fatal path typically
    // adds between the decision and the terminating call.
    constexpr u32 STACK_SLOTS = 512;
    constexpr usize MAX_REPORTED = 16;

    const bool is_64bit = m.cpu_is_64bit();
    const usize slot_size = is_64bit ? sizeof(u64) : sizeof(u32);
    const GuestAddr sp = static_cast<GuestAddr>(m.cpu()->sp());

    std::string chain;
    usize reported = 0;
    u64 previous_value = 0;

    for (u32 slot = 0; slot < STACK_SLOTS && reported < MAX_REPORTED; ++slot) {
        const GuestAddr address = sp + slot * slot_size;
        u64 value = 0;
        if (is_64bit) {
            auto read = m.read_u64(address);
            if (read.is_err()) break;
            value = read.value();
        } else {
            auto read = m.read_u32(address);
            if (read.is_err()) break;
            value = read.value();
        }

        if (value == previous_value) continue;

        const auto located =
            modules->module_for_address(static_cast<GuestAddr>(value));
        if (located.info == nullptr) continue;
        if (!preceded_by_call(m, static_cast<GuestAddr>(value))) continue;
        previous_value = value;

        char entry[128];
        std::snprintf(entry, sizeof(entry), "%s%s+0x%x",
                      chain.empty() ? "" : " <- ",
                      located.info->name.c_str(), located.rva);
        chain += entry;
        ++reported;
    }

    if (chain.empty()) {
        ipc::ipc_log("%s self-termination chain: unresolved", api);
        if (verbose) {
            VITREO_TRACE("[process] %s self-termination chain: "
                         "no stack slot resolved to a loaded module\n", api);
        }
        return;
    }
    ipc::ipc_log("%s self-termination chain: %s", api, chain.c_str());
    if (verbose) {
        VITREO_TRACE("[process] %s self-termination chain (stack scan, "
                     "innermost first): %s\n", api, chain.c_str());
    }
}

void impl_ExitProcess(Marshaler& m, ProcessManager& pm) {
    // VOID WINAPI ExitProcess(UINT uExitCode);
    // uExitCode is the first fastcall argument (RCX).
    auto code = static_cast<u32>(m.arg(0));
    // Log the caller's return address (top of stack on thunk entry) so the
    // module+offset that decided to terminate can be identified. Read it at the
    // guest's pointer width — a u64 read on a 32-bit guest splices the first
    // stack argument into the high half and prints a bogus address.
    u64 caller = 0;
    if (m.has_cpu()) {
        GuestAddr sp = static_cast<GuestAddr>(m.cpu()->sp());
        if (m.cpu_is_64bit()) {
            auto r = m.read_u64(sp);
            if (r.is_ok()) caller = r.value();
        } else {
            auto r = m.read_u32(sp);
            if (r.is_ok()) caller = r.value();
        }
    }
    VITREO_TRACE( "[process] ExitProcess(code=%u, caller_ret=0x%llx)\n", code,
                 static_cast<unsigned long long>(caller));
    std::fprintf(stderr,
                 "[process] ExitProcess pid=%d code=0x%x caller_ret=0x%llx\n",
                 static_cast<int>(::getpid()), code,
                 static_cast<unsigned long long>(caller));
    std::fflush(stderr);
    log_self_termination_chain(m, pm.module_manager(), "ExitProcess");
    vitreo::diagnostics::dump_hostcalls("ExitProcess");
    ipc::ipc_log("ExitProcess(code=0x%x) caller_ret=0x%llx",
                 code, static_cast<unsigned long long>(caller));
    // Learn ExitProcess step 3: DLL_PROCESS_DETACH for every loaded DLL
    // before the process actually terminates.
    if (ModuleManager* modules = pm.module_manager()) {
        modules->notify_process_detach();
    }
    pm.exit_process(code);
}

void impl_GetCurrentProcess(Marshaler& m, ProcessManager& pm) {
    // HANDLE WINAPI GetCurrentProcess(void);
    // Learn: the pseudo handle is (HANDLE)-1 -- all 64 bits on x64
    // (handles_waits_probe: ffffffffffffffff). Vitreo's handle consumers
    // compare the low 32 bits, so both spellings keep meaning this process.
    const u32 pseudo = pm.get_current_process();
    m.set_return(m.cpu_is_64bit()
                     ? static_cast<u64>(static_cast<i64>(static_cast<i32>(pseudo)))
                     : static_cast<u64>(pseudo));
}

void impl_GetCurrentProcessId(Marshaler& m, ProcessManager& pm) {
    // DWORD WINAPI GetCurrentProcessId(void);
    m.set_return(pm.get_current_process_id());
}

void impl_GetCurrentThread(Marshaler& m, ProcessManager& pm) {
    // HANDLE WINAPI GetCurrentThread(void);
    // On x64 the pseudo-handle is the full (HANDLE)-2
    // (thread_lifecycle_probe KERNELBASE_GETCURRENTTHREAD). Returning the
    // truncated u32 0xFFFFFFFE makes kernelbase disagree with kernel32.
    (void)pm;
    m.set_return(0xFFFFFFFFFFFFFFFEULL);
}

void impl_GetCurrentThreadId(Marshaler& m, ProcessManager& pm) {
    // DWORD WINAPI GetCurrentThreadId(void);
    // https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getcurrentthreadid
    m.set_return(pm.get_current_thread_id());
}

void impl_IsDebuggerPresent(Marshaler& m, ProcessManager& pm) {
    // BOOL WINAPI IsDebuggerPresent(void);
    m.set_return(pm.is_debugger_present() ? 1u : 0u);
}

void impl_GetStartupInfoW(Marshaler& m, ProcessManager& pm) {
    // VOID WINAPI GetStartupInfoW(LPSTARTUPINFOW lpStartupInfo);
    // lpStartupInfo is the first fastcall argument (RCX).
    auto addr = m.arg_ptr(0);
    if (addr != 0) {
        pm.write_startup_info(m, addr);
    }
}

// =============================================================================
// Export Registration
// =============================================================================

DllExports make_process_exports(ProcessManager& pm, GlobalLocalManager* gl) {
    (void)gl;  // FormatMessage moved to winapi/kernel32/text/format_message.cpp
    DllExports exports;

    // ExitProcess (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "ExitProcess", 1,
        [&pm](Marshaler& m) { impl_ExitProcess(m, pm); },
        4
    });

    // VOID RaiseFailFastException(PEXCEPTION_RECORD pExceptionRecord,
    //                              PCONTEXT pContextRecord, DWORD dwFlags)
    //
    // Learn: https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-raisefailfastexception
    // Raises STATUS_FAIL_FAST_EXCEPTION and "bypasses all exception handlers":
    // the process dies here. The UCRT fail-fast path calls it, and the
    // compiler emits an `int 3` after the call — so a handler that returns
    // leaves the guest spinning on a breakpoint.
    exports.entries.push_back(ExportEntry{
        "RaiseFailFastException", 0,
        [&pm](Marshaler& m) {
            const GuestAddr record = m.arg_ptr(0);
            const auto flags = static_cast<u32>(m.arg(2));
            u32 code = 0xC0000409u;   // STATUS_FAIL_FAST_EXCEPTION
            u32 address = 0;
            if (record != 0) {
                // EXCEPTION_RECORD: code, flags, next, address, ...
                code = m.read_u32(record).value_or(code);
                const auto ptr_size = m.cpu_is_64bit() ? 8u : 4u;
                address = static_cast<u32>(
                    m.read_ptr(record + 8 + ptr_size).value_or(0));
            }
            std::fprintf(stderr,
                "[process] RaiseFailFastException code=0x%08x address=0x%08x "
                "flags=0x%x — terminating guest\n", code, address, flags);
            std::fflush(stderr);
            log_self_termination_chain(m, pm.module_manager(),
                                       "RaiseFailFastException");
            pm.exit_process(code);
        },
        12
    });

    // FatalAppExitA/W — display message (logged) and terminate. MS Learn:
    // does not return. uAction is reserved (must be 0).
    exports.entries.push_back(ExportEntry{
        "FatalAppExitA", 0,
        [&pm](Marshaler& m) {
            (void)m.arg(0);
            GuestAddr msg = m.arg_ptr(1);
            std::string text;
            if (msg != 0) {
                auto s = m.read_ansi_string(msg, 1024);
                if (s.is_ok()) text = s.value();
            }
            std::fprintf(stderr, "[process] FatalAppExitA: '%s'\n",
                         text.c_str());
            VITREO_TRACE("[process] FatalAppExitA: '%s'\n", text.c_str());
            pm.exit_process(1);
        },
        8
    });
    exports.entries.push_back(ExportEntry{
        "FatalAppExitW", 0,
        [&pm](Marshaler& m) {
            (void)m.arg(0);
            GuestAddr msg = m.arg_ptr(1);
            std::string text;
            if (msg != 0) {
                auto s = m.read_wide_string(msg, 1024);
                if (s.is_ok()) text = s.value();
            }
            std::fprintf(stderr, "[process] FatalAppExitW: '%s'\n",
                         text.c_str());
            VITREO_TRACE("[process] FatalAppExitW: '%s'\n", text.c_str());
            pm.exit_process(1);
        },
        8
    });

    // GetCurrentProcess (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "GetCurrentProcess", 2,
        [&pm](Marshaler& m) { impl_GetCurrentProcess(m, pm); },
        0
    });

    // GetCurrentProcessId (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "GetCurrentProcessId", 3,
        [&pm](Marshaler& m) { impl_GetCurrentProcessId(m, pm); },
        0
    });

    // GetCurrentThread (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "GetCurrentThread", 4,
        [&pm](Marshaler& m) { impl_GetCurrentThread(m, pm); },
        0
    });

    // GetCurrentThreadId (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "GetCurrentThreadId", 5,
        [&pm](Marshaler& m) { impl_GetCurrentThreadId(m, pm); },
        0
    });

    // IsDebuggerPresent (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "IsDebuggerPresent", 6,
        [&pm](Marshaler& m) { impl_IsDebuggerPresent(m, pm); },
        0
    });

    // GetStartupInfoW (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GetStartupInfoW", 7,
        [&pm](Marshaler& m) { impl_GetStartupInfoW(m, pm); },
        4
    });

    // SetThreadUILanguage (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "SetThreadUILanguage", 0,
        [](Marshaler& m) { m.set_return(m.arg(0)); },
        4
    });

    // UnhandledExceptionFilter (1 arg, stdcall_bytes=4)
    // Learn: calls the filter installed by SetUnhandledExceptionFilter (when
    // the process is not being debugged) and returns that filter's disposition.
    // With no filter, Windows displays an error box and returns
    // EXCEPTION_EXECUTE_HANDLER (1). Vitreo has no debugger, so always invoke.
    exports.entries.push_back(ExportEntry{
        "UnhandledExceptionFilter", 0,
        [](Marshaler& m) {
            const GuestAddr exception_pointers =
                static_cast<GuestAddr>(m.arg(0));
            const u64 filter = unhandled_exception_filter_guest();
            if (filter == 0 || !g_unhandled_exception_filter_invoker) {
                m.set_return(1); // EXCEPTION_EXECUTE_HANDLER
                return;
            }
            const u64 disposition = g_unhandled_exception_filter_invoker(
                static_cast<GuestAddr>(filter), exception_pointers,
                m.cpu_is_64bit());
            m.set_return(disposition);
        },
        4
    });

    // SetUnhandledExceptionFilter (1 arg, stdcall_bytes=4)
    // Returns the previous top-level exception filter (or NULL if none).
    // The stored pointer is dispatched after frame SEH ContinueSearch.
    exports.entries.push_back(ExportEntry{
        "SetUnhandledExceptionFilter", 0,
        [](Marshaler& m) {
            m.set_return(set_unhandled_exception_filter_guest(m.arg(0)));
        },
        4
    });

    return exports;
}

// =============================================================================
// Process Handle APIs (OpenProcess)
// =============================================================================

static void impl_OpenProcess(Marshaler& m, ProcessManager& pm, HandleTable& ht) {
    // HANDLE WINAPI OpenProcess(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwProcessId);
    const u32 desired_access = static_cast<u32>(m.arg(0));
    bool inherit = (m.arg(1) != 0);
    u32 pid = static_cast<u32>(m.arg(2));

    if (pid == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    // Windows keeps a process object openable by PID until all handles are
    // closed. Vitreo's capsule snapshot drops dead host PIDs immediately, but
    // CreateProcess parents still track them via the process reaper — allow
    // OpenProcess to reopen those exited peers. Live peers still come from
    // the capsule snapshot.
    // PID 4 is the Windows System process: EnumProcesses always lists it, and
    // OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, …, 4) succeeds on Windows
    // (process_contract_probe.expected).
    constexpr u32 kWindowsSystemPid = 4;
    ipc::ProcessObject* obj =
        ipc::find_tracked_process(static_cast<pid_t>(pid));
    if (obj == nullptr) {
        if (pid != kWindowsSystemPid &&
            pid != pm.get_current_process_id() && !pm.find_process(pid)) {
            ipc::ipc_log(
                "OpenProcess(pid=%u) -> miss (not in capsule snapshot)",
                pid);
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        obj = ipc::track_process(static_cast<pid_t>(pid));
        if (obj == nullptr) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
    }
    u32 flags = inherit ? HandleFlags::Inherit : 0;
    Handle h = ht.allocate(HandleType::Process, obj, flags, desired_access);
    ipc::ipc_log("OpenProcess(pid=%u access=0x%x) -> 0x%x",
                 pid, desired_access, h);
    m.set_return(h);
}

// Terminate the process @p handle names with @p exit_code, as
// NtTerminateProcess does; TerminateProcess is this with the status mapped to
// a Win32 error. Every status is nt_terminate_process_probe.expected (x64 and
// WoW64 agree). @p api names the caller in the diagnostics.
static u32 terminate_process_status(Marshaler& m, ProcessManager& pm,
                                    HandleTable& ht, Handle handle,
                                    u32 exit_code, const char* api) {
    constexpr u32 PROCESS_TERMINATE = 0x0001;

    u64 caller_ret = 0;
    std::array<u64, 8> caller_stack{};
    if (auto* cpu = m.cpu()) {
        if (cpu->is_64bit()) {
            for (std::size_t i = 0; i < caller_stack.size(); ++i) {
                auto r = m.memory().read_u64(cpu->sp() + i * sizeof(u64));
                if (r.is_ok()) caller_stack[i] = r.value();
            }
            caller_ret = caller_stack[0];
        } else {
            for (std::size_t i = 0; i < caller_stack.size(); ++i) {
                auto r = m.memory().read_u32(cpu->sp() + i * sizeof(u32));
                if (r.is_ok()) caller_stack[i] = static_cast<u64>(r.value());
            }
            caller_ret = caller_stack[0];
        }
    }
    std::fprintf(stderr,
                 "[process] TerminateProcess pid=%d handle=0x%x code=0x%x "
                 "caller_ret=0x%llx\n",
                 static_cast<int>(::getpid()), handle, exit_code,
                 static_cast<unsigned long long>(caller_ret));
    std::fflush(stderr);
    VITREO_TRACE(
        "[process] TerminateProcess(handle=0x%x code=%u (0x%X) "
        "caller_ret=0x%llx)\n",
        handle, exit_code, exit_code,
        static_cast<unsigned long long>(caller_ret));
    if (m.cpu() && m.cpu()->is_64bit()) {
        VITREO_TRACE(
            "[process] TerminateProcess x64-stack "
            "+00=0x%llx +08=0x%llx +10=0x%llx +18=0x%llx "
            "+20=0x%llx +28=0x%llx +30=0x%llx +38=0x%llx\n",
            static_cast<unsigned long long>(caller_stack[0]),
            static_cast<unsigned long long>(caller_stack[1]),
            static_cast<unsigned long long>(caller_stack[2]),
            static_cast<unsigned long long>(caller_stack[3]),
            static_cast<unsigned long long>(caller_stack[4]),
            static_cast<unsigned long long>(caller_stack[5]),
            static_cast<unsigned long long>(caller_stack[6]),
            static_cast<unsigned long long>(caller_stack[7]));
    }
    if (std::getenv("VITREO_PROCESS_TERMINATION_TRACE") != nullptr) {
        std::fprintf(
            stderr,
            "[process-termination] handle=0x%x code=%u caller=0x%llx "
            "stack=[0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx]\n",
            handle, exit_code,
            static_cast<unsigned long long>(caller_ret),
            static_cast<unsigned long long>(caller_stack[0]),
            static_cast<unsigned long long>(caller_stack[1]),
            static_cast<unsigned long long>(caller_stack[2]),
            static_cast<unsigned long long>(caller_stack[3]),
            static_cast<unsigned long long>(caller_stack[4]),
            static_cast<unsigned long long>(caller_stack[5]),
            static_cast<unsigned long long>(caller_stack[6]),
            static_cast<unsigned long long>(caller_stack[7]));
    }

    // The current-process pseudo handle is not present in the HandleTable.
    if (handle == CURRENT_PROCESS_PSEUDO) {
        ipc::ipc_log("%s(self, code=0x%x) caller_ret=0x%llx", api,
                     exit_code,
                     static_cast<unsigned long long>(caller_ret));
        log_self_termination_chain(m, pm.module_manager(), api);
        vitreo::diagnostics::dump_hostcalls("TerminateProcess(self)");
        pm.exit_process(exit_code);
        return nt_status::STATUS_SUCCESS;
    }

    const HandleType type = ht.get_type(handle);
    if (type != HandleType::Process) {
        // "event handle" / "invalid handle 0x1234".
        return type == HandleType::Invalid
                   ? nt_status::STATUS_INVALID_HANDLE
                   : nt_status::STATUS_OBJECT_TYPE_MISMATCH;
    }

    const u32 access = ht.get_access(handle);
    if (access != HandleAccess::All && (access & PROCESS_TERMINATE) == 0) {
        return nt_status::STATUS_ACCESS_DENIED;  // "query access only"
    }

    auto* process = static_cast<ipc::ProcessObject*>(
        ht.get(handle, HandleType::Process));
    if (process == nullptr) {
        return nt_status::STATUS_INVALID_HANDLE;
    }

    // DuplicateHandle/OpenProcess can produce a real handle for this process.
    // It must have the same effect as the pseudo handle, without signaling the
    // host process out from underneath Vitreo's orderly guest shutdown.
    if (process->pid == static_cast<pid_t>(pm.get_current_process_id())) {
        ipc::ipc_log("%s(self, code=0x%x) caller_ret=0x%llx", api,
                     exit_code,
                     static_cast<unsigned long long>(caller_ret));
        log_self_termination_chain(m, pm.module_manager(), api);
        vitreo::diagnostics::dump_hostcalls("TerminateProcess(self-handle)");
        pm.exit_process(exit_code);
        return nt_status::STATUS_SUCCESS;
    }

    if (!ipc::terminate_process(*process, exit_code)) {
        // "terminated child again": the process object is valid but already
        // terminated (EPERM is the same observable result).
        return nt_status::STATUS_PROCESS_IS_TERMINATING;
    }
    return nt_status::STATUS_SUCCESS;
}

static void impl_TerminateProcess(Marshaler& m, ProcessManager& pm,
                                  HandleTable& ht) {
    const Handle handle = static_cast<Handle>(m.arg(0));
    const u32 exit_code = static_cast<u32>(m.arg(1));
    const u32 status = terminate_process_status(m, pm, ht, handle, exit_code,
                                                "TerminateProcess");
    if (status == nt_status::STATUS_SUCCESS) {
        m.set_return(1);
        return;
    }
    // RtlNtStatusToDosError: a handle of another type is ERROR_INVALID_HANDLE,
    // and so is an invalid one; denied access and an already-terminated
    // process are both ERROR_ACCESS_DENIED.
    set_last_error(status == nt_status::STATUS_ACCESS_DENIED ||
                           status == nt_status::STATUS_PROCESS_IS_TERMINATING
                       ? error_codes::ERROR_ACCESS_DENIED
                       : error_codes::ERROR_INVALID_HANDLE);
    m.set_return(0);
}

static void impl_NtTerminateProcess(Marshaler& m, ProcessManager& pm,
                                    HandleTable& ht) {
    const Handle handle = static_cast<Handle>(m.arg(0));
    const u32 exit_status = static_cast<u32>(m.arg(1));
    if (handle == NULL_HANDLE) {
        // NULL ends every other thread of the process with ExitStatus, which
        // becomes the process's exit code, and returns STATUS_SUCCESS to the
        // caller ("child NtTerminateProcess(NULL)"). Vitreo cannot end the
        // other threads alone.
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED ntdll!NtTerminateProcess: "
                         "NULL handle (end every other thread)\n");
        }
        m.set_return(nt_status::STATUS_NOT_IMPLEMENTED);
        return;
    }
    m.set_return(terminate_process_status(m, pm, ht, handle, exit_status,
                                          "NtTerminateProcess"));
}

DllExports make_process_handle_exports(ProcessManager& pm, HandleTable& ht) {
    DllExports exports;
    // OpenProcess (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "OpenProcess", 0,
        [&pm, &ht](Marshaler& m) { impl_OpenProcess(m, pm, ht); },
        12
    });
    // TerminateProcess (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "TerminateProcess", 0,
        [&pm, &ht](Marshaler& m) {
            impl_TerminateProcess(m, pm, ht);
        },
        8
    });
    return exports;
}

DllExports make_nt_process_handle_exports(ProcessManager& pm,
                                          HandleTable& ht) {
    DllExports exports;
    // NTSTATUS NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus)
    exports.entries.push_back(ExportEntry{
        "NtTerminateProcess", 0,
        [&pm, &ht](Marshaler& m) { impl_NtTerminateProcess(m, pm, ht); },
        8
    });
    return exports;
}

} // namespace vitreo::winapi
