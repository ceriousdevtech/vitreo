/// @file file_mapping.cpp
/// @brief Implementation of memory-mapped files (WIN32-066)

#include "file_mapping.hpp"
#include "file_mapping_internal.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/error.hpp"
#include "memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/host_gui_pump.hpp"
#include "capsule/vfs/vfs.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/shared_memory.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace vitreo::winapi {

FileMappingObject::~FileMappingObject() {
    if (file_mmap != nullptr && file_mmap_len != 0) {
        ::munmap(file_mmap, file_mmap_len);
        file_mmap = nullptr;
        file_mmap_len = 0;
    }
}

const Byte* FileMappingObject::section_bytes() const noexcept {
    if (file_mmap != nullptr) {
        return static_cast<const Byte*>(file_mmap);
    }
    return backing_data.empty() ? nullptr : backing_data.data();
}

Byte* FileMappingObject::section_bytes() noexcept {
    if (file_mmap != nullptr) {
        return static_cast<Byte*>(file_mmap);
    }
    return backing_data.empty() ? nullptr : backing_data.data();
}

usize FileMappingObject::section_byte_count() const noexcept {
    if (file_mmap != nullptr) {
        return file_mmap_len;
    }
    return backing_data.size();
}

// =============================================================================
// Construction
// =============================================================================

FileMappingManager::FileMappingManager(HandleTable& handles, GuestMemory& memory,
                                       u64 maximum_address)
    : m_handles(handles), m_memory(memory),
      m_maximum_address(maximum_address) {}







// =============================================================================
// Thunk Registration
// =============================================================================

