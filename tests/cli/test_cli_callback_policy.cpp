/// @file test_cli_callback_policy.cpp
/// @brief run_guest_callback execution-policy and callback-frame tests.

#include <gtest/gtest.h>
#include "runtime/execution/guest_run_loop.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/console/console.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include <array>
#include <thread>
#include <vector>

using namespace vitreo::cli;

TEST(Seh64CallbackFrameTest, NoInterruptedFrameOutsideCallback) {
    Seh64HostCallbackFrame frame{};
    EXPECT_FALSE(seh64_interrupted_guest_frame(&frame));
}

TEST(Seh64CallbackFrameTest, ProcessSentinelIsDistinctFromCallbackSentinel) {
    // 0xDEADC0DEFFFF0000 is the process entry return, not a nested callback.
    EXPECT_EQ(PROCESS_RETURN_SENTINEL, 0xDEADC0DEFFFF0000ull);
    EXPECT_EQ(CALLBACK_RETURN_SENTINEL, 0xDEADC0DEDEADC0DEull);
    EXPECT_NE(PROCESS_RETURN_SENTINEL, CALLBACK_RETURN_SENTINEL);
}

TEST(CallbackExecutionPolicyTest, X86CallbacksUseNativeJitByDefault) {
    EXPECT_TRUE(native_callback_jit_default(
        false, GuestCallbackKind::Nested));
    EXPECT_TRUE(native_callback_jit_default(
        false, GuestCallbackKind::ThreadEntry));
}

TEST(CallbackExecutionPolicyTest, X64ThreadEntriesUseNativeJitByDefault) {
    EXPECT_TRUE(native_callback_jit_default(
        true, GuestCallbackKind::ThreadEntry));
}

TEST(CallbackExecutionPolicyTest, NestedX64CallbacksUseNativeJitByDefault) {
    EXPECT_TRUE(native_callback_jit_default(
        true, GuestCallbackKind::Nested));
}

TEST(CallbackExecutionPolicyTest, MultimediaTimerProceduresUseInterpreter) {
    EXPECT_FALSE(native_callback_jit_default(
        false, GuestCallbackKind::TimerProcedure));
    EXPECT_FALSE(native_callback_jit_default(
        true, GuestCallbackKind::TimerProcedure));
}

