/// @file test_wkscli.cpp
/// @brief Unit tests for wkscli.dll / netapi32 join and DS role APIs

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/networking/wkscli/wkscli.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/memory/heap.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <algorithm>
#include <string>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class WksCliManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto res = memory_.map_region(MEM_BASE, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(res.is_ok());

        auto stack_res = memory_.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(stack_res.is_ok());

        cpu_.gpr.rsp = STACK_BASE + 0x800;
    }

    static constexpr GuestAddr MEM_BASE   = 0x1000;
    static constexpr GuestAddr STACK_BASE = 0x80000;

    static constexpr GuestAddr NAME_BUF_PTR_ADDR = MEM_BASE + 0x100;
    static constexpr GuestAddr JOIN_STATUS_ADDR  = MEM_BASE + 0x200;
    static constexpr GuestAddr BUFFER_OUT_ADDR   = MEM_BASE + 0x300;
    static constexpr GuestAddr PP_JOIN_ADDR      = MEM_BASE + 0x400;

    GuestMemory memory_;
    cpu::CpuState cpu_;
    WksCliManager mgr_;
};

// =============================================================================
// NetGetJoinInformation Tests
// =============================================================================

TEST_F(WksCliManagerTest, NetGetJoinInformationReturnsWorkgroupNameBuffer) {
    // iphlp_netapi_probe join: rc=0 status=2 name="WORKGROUP" le=0x3e5, the
    // name in a NetApiBufferFree-able buffer; NULL outputs are 87.
    VirtualMemoryManager vmm{memory_};
    HandleTable handles;
    HeapManager heap{vmm, memory_, handles};
    mgr_.set_heap_manager(&heap);
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    cpu_.gpr.rcx = 0;
    cpu_.gpr.rdx = NAME_BUF_PTR_ADDR;
    cpu_.gpr.r8 = JOIN_STATUS_ADDR;
    (void)memory_.write_u32(NAME_BUF_PTR_ADDR, 0xDEADBEEF);
    set_last_error(0xDEAD);
    mgr_.net_get_join_information(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), 997u);
    const u32 name = memory_.read_u32(NAME_BUF_PTR_ADDR).value();
    ASSERT_NE(name, 0u);
    std::string text;
    for (u32 i = 0; i < 16; ++i) { const u16 c = memory_.read_u16(name + i * 2).value(); if (!c) break; text.push_back(static_cast<char>(c)); }
    EXPECT_EQ(text, "WORKGROUP");
    EXPECT_NE(heap.heap_size(heap.get_process_heap(), 0, name), ~0ULL);
    cpu_.gpr.rdx = 0;
    mgr_.net_get_join_information(m);
    EXPECT_EQ(cpu_.gpr.rax, 87u);
    cpu_.gpr.rdx = NAME_BUF_PTR_ADDR;
    cpu_.gpr.r8 = 0;
    mgr_.net_get_join_information(m);
    EXPECT_EQ(cpu_.gpr.rax, 87u);
    mgr_.set_heap_manager(nullptr);
}

TEST_F(WksCliManagerTest, NetGetJoinInformationWritesWorkgroupStatus) {
    VirtualMemoryManager vmm{memory_};
    HandleTable handles;
    HeapManager heap{vmm, memory_, handles};
    mgr_.set_heap_manager(&heap);
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    cpu_.gpr.rcx = 0;
    cpu_.gpr.rdx = NAME_BUF_PTR_ADDR;
    cpu_.gpr.r8 = JOIN_STATUS_ADDR;

    (void)memory_.write_u32(JOIN_STATUS_ADDR, 0xDEADBEEF);

    mgr_.net_get_join_information(m);

    u32 join_status = memory_.read_u32(JOIN_STATUS_ADDR).value();
    EXPECT_EQ(join_status, 2u);  // NetSetupWorkgroupName
}

TEST_F(WksCliManagerTest, NetGetJoinInformationReturnsNerrSuccess) {
    VirtualMemoryManager vmm{memory_};
    HandleTable handles;
    HeapManager heap{vmm, memory_, handles};
    mgr_.set_heap_manager(&heap);
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    cpu_.gpr.rcx = 0;
    cpu_.gpr.rdx = NAME_BUF_PTR_ADDR;
    cpu_.gpr.r8 = JOIN_STATUS_ADDR;

    mgr_.net_get_join_information(m);

    EXPECT_EQ(cpu_.gpr.rax, 0u);  // NERR_Success
}

// =============================================================================
// DsRole / AAD Tests
// =============================================================================

TEST_F(WksCliManagerTest, DsRoleWithoutHeapReturnsNotEnoughMemory) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    cpu_.gpr.rcx = 0;
    cpu_.gpr.rdx = 1; // DsRolePrimaryDomainInfoBasic
    cpu_.gpr.r8 = BUFFER_OUT_ADDR;
    (void)memory_.write_u32(BUFFER_OUT_ADDR, 0xDEADBEEF);

    mgr_.ds_role_get_primary_domain_information(m);

    EXPECT_EQ(cpu_.gpr.rax, 8u); // ERROR_NOT_ENOUGH_MEMORY
    EXPECT_EQ(memory_.read_u32(BUFFER_OUT_ADDR).value(), 0u);
}

