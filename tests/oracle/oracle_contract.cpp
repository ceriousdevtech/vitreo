// SPDX-License-Identifier: LGPL-2.1-only
#include "oracle/oracle_contract.hpp"
#include "core/math/host_fenv.hpp"

#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"

#if defined(__aarch64__)
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/linear_fault.hpp"
#include "core/cpu/decode/decoder.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include "core/cpu/state/fx_image.hpp"

namespace {

// ---------------------------------------------------------------------------
// x87/MMX transfer.
//
// Both sides marshal through the SAME 512-byte FXSAVE legacy image: the oracle
// runner via hardware FXRSTOR/FXSAVE, Vitreo via fx_image_restore/save. That is
// deliberate -- it is the one layout that already knows the two conversions a
// hand-rolled mapping always gets wrong: CpuState keeps PHYSICAL registers
// R0..R7 with TOP in the status word (the image is TOP-RELATIVE ST(0)..ST(7)),
// and CpuState keeps the FULL 16-bit tag word (the image is the ABRIDGED
// 8-bit one).
// ---------------------------------------------------------------------------
void fx_seed(vitreo::cpu::CpuState& cpu, const vitreo::oracle::State& in,
             vitreo::u8 xmm_count) {
    vitreo::u8 image[vitreo::cpu::kFxImageBytes];
    std::memset(image, 0, sizeof image);
    std::memcpy(image + 0, &in.fcw, 2);
    std::memcpy(image + 2, &in.fsw, 2);
    image[4] = in.ftw;
    std::memcpy(image + 24, &in.mxcsr, 4);
    const vitreo::u32 mxcsr_mask = 0xFFFFu;
    std::memcpy(image + 28, &mxcsr_mask, 4);
    for (int i = 0; i < 8; ++i) std::memcpy(image + 32 + i * 16, in.st[i].data(), 10);
    for (int r = 0; r < xmm_count; ++r) {
        std::memcpy(image + 160 + r * 16, in.xmm[r].data(), 16);
    }
    vitreo::cpu::fx_image_restore(cpu, image, /*wide=*/false, xmm_count);
}

void fx_readback(const vitreo::cpu::CpuState& cpu, vitreo::oracle::State& out,
                 vitreo::u8 xmm_count) {
    vitreo::u8 image[vitreo::cpu::kFxImageBytes];
    std::memset(image, 0, sizeof image);
    vitreo::cpu::fx_image_save(cpu, image, /*wide=*/false, xmm_count);
    std::memcpy(&out.fcw, image + 0, 2);
    std::memcpy(&out.fsw, image + 2, 2);
    // The x87 sticky exception flags live on the HOST unit until something
    // architectural asks for them (core/math/host_fenv.hpp); CpuState's
    // status_word only carries what a previous harvest folded in. The oracle
    // side reads its status word with a real FXSAVE, which on silicon DOES
    // report them -- so reading Vitreo's raw status_word here compares an
    // unharvested value against a harvested one and silently under-reports
    // every x87 exception. Measured on the Windows host 2026-08-30:
    // `fld m64; fld m64; faddp` gives FSW=0x3820 on silicon and 0x3800 from
    // an unharvested readback, the 0x20 being PE.
    //
    // Harvest into the outgoing image rather than into `cpu` (which is const
    // here, and rightly so). This also leaves the host's accumulator clean for
    // the next run, which is what makes back-to-back interpreter/JIT runs of
    // the same snippet comparable.
    out.fsw = static_cast<uint16_t>(out.fsw |
                                    vitreo::math::harvest_x87_exceptions());
    // The SSE class's accumulator is now separate (host_fenv.hpp). Drain it too
    // so it does not leak into the next snippet's MXCSR readback, which is what
    // makes back-to-back interpreter/JIT runs comparable. The MXCSR value the
    // harness reports comes from CpuState, which the guest's own STMXCSR /
    // FXSAVE publishes into.
    vitreo::math::discard_sse_exceptions();
    out.ftw = image[4];
    for (int i = 0; i < 8; ++i) std::memcpy(out.st[i].data(), image + 32 + i * 16, 10);
}

}  // namespace

#include <array>
#include <sstream>
#include <string>

