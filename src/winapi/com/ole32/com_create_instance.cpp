// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_create_instance.cpp
/// @brief CoCreateInstance: class-id dispatch to the built-in COM classes, the
/// registry-driven in-proc and out-of-process activation paths.

#include "shell_item_dialogs.hpp"
#include "com_core.hpp"
#include "filter_graph.hpp"
#include "wic.hpp"
#include "ole_guid_internal.hpp"
#include "com_wasapi_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/com/ole32/task_memory.hpp"
#include "capsule/registry/registry.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "ipc/ipc_gate.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/common/elevation.hpp"
#include "winapi/media/dinput/dinput.hpp"
#include "com_core_internal.hpp"

#include <array>
#include <chrono>
#include <optional>
#include <cstring>
#include <string>
#include <thread>

namespace vitreo::winapi {

namespace {

DirectInputManager* g_direct_input_manager = nullptr;

constexpr Guid CLSID_GLOBALOPTIONS{0x0000034Bu, 0x0000u, 0x0000u, {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
constexpr Guid CLSID_FILTERGRAPH{0xE436EBB3u, 0x524Fu, 0x11CEu, {0x9F,0x53,0x00,0x20,0xAF,0x0B,0xA7,0x70}};
constexpr Guid CLSID_FILTERGRAPH_NOTHREAD{
    0xE436EBB8u, 0x524Fu, 0x11CEu,
    {0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
// Microsoft DirectInput CLSID_DirectInput8 (dinput.h).
constexpr Guid CLSID_DIRECTINPUT8{
    0x25E609E4u, 0xB259u, 0x11CFu,
    {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
// Microsoft Learn / netlistmgr.h
constexpr Guid CLSID_NETWORKLISTMANAGER{
    0xDCB00C01u, 0x570Fu, 0x4A9Bu, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};

} // namespace

u32 ComCoreManager::co_create_instance(Marshaler& m) {
    const GuestAddr rclsid_ptr = m.arg_ptr(0);
    const GuestAddr outer = m.arg_ptr(1);
    const u32 context = static_cast<u32>(m.arg(2));
    const GuestAddr riid_ptr = m.arg_ptr(3);
    const GuestAddr ppv_ptr = m.arg_ptr(4);
    if (rclsid_ptr == 0 || riid_ptr == 0 || ppv_ptr == 0 || m_thunks == nullptr) {
        return com::E_INVALIDARG;
    }

    auto clsid_r = m.read_struct<Guid>(rclsid_ptr);
    auto iid_r = m.read_struct<Guid>(riid_ptr);
    if (clsid_r.is_err() || iid_r.is_err()) {
        return com::E_INVALIDARG;
    }

    const Guid& clsid = clsid_r.value();
    const Guid& iid = iid_r.value();
    (void)m.write_ptr(ppv_ptr, 0);
    ipc::ipc_log(
        "CoCreateInstance clsid=%s iid=%s context=0x%x outer=0x%llx",
        format_guid(clsid).c_str(), format_guid(iid).c_str(), context,
        static_cast<unsigned long long>(outer));
    // Measured (tools/oracle_probe/s7_ole_probe.expected): a thread outside
    // every apartment gets CO_E_NOTINITIALIZED with *ppv NULL.
    if (!thread_in_apartment(m)) {
        return CO_E_NOTINITIALIZED;
    }

    // CoCreateInstance is the CoGetClassObject +
    // IClassFactory::CreateInstance shortcut.  CoRegisterClassObject publishes
    // an EXE's class factory to COM; honor a matching process registration
    // before falling back to Vitreo's built-in in-process classes.
    GuestAddr registered_factory = 0;
    {
        std::lock_guard lock(m_class_objects_mutex);
        for (const auto& [cookie, registration] : m_class_objects) {
            (void)cookie;
            if (guid_equal(registration.clsid, clsid) &&
                !registration.suspended &&
                (registration.context & context) != 0) {
                registered_factory = registration.class_object;
                // Pin the factory while CreateInstance executes so a
                // concurrent CoRevokeClassObject cannot release it first.
                (void)guest_iunknown_addref(m, registered_factory);
                break;
            }
        }
    }
    if (registered_factory != 0) {
        auto vtable = m.read_ptr(registered_factory);
        const GuestAddr pointer_size = m.cpu_is_64bit() ? 8u : 4u;
        if (vtable.is_err() || vtable.value() == 0) {
            guest_iunknown_release(m, registered_factory);
            return com::CLASS_E_CLASSNOTAVAILABLE;
        }
        auto create_instance = m.read_ptr(
            static_cast<GuestAddr>(vtable.value()) + 3u * pointer_size);
        if (create_instance.is_err() || create_instance.value() == 0) {
            guest_iunknown_release(m, registered_factory);
            return com::CLASS_E_CLASSNOTAVAILABLE;
        }

        const u64 args[4] = {
            static_cast<u64>(registered_factory), static_cast<u64>(outer),
            static_cast<u64>(riid_ptr), static_cast<u64>(ppv_ptr)};
        const u32 hr = static_cast<u32>(m_guest_call(
            static_cast<GuestAddr>(create_instance.value()), args, 4,
            m.cpu_is_64bit()));
        guest_iunknown_release(m, registered_factory);
        VITREO_TRACE(
            "[ole32] CoCreateInstance: registered factory=0x%llx "
            "context=0x%x -> 0x%08x\n",
            static_cast<unsigned long long>(registered_factory), context, hr);
        return hr;
    }

    ipc::ComClassId class_id{};
    std::memcpy(class_id.data(), &clsid, sizeof(clsid));
    const auto find_remote_class = [&](ipc::ComClassSnapshot& snapshot) {
        return !m_capsule_root.empty() && ipc::find_com_class(
            m_capsule_root, class_id, context, snapshot);
    };
    const auto remote_class_result = [&](const ipc::ComClassSnapshot& found) {
        if (outer != 0) {
            return com::CLASS_E_NOAGGREGATION;
        }
        std::optional<AutomationInterfaceLayout> automation_layout;
        if (!guid_equal(iid, IID_IUNKNOWN)) {
            if (m_automation_layout_resolver) {
                automation_layout = m_automation_layout_resolver(iid);
            }
            if (!automation_layout) {
                VITREO_TRACE(
                    "[ole32] CoCreateInstance: remote IID %s has no "
                    "registered marshaling layout\n",
                    format_guid(iid).c_str());
                return com::E_NOTIMPL;
            }
        }

        const pid_t owner_pid = static_cast<pid_t>(found.owner_pid);
        const usize pointer_size = found.pointer_size;
        if ((pointer_size != 4 && pointer_size != 8) ||
            found.class_object == 0) {
            return com::CLASS_E_CLASSNOTAVAILABLE;
        }
        if (!ipc::remote_process_endpoint_available(owner_pid)) {
            return com::CLASS_E_CLASSNOTAVAILABLE;
        }

        GuestAddr factory_vtable = 0;
        auto factory_vtable_bytes = std::span<Byte>(
            reinterpret_cast<Byte*>(&factory_vtable), pointer_size);
        if (!ipc::remote_read_process_memory(
                owner_pid, found.class_object, factory_vtable_bytes) ||
            factory_vtable == 0) {
            return com::CLASS_E_CLASSNOTAVAILABLE;
        }
        if (pointer_size == 4) factory_vtable &= 0xFFFFFFFFu;

        GuestAddr create_method = 0;
        auto create_method_bytes = std::span<Byte>(
            reinterpret_cast<Byte*>(&create_method), pointer_size);
        if (!ipc::remote_read_process_memory(
                owner_pid, factory_vtable + 3u * pointer_size,
                create_method_bytes) || create_method == 0) {
            return com::CLASS_E_CLASSNOTAVAILABLE;
        }
        if (pointer_size == 4) create_method &= 0xFFFFFFFFu;

        const GuestAddr scratch = ipc::remote_virtual_alloc(
            owner_pid, 0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (scratch == 0) return E_OUTOFMEMORY;
        struct ScratchGuard {
            pid_t pid;
            GuestAddr address;
            ~ScratchGuard() {
                (void)ipc::remote_virtual_free(pid, address, 0, MEM_RELEASE);
            }
        } scratch_guard{owner_pid, scratch};

        const auto iid_bytes = std::span<const Byte>(
            reinterpret_cast<const Byte*>(&iid), sizeof(iid));
        std::array<Byte, 8> null_output{};
        if (!ipc::remote_write_process_memory(owner_pid, scratch, iid_bytes) ||
            !ipc::remote_write_process_memory(
                owner_pid, scratch + sizeof(Guid),
                std::span<const Byte>(null_output.data(), pointer_size))) {
            return RPC_E_SERVERFAULT;
        }

        const std::array<u64, 4> arguments{
            found.class_object, 0, scratch, scratch + sizeof(Guid)};
        const auto result = ipc::remote_call_guest(
            owner_pid, create_method, arguments,
            m_elevation != nullptr && m_elevation->is_elevated());
        if (!result) return RPC_E_SERVERFAULT;
        const u32 hr = static_cast<u32>(*result);
        if ((hr & 0x80000000u) != 0) return hr;

        GuestAddr remote_object = 0;
        auto object_bytes = std::span<Byte>(
            reinterpret_cast<Byte*>(&remote_object), pointer_size);
        if (!ipc::remote_read_process_memory(
                owner_pid, scratch + sizeof(Guid), object_bytes) ||
            remote_object == 0) {
            return RPC_E_SERVERFAULT;
        }
        if (pointer_size == 4) remote_object &= 0xFFFFFFFFu;

        const GuestAddr proxy = create_remote_iunknown_proxy(
            m, found, remote_object, iid,
            automation_layout ? &*automation_layout : nullptr);
        if (proxy == 0) {
            RemoteInterfaceProxy temporary{
                found.owner_pid, found.pointer_size, remote_object, iid, 1, 0};
            const std::array<u64, 1> release_args{remote_object};
            (void)call_remote_iunknown(temporary, 2, release_args);
            return E_OUTOFMEMORY;
        }
        if (m.write_ptr(ppv_ptr, proxy).is_err()) {
            {
                std::lock_guard lock(m_remote_proxies_mutex);
                m_remote_proxies.erase(proxy);
                std::erase_if(m_remote_methods, [proxy](const auto& entry) {
                    return entry.second.proxy_object == proxy;
                });
            }
            RemoteInterfaceProxy temporary{
                found.owner_pid, found.pointer_size, remote_object, iid, 1, 0};
            const std::array<u64, 1> release_args{remote_object};
            (void)call_remote_iunknown(temporary, 2, release_args);
            m_guest_task_memory->free(proxy);
            return com::E_INVALIDARG;
        }
        VITREO_TRACE(
            "[ole32] CoCreateInstance: remote class owner=%u cookie=%u "
            "factory=0x%llx object=0x%llx proxy=0x%llx\n",
            found.owner_pid, found.cookie,
            static_cast<unsigned long long>(found.class_object),
            static_cast<unsigned long long>(remote_object),
            static_cast<unsigned long long>(proxy));
        return com::S_OK;
    };

    ipc::ComClassSnapshot remote_class{};
    if (find_remote_class(remote_class)) {
        return remote_class_result(remote_class);
    }

    // A local COM class can be hosted by a Windows service. The class's AppID
    // selects LocalService, which takes precedence over LocalServer32. Start
    // the registered service through SCM; its class-table publication is the
    // next activation state, never a fabricated interface pointer.
    bool local_service_registered = false;
    bool activation_started = false;
    bool local_server_registered = false;
    if ((context & 0x4u) != 0 && m_registry != nullptr) {
        const std::string clsid_text = format_guid(clsid);
        const std::wstring clsid_key = L"CLSID\\" +
            std::wstring(clsid_text.begin(), clsid_text.end());
        auto class_key = m_registry->open_key(
            capsule::HKEY_CLASSES_ROOT, clsid_key, false);
        if (class_key.is_ok()) {
            auto appid_value = m_registry->query_value(
                class_key.value(), L"AppID");
            m_registry->close_key(class_key.value());
            if (appid_value.is_ok() &&
                appid_value.value().type == capsule::RegType::Sz &&
                std::holds_alternative<std::wstring>(
                    appid_value.value().data)) {
                const std::wstring& appid =
                    std::get<std::wstring>(appid_value.value().data);
                auto appid_key = m_registry->open_key(
                    capsule::HKEY_CLASSES_ROOT, L"AppID\\" + appid, false);
                if (appid_key.is_ok()) {
                    auto service_value = m_registry->query_value(
                        appid_key.value(), L"LocalService");
                    auto parameters_value = m_registry->query_value(
                        appid_key.value(), L"ServiceParameters");
                    m_registry->close_key(appid_key.value());
                    if (service_value.is_ok() &&
                        service_value.value().type == capsule::RegType::Sz &&
                        std::holds_alternative<std::wstring>(
                            service_value.value().data)) {
                        local_service_registered = true;
                        std::wstring parameters;
                        if (parameters_value.is_ok() &&
                            parameters_value.value().type ==
                                capsule::RegType::Sz &&
                            std::holds_alternative<std::wstring>(
                                parameters_value.value().data)) {
                            parameters = std::get<std::wstring>(
                                parameters_value.value().data);
                        }
                        u32 service_error = 0;
                        const std::wstring& service =
                            std::get<std::wstring>(
                                service_value.value().data);
                        const bool started = m_local_service_activator &&
                            m_local_service_activator(
                                service, parameters, service_error);
                        activation_started = started;
                        VITREO_TRACE(
                            "[ole32] CoCreateInstance: LocalService '%ls' "
                            "start=%d error=%u\n",
                            service.c_str(), started ? 1 : 0, service_error);
                    }
                }
            }
        }
    }

    // LocalService is authoritative when present. COM does not fall through
    // to LocalServer32 merely because starting the registered service failed.
    if (!local_service_registered && (context & 0x4u) != 0 &&
        m_registry != nullptr &&
        m_local_server_activator) {
        const std::string clsid_text = format_guid(clsid);
        const std::wstring server_key = L"CLSID\\" +
            std::wstring(clsid_text.begin(), clsid_text.end()) +
            L"\\LocalServer32";
        auto key = m_registry->open_key(
            capsule::HKEY_CLASSES_ROOT, server_key, false);
        if (key.is_ok()) {
            auto command_value = m_registry->query_value(key.value(), L"");
            auto executable_value = m_registry->query_value(
                key.value(), L"ServerExecutable");
            m_registry->close_key(key.value());
            if (command_value.is_ok() &&
                command_value.value().type == capsule::RegType::Sz &&
                std::holds_alternative<std::wstring>(
                    command_value.value().data)) {
                local_server_registered = true;
                std::wstring command_line = std::get<std::wstring>(
                    command_value.value().data);
                if (!command_line.empty()) command_line.push_back(L' ');
                command_line += L"-Embedding";
                std::wstring executable;
                if (executable_value.is_ok() &&
                    executable_value.value().type == capsule::RegType::Sz &&
                    std::holds_alternative<std::wstring>(
                        executable_value.value().data)) {
                    executable = std::get<std::wstring>(
                        executable_value.value().data);
                }
                u32 launch_error = 0;
                const bool started = m_local_server_activator(
                    executable, command_line, launch_error);
                activation_started = started;
                VITREO_TRACE(
                    "[ole32] CoCreateInstance: LocalServer32 start=%d "
                    "error=%u command='%ls'\n",
                    started ? 1 : 0, launch_error, command_line.c_str());
            }
        }
    }

    if (activation_started) {
        const auto deadline = std::chrono::steady_clock::now() +
            m_local_server_registration_timeout;
        do {
            if (find_remote_class(remote_class)) {
                return remote_class_result(remote_class);
            }
            if (m_local_server_registration_timeout.count() == 0 ||
                std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (true);
        VITREO_TRACE(
            "[ole32] CoCreateInstance: server started but class was not "
            "published before activation timeout\n");
    }
    // A registered local server that could not be started, or that never
    // registered its class object, is CO_E_SERVER_EXEC_FAILURE ("Server
    // execution failed"), not a missing registration.
    constexpr u32 CO_E_SERVER_EXEC_FAILURE = 0x80080005u;
    if (activation_started || local_service_registered ||
        local_server_registered) {
        return CO_E_SERVER_EXEC_FAILURE;
    }

    // Components that own their own CLSIDs (MSXML's DOM) get first refusal
    // before the in-proc server search below.
    if (m_external_class_factory) {
        if (auto handled = m_external_class_factory(m, clsid, iid, ppv_ptr);
            handled.has_value()) {
            return handled.value();
        }
    }

    // WIC ships with every Windows install, so callers do not check this
    // activation: an unclaimed CLSID here reads as a null-pointer crash in
    // whatever thread loads an image through it.
    if (wic_is_imaging_factory_clsid(clsid)) {
        return wic_co_create_instance(*this, m, iid, outer, ppv_ptr);
    }
    if (guid_equal(clsid, CLSID_FILTERGRAPH) ||
        guid_equal(clsid, CLSID_FILTERGRAPH_NOTHREAD)) {
        return filter_graph_co_create_instance(*this, m, iid);
    }
    // quartz's stock objects the graph also builds itself (File Source
    // (Async), DirectSound Renderer, memory allocator, system clock).
    {
        bool claimed = false;
        const u32 dshow_hr = dshow_co_create_instance(m, clsid, iid, outer, ppv_ptr, claimed);
        if (claimed) return dshow_hr;
    }
    if (guid_equal(clsid, CLSID_DIRECTINPUT8)) {
        if (g_direct_input_manager == nullptr) {
            return com::CLASS_E_CLASSNOTAVAILABLE;
        }
        // DirectInput8Create's documented equivalence is CoCreateInstance of
        // this class followed by Initialize. The object implementation accepts
        // both IDirectInput8A/W and emits pointer-width-correct vtables.
        return g_direct_input_manager->direct_input8_create(
            0, 0x0800u, riid_ptr, ppv_ptr, outer, m.cpu_is_64bit());
    }
    if (guid_equal(clsid, CLSID_MMDEVICEENUMERATOR)) {
        if (outer != 0) return com::CLASS_E_NOAGGREGATION;
        if (!guid_equal(iid, IID_IUNKNOWN) &&
            !guid_equal(iid, IID_IMMDEVICEENUMERATOR)) {
            return E_NOINTERFACE;
        }
        const char* names[] = {
            "Vitreo_MMDeviceEnumerator_QueryInterface",
            "Vitreo_MMDeviceEnumerator_AddRef",
            "Vitreo_MMDeviceEnumerator_Release",
            "Vitreo_MMDeviceEnumerator_EnumAudioEndpoints",
            "Vitreo_MMDeviceEnumerator_GetDefaultAudioEndpoint",
            "Vitreo_MMDeviceEnumerator_GetDevice",
            "Vitreo_MMDeviceEnumerator_RegisterEndpointNotificationCallback",
            "Vitreo_MMDeviceEnumerator_UnregisterEndpointNotificationCallback",
        };
        const GuestAddr pointer_size = m.cpu_is_64bit() ? 8u : 4u;
        const GuestAddr object = reserve_com_scratch(
        pointer_size + static_cast<GuestAddr>(std::size(names)) * pointer_size);
        if (object == 0) return E_OUTOFMEMORY;
        const GuestAddr vtable = object + pointer_size;
        if (m.write_ptr(object, vtable).is_err()) return com::E_INVALIDARG;
        for (usize slot = 0; slot < std::size(names); ++slot) {
            auto method = m_thunks->get_thunk_address("ole32.dll", names[slot]);
            if (method.is_err() ||
                m.write_ptr(vtable + slot * pointer_size, method.value()).is_err()) {
                return com::CLASS_E_CLASSNOTAVAILABLE;
            }
        }
        {
            std::lock_guard lock(m_com_mutex);
            m_mmdevice_enumerators.emplace(object, MmDeviceEnumeratorState{});
        }
        if (m.write_ptr(ppv_ptr, object).is_err()) {
            std::lock_guard lock(m_com_mutex);
            m_mmdevice_enumerators.erase(object);
            return com::E_INVALIDARG;
        }
        VITREO_TRACE(
            "[ole32] CoCreateInstance: CLSID_MMDeviceEnumerator -> 0x%llx\n",
            static_cast<unsigned long long>(object));
        return com::S_OK;
    }
    if (guid_equal(clsid, CLSID_GLOBALOPTIONS)) {
        if (!guid_equal(iid, IID_IUNKNOWN) &&
            !guid_equal(iid, IID_IGLOBALOPTIONS)) {
            return E_NOINTERFACE;
        }
        // Pointer-width object + vtable from the locked scratch allocator
        // (the old block wrote 4-byte slots and a 4-byte *ppv, so an x64
        // caller's options->Set() jumped through half a pointer).
        static constexpr std::string_view kMethods[] = {
            "Vitreo_GlobalOptions_QueryInterface",
            "Vitreo_GlobalOptions_AddRef",
            "Vitreo_GlobalOptions_Release",
            "Vitreo_GlobalOptions_Set",
            "Vitreo_GlobalOptions_Query",
        };
        const GuestAddr object = allocate_builtin_object(m, kMethods);
        if (object == 0) return 0x8007000Eu;  // E_OUTOFMEMORY
        if (m.write_ptr(ppv_ptr, object).is_err()) {
            return com::E_INVALIDARG;
        }
        {
            std::lock_guard lock(m_com_mutex);
            m_global_options_refs[object] = 1;
        }
        VITREO_TRACE(
            "[ole32] CoCreateInstance: CLSID_GlobalOptions -> 0x%08x\n",
            static_cast<unsigned>(object));
        return com::S_OK;
    }
    if (guid_equal(clsid, CLSID_NETWORKLISTMANAGER)) {
        if (!guid_equal(iid, IID_IUNKNOWN) &&
            !guid_equal(iid, IID_IDISPATCH) &&
            !guid_equal(iid, IID_INETWORKLISTMANAGER) &&
            !guid_equal(iid, IID_ICONNECTIONPOINTCONTAINER)) {
            return E_NOINTERFACE;
        }
        if (m_thunks == nullptr) return com::CLASS_E_CLASSNOTAVAILABLE;

        auto require_thunk = [&](const char* name) -> GuestAddr {
            auto addr = m_thunks->get_thunk_address("ole32.dll", name);
            return addr.is_ok() ? addr.value() : 0;
        };
        const char* nlm_names[] = {
            "Vitreo_NetworkListManager_QueryInterface",
            "Vitreo_NetworkListManager_AddRef",
            "Vitreo_NetworkListManager_Release",
            "Vitreo_NetworkListManager_GetTypeInfoCount",
            "Vitreo_NetworkListManager_GetTypeInfo",
            "Vitreo_NetworkListManager_GetIDsOfNames",
            "Vitreo_NetworkListManager_Invoke",
            "Vitreo_NetworkListManager_GetNetworks",
            "Vitreo_NetworkListManager_GetNetwork",
            "Vitreo_NetworkListManager_GetNetworkConnections",
            "Vitreo_NetworkListManager_GetNetworkConnection",
            "Vitreo_NetworkListManager_get_IsConnectedToInternet",
            "Vitreo_NetworkListManager_get_IsConnected",
            "Vitreo_NetworkListManager_GetConnectivity",
            "Vitreo_NetworkListManager_SetSimulatedProfileInfo",
            "Vitreo_NetworkListManager_ClearSimulatedProfileInfo",
        };
        const char* cpc_names[] = {
            "Vitreo_NetworkListManager_Cpc_QueryInterface",
            "Vitreo_NetworkListManager_AddRef",
            "Vitreo_NetworkListManager_Release",
            "Vitreo_NetworkListManager_Cpc_EnumConnectionPoints",
            "Vitreo_NetworkListManager_Cpc_FindConnectionPoint",
        };
        const char* cp_names[] = {
            "Vitreo_NetworkListManager_Cp_QueryInterface",
            "Vitreo_NetworkListManager_AddRef",
            "Vitreo_NetworkListManager_Release",
            "Vitreo_NetworkListManager_Cp_GetConnectionInterface",
            "Vitreo_NetworkListManager_Cp_GetConnectionPointContainer",
            "Vitreo_NetworkListManager_Cp_Advise",
            "Vitreo_NetworkListManager_Cp_Unadvise",
            "Vitreo_NetworkListManager_Cp_EnumConnections",
        };

        const GuestAddr pointer_size = m.cpu_is_64bit() ? 8u : 4u;
        auto write_vtable = [&](GuestAddr object, const char* const* names,
                                usize count) -> bool {
            const GuestAddr vtable = object + pointer_size;
            if (m.write_ptr(object, vtable).is_err()) return false;
            for (usize i = 0; i < count; ++i) {
                const GuestAddr fn = require_thunk(names[i]);
                if (fn == 0) return false;
                if (m.write_ptr(vtable + static_cast<GuestAddr>(i) * pointer_size,
                                fn)
                        .is_err()) {
                    return false;
                }
            }
            return true;
        };

        constexpr usize kNlmSlots = sizeof(nlm_names) / sizeof(nlm_names[0]);
        constexpr usize kCpcSlots = sizeof(cpc_names) / sizeof(cpc_names[0]);
        constexpr usize kCpSlots = sizeof(cp_names) / sizeof(cp_names[0]);

        const GuestAddr nlm = reserve_com_scratch(
            pointer_size * (3 + kNlmSlots + kCpcSlots + kCpSlots));
        if (nlm == 0) return E_OUTOFMEMORY;
        const GuestAddr cpc =
            nlm + pointer_size * (1 + static_cast<GuestAddr>(kNlmSlots));
        const GuestAddr cp =
            cpc + pointer_size * (1 + static_cast<GuestAddr>(kCpcSlots));

        if (!write_vtable(nlm, nlm_names, kNlmSlots) ||
            !write_vtable(cpc, cpc_names, kCpcSlots) ||
            !write_vtable(cp, cp_names, kCpSlots)) {
            return com::CLASS_E_CLASSNOTAVAILABLE;
        }

        GuestAddr result = nlm;
        if (guid_equal(iid, IID_ICONNECTIONPOINTCONTAINER)) result = cpc;

        if (m.write_ptr(ppv_ptr, result).is_err()) {
            return com::E_INVALIDARG;
        }

        {
            std::lock_guard lock(m_com_mutex);
            const u32 id = m_next_nlm_id++;
            NetworkListManagerState state{};
            state.refs = 1;
            state.nlm = nlm;
            state.cpc = cpc;
            state.cp = cp;
            m_nlm_states.emplace(id, state);
            m_nlm_iface_to_id.emplace(nlm, id);
            m_nlm_iface_to_id.emplace(cpc, id);
            m_nlm_iface_to_id.emplace(cp, id);
        }
        VITREO_TRACE(
            "[ole32] CoCreateInstance: CLSID_NetworkListManager -> 0x%08x\n",
            static_cast<unsigned>(result));
        return com::S_OK;
    }
    if (bool save = false; ShellItemDialogs::is_dialog_class(clsid, save)) {
        // The Common Item Dialog, presented as NSOpenPanel / NSSavePanel.
        if (outer != 0) return com::CLASS_E_NOAGGREGATION;
        return item_dialogs().create_dialog(m, save, iid, ppv_ptr);
    }
    if (!guid_equal(clsid, CLSID_SHELLLINK)) {
        // A class registered with an in-process server loads that DLL; one
        // with no registration at all is REGDB_E_CLASSNOTREG (measured, and
        // Learn CoCreateInstance) -- the answer a Windows install without the
        // component gives. CLASS_E_CLASSNOTAVAILABLE is a DllGetClassObject
        // answer, not an activation-lookup one.
        if ((context & 0x1u) != 0) {
            if (auto hr = activate_inproc_server(m, clsid, rclsid_ptr, outer,
                                                 riid_ptr, ppv_ptr)) {
                return *hr;
            }
        }
        VITREO_TRACE("[ole32] CoCreateInstance: clsid=%s iid=%s not registered\n",
                     format_guid(clsid).c_str(), format_guid(iid).c_str());
        return com::REGDB_E_CLASSNOTREG;
    }
    if (!guid_equal(iid, IID_IUNKNOWN) &&
        !guid_equal(iid, IID_ISHELLLINKA) &&
        !guid_equal(iid, IID_ISHELLLINKW) &&
        !guid_equal(iid, IID_IPERSISTFILE) &&
        !guid_equal(iid, IID_IPROPERTYSTORE)) {
        return E_NOINTERFACE;
    }
    VITREO_TRACE("[ole32] CoCreateInstance: CLSID_ShellLink requested\n");

    auto qif = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_QueryInterface");
    auto arf = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_AddRef");
    auto rlf = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_Release");
    auto sdp = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_SetDescription");
    auto swd = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_SetWorkingDirectory");
    auto sar = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_SetArguments");
    auto sic = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_SetIconLocation");
    auto ssc = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_SetShowCmd");
    auto spa = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_SetPath");
    auto gpa = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_GetPath");
    auto gil = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_GetIDList");
    auto sil = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_SetIDList");
    auto gds = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_GetDescription");
    auto gwd = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_GetWorkingDirectory");
    auto gar = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_GetArguments");
    auto ghk = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_GetHotkey");
    auto shk = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_SetHotkey");
    auto gsc = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_GetShowCmd");
    auto gic = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_GetIconLocation");
    auto srp = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_SetRelativePath");
    auto rsv = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_Resolve");
    auto pld = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_PersistLoad");
    auto psv = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_PersistSave");
    auto pid = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_PersistIsDirty");
    auto pgc = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_PersistGetClassID");
    auto psc = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_PersistSaveCompleted");
    auto pgf = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_PersistGetCurFile");
    auto ppc = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_PropGetCount");
    auto ppa = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_PropGetAt");
    auto ppg = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_PropGetValue");
    auto pps = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_PropSetValue");
    auto ppm = m_thunks->get_thunk_address("ole32.dll", "Vitreo_ShellLink_PropCommit");
    if (qif.is_err() || arf.is_err() || rlf.is_err() || sdp.is_err() || swd.is_err() ||
        sar.is_err() || sic.is_err() || ssc.is_err() || spa.is_err() || gpa.is_err() ||
        gil.is_err() || sil.is_err() || gds.is_err() || gwd.is_err() || gar.is_err() ||
        ghk.is_err() || shk.is_err() || gsc.is_err() || gic.is_err() || srp.is_err() ||
        rsv.is_err() || pld.is_err() ||
        psv.is_err() || pid.is_err() || pgc.is_err() || psc.is_err() || pgf.is_err() ||
        ppc.is_err() || ppa.is_err() || ppg.is_err() || pps.is_err() || ppm.is_err()) {
        return com::CLASS_E_CLASSNOTAVAILABLE;
    }

    // MSVC COM uses pointer-width vtables. A 32-bit layout on x64 makes
    // `mov rax, [rcx]` / `mov rax, [rax]` splice the object pointer with the
    // truncated QI thunk (the high half of a 64-bit vtable entry was lost).
    const GuestAddr pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    const GuestAddr ansi_obj = reserve_com_scratch((4 + 21 + 21 + 9 + 8) * pointer_size);
    if (ansi_obj == 0) return E_OUTOFMEMORY;
    const GuestAddr ansi_vtbl = ansi_obj + pointer_size;
    const GuestAddr unicode_obj = ansi_vtbl + 21 * pointer_size;
    const GuestAddr unicode_vtbl = unicode_obj + pointer_size;
    const GuestAddr persist_obj = unicode_vtbl + 21 * pointer_size;
    const GuestAddr persist_vtbl = persist_obj + pointer_size;
    const GuestAddr propstore_obj = persist_vtbl + 9 * pointer_size;
    const GuestAddr propstore_vtbl = propstore_obj + pointer_size;
    std::lock_guard shell_lock(m_shell_links_mutex);

    const u32 sid = m_next_shelllink_id++;
    ShellLinkState st{};
    st.ansi_iface = ansi_obj;
    st.unicode_iface = unicode_obj;
    st.persist_iface = persist_obj;
    st.propstore_iface = propstore_obj;
    m_shell_links[sid] = st;
    m_iface_to_shelllink[ansi_obj] = {sid, ShellLinkInterfaceKind::Ansi};
    m_iface_to_shelllink[unicode_obj] = {sid, ShellLinkInterfaceKind::Unicode};
    m_iface_to_shelllink[persist_obj] = {sid, ShellLinkInterfaceKind::PersistFile};
    m_iface_to_shelllink[propstore_obj] = {sid, ShellLinkInterfaceKind::PropertyStore};

    if (m.write_ptr(ansi_obj, ansi_vtbl).is_err() ||
        m.write_ptr(unicode_obj, unicode_vtbl).is_err() ||
        m.write_ptr(persist_obj, persist_vtbl).is_err() ||
        m.write_ptr(propstore_obj, propstore_vtbl).is_err()) {
        return com::E_INVALIDARG;
    }

    const auto write_shell_vtable = [&](GuestAddr vtbl) -> bool {
        const GuestAddr slots[] = {
            qif.value(), arf.value(), rlf.value(),
            gpa.value(), gil.value(), sil.value(), gds.value(), sdp.value(),
            gwd.value(), swd.value(), gar.value(), sar.value(),
            ghk.value(), shk.value(), gsc.value(), ssc.value(),
            gic.value(), sic.value(), srp.value(), rsv.value(), spa.value(),
        };
        for (usize i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i) {
            if (m.write_ptr(vtbl + static_cast<GuestAddr>(i) * pointer_size,
                            slots[i])
                    .is_err()) {
                return false;
            }
        }
        return true;
    };
    if (!write_shell_vtable(ansi_vtbl) || !write_shell_vtable(unicode_vtbl)) {
        return com::E_INVALIDARG;
    }

    // IPersistFile vtable
    const GuestAddr persist_slots[] = {
        qif.value(), arf.value(), rlf.value(), pgc.value(), pid.value(),
        pld.value(), psv.value(), psc.value(), pgf.value(),
    };
    for (usize i = 0; i < sizeof(persist_slots) / sizeof(persist_slots[0]);
         ++i) {
        if (m.write_ptr(persist_vtbl + static_cast<GuestAddr>(i) * pointer_size,
                        persist_slots[i])
                .is_err()) {
            return com::E_INVALIDARG;
        }
    }

    // IPropertyStore vtable
    const GuestAddr propstore_slots[] = {
        qif.value(), arf.value(), rlf.value(), ppc.value(), ppa.value(),
        ppg.value(), pps.value(), ppm.value(),
    };
    for (usize i = 0; i < sizeof(propstore_slots) / sizeof(propstore_slots[0]); ++i) {
        if (m.write_ptr(propstore_vtbl + static_cast<GuestAddr>(i) * pointer_size,
                        propstore_slots[i])
                .is_err()) {
            return com::E_INVALIDARG;
        }
    }

    GuestAddr out_ptr = unicode_obj;
    if (guid_equal(iid, IID_ISHELLLINKA)) {
        out_ptr = ansi_obj;
    } else if (guid_equal(iid, IID_IPERSISTFILE)) {
        out_ptr = persist_obj;
    } else if (guid_equal(iid, IID_IPROPERTYSTORE)) {
        out_ptr = propstore_obj;
    }
    if (m.write_ptr(ppv_ptr, out_ptr).is_err()) {
        return com::E_INVALIDARG;
    }
    ipc::ipc_log(
        "ShellLink CoCreate iid=%s ansi=0x%llx unicode=0x%llx "
        "persist=0x%llx out=0x%llx",
        format_guid(iid).c_str(),
        static_cast<unsigned long long>(ansi_obj),
        static_cast<unsigned long long>(unicode_obj),
        static_cast<unsigned long long>(persist_obj),
        static_cast<unsigned long long>(out_ptr));
    VITREO_TRACE(
        "[ole32] CoCreateInstance: ShellLinkA=0x%llx ShellLinkW=0x%llx "
        "persist=0x%llx out=0x%llx ptr=%u\n",
        static_cast<unsigned long long>(ansi_obj),
        static_cast<unsigned long long>(unicode_obj),
        static_cast<unsigned long long>(persist_obj),
        static_cast<unsigned long long>(out_ptr),
        static_cast<unsigned>(pointer_size));
    return com::S_OK;
}

void ComCoreManager::set_direct_input_manager(
    DirectInputManager* manager) noexcept {
    g_direct_input_manager = manager;
}

} // namespace vitreo::winapi
