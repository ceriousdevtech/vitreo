// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — on-demand Comba bignum throughput benchmark

#include "jit_exec_test_fixture.hpp"

// Throughput benchmark on the real Battle.net Agent OpenSSL bignum hot block
// (guest VA 0x604c50 — the unrolled Comba squaring accumulation that dominates
// RSA-4096 keygen). DISABLED_ so it only runs on demand:
//   ./vitreo_tests --gtest_also_run_disabled_tests \
//                  --gtest_filter='*CombaBignumThroughput*'
// Reports emitted-ARM expansion and JIT-vs-interpreter speedup so an
// optimization can be A/B'd offline without a 19-minute live keygen.
TEST_F(JitExecTest, DISABLED_CombaBignumThroughput) {
    static const u8 comba[] = {
        0x55, 0x8b, 0xec, 0x83, 0xec, 0x0c, 0x53, 0x8b, 0x5d, 0x0c, 0x56, 0x33,
        0xf6, 0x57, 0x8b, 0x03, 0xf7, 0xe0, 0x8b, 0xca, 0x8b, 0x55, 0x08, 0x89,
        0x02, 0x8b, 0x43, 0x04, 0xf7, 0x23, 0x03, 0xc8, 0x8b, 0xfa, 0x13, 0xf7,
        0x03, 0xc1, 0x8b, 0x4d, 0x08, 0x83, 0xd7, 0x00, 0x03, 0xf7, 0x89, 0x41,
        0x04, 0x8b, 0x43, 0x04, 0xf7, 0xe0, 0x8b, 0x4d, 0x0c, 0x8b, 0xd8, 0x03,
        0xde, 0x83, 0xd2, 0x00, 0x3b, 0xf7, 0x1b, 0xc0, 0xf7, 0xd8, 0x03, 0xc2,
        0x3b, 0xc2, 0x89, 0x45, 0xf4, 0x1b, 0xc0, 0xf7, 0xd8, 0x89, 0x45, 0xf8,
        0x8b, 0x41, 0x08, 0xf7, 0x21, 0x33, 0xc9, 0x8b, 0xf0, 0x8b, 0xfa, 0x8b,
        0x55, 0xf4, 0x03, 0xde, 0x13, 0xcf, 0x03, 0xd1, 0x3b, 0xd1, 0x8b, 0x4d,
        0x0c, 0x1b, 0xc0, 0xf7, 0xd8, 0x01, 0x45, 0xf8, 0x03, 0xf3, 0x8b, 0x45,
        0x08, 0x83, 0xd7, 0x00, 0x03, 0xd7, 0x89, 0x55, 0xf4, 0x89, 0x70, 0x08,
    };

    // The routine starts with push ebp; mov ebp,esp, so with ESP=0x10000 its
    // arguments are at 0x10004 and 0x10008 after the prologue establishes
    // EBP=0xfffc. Seed those stack slots so all bignum accesses stay mapped.
    auto seed = [&]() {
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->rip = 0x1000;
        (void)m_memory->write_u32(0x10004, 0x9000); // [ebp+8]  -> output buf
        (void)m_memory->write_u32(0x10008, 0xA000); // [ebp+0xc]-> input buf
    };

    BasicBlock block = build_block(comba, sizeof(comba));
    ASSERT_GT(block.instruction_count, 20u);
    const u32 guest_bytes = block.byte_length;

    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);

    const int iters = 200000;

    seed();
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->rip = 0x1000;
        (void)test::execute_like_dispatcher(*m_jit, *cb, *m_cpu);
    }
    auto t1 = std::chrono::steady_clock::now();

    seed();
    auto t2 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->rip = 0x1000;
        Interpreter interp(*m_cpu, *m_memory);
        (void)interp.run(block.instruction_count);
    }
    auto t3 = std::chrono::steady_clock::now();

    const double jit_ns =
        std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
    const double interp_ns =
        std::chrono::duration<double, std::nano>(t3 - t2).count() / iters;

    std::cout << "\n=== Comba bignum hot-block throughput ===\n"
              << "guest bytes/block:   " << guest_bytes << "\n"
              << "guest insns/block:   " << block.instruction_count << "\n"
              << "emitted ARM bytes:   " << cb->code_size
              << "  (" << (double)cb->code_size / guest_bytes << " x guest,  "
              << (double)cb->code_size / 4 / block.instruction_count
              << " ARM insns/guest insn)\n"
              << "JIT   ns/block:      " << jit_ns << "\n"
              << "interp ns/block:     " << interp_ns << "\n"
              << "JIT speedup:         " << interp_ns / jit_ns << " x\n"
              << std::endl;
}
