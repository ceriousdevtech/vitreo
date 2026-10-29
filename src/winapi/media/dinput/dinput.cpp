/// @file dinput.cpp
/// @brief DINPUT.DLL (DirectInput 3-7) system keyboard and mouse.

#include "winapi/media/dinput/dinput.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/core/marshaler.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vitreo::winapi {
namespace {

constexpr usize STORAGE_SIZE = 0x10000;

struct MethodSlot {
    const char* name;
    u16 stdcall_bytes;
};

/// IDirectInputA, extended through IDirectInput7A. A caller that asked for an
/// older version simply never reaches the later slots.
constexpr std::array<MethodSlot, 10> DINPUT_SLOTS = {{
    {"Vitreo_DInput_QueryInterface", 12},
    {"Vitreo_DInput_AddRef", 4},
    {"Vitreo_DInput_Release", 4},
    {"Vitreo_DInput_CreateDevice", 16},
    {"Vitreo_DInput_EnumDevices", 20},
    {"Vitreo_DInput_GetDeviceStatus", 8},
    {"Vitreo_DInput_RunControlPanel", 12},
    {"Vitreo_DInput_Initialize", 12},
    {"Vitreo_DInput_FindDevice", 16},
    {"Vitreo_DInput_CreateDeviceEx", 20},
}};

/// IDirectInput8A — same prefix as DI7 through FindDevice, then semantics /
/// configure instead of CreateDeviceEx.
constexpr std::array<MethodSlot, 11> DINPUT8_SLOTS = {{
    {"Vitreo_DInput_QueryInterface", 12},
    {"Vitreo_DInput_AddRef", 4},
    {"Vitreo_DInput_Release", 4},
    {"Vitreo_DInput_CreateDevice", 16},
    {"Vitreo_DInput_EnumDevices", 20},
    {"Vitreo_DInput_GetDeviceStatus", 8},
    {"Vitreo_DInput_RunControlPanel", 12},
    {"Vitreo_DInput_Initialize", 12},
    {"Vitreo_DInput_FindDevice", 16},
    {"Vitreo_DInput_EnumDevicesBySemantics", 24},
    {"Vitreo_DInput_ConfigureDevices", 20},
}};

/// IDirectInputDevice8A. Earlier generations never call the trailing slots;
/// QueryInterface can therefore hand back the same pointer for Device/2/7/8.
constexpr std::array<MethodSlot, 32> DEVICE_SLOTS = {{
    {"Vitreo_DIDev_QueryInterface", 12},
    {"Vitreo_DIDev_AddRef", 4},
    {"Vitreo_DIDev_Release", 4},
    {"Vitreo_DIDev_GetCapabilities", 8},
    {"Vitreo_DIDev_EnumObjects", 16},
    {"Vitreo_DIDev_GetProperty", 12},
    {"Vitreo_DIDev_SetProperty", 12},
    {"Vitreo_DIDev_Acquire", 4},
    {"Vitreo_DIDev_Unacquire", 4},
    {"Vitreo_DIDev_GetDeviceState", 12},
    {"Vitreo_DIDev_GetDeviceData", 20},
    {"Vitreo_DIDev_SetDataFormat", 8},
    {"Vitreo_DIDev_SetEventNotification", 8},
    {"Vitreo_DIDev_SetCooperativeLevel", 12},
    {"Vitreo_DIDev_GetObjectInfo", 16},
    {"Vitreo_DIDev_GetDeviceInfo", 8},
    {"Vitreo_DIDev_RunControlPanel", 12},
    {"Vitreo_DIDev_Initialize", 16},
    {"Vitreo_DIDev_CreateEffect", 20},
    {"Vitreo_DIDev_EnumEffects", 16},
    {"Vitreo_DIDev_GetEffectInfo", 12},
    {"Vitreo_DIDev_GetForceFeedbackState", 8},
    {"Vitreo_DIDev_SendForceFeedbackCommand", 8},
    {"Vitreo_DIDev_EnumCreatedEffectObjects", 16},
    {"Vitreo_DIDev_Escape", 8},
    {"Vitreo_DIDev_Poll", 4},
    {"Vitreo_DIDev_SendDeviceData", 20},
    {"Vitreo_DIDev_EnumEffectsInFile", 20},
    {"Vitreo_DIDev_WriteEffectToFile", 16},
    {"Vitreo_DIDev_BuildActionMap", 16},
    {"Vitreo_DIDev_SetActionMap", 16},
    {"Vitreo_DIDev_GetImageInfo", 8},
}};

struct Guid {
    u32 data1;
    u16 data2;
    u16 data3;
    u8  data4[8];
};

constexpr Guid GUID_SYS_KEYBOARD{
    0x6F1D2B61u, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
constexpr Guid GUID_SYS_MOUSE{
    0x6F1D2B60u, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};

constexpr Guid IID_IUNKNOWN{
    0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr Guid IID_IDIRECTINPUT_A{
    0x89521360u, 0xAA8A, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
constexpr Guid IID_IDIRECTINPUT2_A{
    0x5944E662u, 0xAA8A, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
constexpr Guid IID_IDIRECTINPUT7_A{
    0x9A4CB684u, 0x236D, 0x11D3, {0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE}};
constexpr Guid IID_IDIRECTINPUT8_A{
    0xBF798030u, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
constexpr Guid IID_IDIRECTINPUT8_W{
    0xBF798031u, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
constexpr Guid IID_IDIRECTINPUTDEVICE_A{
    0x5944E681u, 0xC92E, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
constexpr Guid IID_IDIRECTINPUTDEVICE2_A{
    0x5944E682u, 0xC92E, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
constexpr Guid IID_IDIRECTINPUTDEVICE7_A{
    0x57D7C6BCu, 0x2356, 0x11D3, {0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE}};
constexpr Guid IID_IDIRECTINPUTDEVICE8_A{
    0x54D41080u, 0xDC15, 0x4833, {0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79}};
constexpr Guid IID_IDIRECTINPUTDEVICE8_W{
    0x54D41081u, 0xDC15, 0x4833, {0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79}};

bool guid_equal(const Guid& a, const Guid& b) {
    return std::memcmp(&a, &b, sizeof(Guid)) == 0;
}

/// Read a GUID the guest passed by pointer.
bool read_guid(GuestMemory& memory, GuestAddr address, Guid& out) {
    if (address == 0) return false;
    return memory.read_bytes(address, reinterpret_cast<Byte*>(&out),
                             sizeof(Guid)).is_ok();
}

// DIPROP_* are MAKEDIPROP(n): a REFGUID whose pointer value is a small
// integer rather than an address. Anything below 0x10000 is one of these.
constexpr u32 DIPROP_BUFFERSIZE_ID = 1;
constexpr u32 DIPROP_AXISMODE_ID   = 2;
constexpr u32 DIPROP_GRANULARITY_ID = 3;
constexpr u32 DIPROP_RANGE_ID      = 4;

// DIMOFS_* — the dwOfs values a mouse reports in buffered mode. They are the
// byte offsets of the fields inside DIMOUSESTATE.
constexpr u32 DIMOFS_X = 0;
constexpr u32 DIMOFS_Y = 4;
constexpr u32 DIMOFS_Z = 8;
constexpr u32 DIMOFS_BUTTON0 = 12;

/// Windows virtual key -> DIK scan code (PS/2 set 1), which is what a
/// DirectInput keyboard reports both as its state-array index and as the
/// dwOfs of a buffered event.
struct KeyMapping { u32 vk; u8 dik; };
constexpr KeyMapping VK_TO_DIK[] = {
    {0x1B, 0x01}, // ESCAPE
    {0x31, 0x02}, {0x32, 0x03}, {0x33, 0x04}, {0x34, 0x05}, {0x35, 0x06},
    {0x36, 0x07}, {0x37, 0x08}, {0x38, 0x09}, {0x39, 0x0A}, {0x30, 0x0B},
    {0xBD, 0x0C}, // MINUS
    {0xBB, 0x0D}, // EQUALS
    {0x08, 0x0E}, // BACK
    {0x09, 0x0F}, // TAB
    {0x51, 0x10}, {0x57, 0x11}, {0x45, 0x12}, {0x52, 0x13}, {0x54, 0x14},
    {0x59, 0x15}, {0x55, 0x16}, {0x49, 0x17}, {0x4F, 0x18}, {0x50, 0x19},
    {0xDB, 0x1A}, // LBRACKET
    {0xDD, 0x1B}, // RBRACKET
    {0x0D, 0x1C}, // RETURN
    {0x11, 0x1D}, // LCONTROL
    {0x41, 0x1E}, {0x53, 0x1F}, {0x44, 0x20}, {0x46, 0x21}, {0x47, 0x22},
    {0x48, 0x23}, {0x4A, 0x24}, {0x4B, 0x25}, {0x4C, 0x26},
    {0xBA, 0x27}, // SEMICOLON
    {0xDE, 0x28}, // APOSTROPHE
    {0xC0, 0x29}, // GRAVE
    {0x10, 0x2A}, // LSHIFT
    {0xDC, 0x2B}, // BACKSLASH
    {0x5A, 0x2C}, {0x58, 0x2D}, {0x43, 0x2E}, {0x56, 0x2F}, {0x42, 0x30},
    {0x4E, 0x31}, {0x4D, 0x32},
    {0xBC, 0x33}, // COMMA
    {0xBE, 0x34}, // PERIOD
    {0xBF, 0x35}, // SLASH
    {0x6A, 0x37}, // MULTIPLY
    {0x12, 0x38}, // LMENU
    {0x20, 0x39}, // SPACE
    {0x14, 0x3A}, // CAPITAL
    {0x70, 0x3B}, {0x71, 0x3C}, {0x72, 0x3D}, {0x73, 0x3E}, {0x74, 0x3F},
    {0x75, 0x40}, {0x76, 0x41}, {0x77, 0x42}, {0x78, 0x43}, {0x79, 0x44},
    {0x90, 0x45}, // NUMLOCK
    {0x91, 0x46}, // SCROLL
    {0x67, 0x47}, {0x68, 0x48}, {0x69, 0x49},
    {0x6D, 0x4A}, // SUBTRACT
    {0x64, 0x4B}, {0x65, 0x4C}, {0x66, 0x4D},
    {0x6B, 0x4E}, // ADD
    {0x61, 0x4F}, {0x62, 0x50}, {0x63, 0x51}, {0x60, 0x52},
    {0x6E, 0x53}, // DECIMAL
    {0x7A, 0x57}, {0x7B, 0x58}, // F11, F12
    // Extended (E0-prefixed) keys carry 0x80 in their DIK code.
    {0x24, 0xC7}, // HOME
    {0x26, 0xC8}, // UP
    {0x21, 0xC9}, // PRIOR
    {0x25, 0xCB}, // LEFT
    {0x27, 0xCD}, // RIGHT
    {0x23, 0xCF}, // END
    {0x28, 0xD0}, // DOWN
    {0x22, 0xD1}, // NEXT
    {0x2D, 0xD2}, // INSERT
    {0x2E, 0xD3}, // DELETE
};

u8 dik_for_vk(u32 vk) {
    for (const auto& entry : VK_TO_DIK) {
        if (entry.vk == vk) return entry.dik;
    }
    return 0;
}

} // namespace

bool DirectInputManager::ensure_storage() {
    if (m_region_base != 0) return true;
    if (!m_memory || !m_thunks) return false;

    const GuestAddr base = m_memory->find_free_gap(0x62000000, STORAGE_SIZE);
    if (base == 0 ||
        m_memory->map_region(base, STORAGE_SIZE,
                             MemoryProtection::ReadWrite).is_err()) {
        return false;
    }

    m_region_base = base;
    m_vtable = base;
    m_vtable8 = base + 0x80;
    m_device_vtable = base + 0x200;
    m_next_object = base + 0x1000;

    for (u32 slot = 0; slot < DINPUT_SLOTS.size(); ++slot) {
        auto thunk = m_thunks->get_thunk_address("dinput.dll",
                                                 DINPUT_SLOTS[slot].name);
        if (thunk.is_err() ||
            m_memory->write_u32(m_vtable + slot * 4,
                                static_cast<u32>(thunk.value())).is_err()) {
            return false;
        }
    }
    for (u32 slot = 0; slot < DINPUT8_SLOTS.size(); ++slot) {
        auto thunk = m_thunks->get_thunk_address("dinput.dll",
                                                 DINPUT8_SLOTS[slot].name);
        if (thunk.is_err() ||
            m_memory->write_u32(m_vtable8 + slot * 4,
                                static_cast<u32>(thunk.value())).is_err()) {
            return false;
        }
    }
    // Win64 COM vtables contain 64-bit code pointers. Keep a parallel table;
    // using the legacy 4-byte layout truncates Vitreo's high thunk addresses.
    constexpr GuestAddr VTABLE8_64_OFFSET = 0x100;
    for (u32 slot = 0; slot < DINPUT8_SLOTS.size(); ++slot) {
        auto thunk = m_thunks->get_thunk_address("dinput.dll",
                                                 DINPUT8_SLOTS[slot].name);
        if (thunk.is_err() ||
            m_memory->write_u64(base + VTABLE8_64_OFFSET + slot * 8u,
                                thunk.value()).is_err()) {
            return false;
        }
    }
    for (u32 slot = 0; slot < DEVICE_SLOTS.size(); ++slot) {
        auto thunk = m_thunks->get_thunk_address("dinput.dll",
                                                 DEVICE_SLOTS[slot].name);
        if (thunk.is_err() ||
            m_memory->write_u32(m_device_vtable + slot * 4,
                                static_cast<u32>(thunk.value())).is_err()) {
            return false;
        }
    }
    constexpr GuestAddr DEVICE_VTABLE64_OFFSET = 0x300;
    for (u32 slot = 0; slot < DEVICE_SLOTS.size(); ++slot) {
        auto thunk = m_thunks->get_thunk_address("dinput.dll",
                                                 DEVICE_SLOTS[slot].name);
        if (thunk.is_err() ||
            m_memory->write_u64(base + DEVICE_VTABLE64_OFFSET + slot * 8u,
                                thunk.value()).is_err()) {
            return false;
        }
    }
    return true;
}

DirectInputManager::Object* DirectInputManager::find_object(GuestAddr object) {
    auto it = m_objects.find(object);
    return it == m_objects.end() ? nullptr : &it->second;
}

DirectInputManager::Device* DirectInputManager::find_device(GuestAddr device) {
    auto it = m_devices.find(device);
    return it == m_devices.end() ? nullptr : &it->second;
}

// =============================================================================
// DLL entry point
// =============================================================================

u32 DirectInputManager::direct_input_create(GuestAddr instance, u32 version,
                                            GuestAddr output, GuestAddr outer) {
    (void)instance;
    if (!m_memory || output == 0) return dinput::E_INVALIDARG;
    (void)m_memory->write_u32(output, 0);
    if (outer != 0) return dinput::CLASS_E_NOAGGREGATION;
    if (!ensure_storage()) return dinput::E_OUTOFMEMORY;

    const GuestAddr object = m_next_object;
    m_next_object += 0x20;
    if (m_memory->write_u32(object, static_cast<u32>(m_vtable)).is_err() ||
        m_memory->write_u32(output, static_cast<u32>(object)).is_err()) {
        return dinput::E_OUTOFMEMORY;
    }
    m_objects.emplace(object, Object{});
    VITREO_TRACE("[dinput] DirectInputCreate version=0x%x -> object=0x%llx\n",
                 version, static_cast<unsigned long long>(object));
    return dinput::DI_OK;
}

u32 DirectInputManager::direct_input8_create(GuestAddr instance, u32 version,
                                             GuestAddr iid, GuestAddr output,
                                             GuestAddr outer, bool is_64bit) {
    (void)instance;
    if (!m_memory || output == 0) return dinput::DIERR_INVALIDPARAM;
    if (is_64bit) (void)m_memory->write_u64(output, 0);
    else (void)m_memory->write_u32(output, 0);
    if (outer != 0) return dinput::CLASS_E_NOAGGREGATION;

    Guid requested{};
    if (!read_guid(*m_memory, iid, requested)) return dinput::DIERR_INVALIDPARAM;
    const bool wide = guid_equal(requested, IID_IDIRECTINPUT8_W);
    if (!wide && !guid_equal(requested, IID_IDIRECTINPUT8_A)) {
        VITREO_TRACE("[dinput8] DirectInput8Create: unsupported IID "
                     "%08x-%04x-%04x -> E_NOINTERFACE\n",
                     requested.data1, requested.data2, requested.data3);
        return dinput::E_NOINTERFACE;
    }
    if (!ensure_storage()) return dinput::E_OUTOFMEMORY;

    const GuestAddr object = m_next_object;
    m_next_object += 0x20;
    constexpr GuestAddr VTABLE8_64_OFFSET = 0x100;
    const GuestAddr vtable = is_64bit ? m_region_base + VTABLE8_64_OFFSET
                                     : m_vtable8;
    const bool write_failed = is_64bit
        ? (m_memory->write_u64(object, vtable).is_err() ||
           m_memory->write_u64(output, object).is_err())
        : (m_memory->write_u32(object, static_cast<u32>(vtable)).is_err() ||
           m_memory->write_u32(output, static_cast<u32>(object)).is_err());
    if (write_failed) {
        return dinput::E_OUTOFMEMORY;
    }
    Object state;
    state.di8 = true;
    state.is_64bit = is_64bit;
    state.wide = wide;
    m_objects.emplace(object, std::move(state));
    VITREO_TRACE("[dinput8] DirectInput8Create version=0x%x -> object=0x%llx\n",
                 version, static_cast<unsigned long long>(object));
    return dinput::DI_OK;
}

// =============================================================================
// IDirectInputA
// =============================================================================

u32 DirectInputManager::query_interface(GuestAddr object, GuestAddr iid,
                                        GuestAddr output) {
    if (!m_memory || output == 0) return dinput::E_INVALIDARG;
    Object* state = find_object(object);
    if (!state) return dinput::E_INVALIDARG;
    if (state->is_64bit) (void)m_memory->write_u64(output, 0);
    else (void)m_memory->write_u32(output, 0);

    Guid requested{};
    if (!read_guid(*m_memory, iid, requested)) return dinput::E_INVALIDARG;
    const bool ok =
        guid_equal(requested, IID_IUNKNOWN) ||
        (state->di8
             ? (state->wide ? guid_equal(requested, IID_IDIRECTINPUT8_W)
                            : guid_equal(requested, IID_IDIRECTINPUT8_A))
             : (guid_equal(requested, IID_IDIRECTINPUT_A) ||
                guid_equal(requested, IID_IDIRECTINPUT2_A) ||
                guid_equal(requested, IID_IDIRECTINPUT7_A)));
    if (!ok) return dinput::E_NOINTERFACE;
    const bool write_failed = state->is_64bit
        ? m_memory->write_u64(output, object).is_err()
        : m_memory->write_u32(output, static_cast<u32>(object)).is_err();
    if (write_failed) {
        return dinput::E_INVALIDARG;
    }
    ++state->ref_count;
    return dinput::DI_OK;
}

u32 DirectInputManager::add_ref(GuestAddr object) {
    Object* state = find_object(object);
    return state ? ++state->ref_count : 0;
}

u32 DirectInputManager::release(GuestAddr object) {
    Object* state = find_object(object);
    if (!state) return 0;
    if (--state->ref_count == 0) {
        m_objects.erase(object);
        return 0;
    }
    return state->ref_count;
}

u32 DirectInputManager::create_device(GuestAddr object, GuestAddr guid,
                                      GuestAddr output, GuestAddr outer) {
    if (!m_memory || output == 0) return dinput::E_INVALIDARG;
    if (outer != 0) return dinput::CLASS_E_NOAGGREGATION;
    const Object* owner = find_object(object);
    if (!owner) return dinput::E_INVALIDARG;
    if (owner->is_64bit) (void)m_memory->write_u64(output, 0);
    else (void)m_memory->write_u32(output, 0);

    Guid requested{};
    if (!read_guid(*m_memory, guid, requested)) return dinput::E_INVALIDARG;

    DeviceKind kind{};
    if (guid_equal(requested, GUID_SYS_KEYBOARD)) {
        kind = DeviceKind::Keyboard;
    } else if (guid_equal(requested, GUID_SYS_MOUSE)) {
        kind = DeviceKind::Mouse;
    } else {
        // Joysticks and other classes have no backend; a caller must be able
        // to tell "not present" from "present but broken".
        VITREO_TRACE("[dinput] CreateDevice: unsupported device GUID "
                     "%08x-%04x-%04x -> DIERR_DEVICENOTREG\n",
                     requested.data1, requested.data2, requested.data3);
        return dinput::DIERR_DEVICENOTREG;
    }

    const GuestAddr device = m_next_object;
    m_next_object += 0x20;
    constexpr GuestAddr DEVICE_VTABLE64_OFFSET = 0x300;
    const GuestAddr vtable = owner->is_64bit
        ? m_region_base + DEVICE_VTABLE64_OFFSET : m_device_vtable;
    const bool write_failed = owner->is_64bit
        ? (m_memory->write_u64(device, vtable).is_err() ||
           m_memory->write_u64(output, device).is_err())
        : (m_memory->write_u32(device, static_cast<u32>(vtable)).is_err() ||
           m_memory->write_u32(output, static_cast<u32>(device)).is_err());
    if (write_failed) {
        return dinput::E_OUTOFMEMORY;
    }
    Device state;
    state.kind = kind;
    state.di8 = owner->di8;
    state.is_64bit = owner->is_64bit;
    state.wide = owner->wide;
    m_devices.emplace(device, std::move(state));
    VITREO_TRACE("[dinput] CreateDevice %s -> device=0x%llx\n",
                 kind == DeviceKind::Keyboard ? "SysKeyboard" : "SysMouse",
                 static_cast<unsigned long long>(device));
    return dinput::DI_OK;
}

u32 DirectInputManager::enum_devices(GuestAddr object, u32 device_type,
                                     GuestAddr callback, GuestAddr ref,
                                     u32 flags, Marshaler& m) {
    (void)m;
    if (!find_object(object)) return dinput::E_INVALIDARG;
    if (callback == 0) return dinput::E_INVALIDARG;
    if (!m_memory || !m_guest_call) return dinput::E_NOTIMPL;
    // Force-feedback-only walks have nothing to report; Vitreo has no FF
    // devices. DIEDFL_ATTACHEDONLY and DIEDFL_ALLDEVICES both see the system
    // keyboard and mouse.
    constexpr u32 DIEDFL_FORCEFEEDBACK = 0x00000100u;
    if ((flags & DIEDFL_FORCEFEEDBACK) != 0) {
        VITREO_TRACE("[dinput] EnumDevices type=0x%x flags=0x%x -> no FF "
                     "devices\n", device_type, flags);
        return dinput::DI_OK;
    }

    // DIDEVICEINSTANCEA (DX8): dwSize + 2×GUID + dwDevType + 2×MAX_PATH names
    // + guidFFDriver + usage page/usage.
    constexpr u32 kInstanceSize = 580;
    constexpr u32 DIENUM_STOP = 0;
    constexpr u32 DI8DEVTYPE_MOUSE = 0x12;
    constexpr u32 DI8DEVTYPE_KEYBOARD = 0x13;
    constexpr u32 DI8DEVCLASS_POINTER = 2;
    constexpr u32 DI8DEVCLASS_KEYBOARD = 3;

    struct EnumDevice {
        Guid guid;
        u32  di8_type;   ///< low byte of dwDevType
        u32  subtype;    ///< high byte of dwDevType
        u16  usage_page;
        u16  usage;
        const char* instance_name;
        const char* product_name;
    };
    static constexpr EnumDevice kDevices[] = {
        {GUID_SYS_KEYBOARD, DI8DEVTYPE_KEYBOARD, 4 /*PCENH*/, 0x01, 0x06,
         "Keyboard", "Keyboard"},
        {GUID_SYS_MOUSE, DI8DEVTYPE_MOUSE, 2 /*TRADITIONAL*/, 0x01, 0x02,
         "Mouse", "Mouse"},
    };

    const auto type_matches = [&](const EnumDevice& device) -> bool {
        if (device_type == 0) return true;
        if (device_type == DI8DEVCLASS_KEYBOARD)
            return device.di8_type == DI8DEVTYPE_KEYBOARD;
        if (device_type == DI8DEVCLASS_POINTER)
            return device.di8_type == DI8DEVTYPE_MOUSE;
        // DI8DEVTYPE_* or MAKE_DIDEVICE_TYPE(type, subtype)
        const u32 want = device_type & 0xFFu;
        return want == device.di8_type;
    };

    const GuestAddr scratch =
        m_memory->find_free_gap(0x62800000, 0x1000);
    if (scratch == 0 ||
        m_memory->map_region(scratch, 0x1000,
                             MemoryProtection::ReadWrite).is_err()) {
        return dinput::E_OUTOFMEMORY;
    }

    u32 enumerated = 0;
    for (const auto& device : kDevices) {
        if (!type_matches(device)) continue;

        std::array<Byte, kInstanceSize> record{};
        auto* dw = reinterpret_cast<u32*>(record.data());
        dw[0] = kInstanceSize;
        std::memcpy(record.data() + 4, &device.guid, sizeof(Guid));
        std::memcpy(record.data() + 20, &device.guid, sizeof(Guid));
        dw[9] = device.di8_type | (device.subtype << 8);
        std::snprintf(reinterpret_cast<char*>(record.data() + 40), 260, "%s",
                      device.instance_name);
        std::snprintf(reinterpret_cast<char*>(record.data() + 300), 260, "%s",
                      device.product_name);
        // guidFFDriver left zero; HID usage identifies the device class.
        std::memcpy(record.data() + 576, &device.usage_page, sizeof(u16));
        std::memcpy(record.data() + 578, &device.usage, sizeof(u16));

        if (m_memory->write_bytes(scratch, record.data(), record.size())
                .is_err()) {
            (void)m_memory->unmap_region(scratch, 0x1000);
            return dinput::E_OUTOFMEMORY;
        }

        const u64 args[2] = {static_cast<u64>(scratch),
                             static_cast<u64>(ref)};
        const u64 result = m_guest_call(callback, args, 2, false);
        ++enumerated;
        if (static_cast<u32>(result) == DIENUM_STOP) {
            VITREO_TRACE("[dinput] EnumDevices type=0x%x flags=0x%x stopped "
                         "after %u device(s)\n",
                         device_type, flags, enumerated);
            (void)m_memory->unmap_region(scratch, 0x1000);
            return dinput::DI_OK;
        }
    }

    VITREO_TRACE("[dinput] EnumDevices type=0x%x flags=0x%x -> %u device(s)\n",
                 device_type, flags, enumerated);
    (void)m_memory->unmap_region(scratch, 0x1000);
    return dinput::DI_OK;
}

u32 DirectInputManager::get_device_status(GuestAddr object, GuestAddr guid) {
    if (!m_memory || !find_object(object)) return dinput::E_INVALIDARG;
    Guid requested{};
    if (!read_guid(*m_memory, guid, requested)) return dinput::E_INVALIDARG;
    if (guid_equal(requested, GUID_SYS_KEYBOARD) ||
        guid_equal(requested, GUID_SYS_MOUSE)) {
        return dinput::DI_OK;
    }
    return dinput::DI_NOTATTACHED;
}

// =============================================================================
// IDirectInputDevice2A
// =============================================================================

u32 DirectInputManager::device_query_interface(GuestAddr device, GuestAddr iid,
                                               GuestAddr output) {
    if (!m_memory || output == 0) return dinput::E_INVALIDARG;
    Device* state = find_device(device);
    if (!state) return dinput::E_INVALIDARG;
    if (state->is_64bit) (void)m_memory->write_u64(output, 0);
    else (void)m_memory->write_u32(output, 0);

    Guid requested{};
    if (!read_guid(*m_memory, iid, requested)) return dinput::E_INVALIDARG;
    // One vtable implements Device through Device8A: later generations only
    // append slots, and the DI8 action-map methods fail explicitly.
    if (!guid_equal(requested, IID_IUNKNOWN) &&
        !guid_equal(requested, IID_IDIRECTINPUTDEVICE_A) &&
        !guid_equal(requested, IID_IDIRECTINPUTDEVICE2_A) &&
        !guid_equal(requested, IID_IDIRECTINPUTDEVICE7_A) &&
        !guid_equal(requested, IID_IDIRECTINPUTDEVICE8_A) &&
        !guid_equal(requested, IID_IDIRECTINPUTDEVICE8_W)) {
        return dinput::E_NOINTERFACE;
    }
    const bool write_failed = state->is_64bit
        ? m_memory->write_u64(output, device).is_err()
        : m_memory->write_u32(output, static_cast<u32>(device)).is_err();
    if (write_failed) {
        return dinput::E_INVALIDARG;
    }
    ++state->ref_count;
    return dinput::DI_OK;
}

u32 DirectInputManager::device_add_ref(GuestAddr device) {
    Device* state = find_device(device);
    return state ? ++state->ref_count : 0;
}

u32 DirectInputManager::device_release(GuestAddr device) {
    Device* state = find_device(device);
    if (!state) return 0;
    if (--state->ref_count == 0) {
        m_devices.erase(device);
        return 0;
    }
    return state->ref_count;
}

u32 DirectInputManager::device_get_capabilities(GuestAddr device,
                                                GuestAddr caps) {
    Device* state = find_device(device);
    if (!state || caps == 0 || !m_memory) return dinput::E_INVALIDARG;

    // DIDEVCAPS: dwSize, dwFlags, dwDevType, dwAxes, dwButtons, dwPOVs, ...
    auto size = m_memory->read_u32(caps);
    if (size.is_err() || size.value() < 20) return dinput::E_INVALIDARG;

    constexpr u32 DIDC_ATTACHED = 0x00000001;
    constexpr u32 DIDEVTYPE_MOUSE = 2;
    constexpr u32 DIDEVTYPE_KEYBOARD = 3;
    const bool keyboard = state->kind == DeviceKind::Keyboard;

    (void)m_memory->write_u32(caps + 4, DIDC_ATTACHED);
    (void)m_memory->write_u32(caps + 8,
                              keyboard ? DIDEVTYPE_KEYBOARD : DIDEVTYPE_MOUSE);
    (void)m_memory->write_u32(caps + 12, keyboard ? 0u : 3u);   // axes
    (void)m_memory->write_u32(caps + 16, keyboard ? 256u : 3u); // buttons
    if (size.value() >= 24) (void)m_memory->write_u32(caps + 20, 0); // POVs
    return dinput::DI_OK;
}

u32 DirectInputManager::device_set_data_format(GuestAddr device,
                                               GuestAddr format) {
    Device* state = find_device(device);
    if (!state || format == 0 || !m_memory) return dinput::E_INVALIDARG;
    if (state->acquired) return dinput::DIERR_ACQUIRED;

    // DIDATAFORMAT: dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs, rgodf
    auto data_size = m_memory->read_u32(format + 12);
    if (data_size.is_err()) return dinput::E_INVALIDARG;
    state->data_format_size = data_size.value();
    VITREO_TRACE("[dinput] SetDataFormat device=0x%llx dwDataSize=%u\n",
                 static_cast<unsigned long long>(device),
                 state->data_format_size);
    return dinput::DI_OK;
}

u32 DirectInputManager::device_set_cooperative_level(GuestAddr device, u32 hwnd,
                                                     u32 flags) {
    Device* state = find_device(device);
    if (!state) return dinput::E_INVALIDARG;
    if (state->acquired) return dinput::DIERR_ACQUIRED;

    const bool exclusive = (flags & dinput::DISCL_EXCLUSIVE) != 0;
    const bool nonexclusive = (flags & dinput::DISCL_NONEXCLUSIVE) != 0;
    const bool foreground = (flags & dinput::DISCL_FOREGROUND) != 0;
    const bool background = (flags & dinput::DISCL_BACKGROUND) != 0;
    if (exclusive == nonexclusive || foreground == background) {
        return dinput::E_INVALIDARG;
    }
    // Windows rejects an exclusive background keyboard outright.
    if (state->kind == DeviceKind::Keyboard && exclusive && background) {
        return dinput::E_INVALIDARG;
    }

    state->hwnd = hwnd;
    state->cooperative_flags = flags;
    VITREO_TRACE("[dinput] SetCooperativeLevel device=0x%llx hwnd=0x%x "
                 "flags=0x%x\n",
                 static_cast<unsigned long long>(device), hwnd, flags);
    return dinput::DI_OK;
}

u32 DirectInputManager::device_set_property(GuestAddr device, GuestAddr guid,
                                            GuestAddr header) {
    Device* state = find_device(device);
    if (!state || header == 0 || !m_memory) return dinput::E_INVALIDARG;

    // DIPROPHEADER: dwSize, dwHeaderSize, dwObj, dwHow — a DIPROPDWORD's
    // dwData follows at +16.
    if (guid >= 0x10000) {
        VITREO_TRACE("[dinput] SetProperty: unsupported property GUID\n");
        return dinput::E_NOTIMPL;
    }
    switch (static_cast<u32>(guid)) {
    case DIPROP_BUFFERSIZE_ID: {
        auto value = m_memory->read_u32(header + 16);
        if (value.is_err()) return dinput::E_INVALIDARG;
        state->buffer_size = value.value();
        state->buffer.clear();
        state->overflowed = false;
        VITREO_TRACE("[dinput] SetProperty BUFFERSIZE device=0x%llx -> %u\n",
                     static_cast<unsigned long long>(device),
                     state->buffer_size);
        return dinput::DI_OK;
    }
    case DIPROP_AXISMODE_ID: {
        auto value = m_memory->read_u32(header + 16);
        if (value.is_err()) return dinput::E_INVALIDARG;
        // 0 = absolute, 1 = relative. Vitreo's mouse reports relative motion;
        // absolute mode would need a different axis pipeline entirely, so it
        // is refused rather than silently ignored.
        if (value.value() == 0) {
            VITREO_TRACE("[dinput] SetProperty AXISMODE absolute is not "
                         "implemented -> E_NOTIMPL\n");
            return dinput::E_NOTIMPL;
        }
        return dinput::DI_OK;
    }
    case DIPROP_GRANULARITY_ID:
    case DIPROP_RANGE_ID:
        // Read-only in Windows for the system devices.
        return dinput::E_NOTIMPL;
    default:
        VITREO_TRACE("[dinput] SetProperty: unhandled property id %u\n",
                     static_cast<u32>(guid));
        return dinput::E_NOTIMPL;
    }
}

u32 DirectInputManager::device_get_property(GuestAddr device, GuestAddr guid,
                                            GuestAddr header) {
    Device* state = find_device(device);
    if (!state || header == 0 || !m_memory) return dinput::E_INVALIDARG;
    if (guid >= 0x10000) return dinput::E_NOTIMPL;

    switch (static_cast<u32>(guid)) {
    case DIPROP_BUFFERSIZE_ID:
        return m_memory->write_u32(header + 16, state->buffer_size).is_ok()
                   ? dinput::DI_OK : dinput::E_INVALIDARG;
    case DIPROP_AXISMODE_ID:
        return m_memory->write_u32(header + 16, 1).is_ok()  // relative
                   ? dinput::DI_OK : dinput::E_INVALIDARG;
    case DIPROP_GRANULARITY_ID:
        return m_memory->write_u32(header + 16, 1).is_ok()
                   ? dinput::DI_OK : dinput::E_INVALIDARG;
    default:
        return dinput::E_NOTIMPL;
    }
}

u32 DirectInputManager::device_set_event_notification(GuestAddr device,
                                                      u32 event_handle) {
    Device* state = find_device(device);
    if (!state) return dinput::E_INVALIDARG;
    if (state->acquired) return dinput::DIERR_ACQUIRED;
    state->event_handle = event_handle;
    VITREO_TRACE("[dinput] SetEventNotification device=0x%llx event=0x%x\n",
                 static_cast<unsigned long long>(device), event_handle);
    return dinput::DI_OK;
}

u32 DirectInputManager::device_acquire(GuestAddr device) {
    Device* state = find_device(device);
    if (!state) return dinput::E_INVALIDARG;
    // Windows requires a data format before the device can be acquired.
    if (state->data_format_size == 0) return dinput::DIERR_INVALIDPARAM;
    if (state->acquired) return dinput::DI_NOTATTACHED;  // S_FALSE
    state->acquired = true;
    state->buffer.clear();
    state->overflowed = false;
    m_cursor_seeded = false;
    VITREO_TRACE("[dinput] Acquire device=0x%llx\n",
                 static_cast<unsigned long long>(device));
    return dinput::DI_OK;
}

u32 DirectInputManager::device_unacquire(GuestAddr device) {
    Device* state = find_device(device);
    if (!state) return dinput::E_INVALIDARG;
    if (!state->acquired) return dinput::DI_NOTATTACHED;  // S_FALSE
    state->acquired = false;
    state->buffer.clear();
    return dinput::DI_OK;
}

u32 DirectInputManager::fill_keyboard_state(GuestAddr data, u32 size) {
    // The keyboard's immediate state is a 256-byte array indexed by DIK code,
    // high bit set while the key is down.
    if (size < 256 || !m_memory) return dinput::E_INVALIDARG;
    std::array<u8, 256> keys{};
    if (m_bridge) {
        for (const auto& entry : VK_TO_DIK) {
            if (m_bridge->native_key_is_down(entry.vk)) {
                keys[entry.dik] = 0x80;
            }
        }
    }
    return m_memory->write_bytes(data, reinterpret_cast<const Byte*>(keys.data()),
                                 keys.size()).is_ok()
               ? dinput::DI_OK : dinput::E_INVALIDARG;
}

u32 DirectInputManager::fill_mouse_state(GuestAddr data, u32 size) {
    // DIMOUSESTATE: LONG lX, lY, lZ; BYTE rgbButtons[4].
    // DIMOUSESTATE2 extends rgbButtons to 8.
    if (size < 16 || !m_memory) return dinput::E_INVALIDARG;

    // Relative axes: report and clear the motion accumulated since the last
    // read, which is what a caller polling immediate state expects.
    const i32 dx = m_pending_dx;
    const i32 dy = m_pending_dy;
    const i32 dz = m_pending_dz;
    m_pending_dx = 0;
    m_pending_dy = 0;
    m_pending_dz = 0;

    (void)m_memory->write_u32(data + 0, static_cast<u32>(dx));
    (void)m_memory->write_u32(data + 4, static_cast<u32>(dy));
    (void)m_memory->write_u32(data + 8, static_cast<u32>(dz));

    const u32 button_count = size >= 20 ? 8u : 4u;
    for (u32 i = 0; i < button_count; ++i) {
        u8 down = 0;
        if (m_bridge && i < 3) {
            // VK_LBUTTON / VK_RBUTTON / VK_MBUTTON
            constexpr std::array<u32, 3> kButtonVk = {0x01, 0x02, 0x04};
            down = m_bridge->native_key_is_down(kButtonVk[i]) ? 0x80 : 0;
        }
        (void)m_memory->write_u8(data + 12 + i, down);
    }
    return dinput::DI_OK;
}

u32 DirectInputManager::device_get_device_state(GuestAddr device, u32 size,
                                                GuestAddr data) {
    Device* state = find_device(device);
    if (!state || data == 0) return dinput::E_INVALIDARG;
    if (!state->acquired) return dinput::DIERR_NOTACQUIRED;
    return state->kind == DeviceKind::Keyboard ? fill_keyboard_state(data, size)
                                               : fill_mouse_state(data, size);
}

u32 DirectInputManager::device_get_device_data(GuestAddr device, u32 entry_size,
                                               GuestAddr data, GuestAddr count,
                                               u32 flags) {
    constexpr u32 DIGDD_PEEK = 0x00000001;

    Device* state = find_device(device);
    if (!state || count == 0 || !m_memory) return dinput::E_INVALIDARG;
    if (!state->acquired) return dinput::DIERR_NOTACQUIRED;
    if (state->buffer_size == 0) return dinput::DIERR_NOTACQUIRED;

    auto requested = m_memory->read_u32(count);
    if (requested.is_err()) return dinput::E_INVALIDARG;

    // dinput.h: DIDEVICEOBJECTDATA_DX3 is dwOfs, dwData, dwTimeStamp,
    // dwSequence (16 bytes); DirectInput 8 appends UINT_PTR uAppData, so
    // sizeof(DIDEVICEOBJECTDATA) is 20 on x86 and 24 on x64. Each interface
    // takes exactly its own size (tools/oracle_probe/dinput_device_data_probe):
    // an IDirectInputDevice8 rejects any other with DIERR_INVALIDPARAM and
    // leaves *pdwInOut alone; an older device returns DI_OK having written
    // nothing.
    const u32 record_size = state->di8 ? (state->is_64bit ? 24u : 20u) : 16u;
    if (entry_size != record_size) {
        return state->di8 ? dinput::E_INVALIDARG : dinput::DI_OK;
    }

    // data == NULL is the documented "discard/count" form.
    const u32 available = static_cast<u32>(state->buffer.size());
    u32 to_report = requested.value() == 0xFFFFFFFFu
                        ? available
                        : (requested.value() < available ? requested.value()
                                                         : available);
    if (data != 0) {
        for (u32 i = 0; i < to_report; ++i) {
            const BufferedEvent& event = state->buffer[i];
            const GuestAddr slot = data + i * entry_size;
            (void)m_memory->write_u32(slot + 0, event.offset);
            (void)m_memory->write_u32(slot + 4, event.data);
            (void)m_memory->write_u32(slot + 8, event.timestamp);
            (void)m_memory->write_u32(slot + 12, event.sequence);
            // uAppData: the DIACTION value from SetActionMap; with no action
            // map Learn says to ignore it.
            if (entry_size == 24) (void)m_memory->write_u64(slot + 16, 0);
            if (entry_size == 20) (void)m_memory->write_u32(slot + 16, 0);
        }
    }
    if ((flags & DIGDD_PEEK) == 0) {
        state->buffer.erase(state->buffer.begin(),
                            state->buffer.begin() + to_report);
    }
    if (m_memory->write_u32(count, to_report).is_err()) {
        return dinput::E_INVALIDARG;
    }

    // Report (once) that events were dropped because the ring was full.
    if (state->overflowed) {
        state->overflowed = false;
        return dinput::DI_BUFFEROVERFLOW;
    }
    return dinput::DI_OK;
}

u32 DirectInputManager::device_poll(GuestAddr device) {
    Device* state = find_device(device);
    if (!state) return dinput::E_INVALIDARG;
    if (!state->acquired) return dinput::DIERR_NOTACQUIRED;
    // Keyboard and mouse are event-driven, so polling has nothing to fetch.
    return dinput::DI_OK;
}

u32 DirectInputManager::device_get_device_info(GuestAddr device,
                                               GuestAddr info) {
    Device* state = find_device(device);
    if (!state || info == 0 || !m_memory) return dinput::E_INVALIDARG;

    // DIDEVICEINSTANCEA: dwSize, guidInstance(16), guidProduct(16), dwDevType,
    // tszInstanceName[260], tszProductName[260], ...
    auto size = m_memory->read_u32(info);
    if (size.is_err() || size.value() < 4 + 16 + 16 + 4) {
        return dinput::E_INVALIDARG;
    }
    const bool keyboard = state->kind == DeviceKind::Keyboard;
    const Guid& guid = keyboard ? GUID_SYS_KEYBOARD : GUID_SYS_MOUSE;
    (void)m_memory->write_bytes(info + 4,
                                reinterpret_cast<const Byte*>(&guid),
                                sizeof(Guid));
    (void)m_memory->write_bytes(info + 20,
                                reinterpret_cast<const Byte*>(&guid),
                                sizeof(Guid));
    constexpr u32 DIDEVTYPE_MOUSE = 2;
    constexpr u32 DIDEVTYPE_KEYBOARD = 3;
    (void)m_memory->write_u32(info + 36,
                              keyboard ? DIDEVTYPE_KEYBOARD : DIDEVTYPE_MOUSE);

    const char* name = keyboard ? "Keyboard" : "Mouse";
    const usize name_len = std::strlen(name) + 1;
    if (size.value() >= 40 + name_len) {
        (void)m_memory->write_bytes(info + 40,
                                    reinterpret_cast<const Byte*>(name),
                                    name_len);
    }
    return dinput::DI_OK;
}

// =============================================================================
// Input feed
// =============================================================================

void DirectInputManager::push_event(DeviceKind kind, u32 offset, u32 data) {
    const u32 timestamp = m_bridge ? m_bridge->last_input_tick_count() : 0;
    const u32 sequence = ++m_sequence;

    for (auto& [address, device] : m_devices) {
        (void)address;
        if (device.kind != kind || !device.acquired ||
            device.buffer_size == 0) {
            continue;
        }
        if (device.buffer.size() >= device.buffer_size) {
            // Windows drops the newest event and reports DI_BUFFEROVERFLOW on
            // the next GetDeviceData.
            device.overflowed = true;
            continue;
        }
        device.buffer.push_back(BufferedEvent{offset, data, timestamp,
                                              sequence});
        if (device.event_handle != 0 && m_sync) {
            (void)m_sync->set_event(static_cast<Handle>(device.event_handle));
        }
    }
}

void DirectInputManager::note_key_transition(u32 vk_code, bool down) {
    const u8 dik = dik_for_vk(vk_code);
    if (dik == 0) return;
    push_event(DeviceKind::Keyboard, dik, down ? 0x80u : 0u);
}

void DirectInputManager::note_mouse_button(u32 button, bool down) {
    if (button > 3) return;
    push_event(DeviceKind::Mouse, DIMOFS_BUTTON0 + button, down ? 0x80u : 0u);
}

void DirectInputManager::note_mouse_motion(double device_dx, double device_dy) {
    m_mouse_frac_x += device_dx;
    m_mouse_frac_y += device_dy;
    const i32 dx = static_cast<i32>(std::trunc(m_mouse_frac_x));
    const i32 dy = static_cast<i32>(std::trunc(m_mouse_frac_y));
    m_mouse_frac_x -= dx;
    m_mouse_frac_y -= dy;
    if (dx == 0 && dy == 0) return;

    m_pending_dx += dx;
    m_pending_dy += dy;
    if (dx != 0) push_event(DeviceKind::Mouse, DIMOFS_X, static_cast<u32>(dx));
    if (dy != 0) push_event(DeviceKind::Mouse, DIMOFS_Y, static_cast<u32>(dy));
}

void DirectInputManager::note_mouse_wheel(i32 delta) {
    if (delta == 0) return;
    m_pending_dz += delta;
    push_event(DeviceKind::Mouse, DIMOFS_Z, static_cast<u32>(delta));
}

// =============================================================================
// Export table
// =============================================================================

DllExports create_dinput_thunks(DirectInputManager& manager) {
    DllExports exports;

    // HRESULT DirectInputCreate{A,W}(HINSTANCE, DWORD, LPDIRECTINPUT*, LPUNKNOWN)
    const auto create = [&manager](Marshaler& m) {
        m.set_return(manager.direct_input_create(
            m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2),
            m.arg_ptr(3)));
    };
    exports.entries.push_back({"DirectInputCreateA", 0, create, 16});
    exports.entries.push_back({"DirectInputCreateW", 0, create, 16});

    // HRESULT DirectInputCreateEx(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN)
    exports.entries.push_back({"DirectInputCreateEx", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.direct_input_create(
                m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(3),
                m.arg_ptr(4)));
        }, 20});

    exports.entries.push_back({"DllCanUnloadNow", 0,
        [](Marshaler& m) { m.set_return(1u); /* S_FALSE: stay loaded */ }, 0});
    exports.entries.push_back({"DllGetClassObject", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg_ptr(1);
            const GuestAddr output = m.arg_ptr(2);
            if (output != 0) (void)m.write_ptr(output, 0);
            m.set_return(0x80040111u);  // CLASS_E_CLASSNOTAVAILABLE
        }, 12});

    // ---- IDirectInputA ----------------------------------------------------
    exports.entries.push_back({"Vitreo_DInput_QueryInterface", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.query_interface(m.arg_ptr(0), m.arg_ptr(1),
                                                 m.arg_ptr(2)));
        }, 12});
    exports.entries.push_back({"Vitreo_DInput_AddRef", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.add_ref(m.arg_ptr(0)));
        }, 4});
    exports.entries.push_back({"Vitreo_DInput_Release", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.release(m.arg_ptr(0)));
        }, 4});
    exports.entries.push_back({"Vitreo_DInput_CreateDevice", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.create_device(m.arg_ptr(0), m.arg_ptr(1),
                                               m.arg_ptr(2), m.arg_ptr(3)));
        }, 16});
    exports.entries.push_back({"Vitreo_DInput_CreateDeviceEx", 0,
        [&manager](Marshaler& m) {
            // (REFGUID, REFIID, LPVOID*, LPUNKNOWN) — the IID selects the
            // device interface generation, all of which share one vtable.
            m.set_return(manager.create_device(m.arg_ptr(0), m.arg_ptr(1),
                                               m.arg_ptr(3), m.arg_ptr(4)));
        }, 20});
    exports.entries.push_back({"Vitreo_DInput_EnumDevices", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.enum_devices(
                m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2),
                m.arg_ptr(3), static_cast<u32>(m.arg(4)), m));
        }, 20});
    exports.entries.push_back({"Vitreo_DInput_GetDeviceStatus", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.get_device_status(m.arg_ptr(0), m.arg_ptr(1)));
        }, 8});
    exports.entries.push_back({"Vitreo_DInput_RunControlPanel", 0,
        [](Marshaler& m) {
            (void)m.arg(0); (void)m.arg(1);
            m.set_return(dinput::E_NOTIMPL);
        }, 12});
    exports.entries.push_back({"Vitreo_DInput_Initialize", 0,
        [](Marshaler& m) {
            // The object is already initialized by DirectInputCreate.
            (void)m.arg(0); (void)m.arg(1);
            m.set_return(dinput::DI_OK);
        }, 12});
    exports.entries.push_back({"Vitreo_DInput_FindDevice", 0,
        [](Marshaler& m) {
            (void)m.arg(0); (void)m.arg(1); (void)m.arg(2);
            m.set_return(dinput::E_NOTIMPL);
        }, 16});
    // IDirectInput8A-only slots. No action-mapper backend yet; succeed with
    // zero enumerations so CreateDevice(GUID_Sys*) remains the supported path.
    exports.entries.push_back({"Vitreo_DInput_EnumDevicesBySemantics", 0,
        [](Marshaler& m) {
            (void)m.arg(0); (void)m.arg(1); (void)m.arg(2);
            (void)m.arg(3); (void)m.arg(4); (void)m.arg(5);
            VITREO_TRACE("[dinput8] EnumDevicesBySemantics -> DI_OK "
                         "(no semantic devices)\n");
            m.set_return(dinput::DI_OK);
        }, 24});
    exports.entries.push_back({"Vitreo_DInput_ConfigureDevices", 0,
        [](Marshaler& m) {
            (void)m.arg(0); (void)m.arg(1); (void)m.arg(2);
            (void)m.arg(3); (void)m.arg(4);
            VITREO_TRACE("[dinput8] ConfigureDevices -> E_NOTIMPL\n");
            m.set_return(dinput::E_NOTIMPL);
        }, 20});

    // ---- IDirectInputDevice2A / 8A ----------------------------------------
    exports.entries.push_back({"Vitreo_DIDev_QueryInterface", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_query_interface(
                m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2)));
        }, 12});
    exports.entries.push_back({"Vitreo_DIDev_AddRef", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_add_ref(m.arg_ptr(0)));
        }, 4});
    exports.entries.push_back({"Vitreo_DIDev_Release", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_release(m.arg_ptr(0)));
        }, 4});
    exports.entries.push_back({"Vitreo_DIDev_GetCapabilities", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_get_capabilities(m.arg_ptr(0),
                                                         m.arg_ptr(1)));
        }, 8});
    exports.entries.push_back({"Vitreo_DIDev_GetProperty", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_get_property(m.arg_ptr(0), m.arg_ptr(1),
                                                     m.arg_ptr(2)));
        }, 12});
    exports.entries.push_back({"Vitreo_DIDev_SetProperty", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_set_property(m.arg_ptr(0), m.arg_ptr(1),
                                                     m.arg_ptr(2)));
        }, 12});
    exports.entries.push_back({"Vitreo_DIDev_Acquire", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_acquire(m.arg_ptr(0)));
        }, 4});
    exports.entries.push_back({"Vitreo_DIDev_Unacquire", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_unacquire(m.arg_ptr(0)));
        }, 4});
    exports.entries.push_back({"Vitreo_DIDev_GetDeviceState", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_get_device_state(
                m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2)));
        }, 12});
    exports.entries.push_back({"Vitreo_DIDev_GetDeviceData", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_get_device_data(
                m.arg_ptr(0), static_cast<u32>(m.arg(1)), m.arg_ptr(2),
                m.arg_ptr(3), static_cast<u32>(m.arg(4))));
        }, 20});
    exports.entries.push_back({"Vitreo_DIDev_SetDataFormat", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_set_data_format(m.arg_ptr(0),
                                                        m.arg_ptr(1)));
        }, 8});
    exports.entries.push_back({"Vitreo_DIDev_SetEventNotification", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_set_event_notification(
                m.arg_ptr(0), static_cast<u32>(m.arg(1))));
        }, 8});
    exports.entries.push_back({"Vitreo_DIDev_SetCooperativeLevel", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_set_cooperative_level(
                m.arg_ptr(0), static_cast<u32>(m.arg(1)),
                static_cast<u32>(m.arg(2))));
        }, 12});
    exports.entries.push_back({"Vitreo_DIDev_GetDeviceInfo", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_get_device_info(m.arg_ptr(0),
                                                        m.arg_ptr(1)));
        }, 8});
    exports.entries.push_back({"Vitreo_DIDev_Poll", 0,
        [&manager](Marshaler& m) {
            m.set_return(manager.device_poll(m.arg_ptr(0)));
        }, 4});
    exports.entries.push_back({"Vitreo_DIDev_Initialize", 0,
        [](Marshaler& m) {
            (void)m.arg(0); (void)m.arg(1); (void)m.arg(2);
            m.set_return(dinput::DI_OK);
        }, 16});

    // Remaining slots have no backend (object enumeration, force feedback,
    // driver escapes, DI8 action maps). They fail explicitly so a caller can
    // tell the feature is absent instead of acting on an empty success.
    static constexpr std::array<const char*, 13> kUnimplemented = {
        "Vitreo_DIDev_EnumObjects", "Vitreo_DIDev_GetObjectInfo",
        "Vitreo_DIDev_RunControlPanel", "Vitreo_DIDev_CreateEffect",
        "Vitreo_DIDev_EnumEffects", "Vitreo_DIDev_GetEffectInfo",
        "Vitreo_DIDev_EnumCreatedEffectObjects", "Vitreo_DIDev_SendDeviceData",
        "Vitreo_DIDev_EnumEffectsInFile", "Vitreo_DIDev_WriteEffectToFile",
        "Vitreo_DIDev_BuildActionMap", "Vitreo_DIDev_SetActionMap",
        "Vitreo_DIDev_GetImageInfo",
    };
    for (const char* name : kUnimplemented) {
        u16 cleanup = 16;
        for (const auto& slot : DEVICE_SLOTS) {
            if (std::strcmp(slot.name, name) == 0) {
                cleanup = slot.stdcall_bytes;
                break;
            }
        }
        exports.entries.push_back({name, 0,
            [name](Marshaler& m) {
                VITREO_TRACE("[dinput] unsupported method=%s\n", name);
                m.set_return(dinput::E_NOTIMPL);
            }, cleanup});
    }
    // Force-feedback state queries return "no effects" rather than an error,
    // which is what a device without actuators reports.
    exports.entries.push_back({"Vitreo_DIDev_GetForceFeedbackState", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            const GuestAddr out = m.arg_ptr(1);
            if (out != 0) (void)m.write_u32(out, 0);
            m.set_return(dinput::E_NOTIMPL);
        }, 8});
    exports.entries.push_back({"Vitreo_DIDev_SendForceFeedbackCommand", 0,
        [](Marshaler& m) {
            (void)m.arg(0); (void)m.arg(1);
            m.set_return(dinput::E_NOTIMPL);
        }, 8});
    exports.entries.push_back({"Vitreo_DIDev_Escape", 0,
        [](Marshaler& m) {
            (void)m.arg(0); (void)m.arg(1);
            m.set_return(dinput::E_NOTIMPL);
        }, 8});

    return exports;
}

} // namespace vitreo::winapi
