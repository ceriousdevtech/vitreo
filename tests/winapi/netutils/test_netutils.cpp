/// @file test_netutils.cpp
/// @brief netutils.dll NetApiBuffer* functions over the process heap
///        (expected results: tools/oracle_probe/iphlp_netapi_probe).

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/memory/heap.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <winapi/networking/netutils/netutils.hpp>

#include <map>
#include <string>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

class NetUtilsTest : public ::testing::Test {
protected:
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr DATA = 0x1000;
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(DATA, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        marshaler.attach_cpu(cpu);
        mgr.set_heap_manager(&heap);
        exports = create_netutils_thunks(mgr);
    }
    u32 call(const char* name, std::initializer_list<u32> args) {
        usize i = 0;
        for (u32 v : args) (void)memory.write_u32(cpu.gpr.rsp + 4 + 4 * i++, v);
        for (auto& e : exports.entries) if (e.name == name) { e.handler(marshaler); return static_cast<u32>(cpu.gpr.rax); }
        ADD_FAILURE() << name;
        return ~0u;
    }
    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    VirtualMemoryManager vmm{memory};
    HandleTable handles;
    HeapManager heap{vmm, memory, handles};
    NetUtilsManager mgr;
    DllExports exports;
};

}  // namespace

TEST_F(NetUtilsTest, ExportsDeclareStdcallFrames) {
    std::map<std::string, u16> bytes;
    for (const auto& e : exports.entries) bytes[e.name] = e.stdcall_bytes;
    EXPECT_EQ(bytes["NetApiBufferFree"], 4u);
    EXPECT_EQ(bytes["NetApiBufferAllocate"], 8u);
    EXPECT_EQ(bytes["NetApiBufferSize"], 8u);
    EXPECT_EQ(bytes["NetApiBufferReallocate"], 12u);
}

TEST_F(NetUtilsTest, NetApiBufferFreeReleasesHeapBlocksAndAcceptsNull) {
    EXPECT_EQ(call("NetApiBufferFree", {0}), 0u);  // free-null rc=0
    ASSERT_EQ(call("NetApiBufferAllocate", {64, DATA}), 0u);  // alloc rc=0
    const u32 block = memory.read_u32(DATA).value();
    ASSERT_NE(block, 0u);
    ASSERT_EQ(call("NetApiBufferSize", {block, DATA + 4}), 0u);
    EXPECT_GE(memory.read_u32(DATA + 4).value(), 64u);  // size ge64=1
    ASSERT_EQ(call("NetApiBufferReallocate", {block, 128, DATA + 8}), 0u);
    const u32 grown = memory.read_u32(DATA + 8).value();
    ASSERT_EQ(call("NetApiBufferSize", {grown, DATA + 4}), 0u);
    EXPECT_GE(memory.read_u32(DATA + 4).value(), 128u);
    EXPECT_EQ(call("NetApiBufferFree", {grown}), 0u);  // free-allocated rc=0
    EXPECT_EQ(heap.heap_size(heap.get_process_heap(), 0, grown), ~0ULL) << "the block is released";
    EXPECT_NE(call("NetApiBufferSize", {grown, DATA + 4}), 0u);
}
