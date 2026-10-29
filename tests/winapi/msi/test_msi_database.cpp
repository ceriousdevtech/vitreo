// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_msi_database.cpp
/// @brief Contract tests for the Windows Installer database area, both bitnesses
/// through msi_fixture.hpp. Expected values are the Windows behaviour measured
/// by tools/oracle_probe/msi_database_*_probe.c (x86 == x64).

#include "msi_fixture.hpp"

#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/system/msi/msi_database.hpp>
#include <winapi/system/msi/msi_package.hpp>
#include <winapi/system/msi/msi_sql.hpp>
#include <winapi/system/msi/msi_validation.hpp>
#include <winapi/com/ole32/storage.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <unistd.h>

namespace vitreo::winapi::msi_test {
namespace {

class Msi_database_Test : public MsiTest {
protected:
    void SetUp() override {
        MsiTest::SetUp();
        root = std::filesystem::temp_directory_path() /
               ("vitreo_msi_db_" + std::to_string(::getpid()) + (x64() ? "_64" : "_32"));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
        mgr.set_vfs(&vfs);
    }
    void TearDown() override {
        (void)call("MsiCloseAllHandles", {});
        mgr.set_vfs(nullptr);
        std::filesystem::remove_all(root);
    }

    /// A NUL-terminated wide string in scratch memory.
    GuestAddr w(const std::u16string& s) {
        const GuestAddr at = next;
        write_wide(at, s);
        next += (s.size() + 1) * 2 + 16;
        return at;
    }
    GuestAddr a(const std::string& s) {
        const GuestAddr at = next;
        EXPECT_TRUE(memory.write_bytes(at, reinterpret_cast<const u8*>(s.data()), s.size()).is_ok());
        EXPECT_TRUE(memory.write_u8(at + s.size(), 0).is_ok());
        next += s.size() + 16;
        return at;
    }
    /// UTF-8 (up to 3-byte sequences) to UTF-16.
    static std::u16string u(const std::string& s) {
        std::u16string out;
        for (usize i = 0; i < s.size();) {
            const u8 b = static_cast<u8>(s[i]);
            if (b < 0x80) { out.push_back(b); ++i; }
            else if ((b & 0xE0) == 0xC0) { out.push_back(static_cast<char16_t>(((b & 0x1F) << 6) | (s[i + 1] & 0x3F))); i += 2; }
            else { out.push_back(static_cast<char16_t>(((b & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F))); i += 3; }
        }
        return out;
    }

    u32 open(const std::string& path, u64 mode) {
        write_int(OUT, 0);
        const u64 rc = call("MsiOpenDatabaseW", {I(w(u(path))), I(mode), I(OUT)});
        EXPECT_EQ(rc, 0u) << path;
        return static_cast<u32>(read_int(OUT));
    }
    /// OpenView + Execute; returns the first failing code.
    u32 exec(u32 db, const std::string& sql, u32 params = 0) {
        write_int(OUT, 0);
        const u32 rc = static_cast<u32>(call("MsiDatabaseOpenViewW", {I(db), I(w(u(sql))), I(OUT)}));
        if (rc) return rc;
        const u32 v = static_cast<u32>(read_int(OUT));
        const u32 rc2 = static_cast<u32>(call("MsiViewExecute", {I(v), I(params)}));
        call("MsiCloseHandle", {I(v)});
        return rc2;
    }
    std::string field(u32 rec, u32 i) {
        if (call("MsiRecordIsNull", {I(rec), I(i)})) return "~";
        write_int(OUT2, 2048);
        const u32 rc = static_cast<u32>(call("MsiRecordGetStringW", {I(rec), I(i), I(BUF), I(OUT2)}));
        if (rc == 1804) return "<s" + std::to_string(call("MsiRecordDataSize", {I(rec), I(i)})) + ">";
        std::string out;
        for (u32 k = 0;; ++k) {
            const u16 c = memory.read_u16(BUF + k * 2).value();
            if (c == 0) break;
            out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        }
        return out;
    }
    std::string rec_text(u32 rec) {
        const u32 n = static_cast<u32>(call("MsiRecordGetFieldCount", {I(rec)}));
        std::string s;
        for (u32 i = 1; i <= n; ++i) { if (i > 1) s += "|"; s += field(rec, i); }
        return s;
    }
    std::vector<std::string> query(u32 db, const std::string& sql, u32 params = 0) {
        std::vector<std::string> rows;
        write_int(OUT, 0);
        const u32 rc = static_cast<u32>(call("MsiDatabaseOpenViewW", {I(db), I(w(u(sql))), I(OUT)}));
        if (rc) { rows.push_back("open=" + std::to_string(rc)); return rows; }
        const u32 v = static_cast<u32>(read_int(OUT));
        const u32 rc2 = static_cast<u32>(call("MsiViewExecute", {I(v), I(params)}));
        if (rc2) { rows.push_back("exec=" + std::to_string(rc2)); call("MsiCloseHandle", {I(v)}); return rows; }
        for (;;) {
            if (call("MsiViewFetch", {I(v), I(OUT3)}) != 0) break;
            const u32 r = static_cast<u32>(read_int(OUT3));
            rows.push_back(rec_text(r));
            call("MsiCloseHandle", {I(r)});
        }
        call("MsiCloseHandle", {I(v)});
        return rows;
    }
    /// MsiGetLastErrorRecord as "code|arg|arg".
    std::string last_error() {
        const u32 r = static_cast<u32>(call("MsiGetLastErrorRecord", {}));
        if (!r) return "none";
        const std::string s = rec_text(r);
        call("MsiCloseHandle", {I(r)});
        return s;
    }
    u32 record(std::initializer_list<std::optional<std::string>> fields) {
        const u32 r = static_cast<u32>(call("MsiCreateRecord", {I(fields.size())}));
        u32 i = 1;
        for (const auto& f : fields) {
            if (f) {
                if (!f->empty() && (std::isdigit(static_cast<unsigned char>((*f)[0])) || (*f)[0] == '-') &&
                    f->find_first_not_of("-0123456789") == std::string::npos)
                    call("MsiRecordSetInteger", {I(r), I(i), I(static_cast<u32>(std::stoi(*f)))});
                else
                    call("MsiRecordSetStringW", {I(r), I(i), I(w(u(*f)))});
            }
            ++i;
        }
        return r;
    }
    void host_file(const std::string& rel, const std::string& bytes) {
        std::filesystem::create_directories((root / rel).parent_path());
        std::ofstream(root / rel, std::ios::binary) << bytes;
    }
    std::string host_read(const std::string& rel) {
        std::ifstream f(root / rel, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), {});
    }

    /// The T / U tables of msi_database_sql_probe.
    u32 sql_db() {
        const u32 db = open("C:\\sql.msi", 3);
        EXPECT_EQ(exec(db, "CREATE TABLE `T` (`K` CHAR(72) NOT NULL, `S` CHAR(0), `I2` SHORT, `I4` LONG, `B` OBJECT PRIMARY KEY `K`)"), 0u);
        EXPECT_EQ(exec(db, "CREATE TABLE `U` (`A` SHORT NOT NULL, `B` CHAR(10) NOT NULL, `C` LONG PRIMARY KEY `A`, `B`)"), 0u);
        for (const char* s : {"INSERT INTO `T` (`K`,`S`,`I2`,`I4`) VALUES ('a','x',1,100)",
                              "INSERT INTO `T` (`K`,`S`,`I2`,`I4`) VALUES ('b','y',-2,-200)",
                              "INSERT INTO `T` (`K`,`S`,`I2`,`I4`) VALUES ('c','',3,0)",
                              "INSERT INTO `T` (`K`,`I2`) VALUES ('d',3)",
                              "INSERT INTO `T` (`K`,`S`,`I4`) VALUES ('10','2',2)",
                              "INSERT INTO `U` (`A`,`B`,`C`) VALUES (1,'a',5)",
                              "INSERT INTO `U` (`A`,`B`,`C`) VALUES (2,'b',4)",
                              "INSERT INTO `U` (`A`,`B`,`C`) VALUES (1,'c',3)",
                              "INSERT INTO `U` (`A`,`B`,`C`) VALUES (3,'a',3)",
                              "INSERT INTO `U` (`A`,`B`) VALUES (4,'d')"})
            EXPECT_EQ(exec(db, s), 0u) << s;
        return db;
    }

