// SPDX-License-Identifier: LGPL-2.1-only

/// @file kernel32_remote.cpp
/// @brief The *Ex calls that operate on another process's address space.
///
/// ReadProcessMemory, WriteProcessMemory, VirtualAllocEx / FreeEx / ProtectEx /
/// QueryEx, CreateRemoteThread(Ex), and the external primary-thread suspend
/// bookkeeping that CreateProcess(CREATE_SUSPENDED) leaves behind.
///
/// On Windows these are the same operations as their in-process counterparts
/// with a handle argument. Here they are not: the target lives in a different
/// host process, so each one crosses the IPC gate to a remote_memory server
/// rather than touching GuestMemory directly. That difference is why they are
/// their own file.
///
/// Split out of the "Group A: Process/Job" run of kernel32_ext.cpp; the job
/// objects that shared that banner are in kernel32_job.cpp.

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_handle.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"

#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

namespace vitreo::winapi {


std::optional<u32> Kernel32ExtManager::resume_external_primary_thread(
    Handle thread_handle) {
    pid_t pid = 0;
    u32 previous = 0;
    bool should_resume = false;
    {
        std::lock_guard lock(m_external_thread_mutex);
        const auto handle_it = m_external_primary_handles.find(thread_handle);
        if (handle_it == m_external_primary_handles.end()) return std::nullopt;
        auto found = m_external_primary_by_tid.find(handle_it->second);
        if (found == m_external_primary_by_tid.end()) return std::nullopt;
        previous = found->second.suspend_count;
        if (found->second.suspend_count > 0) {
            --found->second.suspend_count;
            should_resume = found->second.suspend_count == 0;
        }
        if (found->second.process != nullptr) {
            pid = found->second.process->pid;
        }
    }
    if (should_resume &&
        (pid <= 0 || !ipc::remote_resume_primary_thread(pid))) {
        std::lock_guard lock(m_external_thread_mutex);
        const auto handle_it = m_external_primary_handles.find(thread_handle);
        if (handle_it != m_external_primary_handles.end()) {
            auto found = m_external_primary_by_tid.find(handle_it->second);
            if (found != m_external_primary_by_tid.end()) {
                ++found->second.suspend_count;
            }
        }
        return ~0u;
    }
    ipc::ipc_log(
        "ResumeThread external h=0x%x pid=%d previous=%u",
        static_cast<unsigned>(thread_handle), static_cast<int>(pid), previous);
    return previous;
}

void Kernel32ExtManager::forget_external_primary_thread(
    Handle thread_handle) {
    std::lock_guard lock(m_external_thread_mutex);
    m_external_primary_handles.erase(thread_handle);
    // Leave m_external_primary_by_tid so OpenThread(dwThreadId) still works
    // after the CreateProcess hThread is closed.
}

void Kernel32ExtManager::impl_ReadProcessMemory(Marshaler& m) {
    const u64 proc = m.arg(0);
    const GuestAddr base = m.arg_ptr(1);
    const GuestAddr buf = m.arg_ptr(2);
    const u64 size = m.arg(3);
    const GuestAddr nread = m.arg_ptr(4);
    constexpr u32 ERROR_PARTIAL_COPY = 299;
    constexpr u32 PROCESS_VM_READ = 0x00000010;

    const auto write_count = [&](u64 count) {
        if (nread == 0) return true;
        return m.cpu_is_64bit()
            ? m.write_u64(nread, count).is_ok()
            : (count <= std::numeric_limits<u32>::max() &&
               m.write_u32(nread, static_cast<u32>(count)).is_ok());
    };
    if (!write_count(0)) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }

