// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Guest-base memory mapping (.docs/guest-base-memory-mapping.md): with the
// default-on linear JIT path and a linear GuestMemory, a 32-bit guest
// load/store is a single UXTW instruction off linear_base; a bad access faults
// into the recoverable handler (linear_fault.*) and surfaces as a guest
// STATUS_ACCESS_VIOLATION.
//
// These tests validate value correctness (linear result == interpreter result)
// and fault recovery (bad access -> pending_raise with the right guest addr).
// They also exercise self-modifying-code invalidation through the guarded store
// fallback.
//
// Shared by the test_jit_linear*.cpp split: the fixtures, constants and
// matchers.

#pragma once

#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <gtest/gtest.h>
#include "core/base/perf_window.hpp"
#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "core/cpu/jit/jit_compiler.hpp"
#include "jit_dispatch_execute.hpp"
#include "core/cpu/jit/jit_crash_breadcrumbs.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_helpers.hpp"
#include "core/cpu/jit/jit_stats.hpp"
#include "core/cpu/jit/linear_fault.hpp"
#include "core/cpu/jit/stencils.hpp"
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/lock_prefix.hpp"
#include "core/memory/write_watch_traps.hpp"

namespace vitreo::cpu::jit::test {

// The word ranges a compiled block occupies: [code, code + code_size) and,
// for a block placed split (VITREO_JIT_SPLIT_COLD), its cold part.
struct WordRange {
    const u32* begin;
    const u32* end;
};
inline std::vector<WordRange> block_word_ranges(const CompiledBlock& cb) {
    std::vector<WordRange> ranges;
    const auto* hot = static_cast<const u32*>(cb.code);
    ranges.push_back({hot, hot + cb.code_size / sizeof(u32)});
    if (const u8* cold_bytes = cb.cold_code()) {
        const auto* cold = reinterpret_cast<const u32*>(cold_bytes);
        ranges.push_back({cold, cold + cb.cold_size / sizeof(u32)});
    }
    return ranges;
}

inline constexpr u8 EAX = static_cast<u8>(RegIndex::RAX);
inline constexpr u8 EBX = static_cast<u8>(RegIndex::RBX);
inline constexpr u8 ESP = static_cast<u8>(RegIndex::RSP);
inline constexpr u8 EBP = static_cast<u8>(RegIndex::RBP);

// Addresses are 16 KiB-aligned: macOS ARM64 host pages are 16 KiB, so MAP_FIXED
// into the reservation requires 16 KiB alignment (real PE bases are 64 KiB-
// aligned, so this holds in practice). See .docs/guest-base-memory-mapping.md.
inline constexpr GuestAddr kCode = 0x10000;
inline constexpr GuestAddr kData = 0x00400000;
inline constexpr usize     kDataSize = 0x4000;
inline constexpr GuestAddr kUnmapped = 0x30000000;  // never mapped
inline constexpr usize     kReservation = usize{4} * 1024 * 1024 * 1024;
inline constexpr u32 kDmbIsh = 0xD5033BBFu;

inline bool is_acquire_load32(u32 word, u8 address_reg) {
    const u32 opcode = word & ~0x1Fu;
    return opcode == arm64::ldapr_w(0, address_reg) ||
           opcode == arm64::ldar_w(0, address_reg);
}

inline bool is_release_store(u32 word, u8 address_reg, int width) {
    const u32 opcode = word & ~0x1Fu;
    switch (width) {
        case 1: return opcode == arm64::stlr_b(0, address_reg);
        case 2: return opcode == arm64::stlr_h(0, address_reg);
        case 8: return opcode == arm64::stlr_x(0, address_reg);
        default: return opcode == arm64::stlr_w(0, address_reg);
    }
}

inline bool has_linear64_hot_helper_call(const std::vector<u32>& words, int width,
                                 bool is_store = false) {
    const u32 slot = width >= 16
        ? (width == 32 ? 2u : 0u) + (is_store ? 1u : 0u)
        : (is_store ? 4u : 0u) + (width == 1 ? 0u
                                : width == 2 ? 1u
                                : width == 4 ? 2u : 3u);
    const u32 base = width >= 16
        ? (JIT_CTX_OFF_LINEAR64_HOT_MEM_WIDE >> 3)
        : (JIT_CTX_OFF_LINEAR64_HOT_MEM >> 3);
    const u32 ldr = arm64::ldr_x_uoff(16, REG_JIT_CTX, base + slot);
    for (usize i = 0; i + 1 < words.size(); ++i) {
        if (words[i] == ldr && words[i + 1] == 0xD63F0200u) return true;
    }
    return false;
}

// A range-by-fault site (JitConfig::linear64_range_by_fault): the unmasked
// map index `LSR X3, Xn, #14` immediately followed by `LDRB W3, [X15, X3]`,
// whose fault on an out-of-window EA is the reservation test.
inline u32 count_range_fault_sites(const std::vector<u32>& words) {
    u32 n = 0;
    const u32 rn_mask = ~(u32{0x1F} << 5);
    for (usize i = 0; i + 1 < words.size(); ++i) {
        if ((words[i] & rn_mask) == (arm64::lsr_x_imm(3, 0, 14) & rn_mask) &&
            words[i + 1] == arm64::ldrb_reg(3, REG_LINEAR64_CHECK_MAP, 3)) {
            ++n;
        }
    }
    return n;
}

// The access's own reservation test, in any lowering: a call to the
// process-wide hot helper, the inline `LSR X8, Xn, #bits ; CBNZ X8, cold`, or
// a range-by-fault site.
inline u32 count_inline_linear64_reservation_checks(const std::vector<u32>& words) {
    u32 n = count_range_fault_sites(words);
    for (usize i = 0; i + 1 < words.size(); ++i) {
        // Either `LSR X8, Xn, #bits` (width 1, or the software-protection
        // model) or `ORR X8, X8, Xn, LSR #bits` (the default wide load, which
        // folds the host-page crossing bit into the same register), each
        // immediately followed by `CBNZ X8, .stub`.
        const bool lsr_x8 = (words[i] & 0xFFC0FC1Fu) == 0xD340FC08u;
        const bool orr_lsr_x8 = (words[i] & 0xFFE003FFu) == 0xAA400108u;
        const bool cbnz_x8 = (words[i + 1] & 0xFF00001Fu) == 0xB5000008u;
        // A range variant (linear_fault.hpp) carries the same LSR/CBNZ in
        // the block's cold code, ending `LDRB W3, [X15, X3]; B` back to its
        // site; it is that site's test, already counted above.
        const bool range_variant =
            i + 4 < words.size() &&
            words[i + 3] == arm64::ldrb_reg(3, REG_LINEAR64_CHECK_MAP, 3) &&
            (words[i + 4] & 0xFC000000u) == 0x14000000u;
        if ((lsr_x8 || orr_lsr_x8) && cbnz_x8 && !range_variant) ++n;
    }
    return n;
}

inline bool has_linear64_reservation_check(const std::vector<u32>& words, int width,
                                    bool is_store = false) {
    return has_linear64_hot_helper_call(words, width, is_store) ||
           count_inline_linear64_reservation_checks(words) != 0;
}

// LDR/STR Q Vt, [X14, Xm] with any index register: the 128-bit guard keeps
// the EA where the lowering formed it (emit_linear64_vec128_checked).
// Any index register and any vector register: with the XMM register cache
// a load lands straight in its destination's cache register (V16+n) and a
// store reads its source's.
inline bool is_linear_q_access(u32 w, bool store) {
    const u32 want = store ? arm64::str_q_reg(0, REG_LINEAR_BASE, 0)
                           : arm64::ldr_q_reg(0, REG_LINEAR_BASE, 0);
    return (w & ~((u32{0x1F} << 16) | u32{0x1F})) == want;
}

// The check-byte branch of a linear64 access: TBNZ W3 or CBNZ W3/X3.
inline bool is_check_byte_branch(u32 w) {
    return (w & 31u) == 3u && ((w & 0x7F000000u) == 0x37000000u ||
                               (w & 0x7F000000u) == 0x35000000u);
}

inline bool has_guarded_linear64_load32(const std::vector<u32>& words) {
    if (has_linear64_hot_helper_call(words, 4)) return true;
    for (usize i = 0; i + 2 < words.size(); ++i) {
        // ADD X17, X14, Xm — Xm is X9 or a live pin (Rm is bits 20:16).
        const u32 add_mask = ~(u32{0x1F} << 16);
        if ((words[i] & add_mask) !=
                (arm64::add_x(17, REG_LINEAR_BASE, 0) & add_mask) ||
            !is_acquire_load32(words[i + 1], 17)) {
            continue;
        }
        // The two-word patchable site, or the marker-free one directly
        // behind its check-byte branch (linear_fault.hpp).
        if (words[i + 2] == kTsoPatchMarker ||
            (i >= 1 && is_check_byte_branch(words[i - 1]))) {
            return true;
        }
    }
    return false;
}

inline bool has_guarded_linear64_store(const std::vector<u32>& words, int width) {
    for (usize i = 0; i + 2 < words.size(); ++i) {
        // ADD X8, X14, Xm -- Xm is X9 or wherever the EA was formed (a pin, or
        // emit_compute_ea64's scratch); Rm is bits 20:16.
        const u32 add_mask = ~(u32{0x1F} << 16);
        if ((words[i] & add_mask) !=
            (arm64::add_x(8, REG_LINEAR_BASE, 0) & add_mask)) continue;
        if (width == 1) return is_release_store(words[i + 1], 8, width);
        if (words[i + 1] == kTsoPatchMarker &&
            is_release_store(words[i + 2], 8, width)) {
            return true;
        }
        if (i >= 1 && is_check_byte_branch(words[i - 1]) &&
            is_release_store(words[i + 1], 8, width)) {
            return true;  // the marker-free site (linear_fault.hpp)
        }
    }
    return false;
}

class ScopedEnvironmentFlag {
public:
    ScopedEnvironmentFlag(const char* name, const char* value)
        : m_name(name) {
        if (const char* previous = std::getenv(name)) {
            m_previous = previous;
            m_had_previous = true;
        }
        if (value != nullptr) setenv(name, value, 1);
        else unsetenv(name);
    }

