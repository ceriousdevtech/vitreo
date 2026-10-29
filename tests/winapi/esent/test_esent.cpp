/// @file test_esent.cpp
/// @brief esent.dll JET_API stdcall exports — honest errors, no fake engine.

#include <gtest/gtest.h>

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/system/esent/esent.hpp>

#include <array>

using namespace vitreo;
using namespace vitreo::winapi;

class EsentTest : public ::testing::Test {
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
        exports = create_esent_thunks(manager);
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

    u32 eax() const { return static_cast<u32>(cpu.gpr.rax); }

    static u32 jet(i32 err) {
        return static_cast<u32>(static_cast<i32>(err));
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    EsentManager manager;
    DllExports exports;
};

TEST_F(EsentTest, ExportsHaveDocumentedStdcallArities) {
    struct Expected {
        const char* name;
        u16 bytes;
    };
    constexpr Expected expected[] = {
        {"JetCreateInstanceW", 8},
        {"JetSetSystemParameterW", 20},
        {"JetInit", 4},
        {"JetTerm", 4},
        {"JetBeginSessionW", 16},
        {"JetAttachDatabase2W", 16},
        {"JetOpenDatabaseW", 20},
        {"JetOpenTableW", 28},
        {"JetGetTableColumnInfoW", 24},
        {"JetRetrieveColumn", 32},
        {"JetMove", 16},
        {"JetCloseTable", 8},
    };
    for (const auto& item : expected) {
        auto* entry = find_export(item.name);
        ASSERT_NE(entry, nullptr) << item.name;
        EXPECT_EQ(entry->stdcall_bytes, item.bytes) << item.name;
    }
}

TEST_F(EsentTest, CreateInitTermLifecycle) {
    auto* create = find_export("JetCreateInstanceW");
    auto* init = find_export("JetInit");
    auto* term = find_export("JetTerm");
    ASSERT_NE(create, nullptr);
    ASSERT_NE(init, nullptr);
    ASSERT_NE(term, nullptr);

    push_arg(0, 0);
    push_arg(1, 0);
    create->handler(marshaler);
    EXPECT_EQ(eax(), jet(jet_err::InvalidParameter));

    const GuestAddr instance_addr = DATA_BASE + 0x100;
    ASSERT_TRUE(memory.write_u32(instance_addr, 0).is_ok());
    push_arg(0, static_cast<u32>(instance_addr));
    push_arg(1, 0);
    create->handler(marshaler);
    EXPECT_EQ(eax(), jet(jet_err::Success));
    const auto handle_result = memory.read_u32(instance_addr);
    ASSERT_TRUE(handle_result.is_ok());
    const u32 handle = handle_result.value();
    EXPECT_NE(handle, 0u);

    push_arg(0, static_cast<u32>(instance_addr));
    init->handler(marshaler);
    EXPECT_EQ(eax(), jet(jet_err::FeatureNotAvailable));
    EXPECT_NE(eax(), jet(jet_err::Success));

    push_arg(0, handle);
    term->handler(marshaler);
    EXPECT_EQ(eax(), jet(jet_err::Success));

    push_arg(0, handle);
    term->handler(marshaler);
    EXPECT_EQ(eax(), jet(jet_err::InvalidParameter));
}

TEST_F(EsentTest, OpenTableAndRetrieveColumnWithoutInitDoNotFabricateRows) {
    auto* open_table = find_export("JetOpenTableW");
    auto* retrieve = find_export("JetRetrieveColumn");
    ASSERT_NE(open_table, nullptr);
    ASSERT_NE(retrieve, nullptr);

    const GuestAddr tableid_addr = DATA_BASE + 0x200;
    const GuestAddr buffer_addr = DATA_BASE + 0x300;
    const GuestAddr actual_addr = DATA_BASE + 0x400;
    ASSERT_TRUE(memory.write_u32(tableid_addr, 0x11111111u).is_ok());
    std::array<u8, 16> sentinel{};
    sentinel.fill(0xAB);
    ASSERT_TRUE(memory.write_bytes(
        buffer_addr, sentinel.data(), sentinel.size()).is_ok());
    ASSERT_TRUE(memory.write_u32(actual_addr, 0x22222222u).is_ok());

    push_arg(0, 0);
    push_arg(1, 0);
    push_arg(2, 0);
    push_arg(3, 0);
    push_arg(4, 0);
    push_arg(5, 0);
    push_arg(6, static_cast<u32>(tableid_addr));
    open_table->handler(marshaler);
    EXPECT_NE(eax(), jet(jet_err::Success));
    const auto tableid_result = memory.read_u32(tableid_addr);
    ASSERT_TRUE(tableid_result.is_ok());
    EXPECT_EQ(tableid_result.value(), 0x11111111u);

    push_arg(0, 0);
    push_arg(1, 0);
    push_arg(2, 1);
    push_arg(3, static_cast<u32>(buffer_addr));
    push_arg(4, static_cast<u32>(sentinel.size()));
    push_arg(5, static_cast<u32>(actual_addr));
    push_arg(6, 0);
    push_arg(7, 0);
    retrieve->handler(marshaler);
    EXPECT_NE(eax(), jet(jet_err::Success));
    std::array<u8, 16> after{};
    ASSERT_TRUE(memory.read_bytes(buffer_addr, after.data(), after.size()).is_ok());
    EXPECT_EQ(after, sentinel);
    const auto actual_result = memory.read_u32(actual_addr);
    ASSERT_TRUE(actual_result.is_ok());
    EXPECT_EQ(actual_result.value(), 0x22222222u);
}

TEST_F(EsentTest, SetSystemParameterRecordsGlobalMaxInstances) {
    auto* set_param = find_export("JetSetSystemParameterW");
    ASSERT_NE(set_param, nullptr);

    push_arg(0, 0);
    push_arg(1, 0);
    push_arg(2, jet_param::MaxInstances);
    push_arg(3, 4);
    push_arg(4, 0);
    set_param->handler(marshaler);
    EXPECT_EQ(eax(), jet(jet_err::Success));
    auto recorded = manager.global_int_param(jet_param::MaxInstances);
    ASSERT_TRUE(recorded.has_value());
    EXPECT_EQ(*recorded, 4u);

    push_arg(2, 0xFFFFu);
    push_arg(3, 0);
    set_param->handler(marshaler);
    EXPECT_EQ(eax(), jet(jet_err::InvalidParameter));
}
