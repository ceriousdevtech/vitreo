// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compiler_runtime_api.hpp
/// @brief JitCompiler's public runtime API: execution, L3 in-chain calls, GUI pump, hot threshold, IR gate.
///
/// NOT a standalone header: a slice of class JitCompiler's body, textually
/// included by jit_compiler.hpp inside the class definition (access on
/// entry: public). Include <core/cpu/jit/jit_compiler.hpp> instead.
#ifndef VITREO_JIT_COMPILER_CLASS_BODY
#error "jit_compiler_runtime_api.hpp is part of JitCompiler's body; include <core/cpu/jit/jit_compiler.hpp>"
#endif

public:

    /// TEST HOOK -- compile a standalone block that performs exactly one
    /// 128-bit validated guest access through emit_x64_region_load128 /
    /// emit_x64_region_store128, between guest XMM `xmm_idx` and the address
    /// in guest RAX.
    ///
    /// It exists because the production caller for those primitives is the
    /// VEX.256 memory path in ops/sse.cpp, which is a separate change. Without
    /// this hook the primitives would ship with no execution coverage at all,
    /// and emitted code that has never run is worse than no emitted code.
    /// DELETE THIS once compile_vex256_lane_split calls the primitives and the
    /// parity tests in test_jit_parity_64.cpp cover them.
    CompiledBlock* compile_vec128_probe(bool is_store, u8 xmm_idx);

    /// Execute a compiled block.
    /// Updates cpu state and returns the execution result.
    JitExecResult execute(CompiledBlock& block, CpuState& cpu);

    /// Free a compiled block's native code
    void free_block(CompiledBlock& block);

    /// Set the block cache (needed for arena_reset to invalidate entries,
    /// and to wire the inline block-link L1 lookup base into JitContext).
    void set_block_cache(BlockCache* cache);

    /// Publish the guest API thunk slab for linked-tail fast-reject.
    /// Pass [lo, hi) from ThunkRegistry; hi==lo disables the check.
    void set_thunk_region(GuestAddr lo, GuestAddr hi) noexcept {
        m_jit_ctx.thunk_region_lo = lo;
        m_jit_ctx.thunk_region_hi = hi;
    }

    /// L3: publish the registry's in-chain slot map
    /// (winapi::ThunkRegistry::in_chain_slot_map), alongside the thunk
    /// region it indexes. Null publishes the zero-slot map.
    void set_inchain_slot_map(const u8* map) noexcept {
        m_jit_ctx.inchain_slot_map =
            map != nullptr ? map
                           : reinterpret_cast<const u8*>(&kInChainEmptySlotMap);
    }

    // ---- L3: in-chain API thunk calls ------------------------------------
    //
    // An API-call tail (indirect CALL/JMP, or a CALL/JMP rel32 to a thunk)
    // whose target is a thunk the runtime serves in-chain calls it from
    // translated code through a host trampoline and resumes at the guest
    // return address without leaving execute(). The JIT side
    // (jit_inchain_thunk.cpp) keeps every invariant a dispatcher re-entry
    // re-establishes; the runtime side (runtime/execution/thunk_inchain.hpp)
    // runs the crossing exactly as the direct path does.

    /// Whether API-call tails carry the in-chain stub: on unless
    /// VITREO_JIT_INCHAIN_THUNKS=0 (the kill switch), a JIT diagnostic that
    /// counts dispatcher exits is on (VITREO_JIT_EXIT_STATS, the perf window,
    /// coverage, the emitted-ARM stats, the hot-block dump), or the runtime
    /// closed its gate (set_inchain_thunks_runtime_gate: no direct crossing
    /// at all, or a diagnostic that needs the full path), so an arm without
    /// the lever pays nothing for it. Process-wide, and part of the
    /// compile-memo key.
    [[nodiscard]] static bool inchain_thunks_codegen() noexcept;
    /// Tests only: force the environment half of that decision (1 on, 0 off,
    /// -1 back to the environment); the runtime gate still applies. Compile
    /// memo keys follow it.
    static void set_inchain_thunks_codegen_for_test(int value) noexcept;
    /// Overrides the four shared exit-stub switches (link probes, RSB push,
    /// dispatcher tails, prologue): 0 legacy per-block copies, 1 shared, -1
    /// back to the environment.
    static void set_shared_exit_stubs_for_test(int value) noexcept;
    /// Tests only: re-read VITREO_JIT_TEST_BRANCH_REACH_FATAL/_WORDS after
    /// changing them. They are read once per process otherwise.
    static void reload_branch_reach_test_knobs_for_test() noexcept;
    /// The runtime's half (cli::in_chain_thunks_possible): set before the
    /// first compile of a run, false when nothing could ever be served
    /// in-chain. Open until a runtime says otherwise.
    static void set_inchain_thunks_runtime_gate(bool open) noexcept;

    /// Publishes the runtime's serve hook for this compiler while a chain
    /// loop runs it; restores whatever an enclosing loop published.
    class InChainScope {
    public:
        InChainScope(JitCompiler* jit, InChainThunkHook hook,
                     void* env) noexcept
            : m_jit(jit) {
            if (m_jit == nullptr) return;
            m_hook = m_jit->m_jit_ctx.inchain_hook;
            m_env = m_jit->m_jit_ctx.inchain_env;
            m_jit->m_jit_ctx.inchain_hook = hook;
            m_jit->m_jit_ctx.inchain_env = env;
        }
        ~InChainScope() {
            if (m_jit == nullptr) return;
            m_jit->m_jit_ctx.inchain_hook = m_hook;
            m_jit->m_jit_ctx.inchain_env = m_env;
        }
        InChainScope(const InChainScope&) = delete;
        InChainScope& operator=(const InChainScope&) = delete;

    private:
        JitCompiler* m_jit;
        InChainThunkHook m_hook{nullptr};
        void* m_env{nullptr};
    };

    /// The InChainBreak the last execute() left, cleared. A chain loop
    /// calls it after every execute() and leaves the chain on Outer/Fatal.
    [[nodiscard]] InChainBreak take_inchain_break() noexcept {
        const u32 value = m_jit_ctx.inchain_break;
        // A load and a predictable branch on every execute(); the store only
        // when a crossing set it.
        if (value != 0) [[unlikely]] m_jit_ctx.inchain_break = 0;
        return static_cast<InChainBreak>(value);
    }
    /// In-chain crossings on this compiler that resumed in translated code
    /// (no dispatcher entry), and those that ran but left through it.
    [[nodiscard]] u64 inchain_resumed() const noexcept {
        return m_jit_ctx.inchain_resumed;
    }
    [[nodiscard]] u64 inchain_left() const noexcept {
        return m_jit_ctx.inchain_left;
    }
    /// Tests: the same two counts for the calling thread across every
    /// compiler it ran (kept only while counting is on, so a crossing pays
    /// one relaxed load for them otherwise). A resumed crossing is one after
    /// which execute() did not return: no dispatcher entry.
    static void set_inchain_counting_for_test(bool on) noexcept;
    [[nodiscard]] static u64 inchain_resumed_this_thread() noexcept;
    [[nodiscard]] static u64 inchain_left_this_thread() noexcept;
    /// Of the crossings that left, those that also told the chain loop to
    /// break to the outer dispatcher (InChainBreak::Outer).
    [[nodiscard]] static u64 inchain_broke_outer_this_thread() noexcept;

    /// The trampoline's C++ half (jit_inchain_thunk.cpp); not for callers.
    struct InChainNext {
        void* link_entry;  ///< successor to BR to, or null: leave execute()
        u64 exit_sp;       ///< native_sp_base, for the trampoline's exit
    };
    [[nodiscard]] static InChainNext inchain_from_stub(CpuState* cpu,
                                                       JitContext* ctx) noexcept;

    /// Synchronize this thread-owned block cache with process-wide executable
    /// writes. Returns true when a foreign-thread notification required a
    /// conservative full cache clear.
    bool synchronize_executable_writes();

    /// Optional host GUI pump for the main guest thread. Cold JIT of large
    /// guest libraries (e.g. Intel JPEG `ijl15` during RCT3 slideshow swaps)
    /// can run for seconds without PeekMessage/WFSO, so the execution-loop
    /// instruction pump never runs. Also invoked around `execute()` so warm
    /// linked chains still yield to AppKit. Call this from the run loop with
    /// the same Cocoa/message drain used for WaitForSingleObject.
    using GuiPump = std::function<void()>;
    void set_gui_pump(GuiPump pump) { m_gui_pump = std::move(pump); }
    void poll_gui_pump() { maybe_pump_gui(); }

    /// Max inline block-link hops before returning to the C++ dispatcher.
    /// This is a safety valve for the 16 ms GUI pump, SuspendThread's park
    /// poll and the arena read lease, not a throughput cap: 32 hops made load
    /// loops dispatcher-bound.
    ///
    /// Every counted hop bumps W22 (REG_LOOP_CTR), the chain-wide count
    /// execute() sets to 1 before it calls a block and every exit publishes
    /// as jit_iters,
    /// and leaves when bit 12 of the result is set. One register ADD instead
    /// of a JitContext load/store per hop.
    static constexpr u32 kMaxLinkedHops = 4096;
    /// UBFX W4, W22, #12, #1 (llvm-mc): nonzero iff bit 12 of the bumped W22
    /// is set. Every budget site emits ADD W22, W22, #1 right before it and
    /// pairs it with CBNZ W4 (imm19, so a split block's far fallback stays in
    /// range). From execute()'s 1, the 4095th bump sets it, so a chain of
    /// hops alone runs kMaxLinkedHops - 1 blocks (the entry and 4094 hops)
    /// and its 4095th hop returns to the dispatcher. In-block loops also count
    /// in W22 and may leave any value, but runs of values with bit 12 clear
    /// are 4096 long, so any kMaxLinkedHops + 1 consecutive bumps reach a set
    /// value and a chain still returns within that many hops of anything. A
    /// threshold (W22 >= 4096) would instead make every hop after a long
    /// loop exit. Flag-neutral, so carried guest NZCV still flows through a
    /// hop.
    static constexpr u32 kHopCapTestWord = 0x530C32C4u;

    /// Interpret-first tier: a block runs on the interpreter until its
    /// execution count reaches this many, and the execution that reaches it
    /// is the one that compiles. Most of a game load is one-shot
    /// initialisation code that would otherwise be compiled (~650 bytes of
    /// ARM64 each into a process-wide arena that is never freed per block)
    /// and then never run again. 1 restores compile-on-first-execution.
    /// Override at runtime via VITREO_JIT_HOT_THRESHOLD.
    // Keep compile-on-first-execution as the measured default. BlockCache
    // now retains bounded eviction history for interpreted blocks, so an
    // override above 1 can promote revisited blocks after decoded-cache
    // eviction. Working sets larger than that history can still lose heat;
    // changing the default requires workload measurements.
    static constexpr u64 kJitHotThreshold = 1;
    /// Upper clamp for the override. Anything larger is a request to never
    /// compile, which VITREO_NO_JIT already expresses honestly.
    static constexpr u64 kJitHotThresholdMax = u64{1} << 20;

    /// Resolve the hot threshold: the test override if one is set, else
    /// VITREO_JIT_HOT_THRESHOLD (VITREO_JIT_THRESHOLD is accepted as the
    /// older spelling), else kJitHotThreshold. Parsed once. A value that is
    /// not a whole decimal number, or is 0, falls back to the default; a
    /// value above kJitHotThresholdMax is clamped to it.
    static u64 jit_hot_threshold() noexcept {
        auto& slot = jit_hot_threshold_slot();
        u64 v = slot.load(std::memory_order_relaxed);
        if (v != 0) return v;
        v = parse_jit_hot_threshold_env();
        slot.store(v, std::memory_order_relaxed);
        return v;
    }

    /// Tests only: pin the threshold for the rest of the process (0 = go
    /// back to the environment). Safe mid-process, unlike ir_policy(): the
    /// threshold decides WHEN a block compiles, never what ABI it gets.
    static void set_jit_hot_threshold_for_test(u64 threshold) noexcept {
        if (threshold > kJitHotThresholdMax) threshold = kJitHotThresholdMax;
        jit_hot_threshold_slot().store(threshold, std::memory_order_relaxed);
    }

    /// The interpret-first gate, shared by every site that hands a
    /// decoded-but-uncompiled block to compile(): true when the execution
    /// about to happen is the one that makes `block` hot, i.e. its count
    /// reaches jit_hot_threshold() once this execution is counted.
    ///
    /// Deliberately does NOT count. A chain site that finds a cold successor
    /// hands it back to the dispatcher, which interprets it and counts it;
    /// only the site that actually runs the block counts the execution, so
    /// each execution is counted exactly once whichever path reached it.
    [[nodiscard]] static bool execution_reaches_hot_threshold(
            const BasicBlock& block) noexcept {
        return block.execution_count + 1 >= jit_hot_threshold();
    }

    /// Total number of blocks compiled
    [[nodiscard]] u64 blocks_compiled() const noexcept {
        return m_blocks_compiled;
    }

    /// Total bytes of ARM64 code generated
    [[nodiscard]] u64 bytes_generated() const noexcept {
        return m_bytes_generated;
    }


    /// Clear stale decoded/compiled state after a destructive generation
    /// change.
    bool synchronize_arena_generation();

    /// True when the most recent compile() returned null only because the
    /// arena could not hand out space right then — the current ring slot is
    /// full, or its replacement is being armed. The block is compilable and
    /// the caller must retry it later rather than record a permanent failure.
    [[nodiscard]] bool compile_deferred() const noexcept {
        return m_compile_deferred;
    }