    using V = std::vector<std::string>;
    std::filesystem::path root;
    capsule::Vfs vfs;
    GuestAddr next = DATA + 0x20000;
};

// ---- records (msi_database_record_probe, msi_record_read_probe) -----------------

TEST_P(Msi_database_Test, RecordStringBufferRules) {
    const u32 r = static_cast<u32>(call("MsiCreateRecord", {I(3)}));
    EXPECT_EQ(call("MsiCreateRecord", {I(65536)}), 0u);
    EXPECT_EQ(call("MsiRecordSetStringW", {I(r), I(1), I(w(u"hello"))}), 0u);
    EXPECT_EQ(call("MsiRecordSetStringW", {I(r), I(2), I(w(u""))}), 0u);
    EXPECT_EQ(call("MsiRecordIsNull", {I(r), I(2)}), 1u);
    EXPECT_EQ(call("MsiRecordSetStringW", {I(r), I(4), I(w(u"x"))}), 87u);
    EXPECT_EQ(call("MsiRecordSetStringW", {I(0), I(1), I(w(u"x"))}), 6u);
    EXPECT_EQ(call("MsiRecordDataSize", {I(r), I(1)}), 5u);
    EXPECT_EQ(call("MsiRecordSetInteger", {I(r), I(3), I(static_cast<u32>(-42))}), 0u);
    EXPECT_EQ(call("MsiRecordDataSize", {I(r), I(3)}), 4u);
    // exact fit / one short / capacity 1 / capacity 0 / null buffer
    struct Case { u32 cap; u32 rc; u32 cch; const char16_t* buf; };
    for (const Case c : {Case{6, 0, 5, u"hello"}, Case{5, 234, 5, u"hell"}, Case{1, 234, 5, u""}}) {
        poison(BUF, 32, 0x23);
        write_int(OUT, static_cast<i32>(c.cap));
        EXPECT_EQ(call("MsiRecordGetStringW", {I(r), I(1), I(BUF), I(OUT)}), c.rc) << c.cap;
        EXPECT_EQ(static_cast<u32>(read_int(OUT)), c.cch);
        std::u16string got;
        for (u32 k = 0; memory.read_u16(BUF + k * 2).value() != 0 && k < 16; ++k) got.push_back(static_cast<char16_t>(memory.read_u16(BUF + k * 2).value()));
        EXPECT_EQ(got, std::u16string(c.buf));
    }
    poison(BUF, 4, 0x23);
    write_int(OUT, 0);
    EXPECT_EQ(call("MsiRecordGetStringW", {I(r), I(1), I(BUF), I(OUT)}), 234u);
    EXPECT_EQ(memory.read_u16(BUF).value(), 0x2323u);  // capacity 0 writes nothing
    write_int(OUT, 50);
    EXPECT_EQ(call("MsiRecordGetStringW", {I(r), I(1), I(0), I(OUT)}), 0u);
    EXPECT_EQ(read_int(OUT), 5);
    EXPECT_EQ(call("MsiRecordGetStringW", {I(r), I(1), I(0), I(0)}), 87u);
    EXPECT_EQ(field(r, 3), "-42");
    write_int(OUT, 64);
    EXPECT_EQ(call("MsiRecordGetStringW", {I(r), I(7), I(BUF), I(OUT)}), 0u);  // past the end: ""
    EXPECT_EQ(read_int(OUT), 0);
    // ANSI: é is one CP-1252 byte, 中 becomes '?'.
    EXPECT_EQ(call("MsiRecordSetStringW", {I(r), I(1), I(w(u"\u00e9\u4e2d"))}), 0u);
    write_int(OUT, 64);
    EXPECT_EQ(call("MsiRecordGetStringA", {I(r), I(1), I(BUF), I(OUT)}), 0u);
    EXPECT_EQ(read_int(OUT), 2);
    EXPECT_EQ(memory.read_u8(BUF).value(), 0xE9u);
    EXPECT_EQ(memory.read_u8(BUF + 1).value(), static_cast<u8>('?'));
    EXPECT_EQ(call("MsiRecordDataSize", {I(r), I(1)}), 2u);
    EXPECT_EQ(call("MsiRecordSetStringA", {I(r), I(1), I(a("\xe9x"))}), 0u);
    write_int(OUT, 64);
    EXPECT_EQ(call("MsiRecordGetStringW", {I(r), I(1), I(BUF), I(OUT)}), 0u);
    EXPECT_EQ(memory.read_u16(BUF).value(), 0x00E9u);
    EXPECT_EQ(static_cast<u32>(call("MsiRecordGetInteger", {I(r), I(1)})), 0x80000000u);
    EXPECT_EQ(call("MsiRecordClearData", {I(r)}), 0u);
    EXPECT_EQ(call("MsiRecordIsNull", {I(r), I(1)}), 1u);
    EXPECT_EQ(call("MsiRecordGetFieldCount", {I(r)}), 3u);
    EXPECT_EQ(call("MsiRecordIsNull", {I(0), I(1)}), 0u);
    EXPECT_EQ(call("MsiRecordDataSize", {I(0), I(1)}), 0u);
    EXPECT_EQ(call("MsiCloseHandle", {I(r)}), 0u);
    EXPECT_EQ(call("MsiCloseHandle", {I(r)}), 6u);
    EXPECT_EQ(call("MsiCloseHandle", {I(0)}), 0u);
}

TEST_P(Msi_database_Test, RecordStreams) {
    host_file("s.bin", "0123456789");
    host_file("e.bin", "");
    const u32 r = static_cast<u32>(call("MsiCreateRecord", {I(3)}));
    call("MsiRecordSetStringW", {I(r), I(1), I(w(u"str"))});
    EXPECT_EQ(call("MsiRecordSetStreamW", {I(r), I(2), I(w(u"C:\\s.bin"))}), 0u);
    EXPECT_EQ(call("MsiRecordDataSize", {I(r), I(2)}), 10u);
    EXPECT_EQ(call("MsiRecordIsNull", {I(r), I(2)}), 0u);
    write_int(OUT, 64);
    EXPECT_EQ(call("MsiRecordGetStringW", {I(r), I(2), I(BUF), I(OUT)}), 1804u);
    EXPECT_EQ(read_int(OUT), 0);
    EXPECT_EQ(call("MsiRecordGetInteger", {I(r), I(2)}), 10u);  // a stream reads as its size
    write_int(OUT, 4);
    EXPECT_EQ(call("MsiRecordReadStream", {I(r), I(2), I(BUF), I(OUT)}), 0u);
    EXPECT_EQ(read_int(OUT), 4);
    write_int(OUT, 0);
    EXPECT_EQ(call("MsiRecordReadStream", {I(r), I(2), I(0), I(OUT)}), 0u);
    EXPECT_EQ(read_int(OUT), 6);  // remaining
    write_int(OUT, 16);
    EXPECT_EQ(call("MsiRecordReadStream", {I(r), I(2), I(BUF), I(OUT)}), 0u);
    EXPECT_EQ(read_int(OUT), 6);
    EXPECT_EQ(memory.read_u8(BUF).value(), static_cast<u8>('4'));
    write_int(OUT, 16);
    EXPECT_EQ(call("MsiRecordReadStream", {I(r), I(2), I(BUF), I(OUT)}), 0u);
    EXPECT_EQ(read_int(OUT), 0);
    EXPECT_EQ(call("MsiRecordSetStreamW", {I(r), I(2), I(0)}), 0u);  // rewind
    write_int(OUT, 3);
    EXPECT_EQ(call("MsiRecordReadStream", {I(r), I(2), I(BUF), I(OUT)}), 0u);
    EXPECT_EQ(memory.read_u8(BUF).value(), static_cast<u8>('0'));
    write_int(OUT, 4);
    EXPECT_EQ(call("MsiRecordReadStream", {I(r), I(1), I(BUF), I(OUT)}), 1804u);
    EXPECT_EQ(call("MsiRecordReadStream", {I(r), I(9), I(BUF), I(OUT)}), 13u);
    EXPECT_EQ(call("MsiRecordReadStream", {I(r), I(2), I(BUF), I(0)}), 87u);
    EXPECT_EQ(call("MsiRecordSetStreamW", {I(r), I(1), I(w(u"C:\\nope.bin"))}), 161u);
    EXPECT_EQ(call("MsiRecordSetStreamW", {I(r), I(1), I(0)}), 1804u);
    EXPECT_EQ(call("MsiRecordSetStreamW", {I(r), I(9), I(w(u"C:\\s.bin"))}), 87u);
    EXPECT_EQ(call("MsiRecordSetStreamW", {I(r), I(0), I(w(u"C:\\s.bin"))}), 87u);
    EXPECT_EQ(call("MsiRecordSetStreamW", {I(r), I(3), I(w(u"C:\\e.bin"))}), 0u);
    EXPECT_EQ(call("MsiRecordDataSize", {I(r), I(3)}), 0u);
    EXPECT_EQ(call("MsiRecordIsNull", {I(r), I(3)}), 0u);  // an empty stream is not null
}

TEST_P(Msi_database_Test, CloseAllHandlesCountsTheThreadsHandles) {
    (void)call("MsiCloseAllHandles", {});
    call("MsiCreateRecord", {I(1)});
    call("MsiCreateRecord", {I(1)});
    EXPECT_EQ(call("MsiCloseAllHandles", {}), 2u);
    EXPECT_EQ(call("MsiCloseAllHandles", {}), 0u);
    EXPECT_EQ(call("MsiGetLastErrorRecord", {}), 0u);
}

// ---- SQL (msi_database_sql_probe, msi_database_lex_probe) ---------------------------

TEST_P(Msi_database_Test, SqlNamesAndErrorsMatchWindows) {
    const u32 db = sql_db();
    EXPECT_EQ(query(db, "select * from `U` where `A` = 1"), (V{"1|a|5", "1|c|3"}));
    EXPECT_EQ(query(db, "SELECT B FROM U WHERE A=2"), V{"b"});
    EXPECT_EQ(query(db, "SELECT`B`FROM`U`WHERE`A`=1"), (V{"a", "c"}));
    EXPECT_EQ(query(db, "SELECT `B` FROM `U` WHERE `A` > = 2"), (V{"b", "a", "d"}));
    struct E { const char* sql; const char* error; };
    for (const E& e : {E{"SELECT * FROM `u`", "2228|C:\\sql.msi|u|SELECT * FROM `u`"},
                       E{"SELECT `b` FROM `U`", "2235|C:\\sql.msi|b|SELECT `b` FROM `U`"},
                       E{"SELECT\t`B` FROM `U`", "2237|C:\\sql.msi|SELECT\t`B` FROM `U`|~"},
                       E{"SELECT `B`\r\nFROM `U`", "2238|C:\\sql.msi|SELECT `B`\r\nFROM `U`|~"},
                       E{"SELECT * FROM `U` garbage", "2237|C:\\sql.msi|SELECT * FROM `U` garbage|~"},
                       E{"SELECT `A`", "2238|C:\\sql.msi|SELECT `A`|~"},
                       E{"SELECT `K` FROM `T` WHERE `I2` = '3'", "2232|C:\\sql.msi|3|SELECT `K` FROM `T` WHERE `I2` = '3'"},
                       E{"SELECT `K` FROM `T` WHERE `S` > 'x'", "2237|C:\\sql.msi|SELECT `K` FROM `T` WHERE `S` > 'x'|~"},
                       E{"SELECT `K` FROM `T` WHERE NOT `K` = 'a'", "2232|C:\\sql.msi|NOT|SELECT `K` FROM `T` WHERE NOT `K` = 'a'"},
                       E{"SELECT `B` FROM `U` WHERE `A` != 2", "2236|C:\\sql.msi|!|SELECT `B` FROM `U` WHERE `A` != 2"},
                       E{"SELECT `B` FROM `T`, `U`", "2235|C:\\sql.msi|B|SELECT `B` FROM `T`, `U`"},
                       E{"SELECT `K` FROM `T`, `T`", "2230|C:\\sql.msi|T|SELECT `K` FROM `T`, `T`"},
                       E{"SELECT `B` FROM `U` ORDER BY `C` DESC", "2237|C:\\sql.msi|SELECT `B` FROM `U` ORDER BY `C` DESC|~"},
                       E{"INSERT INTO `U` (`A`,`B`) VALUES (20)", "2239|C:\\sql.msi|INSERT INTO `U` (`A`,`B`) VALUES (20)|~"},
                       E{"CREATE TABLE `U` (`A` SHORT PRIMARY KEY `A`)", "2204|C:\\sql.msi|U|CREATE TABLE `U` (`A` SHORT PRIMARY KEY `A`)"},
                       E{"CREATE TABLE `N1` (`A` SHORT)", "2237|C:\\sql.msi|CREATE TABLE `N1` (`A` SHORT)|~"},
                       E{"CREATE TABLE `N9` (`A` SHORT, `A` SHORT PRIMARY KEY `A`)", "2242|C:\\sql.msi|A|CREATE TABLE `N9` (`A` SHORT, `A` SHORT PRIMARY KEY `A`)"},
                       E{"DROP TABLE `Nope`", "2228|C:\\sql.msi|Nope|DROP TABLE `Nope`"}}) {
        EXPECT_EQ(exec(db, e.sql), 1615u) << e.sql;
        std::string err = last_error();
        for (char& c : err) if (c == '?') c = '\t';
        EXPECT_EQ(err.substr(0, 4), std::string(e.error).substr(0, 4)) << e.sql;
    }
    // Error records carry the path as opened.
    EXPECT_EQ(exec(db, "SELECT * FROM `Nope`"), 1615u);
    EXPECT_EQ(last_error(), "2228|C:\\sql.msi|Nope|SELECT * FROM `Nope`");
    EXPECT_EQ(last_error(), "none");  // reading the record clears it
    // 32 comparisons are accepted, 33 are not.
    std::string where = "SELECT `A` FROM `U` WHERE `A` = 1";
    for (int k = 0; k < 31; ++k) where += " OR `A` = 1";
    EXPECT_EQ(exec(db, where), 0u);
    EXPECT_EQ(exec(db, where + " OR `A` = 1"), 1615u);
}

TEST_P(Msi_database_Test, SqlComparisonsAndNulls) {
    const u32 db = sql_db();
    EXPECT_EQ(query(db, "SELECT `K`,`I2` FROM `T` WHERE `I2` > 0"), (V{"a|1", "c|3", "d|3"}));
    EXPECT_EQ(query(db, "SELECT `K`,`I2` FROM `T` WHERE `I2` < 3"), (V{"a|1", "b|-2"}));
    EXPECT_EQ(query(db, "SELECT `K`,`I2` FROM `T` WHERE `I2` <> 3"), (V{"a|1", "b|-2", "10|~"}));
    EXPECT_EQ(query(db, "SELECT `K` FROM `T` WHERE `I4` <> 0"), (V{"a", "b", "d", "10"}));
    EXPECT_EQ(query(db, "SELECT `K` FROM `T` WHERE `I2` IS NULL"), V{"10"});
    EXPECT_EQ(query(db, "SELECT `K` FROM `T` WHERE `I4` < 2147483648"), V{});
    EXPECT_EQ(query(db, "SELECT `K` FROM `T` WHERE `S` <> 'x'"), (V{"b", "c", "d", "10"}));
    EXPECT_EQ(query(db, "SELECT `K` FROM `T` WHERE `S` = ''"), (V{"c", "d"}));
    EXPECT_EQ(query(db, "SELECT `K` FROM `T` WHERE `S` <> ''"), (V{"a", "b", "10"}));
    EXPECT_EQ(query(db, "SELECT `K` FROM `T` WHERE `S` = 'X'"), V{});
    EXPECT_EQ(query(db, "SELECT `K` FROM `T` WHERE `K` = 'a' OR `K` = 'b' AND `I2` = 3"), V{"a"});
    EXPECT_EQ(query(db, "SELECT `K` FROM `T` WHERE (`K` = 'a' OR `K` = 'b') AND `I2` = 1"), V{"a"});
    EXPECT_EQ(query(db, "SELECT 1, `B` FROM `U` WHERE `A` = 2"), V{"1|b"});
    EXPECT_EQ(query(db, "SELECT *, `A` FROM `U` WHERE `A` = 2"), V{"2|b|4|2"});
}

TEST_P(Msi_database_Test, SqlOrderDistinctJoinsAndParameters) {
    const u32 db = sql_db();
    EXPECT_EQ(query(db, "SELECT `A`,`B`,`C` FROM `U` ORDER BY `C`, `A`"), (V{"4|d|~", "1|c|3", "3|a|3", "2|b|4", "1|a|5"}));
    // Strings order by pool index (x, y, '2' were pooled in that order), nulls first.
    EXPECT_EQ(query(db, "SELECT `K` FROM `T` ORDER BY `S`"), (V{"c", "d", "a", "b", "10"}));
    EXPECT_EQ(query(db, "SELECT DISTINCT `C` FROM `U` ORDER BY `C`"), (V{"~", "3", "4", "5"}));
    // A table joined on its leading key is iterated inside the other.
    EXPECT_EQ(query(db, "SELECT `T`.`K`, `U`.`A` FROM `T`, `U` WHERE `T`.`K` = `U`.`B`"), (V{"a|1", "c|1", "b|2", "a|3", "d|4"}));
    EXPECT_EQ(query(db, "SELECT `T`.`K`, `U`.`B` FROM `T`, `U` WHERE `T`.`I2` = `U`.`A`"), (V{"a|a", "a|c", "c|a", "d|a"}));
    const u32 p1 = record({"1"});
    EXPECT_EQ(query(db, "SELECT `B` FROM `U` WHERE `A` = ?", p1), (V{"a", "c"}));
    const u32 ps = record({std::string("x1")});
    EXPECT_EQ(query(db, "SELECT `B` FROM `U` WHERE `A` = ?", ps), V{"exec=1627"});
    EXPECT_EQ(last_error().substr(0, 4), "2258");
    EXPECT_EQ(query(db, "SELECT `B` FROM `U` WHERE `A` = ?"), V{"exec=1627"});
    EXPECT_EQ(last_error(), "2208|C:\\sql.msi|~|~");
    const u32 pn = record({std::nullopt});
    EXPECT_EQ(query(db, "SELECT `K` FROM `T` WHERE `I2` = ?", pn), V{"10"});
    const u32 pi = record({"7", "p", "9"});
    EXPECT_EQ(exec(db, "INSERT INTO `U` (`A`,`B`,`C`) VALUES (?,?,?)", pi), 0u);
    EXPECT_EQ(query(db, "SELECT `C` FROM `U` WHERE `A` = 7"), V{"9"});
}

TEST_P(Msi_database_Test, SqlDmlAndDdlEdgeCases) {
    const u32 db = sql_db();
    EXPECT_EQ(exec(db, "INSERT INTO `U` (`A`,`B`,`C`) VALUES (1,'a',9)"), 1627u);
    EXPECT_EQ(last_error(), "2259|C:\\sql.msi|~|~");
    EXPECT_EQ(exec(db, "INSERT INTO `U` (`A`,`C`) VALUES (9,9)"), 1627u);
    EXPECT_EQ(exec(db, "INSERT INTO `U` (`A`,`B`) VALUES ('x','e')"), 1627u);
    EXPECT_EQ(exec(db, "INSERT INTO `U` (`A`,`B`) VALUES ('12','e')"), 0u);
    EXPECT_EQ(exec(db, "INSERT INTO `U` (`A`,`B`) VALUES (13,14)"), 0u);
    EXPECT_EQ(query(db, "SELECT * FROM `U` WHERE `A` > 11"), (V{"12|e|~", "13|14|~"}));
    EXPECT_EQ(exec(db, "INSERT INTO `T` (`K`) VALUES ('')"), 1627u);
    EXPECT_EQ(last_error(), "2210|C:\\sql.msi|0");
    EXPECT_EQ(exec(db, "INSERT INTO `T` (`K`,`I2`) VALUES ('e',40000)"), 1627u);
    EXPECT_EQ(last_error(), "2210|C:\\sql.msi|1");
    EXPECT_EQ(exec(db, "INSERT INTO `T` (`K`,`I2`) VALUES ('f',-32768)"), 0u);
    EXPECT_EQ(query(db, "SELECT `I2` FROM `T` WHERE `K` = 'f'"), V{"~"});
    EXPECT_EQ(exec(db, "UPDATE `U` SET `A` = 99 WHERE `A` = 2"), 1627u);
    EXPECT_EQ(exec(db, "UPDATE `U` SET `C` = NULL WHERE `A` = 2"), 0u);
    EXPECT_EQ(query(db, "SELECT `C` FROM `U` WHERE `A` = 2"), V{"~"});
    EXPECT_EQ(exec(db, "UPDATE `T`, `U` SET `I4` = 6 WHERE `T`.`K` = `U`.`B`"), 0u);
    EXPECT_EQ(query(db, "SELECT `I4` FROM `T` WHERE `K` = 'a'"), V{"6"});
    EXPECT_EQ(exec(db, "DELETE FROM `U` WHERE `A` = 3"), 0u);
    EXPECT_EQ(query(db, "SELECT `A` FROM `U` WHERE `A` = 3"), V{});
    EXPECT_EQ(exec(db, "CREATE TABLE `N2` (`A` CHAR(256) PRIMARY KEY `A`)"), 1615u);
    EXPECT_EQ(exec(db, "CREATE TABLE `N5` (`A` SHORT, `B` SHORT PRIMARY KEY `B`)"), 0u);
    write_int(OUT, 0);
    ASSERT_EQ(call("MsiDatabaseOpenViewW", {I(db), I(w(u"SELECT * FROM `N5`")), I(OUT)}), 0u);
    const u32 v = static_cast<u32>(read_int(OUT));
    ASSERT_EQ(call("MsiViewGetColumnInfo", {I(v), I(0), I(OUT2)}), 0u);
    EXPECT_EQ(rec_text(static_cast<u32>(read_int(OUT2))), "B|A");
    ASSERT_EQ(call("MsiViewGetColumnInfo", {I(v), I(1), I(OUT2)}), 0u);
    EXPECT_EQ(rec_text(static_cast<u32>(read_int(OUT2))), "I2|I2");
    EXPECT_EQ(call("MsiViewGetColumnInfo", {I(v), I(2), I(OUT2)}), 87u);
    EXPECT_EQ(call("MsiViewFetch", {I(v), I(OUT2)}), 1627u);  // before execute
    EXPECT_EQ(exec(db, "CREATE TABLE `N6` (`A` INT NOT NULL, `B` INTEGER, `C` CHARACTER(8) LOCALIZABLE, `D` CHAR, `E` LONGCHAR NOT NULL, `F` LONG PRIMARY KEY `A`)"), 0u);
    write_int(OUT, 0);
    ASSERT_EQ(call("MsiDatabaseOpenViewW", {I(db), I(w(u"SELECT * FROM `N6`")), I(OUT)}), 0u);
    ASSERT_EQ(call("MsiViewGetColumnInfo", {I(static_cast<u32>(read_int(OUT))), I(1), I(OUT2)}), 0u);
    EXPECT_EQ(rec_text(static_cast<u32>(read_int(OUT2))), "i2|I2|L8|S0|s0|I4");
    // Temporary tables live while held.
    EXPECT_EQ(exec(db, "CREATE TABLE `TT` (`X` SHORT TEMPORARY PRIMARY KEY `X`)"), 0u);
    EXPECT_EQ(exec(db, "INSERT INTO `TT` (`X`) VALUES (1) TEMPORARY"), 1615u);
    EXPECT_EQ(exec(db, "CREATE TABLE `H` (`X` SHORT TEMPORARY, `Y` CHAR(5) TEMPORARY PRIMARY KEY `X`) HOLD"), 0u);
    EXPECT_EQ(exec(db, "INSERT INTO `H` (`X`,`Y`) VALUES (1,'y') TEMPORARY"), 0u);
    EXPECT_EQ(call("MsiDatabaseIsTablePersistentW", {I(db), I(w(u"H"))}), 0u);
    EXPECT_EQ(exec(db, "ALTER TABLE `H` FREE"), 0u);
    EXPECT_EQ(exec(db, "SELECT * FROM `H`"), 1615u);
    EXPECT_EQ(exec(db, "ALTER TABLE `U` ADD `NN` SHORT NOT NULL"), 1627u);
    EXPECT_EQ(last_error(), "2280|C:\\sql.msi|NN|U");
    EXPECT_EQ(exec(db, "DROP TABLE `_Tables`"), 1627u);
    EXPECT_EQ(last_error().substr(0, 4), "2205");
    EXPECT_EQ(call("MsiDatabaseIsTablePersistentW", {I(db), I(w(u"T"))}), 1u);
    EXPECT_EQ(call("MsiDatabaseIsTablePersistentW", {I(db), I(w(u"_Tables"))}), 2u);
    EXPECT_EQ(call("MsiDatabaseIsTablePersistentW", {I(db), I(w(u"Nope"))}), 2u);
    EXPECT_EQ(call("MsiDatabaseIsTablePersistentW", {I(0), I(w(u"T"))}), 3u);
}

TEST_P(Msi_database_Test, FetchCursorWrapsAfterTheEnd) {
    const u32 db = sql_db();
    write_int(OUT, 0);
    ASSERT_EQ(call("MsiDatabaseOpenViewW", {I(db), I(w(u"SELECT `A` FROM `U` WHERE `A` = 1")), I(OUT)}), 0u);
    const u32 v = static_cast<u32>(read_int(OUT));
    ASSERT_EQ(call("MsiViewExecute", {I(v), I(0)}), 0u);
    EXPECT_EQ(call("MsiViewFetch", {I(v), I(OUT2)}), 0u);
    EXPECT_EQ(call("MsiViewFetch", {I(v), I(OUT2)}), 0u);
    EXPECT_EQ(call("MsiViewFetch", {I(v), I(OUT2)}), 259u);
    EXPECT_EQ(read_int(OUT2), 0);
    EXPECT_EQ(call("MsiViewFetch", {I(v), I(OUT2)}), 0u);
    EXPECT_EQ(call("MsiViewClose", {I(v)}), 0u);
    EXPECT_EQ(call("MsiViewClose", {I(v)}), 0u);
    EXPECT_EQ(call("MsiViewFetch", {I(v), I(OUT2)}), 1627u);
    EXPECT_EQ(call("MsiViewFetch", {I(v), I(0)}), 87u);
    EXPECT_EQ(call("MsiViewExecute", {I(0), I(0)}), 6u);
    EXPECT_EQ(call("MsiDatabaseOpenViewW", {I(db), I(0), I(OUT)}), 1615u);
    EXPECT_EQ(call("MsiDatabaseOpenViewW", {I(db), I(w(u"SELECT `A` FROM `U`")), I(0)}), 87u);
    EXPECT_EQ(call("MsiDatabaseOpenViewW", {I(0), I(w(u"SELECT `A` FROM `U`")), I(OUT)}), 6u);
}

// ---- persistence (msi_database_format_probe) ---------------------------------------------

TEST_P(Msi_database_Test, CommitWritesWhatWindowsReadsBack) {
    u32 db = open("C:\\fmt.msi", 3);
    EXPECT_EQ(call("MsiGetDatabaseState", {I(db)}), 1u);
    EXPECT_EQ(exec(db, "CREATE TABLE `Z` (`N` CHAR(10) NOT NULL PRIMARY KEY `N`)"), 0u);
    for (const char* n : {"a", "b", "c"}) EXPECT_EQ(exec(db, std::string("INSERT INTO `Z` (`N`) VALUES ('") + n + "')"), 0u);
    EXPECT_EQ(exec(db, "CREATE TABLE `W` (`N` CHAR(10) NOT NULL PRIMARY KEY `N`)"), 0u);
    for (const char* n : {"m", "k", "c"}) EXPECT_EQ(exec(db, std::string("INSERT INTO `W` (`N`) VALUES ('") + n + "')"), 0u);
    // Rows sort by key pool index: c was pooled first.
    EXPECT_EQ(query(db, "SELECT * FROM `W`"), (V{"c", "m", "k"}));
    EXPECT_EQ(exec(db, "CREATE TABLE `T` (`K` CHAR(72) NOT NULL, `S` CHAR(0), `L` LONGCHAR LOCALIZABLE, `I2` SHORT, `I4` LONG NOT NULL, `B` OBJECT PRIMARY KEY `K`)"), 0u);
    EXPECT_EQ(exec(db, "INSERT INTO `T` (`K`,`S`,`L`,`I2`,`I4`) VALUES ('c','sc','lc',3,-3)"), 0u);
    EXPECT_EQ(exec(db, "INSERT INTO `T` (`K`,`S`,`I2`,`I4`) VALUES ('a','sa',-32767,2147483647)"), 0u);
    host_file("blob.bin", "hello");
    write_int(OUT, 0);
    ASSERT_EQ(call("MsiDatabaseOpenViewW", {I(db), I(w(u"SELECT `K`,`B`,`I4` FROM `T`")), I(OUT)}), 0u);
    const u32 v = static_cast<u32>(read_int(OUT));
    ASSERT_EQ(call("MsiViewExecute", {I(v), I(0)}), 0u);
    const u32 r = record({std::string("d"), std::nullopt, "9"});
    ASSERT_EQ(call("MsiRecordSetStreamW", {I(r), I(2), I(w(u"C:\\blob.bin"))}), 0u);
    EXPECT_EQ(call("MsiViewModify", {I(v), I(1), I(r)}), 0u);
    EXPECT_EQ(exec(db, "INSERT INTO `T` (`K`,`I4`) VALUES ('tmp',7) TEMPORARY"), 0u);
    EXPECT_EQ(exec(db, "ALTER TABLE `T` ADD `TMP` SHORT TEMPORARY"), 0u);
    EXPECT_EQ(query(db, "SELECT `Name` FROM `_Streams`"), V{"T.d"});
    EXPECT_EQ(query(db, "SELECT * FROM `_Columns` WHERE `Table` = 'T'"),
              (V{"T|1|K|11592", "T|2|S|7424", "T|3|L|7936", "T|4|I2|5378", "T|5|I4|260", "T|6|B|6400"}));
    EXPECT_EQ(call("MsiDatabaseCommit", {I(db)}), 0u);
    EXPECT_EQ(query(db, "SELECT `K` FROM `T`"), (V{"a", "c", "d", "tmp"}));
    call("MsiCloseHandle", {I(db)});
    db = open("C:\\fmt.msi", 0);
    EXPECT_EQ(call("MsiGetDatabaseState", {I(db)}), 0u);
    EXPECT_EQ(query(db, "SELECT * FROM `T`"), (V{"a|sa|~|-32767|2147483647|~", "c|sc|lc|3|-3|~", "d|~|~|~|9|<s5>"}));
    EXPECT_EQ(query(db, "SELECT * FROM `W`"), (V{"c", "m", "k"}));
    EXPECT_EQ(query(db, "SELECT `Name` FROM `_Tables`"), (V{"Z", "W", "T"}));
    EXPECT_EQ(call("MsiDatabaseCommit", {I(db)}), 0u);  // read-only: a no-op
    EXPECT_EQ(exec(db, "INSERT INTO `Z` (`N`) VALUES ('q')"), 1627u);
    EXPECT_EQ(exec(db, "INSERT INTO `Z` (`N`) VALUES ('q') TEMPORARY"), 0u);
    // The written file is a compound file with the database CLSID.
    const std::string image = host_read("fmt.msi");
    auto pkg = msi::Package::open(std::span<const u8>(reinterpret_cast<const u8*>(image.data()), image.size()));
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->clsid(), msi::kClsidDatabase);
    if (const char* out = std::getenv("VITREO_MSI_WRITE_SAMPLE")) {
        std::ofstream(std::string(out) + (x64() ? "64.msi" : "32.msi"), std::ios::binary) << image;
    }
}

TEST_P(Msi_database_Test, OpenModes) {
    write_int(OUT, 0);
    EXPECT_EQ(call("MsiOpenDatabaseW", {I(w(u"C:\\missing.msi")), I(0), I(OUT)}), 110u);
    host_file("junk.msi", "not a database");
    EXPECT_EQ(call("MsiOpenDatabaseW", {I(w(u"C:\\junk.msi")), I(0), I(OUT)}), 110u);
    EXPECT_EQ(call("MsiOpenDatabaseW", {I(w(u"C:\\x.msi")), I(0), I(0)}), 87u);
    EXPECT_EQ(call("MsiOpenDatabaseW", {I(0), I(0), I(OUT)}), 87u);
    u32 db = open("C:\\c.msi", 3);
    EXPECT_TRUE(std::filesystem::exists(root / "c.msi"));  // created at once
    call("MsiCloseHandle", {I(db)});
    EXPECT_FALSE(std::filesystem::exists(root / "c.msi"));  // and gone without a commit
    db = open("C:\\m.msi", 3);
    EXPECT_EQ(exec(db, "CREATE TABLE `M` (`K` CHAR(10) NOT NULL PRIMARY KEY `K`)"), 0u);
    EXPECT_EQ(call("MsiDatabaseCommit", {I(db)}), 0u);
    call("MsiCloseHandle", {I(db)});
    db = open("C:\\m.msi", 1);  // transact: discarded without commit
    EXPECT_EQ(exec(db, "INSERT INTO `M` (`K`) VALUES ('tx')"), 0u);
    call("MsiCloseHandle", {I(db)});
    db = open("C:\\m.msi", 0);
    EXPECT_EQ(query(db, "SELECT `K` FROM `M`"), V{});
    call("MsiCloseHandle", {I(db)});
    db = open("C:\\m.msi", 2);  // direct: written on close
    EXPECT_EQ(exec(db, "INSERT INTO `M` (`K`) VALUES ('dr')"), 0u);
    call("MsiCloseHandle", {I(db)});
    db = open("C:\\m.msi", 0);
    EXPECT_EQ(query(db, "SELECT `K` FROM `M`"), V{"dr"});
    call("MsiCloseHandle", {I(db)});
    write_int(OUT, 0);
    ASSERT_EQ(call("MsiOpenDatabaseW", {I(w(u"C:\\m.msi")), I(w(u"C:\\out.msi")), I(OUT)}), 0u);
    db = static_cast<u32>(read_int(OUT));
    EXPECT_TRUE(std::filesystem::exists(root / "out.msi"));
    EXPECT_EQ(call("MsiGetDatabaseState", {I(db)}), 1u);
    EXPECT_EQ(exec(db, "INSERT INTO `M` (`K`) VALUES ('out')"), 0u);
    EXPECT_EQ(call("MsiDatabaseCommit", {I(db)}), 0u);
    call("MsiCloseHandle", {I(db)});
    db = open("C:\\out.msi", 0);
    EXPECT_EQ(query(db, "SELECT `K` FROM `M`"), (V{"dr", "out"}));
    call("MsiCloseHandle", {I(db)});
    db = open("C:\\m.msi", 0);
    EXPECT_EQ(query(db, "SELECT `K` FROM `M`"), V{"dr"});
    EXPECT_EQ(static_cast<u32>(call("MsiGetDatabaseState", {I(0)})), 0xFFFFFFFFu);
    EXPECT_EQ(call("MsiDatabaseCommit", {I(0)}), 6u);
}

TEST_P(Msi_database_Test, PrimaryKeys) {
    const u32 db = sql_db();
    ASSERT_EQ(call("MsiDatabaseGetPrimaryKeysW", {I(db), I(w(u"U")), I(OUT)}), 0u);
    const u32 r = static_cast<u32>(read_int(OUT));
    EXPECT_EQ(field(r, 0), "U");
    EXPECT_EQ(rec_text(r), "A|B");
    EXPECT_EQ(call("MsiDatabaseGetPrimaryKeysW", {I(db), I(w(u"_Tables")), I(OUT)}), 1628u);
    EXPECT_EQ(call("MsiDatabaseGetPrimaryKeysW", {I(db), I(w(u"u")), I(OUT)}), 1628u);
    EXPECT_EQ(call("MsiDatabaseGetPrimaryKeysW", {I(0), I(w(u"U")), I(OUT)}), 6u);
    EXPECT_EQ(call("MsiDatabaseGetPrimaryKeysW", {I(db), I(w(u"U")), I(0)}), 87u);
    EXPECT_EQ(call("MsiDatabaseGetPrimaryKeysA", {I(db), I(a("T")), I(OUT)}), 0u);
}

// ---- MsiViewModify (msi_database_modify_probe) --------------------------------------------

TEST_P(Msi_database_Test, ViewModifyModes) {
    const u32 db = open("C:\\mod.msi", 3);
    ASSERT_EQ(exec(db, "CREATE TABLE `M` (`K` CHAR(10) NOT NULL, `V` SHORT, `S` CHAR(10) PRIMARY KEY `K`)"), 0u);
    ASSERT_EQ(exec(db, "INSERT INTO `M` (`K`,`V`,`S`) VALUES ('a',1,'x')"), 0u);
    ASSERT_EQ(exec(db, "INSERT INTO `M` (`K`,`V`,`S`) VALUES ('b',2,'y')"), 0u);
    write_int(OUT, 0);
    ASSERT_EQ(call("MsiDatabaseOpenViewW", {I(db), I(w(u"SELECT `K`,`V`,`S` FROM `M`")), I(OUT)}), 0u);
    const u32 v = static_cast<u32>(read_int(OUT));
    ASSERT_EQ(call("MsiViewExecute", {I(v), I(0)}), 0u);
    ASSERT_EQ(call("MsiViewFetch", {I(v), I(OUT2)}), 0u);
    u32 r = static_cast<u32>(read_int(OUT2));
    EXPECT_EQ(call("MsiRecordIsNull", {I(r), I(0)}), 0u);  // the row cookie
    auto mod = [&](i32 mode, u32 rec) { return static_cast<u32>(call("MsiViewModify", {I(v), I(static_cast<u32>(mode)), I(rec)})); };
    EXPECT_EQ(mod(0, r), 0u);
    call("MsiRecordSetInteger", {I(r), I(2), I(10)});
    EXPECT_EQ(mod(2, r), 0u);
    call("MsiRecordSetStringW", {I(r), I(1), I(w(u"a2"))});
    EXPECT_EQ(mod(2, r), 1627u);  // key changed
    EXPECT_EQ(mod(4, r), 0u);     // replace: delete + insert
    EXPECT_EQ(query(db, "SELECT * FROM `M`"), (V{"b|2|y", "a2|10|x"}));
    EXPECT_EQ(mod(6, r), 0u);
    EXPECT_EQ(mod(6, r), 1627u);
    EXPECT_EQ(mod(2, r), 1627u);
    EXPECT_EQ(mod(0, r), 1627u);
    r = record({std::string("c"), "3", std::nullopt});
    EXPECT_EQ(mod(1, r), 0u);
    EXPECT_EQ(mod(1, r), 1627u);
    EXPECT_EQ(mod(3, r), 0u);  // assign existing
    call("MsiRecordSetStringW", {I(r), I(1), I(w(u"d"))});
    EXPECT_EQ(mod(3, r), 0u);  // assign new
    EXPECT_EQ(mod(5, r), 0u);  // merge identical
    call("MsiRecordSetInteger", {I(r), I(2), I(44)});
    EXPECT_EQ(mod(5, r), 1627u);
    call("MsiRecordSetStringW", {I(r), I(1), I(w(u"t"))});
    EXPECT_EQ(mod(7, r), 0u);
    const u32 seek = record({std::string("b"), std::nullopt, std::nullopt});
    EXPECT_EQ(mod(-1, seek), 0u);
    EXPECT_EQ(rec_text(seek), "b|2|y");
    call("MsiRecordSetStringW", {I(seek), I(1), I(w(u"zz"))});
    EXPECT_EQ(mod(-1, seek), 1627u);
    const u32 lenient = record({std::string("h"), std::string("notint"), std::nullopt});
    EXPECT_EQ(mod(1, lenient), 0u);  // a non-number in an integer column reads null
    EXPECT_EQ(query(db, "SELECT `V` FROM `M` WHERE `K` = 'h'"), V{"~"});
    const u32 nullkey = record({std::nullopt, "1", std::nullopt});
    EXPECT_EQ(mod(1, nullkey), 1627u);
    EXPECT_EQ(last_error(), "2210|C:\\mod.msi|0");
    EXPECT_EQ(mod(12, r), 87u);
    EXPECT_EQ(mod(-2, r), 87u);
    EXPECT_EQ(mod(1, 0), 6u);
    const u32 unfetched = record({std::string("b"), std::nullopt, std::nullopt});
    for (const i32 m : {2, 6, 0, 4}) EXPECT_EQ(mod(m, unfetched), 1627u) << m;
    EXPECT_EQ(call("MsiViewClose", {I(v)}), 0u);
    const u32 after_close = record({std::string("q"), std::nullopt, std::nullopt});
    EXPECT_EQ(mod(1, after_close), 0u);
    // Validation needs a _Validation table.
    EXPECT_EQ(mod(9, after_close), 1627u);
}

TEST_P(Msi_database_Test, ValidationCategoriesMatchWindows) {
    using msi::validate_category;
    struct C { const char* category; const char* value; i32 error; };
    for (const C& c : std::initializer_list<C>{
             {"Text", "anything at all", 0}, {"UpperCase", "ABC_1", 0}, {"UpperCase", "AbC", 8}, {"UpperCase", "123", 0},
             {"LowerCase", "aBc", 8}, {"Identifier", "_a1.B", 0}, {"Identifier", "1a", 11}, {"Identifier", "a-b", 11},
             {"Identifier", ".a", 11}, {"Property", "%ENVVAR", 0}, {"Property", "[x]", 21}, {"Property", "a.b", 0},
             {"Filename", "abc.txt", 0}, {"Filename", "ABCDEF~1.TXT|Long name.text", 0}, {"Filename", "Long name.text", 13},
             {"Filename", "abcdefghi.txt|long.txt", 13}, {"Filename", "a*b.txt", 13}, {"Filename", "abc.text", 13},
             {"Filename", "a.b.c", 13}, {"Filename", "a b.txt", 13}, {"Filename", "abc.", 0}, {"Filename", "a+b.txt", 13},
             {"Filename", ".", 13}, {"Filename", "abc|a*b", 13}, {"WildCardFilename", "a?c.t*", 0},
             {"WildCardFilename", "a\\*.txt", 10}, {"Path", "C:\\dir\\file", 0}, {"Path", "\\\\server\\share\\x", 0},
             {"Path", "dir\\file", 14}, {"Path", "[ProgramFilesFolder]x", 0}, {"Path", "C:", 0}, {"Path", "C:x", 14},
             {"Paths", "C:\\a;D:\\b", 0}, {"AnyPath", "dir\\file", 0}, {"AnyPath", "a*b", 14}, {"AnyPath", "a|b|c", 14},
             {"DefaultDir", "SourceDir", 0}, {"DefaultDir", ".", 18}, {"DefaultDir", "PROGRA~1|Program Files", 18},
             {"RegPath", "Software\\Vendor\\[ProductName]", 0}, {"RegPath", "\\Software", 19}, {"RegPath", "a\\\\b", 19},
             {"Formatted", "[Prop] text [\\[] [~] [#file]", 0}, {"Formatted", "[Prop", 16}, {"Formatted", "Prop]", 16},
             {"Formatted", "[]", 16}, {"Formatted", "[[a]]", 0}, {"Template", "Value [1] and [2]", 0}, {"Template", "[]", 17},
             {"Condition", "VersionNT >= 500 AND NOT Installed", 0}, {"Condition", "a = ", 15}, {"Condition", "(a", 15},
             {"Guid", "{01234567-89AB-CDEF-0123-456789ABCDEF}", 0}, {"Guid", "{01234567-89ab-cdef-0123-456789abcdef}", 9},
             {"Guid", "01234567-89AB-CDEF-0123-456789ABCDEF", 9}, {"GUID", "{01234567-89AB-CDEF-0123-456789ABCDEF}", 23},
             {"Version", "1.2.3.4", 0}, {"Version", "1.2.3.4.5", 7}, {"Version", "65536.0", 7}, {"Version", "1.", 7},
             {"Language", "1033,1031", 0}, {"Language", "1033, 1031", 0}, {"Language", "abc", 12}, {"Language", "70000", 12},
             {"Language", "-1", 12}, {"CustomSource", "a b", 0}, {"Cabinet", "#data1.cab", 0}, {"Cabinet", "a*b.cab", 26},
             {"Cabinet", "averylongname.cab", 0}, {"Shortcut", "[INSTALLDIR]app.exe", 0}, {"Shortcut", "a b", 27},
             {"Binary", "x", 23}, {"text", "x", 23}, {"Integer", "x", 23}}) {
        EXPECT_EQ(validate_category(c.category, c.value, false), c.error) << c.category << " " << c.value;
    }
}

TEST_P(Msi_database_Test, ValidateThroughViewModify) {
    const u32 db = open("C:\\val.msi", 3);
    ASSERT_EQ(exec(db, "CREATE TABLE `_Validation` (`Table` CHAR(32) NOT NULL, `Column` CHAR(32) NOT NULL, `Nullable` CHAR(4) NOT NULL, "
                       "`MinValue` LONG, `MaxValue` LONG, `KeyTable` CHAR(255), `KeyColumn` SHORT, `Category` CHAR(32), `Set` CHAR(255), "
                       "`Description` CHAR(255) PRIMARY KEY `Table`, `Column`)"), 0u);
    ASSERT_EQ(exec(db, "CREATE TABLE `VS` (`K` CHAR(72) NOT NULL, `C` CHAR(5) PRIMARY KEY `K`)"), 0u);
    ASSERT_EQ(exec(db, "INSERT INTO `_Validation` (`Table`,`Column`,`Nullable`,`Category`) VALUES ('VS','K','N','Identifier')"), 0u);
    ASSERT_EQ(exec(db, "INSERT INTO `_Validation` (`Table`,`Column`,`Nullable`,`Category`) VALUES ('VS','C','Y','Text')"), 0u);
    write_int(OUT, 0);
    ASSERT_EQ(call("MsiDatabaseOpenViewW", {I(db), I(w(u"SELECT `K`,`C` FROM `VS`")), I(OUT)}), 0u);
    const u32 v = static_cast<u32>(read_int(OUT));
    ASSERT_EQ(call("MsiViewExecute", {I(v), I(0)}), 0u);
    auto errors = [&] {
        std::string s;
        for (;;) {
            write_int(OUT2, 64);
            const i32 e = static_cast<i32>(call("MsiViewGetErrorW", {I(v), I(BUF), I(OUT2)}));
            std::string col;
            for (u32 k = 0; memory.read_u16(BUF + k * 2).value(); ++k) col.push_back(static_cast<char>(memory.read_u16(BUF + k * 2).value()));
            s += std::to_string(e) + ":" + col + " ";
            if (e <= 0) return s;
        }
    };
    const u32 r = record({std::string("1bad"), std::string("1234567")});
    EXPECT_EQ(call("MsiViewModify", {I(v), I(9), I(r)}), 13u);
    EXPECT_EQ(errors(), "11:K 28:C 0: ");
    call("MsiRecordSetStringW", {I(r), I(1), I(w(u"ok"))});
    call("MsiRecordSetStringW", {I(r), I(2), I(w(u"fine"))});
    EXPECT_EQ(call("MsiViewModify", {I(v), I(9), I(r)}), 0u);
    EXPECT_EQ(call("MsiViewModify", {I(v), I(1), I(r)}), 0u);
    EXPECT_EQ(call("MsiViewModify", {I(v), I(9), I(r)}), 13u);
    EXPECT_EQ(errors(), "1:K 0: ");
    const u32 fresh = record({std::string("k2"), std::string("x")});
    EXPECT_EQ(call("MsiViewModify", {I(v), I(8), I(fresh)}), 1627u);  // VALIDATE needs a fetched record
    EXPECT_EQ(call("MsiViewModify", {I(v), I(9), I(r)}), 13u);
    write_int(OUT2, 1);
    EXPECT_EQ(static_cast<i32>(call("MsiViewGetErrorW", {I(v), I(BUF), I(OUT2)})), -2);  // MOREDATA consumes it
    EXPECT_EQ(static_cast<i32>(call("MsiViewGetErrorW", {I(v), I(BUF), I(0)})), -3);
    EXPECT_EQ(static_cast<i32>(call("MsiViewGetErrorW", {I(0), I(BUF), I(OUT2)})), -3);
}

// ---- summary information (msi_database_record_probe, sicount probe) ---------------------

TEST_P(Msi_database_Test, SummaryInformation) {
    u32 db = open("C:\\si.msi", 3);
    ASSERT_EQ(call("MsiGetSummaryInformationW", {I(db), I(0), I(0), I(OUT)}), 0u);
    u32 si = static_cast<u32>(read_int(OUT));
    write_int(OUT2, 99);
    EXPECT_EQ(call("MsiSummaryInfoGetPropertyCount", {I(si), I(OUT2)}), 0u);
    EXPECT_EQ(read_int(OUT2), 0);
    EXPECT_EQ(call("MsiSummaryInfoSetPropertyW", {I(si), I(2), I(30), I(0), I(0), I(w(u"t"))}), 1627u);  // read-only
    write_int(OUT2, 0xdead);
    EXPECT_EQ(call("MsiSummaryInfoGetPropertyW", {I(si), I(2), I(OUT2), I(0), I(0), I(0), I(0)}), 0u);
    EXPECT_EQ(read_int(OUT2), 0);  // VT_EMPTY
    EXPECT_EQ(call("MsiSummaryInfoGetPropertyW", {I(si), I(20), I(OUT2), I(0), I(0), I(0), I(0)}), 1608u);
    call("MsiCloseHandle", {I(si)});
    ASSERT_EQ(call("MsiGetSummaryInformationW", {I(db), I(0), I(3), I(OUT)}), 0u);
    si = static_cast<u32>(read_int(OUT));
    const GuestAddr ft = next; next += 16;
    ASSERT_TRUE(memory.write_u32(ft, 0x89abcdef).is_ok());
    ASSERT_TRUE(memory.write_u32(ft + 4, 0x01d00000).is_ok());
    auto set = [&](u32 pid, u32 vt, u32 iv, GuestAddr pft, GuestAddr psz) {
        return static_cast<u32>(call("MsiSummaryInfoSetPropertyW", {I(si), I(pid), I(vt), I(iv), I(pft), I(psz)}));
    };
    EXPECT_EQ(set(1, 2, 1252, 0, 0), 0u);
    EXPECT_EQ(set(1, 3, 1252, 0, 0), 0u);
    EXPECT_EQ(set(2, 3, 5, 0, 0), 1629u);
    EXPECT_EQ(set(14, 30, 0, 0, w(u"5")), 1629u);
    EXPECT_EQ(set(0, 3, 5, 0, 0), 1629u);
    EXPECT_EQ(set(17, 3, 5, 0, 0), 1629u);
    EXPECT_EQ(set(20, 3, 5, 0, 0), 1608u);
    EXPECT_EQ(set(12, 64, 0, ft, 0), 0u);
    EXPECT_EQ(set(12, 64, 0, 0, 0), 87u);
    EXPECT_EQ(set(2, 30, 0, 0, 0), 87u);
    EXPECT_EQ(set(3, 30, 0, 0, w(u"Subject")), 0u);
    EXPECT_EQ(set(4, 30, 0, 0, w(u"Author")), 1627u);  // a fourth property exceeds uiUpdateCount 3
    EXPECT_EQ(set(3, 0, 0, 0, 0), 0u);                 // VT_EMPTY removes it
    write_int(OUT2, 99);
    EXPECT_EQ(call("MsiSummaryInfoGetPropertyCount", {I(si), I(OUT2)}), 0u);
    EXPECT_EQ(read_int(OUT2), 2);
    write_int(OUT2, 0);
    write_int(OUT3, 0);
    EXPECT_EQ(call("MsiSummaryInfoGetPropertyW", {I(si), I(1), I(OUT2), I(OUT3), I(0), I(0), I(0)}), 0u);
    EXPECT_EQ(read_int(OUT2), 2);
    EXPECT_EQ(read_int(OUT3), 1252);
    EXPECT_EQ(call("MsiSummaryInfoPersist", {I(si)}), 0u);
    EXPECT_EQ(call("MsiSummaryInfoPersist", {I(si)}), 1627u);  // nothing changed since
    call("MsiCloseHandle", {I(si)});
    EXPECT_EQ(call("MsiDatabaseCommit", {I(db)}), 0u);
    call("MsiCloseHandle", {I(db)});
    ASSERT_EQ(call("MsiGetSummaryInformationW", {I(0), I(w(u"C:\\si.msi")), I(2), I(OUT)}), 0u);
    si = static_cast<u32>(read_int(OUT));
    write_int(OUT2, 0);
    const GuestAddr ftout = next; next += 16;
    EXPECT_EQ(call("MsiSummaryInfoGetPropertyW", {I(si), I(12), I(OUT2), I(0), I(ftout), I(0), I(0)}), 0u);
    EXPECT_EQ(read_int(OUT2), 64);
    EXPECT_EQ(memory.read_u32(ftout).value(), 0x89abcdefu);
    EXPECT_EQ(set(9, 30, 0, 0, w(u"{REV}")), 0u);
    EXPECT_EQ(call("MsiSummaryInfoPersist", {I(si)}), 0u);  // written to the file
    call("MsiCloseHandle", {I(si)});
    ASSERT_EQ(call("MsiGetSummaryInformationW", {I(0), I(w(u"C:\\si.msi")), I(0), I(OUT)}), 0u);
    si = static_cast<u32>(read_int(OUT));
    write_int(OUT3, 64);
    EXPECT_EQ(call("MsiSummaryInfoGetPropertyW", {I(si), I(9), I(OUT2), I(0), I(0), I(BUF), I(OUT3)}), 0u);
    EXPECT_EQ(read_int(OUT2), 30);
    EXPECT_EQ(read_int(OUT3), 5);
    EXPECT_EQ(call("MsiGetSummaryInformationW", {I(0), I(w(u"C:\\nope.msi")), I(0), I(OUT)}), 1619u);
    EXPECT_EQ(call("MsiGetSummaryInformationW", {I(0), I(0), I(0), I(OUT)}), 87u);
    EXPECT_EQ(call("MsiGetSummaryInformationW", {I(0), I(w(u"C:\\si.msi")), I(0), I(0)}), 87u);
    EXPECT_EQ(call("MsiSummaryInfoGetPropertyCount", {I(0), I(OUT2)}), 6u);
}

// ---- transforms, import / export, merge ---------------------------------------------------

TEST_P(Msi_database_Test, TransformRoundTrip) {
    auto make = [&](const std::string& path, bool newer) {
        const u32 db = open(path, 3);
        EXPECT_EQ(exec(db, "CREATE TABLE `Property` (`Property` CHAR(72) NOT NULL, `Value` LONGCHAR NOT NULL LOCALIZABLE PRIMARY KEY `Property`)"), 0u);
        EXPECT_EQ(exec(db, "CREATE TABLE `Num` (`K` SHORT NOT NULL, `S` CHAR(10), `L` LONG PRIMARY KEY `K`)"), 0u);
        EXPECT_EQ(exec(db, "INSERT INTO `Property` (`Property`,`Value`) VALUES ('ProductVersion','1.0.0')"), 0u);
        EXPECT_EQ(exec(db, "INSERT INTO `Property` (`Property`,`Value`) VALUES ('Gone','bye')"), 0u);
        EXPECT_EQ(exec(db, "INSERT INTO `Num` (`K`,`S`,`L`) VALUES (2,'two',200)"), 0u);
        if (newer) {
            EXPECT_EQ(exec(db, "UPDATE `Property` SET `Value` = '2.0.0' WHERE `Property` = 'ProductVersion'"), 0u);
            EXPECT_EQ(exec(db, "DELETE FROM `Property` WHERE `Property` = 'Gone'"), 0u);
            EXPECT_EQ(exec(db, "INSERT INTO `Property` (`Property`,`Value`) VALUES ('Added','new')"), 0u);
            EXPECT_EQ(exec(db, "UPDATE `Num` SET `L` = 222, `S` = '' WHERE `K` = 2"), 0u);
            EXPECT_EQ(exec(db, "CREATE TABLE `Extra` (`E` CHAR(5) NOT NULL, `X` SHORT PRIMARY KEY `E`)"), 0u);
            EXPECT_EQ(exec(db, "INSERT INTO `Extra` (`E`,`X`) VALUES ('e1',5)"), 0u);
            EXPECT_EQ(exec(db, "ALTER TABLE `Num` ADD `New` SHORT"), 0u);
        } else {
            EXPECT_EQ(exec(db, "CREATE TABLE `DropMe` (`D` SHORT NOT NULL PRIMARY KEY `D`)"), 0u);
        }
        EXPECT_EQ(call("MsiDatabaseCommit", {I(db)}), 0u);
        return db;
    };
    const u32 base = make("C:\\base.msi", false);
    const u32 newer = make("C:\\new.msi", true);
    EXPECT_EQ(call("MsiDatabaseGenerateTransformW", {I(newer), I(base), I(w(u"C:\\t.mst")), I(0), I(0)}), 0u);
    EXPECT_EQ(call("MsiDatabaseGenerateTransformW", {I(base), I(base), I(0), I(0), I(0)}), 232u);
    EXPECT_EQ(call("MsiCreateTransformSummaryInfoW", {I(newer), I(base), I(w(u"C:\\t.mst")), I(3), I(3)}), 0u);
    EXPECT_EQ(call("MsiCreateTransformSummaryInfoW", {I(newer), I(base), I(w(u"C:\\missing.mst")), I(0), I(0)}), 1619u);
    ASSERT_EQ(call("MsiGetSummaryInformationW", {I(0), I(w(u"C:\\t.mst")), I(0), I(OUT)}), 0u);
    write_int(OUT3, 0);
    EXPECT_EQ(call("MsiSummaryInfoGetPropertyW", {I(static_cast<u32>(read_int(OUT))), I(16), I(OUT2), I(OUT3), I(0), I(0), I(0)}), 0u);
    EXPECT_EQ(read_int(OUT3), 0x00030003);
    // Apply to a copy of the base.
    std::filesystem::copy_file(root / "base.msi", root / "apply.msi");
    u32 target = open("C:\\apply.msi", 1);
    EXPECT_EQ(call("MsiDatabaseApplyTransformW", {I(target), I(w(u"C:\\t.mst")), I(0)}), 0u);
    EXPECT_EQ(query(target, "SELECT * FROM `Property`"), (V{"ProductVersion|2.0.0", "Added|new"}));
    EXPECT_EQ(query(target, "SELECT * FROM `Num`"), V{"2|~|222|~"});
    EXPECT_EQ(query(target, "SELECT * FROM `Extra`"), V{"e1|5"});
    EXPECT_EQ(call("MsiDatabaseIsTablePersistentW", {I(target), I(w(u"DropMe"))}), 2u);
    // Applying again: the table it adds already exists (2252).
    EXPECT_EQ(call("MsiDatabaseApplyTransformW", {I(target), I(w(u"C:\\t.mst")), I(0)}), 1624u);
    EXPECT_EQ(last_error().substr(0, 4), "2252");
    EXPECT_EQ(call("MsiDatabaseApplyTransformW", {I(target), I(w(u"C:\\t.mst")), I(0x3f)}), 0u);
    EXPECT_EQ(call("MsiDatabaseApplyTransformW", {I(target), I(w(u"C:\\nope.mst")), I(0)}), 110u);
    EXPECT_EQ(call("MsiDatabaseApplyTransformW", {I(target), I(w(u"C:\\base.msi")), I(0)}), 1624u);
    EXPECT_EQ(last_error().substr(0, 4), "2246");
    EXPECT_EQ(call("MsiDatabaseApplyTransformW", {I(0), I(w(u"C:\\t.mst")), I(0)}), 6u);
    // MSITRANSFORM_ERROR_VIEWTRANSFORM shows the transform instead of applying it.
    const u32 viewdb = open("C:\\base.msi", 0);
    EXPECT_EQ(call("MsiDatabaseApplyTransformW", {I(viewdb), I(w(u"C:\\t.mst")), I(0x100)}), 0u);
    EXPECT_EQ(query(viewdb, "SELECT `Value` FROM `Property` WHERE `Property` = 'ProductVersion'"), V{"1.0.0"});
    const V tv = query(viewdb, "SELECT * FROM `_TransformView`");
    auto has = [&](const std::string& row) { return std::find(tv.begin(), tv.end(), row) != tv.end(); };
    EXPECT_TRUE(has("Property|Value|ProductVersion|2.0.0|1.0.0"));
    EXPECT_TRUE(has("Property|INSERT|Added|~|~"));
    EXPECT_TRUE(has("Property|DELETE|Gone|~|~"));
    EXPECT_TRUE(has("Num|L|2|222|200"));
    EXPECT_TRUE(has("Num|New|~|5378|4"));
    EXPECT_TRUE(has("Extra|CREATE|~|~|~"));
    EXPECT_TRUE(has("Extra|E|~|11525|1"));
    EXPECT_TRUE(has("DropMe|DROP|~|~|~"));
}

TEST_P(Msi_database_Test, ImportExportArchives) {
    u32 db = open("C:\\e.msi", 3);
    ASSERT_EQ(exec(db, "CREATE TABLE `T` (`K` CHAR(10) NOT NULL, `I` SHORT, `L` LONG NOT NULL, `S` LONGCHAR LOCALIZABLE, `B` OBJECT PRIMARY KEY `K`, `I`)"), 0u);
    host_file("blob.bin", "BLOBDATA");
    write_int(OUT, 0);
    ASSERT_EQ(call("MsiDatabaseOpenViewW", {I(db), I(w(u"SELECT `K`,`I`,`L`,`S`,`B` FROM `T`")), I(OUT)}), 0u);
    const u32 v = static_cast<u32>(read_int(OUT));
    ASSERT_EQ(call("MsiViewExecute", {I(v), I(0)}), 0u);
    const u32 r1 = record({std::string("a"), "1", "-7", std::string("tab\there\r\nnew\nline\\back\xc3\xa9"), std::nullopt});
    ASSERT_EQ(call("MsiViewModify", {I(v), I(1), I(r1)}), 0u);
    const u32 r2 = record({std::string("b"), std::nullopt, "2", std::nullopt, std::nullopt});
    ASSERT_EQ(call("MsiRecordSetStreamW", {I(r2), I(5), I(w(u"C:\\blob.bin"))}), 0u);
    ASSERT_EQ(call("MsiViewModify", {I(v), I(1), I(r2)}), 0u);
    std::filesystem::create_directories(root / "idt");
    EXPECT_EQ(call("MsiDatabaseExportW", {I(db), I(w(u"T")), I(w(u"C:\\idt")), I(w(u"T.idt"))}), 0u);
    EXPECT_EQ(host_read("idt/T.idt"),
              std::string("K\tI\tL\tS\tB\r\ns10\tI2\ti4\tL0\tV0\r\n1252\tT\tK\tI\r\n"
                          "a\t1\t-7\ttab\x10here\x11\x19new\x19line\\back\xe9\t\r\n"
                          "b\t\t2\t\tb..ibd\r\n"));
    EXPECT_EQ(host_read("idt/T/b..ibd"), "BLOBDATA");
    EXPECT_EQ(call("MsiDatabaseExportW", {I(db), I(w(u"_ForceCodepage")), I(w(u"C:\\idt")), I(w(u"cp.idt"))}), 0u);
    EXPECT_EQ(host_read("idt/cp.idt"), "\r\n\r\n0\t_ForceCodepage\r\n");
    EXPECT_EQ(call("MsiDatabaseExportW", {I(db), I(w(u"Nope")), I(w(u"C:\\idt")), I(w(u"n.idt"))}), 1627u);
    EXPECT_EQ(last_error(), "2205|C:\\e.msi|Nope");
    EXPECT_EQ(call("MsiDatabaseExportW", {I(db), I(w(u"T")), I(w(u"idt")), I(w(u"x.idt"))}), 161u);
    EXPECT_EQ(call("MsiDatabaseExportW", {I(db), I(w(u"T")), I(w(u"C:\\idt")), I(0)}), 87u);
    // Import into a new database.
    const u32 db2 = open("C:\\i.msi", 3);
    host_file("idt/a.idt", "K\tV\nS10\tI2\nA\tK\nx\t5\ny\t\n");
    EXPECT_EQ(call("MsiDatabaseImportW", {I(db2), I(w(u"C:\\idt")), I(w(u"a.idt"))}), 0u);
    EXPECT_EQ(query(db2, "SELECT * FROM `A`"), (V{"x|5", "y|~"}));
    EXPECT_EQ(call("MsiDatabaseImportW", {I(db2), I(w(u"C:\\idt")), I(w(u"T.idt"))}), 0u);
    EXPECT_EQ(query(db2, "SELECT `K`, `I`, `L`, `B` FROM `T`"), (V{"a|1|-7|~", "b|~|2|<s8>"}));
    host_file("idt/g.idt", "K\tV\r\ns10\ti2\r\nG\tK\r\nx\t\r\n");
    EXPECT_EQ(call("MsiDatabaseImportW", {I(db2), I(w(u"C:\\idt")), I(w(u"g.idt"))}), 1627u);
    EXPECT_EQ(last_error(), "2216|C:\\i.msi|g.idt|4");
    EXPECT_EQ(call("MsiDatabaseIsTablePersistentW", {I(db2), I(w(u"G"))}), 2u);
    host_file("idt/o.idt", "K\tV\r\ns10\ti2\r\nO\tV\r\nx\t1\r\n");
    EXPECT_EQ(call("MsiDatabaseImportW", {I(db2), I(w(u"C:\\idt")), I(w(u"o.idt"))}), 1627u);
    EXPECT_EQ(last_error(), "2216|C:\\i.msi|o.idt|3");
    EXPECT_EQ(call("MsiDatabaseImportW", {I(db2), I(w(u"C:\\idt")), I(w(u"nope.idt"))}), 1627u);
    EXPECT_EQ(last_error(), "2215|C:\\i.msi|nope.idt|~");
    host_file("idt/k.idt", "K\tV\r\ns10\ti2\r\nA\tK\r\nz\t9\r\n");
    EXPECT_EQ(call("MsiDatabaseImportW", {I(db2), I(w(u"C:\\idt")), I(w(u"k.idt"))}), 0u);
    EXPECT_EQ(query(db2, "SELECT * FROM `A`"), V{"z|9"});  // an import replaces the table
    EXPECT_EQ(call("MsiDatabaseImportW", {I(db2), I(w(u"idt")), I(w(u"a.idt"))}), 161u);
}

TEST_P(Msi_database_Test, MergeReportsConflicts) {
    const u32 a = open("C:\\ma.msi", 3), b = open("C:\\mb.msi", 3);
    ASSERT_EQ(exec(a, "CREATE TABLE `P` (`K` CHAR(10) NOT NULL, `V` CHAR(10) PRIMARY KEY `K`)"), 0u);
    ASSERT_EQ(exec(a, "INSERT INTO `P` (`K`,`V`) VALUES ('same','1')"), 0u);
    ASSERT_EQ(exec(a, "INSERT INTO `P` (`K`,`V`) VALUES ('diff','a')"), 0u);
    ASSERT_EQ(exec(b, "CREATE TABLE `P` (`K` CHAR(20) NOT NULL, `V` CHAR(10) PRIMARY KEY `K`)"), 0u);
    ASSERT_EQ(exec(b, "INSERT INTO `P` (`K`,`V`) VALUES ('same','1')"), 0u);
    ASSERT_EQ(exec(b, "INSERT INTO `P` (`K`,`V`) VALUES ('diff','b')"), 0u);
    ASSERT_EQ(exec(b, "INSERT INTO `P` (`K`,`V`) VALUES ('new','n')"), 0u);
    ASSERT_EQ(exec(b, "CREATE TABLE `Q` (`K` SHORT NOT NULL PRIMARY KEY `K`)"), 0u);
    ASSERT_EQ(exec(b, "INSERT INTO `Q` (`K`) VALUES (5)"), 0u);
    EXPECT_EQ(call("MsiDatabaseMergeW", {I(a), I(b), I(w(u"MergeErr"))}), 1627u);
    EXPECT_EQ(last_error(), "2268|C:\\ma.msi|1");
    EXPECT_EQ(query(a, "SELECT * FROM `P`"), (V{"same|1", "diff|a", "new|n"}));
    EXPECT_EQ(query(a, "SELECT * FROM `Q`"), V{"5"});
    EXPECT_EQ(query(a, "SELECT * FROM `MergeErr`"), V{"P|1"});
    write_int(OUT, 0);
    ASSERT_EQ(call("MsiDatabaseOpenViewW", {I(a), I(w(u"SELECT * FROM `P`")), I(OUT)}), 0u);
    ASSERT_EQ(call("MsiViewGetColumnInfo", {I(static_cast<u32>(read_int(OUT))), I(1), I(OUT2)}), 0u);
    EXPECT_EQ(rec_text(static_cast<u32>(read_int(OUT2))), "s20|S10");
    ASSERT_EQ(exec(b, "CREATE TABLE `R` (`K` SHORT NOT NULL PRIMARY KEY `K`)"), 0u);
    ASSERT_EQ(exec(a, "CREATE TABLE `R` (`K` CHAR(4) NOT NULL PRIMARY KEY `K`)"), 0u);
    EXPECT_EQ(call("MsiDatabaseMergeW", {I(a), I(b), I(0)}), 1629u);
    EXPECT_EQ(last_error(), "2248|C:\\ma.msi|R|1");
    EXPECT_EQ(call("MsiDatabaseMergeW", {I(a), I(0), I(0)}), 6u);
}

// ---- format limits (msi_database_bigpool_probe, widemask probes) ----------------------------

TEST_P(Msi_database_Test, LargePoolsLongStringsAndBigFilesRoundTrip) {
    msi::Package pkg = msi::Package::create();
    using msi::make_column;
    ASSERT_EQ(pkg.create_table("S", {make_column("K", 's', 16, false, true, false, false)}, false), 0u);
    ASSERT_EQ(pkg.create_table("Bin", {make_column("Name", 's', 16, false, true, false, false),
                                       make_column("Data", 'v', 0, true, false, false, false),
                                       make_column("N", 'i', 2, true, false, false, false)}, false), 0u);
    ASSERT_EQ(pkg.create_table("L", {make_column("K", 's', 4, false, true, false, false),
                                     make_column("V", 's', 0, true, false, false, false)}, false), 0u);
    msi::Table* s = pkg.find_table("S");
    for (int i = 0; i < 70000; ++i) {
        char k[16];
        std::snprintf(k, sizeof k, "k%05d", i);
        ASSERT_EQ(pkg.insert_row(*s, {k}, false), 0u);
    }
    std::map<usize, std::vector<u8>> streams{{1, std::vector<u8>{'B', 'I', 'N'}}};
    ASSERT_EQ(pkg.insert_row(*pkg.find_table("Bin"), {"zz_bin", "", "7"}, false, &streams), 0u);
    std::string big(70000, 'x');
    for (usize i = 0; i < big.size(); ++i) big[i] = static_cast<char>('a' + i % 26);
    ASSERT_EQ(pkg.insert_row(*pkg.find_table("L"), {"long", big}, false), 0u);
    // A 10 MB stream needs more FAT sectors than the header lists (DIFAT).
    pkg.set_stream("Huge", std::vector<u8>(10u << 20, 0x5A));
    const std::vector<u8> image = pkg.serialize();
    auto back = msi::Package::open(image);
    ASSERT_TRUE(back.has_value());
    const msi::Table* s2 = back->table("S");
    ASSERT_NE(s2, nullptr);
    ASSERT_EQ(s2->rows.size(), 70000u);
    EXPECT_EQ(s2->rows[69999][0], "k69999");
    const msi::Table* b2 = back->table("Bin");
    ASSERT_NE(b2, nullptr);
    EXPECT_EQ(b2->rows[0][0], "zz_bin");
    EXPECT_EQ(b2->rows[0][2], "7");
    ASSERT_NE(back->stream(b2->rows[0][1]), nullptr);
    EXPECT_EQ(*back->stream(b2->rows[0][1]), (std::vector<u8>{'B', 'I', 'N'}));
    const msi::Table* l2 = back->table("L");
    ASSERT_NE(l2, nullptr);
    EXPECT_EQ(l2->rows[0][0], "long");  // the string after a long one keeps its index
    EXPECT_EQ(l2->rows[0][1], big);
    ASSERT_NE(back->stream("Huge"), nullptr);
    EXPECT_EQ(back->stream("Huge")->size(), 10u << 20);
    if (const char* out = std::getenv("VITREO_MSI_WRITE_SAMPLE")) {
        std::ofstream(std::string(out) + (x64() ? "big64.msi" : "big32.msi"), std::ios::binary)
            .write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    }
}

TEST_P(Msi_database_Test, WideTableTransformsAndRowErrors) {
    auto make = [&](const std::string& path, bool newer) {
        const u32 db = open(path, 3);
        std::string sql = "CREATE TABLE `W` (`K` SHORT NOT NULL";
        for (int c = 1; c < 20; ++c) sql += ", `C" + std::to_string(c) + "` SHORT";
        EXPECT_EQ(exec(db, sql + " PRIMARY KEY `K`)"), 0u);
        for (int k = 1; k <= 3; ++k)
            EXPECT_EQ(exec(db, "INSERT INTO `W` (`K`,`C1`,`C19`) VALUES (" + std::to_string(k) + "," + std::to_string(k) + "," + std::to_string(k) + ")"), 0u);
        if (newer) {
            EXPECT_EQ(exec(db, "UPDATE `W` SET `C19` = 9, `C17` = 7 WHERE `K` = 1"), 0u);
            EXPECT_EQ(exec(db, "DELETE FROM `W` WHERE `K` = 2"), 0u);
            EXPECT_EQ(exec(db, "INSERT INTO `W` (`K`,`C18`) VALUES (4,8)"), 0u);
        }
        EXPECT_EQ(call("MsiDatabaseCommit", {I(db)}), 0u);
        return db;
    };
    const u32 base = make("C:\\b20.msi", false);
    const u32 newer = make("C:\\n20.msi", true);
    ASSERT_EQ(call("MsiDatabaseGenerateTransformW", {I(newer), I(base), I(w(u"C:\\wide.mst")), I(0), I(0)}), 0u);
    // The table stream: 16-bit 7, then 32-bit masks (as Windows writes it).
    {
        const std::string image = host_read("wide.mst");
        auto root = parse_compound_file(std::span<const u8>(reinterpret_cast<const u8*>(image.data()), image.size()));
        ASSERT_TRUE(root);
        const std::u16string enc = msi::Package::encode_stream_name("W", true);
        std::string key;
        for (const char16_t c : enc) { key.push_back(static_cast<char>(0xE0 | (c >> 12))); key.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F))); key.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
        ASSERT_TRUE(root->children.count(key));
        const std::vector<u8>& d = root->children[key]->data;
        ASSERT_GE(d.size(), 12u);
        EXPECT_EQ(std::vector<u8>(d.begin(), d.begin() + 12),
                  (std::vector<u8>{0x07, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x01, 0x80, 0x07, 0x80, 0x09, 0x80}));
    }
    struct Case { const char* name; const char* prep; u32 cond; const char* rows; };
    for (const Case& c : std::initializer_list<Case>{
             {"clean", nullptr, 0, "1|1|7|~|9;3|3|~|~|3;4|~|~|8|~"},
             {"addexisting", "INSERT INTO `W` (`K`) VALUES (4)", 0, nullptr},
             {"addexisting-ok", "INSERT INTO `W` (`K`) VALUES (4)", 1, "1|1|7|~|9;3|3|~|~|3;4|~|~|~|~"},
             {"delmissing-ok", "DELETE FROM `W` WHERE `K` = 2", 2, "1|1|7|~|9;3|3|~|~|3;4|~|~|8|~"},
             {"updmissing", "DELETE FROM `W` WHERE `K` = 1", 0, nullptr},
             {"updmissing-ok", "DELETE FROM `W` WHERE `K` = 1", 0x10, "3|3|~|~|3;4|~|~|8|~"}}) {
        std::filesystem::copy_file(root / "b20.msi", root / "t20.msi", std::filesystem::copy_options::overwrite_existing);
        const u32 t = open("C:\\t20.msi", 1);
        if (c.prep) EXPECT_EQ(exec(t, c.prep), 0u);
        EXPECT_EQ(call("MsiDatabaseApplyTransformW", {I(t), I(w(u"C:\\wide.mst")), I(c.cond)}), 0u) << c.name;
        const V rows = query(t, "SELECT `K`,`C1`,`C17`,`C18`,`C19` FROM `W`");
        if (c.rows == nullptr) {
            // An unsuppressed row error leaves the table unloadable.
            EXPECT_EQ(rows, V{"open=1615"}) << c.name;
            EXPECT_EQ(last_error().substr(0, 4), "2229") << c.name;
        } else {
            std::string joined;
            for (const std::string& r : rows) joined += (joined.empty() ? "" : ";") + r;
            EXPECT_EQ(joined, c.rows) << c.name;
        }
        call("MsiCloseHandle", {I(t)});
    }
}

