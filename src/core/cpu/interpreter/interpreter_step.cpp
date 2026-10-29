// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Interpreter - fetch/decode step loop, JIT-delegate entry, fault diagnostics

#include "interpreter.hpp"
#include <execinfo.h>
#include <vitreo/guest_sentinels.hpp>
#include "../lock_prefix.hpp"
#include "core/memory/mem_watch.hpp"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <string>

namespace vitreo::cpu {

namespace {
std::atomic<PcTraceDumpHook> g_pctrace_dump_hook{nullptr};
std::atomic<ThunkNameHook>   g_thunk_name_hook{nullptr};
}

void set_pctrace_dump_hook(PcTraceDumpHook hook) noexcept {
    g_pctrace_dump_hook.store(hook, std::memory_order_relaxed);
}

void set_thunk_name_hook(ThunkNameHook hook) noexcept {
    g_thunk_name_hook.store(hook, std::memory_order_relaxed);
}

namespace {
/// Render " (kernel32.dll!Foo)" for a thunk address, or "" when not a thunk.
std::string thunk_note(u64 addr) {
    auto hook = g_thunk_name_hook.load(std::memory_order_relaxed);
    if (hook == nullptr) return {};
    char buf[192];
    if (!hook(addr, buf, static_cast<unsigned>(sizeof buf))) return {};
    return std::string(" (") + buf + ")";
}
}

namespace {

// VITREO_TRACE_UD=1: log the PRECISE rip + bytes + decoded opcode whenever an
// executed instruction raises InvalidOpcode. Covers both the pure-interpreter
// step() and the JIT delegate (execute_predecoded) paths — the run loop's
// FATAL dump shows the JIT block-entry rip, which hides the true faulting
// instruction. `src` names which path raised it. Cheap: one env check + a
// branch only taken on an actual #UD.
inline void trace_invalid_opcode(const CpuState& cpu, GuestMemory& mem,
                                 const DecodedInstruction& instr,
                                 const char* src) noexcept {
    static const bool on = (std::getenv("VITREO_TRACE_UD") != nullptr);
    if (!on) return;
    u8 b[16] = {};
    for (int i = 0; i < 16; ++i)
        b[i] = mem.read_u8(static_cast<GuestAddr>(cpu.rip + i)).value_or(0xCC);
    const char* nm = opcode_name(instr.opcode);
    std::fprintf(stderr,
        "[interp-ud %s] rip=0x%llx opcode=%d %s len=%u bytes="
        "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
        src, static_cast<unsigned long long>(cpu.rip),
        static_cast<int>(instr.opcode), nm ? nm : "?",
        static_cast<unsigned>(instr.length),
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9]);

    // Landing on address 0 is a CONTROL-FLOW failure, not a decode failure:
    // the padding there merely happens to be undecodable. What matters is who
    // sent us here, and a CALL leaves that on the stack. Dump the registers
    // and the top of stack so the guest call site can be named -- without
    // this the report says only "rip=0", which identifies nothing.
    if (cpu.rip == 0) {
        std::fprintf(stderr,
            "[interp-ud] NULL RIP. rsp=0x%llx rax=0x%llx rbx=0x%llx "
            "rcx=0x%llx rdx=0x%llx rsi=0x%llx rdi=0x%llx rbp=0x%llx\n",
            static_cast<unsigned long long>(cpu.gpr.rsp),
            static_cast<unsigned long long>(cpu.gpr.rax),
            static_cast<unsigned long long>(cpu.gpr.rbx),
            static_cast<unsigned long long>(cpu.gpr.rcx),
            static_cast<unsigned long long>(cpu.gpr.rdx),
            static_cast<unsigned long long>(cpu.gpr.rsi),
            static_cast<unsigned long long>(cpu.gpr.rdi),
            static_cast<unsigned long long>(cpu.gpr.rbp));
        for (int i = 0; i < 8; ++i) {
            const auto w = mem.read_u64(
                static_cast<GuestAddr>(cpu.gpr.rsp) +
                static_cast<GuestAddr>(i) * 8);
            std::fprintf(stderr, "[interp-ud]   [rsp+%2d] = 0x%016llx\n",
                         i * 8,
                         static_cast<unsigned long long>(
                             w.is_ok() ? w.value() : 0));
        }
        std::fflush(stderr);
    }
}

} // namespace

