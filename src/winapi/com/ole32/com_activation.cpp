// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_activation.cpp
/// @brief In-proc COM server activation (InprocServer32 lookup and
/// DllGetClassObject) and the scratch/builtin-object allocation helpers.

#include "com_core.hpp"
#include "ole_guid_internal.hpp"
#include "wic.hpp"
#include "com_core_internal.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/registry/registry.hpp"

#include <optional>
#include <limits>
#include <string>
#include <vector>

namespace vitreo::winapi {

#include "winapi/com/ole32/com_factory_contract.inc"

namespace {
u32 unsupported_class_object(const char* reason) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::lock_guard lock(mutex);
    if (seen.insert(reason).second)
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ole32!CoGetClassObject: %s\n", reason);
    return com::E_NOTIMPL;
}
}


namespace {
std::optional<u32> resolve_inproc_server_path(capsule::Registry* registry,
    const Guid& clsid, std::string& server_utf8) {
    if (registry == nullptr) return std::nullopt;
    const std::string clsid_text = format_guid(clsid);
    const std::wstring clsid_w(clsid_text.begin(), clsid_text.end());
    // HKCR\CLSID in the process's registry view: a 32-bit process reads the
    // WOW64 view (Classes\WOW6432Node\CLSID) only, so a class registered
    // for the other bitness alone is not an in-process server here (Learn,
    // "Registry Keys Affected by WOW64").
    std::wstring server;
    {
        auto key = registry->open_key(
            capsule::HKEY_CLASSES_ROOT,
            L"CLSID\\" + clsid_w + L"\\InprocServer32", false);
        if (key.is_err()) {
            if (key.error() != Error::NotFound)
                return unsupported_class_object("registry read-error HRESULT translation");
            return std::nullopt;
        }
        auto value = registry->query_value(key.value(), L"");
        registry->close_key(key.value());
        if (value.is_err()) {
            if (value.error() != Error::NotFound)
                return unsupported_class_object("registry value read-error HRESULT translation");
            return std::nullopt;
        }
        if (value.value().type == capsule::RegType::ExpandSz)
            return unsupported_class_object("InprocServer32 expandable environment path");
        if (value.value().type != capsule::RegType::Sz ||
            !std::holds_alternative<std::wstring>(value.value().data))
            return unsupported_class_object("InprocServer32 non-string registry value");
        if (value.is_ok() &&
            (value.value().type == capsule::RegType::Sz ||
             value.value().type == capsule::RegType::ExpandSz) &&
            std::holds_alternative<std::wstring>(value.value().data)) {
            server = std::get<std::wstring>(value.value().data);
        }
    }
    if (server.empty()) return std::nullopt;

    server_utf8.clear();
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
    return com::S_OK;
}
}

std::optional<u32> ComCoreManager::get_inproc_class_object(
    Marshaler& m, const Guid& clsid, GuestAddr rclsid_ptr,
    GuestAddr riid_ptr, GuestAddr ppv_ptr) {
    const bool is64 = m.cpu_is_64bit();
    std::string server_utf8;
    const auto resolved = resolve_inproc_server_path(m_registry, clsid, server_utf8);
    if (!resolved || *resolved != com::S_OK) return resolved;
    if (m_modules == nullptr || !m_guest_call) return com::CO_E_DLLNOTFOUND;
    const GuestAddr module = m_modules->load_library(server_utf8);
    if (module == 0) {
        VITREO_TRACE("[ole32] CoCreateInstance: InprocServer32 '%s' not loadable\n",
                     server_utf8.c_str());
        return com::CO_E_DLLNOTFOUND;
    }
    const GuestAddr get_class_object =
        m_modules->get_proc_address(module, "DllGetClassObject");
    if (get_class_object == 0) return com::CO_E_ERRORINDLL;

    const u64 args[3] = {rclsid_ptr, riid_ptr, ppv_ptr};
    return static_cast<u32>(m_guest_call(get_class_object, args, 3, is64));
}

std::optional<u32> ComCoreManager::activate_inproc_server(
    Marshaler& m, const Guid& clsid, GuestAddr rclsid_ptr, GuestAddr outer,
    GuestAddr riid_ptr, GuestAddr ppv_ptr) {
    // Resolve absence/invalid registration before allocating factory scratch,
    // preserving CoCreateInstance's class-not-registered failure precedence.
    std::string server;
    const auto resolved = resolve_inproc_server_path(m_registry, clsid, server);
    if (!resolved || *resolved != com::S_OK) return resolved;
    if (m_modules == nullptr || !m_guest_call) return com::CO_E_DLLNOTFOUND;
    static constexpr Guid kIClassFactory = com_factory_contract::iid_class_factory;
    const GuestAddr scratch = co_task_mem_alloc_guest(32);
    if (!scratch) return 0x8007000Eu;
    (void)m.write_struct<Guid>(scratch, kIClassFactory);
    (void)m.write_ptr(scratch + 16, 0);
    const auto result = get_inproc_class_object(m, clsid, rclsid_ptr, scratch, scratch + 16);
    const GuestAddr factory = m.read_ptr(scratch + 16).value_or(0);
    co_task_mem_free_guest(scratch);
    if (!result) return std::nullopt;
    u32 hr = *result;
    if ((hr & 0x80000000u) || !factory)
        return (hr & 0x80000000u) ? hr : com::CLASS_E_CLASSNOTAVAILABLE;
    const bool is64 = m.cpu_is_64bit();
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

    return hr;
}

