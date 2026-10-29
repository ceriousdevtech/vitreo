/// @file test_cli_callback_exceptions.cpp
/// @brief Exception/VEH dispatch and unhandled-fault termination through the run loop and run_guest_callback.

#include <gtest/gtest.h>
#include "runtime/execution/guest_run_loop.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/process/vectored_exception.hpp"
#include "winapi/core/thunk_registry.hpp"
#include <array>
#include <string>
#include <vector>

using namespace vitreo::cli;

TEST(CallbackExceptionDispatchTest,
     X86HardwareFaultsInvokeVectoredHandlerWithNoFrameInBothLoops) {
    // Native pmovmskb_encoding_probe confirms #UD can resume through VEH.
    // The AV case additionally covers the hardware exception parameter path.
    for (bool callback : {false, true}) {
        for (bool invalid_opcode : {false, true}) {
            constexpr vitreo::GuestAddr code_addr = 0x21000;
            const vitreo::GuestAddr resume_addr = code_addr + (invalid_opcode ? 3 : 5);
            constexpr vitreo::GuestAddr handler_addr = 0x21100;
            vitreo::GuestMemory memory;
            ASSERT_TRUE(memory.map_region(
                0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());

            // mov eax,[0] ; mov eax,42 ; ret
            std::vector<vitreo::u8> code{
                0xa1, 0x00, 0x00, 0x00, 0x00,
                0xb8, 0x2a, 0x00, 0x00, 0x00,
                0xc3,
            };
            if (invalid_opcode) {
                code = {0x0f, 0xd7, 0x01, 0xb8, 0x2a, 0, 0, 0, 0xc3};
            }
            ASSERT_TRUE(memory.write_bytes(
                code_addr, code.data(), code.size()).is_ok());

            // LONG CALLBACK Veh(EXCEPTION_POINTERS* ep):
            //   ep->ContextRecord->Eip = resume_addr;
            //   return EXCEPTION_CONTINUE_EXECUTION;
            std::array<vitreo::u8, 23> handler{
                0x8b, 0x44, 0x24, 0x04,             // mov eax,[esp+4]
                0x8b, 0x40, 0x04,                   // mov eax,[eax+4]
                0xc7, 0x80, 0xb8, 0x00, 0x00, 0x00, // mov [eax+CTX32_EIP],imm32
                static_cast<vitreo::u8>(resume_addr),
                static_cast<vitreo::u8>(resume_addr >> 8),
                static_cast<vitreo::u8>(resume_addr >> 16),
                static_cast<vitreo::u8>(resume_addr >> 24),
                0xb8, 0xff, 0xff, 0xff, 0xff,       // mov eax,-1
                0xc3,
            };
            ASSERT_TRUE(memory.write_bytes(
                handler_addr, handler.data(), handler.size()).is_ok());

            vitreo::winapi::clear_vectored_exception_handlers_for_test();
            const vitreo::u64 veh =
                vitreo::winapi::add_vectored_exception_handler(1, handler_addr);
            ASSERT_NE(veh, 0u);

            vitreo::cpu::CpuState cpu;
            cpu.init_protected_mode();
            cpu.set_sp(0x2fff0);
            // An empty frame chain is the discriminator: VEH must still run.
            cpu.seg.fs_desc.base = 0x20000;
            ASSERT_TRUE(memory.write_u32(0x20000, 0xffffffffu).is_ok());
            vitreo::winapi::ProcessManager process_mgr;
            vitreo::winapi::ThunkRegistry registry(0x70000000);
            if (callback) {
                bool completed = false;
                EXPECT_EQ(run_guest_callback(
                              cpu, memory, registry, process_mgr, code_addr,
                              nullptr, 0, false, &completed), 42u);
                EXPECT_TRUE(completed);
            } else {
                cpu.rip = code_addr;
                ASSERT_TRUE(memory.write_u32(cpu.sp(),
                    static_cast<vitreo::u32>(PROCESS_RETURN_SENTINEL)).is_ok());
                RunConfig config;
                config.max_instructions = 100;
                const auto result = run_execution_loop(cpu, memory, registry,
                                                       process_mgr, config);
                EXPECT_TRUE(result.success) << result.error_message;
                EXPECT_EQ(result.exit_code, 42);
            }

            EXPECT_TRUE(vitreo::winapi::remove_vectored_exception_handler(veh));
            vitreo::winapi::clear_vectored_exception_handlers_for_test();
        }
    }
}

TEST(CallbackExceptionDispatchTest, X64AssertionInterruptDispatchesAndResumes) {
    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x20000, 0x10000,
        vitreo::MemoryProtection::ReadWrite).is_ok());
    const vitreo::u8 code[]{0xcd, 0x2c, 0xb8, 42, 0, 0, 0, 0xc3};
    ASSERT_TRUE(memory.write_bytes(0x21000, code, sizeof(code)).is_ok());
    // Store ExceptionCode at 0x21200, then continue with the supplied context.
    const vitreo::u8 handler[]{0x48,0x8b,0x01,0x8b,0x00,
        0x89,0x04,0x25,0x00,0x12,0x02,0x00,
        0xb8,0xff,0xff,0xff,0xff,0xc3};
    ASSERT_TRUE(memory.write_bytes(0x21100, handler, sizeof(handler)).is_ok());
    vitreo::winapi::clear_vectored_exception_handlers_for_test();
    const auto veh = vitreo::winapi::add_vectored_exception_handler(1, 0x21100);
    vitreo::cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.set_sp(0x2fff0);
    vitreo::winapi::ProcessManager process_mgr;
    vitreo::winapi::ThunkRegistry registry(0x70000000);
    bool completed = false;
    EXPECT_EQ(run_guest_callback(cpu, memory, registry, process_mgr,
              0x21000, nullptr, 0, true, &completed), 42u);
    EXPECT_TRUE(completed);
    EXPECT_EQ(memory.read_u32(0x21200).value(), 0xC0000420u);
    EXPECT_FALSE(process_mgr.has_exited());
    EXPECT_TRUE(vitreo::winapi::remove_vectored_exception_handler(veh));
    vitreo::winapi::clear_vectored_exception_handlers_for_test();
}

