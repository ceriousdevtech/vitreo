// SPDX-License-Identifier: LGPL-2.1-only

/// @file hostcall_ring.cpp
/// @brief Ring buffer of recent guest→host API calls, for post-mortem dumps.
///
/// See hostcall_ring.hpp. Split out of cli/commands/execution_loop.cpp.
///
/// Note the linkage split below: everything declared in the header is defined
/// at namespace scope, and the filters and formatting helpers stay file-local.
/// A definition inside an anonymous namespace would have internal linkage no
/// matter what `static` says, which is how this goes wrong.

#include "core/cpu/state/cpu_state.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_block.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/system/time.hpp"
#include "winapi/media/winmm/winmm.hpp"
#include "graphics/common/perf_diag.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/memory/mem_watch.hpp"
#include "core/cpu/seh/seh32.hpp"
#include "winapi/msvcrt/runtime/cxx_eh.hpp"
#include <vitreo/diagnostics.hpp>
#include "runtime/diagnostics/pc_trace.hpp"
#include <atomic>
#include <array>
#include <cstdio>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <vector>
#include <mach/mach_time.h>
#include <csignal>
#include "runtime/diagnostics/hostcall_ring.hpp"
#include <vitreo/diagnostics.hpp>

namespace vitreo::cli {

// Marker file as well as the env vars, like /tmp/vitreo-ipc-trace and
// /tmp/vitreo-d3d11-diag: a guest launched by a store client is several
// re-execs removed from the shell and does not reliably inherit the
// environment, so an env-only switch is silently off in exactly the processes
// worth diagnosing -- and an empty ring then reads as "no API was called".
// `echo 65536 > /tmp/vitreo-hostcall-ring` enables capture; the contents, if
// numeric, set the capacity.
constexpr const char* kHostcallRingMarker = "/tmp/vitreo-hostcall-ring";

static bool hostcall_ring_marker_present() {
    static const bool present = ::access(kHostcallRingMarker, F_OK) == 0;
    return present;
}

bool hostcall_capture_enabled() {
    static const bool enabled =
        hostcall_ring_marker_present() ||
        std::getenv("VITREO_HOSTCALL_RING") != nullptr ||
        std::getenv("VITREO_HOSTCALL_HOT") != nullptr ||
        std::getenv("VITREO_HOSTCALL_DETAIL") != nullptr ||
        std::getenv("VITREO_TRACE_BAD_CALLER") != nullptr ||
        std::getenv("VITREO_TRACE_MODULE") != nullptr ||
        std::getenv("VITREO_TRACE_DURING_DLLMAIN") != nullptr;
    return enabled;
}


// ============================================================================
// Hostcall ring buffer (crash debugging)
//
// Records every dispatched x86 thunk call so that when STATUS_ACCESS_VIOLATION
// fires we can dump the last N hostcalls leading up to the crash. The actual
// crash deref is usually the symptom of an upstream Win32 API returning the
// wrong success/failure value — this ring is how we identify which API.
//
// Each entry captures:
//   - seq, tid, api name
//   - ESP before call + return address (direct caller in importing module)
//   - first 8 stack arg dwords [ESP+4..ESP+32]
//   - eax/ebx/ecx/edx before, eax after
//   - GetLastError before and after the call
//   - caller module name + RVA
//   - byte previews for the first two args when they look like readable
//     guest pointers (so we can see strings/structs passed in)
//
// Filtering env vars:
//   VITREO_HOSTCALL_RING       — capacity (default 1024)
//   VITREO_HOSTCALL_DUMP_LIMIT — non-teardown tail length (default 80, max 65536)
//   VITREO_TRACE_MODULE        — when set, only RECORD calls from this module
//                                (case-insensitive substring on module name)
//   VITREO_TRACE_DURING_DLLMAIN — when set to a module name, only record
//                                hostcalls made while that module's DllMain
//                                or TLS callback is on the guest stack.
//
// File-static so both run_loop's dispatch and run_guest_callback's dispatch
// (which is where DllMain AVs are surfaced) share one ring.
// ============================================================================
constexpr size_t HOSTCALL_ARG_COUNT = 8;
constexpr size_t HOSTCALL_PREVIEW_BYTES = 64;
constexpr size_t HOSTCALL_NAME_LEN = 72;
constexpr size_t HOSTCALL_MOD_LEN = 32;


static size_t hostcall_ring_capacity() {
    static size_t cap = []() -> size_t {
        char marker_value[32] = {};
        const char* e = std::getenv("VITREO_HOSTCALL_RING");
        if (hostcall_ring_marker_present()) {
            if (FILE* f = std::fopen(kHostcallRingMarker, "r")) {
                if (std::fgets(marker_value, sizeof(marker_value), f) &&
                    marker_value[0] >= '0' && marker_value[0] <= '9') {
                    e = marker_value;
                }
                std::fclose(f);
            }
        }
        if (!e) return 1024;
        long v = std::strtol(e, nullptr, 10);
        if (v < 16) v = 16;
        if (v > 65536) v = 65536;
        return static_cast<size_t>(v);
    }();
    return cap;
}

// The ring is deliberately leaked (immortal) rather than held in a
// function-local static.
//
// ExitProcess only sets a flag; the main thread then unwinds out of main() and
// the C++ runtime destroys static objects — but guest worker threads are still
// running and still recording hostcalls. A destroyed std::vector frees its
// buffer, so the next `ring[i] = e` memmoves ~456 bytes into freed pages and
// the process dies of SIGBUS in _platform_memmove during teardown. That was
// masking the guest installer's own shutdown and handoff path. An immortal allocation
// keeps the storage valid for the whole process lifetime, which is what a
// crash-diagnostics ring needs anyway.
static std::vector<HostcallEntry>& hostcall_ring_storage() {
    static std::vector<HostcallEntry>* ring =
        new std::vector<HostcallEntry>(hostcall_ring_capacity());
    return *ring;
}

static std::atomic<u64> g_hostcall_seq{0};
// Written from every guest thread; must be atomic or the increment races and
// two threads can claim the same slot (torn entries in the post-mortem dump).
static std::atomic<size_t> g_hostcall_idx{0};

// True while a DllMain or TLS callback for the trace-target module is on
// the guest stack. Maintained by ModuleManager through the diagnostics gate.

static const char* hostcall_trace_module() {
    static const char* v = std::getenv("VITREO_TRACE_MODULE");
    return v;
}
static const char* hostcall_trace_during_dllmain() {
    static const char* v = std::getenv("VITREO_TRACE_DURING_DLLMAIN");
    return v;
}

static bool icase_contains(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return true;
    size_t hl = std::strlen(hay), nl = std::strlen(needle);
    if (nl > hl) return false;
    for (size_t i = 0; i + nl <= hl; ++i) {
        size_t j = 0;
        for (; j < nl; ++j) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
            if (a != b) break;
        }
        if (j == nl) return true;
    }
    return false;
}

