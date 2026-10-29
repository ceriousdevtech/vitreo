/// @file test_ntdll.cpp
/// @brief Unit tests for ntdll core functions (WIN32-038)

#include <algorithm>
#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/system/ntdll/ntdll.hpp>
#include <winapi/kernel32/handle.hpp>
#include <ipc/process_handle.hpp>
#include <winapi/kernel32/synchronization/critical_section.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <winapi/kernel32/error.hpp>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Fixture
// =============================================================================

class NtdllManagerTest : public ::testing::Test {
protected:
    NtdllManager mgr;
};

TEST_F(NtdllManagerTest, PublishesUserApcDispatcherImageEntryPoint) {
    const auto exports = make_ntdll_exports(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "KiUserApcDispatcher";
        });

    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 0u);
}

TEST_F(NtdllManagerTest, PublishesUserExceptionDispatcherImageEntryPoint) {
    const auto exports = make_ntdll_exports(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "KiUserExceptionDispatcher";
        });

    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 0u);
}

// =============================================================================
// RtlInitUnicodeString Tests
// =============================================================================

TEST_F(NtdllManagerTest, InitUnicodeStringKnownString) {
    // "Hello" — 5 char16_t characters
    const char16_t source[] = u"Hello";
    const u32 guest_addr = 0x1000;

    auto us = mgr.init_unicode_string(source, guest_addr);

    EXPECT_EQ(us.length, 10u);      // 5 chars * 2 bytes
    EXPECT_EQ(us.max_length, 12u);  // length + 2
    EXPECT_EQ(us.buffer, guest_addr);
}

TEST_F(NtdllManagerTest, InitUnicodeStringNullSource) {
    auto us = mgr.init_unicode_string(nullptr, 0x2000);

    EXPECT_EQ(us.length, 0u);
    EXPECT_EQ(us.max_length, 0u);
    EXPECT_EQ(us.buffer, 0u);
}

TEST_F(NtdllManagerTest, InitUnicodeStringEmptyString) {
    const char16_t source[] = u"";
    auto us = mgr.init_unicode_string(source, 0x3000);

    EXPECT_EQ(us.length, 0u);
    EXPECT_EQ(us.max_length, 2u);   // NUL terminator only
    EXPECT_EQ(us.buffer, 0x3000u);
}

TEST_F(NtdllManagerTest, InitUnicodeStringSingleChar) {
    const char16_t source[] = u"A";
    auto us = mgr.init_unicode_string(source, 0x4000);

    EXPECT_EQ(us.length, 2u);       // 1 char * 2 bytes
    EXPECT_EQ(us.max_length, 4u);   // 2 + 2
    EXPECT_EQ(us.buffer, 0x4000u);
}

// =============================================================================
// NtQueryInformationProcess Tests
// =============================================================================

TEST_F(NtdllManagerTest, GetPebAddressReturnsSyntheticPeb) {
    EXPECT_EQ(mgr.get_peb_address(), 0x7FFD0000u);
}

TEST_F(NtdllManagerTest, QueryBasicInfoSuccess) {
    u32 status = mgr.query_basic_info(NtdllManager::PROCESS_BASIC_INFO_SIZE);
    EXPECT_EQ(status, nt_status::STATUS_SUCCESS);
}

TEST_F(NtdllManagerTest, QueryBasicInfoBufferTooSmall) {
    u32 status = mgr.query_basic_info(4);
    EXPECT_EQ(status, nt_status::STATUS_INFO_LENGTH_MISMATCH);
}

TEST_F(NtdllManagerTest, QueryBasicInfoUsesX64LayoutAndTebPeb) {
    constexpr GuestAddr kData = 0x10000;
    constexpr GuestAddr kStack = 0x20000;
    constexpr GuestAddr kTeb = 0x30000;
    constexpr GuestAddr kPeb = 0x00000002'12345000ULL;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        kTeb, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u64(kTeb + 0x60, kPeb).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.seg.gs_desc.base = kTeb;
    cpu.gpr.rsp = kStack + 0x800;
    cpu.gpr.rcx = ~u64{0}; // current process pseudo-handle
    cpu.gpr.rdx = 0;       // ProcessBasicInformation
    cpu.gpr.r8 = kData;
    cpu.gpr.r9 = 48;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, kData + 0x80).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    impl_NtQueryInformationProcess(marshaler, mgr);

    EXPECT_EQ(cpu.gpr.rax, nt_status::STATUS_SUCCESS);
    EXPECT_EQ(memory.read_u32(kData + 0).value(), 0u);
    EXPECT_EQ(memory.read_u32(kData + 4).value(), 0u);
    EXPECT_EQ(memory.read_u64(kData + 8).value(), kPeb);
    EXPECT_EQ(memory.read_u64(kData + 16).value(), 1u);
    EXPECT_EQ(memory.read_u32(kData + 24).value(), 8u);
    EXPECT_NE(memory.read_u64(kData + 32).value(), 0u);
    EXPECT_EQ(memory.read_u64(kData + 40).value(), 0u);
    EXPECT_EQ(memory.read_u32(kData + 0x80).value(), 48u);
}