public:
    /// Phase 2 optimizing-IR gate. `VITREO_JIT_IR` selects which guest
    /// bitnesses may attempt the IR path and `VITREO_JIT_IR_OPS` which node
    /// classes it may use; both are read ONCE into a static. A gate that could
    /// change mid-process would let an IR-compiled block link to a stencil
    /// block emitted under a different pin ABI, and linked hops jump past
    /// emit_load_pins() and inherit their predecessor's pins -- corruption
    /// that would surface only in long-running guests, far from its cause.
    enum class IrBitness : u32 { Off = 0, Bits32 = 1, Bits64 = 2, All = 3 };
    enum class IrOps : u32 { None = 0, Alu = 1u << 0 };
    /// Pure policy parse, split out so it is testable: ir_policy() must cache
    /// its result, which makes the parsing unreachable from a test that
    /// manipulates the environment, and a gate whose parsing cannot be tested
    /// is one that silently fails open.
    [[nodiscard]] static u32 parse_ir_policy(const char* mode,
                                             const char* ops) noexcept;
    [[nodiscard]] static u32 ir_policy() noexcept;
    [[nodiscard]] static IrBitness ir_policy_bitness(u32 policy) noexcept {
        return static_cast<IrBitness>(policy & 0x3u);
    }
    [[nodiscard]] static u32 ir_policy_ops(u32 policy) noexcept {
        return policy >> 2;
    }
    [[nodiscard]] static bool ir_enabled_for(bool is_64bit) noexcept;
    [[nodiscard]] static bool ir_op_enabled(IrOps op) noexcept;

    /// Tier 1 whole-block attempt, WITHOUT consulting the gate.
    ///
    /// Public because `ir_policy()` caches its parse in a `static const` --
    /// deliberately, so the gate cannot change mid-process -- which means no
    /// test can reach the accept path by setting the environment. A gate whose
    /// guarded code cannot be tested is one that silently rots. This entry is
    /// the gate-free half, so `tests/cpu/test_ir_seam_a.cpp` can drive a real
    /// `BasicBlock` through the real frontend and assert the verdict.
    ///
    /// Builds into `m_ir_block` and returns the verdict, which is also
    /// retrievable from `ir_last_result()`. Emits nothing and mutates no
    /// compiler emit state.
    const ir::BuildResult& ir_build_block(const BasicBlock& block);

    /// The verdict of the most recent `ir_build_block` on this compiler.
    [[nodiscard]] const ir::BuildResult& ir_last_result() const noexcept {
        return m_ir_result;
    }

    /// The IR of the most recent accepted block. Only meaningful when
    /// `ir_last_result().ok`.
    [[nodiscard]] const ir::IrBlock& ir_block() const noexcept {
        return m_ir_block;
    }

    /// Can `ir_emit_block` lower every op in this IR? Checked BEFORE emission
    /// begins, because emission has no failure path: the prologue is already
    /// written by then and a half-emitted body is worse than either tier.
    ///
    /// Verifies the properties the frontend happens to satisfy but the IR
    /// verifier does not enforce — every op is one this emitter lowers, no
    /// value outlives the guest instruction that defined it, no pin-resident
    /// value is read after that pin is rewritten, and no scratch-resident
    /// value is live across a memory access.
    ///
    /// Public for the same reason `ir_build_block` is: the gate caches its
    /// parse in a `static const`, so a test cannot reach this path through the
    /// environment, and an untestable gate is one that silently rots.
    [[nodiscard]] bool ir_emit_feasible(const ir::IrBlock& block) const noexcept;

    /// Why `ir_emit_feasible` said no. Meaningful only when it returned false.
    [[nodiscard]] ir::DeclineReason ir_emit_decline_reason() const noexcept {
        return m_ir_emit_decline;
    }

    /// True when the last `compile()` lowered its block through Tier 1 rather
    /// than the stencil compiler.
    ///
    /// Tests need this. A Tier 1 test that only compares against the
    /// interpreter passes just as happily when the block quietly fell back to
    /// Tier 0 — it would be asserting that Tier 0 is correct, which is not the
    /// thing under test. Note that `ir_emit_feasible` called standalone is NOT
    /// a substitute: it consults the guest-register pin map, which is only
    /// populated once `compile()` has selected a pin layout.
    [[nodiscard]] bool ir_emit_active() const noexcept {
        return m_ir_emit_active;
    }


