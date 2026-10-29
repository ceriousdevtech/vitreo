// SPDX-License-Identifier: LGPL-2.1-only

/// @file kernel32_ext_registration_parts.hpp
/// @brief Private declarations for the ordered parts of the extended kernel32
///        export table.
///
/// create_kernel32_ext_thunks (kernel32_ext_registration.cpp) is the only
/// caller. Each part appends one contiguous slice of the table to `exports`,
/// and the slices must be appended in the order listed here: registration
/// order is guest-visible. A name registered twice resolves last-wins
/// (GlobalAddAtomW, GlobalFindAtomW, GlobalDeleteAtom and GlobalGetAtomNameW
/// are registered in both the process-misc and atom-comm parts), and thunk
/// addresses are assigned in registration order.
///
/// These functions are private to kernel32_ext_registration*.cpp; nothing else
/// may call them.

#pragma once

#include "winapi/kernel32/kernel32_ext.hpp"

namespace vitreo::winapi {

void append_unwind_path_exports(Kernel32ExtManager& mgr, DllExports& exports);
void append_crt_thread_exports(Kernel32ExtManager& mgr, DllExports& exports);
void append_handle_resource_exports(Kernel32ExtManager& mgr, DllExports& exports);
void append_topology_legacy_exports(Kernel32ExtManager& mgr, DllExports& exports);
void append_process_misc_exports(Kernel32ExtManager& mgr, DllExports& exports);
void append_additional_win32_exports(Kernel32ExtManager& mgr, DllExports& exports);
void append_wave_forwarder_exports(Kernel32ExtManager& mgr, DllExports& exports);
void append_cef_wer_exports(Kernel32ExtManager& mgr, DllExports& exports);
void append_atom_comm_exports(Kernel32ExtManager& mgr, DllExports& exports);

} // namespace vitreo::winapi
