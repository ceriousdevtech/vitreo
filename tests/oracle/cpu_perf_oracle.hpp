// SPDX-License-Identifier: LGPL-2.1-only
/// @file cpu_perf_oracle.hpp
/// @brief Windows / the host x86 runner / Vitreo JIT throughput on identical snippets.
///
/// Same shape as the D3D oracle: one guest byte stream, multiple hosts,
/// compare a metric. The local comparison uses the host x86 runner (`x86_oracle_runner`).
/// Native Windows is the MinGW PE over `ssh -T Windows` — no desktop
/// session, unlike the D3D oracle.
///
/// Dumped JIT blocks usually end in a Jcc/JMP. Those are stripped so the
/// body can fall through into the host x86 runner epilogue. Blocks that still
/// contain CALL/INT/unconditional JMP are skipped — they are not a
/// comparable snippet.

#pragma once

#include "oracle/oracle_client.hpp"
#include "oracle/oracle_contract.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vitreo::oracle::perf {

struct DumpBlock {
    std::string capture_file;
    uint64_t rip = 0;
    uint32_t code_variant = UINT32_MAX;
    uint64_t exec_count = 0;
    uint64_t insn_count = 0;
    bool delegated = false;
    std::string cls;
    std::string accounting = "legacy_unspecified";
    uint32_t sample_rate = 0;
    bool linked_hops_included = false;
    std::vector<uint8_t> bytes;
};

struct SnippetBench {
    std::string name;
    std::string cls;
    std::string capture_file;
    uint64_t rip = 0;
    uint32_t code_variant = UINT32_MAX;
    uint64_t exec_count = 0;
    uint64_t insn_count = 0;
    uint32_t replay_count = 1;
    std::string accounting = "catalog_exact";
    uint32_t sample_rate = 0;
    bool linked_hops_included = false;
    std::vector<uint8_t> body;
    contract::JitBenchResult jit{};
    BenchTimes oracle{};
    BenchTimes windows{};
    std::string skip;
    std::string windows_skip;
};

struct Report {
    std::vector<SnippetBench> rows;
    uint32_t comparable = 0;
    uint32_t skipped = 0;
};

struct Windows64Fault {
    uint32_t code = 0;
    uint32_t flags = 0;
    uint32_t parameter_count = 0;
    uint64_t address = 0;
    std::array<uint64_t, 3> information{};
};

struct Windows64BenchResult {
    BenchTimes times{};
    std::array<uint64_t, 16> gpr{};
    uint64_t rflags = 0;
    uint64_t rip = 0;
    uint64_t initial_rsp = 0;
    uint64_t code_base = 0;
    uint32_t mxcsr = 0;
    std::array<std::array<uint8_t, 16>, 16> xmm{};
    uint16_t fcw = 0;
    uint16_t fsw = 0;
    uint8_t ftw = 0;
    std::array<std::array<uint8_t, 10>, 8> st{};
    std::array<uint8_t, 512> mem{};
    Windows64Fault fault{};
};

struct NormalizedNs {
    double raw_ns_per_iter = 0.0;
    double wrapper_ns_per_iter = 0.0;
    double net_ns_per_iter = 0.0;
};

struct Windows64NormalizedBenchResult {
    Windows64BenchResult raw{};
    BenchTimes wrapper{};
    NormalizedNs ns{};
};

struct X64CatalogEntry {
    std::string name;
    std::string cls;
    std::vector<uint8_t> body;
    State in{};
    uint32_t operations_per_iter = 1;
    bool multiblock = false;
};

struct X64JitNormalizedResult {
    contract::JitBenchResult raw{};
    contract::JitBenchResult wrapper{};
    NormalizedNs ns{};
    int64_t raw_stack_delta = 0;
    int64_t wrapper_stack_delta = 0;
    uint32_t raw_blocks_compiled = 0;
    uint32_t raw_final_dispatches = 0;
    uint32_t wrapper_blocks_compiled = 0;
    uint32_t wrapper_final_dispatches = 0;
};

struct X64CatalogTrial {
    X64JitNormalizedResult jit{};
    Windows64NormalizedBenchResult windows{};
    std::string skip;
    std::string windows_skip;
};

struct X64CatalogMedian {
    NormalizedNs jit{};
    NormalizedNs windows{};
    bool jit_valid = false;
    bool windows_valid = false;
    double raw_ratio = 0.0;
    bool raw_ratio_valid = false;
    double net_ratio = 0.0;
    bool net_ratio_valid = false;
};

struct X64CatalogRow {
    std::string name;
    std::string cls;
    std::vector<uint8_t> body;
    uint32_t operations_per_iter = 1;
    bool multiblock = false;
    X64JitNormalizedResult jit{};
    Windows64NormalizedBenchResult windows{};
    std::vector<X64CatalogTrial> trials;
    X64CatalogMedian median{};
    std::string skip;
    std::string windows_skip;
};

struct X64CatalogReport {
    std::vector<X64CatalogRow> rows;
    uint32_t comparable = 0;
    uint32_t windows_comparable = 0;
    uint32_t skipped = 0;
};

/// Decode @p bytes, drop a trailing branch/call/ret, reject CALL/INT/JMP
/// in the remaining body, and NOP mid-block Jccs so dump scratch cannot
/// take a branch out of the snippet.
[[nodiscard]] bool prepare_body(const std::vector<uint8_t>& bytes,
                                std::vector<uint8_t>& body,
                                std::string& why, bool nop_jcc = true);

