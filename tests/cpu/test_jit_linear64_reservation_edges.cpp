// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Linear64 store faults and the reservation-edge, straddle and constant-address
// decisions. Split from test_jit_linear.cpp; shared fixtures and matchers live
// in jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

TEST_F(JitLinear64Test, ScalarStoreWidthsAbove4GiBMatchX64MovSemantics) {
    struct Case {
        std::vector<u8> code;
        u64 value;
        usize width;
    };
    const std::vector<Case> cases = {
        {{0x88, 0x03}, 0x0123456789ABCDEFull, 1}, // mov [rbx],al
        {{0x66, 0x89, 0x03}, 0x0123456789ABCDEFull, 2}, // mov [rbx],ax
        {{0x89, 0x03}, 0x0123456789ABCDEFull, 4}, // mov [rbx],eax
        {{0x48, 0x89, 0x03}, 0x0123456789ABCDEFull, 8}, // mov [rbx],rax
    };

    for (const Case& c : cases) {
        std::array<u8, 16> expected;
        expected.fill(0xA5);
        std::memcpy(expected.data() + 4, &c.value, c.width);
        std::array<u8, 16> actual;
        actual.fill(0xA5);
        ASSERT_TRUE(m_memory->write_bytes(kData64, actual.data(), actual.size()));
        ASSERT_TRUE(m_memory->write_bytes(kCode64, c.code.data(), c.code.size()));

        BasicBlock block = build64(c.code);
        ASSERT_EQ(block.instruction_count, 1u);
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        CpuState cpu = cpu64(kData64 + 4);
        cpu.gpr.set64(EAX, c.value);
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
        EXPECT_FALSE(cpu.pending_raise.active);
        ASSERT_TRUE(m_memory->read_bytes(kData64, actual.data(), actual.size()));
        EXPECT_EQ(actual, expected) << "store width=" << c.width;
    }
}

TEST_F(JitLinear64Test, OutOfWindowStoreUsesCheckedHelperWithFullAddress) {
    const std::vector<u8> code = {0x89, 0x03}; // mov [rbx],eax
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    constexpr GuestAddr outside = 0x00000007F0000040ull;
    CpuState cpu = cpu64(outside);
    cpu.gpr.set32(EAX, 0x12345678u);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.args[0], 1u);
    EXPECT_EQ(cpu.pending_raise.args[1], outside);
}

TEST_F(JitLinear64Test, InWindowUnmappedDirectStoreRaisesPreciseWriteAv) {
    const std::vector<u8> code = {0x89, 0x03}; // mov [rbx],eax
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    const u32 words = compiled->code_size / 4;
    std::vector<u32> emitted(words);
    std::memcpy(emitted.data(), compiled->code, compiled->code_size);
    ASSERT_TRUE(has_guarded_linear64_store(emitted, 4))
        << "test did not reach the guarded ordered linear64 store path";

    CpuState cpu = cpu64(kUnmapped64);
    for (u8 reg = 0; reg < 16; ++reg) {
        cpu.gpr.set64(reg, 0x5A00000000000000ull + reg);
    }
    cpu.gpr.set64(EAX, 0x12345678ABCDEF01ull);
    cpu.gpr.set64(EBX, kUnmapped64);
    cpu.flags.rflags = 0x8D7u;
    std::array<u64, 16> expected_gpr{};
    for (u8 reg = 0; reg < 16; ++reg) {
        expected_gpr[reg] = cpu.gpr.get64(reg);
    }

    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_TRUE(cpu.pending_raise.from_helper);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.flags, 0u);
    EXPECT_EQ(cpu.pending_raise.num_args, 2u);
    EXPECT_EQ(cpu.pending_raise.args[0], 1u);
    EXPECT_EQ(cpu.pending_raise.args[1], kUnmapped64);
    EXPECT_EQ(cpu.rip, kCode64);
    EXPECT_EQ(cpu.flags.rflags, 0x8D7u);
    for (u8 reg = 0; reg < 16; ++reg) {
        EXPECT_EQ(cpu.gpr.get64(reg), expected_gpr[reg])
            << "register=" << static_cast<unsigned>(reg);
    }
}

