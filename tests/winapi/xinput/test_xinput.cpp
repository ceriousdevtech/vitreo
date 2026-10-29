/// @file test_xinput.cpp
/// @brief XInput DLL state, layout, and connectedness contracts.

#include <gtest/gtest.h>

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <hardware/contracts/gamepad_host.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/media/xinput/xinput.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

class FakeGamepadHost final : public hardware::GamepadHost {
public:
    [[nodiscard]] bool query_state(u32 index,
                                   hardware::GamepadState& out) override {
        ++queries;
        if (fail_query || index >= states.size()) return false;
        out = states[index];
        return true;
    }

    std::array<hardware::GamepadState, 4> states{};
    bool fail_query{false};
    unsigned queries{0};
};

class XinputTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(
            0x1000, 0x2000, MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = 0x1F00;
        host = std::make_shared<FakeGamepadHost>();
    }

    ExportEntry find_export(const char* name) {
        DllExports exports = create_xinput_thunks(XInputDll::V1_3, host);
        for (const auto& e : exports.entries) {
            if (e.name == name) return e;
        }
        ADD_FAILURE() << name << " not found";
        return {};
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    std::shared_ptr<FakeGamepadHost> host;
};

TEST_F(XinputTest, GetStateReportsNoDeviceForValidIndex) {
    auto entry = find_export("XInputGetState");
    ASSERT_TRUE(static_cast<bool>(entry.handler));
    Marshaler m(memory);
    m.attach_cpu(cpu);
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0x1800).is_ok());
    entry.handler(m);
    EXPECT_EQ(cpu.gpr.rax, error_codes::ERROR_DEVICE_NOT_CONNECTED);
}

TEST_F(XinputTest, GetStateRejectsOutOfRangeIndex) {
    auto entry = find_export("XInputGetState");
    ASSERT_TRUE(static_cast<bool>(entry.handler));
    Marshaler m(memory);
    m.attach_cpu(cpu);
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, 4).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0x1800).is_ok());
    entry.handler(m);
    EXPECT_EQ(cpu.gpr.rax, error_codes::ERROR_BAD_ARGUMENTS);
}

// Expected export tables: llvm-objdump -p of the real DLLs (xinput1_3.dll from
// the DirectX redist APR2007_xinput_{x86,x64}.cab; xinput1_4.dll and
// xinput9_1_0.dll from winoracle System32/SysWOW64). x86 == x64 for each.
// Ordinals with no Vitreo implementation (DllMain, the guide-button and
// power-off ordinals 101+) are deliberately absent.
namespace {
struct ExpectedExport {
    const char* name;  // "" = ordinal-only (no name-table entry)
    u16 ordinal;
};

void expect_native_table(XInputDll dll,
                         std::vector<ExpectedExport> expected,
                         std::shared_ptr<FakeGamepadHost> host) {
    DllExports exports = create_xinput_thunks(dll, host);
    std::vector<std::pair<std::string, u16>> got;
    for (const auto& e : exports.entries) got.emplace_back(e.name, e.ordinal);
    std::vector<std::pair<std::string, u16>> want;
    for (const auto& e : expected) want.emplace_back(e.name, e.ordinal);
    std::sort(got.begin(), got.end());
    std::sort(want.begin(), want.end());
    EXPECT_EQ(got, want);
    for (const auto& e : exports.entries) {
        EXPECT_TRUE(static_cast<bool>(e.handler)) << e.ordinal;
    }
}
} // namespace

TEST_F(XinputTest, XInput13MatchesNativeExportTable) {
    expect_native_table(XInputDll::V1_3, {
        {"XInputGetState", 2}, {"XInputSetState", 3},
        {"XInputGetCapabilities", 4}, {"XInputEnable", 5},
        {"XInputGetDSoundAudioDeviceGuids", 6},
        {"XInputGetBatteryInformation", 7}, {"XInputGetKeystroke", 8},
        {"", 100},
    }, host);
}

TEST_F(XinputTest, XInput14MatchesNativeExportTable) {
    expect_native_table(XInputDll::V1_4, {
        {"XInputGetState", 2}, {"XInputSetState", 3},
        {"XInputGetCapabilities", 4}, {"XInputEnable", 5},
        {"XInputGetBatteryInformation", 7}, {"XInputGetKeystroke", 8},
        {"XInputGetAudioDeviceIds", 10}, {"", 100},
    }, host);
}

TEST_F(XinputTest, XInput910MatchesNativeExportTable) {
    expect_native_table(XInputDll::V9_1_0, {
        {"XInputGetCapabilities", 2},
        {"XInputGetDSoundAudioDeviceGuids", 3},
        {"XInputGetState", 4}, {"XInputSetState", 5},
    }, host);
}