namespace vitreo::oracle::contract {

namespace {

constexpr uint64_t kCodeBase = 0x1000;
constexpr uint64_t kRegionSize = 0x10000;
constexpr uint64_t kMemBase = 0x04000000;   // matches oracle ORACLE_MEM_VA
constexpr uint64_t kMemSize = 0x1000;
constexpr uint32_t kMaxSteps = 64;

#if defined(__aarch64__)
/// JitCompiler::execute() as the dispatcher drives it. When an arena
/// retirement round has happened since this compiler last synchronized (a
/// sweep that compiles thousands of forms crosses the arena soft budget),
/// execute() purges its stale pointers and reports StaleCode once WITHOUT
/// entering native code; the dispatcher simply re-enters. Read as "no
/// progress", that one report becomes a fabricated guest fault. A second
/// StaleCode means the block itself was retired: the caller reports the case
/// as not adjudicable. (tests/cpu/jit_dispatch_execute.hpp is the gtest twin.)
vitreo::cpu::jit::JitExecResult execute_like_dispatcher(
    vitreo::cpu::jit::JitCompiler& jit, vitreo::cpu::jit::CompiledBlock& block,
    vitreo::cpu::CpuState& cpu) {
    const auto first = jit.execute(block, cpu);
    if (first != vitreo::cpu::jit::JitExecResult::StaleCode) return first;
    return jit.execute(block, cpu);
}
#endif

// 64-bit snippets are loaded at the same VA the oracle runner MAP_FIXEDs them
// at, so RIP-relative displacements resolve identically on both sides.
constexpr uint64_t kCodeBase64 = vitreo::oracle::kCodeVa64;
constexpr uint64_t kRegion64Size = 0x10000;

}  // namespace

State run_on_interpreter(const State& in, const std::vector<uint8_t>& code) {
    using namespace vitreo::cpu;
    using vitreo::GuestMemory;
    using vitreo::MemoryProtection;

    State out = in;                 // default: pass through
    out.status = 6;                 // "setup" until we successfully execute
    out.bytes_run = 0;

    if (code.empty() || code.size() > 0x1000) {
        if (code.empty()) {
            out.status = 0;
            return out;
        }
        return out;
    }

    auto memory = std::make_unique<GuestMemory>();
    if (!memory->map_region(kCodeBase, kRegionSize, MemoryProtection::ReadWrite)) {
        return out;
    }
    if (!memory->write_bytes(kCodeBase, code.data(), code.size())) {
        return out;
    }
    // Scratch memory page shared with the oracle (interp path).
    if (!memory->map_region(kMemBase, kMemSize, MemoryProtection::ReadWrite)) {
        return out;
    }
    if (!memory->write_bytes(kMemBase, in.mem.data(), in.mem.size())) {
        return out;
    }

    auto cpu = std::make_unique<CpuState>();
    cpu->init_protected_mode();  // 32-bit: matches oracle's effective mode for non-REX snippets

    // Load GPRs (low 32 bits). Skip ESP for parity with the oracle which also
    // does not stomp the real RSP during the snippet.
    for (int i = 0; i < 8; ++i) {
        if (i == 4) continue;  // ESP
        cpu->gpr.set32(static_cast<u8>(i), static_cast<u32>(in.gpr[i]));
    }
    // Give the snippet a sensible stack well away from the code region.
    cpu->gpr.set32(static_cast<u8>(RegIndex::RSP), static_cast<u32>(kCodeBase + kRegionSize - 0x100));

    // Load EFLAGS (u64 RFLAGS, but only lower 32 matter in protected mode).
    cpu->flags.rflags = in.rflags & 0xFFFF'FFFFull;

    // Load MXCSR and XMM0..XMM7.
    cpu->sse.mxcsr = in.mxcsr;
    for (int r = 0; r < 8; ++r) {
        std::memcpy(&cpu->sse.xmm[r], in.xmm[r].data(), 16);
    }
    fx_seed(*cpu, in, 8);
    // Isolate this run's floating-point flags.
    //
    // The host FPU's sticky flags are per-thread and outlive a run, and the
    // harness's OWN work raises them: marshalling the x87 register file at
    // readback converts Float80 values, and those conversions signal. Attributed
    // at the next class transition, they landed in whichever accumulator the
    // FOLLOWING run happened to own — so one case's marshalling showed up as
    // the next case's MXCSR, and the interpreter appeared to raise IE on
    // `VFMADD132SS` with infinity operands where hardware raises nothing.
    //
    // Clearing here means each run reports only the arithmetic it performed.
    vitreo::math::clear_host_fp_exceptions();

    cpu->rip = kCodeBase;
    const uint64_t code_end = kCodeBase + code.size();

    Interpreter interp(*cpu, *memory);
    uint32_t steps = 0;
    bool faulted = false;
    while (cpu->rip < code_end && steps < kMaxSteps) {
        ExecResult r = interp.step();
        ++steps;
        if (r == ExecResult::Exception ||
            r == ExecResult::Halt ||
            r == ExecResult::Interrupt ||
            r == ExecResult::Syscall ||
            r == ExecResult::IOPort) {
            faulted = true;
            break;
        }
    }

    // Settle lazy flags before projecting EFLAGS.
    //
    // The lazy-flag design leaves CpuState::flags.rflags stale until something
    // asks for it architecturally, and a Jcc answered through
    // try_lazy_condition never forces a materialize. So a snippet ending in
    // `TEST edi, edi; JE` leaves the TEST's ZF/PF pending and this readback
    // would report the *seed* flags — silently comparing nothing.
    //
    // The observer is what must settle, exactly as GetThreadContext does in
    // production (thread_suspend.cpp). Both run_on_jit paths already do this;
    // the interpreter paths did not.
    cpu->materialize_lazy_flags();

    // Read back GPRs (low 32 bits, zero-extended — mirrors the runner's
    // normalization of a 32-bit result).
    for (int i = 0; i < 8; ++i) {
        out.gpr[i] = cpu->gpr.get32(static_cast<u8>(i));
    }
    for (int i = 8; i < 16; ++i) out.gpr[i] = 0;
    out.gpr[4] = in.gpr[4] & 0xFFFF'FFFFull;  // preserve ESP field (neither side owns it)

    out.mode = vitreo::oracle::kMode32;
    out.rflags = cpu->flags.rflags & 0xFFFF'FFFFull;
    // MXCSR's sticky exception bits are lazy in both engines: the host FPU
    // accumulates them and they are folded into architectural state only where
    // the guest can observe them (STMXCSR, FXSAVE). A harness that reads
    // CpuState directly never triggers that, so harvest here — exactly as the
    // x87 status word is harvested in fx_readback above. Without it every
    // comparison against hardware reports MXCSR as its reset value.
    out.mxcsr = cpu->sse.mxcsr |
                static_cast<uint32_t>(vitreo::math::harvest_sse_exceptions());
    fx_readback(*cpu, out, 8);
    for (int r = 0; r < 8; ++r) {
        std::memcpy(out.xmm[r].data(), &cpu->sse.xmm[r], 16);
    }
    for (int r = 8; r < 16; ++r) out.xmm[r].fill(0);
    // Read back scratch memory.
    (void)memory->read_bytes(kMemBase, out.mem.data(), out.mem.size());
    out.mem_va = static_cast<uint32_t>(kMemBase);

    if (faulted) {
        // Best-effort mapping; specific signal classification happens on the
        // oracle side. For contract comparisons a non-zero status is enough to
        // signal "fault occurred".
        out.status = 5;
    } else if (cpu->rip == code_end) {
        out.status = 0;
    } else {
        // Ran out of steps before covering the whole snippet.
        out.status = 5;
    }
    out.bytes_run = static_cast<uint32_t>(cpu->rip - kCodeBase);
    return out;
}

// ============================================================================
// JIT contract adapter (ARM64 only)
// ============================================================================

#if defined(__aarch64__)

State run_on_jit(const State& in, const std::vector<uint8_t>& code) {
    using namespace vitreo::cpu;
    using vitreo::GuestMemory;
    using vitreo::MemoryProtection;
    using vitreo::cpu::jit::JitCompiler;
    using vitreo::cpu::jit::CompiledBlock;
    using vitreo::cpu::jit::JitExecResult;

    State out = in;
    out.status = 6;  // setup
    out.bytes_run = 0;

    if (code.empty()) {
        out.status = 0;
        return out;
    }
    if (code.size() > 0x1000) {
        return out;
    }

    auto memory = std::make_unique<GuestMemory>();
    if (!memory->map_region(kCodeBase, kRegionSize, MemoryProtection::ReadWrite)) {
        return out;
    }
    if (!memory->write_bytes(kCodeBase, code.data(), code.size())) {
        return out;
    }
    // Scratch memory page shared with the oracle (JIT path). Must mirror the
    // interpreter adapter so snippets that reference [EDI]=kMemBase see the
    // same initial bytes; otherwise the JIT's guest-memory reads fail silently
    // and jit_sse_op leaves dst unchanged, producing spurious "divergences".
    if (!memory->map_region(kMemBase, kMemSize, MemoryProtection::ReadWrite)) {
        return out;
    }
    if (!memory->write_bytes(kMemBase, in.mem.data(), in.mem.size())) {
        return out;
    }

    auto cpu = std::make_unique<CpuState>();
    cpu->init_protected_mode();

    for (int i = 0; i < 8; ++i) {
        if (i == 4) continue;  // ESP
        cpu->gpr.set32(static_cast<u8>(i), static_cast<u32>(in.gpr[i]));
    }
    cpu->gpr.set32(static_cast<u8>(RegIndex::RSP),
                   static_cast<u32>(kCodeBase + kRegionSize - 0x100));
    cpu->flags.rflags = in.rflags & 0xFFFF'FFFFull;
    cpu->sse.mxcsr = in.mxcsr;
    for (int r = 0; r < 8; ++r) {
        std::memcpy(&cpu->sse.xmm[r], in.xmm[r].data(), 16);
    }
    fx_seed(*cpu, in, 8);
    // Isolate this run's floating-point flags.
    //
    // The host FPU's sticky flags are per-thread and outlive a run, and the
    // harness's OWN work raises them: marshalling the x87 register file at
    // readback converts Float80 values, and those conversions signal. Attributed
    // at the next class transition, they landed in whichever accumulator the
    // FOLLOWING run happened to own — so one case's marshalling showed up as
    // the next case's MXCSR, and the interpreter appeared to raise IE on
    // `VFMADD132SS` with infinity operands where hardware raises nothing.
    //
    // Clearing here means each run reports only the arithmetic it performed.
    vitreo::math::clear_host_fp_exceptions();
    cpu->rip = kCodeBase;

    // Execute the snippet to completion, FOLLOWING CONTROL FLOW.
    //
    // This used to decode every byte into one BasicBlock, compile it once and
    // execute it once. That is only correct for straight-line snippets: the
    // JIT terminates a block at a branch, so a snippet containing an in-block
    // Jcc had its tail silently never executed while the oracle ran the whole
    // thing, and the difference was reported as a CPU divergence. It also
    // computed `partial` (JIT compiled fewer bytes than given, i.e. some
    // opcode lacked a stencil) and then never used it, so a partially executed
    // snippet was compared as if complete.
    JitCompiler jit(*memory);
    const GuestAddr code_end = kCodeBase + code.size();
    // Forward-only snippets terminate quickly; the cap only stops a runaway
    // backward branch from spinning here.
    constexpr int kMaxBlocks = 64;
    bool any_partial = false;
    for (int iter = 0; iter < kMaxBlocks; ++iter) {
        if (cpu->rip < kCodeBase || cpu->rip >= code_end) break;
        const size_t off = static_cast<size_t>(cpu->rip - kCodeBase);

        vitreo::cpu::jit::BasicBlock block;
        block.start_addr = static_cast<GuestAddr>(cpu->rip);
        MemoryByteReader reader(code.data() + off, code.size() - off);
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            DecodeError derr =
                decode_instruction(reader, DecodeMode::Mode32, inst);
            if (derr != DecodeError::Success) {
                // Decoder rejected — treat as unsupported for JIT purposes.
                out.status = 7;
                return out;
            }
            block.instructions.push_back(inst);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count = static_cast<u32>(block.instructions.size());

        CompiledBlock* cb = jit.compile(block);
        if (!cb) {
            // First instruction of this block lacked a stencil.
            out.status = 7;
            return out;
        }
        if (cb->guest_size < block.byte_length) any_partial = true;

        const u64 rip_before = cpu->rip;
        JitExecResult r = execute_like_dispatcher(jit, *cb, *cpu);
        if (r == JitExecResult::Unsupported || r == JitExecResult::StaleCode) {
            out.status = 7;
            return out;
        }
        if (cpu->rip == rip_before) break;  // no progress; stop rather than spin
    }

    // A snippet the JIT could not translate in full is not adjudicable: report
    // "unsupported" so the caller skips it instead of comparing a prefix.
    if (any_partial && (cpu->rip < kCodeBase || cpu->rip >= code_end)) {
        // Ran off the end anyway — the untranslated bytes were never reached,
        // so the comparison is still sound.
        any_partial = false;
    }
    if (any_partial) {
        out.status = 7;
        return out;
    }

    // Required before reading architectural EFLAGS.
    cpu->materialize_lazy_flags();

    for (int i = 0; i < 8; ++i) {
        out.gpr[i] = cpu->gpr.get32(static_cast<u8>(i));
    }
    for (int i = 8; i < 16; ++i) out.gpr[i] = 0;
    out.gpr[4] = in.gpr[4] & 0xFFFF'FFFFull;  // preserve ESP field
    out.mode = vitreo::oracle::kMode32;
    out.rflags = cpu->flags.rflags & 0xFFFF'FFFFull;
    // MXCSR's sticky exception bits are lazy in both engines: the host FPU
    // accumulates them and they are folded into architectural state only where
    // the guest can observe them (STMXCSR, FXSAVE). A harness that reads
    // CpuState directly never triggers that, so harvest here — exactly as the
    // x87 status word is harvested in fx_readback above. Without it every
    // comparison against hardware reports MXCSR as its reset value.
    out.mxcsr = cpu->sse.mxcsr |
                static_cast<uint32_t>(vitreo::math::harvest_sse_exceptions());
    fx_readback(*cpu, out, 8);
    for (int r2 = 0; r2 < 8; ++r2) {
        std::memcpy(out.xmm[r2].data(), &cpu->sse.xmm[r2], 16);
    }
    for (int r2 = 8; r2 < 16; ++r2) out.xmm[r2].fill(0);
    memory->read_bytes(kMemBase, out.mem.data(), out.mem.size());
    out.mem_va = static_cast<uint32_t>(kMemBase);

    // The partial-translation case already returned status 7 above. Here the
    // only question is whether control actually ran off the end of the
    // snippet; the loop follows in-snippet branches, so landing exactly on
    // code_end is the success condition for branching and straight-line
    // snippets alike.
    if (cpu->rip == code_end) {
        out.status = 0;
    } else {
        out.status = 5;
    }
    out.bytes_run = static_cast<uint32_t>(cpu->rip - kCodeBase);
    return out;
}

#else  // !__aarch64__

State run_on_jit(const State& in, const std::vector<uint8_t>&) {
    State out = in;
    out.status = 7;  // unsupported platform
    out.bytes_run = 0;
    return out;
}

#endif

// ============================================================================
// 64-bit (long mode) contract adapters
// ============================================================================

namespace {

void load_cpu64(vitreo::cpu::CpuState& cpu,
                const vitreo::oracle::State& in) {
    using namespace vitreo::cpu;

    cpu.init_long_mode();
    for (int i = 0; i < 16; ++i) {
        if (i == 4 && in.gpr[4] == 0) continue;
        cpu.gpr.set64(static_cast<u8>(i), in.gpr[i]);
    }
    if (in.gpr[4] == 0) {
        cpu.gpr.set64(static_cast<u8>(RegIndex::RSP),
                      kCodeBase64 + kRegion64Size - 0x100);
    }
    cpu.flags.rflags = in.rflags;
    cpu.sse.mxcsr = in.mxcsr;
    for (int r = 0; r < 16; ++r) {
        std::memcpy(&cpu.sse.xmm[r], in.xmm[r].data(), 16);
    }
    fx_seed(cpu, in, 8);
    // Isolate this run's floating-point flags.
    //
    // The host FPU's sticky flags are per-thread and outlive a run, and the
    // harness's OWN work raises them: marshalling the x87 register file at
    // readback converts Float80 values, and those conversions signal. Attributed
    // at the next class transition, they landed in whichever accumulator the
    // FOLLOWING run happened to own — so one case's marshalling showed up as
    // the next case's MXCSR, and the interpreter appeared to raise IE on
    // `VFMADD132SS` with infinity operands where hardware raises nothing.
    //
    // Clearing here means each run reports only the arithmetic it performed.
    vitreo::math::clear_host_fp_exceptions();
    cpu.rip = kCodeBase64;
}

// Shared setup for both 64-bit adapters. Returns false on setup failure.
bool setup64(const State& in, const std::vector<uint8_t>& code,
             std::unique_ptr<vitreo::GuestMemory>& memory,
             std::unique_ptr<vitreo::cpu::CpuState>& cpu) {
    using namespace vitreo::cpu;
    using vitreo::GuestMemory;
    using vitreo::MemoryProtection;

    memory = std::make_unique<GuestMemory>();
    if (!memory->map_region(kCodeBase64, kRegion64Size, MemoryProtection::ReadWrite)) {
        return false;
    }
    if (!code.empty() &&
        !memory->write_bytes(kCodeBase64, code.data(), code.size())) return false;
    if (!memory->map_region(kMemBase, kMemSize, MemoryProtection::ReadWrite)) return false;
    if (!memory->write_bytes(kMemBase, in.mem.data(), in.mem.size())) return false;

    cpu = std::make_unique<CpuState>();
    load_cpu64(*cpu, in);
    return true;
}

// Project the executed CpuState back into oracle::State layout.
void readback64(const State& in, const vitreo::cpu::CpuState& cpu,
                vitreo::GuestMemory& memory, State& out) {
    for (int i = 0; i < 16; ++i) {
        out.gpr[i] = cpu.gpr.get64(static_cast<vitreo::u8>(i));
    }
    if (in.gpr[4] == 0) {
        out.gpr[4] = in.gpr[4];  // preserve unused RSP field
    }
    out.mode = vitreo::oracle::kMode64;
    out.rflags = cpu.flags.rflags;
    // MXCSR's sticky exception bits are lazy in both engines: the host FPU
    // accumulates them and they are folded into architectural state only where
    // the guest can observe them (STMXCSR, FXSAVE). A harness that reads
    // CpuState directly never triggers that, so harvest here — exactly as the
    // x87 status word is harvested in fx_readback above. Without it every
    // comparison against hardware reports MXCSR as its reset value.
    out.mxcsr = cpu.sse.mxcsr |
                static_cast<uint32_t>(vitreo::math::harvest_sse_exceptions());
    fx_readback(cpu, out, 16);
    for (int r = 0; r < 16; ++r) {
        std::memcpy(out.xmm[r].data(), &cpu.sse.xmm[r], 16);
    }
    (void)memory.read_bytes(kMemBase, out.mem.data(), out.mem.size());
    out.mem_va = static_cast<uint32_t>(kMemBase);
    out.rip = cpu.rip;
}

}  // namespace

State run_on_interpreter_64(const State& in, const std::vector<uint8_t>& code) {
    using namespace vitreo::cpu;

    State out = in;
    out.mode = vitreo::oracle::kMode64;
    out.status = 6;
    out.bytes_run = 0;

    if (code.empty()) { out.status = 0; return out; }
    if (code.size() > 0x1000) return out;

    std::unique_ptr<vitreo::GuestMemory> memory;
    std::unique_ptr<CpuState> cpu;
    if (!setup64(in, code, memory, cpu)) return out;

    const uint64_t code_end = kCodeBase64 + code.size();
    Interpreter interp(*cpu, *memory);
    uint32_t steps = 0;
    bool faulted = false;
    while (cpu->rip < code_end && steps < kMaxSteps) {
        ExecResult r = interp.step();
        ++steps;
        if (r == ExecResult::Exception || r == ExecResult::Halt ||
            r == ExecResult::Interrupt || r == ExecResult::Syscall ||
            r == ExecResult::IOPort) {
            faulted = true;
            break;
        }
    }

    // Settle lazy flags before readback64 projects EFLAGS — see the note in
    // run_on_interpreter. run_on_jit_64 already does this.
    cpu->materialize_lazy_flags();
    readback64(in, *cpu, *memory, out);
    out.status = (!faulted && cpu->rip == code_end) ? 0u : 5u;
    out.bytes_run = static_cast<uint32_t>(cpu->rip - kCodeBase64);
    return out;
}

#if defined(__aarch64__)

State run_on_jit_64(const State& in, const std::vector<uint8_t>& code) {
    using namespace vitreo::cpu;
    using vitreo::cpu::jit::JitCompiler;
    using vitreo::cpu::jit::CompiledBlock;
    using vitreo::cpu::jit::JitExecResult;

    State out = in;
    out.mode = vitreo::oracle::kMode64;
    out.status = 6;
    out.bytes_run = 0;

    if (code.empty()) { out.status = 0; return out; }
    if (code.size() > 0x1000) return out;

    std::unique_ptr<vitreo::GuestMemory> memory;
    std::unique_ptr<CpuState> cpu;
    if (!setup64(in, code, memory, cpu)) return out;

    // Execute to completion FOLLOWING CONTROL FLOW -- see the twin comment in
    // run_on_jit(). This matters more here than in 32-bit: the JIT's in-block
    // forward-Jcc join and its internal-join liveness proof are gated on
    // `block.is_64bit`, so a snippet with an in-block Jcc is the only way to
    // reach that machinery at all, and compiling one block and stopping made
    // every such snippet look like a divergence.
    JitCompiler jit(*memory);
    const uint64_t code_end = kCodeBase64 + code.size();
    constexpr int kMaxBlocks = 64;
    bool any_partial = false;
    for (int iter = 0; iter < kMaxBlocks; ++iter) {
        if (cpu->rip < kCodeBase64 || cpu->rip >= code_end) break;
        const size_t off = static_cast<size_t>(cpu->rip - kCodeBase64);

        vitreo::cpu::jit::BasicBlock block;
        block.start_addr = static_cast<vitreo::GuestAddr>(cpu->rip);
        // Without this the compiler takes the 32-bit path (native NEON scalar
        // FP, 32-bit stencil set) and diverges from long-mode semantics — most
        // visibly as wrong NaN propagation on MULSS/DIVSS, where ARM FDIV's
        // operand-order selection does not match Intel SDM Vol. 1 §4.8.3.4.
        block.is_64bit = true;
        MemoryByteReader reader(code.data() + off, code.size() - off);
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            DecodeError derr =
                decode_instruction(reader, DecodeMode::Mode64, inst);
            if (derr != DecodeError::Success) {
                out.status = 7;  // decoder rejected — treat as unsupported
                return out;
            }
            block.instructions.push_back(inst);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count = static_cast<u32>(block.instructions.size());

        CompiledBlock* cb = jit.compile(block);
        if (!cb) { out.status = 7; return out; }
        if (cb->guest_size < block.byte_length) any_partial = true;

        const u64 rip_before = cpu->rip;
        JitExecResult r = execute_like_dispatcher(jit, *cb, *cpu);
        if (r == JitExecResult::Unsupported || r == JitExecResult::StaleCode) {
            out.status = 7;
            return out;
        }
        if (cpu->rip == rip_before) break;  // no progress; stop rather than spin
    }

    cpu->materialize_lazy_flags();
    readback64(in, *cpu, *memory, out);

    const bool ran_off_end = (cpu->rip == code_end);
    if (any_partial && !ran_off_end) {
        out.status = 7;  // untranslated bytes may still be reachable
    } else if (ran_off_end) {
        out.status = 0;
    } else {
        out.status = 5;
    }
    out.bytes_run = static_cast<uint32_t>(cpu->rip - kCodeBase64);
    return out;
}

#else  // !__aarch64__

State run_on_jit_64(const State& in, const std::vector<uint8_t>&) {
    State out = in;
    out.mode = vitreo::oracle::kMode64;
    out.status = 7;  // unsupported platform
    out.bytes_run = 0;
    return out;
}

#endif

std::string compare(const State& got, const State& want, uint32_t flags_mask) {
    CompareOpts opts;
    opts.flags_mask = flags_mask;
    return compare(got, want, opts);
}

std::string compare(const State& got, const State& want, CompareOpts opts) {
    std::ostringstream os;
    const bool w64 = got.is64() || want.is64();
    static const char* gpr32_names[16] = {
        "EAX","ECX","EDX","EBX","ESP","EBP","ESI","EDI",
        "R8D","R9D","R10D","R11D","R12D","R13D","R14D","R15D"};
    static const char* gpr64_names[16] = {
        "RAX","RCX","RDX","RBX","RSP","RBP","RSI","RDI",
        "R8","R9","R10","R11","R12","R13","R14","R15"};
    const int ngpr = w64 ? 16 : 8;
    const int nxmm = w64 ? 16 : 8;
    const uint16_t ignore = static_cast<uint16_t>(
        opts.gpr_ignore_mask | (opts.compare_rsp ? 0u : (1u << 4)));
    for (int i = 0; i < ngpr; ++i) {
        if (ignore & (1u << i)) continue;
        if (got.gpr[i] != want.gpr[i]) {
            char buf[112];
            std::snprintf(buf, sizeof(buf), "  %s: interp=%016llx oracle=%016llx\n",
                          (w64 ? gpr64_names : gpr32_names)[i],
                          static_cast<unsigned long long>(got.gpr[i]),
                          static_cast<unsigned long long>(want.gpr[i]));
            os << buf;
        }
    }
    uint32_t gf = got.eflags32() & opts.flags_mask;
    uint32_t wf = want.eflags32() & opts.flags_mask;
    if (gf != wf) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "  EFLAGS(arith): interp=%03x oracle=%03x (xor=%03x)\n",
                      gf, wf, gf ^ wf);
        os << buf;
    }
    if (got.mxcsr != want.mxcsr) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "  MXCSR: interp=%08x oracle=%08x\n",
                      got.mxcsr, want.mxcsr);
        os << buf;
    }
    for (int r = 0; r < nxmm; ++r) {
        if (got.xmm[r] != want.xmm[r]) {
            os << "  XMM" << r << ": interp=";
            char hex[3];
            for (int b = 15; b >= 0; --b) {
                std::snprintf(hex, sizeof(hex), "%02x", got.xmm[r][b]);
                os << hex;
            }
            os << " oracle=";
            for (int b = 15; b >= 0; --b) {
                std::snprintf(hex, sizeof(hex), "%02x", want.xmm[r][b]);
                os << hex;
            }
            os << "\n";
        }
    }
    // ---- x87 / MMX register file -------------------------------------------
    // Carried in FXSAVE terms: ST(i) is TOP-relative and FTW is the abridged
    // 8-bit tag, so this covers MM0..MM7 too (MMx aliases the low 8 bytes of
    // st[x]).
    if (opts.compare_x87) {
        auto hex_st = [](const std::array<uint8_t, 10>& v) {
            std::string o = "0x";
            char b2[3];
            for (int i = 9; i >= 0; --i) {
                std::snprintf(b2, sizeof(b2), "%02x", v[i]);
                o += b2;
            }
            return o;
        };
        if (got.fcw != want.fcw) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "  FCW: interp=%04x oracle=%04x\n",
                          got.fcw, want.fcw);
            os << buf;
        }
        const uint16_t gs = static_cast<uint16_t>(got.fsw & opts.fsw_mask);
        const uint16_t ws = static_cast<uint16_t>(want.fsw & opts.fsw_mask);
        if (gs != ws) {
            char buf[112];
            std::snprintf(buf, sizeof(buf),
                          "  FSW: interp=%04x oracle=%04x (xor=%04x)\n",
                          gs, ws, static_cast<uint16_t>(gs ^ ws));
            os << buf;
        }
        if (!opts.skip_x87_tags_and_regs) {
            if (got.ftw != want.ftw) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "  FTW: interp=%02x oracle=%02x\n",
                              got.ftw, want.ftw);
                os << buf;
            }
            // The abridged FTW is indexed by PHYSICAL register while st[] is
            // TOP-relative, so a tag lookup for ST(i) has to go through TOP:
            // physical = (TOP + i) & 7. Comparing bit i directly reads the
            // wrong register's tag whenever TOP != 0, which is every case that
            // has pushed -- it is what made the popped slot look live on both
            // sides and report a bogus ST7 diff.
            auto tag_live = [](uint16_t fsw, uint8_t ftw, int i) {
                const int top = (fsw >> 11) & 7;
                return ((ftw >> ((top + i) & 7)) & 1u) != 0u;
            };
            for (int i = 0; i < 8; ++i) {
                // Only registers BOTH sides tag as live are architectural.
                // A slot that either side calls empty holds whatever stale
                // bits the physical register happened to keep, and the two
                // sides legitimately differ there -- after a pop, silicon
                // leaves the popped value in the vacated slot while Rosetta
                // writes the indefinite QNaN (measured on Windows
                // 2026-08-30: `fld m64; fld m64; faddp` leaves ST7 == the
                // result on hardware, and Vitreo matches hardware).
                //
                // Requiring both sides loses no coverage: if Vitreo wrongly
                // tags a register empty, the FTW comparison right above
                // catches it.
                const bool live = tag_live(want.fsw, want.ftw, i) &&
                                  tag_live(got.fsw, got.ftw, i);
                if (!live) continue;
                if (got.st[i] != want.st[i]) {
                    os << "  ST" << i << ": interp=" << hex_st(got.st[i])
                       << " oracle=" << hex_st(want.st[i]) << "\n";
                }
            }
        }
    }

    if ((got.status == 0) != (want.status == 0)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "  fault divergence: interp_status=%u oracle_status=%u\n",
                      got.status, want.status);
        os << buf;
    }
    auto in_skip = [&](size_t b) {
        return (b >= opts.mem_skip_off &&
                b < opts.mem_skip_off + opts.mem_skip_len) ||
               (b >= opts.mem_skip2_off &&
                b < opts.mem_skip2_off + opts.mem_skip2_len) ||
               (b >= opts.mem_skip3_off &&
                b < opts.mem_skip3_off + opts.mem_skip3_len);
    };
    int first_mem = -1;
    for (size_t b = 0; b < got.mem.size() && b < want.mem.size(); ++b) {
        if (in_skip(b)) continue;
        if (got.mem[b] != want.mem[b]) {
            first_mem = static_cast<int>(b);
            break;
        }
    }
    if (first_mem >= 0) {
        const size_t lo = static_cast<size_t>(first_mem > 8 ? first_mem - 8 : 0);
        const size_t hi = std::min(lo + 32, got.mem.size());
        char hdr[80];
        std::snprintf(hdr, sizeof(hdr), "  MEM first-diff @+%u: interp=",
                      static_cast<unsigned>(first_mem));
        os << hdr;
        char hex[3];
        for (size_t b = lo; b < hi; ++b) {
            std::snprintf(hex, sizeof(hex), "%02x", got.mem[b]);
            os << hex;
        }
        os << " oracle=";
        for (size_t b = lo; b < hi; ++b) {
            std::snprintf(hex, sizeof(hex), "%02x", want.mem[b]);
            os << hex;
        }
        os << "\n";
    }
    return os.str();
}

