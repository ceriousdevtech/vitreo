// SPDX-License-Identifier: LGPL-2.1-only
/// @file scm_service_side_exports.cpp
/// @brief advapi32 SCM exports used by the service process itself:
///        StartServiceCtrlDispatcher, RegisterServiceCtrlHandler(Ex) and
///        SetServiceStatus.

#include "winapi/advapi32/services/scm_internal.hpp"
#include <atomic>

namespace vitreo::winapi {

using namespace scm_detail;
using namespace service_constants;

void register_scm_service_side_exports(ScmManager& scm, DllExports& exports) {
    // =========================================================================
    // Service-side APIs — used by the service process itself.
    // =========================================================================

    // ---- BOOL StartServiceCtrlDispatcher{A,W}(const SERVICE_TABLE_ENTRY*)
    // 1 arg, stdcall_bytes=4.
    //
    // The current service process hosts one OWN_PROCESS service. A second
    // entry needs the shared-process dispatch/control transport; fail visibly
    // instead of choosing the first entry and pretending the table ran.
    auto dispatcher = [&scm](Marshaler& m, bool wide) {
        u32 error = 0;
        if (!scm.begin_dispatcher(error)) {
            set_last_error(error); m.set_return(0); return;
        }
        const GuestAddr table = m.arg_ptr(0);
        if (table == 0) {
            set_last_error(error_codes::ERROR_INVALID_DATA); m.set_return(0); return;
        }
        const bool is64 = m.cpu_is_64bit();
        const usize pointer_size = is64 ? sizeof(u64) : sizeof(u32);
        GuestAddr service_main = 0;
        for (usize index = 0; index < 2; ++index) {
            const GuestAddr entry = table + index * pointer_size * 2;
            auto name = m.read_ptr(entry);
            auto procedure = m.read_ptr(entry + pointer_size);
            if (name.is_err() || procedure.is_err()) {
                (void)m.raise_access_violation(false, name.is_err() ? entry : entry + pointer_size);
                m.set_return(0); return;
            }
            if (name.value() == 0 && procedure.value() == 0) break;
            if (procedure.value() == 0) {
                set_last_error(error_codes::ERROR_INVALID_DATA); m.set_return(0); return;
            }
            if (index != 0) {
                static std::atomic_flag reported = ATOMIC_FLAG_INIT;
                if (!reported.test_and_set()) std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED advapi32.dll!StartServiceCtrlDispatcher%s: shared-process service table\n",
                    wide ? "W" : "A");
                set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED); m.set_return(0); return;
            }
            service_main = procedure.value();
            // OWN_PROCESS ignores lpServiceName, including its contents.
        }
        const bool ok = scm.run_connected_dispatcher(service_main,
            scm.own_service_name(), is64, wide, error);
        set_last_error(ok ? error_codes::ERROR_SUCCESS : error);
        m.set_return(ok ? 1u : 0u);
    };
    exports.entries.push_back({"StartServiceCtrlDispatcherA", 0,
        [dispatcher](Marshaler& m) { dispatcher(m, false); }, 4});
    exports.entries.push_back({"StartServiceCtrlDispatcherW", 0,
        [dispatcher](Marshaler& m) { dispatcher(m, true); }, 4});

    // ---- SERVICE_STATUS_HANDLE RegisterServiceCtrlHandler{A,W}(name, handler)
    // 2 args, stdcall_bytes=8.
    auto register_handler = [&scm](Marshaler& m) {
        const GuestAddr handler = m.arg_ptr(1);
        if (handler == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        const u32 handle = scm.register_control_handler(handler, 0, false);
        m.set_return(handle);
    };
    exports.entries.push_back({"RegisterServiceCtrlHandlerA", 0,
        register_handler, 8});
    exports.entries.push_back({"RegisterServiceCtrlHandlerW", 0,
        register_handler, 8});

    // ---- SERVICE_STATUS_HANDLE RegisterServiceCtrlHandlerEx{A,W}(name,
    //          handler, context)
    // 3 args, stdcall_bytes=12.
    auto register_handler_ex = [&scm](Marshaler& m) {
        const GuestAddr handler = m.arg_ptr(1);
        if (handler == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        const u32 handle =
            scm.register_control_handler(handler, m.arg_ptr(2), true);
        m.set_return(handle);
    };
    exports.entries.push_back({"RegisterServiceCtrlHandlerExA", 0,
        register_handler_ex, 12});
    exports.entries.push_back({"RegisterServiceCtrlHandlerExW", 0,
        register_handler_ex, 12});

    // ---- BOOL SetServiceStatus(SERVICE_STATUS_HANDLE, LPSERVICE_STATUS)
    // 2 args, stdcall_bytes=8.
    exports.entries.push_back({"SetServiceStatus", 0,
        [&scm](Marshaler& m) {
            const u32 handle = static_cast<u32>(m.arg(0));
            const GuestAddr status_ptr = m.arg_ptr(1);
            if (status_ptr == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            const ServiceStatus status = read_service_status(m, status_ptr);
            if (!scm.set_status(handle, status)) {
                m.set_return(0);
                return;
            }
            set_last_error(error_codes::ERROR_SUCCESS);
            m.set_return(1);
        }, 8});
}

} // namespace vitreo::winapi
