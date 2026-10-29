// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_transform.cpp
/// @brief Transforms (generate / apply / summary), MsiDatabaseMerge, and the
/// .idt archive format (MsiDatabaseImport / MsiDatabaseExport).
///
/// Contract: Learn "Database Transforms", "MsiDatabaseGenerateTransform",
/// "MsiDatabaseApplyTransform", "MsiCreateTransformSummaryInfo",
/// "Summary Information Stream Property Set" (transform PIDs),
/// "_TransformView Table", "MsiDatabaseMerge", "Archive File Format",
/// "MsiDatabaseImport", "MsiDatabaseExport"; the binary layouts and error
/// numbers measured by tools/oracle_probe/msi_database_transform*_probe.c and
/// msi_database_idt*_probe.c (x86 == x64):
///   * A transform is a compound file (root CLSID {000C1082-...}) with its own
///     string pool and one stream per changed table, rows stored row-major:
///     a 16-bit mask, then the key cells, then the cells the mask names.
///     Mask 0 deletes; `0x01 | column_count << 8` inserts a whole row; any
///     other value updates the columns whose bits are set (bit i = column i).
///     `_Tables` rows add/remove tables, `_Columns` rows add columns (a new
///     table's columns carry a null Number). Changed binary data travels as
///     `Table.Key` streams.
///   * .idt: tab-separated lines (CR LF written; LF accepted) - column names,
///     column types, "[codepage<TAB>]Table<TAB>Key...", then rows with TAB,
///     CR, LF escaped as 0x10, 0x11, 0x19; binary cells name
///     `<folder>\<Table>\<file>`.

#include "winapi/system/msi/msi_validation.hpp"
#include "winapi/system/msi/msi_database.hpp"
#include "winapi/system/msi/msi_sql.hpp"
#include "winapi/system/msi/msi.hpp"
#include "winapi/com/ole32/storage.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/system/time.hpp"

#include <algorithm>
#include <climits>
#include <cstring>
#include <map>
#include <set>

