/// @file test_cli_thunk_stack.cpp
/// @brief Thunk return, stack and callee-saved register ABI contracts.

#include <gtest/gtest.h>
#include "runtime/execution/guest_run_loop.hpp"
#include "runtime/execution/thunk_fast_path.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/process/process.hpp"
#include <array>
#include <thread>
#include <vector>

using namespace vitreo::cli;

TEST(CallbackExecutionPolicyTest, NestedX64HostCallbackPreservesOuterStackAndNonvolatiles) {
    // Microsoft x64 ABI: RDI is nonvolatile and each call reserves shadow space.
    for (const bool linear : {false, true}) {
        SCOPED_TRACE(linear);
        vitreo::GuestMemory memory;
        if (linear) {
            ASSERT_TRUE(memory.enable_linear_reservation(vitreo::usize{8} << 30));
            ASSERT_TRUE(memory.enable_linear_hardware_protection());
        }
        ASSERT_TRUE(memory.map_region(0x20000, 0x10000,
            vitreo::MemoryProtection::ReadWrite).is_ok());
        vitreo::cpu::CpuState cpu;
        cpu.init_long_mode();
        cpu.set_sp(0x2fff0);
        cpu.gpr.rdi = 0x9988776655443322ull;
        vitreo::winapi::ProcessManager process_mgr;
        ASSERT_TRUE(memory.map_region(0x70000000, 0x4000,
            vitreo::MemoryProtection::ReadWrite).is_ok());
        vitreo::winapi::ThunkRegistry registry(0x70000000);
        const vitreo::u8 nested[] = {
            0x57, 0x48, 0x83, 0xec, 0x20, // push rdi; sub rsp,32
            0x48, 0xbf, 1, 2, 3, 4, 5, 6, 7, 8,
            0xb8, 7, 0, 0, 0,
            0x48, 0x83, 0xc4, 0x20, 0x5f, 0xc3};
        ASSERT_TRUE(memory.write_bytes(0x22000, nested, sizeof(nested)).is_ok());
        vitreo::winapi::DllExports exports;
        exports.entries.push_back(vitreo::winapi::ExportEntry{
            "Nested", 1, [&](vitreo::winapi::Marshaler& marshaler) {
                bool completed = false;
                const auto result = run_guest_callback(cpu, memory, registry,
                    process_mgr, 0x22000, nullptr, 0, true, &completed);
                EXPECT_TRUE(completed);
                EXPECT_EQ(result, 7u);
                marshaler.set_return(result);
            }});
        ASSERT_TRUE(registry.register_dll("test.dll", std::move(exports)).is_ok());
        const auto thunk = registry.get_thunk_address("test.dll", "Nested");
        ASSERT_TRUE(thunk.is_ok());
        const std::array<vitreo::u8, 8> sled{0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90};
        ASSERT_TRUE(memory.write_bytes(thunk.value(), sled.data(), sled.size()).is_ok());
        std::vector<vitreo::u8> outer{
            0x57, 0x48, 0x83, 0xec, 0x20,
            0x48, 0xbf, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
            0x48, 0xb8};
        for (unsigned shift = 0; shift < 64; shift += 8)
            outer.push_back(static_cast<vitreo::u8>(thunk.value() >> shift));
        const vitreo::u8 tail[]{0xff, 0xd0, 0x48, 0x89, 0xf8,
            0x48, 0x83, 0xc4, 0x20, 0x5f, 0xc3};
        outer.insert(outer.end(), std::begin(tail), std::end(tail));
        ASSERT_TRUE(memory.write_bytes(0x21000, outer.data(), outer.size()).is_ok());
        bool completed = false;
        vitreo::u64 result = 0;
        std::thread guest([&] {
            init_thread_jit_state(memory);
            result = run_guest_callback(cpu, memory, registry, process_mgr,
                0x21000, nullptr, 0, true, &completed);
        });
        guest.join();
        EXPECT_TRUE(completed);
        EXPECT_EQ(result, 0x1122334455667788ull);
        EXPECT_EQ(cpu.gpr.rdi, 0x9988776655443322ull);
        EXPECT_EQ(cpu.sp(), 0x2fff0u);
    }
}

