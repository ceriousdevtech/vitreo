/// @file test_cli_sysenter_int2e.cpp
/// @brief x86 SYSENTER and INT 2Eh native-service dispatch tests.

#include <gtest/gtest.h>
#include "runtime/execution/guest_run_loop.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/process/process.hpp"
#include <array>
#include <string>
#include <vector>

using namespace vitreo::cli;

TEST(X86SysenterDispatchTest,
     DispatchesNativeServiceNumberAndRestoresCalleeSaved) {
    // Windows 32-bit KiFastSystemCall: mov edx,esp; sysenter; ret.
    // EAX is the native service number; stdcall args sit above the return
    // address. SYSENTER must reach NT dispatch (JIT must not swallow it).
    constexpr vitreo::GuestAddr code_addr = 0x24000;
    constexpr vitreo::u32 initial_sp = 0x2fff0;
    constexpr vitreo::u16 kOrdinal = 2;
    constexpr vitreo::u32 kService = 0;
    constexpr vitreo::u32 kArg0 = 0x0000ABCDu;
    constexpr vitreo::u32 kStatus = 0xC0000001u; // STATUS_UNSUCCESSFUL

    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());

    bool handler_called = false;
    vitreo::u64 seen_arg = 0;
    vitreo::winapi::ThunkRegistry registry(0x70000000);
    vitreo::winapi::DllExports exports;
    exports.entries.push_back(vitreo::winapi::ExportEntry{
        "NtQueryInformationProcess", kOrdinal,
        [&](vitreo::winapi::Marshaler& marshaler) {
            handler_called = true;
            seen_arg = marshaler.arg(0);
            auto* guest = marshaler.cpu();
            guest->gpr.rbx = 0xDEADBEEF;
            guest->gpr.rsi = 0xDEADBEEF;
            guest->gpr.rdi = 0xDEADBEEF;
            guest->gpr.rbp = 0xDEADBEEF;
            marshaler.set_return(kStatus);
        },
        4});
    ASSERT_TRUE(registry.register_dll("ntdll.dll", std::move(exports)).is_ok());
    // The synthetic ntdll image publishes this table; service 0 is the only one.
    registry.publish_native_services({registry.get_thunk_address(
        "ntdll.dll", "NtQueryInformationProcess").value()});

    std::vector<vitreo::u8> code;
    auto emit_u32 = [&](vitreo::u32 value) {
        code.push_back(static_cast<vitreo::u8>(value));
        code.push_back(static_cast<vitreo::u8>(value >> 8));
        code.push_back(static_cast<vitreo::u8>(value >> 16));
        code.push_back(static_cast<vitreo::u8>(value >> 24));
    };
    code.push_back(0x68); // push imm32 arg
    emit_u32(kArg0);
    code.push_back(0xBB); emit_u32(0x11111111); // mov ebx
    code.push_back(0xBE); emit_u32(0x22222222); // mov esi
    code.push_back(0xBF); emit_u32(0x33333333); // mov edi
    code.push_back(0xBD); emit_u32(0x44444444); // mov ebp
    code.push_back(0xB8); emit_u32(kService);   // mov eax, service number
    const std::size_t call_at = code.size();
    code.push_back(0xE8);
    emit_u32(0); // rel32 placeholder
    const std::size_t after_call = code.size();
    code.push_back(0xF4); // hlt
    const std::size_t kifs = code.size();
    code.insert(code.end(), {0x89, 0xE2, 0x0F, 0x34, 0xC3}); // mov edx,esp; sysenter; ret
    const vitreo::u32 rel32 =
        static_cast<vitreo::u32>(kifs - after_call);
    code[call_at + 1] = static_cast<vitreo::u8>(rel32);
    code[call_at + 2] = static_cast<vitreo::u8>(rel32 >> 8);
    code[call_at + 3] = static_cast<vitreo::u8>(rel32 >> 16);
    code[call_at + 4] = static_cast<vitreo::u8>(rel32 >> 24);
    ASSERT_TRUE(memory.write_bytes(
        code_addr, code.data(), code.size()).is_ok());

    vitreo::cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.rip = code_addr;
    cpu.set_sp(initial_sp);
    vitreo::winapi::ProcessManager process_mgr;
    vitreo::cli::RunConfig config;
    config.max_instructions = 64;

    const vitreo::cli::RunResult result = run_execution_loop(
        cpu, memory, registry, process_mgr, config);
    // The service returned to its caller, which then ran into the `hlt` that
    // ends this program: privileged in user mode (Intel SDM HLT: #GP at
    // CPL 3), so STATUS_PRIVILEGED_INSTRUCTION at that very address.
    EXPECT_FALSE(result.success);
    EXPECT_EQ(static_cast<vitreo::u32>(result.exit_code), 0xC0000096u);
    EXPECT_EQ(cpu.rip, code_addr + after_call);
    EXPECT_TRUE(handler_called);
    EXPECT_EQ(seen_arg, kArg0);
    EXPECT_EQ(cpu.gpr.get32(static_cast<vitreo::u8>(
                  vitreo::cpu::RegIndex::RAX)), kStatus);
    EXPECT_EQ(cpu.gpr.get32(static_cast<vitreo::u8>(
                  vitreo::cpu::RegIndex::RBX)), 0x11111111u);
    EXPECT_EQ(cpu.gpr.get32(static_cast<vitreo::u8>(
                  vitreo::cpu::RegIndex::RSI)), 0x22222222u);
    EXPECT_EQ(cpu.gpr.get32(static_cast<vitreo::u8>(
                  vitreo::cpu::RegIndex::RDI)), 0x33333333u);
    EXPECT_EQ(cpu.gpr.get32(static_cast<vitreo::u8>(
                  vitreo::cpu::RegIndex::RBP)), 0x44444444u);
}