// A breakpoint, CC or CD 03, reaches a vectored handler as Windows reports it
// (tools/oracle_probe/int3_record_probe.c, .expected and .x86.expected):
// ExceptionAddress and the context IP are both the instruction end minus one
// (the CC itself, or the 03 byte of CD 03), with one parameter, 0. The
// handler records what it saw, then resumes past the instruction.
TEST(CallbackExceptionDispatchTest, BreakpointRecordAndContextMatchWindows) {
    struct Case { bool is64; bool callback; bool two_byte; };
    const Case cases[] = {
        {true, false, false}, {true, false, true}, {true, true, false}, {true, true, true},
        {false, false, false}, {false, false, true},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(std::string(c.is64 ? "x64" : "x86") + (c.callback ? " callback" : " loop") +
                     (c.two_byte ? " CD03" : " CC"));
        constexpr vitreo::GuestAddr site = 0x21000;
        constexpr vitreo::GuestAddr handler_addr = 0x21100;
        constexpr vitreo::GuestAddr seen = 0x21200;  // addr, nparams, info0, ip
        const vitreo::u32 length = c.two_byte ? 2u : 1u;
        const vitreo::u32 resume = site + length;
        vitreo::GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x20000, 0x10000,
            vitreo::MemoryProtection::ReadWrite).is_ok());

        // int3 | int 3 ; mov eax,42 ; ret
        std::vector<vitreo::u8> code;
        if (c.two_byte) code = {0xcd, 0x03}; else code = {0xcc};
        code.insert(code.end(), {0xb8, 42, 0, 0, 0, 0xc3});
        ASSERT_TRUE(memory.write_bytes(site, code.data(), code.size()).is_ok());
        for (vitreo::u32 i = 0; i < 4; ++i) {
            ASSERT_TRUE(memory.write_u64(seen + 8 * i, 0xDEADBEEFDEADBEEFull).is_ok());
        }

        const vitreo::u8 r0 = static_cast<vitreo::u8>(resume);
        const vitreo::u8 r1 = static_cast<vitreo::u8>(resume >> 8);
        const vitreo::u8 r2 = static_cast<vitreo::u8>(resume >> 16);
        std::vector<vitreo::u8> handler;
        if (c.is64) {
            handler = {
                0x48, 0x8b, 0x01,                               // mov rax,[rcx]   ExceptionRecord
                0x48, 0x8b, 0x50, 0x10,                         // mov rdx,[rax+10h] ExceptionAddress
                0x48, 0x89, 0x14, 0x25, 0x00, 0x12, 0x02, 0x00, // mov [seen],rdx
                0x8b, 0x50, 0x18,                               // mov edx,[rax+18h] NumberParameters
                0x48, 0x89, 0x14, 0x25, 0x08, 0x12, 0x02, 0x00, // mov [seen+8],rdx
                0x48, 0x8b, 0x50, 0x20,                         // mov rdx,[rax+20h] Information[0]
                0x48, 0x89, 0x14, 0x25, 0x10, 0x12, 0x02, 0x00, // mov [seen+10h],rdx
                0x48, 0x8b, 0x41, 0x08,                         // mov rax,[rcx+8] ContextRecord
                0x48, 0x8b, 0x90, 0xf8, 0x00, 0x00, 0x00,       // mov rdx,[rax+F8h] Rip
                0x48, 0x89, 0x14, 0x25, 0x18, 0x12, 0x02, 0x00, // mov [seen+18h],rdx
                0x48, 0xc7, 0x80, 0xf8, 0x00, 0x00, 0x00,       // mov qword [rax+F8h],resume
                r0, r1, r2, 0x00,
                0xb8, 0xff, 0xff, 0xff, 0xff,                   // mov eax,-1
                0xc3,
            };
        } else {
            handler = {
                0x8b, 0x4c, 0x24, 0x04,                         // mov ecx,[esp+4]
                0x8b, 0x01,                                     // mov eax,[ecx]   ExceptionRecord
                0x8b, 0x50, 0x0c,                               // mov edx,[eax+0Ch] ExceptionAddress
                0x89, 0x15, 0x00, 0x12, 0x02, 0x00,             // mov [seen],edx
                0x8b, 0x50, 0x10,                               // mov edx,[eax+10h] NumberParameters
                0x89, 0x15, 0x08, 0x12, 0x02, 0x00,             // mov [seen+8],edx
                0x8b, 0x50, 0x14,                               // mov edx,[eax+14h] Information[0]
                0x89, 0x15, 0x10, 0x12, 0x02, 0x00,             // mov [seen+10h],edx
                0x8b, 0x41, 0x04,                               // mov eax,[ecx+4] ContextRecord
                0x8b, 0x90, 0xb8, 0x00, 0x00, 0x00,             // mov edx,[eax+B8h] Eip
                0x89, 0x15, 0x18, 0x12, 0x02, 0x00,             // mov [seen+18h],edx
                0xc7, 0x80, 0xb8, 0x00, 0x00, 0x00,             // mov dword [eax+B8h],resume
                r0, r1, r2, 0x00,
                0xb8, 0xff, 0xff, 0xff, 0xff,                   // mov eax,-1
                0xc3,
            };
        }
        ASSERT_TRUE(memory.write_bytes(handler_addr, handler.data(), handler.size()).is_ok());

        vitreo::winapi::clear_vectored_exception_handlers_for_test();
        const vitreo::u64 veh = vitreo::winapi::add_vectored_exception_handler(1, handler_addr);
        ASSERT_NE(veh, 0u);
        vitreo::cpu::CpuState cpu;
        if (c.is64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(0x2fff0);
        if (!c.is64) {
            cpu.seg.fs_desc.base = 0x20000;
            ASSERT_TRUE(memory.write_u32(0x20000, 0xffffffffu).is_ok());
        }
        vitreo::winapi::ProcessManager process_mgr;
        vitreo::winapi::ThunkRegistry registry(0x70000000);
        if (c.callback) {
            bool completed = false;
            EXPECT_EQ(run_guest_callback(cpu, memory, registry, process_mgr, site,
                                         nullptr, 0, c.is64, &completed), 42u);
            EXPECT_TRUE(completed);
        } else {
            cpu.rip = site;
            if (c.is64) {
                ASSERT_TRUE(memory.write_u64(cpu.sp(), PROCESS_RETURN_SENTINEL).is_ok());
            } else {
                ASSERT_TRUE(memory.write_u32(cpu.sp(),
                    static_cast<vitreo::u32>(PROCESS_RETURN_SENTINEL)).is_ok());
            }
            RunConfig config;
            config.max_instructions = 100;
            const auto result = run_execution_loop(cpu, memory, registry, process_mgr, config);
            EXPECT_TRUE(result.success) << result.error_message;
            EXPECT_EQ(result.exit_code, 42);
        }

        const vitreo::u64 mask = c.is64 ? ~0ull : 0xFFFFFFFFull;
        EXPECT_EQ(memory.read_u64(seen).value() & mask, site + length - 1);       // ExceptionAddress
        EXPECT_EQ(memory.read_u64(seen + 8).value() & 0xFFFFFFFFull, 1u);         // NumberParameters
        EXPECT_EQ(memory.read_u64(seen + 0x10).value() & mask, 0u);               // Information[0]
        EXPECT_EQ(memory.read_u64(seen + 0x18).value() & mask, site + length - 1); // context IP

        EXPECT_TRUE(vitreo::winapi::remove_vectored_exception_handler(veh));
        vitreo::winapi::clear_vectored_exception_handlers_for_test();
    }
}

