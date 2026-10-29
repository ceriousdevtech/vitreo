/// @file test_uiautomationcore.cpp
/// @brief UIAutomationCore exports measured against Windows 11
/// (tools/oracle_probe/gfx_media_exports_probe.expected).

#include <gtest/gtest.h>

#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/ui/uiautomationcore/uiautomationcore.hpp"

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

const ExportEntry* uia_export(const DllExports& e, const char* name) {
    for (const auto& x : e.entries) if (x.name == name) return &x;
    return nullptr;
}

} // namespace

TEST(UiAutomationCoreTest, RaiseNotificationEventValidatesOnlyTheProvider) {
    GuestMemory memory;
    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler m(memory);
    m.attach_cpu(cpu);
    const DllExports e = create_uiautomationcore_thunks();
    const ExportEntry* f = uia_export(e, "UiaRaiseNotificationEvent");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->stdcall_bytes, 20u);  // provider, kind, processing, 2 BSTRs

    cpu.gpr.rcx = 0;
    f->handler(m);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x80070057u);  // E_INVALIDARG

    // Any provider, any kind/processing, NULL strings: S_OK (measured).
    cpu.gpr.rcx = 0x10000;
    cpu.gpr.rdx = 99;
    cpu.gpr.r8 = 99;
    cpu.gpr.r9 = 0;
    cpu.gpr.rax = 0xFFFFFFFF;
    f->handler(m);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}
