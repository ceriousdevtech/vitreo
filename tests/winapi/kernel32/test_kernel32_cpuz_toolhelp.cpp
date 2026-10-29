/// @file test_kernel32_cpuz_toolhelp.cpp
/// @brief Unit tests for KERNEL32 Toolhelp32 snapshot and process/module enumeration contracts

#include "kernel32_cpuz_fixture.hpp"
#include <atomic>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/module/module.hpp>
#include <ipc/process_handle.hpp>
#include <algorithm>
#include <thread>

// CreateToolhelp32Snapshot inserts a snapshot (rehashing
// m_toolhelp_snapshots) while Thread32First on another thread looks its own
// snapshot up and advances the cursor stored in it. Overlay hooks and crash
// reporters enumerate from worker threads while the main thread snapshots.
TEST_F(Kernel32CpuzTest, ToolhelpSnapshotsSurviveConcurrentCreateAndWalk) {
    constexpr int kThreads = 8;
    constexpr int kRounds = 1500;
    constexpr u32 TH32CS_SNAPTHREAD = 0x4;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            cpu::CpuState tcpu;
            tcpu.init_protected_mode();
            tcpu.gpr.rsp = STACK_BASE + 0x100 + static_cast<u64>(t) * 0x100;
            Marshaler tm(memory);
            tm.attach_cpu(tcpu);
            const u32 rsp = static_cast<u32>(tcpu.gpr.rsp);
            const GuestAddr entry = DATA_BASE + 0x1000 * static_cast<GuestAddr>(t);
            for (int i = 0; i < kRounds; ++i) {
                (void)memory.write_u32(rsp + 4, TH32CS_SNAPTHREAD);
                (void)memory.write_u32(rsp + 8, 0);
                mgr.impl_CreateToolhelp32Snapshot(tm);
                const u32 snapshot = static_cast<u32>(tcpu.gpr.rax);
                if (snapshot == 0 || snapshot == 0xFFFFFFFFu) { ++failures; continue; }
                // THREADENTRY32.dwSize must be >= 28 on input (Learn).
                (void)memory.write_u32(static_cast<u32>(entry), 28);
                (void)memory.write_u32(rsp + 4, snapshot);
                (void)memory.write_u32(rsp + 8, static_cast<u32>(entry));
                mgr.impl_Thread32First(tm);
                // Either an entry or ERROR_NO_MORE_FILES; an unknown-handle
                // answer means the snapshot table lost our entry.
                if (tcpu.gpr.rax == 0 &&
                    get_last_error() != error_codes::ERROR_NO_MORE_FILES) {
                    ++failures;
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    EXPECT_EQ(failures.load(), 0);
}

// =============================================================================
// Toolhelp32 Snapshot / Process Enumeration
// =============================================================================

TEST_F(Kernel32CpuzTest, CreateToolhelp32Snapshot_ReturnsNonZero) {
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0x2);  // TH32CS_SNAPPROCESS
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 0);    // th32ProcessID
    mgr.impl_CreateToolhelp32Snapshot(marshaler);
    EXPECT_NE(cpu.gpr.rax, 0u);
}

TEST_F(Kernel32CpuzTest, Process32First_ReturnsTRUE) {
    constexpr GuestAddr entry = DATA_BASE + 0x600;
    constexpr u32 process_entry32_size = 296;
    // Zero out enough for PROCESSENTRY32
    for (u32 i = 0; i < 300; i += 4) {
        (void)memory.write_u32(entry + i, 0);
    }
    ASSERT_TRUE(memory.write_u32(entry, process_entry32_size).is_ok());
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0x2);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 0);
    mgr.impl_CreateToolhelp32Snapshot(marshaler);
    const u32 snapshot = static_cast<u32>(cpu.gpr.rax);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), snapshot);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), entry);  // lppe
    mgr.impl_Process32First(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    // Verify PID was written at offset 8
    auto pid = memory.read_u32(entry + 8);
    ASSERT_TRUE(pid.is_ok());
    EXPECT_NE(pid.value(), 0u);
}

