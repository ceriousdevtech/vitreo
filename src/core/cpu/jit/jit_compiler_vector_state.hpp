// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compiler_vector_state.hpp
/// @brief Vector emission state: XMM register cache, V0/D0 forwarding, VEX.256 lane split, compiled-block storage.
///
/// NOT a standalone header: a slice of class JitCompiler's body, textually
/// included by jit_compiler.hpp inside the class definition (access on
/// entry: private). Include <core/cpu/jit/jit_compiler.hpp> instead.
#ifndef VITREO_JIT_COMPILER_CLASS_BODY
#error "jit_compiler_vector_state.hpp is part of JitCompiler's body; include <core/cpu/jit/jit_compiler.hpp>"
#endif

    // Write-through forwarding for V0 across one adjacent, whitelisted
    // register-register SSE lowering. Most producers are PE32-only; repeated
    // legacy x64 ADDPS also publishes because it is a measured dependent hot
    // shape. The producer still stores its result to CpuState; this only
    // records that V0 holds the same architectural value so the next lowering
    // can omit one reload. compile_instruction() advances and invalidates the
    // pair at every x86 instruction boundary.
    u8 m_xmm_v0_forward_in{0xFF};
    u8 m_xmm_v0_forward_out{0xFF};

    // ---- XMM register cache (block-local, write-through) ----------------
    // Bit n set: V(kXmmCacheBase + n) holds XMMn's architectural low 128 bits.
    // CpuState is ALWAYS current -- every lowering that writes an XMM still
    // stores it -- so the cache is only a copy, and clearing it is always
    // correct. It is cleared at block entry, at every in-block join, after an
    // instruction whose hot path called a C++ helper (V16-V31 are AAPCS64
    // caller-saved), and after any instruction that touches vector state
    // without maintaining it (xmm_cache_note_instruction). A cold path that
    // calls a helper and rejoins reloads the entries that were live at its
    // site. XMM15 is not cached: V31 is the lazy-flag scratch (kFlagPairVec).
    // VITREO_JIT_XMM_CACHE=0 disables it.
    static constexpr u8 kXmmCacheBase = 16;
    static constexpr u8 kXmmCacheCount = 15;
    u16 m_xmm_cache{0};
    bool m_vregs_clobbered{false};
    bool m_xmm_cache_aware{false};
    [[nodiscard]] bool xmm_cache_on() const noexcept;
    /// The V register holding @p xmm, loading it into its cache register
    /// first when it is not cached. Only when xmm_cache_on() and
    /// xmm < kXmmCacheCount.
    u8 xmm_cache_source(u8 xmm);
    /// V(kXmmCacheBase + xmm) now holds @p xmm's value (already stored to
    /// CpuState by the caller).
    /// Does NOT mark the instruction cache-aware: a helper used inside a
    /// larger lowering may be followed by CpuState-only writes, so only a
    /// complete cache-aware lowering sets m_xmm_cache_aware itself.
    void xmm_cache_publish(u8 xmm) noexcept {
        m_xmm_cache = static_cast<u16>(m_xmm_cache | (1u << xmm));
    }
    void emit_xmm_cache_reload(u16 mask);
    void emit_probe_poison_vregs();
    /// After one instruction: apply the invalidation rule above.
    void xmm_cache_note_instruction(const DecodedInstruction& instr) noexcept;
    void xmm_cache_drop_written(const DecodedInstruction& instr) noexcept;
public:
    /// The XMMs (bit n = XMMn) an instruction may write when its lowering is
    /// not cache-aware; 0xFFFF when that cannot be read off its operands.
    [[nodiscard]] static u16 xmm_cache_written_mask(
        const DecodedInstruction& instr) noexcept;
