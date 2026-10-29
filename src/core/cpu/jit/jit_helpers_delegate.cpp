// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_helpers_delegate.cpp
/// @brief Per-instruction interpreter delegate called from JIT code.
///
/// jit_interpret_decoded and jit_interpret_step run one instruction
/// through the interpreter for an opcode the JIT has no native lowering
/// for, then surface its exception or INT3/INT29 as a pending_raise.
///
/// Split out of jit_helpers_lock.cpp.

#include "jit_helpers.hpp"
#include "flag_liveness.hpp"
#include "jit_coverage.hpp"
#include "../interpreter/interpreter.hpp"
#include "../interpreter/ops/exec_ops.hpp"
#include "../seh/seh32.hpp"

#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vitreo::cpu::jit {

// Shared post-execution result handling for both delegate entry points
// (jit_interpret_step and jit_interpret_decoded). Surfaces guest-visible
// exceptions and INT3/INT29 as a pending_raise so the run loop can dispatch
// guest SEH exactly as if the interpreter main loop had run the instruction.
static void jit_delegate_apply_result(
    CpuState* cpu, ::vitreo::cpu::ExecResult step_result,
    const ::vitreo::cpu::ExecContext& ctx) {
    using ::vitreo::cpu::ExecResult;
    if (step_result == ExecResult::Exception && !cpu->pending_raise.active) {
        // Env-gated diagnostic: a delegated op raised an exception (commonly an
        // op-handler `default:` = unsupported opcode variant). The main-loop
        // decode-fail logger never covers this path, so surface the precise
        // rip + decoded opcode here to pinpoint unimplemented instructions.
        static const bool s_trace_delegate_exc =
            (std::getenv("VITREO_TRACE_DELEGATE_EXC") != nullptr);
        // Every delegated exception needs this attribution, not only #UD: a
        // memory or stack fault raised here is reported to the run loop with
        // the block's RIP, so the guest RIP in the fatal report can name an
        // instruction that did not fault.
        if (s_trace_delegate_exc) {
            u8 raw[16] = {};
            for (int i = 0; i < 16; ++i) {
                auto r = ctx.mem.fetch_u8(static_cast<GuestAddr>(cpu->rip + i));
                raw[i] = r.is_ok() ? r.value() : 0xCC;
            }
            struct MemRd final : ::vitreo::cpu::ByteReader {
                const u8* b; usize pos = 0;
                bool read_u8(u8& o) noexcept override { o = b[pos++]; return pos <= 16; }
                usize position() const noexcept override { return pos; }
            } mr; mr.b = raw;
            ::vitreo::cpu::DecodedInstruction di{};
            auto mode = cpu->is_64bit() ? ::vitreo::cpu::DecodeMode::Mode64
                                        : ::vitreo::cpu::DecodeMode::Mode32;
            (void)::vitreo::cpu::decode_instruction(mr, mode, di);
            const char* nm = opcode_name(di.opcode);
            std::fprintf(stderr,
                "[delegate-exc] vector=%u addr=0x%llx rip=0x%llx opcode=%d %s"
                " bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                (unsigned)ctx.exception,
                (unsigned long long)ctx.exception_addr,
                (unsigned long long)cpu->rip, (int)di.opcode, nm ? nm : "?",
                raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);

            // Assertion and delay-load failure paths frequently keep their
            // diagnostic string in a nonvolatile register. Include only
            // bounded, fully printable, null-terminated ANSI values so an
            // otherwise opaque UD2 still identifies the failed contract.
            const auto trace_ansi_register =
                [&](const char* register_name, u64 address) {
                    if (address == 0) return;
                    char value[257] = {};
                    usize length = 0;
                    for (; length < 256; ++length) {
                        auto byte = ctx.mem.read_u8(
                            static_cast<GuestAddr>(address + length));
                        if (byte.is_err()) return;
                        if (byte.value() == 0) break;
                        if (byte.value() < 0x20 || byte.value() > 0x7e) return;
                        value[length] = static_cast<char>(byte.value());
                    }
                    if (length < 3 || length == 256) return;
                    std::fprintf(
                        stderr,
                        "[delegate-exc]   %s=0x%llx ansi=\"%s\"\n",
                        register_name,
                        static_cast<unsigned long long>(address), value);
                };
            trace_ansi_register("rbx", cpu->gpr.rbx);
            trace_ansi_register("rsi", cpu->gpr.rsi);
            trace_ansi_register("rdi", cpu->gpr.rdi);
            trace_ansi_register("rbp", cpu->gpr.rbp);
            trace_ansi_register("rsp", cpu->gpr.rsp);
        }
        auto& pending = cpu->pending_raise;
        pending.active = true;
        pending.from_helper = true;
        pending.code = cpu_exception_to_status(ctx.exception);
        pending.flags = 0;
        pending.num_args = pending.code == STATUS_ACCESS_VIOLATION ? 2u : ctx.exception_num_params;
        if (pending.num_args == 2) {
            pending.args[0] = ctx.exception_error;
            pending.args[1] = ctx.exception_addr;
        }
    }
    // INT3 executed via the delegate: surfaced as STATUS_BREAKPOINT so the run
    // loop dispatches guest SEH and, if unhandled, terminates like Windows.
    // (Without this a guest CHECK via int3 re-entered the same RIP forever.)
    if (step_result == ExecResult::Interrupt && !cpu->pending_raise.active) {
        const u8 interrupt_num = ctx.interrupt_num;
        if (interrupt_num == 3 || interrupt_num == 0x29) {
            cpu->pending_raise.active = true;
            cpu->pending_raise.code = interrupt_num == 3
                ? 0x80000003u  // STATUS_BREAKPOINT
                : 0xC0000409u; // STATUS_STACK_BUFFER_OVERRUN
            cpu->pending_raise.flags = interrupt_num == 0x29 ? 1u : 0u;
            cpu->pending_raise.num_args = interrupt_num == 0x29 ? 1u : 0u;
            if (interrupt_num == 0x29) {
                cpu->pending_raise.args[0] = cpu->gpr.rcx;
            }
            cpu->pending_raise.from_helper = true;
        }
    }
}

