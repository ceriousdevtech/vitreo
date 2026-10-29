// SPDX-License-Identifier: LGPL-2.1-only

#include "winapi/system/setupapi/setupapi.hpp"

#include "capsule/vfs/vfs.hpp"
#include "winapi/system/setupapi/setupapi_internal.hpp"

#include <algorithm>
#include <iterator>

namespace vitreo::winapi {

using namespace setupapi_internal;

namespace {

/// The CM_* names setupapi.dll itself exports (forwarders to cfgmgr32;
/// setupapi.dll 10.0.26100 export table, identical x86/x64). Other CM_*
/// names — CM_Get_DevNode_PropertyW, CM_MapCrToWin32Err,
/// CM_[Un]Register_Notification, CM_Get_Device_Interface_PropertyW — exist
/// only in cfgmgr32.dll, so GetProcAddress on setupapi must not find them.
constexpr const char* kSetupapiCmForwards[] = {
    "CM_Disable_DevNode", "CM_Enable_DevNode", "CM_Get_Child",
    "CM_Get_DevNode_Registry_PropertyA", "CM_Get_DevNode_Registry_PropertyW",
    "CM_Get_DevNode_Status", "CM_Get_Device_IDA", "CM_Get_Device_IDW",
    "CM_Get_Device_ID_ListA", "CM_Get_Device_ID_ListW",
    "CM_Get_Device_ID_List_SizeA", "CM_Get_Device_ID_List_SizeW",
    "CM_Get_Device_ID_Size", "CM_Get_Device_Interface_ListA",
    "CM_Get_Device_Interface_ListW", "CM_Get_Device_Interface_List_SizeA",
    "CM_Get_Device_Interface_List_SizeW", "CM_Get_Parent", "CM_Get_Sibling",
    "CM_Locate_DevNodeA", "CM_Locate_DevNodeW", "CM_Open_DevNode_Key",
};

} // namespace

DllExports create_setupapi_thunks(
    std::function<std::string()> display_name_provider,
    capsule::Vfs* vfs,
    RegistryThunks* registry,
    const GuestElevationState* elevation,
    const SetupapiGuestServices* services) {
    display_name_provider = default_display_name_provider(std::move(display_name_provider));
    auto state = std::make_shared<SetupapiState>();
    state->display_name_provider = display_name_provider;
    state->vfs = vfs;
    state->registry = registry;
    state->elevation = elevation;
    state->services = services;

    DllExports exports;
    auto add = [&exports, state](const char* name, u16 bytes, void (*fn)(Marshaler&, const StatePtr&)) {
        exports.entries.push_back({name, 0, [state, fn](Marshaler& m) { fn(m, state); }, bytes});
    };
    // x86 stdcall byte counts: SDK setupapi.lib decorations (_Name@N).
    exports.entries.push_back({"SetupDiGetClassDevsA", 0,
        [state](Marshaler& m) { get_class_devs(m, state, false, false); }, 16});
    exports.entries.push_back({"SetupDiGetClassDevsW", 0,
        [state](Marshaler& m) { get_class_devs(m, state, true, false); }, 16});
    exports.entries.push_back({"SetupDiGetClassDevsExA", 0,
        [state](Marshaler& m) { get_class_devs(m, state, false, true); }, 28});
    exports.entries.push_back({"SetupDiGetClassDevsExW", 0,
        [state](Marshaler& m) { get_class_devs(m, state, true, true); }, 28});
    add("SetupDiEnumDeviceInfo", 12, enum_device_info);
    add("SetupDiEnumDeviceInterfaces", 20, enum_device_interfaces);
    exports.entries.push_back({"SetupDiGetDeviceInterfaceDetailA", 0,
        [state](Marshaler& m) { get_device_interface_detail(m, state, false); }, 24});
    exports.entries.push_back({"SetupDiGetDeviceInterfaceDetailW", 0,
        [state](Marshaler& m) { get_device_interface_detail(m, state, true); }, 24});
    add("SetupDiDestroyDeviceInfoList", 4, destroy_device_info_list);
    exports.entries.push_back({"SetupDiCreateDeviceInfoList", 0,
        [state](Marshaler& m) { create_device_info_list(m, state, false, true); }, 8});
    exports.entries.push_back({"SetupDiCreateDeviceInfoListExA", 0,
        [state](Marshaler& m) { create_device_info_list(m, state, true, false); }, 16});
    exports.entries.push_back({"SetupDiCreateDeviceInfoListExW", 0,
        [state](Marshaler& m) { create_device_info_list(m, state, true, true); }, 16});
    exports.entries.push_back({"SetupDiOpenDeviceInfoW", 0,
        [state](Marshaler& m) { open_device_info(m, state, true); }, 20});
    exports.entries.push_back({"SetupDiOpenDeviceInterfaceW", 0,
        [state](Marshaler& m) { open_device_interface(m, state, true); }, 16});
    exports.entries.push_back({"SetupDiGetDeviceRegistryPropertyA", 0,
        [state](Marshaler& m) { get_registry_property(m, state, false); }, 28});
    exports.entries.push_back({"SetupDiGetDeviceRegistryPropertyW", 0,
        [state](Marshaler& m) { get_registry_property(m, state, true); }, 28});
    add("SetupDiGetDevicePropertyW", 32, get_device_property);
    exports.entries.push_back({"SetupDiGetDeviceInstanceIdA", 0,
        [state](Marshaler& m) { get_instance_id(m, state, false); }, 20});
    exports.entries.push_back({"SetupDiGetDeviceInstanceIdW", 0,
        [state](Marshaler& m) { get_instance_id(m, state, true); }, 20});
    add("SetupDiOpenDevRegKey", 24, open_dev_reg_key);
    add("SetupDiOpenClassRegKey", 8, open_class_reg_key);
    add("SetupDiOpenDeviceInterfaceRegKey", 16, open_interface_reg_key);
    add("SetupDiGetDeviceInstallParamsW", 12, get_device_install_params);
    add("SetupDiSetDeviceInstallParamsW", 12, set_device_install_params);
    add("SetupDiSetClassInstallParamsW", 16, set_class_install_params);
    add("SetupDiGetClassInstallParamsW", 20, get_class_install_params);
    add("SetupDiBuildDriverInfoList", 12, build_driver_info_list);
    exports.entries.push_back({"SetupDiEnumDriverInfoA", 0,
        [state](Marshaler& m) { enum_driver_info(m, state, false); }, 20});
    exports.entries.push_back({"SetupDiEnumDriverInfoW", 0,
        [state](Marshaler& m) { enum_driver_info(m, state, true); }, 20});
    add("SetupDiDestroyDriverInfoList", 12, destroy_driver_info_list);
    add("SetupDiGetDriverInfoDetailW", 24, get_driver_info_detail);
    add("SetupDiCreateDeviceInfoW", 28, create_device_info);
    add("SetupDiSetDeviceRegistryPropertyW", 20, set_registry_property);
    add("SetupDiSetDeviceRegistryPropertyA", 20, set_registry_property_a);
    add("SetupDiCallClassInstaller", 12, call_class_installer);
    add("SetupCopyOEMInfW", 32, copy_oem_inf);
    add("SetupCopyOEMInfA", 32, copy_oem_inf_a);
    add("SetupGetInfDriverStoreLocationW", 24, inf_driver_store_location);
    // Device setup classes by name and back: the installed-class table
    // (setup_classes.inc) and every rule in setup_class_probe.expected.
    exports.entries.push_back({"SetupDiClassGuidsFromNameA", 0,
        [](Marshaler& m) { class_guids_from_name(m, false, false); }, 16});
    exports.entries.push_back({"SetupDiClassGuidsFromNameW", 0,
        [](Marshaler& m) { class_guids_from_name(m, true, false); }, 16});
    exports.entries.push_back({"SetupDiClassGuidsFromNameExA", 0,
        [](Marshaler& m) { class_guids_from_name(m, false, true); }, 24});
    exports.entries.push_back({"SetupDiClassGuidsFromNameExW", 0,
        [](Marshaler& m) { class_guids_from_name(m, true, true); }, 24});
    exports.entries.push_back({"SetupDiClassNameFromGuidA", 0,
        [](Marshaler& m) { class_name_from_guid(m, false, false); }, 16});
    exports.entries.push_back({"SetupDiClassNameFromGuidW", 0,
        [](Marshaler& m) { class_name_from_guid(m, true, false); }, 16});
    exports.entries.push_back({"SetupDiClassNameFromGuidExA", 0,
        [](Marshaler& m) { class_name_from_guid(m, false, true); }, 24});
    exports.entries.push_back({"SetupDiClassNameFromGuidExW", 0,
        [](Marshaler& m) { class_name_from_guid(m, true, true); }, 24});
    const auto decompress_or_copy = [state](bool wide) {
        return [state, wide](Marshaler& m) {
            // DWORD SetupDecompressOrCopyFile(PCSTR/PCWSTR, PCSTR/PCWSTR, PUINT)
            // https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdecompressorcopyfilea
            const auto source = wide ? m.read_wide_string(m.arg_ptr(0))
                                     : m.read_ansi_string(m.arg_ptr(0));
            const auto target = wide ? m.read_wide_string(m.arg_ptr(1))
                                     : m.read_ansi_string(m.arg_ptr(1));
            const GuestAddr type_ptr = m.arg_ptr(2);
            if (state->vfs == nullptr || source.is_err() || target.is_err()) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                return;
            }
            u32 compression = 0;
            const u32* type = nullptr;
            if (type_ptr != 0) {
                const auto read = m.read_u32(type_ptr);
                if (read.is_err()) {
                    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(error_codes::ERROR_INVALID_PARAMETER);
                    return;
                }
                compression = read.value();
                type = &compression;
            }
            const u32 result = setup_decompress_or_copy_file(
                *state->vfs, source.value(), target.value(), type);
            set_last_error(result);
            m.set_return(result);
        };
    };
    exports.entries.push_back(
        {"SetupDecompressOrCopyFileA", 0, decompress_or_copy(false), 12});
    exports.entries.push_back(
        {"SetupDecompressOrCopyFileW", 0, decompress_or_copy(true), 12});