TEST(CallbackExecutionPolicyTest, StartupJitMatchesInterpreterBeforeEntryLoop) {
    // Loader callbacks execute before run_execution_loop publishes its cache.
    // Exercise the fresh-thread engine with a hot loop and observable memory
    // output, then compare with the uninitialized interpreter reference.
    for (bool initialize_jit : {false, true}) {
        SCOPED_TRACE(initialize_jit);
        vitreo::GuestMemory memory;
        ASSERT_TRUE(memory.map_region(
            0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());
        const vitreo::u8 code[] = {
            0x31, 0xc0,             // xor eax,eax
            0x01, 0xc8,             // add eax,ecx
            0x83, 0xe9, 0x01,       // sub ecx,1
            0x75, 0xf9,             // jne loop
            0x41, 0x89, 0x00,       // mov [r8],eax
            0xc3,
        };
        ASSERT_TRUE(memory.write_bytes(0x21000, code, sizeof(code)).is_ok());
        vitreo::cpu::CpuState cpu;
        cpu.init_long_mode();
        cpu.set_sp(0x2fff0);
        cpu.rip = 0x22000;
        vitreo::winapi::ProcessManager process_mgr;
        vitreo::winapi::ThunkRegistry registry(0x70000000);
        const vitreo::u64 args[] = {1000, 0, 0x23000};
        bool completed = false;
        vitreo::u64 result = 0;
        std::thread primary([&] {
            if (initialize_jit) init_thread_jit_state(memory);
            result = run_guest_callback(cpu, memory, registry, process_mgr,
                0x21000, args, 3, true, &completed);
        });
        primary.join();
        EXPECT_TRUE(completed);
        EXPECT_EQ(result, 500500u);
        ASSERT_TRUE(memory.read_u32(0x23000).is_ok());
        EXPECT_EQ(memory.read_u32(0x23000).value(), 500500u);
        EXPECT_EQ(cpu.sp(), 0x2fff0u);
        EXPECT_EQ(cpu.rip, 0x22000u);
    }
}

TEST(CallbackExecutionPolicyTest,
     ScatteredMemoryThreadEntryCallToImportStubSkipsNopSled) {
    // CreateThread workers run via run_guest_callback. A CALL through the IAT
    // lands on the 8-byte NOP entry stub, not the dispatch gateway. The
    // callback loop must skip an unmodified sled the same way the main loop
    // does; otherwise every import is eight interpreter steps.
    constexpr vitreo::GuestAddr code_addr = 0x28000;
    constexpr vitreo::GuestAddr thunk_base = 0x70000;

    vitreo::GuestMemory memory;
    ASSERT_FALSE(memory.is_linear());
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        thunk_base, 0x1000, vitreo::MemoryProtection::ReadWrite).is_ok());

    bool handler_called = false;
    vitreo::winapi::ThunkRegistry registry(thunk_base);
    vitreo::winapi::DllExports exports;
    exports.entries.push_back(vitreo::winapi::ExportEntry{
        "ObserveStubDispatch", 1,
        [&](vitreo::winapi::Marshaler& marshaler) {
            handler_called = true;
            marshaler.set_return(42);
        }});
    ASSERT_TRUE(registry.register_dll("test.dll", std::move(exports)).is_ok());
    auto entry = registry.get_thunk_address("test.dll", "ObserveStubDispatch");
    ASSERT_TRUE(entry.is_ok());
    ASSERT_TRUE(registry.is_stub_address(entry.value()));

    const vitreo::u8 nops[vitreo::winapi::ThunkRegistry::STUB_SIZE] = {
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    ASSERT_TRUE(memory.write_bytes(
        entry.value(), nops, sizeof(nops)).is_ok());

    const auto rel32 = static_cast<vitreo::u32>(
        static_cast<vitreo::i32>(entry.value() - (code_addr + 5)));
    const vitreo::u8 code[] = {
        0xE8,
        static_cast<vitreo::u8>(rel32),
        static_cast<vitreo::u8>(rel32 >> 8),
        static_cast<vitreo::u8>(rel32 >> 16),
        static_cast<vitreo::u8>(rel32 >> 24),
        0xC3,
    };
    ASSERT_TRUE(memory.write_bytes(code_addr, code, sizeof(code)).is_ok());

    vitreo::cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(0x2fff0);
    vitreo::winapi::ProcessManager process_mgr;
    bool completed = false;
    const vitreo::u64 eax = run_guest_callback(
        cpu, memory, registry, process_mgr, code_addr,
        nullptr, 0, false, &completed,
        GuestCallbackKind::ThreadEntry);

    EXPECT_TRUE(handler_called);
    EXPECT_EQ(eax, 42u);
    EXPECT_TRUE(completed);
}

namespace {

// Calls an import whose 8-byte entry stub was rewritten to `jmp <target>`
// (as guest_accel does for CRT exports). The JMP's target (next instruction
// + sign-extended displacement, SDM Vol. 2A "JMP") is a guest routine that
// returns 7; the registered host handler must never run.
void expect_callback_follows_patched_stub_jump(bool short_form) {
    constexpr vitreo::GuestAddr code_addr = 0x28000;
    constexpr vitreo::GuestAddr target_addr = 0x70040;
    constexpr vitreo::GuestAddr thunk_base = 0x70000;

    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        thunk_base, 0x1000, vitreo::MemoryProtection::ReadWrite).is_ok());

    bool handler_called = false;
    vitreo::winapi::ThunkRegistry registry(thunk_base);
    vitreo::winapi::DllExports exports;
    exports.entries.push_back(vitreo::winapi::ExportEntry{
        "PatchedExport", 1,
        [&](vitreo::winapi::Marshaler& marshaler) {
            handler_called = true;
            marshaler.set_return(42);
        }});
    ASSERT_TRUE(registry.register_dll("test.dll", std::move(exports)).is_ok());
    auto entry = registry.get_thunk_address("test.dll", "PatchedExport");
    ASSERT_TRUE(entry.is_ok());
    ASSERT_TRUE(registry.is_stub_address(entry.value()));
    ASSERT_LT(entry.value(), target_addr - 0x10);

    vitreo::u8 stub[vitreo::winapi::ThunkRegistry::STUB_SIZE] = {
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    if (short_form) {
        stub[0] = 0xEB;
        stub[1] = static_cast<vitreo::u8>(
            static_cast<vitreo::i8>(target_addr - (entry.value() + 2)));
    } else {
        const auto disp = static_cast<vitreo::u32>(
            static_cast<vitreo::i32>(target_addr - (entry.value() + 5)));
        stub[0] = 0xE9;
        stub[1] = static_cast<vitreo::u8>(disp);
        stub[2] = static_cast<vitreo::u8>(disp >> 8);
        stub[3] = static_cast<vitreo::u8>(disp >> 16);
        stub[4] = static_cast<vitreo::u8>(disp >> 24);
    }
    ASSERT_TRUE(memory.write_bytes(entry.value(), stub, sizeof(stub)).is_ok());

    const vitreo::u8 target[] = {0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3};
    ASSERT_TRUE(memory.write_bytes(target_addr, target, sizeof(target)).is_ok());

    const auto rel32 = static_cast<vitreo::u32>(
        static_cast<vitreo::i32>(entry.value() - (code_addr + 5)));
    const vitreo::u8 code[] = {
        0xE8,
        static_cast<vitreo::u8>(rel32),
        static_cast<vitreo::u8>(rel32 >> 8),
        static_cast<vitreo::u8>(rel32 >> 16),
        static_cast<vitreo::u8>(rel32 >> 24),
        0xC3,
    };
    ASSERT_TRUE(memory.write_bytes(code_addr, code, sizeof(code)).is_ok());

    vitreo::cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(0x2fff0);
    vitreo::winapi::ProcessManager process_mgr;
    bool completed = false;
    const vitreo::u64 eax = run_guest_callback(
        cpu, memory, registry, process_mgr, code_addr,
        nullptr, 0, false, &completed,
        GuestCallbackKind::ThreadEntry);

    EXPECT_FALSE(handler_called);
    EXPECT_EQ(eax, 7u);
    EXPECT_TRUE(completed);
}

}  // namespace

