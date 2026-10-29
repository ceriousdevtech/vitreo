// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_ntdll_ext_exports.cpp
/// @brief Unit tests for ntdll extended functions (WIN32-086): export-table, stdcall cleanup and honest-failure contracts

#include "ntdll_ext_test_fixture.hpp"

TEST_F(NtdllExtTest, ExportsRtlUnwindExForApiSetRtlsupport) {
    // api-ms-win-core-rtlsupport-l1-1-0.dll resolves to ntdll. A missing
    // RtlUnwindEx export becomes a fail-thunk and turns a catchable MSVC
    // C++ throw into an unhandled 0xE06D7363.
    const auto exports = make_ntdll_ext_exports(mgr);
    const auto unwind = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "RtlUnwind";
        });
    const auto unwind_ex = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "RtlUnwindEx";
        });
    ASSERT_NE(unwind, exports.entries.end());
    ASSERT_NE(unwind_ex, exports.entries.end());

    // RtlUnwindEx walks frames from its caller (unwind64.cpp; exercised on
    // real guest code by tools/check_rtl_unwind64_probe.py). Without a module
    // table there is nothing to walk with: it fails loudly with the status
    // Windows raises for a target it cannot reach, rather than leaving an
    // unwind request nothing would ever act on.
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x10000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u64(0x10F00, 0x140002000ull).is_ok());
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.set_sp(0x10F00);
    cpu.gpr.rcx = 0x10F80;
    cpu.gpr.rdx = 0x140001000ull;
    cpu.gpr.r8 = 0;
    cpu.gpr.r9 = 0xABCDull;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    unwind_ex->handler(marshaler);
    EXPECT_FALSE(cpu.pending_unwind.active);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000028u);  // STATUS_BAD_STACK
    EXPECT_EQ(cpu.pending_raise.flags & 1u, 1u);     // noncontinuable
    EXPECT_EQ(cpu.sp(), 0x10F00u);
}

TEST_F(NtdllExtTest, RtlPcToFileHeaderReturnsContainingImageNotMainExe) {
    // Microsoft Learn winnt.h / rtlsupport: RtlPcToFileHeader returns the
    // base of the image that contains PcValue, or NULL if none does.
    const auto exports = make_ntdll_ext_exports(mgr);
    const auto pc_to_header = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "RtlPcToFileHeader";
        });
    ASSERT_NE(pc_to_header, exports.entries.end());

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x10000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    constexpr GuestAddr exe_base = 0x140000000ull;
    constexpr GuestAddr dll_base = 0x180000000ull;
    constexpr GuestAddr base_out = 0x10000;
    ThunkRegistry registry{0x7FFB00000000ull};
    ModuleManager modules{registry};
    modules.set_main_module(exe_base, "C:\\App\\app.exe");
    modules.register_main_module_exports(
        exe_base, "C:\\App\\app.exe", exe_base + 0x200000,
        64, std::span<const u8>{});
    modules.register_main_module_exports(
        dll_base, "C:\\App\\helper.dll", dll_base + 0x100000,
        64, std::span<const u8>{});
    mgr.set_module_manager(&modules);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rcx = dll_base + 0x1234;
    cpu.gpr.rdx = base_out;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    pc_to_header->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, dll_base);
    EXPECT_EQ(memory.read_u64(base_out).value(), dll_base);

    cpu.gpr.rcx = 0x2000;
    pc_to_header->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u64(base_out).value(), 0u);
}

TEST_F(NtdllExtTest, ExportsRtlCaptureStackBackTraceWithStdcallCleanup) {
    const auto exports = make_ntdll_ext_exports(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "RtlCaptureStackBackTrace";
        });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 16u);
}

TEST_F(NtdllExtTest, ExportsDllNotificationFunctionsWithStdcallCleanup) {
    const auto exports = make_ntdll_ext_exports(mgr);
    const auto register_entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "LdrRegisterDllNotification";
        });
    const auto unregister_entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "LdrUnregisterDllNotification";
        });

    ASSERT_NE(register_entry, exports.entries.end());
    ASSERT_NE(unregister_entry, exports.entries.end());
    EXPECT_EQ(register_entry->stdcall_bytes, 16u);
    EXPECT_EQ(unregister_entry->stdcall_bytes, 4u);
}

TEST_F(NtdllExtTest, ExportsLoaderLockFunctionsWithStdcallCleanup) {
    const auto exports = make_ntdll_ext_exports(mgr);
    const auto lock_entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "LdrLockLoaderLock";
        });
    const auto unlock_entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "LdrUnlockLoaderLock";
        });
    ASSERT_NE(lock_entry, exports.entries.end());
    ASSERT_NE(unlock_entry, exports.entries.end());
    EXPECT_EQ(lock_entry->stdcall_bytes, 12u);
    EXPECT_EQ(unlock_entry->stdcall_bytes, 8u);
}

TEST_F(NtdllExtTest, ExportsGrowableFunctionTableFunctions) {
    const auto exports = make_ntdll_ext_exports(mgr);
    const auto add_entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "RtlAddGrowableFunctionTable";
        });
    const auto delete_entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "RtlDeleteGrowableFunctionTable";
        });

    ASSERT_NE(add_entry, exports.entries.end());
    ASSERT_NE(delete_entry, exports.entries.end());
    EXPECT_EQ(add_entry->stdcall_bytes, 24u);
    EXPECT_EQ(delete_entry->stdcall_bytes, 4u);
}

