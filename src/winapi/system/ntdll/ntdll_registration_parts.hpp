// SPDX-License-Identifier: LGPL-2.1-only

/// @file ntdll_registration_parts.hpp
/// @brief Private declarations for the ordered parts of the extended ntdll
///        export table, plus the two helpers more than one part shares.
///
/// make_ntdll_ext_exports (ntdll_registration.cpp) is the only caller. Each
/// part appends one contiguous slice of the table to `exports`, and the
/// slices must be appended in the order listed here: registration order is
/// guest-visible. Ordinals come from the running `ordinal++` counter (which
/// the parts thread through by reference, some entries registering ordinal
/// 0), ModuleManager derives the service veneers from entry order, and
/// tools/architecture/check_export_inventory.sh pins every name, ordinal
/// and stdcall byte count.
///
/// These functions are private to ntdll_registration*.cpp; nothing else may
/// call them.

#pragma once

#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/system/ntdll/ntdll.hpp"
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/system/ntdll/ntdll_internal.hpp"

#include <atomic>
#include <cstdio>
#include <memory>
#include <optional>

namespace vitreo::winapi {

// =============================================================================
// Ordered table parts
// =============================================================================

void append_context_memory_exports(NtdllExtManager& mgr,
                                   DllExports& exports, u16& ordinal);
void append_loader_function_table_exports(NtdllExtManager& mgr,
                                          DllExports& exports, u16& ordinal);
void append_object_query_exports(NtdllExtManager& mgr,
                                 DllExports& exports, u16& ordinal);
void append_thread_wait_exports(NtdllExtManager& mgr,
                                DllExports& exports, u16& ordinal);
void append_rtl_memory_compression_exports(NtdllExtManager& mgr,
                                           DllExports& exports, u16& ordinal);
void append_file_exports(NtdllExtManager& mgr,
                         DllExports& exports, u16& ordinal);
void append_rtl_unwind_version_exports(NtdllExtManager& mgr,
                                       DllExports& exports, u16& ordinal);
void append_native_services_exports(NtdllExtManager& mgr,
                                    DllExports& exports, u16& ordinal);
void append_thread_control_exports(NtdllExtManager& mgr,
                                   DllExports& exports, u16& ordinal);
void append_key_object_exports(NtdllExtManager& mgr,
                               DllExports& exports, u16& ordinal);
void append_debug_slist_thread_start_exports(NtdllExtManager& mgr,
                                             DllExports& exports, u16& ordinal);

// =============================================================================
// Helpers shared by more than one part
// =============================================================================

// Native information class measured by thread_hide_probe.c on Windows x86/x64.
inline constexpr u32 kThreadHideFromDebugger = 17;

inline void thread_hide_information(Marshaler& m, NtdllExtManager& mgr, bool setting) {
    const u32 length = static_cast<u32>(m.arg(3));
    if (length != (setting ? 0u : 1u)) {
        m.set_return(nt_status::STATUS_INFO_LENGTH_MISMATCH);
        return;
    }
    const Handle handle = static_cast<Handle>(m.arg(0));
    auto* threads = mgr.thread_manager();
    auto* handles = mgr.handle_table();
    if (handles && handle != ThreadManager::get_current_thread()) {
        const auto type = handles->get_type(handle);
        if (type != HandleType::Invalid && type != HandleType::Thread) {
            m.set_return(nt_status::STATUS_OBJECT_TYPE_MISMATCH);
            return;
        }
    }
    bool hidden = false;
    const u32 error = threads == nullptr ? error_codes::ERROR_INVALID_HANDLE
        : setting ? threads->set_thread_hidden_from_debugger(handle)
                  : threads->get_thread_hidden_from_debugger(handle, hidden);
    if (error != 0) {
        m.set_return(error == error_codes::ERROR_ACCESS_DENIED
            ? nt_status::STATUS_ACCESS_DENIED : nt_status::STATUS_INVALID_HANDLE);
        return;
    }
    if (!setting) {
        const GuestAddr output = m.arg_ptr(2);
        const GuestAddr returned = m.arg_ptr(4);
        // Windows leaves both outputs untouched if either cannot be written.
        if (!m.memory().is_range_writable(output, 1, m.cpu_is_64bit()) ||
            (returned != 0 && !m.memory().is_range_writable(
                returned, sizeof(u32), m.cpu_is_64bit()))) {
            m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
            return;
        }
        (void)m.memory().write_u8(output, hidden ? 1 : 0);
        if (returned != 0) (void)m.memory().write_u32(returned, 1);
    }
    m.set_return(nt_status::STATUS_SUCCESS);
}

// A Windows sandbox resolves and hot-patches this native-service
// surface before a suspended target runs.  Publishing the real Windows
// export names is distinct from claiming the operations work: unsupported
// calls fail honestly with STATUS_NOT_IMPLEMENTED and clear an output
// handle where the NT contract defines one.  Their service-shaped x64 EAT
// veneers are materialized by ModuleManager just like implemented Nt*
// exports, which lets the broker validate and install its interceptions.
inline void add_unsupported_service(
    DllExports& exports, u16& ordinal, const char* name, u16 stdcall_bytes,
    std::optional<usize> output_handle_arg = std::nullopt) {
    // An unimplemented call names itself once, then fails.
    auto reported = std::make_shared<std::atomic<bool>>(false);
    exports.entries.push_back(ExportEntry{
        name, ordinal++,
        [name, output_handle_arg, reported](Marshaler& m) {
            if (!reported->exchange(true)) {
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED ntdll!%s -> "
                             "STATUS_NOT_IMPLEMENTED\n",
                             name);
            }
            if (output_handle_arg.has_value()) {
                const GuestAddr output =
                    m.arg_ptr(*output_handle_arg);
                if (output != 0) {
                    if (m.cpu_is_64bit()) {
                        (void)m.memory().write_u64(output, 0);
                    } else {
                        (void)m.memory().write_u32(output, 0);
                    }
                }
            }
            VITREO_TRACE(
                "[ntdll] %s unsupported -> "
                "STATUS_NOT_IMPLEMENTED\n",
                name);
            m.set_return(nt_status::STATUS_NOT_IMPLEMENTED);
        },
        stdcall_bytes
    });
}

} // namespace vitreo::winapi
