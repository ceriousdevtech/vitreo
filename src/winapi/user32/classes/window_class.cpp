/// @file window_class.cpp
/// @brief RegisterClass*/UnregisterClass*/GetClassInfo* and the per-process
/// class tables (WIN32-050).
///
/// Contract: Microsoft Learn "About Window Classes", RegisterClassExW,
/// UnregisterClassW, GetClassInfoExW. Behaviour Learn leaves open (error codes,
/// the NULL-hInstance rules, integer-atom names, unregistering a system class)
/// is measured by tools/oracle_probe/s1win_class_probe.c and
/// s1win_api_probe.c ("-- classes").

#include "window_class.hpp"
#include "winapi/user32/classes/system_classes.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <cwctype>
#include <cstdio>
#include <cstdlib>

namespace vitreo::winapi {

namespace {

constexpr u32 kErrorClassDoesNotExist = 1411;  // ERROR_CLASS_DOES_NOT_EXIST
constexpr u32 kErrorClassHasWindows = 1412;    // ERROR_CLASS_HAS_WINDOWS
constexpr u32 kErrorInvalidName = 123;         // ERROR_INVALID_NAME
/// System classes get registration serials of their own, well above anything
/// an application can reach.
constexpr u32 kFirstSystemClassId = 0x80000000u;

} // namespace

// =============================================================================
// WindowClassManager
// =============================================================================

WindowClassManager::WindowClassManager()
    : m_next_atom{FIRST_APP_ATOM} {
    // The system registers its classes for the process before any application
    // code runs (Learn: About Window Classes, "System Classes"). Their atoms
    // come from the same atom table as application classes: string names get
    // string atoms, "#32770" and friends are integer atoms (measured,
    // s1win_class_probe sys_button_atom_is_string_atom / sysclass lines).
    u32 id = kFirstSystemClassId;
    for (const SystemClassInfo* info = system_classes_begin();
         info != system_classes_end(); ++info) {
        WindowClassData cls;
        (void)fill_system_class(info->name, cls);
        cls.id = id++;
        cls.atom = acquire_atom_locked(info->name);
        m_system[to_upper(info->name)] = std::move(cls);
    }
}

void WindowClassManager::set_executable_instance_provider(
    std::function<u64()> provider) {
    std::lock_guard lock(m_mutex);
    m_executable_instance = std::move(provider);
}

u64 WindowClassManager::executable_instance() const {
    std::lock_guard lock(m_mutex);
    return m_executable_instance ? m_executable_instance() : 0;
}

void WindowClassManager::set_system_module_provider(
    std::function<u64()> provider) {
    std::lock_guard lock(m_mutex);
    m_system_module = std::move(provider);
}

u64 WindowClassManager::system_module() const {
    std::lock_guard lock(m_mutex);
    return m_system_module ? m_system_module() : 0;
}

void WindowClassManager::set_system_cursor_provider(std::function<u64()> provider) {
    std::lock_guard lock(m_mutex);
    m_system_cursor = std::move(provider);
}

u64 WindowClassManager::class_cursor(const WindowClassData& cls) const {
    if (cls.hcursor != 0 || !cls.arrow_cursor) return cls.hcursor;
    std::lock_guard lock(m_mutex);
    return m_system_cursor ? m_system_cursor() : 0;
}

void WindowClassManager::set_class_in_use_query(std::function<bool(u32)> query) {
    std::lock_guard lock(m_mutex);
    m_class_in_use = std::move(query);
}

bool WindowClassManager::parse_integer_atom(std::wstring_view name, ATOM& atom) {
    // GlobalAddAtom (Learn): "If the string has the form #1234 it is an
    // integer atom." Class names follow the same atom-table rule (measured:
    // RegisterClassEx("#4660") returns 0x1234).
    if (name.size() < 2 || name[0] != L'#') return false;
    u32 value = 0;
    for (usize i = 1; i < name.size(); ++i) {
        const wchar_t c = name[i];
        if (c < L'0' || c > L'9') return false;
        value = value * 10 + static_cast<u32>(c - L'0');
        if (value >= FIRST_APP_ATOM) return false;
    }
    if (value == 0) return false;
    atom = static_cast<ATOM>(value);
    return true;
}

std::wstring WindowClassManager::normalize(std::wstring_view name) const {
    return to_upper(name);
}

ATOM WindowClassManager::acquire_atom_locked(std::wstring_view name) {
    ATOM integer = 0;
    if (parse_integer_atom(name, integer)) {
        m_atom_upper[integer] = to_upper(name);
        return integer;
    }
    const std::wstring upper = to_upper(name);
    auto it = m_atoms.find(upper);
    if (it != m_atoms.end()) {
        ++it->second.refs;
        return it->second.atom;
    }
    if (m_next_atom == 0xFFFF) return 0;
    AtomEntry entry;
    entry.atom = m_next_atom++;
    entry.refs = 1;
    entry.name = std::wstring(name);
    m_atom_upper[entry.atom] = upper;
    m_atoms.emplace(upper, entry);
    return entry.atom;
}

void WindowClassManager::release_atom_locked(ATOM atom) {
    if (atom < FIRST_APP_ATOM) return;  // integer atoms are never stored
    auto name = m_atom_upper.find(atom);
    if (name == m_atom_upper.end()) return;
    auto it = m_atoms.find(name->second);
    if (it == m_atoms.end()) return;
    if (it->second.refs > 1) {
        --it->second.refs;
        return;
    }
    m_atoms.erase(it);
    m_atom_upper.erase(name);
}

ATOM WindowClassManager::register_class(const WindowClassData& data,
                                        ClassError& error) {
    error = ClassError::None;
    if (data.class_name.empty()) {
        error = ClassError::InvalidName;
        return 0;
    }
    if (data.cls_extra < 0 || data.wnd_extra < 0) {
        error = ClassError::InvalidParameter;
        return 0;
    }

    std::lock_guard lock(m_mutex);
    WindowClassData entry = data;
    if (entry.hinstance == 0 && m_executable_instance) {
        entry.hinstance = m_executable_instance();
    }
    const std::wstring upper = normalize(entry.class_name);

    // A local class conflicts with a class of the same name owned by the same
    // module; a global class conflicts with any global class of that name. A
    // local class may share its name with a global class of another module
    // and with a system class (measured: global_reg_same_name_local,
    // registerclassexw_local_shadows_system).
    for (const auto& [id, cls] : m_classes) {
        (void)id;
        if (normalize(cls.class_name) != upper) continue;
        if (cls.hinstance == entry.hinstance ||
            (cls.global() && entry.global())) {
            error = ClassError::AlreadyExists;
            return 0;
        }
    }

    const ATOM atom = acquire_atom_locked(entry.class_name);
    if (atom == 0) {
        error = ClassError::InvalidParameter;
        return 0;
    }
    entry.atom = atom;
    entry.id = m_next_id++;
    entry.system = false;
    entry.extra.assign(static_cast<usize>(entry.cls_extra), 0);
    m_classes.emplace(entry.id, std::move(entry));
    return atom;
}

ATOM WindowClassManager::register_class(const WindowClassData& data) {
    ClassError error = ClassError::None;
    const ATOM atom = register_class(data, error);
    if (atom == 0) set_last_error(static_cast<u32>(error));
    return atom;
}

const WindowClassData* WindowClassManager::find_local_locked(
    const std::wstring& upper, u64 hinstance, bool any_instance) const {
    for (auto it = m_classes.rbegin(); it != m_classes.rend(); ++it) {
        const WindowClassData& cls = it->second;
        if (cls.global()) continue;
        if (!any_instance && cls.hinstance != hinstance) continue;
        if (normalize(cls.class_name) == upper) return &cls;
    }
    return nullptr;
}

const WindowClassData* WindowClassManager::find_global_locked(
    const std::wstring& upper) const {
    for (auto it = m_classes.rbegin(); it != m_classes.rend(); ++it) {
        const WindowClassData& cls = it->second;
        if (cls.global() && normalize(cls.class_name) == upper) return &cls;
    }
    return nullptr;
}

const WindowClassData* WindowClassManager::find_system_locked(
    const std::wstring& upper) const {
    auto it = m_system.find(upper);
    return it == m_system.end() ? nullptr : &it->second;
}

const WindowClassData* WindowClassManager::find_for_create(
    std::wstring_view name, u64 hinstance) const {
    std::lock_guard lock(m_mutex);
    const std::wstring upper = normalize(name);
    if (const auto* local = find_local_locked(upper, hinstance, hinstance == 0)) {
        return local;
    }
    if (const auto* global = find_global_locked(upper)) return global;
    return find_system_locked(upper);
}

const WindowClassData* WindowClassManager::find_for_info(
    std::wstring_view name, u64 hinstance) const {
    std::lock_guard lock(m_mutex);
    const std::wstring upper = normalize(name);
    if (hinstance != 0) {
        if (const auto* local = find_local_locked(upper, hinstance, false)) {
            return local;
        }
    }
    if (const auto* global = find_global_locked(upper)) return global;
    return find_system_locked(upper);
}

bool WindowClassManager::unregister_class(std::wstring_view name, u64 hinstance,
                                          ClassError& error) {
    error = ClassError::None;
    std::function<bool(u32)> in_use;
    const WindowClassData* victim = nullptr;
    bool system = false;
    {
        std::lock_guard lock(m_mutex);
        const std::wstring upper = normalize(name);
        const u64 owner = hinstance != 0
            ? hinstance
            : (m_executable_instance ? m_executable_instance() : 0);
        victim = find_local_locked(upper, owner, false);
        if (!victim) victim = find_global_locked(upper);
        // Only a NULL hInstance reaches the process's copy of a system class
        // (measured: unregister_system_button_null succeeds and later
        // lookups fail; unregister_system_edit_hi / _static_user32 fail).
        if (!victim && hinstance == 0) {
            victim = find_system_locked(upper);
            system = victim != nullptr;
        }
        if (!victim) {
            error = ClassError::DoesNotExist;
            return false;
        }
        in_use = m_class_in_use;
    }
    // The in-use query walks the window table; ask it without holding the
    // class lock so the two locks never nest in opposite orders.
    const u32 id = victim->id;
    if (in_use && in_use(id)) {
        error = ClassError::HasWindows;
        return false;
    }
    std::lock_guard lock(m_mutex);
    if (system) {
        for (auto it = m_system.begin(); it != m_system.end(); ++it) {
            if (it->second.id == id) {
                release_atom_locked(it->second.atom);
                m_system.erase(it);
                break;
            }
        }
        return true;
    }
    auto it = m_classes.find(id);
    if (it == m_classes.end()) {
        error = ClassError::DoesNotExist;
        return false;
    }
    release_atom_locked(it->second.atom);
    m_classes.erase(it);
    return true;
}

bool WindowClassManager::unregister_class(std::wstring_view name) {
    std::lock_guard lock(m_mutex);
    const std::wstring upper = normalize(name);
    for (auto it = m_classes.rbegin(); it != m_classes.rend(); ++it) {
        if (normalize(it->second.class_name) == upper) {
            release_atom_locked(it->second.atom);
            m_classes.erase(std::next(it).base());
            return true;
        }
    }
    return false;
}

const WindowClassData* WindowClassManager::find_by_atom(ATOM atom) const {
    std::lock_guard lock(m_mutex);
    for (auto it = m_classes.rbegin(); it != m_classes.rend(); ++it) {
        if (it->second.atom == atom) return &it->second;
    }
    for (const auto& [name, cls] : m_system) {
        (void)name;
        if (cls.atom == atom) return &cls;
    }
    return nullptr;
}

WindowClassData* WindowClassManager::find_by_atom_mut(ATOM atom) {
    return const_cast<WindowClassData*>(
        static_cast<const WindowClassManager*>(this)->find_by_atom(atom));
}

const WindowClassData* WindowClassManager::find_by_id(u32 id) const {
    if (id == 0) return nullptr;
    std::lock_guard lock(m_mutex);
    if (auto it = m_classes.find(id); it != m_classes.end()) return &it->second;
    for (const auto& [name, cls] : m_system) {
        (void)name;
        if (cls.id == id) return &cls;
    }
    return nullptr;
}

WindowClassData* WindowClassManager::find_by_id_mut(u32 id) {
    return const_cast<WindowClassData*>(
        static_cast<const WindowClassManager*>(this)->find_by_id(id));
}

const WindowClassData* WindowClassManager::find_by_name(std::wstring_view name) const {
    return find_for_create(name, 0);
}

ATOM WindowClassManager::find_atom(std::wstring_view name) const {
    const WindowClassData* cls = find_by_name(name);
    return cls ? cls->atom : 0;
}

std::wstring WindowClassManager::atom_name(ATOM atom) const {
    std::lock_guard lock(m_mutex);
    for (auto it = m_classes.rbegin(); it != m_classes.rend(); ++it) {
        if (it->second.atom == atom) return it->second.class_name;
    }
    for (const auto& [name, cls] : m_system) {
        (void)name;
        if (cls.atom == atom) return cls.class_name;
    }
    if (atom != 0 && atom < FIRST_APP_ATOM) return L"#" + std::to_wstring(atom);
    return {};
}

usize WindowClassManager::count() const {
    std::lock_guard lock(m_mutex);
    return m_classes.size();
}

std::wstring WindowClassManager::to_upper(std::wstring_view s) {
    std::wstring result(s);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towupper(ch)); });
    return result;
}

