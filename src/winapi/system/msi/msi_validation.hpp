#pragma once
/// @file msi_validation.hpp
/// @brief Database-area internals: `_Validation` checking for MsiViewModify's
/// validate modes (msi_validation.cpp) and the transform / merge / import /
/// export exports (msi_transform.cpp).

#include <vitreo/common.hpp>
#include "winapi/system/msi/msi_core.hpp"
#include "winapi/system/msi/msi_package.hpp"

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi { class MsiManager; class Marshaler; }

namespace vitreo::winapi::msi {

enum class ValidateMode : u8 {
    Existing,  ///< MSIMODIFY_VALIDATE: a fetched row
    New,       ///< MSIMODIFY_VALIDATE_NEW: also checks for a duplicate key
    Field,     ///< MSIMODIFY_VALIDATE_FIELD: only the fields @p present marks
    Delete,    ///< MSIMODIFY_VALIDATE_DELETE: rows that refer to this one
};

/// Validate @p cells (one per column of @p table) against the database's
/// `_Validation` table. Errors are (column, MSIDBERROR) in column order.
/// Returns ERROR_FUNCTION_FAILED without a `_Validation` table,
/// ERROR_INVALID_DATA when errors were found, else ERROR_SUCCESS.
u32 validate_row(Package& pkg, const Table& table, const std::vector<std::string>& cells,
                 const std::vector<bool>& present, ValidateMode mode, std::optional<usize> row,
                 std::vector<std::pair<std::string, i32>>& errors);

/// One category check (exposed for tests): the MSIDBERROR a string value of
/// @p category produces, 0 when valid. @p root_dir is a DefaultDir of a root
/// Directory row.
[[nodiscard]] i32 validate_category(const std::string& category, const std::string& value, bool root_dir);

// ---- shared helpers of the database exports (msi_database.cpp) ----

/// An LPCSTR (CP_ACP) / LPCWSTR argument as UTF-8; nullopt for null.
[[nodiscard]] std::optional<std::string> db_in_str(Marshaler& m, GuestAddr p, bool wide);
/// Whole-file read / write / directory creation through the capsule VFS.
[[nodiscard]] std::optional<std::vector<u8>> db_read_file(MsiManager& mgr, const std::string& path);
[[nodiscard]] bool db_write_file(MsiManager& mgr, const std::string& path, std::span<const u8> bytes);
[[nodiscard]] bool db_make_dir(MsiManager& mgr, const std::string& path);
[[nodiscard]] bool db_is_dir(MsiManager& mgr, const std::string& path);

/// MsiDatabaseApplyTransform / GenerateTransform / CreateTransformSummaryInfo
/// / Merge / Import / Export.
void register_transform_api(MsiManager& mgr, HandlerMap& map);

}  // namespace vitreo::winapi::msi