TEST(X86SysenterDispatchTest, UnknownServiceNumberFailsHonestly) {
    constexpr vitreo::GuestAddr code_addr = 0x25000;
    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());
    const std::array<vitreo::u8, 7> code{
        0xB8, 0xEF, 0xBE, 0x00, 0x00, // mov eax, 0xBEEF
        0x0F, 0x34,                   // sysenter
    };
    ASSERT_TRUE(memory.write_bytes(
        code_addr, code.data(), code.size()).is_ok());

    vitreo::cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.rip = code_addr;
    cpu.set_sp(0x2fff0);
    vitreo::winapi::ProcessManager process_mgr;
    vitreo::winapi::ThunkRegistry registry(0x70000000);
    vitreo::cli::RunConfig config;
    config.max_instructions = 16;

    const vitreo::cli::RunResult result = run_execution_loop(
        cpu, memory, registry, process_mgr, config);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("Unknown x86 SYSENTER NT service number"),
              std::string::npos);
    EXPECT_NE(result.error_message.find("0xbeef"), std::string::npos);
}

TEST(X86Int2EDispatchTest,
     DispatchesNativeServiceNumberAndRestoresCalleeSaved) {
    // Windows 32-bit KiSystemService: INT 2Eh is in-line in NtXxx
    // (not a CALL to KiFastSystemCall). EAX is the native service number;
    // stdcall args sit above the return address. Intel INT n does not
    // advance RIP; the run loop skips CD 2E so the next guest insn runs.
    constexpr vitreo::GuestAddr code_addr = 0x26000;
    constexpr vitreo::u32 initial_sp = 0x2fff0;
    constexpr vitreo::u16 kOrdinal = 2;
    constexpr vitreo::u32 kService = 0;
    constexpr vitreo::u32 kArg0 = 0x0000ABCDu;
    constexpr vitreo::u32 kStatus = 0xC0000001u; // STATUS_UNSUCCESSFUL

    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());

    bool handler_called = false;
    vitreo::u64 seen_arg = 0;
    vitreo::winapi::ThunkRegistry registry(0x70000000);
    vitreo::winapi::DllExports exports;
    exports.entries.push_back(vitreo::winapi::ExportEntry{
        "NtQueryInformationProcess", kOrdinal,
        [&](vitreo::winapi::Marshaler& marshaler) {
            handler_called = true;
            seen_arg = marshaler.arg(0);
            auto* guest = marshaler.cpu();
            guest->gpr.rbx = 0xDEADBEEF;
            guest->gpr.rsi = 0xDEADBEEF;
            guest->gpr.rdi = 0xDEADBEEF;
            guest->gpr.rbp = 0xDEADBEEF;
            marshaler.set_return(kStatus);
        },
        4});
    ASSERT_TRUE(registry.register_dll("ntdll.dll", std::move(exports)).is_ok());
    // The synthetic ntdll image publishes this table; service 0 is the only one.
    registry.publish_native_services({registry.get_thunk_address(
        "ntdll.dll", "NtQueryInformationProcess").value()});

    std::vector<vitreo::u8> code;
    auto emit_u32 = [&](vitreo::u32 value) {
        code.push_back(static_cast<vitreo::u8>(value));
        code.push_back(static_cast<vitreo::u8>(value >> 8));
        code.push_back(static_cast<vitreo::u8>(value >> 16));
        code.push_back(static_cast<vitreo::u8>(value >> 24));
    };
    code.push_back(0x68); emit_u32(kArg0);     // push imm32 arg
    code.push_back(0x68); emit_u32(0);         // push dummy NtXxx return
    code.push_back(0xBB); emit_u32(0x11111111); // mov ebx
    code.push_back(0xBE); emit_u32(0x22222222); // mov esi
    code.push_back(0xBF); emit_u32(0x33333333); // mov edi
    code.push_back(0xBD); emit_u32(0x44444444); // mov ebp
    code.push_back(0xB8); emit_u32(kService);   // mov eax, service number
    code.push_back(0xCD); code.push_back(0x2E); // int 2Eh
    const std::size_t after_call = code.size();
    code.push_back(0xF4);                       // hlt
    ASSERT_TRUE(memory.write_bytes(
        code_addr, code.data(), code.size()).is_ok());

    vitreo::cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.rip = code_addr;
    cpu.set_sp(initial_sp);
    vitreo::winapi::ProcessManager process_mgr;
    vitreo::cli::RunConfig config;
    config.max_instructions = 64;

    const vitreo::cli::RunResult result = run_execution_loop(
        cpu, memory, registry, process_mgr, config);
    // The service returned to its caller, which then ran into the `hlt` that
    // ends this program: privileged in user mode (Intel SDM HLT: #GP at
    // CPL 3), so STATUS_PRIVILEGED_INSTRUCTION at that very address.
    EXPECT_FALSE(result.success);
    EXPECT_EQ(static_cast<vitreo::u32>(result.exit_code), 0xC0000096u);
    EXPECT_EQ(cpu.rip, code_addr + after_call);
    EXPECT_TRUE(handler_called);
    EXPECT_EQ(seen_arg, kArg0);
    EXPECT_EQ(cpu.gpr.get32(static_cast<vitreo::u8>(
                  vitreo::cpu::RegIndex::RAX)), kStatus);
    EXPECT_EQ(cpu.gpr.get32(static_cast<vitreo::u8>(
                  vitreo::cpu::RegIndex::RBX)), 0x11111111u);
    EXPECT_EQ(cpu.gpr.get32(static_cast<vitreo::u8>(
                  vitreo::cpu::RegIndex::RSI)), 0x22222222u);
    EXPECT_EQ(cpu.gpr.get32(static_cast<vitreo::u8>(
                  vitreo::cpu::RegIndex::RDI)), 0x33333333u);
    EXPECT_EQ(cpu.gpr.get32(static_cast<vitreo::u8>(
                  vitreo::cpu::RegIndex::RBP)), 0x44444444u);
}