DllExports create_file_mapping_thunks(FileMappingManager& mgr) {
    DllExports exports;

    exports.entries.push_back(ExportEntry{
        "CreateFileMappingW", 0,
        [&mgr](Marshaler& m) {
            u32 h_file    = static_cast<u32>(m.arg(0));
            const GuestAddr security_attributes = m.arg_ptr(1);
            bool inheritable = false;
            if (security_attributes != 0) {
                const GuestAddr inherit_offset =
                    m.cpu_is_64bit() ? 16u : 8u;
                inheritable =
                    m.read_u32(security_attributes + inherit_offset)
                        .value_or(0) != 0;
            }
            u32 protect   = static_cast<u32>(m.arg(2));
            u32 size_high = static_cast<u32>(m.arg(3));
            u32 size_low  = static_cast<u32>(m.arg(4));

            u64 max_size = (static_cast<u64>(size_high) << 32) | size_low;

            std::string name;
            GuestAddr name_ptr = m.arg_ptr(5);
            if (name_ptr != 0) {
                auto res = m.read_wide_string(name_ptr);
                if (res.is_ok()) {
                    name = res.value();
                }
            }

            Handle result = mgr.create_file_mapping(
                static_cast<Handle>(h_file),
                protect, max_size, name, inheritable);
            // Same gate as the ReadFile load-path trace: a guest that maps a
            // package instead of streaming it takes a completely different
            // path, so the two must be readable in one ordered log.
            static const bool log_io = std::getenv("VITREO_LOG_FILEIO") != nullptr;
            if (log_io) {
                std::fprintf(stderr,
                    "[fileio] ret=0x%llx CreateFileMappingW(hfile=0x%x prot=0x%x "
                    "max=%llu) -> 0x%x\n",
                    static_cast<unsigned long long>(m.guest_return_address()),
                    h_file, protect,
                    static_cast<unsigned long long>(max_size),
                    static_cast<unsigned>(result));
            }
            m.set_return(static_cast<u64>(result));
        }, 24});

    exports.entries.push_back(ExportEntry{
        "OpenFileMappingW", 0,
        [&mgr](Marshaler& m) {
            u32 access = static_cast<u32>(m.arg(0));
            const bool inheritable = m.arg(1) != 0;

            std::string name;
            GuestAddr name_ptr = m.arg_ptr(2);
            if (name_ptr != 0) {
                auto res = m.read_wide_string(name_ptr);
                if (res.is_ok()) {
                    name = res.value();
                }
            }

            Handle result =
                mgr.open_file_mapping(access, name, inheritable);
            m.set_return(static_cast<u64>(result));
        }, 12});

    exports.entries.push_back(ExportEntry{
        "MapViewOfFile", 0,
        [&mgr](Marshaler& m) {
            u32 h_mapping = static_cast<u32>(m.arg(0));
            u32 access    = static_cast<u32>(m.arg(1));
            u32 off_high  = static_cast<u32>(m.arg(2));
            u32 off_low   = static_cast<u32>(m.arg(3));
            u64 num_bytes = m.arg(4);

            u64 offset = (static_cast<u64>(off_high) << 32) | off_low;

            GuestAddr result = mgr.map_view_of_file(
                static_cast<Handle>(h_mapping),
                access, offset, num_bytes);
            VITREO_TRACE(
                "[file_mapping] MapViewOfFile(h=0x%x, access=0x%x, "
                "off=0x%llx, bytes=%llu) -> 0x%llx\n",
                h_mapping, access,
                static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(num_bytes),
                static_cast<unsigned long long>(result));
            static const bool log_io_mv = std::getenv("VITREO_LOG_FILEIO") != nullptr;
            if (log_io_mv) {
                std::fprintf(stderr,
                    "[fileio] ret=0x%llx MapViewOfFile(h=0x%x access=0x%x "
                    "off=0x%llx bytes=%llu) -> 0x%llx\n",
                    static_cast<unsigned long long>(m.guest_return_address()),
                    h_mapping, access,
                    static_cast<unsigned long long>(offset),
                    static_cast<unsigned long long>(num_bytes),
                    static_cast<unsigned long long>(result));
            }
            m.set_return(static_cast<u64>(result));
        }, 20});

    // CreateFileMappingA (6 args, stdcall_bytes=24) — ANSI name variant
    exports.entries.push_back(ExportEntry{
        "CreateFileMappingA", 0,
        [&mgr](Marshaler& m) {
            u32 h_file    = static_cast<u32>(m.arg(0));
            const GuestAddr security_attributes = m.arg_ptr(1);
            bool inheritable = false;
            if (security_attributes != 0) {
                const GuestAddr inherit_offset =
                    m.cpu_is_64bit() ? 16u : 8u;
                inheritable =
                    m.read_u32(security_attributes + inherit_offset)
                        .value_or(0) != 0;
            }
            u32 protect   = static_cast<u32>(m.arg(2));
            u32 size_high = static_cast<u32>(m.arg(3));
            u32 size_low  = static_cast<u32>(m.arg(4));

            u64 max_size = (static_cast<u64>(size_high) << 32) | size_low;

            std::string name;
            GuestAddr name_ptr = m.arg_ptr(5);
            if (name_ptr != 0) {
                auto res = m.read_ansi_string(name_ptr);
                if (res.is_ok()) {
                    name = res.value();
                }
            }

            Handle result = mgr.create_file_mapping(
                static_cast<Handle>(h_file),
                protect, max_size, name, inheritable);
            // TS3.exe imports the ANSI variant, so this is the one that
            // decides whether the guest maps its packages or falls back to
            // streaming them through ReadFile.
            static const bool log_io_a = std::getenv("VITREO_LOG_FILEIO") != nullptr;
            if (log_io_a) {
                std::fprintf(stderr,
                    "[fileio] ret=0x%llx CreateFileMappingA(hfile=0x%x prot=0x%x "
                    "max=%llu) -> 0x%x\n",
                    static_cast<unsigned long long>(m.guest_return_address()),
                    h_file, protect,
                    static_cast<unsigned long long>(max_size),
                    static_cast<unsigned>(result));
            }
            m.set_return(static_cast<u64>(result));
        }, 24});

    // OpenFileMappingA (3 args, stdcall_bytes=12) — ANSI name variant
    exports.entries.push_back(ExportEntry{
        "OpenFileMappingA", 0,
        [&mgr](Marshaler& m) {
            u32 access = static_cast<u32>(m.arg(0));
            const bool inheritable = m.arg(1) != 0;

            std::string name;
            GuestAddr name_ptr = m.arg_ptr(2);
            if (name_ptr != 0) {
                auto res = m.read_ansi_string(name_ptr);
                if (res.is_ok()) {
                    name = res.value();
                }
            }

            Handle result =
                mgr.open_file_mapping(access, name, inheritable);
            m.set_return(static_cast<u64>(result));
        }, 12});

    // MapViewOfFileEx (6 args, stdcall_bytes=24) — same as MapViewOfFile + base hint
    exports.entries.push_back(ExportEntry{
        "MapViewOfFileEx", 0,
        [&mgr](Marshaler& m) {
            u32 h_mapping = static_cast<u32>(m.arg(0));
            u32 access    = static_cast<u32>(m.arg(1));
            u32 off_high  = static_cast<u32>(m.arg(2));
            u32 off_low   = static_cast<u32>(m.arg(3));
            u64 num_bytes = m.arg(4);
            GuestAddr base_address = m.arg_ptr(5);

            u64 offset = (static_cast<u64>(off_high) << 32) | off_low;

            GuestAddr result = mgr.map_view_of_file_ex(
                static_cast<Handle>(h_mapping), access, offset, num_bytes,
                base_address);
            VITREO_TRACE(
                "[file_mapping] MapViewOfFileEx(h=0x%x, access=0x%x, "
                "off=0x%llx, bytes=%llu, base=0x%llx) -> 0x%llx\n",
                h_mapping, access,
                static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(num_bytes),
                static_cast<unsigned long long>(base_address),
                static_cast<unsigned long long>(result));
            m.set_return(static_cast<u64>(result));
        }, 24});

    exports.entries.push_back(ExportEntry{
        "UnmapViewOfFile", 0,
        [&mgr](Marshaler& m) {
            GuestAddr base = m.arg_ptr(0);
            bool ok = mgr.unmap_view_of_file(base);
            m.set_return(ok ? 1u : 0u);
        }, 4});

    exports.entries.push_back(ExportEntry{
        "FlushViewOfFile", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_NOACCESS); m.set_return(0); return; }
            GuestAddr base = m.arg_ptr(0);
            u64 num_bytes = m.arg(1);
            bool ok = mgr.flush_view_of_file(base, num_bytes);
            m.set_return(ok ? 1u : 0u);
        }, 8});

    return exports;
}