TEST_F(JitLinear64Test, DirectStoreSeesPageBecomeReadOnly) {
    const std::vector<u8> code = {0x48, 0x89, 0x03}; // mov [rbx],rax
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    ASSERT_TRUE(m_memory->write_u64(kData64 + 0x100,
                                    0x0102030405060708ull));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    const u32 words = compiled->code_size / 4;
    std::vector<u32> emitted(words);
    std::memcpy(emitted.data(), compiled->code, compiled->code_size);
    ASSERT_TRUE(has_guarded_linear64_store(emitted, 8))
        << "test did not compile the guarded ordered linear64 store";

    CpuState prime = cpu64(kData64 + 0x100);
    prime.gpr.set64(EAX, 0x1112131415161718ull);
    ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, prime), JitExecResult::Continue);
    ASSERT_FALSE(prime.pending_raise.active);

    ASSERT_TRUE(m_memory->protect_region(kData64, 0x4000,
                                         MemoryProtection::Read));
    CpuState cpu = cpu64(kData64 + 0x100);
    cpu.gpr.set64(EAX, 0xFFEEDDCCBBAA9988ull);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.num_args, 2u);
    EXPECT_EQ(cpu.pending_raise.args[0], 1u);
    EXPECT_EQ(cpu.pending_raise.args[1], kData64 + 0x100);
    auto value = m_memory->read_u64(kData64 + 0x100);
    ASSERT_TRUE(value.is_ok());
    EXPECT_EQ(value.value(), 0x1112131415161718ull);
}

TEST_F(JitLinear64Test, DirectStoreSeesPageBecomeNoAccess) {
    const std::vector<u8> code = {0x48, 0x89, 0x03}; // mov [rbx],rax
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    ASSERT_TRUE(m_memory->write_u64(kData64 + 0x180,
                                    0x2122232425262728ull));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    const u32 words = compiled->code_size / 4;
    std::vector<u32> emitted(words);
    std::memcpy(emitted.data(), compiled->code, compiled->code_size);
    ASSERT_TRUE(has_guarded_linear64_store(emitted, 8))
        << "test did not compile the guarded ordered linear64 store";

    CpuState prime = cpu64(kData64 + 0x180);
    prime.gpr.set64(EAX, 0x3132333435363738ull);
    ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, prime), JitExecResult::Continue);
    ASSERT_FALSE(prime.pending_raise.active);

    ASSERT_TRUE(m_memory->protect_region(kData64, 0x4000,
                                         MemoryProtection::None));
    CpuState cpu = cpu64(kData64 + 0x180);
    cpu.gpr.set64(EAX, 0xAABBCCDDEEFF0011ull);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.num_args, 2u);
    EXPECT_EQ(cpu.pending_raise.args[0], 1u);
    EXPECT_EQ(cpu.pending_raise.args[1], kData64 + 0x180);

    ASSERT_TRUE(m_memory->protect_region(kData64, 0x4000,
                                         MemoryProtection::ReadWrite));
    auto value = m_memory->read_u64(kData64 + 0x180);
    ASSERT_TRUE(value.is_ok());
    EXPECT_EQ(value.value(), 0x3132333435363738ull);
}

TEST_F(JitLinear64Test, StoreStraddlingReservationUsesCheckedHelper) {
    const std::vector<u8> code = {0x48, 0x89, 0x03}; // mov [rbx],rax
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    constexpr GuestAddr straddling = kReservation64 - 4;
    CpuState cpu = cpu64(straddling);
    cpu.gpr.set64(EAX, 0x0123456789ABCDEFull);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.args[0], 1u);
    EXPECT_EQ(cpu.pending_raise.args[1], straddling);
}