private:
    /// Rewrite a three-register NEON word that reads V0/V1 and writes V0
    /// (every EMIT_SSE_NEON_RR / _FINITE_FP instantiation) to read @p v0 /
    /// @p v1 in their places and write @p d.
    [[nodiscard]] static u32 xmm_rebind_v01(u32 enc, u8 d, u8 v0, u8 v1) noexcept;
    /// Cache-aware register form of an EMIT_SSE_NEON_RR-shaped lowering:
    /// XMM @p d := enc(XMM @p s1, XMM @p s2), written through to CpuState.
    /// False (nothing emitted) when the cache is off or a register is
    /// outside it; the caller then runs its own lowering.
    [[nodiscard]] bool xmm_cache_rr(u32 enc, u8 d, u8 s1, u8 s2);
    /// Memory form: the r/m operand has already been loaded into V1.
    /// XMM @p d := enc(XMM @p s1, V1). Requires xmm_cache_fits(d, s1).
    void xmm_cache_rm(u32 enc, u8 d, u8 s1);
    /// XMM @p d := XMM @p s, two words either way: a cached source is copied
    /// register to register, an uncached one is loaded straight into d's
    /// cache register. False (nothing emitted) when the cache cannot take it.
    [[nodiscard]] bool emit_xmm_cache_copy(u8 d, u8 s);
    /// MOVSS/MOVSD register form: XMM @p d lane 0 (32 bits, or 64 with
    /// @p qword) := XMM @p s lane 0, the rest of @p d unchanged. When @p s is
    /// cached that is one STR S/D from its cache register; @p d's entry is
    /// dropped (an INS to keep it costs the word a later reload would).
    /// False (nothing emitted) unless @p s is cached.
    [[nodiscard]] bool emit_xmm_cache_store_lane0(u8 d, u8 s, bool qword);
    /// Cache-aware whole-register binary op, both operand shapes:
    /// XMM @p d := enc(XMM @p s1, @p rm). A register @p rm goes through
    /// xmm_cache_rr (with @p xor_zero_idiom, `rm == s1` is a plain zero); a
    /// memory one is one checked 16-byte load into V1, aligned unless VEX.
    /// False (nothing emitted) when the cache cannot take it.
    [[nodiscard]] bool emit_xmm_cache_binop(const DecodedInstruction& instr,
                                            const Operand& rm, u64 next_rip,
                                            u32 enc, u8 d, u8 s1,
                                            bool xor_zero_idiom);
    [[nodiscard]] bool xmm_cache_fits(u8 a, u8 b = 0, u8 c = 0) const noexcept {
        return xmm_cache_on() && a < kXmmCacheCount && b < kXmmCacheCount &&
               c < kXmmCacheCount;
    }
    void push_cold_mem_stub(ColdMemStub stub);
    // A native register copy leaves V0 equal to both its written destination
    // and unchanged source. This value-equivalence expires after one x86
    // instruction; it is not a persistent architectural alias.
    u8 m_xmm_v0_alias_in{0xFF};
    u8 m_xmm_v0_alias_out{0xFF};
    // One-instruction x87 write-through forwarding.  A whitelisted producer
    // still commits the complete architectural ST(0) image; D0 merely mirrors
    // the same binary64 value for the adjacent consumer.  W6 is the emitted
    // runtime-valid bit because a compiled native stencil can still divert to
    // the helper for PC modes, stack faults, and special values.
    bool m_x87_d0_forward_in{false};
    bool m_x87_d0_forward_out{false};
    bool m_current_original_vex{false};

    // ---- VEX.256 lane-split emission state ----
    // A 256-bit VEX element-wise form is lowered by running the existing
    // 128-bit lowering TWICE: once over XMM[dst/srcs] and once over
    // AvxState::ymm_high[...]. m_vec_lane_split suppresses the generic VEX
    // bridge at the top of compile_sse for the re-entrant calls (the bridge's
    // merge copy and high-lane zero must NOT run per lane); m_vec_hi_lane
    // selects which 128-bit half vec_off() names.
    bool m_vec_lane_split{false};
    bool m_vec_hi_lane{false};

    /// CpuState byte offset of the 128-bit half of YMM[reg] currently being
    /// emitted. Identical to xmm_u64_off(reg, 0) outside a lane split.
    /// (Defined in ops/sse.cpp — the offset helpers live in jit_helpers.hpp,
    /// which includes this header.)
    [[nodiscard]] u32 vec_off(u8 reg) const noexcept;

    /// Drives the two-pass emission for an allowlisted VEX.256 form.
    bool compile_vex256_lane_split(const DecodedInstruction& instr,
                                   u64 instr_rip, u64 next_rip, bool is_last);

    // Generated code with a pre-decoded delegate embeds a pointer owned by
    // one BasicBlock and therefore cannot be shared through the process memo.
    bool m_has_predecoded_delegate{false};

    // Storage for compiled blocks.
    //
    // A deque, NOT a vector: both `compile()` and the memo-reuse path do
    // `push_back(...); return &m_compiled_blocks.back();`, so a vector's
    // reallocation would dangle every CompiledBlock* handed out earlier.
    // Nothing here needs contiguity, and deque guarantees references stay
    // valid across push_back. (The `erase_if` in the arena reclaim path still
    // invalidates, but that runs under m_compile_mutex at a drained safe
    // point where no executor holds a record.) Harmless while this is
    // thread-local and each pointer is consumed immediately -- but it is a
    // landmine directly in the path of sharing records across threads.
    std::deque<CompiledBlock> m_compiled_blocks;

