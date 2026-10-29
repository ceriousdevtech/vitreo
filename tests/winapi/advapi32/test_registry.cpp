/// @file test_registry.cpp
/// @brief Unit tests for advapi32 registry thunks (WIN32-060)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "winapi/advapi32/registry/registry.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"

#include <algorithm>
#include <array>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::capsule;

// =============================================================================
// Fixture
// =============================================================================

namespace {
constexpr u32 kCreateKeyProbeValue = 0x2Au;
} // namespace

class Advapi32RegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_registry = std::make_unique<Registry>();
        m_handles  = std::make_unique<HandleTable>();
        m_thunks   = std::make_unique<RegistryThunks>(*m_registry, *m_handles);

        // Pre-populate some registry data under HKLM\SOFTWARE\Vitreo
        auto h = m_registry->open_key(HKEY_LOCAL_MACHINE,
                                       L"SOFTWARE\\Vitreo", /*create=*/true);
        ASSERT_TRUE(h.is_ok());

        m_registry->set_value(h.value(), L"Version",
                              RegValue{RegType::Sz, std::wstring(L"1.0")});
        m_registry->set_value(h.value(), L"Build",
                              RegValue{RegType::Dword, u32(42)});

        m_registry->close_key(h.value());
    }

    std::unique_ptr<Registry>       m_registry;
    std::unique_ptr<HandleTable>    m_handles;
    std::unique_ptr<RegistryThunks> m_thunks;
};

// =============================================================================
// 1. RegOpenKeyExW opens existing key
// =============================================================================

TEST_F(Advapi32RegistryTest, OpenKeyExW_OpensExistingKey) {
    u64 out_handle = 0;
    u32 err = m_thunks->reg_open_key_ex_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", 0, 0, &out_handle);

    EXPECT_EQ(err, ERROR_SUCCESS);
    EXPECT_NE(out_handle, 0u);

    // Clean up
    m_thunks->reg_close_key(out_handle);
}

TEST_F(Advapi32RegistryTest, SignExtendedX64PredefinedKeysResolve) {
    constexpr u64 sign_extension = 0xffffffff00000000ull;
    for (const u64 root : {
             HKEY_CLASSES_ROOT,
             HKEY_CURRENT_USER,
             HKEY_LOCAL_MACHINE,
             HKEY_USERS,
         }) {
        EXPECT_TRUE(m_thunks->is_valid_key(sign_extension | root))
            << "root=0x" << std::hex << root;
    }

    // Do not treat an arbitrary 64-bit handle whose low word resembles a
    // predefined root as that root.
    EXPECT_FALSE(m_thunks->is_valid_key(
        0x1234567880000001ull));
}

TEST_F(Advapi32RegistryTest,
       SignExtendedX64CurrentUserSupportsCreateSetAndQuery) {
    constexpr u64 x64_current_user = 0xffffffff80000001ull;
    u64 key = 0;
    u32 disposition = 0;
    ASSERT_EQ(m_thunks->reg_create_key_ex_a(
                  x64_current_user, "Software\\VitreoX64Hkey",
                  &key, &disposition),
              ERROR_SUCCESS);
    EXPECT_EQ(disposition, REG_CREATED_NEW_KEY);

    constexpr u32 written = 0x1234abcd;
    ASSERT_EQ(m_thunks->reg_set_value_ex_a(
                  key, "Value", REG_DWORD,
                  reinterpret_cast<const u8*>(&written), sizeof(written)),
              ERROR_SUCCESS);

    u32 type = REG_NONE;
    u32 size = sizeof(u32);
    u32 observed = 0;
    EXPECT_EQ(m_thunks->reg_query_value_ex_a(
                  key, "Value", &type,
                  reinterpret_cast<u8*>(&observed), &size),
              ERROR_SUCCESS);
    EXPECT_EQ(type, REG_DWORD);
    EXPECT_EQ(size, sizeof(u32));
    EXPECT_EQ(observed, written);
    EXPECT_EQ(m_thunks->reg_close_key(key), ERROR_SUCCESS);
}

// Storm builds HKCU\Software\Blizzard Entertainment\Internal and queries
// "Debug Memory" / "Protect Memory" as REG_DWORD via the ANSI APIs.
TEST_F(Advapi32RegistryTest, AnsiQueryLoadsSpacedBlizzardInternalDword) {
    constexpr u32 KEY_READ = 0x00020019u;
    u64 key = 0;
    ASSERT_EQ(m_thunks->reg_create_key_ex_a(
                  HKEY_CURRENT_USER,
                  "Software\\Blizzard Entertainment\\Internal",
                  &key, nullptr),
              ERROR_SUCCESS);
    constexpr u32 enabled = 1;
    ASSERT_EQ(m_thunks->reg_set_value_ex_a(
                  key, "Debug Memory", REG_DWORD,
                  reinterpret_cast<const u8*>(&enabled), sizeof(enabled)),
              ERROR_SUCCESS);
    EXPECT_EQ(m_thunks->reg_close_key(key), ERROR_SUCCESS);

    key = 0;
    ASSERT_EQ(m_thunks->reg_open_key_ex_a(
                  HKEY_CURRENT_USER,
                  "Software\\Blizzard Entertainment\\Internal",
                  0, KEY_READ, &key),
              ERROR_SUCCESS);
    u32 type = REG_NONE;
    u32 size = sizeof(u32);
    u32 observed = 0;
    EXPECT_EQ(m_thunks->reg_query_value_ex_a(
                  key, "Debug Memory", &type,
                  reinterpret_cast<u8*>(&observed), &size),
              ERROR_SUCCESS);
    EXPECT_EQ(type, REG_DWORD);
    EXPECT_EQ(size, sizeof(u32));
    EXPECT_EQ(observed, 1u);
    EXPECT_EQ(m_thunks->reg_close_key(key), ERROR_SUCCESS);
}

