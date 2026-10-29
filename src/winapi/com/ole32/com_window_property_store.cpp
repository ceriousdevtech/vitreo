// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_window_property_store.cpp
/// @brief SHGetPropertyStoreForWindow's IPropertyStore (shellapi.h).
///
/// The store is a view: its values live on the window
/// (ipc::hwnd_registry_shell_properties), so two stores for one HWND, and a
/// store opened by another process, see the same values. Learn: SetValue
/// stores immediately and Commit has no effect. Every other result is
/// measured, tools/oracle_probe/window_propstore_probe{32,64}.expected:
///   * NULL hwnd is E_INVALIDARG with *ppv untouched; any other HWND value
///     succeeds, and the store fails its writes when the window is gone
///     (HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE)); NULL ppv is
///     E_POINTER; an IID other than IUnknown/IPropertyStore is E_NOINTERFACE
///     with *ppv = NULL. IUnknown is a distinct interface pointer.
///   * GetCount and GetAt fail with E_FAIL while nothing is stored; GetAt
///     past the end is E_INVALIDARG; neither writes the key on failure.
///   * GetValue of an unset key is S_OK with VT_EMPTY; set values convert to
///     the property's schema type (app_user_model_id.hpp).
///   * SetValue converts the value to text (VT_BOOL true is "-1", a CLSID is
///     its registry form); VT_EMPTY removes the key, moving the last value
///     into its slot; an empty string removes the key and fails with
///     HRESULT_FROM_WIN32(ERROR_INVALID_NAME); a NULL key is
///     HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER).

#include "com_core.hpp"
#include "ole_guid_internal.hpp"
#include "com_core_internal.hpp"
#include "com_wasapi_internal.hpp"
#include "ipc/hwnd_registry.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/system/shell32/app_user_model_id.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u32 kInvalidWindowHandle = 0x80070578u;  // ERROR_INVALID_WINDOW_HANDLE
constexpr u32 kInvalidName = 0x8007007Bu;          // ERROR_INVALID_NAME
constexpr u32 kInsufficientBuffer = 0x8007007Au;   // ERROR_INSUFFICIENT_BUFFER
constexpr u32 kFail = 0x80004005u;                 // E_FAIL
constexpr u32 kUnexpected = 0x8000FFFFu;           // E_UNEXPECTED

constexpr u16 VT_EMPTY = 0, VT_I2 = 2, VT_I4 = 3, VT_R4 = 4, VT_R8 = 5,
              VT_BSTR = 8, VT_BOOL = 11, VT_I1 = 16, VT_UI1 = 17, VT_UI2 = 18,
              VT_UI4 = 19, VT_I8 = 20, VT_UI8 = 21, VT_INT = 22, VT_UINT = 23,
              VT_LPSTR = 30, VT_LPWSTR = 31, VT_CLSID = 72;

constexpr std::string_view kStoreMethods[] = {
    "Vitreo_WindowPropertyStore_QueryInterface",
    "Vitreo_WindowPropertyStore_AddRef",
    "Vitreo_WindowPropertyStore_Release",
    "Vitreo_WindowPropertyStore_GetCount",
    "Vitreo_WindowPropertyStore_GetAt",
    "Vitreo_WindowPropertyStore_GetValue",
    "Vitreo_WindowPropertyStore_SetValue",
    "Vitreo_WindowPropertyStore_Commit",
};

[[nodiscard]] bool read_key(Marshaler& m, GuestAddr at, shell::PropertyFmtid& fmtid,
                            u32& pid) {
    if (at == 0) return false;
    if (m.memory().read_bytes(at, reinterpret_cast<Byte*>(fmtid.data()), fmtid.size())
            .is_err()) {
        return false;
    }
    auto p = m.read_u32(at + 16);
    if (p.is_err()) return false;
    pid = p.value();
    return true;
}

[[nodiscard]] std::u16string read_utf16(Marshaler& m, GuestAddr at) {
    std::u16string text;
    if (at == 0) return text;
    for (usize i = 0; i < 32767; ++i) {
        auto unit = m.read_u16(at + i * 2);
        if (unit.is_err() || unit.value() == 0) break;
        text.push_back(static_cast<char16_t>(unit.value()));
    }
    return text;
}

[[nodiscard]] std::u16string ascii_text(const std::string& text) {
    return std::u16string(text.begin(), text.end());
}

/// The registry form of a GUID, upper case: {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}.
[[nodiscard]] std::u16string guid_text(const Guid& g) {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer),
                  "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g.Data1,
                  g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                  g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return ascii_text(buffer);
}

