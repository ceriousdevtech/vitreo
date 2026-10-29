// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_helpers.cpp
/// @brief Integer, flag and CMPXCHG helpers called from JIT code via BLR.
///
/// What is left here: flag materialisation, the 64-bit MUL/DIV/IDIV/ADC/SBB
/// helpers, the per-condition-code helpers, CMPXCHG/CMPXCHG8B, SHLD32/SHRD32
/// and the per-thread GUI pump. The rest of the extern "C" helpers moved to
/// their own files:
///   - jit_helpers_memory.cpp: checked guest memory read/write wrappers;
///   - jit_helpers_memory_fault.cpp: STATUS_ACCESS_VIOLATION from a failed
///     checked access, and the deferred [jit-av] dump;
///   - jit_helpers_x87.cpp, jit_helpers_x87_op.cpp: x87 state helpers and
///     the jit_fpu_op dispatcher;
///   - jit_helpers_sse_cvt.cpp, jit_helpers_sse.cpp: MXCSR, SSE conversions
///     and the jit_sse_op dispatcher;
///   - jit_helpers_lock.cpp (r/m32), jit_helpers_lock_narrow.cpp,
///     jit_helpers_lock_cmpxchg.cpp, jit_helpers_lock64.cpp,
///     jit_helpers_lock_carry.cpp, jit_helpers_lock_bt.cpp,
///     jit_helpers_string.cpp: LOCK-prefixed and REP string-op helpers;
///   - jit_helpers_fused_memcpy.cpp: the fused memcpy idiom's runtime;
///   - jit_helpers_delegate.cpp: the per-instruction interpreter delegate.

#include "jit_helpers.hpp"

#include <chrono>