DllExports create_placeholder_mapping_thunks(FileMappingManager& mgr,
                                              VirtualMemoryManager& vmm) {
    DllExports exports;
    exports.entries.reserve(2);

    exports.entries.push_back(ExportEntry{
        "MapViewOfFile3", 0,
        [&mgr, &vmm](Marshaler& m) {
            const u64 process = m.arg(1);
            const bool current_process =
                process == 0 || process == ~u64{0} ||
                process == 0xFFFF'FFFFu;
            if (!current_process) {
                set_last_error(error_codes::ERROR_NOT_SUPPORTED);
                m.set_return(0);
                return;
            }

            const Handle mapping = static_cast<Handle>(m.arg(0));
            const GuestAddr base = m.arg_ptr(2);
            u64 offset = 0;
            u64 view_size = 0;
            u32 allocation_type = 0;
            u32 page_protection = 0;
            u64 extended_parameters = 0;
            u32 parameter_count = 0;
            if (m.cpu_is_64bit()) {
                offset = m.arg(3);
                view_size = m.arg(4);
                allocation_type = static_cast<u32>(m.arg(5));
                page_protection = static_cast<u32>(m.arg(6));
                extended_parameters = m.arg(7);
                parameter_count = static_cast<u32>(m.arg(8));
            } else {
                offset = m.arg(3) | (m.arg(4) << 32);
                view_size = m.arg(5);
                allocation_type = static_cast<u32>(m.arg(6));
                page_protection = static_cast<u32>(m.arg(7));
                extended_parameters = m.arg(8);
                parameter_count = static_cast<u32>(m.arg(9));
            }
            if (parameter_count != 0) {
                (void)extended_parameters;
                set_last_error(error_codes::ERROR_NOT_SUPPORTED);
                m.set_return(0);
                return;
            }

            const GuestAddr result = mgr.map_view_of_file3(
                mapping, base, offset, view_size, allocation_type,
                page_protection, vmm);
            m.set_return(result);
        }, 40});

    exports.entries.push_back(ExportEntry{
        "UnmapViewOfFile2", 0,
        [&mgr, &vmm](Marshaler& m) {
            const u64 process = m.arg(0);
            const bool current_process =
                process == 0 || process == ~u64{0} ||
                process == 0xFFFF'FFFFu;
            if (!current_process) {
                set_last_error(error_codes::ERROR_NOT_SUPPORTED);
                m.set_return(0);
                return;
            }
            const bool ok = mgr.unmap_view_of_file2(
                m.arg_ptr(1), static_cast<u32>(m.arg(2)), vmm);
            m.set_return(ok ? 1u : 0u);
        }, 12});

    return exports;
}

} // namespace vitreo::winapi
