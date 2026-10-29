// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — x64 Jcc-first blocks

#include "jit_exec_test_fixture.hpp"

// ---------------------------------------------------------------------------
// x64 Jcc-first blocks: carried NZCV from a linked predecessor.
//
// The 64-bit terminal-Jcc lowering branches natively on the predecessor's
// published NZCV when cpu.nzcv_kind is nonzero, and falls back to the lazy /
// RFLAGS reconstruction when it is zero. Both edges must match the
// interpreter, which is the CPU semantic reference.
// ---------------------------------------------------------------------------
namespace {

// Decodes `code` at `address` as a 64-bit block. Shared by the tests below.
BasicBlock decode_block64_at(GuestMemory& memory, GuestAddr address,
                             const u8* code, usize length) {
    EXPECT_TRUE(memory.write_bytes(address, code, length));
    BasicBlock block;
    block.start_addr = address;
    block.is_64bit = true;
    MemoryByteReader reader(code, length);
    while (reader.remaining() > 0) {
        DecodedInstruction instruction;
        if (decode_instruction(reader, DecodeMode::Mode64, instruction) !=
            DecodeError::Success) {
            break;
        }
        block.instructions.push_back(instruction);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    return block;
}

} // namespace

TEST_F(JitExecTest, Mode64LinkedCmpThenJccFirstMatchesInterpreter) {
    // Block A: cmp rax, rbx (publishes NZCV + nzcv_kind).
    // Block B: a Jcc-first terminal branch that must consume it natively.
    struct LinkGuard {
        ~LinkGuard() { JitCompiler::disable_jit_link(); }
    } link_guard;

    // Every signed and unsigned condition the carried path may fuse, plus a
    // parity condition that must NOT (no NZCV equivalent).
    const struct {
        const char* name;
        u8 opcode_byte;
    } conds[] = {
        {"jb",  0x72}, {"jnb", 0x73}, {"jz",  0x74}, {"jnz", 0x75},
        {"jbe", 0x76}, {"ja",  0x77}, {"js",  0x78}, {"jns", 0x79},
        {"jp",  0x7A}, {"jnp", 0x7B},
        {"jl",  0x7C}, {"jge", 0x7D}, {"jle", 0x7E}, {"jg",  0x7F},
    };

    const std::pair<u64, u64> pairs[] = {
        {5, 3}, {3, 5}, {5, 5}, {0, 1}, {1, 0},
        {0x8000000000000000ull, 1}, {1, 0x8000000000000000ull},
        {0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull},
        {0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull},
        {3, 3}, {0x101, 0x100},
    };

    // Each condition gets its own guest addresses: recompiling one address
    // repeatedly trips the JIT churn guard and routes it to the interpreter.
    GuestAddr head_addr = 0x1000;
    for (const auto& cond : conds) {
        const GuestAddr jcc_addr = head_addr + 3;
        BlockCache cache;
        m_jit->set_block_cache(&cache);
        JitCompiler::enable_jit_link();

        const u8 jcc_code[] = {cond.opcode_byte, 0x02};
        auto jcc = decode_block64_at(*m_memory, jcc_addr, jcc_code,
                                     sizeof(jcc_code));
        ASSERT_EQ(jcc.instruction_count, 1u) << cond.name;
        auto* jcc_compiled = m_jit->compile(jcc);
        ASSERT_NE(jcc_compiled, nullptr) << cond.name;
        cache.insert(std::move(jcc));
        auto* cached_jcc = cache.lookup(jcc_addr);
        ASSERT_NE(cached_jcc, nullptr) << cond.name;
        publish_compiled(*cached_jcc, *jcc_compiled);

        const u8 cmp_code[] = {0x48, 0x39, 0xD8};  // cmp rax, rbx
        auto cmp = decode_block64_at(*m_memory, head_addr, cmp_code,
                                     sizeof(cmp_code));
        auto* cmp_compiled = m_jit->compile(cmp);
        ASSERT_NE(cmp_compiled, nullptr) << cond.name;

        for (auto [rax, rbx] : pairs) {
            CpuState reference;
            reference.init_long_mode();
            reference.gpr.rsp = 0x10000;
            reference.gpr.rax = rax;
            reference.gpr.rbx = rbx;
            reference.rip = head_addr;
            Interpreter interpreter(reference, *m_memory);
            (void)interpreter.run(2);

            m_cpu->init_long_mode();
            m_cpu->gpr.rsp = 0x10000;
            m_cpu->gpr.rax = rax;
            m_cpu->gpr.rbx = rbx;
            m_cpu->rip = head_addr;
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *cmp_compiled, *m_cpu),
                      JitExecResult::Continue);
            EXPECT_EQ(m_cpu->rip, reference.rip)
                << cond.name << " rax=0x" << std::hex << rax
                << " rbx=0x" << rbx;
        }