static bool hostcall_should_record(const HostcallEntry& e) {
    if (const char* dm = hostcall_trace_during_dllmain()) {
        (void)dm;
        if (vitreo::diagnostics::module_init_depth() == 0) return false;
    }
    if (const char* mod = hostcall_trace_module()) {
        if (!icase_contains(e.caller_mod, mod)) return false;
    }
    return true;
}

static const char* hostcall_api_leaf(const char* name);

namespace {
std::mutex g_lifetime_mu;
std::unordered_map<std::string, u64> g_lifetime_counts;
std::vector<std::string> g_lifetime_order;
std::vector<HostcallEntry> g_watched;

bool hostcall_is_watched(const char* name) {
    const char* api = hostcall_api_leaf(name);
    static const char* const kWatch[] = {
        "CreateFileA", "CreateFileW",
        "LoadLibraryA", "LoadLibraryW", "LoadLibraryExA", "LoadLibraryExW",
        "GetProcAddress",
        "FindResourceA", "FindResourceW", "FindResourceExA", "FindResourceExW",
        "PathFileExistsA", "PathFileExistsW",
        "PathFindExtensionA", "PathFindFileNameA",
        "RegCreateKeyExA", "RegCreateKeyExW",
        "RegSetValueExA", "RegSetValueExW",
        "LZOpenFileA", "LZOpenFileW", "LZCopy", "LZClose",
        "GetExpandedNameA", "GetExpandedNameW",
        "MoveFileA", "MoveFileW", "CopyFileA", "CopyFileW", "CopyFileExW",
        "SHFileOperationA", "SHFileOperationW",
        "SetupDecompressOrCopyFileA", "SetupDecompressOrCopyFileW",
        "PathRenameExtensionA", "PathRenameExtensionW",
        "WriteFile", "ReadFile",
        "LdrRegisterDllNotification", "NtCreateFile",
        "NtQueryInformationFile", "NtAllocateVirtualMemory", "NtReadFile",
        "GetCurrentDirectoryA", "GetCurrentDirectoryW",
        "FindFirstFileA", "FindFirstFileW",
    };
    for (const char* w : kWatch) {
        if (std::strcmp(api, w) == 0) return true;
    }
    return false;
}
} // namespace

