/// @file filepos.cpp
/// @brief GetFileSize/GetFileSizeEx/SetFilePointer/SetFilePointerEx thunks (WIN32-037)

#include "winapi/kernel32/filesystem/filepos.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/common/write_sha1_debug.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/system/ntdll/ntdll.hpp"
#include <cstdio>
#include <cstring>
#include <optional>

namespace vitreo::winapi {

namespace {

std::optional<u64> query_file_size(FileManager& fm,
                                   NtdllExtManager* nt_files,
                                   Handle handle) {
    if (auto size = fm.get_file_size64(handle); size.has_value()) {
        return size;
    }
    if (nt_files == nullptr) return std::nullopt;

    // FileStandardInformation.EndOfFile is the same underlying file-object
    // size observed by GetFileSizeEx. Kernel32 APIs must accept a file HANDLE
    // created through NtCreateFile; the HANDLE's creation frontend does not
    // change its Windows-visible type.
    constexpr u32 FileStandardInformation = 5;
    u8 info[24]{};
    if (nt_files->nt_query_information_file(
            handle, FileStandardInformation, info, sizeof(info)) !=
        nt_status::STATUS_SUCCESS) {
        return std::nullopt;
    }
    u64 size = 0;
    std::memcpy(&size, info + 8, sizeof(size));
    return size;
}

} // namespace

// =============================================================================
// impl_GetFileSize
// =============================================================================

void impl_GetFileSize(Marshaler& m, FileManager& fm,
                      NtdllExtManager* nt_files) {
    Handle handle      = static_cast<Handle>(m.arg(0));
    GuestAddr high_ptr = m.arg_ptr(1);

    const auto size = query_file_size(fm, nt_files, handle);
    if (!size.has_value()) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        VITREO_TRACE( "[file] GetFileSize handle=0x%llx -> INVALID\n",
                     static_cast<unsigned long long>(handle));
        m.set_return(static_cast<u64>(INVALID_FILE_SIZE));
        return;
    }

    const u32 low = static_cast<u32>(*size);
    const u32 high_part = static_cast<u32>(*size >> 32);

    if (high_ptr != 0) {
        (void)m.write_u32(high_ptr, high_part);
    }

    VITREO_TRACE( "[file] GetFileSize handle=0x%llx -> %u (high=%u)\n",
                 static_cast<unsigned long long>(handle), low, high_part);
    // Measured on Windows 11 (last_error_on_success_probe.expected): success
    // leaves the last error untouched, except when the low half collides with
    // INVALID_FILE_SIZE. Then it is NO_ERROR (Learn: callers must check
    // GetLastError to tell that size from failure), with or without a high
    // pointer.
    if (low == INVALID_FILE_SIZE) {
        set_last_error(error_codes::ERROR_SUCCESS);
    }
    m.set_return(static_cast<u64>(low));
}

// =============================================================================
// impl_GetFileSizeEx
// =============================================================================

void impl_GetFileSizeEx(Marshaler& m, FileManager& fm,
                        NtdllExtManager* nt_files) {
    Handle handle      = static_cast<Handle>(m.arg(0));
    GuestAddr size_ptr = m.arg_ptr(1);

    const auto size = query_file_size(fm, nt_files, handle);
    if (!size.has_value()) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }

    // Learn: lpFileSize is required. Windows AVs on a NULL out pointer;
    // Vitreo fails without crashing (files_core_probe GetFileSizeEx_null_out /
    // _noaccess_out measured: null -> INVALID_PARAMETER; NOACCESS page ->
    // ERROR_NOACCESS on Vitreo; Windows crashes before the NOACCESS case).
    if (size_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (m.write_u64(size_ptr, *size).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }

    m.set_return(1);
}

// =============================================================================
// impl_SetFilePointer
// =============================================================================

