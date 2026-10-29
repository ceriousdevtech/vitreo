/// @file crt_fpenv.cpp
/// @brief The CRT floating-point environment: _controlfp, _control87,
///        _controlfp_s, __control87_2, _statusfp, _statusfp2, _clearfp,
///        _fpreset and __fpe_flt_rounds.
///
/// Contract: Learn "_control87, _controlfp, __control87_2", "_statusfp",
/// "_clearfp", "_fpreset" and <float.h>'s bit values, with every behaviour the
/// pages leave open measured by tools/oracle_probe/crt_fpctrl_probe.c on x86
/// and x64 (ucrtbase.dll and msvcrt.dll):
///
///  - The abstract control word is computed from the HARDWARE on every call --
///    the x87 FCW and MXCSR of the calling thread -- never from a copy the CRT
///    remembers. A guest that executes FLDCW or LDMXCSR sees the change in
///    _controlfp(0, 0).
///  - x64: only MXCSR is involved. Its masks, rounding and FTZ/DAZ fold into
///    _MCW_EM, _MCW_RC and _MCW_DN; _MCW_PC and _MCW_IC read 0 and setting them
///    does nothing (no invalid-parameter call, measured -- Learn says one is
///    raised). _statusfp/_clearfp see only MXCSR; _fpreset resets only MXCSR.
///  - x86: both units. The returned word is the x87 word ORed with the SSE
///    word, plus _EM_AMBIGUOUS when their masks or rounding disagree; a set
///    updates each unit's own fields (x87: EM RC PC IC; SSE: EM RC DN).
///    _statusfp ORs both units' flags; _clearfp clears both and returns both
///    (ucrtbase) or only the x87 flags (msvcrt.dll, measured); _fpreset
///    restores FCW 0x27F and MXCSR 0x1F80.
///  - _controlfp never changes the DENORMAL exception mask; _control87 can.
///  - _controlfp_s performs no validation (measured: any mask, a NULL
///    current-word pointer and precision control on x64 all return 0).
///
/// The rounding and flush modes reach the host FPU the way LDMXCSR/FLDCW do:
/// the JIT installs them from CpuState at the next FP block, and the status
/// flags are harvested from the host's sticky bits when the guest looks
/// (core/math/host_fenv.hpp).

#include "winapi/msvcrt/runtime/crt_init.hpp"

#include "core/cpu/state/cpu_state.hpp"
#include "core/math/host_fenv.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"

