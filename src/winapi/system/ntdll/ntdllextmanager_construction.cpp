// SPDX-License-Identifier: LGPL-2.1-only

/// @file ntdll_ext.cpp
/// @brief ntdll.dll extended function stubs (WIN32-086)

#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/system/ntdll/ntdll_internal.hpp"
#include "winapi/system/ntdll/ntdll.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/common/write_sha1_debug.hpp"
#include "capsule/vfs/filesystem.hpp"

#include <algorithm>
#include <cwctype>
#include <cstring>
#include <pthread.h>
#include <vector>
#include <unistd.h>
#include <sys/time.h>

/// @file ntdllextmanager_construction.cpp
/// @brief NtdllExtManager Construction
///
/// Closed cut from ntdll_ext.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// NtdllExtManager Construction
// =============================================================================

NtdllExtManager::NtdllExtManager() = default;

u32 NtdllExtManager::register_dll_notification(
    u32 flags, GuestAddr callback, GuestAddr context, GuestAddr* cookie) {
    return m_module_manager
        ? m_module_manager->register_dll_notification(
              flags, callback, context, cookie)
        : nt_status::STATUS_NOT_IMPLEMENTED;
}

u32 NtdllExtManager::unregister_dll_notification(GuestAddr cookie) {
    return m_module_manager
        ? m_module_manager->unregister_dll_notification(cookie)
        : nt_status::STATUS_NOT_IMPLEMENTED;
}

void NtdllExtManager::impl_LdrLockLoaderLock(Marshaler& m) {
    // NTSTATUS LdrLockLoaderLock(ULONG Flags, ULONG* State, ULONG_PTR* Cookie)
    // Public contract (Geoff Chappell / ntdll LDR API): bits 0x1 raise-on-error,
    // 0x2 try-only. Cookie is pointer-sized on modern Windows.
    constexpr u32 kFlagRaise = 0x1u;
    constexpr u32 kFlagTry = 0x2u;
    constexpr u32 kDispAcquired = 1u;
    constexpr u32 kDispNotAcquired = 2u;

    const u32 flags = static_cast<u32>(m.arg(0));
    const GuestAddr state_ptr = m.arg_ptr(1);
    const GuestAddr cookie_ptr = m.arg_ptr(2);

    if ((flags & ~(kFlagRaise | kFlagTry)) != 0) {
        m.set_return(nt_status::STATUS_INVALID_PARAMETER_1);
        return;
    }
    if ((flags & kFlagTry) != 0 && state_ptr == 0) {
        m.set_return(nt_status::STATUS_INVALID_PARAMETER_2);
        return;
    }
    if (cookie_ptr == 0) {
        m.set_return(nt_status::STATUS_INVALID_PARAMETER_3);
        return;
    }

    if ((flags & kFlagTry) != 0) {
        if (!m_loader_lock.try_lock()) {
            (void)m.write_u32(state_ptr, kDispNotAcquired);
            m.set_return(nt_status::STATUS_SUCCESS);
            return;
        }
    } else {
        m_loader_lock.lock();
    }
    ++m_loader_lock_depth;

    if (state_ptr != 0) {
        (void)m.write_u32(state_ptr, kDispAcquired);
    }
    const u32 tid12 =
        static_cast<u32>(reinterpret_cast<uintptr_t>(pthread_self()) & 0xFFFu);
    u32 serial = m_loader_lock_serial++;
    if (serial == 0) serial = m_loader_lock_serial++;
    const u64 cookie =
        (static_cast<u64>(tid12) << 16) | static_cast<u64>(serial & 0xFFFFu);
    (void)m.write_ptr(cookie_ptr, static_cast<GuestAddr>(cookie));
    m.set_return(nt_status::STATUS_SUCCESS);
}

void NtdllExtManager::impl_LdrUnlockLoaderLock(Marshaler& m) {
    // NTSTATUS LdrUnlockLoaderLock(ULONG Flags, ULONG_PTR Cookie)
    constexpr u32 kFlagRaise = 0x1u;
    const u32 flags = static_cast<u32>(m.arg(0));
    const u64 cookie = m.arg(1);
    if ((flags & ~kFlagRaise) != 0) {
        m.set_return(nt_status::STATUS_INVALID_PARAMETER_1);
        return;
    }
    // Cookie 0 is a documented no-op success (do not leave the lock).
    if (cookie == 0) {
        m.set_return(nt_status::STATUS_SUCCESS);
        return;
    }
    if (m_loader_lock_depth == 0) {
        m.set_return(nt_status::STATUS_INVALID_PARAMETER_2);
        return;
    }
    --m_loader_lock_depth;
    m_loader_lock.unlock();
    m.set_return(nt_status::STATUS_SUCCESS);
}

namespace {


} // namespace

