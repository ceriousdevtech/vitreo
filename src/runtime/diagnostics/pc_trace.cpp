// SPDX-License-Identifier: LGPL-2.1-only

/// @file pc_trace.cpp
/// @brief VITREO_PCTRACE — focused per-instruction guest tracing.
///
/// A divergence-hunting tool: when a guest function returns a different value
/// under Vitreo than under Windows, this records every instruction inside a
/// chosen RIP range so the two streams can be diffed to the first difference.
///
/// Entirely opt-in. With VITREO_PCTRACE unset, `pctrace_active()` is false and
/// the execution loop's calls here cost a predictable branch. That is what
/// makes this separable from the loop it instruments: it observes execution,
/// it does not participate in it.
///
/// Split out of cli/commands/execution_loop.cpp, which was 7,510 lines.

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
#include <atomic>
#include <array>
#include <cctype>
#include <cstdio>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <vector>
#include <mach/mach_time.h>
#include <csignal>
#include "runtime/diagnostics/pc_trace.hpp"

namespace vitreo::cli {

// ============================================================================
// VITREO_PCTRACE — focused per-instruction trace within a guest RIP range.
//
// Designed for narrow divergence hunts (e.g. "function at video.dll+0x17fd8c
// returns AL!=0 here but AL=0 on Windows — where does state first diverge?").
//
// Env vars:
//   VITREO_PCTRACE_MODULE=NAME          optional loaded-module name. When set,
//                                       ranges and memtaps are module RVAs.
//   VITREO_PCTRACE_RANGE=LO-HI[,LO-HI] half-open absolute ranges, or RVAs when
//                                       VITREO_PCTRACE_MODULE is set.
//   VITREO_PCTRACE_FILE=/path/log      output file. Default: stderr. Every
//                                       "%p" is replaced with the host PID so
//                                       multi-process guests do not overwrite
//                                       one another's traces.
//   VITREO_PCTRACE_MAX=N               stop tracing after N instructions
//                                       (default 200000) — protects against
//                                       loops blowing up the log.
//   VITREO_PCTRACE_RING=N              optional ring-buffer mode: keep only
//                                       the last N entries, flush on exit /
//                                       fault. Default 0 (stream).
//   VITREO_PCTRACE_MEMTAPS=A1,A2,..    comma-separated hex guest addresses;
//                                       module RVAs when MODULE is set.
//   VITREO_TRACE_MILESTONE=ADDR         arm the process-wide trace milestone
//                                       when RIP reaches ADDR. Accepts an
//                                       absolute address, an RVA with MODULE,
//                                       or MODULE+RVA.
//   VITREO_TRACE_AFTER_MILESTONE=1      suppress capture until the milestone
//                                       latch is reached.
//
// When active:
//   * JIT is forcibly skipped (we execute one instruction at a time via
//     interp.step()) — otherwise we'd only see block-entry RIPs.
//   * On each step we dump: RIP, instr bytes (up to 15), all 8 GPRs, EFLAGS,
//     FS/GS segment bases, ESP top-of-stack (4 dwords), and any memtap reads.
//
// Output format (one line per instruction):
//   PC#NNNN rip=XXXXXXXX EAX=.. ECX=.. EDX=.. EBX=.. ESP=.. EBP=.. ESI=.. EDI=..
//       FL=XXXXXXXX (zf=0 sf=0 cf=0 of=0) FS=XXXXXXXX
//       bytes=8B 4B 04 ...
//       [stk] +0=XX +4=XX +8=XX +C=XX
//       [tap] @XXXXXXXX=XX XX XX XX ...
// ============================================================================

namespace {

struct PcTraceRange {
    u64 lo = 0;
    u64 hi = 0;
};

struct PcTraceConfig {
    bool         enabled    = false;
    bool         module_relative = false;
    bool         after_milestone = false;
    std::string  module_name;
    std::string  milestone_module;
    u64          milestone_addr = 0;
    bool         milestone_relative = false;
    std::vector<PcTraceRange> ranges;
    u64          max_count  = 200000;
    size_t       ring_cap   = 0;   // 0 = stream mode
    std::FILE*   out        = nullptr;
    std::vector<u64> memtaps;
};

static std::atomic<winapi::ModuleManager*> g_pctrace_modules{nullptr};
static std::atomic<u64> g_pctrace_module_base{0};
static std::atomic<u64> g_pctrace_milestone_base{0};
static std::atomic<bool> g_pctrace_resolution_logged{false};

static bool pctrace_env_true(const char* name) {
    const char* value = std::getenv(name);
    return value && *value && std::strcmp(value, "0") != 0 &&
           std::strcmp(value, "false") != 0 && std::strcmp(value, "no") != 0;
}

static u64 pctrace_parse_address(const char* text, char** end) {
    // PCTRACE historically documented bare hexadecimal addresses. Keep that
    // compatibility while also accepting conventional 0x-prefixed values.
    const int base =
        text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 0 : 16;
    return std::strtoull(text, end, base);
}

static bool pctrace_parse_ranges(const char* text,
                                 std::vector<PcTraceRange>& out) {
    const char* p = text;
    while (p && *p) {
        while (*p == ',' || std::isspace(static_cast<unsigned char>(*p))) ++p;
        if (!*p) break;
        char* end = nullptr;
        const u64 lo = pctrace_parse_address(p, &end);
        if (end == p || *end != '-') return false;
        p = end + 1;
        const u64 hi = pctrace_parse_address(p, &end);
        if (end == p || hi <= lo) return false;
        out.push_back({lo, hi});
        p = end;
        while (std::isspace(static_cast<unsigned char>(*p))) ++p;
        if (*p && *p != ',') return false;
    }
    return !out.empty();
}

static std::string pctrace_expand_output_path(const char* path) {
    std::string expanded(path);
    const std::string pid = std::to_string(static_cast<int>(::getpid()));
    constexpr const char* placeholder = "%p";
    size_t offset = 0;
    while ((offset = expanded.find(placeholder, offset)) !=
           std::string::npos) {
        expanded.replace(offset, 2, pid);
        offset += pid.size();
    }
    return expanded;
}

static PcTraceConfig& pctrace_config() {
    static PcTraceConfig cfg = []() {
        PcTraceConfig c;
        const char* r = std::getenv("VITREO_PCTRACE_RANGE");
        if (!r || !*r) return c;
        if (!pctrace_parse_ranges(r, c.ranges)) return c;
        if (const char* module = std::getenv("VITREO_PCTRACE_MODULE");
            module && *module) {
            c.module_name = module;
            c.module_relative = true;
        }
        c.after_milestone = pctrace_env_true("VITREO_TRACE_AFTER_MILESTONE");
        if (const char* milestone = std::getenv("VITREO_TRACE_MILESTONE");
            milestone && *milestone) {
            std::string spec(milestone);
            const size_t plus = spec.rfind('+');
            const char* number = spec.c_str();
            if (plus != std::string::npos) {
                c.milestone_module = spec.substr(0, plus);
                number = spec.c_str() + plus + 1;
                c.milestone_relative = true;
            } else if (c.module_relative) {
                c.milestone_module = c.module_name;
                c.milestone_relative = true;
            }
            char* end = nullptr;
            c.milestone_addr = pctrace_parse_address(number, &end);
            if (end == number || *end != '\0') c.milestone_addr = 0;
        }
        c.enabled = true;

        if (const char* mx = std::getenv("VITREO_PCTRACE_MAX")) {
            u64 v = std::strtoull(mx, nullptr, 0);
            if (v) c.max_count = v;
        }
        if (const char* rc = std::getenv("VITREO_PCTRACE_RING")) {
            u64 v = std::strtoull(rc, nullptr, 0);
            if (v) c.ring_cap = static_cast<size_t>(v);
        }
        if (const char* mt = std::getenv("VITREO_PCTRACE_MEMTAPS")) {
            const char* p = mt;
            while (*p) {
                char* end = nullptr;
                u64 a = std::strtoull(p, &end, 16);
                if (end == p) break;
                c.memtaps.push_back(a);
                if (*end == ',') ++end;
                p = end;
            }
        }
        const char* path = std::getenv("VITREO_PCTRACE_FILE");
        std::string expanded_path;
        if (path && *path) {
            expanded_path = pctrace_expand_output_path(path);
            c.out = std::fopen(expanded_path.c_str(), "w");
        } else {
            c.out = stderr;
        }
        if (!c.out) c.out = stderr;
        std::fprintf(c.out,
            "# pctrace-v1 source=vitreo pid=%d module=%s addressing=%s "
            "ranges=%zu max=%llu ring=%zu taps=%zu after_milestone=%u "
            "milestone=%s output=%s\n",
            static_cast<int>(::getpid()),
            c.module_name.empty() ? "-" : c.module_name.c_str(),
            c.module_relative ? "module-rva" : "absolute", c.ranges.size(),
            (unsigned long long)c.max_count,
            c.ring_cap, c.memtaps.size(),
            c.after_milestone ? 1u : 0u,
            c.milestone_addr ? (c.milestone_relative ? "module-rva" : "absolute")
                             : "external",
            expanded_path.empty() ? "stderr" : expanded_path.c_str());
        std::fflush(c.out);
        return c;
    }();
    return cfg;
}

static std::atomic<u64> g_pctrace_count{0};

static u64 pctrace_resolve_module(std::string_view name,
                                  std::atomic<u64>& cached) {
    u64 base = cached.load(std::memory_order_acquire);
    if (base != 0) return base;
    winapi::ModuleManager* modules =
        g_pctrace_modules.load(std::memory_order_acquire);
    if (!modules) return 0;
    base = modules->get_module_handle(name);
    if (base != 0) cached.store(base, std::memory_order_release);
    return base;
}

static u64 pctrace_range_base(const PcTraceConfig& cfg) {
    if (!cfg.module_relative) return 0;
    const u64 base =
        pctrace_resolve_module(cfg.module_name, g_pctrace_module_base);
    if (base != 0 &&
        !g_pctrace_resolution_logged.exchange(true, std::memory_order_acq_rel)) {
        const auto* modules =
            g_pctrace_modules.load(std::memory_order_acquire);
        const auto* info = modules ? modules->get_module_info_by_handle(base)
                                   : nullptr;
        const u64 size =
            info && info->image_end > base ? info->image_end - base : 0;
        std::fprintf(cfg.out,
                     "# pctrace-module name=%s base=0x%llX size=0x%llX\n",
                     cfg.module_name.c_str(),
                     static_cast<unsigned long long>(base),
                     static_cast<unsigned long long>(size));
        std::fflush(cfg.out);
    }
    return base;
}

static u64 pctrace_milestone_address(const PcTraceConfig& cfg) {
    if (cfg.milestone_addr == 0) return 0;
    u64 milestone = cfg.milestone_addr;
    if (cfg.milestone_relative) {
        const std::string_view module =
            cfg.milestone_module.empty() ? std::string_view(cfg.module_name)
                                         : std::string_view(cfg.milestone_module);
        const u64 base = module == cfg.module_name
            ? pctrace_range_base(cfg)
            : pctrace_resolve_module(module, g_pctrace_milestone_base);
        if (base == 0 || milestone > std::numeric_limits<u64>::max() - base) {
            return 0;
        }
        milestone += base;
    }
    return milestone;
}

static bool pctrace_milestone_hit(const PcTraceConfig& cfg, u64 rip) {
    if (vitreo::diagnostics::trace_milestone_reached()) return false;
    const u64 milestone = pctrace_milestone_address(cfg);
    if (milestone == 0) return false;
    if (rip != milestone) return false;
    vitreo::diagnostics::set_trace_milestone_reached(true);
    std::fprintf(cfg.out,
                 "# pctrace-milestone rip=0x%llX\n",
                 static_cast<unsigned long long>(rip));
    std::fflush(cfg.out);
    return true;
}

static bool pctrace_gate_open(const PcTraceConfig& cfg, u64 rip) {
    (void)pctrace_milestone_hit(cfg, rip);
    return !cfg.after_milestone ||
           vitreo::diagnostics::trace_milestone_reached();
}

static bool pctrace_range_contains(const PcTraceConfig& cfg, u64 rip) {
    const u64 base = pctrace_range_base(cfg);
    if (cfg.module_relative && base == 0) return false;
    for (const auto& range : cfg.ranges) {
        if (range.lo > std::numeric_limits<u64>::max() - base ||
            range.hi > std::numeric_limits<u64>::max() - base) {
            continue;
        }
        if (rip >= base + range.lo && rip < base + range.hi) return true;
    }
    return false;
}


// Ring storage for VITREO_PCTRACE_RING=N mode.
struct PcTraceEntry {
    u64 rip;
    u64 gpr[16];
    u64 mm[8];
    u32 eflags;
    u64 fs_base;
    u64 gs_base;
    u64 stk[10];
    u8  bytes[15];
    u8  byte_len;
    // memtap snapshots: up to 4 taps × 16 bytes
    u64 tap_addr[4];
    u8  tap_data[4][16];
    u8  tap_count;
    /// xmm0-15 raw bytes. Without these a trace of SSE code shows control
    /// flow but not the values it was deciding on.
    u8  xmm[16][16];
};

static std::vector<PcTraceEntry>& pctrace_ring() {
    static std::vector<PcTraceEntry> r;
    return r;
}
static std::atomic<size_t> g_pctrace_ring_idx{0};

static void pctrace_format(std::FILE* out, u64 seq, const PcTraceEntry& e) {
    u32 fl = e.eflags;
    bool zf = (fl >> 6) & 1, sf = (fl >> 7) & 1;
    bool cf = (fl >> 0) & 1, of = (fl >> 11) & 1;
    std::fprintf(out,
        "PC#%06llu rip=%016llX "
        "RAX=%016llX RCX=%016llX RDX=%016llX RBX=%016llX "
        "RSP=%016llX RBP=%016llX RSI=%016llX RDI=%016llX "
        "R8=%016llX R9=%016llX R10=%016llX R11=%016llX "
        "R12=%016llX R13=%016llX R14=%016llX R15=%016llX "
        "FL=%08X(zf=%d sf=%d cf=%d of=%d) FS=%016llX GS=%016llX\n",
        (unsigned long long)seq, (unsigned long long)e.rip,
        (unsigned long long)e.gpr[0], (unsigned long long)e.gpr[1],
        (unsigned long long)e.gpr[2], (unsigned long long)e.gpr[3],
        (unsigned long long)e.gpr[4], (unsigned long long)e.gpr[5],
        (unsigned long long)e.gpr[6], (unsigned long long)e.gpr[7],
        (unsigned long long)e.gpr[8], (unsigned long long)e.gpr[9],
        (unsigned long long)e.gpr[10], (unsigned long long)e.gpr[11],
        (unsigned long long)e.gpr[12], (unsigned long long)e.gpr[13],
        (unsigned long long)e.gpr[14], (unsigned long long)e.gpr[15],
        fl, zf, sf, cf, of,
        (unsigned long long)e.fs_base, (unsigned long long)e.gs_base);
    std::fprintf(out, "    bytes=");
    for (u8 i = 0; i < e.byte_len; ++i) std::fprintf(out, "%02X ", e.bytes[i]);
    std::fprintf(out, "\n");
    for (u32 r = 0; r < 16; ++r) {
        float f[4]; double d[2];
        std::memcpy(f, e.xmm[r], 16); std::memcpy(d, e.xmm[r], 16);
        std::fprintf(out, "    xmm%-2u f=(%.9g, %.9g, %.9g, %.9g) d=(%.17g, %.17g)\n",
                     r, f[0], f[1], f[2], f[3], d[0], d[1]);
    }
    std::fprintf(out, "   ");
    std::fprintf(out,
                 "\n    [mm] 0=%016llX 1=%016llX 2=%016llX 3=%016llX "
                 "4=%016llX 5=%016llX 6=%016llX 7=%016llX\n",
                 (unsigned long long)e.mm[0], (unsigned long long)e.mm[1],
                 (unsigned long long)e.mm[2], (unsigned long long)e.mm[3],
                 (unsigned long long)e.mm[4], (unsigned long long)e.mm[5],
                 (unsigned long long)e.mm[6], (unsigned long long)e.mm[7]);
    std::fprintf(out, "    [stk] +0=%016llX +8=%016llX +10=%016llX +18=%016llX "
                 "+40=%016llX +48=%016llX\n",
                 (unsigned long long)e.stk[0], (unsigned long long)e.stk[1],
                 (unsigned long long)e.stk[2], (unsigned long long)e.stk[3],
                 (unsigned long long)e.stk[8], (unsigned long long)e.stk[9]);
    for (u8 i = 0; i < e.tap_count; ++i) {
        std::fprintf(out, "    [tap@%016llX]", (unsigned long long)e.tap_addr[i]);
        for (u8 j = 0; j < 16; ++j) std::fprintf(out, " %02X", e.tap_data[i][j]);
        std::fprintf(out, "\n");
    }
}


}  // namespace

// ---------------------------------------------------------------------------
// Declared in pc_trace.hpp — these must NOT sit in the anonymous namespace
// above, which would give them internal linkage and leave the execution
// loop's calls unresolved at link time.
// ---------------------------------------------------------------------------

void pctrace_set_module_manager(winapi::ModuleManager* modules) noexcept {
    g_pctrace_modules.store(modules, std::memory_order_release);
}

bool pctrace_active(u64 rip) {
    const auto& c = pctrace_config();
    return c.enabled && pctrace_gate_open(c, rip) &&
           pctrace_range_contains(c, rip) &&
           g_pctrace_count.load(std::memory_order_relaxed) < c.max_count;
}

bool pctrace_overlaps(u64 start, u64 length) {
    const auto& c = pctrace_config();
    if (!c.enabled ||
        g_pctrace_count.load(std::memory_order_relaxed) >= c.max_count) {
        return false;
    }
    if (length == 0 || start > std::numeric_limits<u64>::max() - length) {
        return false;
    }
    const u64 end = start + length;
    // A milestone may sit outside the eventual capture range. Force only the
    // block containing it through the interpreter so pctrace_active() observes
    // the exact instruction and opens the gate.
    if (!vitreo::diagnostics::trace_milestone_reached()) {
        const u64 milestone = pctrace_milestone_address(c);
        if (milestone >= start && milestone < end) return true;
    }
    if (!pctrace_gate_open(c, start)) return false;
    const u64 base = pctrace_range_base(c);
    if (c.module_relative && base == 0) return false;
    for (const auto& range : c.ranges) {
        if (range.lo > std::numeric_limits<u64>::max() - base ||
            range.hi > std::numeric_limits<u64>::max() - base) {
            continue;
        }
        if (start < base + range.hi && end > base + range.lo) return true;
    }
    return false;
}

/// Comma-separated guest RIPs via VITREO_PC_WATCH_ADDR. Unlike VITREO_CODE_WATCH
/// (interpreter-only), these force overlapping JIT blocks onto the interpreter
/// so mid-function probe points (Ping stub / WaitableEvent::Signal) are visible
/// at dispatch speed for the rest of the process.
const std::vector<u64>& pc_watch_addrs() {
    static const std::vector<u64> addrs = [] {
        std::vector<u64> v;
        const char* e = std::getenv("VITREO_PC_WATCH_ADDR");
        if (e == nullptr || e[0] == '\0') return v;
        const char* p = e;
        while (*p) {
            while (*p == ',' || *p == ' ') ++p;
            if (!*p) break;
            char* end = nullptr;
            const unsigned long long parsed = std::strtoull(p, &end, 0);
            if (end == p) break;
            v.push_back(static_cast<u64>(parsed));
            p = end;
        }
        return v;
    }();
    return addrs;
}

bool pc_watch_hit(u64 rip) {
    for (u64 a : pc_watch_addrs()) {
        if (a == rip) return true;
    }
    return false;
}

bool pc_watch_overlaps(u64 start, u64 length) {
    if (pc_watch_addrs().empty() || length == 0) return false;
    const u64 end = start + length;
    for (u64 a : pc_watch_addrs()) {
        if (a >= start && a < end) return true;
    }
    return false;
}

void pctrace_capture(cpu::CpuState& cpu, GuestMemory& memory) {
    auto& cfg = pctrace_config();
    u64 seq = g_pctrace_count.fetch_add(1, std::memory_order_relaxed);
    if (seq >= cfg.max_count) return;

    PcTraceEntry e{};
    e.rip = cpu.rip;
    cpu.materialize_lazy_flags();
    for (u32 i = 0; i < 16; ++i) e.gpr[i] = cpu.gpr.get64(static_cast<u8>(i));
    for (u32 i = 0; i < 8; ++i) e.mm[i] = cpu.fpu.get_mm(static_cast<u8>(i));
    for (u32 i = 0; i < 16; ++i) std::memcpy(e.xmm[i], &cpu.sse.xmm[i], 16);
    e.eflags  = static_cast<u32>(cpu.flags.rflags);
    e.fs_base = cpu.seg.fs_desc.base;
    e.gs_base = cpu.seg.gs_desc.base;

    for (u32 i = 0; i < 10; ++i) {
        auto r = memory.read_u64(static_cast<GuestAddr>(e.gpr[4] + i * 8));
        e.stk[i] = r.is_ok() ? r.value() : 0xDEADDEADDEADDEADull;
    }
    // Instruction bytes.
    e.byte_len = 0;
    for (u8 i = 0; i < 15; ++i) {
        auto r = memory.read_u8(static_cast<GuestAddr>(e.rip + i));
        if (!r.is_ok()) break;
        e.bytes[i] = r.value();
        e.byte_len = static_cast<u8>(i + 1);
    }
    // Memory taps.
    e.tap_count = 0;
    const u64 tap_base = cfg.module_relative ? pctrace_range_base(cfg) : 0;
    for (u64 a : cfg.memtaps) {
        if (e.tap_count >= 4) break;
        if (a > std::numeric_limits<u64>::max() - tap_base) continue;
        a += tap_base;
        e.tap_addr[e.tap_count] = a;
        for (u8 j = 0; j < 16; ++j) {
            auto r = memory.read_u8(static_cast<GuestAddr>(a + j));
            e.tap_data[e.tap_count][j] = r.is_ok() ? r.value() : 0;
        }
        ++e.tap_count;
    }

    if (cfg.ring_cap) {
        auto& ring = pctrace_ring();
        if (ring.empty()) ring.resize(cfg.ring_cap);
        size_t idx = g_pctrace_ring_idx.fetch_add(1, std::memory_order_relaxed);
        ring[idx % cfg.ring_cap] = e;
    } else {
        pctrace_format(cfg.out, seq, e);
        // Flush every entry. A traced guest usually ends via ExitProcess or a
        // fault, neither of which unwinds through stdio cleanup, so a buffered
        // tail is silently lost — and a truncated trace is worse than no trace
        // because it reads as a complete record of the range. Tracing already
        // forces the interpreter and bypasses the block cache, so the syscall
        // per entry is not what makes this path slow.
        std::fflush(cfg.out);
    }
}

bool pctrace_enabled() { return pctrace_config().enabled; }

std::FILE* pctrace_output() { return pctrace_config().out; }

extern "C" void vitreo_pctrace_dump_ring(const char* tag) {
    auto& cfg = pctrace_config();
    if (!cfg.enabled || !cfg.ring_cap) return;
    auto& ring = pctrace_ring();
    if (ring.empty()) return;
    size_t total = g_pctrace_ring_idx.load();
    size_t n = std::min<size_t>(total, cfg.ring_cap);
    size_t start = total > cfg.ring_cap ? total - cfg.ring_cap : 0;
    std::fprintf(cfg.out, "[pctrace-ring] %s n=%zu range=[%zu..%zu)\n",
                 tag ? tag : "", n, start, start + n);
    for (size_t i = 0; i < n; ++i) {
        size_t slot = (start + i) % cfg.ring_cap;
        pctrace_format(cfg.out, start + i, ring[slot]);
    }
    std::fflush(cfg.out);
}

}  // namespace vitreo::cli