// =============================================================================
// Thunk Implementations
// =============================================================================

namespace {

/// WNDCLASS / WNDCLASSEX field offsets (winuser.h; the A and W structures
/// share one layout, and every pointer-sized field widens on x64).
struct ClassLayout {
    bool ex;
    bool is64;
    [[nodiscard]] u32 size() const { return ex ? (is64 ? 80u : 48u) : (is64 ? 72u : 40u); }
    [[nodiscard]] u32 style() const { return ex ? 4u : 0u; }
    [[nodiscard]] u32 wndproc() const { return ex ? 8u : (is64 ? 8u : 4u); }
    [[nodiscard]] u32 cls_extra() const { return ex ? (is64 ? 16u : 12u) : (is64 ? 16u : 8u); }
    [[nodiscard]] u32 wnd_extra() const { return cls_extra() + 4u; }
    [[nodiscard]] u32 hinstance() const { return ex ? (is64 ? 24u : 20u) : (is64 ? 24u : 16u); }
    [[nodiscard]] u32 ptr_size() const { return is64 ? 8u : 4u; }
    [[nodiscard]] u32 hicon() const { return hinstance() + ptr_size(); }
    [[nodiscard]] u32 hcursor() const { return hicon() + ptr_size(); }
    [[nodiscard]] u32 hbr() const { return hcursor() + ptr_size(); }
    [[nodiscard]] u32 menu() const { return hbr() + ptr_size(); }
    [[nodiscard]] u32 class_name() const { return menu() + ptr_size(); }
    [[nodiscard]] u32 hicon_sm() const { return class_name() + ptr_size(); }
};

u64 read_ptr(Marshaler& m, GuestAddr at, bool is64) {
    if (is64) return m.read_u64(at).value_or(0);
    return static_cast<u64>(m.read_u32(at).value_or(0));
}

void write_ptr(Marshaler& m, GuestAddr at, u64 value, bool is64) {
    if (is64) (void)m.write_u64(at, value);
    else (void)m.write_u32(at, static_cast<u32>(value));
}

/// Read a class-name argument: a string, or MAKEINTATOM(n) rendered as the
/// "#n" name that stands for integer atom n. Returns false for NULL.
bool read_class_name(Marshaler& m, GuestAddr ptr, bool ansi, std::wstring& out) {
    if (ptr == 0) return false;
    if (ptr < 0x10000) {
        out = L"#" + std::to_wstring(static_cast<u32>(ptr));
        return true;
    }
    auto r = ansi ? m.read_ansi_string(ptr) : m.read_wide_string(ptr);
    if (!r.is_ok()) return false;
    out = utf8_to_wstring(r.value());
    return true;
}

/// Resolve a class-name argument that may be an application class atom
/// (0xC000..0xFFFF) back to its registered name.
std::wstring resolve_atom_argument(const WindowClassManager& wcm,
                                   const std::wstring& name) {
    ATOM atom = 0;
    if (name.size() > 1 && name[0] == L'#') {
        u32 value = 0;
        bool digits = true;
        for (usize i = 1; i < name.size(); ++i) {
            if (name[i] < L'0' || name[i] > L'9') { digits = false; break; }
            value = value * 10 + static_cast<u32>(name[i] - L'0');
            if (value > 0xFFFF) { digits = false; break; }
        }
        if (digits && value >= 0xC000) atom = static_cast<ATOM>(value);
    }
    if (atom == 0) return name;
    const std::wstring registered = wcm.atom_name(atom);
    return registered.empty() ? name : registered;
}

void register_class_common(Marshaler& m, WindowClassManager& wcm, bool ex, bool ansi) {
    const GuestAddr struct_ptr = m.arg_ptr(0);
    if (struct_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const ClassLayout L{ex, m.cpu_is_64bit()};
    if (ex) {
        const auto cb = m.read_u32(struct_ptr + 0);
        // Measured (registerclassexw_bad_cbsize): a wrong cbSize fails with
        // ERROR_INVALID_PARAMETER.
        if (!cb.is_ok() || cb.value() != L.size()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
    }
    const auto style = m.read_u32(struct_ptr + L.style());
    if (!style.is_ok()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }

    WindowClassData data{};
    data.style = style.value();
    data.wndproc = read_ptr(m, struct_ptr + L.wndproc(), L.is64);
    data.cls_extra = static_cast<i32>(m.read_u32(struct_ptr + L.cls_extra()).value_or(0));
    data.wnd_extra = static_cast<i32>(m.read_u32(struct_ptr + L.wnd_extra()).value_or(0));
    data.hinstance = read_ptr(m, struct_ptr + L.hinstance(), L.is64);
    data.hicon = read_ptr(m, struct_ptr + L.hicon(), L.is64);
    data.hcursor = read_ptr(m, struct_ptr + L.hcursor(), L.is64);
    data.hbr_background = read_ptr(m, struct_ptr + L.hbr(), L.is64);
    data.menu_name_ptr = read_ptr(m, struct_ptr + L.menu(), L.is64);
    if (ex) data.hicon_sm = read_ptr(m, struct_ptr + L.hicon_sm(), L.is64);
    // Learn (About Window Classes, "Class Icons"): given a large icon but no
    // small one, the system supplies a small class icon from the large one.
    // Vitreo's icon handles carry every image size, so the large handle is
    // the small icon too (CopyImage does not create a distinct handle yet).
    if (data.hicon_sm == 0) data.hicon_sm = data.hicon;
    data.unicode = !ansi;

    const GuestAddr name_ptr =
        static_cast<GuestAddr>(read_ptr(m, struct_ptr + L.class_name(), L.is64));
    if (name_ptr == 0) {
        // Measured (registerclassexw_null_name): ERROR_INVALID_PARAMETER.
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    (void)read_class_name(m, name_ptr, ansi, data.class_name);
    if (data.class_name.empty()) {
        // Measured (registerclassexw_empty_name): ERROR_INVALID_NAME.
        set_last_error(kErrorInvalidName);
        m.set_return(0);
        return;
    }
    if (data.menu_name_ptr != 0 && data.menu_name_ptr < 0x10000) {
        data.menu_name = std::to_wstring(data.menu_name_ptr);
    } else if (data.menu_name_ptr != 0) {
        auto r = ansi ? m.read_ansi_string(static_cast<GuestAddr>(data.menu_name_ptr))
                      : m.read_wide_string(static_cast<GuestAddr>(data.menu_name_ptr));
        if (r.is_ok()) data.menu_name = utf8_to_wstring(r.value());
    }

    ClassError error = ClassError::None;
    const ATOM atom = wcm.register_class(data, error);
    if (atom == 0) set_last_error(static_cast<u32>(error));
    VITREO_TRACE("[wclass] RegisterClass%s%c class=\"%ls\" style=0x%x wndproc=0x%llx "
                 "hinst=0x%llx hbr=0x%llx -> 0x%x\n",
                 ex ? "Ex" : "", ansi ? 'A' : 'W', data.class_name.c_str(),
                 data.style, static_cast<unsigned long long>(data.wndproc),
                 static_cast<unsigned long long>(data.hinstance),
                 static_cast<unsigned long long>(data.hbr_background), atom);
    static const bool win_diag = std::getenv("VITREO_WIN_DIAG") != nullptr;
    if (win_diag) {
        std::fprintf(stderr,
                     "[hwnd] RegisterClass class='%ls' style=0x%x "
                     "hbrBackground=0x%llx\n",
                     data.class_name.c_str(), data.style,
                     static_cast<unsigned long long>(data.hbr_background));
    }
    m.set_return(static_cast<u64>(atom));
}

void unregister_class_common(Marshaler& m, WindowClassManager& wcm, bool ansi) {
    const GuestAddr name_ptr = m.arg_ptr(0);
    const u64 hinstance = m.arg(1);
    std::wstring name;
    if (!read_class_name(m, name_ptr, ansi, name) || name.empty()) {
        set_last_error(kErrorClassDoesNotExist);
        m.set_return(0);
        return;
    }
    name = resolve_atom_argument(wcm, name);
    ClassError error = ClassError::None;
    const bool ok = wcm.unregister_class(name, hinstance, error);
    if (!ok) {
        set_last_error(error == ClassError::HasWindows ? kErrorClassHasWindows
                                                       : kErrorClassDoesNotExist);
    }
    VITREO_TRACE("[wclass] UnregisterClass%c('%ls', 0x%llx) -> %d\n",
                 ansi ? 'A' : 'W', name.c_str(),
                 static_cast<unsigned long long>(hinstance), ok ? 1 : 0);
    m.set_return(ok ? 1u : 0u);
}

} // namespace

void get_class_info(Marshaler& m, WindowClassManager& wcm, bool ex, bool ansi) {
    const u64 hinstance = m.arg(0);
    const GuestAddr name_ptr = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(2);
    std::wstring name;
    if (!read_class_name(m, name_ptr, ansi, name) || out == 0) {
        set_last_error(kErrorClassDoesNotExist);
        m.set_return(0);
        return;
    }
    name = resolve_atom_argument(wcm, name);
    const WindowClassData* cls = wcm.find_for_info(name, hinstance);
    if (!cls) {
        // Measured (getclassinfoexw_missing): ERROR_CLASS_DOES_NOT_EXIST and
        // the output structure is left untouched.
        VITREO_TRACE("[wclass] GetClassInfo%s%c('%ls', 0x%llx) -> FALSE\n",
                     ex ? "Ex" : "", ansi ? 'A' : 'W', name.c_str(),
                     static_cast<unsigned long long>(hinstance));
        set_last_error(kErrorClassDoesNotExist);
        m.set_return(0);
        return;
    }
    const ClassLayout L{ex, m.cpu_is_64bit()};
    // Measured (getclassinfoexw_zero_cbsize_ok): cbSize is neither checked
    // nor written; the caller's value survives.
    (void)m.write_u32(out + L.style(), cls->style);
    write_ptr(m, out + L.wndproc(), resolve_system_class_wndproc(cls->wndproc, ansi), L.is64);
    (void)m.write_u32(out + L.cls_extra(), static_cast<u32>(cls->cls_extra));
    (void)m.write_u32(out + L.wnd_extra(), static_cast<u32>(cls->wnd_extra));
    // System classes report a NULL hInstance (measured, button_hinstance_zero).
    write_ptr(m, out + L.hinstance(), cls->system ? 0 : cls->hinstance, L.is64);
    write_ptr(m, out + L.hicon(), cls->hicon, L.is64);
    write_ptr(m, out + L.hcursor(), wcm.class_cursor(*cls), L.is64);
    write_ptr(m, out + L.hbr(), cls->hbr_background, L.is64);
    write_ptr(m, out + L.menu(), cls->menu_name_ptr, L.is64);
    // lpszClassName is the caller's own argument (measured,
    // classname_is_input / button_classname_is_input).
    write_ptr(m, out + L.class_name(), name_ptr, L.is64);
    if (ex) write_ptr(m, out + L.hicon_sm(), cls->hicon_sm, L.is64);
    VITREO_TRACE("[wclass] GetClassInfo%s%c('%ls', 0x%llx) -> atom 0x%x\n",
                 ex ? "Ex" : "", ansi ? 'A' : 'W', name.c_str(),
                 static_cast<unsigned long long>(hinstance), cls->atom);
    // Learn: "If the function finds a matching class ... the return value is
    // a nonzero value"; Windows returns the class atom (measured,
    // getclassinfoexw_returns_atom, getclassinfow_returns_atom).
    m.set_return(static_cast<u64>(cls->atom));
}

void impl_RegisterClassW(Marshaler& m, WindowClassManager& wcm) {
    register_class_common(m, wcm, /*ex=*/false, /*ansi=*/false);
}

void impl_RegisterClassExW(Marshaler& m, WindowClassManager& wcm) {
    register_class_common(m, wcm, /*ex=*/true, /*ansi=*/false);
}

void impl_UnregisterClassW(Marshaler& m, WindowClassManager& wcm) {
    unregister_class_common(m, wcm, /*ansi=*/false);
}

void impl_GetClassInfoExW(Marshaler& m, WindowClassManager& wcm) {
    get_class_info(m, wcm, /*ex=*/true, /*ansi=*/false);
}

void impl_GetClassInfoExA(Marshaler& m, WindowClassManager& wcm) {
    get_class_info(m, wcm, /*ex=*/true, /*ansi=*/true);
}

void impl_RegisterClassA(Marshaler& m, WindowClassManager& wcm) {
    register_class_common(m, wcm, /*ex=*/false, /*ansi=*/true);
}

void impl_RegisterClassExA(Marshaler& m, WindowClassManager& wcm) {
    register_class_common(m, wcm, /*ex=*/true, /*ansi=*/true);
}

// =============================================================================
// Export Registration
// =============================================================================

DllExports make_window_class_exports(WindowClassManager& wcm) {
    DllExports exports;
    exports.entries.push_back(ExportEntry{
        "RegisterClassW", 1,
        [&wcm](Marshaler& m) { impl_RegisterClassW(m, wcm); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "RegisterClassA", 0,
        [&wcm](Marshaler& m) { impl_RegisterClassA(m, wcm); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "RegisterClassExW", 2,
        [&wcm](Marshaler& m) { impl_RegisterClassExW(m, wcm); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "RegisterClassExA", 0,
        [&wcm](Marshaler& m) { impl_RegisterClassExA(m, wcm); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "UnregisterClassW", 3,
        [&wcm](Marshaler& m) { impl_UnregisterClassW(m, wcm); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "UnregisterClassA", 0,
        [&wcm](Marshaler& m) { unregister_class_common(m, wcm, /*ansi=*/true); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "GetClassInfoExW", 4,
        [&wcm](Marshaler& m) { impl_GetClassInfoExW(m, wcm); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "GetClassInfoExA", 0,
        [&wcm](Marshaler& m) { impl_GetClassInfoExA(m, wcm); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "GetClassInfoW", 0,
        [&wcm](Marshaler& m) { get_class_info(m, wcm, /*ex=*/false, /*ansi=*/false); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "GetClassInfoA", 0,
        [&wcm](Marshaler& m) { get_class_info(m, wcm, /*ex=*/false, /*ansi=*/true); },
        12
    });
    return exports;
}

} // namespace vitreo::winapi