[[nodiscard]] bool parse_hot_dump(const std::string& path,
                                  std::vector<DumpBlock>& out,
                                  std::string& error);

/// Point pointer-ish GPRs at the oracle scratch page and fill it.
[[nodiscard]] State scratch_state();

/// Same as scratch_state, but every GPR (including ECX) is a 16-aligned
/// pointer in the scratch page, ESP is low in the 512-byte State.mem
/// window, and every dword points at a 16-aligned address in that page.
/// Catalog scratch leaves ECX=16 for short REP; captured SSE does
/// `movaps [ecx+0x30]` and that EA SIGBUS-hangs JIT.
[[nodiscard]] State dump_scratch_state();

/// Warm-loop the host x86 runner and JIT. @p skip is set when either side cannot run.
void bench_body(const std::vector<uint8_t>& body, uint32_t warmup,
                uint32_t iters, SnippetBench& row);
void bench_body(const std::vector<uint8_t>& body, uint32_t warmup,
                uint32_t iters, SnippetBench& row, const State& in);

[[nodiscard]] Report bench_dump(const std::vector<DumpBlock>& blocks,
                                uint32_t warmup, uint32_t iters,
                                bool with_windows = false);

void print_report(const Report& report);
[[nodiscard]] bool write_report_json(const Report& report,
                                     const std::string& path,
                                     std::string& error);
[[nodiscard]] bool write_report_csv(const Report& report,
                                    const std::string& path,
                                    std::string& error);

/// JIT ns/iter ÷ the host x86 runner ns/iter. 0 if either side has no time.
[[nodiscard]] double ratio(const SnippetBench& row);

/// JIT ns/iter ÷ native Windows ns/iter. 0 if either side has no time.
[[nodiscard]] double ratio_windows(const SnippetBench& row);

/// Cached: MinGW PE built, `Windows` reachable, runner deployed + HELLO.
[[nodiscard]] bool windows_ready(std::string& why);

/// Time the prepared body on native Windows. Requires windows_ready().
[[nodiscard]] bool bench_on_windows(const std::vector<uint8_t>& body,
                                    const State& in, uint32_t warmup,
                                    uint32_t iters, BenchTimes& out,
                                    std::string& error);

/// Cached readiness for the PE32+ runner. Its HELLO architecture tag prevents
/// a stale PE32 deployment from being accepted as native x64 evidence.
[[nodiscard]] bool windows64_ready(std::string& why);

/// Time a fall-through long-mode body on native x64 Windows.
[[nodiscard]] bool bench_on_windows64(const std::vector<uint8_t>& body,
                                      const State& in, uint32_t warmup,
                                      uint32_t iters, BenchTimes& out,
                                      std::string& error);

/// Transport/protocol success is independent of guest status: an owned guest
/// fault returns true with result.times.status == CPU_PERF_WIN_FAULT.
[[nodiscard]] bool bench_on_windows64_observed(
    const std::vector<uint8_t>& body, const State& in, uint32_t warmup,
    uint32_t iters, Windows64BenchResult& result, std::string& error);

/// Compute signed per-iteration cost. Negative net values are preserved as
/// timing-noise evidence rather than clamped to zero.
[[nodiscard]] bool normalize_bench_times(const BenchTimes& raw,
                                         const BenchTimes& wrapper,
                                         NormalizedNs& result);

/// Measure a body and a zero-byte wrapper control at identical iteration
/// counts, scaling both together above the QPC resolution floor.
[[nodiscard]] bool bench_on_windows64_normalized(
    const std::vector<uint8_t>& body, const State& in, uint32_t warmup,
    uint32_t iters, Windows64NormalizedBenchResult& result,
    std::string& error);

/// Compact long-mode integer/memory/branch catalog. This is deliberately
/// separate from the established 32-bit Report and its JSON schema.
[[nodiscard]] std::vector<X64CatalogEntry> cpu_perf_x64_catalog();

/// Measure a long-mode body and an empty JIT wrapper control at identical
/// iteration counts, scaling the pair together above the timer floor.
[[nodiscard]] bool bench_x64_jit_normalized(
    const std::vector<uint8_t>& body, const State& in, uint32_t warmup,
    uint32_t iters, X64JitNormalizedResult& result, std::string& error,
    bool multiblock = false);

[[nodiscard]] X64CatalogReport bench_x64_catalog(uint32_t warmup,
                                                 uint32_t iters,
                                                 bool with_windows = false);

/// Collect independent paired trials. Exact timer counters remain attached to
/// each trial; row.median contains distribution summaries and valid ratios.
[[nodiscard]] X64CatalogReport bench_x64_catalog_trials(
    uint32_t warmup, uint32_t iters, uint32_t trial_count,
    bool with_windows = false);

/// Recompute row.median from row.trials. Exposed for deterministic reporting
/// tests and offline consumers that populate exact trial observations.
void summarize_x64_trials(X64CatalogRow& row);

/// Write the long-mode-only report. This intentionally does not extend or
/// reinterpret the legacy vitreo.cpu_perf.v1 schema.
[[nodiscard]] bool write_x64_report_json(const X64CatalogReport& report,
                                         const std::string& path,
                                         std::string& error);

/// Fill row.windows when the VM is up. Leaves ros/jit results alone.
void attach_windows(SnippetBench& row, uint32_t warmup, uint32_t iters);

/// Same, but with an explicit input State (finite floats, pointer rings,
/// stack pointers) instead of the default scratch_state().
void attach_windows(SnippetBench& row, uint32_t warmup, uint32_t iters,
                    const State& in);

}  // namespace vitreo::oracle::perf