// xinput1_3 ordinals 2/3/4 (imported by ordinal) reach GetState, SetState and
// GetCapabilities: an out-of-range user index fails ERROR_BAD_ARGUMENTS
// (Learn: XInputGetState / XInputSetState / XInputGetCapabilities), a valid
// index with no pad fails ERROR_DEVICE_NOT_CONNECTED.
TEST_F(XinputTest, XInput13OrdinalsTwoThreeFourDispatch) {
    DllExports exports = create_xinput_thunks(XInputDll::V1_3, host);
    const auto by_ordinal = [&](u16 ordinal) {
        for (const auto& e : exports.entries)
            if (e.ordinal == ordinal) return e;
        ADD_FAILURE() << "ordinal " << ordinal;
        return ExportEntry{};
    };
    Marshaler m(memory);
    m.attach_cpu(cpu);
    // Ordinal 2: XInputGetState(user, pState)
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0x1800).is_ok());
    by_ordinal(2).handler(m);
    EXPECT_EQ(cpu.gpr.rax, error_codes::ERROR_DEVICE_NOT_CONNECTED);
    // Ordinal 3: XInputSetState(user, pVibration)
    by_ordinal(3).handler(m);
    EXPECT_EQ(cpu.gpr.rax, error_codes::ERROR_DEVICE_NOT_CONNECTED);
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, 4).is_ok());
    by_ordinal(3).handler(m);
    EXPECT_EQ(cpu.gpr.rax, error_codes::ERROR_BAD_ARGUMENTS);
    // Ordinal 4: XInputGetCapabilities(user, flags, pCaps)
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, 0x1800).is_ok());
    host->states[0].connected = true;
    by_ordinal(4).handler(m);
    EXPECT_EQ(cpu.gpr.rax, error_codes::ERROR_SUCCESS);
    EXPECT_EQ(memory.read_u8(0x1800).value(), 0x01u);  // XINPUT_DEVTYPE_GAMEPAD
}

TEST_F(XinputTest, GetStateMarshalsConnectedControllerAndTracksChanges) {
    host->states[0] = hardware::GamepadState{
        .connected = true,
        .buttons = 0x9109,
        .left_trigger = 17,
        .right_trigger = 231,
        .left_x = -32768,
        .left_y = 32767,
        .right_x = -1234,
        .right_y = 5678,
    };
    DllExports exports = create_xinput_thunks(XInputDll::V1_3, host);
    const auto it = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) { return entry.name == "XInputGetState"; });
    ASSERT_NE(it, exports.entries.end());

    Marshaler m(memory);
    m.attach_cpu(cpu);
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0x1800).is_ok());
    it->handler(m);
    ASSERT_EQ(cpu.gpr.rax, error_codes::ERROR_SUCCESS);
    EXPECT_EQ(memory.read_u32(0x1800).value(), 1u);
    EXPECT_EQ(memory.read_u16(0x1804).value(), 0x9109u);
    EXPECT_EQ(memory.read_u8(0x1806).value(), 17u);
    EXPECT_EQ(memory.read_u8(0x1807).value(), 231u);
    EXPECT_EQ(memory.read_u16(0x1808).value(), 0x8000u);
    EXPECT_EQ(memory.read_u16(0x180A).value(), 0x7FFFu);
    EXPECT_EQ(memory.read_u16(0x180C).value(),
              static_cast<u16>(-1234));
    EXPECT_EQ(memory.read_u16(0x180E).value(), 5678u);

    it->handler(m);
    EXPECT_EQ(memory.read_u32(0x1800).value(), 1u);
    host->states[0].right_y = 6000;
    it->handler(m);
    EXPECT_EQ(memory.read_u32(0x1800).value(), 2u);
    EXPECT_EQ(memory.read_u16(0x180E).value(), 6000u);
}

TEST_F(XinputTest, GetCapabilitiesDescribesStandardGamepad) {
    host->states[0].connected = true;
    auto entry = find_export("XInputGetCapabilities");
    Marshaler m(memory);
    m.attach_cpu(cpu);
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, 0x1800).is_ok());
    entry.handler(m);

    ASSERT_EQ(cpu.gpr.rax, error_codes::ERROR_SUCCESS);
    EXPECT_EQ(memory.read_u8(0x1800).value(), 0x01u);
    EXPECT_EQ(memory.read_u8(0x1801).value(), 1u);
    EXPECT_EQ(memory.read_u16(0x1804).value(), 0xF3FFu);
    EXPECT_EQ(memory.read_u8(0x1806).value(), 0xFFu);
    EXPECT_EQ(memory.read_u16(0x1810).value(), 0u);
}

TEST_F(XinputTest, FailedAndDisconnectedQueriesDoNotAdvanceConnectedPacket) {
    auto entry = find_export("XInputGetState");
    Marshaler m(memory);
    m.attach_cpu(cpu);
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0x1800).is_ok());
    host->fail_query = true;
    testing::internal::CaptureStderr();
    entry.handler(m);
    EXPECT_EQ(cpu.gpr.rax, error_codes::ERROR_GEN_FAILURE);
    host->fail_query = false;
    entry.handler(m);
    EXPECT_EQ(cpu.gpr.rax, error_codes::ERROR_DEVICE_NOT_CONNECTED);
    host->states[0].connected = true;
    entry.handler(m);
    EXPECT_EQ(cpu.gpr.rax, error_codes::ERROR_SUCCESS);
    EXPECT_EQ(memory.read_u32(0x1800).value(), 1u);
    entry.handler(m);
    EXPECT_EQ(memory.read_u32(0x1800).value(), 1u);
    const std::string output = testing::internal::GetCapturedStderr();
    EXPECT_EQ(host->queries, 4u); // Diagnostics must not query the host again.
    const char* diag = std::getenv("VITREO_INPUT_DIAG");
    if (diag && diag[0] && diag[0] != '0') {
        const auto first = output.find("[xinput] first-query");
        EXPECT_NE(first, std::string::npos);
        EXPECT_EQ(output.find("[xinput] first-query", first + 1), std::string::npos);
        EXPECT_NE(output.find("query_result=" + std::to_string(error_codes::ERROR_GEN_FAILURE) + " host_query=0 connected=0 enabled=1"), std::string::npos);
    } else {
        EXPECT_TRUE(output.empty());
    }
}