TEST_F(NtdllExtTest, ChromiumSandboxBootstrapServicesAreExportedAndFailHonestly) {
    static constexpr std::array<const char*, 18> required_services{
        "NtOpenThread",
        "NtOpenProcess",
        "NtOpenProcessToken",
        "NtSetInformationThread",
        "NtOpenThreadToken",
        "NtOpenProcessTokenEx",
        "NtOpenThreadTokenEx",
        "NtQueryAttributesFile",
        "NtQueryFullAttributesFile",
        "NtOpenKey",
        "NtOpenKeyEx",
        "NtOpenDirectoryObject",
        "NtQueryDirectoryObject",
        "NtOpenSymbolicLinkObject",
        "NtQuerySymbolicLinkObject",
        "NtTerminateThread",
        "NtSystemDebugControl",
        "ZwSystemDebugControl",
    };

    GuestMemory memory;
    constexpr GuestAddr STACK_BASE = 0x80000;
    ASSERT_TRUE(memory.map_region(
        STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Long;
    cpu.gpr.rsp = STACK_BASE + 0x400;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto exports = make_ntdll_ext_exports(mgr);

    for (const char* required : required_services) {
        auto entry = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [required](const ExportEntry& candidate) {
                return candidate.name == required;
            });
        ASSERT_NE(entry, exports.entries.end()) << required;
        entry->handler(marshaler);
        EXPECT_EQ(
            static_cast<u32>(cpu.gpr.rax),
            nt_status::STATUS_NOT_IMPLEMENTED) << required;
    }

    // NtSuspendThread / NtResumeThread are implemented over the same
    // cooperative suspend machinery as kernel32 SuspendThread; with no thread
    // manager attached they must still fail honestly on the handle rather than
    // report an unimplemented service.
    for (const char* implemented : {"NtSuspendThread", "NtResumeThread"}) {
        auto entry = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [implemented](const ExportEntry& candidate) {
                return candidate.name == implemented;
            });
        ASSERT_NE(entry, exports.entries.end()) << implemented;
        cpu.gpr.rcx = 0x40;  // arbitrary thread handle
        cpu.gpr.rdx = 0;     // no PreviousSuspendCount
        entry->handler(marshaler);
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax),
                  nt_status::STATUS_INVALID_HANDLE) << implemented;
    }
}

TEST_F(NtdllExtTest, DebugBreakpointExportsRaiseStatusBreakpoint) {
    GuestMemory memory;
    constexpr GuestAddr STACK_BASE = 0x80000;
    ASSERT_TRUE(memory.map_region(
        STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Long;
    cpu.gpr.rsp = STACK_BASE + 0x400;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto exports = make_ntdll_ext_exports(mgr);

    for (const char* name : {"DbgBreakPoint", "DbgUserBreakPoint"}) {
        const auto entry = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& candidate) {
                return candidate.name == name;
            });
        ASSERT_NE(entry, exports.entries.end()) << name;
        EXPECT_EQ(entry->stdcall_bytes, 0u) << name;

        cpu.pending_raise.clear();
        entry->handler(marshaler);
        EXPECT_TRUE(cpu.pending_raise.active) << name;
        EXPECT_FALSE(cpu.pending_raise.from_helper) << name;
        EXPECT_EQ(cpu.pending_raise.code, cpu::STATUS_BREAKPOINT) << name;
        EXPECT_EQ(cpu.pending_raise.flags, 0u) << name;
        EXPECT_EQ(cpu.pending_raise.num_args, 0u) << name;
    }
}

// Windows publishes these services, and a guest that derives service numbers
// from the Zw* exports needs every name it resolves. Vitreo does not
// implement them: each fails with STATUS_NOT_IMPLEMENTED and leaves its
// output handle alone. Stdcall bytes are the WoW64 stubs' `ret n`.
TEST_F(NtdllExtTest, PublishesUnimplementedDebugAndProcessServices) {
    const auto exports = make_ntdll_ext_exports(mgr);
    const std::pair<const char*, u16> services[] = {
        {"NtCreateDebugObject", 16},
        {"NtDebugActiveProcess", 8},
        {"NtCreateUserProcess", 44},
    };
    for (const auto& [name, bytes] : services) {
        SCOPED_TRACE(name);
        const auto entry = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [&](const ExportEntry& candidate) { return candidate.name == name; });
        ASSERT_NE(entry, exports.entries.end());
        EXPECT_EQ(entry->stdcall_bytes, bytes);

        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x10000, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.write_u64(0x10800, 0x5A5A5A5A5A5A5A5Aull).is_ok());
        cpu::CpuState cpu;
        cpu.init_long_mode();
        cpu.gpr.rsp = 0x10400;
        cpu.gpr.rcx = 0x10800;
        Marshaler m(memory);
        m.attach_cpu(cpu);
        entry->handler(m);
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0xC0000002u);
        EXPECT_EQ(memory.read_u64(0x10800).value(), 0x5A5A5A5A5A5A5A5Aull);
    }
}
