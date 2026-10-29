#pragma once
/// @file msi_package.hpp
/// @brief The Windows Installer database: reader, in-memory model with the
/// mutations SQL / MsiViewModify / transforms / custom actions perform, and
/// the writer MsiDatabaseCommit uses.
///
/// An MSI package is an OLE compound file whose streams hold a small
/// relational database. Three things make it unlike an ordinary docfile:
///
///   * **Stream names are encoded.** Table and user-stream names are mangled
///     into a private Unicode range so they stay valid compound-file names,
///     packing two base64-style characters into most code units; table
///     streams carry a leading 0x4840 marker.
///   * **Strings are pooled.** Cells never hold text; they hold an index into
///     a shared pool built from the `_StringData` / `_StringPool` pair. The
///     index is 2 or 3 bytes wide depending on a flag in the pool header.
///   * **Rows are column-major.** A table stream stores every value of the
///     first column, then every value of the second, and so on, rows sorted
///     by primary key (string keys by pool index, not alphabetically).
///
/// The schema lives in `_Tables` / `_Columns`. Column type words (measured,
/// tools/oracle_probe/msi_database_format_probe.c): low byte = width
/// (characters for strings, bytes for integers), 0x0100 valid, 0x0200
/// localizable, 0x0400|0x0800 string, 0x0800 alone binary stream, 0x0400
/// alone short integer, neither long integer, 0x1000 nullable, 0x2000
/// primary key, 0x4000 temporary. Integer cells are biased (0x8000 /
/// 0x80000000) so raw 0 is null; `_Columns.Type` is itself a biased short.
///
/// **The string-row view.** Every reader in the engine sees a table as
/// `Table::rows`, a vector of string cells: an integer as decimal text, a
/// string as UTF-8, null as "" (MSI makes the empty string and null the same
/// value), and a binary stream cell as the name of its stream in this
/// package (`Table.Key1.Key2`) — pass it to stream(). The mutation API
/// below edits those same rows in place, so a custom action's INSERT
/// TEMPORARY or UPDATE is visible to the next `table()` reader. `Table*`
/// pointers stay valid for the Package's lifetime; a dropped table is
/// emptied, not freed.

#include <vitreo/common.hpp>

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi { struct StorageNode; }

namespace vitreo::winapi::msi {

/// Column type word bits (see the file comment for the measurement).
namespace coltype {
inline constexpr u16 kWidthMask = 0x00FF;
inline constexpr u16 kValid = 0x0100;
inline constexpr u16 kLocalizable = 0x0200;
inline constexpr u16 kShort = 0x0400;
inline constexpr u16 kStream = 0x0800;
inline constexpr u16 kString = 0x0C00;
inline constexpr u16 kNullable = 0x1000;
inline constexpr u16 kKey = 0x2000;
inline constexpr u16 kTemporary = 0x4000;
}  // namespace coltype

/// One column of a table, as described by the `_Columns` metadata table.
struct ColumnDef {
    std::string name;
    u16 type = 0;        ///< Raw MSI type word.
    bool is_string = false;  ///< a string-pool column (not a stream)
    u32 width = 0;       ///< Bytes this column occupies per row on disk.

    [[nodiscard]] bool is_stream() const { return (type & coltype::kString) == coltype::kStream; }
    [[nodiscard]] bool is_integer() const { return (type & coltype::kStream) == 0; }
    [[nodiscard]] bool nullable() const { return (type & coltype::kNullable) != 0; }
    [[nodiscard]] bool key() const { return (type & coltype::kKey) != 0; }
    [[nodiscard]] bool localizable() const { return (type & coltype::kLocalizable) != 0; }
    [[nodiscard]] bool temporary() const { return (type & coltype::kTemporary) != 0; }
    /// Declared size: characters for a string (0 = unbounded), bytes for an
    /// integer (2 or 4).
    [[nodiscard]] u32 size() const { return type & coltype::kWidthMask; }
    /// MsiViewGetColumnInfo(MSICOLINFO_TYPES) text: s/l/i/v/g/j/O + size,
    /// upper case when nullable (Learn "Column Definition Format").
    [[nodiscard]] std::string definition() const;
};

/// Build a ColumnDef from its parts (type word computed).
[[nodiscard]] ColumnDef make_column(std::string name, char kind /*'s','i','v'*/, u32 size,
                                    bool nullable, bool key, bool localizable, bool temporary);

/// A decoded table: its columns and its rows, every cell rendered as text.
/// Integer cells are converted to decimal, which is what the record API hands
/// back to a caller that asks for a string.
struct Table {
    std::vector<ColumnDef> columns;
    std::vector<std::vector<std::string>> rows;

    std::string name;
    /// Parallel to rows: 1 for a row inserted TEMPORARY (never persisted).
    std::vector<u8> row_temporary;
    /// SQL HOLD count (ALTER TABLE ... HOLD / FREE).
    u32 hold_count = 0;
    /// Rows are in primary-key order (as Windows writes them): key lookups
    /// binary-search. False for a file whose rows were not, which then uses a
    /// hash index.
    bool sorted = true;