namespace {

void load_cpu32(vitreo::cpu::CpuState& cpu, const State& in) {
    using namespace vitreo::cpu;
    cpu.init_protected_mode();
    for (int i = 0; i < 8; ++i) {
        if (i == 4 && in.gpr[4] == 0) continue;  // ESP — live only when requested
        cpu.gpr.set32(static_cast<u8>(i), static_cast<u32>(in.gpr[i]));
    }
    if (in.gpr[4] == 0) {
        cpu.gpr.set32(static_cast<u8>(RegIndex::RSP),
                      static_cast<u32>(kCodeBase + kRegionSize - 0x100));
    }
    cpu.flags.rflags = in.rflags & 0xFFFF'FFFFull;
    cpu.sse.mxcsr = in.mxcsr;
    for (int r = 0; r < 8; ++r) {
        std::memcpy(&cpu.sse.xmm[r], in.xmm[r].data(), 16);
    }
    fx_seed(cpu, in, 16);
    // Isolate this run's floating-point flags.
    //
    // The host FPU's sticky flags are per-thread and outlive a run, and the
    // harness's OWN work raises them: marshalling the x87 register file at
    // readback converts Float80 values, and those conversions signal. Attributed
    // at the next class transition, they landed in whichever accumulator the
    // FOLLOWING run happened to own — so one case's marshalling showed up as
    // the next case's MXCSR, and the interpreter appeared to raise IE on
    // `VFMADD132SS` with infinity operands where hardware raises nothing.
    //
    // Clearing here means each run reports only the arithmetic it performed.
    vitreo::math::clear_host_fp_exceptions();
    cpu.rip = kCodeBase;
}

void store_cpu32(const State& in, const vitreo::cpu::CpuState& cpu,
                 vitreo::GuestMemory& memory, State& out) {
    using namespace vitreo::cpu;
    for (int i = 0; i < 8; ++i) {
        out.gpr[i] = cpu.gpr.get32(static_cast<u8>(i));
    }
    for (int i = 8; i < 16; ++i) out.gpr[i] = 0;
    out.gpr[4] = in.gpr[4] & 0xFFFF'FFFFull;
    out.mode = vitreo::oracle::kMode32;
    out.rflags = cpu.flags.rflags & 0xFFFF'FFFFull;
    // MXCSR's sticky exception bits are lazy in both engines: the host FPU
    // accumulates them and they are folded into architectural state only where
    // the guest can observe them (STMXCSR, FXSAVE). A harness that reads
    // CpuState directly never triggers that, so harvest here — exactly as the
    // x87 status word is harvested in fx_readback above. Without it every
    // comparison against hardware reports MXCSR as its reset value.
    out.mxcsr = cpu.sse.mxcsr |
                static_cast<uint32_t>(vitreo::math::harvest_sse_exceptions());
    fx_readback(cpu, out, 16);
    for (int r = 0; r < 8; ++r) {
        std::memcpy(out.xmm[r].data(), &cpu.sse.xmm[r], 16);
    }
    for (int r = 8; r < 16; ++r) out.xmm[r].fill(0);
    (void)memory.read_bytes(kMemBase, out.mem.data(), out.mem.size());
    out.mem_va = static_cast<uint32_t>(kMemBase);
    out.bytes_run = static_cast<uint32_t>(cpu.rip - kCodeBase);
}

uint64_t now_ns() {
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            clock::now().time_since_epoch()).count());
}

}  // namespace

