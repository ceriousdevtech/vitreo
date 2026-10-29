// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_ntdll_ext_thread_context.cpp
/// @brief Unit tests for ntdll extended functions (WIN32-086): thread CONTEXT, NtCreateThreadEx and NtGetNextThread

#include "ntdll_ext_test_fixture.hpp"

TEST_F(NtdllExtTest, NtContinueRestoresAmd64ContextAndTransfersControl) {
    const auto exports = make_ntdll_ext_exports(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "NtContinue";
        });
    ASSERT_NE(entry, exports.entries.end());

    GuestMemory memory;
    constexpr GuestAddr kContext = 0x20000;
    ASSERT_TRUE(memory.map_region(
        kContext, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u32(kContext + 0x30, 0x0010000Bu).is_ok());
    ASSERT_TRUE(memory.write_u32(kContext + 0x34, 0x1F80u).is_ok());
    ASSERT_TRUE(memory.write_u32(kContext + 0x44, 0x246u).is_ok());
    ASSERT_TRUE(memory.write_u64(kContext + 0x78, 0x1111u).is_ok());
    ASSERT_TRUE(memory.write_u64(kContext + 0x90, 0x2222u).is_ok());
    ASSERT_TRUE(memory.write_u64(kContext + 0x98, 0x30000u).is_ok());
    ASSERT_TRUE(memory.write_u64(kContext + 0xD8, 0x1212u).is_ok());
    ASSERT_TRUE(memory.write_u64(kContext + 0xF8, 0x140001234u).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rcx = kContext;
    cpu.gpr.rdx = 0; // TestAlert = FALSE
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    entry->handler(marshaler);

    EXPECT_TRUE(cpu.thunk_control_transfer);
    EXPECT_EQ(cpu.rip, 0x140001234u);
    EXPECT_EQ(cpu.gpr.rsp, 0x30000u);
    EXPECT_EQ(cpu.gpr.rax, 0x1111u);
    EXPECT_EQ(cpu.gpr.rbx, 0x2222u);
    EXPECT_EQ(cpu.gpr.r12, 0x1212u);
    EXPECT_EQ(cpu.flags.rflags, 0x246u);
}

TEST_F(NtdllExtTest, LdrInitializeThunkContinuesSuppliedAmd64Context) {
    const auto exports = make_ntdll_ext_exports(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "LdrInitializeThunk";
        });
    ASSERT_NE(entry, exports.entries.end());

    GuestMemory memory;
    constexpr GuestAddr kContext = 0x22000;
    ASSERT_TRUE(memory.map_region(
        kContext, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u32(kContext + 0x30, 0x0010000Bu).is_ok());
    ASSERT_TRUE(memory.write_u32(kContext + 0x34, 0x1F80u).is_ok());
    ASSERT_TRUE(memory.write_u32(kContext + 0x44, 0x202u).is_ok());
    ASSERT_TRUE(memory.write_u64(kContext + 0x78, 0x55u).is_ok());
    ASSERT_TRUE(memory.write_u64(kContext + 0x98, 0x33000u).is_ok());
    ASSERT_TRUE(memory.write_u64(kContext + 0xF8, 0x140004321u).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rcx = kContext;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    entry->handler(marshaler);

    EXPECT_TRUE(cpu.thunk_control_transfer);
    EXPECT_EQ(cpu.rip, 0x140004321u);
    EXPECT_EQ(cpu.gpr.rsp, 0x33000u);
    EXPECT_EQ(cpu.gpr.rax, 0x55u);
    EXPECT_EQ(cpu.flags.rflags, 0x202u);
}

TEST_F(NtdllExtTest, NtSetContextThreadUpdatesSuspendedAmd64Thread) {
    HandleTable handles;
    ThreadManager threads(handles);
    mgr.set_thread_manager(&threads);
    const Handle thread = threads.create_thread(
        0, 0x140001000u, 0, CREATE_SUSPENDED, nullptr);
    ASSERT_NE(thread, INVALID_HANDLE);

    GuestMemory memory;
    constexpr GuestAddr kContext = 0x24000;
    ASSERT_TRUE(memory.map_region(
        kContext, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u32(kContext + 0x30, 0x00100003u).is_ok());
    ASSERT_TRUE(memory.write_u32(kContext + 0x44, 0x246u).is_ok());
    ASSERT_TRUE(memory.write_u64(kContext + 0x78, 0x77u).is_ok());
    ASSERT_TRUE(memory.write_u64(kContext + 0x98, 0x35000u).is_ok());
    ASSERT_TRUE(memory.write_u64(kContext + 0xF8, 0x140005678u).is_ok());
    for (GuestAddr offset = 0x80; offset <= 0xF0; offset += 8) {
        if (offset != 0x98) ASSERT_TRUE(memory.write_u64(kContext + offset, 0).is_ok());
    }

    const auto exports = make_ntdll_ext_exports(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "NtSetContextThread";
        });
    ASSERT_NE(entry, exports.entries.end());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rcx = thread;
    cpu.gpr.rdx = kContext;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    entry->handler(marshaler);

    ASSERT_EQ(cpu.gpr.rax, nt_status::STATUS_SUCCESS);
    const GuestThread* target = threads.get_thread_by_handle(thread);
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->cpu.rip, 0x140005678u);
    EXPECT_EQ(target->cpu.gpr.rsp, 0x35000u);
    EXPECT_EQ(target->cpu.gpr.rax, 0x77u);
    // Privileged/reserved bits are sanitised to (value & 0x00210FD5) | 0x200
    // (thread_lifecycle_probe.expected "EFLAGS set=ffffffff readback=210fd5"),
    // which clears reserved bit 1: 0x246 is stored as 0x244.
    EXPECT_EQ(target->cpu.flags.rflags, 0x244u);
}

TEST_F(NtdllExtTest, NtGetContextThreadCapturesRequestedAmd64Groups) {
    HandleTable handles;
    ThreadManager threads(handles);
    mgr.set_thread_manager(&threads);
    const Handle thread = threads.create_thread(
        0, 0x140001000u, 0, CREATE_SUSPENDED, nullptr);
    ASSERT_NE(thread, INVALID_HANDLE);

    GuestThread* target = threads.get_thread_by_handle(thread);
    ASSERT_NE(target, nullptr);
    target->cpu.gpr.rax = 0x1122334455667788u;
    target->cpu.gpr.rsp = 0x35000u;
    target->cpu.rip = 0x140005678u;
    target->cpu.flags.rflags = 0x246u;

    GuestMemory memory;
    constexpr GuestAddr kContext = 0x24000;
    ASSERT_TRUE(memory.map_region(
        kContext, 0x1000, MemoryProtection::ReadWrite).is_ok());
    constexpr u32 kControlAndInteger = 0x00100003u;
    ASSERT_TRUE(memory.write_u32(kContext + 0x30, kControlAndInteger).is_ok());
    ASSERT_TRUE(memory.write_u64(kContext + 0x3A, 0xA5A5A5A5A5A5A5A5u).is_ok());

    const auto exports = make_ntdll_ext_exports(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "NtGetContextThread";
        });
    ASSERT_NE(entry, exports.entries.end());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rcx = thread;
    cpu.gpr.rdx = kContext;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    entry->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, nt_status::STATUS_SUCCESS);
    EXPECT_EQ(memory.read_u32(kContext + 0x30).value(), kControlAndInteger);
    EXPECT_EQ(memory.read_u64(kContext + 0x78).value(), 0x1122334455667788u);
    EXPECT_EQ(memory.read_u64(kContext + 0x98).value(), 0x35000u);
    EXPECT_EQ(memory.read_u64(kContext + 0xF8).value(), 0x140005678u);
    EXPECT_EQ(memory.read_u32(kContext + 0x44).value(), 0x246u);
    // CONTEXT_SEGMENTS was not requested, so DS/ES/FS/GS remain untouched.
    EXPECT_EQ(memory.read_u64(kContext + 0x3A).value(),
              0xA5A5A5A5A5A5A5A5u);
}

