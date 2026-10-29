// SPDX-License-Identifier: LGPL-2.1-only

#include "winapi/system/setupapi/setupapi.hpp"

#include "winapi/system/setupapi/cfgmgr32_internal.hpp"

#include <unordered_set>

namespace vitreo::winapi {

using namespace setupapi_internal;

DllExports create_cfgmgr32_thunks(
    std::function<std::string()> display_name_provider,
    RegistryThunks* registry,
    const GuestElevationState* elevation) {
    auto state = std::make_shared<CfgmgrState>();
    state->display_name_provider = default_display_name_provider(std::move(display_name_provider));
    state->registry = registry;
    state->elevation = elevation;

    DllExports exports;
    // x86 stdcall byte counts: cfgmgr32.lib decorations (_Name@N).
    exports.entries.push_back({"CM_Locate_DevNodeA", 0,
        [state](Marshaler& m) { cm_locate_devnode(m, state, false); }, 12});
    exports.entries.push_back({"CM_Locate_DevNodeW", 0,
        [state](Marshaler& m) { cm_locate_devnode(m, state, true); }, 12});
    exports.entries.push_back({"CM_Get_Device_IDA", 0,
        [](Marshaler& m) { cm_get_device_id(m, false); }, 16});
    exports.entries.push_back({"CM_Get_Device_IDW", 0,
        [](Marshaler& m) { cm_get_device_id(m, true); }, 16});
    exports.entries.push_back({"CM_Get_Device_ID_Size", 0,
        [](Marshaler& m) { cm_get_device_id_size(m); }, 12});
    exports.entries.push_back({"CM_Get_Parent", 0,
        [state](Marshaler& m) { cm_get_relative(m, state, Relation::Parent); }, 12});
    exports.entries.push_back({"CM_Get_Child", 0,
        [state](Marshaler& m) { cm_get_relative(m, state, Relation::Child); }, 12});
    exports.entries.push_back({"CM_Get_Sibling", 0,
        [state](Marshaler& m) { cm_get_relative(m, state, Relation::Sibling); }, 12});
    exports.entries.push_back({"CM_Get_DevNode_Status", 0,
        [state](Marshaler& m) { cm_get_devnode_status(m, state); }, 16});
    exports.entries.push_back({"CM_Get_DevNode_Registry_PropertyA", 0,
        [state](Marshaler& m) { cm_get_registry_property(m, state, false); }, 24});
    exports.entries.push_back({"CM_Get_DevNode_Registry_PropertyW", 0,
        [state](Marshaler& m) { cm_get_registry_property(m, state, true); }, 24});
    exports.entries.push_back({"CM_Get_DevNode_PropertyW", 0,
        [state](Marshaler& m) { cm_get_devnode_property(m, state); }, 24});
    exports.entries.push_back({"CM_Open_DevNode_Key", 0,
        [state](Marshaler& m) { cm_open_devnode_key(m, state); }, 24});
    exports.entries.push_back({"CM_Get_Device_ID_List_SizeA", 0,
        [state](Marshaler& m) { cm_get_device_id_list_size(m, state, false); }, 12});
    exports.entries.push_back({"CM_Get_Device_ID_List_SizeW", 0,
        [state](Marshaler& m) { cm_get_device_id_list_size(m, state, true); }, 12});
    exports.entries.push_back({"CM_Get_Device_ID_ListA", 0,
        [state](Marshaler& m) { cm_get_device_id_list(m, state, false); }, 16});
    exports.entries.push_back({"CM_Get_Device_ID_ListW", 0,
        [state](Marshaler& m) { cm_get_device_id_list(m, state, true); }, 16});
    exports.entries.push_back({"CM_Get_Device_Interface_List_SizeA", 0,
        [state](Marshaler& m) { cm_get_interface_list_size(m, state, false); }, 16});
    exports.entries.push_back({"CM_Get_Device_Interface_List_SizeW", 0,
        [state](Marshaler& m) { cm_get_interface_list_size(m, state, true); }, 16});
    exports.entries.push_back({"CM_Get_Device_Interface_ListA", 0,
        [state](Marshaler& m) { cm_get_interface_list(m, state, false); }, 20});
    exports.entries.push_back({"CM_Get_Device_Interface_ListW", 0,
        [state](Marshaler& m) { cm_get_interface_list(m, state, true); }, 20});
    exports.entries.push_back({"CM_Disable_DevNode", 0,
        [state](Marshaler& m) { cm_change_devnode(m, state, true); }, 8});
    exports.entries.push_back({"CM_Enable_DevNode", 0,
        [state](Marshaler& m) { cm_change_devnode(m, state, false); }, 8});


    // DWORD CM_MapCrToWin32Err(CONFIGRET CmReturnCode, DWORD DefaultErr).
    // The whole table, measured (tools/oracle_probe/cm_map_cr_probe.expected,
    // x86 == x64): every CONFIGRET not listed returns DefaultErr, and the last
    // error is never touched.
    exports.entries.push_back({"CM_MapCrToWin32Err", 0, [](Marshaler& m) {
        struct Row { u32 cr; u32 win32; };
        static constexpr Row kMap[] = {
            {0x00, 0x000}, {0x02, 0x008}, {0x03, 0x6F8}, {0x04, 0x3EC},
            {0x05, 0x00D}, {0x0D, 0x490}, {0x10, 0x0B7}, {0x1A, 0x07A},
            {0x1C, 0x006}, {0x1D, 0x3F7}, {0x1E, 0x00D}, {0x25, 0x490},
            {0x2E, 0x002}, {0x2F, 0x00D}, {0x30, 0x426}, {0x31, 0x426},
            {0x32, 0x426}, {0x33, 0x005}, {0x34, 0x078}, {0x35, 0x00D},
            {0x37, 0x490}, {0x38, 0x00D},
        };
        const u32 cr = m.arg32(0);
        u32 result = m.arg32(1);
        for (const Row& row : kMap) {
            if (row.cr == cr) { result = row.win32; break; }
        }
        m.set_return(result);
    }, 8});


    struct NotifyState {
        u32 next = 1;
        std::unordered_set<u32> handles;
    };
    auto notify = std::make_shared<NotifyState>();

    // https://learn.microsoft.com/en-us/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_register_notification
    // Vitreo has no PnP event source; registration still returns a live
    // HCMNOTIFICATION so delay-load binds and the caller can unregister.
    exports.entries.push_back({"CM_Register_Notification", 0,
        [notify](Marshaler& m) {
            const GuestAddr filter = m.arg_ptr(0);
            (void)m.arg_ptr(1); // pContext
            const GuestAddr callback = m.arg_ptr(2);
            const GuestAddr out = m.arg_ptr(3);
            if (filter == 0 || callback == 0 || out == 0) {
                m.set_return(CR_INVALID_POINTER);
                return;
            }
            const auto cb_size = m.read_u32(filter);
            if (cb_size.is_err() || cb_size.value() < 16) {
                m.set_return(CR_INVALID_DATA);
                return;
            }
            const u32 handle = notify->next++;
            notify->handles.insert(handle);
            if (m.write_ptr(out, handle).is_err()) {
                notify->handles.erase(handle);
                m.set_return(CR_FAILURE);
                return;
            }
            m.set_return(CR_SUCCESS);
        }, 16});
    exports.entries.push_back({"CM_Unregister_Notification", 0,
        [notify](Marshaler& m) {
            const u32 handle = static_cast<u32>(m.arg(0));
            if (notify->handles.erase(handle) == 0) {
                m.set_return(CR_INVALID_DATA);
                return;
            }
            m.set_return(CR_SUCCESS);
        }, 4});

    // Interface properties are not modelled; no interface has any.
    exports.entries.push_back({"CM_Get_Device_Interface_PropertyW", 0,
        [](Marshaler& m) {
            for (usize i = 0; i < 6; ++i) (void)m.arg(i);
            m.set_return(CR_NO_SUCH_VALUE);
        }, 24});

    return exports;
}

} // namespace vitreo::winapi