/// The text a window keeps for a PROPVARIANT; nullopt for a type Vitreo does
/// not convert.
[[nodiscard]] std::optional<std::u16string> value_text(Marshaler& m, GuestAddr pv,
                                                       u16 vt) {
    const GuestAddr data = pv + 8;
    const auto u32_at = [&] { return m.read_u32(data).value_or(0); };
    const auto u64_at = [&] { return m.read_u64(data).value_or(0); };
    const auto pointer_at = [&]() -> GuestAddr {
        return static_cast<GuestAddr>(m.read_ptr(data).value_or(0));
    };
    switch (vt) {
    case VT_LPWSTR:
    case VT_BSTR:
        return read_utf16(m, pointer_at());
    case VT_LPSTR: {
        const GuestAddr at = pointer_at();
        std::vector<u8> bytes;
        for (usize i = 0; at != 0 && i < 65535; ++i) {
            const u8 byte = m.memory().read_u8(at + i).value_or(0);
            if (byte == 0) break;
            bytes.push_back(byte);
        }
        if (bytes.empty()) return std::u16string{};
        std::vector<u16> wide(bytes.size());
        const auto result = codepage::multi_byte_to_wide(
            codepage::ansi_code_page(), 0, bytes.data(), bytes.size(), wide.data(),
            wide.size());
        if (!result.ok()) return std::nullopt;
        return std::u16string(wide.begin(), wide.begin() + result.count);
    }
    case VT_BOOL:
        return ascii_text(static_cast<i16>(u32_at() & 0xFFFFu) != 0 ? "-1" : "0");
    case VT_I1:
        return ascii_text(std::to_string(static_cast<i8>(u32_at() & 0xFFu)));
    case VT_UI1:
        return ascii_text(std::to_string(u32_at() & 0xFFu));
    case VT_I2:
        return ascii_text(std::to_string(static_cast<i16>(u32_at() & 0xFFFFu)));
    case VT_UI2:
        return ascii_text(std::to_string(u32_at() & 0xFFFFu));
    case VT_I4:
    case VT_INT:
        return ascii_text(std::to_string(static_cast<i32>(u32_at())));
    case VT_UI4:
    case VT_UINT:
        return ascii_text(std::to_string(u32_at()));
    case VT_I8:
        return ascii_text(std::to_string(static_cast<i64>(u64_at())));
    case VT_UI8:
        return ascii_text(std::to_string(u64_at()));
    case VT_R4: {
        const u32 bits = u32_at();
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "%.7g", static_cast<double>(f));
        return ascii_text(buffer);
    }
    case VT_R8: {
        const u64 bits = u64_at();
        double d;
        std::memcpy(&d, &bits, sizeof(d));
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "%.15g", d);
        return ascii_text(buffer);
    }
    case VT_CLSID: {
        const GuestAddr at = pointer_at();
        if (at == 0) return std::nullopt;
        auto g = m.read_struct<Guid>(at);
        if (g.is_err()) return std::nullopt;
        return guid_text(g.value());
    }
    default:
        return std::nullopt;
    }
}

} // namespace

u32 ComCoreManager::create_window_property_store(Marshaler& m) {
    // SHSTDAPI SHGetPropertyStoreForWindow(HWND hwnd, REFIID riid, void** ppv)
    const u32 hwnd = static_cast<u32>(m.arg(0));
    const GuestAddr riid_ptr = m.arg_ptr(1);
    const GuestAddr ppv_ptr = m.arg_ptr(2);
    if (hwnd == 0) return com::E_INVALIDARG;
    if (ppv_ptr == 0) return com::E_POINTER;
    std::optional<Guid> iid;
    if (riid_ptr != 0) {
        if (auto read = m.read_struct<Guid>(riid_ptr); read.is_ok()) iid = read.value();
    }
    const bool want_unknown = iid && guid_equal(*iid, IID_IUNKNOWN);
    if (!iid || (!want_unknown && !guid_equal(*iid, IID_IPROPERTYSTORE))) {
        (void)m.write_ptr(ppv_ptr, 0);
        return E_NOINTERFACE;
    }
    const bool is64 = m.cpu_is_64bit();
    const GuestAddr pointer_size = is64 ? 8u : 4u;
    std::lock_guard lock(m_window_stores_mutex);
    GuestAddr& vtable = m_window_store_vtable[is64 ? 1 : 0];
    if (vtable == 0) {
        if (m_thunks == nullptr) return kUnexpected;
        std::vector<GuestAddr> methods;
        for (const auto name : kStoreMethods) {
            auto thunk = m_thunks->get_thunk_address("ole32.dll", name);
            if (thunk.is_err()) return kUnexpected;
            methods.push_back(thunk.value());
        }
        const GuestAddr table =
            reserve_com_scratch(pointer_size * static_cast<GuestAddr>(methods.size()));
        if (table == 0) return E_OUTOFMEMORY;
        for (usize i = 0; i < methods.size(); ++i) {
            if (m.write_ptr(table + static_cast<GuestAddr>(i) * pointer_size, methods[i])
                    .is_err()) {
                return kUnexpected;
            }
        }
        vtable = table;
    }
    // One block per store: the IUnknown identity, then IPropertyStore. Both
    // use the store's vtable, whose first slots are IUnknown's.
    const GuestAddr block = co_task_mem_alloc_guest(2 * pointer_size);
    if (block == 0) return E_OUTOFMEMORY;
    const GuestAddr unknown_iface = block;
    const GuestAddr store_iface = block + pointer_size;
    if (m.write_ptr(unknown_iface, vtable).is_err() ||
        m.write_ptr(store_iface, vtable).is_err()) {
        co_task_mem_free_guest(block);
        return kUnexpected;
    }
    m_window_stores[block] = WindowPropertyStoreState{hwnd, 1, unknown_iface, store_iface};
    m_window_store_ifaces[unknown_iface] = block;
    m_window_store_ifaces[store_iface] = block;
    (void)m.write_ptr(ppv_ptr, want_unknown ? unknown_iface : store_iface);
    VITREO_TRACE("[shell32] SHGetPropertyStoreForWindow hwnd=0x%x -> 0x%llx\n", hwnd,
                 static_cast<unsigned long long>(want_unknown ? unknown_iface : store_iface));
    return com::S_OK;
}

