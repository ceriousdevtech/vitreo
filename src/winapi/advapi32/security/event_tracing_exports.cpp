// SPDX-License-Identifier: LGPL-2.1-only

/// @file event_tracing_exports.cpp
/// @brief advapi32 event-source, event-log and event-tracing exports.
///
/// The two functions are separate because a long run of other exports
/// (impersonation, CryptoAPI, token operations, Credential Manager, ...) sit
/// between them in the registration order (see security_registration.cpp);
/// regrouping them would move thunk addresses.
///
/// Registration order is ABI-visible: thunk addresses follow entries[] order
/// and the by-name table keeps the last of any duplicate names. The call order
/// lives in security_registration.cpp; this file only supplies registrations.

#include "capsule/vfs/vfs.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_exports.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <os/log.h>

namespace vitreo::winapi {

/// Registers DeregisterEventSource, RegisterEventSourceW, ReportEventW.
void add_event_source_exports(DllExports& exports) {
    // BOOL DeregisterEventSource(HANDLE hEventLog)
    // 1 arg, stdcall_bytes=4
    exports.entries.push_back(
        {"DeregisterEventSource", 21, [](Marshaler& m) {
            if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
            VITREO_TRACE( "[advapi32] DeregisterEventSource -> TRUE\n");
            m.set_return(1); // TRUE
        }, 4});

    // HANDLE RegisterEventSourceW(LPCWSTR lpUNCServerName, LPCWSTR lpSourceName)
    // 2 args, stdcall_bytes=8
    exports.entries.push_back(
        {"RegisterEventSourceW", 22, [](Marshaler& m) {
            if (m.arg(1) == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0u); return; }
            auto src_name = m.read_wide_string(m.arg_ptr(1));
            if (src_name.is_ok()) {
                os_log_info(OS_LOG_DEFAULT, "RegisterEventSourceW: source='%{public}s'",
                            src_name.value().c_str());
            }
            VITREO_TRACE( "[advapi32] RegisterEventSourceW -> 0xEEEE0001\n");
            m.set_return(0xEEEE0001); // Fake handle
        }, 8});

    // BOOL ReportEventW(HANDLE hEventLog, WORD wType, WORD wCategory,
    //                   DWORD dwEventID, PSID lpUserSid, WORD wNumStrings,
    //                   DWORD dwDataSize, LPCWSTR* lpStrings, LPVOID lpRawData)
    // 9 args, stdcall_bytes=36
    exports.entries.push_back(
        {"ReportEventW", 23, [](Marshaler& m) {
            if (m.arg(0) == 0) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            u32 event_type = static_cast<u32>(m.arg(1));
            u32 event_id = static_cast<u32>(m.arg(3));
            // Map Windows event types to os_log levels
            switch (event_type) {
                case 0x0001: // EVENTLOG_ERROR_TYPE
                    os_log_error(OS_LOG_DEFAULT,
                        "Guest ReportEventW: ERROR id=%{public}u", event_id);
                    break;
                case 0x0002: // EVENTLOG_WARNING_TYPE
                    os_log_info(OS_LOG_DEFAULT,
                        "Guest ReportEventW: WARNING id=%{public}u", event_id);
                    break;
                default: // EVENTLOG_INFORMATION_TYPE (0x0004) and others
                    os_log_info(OS_LOG_DEFAULT,
                        "Guest ReportEventW: INFO type=0x%{public}x id=%{public}u",
                        event_type, event_id);
                    break;
            }
            VITREO_TRACE( "[advapi32] ReportEventW(type=0x%x, id=%u) -> TRUE (os_log)\n",
                         event_type, event_id);
            m.set_return(1); // TRUE
        }, 36});
}