// The default load tests only the FIRST byte against the reservation and lets
// the host-page crossing bit catch an access that runs past its end: the
// reservation ends on a 16 KiB host-page boundary, so such an access always
// crosses one. Every width that can straddle (2/4/8) must reach the helper and
// raise at the exact EA (Intel SDM Vol. 3A §6.15 #PF reports the access
// address), never read past the reservation.
TEST_F(JitLinear64Test, LoadStraddlingReservationUsesCheckedHelper) {
    struct Case {
        std::vector<u8> code;
        int width;
    };
    const std::vector<Case> cases = {
        {{0x66, 0x8B, 0x03}, 2},  // mov ax,[rbx]
        {{0x8B, 0x03}, 4},        // mov eax,[rbx]
        {{0x48, 0x8B, 0x03}, 8},  // mov rax,[rbx]
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(::testing::Message() << "width " << c.width);
        ASSERT_TRUE(m_memory->write_bytes(kCode64, c.code.data(),
                                          c.code.size()));
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_jit->set_block_cache(m_cache.get());
        CompiledBlock* compiled = m_jit->compile(build64(c.code));
        ASSERT_NE(compiled, nullptr);
        for (int back = 1; back < c.width; ++back) {
            const GuestAddr straddling =
                kReservation64 - static_cast<GuestAddr>(back);
            CpuState cpu = cpu64(straddling);
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu),
                      JitExecResult::Continue);
            ASSERT_TRUE(cpu.pending_raise.active) << "back " << back;
            EXPECT_EQ(cpu.pending_raise.args[0], 0u);
            EXPECT_EQ(cpu.pending_raise.args[1], straddling);
        }
    }
}

// A compile-time-constant EA (moffs, RIP-relative) has its reservation and
// crossing tests decided while compiling. Each outcome must still give the
// architectural result: above the window and straddling a host page go to the
// checked helper and read the right bytes; inside the window keeps only the
// check-byte test; unmapped raises at the exact address.
TEST_F(JitLinear64Test, ConstantAddressLoadsMatchArchitecturalResults) {
    constexpr GuestAddr kHigh = 0x00007FFA00000000ull;
    ASSERT_GT(kHigh, kReservation64);
    ASSERT_TRUE(m_memory->map_region(kHigh, 0x4000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u64(kHigh + 0x40,
                                    0x1122334455667788ull).is_ok());
    // A second data host page so an 8-byte read can straddle the boundary.
    ASSERT_TRUE(m_memory->map_region(kData64 + 0x4000, 0x4000,
                                     MemoryProtection::ReadWrite));
    const u8 straddle_bytes[8] = {0x11, 0x22, 0x33, 0x44,
                                  0x55, 0x66, 0x77, 0x88};
    ASSERT_TRUE(m_memory->write_bytes(kData64 + 0x3FFC, straddle_bytes, 8));
    ASSERT_TRUE(m_memory->write_u64(kData64 + 0x100,
                                    0x0102030405060708ull).is_ok());

    const auto moffs_load = [](GuestAddr address) {
        std::vector<u8> code = {0x48, 0xA1};  // mov rax,[moffs64]
        for (int i = 0; i < 8; ++i) {
            code.push_back(static_cast<u8>(address >> (8 * i)));
        }
        return code;
    };
    struct Case {
        const char* name;
        std::vector<u8> code;
        bool faults;
        u64 value;
        GuestAddr fault_address;
    };
    // mov rax,[rip+disp32]: next RIP is kCode64 + 7.
    const i32 rip_disp =
        static_cast<i32>(static_cast<i64>(kData64 + 0x100) -
                         static_cast<i64>(kCode64 + 7));
    std::vector<u8> rip_code = {0x48, 0x8B, 0x05};
    for (int i = 0; i < 4; ++i) {
        rip_code.push_back(static_cast<u8>(static_cast<u32>(rip_disp) >> (8 * i)));
    }
    const std::vector<Case> cases = {
        {"above window", moffs_load(kHigh + 0x40), false,
         0x1122334455667788ull, 0},
        {"straddles host page", moffs_load(kData64 + 0x3FFC), false,
         0x8877665544332211ull, 0},
        {"inside window", moffs_load(kData64 + 0x100), false,
         0x0102030405060708ull, 0},
        {"rip-relative inside window", rip_code, false,
         0x0102030405060708ull, 0},
        {"unmapped", moffs_load(kUnmapped64 + 0x20), true, 0,
         kUnmapped64 + 0x20},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        ASSERT_TRUE(m_memory->write_bytes(kCode64, c.code.data(),
                                          c.code.size()));
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_jit->set_block_cache(m_cache.get());
        CompiledBlock* compiled = m_jit->compile(build64(c.code));
        ASSERT_NE(compiled, nullptr);
        // The constant path must actually be reached: no per-access
        // reservation test is emitted for a compile-time EA.
        std::vector<u32> emitted(compiled->code_size / 4);
        std::memcpy(emitted.data(), compiled->code, compiled->code_size);
        EXPECT_EQ(count_inline_linear64_reservation_checks(emitted), 0u);
        CpuState cpu = cpu64(0);
        cpu.gpr.set64(EAX, 0xDEADBEEFDEADBEEFull);
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
        if (c.faults) {
            ASSERT_TRUE(cpu.pending_raise.active);
            EXPECT_EQ(cpu.pending_raise.args[0], 0u);
            EXPECT_EQ(cpu.pending_raise.args[1], c.fault_address);
            continue;
        }
        EXPECT_FALSE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.gpr.get64(EAX), c.value);
    }
}