namespace {

void record_jit_delegate_fallback(Opcode opcode, u64 instr_rip) {
    static const bool enabled = vitreo::detail::g_fallback_hist;
    if (!enabled) return;

    static const uint64_t dump_interval = [] {
        constexpr uint64_t kDefaultInterval = 5'000'000;
        const char* value = std::getenv("VITREO_JIT_FALLBACK_HIST_INTERVAL");
        if (!value || *value == '\0') return kDefaultInterval;
        char* end = nullptr;
        const auto parsed = std::strtoull(value, &end, 10);
        return end != value && *end == '\0' && parsed != 0
            ? static_cast<uint64_t>(parsed)
            : kDefaultInterval;
    }();

    static std::mutex mutex;
    static std::array<uint64_t, 2048> opcode_histogram{};
    static std::unordered_map<uint64_t, uint64_t> rip_histogram;
    static uint64_t total = 0;
    static uint64_t next_dump = dump_interval;

    std::lock_guard<std::mutex> lock(mutex);
    const auto opcode_index = static_cast<size_t>(opcode);
    if (opcode_index < opcode_histogram.size()) {
        ++opcode_histogram[opcode_index];
    }
    ++rip_histogram[instr_rip];
    ++total;
    if (total < next_dump) return;
    next_dump += dump_interval;

    std::vector<std::pair<size_t, uint64_t>> top_opcodes;
    for (size_t i = 0; i < opcode_histogram.size(); ++i) {
        if (opcode_histogram[i] != 0) {
            top_opcodes.emplace_back(i, opcode_histogram[i]);
        }
    }
    std::sort(top_opcodes.begin(), top_opcodes.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(stderr, "[jit-fallback-hist] total=%llu top opcodes:\n",
                 static_cast<unsigned long long>(total));
    for (size_t i = 0; i < std::min<size_t>(30, top_opcodes.size()); ++i) {
        const char* name = opcode_name(static_cast<Opcode>(top_opcodes[i].first));
        std::fprintf(stderr, "  op=%-5zu %-22s %llu (%.1f%%)\n",
                     top_opcodes[i].first, name ? name : "?",
                     static_cast<unsigned long long>(top_opcodes[i].second),
                     100.0 * static_cast<double>(top_opcodes[i].second) /
                         static_cast<double>(total));
    }

    std::vector<std::pair<uint64_t, uint64_t>> top_rips(
        rip_histogram.begin(), rip_histogram.end());
    std::sort(top_rips.begin(), top_rips.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(stderr, "[jit-fallback-hist] top RIPs:\n");
    for (size_t i = 0; i < std::min<size_t>(10, top_rips.size()); ++i) {
        std::fprintf(stderr, "  0x%llx  %llu\n",
                     static_cast<unsigned long long>(top_rips[i].first),
                     static_cast<unsigned long long>(top_rips[i].second));
    }
}

}  // namespace

// Fast fallback stencil variant: execute one *already-decoded* instruction.
// The JIT bakes a stable pointer to the block's own DecodedInstruction (owned
// by the block cache and freed together with the compiled code) plus the
// instruction rip, so this path skips the fetch+decode that jit_interpret_step
// repeats on every execution — the dominant cost in the x64 extraction sample
// profile. Semantics are otherwise identical to jit_interpret_step.
extern "C" void jit_interpret_decoded(CpuState* cpu, GuestMemory* mem,
                                       const ::vitreo::cpu::DecodedInstruction* instr,
                                       u64 instr_rip) {
    // Computing RFLAGS for an instruction that redefines every flag and reads
    // none is pure waste -- and it is the common case, because a delegated
    // ALU op is exactly that shape. Drop the pending lazy op instead.
    static const bool no_discard =
        (std::getenv("VITREO_JIT_NO_FLAG_DISCARD") != nullptr);
    const bool pending_flags = cpu->flag_op != CpuState::FlagOp::None ||
                               cpu->cf_overlay != 0 || cpu->of_overlay != 0;
    // Consecutive delegated SIMD instructions leave RFLAGS authoritative.
    // Do not repeat opcode-name/flag-liveness classification in that case.
    if (!no_discard && pending_flags &&
        ::vitreo::cpu::jit::pending_flags_dead_across(*instr)) {
        cpu->discard_lazy_flags();
    } else {
        cpu->materialize_lazy_flags();
    }
    cpu->rip = instr_rip;
    coverage::record_delegate_exec(instr->opcode, cpu->is_64bit());
    diagnostics::perf_add_fallback_insns(1);
    record_jit_delegate_fallback(instr->opcode, instr_rip);
    ::vitreo::cpu::Interpreter interp(*cpu, *mem);
    auto step_result = interp.execute_predecoded(*instr);
    jit_delegate_apply_result(cpu, step_result, interp.context());
    // Interpreter writes flags directly; flag_op is already None.
}

// Generic fallback stencil: decode & execute one instruction via the
// interpreter. The JIT calls this for any opcode without a native stencil
// so that JIT compilation never gives up mid-block. On return, cpu->rip is
// advanced to the next instruction (for Continue results) and flag_op is
// guaranteed to be FlagOp::None (interpreter writes flags architecturally),
// so subsequent JIT code in the same block can read state directly.
extern "C" void jit_interpret_step(CpuState* cpu, GuestMemory* mem,
                                    u64 instr_rip) {
    // Ensure any pending lazy flags from prior JIT ops are committed to
    // eflags before the interpreter reads them.
    cpu->materialize_lazy_flags();
    cpu->rip = instr_rip;
    diagnostics::perf_add_fallback_insns(1);
    ::vitreo::cpu::Interpreter interp(*cpu, *mem);

    if (vitreo::detail::g_fallback_hist) {
        // The legacy delegate has no pre-decoded instruction, so decode only
        // for the optional diagnostic histogram.
        struct MemRd final : ::vitreo::cpu::ByteReader {
            GuestMemory* m; u64 addr; usize pos = 0;
            bool read_u8(u8& o) noexcept override {
                auto r = m->fetch_u8(static_cast<GuestAddr>(addr + pos));
                if (r.is_err()) return false;
                o = r.value(); ++pos; return true;
            }
            usize position() const noexcept override { return pos; }
        };
        MemRd rd; rd.m = mem; rd.addr = instr_rip;
        ::vitreo::cpu::DecodedInstruction di{};
        auto mode = cpu->is_64bit()
                  ? ::vitreo::cpu::DecodeMode::Mode64
                  : ::vitreo::cpu::DecodeMode::Mode32;
        auto err = ::vitreo::cpu::decode_instruction(rd, mode, di);
        Opcode op = (err == ::vitreo::cpu::DecodeError::Success)
                  ? di.opcode : Opcode::INVALID;
        record_jit_delegate_fallback(op, instr_rip);
    }

    auto step_result = interp.step();
    jit_delegate_apply_result(cpu, step_result, interp.context());
    // Interpreter writes flags directly; flag_op is already None.
}

} // namespace vitreo::cpu::jit
