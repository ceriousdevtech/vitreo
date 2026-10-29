/// @file test_hardware_description.cpp
/// @brief HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor describes the host.
///
/// Value names, types and the Identifier format are
/// tools/oracle_probe/processor_topology_probe64.expected ("reg"); values come
/// from a fixed M4 Pro profile.

#include <gtest/gtest.h>

#include "capsule/registry/registry.hpp"
#include "platform/host_hardware_fixtures.hpp"
#include "winapi/kernel32/system/hardware_description.hpp"

#include <string>
#include <variant>

using namespace vitreo;
using namespace vitreo::capsule;

namespace {

TEST(HardwareDescriptionTest, CentralProcessorKeysDescribeEachProcessor) {
    test::ScopedHostHardware host(test::m4_pro_profile());
    Registry reg;
    // A stale key from an earlier machine or version disappears.
    auto stale = reg.open_key(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\20", true);
    ASSERT_TRUE(stale.is_ok());
    reg.close_key(stale.value());

    winapi::publish_hardware_description(reg);

    auto root = reg.open_key(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor", false);
    ASSERT_TRUE(root.is_ok());
    auto keys = reg.enum_keys(root.value());
    ASSERT_TRUE(keys.is_ok());
    EXPECT_EQ(keys.value().size(), 14u);
    reg.close_key(root.value());

    for (const u32 p : {0u, 3u, 4u, 13u}) {
        SCOPED_TRACE(p);
        auto key = reg.open_key(HKEY_LOCAL_MACHINE,
                                L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\" + std::to_wstring(p), false);
        ASSERT_TRUE(key.is_ok());
        auto mhz = reg.query_value(key.value(), L"~MHz");
        ASSERT_TRUE(mhz.is_ok());
        EXPECT_EQ(mhz.value().type, RegType::Dword);
        EXPECT_EQ(std::get<u32>(mhz.value().data), p < 4 ? 2592u : 4512u);  // E / P class clock
        auto text = [&](const wchar_t* name) {
            auto value = reg.query_value(key.value(), name);
            EXPECT_TRUE(value.is_ok()) << name;
            if (value.is_err()) return std::wstring();
            EXPECT_EQ(value.value().type, RegType::Sz);
            return std::get<std::wstring>(value.value().data);
        };
        EXPECT_EQ(text(L"Identifier"), L"Intel64 Family 6 Model 60 Stepping 3");  // CPUID 0x306C3
        EXPECT_EQ(text(L"ProcessorNameString"), L"Apple M4 Pro");
        EXPECT_EQ(text(L"VendorIdentifier"), L"GenuineIntel");
        reg.close_key(key.value());
    }
    EXPECT_TRUE(reg.open_key(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\20", false)
                    .is_err());
}

} // namespace
