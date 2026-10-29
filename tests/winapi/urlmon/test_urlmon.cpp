/// @file test_urlmon.cpp
/// @brief urlmon.dll CoInternetCreateSecurityManager + IInternetSecurityManager.

#include <gtest/gtest.h>

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/networking/urlmon/urlmon.hpp>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr u32 S_OK = 0;
constexpr u32 E_NOINTERFACE = 0x80004002u;
constexpr u32 E_POINTER = 0x80004003u;

struct Guid {
    u32 data1;
    u16 data2;
    u16 data3;
    u8 data4[8];
};

constexpr Guid IID_IUNKNOWN{
    0x00000000u, 0x0000u, 0x0000u,
    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr Guid IID_UNKNOWN{
    0x11223344u, 0x5566u, 0x7788u,
    {0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00}};

UrlmonManager::MethodThunks dummy_thunks() {
    UrlmonManager::MethodThunks thunks{};
    thunks.query_interface = 0x7E000010;
    thunks.add_ref = 0x7E000020;
    thunks.release = 0x7E000030;
    thunks.set_security_site = 0x7E000040;
    thunks.get_security_site = 0x7E000050;
    thunks.map_url_to_zone = 0x7E000060;
    thunks.get_security_id = 0x7E000070;
    thunks.process_url_action = 0x7E000080;
    thunks.query_custom_policy = 0x7E000090;
    thunks.set_zone_mapping = 0x7E0000A0;
    thunks.get_zone_mappings = 0x7E0000B0;
    return thunks;
}

}  // namespace

class UrlmonTest : public ::testing::Test {
protected:
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr DATA_BASE = 0x1000;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(
            DATA_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(
            STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        marshaler.attach_cpu(cpu);
        manager.set_method_thunks(dummy_thunks());
        exports = create_urlmon_thunks(manager);
    }

    ExportEntry* find_export(const char* name) {
        for (auto& entry : exports.entries) {
            if (entry.name == name) return &entry;
        }
        return nullptr;
    }

    void push_arg(usize index, u32 value) {
        ASSERT_TRUE(memory.write_u32(
            cpu.gpr.rsp + 4 + index * 4, value).is_ok());
    }

    void write_utf16(GuestAddr address, const char16_t* text) {
        usize bytes = 0;
        while (text[bytes / 2] != 0) bytes += 2;
        bytes += 2;
        ASSERT_TRUE(memory.write_bytes(
            address, reinterpret_cast<const Byte*>(text), bytes).is_ok());
    }

    GuestAddr create_security_manager() {
        const GuestAddr out = DATA_BASE + 0x10;
        EXPECT_TRUE(memory.write_u32(out, 0).is_ok());
        push_arg(0, 0);
        push_arg(1, static_cast<u32>(out));
        push_arg(2, 0);
        auto* entry = find_export("CoInternetCreateSecurityManager");
        EXPECT_NE(entry, nullptr);
        if (entry == nullptr) return 0;
        entry->handler(marshaler);
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), S_OK);
        return memory.read_u32(out).value_or(0);
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    UrlmonManager manager{memory};
    DllExports exports;
};

TEST_F(UrlmonTest, NullPpSmReturnsEPointer) {
    auto* entry = find_export("CoInternetCreateSecurityManager");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 12);

    push_arg(0, 0);
    push_arg(1, 0);
    push_arg(2, 0);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), E_POINTER);
}

TEST_F(UrlmonTest, CreateWritesObjectWithVtable) {
    const GuestAddr object = create_security_manager();
    ASSERT_NE(object, 0u);

    const u32 vtable = memory.read_u32(object).value_or(0);
    ASSERT_NE(vtable, 0u);
    EXPECT_NE(memory.read_u32(vtable + 0 * 4).value_or(0), 0u);
    EXPECT_NE(memory.read_u32(vtable + 1 * 4).value_or(0), 0u);
    EXPECT_NE(memory.read_u32(vtable + 2 * 4).value_or(0), 0u);
    EXPECT_NE(memory.read_u32(vtable + 5 * 4).value_or(0), 0u);
    EXPECT_NE(memory.read_u32(vtable + 7 * 4).value_or(0), 0u);
}

