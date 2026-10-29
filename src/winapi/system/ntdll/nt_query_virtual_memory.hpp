#pragma once
/// @file nt_query_virtual_memory.hpp
/// @brief ntdll!NtQueryVirtualMemory: MemoryBasicInformation (class 0) and
///        MemoryMappedFilenameInformation (class 2), x86 and x64.

#include <functional>
#include <string>

#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"

namespace vitreo::winapi::ntdll {

/// What NtQueryVirtualMemory needs from the rest of the runtime.
struct NtQueryVirtualMemoryHost {
    /// Which process @p handle names: the current one, none (invalid), or
    /// another process (not implemented).
    enum class Target { Current, Invalid, Other };
    std::function<Target(u64 handle)> target;
    /// VirtualQuery for the current process (0 on an address it rejects).
    std::function<usize(u64 address, MemoryBasicInformation& info, u64 maximum)> basic;
    /// The Win32 path of the file backing an image or file-mapped view at
    /// @p address, or empty when the region has no file (private memory, a
    /// pagefile-backed section, free or reserved space).
    std::function<std::string(u64 address)> mapped_file;
    /// Win32 path -> NT device path (C:\ -> \Device\HarddiskVolume3\), or
    /// empty when no device names it.
    std::function<std::string(const std::string& win32_path)> nt_device_path;
};

/// NTSTATUS NtQueryVirtualMemory(HANDLE, PVOID BaseAddress,
///     MEMORY_INFORMATION_CLASS, PVOID, SIZE_T Length, PSIZE_T ReturnLength).
void query_virtual_memory(Marshaler& m, const NtQueryVirtualMemoryHost& host);

/// Install the host the ntdll export uses (wired once by the DLL registry).
void set_query_virtual_memory_host(NtQueryVirtualMemoryHost host);
[[nodiscard]] const NtQueryVirtualMemoryHost& query_virtual_memory_host();

}  // namespace vitreo::winapi::ntdll
