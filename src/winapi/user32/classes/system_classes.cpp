// SPDX-License-Identifier: LGPL-2.1-only

/// @file system_classes.cpp
/// @brief System-registered window class table.

#include "winapi/user32/classes/system_classes.hpp"

#include "winapi/user32/user32_internal.hpp"

#include <array>
#include <atomic>
#include <cwctype>

namespace vitreo::winapi {

namespace {

// Captured with GetClassInfoExW on a real Windows installation; see
// tests/winapi/user32/oracle_system_classes.txt. hbrBackground is the raw
// value Windows reports (COLOR_* + 1), which is 0 for the six user32 classes
// because they paint themselves.
constexpr std::array<SystemClassInfo, 28> kSystemClasses{{
    // user32 — always present, registered before any application code runs.
    {L"Button",             0x0000008Bu, 0,  8, 0x00000000u, true},
    {L"Edit",               0x00000088u, 0,  8, 0x00000000u, true},
    {L"Static",             0x00000088u, 0,  8, 0x00000000u, true},
    {L"ListBox",            0x00000088u, 0,  8, 0x00000000u, true},
    {L"ComboBox",           0x0000008Bu, 0,  8, 0x00000000u, true},
    {L"ScrollBar",          0x00000088u, 0, 80, 0x00000000u, true},
    {L"ComboLBox",          0x00000808u, 0,  8, 0x00000000u, true},
    {L"MDIClient",          0x00000000u, 0, 16, 0x0000000Du, true},
    // The system-only classes Learn lists ("About Window Classes"). Their
    // registrations are measured the same way (s1win_class_probe sysclass
    // lines); #32771 (task switch) is not registered in an application.
    {L"#32768",             0x00020808u, 0, 24, 0x00000005u, false},
    {L"#32769",             0x00000008u, 0,  0, 0x00000002u, true},
    {L"#32770",             0x00000808u, 0, 30, 0x00000000u, true},
    {L"#32772",             0x00000000u, 0,  0, 0x00000000u, true},
    {L"Message",            0x00000000u, 0,  4, 0x00000000u, true},
    {L"IME",                0x00000000u, 0,  8, 0x00000000u, true},
    {L"DDEMLEvent",         0x00000000u, 0,  8, 0x00000000u, false},
    // comctl32 — present once the common controls are initialised. Vitreo
    // provides comctl32 for every process, so they are always reportable.
    {L"SysListView32",      0x00004008u, 0,  4, 0x00000006u, true},
    {L"SysTreeView32",      0x00004008u, 0,  4, 0x00000000u, true},
    {L"SysTabControl32",    0x0000400Bu, 0,  4, 0x00000010u, true},
    {L"msctls_progress32",  0x00004003u, 0,  4, 0x00000010u, true},
    {L"msctls_trackbar32",  0x00004000u, 0,  4, 0x00000010u, true},
    {L"SysHeader32",        0x00004008u, 0,  4, 0x00000010u, false},
    {L"ToolbarWindow32",    0x00004008u, 0,  4, 0x00000010u, true},
    {L"tooltips_class32",   0x00004808u, 0,  4, 0x00000000u, true},
    {L"SysAnimate32",       0x00004008u, 0,  4, 0x00000010u, true},
    {L"msctls_updown32",    0x00004003u, 0,  4, 0x00000010u, true},
    {L"ReBarWindow32",      0x00004008u, 0,  4, 0x00000010u, false},
    {L"msctls_statusbar32", 0x00004009u, 0,  4, 0x00000010u, true},
    // SysLink: tools/oracle_probe/syslink_probe{32,64}.expected line 3.
    {L"SysLink",            0x00004000u, 0,  4, 0x00000006u, true},
}};

std::array<std::array<std::atomic<u64>, 2>, kSystemClasses.size()> g_procedures{};
// Runtime-private opaque identities, not Windows constants or exports.
constexpr u64 procedure_identity(usize index, bool ansi) {
    return kNativeControlWndProc + 0x1000 + index * 2 + (ansi ? 1 : 0);
}

[[nodiscard]] bool iequals(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (std::towlower(static_cast<std::wint_t>(a[i])) !=
            std::towlower(static_cast<std::wint_t>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

const SystemClassInfo* system_classes_begin() { return kSystemClasses.data(); }
const SystemClassInfo* system_classes_end() {
    return kSystemClasses.data() + kSystemClasses.size();
}

const SystemClassInfo* find_system_class(std::wstring_view name) {
    if (name.empty()) return nullptr;
    for (const auto& entry : kSystemClasses) {
        if (iequals(name, entry.name)) return &entry;
    }
    return nullptr;
}

u64 system_class_wndproc(std::wstring_view name, bool ansi) {
    const auto* info = find_system_class(name);
    if (!info) return 0;
    const auto index = static_cast<usize>(info - kSystemClasses.data());
    const u64 address = g_procedures[index][ansi ? 1 : 0].load();
    return address ? address : procedure_identity(index, ansi);
}

void set_system_class_wndproc(std::wstring_view name, bool ansi, u64 address) {
    const auto* info = find_system_class(name);
    if (info) g_procedures[static_cast<usize>(info - kSystemClasses.data())][ansi ? 1 : 0].store(address);
}

const SystemClassInfo* find_system_class_by_wndproc(u64 address) {
    if (!address) return nullptr;
    for (usize i = 0; i < kSystemClasses.size(); ++i) {
        for (bool ansi : {false, true}) {
            if (address == procedure_identity(i, ansi) ||
                address == g_procedures[i][ansi ? 1 : 0].load()) return &kSystemClasses[i];
        }
    }
    return nullptr;
}

u64 resolve_system_class_wndproc(u64 address, bool ansi) {
    const auto* info = find_system_class_by_wndproc(address);
    return info ? system_class_wndproc(info->name, ansi) : address;
}

bool fill_system_class(std::wstring_view name, WindowClassData& out) {
    const SystemClassInfo* info = find_system_class(name);
    if (info == nullptr) return false;
    out = WindowClassData{};
    out.class_name     = std::wstring(info->name);
    out.style          = info->style;
    out.cls_extra      = info->cls_extra;
    out.wnd_extra      = info->wnd_extra;
    out.hbr_background = info->hbr_background;
    // System classes belong to the system, not to a module: GetClassInfo
    // reports hInstance as NULL for them, and callers use that to tell a
    // system class from one of their own.
    out.hinstance = 0;
    // Must be callable: subclassers save this and pass it to CallWindowProc.
    out.wndproc = system_class_wndproc(info->name);
    // DDEMLEvent is registered without a window procedure (measured,
    // s1win_class_probe sysclass DDEMLEvent proc=0).
    if (iequals(info->name, L"DDEMLEvent")) out.wndproc = 0;
    // The cursor handle and atom are filled by WindowClassManager, which owns
    // the process's copies of the system classes.
    out.hcursor = 0;
    out.atom    = 0;
    out.system  = true;
    out.arrow_cursor = info->arrow_cursor;
    return true;
}

} // namespace vitreo::winapi