TEST(ExecutionLoopDiagnosticsTest, UnhandledX64AssertionIsNotSuccessfulExit) {
    vitreo::winapi::clear_vectored_exception_handlers_for_test();
    for (bool callback : {false, true}) {
        vitreo::GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x20000, 0x10000,
            vitreo::MemoryProtection::ReadWrite).is_ok());
        const vitreo::u8 code[]{0xcd, 0x2c};
        ASSERT_TRUE(memory.write_bytes(0x21000, code, sizeof(code)).is_ok());
        vitreo::cpu::CpuState cpu;
        cpu.init_long_mode(); cpu.rip = 0x21000; cpu.set_sp(0x2fff0);
        vitreo::winapi::ProcessManager process_mgr;
        vitreo::winapi::ThunkRegistry registry(0x70000000);
        if (callback) {
            (void)run_guest_callback(cpu, memory, registry, process_mgr,
                                    0x21000, nullptr, 0, true);
        } else {
            RunConfig config; config.max_instructions = 4;
            const auto result = run_execution_loop(cpu, memory, registry, process_mgr, config);
            EXPECT_FALSE(result.success);
            EXPECT_EQ(static_cast<vitreo::u32>(result.exit_code), 0xC0000420u);
        }
        EXPECT_TRUE(process_mgr.has_exited());
        EXPECT_EQ(process_mgr.exit_code(), 0xC0000420u);
    }
}