JitBenchResult bench_on_jit(const State& in, const std::vector<uint8_t>& code,
                            uint32_t warmup, uint32_t iters) {
    JitBenchResult result;
    result.out = in;
    result.out.status = 6;
#if !defined(__aarch64__)
    result.out.status = 7;
    return result;
#else
    using namespace vitreo::cpu;
    using vitreo::GuestMemory;
    using vitreo::MemoryProtection;
    using vitreo::cpu::jit::JitCompiler;
    using vitreo::cpu::jit::CompiledBlock;
    using vitreo::cpu::jit::JitExecResult;

    if (code.empty() || code.size() > 0x1000) {
        result.out.status = code.empty() ? 0 : 6;
        return result;
    }

    auto memory = std::make_unique<GuestMemory>();
    // 32-bit production guests are linear. Without this, SSE mem128 misses
    // the two-region cache and every access is jit_sse_op (~10× Rosetta).
    // 128 MiB covers kMemBase (64 MiB) plus the code window.
    constexpr usize kBenchLinearSize = 0x08000000ull;
    if (!memory->enable_linear_reservation(kBenchLinearSize)) return result;
    // Direct-linear lowering is legal only while faults in this reservation
    // are registered as recoverable. Keep the registration stack-owned and
    // declared after GuestMemory so it is removed before the mapping dies.
    vitreo::cpu::jit::LinearReservationRegistration linear_registration;
    if (!linear_registration.acquire(memory->linear_base(), kBenchLinearSize)) {
        return result;
    }
    if (!memory->map_region(kCodeBase, kRegionSize, MemoryProtection::ReadWrite)) {
        return result;
    }
    if (!memory->write_bytes(kCodeBase, code.data(), code.size())) return result;
    if (!memory->map_region(kMemBase, kMemSize, MemoryProtection::ReadWrite)) {
        return result;
    }
    if (!memory->write_bytes(kMemBase, in.mem.data(), in.mem.size())) return result;

    vitreo::cpu::jit::BasicBlock block;
    block.start_addr = kCodeBase;
    MemoryByteReader reader(code.data(), code.size());
    while (reader.remaining() > 0) {
        DecodedInstruction inst;
        if (decode_instruction(reader, DecodeMode::Mode32, inst) !=
            DecodeError::Success) {
            result.out.status = 7;
            return result;
        }
        block.instructions.push_back(inst);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());

    auto cpu = std::make_unique<CpuState>();
    JitCompiler jit(*memory);
    const uint64_t t_compile0 = now_ns();
    CompiledBlock* cb = jit.compile(block);
    result.compile_ns = now_ns() - t_compile0;
    if (!cb || cb->guest_size < block.byte_length) {
        result.out.status = 7;
        return result;
    }

    const auto restore = [&]() {
        load_cpu32(*cpu, in);
        (void)memory->write_bytes(kMemBase, in.mem.data(), in.mem.size());
    };

    const auto one = [&]() -> uint32_t {
        restore();
        JitExecResult r = execute_like_dispatcher(jit, *cb, *cpu);
        cpu->materialize_lazy_flags();
        if (r == JitExecResult::Unsupported || r == JitExecResult::StaleCode)
            return 7;
        const uint64_t code_end = kCodeBase + code.size();
        if (r == JitExecResult::Continue && cpu->rip == code_end) return 0;
        return 5;
    };

    uint32_t status = 0;
    const uint64_t t_warm0 = now_ns();
    for (uint32_t i = 0; i < warmup; ++i) {
        status = one();
        if (status != 0) break;
    }
    result.warmup_ns = now_ns() - t_warm0;

    const uint64_t t_run0 = now_ns();
    for (uint32_t i = 0; i < iters; ++i) {
        status = one();
        if (status != 0) break;
    }
    result.run_ns = now_ns() - t_run0;
    result.iters = iters;
    result.status = status;
    result.out.status = status;
    store_cpu32(in, *cpu, *memory, result.out);
    return result;
#endif
}

