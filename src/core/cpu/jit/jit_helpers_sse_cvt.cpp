// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_helpers_sse_cvt.cpp
/// @brief MXCSR state and SSE conversion helpers called from JIT code.
///
/// Publishing and discarding the host's SSE sticky exceptions, the SSE
/// rounding-mode install, CVTSI2SS/CVTSI2SD, and jit_sse_cvt for the
/// CVT*/RCPSS/RSQRTSS family.
///
/// Split out of jit_helpers.cpp. Closed in both directions, checked with
/// tools/architecture/plan_extraction.py before moving; the cut is open only
/// by the two promotions declared in jit_helpers_internal.hpp.

#include "jit_helpers.hpp"
#include "core/math/host_fenv.hpp"
#include "../sse_float_semantics.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace vitreo::cpu::jit {

// Publish the SSE class's pending sticky exception bits into MXCSR, for the
// points where the guest can observe it. The interpreter does the same thing
// inline in its STMXCSR/FXSAVE paths; both engines must agree, and both run on
// the same host FPU, so they agree by construction once each publishes.
extern "C" void jit_publish_mxcsr(CpuState* cpu) {
    cpu->sse.mxcsr |= math::harvest_sse_exceptions();
}

// LDMXCSR writes the sticky bits outright, so whatever the host accumulated
// for the SSE class beforehand is superseded.
extern "C" void jit_discard_mxcsr_exceptions(CpuState*) {
    math::discard_sse_exceptions();
}

extern "C" void jit_cvtsi2ss_r64(CpuState* cpu, u32 xmm_dst, u64 src) {
    // Intel SDM Vol. 2A, CVTSI2SS: convert signed integer to scalar single.
    // Only the destination's low 32 bits change; MXCSR rounding applies.
    // Default MXCSR (0x1F80) is round-to-nearest-even, matching C++ float cast
    // on IEEE hosts. EFLAGS unchanged.
    const float f = static_cast<float>(static_cast<i64>(src));
    sse_cvt_int_to_f32_pe(static_cast<i64>(src), f, cpu->sse.mxcsr);
    u32 bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    cpu->sse.xmm[xmm_dst & 15].u32_[0] = bits;
}

extern "C" void jit_cvtsi2sd_r32(CpuState* cpu, u32 xmm_dst, u32 src) {
    // Intel SDM Vol. 2A, CVTSI2SD with a 32-bit source: convert signed
    // doubleword to scalar double in the low qword; high qword unchanged.
    const double d = static_cast<double>(static_cast<i32>(src));
    u64 bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    cpu->sse.xmm[xmm_dst & 15].u64_[0] = bits;
}

extern "C" void jit_cvtsi2sd_r64(CpuState* cpu, u32 xmm_dst, u64 src) {
    // Intel SDM Vol. 2A, CVTSI2SD with a 64-bit source: convert signed
    // quadword to scalar double in the low qword; high qword unchanged.
    const double d = static_cast<double>(static_cast<i64>(src));
    u64 bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    cpu->sse.xmm[xmm_dst & 15].u64_[0] = bits;
}

extern "C" void jit_sse_apply_rounding(CpuState* cpu) {
    math::apply_sse_rounding(cpu->sse.mxcsr);
}

namespace {

i32 sse_cvt_f32_i32(float val, bool truncate, u32& mxcsr) {
    if (std::isnan(val) || val >= static_cast<float>(INT32_MAX) + 1.0f ||
        val < static_cast<float>(INT32_MIN)) {
        return static_cast<i32>(0x80000000u);
    }
    const i32 r = truncate ? static_cast<i32>(std::truncf(val))
                           : static_cast<i32>(std::lrintf(val));
    sse_cvt_int_pe(val, r, mxcsr);
    return r;
}

i32 sse_cvt_f64_i32(double val, bool truncate, u32& mxcsr) {
    if (std::isnan(val) || val >= static_cast<double>(INT32_MAX) + 1.0 ||
        val < static_cast<double>(INT32_MIN)) {
        return static_cast<i32>(0x80000000u);
    }
    const i32 r = truncate ? static_cast<i32>(std::trunc(val))
                           : static_cast<i32>(std::lrint(val));
    sse_cvt_int_pe(val, r, mxcsr);
    return r;
}

} // namespace

