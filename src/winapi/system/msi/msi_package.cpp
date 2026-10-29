// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_package.cpp
/// @brief Windows Installer database reader, model and writer (msi_package.hpp).

#include "msi_package.hpp"
#include "winapi/com/ole32/storage.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"

#include <algorithm>
#include <climits>
#include <cstring>

namespace vitreo::winapi::msi {

// {000C1084-0000-0000-C000-000000000046}: the root CLSID of a database
// Windows creates (msi_database_format_probe). Transform {000C1082-...} and
// patch {000C1086-...} measured by msi_database_transform_probe.
const std::array<u8, 16> kClsidDatabase = {0x84, 0x10, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00,
                                           0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
const std::array<u8, 16> kClsidTransform = {0x82, 0x10, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00,
                                            0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
const std::array<u8, 16> kClsidPatch = {0x86, 0x10, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00,
                                        0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};

namespace {

/// The 64 characters MSI packs stream names into, in code order.
constexpr std::string_view kAlphabet =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._";

constexpr u16 kPairBase   = 0x3800; ///< Two packed characters per code unit.
constexpr u16 kSingleBase = 0x4800; ///< One character per code unit.
constexpr u16 kSingleEnd  = 0x4840;
/// Table streams carry this leading code unit as a "this is a database table"
/// marker. It sits one past the single-character range and encodes no
/// character.
constexpr u16 kTableMarker = 0x4840;

constexpr std::string_view kStringData = "_StringData";
constexpr std::string_view kStringPool = "_StringPool";
constexpr std::string_view kTables     = "_Tables";
constexpr std::string_view kColumns    = "_Columns";

/// Read a little-endian unsigned value of @p width bytes.
u32 read_le(std::span<const u8> data, usize offset, u32 width) {
    u32 value = 0;
    for (u32 k = 0; k < width; ++k) {
        if (offset + k >= data.size()) return value;
        value |= static_cast<u32>(data[offset + k]) << (8 * k);
    }
    return value;
}

void put_le(std::vector<u8>& out, u32 value, u32 width) {
    for (u32 k = 0; k < width; ++k) out.push_back(static_cast<u8>(value >> (8 * k)));
}

int alphabet_index(char c) {
    const auto pos = kAlphabet.find(c);
    return pos == std::string_view::npos ? -1 : static_cast<int>(pos);
}

/// UTF-8 <-> UTF-16 for compound-file entry names (which the CFB layer keeps
/// as UTF-8 of the UTF-16 name).
std::vector<u16> widen(const std::string& raw_name) {
    std::vector<u16> units;
    units.reserve(raw_name.size());
    usize i = 0;
    while (i < raw_name.size()) {
        const u8 b0 = static_cast<u8>(raw_name[i]);
        if ((b0 & 0x80) == 0) {
            units.push_back(b0);
            i += 1;
        } else if ((b0 & 0xE0) == 0xC0 && i + 1 < raw_name.size()) {
            units.push_back(static_cast<u16>(((b0 & 0x1Fu) << 6) |
                                             (static_cast<u8>(raw_name[i + 1]) & 0x3Fu)));
            i += 2;
        } else if ((b0 & 0xF0) == 0xE0 && i + 2 < raw_name.size()) {
            units.push_back(static_cast<u16>(((b0 & 0x0Fu) << 12) |
                                             ((static_cast<u8>(raw_name[i + 1]) & 0x3Fu) << 6) |
                                             (static_cast<u8>(raw_name[i + 2]) & 0x3Fu)));
            i += 3;
        } else {
            units.push_back(b0);
            i += 1;
        }
    }
    return units;
}

std::string narrow(const std::u16string& units) {
    std::string out;
    for (const char16_t u : units) {
        const u32 cp = u;
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }
    return out;
}

u32 stored_width(const ColumnDef& c, u32 string_ref_size) {
    if (c.is_string) return string_ref_size;
    if (c.is_stream()) return 2;
    return c.size() == 4 ? 4u : 2u;
}

constexpr char kKeySeparator = '\x1f';

std::string joined_key(std::span<const std::string> cells) {
    std::string key;
    for (usize i = 0; i < cells.size(); ++i) {
        if (i) key.push_back(kKeySeparator);
        key += cells[i];
    }
    return key;
}

bool is_system_table(const std::string& name) {
    return name == kTables || name == kColumns || name == "_Streams" || name == "_Storages";
}

} // namespace

// ---- column helpers -----------------------------------------------------------

std::string ColumnDef::definition() const {
    char letter = 'i';
    if (is_string) letter = temporary() ? 'g' : (localizable() ? 'l' : 's');
    else if (is_stream()) letter = temporary() ? 'o' : 'v';
    else letter = temporary() ? 'j' : 'i';
    if (nullable()) letter = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
    return std::string(1, letter) + std::to_string(size());
}

ColumnDef make_column(std::string name, char kind, u32 size, bool nullable, bool key,
                      bool localizable, bool temporary) {
    ColumnDef c;
    c.name = std::move(name);
    u16 type = coltype::kValid;
    if (kind == 's') {
        type |= coltype::kString | static_cast<u16>(size & 0xFF);
        if (localizable) type |= coltype::kLocalizable;
        c.is_string = true;
    } else if (kind == 'v') {
        type |= coltype::kStream;
    } else {
        type |= static_cast<u16>(size == 4 ? 4 : 2);
        if (size != 4) type |= coltype::kShort;
    }
    if (nullable) type |= coltype::kNullable;
    if (key) type |= coltype::kKey;
    if (temporary) type |= coltype::kTemporary;
    c.type = type;
    c.width = stored_width(c, 2);
    return c;
}

std::optional<i32> cell_int(const std::string& cell) {
    if (cell.empty()) return std::nullopt;
    bool neg = cell[0] == '-';
    i64 v = 0;
    for (usize i = neg ? 1 : 0; i < cell.size(); ++i) {
        if (cell[i] < '0' || cell[i] > '9') return std::nullopt;
        v = v * 10 + (cell[i] - '0');
        if (v > 0x100000000LL) return std::nullopt;
    }
    return static_cast<i32>(neg ? -v : v);
}

std::optional<usize> Table::column_index(std::string_view name) const {
    for (usize i = 0; i < columns.size(); ++i) {
        if (columns[i].name == name) return i;
    }
    // Engine readers name standard columns; tolerate case differences there.
    for (usize i = 0; i < columns.size(); ++i) {
        const std::string& n = columns[i].name;
        if (n.size() != name.size()) continue;
        bool same = true;
        for (usize k = 0; k < n.size() && same; ++k) {
            same = std::tolower(static_cast<unsigned char>(n[k])) ==
                   std::tolower(static_cast<unsigned char>(name[k]));
        }
        if (same) return i;
    }
    return std::nullopt;
}

std::optional<usize> Table::exact_column(std::string_view name) const {
    for (usize i = 0; i < columns.size(); ++i) {
        if (columns[i].name == name) return i;
    }
    return std::nullopt;
}

usize Table::key_count() const {
    usize n = 0;
    while (n < columns.size() && columns[n].key()) ++n;
    return n;
}

bool Table::temporary() const {
    for (const ColumnDef& c : columns) {
        if (!c.temporary()) return false;
    }
    return true;
}

// ---- code pages --------------------------------------------------------------------

std::string decode_codepage(std::span<const u8> bytes, u32 codepage) {
    if (bytes.empty()) return {};
    const u32 cp = codepage == 0 ? codepage::kCpAcp : codepage;
    std::vector<u16> wide(bytes.size() * 2 + 2);
    auto r = codepage::multi_byte_to_wide(cp, 0, bytes.data(), bytes.size(), wide.data(), wide.size());
    if (!r.ok()) {
        // A code page Vitreo does not convert: keep the bytes as Latin-1.
        std::u16string latin(bytes.begin(), bytes.end());
        return narrow(latin);
    }
    std::u16string units(wide.begin(), wide.begin() + r.count);
    // Surrogate pairs survive as their two units (UTF-8 of each half is not
    // valid UTF-8, so combine them).
    std::string out;
    for (usize i = 0; i < units.size(); ++i) {
        u32 c = units[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < units.size() && units[i + 1] >= 0xDC00 &&
            units[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (units[i + 1] - 0xDC00);
            ++i;
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out += narrow(std::u16string(1, static_cast<char16_t>(c)));
        }
    }
    return out;
}

std::vector<u8> encode_codepage(const std::string& utf8, u32 codepage) {
    std::u16string wide;
    for (usize i = 0; i < utf8.size();) {
        const u8 b0 = static_cast<u8>(utf8[i]);
        u32 cp = b0;
        usize n = 1;
        if ((b0 & 0xE0) == 0xC0 && i + 1 < utf8.size()) { cp = ((b0 & 0x1F) << 6) | (utf8[i + 1] & 0x3F); n = 2; }
        else if ((b0 & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
            cp = ((b0 & 0x0F) << 12) | ((utf8[i + 1] & 0x3F) << 6) | (utf8[i + 2] & 0x3F); n = 3;
        } else if ((b0 & 0xF8) == 0xF0 && i + 3 < utf8.size()) {
            cp = ((b0 & 0x07) << 18) | ((utf8[i + 1] & 0x3F) << 12) | ((utf8[i + 2] & 0x3F) << 6) |
                 (utf8[i + 3] & 0x3F);
            n = 4;
        }
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            wide.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            wide.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            wide.push_back(static_cast<char16_t>(cp));
        }
    }
    if (wide.empty()) return {};
    const u32 cp = codepage == 0 ? codepage::kCpAcp : codepage;
    std::vector<u8> out(wide.size() * 4 + 4);
    auto r = codepage::wide_to_multi_byte(cp, 0, reinterpret_cast<const u16*>(wide.data()), wide.size(),
                                          out.data(), out.size(), nullptr, false);
    if (!r.ok()) {
        std::vector<u8> latin;
        for (const char16_t u : wide) latin.push_back(u < 0x100 ? static_cast<u8>(u) : '?');
        return latin;
    }
    out.resize(r.count);
    return out;
}

// ---- stream names -------------------------------------------------------------------

std::string decode_stream_name(std::span<const u16> encoded) {
    std::string out;
    out.reserve(encoded.size() * 2);

    for (const u16 c : encoded) {
        if (c == kTableMarker) {
            continue;
        } else if (c >= kPairBase && c < kSingleBase) {
            const u16 v = static_cast<u16>(c - kPairBase);
            out.push_back(kAlphabet[v & 0x3Fu]);
            out.push_back(kAlphabet[(v >> 6) & 0x3Fu]);
        } else if (c >= kSingleBase && c < kSingleEnd) {
            out.push_back(kAlphabet[c - kSingleBase]);
        } else if (c < 0x80) {
            // Names outside the encoded range (the summary information
            // stream, for instance) are stored literally.
            out.push_back(static_cast<char>(c));
        } else {
            out += narrow(std::u16string(1, static_cast<char16_t>(c)));
        }
    }

    return out;
}

std::u16string Package::encode_stream_name(const std::string& name, bool table) {
    std::u16string out;
    if (table) out.push_back(static_cast<char16_t>(kTableMarker));
    const std::vector<u16> units = widen(name);
    // Property-set streams ("\005SummaryInformation") keep their literal
    // name, as Windows stores them (msi_database_format_probe).
    if (!table && !units.empty() && units[0] < 0x20) {
        out.assign(units.begin(), units.end());
        return out;
    }
    for (usize i = 0; i < units.size(); ++i) {
        const int a = units[i] < 0x80 ? alphabet_index(static_cast<char>(units[i])) : -1;
        if (a < 0) {
            out.push_back(static_cast<char16_t>(units[i]));
            continue;
        }
        const int b = (i + 1 < units.size() && units[i + 1] < 0x80)
                          ? alphabet_index(static_cast<char>(units[i + 1])) : -1;
        if (b >= 0) {
            out.push_back(static_cast<char16_t>(kPairBase + a + (b << 6)));
            ++i;
        } else {
            out.push_back(static_cast<char16_t>(kSingleBase + a));
        }
    }
    return out;
}

// ---- construction -----------------------------------------------------------------

Package::Package() : m_mutex(std::make_unique<std::recursive_mutex>()) {
    m_strings.emplace_back();
    m_clsid = kClsidDatabase;
}
Package::Package(Package&&) noexcept = default;
Package& Package::operator=(Package&&) noexcept = default;
Package::~Package() = default;

Package Package::create(u32 codepage) {
    Package pkg;
    pkg.m_codepage = codepage;
    // Windows pools the system tables' column names first in a new database;
    // they are never persisted, so on disk those slots are free
    // (msi_database_format_probe: user strings start at index 5).
    for (const char* s : {"Name", "Table", "Number", "Type"}) {
        pkg.m_string_ids.emplace(s, static_cast<u32>(pkg.m_strings.size()));
        pkg.m_strings.emplace_back(s);
    }
    return pkg;
}

std::string Package::pooled(u32 index) const {
    if (index == 0 || index >= m_strings.size()) return std::string{};
    return m_strings[index];
}

std::optional<Package> Package::open(std::span<const u8> image) {
    auto root = parse_compound_file(image);
    if (root == nullptr) return std::nullopt;

    Package pkg;
    pkg.m_clsid = root->clsid;
    std::map<std::string, std::vector<u8>> table_streams;

    for (const auto& [raw_name, node] : root->children) {
        if (node == nullptr) continue;
        const std::vector<u16> units = widen(raw_name);
        const std::string name = decode_stream_name(units);
        if (node->is_storage) {
            pkg.m_storages.emplace(name, node);
            continue;
        }
        if (!units.empty() && units[0] == kTableMarker) {
            table_streams.emplace(name, node->data);
        } else {
            pkg.m_streams.emplace(name, node->data);
        }
    }

    // -- string pool -------------------------------------------------------
    auto pool_it = table_streams.find(std::string(kStringPool));
    auto data_it = table_streams.find(std::string(kStringData));
    if (pool_it == table_streams.end() || data_it == table_streams.end()) {
        return std::nullopt; // not an MSI database
    }

    const std::vector<u8>& pool = pool_it->second;
    const std::vector<u8>& data = data_it->second;
    if (pool.size() < 4) return std::nullopt;

    // Entry 0 carries the codepage and the pool flags rather than a string.
    // The high bit widens every string reference to three bytes.
    const u32 header = read_le(pool, 0, 4);
    pkg.m_codepage = header & 0x7FFFFFFFu;
    pkg.m_string_ref_size = (header & 0x80000000u) != 0 ? 3u : 2u;

    // One entry per string index. A string longer than 65535 bytes takes two
    // pool entries but one index: (0, refcount) then (length low, length
    // high); the next string's index follows the long string's
    // (msi_database_bigpool_probe: "long" after a 70000-byte string).
    usize data_offset = 0;
    const usize slot_count = pool.size() / 4;
    for (usize slot = 1; slot < slot_count; ++slot) {
        const usize base = slot * 4;
        u32 length = read_le(pool, base, 2);
        const u32 refcount = read_le(pool, base + 2, 2);

        if (length == 0 && refcount != 0 && slot + 1 < slot_count) {
            const usize next = base + 4;
            length = read_le(pool, next, 2) | (read_le(pool, next + 2, 2) << 16);
            ++slot;
        } else if (length == 0) {
            pkg.m_free_slots.push_back(static_cast<u32>(pkg.m_strings.size()));
            pkg.m_strings.emplace_back();
            continue;
        }

        if (data_offset + length > data.size()) break;
        std::string s = decode_codepage(std::span<const u8>(data.data() + data_offset, length), pkg.m_codepage);
        data_offset += length;
        pkg.m_string_ids.emplace(s, static_cast<u32>(pkg.m_strings.size()));
        pkg.m_strings.push_back(std::move(s));
    }

    // -- schema ------------------------------------------------------------
    const u32 sref = pkg.m_string_ref_size;
    std::vector<std::string> names;
    if (auto it = table_streams.find(std::string(kTables)); it != table_streams.end()) {
        const std::vector<u8>& t = it->second;
        for (usize off = 0; off + sref <= t.size(); off += sref) {
            std::string name = pkg.pooled(read_le(t, off, sref));
            if (!name.empty()) names.push_back(std::move(name));
        }
    }

    std::map<std::string, std::vector<std::pair<u32, ColumnDef>>> columns;
    if (auto it = table_streams.find(std::string(kColumns)); it != table_streams.end()) {
        const std::vector<u8>& c = it->second;
        // Column-major: Table(string), Number(i2), Name(string), Type(i2).
        const u32 row_bytes = sref + 2 + sref + 2;
        const usize rows = c.size() / row_bytes;
        const usize off_number = rows * sref;
        const usize off_name = off_number + rows * 2;
        const usize off_type = off_name + rows * sref;
        for (usize r = 0; r < rows; ++r) {
            const std::string table_name = pkg.pooled(read_le(c, r * sref, sref));
            if (table_name.empty()) continue;
            ColumnDef def;
            def.name = pkg.pooled(read_le(c, off_name + r * sref, sref));
            const u32 number = read_le(c, off_number + r * 2, 2) ^ 0x8000u;
            def.type = static_cast<u16>(read_le(c, off_type + r * 2, 2) ^ 0x8000u);
            def.is_string = (def.type & coltype::kString) == coltype::kString;
            def.width = stored_width(def, sref);
            columns[table_name].emplace_back(number, std::move(def));
        }
    }

    for (const std::string& name : names) {
        TableSlot slot;
        slot.table = std::make_unique<Table>();
        slot.table->name = name;
        auto cols = columns[name];
        std::stable_sort(cols.begin(), cols.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        for (auto& [n, def] : cols) slot.table->columns.push_back(std::move(def));
        if (auto it = table_streams.find(name); it != table_streams.end()) slot.raw = std::move(it->second);
        pkg.m_tables.emplace(name, std::move(slot));
        pkg.m_table_names.push_back(name);
    }

    return pkg;
}

bool Package::decode_table(TableSlot& s) {
    if (s.loaded) return true;
    s.loaded = true;
    Table& table = *s.table;
    if (!s.raw.has_value()) return true;  // a declared table with no rows

    u32 row_bytes = 0;
    for (ColumnDef& col : table.columns) {
        col.width = stored_width(col, m_string_ref_size);
        row_bytes += col.width;
    }
    if (row_bytes == 0) return true;

    const std::vector<u8>& raw = *s.raw;
    const usize rows = raw.size() / row_bytes;
    table.rows.assign(rows, std::vector<std::string>(table.columns.size()));
    table.row_temporary.assign(rows, 0);

    // Column-major: walk one column at a time, filling that cell in each row.
    usize offset = 0;
    std::vector<usize> stream_columns;
    for (usize c = 0; c < table.columns.size(); ++c) {
        const ColumnDef& col = table.columns[c];
        for (usize r = 0; r < rows; ++r) {
            const u32 value = read_le(raw, offset + r * col.width, col.width);
            if (col.is_string) {
                table.rows[r][c] = pooled(value);
            } else if (col.is_stream()) {
                // A present stream is a non-zero cell; the bytes are the
                // `Table.Key` stream, named once the keys are decoded.
                table.rows[r][c] = value != 0 ? std::string("\x01") : std::string{};
            } else if (value == 0) {
                table.rows[r][c].clear();
            } else {
                const i64 v = (col.width == 2) ? static_cast<i64>(value) - 0x8000
                                               : static_cast<i64>(value) - 0x80000000LL;
                table.rows[r][c] = std::to_string(v);
            }
        }
        if (col.is_stream()) stream_columns.push_back(c);
        offset += rows * col.width;
    }
    for (const usize c : stream_columns) {
        for (usize r = 0; r < rows; ++r) {
            if (!table.rows[r][c].empty()) table.rows[r][c] = stream_key(table, r);
        }
    }
    table.sorted = true;
    for (usize r = 1; r < rows && table.sorted; ++r) table.sorted = compare_keys(table, table.rows[r - 1], table.rows[r]) < 0;
    s.raw.reset();
    return true;
}

void Package::mark_unloadable(const std::string& name) {
    auto it = m_tables.find(name);
    if (it != m_tables.end()) it->second.unloadable = true;
}

bool Package::unloadable(const std::string& name) const {
    auto it = m_tables.find(name);
    return it != m_tables.end() && it->second.present && it->second.unloadable;
}

Package::TableSlot* Package::slot(const std::string& name) {
    auto it = m_tables.find(name);
    if (it == m_tables.end() || !it->second.present || it->second.unloadable) return nullptr;
    (void)decode_table(it->second);
    return &it->second;
}

const Table* Package::table(const std::string& name) { return find_table(name); }

Table* Package::find_table(const std::string& name) {
    TableSlot* s = slot(name);
    return s ? s->table.get() : nullptr;
}

void Package::load_all_tables() {
    for (auto& [name, s] : m_tables) {
        if (s.present) (void)decode_table(s);
    }
}

// ---- streams and storages -------------------------------------------------------

const std::vector<u8>* Package::stream(const std::string& name) const {
    auto it = m_streams.find(name);
    return (it == m_streams.end()) ? nullptr : &it->second;
}

std::vector<std::string> Package::stream_names() const {
    std::vector<std::string> out;
    for (const auto& [name, bytes] : m_streams) out.push_back(name);
    return out;
}

void Package::set_stream(const std::string& name, std::vector<u8> bytes) {
    m_streams[name] = std::move(bytes);
}

bool Package::remove_stream(const std::string& name) { return m_streams.erase(name) != 0; }

std::vector<std::string> Package::storage_names() const {
    std::vector<std::string> out;
    for (const auto& [name, node] : m_storages) out.push_back(name);
    return out;
}

std::optional<std::vector<u8>> Package::storage_image(const std::string& name) const {
    auto it = m_storages.find(name);
    if (it == m_storages.end()) return std::nullopt;
    StorageNode root = *it->second;
    root.is_storage = true;
    return serialize_compound_file(root);
}

bool Package::set_storage_image(const std::string& name, std::span<const u8> image) {
    auto root = parse_compound_file(image);
    if (!root) return false;
    root->name = narrow(encode_stream_name(name, false));
    root->is_storage = true;
    m_storages[name] = root;
    return true;
}

bool Package::remove_storage(const std::string& name) { return m_storages.erase(name) != 0; }

// ---- strings ----------------------------------------------------------------------

std::optional<u32> Package::string_id(const std::string& s) const {
    if (s.empty()) return 0u;
    auto it = m_string_ids.find(s);
    if (it == m_string_ids.end()) return std::nullopt;
    return it->second;
}

u32 Package::intern(const std::string& s) {
    if (s.empty()) return 0;
    if (auto it = m_string_ids.find(s); it != m_string_ids.end()) return it->second;
    u32 id = 0;
    if (!m_free_slots.empty()) {
        id = m_free_slots.front();
        m_free_slots.erase(m_free_slots.begin());
        m_strings[id] = s;
    } else {
        id = static_cast<u32>(m_strings.size());
        m_strings.push_back(s);
    }
    m_string_ids.emplace(s, id);
    return id;
}

i64 Package::order_key(const ColumnDef& column, const std::string& cell) const {
    if (cell.empty()) return LLONG_MIN;
    if (column.is_string) {
        auto id = string_id(cell);
        return id ? static_cast<i64>(*id) : static_cast<i64>(UINT_MAX) + 1;
    }
    if (column.is_stream()) return 1;
    auto v = cell_int(cell);
    return v ? *v : 0;
}

int Package::compare_keys(const Table& table, const std::vector<std::string>& a,
                          const std::vector<std::string>& b) const {
    const usize keys = table.key_count();
    for (usize k = 0; k < keys && k < a.size() && k < b.size(); ++k) {
        const i64 ka = order_key(table.columns[k], a[k]);
        const i64 kb = order_key(table.columns[k], b[k]);
        if (ka != kb) return ka < kb ? -1 : 1;
    }
    return 0;
}

// ---- schema mutations ---------------------------------------------------------------

void Package::touch(Table& table) { m_key_index.erase(&table); }

void Package::add_table_name(const std::string& name) {
    // _Tables rows are sorted by their key's pool index.
    const u32 id = intern(name);
    auto pos = std::find_if(m_table_names.begin(), m_table_names.end(), [&](const std::string& n) {
        auto other = string_id(n);
        return other && *other > id;
    });
    m_table_names.insert(pos, name);
}

void Package::remove_table_name(const std::string& name) {
    m_table_names.erase(std::remove(m_table_names.begin(), m_table_names.end(), name), m_table_names.end());
}

u32 Package::create_table(const std::string& name, std::vector<ColumnDef> columns, bool hold,
                          DbError* err) {
    std::lock_guard lock(*m_mutex);
    if (find_table(name) != nullptr || is_system_table(name)) {
        if (err) *err = DbError{2204, name, {}};
        return kDbBadQuery;
    }
    bool all_temporary = true;
    for (const ColumnDef& c : columns) all_temporary = all_temporary && c.temporary();
    // A table of temporary columns lives only while held (Learn "SQL
    // Syntax"; measured: CREATE without HOLD succeeds, the table is then
    // unknown).
    if (all_temporary && !hold) return kDbOk;

    auto it = m_tables.find(name);
    if (it == m_tables.end()) it = m_tables.emplace(name, TableSlot{}).first;
    TableSlot& s = it->second;
    if (!s.table) s.table = std::make_unique<Table>();
    Table& t = *s.table;
    t = Table{};
    t.name = name;
    s.loaded = true;
    s.present = true;
    s.raw.reset();
    add_table_name(name);
    for (ColumnDef& c : columns) {
        if (!c.temporary()) intern(c.name);
        c.width = stored_width(c, m_string_ref_size);
        t.columns.push_back(std::move(c));
    }
    t.hold_count = hold ? 1 : 0;
    ++m_schema_generation;
    return kDbOk;
}

u32 Package::drop_table(const std::string& name, DbError* err) {
    std::lock_guard lock(*m_mutex);
    if (is_system_table(name)) {
        if (err) *err = DbError{2205, name, {}};
        return kDbFunctionFailed;
    }
    Table* t = find_table(name);
    if (t == nullptr) {
        if (err) *err = DbError{2228, name, {}};
        return kDbBadQuery;
    }
    for (usize c = 0; c < t->columns.size(); ++c) {
        if (!t->columns[c].is_stream()) continue;
        for (const auto& row : t->rows) {
            if (!row[c].empty()) m_streams.erase(row[c]);
        }
    }
    touch(*t);
    t->rows.clear();
    t->row_temporary.clear();
    t->columns.clear();
    t->hold_count = 0;
    m_tables[name].present = false;
    remove_table_name(name);
    ++m_schema_generation;
    return kDbOk;
}

u32 Package::add_column(const std::string& table, ColumnDef column, bool hold, DbError* err) {
    std::lock_guard lock(*m_mutex);
    Table* t = find_table(table);
    if (t == nullptr) {
        if (err) *err = DbError{2228, table, {}};
        return kDbBadQuery;
    }
    if (t->column_index(column.name) && t->columns[*t->column_index(column.name)].name == column.name) {
        if (err) *err = DbError{2242, column.name, {}};
        return kDbBadQuery;
    }
    // A temporary column added to an unheld table is never accessible
    // (msi_database_format_probe: ALTER ... ADD `TMP` SHORT TEMPORARY).
    if (column.temporary() && !hold && t->hold_count == 0) return kDbOk;
    // Existing rows would hold null in a NOT NULL column; an OBJECT column
    // cannot be added to a table holding rows (measured: both fail 2280).
    if (!t->rows.empty() && (!column.nullable() || column.is_stream())) {
        if (err) *err = DbError{2280, column.name, table};
        return kDbFunctionFailed;
    }
    usize at = t->columns.size();
    if (!column.temporary()) {
        // Persistent columns precede temporary ones (Learn "SQL Syntax").
        for (usize i = 0; i < t->columns.size(); ++i) {
            if (t->columns[i].temporary()) { at = i; break; }
        }
        intern(column.name);
    }
    column.width = stored_width(column, m_string_ref_size);
    t->columns.insert(t->columns.begin() + static_cast<std::ptrdiff_t>(at), std::move(column));
    for (auto& row : t->rows) row.insert(row.begin() + static_cast<std::ptrdiff_t>(at), std::string{});
    if (hold) ++t->hold_count;
    ++m_schema_generation;
    return kDbOk;
}

u32 Package::hold_table(const std::string& table, DbError* err) {
    std::lock_guard lock(*m_mutex);
    Table* t = find_table(table);
    if (t == nullptr) {
        if (err) *err = DbError{2228, table, {}};
        return kDbBadQuery;
    }
    ++t->hold_count;
    return kDbOk;
}

u32 Package::free_table(const std::string& table, DbError* err) {
    std::lock_guard lock(*m_mutex);
    Table* t = find_table(table);
    if (t == nullptr) {
        if (err) *err = DbError{2228, table, {}};
        return kDbBadQuery;
    }
    if (t->hold_count == 0) return kDbOk;  // measured: FREE of an unheld table succeeds
    if (--t->hold_count == 0 && t->temporary()) {
        touch(*t);
        t->rows.clear();
        t->row_temporary.clear();
        t->columns.clear();
        m_tables[table].present = false;
        remove_table_name(table);
        ++m_schema_generation;
    }
    return kDbOk;
}

// ---- rows -------------------------------------------------------------------------

std::optional<usize> Package::find_row(const Table& table, std::span<const std::string> key) const {
    const usize keys = table.key_count();
    if (keys == 0 || key.size() < keys || table.rows.empty()) return std::nullopt;
    if (table.sorted) {
        std::vector<i64> want(keys);
        for (usize k = 0; k < keys; ++k) {
            const ColumnDef& col = table.columns[k];
            if (col.is_string && !key[k].empty() && !string_id(key[k])) return std::nullopt;  // never pooled
            want[k] = order_key(col, key[k]);
        }
        usize lo = 0, hi = table.rows.size();
        while (lo < hi) {
            const usize mid = (lo + hi) / 2;
            int c = 0;
            for (usize k = 0; k < keys && c == 0; ++k) {
                const i64 v = order_key(table.columns[k], table.rows[mid][k]);
                if (v != want[k]) c = v < want[k] ? -1 : 1;
            }
            if (c < 0) lo = mid + 1;
            else hi = mid;
        }
        if (lo < table.rows.size()) {
            bool same = true;
            for (usize k = 0; k < keys && same; ++k) {
                same = table.columns[k].is_string ? table.rows[lo][k] == key[k]
                                                  : order_key(table.columns[k], table.rows[lo][k]) == want[k];
            }
            if (same) return lo;
        }
        return std::nullopt;
    }
    auto& index = m_key_index[&table];
    if (index.empty()) {
        for (usize r = 0; r < table.rows.size(); ++r) {
            index.emplace(joined_key(std::span<const std::string>(table.rows[r].data(), keys)), r);
        }
    }
    auto it = index.find(joined_key(key.first(keys)));
    if (it == index.end() || it->second >= table.rows.size()) return std::nullopt;
    return it->second;
}

std::string Package::stream_key(const Table& table, usize row) const {
    std::string name = table.name;
    const usize keys = table.key_count();
    for (usize k = 0; k < keys && row < table.rows.size(); ++k) {
        name.push_back('.');
        name += table.rows[row][k];
    }
    return name;
}

u32 Package::insert_row(Table& table, std::vector<std::string> cells, bool temporary,
                        const std::map<usize, std::vector<u8>>* streams, usize* index, DbError* err) {
    std::lock_guard lock(*m_mutex);
    if (cells.size() != table.columns.size()) cells.resize(table.columns.size());
    for (usize c = 0; c < table.columns.size(); ++c) {
        const ColumnDef& col = table.columns[c];
        if (col.is_stream()) {
            const bool has = streams != nullptr && streams->contains(c);
            cells[c] = has ? std::string("\x01") : std::string{};
        } else if (col.is_integer() && !cells[c].empty()) {
            auto v = cell_int(cells[c]);
            const bool in_range = v.has_value() &&
                (col.size() == 4 || (*v >= -32768 && *v <= 32767));
            if (!in_range) {
                if (err) *err = DbError{2259, {}, {}};
                return kDbFunctionFailed;
            }
            // The biased encoding's zero is null: the minimum value reads back
            // null (measured -32768 / -2147483648).
            const bool is_min = col.size() == 4 ? *v == INT32_MIN : *v == -32768;
            cells[c] = is_min ? std::string{} : std::to_string(*v);
        }
        if (cells[c].empty() && !col.nullable()) {
            if (err) *err = DbError{2259, {}, {}};
            return kDbFunctionFailed;
        }
    }
    if (find_row(table, std::span<const std::string>(cells.data(), table.key_count()))) {
        if (err) *err = DbError{2259, {}, {}};
        return kDbFunctionFailed;
    }
    // Strings are pooled last column first (msi_database_format_probe: the
    // pool order of one row's new strings).
    for (usize c = table.columns.size(); c-- > 0;) {
        if (table.columns[c].is_string) intern(cells[c]);
    }
    const auto pos = std::upper_bound(table.rows.begin(), table.rows.end(), cells,
                                      [&](const std::vector<std::string>& a, const std::vector<std::string>& b) {
                                          return compare_keys(table, a, b) < 0;
                                      });
    const usize at = static_cast<usize>(pos - table.rows.begin());
    table.rows.insert(pos, std::move(cells));
    table.row_temporary.insert(table.row_temporary.begin() + static_cast<std::ptrdiff_t>(at), temporary ? 1 : 0);
    touch(table);
    for (usize c = 0; c < table.columns.size(); ++c) {
        if (!table.columns[c].is_stream() || table.rows[at][c].empty()) continue;
        const std::string name = stream_key(table, at);
        table.rows[at][c] = name;
        m_streams[name] = streams->at(c);
    }
    if (index) *index = at;
    return kDbOk;
}

u32 Package::update_cell(Table& table, usize row, usize column, std::string value,
                         const std::vector<u8>* stream, DbError* err) {
    std::lock_guard lock(*m_mutex);
    if (row >= table.rows.size() || column >= table.columns.size()) return kDbFunctionFailed;
    const ColumnDef& col = table.columns[column];
    if (col.key()) {
        if (err) *err = DbError{2259, {}, {}};
        return kDbFunctionFailed;
    }
    if (col.is_stream()) {
        std::string& cell = table.rows[row][column];
        const std::string name = stream_key(table, row);
        if (stream != nullptr) {
            m_streams[name] = *stream;
            cell = name;
        } else if (value.empty()) {
            if (!col.nullable()) {
                if (err) *err = DbError{2259, {}, {}};
                return kDbFunctionFailed;
            }
            m_streams.erase(name);
            cell.clear();
        }
        return kDbOk;
    }
    if (col.is_integer() && !value.empty()) {
        auto v = cell_int(value);
        if (!v || (col.size() != 4 && (*v < -32768 || *v > 32767))) {
            if (err) *err = DbError{2259, {}, {}};
            return kDbFunctionFailed;
        }
        const bool is_min = col.size() == 4 ? *v == INT32_MIN : *v == -32768;
        value = is_min ? std::string{} : std::to_string(*v);
    }
    if (value.empty() && !col.nullable()) {
        if (err) *err = DbError{2259, {}, {}};
        return kDbFunctionFailed;
    }
    if (col.is_string && !col.temporary()) intern(value);
    table.rows[row][column] = std::move(value);
    return kDbOk;
}

u32 Package::delete_row(Table& table, usize row) {
    std::lock_guard lock(*m_mutex);
    if (row >= table.rows.size()) return kDbFunctionFailed;
    for (usize c = 0; c < table.columns.size(); ++c) {
        if (table.columns[c].is_stream() && !table.rows[row][c].empty()) m_streams.erase(table.rows[row][c]);
    }
    table.rows.erase(table.rows.begin() + static_cast<std::ptrdiff_t>(row));
    if (row < table.row_temporary.size())
        table.row_temporary.erase(table.row_temporary.begin() + static_cast<std::ptrdiff_t>(row));
    touch(table);
    return kDbOk;
}

// ---- writer -------------------------------------------------------------------------

std::vector<u8> Package::encode_string_pool_data() const {
    std::vector<u8> data;
    for (usize i = 1; i < m_strings.size(); ++i) {
        const std::vector<u8> bytes = encode_codepage(m_strings[i], m_codepage);
        data.insert(data.end(), bytes.begin(), bytes.end());
    }
    return data;
}

std::vector<u8> Package::serialize() {
    std::lock_guard lock(*m_mutex);
    load_all_tables();

    // Persistent content only: temporary tables, columns and rows never
    // reach the file (msi_database_format_probe: reopened after commit).
    struct Out {
        const Table* table;
        std::vector<usize> columns;
        std::vector<usize> rows;
    };
    std::vector<Out> outs;
    std::vector<u32> refcount(m_strings.size(), 0);
    auto ref = [&](const std::string& s) {
        if (s.empty()) return;
        auto id = string_id(s);
        if (id && *id < refcount.size()) ++refcount[*id];
    };
    for (const std::string& name : m_table_names) {
        const Table* t = find_table(name);
        if (t == nullptr || t->temporary()) continue;
        Out o{t, {}, {}};
        for (usize c = 0; c < t->columns.size(); ++c) {
            if (!t->columns[c].temporary()) o.columns.push_back(c);
        }
        for (usize r = 0; r < t->rows.size(); ++r) {
            if (r < t->row_temporary.size() && t->row_temporary[r]) continue;
            o.rows.push_back(r);
        }
        ref(name);  // _Tables
        for (usize n = 0; n < o.columns.size(); ++n) {
            ref(name);  // _Columns.Table
            ref(t->columns[o.columns[n]].name);
        }
        for (const usize r : o.rows) {
            for (const usize c : o.columns) {
                if (t->columns[c].is_string) ref(t->rows[r][c]);
            }
        }
        outs.push_back(std::move(o));
    }

    u32 max_id = 0;
    for (u32 i = 1; i < refcount.size(); ++i) {
        if (refcount[i]) max_id = i;
    }
    const u32 sref = max_id > 0xFFFF ? 3u : 2u;

    // _StringPool / _StringData.
    std::vector<u8> pool, data;
    put_le(pool, m_codepage | (sref == 3 ? 0x80000000u : 0u), 4);
    for (u32 i = 1; i <= max_id; ++i) {
        if (refcount[i] == 0) {
            put_le(pool, 0, 4);
            continue;
        }
        const std::vector<u8> bytes = encode_codepage(m_strings[i], m_codepage);
        const u32 count = std::min<u32>(refcount[i], 0xFFFF);
        if (bytes.size() > 0xFFFF) {
            put_le(pool, 0, 2);
            put_le(pool, count, 2);
            put_le(pool, static_cast<u32>(bytes.size()), 4);
        } else {
            put_le(pool, static_cast<u32>(bytes.size()), 2);
            put_le(pool, count, 2);
        }
        data.insert(data.end(), bytes.begin(), bytes.end());
    }

    auto id_of = [&](const std::string& s) -> u32 { auto id = string_id(s); return id ? *id : 0; };

    StorageNode root;
    root.is_storage = true;
    root.name = "Root Entry";
    root.clsid = m_clsid;
    auto add = [&](const std::string& name, bool table, std::vector<u8> bytes) {
        auto node = std::make_shared<StorageNode>();
        node->name = narrow(encode_stream_name(name, table));
        node->data = std::move(bytes);
        root.children[node->name] = node;
    };
    add(std::string(kStringPool), true, std::move(pool));
    add(std::string(kStringData), true, std::move(data));

    std::vector<u8> tables_stream;
    struct ColRow { u32 table; u32 number; u32 name; u16 type; };
    std::vector<ColRow> colrows;
    for (const Out& o : outs) {
        const Table& t = *o.table;
        put_le(tables_stream, id_of(t.name), sref);
        for (usize n = 0; n < o.columns.size(); ++n) {
            colrows.push_back({id_of(t.name), static_cast<u32>(n + 1),
                               id_of(t.columns[o.columns[n]].name), t.columns[o.columns[n]].type});
        }
        if (o.rows.empty()) continue;
        std::vector<u8> s;
        for (const usize c : o.columns) {
            const ColumnDef& col = t.columns[c];
            const u32 w = stored_width(col, sref);
            for (const usize r : o.rows) {
                const std::string& cell = t.rows[r][c];
                u32 v = 0;
                if (col.is_string) v = id_of(cell);
                else if (col.is_stream()) v = cell.empty() ? 0 : 1;
                else if (!cell.empty()) {
                    const i32 iv = cell_int(cell).value_or(0);
                    v = w == 2 ? static_cast<u32>(iv + 0x8000) : static_cast<u32>(iv) + 0x80000000u;
                }
                put_le(s, v, w);
            }
        }
        add(t.name, true, std::move(s));
    }
    add(std::string(kTables), true, std::move(tables_stream));
    std::vector<u8> cs;
    for (const ColRow& c : colrows) put_le(cs, c.table, sref);
    for (const ColRow& c : colrows) put_le(cs, c.number ^ 0x8000u, 2);
    for (const ColRow& c : colrows) put_le(cs, c.name, sref);
    for (const ColRow& c : colrows) put_le(cs, static_cast<u32>(c.type & 0x7FFF) ^ 0x8000u, 2);
    add(std::string(kColumns), true, std::move(cs));

    for (const auto& [name, bytes] : m_streams) add(name, false, bytes);
    for (const auto& [name, node] : m_storages) {
        auto copy = std::make_shared<StorageNode>(*node);
        copy->is_storage = true;
        copy->name = narrow(encode_stream_name(name, false));
        root.children[copy->name] = copy;
    }
    return serialize_compound_file(root);
}

} // namespace vitreo::winapi::msi
