#pragma once

/// @file window_class.hpp
/// @brief RegisterClass*/UnregisterClass*/GetClassInfo* and the per-process
/// class tables (WIN32-050).
///
/// Microsoft Learn, "About Window Classes": a process has three class lists —
/// application local classes (owned by the registering module's hInstance;
/// several modules may register the same name), application global classes
/// (CS_GLOBALCLASS, visible to every module) and the system classes the
/// system registers for the process. CreateWindowEx searches local classes of
/// the caller's hInstance, then global classes, then system classes, and a
/// local class may shadow a system class. The per-instance and ordering rules
/// Learn leaves open are measured in tools/oracle_probe/s1win_class_probe.c.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi {

class Marshaler;

using ATOM = u16;

// =============================================================================
// Window Class Styles (CS_*)
// =============================================================================

namespace CS {
    constexpr u32 VREDRAW         = 0x0001;
    constexpr u32 HREDRAW         = 0x0002;
    constexpr u32 DBLCLKS         = 0x0008;
    constexpr u32 OWNDC           = 0x0020;
    constexpr u32 CLASSDC         = 0x0040;
    constexpr u32 PARENTDC        = 0x0080;
    constexpr u32 NOCLOSE         = 0x0200;
    constexpr u32 SAVEBITS        = 0x0800;
    constexpr u32 BYTEALIGNCLIENT = 0x1000;
    constexpr u32 BYTEALIGNWINDOW = 0x2000;
    constexpr u32 GLOBALCLASS     = 0x4000;
} // namespace CS

// =============================================================================
// Error Codes (winerror.h)
// =============================================================================

constexpr u32 ERROR_CLASS_ALREADY_EXISTS = 1410;

// =============================================================================
// Window Class Data
// =============================================================================

/// A registered window class: the WNDCLASSEX fields plus the state USER keeps
/// for it (charset, class extra memory, owning list).
struct WindowClassData {
    ATOM atom = 0;
    /// Registration serial, unique per registration. Windows that belong to
    /// the class record it: two modules may register the same name (and so
    /// share one atom), and the window must keep resolving to its own class.
    u32 id = 0;
    std::wstring class_name;
    u32 style = 0;
    u64 wndproc = 0;          ///< Guest WndProc pointer
    int cls_extra = 0;
    int wnd_extra = 0;
    u64 hinstance = 0;
    u64 hicon = 0;
    u64 hcursor = 0;
    u64 hbr_background = 0;
    /// lpszMenuName as text (MAKEINTRESOURCE ids rendered in decimal) — the
    /// form the class-menu loader consumes.
    std::wstring menu_name;
    /// lpszMenuName exactly as registered (a guest pointer or a
    /// MAKEINTRESOURCE value) — what GetClassInfo and GCLP_MENUNAME report.
    u64 menu_name_ptr = 0;
    u64 hicon_sm = 0;
    /// RegisterClass*W registers a Unicode class, RegisterClass*A an ANSI one
    /// (Learn: About Window Classes, "Registering a Window Class").
    bool unicode = true;
    /// Registered by the system for the process (Button, Edit, #32770, ...).
    bool system = false;
    /// A system class whose class cursor is the system arrow; its handle
    /// comes from the resource manager (WindowClassManager::class_cursor).
    bool arrow_cursor = false;
    /// cbClsExtra bytes; Learn: "The system initializes this memory to zero."
    std::vector<u8> extra;

    [[nodiscard]] bool global() const noexcept {
        return (style & CS::GLOBALCLASS) != 0;
    }
};

/// Why a class lookup failed or an update was refused; mapped to the
/// Win32 error by the thunks.
enum class ClassError : u32 {
    None = 0,
    AlreadyExists = 1410,     ///< ERROR_CLASS_ALREADY_EXISTS
    DoesNotExist = 1411,      ///< ERROR_CLASS_DOES_NOT_EXIST
    HasWindows = 1412,        ///< ERROR_CLASS_HAS_WINDOWS
    InvalidParameter = 87,    ///< ERROR_INVALID_PARAMETER
    InvalidName = 123,        ///< ERROR_INVALID_NAME
};

// =============================================================================
// Window Class Manager
// =============================================================================

class WindowClassManager {
public:
    WindowClassManager();

    /// The executable's module handle (GetModuleHandle(NULL)). RegisterClass
    /// with a NULL hInstance registers the class for the executable, and
    /// UnregisterClass with a NULL hInstance searches the executable's
    /// classes (measured, s1win_class_probe reg_null_instance /
    /// unregister_local_null_instance).
    void set_executable_instance_provider(std::function<u64()> provider);
    [[nodiscard]] u64 executable_instance() const;

    /// user32's module handle: GCLP_HMODULE of a system class window
    /// (measured, s1win_class_probe button_window_hmodule_is_user32).
    void set_system_module_provider(std::function<u64()> provider);
    [[nodiscard]] u64 system_module() const;

    /// The system arrow cursor handle (LoadCursor(NULL, IDC_ARROW)): the class
    /// cursor of most system classes (measured, button_cursor_nonzero).
    void set_system_cursor_provider(std::function<u64()> provider);
    /// hCursor as GetClassInfo / GCLP_HCURSOR report it.
    [[nodiscard]] u64 class_cursor(const WindowClassData& cls) const;