TEST(CallbackExecutionPolicyTest, CallToStubPatchedWithJmpRel32ReachesTarget) {
    expect_callback_follows_patched_stub_jump(false);
}

TEST(CallbackExecutionPolicyTest, CallToStubPatchedWithJmpRel8ReachesTarget) {
    expect_callback_follows_patched_stub_jump(true);
}

TEST(CallbackExecutionPolicyTest, WorkerCallbackPublishesItsOwnCpuState) {
    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());

    vitreo::cpu::CpuState main_cpu;
    main_cpu.init_protected_mode();
    vitreo::cpu::CpuState worker_cpu;
    worker_cpu.init_protected_mode();
    worker_cpu.set_sp(0x2fff0);

    vitreo::winapi::ProcessManager process_mgr;
    vitreo::winapi::ThunkRegistry registry(0x70000000);
    vitreo::cpu::CpuState* observed_active = nullptr;
    vitreo::cpu::CpuState* observed_selected = nullptr;

    vitreo::winapi::DllExports exports;
    exports.entries.push_back(vitreo::winapi::ExportEntry{
        "ObserveActiveCpu", 1,
        [&](vitreo::winapi::Marshaler& marshaler) {
            observed_active = active_guest_cpu();
            observed_selected = &active_guest_cpu_or(main_cpu);
            EXPECT_EQ(observed_active, marshaler.cpu());
            marshaler.set_return(42);
        }});
    ASSERT_TRUE(registry.register_dll("test.dll", std::move(exports)).is_ok());
    auto entry = registry.get_thunk_address("test.dll", "ObserveActiveCpu");
    ASSERT_TRUE(entry.is_ok());

    vitreo::u64 result = 0;
    bool worker_cleared_active = false;
    std::thread worker([&] {
        result = run_guest_callback(
            worker_cpu, memory, registry, process_mgr,
            entry.value() + vitreo::winapi::ThunkRegistry::STUB_SIZE,
            nullptr, 0, false);
        worker_cleared_active = active_guest_cpu() == nullptr;
    });
    worker.join();

    EXPECT_EQ(result, 42u);
    EXPECT_EQ(observed_active, &worker_cpu);
    EXPECT_EQ(observed_selected, &worker_cpu);
    EXPECT_TRUE(worker_cleared_active);
    EXPECT_EQ(active_guest_cpu(), nullptr);
}