ComCoreManager::WindowPropertyStoreState* ComCoreManager::window_store_locked(
    GuestAddr iface) {
    const auto binding = m_window_store_ifaces.find(iface);
    if (binding == m_window_store_ifaces.end()) return nullptr;
    const auto state = m_window_stores.find(binding->second);
    return state == m_window_stores.end() ? nullptr : &state->second;
}

u32 ComCoreManager::window_propstore_query_interface(Marshaler& m) {
    const GuestAddr ppv_ptr = m.arg_ptr(2);
    const GuestAddr riid_ptr = m.arg_ptr(1);
    if (ppv_ptr == 0) return com::E_POINTER;
    std::lock_guard lock(m_window_stores_mutex);
    WindowPropertyStoreState* store = window_store_locked(m.arg_ptr(0));
    std::optional<Guid> iid;
    if (riid_ptr != 0) {
        if (auto read = m.read_struct<Guid>(riid_ptr); read.is_ok()) iid = read.value();
    }
    if (store == nullptr || !iid) {
        (void)m.write_ptr(ppv_ptr, 0);
        return com::E_INVALIDARG;
    }
    GuestAddr out = 0;
    if (guid_equal(*iid, IID_IUNKNOWN)) {
        out = store->unknown_iface;
    } else if (guid_equal(*iid, IID_IPROPERTYSTORE)) {
        out = store->store_iface;
    } else {
        (void)m.write_ptr(ppv_ptr, 0);
        return E_NOINTERFACE;
    }
    ++store->refs;
    (void)m.write_ptr(ppv_ptr, out);
    return com::S_OK;
}

u32 ComCoreManager::window_propstore_add_ref(Marshaler& m) {
    std::lock_guard lock(m_window_stores_mutex);
    WindowPropertyStoreState* store = window_store_locked(m.arg_ptr(0));
    return store == nullptr ? 0u : ++store->refs;
}

u32 ComCoreManager::window_propstore_release(Marshaler& m) {
    GuestAddr freed = 0;
    u32 refs = 0;
    {
        std::lock_guard lock(m_window_stores_mutex);
        WindowPropertyStoreState* store = window_store_locked(m.arg_ptr(0));
        if (store == nullptr) return 0;
        if (store->refs > 0) --store->refs;
        refs = store->refs;
        if (refs == 0) {
            freed = store->unknown_iface;
            m_window_store_ifaces.erase(store->unknown_iface);
            m_window_store_ifaces.erase(store->store_iface);
            m_window_stores.erase(freed);
        }
    }
    if (freed != 0) co_task_mem_free_guest(freed);
    return refs;
}

u32 ComCoreManager::window_propstore_hwnd(Marshaler& m, u32& hwnd) {
    std::lock_guard lock(m_window_stores_mutex);
    WindowPropertyStoreState* store = window_store_locked(m.arg_ptr(0));
    if (store == nullptr) return com::E_INVALIDARG;
    hwnd = store->hwnd;
    return com::S_OK;
}