TEST(CallbackExceptionDispatchTest,
     X64VectoredHandlerHandlesHardwareFaultAndResumes) {
    constexpr vitreo::GuestAddr code_addr = 0x21000;
    constexpr vitreo::GuestAddr resume_addr = code_addr + 4;
    constexpr vitreo::GuestAddr handler_addr = 0x21100;
    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());

    // xor eax,eax ; mov eax,[rax] ; mov eax,42 ; ret
    const std::array<vitreo::u8, 10> code{
        0x31, 0xc0,
        0x8b, 0x00,
        0xb8, 0x2a, 0x00, 0x00, 0x00,
        0xc3,
    };
    ASSERT_TRUE(memory.write_bytes(
        code_addr, code.data(), code.size()).is_ok());

    // LONG CALLBACK Veh(EXCEPTION_POINTERS* ep):
    //   ep->ContextRecord->Rip = resume_addr;
    //   return EXCEPTION_CONTINUE_EXECUTION;
    std::array<vitreo::u8, 27> handler{
        0x48, 0x8b, 0x41, 0x08,             // mov rax,[rcx+8]
        0x48, 0xba,                           // mov rdx,resume_addr
        static_cast<vitreo::u8>(resume_addr),
        static_cast<vitreo::u8>(resume_addr >> 8),
        static_cast<vitreo::u8>(resume_addr >> 16),
        static_cast<vitreo::u8>(resume_addr >> 24),
        static_cast<vitreo::u8>(resume_addr >> 32),
        static_cast<vitreo::u8>(resume_addr >> 40),
        static_cast<vitreo::u8>(resume_addr >> 48),
        static_cast<vitreo::u8>(resume_addr >> 56),
        0x48, 0x89, 0x90, 0xf8, 0x00, 0x00, 0x00, // mov [rax+f8h],rdx
        0xb8, 0xff, 0xff, 0xff, 0xff,       // mov eax,-1
        0xc3,
    };
    ASSERT_TRUE(memory.write_bytes(
        handler_addr, handler.data(), handler.size()).is_ok());

    vitreo::winapi::clear_vectored_exception_handlers_for_test();
    const vitreo::u64 veh =
        vitreo::winapi::add_vectored_exception_handler(1, handler_addr);
    ASSERT_NE(veh, 0u);

    vitreo::cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.set_sp(0x2fff0);
    vitreo::winapi::ProcessManager process_mgr;
    vitreo::winapi::ThunkRegistry registry(0x70000000);
    bool completed = false;
    EXPECT_EQ(run_guest_callback(
                  cpu, memory, registry, process_mgr, code_addr,
                  nullptr, 0, true, &completed),
              42u);
    EXPECT_TRUE(completed);

    EXPECT_TRUE(vitreo::winapi::remove_vectored_exception_handler(veh));
    vitreo::winapi::clear_vectored_exception_handlers_for_test();
}