TEST_F(NtdllManagerTest, QueryBasicInfoReportsCurrentParentPidX86) {
    constexpr GuestAddr kData = 0x10000;
    constexpr GuestAddr kStack = 0x20000;
    constexpr u32 kParentPid = 4242;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(kStack + 0x800);
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 12, kData).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 16, 24).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 20, kData + 0x80).is_ok());
    mgr.set_current_parent_process_id(kParentPid);

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    impl_NtQueryInformationProcess(marshaler, mgr);

    EXPECT_EQ(cpu.gpr.rax, nt_status::STATUS_SUCCESS);
    EXPECT_NE(memory.read_u32(kData + 16).value(), 0u);
    EXPECT_EQ(memory.read_u32(kData + 20).value(), kParentPid);
    EXPECT_EQ(memory.read_u32(kData + 0x80).value(), 24u);
}

TEST_F(NtdllManagerTest, QueryBasicInfoUsesCreatedProcessPebAndPid) {
    constexpr GuestAddr kData = 0x10000;
    constexpr GuestAddr kStack = 0x20000;
    constexpr GuestAddr kPeb = 0x7FFE2000;
    constexpr u64 kImageBase = 0x00000001'40000000ULL;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());

    ipc::ProcessObject process;
    process.pid = 4242;
    process.publish_guest_metadata(
        ipc::GuestProcessMetadata{true, kPeb, kImageBase});
    HandleTable handles;
    const Handle process_handle =
        handles.allocate(HandleType::Process, &process);
    mgr.set_handle_table(&handles);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kStack + 0x800;
    cpu.gpr.rcx = process_handle;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = kData;
    cpu.gpr.r9 = 48;
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x28, kData + 0x80).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    impl_NtQueryInformationProcess(marshaler, mgr);

    EXPECT_EQ(cpu.gpr.rax, nt_status::STATUS_SUCCESS);
    EXPECT_EQ(memory.read_u64(kData + 8).value(), kPeb);
    EXPECT_EQ(memory.read_u64(kData + 32).value(), 4242u);
    EXPECT_EQ(memory.read_u32(kData + 0x80).value(), 48u);
}

TEST_F(NtdllManagerTest, QueryProcessHandleTableReturnsOpenHandles) {
    constexpr GuestAddr kData = 0x10000;
    constexpr GuestAddr kStack = 0x20000;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());

    HandleTable handles;
    const Handle h1 = handles.allocate(HandleType::Event, nullptr);
    const Handle h2 = handles.allocate(HandleType::Mutex, nullptr);
    mgr.set_handle_table(&handles);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kStack + 0x800;
    cpu.gpr.rcx = ~u64{0}; // current process
    cpu.gpr.rdx = 58;      // ProcessHandleTable
    cpu.gpr.r8 = kData;
    cpu.gpr.r9 = 64;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, kData + 0x80).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    impl_NtQueryInformationProcess(marshaler, mgr);

    EXPECT_EQ(cpu.gpr.rax, nt_status::STATUS_SUCCESS);
    EXPECT_EQ(memory.read_u32(kData + 0x80).value(), 8u);
    const u32 a = memory.read_u32(kData + 0).value();
    const u32 b = memory.read_u32(kData + 4).value();
    EXPECT_TRUE((a == h1 && b == h2) || (a == h2 && b == h1));
}

TEST_F(NtdllManagerTest, QueryBasicInfoRejectsUnknownProcessHandle) {
    constexpr GuestAddr kData = 0x10000;
    constexpr GuestAddr kStack = 0x20000;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    HandleTable handles;
    mgr.set_handle_table(&handles);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kStack + 0x800;
    cpu.gpr.rcx = 0x1234;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = kData;
    cpu.gpr.r9 = 48;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, 0).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    impl_NtQueryInformationProcess(marshaler, mgr);

    EXPECT_EQ(cpu.gpr.rax, nt_status::STATUS_INVALID_HANDLE);
}

// =============================================================================
// NTSTATUS Constant Tests
// =============================================================================