TEST(CallbackExecutionPolicyTest, HostcallClobbersX86CalleeScratchStack) {
    // Host thunks stand in for stdcall callees, which write locals below the
    // return address. Leaving that region intact is guest-visible.
    constexpr vitreo::GuestAddr thunk_base = 0x70000000;
    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());

    vitreo::winapi::ThunkRegistry registry(thunk_base);
    vitreo::winapi::DllExports exports;
    exports.entries.push_back(vitreo::winapi::ExportEntry{
        "ObserveStack", 1,
        [&](vitreo::winapi::Marshaler& marshaler) {
            marshaler.set_return(1);
        }});
    ASSERT_TRUE(registry.register_dll("test.dll", std::move(exports)).is_ok());
    auto entry = registry.get_thunk_address("test.dll", "ObserveStack");
    ASSERT_TRUE(entry.is_ok());

    constexpr vitreo::u32 kSp = 0x2fff0;
    constexpr vitreo::u32 kClobber = 0x200;
    for (vitreo::u32 off = 0; off < kClobber; off += 4) {
        ASSERT_TRUE(memory.write_u32(kSp - kClobber + off, 0xA5A5A5A5u).is_ok());
    }
    ASSERT_TRUE(memory.write_u32(kSp, 0x0040FEEDu).is_ok());
    ASSERT_TRUE(memory.write_u32(kSp + 4, 0x11111111u).is_ok());

    vitreo::cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(kSp);
    vitreo::winapi::ProcessManager process_mgr;
    const vitreo::u64 eax = run_guest_callback(
        cpu, memory, registry, process_mgr,
        entry.value() + vitreo::winapi::ThunkRegistry::STUB_SIZE,
        nullptr, 0, false);
    EXPECT_EQ(eax, 1u);

    // The callback push occupies [ESP-4, ESP). The wipe covers unused stack
    // below that and must not touch the caller-owned return slot or args.
    for (vitreo::u32 off = 0; off + 4 < kClobber; off += 4) {
        auto word = memory.read_u32(kSp - kClobber + off);
        ASSERT_TRUE(word.is_ok());
        EXPECT_EQ(word.value(), 0u) << "off=" << off;
    }
    EXPECT_EQ(memory.read_u32(kSp).value(), 0x0040FEEDu);
    EXPECT_EQ(memory.read_u32(kSp + 4).value(), 0x11111111u);
}

TEST(ThunkStackClobberTest, X86ThunkScrubsBelowEspLeavesCallerFrame) {
    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());

    constexpr vitreo::u32 kSp = 0x2fff0;
    constexpr vitreo::u32 kClobber = 0x200;
    for (vitreo::u32 off = 0; off < kClobber; off += 4) {
        ASSERT_TRUE(memory.write_u32(kSp - kClobber + off, 0xA5A5A5A5u).is_ok());
    }
    ASSERT_TRUE(memory.write_u32(kSp, 0x00401000u).is_ok());
    ASSERT_TRUE(memory.write_u32(kSp + 4, 0x22222222u).is_ok());
    ASSERT_TRUE(memory.write_u32(kSp + 8, 0x33333333u).is_ok());

    vitreo::cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(kSp);
    maybe_clobber_x86_thunk_callee_stack(cpu, memory);

    for (vitreo::u32 off = 0; off < kClobber; off += 4) {
        auto word = memory.read_u32(kSp - kClobber + off);
        ASSERT_TRUE(word.is_ok());
        EXPECT_EQ(word.value(), 0u) << "off=" << off;
    }
    EXPECT_EQ(memory.read_u32(kSp).value(), 0x00401000u);
    EXPECT_EQ(memory.read_u32(kSp + 4).value(), 0x22222222u);
    EXPECT_EQ(memory.read_u32(kSp + 8).value(), 0x33333333u);
    EXPECT_EQ(cpu.sp(), kSp);
}