    const auto read_optional_path = [](Marshaler& m, GuestAddr ptr, bool wide)
        -> std::optional<std::string> {
        if (ptr == 0) return std::string{};
        auto value = wide ? m.read_wide_string(ptr) : m.read_ansi_string(ptr);
        if (value.is_err()) return std::nullopt;
        return value.value();
    };

    const auto install_file = [state, read_optional_path](bool wide, bool is_ex) {
        return [state, read_optional_path, wide, is_ex](Marshaler& m) {
            // BOOL SetupInstallFile(HINF, PINFCONTEXT, PCSTR/PCWSTR,
            //     PCSTR/PCWSTR, PCSTR/PCWSTR, DWORD, PSP_FILE_CALLBACK, PVOID)
            // https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupinstallfilea
            (void)m.arg_ptr(0); // InfHandle
            const GuestAddr inf_context = m.arg_ptr(1);
            const auto source_file =
                read_optional_path(m, m.arg_ptr(2), wide);
            const auto source_root =
                read_optional_path(m, m.arg_ptr(3), wide);
            const auto destination =
                read_optional_path(m, m.arg_ptr(4), wide);
            const u32 copy_style = static_cast<u32>(m.arg(5));
            (void)m.arg_ptr(6); // CopyMsgHandler
            (void)m.arg_ptr(7); // Context
            const GuestAddr was_in_use = is_ex ? m.arg_ptr(8) : 0;
            if (was_in_use != 0) {
                (void)m.write_u32(was_in_use, 0);
            }
            if (state->vfs == nullptr || !source_file || !source_root ||
                !destination) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            if (inf_context != 0) {
                set_last_error(error_codes::ERROR_NOT_SUPPORTED);
                m.set_return(0);
                return;
            }
            const u32 result = setup_install_file(
                *state->vfs, *source_file, *source_root, *destination,
                copy_style);
            set_last_error(result);
            m.set_return(result == error_codes::ERROR_SUCCESS ? 1 : 0);
        };
    };
    exports.entries.push_back(
        {"SetupInstallFileA", 0, install_file(false, false), 32});
    exports.entries.push_back(
        {"SetupInstallFileW", 0, install_file(true, false), 32});
    exports.entries.push_back(
        {"SetupInstallFileExA", 0, install_file(false, true), 36});
    exports.entries.push_back(
        {"SetupInstallFileExW", 0, install_file(true, true), 36});

