// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file user32_ext_thunk_harness.hpp
/// @brief Shared fixture (User32ExtThunkTest) and include set for the user32_ext
///        contract tests that call the exported thunks directly.

#include <winapi/user32/messages/mouse_in_pointer.hpp>
#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/user32/user32_ext.hpp>
#include <winapi/user32/painting/sys_colors.hpp>
#include <winapi/user32/messages/message_loop.hpp>
#include <winapi/user32/messages/message_time.hpp>
#include <winapi/kernel32/system/time.hpp>
#include <winapi/user32/windows/show_window.hpp>
#include <winapi/user32/painting/painting.hpp>
#include <winapi/user32/messages/message_send.hpp>
#include <winapi/user32/windows/window.hpp>
#include <winapi/user32/windows/def_window_proc.hpp>
#include <winapi/graphics/gdi32/gdi32.hpp>
#include <winapi/user32/resources/resources.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/synthetic_handle_registry.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/memory/global_local.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <winapi/kernel32/process/process.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <capsule/vfs/vfs.hpp>
#include <ipc/hwnd_registry.hpp>
#include <algorithm>
#include <atomic>
#include <vector>
#include <fstream>
#include <map>
#include <array>
#include <tuple>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::windowing;

// =============================================================================
// Test Fixture
// =============================================================================

class User32ExtThunkTest : public ::testing::Test {
protected:
    static constexpr GuestAddr MEM_BASE   = 0x1000;
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr BUF_ADDR   = 0x2000;

    void SetUp() override {
        (void)memory_.map_region(MEM_BASE, 0x10000, MemoryProtection::ReadWrite);
        (void)memory_.map_region(STACK_BASE, 0x10000, MemoryProtection::ReadWrite);

        mgr_ = std::make_unique<User32ExtManager>(
            memory_, vfs_, hwnd_mgr_, class_mgr_, bridge_, queue_);
        mgr_->set_process_manager(&process_mgr_);
        mgr_->set_windowpos_scratch(0xA000);
        cpu_.gpr.rsp = STACK_BASE + 0x800;
    }

    ExportEntry find_export(const std::string& name) {
        DllExports exports = create_user32_ext_thunks(*mgr_);
        for (const auto& e : exports.entries) {
            if (e.name == name) return e;
        }
        ADD_FAILURE() << name << " not found in user32_ext thunks";
        return {};
    }

    void set_fastcall_arg4_6(u64 arg4, u64 arg5, u64 arg6) {
        (void)memory_.write_u64(cpu_.gpr.rsp + 0x28, arg4);
        (void)memory_.write_u64(cpu_.gpr.rsp + 0x30, arg5);
        (void)memory_.write_u64(cpu_.gpr.rsp + 0x38, arg6);
    }

    GuestMemory          memory_;
    capsule::Vfs         vfs_;
    HwndManager          hwnd_mgr_;
    WindowClassManager   class_mgr_;
    CocoaBridge          bridge_;
    MessageQueue         queue_;
    ProcessManager       process_mgr_;
    std::unique_ptr<User32ExtManager> mgr_;
    cpu::CpuState        cpu_;
};