extern "C" void jit_sse_cvt(CpuState* cpu, GuestMemory* mem, u32 kind,
                            u32 dst, u64 src) {
    // Intel SDM Vol. 2 CVT*/RCPSS/RSQRTSS. kind selects the conversion;
    // bit 8 marks a 16-byte memory source at `src` for packed forms.
    constexpr u32 kMem = 0x100u;
    const bool is_mem = (kind & kMem) != 0;
    kind &= ~kMem;
    auto load_f32 = [&](float& value) -> bool {
        u32 bits = static_cast<u32>(src);
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    };
    auto load_f64 = [&](double& value) -> bool {
        std::memcpy(&value, &src, sizeof(value));
        return true;
    };
    auto load_pack = [&](u32 (&lanes)[4]) -> bool {
        if (is_mem) {
            const auto lo = mem->read_u64(static_cast<GuestAddr>(src));
            const auto hi = mem->read_u64(static_cast<GuestAddr>(src) + 8);
            if (!lo.is_ok() || !hi.is_ok()) return false;
            std::memcpy(&lanes[0], &lo.value(), 8);
            std::memcpy(&lanes[2], &hi.value(), 8);
            return true;
        }
        const auto& xmm = cpu->sse.xmm[src & 15];
        lanes[0] = xmm.u32_[0];
        lanes[1] = xmm.u32_[1];
        lanes[2] = xmm.u32_[2];
        lanes[3] = xmm.u32_[3];
        return true;
    };

    switch (kind) {
    case 0: { // CVTSI2SS
        const float f = static_cast<float>(static_cast<i32>(src));
        sse_cvt_int_to_f32_pe(static_cast<i32>(src), f, cpu->sse.mxcsr);
        u32 bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        cpu->sse.xmm[dst & 15].u32_[0] = bits;
        return;
    }
    case 1: { // CVTSI2SD
        const double d = static_cast<double>(static_cast<i32>(src));
        u64 bits = 0;
        std::memcpy(&bits, &d, sizeof(bits));
        cpu->sse.xmm[dst & 15].u64_[0] = bits;
        return;
    }
    case 2:
    case 4: { // CVTSS2SI / CVTTSS2SI
        float value = 0.0f;
        if (!load_f32(value)) return;
        cpu->gpr.set32(static_cast<u8>(dst),
                       static_cast<u32>(sse_cvt_f32_i32(value, kind == 4,
                                                        cpu->sse.mxcsr)));
        return;
    }
    case 3:
    case 5: { // CVTSD2SI / CVTTSD2SI
        double value = 0.0;
        if (!load_f64(value)) return;
        cpu->gpr.set32(static_cast<u8>(dst),
                       static_cast<u32>(sse_cvt_f64_i32(value, kind == 5,
                                                        cpu->sse.mxcsr)));
        return;
    }
    case 6: { // CVTSS2SD
        float value = 0.0f;
        if (!load_f32(value)) return;
        const double d = static_cast<double>(value);
        u64 bits = 0;
        std::memcpy(&bits, &d, sizeof(bits));
        cpu->sse.xmm[dst & 15].u64_[0] = bits;
        return;
    }
    case 7: { // CVTSD2SS
        double value = 0.0;
        if (!load_f64(value)) return;
        const float f = static_cast<float>(value);
        sse_cvt_narrow_pe(value, f, cpu->sse.mxcsr);
        u32 bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        cpu->sse.xmm[dst & 15].u32_[0] = bits;
        return;
    }
    case 8:
    case 10: { // CVTPS2DQ / CVTTPS2DQ
        u32 lanes[4]{};
        if (!load_pack(lanes)) return;
        auto& xmm = cpu->sse.xmm[dst & 15];
        for (int i = 0; i < 4; ++i) {
            float value = 0.0f;
            std::memcpy(&value, &lanes[i], sizeof(value));
            xmm.i32_[i] = sse_cvt_f32_i32(value, kind == 10, cpu->sse.mxcsr);
        }
        return;
    }
    case 9: { // CVTDQ2PS
        u32 lanes[4]{};
        if (!load_pack(lanes)) return;
        auto& xmm = cpu->sse.xmm[dst & 15];
        for (int i = 0; i < 4; ++i) {
            const float f = static_cast<float>(static_cast<i32>(lanes[i]));
            sse_cvt_int_to_f32_pe(static_cast<i32>(lanes[i]), f, cpu->sse.mxcsr);
            u32 bits = 0;
            std::memcpy(&bits, &f, sizeof(bits));
            xmm.u32_[i] = bits;
        }
        return;
    }
    case 11: { // RCPSS
        float value = 0.0f;
        if (!load_f32(value)) return;
        const float r = 1.0f / value;
        u32 bits = 0;
        std::memcpy(&bits, &r, sizeof(bits));
        cpu->sse.xmm[dst & 15].u32_[0] = bits;
        return;
    }
    case 12: { // RSQRTSS
        float value = 0.0f;
        if (!load_f32(value)) return;
        const float r = 1.0f / std::sqrt(value);
        u32 bits = 0;
        std::memcpy(&bits, &r, sizeof(bits));
        cpu->sse.xmm[dst & 15].u32_[0] = bits;
        return;
    }
    default:
        return;
    }
}

} // namespace vitreo::cpu::jit
