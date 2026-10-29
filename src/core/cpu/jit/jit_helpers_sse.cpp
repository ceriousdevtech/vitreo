// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_helpers_sse.cpp
/// @brief jit_sse_op: the generic SSE/AES/SHA/PCLMUL dispatcher called from
/// JIT code.
///
/// jit_sse_op is one switch over op_id. Its AES/SHA/CLMUL natives and its
/// VITREO_JIT_SSE_HIST histogram live in single-includer private headers
/// (jit_helpers_sse_crypto.hpp, jit_helpers_sse_histogram.hpp) so that they
/// stay in this translation unit and inline exactly as before.
///
/// Split out of jit_helpers.cpp. Closed in both directions, checked with
/// tools/architecture/plan_extraction.py before moving; the cut is open only
/// by the two promotions declared in jit_helpers_internal.hpp.

#include "jit_helpers.hpp"
#include "../sse_float_semantics.hpp"
#include "jit_helpers_sse_crypto.hpp"
#include "jit_helpers_sse_histogram.hpp"

#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace vitreo::cpu::jit {

static inline bool diag_logs_enabled() noexcept {
    static const bool enabled = (std::getenv("VITREO_ENABLE_DIAG_LOGS") != nullptr);
    return enabled;
}

void jit_set_sse_helper_histogram_enabled(bool enabled) noexcept {
    g_sse_helper_histogram_enabled.store(enabled, std::memory_order_relaxed);
}

void jit_reset_sse_helper_histogram() noexcept {
    auto& histogram = sse_helper_histogram();
    std::lock_guard<std::mutex> lock(histogram.report_mutex);
    for (auto& count : histogram.counts) {
        count.store(0, std::memory_order_relaxed);
    }
    histogram.overflow.store(0, std::memory_order_relaxed);
    histogram.total.store(0, std::memory_order_relaxed);
    histogram.next_report.store(kSseHelperFirstReport,
                                std::memory_order_relaxed);
}

