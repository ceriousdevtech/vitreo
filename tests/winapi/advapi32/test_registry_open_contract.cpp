// Original Windows oracle replay: tools/oracle_probe/reg_open_probe.c.
#include <gtest/gtest.h>
#include "winapi/advapi32/registry/registry.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "winapi/kernel32/error.hpp"
#include "../../../src/winapi/advapi32/registry/reg_open_sdk.generated.inc"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
using namespace vitreo;
using namespace vitreo::winapi;
namespace facts = reg_open_sdk;
namespace {
class RegistryOpenContract : public ::testing::TestWithParam<bool> {
protected:
    capsule::Registry registry;
    HandleTable handles;
    RegistryThunks thunks{registry, handles};
    GuestMemory memory;
    std::map<std::string, std::string> expected;
    bool is64() const { return GetParam(); }
    u64 root() const { return is64() ? facts::x64::HKEY_CURRENT_USER_NATIVE : facts::x86::HKEY_CURRENT_USER_NATIVE; }
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(0x10000, 0x6000, MemoryProtection::ReadWrite).is_ok());
        auto file = std::filesystem::path(__FILE__).parent_path() / "../../oracle_data" /
            (is64() ? "reg_open64.expected" : "reg_open32.expected");
        std::ifstream in(file);
        ASSERT_TRUE(in.is_open()) << file;
        for (std::string line; std::getline(in, line);) {
            if (line.ends_with('\r')) line.pop_back();
            auto at = line.find(" status=");
            if (at != std::string::npos) expected.emplace(line.substr(0, at), line.substr(at));
        }
        ASSERT_GT(expected.size(), 90u);
    }
    u32 call(const char* name, std::initializer_list<u64> list) {
        const auto exports = create_advapi32_registry_thunks(thunks);
        const auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
            [&](const ExportEntry& value) { return value.name == name; });
        EXPECT_NE(entry, exports.entries.end());
        if (entry == exports.entries.end()) return ~0u;
        std::vector<u64> args(list);
        cpu::CpuState state;
        if (is64()) {
            state.init_long_mode(); state.gpr.rsp = 0x14000;
            std::array<u64*, 4> regs{&state.gpr.rcx, &state.gpr.rdx, &state.gpr.r8, &state.gpr.r9};
            for (usize i = 0; i < args.size(); ++i) {
                if (i < 4) *regs[i] = args[i];
                else (void)memory.write_u64(0x14000 + (i + 1) * 8, args[i]);
            }
        } else {
            state.init_protected_mode(); state.set_sp(0x14000);
            for (usize i = 0; i < args.size(); ++i)
                (void)memory.write_u32(0x14000 + (i + 1) * 4, static_cast<u32>(args[i]));
        }
        Marshaler m(memory); m.attach_cpu(state);
        set_last_error(0xdead);
        entry->handler(m);
        return static_cast<u32>(state.gpr.rax);
    }
    void open(const std::string& label, u64 parent, const char* sub, u32 options, u32 sam, bool no_result = false) {
        for (bool wide : {false, true}) {
            const std::string tag = label + (wide ? " W" : " A");
            SCOPED_TRACE(tag);
            ASSERT_TRUE(expected.contains(tag));
            if (sub) {
                for (usize i = 0;; ++i) {
                    if (wide) (void)memory.write_u16(0x11000 + i * 2, static_cast<u8>(sub[i]));
                    else (void)memory.write_u8(0x11000 + i, static_cast<u8>(sub[i]));
                    if (!sub[i]) break;
                }
            }
            (void)memory.write_u64(0x12000, 0xaaaaaaaa12345678ull);
            if (is64()) (void)memory.write_u64(0x12000, 0x12345678);
            const u32 status = call(wide ? "RegOpenKeyExW" : "RegOpenKeyExA",
                {parent, sub ? 0x11000u : 0u, options, sam, no_result ? 0u : 0x12000u});
            const u64 result = is64() ? memory.read_u64(0x12000).value() : memory.read_u32(0x12000).value();
            const std::string category = result == 0 ? "null" : result == parent ? "same" :
                result == 0x12345678 ? "untouched" : "new";
            std::ostringstream actual;
            actual << " status=" << status << " exception=0 error=" << get_last_error() << " result=" << category;
            EXPECT_EQ(actual.str(), expected.at(tag));
            if (!is64()) EXPECT_EQ(memory.read_u32(0x12004).value(), 0xaaaaaaaau);
            if (status == facts::ERROR_SUCCESS && category == "new")
                EXPECT_EQ(call("RegCloseKey", {result}), facts::ERROR_SUCCESS);
        }
    }
};
TEST_P(RegistryOpenContract, ReplaysEveryMeasuredOpenAndCloseInGuestAbi) {
    u64 key{}, child{};
    ASSERT_EQ(thunks.reg_create_key_ex_w(root(), L"Software\\RegistryOpenContract", &key, nullptr, facts::KEY_ALL_ACCESS), facts::ERROR_SUCCESS);
    ASSERT_EQ(thunks.reg_create_key_ex_w(key, L"child", &child, nullptr, facts::KEY_ALL_ACCESS), facts::ERROR_SUCCESS);
    ASSERT_EQ(thunks.reg_close_key(child), facts::ERROR_SUCCESS);
    const std::pair<const char*, u32> masks[] = {
        {"zero", 0}, {"query", facts::KEY_QUERY_VALUE}, {"set", facts::KEY_SET_VALUE},
        {"maximum", facts::MAXIMUM_ALLOWED}, {"wow32", facts::KEY_QUERY_VALUE | facts::KEY_WOW64_32KEY},
        {"wow64", facts::KEY_QUERY_VALUE | facts::KEY_WOW64_64KEY},
        {"wow-both", facts::KEY_QUERY_VALUE | facts::KEY_WOW64_RES}, {"reserved-access", 0x08000000}};
    for (const auto& [name, sam] : masks) {
        open(std::string("root-null-") + name, root(), nullptr, 0, sam);
        open(std::string("root-empty-") + name, root(), "", 0, sam);
        open(std::string("real-null-") + name, key, nullptr, 0, sam);
        open(std::string("real-empty-") + name, key, "", 0, sam);
        open(std::string("real-child-") + name, key, "child", 0, sam);
    }
    open("root-path", root(), "Software\\RegistryOpenContract", 0, facts::KEY_QUERY_VALUE);
    open("option-open-link", key, "child", facts::REG_OPTION_OPEN_LINK, facts::KEY_QUERY_VALUE);
    open("option-invalid-one", key, "child", 1, facts::KEY_QUERY_VALUE);
    open("option-invalid-high", key, "child", 0x80000000, facts::KEY_QUERY_VALUE);
    open("root-null-option-open-link", root(), nullptr, facts::REG_OPTION_OPEN_LINK, facts::KEY_QUERY_VALUE);
    open("root-null-option-invalid", root(), nullptr, 0x80000000, facts::KEY_QUERY_VALUE);
    open("real-missing", key, "absent", 0, facts::KEY_QUERY_VALUE);
    open("invalid-handle", 0x1234, "child", 0, facts::KEY_QUERY_VALUE);
    open("null-handle", 0, "child", 0, facts::KEY_QUERY_VALUE);
    open("missing-parent", key, "absent\\child", 0, facts::KEY_QUERY_VALUE);
    open("result-null-child", key, "child", 0, facts::KEY_QUERY_VALUE, true);
    open("result-null-root-null", root(), nullptr, 0, facts::KEY_QUERY_VALUE, true);
    open("result-null-missing", key, "absent", 0, facts::KEY_QUERY_VALUE, true);
    if (is64()) {
        open("root-zero-extended", facts::HKEY_CURRENT_USER_LOW, "Software\\RegistryOpenContract", 0, facts::KEY_QUERY_VALUE);
        open("root-sign-extended", root(), "Software\\RegistryOpenContract", 0, facts::KEY_QUERY_VALUE);
        open("root-noncanonical", 0x1234567880000001ull, "Software\\RegistryOpenContract", 0, facts::KEY_QUERY_VALUE);
    }
    const std::pair<const char*, u32> roots[] = {
        {"hkcr", facts::HKEY_CLASSES_ROOT_LOW}, {"hkcu", facts::HKEY_CURRENT_USER_LOW},
        {"hklm", facts::HKEY_LOCAL_MACHINE_LOW}, {"hku", facts::HKEY_USERS_LOW},
        {"hkcc", facts::HKEY_CURRENT_CONFIG_LOW}, {"performance-data", facts::HKEY_PERFORMANCE_DATA_LOW},
        {"performance-text", facts::HKEY_PERFORMANCE_TEXT_LOW}, {"performance-nlstext", facts::HKEY_PERFORMANCE_NLSTEXT_LOW},
        {"null", 0}, {"invalid", 0x1234}};
    const auto close = [&](const std::string& tag, u64 handle, bool has_error = true) {
        SCOPED_TRACE(tag);
        const auto status = call("RegCloseKey", {handle});
        std::ostringstream actual; actual << " status=" << status;
        if (has_error) actual << " error=" << get_last_error();
        EXPECT_EQ(actual.str(), expected.at("close-" + tag));
    };
    for (const auto& [name, low] : roots)
        close(name, is64() ? static_cast<u64>(static_cast<i64>(static_cast<i32>(low))) : low);
    if (is64()) close("hkcu-zero-extended", facts::HKEY_CURRENT_USER_LOW);
    close("real", key, false);
    close("real-again", key);
}
INSTANTIATE_TEST_SUITE_P(BothGuestAbis, RegistryOpenContract, ::testing::Bool());
} // namespace