TEST_F(NtdllManagerTest, NtStatusConstants) {
    EXPECT_EQ(nt_status::STATUS_SUCCESS,              0x00000000u);
    EXPECT_EQ(nt_status::STATUS_INVALID_HANDLE,       0xC0000008u);
    EXPECT_EQ(nt_status::STATUS_INVALID_INFO_CLASS,   0xC0000003u);
    EXPECT_EQ(nt_status::STATUS_BUFFER_TOO_SMALL,     0xC0000023u);
    EXPECT_EQ(nt_status::STATUS_INFO_LENGTH_MISMATCH, 0xC0000004u);
}

namespace {
#include "../../oracle_data/rtl_utf8_inputs.h"
struct Utf8OracleCase { u32 input, capacity, status, actual; u16 units[8]; };
const Utf8OracleCase utf8_oracle_cases[] = {
#include "rtl_utf8_cases.inc"
};
}

TEST_F(NtdllManagerTest, Utf8ConversionMatchesWindowsBothBitnesses) {
    const auto exports = make_ntdll_exports(mgr);
    const auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "RtlUTF8ToUnicodeN"; });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 20u);
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64);
        GuestMemory memory;
        const GuestAddr base = x64 ? 0x200010000ULL : 0x10000;
        ASSERT_TRUE(memory.map_region(base, 0x2000, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(base + 0x1800);
        Marshaler m(memory); m.attach_cpu(cpu);
        for (const auto& c : utf8_oracle_cases) {
            SCOPED_TRACE(c.input);
            SCOPED_TRACE(c.capacity);
            GuestAddr dest = c.capacity == 14 ? 0 : base, count = base + 32, src = base + 64;
            u32 cap = c.capacity == 14 ? 0 : c.capacity, len = 1;
            for (unsigned i = 0; i < 8; ++i) {
                ASSERT_TRUE(memory.write_u16(base + 2*i, 0xcccc).is_ok());
                const u8 byte = c.input < 0x100 ? inputs[c.input][i] : (i ? 0 : 'A');
                ASSERT_TRUE(memory.write_u8(src + i, byte).is_ok());
            }
            ASSERT_TRUE(memory.write_u32(count, 0xdead).is_ok());
            if (c.input < 0x100) len = lengths[c.input];
            else {
                const u32 mask = c.input - 0x100;
                if (!(mask & 1)) dest = 0;
                if (!(mask & 2)) count = 0;
                if (!(mask & 4)) src = 0;
            }
            const GuestAddr args[] = {dest, cap, count, src, len};
            if (x64) {
                cpu.gpr.rcx = args[0]; cpu.gpr.rdx = args[1];
                cpu.gpr.r8 = args[2]; cpu.gpr.r9 = args[3];
                ASSERT_TRUE(memory.write_u64(cpu.sp() + 0x28, args[4]).is_ok());
            } else for (unsigned i = 0; i < 5; ++i)
                ASSERT_TRUE(memory.write_u32(cpu.sp() + 4 + 4*i, static_cast<u32>(args[i])).is_ok());
            entry->handler(m);
            EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), c.status);
            EXPECT_EQ(memory.read_u32(base + 32).value(), c.actual);
            for (unsigned i = 0; i < 8; ++i) EXPECT_EQ(memory.read_u16(base + 2*i).value(), c.units[i]);
        }
    }
}

// Six states measured by rtl_cs_owner_probe.c, in both x86 and x64.
TEST_F(NtdllManagerTest, CriticalSectionOwnershipMatchesWindowsBothBitnesses) {
    struct RestoreTid {
        u32 saved = ThreadManager::get_current_thread_id_static();
        ~RestoreTid() { ThreadManager::set_current_thread_id(saved); }
    } restore;
    const auto exports = make_ntdll_exports(mgr);
    const auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "RtlIsCriticalSectionLockedByThread"; });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 4u);
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64);
        GuestMemory memory;
        const GuestAddr base = x64 ? 0x200010000ULL : 0x10000;
        ASSERT_TRUE(memory.map_region(base, 0x2000, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(base + 0x1800);
        if (x64) cpu.gpr.rcx = base;
        else ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, static_cast<u32>(base)).is_ok());
        Marshaler m(memory); m.attach_cpu(cpu);
        CriticalSectionManager sections(memory); sections.note_bitness(x64);
        ThreadManager::set_current_thread_id(100);
        auto check = [&](u32 expected) {
            set_last_error(0xdead);
            entry->handler(m);
            EXPECT_EQ(cpu.gpr.rax, expected);
            EXPECT_EQ(get_last_error(), 0xdeadu);
        };
        sections.initialize(base); check(0);
        sections.enter(base); check(1);
        sections.enter(base); check(1);
        ThreadManager::set_current_thread_id(101); check(0);
        ThreadManager::set_current_thread_id(100);
        sections.leave(base); check(1);
        sections.leave(base); check(0);
        sections.delete_cs(base);
    }
}
