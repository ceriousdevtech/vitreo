// SPDX-License-Identifier: LGPL-2.1-only
/// @file scm_status_control_exports.cpp
/// @brief advapi32 SCM exports: QueryServiceStatus(Ex), the service security
///        descriptor calls, ControlService, DeleteService and
///        CloseServiceHandle.

#include "winapi/advapi32/services/scm_internal.hpp"

namespace vitreo::winapi {

using namespace scm_detail;
using namespace service_constants;

namespace {

std::optional<std::vector<u8>> capture_service_dacl(
    Marshaler& m, GuestAddr descriptor, u32& error) {
    constexpr u16 SE_DACL_PRESENT = 0x0004;
    constexpr u16 SE_SELF_RELATIVE = 0x8000;

    auto revision = m.read_u8(descriptor);
    auto control = m.read_u16(descriptor + 2);
    if (revision.is_err() || control.is_err() || revision.value() != 1) {
        error = error_codes::ERROR_INVALID_DATA;
        return std::nullopt;
    }

    const bool is_relative = (control.value() & SE_SELF_RELATIVE) != 0;
    const GuestAddr dacl_field =
        descriptor + (is_relative ? 16u : (m.cpu_is_64bit() ? 32u : 16u));
    GuestAddr dacl = 0;
    if (is_relative) {
        auto offset = m.read_u32(dacl_field);
        if (offset.is_err()) {
            error = error_codes::ERROR_INVALID_DATA;
            return std::nullopt;
        }
        if (offset.value() != 0) dacl = descriptor + offset.value();
    } else if (m.cpu_is_64bit()) {
        auto pointer = m.read_u64(dacl_field);
        if (pointer.is_err()) {
            error = error_codes::ERROR_INVALID_DATA;
            return std::nullopt;
        }
        dacl = static_cast<GuestAddr>(pointer.value());
    } else {
        auto pointer = m.read_u32(dacl_field);
        if (pointer.is_err()) {
            error = error_codes::ERROR_INVALID_DATA;
            return std::nullopt;
        }
        dacl = pointer.value();
    }

    u32 acl_size = 0;
    if (dacl != 0) {
        auto size = m.read_u16(dacl + 2);
        if (size.is_err() || size.value() < 8) {
            error = error_codes::ERROR_INVALID_DATA;
            return std::nullopt;
        }
        acl_size = size.value();
    }

    std::vector<u8> result(20 + acl_size, 0);
    result[0] = 1;
    const u16 stored_control = static_cast<u16>(
        SE_SELF_RELATIVE | (control.value() & SE_DACL_PRESENT));
    result[2] = static_cast<u8>(stored_control & 0xFFu);
    result[3] = static_cast<u8>(stored_control >> 8);
    if (acl_size != 0) {
        result[16] = 20;
        if (m.memory().read_bytes(dacl, result.data() + 20, acl_size).is_err()) {
            error = error_codes::ERROR_NOACCESS;
            return std::nullopt;
        }
    }
    error = error_codes::ERROR_SUCCESS;
    return result;
}

} // namespace

void register_scm_status_control_exports(ScmManager& scm, DllExports& exports) {
    // ---- BOOL QueryServiceStatus(SC_HANDLE, LPSERVICE_STATUS)
    // 2 args, stdcall_bytes=8.
    exports.entries.push_back({"QueryServiceStatus", 0,
        [&scm](Marshaler& m) {
            auto name = resolve_service_arg(scm, static_cast<u32>(m.arg(0)));
            if (!name) { m.set_return(0); return; }
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            const ServiceStatus status = scm.query_status(*name);
            write_service_status(m, out, status, false);
            VITREO_TRACE("[scm] QueryServiceStatus '%s' -> state=%u\n",
                         to_narrow(*name).c_str(), status.current_state);
            m.set_return(1);
        }, 8});

    // ---- BOOL QueryServiceStatusEx(SC_HANDLE, SC_STATUS_TYPE, LPBYTE buffer,
    //          DWORD cbBufSize, LPDWORD pcbBytesNeeded)
    // 5 args, stdcall_bytes=20.
    exports.entries.push_back({"QueryServiceStatusEx", 0,
        [&scm](Marshaler& m) {
            auto name = resolve_service_arg(scm, static_cast<u32>(m.arg(0)));
            if (!name) { m.set_return(0); return; }
            const u32 info_level = static_cast<u32>(m.arg(1));
            if (info_level != SC_STATUS_PROCESS_INFO) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            const GuestAddr buffer = m.arg_ptr(2);
            const u32 buffer_size = static_cast<u32>(m.arg(3));
            const GuestAddr needed_ptr = m.arg_ptr(4);
            if (needed_ptr != 0) {
                (void)m.write_u32(needed_ptr, SERVICE_STATUS_PROCESS_SIZE);
            }
            if (buffer == 0 || buffer_size < SERVICE_STATUS_PROCESS_SIZE) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }
            const ServiceStatus status = scm.query_status(*name);
            write_service_status(m, buffer, status, true);
            VITREO_TRACE("[scm] QueryServiceStatusEx '%s' -> state=%u pid=%u\n",
                         to_narrow(*name).c_str(), status.current_state,
                         status.process_id);
            m.set_return(1);
        }, 20});

