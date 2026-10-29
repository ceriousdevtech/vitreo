#pragma once
/// @file pc_trace.hpp
/// @brief VITREO_PCTRACE — focused per-instruction guest tracing.
///
/// A divergence-hunting tool. When a guest function returns a different value
/// under Vitreo than under Windows, this records every instruction inside a
/// chosen RIP range so the two streams can be diffed down to the first
/// difference.
///
/// Entirely opt-in: with `VITREO_PCTRACE` unset, `pctrace_active()` is false
/// and every call here costs a predictable branch. That is what makes it
/// separable from the loop it instruments — it observes execution rather than
/// participating in it.
///
/// Private to the runtime. Nothing outside the execution loop should trace
/// individual guest instructions.

#include <vitreo/common.hpp>

#include <cstdio>
#include <vector>

namespace vitreo {
class GuestMemory;
namespace cpu { struct CpuState; }
namespace winapi { class ModuleManager; }
} // namespace vitreo

namespace vitreo::cli {

/// Attach the loaded-module registry used to resolve module-relative trace
/// ranges. The execution runtime calls this alongside its own registry wiring.
void pctrace_set_module_manager(winapi::ModuleManager* modules) noexcept;

/// Whether tracing is configured at all.
///
/// The execution loop consults this to decide whether JIT block linking may be
/// enabled: a linked block does not return to the dispatcher, so the
/// per-instruction hook would never run.
[[nodiscard]] bool pctrace_enabled();

/// Stream trace records are written to. Never null when tracing is enabled.
[[nodiscard]] std::FILE* pctrace_output();

/// True when @p rip falls inside a configured trace range.
[[nodiscard]] bool pctrace_active(u64 rip);

/// True when [start, start+length) intersects a configured trace range.
/// Used to decide whether a JIT block may be compiled or must be single
/// stepped, since a compiled block emits no per-instruction record.
[[nodiscard]] bool pctrace_overlaps(u64 start, u64 length);

/// Record one instruction's guest state into the trace ring.
void pctrace_capture(cpu::CpuState& cpu, GuestMemory& memory);

/// Addresses armed by VITREO_PC_WATCH, in guest RIP order.
[[nodiscard]] const std::vector<u64>& pc_watch_addrs();

/// True when @p rip is an armed watch address.
[[nodiscard]] bool pc_watch_hit(u64 rip);

/// True when [start, start+length) contains an armed watch address.
[[nodiscard]] bool pc_watch_overlaps(u64 start, u64 length);

} // namespace vitreo::cli

/// Dump the trace ring under @p tag. Extern "C" because the CPU layer installs
/// it as a hook without linking the runtime that owns the ring.
extern "C" void vitreo_pctrace_dump_ring(const char* tag);