TEST_F(NtdllExtTest, ThreadHideFromDebuggerMatchesWindowsBothBitnesses) {
    // Expected statuses, byte counts, output preservation and rights come
    // from tools/oracle_probe/thread_hide{32,64}.expected.
    for (bool wide : {false, true}) {
        SCOPED_TRACE(wide);
        HandleTable handles;
        ThreadManager threads(handles);
        const auto old_tid = ThreadManager::get_current_thread_id_static();
        ThreadManager::set_current_thread_id(threads.main_thread_id());
        mgr.set_handle_table(&handles);
        mgr.set_thread_manager(&threads);
        GuestMemory memory;
        const GuestAddr data = wide ? 0x100020000ull : 0x20000;
        ASSERT_TRUE(memory.map_region(data, 0x2000, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(data + 0x1000);
        Marshaler m(memory);
        m.attach_cpu(cpu);
        const auto exports = make_ntdll_ext_exports(mgr);
        auto call = [&](const char* name, u64 h, GuestAddr out, u32 len,
                        GuestAddr returned) {
            const auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
                [&](const ExportEntry& e) { return e.name == name; });
            EXPECT_NE(entry, exports.entries.end());
            const u64 args[] = {h, 17, out, len, returned};
            if (wide) {
                cpu.gpr.rcx = args[0]; cpu.gpr.rdx = args[1];
                cpu.gpr.r8 = args[2]; cpu.gpr.r9 = args[3];
                (void)memory.write_u64(cpu.sp() + 0x28, args[4]);
            } else {
                for (usize i = 0; i < 5; ++i)
                    (void)memory.write_u32(cpu.sp() + 4 + i * 4, static_cast<u32>(args[i]));
            }
            set_last_error(0x1234);
            entry->handler(m);
            EXPECT_EQ(get_last_error(), 0x1234u);
            return static_cast<u32>(cpu.gpr.rax);
        };
        const char* set = "NtSetInformationThread";
        const char* query = "NtQueryInformationThread";
        const u64 self = wide ? ~u64{1} : 0xfffffffeu;
        (void)memory.write_u32(data, 0xa5a5a5a5);
        (void)memory.write_u32(data + 8, 0xcccccccc);
        EXPECT_EQ(call(query, self, data, 1, data + 8), nt_status::STATUS_SUCCESS);
        EXPECT_EQ(memory.read_u32(data).value(), 0xa5a5a500u);
        EXPECT_EQ(memory.read_u32(data + 8).value(), 1u);
        for (u32 length : {0u, 2u}) {
            (void)memory.write_u32(data, 0xa5a5a5a5);
            (void)memory.write_u32(data + 8, 0xcccccccc);
            EXPECT_EQ(call(query, 0, data, length, data + 8), nt_status::STATUS_INFO_LENGTH_MISMATCH);
            EXPECT_EQ(memory.read_u32(data).value(), 0xa5a5a5a5u);
            EXPECT_EQ(memory.read_u32(data + 8).value(), 0xccccccccu);
        }
        EXPECT_EQ(call(set, 0, 0, 1, 0), nt_status::STATUS_INFO_LENGTH_MISMATCH);
        EXPECT_EQ(call(set, 0, 0, 0, 0), nt_status::STATUS_INVALID_HANDLE);
        EXPECT_EQ(call(query, 0, 0, 1, data + 8), nt_status::STATUS_INVALID_HANDLE);
        const Handle wrong = handles.allocate(HandleType::Event, nullptr);
        EXPECT_EQ(call(set, wrong, 0, 0, 0), nt_status::STATUS_OBJECT_TYPE_MISMATCH);
        EXPECT_EQ(call(query, wrong, data, 1, 0), nt_status::STATUS_OBJECT_TYPE_MISMATCH);
        u32 tid = 0;
        const Handle other = threads.create_thread(0, 0x21000, 0, CREATE_SUSPENDED, &tid);
        for (u32 rights : {0u, ThreadManager::THREAD_SET_INFORMATION,
                           ThreadManager::THREAD_SET_LIMITED_INFORMATION,
                           ThreadManager::THREAD_QUERY_INFORMATION,
                           ThreadManager::THREAD_QUERY_LIMITED_INFORMATION}) {
            const Handle restricted = threads.open_thread(tid, rights);
            EXPECT_EQ(call(set, restricted, 0, 0, 0), rights == ThreadManager::THREAD_SET_INFORMATION
                ? nt_status::STATUS_SUCCESS : nt_status::STATUS_ACCESS_DENIED);
            EXPECT_EQ(call(query, restricted, data, 1, 0), rights == ThreadManager::THREAD_QUERY_INFORMATION
                ? nt_status::STATUS_SUCCESS : nt_status::STATUS_ACCESS_DENIED);
        }
        EXPECT_EQ(call(query, other, data, 1, 0), nt_status::STATUS_SUCCESS);
        EXPECT_EQ(memory.read_u8(data).value(), 1u);
        EXPECT_EQ(call(query, self, data, 1, 0), nt_status::STATUS_SUCCESS);
        EXPECT_EQ(memory.read_u8(data).value(), 0u);
        EXPECT_EQ(call(set, self, 1, 0, 0), nt_status::STATUS_SUCCESS);
        EXPECT_EQ(call(set, self, 0, 0, 0), nt_status::STATUS_SUCCESS);
        EXPECT_EQ(call(query, self, data, 1, 0), nt_status::STATUS_SUCCESS);
        EXPECT_EQ(memory.read_u8(data).value(), 1u);
        (void)memory.write_u8(data, 0xa5);
        EXPECT_EQ(call(query, self, data, 1, 1), nt_status::STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(memory.read_u8(data).value(), 0xa5u);
        EXPECT_EQ(call(query, self, 0, 1, data + 8), nt_status::STATUS_ACCESS_VIOLATION);
        threads.get_thread(tid)->terminated = true;
        EXPECT_EQ(call(set, other, 0, 0, 0), nt_status::STATUS_SUCCESS);
        EXPECT_EQ(call(query, other, data, 1, 0), nt_status::STATUS_SUCCESS);
        EXPECT_EQ(memory.read_u8(data).value(), 1u);
        mgr.set_thread_manager(nullptr);
        mgr.set_handle_table(nullptr);
        ThreadManager::set_current_thread_id(old_tid);
    }
}

TEST_F(NtdllExtTest, NtCreateThreadExCreatesSuspendedCurrentProcessThread) {
    HandleTable handles;
    ThreadManager threads(handles);
    mgr.set_handle_table(&handles);
    mgr.set_thread_manager(&threads);

    GuestMemory memory;
    constexpr GuestAddr kData = 0x20000;
    constexpr GuestAddr kThreadOut = kData;
    constexpr GuestAddr kObjectAttributes = kData + 0x100;
    constexpr GuestAddr kStack = 0x80000;
    constexpr GuestAddr kStart = 0x140001000u;
    constexpr GuestAddr kArgument = 0x12345678u;
    ASSERT_TRUE(memory.map_region(
        kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u32(kObjectAttributes, 48).is_ok());
    ASSERT_TRUE(memory.write_u32(kObjectAttributes + 24, 2).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kStack + 0x400;
    cpu.gpr.rcx = kThreadOut;
    cpu.gpr.rdx = 0x001FFFFFu;
    cpu.gpr.r8 = kObjectAttributes;
    cpu.gpr.r9 = ~u64{0};
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, kStart).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, kArgument).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x38, 7).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x40, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x48, 0x2000).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x50, 0x30000).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x58, 0).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto exports = make_ntdll_ext_exports(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "NtCreateThreadEx";
        });
    ASSERT_NE(entry, exports.entries.end());
    entry->handler(marshaler);

    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), nt_status::STATUS_SUCCESS);
    const Handle handle = static_cast<Handle>(
        memory.read_u64(kThreadOut).value());
    ASSERT_EQ(handles.get_type(handle), HandleType::Thread);
    EXPECT_NE(handles.get_flags(handle) & HandleFlags::Inherit, 0u);
    const GuestThread* thread = threads.get_thread_by_handle(handle);
    ASSERT_NE(thread, nullptr);
    EXPECT_EQ(thread->start_address, kStart);
    EXPECT_EQ(thread->parameter, kArgument);
    // SizeOfStackCommit 0x2000 < MaximumStackSize 0x30000: both kept as given
    // (Learn "Thread Stack Size": only a commit >= the reserve moves it).
    EXPECT_EQ(thread->stack_size, 0x30000u);
    EXPECT_EQ(thread->stack_commit, 0x2000u);
    EXPECT_EQ(thread->suspend_count, 1u);
    EXPECT_FALSE(thread->launched);
    EXPECT_TRUE(thread->hidden_from_debugger);
    // flags 7 includes SKIP_THREAD_ATTACH (0x2): no attach/detach
    // notifications for this thread (skip_thread_attach_probe).
    EXPECT_TRUE(thread->skip_thread_attach);
}

