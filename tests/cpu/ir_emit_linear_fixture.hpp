// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_emit_linear_fixture.hpp
/// @brief IrEmitLinear and IrEmitFaultRecovery fixtures over a linear guest reservation.

#pragma once

#include "ir_emit_test_support.hpp"

namespace vitreo::cpu::jit::test::ir_emit {

// =============================================================================
// The LINEAR path — production lowering
// =============================================================================
//
// Everything above runs on a plain `GuestMemory`, which has no reservation, so
// the accesses take the REGION-CACHE lowering: a bounds check plus a helper
// that raises in software. That is not what a real 32-bit guest uses.
//
// With `VITREO_JIT_LINEAR` (the default) and a registered reservation, a guest
// access becomes a single `LDR W, [X14, Wn, UXTW]` with NO bounds check at all,
// and a bad address is a real SIGSEGV recovered by the fault handler from the
// host-PC side table. Different instructions, different fault path, different
// RIP recovery — so it needs its own tests or the production path ships
// unexercised.
class IrEmitLinear : public ::testing::Test {
protected:
    // These tests assert on the IR's emission from a single compile; under
    // tiering a first compile is tier 0 unless the threshold is pinned.
    IrTier1FirstCompile m_ir_tier1_first_compile;

    static constexpr GuestAddr kLinCode = 0x1000;
    static constexpr GuestAddr kLinData = 0x40000;
    static constexpr usize kReservation = usize{4} * 1024 * 1024 * 1024;

    static void SetUpTestSuite() {
        setenv("VITREO_JIT_LINEAR", "1", 1);
        ASSERT_TRUE(install_linear_fault_handler());
    }

    void SetUp() override {
        if (!JitCompiler::ir_enabled_for(false)) {
            GTEST_SKIP() << "Tier 1 gate off; run vitreo_cpu_tests_ir_gate_on";
        }
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->enable_linear_reservation(kReservation));
        // Real host protections for guest pages, so the JIT's inline linear
        // store cannot bypass a read-only page. Must precede any mapping.
        ASSERT_TRUE(m_memory->enable_linear_hardware_protection());
        ASSERT_TRUE(register_linear_reservation(m_memory->linear_base(),
                                                kReservation));
        ASSERT_TRUE(m_memory->map_region(kLinCode, 0x4000,
                                         MemoryProtection::ReadWrite));
        ASSERT_TRUE(m_memory->map_region(kLinData, 0x1000,
                                         MemoryProtection::ReadWrite));
        ASSERT_TRUE(m_memory->is_linear());
        m_jit = std::make_unique<JitCompiler>(*m_memory);
    }

    void TearDown() override {
        m_jit.reset();
        if (m_memory) unregister_linear_reservation(m_memory->linear_base());
    }

