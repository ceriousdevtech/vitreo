/// @file kernel32_in_chain.cpp
/// @brief kernel32 in-chain classes (see kernel32_in_chain.hpp).
///
/// Sources: the Microsoft Learn reference pages of each export (Win32
/// profileapi, sysinfoapi, errhandlingapi, processthreadsapi, synchapi,
/// fibersapi, debugapi, interlockedapi). The names are checked against the
/// Windows export list tools/gen_windows_export_names.py captured
/// (kernelbase_export_names.inc) by Kernel32InChainClassTest.

#include "winapi/kernel32/kernel32_in_chain.hpp"

#include <array>

namespace vitreo::winapi::kernel32 {

namespace {

constexpr InChainClass A = InChainClass::A;

constexpr std::string_view kClock =
    "reads a clock into the return value or the caller's buffer and returns "
    "(Learn: QueryPerformanceCounter/Frequency return nonzero on every system "
    "since XP; GetTickCount[64] and GetSystemTimeAsFileTime cannot fail)";
constexpr std::string_view kLastError =
    "reads or writes the calling thread's last-error value "
    "(TEB.LastErrorValue) and returns";
constexpr std::string_view kIdentity =
    "returns the calling thread's or process's identifier";
constexpr std::string_view kDebugger =
    "returns whether a user-mode debugger is attached (FALSE here)";
constexpr std::string_view kTls =
    "reads or writes the calling thread's TLS slot and returns; failure "
    "reports ERROR_INVALID_PARAMETER (TlsGetValue sets ERROR_SUCCESS on "
    "success)";
constexpr std::string_view kFls =
    "reads or writes the calling fiber's FLS slot and returns; an FLS "
    "callback runs only on FlsFree and fiber or thread exit, never here";
constexpr std::string_view kCsEnter =
    "waits for ownership of the critical section and returns, or leaves only "
    "through the crossing's structural exits, with process exit observed. "
    "The wait is a host wait with the host call published. Learn allows "
    "EXCEPTION_POSSIBLE_DEADLOCK (a wait past the CriticalSectionTimeout "
    "registry value): a raise, which the crossing answers with BreakOuter "
    "and pending_raise. While the process exits a blocked call returns "
    "without ownership (Learn: it terminates); the crossing tests "
    "has_exited() and leaves before the next guest instruction";
constexpr std::string_view kCsNoWait =
    "releases or tries to take the critical section without waiting and "
    "returns";
constexpr std::string_view kSrwAcquire =
    "waits for the SRW lock and returns (a host wait, published)";
constexpr std::string_view kSrwNoWait =
    "releases or tries to take the SRW lock without waiting and returns";
constexpr std::string_view kInterlocked =
    "atomically updates a LONG and returns its value (x86 kernel32 exports; "
    "x64 code uses the compiler intrinsics)";
constexpr std::string_view kSleep =
    "suspends the thread for at least the given time and returns; not "
    "alertable, so no APC runs (SleepEx is not in-chain)";
constexpr std::string_view kYield =
    "yields the processor to another ready thread and returns whether one "
    "ran; no object wait, no callback";
constexpr std::string_view kEvent =
    "sets or clears the event's state and returns BOOL (GetLastError on "
    "failure); waiters are other threads, the caller never waits";

constexpr std::array<InChainRule, 30> kRules{{
    {"QueryPerformanceCounter", A, kClock},
    {"QueryPerformanceFrequency", A, kClock},
    {"GetTickCount", A, kClock},
    {"GetTickCount64", A, kClock},
    {"GetSystemTimeAsFileTime", A, kClock},
    {"GetLastError", A, kLastError},
    {"SetLastError", A, kLastError},
    {"GetCurrentThreadId", A, kIdentity},
    {"GetCurrentProcessId", A, kIdentity},
    {"IsDebuggerPresent", A, kDebugger},
    {"TlsGetValue", A, kTls},
    {"TlsSetValue", A, kTls},
    {"FlsGetValue", A, kFls},
    {"FlsSetValue", A, kFls},
    {"EnterCriticalSection", A, kCsEnter},
    {"LeaveCriticalSection", A, kCsNoWait},
    {"TryEnterCriticalSection", A, kCsNoWait},
    {"AcquireSRWLockExclusive", A, kSrwAcquire},
    {"AcquireSRWLockShared", A, kSrwAcquire},
    {"ReleaseSRWLockExclusive", A, kSrwNoWait},
    {"ReleaseSRWLockShared", A, kSrwNoWait},
    {"TryAcquireSRWLockExclusive", A, kSrwNoWait},
    {"TryAcquireSRWLockShared", A, kSrwNoWait},
    {"InterlockedIncrement", A, kInterlocked},
    {"InterlockedDecrement", A, kInterlocked},
    {"InterlockedCompareExchange", A, kInterlocked},
    {"Sleep", A, kSleep},
    {"SwitchToThread", A, kYield},
    {"SetEvent", A, kEvent},
    {"ResetEvent", A, kEvent},
}};

}  // namespace

std::span<const InChainRule> in_chain_rules() noexcept { return kRules; }

const InChainRule* in_chain_rule(std::string_view name) noexcept {
    for (const InChainRule& rule : kRules) {
        if (rule.name == name) return &rule;
    }
    return nullptr;
}

void apply_in_chain_classes(DllExports& exports) noexcept {
    for (ExportEntry& entry : exports.entries) {
        if (entry.direct.fn != nullptr) continue;
        if (const InChainRule* rule = in_chain_rule(entry.name)) {
            entry.in_chain = rule->cls;
        }
    }
}

}  // namespace vitreo::winapi::kernel32