TEST(FinishThunkRetTest, X86StdcallPopsReturnAndCleanupBytes) {
    // stdcall RET: pop 4-byte return, then pop stdcall_bytes of args.
    constexpr vitreo::GuestAddr thunk_base = 0x70000000;
    constexpr vitreo::u32 kSp = 0x2ffe0;
    constexpr vitreo::u32 kRet = 0x00401234u;
    constexpr vitreo::u16 kCleanup = 8;  // two DWORD args

    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());

    vitreo::winapi::ThunkRegistry registry(thunk_base);
    vitreo::winapi::DllExports exports;
    exports.entries.push_back(vitreo::winapi::ExportEntry{
        "TwoArgs", 1, [](vitreo::winapi::Marshaler&) {}, kCleanup});
    ASSERT_TRUE(registry.register_dll("test.dll", std::move(exports)).is_ok());
    const auto entry = registry.get_thunk_address("test.dll", "TwoArgs");
    ASSERT_TRUE(entry.is_ok());
    const vitreo::GuestAddr gateway =
        entry.value() + vitreo::winapi::ThunkRegistry::STUB_SIZE;
    EXPECT_EQ(registry.resolve_stdcall_bytes(gateway), kCleanup);

    ASSERT_TRUE(memory.write_u32(kSp, kRet).is_ok());
    ASSERT_TRUE(memory.write_u32(kSp + 4, 0x11111111u).is_ok());
    ASSERT_TRUE(memory.write_u32(kSp + 8, 0x22222222u).is_ok());

    vitreo::cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.rip = gateway;
    cpu.set_sp(kSp);

    ASSERT_TRUE(finish_thunk_ret(cpu, memory, registry, gateway));
    EXPECT_EQ(cpu.rip, kRet);
    EXPECT_EQ(cpu.sp(), kSp + 4 + kCleanup);
}

TEST(FinishThunkRetTest, X64PopsEightByteReturnNoArgCleanup) {
    // x64: caller cleans args; RET only pops the 8-byte return address.
    constexpr vitreo::GuestAddr thunk_base = 0x00007FFB'00000000ULL;
    constexpr vitreo::u64 kSp = 0x2ffe0;
    constexpr vitreo::u64 kRet = 0x0000000140001234ULL;

    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());

    vitreo::winapi::ThunkRegistry registry(thunk_base);
    vitreo::winapi::DllExports exports;
    exports.entries.push_back(vitreo::winapi::ExportEntry{
        "NoArgs", 1, [](vitreo::winapi::Marshaler&) {}, 0});
    ASSERT_TRUE(registry.register_dll("test.dll", std::move(exports)).is_ok());
    const auto entry = registry.get_thunk_address("test.dll", "NoArgs");
    ASSERT_TRUE(entry.is_ok());
    const vitreo::GuestAddr gateway =
        entry.value() + vitreo::winapi::ThunkRegistry::STUB_SIZE;

    ASSERT_TRUE(memory.write_u64(kSp, kRet).is_ok());
    // Decoy args above the return — must not be popped by finish_thunk_ret.
    ASSERT_TRUE(memory.write_u64(kSp + 8, 0xDEADBEEFCAFEBABEULL).is_ok());

    vitreo::cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.rip = gateway;
    cpu.set_sp(kSp);

    ASSERT_TRUE(finish_thunk_ret(cpu, memory, registry, gateway));
    EXPECT_EQ(cpu.rip, kRet);
    EXPECT_EQ(cpu.sp(), kSp + 8);
}