/// Registers the ETW / trace APIs (RegisterTraceGuidsW ..
/// GetTraceEnableFlags), OpenEventLogA, CloseEventLog and ReadEventLogW.
void add_event_tracing_exports(DllExports& exports,
                               SecurityStubs& stubs,
                               capsule::Vfs* vfs) {
    // ULONG RegisterTraceGuidsW(WMIDPREQUEST,PVOID,LPCGUID,ULONG,
    //   PTRACE_GUID_REGISTRATION,LPCWSTR,LPCWSTR,PTRACEHANDLE)
    exports.entries.push_back({"RegisterTraceGuidsW", 0,
        [&stubs](Marshaler& m) {
            (void)m.arg_ptr(0); // callback is invoked only by an active session
            (void)m.arg_ptr(1);
            const GuestAddr control_guid = m.arg_ptr(2);
            (void)m.arg(3);
            (void)m.arg_ptr(4);
            (void)m.arg_ptr(5);
            (void)m.arg_ptr(6);
            const GuestAddr handle_out = m.arg_ptr(7);
            if (control_guid == 0 || handle_out == 0) {
                m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                return;
            }
            const u64 handle = stubs.register_trace_provider();
            (void)m.write_u64(handle_out, handle);
            m.set_return(error_codes::ERROR_SUCCESS);
        }, 32});

    // ULONG UnregisterTraceGuids(TRACEHANDLE)
    exports.entries.push_back({"UnregisterTraceGuids", 0,
        [&stubs](Marshaler& m) {
            const u64 handle = m.cpu_is_64bit()
                ? m.arg(0)
                : m.arg(0) | (m.arg(1) << 32);
            m.set_return(stubs.unregister_trace_provider(handle)
                ? error_codes::ERROR_SUCCESS
                : error_codes::ERROR_INVALID_HANDLE);
        }, 8});

    // ULONG StartTraceA(PTRACEHANDLE, LPCSTR, PEVENT_TRACE_PROPERTIES)
    // https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-starttracea
    // NULL TraceHandle or Properties is ERROR_INVALID_PARAMETER. Vitreo does
    // not host ETW sessions; controlling a session requires the documented
    // Performance Log Users / admin right, so the start is ACCESS_DENIED.
    exports.entries.push_back({"StartTraceA", 0,
        [](Marshaler& m) {
            const GuestAddr trace_handle = m.arg_ptr(0);
            (void)m.arg_ptr(1);
            const GuestAddr properties = m.arg_ptr(2);
            if (trace_handle == 0 || properties == 0) {
                m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                return;
            }
            m.set_return(error_codes::ERROR_ACCESS_DENIED);
        }, 12});
    exports.entries.push_back({"StartTraceW", 0,
        [](Marshaler& m) {
            const GuestAddr trace_handle = m.arg_ptr(0);
            (void)m.arg_ptr(1);
            const GuestAddr properties = m.arg_ptr(2);
            if (trace_handle == 0 || properties == 0) {
                m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                return;
            }
            m.set_return(error_codes::ERROR_ACCESS_DENIED);
        }, 12});

    // ---------------------------------------------------------------
    // Classic ETW controller / consumer surface. The capsule hosts no ETW
    // session, so every call reports exactly what Windows reports for a
    // session that is not running (tools/oracle_probe/advapi_etw_probe).
    // ---------------------------------------------------------------
    struct EtwArgs {
        // A 64-bit handle is one x64 register, or two x86 stack slots.
        static u64 u64_at(Marshaler& m, usize index) {
            return m.cpu_is_64bit() ? m.arg(index)
                                    : (m.arg(index) & 0xFFFFFFFFull) | (m.arg(index + 1) << 32);
        }
        static usize after_u64(Marshaler& m, usize index) {
            return index + (m.cpu_is_64bit() ? 1 : 2);
        }
    };

    // ULONG ControlTrace{A,W}(CONTROLTRACE_ID TraceId, LPCTSTR InstanceName,
    //   PEVENT_TRACE_PROPERTIES Properties, ULONG ControlCode)
    // https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-controltracew
    // x86 stdcall 20. The result is also the last error (probe ctw_*). With
    // no session running: a named session -> ERROR_WMI_INSTANCE_NOT_FOUND,
    // a handle alone -> ERROR_INVALID_HANDLE, neither -> ERROR_INVALID_PARAMETER.
    auto control_trace = [](Marshaler& m, u64 trace_id, GuestAddr name, GuestAddr props,
                            u32 code, bool wide) -> u32 {
        constexpr u32 ERROR_WMI_INSTANCE_NOT_FOUND = 4201;
        constexpr u32 kPropsSize = 120;  // sizeof(EVENT_TRACE_PROPERTIES), both bitnesses
        if (props == 0) return error_codes::ERROR_INVALID_PARAMETER;
        // EVENT_TRACE_CONTROL_QUERY..CONVERT_TO_REALTIME (0..5).
        if (code > 5) return error_codes::ERROR_INVALID_PARAMETER;
        // WOW64 refuses EVENT_TRACE_CONTROL_CONVERT_TO_REALTIME (x86 probe
        // ctw_code_5: ERROR_NOACCESS).
        if (code == 5 && !m.cpu_is_64bit()) return error_codes::ERROR_NOACCESS;
        const u32 size = m.read_u32(props).value_or(0);   // Wnode.BufferSize
        if (size < kPropsSize) return error_codes::ERROR_BAD_LENGTH;
        const u32 file_off = m.read_u32(props + (m.cpu_is_64bit() ? 112 : 108)).value_or(0);
        const u32 name_off = m.read_u32(props + (m.cpu_is_64bit() ? 116 : 112)).value_or(0);
        auto bad_offset = [&](u32 off) { return off != 0 && (off < kPropsSize || off > size); };
        if (bad_offset(name_off) || bad_offset(file_off)) return error_codes::ERROR_INVALID_PARAMETER;
        // Wnode.Guid == SystemTraceControlGuid needs the kernel logger's name
        // (probe ctw_systemguid_wrongname). evntrace.h:
        // 9e814aad-3204-11d2-9a82-006008a86939.
        static constexpr u8 kSystemTraceControlGuid[16] = {
            0xad, 0x4a, 0x81, 0x9e, 0x04, 0x32, 0xd2, 0x11,
            0x9a, 0x82, 0x00, 0x60, 0x08, 0xa8, 0x69, 0x39};
        u8 guid[16] = {};
        (void)m.memory().read_bytes(props + 24, guid, 16);   // WNODE_HEADER.Guid
        if (std::memcmp(guid, kSystemTraceControlGuid, 16) == 0) {
            std::string n;
            if (name != 0) {
                auto r = wide ? m.read_wide_string(name) : m.read_ansi_string(name);
                if (r.is_ok()) n = r.value();
            }
            if (n != "NT Kernel Logger") return error_codes::ERROR_INVALID_PARAMETER;
        }
        if (name == 0) {
            return trace_id == 0 ? error_codes::ERROR_INVALID_PARAMETER
                                 : error_codes::ERROR_INVALID_HANDLE;
        }
        return ERROR_WMI_INSTANCE_NOT_FOUND;
    };
    auto make_control = [control_trace](bool wide, bool fixed_code, u32 code) {
        return [control_trace, wide, fixed_code, code](Marshaler& m) {
            const u64 trace_id = EtwArgs::u64_at(m, 0);
            const usize i = EtwArgs::after_u64(m, 0);
            const GuestAddr name = m.arg_ptr(i);
            const GuestAddr props = m.arg_ptr(i + 1);
            const u32 control = fixed_code ? code : m.arg32(i + 2);
            const u32 err = control_trace(m, trace_id, name, props, control, wide);
            set_last_error(err);
            m.set_return(err);
        };
    };
    exports.entries.push_back({"ControlTraceA", 0, make_control(false, false, 0), 20});
    exports.entries.push_back({"ControlTraceW", 0, make_control(true, false, 0), 20});
    // StopTrace/QueryTrace/FlushTrace are ControlTrace with a fixed code
    // (Learn "supersedes"; probe stopa_name / stopw_* / queryw_name / flushw_name).
    exports.entries.push_back({"StopTraceA", 0, make_control(false, true, 1), 16});
    exports.entries.push_back({"StopTraceW", 0, make_control(true, true, 1), 16});
    exports.entries.push_back({"QueryTraceA", 0, make_control(false, true, 0), 16});
    exports.entries.push_back({"QueryTraceW", 0, make_control(true, true, 0), 16});
    exports.entries.push_back({"FlushTraceA", 0, make_control(false, true, 3), 16});
    exports.entries.push_back({"FlushTraceW", 0, make_control(true, true, 3), 16});

    auto read_reg_handle = [](Marshaler& m) {
        return m.cpu_is_64bit() ? m.arg(0) : m.arg(0) | (m.arg(1) << 32);
    };

    // Modern manifest-based ETW. No trace sessions are attached to the
    // capsule, so providers are registered but remain disabled.
    exports.entries.push_back({"EventRegister", 0,
        [&stubs](Marshaler& m) {
            GuestAddr provider_id = m.arg_ptr(0);
            GuestAddr handle_out = m.arg_ptr(3);
            if (provider_id == 0 || handle_out == 0) {
                // Parameter validation fails before RegHandle is touched:
                // Windows leaves it unchanged (advapi_token_probe evreg_null).
                m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                return;
            }
            u64 handle = stubs.register_trace_provider();
            if (m.write_u64(handle_out, handle).is_err()) {
                (void)stubs.unregister_trace_provider(handle);
                m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                return;
            }
            m.set_return(error_codes::ERROR_SUCCESS);
        }, 16});
    exports.entries.push_back({"EventUnregister", 0,
        [&stubs, read_reg_handle](Marshaler& m) {
            m.set_return(stubs.unregister_trace_provider(read_reg_handle(m))
                ? error_codes::ERROR_SUCCESS
                : error_codes::ERROR_INVALID_HANDLE);
        }, 8});
    // x86 stdcall byte counts from evntprov.h (REGHANDLE is a ULONGLONG,
    // 8 bytes on the x86 stack): EventWrite(REGHANDLE, PCEVENT_DESCRIPTOR,
    // ULONG, PEVENT_DATA_DESCRIPTOR) = 20; EventWriteTransfer adds two
    // LPCGUIDs = 28; EventProviderEnabled(REGHANDLE, UCHAR, ULONGLONG) = 20.
    exports.entries.push_back({"EventWrite", 0,
        [&stubs, read_reg_handle](Marshaler& m) {
            m.set_return(stubs.is_trace_provider_registered(read_reg_handle(m))
                ? error_codes::ERROR_SUCCESS
                : error_codes::ERROR_INVALID_HANDLE);
        }, 20});
    exports.entries.push_back({"EventWriteTransfer", 0,
        [&stubs, read_reg_handle](Marshaler& m) {
            m.set_return(stubs.is_trace_provider_registered(read_reg_handle(m))
                ? error_codes::ERROR_SUCCESS
                : error_codes::ERROR_INVALID_HANDLE);
        }, 28});
    exports.entries.push_back({"EventSetInformation", 0,
        [&stubs, read_reg_handle](Marshaler& m) {
            m.set_return(stubs.is_trace_provider_registered(read_reg_handle(m))
                ? error_codes::ERROR_SUCCESS
                : error_codes::ERROR_INVALID_HANDLE);
        }, 20});
    exports.entries.push_back({"EventEnabled", 0,
        [](Marshaler& m) { m.set_return(0); }, 12});
    exports.entries.push_back({"EventProviderEnabled", 0,
        [](Marshaler& m) { m.set_return(0); }, 20});
    exports.entries.push_back({"EventWriteEx", 0,
        [&stubs, read_reg_handle](Marshaler& m) {
            m.set_return(stubs.is_trace_provider_registered(read_reg_handle(m))
                ? error_codes::ERROR_SUCCESS
                : error_codes::ERROR_INVALID_HANDLE);
        }, 40});
    exports.entries.push_back({"EventWriteString", 0,
        [&stubs, read_reg_handle](Marshaler& m) {
            m.set_return(stubs.is_trace_provider_registered(read_reg_handle(m))
                ? error_codes::ERROR_SUCCESS
                : error_codes::ERROR_INVALID_HANDLE);
        }, 24});

    // ULONG EventActivityIdControl(ULONG ControlCode, LPGUID ActivityId)
    // https://learn.microsoft.com/en-us/windows/win32/api/evntprov/nf-evntprov-eventactivityidcontrol
    // The activity id is per-thread state ETW keeps for the caller, and it
    // is real state even with no session attached: .NET's EventSource sets
    // an id, runs work, then restores the previous one, so a call that
    // silently did nothing would corrupt its activity nesting.
    exports.entries.push_back({"EventActivityIdControl", 0,
        [](Marshaler& m) {
            constexpr u32 kGetId = 1, kSetId = 2, kCreateId = 3;
            constexpr u32 kGetSetId = 4, kCreateSetId = 5;
            struct ActivityGuid { u8 bytes[16]; };
            thread_local ActivityGuid t_activity{};
            thread_local u32 t_next_activity = 1;

            const u32 code = static_cast<u32>(m.arg(0));
            const GuestAddr id = m.arg_ptr(1);
            if (id == 0) {
                m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                return;
            }
            auto make_new = [] {
                ActivityGuid g{};
                const u32 n = t_next_activity++;
                std::memcpy(g.bytes, &n, sizeof(n));
                return g;
            };
            switch (code) {
            case kGetId:
                if (m.write_struct(id, t_activity).is_err()) {
                    m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                    return;
                }
                break;
            case kSetId: {
                auto in = m.read_struct<ActivityGuid>(id);
                if (in.is_err()) {
                    m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                    return;
                }
                t_activity = in.value();
                break;
            }
            case kCreateId: {
                const ActivityGuid g = make_new();
                if (m.write_struct(id, g).is_err()) {
                    m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                    return;
                }
                break;
            }
            case kGetSetId: {
                // Swap: the caller's buffer receives the previous id.
                auto in = m.read_struct<ActivityGuid>(id);
                if (in.is_err() ||
                    m.write_struct(id, t_activity).is_err()) {
                    m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                    return;
                }
                t_activity = in.value();
                break;
            }
            case kCreateSetId: {
                const ActivityGuid previous = t_activity;
                t_activity = make_new();
                if (m.write_struct(id, previous).is_err()) {
                    m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                    return;
                }
                break;
            }
            default:
                m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                return;
            }
            m.set_return(error_codes::ERROR_SUCCESS);
        }, 8});

    // HANDLE OpenEventLogA(LPCSTR lpUNCServerName, LPCSTR lpSourceName)
    // https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-openeventloga
    // No event-log service is present; NULL + ERROR_FILE_NOT_FOUND is the
    // documented failure for a missing source, not a fabricated handle.
    exports.entries.push_back({"OpenEventLogA", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0);
            (void)m.arg_ptr(1);
            set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
            m.set_return(0);
        }, 8});
    exports.entries.push_back({"CloseEventLog", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
        }, 4});
    exports.entries.push_back({"ReadEventLogW", 0,
        [](Marshaler& m) {
            for (usize i = 0; i < 7; ++i) (void)m.arg(i);
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
        }, 28});

    // PROCESSTRACE_HANDLE OpenTrace{A,W}(PEVENT_TRACE_LOGFILE Logfile)
    // https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-opentracew
    // x86 stdcall 4; the 64-bit result is EDX:EAX. INVALID_PROCESSTRACE_HANDLE
    // is (UINT64)UINTPTR_MAX: ~0 on x64, 0x00000000FFFFFFFF on x86. Measured
    // (advapi_etw_probe open*): NULL -> ERROR_INVALID_PARAMETER; a real-time
    // session name opens (ERROR_SUCCESS) even when that session is not
    // running — ProcessTrace then fails; no real-time name and no file ->
    // ERROR_PATH_NOT_FOUND. Reading an .etl log file is not implemented.
    auto open_trace = [&stubs, vfs](Marshaler& m, bool wide) {
        constexpr u32 PROCESS_TRACE_MODE_REAL_TIME = 0x00000100;
        const u64 invalid = m.cpu_is_64bit() ? ~0ull : 0xFFFFFFFFull;
        const GuestAddr logfile = m.arg_ptr(0);
        if (logfile == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return_i64(invalid);
            return;
        }
        const u32 ptr = m.cpu_is_64bit() ? 8u : 4u;
        const GuestAddr file_name = static_cast<GuestAddr>(m.read_ptr(logfile).value_or(0));
        const GuestAddr logger_name = static_cast<GuestAddr>(m.read_ptr(logfile + ptr).value_or(0));
        const u32 mode = m.read_u32(logfile + (m.cpu_is_64bit() ? 28 : 20)).value_or(0);
        if ((mode & PROCESS_TRACE_MODE_REAL_TIME) != 0 && logger_name != 0) {
            const u64 handle = stubs.open_trace_consumer();
            set_last_error(error_codes::ERROR_SUCCESS);
            m.set_return_i64(handle);
            return;
        }
        if (file_name == 0) {
            set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
            m.set_return_i64(invalid);
            return;
        }
        auto path = wide ? m.read_wide_string(file_name) : m.read_ansi_string(file_name);
        u32 err = error_codes::ERROR_PATH_NOT_FOUND;
        if (path.is_ok() && vfs != nullptr) {
            const std::string& p = path.value();
            const usize slash = p.find_last_of("\\/");
            const bool dir_exists = slash == std::string::npos ||
                                    vfs->stat(p.substr(0, slash)).is_ok();
            if (!dir_exists) err = error_codes::ERROR_PATH_NOT_FOUND;
            else if (vfs->stat(p).is_err()) err = error_codes::ERROR_FILE_NOT_FOUND;
            else {
                static std::atomic<bool> logged{false};
                if (!logged.exchange(true)) {
                    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED advapi32!OpenTrace%c: "
                                 "reading an .etl log file\n", wide ? 'W' : 'A');
                }
                err = error_codes::ERROR_NOT_SUPPORTED;
            }
        }
        set_last_error(err);
        m.set_return_i64(invalid);
    };
    exports.entries.push_back({"OpenTraceA", 0,
        [open_trace](Marshaler& m) { open_trace(m, false); }, 4});
    exports.entries.push_back({"OpenTraceW", 0,
        [open_trace](Marshaler& m) { open_trace(m, true); }, 4});
    // ULONG CloseTrace(PROCESSTRACE_HANDLE) — x86 stdcall 8. Last error untouched.
    exports.entries.push_back({"CloseTrace", 0,
        [&stubs](Marshaler& m) {
            const u64 handle = EtwArgs::u64_at(m, 0);
            m.set_return(stubs.close_trace_consumer(handle) ? error_codes::ERROR_SUCCESS
                                                            : error_codes::ERROR_INVALID_HANDLE);
        }, 8});
    // ULONG ProcessTrace(PPROCESSTRACE_HANDLE, ULONG HandleCount, LPFILETIME,
    // LPFILETIME) — x86 stdcall 16. Probe processtrace_*: no handles ->
    // ERROR_BAD_LENGTH, an unknown handle -> ERROR_INVALID_HANDLE (last error
    // untouched), an opened real-time session that is not running ->
    // ERROR_WMI_INSTANCE_NOT_FOUND (also the last error).
    exports.entries.push_back({"ProcessTrace", 0,
        [&stubs](Marshaler& m) {
            constexpr u32 ERROR_WMI_INSTANCE_NOT_FOUND = 4201;
            const GuestAddr handles = m.arg_ptr(0);
            const u32 count = m.arg32(1);
            if (count == 0) {
                m.set_return(error_codes::ERROR_BAD_LENGTH);
                return;
            }
            if (handles == 0) {
                m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                return;
            }
            for (u32 i = 0; i < count; ++i) {
                const u64 h = m.read_u64(handles + i * 8).value_or(0);
                if (!stubs.is_trace_consumer(h)) {
                    m.set_return(error_codes::ERROR_INVALID_HANDLE);
                    return;
                }
            }
            set_last_error(ERROR_WMI_INSTANCE_NOT_FOUND);
            m.set_return(ERROR_WMI_INSTANCE_NOT_FOUND);
        }, 16});
    // ULONG EnableTrace(ULONG Enable, ULONG EnableFlag, ULONG EnableLevel,
    //   LPCGUID ControlGuid, TRACEHANDLE TraceHandle) — x86 stdcall 24.
    // With no session every call is ERROR_INVALID_PARAMETER (also the last
    // error), except that WOW64 accepts TraceHandle (ULONG64)-1 and returns
    // ERROR_SUCCESS (probe enabletrace_*, .x86.expected enabletrace_minus1).
    exports.entries.push_back({"EnableTrace", 0,
        [](Marshaler& m) {
            const u64 handle = EtwArgs::u64_at(m, 4);
            if (!m.cpu_is_64bit() && handle == ~0ull) {
                m.set_return(error_codes::ERROR_SUCCESS);
                return;
            }
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        }, 24});
    // ULONG TraceMessage(TRACEHANDLE, ULONG, LPCGUID, USHORT, ...) — cdecl
    // (WMIAPIV), no stdcall cleanup. ULONG TraceMessageVa(..., va_list) —
    // stdcall 24 on x86. No logger session exists: ERROR_INVALID_HANDLE with
    // the last error untouched (probe tracemessage_*).
    exports.entries.push_back({"TraceMessage", 0,
        [](Marshaler& m) {
            (void)EtwArgs::u64_at(m, 0);
            m.set_return(error_codes::ERROR_INVALID_HANDLE);
        }, 0});
    exports.entries.push_back({"TraceMessageVa", 0,
        [](Marshaler& m) {
            (void)EtwArgs::u64_at(m, 0);
            m.set_return(error_codes::ERROR_INVALID_HANDLE);
        }, 24});
    // ULONG TraceEvent(TRACEHANDLE, PEVENT_TRACE_HEADER) — x86 stdcall 12.
    // NULL header -> ERROR_INVALID_PARAMETER, otherwise no session ->
    // ERROR_INVALID_HANDLE; last error untouched (probe traceevent_*).
    exports.entries.push_back({"TraceEvent", 0,
        [](Marshaler& m) {
            const usize i = EtwArgs::after_u64(m, 0);
            m.set_return(m.arg_ptr(i) == 0 ? error_codes::ERROR_INVALID_PARAMETER
                                           : error_codes::ERROR_INVALID_HANDLE);
        }, 12});
    // TRACEHANDLE GetTraceLoggerHandle(PVOID Buffer) — x86 stdcall 4, EDX:EAX.
    // Buffer is the WNODE_HEADER a provider's control callback received; its
    // HistoricalContext names the logger. 0 -> 0 (last error untouched);
    // anything else names no session -> INVALID_HANDLE_VALUE (x86: EDX = 0)
    // and ERROR_INVALID_HANDLE (probe gettraceloggerhandle_*).
    exports.entries.push_back({"GetTraceLoggerHandle", 0,
        [](Marshaler& m) {
            const GuestAddr buffer = m.arg_ptr(0);
            const u64 context = buffer == 0 ? 1 : m.read_u64(buffer + 8).value_or(1);
            if (context == 0) {
                m.set_return_i64(0);
                return;
            }
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return_i64(m.cpu_is_64bit() ? ~0ull : 0xFFFFFFFFull);
        }, 4});
    // UCHAR GetTraceEnableLevel(TRACEHANDLE) / ULONG GetTraceEnableFlags(
    // TRACEHANDLE) — x86 stdcall 8. No logger: 0 with ERROR_INVALID_HANDLE.
    exports.entries.push_back({"GetTraceEnableLevel", 0,
        [](Marshaler& m) {
            (void)EtwArgs::u64_at(m, 0);
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
        }, 8});
    exports.entries.push_back({"GetTraceEnableFlags", 0,
        [](Marshaler& m) {
            (void)EtwArgs::u64_at(m, 0);
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
        }, 8});
}

} // namespace vitreo::winapi