namespace {

/// Memory reader for instruction fetching — bulk-fetches into a local buffer
/// to avoid per-byte find_region lookups.
class GuestMemoryReader final : public ByteReader {
public:
    GuestMemoryReader(GuestMemory& memory, u64 address) noexcept
        : m_memory(memory), m_start(address), m_pos(0), m_len(0) {
        // Prefetch up to 15 bytes (max x86 instruction length)
        const Byte* p = memory.fetch_ptr(static_cast<GuestAddr>(address), 15);
        if (p) {
            std::memcpy(m_buf, p, 15);
            m_len = 15;
        }
    }
    
    [[nodiscard]] bool read_u8(u8& out) noexcept override {
        if (m_pos < m_len) {
            out = m_buf[m_pos++];
            return true;
        }
        // Fallback for instructions spanning region boundaries
        auto result = m_memory.fetch_u8(m_start + m_pos);
        if (!result) return false;
        out = result.value();
        ++m_pos;
        return true;
    }
    
    [[nodiscard]] usize position() const noexcept override {
        return m_pos;
    }
    
private:
    GuestMemory& m_memory;
    u64 m_start;
    usize m_pos;
    usize m_len;
    u8 m_buf[15];
};

} // anonymous namespace

// ============================================================================
// Interpreter methods
// ============================================================================

// Last instruction pointer that decoded successfully (diagnostic: identifies the
// source of a branch into unmapped memory). Updated per step.
thread_local u64 g_last_exec_ip = 0;

[[nodiscard]] bool instruction_owns_lock_serialization(
    const DecodedInstruction& instr) noexcept {
    switch (instr.opcode) {
    case Opcode::OR_RM8_R8:
    case Opcode::OR_RM8_IMM8:
        return instr.operand_count >= 2 &&
               is_memory_operand(instr.operands[0].type);
    case Opcode::CMPXCHG_RM8_R8:
    case Opcode::CMPXCHG_RM16_R16:
    case Opcode::CMPXCHG_RM32_R32:
    case Opcode::CMPXCHG_RM64_R64:
        return true;
    default:
        return false;
    }
}

ExecResult Interpreter::execute_predecoded(
    const DecodedInstruction& instr) noexcept {
    // The JIT delegate has already set m_cpu.rip to this instruction's address.
    // This is exactly step()'s tail (lines after a successful decode): set the
    // context length, record the last-exec ip, run the op (LOCK-aware), advance
    // rip on Continue, and bump the counter. No fetch/decode — that is the win.
    u64 ip = m_cpu.rip;
    m_ctx.instr_length = instr.length;
    extern thread_local u64 g_last_exec_ip;
    g_last_exec_ip = ip;
    ExecResult result;
    if (instr.prefixes.lock && !instruction_owns_lock_serialization(instr)) {
        LockPrefixGuard guard;
        result = execute_instruction(m_ctx, instr);
    } else {
        result = execute_instruction(m_ctx, instr);
    }
    if (result == ExecResult::Exception &&
        m_ctx.exception == CpuException::InvalidOpcode) {
        trace_invalid_opcode(m_cpu, m_memory, instr, "jit-delegate");
    }
    if (result == ExecResult::Continue) {
        m_cpu.advance_ip(instr.length);
    }
    ++m_instr_count;
    return result;
}

