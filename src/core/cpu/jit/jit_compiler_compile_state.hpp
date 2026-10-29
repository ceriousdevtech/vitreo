// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compiler_compile_state.hpp
/// @brief Per-compiler state: generations, branch-reach decline, emit attribution, arena cursor, lazy-flag and FP-rounding state.
///
/// NOT a standalone header: a slice of class JitCompiler's body, textually
/// included by jit_compiler.hpp inside the class definition (access on
/// entry: private). Include <core/cpu/jit/jit_compiler.hpp> instead.
#ifndef VITREO_JIT_COMPILER_CLASS_BODY
#error "jit_compiler_compile_state.hpp is part of JitCompiler's body; include <core/cpu/jit/jit_compiler.hpp>"
#endif

private:
    // Storage for jit_hot_threshold(): 0 means "not resolved yet". Header-
    // local so the x86_64 stand-in (jit_unavailable.cpp) needs no definition.
    static std::atomic<u64>& jit_hot_threshold_slot() noexcept {
        static std::atomic<u64> slot{0};
        return slot;
    }
    static u64 parse_jit_hot_threshold_env() noexcept {
        const char* e = std::getenv("VITREO_JIT_HOT_THRESHOLD");
        if (!e || !*e) e = std::getenv("VITREO_JIT_THRESHOLD");
        if (!e || !*e) return kJitHotThreshold;
        char* end = nullptr;
        const unsigned long long v = std::strtoull(e, &end, 10);
        if (end == e || *end != '\0' || v == 0) return kJitHotThreshold;
        return v > kJitHotThresholdMax ? kJitHotThresholdMax
                                       : static_cast<u64>(v);
    }
    /// Drain AppKit / posted messages while compiling on the main thread.
    /// Throttled to ~16 ms so compile throughput stays intact.
    void maybe_pump_gui();

    /// Warm JIT execution can return to the dispatcher hundreds of thousands
    /// of times per second. Poll the clock only periodically on that path.
    /// compile() uses the same 16 ms contract, but copies the source block
    /// only when a pump is actually about to run (re-entrant eviction).
    void maybe_pump_gui_after_execute();

    GuestMemory& m_memory;
    BlockCache* m_block_cache{nullptr};
    std::mutex m_compile_mutex;
    u64 m_blocks_compiled{0};
    u64 m_bytes_generated{0};
    GuiPump m_gui_pump;
    std::chrono::steady_clock::time_point m_last_gui_pump{};
    u32 m_execute_gui_poll_count{0};
    u32 m_compile_gui_poll_count{0};

    // Persistent JIT context — function pointers set once in constructor;
    // only the region cache is refreshed per execute() call.
    JitContext m_jit_ctx{};

    // Backing storage for the direct-mapped long-mode translation caches the
    // context points at. Retracted together with the other region caches
    // whenever the memory or code generation changes.
    std::array<X64MemSlot, X64_SLOT_COUNT> m_x64_rslots{};
    std::array<X64MemSlot, X64_SLOT_COUNT> m_x64_wslots{};

    // Tracked memory generation — when this changes (map/unmap/commit),
    // both region caches in m_jit_ctx must be invalidated. VirtualProtect
    // uses m_protect_seq ranged shootdown instead.
    u64 m_region_gen{0};
    u64 m_protect_seq{0};

    // Last process-wide executable-code generation handled by this cache.
    u64 m_executable_generation{0};
    // Set by compile() when its null return means "not now" rather than
    // "never". See compile_deferred().
    bool m_compile_deferred{false};
    // Guest code hash of the block compile_attempt() is emitting, handed to
    // JitCodeArena::note_fresh_translation() for retirement-thrash
    // accounting. Valid only while m_emit_has_hash is set.
    u64 m_emit_code_hash{0};
    bool m_emit_has_hash{false};
    // Set by cold_branch_disp() when one of this block's deferred branches
    // cannot reach its target: finalize_code() declines the block. The first
    // overflowing branch is kept for the log line.
    bool m_branch_reach_exceeded{false};
    const char* m_reach_what{""};
    i64 m_reach_words{0};
    unsigned m_reach_bits{0};
    // Set while shared code is materialized mid-compile (SharedEmissionScope):
    // a reach overflow there aborts instead of declining the in-flight block.
    bool m_branch_reach_fatal{false};
    // TEST-ONLY knobs (compile_attempt): VITREO_JIT_TEST_BRANCH_REACH_FATAL
    // and VITREO_JIT_TEST_BRANCH_REACH_WORDS.
    bool m_test_reach_fatal{false};
    i64 m_test_reach_words{0};

    /// Scope for code emitted OUTSIDE the block being compiled -- the shared
    /// memory helpers, the capture-CF stub, the process raise exit -- which
    /// swap m_code_buf. Such code is published process-wide, so it has no
    /// block to decline: a branch that cannot reach is fatal there, and the
    /// in-flight block's own decline state is saved and restored around it.
    struct SharedEmissionScope {
        explicit SharedEmissionScope(JitCompiler& c) noexcept
            : jit(c), saved_fatal(c.m_branch_reach_fatal),
              saved_exceeded(c.m_branch_reach_exceeded) {
            jit.m_branch_reach_fatal = true;
            jit.attr_credit();
            ++jit.m_attr_suspend;
            ++jit.m_ts_foreign_emission;
        }
        ~SharedEmissionScope() {
            jit.m_branch_reach_fatal = saved_fatal;
            jit.m_branch_reach_exceeded = saved_exceeded;
            --jit.m_attr_suspend;
            --jit.m_ts_foreign_emission;
            jit.m_attr_mark = jit.m_code_buf.size();
        }
        SharedEmissionScope(const SharedEmissionScope&) = delete;
        SharedEmissionScope& operator=(const SharedEmissionScope&) = delete;
        JitCompiler& jit;
        bool saved_fatal;
        bool saved_exceeded;
    };

    // EmitAttribution: words of m_code_buf tagged by category as emitted.
    bool m_attr_on{false};
    u32 m_attr_suspend{0};
    u8 m_attr_cat{EmitAttribution::Prologue};
    usize m_attr_mark{0};
    std::vector<u8> m_attr_tags;
    void attr_credit() noexcept {
        if (!m_attr_on || m_attr_suspend != 0) return;
        const usize now = m_code_buf.size();
        if (now < m_attr_mark) m_attr_mark = now;
        if (m_attr_tags.size() < now) m_attr_tags.resize(now, EmitAttribution::Other);
        for (usize i = m_attr_mark; i < now; ++i) m_attr_tags[i] = m_attr_cat;
        m_attr_mark = now;
    }
    void attr_set(u8 cat) noexcept {
        attr_credit();
        m_attr_cat = cat;
    }
    static constexpr bool attr_exit(u8 cat) noexcept {
        return cat >= EmitAttribution::ExitLink && cat <= EmitAttribution::ExitOther;
    }
    static constexpr bool attr_region(u8 cat) noexcept {
        return cat == EmitAttribution::Prologue || attr_exit(cat) ||
               cat == EmitAttribution::Raise || cat == EmitAttribution::ColdMem ||
               cat == EmitAttribution::ColdArms;
    }
    struct AttrScope {
        AttrScope(JitCompiler& c, u8 cat) noexcept : jit(c), saved(c.m_attr_cat) {
            if (jit.m_attr_on &&
                (!attr_region(saved) || (attr_exit(saved) && attr_exit(cat)))) {
                jit.attr_set(cat);
            }
        }
        ~AttrScope() {
            if (jit.m_attr_on) jit.attr_set(saved);
        }
        AttrScope(const AttrScope&) = delete;
        AttrScope& operator=(const AttrScope&) = delete;
        JitCompiler& jit;
        u8 saved;
    };
    void attr_commit() noexcept;
    // Last retirement round this cache purged against (JitCodeArena::s_arena_retire_seq).
    // Taking a chunk invalidates nothing; only a retirement does, and the
    // purge keeps exactly the translations JitCodeArena::arena_code_live() still admits.
    u64 m_arena_retire_seq{0};
    // This compiler's private bump chunk. Allocation is a plain add on
    // these; the only shared operation is taking a new chunk.
    // This compiler's bump cursor into the shared arena. The arena itself is
    // process-wide and lives in JitCodeArena; this is the whole of what a
    // compiler owns of it.
    JitCodeArena::Cursor m_arena;

    // Everything this compiler read from the environment. Fixed at
    // construction; see jit_config.hpp for why it is one object.
    JitConfig m_cfg;
    // Sampling counter for per-chunk recent-hit accounting in execute().
    u32 m_exec_sample{0};

    // ---- Lazy-flag emission state ---------------------------------------
    //
    // Thirteen fields describing one machine, in core/cpu/jit/lazy_flag_state.hpp
    // along with the bug history that argues for their being one type. They
    // were members here, indistinguishable among 135 others.
    LazyFlagState m_flags;

    void note_lazy_kind_publish(CpuState::FlagOp op) noexcept;
    /// True for a flag writer that changes CF/OF through the overlay and
    /// leaves the lazy record's kind byte alone, so a kind prediction may
    /// survive it. See the definition for why the set is this small.
    [[nodiscard]] static bool overlay_only_flag_writer(Opcode op) noexcept;
    /// Specialised lazy-condition evaluation for a statically predicted kind.
    /// Returns false (emitting nothing) when no prediction is available.
    bool emit_eval_lazy_condition_predicted(u8 cc);

    /// Which architectural rounding-control field governs an instruction.
    /// x87 uses FCW.RC, SSE uses MXCSR.RC, and they are independent -- but the
    /// host has one FPCR, so the JIT installs the right one per class. See the
    /// comment in compile_instruction().
    enum class FpRoundingClass : u8 { None, X87, Sse };

    /// The class whose rounding mode the emitted code has installed so far in
    /// this block. Reset per block: on entry the host mode is unknown.
    FpRoundingClass m_fp_class_installed{FpRoundingClass::None};

    /// Which rounding-control field (if any) governs `op`.
    [[nodiscard]] static FpRoundingClass fp_rounding_class(Opcode op) noexcept;

    /// True when `op` loads an x87 control word or MXCSR, so the tracked class
    /// must be invalidated and the next FP instruction re-install.
    [[nodiscard]] static bool writes_fp_control_word(Opcode op) noexcept;

    /// Emit a call installing `cls`'s rounding mode on the host FPU.
    void emit_install_fp_rounding(FpRoundingClass cls);

    /// Install the block's rounding class once, in the prologue, when every FP
    /// instruction in the block agrees on a class. Keeps the guard off a
    /// mid-block backward edge. No-op for blocks with no FP instruction or
    /// with both x87 and SSE arithmetic.
    void emit_block_fp_rounding_prologue(const BasicBlock& block);