TEST(X86Int2EDispatchTest, UnknownServiceNumberFailsHonestly) {
    constexpr vitreo::GuestAddr code_addr = 0x27000;
    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());
    const std::array<vitreo::u8, 7> code{
        0xB8, 0xEF, 0xBE, 0x00, 0x00, // mov eax, 0xBEEF
        0xCD, 0x2E,                   // int 2Eh
    };
    ASSERT_TRUE(memory.write_bytes(
        code_addr, code.data(), code.size()).is_ok());

    vitreo::cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.rip = code_addr;
    cpu.set_sp(0x2fff0);
    vitreo::winapi::ProcessManager process_mgr;
    vitreo::winapi::ThunkRegistry registry(0x70000000);
    vitreo::cli::RunConfig config;
    config.max_instructions = 16;

    const vitreo::cli::RunResult result = run_execution_loop(
        cpu, memory, registry, process_mgr, config);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("INT 2E"), std::string::npos);
    EXPECT_NE(result.error_message.find("0xbeef"), std::string::npos);
}

TEST(X86Int2EDispatchTest, CallbackDispatchesNativeServiceNumber) {
    // Same KiSystemService contract as the main-loop INT 2E test, but
    // through run_guest_callback so the callback loop skips CD 2E and
    // dispatches. Guest body must RET to CALLBACK_RETURN_SENTINEL (not HLT).
    constexpr vitreo::GuestAddr code_addr = 0x28000;
    constexpr vitreo::u16 kOrdinal = 2;
    constexpr vitreo::u32 kService = 0;
    constexpr vitreo::u32 kArg0 = 0x0000ABCDu;
    constexpr vitreo::u32 kStatus = 0xC0000001u; // STATUS_UNSUCCESSFUL

    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());

    bool handler_called = false;
    vitreo::u64 seen_arg = 0;
    vitreo::winapi::ThunkRegistry registry(0x70000000);
    vitreo::winapi::DllExports exports;
    exports.entries.push_back(vitreo::winapi::ExportEntry{
        "NtQueryInformationProcess", kOrdinal,
        [&](vitreo::winapi::Marshaler& marshaler) {
            handler_called = true;
            seen_arg = marshaler.arg(0);
            marshaler.set_return(kStatus);
        },
        4});
    ASSERT_TRUE(registry.register_dll("ntdll.dll", std::move(exports)).is_ok());
    // The synthetic ntdll image publishes this table; service 0 is the only one.
    registry.publish_native_services({registry.get_thunk_address(
        "ntdll.dll", "NtQueryInformationProcess").value()});

    std::vector<vitreo::u8> code;
    auto emit_u32 = [&](vitreo::u32 value) {
        code.push_back(static_cast<vitreo::u8>(value));
        code.push_back(static_cast<vitreo::u8>(value >> 8));
        code.push_back(static_cast<vitreo::u8>(value >> 16));
        code.push_back(static_cast<vitreo::u8>(value >> 24));
    };
    code.push_back(0x68); emit_u32(kArg0);     // push imm32 arg
    code.push_back(0x68); emit_u32(0);         // push dummy NtXxx return
    code.push_back(0xB8); emit_u32(kService);   // mov eax, service number
    code.push_back(0xCD); code.push_back(0x2E); // int 2Eh
    code.push_back(0x83); code.push_back(0xC4); code.push_back(0x08); // add esp, 8
    code.push_back(0xC3);                       // ret to sentinel
    ASSERT_TRUE(memory.write_bytes(
        code_addr, code.data(), code.size()).is_ok());

    vitreo::cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(0x2fff0);
    vitreo::winapi::ProcessManager process_mgr;
    bool completed = false;
    const vitreo::u64 eax = run_guest_callback(
        cpu, memory, registry, process_mgr, code_addr,
        nullptr, 0, false, &completed);
    EXPECT_TRUE(handler_called);
    EXPECT_EQ(seen_arg, kArg0);
    EXPECT_EQ(eax, kStatus);
    EXPECT_TRUE(completed);
}