TEST_F(WksCliManagerTest, DsRoleLevelsMatchWindows) {
    // iphlp_netapi_probe dsrole: levels 0 and 4 are 87 with *Buffer NULL;
    // level 2 (DSROLE_UPGRADE_STATUS_INFO, 8 bytes) and level 3
    // (DSROLE_OPERATION_STATE_INFO, 4 bytes) succeed with all-zero states; the
    // last error is ERROR_IO_PENDING on every level.
    VirtualMemoryManager vmm{memory_};
    HandleTable handles;
    HeapManager heap{vmm, memory_, handles};
    mgr_.set_heap_manager(&heap);
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    for (u32 level : {0u, 4u, 2u, 3u}) {
        cpu_.gpr.rcx = 0;
        cpu_.gpr.rdx = level;
        cpu_.gpr.r8 = BUFFER_OUT_ADDR;
        (void)memory_.write_u64(BUFFER_OUT_ADDR, 0xDEADBEEF);
        set_last_error(0xDEAD);
        mgr_.ds_role_get_primary_domain_information(m);
        EXPECT_EQ(get_last_error(), 997u) << level;
        const u64 buffer = memory_.read_u64(BUFFER_OUT_ADDR).value();
        if (level == 0 || level == 4) {
            EXPECT_EQ(cpu_.gpr.rax, 87u) << level;
            EXPECT_EQ(buffer, 0u);
            continue;
        }
        ASSERT_EQ(cpu_.gpr.rax, 0u) << level;
        ASSERT_NE(buffer, 0u);
        EXPECT_EQ(heap.heap_size(heap.get_process_heap(), 0, buffer), level == 2 ? 8u : 4u);
        EXPECT_EQ(memory_.read_u32(buffer).value(), 0u);
        if (level == 2) EXPECT_EQ(memory_.read_u32(buffer + 4).value(), 0u);
    }
    cpu_.gpr.r8 = 0;
    mgr_.ds_role_get_primary_domain_information(m);
    EXPECT_EQ(cpu_.gpr.rax, 87u);
    mgr_.set_heap_manager(nullptr);
}

TEST_F(WksCliManagerTest, NetUserGetInfoLevelsAndErrorsMatchWindows) {
    // iphlp_netapi_probe user*: levels 0/1/10/20/23/24 return the account;
    // other undefined levels are ERROR_INVALID_LEVEL; a NULL name is
    // NERR_BadUsername; *bufptr is NULL on failure; last error cleared.
    VirtualMemoryManager vmm{memory_};
    HandleTable handles;
    HeapManager heap{vmm, memory_, handles};
    mgr_.set_heap_manager(&heap);
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    constexpr GuestAddr kName = MEM_BASE + 0x600;
    const char16_t user[] = u"VitUser";
    for (usize i = 0; i < 8; ++i) (void)memory_.write_u16(kName + i * 2, user[i]);
    auto get = [&](GuestAddr name, u32 level) {
        cpu_.gpr.rcx = 0;
        cpu_.gpr.rdx = name;
        cpu_.gpr.r8 = level;
        cpu_.gpr.r9 = BUFFER_OUT_ADDR;
        (void)memory_.write_u64(BUFFER_OUT_ADDR, 0xDEADBEEF);
        set_last_error(0xDEAD);
        mgr_.net_user_get_info(m);
        EXPECT_EQ(get_last_error(), 0u);
        return static_cast<u32>(cpu_.gpr.rax);
    };
    auto text = [&](u64 at) {
        std::string s;
        for (u32 i = 0; i < 64; ++i) { const u16 c = memory_.read_u16(at + i * 2).value_or(0); if (!c) break; s.push_back(static_cast<char>(c)); }
        return s;
    };
    for (u32 level : {0u, 1u, 10u, 20u, 23u}) {
        ASSERT_EQ(get(kName, level), 0u) << level;
        const u64 buffer = memory_.read_u64(BUFFER_OUT_ADDR).value();
        EXPECT_EQ(text(memory_.read_u64(buffer).value()), "VitUser") << level;
    }
    ASSERT_EQ(get(kName, 20), 0u);
    u64 buffer = memory_.read_u64(BUFFER_OUT_ADDR).value();
    EXPECT_GE(memory_.read_u32(buffer + 28).value(), 1000u);  // usri20_user_id (rid_ge_1000)
    ASSERT_EQ(get(kName, 23), 0u);
    buffer = memory_.read_u64(BUFFER_OUT_ADDR).value();
    const u64 sid = memory_.read_u64(buffer + 32).value();
    EXPECT_EQ(memory_.read_u8(sid).value(), 1u);                       // revision
    EXPECT_EQ(memory_.read_u32(sid + 8).value(), 21u);                 // S-1-5-21-...
    ASSERT_EQ(get(kName, 24), 0u);
    buffer = memory_.read_u64(BUFFER_OUT_ADDR).value();
    EXPECT_EQ(memory_.read_u32(buffer).value(), 0u);                   // internet_identity=0
    EXPECT_EQ(memory_.read_u64(buffer + 24).value() != 0, true);       // usri24_user_sid
    for (u32 level : {5u, 6u, 7u, 8u, 9u, 12u, 21u, 22u, 1003u, 1005u}) {
        EXPECT_EQ(get(kName, level), 124u) << level;
        EXPECT_EQ(memory_.read_u64(BUFFER_OUT_ADDR).value(), 0u);
    }
    EXPECT_EQ(get(0, 0), 2202u);
    EXPECT_EQ(memory_.read_u64(BUFFER_OUT_ADDR).value(), 0u);
    constexpr GuestAddr kOther = MEM_BASE + 0x700;
    const char16_t other[] = u"no_such_user_vitreo_7";
    for (usize i = 0; i < 22; ++i) (void)memory_.write_u16(kOther + i * 2, other[i]);
    EXPECT_EQ(get(kOther, 0), 2221u);
    EXPECT_EQ(memory_.read_u64(BUFFER_OUT_ADDR).value(), 0u);
    mgr_.set_heap_manager(nullptr);
}