JitBenchResult bench_on_jit_64(const State& in,
                               const std::vector<uint8_t>& code,
                               uint32_t warmup, uint32_t iters) {
    JitBenchResult result;
    result.out = in;
    result.out.mode = vitreo::oracle::kMode64;
    result.out.status = 6;
#if !defined(__aarch64__)
    result.out.status = 7;
    return result;
#else
    using namespace vitreo::cpu;
    using vitreo::cpu::jit::BasicBlock;
    using vitreo::cpu::jit::CompiledBlock;
    using vitreo::cpu::jit::JitCompiler;
    using vitreo::cpu::jit::JitExecResult;

    if (code.size() > 0x1000) return result;

    std::unique_ptr<vitreo::GuestMemory> memory;
    std::unique_ptr<CpuState> cpu;
    if (!setup64(in, code, memory, cpu)) return result;

    std::unique_ptr<JitCompiler> jit;
    CompiledBlock* compiled = nullptr;
    uint32_t byte_length = 0;
    if (!code.empty()) {
        BasicBlock block;
        block.start_addr = kCodeBase64;
        block.is_64bit = true;
        MemoryByteReader reader(code.data(), code.size());
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            if (decode_instruction(reader, DecodeMode::Mode64, inst) !=
                DecodeError::Success) {
                result.out.status = 7;
                return result;
            }
            block.instructions.push_back(inst);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count =
            static_cast<u32>(block.instructions.size());
        byte_length = block.byte_length;

        jit = std::make_unique<JitCompiler>(*memory);
        const uint64_t compile_begin = now_ns();
        compiled = jit->compile(block);
        result.compile_ns = now_ns() - compile_begin;
        if (compiled == nullptr || compiled->guest_size < byte_length) {
            result.out.status = 7;
            return result;
        }
    }

    const auto restore = [&]() -> bool {
        load_cpu64(*cpu, in);
        return memory->write_bytes(kMemBase, in.mem.data(), in.mem.size())
            .is_ok();
    };
    const auto one = [&]() -> uint32_t {
        if (!restore()) return 6;
        if (compiled == nullptr) return 0;
        const JitExecResult exec = jit->execute(*compiled, *cpu);
        cpu->materialize_lazy_flags();
        if (exec == JitExecResult::Unsupported) return 7;
        const uint64_t code_end = kCodeBase64 + code.size();
        return exec == JitExecResult::Continue && cpu->rip == code_end ? 0u
                                                                       : 5u;
    };

    uint32_t status = 0;
    const uint64_t warmup_begin = now_ns();
    for (uint32_t i = 0; i < warmup; ++i) {
        status = one();
        if (status != 0) break;
    }
    result.warmup_ns = now_ns() - warmup_begin;

    uint32_t completed = 0;
    if (status == 0) {
        const uint64_t run_begin = now_ns();
        for (uint32_t i = 0; i < iters; ++i) {
            status = one();
            if (status != 0) break;
            ++completed;
        }
        result.run_ns = now_ns() - run_begin;
    }
    result.iters = completed;
    result.status = status;
    cpu->materialize_lazy_flags();
    readback64(in, *cpu, *memory, result.out);
    result.out.status = status;
    result.out.bytes_run =
        static_cast<uint32_t>(cpu->rip - kCodeBase64);
    return result;
#endif
}