    ipc::ProcessObject* process = nullptr;
    const bool current_process =
        proc == ~u64{0} ||
        proc == static_cast<u64>(CURRENT_PROCESS_PSEUDO);
    if (!current_process) {
        if (m_handle_table == nullptr ||
            proc > static_cast<u64>(std::numeric_limits<Handle>::max())) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const Handle handle = static_cast<Handle>(proc);
        process = m_handle_table->get_as<ipc::ProcessObject>(
            handle, HandleType::Process);
        if (process == nullptr) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const u32 access = m_handle_table->get_access(handle);
        if (access != HandleAccess::All &&
            (access & PROCESS_VM_READ) == 0) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            m.set_return(0);
            return;
        }
    }

    // A duplicated current-process handle addresses this guest memory just as
    // the pseudo-handle does.
    if (current_process ||
        (process != nullptr &&
         process->pid == static_cast<pid_t>(::getpid()))) {
        for (u64 i = 0; i < size; ++i) {
            const auto b = m_memory.read_u8(base + i);
            if (b.is_err() || m.write_u8(buf + i, b.value()).is_err()) {
                (void)write_count(i);
                set_last_error(ERROR_PARTIAL_COPY);
                m.set_return(0);
                return;
            }
        }
        if (!write_count(size)) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }
        m.set_return(1);
        return;
    }

    // Independent workers do not share GuestMemory. The parent nevertheless
    // owns the deterministic startup facts Windows exposes for a just-created
    // process. Model only the PEB prefix initialized by setup_teb. That
    // prefix includes ImageBaseAddress. Fail honestly for every unmodeled
    // remote range.
    ipc::GuestProcessMetadata metadata;
    if (process == nullptr || !process->guest_metadata(metadata)) {
        set_last_error(ERROR_PARTIAL_COPY);
        m.set_return(0);
        return;
    }
    const u64 modeled_size = metadata.is_64bit ? 0x28u : 0x14u;
    const bool in_modeled_peb =
        base >= metadata.peb_address &&
        base <= metadata.peb_address + modeled_size &&
        size <= modeled_size &&
        base - metadata.peb_address <= modeled_size - size;
    if (!in_modeled_peb) {
        if (size > static_cast<u64>(std::numeric_limits<usize>::max())) {
            set_last_error(ERROR_PARTIAL_COPY);
            m.set_return(0);
            return;
        }
        std::vector<Byte> bytes(static_cast<usize>(size));
        if (!metadata.remote_memory_available ||
            !ipc::remote_read_process_memory(
                process->pid, base, std::span<Byte>(bytes))) {
            std::fprintf(stderr,
                         "[process] ReadProcessMemory remote-fail "
                         "self=%d target=%d base=0x%llx size=%llu\n",
                         static_cast<int>(::getpid()),
                         static_cast<int>(process->pid),
                         static_cast<unsigned long long>(base),
                         static_cast<unsigned long long>(size));
            std::fflush(stderr);
            ipc::ipc_log(
                "ReadProcessMemory pid=%d remote read failed "
                "base=0x%llx size=%llu",
                static_cast<int>(process->pid),
                static_cast<unsigned long long>(base),
                static_cast<unsigned long long>(size));
            set_last_error(ERROR_PARTIAL_COPY);
            m.set_return(0);
            return;
        }
        for (u64 i = 0; i < size; ++i) {
            if (m.write_u8(buf + i, bytes[static_cast<usize>(i)]).is_err()) {
                (void)write_count(i);
                set_last_error(ERROR_PARTIAL_COPY);
                m.set_return(0);
                return;
            }
        }
        if (!write_count(size)) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }
        ipc::ipc_log(
            "ReadProcessMemory pid=%d remote base=0x%llx bytes=%llu",
            static_cast<int>(process->pid),
            static_cast<unsigned long long>(base),
            static_cast<unsigned long long>(size));
        m.set_return(1);
        return;
    }

    const u64 byte_offset = base - metadata.peb_address;
    const u64 image_base_offset = metadata.is_64bit ? 0x10u : 0x08u;
    const u64 pointer_size = metadata.is_64bit ? 8u : 4u;
    const u64 process_parameters_offset = metadata.is_64bit ? 0x20u : 0x10u;
    const u64 process_parameters = metadata.peb_address + 0x400u;
    for (u64 i = 0; i < size; ++i) {
        const u64 offset = byte_offset + i;
        u8 value = 0;
        if (offset >= image_base_offset &&
            offset < image_base_offset + pointer_size) {
            value = static_cast<u8>(
                metadata.image_base >>
                ((offset - image_base_offset) * 8));
        } else if (offset >= process_parameters_offset &&
                   offset < process_parameters_offset + pointer_size) {
            value = static_cast<u8>(
                process_parameters >>
                ((offset - process_parameters_offset) * 8));
        }
        if (m.write_u8(buf + i, value).is_err()) {
            (void)write_count(i);
            set_last_error(ERROR_PARTIAL_COPY);
            m.set_return(0);
            return;
        }
    }
    if (!write_count(size)) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    ipc::ipc_log(
        "ReadProcessMemory pid=%d PEB startup prefix image_base=0x%llx "
        "base=0x%llx bytes=%llu",
        static_cast<int>(process->pid),
        static_cast<unsigned long long>(metadata.image_base),
        static_cast<unsigned long long>(base),
        static_cast<unsigned long long>(size));
    m.set_return(1);
}
void Kernel32ExtManager::impl_WriteProcessMemory(Marshaler& m) {
    const u64 proc = m.arg(0);
    const GuestAddr base = m.arg_ptr(1);
    const GuestAddr buf = m.arg_ptr(2);
    const u64 size = m.arg(3);
    const GuestAddr nwritten = m.arg_ptr(4);
    constexpr u32 ERROR_PARTIAL_COPY = 299;
    constexpr u32 PROCESS_VM_OPERATION = 0x00000008;
    constexpr u32 PROCESS_VM_WRITE = 0x00000020;
    const auto write_count = [&](u64 count) {
        if (nwritten == 0) return true;
        return m.cpu_is_64bit()
            ? m.write_u64(nwritten, count).is_ok()
            : (count <= std::numeric_limits<u32>::max() &&
               m.write_u32(nwritten, static_cast<u32>(count)).is_ok());
    };
    if (!write_count(0)) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }

    ipc::ProcessObject* process = nullptr;
    const bool current_process =
        proc == ~u64{0} ||
        proc == static_cast<u64>(CURRENT_PROCESS_PSEUDO);
    if (!current_process) {
        if (m_handle_table == nullptr ||
            proc > static_cast<u64>(std::numeric_limits<Handle>::max())) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const Handle handle = static_cast<Handle>(proc);
        process = m_handle_table->get_as<ipc::ProcessObject>(
            handle, HandleType::Process);
        if (process == nullptr) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const u32 access = m_handle_table->get_access(handle);
        if (access != HandleAccess::All &&
            (access & (PROCESS_VM_OPERATION | PROCESS_VM_WRITE)) !=
                (PROCESS_VM_OPERATION | PROCESS_VM_WRITE)) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            m.set_return(0);
            return;
        }
    }

    if (size > static_cast<u64>(std::numeric_limits<usize>::max())) {
        set_last_error(ERROR_PARTIAL_COPY);
        m.set_return(0);
        return;
    }
    std::vector<Byte> bytes(static_cast<usize>(size));
    for (u64 i = 0; i < size; ++i) {
        const auto byte = m_memory.read_u8(buf + i);
        if (byte.is_err()) {
            (void)write_count(i);
            set_last_error(ERROR_PARTIAL_COPY);
            m.set_return(0);
            return;
        }
        bytes[static_cast<usize>(i)] = byte.value();
    }

    // WriteProcessMemory is a debugger convenience: if the target pages are
    // not writable (typical for PE .text / packer RX sections), Windows
    // temporarily elevates protection, writes, then restores (Raymond Chen /
    // NtProtectVirtualMemory). Plain write_bytes respects guest protection
    // and would return ERROR_PARTIAL_COPY — breaking SecuROM-style self-decrypt.
    auto write_local = [&]() -> bool {
        if (m_memory.write_bytes(base, bytes.data(), bytes.size()).is_ok()) {
            return true;
        }
        if (m_virtual_memory_manager == nullptr || size == 0) {
            return false;
        }
        u32 elevate = PAGE_READWRITE;
        GuestAddr span_base = 0;
        usize span_size = 0;
        MemoryProtection cur = MemoryProtection::None;
        if (m_memory.query_region(base, span_base, span_size, cur) &&
            has_flag(cur, MemoryProtection::Execute)) {
            elevate = PAGE_EXECUTE_READWRITE;
        }
        u32 previous = 0;
        if (!m_virtual_memory_manager->virtual_protect(
                static_cast<u64>(base), size, elevate, &previous)) {
            return false;
        }
        const bool ok =
            m_memory.write_bytes(base, bytes.data(), bytes.size()).is_ok();
        u32 ignored = 0;
        (void)m_virtual_memory_manager->virtual_protect(
            static_cast<u64>(base), size, previous, &ignored);
        return ok;
    };

    bool wrote = false;
    if (current_process ||
        (process != nullptr &&
         process->pid == static_cast<pid_t>(::getpid()))) {
        wrote = write_local();
    } else {
        ipc::GuestProcessMetadata metadata;
        wrote = process != nullptr &&
                process->guest_metadata(metadata) &&
                metadata.remote_memory_available &&
                ipc::remote_write_process_memory(
                    process->pid, base, std::span<const Byte>(bytes));
    }
    if (!wrote) {
        if (process != nullptr &&
            process->pid != static_cast<pid_t>(::getpid())) {
            ipc::GuestProcessMetadata failed_meta;
            const bool have_meta = process->guest_metadata(failed_meta);
            ipc::ipc_log(
                "WriteProcessMemory pid=%d remote write failed "
                "base=0x%llx bytes=%llu remote_mem=%d",
                static_cast<int>(process->pid),
                static_cast<unsigned long long>(base),
                static_cast<unsigned long long>(size),
                have_meta && failed_meta.remote_memory_available ? 1 : 0);
        }
        set_last_error(ERROR_PARTIAL_COPY);
        m.set_return(0);
        return;
    }
    // Flush the instruction cache after
    // a successful local write so modified code becomes visible. Vitreo's JIT
    // is that cache; write_bytes already notifies, but restore the explicit
    // flush contract for elevated-protect paths and clarity.
    if (current_process ||
        (process != nullptr &&
         process->pid == static_cast<pid_t>(::getpid()))) {
        m_memory.notify_executable_write(base, static_cast<usize>(size));
    }
    if (!write_count(size)) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    if (process != nullptr &&
        process->pid != static_cast<pid_t>(::getpid())) {
        ipc::ipc_log(
            "WriteProcessMemory pid=%d remote base=0x%llx bytes=%llu",
            static_cast<int>(process->pid),
            static_cast<unsigned long long>(base),
            static_cast<unsigned long long>(size));
    }
    m.set_return(1);
}
void Kernel32ExtManager::impl_VirtualAllocEx(Marshaler& m) {
    const u64 proc = m.arg(0);
    const u64 address = m.arg(1);
    const u64 size = m.arg(2);
    const u32 allocation_type = static_cast<u32>(m.arg(3));
    const u32 protect = static_cast<u32>(m.arg(4));
    if (!m_virtual_memory_manager || size == 0 ||
        (allocation_type & (MEM_COMMIT | MEM_RESERVE)) == 0 ||
        !is_supported_win32_protection(protect)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0); return;
    }
    constexpr u32 PROCESS_VM_OPERATION = 0x00000008;
    ipc::ProcessObject* process = nullptr;
    const bool current_process =
        proc == ~u64{0} ||
        proc == static_cast<u64>(CURRENT_PROCESS_PSEUDO);
    if (!current_process) {
        if (m_handle_table == nullptr ||
            proc > static_cast<u64>(std::numeric_limits<Handle>::max())) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const Handle handle = static_cast<Handle>(proc);
        process = m_handle_table->get_as<ipc::ProcessObject>(
            handle, HandleType::Process);
        if (process == nullptr) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const u32 access = m_handle_table->get_access(handle);
        if (access != HandleAccess::All &&
            (access & PROCESS_VM_OPERATION) == 0) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            m.set_return(0);
            return;
        }
    }

    u64 result = 0;
    if (current_process ||
        (process != nullptr &&
         process->pid == static_cast<pid_t>(::getpid()))) {
        result = m_virtual_memory_manager->virtual_alloc(
            address, size, allocation_type, protect);
    } else {
        ipc::GuestProcessMetadata metadata;
        if (process != nullptr &&
            process->guest_metadata(metadata) &&
            metadata.remote_memory_available) {
            result = ipc::remote_virtual_alloc(
                process->pid, address, size, allocation_type, protect);
        }
    }
    static const bool log_valloc = std::getenv("VITREO_LOG_VALLOC") != nullptr;
    if (log_valloc) {
        std::fprintf(stderr,
            "[valloc] VirtualAllocEx(process=0x%llx hint=0x%llx size=0x%llx type=0x%x prot=0x%x) -> 0x%llx\n",
            static_cast<unsigned long long>(proc),
            static_cast<unsigned long long>(address),
            static_cast<unsigned long long>(size), allocation_type, protect,
            static_cast<unsigned long long>(result));
    }
    if (result != 0 && process != nullptr &&
        process->pid != static_cast<pid_t>(::getpid())) {
        ipc::ipc_log(
            "VirtualAllocEx pid=%d remote hint=0x%llx size=%llu "
            "type=0x%x protect=0x%x -> 0x%llx",
            static_cast<int>(process->pid),
            static_cast<unsigned long long>(address),
            static_cast<unsigned long long>(size),
            allocation_type, protect,
            static_cast<unsigned long long>(result));
    }
    if (!result) set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
    m.set_return(result);
}
void Kernel32ExtManager::impl_VirtualFreeEx(Marshaler& m) {
    const u64 proc = m.arg(0);
    const u64 address = m.arg(1);
    const u64 size = m.arg(2);
    const u32 free_type = static_cast<u32>(m.arg(3));
    if (!m_virtual_memory_manager || address == 0 ||
        (free_type != MEM_RELEASE && free_type != MEM_DECOMMIT) ||
        (free_type == MEM_RELEASE && size != 0)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0); return;
    }
    constexpr u32 PROCESS_VM_OPERATION = 0x00000008;
    ipc::ProcessObject* process = nullptr;
    const bool current_process =
        proc == ~u64{0} ||
        proc == static_cast<u64>(CURRENT_PROCESS_PSEUDO);
    if (!current_process) {
        if (m_handle_table == nullptr ||
            proc > static_cast<u64>(std::numeric_limits<Handle>::max())) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const Handle handle = static_cast<Handle>(proc);
        process = m_handle_table->get_as<ipc::ProcessObject>(
            handle, HandleType::Process);
        if (process == nullptr) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const u32 access = m_handle_table->get_access(handle);
        if (access != HandleAccess::All &&
            (access & PROCESS_VM_OPERATION) == 0) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            m.set_return(0);
            return;
        }
    }
    bool result = false;
    if (current_process ||
        (process != nullptr &&
         process->pid == static_cast<pid_t>(::getpid()))) {
        result = m_virtual_memory_manager->virtual_free(
            address, size, free_type);
    } else {
        ipc::GuestProcessMetadata metadata;
        result = process != nullptr &&
                 process->guest_metadata(metadata) &&
                 metadata.remote_memory_available &&
                 ipc::remote_virtual_free(
                     process->pid, address, size, free_type);
    }
    if (!result) set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    m.set_return(result ? 1 : 0);
}
void Kernel32ExtManager::impl_VirtualProtectEx(Marshaler& m) {
    const u64 proc = m.arg(0);
    const u64 address = m.arg(1);
    const u64 size = m.arg(2);
    const u32 new_protect = static_cast<u32>(m.arg(3));
    const GuestAddr old_protect = m.arg_ptr(4);
    if (!m_virtual_memory_manager || old_protect == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0); return;
    }
    constexpr u32 PROCESS_VM_OPERATION = 0x00000008;
    ipc::ProcessObject* process = nullptr;
    const bool current_process =
        proc == ~u64{0} ||
        proc == static_cast<u64>(CURRENT_PROCESS_PSEUDO);
    if (!current_process) {
        if (m_handle_table == nullptr ||
            proc > static_cast<u64>(std::numeric_limits<Handle>::max())) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const Handle handle = static_cast<Handle>(proc);
        process = m_handle_table->get_as<ipc::ProcessObject>(
            handle, HandleType::Process);
        if (process == nullptr) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const u32 access = m_handle_table->get_access(handle);
        if (access != HandleAccess::All &&
            (access & PROCESS_VM_OPERATION) == 0) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            m.set_return(0);
            return;
        }
    }
    // VirtualProtect rejects PAGE_WRITECOPY on private memory. VirtualProtectEx
    // of another process's image accepts PAGE_WRITECOPY so an interception
    // thunk can be a private copy of the image page.
    u32 protect = new_protect;
    if (!current_process) {
        constexpr u32 kPageMask = 0xFFu;
        const u32 base = protect & kPageMask;
        if (base == PAGE_WRITECOPY) {
            protect = (protect & ~kPageMask) | PAGE_READWRITE;
        } else if (base == PAGE_EXECUTE_WRITECOPY) {
            protect = (protect & ~kPageMask) | PAGE_EXECUTE_READWRITE;
        }
    }
    if (!is_supported_win32_protection(protect)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    u32 previous = 0;
    bool result = false;
    if (current_process ||
        (process != nullptr &&
         process->pid == static_cast<pid_t>(::getpid()))) {
        result = m_virtual_memory_manager->virtual_protect(
            address, size, protect, &previous);
    } else {
        ipc::GuestProcessMetadata metadata;
        result = process != nullptr &&
                 process->guest_metadata(metadata) &&
                 metadata.remote_memory_available &&
                 ipc::remote_virtual_protect(
                     process->pid, address, size, protect, previous);
    }
    if (result && m.write_u32(old_protect, previous).is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0); return;
    }
    if (!result) set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    m.set_return(result ? 1 : 0);
}

