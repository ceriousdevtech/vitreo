/// @file test_glu32_export_audit.cpp
/// @brief Audits the generated glu32 export table against the Windows
/// glu32.dll export directory (x86 and x64) and the mingw x86 import
/// library's stdcall stack sizes, and checks that every export is registered
/// with its ordinal and stack bytes.
///
/// Both .inc files are produced by tools/gen_glu32_exports.py from the real
/// DLLs (winoracle System32 / SysWOW64 glu32.dll), mingw GL/glu.h and
/// `nm libglu32.a`; nothing here reads the DLLs themselves.

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>

#include "winapi/core/thunk_registry.hpp"
#include "winapi/graphics/glu32/glu32.hpp"

namespace {

struct Row {
    uint32_t ordinal;
    uint32_t stack_bytes;
    char ret;
    std::string kinds;
};
struct OracleRow {
    uint32_t ordinal32;
    uint32_t ordinal64;
    uint32_t stack_bytes;
};

std::map<std::string, Row> rows() {
    std::map<std::string, Row> m;
#define GLU32_EXPORT(name, ordinal, stack, ret, kinds) m.emplace(#name, Row{ordinal, stack, #ret[0], kinds});
#include "winapi/graphics/glu32/glu32_exports.inc"
#undef GLU32_EXPORT
    return m;
}

std::map<std::string, OracleRow> oracle_rows() {
    std::map<std::string, OracleRow> m;
#define GLU32_ORACLE_EXPORT(name, ord32, ord64, stack) m.emplace(#name, OracleRow{ord32, ord64, stack});
#include "glu32_oracle_exports.inc"
#undef GLU32_ORACLE_EXPORT
    return m;
}

uint32_t stack_of_kinds(const std::string& kinds) {
    uint32_t n = 0;
    for (char k : kinds) n += (k == 'd') ? 8 : 4;
    return n;
}

}  // namespace

TEST(Glu32ExportAudit, GeneratedSetEqualsWindowsExports) {
    const auto gen = rows();
    const auto oracle = oracle_rows();
    ASSERT_EQ(oracle.size(), 52u);
    std::set<std::string> a, b;
    for (const auto& [n, r] : gen) a.insert(n);
    for (const auto& [n, r] : oracle) b.insert(n);
    EXPECT_EQ(a, b);
    for (const auto& [n, r] : gen) {
        auto it = oracle.find(n);
        if (it == oracle.end()) continue;
        EXPECT_EQ(r.ordinal, it->second.ordinal32) << n;
        EXPECT_EQ(it->second.ordinal32, it->second.ordinal64) << n;   // same ordinals on x64 (fixture)
    }
}

TEST(Glu32ExportAudit, X86StackBytesMatchImportLibrary) {
    const auto gen = rows();
    const auto oracle = oracle_rows();
    for (const auto& [n, r] : gen) {
        auto it = oracle.find(n);
        ASSERT_NE(it, oracle.end()) << n;
        EXPECT_EQ(r.stack_bytes, it->second.stack_bytes) << n;
        EXPECT_EQ(r.stack_bytes, stack_of_kinds(r.kinds)) << n;
        for (char k : r.kinds) EXPECT_NE(std::string("ifdp").find(k), std::string::npos) << n;
        EXPECT_NE(std::string("vip").find(r.ret), std::string::npos) << n;
    }
}

TEST(Glu32ExportAudit, RegisteredWithOrdinalsStackBytesAndHandlers) {
    vitreo::winapi::Glu32Manager mgr;
    const auto exports = vitreo::winapi::create_glu32_thunks(mgr);
    const auto gen = rows();
    ASSERT_EQ(exports.entries.size(), gen.size());
    for (const auto& e : exports.entries) {
        auto it = gen.find(e.name);
        ASSERT_NE(it, gen.end()) << e.name;
        EXPECT_EQ(e.ordinal, it->second.ordinal) << e.name;
        EXPECT_EQ(e.stdcall_bytes, it->second.stack_bytes) << e.name;
        EXPECT_TRUE(static_cast<bool>(e.handler)) << e.name;
    }
}
