#pragma once

/// @file kernel32_in_chain.hpp
/// @brief The in-chain contract class (winapi::InChainClass) of the hot
/// kernel32 exports (L3-K; winapi/core/host_in_chain.hpp).
///
/// The class is a property of the export's DOCUMENTED contract, keyed by its
/// name, never by what a run observed. The invariant is L3's: the export
/// returns to its caller and runs no guest code; if it leaves otherwise, it
/// leaves only through the crossing's structural exits (a raise, an unwind or
/// a control transfer each breaks to the outer dispatcher, as the full path
/// does), and process exit is observed after every call (has_exited, as the
/// dispatcher tests it). Blocking is allowed -- the in-chain crossing parks
/// the chain's arena lease and publishes the host call exactly as the
/// dispatcher does -- so the critical-section and SRW acquires qualify:
/// EnterCriticalSection's documented EXCEPTION_POSSIBLE_DEADLOCK is such a
/// raise, and a wait cut short by ExitProcess (no ownership) is such an
/// exit. Not named here, and
/// never in-chain: anything that can run guest code (SleepEx and the other
/// alertable waits deliver APCs), anything that may raise by contract
/// (HeapAlloc with HEAP_GENERATE_EXCEPTIONS), and every export that ends the
/// thread or process (ends_thread_or_process, enforced by register_dll).
///
/// The rules apply to kernel32.dll (and the kernelbase.dll copies collected
/// from it); dll_registry.cpp calls apply_in_chain_classes on kernel32's
/// assembled exports before either is registered.

#include "winapi/core/thunk_registry.hpp"

#include <span>
#include <string_view>

namespace vitreo::winapi::kernel32 {

/// One export's class and the contract it rests on.
struct InChainRule {
    std::string_view name;
    InChainClass cls;
    /// The documented behaviour the class rests on, for the inventory.
    std::string_view reason;
};

/// Every rule, in table order.
[[nodiscard]] std::span<const InChainRule> in_chain_rules() noexcept;

/// The rule naming @p name, or null -- an export no rule names keeps its
/// own class (None unless it has a direct entry).
[[nodiscard]] const InChainRule* in_chain_rule(std::string_view name) noexcept;

/// Set the class of each export of @p exports a rule names. Exports with a
/// direct entry keep theirs.
void apply_in_chain_classes(DllExports& exports) noexcept;

}  // namespace vitreo::winapi::kernel32