TEST_F(NtdllExtTest, QueryThreadWin32StartAddressReturnsTrackedEntryPoint) {
    HandleTable handles;
    ThreadManager threads(handles);
    mgr.set_handle_table(&handles);
    mgr.set_thread_manager(&threads);

    constexpr GuestAddr kStart = 0x140123000u;
    u32 tid = 0;
    const Handle handle =
        threads.create_thread(0, kStart, 0, CREATE_SUSPENDED, &tid);
    ASSERT_NE(handle, INVALID_HANDLE);

    GuestMemory memory;
    constexpr GuestAddr kData = 0x20000;
    constexpr GuestAddr kStack = 0x80000;
    ASSERT_TRUE(memory.map_region(
        kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kStack + 0x400;
    cpu.gpr.rcx = handle;
    cpu.gpr.rdx = 9; // ThreadQuerySetWin32StartAddress
    cpu.gpr.r8 = kData;
    cpu.gpr.r9 = 8;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, 0).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto exports = make_ntdll_ext_exports(mgr);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "NtQueryInformationThread";
        });
    ASSERT_NE(entry, exports.entries.end());
    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), nt_status::STATUS_SUCCESS);
    EXPECT_EQ(memory.read_u64(kData).value(), kStart);
}

// NtGetNextThread (tools/oracle_probe/s4_ntgetnextthread_probe.expected):
// creation order, STATUS_NO_MORE_ENTRIES at the end, *NewThreadHandle = 0 on
// every failure, Flags != 0 -> STATUS_INVALID_PARAMETER, bad handles ->
// STATUS_INVALID_HANDLE. RtlAcquirePrivilege fails honestly with no state.
TEST_F(NtdllExtTest, NtGetNextThreadWalksThisProcessInCreationOrder) {
    HandleTable handles;
    ThreadManager threads(handles);
    mgr.set_thread_manager(&threads);
    mgr.set_handle_table(&handles);
    u32 first_tid = 0, second_tid = 0;
    ASSERT_NE(threads.create_thread(0, 0x140001000u, 0, CREATE_SUSPENDED, &first_tid),
              INVALID_HANDLE);
    ASSERT_NE(threads.create_thread(0, 0x140001000u, 0, CREATE_SUSPENDED, &second_tid),
              INVALID_HANDLE);

    GuestMemory memory;
    constexpr GuestAddr kOut = 0x24000;
    ASSERT_TRUE(memory.map_region(kOut, 0x1000, MemoryProtection::ReadWrite).is_ok());
    const auto exports = make_ntdll_ext_exports(mgr);
    auto find = [&](std::string_view name) {
        return std::find_if(exports.entries.begin(), exports.entries.end(),
                            [&](const ExportEntry& e) { return e.name == name; });
    };
    const auto entry = find("NtGetNextThread");
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 24u);
    EXPECT_EQ(find("RtlAcquirePrivilege")->stdcall_bytes, 16u);
    EXPECT_EQ(find("RtlReleasePrivilege")->stdcall_bytes, 4u);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = 0x24800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    auto call = [&](u64 process, u64 thread, u32 flags) {
        cpu.gpr.rcx = process;
        cpu.gpr.rdx = thread;
        cpu.gpr.r8 = 0x0800;  // THREAD_QUERY_LIMITED_INFORMATION
        cpu.gpr.r9 = 0;
        (void)memory.write_u64(cpu.gpr.rsp + 0x28, flags);
        (void)memory.write_u64(cpu.gpr.rsp + 0x30, kOut);
        (void)memory.write_u64(kOut, 0x5555);
        entry->handler(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    };
    constexpr u64 kSelf = ~0ull;
    std::vector<u32> seen;
    u64 prev = 0;
    u32 status = 0;
    for (int i = 0; i < 8; ++i) {
        status = call(kSelf, prev, 0);
        if (status != 0) break;
        prev = memory.read_u64(kOut).value();
        const GuestThread* t = threads.get_thread_by_handle(static_cast<Handle>(prev));
        ASSERT_NE(t, nullptr);
        seen.push_back(t->thread_id);
    }
    EXPECT_EQ(status, 0x8000001Au);                     // iterate end_status
    EXPECT_EQ(memory.read_u64(kOut).value(), 0u);       // out_on_end=0
    ASSERT_GE(seen.size(), 2u);
    EXPECT_LT(std::find(seen.begin(), seen.end(), first_tid),
              std::find(seen.begin(), seen.end(), second_tid));
    EXPECT_EQ(call(kSelf, 0, 1), 0xC000000Du);          // flags_1
    EXPECT_EQ(memory.read_u64(kOut).value(), 0u);
    EXPECT_EQ(call(0x1230, 0, 0), 0xC0000008u);         // bad_process
    EXPECT_EQ(call(kSelf, 0x1230, 0), 0xC0000008u);     // bad_thread
    EXPECT_EQ(memory.read_u64(kOut).value(), 0u);
}