namespace vitreo::cpu::jit {

namespace {
thread_local void (*t_gui_pump)() = nullptr;
thread_local u32 t_gui_pump_counter = 0;
} // namespace

void jit_install_gui_pump(void (*fn)()) noexcept {
    t_gui_pump = fn;
}

void jit_poll_gui_pump_throttled() noexcept {
    if (t_gui_pump == nullptr) return;
    // Amortize the clock: every 4096 helper calls, then the same 16 ms gate
    // as JitCompiler::maybe_pump_gui. A 64-call pump (old 0x3F mask) made
    // x87 helpers dispatcher-bound.
    if ((++t_gui_pump_counter & 0xFFFu) != 0) return;
    thread_local auto last = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (last.time_since_epoch().count() != 0 &&
        now - last < std::chrono::milliseconds(16)) {
        return;
    }
    last = now;
    t_gui_pump();
}

extern "C" void jit_materialize_flags(CpuState* cpu) {
    cpu->materialize_lazy_flags();
}

extern "C" void jit_mul_rm64(CpuState* cpu, u64 src) {
    // Intel SDM Vol. 2A, MUL r/m64: unsigned RDX:RAX := RAX * SRC; CF and OF
    // are set iff the high half is nonzero. Other status flags are undefined
    // — leave ZF/SF/PF/AF unchanged (same as the interpreter).
    using u128 = unsigned __int128;
    const u128 result =
        static_cast<u128>(cpu->gpr.get64(static_cast<u8>(RegIndex::RAX))) *
        static_cast<u128>(src);
    cpu->gpr.set64(static_cast<u8>(RegIndex::RAX), static_cast<u64>(result));
    cpu->gpr.set64(static_cast<u8>(RegIndex::RDX),
                   static_cast<u64>(result >> 64));
    cpu->set_flag_op(CpuState::FlagOp::None);
    const bool upper = static_cast<u64>(result >> 64) != 0;
    cpu->flags.set_cf(upper);
    cpu->flags.set_of(upper);
}

extern "C" int jit_div_rm64(CpuState* cpu, u64 src) {
    // Intel SDM Vol. 2A, DIV r/m64: unsigned RDX:RAX / SRC → RAX, rem RDX.
    // Zero divisor or quotient > 64 bits → #DE without committing.
    if (src == 0) return 1;
    using u128 = unsigned __int128;
    const u128 dividend =
        (static_cast<u128>(cpu->gpr.get64(static_cast<u8>(RegIndex::RDX)))
         << 64) |
        cpu->gpr.get64(static_cast<u8>(RegIndex::RAX));
    const u128 quotient = dividend / src;
    if (quotient > static_cast<u128>(0xFFFFFFFFFFFFFFFFULL)) return 1;
    cpu->gpr.set64(static_cast<u8>(RegIndex::RAX),
                   static_cast<u64>(quotient));
    cpu->gpr.set64(static_cast<u8>(RegIndex::RDX),
                   static_cast<u64>(dividend % src));
    return 0;
}

extern "C" int jit_idiv_rm64(CpuState* cpu, u64 src) {
    // Intel SDM Vol. 2A, IDIV r/m64: signed RDX:RAX / SRC → RAX, rem RDX.
    const i64 divisor = static_cast<i64>(src);
    if (divisor == 0) return 1;
    using i128 = __int128;
    const i128 dividend =
        (static_cast<i128>(
             static_cast<i64>(cpu->gpr.get64(static_cast<u8>(RegIndex::RDX))))
         << 64) |
        static_cast<unsigned __int128>(
            cpu->gpr.get64(static_cast<u8>(RegIndex::RAX)));
    const i128 quotient = dividend / divisor;
    if (quotient > static_cast<i128>(0x7FFFFFFFFFFFFFFFLL) ||
        quotient < static_cast<i128>(-0x7FFFFFFFFFFFFFFFLL - 1)) {
        return 1;
    }
    cpu->gpr.set64(static_cast<u8>(RegIndex::RAX),
                   static_cast<u64>(quotient));
    cpu->gpr.set64(static_cast<u8>(RegIndex::RDX),
                   static_cast<u64>(static_cast<i64>(dividend % divisor)));
    return 0;
}

extern "C" u64 jit_adc64(CpuState* cpu, u64 dst, u64 src) {
    // Intel SDM Vol. 2A, ADC: DEST := DEST + SRC + CF. ADC overwrites all
    // arithmetic status flags, so only the incoming lazy CF must be read.
    const u64 carry = cpu->lazy_cf() ? 1u : 0u;
    const u64 result = dst + src + carry;
    cpu->set_flag_op(CpuState::FlagOp::None);
    cpu->flags.update_zsp(result);
    cpu->flags.set_cf(result < dst || (carry != 0 && result == dst));
    constexpr u64 sign_bit = u64(1) << 63;
    cpu->flags.set_of(((dst ^ result) & (src ^ result) & sign_bit) != 0);
    cpu->flags.set_af(((dst ^ src ^ result) & 0x10u) != 0);
    return result;
}

extern "C" u64 jit_sbb64(CpuState* cpu, u64 dst, u64 src) {
    // Intel SDM Vol. 2B, SBB: DEST := DEST - (SRC + CF). Same eager-flag
    // contract as jit_adc64 — there is no FlagOp::Sbb64 lazy producer yet, and
    // an x64 launcher profile shows SBB r64 as a top remaining delegate.
    const u64 carry = cpu->lazy_cf() ? 1u : 0u;
    const u64 result = dst - src - carry;
    cpu->set_flag_op(CpuState::FlagOp::None);
    cpu->flags.update_zsp(result);
    cpu->flags.set_cf(dst < src || (carry != 0 && dst == src));
    constexpr u64 sign_bit = u64(1) << 63;
    cpu->flags.set_of(((dst ^ src) & (dst ^ result) & sign_bit) != 0);
    cpu->flags.set_af(((dst ^ src ^ result) & 0x10u) != 0);
    return result;
}

// ---- Specialized per-cc helpers ----------------------------------------
// Each helper is the cond_code-specialized form of jit_eval_condition.
// At JIT time the cond_code is a compile-time constant, so the JIT
// emit-site can directly load the matching specialized fnptr from
// JitContext::cond_fns[cc] instead of passing cond_code as an argument
// and dispatching through the generic 16-way switch. This drops one
// movz_w per call and lets each leaf inline its single flag formula.
// Each helper first asks the lazy flag state for the single condition it
// needs (CpuState::try_lazy_condition) and materializes the full flag set
// only when the recorded op does not determine that condition. Materializing
// computes all six flags and rewrites rflags; a Jcc needs one or two bits,
// and on flag-dense guest loops that difference is the dominant JIT cost.
// The lazy state is left intact by the fast path, so a later full flag read
// still materializes exactly the same values.
extern "C" u32 jit_cond_o(CpuState* cpu) {
    const int v = cpu->try_lazy_condition(0x0);
    if (v >= 0) return static_cast<u32>(v);
    cpu->materialize_lazy_flags();
    return static_cast<u32>((cpu->flags.rflags >> 11) & 1);
}
extern "C" u32 jit_cond_no(CpuState* cpu) {
    const int v = cpu->try_lazy_condition(0x1);
    if (v >= 0) return static_cast<u32>(v);
    cpu->materialize_lazy_flags();
    return static_cast<u32>(((cpu->flags.rflags >> 11) & 1) ^ 1);
}
extern "C" u32 jit_cond_b(CpuState* cpu) {
    const int v = cpu->try_lazy_condition(0x2);
    if (v >= 0) return static_cast<u32>(v);
    cpu->materialize_lazy_flags();
    return static_cast<u32>(cpu->flags.rflags & 1);
}
extern "C" u32 jit_cond_nb(CpuState* cpu) {
    const int v = cpu->try_lazy_condition(0x3);
    if (v >= 0) return static_cast<u32>(v);
    cpu->materialize_lazy_flags();
    return static_cast<u32>((cpu->flags.rflags & 1) ^ 1);
}
extern "C" u32 jit_cond_z(CpuState* cpu) {
    const int v = cpu->try_lazy_condition(0x4);
    if (v >= 0) return static_cast<u32>(v);
    cpu->materialize_lazy_flags();
    return static_cast<u32>((cpu->flags.rflags >> 6) & 1);
}
extern "C" u32 jit_cond_nz(CpuState* cpu) {
    const int v = cpu->try_lazy_condition(0x5);
    if (v >= 0) return static_cast<u32>(v);
    cpu->materialize_lazy_flags();
    return static_cast<u32>(((cpu->flags.rflags >> 6) & 1) ^ 1);
}
extern "C" u32 jit_cond_be(CpuState* cpu) {
    const int v = cpu->try_lazy_condition(0x6);
    if (v >= 0) return static_cast<u32>(v);
    cpu->materialize_lazy_flags();
    u64 f = cpu->flags.rflags;
    return static_cast<u32>((f & 1) | ((f >> 6) & 1));
}
extern "C" u32 jit_cond_nbe(CpuState* cpu) {
    const int v = cpu->try_lazy_condition(0x7);
    if (v >= 0) return static_cast<u32>(v);
    cpu->materialize_lazy_flags();
    u64 f = cpu->flags.rflags;
    return static_cast<u32>(((f & 1) | ((f >> 6) & 1)) ^ 1);
}
extern "C" u32 jit_cond_s(CpuState* cpu) {
    const int v = cpu->try_lazy_condition(0x8);
    if (v >= 0) return static_cast<u32>(v);
    cpu->materialize_lazy_flags();
    return static_cast<u32>((cpu->flags.rflags >> 7) & 1);
}
extern "C" u32 jit_cond_ns(CpuState* cpu) {
    const int v = cpu->try_lazy_condition(0x9);
    if (v >= 0) return static_cast<u32>(v);
    cpu->materialize_lazy_flags();
    return static_cast<u32>(((cpu->flags.rflags >> 7) & 1) ^ 1);
}
extern "C" u32 jit_cond_p(CpuState* cpu) {
    const int v = cpu->try_lazy_condition(0xA);
    if (v >= 0) return static_cast<u32>(v);
    cpu->materialize_lazy_flags();
    return static_cast<u32>((cpu->flags.rflags >> 2) & 1);
}
extern "C" u32 jit_cond_np(CpuState* cpu) {
    const int v = cpu->try_lazy_condition(0xB);
    if (v >= 0) return static_cast<u32>(v);
    cpu->materialize_lazy_flags();
    return static_cast<u32>(((cpu->flags.rflags >> 2) & 1) ^ 1);
}
extern "C" u32 jit_cond_l(CpuState* cpu) {
    const int v = cpu->try_lazy_condition(0xC);
    if (v >= 0) return static_cast<u32>(v);
    cpu->materialize_lazy_flags();
    u64 f = cpu->flags.rflags;
    return static_cast<u32>(((f >> 7) & 1) ^ ((f >> 11) & 1));
}
extern "C" u32 jit_cond_nl(CpuState* cpu) {
    const int v = cpu->try_lazy_condition(0xD);
    if (v >= 0) return static_cast<u32>(v);
    cpu->materialize_lazy_flags();
    u64 f = cpu->flags.rflags;
    return static_cast<u32>((((f >> 7) & 1) ^ ((f >> 11) & 1)) ^ 1);
}
extern "C" u32 jit_cond_le(CpuState* cpu) {
    const int v = cpu->try_lazy_condition(0xE);
    if (v >= 0) return static_cast<u32>(v);
    cpu->materialize_lazy_flags();
    u64 f = cpu->flags.rflags;
    u32 zf = static_cast<u32>((f >> 6) & 1);
    u32 ne = static_cast<u32>(((f >> 7) & 1) ^ ((f >> 11) & 1));
    return zf | ne;
}
extern "C" u32 jit_cond_nle(CpuState* cpu) {
    const int v = cpu->try_lazy_condition(0xF);
    if (v >= 0) return static_cast<u32>(v);
    cpu->materialize_lazy_flags();
    u64 f = cpu->flags.rflags;
    u32 zf = static_cast<u32>((f >> 6) & 1);
    u32 ne = static_cast<u32>(((f >> 7) & 1) ^ ((f >> 11) & 1));
    return (zf | ne) ^ 1;
}

// Intel SDM Vol. 2 CMPXCHG8B / Vol. 3 paging: an unreadable m64 operand faults
// (#PF/#GP), it does not complete. Raising through cpu->pending_raise is what
// the LOCKed jit_lock_cmpxchg8b/jit_lock_cmpxchg16b helpers do; a silent return
// here would leave ZF and EDX:EAX holding whatever the previous instruction put
// there, so the guest's compare-and-swap loop reads a stale answer as if the
// RMW had succeeded.
static void jit_cmpxchg_raise_av(CpuState* cpu, u64 addr) noexcept {
    if (cpu == nullptr || cpu->pending_raise.active) return;
    auto& pr = cpu->pending_raise;
    pr.active = true;
    pr.from_helper = true;
    pr.code = 0xC0000005u;  // STATUS_ACCESS_VIOLATION
    pr.flags = 0;
    pr.num_args = 2;
    pr.args[0] = 1u;  // write attempted
    pr.args[1] = addr;
}

extern "C" void jit_cmpxchg8b(CpuState* cpu, GuestMemory* mem, u64 addr) {
    auto lo = mem->read_u32(addr);
    auto hi = mem->read_u32(addr + 4);
    if (!lo.is_ok() || !hi.is_ok()) {
        jit_cmpxchg_raise_av(cpu, addr);
        return;
    }

    u32 mem_lo = lo.value();
    u32 mem_hi = hi.value();
    u32 eax = static_cast<u32>(cpu->gpr.get64(static_cast<u8>(RegIndex::RAX)));
    u32 edx = static_cast<u32>(cpu->gpr.get64(static_cast<u8>(RegIndex::RDX)));

    cpu->materialize_lazy_flags();

    if (mem_lo == eax && mem_hi == edx) {
        cpu->flags.set_zf(true);
        u32 ebx = static_cast<u32>(cpu->gpr.get64(static_cast<u8>(RegIndex::RBX)));
        u32 ecx = static_cast<u32>(cpu->gpr.get64(static_cast<u8>(RegIndex::RCX)));
        (void)mem->write_u32(addr, ebx);
        (void)mem->write_u32(addr + 4, ecx);
    } else {
        cpu->flags.set_zf(false);
        cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), mem_lo);
        cpu->gpr.set32(static_cast<u8>(RegIndex::RDX), mem_hi);
    }
}