JitMultiBlockBenchResult bench_on_jit_64_multiblock(
    const State& in, const std::vector<uint8_t>& code,
    uint32_t warmup, uint32_t iters) {
    JitMultiBlockBenchResult result;
    result.bench.out = in;
    result.bench.out.mode = vitreo::oracle::kMode64;
    result.bench.out.status = 6;
#if !defined(__aarch64__)
    result.bench.status = 7;
    result.bench.out.status = 7;
    return result;
#else
    using namespace vitreo::cpu;
    using vitreo::cpu::jit::BasicBlock;
    using vitreo::cpu::jit::BlockCache;
    using vitreo::cpu::jit::CompiledBlock;
    using vitreo::cpu::jit::JitCompiler;
    using vitreo::cpu::jit::JitExecResult;

    // The adapter owns its stack. A caller-supplied RSP could point outside
    // the reset window and make iterations stateful or corrupt test memory.
    if (code.size() > 0x1000 || in.gpr[4] != 0) return result;

    std::unique_ptr<vitreo::GuestMemory> memory;
    std::unique_ptr<CpuState> cpu;
    if (!setup64(in, code, memory, cpu)) return result;

    constexpr uint64_t kStackBytes = 0x200;
    constexpr uint32_t kMaxDispatches = 4096;
    result.initial_rsp = cpu->gpr.get64(static_cast<u8>(RegIndex::RSP));
    const uint64_t stack_begin = result.initial_rsp - kStackBytes / 2;
    const std::array<uint8_t, kStackBytes> clean_stack{};
    const uint64_t code_end = kCodeBase64 + code.size();

    auto is_terminator = [](Opcode op) {
        return (op >= Opcode::JMP_REL8 && op <= Opcode::JMP_M16_64) ||
               (op >= Opcode::JO_REL8 && op <= Opcode::JNLE_REL32) ||
               (op >= Opcode::LOOP_REL8 && op <= Opcode::JRCXZ_REL8) ||
               (op >= Opcode::CALL_REL16 && op <= Opcode::CALL_M16_64) ||
               (op >= Opcode::RET_NEAR && op <= Opcode::RET_FAR_IMM16);
    };

    struct ScopedJitLink {
        bool was_enabled{JitCompiler::jit_link_enabled()};
        ScopedJitLink() { JitCompiler::enable_jit_link(); }
        ~ScopedJitLink() {
            if (!was_enabled) JitCompiler::disable_jit_link();
        }
    } link_guard;
    BlockCache cache;
    JitCompiler jit(*memory);
    jit.set_block_cache(&cache);
    uint32_t blocks_compiled = 0;
    auto block_at = [&](uint64_t rip) -> BasicBlock* {
        if (BasicBlock* cached = cache.lookup(rip)) return cached;
        if (rip < kCodeBase64 || rip >= code_end) return nullptr;
        const size_t offset = static_cast<size_t>(rip - kCodeBase64);
        BasicBlock block;
        block.start_addr = rip;
        block.is_64bit = true;
        MemoryByteReader reader(code.data() + offset, code.size() - offset);
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            if (decode_instruction(reader, DecodeMode::Mode64, inst) !=
                DecodeError::Success) {
                return nullptr;
            }
            block.instructions.push_back(inst);
            if (is_terminator(inst.opcode)) {
                block.ends_with_branch = true;
                break;
            }
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count = static_cast<u32>(block.instructions.size());
        if (block.byte_length == 0) return nullptr;
        BasicBlock* cached = cache.insert(std::move(block));
        CompiledBlock* compiled = jit.compile(*cached);
        if (compiled == nullptr || compiled->guest_size < cached->byte_length) {
            return nullptr;
        }
        jit.publish_compiled_block(*cached, *compiled);
        ++blocks_compiled;
        return cached;
    };

    const auto restore = [&]() -> bool {
        load_cpu64(*cpu, in);
        return memory->write_bytes(kMemBase, in.mem.data(), in.mem.size()).is_ok() &&
               memory->write_bytes(stack_begin, clean_stack.data(),
                                   clean_stack.size()).is_ok();
    };
    const auto one = [&](uint32_t& dispatches) -> uint32_t {
        dispatches = 0;
        if (!restore()) return 6;
        while (cpu->rip != code_end && dispatches < kMaxDispatches) {
            if (cpu->rip < kCodeBase64 || cpu->rip >= code_end) return 5;
            BasicBlock* block = block_at(cpu->rip);
            if (block == nullptr) return 7;
            CompiledBlock compiled = compiled_from_cache(*block);
            const JitExecResult exec = jit.execute(compiled, *cpu);
            ++dispatches;
            if (exec == JitExecResult::Unsupported ||
                exec == JitExecResult::StaleCode) {
                return 7;
            }
            if (exec != JitExecResult::Continue) return 5;
        }
        return cpu->rip == code_end ? 0u : 5u;
    };
    const auto finish = [&](uint32_t status, uint32_t dispatches) {
        result.bench.status = status;
        result.final_dispatches = dispatches;
        cpu->materialize_lazy_flags();
        readback64(in, *cpu, *memory, result.bench.out);
        result.final_rsp = cpu->gpr.get64(static_cast<u8>(RegIndex::RSP));
        result.stack_delta = static_cast<int64_t>(result.final_rsp) -
                             static_cast<int64_t>(result.initial_rsp);
        result.bench.out.gpr[4] = result.final_rsp;
        result.bench.out.status = status;
        result.bench.out.bytes_run = cpu->rip >= kCodeBase64
            ? static_cast<uint32_t>(cpu->rip - kCodeBase64) : 0;
    };

    // Preflight discovers and compiles every block reached from the fixed
    // input before either timed interval. It also prevents first-iteration
    // compilation from contaminating measurements when warmup is zero.
    uint32_t dispatches = 0;
    const uint64_t compile_begin = now_ns();
    uint32_t status = one(dispatches);
    result.bench.compile_ns = now_ns() - compile_begin;
    if (status != 0) {
        finish(status, dispatches);
        return result;
    }
    result.blocks_compiled = blocks_compiled;

    const uint64_t warmup_begin = now_ns();
    for (uint32_t i = 0; i < warmup; ++i) {
        status = one(dispatches);
        if (status != 0) break;
    }
    result.bench.warmup_ns = now_ns() - warmup_begin;

    uint32_t completed = 0;
    if (status == 0) {
        const uint64_t run_begin = now_ns();
        for (uint32_t i = 0; i < iters; ++i) {
            status = one(dispatches);
            if (status != 0) break;
            ++completed;
        }
        result.bench.run_ns = now_ns() - run_begin;
    }
    result.bench.iters = completed;
    finish(status, dispatches);
    return result;
#endif
}