TEST_F(Kernel32CpuzTest, Process32Next_ReturnsFALSE) {
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0x2);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 0);
    mgr.impl_CreateToolhelp32Snapshot(marshaler);
    const u32 snapshot = static_cast<u32>(cpu.gpr.rax);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), snapshot);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 0);  // lppe
    mgr.impl_Process32Next(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

TEST_F(Kernel32CpuzTest, Process32NextWEnumeratesPeerProcessFromSnapshot) {
    mgr.set_process_snapshot_provider([=] {
        return std::vector<ipc::ProcessSnapshotEntry>{
            {1200, 1, "Battle.net-Setup.exe",
             "C:\\Vitreo\\Apps\\Win-exe\\Battle.net-Setup.exe"},
            {1201, 1200, "Agent.exe",
             "C:\\ProgramData\\Battle.net\\Agent\\Agent.exe"},
        };
    });

    const GuestAddr stack = cpu.sp();
    constexpr GuestAddr entry = DATA_BASE + 0x600;
    constexpr u32 process_entry32w_size = 556;
    ASSERT_TRUE(memory.write_u32(stack + 4, 0x2).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 0).is_ok());
    mgr.impl_CreateToolhelp32Snapshot(marshaler);
    const u32 snapshot = static_cast<u32>(cpu.gpr.rax);

    ASSERT_TRUE(memory.write_u32(entry, process_entry32w_size).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, snapshot).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, entry).is_ok());
    mgr.impl_Process32FirstW(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(entry + 8).value_or(0), 1200u);
    EXPECT_EQ(read_guest_wide_string(entry + 36), "Battle.net-Setup.exe");

    mgr.impl_Process32NextW(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(entry + 8).value_or(0), 1201u);
    EXPECT_EQ(memory.read_u32(entry + 24).value_or(0), 1200u);
    EXPECT_EQ(read_guest_wide_string(entry + 36), "Agent.exe");

    mgr.impl_Process32NextW(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NO_MORE_FILES);
}

TEST_F(Kernel32CpuzTest, Process32FirstWRejectsUndersizedStructure) {
    constexpr GuestAddr entry = DATA_BASE + 0x600;
    constexpr u32 sentinel = 0xA5A5A5A5u;
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, 0x2).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, 0).is_ok());
    mgr.impl_CreateToolhelp32Snapshot(marshaler);
    const u32 snapshot = static_cast<u32>(cpu.gpr.rax);

    ASSERT_TRUE(memory.write_u32(entry, 104).is_ok());
    ASSERT_TRUE(memory.write_u32(entry + 8, sentinel).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, snapshot).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, entry).is_ok());
    mgr.impl_Process32FirstW(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_BAD_LENGTH);
    EXPECT_EQ(memory.read_u32(entry + 8).value_or(0), sentinel);
}

TEST_F(Kernel32CpuzTest, Module32FirstEnumeratesMainModuleInX64Snapshot) {
    constexpr GuestAddr image_base = 0x0000000140000000ull;
    constexpr u32 image_size = 0x123000;
    constexpr GuestAddr entry = DATA_BASE + 0x600;
    constexpr u32 module_entry32_size = 568;
    ThunkRegistry registry{0x00007FFB00000000ull};
    ModuleManager modules{registry};
    modules.set_main_module(image_base, "C:\\Program Files\\Steam\\steam.exe");
    modules.register_main_module_exports(
        image_base, "C:\\Program Files\\Steam\\steam.exe",
        image_base + image_size, 64, std::span<const u8>{});
    mgr.set_module_manager(&modules);

    cpu.init_long_mode();
    cpu.gpr.rcx = 0x18;  // TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32
    cpu.gpr.rdx = 0;     // current process
    mgr.impl_CreateToolhelp32Snapshot(marshaler);
    const u32 snapshot = static_cast<u32>(cpu.gpr.rax);

    ASSERT_TRUE(memory.write_u32(entry, module_entry32_size).is_ok());
    cpu.gpr.rcx = snapshot;
    cpu.gpr.rdx = entry;
    mgr.impl_Module32First(marshaler);

    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(entry + 4).value_or(0), 1u);
    EXPECT_NE(memory.read_u32(entry + 8).value_or(0), 0u);
    EXPECT_EQ(memory.read_u64(entry + 24).value_or(0), image_base);
    EXPECT_EQ(memory.read_u32(entry + 32).value_or(0), image_size);
    EXPECT_EQ(memory.read_u64(entry + 40).value_or(0), image_base);
    EXPECT_EQ(read_guest_ansi_string(entry + 48), "steam.exe");
    EXPECT_EQ(read_guest_ansi_string(entry + 304),
              "C:\\Program Files\\Steam\\steam.exe");
}

TEST_F(Kernel32CpuzTest, Module32FirstWRequiresNativeStructureSize) {
    constexpr GuestAddr image_base = 0x0000000140000000ull;
    constexpr GuestAddr entry = DATA_BASE + 0x600;
    ThunkRegistry registry{0x00007FFB00000000ull};
    ModuleManager modules{registry};
    modules.set_main_module(image_base, "C:\\test.exe");
    modules.register_main_module_exports(
        image_base, "C:\\test.exe", image_base + 0x10000, 64,
        std::span<const u8>{});
    mgr.set_module_manager(&modules);

    cpu.init_long_mode();
    cpu.gpr.rcx = 0x8;  // TH32CS_SNAPMODULE
    cpu.gpr.rdx = 0;
    mgr.impl_CreateToolhelp32Snapshot(marshaler);
    const u32 snapshot = static_cast<u32>(cpu.gpr.rax);

    ASSERT_TRUE(memory.write_u32(entry, 1064).is_ok()); // x86 W size
    cpu.gpr.rcx = snapshot;
    cpu.gpr.rdx = entry;
    mgr.impl_Module32FirstW(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_BAD_LENGTH);
}

