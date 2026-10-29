// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file vectored_exception.hpp
/// @brief Process-wide vectored exception and vectored continue handler lists.
///
/// Learn: AddVectoredExceptionHandler / RemoveVectoredExceptionHandler /
/// AddVectoredContinueHandler / RemoveVectoredContinueHandler /
/// PVECTORED_EXCEPTION_HANDLER. First != 0 inserts at the front; First == 0
/// appends. Dispatch (runtime) snapshots the exception list and calls each
/// handler with PEXCEPTION_POINTERS before the frame walk.
///
/// tools/oracle_probe/vectored_continue_probe (x64 and x86) measures the
/// rest: the two lists are separate (a handle from one is refused by the
/// other's Remove), a NULL handler is registered like any other, Remove(NULL)
/// fails, and the continue list runs whenever dispatch resumes execution --
/// after a vectored handler, a frame-based handler or the top-level filter
/// returns "continue execution" -- in list order on the same CONTEXT, until
/// one returns EXCEPTION_CONTINUE_EXECUTION.

#include <vitreo/common.hpp>
#include <vector>

namespace vitreo::winapi {

/// Register @p handler (NULL included). Returns a non-zero handle.
[[nodiscard]] u64 add_vectored_exception_handler(u32 first, u64 handler) noexcept;

/// Unregister by handle. Returns true if a matching registration was removed.
[[nodiscard]] bool remove_vectored_exception_handler(u64 handle) noexcept;

/// Snapshot handler VAs in call order (front of list is called first).
[[nodiscard]] std::vector<u64> snapshot_vectored_exception_handlers();

/// The continue-handler list: same rules, its own registrations.
[[nodiscard]] u64 add_vectored_continue_handler(u32 first, u64 handler) noexcept;
[[nodiscard]] bool remove_vectored_continue_handler(u64 handle) noexcept;
[[nodiscard]] std::vector<u64> snapshot_vectored_continue_handlers();

/// Test isolation only — not a Windows API.
void clear_vectored_exception_handlers_for_test() noexcept;

} // namespace vitreo::winapi