void Kernel32ExtManager::impl_VirtualQueryEx(Marshaler& m) {
    constexpr u32 PROCESS_QUERY_INFORMATION = 0x00000400;

    const u64 process_argument = m.arg(0);
    const GuestAddr address = m.arg_ptr(1);
    const GuestAddr buffer = m.arg_ptr(2);
    const u64 length = m.arg(3);
    const bool caller_is_64bit = m.cpu_is_64bit();
    const u64 output_size = caller_is_64bit ? 48u : 28u;

    if (buffer == 0 || length < output_size) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    ipc::ProcessObject* process = nullptr;
    const bool current_process =
        process_argument == ~u64{0} ||
        process_argument == static_cast<u64>(CURRENT_PROCESS_PSEUDO);
    if (!current_process) {
        if (m_handle_table == nullptr ||
            process_argument >
                static_cast<u64>(std::numeric_limits<Handle>::max())) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const Handle handle = static_cast<Handle>(process_argument);
        process = m_handle_table->get_as<ipc::ProcessObject>(
            handle, HandleType::Process);
        if (process == nullptr) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const u32 access = m_handle_table->get_access(handle);
        if (access != HandleAccess::All &&
            (access & PROCESS_QUERY_INFORMATION) == 0) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            m.set_return(0);
            return;
        }
    }

    ipc::RemoteMemoryBasicInformation information{};
    bool queried = false;
    const bool local_target =
        current_process ||
        (process != nullptr &&
         process->pid == static_cast<pid_t>(::getpid()));
    if (local_target) {
        MemoryBasicInformation local{};
        const u64 maximum_address =
            caller_is_64bit ? 0x00007FFF'FFFEFFFFULL : 0xFFFF'FFFFULL;
        VirtualQueryManager manager(
            m_memory, m_virtual_memory_manager);
        queried = manager.virtual_query(address, local, maximum_address) != 0;
        if (queried) {
            information = ipc::RemoteMemoryBasicInformation{
                local.base_address,
                local.allocation_base,
                local.region_size,
                local.allocation_protect,
                local.state,
                local.protect,
                local.type,
            };
        }
    } else {
        ipc::GuestProcessMetadata metadata;
        if (process != nullptr && process->guest_metadata(metadata) &&
            metadata.remote_memory_available) {
            const u64 maximum_address =
                caller_is_64bit && metadata.is_64bit
                    ? 0x00007FFF'FFFEFFFFULL
                    : 0xFFFF'FFFFULL;
            queried = ipc::remote_virtual_query(
                process->pid, address, maximum_address, information);
        }
    }

    if (!queried) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    VoidResult written = VoidResult::ok();
    if (caller_is_64bit) {
        written = m.write_u64(buffer, information.base_address);
        if (written.is_ok()) {
            written =
                m.write_u64(buffer + 8, information.allocation_base);
        }
        if (written.is_ok()) {
            written = m.write_u32(
                buffer + 16, information.allocation_protect);
        }
        if (written.is_ok()) {
            written = m.write_u32(buffer + 20, 0);
        }
        if (written.is_ok()) {
            written = m.write_u64(buffer + 24, information.region_size);
        }
        if (written.is_ok()) {
            written = m.write_u32(buffer + 32, information.state);
        }
        if (written.is_ok()) {
            written = m.write_u32(buffer + 36, information.protect);
        }
        if (written.is_ok()) {
            written = m.write_u32(buffer + 40, information.type);
        }
        if (written.is_ok()) {
            written = m.write_u32(buffer + 44, 0);
        }
    } else {
        written = m.write_u32(
            buffer, static_cast<u32>(information.base_address));
        if (written.is_ok()) {
            written = m.write_u32(
                buffer + 4, static_cast<u32>(information.allocation_base));
        }
        if (written.is_ok()) {
            written = m.write_u32(
                buffer + 8, information.allocation_protect);
        }
        if (written.is_ok()) {
            written = m.write_u32(
                buffer + 12, static_cast<u32>(information.region_size));
        }
        if (written.is_ok()) {
            written = m.write_u32(buffer + 16, information.state);
        }
        if (written.is_ok()) {
            written = m.write_u32(buffer + 20, information.protect);
        }
        if (written.is_ok()) {
            written = m.write_u32(buffer + 24, information.type);
        }
    }
    if (written.is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }

    VITREO_TRACE(
        "[kernel32_ext] VirtualQueryEx(process=0x%llx, address=0x%llx) "
        "-> base=0x%llx size=0x%llx state=0x%x protect=0x%x type=0x%x\n",
        static_cast<unsigned long long>(process_argument),
        static_cast<unsigned long long>(address),
        static_cast<unsigned long long>(information.base_address),
        static_cast<unsigned long long>(information.region_size),
        information.state, information.protect, information.type);
    m.set_return(output_size);
}

