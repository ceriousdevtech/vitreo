/// @file wtsapi32.cpp
/// @brief Remote Desktop Services session-information contracts.

#include "wtsapi32.hpp"

#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace vitreo::winapi {
namespace {

constexpr u64 WTS_CURRENT_SERVER_HANDLE = 0;
constexpr u32 WTS_CURRENT_SESSION = 0xFFFFFFFFu;
constexpr u32 VITREO_SESSION_ID = 1;
constexpr u32 WTS_ACTIVE = 0;
constexpr u32 WTS_SESSIONSTATE_UNLOCK = 1;
constexpr usize WINSTATION_NAME_CAPACITY = 33;
constexpr usize USER_NAME_CAPACITY = 21;
constexpr usize DOMAIN_NAME_CAPACITY = 18;

enum class WtsInfoClass : u32 {
    InitialProgram = 0,
    ApplicationName = 1,
    WorkingDirectory = 2,
    SessionId = 4,
    UserName = 5,
    WinStationName = 6,
    DomainName = 7,
    ConnectState = 8,
    ClientName = 10,
    ClientDirectory = 11,
    ClientHardwareId = 13,
    ClientProtocolType = 16,
    SessionInfo = 24,
    SessionInfoEx = 25,
    IsRemoteSession = 29,
};

std::string environment_or(
    const EnvironmentManager& environment,
    std::string_view name,
    std::string_view fallback
) {
    const auto value = environment.get_variable(name);
    return value.is_ok() ? value.value() : std::string(fallback);
}

std::vector<Byte> scalar_payload(u64 value, usize size) {
    std::vector<Byte> bytes(size);
    for (usize index = 0; index < size; ++index) {
        bytes[index] = static_cast<Byte>((value >> (index * 8)) & 0xFFu);
    }
    return bytes;
}

std::vector<Byte> string_payload(std::string_view value, bool wide) {
    if (!wide) {
        std::vector<Byte> bytes(value.size() + 1);
        std::memcpy(bytes.data(), value.data(), value.size());
        return bytes;
    }

    const std::vector<u16> utf16 = utf8_to_utf16(value);
    std::vector<Byte> bytes((utf16.size() + 1) * sizeof(u16));
    for (usize index = 0; index < utf16.size(); ++index) {
        bytes[index * 2] = static_cast<Byte>(utf16[index] & 0xFFu);
        bytes[index * 2 + 1] =
            static_cast<Byte>((utf16[index] >> 8) & 0xFFu);
    }
    return bytes;
}

void write_le(std::vector<Byte>& bytes, usize offset, u64 value, usize size) {
    for (usize index = 0; index < size; ++index) {
        bytes[offset + index] =
            static_cast<Byte>((value >> (index * 8)) & 0xFFu);
    }
}

void write_fixed_string(
    std::vector<Byte>& bytes,
    usize offset,
    usize capacity,
    std::string_view value,
    bool wide
) {
    if (!wide) {
        const usize count = std::min(value.size(), capacity - 1);
        std::memcpy(bytes.data() + offset, value.data(), count);
        return;
    }

    const std::vector<u16> utf16 = utf8_to_utf16(value);
    const usize count = std::min(utf16.size(), capacity - 1);
    for (usize index = 0; index < count; ++index) {
        write_le(bytes, offset + index * sizeof(u16), utf16[index],
                 sizeof(u16));
    }
}

u64 current_filetime() {
    constexpr u64 WINDOWS_TO_UNIX_EPOCH_100NS = 116444736000000000ULL;
    const auto since_unix_epoch =
        std::chrono::system_clock::now().time_since_epoch();
    const auto ticks = std::chrono::duration_cast<
        std::chrono::duration<i64, std::ratio<1, 10000000>>>(
            since_unix_epoch).count();
    return WINDOWS_TO_UNIX_EPOCH_100NS + static_cast<u64>(ticks);
}

std::vector<Byte> session_info_ex_payload(
    bool wide,
    const EnvironmentManager& environment
) {
    // WTSINFOEX contains a DWORD Level followed by an 8-byte-aligned
    // WTSINFOEX_LEVEL1 union. The fixed strings use the exact capacities from
    // wtsapi32.h, including their terminating code unit.
    constexpr usize OUTER_DATA_OFFSET = 8;
    const usize winstation_offset = OUTER_DATA_OFFSET + 12;
    const usize user_offset = winstation_offset +
        WINSTATION_NAME_CAPACITY * (wide ? sizeof(u16) : sizeof(char));
    const usize domain_offset = user_offset +
        USER_NAME_CAPACITY * (wide ? sizeof(u16) : sizeof(char));
    const usize strings_end = domain_offset +
        DOMAIN_NAME_CAPACITY * (wide ? sizeof(u16) : sizeof(char));
    const usize times_offset = (strings_end + 7u) & ~usize{7u};
    constexpr usize TIME_FIELD_COUNT = 5;
    constexpr usize COUNTER_FIELD_COUNT = 6;
    const usize counters_offset = times_offset + TIME_FIELD_COUNT * sizeof(u64);
    const usize payload_size =
        counters_offset + COUNTER_FIELD_COUNT * sizeof(u32);

    std::vector<Byte> payload(payload_size);
    write_le(payload, 0, 1, sizeof(u32)); // WTSINFOEX level 1
    write_le(payload, OUTER_DATA_OFFSET, VITREO_SESSION_ID, sizeof(u32));
    write_le(payload, OUTER_DATA_OFFSET + 4, WTS_ACTIVE, sizeof(u32));
    write_le(payload, OUTER_DATA_OFFSET + 8, WTS_SESSIONSTATE_UNLOCK,
             sizeof(u32));

    write_fixed_string(payload, winstation_offset,
                       WINSTATION_NAME_CAPACITY, "Console", wide);
    write_fixed_string(
        payload, user_offset, USER_NAME_CAPACITY,
        environment_or(environment, "USERNAME", "Default"), wide);
    write_fixed_string(
        payload, domain_offset, DOMAIN_NAME_CAPACITY,
        environment_or(environment, "USERDOMAIN",
                       environment_or(environment, "COMPUTERNAME", "VITREO")),
        wide);

    const u64 now = current_filetime();
    // Vitreo owns one local console session for the lifetime of the process.
    // It has no RDP connect/disconnect traffic, so only the current timestamp
    // is populated; unavailable historical timestamps and traffic counters
    // remain zero.
    write_le(payload, times_offset + 4 * sizeof(u64), now, sizeof(u64));
    return payload;
}

std::vector<Byte> session_info_payload(
    bool wide,
    const EnvironmentManager& environment
) {
    // WTSINFO (wtsapi32.h): 8 DWORDs, WinStationName[32], Domain[17],
    // UserName[21], then five 8-byte-aligned LARGE_INTEGER times.
    // W = 216 bytes (measured pBytesReturned 0xD8), A = 144.
    const usize unit = wide ? sizeof(u16) : sizeof(char);
    constexpr usize winstation_offset = 32;
    const usize domain_offset = winstation_offset + 32 * unit;
    const usize user_offset = domain_offset + 17 * unit;
    const usize times_offset = (user_offset + 21 * unit + 7u) & ~usize{7u};
    std::vector<Byte> payload(times_offset + 5 * sizeof(u64));
    write_le(payload, 0, WTS_ACTIVE, sizeof(u32));
    write_le(payload, 4, VITREO_SESSION_ID, sizeof(u32));
    write_fixed_string(payload, winstation_offset, 32, "Console", wide);
    write_fixed_string(
        payload, domain_offset, 17,
        environment_or(environment, "USERDOMAIN",
                       environment_or(environment, "COMPUTERNAME", "VITREO")),
        wide);
    write_fixed_string(
        payload, user_offset, 21,
        environment_or(environment, "USERNAME", "Default"), wide);
    // Same policy as WTSINFOEX: one local console session with no RDP
    // history, so only CurrentTime is populated.
    write_le(payload, times_offset + 4 * sizeof(u64), current_filetime(),
             sizeof(u64));
    return payload;
}

bool make_payload(
    WtsInfoClass info_class,
    bool wide,
    const EnvironmentManager& environment,
    std::vector<Byte>& payload
) {
    switch (info_class) {
    case WtsInfoClass::InitialProgram:
    case WtsInfoClass::ApplicationName:
    case WtsInfoClass::WorkingDirectory:
    case WtsInfoClass::ClientName:
    case WtsInfoClass::ClientDirectory:
        payload = string_payload("", wide);
        return true;
    case WtsInfoClass::SessionId:
        payload = scalar_payload(VITREO_SESSION_ID, sizeof(u32));
        return true;
    case WtsInfoClass::UserName:
        payload = string_payload(
            environment_or(environment, "USERNAME", "Default"), wide);
        return true;
    case WtsInfoClass::WinStationName:
        payload = string_payload("Console", wide);
        return true;
    case WtsInfoClass::DomainName:
        payload = string_payload(
            environment_or(environment, "USERDOMAIN",
                           environment_or(
                               environment, "COMPUTERNAME", "VITREO")),
            wide);
        return true;
    case WtsInfoClass::ConnectState:
        // WTSActive: a user is logged on and connected to this console session.
        payload = scalar_payload(0, sizeof(u32));
        return true;
    case WtsInfoClass::ClientHardwareId:
        // Reserved by Windows and documented to return zero.
        payload = scalar_payload(0, sizeof(u32));
        return true;
    case WtsInfoClass::ClientProtocolType:
        // Zero identifies the physical console rather than RDP.
        payload = scalar_payload(0, sizeof(u16));
        return true;
    case WtsInfoClass::SessionInfo:
        payload = session_info_payload(wide, environment);
        return true;
    case WtsInfoClass::SessionInfoEx:
        payload = session_info_ex_payload(wide, environment);
        return true;
    case WtsInfoClass::IsRemoteSession:
        // A BOOLEAN: one byte (measured pBytesReturned == 1).
        payload = scalar_payload(0, 1);
        return true;
    default:
        return false;
    }
}

void query_session_information(
    Marshaler& m,
    GlobalLocalManager& allocations,
    const EnvironmentManager& environment,
    bool wide
) {
    (void)m.arg(0);  // hServer: not validated (measured: 0x1234 -> TRUE)
    const u32 session_id = static_cast<u32>(m.arg(1));
    const u32 raw_class = static_cast<u32>(m.arg(2));
    const auto info_class = static_cast<WtsInfoClass>(raw_class);
    const GuestAddr buffer_out = m.arg_ptr(3);
    const GuestAddr bytes_out = m.arg_ptr(4);

    // Measured on Windows 11 (tools/oracle_probe/s7_misc_probe.expected,
    // interactive session, x86 = x64). Every failure leaves *ppBuffer and
    // *pBytesReturned untouched.
    //  - NULL ppBuffer/pBytesReturned: ERROR_INVALID_USER_BUFFER.
    //  - class > WTSIsRemoteSession (29): ERROR_INVALID_PARAMETER.
    //  - WTSIdleTime..WTSOutgoingFrames (17-22, "not used"):
    //    ERROR_NOT_SUPPORTED, for any session.
    //  - WTSSessionId echoes the requested id, even for a session that
    //    does not exist; every other class on such a session is
    //    ERROR_FILE_NOT_FOUND.
    if (buffer_out == 0 || bytes_out == 0) {
        set_last_error(1784u);  // ERROR_INVALID_USER_BUFFER
        m.set_return(0);
        return;
    }
    if (raw_class > static_cast<u32>(WtsInfoClass::IsRemoteSession)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (raw_class >= 17 && raw_class <= 22) {
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        m.set_return(0);
        return;
    }
    const bool current = session_id == WTS_CURRENT_SESSION ||
                         session_id == VITREO_SESSION_ID;
    std::vector<Byte> payload;
    if (info_class == WtsInfoClass::SessionId) {
        payload = scalar_payload(current ? VITREO_SESSION_ID : session_id,
                                 sizeof(u32));
    } else if (!current) {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        m.set_return(0);
        return;
    } else if (!make_payload(info_class, wide, environment, payload)) {
        static std::once_flag once;
        std::call_once(once, [raw_class] {
            std::fprintf(stderr,
                "[vitreo] UNIMPLEMENTED wtsapi32!WTSQuerySessionInformation: "
                "info class %u\n", raw_class);
        });
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        m.set_return(0);
        return;
    }

    const u64 allocation = allocations.global_alloc(GPTR, payload.size());
    if (allocation == 0) {
        m.set_return(0);
        return;
    }
    bool wrote_payload = true;
    for (usize index = 0; index < payload.size(); ++index) {
        if (m.write_u8(allocation + index, payload[index]).is_err()) {
            wrote_payload = false;
            break;
        }
    }
    if (!wrote_payload ||
        m.write_ptr(buffer_out, allocation).is_err() ||
        m.write_u32(bytes_out, static_cast<u32>(payload.size())).is_err()) {
        (void)allocations.global_free(allocation);
        (void)m.write_ptr(buffer_out, 0);
        (void)m.write_u32(bytes_out, 0);
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    // Success leaves ERROR_NO_TOKEN behind for the classes that look up
    // the session user, and clears it for the protocol/remote queries
    // (measured); WTSSessionId leaves it untouched.
    switch (info_class) {
    case WtsInfoClass::ConnectState:
    case WtsInfoClass::UserName:
    case WtsInfoClass::WinStationName:
    case WtsInfoClass::SessionInfo:
        set_last_error(1008u);  // ERROR_NO_TOKEN
        break;
    case WtsInfoClass::ClientProtocolType:
    case WtsInfoClass::IsRemoteSession:
        set_last_error(0);
        break;
    default:
        break;
    }
    VITREO_TRACE(
        "[wtsapi32] WTSQuerySessionInformation%c session=%u class=%u "
        "-> TRUE buffer=0x%llx bytes=%zu\n",
        wide ? 'W' : 'A', session_id, static_cast<u32>(info_class),
        static_cast<unsigned long long>(allocation), payload.size());
    m.set_return(1);
}

} // namespace

DllExports create_wtsapi32_thunks(
    GlobalLocalManager& allocations,
    const EnvironmentManager& environment
) {
    DllExports exports;
    exports.entries.reserve(5);
    exports.entries.push_back({
        "WTSGetActiveConsoleSessionId", 0,
        [](Marshaler& m) { m.set_return(VITREO_SESSION_ID); }, 0});
    exports.entries.push_back({
        "WTSQuerySessionInformationW", 0,
        [&allocations, &environment](Marshaler& m) {
            query_session_information(
                m, allocations, environment, true);
        },
        20});
    exports.entries.push_back({
        "WTSQuerySessionInformationA", 0,
        [&allocations, &environment](Marshaler& m) {
            query_session_information(
                m, allocations, environment, false);
        },
        20});
    exports.entries.push_back({
        "WTSFreeMemory", 0,
        [&allocations](Marshaler& m) {
            (void)allocations.global_free(m.arg_ptr(0));
        },
        4});
    return exports;
}

} // namespace vitreo::winapi
