#pragma once

/// @file dinput.hpp
/// @brief DINPUT.DLL (DirectInput 3-7) system keyboard and mouse.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <deque>
#include <functional>
#include <unordered_map>

namespace vitreo { class GuestMemory; }
namespace vitreo::windowing { class CocoaBridge; }

namespace vitreo::winapi {

class Marshaler;
class SyncManager;

namespace dinput {
constexpr u32 DI_OK               = 0;
constexpr u32 DI_NOTATTACHED      = 1;  // S_FALSE
constexpr u32 DI_BUFFEROVERFLOW   = 1;
constexpr u32 DI_PROPNOEFFECT     = 1;
constexpr u32 E_NOINTERFACE       = 0x80004002u;
constexpr u32 E_NOTIMPL           = 0x80004001u;
constexpr u32 E_INVALIDARG        = 0x80070057u;
constexpr u32 E_OUTOFMEMORY       = 0x8007000Eu;
constexpr u32 CLASS_E_NOAGGREGATION = 0x80040110u;
constexpr u32 DIERR_INVALIDPARAM  = 0x80070057u;
constexpr u32 DIERR_NOTACQUIRED   = 0x8007001Cu;
constexpr u32 DIERR_INPUTLOST     = 0x8007001Eu;
constexpr u32 DIERR_ACQUIRED      = 0x80070005u;
constexpr u32 DIERR_DEVICENOTREG  = 0x80040154u;

/// Cooperative-level flags.
constexpr u32 DISCL_EXCLUSIVE     = 0x00000001;
constexpr u32 DISCL_NONEXCLUSIVE  = 0x00000002;
constexpr u32 DISCL_FOREGROUND    = 0x00000004;
constexpr u32 DISCL_BACKGROUND    = 0x00000008;
} // namespace dinput

/// Implements the system keyboard and mouse behind DINPUT.DLL.
///
/// The interface set is what a DirectInput 3-7 era caller expects:
/// IDirectInputA plus IDirectInputDevice2A for GUID_SysKeyboard and
/// GUID_SysMouse. Device state is sourced from the same native key/cursor
/// tracking that user32's GetKeyState and GetCursorPos read, so the two APIs
/// never disagree about which keys are down.
///
/// Both access models are supported because callers pick one and rely on it:
/// immediate mode through GetDeviceState, and buffered mode through
/// GetDeviceData against a ring sized by DIPROP_BUFFERSIZE.
class DirectInputManager {
public:
    DirectInputManager() = default;

    void set_memory(GuestMemory* memory) noexcept { m_memory = memory; }
    void set_thunk_registry(ThunkRegistry* registry) noexcept {
        m_thunks = registry;
    }
    void set_cocoa_bridge(windowing::CocoaBridge* bridge) noexcept {
        m_bridge = bridge;
    }
    /// Needed to signal the event a caller registered with
    /// SetEventNotification when buffered input arrives.
    void set_sync_manager(SyncManager* sync) noexcept { m_sync = sync; }

    /// Guest callback runner for EnumDevices (same shape as ddraw's).
    using GuestCall = std::function<u64(GuestAddr, const u64*, usize, bool)>;
    void set_guest_call(GuestCall call) { m_guest_call = std::move(call); }

    /// Which physical device an object stands for.
    enum class DeviceKind : u8 { Keyboard, Mouse };

    // -- DLL entry points ---------------------------------------------------
    [[nodiscard]] u32 direct_input_create(GuestAddr instance, u32 version,
                                          GuestAddr output, GuestAddr outer);

    /// dinput8.dll!DirectInput8Create — same keyboard/mouse backend, DI8 IID
    /// and vtable (EnumDevicesBySemantics / ConfigureDevices appended).
    [[nodiscard]] u32 direct_input8_create(GuestAddr instance, u32 version,
                                           GuestAddr iid, GuestAddr output,
                                           GuestAddr outer, bool is_64bit);

    // -- IDirectInputA / IDirectInput8A ------------------------------------
    [[nodiscard]] u32 query_interface(GuestAddr object, GuestAddr iid,
                                      GuestAddr output);
    [[nodiscard]] u32 add_ref(GuestAddr object);
    [[nodiscard]] u32 release(GuestAddr object);
    [[nodiscard]] u32 create_device(GuestAddr object, GuestAddr guid,
                                    GuestAddr output, GuestAddr outer);
    [[nodiscard]] u32 enum_devices(GuestAddr object, u32 device_type,
                                   GuestAddr callback, GuestAddr ref,
                                   u32 flags, Marshaler& m);
    [[nodiscard]] u32 get_device_status(GuestAddr object, GuestAddr guid);