TEST_F(WksCliManagerTest, NetGetAadJoinInformationReturnsNotJoined) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    cpu_.gpr.rcx = 0;
    cpu_.gpr.rdx = PP_JOIN_ADDR;
    (void)memory_.write_u32(PP_JOIN_ADDR, 0xDEADBEEF);

    mgr_.net_get_aad_join_information(m);

    EXPECT_EQ(cpu_.gpr.rax, 0u); // S_OK
    EXPECT_EQ(memory_.read_u32(PP_JOIN_ADDR).value(), 0u);
}

// =============================================================================
// Thunk Table Tests
// =============================================================================

TEST_F(WksCliManagerTest, ThunksContainExpectedEntries) {
    DllExports exports = create_wkscli_thunks(mgr_);
    EXPECT_EQ(exports.entries.size(), 7u);

    const char* expected[] = {
        "NetGetJoinInformation",
        "DsRoleGetPrimaryDomainInformation",
        "DsRoleFreeMemory",
        "NetGetAadJoinInformation",
        "NetFreeAadJoinInformation",
        "NetUserGetInfo",
        "NetShareEnum",
    };
    for (usize i = 0; i < 7; ++i) {
        EXPECT_EQ(exports.entries[i].name, expected[i]);
    }
}

TEST_F(WksCliManagerTest, NetShareEnumReturnsEmptyList) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    constexpr GuestAddr kBuf = MEM_BASE + 0x500;
    constexpr GuestAddr kRead = MEM_BASE + 0x504;
    constexpr GuestAddr kTotal = MEM_BASE + 0x508;
    cpu_.gpr.rcx = 0;
    cpu_.gpr.rdx = 1;
    cpu_.gpr.r8 = kBuf;
    cpu_.gpr.r9 = 0xFFFFFFFF;
    // Remaining args on stack for x64 shadow — Marshaler arg(4..) from stack.
    // Use the manager entry directly with written arg slots via attach:
    (void)memory_.write_u32(kBuf, 0xDEADBEEF);
    (void)memory_.write_u32(kRead, 0xDEADBEEF);
    (void)memory_.write_u32(kTotal, 0xDEADBEEF);

    // Call through a tiny shim: put ptrs in the expected arg positions by
    // invoking the method after manually setting stack args is awkward; call
    // the manager with a local Marshaler setup that only needs arg_ptr reads
    // for out params — use write via a dedicated path:
    // Re-bind: arg0..3 in regs, push arg4..6.
    cpu_.gpr.rsp -= 0x40;
    (void)memory_.write_u64(cpu_.gpr.rsp + 0x28, kRead);
    (void)memory_.write_u64(cpu_.gpr.rsp + 0x30, kTotal);
    (void)memory_.write_u64(cpu_.gpr.rsp + 0x38, 0);

    mgr_.net_share_enum(m);

    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(memory_.read_u32(kBuf).value(), 0u);
    EXPECT_EQ(memory_.read_u32(kRead).value(), 0u);
    EXPECT_EQ(memory_.read_u32(kTotal).value(), 0u);
}

TEST_F(WksCliManagerTest, ThunkNetGetJoinInformation) {
    DllExports exports = create_wkscli_thunks(mgr_);

    ASSERT_GE(exports.entries.size(), 1u);
    const auto& entry = exports.entries[0];

    EXPECT_EQ(entry.name, "NetGetJoinInformation");
    EXPECT_EQ(entry.ordinal, 1u);
    EXPECT_EQ(entry.stdcall_bytes, 12u);
}
