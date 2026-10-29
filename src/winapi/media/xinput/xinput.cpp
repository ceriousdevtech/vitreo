/// @file xinput.cpp
/// @brief XInput 1.4 / 1.3 / 9.1.0 exports.
///
/// https://learn.microsoft.com/en-us/windows/win32/api/xinput/nf-xinput-xinputgetstate
/// https://learn.microsoft.com/en-us/windows/win32/xinput/getting-started-with-xinput

#include "winapi/media/xinput/xinput.hpp"

#include "hardware/contracts/gamepad_host.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <span>
#include <string_view>

namespace vitreo::winapi {
namespace {

constexpr u32 kXUserMaxCount = 4;
constexpr u32 kErrorEmpty = 4306;

[[nodiscard]] bool input_diag_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("VITREO_INPUT_DIAG");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

class XInputRuntime {
public:
    explicit XInputRuntime(std::shared_ptr<hardware::GamepadHost> host)
        : m_host(std::move(host)) {}

    [[nodiscard]] u32 get_state(u32 index, hardware::GamepadState& state,
                                u32& packet) {
        if (index >= kXUserMaxCount) return error_codes::ERROR_BAD_ARGUMENTS;
        hardware::GamepadState current{};
        const bool query_ok = m_host && m_host->query_state(index, current);
        const u32 query_result = !query_ok ? error_codes::ERROR_GEN_FAILURE
            : !current.connected ? error_codes::ERROR_DEVICE_NOT_CONNECTED
                                 : error_codes::ERROR_SUCCESS;
        if (query_result != error_codes::ERROR_SUCCESS) {
            if (input_diag_enabled()) {
                std::scoped_lock lock(m_mutex);
                log_first_query(index, query_ok, current.connected, query_result);
            }
            return query_result;
        }

        std::scoped_lock lock(m_mutex);
        if (input_diag_enabled())
            log_first_query(index, query_ok, current.connected, query_result);
        if (!m_enabled) {
            current = hardware::GamepadState{};
            current.connected = true;
        }
        Slot& slot = m_slots[index];
        if (!slot.initialized || slot.state != current) {
            ++slot.packet;
            slot.state = current;
            slot.initialized = true;
            if (input_diag_enabled()) {
                std::fprintf(
                    stderr,
                    "[xinput] user=%u packet=%u buttons=0x%04x "
                    "triggers=(%u,%u) left=(%d,%d) right=(%d,%d)\n",
                    index, slot.packet, current.buttons,
                    current.left_trigger, current.right_trigger,
                    current.left_x, current.left_y,
                    current.right_x, current.right_y);
            }
        }
        state = current;
        packet = slot.packet;
        return error_codes::ERROR_SUCCESS;
    }

    [[nodiscard]] u32 query_device(u32 index, hardware::GamepadState& state) {
        if (index >= kXUserMaxCount) return error_codes::ERROR_BAD_ARGUMENTS;
        if (!m_host || !m_host->query_state(index, state)) {
            return error_codes::ERROR_GEN_FAILURE;
        }
        return state.connected ? error_codes::ERROR_SUCCESS
                               : error_codes::ERROR_DEVICE_NOT_CONNECTED;
    }

    void enable(bool enabled) {
        std::scoped_lock lock(m_mutex);
        m_enabled = enabled;
    }

private:
    // Caller holds m_mutex. At most four lines per runtime, diagnostic-only.
    // This is the host query result; a later guest output write can still fail.
    void log_first_query(u32 index, bool query_ok, bool connected, u32 result) {
        if (m_query_logged[index]) return;
        m_query_logged[index] = true;
        std::fprintf(stderr,
            "[xinput] first-query user=%u query_result=%u host_query=%u "
            "connected=%u enabled=%u\n",
            index, result, unsigned(query_ok), unsigned(connected), unsigned(m_enabled));
    }

    struct Slot {
        hardware::GamepadState state{};
        u32 packet{0};
        bool initialized{false};
    };