    // -- IDirectInputDevice2A ----------------------------------------------
    [[nodiscard]] u32 device_query_interface(GuestAddr device, GuestAddr iid,
                                             GuestAddr output);
    [[nodiscard]] u32 device_add_ref(GuestAddr device);
    [[nodiscard]] u32 device_release(GuestAddr device);
    [[nodiscard]] u32 device_get_capabilities(GuestAddr device, GuestAddr caps);
    [[nodiscard]] u32 device_set_data_format(GuestAddr device, GuestAddr format);
    [[nodiscard]] u32 device_set_cooperative_level(GuestAddr device, u32 hwnd,
                                                   u32 flags);
    [[nodiscard]] u32 device_set_property(GuestAddr device, GuestAddr guid,
                                          GuestAddr header);
    [[nodiscard]] u32 device_get_property(GuestAddr device, GuestAddr guid,
                                          GuestAddr header);
    [[nodiscard]] u32 device_set_event_notification(GuestAddr device,
                                                    u32 event_handle);
    [[nodiscard]] u32 device_acquire(GuestAddr device);
    [[nodiscard]] u32 device_unacquire(GuestAddr device);
    [[nodiscard]] u32 device_get_device_state(GuestAddr device, u32 size,
                                              GuestAddr data);
    [[nodiscard]] u32 device_get_device_data(GuestAddr device, u32 entry_size,
                                             GuestAddr data, GuestAddr count,
                                             u32 flags);
    [[nodiscard]] u32 device_poll(GuestAddr device);
    [[nodiscard]] u32 device_get_device_info(GuestAddr device, GuestAddr info);

    /// Record a key transition for every acquired keyboard device. Called from
    /// the windowing layer at the same point user32's key-state tracking is
    /// updated, so buffered DirectInput sees exactly the same transitions.
    void note_key_transition(u32 vk_code, bool down);

    /// Record a mouse button transition (0 = left, 1 = right, 2 = middle).
    void note_mouse_button(u32 button, bool down);

    /// Record absolute cursor motion; converted to the relative deltas
    /// DirectInput reports by default.
    /// Device motion for this event (DirectInput axes are relative device
    /// counts, like RAWMOUSE lLastX/lLastY -- not cursor-position deltas, which
    /// SetCursorPos warps and ClipCursor would corrupt).
    void note_mouse_motion(double device_dx, double device_dy);

    /// Vertical wheel movement in WHEEL_DELTA units (positive = away from the
    /// user), the same signed delta WM_MOUSEWHEEL and RAWMOUSE carry. It is
    /// the mouse's Z axis: DIMOUSESTATE.lZ and buffered DIMOFS_Z.
    void note_mouse_wheel(i32 delta);

private:
    /// One buffered DIDEVICEOBJECTDATA record.
    struct BufferedEvent {
        u32 offset;    ///< dwOfs — DIK code, or DIMOFS_* for the mouse
        u32 data;      ///< dwData
        u32 timestamp;
        u32 sequence;
    };

    struct Device {
        DeviceKind kind{DeviceKind::Keyboard};
        /// Created from an IDirectInput8 object (IDirectInputDevice8).
        bool di8{false};
        bool is_64bit{false};
        bool wide{false};
        u32  ref_count{1};
        u32  hwnd{0};
        u32  cooperative_flags{0};
        bool acquired{false};
        u32  buffer_size{0};       ///< DIPROP_BUFFERSIZE; 0 = immediate only
        u32  event_handle{0};      ///< SetEventNotification target
        u32  data_format_size{0};  ///< dwObjSize of the caller's data format
        bool overflowed{false};
        std::deque<BufferedEvent> buffer;
    };

    struct Object {
        u32 ref_count{1};
        /// True when created by DirectInput8Create (IDirectInput8A vtable).
        bool di8{false};
        bool is_64bit{false};
        bool wide{false};
    };

    [[nodiscard]] bool ensure_storage();
    [[nodiscard]] Object* find_object(GuestAddr object);
    [[nodiscard]] Device* find_device(GuestAddr device);

    /// Append to every acquired device of @p kind and signal its event.
    void push_event(DeviceKind kind, u32 offset, u32 data);

    [[nodiscard]] u32 fill_keyboard_state(GuestAddr data, u32 size);
    [[nodiscard]] u32 fill_mouse_state(GuestAddr data, u32 size);

    GuestMemory*             m_memory{nullptr};
    ThunkRegistry*           m_thunks{nullptr};
    windowing::CocoaBridge*  m_bridge{nullptr};
    SyncManager*             m_sync{nullptr};
    GuestCall                m_guest_call;

    GuestAddr m_region_base{0};
    GuestAddr m_vtable{0};       ///< IDirectInputA…7A
    GuestAddr m_vtable8{0};      ///< IDirectInput8A
    GuestAddr m_device_vtable{0};
    GuestAddr m_next_object{0};

    std::unordered_map<GuestAddr, Object> m_objects;
    std::unordered_map<GuestAddr, Device> m_devices;

    u32 m_sequence{0};
    /// Last cursor position, for turning absolute motion into DirectInput's
    /// relative axis reports.
    double m_mouse_frac_x{0.0};
    double m_mouse_frac_y{0.0};
    i32  m_last_cursor_x{0};
    i32  m_last_cursor_y{0};
    bool m_cursor_seeded{false};
    i32  m_pending_dx{0};
    i32  m_pending_dy{0};
    i32  m_pending_dz{0};
};

/// Build the legacy DirectInput export table.
///
/// Distinct from dinput8.dll: the pre-8 DLL is what SDL 1.2's DirectX video
/// driver probes for, and its absence alone pushes SDL onto the windib
/// backend, which in turn denies an app DirectDraw output.
DllExports create_dinput_thunks(DirectInputManager& manager);

} // namespace vitreo::winapi
