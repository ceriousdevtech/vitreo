/// @file nt_query_virtual_memory.cpp
/// @brief ntdll!NtQueryVirtualMemory.
///
/// Learn (ZwQueryVirtualMemory) documents the prototype, class 0
/// (MemoryBasicInformation -> MEMORY_BASIC_INFORMATION) and the status codes.
/// Every rule below that Learn does not state is measured by
/// tools/oracle_probe/nt_query_virtual_memory_probe (x64 and x86 recordings):
///  * class 0: Length below sizeof(MEMORY_BASIC_INFORMATION) (48 / 28) is
///    STATUS_INFO_LENGTH_MISMATCH before the handle is looked at; a kernel
///    address is STATUS_INVALID_PARAMETER; ReturnLength = the structure size.
///  * class 2 (MemoryMappedFilenameInformation): a UNICODE_STRING followed by
///    the NUL-terminated NT device path of the backing file (MaximumLength =
///    Length + 2); ReturnLength = the whole size even when the buffer is too
///    small (STATUS_INFO_LENGTH_MISMATCH below the UNICODE_STRING itself,
///    STATUS_BUFFER_OVERFLOW otherwise). An image or file-backed view has a
///    name; a pagefile-backed view is STATUS_FILE_INVALID; private, reserved
///    and free memory is STATUS_INVALID_ADDRESS. A 32-bit process sees its
///    system DLLs under SysWOW64.
///  * a bad process handle is STATUS_INVALID_HANDLE.
///  * x86 (WOW64) writes ReturnLength on some failures as well: class 0 with
///    a bad handle reports 28, and every class-2 failure past the length
///    check reports Length + 8. x64 leaves it untouched on failure.
///  * classes Windows answers but Vitreo does not implement fail with
///    STATUS_INVALID_INFO_CLASS and an UNIMPLEMENTED line; classes Windows
///    itself rejects keep Windows' status.

#include "winapi/system/ntdll/nt_query_virtual_memory.hpp"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <utility>

#include "winapi/kernel32/locale/string_conv.hpp"

namespace vitreo::winapi::ntdll {

namespace {

constexpr u32 kStatusSuccess = 0x00000000;
constexpr u32 kStatusBufferOverflow = 0x80000005;
constexpr u32 kStatusInvalidInfoClass = 0xC0000003;
constexpr u32 kStatusInfoLengthMismatch = 0xC0000004;
constexpr u32 kStatusInvalidHandle = 0xC0000008;
constexpr u32 kStatusInvalidParameter = 0xC000000D;
constexpr u32 kStatusNotImplemented = 0xC0000002;
constexpr u32 kStatusFileInvalid = 0xC0000098;
constexpr u32 kStatusInvalidAddress = 0xC0000141;

constexpr u32 kMemoryBasicInformation = 0;
constexpr u32 kMemoryMappedFilenameInformation = 2;
constexpr u32 kMemMapped = 0x40000;

void report_unimplemented_class(u32 cls, bool is64) {
    static std::atomic<u64> reported{0};
    const u64 bit = u64{1} << (cls & 63u);
    if ((reported.fetch_or(bit) & bit) == 0) {
        std::fprintf(stderr,
                     "[vitreo] UNIMPLEMENTED ntdll!NtQueryVirtualMemory: "
                     "MemoryInformationClass %u (%s) -> STATUS_INVALID_INFO_CLASS\n",
                     cls, is64 ? "x64" : "x86");
    }
}

/// Classes Windows itself rejects for an ordinary user-mode caller, and the
/// status it gives (probe "ntdll_classes"). Anything else that is not 0 or 2
/// is a class Windows answers.
bool windows_rejects(u32 cls, bool is64, u32& status) {
    if (is64) {
        if (cls == 9 || cls == 10 || cls >= 15) {
            status = kStatusInvalidInfoClass;
            return true;
        }
        return false;
    }
    if (cls == 5 || cls == 9 || cls >= 11) {
        status = kStatusInvalidInfoClass;
        return true;
    }
    return false;
}

std::string to_wow64_view(std::string path) {
    // A 32-bit process maps the x86 system DLLs from SysWOW64; the loader
    // reports them under System32 (GetModuleFileName), the file is SysWOW64.
    static constexpr std::string_view kSystem32 = "c:\\windows\\system32\\";
    if (path.size() > kSystem32.size()) {
        std::string head = path.substr(0, kSystem32.size());
        for (char& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (head == kSystem32) {
            path = path.substr(0, 11) + "SysWOW64\\" + path.substr(kSystem32.size());
        }
    }
    return path;
}

NtQueryVirtualMemoryHost& host_slot() {
    static NtQueryVirtualMemoryHost host;
    return host;
}

}  // namespace

void set_query_virtual_memory_host(NtQueryVirtualMemoryHost host) {
    host_slot() = std::move(host);
}

const NtQueryVirtualMemoryHost& query_virtual_memory_host() {
    return host_slot();
}

void query_virtual_memory(Marshaler& m, const NtQueryVirtualMemoryHost& host) {
    const u64 handle = m.arg(0);
    const u64 address = m.arg(1);
    const u32 cls = m.arg32(2);
    const GuestAddr buffer = m.arg_ptr(3);
    const u64 length = m.cpu_is_64bit() ? m.arg(4) : m.arg32(4);
    const GuestAddr return_length = m.arg_ptr(5);
    const bool is64 = m.cpu_is_64bit();
    const auto write_size = [&](u64 value) {
        if (return_length == 0) return;
        if (is64) (void)m.memory().write_u64(return_length, value);
        else (void)m.memory().write_u32(return_length, static_cast<u32>(value));
    };
    const auto fail = [&](u32 status) { m.set_return(static_cast<u64>(status)); };

    if (cls != kMemoryBasicInformation && cls != kMemoryMappedFilenameInformation) {
        u32 status = kStatusInvalidInfoClass;
        if (!windows_rejects(cls, is64, status)) report_unimplemented_class(cls, is64);
        fail(status);
        return;
    }

    if (cls == kMemoryBasicInformation) {
        const u64 size = is64 ? 48u : 28u;
        if (length < size) {
            fail(kStatusInfoLengthMismatch);
            return;
        }
        const auto target = host.target ? host.target(handle)
                                         : NtQueryVirtualMemoryHost::Target::Invalid;
        if (target == NtQueryVirtualMemoryHost::Target::Invalid) {
            if (!is64) write_size(size);
            fail(kStatusInvalidHandle);
            return;
        }
        if (target == NtQueryVirtualMemoryHost::Target::Other) {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ntdll!NtQueryVirtualMemory: "
                                 "another process's address space\n");
            fail(kStatusNotImplemented);
            return;
        }
        MemoryBasicInformation info{};
        const u64 maximum = is64 ? 0x00007FFF'FFFEFFFFULL : 0xFFFF'FFFFULL;
        if (!host.basic || host.basic(address, info, maximum) == 0) {
            fail(kStatusInvalidParameter);
            return;
        }
        bool ok = true;
        if (is64) {
            // MEMORY_BASIC_INFORMATION (x64): PartitionId (WORD) at +0x14.
            ok = m.write_struct<MemoryBasicInformation>(buffer, info).is_ok() &&
                 m.memory().write_u16(buffer + 0x14, 0).is_ok();
        } else {
            const u32 fields[7] = {
                static_cast<u32>(info.base_address), static_cast<u32>(info.allocation_base),
                info.allocation_protect, static_cast<u32>(info.region_size), info.state,
                info.protect, info.type};
            for (u32 i = 0; i < 7 && ok; ++i) ok = m.write_u32(buffer + i * 4, fields[i]).is_ok();
        }
        if (!ok) {
            fail(0xC0000005);  // STATUS_ACCESS_VIOLATION writing MemoryInformation
            return;
        }
        write_size(size);
        m.set_return(kStatusSuccess);
        return;
    }