void hostcall_record(const HostcallEntry& e) {
    if (!hostcall_should_record(e)) return;
    auto& ring = hostcall_ring_storage();
    size_t slot = g_hostcall_idx.fetch_add(1, std::memory_order_relaxed);
    ring[slot % ring.size()] = e;
    std::lock_guard lock(g_lifetime_mu);
    auto [it, inserted] = g_lifetime_counts.emplace(e.name, 0);
    if (inserted) g_lifetime_order.push_back(e.name);
    ++it->second;
    if (hostcall_is_watched(e.name) && g_watched.size() < 4096) {
        g_watched.push_back(e);
    }
}

static void hostcall_dump_preview(const u8* p, size_t n) {
    // Hex.
    for (size_t i = 0; i < n; ++i) std::fprintf(stderr, "%02x ", p[i]);
    std::fprintf(stderr, " |");
    // ASCII printable.
    for (size_t i = 0; i < n; ++i) {
        u8 b = p[i];
        std::fputc((b >= 0x20 && b < 0x7f) ? b : '.', stderr);
    }
    std::fputc('|', stderr);
}

static const char* hostcall_api_leaf(const char* name) {
    if (!name) return "";
    const char* bang = std::strrchr(name, '!');
    return bang ? bang + 1 : name;
}

// CRT / heap / TLS noise that buries the last real WinAPI before ExitProcess.
static bool hostcall_is_teardown_noise(const char* name) {
    const char* api = hostcall_api_leaf(name);
    static const char* const kNoise[] = {
        "HeapAlloc", "HeapFree", "HeapReAlloc", "HeapSize", "HeapCreate",
        "HeapDestroy", "EnterCriticalSection", "LeaveCriticalSection",
        "TryEnterCriticalSection", "InitializeCriticalSection",
        "InitializeCriticalSectionAndSpinCount", "InitializeCriticalSectionEx",
        "DeleteCriticalSection", "TlsGetValue", "TlsSetValue", "TlsAlloc",
        "TlsFree", "FlsGetValue", "FlsSetValue", "FlsAlloc", "FlsFree",
        "EncodePointer", "DecodePointer", "GetLastError", "SetLastError",
        "GetCurrentThreadId", "GetCurrentProcessId", "GetCurrentProcess",
        "GetCurrentThread", "QueryPerformanceCounter",
        "QueryPerformanceFrequency", "InterlockedIncrement",
        "InterlockedDecrement", "InterlockedExchange",
        "InterlockedCompareExchange", "InterlockedExchangeAdd",
    };
    for (const char* n : kNoise) {
        if (std::strcmp(api, n) == 0) return true;
    }
    return false;
}

