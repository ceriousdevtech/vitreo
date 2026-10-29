// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file com_test_memory_stream_fixture.hpp
/// @brief MemoryStreamTest fixture (test_com_core*.cpp). Test-local.

#include "com_test_common.hpp"

class MemoryStreamTest : public ::testing::Test {
protected:
    static constexpr GuestAddr DATA_BASE = 0x00100000;
    static constexpr GuestAddr STACK_BASE = 0x00200000;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(
            DATA_BASE, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(
            STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        marshaler.attach_cpu(cpu);
        ASSERT_TRUE(registry.register_dll(
            "ole32.dll", create_com_core_thunks(manager)).is_ok());
        manager.set_thunk_registry(&registry);
        // CoCreateInstance needs an apartment on the calling thread
        // (measured: CO_E_NOTINITIALIZED otherwise, s7_ole_probe.expected).
        (void)manager.co_initialize();
    }

    void set_args(std::initializer_list<u32> args) {
        const GuestAddr frame = STACK_BASE + 0x400;
        cpu.set_sp(frame);
        ASSERT_TRUE(memory.write_u32(frame, 0xDEADBEEF).is_ok());
        usize index = 0;
        for (u32 value : args) {
            ASSERT_TRUE(memory.write_u32(
                frame + 4 + index * 4, value).is_ok());
            ++index;
        }
    }

    GuestMemory memory;
    HandleTable handles;
    VirtualMemoryManager vmm{memory};
    HeapManager heap_manager{vmm, memory, handles};
    ComTaskMemoryManager task_memory{heap_manager};
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    ComCoreManager manager{task_memory};
    ThunkRegistry registry{0x70000000};
};