    /// Index of @p name within columns, or nullopt. An exact (case-sensitive,
    /// as MSI SQL resolves names) match wins; otherwise a case-insensitive one,
    /// for engine readers naming standard columns. SQL uses exact_column().
    [[nodiscard]] std::optional<usize> column_index(std::string_view name) const;
    [[nodiscard]] std::optional<usize> exact_column(std::string_view name) const;
    /// Number of leading primary-key columns.
    [[nodiscard]] usize key_count() const;
    /// True when every column is temporary: the table is not persisted.
    [[nodiscard]] bool temporary() const;
};

/// Integer value of a cell, nullopt for null ("").
[[nodiscard]] std::optional<i32> cell_int(const std::string& cell);

/// Windows error codes the mutation API returns (they surface through
/// MsiViewExecute / MsiViewModify).
enum DbStatus : u32 {
    kDbOk = 0,
    kDbFunctionFailed = 1627,   ///< ERROR_FUNCTION_FAILED
    kDbInvalidTable = 1628,     ///< ERROR_INVALID_TABLE
    kDbBadQuery = 1615,         ///< ERROR_BAD_QUERY_SYNTAX
    kDbAccessDenied = 5,        ///< ERROR_ACCESS_DENIED
};

/// Why a mutation failed, for MsiGetLastErrorRecord (Windows internal error
/// numbers, measured by msi_database_sql_probe.c).
struct DbError {
    u32 code = 0;        ///< e.g. 2259 database update failed, 2204 table exists
    std::string arg1;    ///< usually the table or column involved
    std::string arg2;
};

/// A parsed, editable package.
class Package : public std::enable_shared_from_this<Package> {
public:
    Package();
    Package(Package&&) noexcept;
    Package& operator=(Package&&) noexcept;
    Package(const Package&) = delete;
    ~Package();

    /// Parse @p image (the whole .msi file). Returns nullopt when the bytes
    /// are not a compound file or lack the MSI metadata streams.
    [[nodiscard]] static std::optional<Package> open(std::span<const u8> image);
    /// A new, empty database (MSIDBOPEN_CREATE).
    [[nodiscard]] static Package create(u32 codepage = 0);

    /// Decode a table by name, or nullptr when the package has no such table.
    /// The returned pointer stays valid for the lifetime of the Package.
    [[nodiscard]] const Table* table(const std::string& name);
    /// Mutable access (the SQL engine and the mutation helpers).
    [[nodiscard]] Table* find_table(const std::string& name);

    /// Raw bytes of a non-table stream (a `_Streams` row: Binary/Icon data,
    /// embedded cabinets, `\005SummaryInformation`), or nullptr when absent.
    [[nodiscard]] const std::vector<u8>* stream(const std::string& name) const;
    /// Names of the non-table streams, sorted.
    [[nodiscard]] std::vector<std::string> stream_names() const;
    void set_stream(const std::string& name, std::vector<u8> bytes);
    bool remove_stream(const std::string& name);

    /// Sub-storages (`_Storages`: embedded transforms, nested packages), as
    /// compound-file images.
    [[nodiscard]] std::vector<std::string> storage_names() const;
    [[nodiscard]] std::optional<std::vector<u8>> storage_image(const std::string& name) const;
    bool set_storage_image(const std::string& name, std::span<const u8> image);
    bool remove_storage(const std::string& name);

    /// Names of every accessible table, in `_Tables` order.
    [[nodiscard]] const std::vector<std::string>& table_names() const {
        return m_table_names;
    }

    /// Serializes every mutation and SQL statement; recursive so a mutation
    /// helper can call another. Readers on the sequencer thread do not lock.
    [[nodiscard]] std::recursive_mutex& mutex() const { return *m_mutex; }

    [[nodiscard]] u32 codepage() const { return m_codepage; }
    void set_codepage(u32 codepage) { m_codepage = codepage; }
    /// Root storage CLSID (database / transform / patch).
    [[nodiscard]] const std::array<u8, 16>& clsid() const { return m_clsid; }
    void set_clsid(const std::array<u8, 16>& clsid) { m_clsid = clsid; }

    // ---- schema mutations (return a DbStatus; *err says why) ------------

    /// CREATE TABLE. Key columns must lead (callers order them). A table of
    /// only temporary columns exists only while held (Learn "SQL Syntax":
    /// without HOLD it is created inaccessible).
    u32 create_table(const std::string& name, std::vector<ColumnDef> columns, bool hold,
                     DbError* err = nullptr);
    u32 drop_table(const std::string& name, DbError* err = nullptr);
    /// ALTER TABLE ADD. Existing rows get null in the new column.
    u32 add_column(const std::string& table, ColumnDef column, bool hold, DbError* err = nullptr);
    u32 hold_table(const std::string& table, DbError* err = nullptr);
    u32 free_table(const std::string& table, DbError* err = nullptr);

    // ---- row mutations ---------------------------------------------------

