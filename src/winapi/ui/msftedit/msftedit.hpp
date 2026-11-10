#pragma once
/// @file msftedit.hpp
/// @brief Msftedit.dll -- the Rich Edit 4.1+ module (Learn: About Rich Edit
/// Controls). Loading it registers the RICHEDIT50W window class, whose
/// windows Vitreo presents with its native Rich Edit control; the last
/// FreeLibrary unregisters it again.
///
/// Contract sources: Learn CreateTextServices / ShutdownTextServices /
/// DllGetVersion; tests/oracle_data/msftedit_load_probe{32,64}.expected and
/// msftedit_lifecycle_probe{32,64}.expected (class registration, exports,
/// ordinals, interface identifiers, version, reference counting); x86
/// argument bytes measured from each export's return instruction
/// (.docs/rdr2-bringup.md, "Builtin Msftedit").

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <array>
#include <functional>
#include <span>

namespace vitreo::winapi {

class Marshaler;

/// What Msftedit.dll needs from the rest of the runtime.
struct MsfteditServices {
    /// The native Rich Edit control's window procedure body: a wndproc's
    /// (hwnd, msg, wParam, lParam), @p ansi selecting ANSI message text.
    std::function<void(Marshaler&, bool ansi)> control_proc;
    /// DLL_PROCESS_ATTACH: register RICHEDIT50W for the module at
    /// @p module_base with @p wndproc (the RichEditWndProc export).
    std::function<void(GuestAddr module_base, GuestAddr wndproc)> register_class;
    /// DLL_PROCESS_DETACH: unregister what register_class registered.
    std::function<void(GuestAddr module_base)> unregister_class;
    /// The guest address of a callable export of this DLL (GetProcAddress's
    /// value), 0 when unknown.
    std::function<GuestAddr(const char* name)> export_address;
};

/// One interface identifier Msftedit.dll exports as data.
struct MsfteditInterfaceId {
    const char* name;
    u16 ordinal;
    std::array<u8, 16> guid;  ///< GUID in memory order
};

/// The ten IID_* data exports (msftedit_lifecycle_probe "iid" lines).
[[nodiscard]] std::span<const MsfteditInterfaceId> msftedit_interface_ids();

/// RICHEDIT50W's registration (msftedit_lifecycle_probe "loaded" line).
inline constexpr u32 kMsfteditClassStyle = 0x4088;     // GLOBALCLASS|PARENTDC|DBLCLKS
inline constexpr u64 kMsfteditClassBrush = 6;          // COLOR_WINDOW + 1

/// The callable exports, names and ordinals per bitness.
[[nodiscard]] DllExports create_msftedit_thunks(bool guest_is_64bit,
                                                MsfteditServices services);

} // namespace vitreo::winapi