extern "C" void jit_sse_op(CpuState* cpu, GuestMemory* mem,
                            u32 op_id, u32 xmm_dst, u64 addr_or_src) {
    record_sse_helper_call(op_id);
    auto& sse = cpu->sse;

    switch (op_id) {
    case 0: {
        sse.xmm[xmm_dst & 15] = sse.xmm[addr_or_src & 15];
        return;
    }
    case 1: {
        auto lo = mem->read_u64(static_cast<GuestAddr>(addr_or_src));
        auto hi = mem->read_u64(static_cast<GuestAddr>(addr_or_src + 8));
        if (lo.is_ok() && hi.is_ok()) {
            sse.xmm[xmm_dst & 15].u64_[0] = lo.value();
            sse.xmm[xmm_dst & 15].u64_[1] = hi.value();
        }
        return;
    }
    case 2: {
        auto v = mem->read_u64(static_cast<GuestAddr>(addr_or_src));
        if (v.is_ok()) {
            sse.xmm[xmm_dst & 15].u64_[0] = v.value();
        }
        return;
    }
    case 3: {
        float v0, v1;
        std::memcpy(&v0, &sse.xmm[addr_or_src & 15].u32_[0], sizeof(float));
        std::memcpy(&v1, &sse.xmm[addr_or_src & 15].u32_[1], sizeof(float));
        double d0 = static_cast<double>(v0);
        double d1 = static_cast<double>(v1);
        std::memcpy(&sse.xmm[xmm_dst & 15].u64_[0], &d0, sizeof(double));
        std::memcpy(&sse.xmm[xmm_dst & 15].u64_[1], &d1, sizeof(double));
        return;
    }
    case 4: {
        auto lo = mem->read_u64(static_cast<GuestAddr>(addr_or_src));
        if (!lo.is_ok()) return;
        u64 bits = lo.value();
        float v0, v1;
        u32 lo32 = static_cast<u32>(bits);
        u32 hi32 = static_cast<u32>(bits >> 32);
        std::memcpy(&v0, &lo32, sizeof(float));
        std::memcpy(&v1, &hi32, sizeof(float));
        double d0 = static_cast<double>(v0);
        double d1 = static_cast<double>(v1);
        std::memcpy(&sse.xmm[xmm_dst & 15].u64_[0], &d0, sizeof(double));
        std::memcpy(&sse.xmm[xmm_dst & 15].u64_[1], &d1, sizeof(double));
        return;
    }
    case 5: {
        auto& dst = sse.xmm[xmm_dst & 15];
        dst.clear();
        dst.u32_[0] = static_cast<u32>(cpu->gpr.get64(static_cast<u8>(addr_or_src & 15)));
        return;
    }
    case 6: {
        auto r = mem->read_u32(static_cast<GuestAddr>(addr_or_src));
        if (!r.is_ok()) return;
        auto& dst = sse.xmm[xmm_dst & 15];
        dst.clear();
        dst.u32_[0] = r.value();
        return;
    }
    case 7: {
        cpu->gpr.set32(static_cast<u8>(addr_or_src & 15),
                        sse.xmm[xmm_dst & 15].u32_[0]);
        return;
    }
    case 8: {
        (void)mem->write_u32(static_cast<GuestAddr>(addr_or_src),
                              sse.xmm[xmm_dst & 15].u32_[0]);
        return;
    }
    case 9: {
        auto& dst = sse.xmm[xmm_dst & 15];
        u64 val = sse.xmm[addr_or_src & 15].u64_[0];
        dst.clear();
        dst.u64_[0] = val;
        return;
    }
    case 10: {
        auto r = mem->read_u64(static_cast<GuestAddr>(addr_or_src));
        if (!r.is_ok()) return;
        auto& dst = sse.xmm[xmm_dst & 15];
        dst.clear();
        dst.u64_[0] = r.value();
        return;
    }
    case 11: {
        (void)mem->write_u64(static_cast<GuestAddr>(addr_or_src),
                              sse.xmm[xmm_dst & 15].u64_[0]);
        return;
    }
    case 12: {
        const auto& src = sse.xmm[addr_or_src & 15];
        u32 mask = 0;
        for (u8 i = 0; i < 16; ++i) {
            if (src.u8_[i] & 0x80u) mask |= (1u << i);
        }
        cpu->gpr.set32(static_cast<u8>(xmm_dst & 15), mask);
        return;
    }
    case 13: {
        sse.xmm[xmm_dst & 15].u64_[1] = sse.xmm[addr_or_src & 15].u64_[0];
        return;
    }
    case 14: {
        sse.xmm[xmm_dst & 15] = sse.xmm[addr_or_src & 15];
        return;
    }
    case 15: {
        auto& dst = sse.xmm[xmm_dst & 15];
        auto r = mem->read_bytes(static_cast<GuestAddr>(addr_or_src),
                                 reinterpret_cast<Byte*>(dst.u8_), 16);
        if (!r.is_ok()) return;
        return;
    }
    case 16: {
        sse.xmm[xmm_dst & 15] = sse.xmm[addr_or_src & 15];
        return;
    }
    case 17: {
        const auto& s = sse.xmm[xmm_dst & 15];
        const auto stored = mem->write_bytes(
            static_cast<GuestAddr>(addr_or_src),
            reinterpret_cast<const Byte*>(s.u8_), 16);
        if (!stored.is_ok()) {
            // A discarded failure here is silent data loss: the guest's store
            // executed, nothing landed, and the next read of that memory sees
            // whatever was there before. Real hardware faults instead, so the
            // guest never gets a half-written buffer it believes is complete.
            static std::atomic<u32> reported{0};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 32) {
                std::fprintf(stderr,
                    "[jit-sse] 128-bit store DROPPED addr=0x%llx (write_bytes "
                    "rejected) — guest sees a silent no-op\n",
                    static_cast<unsigned long long>(addr_or_src));
            }
        }
        return;
    }
    case 18: {
        sse.xmm[xmm_dst & 15].u64_[0] = sse.xmm[addr_or_src & 15].u64_[1];
        return;
    }
    case 19: {
        sse.xmm[xmm_dst & 15].u32_[0] = sse.xmm[addr_or_src & 15].u32_[0];
        return;
    }
    case 200: {
        auto r = mem->read_u32(static_cast<GuestAddr>(addr_or_src));
        if (!r.is_ok()) return;
        auto& dst = sse.xmm[xmm_dst & 15];
        dst.clear();
        dst.u32_[0] = r.value();
        return;
    }

    // ---- 500-509: COMISS/COMISD/UCOMISS/UCOMISD/PTEST flag-setters. ----
    // For 500..507: xmm_dst = dst xmm idx; addr_or_src = src xmm idx (reg) or guest addr (mem).
    // 508/509: xmm_dst = dst xmm; addr_or_src = src xmm or guest addr.
    case 500: case 501: case 502: case 503:
    case 504: case 505: case 506: case 507: {
        cpu->materialize_lazy_flags();
        bool is_double = (op_id >= 504);
        bool is_mem    = (op_id & 1) != 0;
        const auto& dst = sse.xmm[xmm_dst & 15];
        bool unord;
        bool gt, lt, eq;
        if (is_double) {
            f64 a, b;
            std::memcpy(&a, &dst.u64_[0], sizeof(f64));
            if (is_mem) {
                auto r = mem->read_u64(static_cast<GuestAddr>(addr_or_src));
                if (!r.is_ok()) return;
                u64 bits = r.value();
                std::memcpy(&b, &bits, sizeof(f64));
            } else {
                std::memcpy(&b, &sse.xmm[addr_or_src & 15].u64_[0], sizeof(f64));
            }
            unord = std::isnan(a) || std::isnan(b);
            gt = !unord && (a > b);
            lt = !unord && (a < b);
            eq = !unord && (a == b);
        } else {
            f32 a, b;
            std::memcpy(&a, &dst.u32_[0], sizeof(f32));
            if (is_mem) {
                auto r = mem->read_u32(static_cast<GuestAddr>(addr_or_src));
                if (!r.is_ok()) return;
                u32 bits = r.value();
                std::memcpy(&b, &bits, sizeof(f32));
            } else {
                std::memcpy(&b, &sse.xmm[addr_or_src & 15].u32_[0], sizeof(f32));
            }
            unord = std::isnan(a) || std::isnan(b);
            gt = !unord && (a > b);
            lt = !unord && (a < b);
            eq = !unord && (a == b);
        }
        cpu->flags.set_of(false);
        cpu->flags.set_sf(false);
        cpu->flags.set_af(false);
        if (unord) {
            cpu->flags.set_zf(true);
            cpu->flags.set_pf(true);
            cpu->flags.set_cf(true);
        } else if (gt) {
            cpu->flags.set_zf(false);
            cpu->flags.set_pf(false);
            cpu->flags.set_cf(false);
        } else if (lt) {
            cpu->flags.set_zf(false);
            cpu->flags.set_pf(false);
            cpu->flags.set_cf(true);
        } else { // eq
            (void)eq;
            cpu->flags.set_zf(true);
            cpu->flags.set_pf(false);
            cpu->flags.set_cf(false);
        }
        cpu->set_flag_op(CpuState::FlagOp::None);
        return;
    }

    // ---- 508 PTEST reg-reg, 509 PTEST reg-mem ----
    case 508: case 509: {
        cpu->materialize_lazy_flags();
        bool is_mem = (op_id == 509);
        const auto& dst = sse.xmm[xmm_dst & 15];
        XmmReg src{};
        if (is_mem) {
            auto lo = mem->read_u64(static_cast<GuestAddr>(addr_or_src));
            auto hi = mem->read_u64(static_cast<GuestAddr>(addr_or_src + 8));
            if (!lo.is_ok() || !hi.is_ok()) return;
            src.u64_[0] = lo.value();
            src.u64_[1] = hi.value();
        } else {
            src = sse.xmm[addr_or_src & 15];
        }
        u64 zf_lo = dst.u64_[0] & src.u64_[0];
        u64 zf_hi = dst.u64_[1] & src.u64_[1];
        u64 cf_lo = (~dst.u64_[0]) & src.u64_[0];
        u64 cf_hi = (~dst.u64_[1]) & src.u64_[1];
        cpu->flags.set_of(false);
        cpu->flags.set_sf(false);
        cpu->flags.set_af(false);
        cpu->flags.set_pf(false);
        cpu->flags.set_zf((zf_lo | zf_hi) == 0);
        cpu->flags.set_cf((cf_lo | cf_hi) == 0);
        cpu->set_flag_op(CpuState::FlagOp::None);
        return;
    }

    // ---- 510-521: MMX bitwise / PADDQ / PUNPCK*DQ helpers. ----
    // xmm_dst encodes mm dst index (low 3 bits); addr_or_src is mm src idx
    // for reg variants (low 3 bits) or guest addr for mem variants.
    case 510: case 511: case 512: case 513:
    case 514: case 515: case 516: case 517:
    case 518: case 519: case 520: case 521: {
        u32 op_kind = (op_id - 510) >> 1;   // 0=PAND 1=POR 2=PXOR 3=PADDQ 4=LDQ 5=HDQ
        bool is_mem = ((op_id - 510) & 1) != 0;
        u8 dst_idx = static_cast<u8>(xmm_dst & 7);
        u64 dst = cpu->fpu.st[dst_idx].mantissa;
        u64 src;
        if (is_mem) {
            auto r = mem->read_u64(static_cast<GuestAddr>(addr_or_src));
            if (!r.is_ok()) return;
            src = r.value();
        } else {
            src = cpu->fpu.st[addr_or_src & 7].mantissa;
        }
        u64 result = 0;
        switch (op_kind) {
        case 0: result = dst & src; break;
        case 1: result = dst | src; break;
        case 2: result = dst ^ src; break;
        case 3: result = dst + src; break;
        case 4: { // PUNPCKLDQ
            u32 d_lo = static_cast<u32>(dst);
            u32 s_lo = static_cast<u32>(src);
            result = static_cast<u64>(d_lo) | (static_cast<u64>(s_lo) << 32);
            break;
        }
        case 5: { // PUNPCKHDQ
            u32 d_hi = static_cast<u32>(dst >> 32);
            u32 s_hi = static_cast<u32>(src >> 32);
            result = static_cast<u64>(d_hi) | (static_cast<u64>(s_hi) << 32);
            break;
        }
        }
        cpu->fpu.st[dst_idx].mantissa = result;
        return;
    }

    default: break;
    }

    // Standard 128-bit source ops (op_id 20-95)
    if (op_id >= 20 && op_id <= 95) {
        bool is_mem = (op_id & 1);
        u32 op_type = (op_id - 20) >> 1;

        XmmReg src{};
        if (is_mem) {
            auto lo = mem->read_u64(static_cast<GuestAddr>(addr_or_src));
            auto hi = mem->read_u64(static_cast<GuestAddr>(addr_or_src + 8));
            if (!lo.is_ok() || !hi.is_ok()) return;
            src.u64_[0] = lo.value();
            src.u64_[1] = hi.value();
        } else {
            src = sse.xmm[addr_or_src & 15];
        }

        auto& dst = sse.xmm[xmm_dst & 15];

        switch (op_type) {
        case 0:  dst.u64_[0] ^= src.u64_[0]; dst.u64_[1] ^= src.u64_[1]; break;
        case 1:  dst.u64_[0] &= src.u64_[0]; dst.u64_[1] &= src.u64_[1]; break;
        case 2:  dst.u64_[0] |= src.u64_[0]; dst.u64_[1] |= src.u64_[1]; break;
        case 3:
            dst.u64_[0] = ~dst.u64_[0] & src.u64_[0];
            dst.u64_[1] = ~dst.u64_[1] & src.u64_[1]; break;
        case 4:  for (int i = 0; i < 16; ++i) dst.u8_[i] += src.u8_[i]; break;
        case 5:  for (int i = 0; i < 8; ++i) dst.u16_[i] += src.u16_[i]; break;
        case 6:  for (int i = 0; i < 4; ++i) dst.u32_[i] += src.u32_[i]; break;
        case 7:  dst.u64_[0] += src.u64_[0]; dst.u64_[1] += src.u64_[1]; break;
        case 8:  for (int i = 0; i < 16; ++i) dst.u8_[i] -= src.u8_[i]; break;
        case 9:  for (int i = 0; i < 8; ++i) dst.u16_[i] -= src.u16_[i]; break;
        case 10: for (int i = 0; i < 4; ++i) dst.u32_[i] -= src.u32_[i]; break;
        case 11: dst.u64_[0] -= src.u64_[0]; dst.u64_[1] -= src.u64_[1]; break;
        case 12: {
            u64 lo = static_cast<u64>(dst.u32_[0]) * static_cast<u64>(src.u32_[0]);
            u64 hi = static_cast<u64>(dst.u32_[2]) * static_cast<u64>(src.u32_[2]);
            dst.u64_[0] = lo; dst.u64_[1] = hi; break;
        }
        case 13:
            for (int i = 0; i < 16; ++i)
                dst.u8_[i] = (dst.u8_[i] == src.u8_[i]) ? 0xFF : 0x00;
            break;
        case 14:
            for (int i = 0; i < 8; ++i)
                dst.u16_[i] = (dst.u16_[i] == src.u16_[i]) ? 0xFFFF : 0x0000;
            break;
        case 15:
            for (int i = 0; i < 4; ++i)
                dst.u32_[i] = (dst.u32_[i] == src.u32_[i]) ? 0xFFFFFFFFu : 0u;
            break;
        case 16:
            for (int i = 0; i < 16; ++i)
                dst.u8_[i] = (dst.i8_[i] > src.i8_[i]) ? 0xFF : 0x00;
            break;
        case 17:
            for (int i = 0; i < 8; ++i)
                dst.u16_[i] = (dst.i16_[i] > src.i16_[i]) ? 0xFFFF : 0x0000;
            break;
        case 18:
            for (int i = 0; i < 4; ++i)
                dst.u32_[i] = (dst.i32_[i] > src.i32_[i]) ? 0xFFFFFFFFu : 0u;
            break;
        case 19:
            for (int i = 0; i < 8; ++i) {
                i32 p = static_cast<i32>(dst.i16_[i]) * static_cast<i32>(src.i16_[i]);
                dst.i16_[i] = static_cast<i16>(p & 0xFFFF);
            }
            break;
        case 20:
            for (int i = 0; i < 8; ++i) {
                i32 p = static_cast<i32>(dst.i16_[i]) * static_cast<i32>(src.i16_[i]);
                dst.i16_[i] = static_cast<i16>((p >> 16) & 0xFFFF);
            }
            break;
        case 21:
            for (int i = 0; i < 8; ++i) {
                u32 p = static_cast<u32>(dst.u16_[i]) * static_cast<u32>(src.u16_[i]);
                dst.u16_[i] = static_cast<u16>((p >> 16) & 0xFFFF);
            }
            break;
        case 22:
            for (int i = 0; i < 4; ++i) {
                i32 lo = static_cast<i32>(dst.i16_[2*i]) * static_cast<i32>(src.i16_[2*i]);
                i32 hi = static_cast<i32>(dst.i16_[2*i+1]) * static_cast<i32>(src.i16_[2*i+1]);
                dst.i32_[i] = lo + hi;
            }
            break;
        case 23:
            for (int i = 0; i < 16; ++i)
                dst.u8_[i] = (dst.u8_[i] < src.u8_[i]) ? dst.u8_[i] : src.u8_[i];
            break;
        case 24:
            for (int i = 0; i < 16; ++i)
                dst.u8_[i] = (dst.u8_[i] > src.u8_[i]) ? dst.u8_[i] : src.u8_[i];
            break;
        case 25:
            for (int i = 0; i < 8; ++i)
                dst.i16_[i] = (dst.i16_[i] < src.i16_[i]) ? dst.i16_[i] : src.i16_[i];
            break;
        case 26:
            for (int i = 0; i < 8; ++i)
                dst.i16_[i] = (dst.i16_[i] > src.i16_[i]) ? dst.i16_[i] : src.i16_[i];
            break;
        case 27:
            for (int i = 0; i < 16; ++i) {
                u16 sum = static_cast<u16>(dst.u8_[i]) + static_cast<u16>(src.u8_[i]) + 1;
                dst.u8_[i] = static_cast<u8>(sum >> 1);
            }
            break;
        case 28:
            for (int i = 0; i < 8; ++i) {
                u32 sum = static_cast<u32>(dst.u16_[i]) + static_cast<u32>(src.u16_[i]) + 1;
                dst.u16_[i] = static_cast<u16>(sum >> 1);
            }
            break;
        case 29: {
            u64 sad0 = 0;
            for (int i = 0; i < 8; ++i) {
                i16 d = static_cast<i16>(dst.u8_[i]) - static_cast<i16>(src.u8_[i]);
                sad0 += static_cast<u64>(d < 0 ? -d : d);
            }
            u64 sad1 = 0;
            for (int i = 8; i < 16; ++i) {
                i16 d = static_cast<i16>(dst.u8_[i]) - static_cast<i16>(src.u8_[i]);
                sad1 += static_cast<u64>(d < 0 ? -d : d);
            }
            dst.u64_[0] = sad0; dst.u64_[1] = sad1; break;
        }
        case 30:
            for (int i = 0; i < 16; ++i) {
                u16 sum = static_cast<u16>(dst.u8_[i]) + static_cast<u16>(src.u8_[i]);
                dst.u8_[i] = (sum > 255) ? 255 : static_cast<u8>(sum);
            }
            break;
        case 31:
            for (int i = 0; i < 8; ++i) {
                u32 sum = static_cast<u32>(dst.u16_[i]) + static_cast<u32>(src.u16_[i]);
                dst.u16_[i] = (sum > 65535) ? 65535 : static_cast<u16>(sum);
            }
            break;
        case 32:
            for (int i = 0; i < 16; ++i) {
                i16 diff = static_cast<i16>(dst.u8_[i]) - static_cast<i16>(src.u8_[i]);
                dst.u8_[i] = (diff < 0) ? 0 : static_cast<u8>(diff);
            }
            break;
        case 33:
            for (int i = 0; i < 8; ++i) {
                i32 diff = static_cast<i32>(dst.u16_[i]) - static_cast<i32>(src.u16_[i]);
                dst.u16_[i] = (diff < 0) ? 0 : static_cast<u16>(diff);
            }
            break;
        case 34: dst.u64_[0] ^= src.u64_[0]; dst.u64_[1] ^= src.u64_[1]; break;
        case 35: dst.u64_[0] &= src.u64_[0]; dst.u64_[1] &= src.u64_[1]; break;
        case 36: dst.u64_[0] |= src.u64_[0]; dst.u64_[1] |= src.u64_[1]; break;
        case 37:
            dst.u64_[0] = ~dst.u64_[0] & src.u64_[0];
            dst.u64_[1] = ~dst.u64_[1] & src.u64_[1]; break;
        }
        return;
    }

    // Packed FP arithmetic (op_id 300-327): 14 ops × {reg,mem}.
    // Memory form, huge-offset fallback, and Inf/NaN slow path from the
    // gated native NEON emitter. ADD/SUB/MUL use sse_* (not host operators).
    if (op_id >= 300 && op_id <= 327) {
        bool is_mem = (op_id & 1) != 0;
        u32 fp_type = (op_id - 300) >> 1;   // 0..13

        XmmReg src{};
        if (is_mem) {
            auto lo = mem->read_u64(static_cast<GuestAddr>(addr_or_src));
            auto hi = mem->read_u64(static_cast<GuestAddr>(addr_or_src + 8));
            if (!lo.is_ok() || !hi.is_ok()) return;
            src.u64_[0] = lo.value();
            src.u64_[1] = hi.value();
        } else {
            src = sse.xmm[addr_or_src & 15];
        }
        auto& dst = sse.xmm[xmm_dst & 15];

        float  fd_f[4], fs_f[4];
        double fd_d[2], fs_d[2];
        std::memcpy(fd_f, dst.u32_, 16); std::memcpy(fs_f, src.u32_, 16);
        std::memcpy(fd_d, dst.u64_, 16); std::memcpy(fs_d, src.u64_, 16);

        switch (fp_type) {
        // ADD/SUB/MUL use sse_* so Inf/NaN match Intel SDM Vol. 1 §4.8.3.4
        // (destination-first quieting / indefinite QNaN). Raw host operators
        // diverge on ARM (wrong NaN order / unsigned indefinite).
        case 0:  for (int i=0;i<4;++i) fd_f[i] = sse_add(fd_f[i], fs_f[i]); break; // ADDPS
        case 1:  for (int i=0;i<4;++i) fd_f[i] = sse_sub(fd_f[i], fs_f[i]); break; // SUBPS
        case 2:  for (int i=0;i<4;++i) fd_f[i] = sse_mul(fd_f[i], fs_f[i]); break; // MULPS
        case 3:  for (int i=0;i<4;++i)
                     fd_f[i] = sse_div(fd_f[i], fs_f[i], sse.mxcsr);         break; // DIVPS
        case 4:  for (int i=0;i<4;++i) fd_f[i] = sse_min(fd_f[i], fs_f[i], sse.mxcsr); break; // MINPS
        case 5:  for (int i=0;i<4;++i) fd_f[i] = sse_max(fd_f[i], fs_f[i], sse.mxcsr); break; // MAXPS
        case 6:  for (int i=0;i<4;++i) fd_f[i] = std::sqrt(fs_f[i]);         break; // SQRTPS
        case 7:  for (int i=0;i<2;++i) fd_d[i] = sse_add(fd_d[i], fs_d[i]); break; // ADDPD
        case 8:  for (int i=0;i<2;++i) fd_d[i] = sse_sub(fd_d[i], fs_d[i]); break; // SUBPD
        case 9:  for (int i=0;i<2;++i) fd_d[i] = sse_mul(fd_d[i], fs_d[i]); break; // MULPD
        case 10: for (int i=0;i<2;++i)
                     fd_d[i] = sse_div(fd_d[i], fs_d[i], sse.mxcsr);         break; // DIVPD
        case 11: for (int i=0;i<2;++i) fd_d[i] = sse_min(fd_d[i], fs_d[i], sse.mxcsr); break; // MINPD
        case 12: for (int i=0;i<2;++i) fd_d[i] = sse_max(fd_d[i], fs_d[i], sse.mxcsr); break; // MAXPD
        case 13: for (int i=0;i<2;++i) fd_d[i] = std::sqrt(fs_d[i]);         break; // SQRTPD
        }
        if (fp_type < 7) {
            std::memcpy(dst.u32_, fd_f, 16);
        } else {
            std::memcpy(dst.u64_, fd_d, 16);
        }
        return;
    }

    // SSE4.1/4.2 integer compare/multiply/min-max (op_id 330-351): 11 ops × {reg,mem}.
    // Same mem-fallback / unreachable-huge-offset pattern as the FP block above.
    if (op_id >= 330 && op_id <= 351) {
        bool is_mem = (op_id & 1) != 0;
        u32 op_type = (op_id - 330) >> 1;   // 0..10

        XmmReg src{};
        if (is_mem) {
            auto lo = mem->read_u64(static_cast<GuestAddr>(addr_or_src));
            auto hi = mem->read_u64(static_cast<GuestAddr>(addr_or_src + 8));
            if (!lo.is_ok() || !hi.is_ok()) return;
            src.u64_[0] = lo.value();
            src.u64_[1] = hi.value();
        } else {
            src = sse.xmm[addr_or_src & 15];
        }
        auto& dst = sse.xmm[xmm_dst & 15];

        switch (op_type) {
        case 0: // PCMPEQQ
            for (int i = 0; i < 2; ++i)
                dst.u64_[i] = (dst.u64_[i] == src.u64_[i]) ? ~u64(0) : 0;
            break;
        case 1: // PCMPGTQ
            for (int i = 0; i < 2; ++i)
                dst.u64_[i] = (static_cast<i64>(dst.u64_[i]) >
                               static_cast<i64>(src.u64_[i])) ? ~u64(0) : 0;
            break;
        case 2: // PMULLD
            for (int i = 0; i < 4; ++i)
                dst.u32_[i] = static_cast<u32>(
                    static_cast<i32>(dst.u32_[i]) *
                    static_cast<i32>(src.u32_[i]));
            break;
        case 3: // PMINSB
            for (int i = 0; i < 16; ++i)
                dst.i8_[i] = std::min(dst.i8_[i], src.i8_[i]);
            break;
        case 4: // PMINSD
            for (int i = 0; i < 4; ++i)
                dst.i32_[i] = std::min(dst.i32_[i], src.i32_[i]);
            break;
        case 5: // PMINUW
            for (int i = 0; i < 8; ++i)
                dst.u16_[i] = std::min(dst.u16_[i], src.u16_[i]);
            break;
        case 6: // PMINUD
            for (int i = 0; i < 4; ++i)
                dst.u32_[i] = std::min(dst.u32_[i], src.u32_[i]);
            break;
        case 7: // PMAXSB
            for (int i = 0; i < 16; ++i)
                dst.i8_[i] = std::max(dst.i8_[i], src.i8_[i]);
            break;
        case 8: // PMAXSD
            for (int i = 0; i < 4; ++i)
                dst.i32_[i] = std::max(dst.i32_[i], src.i32_[i]);
            break;
        case 9: // PMAXUW
            for (int i = 0; i < 8; ++i)
                dst.u16_[i] = std::max(dst.u16_[i], src.u16_[i]);
            break;
        case 10: // PMAXUD
            for (int i = 0; i < 4; ++i)
                dst.u32_[i] = std::max(dst.u32_[i], src.u32_[i]);
            break;
        }
        return;
    }

    // PMOVSX / PMOVZX (op_id 360-383): 12 ops × {reg, mem}.
    // Sign/zero extend a sub-quadword of src into a full 128-bit dst.
    if (op_id >= 360 && op_id <= 383) {
        bool is_mem = (op_id & 1) != 0;
        u32 op_type = (op_id - 360) >> 1;   // 0..11

        // Widths read: BW/WD/DQ: 8 bytes; BD/WQ: 4 bytes; BQ: 2 bytes.
        auto width_bytes = [](u32 t) -> u32 {
            switch (t % 6) {
            case 0: return 8;  // BW
            case 1: return 4;  // BD
            case 2: return 2;  // BQ
            case 3: return 8;  // WD
            case 4: return 4;  // WQ
            case 5: return 8;  // DQ
            }
            return 0;
        };

        u8 raw[8] = {};
        u32 wb = width_bytes(op_type);
        if (is_mem) {
            auto a = static_cast<GuestAddr>(addr_or_src);
            if (wb == 8) {
                auto r = mem->read_u64(a);
                if (!r.is_ok()) return;
                const u64 v = r.value();
                std::memcpy(raw, &v, 8);
            } else if (wb == 4) {
                auto r = mem->read_u32(a);
                if (!r.is_ok()) return;
                const u32 v = r.value();
                std::memcpy(raw, &v, 4);
            } else {
                auto r = mem->read_u16(a);
                if (!r.is_ok()) return;
                const u16 v = r.value();
                std::memcpy(raw, &v, 2);
            }
        } else {
            const auto& s = sse.xmm[addr_or_src & 15];
            for (u32 i = 0; i < wb; ++i) raw[i] = s.u8_[i];
        }

        auto& dst = sse.xmm[xmm_dst & 15];
        dst.clear();
        bool is_signed = (op_type < 6);
        u32 kind = op_type % 6;

        auto extend8  = [&](int n, int out_bytes) {
            for (int i = 0; i < n; ++i) {
                u64 v = raw[i];
                if (is_signed && (v & 0x80)) v |= ~u64(0xFF);
                switch (out_bytes) {
                case 2: dst.u16_[i] = static_cast<u16>(v); break;
                case 4: dst.u32_[i] = static_cast<u32>(v); break;
                case 8: dst.u64_[i] = v;                   break;
                }
            }
        };
        auto extend16 = [&](int n, int out_bytes) {
            for (int i = 0; i < n; ++i) {
                u64 v = static_cast<u16>(raw[2*i] | (raw[2*i+1] << 8));
                if (is_signed && (v & 0x8000)) v |= ~u64(0xFFFF);
                switch (out_bytes) {
                case 4: dst.u32_[i] = static_cast<u32>(v); break;
                case 8: dst.u64_[i] = v;                   break;
                }
            }
        };
        auto extend32 = [&](int n) {
            for (int i = 0; i < n; ++i) {
                u64 v = static_cast<u32>(raw[4*i] | (raw[4*i+1] << 8)
                                        | (raw[4*i+2] << 16) | (raw[4*i+3] << 24));
                if (is_signed && (v & 0x80000000u)) v |= ~u64(0xFFFFFFFFu);
                dst.u64_[i] = v;
            }
        };

        switch (kind) {
        case 0: extend8(8,  2); break; // BW
        case 1: extend8(4,  4); break; // BD
        case 2: extend8(2,  8); break; // BQ
        case 3: extend16(4, 4); break; // WD
        case 4: extend16(2, 8); break; // WQ
        case 5: extend32(2);    break; // DQ
        }
        return;
    }

    // Scalar FP (op_id 400-427): 14 ops × {reg, mem}.
    // Preserves upper lanes of dst; only lane 0 is updated.
    // Types (0..13):
    //   0 ADDSS  1 ADDSD  2 SUBSS  3 SUBSD  4 MULSS  5 MULSD
    //   6 DIVSS  7 DIVSD  8 MINSS  9 MINSD 10 MAXSS 11 MAXSD
    //  12 SQRTSS 13 SQRTSD
    if (op_id >= 400 && op_id <= 427) {
        bool is_mem = (op_id & 1) != 0;
        u32 op_type = (op_id - 400) >> 1;
        bool is_double = (op_type & 1) != 0;

        auto& dst = sse.xmm[xmm_dst & 15];
        double s1d = 0.0;
        float  s1f = 0.0f;
        if (is_mem) {
            auto a = static_cast<GuestAddr>(addr_or_src);
            if (is_double) {
                auto r = mem->read_u64(a);
                if (!r.is_ok()) return;
                const u64 v = r.value();
                std::memcpy(&s1d, &v, 8);
            } else {
                auto r = mem->read_u32(a);
                if (!r.is_ok()) return;
                const u32 v = r.value();
                std::memcpy(&s1f, &v, 4);
            }
        } else {
            const auto& src = sse.xmm[addr_or_src & 15];
            if (is_double) std::memcpy(&s1d, &src.u64_[0], 8);
            else           std::memcpy(&s1f, &src.u32_[0], 4);
        }

        if (is_double) {
            double s0;
            std::memcpy(&s0, &dst.u64_[0], 8);
            double r = 0.0;
            switch (op_type) {
            case 1:  r = sse_add(s0, s1d); break;  // ADDSD
            case 3:  r = sse_sub(s0, s1d); break;  // SUBSD
            case 5:  r = sse_mul(s0, s1d); break;  // MULSD
            case 7:  r = sse_div(s0, s1d, sse.mxcsr); break;  // DIVSD
            case 9:  r = sse_min(s0, s1d, sse.mxcsr); break; // MINSD
            case 11: r = sse_max(s0, s1d, sse.mxcsr); break; // MAXSD
            case 13: r = std::sqrt(s1d); break;     // SQRTSD (1-src, src=s1d)
            }
            std::memcpy(&dst.u64_[0], &r, 8);
        } else {
            float s0;
            std::memcpy(&s0, &dst.u32_[0], 4);
            float r = 0.0f;
            switch (op_type) {
            case 0:  r = sse_add(s0, s1f); break;  // ADDSS
            case 2:  r = sse_sub(s0, s1f); break;  // SUBSS
            case 4:  r = sse_mul(s0, s1f); break;  // MULSS
            case 6:  r = sse_div(s0, s1f, sse.mxcsr); break;  // DIVSS
            case 8:  r = sse_min(s0, s1f, sse.mxcsr); break; // MINSS
            case 10: r = sse_max(s0, s1f, sse.mxcsr); break; // MAXSS
            case 12: r = std::sqrt(s1f); break;     // SQRTSS
            }
            std::memcpy(&dst.u32_[0], &r, 4);
        }
        return;
    }

    // Shift by imm8 (op_id 100-109)
    if (op_id >= 100 && op_id <= 109) {
        auto& xmm = sse.xmm[xmm_dst & 15];
        u8 shift = static_cast<u8>(addr_or_src);
        switch (op_id) {
        case 100:
            if (shift > 15) { for (int i=0;i<8;++i) xmm.u16_[i]=0; }
            else { for (int i=0;i<8;++i) xmm.u16_[i]<<=shift; }
            break;
        case 101:
            if (shift > 31) { for (int i=0;i<4;++i) xmm.u32_[i]=0; }
            else { for (int i=0;i<4;++i) xmm.u32_[i]<<=shift; }
            break;
        case 102:
            if (shift > 63) { xmm.u64_[0]=0; xmm.u64_[1]=0; }
            else { xmm.u64_[0]<<=shift; xmm.u64_[1]<<=shift; }
            break;
        case 103:
            if (shift > 15) { xmm.clear(); }
            else {
                for (int i=15; i>=shift; --i) xmm.u8_[i] = xmm.u8_[i-shift];
                for (int i=shift-1; i>=0; --i) xmm.u8_[i] = 0;
            }
            break;
        case 104:
            if (shift > 15) { for (int i=0;i<8;++i) xmm.u16_[i]=0; }
            else { for (int i=0;i<8;++i) xmm.u16_[i]>>=shift; }
            break;
        case 105:
            if (shift > 31) { for (int i=0;i<4;++i) xmm.u32_[i]=0; }
            else { for (int i=0;i<4;++i) xmm.u32_[i]>>=shift; }
            break;
        case 106:
            if (shift > 63) { xmm.u64_[0]=0; xmm.u64_[1]=0; }
            else { xmm.u64_[0]>>=shift; xmm.u64_[1]>>=shift; }
            break;
        case 107:
            if (shift > 15) { xmm.clear(); }
            else {
                for (int i=0; i<16-shift; ++i) xmm.u8_[i] = xmm.u8_[i+shift];
                for (int i=16-shift; i<16; ++i) xmm.u8_[i] = 0;
            }
            break;
        case 108:
            if (shift > 15) shift = 15;
            for (int i=0;i<8;++i) xmm.i16_[i]>>=shift;
            break;
        case 109:
            if (shift > 31) shift = 31;
            for (int i=0;i<4;++i) xmm.i32_[i]>>=shift;
            break;
        }
        return;
    }

    // Shift by register (op_id 120-135)
    if (op_id >= 120 && op_id <= 135) {
        bool is_mem = (op_id & 1);
        u32 stype = (op_id - 120) >> 1;
        u64 count;
        if (is_mem) {
            auto r = mem->read_u64(static_cast<GuestAddr>(addr_or_src));
            if (!r.is_ok()) return;
            count = r.value();
        } else {
            count = sse.xmm[addr_or_src & 15].u64_[0];
        }
        auto& dst = sse.xmm[xmm_dst & 15];
        switch (stype) {
        case 0:
            if (count>15) { for (int i=0;i<8;++i) dst.u16_[i]=0; }
            else { for (int i=0;i<8;++i) dst.u16_[i]<<=count; }
            break;
        case 1:
            if (count>31) { for (int i=0;i<4;++i) dst.u32_[i]=0; }
            else { for (int i=0;i<4;++i) dst.u32_[i]<<=count; }
            break;
        case 2:
            if (count>63) { dst.u64_[0]=0; dst.u64_[1]=0; }
            else { dst.u64_[0]<<=count; dst.u64_[1]<<=count; }
            break;
        case 3:
            if (count>15) { for (int i=0;i<8;++i) dst.u16_[i]=0; }
            else { for (int i=0;i<8;++i) dst.u16_[i]>>=count; }
            break;
        case 4:
            if (count>31) { for (int i=0;i<4;++i) dst.u32_[i]=0; }
            else { for (int i=0;i<4;++i) dst.u32_[i]>>=count; }
            break;
        case 5:
            if (count>63) { dst.u64_[0]=0; dst.u64_[1]=0; }
            else { dst.u64_[0]>>=count; dst.u64_[1]>>=count; }
            break;
        case 6:
            { u64 c = (count > 15) ? 15 : count;
              for (int i=0;i<8;++i) dst.i16_[i]>>=c; }
            break;
        case 7:
            { u64 c = (count > 31) ? 31 : count;
              for (int i=0;i<4;++i) dst.i32_[i]>>=c; }
            break;
        }
        return;
    }

    // PSHUFD / PSHUFLW / PSHUFHW / SHUFPS / SHUFPD (op_id 140-149)
    if (op_id >= 140 && op_id <= 149) {
        u8 dst_idx, imm;
        XmmReg src{};
        bool is_mem = (op_id & 1);
        if (is_mem) {
            dst_idx = static_cast<u8>(xmm_dst & 0xFF) & 15;
            imm = static_cast<u8>((xmm_dst >> 8) & 0xFF);
            auto lo = mem->read_u64(static_cast<GuestAddr>(addr_or_src));
            auto hi = mem->read_u64(static_cast<GuestAddr>(addr_or_src + 8));
            if (!lo.is_ok() || !hi.is_ok()) return;
            src.u64_[0] = lo.value(); src.u64_[1] = hi.value();
        } else {
            dst_idx = xmm_dst & 15;
            imm = static_cast<u8>((addr_or_src >> 8) & 0xFF);
            src = sse.xmm[addr_or_src & 15];
        }
        auto& dst = sse.xmm[dst_idx];
        u32 stype = (op_id - 140) >> 1;
        switch (stype) {
        case 0: {
            XmmReg result{};
            result.u32_[0] = src.u32_[(imm >> 0) & 3];
            result.u32_[1] = src.u32_[(imm >> 2) & 3];
            result.u32_[2] = src.u32_[(imm >> 4) & 3];
            result.u32_[3] = src.u32_[(imm >> 6) & 3];
            dst = result; break;
        }
        case 1: {
            XmmReg result{};
            result.u16_[0] = src.u16_[(imm >> 0) & 3];
            result.u16_[1] = src.u16_[(imm >> 2) & 3];
            result.u16_[2] = src.u16_[(imm >> 4) & 3];
            result.u16_[3] = src.u16_[(imm >> 6) & 3];
            for (int i=4;i<8;++i) result.u16_[i] = src.u16_[i];
            dst = result; break;
        }
        case 2: {
            XmmReg result{};
            for (int i=0;i<4;++i) result.u16_[i] = src.u16_[i];
            result.u16_[4] = src.u16_[4 + ((imm >> 0) & 3)];
            result.u16_[5] = src.u16_[4 + ((imm >> 2) & 3)];
            result.u16_[6] = src.u16_[4 + ((imm >> 4) & 3)];
            result.u16_[7] = src.u16_[4 + ((imm >> 6) & 3)];
            dst = result; break;
        }
        case 3: {
            XmmReg result{};
            result.f32_[0] = dst.f32_[(imm >> 0) & 3];
            result.f32_[1] = dst.f32_[(imm >> 2) & 3];
            result.f32_[2] = src.f32_[(imm >> 4) & 3];
            result.f32_[3] = src.f32_[(imm >> 6) & 3];
            dst = result; break;
        }
        case 4: {
            XmmReg result{};
            result.f64_[0] = dst.f64_[(imm >> 0) & 1];
            result.f64_[1] = src.f64_[(imm >> 1) & 1];
            dst = result; break;
        }
        }
        return;
    }

    // PUNPCK*/UNPCK* (op_id 150-169)
    if (op_id >= 150 && op_id <= 169) {
        bool is_mem = (op_id & 1);
        u32 variant = (op_id - 150) >> 1;
        XmmReg src{};
        if (is_mem) {
            auto lo = mem->read_u64(static_cast<GuestAddr>(addr_or_src));
            auto hi = mem->read_u64(static_cast<GuestAddr>(addr_or_src + 8));
            if (!lo.is_ok() || !hi.is_ok()) return;
            src.u64_[0] = lo.value(); src.u64_[1] = hi.value();
        } else {
            src = sse.xmm[addr_or_src & 15];
        }
        auto& dst = sse.xmm[xmm_dst & 15];
        XmmReg result{};
        switch (variant) {
        case 0:
            for (int i=0;i<8;++i) { result.u8_[2*i]=dst.u8_[i]; result.u8_[2*i+1]=src.u8_[i]; }
            dst = result; break;
        case 1:
            for (int i=0;i<8;++i) { result.u8_[2*i]=dst.u8_[8+i]; result.u8_[2*i+1]=src.u8_[8+i]; }
            dst = result; break;
        case 2:
            for (int i=0;i<4;++i) { result.u16_[2*i]=dst.u16_[i]; result.u16_[2*i+1]=src.u16_[i]; }
            dst = result; break;
        case 3:
            for (int i=0;i<4;++i) { result.u16_[2*i]=dst.u16_[4+i]; result.u16_[2*i+1]=src.u16_[4+i]; }
            dst = result; break;
        case 4:
            result.u32_[0]=dst.u32_[0]; result.u32_[1]=src.u32_[0];
            result.u32_[2]=dst.u32_[1]; result.u32_[3]=src.u32_[1];
            dst = result; break;
        case 5:
            result.u32_[0]=dst.u32_[2]; result.u32_[1]=src.u32_[2];
            result.u32_[2]=dst.u32_[3]; result.u32_[3]=src.u32_[3];
            dst = result; break;
        case 6:
            result.u64_[0]=dst.u64_[0]; result.u64_[1]=src.u64_[0];
            dst = result; break;
        case 7:
            result.u64_[0]=dst.u64_[1]; result.u64_[1]=src.u64_[1];
            dst = result; break;
        case 8:
            result.f32_[0]=dst.f32_[0]; result.f32_[1]=src.f32_[0];
            result.f32_[2]=dst.f32_[1]; result.f32_[3]=src.f32_[1];
            dst = result; break;
        case 9:
            result.f32_[0]=dst.f32_[2]; result.f32_[1]=src.f32_[2];
            result.f32_[2]=dst.f32_[3]; result.f32_[3]=src.f32_[3];
            dst = result; break;
        }
        return;
    }

    // PINSRW / PEXTRW (op_id 190-192)
    if (op_id >= 190 && op_id <= 192) {
        switch (op_id) {
        case 190: {
            u8 dst_idx = static_cast<u8>(xmm_dst & 0xFF) & 15;
            u8 sel = static_cast<u8>((xmm_dst >> 8) & 7);
            u16 word = static_cast<u16>(cpu->gpr.get64(static_cast<u8>(addr_or_src & 15)));
            sse.xmm[dst_idx].u16_[sel] = word;
            break;
        }
        case 191: {
            u8 dst_idx = static_cast<u8>(xmm_dst & 0xFF) & 15;
            u8 sel = static_cast<u8>((xmm_dst >> 8) & 7);
            auto r = mem->read_u16(static_cast<GuestAddr>(addr_or_src));
            if (!r.is_ok()) return;
            sse.xmm[dst_idx].u16_[sel] = static_cast<u16>(r.value());
            break;
        }
        case 192: {
            u8 src_xmm = static_cast<u8>(addr_or_src & 15);
            u8 sel = static_cast<u8>((addr_or_src >> 8) & 7);
            u32 val = sse.xmm[src_xmm].u16_[sel];
            cpu->gpr.set32(static_cast<u8>(xmm_dst & 15), val);
            break;
        }
        }
        return;
    }

    // AES-NI (op_id 240-249) — using ARM64 hardware crypto extensions
    if (op_id >= 240 && op_id <= 249) {
        if (diag_logs_enabled()) {
            static std::atomic<uint64_t> aes_hit_count{0};
            auto c = aes_hit_count.fetch_add(1, std::memory_order_relaxed);
            if ((c & (c - 1)) == 0) {
                fprintf(stderr, "[crypto-diag] AES-NI op_id=%u hit count=%llu\n",
                        op_id, static_cast<unsigned long long>(c + 1));
            }
        }
        bool is_mem = (op_id & 1);
        u32 aes_type = (op_id - 240) >> 1; // 0=AESENC,1=AESENCLAST,2=AESDEC,3=AESDECLAST,4=AESIMC

        XmmReg src{};
        if (is_mem) {
            auto lo = mem->read_u64(static_cast<GuestAddr>(addr_or_src));
            auto hi = mem->read_u64(static_cast<GuestAddr>(addr_or_src + 8));
            if (!lo.is_ok() || !hi.is_ok()) return;
            src.u64_[0] = lo.value();
            src.u64_[1] = hi.value();
        } else {
            src = sse.xmm[addr_or_src & 15];
        }

        auto& dst = sse.xmm[xmm_dst & 15];

#ifdef __aarch64__
        aes_round_native(dst.u8_, src.u8_, aes_type);
#else
        (void)dst; (void)src; (void)aes_type;
#endif
        return;
    }

    // AESKEYGENASSIST (op_id 250/251)
    if (op_id == 250 || op_id == 251) {
        static constexpr u8 aes_sbox[256] = {
            0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
            0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
            0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
            0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
            0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
            0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
            0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
            0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
            0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
            0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
            0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
            0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
            0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
            0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
            0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
            0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
        };
        auto sub_word = [](u32 w) -> u32 {
            return static_cast<u32>(aes_sbox[w & 0xFF]) |
                   (static_cast<u32>(aes_sbox[(w >> 8) & 0xFF]) << 8) |
                   (static_cast<u32>(aes_sbox[(w >> 16) & 0xFF]) << 16) |
                   (static_cast<u32>(aes_sbox[(w >> 24) & 0xFF]) << 24);
        };
        auto rot_word = [](u32 w) -> u32 { return (w >> 8) | (w << 24); };

        XmmReg src{};
        u8 rcon;
        if (op_id == 250) {
            // Register source: addr_or_src = src_xmm | (imm8 << 8)
            src = sse.xmm[addr_or_src & 15];
            rcon = static_cast<u8>((addr_or_src >> 8) & 0xFF);
        } else {
            // Memory source: xmm_dst = dst | (imm8 << 8), addr_or_src = EA
            rcon = static_cast<u8>((xmm_dst >> 8) & 0xFF);
            xmm_dst &= 0xFF;
            auto lo = mem->read_u64(static_cast<GuestAddr>(addr_or_src));
            auto hi = mem->read_u64(static_cast<GuestAddr>(addr_or_src + 8));
            if (!lo.is_ok() || !hi.is_ok()) return;
            src.u64_[0] = lo.value();
            src.u64_[1] = hi.value();
        }

        auto& dst = sse.xmm[xmm_dst & 15];
        u32 x1 = sub_word(src.u32_[1]);
        u32 x3 = sub_word(src.u32_[3]);
        dst.u32_[0] = x1;
        dst.u32_[1] = rot_word(x1) ^ static_cast<u32>(rcon);
        dst.u32_[2] = x3;
        dst.u32_[3] = rot_word(x3) ^ static_cast<u32>(rcon);
        return;
    }

    // PCLMULQDQ (op_id 252/253) — carry-less multiplication
    if (op_id == 252 || op_id == 253) {
        XmmReg src{};
        u8 imm;
        u8 dst_idx;
        if (op_id == 252) {
            // Register source: addr_or_src = src_xmm | (imm8 << 8)
            src = sse.xmm[addr_or_src & 15];
            imm = static_cast<u8>((addr_or_src >> 8) & 0xFF);
            dst_idx = static_cast<u8>(xmm_dst & 15);
        } else {
            // Memory source: xmm_dst = dst | (imm8 << 8), addr_or_src = EA
            imm = static_cast<u8>((xmm_dst >> 8) & 0xFF);
            dst_idx = static_cast<u8>(xmm_dst & 0xFF) & 15;
            auto lo = mem->read_u64(static_cast<GuestAddr>(addr_or_src));
            auto hi = mem->read_u64(static_cast<GuestAddr>(addr_or_src + 8));
            if (!lo.is_ok() || !hi.is_ok()) return;
            src.u64_[0] = lo.value();
            src.u64_[1] = hi.value();
        }

        auto& dst = sse.xmm[dst_idx];
        u64 a = dst.u64_[(imm & 0x01) ? 1 : 0];
        u64 b = src.u64_[(imm & 0x10) ? 1 : 0];

#ifdef __aarch64__
        static const bool force_sw_clmul = std::getenv("VITREO_SW_PCLMUL") != nullptr;
        if (force_sw_clmul) {
            u64 lo_r = 0, hi_r = 0;
            for (int i = 0; i < 64; ++i) {
                if ((b >> i) & 1) {
                    lo_r ^= (a << i);
                    if (i > 0) hi_r ^= (a >> (64 - i));
                }
            }
            dst.u64_[0] = lo_r;
            dst.u64_[1] = hi_r;
        } else {
            clmul_native(a, b, dst.u64_[0], dst.u64_[1]);
        }
#else
        u64 lo_r = 0, hi_r = 0;
        for (int i = 0; i < 64; ++i) {
            if ((b >> i) & 1) {
                lo_r ^= (a << i);
                if (i > 0) hi_r ^= (a >> (64 - i));
            }
        }
        dst.u64_[0] = lo_r;
        dst.u64_[1] = hi_r;
#endif
        return;
    }

    // PSHUFB (op_id 254/255) — SSSE3 packed shuffle bytes.
    // dst[i] = (src[i] & 0x80) ? 0 : dst_orig[src[i] & 0x0F]
    if (op_id == 254 || op_id == 255) {
        bool is_mem = (op_id & 1);
        u8 dst_idx = static_cast<u8>(xmm_dst & 15);
        XmmReg src{};
        if (is_mem) {
            auto lo = mem->read_u64(static_cast<GuestAddr>(addr_or_src));
            auto hi = mem->read_u64(static_cast<GuestAddr>(addr_or_src + 8));
            if (!lo.is_ok() || !hi.is_ok()) return;
            src.u64_[0] = lo.value();
            src.u64_[1] = hi.value();
        } else {
            src = sse.xmm[addr_or_src & 15];
        }
        auto& dst = sse.xmm[dst_idx];
#ifdef __aarch64__
        // PSHUFB with src==dst (e.g. byte-reflect-by-self) needs the
        // table snapshot before any writes; load both into NEON regs first.
        uint8x16_t table = vld1q_u8(dst.u8_);
        uint8x16_t idx   = vld1q_u8(src.u8_);
        uint8x16_t mask  = vdupq_n_u8(0x8F);
        uint8x16_t r     = vqtbl1q_u8(table, vandq_u8(idx, mask));
        vst1q_u8(dst.u8_, r);
#else
        XmmReg orig = dst;
        for (int i = 0; i < 16; ++i) {
            u8 s = src.u8_[i];
            dst.u8_[i] = (s & 0x80) ? 0 : orig.u8_[s & 0x0F];
        }
#endif
        return;
    }

    // SHA-256 (op_id 260-265)
    // 260/261 = SHA256RNDS2 reg/mem (implicit XMM0)
    // 262/263 = SHA256MSG1 reg/mem
    // 264/265 = SHA256MSG2 reg/mem
    if (op_id >= 260 && op_id <= 265) {
        if (diag_logs_enabled()) {
            static std::atomic<uint64_t> sha256_hit_count{0};
            auto c = sha256_hit_count.fetch_add(1, std::memory_order_relaxed);
            if ((c & (c - 1)) == 0) { // power of 2
                fprintf(stderr, "[sha-diag] SHA-256 op_id=%u hit count=%llu\n",
                        op_id, static_cast<unsigned long long>(c + 1));
            }
        }
        bool is_mem = (op_id & 1);
        u32 sha_type = (op_id - 260) >> 1; // 0=RNDS2, 1=MSG1, 2=MSG2

        XmmReg src{};
        u8 dst_idx = static_cast<u8>(xmm_dst & 15);
        if (is_mem) {
            auto lo = mem->read_u64(static_cast<GuestAddr>(addr_or_src));
            auto hi = mem->read_u64(static_cast<GuestAddr>(addr_or_src + 8));
            if (!lo.is_ok() || !hi.is_ok()) return;
            src.u64_[0] = lo.value();
            src.u64_[1] = hi.value();
        } else {
            src = sse.xmm[addr_or_src & 15];
        }

        auto& dst = sse.xmm[dst_idx];
#ifdef __aarch64__
        switch (sha_type) {
        case 0: sha256rnds2_native(dst.u32_, src.u32_, sse.xmm[0].u32_); break;
        case 1: sha256msg1_native(dst.u32_, src.u32_); break;
        case 2: sha256msg2_native(dst.u32_, src.u32_); break;
        }
#else
        // Scalar fallback for non-ARM64 builds
        switch (sha_type) {
        case 0: {
            u32 a = src.u32_[3], b = src.u32_[2], e = src.u32_[1], f = src.u32_[0];
            u32 c = dst.u32_[3], d = dst.u32_[2], g = dst.u32_[1], h = dst.u32_[0];
            auto& wk = sse.xmm[0];
            for (int i = 0; i < 2; ++i) {
                u32 ror6 = (e >> 6) | (e << 26), ror11 = (e >> 11) | (e << 21), ror25 = (e >> 25) | (e << 7);
                u32 t1 = h + (ror6 ^ ror11 ^ ror25) + ((e & f) ^ (~e & g)) + wk.u32_[i];
                u32 ror2 = (a >> 2) | (a << 30), ror13 = (a >> 13) | (a << 19), ror22 = (a >> 22) | (a << 10);
                u32 t2 = (ror2 ^ ror13 ^ ror22) + ((a & b) ^ (a & c) ^ (b & c));
                h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
            }
            dst.u32_[3] = a; dst.u32_[2] = b; dst.u32_[1] = e; dst.u32_[0] = f;
            break;
        }
        case 1: {
            auto small_s0 = [](u32 x) -> u32 { return ((x >> 7) | (x << 25)) ^ ((x >> 18) | (x << 14)) ^ (x >> 3); };
            dst.u32_[0] += small_s0(dst.u32_[1]);
            dst.u32_[1] += small_s0(dst.u32_[2]);
            dst.u32_[2] += small_s0(dst.u32_[3]);
            dst.u32_[3] += small_s0(src.u32_[0]);
            break;
        }
        case 2: {
            auto small_s1 = [](u32 x) -> u32 { return ((x >> 17) | (x << 15)) ^ ((x >> 19) | (x << 13)) ^ (x >> 10); };
            dst.u32_[0] += small_s1(src.u32_[2]);
            dst.u32_[1] += small_s1(src.u32_[3]);
            dst.u32_[2] += small_s1(dst.u32_[0]);
            dst.u32_[3] += small_s1(dst.u32_[1]);
            break;
        }
        }
#endif
        return;
    }

    // SHA-1 (op_id 266-273)
    // 266/267 = SHA1RNDS4 reg/mem (imm8 packed in xmm_dst upper bits)
    // 268/269 = SHA1NEXTE reg/mem
    // 270/271 = SHA1MSG1 reg/mem
    // 272/273 = SHA1MSG2 reg/mem
    if (op_id >= 266 && op_id <= 273) {
        if (diag_logs_enabled()) {
            static std::atomic<uint64_t> sha1_hit_count{0};
            auto c = sha1_hit_count.fetch_add(1, std::memory_order_relaxed);
            if ((c & (c - 1)) == 0) {
                fprintf(stderr, "[sha-diag] SHA-1 op_id=%u hit count=%llu\n",
                        op_id, static_cast<unsigned long long>(c + 1));
            }
        }
        bool is_mem = (op_id & 1);
        u32 sha1_type = (op_id - 266) >> 1; // 0=RNDS4, 1=NEXTE, 2=MSG1, 3=MSG2

        XmmReg src{};
        u8 dst_idx;
        u8 imm8 = 0;

        if (sha1_type == 0) {
            // SHA1RNDS4: imm8 packed in upper bits
            if (is_mem) {
                imm8 = static_cast<u8>((xmm_dst >> 8) & 0xFF);
                dst_idx = static_cast<u8>(xmm_dst & 0xFF) & 15;
                auto lo = mem->read_u64(static_cast<GuestAddr>(addr_or_src));
                auto hi = mem->read_u64(static_cast<GuestAddr>(addr_or_src + 8));
                if (!lo.is_ok() || !hi.is_ok()) return;
                src.u64_[0] = lo.value();
                src.u64_[1] = hi.value();
            } else {
                imm8 = static_cast<u8>((addr_or_src >> 8) & 0xFF);
                src = sse.xmm[addr_or_src & 15];
                dst_idx = static_cast<u8>(xmm_dst & 15);
            }
        } else {
            dst_idx = static_cast<u8>(xmm_dst & 15);
            if (is_mem) {
                auto lo = mem->read_u64(static_cast<GuestAddr>(addr_or_src));
                auto hi = mem->read_u64(static_cast<GuestAddr>(addr_or_src + 8));
                if (!lo.is_ok() || !hi.is_ok()) return;
                src.u64_[0] = lo.value();
                src.u64_[1] = hi.value();
            } else {
                src = sse.xmm[addr_or_src & 15];
            }
        }

        auto& dst = sse.xmm[dst_idx];
#ifdef __aarch64__
        switch (sha1_type) {
        case 0: sha1rnds4_native(dst.u32_, src.u32_, imm8 & 3); break;
        case 1: sha1nexte_native(dst.u32_, src.u32_); break;
        case 2: sha1msg1_native(dst.u32_, src.u32_); break;
        case 3: sha1msg2_native(dst.u32_, src.u32_); break;
        }
#else
        switch (sha1_type) {
        case 0: {
            u32 a = dst.u32_[3], b = dst.u32_[2], c = dst.u32_[1], d = dst.u32_[0];
            u8 fn = imm8 & 3;
            for (int i = 0; i < 4; ++i) {
                u32 f;
                switch (fn) {
                    case 0: f = (b & c) ^ (~b & d); break;
                    case 1: f = b ^ c ^ d; break;
                    case 2: f = (b & c) ^ (b & d) ^ (c & d); break;
                    default: f = b ^ c ^ d; break;
                }
                u32 temp = ((a << 5) | (a >> 27)) + f + d + src.u32_[3 - i];
                d = c; c = (b << 30) | (b >> 2); b = a; a = temp;
            }
            dst.u32_[3] = a; dst.u32_[2] = b; dst.u32_[1] = c; dst.u32_[0] = d;
            break;
        }
        case 1: {
            u32 e = (dst.u32_[3] << 30) | (dst.u32_[3] >> 2);
            dst = src;
            dst.u32_[3] += e;
            break;
        }
        case 2: {
            u32 w0 = dst.u32_[0], w1 = dst.u32_[1], w2 = dst.u32_[2], w3 = dst.u32_[3];
            dst.u32_[0] = w0 ^ w2;
            dst.u32_[1] = w1 ^ w3;
            dst.u32_[2] = w2 ^ src.u32_[0];
            dst.u32_[3] = w3 ^ src.u32_[1];
            break;
        }
        case 3: {
            u32 w0 = ((dst.u32_[0] ^ src.u32_[1]) << 1) | ((dst.u32_[0] ^ src.u32_[1]) >> 31);
            u32 w1 = ((dst.u32_[1] ^ src.u32_[2]) << 1) | ((dst.u32_[1] ^ src.u32_[2]) >> 31);
            u32 w2 = ((dst.u32_[2] ^ src.u32_[3]) << 1) | ((dst.u32_[2] ^ src.u32_[3]) >> 31);
            u32 w3 = ((dst.u32_[3] ^ w0) << 1) | ((dst.u32_[3] ^ w0) >> 31);
            dst.u32_[0] = w0; dst.u32_[1] = w1; dst.u32_[2] = w2; dst.u32_[3] = w3;
            break;
        }
        }
#endif
        return;
    }
}

} // namespace vitreo::cpu::jit