TEST(X86Int2EDispatchTest, CallbackUnknownServiceNumberFailsHonestly) {
    constexpr vitreo::GuestAddr code_addr = 0x29000;
    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());
    const std::array<vitreo::u8, 8> code{
        0xB8, 0xEF, 0xBE, 0x00, 0x00, // mov eax, 0xBEEF
        0xCD, 0x2E,                   // int 2Eh
        0xC3,                         // ret — must not complete as success
    };
    ASSERT_TRUE(memory.write_bytes(
        code_addr, code.data(), code.size()).is_ok());

    vitreo::cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(0x2fff0);
    vitreo::winapi::ProcessManager process_mgr;
    vitreo::winapi::ThunkRegistry registry(0x70000000);
    bool completed = false;
    const vitreo::u64 eax = run_guest_callback(
        cpu, memory, registry, process_mgr, code_addr,
        nullptr, 0, false, &completed);
    EXPECT_FALSE(completed);
    EXPECT_NE(eax, 0xBEEFu);
}

namespace {

// mov rcx, arg; mov r10, rcx; xor ecx, ecx; mov eax, service; syscall; hlt
// -- the Windows x64 service entry, with RCX clobbered before the SYSCALL so
// arg 0 can only come from R10. The `hlt` ends the program with
// STATUS_PRIVILEGED_INSTRUCTION once the service has returned.
vitreo::cli::RunResult run_x64_syscall(vitreo::winapi::ThunkRegistry& registry,
                                       vitreo::cpu::CpuState& cpu,
                                       vitreo::u32 service, vitreo::u64 arg0) {
    constexpr vitreo::GuestAddr code_addr = 0x24000;
    vitreo::GuestMemory memory;
    EXPECT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());
    std::vector<vitreo::u8> code = {0x48, 0xB9};
    for (int i = 0; i < 8; ++i) code.push_back(static_cast<vitreo::u8>(arg0 >> (8 * i)));
    code.insert(code.end(), {0x4C, 0x8B, 0xD1, 0x31, 0xC9, 0xB8});
    for (int i = 0; i < 4; ++i) code.push_back(static_cast<vitreo::u8>(service >> (8 * i)));
    code.insert(code.end(), {0x0F, 0x05, 0xF4});
    EXPECT_TRUE(memory.write_bytes(code_addr, code.data(), code.size()).is_ok());
    cpu.init_long_mode();
    cpu.rip = code_addr;
    cpu.set_sp(0x2fff0);
    vitreo::winapi::ProcessManager process_mgr;
    vitreo::cli::RunConfig config;
    config.max_instructions = 64;
    return run_execution_loop(cpu, memory, registry, process_mgr, config);
}

}  // namespace