void impl_SetFilePointer(Marshaler& m, FileManager& fm) {
    Handle handle      = static_cast<Handle>(m.arg(0));
    i32 distance       = static_cast<i32>(m.arg(1));
    GuestAddr high_ptr = m.arg_ptr(2);
    u32 move_method    = static_cast<u32>(m.arg(3));

    // Learn: invalid handle -> ERROR_INVALID_HANDLE; invalid dwMoveMethod
    // -> ERROR_INVALID_PARAMETER; only a true negative seek is
    // ERROR_NEGATIVE_SEEK (files_core_probe SFP_invalid_*).
    if (fm.vfs_handle_for(handle) == capsule::INVALID_FILE_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(static_cast<u64>(INVALID_SET_FILE_POINTER));
        return;
    }
    if (move_method > 2) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(static_cast<u64>(INVALID_SET_FILE_POINTER));
        return;
    }

    i32 distance_high = 0;
    i32* distance_high_ptr = nullptr;
    if (high_ptr != 0) {
        auto high_val = m.read_u32(high_ptr);
        if (high_val.is_ok()) {
            distance_high = static_cast<i32>(high_val.value());
            distance_high_ptr = &distance_high;
        }
    }

    // Seek with the 64-bit primitive: a new position whose low half is
    // 0xFFFFFFFF is a success that collides with INVALID_SET_FILE_POINTER.
    const i64 offset = distance_high_ptr
        ? static_cast<i64>((static_cast<u64>(static_cast<u32>(distance_high)) << 32) |
                           static_cast<u64>(static_cast<u32>(distance)))
        : static_cast<i64>(distance);

    // Learn: when lpDistanceToMoveHigh is NULL and the new position does not
    // fit in a 32-bit value, fail and leave the pointer unmoved.
    std::optional<u64> prior_pos;
    if (high_ptr == 0) {
        prior_pos = fm.set_file_pointer64(handle, 0, 1 /*FILE_CURRENT*/);
    }

    const auto new_pos = fm.set_file_pointer64(handle, offset, move_method);
    if (!new_pos.has_value()) {
        // set_file_pointer64 already set INVALID_PARAMETER for NO_BUFFERING
        // misalignment / null-high overflow restore; keep it.
        if (get_last_error() != error_codes::ERROR_INVALID_PARAMETER) {
            set_last_error(error_codes::ERROR_NEGATIVE_SEEK);
        }
        m.set_return(static_cast<u64>(INVALID_SET_FILE_POINTER));
        return;
    }
    if (high_ptr == 0 && *new_pos > 0xFFFFFFFFull) {
        if (prior_pos.has_value()) {
            (void)fm.set_file_pointer64(handle, static_cast<i64>(*prior_pos),
                                        0 /*FILE_BEGIN*/);
        }
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(static_cast<u64>(INVALID_SET_FILE_POINTER));
        return;
    }
    const u32 low = static_cast<u32>(*new_pos);
    distance_high = static_cast<i32>(*new_pos >> 32);

    if (debug::is_watched(handle)) {
        const char* path = debug::watched_path(handle);
        std::fprintf(stderr,
                     "[watch] SetFilePointer(handle=0x%x, distance=%d, method=%u) path=%s -> 0x%x\n",
                     static_cast<unsigned>(handle), distance, move_method,
                     path ? path : "?", low);
    }

    static const bool log_io_s = std::getenv("VITREO_LOG_FILEIO") != nullptr;
    if (log_io_s) {
        std::fprintf(stderr,
                     "[fileio] ret=0x%llx SetFilePointer(h=0x%x dist=%d method=%u)"
                     " -> 0x%x\n",
                     static_cast<unsigned long long>(m.guest_return_address()),
                     static_cast<unsigned>(handle), distance, move_method, low);
    }

    if (high_ptr != 0 && distance_high_ptr != nullptr) {
        (void)m.write_u32(high_ptr, static_cast<u32>(distance_high));
    }

    // Measured on Windows 11 (last_error_on_success_probe.expected): success
    // leaves the last error untouched unless the returned low half collides
    // with INVALID_SET_FILE_POINTER, in which case it is NO_ERROR.
    if (low == INVALID_SET_FILE_POINTER) {
        set_last_error(error_codes::ERROR_SUCCESS);
    }
    m.set_return(static_cast<u64>(low));
    VITREO_TRACE( "[file] SetFilePointer handle=0x%x dist=%d method=%u -> pos=%u\n",
                 static_cast<unsigned>(handle), distance, move_method, low);
}

