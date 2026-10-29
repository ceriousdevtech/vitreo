/// @file test_normalization.cpp
/// @brief NormalizeString, IsNormalizedString and IdnTo* against Windows.
///
/// tests/oracle_data/normaliz_{probe,limits_probe,edges_probe}{,.x86}.expected
/// are winoracle runs of probes that call the five functions through
/// Normaliz.dll. Each line records the call (flags or form, the counted input
/// units, the output capacity, whether a buffer was passed) and Windows'
/// result: return value, GetLastError (0x12345678 = left unchanged) and the
/// destination buffer, pre-filled with 0xABCD. Every line is replayed here.
///
/// Two measured behaviours are deliberately not reproduced; the test checks
/// the documented contract for them instead:
///   - NORM_FORM 13 (undocumented Windows IDNA mapping) fails with the
///     documented ERROR_INVALID_PARAMETER.
///   - NormalizeString's ERROR_INSUFFICIENT_BUFFER return is the negative of
///     an internal Windows re-estimate. Vitreo returns the negative of the
///     exact size needed; Windows' value was never smaller.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/locale/normalization.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr u32 kUnchanged = 0x12345678u;
constexpr u16 kFill = 0xABCD;

struct OracleCase {
    std::string line;
    std::string api;
    i64 arg = 0;
    nls::StringArgs args;
    i32 ret = 0;
    u32 err = 0;
    std::vector<u16> out;
};

std::vector<u16> parse_units(const std::string& hex) {
    std::vector<u16> units;
    std::istringstream in(hex);
    for (std::string word; in >> word;) units.push_back(static_cast<u16>(std::stoul(word, nullptr, 16)));
    return units;
}

std::string field(const std::vector<std::string>& parts, const std::string& key) {
    for (const auto& p : parts) {
        if (p.rfind(key + "=", 0) == 0) return p.substr(key.size() + 1);
    }
    ADD_FAILURE() << "missing " << key;
    return {};
}

std::vector<OracleCase> load(const char* file) {
    std::ifstream in(std::string(VITREO_ORACLE_PROBE_DIR) + "/" + file);
    EXPECT_TRUE(in.good()) << file;
    std::vector<OracleCase> cases;
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("bits=", 0) == 0 || line.empty()) continue;
        std::vector<std::string> parts;
        std::stringstream ss(line);
        for (std::string p; std::getline(ss, p, '|');) parts.push_back(p);
        OracleCase c;
        c.line = line;
        c.api = parts.at(0);
        c.arg = std::stoll(field(parts, "arg"));
        const std::string input = field(parts, "in");
        const usize colon = input.find(':');
        if (input.rfind("null", 0) == 0) {
            c.args.src_null = true;
            c.args.src_len = std::stoi(input.substr(colon + 1));
        } else {
            c.args.src_len = std::stoi(input.substr(0, colon));
            for (u16 u : parse_units(input.substr(colon + 1))) c.args.src.push_back(static_cast<char16_t>(u));
        }
        c.args.dst_len = std::stoi(field(parts, "cap"));
        c.args.dst_null = field(parts, "buf") == "0";
        c.ret = std::stoi(field(parts, "ret"));
        c.err = static_cast<u32>(std::stoul(field(parts, "err")));
        c.out = parse_units(field(parts, "out"));
        cases.push_back(std::move(c));
    }
    EXPECT_FALSE(cases.empty()) << file;
    return cases;
}

nls::StringResult run(const OracleCase& c) {
    if (c.api == "NormalizeString") return nls::normalize_string(static_cast<i32>(c.arg), c.args);
    if (c.api == "IsNormalizedString") return nls::is_normalized_string(static_cast<i32>(c.arg), c.args);
    const u32 flags = static_cast<u32>(c.arg);
    if (c.api == "IdnToAscii") return nls::idn_convert(nls::IdnApi::ToAscii, flags, c.args);
    if (c.api == "IdnToUnicode") return nls::idn_convert(nls::IdnApi::ToUnicode, flags, c.args);
    if (c.api == "IdnToNameprepUnicode") {
        return nls::idn_convert(nls::IdnApi::ToNameprepUnicode, flags, c.args);
    }
    ADD_FAILURE() << "unknown api " << c.api;
    return {};
}