// Store twin of ConstantAddressLoadsMatchArchitecturalResults: moffs stores
// whose reservation/crossing outcome is decided at compile time. A store asks
// every check-byte reason, so the straddle lands in a MIXED second host page
// (first 4 KiB NOACCESS) and must raise without writing either page.
TEST_F(JitLinear64Test, ConstantAddressStoresMatchArchitecturalResults) {
    constexpr GuestAddr kHigh = 0x00007FFA00000000ull;
    ASSERT_GT(kHigh, kReservation64);
    ASSERT_TRUE(m_memory->map_region(kHigh, 0x4000,
                                     MemoryProtection::ReadWrite));
    constexpr GuestAddr second_host = kData64 + 0x4000;
    ASSERT_TRUE(m_memory->map_region(second_host, 0x4000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->protect_region(second_host, 0x1000,
                                         MemoryProtection::None));

    const auto moffs_store = [](GuestAddr address) {
        std::vector<u8> code = {0x48, 0xA3};  // mov [moffs64],rax
        for (int i = 0; i < 8; ++i) {
            code.push_back(static_cast<u8>(address >> (8 * i)));
        }
        return code;
    };
    constexpr u64 kValue = 0x0123456789ABCDEFull;
    struct Case {
        const char* name;
        GuestAddr address;
        bool faults;
    };
    const std::vector<Case> cases = {
        {"above window", kHigh + 0x40, false},
        {"inside window", kData64 + 0x200, false},
        {"straddles into mixed page", second_host - 4, true},
        {"unmapped", kUnmapped64 + 0x20, true},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        const std::vector<u8> code = moffs_store(c.address);
        ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_jit->set_block_cache(m_cache.get());
        CompiledBlock* compiled = m_jit->compile(build64(code));
        ASSERT_NE(compiled, nullptr);
        std::vector<u32> emitted(compiled->code_size / 4);
        std::memcpy(emitted.data(), compiled->code, compiled->code_size);
        EXPECT_EQ(count_inline_linear64_reservation_checks(emitted), 0u);
        if (c.name[0] == 's') {
            ASSERT_TRUE(m_memory->write_u32(second_host - 4, 0x5A5A5A5Au));
        }
        CpuState cpu = cpu64(0);
        cpu.gpr.set64(EAX, kValue);
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
        if (c.faults) {
            ASSERT_TRUE(cpu.pending_raise.active);
            EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
            EXPECT_EQ(cpu.pending_raise.args[0], 1u);
            if (c.name[0] == 's') {
                EXPECT_EQ(m_memory->read_u32(second_host - 4).value_or(0),
                          0x5A5A5A5Au)
                    << "a faulting straddling store wrote its first page";
            }
            continue;
        }
        EXPECT_FALSE(cpu.pending_raise.active);
        EXPECT_EQ(m_memory->read_u64(c.address).value_or(0), kValue);
    }
}

TEST_F(JitLinear64Test, WrappingStoreUsesCheckedHelperWithoutAliasing) {
    const std::vector<u8> code = {0x48, 0x89, 0x03}; // mov [rbx],rax
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    constexpr GuestAddr wrapping = 0xFFFFFFFFFFFFFFFCull;
    CpuState cpu = cpu64(wrapping);
    cpu.gpr.set64(EAX, 0x0123456789ABCDEFull);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.args[0], 1u);
    EXPECT_EQ(cpu.pending_raise.args[1], wrapping);
}

