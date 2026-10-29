// SPDX-License-Identifier: LGPL-2.1-only

/// @file fx_image.cpp
/// @brief FXSAVE/FXRSTOR image marshalling (Intel SDM Vol. 2, FXSAVE/FXRSTOR).

#include "core/cpu/state/fx_image.hpp"
#include "core/math/host_fenv.hpp"
#include "core/cpu/state/cpu_state.hpp"

#include <cstring>

namespace vitreo::cpu {

namespace {

void store_u16(u8* image, usize offset, u16 value) noexcept {
    std::memcpy(image + offset, &value, sizeof(value));
}
void store_u32(u8* image, usize offset, u32 value) noexcept {
    std::memcpy(image + offset, &value, sizeof(value));
}
void store_u64(u8* image, usize offset, u64 value) noexcept {
    std::memcpy(image + offset, &value, sizeof(value));
}
u16 load_u16(const u8* image, usize offset) noexcept {
    u16 value;
    std::memcpy(&value, image + offset, sizeof(value));
    return value;
}
u32 load_u32(const u8* image, usize offset) noexcept {
    u32 value;
    std::memcpy(&value, image + offset, sizeof(value));
    return value;
}
u64 load_u64(const u8* image, usize offset) noexcept {
    u64 value;
    std::memcpy(&value, image + offset, sizeof(value));
    return value;
}

} // namespace

void fx_image_save(const CpuState& cpu, u8* image, bool wide,
                   u8 xmm_count) noexcept {
    std::memset(image, 0, kFxImageBytes);

    const auto& fpu = cpu.fpu;
    store_u16(image, 0, fpu.control_word);
    store_u16(image, 2, fpu.status_word);

    // FXSAVE uses an abridged tag word: one bit for each physical x87
    // register, set when the corresponding full tag is not Empty.
    u8 abridged_tag = 0;
    for (u8 reg = 0; reg < 8; ++reg) {
        if (fpu.get_tag(reg) != FpuTag::Empty) {
            abridged_tag |= static_cast<u8>(1u << reg);
        }
    }
    image[4] = abridged_tag;
    store_u16(image, 6, static_cast<u16>(fpu.fop & 0x07ffu));

    if (wide) {
        store_u64(image, 8, fpu.fip);
        store_u64(image, 16, fpu.fdp);
    } else {
        store_u32(image, 8, static_cast<u32>(fpu.fip));
        store_u32(image, 16, static_cast<u32>(fpu.fdp));
        // FCS and FDS are zero because Vitreo's flat user-mode CPU state
        // does not retain last-instruction selector metadata.
    }
    store_u32(image, 24, cpu.sse.mxcsr);
    store_u32(image, 28, cpu.sse.mxcsr_mask);

    // The register slots are in logical ST(0)..ST(7) stack order, while the
    // abridged tag bits above retain physical R0..R7 numbering.
    for (u8 i = 0; i < 8; ++i) {
        const auto& value = fpu.get_st(i);
        std::memcpy(image + 32 + static_cast<usize>(i) * 16,
                    &value, sizeof(value));
    }

    for (u8 i = 0; i < xmm_count; ++i) {
        std::memcpy(image + 160 + static_cast<usize>(i) * 16,
                    cpu.sse.xmm[i].u8_, 16);
    }
}

void fx_image_restore(CpuState& cpu, const u8* image, bool wide,
                      u8 xmm_count) noexcept {
    // Restoring an FPU image replaces the unit's exception state wholesale:
    // the image's status word IS the exception state after the restore, so
    // whatever the host unit had accumulated belongs to the state being
    // discarded. Leaving it there means the next harvest (FNSTENV / FNSAVE,
    // see core/math/host_fenv.hpp) reports exceptions from before the restore.
    math::clear_host_fp_exceptions();
    fx_image_restore_registers(cpu, image, wide, xmm_count);
}

void fx_image_restore_registers(CpuState& cpu, const u8* image, bool wide,
                                u8 xmm_count) noexcept {
    auto& fpu = cpu.fpu;
    fpu.control_word = FpuState::sanitize_control_word(load_u16(image, 0));
    fpu.status_word = load_u16(image, 2);
    // Deliberately NOT apply_x87_rounding(fpu.control_word) here. The image's
    // control word does select an x87 rounding mode, but x87 RC and MXCSR RC
    // are independent architectural fields sharing one host FPCR, so
    // installing x87's mode on a restore also -- wrongly -- re-rounds every
    // subsequent SSE operation. Measured 2026-08-30: doing it here takes the
    // x64 oracle ISA fuzz from 1354 to 1661 failures, all of them SSE
    // arithmetic under a seeded non-default FCW. See the DISABLED_ pin in
    // tests/oracle/test_oracle_fp_rounding_64.cpp; this needs the real fix
    // (rounding installed per instruction class), not a wider trigger.
    fpu.fop = static_cast<u16>(load_u16(image, 6) & 0x07ffu);

    if (wide) {
        fpu.fip = load_u64(image, 8);
        fpu.fdp = load_u64(image, 16);
    } else {
        fpu.fip = load_u32(image, 8);
        fpu.fdp = load_u32(image, 16);
    }

    cpu.sse.mxcsr = load_u32(image, 24) & cpu.sse.mxcsr_mask;

    for (u8 i = 0; i < 8; ++i) {
        Float80 value{};
        std::memcpy(&value, image + 32 + static_cast<usize>(i) * 16,
                    sizeof(value));
        fpu.set_st(i, value);
    }

    // Abridged FTW uses one bit per physical x87 register (not ST(i)).
    // bit=0 -> Empty; bit=1 -> Zero if the restored value is +0/-0, else Valid.
    const u8 abridged_tag = image[4];
    for (u8 reg = 0; reg < 8; ++reg) {
        if ((abridged_tag & static_cast<u8>(1u << reg)) == 0) {
            fpu.set_tag(reg, FpuTag::Empty);
        } else {
            const auto& value = fpu.st[reg];
            const bool is_zero =
                value.mantissa == 0 && (value.exponent_sign & 0x7FFF) == 0;
            fpu.set_tag(reg, is_zero ? FpuTag::Zero : FpuTag::Valid);
        }
    }

    for (u8 i = 0; i < xmm_count; ++i) {
        std::memcpy(cpu.sse.xmm[i].u8_,
                    image + 160 + static_cast<usize>(i) * 16, 16);
    }
}

} // namespace vitreo::cpu