// =============================================================================
// impl_SetFilePointerEx
// =============================================================================

void impl_SetFilePointerEx(Marshaler& m, FileManager& fm) {
    // BOOL SetFilePointerEx(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD)
    // In x86 (32-bit), LARGE_INTEGER occupies TWO stack slots (8 bytes).
    // In x64, it fits in a single 64-bit register (RDX), so arg(1) is
    // already the full 8-byte value and arg(2)/arg(3) advance normally.
    Handle handle;
    i64 distance;
    GuestAddr new_ptr;
    u32 move_method;

    if (m.cpu() && m.cpu()->is_64bit()) {
        handle      = static_cast<Handle>(m.arg(0));
        distance    = static_cast<i64>(m.arg(1));
        new_ptr     = m.arg_ptr(2);
        move_method = static_cast<u32>(m.arg(3));
    } else {
        // x86: arg(1) = low 32 bits, arg(2) = high 32 bits of LARGE_INTEGER
        handle = static_cast<Handle>(m.arg(0));
        u32 dist_lo = static_cast<u32>(m.arg(1));
        u32 dist_hi = static_cast<u32>(m.arg(2));
        distance    = static_cast<i64>(
                          (static_cast<u64>(dist_hi) << 32)
                          | static_cast<u64>(dist_lo));
        new_ptr     = static_cast<GuestAddr>(m.arg(3));
        move_method = static_cast<u32>(m.arg(4));
    }

    // The 64-bit seek keeps a position whose low half is 0xFFFFFFFF from
    // reading as failure.
    if (fm.vfs_handle_for(handle) == capsule::INVALID_FILE_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    if (move_method > 2) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const auto moved = fm.set_file_pointer64(handle, distance, move_method);
    if (!moved.has_value()) {
        if (get_last_error() != error_codes::ERROR_INVALID_PARAMETER) {
            set_last_error(error_codes::ERROR_NEGATIVE_SEEK);
        }
        m.set_return(0);
        return;
    }
    const u64 new_pos = *moved;

    if (new_ptr != 0) {
        (void)m.write_u64(new_ptr, new_pos);
    }

    m.set_return(1);
    VITREO_TRACE("[file] SetFilePointerEx handle=0x%x dist=%lld method=%u -> pos=%llu\n",
                 static_cast<unsigned>(handle),
                 static_cast<long long>(distance), move_method,
                 static_cast<unsigned long long>(new_pos));
}

// =============================================================================
// Export Registration
// =============================================================================

DllExports make_filepos_exports(FileManager& fm, NtdllExtManager* nt_files) {
    DllExports exports;
    exports.entries.reserve(4);

    exports.entries.push_back(ExportEntry{
        "GetFileSize", 1,
        [&fm, nt_files](Marshaler& m) {
            impl_GetFileSize(m, fm, nt_files);
        },
        8
    });

    exports.entries.push_back(ExportEntry{
        "GetFileSizeEx", 2,
        [&fm, nt_files](Marshaler& m) {
            impl_GetFileSizeEx(m, fm, nt_files);
        },
        8
    });

    exports.entries.push_back(ExportEntry{
        "SetFilePointer", 3,
        [&fm](Marshaler& m) { impl_SetFilePointer(m, fm); },
        16
    });

    exports.entries.push_back(ExportEntry{
        "SetFilePointerEx", 4,
        [&fm](Marshaler& m) { impl_SetFilePointerEx(m, fm); },
        20
    });

    return exports;
}

} // namespace vitreo::winapi