JitBenchResult bench_on_interpreter(const State& in,
                                    const std::vector<uint8_t>& code,
                                    uint32_t warmup, uint32_t iters) {
    JitBenchResult result;
    result.out = in;
    result.out.status = 6;
    if (code.empty() || code.size() > 0x1000) {
        result.out.status = code.empty() ? 0 : 6;
        return result;
    }

    using namespace vitreo::cpu;
    using vitreo::GuestMemory;
    using vitreo::MemoryProtection;

    auto memory = std::make_unique<GuestMemory>();
    if (!memory->map_region(kCodeBase, kRegionSize, MemoryProtection::ReadWrite)) {
        return result;
    }
    if (!memory->write_bytes(kCodeBase, code.data(), code.size())) return result;
    if (!memory->map_region(kMemBase, kMemSize, MemoryProtection::ReadWrite)) {
        return result;
    }
    if (!memory->write_bytes(kMemBase, in.mem.data(), in.mem.size())) return result;

    auto cpu = std::make_unique<CpuState>();
    Interpreter interp(*cpu, *memory);
    const uint64_t code_end = kCodeBase + code.size();

    const auto one = [&]() -> uint32_t {
        load_cpu32(*cpu, in);
        (void)memory->write_bytes(kMemBase, in.mem.data(), in.mem.size());
        uint32_t steps = 0;
        while (cpu->rip < code_end && steps < kMaxSteps) {
            ExecResult r = interp.step();
            ++steps;
            if (r != ExecResult::Continue) return 5;
        }
        return cpu->rip == code_end ? 0 : 5;
    };

    uint32_t status = 0;
    const uint64_t t_warm0 = now_ns();
    for (uint32_t i = 0; i < warmup; ++i) {
        status = one();
        if (status != 0) break;
    }
    result.warmup_ns = now_ns() - t_warm0;
    const uint64_t t_run0 = now_ns();
    for (uint32_t i = 0; i < iters; ++i) {
        status = one();
        if (status != 0) break;
    }
    result.run_ns = now_ns() - t_run0;
    result.iters = iters;
    result.status = status;
    result.out.status = status;
    cpu->materialize_lazy_flags();
    store_cpu32(in, *cpu, *memory, result.out);
    return result;
}

}  // namespace vitreo::oracle::contract