TEST_F(Advapi32RegistryTest, RegOpenKeyAWritesPointerSizedHkeyOnX64) {
    constexpr GuestAddr memory_base = 0x1000;
    constexpr GuestAddr stack = 0x8000;
    constexpr GuestAddr subkey = 0x2000;
    constexpr GuestAddr result = 0x3000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        memory_base, 0x10000, MemoryProtection::ReadWrite).is_ok());
    constexpr char path[] = "SOFTWARE\\Vitreo";
    ASSERT_TRUE(memory.write_bytes(
        subkey, reinterpret_cast<const Byte*>(path), sizeof(path)).is_ok());
    ASSERT_TRUE(memory.write_u64(result, 0xaaaaaaaaaaaaaaaaull).is_ok());

    auto exports = create_advapi32_registry_thunks(*m_thunks);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "RegOpenKeyA";
        });
    ASSERT_NE(entry, exports.entries.end());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = stack;
    cpu.gpr.rcx = 0xffffffff80000002ull;
    cpu.gpr.rdx = subkey;
    cpu.gpr.r8 = result;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    entry->handler(marshaler);

    ASSERT_EQ(cpu.gpr.rax, ERROR_SUCCESS);
    const u64 hkey = memory.read_u64(result).value_or(0);
    EXPECT_NE(hkey, 0u);
    EXPECT_EQ(hkey >> 32, 0u);
    EXPECT_EQ(m_thunks->reg_close_key(hkey), ERROR_SUCCESS);
}

// RegCreateKeyW is the legacy 3-argument form of RegCreateKeyExW: it creates
// the subkey when absent, opens it when present, writes the handle through
// phkResult and returns ERROR_SUCCESS (a system error code, not a BOOL).
// Unity's PlayerPrefs stores values under HKCU\Software\<company>\<product>
// through exactly this export, so a missing implementation surfaced as
// "PlayerPrefsException: Could not store preference value".
TEST_F(Advapi32RegistryTest, RegCreateKeyWCreatesSubkeyAndWritesHandle) {
    constexpr GuestAddr memory_base = 0x1000;
    constexpr GuestAddr stack = 0x8000;
    constexpr GuestAddr subkey = 0x2000;
    constexpr GuestAddr result = 0x3000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        memory_base, 0x10000, MemoryProtection::ReadWrite).is_ok());

    const std::wstring path = L"Software\\Johan Gronvall\\BoplBattle";
    for (usize i = 0; i <= path.size(); ++i) {
        ASSERT_TRUE(memory.write_u16(
            subkey + i * 2,
            static_cast<u16>(i < path.size() ? path[i] : L'\0')).is_ok());
    }
    ASSERT_TRUE(memory.write_u64(result, 0xaaaaaaaaaaaaaaaaull).is_ok());

    auto exports = create_advapi32_registry_thunks(*m_thunks);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "RegCreateKeyW";
        });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 12u);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = stack;
    cpu.gpr.rcx = HKEY_CURRENT_USER;
    cpu.gpr.rdx = subkey;
    cpu.gpr.r8 = result;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    entry->handler(marshaler);

    // No fake success and no 0xFFFFFFFF missing-export value.
    ASSERT_EQ(cpu.gpr.rax, ERROR_SUCCESS);
    const u64 hkey = memory.read_u64(result).value_or(0);
    ASSERT_NE(hkey, 0u);

    // The created key is real: a value written through it reads back.
    ASSERT_EQ(m_thunks->reg_set_value_ex_w(
                  hkey, L"Volume", REG_DWORD,
                  reinterpret_cast<const Byte*>(&kCreateKeyProbeValue),
                  sizeof(kCreateKeyProbeValue)),
              ERROR_SUCCESS);
    EXPECT_EQ(m_thunks->reg_close_key(hkey), ERROR_SUCCESS);

    u64 reopened = 0;
    ASSERT_EQ(m_thunks->reg_open_key_ex_w(
                  HKEY_CURRENT_USER, path, 0, 0, &reopened),
              ERROR_SUCCESS);
    u32 type = REG_NONE;
    u32 size = sizeof(u32);
    u32 observed = 0;
    EXPECT_EQ(m_thunks->reg_query_value_ex_w(
                  reopened, L"Volume", &type,
                  reinterpret_cast<u8*>(&observed), &size),
              ERROR_SUCCESS);
    EXPECT_EQ(type, REG_DWORD);
    EXPECT_EQ(observed, kCreateKeyProbeValue);
    EXPECT_EQ(m_thunks->reg_close_key(reopened), ERROR_SUCCESS);

    // Re-creating an existing key opens it rather than failing.
    cpu.gpr.rax = 0;
    ASSERT_TRUE(memory.write_u64(result, 0).is_ok());
    entry->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, ERROR_SUCCESS);
    const u64 hkey2 = memory.read_u64(result).value_or(0);
    ASSERT_NE(hkey2, 0u);
    EXPECT_EQ(m_thunks->reg_close_key(hkey2), ERROR_SUCCESS);
}