// Spotify 1.3.6's CEF renderer (2026-10-22) died on a ud2 that only an ADD
// leaving ZF=1 reaches. Its block, libcef RVA 0x3b42c79, erases a vector
// element and returns: `add qword [rdi+0x880], -8; je ud2` with a nonzero
// sum -- a pointer above the linear window -- mid-block. ADD sets ZF exactly
// when the result is zero (Intel SDM Vol. 2A, ADD), so JE falls through,
// TEST RSI,RSI (nonzero) does too, and the block leaves by its JMP with the
// three registers popped. The bytes are the renderer's own; the window's
// inside, where the store needs no helper, is the control.
//
// The ADD publishes its flags lazily for the JE, then stores its sum. Above
// the window that store runs through the shared helper, and when the
// region-slot cache misses (as it does for a 64 KiB granule the thread has
// not stored to yet) the helper calls C++, which clobbers the registers
// holding the flag operands. So each run's ADD target sits in a fresh
// granule, apart from the vector's. Odd runs make the sum zero, which must
// take the JE with the ADD's flags: 8 + (2^64 - 8) = 0, carrying out of bit
// 63 and of bit 3, even parity -- ZF CF AF PF.
TEST_F(JitLinear64Test, MemoryAddImmediateAboveTheWindowSetsTheResultsFlags) {
    constexpr GuestAddr kHigh = 0x00007FFA00000000ull;
    constexpr GuestAddr kGranule = GuestAddr{1} << 16;  // X64_SLOT_GRANULE_SHIFT
    constexpr int kRuns = 4;
    ASSERT_GT(kHigh, kReservation64);
    ASSERT_TRUE(m_memory->map_region(kHigh, 0x4000, MemoryProtection::ReadWrite));
    for (GuestAddr g = 2; g < 2 + kRuns; ++g) {
        ASSERT_TRUE(m_memory->map_region(kHigh + g * kGranule, 0x4000,
                                         MemoryProtection::ReadWrite));
    }
    const std::vector<u8> code = {
        0x48, 0x8B, 0x49, 0xF8,                          // mov rcx,[rcx-8]
        0x48, 0x89, 0x0C, 0x10,                          // mov [rax+rdx],rcx
        0x48, 0x83, 0x87, 0x80, 0x08, 0x00, 0x00, 0xF8,  // add qword [rdi+0x880],-8
        0x74, 0x19,                                      // je +0x19 (the ud2)
        0x48, 0x85, 0xF6,                                // test rsi,rsi
        0x74, 0x1A,                                      // je +0x1a
        0x48, 0x89, 0xF1,                                // mov rcx,rsi
        0x48, 0x83, 0xC4, 0x20,                          // add rsp,0x20
        0x5B, 0x5F, 0x5E,                                // pop rbx; pop rdi; pop rsi
        0xE9, 0xA1, 0xF6, 0xB3, 0x00,                    // jmp +0xb3f6a1
    };
    ASSERT_EQ(code.size(), 38u);
    constexpr GuestAddr kUd2 = kCode64 + 0x2B;
    constexpr GuestAddr kJmpTarget = kCode64 + 38 + 0xB3F6A1;
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    BasicBlock block = build64(code);
    ASSERT_EQ(block.instruction_count, 12u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    for (const GuestAddr base : {kHigh, kData64}) {
        for (int run = 0; run < kRuns; ++run) {
            const bool zero_sum = (run & 1) != 0;
            SCOPED_TRACE(::testing::Message()
                         << (base == kHigh ? "above the window" : "inside the window")
                         << " run " << run << (zero_sum ? " (zero sum)" : ""));
            const GuestAddr begin = base + 0x220;
            const GuestAddr end = begin + 0x20;
            const GuestAddr owner =
                (base == kHigh ? kHigh + static_cast<GuestAddr>(2 + run) * kGranule
                               : base) +
                0x100;
            const GuestAddr stack = kData64 + 0x1000;
            constexpr u64 kBack = 0x5D0400DEFA70ull;
            constexpr u64 kRsi = 0x5D0400584190ull;
            ASSERT_TRUE(m_memory->write_u64(end - 8, kBack));
            ASSERT_TRUE(m_memory->write_u64(begin + 0x10, 0x5D0400DEFAB8ull));
            const u64 before = zero_sum ? 8u : end;
            ASSERT_TRUE(m_memory->write_u64(owner + 0x880, before));
            ASSERT_TRUE(m_memory->write_u64(stack + 0x20, 0x1111));  // rbx
            ASSERT_TRUE(m_memory->write_u64(stack + 0x28, 0x2222));  // rdi
            ASSERT_TRUE(m_memory->write_u64(stack + 0x30, 0x3333));  // rsi
            CpuState cpu = cpu64(0);
            cpu.gpr.set64(EAX, begin);
            cpu.gpr.set64(1, end);    // RCX
            cpu.gpr.set64(2, 0x10);   // RDX
            cpu.gpr.set64(ESP, stack);
            cpu.gpr.set64(6, kRsi);   // RSI
            cpu.gpr.set64(7, owner);  // RDI
            // ZF opposite the answer before: the ADD must set it.
            cpu.flags.rflags = zero_sum ? 0x202u : 0x242u;
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu),
                      JitExecResult::Continue);
            ASSERT_FALSE(cpu.pending_raise.active);
            EXPECT_EQ(m_memory->read_u64(owner + 0x880).value_or(1), before - 8);
            EXPECT_EQ(m_memory->read_u64(begin + 0x10).value_or(0), kBack);
            if (zero_sum) {
                EXPECT_EQ(cpu.rip, kUd2) << "JE not taken on a zero sum";
                cpu.materialize_lazy_flags();
                EXPECT_EQ(cpu.flags.rflags & 0x8D5u, 0x055u) << "ZF CF AF PF";
                continue;
            }
            EXPECT_NE(cpu.rip, kUd2) << "JE taken on a nonzero sum";
            EXPECT_EQ(cpu.rip, kJmpTarget);
            EXPECT_EQ(cpu.gpr.get64(1), kRsi);  // RCX
            EXPECT_EQ(cpu.gpr.get64(EBX), 0x1111u);
            EXPECT_EQ(cpu.gpr.get64(7), 0x2222u);
            EXPECT_EQ(cpu.gpr.get64(6), 0x3333u);
            EXPECT_EQ(cpu.gpr.get64(ESP), stack + 0x38);
        }
    }
}

}  // namespace
