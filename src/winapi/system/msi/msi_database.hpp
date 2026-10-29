#pragma once
/// @file msi_database.hpp
/// @brief The database area's MSIHANDLE objects (database, view, summary
/// information) and the record / error-record helpers the other areas use.
///
/// Records are `msi::Record` (msi_core.hpp) in the HandleTable. A record a
/// view fetched carries a row cookie in field 0 (Windows reports a non-null
/// integer there; MsiViewModify identifies the fetched row by it, so Learn
/// warns not to change field 0 before UPDATE/DELETE).

#include <vitreo/common.hpp>
#include "winapi/system/msi/msi_core.hpp"
#include "winapi/system/msi/msi_package.hpp"
#include "winapi/system/msi/msi_sql.hpp"

#include <map>
#include <memory>
#include <span>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi { class MsiManager; }

namespace vitreo::winapi::msi {

/// MSIDBOPEN_* (MsiQuery.h); PATCHFILE is 32/sizeof(TCHAR): 16 for W, 32 for A.
enum : u32 {
    kDbOpenReadOnly = 0, kDbOpenTransact = 1, kDbOpenDirect = 2,
    kDbOpenCreate = 3, kDbOpenCreateDirect = 4,
};

/// The object behind a database MSIHANDLE. Several handles may share one
/// Package (MsiGetActiveDatabase returns the install session's package).
struct DatabaseObject {
    std::shared_ptr<Package> package;
    std::string path;          ///< as opened (error records name it)
    std::string output_path;   ///< where MsiDatabaseCommit writes ("" = path)
    bool read_only = true;     ///< MSIDBSTATE_READ: only temporary data changes
    bool patch = false;        ///< MSIDBOPEN_PATCHFILE
    bool direct = false;       ///< MSIDBOPEN_DIRECT / CREATEDIRECT: written on close
    bool created = false;      ///< MSIDBOPEN_CREATE*: the file goes away unless committed
    bool committed = false;
    bool dirty = false;
    MsiManager* manager = nullptr;  ///< file access for the close-time write / removal
    std::shared_ptr<void> owner;    ///< keeps an install session's engine alive

    DatabaseObject() = default;
    DatabaseObject(const DatabaseObject&) = delete;
    ~DatabaseObject();
};

/// A row a view handed out: its table and the key that identifies it.
struct FetchedRow {
    std::vector<std::vector<std::string>> keys;  ///< per FROM table
    bool deleted = false;
};

/// The object behind a view MSIHANDLE.
struct ViewObject {
    std::shared_ptr<DatabaseObject> db;
    std::unique_ptr<sql::Statement> statement;
    bool executed = false;
    usize next = 0;
    /// MsiViewGetError state: the MSIDBERROR of the last MsiViewModify
    /// validation and the column it names.
    i32 error = 0;
    std::string error_column;
    /// Every validation error of the last validate call (column, MSIDBERROR);
    /// MsiViewGetError returns them one at a time.
    std::vector<std::pair<std::string, i32>> errors;
    std::unordered_map<i32, FetchedRow> fetched;
};

/// One summary-information property value.
struct SummaryProperty {
    u32 vt = 0;          ///< VT_I2 / VT_I4 / VT_LPSTR / VT_FILETIME
    i32 integer = 0;
    u64 filetime = 0;
    std::string text;    ///< UTF-8
};

/// The object behind a summary-information MSIHANDLE.
struct SummaryInfoObject {
    std::shared_ptr<DatabaseObject> db;   ///< null for a path-opened stream
    std::string path;                     ///< the package file for a path open
    u32 update_count = 0;                 ///< max properties that may change
    std::map<u32, SummaryProperty> properties;
    std::vector<u32> changed;             ///< distinct pids set so far
    bool dirty = false;
};

/// Parse / build the `\005SummaryInformation` property set (MS-OLEPS,
/// FMTID_SummaryInformation). Strings are in the PID_CODEPAGE code page.
[[nodiscard]] std::map<u32, SummaryProperty> parse_summary_information(std::span<const u8> stream);
[[nodiscard]] std::vector<u8> build_summary_information(const std::map<u32, SummaryProperty>& properties);

// ---- record helpers (any area) -------------------------------------------------

/// The record behind @p handle, or null.
[[nodiscard]] std::shared_ptr<Record> record_of(MsiManager& mgr, u32 handle);
/// A new record handle (0 when @p count > 65535).
[[nodiscard]] u32 new_record(MsiManager& mgr, u32 count);
/// A record handle for an existing record object.
[[nodiscard]] u32 add_record(MsiManager& mgr, std::shared_ptr<Record> record);
/// Field text: an integer as decimal, a stream as "", null as "".
[[nodiscard]] std::string record_string(const Record& r, u32 field);
/// MsiRecordGetInteger semantics (MSI_NULL_INTEGER for null / non-numeric).
[[nodiscard]] i32 record_integer(const Record& r, u32 field);
void record_set_string(Record& r, u32 field, const std::string& utf8);
void record_set_integer(Record& r, u32 field, i32 value);
[[nodiscard]] bool record_is_null(const Record& r, u32 field);

// ---- MsiGetLastErrorRecord (per process; Learn) ---------------------------------

/// Set the per-process error record: field 1 @p code, then @p args.
void set_last_error_record(u32 code, const std::vector<std::string>& args);
/// A successful call of a function in Learn's list resets it.
void clear_last_error_record();

// ---- database handles ----------------------------------------------------------

/// A database handle over an existing package (MsiGetActiveDatabase, the
/// engine's own MsiOpenPackage); @p owner keeps the package alive.
[[nodiscard]] u32 open_database_handle(MsiManager& mgr, std::shared_ptr<Package> package,
                                       const std::string& path, bool read_only);
/// The database object behind @p handle, or null.
[[nodiscard]] std::shared_ptr<DatabaseObject> database_of(MsiManager& mgr, u32 handle);

/// MsiGetSummaryInformation (the registration area owns the export; this is
/// the implementation it and this area register). Returns an ERROR_* code.
u32 get_summary_information(MsiManager& mgr, u32 database, const std::string* path, u32 update_count,
                            u32& out_handle);

/// MsiDatabaseApplyTransform on a package (the engine applies TRANSFORMS=
/// through this). Returns an ERROR_* code.
u32 apply_transform(Package& target, std::span<const u8> transform_image, i32 error_conditions,
                    std::string* error_table = nullptr);

}  // namespace vitreo::winapi::msi
