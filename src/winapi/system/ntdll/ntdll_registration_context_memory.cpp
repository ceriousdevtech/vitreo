// SPDX-License-Identifier: LGPL-2.1-only

/// @file ntdll_registration_context_memory.cpp
/// @brief Extended ntdll export table, part 1 of 11: NtContinue, the
///        Nt*VirtualMemory trio, LdrInitializeThunk and the thread-context
///        calls.
///
/// A contiguous slice of the table that make_ntdll_ext_exports builds
/// (see ntdll_registration.cpp). The slice boundaries follow the table's
/// registration order, which is load-bearing: ordinals come from the
/// running `ordinal++` counter and ModuleManager derives the service
/// veneers from entry order. Do not reorder entries or call the parts out
/// of order.

#include "winapi/system/ntdll/ntdll_registration_parts.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/thread/context64.hpp"
#include "winapi/kernel32/thread/thread_context.hpp"
#include "core/cpu/seh/seh32.hpp"
#include <limits>
#include <optional>

namespace vitreo::winapi {

using namespace ntdll;

namespace {

[[nodiscard]] u32 restore_context_and_transfer(Marshaler& m,
                                               GuestAddr context) {
    auto* cpu = m.cpu();
    if (context == 0 || cpu == nullptr) {
        return nt_status::STATUS_ACCESS_VIOLATION;
    }

    const bool restored = m.cpu_is_64bit()
        ? restore_context64(m, context)
        : cpu::read_context_32(
              m.memory(), static_cast<u32>(context), *cpu).is_ok();
    if (!restored) {
        return nt_status::STATUS_ACCESS_VIOLATION;
    }

    // Both NtContinue and LdrInitializeThunk are non-returning on success.
    // Suppress the thunk dispatcher RET so it cannot replace the restored
    // instruction and stack pointers.
    cpu->thunk_control_transfer = true;
    return nt_status::STATUS_SUCCESS;
}

} // namespace

void append_context_memory_exports(NtdllExtManager& mgr,
                                   DllExports& exports, u16& ordinal) {
    // NtContinue restores the supplied guest CONTEXT and does not return on
    // success. Microsoft documents the equivalent restore semantics through
    // RtlRestoreContext. TestAlert only has an additional effect when an APC
    // is pending; unsupported pending delivery fails honestly below.
    exports.entries.push_back(ExportEntry{
        "NtContinue", ordinal++,
        [&mgr](Marshaler& m) {
            const GuestAddr context = m.arg_ptr(0);
            const bool test_alert = m.arg(1) != 0;

            if (test_alert && mgr.thread_manager() != nullptr) {
                const u32 tid = ThreadManager::get_current_thread_id_static();
                if (mgr.thread_manager()->has_pending_user_apcs(tid) ||
                    mgr.thread_manager()->has_pending_io_completion_apcs(tid) ||
                    mgr.thread_manager()->has_pending_nt_io_apcs(tid)) {
                    m.set_return(nt_status::STATUS_NOT_IMPLEMENTED);
                    return;
                }
            }

            const u32 status = restore_context_and_transfer(m, context);
            if (status != nt_status::STATUS_SUCCESS) m.set_return(status);
        },
        8
    });

    // Microsoft Learn: NtFreeVirtualMemory takes pointers to the base and
    // region size, updates them to page-rounded values, and accepts exactly
    // MEM_RELEASE or MEM_DECOMMIT for the current process address space.
    exports.entries.push_back(ExportEntry{
        "NtFreeVirtualMemory", ordinal++,
        [&mgr](Marshaler& m) {
            constexpr u64 current_process64 = ~u64{0};
            constexpr u64 current_process32 = 0xFFFFFFFFu;
            const u64 process = m.arg(0);
            const GuestAddr base_ptr = m.arg_ptr(1);
            const GuestAddr size_ptr = m.arg_ptr(2);
            const u32 free_type = static_cast<u32>(m.arg(3));
            auto* vmm = mgr.virtual_memory_manager();

            if ((process != current_process64 && process != current_process32) ||
                vmm == nullptr) {
                m.set_return(nt_status::STATUS_INVALID_HANDLE);
                return;
            }
            if (base_ptr == 0 || size_ptr == 0 ||
                (free_type != MEM_RELEASE && free_type != MEM_DECOMMIT)) {
                m.set_return(nt_status::STATUS_INVALID_PARAMETER);
                return;
            }

            const auto read_size_t = [&](GuestAddr address) -> std::optional<u64> {
                if (m.cpu_is_64bit()) {
                    const auto value = m.read_u64(address);
                    if (!value.is_ok()) return std::nullopt;
                    return value.value();
                }
                const auto value = m.read_u32(address);
                if (!value.is_ok()) return std::nullopt;
                return value.value();
            };
            const auto base_value = read_size_t(base_ptr);
            const auto size_value = read_size_t(size_ptr);
            if (!base_value.has_value() || !size_value.has_value() ||
                base_value.value() == 0) {
                m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
                return;
            }

            u64 base = base_value.value();
            u64 size = size_value.value();
            if (free_type == MEM_RELEASE && size != 0) {
                m.set_return(nt_status::STATUS_INVALID_PARAMETER);
                return;
            }

            constexpr u64 page_size = 0x1000;
            if (free_type == MEM_DECOMMIT) {
                VirtualAllocationQuery query{};
                if (!vmm->virtual_query(base, query)) {
                    m.set_return(nt_status::STATUS_INVALID_PARAMETER);
                    return;
                }
                if (size == 0) {
                    if (base != query.allocation_base) {
                        m.set_return(nt_status::STATUS_INVALID_PARAMETER);
                        return;
                    }
                    u64 cursor = query.allocation_base;
                    size = 0;
                    while (vmm->virtual_query(cursor, query) &&
                           query.allocation_base == base &&
                           query.region_size != 0) {
                        size += query.region_size;
                        cursor += query.region_size;
                    }
                } else {
                    if (base > std::numeric_limits<u64>::max() - size) {
                        m.set_return(nt_status::STATUS_INVALID_PARAMETER);
                        return;
                    }
                    const u64 end = (base + size + page_size - 1) & ~(page_size - 1);
                    base &= ~(page_size - 1);
                    size = end - base;
                }
            }

            if (!vmm->virtual_free(base, size, free_type)) {
                m.set_return(nt_status::STATUS_INVALID_PARAMETER);
                return;
            }
            const bool wrote = m.cpu_is_64bit()
                ? (m.write_u64(base_ptr, base).is_ok() &&
                   m.write_u64(size_ptr, size).is_ok())
                : (m.write_u32(base_ptr, static_cast<u32>(base)).is_ok() &&
                   m.write_u32(size_ptr, static_cast<u32>(size)).is_ok());
            m.set_return(wrote ? nt_status::STATUS_SUCCESS
                               : nt_status::STATUS_ACCESS_VIOLATION);
        },
        16
    });

    // Microsoft Learn: NtAllocateVirtualMemory reserves and/or commits pages
    // in the current process and writes the selected base plus page-rounded
    // region size back through pointer-sized in/out parameters.
    exports.entries.push_back(ExportEntry{
        "NtAllocateVirtualMemory", ordinal++,
        [&mgr](Marshaler& m) {
            constexpr u64 current_process64 = ~u64{0};
            constexpr u64 current_process32 = 0xFFFFFFFFu;
            const u64 process = m.arg(0);
            const GuestAddr base_ptr = m.arg_ptr(1);
            const u64 zero_bits = m.arg(2);
            const GuestAddr size_ptr = m.arg_ptr(3);
            const u32 allocation_type = static_cast<u32>(m.arg(4));
            const u32 protection = static_cast<u32>(m.arg(5));
            auto* vmm = mgr.virtual_memory_manager();

            if ((process != current_process64 && process != current_process32) ||
                vmm == nullptr) {
                m.set_return(nt_status::STATUS_INVALID_HANDLE);
                return;
            }
            if (base_ptr == 0 || size_ptr == 0) {
                m.set_return(nt_status::STATUS_INVALID_PARAMETER);
                return;
            }
            if (zero_bits != 0) {
                m.set_return(nt_status::STATUS_NOT_IMPLEMENTED);
                return;
            }

            const auto read_size_t = [&](GuestAddr address) -> std::optional<u64> {
                if (m.cpu_is_64bit()) {
                    const auto value = m.read_u64(address);
                    if (!value.is_ok()) return std::nullopt;
                    return value.value();
                }
                const auto value = m.read_u32(address);
                if (!value.is_ok()) return std::nullopt;
                return value.value();
            };
            const auto base_value = read_size_t(base_ptr);
            const auto size_value = read_size_t(size_ptr);
            if (!base_value.has_value() || !size_value.has_value()) {
                m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
                return;
            }
            if (size_value.value() == 0 ||
                size_value.value() >
                    std::numeric_limits<u64>::max() - (0x1000u - 1u) ||
                (allocation_type & (MEM_COMMIT | MEM_RESERVE)) == 0 ||
                (allocation_type & ~(MEM_COMMIT | MEM_RESERVE | MEM_WRITE_WATCH)) != 0 ||
                !is_supported_win32_protection(protection)) {
                m.set_return(nt_status::STATUS_INVALID_PARAMETER);
                return;
            }

            const u64 allocated = vmm->virtual_alloc(
                base_value.value(), size_value.value(),
                allocation_type, protection);
            if (allocated == 0) {
                // STATUS_COMMITMENT_LIMIT for the host memory floor (the
                // Win32 twin is ERROR_COMMITMENT_LIMIT); anything else keeps
                // STATUS_NO_MEMORY.
                m.set_return(vmm->take_last_alloc_error() ==
                                     error_codes::ERROR_COMMITMENT_LIMIT
                                 ? 0xC000012Du
                                 : 0xC0000017u);
                return;
            }
            constexpr u64 page_size = 0x1000;
            const u64 rounded_size =
                (size_value.value() + page_size - 1) & ~(page_size - 1);
            const bool wrote = m.cpu_is_64bit()
                ? (m.write_u64(base_ptr, allocated).is_ok() &&
                   m.write_u64(size_ptr, rounded_size).is_ok())
                : (m.write_u32(base_ptr, static_cast<u32>(allocated)).is_ok() &&
                   m.write_u32(size_ptr, static_cast<u32>(rounded_size)).is_ok());
            if (!wrote) {
                (void)vmm->virtual_free(allocated, 0, MEM_RELEASE);
                m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
                return;
            }
            m.set_return(nt_status::STATUS_SUCCESS);
        },
        24
    });

    // NtProtectVirtualMemory changes protection in the current process and
    // returns the page-aligned range actually affected through its pointer-
    // sized in/out arguments. This is the native contract underlying
    // VirtualProtect; unlike the Win32 wrapper it returns NTSTATUS directly.
    exports.entries.push_back(ExportEntry{
        "NtProtectVirtualMemory", ordinal++,
        [&mgr](Marshaler& m) {
            constexpr u64 current_process64 = ~u64{0};
            constexpr u64 current_process32 = 0xFFFFFFFFu;
            constexpr u64 page_size = 0x1000;
            const u64 process = m.arg(0);
            const GuestAddr base_ptr = m.arg_ptr(1);
            const GuestAddr size_ptr = m.arg_ptr(2);
            const u32 new_protection = static_cast<u32>(m.arg(3));
            const GuestAddr old_protection_ptr = m.arg_ptr(4);
            auto* vmm = mgr.virtual_memory_manager();

            if ((process != current_process64 && process != current_process32) ||
                vmm == nullptr) {
                m.set_return(nt_status::STATUS_INVALID_HANDLE);
                return;
            }
            if (base_ptr == 0 || size_ptr == 0 || old_protection_ptr == 0) {
                m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
                return;
            }
            if (!is_supported_win32_protection(new_protection)) {
                m.set_return(nt_status::STATUS_INVALID_PAGE_PROTECTION);
                return;
            }

            const auto read_size_t = [&](GuestAddr address) -> std::optional<u64> {
                if (m.cpu_is_64bit()) {
                    const auto value = m.read_u64(address);
                    if (!value.is_ok()) return std::nullopt;
                    return value.value();
                }
                const auto value = m.read_u32(address);
                if (!value.is_ok()) return std::nullopt;
                return value.value();
            };
            const auto requested_base = read_size_t(base_ptr);
            const auto requested_size = read_size_t(size_ptr);
            if (!requested_base.has_value() || !requested_size.has_value()) {
                m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
                return;
            }
            if (requested_size.value() == 0 ||
                requested_base.value() >
                    std::numeric_limits<u64>::max() - requested_size.value() ||
                requested_base.value() + requested_size.value() >
                    std::numeric_limits<u64>::max() - (page_size - 1)) {
                m.set_return(nt_status::STATUS_INVALID_PARAMETER);
                return;
            }

            const u64 aligned_base = requested_base.value() & ~(page_size - 1);
            const u64 aligned_end =
                (requested_base.value() + requested_size.value() +
                 page_size - 1) & ~(page_size - 1);
            const u64 aligned_size = aligned_end - aligned_base;
            u32 old_protection = 0;
            if (!vmm->virtual_protect(aligned_base, aligned_size,
                                      new_protection, &old_protection)) {
                m.set_return(nt_status::STATUS_INVALID_PARAMETER);
                return;
            }

            const bool wrote_range = m.cpu_is_64bit()
                ? (m.write_u64(base_ptr, aligned_base).is_ok() &&
                   m.write_u64(size_ptr, aligned_size).is_ok())
                : (m.write_u32(base_ptr, static_cast<u32>(aligned_base)).is_ok() &&
                   m.write_u32(size_ptr, static_cast<u32>(aligned_size)).is_ok());
            if (!wrote_range ||
                !m.write_u32(old_protection_ptr, old_protection).is_ok()) {
                m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
                return;
            }
            m.set_return(nt_status::STATUS_SUCCESS);
        },
        20
    });

    // New Windows user threads enter ntdll through LdrInitializeThunk with
    // the requested CONTEXT. Vitreo performs the loader-owned TEB, stack and
    // TLS setup before launching each guest worker; this image entry completes
    // the same non-returning transition to that supplied context.
    exports.entries.push_back(ExportEntry{
        "LdrInitializeThunk", ordinal++,
        [](Marshaler& m) {
            const u32 status = restore_context_and_transfer(m, m.arg_ptr(0));
            if (status != nt_status::STATUS_SUCCESS) m.set_return(status);
        },
        8
    });

    // Native frontend for the same context-setting contract exposed by
    // SetThreadContext. The target state is staged through ThreadSuspendRegistry
    // so a suspended worker observes it atomically when resumed.
    exports.entries.push_back(ExportEntry{
        "NtSetContextThread", ordinal++,
        [&mgr](Marshaler& m) {
            m.set_return(nt_set_context_thread(m, mgr.thread_manager(), m.arg(0),
                                               m.arg_ptr(1)));
        },
        8
    });

    // Native frontend for GetThreadContext. ContextFlags is an input mask:
    // only the selected AMD64 CONTEXT groups are written, and the target is
    // sampled from its parked snapshot (or its pre-launch seed state).
    exports.entries.push_back(ExportEntry{
        "NtGetContextThread", ordinal++,
        [&mgr](Marshaler& m) {
            m.set_return(nt_get_context_thread(m, mgr.thread_manager(), m.arg(0),
                                               m.arg_ptr(1)));
        },
        8
    });
}

} // namespace vitreo::winapi
