/// @file ntdll.cpp
/// @brief ntdll.dll core function stubs (WIN32-038)

#include "winapi/system/ntdll/ntdll.hpp"
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/handle.hpp"
#include "ipc/process_handle.hpp"
#include "ipc/ipc_gate.hpp"
#include "winapi/kernel32/synchronization/critical_section.hpp"
#include "winapi/kernel32/thread/thread.hpp"

#include <cstring>
#include <limits>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#include <ctime>
#include <unistd.h>

namespace vitreo::winapi {

// =============================================================================
// NtdllManager Construction
// =============================================================================

NtdllManager::NtdllManager() = default;

// =============================================================================
// RtlInitUnicodeString
// =============================================================================

NtdllManager::UnicodeString NtdllManager::init_unicode_string(
    const char16_t* source,
    u32 source_addr) const
{
    if (source == nullptr) {
        return UnicodeString{0, 0, 0};
    }

    // Count characters (excluding NUL)
    u32 char_count = 0;
    while (source[char_count] != u'\0') {
        ++char_count;
    }

    // Length in bytes (excluding NUL), MaximumLength includes NUL terminator
    const u16 length     = static_cast<u16>(char_count * sizeof(char16_t));
    const u16 max_length = static_cast<u16>(length + sizeof(char16_t));

    return UnicodeString{length, max_length, source_addr};
}

// =============================================================================
// NtQueryInformationProcess
// =============================================================================

u32 NtdllManager::get_peb_address() const noexcept {
    return SYNTHETIC_PEB;
}

u32 NtdllManager::query_basic_info(u32 buffer_size) const {
    if (buffer_size < PROCESS_BASIC_INFO_SIZE) {
        return nt_status::STATUS_INFO_LENGTH_MISMATCH;
    }
    return nt_status::STATUS_SUCCESS;
}

std::optional<NtdllManager::ProcessIdentity>
NtdllManager::process_identity(u64 process_handle) const {
    if (m_handle_table == nullptr ||
        process_handle == 0 ||
        process_handle > static_cast<u64>(std::numeric_limits<Handle>::max())) {
        return std::nullopt;
    }
    const Handle handle = static_cast<Handle>(process_handle);
    auto* process = m_handle_table->get_as<ipc::ProcessObject>(
        handle, HandleType::Process);
    if (process == nullptr) return std::nullopt;

    constexpr u32 PROCESS_QUERY_INFORMATION = 0x00000400;
    constexpr u32 PROCESS_QUERY_LIMITED_INFORMATION = 0x00001000;
    const u32 access = m_handle_table->get_access(handle);
    if (access != HandleAccess::All &&
        (access & (PROCESS_QUERY_INFORMATION |
                   PROCESS_QUERY_LIMITED_INFORMATION)) == 0) {
        return std::nullopt;
    }

    ipc::GuestProcessMetadata metadata;
    if (!process->guest_metadata(metadata)) return std::nullopt;
    return ProcessIdentity{
        metadata.peb_address,
        static_cast<u64>(process->pid),
    };
}

// =============================================================================
// Thunk Implementations
// =============================================================================

void impl_RtlInitUnicodeString(Marshaler& m, NtdllManager& nm) {
    // VOID RtlInitUnicodeString(
    //   PUNICODE_STRING DestinationString,  // arg 0 — guest ptr
    //   PCWSTR          SourceString         // arg 1 — guest ptr
    // );
    const u32 dest_addr = static_cast<u32>(m.arg(0));
    const u32 src_addr  = static_cast<u32>(m.arg(1));

    if (dest_addr == 0) {
        return;
    }

    if (src_addr == 0) {
        // Null source — zero the struct
        (void)m.write_u16(dest_addr,     0);  // Length
        (void)m.write_u16(dest_addr + 2, 0);  // MaximumLength
        (void)m.write_u32(dest_addr + 4, 0);  // Buffer
        return;
    }

    // Read wide string from guest to measure length
    auto str_result = m.read_wide_string(static_cast<GuestAddr>(src_addr));
    if (str_result.is_err()) {
        (void)m.write_u16(dest_addr,     0);
        (void)m.write_u16(dest_addr + 2, 0);
        (void)m.write_u32(dest_addr + 4, 0);
        return;
    }

    // Compute byte lengths from the UTF-8 string (each original char16_t = 2 bytes).
    // The read_wide_string converts to UTF-8, so approximate char count.
    // For correctness we re-walk guest memory to count char16_t units.
    u32 char_count = 0;
    GuestAddr walk = static_cast<GuestAddr>(src_addr);
    for (;;) {
        auto ch = m.read_u16(walk);
        if (ch.is_err() || ch.value() == 0) break;
        ++char_count;
        walk += 2;
    }

    const u16 length     = static_cast<u16>(char_count * 2);
    const u16 max_length = static_cast<u16>(length + 2);

    (void)m.write_u16(dest_addr,     length);
    (void)m.write_u16(dest_addr + 2, max_length);
    (void)m.write_u32(dest_addr + 4, src_addr);
}

void impl_NtQueryInformationProcess(Marshaler& m, NtdllManager& nm) {
    // NTSTATUS NtQueryInformationProcess(
    //   HANDLE           ProcessHandle,            // arg 0
    //   PROCESSINFOCLASS ProcessInformationClass,   // arg 1
    //   PVOID            ProcessInformation,        // arg 2
    //   ULONG            ProcessInformationLength,  // arg 3
    //   PULONG           ReturnLength               // arg 4
    // );
    const u64 process_handle = m.arg(0);
    const u32 info_class = static_cast<u32>(m.arg(1));
    const GuestAddr buf_addr = m.arg_ptr(2);
    const u32 buf_len    = static_cast<u32>(m.arg(3));
    const GuestAddr rlen_addr = m.arg_ptr(4);
    const bool is_64bit = m.cpu_is_64bit();
    constexpr u32 STATUS_ACCESS_VIOLATION = 0xC0000005u;

    const auto set_return_length = [&](u32 length) {
        return rlen_addr == 0 || m.write_u32(rlen_addr, length).is_ok();
    };
    const auto process_peb = [&]() -> GuestAddr {
        if (auto* cpu = m.cpu()) {
            const GuestAddr teb =
                is_64bit ? cpu->seg.gs_desc.base : cpu->seg.fs_desc.base;
            if (teb != 0) {
                if (is_64bit) {
                    auto peb = m.read_u64(teb + 0x60);
                    if (peb.is_ok() && peb.value() != 0) return peb.value();
                } else {
                    auto peb = m.read_u32(teb + 0x30);
                    if (peb.is_ok() && peb.value() != 0) return peb.value();
                }
            }
        }
        return nm.get_peb_address();
    };
    const bool current_process =
        process_handle == ~u64{0} ||
        process_handle == static_cast<u64>(static_cast<u32>(-1));

    switch (info_class) {
    case 0: { // ProcessBasicInformation
        // PROCESS_BASIC_INFORMATION is 48 bytes under the Windows x64 ABI
        // (four pointer-sized fields plus natural alignment), and 24 bytes
        // for x86. A sandbox bootstrap requests the x64 form.
        const u32 required = is_64bit ? 48u
                                      : NtdllManager::PROCESS_BASIC_INFO_SIZE;
        if (!set_return_length(required)) {
            m.set_return(STATUS_ACCESS_VIOLATION);
            return;
        }
        if (buf_len < required) {
            m.set_return(nt_status::STATUS_INFO_LENGTH_MISMATCH);
            return;
        }
        if (buf_addr == 0) {
            m.set_return(STATUS_ACCESS_VIOLATION);
            return;
        }

        GuestAddr peb_address = 0;
        u64 process_id = 0;
        u64 parent_process_id = 0;
        if (current_process) {
            peb_address = process_peb();
            process_id = static_cast<u64>(::getpid());
            parent_process_id = nm.current_parent_process_id();
        } else {
            const auto identity = nm.process_identity(process_handle);
            if (!identity.has_value()) {
                ipc::ipc_log(
                    "NtQueryInformationProcess(Basic) handle=0x%llx "
                    "-> STATUS_INVALID_HANDLE (no identity)",
                    static_cast<unsigned long long>(process_handle));
                m.set_return(nt_status::STATUS_INVALID_HANDLE);
                return;
            }
            peb_address = identity->peb_address;
            process_id = identity->process_id;
            ipc::ipc_log(
                "NtQueryInformationProcess(Basic) handle=0x%llx pid=%llu "
                "peb=0x%llx",
                static_cast<unsigned long long>(process_handle),
                static_cast<unsigned long long>(process_id),
                static_cast<unsigned long long>(peb_address));
        }

        bool wrote = false;
        if (is_64bit) {
            wrote =
                m.write_u32(buf_addr + 0, 0).is_ok() &&   // ExitStatus
                m.write_u32(buf_addr + 4, 0).is_ok() &&   // alignment
                m.write_u64(buf_addr + 8, peb_address).is_ok() &&
                m.write_u64(buf_addr + 16, 1).is_ok() &&  // AffinityMask
                m.write_u32(buf_addr + 24, 8).is_ok() &&  // BasePriority
                m.write_u32(buf_addr + 28, 0).is_ok() &&  // alignment
                m.write_u64(buf_addr + 32, process_id).is_ok() &&
                m.write_u64(buf_addr + 40, parent_process_id).is_ok();
        } else {
            wrote =
                m.write_u32(buf_addr + 0, 0).is_ok() &&
                m.write_u32(buf_addr + 4,
                            static_cast<u32>(peb_address)).is_ok() &&
                m.write_u32(buf_addr + 8, 1).is_ok() &&
                m.write_u32(buf_addr + 12, 8).is_ok() &&
                m.write_u32(buf_addr + 16,
                            static_cast<u32>(process_id)).is_ok() &&
                m.write_u32(buf_addr + 20,
                            static_cast<u32>(parent_process_id)).is_ok();
        }
        if (!wrote) {
            m.set_return(STATUS_ACCESS_VIOLATION);
            return;
        }

        m.set_return(nt_status::STATUS_SUCCESS);
        return;
    }
    case 7: { // ProcessDebugPort — return 0 (not being debugged)
        const u32 required = is_64bit ? 8u : 4u;
        if (!set_return_length(required)) {
            m.set_return(STATUS_ACCESS_VIOLATION);
            return;
        }
        if (buf_len < required) {
            m.set_return(nt_status::STATUS_INFO_LENGTH_MISMATCH);
            return;
        }
        if (buf_addr == 0 || m.write_ptr(buf_addr, 0).is_err()) {
            m.set_return(STATUS_ACCESS_VIOLATION);
            return;
        }
        m.set_return(nt_status::STATUS_SUCCESS);
        return;
    }
    case 26: { // ProcessWow64Information — return 0 (not WoW64 from NT perspective)
        const u32 required = is_64bit ? 8u : 4u;
        if (!set_return_length(required)) {
            m.set_return(STATUS_ACCESS_VIOLATION);
            return;
        }
        if (buf_len < required) {
            m.set_return(nt_status::STATUS_INFO_LENGTH_MISMATCH);
            return;
        }
        if (buf_addr == 0 || m.write_ptr(buf_addr, 0).is_err()) {
            m.set_return(STATUS_ACCESS_VIOLATION);
            return;
        }
        m.set_return(nt_status::STATUS_SUCCESS);
        return;
    }
    case 58: { // ProcessHandleTable — ULONG[] of open handle values
        // A sandbox handle closer queries this for the current process, then
        // closes selected File/Section/ALPC handles. Failure here aborts the
        // child at startup.
        if (!current_process) {
            m.set_return(nt_status::STATUS_INVALID_HANDLE);
            return;
        }
        if (nm.handle_table() == nullptr) {
            m.set_return(nt_status::STATUS_INVALID_HANDLE);
            return;
        }
        const auto handles = nm.handle_table()->snapshot_handles();
        const u32 required =
            static_cast<u32>(handles.size() * sizeof(u32));
        if (!set_return_length(required)) {
            m.set_return(STATUS_ACCESS_VIOLATION);
            return;
        }
        if (buf_len < required) {
            m.set_return(nt_status::STATUS_INFO_LENGTH_MISMATCH);
            return;
        }
        if (buf_addr == 0 && required != 0) {
            m.set_return(STATUS_ACCESS_VIOLATION);
            return;
        }
        for (usize i = 0; i < handles.size(); ++i) {
            if (m.write_u32(buf_addr + i * sizeof(u32), handles[i]).is_err()) {
                m.set_return(STATUS_ACCESS_VIOLATION);
                return;
            }
        }
        m.set_return(nt_status::STATUS_SUCCESS);
        return;
    }
    default:
        m.set_return(static_cast<u64>(nt_status::STATUS_INVALID_INFO_CLASS));
        return;
    }
}

// =============================================================================
// NtQuerySystemInformation
// =============================================================================

/// Helper: convert Unix timespec to Windows FILETIME (100-ns intervals since 1601-01-01).
static u64 unix_time_to_filetime(time_t sec, long nsec) {
    // Difference between 1601-01-01 and 1970-01-01 in 100-ns intervals
    constexpr u64 EPOCH_DELTA = 116444736000000000ULL;
    u64 ft = static_cast<u64>(sec) * 10000000ULL + static_cast<u64>(nsec) / 100ULL;
    return ft + EPOCH_DELTA;
}

void impl_NtQuerySystemInformation(Marshaler& m, NtdllManager& /*nm*/) {
    // NTSTATUS NtQuerySystemInformation(
    //   SYSTEM_INFORMATION_CLASS SystemInformationClass,  // arg 0
    //   PVOID            SystemInformation,               // arg 1
    //   ULONG            SystemInformationLength,         // arg 2
    //   PULONG           ReturnLength                     // arg 3
    // );
    const u32 info_class = static_cast<u32>(m.arg(0));
    const u32 buf_addr   = static_cast<u32>(m.arg(1));
    const u32 buf_len    = static_cast<u32>(m.arg(2));
    const u32 rlen_addr  = static_cast<u32>(m.arg(3));

    VITREO_TRACE("[ntdll] NtQuerySystemInformation(class=%u, buf=0x%x, len=%u)\n",
                 info_class, buf_addr, buf_len);

    switch (info_class) {
    case 0: { // SystemBasicInformation — 44 bytes (32-bit layout)
        constexpr u32 REQUIRED = 44;
        if (buf_len < REQUIRED) {
            if (rlen_addr != 0) (void)m.write_u32(rlen_addr, REQUIRED);
            m.set_return(static_cast<u64>(nt_status::STATUS_INFO_LENGTH_MISMATCH));
            return;
        }

        // Get total physical memory via sysctl
        u64 total_ram = 0;
        {
            int mib[2] = {CTL_HW, HW_MEMSIZE};
            size_t len = sizeof(total_ram);
            if (sysctl(mib, 2, &total_ram, &len, nullptr, 0) != 0) {
                total_ram = 4ULL * 1024 * 1024 * 1024; // default 4 GB
            }
        }
        u64 num_pages_64 = total_ram / 4096;
        u32 num_pages = (num_pages_64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : static_cast<u32>(num_pages_64);

        if (buf_addr != 0) {
            (void)m.write_u32(buf_addr + 0,  0);           // Reserved
            (void)m.write_u32(buf_addr + 4,  156250);      // TimerResolution (100ns units ~15.6ms)
            (void)m.write_u32(buf_addr + 8,  4096);        // PageSize
            (void)m.write_u32(buf_addr + 12, num_pages);   // NumberOfPhysicalPages
            (void)m.write_u32(buf_addr + 16, 1);           // LowestPhysicalPageNumber
            (void)m.write_u32(buf_addr + 20, num_pages);   // HighestPhysicalPageNumber
            (void)m.write_u32(buf_addr + 24, 65536);       // AllocationGranularity
            (void)m.write_u32(buf_addr + 28, 0x00010000);  // MinimumUserModeAddress
            (void)m.write_u32(buf_addr + 32, 0x7FFEFFFF);  // MaximumUserModeAddress
            (void)m.write_u32(buf_addr + 36, 0xFF);        // ActiveProcessorsAffinityMask (8 CPUs)
            (void)m.write_u8(buf_addr + 40,  8);           // NumberOfProcessors
        }
        if (rlen_addr != 0) (void)m.write_u32(rlen_addr, REQUIRED);
        m.set_return(static_cast<u64>(nt_status::STATUS_SUCCESS));
        return;
    }

    case 1: { // SystemProcessorInformation — 12 bytes
        constexpr u32 REQUIRED = 12;
        if (buf_len < REQUIRED) {
            if (rlen_addr != 0) (void)m.write_u32(rlen_addr, REQUIRED);
            m.set_return(static_cast<u64>(nt_status::STATUS_INFO_LENGTH_MISMATCH));
            return;
        }

        if (buf_addr != 0) {
            (void)m.write_u16(buf_addr + 0, 0);      // ProcessorArchitecture (INTEL)
            (void)m.write_u16(buf_addr + 2, 6);      // ProcessorLevel (P6 family)
            (void)m.write_u16(buf_addr + 4, 0x0F00); // ProcessorRevision
            (void)m.write_u16(buf_addr + 6, 8);      // MaximumProcessors
            (void)m.write_u32(buf_addr + 8, 0);      // ProcessorFeatureBits
        }
        if (rlen_addr != 0) (void)m.write_u32(rlen_addr, REQUIRED);
        m.set_return(static_cast<u64>(nt_status::STATUS_SUCCESS));
        return;
    }

    case 2: { // SystemPerformanceInformation — complex, stub
        m.set_return(static_cast<u64>(nt_status::STATUS_INFO_LENGTH_MISMATCH));
        return;
    }

    case 3: { // SystemTimeOfDayInformation — 48 bytes
        constexpr u32 REQUIRED = 48;
        if (buf_len < REQUIRED) {
            if (rlen_addr != 0) (void)m.write_u32(rlen_addr, REQUIRED);
            m.set_return(static_cast<u64>(nt_status::STATUS_INFO_LENGTH_MISMATCH));
            return;
        }

        // Get boot time via sysctl
        u64 boot_filetime = 0;
        {
            struct timeval boottime{};
            int mib[2] = {CTL_KERN, KERN_BOOTTIME};
            size_t len = sizeof(boottime);
            if (sysctl(mib, 2, &boottime, &len, nullptr, 0) == 0) {
                boot_filetime = unix_time_to_filetime(boottime.tv_sec, boottime.tv_usec * 1000L);
            }
        }

        // Current time
        struct timespec now{};
        clock_gettime(CLOCK_REALTIME, &now);
        u64 current_filetime = unix_time_to_filetime(now.tv_sec, now.tv_nsec);

        if (buf_addr != 0) {
            // offset 0: BootTime (u64)
            (void)m.write_u32(buf_addr + 0,  static_cast<u32>(boot_filetime & 0xFFFFFFFF));
            (void)m.write_u32(buf_addr + 4,  static_cast<u32>(boot_filetime >> 32));
            // offset 8: CurrentTime (u64)
            (void)m.write_u32(buf_addr + 8,  static_cast<u32>(current_filetime & 0xFFFFFFFF));
            (void)m.write_u32(buf_addr + 12, static_cast<u32>(current_filetime >> 32));
            // offset 16: TimeZoneBias (u64)
            (void)m.write_u32(buf_addr + 16, 0);
            (void)m.write_u32(buf_addr + 20, 0);
            // offset 24: TimeZoneId (u32)
            (void)m.write_u32(buf_addr + 24, 0);
            // offset 28: Reserved (u32)
            (void)m.write_u32(buf_addr + 28, 0);
            // offset 32: BootTimeBias (u64)
            (void)m.write_u32(buf_addr + 32, 0);
            (void)m.write_u32(buf_addr + 36, 0);
            // offset 40: SleepTimeBias (u64)
            (void)m.write_u32(buf_addr + 40, 0);
            (void)m.write_u32(buf_addr + 44, 0);
        }
        if (rlen_addr != 0) (void)m.write_u32(rlen_addr, REQUIRED);
        m.set_return(static_cast<u64>(nt_status::STATUS_SUCCESS));
        return;
    }

    default:
        VITREO_TRACE("[ntdll] NtQuerySystemInformation: unsupported class %u\n", info_class);
        m.set_return(static_cast<u64>(nt_status::STATUS_NOT_IMPLEMENTED));
        return;
    }
}

// =============================================================================
// Export Factory
// =============================================================================

// Microsoft Learn RtlUTF8ToUnicodeN; malformed-prefix consumption and partial
// surrogate writes are measured by tools/oracle_probe/rtl_utf8_probe.c.
static void rtl_utf8_to_unicode(Marshaler& m) {
#include "rtl_utf8_status.inc"
    const GuestAddr destination = m.arg_ptr(0), actual = m.arg_ptr(2), source = m.arg_ptr(3);
    const u32 capacity = m.arg32(1), length = m.arg32(4);
    if (!source) { m.set_return(invalid_parameter_4); return; }
    if (!destination && !actual) { m.set_return(invalid_parameter); return; }
    u32 position = 0, written = 0, status = success;
    auto emit = [&](u16 unit) {
        if (destination) {
            if (capacity - written < 2) { status = buffer_too_small; return false; }
            if (!m.write_u16(destination + written, unit).is_ok()) {
                status = nt_status::STATUS_ACCESS_VIOLATION;
                return false;
            }
        }
        written += 2;
        return true;
    };
    while (position < length) {
        if (destination && capacity - written < 2) { status = buffer_too_small; break; }
        const auto first = m.read_u8(source + position++);
        if (!first.is_ok()) { status = nt_status::STATUS_ACCESS_VIOLATION; break; }
        const u8 lead = first.value();
        u32 code = lead;
        unsigned following = 0;
        bool valid = true;
        if (lead >= 0xc2 && lead <= 0xdf) { code = lead & 0x1f; following = 1; }
        else if (lead >= 0xe0 && lead <= 0xef) { code = lead & 0x0f; following = 2; }
        else if (lead >= 0xf0 && lead <= 0xf4) { code = lead & 0x07; following = 3; }
        else if (lead >= 0x80) valid = false;
        for (unsigned i = 0; valid && i < following; ++i) {
            if (position == length) { valid = false; break; }
            const auto next = m.read_u8(source + position);
            if (!next.is_ok()) { m.set_return(nt_status::STATUS_ACCESS_VIOLATION); return; }
            const u8 byte = next.value();
            if ((byte & 0xc0) != 0x80) { valid = false; break; }
            ++position;
            // Windows consumes an out-of-range second continuation byte,
            // then replaces the remaining continuation bytes separately.
            if (i == 0 && ((lead == 0xe0 && byte < 0xa0) ||
                           (lead == 0xed && byte >= 0xa0) ||
                           (lead == 0xf0 && byte < 0x90) ||
                           (lead == 0xf4 && byte >= 0x90))) {
                valid = false; break;
            }
            code = (code << 6) | (byte & 0x3f);
        }
        if (!valid) { code = 0xfffd; status = some_not_mapped; }
        if (code > 0xffff) {
            code -= 0x10000;
            if (!emit(static_cast<u16>(0xd800 + (code >> 10)))) break;
            if (!emit(static_cast<u16>(0xdc00 + (code & 0x3ff)))) break;
        } else if (!emit(static_cast<u16>(code))) break;
    }
    if (actual && !m.write_u32(actual, written).is_ok()) status = nt_status::STATUS_ACCESS_VIOLATION;
    m.set_return(status);
}

DllExports make_ntdll_exports(NtdllManager& nm) {
    DllExports exports;

    // KiUserApcDispatcher is a named ntdll image entry point, even though it
    // is not an application-callable NT routine. The kernel transfers control
    // to it when returning an alertable thread to user mode; Vitreo performs
    // that transfer through ThreadManager's alertable-wait APC path. Publish
    // the image identity so PE export walkers observe the same module
    // contract. A direct call has no documented application ABI, so fail it
    // diagnostically instead of pretending that an APC was dispatched.
    exports.entries.push_back(ExportEntry{
        "KiUserApcDispatcher", 0,
        [](Marshaler& m) {
            VITREO_TRACE(
                "[ntdll] KiUserApcDispatcher entered outside APC delivery\n");
            m.set_return(static_cast<u64>(nt_status::STATUS_NOT_IMPLEMENTED));
        },
        0
    });

    // KiUserExceptionDispatcher is the corresponding ntdll user-mode image
    // entry for hardware and software exceptions. Microsoft documents that
    // this entry feeds the normal RtlDispatchException path, with vectored
    // handlers running before the frame-based unwind walk. Vitreo implements
    // those observable semantics in runtime/execution/exception_dispatch;
    // publish the image entry for PE export walkers without duplicating that
    // delivery machinery in an ordinary callable thunk.
    exports.entries.push_back(ExportEntry{
        "KiUserExceptionDispatcher", 0,
        [](Marshaler& m) {
            VITREO_TRACE(
                "[ntdll] KiUserExceptionDispatcher entered outside exception delivery\n");
            m.set_return(static_cast<u64>(nt_status::STATUS_NOT_IMPLEMENTED));
        },
        0
    });

    exports.entries.push_back(ExportEntry{
        "RtlInitUnicodeString", 1,
        [&nm](Marshaler& m) { impl_RtlInitUnicodeString(m, nm); },
        8
    });

    exports.entries.push_back(ExportEntry{
        "NtQueryInformationProcess", 2,
        [&nm](Marshaler& m) { impl_NtQueryInformationProcess(m, nm); },
        20
    });

    exports.entries.push_back(ExportEntry{
        "NtQuerySystemInformation", 5,
        [&nm](Marshaler& m) { impl_NtQuerySystemInformation(m, nm); },
        16
    });

    exports.entries.push_back({"RtlUTF8ToUnicodeN", 0, rtl_utf8_to_unicode, 20});
    // Windows observations: tools/oracle_probe/rtl_cs_owner_probe.c.
    // Read the guest structure, also used by guest inline lock operations.
    exports.entries.push_back({"RtlIsCriticalSectionLockedByThread", 0,
        [](Marshaler& m) {
            const auto& layout = m.cpu_is_64bit() ? cs_layout_64 : cs_layout_32;
            const GuestAddr address = m.arg_ptr(0) + layout.off_owning_thread;
            const auto owner = m.read_ptr(address);
            if (!owner.is_ok()) {
                if (auto* cpu = m.cpu()) {
                    cpu->pending_raise.active = true;
                    cpu->pending_raise.from_helper = false;
                    cpu->pending_raise.code = nt_status::STATUS_ACCESS_VIOLATION;
                    cpu->pending_raise.flags = 0;
                    cpu->pending_raise.num_args = 2;
                    cpu->pending_raise.args[0] = 0; // read access
                    cpu->pending_raise.args[1] = address;
                }
                return;
            }
            m.set_return(owner.value() == ThreadManager::get_current_thread_id_static());
        }, 4});
    return exports;
}

} // namespace vitreo::winapi