    ~ScopedEnvironmentFlag() {
        if (m_had_previous) setenv(m_name.c_str(), m_previous.c_str(), 1);
        else unsetenv(m_name.c_str());
    }

    ScopedEnvironmentFlag(const ScopedEnvironmentFlag&) = delete;
    ScopedEnvironmentFlag& operator=(const ScopedEnvironmentFlag&) = delete;

private:
    std::string m_name;
    std::string m_previous;
    bool m_had_previous{false};
};

class ScopedCompileMemoIsolation {
public:
    ScopedCompileMemoIsolation() {
        JitCompileMemo::clear_compile_memo();
        JitCompileMemo::disable_compile_memo();
    }

    ~ScopedCompileMemoIsolation() {
        JitCompileMemo::clear_compile_memo();
        // CPU tests intentionally leave the process-wide memo disabled: test
        // cases reuse guest VAs for different instruction bytes.
        JitCompileMemo::disable_compile_memo();
    }

    ScopedCompileMemoIsolation(const ScopedCompileMemoIsolation&) = delete;
    ScopedCompileMemoIsolation& operator=(const ScopedCompileMemoIsolation&) =
        delete;
};

class JitLinearTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        setenv("VITREO_JIT_LINEAR", "1", 1);
        ASSERT_TRUE(install_linear_fault_handler());
    }

    void SetUp() override {
        // The reclaim backoff is process-global and sticky by design; a test
        // that leaves one behind would silently suppress the next test's
        // reclaim request.
        JitCodeArena::reset_arena_reclaim_backoff();
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->enable_linear_reservation(kReservation));
        ASSERT_TRUE(register_linear_reservation(m_memory->linear_base(),
                                                kReservation));
        ASSERT_TRUE(m_memory->map_region(kCode, 0x4000, MemoryProtection::ReadWrite));
        ASSERT_TRUE(m_memory->map_region(kData, kDataSize, MemoryProtection::ReadWrite));
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_cache = std::make_unique<BlockCache>();
        m_jit->set_block_cache(m_cache.get());
    }

    void TearDown() override {
        if (m_memory) unregister_linear_reservation(m_memory->linear_base());
    }

    BasicBlock build_block(const std::vector<u8>& code) {
        BasicBlock b;
        b.start_addr = kCode;
        MemoryByteReader r(code.data(), code.size());
        while (r.remaining() > 0) {
            DecodedInstruction i;
            if (decode_instruction(r, DecodeMode::Mode32, i) != DecodeError::Success) break;
            b.instructions.push_back(i);
        }
        b.byte_length = static_cast<u32>(r.position());
        b.instruction_count = static_cast<u32>(b.instructions.size());
        return b;
    }

    void init_cpu(CpuState& c) {
        c.init_protected_mode();
        c.gpr.set32(ESP, 0x10000);
        c.rip = kCode;
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
    std::unique_ptr<BlockCache> m_cache;
};