static void hostcall_dump_one(const HostcallEntry& e) {
    std::fprintf(stderr,
        "  #%llu T%u %s+0x%x %s%s rip=0x%llx\n",
        static_cast<unsigned long long>(e.seq), e.tid,
        e.caller_mod[0] ? e.caller_mod : "?", e.caller_rva, e.name,
        e.is_64bit ? " (x64)" : "",
        (unsigned long long)e.caller_rip);
    if (e.is_64bit) {
        std::fprintf(stderr,
            "    ret=0x%llx rsp=0x%llx rcx=%llx rdx=%llx r8=%llx r9=%llx stk5=%llx stk6=%llx stk7=%llx stk8=%llx\n",
            (unsigned long long)e.ret_addr, (unsigned long long)e.esp_before,
            (unsigned long long)e.args[0], (unsigned long long)e.args[1],
            (unsigned long long)e.args[2], (unsigned long long)e.args[3],
            (unsigned long long)e.args[4], (unsigned long long)e.args[5],
            (unsigned long long)e.args[6], (unsigned long long)e.args[7]);
        std::fprintf(stderr,
            "    rax=%llx->%llx rbx=%llx rcx=%llx rdx=%llx gle %u->%u\n",
            (unsigned long long)e.eax_in, (unsigned long long)e.eax_out,
            (unsigned long long)e.ebx_in, (unsigned long long)e.ecx_in,
            (unsigned long long)e.edx_in,
            e.gle_before, e.gle_after);
    } else {
        std::fprintf(stderr,
            "    ret=0x%08llx esp=0x%08llx args=[%08llx %08llx %08llx %08llx %08llx %08llx %08llx %08llx]\n",
            (unsigned long long)e.ret_addr, (unsigned long long)e.esp_before,
            (unsigned long long)e.args[0], (unsigned long long)e.args[1],
            (unsigned long long)e.args[2], (unsigned long long)e.args[3],
            (unsigned long long)e.args[4], (unsigned long long)e.args[5],
            (unsigned long long)e.args[6], (unsigned long long)e.args[7]);
        std::fprintf(stderr,
            "    eax=%08llx->%08llx ebx=%08llx ecx=%08llx edx=%08llx gle %u->%u\n",
            (unsigned long long)e.eax_in, (unsigned long long)e.eax_out,
            (unsigned long long)e.ebx_in, (unsigned long long)e.ecx_in,
            (unsigned long long)e.edx_in,
            e.gle_before, e.gle_after);
    }
    for (int a = 0; a < 2; ++a) {
        if (e.arg_preview_len[a] == 0) continue;
        std::fprintf(stderr, "    arg%d@0x%llx: ", a, (unsigned long long)e.args[a]);
        hostcall_dump_preview(e.arg_preview[a], e.arg_preview_len[a]);
        std::fputc('\n', stderr);
    }
    if (e.completion_snapshot) {
        std::fprintf(stderr,
            "    iocp readable=0x%x bytes=0x%llx key=0x%llx overlapped=0x%llx\n",
            e.completion_readable,
            (unsigned long long)e.completion_outputs[0],
            (unsigned long long)e.completion_outputs[1],
            (unsigned long long)e.completion_outputs[2]);
    }
    if (e.out_preview_len) {
        std::fprintf(stderr, "    out@0x%llx: ",
                     (unsigned long long)e.out_preview_addr);
        hostcall_dump_preview(e.out_preview, e.out_preview_len);
        std::fputc('\n', stderr);
    }
}

