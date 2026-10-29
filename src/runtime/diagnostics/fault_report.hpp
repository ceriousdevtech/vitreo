#pragma once
/// @file fault_report.hpp
/// @brief Human-readable reports for guest faults.
///
/// When a guest faults, the raw facts — an exception code and a RIP — are
/// rarely enough to act on. These turn that into something a person can read:
/// which module and RVA the fault is in, what the registers held, what the
/// stack looked like, and whether the address is even plausible code.
///
/// Reporting only. Nothing here changes guest state or decides whether a fault
/// is handled; that is the exception dispatchers' job. A report may be
/// produced for a fault that is subsequently handled cleanly.

#include <vitreo/common.hpp>

namespace vitreo {
class GuestMemory;
namespace cpu {
struct CpuState;
// Underlying type must match the definition in cpu/interpreter/interpreter.hpp;
// a forward declaration that omits it is a different type to the compiler.
enum class CpuException : u8;
} // namespace cpu
namespace winapi {
class ThunkRegistry;
class ModuleManager;
} // namespace winapi
} // namespace vitreo

namespace vitreo::cli {

/// Dump registers and stack around a control-flow fault — a jump or call to an
/// address that cannot be code. Gated by VITREO_CONTROL_FLOW_DIAG.
///
/// These faults are usually the *second* symptom: something earlier corrupted
/// a return address or vtable pointer, and this is where it finally lands.
void dump_control_flow_fault_context(const cpu::CpuState& cpu, GuestMemory& memory);

/// Summarise a guest fault: module and RVA, exception, registers, and nearby
/// stack, resolving addresses through the module manager where possible.
///
/// @param module_manager May be null during early bootstrap, in which case
///        addresses are reported raw rather than as module+RVA.
void dump_guest_fault_summary(const cpu::CpuState& cpu, GuestMemory& memory,
                              const winapi::ThunkRegistry& thunk_registry,
                              const winapi::ModuleManager* module_manager,
                              cpu::CpuException exception);

/// Periodically record where a worker thread is executing.
///
/// A thread that appears hung is the case this exists for: sampling its PC
/// over time distinguishes a spin from a genuine block. Sampling interval
/// comes from VITREO_THREAD_SAMPLE_EVERY; zero disables it.
void maybe_sample_worker_pc(const cpu::CpuState& cpu, GuestAddr callback_entry,
                            const winapi::ThunkRegistry& thunk_registry);

} // namespace vitreo::cli
