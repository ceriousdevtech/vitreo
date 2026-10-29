// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compiler_gpr_pins.hpp
/// @brief Guest GPR pin layout, in-place pinned access, constant-EA reuse, fused NZCV condition mapping, code buffer.
///
/// NOT a standalone header: a slice of class JitCompiler's body, textually
/// included by jit_compiler.hpp inside the class definition (access on
/// entry: private). Include <core/cpu/jit/jit_compiler.hpp> instead.
#ifndef VITREO_JIT_COMPILER_CLASS_BODY
#error "jit_compiler_gpr_pins.hpp is part of JitCompiler's body; include <core/cpu/jit/jit_compiler.hpp>"
#endif


    // Within-block GPR pins: the fixed low-eight layout for both bitnesses.
    //   X23-X28 = EAX/ECX/EDX/EBX/ESI/EDI
    //   X12/X13 = ESP/EBP (X8/X9 are guest-address scratches)
    // Loads read the pin; stores write the pin only. CpuState is flushed
    // at helper BLR and epilogue. Linear faults copy pins from ucontext.
    // Opt out with VITREO_JIT_NO_PIN=1.
    bool m_pins_live{false};
    u8 m_pin_arm[16]{};
    u64 m_pin_layout{0};

    struct SelectedPinLayout {
        std::array<u8, 16> arm{};
        bool pins_live{false};
        bool linear_base_pinned{false};
        u64 id{0};
    };
    [[nodiscard]] SelectedPinLayout select_pin_layout(
        const BasicBlock& block) const noexcept;

    /// The ARM register that IS guest 32-bit register @p x86_reg, or 0xFF when
    /// the guest register lives in CpuState instead.
    ///
    /// Lets an ALU op update the guest register in place. The generic sequence
    /// copies the pin into a scratch, operates, and copies back, so
    /// `add eax, 3` costs MOV/MOVZ/ADDS/MOV where ARM64 needs one ADDS. A
    /// W-form write zero-extends bits 63:32, which is exactly what x86
    /// requires of a 32-bit register write, so operating on the pin directly
    /// carries the same truncation semantics as the copy-back it replaces.
    [[nodiscard]] u8 pinned_gpr32(u8 x86_reg) const noexcept {
        if (!m_pins_live || x86_reg >= 16) return 0xFF;
        return m_pin_arm[x86_reg];
    }

    /// 64-bit variant. Only a 64-bit block's pin carries the full value (a
    /// 32-bit block pins bits 31:0 only), mirroring emit_load_gpr64.
    [[nodiscard]] u8 pinned_gpr64(u8 x86_reg) const noexcept {
        if (!m_block_is_64bit || !m_pins_live || x86_reg >= 16) return 0xFF;
        return m_pin_arm[x86_reg];
    }

    /// MOVZX / MOVSX / MOVSXD in a 64-bit block (Intel SDM Vol. 2): read or
    /// load the `src_bits` source straight into the destination's pin when it
    /// has one, then sign-extend in place if `sign`. Byte/halfword reads and
    /// loads already zero-extend, so the zero-extending forms need nothing
    /// more; `dst64` picks the 64-bit sign extension.
    void emit_movx64(const DecodedInstruction& instr, u64 next_rip,
                     u8 src_bits, bool sign, bool dst64);

    /// r64 <- r64 when either side is pinned: one MOV, LDR or STR, instead of
    /// staging the value through X0. False when neither is pinned.
    bool emit_mov_gpr64_direct(u8 dst_x86, u8 src_x86) {
        const u8 dst_pin = pinned_gpr64(dst_x86);
        if (dst_pin != 0xFF) {
            emit_load_gpr64(dst_pin, src_x86);
            if (dst_x86 == 4) m_esp_frame_live = false;  // RSP
            return true;
        }
        const u8 src_pin = pinned_gpr64(src_x86);
        if (src_pin != 0xFF) {
            emit_store_gpr64(src_pin, dst_x86);
            if (dst_x86 == 4) m_esp_frame_live = false;  // RSP
            return true;
        }
        return false;
    }

    /// The hardware-protected linear64 store (emit_linear64_store_checked and
    /// the hot-helper form) reads its value operand in place and moves it out
    /// of the way itself when a scratch it uses would alias it, so a store
    /// whose source is a pinned GPR can hand the pin over directly.
    [[nodiscard]] bool linear64_store_reads_value_in_place() const noexcept {
        return m_block_is_64bit && m_cfg.linear64 && m_linear_base_pinned &&
               m_cfg.linear64_hw_protect;
    }
    // X14 = GuestMemory linear_base for linear 32-bit blocks and opt-in
    // linear64 blocks.
    bool m_linear_base_pinned{false};
    // X15 = linear_base + zext(ESP). Valid only while ESP is unchanged.
    bool m_esp_frame_live{false};
    // A compile-time-constant effective address (RIP-relative, moffs or
    // displacement-only, no segment base) that emit_compute_ea64 has just
    // left in `reg`. Valid only while m_code_buf.size() == pos, i.e. nothing
    // has been emitted since -- so `reg` provably still holds `value` when a
    // linear64 emitter consumes it and may decide the reservation and
    // host-page bounds at compile time instead of testing them per access.
    struct ConstEa {
        bool valid = false;
        u8 reg = 0xFF;
        u64 value = 0;
        usize pos = 0;
    };
    ConstEa m_const_ea;
    [[nodiscard]] bool take_const_ea(u8 reg, u64& value) noexcept {
        const bool hit = m_const_ea.valid && m_const_ea.reg == reg &&
                         m_const_ea.pos == m_code_buf.size();
        m_const_ea.valid = false;
        if (hit) value = m_const_ea.value;
        return hit;
    }
    // g_mem_helper_generation as of this context's last sync_process_wide_stubs().
    u64 m_mem_helper_generation{0};

    /// Tag for a consumer that re-checks the runtime `nzcv_kind` byte before
    /// it branches. Required at the call site, and named there, because the
    /// obligation is invisible in the arguments.
    struct KindGuarded {};

    /// Map an x86 condition code (0x0-0xF) to the ARM64 condition to branch
    /// from the NZCV left by a fusable compare, or -1 if not fusable (parity
    /// has no NZCV equivalent; TEST/AND's unsigned conditions don't map
    /// because ANDS clears C). See the mapping derivation in jit_compiler.cpp.
    ///
    /// This overload is for a consumer that branches on PSTATE with NO runtime
    /// guard, and it refuses `NzcvState::Carried` for that reason: Carried
    /// means "NZCV holds the x86 flags *only when* the runtime nzcv_kind byte
    /// is nonzero", and the Jcc that publishes it has a second route (lazy
    /// evaluation, possibly a cond-helper BLR) that leaves NZCV holding
    /// something else entirely.
    ///
    /// This is deliberately the SHORT name and the safe one. It used to be the
    /// other way round -- the unguarded mapping was `fused_arm_cond` and the
    /// filtered version was `static_fused_arm_cond` -- so a lowering that
    /// reached for the obvious name got the dangerous behavior. One did, and
    /// the result was a bare `B.cond` reading NZCV a helper had destroyed.
    [[nodiscard]] int fused_arm_cond(NzcvState state, u8 x86_cc) const noexcept {
        if (state == NzcvState::Carried) return -1;
        return fused_arm_cond(state, x86_cc, KindGuarded{});
    }

    /// The raw mapping, for a consumer that re-checks the kind byte itself.
    /// Callers pass a literal writer kind (`NzcvState::Sub`, `::Add`, ...)
    /// describing the arm they are about to emit behind that check.
    [[nodiscard]] int fused_arm_cond(NzcvState state, u8 x86_cc,
                                     KindGuarded) const noexcept;




    /// Recompile counts per guest block address, and the set of addresses that
    /// have churned enough to be permanently routed to the interpreter.
    ///
    /// A block whose bytes the guest keeps rewriting (self-modifying dispatch
    /// stubs, or a heap region cycling between code and data) is invalidated
    /// and recompiled every time. A 64-bit title's startup hit this hard — one block
    /// at 0x00190a28 was recompiled 85,872 times in a single run, and the
    /// compile cost dominated startup. Past the threshold the JIT is a net
    /// loss, so interpret instead. This is always semantically safe: the
    /// interpreter is the reference implementation.
    std::unordered_map<GuestAddr, u32> m_recompile_counts;
    std::unordered_set<GuestAddr> m_churn_interpret;
    void note_invalidated_blocks(const std::vector<GuestAddr>& removed);



    // Code buffer for current compilation (reused across compilations)
    std::vector<u32> m_code_buf;