TEST(CallbackExecutionPolicyTest,
     X86StdcallCleanupPreservesSavedRegistersAroundConsoleTitleCall) {
    constexpr vitreo::GuestAddr code_addr = 0x21000;
    constexpr vitreo::GuestAddr title_addr = 0x22000;
    constexpr vitreo::GuestAddr output_addr = 0x22100;

    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u16(title_addr + 0, u'S').is_ok());
    ASSERT_TRUE(memory.write_u16(title_addr + 2, u't').is_ok());
    ASSERT_TRUE(memory.write_u16(title_addr + 4, u'e').is_ok());
    ASSERT_TRUE(memory.write_u16(title_addr + 6, u'a').is_ok());
    ASSERT_TRUE(memory.write_u16(title_addr + 8, u'm').is_ok());
    ASSERT_TRUE(memory.write_u16(title_addr + 10, 0).is_ok());

    vitreo::winapi::HandleTable handles;
    vitreo::winapi::ConsoleManager console(handles);
    vitreo::winapi::ThunkRegistry registry(0x70000000);
    ASSERT_TRUE(registry.register_dll(
        "kernel32.dll",
        vitreo::winapi::make_console_exports(console)).is_ok());
    auto set_title = registry.get_thunk_address(
        "kernel32.dll", "SetConsoleTitleW");
    ASSERT_TRUE(set_title.is_ok());
    const vitreo::u32 gateway = static_cast<vitreo::u32>(
        set_title.value() + vitreo::winapi::ThunkRegistry::STUB_SIZE);

    std::vector<vitreo::u8> code;
    auto emit_u32 = [&](vitreo::u32 value) {
        code.push_back(static_cast<vitreo::u8>(value));
        code.push_back(static_cast<vitreo::u8>(value >> 8));
        code.push_back(static_cast<vitreo::u8>(value >> 16));
        code.push_back(static_cast<vitreo::u8>(value >> 24));
    };
    auto emit_mov_imm = [&](vitreo::u8 opcode, vitreo::u32 value) {
        code.push_back(opcode);
        emit_u32(value);
    };
    auto emit_store_abs = [&](vitreo::u8 modrm, vitreo::u32 address) {
        code.push_back(0x89);
        code.push_back(modrm);
        emit_u32(address);
    };

    code.insert(code.end(), {0x55, 0x8B, 0xEC}); // push ebp; mov ebp,esp
    emit_mov_imm(0xBB, 0x11111111);              // mov ebx,imm32
    emit_mov_imm(0xBE, 0x22222222);              // mov esi,imm32
    emit_mov_imm(0xBF, 0x33333333);              // mov edi,imm32
    code.insert(code.end(), {0x53, 0x56, 0x57}); // save ebx,esi,edi
    code.push_back(0x68);                         // push title_addr
    emit_u32(static_cast<vitreo::u32>(title_addr));
    emit_mov_imm(0xB8, gateway);                  // mov eax,thunk gateway
    code.insert(code.end(), {0xFF, 0xD0});       // call eax
    code.push_back(0x5F);                         // pop edi
    emit_store_abs(0x3D, output_addr + 0);        // mov [abs],edi
    code.push_back(0x5E);                         // pop esi
    emit_store_abs(0x35, output_addr + 4);        // mov [abs],esi
    code.push_back(0x5B);                         // pop ebx
    emit_store_abs(0x1D, output_addr + 8);        // mov [abs],ebx
    code.insert(code.end(), {
        0x8B, 0xE5,                               // mov esp,ebp
        0x5D,                                     // pop ebp
        0xB8, 0x01, 0x00, 0x00, 0x00,             // mov eax,1
        0xC3                                      // ret
    });
    ASSERT_TRUE(memory.write_bytes(
        code_addr, code.data(), code.size()).is_ok());

    vitreo::cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(0x2fff0);
    vitreo::winapi::ProcessManager process_mgr;
    bool completed = false;
    EXPECT_EQ(run_guest_callback(
                  cpu, memory, registry, process_mgr, code_addr,
                  nullptr, 0, false, &completed),
              1u);
    EXPECT_TRUE(completed);
    EXPECT_EQ(memory.read_u32(output_addr + 0).value_or(0), 0x33333333u);
    EXPECT_EQ(memory.read_u32(output_addr + 4).value_or(0), 0x22222222u);
    EXPECT_EQ(memory.read_u32(output_addr + 8).value_or(0), 0x11111111u);
}

TEST(CallbackExecutionPolicyTest,
     X64CallbacksPlaceArgumentsAfterFourBeyondShadowSpace) {
    constexpr vitreo::GuestAddr code_addr = 0x21000;
    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());
    // mov rax,[rsp+28h]; add rax,[rsp+30h]; ret
    const std::array<vitreo::u8, 11> code{
        0x48, 0x8b, 0x44, 0x24, 0x28,
        0x48, 0x03, 0x44, 0x24, 0x30,
        0xc3,
    };
    ASSERT_TRUE(memory.write_bytes(
        code_addr, code.data(), code.size()).is_ok());

    vitreo::cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.set_sp(0x2fff0);
    vitreo::winapi::ProcessManager process_mgr;
    vitreo::winapi::ThunkRegistry registry(0x70000000);
    const std::array<vitreo::u64, 6> args{1, 2, 3, 4, 5, 6};
    bool completed = false;
    EXPECT_EQ(run_guest_callback(
                  cpu, memory, registry, process_mgr, code_addr,
                  args.data(), args.size(), true, &completed),
              11u);
    EXPECT_TRUE(completed);
}
