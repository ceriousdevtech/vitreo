/// @file ntdll_ext.cpp
/// @brief ntdll.dll extended function stubs (WIN32-086)

#include "winapi/system/ntdll/nt_query_virtual_memory.hpp"
#include "core/pe/os_version.hpp"
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
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/common/host_yield.hpp"
#include "winapi/system/ntdll/nt_timeout.hpp"
#include "winapi/common/write_sha1_debug.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "ipc/ipc_gate.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <cstring>
#include <vector>
#include <cerrno>
#include <unistd.h>
#include <sys/time.h>

namespace vitreo::winapi {

void impl_RtlCaptureStackBackTrace(Marshaler& m) {
    const u32 frames_to_skip = static_cast<u32>(m.arg(0));
    const u32 requested = std::min<u32>(static_cast<u32>(m.arg(1)), 0xffffu);
    const GuestAddr back_trace = m.arg_ptr(2);
    const GuestAddr hash_ptr = m.arg_ptr(3);
    auto* cpu = m.cpu();
    u32 captured = 0;
    u32 visited = 0;
    u32 hash = 0;

    auto record = [&](u64 return_address) {
        if (return_address == 0) return false;
        if (visited++ < frames_to_skip) return true;
        if (captured >= requested || back_trace == 0) return false;
        const GuestAddr slot = back_trace +
            static_cast<GuestAddr>(captured) *
                (cpu->is_64bit() ? sizeof(u64) : sizeof(u32));
        auto write = cpu->is_64bit()
            ? m.write_u64(slot, return_address)
            : m.write_u32(slot, static_cast<u32>(return_address));
        if (!write.is_ok()) return false;
        hash += static_cast<u32>(return_address);
        ++captured;
        return captured < requested;
    };

    if (cpu != nullptr && requested != 0 && back_trace != 0) {
        const bool is_64bit = cpu->is_64bit();
        const GuestAddr stack = static_cast<GuestAddr>(cpu->sp());
        const u64 caller = is_64bit
            ? m.read_u64(stack).value_or(0)
            : m.read_u32(stack).value_or(0);
        bool keep_walking = record(caller);
        u64 frame = cpu->gpr.rbp;
        const u64 alignment = is_64bit ? alignof(u64) : alignof(u32);
        while (keep_walking && frame >= 0x10000) {
            const auto previous = m.read_ptr(static_cast<GuestAddr>(frame));
            const auto return_address = m.read_ptr(
                static_cast<GuestAddr>(frame +
                    (is_64bit ? sizeof(u64) : sizeof(u32))));
            if (!previous.is_ok() || !return_address.is_ok()) break;
            keep_walking = record(return_address.value());
            const u64 next = previous.value();
            if (next <= frame || next - frame > 16 * 1024 * 1024 ||
                (next & (alignment - 1)) != 0) {
                break;
            }
            frame = next;
        }
    }
    if (hash_ptr != 0) (void)m.write_u32(hash_ptr, hash);
    VITREO_TRACE(
        "[ntdll] RtlCaptureStackBackTrace skip=%u requested=%u -> %u hash=0x%08x\n",
        frames_to_skip, requested, captured, hash);
    m.set_return(captured);
}


// =============================================================================
// RtlNtStatusToDosError
// =============================================================================

u32 NtdllExtManager::nt_status_to_dos_error(u32 status) const {
    // Facility 7 carries an existing Win32 error in the low word, in both
    // the HRESULT warning-severity form and the NT error-severity form.
    const u32 facility_prefix = status & 0xFFFF0000u;
    if (facility_prefix == 0x80070000u || facility_prefix == 0xC0070000u)
        return status & 0xFFFFu;
    switch (status) {
        case nt_status::STATUS_SUCCESS:              return 0;   // ERROR_SUCCESS
        case nt_status::STATUS_INVALID_HANDLE:       return 6;   // ERROR_INVALID_HANDLE
        case nt_status::STATUS_INVALID_PARAMETER:    return 87;  // ERROR_INVALID_PARAMETER
        case nt_status::STATUS_ACCESS_DENIED:        return 5;   // ERROR_ACCESS_DENIED
        case nt_status::STATUS_OBJECT_NAME_NOT_FOUND:return 2;   // ERROR_FILE_NOT_FOUND
        case nt_status::STATUS_NO_MEMORY:            return 8;   // ERROR_NOT_ENOUGH_MEMORY
        case nt_status::STATUS_BUFFER_TOO_SMALL:     return 122; // ERROR_INSUFFICIENT_BUFFER
        case nt_status::STATUS_INFO_LENGTH_MISMATCH: return 24;  // ERROR_BAD_LENGTH
        case 0xC000014Bu:                            return 109; // STATUS_PIPE_BROKEN -> ERROR_BROKEN_PIPE
        case 0xC0000120u:                            return 995; // STATUS_CANCELLED -> ERROR_OPERATION_ABORTED
        default:                                     return 317; // ERROR_MR_MID_NOT_FOUND
    }
}

// =============================================================================
// RtlGetVersion
// =============================================================================

NtdllExtManager::VersionInfo NtdllExtManager::get_version() const {
    return VersionInfo{pe::kOsMajorVersion, pe::kOsMinorVersion, pe::kOsBuildNumber, pe::kOsPlatformId};
}

// =============================================================================
// NtQueryVirtualMemory
// =============================================================================

u32 NtdllExtManager::query_virtual_memory(u64 address, MemoryBasicInfo& info) const {
    // Addresses above 0xFFFF0000 are treated as free,
    // everything else is a committed private region.
    constexpr u64 HIGH_ADDR_THRESHOLD = 0xFFFF0000ULL;

    if (address >= HIGH_ADDR_THRESHOLD) {
        info.base_address = address & ~0xFFFULL;  // page-align
        info.region_size  = 0x1000;
        info.state        = 0x10000;  // MEM_FREE
        info.protect      = 0x01;     // PAGE_NOACCESS
        info.type         = 0;
    } else {
        info.base_address = address & ~0xFFFULL;  // page-align
        info.region_size  = 0x1000;
        info.state        = 0x1000;   // MEM_COMMIT
        info.protect      = 0x04;     // PAGE_READWRITE
        info.type         = 0x20000;  // MEM_PRIVATE
    }

    return nt_status::STATUS_SUCCESS;
}



// =============================================================================
// NtDelayExecution
// =============================================================================

u32 NtdllExtManager::nt_delay_execution(bool alertable, i64 delay_100ns) const {
    // NTSTATUS NtDelayExecution(BOOLEAN Alertable, PLARGE_INTEGER DelayInterval)
    // DelayInterval follows the NT timeout convention (nt_timeout.hpp): a
    // negative interval, or an absolute system time that tracks clock
    // changes. Every status below is nt_alert_wait_probe.expected
    // "-- delay_execution".
    const nt_timeout::Deadline deadline = nt_timeout::Deadline::from_value(delay_100ns);
    SyncManager* sync = m_sync_manager;
    // An alertable delay delivers queued user APCs and returns
    // STATUS_USER_APC, at once when one is already queued
    // (delay_alertable_queued_apc, delay_zero_alertable_queued_apc). A
    // non-alertable delay leaves them queued (delay_nonalertable_queued_apc).
    u64 apc_generation = (alertable && sync != nullptr) ? sync->apc_generation() : 0;
    auto deliver = [&] {
        return alertable && sync != nullptr && sync->try_drain_alertable_apcs();
    };
    if (deliver()) return nt_status::STATUS_USER_APC;

    if (deadline.is_zero()) {
        // A zero delay yields the rest of the quantum: STATUS_SUCCESS when
        // another thread ran, STATUS_NO_YIELD_PERFORMED when none was ready
        // (delay_zero). The host cannot report whether its yield switched;
        // as SwitchToThread does, "another live guest thread exists" stands
        // in for "another thread was ready".
        const u32 self = ThreadManager::get_current_thread_id_static();
        const bool others = m_thread_manager != nullptr &&
                            m_thread_manager->any_live_thread_except(self);
        host_yield_timeslice();
        return others ? nt_status::STATUS_SUCCESS
                      : nt_status::STATUS_NO_YIELD_PERFORMED;
    }

    // An alert-by-thread-id does not end a delay and stays pending for the
    // next NtWaitForAlertByThreadId (delay_alertable_then_alert_by_tid): the
    // two are separate, and nothing here consumes the alert.
    for (;;) {
        const i64 slice_ns = deadline.next_slice_ns();
        if (slice_ns == 0) return nt_status::STATUS_SUCCESS;
        if (alertable && sync != nullptr) {
            // Woken early by note_apc_queued (any thread's APC); deliver ours.
            constexpr u32 kParkCapMs = 3'600'000u;
            const u32 ms = slice_ns < 0
                ? kParkCapMs
                : static_cast<u32>(std::min<u64>(
                      (static_cast<u64>(slice_ns) + 999'999u) / 1'000'000u, kParkCapMs));
            if (sync->alertable_sleep(ms, apc_generation) && deliver()) {
                return nt_status::STATUS_USER_APC;
            }
            continue;
        }
        // Non-alertable: sleep the slice precisely. nanosleep returns early
        // on EINTR (SIGCHLD); NtDelayExecution does not, so the loop resumes.
        constexpr i64 kHourNs = 3'600'000'000'000LL;
        const i64 ns = slice_ns < 0 ? kHourNs : std::min(slice_ns, kHourNs);
        timespec ts{};
        ts.tv_sec = static_cast<time_t>(ns / 1'000'000'000LL);
        ts.tv_nsec = static_cast<long>(ns % 1'000'000'000LL);
        (void)::nanosleep(&ts, nullptr);
    }
}

u32 NtdllExtManager::nt_wait_for_single_object(
    Handle handle, bool alertable, const nt_timeout::Deadline& deadline) const {
    // NTSTATUS NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable,
    //                                PLARGE_INTEGER Timeout)
    // Every status below is nt_wait_single_probe.expected (x64 and WoW64
    // agree). SyncManager does the wait itself: it acquires what a wait
    // acquires (an auto-reset event, a semaphore count, mutex ownership)
    // and rejects a handle without SYNCHRONIZE access.
    SyncManager* sync = m_sync_manager;
    if (sync == nullptr) return nt_status::STATUS_INVALID_HANDLE;

    // An alertable wait runs a queued user APC and returns STATUS_USER_APC,
    // even when the object is already signaled ("alertable, apc queued,
    // signaled"); a non-alertable wait leaves the APC queued.
    if (alertable && sync->try_drain_alertable_apcs()) {
        return nt_status::STATUS_USER_APC;
    }
    // A native service leaves the thread's last error alone, and SyncManager
    // reports why a wait failed through it.
    const u32 caller_error = get_last_error();
    const u64 apc_object =
        alertable ? sync->apc_wait_object_for_current_thread() : 0;
    for (;;) {
        // A zero or past time-out still tests the object once ("auto
        // signaled, zero timeout" acquires it). An absolute time-out parks
        // in slices so a clock change is noticed (nt_timeout.hpp).
        const u32 slice_ms = deadline.next_slice_ms();
        const u32 result =
            alertable
                ? sync->wait_for_multiple_objects(1, &handle, false, slice_ms,
                                                  apc_object)
                : sync->wait_for_single_object(handle, slice_ms);
        if (result == WAIT_OBJECT_0 + 1 && alertable) {
            if (sync->try_drain_alertable_apcs()) {
                set_last_error(caller_error);
                return nt_status::STATUS_USER_APC;
            }
            continue;
        }
        if (result == WAIT_TIMEOUT && !deadline.expired()) continue;
        const u32 failure = get_last_error();
        set_last_error(caller_error);
        switch (result) {
        case WAIT_OBJECT_0:
            return nt_status::STATUS_SUCCESS;
        case WAIT_ABANDONED:
            return nt_status::STATUS_ABANDONED_WAIT_0;
        case WAIT_TIMEOUT:
            return nt_status::STATUS_TIMEOUT;
        default:
            break;
        }
        // An invalid or NULL handle is STATUS_INVALID_HANDLE; one opened
        // without SYNCHRONIZE is STATUS_ACCESS_DENIED.
        if (failure == error_codes::ERROR_ACCESS_DENIED) {
            return nt_status::STATUS_ACCESS_DENIED;
        }
        const HandleType type = m_handles != nullptr
                                    ? m_handles->get_type(handle)
                                    : HandleType::Invalid;
        if (type == HandleType::Invalid) {
            return nt_status::STATUS_INVALID_HANDLE;
        }
        // A live handle SyncManager cannot wait on. Windows waits on a file
        // (signaled while no I/O is in flight: "file handle" is
        // STATUS_SUCCESS) and refuses a key or section opened without
        // SYNCHRONIZE (STATUS_ACCESS_DENIED); SyncManager models neither.
        static std::atomic<u32> reported_types{0};
        const u32 bit = 1u << (static_cast<u32>(type) & 31u);
        if ((reported_types.fetch_or(bit) & bit) == 0) {
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED ntdll!NtWaitForSingleObject: "
                         "%s handle\n",
                         handle_type_to_string(type));
        }
        return nt_status::STATUS_NOT_IMPLEMENTED;
    }
}

// =============================================================================
// SetEndOfFile (via NtSetInformationFile semantics)
// =============================================================================

u32 NtdllExtManager::nt_set_end_of_file(Handle handle) {
    if (!has_file_support()) {
        return nt_status::STATUS_NOT_IMPLEMENTED;
    }

    auto type = m_handles->get_type(handle);
    if (type == HandleType::Invalid) {
        return nt_status::STATUS_INVALID_HANDLE;
    }

    const auto mapped_handle = vfs_handle_for(handle);
    if (!mapped_handle.has_value()) {
        return nt_status::STATUS_INVALID_HANDLE;
    }
    const capsule::FileHandle vfs_handle = *mapped_handle;

    // Get current file pointer position
    auto pos_result = m_vfs->seek(vfs_handle, 0, capsule::SeekOrigin::Current);
    if (pos_result.is_err()) {
        return nt_status::STATUS_ACCESS_DENIED;
    }

    u64 current_pos = pos_result.value();

    // Truncate/extend to current position
    auto trunc_result = m_vfs->truncate(vfs_handle, current_pos);
    if (trunc_result.is_err()) {
        return nt_status::STATUS_ACCESS_DENIED;
    }

    VITREO_TRACE("[ntdll] SetEndOfFile: handle=0x%x -> size=%llu\n",
                handle, static_cast<unsigned long long>(current_pos));
    return nt_status::STATUS_SUCCESS;
}



// =============================================================================
// Rtl Memory Operations
// =============================================================================

void NtdllExtManager::rtl_copy_memory(void* dst, const void* src, usize length) {
    std::memcpy(dst, src, length);
}

void NtdllExtManager::rtl_zero_memory(void* dst, usize length) {
    std::memset(dst, 0, length);
}

void NtdllExtManager::rtl_move_memory(void* dst, const void* src, usize length) {
    std::memmove(dst, src, length);
}

void NtdllExtManager::rtl_fill_memory(void* dst, usize length, u8 fill) {
    std::memset(dst, fill, length);
}

// =============================================================================
// Thunk Implementations
// =============================================================================

void ntdll::impl_RtlNtStatusToDosError(Marshaler& m, NtdllExtManager& mgr) {
    const u32 status = static_cast<u32>(m.arg(0));

    const u32 dos_error = mgr.nt_status_to_dos_error(status);
    // Every status, success included, becomes the thread's
    // TEB.LastStatusValue (RtlGetLastNtStatus); LastErrorValue is untouched
    // and the NoTeb variant stores nothing (rtl_string_status_probe).
    if (const GuestAddr teb = current_teb(m); teb != 0) {
        (void)m.write_u32(teb + teb_last_status_offset(m), status);
    }
    m.set_return(static_cast<u64>(dos_error));
}

void ntdll::impl_RtlGetVersion(Marshaler& m, NtdllExtManager& mgr) {
    // NTSTATUS RtlGetVersion(PRTL_OSVERSIONINFOW lpVersionInformation)
    const GuestAddr buf_addr = m.arg_ptr(0);

    if (buf_addr == 0) {
        m.set_return(static_cast<u64>(nt_status::STATUS_INVALID_PARAMETER));
        return;
    }

    auto ver = mgr.get_version();
    (void)m.write_u32(buf_addr + 4,  ver.major);
    (void)m.write_u32(buf_addr + 8,  ver.minor);
    (void)m.write_u32(buf_addr + 12, ver.build);
    (void)m.write_u32(buf_addr + 16, ver.platform_id);

    // szCSDVersion — 128 wchars (256 bytes) at offset +20.
    // Write empty service-pack string (just a null terminator).
    (void)m.write_u16(buf_addr + 20, 0);

    // Learn RtlGetVersion: an RTL_OSVERSIONINFOEXW (dwOSVersionInfoSize 284)
    // also receives wServicePackMajor/Minor (+276/+278), wSuiteMask (+280)
    // and wProductType (+282) (verify_version_info_probe: real_sp=0.0
    // real_suite=0x100 real_product=1).
    constexpr u32 kRtlOsVersionInfoExWSize = 284;
    if (m.read_u32(buf_addr).value_or(0) == kRtlOsVersionInfoExWSize) {
        (void)m.write_u16(buf_addr + 276, pe::kOsServicePackMajor);
        (void)m.write_u16(buf_addr + 278, pe::kOsServicePackMinor);
        (void)m.write_u16(buf_addr + 280, pe::kOsSuiteMask);
        (void)m.write_u8(buf_addr + 282, pe::kOsProductType);
        (void)m.write_u8(buf_addr + 283, 0);
    }

    m.set_return(static_cast<u64>(nt_status::STATUS_SUCCESS));
}

void ntdll::impl_RtlQueryElevationFlags(Marshaler& m) {
    // NTSTATUS RtlQueryElevationFlags(PRTL_ELEVATION_FLAGS Flags)
    //
    // This is system UAC-policy state, not the current process's elevated
    // token state. Bit 0 means elevation is enabled; bits 1 and 2 describe
    // legacy virtualization and automatic installer detection respectively.
    // Vitreo provides an explicit manifest/runas elevation broker, but does
    // not currently model Windows VirtualStore or heuristic installer
    // detection, so only the first capability is advertised.
    const GuestAddr flags = m.arg_ptr(0);
    if (flags == 0 || m.write_u32(flags, 0x1u).is_err()) {
        m.set_return(static_cast<u64>(nt_status::STATUS_INVALID_PARAMETER));
        return;
    }
    VITREO_TRACE("[ntdll] RtlQueryElevationFlags -> ELEVATION_ENABLED\n");
    m.set_return(static_cast<u64>(nt_status::STATUS_SUCCESS));
}

void ntdll::impl_NtQueryVirtualMemory(Marshaler& m, NtdllExtManager& mgr) {
    (void)mgr;
    query_virtual_memory(m, query_virtual_memory_host());
}

// Layout of SYSTEM_PROCESS_INFORMATION (32-bit, packed for guest memory).
// Only the fields that nsProcess::_FindProcess actually reads are meaningful;
// the rest are zeroed.
#pragma pack(push, 1)
struct GuestSystemProcessInfo {
    u32 NextEntryOffset;       // 0x00  offset to next entry (0 = last)
    u32 NumberOfThreads;       // 0x04
    u8  Reserved1[24];         // 0x08  WorkingSetPrivateSize..Reserved
    i64 CreateTime;            // 0x20
    i64 UserTime;              // 0x28
    i64 KernelTime;            // 0x30
    // UNICODE_STRING ImageName (4+4 bytes on 32-bit)
    u16 ImageName_Length;      // 0x38  byte length (excl NUL)
    u16 ImageName_MaxLength;   // 0x3A  byte length (incl NUL pair)
    u32 ImageName_Buffer;      // 0x3C  guest pointer to wide string
    i32 BasePriority;          // 0x40
    u32 UniqueProcessId;       // 0x44
    u32 InheritedFromUniqueProcessId; // 0x48
    u32 HandleCount;           // 0x4C
    u32 SessionId;             // 0x50
    u32 UniqueProcessKey;      // 0x54
    u32 PeakVirtualSize;       // 0x58
    u32 VirtualSize;           // 0x5C
    u32 PageFaultCount;        // 0x60
    u32 PeakWorkingSetSize;    // 0x64
    u32 WorkingSetSize;        // 0x68
    // ... more fields follow in real Windows, but they're not read by
    // nsProcess.  We only need to emit enough to reach ImageName/UniqueProcessId.
};
#pragma pack(pop)

static_assert(offsetof(GuestSystemProcessInfo, ImageName_Length) == 0x38,
              "ImageName must sit at offset 0x38");
static_assert(offsetof(GuestSystemProcessInfo, UniqueProcessId) == 0x44,
              "UniqueProcessId must sit at offset 0x44");

void ntdll::impl_NtQuerySystemInformation(Marshaler& m, NtdllExtManager& mgr) {
    const u32 info_class = static_cast<u32>(m.arg(0));
    const GuestAddr buf_addr = static_cast<GuestAddr>(m.arg(1));
    const u32 buf_len  = static_cast<u32>(m.arg(2));
    const GuestAddr ret_len_addr = static_cast<GuestAddr>(m.arg(3));

    // --- SystemProcessInformation (class 5) ---
    // Return a minimal but valid process list so generic enumeration code
    // (process enumeration, anti-debug heuristics, "is X already running?"
    // probes) can walk it correctly.
    //
    // We list four entries (no app-specific names):
    //   - Idle (PID 0)
    //   - System (PID 4)
    //   - vitreo (PID 1000)            -- the emulator host
    //   - <main image> (PID 1001)      -- the current guest process,
    //                                    named after the actual loaded .exe
    //   - explorer.exe (PID 1003)
    //
    // We deliberately do NOT include duplicates of the current image under
    // a different PID; if a guest probes for "is another instance of me
    // running?", it must find exactly one match (itself) and conclude no.
    if (info_class == 5) {
        // Use explicit char16_t arrays since host wchar_t is 4 bytes on macOS
        // but the guest expects UTF-16LE (2 bytes per character).
        static const char16_t kIdleName[]   = u"System Idle Process";
        static const char16_t kSystemName[] = u"System";
        static const char16_t kVitreoName[] = u"vitreo.exe";
        static const char16_t kExplorerName[] = u"explorer.exe";

        // Build a UTF-16 buffer for the current main image filename.
        // Default to "process.exe" if not set (no app coupling).
        std::u16string main_name_utf16;
        {
            const std::string& src = mgr.main_image_name().empty()
                                        ? std::string("process.exe")
                                        : mgr.main_image_name();
            main_name_utf16.reserve(src.size());
            for (char c : src) main_name_utf16.push_back(static_cast<char16_t>(static_cast<u8>(c)));
            main_name_utf16.push_back(u'\0');
        }
        const char16_t* kMainName = main_name_utf16.c_str();
        const u32 kMainChars = static_cast<u32>(main_name_utf16.size());

        constexpr u32 kIdleChars   = sizeof(kIdleName) / sizeof(char16_t);
        constexpr u32 kSystemChars = sizeof(kSystemName) / sizeof(char16_t);
        constexpr u32 kVitreoChars = sizeof(kVitreoName) / sizeof(char16_t);
        constexpr u32 kExplorerChars = sizeof(kExplorerName) / sizeof(char16_t);
        constexpr u32 kIdleNameBytes   = kIdleChars * 2;
        constexpr u32 kSystemNameBytes = kSystemChars * 2;
        constexpr u32 kVitreoNameBytes = kVitreoChars * 2;
        const u32 kMainNameBytes  = kMainChars * 2;
        constexpr u32 kExplorerNameBytes = kExplorerChars * 2;

        constexpr u32 kEntry0Raw = static_cast<u32>(sizeof(GuestSystemProcessInfo)) + kIdleNameBytes;
        constexpr u32 kEntry0    = (kEntry0Raw + 7u) & ~7u;
        constexpr u32 kEntry1Raw = static_cast<u32>(sizeof(GuestSystemProcessInfo)) + kSystemNameBytes;
        constexpr u32 kEntry1    = (kEntry1Raw + 7u) & ~7u;
        constexpr u32 kEntry2Raw = static_cast<u32>(sizeof(GuestSystemProcessInfo)) + kVitreoNameBytes;
        constexpr u32 kEntry2    = (kEntry2Raw + 7u) & ~7u;
        const u32 kEntry3Raw = static_cast<u32>(sizeof(GuestSystemProcessInfo)) + kMainNameBytes;
        const u32 kEntry3    = (kEntry3Raw + 7u) & ~7u;
        constexpr u32 kEntry4Raw = static_cast<u32>(sizeof(GuestSystemProcessInfo)) + kExplorerNameBytes;
        constexpr u32 kEntry4    = (kEntry4Raw + 7u) & ~7u;
        const u32 kTotalSize = kEntry0 + kEntry1 + kEntry2 + kEntry3 + kEntry4;

        // Write return length
        if (ret_len_addr != 0) {
            (void)m.write_u32(static_cast<GuestAddr>(ret_len_addr), kTotalSize);
        }

        if (buf_len < kTotalSize || buf_addr == 0) {
            m.set_return(static_cast<u64>(nt_status::STATUS_INFO_LENGTH_MISMATCH));
            return;
        }

        // Build entry 0: System Idle Process (PID 0)
        GuestSystemProcessInfo e0{};
        std::memset(&e0, 0, sizeof(e0));
        e0.NextEntryOffset = kEntry0;
        e0.UniqueProcessId = 0;
        e0.BasePriority    = 0;
        e0.ImageName_Length    = static_cast<u16>(kIdleNameBytes - 2);  // excl NUL pair
        e0.ImageName_MaxLength = static_cast<u16>(kIdleNameBytes);
        e0.ImageName_Buffer    = static_cast<u32>(buf_addr + sizeof(GuestSystemProcessInfo));

        (void)m.write_struct(buf_addr, e0);
        // Write the UTF-16LE name right after the struct
        GuestAddr name0_addr = buf_addr + sizeof(GuestSystemProcessInfo);
        for (u32 i = 0; i < kIdleChars; ++i) {
            (void)m.write_u16(name0_addr + i * 2, static_cast<u16>(kIdleName[i]));
        }

        // Build entry 1: System (PID 4)
        GuestAddr entry1_addr = buf_addr + kEntry0;
        GuestSystemProcessInfo e1{};
        std::memset(&e1, 0, sizeof(e1));
        e1.NextEntryOffset = kEntry1;
        e1.UniqueProcessId = 4;
        e1.BasePriority    = 8;
        e1.ImageName_Length    = static_cast<u16>(kSystemNameBytes - 2);
        e1.ImageName_MaxLength = static_cast<u16>(kSystemNameBytes);
        e1.ImageName_Buffer    = static_cast<u32>(entry1_addr + sizeof(GuestSystemProcessInfo));

        (void)m.write_struct(entry1_addr, e1);
        GuestAddr name1_addr = entry1_addr + sizeof(GuestSystemProcessInfo);
        for (u32 i = 0; i < kSystemChars; ++i) {
            (void)m.write_u16(name1_addr + i * 2, static_cast<u16>(kSystemName[i]));
        }

        // Build entry 2: vitreo.exe (PID 1000)
        GuestAddr entry2_addr = entry1_addr + kEntry1;
        GuestSystemProcessInfo e2{};
        std::memset(&e2, 0, sizeof(e2));
        e2.NextEntryOffset = kEntry2;
        e2.UniqueProcessId = 1000;
        e2.InheritedFromUniqueProcessId = 4;
        e2.BasePriority = 8;
        e2.ImageName_Length = static_cast<u16>(kVitreoNameBytes - 2);
        e2.ImageName_MaxLength = static_cast<u16>(kVitreoNameBytes);
        e2.ImageName_Buffer = static_cast<u32>(entry2_addr + sizeof(GuestSystemProcessInfo));

        (void)m.write_struct(entry2_addr, e2);
        GuestAddr name2_addr = entry2_addr + sizeof(GuestSystemProcessInfo);
        for (u32 i = 0; i < kVitreoChars; ++i) {
            (void)m.write_u16(name2_addr + i * 2, static_cast<u16>(kVitreoName[i]));
        }

        // Build entry 3: <main image> (PID 1001) — current guest process.
        GuestAddr entry3_addr = entry2_addr + kEntry2;
        GuestSystemProcessInfo e3{};
        std::memset(&e3, 0, sizeof(e3));
        e3.NextEntryOffset = kEntry3;
        e3.UniqueProcessId = 1001;
        e3.InheritedFromUniqueProcessId = 1000;
        e3.BasePriority = 8;
        e3.ImageName_Length = static_cast<u16>(kMainNameBytes - 2);
        e3.ImageName_MaxLength = static_cast<u16>(kMainNameBytes);
        e3.ImageName_Buffer = static_cast<u32>(entry3_addr + sizeof(GuestSystemProcessInfo));

        (void)m.write_struct(entry3_addr, e3);
        GuestAddr name3_addr = entry3_addr + sizeof(GuestSystemProcessInfo);
        for (u32 i = 0; i < kMainChars; ++i) {
            (void)m.write_u16(name3_addr + i * 2, static_cast<u16>(kMainName[i]));
        }

        // Build entry 4: explorer.exe (PID 1003)
        GuestAddr entry4_addr = entry3_addr + kEntry3;
        GuestSystemProcessInfo e4{};
        std::memset(&e4, 0, sizeof(e4));
        e4.NextEntryOffset = 0;
        e4.UniqueProcessId = 1003;
        e4.InheritedFromUniqueProcessId = 4;
        e4.BasePriority = 8;
        e4.ImageName_Length = static_cast<u16>(kExplorerNameBytes - 2);
        e4.ImageName_MaxLength = static_cast<u16>(kExplorerNameBytes);
        e4.ImageName_Buffer = static_cast<u32>(entry4_addr + sizeof(GuestSystemProcessInfo));

        (void)m.write_struct(entry4_addr, e4);
        GuestAddr name4_addr = entry4_addr + sizeof(GuestSystemProcessInfo);
        for (u32 i = 0; i < kExplorerChars; ++i) {
            (void)m.write_u16(name4_addr + i * 2, static_cast<u16>(kExplorerName[i]));
        }

        VITREO_TRACE( "[ntdll] NtQuerySystemInformation(SystemProcessInformation): "
                     "wrote %u bytes (5 processes) to guest 0x%x\n",
                     kTotalSize, static_cast<u32>(buf_addr));
        m.set_return(static_cast<u64>(nt_status::STATUS_SUCCESS));
        return;
    }

    // --- Other info classes: delegate to query_system_info ---
    // Classes 0 and 1 are handled; others return STATUS_INVALID_INFO_CLASS.
    u32 return_len = 0;
    const u32 status = mgr.query_system_info(info_class, nullptr, 0, &return_len);
    m.set_return(static_cast<u64>(status));
}

void ntdll::impl_NtQuerySection(Marshaler& m, NtdllExtManager& mgr) {
    constexpr u32 SECTION_BASIC_INFORMATION = 0;
    constexpr u32 SECTION_QUERY = 0x00000001;
    constexpr u32 SEC_FILE = 0x00800000;
    constexpr u32 SEC_COMMIT = 0x08000000;

    const Handle section_handle = static_cast<Handle>(m.arg(0));
    const u32 info_class = static_cast<u32>(m.arg(1));
    const GuestAddr info = m.arg_ptr(2);
    const u32 info_length = static_cast<u32>(m.arg(3));
    const GuestAddr return_length = m.arg_ptr(4);
    const u32 required = m.cpu_is_64bit() ? 24u : 16u;

    if (return_length != 0) {
        (void)m.memory().write_u32(return_length, required);
    }
    if (info_class != SECTION_BASIC_INFORMATION) {
        ipc::ipc_log("NtQuerySection h=0x%x class=%u -> INVALID_INFO_CLASS",
                     static_cast<unsigned>(section_handle), info_class);
        m.set_return(nt_status::STATUS_INVALID_INFO_CLASS);
        return;
    }
    if (info == 0 || info_length < required) {
        ipc::ipc_log(
            "NtQuerySection h=0x%x info=0x%llx length=%u required=%u "
            "-> INFO_LENGTH_MISMATCH",
            static_cast<unsigned>(section_handle),
            static_cast<unsigned long long>(info), info_length, required);
        m.set_return(nt_status::STATUS_INFO_LENGTH_MISMATCH);
        return;
    }

    auto* handles = mgr.handle_table();
    auto* section = handles == nullptr
        ? nullptr
        : handles->get_as<FileMappingObject>(section_handle, HandleType::FileMapping);
    if (section == nullptr) {
        ipc::ipc_log("NtQuerySection h=0x%x -> INVALID_HANDLE",
                     static_cast<unsigned>(section_handle));
        m.set_return(nt_status::STATUS_INVALID_HANDLE);
        return;
    }
    const u32 granted_access = handles->get_access(section_handle);
    if (granted_access != HandleAccess::All &&
        (granted_access & SECTION_QUERY) == 0) {
        ipc::ipc_log("NtQuerySection h=0x%x access=0x%x -> ACCESS_DENIED",
                     static_cast<unsigned>(section_handle), granted_access);
        m.set_return(nt_status::STATUS_ACCESS_DENIED);
        return;
    }

    const u32 allocation_attributes =
        section->file_handle == FILE_MAPPING_INVALID_FILE
            ? SEC_COMMIT
            : SEC_FILE;

    if (m.cpu_is_64bit()) {
        (void)m.memory().write_u64(info, 0);
        (void)m.memory().write_u32(info + 8, allocation_attributes);
        (void)m.memory().write_u32(info + 12, 0);
        (void)m.memory().write_u64(info + 16, section->size);
    } else {
        (void)m.memory().write_u32(info, 0);
        (void)m.memory().write_u32(info + 4, allocation_attributes);
        (void)m.memory().write_u64(info + 8, section->size);
    }
    ipc::ipc_log(
        "NtQuerySection h=0x%x access=0x%x size=%llu attributes=0x%x -> SUCCESS",
        static_cast<unsigned>(section_handle), granted_access,
        static_cast<unsigned long long>(section->size), allocation_attributes);
    m.set_return(nt_status::STATUS_SUCCESS);
}

static bool is_current_process_handle(u64 handle, bool is_64bit) noexcept {
    return handle == (is_64bit ? 0xFFFFFFFFFFFFFFFFULL : 0xFFFFFFFFULL);
}

static u32 map_view_access_for_protection(u32 protection) noexcept {
    constexpr u32 PAGE_READONLY = 0x02;
    constexpr u32 PAGE_READWRITE = 0x04;
    constexpr u32 PAGE_WRITECOPY = 0x08;
    constexpr u32 PAGE_EXECUTE = 0x10;
    constexpr u32 PAGE_EXECUTE_READ = 0x20;
    constexpr u32 PAGE_EXECUTE_READWRITE = 0x40;
    constexpr u32 PAGE_EXECUTE_WRITECOPY = 0x80;
    constexpr u32 FILE_MAP_COPY = 0x00000001;
    constexpr u32 FILE_MAP_WRITE = 0x00000002;
    constexpr u32 FILE_MAP_READ = 0x00000004;
    constexpr u32 FILE_MAP_EXECUTE = 0x00000020;

    switch (protection & 0xFFu) {
        case PAGE_READONLY:
            return FILE_MAP_READ;
        case PAGE_READWRITE:
            return FILE_MAP_WRITE;
        case PAGE_WRITECOPY:
            return FILE_MAP_COPY;
        case PAGE_EXECUTE:
            return FILE_MAP_EXECUTE;
        case PAGE_EXECUTE_READ:
            return FILE_MAP_READ | FILE_MAP_EXECUTE;
        case PAGE_EXECUTE_READWRITE:
            return FILE_MAP_WRITE | FILE_MAP_EXECUTE;
        case PAGE_EXECUTE_WRITECOPY:
            return FILE_MAP_COPY | FILE_MAP_EXECUTE;
        default:
            return 0;
    }
}

static u32 map_view_failure_status() noexcept {
    switch (get_last_error()) {
        case error_codes::ERROR_INVALID_HANDLE:
            return nt_status::STATUS_INVALID_HANDLE;
        case error_codes::ERROR_ACCESS_DENIED:
            return nt_status::STATUS_ACCESS_DENIED;
        case error_codes::ERROR_NOT_ENOUGH_MEMORY:
        case error_codes::ERROR_OUTOFMEMORY:
            return nt_status::STATUS_NO_MEMORY;
        default:
            return nt_status::STATUS_INVALID_PARAMETER;
    }
}

void ntdll::impl_NtCreateSection(Marshaler& m, NtdllExtManager& mgr) {
    // NTSTATUS NtCreateSection(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
    //                          PLARGE_INTEGER MaximumSize, ULONG Protect,
    //                          ULONG AllocationAttributes, HANDLE FileHandle)
    // Learn (ZwCreateSection) documents the parameters. The validation order,
    // statuses and granted rights below are measured, x64 and x86, by
    // tools/oracle_probe/nt_create_section_probe:
    //  * AllocationAttributes names exactly one kind: SEC_IMAGE (or
    //    SEC_IMAGE_NO_EXECUTE) alone, or SEC_COMMIT / SEC_RESERVE with their
    //    modifiers. None, SEC_FILE alone, or SEC_IMAGE with another attribute
    //    is STATUS_INVALID_PARAMETER_6.
    //  * SEC_IMAGE_NO_EXECUTE with an execute protection, PAGE_NOACCESS and
    //    unknown protections are STATUS_INVALID_PAGE_PROTECTION.
    //  * SEC_IMAGE without a file is STATUS_INVALID_FILE_FOR_SECTION; a file
    //    without "MZ" is STATUS_INVALID_IMAGE_NOT_MZ; an empty file as a
    //    whole-file section is STATUS_MAPPED_FILE_SIZE_ZERO; a file opened
    //    without the rights the protection needs is STATUS_ACCESS_DENIED.
    //  * the handle is granted DesiredAccess within SECTION_ALL_ACCESS, with
    //    GENERIC_READ = 0x20005, GENERIC_WRITE = 0x20002, GENERIC_EXECUTE =
    //    0x20008 and GENERIC_ALL / MAXIMUM_ALLOWED = SECTION_ALL_ACCESS.
    constexpr u32 SEC_IMAGE = 0x01000000;
    constexpr u32 SEC_IMAGE_NO_EXECUTE = 0x11000000;
    constexpr u32 SEC_COMMIT = 0x08000000;
    constexpr u32 SEC_RESERVE = 0x04000000;
    constexpr u32 SEC_FILE = 0x00800000;
    constexpr u32 SEC_NOCACHE = 0x10000000;
    constexpr u32 SEC_WRITECOMBINE = 0x40000000;
    constexpr u32 SEC_LARGE_PAGES = 0x80000000;
    constexpr u32 PAGE_READONLY = 0x02;
    constexpr u32 PAGE_READWRITE = 0x04;
    constexpr u32 PAGE_WRITECOPY = 0x08;
    constexpr u32 PAGE_EXECUTE = 0x10;
    constexpr u32 PAGE_EXECUTE_READ = 0x20;
    constexpr u32 PAGE_EXECUTE_READWRITE = 0x40;
    constexpr u32 PAGE_EXECUTE_WRITECOPY = 0x80;
    constexpr u32 OBJ_INHERIT = 0x00000002;
    constexpr u32 STATUS_INVALID_PARAMETER_6 = 0xC00000F4u;
    constexpr u32 STATUS_MAPPED_FILE_SIZE_ZERO = 0xC000011Eu;
    constexpr u32 SECTION_ALL_ACCESS = 0x000F001Fu;
    constexpr u32 GENERIC_READ = 0x80000000u;
    constexpr u32 GENERIC_WRITE = 0x40000000u;
    constexpr u32 GENERIC_EXECUTE = 0x20000000u;
    constexpr u32 GENERIC_ALL = 0x10000000u;
    constexpr u32 MAXIMUM_ALLOWED = 0x02000000u;

    const GuestAddr section_handle_out = m.arg_ptr(0);
    const u32 desired_access = static_cast<u32>(m.arg(1));
    const GuestAddr object_attributes = m.arg_ptr(2);
    const GuestAddr maximum_size_ptr = m.arg_ptr(3);
    const u32 protect = static_cast<u32>(m.arg(4));
    const u32 alloc_attributes = static_cast<u32>(m.arg(5));
    const Handle file_handle = static_cast<Handle>(m.arg(6));

    if (section_handle_out == 0) {
        m.set_return(nt_status::STATUS_INVALID_PARAMETER);
        return;
    }
    auto* file_mapping = mgr.file_mapping_manager();
    if (file_mapping == nullptr) {
        m.set_return(nt_status::STATUS_NOT_SUPPORTED);
        return;
    }

    const bool image = (alloc_attributes & SEC_IMAGE) != 0;
    const bool no_execute = image &&
        (alloc_attributes & SEC_IMAGE_NO_EXECUTE) == SEC_IMAGE_NO_EXECUTE;
    if (image) {
        const u32 kind = no_execute ? SEC_IMAGE_NO_EXECUTE : SEC_IMAGE;
        if ((alloc_attributes & ~kind) != 0) {
            m.set_return(STATUS_INVALID_PARAMETER_6);
            return;
        }
    } else {
        const u32 kinds = alloc_attributes & (SEC_COMMIT | SEC_RESERVE);
        const u32 modifiers = SEC_FILE | SEC_NOCACHE | SEC_WRITECOMBINE | SEC_LARGE_PAGES;
        if ((kinds != SEC_COMMIT && kinds != SEC_RESERVE) ||
            (alloc_attributes & ~(SEC_COMMIT | SEC_RESERVE | modifiers)) != 0) {
            m.set_return(STATUS_INVALID_PARAMETER_6);
            return;
        }
    }

    const u32 page = protect & 0xFFu;
    switch (page) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            break;
        default:
            m.set_return(nt_status::STATUS_INVALID_PAGE_PROTECTION);
            return;
    }
    if (no_execute && page >= PAGE_EXECUTE) {
        m.set_return(nt_status::STATUS_INVALID_PAGE_PROTECTION);
        return;
    }

    const bool file_backed = (file_handle != 0 && file_handle != 0xFFFFFFFFu &&
                              file_handle != static_cast<Handle>(~u64{0}));
    if (!file_backed && image) {
        m.set_return(nt_status::STATUS_INVALID_FILE_FOR_SECTION);
        return;
    }

    u64 max_size = 0;
    if (maximum_size_ptr != 0) {
        auto lo = m.read_u32(maximum_size_ptr);
        auto hi = m.read_u32(maximum_size_ptr + 4);
        if (lo.is_err() || hi.is_err()) {
            m.set_return(nt_status::STATUS_INVALID_PARAMETER);
            return;
        }
        max_size = static_cast<u64>(lo.value()) |
                   (static_cast<u64>(hi.value()) << 32);
    }
    if (!file_backed && max_size == 0) {
        m.set_return(nt_status::STATUS_INVALID_PARAMETER);
        return;
    }

    bool inheritable = false;
    if (object_attributes != 0) {
        // OBJECT_ATTRIBUTES (winternl): Length@0, RootDirectory@ptr,
        // ObjectName@2*ptr, Attributes@3*ptr (ULONG). On x64 Attributes is
        // at 24; on x86 at 12. Misreading this drops OBJ_INHERIT, so a
        // section handle fails UpdateProcThreadAttribute
        // (PROC_THREAD_ATTRIBUTE_HANDLE_LIST).
        const u32 attrs_off = m.cpu_is_64bit() ? 24u : 12u;
        auto attrs = m.read_u32(object_attributes + attrs_off);
        if (attrs.is_ok() && (attrs.value() & OBJ_INHERIT) != 0) {
            inheritable = true;
        }
    }

    FileMappingManager::SectionCreateExtras extras;
    if ((desired_access & (GENERIC_ALL | MAXIMUM_ALLOWED)) != 0) {
        extras.granted_access = SECTION_ALL_ACCESS;
    } else {
        u32 granted = desired_access & SECTION_ALL_ACCESS;
        if ((desired_access & GENERIC_READ) != 0) granted |= 0x00020005u;
        if ((desired_access & GENERIC_WRITE) != 0) granted |= 0x00020002u;
        if ((desired_access & GENERIC_EXECUTE) != 0) granted |= 0x00020008u;
        extras.granted_access = granted;
    }

    // CreateFileMapping's flProtect carries the SEC_* kind; SEC_FILE is
    // implied by a file handle.
    const u32 mapping_protect = protect | (alloc_attributes & ~SEC_FILE);
    const Handle mapping_file =
        file_backed ? file_handle : FILE_MAPPING_INVALID_FILE;
    Handle section = file_mapping->create_file_mapping(
        mapping_file, mapping_protect, max_size, /*name=*/{}, inheritable, &extras);
    if (section == NULL_HANDLE) {
        const u32 gle = get_last_error();
        u32 status = nt_status::STATUS_INVALID_PARAMETER;
        if (gle == error_codes::ERROR_INVALID_HANDLE) {
            status = nt_status::STATUS_INVALID_HANDLE;
        } else if (gle == error_codes::ERROR_ACCESS_DENIED) {
            status = nt_status::STATUS_ACCESS_DENIED;
        } else if (gle == error_codes::ERROR_FILE_INVALID) {
            status = STATUS_MAPPED_FILE_SIZE_ZERO;
        } else if (gle == error_codes::ERROR_BAD_EXE_FORMAT && extras.image_status != 0) {
            status = extras.image_status;
        } else if (gle == error_codes::ERROR_NOT_ENOUGH_MEMORY) {
            status = nt_status::STATUS_NO_MEMORY;
        }
        m.set_return(status);
        return;
    }

    if (m.cpu_is_64bit()) {
        (void)m.write_u64(section_handle_out, section);
    } else {
        (void)m.write_u32(section_handle_out, static_cast<u32>(section));
    }
    ipc::ipc_log(
        "NtCreateSection file=0x%x protect=0x%x attr=0x%x -> section=0x%x",
        static_cast<unsigned>(file_handle), protect, alloc_attributes,
        static_cast<unsigned>(section));
    m.set_return(nt_status::STATUS_SUCCESS);
}

void ntdll::impl_NtMapViewOfSection(Marshaler& m, NtdllExtManager& mgr) {
    constexpr u32 VIEW_SHARE = 1;
    constexpr u32 VIEW_UNMAP = 2;
    constexpr u32 STATUS_NOT_SUPPORTED = 0xC00000BBu;

    const Handle section_handle = static_cast<Handle>(m.arg(0));
    const u64 process_handle = m.arg(1);
    const GuestAddr base_address_ptr = m.arg_ptr(2);
    const u64 zero_bits = m.arg(3);
    const u64 commit_size = m.arg(4);
    const GuestAddr section_offset_ptr = m.arg_ptr(5);
    const GuestAddr view_size_ptr = m.arg_ptr(6);
    const u32 inherit_disposition = static_cast<u32>(m.arg(7));
    const u32 allocation_type = static_cast<u32>(m.arg(8));
    const u32 protection = static_cast<u32>(m.arg(9));

    if (!is_current_process_handle(process_handle, m.cpu_is_64bit())) {
        // Remote section mapping needs ownership of another guest address
        // space, which FileMappingManager does not currently expose.
        m.set_return(STATUS_NOT_SUPPORTED);
        return;
    }
    auto* file_mapping = mgr.file_mapping_manager();
    if (file_mapping == nullptr || base_address_ptr == 0 ||
        view_size_ptr == 0) {
        m.set_return(nt_status::STATUS_INVALID_PARAMETER);
        return;
    }
    if (zero_bits != 0 || commit_size != 0 || allocation_type != 0 ||
        (inherit_disposition != VIEW_SHARE &&
         inherit_disposition != VIEW_UNMAP)) {
        m.set_return(STATUS_NOT_SUPPORTED);
        return;
    }

    auto read_native_size = [&](GuestAddr address, u64& value) {
        if (m.cpu_is_64bit()) {
            auto result = m.read_u64(address);
            if (result.is_err()) return false;
            value = result.value();
            return true;
        }
        auto result = m.read_u32(address);
        if (result.is_err()) return false;
        value = result.value();
        return true;
    };
    auto write_native_size = [&](GuestAddr address, u64 value) {
        return m.cpu_is_64bit()
            ? m.memory().write_u64(address, value).is_ok()
            : m.memory().write_u32(address, static_cast<u32>(value)).is_ok();
    };

    u64 requested_base = 0;
    u64 requested_size = 0;
    if (!read_native_size(base_address_ptr, requested_base) ||
        !read_native_size(view_size_ptr, requested_size)) {
        m.set_return(nt_status::STATUS_INVALID_PARAMETER);
        return;
    }
    // The shared backend currently chooses the guest address. Refuse a fixed
    // address rather than mapping elsewhere and reporting false success.
    if (requested_base != 0) {
        m.set_return(STATUS_NOT_SUPPORTED);
        return;
    }

    u64 section_offset = 0;
    if (section_offset_ptr != 0) {
        auto result = m.read_u64(section_offset_ptr);
        if (result.is_err()) {
            m.set_return(nt_status::STATUS_INVALID_PARAMETER);
            return;
        }
        section_offset = result.value();
    }

    const u32 desired_access = map_view_access_for_protection(protection);
    if (desired_access == 0) {
        m.set_return(nt_status::STATUS_INVALID_PARAMETER);
        return;
    }

    const auto* section = mgr.handle_table() == nullptr
        ? nullptr
        : mgr.handle_table()->get_as<FileMappingObject>(
              section_handle, HandleType::FileMapping);
    // A PAGE_EXECUTE data section has no readable pages: a readable view of
    // it is STATUS_SECTION_PROTECTION (nt_create_section_probe commit_x_rx).
    // An image section's pages take their protections from the image.
    constexpr u32 STATUS_SECTION_PROTECTION = 0xC000004Eu;
    if (section != nullptr && !section->sec_image &&
        (section->protection & 0xFFu) == 0x10u /*PAGE_EXECUTE*/ &&
        (protection & 0xFFu) != 0x10u) {
        m.set_return(STATUS_SECTION_PROTECTION);
        return;
    }

    const GuestAddr base = file_mapping->map_view_of_file(
        section_handle, desired_access, section_offset, requested_size);
    if (base == 0) {
        m.set_return(map_view_failure_status());
        return;
    }
    // ViewSize comes back rounded up to whole pages (nt_create_section_probe
    // commit_ro_rd: a 0x270xxx-byte file maps as 0x271000).
    const u64 unrounded_size = requested_size != 0
        ? requested_size
        : (section != nullptr && section_offset < section->size
               ? section->size - section_offset
               : 0);
    const u64 mapped_size = (unrounded_size + 0xFFFu) & ~u64{0xFFFu};
    if (!write_native_size(base_address_ptr, base) ||
        !write_native_size(view_size_ptr, mapped_size)) {
        (void)file_mapping->unmap_view_of_file(base);
        m.set_return(nt_status::STATUS_INVALID_PARAMETER);
        return;
    }

    VITREO_TRACE(
        "[ntdll] NtMapViewOfSection section=0x%x offset=0x%llx "
        "size=0x%llx protection=0x%x -> base=0x%llx\n",
        static_cast<unsigned>(section_handle),
        static_cast<unsigned long long>(section_offset),
        static_cast<unsigned long long>(mapped_size), protection,
        static_cast<unsigned long long>(base));
    // An image view placed anywhere but its ImageBase succeeds with
    // STATUS_IMAGE_NOT_AT_BASE (Learn ZwMapViewOfSection; measured by
    // nt_create_section_probe: the probe's ntdll copy maps at 0x40000003).
    constexpr u32 STATUS_IMAGE_NOT_AT_BASE = 0x40000003u;
    const bool relocated_image = section != nullptr && section->sec_image &&
                                 section->image_base != base;
    m.set_return(relocated_image ? STATUS_IMAGE_NOT_AT_BASE : nt_status::STATUS_SUCCESS);
}

void ntdll::impl_NtUnmapViewOfSection(Marshaler& m, NtdllExtManager& mgr) {
    constexpr u32 STATUS_NOT_SUPPORTED = 0xC00000BBu;
    const u64 process_handle = m.arg(0);
    const GuestAddr base_address = m.arg_ptr(1);

    if (!is_current_process_handle(process_handle, m.cpu_is_64bit())) {
        m.set_return(STATUS_NOT_SUPPORTED);
        return;
    }
    auto* file_mapping = mgr.file_mapping_manager();
    if (file_mapping == nullptr || base_address == 0) {
        VITREO_TRACE(
            "[ntdll] NtUnmapViewOfSection base=0x%llx -> INVALID_PARAMETER\n",
            static_cast<unsigned long long>(base_address));
        m.set_return(nt_status::STATUS_INVALID_PARAMETER);
        return;
    }
    if (!file_mapping->unmap_view_of_file(base_address)) {
        // Only the failing path used to be silent, which hides exactly the
        // call a packer makes to unmap its own image before remapping a
        // decrypted copy over the same base.
        VITREO_TRACE(
            "[ntdll] NtUnmapViewOfSection base=0x%llx -> FAILED "
            "(no MapViewOfFile view at that base)\n",
            static_cast<unsigned long long>(base_address));
        m.set_return(map_view_failure_status());
        return;
    }

    VITREO_TRACE(
        "[ntdll] NtUnmapViewOfSection base=0x%llx -> STATUS_SUCCESS\n",
        static_cast<unsigned long long>(base_address));
    m.set_return(nt_status::STATUS_SUCCESS);
}

void ntdll::impl_NtClose(Marshaler& m, NtdllExtManager& mgr) {
    const u64 handle = m.arg(0);

    const u32 status = mgr.nt_close(handle);
    m.set_return(static_cast<u64>(status));
}

void ntdll::impl_NtDelayExecution(Marshaler& m, NtdllExtManager& mgr) {
    // BOOLEAN is a UCHAR: only the low byte of the argument slot is it.
    const bool alertable = (static_cast<u8>(m.arg(0)) != 0);
    const GuestAddr delay_ptr = m.arg_ptr(1);
    // A NULL or unreadable DelayInterval is STATUS_ACCESS_VIOLATION, returned
    // rather than raised (delay_null_ptr, delay_bad_ptr).
    if (delay_ptr == 0) {
        m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
        return;
    }
    const auto delay = m.memory().read_u64(delay_ptr);
    if (!delay.is_ok()) {
        m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
        return;
    }
    const u32 status = mgr.nt_delay_execution(alertable, static_cast<i64>(delay.value()));
    m.set_return(static_cast<u64>(status));
}

void ntdll::impl_NtWaitForSingleObject(Marshaler& m, NtdllExtManager& mgr) {
    const Handle handle = static_cast<Handle>(m.arg(0));
    // BOOLEAN is a UCHAR: only the low byte of the argument slot is it.
    const bool alertable = (static_cast<u8>(m.arg(1)) != 0);
    // An unreadable Timeout is STATUS_ACCESS_VIOLATION, returned rather than
    // raised ("bad timeout pointer"); NULL waits without a time-out.
    const auto deadline = nt_timeout::read_timeout(m.memory(), m.arg_ptr(2));
    if (!deadline.has_value()) {
        m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
        return;
    }
    m.set_return(static_cast<u64>(
        mgr.nt_wait_for_single_object(handle, alertable, *deadline)));
}





} // namespace vitreo::winapi
