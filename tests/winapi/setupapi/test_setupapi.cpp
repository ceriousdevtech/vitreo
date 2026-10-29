// SPDX-License-Identifier: LGPL-2.1-only

#include <gtest/gtest.h>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/common/display_device_identity.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "hardware/contracts/bluetooth_host.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/system/hid/hid.hpp"
#include "winapi/system/setupapi/setupapi.hpp"

#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

class SetupapiTest : public ::testing::Test {
protected:
    void SetUp() override {
        display_device_identity::set_adapter_identity({});
        ASSERT_TRUE(memory.map_region(
            0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(
            0x80000, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = 0x80800;
        marshaler.attach_cpu(cpu);
        exports = create_setupapi_thunks(
            [] { return std::string{"Test Apple GPU"}; });
    }

    void arg(usize index, u32 value) {
        ASSERT_TRUE(memory.write_u32(
            cpu.sp() + 4 + index * 4, value).is_ok());
    }

    void call(const char* name) {
        for (auto& entry : exports.entries) {
            if (entry.name == name) {
                entry.handler(marshaler);
                return;
            }
        }
        FAIL() << "missing export " << name;
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    DllExports exports;
};

TEST_F(SetupapiTest, EnumeratesMetalDisplayAndReportsActualName) {
    constexpr GuestAddr guid = 0x1100;
    constexpr GuestAddr device_info = 0x1200;
    constexpr GuestAddr property = 0x1300;
    constexpr GuestAddr required = 0x1400;
    constexpr std::array<u8, 16> display_class{
        0x68, 0xE9, 0x36, 0x4D, 0x25, 0xE3, 0xCE, 0x11,
        0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18};
    ASSERT_TRUE(memory.write_bytes(
        guid, display_class.data(), display_class.size()).is_ok());

    arg(0, guid);
    arg(1, 0);
    arg(2, 0);
    arg(3, 0x2); // DIGCF_PRESENT
    call("SetupDiGetClassDevsA");
    const u32 set = static_cast<u32>(cpu.gpr.rax);
    ASSERT_NE(set, 0xFFFFFFFFu);

    ASSERT_TRUE(memory.write_u32(device_info, 28).is_ok());
    arg(0, set);
    arg(1, 0);
    arg(2, device_info);
    call("SetupDiEnumDeviceInfo");
    ASSERT_EQ(cpu.gpr.rax, 1u);

    arg(0, set);
    arg(1, device_info);
    arg(2, 0); // SPDRP_DEVICEDESC
    arg(3, 0);
    arg(4, property);
    arg(5, 128);
    arg(6, required);
    call("SetupDiGetDeviceRegistryPropertyA");
    ASSERT_EQ(cpu.gpr.rax, 1u);

    std::array<char, 128> text{};
    ASSERT_TRUE(memory.read_bytes(
        property, reinterpret_cast<Byte*>(text.data()),
        text.size()).is_ok());
    EXPECT_STREQ(text.data(), "Test Apple GPU");
    EXPECT_EQ(memory.read_u32(required).value(), 15u);
}

TEST_F(SetupapiTest, BuildsCompatibleDriverListAndReportsInstallParams) {
    constexpr GuestAddr guid = 0x1100;
    constexpr GuestAddr device_info = 0x1200;
    constexpr GuestAddr params = 0x1300;
    constexpr GuestAddr driver_info = 0x2000;
    constexpr std::array<u8, 16> display_class{
        0x68, 0xE9, 0x36, 0x4D, 0x25, 0xE3, 0xCE, 0x11,
        0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18};
    ASSERT_TRUE(memory.write_bytes(
        guid, display_class.data(), display_class.size()).is_ok());

    arg(0, guid);
    arg(1, 0);
    arg(2, 0);
    arg(3, 0x2); // DIGCF_PRESENT
    call("SetupDiGetClassDevsW");
    const u32 set = static_cast<u32>(cpu.gpr.rax);

    ASSERT_TRUE(memory.write_u32(device_info, 28).is_ok());
    arg(0, set);
    arg(1, 0);
    arg(2, device_info);
    call("SetupDiEnumDeviceInfo");
    ASSERT_EQ(cpu.gpr.rax, 1u);

    arg(0, set);
    arg(1, device_info);
    arg(2, 2); // SPDIT_COMPATDRIVER
    call("SetupDiBuildDriverInfoList");
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);

    ASSERT_TRUE(memory.write_u32(params, 556).is_ok());
    ASSERT_TRUE(memory.write_u32(params + 4, 0x4000).is_ok());
    ASSERT_TRUE(memory.write_u32(params + 8, 0x200).is_ok());
    arg(0, set);
    arg(1, device_info);
    arg(2, params);
    call("SetupDiSetDeviceInstallParamsW");
    ASSERT_EQ(cpu.gpr.rax, 1u);

    arg(0, set);
    arg(1, device_info);
    arg(2, params);
    call("SetupDiGetDeviceInstallParamsW");
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(params).value(), 556u);
    EXPECT_EQ(memory.read_u32(params + 4).value(), 0x4000u);
    EXPECT_EQ(memory.read_u32(params + 8).value(), 0x220u);
    EXPECT_EQ(memory.read_u16(params + 36).value(), 0u);

    ASSERT_TRUE(memory.write_u32(driver_info, 1564).is_ok());
    arg(0, set);
    arg(1, device_info);
    arg(2, 2); // SPDIT_COMPATDRIVER
    arg(3, 0);
    arg(4, driver_info);
    call("SetupDiEnumDriverInfoW");
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(driver_info + 4).value(), 2u);
    EXPECT_EQ(memory.read_u16(driver_info + 12).value(), u'T');

    arg(3, 1);
    call("SetupDiEnumDriverInfoW");
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NO_MORE_ITEMS);
}

TEST_F(SetupapiTest, RejectsMalformedDriverListAndInstallParamsInputs) {
    constexpr GuestAddr guid = 0x1100;
    constexpr GuestAddr device_info = 0x1200;
    constexpr GuestAddr params = 0x1300;
    constexpr std::array<u8, 16> display_class{
        0x68, 0xE9, 0x36, 0x4D, 0x25, 0xE3, 0xCE, 0x11,
        0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18};
    ASSERT_TRUE(memory.write_bytes(
        guid, display_class.data(), display_class.size()).is_ok());
    arg(0, guid);
    arg(1, 0);
    arg(2, 0);
    arg(3, 0x2);
    call("SetupDiGetClassDevsW");
    const u32 set = static_cast<u32>(cpu.gpr.rax);

    ASSERT_TRUE(memory.write_u32(device_info, 28).is_ok());
    ASSERT_TRUE(memory.write_u32(device_info + 20, 99).is_ok());
    arg(0, set);
    arg(1, device_info);
    arg(2, 2);
    call("SetupDiBuildDriverInfoList");
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    ASSERT_TRUE(memory.write_u32(params, 12).is_ok());
    arg(0, set);
    arg(1, 0);
    arg(2, params);
    call("SetupDiGetDeviceInstallParamsW");
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), 1784u); // ERROR_INVALID_USER_BUFFER
}

TEST_F(SetupapiTest, ReportsWideDisplayPropertiesWithByteSizedBuffers) {
    constexpr GuestAddr guid = 0x1100;
    constexpr GuestAddr device_info = 0x1200;
    constexpr GuestAddr property = 0x1300;
    constexpr GuestAddr type = 0x1400;
    constexpr GuestAddr required = 0x1500;
    constexpr std::array<u8, 16> display_class{
        0x68, 0xE9, 0x36, 0x4D, 0x25, 0xE3, 0xCE, 0x11,
        0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18};
    ASSERT_TRUE(memory.write_bytes(
        guid, display_class.data(), display_class.size()).is_ok());

    arg(0, guid);
    arg(1, 0);
    arg(2, 0);
    arg(3, 0x2); // DIGCF_PRESENT
    call("SetupDiGetClassDevsW");
    const u32 set = static_cast<u32>(cpu.gpr.rax);
    ASSERT_NE(set, 0xFFFFFFFFu);

    ASSERT_TRUE(memory.write_u32(device_info, 28).is_ok());
    arg(0, set);
    arg(1, 0);
    arg(2, device_info);
    call("SetupDiEnumDeviceInfo");
    ASSERT_EQ(cpu.gpr.rax, 1u);

    arg(0, set);
    arg(1, device_info);
    arg(2, 0); // SPDRP_DEVICEDESC
    arg(3, type);
    arg(4, 0);
    arg(5, 0);
    arg(6, required);
    call("SetupDiGetDeviceRegistryPropertyW");
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(memory.read_u32(type).value(), 1u); // REG_SZ
    EXPECT_EQ(memory.read_u32(required).value(), 30u);

    arg(4, property);
    arg(5, 30);
    call("SetupDiGetDeviceRegistryPropertyW");
    ASSERT_EQ(cpu.gpr.rax, 1u);
    const std::u16string expected = u"Test Apple GPU";
    std::array<u16, 15> text{};
    ASSERT_TRUE(memory.read_bytes(
        property, reinterpret_cast<Byte*>(text.data()),
        sizeof(text)).is_ok());
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), text.begin()));
    EXPECT_EQ(text.back(), 0u);

    arg(2, 1); // SPDRP_HARDWAREID
    arg(5, 128);
    call("SetupDiGetDeviceRegistryPropertyW");
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(type).value(), 7u); // REG_MULTI_SZ
    const u32 hardware_id_bytes = memory.read_u32(required).value();
    ASSERT_GE(hardware_id_bytes, 4u);
    EXPECT_EQ(hardware_id_bytes, 38u);
    EXPECT_EQ(memory.read_u16(property + hardware_id_bytes - 4).value(), 0u);
    EXPECT_EQ(memory.read_u16(property + hardware_id_bytes - 2).value(), 0u);
}

TEST_F(SetupapiTest, WideHardwareIdMatchesConfiguredPciIdentity) {
    display_device_identity::AdapterIdentity identity;
    identity.vendor_id = 0x10DE;
    identity.device_id = 0x1B80;
    identity.subsystem_id = 0x119E10DE;
    identity.revision = 0xA1;
    display_device_identity::set_adapter_identity(identity);

    constexpr GuestAddr guid = 0x1100;
    constexpr GuestAddr device_info = 0x1200;
    constexpr GuestAddr property = 0x1300;
    constexpr GuestAddr required = 0x1500;
    constexpr std::array<u8, 16> display_class{
        0x68, 0xE9, 0x36, 0x4D, 0x25, 0xE3, 0xCE, 0x11,
        0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18};
    ASSERT_TRUE(memory.write_bytes(
        guid, display_class.data(), display_class.size()).is_ok());

    arg(0, guid);
    arg(1, 0);
    arg(2, 0);
    arg(3, 0x2);
    call("SetupDiGetClassDevsW");
    const u32 set = static_cast<u32>(cpu.gpr.rax);
    ASSERT_TRUE(memory.write_u32(device_info, 28).is_ok());
    arg(0, set);
    arg(1, 0);
    arg(2, device_info);
    call("SetupDiEnumDeviceInfo");
    ASSERT_EQ(cpu.gpr.rax, 1u);

    arg(0, set);
    arg(1, device_info);
    arg(2, 1); // SPDRP_HARDWAREID
    arg(3, 0);
    arg(4, property);
    arg(5, 128);
    arg(6, required);
    call("SetupDiGetDeviceRegistryPropertyW");
    ASSERT_EQ(cpu.gpr.rax, 1u);

    constexpr std::u16string_view expected =
        u"PCI\\VEN_10DE&DEV_1B80&SUBSYS_119E10DE&REV_A1";
    std::vector<u16> text(expected.size() + 2);
    ASSERT_TRUE(memory.read_bytes(
        property, reinterpret_cast<Byte*>(text.data()),
        text.size() * sizeof(u16)).is_ok());
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), text.begin()));
    EXPECT_EQ(text[expected.size()], 0u);
    EXPECT_EQ(text[expected.size() + 1], 0u);
}

