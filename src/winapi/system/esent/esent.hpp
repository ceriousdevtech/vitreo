#pragma once

/// @file esent.hpp
/// @brief Extensible Storage Engine (esent.dll) JET_API stdcall exports.
///
/// JET_API is __stdcall. stdcall_bytes = 4 * (32-bit stack slots). JET_INSTANCE,
/// JET_SESID, JET_API_PTR, and JET_TABLEID are ULONG_PTR (4 bytes on x86).
///
///   JetCreateInstanceW        8   JET_INSTANCE*, JET_PCWSTR
///   JetSetSystemParameterW   20   JET_INSTANCE*, JET_SESID, unsigned long,
///                                 JET_API_PTR, JET_PCWSTR
///   JetInit                   4   JET_INSTANCE*
///   JetTerm                   4   JET_INSTANCE
///   JetBeginSessionW         16   JET_INSTANCE, JET_SESID*, JET_PCWSTR,
///                                 JET_PCWSTR
///   JetAttachDatabase2W      16   JET_SESID, JET_PCWSTR, unsigned long,
///                                 JET_GRBIT
///   JetOpenDatabaseW         20   JET_SESID, JET_PCWSTR, JET_PCWSTR,
///                                 JET_DBID*, JET_GRBIT
///   JetOpenTableW            28   JET_SESID, JET_DBID, JET_PCWSTR, const void*,
///                                 unsigned long, JET_GRBIT, JET_TABLEID*
///   JetGetTableColumnInfoW   24   JET_SESID, JET_TABLEID, JET_PCWSTR, void*,
///                                 unsigned long, unsigned long
///   JetRetrieveColumn        32   JET_SESID, JET_TABLEID, JET_COLUMNID, void*,
///                                 unsigned long, unsigned long*, JET_GRBIT,
///                                 JET_RETINFO*
///   JetMove                  16   JET_SESID, JET_TABLEID, long, JET_GRBIT
///   JetCloseTable             8   JET_SESID, JET_TABLEID
///
/// dll_registry one-liner (no RuntimeContext member):
///   (void)reg.register_dll("esent.dll", winapi::create_esent_thunks());
/// create_esent_thunks() captures a process-lifetime EsentManager. Tests
/// construct EsentManager and call create_esent_thunks(manager).

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace vitreo::winapi {

/// Documented JET_ERR values from
/// https://learn.microsoft.com/en-us/windows/win32/extensible-storage-engine/extensible-storage-engine-error-codes
namespace jet_err {
constexpr i32 Success = 0;
constexpr i32 FeatureNotAvailable = -1001;
constexpr i32 InvalidParameter = -1003;
constexpr i32 NotInitialized = -1029;
constexpr i32 InstanceNameInUse = -1086;
constexpr i32 InvalidSesid = -1104;
constexpr i32 InvalidInstance = -1115;
constexpr i32 TooManyInstances = -1214;
}  // namespace jet_err

/// Documented system-parameter IDs from Esent.h / Learn resource-parameters.
namespace jet_param {
constexpr u32 SystemPath = 0;
constexpr u32 TempPath = 1;
constexpr u32 LogFilePath = 2;
constexpr u32 BaseName = 3;
constexpr u32 EventSource = 4;
constexpr u32 MaxSessions = 5;
constexpr u32 MaxOpenTables = 6;
constexpr u32 MaxCursors = 8;
constexpr u32 MaxVerPages = 9;
constexpr u32 MaxTemporaryTables = 10;
constexpr u32 LogFileSize = 11;
constexpr u32 CircularLog = 17;
constexpr u32 CacheSizeMax = 23;
constexpr u32 StopFlushThreshold = 32;
constexpr u32 Recovery = 34;
constexpr u32 DatabasePageSize = 64;
constexpr u32 CreatePathIfNotExist = 100;
constexpr u32 MaxInstances = 104;
}  // namespace jet_param

class EsentManager {
public:
    EsentManager() = default;
    EsentManager(const EsentManager&) = delete;
    EsentManager& operator=(const EsentManager&) = delete;

    [[nodiscard]] i32 create_instance(u64* instance, std::string name);
    [[nodiscard]] i32 set_system_parameter(
        std::optional<u64> instance, u64 sesid, u32 paramid, u64 lparam,
        std::optional<std::string> sz);
    [[nodiscard]] i32 init(std::optional<u64> instance);
    [[nodiscard]] i32 term(u64 instance);
    [[nodiscard]] i32 begin_session(u64 instance);
    [[nodiscard]] i32 session_api(u64 sesid) const;

    [[nodiscard]] std::optional<u64> global_int_param(u32 paramid) const;

private:
    struct Instance {
        std::string name;
        std::unordered_map<u32, u64> int_params;
        std::unordered_map<u32, std::string> string_params;
    };

    [[nodiscard]] static bool is_string_param(u32 paramid);
    [[nodiscard]] static bool is_integer_param(u32 paramid);
    [[nodiscard]] u32 max_instances() const;

    std::unordered_map<u64, Instance> instances_;
    std::unordered_map<u32, u64> global_int_params_;
    std::unordered_map<u32, std::string> global_string_params_;
    std::unordered_set<std::string> instance_names_;
    u64 next_handle_{1};
};

DllExports create_esent_thunks(EsentManager& manager);
DllExports create_esent_thunks();

}  // namespace vitreo::winapi