TEST_F(UrlmonTest, MapUrlToZoneHttpsAndFile) {
    const GuestAddr object = create_security_manager();
    ASSERT_NE(object, 0u);
    auto* entry = find_export("_Urlmon_MapUrlToZone");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 16);

    write_utf16(DATA_BASE + 0x100, u"https://example.com");
    ASSERT_TRUE(memory.write_u32(DATA_BASE + 0x200, 0xFFFFFFFFu).is_ok());
    push_arg(0, static_cast<u32>(object));
    push_arg(1, static_cast<u32>(DATA_BASE + 0x100));
    push_arg(2, static_cast<u32>(DATA_BASE + 0x200));
    push_arg(3, 0);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), S_OK);
    EXPECT_EQ(memory.read_u32(DATA_BASE + 0x200).value_or(99), 3u);

    write_utf16(DATA_BASE + 0x100, u"file:///C:/Windows/notepad.exe");
    ASSERT_TRUE(memory.write_u32(DATA_BASE + 0x200, 0xFFFFFFFFu).is_ok());
    push_arg(1, static_cast<u32>(DATA_BASE + 0x100));
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), S_OK);
    EXPECT_EQ(memory.read_u32(DATA_BASE + 0x200).value_or(99), 0u);
}

TEST_F(UrlmonTest, ProcessUrlActionWritesPolicy) {
    const GuestAddr object = create_security_manager();
    ASSERT_NE(object, 0u);
    auto* entry = find_export("_Urlmon_ProcessUrlAction");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 36);

    write_utf16(DATA_BASE + 0x100, u"https://example.com");
    ASSERT_TRUE(memory.write_u32(DATA_BASE + 0x200, 0xFFFFFFFFu).is_ok());
    push_arg(0, static_cast<u32>(object));
    push_arg(1, static_cast<u32>(DATA_BASE + 0x100));
    push_arg(2, 0x1400);  // URLACTION_SCRIPT_RUN
    push_arg(3, static_cast<u32>(DATA_BASE + 0x200));
    push_arg(4, 4);
    push_arg(5, 0);
    push_arg(6, 0);
    push_arg(7, 0);
    push_arg(8, 0);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), S_OK);
    EXPECT_EQ(memory.read_u32(DATA_BASE + 0x200).value_or(0xFFu), 0u);
}

TEST_F(UrlmonTest, QueryInterfaceUnknownIid) {
    const GuestAddr object = create_security_manager();
    ASSERT_NE(object, 0u);
    auto* entry = find_export("_Urlmon_QueryInterface");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 12);

    ASSERT_TRUE(memory.write_bytes(
        DATA_BASE + 0x100, reinterpret_cast<const Byte*>(&IID_UNKNOWN),
        sizeof(IID_UNKNOWN)).is_ok());
    ASSERT_TRUE(memory.write_u32(DATA_BASE + 0x200, 0xDEADBEEFu).is_ok());
    push_arg(0, static_cast<u32>(object));
    push_arg(1, static_cast<u32>(DATA_BASE + 0x100));
    push_arg(2, static_cast<u32>(DATA_BASE + 0x200));
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), E_NOINTERFACE);
    EXPECT_EQ(memory.read_u32(DATA_BASE + 0x200).value_or(0xDEADBEEFu), 0u);

    ASSERT_TRUE(memory.write_bytes(
        DATA_BASE + 0x100, reinterpret_cast<const Byte*>(&IID_IUNKNOWN),
        sizeof(IID_IUNKNOWN)).is_ok());
    ASSERT_TRUE(memory.write_u32(DATA_BASE + 0x200, 0).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), S_OK);
    EXPECT_EQ(memory.read_u32(DATA_BASE + 0x200).value_or(0), object);
}