TEST_F(SetupapiTest, EnumeratesMetalDisplayDeviceInterfaceWithSizeQuery) {
    constexpr GuestAddr guid = 0x1100;
    constexpr GuestAddr interface_data = 0x1200;
    constexpr GuestAddr required = 0x1300;
    constexpr GuestAddr detail = 0x1400;
    constexpr GuestAddr device_info = 0x1500;
    constexpr std::array<u8, 16> display_device_arrival{
        0x80, 0x51, 0xA0, 0x1C, 0x99, 0xA6, 0x0A, 0x45,
        0x9A, 0x0C, 0xDE, 0x4F, 0xBE, 0x3D, 0xDD, 0x89};
    ASSERT_TRUE(memory.write_bytes(
        guid, display_device_arrival.data(),
        display_device_arrival.size()).is_ok());

    arg(0, guid);
    arg(1, 0);
    arg(2, 0);
    arg(3, 0x12); // DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    call("SetupDiGetClassDevsA");
    const u32 set = static_cast<u32>(cpu.gpr.rax);
    ASSERT_NE(set, 0xFFFFFFFFu);

    ASSERT_TRUE(memory.write_u32(interface_data, 28).is_ok());
    arg(0, set);
    arg(1, 0);
    arg(2, guid);
    arg(3, 0);
    arg(4, interface_data);
    call("SetupDiEnumDeviceInterfaces");
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(interface_data + 20).value(), 1u);

    arg(0, set);
    arg(1, interface_data);
    arg(2, 0);
    arg(3, 0);
    arg(4, required);
    arg(5, 0);
    call("SetupDiGetDeviceInterfaceDetailA");
    ASSERT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(
        get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    const u32 detail_size = memory.read_u32(required).value();
    ASSERT_GT(detail_size, 5u);

    ASSERT_TRUE(memory.write_u32(detail, 5).is_ok());
    ASSERT_TRUE(memory.write_u32(device_info, 28).is_ok());
    arg(0, set);
    arg(1, interface_data);
    arg(2, detail);
    arg(3, detail_size);
    arg(4, required);
    arg(5, device_info);
    call("SetupDiGetDeviceInterfaceDetailA");
    ASSERT_EQ(cpu.gpr.rax, 1u);

    std::array<char, 128> path{};
    ASSERT_TRUE(memory.read_bytes(
        detail + 4, reinterpret_cast<Byte*>(path.data()),
        path.size()).is_ok());
    // SetupDi reports the symbolic link in lower case, ending in the
    // interface class GUID (s7_setupapi_probe.expected detw_full:
    // all_lower=1, tail={5b45201d-...} for the adapter interface).
    EXPECT_STREQ(
        path.data(),
        "\\\\?\\root#vitreo_metal#0000#"
        "{1ca05180-a699-450a-9a0c-de4fbe3ddd89}");
    // The DeviceInfoData is filled with the display element (detw_query:
    // class GUID and a non-zero DevInst).
    EXPECT_NE(memory.read_u32(device_info + 20).value(), 0u);
}

TEST_F(SetupapiTest, ClassGuidsFromNameAReturnsDisplayClass) {
    constexpr GuestAddr name = 0x1100;
    constexpr GuestAddr guid_list = 0x1200;
    constexpr GuestAddr required = 0x1300;
    const char* class_name = "Display";
    for (usize i = 0; class_name[i] != '\0'; ++i) {
        ASSERT_TRUE(memory.write_u8(
            name + i, static_cast<u8>(class_name[i])).is_ok());
    }
    ASSERT_TRUE(memory.write_u8(name + 7, 0).is_ok());

    arg(0, name);
    arg(1, guid_list);
    arg(2, 0);
    arg(3, required);
    call("SetupDiClassGuidsFromNameA");
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(memory.read_u32(required).value(), 1u);

    arg(2, 1);
    call("SetupDiClassGuidsFromNameA");
    EXPECT_EQ(cpu.gpr.rax, 1u);
    constexpr std::array<u8, 16> display_class{
        0x68, 0xE9, 0x36, 0x4D, 0x25, 0xE3, 0xCE, 0x11,
        0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18};
    std::array<u8, 16> got{};
    ASSERT_TRUE(memory.read_bytes(guid_list, got.data(), got.size()).is_ok());
    EXPECT_EQ(got, display_class);
}

TEST_F(SetupapiTest, CfgmgrRefusesEnableDisableDevNode) {
    auto cfg = create_cfgmgr32_thunks();
    auto call_cfg = [&](const char* name) {
        for (auto& entry : cfg.entries) {
            if (entry.name == name) {
                entry.handler(marshaler);
                return;
            }
        }
        FAIL() << "missing export " << name;
    };
    constexpr GuestAddr out = 0x1500;
    arg(0, out);
    arg(1, 0);
    arg(2, 0);
    call_cfg("CM_Locate_DevNodeW");
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const u32 root = memory.read_u32(out).value();

    // s7_cfgmgr_probe.expected: disable_root CR_FAILURE, enable_root
    // CR_INVALID_DEVNODE, disable_junk CR_INVALID_DEVNODE, enable/disable
    // with unknown flags CR_INVALID_FLAG.
    arg(0, root);
    arg(1, 0x4); // CM_DISABLE_UI_NOT_OK
    call_cfg("CM_Disable_DevNode");
    EXPECT_EQ(cpu.gpr.rax, 0x13u);
    arg(1, 0);
    call_cfg("CM_Enable_DevNode");
    EXPECT_EQ(cpu.gpr.rax, 0x05u);
    arg(1, 0x80);
    call_cfg("CM_Enable_DevNode");
    EXPECT_EQ(cpu.gpr.rax, 0x04u);
    call_cfg("CM_Disable_DevNode");
    EXPECT_EQ(cpu.gpr.rax, 0x04u);
    arg(0, 0x7fff1234);
    arg(1, 0);
    call_cfg("CM_Disable_DevNode");
    EXPECT_EQ(cpu.gpr.rax, 0x05u);
}

TEST_F(SetupapiTest, DeviceInterfaceListSizeIsEmptyMultiSz) {
    auto cfg = create_cfgmgr32_thunks();
    auto call_cfg = [&](const char* name) {
        for (auto& entry : cfg.entries) {
            if (entry.name == name) {
                entry.handler(marshaler);
                return;
            }
        }
        FAIL() << "missing export " << name;
    };

    // s7_cfgmgr_probe.expected ifl_size_bogus: an interface class with no
    // interfaces is an empty MULTI_SZ, one character; ifl_size_nullguid: a
    // NULL class GUID is CR_FAILURE.
    constexpr GuestAddr len = 0x1500;
    constexpr GuestAddr guid = 0x1600;
    const std::array<u8, 16> bogus{0x3e, 0x5d, 0x1c, 0x7a, 0x11, 0x11, 0x22, 0x42,
                                   0x93, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
    ASSERT_TRUE(memory.write_bytes(guid, bogus.data(), bogus.size()).is_ok());
    ASSERT_TRUE(memory.write_u32(len, 0xFFFFFFFFu).is_ok());
    arg(0, len);
    arg(1, guid);
    arg(2, 0);
    arg(3, 0);
    call_cfg("CM_Get_Device_Interface_List_SizeA");
    EXPECT_EQ(cpu.gpr.rax, 0u); // CR_SUCCESS
    EXPECT_EQ(memory.read_u32(len).value(), 1u);

    arg(1, 0);
    call_cfg("CM_Get_Device_Interface_List_SizeA");
    EXPECT_EQ(cpu.gpr.rax, 0x13u); // CR_FAILURE

    bool on_setupapi = false;
    for (const auto& entry : exports.entries) {
        if (entry.name == "CM_Get_Device_Interface_List_SizeA") {
            on_setupapi = true;
        }
    }
    EXPECT_TRUE(on_setupapi);
}

TEST_F(SetupapiTest, DeviceIdSizeForRootIsItsInstanceId) {
    auto cfg = create_cfgmgr32_thunks();
    auto call_cfg = [&](const char* name) {
        for (auto& entry : cfg.entries) {
            if (entry.name == name) {
                entry.handler(marshaler);
                return;
            }
        }
        FAIL() << "missing export " << name;
    };
    constexpr GuestAddr out = 0x1500;
    arg(0, out);
    arg(1, 0);
    arg(2, 0);
    call_cfg("CM_Locate_DevNodeW");
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const u32 root = memory.read_u32(out).value();

    // s7_cfgmgr_probe.expected getidsize_root: "HTREE\ROOT\0" is 12
    // characters (the size excludes the NUL); getidsize_junk writes 0 and
    // fails CR_INVALID_DEVNODE.
    constexpr GuestAddr len = 0x1600;
    ASSERT_TRUE(memory.write_u32(len, 0xFFFFFFFFu).is_ok());
    arg(0, len);
    arg(1, root);
    arg(2, 0);
    call_cfg("CM_Get_Device_ID_Size");
    EXPECT_EQ(cpu.gpr.rax, 0u); // CR_SUCCESS
    EXPECT_EQ(memory.read_u32(len).value(), 12u);

    arg(1, 0x7fff1234);
    call_cfg("CM_Get_Device_ID_Size");
    EXPECT_EQ(cpu.gpr.rax, 0x05u);
    EXPECT_EQ(memory.read_u32(len).value(), 0u);
}

TEST_F(SetupapiTest, RegisterNotificationReturnsHandleAndUnregisters) {
    auto cfg = create_cfgmgr32_thunks();
    auto call_cfg = [&](const char* name) {
        for (auto& entry : cfg.entries) {
            if (entry.name == name) {
                entry.handler(marshaler);
                return;
            }
        }
        FAIL() << "missing export " << name;
    };

    constexpr GuestAddr filter = 0x1700;
    constexpr GuestAddr callback = 0x1800;
    constexpr GuestAddr out = 0x1900;
    ASSERT_TRUE(memory.write_u32(filter, 16).is_ok());
    ASSERT_TRUE(memory.write_u32(out, 0).is_ok());
    arg(0, filter);
    arg(1, 0);
    arg(2, callback);
    arg(3, out);
    call_cfg("CM_Register_Notification");
    EXPECT_EQ(cpu.gpr.rax, 0u); // CR_SUCCESS
    const auto handle = memory.read_u32(out);
    ASSERT_TRUE(handle.is_ok());
    EXPECT_NE(handle.value(), 0u);

    arg(0, handle.value());
    call_cfg("CM_Unregister_Notification");
    EXPECT_EQ(cpu.gpr.rax, 0u);

    // setupapi.dll does not export CM_Register_Notification (its export
    // table forwards only part of the CM_* surface), so GetProcAddress on it
    // must fail.
    bool on_setupapi = false;
    for (const auto& entry : exports.entries) {
        if (entry.name == "CM_Register_Notification") on_setupapi = true;
    }
    EXPECT_FALSE(on_setupapi);
}

TEST_F(SetupapiTest, PublishesDecompressOrCopyFileExports) {
    bool found_a = false;
    bool found_w = false;
    for (const auto& entry : exports.entries) {
        if (entry.name == "SetupDecompressOrCopyFileA") {
            found_a = true;
            EXPECT_EQ(entry.stdcall_bytes, 12);
        }
        if (entry.name == "SetupDecompressOrCopyFileW") {
            found_w = true;
            EXPECT_EQ(entry.stdcall_bytes, 12);
        }
    }
    EXPECT_TRUE(found_a);
    EXPECT_TRUE(found_w);
}

class SetupapiDecompressTest : public ::testing::Test {
protected:
    void SetUp() override {
        root = std::filesystem::temp_directory_path() /
            ("vitreo-setupapi-test-" + std::to_string(::getpid()));
        std::error_code error;
        std::filesystem::remove_all(root, error);
        ASSERT_TRUE(std::filesystem::create_directories(root, error));
        ASSERT_TRUE(vfs.mount_drive(
            'C', std::make_unique<capsule::HostFileSystem>(
                     root.string())).is_ok());
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    void write_file(std::string_view relative, const std::vector<u8>& bytes) {
        std::ofstream stream(
            root / relative, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(stream.good());
        stream.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(stream.good());
    }

    [[nodiscard]] std::vector<u8> read_file(std::string_view relative) {
        std::ifstream stream(root / relative, std::ios::binary);
        return std::vector<u8>(
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>());
    }

    std::filesystem::path root;
    capsule::Vfs vfs;
};

TEST_F(SetupapiDecompressTest, CopiesCompressedFormNameToExactTarget) {
    const std::vector<u8> mz{'M', 'Z', 0x90, 0x00};
    write_file("payload.ex_", mz);

    EXPECT_EQ(
        setup_decompress_or_copy_file(
            vfs, "C:\\payload.exe", "C:\\payload.exe", nullptr),
        error_codes::ERROR_SUCCESS);
    EXPECT_EQ(read_file("payload.exe"), mz);
}

TEST_F(SetupapiDecompressTest, DoesNotSearchCompressedFormWhenTypeSpecified) {
    const std::vector<u8> mz{'M', 'Z'};
    write_file("payload.ex_", mz);
    const u32 none = file_compression::NONE;

    EXPECT_EQ(
        setup_decompress_or_copy_file(
            vfs, "C:\\payload.exe", "C:\\payload.exe", &none),
        error_codes::ERROR_FILE_NOT_FOUND);
}

TEST_F(SetupapiDecompressTest, ExpandsSzddWhenTypeIsUnspecified) {
    std::vector<u8> compressed(14, 0);
    const std::array<u8, 8> signature{
        'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33};
    std::copy(signature.begin(), signature.end(), compressed.begin());
    compressed[8] = 'A';
    compressed[9] = 'l';
    compressed[10] = 3;
    compressed.push_back(0x07);
    compressed.push_back('a');
    compressed.push_back('b');
    compressed.push_back('c');
    write_file("setup.dl_", compressed);

    EXPECT_EQ(
        setup_decompress_or_copy_file(
            vfs, "C:\\setup.dll", "C:\\setup.dll", nullptr),
        error_codes::ERROR_SUCCESS);
    const auto out = read_file("setup.dll");
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0], 'a');
    EXPECT_EQ(out[1], 'b');
    EXPECT_EQ(out[2], 'c');
}

TEST_F(SetupapiDecompressTest, RejectsMissingSource) {
    EXPECT_EQ(
        setup_decompress_or_copy_file(
            vfs, "C:\\missing.exe", "C:\\out.exe", nullptr),
        error_codes::ERROR_FILE_NOT_FOUND);
}

namespace {

std::vector<u8> make_szdd_abc(char lastchar) {
    std::vector<u8> compressed(14, 0);
    const std::array<u8, 8> signature{
        'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33};
    std::copy(signature.begin(), signature.end(), compressed.begin());
    compressed[8] = 'A';
    compressed[9] = static_cast<u8>(lastchar);
    compressed[10] = 3;
    compressed.push_back(0x07);
    compressed.push_back('a');
    compressed.push_back('b');
    compressed.push_back('c');
    return compressed;
}

} // namespace

TEST_F(SetupapiTest, PublishesSetupInstallFileExports) {
    bool found_a = false;
    bool found_w = false;
    bool found_queue = false;
    bool found_commit = false;
    for (const auto& entry : exports.entries) {
        if (entry.name == "SetupInstallFileA") {
            found_a = true;
            EXPECT_EQ(entry.stdcall_bytes, 32);
        }
        if (entry.name == "SetupInstallFileW") {
            found_w = true;
            EXPECT_EQ(entry.stdcall_bytes, 32);
        }
        if (entry.name == "SetupQueueCopyA") found_queue = true;
        if (entry.name == "SetupCommitFileQueue") found_commit = true;
    }
    EXPECT_TRUE(found_a);
    EXPECT_TRUE(found_w);
    EXPECT_TRUE(found_queue);
    EXPECT_TRUE(found_commit);
}

TEST_F(SetupapiDecompressTest, InstallFileDirectoryDestUsesUncompressedSzddName) {
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directories(root / "x86", error));
    ASSERT_TRUE(std::filesystem::create_directories(root / "temp", error));
    write_file("x86/cmd.ex_", make_szdd_abc('e'));

    EXPECT_EQ(
        setup_install_file(
            vfs, "C:\\x86\\cmd.ex_", "", "C:\\temp",
            sp_copy::SOURCE_ABSOLUTE),
        error_codes::ERROR_SUCCESS);
    const auto out = read_file("temp/cmd.exe");
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0], 'a');
    EXPECT_EQ(out[1], 'b');
    EXPECT_EQ(out[2], 'c');
    EXPECT_TRUE(read_file("temp/cmd.ex_").empty());
}