    // MemoryMappedFilenameInformation.
    const u64 header = is64 ? 16u : 8u;  // UNICODE_STRING
    const auto failed_name = [&](u32 status) {
        if (!is64) write_size(length + 8);
        fail(status);
    };
    const auto target = host.target ? host.target(handle)
                                    : NtQueryVirtualMemoryHost::Target::Invalid;
    if (target == NtQueryVirtualMemoryHost::Target::Invalid) {
        failed_name(kStatusInvalidHandle);
        return;
    }
    if (target == NtQueryVirtualMemoryHost::Target::Other) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ntdll!NtQueryVirtualMemory: "
                             "another process's address space\n");
        fail(kStatusNotImplemented);
        return;
    }
    std::string path = host.mapped_file ? host.mapped_file(address) : std::string{};
    if (path.empty()) {
        MemoryBasicInformation info{};
        const u64 maximum = is64 ? 0x00007FFF'FFFEFFFFULL : 0xFFFF'FFFFULL;
        const bool known = host.basic && host.basic(address, info, maximum) != 0;
        failed_name(known && info.type == kMemMapped ? kStatusFileInvalid : kStatusInvalidAddress);
        return;
    }
    if (!is64) path = to_wow64_view(path);
    const std::string device = host.nt_device_path ? host.nt_device_path(path) : std::string{};
    if (device.empty()) {
        failed_name(kStatusFileInvalid);
        return;
    }
    const std::vector<u16> text = utf8_to_utf16(device);
    const u64 text_bytes = text.size() * 2u;
    const u64 required = header + text_bytes + 2u;
    if (length < header) {
        write_size(required);
        fail(kStatusInfoLengthMismatch);
        return;
    }
    if (length < required) {
        write_size(required);
        fail(kStatusBufferOverflow);
        return;
    }
    const GuestAddr chars = buffer + header;
    bool ok = m.memory().write_u16(buffer + 0, static_cast<u16>(text_bytes)).is_ok() &&
              m.memory().write_u16(buffer + 2, static_cast<u16>(text_bytes + 2)).is_ok();
    if (is64) {
        ok = ok && m.memory().write_u32(buffer + 4, 0).is_ok() &&
             m.memory().write_u64(buffer + 8, chars).is_ok();
    } else {
        ok = ok && m.memory().write_u32(buffer + 4, static_cast<u32>(chars)).is_ok();
    }
    for (usize i = 0; i < text.size() && ok; ++i) {
        ok = m.memory().write_u16(chars + i * 2u, static_cast<u16>(text[i])).is_ok();
    }
    ok = ok && m.memory().write_u16(chars + text_bytes, 0).is_ok();
    if (!ok) {
        fail(0xC0000005);
        return;
    }
    write_size(required);
    m.set_return(kStatusSuccess);
}

}  // namespace vitreo::winapi::ntdll