TEST_F(Advapi32RegistryTest,
       RegConnectRegistryCreatesLocalAliasAndRejectsRemote) {
    constexpr GuestAddr memory_base = 0x1000;
    constexpr GuestAddr stack = 0x8000;
    constexpr GuestAddr machine = 0x2000;
    constexpr GuestAddr result = 0x3000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        memory_base, 0x10000, MemoryProtection::ReadWrite).is_ok());

    auto exports = create_advapi32_registry_thunks(*m_thunks);
    auto find_export = [&exports](std::string_view name) {
        return std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& candidate) {
                return candidate.name == name;
            });
    };
    const auto connect_a = find_export("RegConnectRegistryA");
    const auto connect_w = find_export("RegConnectRegistryW");
    ASSERT_NE(connect_a, exports.entries.end());
    ASSERT_NE(connect_w, exports.entries.end());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(memory.write_u32(stack + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, HKEY_LOCAL_MACHINE).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, result).is_ok());
    connect_a->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, ERROR_SUCCESS);
    const u32 local = memory.read_u32(result).value_or(0);
    EXPECT_NE(local, 0u);
    EXPECT_NE(local, HKEY_LOCAL_MACHINE);
    EXPECT_TRUE(m_thunks->is_valid_key(local));
    EXPECT_EQ(m_thunks->reg_close_key(local), ERROR_SUCCESS);
    EXPECT_FALSE(m_thunks->is_valid_key(local));

    constexpr char16_t remote[] = u"\\\\remote";
    ASSERT_TRUE(memory.write_bytes(
        machine, reinterpret_cast<const Byte*>(remote),
        sizeof(remote)).is_ok());
    ASSERT_TRUE(memory.write_u32(result, 0xAAAAAAAAu).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, machine).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, HKEY_USERS).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, result).is_ok());
    connect_w->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, ERROR_CALL_NOT_IMPLEMENTED);
    EXPECT_EQ(memory.read_u32(result).value_or(0), 0xAAAAAAAAu);
}

// =============================================================================
// 2. RegQueryValueExW reads REG_SZ value
// =============================================================================

TEST_F(Advapi32RegistryTest, QueryValueExW_ReadsSzValue) {
    u64 hk = 0;
    ASSERT_EQ(m_thunks->reg_open_key_ex_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", 0, 0, &hk), ERROR_SUCCESS);

    u32 type = 0;
    u32 size = 256;
    u8 buf[256] = {};

    u32 err = m_thunks->reg_query_value_ex_w(hk, L"Version", &type, buf, &size);
    EXPECT_EQ(err, ERROR_SUCCESS);
    EXPECT_EQ(type, REG_SZ);

    // Verify we got the wide string "1.0\0" back in UTF-16LE.
    // Output is UTF-16LE (2 bytes per code unit), matching Win32 format.
    const auto* u16 = reinterpret_cast<const char16_t*>(buf);
    EXPECT_EQ(std::u16string(u16), u"1.0");

    m_thunks->reg_close_key(hk);
}

// =============================================================================
// 3. RegQueryValueExW reads REG_DWORD value
// =============================================================================

TEST_F(Advapi32RegistryTest, QueryValueExW_ReadsDwordValue) {
    u64 hk = 0;
    ASSERT_EQ(m_thunks->reg_open_key_ex_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", 0, 0, &hk), ERROR_SUCCESS);

    u32 type = 0;
    u32 size = sizeof(u32);
    u32 data = 0;

    u32 err = m_thunks->reg_query_value_ex_w(
        hk, L"Build", &type, reinterpret_cast<u8*>(&data), &size);

    EXPECT_EQ(err, ERROR_SUCCESS);
    EXPECT_EQ(type, REG_DWORD);
    EXPECT_EQ(data, 42u);
    EXPECT_EQ(size, sizeof(u32));

    m_thunks->reg_close_key(hk);
}

// =============================================================================
// 4. RegQueryValueExW size query (data=nullptr)
// =============================================================================

TEST_F(Advapi32RegistryTest, QueryValueExW_SizeQuery) {
    u64 hk = 0;
    ASSERT_EQ(m_thunks->reg_open_key_ex_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", 0, 0, &hk), ERROR_SUCCESS);

    u32 type = 0;
    u32 size = 0;

    u32 err = m_thunks->reg_query_value_ex_w(
        hk, L"Build", &type, nullptr, &size);

    EXPECT_EQ(err, ERROR_SUCCESS);
    EXPECT_EQ(type, REG_DWORD);
    EXPECT_EQ(size, sizeof(u32));

    m_thunks->reg_close_key(hk);
}

TEST_F(Advapi32RegistryTest,
       QueryValueExWDoesNotDereferenceDataWhenBufferIsTooSmall) {
    constexpr GuestAddr memory_base = 0x1000;
    constexpr GuestAddr stack = 0x1800;
    constexpr GuestAddr name = 0x1900;
    constexpr GuestAddr type_out = 0x1A00;
    constexpr GuestAddr size_out = 0x1A04;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        memory_base, 0x2000, MemoryProtection::ReadWrite).is_ok());

    u64 hk = 0;
    ASSERT_EQ(m_thunks->reg_open_key_ex_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", 0, 0, &hk), ERROR_SUCCESS);

    // UTF-16LE "Build\0"
    const char16_t build_name[] = u"Build";
    ASSERT_TRUE(memory.write_bytes(
        name, reinterpret_cast<const Byte*>(build_name),
        sizeof(build_name)).is_ok());
    ASSERT_TRUE(memory.write_u32(size_out, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(type_out, 0xFFFFFFFFu).is_ok());

    auto exports = create_advapi32_registry_thunks(*m_thunks);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "RegQueryValueExW";
        });
    ASSERT_NE(entry, exports.entries.end());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(memory.write_u32(stack, 0xDEADBEEF).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, static_cast<u32>(hk)).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, static_cast<u32>(name)).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, static_cast<u32>(type_out)).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, 1).is_ok()); // invalid lpData
    ASSERT_TRUE(memory.write_u32(stack + 24, static_cast<u32>(size_out)).is_ok());

    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), ERROR_MORE_DATA);
    EXPECT_EQ(memory.read_u32(size_out).value_or(0), sizeof(u32));
    EXPECT_EQ(memory.read_u32(type_out).value_or(0), REG_DWORD);

    // Once the caller claims enough capacity, the API attempts the copy and
    // the same unmapped pointer must fail honestly.
    ASSERT_TRUE(memory.write_u32(size_out, sizeof(u32)).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), ERROR_NOACCESS);

    m_thunks->reg_close_key(hk);
}

