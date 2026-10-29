// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include <core/cpu/jit/jit_config.hpp>

#include <cstdio>

namespace vitreo::cpu::jit {

namespace {

[[nodiscard]] const char* tso_name(TsoMode mode) noexcept {
    switch (mode) {
    case TsoMode::Off:     return "off";
    case TsoMode::Ordered: return "ordered";
    case TsoMode::Fence:   return "fence";
    }
    return "?";
}

}  // namespace

std::string JitConfig::describe() const {
    // Deliberately every field, including the ones that are usually default.
    // A configuration dump that hides defaults cannot answer "was this run
    // configured the way I think it was", which is the only question it exists
    // to answer.
    char buf[1024];
    int n = std::snprintf(
        buf, sizeof(buf),
        "no_jitcache=%d linear_jit=%d linear64=%d linear64_address_bits=%u\n"
        "linear32_check_map=%d linear32_check_index_bits=%u\n"
        "linear64_hw_protect=%d linear64_perm_guard=%d "
        "linear64_vec128=%d linear64_range_by_fault=%d\n"
        "tso=%s strict_tso=%d tso_use_ldapr=%d tso_fault_patch=%d "
        "tso_vector=%d tso_store_tail_fence=%d tso_crossing_fault=%d\n"
        "native_ud2=%d native_callret=%d forward_join_state=%d\n"
        "jit_skip_addrs=%zu skip_above=0x%llx skip_below=0x%llx skip_ranges=%u",
        no_jitcache, linear_jit, linear64,
        static_cast<unsigned>(linear64_address_bits),
        linear32_check_map, static_cast<unsigned>(linear32_check_index_bits),
        linear64_hw_protect, linear64_perm_guard,
        linear64_vec128, linear64_range_by_fault,
        tso_name(tso_mode), strict_tso, tso_use_ldapr, tso_fault_patch,
        tso_vector, tso_store_tail_fence, tso_crossing_fault,
        native_ud2_enabled, native_callret_enabled, forward_join_state_enabled,
        jit_skip.size(), static_cast<unsigned long long>(jit_skip_above),
        static_cast<unsigned long long>(jit_skip_below),
        static_cast<unsigned>(jit_skip_range_count));
    if (n < 0) return {};

    std::string out(buf);
    for (u8 i = 0; i < jit_skip_range_count && i < kMaxJitSkipRanges; ++i) {
        char range[64];
        std::snprintf(range, sizeof(range), "\n  skip[%u]=0x%llx-0x%llx",
                      static_cast<unsigned>(i),
                      static_cast<unsigned long long>(jit_skip_range_from[i]),
                      static_cast<unsigned long long>(jit_skip_range_to[i]));
        out += range;
    }
    return out;
}

}  // namespace vitreo::cpu::jit