// ---- the string-row API the engine reads through ------------------------------------------

TEST_P(Msi_database_Test, PackageRowsStayCoherentWithSql) {
    const u32 db = sql_db();
    auto obj = msi::database_of(mgr, db);
    ASSERT_TRUE(obj);
    msi::Package& pkg = *obj->package;
    const msi::Table* t = pkg.table("U");
    ASSERT_NE(t, nullptr);
    const usize before = t->rows.size();
    EXPECT_EQ(exec(db, "INSERT INTO `U` (`A`,`B`,`C`) VALUES (50,'z',1) TEMPORARY"), 0u);
    EXPECT_EQ(t->rows.size(), before + 1);  // the same Table object
    EXPECT_EQ(exec(db, "UPDATE `U` SET `C` = 77 WHERE `A` = 50"), 0u);
    const auto row = pkg.find_row(*t, std::vector<std::string>{"50", "z"});
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(t->rows[*row][2], "77");
    EXPECT_EQ(exec(db, "ALTER TABLE `U` ADD `Tmp` CHAR(9) TEMPORARY HOLD"), 0u);
    EXPECT_EQ(t->columns.back().name, "Tmp");
    EXPECT_EQ(t->rows[*row].size(), 4u);
    // A record's integer fields come back as integers.
    EXPECT_EQ(query(db, "SELECT `A`, `Tmp` FROM `U` WHERE `A` = 50"), V{"50|~"});
}