TEST_F(Advapi32RegistryTest, RegGetValueWQueriesSubkeyAndFiltersType) {
    u32 type = 0;
    u32 size = sizeof(u32);
    u32 value = 0;
    EXPECT_EQ(m_thunks->reg_get_value_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", L"Build",
        0x00000010u, &type, reinterpret_cast<u8*>(&value), &size),
        ERROR_SUCCESS);
    EXPECT_EQ(type, REG_DWORD);
    EXPECT_EQ(value, 42u);
    EXPECT_EQ(size, sizeof(u32));

    size = sizeof(value);
    EXPECT_EQ(m_thunks->reg_get_value_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", L"Build",
        0x00000002u, &type, reinterpret_cast<u8*>(&value), &size),
        ERROR_UNSUPPORTED_TYPE);
}

TEST_F(Advapi32RegistryTest, RegGetValueAQueriesSubkeyAndConvertsString) {
    u32 type = REG_NONE;
    u32 size = 16;
    std::array<u8, 16> value{};
    EXPECT_EQ(m_thunks->reg_get_value_a(
                  HKEY_LOCAL_MACHINE, "SOFTWARE\\Vitreo", "Version",
                  0x00000002u, &type, value.data(), &size),
              ERROR_SUCCESS);
    EXPECT_EQ(type, REG_SZ);
    EXPECT_EQ(size, 4u);
    EXPECT_STREQ(reinterpret_cast<const char*>(value.data()), "1.0");

    size = 2;
    EXPECT_EQ(m_thunks->reg_get_value_a(
                  HKEY_LOCAL_MACHINE, "SOFTWARE\\Vitreo", "Version",
                  0x00000002u, &type, value.data(), &size),
              ERROR_MORE_DATA);
    EXPECT_EQ(size, 4u);
}

