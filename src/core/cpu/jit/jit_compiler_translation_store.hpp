// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compiler_translation_store.hpp
/// @brief Per-compiler state of the persistent translation store
///        (translation_store.hpp): host-address relocations recorded while
///        emitting, and the capture/restore of everything finalize_code reads.
///
/// NOT a standalone header: a slice of class JitCompiler's body, textually
/// included by jit_compiler.hpp inside the class definition (access on
/// entry: private). Include <core/cpu/jit/jit_compiler.hpp> instead.
#ifndef VITREO_JIT_COMPILER_CLASS_BODY
#error "jit_compiler_translation_store.hpp is part of JitCompiler's body; include <core/cpu/jit/jit_compiler.hpp>"
#endif

private:
    /// emit_mov_imm64 for an absolute HOST address (a helper function the
    /// block calls). Emits exactly the words emit_mov_imm64 would, and records
    /// where they are so a stored copy can be relocated into a process whose
    /// image slide differs.
    void emit_host_addr_imm64(u8 arm_reg, i64 value);
    /// The block bakes a host value no relocation describes (a heap pointer,
    /// a per-process helper page, a diagnostic): never store it.
    void note_unrelocatable_host_bake() noexcept { m_ts_unrelocatable = true; }

    /// Host-address MOV runs of the block being emitted, in m_code_buf words.
    std::vector<StoredHostReloc> m_ts_relocs;
    bool m_ts_unrelocatable{false};
    /// > 0 while m_code_buf holds code emitted outside the block (shared
    /// stubs, late lazy-cold stubs): no relocation is recorded for it.
    u32 m_ts_foreign_emission{0};
    /// Process-wide stubs the block being emitted asked for (stored_stub
    /// bits, StoredMemHelper): its words reach them through JitContext.
    u32 m_ts_stubs{0};
    std::vector<StoredMemHelper> m_ts_mem_helpers;
    void ts_note_stub(u32 bit) noexcept { m_ts_stubs |= bit; }
    void ts_note_mem_helper(u32 helper_off, int width, bool is_store);
    /// Publish every stub @p in names into this process and this context,
    /// as the emitter would have. False when one cannot be.
    [[nodiscard]] bool ts_publish_stubs(const StoredEmission& in);
    /// The published linear64 cold-memory helper the emitter would select
    /// for (helper_off, width, is_store) under this compiler's state, now in
    /// this context's slot; nullptr when the process has none yet.
    [[nodiscard]] void* adopt_scalar_mem_helper(u32 helper_off, int width,
                                                bool is_store);

    /// The record a lookup returned, and the fresh emission captured for
    /// storing or for shadow verification. Reused across compiles.
    StoredEmission m_ts_record;
    StoredEmission m_ts_fresh;
    /// Hash of m_cfg, computed once (part of every stored key).
    u64 m_ts_cfg_hash{0};
    /// How the last compile() was served, for the measurement counters.
    TranslationOutcome m_ts_outcome{TranslationOutcome::None};

    void capture_emission(const BasicBlock& block, StoredEmission& out) const;
    /// Install @p in as if the emitter had just produced it. False when the
    /// record does not fit this compiler (sizes, pin layout).
    [[nodiscard]] bool restore_emission(const BasicBlock& block,
                                        const StoredEmission& in);