TEST_F(UrlmonTest, ReleaseToZeroDoesNotCrash) {
    const GuestAddr object = create_security_manager();
    ASSERT_NE(object, 0u);
    auto* entry = find_export("_Urlmon_Release");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 4);

    push_arg(0, static_cast<u32>(object));
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

class UrlmonX64Test : public ::testing::Test {
protected:
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr DATA_BASE = 0x1000;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(
            DATA_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(
            STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu.init_long_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        marshaler.attach_cpu(cpu);
        manager.set_method_thunks(dummy_thunks());
        exports = create_urlmon_thunks(manager);
    }

    ExportEntry* find_export(const char* name) {
        for (auto& entry : exports.entries) {
            if (entry.name == name) return &entry;
        }
        return nullptr;
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    UrlmonManager manager{memory};
    DllExports exports;
};

TEST_F(UrlmonX64Test, CreateWritesPointerSizedObject) {
    auto* entry = find_export("CoInternetCreateSecurityManager");
    ASSERT_NE(entry, nullptr);

    const GuestAddr out = DATA_BASE + 0x10;
    ASSERT_TRUE(memory.write_u64(out, 0xFFFFFFFFFFFFFFFFull).is_ok());
    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = out;
    cpu.gpr.r8 = 0;
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, S_OK);
    const u64 object = memory.read_u64(out).value_or(0);
    EXPECT_NE(object, 0u);
    EXPECT_NE(object, 0xFFFFFFFFFFFFFFFFull);
    EXPECT_NE(memory.read_u64(object).value_or(0), 0u);

    const char16_t https[] = u"https://example.com";
    ASSERT_TRUE(memory.write_bytes(
        DATA_BASE + 0x100, reinterpret_cast<const Byte*>(https),
        sizeof(https)).is_ok());
    ASSERT_TRUE(memory.write_u32(DATA_BASE + 0x200, 0xFFFFFFFFu).is_ok());
    auto* map = find_export("_Urlmon_MapUrlToZone");
    ASSERT_NE(map, nullptr);
    cpu.gpr.rcx = object;
    cpu.gpr.rdx = DATA_BASE + 0x100;
    cpu.gpr.r8 = DATA_BASE + 0x200;
    cpu.gpr.r9 = 0;
    map->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, S_OK);
    EXPECT_EQ(memory.read_u32(DATA_BASE + 0x200).value_or(99), 3u);
}

// CoInternetGetSession is refused (no URL binding engine to honour a
// session's registrations): E_NOTIMPL with the out pointer cleared, for any
// dwSessionMode / dwReserved (Windows ignores both — propsheet_theme_session_
// probe session_mode1 / session_reserved1), E_POINTER for a NULL out pointer.
TEST_F(UrlmonTest, CoInternetGetSessionRefusedAndClearsOutX86) {
    auto* entry = find_export("CoInternetGetSession");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 12);
    const GuestAddr out = DATA_BASE + 0x40;
    for (u32 mode : {0u, 1u}) {
        ASSERT_TRUE(memory.write_u32(out, 0x1234u).is_ok());
        push_arg(0, mode);
        push_arg(1, static_cast<u32>(out));
        push_arg(2, mode);
        entry->handler(marshaler);
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x80004001u);
        EXPECT_EQ(memory.read_u32(out).value(), 0u);
    }
    push_arg(0, 0);
    push_arg(1, 0);
    push_arg(2, 0);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), E_POINTER);
}

TEST_F(UrlmonX64Test, CoInternetGetSessionClearsPointerSizedOut) {
    auto* entry = find_export("CoInternetGetSession");
    ASSERT_NE(entry, nullptr);
    const GuestAddr out = DATA_BASE + 0x40;
    ASSERT_TRUE(memory.write_u64(out, 0xFFFFFFFFFFFFFFFFull).is_ok());
    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = out;
    cpu.gpr.r8 = 0;
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x80004001u);
    EXPECT_EQ(memory.read_u64(out).value(), 0u);
}
