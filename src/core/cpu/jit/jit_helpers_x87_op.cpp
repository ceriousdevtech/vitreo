// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_helpers_x87_op.cpp
/// @brief jit_fpu_op: the generic x87 dispatcher called from JIT code.
///
/// Handles the common x87 forms inline and delegates the rest to the
/// interpreter's exec_fpu. VITREO_JIT_FPU_HIST enables its opcode histogram.
///
/// Split out of jit_helpers.cpp. Closed in both directions, checked with
/// tools/architecture/plan_extraction.py before moving; the cut is open only
/// by the two promotions declared in jit_helpers_internal.hpp.

#include "jit_helpers.hpp"
#include "jit_helpers_internal.hpp"
#include "../interpreter/interpreter.hpp"
#include "../interpreter/ops/exec_ops.hpp"
#include "../seh/seh32.hpp"
#include "core/memory/mem_watch.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace vitreo::cpu::jit {

// Read once at static init rather than latched on first call, so the answer
// does not depend on when the first SSE helper happens to run.
static std::atomic<bool> g_fpu_helper_histogram_enabled{
    std::getenv("VITREO_JIT_FPU_HIST") != nullptr};

extern "C" void jit_fpu_op(CpuState* cpu, GuestMemory* mem, u32 opcode_id, u64 addr) {
    // Packed by compile_fpu: bits[31:28]=kind (0 none / 1 mem / 2 sti /
    // 3 preloaded m32 / 4 preloaded signed i64), bits[27:0]=Opcode. Legacy
    // ids 0..10 (pre-generic FPU JIT) are still accepted so any stale JIT
    // pages that survive a soft reload stay safe.
    constexpr u32 kKindNone = 0;
    constexpr u32 kKindMem  = 1;
    constexpr u32 kKindSti  = 2;
    constexpr u32 kKindM32Value = 3;
    constexpr u32 kKindI64Value = 4;
    // FSTP/etc. write through GuestMemory; publish RIP so VITREO_WATCH_VALUE
    // can attribute the store under JIT. Skip the call when watch is off —
    // this helper is the x87 hot path.
    if (vitreo::mem_watch_enabled()) {
        jit_publish_mem_watch_cpu(cpu);
    }

    u32 kind = opcode_id >> 28;
    Opcode op = static_cast<Opcode>(opcode_id & 0x0FFFFFFFu);

    // DIAGNOSTIC (VITREO_JIT_FPU_INLINE=<mask>): bisect the helper's inline
    // fast cases against its exec_fpu delegate tail. bit0 = mem/m32-value
    // cases, bit1 = ST(i) cases, bit2 = no-operand cases. A cleared bit routes
    // that group to the delegate instead. Default (unset) keeps every group
    // inline, i.e. no behaviour change.
    static const u32 s_inline_mask = [] {
        const char* v = std::getenv("VITREO_JIT_FPU_INLINE");
        return v ? static_cast<u32>(std::strtoul(v, nullptr, 0)) : 0x7u;
    }();
    // DIAGNOSTIC (VITREO_JIT_FPU_SKIP=NAME[,NAME...]): force these specific
    // opcodes to the exec_fpu delegate, for bisecting one inline case at a
    // time. Matches opcode_name(), case-sensitive.
    // Numeric Opcode enum ids, not names: opcode_name() returns display text
    // ("MOV r/m32, r32") and has no case at all for the x87 memory forms, so a
    // name-matched list silently matches nothing and every bisect step reads
    // as "still fails".
    static const std::vector<u32> s_skip_ids = [] {
        std::vector<u32> ids;
        const char* v = std::getenv("VITREO_JIT_FPU_SKIP");
        if (v == nullptr) return ids;
        const char* p = v;
        while (*p != '\0') {
            char* end = nullptr;
            const unsigned long id = std::strtoul(p, &end, 0);
            if (end == p) break;
            ids.push_back(static_cast<u32>(id));
            p = (*end == ',') ? end + 1 : end;
        }
        return ids;
    }();
    const bool skip_this_op =
        !s_skip_ids.empty() &&
        std::find(s_skip_ids.begin(), s_skip_ids.end(),
                  static_cast<u32>(op)) != s_skip_ids.end();

    const bool inline_mem  = !skip_this_op && (s_inline_mask & 1u) != 0;
    const bool inline_sti  = !skip_this_op && (s_inline_mask & 2u) != 0;
    const bool inline_none = !skip_this_op && (s_inline_mask & 4u) != 0;

    // GUI pumping stays in the dispatcher (execute returns on the hop cap).
    // A function-local static here was an Apple TLS guard on every x87 helper.
    if (g_fpu_helper_histogram_enabled.load(std::memory_order_relaxed)) {
        static std::array<std::atomic<u64>, 2048> s_op_hist{};
        static std::atomic<u64> s_total{0};
        const usize op_index = static_cast<usize>(op);
        if (op_index < s_op_hist.size()) {
            s_op_hist[op_index].fetch_add(1, std::memory_order_relaxed);
        }
        const u64 total =
            s_total.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((total % 1'000'000ULL) == 0) {
            std::vector<std::pair<usize, u64>> top;
            for (usize i = 0; i < s_op_hist.size(); ++i) {
                const u64 count =
                    s_op_hist[i].load(std::memory_order_relaxed);
                if (count != 0) top.emplace_back(i, count);
            }
            std::sort(top.begin(), top.end(),
                      [](const auto& a, const auto& b) {
                          return a.second > b.second;
                      });
            std::fprintf(stderr,
                         "[jit-fpu-hist] total=%llu top opcodes:\n",
                         static_cast<unsigned long long>(total));
            for (usize i = 0; i < std::min<usize>(10, top.size()); ++i) {
                const char* name =
                    opcode_name(static_cast<Opcode>(top[i].first));
                std::fprintf(
                    stderr, "  op=%-5zu %-22s %llu (%.1f%%)\n", top[i].first,
                    name != nullptr ? name : "?",
                    static_cast<unsigned long long>(top[i].second),
                    100.0 * static_cast<double>(top[i].second) /
                        static_cast<double>(total));
            }
        }
    }

    // Legacy small-id path (FLD_M64 / FSTP_M64 / … / FILD_M32).
    if (kind == kKindNone && opcode_id <= 10) {
        auto& fpu = cpu->fpu;
        switch (opcode_id) {
        case 0: { // FLD_M64
            auto r = mem->read_u64(addr);
            if (!r.is_ok()) return;
            double val;
            u64 bits = r.value();
            std::memcpy(&val, &bits, sizeof(double));
            fpu.push(fpu.prepare_ieee_source(val));
            return;
        }
        case 1: { // FSTP_M64
            Float80 v = fpu.pop_src();
            double d = fpu.store_m64(v);
            u64 bits;
            std::memcpy(&bits, &d, sizeof(double));
            (void)mem->write_u64(addr, bits);
            return;
        }
        case 2: // FLDZ
            fpu.push(x87_const::fldz());
            return;
        case 3: { // FADD_M64
            auto r = mem->read_u64(addr);
            if (!r.is_ok()) return;
            double val;
            u64 bits = r.value();
            std::memcpy(&val, &bits, sizeof(double));
            // Legacy small-id FADD_M64: route through arith_x87 like every
            // other arithmetic form, or this one path keeps evaluating in
            // binary64 while the rest works in the architectural format.
            fpu.set_st(0, fpu.arith_x87(fpu.read_src(0), fpu.prepare_ieee_source(val),
                                        X87Op::Add));
            return;
        }
        case 4: // FLDLG2
            fpu.push(x87_const::fldlg2(fpu.control_word));
            return;
        case 5: { // FXAM
            Float80 val = fpu.read_src(0);
            bool sign = (val.exponent_sign & 0x8000u) != 0;
            u16 exp = val.exponent_sign & 0x7FFFu;
            fpu.status_word = static_cast<u16>(fpu.status_word & ~(0x4700u));
            if (sign) fpu.status_word = static_cast<u16>(fpu.status_word | 0x0200u);
            if (exp == 0 && val.mantissa == 0) {
                fpu.status_word = static_cast<u16>(fpu.status_word | 0x4000u);
            } else if (exp == 0x7FFF) {
                fpu.status_word = static_cast<u16>(
                    fpu.status_word |
                    (val.mantissa == 0x8000000000000000ULL ? 0x0500u : 0x0100u));
            } else {
                fpu.status_word = static_cast<u16>(fpu.status_word | 0x0400u);
            }
            return;
        }
        case 6: { // FILD_M64
            auto r = mem->read_u64(addr);
            if (!r.is_ok()) return;
            i64 ival = static_cast<i64>(r.value());
            fpu.push(Float80::from_i64(ival));
            return;
        }
        case 7: // EMMS
            fpu.tag_word = 0xFFFF;
            return;
        case 8: { // FSTP_M32
            Float80 v = fpu.pop_src();
            float f = fpu.store_m32(v);
            u32 bits;
            std::memcpy(&bits, &f, sizeof(float));
            (void)mem->write_u32(addr, bits);
            return;
        }
        case 9: // FSTP_STI
            fpu.set_st(static_cast<u8>(addr & 7), fpu.read_src(0));
            fpu.pop();
            return;
        case 10: { // FILD_M32
            auto r = mem->read_u32(addr);
            if (!r.is_ok()) return;
            i32 ival = static_cast<i32>(r.value());
            fpu.push(Float80::from_i64(ival));
            return;
        }
        default:
            break;
        }
    }

    // The JPEG/image paths exercised by real 32-bit applications spend most
    // of their x87 time in this small set. These instructions affect the x87
    // stack/status state, not EFLAGS (Intel SDM Vol. 2, FLD/FST/FADD/FMUL/
    // FSUB/FCOM/FNSTSW), so they must not force Vitreo's pending integer flags
    // to materialize. Keep the interpreter as the semantic fallback for the
    // rest of x87, including the EFLAGS-reading/writing FCMOV/FCOMI families.
    //
    // Intel SDM Vol. 1 x87 data types and Vol. 2 FLD/FSTP/FADD: m32/m64
    // operands convert through 80-bit extended precision on the stack. Use
    // the same Float80 helpers as exec_fpu. Mapped ranges go through
    // host_ptr / host_ptr_mut (record_store + executable-write) instead of
    // the locked write_bytes/read_bytes path; a null host pointer falls
    // back to the previous GuestMemory scalar helpers.
    auto& fpu = cpu->fpu;
    // Apply x87 precision control to the results PC governs. The JIT must
    // do this too: an interpreter-only fix is bypassed the moment a block
    // containing this arithmetic is compiled.
    const GuestAddr gaddr = static_cast<GuestAddr>(addr);
    // DIAGNOSTIC (VITREO_JIT_FPU_NO_HOSTPTR=1): bypass the raw host_ptr read
    // and use the checked GuestMemory scalar path the interpreter uses, to
    // test whether the two disagree about what is at an address.
    static const bool s_no_host_ptr =
        std::getenv("VITREO_JIT_FPU_NO_HOSTPTR") != nullptr;
    const auto load_host = [&](usize size, void* dst) -> bool {
        if (s_no_host_ptr) return false;
        const Byte* host = mem->host_ptr(gaddr, size);
        if (host == nullptr) return false;
        std::memcpy(dst, host, size);
        return true;
    };
    const auto store_host = [&](usize size, const void* src) -> bool {
        Byte* host = mem->host_ptr_store(gaddr, size);
        if (host == nullptr) return false;
        std::memcpy(host, src, size);
        if (store_ring_enabled()) mem_watch_bytes(gaddr, host, size, "jit.fpu");
        return true;
    };
    // A failed guest read has to become a guest access violation, exactly as
    // the native lowering's checked load does. Returning false and leaving the
    // x87 stack alone is only half the contract -- the other half is that the
    // guest is told. This path used to be reachable only under PC = 24, so the
    // gap sat unnoticed until PC = 64 started routing here for the 80-bit
    // arithmetic; JitExecTest.FaddM32ReadFaultDoesNotChangeX87Stack caught it.
    const auto raise_av = [&](bool is_write) {
        auto& pr = cpu->pending_raise;
        if (pr.active) return;  // first AV wins
        pr.active = true;
        pr.from_helper = true;
        pr.code = 0xC0000005u;  // STATUS_ACCESS_VIOLATION
        pr.flags = 0;
        pr.num_args = 2;
        pr.args[0] = is_write ? 1u : 0u;
        pr.args[1] = static_cast<u64>(gaddr);
    };
    const auto read_f32 = [&](float& value) {
        if (kind == kKindM32Value) {
            u32 bits = addr;
            std::memcpy(&value, &bits, sizeof(value));
            return true;
        }
        if (load_host(sizeof(value), &value)) return true;
        const auto raw = mem->read_u32(gaddr);
        if (!raw.is_ok()) { raise_av(false); return false; }
        const u32 bits = raw.value();
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    };
    const auto read_f64 = [&](double& value) {
        if (load_host(sizeof(value), &value)) return true;
        const auto raw = mem->read_u64(gaddr);
        if (!raw.is_ok()) { raise_av(false); return false; }
        const u64 bits = raw.value();
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    };
    const auto compare_x87 = [&](Float80 a, Float80 b, bool quiet) {
        (void)fpu.compare(a, b, quiet);
    };

    if (inline_mem && (kind == kKindMem || kind == kKindM32Value ||
                       kind == kKindI64Value)) {
        float memory_f32 = 0.0f;
        double memory_f64 = 0.0;
        switch (op) {
        case Opcode::FLD_M32:
            if (read_f32(memory_f32)) {
                fpu.push(fpu.prepare_ieee_source(memory_f32));
            }
            return;
        case Opcode::FLD_M64:
            if (read_f64(memory_f64)) {
                fpu.push(fpu.prepare_ieee_source(memory_f64));
            }
            return;
        case Opcode::FILD_M64: {
            u64 raw = addr;
            if (kind != kKindI64Value) {
                if (!load_host(sizeof(raw), &raw)) {
                    const auto value = mem->read_u64(gaddr);
                    if (!value.is_ok()) return;
                    raw = value.value();
                }
            }
            fpu.push(Float80::from_i64(static_cast<i64>(raw)));
            return;
        }
        case Opcode::FST_M32:
        case Opcode::FSTP_M32: {
            // Peek, store, and pop only on success: a faulting store must
            // leave TOP where it was, and must raise the access violation
            // rather than discard the write result. Popping first (and
            // ignoring the write) did both wrong -- invisible while this path
            // only ran under PC = 24, and caught by
            // JitExecTest.FstpM32WriteFaultDoesNotPopX87Stack once the store
            // forms started routing here.
            const Float80 value = fpu.read_src(0);
            const float stored = fpu.store_m32(value);
            u32 bits = 0;
            std::memcpy(&bits, &stored, sizeof(bits));
            if (!store_host(sizeof(bits), &bits) &&
                !mem->write_u32(gaddr, bits).is_ok()) {
                raise_av(true);
                return;
            }
            if (op == Opcode::FSTP_M32) fpu.pop();
            return;
        }
        case Opcode::FST_M64:
        case Opcode::FSTP_M64: {
            // Same peek-store-then-pop rule as the m32 forms above.
            const Float80 value = fpu.read_src(0);
            const double stored = fpu.store_m64(value);
            if (!store_host(sizeof(stored), &stored)) {
                u64 bits = 0;
                std::memcpy(&bits, &stored, sizeof(bits));
                if (!mem->write_u64(gaddr, bits).is_ok()) {
                    raise_av(true);
                    return;
                }
            }
            if (op == Opcode::FSTP_M64) fpu.pop();
            return;
        }
        // SDM Vol. 2 FADD/FSUB/FSUBR/FMUL/FDIV/FDIVR m32fp/m64fp. The reverse
        // forms compute memory OP ST(0), not ST(0) OP memory.
        //
        // FDIV, FDIVR and FSUBR were absent here and fell through to
        // `default: break`, which returns with ST(0) UNTOUCHED -- the
        // instruction silently did nothing instead of dividing. The native
        // ARM64 lowering implements them, so this only fired when an
        // instruction took the helper fallback (Inf/NaN/subnormal operand, or
        // PC=Single), making the JIT disagree with real x86 and with Vitreo's
        // own interpreter on operands the guest reaches at runtime.
        case Opcode::FADD_M32:
        case Opcode::FMUL_M32:
        case Opcode::FSUB_M32:
        case Opcode::FSUBR_M32:
        case Opcode::FDIV_M32:
        case Opcode::FDIVR_M32:
        case Opcode::FADD_M64:
        case Opcode::FMUL_M64:
        case Opcode::FSUB_M64:
        case Opcode::FSUBR_M64:
        case Opcode::FDIV_M64:
        case Opcode::FDIVR_M64: {
            const bool is_m32 =
                op == Opcode::FADD_M32 || op == Opcode::FMUL_M32 ||
                op == Opcode::FSUB_M32 || op == Opcode::FSUBR_M32 ||
                op == Opcode::FDIV_M32 || op == Opcode::FDIVR_M32;
            double operand;
            if (is_m32) {
                if (!read_f32(memory_f32)) return;
                operand = static_cast<double>(memory_f32);
            } else {
                if (!read_f64(memory_f64)) return;
                operand = memory_f64;
            }
            X87Op arith;
            bool reverse = false;
            switch (op) {
            case Opcode::FADD_M32:
            case Opcode::FADD_M64:  arith = X87Op::Add; break;
            case Opcode::FMUL_M32:
            case Opcode::FMUL_M64:  arith = X87Op::Mul; break;
            case Opcode::FSUB_M32:
            case Opcode::FSUB_M64:  arith = X87Op::Sub; break;
            case Opcode::FSUBR_M32:
            case Opcode::FSUBR_M64: arith = X87Op::Sub; reverse = true; break;
            case Opcode::FDIV_M32:
            case Opcode::FDIV_M64:  arith = X87Op::Div; break;
            default:                arith = X87Op::Div; reverse = true; break;
            }
            // The m32/m64 operand widens to the 80-bit format exactly, so
            // building it here keeps the whole operation in one format and
            // lets arith_x87 round once -- the same contract the interpreter
            // follows, through the same function, so the two cannot drift.
            const Float80 st0 = fpu.read_src(0);
            const Float80 mem = is_m32
                ? fpu.prepare_ieee_source(memory_f32)
                : fpu.prepare_ieee_source(memory_f64);
            const Float80 a = reverse ? mem : st0;
            const Float80 b = reverse ? st0 : mem;
            fpu.set_st(0, fpu.arith_x87(a, b, arith));
            return;
        }
        case Opcode::FCOMP_M32:
            if (!read_f32(memory_f32)) return;
            compare_x87(fpu.read_src(0),
                        fpu.prepare_ieee_source(memory_f32), false);
            fpu.pop();
            return;
        default:
            break;
        }
    } else if (inline_sti && kind == kKindSti) {
        const u8 i = static_cast<u8>(addr & 7);
        switch (op) {
        case Opcode::FLD_STI:
            fpu.push(fpu.read_src(i));
            return;
        case Opcode::FST_STI:
            fpu.set_st(i, fpu.read_src(0));
            return;
        case Opcode::FSTP_STI:
            fpu.set_st(i, fpu.read_src(0));
            fpu.pop();
            return;

        // Intel SDM Vol. 2, FADD/FSUB/FMUL/FDIV: the D8 forms write ST(0),
        // the DC forms write ST(i), and the DE forms write ST(i) then pop.
        // Keep the same double-backed Float80 arithmetic as the interpreter;
        // this path removes only DecodedInstruction/ExecContext reconstruction.
        case Opcode::FADD_STI:
            fpu.set_st(0, fpu.arith_x87(fpu.read_src(0), fpu.read_src(i), X87Op::Add));
            return;
        case Opcode::FSUB_STI:
            fpu.set_st(0, fpu.arith_x87(fpu.read_src(0), fpu.read_src(i), X87Op::Sub));
            return;
        case Opcode::FSUBR_STI:
            fpu.set_st(0, fpu.arith_x87(fpu.read_src(i), fpu.read_src(0), X87Op::Sub));
            return;
        case Opcode::FMUL_STI:
            fpu.set_st(0, fpu.arith_x87(fpu.read_src(0), fpu.read_src(i), X87Op::Mul));
            return;
        case Opcode::FDIV_STI:
            fpu.set_st(0, fpu.arith_x87(fpu.read_src(0), fpu.read_src(i), X87Op::Div));
            return;
        case Opcode::FDIVR_STI:
            fpu.set_st(0, fpu.arith_x87(fpu.read_src(i), fpu.read_src(0), X87Op::Div));
            return;

        case Opcode::FADD_TO_STI:
            fpu.set_st(i, fpu.arith_x87(fpu.read_src(i), fpu.read_src(0), X87Op::Add));
            return;
        case Opcode::FSUB_TO_STI:
            fpu.set_st(i, fpu.arith_x87(fpu.read_src(i), fpu.read_src(0), X87Op::Sub));
            return;
        case Opcode::FSUBR_TO_STI:
            fpu.set_st(i, fpu.arith_x87(fpu.read_src(0), fpu.read_src(i), X87Op::Sub));
            return;
        case Opcode::FMUL_TO_STI:
            fpu.set_st(i, fpu.arith_x87(fpu.read_src(i), fpu.read_src(0), X87Op::Mul));
            return;
        case Opcode::FDIV_TO_STI:
            fpu.set_st(i, fpu.arith_x87(fpu.read_src(i), fpu.read_src(0), X87Op::Div));
            return;
        case Opcode::FDIVR_TO_STI:
            fpu.set_st(i, fpu.arith_x87(fpu.read_src(0), fpu.read_src(i), X87Op::Div));
            return;

        case Opcode::FADDP_STI: {
            const Float80 result = fpu.arith_x87(fpu.read_src(i), fpu.read_src(0), X87Op::Add);
            fpu.set_st(i, result);
            fpu.pop();
            return;
        }
        case Opcode::FSUBP_STI: {
            const Float80 result = fpu.arith_x87(fpu.read_src(i), fpu.read_src(0), X87Op::Sub);
            fpu.set_st(i, result);
            fpu.pop();
            return;
        }
        case Opcode::FSUBRP_STI: {
            const Float80 result = fpu.arith_x87(fpu.read_src(0), fpu.read_src(i), X87Op::Sub);
            fpu.set_st(i, result);
            fpu.pop();
            return;
        }
        case Opcode::FMULP_STI: {
            const Float80 result = fpu.arith_x87(fpu.read_src(i), fpu.read_src(0), X87Op::Mul);
            fpu.set_st(i, result);
            fpu.pop();
            return;
        }
        case Opcode::FDIVP_STI: {
            const Float80 result = fpu.arith_x87(fpu.read_src(i), fpu.read_src(0), X87Op::Div);
            fpu.set_st(i, result);
            fpu.pop();
            return;
        }
        case Opcode::FDIVRP_STI: {
            const Float80 result = fpu.arith_x87(fpu.read_src(0), fpu.read_src(i), X87Op::Div);
            fpu.set_st(i, result);
            fpu.pop();
            return;
        }

        case Opcode::FCOM_STI:
        case Opcode::FUCOM_STI:
            compare_x87(fpu.read_src(0), fpu.read_src(i),
                        op == Opcode::FUCOM_STI);
            return;
        case Opcode::FCOMP_STI:
        case Opcode::FUCOMP_STI:
            compare_x87(fpu.read_src(0), fpu.read_src(i),
                        op == Opcode::FUCOMP_STI);
            fpu.pop();
            return;
        default:
            break;
        }
    } else if (inline_none && kind == kKindNone) {
        switch (op) {
        case Opcode::FNSTSW_AX:
            cpu->gpr.set16(static_cast<u8>(RegIndex::RAX), fpu.status_word);
            return;
        case Opcode::FLD1:
            fpu.push(x87_const::fld1());
            return;
        case Opcode::FLDZ:
            fpu.push(x87_const::fldz());
            return;
        case Opcode::FCHS: {
            Float80 value = fpu.read_src(0);
            value.exponent_sign ^= 0x8000u;
            fpu.set_st(0, value);
            return;
        }
        case Opcode::FABS: {
            Float80 value = fpu.read_src(0);
            value.exponent_sign &= 0x7FFFu;
            fpu.set_st(0, value);
            return;
        }
        default:
            break;
        }
    }

    // FCOMI/FUCOMI write CF/PF/ZF and clear OF/SF/AF, so materialize lazy
    // EFLAGS before the interpreter updates those bits. FCMOV performs
    // its own materialization before consuming flags. Every other x87 opcode
    // leaves integer flags untouched and can keep them lazy across the helper.
    if (op == Opcode::FCOMI_STI || op == Opcode::FCOMIP_STI ||
        op == Opcode::FUCOMI_STI || op == Opcode::FUCOMIP_STI) {
        cpu->materialize_lazy_flags();
    }

    DecodedInstruction di{};
    di.opcode = op;

    // EMMS lives with the MMX/SSE dispatch in the interpreter, not exec_fpu.
    if (op == Opcode::EMMS) {
        cpu->fpu.emms();
        return;
    }

    // kKindM32Value carries the operand's VALUE in `addr`, not an address, so
    // there is no memory operand to rebuild and exec_fpu cannot execute it.
    // Falling through built NO operand at all, which made get_mem_addr fail and
    // turned the instruction into a SILENT NO-OP -- the mechanism behind
    // `fdiv dword[m]` leaving ST(0) untouched. compile_fpu preloads m32 for
    // FLD/FADD/FMUL/FSUB/FSUBR/FDIV/FDIVR/FCOMP (jit/ops/fpu.cpp), so every one
    // of those MUST be handled by the inline switch above; reaching here means
    // that coverage regressed.
    if (kind == kKindM32Value) {
        static std::array<std::atomic<bool>, 2048> s_m32_reported{};
        const usize op_index = static_cast<usize>(op);
        if (op_index < s_m32_reported.size() &&
            !s_m32_reported[op_index].exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr,
                         "[jit-fpu] BUG: preloaded-m32 opcode id=%u has no "
                         "inline case; it cannot be delegated and would be a "
                         "silent no-op\n",
                         static_cast<unsigned>(op));
        }
        return;
    }

    if (kind == kKindMem) {
        // Disp-only addressing: in 32-bit mode calc_effective_address
        // truncates after sign-extending i32, which recovers the original
        // guest EA for any 32-bit address (including those with bit 31 set).
        MemoryOperand m{};
        m.disp = static_cast<i32>(addr);
        // MemUnknown is fine — exec_fpu sizes the access from the opcode.
        di.operands[0] = Operand::make_mem(OperandType::MemUnknown, m);
        di.operand_count = 1;
    } else if (kind == kKindSti) {
        di.operands[0] = Operand::make_imm(OperandType::Imm8, addr & 7);
        di.operand_count = 1;
    }

    ExecContext ctx{*cpu, *mem};
    // Do NOT discard this result. exec_fpu returning anything but Ok means the
    // interpreter declined the synthesized instruction, and dropping that on
    // the floor turns "cannot execute" into "silently did nothing" -- the
    // guest's x87 state simply does not advance, which is indistinguishable
    // from a correct run until a value comes out wrong much later. Report each
    // distinct opcode once so a divergence names itself instead of having to
    // be bisected out of the guest.
    const ExecResult fpu_result = ::vitreo::cpu::ops::exec_fpu(ctx, di);
    if (fpu_result == ExecResult::Exception &&
        ctx.exception == CpuException::FloatingPointStackCheck &&
        !cpu->pending_raise.active) {
        auto& pending = cpu->pending_raise;
        pending.active = true;
        pending.code = cpu_exception_to_status(ctx.exception);
        pending.flags = 0;
        pending.num_args = 1;
        pending.args[0] = 0;
        pending.from_helper = true;
    }
    if (fpu_result != ExecResult::Continue &&
        fpu_result != ExecResult::Exception) {
        static std::array<std::atomic<bool>, 2048> s_reported{};
        const usize op_index = static_cast<usize>(op);
        if (op_index < s_reported.size() &&
            !s_reported[op_index].exchange(true, std::memory_order_relaxed)) {
            const char* name = opcode_name(op);
            std::fprintf(stderr,
                         "[jit-fpu] UNEXECUTED x87 opcode %s (id=%u kind=%u) "
                         "-- interpreter declined; guest FPU state did not "
                         "advance\n",
                         name != nullptr ? name : "?",
                         static_cast<unsigned>(op), static_cast<unsigned>(kind));
        }
    }
}

} // namespace vitreo::cpu::jit