        m_jit->set_block_cache(nullptr);
        JitCompiler::disable_jit_link();
        head_addr += 0x40;
    }
}

TEST_F(JitExecTest, Mode64JccFirstAfterClcBranchesOnOverlayCarry) {
    // ADD; CLC; JB. CLC installs a CF overlay over the still-live ADD lazy
    // FlagOp (Intel SDM Vol. 2 CLC clears CF only). The Jcc-first successor
    // must branch on the CLC's cleared carry, not on the ADD's carry-out —
    // so the predecessor must publish nzcv_kind 0 and the successor must take
    // its lazy arm. STC is the mirror case: CF forced set, JB always taken.
    struct LinkGuard {
        ~LinkGuard() { JitCompiler::disable_jit_link(); }
    } link_guard;

    // {tail byte, name}: 0xF8 = CLC, 0xF9 = STC, 0xF5 = CMC.
    const struct {
        u8 byte;
        const char* name;
    } tails[] = {{0xF8, "clc"}, {0xF9, "stc"}, {0xF5, "cmc"}};

    // Operand pairs chosen so the ADD both does and does not carry out.
    const std::pair<u64, u64> pairs[] = {
        {0xFFFFFFFFFFFFFFFFull, 1},   // carry out
        {1, 1},                       // no carry out
        {0x8000000000000000ull, 0x8000000000000000ull},  // carry out, zero
        {0, 0},
    };

    GuestAddr clc_head = 0x2000;
    for (const auto& tail : tails) {
        const GuestAddr clc_jcc = clc_head + 4;
        BlockCache cache;
        m_jit->set_block_cache(&cache);
        JitCompiler::enable_jit_link();

        const u8 jcc_code[] = {0x72, 0x02};  // jb +2
        auto jcc = decode_block64_at(*m_memory, clc_jcc, jcc_code,
                                     sizeof(jcc_code));
        auto* jcc_compiled = m_jit->compile(jcc);
        ASSERT_NE(jcc_compiled, nullptr) << tail.name;
        cache.insert(std::move(jcc));
        auto* cached_jcc = cache.lookup(clc_jcc);
        ASSERT_NE(cached_jcc, nullptr) << tail.name;
        publish_compiled(*cached_jcc, *jcc_compiled);

        const u8 head_code[] = {0x48, 0x01, 0xD8, tail.byte};  // add rax,rbx
        auto head = decode_block64_at(*m_memory, clc_head, head_code,
                                      sizeof(head_code));
        ASSERT_EQ(head.instruction_count, 2u) << tail.name;
        auto* head_compiled = m_jit->compile(head);
        ASSERT_NE(head_compiled, nullptr) << tail.name;

        for (auto [rax, rbx] : pairs) {
            CpuState reference;
            reference.init_long_mode();
            reference.gpr.rsp = 0x10000;
            reference.gpr.rax = rax;
            reference.gpr.rbx = rbx;
            reference.rip = clc_head;
            Interpreter interpreter(reference, *m_memory);
            (void)interpreter.run(3);

            m_cpu->init_long_mode();
            m_cpu->gpr.rsp = 0x10000;
            m_cpu->gpr.rax = rax;
            m_cpu->gpr.rbx = rbx;
            m_cpu->rip = clc_head;
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *head_compiled, *m_cpu),
                      JitExecResult::Continue);
            EXPECT_EQ(m_cpu->rip, reference.rip)
                << tail.name << " rax=0x" << std::hex << rax
                << " rbx=0x" << rbx;
            EXPECT_EQ(m_cpu->gpr.rax, reference.gpr.rax) << tail.name;
        }

        m_jit->set_block_cache(nullptr);
        JitCompiler::disable_jit_link();
        clc_head += 0x40;
    }
}