TEST(FinishThunkRetTest, StackUnderflowLeavesRipAndSpUnchanged) {
    constexpr vitreo::GuestAddr thunk_base = 0x70000000;
    vitreo::GuestMemory memory;
    // No stack mapping — read_u32/u64 fails.
    vitreo::winapi::ThunkRegistry registry(thunk_base);
    vitreo::winapi::DllExports exports;
    exports.entries.push_back(vitreo::winapi::ExportEntry{
        "Empty", 1, [](vitreo::winapi::Marshaler&) {}, 0});
    ASSERT_TRUE(registry.register_dll("test.dll", std::move(exports)).is_ok());
    const auto entry = registry.get_thunk_address("test.dll", "Empty");
    ASSERT_TRUE(entry.is_ok());
    const vitreo::GuestAddr gateway =
        entry.value() + vitreo::winapi::ThunkRegistry::STUB_SIZE;

    vitreo::cpu::CpuState cpu;
    cpu.init_protected_mode();
    constexpr vitreo::u64 kRip = 0x70000008;
    constexpr vitreo::u64 kSp = 0x2ffe0;
    cpu.rip = kRip;
    cpu.set_sp(kSp);

    EXPECT_FALSE(finish_thunk_ret(cpu, memory, registry, gateway));
    EXPECT_EQ(cpu.rip, kRip);
    EXPECT_EQ(cpu.sp(), kSp);
}

TEST(ThunkCalleeStackContractTest,
     X86MainDispatchClobbersOnlyBelowThunkEsp) {
    constexpr vitreo::GuestAddr code_addr = 0x21000;
    constexpr vitreo::GuestAddr stack_base = 0x2f000;
    constexpr vitreo::u32 initial_sp = 0x2fff0;

    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());
    std::array<vitreo::u8, 0x1000> stack_paint{};
    stack_paint.fill(0xa5);
    ASSERT_TRUE(memory.write_bytes(
        stack_base, stack_paint.data(), stack_paint.size()).is_ok());

    vitreo::winapi::ThunkRegistry registry(0x70000000);
    vitreo::winapi::DllExports exports;
    exports.entries.push_back(vitreo::winapi::ExportEntry{
        "HostThunk", 1,
        [](vitreo::winapi::Marshaler& marshaler) {
            marshaler.set_return(7);
        }});
    ASSERT_TRUE(registry.register_dll("test.dll", std::move(exports)).is_ok());
    const auto thunk = registry.get_thunk_address("test.dll", "HostThunk");
    ASSERT_TRUE(thunk.is_ok());
    const vitreo::u32 gateway = static_cast<vitreo::u32>(
        thunk.value() + vitreo::winapi::ThunkRegistry::STUB_SIZE);

    std::array<vitreo::u8, 8> code{
        0xb8,
        static_cast<vitreo::u8>(gateway),
        static_cast<vitreo::u8>(gateway >> 8),
        static_cast<vitreo::u8>(gateway >> 16),
        static_cast<vitreo::u8>(gateway >> 24),
        0xff, 0xd0, // call eax
        0xc3,       // ret to the process-return sentinel
    };
    ASSERT_TRUE(memory.write_bytes(
        code_addr, code.data(), code.size()).is_ok());

    vitreo::cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.rip = code_addr;
    cpu.set_sp(initial_sp);
    // The program ends the way an entry point does: it returns. (HLT at
    // CPL 3 is #GP(0), Intel SDM Vol. 2 HLT -- STATUS_PRIVILEGED_INSTRUCTION.)
    constexpr vitreo::u32 sentinel =
        static_cast<vitreo::u32>(PROCESS_RETURN_SENTINEL);
    ASSERT_TRUE(memory.write_u32(initial_sp, sentinel).is_ok());
    vitreo::winapi::ProcessManager process_mgr;
    vitreo::cli::RunConfig config;
    config.max_instructions = 32;

    const vitreo::cli::RunResult result = run_execution_loop(
        cpu, memory, registry, process_mgr, config);
    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(memory.read_u32(initial_sp - 0x100).value_or(0xffffffffu), 0u);
    EXPECT_EQ(memory.read_u32(initial_sp - 4).value_or(0), code_addr + 7);
    // The caller's frame above the thunk's ESP is untouched.
    EXPECT_EQ(memory.read_u32(initial_sp).value_or(0), sentinel);
    EXPECT_EQ(memory.read_u32(initial_sp + 4).value_or(0), 0xa5a5a5a5u);
}

