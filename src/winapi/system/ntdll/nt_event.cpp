/// @file nt_event.cpp
/// @brief ntdll event services.
///
/// Learn (ZwCreateEvent, ZwOpenEvent, ZwSetEvent, ZwClearEvent,
/// ZwQueryEvent's EVENT_BASIC_INFORMATION) documents the prototypes. The
/// rules below that Learn does not state are measured, x64 and x86, by
/// tools/oracle_probe/nt_event_probe:
///  * EventType 0 is NotificationEvent (manual reset), 1 SynchronizationEvent;
///    anything else is STATUS_INVALID_PARAMETER. InitialState is a BOOLEAN:
///    only its low byte counts.
///  * OBJECT_ATTRIBUTES.Length must be the structure size
///    (STATUS_INVALID_PARAMETER); an empty ObjectName is an unnamed event; a
///    name with no RootDirectory that does not start with '\' is
///    STATUS_OBJECT_PATH_SYNTAX_BAD; a directory that does not exist is
///    STATUS_OBJECT_PATH_NOT_FOUND.
///  * a name already taken by an event is STATUS_OBJECT_NAME_COLLISION, or
///    with OBJ_OPENIF a handle to it and STATUS_OBJECT_NAME_EXISTS; taken by
///    another type it is STATUS_OBJECT_TYPE_MISMATCH. NtOpenEvent of a free
///    name is STATUS_OBJECT_NAME_NOT_FOUND.
///  * set / reset / pulse report PreviousState; a handle without
///    EVENT_MODIFY_STATE (or EVENT_QUERY_STATE for a query) is
///    STATUS_ACCESS_DENIED, a non-event STATUS_OBJECT_TYPE_MISMATCH, a bad or
///    NULL handle STATUS_INVALID_HANDLE.
///  * NtQueryEvent: class 0 only (STATUS_INVALID_INFO_CLASS), Length exactly
///    8 (STATUS_INFO_LENGTH_MISMATCH), ReturnLength untouched on failure.
/// None of them touches the thread's Win32 last error.

#include "winapi/system/ntdll/nt_event.hpp"

#include "winapi/common/object_access_constants.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"

#include <vector>