u32 ComCoreManager::window_propstore_get_count(Marshaler& m) {
    u32 hwnd = 0;
    if (const u32 hr = window_propstore_hwnd(m, hwnd); hr != com::S_OK) return hr;
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_POINTER;
    const auto values = ipc::hwnd_registry_shell_properties(hwnd);
    (void)m.write_u32(out, static_cast<u32>(values.size()));
    return values.empty() ? kFail : com::S_OK;
}

u32 ComCoreManager::window_propstore_get_at(Marshaler& m) {
    u32 hwnd = 0;
    if (const u32 hr = window_propstore_hwnd(m, hwnd); hr != com::S_OK) return hr;
    const u32 index = static_cast<u32>(m.arg(1));
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) return com::E_POINTER;
    const auto values = ipc::hwnd_registry_shell_properties(hwnd);
    if (values.empty()) return kFail;
    if (index >= values.size()) return com::E_INVALIDARG;
    (void)m.memory().write_bytes(out, reinterpret_cast<const Byte*>(values[index].fmtid.data()),
                                 values[index].fmtid.size());
    (void)m.write_u32(out + 16, values[index].pid);
    return com::S_OK;
}

u32 ComCoreManager::window_propstore_get_value(Marshaler& m) {
    u32 hwnd = 0;
    if (const u32 hr = window_propstore_hwnd(m, hwnd); hr != com::S_OK) return hr;
    shell::PropertyFmtid fmtid{};
    u32 pid = 0;
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0 || !read_key(m, m.arg_ptr(1), fmtid, pid)) return com::E_POINTER;
    const std::vector<u8> zeros(m.cpu_is_64bit() ? 24u : 16u, 0);
    (void)m.memory().write_bytes(out, zeros.data(), zeros.size());
    for (const auto& property : ipc::hwnd_registry_shell_properties(hwnd)) {
        if (property.fmtid != fmtid || property.pid != pid) continue;
        const auto value = shell::canonical_shell_value(fmtid, pid, property.text);
        if (value.kind == shell::CanonicalShellValue::Kind::Boolean) {
            (void)m.write_u16(out, VT_BOOL);
            (void)m.write_u16(out + 8, value.flag ? 0xFFFFu : 0u);
        } else if (value.kind == shell::CanonicalShellValue::Kind::String) {
            const GuestAddr text =
                co_task_mem_alloc_guest((value.text.size() + 1) * sizeof(u16));
            if (text == 0) return E_OUTOFMEMORY;
            for (usize i = 0; i < value.text.size(); ++i) {
                (void)m.write_u16(text + i * 2, static_cast<u16>(value.text[i]));
            }
            (void)m.write_u16(text + value.text.size() * 2, 0);
            (void)m.write_u16(out, VT_LPWSTR);
            (void)m.write_ptr(out + 8, text);
        }
        return value.hr;
    }
    return com::S_OK;
}

u32 ComCoreManager::window_propstore_set_value(Marshaler& m) {
    u32 hwnd = 0;
    if (const u32 hr = window_propstore_hwnd(m, hwnd); hr != com::S_OK) return hr;
    shell::PropertyFmtid fmtid{};
    u32 pid = 0;
    if (!read_key(m, m.arg_ptr(1), fmtid, pid)) return kInsufficientBuffer;
    const GuestAddr pv = m.arg_ptr(2);
    if (pv == 0) return com::E_POINTER;
    if (!m_window_exists || !m_window_exists(hwnd)) return kInvalidWindowHandle;
    const u16 vt = m.read_u16(pv).value_or(VT_EMPTY);
    std::optional<std::u16string> text;
    if (vt != VT_EMPTY) {
        text = value_text(m, pv, vt);
        if (!text) {
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true)) {
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED shell32!SHGetPropertyStoreForWindow "
                             "IPropertyStore::SetValue: PROPVARIANT vt=0x%x\n",
                             vt);
            }
            return com::E_NOTIMPL;
        }
    }
    const bool remove = !text || text->empty();
    const bool ok = ipc::hwnd_registry_update_shell_properties(
        hwnd, [&](std::vector<ipc::WindowShellProperty>& values) {
            const auto found = std::find_if(values.begin(), values.end(), [&](const auto& v) {
                return v.fmtid == fmtid && v.pid == pid;
            });
            if (remove) {
                if (found != values.end()) {
                    if (found != values.end() - 1) *found = std::move(values.back());
                    values.pop_back();
                }
            } else if (found != values.end()) {
                found->text = *text;
            } else {
                values.push_back({fmtid, pid, *text});
            }
        });
    if (!ok) return kFail;
    return text && text->empty() ? kInvalidName : com::S_OK;
}

u32 ComCoreManager::window_propstore_commit(Marshaler& m) {
    u32 hwnd = 0;
    return window_propstore_hwnd(m, hwnd);
}

} // namespace vitreo::winapi