TEST(ThunkCalleeStackContractTest,
     X86CallbackDispatchClobbersOnlyBelowThunkEsp) {
    constexpr vitreo::GuestAddr stack_base = 0x2f000;
    constexpr vitreo::u32 initial_sp = 0x2fff0;

    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());
    std::array<vitreo::u8, 0x1000> stack_paint{};
    stack_paint.fill(0xa5);
    ASSERT_TRUE(memory.write_bytes(
        stack_base, stack_paint.data(), stack_paint.size()).is_ok());

    vitreo::winapi::ThunkRegistry registry(0x70000000);
    vitreo::winapi::DllExports exports;
    exports.entries.push_back(vitreo::winapi::ExportEntry{
        "CallbackHostThunk", 1,
        [](vitreo::winapi::Marshaler& marshaler) {
            marshaler.set_return(9);
        }});
    ASSERT_TRUE(registry.register_dll("test.dll", std::move(exports)).is_ok());
    const auto thunk = registry.get_thunk_address(
        "test.dll", "CallbackHostThunk");
    ASSERT_TRUE(thunk.is_ok());

    vitreo::cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(initial_sp);
    vitreo::winapi::ProcessManager process_mgr;
    bool completed = false;
    EXPECT_EQ(run_guest_callback(
                  cpu, memory, registry, process_mgr,
                  thunk.value() + vitreo::winapi::ThunkRegistry::STUB_SIZE,
                  nullptr, 0, false, &completed),
              9u);
    EXPECT_TRUE(completed);
    EXPECT_EQ(memory.read_u32(initial_sp - 0x100).value_or(0xffffffffu), 0u);
    EXPECT_EQ(memory.read_u32(initial_sp - 4).value_or(0),
              static_cast<vitreo::u32>(CALLBACK_RETURN_SENTINEL));
    EXPECT_EQ(memory.read_u32(initial_sp).value_or(0), 0xa5a5a5a5u);
}

TEST(ThunkCalleeSavedRegisterTest,
     X64MainDispatchPreservesNonvolatileXmmRegisters) {
    constexpr vitreo::GuestAddr code_addr = 0x21000;
    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());

    vitreo::winapi::ThunkRegistry registry(0x70000000);
    vitreo::winapi::DllExports exports;
    exports.entries.push_back(vitreo::winapi::ExportEntry{
        "ClobberXmm", 1,
        [](vitreo::winapi::Marshaler& marshaler) {
            marshaler.cpu()->sse.xmm[6].u64_[0] = 0x6666666666666666ull;
            marshaler.cpu()->sse.xmm[15].u64_[1] = 0xffffffffffffffffull;
            marshaler.cpu()->sse.mxcsr = 0;
            marshaler.set_return(7);
        }});
    ASSERT_TRUE(registry.register_dll("test.dll", std::move(exports)).is_ok());
    const auto thunk = registry.get_thunk_address("test.dll", "ClobberXmm");
    ASSERT_TRUE(thunk.is_ok());
    const vitreo::u64 gateway =
        thunk.value() + vitreo::winapi::ThunkRegistry::STUB_SIZE;

    std::array<vitreo::u8, 13> code{
        0x48, 0xb8,
        static_cast<vitreo::u8>(gateway),
        static_cast<vitreo::u8>(gateway >> 8),
        static_cast<vitreo::u8>(gateway >> 16),
        static_cast<vitreo::u8>(gateway >> 24),
        static_cast<vitreo::u8>(gateway >> 32),
        static_cast<vitreo::u8>(gateway >> 40),
        static_cast<vitreo::u8>(gateway >> 48),
        static_cast<vitreo::u8>(gateway >> 56),
        0xff, 0xd0, 0xc3,  // call rax ; ret to the process-return sentinel
    };
    ASSERT_TRUE(memory.write_bytes(code_addr, code.data(), code.size()).is_ok());

    vitreo::cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.rip = code_addr;
    cpu.set_sp(0x2fff0);
    ASSERT_TRUE(memory.write_u64(0x2fff0, PROCESS_RETURN_SENTINEL).is_ok());
    cpu.sse.xmm[6].u64_[0] = 6;
    cpu.sse.xmm[15].u64_[1] = 15;
    cpu.sse.mxcsr = 0x1f80;
    vitreo::winapi::ProcessManager process_mgr;
    RunConfig config;
    config.max_instructions = 16;

    const RunResult result = run_execution_loop(
        cpu, memory, registry, process_mgr, config);
    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(cpu.sse.xmm[6].u64_[0], 6u);
    EXPECT_EQ(cpu.sse.xmm[15].u64_[1], 15u);
    EXPECT_EQ(cpu.sse.mxcsr & 0xffc0u, 0x1f80u);
}