TEST_F(Kernel32CpuzTest, QueryFullProcessImageNameWReturnsRegisteredPeerPath) {
    constexpr u32 peer_pid = 424242;
    constexpr GuestAddr buffer = DATA_BASE + 0x1000;
    constexpr GuestAddr size = DATA_BASE + 0x1800;
    constexpr const char* expected =
        "C:\\Vitreo\\Apps\\Win-exe\\Battle.net-Setup.exe";

    HandleTable handles;
    auto* process = ipc::track_process(static_cast<pid_t>(peer_pid));
    ASSERT_NE(process, nullptr);
    const Handle handle = handles.allocate(HandleType::Process, process);
    mgr.set_handle_table(&handles);
    mgr.set_process_snapshot_provider([] {
        return std::vector<ipc::ProcessSnapshotEntry>{
            {peer_pid, 1, "Battle.net-Setup.exe", expected},
        };
    });

    auto exports = create_kernel32_ext_thunks(mgr);
    auto query = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.name == "QueryFullProcessImageNameW";
        });
    ASSERT_NE(query, exports.entries.end());
    ASSERT_TRUE(memory.write_u32(size, 260).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, handle).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 12, buffer).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 16, size).is_ok());

    query->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(read_guest_wide_string(buffer), expected);
    EXPECT_EQ(memory.read_u32(size).value_or(0), std::strlen(expected));
}

TEST_F(Kernel32CpuzTest, K32GetModuleFileNameExWReturnsPeerMainImagePath) {
    // hModule == NULL must return the process main executable path — not the
    // historical "unknown.exe" stub that broke Battle.net HelperSvc auth.
    constexpr u32 peer_pid = 424243;
    constexpr GuestAddr buffer = DATA_BASE + 0x2000;
    constexpr const char* expected =
        "C:\\Program Files (x86)\\Battle.net\\Battle.net.exe";

    HandleTable handles;
    auto* process = ipc::track_process(static_cast<pid_t>(peer_pid));
    ASSERT_NE(process, nullptr);
    const Handle handle = handles.allocate(HandleType::Process, process);
    mgr.set_handle_table(&handles);
    mgr.set_process_snapshot_provider([] {
        return std::vector<ipc::ProcessSnapshotEntry>{
            {peer_pid, 1, "Battle.net.exe", expected},
        };
    });

    auto exports = create_kernel32_ext_thunks(mgr);
    auto query = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.name == "K32GetModuleFileNameExW";
        });
    ASSERT_NE(query, exports.entries.end());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, handle).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, 0).is_ok()); // hModule NULL
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 12, buffer).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 16, 260).is_ok());

    set_last_error(0xDEADu);
    query->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, std::strlen(expected));
    EXPECT_EQ(read_guest_wide_string(buffer), expected);
    // Success leaves the last error untouched
    // (last_error_on_success_probe.expected "K32GetModuleFileNameExW ok err=0xdead").
    EXPECT_EQ(get_last_error(), 0xDEADu);
}

TEST_F(Kernel32CpuzTest, K32EnumProcessesWritesIdleSystemAndSelf) {
    auto exports = create_kernel32_ext_thunks(mgr);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "K32EnumProcesses"; });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(static_cast<u32>(entry->stdcall_bytes), 12u);

    constexpr GuestAddr ids = DATA_BASE + 0x100;
    constexpr GuestAddr needed = DATA_BASE + 0x200;

    // Oracle (process_contract_probe.expected): NULL + cb=0 succeeds, needed=0,
    // last-error left untouched.
    set_last_error(0xDEADu);
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 12, needed).is_ok());
    ASSERT_TRUE(memory.write_u32(needed, 0x77u).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(needed).value(), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);

    // NULL + cb>0 fails ERROR_NOACCESS (no writable buffer).
    set_last_error(0xDEADu);
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, 64).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NOACCESS);

    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, ids).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    const u32 bytes = memory.read_u32(needed).value();
    EXPECT_GE(bytes, 12u);
    EXPECT_EQ(memory.read_u32(ids).value(), 0u);
    EXPECT_EQ(memory.read_u32(ids + 4).value(), 4u);
}
