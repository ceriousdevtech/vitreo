// Original facts-only Windows VerLanguageName replay, both guest ABIs and ACPs.
#include <gtest/gtest.h>
#include "winapi/system/version/version.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <array>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <tuple>
using namespace vitreo;
using namespace vitreo::winapi;
namespace {
class VerLanguageContract : public ::testing::TestWithParam<std::tuple<bool, u32>> {
protected:
    VersionManager manager;
    DllExports exports;
    GuestMemory memory;
    u32 old_acp{};
    bool is64() const { return std::get<0>(GetParam()); }
    u32 acp() const { return std::get<1>(GetParam()); }
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(0x10000, 0x5000, MemoryProtection::ReadWrite).is_ok());
        exports = create_version_thunks(manager);
        old_acp = codepage::ansi_code_page(); codepage::set_ansi_code_page(acp());
    }
    void TearDown() override { codepage::set_ansi_code_page(old_acp); }
    u32 call(bool wide, u32 language, u32 capacity, bool null_buffer = false) {
        const auto entry = std::find_if(exports.entries.begin(), exports.entries.end(), [&](const auto& e) {
            return e.name == (wide ? "VerLanguageNameW" : "VerLanguageNameA"); });
        EXPECT_NE(entry, exports.entries.end());
        if (entry == exports.entries.end()) return ~0u;
        cpu::CpuState state;
        const GuestAddr buffer = null_buffer ? 0 : 0x11000;
        if (is64()) {
            state.init_long_mode(); state.gpr.rcx = language; state.gpr.rdx = buffer; state.gpr.r8 = capacity;
            state.gpr.rsp = 0x14000;
        } else {
            state.init_protected_mode(); state.set_sp(0x14000);
            (void)memory.write_u32(0x14004, language);
            (void)memory.write_u32(0x14008, static_cast<u32>(buffer));
            (void)memory.write_u32(0x1400c, capacity);
        }
        Marshaler m(memory); m.attach_cpu(state); set_last_error(0xdead); entry->handler(m);
        EXPECT_EQ(get_last_error(), 0xdeadu);
        return static_cast<u32>(state.gpr.rax);
    }
    static std::vector<u8> hex(const std::string& text) {
        std::vector<u8> result;
        for (usize i = 0; i < text.size(); i += 2)
            result.push_back(static_cast<u8>(std::stoul(text.substr(i, 2), nullptr, 16)));
        return result;
    }
    std::vector<std::string> lines() {
        auto path = std::filesystem::path(__FILE__).parent_path() / "../../../tools/oracle_probe" /
            (std::string(acp() == 65001 ? "ver_language_utf8" : "ver_language") + (is64() ? "64.expected" : "32.expected"));
        std::ifstream file(path); EXPECT_TRUE(file.is_open()) << path;
        std::vector<std::string> result;
        for (std::string line; std::getline(file, line);) {
            if (line.ends_with('\r')) line.pop_back();
            result.push_back(line);
        }
        return result;
    }
};
TEST_P(VerLanguageContract, WindowsBufferAndErrorMatrix) {
    const std::regex sample(R"(sample ([AW]) lang=([0-9a-f]+) cap=(\d+) null=([01]) result=(\d+) exception=00000000 error=57005 bytes=([0-9a-f]+))");
    usize count = 0;
    for (const auto& line : lines()) {
        if (!line.starts_with("sample ")) continue;
        SCOPED_TRACE(line.substr(0, line.find(" bytes=")));
        std::smatch fields; ASSERT_TRUE(std::regex_match(line, fields, sample));
        std::array<u8, 512> fill; fill.fill(0xa5);
        ASSERT_TRUE(memory.write_bytes(0x11000, reinterpret_cast<const Byte*>(fill.data()), fill.size()).is_ok());
        EXPECT_EQ(call(fields[1] == "W", static_cast<u32>(std::stoul(fields[2], nullptr, 16)),
            static_cast<u32>(std::stoul(fields[3])), fields[4] == "1"), std::stoul(fields[5]));
        const auto wanted = hex(fields[6]); std::vector<u8> actual(wanted.size());
        ASSERT_TRUE(memory.read_bytes(0x11000, reinterpret_cast<Byte*>(actual.data()), actual.size()).is_ok());
        EXPECT_EQ(actual, wanted); ++count;
    }
    EXPECT_GT(count, 900u);
}
TEST_P(VerLanguageContract, CompleteWindowsLanguageNameInventory) {
    const std::regex name(R"(name ([0-9a-f]+) w=(\d+):([0-9a-f]*) a=(\d+):([0-9a-f]*))");
    usize count = 0;
    for (const auto& line : lines()) {
        if (!line.starts_with("name ")) continue;
        std::smatch fields; ASSERT_TRUE(std::regex_match(line, fields, name));
        SCOPED_TRACE(fields[1].str());
        const auto language = static_cast<u32>(std::stoul(fields[1], nullptr, 16));
        for (bool wide : {false, true}) {
            auto wanted = hex(fields[wide ? 3 : 5]);
            if (wide) for (usize i = 0; i < wanted.size(); i += 2) std::swap(wanted[i], wanted[i + 1]);
            wanted.push_back(0); if (wide) wanted.push_back(0);
            EXPECT_EQ(call(wide, language, 256), std::stoul(fields[wide ? 2 : 4]));
            std::vector<u8> actual(wanted.size());
            ASSERT_TRUE(memory.read_bytes(0x11000, reinterpret_cast<Byte*>(actual.data()), actual.size()).is_ok());
            EXPECT_EQ(actual, wanted);
        }
        ++count;
    }
    EXPECT_EQ(count, 937u);
}
INSTANTIATE_TEST_SUITE_P(BothAbisAndAcp, VerLanguageContract,
    ::testing::Combine(::testing::Bool(), ::testing::Values(1252u, 65001u)));
} // namespace