TEST(CallbackExceptionDispatchTest,
     X64VectoredHandlerRestoresEditedControlFlags) {
    constexpr vitreo::GuestAddr code_addr = 0x21000;
    constexpr vitreo::GuestAddr resume_addr = code_addr + 4;
    constexpr vitreo::GuestAddr handler_addr = 0x21100;
    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());

    // Fault, then return the resumed RFLAGS value while clearing DF before RET.
    const std::array<vitreo::u8, 8> code{
        0x31, 0xc0,       // xor eax,eax
        0x8b, 0x00,       // mov eax,[rax]
        0x9c,             // pushfq
        0x58,             // pop rax
        0xfc,             // cld
        0xc3,             // ret
    };
    ASSERT_TRUE(memory.write_bytes(
        code_addr, code.data(), code.size()).is_ok());

    // Set both the resume RIP and DF in the handler-facing AMD64 CONTEXT.
    const std::array<vitreo::u8, 34> handler{
        0x48, 0x8b, 0x41, 0x08,             // mov rax,[rcx+8]
        0x48, 0xba,                           // mov rdx,resume_addr
        static_cast<vitreo::u8>(resume_addr),
        static_cast<vitreo::u8>(resume_addr >> 8),
        static_cast<vitreo::u8>(resume_addr >> 16),
        static_cast<vitreo::u8>(resume_addr >> 24),
        static_cast<vitreo::u8>(resume_addr >> 32),
        static_cast<vitreo::u8>(resume_addr >> 40),
        static_cast<vitreo::u8>(resume_addr >> 48),
        static_cast<vitreo::u8>(resume_addr >> 56),
        0x48, 0x89, 0x90, 0xf8, 0x00, 0x00, 0x00, // mov [rax+f8h],rdx
        0xc7, 0x40, 0x44, 0x02, 0x04, 0x00, 0x00, // mov dword [rax+44h],402h
        0xb8, 0xff, 0xff, 0xff, 0xff,       // mov eax,-1
        0xc3,
    };
    ASSERT_TRUE(memory.write_bytes(
        handler_addr, handler.data(), handler.size()).is_ok());

    vitreo::winapi::clear_vectored_exception_handlers_for_test();
    const vitreo::u64 veh =
        vitreo::winapi::add_vectored_exception_handler(1, handler_addr);
    ASSERT_NE(veh, 0u);

    vitreo::cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.set_sp(0x2fff0);
    vitreo::winapi::ProcessManager process_mgr;
    vitreo::winapi::ThunkRegistry registry(0x70000000);
    bool completed = false;
    const vitreo::u64 resumed_flags = run_guest_callback(
        cpu, memory, registry, process_mgr, code_addr,
        nullptr, 0, true, &completed);
    EXPECT_TRUE(completed);
    EXPECT_NE(resumed_flags & 0x400u, 0u);

    EXPECT_TRUE(vitreo::winapi::remove_vectored_exception_handler(veh));
    vitreo::winapi::clear_vectored_exception_handlers_for_test();
}