    /// Asks whether any window still belongs to class registration @p id
    /// (UnregisterClass fails with ERROR_CLASS_HAS_WINDOWS while one does).
    void set_class_in_use_query(std::function<bool(u32)> query);

    /// Register a class. Returns its atom, or 0 with @p error set.
    ATOM register_class(const WindowClassData& data, ClassError& error);
    /// Register a class; failures set the thread's last error.
    ATOM register_class(const WindowClassData& data);

    /// UnregisterClass: the local class of @p hinstance (NULL = the
    /// executable), else a global class of that name, else — only for a NULL
    /// hInstance — the process's copy of a system class.
    bool unregister_class(std::wstring_view name, u64 hinstance,
                          ClassError& error);
    /// Unregister the newest class of that name, whatever its owner.
    bool unregister_class(std::wstring_view name);

    /// CreateWindowEx lookup (Learn: "How the System Locates a Window Class"):
    /// local classes of @p hinstance, then global classes, then system
    /// classes. A NULL hInstance matches the newest local class of any module
    /// (measured, s1win_class_probe create_null_instance_proc).
    [[nodiscard]] const WindowClassData* find_for_create(std::wstring_view name,
                                                         u64 hinstance) const;
    /// GetClassInfo(Ex) lookup: a NULL hInstance names only global and system
    /// classes; otherwise local classes of @p hinstance come first (measured,
    /// s1win_class_probe getclassinfo_null / global_getclassinfo_*).
    [[nodiscard]] const WindowClassData* find_for_info(std::wstring_view name,
                                                       u64 hinstance) const;

    /// Look up a class by atom (newest registration that uses the atom).
    const WindowClassData* find_by_atom(ATOM atom) const;
    WindowClassData* find_by_atom_mut(ATOM atom);

    /// Look up a class by its registration serial.
    const WindowClassData* find_by_id(u32 id) const;
    WindowClassData* find_by_id_mut(u32 id);

    /// Look up a class by name (case-insensitive), as CreateWindowEx with a
    /// NULL hInstance would.
    const WindowClassData* find_by_name(std::wstring_view name) const;

    /// Look up atom for a class by name (case-insensitive).
    /// Returns 0 if not found.
    ATOM find_atom(std::wstring_view name) const;

    /// The class name a class atom stands for ("#nnnnn" for integer atoms).
    [[nodiscard]] std::wstring atom_name(ATOM atom) const;

    /// Get number of registered application classes.
    usize count() const;

    /// Parse the "#nnnnn" string form of an integer atom; false when @p name
    /// is not one (Learn, GlobalAddAtom: "#1234" names integer atom 1234).
    [[nodiscard]] static bool parse_integer_atom(std::wstring_view name,
                                                 ATOM& atom);

private:
    ATOM acquire_atom_locked(std::wstring_view name);
    void release_atom_locked(ATOM atom);
    [[nodiscard]] const WindowClassData* find_local_locked(
        const std::wstring& upper, u64 hinstance, bool any_instance) const;
    [[nodiscard]] const WindowClassData* find_global_locked(
        const std::wstring& upper) const;
    [[nodiscard]] const WindowClassData* find_system_locked(
        const std::wstring& upper) const;
    [[nodiscard]] std::wstring normalize(std::wstring_view name) const;
    static std::wstring to_upper(std::wstring_view s);

    mutable std::recursive_mutex m_mutex;
    /// id -> class. Iterated newest-first (reverse) for lookups.
    std::map<u32, WindowClassData> m_classes;
    /// System classes by upper-case name.
    std::map<std::wstring, WindowClassData> m_system;
    struct AtomEntry { ATOM atom = 0; u32 refs = 0; std::wstring name; };
    std::map<std::wstring, AtomEntry> m_atoms;     ///< upper name -> atom
    std::map<ATOM, std::wstring> m_atom_upper;     ///< atom -> upper name
    ATOM m_next_atom;
    u32 m_next_id = 1;
    std::function<u64()> m_executable_instance;
    std::function<u64()> m_system_module;
    std::function<u64()> m_system_cursor;
    std::function<bool(u32)> m_class_in_use;

    static constexpr ATOM FIRST_APP_ATOM = 0xC000;
};

// =============================================================================
// Thunk Implementations
// =============================================================================

void impl_RegisterClassW(Marshaler& m, WindowClassManager& wcm);
void impl_RegisterClassExW(Marshaler& m, WindowClassManager& wcm);
void impl_UnregisterClassW(Marshaler& m, WindowClassManager& wcm);
void impl_GetClassInfoExW(Marshaler& m, WindowClassManager& wcm);
void impl_GetClassInfoExA(Marshaler& m, WindowClassManager& wcm);

/// GetClassInfo(A/W) and GetClassInfoEx(A/W) share one lookup and writer.
void get_class_info(Marshaler& m, WindowClassManager& wcm, bool ex, bool ansi);

// =============================================================================
// Export Registration (user32.dll)
// =============================================================================

[[nodiscard]] DllExports make_window_class_exports(WindowClassManager& wcm);

} // namespace vitreo::winapi