/// The units Windows wrote: the shown buffer up to the first untouched fill.
/// Shown units are the full result for a success and the first few units
/// (or the whole small buffer) otherwise.
void expect_written(const OracleCase& c, const nls::StringResult& got) {
    if (c.args.dst_null || c.args.dst_len == 0) {
        EXPECT_TRUE(got.written.empty()) << c.line;
        return;
    }
    for (usize i = 0; i < c.out.size(); ++i) {
        if (c.out[i] == kFill) {
            EXPECT_LE(got.written.size(), i) << "wrote past Windows at unit " << i << ": " << c.line;
            return;
        }
        ASSERT_LT(i, got.written.size()) << "Windows wrote unit " << i << ": " << c.line;
        EXPECT_EQ(static_cast<u16>(got.written[i]), c.out[i]) << "unit " << i << ": " << c.line;
    }
}

void replay(const char* file) {
    usize replayed = 0;
    for (const OracleCase& c : load(file)) {
        const nls::StringResult got = run(c);
        const bool normalization = c.api == "NormalizeString" || c.api == "IsNormalizedString";
        if (normalization && c.arg == 13) {
            EXPECT_EQ(got.ret, 0) << c.line;
            EXPECT_EQ(got.last_error, std::optional<u32>(error_codes::ERROR_INVALID_PARAMETER)) << c.line;
            EXPECT_TRUE(got.written.empty()) << c.line;
            continue;
        }
        ++replayed;
        const std::optional<u32> expected_error =
            c.err == kUnchanged ? std::nullopt : std::optional<u32>(c.err);
        EXPECT_EQ(got.last_error, expected_error) << c.line;
        if (c.api == "NormalizeString" && c.err == error_codes::ERROR_INSUFFICIENT_BUFFER) {
            ASSERT_LT(got.ret, 0) << c.line;
            EXPECT_GE(-c.ret, -got.ret) << "Windows' re-estimate was smaller: " << c.line;
            EXPECT_EQ(got.written.size(), static_cast<usize>(c.args.dst_len)) << c.line;
        } else {
            EXPECT_EQ(got.ret, c.ret) << c.line;
        }
        expect_written(c, got);
    }
    EXPECT_GT(replayed, 100u) << file;
}

} // namespace

TEST(NormalizationOracle, CoreCasesX64) { replay("normaliz_probe.expected"); }
TEST(NormalizationOracle, CoreCasesX86) { replay("normaliz_probe.x86.expected"); }
TEST(NormalizationOracle, LimitsX64) { replay("normaliz_limits_probe.expected"); }
TEST(NormalizationOracle, LimitsX86) { replay("normaliz_limits_probe.x86.expected"); }
TEST(NormalizationOracle, EdgesX64) { replay("normaliz_edges_probe.expected"); }
TEST(NormalizationOracle, EdgesX86) { replay("normaliz_edges_probe.x86.expected"); }

// ---------------------------------------------------------------------------
// The guest-facing layer: argument order, -1 reading, output and last error.
// ---------------------------------------------------------------------------

class NormalizationThunkTest : public ::testing::TestWithParam<bool> {
protected:
    static constexpr GuestAddr MEM_BASE = 0x1000;
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr SRC = 0x2000;
    static constexpr GuestAddr DST = 0x3000;

    void SetUp() override {
        (void)memory_.map_region(MEM_BASE, 0x10000, MemoryProtection::ReadWrite);
        (void)memory_.map_region(STACK_BASE, 0x10000, MemoryProtection::ReadWrite);
        cpu_.gpr.rsp = STACK_BASE + 0x800;
        cpu_.mode = GetParam() ? cpu::CpuMode::Long : cpu::CpuMode::Protected;
    }

    void args(std::initializer_list<u64> values) {
        usize i = 0;
        for (u64 v : values) {
            if (!GetParam()) {
                (void)memory_.write_u32(static_cast<GuestAddr>(cpu_.gpr.rsp) + 4 + i * 4,
                                        static_cast<u32>(v));
            } else if (i == 0) cpu_.gpr.rcx = v;
            else if (i == 1) cpu_.gpr.rdx = v;
            else if (i == 2) cpu_.gpr.r8 = v;
            else if (i == 3) cpu_.gpr.r9 = v;
            else (void)memory_.write_u64(static_cast<GuestAddr>(cpu_.gpr.rsp) + 0x28 + (i - 4) * 8, v);
            ++i;
        }
    }

    void put(GuestAddr at, std::u16string_view s) {
        for (usize i = 0; i < s.size(); ++i) (void)memory_.write_u16(at + i * 2, s[i]);
    }

    std::u16string get(GuestAddr at, usize n) {
        std::u16string s;
        for (usize i = 0; i < n; ++i) s.push_back(static_cast<char16_t>(memory_.read_u16(at + i * 2).value_or(0)));
        return s;
    }

