// SPDX-License-Identifier: LGPL-2.1-only

/// @file ntdll_registration_native_services.cpp
/// @brief Extended ntdll export table, part 8 of 11: NtDuplicateObject,
///        NtSetInformationThread and the sandbox native-service
///        veneers.
///
/// A contiguous slice of the table that make_ntdll_ext_exports builds
/// (see ntdll_registration.cpp). The slice boundaries follow the table's
/// registration order, which is load-bearing: ordinals come from the
/// running `ordinal++` counter and ModuleManager derives the service
/// veneers from entry order. Do not reorder entries or call the parts out
/// of order.

#include "winapi/system/ntdll/ntdll_registration_parts.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include <atomic>
#include <cstdio>

namespace vitreo::winapi {

using namespace ntdll;

void append_native_services_exports(NtdllExtManager& mgr,
                                    DllExports& exports, u16& ordinal) {
    // NtDuplicateObject is the native same-process primitive behind
    // DuplicateHandle.  Native callers use HANDLE attributes (OBJ_INHERIT)
    // instead of a BOOL, but the duplicated object must share the same owner
    // and lifetime machinery as its Win32 counterpart.
    auto nt_duplicate_object = [&mgr](Marshaler& m) {
            VITREO_TRACE("[ntdll] NtDuplicateObject enter\n");
            constexpr u32 kDuplicateCloseSource = 0x00000001;
            constexpr u32 kDuplicateSameAccess = 0x00000002;
            constexpr u32 kDuplicateSameAttributes = 0x00000004;
            constexpr u32 kObjInherit = 0x00000002;
            constexpr Handle kCurrentProcess = static_cast<Handle>(-1);
            constexpr Handle kCurrentThread = static_cast<Handle>(-2);
            const Handle source_process = static_cast<Handle>(m.arg(0));
            const Handle source = static_cast<Handle>(m.arg(1));
            const Handle target_process = static_cast<Handle>(m.arg(2));
            const GuestAddr target_ptr = m.arg_ptr(3);
            const u32 desired_access = static_cast<u32>(m.arg(4));
            const u32 attributes = static_cast<u32>(m.arg(5));
            const u32 options = static_cast<u32>(m.arg(6));
            auto* handles = mgr.handle_table();

            VITREO_TRACE(
                "[ntdll] NtDuplicateObject raw srcproc=0x%x src=0x%x "
                "dstproc=0x%x target=0x%llx desired=0x%x attrs=0x%x "
                "options=0x%x\n",
                static_cast<unsigned>(source_process),
                static_cast<unsigned>(source),
                static_cast<unsigned>(target_process),
                static_cast<unsigned long long>(target_ptr), desired_access,
                attributes, options);

            if (handles == nullptr || target_ptr == 0 ||
                source_process != kCurrentProcess ||
                target_process != kCurrentProcess ||
                (options & ~(kDuplicateCloseSource |
                             kDuplicateSameAccess |
                             kDuplicateSameAttributes)) != 0) {
                m.set_return(nt_status::STATUS_INVALID_PARAMETER);
                return;
            }

            const u32 flags = (options & kDuplicateSameAttributes) != 0
                ? (source == kCurrentThread ? 0 : handles->get_flags(source))
                : ((attributes & kObjInherit) != 0
                       ? HandleFlags::Inherit
                       : 0);
            const u32 access = (options & kDuplicateSameAccess) != 0
                ? handles->get_access(source)
                : desired_access;
            Handle duplicate = NULL_HANDLE;
            const HandleType type = handles->get_type(source);
            VITREO_TRACE(
                "[ntdll] NtDuplicateObject srcproc=0x%x src=0x%x "
                "dstproc=0x%x access=0x%x attrs=0x%x options=0x%x "
                "type=%s\n",
                static_cast<unsigned>(source_process),
                static_cast<unsigned>(source),
                static_cast<unsigned>(target_process), access, attributes,
                options, handle_type_to_string(type));
            if (type == HandleType::FileMapping &&
                mgr.file_mapping_manager() != nullptr) {
                duplicate = mgr.file_mapping_manager()->duplicate_handle(
                    source, flags, access);
            } else if ((type == HandleType::File || type == HandleType::Pipe) &&
                       mgr.file_manager() != nullptr) {
                duplicate = mgr.file_manager()->duplicate_handle(
                    source, flags, access);
            } else if ((type == HandleType::Event ||
                        type == HandleType::Mutex ||
                        type == HandleType::Semaphore) &&
                       mgr.sync_manager() != nullptr) {
                duplicate = mgr.sync_manager()->duplicate_handle(
                    source, flags, access);
            } else if ((type == HandleType::Thread ||
                        source == kCurrentThread) &&
                       (options & kDuplicateSameAccess) != 0 &&
                       mgr.thread_manager() != nullptr) {
                duplicate = mgr.thread_manager()->duplicate_thread_handle(
                    source);
                if (duplicate == INVALID_HANDLE) duplicate = NULL_HANDLE;
            }

            if ((options & kDuplicateCloseSource) != 0) {
                (void)handles->close(source);
            }
            if (duplicate == NULL_HANDLE) {
                m.set_return(type == HandleType::Invalid
                    ? nt_status::STATUS_INVALID_HANDLE
                    : nt_status::STATUS_NOT_SUPPORTED);
                return;
            }
            const auto wrote = m.cpu_is_64bit()
                ? m.memory().write_u64(target_ptr, duplicate)
                : m.memory().write_u32(target_ptr, duplicate);
            if (wrote.is_err()) {
                (void)handles->close(duplicate);
                m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
                return;
            }
            m.set_return(nt_status::STATUS_SUCCESS);
        };

    // Mandatory basic-interception native services. A sandbox resolves
    // these before the target runs.
    add_unsupported_service(exports, ordinal, "NtOpenThread", 16, 0);
    add_unsupported_service(exports, ordinal, "NtOpenProcess", 16, 0);
    add_unsupported_service(exports, ordinal, "NtOpenProcessToken", 12, 2);
    exports.entries.push_back(ExportEntry{
        "NtSetInformationThread", ordinal++,
        [&mgr](Marshaler& m) {
            if (static_cast<u32>(m.arg(1)) == kThreadHideFromDebugger) {
                thread_hide_information(m, mgr, true);
                return;
            }
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ntdll!NtSetInformationThread: information class %u\n",
                    static_cast<u32>(m.arg(1)));
            }
            m.set_return(nt_status::STATUS_NOT_IMPLEMENTED);
        }, 16
    });
    add_unsupported_service(exports, ordinal, "NtOpenThreadToken", 16, 3);
    add_unsupported_service(exports, ordinal, "NtOpenProcessTokenEx", 16, 3);
    add_unsupported_service(exports, ordinal, "NtOpenThreadTokenEx", 20, 4);
    add_unsupported_service(exports, ordinal, "NtSuspendProcess", 4);
    add_unsupported_service(exports, ordinal, "NtResumeProcess", 4);

    // Policy-selected services used by filesystem, registry, and
    // signed-section dispatchers. NtCreateFile, NtOpenFile,
    // NtSetInformationFile, and NtCreateKey already have implementations.
    add_unsupported_service(exports, ordinal, "NtQueryAttributesFile", 8);
    add_unsupported_service(exports, ordinal, "NtQueryFullAttributesFile", 8);
    add_unsupported_service(exports, ordinal, "NtOpenKey", 12, 0);
    add_unsupported_service(exports, ordinal, "NtOpenKeyEx", 16, 0);

    // Object-manager directory lookup is a native service in every supported
    // Windows user-mode ntdll.  Keep the export visible even while Vitreo's
    // object namespace is incomplete; calls fail explicitly and leave the
    // output handle null rather than masquerading as a successful open.
    add_unsupported_service(exports, ordinal, "NtOpenDirectoryObject", 12, 0);
    add_unsupported_service(exports, ordinal, "NtQueryDirectoryObject", 28);
    add_unsupported_service(exports, ordinal, "NtOpenSymbolicLinkObject", 12, 0);
    add_unsupported_service(exports, ordinal, "NtQuerySymbolicLinkObject", 12);
    exports.entries.push_back(ExportEntry{
        "NtDuplicateObject", ordinal++, nt_duplicate_object, 28});
    add_unsupported_service(exports, ordinal, "NtTerminateThread", 8);
}

} // namespace vitreo::winapi