    exports.entries.push_back({"SetupOpenFileQueue", 0,
        [state](Marshaler& m) {
            // HSPFILEQ SetupOpenFileQueue(VOID)
            // https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupopenfilequeue
            const u32 handle = state->next_file_queue++;
            state->file_queues.emplace(handle, std::vector<QueuedCopy>{});
            set_last_error(error_codes::ERROR_SUCCESS);
            m.set_return(handle);
        }, 0});
    exports.entries.push_back({"SetupCloseFileQueue", 0,
        [state](Marshaler& m) {
            // BOOL SetupCloseFileQueue(HSPFILEQ)
            const u32 handle = static_cast<u32>(m.arg(0));
            if (state->file_queues.erase(handle) == 0) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            m.set_return(1);
        }, 4});

    const auto queue_copy = [state, read_optional_path](bool wide) {
        return [state, read_optional_path, wide](Marshaler& m) {
            // BOOL SetupQueueCopy(HSPFILEQ, PCSTR, PCSTR, PCSTR, PCSTR,
            //     PCSTR, PCSTR, PCSTR, DWORD)
            // https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupqueuecopya
            const u32 handle = static_cast<u32>(m.arg(0));
            const auto source_root =
                read_optional_path(m, m.arg_ptr(1), wide);
            const auto source_path =
                read_optional_path(m, m.arg_ptr(2), wide);
            const auto source_filename =
                read_optional_path(m, m.arg_ptr(3), wide);
            (void)read_optional_path(m, m.arg_ptr(4), wide); // description
            (void)read_optional_path(m, m.arg_ptr(5), wide); // tagfile
            const auto target_directory =
                read_optional_path(m, m.arg_ptr(6), wide);
            const auto target_filename =
                read_optional_path(m, m.arg_ptr(7), wide);
            const u32 copy_style = static_cast<u32>(m.arg(8));
            auto queue = state->file_queues.find(handle);
            if (queue == state->file_queues.end()) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            if (!source_root || !source_path || !source_filename ||
                !target_directory || !target_filename) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            if (source_filename->empty() || target_directory->empty()) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            const std::string source_path_root =
                join_win_path(*source_root, *source_path);
            const std::string destination = target_filename->empty()
                ? *target_directory
                : join_win_path(*target_directory, *target_filename);
            queue->second.push_back(QueuedCopy{
                *source_filename, source_path_root, destination, copy_style});
            set_last_error(error_codes::ERROR_SUCCESS);
            m.set_return(1);
        };
    };
    exports.entries.push_back(
        {"SetupQueueCopyA", 0, queue_copy(false), 36});
    exports.entries.push_back(
        {"SetupQueueCopyW", 0, queue_copy(true), 36});
    exports.entries.push_back({"SetupCommitFileQueue", 0,
        [state](Marshaler& m) {
            // BOOL SetupCommitFileQueue(HWND, HSPFILEQ, PSP_FILE_CALLBACK,
            //     PVOID)
            // https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupcommitfilequeue
            (void)m.arg_ptr(0); // Owner
            const u32 handle = static_cast<u32>(m.arg(1));
            (void)m.arg_ptr(2); // MsgHandler
            (void)m.arg_ptr(3); // Context
            auto queue = state->file_queues.find(handle);
            if (queue == state->file_queues.end() || state->vfs == nullptr) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            for (const auto& copy : queue->second) {
                const u32 result = setup_install_file(
                    *state->vfs, copy.source_file, copy.source_path_root,
                    copy.destination_name, copy.copy_style);
                if (result != error_codes::ERROR_SUCCESS) {
                    set_last_error(result);
                    m.set_return(0);
                    return;
                }
            }
            set_last_error(error_codes::ERROR_SUCCESS);
            m.set_return(1);
        }, 16});

    add_inf_exports(exports, state);

    // setupapi.dll re-exports part of the cfgmgr32 CM_* surface; callers
    // GetProcAddress those names from the SetupAPI handle they just loaded.
    auto cfgmgr = create_cfgmgr32_thunks(display_name_provider, registry, elevation);
    for (auto& entry : cfgmgr.entries) {
        const bool forwarded = std::any_of(
            std::begin(kSetupapiCmForwards), std::end(kSetupapiCmForwards),
            [&entry](const char* name) { return entry.name == name; });
        if (forwarded) exports.entries.push_back(std::move(entry));
    }
    return exports;
}

} // namespace vitreo::winapi