    // ---- BOOL QueryServiceObjectSecurity(SC_HANDLE, SECURITY_INFORMATION,
    //          PSECURITY_DESCRIPTOR, DWORD, LPDWORD)
    // 5 args, stdcall_bytes=20.
    exports.entries.push_back({"QueryServiceObjectSecurity", 0,
        [&scm](Marshaler& m) {
            auto name = resolve_service_arg(scm, static_cast<u32>(m.arg(0)));
            if (!name) { m.set_return(0); return; }
            const u32 information = static_cast<u32>(m.arg(1));
            constexpr u32 supported = 0x1u | 0x2u | 0x4u;
            if (information == 0 || (information & ~supported) != 0) {
                set_last_error((information & 0x8u) != 0
                    ? error_codes::ERROR_ACCESS_DENIED
                    : error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            const GuestAddr buffer = m.arg_ptr(2);
            const u32 buffer_size = static_cast<u32>(m.arg(3));
            const GuestAddr needed_ptr = m.arg_ptr(4);
            if (needed_ptr == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            auto descriptor = scm.query_security_descriptor(*name);
            if (!descriptor) {
                set_last_error(error_codes::ERROR_SERVICE_DOES_NOT_EXIST);
                m.set_return(0);
                return;
            }
            const u32 needed = static_cast<u32>(descriptor->size());
            (void)m.write_u32(needed_ptr, needed);
            if (buffer == 0 || buffer_size < needed) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }
            if (m.memory().write_bytes(buffer, descriptor->data(),
                                       descriptor->size()).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            VITREO_TRACE("[scm] QueryServiceObjectSecurity '%s' info=0x%x "
                         "size=%u\n", to_narrow(*name).c_str(), information,
                         needed);
            m.set_return(1);
        }, 20});

    // ---- BOOL SetServiceObjectSecurity(SC_HANDLE, SECURITY_INFORMATION,
    //          PSECURITY_DESCRIPTOR)
    // 3 args, stdcall_bytes=12. The helper-service path updates the DACL. Store
    // it self-relative so it remains valid after the guest buffer is released.
    exports.entries.push_back({"SetServiceObjectSecurity", 0,
        [&scm](Marshaler& m) {
            auto name = resolve_service_arg(scm, static_cast<u32>(m.arg(0)));
            if (!name) { m.set_return(0); return; }
            const u32 information = static_cast<u32>(m.arg(1));
            const GuestAddr descriptor_ptr = m.arg_ptr(2);
            constexpr u32 DACL_SECURITY_INFORMATION = 0x4;
            if (information != DACL_SECURITY_INFORMATION ||
                descriptor_ptr == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            u32 error = 0;
            auto descriptor =
                capture_service_dacl(m, descriptor_ptr, error);
            if (!descriptor ||
                !scm.set_security_descriptor(*name, *descriptor, error)) {
                set_last_error(error);
                m.set_return(0);
                return;
            }
            VITREO_TRACE("[scm] SetServiceObjectSecurity '%s' DACL size=%zu\n",
                         to_narrow(*name).c_str(), descriptor->size());
            set_last_error(error_codes::ERROR_SUCCESS);
            m.set_return(1);
        }, 12});

    // ---- BOOL ControlService(SC_HANDLE, DWORD control, LPSERVICE_STATUS)
    // 3 args, stdcall_bytes=12.
    exports.entries.push_back({"ControlService", 0,
        [&scm](Marshaler& m) {
            auto name = resolve_service_arg(scm, static_cast<u32>(m.arg(0)));
            if (!name) { m.set_return(0); return; }
            const u32 control = static_cast<u32>(m.arg(1));
            const GuestAddr out = m.arg_ptr(2);

            ServiceStatus status;
            u32 error = 0;
            const bool ok = scm.control_service(*name, control, status, error);
            if (out != 0) write_service_status(m, out, status, false);
            set_last_error(ok ? error_codes::ERROR_SUCCESS : error);
            m.set_return(ok ? 1u : 0u);
        }, 12});

    // ---- BOOL DeleteService(SC_HANDLE)
    // 1 arg, stdcall_bytes=4.
    exports.entries.push_back({"DeleteService", 0,
        [&scm](Marshaler& m) {
            auto name = resolve_service_arg(scm, static_cast<u32>(m.arg(0)));
            if (!name) { m.set_return(0); return; }
            u32 error = 0;
            if (!scm.delete_service(*name, error)) {
                set_last_error(error);
                m.set_return(0);
                return;
            }
            set_last_error(error_codes::ERROR_SUCCESS);
            m.set_return(1);
        }, 4});

    // ---- BOOL CloseServiceHandle(SC_HANDLE)
    // 1 arg, stdcall_bytes=4.
    exports.entries.push_back({"CloseServiceHandle", 0,
        [&scm](Marshaler& m) {
            const u32 handle = static_cast<u32>(m.arg(0));
            if (!scm.close_handle(handle)) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            m.set_return(1);
        }, 4});
}

} // namespace vitreo::winapi