    /// Row index of the row whose key cells equal @p key (one cell per key
    /// column, in order), or nullopt.
    [[nodiscard]] std::optional<usize> find_row(const Table& table,
                                                std::span<const std::string> key) const;
    /// Insert @p cells (one per column; stream cells are ignored and
    /// replaced by @p streams). Fails (kDbFunctionFailed) on a duplicate key,
    /// a null in a non-nullable column, or a non-integer in an integer
    /// column. @p index receives the row's position.
    u32 insert_row(Table& table, std::vector<std::string> cells, bool temporary,
                   const std::map<usize, std::vector<u8>>* streams = nullptr,
                   usize* index = nullptr, DbError* err = nullptr);
    /// Replace one non-key cell (a stream column takes @p stream; nullopt
    /// with an empty value clears it).
    u32 update_cell(Table& table, usize row, usize column, std::string value,
                    const std::vector<u8>* stream = nullptr, DbError* err = nullptr);
    u32 delete_row(Table& table, usize row);
    /// Name of the stream that holds row @p row's binary column.
    [[nodiscard]] std::string stream_key(const Table& table, usize row) const;

    /// The pool index of @p s (0 for ""), nullopt when the string is not
    /// pooled. Orders string keys and ORDER BY (Learn: ordering by strings
    /// groups identical strings but does not alphabetize).
    [[nodiscard]] std::optional<u32> string_id(const std::string& s) const;
    /// Pool @p s (lowest free slot, else a new one) and return its index.
    u32 intern(const std::string& s);
    /// Sort key of one cell for ordering: integers by value (null lowest),
    /// strings by pool index.
    [[nodiscard]] i64 order_key(const ColumnDef& column, const std::string& cell) const;

    /// Every persistent table, column, stream and storage as a compound file
    /// image (MsiDatabaseCommit).
    [[nodiscard]] std::vector<u8> serialize();
    /// Transform images also need the raw table-stream encoder.
    [[nodiscard]] std::vector<u8> encode_string_pool_data() const;

    /// A table a transform's row error left unloadable (Windows then fails
    /// every query of it with 2229); table() no longer returns it.
    void mark_unloadable(const std::string& name);
    [[nodiscard]] bool unloadable(const std::string& name) const;

    /// Ensure every table is decoded (before a whole-database operation).
    void load_all_tables();
    /// Bump on every schema change (views re-validate against it).
    [[nodiscard]] u64 schema_generation() const { return m_schema_generation; }

    /// Encode / decode one stream name (exposed for transforms and tests).
    [[nodiscard]] static std::u16string encode_stream_name(const std::string& name, bool table);

private:
    struct TableSlot {
        std::unique_ptr<Table> table;
        std::optional<std::vector<u8>> raw;  ///< undecoded stream bytes
        bool loaded = false;
        bool present = true;                 ///< false once dropped / freed
        bool unloadable = false;
    };

    [[nodiscard]] std::string pooled(u32 index) const;
    [[nodiscard]] bool decode_table(TableSlot& slot);
    [[nodiscard]] TableSlot* slot(const std::string& name);
    void add_table_name(const std::string& name);
    void remove_table_name(const std::string& name);
    [[nodiscard]] int compare_keys(const Table& table, const std::vector<std::string>& a,
                                   const std::vector<std::string>& b) const;
    void touch(Table& table);

    std::unique_ptr<std::recursive_mutex> m_mutex;
    std::map<std::string, TableSlot> m_tables;
    std::map<std::string, std::vector<u8>> m_streams;   ///< non-table streams
    std::map<std::string, std::shared_ptr<StorageNode>> m_storages;
    std::vector<std::string> m_strings;      ///< 1-based; slot 0 is empty.
    std::unordered_map<std::string, u32> m_string_ids;
    std::vector<u32> m_free_slots;           ///< empty pool slots, ascending
    std::vector<std::string> m_table_names;
    /// Per table: key index (joined key cells -> row), rebuilt lazily.
    mutable std::map<const Table*, std::unordered_map<std::string, usize>> m_key_index;
    u32 m_string_ref_size = 2;
    u32 m_codepage = 0;
    std::array<u8, 16> m_clsid{};
    u64 m_schema_generation = 0;
};

/// Decode an MSI-mangled stream name. Exposed for testing because the
/// two-characters-per-code-unit packing is the easiest part to get subtly
/// wrong, and a wrong decode simply makes tables look absent.
[[nodiscard]] std::string decode_stream_name(std::span<const u16> encoded);

/// Root-storage CLSIDs (measured on a Windows-created database; the transform
/// and patch values are the documented MSI file-class ids).
extern const std::array<u8, 16> kClsidDatabase;
extern const std::array<u8, 16> kClsidTransform;
extern const std::array<u8, 16> kClsidPatch;

/// The internal string encoding of a database code page: text <-> bytes.
[[nodiscard]] std::string decode_codepage(std::span<const u8> bytes, u32 codepage);
[[nodiscard]] std::vector<u8> encode_codepage(const std::string& utf8, u32 codepage);

} // namespace vitreo::winapi::msi
