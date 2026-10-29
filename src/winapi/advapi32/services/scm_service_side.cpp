// SPDX-License-Identifier: LGPL-2.1-only
/// @file scm_service_side.cpp
/// @brief ScmManager service-process side: the dispatcher, control-handler
///        registration and SetServiceStatus.

#include "winapi/advapi32/services/scm_internal.hpp"

#include <unistd.h>
#include <atomic>

namespace vitreo::winapi {

using namespace scm_detail;
using namespace service_constants;

namespace {

// Learn StartServiceW: before returning success the SCM sets START_PENDING
// with WaitHint 2 seconds. StartService itself returns when the service
// control dispatcher has created the ServiceMain thread, and fails with
// ERROR_SERVICE_REQUEST_TIMEOUT if the process started but never called
// StartServiceCtrlDispatcher. The same 30-second bound is the documented
// StartService block timeout when the SCM is busy handling a control.
constexpr u32 kScmStartPendingWaitHintMs = 2000;

} // namespace

// ---- Service side -----------------------------------------------------------

bool ScmManager::run_dispatcher(GuestAddr service_main,
                                const std::wstring& entry_name, bool is_64bit,
                                bool wide, u32& error) {
    if (!begin_dispatcher(error)) return false;
    return run_connected_dispatcher(service_main, entry_name, is_64bit, wide, error);
}

bool ScmManager::begin_dispatcher(u32& error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Windows consumes the one-shot connection even when the first call
    // fails with ERROR_FAILED_SERVICE_CONTROLLER_CONNECT (console oracle).
    if (m_dispatcher_running) {
        error = error_codes::ERROR_SERVICE_ALREADY_RUNNING;
        return false;
    }
    m_dispatcher_running = true;
    if (!launched_as_service()) {
        error = error_codes::ERROR_FAILED_SERVICE_CONTROLLER_CONNECT;
        return false;
    }
    return true;
}

bool ScmManager::run_connected_dispatcher(GuestAddr service_main,
                                         const std::wstring& entry_name,
                                         bool is_64bit, bool wide, u32& error) {
    if (service_main == 0) {
        error = error_codes::ERROR_INVALID_DATA;
        return false;
    }
    if (!m_service_thread_call) {
        static std::atomic_flag reported = ATOMIC_FLAG_INIT;
        if (!reported.test_and_set()) std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED advapi32.dll!StartServiceCtrlDispatcher: service-thread binding unavailable\n");
        error = error_codes::ERROR_SERVICE_NO_THREAD;
        return false;
    }

    const std::wstring& name =
        m_own_service_name.empty() ? entry_name : m_own_service_name;

    std::vector<std::wstring> service_arguments;
    service_arguments.reserve(m_own_service_arguments.size() + 1);
    service_arguments.push_back(name);
    service_arguments.insert(service_arguments.end(),
                             m_own_service_arguments.begin(),
                             m_own_service_arguments.end());
    VITREO_TRACE("[scm] StartServiceCtrlDispatcher: entering ServiceMain "
                 "0x%llx for '%s'\n",
                 static_cast<unsigned long long>(service_main),
                 to_narrow(name).c_str());

    // Report START_PENDING on the service's behalf. A service that never calls
    // SetServiceStatus would otherwise look stopped to its controller while it
    // is in fact starting. The handshake file is what lets StartService in the
    // controller return — Learn: it returns when ServiceMain is created.
    {
        ServiceStatus starting;
        starting.current_state = SERVICE_START_PENDING;
        starting.process_id = static_cast<u32>(::getpid());
        starting.wait_hint = kScmStartPendingWaitHintMs;
        starting.controls_accepted = 0;
        if (!write_status_file(name, starting)) {
            error = error_codes::ERROR_ACCESS_DENIED;
            return false;
        }
        if (!write_dispatcher_handshake(status_file_path(name))) {
            remove_status_file(name);
            error = error_codes::ERROR_ACCESS_DENIED;
            return false;
        }
    }

    // The calling thread is the service control dispatcher. Windows creates a
    // distinct thread for ServiceMain; this matters to guest thread-local
    // state established before StartServiceCtrlDispatcher is called.
    const bool thread_completed = m_service_thread_call(
        service_main, service_arguments, wide, is_64bit);

    if (!thread_completed) {
        // Failure to create/join the real guest thread must not leave a
        // readiness marker or invent SERVICE_STOPPED on the guest's behalf.
        remove_dispatcher_handshake(status_file_path(name));
        remove_status_file(name);
        error = error_codes::ERROR_SERVICE_NO_THREAD;
        return false;
    }

    // ServiceMain may return after registering a worker or wait callback.
    // Only SetServiceStatus(SERVICE_STOPPED), not a void thread return, ends
    // the service's lifetime (Learn StartServiceCtrlDispatcher Remarks).
    for (;;) {
        const auto status = read_status_file(name);
        if (status && status->current_state == SERVICE_STOPPED) break;
        if (!m_service_dispatch_wait) {
            static std::atomic_flag reported = ATOMIC_FLAG_INIT;
            if (!reported.test_and_set()) std::fprintf(stderr,
                "[vitreo] UNIMPLEMENTED advapi32.dll!StartServiceCtrlDispatcher: runtime status wait unavailable\n");
            remove_dispatcher_handshake(status_file_path(name));
            error = error_codes::ERROR_CALL_NOT_IMPLEMENTED;
            return false;
        }
        if (!m_service_dispatch_wait()) {
            remove_dispatcher_handshake(status_file_path(name));
            error = error_codes::ERROR_OPERATION_ABORTED;
            return false;
        }
    }
    remove_dispatcher_handshake(status_file_path(name));

    VITREO_TRACE("[scm] StartServiceCtrlDispatcher: '%s' reported STOPPED\n",
                 to_narrow(name).c_str());
    error = error_codes::ERROR_SUCCESS;
    return true;
}

u32 ScmManager::register_control_handler(GuestAddr handler, GuestAddr context,
                                         bool is_ex) {
    if (!launched_as_service()) {
        set_last_error(error_codes::ERROR_FAILED_SERVICE_CONTROLLER_CONNECT);
        return 0;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_handler_addr = handler;
    m_handler_context = context;
    m_handler_is_ex = is_ex;
    if (m_status_handle == 0) m_status_handle = m_next_handle++;
    VITREO_TRACE("[scm] RegisterServiceCtrlHandler%s handler=0x%llx -> 0x%x\n",
                 is_ex ? "Ex" : "",
                 static_cast<unsigned long long>(handler), m_status_handle);
    return m_status_handle;
}

bool ScmManager::set_status(u32 status_handle, const ServiceStatus& status) {
    if (!launched_as_service()) {
        set_last_error(error_codes::ERROR_FAILED_SERVICE_CONTROLLER_CONNECT);
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (status_handle == 0 || status_handle != m_status_handle) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }

    ServiceStatus published = status;
    // The service reports its own state; the pid is ours to fill in, and is
    // what lets a controller detect a service process that dies.
    published.process_id = published.current_state == SERVICE_STOPPED
                               ? 0u
                               : static_cast<u32>(::getpid());
    if (!write_status_file(m_own_service_name, published)) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return false;
    }
    VITREO_TRACE("[scm] SetServiceStatus '%s' state=%u accepted=0x%x "
                 "checkpoint=%u\n",
                 to_narrow(m_own_service_name).c_str(), published.current_state,
                 published.controls_accepted, published.check_point);
    return true;
}

} // namespace vitreo::winapi