    BasicBlock decode_at(const std::vector<u8>& code) {
        BasicBlock block;
        block.start_addr = kLinCode;
        block.is_64bit = false;
        MemoryByteReader reader(code.data(), code.size());
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            if (decode_instruction(reader, DecodeMode::Mode32, inst) !=
                DecodeError::Success) {
                break;
            }
            block.instructions.push_back(inst);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count =
            static_cast<u32>(block.instructions.size());
        return block;
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
};

/// One faulting-form case: the same bytes, the same seed, run on the
/// interpreter and on Tier 1 over the linear reservation, compared on every
/// guest-visible field.
struct FaultFormResult {
    u64 rip{0};
    u32 gpr[8]{};
    u64 rflags{0};
    bool raised{false};
    u32 code{0};
    u64 is_write{0};
    u64 address{0};
};

/// Normalize the flag representation before comparing. The interpreter leaves
/// authoritative RFLAGS; a JIT block leaves a lazy record, an NZCV claim, or
/// both. Materializing collapses every representation to the architectural
/// value, which is the only thing the guest can actually observe.
inline u64 observable_rflags(CpuState& cpu) {
    cpu.materialize_lazy_flags();
    return cpu.flags.rflags;
}

inline FaultFormResult capture(CpuState& cpu) {
    FaultFormResult r;
    r.rip = cpu.rip;
    for (u8 i = 0; i < 8; ++i) r.gpr[i] = cpu.gpr.get32(i);
    r.rflags = observable_rflags(cpu);
    r.raised = cpu.pending_raise.active;
    r.code = cpu.pending_raise.code;
    if (cpu.pending_raise.num_args >= 2) {
        r.is_write = cpu.pending_raise.args[0];
        r.address = cpu.pending_raise.args[1];
    }
    return r;
}

class IrEmitFaultRecovery : public IrEmitLinear {
protected:
    /// Seeds asymmetric, sign-crossing values on purpose. A symmetric seed
    /// makes SF/OF/CF agree by accident, and a flag-recovery bug that swaps
    /// the record's two operands is then invisible — the same trap that hid
    /// the `GprWrite`-clobbers-a-pin bug until an asymmetric operand was used.
    static void seed(CpuState& cpu, u32 edx) {
        cpu.init_protected_mode();
        cpu.gpr.set32(0, 0x8000'0001u);   // EAX
        cpu.gpr.set32(1, 0x7FFF'FFFFu);   // ECX
        cpu.gpr.set32(2, edx);            // EDX — the faulting pointer
        cpu.gpr.set32(3, 0x0000'00FFu);   // EBX
        cpu.gpr.set32(4, kLinData + 0x800);
        cpu.gpr.set32(5, kLinData + 0x400);
        cpu.gpr.set32(6, 0x1234'5678u);
        cpu.gpr.set32(7, 0x9ABC'DEF0u);
        cpu.rip = kLinCode;
    }

    /// The address is inside the reservation and unmapped, so the access is a
    /// real SIGSEGV that the linear handler owns — not a software raise.
    static constexpr u32 kUnmapped = 0x7F00'0000u;

    void expect_matches_interpreter(const std::vector<u8>& code,
                                    const char* what, bool require_ir = true) {
        ASSERT_TRUE(m_memory->write_bytes(kLinCode, code.data(), code.size()));

        // The interpreter reports a fault by returning something other than
        // Continue; `pending_raise` is the JIT's own channel and the
        // interpreter never sets it. So the oracle's "did it fault" comes from
        // the step result, and only RIP/GPRs/EFLAGS are compared against it —
        // the exception code and address are asserted on the JIT side.
        CpuState interp_cpu;
        seed(interp_cpu, kUnmapped);
        bool interp_faulted = false;
        {
            Interpreter interp(interp_cpu, *m_memory);
            for (int i = 0; i < 4; ++i) {
                if (interp.step() != ExecResult::Continue) {
                    interp_faulted = true;
                    break;
                }
            }
        }
        const FaultFormResult oracle = capture(interp_cpu);
        ASSERT_TRUE(interp_faulted)
            << what << ": the interpreter did not fault; the case is wrong";

        CpuState jit_cpu;
        seed(jit_cpu, kUnmapped);
        BasicBlock block = decode_at(code);
        ASSERT_TRUE(m_jit->ir_build_block(block).ok)
            << what << ": "
            << ir::decline_reason_name(m_jit->ir_last_result().reason);
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr) << what;
        if (require_ir) {
            ASSERT_TRUE(m_jit->ir_emit_active())
                << what << ": fell back to Tier 0; this would assert Tier 0";
        }
        m_jit->execute(*compiled, jit_cpu);
        const FaultFormResult got = capture(jit_cpu);

        EXPECT_TRUE(got.raised) << what << ": Tier 1 did not fault";
        EXPECT_EQ(got.code, 0xC000'0005u) << what << ": exception code";
        EXPECT_EQ(got.address, kUnmapped) << what << ": fault address";
        EXPECT_EQ(got.rip, oracle.rip) << what << ": faulting RIP";
        for (u8 i = 0; i < 8; ++i) {
            EXPECT_EQ(got.gpr[i], oracle.gpr[i])
                << what << ": GPR " << static_cast<int>(i);
        }
        // THE assertion this suite exists for.
        EXPECT_EQ(got.rflags, oracle.rflags)
            << what << ": EFLAGS across a hardware fault (got 0x" << std::hex
            << got.rflags << ", oracle 0x" << oracle.rflags << std::dec << ")";
    }
};

}  // namespace vitreo::cpu::jit::test::ir_emit
