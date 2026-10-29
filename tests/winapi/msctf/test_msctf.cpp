/// @file test_msctf.cpp
/// @brief Contract tests for msctf.dll input-scope functions (inputscope.h).

#include <gtest/gtest.h>

#include <algorithm>

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <windowing/model/hwnd_manager.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <winapi/ui/msctf/msctf.hpp>

using namespace vitreo;
using namespace vitreo::windowing;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kDataBase = 0x10000;
constexpr GuestAddr kStackBase = 0x20000;
constexpr GuestAddr kScopes = kDataBase;
constexpr GuestAddr kPhrase = kDataBase + 0x100;
constexpr GuestAddr kPhraseList = kDataBase + 0x180;
constexpr GuestAddr kRegexp = kDataBase + 0x200;
constexpr GuestAddr kSrgs = kDataBase + 0x280;
constexpr GuestAddr kXml = kDataBase + 0x300;
constexpr u32 kSOk = 0x00000000u;
constexpr u32 kEInvalidArg = 0x80070057u;
constexpr i32 kIsDefault = 0;
constexpr i32 kIsUrl = 1;
constexpr i32 kIsEmailUsername = 4;
constexpr i32 kIsEnumString = -5;
constexpr u32 kCallingThread = 1;
constexpr u32 kOtherThread = 2;

class MsctfTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(
            kDataBase, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(
            kStackBase, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = kStackBase + 0x800;
        marshaler.attach_cpu(cpu);
        ThreadManager::set_current_thread_id(kCallingThread);

        WindowData data{};
        data.parent = HWND_DESKTOP;
        data.owner_thread_id = kCallingThread;
        hwnd = hwnd_mgr.create(data);

        WindowData other{};
        other.parent = HWND_DESKTOP;
        other.owner_thread_id = kOtherThread;
        other_hwnd = hwnd_mgr.create(other);

        exports = create_msctf_thunks(&hwnd_mgr);
        set_scope = find("SetInputScope");
        set_scopes = find("SetInputScopes");
        set_scopes2 = find("SetInputScopes2");
        set_xml = find("SetInputScopeXML");
        // Drop any association left by an earlier test on a recycled HWND.
        call_set(hwnd, static_cast<u32>(kIsDefault));
        call_set(other_hwnd, static_cast<u32>(kIsDefault));
    }

    ExportEntry* find(const char* name) {
        const auto it = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [&](const ExportEntry& entry) { return entry.name == name; });
        EXPECT_NE(it, exports.entries.end());
        return it == exports.entries.end() ? nullptr : &*it;
    }

    void set_arg(usize index, u32 value) {
        ASSERT_TRUE(memory.write_u32(
            cpu.gpr.rsp + 4 + index * sizeof(u32), value).is_ok());
    }

    void write_wide(GuestAddr addr, const char* text) {
        usize i = 0;
        for (; text[i] != '\0'; ++i) {
            ASSERT_TRUE(memory.write_u16(
                addr + i * 2, static_cast<u16>(text[i])).is_ok());
        }
        ASSERT_TRUE(memory.write_u16(addr + i * 2, 0).is_ok());
    }

    u32 call_set(u32 target, u32 scope) {
        set_arg(0, target);
        set_arg(1, scope);
        set_scope->handler(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    HwndManager hwnd_mgr;
    HWND hwnd = NULL_HWND;
    HWND other_hwnd = NULL_HWND;
    DllExports exports;
    ExportEntry* set_scope = nullptr;
    ExportEntry* set_scopes = nullptr;
    ExportEntry* set_scopes2 = nullptr;
    ExportEntry* set_xml = nullptr;
};

TEST_F(MsctfTest, StdcallBytesMatchInputScopeHeader) {
    ASSERT_NE(set_scope, nullptr);
    ASSERT_NE(set_scopes, nullptr);
    ASSERT_NE(set_scopes2, nullptr);
    ASSERT_NE(set_xml, nullptr);
    EXPECT_EQ(set_scope->stdcall_bytes, 8u);
    EXPECT_EQ(set_scopes->stdcall_bytes, 28u);
    EXPECT_EQ(set_scopes2->stdcall_bytes, 24u);
    EXPECT_EQ(set_xml->stdcall_bytes, 8u);
}

TEST_F(MsctfTest, SetInputScopeStoresAndDefaultClears) {
    set_last_error(0x1234);
    EXPECT_EQ(call_set(hwnd, static_cast<u32>(kIsEmailUsername)), kSOk);
    EXPECT_EQ(get_last_error(), 0x1234u);
    const auto stored = stored_input_scope(hwnd);
    ASSERT_TRUE(stored.has_value());
    ASSERT_EQ(stored->scopes.size(), 1u);
    EXPECT_EQ(stored->scopes[0], kIsEmailUsername);

    EXPECT_EQ(call_set(hwnd, static_cast<u32>(kIsDefault)), kSOk);
    EXPECT_FALSE(stored_input_scope(hwnd).has_value());
    EXPECT_EQ(get_last_error(), 0x1234u);
}

TEST_F(MsctfTest, SetInputScopeRejectsOtherThreadAndUnknownScope) {
    set_last_error(0x1234);
    EXPECT_EQ(call_set(other_hwnd, static_cast<u32>(kIsUrl)), kEInvalidArg);
    EXPECT_FALSE(stored_input_scope(other_hwnd).has_value());
    EXPECT_EQ(call_set(0, static_cast<u32>(kIsUrl)), kEInvalidArg);
    EXPECT_EQ(call_set(0xDEADBEEFu, static_cast<u32>(kIsUrl)), kEInvalidArg);
    EXPECT_EQ(call_set(hwnd, 0x7Fu), kEInvalidArg);
    EXPECT_FALSE(stored_input_scope(hwnd).has_value());
    EXPECT_EQ(get_last_error(), 0x1234u);
}

TEST_F(MsctfTest, SetInputScopesReplacesPreviousScope) {
    EXPECT_EQ(call_set(hwnd, static_cast<u32>(kIsEmailUsername)), kSOk);
    ASSERT_TRUE(memory.write_u32(kScopes, static_cast<u32>(kIsUrl)).is_ok());
    ASSERT_TRUE(memory.write_u32(kScopes + 4, static_cast<u32>(kIsEnumString)).is_ok());
    write_wide(kPhrase, "suite");
    ASSERT_TRUE(memory.write_u32(kPhraseList, static_cast<u32>(kPhrase)).is_ok());
    write_wide(kRegexp, "a+");
    write_wide(kSrgs, "<grammar/>");

    set_arg(0, hwnd);
    set_arg(1, static_cast<u32>(kScopes));
    set_arg(2, 2);
    set_arg(3, static_cast<u32>(kPhraseList));
    set_arg(4, 1);
    set_arg(5, static_cast<u32>(kRegexp));
    set_arg(6, static_cast<u32>(kSrgs));
    set_scopes->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kSOk);

    const auto stored = stored_input_scope(hwnd);
    ASSERT_TRUE(stored.has_value());
    ASSERT_EQ(stored->scopes.size(), 2u);
    EXPECT_EQ(stored->scopes[0], kIsUrl);
    EXPECT_EQ(stored->scopes[1], kIsEnumString);
    ASSERT_EQ(stored->phrases.size(), 1u);
    EXPECT_EQ(stored->phrases[0], "suite");
    EXPECT_EQ(stored->regexp, "a+");
    EXPECT_EQ(stored->srgs, "<grammar/>");
    EXPECT_TRUE(stored->xml.empty());
    EXPECT_EQ(stored->enum_string, 0u);
}

