// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Randomized multi-instruction SSE blocks, linear64 JIT against the
// interpreter. Split from test_jit_linear.cpp; shared fixtures and matchers
// live in jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

// Randomized multi-instruction SSE blocks, JIT against the interpreter: every
// XMM register, every GPR and the data pages must agree after each block.
// Single-instruction differential sweeps (the ISA matrix) cannot see state
// carried between instructions -- forwarded vector values, cached registers,
// a slow path that rejoins the fast one -- so the blocks here mix packed and
// scalar FP, integer and bitwise SSE, register copies, aligned and unaligned
// loads and stores (some straddling a host page, which takes the helper
// path), divides (whose NaN/zero arm is out of line), and GPR arithmetic.
TEST_F(JitLinear64Test, RandomSseBlocksMatchInterpreter) {
    constexpr int kBlocks = 2000;
    constexpr usize kFuzzCodeSize = 0x100000;  // > (kBlocks + 1) * 0x200, host pages
    static_assert(kFuzzCodeSize >= (kBlocks + 1) * 0x200);
    // Pass 1 repeats the blocks with V16-V30 overwritten after every C++
    // helper call (VITREO_JIT_PROBE_POISON_VREGS, a TEST PROBE): AAPCS64 lets
    // a callee do that, but these helpers rarely do, so without the poison a
    // cached XMM that survives a helper by luck would pass. Each pass has its
    // own code region so no address is rewritten (self-modifying code).
    for (int pass = 0; pass < 2; ++pass) {
        ScopedEnvironmentFlag poison("VITREO_JIT_PROBE_POISON_VREGS",
                                     pass == 1 ? "1" : nullptr);
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_jit->set_block_cache(m_cache.get());
        constexpr GuestAddr kSecond = kData64 + 0x4000;
        ASSERT_TRUE(m_memory->map_region(kSecond, 0x4000, MemoryProtection::ReadWrite));
        // RBX sits so that [rbx + disp8] reaches both host pages.
        constexpr GuestAddr kBase = kData64 + 0x3FC0;
        u64 rng = 0x9E3779B97F4A7C15ull;
        const auto next = [&rng]() {
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            return rng;
        };
        const auto pick = [&](u32 n) { return static_cast<u32>(next() % n); };
        // A float lane: mostly ordinary values, sometimes a special one.
        const auto lane32 = [&]() -> u32 {
            switch (pick(16)) {
                case 0: return 0x7FC00000u | pick(0x400000);  // QNaN
                case 1: return 0xFF800000u;                    // -inf
                case 2: return 0x00000001u + pick(0x7FFFFF);   // denormal
                case 3: return 0x80000000u;                    // -0
                default: {
                    const u32 exp = 110u + pick(36);
                    return (pick(2) << 31) | (exp << 23) | pick(0x800000);
                }
            }
        };
        struct Encoded { std::vector<u8> bytes; };
        // Legacy SSE reg-reg: [pfx] [REX] 0F op modrm(11 reg rm) [imm8].
        const auto sse_rr = [&](u8 pfx, u8 op, u8 d, u8 sreg, int imm = -1) {
            std::vector<u8> b;
            if (pfx) b.push_back(pfx);
            const u8 rex = 0x40 | ((d >> 3) << 2) | (sreg >> 3);
            if (rex != 0x40) b.push_back(rex);
            b.push_back(0x0F); b.push_back(op);
            b.push_back(static_cast<u8>(0xC0 | ((d & 7) << 3) | (sreg & 7)));
            if (imm >= 0) b.push_back(static_cast<u8>(imm));
            return b;
        };
        // Legacy SSE with [rbx + disp8].
        const auto sse_rm = [&](u8 pfx, u8 op, u8 r, i8 disp, int imm = -1) {
            std::vector<u8> b;
            if (pfx) b.push_back(pfx);
            if (r >= 8) b.push_back(0x44);
            b.push_back(0x0F); b.push_back(op);
            b.push_back(static_cast<u8>(0x40 | ((r & 7) << 3) | 3));
            b.push_back(static_cast<u8>(disp));
            if (imm >= 0) b.push_back(static_cast<u8>(imm));
            return b;
        };
        struct Op { u8 pfx; u8 op; bool imm; };
        const Op rr_ops[] = {
            {0x00, 0x58, false}, {0x66, 0x58, false}, {0xF3, 0x58, false},
            {0xF2, 0x58, false}, {0x00, 0x59, false}, {0x66, 0x59, false},
            {0xF3, 0x59, false}, {0xF2, 0x59, false}, {0x00, 0x5C, false},
            {0x66, 0x5C, false}, {0x00, 0x5E, false}, {0xF2, 0x5E, false},
            {0x00, 0x57, false}, {0x00, 0x54, false}, {0x00, 0x56, false},
            {0x66, 0xEF, false}, {0x66, 0xFE, false}, {0x66, 0xD4, false},
            {0x00, 0x28, false}, {0x00, 0x10, false}, {0xF3, 0x10, false},
            {0xF2, 0x10, false}, {0x00, 0xC6, true},  {0x00, 0x14, false},
            {0x66, 0x6C, false}, {0x00, 0x5A, false}, {0x66, 0x5A, false},
            {0x66, 0x70, true},  {0x00, 0x5D, false}, {0x66, 0x5F, false},
            {0xF3, 0x5C, false}, {0xF2, 0x5C, false}, {0x00, 0x29, false},
            {0x00, 0x11, false}, {0xF3, 0x11, false}, {0x66, 0x6F, false},
            {0x66, 0x7F, false}, {0x66, 0x62, false}, {0x00, 0x15, false},
            {0x66, 0x6D, false}, {0x66, 0xDB, false}, {0x66, 0xFA, false},
            {0x00, 0x55, false}, {0x00, 0xC2, true},  {0x66, 0xC2, true},
            {0x66, 0xC6, true},  {0xF2, 0x70, true},  {0xF3, 0x70, true},
            {0x00, 0x2F, false}, {0x66, 0x2E, false},  // comiss, ucomisd
            {0xF3, 0x5D, false}, {0xF2, 0x5F, false},  // minss, maxsd
        };
        // GPR filler that never touches RBX or RSP.
        const std::vector<std::vector<u8>> gpr_ops = {
            {0x48, 0x01, 0xC8}, {0x48, 0x29, 0xC2}, {0x48, 0x39, 0xC8},
            {0x31, 0xD1},       {0x49, 0x89, 0xC0}, {0x48, 0x8D, 0x14, 0x48},
            {0x48, 0x0F, 0xAF, 0xC1},
        };
        // One fresh code address per block: rewriting one address repeatedly is
        // self-modifying code to the JIT, which demotes a churning address.
        const GuestAddr kFuzzCode = 0x0000000150000000ull + pass * kFuzzCodeSize;
        ASSERT_TRUE(m_memory->map_region(kFuzzCode, kFuzzCodeSize,
                                         MemoryProtection::ReadWriteExecute));
        GuestAddr code_at = kFuzzCode;
        int failures = 0;
        int declined = 0;
        int compared = 0;
        for (int blk = 0; blk < kBlocks && failures < 3; ++blk) {
            std::vector<u8> code;
            std::ostringstream listing;
            const int n = 3 + static_cast<int>(pick(18));
            for (int i = 0; i < n; ++i) {
                std::vector<u8> b;
                const u32 kind = pick(10);
                const u8 d = static_cast<u8>(pick(16)), r2 = static_cast<u8>(pick(16));
                if (kind < 6) {
                    const Op& o = rr_ops[pick(sizeof(rr_ops) / sizeof(rr_ops[0]))];
                    b = sse_rr(o.pfx, o.op, d, r2, o.imm ? static_cast<int>(pick(256)) : -1);
                } else if (kind < 9) {
                    // Memory: aligned forms use 16-aligned displacements (RBX is
                    // 16-aligned); unaligned forms any displacement in range.
                    const i8 disp_al = static_cast<i8>(-64 + 16 * static_cast<int>(pick(8)));
                    const i8 disp_any = static_cast<i8>(-64 + static_cast<int>(pick(120)));
                    switch (pick(18)) {
                        case 0: b = sse_rm(0x00, 0x10, d, disp_any); break;  // movups x,[m]
                        case 1: b = sse_rm(0x00, 0x11, d, disp_any); break;  // movups [m],x
                        case 2: b = sse_rm(0x00, 0x28, d, disp_al); break;   // movaps x,[m]
                        case 3: b = sse_rm(0x00, 0x29, d, disp_al); break;   // movaps [m],x
                        case 4: b = sse_rm(0xF3, 0x10, d, disp_any); break;  // movss x,[m]
                        case 5: b = sse_rm(0xF2, 0x11, d, disp_any); break;  // movsd [m],x
                        case 6: b = sse_rm(0x00, 0x58, d, disp_al); break;   // addps x,[m]
                        case 7: b = sse_rm(0x66, 0x59, d, disp_al); break;   // mulpd x,[m]
                        case 8: b = sse_rm(0x66, 0xEF, d, disp_al); break;   // pxor x,[m]
                        case 9: b = sse_rm(0x00, 0x54, d, disp_al); break;   // andps x,[m]
                        case 10: b = sse_rm(0x66, 0xFE, d, disp_al); break;  // paddd x,[m]
                        case 11: b = sse_rm(0x00, 0x5C, d, disp_al); break;  // subps x,[m]
                        case 12: b = sse_rm(0x00, 0x5E, d, disp_al); break;  // divps x,[m]
                        case 13: b = sse_rm(0x66, 0x70, d, disp_al, static_cast<int>(pick(256))); break;  // pshufd x,[m],i
                        case 14: b = sse_rm(0x66, 0x62, d, disp_al); break;  // punpckldq x,[m]
                        case 15: b = sse_rm(0xF2, 0x59, d, disp_any); break; // mulsd x,[m]
                        case 16: b = sse_rm(0x00, 0xC6, d, disp_al, static_cast<int>(pick(256))); break;  // shufps x,[m],i
                        default: b = sse_rm(0xF3, 0x6F, d, disp_any); break; // movdqu x,[m]
                    }
                } else {
                    b = gpr_ops[pick(static_cast<u32>(gpr_ops.size()))];
                }
                for (u8 x : b) listing << std::hex << std::setw(2) << std::setfill('0') << int(x) << ' ';
                listing << "| ";
                code.insert(code.end(), b.begin(), b.end());
            }
            code_at += 0x200;
            ASSERT_LE(code_at + code.size(), kFuzzCode + kFuzzCodeSize);
            ASSERT_TRUE(m_memory->write_bytes(code_at, code.data(), code.size()));
            BasicBlock block = build64(code);
            block.start_addr = code_at;
            if (block.instruction_count != static_cast<u32>(n)) continue;  // decoder split
            CompiledBlock* compiled = m_jit->compile(block);
            if (compiled == nullptr) {  // declined: the dispatcher interprets it
                ++declined;
                continue;
            }
            ++compared;

            // Fresh random state: data pages and all sixteen XMM registers.
            std::vector<u8> page(0x8000);
            for (usize i = 0; i < page.size(); i += 4) {
                const u32 v = lane32();
                std::memcpy(&page[i], &v, 4);
            }
            CpuState seed = cpu64(kBase);
            seed.rip = code_at;
            for (u8 r = 0; r < 16; ++r) {
                for (int l = 0; l < 4; ++l) seed.sse.xmm[r].u32_[l] = lane32();
                if (r != EBX && r != ESP) seed.gpr.set64(r, next());
            }
            ASSERT_TRUE(m_memory->write_bytes(kData64, page.data(), page.size()));
            CpuState jcpu = seed;
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, jcpu), JitExecResult::Continue);
            std::vector<u8> jit_mem(page.size());
            ASSERT_TRUE(m_memory->read_bytes(kData64, jit_mem.data(), jit_mem.size()));
            ASSERT_TRUE(m_memory->write_bytes(kData64, page.data(), page.size()));
            CpuState icpu = seed;
            Interpreter interp(icpu, *m_memory);
            (void)interp.run(static_cast<u64>(n));
            std::vector<u8> int_mem(page.size());
            ASSERT_TRUE(m_memory->read_bytes(kData64, int_mem.data(), int_mem.size()));

            bool ok = !jcpu.pending_raise.active && jcpu.rip == icpu.rip;
            for (u8 r = 0; r < 16 && ok; ++r) {
                ok = std::memcmp(&jcpu.sse.xmm[r], &icpu.sse.xmm[r], 16) == 0 &&
                     jcpu.gpr.get64(r) == icpu.gpr.get64(r);
            }
            ok = ok && jit_mem == int_mem;
            if (!ok) {
                ++failures;
                ADD_FAILURE() << "block " << blk << " diverged: " << listing.str();
                for (u8 r = 0; r < 16; ++r) {
                    if (std::memcmp(&jcpu.sse.xmm[r], &icpu.sse.xmm[r], 16) != 0) {
                        ADD_FAILURE() << "  xmm" << int(r) << std::hex
                                      << " jit " << jcpu.sse.xmm[r].u64_[1] << ':'
                                      << jcpu.sse.xmm[r].u64_[0] << " interp "
                                      << icpu.sse.xmm[r].u64_[1] << ':'
                                      << icpu.sse.xmm[r].u64_[0];
                    }
                    if (jcpu.gpr.get64(r) != icpu.gpr.get64(r)) {
                        ADD_FAILURE() << "  r" << int(r) << std::hex << " jit "
                                      << jcpu.gpr.get64(r) << " interp "
                                      << icpu.gpr.get64(r);
                    }
                }
                if (jit_mem != int_mem) ADD_FAILURE() << "  memory differs";
                if (jcpu.pending_raise.active) ADD_FAILURE() << "  jit raised";
            }
        }
        EXPECT_GT(compared, kBlocks * 9 / 10) << declined << " blocks declined";
        ASSERT_TRUE(m_memory->unmap_region(kSecond, 0x4000));
        ASSERT_TRUE(m_memory->unmap_region(kFuzzCode, kFuzzCodeSize));
    }
}

}  // namespace
