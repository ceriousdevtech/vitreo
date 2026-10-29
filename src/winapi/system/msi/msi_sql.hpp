#pragma once
/// @file msi_sql.hpp
/// @brief The Windows Installer SQL dialect: lexer, parser, binder and
/// executor over a Package (Learn "SQL Syntax").
///
/// Measured rules (tools/oracle_probe/msi_database_sql_probe.c,
/// msi_database_lex_probe.c, x86 == x64):
///   * Only the space character separates tokens; TAB/CR/LF are errors.
///     Keywords are case-insensitive, table and column names are not.
///   * `'...'` strings (no escapes), `` `...` `` names, bare names, integers
///     with an optional glued '-', 32-bit wrapping; `?` parameters numbered
///     in text order across the whole statement.
///   * Names are bound at MsiDatabaseOpenView: an unknown table is 2228, an
///     unknown/ambiguous column 2235, a constant whose type does not match its
///     column 2232, column=column of different types 2237 (syntax).
///   * Integers compare with = <> < > <= >=, strings and the null literal
///     only with = and <>; streams only with IS [NOT] NULL. A string null is
///     the empty string; an integer null fails every comparison except <>
///     (and = against a null parameter).
///   * ORDER BY sorts integers by value, strings by string-pool index (Learn:
///     "will not alphabetize"), nulls first; no DESC. At most 32 comparisons.
///   * Joins are inner, nested loops; a table whose join column is its
///     leading primary key is iterated inside the other.

#include <vitreo/common.hpp>
#include "winapi/system/msi/msi_package.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi::msi {
struct Record;
}

namespace vitreo::winapi::msi::sql {

/// What MsiGetLastErrorRecord reports for a failed statement: field 1 the
/// internal error number, field 2 the database name (filled by the caller),
/// fields 3-4 these. `fields` = 4 or 3 (some errors carry one argument).
struct Error {
    u32 code = 0;
    std::string a;
    std::string b;
    u32 fields = 4;
};

/// One cell of a result row.
struct Value {
    enum class Kind : u8 { Null, Int, Str, Stream };
    Kind kind = Kind::Null;
    i32 i = 0;
    std::string s;  ///< text, or the stream's name in the package
};

/// One result column: where it comes from and how it is described.
struct OutputColumn {
    std::string name;        ///< "" for a constant (MsiViewGetColumnInfo reports null)
    std::string definition;  ///< Column Definition Format, e.g. "s72", "I2"
    i32 table = -1;          ///< source table index, -1 for constants / parameters
    i32 column = -1;         ///< column index within that table
};

/// One row of a SELECT result: values plus the source row of each table.
struct ResultRow {
    std::vector<Value> values;
    std::vector<usize> rows;
};

/// The statement kinds of the dialect.
enum class Kind : u8 { Select, Insert, Update, Delete, CreateTable, DropTable, AlterAdd, AlterHold, AlterFree };

class Statement {
public:
    virtual ~Statement() = default;

    /// Parse and bind @p sql against @p pkg. Null (with @p err set) means
    /// ERROR_BAD_QUERY_SYNTAX.
    [[nodiscard]] static std::unique_ptr<Statement> prepare(Package& pkg, std::string_view sql, Error& err);

    /// Run the statement. @p params supplies `?` values; @p read_only forbids
    /// persistent changes (temporary rows/columns/tables stay writable, as a
    /// custom action's active database allows). Returns an ERROR_* code.
    virtual u32 execute(Package& pkg, const Record* params, bool read_only, Error& err) = 0;

    [[nodiscard]] Kind kind() const { return m_kind; }
    /// Result columns (SELECT); bound at prepare, refreshed by execute.
    [[nodiscard]] const std::vector<OutputColumn>& columns() const { return m_columns; }
    /// SELECT result rows of the last execute.
    [[nodiscard]] std::vector<ResultRow>& results() { return m_results; }
    /// The tables of the FROM list (or the statement's single table).
    [[nodiscard]] const std::vector<std::string>& tables() const { return m_tables; }

protected:
    Kind m_kind = Kind::Select;
    std::vector<OutputColumn> m_columns;
    std::vector<ResultRow> m_results;
    std::vector<std::string> m_tables;
};

/// The virtual tables (`_Tables`, `_Columns`, `_Streams`, `_Storages`).
[[nodiscard]] bool is_virtual_table(std::string_view name);
/// Materialize a virtual table from @p pkg (rows as string cells; `_Streams`
/// / `_Storages` Data cells name the stream / storage).
[[nodiscard]] Table virtual_table(Package& pkg, std::string_view name);

/// Convert a value to a cell of @p column: integers checked, strings for
/// integer columns parsed. False when the value does not fit the column
/// (*range_error: out of the column's range rather than not a number).
[[nodiscard]] bool value_to_cell(const ColumnDef& column, const Value& v, std::string& cell,
                                 bool* range_error = nullptr);
/// A cell of @p column as a Value.
[[nodiscard]] Value cell_value(const ColumnDef& column, const std::string& cell);

}  // namespace vitreo::winapi::msi::sql