// SYSCALL enters the service EAX names -- wherever the instruction is, so an
// indirect syscall reaches the service it asked for -- with R10 as arg 0.
TEST(X64SyscallDispatchTest, EntersTheServiceEaxNamesWithR10AsArgZero) {
    vitreo::winapi::ThunkRegistry registry(0x00007FFB00000000ull);
    vitreo::u64 first_arg = 0;
    vitreo::u64 second_arg = 0;
    vitreo::winapi::DllExports exports;
    exports.entries.push_back(vitreo::winapi::ExportEntry{
        "NtFirst", 0,
        [&](vitreo::winapi::Marshaler& m) {
            first_arg = m.arg(0);
            m.set_return(0x111);
        },
        4});
    exports.entries.push_back(vitreo::winapi::ExportEntry{
        "NtSecond", 0,
        [&](vitreo::winapi::Marshaler& m) {
            second_arg = m.arg(0);
            m.set_return(0x222);
        },
        4});
    ASSERT_TRUE(registry.register_dll("ntdll.dll", std::move(exports)).is_ok());
    registry.publish_native_services(
        {registry.get_thunk_address("ntdll.dll", "NtFirst").value(),
         registry.get_thunk_address("ntdll.dll", "NtSecond").value()});

    vitreo::cpu::CpuState cpu;
    const auto result = run_x64_syscall(registry, cpu, 1, 0xABCD1234ull);
    // The service returned; the program then ends at its privileged `hlt`.
    EXPECT_EQ(static_cast<vitreo::u32>(result.exit_code), 0xC0000096u);
    EXPECT_EQ(cpu.rip, 0x24000u + 22u);
    EXPECT_EQ(first_arg, 0u);
    EXPECT_EQ(second_arg, 0xABCD1234ull);
    EXPECT_EQ(cpu.gpr.rax, 0x222u);
    // SYSCALL leaves the return RIP in RCX (the `hlt`).
    EXPECT_EQ(cpu.gpr.rcx, 0x24000u + 22u);
}

// ntdll_service_layout_probe "service 0xfff": a number past the service
// table returns STATUS_INVALID_SYSTEM_SERVICE in RAX; it is not a fault.
TEST(X64SyscallDispatchTest, NumberPastTheTableIsInvalidSystemService) {
    vitreo::winapi::ThunkRegistry registry(0x00007FFB00000000ull);
    vitreo::winapi::DllExports exports;
    exports.entries.push_back(vitreo::winapi::ExportEntry{
        "NtFirst", 0, [](vitreo::winapi::Marshaler& m) { m.set_return(0); }, 4});
    ASSERT_TRUE(registry.register_dll("ntdll.dll", std::move(exports)).is_ok());
    registry.publish_native_services(
        {registry.get_thunk_address("ntdll.dll", "NtFirst").value()});

    for (const vitreo::u32 service : {1u, 0xFFFu, 0x1000u}) {
        SCOPED_TRACE(service);
        vitreo::cpu::CpuState cpu;
        const auto result = run_x64_syscall(registry, cpu, service, 0);
        EXPECT_EQ(static_cast<vitreo::u32>(result.exit_code), 0xC0000096u);
        EXPECT_EQ(cpu.rip, 0x24000u + 22u);
        EXPECT_EQ(cpu.gpr.rax, 0xC000001Cu);
        EXPECT_EQ(cpu.gpr.rcx, 0x24000u + 22u);
    }
}