TEST(ExecutionLoopDiagnosticsTest,
     UnhandledUd2ReportsArchitecturalTrapAndValidatedGuestChain) {
    constexpr vitreo::GuestAddr code_addr = 0x21000;
    constexpr vitreo::GuestAddr stack_base = 0x2f000;
    constexpr vitreo::u32 first_frame = 0x2ff20;
    constexpr vitreo::u32 second_frame = 0x2ff40;

    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());
    const vitreo::u8 ud2[] = {0x0f, 0x0b};
    ASSERT_TRUE(memory.write_bytes(code_addr, ud2, sizeof(ud2)).is_ok());
    ASSERT_TRUE(memory.write_u32(first_frame, second_frame).is_ok());
    ASSERT_TRUE(memory.write_u32(first_frame + 4, 0x22010).is_ok());
    ASSERT_TRUE(memory.write_u32(second_frame, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(second_frame + 4, 0x23020).is_ok());

    vitreo::cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.rip = code_addr;
    cpu.set_sp(stack_base);
    cpu.gpr.rbp = first_frame;

    vitreo::winapi::ProcessManager process_mgr;
    vitreo::winapi::ThunkRegistry registry(0x70000000);
    RunConfig config;
    config.max_instructions = 4;

    testing::internal::CaptureStderr();
    const RunResult result = run_execution_loop(
        cpu, memory, registry, process_mgr, config);
    const std::string diagnostics = testing::internal::GetCapturedStderr();

    EXPECT_FALSE(result.success);
    EXPECT_NE(diagnostics.find(
                  "Fault instruction: UD2 "
                  "(architectural invalid-opcode trap)"),
              std::string::npos);
    EXPECT_NE(diagnostics.find(
                  "Guest call chain (validated x86 EBP, best effort)"),
              std::string::npos);
    EXPECT_NE(diagnostics.find("#1 0x22010"), std::string::npos);
    EXPECT_NE(diagnostics.find("#2 0x23020"), std::string::npos);
}

TEST(ExecutionLoopDiagnosticsTest,
     UnhandledX64ReadAvTerminatesWithAccessViolationStatus) {
    constexpr vitreo::GuestAddr code_addr = 0x21000;
    constexpr vitreo::GuestAddr stack_addr = 0x2fff0;

    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());
    const vitreo::u8 code[] = {
        0x48, 0x8b, 0x12, // mov rdx,[rdx]
        0xf4,             // hlt (unreachable)
    };
    ASSERT_TRUE(memory.write_bytes(code_addr, code, sizeof(code)).is_ok());

    vitreo::cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.rip = code_addr;
    cpu.set_sp(stack_addr);
    cpu.gpr.rdx = 2;

    vitreo::winapi::ProcessManager process_mgr;
    vitreo::winapi::ThunkRegistry registry(0x70000000);
    RunConfig config;
    config.max_instructions = 4;

    testing::internal::CaptureStderr();
    const RunResult result = run_execution_loop(
        cpu, memory, registry, process_mgr, config);
    const std::string diagnostics = testing::internal::GetCapturedStderr();

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.exit_code, static_cast<int>(0xC0000005u));
    EXPECT_EQ(result.error_message, "unhandled guest exception");
    EXPECT_TRUE(process_mgr.has_exited());
    EXPECT_EQ(process_mgr.exit_code(), 0xC0000005u);
    EXPECT_NE(diagnostics.find("unhandled exception 0xc0000005"),
              std::string::npos);
    EXPECT_EQ(diagnostics.find("Unhandled GeneralProtection"),
              std::string::npos);
    EXPECT_EQ(cpu.rip, code_addr);
    EXPECT_EQ(cpu.gpr.rdx, 2u);
}