u32 ComCoreManager::co_get_class_object(Marshaler& m) {
    const GuestAddr clsid_ptr = m.arg_ptr(0), server = m.arg_ptr(2);
    const GuestAddr iid_ptr = m.arg_ptr(3), out = m.arg_ptr(4);
    const u32 context = m.arg32(1);
    if (!out) return com::E_INVALIDARG;
    if (m.write_ptr(out, 0).is_err()) return com::E_INVALIDARG;
    auto clsid = m.read_struct<Guid>(clsid_ptr), iid = m.read_struct<Guid>(iid_ptr);
    if (!clsid_ptr || !iid_ptr || clsid.is_err() || iid.is_err()) return com::E_INVALIDARG;
    if (!thread_in_apartment(m)) return CO_E_NOTINITIALIZED;
    const auto unsupported = unsupported_class_object;
    // COSERVERINFO has DWORD reserved fields surrounding two guest pointers.
    // A zero structure with INPROC is accepted by Windows on both ABIs.
    if (server) {
        const auto& offsets = m.cpu_is_64bit() ? com_factory_contract::server_offsets64 : com_factory_contract::server_offsets32;
        const u32 name_offset = offsets[1], auth_offset = offsets[2], tail_offset = offsets[3];
        auto reserved = m.read_u32(server);
        auto name = m.read_ptr(server + name_offset);
        auto auth = m.read_ptr(server + auth_offset);
        auto tail = m.read_u32(server + tail_offset);
        if (reserved.is_err() || name.is_err() || auth.is_err() || tail.is_err()) return com::E_INVALIDARG;
        if (reserved.value() || tail.value()) return com::E_INVALIDARG;
        if (name.value() || auth.value()) return unsupported("explicit server or authentication");
    }
    // Location bits are independent of modifiers. Activation modifiers require
    // their real security/architecture semantics, so never silently discard one.
    using namespace com_factory_contract;
    for (const u32 pair : {CLSCTX_NO_CODE_DOWNLOAD | CLSCTX_ENABLE_CODE_DOWNLOAD,
                         CLSCTX_DISABLE_AAA | CLSCTX_ENABLE_AAA,
                         CLSCTX_ACTIVATE_32_BIT_SERVER | CLSCTX_ACTIVATE_64_BIT_SERVER})
        if ((context & pair) == pair) return com::E_INVALIDARG;
    if (context & ~location_mask) return unsupported("activation modifier / handler mode");
    GuestAddr registered = 0;
    std::shared_ptr<ClassObjectBorrow> borrowed;
    {
        std::lock_guard lock(m_class_objects_mutex);
        for (const auto& [cookie, entry] : m_class_objects) {
            (void)cookie;
            if (guid_equal(entry.clsid, clsid.value()) && !entry.suspended &&
                (entry.context & context)) {
                const GuestAddr current = m.has_cpu() ? static_cast<GuestAddr>(
                    m.cpu_is_64bit() ? m.cpu()->seg.gs_desc.base : m.cpu()->seg.fs_desc.base) : 0;
                if (entry.apartment != current)
                    return unsupported("cross-apartment registered class-object marshaling");
                if ((entry.flags & (REGCLS_MULTIPLEUSE | REGCLS_MULTI_SEPARATE)) == 0)
                    return unsupported("REGCLS_SINGLEUSE class-object consumption");
                registered = entry.class_object;
                borrowed = entry.borrow;
                break;
            }
        }
    }
    if (registered) {
        const u32 hr = guest_iunknown_qi(m, registered, iid_ptr, out);
        bool release = false;
        {
            std::lock_guard lock(m_class_objects_mutex);
            release = borrowed.use_count() == 1 && borrowed->release_pending;
            borrowed.reset();
        }
        if (release) guest_iunknown_release(m, registered);
        return hr;
    }
    if (context & CLSCTX_INPROC_SERVER) {
        if (m_external_class_object_provider) {
            if (auto hr = m_external_class_object_provider(m, clsid.value(), iid.value(), out)) return *hr;
        }
        if (wic_is_imaging_factory_clsid(clsid.value()))
            return wic_get_class_object(*this, m, clsid.value(), iid.value(), out);
        if (has_builtin_class(clsid.value())) return unsupported("native class has no class-object provider");
        if (auto hr = get_inproc_class_object(m, clsid.value(), clsid_ptr, iid_ptr, out)) return *hr;
    }
    if ((context & CLSCTX_INPROC_HANDLER) && m_registry) {
        const std::string text = format_guid(clsid.value());
        const std::wstring path = L"CLSID\\" + std::wstring(text.begin(), text.end()) + L"\\InprocHandler32";
        auto key = m_registry->open_key(capsule::HKEY_CLASSES_ROOT, path, false);
        if (key.is_ok()) {
            m_registry->close_key(key.value());
            return unsupported("registered InprocHandler32 activation");
        }
        if (key.error() != Error::NotFound) return unsupported("registry handler read-error HRESULT translation");
    }
    if (context & (CLSCTX_LOCAL_SERVER | CLSCTX_REMOTE_SERVER)) return unsupported("local-server launch / remote class-object proxy");
    return com::REGDB_E_CLASSNOTREG;
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