TEST_F(SetupapiDecompressTest, InstallFileNodecompKeepsCompressedSourceName) {
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directories(root / "x86", error));
    ASSERT_TRUE(std::filesystem::create_directories(root / "temp", error));
    write_file("x86/cmd.ex_", make_szdd_abc('e'));

    EXPECT_EQ(
        setup_install_file(
            vfs, "C:\\x86\\cmd.ex_", "", "C:\\temp\\cmd.exe",
            sp_copy::SOURCE_ABSOLUTE | sp_copy::NODECOMP),
        error_codes::ERROR_SUCCESS);
    const auto out = read_file("temp/cmd.ex_");
    ASSERT_GE(out.size(), 4u);
    EXPECT_EQ(out[0], 'S');
    EXPECT_TRUE(read_file("temp/cmd.exe").empty());
}

TEST_F(SetupapiDecompressTest, InstallFileDirectoryDestKeepsNonSzddSourceName) {
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directories(root / "media", error));
    ASSERT_TRUE(std::filesystem::create_directories(root / "bin", error));
    const std::vector<u8> mz{'M', 'Z', 0x90, 0x00};
    write_file("media/payload.ex_", mz);

    EXPECT_EQ(
        setup_install_file(
            vfs, "C:\\media\\payload.ex_", "", "C:\\bin",
            sp_copy::SOURCE_ABSOLUTE),
        error_codes::ERROR_SUCCESS);
    EXPECT_EQ(read_file("bin/payload.ex_"), mz);
    EXPECT_TRUE(read_file("bin/payload.exe").empty());
}

TEST_F(SetupapiDecompressTest, InstallFileFullDestPathIsCallerSupplied) {
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directories(root / "bin", error));
    const std::vector<u8> mz{'M', 'Z'};
    write_file("bin/payload.ex_", mz);

    EXPECT_EQ(
        setup_install_file(
            vfs, "payload.ex_", "C:\\bin", "C:\\bin\\launcher.exe", 0),
        error_codes::ERROR_SUCCESS);
    EXPECT_EQ(read_file("bin/launcher.exe"), mz);
}

// =============================================================================
// Device-information sets and the devnode tree, both bitnesses. Expected values
// are from tools/oracle_probe/s7_setupapi_probe.expected and
// s7_cfgmgr_probe.expected (measured non-elevated on Windows 11 24H2).
// =============================================================================

namespace {

constexpr std::array<u8, 16> kBogusGuid{0x3e, 0x5d, 0x1c, 0x7a, 0x11, 0x11, 0x22, 0x42,
                                        0x93, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
constexpr std::array<u8, 16> kDisplayClass{0x68, 0xE9, 0x36, 0x4D, 0x25, 0xE3, 0xCE, 0x11,
                                           0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18};
constexpr std::array<u8, 16> kDisplayAdapterInterface{0x1D, 0x20, 0x45, 0x5B, 0xF2, 0xF2,
                                                      0x3B, 0x4F, 0x85, 0xBB, 0x30, 0xFF,
                                                      0x1F, 0x95, 0x35, 0x99};
constexpr char kAdapterPath[] =
    "\\\\?\\root#vitreo_metal#0000#{5b45201d-f2f2-4f3b-85bb-30ff1f953599}";

} // namespace

class SetupDiContractTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        display_device_identity::set_adapter_identity({});
        ASSERT_TRUE(memory.map_region(0x10000, 0x40000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(0x80000, 0x2000, MemoryProtection::ReadWrite).is_ok());
        if (x64()) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = 0x80800;
        marshaler.attach_cpu(cpu);
        setupapi = create_setupapi_thunks([] { return std::string{"Test Apple GPU"}; });
        cfgmgr = create_cfgmgr32_thunks([] { return std::string{"Test Apple GPU"}; });
    }

    [[nodiscard]] bool x64() const { return GetParam(); }
    [[nodiscard]] u32 devinfo_size() const { return x64() ? 32u : 28u; }
    [[nodiscard]] u64 invalid() const { return x64() ? ~0ull : 0xFFFFFFFFull; }

    u64 call(DllExports& dll, const char* name, std::initializer_list<u64> args) {
        usize i = 0;
        for (const u64 value : args) {
            if (x64()) {
                switch (i) {
                    case 0: cpu.gpr.rcx = value; break;
                    case 1: cpu.gpr.rdx = value; break;
                    case 2: cpu.gpr.r8 = value; break;
                    case 3: cpu.gpr.r9 = value; break;
                    default:
                        EXPECT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28 + (i - 4) * 8, value).is_ok());
                }
            } else {
                EXPECT_TRUE(memory.write_u32(cpu.gpr.rsp + 4 + i * 4, static_cast<u32>(value)).is_ok());
            }
            ++i;
        }
        for (auto& entry : dll.entries) {
            if (entry.name == name) {
                entry.handler(marshaler);
                return x64() ? cpu.gpr.rax : (cpu.gpr.rax & 0xFFFFFFFFull);
            }
        }
        ADD_FAILURE() << "missing export " << name;
        return 0;
    }
    u64 sdi(const char* name, std::initializer_list<u64> args) { return call(setupapi, name, args); }
    u64 cm(const char* name, std::initializer_list<u64> args) { return call(cfgmgr, name, args); }

    GuestAddr alloc(usize size) {
        const GuestAddr at = next;
        next += (size + 15) & ~usize{15};
        return at;
    }
    GuestAddr guid(const std::array<u8, 16>& g) {
        const GuestAddr at = alloc(16);
        EXPECT_TRUE(memory.write_bytes(at, g.data(), g.size()).is_ok());
        return at;
    }
    GuestAddr wide(std::string_view text) {
        const GuestAddr at = alloc((text.size() + 1) * 2);
        for (usize i = 0; i <= text.size(); ++i) {
            EXPECT_TRUE(memory.write_u16(at + i * 2, i < text.size() ? static_cast<u8>(text[i]) : 0).is_ok());
        }
        return at;
    }
    GuestAddr sized(u32 cb, usize bytes = 64) {
        const GuestAddr at = alloc(bytes);
        EXPECT_TRUE(memory.write_u32(at, cb).is_ok());
        return at;
    }
    std::string read_wide(GuestAddr at) {
        std::string text;
        for (;; at += 2) {
            const u16 unit = memory.read_u16(at).value();
            if (unit == 0) break;
            text.push_back(static_cast<char>(unit));
        }
        return text;
    }
    u32 u32_at(GuestAddr at) { return memory.read_u32(at).value(); }

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    DllExports setupapi;
    DllExports cfgmgr;
    GuestAddr next{0x10000};
};

