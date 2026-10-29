/// @file registry.cpp
/// @brief advapi32 registry API thunks implementation (WIN32-060)

#include "winapi/advapi32/registry/registry.hpp"
#include "winapi/advapi32/registry/registry_internal.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>
#include <variant>

namespace vitreo::winapi {

// =============================================================================
// Helpers
// =============================================================================

// VITREO_LOG_REGISTRY=1 emits one line per key open and value query, at the
// same low volume as the [ipc] logs. The detailed registry traces are behind
// the global VITREO_TRACE, which also turns on the CPU and loader traces and is
// unusable on a real application run. This answers "did the guest ever ask for
// this value, and what did it get back" without that cost.
static bool registry_log_enabled() {
    static const bool on = std::getenv("VITREO_LOG_REGISTRY") != nullptr;
    return on;
}

void log_registry_open(const char* api, u64 hkey,
                              const std::wstring& subkey, u32 err) {
    if (!registry_log_enabled()) return;
    std::fprintf(stderr, "[registry] %s(0x%llx, \"%ls\") -> err=%u\n", api,
                 static_cast<unsigned long long>(hkey), subkey.c_str(), err);
}

void log_registry_query(const char* api, u64 hkey,
                               const std::wstring& value_name, u32 err,
                               u32 type, u32 size) {
    if (!registry_log_enabled()) return;
    std::fprintf(stderr,
                 "[registry] %s(0x%llx, \"%ls\") -> err=%u type=%u size=%u\n",
                 api, static_cast<unsigned long long>(hkey),
                 value_name.c_str(), err, type, size);
}

std::wstring ansi_to_wide(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (char c : s) {
        w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return w;
}

static std::wstring expand_environment(
    const std::wstring& input, const EnvironmentManager& environment) {
    std::wstring expanded;
    expanded.reserve(input.size());
    usize position = 0;
    while (position < input.size()) {
        if (input[position] != L'%') {
            expanded.push_back(input[position++]);
            continue;
        }

        const usize end = input.find(L'%', position + 1);
        if (end == std::wstring::npos || end == position + 1) {
            expanded.push_back(input[position++]);
            continue;
        }

        std::string name;
        name.reserve(end - position - 1);
        bool ansi_name = true;
        for (usize i = position + 1; i < end; ++i) {
            if (input[i] > 0x7f) {
                ansi_name = false;
                break;
            }
            name.push_back(static_cast<char>(input[i]));
        }
        const auto value = ansi_name
            ? environment.get_variable(name)
            : VResult<std::string>::err(Error::NotFound);
        if (value.is_err()) {
            expanded.append(input, position, end - position + 1);
        } else {
            for (const char ch : value.value()) {
                expanded.push_back(static_cast<wchar_t>(
                    static_cast<unsigned char>(ch)));
            }
        }
        position = end + 1;
    }
    return expanded;
}

/// Read a null-terminated UTF-16LE string from guest memory as std::wstring.
std::wstring read_guest_wstring(GuestMemory& mem, GuestAddr ptr,
                                usize max_chars) {
    std::wstring out;
    if (ptr == 0) return out;
    for (usize i = 0; i < max_chars; ++i) {
        auto r = mem.read_u16(ptr + i * 2);
        if (r.is_err()) break;
        u16 ch = r.value();
        if (ch == 0) break;
        out.push_back(static_cast<wchar_t>(ch));
    }
    return out;
}

/// Read a null-terminated ANSI string from guest memory as std::string.
std::string read_guest_astring(GuestMemory& mem, GuestAddr ptr,
                               usize max_chars) {
    std::string out;
    if (ptr == 0) return out;
    for (usize i = 0; i < max_chars; ++i) {
        auto r = mem.read_u8(ptr + i);
        if (r.is_err()) break;
        u8 ch = r.value();
        if (ch == 0) break;
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

static std::wstring append_canonical_subkey(
    std::wstring base, std::wstring_view subkey) {
    for (wchar_t ch : subkey) {
        const wchar_t normalized = ch == L'/' ? L'\\' : std::towlower(ch);
        if (normalized == L'\\' && !base.empty() && base.back() == L'\\') {
            continue;
        }
        base.push_back(normalized);
    }
    while (!base.empty() && base.back() == L'\\') base.pop_back();
    return base;
}

/// Write a std::wstring to guest memory as UTF-16LE with null terminator.
void write_guest_wstring(GuestMemory& mem, GuestAddr ptr,
                                const std::wstring& s, u32 max_chars) {
    u32 n = static_cast<u32>(s.size());
    if (n > max_chars - 1) n = max_chars - 1;
    for (u32 i = 0; i < n; ++i) {
        (void)mem.write_u16(ptr + i * 2, static_cast<u16>(s[i]));
    }
    (void)mem.write_u16(ptr + n * 2, 0);
}

/// Write a PHKEY result using the guest pointer width. HKEY is a pointer-sized
/// opaque handle on Windows even though Vitreo's allocated handle values
/// currently fit in 32 bits.
void write_guest_hkey(Marshaler& m, GuestAddr ptr, u64 hkey) {
    if (m.cpu_is_64bit()) {
        (void)m.write_u64(ptr, hkey);
    } else {
        (void)m.write_u32(ptr, static_cast<u32>(hkey));
    }
}

// =============================================================================
// Construction
// =============================================================================

RegistryThunks::RegistryThunks(capsule::Registry& registry, HandleTable& handles)
    : m_registry(registry)
    , m_handles(handles)
{}

// =============================================================================
// resolve_hkey
// =============================================================================

namespace {

constexpr u32 KEY_WOW64_64KEY = 0x00000100u;
constexpr u32 KEY_WOW64_32KEY = 0x00000200u;
constexpr u32 KEY_WOW64_RES = KEY_WOW64_64KEY | KEY_WOW64_32KEY;

}  // namespace

bool RegistryThunks::wow64_view_from_sam(u32 sam_desired,
                                         capsule::RegView& view) noexcept {
    // Learn, "Accessing an Alternate Registry View": the two flags are
    // exclusive. Both together is ERROR_INVALID_PARAMETER for RegOpenKeyEx,
    // RegCreateKeyEx and RegDeleteKeyEx (wow64_registry_probe
    // open_both_flags / create_both_flags / delex_both, x86 and x64).
    switch (sam_desired & KEY_WOW64_RES) {
        case KEY_WOW64_64KEY: view = capsule::RegView::Native64; return true;
        case KEY_WOW64_32KEY: view = capsule::RegView::Wow32; return true;
        case 0: view = capsule::RegView::Default; return true;
        default: return false;
    }
}

capsule::RegHandle RegistryThunks::resolve_hkey(u64 hkey) const {
    // The Win64 headers define predefined HKEY values by casting a signed
    // 32-bit constant through LONG_PTR, so native x64 callers pass values such
    // as 0xffffffff80000001 for HKEY_CURRENT_USER. x86 callers and some guest
    // code use the equivalent zero-extended value. Accept both encodings, but
    // do not truncate arbitrary 64-bit handles into the predefined range.
    const u32 low = static_cast<u32>(hkey);
    const u64 high = hkey >> 32;
    const bool canonical_extension = high == 0 || high == 0xffffffffu;
    const bool predefined =
        low == capsule::HKEY_CLASSES_ROOT ||
        low == capsule::HKEY_CURRENT_USER ||
        low == capsule::HKEY_LOCAL_MACHINE ||
        low == capsule::HKEY_USERS;
    if (canonical_extension && predefined) {
        return static_cast<capsule::RegHandle>(low);
    }

    // Otherwise the hkey is a Handle allocated by our HandleTable.
    auto* entry = m_handles.get_entry(static_cast<Handle>(hkey));
    if (entry && entry->type == HandleType::Registry) {
        // The object pointer stores the RegHandle value.
        return static_cast<capsule::RegHandle>(
            reinterpret_cast<uintptr_t>(entry->object));
    }

    // Return 0 — will fail resolution in the capsule Registry.
    return 0;
}

std::optional<std::wstring> RegistryThunks::key_path(u64 hkey) const {
    const capsule::RegHandle handle = resolve_hkey(hkey);
    if (handle == 0) return std::nullopt;
    return m_registry.canonical_key_path(handle);
}

u32 RegistryThunks::reg_delete_key(u64 hkey, const std::wstring& subkey,
                                   capsule::RegView view) {
    auto handle = resolve_hkey(hkey);
    if (handle == 0) return ERROR_INVALID_HANDLE;
    const auto parent_path = m_registry.canonical_key_path(handle);
    auto r = m_registry.delete_key(handle, subkey, view);
    if (r.is_ok()) {
        if (parent_path.has_value()) {
            notify_registry_change(
                subkey.empty()
                    ? *parent_path
                    : append_canonical_subkey(*parent_path + L"\\", subkey),
                0x00000001, true);
        }
        return ERROR_SUCCESS;
    }
    switch (r.error()) {
        case Error::NotFound:      return ERROR_FILE_NOT_FOUND;
        case Error::AccessDenied:  return ERROR_ACCESS_DENIED;
        default:                   return ERROR_INVALID_HANDLE;
    }
}

u32 RegistryThunks::reg_delete_key_ex(u64 hkey,
                                      const std::wstring& subkey,
                                      u32 sam_desired,
                                      u32 reserved) {
    // samDesired selects the view; its access bits are ignored (Windows 11
    // deletes with KEY_READ or KEY_READ|KEY_WOW64_32KEY: delex_read_only,
    // delex_read_32key). Both view flags fail (delex_both).
    capsule::RegView view = capsule::RegView::Default;
    if (reserved != 0 || !wow64_view_from_sam(sam_desired, view)) {
        return ERROR_INVALID_PARAMETER;
    }
    return reg_delete_key(hkey, subkey, view);
}

u32 RegistryThunks::reg_delete_tree(u64 hkey, const std::wstring& subkey) {
    u64 target = hkey;
    bool opened = false;
    if (!subkey.empty()) {
        u64 child = 0;
        const u32 err = reg_open_key_ex_w(hkey, subkey, 0, 0, &child);
        if (err != ERROR_SUCCESS) return err;
        target = child;
        opened = true;
    } else if (!is_valid_key(hkey)) {
        return ERROR_INVALID_HANDLE;
    }

    for (;;) {
        std::wstring child_name;
        const u32 en = reg_enum_key_ex_w(target, 0, child_name);
        if (en == ERROR_NO_MORE_ITEMS) break;
        if (en != ERROR_SUCCESS) {
            if (opened) (void)reg_close_key(target);
            return en;
        }
        const u32 del = reg_delete_tree(target, child_name);
        if (del != ERROR_SUCCESS) {
            if (opened) (void)reg_close_key(target);
            return del;
        }
    }

    std::vector<std::wstring> value_names;
    for (u32 index = 0;; ++index) {
        std::wstring value_name;
        const u32 en = reg_enum_value_w(target, index, value_name, nullptr,
                                        nullptr, nullptr);
        if (en == ERROR_NO_MORE_ITEMS) break;
        if (en != ERROR_SUCCESS) {
            if (opened) (void)reg_close_key(target);
            return en;
        }
        value_names.push_back(std::move(value_name));
    }
    for (const auto& value_name : value_names) {
        const u32 del = reg_delete_value_w(target, value_name);
        if (del != ERROR_SUCCESS) {
            if (opened) (void)reg_close_key(target);
            return del;
        }
    }

    if (opened) {
        (void)reg_close_key(target);
        return reg_delete_key(hkey, subkey);
    }
    return ERROR_SUCCESS;
}

bool RegistryThunks::is_valid_key(u64 hkey) const {
    return resolve_hkey(hkey) != 0;
}

u32 RegistryThunks::reg_notify_change_key_value(
    u64 hkey, bool watch_subtree, u32 notify_filter, Handle event,
    bool asynchronous) {
    constexpr u32 REG_NOTIFY_CHANGE_MASK = 0x0000000F;
    constexpr u32 REG_NOTIFY_THREAD_AGNOSTIC = 0x10000000;
    const u32 supported_flags =
        REG_NOTIFY_CHANGE_MASK | REG_NOTIFY_THREAD_AGNOSTIC;
    if ((notify_filter & REG_NOTIFY_CHANGE_MASK) == 0 ||
        (notify_filter & ~supported_flags) != 0) {
        return ERROR_INVALID_PARAMETER;
    }

    const capsule::RegHandle key = resolve_hkey(hkey);
    const auto key_path = m_registry.canonical_key_path(key);
    if (!key_path.has_value()) return ERROR_INVALID_HANDLE;

    if (asynchronous &&
        (event == NULL_HANDLE || m_sync_manager == nullptr ||
         m_handles.get_type(event) != HandleType::Event)) {
        return ERROR_INVALID_HANDLE;
    }

    auto synchronous_wait = asynchronous
        ? std::shared_ptr<SynchronousRegistryWait>{}
        : std::make_shared<SynchronousRegistryWait>();
    {
        std::lock_guard<std::mutex> lock(m_watch_mutex);
        m_watches.push_back(RegistryWatch{
            hkey, *key_path, watch_subtree,
            notify_filter & REG_NOTIFY_CHANGE_MASK,
            asynchronous ? event : NULL_HANDLE,
            synchronous_wait});
    }

    VITREO_TRACE(
        "[registry] RegNotifyChangeKeyValue(hkey=0x%llx subtree=%d "
        "filter=0x%x event=0x%x async=%d) -> ERROR_SUCCESS\n",
        static_cast<unsigned long long>(hkey), watch_subtree ? 1 : 0,
        notify_filter, static_cast<unsigned>(event),
        asynchronous ? 1 : 0);

    if (!asynchronous) {
        std::unique_lock<std::mutex> lock(synchronous_wait->mutex);
        synchronous_wait->cv.wait(
            lock, [&] { return synchronous_wait->signaled; });
    }
    return ERROR_SUCCESS;
}

void RegistryThunks::notify_registry_change(
    const std::wstring& changed_key_path, u32 change_filter,
    bool name_change) {
    std::vector<RegistryWatch> triggered;
    {
        std::lock_guard<std::mutex> lock(m_watch_mutex);
        auto watch = m_watches.begin();
        while (watch != m_watches.end()) {
            bool matches_path = false;
            if ((watch->notify_filter & change_filter) != 0) {
                const bool descendant =
                    changed_key_path.size() > watch->key_path.size() &&
                    changed_key_path.compare(
                        0, watch->key_path.size(), watch->key_path) == 0 &&
                    changed_key_path[watch->key_path.size()] == L'\\';
                if (name_change) {
                    const usize separator = changed_key_path.rfind(L'\\');
                    const std::wstring_view parent =
                        separator == std::wstring::npos
                            ? std::wstring_view{}
                            : std::wstring_view(changed_key_path).substr(
                                  0, separator);
                    matches_path =
                        parent == watch->key_path ||
                        (watch->watch_subtree && descendant);
                } else {
                    matches_path =
                        changed_key_path == watch->key_path ||
                        (watch->watch_subtree && descendant);
                }
            }
            if (matches_path) {
                triggered.push_back(std::move(*watch));
                watch = m_watches.erase(watch);
            } else {
                ++watch;
            }
        }
    }

    for (auto& watch : triggered) {
        if (watch.event != NULL_HANDLE && m_sync_manager != nullptr) {
            (void)m_sync_manager->set_event(watch.event);
        }
        if (watch.synchronous_wait) {
            {
                std::lock_guard<std::mutex> lock(
                    watch.synchronous_wait->mutex);
                watch.synchronous_wait->signaled = true;
            }
            watch.synchronous_wait->cv.notify_one();
        }
    }
}

void RegistryThunks::cancel_watches_for_handle(u64 hkey) {
    std::vector<RegistryWatch> cancelled;
    {
        std::lock_guard<std::mutex> lock(m_watch_mutex);
        auto watch = m_watches.begin();
        while (watch != m_watches.end()) {
            if (watch->registration_hkey == hkey) {
                cancelled.push_back(std::move(*watch));
                watch = m_watches.erase(watch);
            } else {
                ++watch;
            }
        }
    }
    for (auto& watch : cancelled) {
        if (watch.event != NULL_HANDLE && m_sync_manager != nullptr) {
            (void)m_sync_manager->set_event(watch.event);
        }
        if (watch.synchronous_wait) {
            {
                std::lock_guard<std::mutex> lock(
                    watch.synchronous_wait->mutex);
                watch.synchronous_wait->signaled = true;
            }
            watch.synchronous_wait->cv.notify_one();
        }
    }
}

// =============================================================================
// reg_type_to_win32
// =============================================================================

u32 RegistryThunks::reg_type_to_win32(const capsule::RegValue& v) noexcept {
    if (v.type == capsule::RegType::Raw) return v.raw_type;
    switch (v.type) {
        case capsule::RegType::None:     return REG_NONE;
        case capsule::RegType::Sz:       return REG_SZ;
        case capsule::RegType::ExpandSz: return REG_EXPAND_SZ;
        case capsule::RegType::Binary:   return REG_BINARY;
        case capsule::RegType::Dword:    return REG_DWORD;
        case capsule::RegType::MultiSz:  return REG_MULTI_SZ;
        case capsule::RegType::Qword:    return REG_QWORD;
        default:                         return REG_NONE;
    }
}


// =============================================================================
// RegOpenKeyExW
// =============================================================================

u32 RegistryThunks::reg_open_key_ex_w(u64 hkey, const std::wstring& subkey,
                                       u32 /*options*/, u32 sam_desired,
                                       u64* result_handle) {
    capsule::RegView view = capsule::RegView::Default;
    if (!wow64_view_from_sam(sam_desired, view)) return ERROR_INVALID_PARAMETER;
    capsule::RegHandle parent = resolve_hkey(hkey);
    auto res = m_registry.open_key(parent, subkey, /*create=*/false, view);

    if (res.is_err()) {
        if (res.error() == Error::NotFound) {
            return ERROR_FILE_NOT_FOUND;
        }
        return ERROR_FILE_NOT_FOUND; // map all others to not-found for now
    }

    capsule::RegHandle rh = res.value();

    // Store in HandleTable so the guest can use it as an HKEY.
    Handle h = m_handles.allocate(
        HandleType::Registry,
        reinterpret_cast<void*>(static_cast<uintptr_t>(rh)));

    if (result_handle) {
        *result_handle = static_cast<u64>(h);
    }

    return ERROR_SUCCESS;
}

// =============================================================================
// RegOpenKeyExA
// =============================================================================

u32 RegistryThunks::reg_open_key_ex_a(u64 hkey, const std::string& subkey,
                                       u32 options, u32 sam_desired,
                                       u64* result_handle) {
    return reg_open_key_ex_w(hkey, ansi_to_wide(subkey), options,
                             sam_desired, result_handle);
}

u32 RegistryThunks::reg_connect_registry(u64 hkey, u64* result_handle) {
    if (result_handle == nullptr) return ERROR_INVALID_PARAMETER;

    const u32 low = static_cast<u32>(hkey);
    const u64 high = hkey >> 32;
    const bool canonical_extension = high == 0 || high == 0xffffffffu;
    if (!canonical_extension ||
        (low != capsule::HKEY_LOCAL_MACHINE && low != capsule::HKEY_USERS)) {
        return ERROR_INVALID_HANDLE;
    }
    return reg_open_key_ex_w(hkey, L"", 0, 0, result_handle);
}

// =============================================================================
// RegCreateKeyExW
// =============================================================================

u32 RegistryThunks::reg_create_key_ex_w(u64 hkey, const std::wstring& subkey,
                                         u64* result_handle, u32* disposition,
                                         u32 sam_desired) {
    capsule::RegView view = capsule::RegView::Default;
    if (!wow64_view_from_sam(sam_desired, view)) return ERROR_INVALID_PARAMETER;
    capsule::RegHandle parent = resolve_hkey(hkey);
    const auto parent_path = m_registry.canonical_key_path(parent);

    bool created = false;
    auto res = m_registry.open_key(parent, subkey, /*create=*/true, view, &created);
    if (res.is_err()) {
        return ERROR_FILE_NOT_FOUND;
    }
    const bool existed = !created;

    capsule::RegHandle rh = res.value();
    Handle h = m_handles.allocate(
        HandleType::Registry,
        reinterpret_cast<void*>(static_cast<uintptr_t>(rh)));

    if (result_handle) {
        *result_handle = static_cast<u64>(h);
    }

    if (disposition) {
        *disposition = existed ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;
    }
    if (!existed && parent_path.has_value()) {
        notify_registry_change(
            append_canonical_subkey(*parent_path + L"\\", subkey),
            0x00000001, true);
    }

    return ERROR_SUCCESS;
}

// =============================================================================
// RegCreateKeyExA — thin wrapper over the wide variant
// =============================================================================

u32 RegistryThunks::reg_create_key_ex_a(u64 hkey, const std::string& subkey,
                                         u64* result_handle, u32* disposition,
                                         u32 sam_desired) {
    return reg_create_key_ex_w(hkey, ansi_to_wide(subkey), result_handle, disposition,
                               sam_desired);
}

// =============================================================================
// RegOpenKeyA — simplified RegOpenKeyExA with fewer params
// =============================================================================

u32 RegistryThunks::reg_open_key_a(u64 hkey, const std::string& subkey,
                                    u64* result_handle) {
    return reg_open_key_ex_a(hkey, subkey, 0, 0, result_handle);
}



// =============================================================================
// RegGetValueW
// =============================================================================

u32 RegistryThunks::query_get_value(
    u64 hkey, const std::wstring& subkey, const std::wstring& value_name,
    u32 flags, capsule::RegValue& value_out, u32& type_out) {
    constexpr u32 RRF_RT_ANY = 0x0000FFFFu;
    constexpr u32 RRF_SUBKEY_WOW6464KEY = 0x00010000u;
    constexpr u32 RRF_SUBKEY_WOW6432KEY = 0x00020000u;
    constexpr u32 RRF_NOEXPAND = 0x10000000u;
    constexpr u32 RRF_ZEROONFAILURE = 0x20000000u;
    constexpr u32 VALID_FLAGS =
        RRF_RT_ANY | RRF_SUBKEY_WOW6464KEY | RRF_SUBKEY_WOW6432KEY |
        RRF_NOEXPAND | RRF_ZEROONFAILURE;

    if ((flags & ~VALID_FLAGS) != 0 ||
        (flags & (RRF_SUBKEY_WOW6464KEY | RRF_SUBKEY_WOW6432KEY)) ==
            (RRF_SUBKEY_WOW6464KEY | RRF_SUBKEY_WOW6432KEY)) {
        return ERROR_INVALID_PARAMETER;
    }

    capsule::RegHandle key = resolve_hkey(hkey);
    if (key == 0) return ERROR_INVALID_HANDLE;

    // RRF_SUBKEY_WOW6464KEY / RRF_SUBKEY_WOW6432KEY open lpSubKey in that
    // view (getvalue_6464 / getvalue_6432).
    const capsule::RegView view =
        (flags & RRF_SUBKEY_WOW6464KEY) != 0 ? capsule::RegView::Native64
        : (flags & RRF_SUBKEY_WOW6432KEY) != 0 ? capsule::RegView::Wow32
                                               : capsule::RegView::Default;
    capsule::RegHandle opened = 0;
    if (!subkey.empty()) {
        auto result = m_registry.open_key(key, subkey, false, view);
        if (result.is_err()) return ERROR_FILE_NOT_FOUND;
        opened = result.value();
        key = opened;
    }

    auto result = m_registry.query_value(key, value_name);
    if (opened != 0) m_registry.close_key(opened);
    if (result.is_err()) return ERROR_FILE_NOT_FOUND;

    value_out = result.value();
    type_out = reg_type_to_win32(value_out);

    const u32 accepted_types = flags & RRF_RT_ANY;
    u32 type_flag = 0;
    switch (type_out) {
        case REG_NONE:      type_flag = 0x00000001u; break;
        case REG_SZ:        type_flag = 0x00000002u; break;
        case REG_EXPAND_SZ: type_flag = 0x00000004u; break;
        case REG_BINARY:    type_flag = 0x00000008u; break;
        case REG_DWORD:     type_flag = 0x00000010u; break;
        case REG_MULTI_SZ:  type_flag = 0x00000020u; break;
        case REG_QWORD:     type_flag = 0x00000040u; break;
        default: break;
    }
    if (accepted_types != 0 && (accepted_types & type_flag) == 0) {
        return ERROR_UNSUPPORTED_TYPE;
    }

    if (value_out.type == capsule::RegType::ExpandSz &&
        (flags & RRF_NOEXPAND) == 0) {
        if (m_environment_manager == nullptr) {
            return ERROR_CALL_NOT_IMPLEMENTED;
        }
        value_out.data = expand_environment(
            std::get<std::wstring>(value_out.data),
            *m_environment_manager);
    }
    return ERROR_SUCCESS;
}

u32 RegistryThunks::reg_get_value_w(u64 hkey, const std::wstring& subkey,
                                     const std::wstring& value_name, u32 flags,
                                     u32* type_out, u8* data_out,
                                     u32* data_size) {
    capsule::RegValue value;
    u32 type = REG_NONE;
    const u32 query_error =
        query_get_value(hkey, subkey, value_name, flags, value, type);
    if (type_out != nullptr) *type_out = type;
    if (query_error != ERROR_SUCCESS) return query_error;

    const u32 required = serialise_value(value, nullptr, 0);
    if (data_out == nullptr) {
        if (data_size != nullptr) *data_size = required;
        return ERROR_SUCCESS;
    }
    if (data_size == nullptr) return ERROR_INVALID_PARAMETER;
    if (*data_size < required) {
        *data_size = required;
        return ERROR_MORE_DATA;
    }

    serialise_value(value, data_out, *data_size);
    *data_size = required;
    return ERROR_SUCCESS;
}

u32 RegistryThunks::reg_get_value_a(u64 hkey, const std::string& subkey,
                                     const std::string& value_name, u32 flags,
                                     u32* type_out, u8* data_out,
                                     u32* data_size) {
    capsule::RegValue value;
    u32 type = REG_NONE;
    const u32 query_error = query_get_value(
        hkey, ansi_to_wide(subkey), ansi_to_wide(value_name),
        flags, value, type);
    if (type_out != nullptr) *type_out = type;
    if (query_error != ERROR_SUCCESS) return query_error;

    const u32 required = serialise_value_ansi(value, nullptr, 0);
    if (data_out == nullptr) {
        if (data_size != nullptr) *data_size = required;
        return ERROR_SUCCESS;
    }
    if (data_size == nullptr) return ERROR_INVALID_PARAMETER;
    if (*data_size < required) {
        *data_size = required;
        return ERROR_MORE_DATA;
    }

    serialise_value_ansi(value, data_out, *data_size);
    *data_size = required;
    return ERROR_SUCCESS;
}


// =============================================================================
// RegSetValueExA — thin wrapper over the wide variant
// =============================================================================

u32 RegistryThunks::reg_set_value_ex_a(u64 hkey, const std::string& value_name,
                                        u32 type, const u8* data,
                                        u32 data_size) {
    if (type == REG_SZ || type == REG_EXPAND_SZ) {
        capsule::RegValue val;
        val.type = type == REG_SZ ? capsule::RegType::Sz
                                  : capsule::RegType::ExpandSz;
        std::wstring text;
        if (data != nullptr) {
            for (u32 i = 0; i < data_size && data[i] != 0; ++i) {
                text.push_back(static_cast<wchar_t>(data[i]));
            }
        }
        val.data = std::move(text);
        const capsule::RegHandle key = resolve_hkey(hkey);
        const auto key_path = m_registry.canonical_key_path(key);
        auto result =
            m_registry.set_value(key, ansi_to_wide(value_name), val);
        if (result.is_ok() && key_path.has_value()) {
            notify_registry_change(*key_path, 0x00000004, false);
        }
        return result.is_ok() ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
    }
    if (type == REG_MULTI_SZ && data != nullptr) {
        // Learn (RegSetValueExA): string types are converted to Unicode, so
        // each ANSI byte (NULs included) becomes one UTF-16 unit.
        std::vector<u8> wide_bytes(static_cast<usize>(data_size) * 2);
        for (u32 i = 0; i < data_size; ++i) {
            wide_bytes[i * 2] = data[i];
            wide_bytes[i * 2 + 1] = 0;
        }
        return reg_set_value_ex_w(hkey, ansi_to_wide(value_name), type,
                                  wide_bytes.data(), data_size * 2);
    }
    return reg_set_value_ex_w(hkey, ansi_to_wide(value_name), type, data, data_size);
}

// =============================================================================
// RegSetKeyValueW
// =============================================================================

u32 RegistryThunks::reg_set_key_value_w(u64 hkey, const std::wstring& subkey,
                                         const std::wstring& value_name,
                                         u32 type, const u8* data,
                                         u32 data_size) {
    if (subkey.empty()) {
        return reg_set_value_ex_w(hkey, value_name, type, data, data_size);
    }

    u64 child_handle = 0;
    u32 err = reg_create_key_ex_w(hkey, subkey, &child_handle, nullptr);
    if (err != ERROR_SUCCESS) {
        return err;
    }

    err = reg_set_value_ex_w(child_handle, value_name, type, data, data_size);
    (void)reg_close_key(child_handle);
    return err;
}

// =============================================================================
// RegDeleteValueW
// =============================================================================

u32 RegistryThunks::reg_delete_value_w(u64 hkey, const std::wstring& value_name) {
    capsule::RegHandle rh = resolve_hkey(hkey);
    const auto key_path = m_registry.canonical_key_path(rh);
    auto res = m_registry.delete_value(rh, value_name);
    if (res.is_ok() && key_path.has_value()) {
        notify_registry_change(*key_path, 0x00000004, false);
    }
    return res.is_ok() ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

// =============================================================================
// RegDeleteKeyValueW
// =============================================================================

u32 RegistryThunks::reg_delete_key_value_w(u64 hkey,
                                            const std::wstring& subkey,
                                            const std::wstring& value_name) {
    if (subkey.empty()) {
        return reg_delete_value_w(hkey, value_name);
    }

    u64 child_handle = 0;
    u32 err = reg_open_key_ex_w(hkey, subkey, 0, 0, &child_handle);
    if (err != ERROR_SUCCESS) {
        return err;
    }

    err = reg_delete_value_w(child_handle, value_name);
    (void)reg_close_key(child_handle);
    return err;
}

// =============================================================================
// RegCloseKey
// =============================================================================

u32 RegistryThunks::reg_close_key(u64 hkey) {
    // Root constants — silently succeed.
    if (hkey == capsule::HKEY_CLASSES_ROOT  ||
        hkey == capsule::HKEY_CURRENT_USER  ||
        hkey == capsule::HKEY_LOCAL_MACHINE ||
        hkey == capsule::HKEY_USERS) {
        return ERROR_SUCCESS;
    }

    // Remove from HandleTable.
    Handle h = static_cast<Handle>(hkey);
    auto* entry = m_handles.get_entry(h);
    if (!entry || entry->type != HandleType::Registry) {
        return ERROR_INVALID_HANDLE;
    }

    capsule::RegHandle rh = static_cast<capsule::RegHandle>(
        reinterpret_cast<uintptr_t>(entry->object));
    cancel_watches_for_handle(hkey);
    m_registry.close_key(rh);
    m_handles.close(h);

    return ERROR_SUCCESS;
}

// =============================================================================
// RegEnumKeyExW
// =============================================================================

u32 RegistryThunks::reg_enum_key_ex_w(u64 hkey, u32 index,
                                       std::wstring& name_out) {
    capsule::RegHandle rh = resolve_hkey(hkey);
    auto res = m_registry.enum_keys(rh);
    if (res.is_err()) {
        return ERROR_FILE_NOT_FOUND;
    }

    const auto& keys = res.value();
    if (index >= static_cast<u32>(keys.size())) {
        return ERROR_NO_MORE_ITEMS;
    }

    name_out = keys[index];
    return ERROR_SUCCESS;
}


// =============================================================================
// RegQueryValueW — legacy API, opens subkey and queries default value
// =============================================================================

u32 RegistryThunks::reg_query_value_w(u64 hkey, const std::wstring& subkey,
                                       u8* data_out, u32* data_size) {
    if (!subkey.empty()) {
        u64 child_handle = 0;
        u32 err = reg_open_key_ex_w(hkey, subkey, 0, 0, &child_handle);
        if (err != ERROR_SUCCESS) return err;
        u32 result = reg_query_value_ex_w(child_handle, L"", nullptr,
                                          data_out, data_size);
        reg_close_key(child_handle);
        return result;
    }
    return reg_query_value_ex_w(hkey, L"", nullptr, data_out, data_size);
}

// =============================================================================
// RegEnumKeyExA — ANSI variant of RegEnumKeyExW
// =============================================================================

u32 RegistryThunks::reg_enum_key_ex_a(u64 hkey, u32 index,
                                       std::string& name_out) {
    std::wstring wide_name;
    u32 err = reg_enum_key_ex_w(hkey, index, wide_name);
    if (err == ERROR_SUCCESS) {
        name_out.clear();
        name_out.reserve(wide_name.size());
        for (wchar_t ch : wide_name) {
            name_out.push_back(static_cast<char>(ch & 0xFF));
        }
    }
    return err;
}


// =============================================================================
// RegOpenKeyW — simplified RegOpenKeyExW wrapper
// =============================================================================

u32 RegistryThunks::reg_open_key_w(u64 hkey, const std::wstring& subkey,
                                    u64* result_handle) {
    return reg_open_key_ex_w(hkey, subkey, 0, 0, result_handle);
}


} // namespace vitreo::winapi