extern "C" void vitreo_hostcall_dump(const char* reason) {
    if (!hostcall_capture_enabled()) {
        // Dumps are requested from hot failure paths that can fire thousands
        // of times in one run. The notice is worth printing — it names the
        // knob that turns capture on — but only once per process; repeating
        // it per request adds nothing and buries every other log line.
        static std::atomic<bool> notice_printed{false};
        if (!notice_printed.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr,
                "[hostcall-ring] capture disabled (first request: %s); set "
                "VITREO_HOSTCALL_RING=<capacity> to enable (this notice is "
                "printed once)\n",
                reason ? reason : "");
        }
        return;
    }
    auto& ring = hostcall_ring_storage();
    const size_t cap = ring.size();
    size_t total = g_hostcall_idx.load(std::memory_order_relaxed);
    size_t start = (total >= cap) ? (total - cap) : 0;

    std::fprintf(stderr,
        "[hostcall-ring] === unique APIs in last %zu (%s, recorded=%zu) ===\n",
        cap, reason ? reason : "", total);
    {
        std::unordered_map<std::string, size_t> counts;
        std::vector<std::string> order;
        for (size_t i = start; i < total; ++i) {
            const char* n = ring[i % cap].name;
            auto [it, inserted] = counts.emplace(n, 0);
            if (inserted) order.emplace_back(n);
            ++it->second;
        }
        for (const auto& n : order) {
            std::fprintf(stderr, "  %s x%zu\n", n.c_str(), counts[n]);
        }
    }

    {
        std::lock_guard lock(g_lifetime_mu);
        std::fprintf(stderr,
            "[hostcall-ring] === lifetime unique APIs (%zu names, %s) ===\n",
            g_lifetime_order.size(), reason ? reason : "");
        for (const auto& n : g_lifetime_order) {
            std::fprintf(stderr, "  %s x%llu\n", n.c_str(),
                         static_cast<unsigned long long>(g_lifetime_counts[n]));
        }
    }

    const bool av_dump = reason && std::strstr(reason, "seh-cb-av") != nullptr;
    if (av_dump) {
        std::fprintf(stderr, "[hostcall-ring] === end (av unique-only) ===\n");
        return;
    }

    {
        std::lock_guard lock(g_lifetime_mu);
        std::fprintf(stderr,
            "[hostcall-ring] === watched hostcalls (%zu, %s) ===\n",
            g_watched.size(), reason ? reason : "");
        for (const auto& e : g_watched) {
            hostcall_dump_one(e);
        }
    }

    // A quiet failure followed by a message loop may be older than the default
    // tail. Allow a bounded full-ring diagnostic without changing capture cost.
    static const size_t kInterestingCap = [] {
        const char* value = std::getenv("VITREO_HOSTCALL_DUMP_LIMIT");
        if (!value) return size_t{80};
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end == value || *end != '\0' || parsed == 0) return size_t{80};
        return static_cast<size_t>(std::min(parsed, 65536ul));
    }();
    std::vector<size_t> interesting;
    for (size_t i = start; i < total; ++i) {
        if (!hostcall_is_teardown_noise(ring[i % cap].name)) {
            interesting.push_back(i);
        }
    }
    const size_t interesting_start =
        interesting.size() > kInterestingCap
            ? interesting.size() - kInterestingCap
            : 0;
    std::fprintf(stderr,
        "[hostcall-ring] === last %zu non-teardown hostcalls (%s) ===\n",
        interesting.size() - interesting_start, reason ? reason : "");
    for (size_t n = interesting_start; n < interesting.size(); ++n) {
        hostcall_dump_one(ring[interesting[n] % cap]);
    }

    // The same tail, restricted to the thread that asked for the dump. On a
    // process with dozens of busy threads the interleaved tail above can be
    // entirely other threads' waits, while the question is almost always
    // "what was THIS thread doing" -- a runaway recursion, a fault, a hang.
    constexpr size_t kThreadCap = 96;
    const u32 requesting_tid =
        winapi::ThreadManager::get_current_thread_id_static();
    std::vector<size_t> mine;
    for (size_t i = start; i < total; ++i) {
        if (ring[i % cap].tid == requesting_tid) mine.push_back(i);
    }
    const size_t mine_start =
        mine.size() > kThreadCap ? mine.size() - kThreadCap : 0;
    std::fprintf(stderr,
        "[hostcall-ring] === last %zu hostcalls on requesting thread T%u "
        "(%s) ===\n",
        mine.size() - mine_start, requesting_tid, reason ? reason : "");
    for (size_t n = mine_start; n < mine.size(); ++n) {
        hostcall_dump_one(ring[mine[n] % cap]);
    }
    std::fprintf(stderr, "[hostcall-ring] === end ===\n");
}

// Publish the dump to layers below this one. The CPU, the WinAPI thunks, and
// IPC all notice conditions worth dumping, but none of them can link the CLI
// that owns the ring — so they request a dump through the hook and this, the
// owning component, supplies it.
//
// Installed during static initialization, which is before any guest code can
// run and therefore before any dump can be requested.
namespace {
const struct HostcallDumpInstaller {
    HostcallDumpInstaller() noexcept {
        vitreo::diagnostics::set_hostcall_dump(&vitreo_hostcall_dump);
    }
} g_hostcall_dump_installer;
} // namespace

void hostcall_dump(const char* reason) {
    vitreo_hostcall_dump(reason);
}

// Fill the pre-call portion of a HostcallEntry from current CPU/memory.
// Captures ESP, return address, first 8 stack args, register inputs,
// the api name, the caller module/RVA, and best-effort byte previews of
// the first two arg pointers (only when they look like readable guest
// pointers). Output buffer preview is taken AFTER the call, separately.
// The ring records every hostcall on every guest thread, so anything it does
// per call is paid by the whole runtime. Each guest-memory access below takes
// GuestMemory's m_regions_mutex, and libc++'s std::shared_mutex serializes even
// readers on an internal mutex — so the stack-argument reads and pointer
// previews turn into cross-thread contention. Sampling a launcher client showed
// five worker threads each spending roughly two thirds of their time inside
// this function waiting on lock_shared().
//
// The default capture therefore keeps only what is free or nearly free: the
// sequence, thread, RIP, register inputs, LastError, the return address (one
// read), and on x64 the first four arguments (which live in registers). That is
// the subset that has actually resolved past defects. VITREO_HOSTCALL_DETAIL=1
// restores stack arguments 5-8 and the argument/output byte previews.
// VITREO_HOSTCALL_DETAIL_API limits extra reads to comma-separated API leaves.
// A focused error-buffer capture must not serialize unrelated worker calls.
static bool hostcall_detail_enabled() {
    static const bool on = std::getenv("VITREO_HOSTCALL_DETAIL") != nullptr;
    return on;
}