void Kernel32ExtManager::impl_CreateRemoteThread(Marshaler& m) {
    u64 proc = m.arg(0);
    // (HANDLE)-1 in either width: the x64 guest's own constant is all ones.
    if (proc != ~u64{0} && proc != static_cast<u64>(CURRENT_PROCESS_PSEUDO)) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        m.set_return(0); return;
    }
    if (m_thread_manager) {
        GuestAddr start = m.arg_ptr(3);
        GuestAddr param = m.arg_ptr(4);
        u32 flags = static_cast<u32>(m.arg(5));
        u32 tid = 0;
        Handle h = m_thread_manager->create_thread(0, start, param, flags, &tid);
        GuestAddr tid_out = m.arg_ptr(6);
        if (tid_out) (void)m.write_u32(tid_out, tid);
        m.set_return(static_cast<u64>(h));
        return;
    }
    set_last_error(error_codes::ERROR_ACCESS_DENIED);
    m.set_return(0);
}

void Kernel32ExtManager::impl_CreateRemoteThreadEx(Marshaler& m) {
    // HANDLE CreateRemoteThreadEx(
    //   HANDLE hProcess, LPSECURITY_ATTRIBUTES lpThreadAttributes,
    //   SIZE_T dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress,
    //   LPVOID lpParameter, DWORD dwCreationFlags,
    //   LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList, LPDWORD lpThreadId)
    //
    // CreateRemoteThreadEx is also used for worker threads in-process.
    // Attribute lists are accepted and ignored (same policy as
    // UpdateProcThreadAttribute stubs); stack size is left to ThreadManager
    // defaults.
    const u64 proc = m.arg(0);
    (void)m.arg_ptr(1); // security attributes
    (void)m.arg(2);     // stack size
    const GuestAddr start = m.arg_ptr(3);
    const GuestAddr param = m.arg_ptr(4);
    const u32 flags = static_cast<u32>(m.arg(5));
    (void)m.arg_ptr(6); // attribute list
    const GuestAddr tid_out = m.arg_ptr(7);

    bool same_process = proc == ~u64{0} ||
                        proc == static_cast<u64>(CURRENT_PROCESS_PSEUDO);
    if (!same_process && m_handle_table != nullptr) {
        if (auto* process = m_handle_table->get_as<ipc::ProcessObject>(
                static_cast<Handle>(proc), HandleType::Process)) {
            same_process = (process->pid == ::getpid());
        }
    }
    if (!same_process || m_thread_manager == nullptr) {
        ipc::ipc_log(
            "CreateRemoteThreadEx denied proc=0x%llx start=0x%llx",
            static_cast<unsigned long long>(proc),
            static_cast<unsigned long long>(start));
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        m.set_return(0);
        return;
    }

    u32 tid = 0;
    Handle h = m_thread_manager->create_thread(0, start, param, flags, &tid);
    if (tid_out) {
        (void)m.write_u32(tid_out, tid);
    }
    ipc::ipc_log(
        "CreateRemoteThreadEx start=0x%llx param=0x%llx flags=0x%x "
        "tid=%u -> handle=0x%x",
        static_cast<unsigned long long>(start),
        static_cast<unsigned long long>(param),
        flags, tid, static_cast<unsigned>(h));
    m.set_return(static_cast<u64>(h));
}
void Kernel32ExtManager::impl_GetProcessHandleCount(Marshaler& m) {
    // BOOL GetProcessHandleCount(HANDLE hProcess, PDWORD pdwHandleCount)
    // https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprocesshandlecount
    const u64 proc = m.arg(0);
    const GuestAddr count_ptr = m.arg_ptr(1);
    const bool current_process =
        proc == ~u64{0} ||
        proc == static_cast<u64>(CURRENT_PROCESS_PSEUDO);
    if (!current_process) {
        // Cross-process handle counts are not modeled yet.
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    if (count_ptr == 0 || m_handle_table == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const u32 count = static_cast<u32>(m_handle_table->count());
    if (m.write_u32(count_ptr, count).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

void Kernel32ExtManager::impl_GetProcessWorkingSetSizeEx(Marshaler& m) {
    // BOOL GetProcessWorkingSetSizeEx(HANDLE, PSIZE_T, PSIZE_T, PDWORD)
    // Learn: https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/
    //        nf-memoryapi-getprocessworkingsetsizeex
    // Windows documents default limits of 50 and 345 guest pages. They are
    // soft limits unless the corresponding HARDWS enable flag is selected.
    constexpr u64 guest_page_size = 4096;
    constexpr u64 default_minimum = 50 * guest_page_size;
    constexpr u64 default_maximum = 345 * guest_page_size;
    constexpr u32 QUOTA_LIMITS_HARDWS_MIN_DISABLE = 0x00000002;
    constexpr u32 QUOTA_LIMITS_HARDWS_MAX_DISABLE = 0x00000008;
    constexpr u32 PROCESS_QUERY_INFORMATION = 0x00000400;
    constexpr u32 PROCESS_QUERY_LIMITED_INFORMATION = 0x00001000;

    const u64 process_argument = m.arg(0);
    const GuestAddr minimum = m.arg_ptr(1);
    const GuestAddr maximum = m.arg_ptr(2);
    const GuestAddr flags = m.arg_ptr(3);
    if (minimum == 0 || maximum == 0 || flags == 0) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }

    const bool current_process =
        process_argument == ~u64{0} ||
        process_argument == static_cast<u64>(CURRENT_PROCESS_PSEUDO);
    if (!current_process) {
        if (m_handle_table == nullptr ||
            process_argument >
                static_cast<u64>(std::numeric_limits<Handle>::max())) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const Handle handle = static_cast<Handle>(process_argument);
        auto* process = m_handle_table->get_as<ipc::ProcessObject>(
            handle, HandleType::Process);
        if (process == nullptr) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const u32 access = m_handle_table->get_access(handle);
        if (access != HandleAccess::All &&
            (access & (PROCESS_QUERY_INFORMATION |
                       PROCESS_QUERY_LIMITED_INFORMATION)) == 0) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            m.set_return(0);
            return;
        }
        if (process->exited.load(std::memory_order_acquire)) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            m.set_return(0);
            return;
        }
    }

    const u32 default_flags = QUOTA_LIMITS_HARDWS_MIN_DISABLE |
                              QUOTA_LIMITS_HARDWS_MAX_DISABLE;
    if (m.write_ptr(minimum, default_minimum).is_err() ||
        m.write_ptr(maximum, default_maximum).is_err() ||
        m.write_u32(flags, default_flags).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

void Kernel32ExtManager::impl_GetProcessWorkingSetSize(Marshaler& m) {
    // BOOL GetProcessWorkingSetSize(HANDLE, PSIZE_T, PSIZE_T)
    // Same contract as Ex without the flags out-parameter.
    constexpr u64 guest_page_size = 4096;
    constexpr u64 default_minimum = 50 * guest_page_size;
    constexpr u64 default_maximum = 345 * guest_page_size;
    constexpr u32 PROCESS_QUERY_INFORMATION = 0x00000400;
    constexpr u32 PROCESS_QUERY_LIMITED_INFORMATION = 0x00001000;

    const u64 process_argument = m.arg(0);
    const GuestAddr minimum = m.arg_ptr(1);
    const GuestAddr maximum = m.arg_ptr(2);
    if (minimum == 0 || maximum == 0) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }

    const bool current_process =
        process_argument == ~u64{0} ||
        process_argument == static_cast<u64>(CURRENT_PROCESS_PSEUDO);
    if (!current_process) {
        if (m_handle_table == nullptr ||
            process_argument >
                static_cast<u64>(std::numeric_limits<Handle>::max())) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const Handle handle = static_cast<Handle>(process_argument);
        auto* process = m_handle_table->get_as<ipc::ProcessObject>(
            handle, HandleType::Process);
        if (process == nullptr) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const u32 access = m_handle_table->get_access(handle);
        if (access != HandleAccess::All &&
            (access & (PROCESS_QUERY_INFORMATION |
                       PROCESS_QUERY_LIMITED_INFORMATION)) == 0) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            m.set_return(0);
            return;
        }
    }

    if (m.write_ptr(minimum, default_minimum).is_err() ||
        m.write_ptr(maximum, default_maximum).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

void Kernel32ExtManager::impl_SetProcessWorkingSetSize(Marshaler& m) {
    // BOOL SetProcessWorkingSetSize(HANDLE, SIZE_T, SIZE_T)
    // Soft quota hint on Windows; Vitreo accepts current-process calls.
    constexpr u32 PROCESS_SET_QUOTA = 0x00000100;
    const u64 process_argument = m.arg(0);
    (void)m.arg(1);
    (void)m.arg(2);
    const bool current_process =
        process_argument == ~u64{0} ||
        process_argument == static_cast<u64>(CURRENT_PROCESS_PSEUDO);
    if (!current_process) {
        if (m_handle_table == nullptr ||
            process_argument >
                static_cast<u64>(std::numeric_limits<Handle>::max())) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const Handle handle = static_cast<Handle>(process_argument);
        auto* process = m_handle_table->get_as<ipc::ProcessObject>(
            handle, HandleType::Process);
        if (process == nullptr) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const u32 access = m_handle_table->get_access(handle);
        if (access != HandleAccess::All &&
            (access & PROCESS_SET_QUOTA) == 0) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            m.set_return(0);
            return;
        }
    }
    m.set_return(1);
}

void Kernel32ExtManager::impl_SetProcessWorkingSetSizeEx(Marshaler& m) {
    // BOOL SetProcessWorkingSetSizeEx(HANDLE, SIZE_T, SIZE_T, DWORD)
    (void)m.arg(3); // flags — soft vs hard ignored; Vitreo has no hard WS.
    impl_SetProcessWorkingSetSize(m);
}

void Kernel32ExtManager::impl_GetProcessMitigationPolicy(Marshaler& m) {
    const u64 process = m.arg(0);
    const u32 policy = static_cast<u32>(m.arg(1));
    GuestAddr buf = m.arg_ptr(2);
    u32 size = static_cast<u32>(m.arg(3));
    VITREO_TRACE("[process] GetProcessMitigationPolicy(process=0x%llx, policy=%u, "
                 "buf=0x%llx, size=%u)\n",
                 static_cast<unsigned long long>(process), policy,
                 static_cast<unsigned long long>(buf), size);
    if (buf && size) { for (u32 i = 0; i < size; ++i) (void)m.write_u8(buf + i, 0); }
    m.set_return(1);
}
void Kernel32ExtManager::impl_SetProcessMitigationPolicy(Marshaler& m) {
    (void)m.arg(0); (void)m.arg_ptr(1); (void)m.arg(2);
    m.set_return(1);
}
void Kernel32ExtManager::impl_SetProcessDEPPolicy(Marshaler& m) {
    (void)m.arg(0); m.set_return(1);
}
// BOOL SetProcessInformation(HANDLE hProcess, PROCESS_INFORMATION_CLASS,
//                            LPVOID ProcessInformation, DWORD ProcessInformationSize)
// Validation order and errors measured by power_throttling_probe.c (x86 = x64):
// size and contents are checked before the handle; PROCESS_SET_INFORMATION is
// required. Classes this does not implement fail loudly.
void Kernel32ExtManager::impl_SetProcessInformation(Marshaler& m) {
    // processthreadsapi.h PROCESS_INFORMATION_CLASS (SDK 10.0.26100).
    enum : u32 {
        ProcessMemoryPriority = 0,
        ProcessPowerThrottling = 4,
        ProcessInformationClassMax = 12,
    };
    constexpr u32 kProcessSetInformation = 0x0200u;
    const Handle process_handle = static_cast<Handle>(m.arg(0));
    const u32 info_class = static_cast<u32>(m.arg(1));
    const GuestAddr info = m.arg_ptr(2);
    const u32 size = static_cast<u32>(m.arg(3));
    auto fail = [&](u32 error) {
        set_last_error(error);
        m.set_return(0);
    };
    GuestAddr faulted = 0;
    auto read = [&](u32 offset, u32& out) {
        auto value = m_memory.read_u32(info + offset);
        if (value.is_err()) {
            faulted = info + offset;
            return false;
        }
        out = value.value();
        return true;
    };
    // Windows reads the structure in user mode: an unreadable one faults.
    auto fault = [&] {
        if (!m.raise_access_violation(false, faulted)) {
            fail(error_codes::ERROR_NOACCESS);
        }
    };
    // 0 with @p self set, or the Win32 error for the handle.
    auto resolve = [&](bool& self) -> u32 {
        self = process_handle == CURRENT_PROCESS_PSEUDO;
        if (self) return 0;
        if (m_handle_table == nullptr ||
            m_handle_table->get_type(process_handle) != HandleType::Process) {
            return error_codes::ERROR_INVALID_HANDLE;
        }
        const u32 access = m_handle_table->get_access(process_handle);
        if (access != HandleAccess::All &&
            (access & kProcessSetInformation) == 0) {
            return error_codes::ERROR_ACCESS_DENIED;
        }
        auto* process = static_cast<ipc::ProcessObject*>(
            m_handle_table->get(process_handle, HandleType::Process));
        if (process == nullptr) return error_codes::ERROR_INVALID_HANDLE;
        self = m_process_manager != nullptr &&
               process->pid == static_cast<pid_t>(
                   m_process_manager->get_current_process_id());
        return 0;
    };
    auto remote_unimplemented = [&](const char* what) {
        static std::atomic<bool> reported[ProcessInformationClassMax]{};
        if (!reported[info_class].exchange(true)) {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED kernel32!"
                         "SetProcessInformation: %s on another process\n", what);
        }
        fail(error_codes::ERROR_NOT_SUPPORTED);
    };

    switch (info_class) {
    case ProcessMemoryPriority: {
        // MEMORY_PRIORITY_INFORMATION; MEMORY_PRIORITY_VERY_LOW 1 .. NORMAL 5.
        if (size != 4) return fail(error_codes::ERROR_BAD_LENGTH);
        u32 priority = 0;
        if (!read(0, priority)) return fault();
        if (priority < 1 || priority > 5) {
            return fail(error_codes::ERROR_INVALID_PARAMETER);
        }
        bool self = false;
        if (const u32 error = resolve(self); error != 0) return fail(error);
        if (!self || m_process_manager == nullptr) {
            return remote_unimplemented("ProcessMemoryPriority");
        }
        // Orders working-set trimming, which Vitreo never does to a guest;
        // GetProcessInformation reads the recorded value back.
        m_process_manager->set_memory_priority(priority);
        m.set_return(1);
        return;
    }
    case ProcessPowerThrottling: {
        // PROCESS_POWER_THROTTLING_STATE { Version; ControlMask; StateMask; }
        u32 version = 0, control = 0, state = 0;
        if (size == 12 &&
            (!read(0, version) || !read(4, control) || !read(8, state))) {
            return fault();
        }
        if (const u32 error = validate_process_power_throttling(
                size, version, control, state);
            error != 0) {
            return fail(error);
        }
        bool self = false;
        if (const u32 error = resolve(self); error != 0) return fail(error);
        if (!self || m_thread_manager == nullptr) {
            return remote_unimplemented("ProcessPowerThrottling");
        }
        // IGNORE_TIMER_RESOLUTION only releases the system from honouring
        // timer-resolution requests ("no longer guaranteed"); keeping them
        // honoured is within it. The SDK names no flag for 0x2.
        if ((control & 0x2u) != 0) {
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true)) {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED kernel32!"
                             "SetProcessInformation: ProcessPowerThrottling "
                             "ControlMask 0x2 (accepted by Windows, no SDK "
                             "flag) has no host effect\n");
            }
        }
        m_thread_manager->set_process_power_throttling(control, state);
        m.set_return(1);
        return;
    }
    default:
        if (info_class < ProcessInformationClassMax) {
            static std::atomic<bool> reported[ProcessInformationClassMax]{};
            if (!reported[info_class].exchange(true)) {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED kernel32!"
                             "SetProcessInformation: class %u\n", info_class);
            }
        }
        return fail(error_codes::ERROR_INVALID_PARAMETER);
    }
}
void Kernel32ExtManager::impl_GetProcessInformation(Marshaler& m) {
    // BOOL GetProcessInformation(HANDLE hProcess,
    //   PROCESS_INFORMATION_CLASS ProcessInformationClass,
    //   LPVOID ProcessInformation, DWORD ProcessInformationSize)
    //
    // A query API, so only classes whose answer is unambiguous for Vitreo are
    // served: the memory priority SetProcessInformation recorded. Windows
    // itself refuses ProcessPowerThrottling queries (power_throttling_probe:
    // size != 12 -> ERROR_BAD_LENGTH, else ERROR_INVALID_PARAMETER, before
    // and after a SetProcessInformation). Every other class — including the
    // ones that report live runtime figures Vitreo does not track, such as
    // ProcessAppMemoryInfo — fails honestly.
    enum : u32 {
        ProcessMemoryPriority = 0,
        ProcessPowerThrottling = 4,
    };
    constexpr u32 MEMORY_PRIORITY_NORMAL = 5;

    (void)m.arg(0); // hProcess — only the current process is described
    const u32 info_class = static_cast<u32>(m.arg(1));
    const GuestAddr buffer = m.arg_ptr(2);
    const u32 size = static_cast<u32>(m.arg(3));

    if (buffer == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    switch (info_class) {
    case ProcessMemoryPriority: {
        // MEMORY_PRIORITY_INFORMATION { ULONG MemoryPriority; }
        if (size != 4) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        const u32 priority = m_process_manager != nullptr
                                 ? m_process_manager->memory_priority()
                                 : MEMORY_PRIORITY_NORMAL;
        (void)m.write_u32(buffer + 0, priority);
        VITREO_TRACE("[k32ext] GetProcessInformation(MemoryPriority) -> %u\n",
                     priority);
        m.set_return(1);
        return;
    }
    case ProcessPowerThrottling:
        set_last_error(size != 12 ? error_codes::ERROR_BAD_LENGTH
                                  : error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    default:
        VITREO_TRACE("[k32ext] GetProcessInformation(class=%u) -> FALSE "
                     "(class not supported)\n", info_class);
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
}
void Kernel32ExtManager::impl_CheckRemoteDebuggerPresent(Marshaler& m) {
    (void)m.arg(0);
    GuestAddr p = m.arg_ptr(1);
    if (p) (void)m.write_u32(p, 0);
    m.set_return(1);
}

} // namespace vitreo::winapi
