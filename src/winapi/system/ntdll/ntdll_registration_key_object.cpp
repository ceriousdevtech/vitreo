// SPDX-License-Identifier: LGPL-2.1-only

/// @file ntdll_registration_key_object.cpp
/// @brief Extended ntdll export table, part 10 of 11: the Wave 7 registry-key
///        stubs, NtQueryObject and the Rtl string / last-status exports.
///
/// A contiguous slice of the table that make_ntdll_ext_exports builds
/// (see ntdll_registration.cpp). The slice boundaries follow the table's
/// registration order, which is load-bearing: ordinals come from the
/// running `ordinal++` counter and ModuleManager derives the service
/// veneers from entry order. Do not reorder entries or call the parts out
/// of order.

#include "winapi/common/object_access.hpp"
#include "winapi/system/ntdll/ntdll_registration_parts.hpp"
#include "winapi/system/ntdll/ntdll_internal.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/system/ntdll/nt_event.hpp"
#include "ipc/ipc_gate.hpp"
#include <string>
#include <string_view>

namespace vitreo::winapi {

using namespace ntdll;

void append_key_object_exports(NtdllExtManager& mgr,
                               DllExports& exports, u16& /*ordinal*/) {
    // ------------------------------------------------------------------------
    // Wave 7: minimal Nt*Key / RtlFormatCurrentUserKeyPath.
    // These return failure (key-not-found) cleanly so callers fall back to
    // public Reg*W APIs from advapi32.dll instead of crashing on -1.
    // ------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{ "NtCreateKey", 0,
        [](Marshaler& m){
            GuestAddr pkey = m.arg_ptr(0);
            for (u32 i = 1; i < 6; ++i) (void)m.arg(i);
            GuestAddr pdisp = m.arg_ptr(6);
            if (pkey) (void)m.write_u32(pkey, 0);
            if (pdisp) (void)m.write_u32(pdisp, 0);
            m.set_return(0xC0000034u); // STATUS_OBJECT_NAME_NOT_FOUND
        }, 28 });
    exports.entries.push_back(ExportEntry{ "NtDeleteKey", 0,
        [](Marshaler& m){ (void)m.arg(0); m.set_return(0xC0000008u); }, 4 });
    exports.entries.push_back(ExportEntry{ "NtSetValueKey", 0,
        [](Marshaler& m){
            for (u32 i = 0; i < 6; ++i) (void)m.arg(i);
            m.set_return(0xC0000008u); // STATUS_INVALID_HANDLE
        }, 24 });
    exports.entries.push_back(ExportEntry{ "NtQueryObject", 0,
        [&mgr](Marshaler& m){
            const Handle handle = static_cast<Handle>(m.arg(0));
            const u32 info_class = static_cast<u32>(m.arg(1));
            GuestAddr info = m.arg_ptr(2);
            const u32 cb = static_cast<u32>(m.arg(3));
            const GuestAddr ret_cb = m.arg_ptr(4);

            // 0=ObjectBasicInformation, 1=ObjectNameInformation,
            // 2=ObjectTypeInformation (ntifs / winternl).
            if (info_class != 0 && info_class != 1 && info_class != 2) {
                m.set_return(nt_status::STATUS_INVALID_INFO_CLASS);
                return;
            }

            HandleTable* handles = mgr.handle_table();
            const HandleType type = handles == nullptr
                ? HandleType::Invalid
                : handles->get_type(handle);
            if (type == HandleType::Invalid) {
                m.set_return(nt_status::STATUS_INVALID_HANDLE);
                return;
            }

            if (info_class == 1) { // ObjectNameInformation
                // A signed-policy check uses this to obtain the NT path of a
                // FILE_EXECUTE CreateFile handle before
                // NtCreateSection(SEC_IMAGE).
                std::string win32_path;
                if (type == HandleType::File && mgr.file_manager() != nullptr) {
                    win32_path = mgr.file_manager()->path_for_handle(handle);
                }
                std::u16string nt_name;
                if (!win32_path.empty()) {
                    std::string nt = win32_path;
                    if (nt.size() >= 2 && nt[1] == ':') {
                        nt = "\\??\\" + nt;
                    } else if (nt.rfind("\\\\?\\", 0) == 0) {
                        nt = "\\??\\" + nt.substr(4);
                    } else if (nt.rfind("\\??\\", 0) != 0) {
                        nt = "\\??\\" + nt;
                    }
                    nt_name.assign(nt.begin(), nt.end());
                }
                // A named event / mutex / semaphore reports its
                // BaseNamedObjects path (nt_event_probe local_directory:
                // "\Sessions\N\BaseNamedObjects\X" for Local\X).
                if ((type == HandleType::Event || type == HandleType::Mutex ||
                     type == HandleType::Semaphore) &&
                    mgr.sync_manager() != nullptr) {
                    const std::string object = mgr.sync_manager()->object_name(handle);
                    if (!object.empty()) {
                        const std::vector<u16> wide =
                            utf8_to_utf16(ntdll::win32_object_name_to_nt(object));
                        nt_name.assign(wide.begin(), wide.end());
                        win32_path = object;
                    }
                }

                const u32 us_size = m.cpu_is_64bit() ? 16u : 8u;
                const u32 name_bytes = static_cast<u32>(nt_name.size() * 2u);
                const u32 required = us_size + name_bytes + 2u;
                if (ret_cb != 0) (void)m.write_u32(ret_cb, required);
                if (nt_name.empty()) {
                    // No associated name — Length/MaximumLength zero.
                    if (ret_cb != 0) (void)m.write_u32(ret_cb, us_size);
                    if (info == 0 || cb < us_size) {
                        m.set_return(nt_status::STATUS_INFO_LENGTH_MISMATCH);
                        return;
                    }
                    for (u32 i = 0; i < us_size; ++i) (void)m.write_u8(info + i, 0);
                    m.set_return(nt_status::STATUS_SUCCESS);
                    return;
                }
                if (info == 0 || cb < required) {
                    m.set_return(nt_status::STATUS_INFO_LENGTH_MISMATCH);
                    return;
                }
                for (u32 i = 0; i < us_size; ++i) (void)m.write_u8(info + i, 0);
                (void)m.write_u16(info, static_cast<u16>(name_bytes));
                (void)m.write_u16(info + 2, static_cast<u16>(name_bytes + 2u));
                const GuestAddr name_buf = info + us_size;
                (void)m.write_ptr(info + (m.cpu_is_64bit() ? 8u : 4u), name_buf);
                for (usize i = 0; i < nt_name.size(); ++i) {
                    (void)m.write_u16(name_buf + i * 2u,
                                      static_cast<u16>(nt_name[i]));
                }
                (void)m.write_u16(name_buf + name_bytes, 0);
                ipc::ipc_log(
                    "NtQueryObject ObjectNameInformation h=0x%x -> '%s'",
                    static_cast<unsigned>(handle), win32_path.c_str());
                m.set_return(nt_status::STATUS_SUCCESS);
                return;
            }

            if (info_class == 0) { // ObjectBasicInformation
                // PUBLIC_OBJECT_BASIC_INFORMATION is native-width independent:
                // four ULONG/ACCESS_MASK fields followed by ten reserved ULONGs.
                constexpr u32 required = 56;
                if (ret_cb != 0) (void)m.write_u32(ret_cb, required);
                if (info == 0 || cb < required) {
                    m.set_return(nt_status::STATUS_INFO_LENGTH_MISMATCH);
                    return;
                }

                u32 granted_access = handles->get_access(handle);
                const bool is_file_object =
                    type == HandleType::File || type == HandleType::Pipe ||
                    type == HandleType::Mailslot ||
                    type == HandleType::ConsoleInput ||
                    type == HandleType::ConsoleOutput;
                if (is_file_object) {
                    // The object manager reports mapped, object-specific rights;
                    // generic bits supplied to CreateFile are not retained in
                    // GrantedAccess.
                    granted_access = object_access::file_granted_access(
                        granted_access, HandleAccess::All);
                }

                for (u32 offset = 0; offset < required; ++offset)
                    (void)m.write_u8(info + offset, 0);
                const u32 attributes =
                    (handles->get_flags(handle) & HandleFlags::Inherit) != 0
                        ? 0x00000002u // OBJ_INHERIT
                        : 0u;
                (void)m.write_u32(info, attributes);
                (void)m.write_u32(info + 4, granted_access);
                (void)m.write_u32(info + 8, 1);  // this open handle
                (void)m.write_u32(info + 12, 1); // this table reference
                m.set_return(nt_status::STATUS_SUCCESS);
                return;
            }

            const auto type_name = [](HandleType value) -> std::u16string_view {
                switch (value) {
                    case HandleType::FileMapping:      return u"Section";
                    case HandleType::File:
                    case HandleType::Pipe:
                    case HandleType::Mailslot:
                    case HandleType::ConsoleInput:
                    case HandleType::ConsoleOutput:   return u"File";
                    case HandleType::Process:          return u"Process";
                    case HandleType::Thread:           return u"Thread";
                    case HandleType::Event:            return u"Event";
                    case HandleType::Mutex:            return u"Mutant";
                    case HandleType::Semaphore:        return u"Semaphore";
                    case HandleType::Registry:         return u"Key";
                    case HandleType::Timer:            return u"Timer";
                    case HandleType::Token:            return u"Token";
                    case HandleType::Job:              return u"Job";
                    case HandleType::Device:           return u"Device";
                    case HandleType::IoCompletionPort: return u"IoCompletion";
                    case HandleType::Heap:             return u"Heap";
                    case HandleType::Module:           return u"Section";
                    case HandleType::Invalid:          return {};
                }
                return {};
            }(type);

            // PUBLIC_OBJECT_TYPE_INFORMATION is UNICODE_STRING followed by
            // 22 reserved ULONGs. Keep the string in the caller's buffer,
            // immediately after that fixed native-width structure.
            const u32 fixed_size = m.cpu_is_64bit() ? 104u : 96u;
            const u32 name_bytes = static_cast<u32>(type_name.size() * 2u);
            const u32 required = fixed_size + name_bytes + 2u;
            if (ret_cb != 0) (void)m.write_u32(ret_cb, required);
            if (info == 0 || cb < required) {
                m.set_return(nt_status::STATUS_INFO_LENGTH_MISMATCH);
                return;
            }

            for (u32 offset = 0; offset < fixed_size; ++offset) {
                (void)m.write_u8(info + offset, 0);
            }
            (void)m.write_u16(info, static_cast<u16>(name_bytes));
            (void)m.write_u16(info + 2, static_cast<u16>(name_bytes + 2u));
            (void)m.write_ptr(info + (m.cpu_is_64bit() ? 8u : 4u),
                              info + fixed_size);
            for (usize index = 0; index < type_name.size(); ++index) {
                (void)m.write_u16(info + fixed_size + index * 2u,
                                  static_cast<u16>(type_name[index]));
            }
            (void)m.write_u16(info + fixed_size + name_bytes, 0);
            m.set_return(nt_status::STATUS_SUCCESS);
        }, 20 });
    exports.entries.push_back(ExportEntry{ "RtlFormatCurrentUserKeyPath", 0,
        [](Marshaler& m){
            GuestAddr out = m.arg_ptr(0);
            (void)out;
            // Cannot easily allocate guest memory here without a heap manager
            // reference; write zeros into the UNICODE_STRING to be safe.
            if (out) {
                (void)m.write_u16(out, 0);
                (void)m.write_u16(out + 2, 0);
                (void)m.write_u32(out + 4, 0);
            }
            m.set_return(0xC0000017u); // STATUS_NO_MEMORY (so caller falls back)
        }, 4 });
    // ULONG RtlNtStatusToDosErrorNoTeb(NTSTATUS): RtlNtStatusToDosError's
    // mapping without storing TEB.LastStatusValue (rtl_string_status_probe
    // to_dos_noteb).
    exports.entries.push_back(ExportEntry{ "RtlNtStatusToDosErrorNoTeb", 0,
        [&mgr](Marshaler& m){
            m.set_return(mgr.nt_status_to_dos_error(static_cast<u32>(m.arg(0))));
        }, 4 });
    // RtlFreeUnicodeString frees into the process heap, so it lives with
    // the Rtl heap (create_rtl_heap_thunks, heap_ext.cpp).
    // NTSTATUS RtlGetLastNtStatus(void): the calling thread's
    // TEB.LastStatusValue (rtl_string_status_probe: x64 +0x1250, x86
    // +0xBF4), which RtlNtStatusToDosError stores.
    exports.entries.push_back(ExportEntry{ "RtlGetLastNtStatus", 0,
        [](Marshaler& m){
            const auto teb = ntdll::current_teb(m);
            m.set_return(teb == 0 ? 0 : m.read_u32(teb + ntdll::teb_last_status_offset(m))
                                              .value_or(0));
        }, 0 });
}

} // namespace vitreo::winapi