bool hostcall_detail_matches(std::string_view name, std::string_view selector) {
    if (selector.empty()) return true;
    const auto bang = name.rfind('!');
    if (bang != std::string_view::npos) name.remove_prefix(bang + 1);
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    while (!selector.empty()) {
        const auto comma = selector.find(',');
        auto token = selector.substr(0, comma);
        const auto first = token.find_first_not_of(" \t\r\n");
        if (first != std::string_view::npos) {
            token.remove_prefix(first);
            token = token.substr(0, token.find_last_not_of(" \t\r\n") + 1);
            if (token.size() == name.size() &&
                std::equal(token.begin(), token.end(), name.begin(),
                           [&](char a, char b) { return lower(a) == lower(b); })) return true;
        }
        if (comma == std::string_view::npos) break;
        selector.remove_prefix(comma + 1);
    }
    return false;
}

void hostcall_capture_completion_outputs(HostcallEntry& e, GuestMemory& memory) {
    e.completion_snapshot = true;
    e.completion_readable = 0;
    std::fill(std::begin(e.completion_outputs), std::end(e.completion_outputs), 0);
    for (size_t i = 0; i < 3; ++i) {
        const u64 address = e.args[i + 1];
        if (address < 0x10000 || address > 0x0000800000000000ull ||
            (!e.is_64bit && address >= 0xFFFF0000ull)) continue;
        if (i == 0 || !e.is_64bit) {
            auto value = memory.read_u32(address);
            if (!value.is_ok()) continue;
            e.completion_outputs[i] = value.value();
        } else {
            auto value = memory.read_u64(address);
            if (!value.is_ok()) continue;
            e.completion_outputs[i] = value.value();
        }
        e.completion_readable |= static_cast<u8>(1u << i);
    }
}

static bool hostcall_detail_for(const char* name) {
    if (!hostcall_detail_enabled()) return false;
    static const char* selected = std::getenv("VITREO_HOSTCALL_DETAIL_API");
    return hostcall_detail_matches(name, selected ? selected : "");
}