namespace vitreo::winapi::msi {

namespace {

constexpr u32 kOk = 0;
constexpr u32 kInvalidHandle = 6;
constexpr u32 kInvalidParameter = 87;
constexpr u32 kOpenFailed = 110;
constexpr u32 kBadPathname = 161;
constexpr u32 kNoData = 232;
constexpr u32 kPackageOpenFailed = 1619;
constexpr u32 kTransformFailure = 1624;
constexpr u32 kFunctionFailed = 1627;
constexpr u32 kDatatypeMismatch = 1629;

constexpr i32 kErrAddExistingRow = 0x01, kErrDelMissingRow = 0x02, kErrAddExistingTable = 0x04,
              kErrDelMissingTable = 0x08, kErrUpdateMissingRow = 0x10, kErrChangeCodepage = 0x20,
              kErrViewTransform = 0x100;

constexpr char kSummaryStream[] = "\x05SummaryInformation";

void put_le(std::vector<u8>& out, u32 value, u32 width) {
    for (u32 k = 0; k < width; ++k) out.push_back(static_cast<u8>(value >> (8 * k)));
}
u32 read_le(std::span<const u8> d, usize o, u32 w) {
    u32 v = 0;
    for (u32 k = 0; k < w && o + k < d.size(); ++k) v |= static_cast<u32>(d[o + k]) << (8 * k);
    return v;
}

std::string narrow16(const std::u16string& u) {
    std::string out;
    for (const char16_t c16 : u) {
        const u32 c = c16;
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else if (c < 0x800) { out.push_back(static_cast<char>(0xC0 | (c >> 6))); out.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
        else { out.push_back(static_cast<char>(0xE0 | (c >> 12))); out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
    }
    return out;
}

/// Stored width of one cell in a transform / table stream.
u32 cell_width(const ColumnDef& c, u32 sref) {
    if (c.is_string) return sref;
    if (c.is_stream()) return 2;
    return c.size() == 4 ? 4u : 2u;
}

u32 encode_int_cell(const ColumnDef& c, const std::string& cell) {
    if (cell.empty()) return 0;
    const i32 v = cell_int(cell).value_or(0);
    return cell_width(c, 2) == 2 ? static_cast<u32>(v + 0x8000) : static_cast<u32>(v) + 0x80000000u;
}

std::string decode_int_cell(const ColumnDef& c, u32 raw) {
    if (raw == 0) return {};
    const i64 v = cell_width(c, 2) == 2 ? static_cast<i64>(raw) - 0x8000 : static_cast<i64>(raw) - 0x80000000LL;
    return std::to_string(v);
}

/// The persistent part of a table: column indices and row indices.
std::vector<usize> persistent_columns(const Table& t) {
    std::vector<usize> out;
    for (usize c = 0; c < t.columns.size(); ++c) if (!t.columns[c].temporary()) out.push_back(c);
    return out;
}
bool persistent_row(const Table& t, usize r) { return r >= t.row_temporary.size() || !t.row_temporary[r]; }

std::vector<std::string> key_cells(const Table& t, usize r) {
    return std::vector<std::string>(t.rows[r].begin(), t.rows[r].begin() + static_cast<std::ptrdiff_t>(t.key_count()));
}

/// A transform's own string pool, built while encoding.
struct Pool {
    std::vector<std::string> strings{std::string{}};
    std::vector<u32> refs{0};
    std::map<std::string, u32> ids;
    u32 id(const std::string& s) {
        if (s.empty()) return 0;
        auto [it, added] = ids.emplace(s, static_cast<u32>(strings.size()));
        if (added) { strings.push_back(s); refs.push_back(0); }
        ++refs[it->second];
        return it->second;
    }
};

// ---- transform generation ----------------------------------------------------------------

struct TransformRow {
    u32 mask = 0;
    std::vector<std::pair<const ColumnDef*, std::string>> cells;  ///< keys first, then masked cells
};

struct TableDiff {
    std::string name;
    const Table* schema = nullptr;      ///< the new database's table
    std::vector<TransformRow> rows;
};

/// Differences of @p newer against @p base, as transform rows; streams that
/// changed go to @p streams.
bool diff_databases(Package& newer, Package& base, std::vector<TransformRow>& tables_rows,
                    std::vector<TransformRow>& columns_rows, std::vector<TableDiff>& diffs,
                    std::map<std::string, std::vector<u8>>& streams) {
    static const ColumnDef kTablesName = make_column("Name", 's', 64, false, true, false, false);
    static const ColumnDef kColTable = make_column("Table", 's', 64, false, true, false, false);
    static const ColumnDef kColNumber = make_column("Number", 'i', 2, true, true, false, false);
    static const ColumnDef kColName = make_column("Name", 's', 64, false, false, false, false);
    static const ColumnDef kColType = make_column("Type", 'i', 2, false, false, false, false);
    bool changed = false;
    newer.load_all_tables();
    base.load_all_tables();
    for (const std::string& name : newer.table_names()) {
        const Table* nt = newer.table(name);
        if (!nt || nt->temporary()) continue;
        const Table* bt = base.table(name);
        if (bt && bt->temporary()) bt = nullptr;
        const auto ncols = persistent_columns(*nt);
        usize existing_cols = 0;
        if (!bt) {
            tables_rows.push_back(TransformRow{0x01 | (1u << 8), {{&kTablesName, name}}});
            changed = true;
        } else {
            existing_cols = persistent_columns(*bt).size();
        }
        // New columns (all of a new table's, or those past the base's).
        for (usize n = existing_cols; n < ncols.size(); ++n) {
            const ColumnDef& c = nt->columns[ncols[n]];
            TransformRow r{0x01 | (4u << 8), {}};
            r.cells.emplace_back(&kColTable, name);
            r.cells.emplace_back(&kColNumber, bt ? std::to_string(n + 1) : std::string{});
            r.cells.emplace_back(&kColName, c.name);
            r.cells.emplace_back(&kColType, std::to_string(c.type & 0x7FFF));
            columns_rows.push_back(std::move(r));
            changed = true;
        }
        TableDiff diff{name, nt, {}};
        const usize keys = nt->key_count();
        for (usize r = 0; r < nt->rows.size(); ++r) {
            if (!persistent_row(*nt, r)) continue;
            const auto key = key_cells(*nt, r);
            std::optional<usize> br;
            if (bt) {
                br = base.find_row(*bt, key);
                if (br && !persistent_row(*bt, *br)) br.reset();
            }
            TransformRow tr;
            for (usize k = 0; k < keys; ++k) tr.cells.emplace_back(&nt->columns[k], key[k]);
            if (!br) {
                tr.mask = 0x01 | (static_cast<u32>(ncols.size()) << 8);
                for (usize n = keys; n < ncols.size(); ++n) {
                    const usize c = ncols[n];
                    tr.cells.emplace_back(&nt->columns[c], nt->rows[r][c]);
                    if (nt->columns[c].is_stream() && !nt->rows[r][c].empty())
                        if (const auto* s = newer.stream(nt->rows[r][c])) streams[newer.stream_key(*nt, r)] = *s;
                }
            } else {
                for (usize n = keys; n < ncols.size(); ++n) {
                    const usize c = ncols[n];
                    const std::string& now = nt->rows[r][c];
                    const auto bc = n < existing_cols ? std::optional<usize>(persistent_columns(*bt)[n]) : std::nullopt;
                    const std::string was = bc ? bt->rows[*br][*bc] : std::string{};
                    bool differs = now != was;
                    if (nt->columns[c].is_stream()) {
                        const auto* a = now.empty() ? nullptr : newer.stream(now);
                        const auto* b = was.empty() ? nullptr : base.stream(was);
                        differs = (a == nullptr) != (b == nullptr) || (a && b && *a != *b);
                        if (differs && a) streams[newer.stream_key(*nt, r)] = *a;
                    }
                    if (!differs) continue;
                    tr.mask |= 1u << n;
                    tr.cells.emplace_back(&nt->columns[c], now);
                }
                if (tr.mask == 0) continue;
            }
            diff.rows.push_back(std::move(tr));
        }
        if (bt) {
            for (usize r = 0; r < bt->rows.size(); ++r) {
                if (!persistent_row(*bt, r)) continue;
                const auto key = key_cells(*bt, r);
                auto nr = newer.find_row(*nt, key);
                if (nr && persistent_row(*nt, *nr)) continue;
                TransformRow tr;
                for (usize k = 0; k < keys; ++k) tr.cells.emplace_back(&nt->columns[k], key[k]);
                diff.rows.push_back(std::move(tr));
            }
        }
        if (!diff.rows.empty()) { changed = true; diffs.push_back(std::move(diff)); }
    }
    for (const std::string& name : base.table_names()) {
        const Table* bt = base.table(name);
        if (!bt || bt->temporary()) continue;
        const Table* nt = newer.table(name);
        if (nt && !nt->temporary()) continue;
        tables_rows.push_back(TransformRow{0, {{&kTablesName, name}}});
        changed = true;
    }
    return changed;
}

std::vector<u8> encode_transform(Package& newer, std::vector<TransformRow>& tables_rows,
                                 std::vector<TransformRow>& columns_rows, std::vector<TableDiff>& diffs,
                                 std::map<std::string, std::vector<u8>>& streams) {
    Pool pool;
    // Intern in stream order so the pool reads in the order Windows writes
    // it: added columns, each table's rows, then table adds / drops.
    auto intern_rows = [&](std::vector<TransformRow>& rows) {
        for (TransformRow& r : rows)
            for (auto& [col, cell] : r.cells) if (col->is_string) (void)pool.id(cell);
    };
    intern_rows(columns_rows);
    for (TableDiff& d : diffs) intern_rows(d.rows);
    intern_rows(tables_rows);
    const u32 sref = pool.strings.size() > 0xFFFF ? 3u : 2u;

    auto encode_rows = [&](const std::vector<TransformRow>& rows, usize columns) {
        std::vector<u8> out;
        // A table of more than 16 columns has 32-bit masks, and its stream
        // starts with the 16-bit value 7 (msi_database_widemask*_probe).
        const u32 mask_width = columns > 16 ? 4u : 2u;
        if (columns > 16) put_le(out, 7, 2);
        for (const TransformRow& r : rows) {
            put_le(out, r.mask, mask_width);
            for (const auto& [col, cell] : r.cells) {
                const u32 w = cell_width(*col, sref);
                u32 v = 0;
                if (col->is_string) v = pool.ids.contains(cell) ? pool.ids[cell] : 0;
                else if (col->is_stream()) v = cell.empty() ? 0 : 1;
                else v = encode_int_cell(*col, cell);
                put_le(out, v, w);
            }
        }
        return out;
    };

    StorageNode root;
    root.is_storage = true;
    root.clsid = kClsidTransform;
    auto add = [&](const std::string& name, bool table, std::vector<u8> bytes) {
        auto node = std::make_shared<StorageNode>();
        node->name = narrow16(Package::encode_stream_name(name, table));
        node->data = std::move(bytes);
        root.children[node->name] = node;
    };
    std::vector<u8> pool_stream, data_stream;
    put_le(pool_stream, newer.codepage() | (sref == 3 ? 0x80000000u : 0u), 4);
    for (usize i = 1; i < pool.strings.size(); ++i) {
        const std::vector<u8> bytes = encode_codepage(pool.strings[i], newer.codepage());
        const u32 refs = std::min<u32>(pool.refs[i], 0xFFFF);
        if (bytes.size() > 0xFFFF) {
            put_le(pool_stream, 0, 2); put_le(pool_stream, refs, 2); put_le(pool_stream, static_cast<u32>(bytes.size()), 4);
        } else {
            put_le(pool_stream, static_cast<u32>(bytes.size()), 2); put_le(pool_stream, refs, 2);
        }
        data_stream.insert(data_stream.end(), bytes.begin(), bytes.end());
    }
    add("_StringPool", true, std::move(pool_stream));
    add("_StringData", true, std::move(data_stream));
    if (!tables_rows.empty()) add("_Tables", true, encode_rows(tables_rows, 1));
    if (!columns_rows.empty()) add("_Columns", true, encode_rows(columns_rows, 4));
    for (const TableDiff& d : diffs) add(d.name, true, encode_rows(d.rows, persistent_columns(*d.schema).size()));
    for (auto& [name, bytes] : streams) add(name, false, std::move(bytes));
    return serialize_compound_file(root);
}

// ---- transform application ----------------------------------------------------------------

struct ParsedTransform {
    std::vector<std::string> strings{std::string{}};
    u32 sref = 2;
    u32 codepage = 0;
    std::map<std::string, std::vector<u8>> tables;   ///< table streams by decoded name
    std::map<std::string, std::vector<u8>> streams;  ///< other streams
};

std::optional<ParsedTransform> parse_transform(std::span<const u8> image, bool& is_transform) {
    is_transform = false;
    auto root = parse_compound_file(image);
    if (!root) return std::nullopt;
    ParsedTransform t;
    for (const auto& [raw, node] : root->children) {
        if (!node || node->is_storage) continue;
        std::vector<u16> units;
        // UTF-8 of the UTF-16 name back to code units.
        for (usize i = 0; i < raw.size();) {
            const u8 b0 = static_cast<u8>(raw[i]);
            if (b0 < 0x80) { units.push_back(b0); ++i; }
            else if ((b0 & 0xE0) == 0xC0 && i + 1 < raw.size()) { units.push_back(static_cast<u16>(((b0 & 0x1F) << 6) | (raw[i + 1] & 0x3F))); i += 2; }
            else if (i + 2 < raw.size()) { units.push_back(static_cast<u16>(((b0 & 0x0F) << 12) | ((raw[i + 1] & 0x3F) << 6) | (raw[i + 2] & 0x3F))); i += 3; }
            else ++i;
        }
        const std::string name = decode_stream_name(units);
        if (!units.empty() && units[0] == 0x4840) t.tables[name] = node->data;
        else t.streams[name] = node->data;
    }
    auto pool_it = t.tables.find("_StringPool");
    auto data_it = t.tables.find("_StringData");
    if (pool_it == t.tables.end() || data_it == t.tables.end()) return std::nullopt;
    // A database is not a transform (measured: applying an .msi fails 2246).
    is_transform = root->clsid != kClsidDatabase && root->clsid != kClsidPatch;
    const auto& pool = pool_it->second;
    const auto& data = data_it->second;
    if (pool.size() < 4) return std::nullopt;
    const u32 header = read_le(pool, 0, 4);
    t.codepage = header & 0x7FFFFFFF;
    t.sref = (header & 0x80000000u) ? 3 : 2;
    usize off = 0;
    for (usize slot = 1; slot < pool.size() / 4; ++slot) {
        u32 len = read_le(pool, slot * 4, 2);
        const u32 refs = read_le(pool, slot * 4 + 2, 2);
        if (len == 0 && refs != 0 && slot + 1 < pool.size() / 4) {
            // A long string: two pool entries, one index.
            len = read_le(pool, slot * 4 + 4, 2) | (read_le(pool, slot * 4 + 6, 2) << 16);
            ++slot;
        }
        if (off + len > data.size()) break;
        t.strings.push_back(decode_codepage(std::span<const u8>(data).subspan(off, len), t.codepage));
        off += len;
    }
    return t;
}

/// One decoded transform row.
struct Op {
    u32 mask = 0;
    std::vector<std::string> keys;
    std::map<usize, std::string> cells;  ///< column index -> value (insert: every column)
};

/// Decode a table stream against @p columns.
std::vector<Op> decode_ops(const ParsedTransform& t, std::span<const u8> s, const std::vector<ColumnDef>& columns) {
    std::vector<Op> ops;
    usize keys = 0;
    while (keys < columns.size() && columns[keys].key()) ++keys;
    const u32 mask_width = columns.size() > 16 ? 4u : 2u;
    usize o = columns.size() > 16 ? 2 : 0;  // the leading 7 of a wide table's stream
    auto read_cell = [&](const ColumnDef& c) -> std::string {
        const u32 w = cell_width(c, t.sref);
        const u32 raw = read_le(s, o, w);
        o += w;
        if (c.is_string) return raw < t.strings.size() ? t.strings[raw] : std::string{};
        if (c.is_stream()) return raw ? std::string("\x01") : std::string{};
        return decode_int_cell(c, raw);
    };
    while (o + mask_width <= s.size()) {
        Op op;
        op.mask = read_le(s, o, mask_width);
        o += mask_width;
        if (op.mask & 1) {
            const usize n = std::min<usize>(columns.size(), op.mask >> 8 ? op.mask >> 8 : columns.size());
            for (usize c = 0; c < n; ++c) {
                std::string v = read_cell(columns[c]);
                if (c < keys) op.keys.push_back(v);
                op.cells[c] = std::move(v);
            }
        } else {
            for (usize c = 0; c < keys; ++c) op.keys.push_back(read_cell(columns[c]));
            for (usize c = keys; c < columns.size(); ++c)
                if (op.mask & (1u << c)) op.cells[c] = read_cell(columns[c]);
        }
        ops.push_back(std::move(op));
    }
    return ops;
}

std::string join_keys(const std::vector<std::string>& keys) {
    std::string s;
    for (usize i = 0; i < keys.size(); ++i) { if (i) s.push_back('\t'); s += keys[i]; }
    return s;
}

}  // namespace

u32 apply_transform(Package& target, std::span<const u8> image, i32 conditions, std::string* error_table) {
    bool is_transform = false;
    auto parsed = parse_transform(image, is_transform);
    if (!parsed || !is_transform) {
        set_last_error_record(2246, {error_table ? *error_table : std::string{}, "", ""});
        return kTransformFailure;
    }
    ParsedTransform& t = *parsed;
    std::lock_guard lock(target.mutex());
    target.load_all_tables();
    const bool view = (conditions & kErrViewTransform) != 0;
    auto fail = [&](u32 code, const std::string& arg) {
        set_last_error_record(code, {error_table ? *error_table : std::string{}, arg});
        return kTransformFailure;
    };
    if (!view && t.codepage != 0 && target.codepage() != 0 && t.codepage != target.codepage() &&
        !(conditions & kErrChangeCodepage))
        return fail(2254, std::to_string(t.codepage));

    // _TransformView rows (MSITRANSFORM_ERROR_VIEWTRANSFORM): Table, Column,
    // Row, Data, Current, all temporary strings (measured g0 g0 G0 G0 G0).
    Table* tv = nullptr;
    if (view) {
        tv = target.find_table("_TransformView");
        if (!tv) {
            std::vector<ColumnDef> cols{make_column("Table", 's', 0, false, true, false, true),
                                        make_column("Column", 's', 0, false, true, false, true),
                                        make_column("Row", 's', 0, true, true, false, true),
                                        make_column("Data", 's', 0, true, false, false, true),
                                        make_column("Current", 's', 0, true, false, false, true)};
            (void)target.create_table("_TransformView", std::move(cols), true);
            tv = target.find_table("_TransformView");
        }
    }
    auto view_row = [&](const std::string& table, const std::string& column, const std::string& row,
                        const std::string& data, const std::string& current) {
        if (tv) (void)target.insert_row(*tv, {table, column, row, data, current}, true);
    };

    // _Tables: adds and drops.
    std::vector<std::string> created;
    if (auto it = t.tables.find("_Tables"); it != t.tables.end()) {
        const std::vector<ColumnDef> cols{make_column("Name", 's', 64, false, true, false, false)};
        for (const Op& op : decode_ops(t, it->second, cols)) {
            const std::string& name = op.keys.empty() ? std::string{} : op.keys[0];
            const bool exists = target.find_table(name) != nullptr;
            if (op.mask & 1) {
                if (view) { view_row(name, "CREATE", {}, {}, {}); created.push_back(name); continue; }
                if (exists && !(conditions & kErrAddExistingTable)) return fail(2252, name);
                if (!exists) created.push_back(name);
            } else {
                if (view) { view_row(name, "DROP", {}, {}, {}); continue; }
                if (!exists) {
                    if (!(conditions & kErrDelMissingTable)) return fail(2253, name);
                    continue;
                }
                (void)target.drop_table(name);
            }
        }
    }
    // _Columns: the new tables' schemas and added columns.
    std::map<std::string, std::vector<ColumnDef>> new_schemas;
    std::map<std::string, std::vector<ColumnDef>> view_schemas;
    if (auto it = t.tables.find("_Columns"); it != t.tables.end()) {
        const std::vector<ColumnDef> cols{make_column("Table", 's', 64, false, true, false, false),
                                          make_column("Number", 'i', 2, true, true, false, false),
                                          make_column("Name", 's', 64, false, false, false, false),
                                          make_column("Type", 'i', 2, false, false, false, false)};
        for (const Op& op : decode_ops(t, it->second, cols)) {
            if (!(op.mask & 1)) continue;
            const std::string table = op.cells.count(0) ? op.cells.at(0) : std::string{};
            ColumnDef c;
            c.name = op.cells.count(2) ? op.cells.at(2) : std::string{};
            c.type = static_cast<u16>(cell_int(op.cells.count(3) ? op.cells.at(3) : "0").value_or(0));
            c.is_string = (c.type & coltype::kString) == coltype::kString;
            const bool is_new = std::find(created.begin(), created.end(), table) != created.end();
            std::string number = op.cells.count(1) ? op.cells.at(1) : std::string{};
            auto& schema = is_new ? new_schemas[table] : view_schemas[table];
            if (number.empty()) number = std::to_string(schema.size() + 1);
            schema.push_back(c);
            if (view) { view_row(table, c.name, {}, std::to_string(c.type), number); continue; }
            if (!is_new && target.find_table(table)) (void)target.add_column(table, c, false);
        }
    }
    if (!view) {
        for (const std::string& name : created) {
            auto cols = new_schemas[name];
            (void)target.create_table(name, std::move(cols), false);
        }
    }

    // Row changes. An unsuppressed row error does not fail the call: the
    // table it hit can no longer be loaded (measured: apply returns 0, the
    // next query of that table fails 2229).
    std::set<std::string> broken;
    for (const auto& [name, bytes] : t.tables) {
        if (name == "_StringPool" || name == "_StringData" || name == "_Tables" || name == "_Columns") continue;
        Table* table = target.find_table(name);
        std::vector<ColumnDef> columns;
        if (table) columns = table->columns;
        else if (new_schemas.count(name)) columns = new_schemas[name];
        if (columns.empty()) {
            if (view) continue;
            if (!(conditions & kErrUpdateMissingRow)) return fail(2255, name);
            continue;
        }
        if (view && view_schemas.count(name) && table) {
            for (const ColumnDef& c : view_schemas[name]) columns.push_back(c);
        }
        const std::vector<Op> ops = decode_ops(t, bytes, columns);
        const usize keys = [&] { usize k = 0; while (k < columns.size() && columns[k].key()) ++k; return k; }();
        auto stream_name = [&](const Op& op) {
            std::string s = name;
            for (const std::string& k : op.keys) { s.push_back('.'); s += k; }
            return s;
        };
        if (view) {
            // Per column: each row's value; then INSERT / DELETE markers.
            for (usize c = keys; c < columns.size(); ++c) {
                for (const Op& op : ops) {
                    if (op.mask == 0 || !op.cells.count(c)) continue;
                    std::string data = op.cells.at(c);
                    if (columns[c].is_stream() && !data.empty()) data = stream_name(op);
                    std::string current;
                    if (table && c < table->columns.size()) {
                        if (auto r = target.find_row(*table, op.keys)) current = table->rows[*r][c];
                    }
                    view_row(name, columns[c].name, join_keys(op.keys), data, current);
                }
            }
            for (const Op& op : ops) if (op.mask & 1) view_row(name, "INSERT", join_keys(op.keys), {}, {});
            for (const Op& op : ops) if (op.mask == 0) view_row(name, "DELETE", join_keys(op.keys), {}, {});
            continue;
        }
        if (!table) table = target.find_table(name);
        if (!table) continue;
        for (const Op& op : ops) {
            auto row = target.find_row(*table, op.keys);
            if (op.mask == 0) {
                if (!row) {
                    if (!(conditions & kErrDelMissingRow)) { broken.insert(name); break; }
                    continue;
                }
                (void)target.delete_row(*table, *row);
                continue;
            }
            std::map<usize, std::vector<u8>> streams;
            for (const auto& [c, v] : op.cells) {
                if (c < table->columns.size() && table->columns[c].is_stream() && !v.empty()) {
                    auto s = t.streams.find(stream_name(op));
                    streams[c] = s != t.streams.end() ? s->second : std::vector<u8>{};
                }
            }
            if (op.mask & 1) {
                if (row) {
                    if (!(conditions & kErrAddExistingRow)) { broken.insert(name); break; }
                    continue;  // suppressed: the existing row stays (measured)
                }
                std::vector<std::string> cells(table->columns.size());
                for (const auto& [c, v] : op.cells) if (c < cells.size()) cells[c] = v;
                (void)target.insert_row(*table, std::move(cells), false, streams.empty() ? nullptr : &streams);
                continue;
            }
            if (!row) {
                if (!(conditions & kErrUpdateMissingRow)) { broken.insert(name); break; }
                continue;
            }
            for (const auto& [c, v] : op.cells) {
                if (c >= table->columns.size()) continue;
                if (table->columns[c].is_stream()) {
                    auto s = streams.find(c);
                    (void)target.update_cell(*table, *row, c, v, s != streams.end() ? &s->second : nullptr);
                } else {
                    (void)target.update_cell(*table, *row, c, v);
                }
            }
        }
    }
    for (const std::string& name : broken) target.mark_unloadable(name);
    return kOk;
}

namespace {

// ---- .idt archives -------------------------------------------------------------------------

/// FILETIME <-> "yyyy/mm/dd hh:mm:ss" local time (measured: export renders
/// local time rounded to the second; import reads it back as local time).
std::string format_filetime(u64 ft) {
    TimeManager tm;
    u64 local = 0;
    if (!tm.file_time_to_local_file_time(ft, local)) local = ft;
    local += 5000000;  // round to the nearest second
    SystemTime st{};
    if (!tm.file_time_to_system_time(local, st)) return {};
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04u/%02u/%02u %02u:%02u:%02u", st.year, st.month, st.day, st.hour, st.minute, st.second);
    return buf;
}

std::optional<u64> parse_filetime(const std::string& s) {
    unsigned y, mo, d, h, mi, se;
    if (std::sscanf(s.c_str(), "%u/%u/%u %u:%u:%u", &y, &mo, &d, &h, &mi, &se) != 6) return std::nullopt;
    SystemTime st{};
    st.year = static_cast<u16>(y); st.month = static_cast<u16>(mo); st.day = static_cast<u16>(d);
    st.hour = static_cast<u16>(h); st.minute = static_cast<u16>(mi); st.second = static_cast<u16>(se);
    TimeManager tm;
    u64 local = 0, utc = 0;
    if (!tm.system_time_to_file_time(st, local)) return std::nullopt;
    if (!tm.local_file_time_to_file_time(local, utc)) utc = local;
    return utc;
}

std::string escape_idt(const std::string& v) {
    std::string out;
    for (const char c : v) {
        if (c == '\t') out.push_back('\x10');
        else if (c == '\r') out.push_back('\x11');
        else if (c == '\n') out.push_back('\x19');
        else out.push_back(c);
    }
    return out;
}

std::string unescape_idt(const std::string& v) {
    std::string out;
    for (const char c : v) {
        if (c == '\x10') out.push_back('\t');
        else if (c == '\x11') out.push_back('\r');
        else if (c == '\x19') out.push_back('\n');
        else out.push_back(c);
    }
    return out;
}

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> out;
    usize start = 0;
    for (;;) {
        const usize p = line.find('\t', start);
        out.push_back(line.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos) return out;
        start = p + 1;
    }
}

bool absolute_folder(const std::string& f) {
    return (f.size() >= 2 && std::isalpha(static_cast<unsigned char>(f[0])) && f[1] == ':') ||
           (f.size() >= 2 && f[0] == '\\' && f[1] == '\\');
}

std::string join_path(const std::string& folder, const std::string& name) {
    if (!folder.empty() && (folder.back() == '\\' || folder.back() == '/')) return folder + name;
    return folder + "\\" + name;
}

u32 export_table(MsiManager& mgr, DatabaseObject& db, const std::string& table, const std::string& folder,
                 const std::string& file) {
    Package& pkg = *db.package;
    std::lock_guard lock(pkg.mutex());
    std::string text;
    const std::string crlf = "\r\n";
    if (table == "_ForceCodepage") {
        text = crlf + crlf + std::to_string(pkg.codepage()) + "\t_ForceCodepage" + crlf;
    } else if (table == "_SummaryInformation") {
        text = "PropertyId\tValue" + crlf + "i2\tl255" + crlf + "_SummaryInformation\tPropertyId" + crlf;
        if (const auto* s = pkg.stream(kSummaryStream)) {
            for (const auto& [pid, p] : parse_summary_information(*s)) {
                std::string v;
                if (p.vt == 30) v = p.text;
                else if (p.vt == 64) v = format_filetime(p.filetime);
                else v = std::to_string(p.integer);
                text += std::to_string(pid) + "\t" + escape_idt(v) + crlf;
            }
        }
    } else {
        const Table* t = sql::is_virtual_table(table) ? nullptr : pkg.find_table(table);
        if (!t) {
            set_last_error_record(2205, {db.path, table});
            return kFunctionFailed;
        }
        const auto cols = persistent_columns(*t);
        std::string names, types, keys;
        for (usize n = 0; n < cols.size(); ++n) {
            const ColumnDef& c = t->columns[cols[n]];
            if (n) { names += "\t"; types += "\t"; }
            names += c.name;
            types += c.definition();
            if (c.key()) keys += "\t" + c.name;
        }
        std::string body;
        bool non_ascii = false;
        for (usize r = 0; r < t->rows.size(); ++r) {
            if (!persistent_row(*t, r)) continue;
            for (usize n = 0; n < cols.size(); ++n) {
                const ColumnDef& c = t->columns[cols[n]];
                std::string v = t->rows[r][cols[n]];
                if (c.is_stream() && !v.empty()) {
                    // <folder>\<Table>\<key1>.<key2>....ibd (measured "b..ibd").
                    std::string fname;
                    for (usize k = 0; k < t->key_count(); ++k) { if (k) fname += "."; fname += t->rows[r][k]; }
                    fname += ".ibd";
                    const std::string dir = join_path(folder, table);
                    if (!db_make_dir(mgr, dir)) { set_last_error_record(2214, {db.path, file, ""}); return kFunctionFailed; }
                    const auto* bytes = pkg.stream(v);
                    if (!db_write_file(mgr, join_path(dir, fname), bytes ? std::span<const u8>(*bytes) : std::span<const u8>{})) {
                        set_last_error_record(2214, {db.path, file, ""});
                        return kFunctionFailed;
                    }
                    v = fname;
                }
                for (const char ch : v) non_ascii = non_ascii || (static_cast<unsigned char>(ch) >= 0x80);
                if (n) body += "\t";
                body += escape_idt(v);
            }
            body += crlf;
        }
        // The code page leads line 3 when the text needs one (measured).
        std::string line3 = table + keys;
        if (non_ascii || pkg.codepage() != 0) {
            if (non_ascii) line3 = std::to_string(pkg.codepage() ? pkg.codepage() : 1252u) + "\t" + line3;
        }
        text = names + crlf + types + crlf + line3 + crlf + body;
    }
    const u32 cp = pkg.codepage();
    const std::vector<u8> bytes = encode_codepage(text, cp);
    if (!db_write_file(mgr, join_path(folder, file), bytes)) {
        set_last_error_record(2214, {db.path, file, ""});
        return kFunctionFailed;
    }
    return kOk;
}

u32 import_table(MsiManager& mgr, DatabaseObject& db, const std::string& folder, const std::string& file) {
    auto bytes = db_read_file(mgr, join_path(folder, file));
    if (!bytes) { set_last_error_record(2215, {db.path, file, ""}); return kFunctionFailed; }
    Package& pkg = *db.package;
    std::lock_guard lock(pkg.mutex());
    // Lines split on LF, a trailing CR dropped (CR LF and LF both accepted).
    std::vector<std::string> raw_lines;
    {
        std::string cur;
        for (const u8 b : *bytes) {
            if (b == '\n') { if (!cur.empty() && cur.back() == '\r') cur.pop_back(); raw_lines.push_back(cur); cur.clear(); }
            else cur.push_back(static_cast<char>(b));
        }
        if (!cur.empty()) raw_lines.push_back(cur);
    }
    auto bad = [&](usize line) { set_last_error_record(2216, {db.path, file, std::to_string(line)}); return kFunctionFailed; };
    if (raw_lines.size() < 3) return bad(raw_lines.size() + 1);
    std::vector<std::string> l3 = split_tabs(raw_lines[2]);
    u32 codepage = pkg.codepage();
    if (raw_lines[0].empty() && raw_lines[1].empty() && l3.size() == 2 && l3[1] == "_ForceCodepage") {
        pkg.set_codepage(static_cast<u32>(cell_int(l3[0]).value_or(0)));
        return kOk;
    }
    if (!l3.empty() && !l3[0].empty() && std::all_of(l3[0].begin(), l3[0].end(), [](char c) { return c >= '0' && c <= '9'; })) {
        codepage = static_cast<u32>(std::stoul(l3[0]));
        l3.erase(l3.begin());
    }
    auto decode = [&](const std::string& s) {
        return decode_codepage(std::span<const u8>(reinterpret_cast<const u8*>(s.data()), s.size()), codepage);
    };
    const std::vector<std::string> names = split_tabs(decode(raw_lines[0]));
    const std::vector<std::string> types = split_tabs(raw_lines[1]);
    if (l3.empty()) return bad(3);
    const std::string table = decode(l3[0]);

    if (table == "_SummaryInformation") {
        std::map<u32, SummaryProperty> props;
        if (const auto* s = pkg.stream(kSummaryStream)) props = parse_summary_information(*s);
        for (usize i = 3; i < raw_lines.size(); ++i) {
            const auto f = split_tabs(raw_lines[i]);
            if (f.size() != 2) return bad(i + 1);
            const auto pid = cell_int(f[0]);
            if (!pid) return bad(i + 1);
            const std::string v = unescape_idt(decode(f[1]));
            SummaryProperty p;
            const u32 id = static_cast<u32>(*pid);
            if (id == 1) { p.vt = 2; p.integer = cell_int(v).value_or(0); }
            else if (id == 14 || id == 15 || id == 16 || id == 19) { p.vt = 3; p.integer = cell_int(v).value_or(0); }
            else if (id >= 10 && id <= 13) { auto ft = parse_filetime(v); if (!ft) return bad(i + 1); p.vt = 64; p.filetime = *ft; }
            else if ((id >= 2 && id <= 9) || id == 18) { p.vt = 30; p.text = v; }
            else return bad(i + 1);
            props[id] = std::move(p);
        }
        pkg.set_stream(kSummaryStream, build_summary_information(props));
        db.dirty = true;
        return kOk;
    }

    if (names.size() != types.size() || names.empty()) return bad(2);
    std::vector<ColumnDef> cols;
    for (usize i = 0; i < names.size(); ++i) {
        const std::string& ty = types[i];
        if (ty.empty()) return bad(2);
        const char letter = ty[0];
        const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
        auto width = cell_int(ty.substr(1));
        if (!width || *width < 0) return bad(2);
        const bool nullable = std::isupper(static_cast<unsigned char>(letter)) != 0;
        if (lower == 's' || lower == 'l') {
            if (*width > 255) return bad(2);
            cols.push_back(make_column(names[i], 's', static_cast<u32>(*width), nullable, false, lower == 'l', false));
        } else if (lower == 'i') {
            if (*width != 2 && *width != 4) return bad(2);
            cols.push_back(make_column(names[i], 'i', static_cast<u32>(*width), nullable, false, false, false));
        } else if (lower == 'v') {
            cols.push_back(make_column(names[i], 'v', 0, nullable, false, false, false));
        } else {
            return bad(2);
        }
    }
    // Keys name the leading columns, in order.
    for (usize k = 1; k < l3.size(); ++k) {
        if (k - 1 >= cols.size() || cols[k - 1].name != decode(l3[k])) return bad(3);
        cols[k - 1].type |= coltype::kKey;
    }
    if (l3.size() < 2) return bad(3);

    // Build the table off to the side so a failure changes nothing.
    Package staging = Package::create(codepage);
    if (staging.create_table(table, cols, false)) return bad(3);
    Table* st = staging.find_table(table);
    for (usize i = 3; i < raw_lines.size(); ++i) {
        const auto f = split_tabs(raw_lines[i]);
        if (f.size() != cols.size()) return bad(i + 1);
        std::vector<std::string> cells(cols.size());
        std::map<usize, std::vector<u8>> streams;
        for (usize c = 0; c < cols.size(); ++c) {
            std::string v = unescape_idt(decode(f[c]));
            if (cols[c].is_stream()) {
                if (!v.empty()) {
                    auto data = db_read_file(mgr, join_path(join_path(folder, table), v));
                    if (!data) { set_last_error_record(2216, {db.path, v, std::to_string(i + 1)}); return kFunctionFailed; }
                    streams[c] = std::move(*data);
                }
                continue;
            }
            if (cols[c].is_integer() && !v.empty()) {
                sql::Value val;
                val.kind = sql::Value::Kind::Str;
                val.s = v;
                if (!sql::value_to_cell(cols[c], val, v)) return bad(i + 1);
            }
            cells[c] = std::move(v);
        }
        if (staging.insert_row(*st, std::move(cells), false, streams.empty() ? nullptr : &streams)) return bad(i + 1);
    }
    // Replace any existing table (measured: an import overwrites).
    if (pkg.find_table(table)) (void)pkg.drop_table(table);
    if (pkg.create_table(table, cols, false)) return bad(3);
    Table* t = pkg.find_table(table);
    for (usize r = 0; r < st->rows.size(); ++r) {
        std::map<usize, std::vector<u8>> streams;
        std::vector<std::string> cells = st->rows[r];
        for (usize c = 0; c < cols.size(); ++c) {
            if (cols[c].is_stream() && !cells[c].empty()) {
                if (const auto* s = staging.stream(cells[c])) streams[c] = *s;
            }
        }
        (void)pkg.insert_row(*t, std::move(cells), false, streams.empty() ? nullptr : &streams);
    }
    db.dirty = true;
    return kOk;
}

// ---- merge ---------------------------------------------------------------------------------

u32 merge_databases(DatabaseObject& base_db, DatabaseObject& ref_db, const std::string* error_table) {
    Package& base = *base_db.package;
    Package& ref = *ref_db.package;
    if (&base == &ref) return kOk;
    std::scoped_lock lock(base.mutex(), ref.mutex());
    base.load_all_tables();
    ref.load_all_tables();
    // Schemas must agree: names, key count, types (localization and length
    // ignored); a mismatch is ERROR_DATATYPE_MISMATCH before anything merges.
    for (const std::string& name : ref.table_names()) {
        const Table* rt = ref.table(name);
        const Table* bt = base.table(name);
        if (!rt || !bt || rt->temporary()) continue;
        bool same = rt->columns.size() == bt->columns.size() && rt->key_count() == bt->key_count();
        for (usize c = 0; same && c < rt->columns.size(); ++c) {
            const ColumnDef& a = rt->columns[c];
            const ColumnDef& b = bt->columns[c];
            const u16 mask = coltype::kString | coltype::kNullable | coltype::kKey | (a.is_string ? 0 : coltype::kWidthMask);
            same = a.name == b.name && (a.type & mask) == (b.type & mask);
        }
        if (!same) {
            set_last_error_record(2248, {base_db.path, name, "1"});
            return kDatatypeMismatch;
        }
    }
    std::vector<std::pair<std::string, u32>> conflicts;
    u32 total = 0;
    for (const std::string& name : ref.table_names()) {
        const Table* rt = ref.table(name);
        if (!rt || rt->temporary()) continue;
        Table* bt = base.find_table(name);
        if (!bt) {
            std::vector<ColumnDef> cols;
            for (const ColumnDef& c : rt->columns) if (!c.temporary()) cols.push_back(c);
            (void)base.create_table(name, std::move(cols), false);
            bt = base.find_table(name);
        } else {
            // Grow string columns to the reference's length (0 = unbounded).
            for (usize c = 0; c < bt->columns.size(); ++c) {
                ColumnDef& b = bt->columns[c];
                const ColumnDef& a = rt->columns[c];
                if (!b.is_string || b.size() == 0) continue;
                if (a.size() == 0 || a.size() > b.size())
                    b.type = static_cast<u16>((b.type & ~coltype::kWidthMask) | a.size());
            }
        }
        u32 table_conflicts = 0;
        for (usize r = 0; r < rt->rows.size(); ++r) {
            if (!persistent_row(*rt, r)) continue;
            const auto key = key_cells(*rt, r);
            if (auto br = base.find_row(*bt, key)) {
                bool identical = true;
                for (usize c = 0; c < rt->columns.size() && identical; ++c) {
                    if (rt->columns[c].is_stream()) {
                        const auto* a = rt->rows[r][c].empty() ? nullptr : ref.stream(rt->rows[r][c]);
                        const auto* b = bt->rows[*br][c].empty() ? nullptr : base.stream(bt->rows[*br][c]);
                        identical = (a == nullptr) == (b == nullptr) && (!a || *a == *b);
                    } else {
                        identical = rt->rows[r][c] == bt->rows[*br][c];
                    }
                }
                if (!identical) ++table_conflicts;
                continue;
            }
            std::map<usize, std::vector<u8>> streams;
            for (usize c = 0; c < rt->columns.size(); ++c) {
                if (rt->columns[c].is_stream() && !rt->rows[r][c].empty())
                    if (const auto* s = ref.stream(rt->rows[r][c])) streams[c] = *s;
            }
            std::vector<std::string> cells(rt->rows[r].begin(), rt->rows[r].begin() + static_cast<std::ptrdiff_t>(bt->columns.size()));
            (void)base.insert_row(*bt, std::move(cells), false, streams.empty() ? nullptr : &streams);
        }
        if (table_conflicts) { conflicts.emplace_back(name, table_conflicts); total += table_conflicts; }
    }
    base_db.dirty = true;
    if (conflicts.empty()) return kOk;
    if (error_table && !error_table->empty()) {
        Table* et = base.find_table(*error_table);
        if (!et) {
            (void)base.create_table(*error_table,
                                    {make_column("Table", 's', 255, false, true, false, false),
                                     make_column("NumRowMergeConflicts", 'i', 2, false, false, false, false)},
                                    false);
            et = base.find_table(*error_table);
        }
        if (et) {
            for (const auto& [name, n] : conflicts) {
                const std::vector<std::string> key{name};
                if (auto row = base.find_row(*et, key)) (void)base.delete_row(*et, *row);
                (void)base.insert_row(*et, {name, std::to_string(n)}, false);
            }
        }
    }
    set_last_error_record(2268, {base_db.path, std::to_string(total)});
    return kFunctionFailed;
}

/// MsiCreateTransformSummaryInfo's properties (measured): PIDs 2-6 and 18
/// empty, 7 the base template, 8 the new template, 9 "{base ProductCode}
/// base version;{new ProductCode}new version;{UpgradeCode}", 14 the new
/// database's, 16 the error conditions with the validation flags above them.
std::map<u32, SummaryProperty> transform_summary(Package& newer, Package& base, i32 errors, i32 validation) {
    auto summary = [](Package& p) {
        const auto* s = p.stream(kSummaryStream);
        return s ? parse_summary_information(*s) : std::map<u32, SummaryProperty>{};
    };
    auto property = [](Package& p, const std::string& name) -> std::string {
        const Table* t = p.table("Property");
        if (!t) return {};
        auto r = p.find_row(*t, std::vector<std::string>{name});
        return r && t->columns.size() > 1 ? t->rows[*r][1] : std::string{};
    };
    const auto ns = summary(newer), bs = summary(base);
    std::map<u32, SummaryProperty> out;
    auto text = [&](u32 pid, std::string v) { SummaryProperty p; p.vt = 30; p.text = std::move(v); out[pid] = p; };
    for (u32 pid : {2u, 3u, 4u, 5u, 6u, 18u}) text(pid, {});
    text(7, bs.count(7) ? bs.at(7).text : std::string{});
    text(8, ns.count(7) ? ns.at(7).text : std::string{});
    text(9, property(base, "ProductCode") + property(base, "ProductVersion") + ";" + property(newer, "ProductCode") +
                property(newer, "ProductVersion") + ";" + property(base, "UpgradeCode"));
    SummaryProperty v14;
    v14.vt = 3;
    v14.integer = ns.count(14) ? ns.at(14).integer : 0;
    out[14] = v14;
    SummaryProperty v16;
    v16.vt = 3;
    v16.integer = static_cast<i32>((static_cast<u32>(errors) & 0xFFFF) | (static_cast<u32>(validation) << 16));
    out[16] = v16;
    return out;
}

}  // namespace

void register_transform_api(MsiManager& mgr, HandlerMap& map) {
    MsiManager* M = &mgr;
    for (const bool wide : {false, true}) {
        map[wide ? "MsiDatabaseGenerateTransformW" : "MsiDatabaseGenerateTransformA"] = [M, wide](Marshaler& m) {
            auto newer = database_of(*M, m.arg32(0));
            auto base = database_of(*M, m.arg32(1));
            if (!newer || !base) { m.set_return(kInvalidHandle); return; }
            auto file = db_in_str(m, m.arg_ptr(2), wide);
            std::vector<TransformRow> tables_rows, columns_rows;
            std::vector<TableDiff> diffs;
            std::map<std::string, std::vector<u8>> streams;
            std::scoped_lock lock(newer->package->mutex(), base->package->mutex());
            const bool changed = diff_databases(*newer->package, *base->package, tables_rows, columns_rows, diffs, streams);
            clear_last_error_record();
            // A null file only asks whether the databases differ (Learn).
            if (!file) { m.set_return(changed ? kOk : kNoData); return; }
            const std::vector<u8> image = encode_transform(*newer->package, tables_rows, columns_rows, diffs, streams);
            if (!db_write_file(*M, *file, image)) {
                set_last_error_record(2203, {*file, ""});
                m.set_return(kOpenFailed);
                return;
            }
            m.set_return(kOk);
        };
        map[wide ? "MsiDatabaseApplyTransformW" : "MsiDatabaseApplyTransformA"] = [M, wide](Marshaler& m) {
            auto db = database_of(*M, m.arg32(0));
            if (!db) { m.set_return(kInvalidHandle); return; }
            auto file = db_in_str(m, m.arg_ptr(1), wide);
            if (!file) { m.set_return(kInvalidParameter); return; }
            auto bytes = db_read_file(*M, *file);
            if (!bytes) {
                // STG_E_FILENOTFOUND as the record's second argument (measured).
                set_last_error_record(2203, {*file, std::to_string(static_cast<i32>(0x80030002u))});
                m.set_return(kOpenFailed);
                return;
            }
            std::string name = db->path;
            const u32 rc = apply_transform(*db->package, *bytes, static_cast<i32>(m.arg32(2)), &name);
            if (rc == kOk) { clear_last_error_record(); db->dirty = true; }
            m.set_return(rc);
        };
        map[wide ? "MsiCreateTransformSummaryInfoW" : "MsiCreateTransformSummaryInfoA"] = [M, wide](Marshaler& m) {
            auto newer = database_of(*M, m.arg32(0));
            auto base = database_of(*M, m.arg32(1));
            if (!newer || !base) { m.set_return(kInvalidHandle); return; }
            auto file = db_in_str(m, m.arg_ptr(2), wide);
            if (!file) { m.set_return(kInvalidParameter); return; }
            auto bytes = db_read_file(*M, *file);
            auto root = bytes ? parse_compound_file(*bytes) : nullptr;
            if (!root) { m.set_return(kPackageOpenFailed); return; }
            std::map<u32, SummaryProperty> props;
            {
                std::scoped_lock lock(newer->package->mutex(), base->package->mutex());
                props = transform_summary(*newer->package, *base->package, static_cast<i32>(m.arg32(3)),
                                          static_cast<i32>(m.arg32(4)));
            }
            auto node = std::make_shared<StorageNode>();
            node->name = kSummaryStream;
            node->data = build_summary_information(props);
            root->children[node->name] = node;
            if (!db_write_file(*M, *file, serialize_compound_file(*root))) { m.set_return(kFunctionFailed); return; }
            clear_last_error_record();
            m.set_return(kOk);
        };
        map[wide ? "MsiDatabaseMergeW" : "MsiDatabaseMergeA"] = [M, wide](Marshaler& m) {
            auto base = database_of(*M, m.arg32(0));
            auto ref = database_of(*M, m.arg32(1));
            if (!base || !ref) { m.set_return(kInvalidHandle); return; }
            auto table = db_in_str(m, m.arg_ptr(2), wide);
            const u32 rc = merge_databases(*base, *ref, table ? &*table : nullptr);
            if (rc == kOk) clear_last_error_record();
            m.set_return(rc);
        };
        map[wide ? "MsiDatabaseExportW" : "MsiDatabaseExportA"] = [M, wide](Marshaler& m) {
            auto db = database_of(*M, m.arg32(0));
            if (!db) { m.set_return(kInvalidHandle); return; }
            auto table = db_in_str(m, m.arg_ptr(1), wide);
            auto folder = db_in_str(m, m.arg_ptr(2), wide);
            auto file = db_in_str(m, m.arg_ptr(3), wide);
            if (!table || !folder || !file) { m.set_return(kInvalidParameter); return; }
            if (!absolute_folder(*folder)) { m.set_return(kBadPathname); return; }
            const u32 rc = export_table(*M, *db, *table, *folder, *file);
            if (rc == kOk) clear_last_error_record();
            m.set_return(rc);
        };
        map[wide ? "MsiDatabaseImportW" : "MsiDatabaseImportA"] = [M, wide](Marshaler& m) {
            auto db = database_of(*M, m.arg32(0));
            if (!db) { m.set_return(kInvalidHandle); return; }
            auto folder = db_in_str(m, m.arg_ptr(1), wide);
            auto file = db_in_str(m, m.arg_ptr(2), wide);
            if (!folder || !file) { m.set_return(kInvalidParameter); return; }
            if (!absolute_folder(*folder)) { m.set_return(kBadPathname); return; }
            const u32 rc = import_table(*M, *db, *folder, *file);
            if (rc == kOk) clear_last_error_record();
            m.set_return(rc);
        };
    }
}

}  // namespace vitreo::winapi::msi