void ntdll::impl_NtQuerySemaphore(Marshaler& m, NtdllExtManager& mgr) {
    constexpr u32 SemaphoreBasicInformation = 0;
    const Handle h = static_cast<Handle>(m.arg(0));
    const u32 info_class = static_cast<u32>(m.arg(1));
    const GuestAddr info_ptr = m.arg_ptr(2);
    const u32 info_length = static_cast<u32>(m.arg(3));
    const GuestAddr ret_len = m.arg_ptr(4);

    if (info_class != SemaphoreBasicInformation) {
        m.set_return(nt_status::STATUS_INVALID_INFO_CLASS);
        return;
    }
    if (info_length < 8 || info_ptr == 0) {
        m.set_return(nt_status::STATUS_INFO_LENGTH_MISMATCH);
        return;
    }
    SyncManager* sync = mgr.sync_manager();
    if (!sync) {
        m.set_return(nt_status::STATUS_NOT_IMPLEMENTED);
        return;
    }
    i32 current = 0;
    i32 maximum = 0;
    if (!sync->query_semaphore(h, &current, &maximum)) {
        m.set_return(nt_status::STATUS_INVALID_HANDLE);
        return;
    }
    (void)m.write_u32(info_ptr, static_cast<u32>(current));
    (void)m.write_u32(info_ptr + 4, static_cast<u32>(maximum));
    if (ret_len) {
        (void)m.write_u32(ret_len, 8u);
    }
    VITREO_TRACE("[ntdll] NtQuerySemaphore(handle=0x%x) -> cur=%d max=%d\n",
                 h, current, maximum);
    m.set_return(nt_status::STATUS_SUCCESS);
}

void NtdllExtManager::impl_LdrLoadDll(Marshaler& m) {
    // NTSTATUS LdrLoadDll(PWSTR SearchPath OPTIONAL, PULONG DllCharacteristics
    // OPTIONAL, PUNICODE_STRING ModuleFileName, PVOID* BaseAddress)
    (void)m.arg_ptr(0);
    (void)m.arg_ptr(1);
    const GuestAddr name_us = m.arg_ptr(2);
    const GuestAddr base_out = m.arg_ptr(3);
    if (name_us == 0 || base_out == 0 || m_module_manager == nullptr) {
        m.set_return(nt_status::STATUS_INVALID_PARAMETER);
        return;
    }
    const auto length = m.memory().read_u16(name_us);
    const auto buffer = m.read_ptr(name_us + (m.cpu_is_64bit() ? 8u : 4u));
    if (length.is_err() || buffer.is_err() || buffer.value() == 0) {
        m.set_return(nt_status::STATUS_INVALID_PARAMETER);
        return;
    }
    const u16 bytes = length.value();
    const u32 units = bytes / 2u;
    std::vector<u16> wide(units);
    for (u32 i = 0; i < units; ++i) {
        auto cu = m.memory().read_u16(buffer.value() + i * 2u);
        if (cu.is_err()) {
            m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
            return;
        }
        wide[i] = cu.value();
    }
    const std::string name = utf16_to_utf8(wide.data(), static_cast<int>(units));
    const GuestAddr base = m_module_manager->load_library(name, 0);
    if (base == 0) {
        m.set_return(nt_status::STATUS_DLL_NOT_FOUND);
        return;
    }
    (void)m.write_ptr(base_out, base);
    m.set_return(nt_status::STATUS_SUCCESS);
}

void NtdllExtManager::impl_LdrGetProcedureAddress(Marshaler& m) {
    // NTSTATUS LdrGetProcedureAddress(PVOID BaseAddress, PANSI_STRING Name,
    //                                 ULONG Ordinal, PVOID* ProcedureAddress)
    const GuestAddr module = m.arg_ptr(0);
    const GuestAddr name_as = m.arg_ptr(1);
    const u32 ordinal = static_cast<u32>(m.arg(2));
    const GuestAddr out = m.arg_ptr(3);
    if (module == 0 || out == 0 || m_module_manager == nullptr) {
        m.set_return(nt_status::STATUS_INVALID_PARAMETER);
        return;
    }
    GuestAddr proc = 0;
    if (name_as != 0) {
        const auto length = m.memory().read_u16(name_as);
        const auto buffer = m.read_ptr(name_as + (m.cpu_is_64bit() ? 8u : 4u));
        if (length.is_err() || buffer.is_err() || buffer.value() == 0) {
            m.set_return(nt_status::STATUS_INVALID_PARAMETER);
            return;
        }
        std::string ansi;
        ansi.resize(length.value());
        auto rd = m.memory().read_bytes(buffer.value(),
                                        reinterpret_cast<u8*>(ansi.data()),
                                        length.value());
        if (rd.is_err()) {
            m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
            return;
        }
        proc = m_module_manager->get_proc_address(module, ansi);
    } else {
        proc = m_module_manager->get_proc_address_by_ordinal(
            module, static_cast<u16>(ordinal));
    }
    if (proc == 0) {
        m.set_return(nt_status::STATUS_PROCEDURE_NOT_FOUND);
        return;
    }
    (void)m.write_ptr(out, proc);
    m.set_return(nt_status::STATUS_SUCCESS);
}

} // namespace vitreo::winapi
