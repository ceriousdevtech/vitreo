// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file cfgmgr32_internal.hpp
/// @brief Private surface shared by the cfgmgr32 parts (cfgmgr32.cpp,
///        cfgmgr32_devnode.cpp, cfgmgr32_lists.cpp).
///
/// The CM_* handlers read the same modelled device tree as SetupAPI, so this
/// builds on setupapi_internal.hpp. Private to src/winapi/system/setupapi/.

#include "winapi/system/setupapi/setupapi_internal.hpp"

namespace vitreo::winapi::setupapi_internal {

// ---- cfgmgr32 ---------------------------------------------------------------------

// CONFIGRET (cfgmgr32.h).
constexpr u32 CR_SUCCESS = 0x00;
constexpr u32 CR_INVALID_POINTER = 0x03;
constexpr u32 CR_INVALID_FLAG = 0x04;
constexpr u32 CR_INVALID_DEVNODE = 0x05;
constexpr u32 CR_NO_SUCH_DEVNODE = 0x0D;
constexpr u32 CR_FAILURE = 0x13;
constexpr u32 CR_BUFFER_SMALL = 0x1A;
constexpr u32 CR_INVALID_DEVICE_ID = 0x1E;
constexpr u32 CR_INVALID_DATA = 0x1F;
constexpr u32 CR_NO_SUCH_VALUE = 0x25;
constexpr u32 CR_NO_SUCH_REGISTRY_KEY = 0x2E;
constexpr u32 CR_ACCESS_DENIED = 0x33;
constexpr u32 CR_INVALID_PROPERTY = 0x35;

struct CfgmgrState {
    std::function<std::string()> display_name_provider;
    RegistryThunks* registry{nullptr};
    const GuestElevationState* elevation{nullptr};
    [[nodiscard]] std::vector<NodeRecord> nodes() const {
        return device_nodes(display_name_provider ? display_name_provider() : std::string{});
    }
    [[nodiscard]] bool elevated() const {
        return elevation != nullptr && elevation->is_elevated();
    }
};
using CfgPtr = std::shared_ptr<CfgmgrState>;

enum class Relation { Parent, Child, Sibling };

// ---- cfgmgr32 handlers registered by create_cfgmgr32_thunks -----------------

// cfgmgr32_devnode.cpp
void cm_locate_devnode(Marshaler& m, const CfgPtr& state, bool wide);
void cm_get_device_id(Marshaler& m, bool wide);
void cm_get_device_id_size(Marshaler& m);
void cm_get_relative(Marshaler& m, const CfgPtr& state, Relation relation);
void cm_get_devnode_status(Marshaler& m, const CfgPtr& state);
void cm_get_registry_property(Marshaler& m, const CfgPtr& state, bool wide);
void cm_get_devnode_property(Marshaler& m, const CfgPtr& state);
void cm_open_devnode_key(Marshaler& m, const CfgPtr& state);
void cm_change_devnode(Marshaler& m, const CfgPtr& state, bool disable);

// cfgmgr32_lists.cpp
void cm_get_device_id_list_size(Marshaler& m, const CfgPtr& state, bool wide);
void cm_get_device_id_list(Marshaler& m, const CfgPtr& state, bool wide);
void cm_get_interface_list_size(Marshaler& m, const CfgPtr& state, bool wide);
void cm_get_interface_list(Marshaler& m, const CfgPtr& state, bool wide);

} // namespace vitreo::winapi::setupapi_internal