INSTANTIATE_TEST_SUITE_P(Bitness, SetupDiContractTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

TEST_P(SetupDiContractTest, EmptyInterfaceSetEndsWithNoMoreItems) {
    // gcd_bogus_iface: a valid empty set; the index is checked before the
    // output, so even a mis-sized or NULL output reports ERROR_NO_MORE_ITEMS.
    const GuestAddr bogus = guid(kBogusGuid);
    const u64 set = sdi("SetupDiGetClassDevsW", {bogus, 0, 0, 0x12});
    ASSERT_NE(set, invalid());
    EXPECT_EQ(get_last_error(), 0u);
    EXPECT_EQ(sdi("SetupDiEnumDeviceInterfaces", {set, 0, bogus, 0, sized(devinfo_size())}), 0u);
    EXPECT_EQ(get_last_error(), 259u);
    EXPECT_EQ(sdi("SetupDiEnumDeviceInterfaces", {set, 0, bogus, 0, sized(3)}), 0u);
    EXPECT_EQ(get_last_error(), 259u);
    EXPECT_EQ(sdi("SetupDiEnumDeviceInterfaces", {set, 0, bogus, 0, 0}), 0u);
    EXPECT_EQ(get_last_error(), 259u);
    EXPECT_EQ(sdi("SetupDiEnumDeviceInfo", {set, 0, sized(devinfo_size())}), 0u);
    EXPECT_EQ(get_last_error(), 259u);
    EXPECT_EQ(sdi("SetupDiDestroyDeviceInfoList", {set}), 1u);
    EXPECT_EQ(sdi("SetupDiDestroyDeviceInfoList", {set}), 0u);
    EXPECT_EQ(get_last_error(), 6u);
    EXPECT_EQ(sdi("SetupDiEnumDeviceInfo", {0x1234, 0, sized(devinfo_size())}), 0u);
    EXPECT_EQ(get_last_error(), 6u);
}

TEST_P(SetupDiContractTest, GetClassDevsValidatesClassAndEnumerator) {
    // gcd_null_present, gcd_empty_enumerator, gcd_iface_enumname, gcd_badflags.
    EXPECT_EQ(sdi("SetupDiGetClassDevsW", {0, 0, 0, 0x2}), invalid());
    EXPECT_EQ(get_last_error(), 87u);
    const GuestAddr display = guid(kDisplayClass);
    EXPECT_EQ(sdi("SetupDiGetClassDevsW", {display, wide(""), 0, 0x2}), invalid());
    EXPECT_EQ(get_last_error(), 13u);
    const GuestAddr adapter = guid(kDisplayAdapterInterface);
    EXPECT_EQ(sdi("SetupDiGetClassDevsW", {adapter, wide("ROOT"), 0, 0x10}), invalid());
    EXPECT_EQ(get_last_error(), 13u);
    EXPECT_NE(sdi("SetupDiGetClassDevsW", {guid(kBogusGuid), 0, 0, 0x80000000u}), invalid());
    // A setup-class set carries no interfaces (disp_class enumif).
    const u64 set = sdi("SetupDiGetClassDevsW", {display, 0, 0, 0x2});
    ASSERT_NE(set, invalid());
    EXPECT_EQ(sdi("SetupDiEnumDeviceInfo", {set, 0, sized(devinfo_size())}), 1u);
    EXPECT_EQ(sdi("SetupDiEnumDeviceInterfaces", {set, 0, adapter, 0, sized(devinfo_size())}), 0u);
    EXPECT_EQ(get_last_error(), 259u);
    // gcd_all_has_root: an all-classes set includes the root devnode.
    const u64 all = sdi("SetupDiGetClassDevsW", {0, 0, 0, 0x4});
    bool has_root = false;
    for (u32 i = 0;; ++i) {
        const GuestAddr did = sized(devinfo_size());
        if (sdi("SetupDiEnumDeviceInfo", {all, i, did}) == 0) break;
        const GuestAddr id = alloc(128);
        ASSERT_EQ(sdi("SetupDiGetDeviceInstanceIdW", {all, did, id, 64, 0}), 1u);
        has_root |= read_wide(id) == "HTREE\\ROOT\\0";
    }
    EXPECT_TRUE(has_root);
}

TEST_P(SetupDiContractTest, InterfaceDetailFollowsMeasuredRules) {
    const GuestAddr adapter = guid(kDisplayAdapterInterface);
    const u64 set = sdi("SetupDiGetClassDevsW", {adapter, 0, 0, 0x12});
    const GuestAddr iface = sized(devinfo_size());
    ASSERT_EQ(sdi("SetupDiEnumDeviceInterfaces", {set, 0, adapter, 0, iface}), 1u);
    EXPECT_EQ(u32_at(iface + 20), 1u);  // SPINT_ACTIVE (disp_iface flags=1)

    // detw_query: 122, RequiredSize = offsetof(DevicePath) + (len+1)*2, and
    // the DeviceInfoData is filled even without a detail buffer.
    const GuestAddr required = alloc(4);
    const GuestAddr did = sized(devinfo_size());
    EXPECT_EQ(sdi("SetupDiGetDeviceInterfaceDetailW", {set, iface, 0, 0, required, did}), 0u);
    EXPECT_EQ(get_last_error(), 122u);
    const u32 needed = 4 + static_cast<u32>(sizeof(kAdapterPath)) * 2;
    EXPECT_EQ(u32_at(required), needed);
    std::array<u8, 16> cls{};
    ASSERT_TRUE(memory.read_bytes(did + 4, cls.data(), 16).is_ok());
    EXPECT_EQ(cls, kDisplayClass);
    EXPECT_NE(u32_at(did + 20), 0u);

    // detw_cb / detw_5 / detw_min6: a wrong cbSize or a buffer below
    // offsetof+sizeof(WCHAR) is 1784 with RequiredSize untouched; the
    // minimum buffer is 122.
    const u32 right_cb = x64() ? 8u : 6u;
    const GuestAddr detail = sized(x64() ? 6u : 8u, 256);
    ASSERT_TRUE(memory.write_u32(required, 0x5555).is_ok());
    EXPECT_EQ(sdi("SetupDiGetDeviceInterfaceDetailW", {set, iface, detail, 256, required, 0}), 0u);
    EXPECT_EQ(get_last_error(), 1784u);
    EXPECT_EQ(u32_at(required), 0x5555u);
    ASSERT_TRUE(memory.write_u32(detail, right_cb).is_ok());
    EXPECT_EQ(sdi("SetupDiGetDeviceInterfaceDetailW", {set, iface, detail, 5, required, 0}), 0u);
    EXPECT_EQ(get_last_error(), 1784u);
    EXPECT_EQ(sdi("SetupDiGetDeviceInterfaceDetailW", {set, iface, detail, 6, required, 0}), 0u);
    EXPECT_EQ(get_last_error(), 122u);
    EXPECT_EQ(sdi("SetupDiGetDeviceInterfaceDetailW", {set, iface, 0, 100, required, 0}), 0u);
    EXPECT_EQ(get_last_error(), 1784u);

    // detw_full: the lower-case path ending in the interface GUID.
    set_last_error(0xDEAD);
    EXPECT_EQ(sdi("SetupDiGetDeviceInterfaceDetailW", {set, iface, detail, needed, required, 0}), 1u);
    EXPECT_EQ(get_last_error(), 0u);
    EXPECT_EQ(read_wide(detail + 4), kAdapterPath);
    EXPECT_EQ(u32_at(detail), right_cb);

    // detw_dd_badcb: the path is written, then the mis-sized DeviceInfoData fails.
    const GuestAddr bad_did = sized(5);
    EXPECT_EQ(sdi("SetupDiGetDeviceInterfaceDetailW", {set, iface, detail, needed, 0, bad_did}), 0u);
    EXPECT_EQ(get_last_error(), 1784u);

    // deta_*: the ANSI detail cbSize is 5 on x86 and 8 on x64.
    const GuestAddr detail_a = sized(x64() ? 8u : 5u, 256);
    EXPECT_EQ(sdi("SetupDiGetDeviceInterfaceDetailA", {set, iface, detail_a, 256, required, 0}), 1u);
    EXPECT_EQ(u32_at(required), 4 + static_cast<u32>(sizeof(kAdapterPath)));

    // detw_if_badcb / detw_foreignset / detw_badset.
    const GuestAddr other = sdi("SetupDiCreateDeviceInfoList", {0, 0});
    EXPECT_EQ(sdi("SetupDiGetDeviceInterfaceDetailW", {other, iface, 0, 0, required, 0}), 0u);
    EXPECT_EQ(get_last_error(), 87u);
    EXPECT_EQ(sdi("SetupDiGetDeviceInterfaceDetailW", {0x1234, iface, 0, 0, required, 0}), 0u);
    EXPECT_EQ(get_last_error(), 6u);
    // Destroying one set leaves every other set intact.
    EXPECT_EQ(sdi("SetupDiDestroyDeviceInfoList", {other}), 1u);
    EXPECT_EQ(sdi("SetupDiEnumDeviceInterfaces", {set, 0, adapter, 0, iface}), 1u);
}

TEST_P(SetupDiContractTest, OpenDeviceInfoAndPropertiesOfTheRoot) {
    const u64 set = sdi("SetupDiCreateDeviceInfoList", {0, 0});
    ASSERT_NE(set, invalid());
    const GuestAddr did = sized(devinfo_size());
    // odi_null_is_root / odi_root.
    EXPECT_EQ(sdi("SetupDiOpenDeviceInfoW", {set, 0, 0, 0, did}), 1u);
    const GuestAddr id = alloc(64);
    const GuestAddr required = alloc(4);
    EXPECT_EQ(sdi("SetupDiGetDeviceInstanceIdW", {set, did, id, 32, required}), 1u);
    EXPECT_EQ(read_wide(id), "HTREE\\ROOT\\0");
    EXPECT_EQ(u32_at(required), 13u);
    EXPECT_EQ(sdi("SetupDiOpenDeviceInfoW", {set, wide("htree\\root\\0"), 0, 0, did}), 1u);
    EXPECT_EQ(sdi("SetupDiEnumDeviceInfo", {set, 1, sized(devinfo_size())}), 0u);  // not added twice

    // odi_badflags / odi_badformat / odi_bogus / odi_classmismatch.
    EXPECT_EQ(sdi("SetupDiOpenDeviceInfoW", {set, 0, 0, 0x100, did}), 0u);
    EXPECT_EQ(get_last_error(), 1004u);
    EXPECT_EQ(sdi("SetupDiOpenDeviceInfoW", {set, wide("A\\B"), 0, 0, did}), 0u);
    EXPECT_EQ(get_last_error(), 0xE0000205u);
    EXPECT_EQ(sdi("SetupDiOpenDeviceInfoW", {set, wide("ROOT\\NOPE\\0000"), 0, 0, did}), 0u);
    EXPECT_EQ(get_last_error(), 0xE000020Bu);
    const u64 classed = sdi("SetupDiCreateDeviceInfoList", {guid(kBogusGuid), 0});
    EXPECT_EQ(sdi("SetupDiOpenDeviceInfoW", {classed, 0, 0, 0, sized(devinfo_size())}), 0u);
    EXPECT_EQ(get_last_error(), 0xE0000201u);

    // gdp_instid*, gdp_class, gdp_flags, gdp_nullkey, gdp_root_status.
    const GuestAddr key = alloc(20);
    const std::array<u8, 16> instance_fmtid{0xC8, 0x4F, 0xC3, 0x78, 0x4A, 0x10, 0xCA, 0x4A,
                                            0x9E, 0xA4, 0x52, 0x4D, 0x52, 0x99, 0x6E, 0x57};
    ASSERT_TRUE(memory.write_bytes(key, instance_fmtid.data(), 16).is_ok());
    ASSERT_TRUE(memory.write_u32(key + 16, 256).is_ok());
    const GuestAddr type = alloc(4);
    const GuestAddr buffer = alloc(128);
    EXPECT_EQ(sdi("SetupDiGetDevicePropertyW", {set, did, key, type, 0, 0, required, 0}), 0u);
    EXPECT_EQ(get_last_error(), 122u);
    EXPECT_EQ(u32_at(type), 0x12u);
    EXPECT_EQ(u32_at(required), 26u);
    EXPECT_EQ(sdi("SetupDiGetDevicePropertyW", {set, did, key, type, buffer, 128, required, 0}), 1u);
    EXPECT_EQ(read_wide(buffer), "HTREE\\ROOT\\0");
    EXPECT_EQ(sdi("SetupDiGetDevicePropertyW", {set, did, key, type, buffer, 128, required, 1}), 0u);
    EXPECT_EQ(get_last_error(), 1004u);
    EXPECT_EQ(sdi("SetupDiGetDevicePropertyW", {set, did, 0, type, buffer, 128, required, 0}), 0u);
    EXPECT_EQ(get_last_error(), 13u);
    EXPECT_EQ(sdi("SetupDiGetDevicePropertyW", {set, did, key, 0, buffer, 128, required, 0}), 0u);
    EXPECT_EQ(get_last_error(), 1784u);
    const std::array<u8, 16> device_fmtid{0x4E, 0x25, 0x5C, 0xA4, 0x1C, 0xDF, 0xFD, 0x4E,
                                          0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0};
    ASSERT_TRUE(memory.write_bytes(key, device_fmtid.data(), 16).is_ok());
    ASSERT_TRUE(memory.write_u32(key + 16, 9).is_ok());   // DEVPKEY_Device_Class
    ASSERT_TRUE(memory.write_u32(type, 0x5555).is_ok());
    EXPECT_EQ(sdi("SetupDiGetDevicePropertyW", {set, did, key, type, buffer, 128, required, 0}), 0u);
    EXPECT_EQ(get_last_error(), 1168u);
    EXPECT_EQ(u32_at(type), 0x5555u);
    const std::array<u8, 16> node_fmtid{0xC5, 0xA6, 0x40, 0x43, 0xFA, 0x93, 0x06, 0x47,
                                        0x97, 0x2C, 0x7B, 0x64, 0x80, 0x08, 0xA5, 0xA7};
    ASSERT_TRUE(memory.write_bytes(key, node_fmtid.data(), 16).is_ok());
    ASSERT_TRUE(memory.write_u32(key + 16, 2).is_ok());   // DEVPKEY_Device_DevNodeStatus
    EXPECT_EQ(sdi("SetupDiGetDevicePropertyW", {set, did, key, type, buffer, 4, required, 0}), 1u);
    EXPECT_EQ(u32_at(type), 7u);
    EXPECT_EQ(u32_at(buffer), 0x4180000Au);

    // gdrp_root_class / gdrp_badprop / gdrp_root_prop p=24.
    EXPECT_EQ(sdi("SetupDiGetDeviceRegistryPropertyW", {set, did, 7, type, buffer, 128, required}), 0u);
    EXPECT_EQ(get_last_error(), 13u);
    EXPECT_EQ(sdi("SetupDiGetDeviceRegistryPropertyW", {set, did, 0x25, type, buffer, 128, required}), 0u);
    EXPECT_EQ(get_last_error(), 0xE0000209u);
    EXPECT_EQ(sdi("SetupDiGetDeviceRegistryPropertyW", {set, did, 0x24, type, buffer, 128, required}), 1u);
    EXPECT_EQ(read_wide(buffer), "{00000000-0000-0000-FFFF-FFFFFFFFFFFF}");
}

TEST_P(SetupDiContractTest, OpenDeviceInterfaceByPath) {
    const u64 set = sdi("SetupDiCreateDeviceInfoList", {0, 0});
    const GuestAddr out = sized(devinfo_size());
    // openif_noadd_fresh, openif, openif_upper, openif_bogus, openif_badflags.
    EXPECT_EQ(sdi("SetupDiOpenDeviceInterfaceW", {set, wide(kAdapterPath), 1, out}), 0u);
    EXPECT_EQ(get_last_error(), 0xE0000225u);
    EXPECT_EQ(sdi("SetupDiOpenDeviceInterfaceW", {set, wide(kAdapterPath), 0, out}), 1u);
    std::string upper = kAdapterPath;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    EXPECT_EQ(sdi("SetupDiOpenDeviceInterfaceW", {set, wide(upper), 0, out}), 1u);
    const GuestAddr adapter = guid(kDisplayAdapterInterface);
    EXPECT_EQ(sdi("SetupDiEnumDeviceInterfaces", {set, 0, adapter, 1, sized(devinfo_size())}), 0u);
    EXPECT_EQ(get_last_error(), 259u);
    EXPECT_EQ(sdi("SetupDiOpenDeviceInterfaceW",
                  {set, wide("\\\\?\\root#nope#0000#{5b45201d-f2f2-4f3b-85bb-30ff1f953599}"), 0, out}),
              0u);
    EXPECT_EQ(get_last_error(), 0xE0000225u);
    EXPECT_EQ(sdi("SetupDiOpenDeviceInterfaceW", {set, wide(kAdapterPath), 0x10, out}), 0u);
    EXPECT_EQ(get_last_error(), 1004u);
    EXPECT_EQ(sdi("SetupDiOpenDeviceInterfaceW", {set, 0, 0, out}), 0u);
    EXPECT_EQ(get_last_error(), 87u);
}

TEST_P(SetupDiContractTest, InstallExportsAreRegisteredWithImportLibraryBytes) {
    // setupapi.lib decorations (_Name@N); SetupDiGetDevicePropertyA does not
    // exist in setupapi.dll.
    const std::pair<const char*, u16> expected[] = {
        {"SetupCopyOEMInfW", 32}, {"SetupDiCallClassInstaller", 12},
        {"SetupDiCreateDeviceInfoW", 28}, {"SetupDiDestroyDriverInfoList", 12},
        {"SetupDiGetDriverInfoDetailW", 24}, {"SetupDiOpenClassRegKey", 8},
        {"SetupDiSetClassInstallParamsW", 16}, {"SetupDiSetDeviceRegistryPropertyW", 20},
        {"SetupGetInfDriverStoreLocationW", 24}, {"SetupDiCreateDeviceInfoListExW", 16},
        {"SetupDiGetClassInstallParamsW", 20}, {"SetupDiGetDeviceInterfaceDetailW", 24},
        {"SetupDiGetDevicePropertyW", 32}, {"SetupDiCreateDeviceInfoList", 8},
        {"SetupDiOpenDeviceInfoW", 20}, {"SetupDiOpenDeviceInterfaceW", 16},
    };
    for (const auto& [name, bytes] : expected) {
        const auto it = std::find_if(setupapi.entries.begin(), setupapi.entries.end(),
                                     [name](const auto& e) { return e.name == name; });
        ASSERT_NE(it, setupapi.entries.end()) << name;
        EXPECT_EQ(it->stdcall_bytes, bytes) << name;
    }
    for (const char* absent : {"SetupDiGetDevicePropertyA", "CM_Get_DevNode_PropertyW",
                               "CM_MapCrToWin32Err", "CM_Register_Notification"}) {
        EXPECT_TRUE(std::none_of(setupapi.entries.begin(), setupapi.entries.end(),
                                 [absent](const auto& e) { return e.name == absent; }))
            << absent;
    }
}

TEST_P(SetupDiContractTest, NonElevatedInstallCallsFailLikeANonAdministrator) {
    const GuestAddr system_class = guid({0x7d, 0xe9, 0x36, 0x4d, 0x25, 0xe3, 0xce, 0x11,
                                         0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18});
    const u64 set = sdi("SetupDiCreateDeviceInfoList", {system_class, 0});
    const GuestAddr did = sized(devinfo_size());
    // cdi_badflags, cdi_nullname, cdi_gen, cdi_badset.
    EXPECT_EQ(sdi("SetupDiCreateDeviceInfoW", {set, wide("VitreoProbeDev"), system_class, 0, 0, 0x8, did}), 0u);
    EXPECT_EQ(get_last_error(), 1004u);
    EXPECT_EQ(sdi("SetupDiCreateDeviceInfoW", {set, 0, system_class, 0, 0, 1, did}), 0u);
    EXPECT_EQ(get_last_error(), 0xE0000205u);
    EXPECT_EQ(sdi("SetupDiCreateDeviceInfoW", {set, wide("VitreoProbeDev"), system_class, 0, 0, 1, did}), 0u);
    EXPECT_EQ(get_last_error(), 5u);
    EXPECT_EQ(sdi("SetupDiCreateDeviceInfoW", {0x1234, wide("X"), system_class, 0, 0, 1, did}), 0u);
    EXPECT_EQ(get_last_error(), 6u);

    // sdrp_*: validation, then ERROR_ACCESS_DENIED.
    const u64 roots = sdi("SetupDiCreateDeviceInfoList", {0, 0});
    const GuestAddr root = sized(devinfo_size());
    ASSERT_EQ(sdi("SetupDiOpenDeviceInfoW", {roots, 0, 0, 0, root}), 1u);
    const GuestAddr value = wide("ROOT\\VitreoProbe");
    EXPECT_EQ(sdi("SetupDiSetDeviceRegistryPropertyW", {roots, root, 0x100, value, 34}), 0u);
    EXPECT_EQ(get_last_error(), 0xE0000209u);
    EXPECT_EQ(sdi("SetupDiSetDeviceRegistryPropertyW", {roots, root, 0x0C, 0, 8}), 0u);
    EXPECT_EQ(get_last_error(), 1784u);
    EXPECT_EQ(sdi("SetupDiSetDeviceRegistryPropertyW", {roots, root, 0x0C, value, 34}), 0u);
    EXPECT_EQ(get_last_error(), 5u);
    EXPECT_EQ(sdi("SetupDiSetDeviceRegistryPropertyW", {roots, 0, 0x0C, value, 34}), 0u);
    EXPECT_EQ(get_last_error(), 87u);

    // cci_*: ERROR_IN_WOW64 for every x86 call; x64 per DIF.
    if (!x64()) {
        EXPECT_EQ(sdi("SetupDiCallClassInstaller", {0x19, roots, root}), 0u);
        EXPECT_EQ(get_last_error(), 0xE0000235u);
        EXPECT_EQ(sdi("SetupDiCallClassInstaller", {0x19, 0x1234, 0}), 0u);
        EXPECT_EQ(get_last_error(), 0xE0000235u);
    } else {
        EXPECT_EQ(sdi("SetupDiCallClassInstaller", {0x19, roots, root}), 1u);
        EXPECT_EQ(sdi("SetupDiCallClassInstaller", {0x19, roots, 0}), 0u);
        EXPECT_EQ(get_last_error(), 87u);
        EXPECT_EQ(sdi("SetupDiCallClassInstaller", {0x02, set, 0}), 0u);
        EXPECT_EQ(get_last_error(), 87u);
        EXPECT_EQ(sdi("SetupDiCallClassInstaller", {0x9999, set, 0}), 0u);
        EXPECT_EQ(get_last_error(), 0xE000020Eu);
        EXPECT_EQ(sdi("SetupDiCallClassInstaller", {0x17, roots, root}), 0u);
        EXPECT_EQ(get_last_error(), 0xE0000228u);
        EXPECT_EQ(sdi("SetupDiCallClassInstaller", {0x19, 0x1234, 0}), 0u);
        EXPECT_EQ(get_last_error(), 6u);
    }

    // coi_null / coi_badmedia / coi_missing (no file system attached here).
    EXPECT_EQ(sdi("SetupCopyOEMInfW", {0, 0, 0, 0, 0, 0, 0, 0}), 0u);
    EXPECT_EQ(get_last_error(), 87u);
    EXPECT_EQ(sdi("SetupCopyOEMInfW", {wide("C:\\x.inf"), 0, 7, 0, 0, 0, 0, 0}), 0u);
    EXPECT_EQ(get_last_error(), 87u);
    EXPECT_EQ(sdi("SetupCopyOEMInfW", {wide("C:\\nope\\nope.inf"), 0, 0, 0, 0, 0, 0, 0}), 0u);
    EXPECT_EQ(get_last_error(), 2u);

    // sgidsl_null / sgidsl_missing / sgidsl_oem.
    EXPECT_EQ(sdi("SetupGetInfDriverStoreLocationW", {0, 0, 0, alloc(8), 4, alloc(4)}), 0u);
    EXPECT_EQ(get_last_error(), 87u);
    EXPECT_EQ(sdi("SetupGetInfDriverStoreLocationW", {wide("C:\\nope\\nope.inf"), 0, 0, alloc(8), 4, alloc(4)}), 0u);
    EXPECT_EQ(get_last_error(), 1168u);
    EXPECT_EQ(sdi("SetupGetInfDriverStoreLocationW", {wide("oem9999.inf"), 0, 0, alloc(8), 4, alloc(4)}), 0u);
    EXPECT_EQ(get_last_error(), 2u);

    // ocrk_bogus / ocrk_guidnull: not installed, pointer-width INVALID_HANDLE_VALUE.
    EXPECT_EQ(sdi("SetupDiOpenClassRegKey", {guid(kBogusGuid), 0x20019}), invalid());
    EXPECT_EQ(get_last_error(), 0xE0000206u);
    EXPECT_EQ(sdi("SetupDiOpenClassRegKey", {guid({}), 0x20019}), invalid());
    EXPECT_EQ(get_last_error(), 0xE0000206u);
}

TEST_P(SetupDiContractTest, ClassInstallParamsAndDriverLists) {
    const u64 set = sdi("SetupDiCreateDeviceInfoList", {0, 0});
    // SP_PROPCHANGE_PARAMS {header{8, DIF_PROPERTYCHANGE}, StateChange, Scope, HwProfile}.
    const GuestAddr params = alloc(64);
    auto propchange = [&](u32 change, u32 scope) {
        EXPECT_TRUE(memory.write_u32(params, 8).is_ok());
        EXPECT_TRUE(memory.write_u32(params + 4, 0x12).is_ok());
        EXPECT_TRUE(memory.write_u32(params + 8, change).is_ok());
        EXPECT_TRUE(memory.write_u32(params + 12, scope).is_ok());
        EXPECT_TRUE(memory.write_u32(params + 16, 0).is_ok());
    };
    // scip_set / scip_content / scip_short / scip_badcb / scip_null_size.
    propchange(1, 1);
    EXPECT_EQ(sdi("SetupDiSetClassInstallParamsW", {set, 0, params, 20}), 1u);
    const u32 install_size = x64() ? 584u : 556u;
    const GuestAddr install = sized(install_size, install_size);
    ASSERT_EQ(sdi("SetupDiGetDeviceInstallParamsW", {set, 0, install}), 1u);
    EXPECT_NE(u32_at(install + 4) & 0x00100000u, 0u);   // DI_CLASSINSTALLPARAMS
    const GuestAddr got = alloc(64);
    const GuestAddr required = alloc(4);
    ASSERT_TRUE(memory.write_u32(got, 8).is_ok());
    EXPECT_EQ(sdi("SetupDiGetClassInstallParamsW", {set, 0, got, 64, required}), 1u);
    EXPECT_EQ(u32_at(required), 20u);
    EXPECT_EQ(u32_at(got + 4), 0x12u);
    EXPECT_EQ(sdi("SetupDiSetClassInstallParamsW", {set, 0, 0, 0}), 1u);
    ASSERT_EQ(sdi("SetupDiGetDeviceInstallParamsW", {set, 0, install}), 1u);
    EXPECT_EQ(u32_at(install + 4) & 0x00100000u, 0u);
    EXPECT_EQ(sdi("SetupDiGetClassInstallParamsW", {set, 0, got, 64, required}), 0u);
    EXPECT_EQ(get_last_error(), 0xE0000215u);
    EXPECT_EQ(sdi("SetupDiSetClassInstallParamsW", {set, 0, params, 8}), 0u);
    EXPECT_EQ(get_last_error(), 87u);
    propchange(4, 1);   // DICS_START needs DICS_FLAG_CONFIGSPECIFIC
    EXPECT_EQ(sdi("SetupDiSetClassInstallParamsW", {set, 0, params, 20}), 0u);
    EXPECT_EQ(get_last_error(), 87u);
    propchange(4, 2);
    EXPECT_EQ(sdi("SetupDiSetClassInstallParamsW", {set, 0, params, 20}), 1u);
    ASSERT_TRUE(memory.write_u32(params, 4).is_ok());
    EXPECT_EQ(sdi("SetupDiSetClassInstallParamsW", {set, 0, params, 20}), 0u);
    EXPECT_EQ(get_last_error(), 1784u);
    EXPECT_EQ(sdi("SetupDiSetClassInstallParamsW", {set, 0, 0, 8}), 0u);
    EXPECT_EQ(get_last_error(), 1784u);
    // scip_fn: DIF_INSTALLDEVICE takes no block; an unknown code takes any.
    ASSERT_TRUE(memory.write_u32(params, 8).is_ok());
    ASSERT_TRUE(memory.write_u32(params + 4, 0x02).is_ok());
    EXPECT_EQ(sdi("SetupDiSetClassInstallParamsW", {set, 0, params, 8}), 0u);
    EXPECT_EQ(get_last_error(), 87u);
    ASSERT_TRUE(memory.write_u32(params + 4, 0x2B).is_ok());
    EXPECT_EQ(sdi("SetupDiSetClassInstallParamsW", {set, 0, params, 13}), 1u);
    // DIF_DETECT: SP_DETECTDEVICE_PARAMS is 24/16 bytes with a callback.
    ASSERT_TRUE(memory.write_u32(params + 4, 0x0F).is_ok());
    ASSERT_TRUE(memory.write_u64(params + 8, 0x401000).is_ok());
    EXPECT_EQ(sdi("SetupDiSetClassInstallParamsW", {set, 0, params, x64() ? 24u : 16u}), 1u);

    // ddil_class_unbuilt / ddil_compat_nulldid / ddil_badtype / ddil_badset.
    EXPECT_EQ(sdi("SetupDiDestroyDriverInfoList", {set, 0, 1}), 1u);
    EXPECT_EQ(sdi("SetupDiDestroyDriverInfoList", {set, 0, 2}), 0u);
    EXPECT_EQ(get_last_error(), 87u);
    EXPECT_EQ(sdi("SetupDiDestroyDriverInfoList", {set, 0, 7}), 0u);
    EXPECT_EQ(get_last_error(), 87u);
    EXPECT_EQ(sdi("SetupDiDestroyDriverInfoList", {0x1234, 0, 1}), 0u);
    EXPECT_EQ(get_last_error(), 6u);

    // bdil_disp: the display's compatible driver and its detail; the detail
    // buffer rules (gdid_*): unknown driver 87, cbSize != sizeof 1784.
    const GuestAddr display = guid(kDisplayClass);
    const u64 dset = sdi("SetupDiGetClassDevsW", {display, 0, 0, 0x2});
    const GuestAddr did = sized(devinfo_size());
    ASSERT_EQ(sdi("SetupDiEnumDeviceInfo", {dset, 0, did}), 1u);
    ASSERT_EQ(sdi("SetupDiBuildDriverInfoList", {dset, did, 2}), 1u);
    const u32 drv_size = x64() ? 1568u : 1564u;
    const GuestAddr drv = sized(drv_size, drv_size);
    ASSERT_EQ(sdi("SetupDiEnumDriverInfoW", {dset, did, 2, 0, drv}), 1u);
    EXPECT_EQ(sdi("SetupDiGetDriverInfoDetailW", {dset, did, drv, 0, 0, required}), 0u);
    EXPECT_EQ(get_last_error(), 122u);
    const u32 detail_size = x64() ? 1584u : 1570u;
    EXPECT_GE(u32_at(required), detail_size);
    const GuestAddr detail = sized(detail_size - 2, 2048);
    EXPECT_EQ(sdi("SetupDiGetDriverInfoDetailW", {dset, did, drv, detail, 2048, required}), 0u);
    EXPECT_EQ(get_last_error(), 1784u);
    ASSERT_TRUE(memory.write_u32(detail, detail_size).is_ok());
    EXPECT_EQ(sdi("SetupDiGetDriverInfoDetailW", {dset, did, drv, detail, 2048, required}), 1u);
    const GuestAddr stranger = sized(drv_size, drv_size);
    EXPECT_EQ(sdi("SetupDiGetDriverInfoDetailW", {dset, did, stranger, 0, 0, required}), 0u);
    EXPECT_EQ(get_last_error(), 87u);
}

TEST_P(SetupDiContractTest, CfgmgrDevnodeTreeFollowsMeasuredRules) {
    const GuestAddr out = alloc(8);
    // locate_null / getid_root / getidsize_root / status_root.
    ASSERT_EQ(cm("CM_Locate_DevNodeW", {out, 0, 0}), 0u);
    const u32 root = u32_at(out);
    const GuestAddr buffer = alloc(512);
    EXPECT_EQ(cm("CM_Get_Device_IDW", {root, buffer, 256, 0}), 0u);
    EXPECT_EQ(read_wide(buffer), "HTREE\\ROOT\\0");
    // getid_root_exact: no terminator when exactly the length; small: partial.
    ASSERT_TRUE(memory.write_u16(buffer + 24, 0x5A5A).is_ok());
    EXPECT_EQ(cm("CM_Get_Device_IDW", {root, buffer, 12, 0}), 0u);
    EXPECT_EQ(memory.read_u16(buffer + 24).value(), 0x5A5Au);
    EXPECT_EQ(cm("CM_Get_Device_IDW", {root, buffer, 11, 0}), 0x1Au);
    EXPECT_EQ(cm("CM_Get_Device_IDW", {root, buffer, 0, 0}), 0x03u);
    EXPECT_EQ(cm("CM_Get_Device_IDW", {0x7fff1234, buffer, 256, 0}), 0x05u);
    const GuestAddr status = alloc(4);
    const GuestAddr problem = alloc(4);
    EXPECT_EQ(cm("CM_Get_DevNode_Status", {status, problem, root, 0}), 0u);
    EXPECT_EQ(u32_at(status), 0x4180000Au);
    EXPECT_EQ(u32_at(problem), 0u);

    // locate_missing / _noval / _onepart / _badflag / _nullout.
    EXPECT_EQ(cm("CM_Locate_DevNodeW", {out, wide("ROOT\\NOPE\\0000"), 0}), 0x0Du);
    EXPECT_EQ(u32_at(out), 0u);
    EXPECT_EQ(cm("CM_Locate_DevNodeW", {out, wide("ROOT\\NOPE\\0000"), 4}), 0u);
    EXPECT_NE(u32_at(out), 0u);
    EXPECT_EQ(cm("CM_Locate_DevNodeW", {out, wide("A\\B"), 0}), 0x1Eu);
    ASSERT_TRUE(memory.write_u32(out, 0x5555).is_ok());
    EXPECT_EQ(cm("CM_Locate_DevNodeW", {out, 0, 0x80}), 0x04u);
    EXPECT_EQ(u32_at(out), 0x5555u);
    EXPECT_EQ(cm("CM_Locate_DevNodeW", {0, 0, 0}), 0x03u);

    // parent_root / sibling_root / child_root and the child's round trip.
    EXPECT_EQ(cm("CM_Get_Parent", {out, root, 0}), 0x0Du);
    EXPECT_EQ(u32_at(out), 0u);
    EXPECT_EQ(cm("CM_Get_Sibling", {out, root, 0}), 0x0Du);
    ASSERT_EQ(cm("CM_Get_Child", {out, root, 0}), 0u);
    const u32 child = u32_at(out);
    EXPECT_EQ(cm("CM_Get_Parent", {out, child, 0}), 0u);
    EXPECT_EQ(u32_at(out), root);
    EXPECT_EQ(cm("CM_Get_Device_IDW", {child, buffer, 256, 0}), 0u);
    EXPECT_EQ(cm("CM_Locate_DevNodeW", {out, buffer, 0}), 0u);
    EXPECT_EQ(u32_at(out), child);
    ASSERT_TRUE(memory.write_u32(out, 0x5555).is_ok());
    EXPECT_EQ(cm("CM_Get_Child", {out, 0, 0}), 0x05u);
    EXPECT_EQ(u32_at(out), 0x5555u);

    // idl_*: the HTREE enumerator lists only the root; unknown is empty.
    const GuestAddr len = alloc(4);
    EXPECT_EQ(cm("CM_Get_Device_ID_List_SizeW", {len, wide("HTREE"), 1}), 0u);
    EXPECT_EQ(u32_at(len), 14u);
    EXPECT_EQ(cm("CM_Get_Device_ID_ListW", {wide("HTREE"), buffer, 14, 1}), 0u);
    EXPECT_EQ(read_wide(buffer), "HTREE\\ROOT\\0");
    EXPECT_EQ(memory.read_u16(buffer + 26).value(), 0u);
    ASSERT_TRUE(memory.write_u16(buffer, 0x5A5A).is_ok());
    EXPECT_EQ(cm("CM_Get_Device_ID_ListW", {wide("HTREE"), buffer, 5, 1}), 0x1Au);
    EXPECT_EQ(memory.read_u16(buffer).value(), 0u);
    EXPECT_EQ(cm("CM_Get_Device_ID_List_SizeW", {len, wide("NOPEENUM"), 1}), 0u);
    EXPECT_EQ(u32_at(len), 1u);
    EXPECT_EQ(cm("CM_Get_Device_ID_List_SizeW", {len, wide("HTREE\\ROOT\\0"), 1}), 0x25u);
    EXPECT_EQ(u32_at(len), 0u);
    EXPECT_EQ(cm("CM_Get_Device_ID_List_SizeW", {len, wide("notaguid"), 0x200}), 0x1Fu);
    EXPECT_EQ(cm("CM_Get_Device_ID_List_SizeW", {len, 0, 1}), 0x03u);
    EXPECT_EQ(cm("CM_Get_Device_ID_List_SizeW", {len, 0, 0x80000000u}), 0x04u);

    // drp_root_class / drp_zero / dnp_nullkey / odk_root_hw / odk_baddisp.
    const GuestAddr type = alloc(4);
    ASSERT_TRUE(memory.write_u32(len, 512).is_ok());
    EXPECT_EQ(cm("CM_Get_DevNode_Registry_PropertyW", {root, 8, type, buffer, len, 0}), 0x25u);
    EXPECT_EQ(u32_at(len), 0u);
    ASSERT_TRUE(memory.write_u32(len, 512).is_ok());
    EXPECT_EQ(cm("CM_Get_DevNode_Registry_PropertyW", {root, 0, type, buffer, len, 0}), 0x35u);
    EXPECT_EQ(u32_at(len), 512u);
    EXPECT_EQ(cm("CM_Get_DevNode_PropertyW", {root, 0, type, buffer, len, 0}), 0x13u);
    const GuestAddr hkey = alloc(8);
    EXPECT_EQ(cm("CM_Open_DevNode_Key", {root, 0x20019, 0, 1, hkey, 0}), 0x2Eu);
    EXPECT_EQ(cm("CM_Open_DevNode_Key", {root, 0x20019, 0, 7, hkey, 0}), 0x1Fu);
    EXPECT_EQ(cm("CM_Open_DevNode_Key", {root, 0x20019, 0, 1, hkey, 0x80}), 0x04u);
}

// =============================================================================
// hid.dll / winusb.dll / bthprops.cpl (winapi/system/hid/hid.hpp). Expected
// values: tools/oracle_probe/hid_probe.expected, s7_winusb_probe.expected,
// s7_bluetooth_probe.expected, and the import-library decorations.
// =============================================================================

namespace {

class FakeBluetoothHost final : public hardware::BluetoothHost {
public:
    explicit FakeBluetoothHost(bool present) : m_present(present) {}
    bool query_default_adapter(hardware::BluetoothAdapterState& out) override {
        out = {};
        out.present = m_present;
        out.powered = m_present;
        return true;
    }

private:
    bool m_present;
};

[[nodiscard]] const ExportEntry* find_export(const DllExports& dll, const char* name) {
    for (const auto& entry : dll.entries) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

// The s7_bluetooth_probe SDP record (attributes 0x0000..0x0210).
const std::vector<u8> kSdpRecord = {
    0x35, 0xC2,
    0x09, 0x00, 0x00, 0x0A, 0x00, 0x01, 0x00, 0x00,
    0x09, 0x00, 0x01, 0x35, 0x03, 0x19, 0x11, 0x01,
    0x09, 0x00, 0x06, 0x35, 0x09, 0x09, 0x65, 0x6E, 0x09, 0x00, 0x6A, 0x09, 0x01, 0x00,
    0x09, 0x01, 0x00, 0x25, 0x03, 'C', 'O', 'M',
    0x09, 0x01, 0x01, 0x08, 0x05,
    0x09, 0x02, 0x00, 0x10, 0xFB,
    0x09, 0x02, 0x01, 0x28, 0x01,
    0x09, 0x02, 0x02, 0x45, 0x03, 'h', 't', 'p',
    0x09, 0x02, 0x03, 0x1A, 0x12, 0x34, 0x56, 0x78,
    0x09, 0x02, 0x04, 0x1C, 0x00, 0x00, 0x11, 0x01, 0x00, 0x00, 0x10, 0x00,
          0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB,
    0x09, 0x02, 0x05, 0x0B, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x02, 0x06, 0x0C, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
          0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    0x09, 0x02, 0x07, 0x00,
    0x09, 0x02, 0x08, 0x3D, 0x04, 0x08, 0x01, 0x08, 0x02,
    0x09, 0x02, 0x09, 0x36, 0x00, 0x02, 0x08, 0x07,
    0x09, 0x02, 0x0A, 0x26, 0x00, 0x02, 'h', 'i',
    0x09, 0x02, 0x0B, 0x11, 0x80, 0x00,
    0x09, 0x02, 0x0C, 0x12, 0x12, 0x34, 0x56, 0x78,
    0x09, 0x02, 0x0D, 0x13, 0x81, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x02, 0x0E, 0x11, 0xAB, 0xCD,
    0x09, 0x02, 0x0F, 0x09, 0xBE, 0xEF,
    0x09, 0x02, 0x10, 0x11, 0x12, 0x34,
};

} // namespace

TEST_P(SetupDiContractTest, HidAndWinUsbImportLibraryByteCounts) {
    // libhid.a / libwinusb.a (i686) decorations: USHORT and UCHAR arguments
    // still take 4-byte slots, and WINUSB_SETUP_PACKET is 8 bytes by value.
    const auto hid = create_hid_thunks();
    const std::pair<const char*, u16> hid_bytes[] = {
        {"HidP_GetScaledUsageValue", 32}, {"HidP_GetUsageValue", 32}, {"HidP_GetUsages", 32},
        {"HidP_GetUsagesEx", 28}, {"HidP_GetCaps", 8}, {"HidD_GetAttributes", 8},
        {"HidD_FreePreparsedData", 4}, {"HidP_SetUsages", 32}, {"HidP_GetData", 24},
    };
    for (const auto& [name, bytes] : hid_bytes) {
        const ExportEntry* entry = find_export(hid, name);
        ASSERT_NE(entry, nullptr) << name;
        EXPECT_EQ(entry->stdcall_bytes, bytes) << name;
    }
    const auto usb = create_winusb_thunks();
    const std::pair<const char*, u16> usb_bytes[] = {
        {"WinUsb_AbortPipe", 8}, {"WinUsb_AbortPipeAsync", 12}, {"WinUsb_ControlTransfer", 28},
        {"WinUsb_FlushPipe", 8}, {"WinUsb_Free", 4}, {"WinUsb_GetAdjustedFrameNumber", 12},
        {"WinUsb_GetAssociatedInterface", 12}, {"WinUsb_GetCurrentAlternateSetting", 8},
        {"WinUsb_GetCurrentFrameNumber", 12}, {"WinUsb_GetDescriptor", 28},
        {"WinUsb_GetOverlappedResult", 16}, {"WinUsb_GetPipePolicy", 20},
        {"WinUsb_GetPowerPolicy", 16}, {"WinUsb_Initialize", 8},
        {"WinUsb_ParseConfigurationDescriptor", 28}, {"WinUsb_ParseDescriptors", 16},
        {"WinUsb_QueryDeviceInformation", 16}, {"WinUsb_QueryInterfaceSettings", 12},
        {"WinUsb_QueryPipe", 16}, {"WinUsb_QueryPipeEx", 16}, {"WinUsb_ReadIsochPipe", 28},
        {"WinUsb_ReadIsochPipeAsap", 28}, {"WinUsb_ReadPipe", 24},
        {"WinUsb_RegisterIsochBuffer", 20}, {"WinUsb_ResetPipe", 8}, {"WinUsb_ResetPipeAsync", 12},
        {"WinUsb_SetCurrentAlternateSetting", 8}, {"WinUsb_SetCurrentAlternateSettingAsync", 12},
        {"WinUsb_SetPipePolicy", 20}, {"WinUsb_SetPowerPolicy", 16},
        {"WinUsb_UnregisterIsochBuffer", 4}, {"WinUsb_WriteIsochPipe", 20},
        {"WinUsb_WriteIsochPipeAsap", 20}, {"WinUsb_WritePipe", 24},
        {"WinUsb_StartTrackingForTimeSync", 8}, {"WinUsb_GetCurrentFrameNumberAndQpc", 8},
        {"WinUsb_StopTrackingForTimeSync", 8},
    };
    EXPECT_EQ(usb.entries.size(), std::size(usb_bytes));   // the whole winusb.dll export table
    for (const auto& [name, bytes] : usb_bytes) {
        const ExportEntry* entry = find_export(usb, name);
        ASSERT_NE(entry, nullptr) << name;
        EXPECT_EQ(entry->stdcall_bytes, bytes) << name;
    }
}

TEST_P(SetupDiContractTest, WinUsbHasNoInterfacesAndParsesDescriptors) {
    auto usb = create_winusb_thunks();
    // init_null / init_file: FALSE, ERROR_INVALID_HANDLE, *InterfaceHandle = NULL.
    const GuestAddr out = alloc(8);
    ASSERT_TRUE(memory.write_u64(out, 0x5555555555555555ull).is_ok());
    EXPECT_EQ(call(usb, "WinUsb_Initialize", {0x44, out}), 0u);
    EXPECT_EQ(get_last_error(), 6u);
    EXPECT_EQ(x64() ? memory.read_u64(out).value() : u32_at(out), 0u);
    // free_null: TRUE with the last error untouched.
    set_last_error(0xDEAD);
    EXPECT_EQ(call(usb, "WinUsb_Free", {0}), 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(call(usb, "WinUsb_ReadPipe", {0, 0x81, alloc(64), 64, alloc(4), 0}), 0u);
    EXPECT_EQ(get_last_error(), 6u);
    EXPECT_EQ(call(usb, "WinUsb_WriteIsochPipe", {0, 0, 8, alloc(4), 0}), 0u);
    EXPECT_EQ(get_last_error(), 87u);
    EXPECT_EQ(call(usb, "WinUsb_GetAdjustedFrameNumber", {alloc(4), 0, 0}), 0u);
    EXPECT_EQ(get_last_error(), 87u);

    // pcd_* / pd_*: the probe's configuration descriptor.
    const std::vector<u8> config = {
        9, 2, 50, 0, 2, 1, 0, 0x80, 50,
        9, 4, 0, 0, 1, 0xFF, 1, 2, 0,
        7, 5, 0x81, 3, 8, 0, 10,
        9, 4, 0, 1, 0, 0xFF, 1, 3, 0,
        9, 4, 1, 0, 1, 3, 0, 0, 0,
        9, 0x21, 0x11, 1, 0, 1, 0x22, 50, 0,
        7, 5, 0x82, 3, 8, 0, 10,
    };
    const GuestAddr c = alloc(config.size());
    ASSERT_TRUE(memory.write_bytes(c, config.data(), config.size()).is_ok());
    const u64 minus1 = 0xFFFFFFFFu;
    auto pcd = [&](GuestAddr start, u64 ifn, u64 alt, u64 cls, u64 sub, u64 proto) {
        const u64 r = call(usb, "WinUsb_ParseConfigurationDescriptor", {c, start, ifn, alt, cls, sub, proto});
        return r == 0 ? 0xEEEEu : static_cast<u32>(r - c);
    };
    EXPECT_EQ(pcd(c, minus1, minus1, minus1, minus1, minus1), 9u);
    EXPECT_EQ(pcd(c, 0, 1, minus1, minus1, minus1), 25u);
    EXPECT_EQ(pcd(c, 1, minus1, minus1, minus1, minus1), 34u);
    EXPECT_EQ(pcd(c, minus1, minus1, 3, minus1, minus1), 34u);
    EXPECT_EQ(pcd(c, minus1, minus1, minus1, minus1, 3), 25u);
    EXPECT_EQ(pcd(c, 5, minus1, minus1, minus1, minus1), 0xEEEEu);
    EXPECT_EQ(pcd(c + 10, minus1, minus1, minus1, minus1, minus1), 0xEEEEu);
    auto pd = [&](u32 total, GuestAddr start, u32 type) {
        const u64 r = call(usb, "WinUsb_ParseDescriptors", {c, total, start, type});
        return r == 0 ? 0xEEEEu : static_cast<u32>(r - c);
    };
    EXPECT_EQ(pd(59, c, 5), 18u);
    EXPECT_EQ(pd(59, c + 19, 5), 52u);
    EXPECT_EQ(pd(59, c, 0x21), 43u);
    EXPECT_EQ(pd(59, c, 0x30), 0xEEEEu);
    EXPECT_EQ(pd(20, c, 5), 18u);
    ASSERT_TRUE(memory.write_u8(c + 18, 0).is_ok());   // zero-length descriptor
    EXPECT_EQ(pd(59, c, 5), 18u);
    EXPECT_EQ(pcd(c, 1, minus1, minus1, minus1, minus1), 0xEEEEu);
}

TEST_P(SetupDiContractTest, BluetoothRadioHandlesFollowMeasuredRules) {
    FakeBluetoothHost host(true);
    HandleTable handles;
    auto bt = create_bthprops_thunks(&host, &handles, {});
    const GuestAddr params = sized(4);
    const GuestAddr radio_out = alloc(8);
    // ffr_nullparams / ffr_size0.
    EXPECT_EQ(call(bt, "BluetoothFindFirstRadio", {0, radio_out}), 0u);
    EXPECT_EQ(get_last_error(), 87u);
    ASSERT_TRUE(memory.write_u32(params, 0).is_ok());
    EXPECT_EQ(call(bt, "BluetoothFindFirstRadio", {params, radio_out}), 0u);
    EXPECT_EQ(get_last_error(), 1306u);
    // A radio: a real kernel handle and a find handle that is exhausted.
    ASSERT_TRUE(memory.write_u32(params, 4).is_ok());
    const u64 find = call(bt, "BluetoothFindFirstRadio", {params, radio_out});
    ASSERT_NE(find, 0u);
    const u32 radio = u32_at(radio_out);
    EXPECT_EQ(handles.get_type(radio), HandleType::Device);
    EXPECT_EQ(call(bt, "BluetoothFindNextRadio", {find, alloc(8)}), 0u);
    EXPECT_EQ(get_last_error(), 259u);
    EXPECT_EQ(call(bt, "BluetoothFindNextRadio", {0, alloc(8)}), 0u);   // fnr_null
    EXPECT_EQ(get_last_error(), 6u);
    EXPECT_EQ(call(bt, "BluetoothFindRadioClose", {find}), 1u);
    EXPECT_EQ(call(bt, "BluetoothFindRadioClose", {find}), 0u);         // frc_null
    EXPECT_EQ(get_last_error(), 6u);
    // gri_*: NULL 87, dwSize 1306 (520 is right), not a radio 6.
    const GuestAddr info = sized(520, 520);
    EXPECT_EQ(call(bt, "BluetoothGetRadioInfo", {0, info}), 87u);
    EXPECT_EQ(call(bt, "BluetoothGetRadioInfo", {radio, 0}), 87u);
    ASSERT_TRUE(memory.write_u32(info, 8).is_ok());
    EXPECT_EQ(call(bt, "BluetoothGetRadioInfo", {radio, info}), 1306u);
    ASSERT_TRUE(memory.write_u32(info, 520).is_ok());
    const Handle event = handles.allocate(HandleType::Event, nullptr);
    EXPECT_EQ(call(bt, "BluetoothGetRadioInfo", {event, info}), 6u);
    EXPECT_EQ(call(bt, "BluetoothIsConnectable", {event}), 0u);          // isconn_event
    EXPECT_EQ(get_last_error(), 6u);
    // Device searches: ffd_* and fnd_null / fdc_null.
    const u32 search_size = x64() ? 40u : 32u;
    const GuestAddr search = sized(search_size, 64);
    const GuestAddr device = sized(560, 560);
    EXPECT_EQ(call(bt, "BluetoothFindFirstDevice", {0, device}), 0u);
    EXPECT_EQ(get_last_error(), 87u);
    ASSERT_TRUE(memory.write_u32(search, 4).is_ok());
    EXPECT_EQ(call(bt, "BluetoothFindFirstDevice", {search, device}), 0u);
    EXPECT_EQ(get_last_error(), 1306u);
    ASSERT_TRUE(memory.write_u32(search, search_size).is_ok());
    ASSERT_TRUE(memory.write_u32(device, 4).is_ok());
    EXPECT_EQ(call(bt, "BluetoothFindFirstDevice", {search, device}), 0u);
    EXPECT_EQ(get_last_error(), 1306u);
    EXPECT_EQ(call(bt, "BluetoothFindNextDevice", {0, device}), 0u);
    EXPECT_EQ(get_last_error(), 6u);
    EXPECT_EQ(call(bt, "BluetoothFindDeviceClose", {0}), 0u);
    EXPECT_EQ(get_last_error(), 6u);

    // No radio: NULL + ERROR_NO_MORE_ITEMS, phRadio untouched.
    FakeBluetoothHost none(false);
    auto bt_none = create_bthprops_thunks(&none, &handles, {});
    ASSERT_TRUE(memory.write_u32(radio_out, 0x5555).is_ok());
    EXPECT_EQ(call(bt_none, "BluetoothFindFirstRadio", {params, radio_out}), 0u);
    EXPECT_EQ(get_last_error(), 259u);
    EXPECT_EQ(u32_at(radio_out), 0x5555u);
}

TEST_P(SetupDiContractTest, BluetoothSdpParsersMatchWindows) {
    FakeBluetoothHost host(false);
    std::vector<std::array<u64, 4>> callbacks;
    auto bt = create_bthprops_thunks(&host, nullptr,
        [&callbacks](GuestAddr, const u64* args, usize) -> u64 {
            callbacks.push_back({args[0], args[1], args[2], args[3]});
            return callbacks.size() < 4 ? 1 : 0;
        });
    const GuestAddr rec = alloc(kSdpRecord.size());
    ASSERT_TRUE(memory.write_bytes(rec, kSdpRecord.data(), kSdpRecord.size()).is_ok());
    const u32 len = static_cast<u32>(kSdpRecord.size());
    const GuestAddr data = alloc(24);
    auto attr = [&](u32 id) { return call(bt, "BluetoothSdpGetAttributeValue", {rec, len, id, data}); };
    auto u64_at = [&](u32 offset) { return memory.read_u64(data + offset).value(); };
    // attr id=0: UINT32 (type 1, SDP_ST_UINT32 0x210).
    EXPECT_EQ(attr(0x0000), 0u);
    EXPECT_EQ(u32_at(data), 1u);
    EXPECT_EQ(u32_at(data + 4), 0x210u);
    EXPECT_EQ(u64_at(8), 0x00010000u);
    // attr id=1: a sequence points at its own descriptor, whole length.
    EXPECT_EQ(attr(0x0001), 0u);
    EXPECT_EQ(u32_at(data), 6u);
    const u64 seq_ptr = x64() ? u64_at(8) : u32_at(data + 8);
    const u32 seq_len = u32_at(data + (x64() ? 16 : 12));
    EXPECT_EQ(seq_ptr - rec, 0x0Du);
    EXPECT_EQ(seq_len, 5u);
    // attr id=100: a string points at its text.
    EXPECT_EQ(attr(0x0100), 0u);
    EXPECT_EQ(u32_at(data), 4u);
    EXPECT_EQ((x64() ? u64_at(8) : u32_at(data + 8)) - rec, 0x25u);
    // attr id=200: INT8 -5 in a zeroed union; id=203: UUID32 is 0x220.
    EXPECT_EQ(attr(0x0200), 0u);
    EXPECT_EQ(u32_at(data + 4), 0x20u);
    EXPECT_EQ(u64_at(8), 0xFBu);
    EXPECT_EQ(u64_at(16), 0u);
    EXPECT_EQ(attr(0x0203), 0u);
    EXPECT_EQ(u32_at(data), 3u);
    EXPECT_EQ(u32_at(data + 4), 0x220u);
    EXPECT_EQ(u32_at(data + 8), 0x12345678u);
    // attr id=204: UUID128 as a GUID.
    EXPECT_EQ(attr(0x0204), 0u);
    EXPECT_EQ(u32_at(data + 4), 0x430u);
    EXPECT_EQ(u32_at(data + 8), 0x00001101u);
    EXPECT_EQ(memory.read_u16(data + 14).value(), 0x1000u);
    EXPECT_EQ(memory.read_u8(data + 16).value(), 0x80u);
    // attr id=206: UINT128 LowPart/HighPart.
    EXPECT_EQ(attr(0x0206), 0u);
    EXPECT_EQ(u64_at(8), 0x090A0B0C0D0E0F10ull);
    EXPECT_EQ(u64_at(16), 0x0102030405060708ull);
    // attr id=300 missing: ERROR_FILE_NOT_FOUND; attr_notseq / NULL: 87.
    EXPECT_EQ(attr(0x0300), 2u);
    EXPECT_EQ(call(bt, "BluetoothSdpGetAttributeValue", {0, len, 0, data}), 87u);

    // elem desc=01/18/48 invalid, desc=25 ok.
    const GuestAddr raw = alloc(8);
    auto elem = [&](std::initializer_list<u8> bytes) {
        std::vector<u8> v(bytes);
        EXPECT_TRUE(memory.write_bytes(raw, v.data(), v.size()).is_ok());
        return call(bt, "BluetoothSdpGetElementData", {raw, static_cast<u64>(v.size()), data});
    };
    EXPECT_EQ(elem({0x01}), 87u);
    EXPECT_EQ(elem({0x18, 0x01}), 87u);
    EXPECT_EQ(elem({0x48}), 87u);
    EXPECT_EQ(elem({0x09, 0x12}), 87u);
    EXPECT_EQ(elem({0x25, 0x01, 'x'}), 0u);

    // cw1..cw4: the element handle walks, ends NULL + 259, then restarts.
    const std::vector<u8> seq = {0x35, 0x04, 0x08, 0x01, 0x08, 0x02};
    const GuestAddr sq = alloc(seq.size());
    ASSERT_TRUE(memory.write_bytes(sq, seq.data(), seq.size()).is_ok());
    const GuestAddr it = alloc(8);
    ASSERT_TRUE(memory.write_u64(it, 0).is_ok());
    auto cont = [&] { return call(bt, "BluetoothSdpGetContainerElementData", {sq, 6, it, data}); };
    auto it_off = [&] {
        const u64 v = x64() ? memory.read_u64(it).value() : u32_at(it);
        return v == 0 ? 0xEEu : static_cast<u32>(v - sq);
    };
    EXPECT_EQ(cont(), 0u);
    EXPECT_EQ(it_off(), 2u);
    EXPECT_EQ(cont(), 0u);
    EXPECT_EQ(it_off(), 4u);
    EXPECT_EQ(cont(), 259u);
    EXPECT_EQ(it_off(), 0xEEu);
    EXPECT_EQ(cont(), 0u);
    EXPECT_EQ(it_off(), 2u);

    // str_explicit / str_query / str_badmib / str_missing.
    const GuestAddr st = alloc(8);
    ASSERT_TRUE(memory.write_u16(st, 0x656E).is_ok());
    ASSERT_TRUE(memory.write_u16(st + 2, 106).is_ok());
    ASSERT_TRUE(memory.write_u16(st + 4, 0x0100).is_ok());
    const GuestAddr text = alloc(64);
    const GuestAddr cch = alloc(4);
    ASSERT_TRUE(memory.write_u32(cch, 16).is_ok());
    EXPECT_EQ(call(bt, "BluetoothSdpGetString", {rec, len, st, 0, text, cch}), 0u);
    EXPECT_EQ(read_wide(text), "COM");
    EXPECT_EQ(u32_at(cch), 4u);
    EXPECT_EQ(call(bt, "BluetoothSdpGetString", {rec, len, st, 0, 0, cch}), 234u);
    EXPECT_EQ(u32_at(cch), 4u);
    EXPECT_EQ(call(bt, "BluetoothSdpGetString", {rec, len, st, 2, text, cch}), 2u);
    EXPECT_EQ(call(bt, "BluetoothSdpGetString", {rec, len, st, 1, text, cch}), 87u);
    ASSERT_TRUE(memory.write_u16(st + 2, 0x7777).is_ok());
    EXPECT_EQ(call(bt, "BluetoothSdpGetString", {rec, len, st, 0, text, cch}), 13u);
    ASSERT_TRUE(memory.write_u32(cch, 16).is_ok());
    EXPECT_EQ(call(bt, "BluetoothSdpGetString", {rec, len, 0, 0, text, cch}), 0u);   // str_locale
    EXPECT_EQ(read_wide(text), "COM");

    // enum_stop4: callbacks get (id, value element, its size, param); FALSE
    // stops the walk, which still returns TRUE with ERROR_INVALID_DATA.
    set_last_error(0xDEAD);
    EXPECT_EQ(call(bt, "BluetoothSdpEnumAttributes", {rec, len, 0x401000, 4}), 1u);
    EXPECT_EQ(get_last_error(), 13u);
    ASSERT_EQ(callbacks.size(), 4u);
    EXPECT_EQ(callbacks[0][0], 0x0000u);
    EXPECT_EQ(callbacks[0][1] - rec, 5u);
    EXPECT_EQ(callbacks[0][2], 5u);
    EXPECT_EQ(callbacks[0][3], 4u);
    EXPECT_EQ(callbacks[3][0], 0x0100u);
    EXPECT_EQ(callbacks[3][1] - rec, 0x23u);
    EXPECT_EQ(call(bt, "BluetoothSdpEnumAttributes", {0, len, 0x401000, 4}), 0u);   // enum_nullstream
    EXPECT_EQ(get_last_error(), 87u);
}

TEST_P(SetupDiContractTest, HidDeviceCallsClassifyTheHandle) {
    // hid_probe.expected: an open handle that is not a HID device is
    // ERROR_INVALID_PARAMETER (attrs_file); NULL, INVALID_HANDLE_VALUE and a
    // closed handle are ERROR_INVALID_HANDLE (attrs_null/_invalid/_closed).
    HandleTable handles;
    auto hid = create_hid_thunks(&handles);
    const GuestAddr attributes = sized(12);
    const Handle open = handles.allocate(HandleType::Event, nullptr);
    EXPECT_EQ(call(hid, "HidD_GetAttributes", {open, attributes}), 0u);
    EXPECT_EQ(get_last_error(), 87u);
    EXPECT_EQ(call(hid, "HidD_GetAttributes", {0, attributes}), 0u);
    EXPECT_EQ(get_last_error(), 6u);
    EXPECT_EQ(call(hid, "HidD_GetAttributes", {invalid(), attributes}), 0u);
    EXPECT_EQ(get_last_error(), 6u);
    ASSERT_TRUE(handles.close(open));
    EXPECT_EQ(call(hid, "HidD_GetAttributes", {open, attributes}), 0u);
    EXPECT_EQ(get_last_error(), 6u);
    // free_fake: FALSE with the last error untouched.
    set_last_error(0xDEAD);
    EXPECT_EQ(call(hid, "HidD_FreePreparsedData", {alloc(64)}), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    // caps_fake: HIDP_STATUS_INVALID_PREPARSED_DATA.
    EXPECT_EQ(call(hid, "HidP_GetCaps", {alloc(64), alloc(64)}), 0xC0110001u);
}