TEST_F(Advapi32RegistryTest, RegGetValueExpandsEnvironmentUnlessSuppressed) {
    auto key = m_registry->open_key(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", false);
    ASSERT_TRUE(key.is_ok());
    ASSERT_TRUE(m_registry->set_value(
        key.value(), L"Path",
        RegValue{RegType::ExpandSz, std::wstring(L"%SYSTEMROOT%\\Temp")})
                    .is_ok());
    m_registry->close_key(key.value());

    GuestMemory memory;
    EnvironmentManager environment("", memory);
    m_thunks->set_environment_manager(&environment);

    u32 type = REG_NONE;
    u32 size = 64;
    std::array<u8, 64> ansi{};
    ASSERT_EQ(m_thunks->reg_get_value_a(
                  HKEY_LOCAL_MACHINE, "SOFTWARE\\Vitreo", "Path",
                  0x00000004u, &type, ansi.data(), &size),
              ERROR_SUCCESS);
    EXPECT_EQ(type, REG_EXPAND_SZ);
    EXPECT_STREQ(
        reinterpret_cast<const char*>(ansi.data()), "C:\\Windows\\Temp");

    size = static_cast<u32>(ansi.size());
    ASSERT_EQ(m_thunks->reg_get_value_a(
                  HKEY_LOCAL_MACHINE, "SOFTWARE\\Vitreo", "Path",
                  0x10000004u, &type, ansi.data(), &size),
              ERROR_SUCCESS);
    EXPECT_STREQ(
        reinterpret_cast<const char*>(ansi.data()), "%SYSTEMROOT%\\Temp");
}

TEST_F(Advapi32RegistryTest, RegGetValueRejectsInvalidFlagCombinations) {
    u32 type = REG_NONE;
    u32 size = 0;
    EXPECT_EQ(m_thunks->reg_get_value_w(
                  HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", L"Build",
                  0x00030000u, &type, nullptr, &size),
              ERROR_INVALID_PARAMETER);
    EXPECT_EQ(m_thunks->reg_get_value_w(
                  HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", L"Build",
                  0x40000000u, &type, nullptr, &size),
              ERROR_INVALID_PARAMETER);
}

// =============================================================================
// 5. RegSetValueExW + RegQueryValueExW round-trip
// =============================================================================

TEST_F(Advapi32RegistryTest, SetAndQueryValueRoundTrip) {
    u64 hk = 0;
    ASSERT_EQ(m_thunks->reg_open_key_ex_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", 0, 0, &hk), ERROR_SUCCESS);

    // Write a DWORD value "TestVal" = 99
    u32 write_val = 99;
    u32 err = m_thunks->reg_set_value_ex_w(
        hk, L"TestVal", REG_DWORD,
        reinterpret_cast<const u8*>(&write_val), sizeof(write_val));
    EXPECT_EQ(err, ERROR_SUCCESS);

    // Read it back
    u32 type = 0;
    u32 size = sizeof(u32);
    u32 read_val = 0;
    err = m_thunks->reg_query_value_ex_w(
        hk, L"TestVal", &type, reinterpret_cast<u8*>(&read_val), &size);

    EXPECT_EQ(err, ERROR_SUCCESS);
    EXPECT_EQ(type, REG_DWORD);
    EXPECT_EQ(read_val, 99u);

    m_thunks->reg_close_key(hk);
}

// =============================================================================
// 6. RegDeleteValueW removes value
// =============================================================================

TEST_F(Advapi32RegistryTest, DeleteValueW_RemovesValue) {
    u64 hk = 0;
    ASSERT_EQ(m_thunks->reg_open_key_ex_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", 0, 0, &hk), ERROR_SUCCESS);

    u32 err = m_thunks->reg_delete_value_w(hk, L"Version");
    EXPECT_EQ(err, ERROR_SUCCESS);

    // Querying the deleted value should fail.
    u32 type = 0;
    u32 size = 256;
    u8 buf[256] = {};
    err = m_thunks->reg_query_value_ex_w(hk, L"Version", &type, buf, &size);
    EXPECT_EQ(err, ERROR_FILE_NOT_FOUND);

    m_thunks->reg_close_key(hk);
}

// =============================================================================
// 7. RegCloseKey returns SUCCESS
// =============================================================================

TEST_F(Advapi32RegistryTest, CloseKey_ReturnsSuccess) {
    u64 hk = 0;
    ASSERT_EQ(m_thunks->reg_open_key_ex_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", 0, 0, &hk), ERROR_SUCCESS);

    EXPECT_EQ(m_thunks->reg_close_key(hk), ERROR_SUCCESS);
}

// =============================================================================
// 8. RegEnumKeyExW iterates subkeys
// =============================================================================

TEST_F(Advapi32RegistryTest, EnumKeyExW_IteratesSubkeys) {
    // Create a few subkeys under HKLM\SOFTWARE\Vitreo
    u64 parent = 0;
    ASSERT_EQ(m_thunks->reg_open_key_ex_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", 0, 0, &parent), ERROR_SUCCESS);

    u64 child1 = 0, child2 = 0;
    u32 disp = 0;
    ASSERT_EQ(m_thunks->reg_create_key_ex_w(parent, L"SubA", &child1, &disp),
              ERROR_SUCCESS);
    ASSERT_EQ(m_thunks->reg_create_key_ex_w(parent, L"SubB", &child2, &disp),
              ERROR_SUCCESS);

    // Enumerate
    std::wstring name;
    EXPECT_EQ(m_thunks->reg_enum_key_ex_w(parent, 0, name), ERROR_SUCCESS);
    EXPECT_FALSE(name.empty());

    EXPECT_EQ(m_thunks->reg_enum_key_ex_w(parent, 1, name), ERROR_SUCCESS);
    EXPECT_FALSE(name.empty());

    // Index 2 should be no more items.
    EXPECT_EQ(m_thunks->reg_enum_key_ex_w(parent, 2, name), ERROR_NO_MORE_ITEMS);

    m_thunks->reg_close_key(child1);
    m_thunks->reg_close_key(child2);
    m_thunks->reg_close_key(parent);
}

// =============================================================================
// 9. RegEnumValueW iterates values
// =============================================================================

TEST_F(Advapi32RegistryTest, EnumValueW_IteratesValues) {
    u64 hk = 0;
    ASSERT_EQ(m_thunks->reg_open_key_ex_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", 0, 0, &hk), ERROR_SUCCESS);

    // We have "Version" and "Build" from SetUp.
    std::wstring name;
    u32 type = 0;
    EXPECT_EQ(m_thunks->reg_enum_value_w(hk, 0, name, &type, nullptr, nullptr),
              ERROR_SUCCESS);
    EXPECT_FALSE(name.empty());

    EXPECT_EQ(m_thunks->reg_enum_value_w(hk, 1, name, &type, nullptr, nullptr),
              ERROR_SUCCESS);
    EXPECT_FALSE(name.empty());

    EXPECT_EQ(m_thunks->reg_enum_value_w(hk, 2, name, &type, nullptr, nullptr),
              ERROR_NO_MORE_ITEMS);

    m_thunks->reg_close_key(hk);
}

// =============================================================================
// 10. RegCreateKeyExW creates new key
// =============================================================================

TEST_F(Advapi32RegistryTest, CreateKeyExW_CreatesNewKey) {
    u64 hk = 0;
    u32 disp = 0;
    u32 err = m_thunks->reg_create_key_ex_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\NewApp", &hk, &disp);

    EXPECT_EQ(err, ERROR_SUCCESS);
    EXPECT_NE(hk, 0u);
    EXPECT_EQ(disp, REG_CREATED_NEW_KEY);

    // Opening the same key again should report existing.
    u64 hk2 = 0;
    u32 disp2 = 0;
    err = m_thunks->reg_create_key_ex_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\NewApp", &hk2, &disp2);
    EXPECT_EQ(err, ERROR_SUCCESS);
    EXPECT_EQ(disp2, REG_OPENED_EXISTING_KEY);

    m_thunks->reg_close_key(hk);
    m_thunks->reg_close_key(hk2);
}

// =============================================================================
// 11. Non-existent key returns ERROR_FILE_NOT_FOUND
// =============================================================================

TEST_F(Advapi32RegistryTest, OpenKeyExW_NotFoundForMissingKey) {
    u64 hk = 0;
    u32 err = m_thunks->reg_open_key_ex_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\DoesNotExist", 0, 0, &hk);

    EXPECT_EQ(err, ERROR_FILE_NOT_FOUND);
}

// =============================================================================
// 12. RegOpenKeyExA with ANSI path
// =============================================================================

TEST_F(Advapi32RegistryTest, OpenKeyExA_OpensWithAnsiPath) {
    u64 hk = 0;
    u32 err = m_thunks->reg_open_key_ex_a(
        HKEY_LOCAL_MACHINE, "SOFTWARE\\Vitreo", 0, 0, &hk);

    EXPECT_EQ(err, ERROR_SUCCESS);
    EXPECT_NE(hk, 0u);

    // Verify we can query through the ANSI-opened handle.
    u32 type = 0;
    u32 size = sizeof(u32);
    u32 data = 0;
    err = m_thunks->reg_query_value_ex_w(
        hk, L"Build", &type, reinterpret_cast<u8*>(&data), &size);
    EXPECT_EQ(err, ERROR_SUCCESS);
    EXPECT_EQ(data, 42u);

    m_thunks->reg_close_key(hk);
}

// =============================================================================
// 13. QueryValueExW with buffer too small returns ERROR_MORE_DATA
// =============================================================================

TEST_F(Advapi32RegistryTest, QueryValueExW_BufferTooSmall) {
    u64 hk = 0;
    ASSERT_EQ(m_thunks->reg_open_key_ex_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", 0, 0, &hk), ERROR_SUCCESS);

    u32 type = 0;
    u32 size = 1; // too small for a DWORD
    u8 buf[1] = {};
    u32 err = m_thunks->reg_query_value_ex_w(
        hk, L"Build", &type, buf, &size);

    EXPECT_EQ(err, ERROR_MORE_DATA);
    EXPECT_EQ(size, sizeof(u32)); // required size reported back

    m_thunks->reg_close_key(hk);
}

TEST_F(Advapi32RegistryTest, QueryValueExA_ReturnsAnsiStringAndByteCount) {
    u64 hk = 0;
    ASSERT_EQ(m_thunks->reg_open_key_ex_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", 0, 0, &hk), ERROR_SUCCESS);

    u32 type = 0;
    u32 size = 0;
    EXPECT_EQ(m_thunks->reg_query_value_ex_a(hk, "Version", &type, nullptr, &size),
              ERROR_SUCCESS);
    EXPECT_EQ(type, REG_SZ);
    EXPECT_EQ(size, 4u);

    u8 data[4] = {};
    EXPECT_EQ(m_thunks->reg_query_value_ex_a(hk, "Version", &type, data, &size),
              ERROR_SUCCESS);
    EXPECT_STREQ(reinterpret_cast<const char*>(data), "1.0");
    EXPECT_EQ(size, 4u);

    m_thunks->reg_close_key(hk);
}

TEST_F(Advapi32RegistryTest, QueryValueExA_ReportsAnsiSizeWhenBufferIsSmall) {
    u64 hk = 0;
    ASSERT_EQ(m_thunks->reg_open_key_ex_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", 0, 0, &hk), ERROR_SUCCESS);

    u32 size = 3;
    u8 data[3] = {};
    EXPECT_EQ(m_thunks->reg_query_value_ex_a(hk, "Version", nullptr, data, &size),
              ERROR_MORE_DATA);
    EXPECT_EQ(size, 4u);

    m_thunks->reg_close_key(hk);
}

TEST_F(Advapi32RegistryTest, QueryValueExA_PreservesDwordBytes) {
    u64 hk = 0;
    ASSERT_EQ(m_thunks->reg_open_key_ex_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", 0, 0, &hk), ERROR_SUCCESS);

    u32 type = 0;
    u32 size = sizeof(u32);
    u32 data = 0;
    EXPECT_EQ(m_thunks->reg_query_value_ex_a(
        hk, "Build", &type, reinterpret_cast<u8*>(&data), &size), ERROR_SUCCESS);
    EXPECT_EQ(type, REG_DWORD);
    EXPECT_EQ(data, 42u);
    EXPECT_EQ(size, sizeof(u32));

    m_thunks->reg_close_key(hk);
}

TEST_F(Advapi32RegistryTest, SetAndQueryValueExA_RoundTripsAnsiString) {
    u64 hk = 0;
    ASSERT_EQ(m_thunks->reg_open_key_ex_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", 0, 0, &hk), ERROR_SUCCESS);

    constexpr char path[] = "C:\\Program Files (x86)\\Starcraft";
    ASSERT_EQ(m_thunks->reg_set_value_ex_a(
        hk, "InstallPath", REG_SZ, reinterpret_cast<const u8*>(path), sizeof(path)),
        ERROR_SUCCESS);

    u8 data[128] = {};
    u32 type = 0;
    u32 size = sizeof(data);
    EXPECT_EQ(m_thunks->reg_query_value_ex_a(
        hk, "InstallPath", &type, data, &size), ERROR_SUCCESS);
    EXPECT_EQ(type, REG_SZ);
    EXPECT_STREQ(reinterpret_cast<const char*>(data), path);
    EXPECT_EQ(size, sizeof(path));

    m_thunks->reg_close_key(hk);
}

// =============================================================================
// 14. DLL export table has expected entries
// =============================================================================

TEST_F(Advapi32RegistryTest, ExportTable_HasAllEntries) {
    auto exports = create_advapi32_registry_thunks(*m_thunks);
    EXPECT_EQ(exports.entries.size(), 44u);

    auto has_export = [&](const char* name) {
        return std::find_if(exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& entry) {
                return entry.name == name;
            }) != exports.entries.end();
    };

    EXPECT_TRUE(has_export("RegOpenKeyExW"));
    EXPECT_TRUE(has_export("RegSetKeyValueW"));
    EXPECT_TRUE(has_export("RegDeleteKeyValueW"));
    EXPECT_TRUE(has_export("RegCloseKey"));
    EXPECT_TRUE(has_export("RegNotifyChangeKeyValue"));
    EXPECT_TRUE(has_export("RegEnumValueW"));
    EXPECT_TRUE(has_export("RegFlushKey"));
    EXPECT_TRUE(has_export("RegEnumKeyA"));
    EXPECT_TRUE(has_export("RegDeleteKeyA"));
    EXPECT_TRUE(has_export("RegDeleteTreeA"));
    EXPECT_TRUE(has_export("RegDeleteTreeW"));
    EXPECT_TRUE(has_export("RegDisableReflectionKey"));
    EXPECT_TRUE(has_export("RegCreateKeyExA"));
    EXPECT_TRUE(has_export("RegOpenKeyA"));
    EXPECT_TRUE(has_export("RegSetValueExA"));
    EXPECT_TRUE(has_export("RegSetKeyValueA"));
    EXPECT_TRUE(has_export("RegDeleteKeyW"));
    EXPECT_TRUE(has_export("RegDeleteKeyExW"));
    EXPECT_TRUE(has_export("RegDeleteKeyExA"));
    EXPECT_TRUE(has_export("RegEnumKeyW"));
    EXPECT_TRUE(has_export("RegQueryValueA"));

    auto reg_get_value = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) { return entry.name == "RegGetValueW"; });
    ASSERT_NE(reg_get_value, exports.entries.end());
    EXPECT_EQ(reg_get_value->stdcall_bytes, 28u);

    auto reg_get_value_a = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) { return entry.name == "RegGetValueA"; });
    ASSERT_NE(reg_get_value_a, exports.entries.end());
    EXPECT_EQ(reg_get_value_a->stdcall_bytes, 28u);
}

TEST_F(Advapi32RegistryTest, NotifyChangeSignalsEventOnValueMutation) {
    SyncManager sync(*m_handles);
    m_thunks->set_sync_manager(&sync);
    const Handle event =
        sync.create_event(true, false, std::string{});
    ASSERT_NE(event, NULL_HANDLE);

    u64 key = 0;
    ASSERT_EQ(m_thunks->reg_open_key_ex_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", 0, 0, &key),
        ERROR_SUCCESS);
    ASSERT_EQ(m_thunks->reg_notify_change_key_value(
        key, false, 0x00000004, event, true), ERROR_SUCCESS);

    const u32 value = 43;
    ASSERT_EQ(m_thunks->reg_set_value_ex_w(
        key, L"Build", REG_DWORD,
        reinterpret_cast<const u8*>(&value), sizeof(value)), ERROR_SUCCESS);
    EXPECT_EQ(sync.wait_for_single_object(event, 0), WAIT_OBJECT_0);

    // Registrations are one-shot. A second change does not signal the reset
    // event until the caller registers again.
    ASSERT_TRUE(sync.reset_event(event));
    ASSERT_EQ(m_thunks->reg_set_value_ex_w(
        key, L"Build", REG_DWORD,
        reinterpret_cast<const u8*>(&value), sizeof(value)), ERROR_SUCCESS);
    EXPECT_EQ(sync.wait_for_single_object(event, 0), WAIT_TIMEOUT);
    EXPECT_EQ(m_thunks->reg_close_key(key), ERROR_SUCCESS);
}

TEST_F(Advapi32RegistryTest, NotifyChangeMatchesSubtreeAndKeyNameChanges) {
    SyncManager sync(*m_handles);
    m_thunks->set_sync_manager(&sync);
    const Handle event =
        sync.create_event(true, false, std::string{});

    u64 parent = 0;
    ASSERT_EQ(m_thunks->reg_create_key_ex_w(
        HKEY_CURRENT_USER, L"Software\\NotifyParent", &parent, nullptr),
        ERROR_SUCCESS);
    ASSERT_EQ(m_thunks->reg_notify_change_key_value(
        parent, false, 0x00000001, event, true), ERROR_SUCCESS);

    u64 child = 0;
    ASSERT_EQ(m_thunks->reg_create_key_ex_w(
        parent, L"Child", &child, nullptr), ERROR_SUCCESS);
    EXPECT_EQ(sync.wait_for_single_object(event, 0), WAIT_OBJECT_0);

    ASSERT_TRUE(sync.reset_event(event));
    ASSERT_EQ(m_thunks->reg_notify_change_key_value(
        parent, true, 0x00000004, event, true), ERROR_SUCCESS);
    const u32 value = 1;
    ASSERT_EQ(m_thunks->reg_set_value_ex_w(
        child, L"Changed", REG_DWORD,
        reinterpret_cast<const u8*>(&value), sizeof(value)), ERROR_SUCCESS);
    EXPECT_EQ(sync.wait_for_single_object(event, 0), WAIT_OBJECT_0);

    EXPECT_EQ(m_thunks->reg_close_key(child), ERROR_SUCCESS);
    EXPECT_EQ(m_thunks->reg_close_key(parent), ERROR_SUCCESS);
}

TEST_F(Advapi32RegistryTest, AsyncNotifyRejectsNonEventHandle) {
    u64 key = 0;
    ASSERT_EQ(m_thunks->reg_open_key_ex_w(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Vitreo", 0, 0, &key),
        ERROR_SUCCESS);
    EXPECT_EQ(m_thunks->reg_notify_change_key_value(
        key, false, 0x00000004, 0x1234, true), ERROR_INVALID_HANDLE);
    EXPECT_EQ(m_thunks->reg_close_key(key), ERROR_SUCCESS);
}

// =============================================================================
// RegDeleteKey — real deletion, not fake success
// =============================================================================

TEST_F(Advapi32RegistryTest, DeleteKeyRemovesAnEmptySubkey) {
    u64 hk = 0;
    ASSERT_EQ(m_thunks->reg_create_key_ex_a(HKEY_CURRENT_USER, "Software\\VitreoDelTest",
                                            &hk, nullptr), ERROR_SUCCESS);
    m_thunks->reg_close_key(hk);

    EXPECT_EQ(m_thunks->reg_delete_key(HKEY_CURRENT_USER, L"Software\\VitreoDelTest"),
              ERROR_SUCCESS);
    // It must really be gone, not merely reported as deleted.
    u64 reopened = 0;
    EXPECT_NE(m_thunks->reg_open_key_ex_a(HKEY_CURRENT_USER, "Software\\VitreoDelTest",
                                          0, 0, &reopened), ERROR_SUCCESS);
}

TEST_F(Advapi32RegistryTest, DeleteKeyReportsMissingKey) {
    EXPECT_EQ(m_thunks->reg_delete_key(HKEY_CURRENT_USER, L"Software\\NoSuchVitreoKey"),
              ERROR_FILE_NOT_FOUND);
}

TEST_F(Advapi32RegistryTest, DeleteKeyRefusesKeyWithSubkeys) {
    u64 hk = 0;
    ASSERT_EQ(m_thunks->reg_create_key_ex_a(HKEY_CURRENT_USER,
                                            "Software\\VitreoParent\\Child",
                                            &hk, nullptr), ERROR_SUCCESS);
    m_thunks->reg_close_key(hk);
    // Windows NT refuses to delete a key that still has subkeys.
    EXPECT_EQ(m_thunks->reg_delete_key(HKEY_CURRENT_USER, L"Software\\VitreoParent"),
              ERROR_ACCESS_DENIED);
}

TEST_F(Advapi32RegistryTest, DeleteKeyExPerformsDeletionForSupportedViews) {
    constexpr u32 KEY_WOW64_64KEY = 0x00000100u;
    constexpr u32 KEY_WOW64_32KEY = 0x00000200u;

    for (const u32 view : {0u, KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        const std::string key =
            "Software\\VitreoDelEx" + std::to_string(view);
        u64 created = 0;
        ASSERT_EQ(m_thunks->reg_create_key_ex_a(
                      HKEY_CURRENT_USER, key, &created, nullptr),
                  ERROR_SUCCESS);
        ASSERT_EQ(m_thunks->reg_close_key(created), ERROR_SUCCESS);

        EXPECT_EQ(m_thunks->reg_delete_key_ex(
                      HKEY_CURRENT_USER,
                      std::wstring(key.begin(), key.end()), view, 0),
                  ERROR_SUCCESS);
        u64 reopened = 0;
        EXPECT_EQ(m_thunks->reg_open_key_ex_a(
                      HKEY_CURRENT_USER, key, 0, 0, &reopened),
                  ERROR_FILE_NOT_FOUND);
    }
}

TEST_F(Advapi32RegistryTest, DeleteKeyExEmptySubkeyDeletesOpenedKey) {
    constexpr u32 KEY_WOW64_32KEY = 0x00000200u;
    constexpr char path[] = "Software\\VitreoDelOpened";

    u64 opened = 0;
    ASSERT_EQ(m_thunks->reg_create_key_ex_a(
                  HKEY_CURRENT_USER, path, &opened, nullptr),
              ERROR_SUCCESS);
    const u32 value = 7;
    ASSERT_EQ(m_thunks->reg_set_value_ex_a(
                  opened, "Value", REG_DWORD,
                  reinterpret_cast<const u8*>(&value), sizeof(value)),
              ERROR_SUCCESS);

    EXPECT_EQ(m_thunks->reg_delete_key_ex(
                  opened, L"", KEY_WOW64_32KEY, 0),
              ERROR_SUCCESS);
    EXPECT_EQ(m_thunks->reg_close_key(opened), ERROR_SUCCESS);

    u64 reopened = 0;
    EXPECT_EQ(m_thunks->reg_open_key_ex_a(
                  HKEY_CURRENT_USER, path, 0, 0, &reopened),
              ERROR_FILE_NOT_FOUND);
}

TEST_F(Advapi32RegistryTest,
       DeleteKeyExEmptySubkeyRejectsOpenedKeyWithChildren) {
    u64 opened = 0;
    ASSERT_EQ(m_thunks->reg_create_key_ex_a(
                  HKEY_CURRENT_USER,
                  "Software\\VitreoDelOpenedParent\\Child",
                  &opened, nullptr),
              ERROR_SUCCESS);
    ASSERT_EQ(m_thunks->reg_close_key(opened), ERROR_SUCCESS);
    ASSERT_EQ(m_thunks->reg_open_key_ex_a(
                  HKEY_CURRENT_USER, "Software\\VitreoDelOpenedParent",
                  0, 0, &opened),
              ERROR_SUCCESS);

    EXPECT_EQ(m_thunks->reg_delete_key_ex(opened, L"", 0, 0),
              ERROR_ACCESS_DENIED);
    EXPECT_EQ(m_thunks->reg_close_key(opened), ERROR_SUCCESS);
}

TEST_F(Advapi32RegistryTest, DeleteKeyExRejectsInvalidControlArguments) {
    constexpr u32 KEY_WOW64_64KEY = 0x00000100u;
    constexpr u32 KEY_WOW64_32KEY = 0x00000200u;

    EXPECT_EQ(m_thunks->reg_delete_key_ex(
                  HKEY_CURRENT_USER, L"Software\\Unused",
                  KEY_WOW64_64KEY | KEY_WOW64_32KEY, 0),
              ERROR_INVALID_PARAMETER);
    // Access bits beside the view flags are ignored, as KEY_READ is on
    // Windows 11 (wow64_registry_probe delex_read_only): the call reaches the
    // missing key.
    EXPECT_EQ(m_thunks->reg_delete_key_ex(
                  HKEY_CURRENT_USER, L"Software\\Unused", 0x00000001u, 0),
              ERROR_FILE_NOT_FOUND);
    EXPECT_EQ(m_thunks->reg_delete_key_ex(
                  HKEY_CURRENT_USER, L"Software\\Unused", 0, 1),
              ERROR_INVALID_PARAMETER);
}