namespace vitreo::winapi::ntdll {

namespace {

constexpr u32 kStatusSuccess = 0x00000000u;
constexpr u32 kStatusObjectNameExists = 0x40000000u;
constexpr u32 kStatusInvalidInfoClass = 0xC0000003u;
constexpr u32 kStatusInfoLengthMismatch = 0xC0000004u;
constexpr u32 kStatusInvalidHandle = 0xC0000008u;
constexpr u32 kStatusInvalidParameter = 0xC000000Du;
constexpr u32 kStatusNoMemory = 0xC0000017u;
constexpr u32 kStatusAccessDenied = 0xC0000022u;
constexpr u32 kStatusObjectTypeMismatch = 0xC0000024u;
constexpr u32 kStatusObjectNameInvalid = 0xC0000033u;
constexpr u32 kStatusObjectNameNotFound = 0xC0000034u;
constexpr u32 kStatusObjectNameCollision = 0xC0000035u;
constexpr u32 kStatusObjectPathNotFound = 0xC000003Au;
constexpr u32 kStatusObjectPathSyntaxBad = 0xC000003Bu;

constexpr u32 kObjInherit = 0x00000002u;
constexpr u32 kObjOpenIf = 0x00000080u;
constexpr u32 kEventQueryState = 0x00000001u;

constexpr std::string_view kGlobalDirectory = "\\BaseNamedObjects\\";

std::string session_directory() {
    return "\\Sessions\\" + std::to_string(kVitreoSessionId) + "\\BaseNamedObjects\\";
}

/// What OBJECT_ATTRIBUTES says about the object to create or open.
struct ObjectRequest {
    bool inherit{false};
    bool open_if{false};
    std::string name;  ///< Win32 SyncManager key; empty = unnamed
};

/// Read OBJECT_ATTRIBUTES (winternl): Length, RootDirectory, ObjectName,
/// Attributes at pointer-sized strides; UNICODE_STRING Length then Buffer.
u32 read_object_attributes(Marshaler& m, HandleTable& handles, GuestAddr oa,
                           ObjectRequest& request) {
    if (oa == 0) return kStatusSuccess;
    const bool is64 = m.cpu_is_64bit();
    const u32 ptr = is64 ? 8u : 4u;
    const auto length = m.read_u32(oa);
    if (length.is_err() || length.value() != (is64 ? 48u : 24u)) return kStatusInvalidParameter;
    const auto root = m.read_ptr(oa + ptr);
    const auto name_ptr = m.read_ptr(oa + 2 * ptr);
    const auto attributes = m.read_u32(oa + 3 * ptr);
    if (root.is_err() || name_ptr.is_err() || attributes.is_err()) return kStatusInvalidParameter;
    request.inherit = (attributes.value() & kObjInherit) != 0;
    request.open_if = (attributes.value() & kObjOpenIf) != 0;

    std::string nt_name;
    if (name_ptr.value() != 0) {
        const auto bytes = m.read_u16(name_ptr.value());
        const auto buffer = m.read_ptr(name_ptr.value() + ptr);
        if (bytes.is_err() || buffer.is_err()) return kStatusInvalidParameter;
        if ((bytes.value() & 1u) != 0) return kStatusObjectNameInvalid;
        std::vector<u16> text(bytes.value() / 2u);
        for (usize i = 0; i < text.size(); ++i) {
            const auto c = m.read_u16(buffer.value() + i * 2u);
            if (c.is_err()) return kStatusInvalidParameter;
            text[i] = c.value();
        }
        if (!text.empty()) nt_name = utf16_to_utf8(text.data(), static_cast<int>(text.size()));
    }
    if (root.value() != 0) {
        // Vitreo has no object directories (NtOpenDirectoryObject is an
        // unsupported service), so a RootDirectory is never one: Windows
        // rejects any other handle type as a mismatch.
        return handles.get_type(static_cast<Handle>(root.value())) == HandleType::Invalid
                   ? kStatusInvalidHandle
                   : kStatusObjectTypeMismatch;
    }
    if (nt_name.empty()) return kStatusSuccess;
    return nt_object_path_to_win32(nt_name, request.name);
}

/// The status for using @p h as an event that needs @p access.
u32 event_handle_status(SyncManager& sync, HandleTable& handles, Handle h, u32 access) {
    const HandleType type = handles.get_type(h);
    if (type == HandleType::Invalid) return kStatusInvalidHandle;
    if (type != HandleType::Event || sync.is_waitable_timer(h)) return kStatusObjectTypeMismatch;
    const u32 granted = handles.get_access(h);
    if (granted != HandleAccess::All && (granted & access) == 0) return kStatusAccessDenied;
    return kStatusSuccess;
}

/// Run a SyncManager call without leaving its Win32 last error behind.
struct PreserveLastError {
    u32 saved = get_last_error();
    ~PreserveLastError() { set_last_error(saved); }
};

void write_handle(Marshaler& m, GuestAddr out, Handle h) {
    (void)m.write_ptr(out, static_cast<u64>(h));
}

/// NtSetEvent / NtResetEvent / NtPulseEvent (HANDLE, PLONG PreviousState).
template <typename Op>
void change_event(Marshaler& m, SyncManager& sync, HandleTable& handles, Op op) {
    const auto h = static_cast<Handle>(m.arg(0));
    const GuestAddr previous_out = m.arg_ptr(1);
    const u32 status = event_handle_status(sync, handles, h, sync_access::EVENT_MODIFY_STATE);
    if (status != kStatusSuccess) {
        m.set_return(status);
        return;
    }
    bool previous = false;
    bool ok = false;
    {
        PreserveLastError keep;
        ok = op(h, &previous);
    }
    if (!ok) {
        m.set_return(kStatusInvalidHandle);
        return;
    }
    if (previous_out != 0) (void)m.write_u32(previous_out, previous ? 1u : 0u);
    m.set_return(kStatusSuccess);
}

}  // namespace

u32 nt_object_path_to_win32(std::string_view nt_path, std::string& win32_name) {
    win32_name.clear();
    if (nt_path.empty()) return kStatusSuccess;
    if (nt_path.front() != '\\') return kStatusObjectPathSyntaxBad;
    // Inside a BaseNamedObjects directory, "Global\", "Local\" and
    // "Session\" are symbolic links; Vitreo honours the first two.
    const auto leaf = [&](std::string_view rest, bool global) -> u32 {
        if (rest.starts_with("Global\\")) {
            rest.remove_prefix(7);
            global = true;
        } else if (rest.starts_with("Local\\")) {
            rest.remove_prefix(6);
        }
        if (rest.empty()) return kStatusObjectNameInvalid;
        if (rest.find('\\') != std::string_view::npos) return kStatusObjectPathNotFound;
        win32_name = global ? "Global\\" + std::string(rest) : std::string(rest);
        return kStatusSuccess;
    };
    const std::string session = session_directory();
    if (nt_path.starts_with(session)) return leaf(nt_path.substr(session.size()), false);
    if (nt_path.starts_with(kGlobalDirectory)) {
        return leaf(nt_path.substr(kGlobalDirectory.size()), true);
    }
    return kStatusObjectPathNotFound;
}

std::string win32_object_name_to_nt(std::string_view win32_name) {
    if (win32_name.starts_with("Global\\")) {
        return std::string(kGlobalDirectory) + std::string(win32_name.substr(7));
    }
    if (win32_name.starts_with("Local\\")) win32_name.remove_prefix(6);
    return session_directory() + std::string(win32_name);
}

DllExports make_nt_event_exports(SyncManager& sync, HandleTable& handles) {
    DllExports exports;

    // NTSTATUS NtCreateEvent(PHANDLE EventHandle, ACCESS_MASK DesiredAccess,
    //     POBJECT_ATTRIBUTES, EVENT_TYPE EventType, BOOLEAN InitialState)
    exports.entries.push_back({"NtCreateEvent", 0, [&sync, &handles](Marshaler& m) {
        const GuestAddr handle_out = m.arg_ptr(0);
        const u32 access = static_cast<u32>(m.arg(1));
        const GuestAddr oa = m.arg_ptr(2);
        const u32 type = static_cast<u32>(m.arg(3));
        const bool initial = (m.arg(4) & 0xFFu) != 0;
        if (type > 1) {
            m.set_return(kStatusInvalidParameter);
            return;
        }
        ObjectRequest request;
        if (const u32 st = read_object_attributes(m, handles, oa, request); st != kStatusSuccess) {
            m.set_return(st);
            return;
        }
        Handle h = NULL_HANDLE;
        u32 error = 0;
        {
            PreserveLastError keep;
            set_last_error(0);
            h = sync.create_event(/*manual_reset=*/type == 0, initial, request.name, request.inherit,
                                  {}, access);
            error = get_last_error();
        }
        if (h == NULL_HANDLE) {
            m.set_return(error == error_codes::ERROR_INVALID_HANDLE  ? kStatusObjectTypeMismatch
                         : error == error_codes::ERROR_ACCESS_DENIED ? kStatusAccessDenied
                                                                     : kStatusNoMemory);
            return;
        }
        u32 status = kStatusSuccess;
        if (error == error_codes::ERROR_ALREADY_EXISTS) {
            if (!request.open_if) {
                PreserveLastError keep;
                (void)sync.close_handle(h);
                m.set_return(kStatusObjectNameCollision);
                return;
            }
            status = kStatusObjectNameExists;
        }
        write_handle(m, handle_out, h);
        m.set_return(status);
    }, 20});

    // NTSTATUS NtOpenEvent(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES)
    exports.entries.push_back({"NtOpenEvent", 0, [&sync, &handles](Marshaler& m) {
        const GuestAddr handle_out = m.arg_ptr(0);
        const u32 access = static_cast<u32>(m.arg(1));
        ObjectRequest request;
        if (const u32 st = read_object_attributes(m, handles, m.arg_ptr(2), request);
            st != kStatusSuccess) {
            m.set_return(st);
            return;
        }
        if (request.name.empty()) {
            m.set_return(kStatusObjectPathSyntaxBad);
            return;
        }
        Handle h = NULL_HANDLE;
        u32 error = 0;
        {
            PreserveLastError keep;
            h = sync.open_event(access, request.name, request.inherit);
            error = get_last_error();
        }
        if (h == NULL_HANDLE) {
            m.set_return(error == error_codes::ERROR_INVALID_HANDLE  ? kStatusObjectTypeMismatch
                         : error == error_codes::ERROR_ACCESS_DENIED ? kStatusAccessDenied
                                                                     : kStatusObjectNameNotFound);
            return;
        }
        write_handle(m, handle_out, h);
        m.set_return(kStatusSuccess);
    }, 12});

    exports.entries.push_back({"NtSetEvent", 0, [&sync, &handles](Marshaler& m) {
        change_event(m, sync, handles,
                     [&sync](Handle h, bool* previous) { return sync.set_event(h, {}, previous); });
    }, 8});
    exports.entries.push_back({"NtResetEvent", 0, [&sync, &handles](Marshaler& m) {
        change_event(m, sync, handles,
                     [&sync](Handle h, bool* previous) { return sync.reset_event(h, {}, previous); });
    }, 8});
    exports.entries.push_back({"NtPulseEvent", 0, [&sync, &handles](Marshaler& m) {
        change_event(m, sync, handles,
                     [&sync](Handle h, bool* previous) { return sync.pulse_event(h, {}, previous); });
    }, 8});
    // NTSTATUS NtClearEvent(HANDLE): NtResetEvent without PreviousState.
    exports.entries.push_back({"NtClearEvent", 0, [&sync, &handles](Marshaler& m) {
        const auto h = static_cast<Handle>(m.arg(0));
        const u32 status = event_handle_status(sync, handles, h, sync_access::EVENT_MODIFY_STATE);
        if (status != kStatusSuccess) {
            m.set_return(status);
            return;
        }
        PreserveLastError keep;
        m.set_return(sync.reset_event(h) ? kStatusSuccess : kStatusInvalidHandle);
    }, 4});

    // NTSTATUS NtQueryEvent(HANDLE, EVENT_INFORMATION_CLASS, PVOID, ULONG,
    //     PULONG ReturnLength): EventBasicInformation {EVENT_TYPE; LONG}.
    exports.entries.push_back({"NtQueryEvent", 0, [&sync, &handles](Marshaler& m) {
        const auto h = static_cast<Handle>(m.arg(0));
        const u32 info_class = static_cast<u32>(m.arg(1));
        const GuestAddr info = m.arg_ptr(2);
        const u32 length = static_cast<u32>(m.arg(3));
        const GuestAddr return_length = m.arg_ptr(4);
        if (info_class != 0) {
            m.set_return(kStatusInvalidInfoClass);
            return;
        }
        if (length != 8) {
            m.set_return(kStatusInfoLengthMismatch);
            return;
        }
        const u32 status = event_handle_status(sync, handles, h, kEventQueryState);
        if (status != kStatusSuccess) {
            m.set_return(status);
            return;
        }
        bool manual_reset = false;
        bool signaled = false;
        if (!sync.query_event(h, manual_reset, signaled)) {
            m.set_return(kStatusObjectTypeMismatch);
            return;
        }
        (void)m.write_u32(info, manual_reset ? 0u : 1u);
        (void)m.write_u32(info + 4, signaled ? 1u : 0u);
        if (return_length != 0) (void)m.write_u32(return_length, 8);
        m.set_return(kStatusSuccess);
    }, 20});

    return exports;
}

}  // namespace vitreo::winapi::ntdll