    std::shared_ptr<hardware::GamepadHost> m_host;
    std::mutex m_mutex;
    std::array<Slot, kXUserMaxCount> m_slots{};
    bool m_enabled{true};
    std::array<bool, kXUserMaxCount> m_query_logged{};
};

[[nodiscard]] u32 write_state(Marshaler& m, XInputRuntime& runtime,
                              u32 index, GuestAddr out) {
    if (out == 0) return error_codes::ERROR_BAD_ARGUMENTS;
    hardware::GamepadState state{};
    u32 packet = 0;
    const u32 result = runtime.get_state(index, state, packet);
    if (result != error_codes::ERROR_SUCCESS) return result;
    if (m.write_u32(out, packet).is_err() ||
        m.write_u16(out + 4, state.buttons).is_err() ||
        m.write_u8(out + 6, state.left_trigger).is_err() ||
        m.write_u8(out + 7, state.right_trigger).is_err() ||
        m.write_u16(out + 8, static_cast<u16>(state.left_x)).is_err() ||
        m.write_u16(out + 10, static_cast<u16>(state.left_y)).is_err() ||
        m.write_u16(out + 12, static_cast<u16>(state.right_x)).is_err() ||
        m.write_u16(out + 14, static_cast<u16>(state.right_y)).is_err()) {
        return error_codes::ERROR_BAD_ARGUMENTS;
    }
    return error_codes::ERROR_SUCCESS;
}

[[nodiscard]] u32 write_capabilities(Marshaler& m, XInputRuntime& runtime,
                                     u32 index, GuestAddr out) {
    if (out == 0) return error_codes::ERROR_BAD_ARGUMENTS;
    hardware::GamepadState state{};
    const u32 result = runtime.query_device(index, state);
    if (result != error_codes::ERROR_SUCCESS) return result;
    // XINPUT_CAPABILITIES: gamepad type/subtype, no optional feature flags,
    // full standard gamepad input resolution, and no advertised vibration.
    constexpr u16 kAllButtons = 0xF3FF;
    constexpr u8 kDeviceTypeGamepad = 0x01;
    constexpr u8 kDeviceSubtypeGamepad = 0x01;
    if (m.write_u8(out, kDeviceTypeGamepad).is_err() ||
        m.write_u8(out + 1, kDeviceSubtypeGamepad).is_err() ||
        m.write_u16(out + 2, 0).is_err() ||
        m.write_u16(out + 4, kAllButtons).is_err() ||
        m.write_u8(out + 6, 0xFF).is_err() ||
        m.write_u8(out + 7, 0xFF).is_err() ||
        m.write_u16(out + 8, 0xFFFF).is_err() ||
        m.write_u16(out + 10, 0xFFFF).is_err() ||
        m.write_u16(out + 12, 0xFFFF).is_err() ||
        m.write_u16(out + 14, 0xFFFF).is_err() ||
        m.write_u16(out + 16, 0).is_err() ||
        m.write_u16(out + 18, 0).is_err()) {
        return error_codes::ERROR_BAD_ARGUMENTS;
    }
    return error_codes::ERROR_SUCCESS;
}

// Native export tables, from the PE export directory of each real DLL:
//   xinput1_4.dll / xinput9_1_0.dll: winoracle System32 (x64) and SysWOW64
//   (x86); xinput1_3.dll: DirectX redist APR2007_xinput_{x86,x64}.cab.
// x86 and x64 tables are identical for each DLL. `named == false` is an
// ordinal-only export (absent from the export name table). Exports the real
// DLL has but this table omits (DllMain; 1_3 ordinals 101-103; 1_4 ordinals
// 101-104, 108, 109) have no implementation and stay unresolved.
struct NativeExport {
    std::string_view name;   ///< Implementation key in the shared entry list
    u16 ordinal;
    bool named;              ///< Present in the DLL's export name table
};

constexpr NativeExport kXInput14Exports[] = {
    {"XInputGetState", 2, true},
    {"XInputSetState", 3, true},
    {"XInputGetCapabilities", 4, true},
    {"XInputEnable", 5, true},
    {"XInputGetBatteryInformation", 7, true},
    {"XInputGetKeystroke", 8, true},
    {"XInputGetAudioDeviceIds", 10, true},
    {"XInputGetStateEx", 100, false},
};

constexpr NativeExport kXInput13Exports[] = {
    {"XInputGetState", 2, true},
    {"XInputSetState", 3, true},
    {"XInputGetCapabilities", 4, true},
    {"XInputEnable", 5, true},
    {"XInputGetDSoundAudioDeviceGuids", 6, true},
    {"XInputGetBatteryInformation", 7, true},
    {"XInputGetKeystroke", 8, true},
    {"XInputGetStateEx", 100, false},
};

constexpr NativeExport kXInput910Exports[] = {
    {"XInputGetCapabilities", 2, true},
    {"XInputGetDSoundAudioDeviceGuids", 3, true},
    {"XInputGetState", 4, true},
    {"XInputSetState", 5, true},
};

[[nodiscard]] std::span<const NativeExport> native_exports(XInputDll dll) {
    switch (dll) {
    case XInputDll::V1_4: return kXInput14Exports;
    case XInputDll::V1_3: return kXInput13Exports;
    case XInputDll::V9_1_0: return kXInput910Exports;
    }
    return {};
}

// Keep only the exports the native DLL has, under its ordinals and names.
void apply_native_exports(DllExports& exports, XInputDll dll) {
    const auto table = native_exports(dll);
    const auto find = [&](const ExportEntry& entry) {
        return std::find_if(table.begin(), table.end(),
            [&](const NativeExport& e) { return e.name == entry.name; });
    };
    std::erase_if(exports.entries, [&](const ExportEntry& entry) {
        return find(entry) == table.end();
    });
    for (auto& entry : exports.entries) {
        const auto it = find(entry);
        entry.ordinal = it->ordinal;
        if (!it->named) entry.name.clear();
    }
}

} // namespace

DllExports create_xinput_thunks(XInputDll dll,
                                std::shared_ptr<hardware::GamepadHost> host) {
    DllExports exports;
    if (!host) host = hardware::make_platform_gamepad_host();
    auto runtime = std::make_shared<XInputRuntime>(std::move(host));

    // DWORD XInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState)
    exports.entries.push_back(ExportEntry{
        "XInputGetState", 0,
        [runtime](Marshaler& m) {
            m.set_return(write_state(m, *runtime,
                static_cast<u32>(m.arg(0)), m.arg_ptr(1)));
        },
        8,
    });

    // DWORD XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration)
    exports.entries.push_back(ExportEntry{
        "XInputSetState", 0,
        [runtime](Marshaler& m) {
            hardware::GamepadState state{};
            const u32 result = runtime->query_device(
                static_cast<u32>(m.arg(0)), state);
            m.set_return(result == error_codes::ERROR_SUCCESS
                ? error_codes::ERROR_NOT_SUPPORTED : result);
        },
        8,
    });

    // DWORD XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags,
    //                             XINPUT_CAPABILITIES* pCapabilities)
    exports.entries.push_back(ExportEntry{
        "XInputGetCapabilities", 0,
        [runtime](Marshaler& m) {
            (void)m.arg(1);
            m.set_return(write_capabilities(m, *runtime,
                static_cast<u32>(m.arg(0)), m.arg_ptr(2)));
        },
        12,
    });

    // void XInputEnable(BOOL enable) — 1.3+; no return value.
    exports.entries.push_back(ExportEntry{
        "XInputEnable", 0,
        [runtime](Marshaler& m) {
            runtime->enable(m.arg(0) != 0);
        },
        4,
    });

    // DWORD XInputGetBatteryInformation(DWORD dwUserIndex, BYTE devType,
    //                                   XINPUT_BATTERY_INFORMATION* pBattery)
    exports.entries.push_back(ExportEntry{
        "XInputGetBatteryInformation", 0,
        [runtime](Marshaler& m) {
            (void)m.arg(1);
            hardware::GamepadState state{};
            const u32 result = runtime->query_device(
                static_cast<u32>(m.arg(0)), state);
            m.set_return(result == error_codes::ERROR_SUCCESS
                ? error_codes::ERROR_NOT_SUPPORTED : result);
        },
        12,
    });

    // DWORD XInputGetKeystroke(DWORD dwUserIndex, DWORD dwReserved,
    //                          PXINPUT_KEYSTROKE pKeystroke)
    exports.entries.push_back(ExportEntry{
        "XInputGetKeystroke", 0,
        [runtime](Marshaler& m) {
            (void)m.arg(1);
            hardware::GamepadState state{};
            const u32 result = runtime->query_device(
                static_cast<u32>(m.arg(0)), state);
            m.set_return(result == error_codes::ERROR_SUCCESS
                ? kErrorEmpty : result);
        },
        12,
    });

    // DWORD XInputGetAudioDeviceIds(DWORD dwUserIndex, LPWSTR pRenderDeviceId,
    //     UINT* pRenderCount, LPWSTR pCaptureDeviceId, UINT* pCaptureCount)
    exports.entries.push_back(ExportEntry{
        "XInputGetAudioDeviceIds", 0,
        [](Marshaler& m) {
            const u32 user_index = static_cast<u32>(m.arg(0));
            (void)m.arg_ptr(1);
            (void)m.arg_ptr(2);
            (void)m.arg_ptr(3);
            (void)m.arg_ptr(4);
            if (user_index >= kXUserMaxCount) {
                m.set_return(error_codes::ERROR_BAD_ARGUMENTS);
                return;
            }
            m.set_return(error_codes::ERROR_DEVICE_NOT_CONNECTED);
        },
        20,
    });

    // DWORD XInputGetDSoundAudioDeviceGuids(DWORD dwUserIndex, GUID* pDSoundRenderGuid,
    //                                       GUID* pDSoundCaptureGuid) — 1.3
    exports.entries.push_back(ExportEntry{
        "XInputGetDSoundAudioDeviceGuids", 0,
        [](Marshaler& m) {
            const u32 user_index = static_cast<u32>(m.arg(0));
            (void)m.arg_ptr(1);
            (void)m.arg_ptr(2);
            if (user_index >= kXUserMaxCount) {
                m.set_return(error_codes::ERROR_BAD_ARGUMENTS);
                return;
            }
            m.set_return(error_codes::ERROR_DEVICE_NOT_CONNECTED);
        },
        12,
    });

    // Ordinal 100 of xinput1_3/xinput1_4, nameless in their export tables:
    // XInputGetStateEx (undocumented extra byte for the guide button). Same
    // connectedness contract as XInputGetState. The name is only the table
    // key; apply_native_exports clears it.
    exports.entries.push_back(ExportEntry{
        "XInputGetStateEx", 0,
        [runtime](Marshaler& m) {
            m.set_return(write_state(m, *runtime,
                static_cast<u32>(m.arg(0)), m.arg_ptr(1)));
        },
        8,
    });

    apply_native_exports(exports, dll);
    return exports;
}

} // namespace vitreo::winapi
