// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file psapi.hpp
/// @brief PSAPI.dll API stubs

#include <vitreo/common.hpp>
#include <functional>
#include <string>
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

class Marshaler;
class ModuleManager;
class Kernel32ExtManager;

class PsapiManager {
public:
    PsapiManager() = default;
    ~PsapiManager() = default;

    PsapiManager(const PsapiManager&) = delete;
    PsapiManager& operator=(const PsapiManager&) = delete;

    /// Attach ModuleManager for module path lookups
    void set_module_manager(ModuleManager* mm) { m_module_mgr = mm; }

    using MappedPath = std::function<std::string(GuestAddr)>;
    void set_mapped_path_resolver(MappedPath resolver) { m_mapped_path = std::move(resolver); }
    void get_mapped_file_name(Marshaler& m, bool wide);

private:
    MappedPath m_mapped_path;
    ModuleManager* m_module_mgr{nullptr};
};

/// Create PSAPI.dll export thunks. psapi.dll's process/module queries are
/// forwarders to kernel32's K32* functions on Windows 7+, so those exports
/// bind to @p k32's single implementation (kernel32/module/psapi_k32.cpp).
DllExports create_psapi_thunks(PsapiManager& mgr, Kernel32ExtManager& k32);

/// GetModuleInformation / K32GetModuleInformation (one implementation):
/// MODULEINFO {lpBaseOfDll, SizeOfImage, EntryPoint} from the loaded module's
/// PE header -- 12 bytes on x86, 24 on x64 (pointer, DWORD + padding, pointer).
[[nodiscard]] u32 psapi_get_module_information(Marshaler& m,
                                               const ModuleManager* modules,
                                               u32 h_process, GuestAddr h_module,
                                               GuestAddr output, u32 cb);

} // namespace vitreo::winapi
