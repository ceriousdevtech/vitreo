/// @file test_machine_identity_registry.cpp
/// @brief The machine name and system environment keys every Windows has.
///
/// Names, types and %SystemRoot% forms are
/// tools/oracle_probe/machine_identity_registry.expected (`reg query` on
/// Windows 11); the values follow GetComputerName and the process environment.

#include <gtest/gtest.h>

#include "capsule/registry/registry.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/system/machine_identity_registry.hpp"

#include <string>
#include <variant>

using namespace vitreo;
using namespace vitreo::capsule;

namespace {

struct Expected {
    const wchar_t* key;
    const wchar_t* name;
    RegType type;
    std::wstring value;
};

void expect_value(Registry& reg, const Expected& e) {
    SCOPED_TRACE(std::string(e.name, e.name + std::wcslen(e.name)));
    auto key = reg.open_key(HKEY_LOCAL_MACHINE, e.key, false);
    ASSERT_TRUE(key.is_ok());
    auto value = reg.query_value(key.value(), e.name);
    ASSERT_TRUE(value.is_ok());
    EXPECT_EQ(value.value().type, e.type);
    EXPECT_EQ(std::get<std::wstring>(value.value().data), e.value);
    reg.close_key(key.value());
}

constexpr const wchar_t* kComputerName = L"SYSTEM\\CurrentControlSet\\Control\\ComputerName\\ComputerName";
constexpr const wchar_t* kActive = L"SYSTEM\\CurrentControlSet\\Control\\ComputerName\\ActiveComputerName";
constexpr const wchar_t* kTcpip = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters";
constexpr const wchar_t* kEnvironment = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";

TEST(MachineIdentityRegistryTest, NameAndSystemEnvironmentMatchWindowsLayout) {
    GuestMemory memory;
    winapi::EnvironmentManager environment("app.exe", memory);
    Registry reg;

    winapi::publish_machine_identity(reg, "Vitreo", environment);

    // NetBIOS name upper case, DNS host name as given.
    expect_value(reg, {kComputerName, L"ComputerName", RegType::Sz, L"VITREO"});
    expect_value(reg, {kComputerName, L"", RegType::Sz, L"mnmsrvc"});
    expect_value(reg, {kActive, L"ComputerName", RegType::Sz, L"VITREO"});
    expect_value(reg, {kTcpip, L"Hostname", RegType::Sz, L"Vitreo"});
    expect_value(reg, {kTcpip, L"NV Hostname", RegType::Sz, L"Vitreo"});

    expect_value(reg, {kEnvironment, L"ComSpec", RegType::ExpandSz, L"%SystemRoot%\\system32\\cmd.exe"});
    expect_value(reg, {kEnvironment, L"OS", RegType::Sz, L"Windows_NT"});
    expect_value(reg, {kEnvironment, L"PATHEXT", RegType::Sz,
                       L".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC"});
    expect_value(reg, {kEnvironment, L"TEMP", RegType::ExpandSz, L"%SystemRoot%\\TEMP"});
    expect_value(reg, {kEnvironment, L"TMP", RegType::ExpandSz, L"%SystemRoot%\\TEMP"});
    expect_value(reg, {kEnvironment, L"USERNAME", RegType::Sz, L"SYSTEM"});
    expect_value(reg, {kEnvironment, L"windir", RegType::ExpandSz, L"%SystemRoot%"});
    expect_value(reg, {kEnvironment, L"Path", RegType::ExpandSz, L"%SystemRoot%\\system32;%SystemRoot%"});
    expect_value(reg, {kEnvironment, L"PROCESSOR_ARCHITECTURE", RegType::Sz, L"AMD64"});
    // The processor variables are the process environment's.
    for (const char* name : {"NUMBER_OF_PROCESSORS", "PROCESSOR_IDENTIFIER", "PROCESSOR_LEVEL",
                             "PROCESSOR_REVISION", "DriverData"}) {
        const std::string value = environment.get_variable(name).value();
        const std::wstring wide_name(name, name + std::strlen(name));
        const RegType type = RegType::Sz;
        expect_value(reg, {kEnvironment, wide_name.c_str(), type, std::wstring(value.begin(), value.end())});
    }
}

TEST(MachineIdentityRegistryTest, GuestEditsToSystemVariablesSurviveRepublishing) {
    GuestMemory memory;
    winapi::EnvironmentManager environment("app.exe", memory);
    Registry reg;
    winapi::publish_machine_identity(reg, "VITREO", environment);

    auto env = reg.open_key(HKEY_LOCAL_MACHINE, kEnvironment, false);
    ASSERT_TRUE(env.is_ok());
    ASSERT_TRUE(reg.set_value(env.value(), L"Path",
                              RegValue{RegType::ExpandSz, {std::wstring(L"%SystemRoot%\\system32;C:\\Tools")}})
                    .is_ok());
    reg.close_key(env.value());

    winapi::publish_machine_identity(reg, "VITREO", environment);
    expect_value(reg, {kEnvironment, L"Path", RegType::ExpandSz, L"%SystemRoot%\\system32;C:\\Tools"});
}

} // namespace