class JitLinear64Test : public ::testing::Test {
protected:
    static constexpr GuestAddr kCode64 = 0x0000000140000000ull;
    static constexpr GuestAddr kData64 = 0x0000000180000000ull;
    static constexpr GuestAddr kUnmapped64 = 0x00000001C0000000ull;
    static constexpr usize kReservation64 = usize{8} * 1024 * 1024 * 1024;

    static void SetUpTestSuite() {
        setenv("VITREO_JIT_LINEAR64", "1", 1);
        ASSERT_TRUE(install_linear_fault_handler());
    }

    static void TearDownTestSuite() {
        unsetenv("VITREO_JIT_LINEAR64");
    }

    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->enable_linear_reservation(kReservation64));
        // Hardware enforcement is what lets a direct access skip the software
        // permission probe: without it the reservation proves only that an
        // address is mapped. Every protection test below is therefore a test
        // of the host page, not of a slot table.
        ASSERT_TRUE(m_memory->enable_linear_hardware_protection());
        ASSERT_TRUE(register_linear_reservation(m_memory->linear_base(),
                                                kReservation64));
        ASSERT_TRUE(m_memory->map_region(kCode64, 0x4000,
                                         MemoryProtection::ReadWrite));
        ASSERT_TRUE(m_memory->map_region(kData64, 0x4000,
                                         MemoryProtection::ReadWrite));
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_cache = std::make_unique<BlockCache>();
        m_jit->set_block_cache(m_cache.get());
    }

    void TearDown() override {
        if (m_memory) unregister_linear_reservation(m_memory->linear_base());
    }

    BasicBlock build64(const std::vector<u8>& code) {
        BasicBlock b;
        b.start_addr = kCode64;
        b.is_64bit = true;
        MemoryByteReader r(code.data(), code.size());
        while (r.remaining() > 0) {
            DecodedInstruction i;
            if (decode_instruction(r, DecodeMode::Mode64, i) !=
                DecodeError::Success) {
                break;
            }
            b.instructions.push_back(i);
        }
        b.byte_length = static_cast<u32>(r.position());
        b.instruction_count = static_cast<u32>(b.instructions.size());
        return b;
    }

    CpuState cpu64(GuestAddr rbx) {
        CpuState cpu;
        cpu.init_long_mode();
        cpu.rip = kCode64;
        cpu.gpr.set64(EBX, rbx);
        return cpu;
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
    std::unique_ptr<BlockCache> m_cache;
};

}  // namespace vitreo::cpu::jit::test
