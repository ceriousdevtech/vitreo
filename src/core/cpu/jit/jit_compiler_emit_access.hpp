// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compiler_emit_access.hpp
/// @brief Operand access emitters: guest GPR, flag-record publication, guest memory loads and stores, TSO ordering.
///
/// NOT a standalone header: a slice of class JitCompiler's body, textually
/// included by jit_compiler.hpp inside the class definition (access on
/// entry: private). Include <core/cpu/jit/jit_compiler.hpp> instead.
#ifndef VITREO_JIT_COMPILER_CLASS_BODY
#error "jit_compiler_emit_access.hpp is part of JitCompiler's body; include <core/cpu/jit/jit_compiler.hpp>"
#endif

    /// Cold body for a 128-bit access whose region probe missed: two 8-byte
    /// helper calls staged through SseState::jit_vec_scratch, because nothing
    /// but memory survives a helper call here.
    void emit_cold_mem_stub_vec128(const ColdMemStub& s);

    void emit_load_linear_base();
    /// Load X15 for an x64 linear block: the check map, or under
    /// JitConfig::linear64_jit_view the JIT view's base.
    void emit_load_linear64_check_map();
    /// The register holding the check map's base for one probe: X15 when it
    /// is the map; under the JIT view, @p scratch after loading the map from
    /// JitContext into it (one LDR). @p scratch must be dead, and may be the
    /// probe's LDRB destination when that differs from its index register.
    [[nodiscard]] u8 emit_linear64_check_map_base(u8 scratch);
    /// JIT-view lean sites (ColdMemStub::lean_variant).
    [[nodiscard]] bool linear64_lean_site_eligible() const noexcept;
    [[nodiscard]] bool emit_linear64_lean_head(ColdMemStub& stub, u8 ea,
                                               bool have_const, u64 const_ea,
                                               int width);
    void emit_linear64_lean_vector_guard(ColdMemStub& stub, u8 ea, int width,
                                         bool aligned);
    void emit_linear_load32(u8 dst_arm_reg, u8 addr_arm_reg);
    void emit_linear_load16(u8 dst_arm_reg, u8 addr_arm_reg);
    void emit_linear_load8(u8 dst_arm_reg, u8 addr_arm_reg);

    /// Inline FpuState::pop(): TAG(TOP) ← Empty, TOP ← (TOP+1) & 7.
    /// Replaces jit_fpu_pop BLR on FSTP / FSUBp / FUCOMIP.
    void emit_fpu_pop();

    void emit_load_gpr32_mem(u8 arm_reg, u8 x86_reg);
    void emit_store_gpr32_mem(u8 arm_reg, u8 x86_reg);
    void emit_load_pins();
    void emit_store_pins();

    /// Emit: load 8-bit value from CpuState GPR into ARM64 Wt
    /// Load 8-bit guest register into arm_reg.
    /// x86_reg: 0..15 raw rm-field encoding.
    /// has_rex: true if instruction had any REX prefix (including bare 0x40).
    ///   When has_rex is false and x86_reg is 4..7, decode as legacy AH/CH/DH/BH.
    ///   When has_rex is true, x86_reg 4..7 means SPL/BPL/SIL/DIL (low byte).
    void emit_load_gpr8(u8 arm_reg, u8 x86_reg, bool has_rex = false);

    /// Emit: store 8-bit value from ARM64 Wt into CpuState GPR (low byte)
    /// Store low 8 bits of arm_reg into 8-bit guest register.
    /// See emit_load_gpr8 for has_rex semantics.
    void emit_store_gpr8(u8 arm_reg, u8 x86_reg, bool has_rex = false);

    /// Emit: load 16-bit value from CpuState GPR into ARM64 Wt (zero-extended)
    void emit_load_gpr16(u8 arm_reg, u8 x86_reg);

    /// Emit: store 16-bit value from ARM64 Wt into CpuState GPR (low halfword)
    void emit_store_gpr16(u8 arm_reg, u8 x86_reg);

    /// Emit: load 64-bit value from CpuState GPR into ARM64 Xt
    /// Load/store a register being used as a STRING-OP POINTER or counter
    /// (RSI/RDI/RCX). These are address-width, not operand-width: in a 64-bit
    /// block they are full 64-bit pointers, and reading one with
    /// emit_load_gpr32 truncates the address while writing it back with
    /// emit_store_gpr32 zero-extends away the top half. In a 32-bit block
    /// they behave exactly as the 32-bit helpers.
    void emit_load_gpr_ptr(u8 arm_reg, u8 x86_reg);
    void emit_store_gpr_ptr(u8 arm_reg, u8 x86_reg);
    /// SHLD/SHRD OF: defined only for count==1, set when the sign changed.
    void emit_double_shift_of(u8 orig_reg, u8 result_reg, bool wide);
    void emit_load_gpr64(u8 arm_reg, u8 x86_reg);
    /// Spill/reload a single pinned GPR at the width its pin actually holds.
    /// A lowering that hand-rolls a CpuState access for one register must
    /// bracket it with these, or it reads a stale slot (64-bit blocks, where
    /// the pin is authoritative) or leaves a stale pin to clobber its result.
    void emit_pin_spill_one(u8 x86_reg);
    void emit_pin_reload_one(u8 x86_reg);
    /// Bypass the pin and go straight to CpuState (pin fill/spill uses these).
    void emit_load_gpr64_mem(u8 arm_reg, u8 x86_reg);
    void emit_store_gpr64_mem(u8 arm_reg, u8 x86_reg);

    /// Emit: store 64-bit value from ARM64 Xt into CpuState GPR
    void emit_store_gpr64(u8 arm_reg, u8 x86_reg);

    /// Emit: load 32-bit MOV immediate into ARM64 Wt
    void emit_mov_imm32(u8 arm_reg, u32 value);

    /// Emit Xd = Xn + `value` for any value below 2^24, without a scratch:
    /// ADD (immediate) holds imm12 at LSL #0 or #12, so this is one ADD below
    /// 4096 and at most two above. Flags untouched. For a value that is not a
    /// constant below 4096 -- a guest immediate such as RET imm16's release
    /// count, or a displacement -- where arm64::add_x_imm would refuse it.
    /// The words come from arm64::add_x_uimm24, which is tested directly.
    void emit_add_x_uimm24(u8 d, u8 n, u64 value);

    /// STRB WZR over `CpuState::cf_overlay`. Full-flag writers call this so a
    /// prior CLC/STC/CMC cannot keep answering CF after a new FlagOp is stored.
    void emit_clear_cf_overlay();

    /// Emit: set lazy flag state in CpuState
    void emit_set_lazy_flags(CpuState::FlagOp op, u8 src_arm_reg,
                             u8 dst_arm_reg);

    /// Emit: set lazy flag state for INC/DEC, packing preserved CF above the
    /// 32-bit original operand in flag_src.
    void emit_set_lazy_flags_preserve_cf(CpuState::FlagOp op,
                                         u8 src_arm_reg,
                                         u8 dst_arm_reg,
                                         u8 cf_arm_reg);

    /// Emit: set shift lazy flags — packs original, count, and result
    /// so that CF can be correctly reconstructed during materialization.
    /// Convention: flag_src = original, flag_dst = (count << 32) | result
    void emit_set_shift_lazy_flags(CpuState::FlagOp op,
                                    u8 original_arm_reg,
                                    u8 count_arm_reg,
                                    u8 result_arm_reg);

    /// Emit: set lazy flag state for 64-bit operations (no zero-extend)
    void emit_set_lazy_flags64(CpuState::FlagOp op, u8 src_arm_reg,
                               u8 dst_arm_reg);

    /// Intel SDM Vol. 2 SAL/SAR/SHL/SHR: CF = last bit shifted out.
    /// FlagOp::Shl64/Shr64/Sar64 only record ZSP — the count does not fit
    /// beside a 64-bit result — so write cf_overlay + rflags.CF from `orig`
    /// and the masked count. `left` is SHL (bit 64-count) vs SHR/SAR (bit
    /// count-1).
    void emit_shift64_cf(u8 orig_reg, u8 count_reg, bool left);
    void emit_shift64_cf_imm(u8 orig_reg, u8 count, bool left);
    /// After a nonzero-count 64-bit shift whose result register still holds
    /// the value, republish N/Z with TST and arm NzcvState::NzOnly for the
    /// next instruction — but only when that instruction is a JZ/JNZ/JS/JNS
    /// that can fuse against it (see m_next_opcode).
    void maybe_publish_result_nz64(u8 result_reg);

    /// Emit: load sign-extended 64-bit immediate into ARM64 Xt
    void emit_mov_imm64(u8 arm_reg, i64 value);

    /// Emit: materialize x86 flags from lazy state into flags.rflags
    void emit_materialize_flags();
    /// Leave x86 CF in REG_CALL_TMP, using live NZCV or a known carry source
    /// when possible and only falling back to the CAPTURE_CF helper call.
    /// Shared by the 32-bit and 64-bit ALU paths -- see the definition in
    /// ops/carry_flag.cpp for why it must not be duplicated.
    void emit_lazy_cf_to_call_tmp();

    /// Intel SDM Vol. 2 ROR/ROL: write only CF, plus OF when count==1.
    /// Leaves `flag_op` intact so a prior SUB/XOR still answers ZF/SF/PF.
    void emit_rotate64_partial_flags(u8 result_reg, bool cf_from_msb,
                                    bool define_of);

    /// Emit: load 32-bit from guest memory at address in Xt
    void emit_guest_load32(u8 dst_arm_reg, u8 addr_arm_reg);

    /// Emit: store 32-bit to guest memory at address in Xt
    void emit_guest_store32(u8 src_arm_reg, u8 addr_arm_reg);

    /// Emit: load 8-bit from guest memory (zero-extended to 32-bit)
    void emit_guest_load8(u8 dst_arm_reg, u8 addr_arm_reg);

    /// Emit: store 8-bit to guest memory (low byte of Wt)
    void emit_guest_store8(u8 src_arm_reg, u8 addr_arm_reg);

    /// Emit: load 16-bit from guest memory (zero-extended to 32-bit)
    void emit_guest_load16(u8 dst_arm_reg, u8 addr_arm_reg);

    /// Emit: store 16-bit to guest memory (low halfword of Wt)
    void emit_guest_store16(u8 src_arm_reg, u8 addr_arm_reg);

    /// Long-mode narrow loads use the persistent full-width region cache for
    /// both low and high guest addresses. Stores retain the validated legacy
    /// low-address fast path and checked full-width fallback.
    void emit_guest_load16_64(u8 dst_arm_reg, u8 addr_arm_reg);
    void emit_guest_store16_64(u8 src_arm_reg, u8 addr_arm_reg);
    void emit_guest_load32_64(u8 dst_arm_reg, u8 addr_arm_reg);
    void emit_guest_store32_64(u8 src_arm_reg, u8 addr_arm_reg);

    /// Emit a long-mode inline load of `width` bytes (1/4/8) through the two
    /// full-width x64 region slots, falling back to the width's read helper on
    /// a miss. The helper republishes the region, so a loop settles onto the
    /// inline path after its first access.
    void emit_x64_region_load(u8 dst_arm_reg, u8 addr_arm_reg, int width);
    /// One `LDRB` + one test of the host page's check byte. Emitted for every
    /// direct store (it is what replaced the per-store code-span compare) and
    /// for a direct load only when hardware protection is on. A store asks the
    /// whole byte; a load asks only `kLinearCheckMixedProt`, because a page
    /// merely holding translated code is safe to READ directly.
    void emit_linear64_check_page_test(u8 first_arm, u8 last_arm, int width,
                                       usize& check_page_pos);
    void emit_linear64_permission_guard(bool is_store, u8 last_arm,
                                        usize& protection_generation_pos,
                                        usize& permission_below_pos,
                                        usize& permission_above_pos);
    void emit_linear64_load(u8 dst_arm_reg, u8 addr_arm_reg, int width);
    /// The default (hardware-protected, unproven) linear64 scalar load. See
    /// the definition for why its combined reservation/crossing test is exact.
    void emit_linear64_load_checked(u8 dst_arm_reg, u8 addr_arm_reg, int width,
                                    u32 helper_off);
    /// The default (hardware-protected) linear64 scalar store.
    void emit_linear64_store_checked(u8 src_arm_reg, u8 addr_arm_reg,
                                     int width, u32 helper_off);
    /// 16-byte guest-base access for the x64 SSE/AVX lane paths. Without these
    /// every 128-bit memory operand in a 64-bit block calls the C SSE helper.
    void emit_linear64_first_byte_index(u8 ea, ColdMemStub& stub);
    void emit_linear64_vec128_checked(u8 vec_reg, bool is_store,
                                      u8 addr_arm_reg, bool aligned16);
    /// `aligned16`: the caller has just emitted a 16-byte alignment check of
    /// `addr_arm_reg` (a #GP on failure), so the access cannot cross a host
    /// page and the guard tests only the reservation.
    void emit_linear64_load128(u8 vec_reg, u8 addr_arm_reg,
                               bool aligned16 = false);
    void emit_linear64_store128(u8 vec_reg, u8 addr_arm_reg,
                                bool aligned16 = false);
    /// 32-byte guest-base access for the VEX.256 pure moves: one guard over
    /// [EA, EA+31], then LDP/STP Q into `vec_lo` (bytes 0..15) and `vec_hi`
    /// (bytes 16..31). A browser renderer spends its hottest loop in a memmove
    /// of sixteen such moves; the four-way 64-bit split cost 71 ARM words a
    /// load and 107 a store.
    /// `slot_lo`/`slot_hi` are the CpuState byte offsets of the value's two
    /// halves: the load's destination (the stub fills them piecewise on the
    /// cold path and reloads the V registers from them), the store's source
    /// (the stub reads its pieces from them).
    void emit_linear64_load256(u8 vec_lo, u8 vec_hi, u8 addr_arm_reg,
                               u32 slot_lo, u32 slot_hi);
    void emit_linear64_store256(u8 vec_lo, u8 vec_hi, u8 addr_arm_reg,
                                u32 slot_lo, u32 slot_hi);
    void emit_cold_mem_stub_vec256(const ColdMemStub& s);
    /// The 128-bit cold body for the vector register `vreg`, with X9 holding
    /// the guest address on entry. Shared by the 128- and 256-bit stubs.
    void emit_cold_mem_stub_vec128_half(const ColdMemStub& s, u8 vreg);
    void emit_linear64_store(u8 src_arm_reg, u8 addr_arm_reg, int width);

    /// Emit a long-mode inline store of `width` bytes (1/2/4/8) through the
    /// x64 store region, falling back to the width's write helper on a miss.
    /// The region is only published when writable and code-free, so the
    /// emitted store needs no permission test and owes no SMC notification.
    void emit_x64_region_store(u8 src_arm_reg, u8 addr_arm_reg, int width);

    /// 128-bit validated guest load/store: ONE region probe, then a single
    /// `LDR Q` / `STR Q`.
    ///
    /// Why this exists. A VEX.256 memory operand today costs four separate
    /// `emit_guest_load64` sequences, each running its own inline probe, so a
    /// 256-bit access pays the validation four times. Validating once and
    /// moving 16 bytes cuts that by roughly 4x. `emit_sse_load_mem128_fast`
    /// (ops/sse.cpp) cannot be used for this: in a 64-bit block it is
    /// unconditionally a helper call, and its 32-bit path narrows the address
    /// with `mov_w`, which is the recorded EA-truncation shape.
    ///
    /// Contract:
    ///   * 64-bit blocks only (it uses the full-width x64 region slots).
    ///   * `addr_arm_reg` is a FULL 64-bit effective address and is never
    ///     narrowed: the marshal is `MOV Xd,Xn` throughout.
    ///   * The bound check uses `addr + 15`, so a 16-byte access straddling
    ///     the end of a region takes the slow path rather than reading past it.
    ///   * `vec_reg` is an ARM V register index, not an x86 XMM index.
    ///   * No alignment requirement: x86 VEX memory operands are unaligned,
    ///     and `LDR Q`/`STR Q` are unaligned-capable.
    ///   * The slow path splits into two 8-byte helper calls staged through
    ///     SseState::jit_vec_scratch, and is emitted out of line in the block
    ///     tail like every other memory slow path.
    void emit_x64_region_load128(u8 vec_reg, u8 addr_arm_reg);
    void emit_x64_region_store128(u8 vec_reg, u8 addr_arm_reg);

    /// Observe a scalar store without changing its normal memory lowering.
    /// Returns false so callers always emit the actual store.
    bool emit_store_ring_helper(u8 src_arm_reg, u8 addr_arm_reg, u32 width);

    /// Record a 128-bit store whose halves are staged in X10/X11.
    void emit_store_ring_record_vec(u8 addr_arm_reg);

    /// Record a store whose value is already staged in X10.
    void emit_store_ring_record_preloaded(u8 addr_arm_reg, u32 width);

    /// Opt-in diagnostic recording for scalar and vector stores.
    bool m_store_ring_stores{false};

    void emit_tso_barrier();

    /// Barrier emitted AFTER a 32-bit guest store. Separate from
    /// emit_tso_barrier() because x86-TSO does not require it; see
    /// m_cfg.tso_store_tail_fence.
    void emit_tso_store_tail_barrier();

    /// True when guest accesses carry x86-TSO ordering in the access
    /// instruction itself (LDAPR/LDAR + STLR) rather than via a barrier.
    [[nodiscard]] bool tso_ordered() const noexcept;

    /// Materialize the full host address X<base> + zext32(W<addr>) into
    /// X<scratch> and return it. The ordered access forms have no offset
    /// addressing mode, so they need the address in one register.
    /// Safe when scratch == addr_w.
    u8 emit_linear_addr_x(u8 base_x, u8 addr_w, u8 scratch_x);

    /// Acquire-load `width` bytes from [X<addr_x>] into <dst>, zero-extended.
    /// Uses LDAPR where FEAT_LRCPC is advertised, else LDAR.
    void emit_acquire_load(u8 dst, u8 addr_x, int width);

    /// Release-store the low `width` bytes of <src> to [X<addr_x>].
    void emit_release_store(u8 src, u8 addr_x, int width);

    /// Long-mode scalar access after the guest mapping has already been
    /// validated. Same-granule accesses carry TSO in LDAPR/STLR; the rare
    /// 16-byte-granule crossing retains the plain access plus DMB fallback.
    void emit_x64_tso_load(u8 dst, u8 addr_x, u8 scratch_x, int width);
    void emit_x64_tso_store(u8 src, u8 addr_x, u8 scratch_x, int width);

    /// Long-mode variant of emit_linear_addr_x: X<scratch> = X<base> +
    /// X<addr>, no zero-extension. Safe when scratch == addr_x.
    u8 emit_addr_x_add(u8 base_x, u8 addr_x, u8 scratch_x);

    /// Inline access on a region fast path whose host base is in X<base_x>
    /// and whose byte offset within that region is in W<off_w>. Emits the
    /// ordered form when tso_ordered(), folding base+offset into X<off_w>
    /// (dead at every such site), else the plain register-offset form.
    void emit_region_inline_load(u8 dst, u8 base_x, u8 off_w, int width);
    void emit_region_inline_store(u8 src, u8 base_x, u8 off_w, int width);


    /// True when 128-bit guest accesses are ordered. Separate from
    /// tso_ordered() because vectors can only be ordered with a full barrier
    /// and are therefore the expensive half.
    [[nodiscard]] bool tso_vector() const noexcept;

    /// DMB ISH for 128-bit guest accesses, which have no ordered form.
    /// Acquire == access then fence; release == fence then access.
    void emit_vector_tso_fence();

    /// Emit: load 64-bit from guest memory at address in Xt
    /// 128-bit validated guest access, long mode only. Dispatches to the
    /// linear or region primitive exactly as the scalar widths do.
    void emit_guest_load128(u8 vec_reg, u8 addr_arm_reg);
    void emit_guest_store128(u8 vec_reg, u8 addr_arm_reg);
    void emit_guest_load64(u8 dst_arm_reg, u8 addr_arm_reg);

    /// Emit: store 64-bit to guest memory at address in Xt
    void emit_guest_store64(u8 src_arm_reg, u8 addr_arm_reg);