void hostcall_fill_pre(HostcallEntry& e,
                       const cpu::CpuState& cpu,
                       GuestMemory& memory,
                       const winapi::ThunkRegistry& registry,
                       winapi::ModuleManager* modules) {
    e.seq = g_hostcall_seq.fetch_add(1, std::memory_order_relaxed);
    e.tid = winapi::ThreadManager::get_current_thread_id_static();
    e.is_64bit = cpu.is_64bit() ? 1 : 0;
    e.caller_rip = cpu.rip;
    e.eax_in = cpu.gpr.rax;
    e.ecx_in = cpu.gpr.rcx;
    e.edx_in = cpu.gpr.rdx;
    e.ebx_in = cpu.gpr.rbx;
    e.gle_before = winapi::get_last_error();

    {
        std::string nm = registry.resolve_name(cpu.rip);
        std::snprintf(e.name, sizeof(e.name), "%s", nm.c_str());
    }
    const bool detail = hostcall_detail_for(e.name);

    if (!cpu.is_64bit()) {
        u32 esp = static_cast<u32>(cpu.sp());
        e.esp_before = esp;
        auto ra = memory.read_u32(esp);
        e.ret_addr = ra.is_ok() ? ra.value() : 0;
        // A 32-bit guest passes everything on the stack, so the first four
        // arguments are worth the reads; the rest are detail-only.
        const size_t count = detail ? HOSTCALL_ARG_COUNT : 4;
        for (size_t i = 0; i < count; ++i) {
            auto a = memory.read_u32(esp + 4 + 4 * static_cast<u32>(i));
            e.args[i] = a.is_ok() ? a.value() : 0;
        }
    } else {
        // x64 Win64 ABI: args 1-4 in RCX/RDX/R8/R9, args 5+ on stack at
        // [rsp+0x28], [rsp+0x30], ... Return address at [rsp].
        u64 rsp = cpu.sp();
        e.esp_before = rsp;
        auto ra = memory.read_u64(static_cast<GuestAddr>(rsp));
        e.ret_addr = ra.is_ok() ? ra.value() : 0;
        e.args[0] = cpu.gpr.rcx;
        e.args[1] = cpu.gpr.rdx;
        e.args[2] = cpu.gpr.r8;
        e.args[3] = cpu.gpr.r9;
        for (size_t i = 4; detail && i < HOSTCALL_ARG_COUNT; ++i) {
            // Win64 home space is at [rsp+8..+0x27]; stack args start at
            // [rsp+0x28] for the 5th arg. So arg i (0-based) lives at
            // [rsp + 0x28 + (i-4)*8] = [rsp + 8 + i*8] for i>=4.
            auto a = memory.read_u64(
                static_cast<GuestAddr>(rsp + 8 + i * 8));
            e.args[i] = a.is_ok() ? a.value() : 0;
        }
    }

    // Caller module + RVA (best effort; ModuleManager may not be wired
    // for early bootstrap thunks).
    if (modules != nullptr && e.ret_addr) {
        auto m = modules->module_for_address(
            static_cast<GuestAddr>(e.ret_addr));
        if (m.info) {
            std::snprintf(e.caller_mod, sizeof(e.caller_mod), "%s",
                          m.info->name.c_str());
            e.caller_rva = m.rva;
        }
    }

    // Pointer previews for arg0 and arg1 if they look readable.
    // VITREO_TRACE_MODULE already restricts which callers are recorded; take
    // previews for those callers even without VITREO_HOSTCALL_DETAIL so an
    // ExitProcess dump can show CreateFile/GetModuleHandle names.
    const bool want_preview =
        detail || (hostcall_trace_module() &&
                   icase_contains(e.caller_mod, hostcall_trace_module()));
    for (int a = 0; want_preview && a < 2; ++a) {
        u64 ptr = e.args[a];
        if (ptr < 0x10000) continue;
        // Defensive: skip wild ptrs (sign-extended / near max u64) that
        // can overflow address arithmetic deeper in the memory layer.
        if (ptr > 0x0000800000000000ull) continue;
        if (!e.is_64bit && ptr >= 0xFFFF0000ull) continue;
        auto probe = memory.probe_access(static_cast<GuestAddr>(ptr),
                                          HOSTCALL_PREVIEW_BYTES);
        if (probe.host_ptr && probe.readable) {
            std::memcpy(e.arg_preview[a], probe.host_ptr, HOSTCALL_PREVIEW_BYTES);
            e.arg_preview_len[a] = HOSTCALL_PREVIEW_BYTES;
        }
    }
}

// Capture an output-buffer snapshot AFTER the call. Argument zero remains the
// default because that is the canonical "out" location for the locale/CRT
// APIs this diagnostic originally targeted. VITREO_HOSTCALL_OUT_ARG selects a
// different zero-based argument when diagnosing APIs such as ReadFile, whose
// output buffer is argument one.
void hostcall_fill_post(HostcallEntry& e,
                               const cpu::CpuState& cpu,
                               GuestMemory& memory) {
    e.eax_out = cpu.gpr.rax;
    e.gle_after = winapi::get_last_error();
    if (!hostcall_detail_for(e.name)) return;
    if (std::strcmp(hostcall_api_leaf(e.name), "GetQueuedCompletionStatus") == 0)
        hostcall_capture_completion_outputs(e, memory);
    static const size_t out_arg = []() -> size_t {
        const char* value = std::getenv("VITREO_HOSTCALL_OUT_ARG");
        if (!value) return 0;
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        return end != value && *end == '\0' && parsed < HOSTCALL_ARG_COUNT
                   ? static_cast<size_t>(parsed)
                   : 0;
    }();
    u64 ptr = e.args[out_arg];
    bool ok = ptr >= 0x10000 && ptr <= 0x0000800000000000ull
              && (e.is_64bit || ptr < 0xFFFF0000ull);
    if (ok) {
        auto probe = memory.probe_access(static_cast<GuestAddr>(ptr),
                                          HOSTCALL_PREVIEW_BYTES);
        if (probe.host_ptr && probe.readable) {
            e.out_preview_addr = ptr;
            std::memcpy(e.out_preview, probe.host_ptr, HOSTCALL_PREVIEW_BYTES);
            e.out_preview_len = HOSTCALL_PREVIEW_BYTES;
        }
    }
}

}  // namespace vitreo::cli
