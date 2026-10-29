#pragma once
/// @file kernel32_cpuz_fixture.hpp
/// @brief Shared gtest fixture for the Kernel32 CPU-Z contract tests (test_kernel32_cpuz*.cpp)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <capsule/vfs/vfs.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/synchronization/sync.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <winapi/kernel32/system/processor_topology.hpp>
#include "platform/host_hardware_fixtures.hpp"
#include <cstring>
#include <string>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class Kernel32CpuzTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto res = memory.map_region(DATA_BASE, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(res.is_ok());
        auto stack_res = memory.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(stack_res.is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        marshaler.attach_cpu(cpu);
        mgr.set_handle_table(&handles);
        mgr.set_sync_manager(&sync);
        mgr.set_thread_manager(&threads);
    }

    /// Write a wide (UTF-16LE) string to guest memory at the given address.
    void write_guest_wide_string(GuestAddr addr, const char* s) {
        for (u32 i = 0; s[i]; ++i) {
            (void)memory.write_u16(addr + i * 2, static_cast<u16>(s[i]));
        }
        (void)memory.write_u16(addr + static_cast<u32>(std::strlen(s)) * 2, 0);
    }

    /// Read a null-terminated ANSI string from guest memory.
    std::string read_guest_ansi_string(GuestAddr addr, u32 max_len = 260) {
        std::string result;
        for (u32 i = 0; i < max_len; ++i) {
            auto r = memory.read_u8(addr + i);
            if (r.is_err() || r.value() == 0) break;
            result.push_back(static_cast<char>(r.value()));
        }
        return result;
    }

    std::string read_guest_wide_string(GuestAddr addr, u32 max_len = 260) {
        std::string result;
        for (u32 i = 0; i < max_len; ++i) {
            auto r = memory.read_u16(addr + i * 2);
            if (r.is_err() || r.value() == 0) break;
            result.push_back(static_cast<char>(r.value()));
        }
        return result;
    }

    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr DATA_BASE  = 0x1000;

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    capsule::Vfs vfs;
    HandleTable handles;
    SyncManager sync{handles};
    ThreadManager threads{handles};
    Kernel32ExtManager mgr{memory, vfs};
};