TEST_P(Msi_database_Test, ActiveDatabaseIsTheSessionsPackage) {
    msi::Package pkg = msi::Package::create();
    using msi::make_column;
    ASSERT_EQ(pkg.create_table("Property", {make_column("Property", 's', 72, false, true, false, false),
                                            make_column("Value", 's', 0, false, false, true, false)}, false), 0u);
    ASSERT_EQ(pkg.insert_row(*pkg.find_table("Property"), {"ProductCode", "{11111111-2222-3333-4444-555555555555}"}, false), 0u);
    ASSERT_EQ(pkg.create_table("ListBox", {make_column("Property", 's', 72, false, true, false, false),
                                           make_column("Order", 'i', 2, false, true, false, false),
                                           make_column("Value", 's', 64, false, false, false, false),
                                           make_column("Text", 's', 64, true, false, true, false)}, false), 0u);
    const std::vector<u8> image = pkg.serialize();
    std::ofstream(root / "pkg.msi", std::ios::binary).write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    write_int(OUT, 0);
    ASSERT_EQ(call("MsiOpenPackageW", {I(w(u"C:\\pkg.msi")), I(OUT)}), 0u);
    const u32 install = static_cast<u32>(read_int(OUT));
    const u32 db = static_cast<u32>(call("MsiGetActiveDatabase", {I(install)}));
    ASSERT_NE(db, 0u);
    EXPECT_EQ(call("MsiGetDatabaseState", {I(db)}), 0u);
    // A custom action may add temporary rows only.
    EXPECT_EQ(exec(db, "INSERT INTO `ListBox` (`Property`,`Order`,`Value`,`Text`) VALUES ('P',1,'v','t') TEMPORARY"), 0u);
    EXPECT_EQ(exec(db, "INSERT INTO `ListBox` (`Property`,`Order`,`Value`,`Text`) VALUES ('P',2,'v','t')"), 1627u);
    EXPECT_EQ(exec(db, "UPDATE `ListBox` SET `Text` = 'u' WHERE `Order` = 1"), 0u);
    write_int(OUT, 0);
    ASSERT_EQ(call("MsiDatabaseOpenViewW", {I(db), I(w(u"SELECT `Property`,`Order`,`Value`,`Text` FROM `ListBox`")), I(OUT)}), 0u);
    const u32 v = static_cast<u32>(read_int(OUT));
    ASSERT_EQ(call("MsiViewExecute", {I(v), I(0)}), 0u);
    const u32 r = record({std::string("P"), "3", std::string("w"), std::string("x")});
    EXPECT_EQ(call("MsiViewModify", {I(v), I(7), I(r)}), 0u);  // MSIMODIFY_INSERT_TEMPORARY
    EXPECT_EQ(call("MsiViewModify", {I(v), I(1), I(r)}), 1627u);
    // The engine sees the rows through the same Package.
    auto obj = msi::database_of(mgr, db);
    ASSERT_TRUE(obj);
    const msi::Table* t = obj->package->table("ListBox");
    ASSERT_NE(t, nullptr);
    ASSERT_EQ(t->rows.size(), 2u);
    EXPECT_EQ(t->rows[0][3], "u");
    EXPECT_EQ(call("MsiGetActiveDatabase", {I(0)}), 0u);
    EXPECT_EQ(call("MsiGetActiveDatabase", {I(db)}), 0u);  // not an install handle
}

