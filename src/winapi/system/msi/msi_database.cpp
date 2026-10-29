// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_database.cpp
/// @brief Windows Installer engine area: database, view, record, summary
/// information exports (msi_database.hpp).
///
/// Contract sources: MsiQuery.h (SDK 10.0.26100), Learn "Database Functions",
/// "Record Processing Functions", "Summary Information Stream Functions",
/// MsiViewModify / MsiGetLastErrorRecord / MsiOpenDatabase pages, and the
/// tools/oracle_probe/msi_database_*_probe.c measurements (x86 == x64).
/// Gaps: .docs/contracts/msi/database.md.

#include "winapi/system/msi/msi_database.hpp"
#include "winapi/system/msi/msi_core.hpp"
#include "winapi/system/msi/msi.hpp"
#include "winapi/system/msi/msi_engine.hpp"
#include "winapi/system/msi/msi_validation.hpp"
#include "winapi/system/msi/msi_remote.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "capsule/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstring>

namespace vitreo::winapi::msi {

namespace {

// ---- error codes (winerror.h) ---------------------------------------------------------
constexpr u32 kOk = 0;
constexpr u32 kInvalidHandle = 6;
constexpr u32 kInvalidData = 13;
constexpr u32 kOpenFailed = 110;
constexpr u32 kInvalidParameter = 87;
constexpr u32 kBadPathname = 161;
constexpr u32 kMoreData = 234;
constexpr u32 kNoMoreItems = 259;
constexpr u32 kBadQuery = 1615;
constexpr u32 kUnknownProperty = 1608;
constexpr u32 kPackageOpenFailed = 1619;
constexpr u32 kPackageInvalid = 1620;
constexpr u32 kFunctionFailed = 1627;
constexpr u32 kInvalidTable = 1628;
constexpr u32 kDatatypeMismatch = 1629;
constexpr u32 kInvalidDatatype = 1804;
constexpr u32 kNullInteger = 0x80000000u;

constexpr u32 kVtEmpty = 0, kVtI2 = 2, kVtI4 = 3, kVtLpstr = 30, kVtFiletime = 64;

// ---- strings ------------------------------------------------------------------------------

std::u16string to_u16(const std::string& s) {
    std::u16string out;
    for (usize i = 0; i < s.size();) {
        const u8 b0 = static_cast<u8>(s[i]);
        u32 cp = b0;
        usize n = 1;
        if ((b0 & 0xE0) == 0xC0 && i + 1 < s.size()) { cp = ((b0 & 0x1F) << 6) | (s[i + 1] & 0x3F); n = 2; }
        else if ((b0 & 0xF0) == 0xE0 && i + 2 < s.size()) { cp = ((b0 & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); n = 3; }
        else if ((b0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
            cp = ((b0 & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); n = 4;
        }
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::string from_u16(const std::u16string& w) {
    std::string out;
    for (usize i = 0; i < w.size(); ++i) {
        u32 c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < w.size() && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00);
            ++i;
        }
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else if (c < 0x800) { out.push_back(static_cast<char>(0xC0 | (c >> 6))); out.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
        else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

/// ANSI (CP_ACP) bytes -> UTF-8.
std::string acp_to_utf8(const std::string& bytes) {
    return decode_codepage(std::span<const u8>(reinterpret_cast<const u8*>(bytes.data()), bytes.size()), 0);
}

/// Read an LPCSTR / LPCWSTR argument as UTF-8; nullopt for a null pointer.
std::optional<std::string> in_str(Marshaler& m, GuestAddr p, bool wide) {
    if (p == 0) return std::nullopt;
    if (wide) {
        auto r = m.read_wide_string(p);
        if (r.is_err()) return std::nullopt;
        return r.value();
    }
    auto r = m.read_ansi_string(p);
    if (r.is_err()) return std::nullopt;
    return acp_to_utf8(r.value());
}

/// The record-string output convention (measured, msi_database_record_probe):
/// pcch null -> ERROR_INVALID_PARAMETER; buffer null -> *pcch = length, success;
/// capacity <= length -> copy capacity-1 units + NUL (nothing for capacity 0),
/// *pcch = length, ERROR_MORE_DATA; else copy, *pcch = length.
u32 out_str(Marshaler& m, GuestAddr buf, GuestAddr pcch, const std::string& utf8, bool wide) {
    if (pcch == 0) return kInvalidParameter;
    auto cap_r = m.read_u32(pcch);
    if (cap_r.is_err()) return kInvalidParameter;
    const u32 cap = cap_r.value();
    if (wide) {
        const std::u16string w = to_u16(utf8);
        const u32 len = static_cast<u32>(w.size());
        if (buf == 0) { (void)m.write_u32(pcch, len); return kOk; }
        if (cap <= len) {
            if (cap > 0) {
                for (u32 k = 0; k + 1 < cap; ++k) (void)m.write_u16(buf + k * 2, w[k]);
                (void)m.write_u16(buf + (cap - 1) * 2, 0);
            }
            (void)m.write_u32(pcch, len);
            return kMoreData;
        }
        for (u32 k = 0; k < len; ++k) (void)m.write_u16(buf + k * 2, w[k]);
        (void)m.write_u16(buf + len * 2, 0);
        (void)m.write_u32(pcch, len);
        return kOk;
    }
    const std::vector<u8> a = encode_codepage(utf8, 0);
    const u32 len = static_cast<u32>(a.size());
    if (buf == 0) { (void)m.write_u32(pcch, len); return kOk; }
    if (cap <= len) {
        if (cap > 0) {
            for (u32 k = 0; k + 1 < cap; ++k) (void)m.write_u8(buf + k, a[k]);
            (void)m.write_u8(buf + cap - 1, 0);
        }
        (void)m.write_u32(pcch, len);
        return kMoreData;
    }
    for (u32 k = 0; k < len; ++k) (void)m.write_u8(buf + k, a[k]);
    (void)m.write_u8(buf + len, 0);
    (void)m.write_u32(pcch, len);
    return kOk;
}

// ---- files through the capsule VFS ------------------------------------------------------

std::optional<std::vector<u8>> read_file(MsiManager& mgr, const std::string& path) {
    capsule::Vfs* vfs = mgr.vfs();
    if (vfs == nullptr) return std::nullopt;
    auto opened = vfs->open(path, capsule::AccessMode::Read, capsule::ShareMode::Read,
                            capsule::CreationDisposition::OpenExisting);
    if (opened.is_err()) return std::nullopt;
    std::vector<u8> bytes;
    std::vector<u8> chunk(1 << 20);
    for (;;) {
        auto n = vfs->read(opened.value(), chunk);
        if (n.is_err() || n.value() == 0) break;
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n.value()));
    }
    (void)vfs->close(opened.value());
    return bytes;
}

bool write_file(MsiManager& mgr, const std::string& path, std::span<const u8> bytes) {
    capsule::Vfs* vfs = mgr.vfs();
    if (vfs == nullptr) return false;
    auto opened = vfs->open(path, capsule::AccessMode::Write, capsule::ShareMode::None,
                            capsule::CreationDisposition::CreateAlways);
    if (opened.is_err()) return false;
    bool ok = true;
    usize written = 0;
    while (written < bytes.size()) {
        auto n = vfs->write(opened.value(), bytes.subspan(written));
        if (n.is_err() || n.value() == 0) { ok = false; break; }
        written += n.value();
    }
    (void)vfs->close(opened.value());
    return ok;
}

void remove_file(MsiManager& mgr, const std::string& path) {
    if (capsule::Vfs* vfs = mgr.vfs()) (void)vfs->remove(path);
}

// ---- the per-process error record -------------------------------------------------------

std::mutex g_error_mutex;
std::shared_ptr<Record> g_error_record;

std::atomic<i32> g_next_cookie{1};

// ---- conversions between cells and record fields -------------------------------------------

void value_to_field(Package& pkg, const sql::Value& v, Record::Field& f) {
    f = Record::Field{};
    switch (v.kind) {
    case sql::Value::Kind::Null: break;
    case sql::Value::Kind::Int: f.kind = Record::Kind::Integer; f.integer = v.i; break;
    case sql::Value::Kind::Str: f.kind = Record::Kind::String; f.text = to_u16(v.s); break;
    case sql::Value::Kind::Stream:
        f.kind = Record::Kind::Stream;
        if (const auto* bytes = pkg.stream(v.s)) f.stream = *bytes;
        break;
    }
}

void cell_to_field(Package& pkg, const ColumnDef& col, const std::string& cell, Record::Field& f) {
    value_to_field(pkg, sql::cell_value(col, cell), f);
}

/// A record field as a cell of @p col. MsiViewModify is lenient: a string
/// that is not a number reads as null in an integer column (measured).
std::string field_to_cell(const ColumnDef& col, const Record::Field& f) {
    switch (f.kind) {
    case Record::Kind::Null: return {};
    case Record::Kind::Integer:
        if (f.integer == INT32_MIN) return {};
        return std::to_string(f.integer);
    case Record::Kind::String: {
        const std::string s = from_u16(f.text);
        if (col.is_integer()) {
            auto n = cell_int(s);
            return n ? std::to_string(*n) : std::string{};
        }
        return s;
    }
    case Record::Kind::Stream: return std::string("\x01");
    }
    return {};
}

std::shared_ptr<DatabaseObject> db_of(MsiManager& mgr, u32 h) { return mgr.handles().get<DatabaseObject>(h); }
std::shared_ptr<ViewObject> view_of(MsiManager& mgr, u32 h) { return mgr.handles().get<ViewObject>(h); }

/// The name Windows puts in error records for a database (the path as opened).
std::string db_name(const DatabaseObject& db) { return db.path; }

void set_sql_error(const DatabaseObject& db, const sql::Error& e) {
    if (e.fields == 3) set_last_error_record(e.code, {db_name(db), e.a});
    else set_last_error_record(e.code, {db_name(db), e.a, e.b});
}

}  // namespace

// ---- public helpers ---------------------------------------------------------------------------

std::optional<std::string> db_in_str(Marshaler& m, GuestAddr p, bool wide) { return in_str(m, p, wide); }
std::optional<std::vector<u8>> db_read_file(MsiManager& mgr, const std::string& path) { return read_file(mgr, path); }
bool db_write_file(MsiManager& mgr, const std::string& path, std::span<const u8> bytes) {
    return write_file(mgr, path, bytes);
}
bool db_make_dir(MsiManager& mgr, const std::string& path) {
    capsule::Vfs* vfs = mgr.vfs();
    if (vfs == nullptr) return false;
    if (db_is_dir(mgr, path)) return true;
    return vfs->create_dir(path).is_ok();
}
bool db_is_dir(MsiManager& mgr, const std::string& path) {
    capsule::Vfs* vfs = mgr.vfs();
    if (vfs == nullptr) return false;
    auto st = vfs->stat(path);
    return st.is_ok() && (static_cast<u32>(st.value().attributes) & static_cast<u32>(capsule::FileAttributes::Directory)) != 0;
}

DatabaseObject::~DatabaseObject() {
    // MSIDBOPEN_DIRECT writes without a commit; a CREATE that was never
    // committed leaves no file (msi_database_modify_probe).
    if (manager == nullptr || package == nullptr) return;
    if (direct && dirty && !read_only) {
        std::lock_guard lock(package->mutex());
        const std::vector<u8> image = package->serialize();
        (void)write_file(*manager, output_path.empty() ? path : output_path, image);
    } else if (created && !committed && !path.empty()) {
        remove_file(*manager, path);
    }
}

std::shared_ptr<Record> record_of(MsiManager& mgr, u32 handle) { return mgr.handles().get<Record>(handle); }

u32 new_record(MsiManager& mgr, u32 count) {
    if (count > 65535) return 0;
    return mgr.handles().add(std::make_shared<Record>(count));
}

u32 add_record(MsiManager& mgr, std::shared_ptr<Record> record) { return mgr.handles().add(std::move(record)); }

std::string record_string(const Record& r, u32 field) {
    if (field > r.count()) return {};
    const Record::Field& f = r.fields[field];
    switch (f.kind) {
    case Record::Kind::Integer: return std::to_string(f.integer);
    case Record::Kind::String: return from_u16(f.text);
    default: return {};
    }
}

i32 record_integer(const Record& r, u32 field) {
    if (field > r.count()) return static_cast<i32>(kNullInteger);
    const Record::Field& f = r.fields[field];
    if (f.kind == Record::Kind::Integer) return f.integer;
    if (f.kind == Record::Kind::Stream) return static_cast<i32>(f.stream.size());
    if (f.kind != Record::Kind::String || f.text.empty()) return static_cast<i32>(kNullInteger);
    // Decimal digits with an optional leading minus, wrapping at 32 bits
    // (msi_record_read_probe.c).
    const std::u16string& t = f.text;
    const bool neg = t[0] == u'-';
    usize i = neg ? 1 : 0;
    if (i >= t.size()) return static_cast<i32>(kNullInteger);
    u32 v = 0;
    for (; i < t.size(); ++i) {
        if (t[i] < u'0' || t[i] > u'9') return static_cast<i32>(kNullInteger);
        v = v * 10u + static_cast<u32>(t[i] - u'0');
    }
    return static_cast<i32>(neg ? 0u - v : v);
}

void record_set_string(Record& r, u32 field, const std::string& utf8) {
    if (field > r.count()) return;
    Record::Field& f = r.fields[field];
    f = Record::Field{};
    if (!utf8.empty()) { f.kind = Record::Kind::String; f.text = to_u16(utf8); }
}

void record_set_integer(Record& r, u32 field, i32 value) {
    if (field > r.count()) return;
    Record::Field& f = r.fields[field];
    f = Record::Field{};
    if (value != INT32_MIN) { f.kind = Record::Kind::Integer; f.integer = value; }
}

bool record_is_null(const Record& r, u32 field) {
    return field > r.count() || r.fields[field].kind == Record::Kind::Null;
}

void set_last_error_record(u32 code, const std::vector<std::string>& args) {
    auto rec = std::make_shared<Record>(static_cast<u32>(args.size() + 1));
    record_set_integer(*rec, 1, static_cast<i32>(code));
    for (usize i = 0; i < args.size(); ++i) record_set_string(*rec, static_cast<u32>(i + 2), args[i]);
    std::lock_guard lock(g_error_mutex);
    g_error_record = std::move(rec);
}

void clear_last_error_record() {
    std::lock_guard lock(g_error_mutex);
    g_error_record.reset();
}

std::shared_ptr<Record> take_last_error_record() {
    std::lock_guard lock(g_error_mutex);
    return std::move(g_error_record);
}

void put_last_error_record(std::shared_ptr<Record> record) {
    std::lock_guard lock(g_error_mutex);
    g_error_record = std::move(record);
}

u32 open_database_handle(MsiManager& mgr, std::shared_ptr<Package> package, const std::string& path,
                         bool read_only) {
    auto db = std::make_shared<DatabaseObject>();
    db->package = std::move(package);
    db->path = path;
    db->read_only = read_only;
    return mgr.handles().add(std::move(db));
}

std::shared_ptr<DatabaseObject> database_of(MsiManager& mgr, u32 handle) { return db_of(mgr, handle); }

// ---- summary information ----------------------------------------------------------------------

namespace {

// FMTID_SummaryInformation {F29F85E0-4FF9-1068-AB91-08002B27B3D9}.
constexpr std::array<u8, 16> kFmtidSummary = {0xE0, 0x85, 0x9F, 0xF2, 0xF9, 0x4F, 0x68, 0x10,
                                              0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9};
constexpr char kSummaryStream[] = "\x05SummaryInformation";

u32 rd32(std::span<const u8> d, usize o) {
    if (o + 4 > d.size()) return 0;
    return static_cast<u32>(d[o]) | (static_cast<u32>(d[o + 1]) << 8) | (static_cast<u32>(d[o + 2]) << 16) |
           (static_cast<u32>(d[o + 3]) << 24);
}
void wr32(std::vector<u8>& d, u32 v) { for (int k = 0; k < 4; ++k) d.push_back(static_cast<u8>(v >> (8 * k))); }

/// Which VT a summary property holds (MsiQuery.h: integer 1,14,15,16,19;
/// datetime 10-13; text 2-9,18; 0 and 17 unsupported).
u32 summary_vt(u32 pid) {
    if (pid == 1) return kVtI2;
    if (pid == 14 || pid == 15 || pid == 16 || pid == 19) return kVtI4;
    if (pid >= 10 && pid <= 13) return kVtFiletime;
    if ((pid >= 2 && pid <= 9) || pid == 18) return kVtLpstr;
    return kVtEmpty;
}

}  // namespace

std::map<u32, SummaryProperty> parse_summary_information(std::span<const u8> d) {
    std::map<u32, SummaryProperty> props;
    if (d.size() < 48 || d[0] != 0xFE || d[1] != 0xFF) return props;
    const u32 sections = rd32(d, 24);
    for (u32 s = 0; s < sections && s < 4; ++s) {
        const usize entry = 28 + s * 20;
        if (entry + 20 > d.size()) break;
        if (std::memcmp(d.data() + entry, kFmtidSummary.data(), 16) != 0) continue;
        const usize base = rd32(d, entry + 16);
        const u32 count = rd32(d, base + 4);
        // The code page first: strings depend on it.
        u32 codepage = 0;
        for (u32 i = 0; i < count; ++i) {
            if (rd32(d, base + 8 + i * 8) == 1) {
                const usize off = base + rd32(d, base + 12 + i * 8);
                codepage = rd32(d, off + 4) & 0xFFFF;
            }
        }
        for (u32 i = 0; i < count; ++i) {
            const u32 pid = rd32(d, base + 8 + i * 8);
            const usize off = base + rd32(d, base + 12 + i * 8);
            if (off + 4 > d.size()) continue;
            const u32 vt = rd32(d, off) & 0xFFFF;
            SummaryProperty p;
            p.vt = vt;
            if (vt == kVtI2) p.integer = static_cast<i16>(rd32(d, off + 4) & 0xFFFF);
            else if (vt == kVtI4) p.integer = static_cast<i32>(rd32(d, off + 4));
            else if (vt == kVtFiletime) p.filetime = rd32(d, off + 4) | (static_cast<u64>(rd32(d, off + 8)) << 32);
            else if (vt == kVtLpstr) {
                u32 len = rd32(d, off + 4);
                if (off + 8 + len > d.size()) len = static_cast<u32>(d.size() - off - 8);
                while (len > 0 && d[off + 8 + len - 1] == 0) --len;
                p.text = decode_codepage(d.subspan(off + 8, len), codepage);
            } else {
                continue;
            }
            props[pid] = std::move(p);
        }
    }
    return props;
}

std::vector<u8> build_summary_information(const std::map<u32, SummaryProperty>& props) {
    u32 codepage = 0;
    if (auto it = props.find(1); it != props.end()) codepage = static_cast<u32>(it->second.integer) & 0xFFFF;
    std::vector<u8> values;
    std::vector<std::pair<u32, u32>> index;
    const u32 header = 8 + static_cast<u32>(props.size()) * 8;
    for (const auto& [pid, p] : props) {
        index.emplace_back(pid, header + static_cast<u32>(values.size()));
        wr32(values, p.vt);
        if (p.vt == kVtI2) wr32(values, static_cast<u32>(static_cast<u16>(p.integer)));
        else if (p.vt == kVtI4) wr32(values, static_cast<u32>(p.integer));
        else if (p.vt == kVtFiletime) { wr32(values, static_cast<u32>(p.filetime)); wr32(values, static_cast<u32>(p.filetime >> 32)); }
        else {
            std::vector<u8> bytes = encode_codepage(p.text, codepage);
            bytes.push_back(0);
            wr32(values, static_cast<u32>(bytes.size()));
            values.insert(values.end(), bytes.begin(), bytes.end());
            while (values.size() % 4) values.push_back(0);
        }
    }
    std::vector<u8> out;
    // Byte order 0xFFFE, format 0, OS id (Windows 6.2 / Win32, as Windows
    // writes it: msi_database_format_probe), null CLSID, one section.
    out.insert(out.end(), {0xFE, 0xFF, 0x00, 0x00, 0x06, 0x02, 0x02, 0x00});
    out.insert(out.end(), 16, 0);
    wr32(out, 1);
    out.insert(out.end(), kFmtidSummary.begin(), kFmtidSummary.end());
    wr32(out, 48);
    wr32(out, header + static_cast<u32>(values.size()));
    wr32(out, static_cast<u32>(props.size()));
    for (const auto& [pid, off] : index) { wr32(out, pid); wr32(out, off); }
    out.insert(out.end(), values.begin(), values.end());
    return out;
}

u32 get_summary_information(MsiManager& mgr, u32 database, const std::string* path, u32 update_count,
                            u32& out_handle) {
    out_handle = 0;
    auto si = std::make_shared<SummaryInfoObject>();
    si->update_count = update_count;
    if (database != 0) {
        auto db = db_of(mgr, database);
        if (!db) return kInvalidHandle;
        si->db = db;
        // A read-only database refuses changes (measured: set fails 1627).
        if (db->read_only) si->update_count = 0;
        std::lock_guard lock(db->package->mutex());
        if (const auto* s = db->package->stream(kSummaryStream)) si->properties = parse_summary_information(*s);
    } else {
        if (path == nullptr) return kInvalidParameter;
        auto bytes = read_file(mgr, *path);
        if (!bytes) return kPackageOpenFailed;
        auto pkg = Package::open(*bytes);
        if (!pkg) return kPackageInvalid;
        si->path = *path;
        if (const auto* s = pkg->stream(kSummaryStream)) si->properties = parse_summary_information(*s);
    }
    out_handle = mgr.handles().add(std::move(si));
    clear_last_error_record();
    return kOk;
}

// ---- views --------------------------------------------------------------------------------

namespace {

/// Re-find the row a fetched record names; nullopt when it is gone.
std::optional<usize> fetched_row(ViewObject& v, Package& pkg, Table& t, const Record& rec) {
    if (rec.count() < 0 || rec.fields[0].kind != Record::Kind::Integer) return std::nullopt;
    auto it = v.fetched.find(rec.fields[0].integer);
    if (it == v.fetched.end() || it->second.deleted || it->second.keys.empty()) return std::nullopt;
    return pkg.find_row(t, it->second.keys[0]);
}

std::vector<std::string> key_of(const Table& t, usize row) {
    return std::vector<std::string>(t.rows[row].begin(), t.rows[row].begin() + static_cast<std::ptrdiff_t>(t.key_count()));
}

i32 remember(ViewObject& v, Record& rec, const Table& t, usize row) {
    const i32 cookie = g_next_cookie.fetch_add(1);
    v.fetched[cookie] = FetchedRow{{key_of(t, row)}, false};
    rec.fields[0] = Record::Field{};
    rec.fields[0].kind = Record::Kind::Integer;
    rec.fields[0].integer = cookie;
    return cookie;
}

/// Fill @p rec's fields from row @p row through the view's columns.
void fill(Package& pkg, const std::vector<sql::OutputColumn>& cols, const std::vector<i32>& map, const Table& t,
          usize row, Record& rec) {
    for (usize i = 0; i < cols.size() && i + 1 <= rec.count(); ++i) {
        if (map[i] < 0) continue;
        const usize c = static_cast<usize>(map[i]);
        cell_to_field(pkg, t.columns[c], t.rows[row][c], rec.fields[i + 1]);
    }
}

/// MsiViewModify on a `_Streams` / `_Storages` view: the rows are the
/// package's streams / sub-storages, keyed by Name (Learn "_Streams Table":
/// inserting a record with a stream field adds the stream).
u32 modify_streams(ViewObject& v, i32 mode, Record& rec, bool streams) {
    Package& pkg = *v.db->package;
    const auto& cols = v.statement->columns();
    i32 name_field = -1, data_field = -1;
    for (usize i = 0; i < cols.size(); ++i) {
        if (cols[i].table < 0) continue;
        if (cols[i].name == "Name") name_field = static_cast<i32>(i + 1);
        if (cols[i].name == "Data") data_field = static_cast<i32>(i + 1);
    }
    auto exists = [&](const std::string& n) {
        if (streams) return pkg.stream(n) != nullptr;
        const auto names = pkg.storage_names();
        return std::find(names.begin(), names.end(), n) != names.end();
    };
    auto field_name = [&]() -> std::string {
        return name_field > 0 && static_cast<u32>(name_field) <= rec.count() ? record_string(rec, static_cast<u32>(name_field)) : std::string{};
    };
    auto fetched_name = [&]() -> std::optional<std::string> {
        if (rec.fields[0].kind != Record::Kind::Integer) return std::nullopt;
        auto it = v.fetched.find(rec.fields[0].integer);
        if (it == v.fetched.end() || it->second.deleted || it->second.keys.empty() || it->second.keys[0].empty()) return std::nullopt;
        if (!exists(it->second.keys[0][0])) return std::nullopt;
        return it->second.keys[0][0];
    };
    auto put = [&](const std::string& n) -> u32 {
        const Record::Field none{};
        const Record::Field& f = data_field > 0 && static_cast<u32>(data_field) <= rec.count()
                                     ? rec.fields[static_cast<usize>(data_field)] : none;
        // A row without data is accepted (measured: Data reads null on
        // Windows; here an empty stream).
        if (f.kind != Record::Kind::Stream && f.kind != Record::Kind::Null) return kFunctionFailed;
        if (streams) pkg.set_stream(n, f.stream);
        else if (!pkg.set_storage_image(n, f.stream)) return kFunctionFailed;
        v.db->dirty = true;
        return kOk;
    };
    auto remove = [&](const std::string& n) {
        if (streams) (void)pkg.remove_stream(n);
        else (void)pkg.remove_storage(n);
        v.db->dirty = true;
    };
    auto remember_name = [&](const std::string& n) {
        const i32 cookie = g_next_cookie.fetch_add(1);
        v.fetched[cookie] = FetchedRow{{{n}}, false};
        rec.fields[0] = Record::Field{};
        rec.fields[0].kind = Record::Kind::Integer;
        rec.fields[0].integer = cookie;
    };
    auto fill_from = [&](const std::string& n) {
        if (name_field > 0 && static_cast<u32>(name_field) <= rec.count()) record_set_string(rec, static_cast<u32>(name_field), n);
        if (data_field > 0 && static_cast<u32>(data_field) <= rec.count()) {
            Record::Field& f = rec.fields[static_cast<usize>(data_field)];
            f = Record::Field{};
            f.kind = Record::Kind::Stream;
            if (streams) { if (const auto* b = pkg.stream(n)) f.stream = *b; }
            else if (auto img = pkg.storage_image(n)) f.stream = *img;
        }
    };
    const bool writes = mode != -1 && mode != 0 && mode < 8;
    if (writes && v.db->read_only) return kFunctionFailed;
    switch (mode) {
    case -1: {
        const std::string n = field_name();
        if (n.empty() || !exists(n)) return kFunctionFailed;
        fill_from(n);
        remember_name(n);
        return kOk;
    }
    case 0: { auto n = fetched_name(); if (!n) return kFunctionFailed; fill_from(*n); return kOk; }
    case 1: case 7: case 5: {
        const std::string n = field_name();
        if (n.empty()) return kFunctionFailed;
        if (exists(n)) return mode == 5 ? kOk : kFunctionFailed;
        if (u32 rc = put(n)) return rc;
        remember_name(n);
        return kOk;
    }
    case 3: {
        const std::string n = field_name();
        if (n.empty()) return kFunctionFailed;
        if (u32 rc = put(n)) return rc;
        remember_name(n);
        return kOk;
    }
    case 2: case 4: {
        auto n = fetched_name();
        if (!n) return kFunctionFailed;
        const std::string want = field_name();
        if (!want.empty() && want != *n) {
            if (mode == 2) return kFunctionFailed;
            if (u32 rc = put(want)) return rc;
            remove(*n);
            remember_name(want);
            return kOk;
        }
        return put(*n);
    }
    case 6: {
        auto n = fetched_name();
        if (!n) return kFunctionFailed;
        remove(*n);
        v.fetched[rec.fields[0].integer].deleted = true;
        return kOk;
    }
    default:
        return kFunctionFailed;  // no _Validation rows describe the virtual tables
    }
}

u32 modify(MsiManager& mgr, ViewObject& v, i32 mode, Record& rec) {
    (void)mgr;
    if (mode < -1 || mode > 11) return kInvalidParameter;
    if (!v.statement || v.statement->kind() != sql::Kind::Select) return kFunctionFailed;
    if (v.statement->tables().size() != 1) return kFunctionFailed;  // joins (Learn)
    Package& pkg = *v.db->package;
    std::lock_guard lock(pkg.mutex());
    const std::string& name = v.statement->tables()[0];
    const auto& cols = v.statement->columns();
    if (name == "_Streams" || name == "_Storages") return modify_streams(v, mode, rec, name == "_Streams");
    Table* t = pkg.find_table(name);
    if (t == nullptr) return kFunctionFailed;
    // Output column -> table column (by name: the schema may have moved).
    std::vector<i32> map(cols.size(), -1);
    for (usize i = 0; i < cols.size(); ++i) {
        if (cols[i].table < 0) continue;
        if (auto c = t->exact_column(cols[i].name)) map[i] = static_cast<i32>(*c);
    }
    const usize keys = t->key_count();
    std::vector<i32> key_field(keys, -1);
    for (usize i = 0; i < map.size(); ++i) {
        if (map[i] >= 0 && static_cast<usize>(map[i]) < keys) key_field[static_cast<usize>(map[i])] = static_cast<i32>(i);
    }
    const bool all_keys = std::all_of(key_field.begin(), key_field.end(), [](i32 f) { return f >= 0; });
    const bool read_only = v.db->read_only;

    // Cells for a new or replacing row: @p base overlaid with the record.
    std::map<usize, std::vector<u8>> streams;
    auto overlay = [&](std::vector<std::string> base) {
        base.resize(t->columns.size());
        streams.clear();
        for (usize i = 0; i < map.size(); ++i) {
            if (map[i] < 0) continue;
            const usize c = static_cast<usize>(map[i]);
            const Record::Field null_field{};
            const Record::Field& f = i + 1 <= rec.count() ? rec.fields[i + 1] : null_field;
            if (t->columns[c].is_stream()) {
                if (f.kind == Record::Kind::Stream) { streams[c] = f.stream; base[c] = "\x01"; }
                else if (f.kind == Record::Kind::Null) base[c].clear();
                continue;
            }
            base[c] = field_to_cell(t->columns[c], f);
        }
        return base;
    };
    auto record_keys = [&]() {
        std::vector<std::string> k(keys);
        for (usize i = 0; i < keys; ++i) {
            const usize f = static_cast<usize>(key_field[i]) + 1;
            k[i] = f <= rec.count() ? field_to_cell(t->columns[i], rec.fields[f]) : std::string{};
        }
        return k;
    };
    auto do_insert = [&](bool temporary) -> u32 {
        if (read_only && !temporary && !t->temporary()) {
            set_last_error_record(2259, {db_name(*v.db), "", ""});
            return kFunctionFailed;
        }
        std::vector<std::string> cells = overlay({});
        for (usize i = 0; i < map.size(); ++i) {
            if (map[i] < 0) continue;
            const usize c = static_cast<usize>(map[i]);
            if (c < keys && cells[c].empty() && !t->columns[c].nullable()) {
                // Measured: a null key reports 2210 with the field's index.
                set_last_error_record(2210, {db_name(*v.db), std::to_string(i)});
                rec.fields[0] = Record::Field{};
                return kFunctionFailed;
            }
        }
        usize at = 0;
        DbError de;
        if (pkg.insert_row(*t, std::move(cells), temporary || t->temporary(), streams.empty() ? nullptr : &streams, &at, &de)) {
            rec.fields[0] = Record::Field{};
            set_last_error_record(de.code ? de.code : 2259, {db_name(*v.db), "", ""});
            return kFunctionFailed;
        }
        remember(v, rec, *t, at);
        v.db->dirty = true;
        return kOk;
    };
    auto writable_row = [&](usize row) {
        return !read_only || (row < t->row_temporary.size() && t->row_temporary[row]) || t->temporary();
    };
    auto do_update = [&](usize row) -> u32 {
        for (usize i = 0; i < map.size(); ++i) {
            if (map[i] < 0 || static_cast<usize>(map[i]) < keys) continue;
            const usize c = static_cast<usize>(map[i]);
            if (!writable_row(row) && !t->columns[c].temporary()) return kFunctionFailed;
            const Record::Field null_field{};
            const Record::Field& f = i + 1 <= rec.count() ? rec.fields[i + 1] : null_field;
            u32 rc = 0;
            if (t->columns[c].is_stream()) {
                if (f.kind == Record::Kind::Stream) rc = pkg.update_cell(*t, row, c, {}, &f.stream);
                else if (f.kind == Record::Kind::Null) rc = pkg.update_cell(*t, row, c, {}, nullptr);
            } else {
                rc = pkg.update_cell(*t, row, c, field_to_cell(t->columns[c], f));
            }
            if (rc) return kFunctionFailed;
        }
        v.db->dirty = true;
        return kOk;
    };

    switch (mode) {
    case -1: {  // MSIMODIFY_SEEK
        if (!all_keys) return kFunctionFailed;
        auto row = pkg.find_row(*t, record_keys());
        if (!row) return kFunctionFailed;
        fill(pkg, cols, map, *t, *row, rec);
        remember(v, rec, *t, *row);
        return kOk;
    }
    case 0: {  // MSIMODIFY_REFRESH
        auto row = fetched_row(v, pkg, *t, rec);
        if (!row) return kFunctionFailed;
        fill(pkg, cols, map, *t, *row, rec);
        return kOk;
    }
    case 1: return all_keys ? do_insert(false) : kFunctionFailed;   // MSIMODIFY_INSERT
    case 7: return all_keys ? do_insert(true) : kFunctionFailed;    // MSIMODIFY_INSERT_TEMPORARY
    case 2: {  // MSIMODIFY_UPDATE
        auto row = fetched_row(v, pkg, *t, rec);
        if (!row) return kFunctionFailed;
        // Key fields in the view must still name the fetched row.
        const auto current = key_of(*t, *row);
        for (usize k = 0; k < keys; ++k) {
            if (key_field[k] < 0) continue;
            const usize f = static_cast<usize>(key_field[k]) + 1;
            const std::string cell = f <= rec.count() ? field_to_cell(t->columns[k], rec.fields[f]) : std::string{};
            if (cell != current[k]) return kFunctionFailed;
        }
        return do_update(*row);
    }
    case 3: {  // MSIMODIFY_ASSIGN
        if (!all_keys) return kFunctionFailed;
        if (auto row = pkg.find_row(*t, record_keys())) {
            if (u32 rc = do_update(*row)) return rc;
            remember(v, rec, *t, *pkg.find_row(*t, record_keys()));
            return kOk;
        }
        return do_insert(false);
    }
    case 4: {  // MSIMODIFY_REPLACE
        auto row = fetched_row(v, pkg, *t, rec);
        if (!row) return kFunctionFailed;
        if (!all_keys || record_keys() == key_of(*t, *row)) return do_update(*row);
        if (!writable_row(*row)) return kFunctionFailed;
        std::vector<std::string> cells = overlay(t->rows[*row]);
        const bool temp = *row < t->row_temporary.size() && t->row_temporary[*row];
        // Keep the old row's streams for columns the record did not supply.
        for (usize c = 0; c < t->columns.size(); ++c) {
            if (t->columns[c].is_stream() && !streams.contains(c) && !cells[c].empty()) {
                if (const auto* bytes = pkg.stream(t->rows[*row][c])) streams[c] = *bytes;
            }
        }
        (void)pkg.delete_row(*t, *row);
        usize at = 0;
        if (pkg.insert_row(*t, std::move(cells), temp, streams.empty() ? nullptr : &streams, &at)) return kFunctionFailed;
        remember(v, rec, *t, at);
        v.db->dirty = true;
        return kOk;
    }
    case 5: {  // MSIMODIFY_MERGE
        if (!all_keys) return kFunctionFailed;
        if (auto row = pkg.find_row(*t, record_keys())) {
            const std::vector<std::string> want = overlay(t->rows[*row]);
            for (usize i = 0; i < map.size(); ++i) {
                if (map[i] < 0) continue;
                const usize c = static_cast<usize>(map[i]);
                if (t->columns[c].is_stream()) continue;
                if (want[c] != t->rows[*row][c]) return kFunctionFailed;
            }
            return kOk;
        }
        return do_insert(false);
    }
    case 6: {  // MSIMODIFY_DELETE
        auto row = fetched_row(v, pkg, *t, rec);
        if (!row) return kFunctionFailed;
        if (!writable_row(*row)) return kFunctionFailed;
        (void)pkg.delete_row(*t, *row);
        v.fetched[rec.fields[0].integer].deleted = true;
        v.db->dirty = true;
        return kOk;
    }
    default: {  // MSIMODIFY_VALIDATE / _NEW / _FIELD / _DELETE
        std::optional<usize> row;
        if (mode != 9) {
            row = fetched_row(v, pkg, *t, rec);
            if (!row) return kFunctionFailed;
        }
        std::vector<std::string> cells = overlay(row ? t->rows[*row] : std::vector<std::string>{});
        std::vector<bool> present(t->columns.size(), false);
        for (usize i = 0; i < map.size(); ++i) {
            if (map[i] < 0) continue;
            const bool set = i + 1 <= rec.count() && rec.fields[i + 1].kind != Record::Kind::Null;
            present[static_cast<usize>(map[i])] = mode == 10 ? set : true;
        }
        v.errors.clear();
        ValidateMode vm = mode == 8 ? ValidateMode::Existing
                        : mode == 9 ? ValidateMode::New
                        : mode == 10 ? ValidateMode::Field : ValidateMode::Delete;
        const u32 rc = validate_row(pkg, *t, cells, present, vm, row, v.errors);
        v.error = 0;
        return rc;
    }
    }
}

}  // namespace

// ---- registration -----------------------------------------------------------------------------

void register_database_api(MsiManager& mgr, HandlerMap& map) {
    MsiManager* M = &mgr;

    // ---- handles ----
    map["MsiCloseHandle"] = [M](Marshaler& m) {
        const u32 h = m.arg32(0);
        if (h == 0) { m.set_return(kOk); return; }
        m.set_return(M->handles().close(h) ? kOk : kInvalidHandle);
    };
    map["MsiCloseAllHandles"] = [M](Marshaler& m) { m.set_return(M->handles().close_all_of_current_thread()); };

    // ---- records ----
    map["MsiCreateRecord"] = [M](Marshaler& m) { m.set_return(new_record(*M, m.arg32(0))); };
    map["MsiRecordGetFieldCount"] = [M](Marshaler& m) {
        auto r = record_of(*M, m.arg32(0));
        m.set_return(r ? r->count() : 0xFFFFFFFFu);
    };
    map["MsiRecordIsNull"] = [M](Marshaler& m) {
        auto r = record_of(*M, m.arg32(0));
        m.set_return(r ? (record_is_null(*r, m.arg32(1)) ? 1u : 0u) : 0u);
    };
    map["MsiRecordDataSize"] = [M](Marshaler& m) {
        auto r = record_of(*M, m.arg32(0));
        const u32 f = m.arg32(1);
        if (!r || f > r->count()) { m.set_return(0); return; }
        const Record::Field& x = r->fields[f];
        switch (x.kind) {
        case Record::Kind::Integer: m.set_return(4); break;
        case Record::Kind::String: m.set_return(static_cast<u32>(x.text.size())); break;
        case Record::Kind::Stream: m.set_return(static_cast<u32>(x.stream.size())); break;
        default: m.set_return(0); break;
        }
    };
    map["MsiRecordGetInteger"] = [M](Marshaler& m) {
        auto r = record_of(*M, m.arg32(0));
        const u32 f = m.arg32(1);
        if (!r || f > r->count()) { m.set_return(kNullInteger); return; }
        const Record::Field& x = r->fields[f];
        const i32 v = record_integer(*r, f);
        // A string field that is not a number sets ERROR_INVALID_DATA; a
        // numeric one (even one that wraps to MSI_NULL_INTEGER) clears
        // last-error (msi_record_read_probe.c).
        if (x.kind == Record::Kind::String && !x.text.empty()) {
            const std::u16string& t = x.text;
            const usize start = t[0] == u'-' ? 1 : 0;
            bool numeric = start < t.size();
            for (usize i = start; i < t.size() && numeric; ++i) numeric = t[i] >= u'0' && t[i] <= u'9';
            set_last_error(numeric ? 0u : 13u);
        }
        m.set_return(static_cast<u32>(v));
    };
    map["MsiRecordSetInteger"] = [M](Marshaler& m) {
        auto r = record_of(*M, m.arg32(0));
        if (!r) { m.set_return(kInvalidHandle); return; }
        const u32 f = m.arg32(1);
        if (f > r->count()) { m.set_return(kInvalidParameter); return; }
        record_set_integer(*r, f, static_cast<i32>(m.arg32(2)));
        m.set_return(kOk);
    };
    for (const bool wide : {false, true}) {
        map[wide ? "MsiRecordSetStringW" : "MsiRecordSetStringA"] = [M, wide](Marshaler& m) {
            auto r = record_of(*M, m.arg32(0));
            if (!r) { m.set_return(kInvalidHandle); return; }
            const u32 f = m.arg32(1);
            if (f > r->count()) { m.set_return(kInvalidParameter); return; }
            auto s = in_str(m, m.arg_ptr(2), wide);
            record_set_string(*r, f, s.value_or(std::string{}));
            m.set_return(kOk);
        };
        map[wide ? "MsiRecordGetStringW" : "MsiRecordGetStringA"] = [M, wide](Marshaler& m) {
            auto r = record_of(*M, m.arg32(0));
            if (!r) { m.set_return(kInvalidHandle); return; }
            const u32 f = m.arg32(1);
            const GuestAddr buf = m.arg_ptr(2), pcch = m.arg_ptr(3);
            if (pcch == 0) { m.set_return(kInvalidParameter); return; }
            if (f <= r->count() && r->fields[f].kind == Record::Kind::Stream) {
                (void)m.write_u32(pcch, 0);
                m.set_return(kInvalidDatatype);
                return;
            }
            m.set_return(out_str(m, buf, pcch, record_string(*r, f), wide));
        };
        map[wide ? "MsiRecordSetStreamW" : "MsiRecordSetStreamA"] = [M, wide](Marshaler& m) {
            auto r = record_of(*M, m.arg32(0));
            if (!r) { m.set_return(kInvalidHandle); return; }
            const u32 f = m.arg32(1);
            if (f == 0 || f > r->count()) { m.set_return(kInvalidParameter); return; }
            auto path = in_str(m, m.arg_ptr(2), wide);
            Record::Field& x = r->fields[f];
            if (!path) {
                // A null path rewinds an existing stream.
                if (x.kind != Record::Kind::Stream) { m.set_return(kInvalidDatatype); return; }
                x.stream_pos = 0;
                m.set_return(kOk);
                return;
            }
            auto bytes = read_file(*M, *path);
            if (!bytes) { m.set_return(kBadPathname); return; }
            x = Record::Field{};
            x.kind = Record::Kind::Stream;
            x.stream = std::move(*bytes);
            clear_last_error_record();
            m.set_return(kOk);
        };
    }
    map["MsiRecordReadStream"] = [M](Marshaler& m) {
        auto r = record_of(*M, m.arg32(0));
        const u32 f = m.arg32(1);
        const GuestAddr buf = m.arg_ptr(2), pcb = m.arg_ptr(3);
        if (pcb == 0) { m.set_return(kInvalidParameter); return; }
        if (!r) { m.set_return(kInvalidHandle); return; }
        if (f > r->count()) { (void)m.write_u32(pcb, 0); m.set_return(kInvalidData); return; }
        Record::Field& x = r->fields[f];
        if (x.kind != Record::Kind::Stream) { m.set_return(kInvalidDatatype); return; }
        const usize remaining = x.stream.size() - std::min(x.stream_pos, x.stream.size());
        if (buf == 0) { (void)m.write_u32(pcb, static_cast<u32>(remaining)); m.set_return(kOk); return; }
        const usize n = std::min<usize>(m.read_u32(pcb).value_or(0), remaining);
        if (n) (void)m.memory().write_bytes(buf, x.stream.data() + x.stream_pos, n);
        x.stream_pos += n;
        (void)m.write_u32(pcb, static_cast<u32>(n));
        m.set_return(kOk);
    };
    map["MsiRecordClearData"] = [M](Marshaler& m) {
        auto r = record_of(*M, m.arg32(0));
        if (!r) { m.set_return(kInvalidHandle); return; }
        for (Record::Field& x : r->fields) x = Record::Field{};
        m.set_return(kOk);
    };
    map["MsiGetLastErrorRecord"] = [M](Marshaler& m) {
        std::shared_ptr<Record> rec;
        {
            std::lock_guard lock(g_error_mutex);
            rec = std::move(g_error_record);
        }
        m.set_return(rec ? add_record(*M, std::move(rec)) : 0u);
    };

    // ---- databases ----
    for (const bool wide : {false, true}) {
        map[wide ? "MsiOpenDatabaseW" : "MsiOpenDatabaseA"] = [M, wide](Marshaler& m) {
            const GuestAddr out = m.arg_ptr(2);
            if (out == 0) { m.set_return(kInvalidParameter); return; }
            (void)m.write_u32(out, 0);
            auto path = in_str(m, m.arg_ptr(0), wide);
            if (!path) { m.set_return(kInvalidParameter); return; }
            const GuestAddr persist = m.arg_ptr(1);
            const u32 patch_bit = wide ? 16u : 32u;
            auto db = std::make_shared<DatabaseObject>();
            db->manager = M;
            db->path = *path;
            u32 mode = kDbOpenReadOnly;
            // Persist values below 64 KiB are the predefined modes (no user
            // pointer lives there); anything else names an output database.
            if (persist >= 0x10000) {
                auto output = in_str(m, persist, wide);
                if (!output) { m.set_return(kInvalidParameter); return; }
                db->output_path = *output;
                mode = kDbOpenTransact;
            } else {
                db->patch = (persist & patch_bit) != 0;
                mode = static_cast<u32>(persist) & ~patch_bit;
                // Unknown small values open read-only (measured: 5 succeeds).
                if (mode > kDbOpenCreateDirect) mode = kDbOpenReadOnly;
            }
            if (mode == kDbOpenCreate || mode == kDbOpenCreateDirect) {
                db->package = std::make_shared<Package>(Package::create());
                if (db->patch) db->package->set_clsid(kClsidPatch);
                // Windows creates the file at once and removes it again if the
                // database is never committed (msi_database_modify_probe).
                if (!write_file(*M, *path, db->package->serialize())) { m.set_return(kOpenFailed); return; }
                db->created = true;
                db->read_only = false;
                db->direct = mode == kDbOpenCreateDirect;
            } else {
                auto bytes = read_file(*M, *path);
                if (!bytes) { m.set_return(kOpenFailed); return; }
                auto pkg = Package::open(*bytes);
                if (!pkg) { m.set_return(kOpenFailed); return; }
                db->package = std::make_shared<Package>(std::move(*pkg));
                db->read_only = mode == kDbOpenReadOnly;
                db->direct = mode == kDbOpenDirect;
                if (!db->output_path.empty()) {
                    if (!write_file(*M, db->output_path, *bytes)) { m.set_return(kOpenFailed); return; }
                }
            }
            clear_last_error_record();
            (void)m.write_u32(out, M->handles().add(std::move(db)));
            m.set_return(kOk);
        };
    }
    map["MsiDatabaseCommit"] = [M](Marshaler& m) {
        auto db = db_of(*M, m.arg32(0));
        if (!db) { m.set_return(kInvalidHandle); return; }
        clear_last_error_record();
        if (db->read_only) { m.set_return(kOk); return; }
        std::vector<u8> image;
        {
            std::lock_guard lock(db->package->mutex());
            image = db->package->serialize();
        }
        if (!write_file(*M, db->output_path.empty() ? db->path : db->output_path, image)) {
            set_last_error_record(2203, {db_name(*db), ""});
            m.set_return(kFunctionFailed);
            return;
        }
        db->committed = true;
        db->dirty = false;
        m.set_return(kOk);
    };
    map["MsiGetDatabaseState"] = [M](Marshaler& m) {
        auto db = db_of(*M, m.arg32(0));
        m.set_return(!db ? 0xFFFFFFFFu : (db->read_only ? 0u : 1u));
    };
    for (const bool wide : {false, true}) {
        map[wide ? "MsiDatabaseOpenViewW" : "MsiDatabaseOpenViewA"] = [M, wide](Marshaler& m) {
            const GuestAddr out = m.arg_ptr(2);
            if (out == 0) { m.set_return(kInvalidParameter); return; }
            (void)m.write_u32(out, 0);
            auto db = db_of(*M, m.arg32(0));
            if (!db) { m.set_return(kInvalidHandle); return; }
            auto sql_text = in_str(m, m.arg_ptr(1), wide);
            if (!sql_text) { m.set_return(kBadQuery); return; }
            sql::Error err;
            auto stmt = sql::Statement::prepare(*db->package, *sql_text, err);
            if (!stmt) {
                set_sql_error(*db, err);
                m.set_return(kBadQuery);
                return;
            }
            auto v = std::make_shared<ViewObject>();
            v->db = db;
            v->statement = std::move(stmt);
            clear_last_error_record();
            (void)m.write_u32(out, M->handles().add(std::move(v)));
            m.set_return(kOk);
        };
        map[wide ? "MsiDatabaseGetPrimaryKeysW" : "MsiDatabaseGetPrimaryKeysA"] = [M, wide](Marshaler& m) {
            const GuestAddr out = m.arg_ptr(2);
            auto db = db_of(*M, m.arg32(0));
            if (!db) { m.set_return(kInvalidHandle); return; }
            auto name = in_str(m, m.arg_ptr(1), wide);
            if (out == 0 || !name) { m.set_return(kInvalidParameter); return; }
            (void)m.write_u32(out, 0);
            std::lock_guard lock(db->package->mutex());
            const Table* t = db->package->find_table(*name);
            if (!t) {
                set_last_error_record(2220, {db_name(*db), "", ""});
                m.set_return(kInvalidTable);
                return;
            }
            const usize keys = t->key_count();
            auto rec = std::make_shared<Record>(static_cast<u32>(keys));
            record_set_string(*rec, 0, *name);
            for (usize k = 0; k < keys; ++k) record_set_string(*rec, static_cast<u32>(k + 1), t->columns[k].name);
            (void)m.write_u32(out, add_record(*M, std::move(rec)));
            m.set_return(kOk);
        };
        map[wide ? "MsiDatabaseIsTablePersistentW" : "MsiDatabaseIsTablePersistentA"] = [M, wide](Marshaler& m) {
            auto db = db_of(*M, m.arg32(0));
            auto name = in_str(m, m.arg_ptr(1), wide);
            if (!db || !name) { m.set_return(3); return; }  // MSICONDITION_ERROR
            std::lock_guard lock(db->package->mutex());
            const Table* t = db->package->find_table(*name);
            // Missing and virtual tables are MSICONDITION_NONE (measured).
            m.set_return(!t ? 2u : (t->temporary() ? 0u : 1u));
        };
    }
    map["MsiGetActiveDatabase"] = [M](Marshaler& m) {
        auto install = M->handles().get<InstallHandle>(m.arg32(0));
        // Deferred custom actions have no database (Learn "Obtaining Context
        // Information for Deferred Execution Custom Actions").
        if (!install || !install->engine || install->deferred) { m.set_return(0); return; }
        Session& session = install->engine->session();
        Package* pkg = session.package();
        if (pkg == nullptr) { m.set_return(0); return; }
        std::shared_ptr<Package> shared = pkg->weak_from_this().lock();
        if (!shared) shared = std::shared_ptr<Package>(std::shared_ptr<Package>{}, pkg);
        auto db = std::make_shared<DatabaseObject>();
        db->package = std::move(shared);
        db->path = session.property("OriginalDatabase");
        db->owner = install->engine;
        // Custom actions change only temporary data (Learn MsiViewModify).
        db->read_only = true;
        m.set_return(M->handles().add(std::move(db)));
    };

    // ---- views ----
    map["MsiViewExecute"] = [M](Marshaler& m) {
        auto v = view_of(*M, m.arg32(0));
        if (!v) { m.set_return(kInvalidHandle); return; }
        std::shared_ptr<Record> params;
        if (const u32 h = m.arg32(1)) {
            params = record_of(*M, h);
            if (!params) { m.set_return(kInvalidHandle); return; }
        }
        Package& pkg = *v->db->package;
        std::lock_guard lock(pkg.mutex());
        sql::Error err;
        const u32 rc = v->statement->execute(pkg, params.get(), v->db->read_only, err);
        v->next = 0;
        v->fetched.clear();
        if (rc) {
            v->executed = false;
            set_sql_error(*v->db, err);
            m.set_return(rc);
            return;
        }
        if (v->statement->kind() != sql::Kind::Select) v->db->dirty = true;
        v->executed = true;
        clear_last_error_record();
        m.set_return(kOk);
    };
    map["MsiViewFetch"] = [M](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(1);
        auto v = view_of(*M, m.arg32(0));
        if (!v) { m.set_return(kInvalidHandle); return; }
        if (out == 0) { m.set_return(kInvalidParameter); return; }
        (void)m.write_u32(out, 0);
        if (!v->executed || v->statement->kind() != sql::Kind::Select) { m.set_return(kFunctionFailed); return; }
        auto& rows = v->statement->results();
        if (v->next >= rows.size()) {
            // The cursor starts over after reporting the end (measured).
            v->next = 0;
            m.set_return(kNoMoreItems);
            return;
        }
        const sql::ResultRow& row = rows[v->next++];
        Package& pkg = *v->db->package;
        std::lock_guard lock(pkg.mutex());
        auto rec = std::make_shared<Record>(static_cast<u32>(row.values.size()));
        for (usize i = 0; i < row.values.size(); ++i) value_to_field(pkg, row.values[i], rec->fields[i + 1]);
        // Field 0: the row cookie MsiViewModify identifies the row by.
        const i32 cookie = g_next_cookie.fetch_add(1);
        FetchedRow fr;
        for (usize t = 0; t < v->statement->tables().size() && t < row.rows.size(); ++t) {
            const std::string& tname = v->statement->tables()[t];
            const Table* tb = pkg.find_table(tname);
            if (tb && row.rows[t] < tb->rows.size()) {
                fr.keys.push_back(key_of(*tb, row.rows[t]));
            } else if (tname == "_Streams" || tname == "_Storages") {
                // A virtual row is identified by its Name, when selected.
                std::vector<std::string> key;
                const auto& cols = v->statement->columns();
                for (usize i = 0; i < cols.size(); ++i)
                    if (cols[i].table == static_cast<i32>(t) && cols[i].name == "Name") key.push_back(row.values[i].s);
                fr.keys.push_back(std::move(key));
            } else {
                fr.keys.emplace_back();
            }
        }
        v->fetched[cookie] = std::move(fr);
        rec->fields[0].kind = Record::Kind::Integer;
        rec->fields[0].integer = cookie;
        (void)m.write_u32(out, add_record(*M, std::move(rec)));
        m.set_return(kOk);
    };
    map["MsiViewClose"] = [M](Marshaler& m) {
        auto v = view_of(*M, m.arg32(0));
        if (!v) { m.set_return(kInvalidHandle); return; }
        v->executed = false;
        v->next = 0;
        m.set_return(kOk);
    };
    map["MsiViewGetColumnInfo"] = [M](Marshaler& m) {
        auto v = view_of(*M, m.arg32(0));
        const u32 kind = m.arg32(1);
        const GuestAddr out = m.arg_ptr(2);
        if (!v) { m.set_return(kInvalidHandle); return; }
        if (kind > 1 || out == 0) { m.set_return(kInvalidParameter); return; }
        const auto& cols = v->statement->columns();
        const bool select = v->statement->kind() == sql::Kind::Select;
        auto rec = std::make_shared<Record>(select ? static_cast<u32>(cols.size()) : 0u);
        for (usize i = 0; select && i < cols.size(); ++i)
            record_set_string(*rec, static_cast<u32>(i + 1), kind ? cols[i].definition : cols[i].name);
        (void)m.write_u32(out, add_record(*M, std::move(rec)));
        m.set_return(kOk);
    };
    for (const bool wide : {false, true}) {
        map[wide ? "MsiViewGetErrorW" : "MsiViewGetErrorA"] = [M, wide](Marshaler& m) {
            constexpr u32 kInvalidArg = static_cast<u32>(-3), kMore = static_cast<u32>(-2);
            auto v = view_of(*M, m.arg32(0));
            const GuestAddr buf = m.arg_ptr(1), pcch = m.arg_ptr(2);
            if (!v || pcch == 0) { m.set_return(kInvalidArg); return; }
            // Errors come out one per call, in column order (measured);
            // a too-small buffer still consumes the error.
            if (v->errors.empty()) {
                (void)out_str(m, buf, pcch, {}, wide);
                m.set_return(0);
                return;
            }
            const auto [column, code] = v->errors.front();
            v->errors.erase(v->errors.begin());
            const u32 rc = out_str(m, buf, pcch, column, wide);
            m.set_return(rc == kMoreData ? kMore : static_cast<u32>(code));
        };
    }
    map["MsiViewModify"] = [M](Marshaler& m) {
        auto v = view_of(*M, m.arg32(0));
        const i32 mode = static_cast<i32>(m.arg32(1));
        auto rec = record_of(*M, m.arg32(2));
        if (!v || !rec) { m.set_return(kInvalidHandle); return; }
        const u32 rc = modify(*M, *v, mode, *rec);
        if (rc == kOk) clear_last_error_record();
        m.set_return(rc);
    };

    // ---- summary information ----
    for (const bool wide : {false, true}) {
        map[wide ? "MsiGetSummaryInformationW" : "MsiGetSummaryInformationA"] = [M, wide](Marshaler& m) {
            const GuestAddr out = m.arg_ptr(3);
            if (out == 0) { m.set_return(kInvalidParameter); return; }
            (void)m.write_u32(out, 0);
            auto path = in_str(m, m.arg_ptr(1), wide);
            u32 h = 0;
            const u32 rc = get_summary_information(*M, m.arg32(0), path ? &*path : nullptr, m.arg32(2), h);
            if (rc == kOk) (void)m.write_u32(out, h);
            m.set_return(rc);
        };
        map[wide ? "MsiSummaryInfoGetPropertyW" : "MsiSummaryInfoGetPropertyA"] = [M, wide](Marshaler& m) {
            auto si = M->handles().get<SummaryInfoObject>(m.arg32(0));
            const u32 pid = m.arg32(1);
            const GuestAddr ptype = m.arg_ptr(2), pint = m.arg_ptr(3), pft = m.arg_ptr(4);
            const GuestAddr buf = m.arg_ptr(5), pcch = m.arg_ptr(6);
            if (!si) { m.set_return(kInvalidHandle); return; }
            if (pid > 19) { m.set_return(kUnknownProperty); return; }
            auto it = si->properties.find(pid);
            if (it == si->properties.end()) {
                // Absent: VT_EMPTY and nothing else written (measured).
                if (ptype) (void)m.write_u32(ptype, kVtEmpty);
                m.set_return(kOk);
                return;
            }
            const SummaryProperty& p = it->second;
            if (ptype) (void)m.write_u32(ptype, p.vt);
            if (p.vt == kVtI2 || p.vt == kVtI4) {
                if (pint) (void)m.write_u32(pint, static_cast<u32>(p.integer));
            } else if (p.vt == kVtFiletime) {
                if (pft) { (void)m.write_u32(pft, static_cast<u32>(p.filetime)); (void)m.write_u32(pft + 4, static_cast<u32>(p.filetime >> 32)); }
            } else {
                if (pcch == 0) { m.set_return(buf ? kInvalidParameter : kOk); return; }
                m.set_return(out_str(m, buf, pcch, p.text, wide));
                return;
            }
            m.set_return(kOk);
        };
        map[wide ? "MsiSummaryInfoSetPropertyW" : "MsiSummaryInfoSetPropertyA"] = [M, wide](Marshaler& m) {
            auto si = M->handles().get<SummaryInfoObject>(m.arg32(0));
            const u32 pid = m.arg32(1);
            const u32 vt = m.arg32(2);
            const i32 iv = static_cast<i32>(m.arg32(3));
            const GuestAddr pft = m.arg_ptr(4);
            const GuestAddr psz = m.arg_ptr(5);
            if (!si) { m.set_return(kInvalidHandle); return; }
            if (pid > 19) { m.set_return(kUnknownProperty); return; }
            const u32 want = summary_vt(pid);
            if (want == kVtEmpty) { m.set_return(kDatatypeMismatch); return; }
            const bool integer_ok = (want == kVtI2 || want == kVtI4) && (vt == kVtI2 || vt == kVtI4);
            if (vt != kVtEmpty && vt != want && !integer_ok) { m.set_return(kDatatypeMismatch); return; }
            SummaryProperty p;
            p.vt = want;
            if (vt != kVtEmpty) {
                if (want == kVtFiletime) {
                    if (pft == 0) { m.set_return(kInvalidParameter); return; }
                    p.filetime = m.read_u32(pft).value_or(0) | (static_cast<u64>(m.read_u32(pft + 4).value_or(0)) << 32);
                } else if (want == kVtLpstr) {
                    auto s = in_str(m, psz, wide);
                    if (!s) { m.set_return(kInvalidParameter); return; }
                    p.text = *s;
                } else {
                    p.integer = want == kVtI2 ? static_cast<i16>(iv) : iv;
                }
            }
            // At most uiUpdateCount distinct properties may change.
            if (std::find(si->changed.begin(), si->changed.end(), pid) == si->changed.end()) {
                if (si->changed.size() >= si->update_count) { m.set_return(kFunctionFailed); return; }
                si->changed.push_back(pid);
            }
            if (vt == kVtEmpty) si->properties.erase(pid);
            else si->properties[pid] = std::move(p);
            si->dirty = true;
            m.set_return(kOk);
        };
    }
    map["MsiSummaryInfoGetPropertyCount"] = [M](Marshaler& m) {
        auto si = M->handles().get<SummaryInfoObject>(m.arg32(0));
        if (!si) { m.set_return(kInvalidHandle); return; }
        if (const GuestAddr p = m.arg_ptr(1)) (void)m.write_u32(p, static_cast<u32>(si->properties.size()));
        m.set_return(kOk);
    };
    map["MsiSummaryInfoPersist"] = [M](Marshaler& m) {
        auto si = M->handles().get<SummaryInfoObject>(m.arg32(0));
        if (!si) { m.set_return(kInvalidHandle); return; }
        // Nothing changed (or read-only): ERROR_FUNCTION_FAILED (measured).
        if (!si->dirty || si->update_count == 0) { m.set_return(kFunctionFailed); return; }
        const std::vector<u8> stream = build_summary_information(si->properties);
        if (si->db) {
            std::lock_guard lock(si->db->package->mutex());
            si->db->package->set_stream(kSummaryStream, stream);
            si->db->dirty = true;
        } else {
            auto bytes = read_file(*M, si->path);
            auto pkg = bytes ? Package::open(*bytes) : std::nullopt;
            if (!pkg) { m.set_return(kFunctionFailed); return; }
            pkg->set_stream(kSummaryStream, stream);
            if (!write_file(*M, si->path, pkg->serialize())) { m.set_return(kFunctionFailed); return; }
        }
        si->dirty = false;
        m.set_return(kOk);
    };

    register_transform_api(mgr, map);
}

}  // namespace vitreo::winapi::msi