TEST_F(MsctfTest, SetInputScopesRejectsNullArrayWithCount) {
    EXPECT_EQ(call_set(hwnd, static_cast<u32>(kIsUrl)), kSOk);
    set_arg(0, hwnd);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, 0);
    set_arg(4, 0);
    set_arg(5, 0);
    set_arg(6, 0);
    set_scopes->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kEInvalidArg);
    const auto stored = stored_input_scope(hwnd);
    ASSERT_TRUE(stored.has_value());
    ASSERT_EQ(stored->scopes.size(), 1u);
    EXPECT_EQ(stored->scopes[0], kIsUrl);
}

TEST_F(MsctfTest, SetInputScopes2RequiresCountToMatchPointer) {
    ASSERT_TRUE(memory.write_u32(kScopes, static_cast<u32>(kIsUrl)).is_ok());
    set_arg(0, hwnd);
    set_arg(1, static_cast<u32>(kScopes));
    set_arg(2, 0);
    set_arg(3, 0x5000);
    set_arg(4, 0);
    set_arg(5, 0);
    set_scopes2->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kEInvalidArg);
    EXPECT_FALSE(stored_input_scope(hwnd).has_value());

    set_arg(0, hwnd);
    set_arg(1, static_cast<u32>(kScopes));
    set_arg(2, 1);
    set_arg(3, 0x5000);
    set_arg(4, 0);
    set_arg(5, 0);
    set_scopes2->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kSOk);
    const auto stored = stored_input_scope(hwnd);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->enum_string, 0x5000u);
    ASSERT_EQ(stored->scopes.size(), 1u);
    EXPECT_EQ(stored->scopes[0], kIsUrl);
}

TEST_F(MsctfTest, SetInputScopeXmlKeepsExistingScope) {
    EXPECT_EQ(call_set(hwnd, static_cast<u32>(kIsUrl)), kSOk);
    write_wide(kXml, "<scope/>");
    set_arg(0, hwnd);
    set_arg(1, static_cast<u32>(kXml));
    set_xml->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kSOk);
    const auto stored = stored_input_scope(hwnd);
    ASSERT_TRUE(stored.has_value());
    ASSERT_EQ(stored->scopes.size(), 1u);
    EXPECT_EQ(stored->scopes[0], kIsUrl);
    EXPECT_EQ(stored->xml, "<scope/>");

    set_arg(0, other_hwnd);
    set_arg(1, static_cast<u32>(kXml));
    set_xml->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kEInvalidArg);
    EXPECT_FALSE(stored_input_scope(other_hwnd).has_value());
}

TEST_F(MsctfTest, SetInputScopeUsesX64Registers) {
    cpu.init_long_mode();
    cpu.gpr.rcx = hwnd;
    cpu.gpr.rdx = static_cast<u64>(static_cast<u32>(kIsEmailUsername));
    set_scope->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kSOk);
    const auto stored = stored_input_scope(hwnd);
    ASSERT_TRUE(stored.has_value());
    ASSERT_EQ(stored->scopes.size(), 1u);
    EXPECT_EQ(stored->scopes[0], kIsEmailUsername);
}

} // namespace
