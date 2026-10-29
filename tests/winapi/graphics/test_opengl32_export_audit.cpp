/// @file test_opengl32_export_audit.cpp
/// @brief Audits the generated opengl32 export table against the Windows
/// opengl32.dll export directory (x86 and x64) and the mingw x86 import
/// library's stdcall stack sizes.
///
/// Both .inc files are produced by tools/gen_opengl32_exports.py from the
/// real DLLs, mingw GL/gl.h and `nm libopengl32.a`; nothing here reads the
/// DLLs themselves.

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace {

struct GlRow {
    uint32_t ordinal;
    uint32_t stack_bytes;
    char ret;
    std::string kinds;
};
struct OtherRow {
    uint32_t ordinal;
    uint32_t stack_bytes;
};
struct OracleRow {
    uint32_t ordinal32;
    uint32_t ordinal64;
    uint32_t stack_bytes;  ///< 0xFFFF when the import library lacks the symbol.
};

constexpr uint32_t kUnknownStack = 0xFFFF;

std::map<std::string, GlRow> gl_rows() {
    std::map<std::string, GlRow> m;
#define GL11_EXPORT(name, ordinal, stack, ret, kinds) \
    m.emplace(#name, GlRow{ordinal, stack, #ret[0], kinds});
#define GL11_OTHER_EXPORT(name, ordinal, stack)
#include "winapi/graphics/opengl32/gl11_exports.inc"
#undef GL11_EXPORT
#undef GL11_OTHER_EXPORT
    return m;
}

std::map<std::string, OtherRow> other_rows() {
    std::map<std::string, OtherRow> m;
#define GL11_EXPORT(name, ordinal, stack, ret, kinds)
#define GL11_OTHER_EXPORT(name, ordinal, stack) \
    m.emplace(#name, OtherRow{ordinal, stack});
#include "winapi/graphics/opengl32/gl11_exports.inc"
#undef GL11_EXPORT
#undef GL11_OTHER_EXPORT
    return m;
}

std::map<std::string, OracleRow> oracle_rows() {
    std::map<std::string, OracleRow> m;
#define OPENGL32_ORACLE_EXPORT(name, ord32, ord64, stack) \
    m.emplace(#name, OracleRow{ord32, ord64, stack});
#include "opengl32_oracle_exports.inc"
#undef OPENGL32_ORACLE_EXPORT
    return m;
}

uint32_t stack_of_kinds(const std::string& kinds) {
    uint32_t n = 0;
    for (char k : kinds) n += (k == 'd') ? 8 : 4;
    return n;
}

}  // namespace

TEST(Opengl32ExportAudit, GeneratedSetEqualsWindowsExports) {
    const auto gl = gl_rows();
    const auto other = other_rows();
    const auto oracle = oracle_rows();
    ASSERT_FALSE(oracle.empty());

    std::set<std::string> generated;
    for (const auto& [name, row] : gl) {
        EXPECT_TRUE(generated.insert(name).second) << "duplicate " << name;
    }
    for (const auto& [name, row] : other) {
        EXPECT_TRUE(generated.insert(name).second) << "duplicate " << name;
    }
    std::set<std::string> windows;
    for (const auto& [name, row] : oracle) windows.insert(name);

    for (const auto& name : windows) {
        EXPECT_TRUE(generated.count(name)) << "missing from table: " << name;
    }
    for (const auto& name : generated) {
        EXPECT_TRUE(windows.count(name)) << "not exported by Windows: " << name;
    }
    EXPECT_EQ(generated.size(), windows.size());

    // The row ordinal is the x86 ordinal; the x64 ordinal lives in the fixture.
    auto check_ordinal = [&](const std::string& name, uint32_t ordinal) {
        auto it = oracle.find(name);
        if (it != oracle.end()) {
            EXPECT_EQ(ordinal, it->second.ordinal32) << name;
        }
    };
    for (const auto& [name, row] : gl) check_ordinal(name, row.ordinal);
    for (const auto& [name, row] : other) check_ordinal(name, row.ordinal);
}

TEST(Opengl32ExportAudit, X86StackBytesMatchImportLibrary) {
    const auto gl = gl_rows();
    const auto other = other_rows();
    const auto oracle = oracle_rows();

    for (const auto& [name, row] : gl) {
        auto it = oracle.find(name);
        ASSERT_NE(it, oracle.end()) << name;
        EXPECT_EQ(row.stack_bytes, it->second.stack_bytes) << name;
        EXPECT_EQ(row.stack_bytes, stack_of_kinds(row.kinds)) << name;
        EXPECT_NE(row.stack_bytes, kUnknownStack) << name;
        EXPECT_TRUE(row.ret == 'v' || row.ret == 'i' || row.ret == 'p') << name;
        for (char k : row.kinds) {
            EXPECT_NE(std::string("ifdp").find(k), std::string::npos) << name;
        }
    }
    for (const auto& [name, row] : other) {
        auto it = oracle.find(name);
        ASSERT_NE(it, oracle.end()) << name;
        if (it->second.stack_bytes != kUnknownStack) {
            EXPECT_EQ(row.stack_bytes, it->second.stack_bytes) << name;
        }
    }
}