extern "C" u32 jit_shld32(CpuState* cpu, u32 dest, u32 src, u32 count) {
    count &= 31;
    if (count == 0) return dest;
    // Intel SDM Vol. 2B (SHLD/SHRD): for a non-zero count CF is the last bit
    // shifted out and SF/ZF/PF are set from the result; AF is undefined and OF
    // is defined only for count == 1. Every architecturally-defined flag is
    // rewritten below, so a pending lazy op contributes nothing that survives
    // -- only AF, and OF when count > 1, could carry over, and the SDM leaves
    // both undefined. Discard the lazy state instead of computing it.
    //
    // This was the single largest source of flag materialization measured on
    // one load: one guest RIP (a table-driven hash inner loop --
    // movzx/shld/shr/xor/table-xor) accounted for 9,871,370 of 15.4M
    // materialize calls, 64% of the total.
    //
    // set_flag_op() also clears cf_overlay/of_overlay, which is correct here:
    // SHLD/SHRD rewrite CF, and OF is either rewritten (count == 1) or
    // undefined. nzcv_kind/nzcv_bits must be cleared too -- they advertise a
    // carried ARM NZCV that a later Jcc would consult in preference to the
    // rflags written below.
    cpu->set_flag_op(CpuState::FlagOp::None);
    cpu->nzcv_kind = 0;
    cpu->nzcv_bits = 0;

    bool cf = (dest >> (32 - count)) & 1;
    u32 result = (dest << count) | (src >> (32 - count));

    u64 rf = cpu->flags.rflags;
    rf = (rf & ~1ULL) | (cf ? 1ULL : 0ULL);
    rf = (rf & ~(1ULL << 6)) | ((result == 0) ? (1ULL << 6) : 0ULL);
    rf = (rf & ~(1ULL << 7)) | (((result >> 31) & 1) ? (1ULL << 7) : 0ULL);
    u8 lo = result & 0xFF;
    lo ^= lo >> 4; lo ^= lo >> 2; lo ^= lo >> 1;
    rf = (rf & ~(1ULL << 2)) | ((lo & 1) == 0 ? (1ULL << 2) : 0ULL);
    if (count == 1) {
        bool of = ((result >> 31) ^ (dest >> 31)) & 1;
        rf = (rf & ~(1ULL << 11)) | (of ? (1ULL << 11) : 0ULL);
    }
    cpu->flags.rflags = rf;
    cpu->set_flag_op(CpuState::FlagOp::None);
    return result;
}

