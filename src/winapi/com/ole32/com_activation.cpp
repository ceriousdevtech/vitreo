// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_activation.cpp
/// @brief In-proc COM server activation (InprocServer32 lookup and
/// DllGetClassObject) and the scratch/builtin-object allocation helpers.

#include "com_core.hpp"
#include "ole_guid_internal.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/registry/registry.hpp"

#include <optional>
#include <limits>
#include <string>
#include <vector>

namespace vitreo::winapi {

std::optional<u32> ComCoreManager::activate_inproc_server(
    Marshaler& m, const Guid& clsid, GuestAddr rclsid_ptr, GuestAddr outer,
    GuestAddr riid_ptr, GuestAddr ppv_ptr) {
    if (m_registry == nullptr) return std::nullopt;
    const bool is64 = m.cpu_is_64bit();
    const std::string clsid_text = format_guid(clsid);
    const std::wstring clsid_w(clsid_text.begin(), clsid_text.end());
    // HKCR\CLSID in the process's registry view: a 32-bit process reads the
    // WOW64 view (Classes\WOW6432Node\CLSID) only, so a class registered
    // for the other bitness alone is not an in-process server here (Learn,
    // "Registry Keys Affected by WOW64").
    std::wstring server;
    {
        auto key = m_registry->open_key(
            capsule::HKEY_CLASSES_ROOT,
            L"CLSID\\" + clsid_w + L"\\InprocServer32", false);
        if (key.is_err()) return std::nullopt;
        auto value = m_registry->query_value(key.value(), L"");
        m_registry->close_key(key.value());
        if (value.is_ok() &&
            (value.value().type == capsule::RegType::Sz ||
             value.value().type == capsule::RegType::ExpandSz) &&
            std::holds_alternative<std::wstring>(value.value().data)) {
            server = std::get<std::wstring>(value.value().data);
        }
    }
    if (server.empty()) return std::nullopt;
    if (m_modules == nullptr || !m_guest_call) return com::CO_E_DLLNOTFOUND;

    std::string server_utf8;
    for (const wchar_t ch : server) {
        const u32 c = static_cast<u32>(ch);
        if (c < 0x80) server_utf8.push_back(static_cast<char>(c));
        else if (c < 0x800) {
            server_utf8.push_back(static_cast<char>(0xC0 | (c >> 6)));
            server_utf8.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            server_utf8.push_back(static_cast<char>(0xE0 | (c >> 12)));
            server_utf8.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            server_utf8.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    const GuestAddr module = m_modules->load_library(server_utf8);
    if (module == 0) {
        VITREO_TRACE("[ole32] CoCreateInstance: InprocServer32 '%s' not loadable\n",
                     server_utf8.c_str());
        return com::CO_E_DLLNOTFOUND;
    }
    const GuestAddr get_class_object =
        m_modules->get_proc_address(module, "DllGetClassObject");
    if (get_class_object == 0) return com::CO_E_ERRORINDLL;

    // IID_IClassFactory and the factory out-slot live in guest task memory
    // for the duration of the two guest calls.
    static constexpr Guid kIClassFactory{
        0x00000001, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
    const GuestAddr scratch = co_task_mem_alloc_guest(32);
    if (scratch == 0) return 0x8007000Eu;  // E_OUTOFMEMORY
    (void)m.write_struct<Guid>(scratch, kIClassFactory);
    (void)m.write_ptr(scratch + 16, 0);
    const u64 gco_args[3] = {rclsid_ptr, scratch, scratch + 16};
    u32 hr = static_cast<u32>(m_guest_call(get_class_object, gco_args, 3, is64));
    const GuestAddr factory =
        static_cast<GuestAddr>(m.read_ptr(scratch + 16).value_or(0));
    co_task_mem_free_guest(scratch);
    if ((hr & 0x80000000u) != 0 || factory == 0) {
        return (hr & 0x80000000u) != 0 ? hr : com::CLASS_E_CLASSNOTAVAILABLE;
    }
    const u32 ptr = is64 ? 8u : 4u;
    const GuestAddr vtable = static_cast<GuestAddr>(m.read_ptr(factory).value_or(0));
    const GuestAddr create_instance = vtable == 0 ? 0
        : static_cast<GuestAddr>(m.read_ptr(vtable + 3u * ptr).value_or(0));
    if (create_instance == 0) {
        guest_iunknown_release(m, factory);
        return com::CLASS_E_CLASSNOTAVAILABLE;
    }
    const u64 ci_args[4] = {factory, outer, riid_ptr, ppv_ptr};
    hr = static_cast<u32>(m_guest_call(create_instance, ci_args, 4, is64));
    guest_iunknown_release(m, factory);
    VITREO_TRACE("[ole32] CoCreateInstance: InprocServer32 '%s' -> 0x%08x\n",
                 server_utf8.c_str(), hr);
    return hr;
}

void ComCoreManager::set_scratch_base(GuestAddr base) noexcept {
    std::lock_guard lock(m_scratch_mutex);
    if (base > std::numeric_limits<GuestAddr>::max() - COM_SCRATCH_SIZE) {
        m_next_com_addr = m_com_scratch_end = 0;
        return;
    }
    m_next_com_addr = base;
    m_com_scratch_end = base + COM_SCRATCH_SIZE;
}

GuestAddr ComCoreManager::reserve_com_scratch(GuestAddr bytes) noexcept {
    std::lock_guard lock(m_scratch_mutex);
    if (bytes == 0 || m_next_com_addr == 0 ||
        m_next_com_addr > m_com_scratch_end ||
        bytes > m_com_scratch_end - m_next_com_addr) {
        VITREO_TRACE("[ole32] COM scratch arena exhausted\n");
        return 0;
    }
    const GuestAddr address = m_next_com_addr;
    m_next_com_addr += bytes;
    return address;
}

GuestAddr ComCoreManager::allocate_builtin_object(
    Marshaler& m, std::span<const std::string_view> method_names) {
    if (m_thunks == nullptr) return 0;
    std::vector<GuestAddr> methods(method_names.size());
    for (usize i = 0; i < method_names.size(); ++i) {
        auto thunk = m_thunks->get_thunk_address("ole32.dll", method_names[i]);
        if (thunk.is_err()) return 0;
        methods[i] = thunk.value();
    }
    const GuestAddr pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const GuestAddr bytes =
        pointer_size * static_cast<GuestAddr>(1 + methods.size());
    const GuestAddr object = reserve_com_scratch((bytes + 15u) & ~GuestAddr{15u});
    if (object == 0) return 0;
    const GuestAddr vtable = object + pointer_size;
    if (m.write_ptr(object, vtable).is_err()) return 0;
    for (usize i = 0; i < methods.size(); ++i) {
        if (m.write_ptr(vtable + static_cast<GuestAddr>(i) * pointer_size,
                        methods[i]).is_err()) {
            return 0;
        }
    }
    return object;
}

} // namespace vitreo::winapi