TEST(ThunkCalleeSavedRegisterTest,
     X64CallbackDispatchPreservesNonvolatileXmmRegisters) {
    constexpr vitreo::GuestAddr code_addr = 0x21000;
    constexpr vitreo::GuestAddr output_addr = 0x22000;
    vitreo::GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        0x20000, 0x10000, vitreo::MemoryProtection::ReadWrite).is_ok());

    vitreo::winapi::ThunkRegistry registry(0x70000000);
    vitreo::winapi::DllExports exports;
    exports.entries.push_back(vitreo::winapi::ExportEntry{
        "ClobberCallbackXmm", 1,
        [](vitreo::winapi::Marshaler& marshaler) {
            marshaler.cpu()->sse.xmm[6].u64_[0] = 0xfeedfacecafebeefull;
            marshaler.set_return(0);
        }});
    ASSERT_TRUE(registry.register_dll("test.dll", std::move(exports)).is_ok());
    const auto thunk = registry.get_thunk_address(
        "test.dll", "ClobberCallbackXmm");
    ASSERT_TRUE(thunk.is_ok());
    const vitreo::u64 gateway =
        thunk.value() + vitreo::winapi::ThunkRegistry::STUB_SIZE;

    std::array<vitreo::u8, 15> code{
        0x48, 0xb8,
        static_cast<vitreo::u8>(gateway),
        static_cast<vitreo::u8>(gateway >> 8),
        static_cast<vitreo::u8>(gateway >> 16),
        static_cast<vitreo::u8>(gateway >> 24),
        static_cast<vitreo::u8>(gateway >> 32),
        static_cast<vitreo::u8>(gateway >> 40),
        static_cast<vitreo::u8>(gateway >> 48),
        static_cast<vitreo::u8>(gateway >> 56),
        0xff, 0xd0,
        0xf3, 0x0f, 0x7f,
    };
    const std::array<vitreo::u8, 2> tail{0x31, 0xc3};
    ASSERT_TRUE(memory.write_bytes(code_addr, code.data(), code.size()).is_ok());
    ASSERT_TRUE(memory.write_bytes(
        code_addr + code.size(), tail.data(), tail.size()).is_ok());

    vitreo::cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.set_sp(0x2fff0);
    cpu.sse.xmm[6].u64_[0] = 0x123456789abcdef0ull;
    cpu.sse.xmm[6].u64_[1] = 0x0fedcba987654321ull;
    vitreo::winapi::ProcessManager process_mgr;
    const std::array<vitreo::u64, 1> args{output_addr};
    bool completed = false;

    EXPECT_EQ(run_guest_callback(
                  cpu, memory, registry, process_mgr, code_addr,
                  args.data(), args.size(), true, &completed),
              0u);
    EXPECT_TRUE(completed);
    EXPECT_EQ(memory.read_u64(output_addr).value_or(0),
              0x123456789abcdef0ull);
    EXPECT_EQ(memory.read_u64(output_addr + 8).value_or(0),
              0x0fedcba987654321ull);
    EXPECT_EQ(cpu.sse.xmm[6].u64_[0], 0x123456789abcdef0ull);
    EXPECT_EQ(cpu.sse.xmm[6].u64_[1], 0x0fedcba987654321ull);
}