ExecResult Interpreter::step() noexcept {
    u64 ip = m_cpu.rip;

    // VITREO_CODE_WATCH=<addr>[,<addr>...]: log every execution of a watched
    // guest instruction with the register state at that point. mem_watch.hpp
    // has declared this set since it was written, but nothing ever consulted
    // it -- the env var silently did nothing, which is worse than not existing,
    // because an empty log reads as "the guest never went there".
    //
    // No address or field layout is baked in, so it can be aimed at any
    // function under
    // VITREO_NO_JIT=1 (where the interpreter sees every instruction) without
    // editing code.
    if (::vitreo::code_watch_enabled() && ::vitreo::code_watch_hit(ip)) {
        static std::atomic<u64> s_code_watch_hits{0};
        const u64 n = s_code_watch_hits.fetch_add(1, std::memory_order_relaxed);
        // Bound the output: a watched address inside a hot loop would other-
        // wise bury the log. Early hits are what name a first-bad-value bug.
        if (n < 256 || (n & (n - 1)) == 0) {
            const u64 ret =
                m_memory.read_u64(static_cast<GuestAddr>(m_cpu.gpr.rsp))
                    .value_or(0);
            std::fprintf(stderr,
                "[code-watch #%llu] rip=0x%llx ret=0x%llx rcx=0x%llx "
                "rdx=0x%llx r8=0x%llx r9=0x%llx rax=0x%llx rbx=0x%llx "
                "rbp=0x%llx rsi=0x%llx rdi=0x%llx rsp=0x%llx "
                "r10=0x%llx r11=0x%llx r12=0x%llx r13=0x%llx r14=0x%llx "
                "r15=0x%llx\n",
                (unsigned long long)n,
                (unsigned long long)ip, (unsigned long long)ret,
                (unsigned long long)m_cpu.gpr.rcx,
                (unsigned long long)m_cpu.gpr.rdx,
                (unsigned long long)m_cpu.gpr.r8,
                (unsigned long long)m_cpu.gpr.r9,
                (unsigned long long)m_cpu.gpr.rax,
                (unsigned long long)m_cpu.gpr.rbx,
                (unsigned long long)m_cpu.gpr.rbp,
                (unsigned long long)m_cpu.gpr.rsi,
                (unsigned long long)m_cpu.gpr.rdi,
                (unsigned long long)m_cpu.gpr.rsp,
                (unsigned long long)m_cpu.gpr.r10,
                (unsigned long long)m_cpu.gpr.r11,
                (unsigned long long)m_cpu.gpr.r12,
                (unsigned long long)m_cpu.gpr.r13,
                (unsigned long long)m_cpu.gpr.r14,
                (unsigned long long)m_cpu.gpr.r15);
        }
    }

    GuestMemoryReader reader(m_memory, ip);
    
    DecodeMode mode = m_cpu.is_64bit() ? DecodeMode::Mode64 : DecodeMode::Mode32;
    
    DecodedInstruction instr;
    DecodeError err = decode_instruction(reader, mode, instr);
    
    if (err != DecodeError::Success) {
        // Debug: dump the first bytes at the failing IP
        u8 raw[8] = {};
        for (int i = 0; i < 8; ++i) {
            auto r = m_memory.read_u8(static_cast<GuestAddr>(ip + i));
            raw[i] = r.is_ok() ? r.value() : 0xCC;
        }
        // A branch into unmapped memory is fatal-class: the guest's SEH usually
        // turns it into a bare ExitProcess, leaving no record of where the bad
        // branch came from. Report it by default instead of only under
        // VITREO_TRACE_UD, capped so a guest that faults in a loop cannot flood
        // the log. last_jit_block_pc() is what names the culprit when the branch
        // was executed by compiled code; g_last_exec_ip is only meaningful when
        // the interpreter itself ran the branch.
        if (ip < 0x10000u ||
            !m_memory.is_mapped(static_cast<GuestAddr>(ip), 1)) {
            // DIAGNOSTIC: a fetch at a host return sentinel means a loop
            // asked the interpreter to run guest code where the host should
            // have taken control back. Name the host path that did it.
            if (ip == kCallbackReturnSentinel || ip == kProcessReturnSentinel ||
                ip == (kCallbackReturnSentinel & 0xFFFFFFFFull)) {
                static std::atomic<int> s_sentinel_reports{0};
                if (s_sentinel_reports.fetch_add(1, std::memory_order_relaxed) < 4) {
                    std::fprintf(stderr,
                        "[cpu] FETCH AT RETURN SENTINEL 0x%llx rsp=0x%llx -- host backtrace:\n",
                        static_cast<unsigned long long>(ip),
                        static_cast<unsigned long long>(m_cpu.sp()));
                    void* frames[48];
                    const int n = ::backtrace(frames, 48);
                    ::backtrace_symbols_fd(frames, n, 2);
                    std::fflush(stderr);
                }
            }
            static std::atomic<int> s_unmapped_reports{0};
            if (s_unmapped_reports.fetch_add(1, std::memory_order_relaxed) < 8) {
                extern thread_local u64 g_last_exec_ip;
                const u32 esp = static_cast<u32>(m_cpu.sp());
                std::fprintf(
                    stderr,
                    "[cpu] branch to unmapped target=0x%llx jit-block=0x%llx "
                    "last-interp=0x%llx rax=0x%llx rcx=0x%llx rdx=0x%llx "
                    "rbx=0x%llx rsp=0x%x [esp]=0x%x\n",
                    static_cast<unsigned long long>(ip),
                    static_cast<unsigned long long>(last_jit_block_pc()),
                    static_cast<unsigned long long>(g_last_exec_ip),
                    static_cast<unsigned long long>(m_cpu.gpr.rax),
                    static_cast<unsigned long long>(m_cpu.gpr.rcx),
                    static_cast<unsigned long long>(m_cpu.gpr.rdx),
                    static_cast<unsigned long long>(m_cpu.gpr.rbx),
                    esp,
                    m_memory.read_u32(static_cast<GuestAddr>(esp)).value_or(0));
                std::fflush(stderr);
            }
        }

        {
            static const bool ud_on = (std::getenv("VITREO_TRACE_UD") != nullptr);
            if (ud_on) {
                u8 b[16] = {};
                for (int i = 0; i < 16; ++i)
                    b[i] = m_memory.read_u8(static_cast<GuestAddr>(ip + i)).value_or(0xCC);
                std::fprintf(stderr,
                    "[interp-ud decode-fail] rip=0x%llx err=%d bytes="
                    "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                    static_cast<unsigned long long>(ip), static_cast<int>(err),
                    b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                    b[8], b[9], b[10], b[11]);
                // Always dump the prior interpreted IP for unmapped / nonsense
                // targets (poison patterns like 0x37373737, NULL-ish jumps).
                // Previously this only fired for ip < 0x10000, which hid the
                // common "call/jmp through corrupted pointer" case.
                const bool unmapped_target = !m_memory.is_mapped(
                    static_cast<GuestAddr>(ip), 1);
                if (ip < 0x10000u || unmapped_target) {
                    extern thread_local u64 g_last_exec_ip;
                    u8 source[16] = {};
                    for (int i = 0; i < 16; ++i) {
                        source[i] = m_memory.read_u8(
                            static_cast<GuestAddr>(g_last_exec_ip + i))
                            .value_or(0xCC);
                    }
                    const u32 esp = static_cast<u32>(m_cpu.sp());
                    std::fprintf(
                        stderr,
                        "[interp-ud last-interpreted] ip=0x%llx bytes="
                        "%02x %02x %02x %02x %02x %02x %02x %02x "
                        "%02x %02x %02x %02x %02x %02x %02x %02x "
                        "rax=0x%llx rcx=0x%llx rdx=0x%llx rbx=0x%llx "
                        "rsp=0x%x [esp]=0x%x [esp+4]=0x%x [esp+8]=0x%x\n",
                        static_cast<unsigned long long>(g_last_exec_ip),
                        source[0], source[1], source[2], source[3],
                        source[4], source[5], source[6], source[7],
                        source[8], source[9], source[10], source[11],
                        source[12], source[13], source[14], source[15],
                        static_cast<unsigned long long>(m_cpu.gpr.rax),
                        static_cast<unsigned long long>(m_cpu.gpr.rcx),
                        static_cast<unsigned long long>(m_cpu.gpr.rdx),
                        static_cast<unsigned long long>(m_cpu.gpr.rbx),
                        esp,
                        m_memory.read_u32(static_cast<GuestAddr>(esp)).value_or(0),
                        m_memory.read_u32(static_cast<GuestAddr>(esp + 4)).value_or(0),
                        m_memory.read_u32(static_cast<GuestAddr>(esp + 8)).value_or(0));
                    std::fflush(stderr);
                    // If a PCTRACE ring is active, dump it so the clobber
                    // site is visible even when guest SEH "handles" the UD.
                    if (auto hook = g_pctrace_dump_hook.load(std::memory_order_relaxed)) {
                        hook("unmapped-invalid-opcode");
                    }
                }
            }
        }
        {
            // Print the FULL address, not the low 32 bits: on x64 a fault in
            // the thunk region reads as a small number once truncated, which
            // hides both the region it is in and which export it belongs to.
            const std::string note = thunk_note(static_cast<u64>(ip));
            VITREO_TRACE("[interp] InvalidOpcode at RIP=0x%llx%s bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                         static_cast<unsigned long long>(ip), note.c_str(),
                         raw[0], raw[1], raw[2], raw[3],
                         raw[4], raw[5], raw[6], raw[7]);
        }
        // Dump the prior IP and stack so the call/jmp site is visible. An
        // undecodable byte at a *mapped* address is the same defect as one at
        // an unmapped address -- control reached a spot that is not an
        // instruction boundary -- and naming the branch source is the whole
        // diagnosis. (Under the JIT the previous IP belongs to the last
        // interpreted instruction, so treat it as a hint, not proof.)
        {
            u32 esp = static_cast<u32>(m_cpu.sp());
            extern thread_local u64 g_last_exec_ip;
            const std::string from_note = thunk_note(g_last_exec_ip);
            VITREO_TRACE("[interp]   bad-branch from=0x%llx%s esp=0x%08x [esp]=0x%08x [esp+4]=0x%08x [esp+8]=0x%08x\n",
                         static_cast<unsigned long long>(g_last_exec_ip),
                         from_note.c_str(), esp,
                         m_memory.read_u32(static_cast<GuestAddr>(esp)).value_or(0),
                         m_memory.read_u32(static_cast<GuestAddr>(esp + 4)).value_or(0),
                         m_memory.read_u32(static_cast<GuestAddr>(esp + 8)).value_or(0));
        }
        m_ctx.exception = CpuException::InvalidOpcode;
        return ExecResult::Exception;
    }
    
    m_ctx.instr_length = instr.length;
    extern thread_local u64 g_last_exec_ip;
    g_last_exec_ip = ip;
    ExecResult result;
    if (instr.prefixes.lock) {
        LockPrefixGuard guard;
        result = execute_instruction(m_ctx, instr);
    } else {
        result = execute_instruction(m_ctx, instr);
    }

    if (result == ExecResult::Exception &&
        m_ctx.exception == CpuException::InvalidOpcode) {
        trace_invalid_opcode(m_cpu, m_memory, instr, "interp");
    }

    if (result == ExecResult::Continue) {
        m_cpu.advance_ip(instr.length);
    }

    ++m_instr_count;
    return result;
}

ExecResult Interpreter::run(u64 max_instructions) noexcept {
    ExecResult result = ExecResult::Continue;
    u64 count = 0;
    
    while (result == ExecResult::Continue || result == ExecResult::Branch) {
        result = step();
        ++count;
        
        if (max_instructions > 0 && count >= max_instructions) {
            break;
        }
    }
    
    return result;
}

} // namespace vitreo::cpu