    ExportEntry entry(const std::string& name) {
        for (auto& e : nls::create_normalization_thunks().entries) {
            if (e.name == name) return e;
        }
        ADD_FAILURE() << name;
        return {};
    }

    i32 call(const std::string& name) {
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        entry(name).handler(m);
        return static_cast<i32>(static_cast<u32>(cpu_.gpr.rax));
    }

    GuestMemory memory_;
    cpu::CpuState cpu_;
};

TEST_P(NormalizationThunkTest, IdnToAsciiReadsTerminatedInputAndWritesTerminatedOutput) {
    put(SRC, u"bücher.de");
    put(SRC + 9 * 2, std::u16string_view(u"\0", 1));
    set_last_error(0x12345678u);
    args({0, SRC, static_cast<u32>(-1), DST, 64});
    EXPECT_EQ(call("IdnToAscii"), 17);
    EXPECT_EQ(get(DST, 17), std::u16string(u"xn--bcher-kva.de") + u'\0');
    EXPECT_EQ(get_last_error(), 0x12345678u) << "success leaves GetLastError";
}

TEST_P(NormalizationThunkTest, IdnToUnicodeShortBufferFailsWithoutWriting) {
    put(SRC, u"xn--bcher-kva.de");
    put(DST, u"\xabcd\xabcd\xabcd\xabcd");
    args({0, SRC, 16, DST, 4});
    EXPECT_EQ(call("IdnToUnicode"), 0);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(get(DST, 4), u"\xabcd\xabcd\xabcd\xabcd");
}

TEST_P(NormalizationThunkTest, NormalizeStringNegativeReturnAndPartialPrefix) {
    put(SRC, u"ab\xd800" u"cd");
    put(SRC + 5 * 2, std::u16string_view(u"\0", 1));
    args({1, SRC, static_cast<u32>(-1), DST, 64});
    EXPECT_EQ(call("NormalizeString"), -3);
    EXPECT_EQ(get_last_error(), 1113u);
    EXPECT_EQ(get(DST, 2), u"ab");
}

TEST_P(NormalizationThunkTest, NormalizeStringQueryAndConversion) {
    put(SRC, u"é");
    args({1, SRC, 2, 0, 0});
    set_last_error(0x12345678u);
    EXPECT_EQ(call("NormalizeString"), 6);
    EXPECT_EQ(get_last_error(), 0u) << "NormalizeString sets ERROR_SUCCESS";
    args({1, SRC, 2, DST, 6});
    EXPECT_EQ(call("NormalizeString"), 1);
    EXPECT_EQ(get(DST, 1), u"é");
}

TEST_P(NormalizationThunkTest, IsNormalizedStringThreeArguments) {
    put(SRC, u"é");
    set_last_error(0x12345678u);
    args({2, SRC, 2});
    EXPECT_EQ(call("IsNormalizedString"), 1);
    args({1, SRC, 2});
    EXPECT_EQ(call("IsNormalizedString"), 0);
    EXPECT_EQ(get_last_error(), 0x12345678u) << "a plain FALSE leaves GetLastError";
}

INSTANTIATE_TEST_SUITE_P(Bitness, NormalizationThunkTest, ::testing::Values(true, false),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

TEST(NormalizationExports, StdcallBytesAndNormalizOrdinals) {
    // Normaliz.dll export table, Windows 11 x86 and x64: ordinal base 1.
    const std::pair<const char*, u16> table[] = {
        {"IdnToAscii", 1}, {"IdnToNameprepUnicode", 2}, {"IdnToUnicode", 3},
        {"IsNormalizedString", 4}, {"NormalizeString", 5},
    };
    const auto exports = nls::create_normalization_thunks();
    ASSERT_EQ(exports.entries.size(), 5u);
    for (const auto& [name, ordinal] : table) {
        EXPECT_EQ(nls::normaliz_ordinal(name), ordinal) << name;
        bool found = false;
        for (const auto& e : exports.entries) {
            if (e.name != name) continue;
            found = true;
            // winnls.h: five-argument APIs pop 20 bytes, IsNormalizedString 12.
            EXPECT_EQ(e.stdcall_bytes, std::string(name) == "IsNormalizedString" ? 12 : 20) << name;
        }
        EXPECT_TRUE(found) << name;
    }
    EXPECT_EQ(nls::normaliz_ordinal("LCMapStringEx"), 0);
}