namespace vitreo::winapi {

namespace {

// <float.h>
constexpr u32 kMcwEm = 0x0008001Fu;
constexpr u32 kEmInvalid = 0x00000010u;
constexpr u32 kEmDenormal = 0x00080000u;
constexpr u32 kEmZeroDivide = 0x00000008u;
constexpr u32 kEmOverflow = 0x00000004u;
constexpr u32 kEmUnderflow = 0x00000002u;
constexpr u32 kEmInexact = 0x00000001u;
constexpr u32 kEmAmbiguous = 0x80000000u;
constexpr u32 kMcwRc = 0x00000300u;
constexpr u32 kMcwPc = 0x00030000u;
constexpr u32 kPc24 = 0x00020000u;
constexpr u32 kPc53 = 0x00010000u;
constexpr u32 kMcwIc = 0x00040000u;
constexpr u32 kMcwDn = 0x03000000u;
constexpr u32 kDnFlush = 0x01000000u;
constexpr u32 kDnFlushOperandsSaveResults = 0x02000000u;
constexpr u32 kDnSaveOperandsFlushResults = 0x03000000u;

// MXCSR
constexpr u32 kMxcsrFlags = 0x003Fu;
constexpr u32 kMxcsrDaz = 0x0040u;
constexpr u32 kMxcsrFtz = 0x8000u;
constexpr u32 kMxcsrDefault = 0x1F80u;
// x87
constexpr u16 kFcwDefault = 0x027Fu;

/// Exception bits in unit order (IE DE ZE OE UE PE: FCW/FSW bits 0-5, MXCSR
/// flag bits 0-5 and mask bits 7-12) and their <float.h> _EM_ / _SW_ values,
/// which share encodings.
constexpr u32 kAbstractException[6] = {kEmInvalid,  kEmDenormal,  kEmZeroDivide,
                                       kEmOverflow, kEmUnderflow, kEmInexact};

u32 exceptions_to_abstract(u32 six_bits) {
    u32 out = 0;
    for (u32 i = 0; i < 6; ++i) {
        if (six_bits & (1u << i)) out |= kAbstractException[i];
    }
    return out;
}
u32 abstract_to_exceptions(u32 abstract) {
    u32 out = 0;
    for (u32 i = 0; i < 6; ++i) {
        if (abstract & kAbstractException[i]) out |= 1u << i;
    }
    return out;
}

/// Hardware RC (0 near, 1 down, 2 up, 3 chop) <-> _RC_* (same order, << 8).
u32 rc_to_abstract(u32 rc) { return (rc & 3u) << 8; }
u32 abstract_to_rc(u32 abstract) { return (abstract & kMcwRc) >> 8; }

u32 x87_abstract(u16 fcw) {
    u32 word = exceptions_to_abstract(fcw & 0x3Fu);
    word |= rc_to_abstract(fcw >> 10);
    switch ((fcw >> 8) & 3u) {
    case 0: word |= kPc24; break;
    case 2: word |= kPc53; break;
    default: break;  // 1 (reserved) and 3 read as _PC_64
    }
    if (fcw & 0x1000u) word |= kMcwIc;  // _IC_AFFINE
    return word;
}

u16 x87_from_abstract(u16 fcw, u32 word) {
    u16 out = static_cast<u16>(fcw & ~0x1F3Fu);
    out = static_cast<u16>(out | abstract_to_exceptions(word & kMcwEm));
    out = static_cast<u16>(out | (abstract_to_rc(word) << 10));
    u16 pc = 3;
    if ((word & kMcwPc) == kPc24) pc = 0;
    else if ((word & kMcwPc) == kPc53) pc = 2;
    out = static_cast<u16>(out | (pc << 8));
    if (word & kMcwIc) out = static_cast<u16>(out | 0x1000u);
    return out;
}

u32 sse_abstract(u32 mxcsr) {
    u32 word = exceptions_to_abstract((mxcsr >> 7) & 0x3Fu);
    word |= rc_to_abstract(mxcsr >> 13);
    const bool ftz = (mxcsr & kMxcsrFtz) != 0;
    const bool daz = (mxcsr & kMxcsrDaz) != 0;
    if (ftz && daz) word |= kDnFlush;
    else if (daz) word |= kDnFlushOperandsSaveResults;
    else if (ftz) word |= kDnSaveOperandsFlushResults;
    return word;
}

u32 sse_from_abstract(u32 mxcsr, u32 word) {
    u32 out = mxcsr & ~(0x1F80u | 0x6000u | kMxcsrFtz | kMxcsrDaz);
    out |= abstract_to_exceptions(word & kMcwEm) << 7;
    out |= abstract_to_rc(word) << 13;
    switch (word & kMcwDn) {
    case kDnFlush: out |= kMxcsrFtz | kMxcsrDaz; break;
    case kDnFlushOperandsSaveResults: out |= kMxcsrDaz; break;
    case kDnSaveOperandsFlushResults: out |= kMxcsrFtz; break;
    default: break;
    }
    return out;
}

/// The word _control87/_controlfp report.
u32 combined(const cpu::CpuState& cpu, bool is64) {
    const u32 sse = sse_abstract(cpu.sse.mxcsr);
    if (is64) return sse;
    const u32 x87 = x87_abstract(cpu.fpu.control_word);
    u32 word = x87 | sse;
    if ((x87 & (kMcwEm | kMcwRc)) != (sse & (kMcwEm | kMcwRc))) word |= kEmAmbiguous;
    return word;
}

void write_fcw(cpu::CpuState& cpu, u16 fcw) {
    cpu.fpu.control_word = fcw;
    math::apply_x87_rounding(fcw);
}

/// Like LDMXCSR: the value is the whole register, sticky flags included, so
/// flags the host accumulated before now are superseded. Callers that keep the
/// flags harvest them into @p mxcsr first.
void write_mxcsr(cpu::CpuState& cpu, u32 mxcsr) {
    math::discard_sse_exceptions();
    cpu.sse.mxcsr = mxcsr & cpu.sse.mxcsr_mask;
    // Changing MXCSR is these functions' documented purpose: exempt them from
    // the x64 dispatcher's callee-saved MXCSR restore.
    cpu.thunk_sets_fp_control = true;
}

/// Apply @p new_bits under @p mask to each unit's own fields.
void set_units(cpu::CpuState& cpu, bool is64, u32 new_bits, u32 mask, bool x87,
               bool sse) {
    if (sse) {
        cpu.sse.mxcsr |= math::harvest_sse_exceptions();
        const u32 current = sse_abstract(cpu.sse.mxcsr);
        const u32 sse_mask = mask & (kMcwEm | kMcwRc | kMcwDn);
        write_mxcsr(cpu, sse_from_abstract(cpu.sse.mxcsr,
                                           (current & ~sse_mask) | (new_bits & sse_mask)));
    }
    if (x87 && !is64) {
        const u32 current = x87_abstract(cpu.fpu.control_word);
        const u32 x87_mask = mask & (kMcwEm | kMcwRc | kMcwPc | kMcwIc);
        write_fcw(cpu, x87_from_abstract(cpu.fpu.control_word,
                                         (current & ~x87_mask) | (new_bits & x87_mask)));
    }
}

/// _control87 (@p keep_denormal false) and _controlfp (true).
u32 control(cpu::CpuState& cpu, bool is64, u32 new_bits, u32 mask, bool keep_denormal) {
    if (keep_denormal) mask &= ~kEmDenormal;
    if (mask != 0) set_units(cpu, is64, new_bits, mask, true, true);
    return combined(cpu, is64);
}

u32 x87_status(cpu::CpuState& cpu) {
    cpu.fpu.status_word = static_cast<u16>(cpu.fpu.status_word |
                                           math::harvest_x87_exceptions());
    return exceptions_to_abstract(cpu.fpu.status_word & 0x3Fu);
}
u32 sse_status(cpu::CpuState& cpu) {
    cpu.sse.mxcsr |= math::harvest_sse_exceptions();
    return exceptions_to_abstract(cpu.sse.mxcsr & kMxcsrFlags);
}

} // namespace

void add_crt_fpenv_exports(DllExports& exports) {
    // unsigned int _controlfp(unsigned int new, unsigned int mask)
    exports.entries.push_back({"_controlfp", 0, [](Marshaler& m) {
        if (!m.has_cpu()) return m.set_return(0);
        m.set_return(control(*m.cpu(), m.cpu_is_64bit(), static_cast<u32>(m.arg(0)),
                             static_cast<u32>(m.arg(1)), true));
    }, 0});
    // unsigned int _control87(unsigned int new, unsigned int mask)
    exports.entries.push_back({"_control87", 0, [](Marshaler& m) {
        if (!m.has_cpu()) return m.set_return(0);
        m.set_return(control(*m.cpu(), m.cpu_is_64bit(), static_cast<u32>(m.arg(0)),
                             static_cast<u32>(m.arg(1)), false));
    }, 0});
    // errno_t _controlfp_s(unsigned int* current, unsigned int new,
    //                      unsigned int mask)
    exports.entries.push_back({"_controlfp_s", 0, [](Marshaler& m) {
        if (!m.has_cpu()) return m.set_return(0);
        const GuestAddr current = m.arg_ptr(0);
        const u32 word = control(*m.cpu(), m.cpu_is_64bit(), static_cast<u32>(m.arg(1)),
                                 static_cast<u32>(m.arg(2)), true);
        if (current != 0) (void)m.write_u32(current, word);
        m.set_return(0);
    }, 0});
    // int __control87_2(unsigned int new, unsigned int mask,
    //                   unsigned int* x86_cw, unsigned int* sse2_cw)  [x86]
    // Each unit is read and set only when its pointer is non-NULL.
    exports.entries.push_back({"__control87_2", 0, [](Marshaler& m) {
        if (!m.has_cpu()) return m.set_return(0);
        auto& cpu = *m.cpu();
        const bool is64 = m.cpu_is_64bit();
        const u32 new_bits = static_cast<u32>(m.arg(0));
        const u32 mask = static_cast<u32>(m.arg(1));
        const GuestAddr x87_out = m.arg_ptr(2);
        const GuestAddr sse_out = m.arg_ptr(3);
        if (mask != 0) set_units(cpu, is64, new_bits, mask, x87_out != 0, sse_out != 0);
        if (x87_out != 0) (void)m.write_u32(x87_out, x87_abstract(cpu.fpu.control_word));
        if (sse_out != 0) (void)m.write_u32(sse_out, sse_abstract(cpu.sse.mxcsr));
        m.set_return(1);
    }, 0});

    // unsigned int _statusfp(void)
    exports.entries.push_back({"_statusfp", 0, [](Marshaler& m) {
        if (!m.has_cpu()) return m.set_return(0);
        auto& cpu = *m.cpu();
        u32 status = sse_status(cpu);
        if (!m.cpu_is_64bit()) status |= x87_status(cpu);
        m.set_return(status);
    }, 0});
    // void _statusfp2(unsigned int* x87, unsigned int* sse2)  [x86]
    exports.entries.push_back({"_statusfp2", 0, [](Marshaler& m) {
        if (!m.has_cpu()) return;
        auto& cpu = *m.cpu();
        const GuestAddr x87_out = m.arg_ptr(0);
        const GuestAddr sse_out = m.arg_ptr(1);
        if (x87_out != 0) (void)m.write_u32(x87_out, x87_status(cpu));
        if (sse_out != 0) (void)m.write_u32(sse_out, sse_status(cpu));
    }, 0});
    // unsigned int _clearfp(void): the flags it cleared.
    exports.entries.push_back({"_clearfp", 0, [](Marshaler& m) {
        if (!m.has_cpu()) return m.set_return(0);
        auto& cpu = *m.cpu();
        const bool is64 = m.cpu_is_64bit();
        const u32 x87 = x87_status(cpu);  // harvested on x64 too, then kept
        const u32 sse = sse_status(cpu);
        write_mxcsr(cpu, cpu.sse.mxcsr & ~kMxcsrFlags);
        if (is64) return m.set_return(sse);
        // FNCLEX: the exception flags, the stack fault, the summary and busy.
        cpu.fpu.status_word = static_cast<u16>(cpu.fpu.status_word & ~0x80FFu);
        math::clear_host_fp_exceptions();
        m.set_return(CrtDllFlavorScope::current() == CrtDllFlavor::Msvcrt ? x87 : (x87 | sse));
    }, 0});
    // void _fpreset(void)
    exports.entries.push_back({"_fpreset", 0, [](Marshaler& m) {
        if (!m.has_cpu()) return;
        auto& cpu = *m.cpu();
        write_mxcsr(cpu, kMxcsrDefault);
        if (m.cpu_is_64bit()) return;
        // FNINIT, then the CRT's default control word.
        cpu.fpu.status_word = 0;
        cpu.fpu.tag_word = 0xFFFF;
        math::clear_host_fp_exceptions();
        write_fcw(cpu, kFcwDefault);
    }, 0});

    // int __fpe_flt_rounds(void): FLT_ROUNDS (C17 5.2.4.2.2) for the rounding
    // mode _controlfp reports: -1 indeterminable, 0 toward zero, 1 nearest,
    // 2 toward +inf, 3 toward -inf.
    exports.entries.push_back({"__fpe_flt_rounds", 0, [](Marshaler& m) {
        if (!m.has_cpu()) return m.set_return(1);
        switch (combined(*m.cpu(), m.cpu_is_64bit()) & kMcwRc) {
        case 0x000u: return m.set_return(1);
        case 0x100u: return m.set_return(3);
        case 0x200u: return m.set_return(2);
        default: return m.set_return(0);
        }
    }, 0});
}

} // namespace vitreo::winapi
