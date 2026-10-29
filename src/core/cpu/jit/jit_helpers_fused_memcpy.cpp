// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_helpers_fused_memcpy.cpp
/// @brief Runtime behind the idiom detector's fused memcpy.
///
/// Reached through JitContext::fused_memcpy, which
/// constructor_destructor.cpp points at jit_fused_memcpy.
///
/// Split out of jit_helpers_lock.cpp.

#include "jit_helpers.hpp"

#include <vitreo/diagnostics.hpp>

#include <cstring>

namespace vitreo::cpu::jit {

extern "C" void jit_fused_memcpy(CpuState* cpu, GuestMemory* mem,
                                  u32 src_gva, u32 dst_gva,
                                  u32 byte_count, u64 xmm_pack,
                                  u32 xmm_count) {
    diagnostics::perf_add_fused_memcpy(byte_count);
    u8 xmm_tmp[8][16]{};
    const u32 nxmm = xmm_count > 8 ? 8 : xmm_count;
    for (u32 i = 0; i < nxmm; ++i) {
        for (u32 b = 0; b < 16; ++b) {
            auto v = mem->read_u8(src_gva + i * 16 + b);
            if (v.is_ok()) xmm_tmp[i][b] = v.value();
        }
    }

    auto src_res = mem->translate(src_gva);
    auto dst_res = mem->translate(dst_gva);
    bool can_use_fast_path = src_res.is_ok() && dst_res.is_ok() &&
                             mem->is_mapped(src_gva, byte_count) &&
                             mem->is_mapped(dst_gva, byte_count);

    if (can_use_fast_path) {
        std::memmove(dst_res.value(), src_res.value(), byte_count);
    } else {
        for (u32 i = 0; i < byte_count; ++i) {
            auto b = mem->read_u8(src_gva + i);
            if (b.is_ok()) (void)mem->write_u8(dst_gva + i, b.value());
        }
    }
    for (u32 i = 0; i < nxmm; ++i) {
        const u8 xmm = static_cast<u8>((xmm_pack >> (8 * i)) & 15);
        cpu->sse.xmm[xmm].set_bytes(xmm_tmp[i]);
    }
}

} // namespace vitreo::cpu::jit