extern "C" u32 jit_shrd32(CpuState* cpu, u32 dest, u32 src, u32 count) {
    count &= 31;
    if (count == 0) return dest;
    // Intel SDM Vol. 2B (SHLD/SHRD): for a non-zero count CF is the last bit
    // shifted out and SF/ZF/PF are set from the result; AF is undefined and OF
    // is defined only for count == 1. Every architecturally-defined flag is
    // rewritten below, so a pending lazy op contributes nothing that survives
    // -- only AF, and OF when count > 1, could carry over, and the SDM leaves
    // both undefined. Discard the lazy state instead of computing it.
    //
    // This was the single largest source of flag materialization measured on
    // one load: one guest RIP (a table-driven hash inner loop --
    // movzx/shld/shr/xor/table-xor) accounted for 9,871,370 of 15.4M
    // materialize calls, 64% of the total.
    //
    // set_flag_op() also clears cf_overlay/of_overlay, which is correct here:
    // SHLD/SHRD rewrite CF, and OF is either rewritten (count == 1) or
    // undefined. nzcv_kind/nzcv_bits must be cleared too -- they advertise a
    // carried ARM NZCV that a later Jcc would consult in preference to the
    // rflags written below.
    cpu->set_flag_op(CpuState::FlagOp::None);
    cpu->nzcv_kind = 0;
    cpu->nzcv_bits = 0;

    bool cf = (dest >> (count - 1)) & 1;
    u32 result = (dest >> count) | (src << (32 - count));

    u64 rf = cpu->flags.rflags;
    rf = (rf & ~1ULL) | (cf ? 1ULL : 0ULL);
    rf = (rf & ~(1ULL << 6)) | ((result == 0) ? (1ULL << 6) : 0ULL);
    rf = (rf & ~(1ULL << 7)) | (((result >> 31) & 1) ? (1ULL << 7) : 0ULL);
    u8 lo = result & 0xFF;
    lo ^= lo >> 4; lo ^= lo >> 2; lo ^= lo >> 1;
    rf = (rf & ~(1ULL << 2)) | ((lo & 1) == 0 ? (1ULL << 2) : 0ULL);
    if (count == 1) {
        bool of = ((dest >> 31) ^ (dest >> 30)) & 1;
        rf = (rf & ~(1ULL << 11)) | (of ? (1ULL << 11) : 0ULL);
    }
    cpu->flags.rflags = rf;
    cpu->set_flag_op(CpuState::FlagOp::None);
    return result;
}

extern "C" void jit_cmpxchg32(CpuState* cpu, GuestMemory* mem, u64 addr, u32 reg_val) {
    auto rm_result = mem->read_u32(addr);
    if (!rm_result.is_ok()) return;
    u32 rm_val = rm_result.value();
    u32 eax = static_cast<u32>(cpu->gpr.get64(static_cast<u8>(RegIndex::RAX)));

    cpu->materialize_lazy_flags();
    // Flags are `accumulator - DEST`, measured on real hardware:
    // tools/oracle_probe/cmpxchg_flag_probe.c and its recorded output.
    u32 cmp_result = eax - rm_val;
    cpu->flags.update_flags_sub(eax, rm_val, cmp_result);

    if (eax == rm_val) {
        (void)mem->write_u32(addr, reg_val);
    } else {
        cpu->gpr.set32(static_cast<u8>(RegIndex::RAX), rm_val);
    }
    cpu->set_flag_op(CpuState::FlagOp::None);
}

} // namespace vitreo::cpu::jit