TEST_P(Msi_database_Test, StreamsTableThroughViewModifyAndSql) {
    const u32 db = open("C:\\st.msi", 3);
    host_file("a.bin", "AAAA");
    host_file("b.bin", "BB");
    write_int(OUT, 0);
    ASSERT_EQ(call("MsiDatabaseOpenViewW", {I(db), I(w(u"SELECT `Name`,`Data` FROM `_Streams`")), I(OUT)}), 0u);
    const u32 v = static_cast<u32>(read_int(OUT));
    ASSERT_EQ(call("MsiViewExecute", {I(v), I(0)}), 0u);
    const u32 r = record({std::string("MyStream"), std::nullopt});
    ASSERT_EQ(call("MsiRecordSetStreamW", {I(r), I(2), I(w(u"C:\\a.bin"))}), 0u);
    EXPECT_EQ(call("MsiViewModify", {I(v), I(1), I(r)}), 0u);
    EXPECT_EQ(call("MsiViewModify", {I(v), I(1), I(r)}), 1627u);
    ASSERT_EQ(call("MsiRecordSetStreamW", {I(r), I(2), I(w(u"C:\\b.bin"))}), 0u);
    EXPECT_EQ(call("MsiViewModify", {I(v), I(3), I(r)}), 0u);
    EXPECT_EQ(query(db, "SELECT `Name`,`Data` FROM `_Streams`"), V{"MyStream|<s2>"});
    write_int(OUT, 0);
    ASSERT_EQ(call("MsiDatabaseOpenViewW", {I(db), I(w(u"SELECT `Name`,`Data` FROM `_Streams` WHERE `Name` = 'MyStream'")), I(OUT)}), 0u);
    const u32 v2 = static_cast<u32>(read_int(OUT));
    ASSERT_EQ(call("MsiViewExecute", {I(v2), I(0)}), 0u);
    ASSERT_EQ(call("MsiViewFetch", {I(v2), I(OUT2)}), 0u);
    const u32 f = static_cast<u32>(read_int(OUT2));
    ASSERT_EQ(call("MsiRecordSetStreamW", {I(f), I(2), I(w(u"C:\\a.bin"))}), 0u);
    EXPECT_EQ(call("MsiViewModify", {I(v2), I(2), I(f)}), 0u);
    EXPECT_EQ(query(db, "SELECT `Name`,`Data` FROM `_Streams`"), V{"MyStream|<s4>"});
    EXPECT_EQ(call("MsiViewModify", {I(v2), I(6), I(f)}), 0u);
    EXPECT_EQ(query(db, "SELECT `Name` FROM `_Streams`"), V{});
    const u32 p = record({std::nullopt});
    ASSERT_EQ(call("MsiRecordSetStreamW", {I(p), I(1), I(w(u"C:\\a.bin"))}), 0u);
    EXPECT_EQ(exec(db, "INSERT INTO `_Streams` (`Name`,`Data`) VALUES ('Sql',?)", p), 0u);
    EXPECT_EQ(query(db, "SELECT `Name` FROM `_Streams`"), V{"Sql"});
    EXPECT_EQ(exec(db, "DELETE FROM `_Streams` WHERE `Name` = 'Sql'"), 0u);
    EXPECT_EQ(query(db, "SELECT `Name` FROM `_Streams`"), V{});
}

INSTANTIATE_MSI_BITNESS(Msi_database_Test);

}  // namespace
}  // namespace vitreo::winapi::msi_test
