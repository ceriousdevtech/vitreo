// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_jit_emission_golden.cpp
/// @brief Byte-level gate on what the JIT emits, for refactors of the emitter.
///
/// This is the byte-level gate an emitter refactor is built on.
///
/// Everything in the module refactor produced identical object code, which is
/// why "the same tests fail" was strong evidence there. Splitting `JitCompiler`
/// does not have that property: every phase moves live emission logic, and a
/// behavioural test that passes tells you only that the corpus did not happen
/// to hit what you broke. The available stronger evidence is that the JIT is
/// deterministic — the same decoded block, the same configuration, the same
/// ARM64 — so a phase that emits identical code cannot have changed behavior.
///
/// ## Why the emitted words need normalizing first
///
/// 49 call sites emit a host function address inline, as
/// `emit_mov_imm64(16, (uintptr_t)&helper)` followed by `BLR X16`. Those
/// addresses move whenever the binary is relinked, which every phase of the
/// split does. A raw hash of the code would therefore fail on the first commit
/// and teach everyone to regenerate the baseline — the exact habit that makes a
/// gate worthless.
///
/// So a `MOVZ`/`MOVK` run targeting X16 is reconstructed into the 64-bit value
/// it builds, that value is resolved with `dladdr`, and the **symbol name** is
/// hashed in place of the address. X16 is IP0, the ARM64 intra-procedure
/// scratch: by convention it holds a call target, never a value the emitted
/// code computes with.
///
/// Zeroing the immediate would have been simpler and is what this did first.
/// It is also wrong in a way worth recording: two helper addresses have the
/// same number of nonzero halfwords almost always, so `MOVZ/MOVK` sequences
/// for two *different* helpers normalize identically. Swapping
/// `jit_fpu_apply_rounding` for `jit_sse_apply_rounding` — a real behavior
/// change, at any of 49 sites — would have produced a byte-identical
/// fingerprint. Resolving the symbol keeps the gate build-independent without
/// giving that up.
///
/// Helper calls made through `JitContext` — `LDR X16,[X20,#off]; BLR X16`,
/// which is most of them — need no masking at all: the address is fetched at
/// run time and the emitted offset is a compile-time constant.
///
/// ## Delegate-bearing blocks are hashed on structure only
///
/// An instruction with no native lowering emits an interpreter-delegate
/// stencil, and that stencil bakes the *heap* address of the block's
/// `DecodedInstruction`. That address is not merely build-dependent, it is
/// **run**-dependent: heap ASLR moves it between two runs of the same binary.
/// Masking it by opcode is not possible, because unlike a call target it is
/// loaded into an ordinary register.
///
/// So a block whose `delegate_insns` is set is compared on its metadata and
/// emitted word count rather than its bytes, and the row is marked `D` to make
/// that visible in the baseline. The coverage that is lost is a stencil call,
/// which is the part of emission this gate does not touch; the coverage that
/// is kept — `native_insns` — is what catches an instruction silently starting
/// or stopping being delegated, which is the regression that actually matters.
///
/// This was found, not assumed: `movzx64_mem` hashed differently on every run
/// of the same binary.
///
/// ## What is NOT covered
///
/// Blocks are compiled standalone and never linked, so no cross-block branch
/// targets are baked. Block linking has its own tests; this gate is about what
/// one block's compilation produces.
///
/// ## Regenerating
///
///     VITREO_GOLDEN_WRITE=1 ./build/tests/vitreo_cpu_tests \
///         --gtest_filter='JitEmissionGolden.*'
///
/// Regenerate only when the emitted code is *meant* to change, and say so in
/// the commit message. A phase of the split that regenerates this file has, by
/// definition, failed to be behavior-preserving.

#include <gtest/gtest.h>

#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_helpers.hpp"
#include "core/cpu/decode/decoder.hpp"
#include "core/memory/guest_memory.hpp"

#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

constexpr GuestAddr kCode64   = 0x140000000ull;
constexpr GuestAddr kCode32   = 0x00400000ull;
constexpr usize     kCodeSize = 0x4000;
constexpr GuestAddr kData     = 0x00600000ull;
constexpr usize     kDataSize = 0x4000;

// MOVZ/MOVK (64-bit, immediate) and the register they target.
constexpr u32 kMovzX  = 0xD2800000u;
constexpr u32 kMovkX  = 0xF2800000u;
constexpr u32 kMovMask = 0xFF800000u;
constexpr u32 kImm16Mask = 0x001FFFE0u;  // bits 20:5
constexpr u8  kIp0 = 16;

[[nodiscard]] bool is_mov_into_ip0(u32 w) noexcept {
    const u32 op = w & kMovMask;
    return (op == kMovzX || op == kMovkX) && (w & 0x1Fu) == kIp0;
}

[[nodiscard]] u32 mov_imm16(u32 w) noexcept { return (w & kImm16Mask) >> 5; }
[[nodiscard]] u32 mov_shift(u32 w) noexcept { return ((w >> 21) & 0x3u) * 16u; }

/// Zero the immediate of a MOVZ/MOVK that targets X16, keeping the opcode and
/// the shift so MOVZ stays distinguishable from MOVK. Used for the words of a
/// run whose target symbol is hashed separately.
[[nodiscard]] u32 normalize_word(u32 w) noexcept {
    return is_mov_into_ip0(w) ? (w & ~kImm16Mask) : w;
}

/// Name of the host symbol a reconstructed call target resolves to.
///
/// `dladdr` gives a build-independent identity for the address that a relink
/// moves. An address that does not resolve is reported as unresolved rather
/// than as its numeric value: a value that changes per build must never reach
/// the hash, and silently hashing zero instead would be the bug this whole
/// normalization exists to avoid.
[[nodiscard]] std::string call_target_name(u64 addr) {
    Dl_info info{};
    if (addr != 0 && dladdr(reinterpret_cast<void*>(addr), &info) != 0 &&
        info.dli_sname != nullptr) {
        return info.dli_sname;
    }
    return "<unresolved>";
}

/// FNV-1a over the normalized instruction stream plus the metadata that
/// describes how the block may be entered. The metadata matters as much as the
/// code: `pin_layout` and the link/call entry offsets are the ABI a linked hop
/// relies on, and a refactor that preserved every word but shifted an entry
/// offset would have changed behavior.
[[nodiscard]] u64 fingerprint(const CompiledBlock& cb, bool delegated) {
    u64 h = 0xcbf29ce484222325ull;
    auto mix = [&h](u64 v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFFu;
            h *= 0x100000001b3ull;
        }
    };

    const u32 word_count = cb.code_size / sizeof(u32);
    if (!delegated) {
        const auto* words = static_cast<const u32*>(cb.code);
        for (u32 i = 0; i < word_count; ++i) {
            mix(normalize_word(words[i]));
            if (!is_mov_into_ip0(words[i])) continue;
            // Start of a MOVZ/MOVK run: reconstruct the whole value, then mix
            // the symbol it names. The run's remaining words are still mixed
            // individually by the loop, which keeps its length in the hash.
            if ((words[i] & kMovMask) != kMovzX) continue;
            u64 target = 0;
            for (u32 j = i; j < word_count && is_mov_into_ip0(words[j]); ++j) {
                if (j > i && (words[j] & kMovMask) != kMovkX) break;
                target |= static_cast<u64>(mov_imm16(words[j]))
                          << mov_shift(words[j]);
            }
            for (unsigned char ch : call_target_name(target)) mix(ch);
        }
    }

    mix(cb.code_size);
    mix(cb.native_insns);
    mix(cb.pin_layout);
    mix(cb.link_code_offset);
    mix(cb.call_code_offset);
    mix(cb.requires_protection_lease ? 1u : 0u);
    return h;
}

struct Snippet {
    const char* name;
    bool is_64;
    std::vector<u8> bytes;
};

// The corpus is curated rather than generated, for two reasons: a golden file
// has to be readable by whoever is looking at a diff, and the cases that
// matter are known. Weighted towards the lazy-flag machinery, because that is
// what the split is aimed at and where every recurring defect in this area has
// been.
const std::vector<Snippet>& corpus() {
    static const std::vector<Snippet> c = {
        // ---- Lazy flags: produce, then consume -------------------------
        {"add32_jz",        false, {0x01,0xD8, 0x74,0x02, 0x90,0x90}},
        {"sub32_jnz",       false, {0x29,0xD8, 0x75,0x02, 0x90,0x90}},
        {"cmp32_jb",        false, {0x39,0xD8, 0x72,0x02, 0x90,0x90}},
        {"test32_js",       false, {0x85,0xD8, 0x78,0x02, 0x90,0x90}},
        {"inc32_jo",        false, {0x40, 0x70,0x02, 0x90,0x90}},
        // Flags produced, then a flag-neutral op, then consumed. The forward
        // path a deferred store has to survive.
        {"add32_mov_jz",    false, {0x01,0xD8, 0x89,0xCA, 0x74,0x02, 0x90,0x90}},

        // ---- The CF overlay, which has broken twice --------------------
        // ADD sets CF, CLC forces it to 0, JB must read the overlay and not
        // the ADD. See jit_cf_overlay_ignored_by_jcc.
        {"add_clc_jb",      false, {0x01,0xD8, 0xF8, 0x72,0x02, 0x90,0x90}},
        {"add_stc_jb",      false, {0x01,0xD8, 0xF9, 0x72,0x02, 0x90,0x90}},
        {"add_cmc_jb",      false, {0x01,0xD8, 0xF5, 0x72,0x02, 0x90,0x90}},
        // BT installs a CF overlay over a live FlagOp by design.
        {"add_bt_jb",       false, {0x01,0xD8, 0x0F,0xA3,0xD8, 0x72,0x02, 0x90,0x90}},

        // ---- Carry chains: the ADC/SBB producer-consumer wire ----------
        {"adc32_chain",     false, {0x11,0xD8, 0x11,0xCA, 0x11,0xF7}},
        {"sbb32_chain",     false, {0x19,0xD8, 0x19,0xCA, 0x19,0xF7}},
        {"add_adc_pair",    false, {0x01,0xD8, 0x11,0xCA}},

        // ---- Partial-flag ops: must not install a lazy FlagOp ----------
        {"ror32_1",         false, {0xD1,0xC8}},
        {"rol32_cl",        false, {0xD3,0xC0}},
        {"shl32_imm",       false, {0xC1,0xE0,0x05}},
        {"shr32_zero_cnt",  false, {0xC1,0xE8,0x00}},

        // ---- SETcc / CMOV: the other lazy-flag consumers ---------------
        {"cmp_setb",        false, {0x39,0xD8, 0x0F,0x92,0xC0}},
        {"cmp_cmovz",       false, {0x39,0xD8, 0x0F,0x44,0xC1}},

        // ---- Memory: the linear and helper store paths -----------------
        {"load32_disp",     false, {0x8B,0x05,0x00,0x00,0x60,0x00}},
        {"store32_disp",    false, {0x89,0x05,0x00,0x00,0x60,0x00}},
        {"load8_zx",        false, {0x0F,0xB6,0x05,0x00,0x00,0x60,0x00}},
        {"rmw_add_mem",     false, {0x01,0x05,0x00,0x00,0x60,0x00}},
        {"lock_add_mem",    false, {0xF0,0x01,0x05,0x00,0x00,0x60,0x00}},

        // ---- x87 and SSE: the paths that bake a host helper address ----
        // These are what makes the X16 normalization load-bearing; if it were
        // wrong, these entries would be the ones that churn.
        {"fld_fstp_m32",    false, {0xD9,0x05,0x00,0x00,0x60,0x00,
                                    0xD9,0x1D,0x04,0x00,0x60,0x00}},
        {"fadd_m64",        false, {0xDC,0x05,0x00,0x00,0x60,0x00}},
        {"fdiv_m32",        false, {0xD8,0x35,0x00,0x00,0x60,0x00}},
        {"sse_addps",       false, {0x0F,0x58,0xC1}},
        {"sse_cvtsi2sd",    false, {0xF2,0x0F,0x2A,0xC0}},

        // ---- Control flow ---------------------------------------------
        {"call_rel32",      false, {0xE8,0x00,0x00,0x00,0x00}},
        {"ret",             false, {0xC3}},
        {"jmp_indirect",    false, {0xFF,0xE0}},
        {"push_pop",        false, {0x50, 0x58}},

        // ---- String ops -----------------------------------------------
        {"rep_movsd",       false, {0xF3,0xA5}},
        {"rep_stosd",       false, {0xF3,0xAB}},

        // ---- The switch-dispatch shape from tools/stress sc_blocksize ----
        // `CMP BYTE [mem], imm8 ; JA` is the bounds check of a dense switch,
        // and profiling bsz1 put 24.5% of ALL JIT time on the cond helper this
        // emits. Sub8 permits cc 0x2-0x7, so JA is fusable in principle; these
        // rows are here to make the emitted size of that decision visible.
        {"cmp_m8_ja",       true,  {0x80, 0x3A, 0x07, 0x77, 0x02, 0x90, 0x90}},
        {"cmp_m8_jb",       true,  {0x80, 0x3A, 0x07, 0x72, 0x02, 0x90, 0x90}},
        {"cmp_r8_ja",       true,  {0x38, 0xD8, 0x77, 0x02, 0x90, 0x90}},
        {"cmp_m32_ja",      true,  {0x83, 0x3A, 0x07, 0x77, 0x02, 0x90, 0x90}},

        // ---- 64-bit forms ----------------------------------------------
        {"add64_jz",        true,  {0x48,0x01,0xD8, 0x74,0x02, 0x90,0x90}},
        {"cmp64_jb",        true,  {0x48,0x39,0xD8, 0x72,0x02, 0x90,0x90}},
        {"adc64_chain",     true,  {0x48,0x11,0xD8, 0x48,0x11,0xCA}},
        {"add64_clc_jb",    true,  {0x48,0x01,0xD8, 0xF8, 0x72,0x02, 0x90,0x90}},
        {"ror64_1",         true,  {0x48,0xD1,0xC8}},
        {"load64_rip_rel",  true,  {0x48,0x8B,0x05,0x00,0x00,0x00,0x00}},
        {"store64_reg",     true,  {0x48,0x89,0x03}},
        {"movzx64_mem",     true,  {0x48,0x0F,0xB6,0x03}},
        {"lock_xadd64",     true,  {0xF0,0x48,0x0F,0xC1,0x03}},
        {"push_pop64",      true,  {0x50, 0x58}},
    };
    return c;
}

struct GoldenRow {
    u64 hash = 0;
    u32 words = 0;
    u32 native_insns = 0;
    u64 pin_layout = 0;
    u32 link_off = 0;
    u32 call_off = 0;
    bool delegated = false;  // hashed on structure only; see the file comment
};

std::string baseline_path() {
    // The file lives beside this test rather than in the build tree: it is
    // reviewed input, not an artifact.
    return std::string(VITREO_TEST_SOURCE_DIR) + "/jit_emission_golden.tsv";
}

std::map<std::string, GoldenRow> read_baseline() {
    std::map<std::string, GoldenRow> rows;
    std::ifstream in(baseline_path());
    if (!in) return rows;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string name;
        GoldenRow r;
        std::string mode;
        ss >> name >> std::hex >> r.hash >> std::dec >> r.words
           >> r.native_insns >> std::hex >> r.pin_layout >> std::dec
           >> r.link_off >> r.call_off >> mode;
        r.delegated = (mode == "D");
        if (!name.empty()) rows[name] = r;
    }
    return rows;
}

class JitEmissionGolden : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(kCode64, kCodeSize,
                                         MemoryProtection::ReadWriteExecute));
        ASSERT_TRUE(m_memory->map_region(kCode32, kCodeSize,
                                         MemoryProtection::ReadWriteExecute));
        ASSERT_TRUE(m_memory->map_region(kData, kDataSize,
                                         MemoryProtection::ReadWrite));
        m_cache = std::make_unique<BlockCache>();
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_jit->set_block_cache(m_cache.get());
    }

    /// Compile one snippet standalone. Returns false when the JIT declines,
    /// which is a legitimate answer for some forms and is itself recorded.
    ///
    /// `slot` gives every snippet its own guest address. Compiling them all at
    /// one address looks harmless and is not: rewriting bytes under a page
    /// that already holds compiled code is self-modifying code, and after a
    /// few rewrites the JIT permanently demotes that range to the interpreter.
    /// The first draft of this test did that and reported 29 of 44 snippets as
    /// "the JIT declines to compile this", which was entirely the harness.
    bool compile(const Snippet& s, usize slot, GoldenRow& out) {
        const GuestAddr base =
            (s.is_64 ? kCode64 : kCode32) + static_cast<GuestAddr>(slot) * 0x40;
        EXPECT_TRUE(m_memory->write_bytes(base, s.bytes.data(), s.bytes.size()));

        BasicBlock block;
        block.start_addr = base;
        block.is_64bit = s.is_64;
        MemoryByteReader reader(s.bytes.data(), s.bytes.size());
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            if (decode_instruction(reader,
                                   s.is_64 ? DecodeMode::Mode64
                                           : DecodeMode::Mode32,
                                   inst) != DecodeError::Success) {
                break;
            }
            block.instructions.push_back(inst);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count = static_cast<u32>(block.instructions.size());
        if (block.instruction_count == 0) return false;

        CompiledBlock* cb = m_jit->compile(block);
        if (cb == nullptr || cb->code == nullptr) return false;

        out.delegated = (cb->delegate_insns != nullptr);
        out.hash = fingerprint(*cb, out.delegated);
        out.words = cb->code_size / sizeof(u32);
        out.native_insns = cb->native_insns;
        out.pin_layout = cb->pin_layout;
        out.link_off = cb->link_code_offset;
        out.call_off = cb->call_code_offset;
        return true;
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
    std::unique_ptr<BlockCache> m_cache;
};

// -----------------------------------------------------------------------------
// The gate
// -----------------------------------------------------------------------------

TEST_F(JitEmissionGolden, EmissionMatchesRecordedBaseline) {
    const bool writing = std::getenv("VITREO_GOLDEN_WRITE") != nullptr;
    const auto baseline = read_baseline();

    std::ostringstream out;
    out << "# JIT emission golden baseline.\n"
        << "# Regenerate ONLY when emission is meant to change:\n"
        << "#   VITREO_GOLDEN_WRITE=1 ./build/tests/vitreo_cpu_tests \\\n"
        << "#       --gtest_filter='JitEmissionGolden.*'\n"
        << "# MOVZ/MOVK immediates targeting X16 are masked before hashing;\n"
        << "# they are host helper addresses that move on every relink.\n"
        << "# A row marked D is delegate-bearing: its hash covers metadata\n"
        << "# only, because delegate stencils bake a run-dependent heap\n"
        << "# address. native_insns still gates it.\n"
        << "# name\thash\twords\tnative_insns\tpin_layout\tlink_off\tcall_off\tmode\n";

    usize compiled = 0;
    usize declined = 0;

    usize slot = 0;
    for (const auto& s : corpus()) {
        GoldenRow row;
        const bool ok = compile(s, slot++, row);
        if (!ok) {
            ++declined;
            // A declined block is recorded too. "The JIT stopped being able to
            // compile this" is exactly the kind of silent regression a
            // coverage ratchet exists to catch, and it would otherwise look
            // like an absent row.
            out << s.name << "\tDECLINED\t0\t0\t0\t0\t0\t-\n";
        } else {
            ++compiled;
            out << s.name << "\t" << std::hex << row.hash << std::dec
                << "\t" << row.words << "\t" << row.native_insns
                << "\t" << std::hex << row.pin_layout << std::dec
                << "\t" << row.link_off << "\t" << row.call_off
                << "\t" << (row.delegated ? "D" : "N") << "\n";
        }

        if (writing) continue;

        const auto it = baseline.find(s.name);
        if (it == baseline.end()) {
            ADD_FAILURE() << "no baseline row for '" << s.name
                          << "'. If this snippet is new, regenerate with "
                             "VITREO_GOLDEN_WRITE=1.";
            continue;
        }
        if (!ok) {
            EXPECT_EQ(it->second.words, 0u)
                << "the JIT now DECLINES to compile '" << s.name
                << "', which it used to compile into " << it->second.words
                << " words. That is a coverage regression, not a refactor.";
            continue;
        }

        // Report the cheap structural differences first: they say *what*
        // changed, where a hash mismatch only says *that* something did.
        EXPECT_EQ(it->second.words, row.words)
            << "'" << s.name << "': emitted word count changed";
        EXPECT_EQ(it->second.native_insns, row.native_insns)
            << "'" << s.name << "': native lowering count changed — an"
               " instruction started or stopped falling back to the"
               " interpreter delegate";
        EXPECT_EQ(it->second.pin_layout, row.pin_layout)
            << "'" << s.name << "': pinned-register ABI at the link entry"
               " changed; a linked hop bypasses the prologue on this";
        EXPECT_EQ(it->second.link_off, row.link_off)
            << "'" << s.name << "': JIT-to-JIT link entry offset changed";
        EXPECT_EQ(it->second.call_off, row.call_off)
            << "'" << s.name << "': native CALL landing pad offset changed";
        EXPECT_EQ(it->second.delegated, row.delegated)
            << "'" << s.name << "': block started or stopped carrying an"
               " interpreter delegate";
        EXPECT_EQ(it->second.hash, row.hash)
            << "'" << s.name << "': emitted code changed. If this is a"
               " behavior-preserving refactor, it is not — read the emitted"
               " words. If the change is intended, regenerate the baseline"
               " and say so in the commit message.";
    }

    if (writing) {
        std::ofstream f(baseline_path(), std::ios::trunc);
        ASSERT_TRUE(f.good()) << "cannot write " << baseline_path();
        f << out.str();
        GTEST_SKIP() << "wrote baseline: " << compiled << " compiled, "
                     << declined << " declined";
    }

    // A corpus that compiles nothing would pass every comparison above.
    EXPECT_GT(compiled, corpus().size() / 2)
        << "most of the corpus failed to compile; the gate is not measuring"
           " anything";
}

// -----------------------------------------------------------------------------
// Gate self-tests
// -----------------------------------------------------------------------------
//
// A gate that has never been shown to fail is not known to work. These two
// pin the normalization from both sides, which is the part that could silently
// destroy the gate's value: mask too little and it churns on every relink,
// mask too much and it stops noticing real changes.

TEST(JitEmissionGoldenNormalize, ResolvesCallTargetsToSymbolNames) {
    // The property the gate depends on: an address resolves to a name that
    // does not move on relink, and two different helpers get different names.
    // The two helpers are the real case: ops/fpu.cpp picks between them at one
    // emit site, and swapping them is a behavior change that must be visible.
    // (Deliberately not memcpy/memmove — on macOS both alias
    // _platform_memmove, so that pair proves nothing.)
    const u64 fpu = reinterpret_cast<u64>(&jit_fpu_apply_rounding);
    const u64 sse = reinterpret_cast<u64>(&jit_sse_apply_rounding);
    EXPECT_EQ(call_target_name(fpu), call_target_name(fpu));
    EXPECT_NE(call_target_name(fpu), call_target_name(sse))
        << "two different helpers must not normalize to the same identity —"
           " zeroing the immediate did exactly that, which is why this"
           " resolves the symbol instead";
    EXPECT_EQ(call_target_name(0), "<unresolved>");
}

TEST(JitEmissionGoldenNormalize, MasksHostAddressLoadsIntoIp0) {
    // MOVZ X16, #0x1234  /  MOVK X16, #0x5678, LSL #16
    const u32 movz = kMovzX | (0x1234u << 5) | kIp0;
    const u32 movk = kMovkX | (1u << 21) | (0x5678u << 5) | kIp0;
    const u32 movz_other_addr = kMovzX | (0x9ABCu << 5) | kIp0;

    EXPECT_EQ(normalize_word(movz), normalize_word(movz_other_addr))
        << "two different host addresses in X16 must normalize alike";
    EXPECT_NE(normalize_word(movz), normalize_word(movk))
        << "MOVZ and MOVK must stay distinguishable after masking";
}

TEST(JitEmissionGoldenNormalize, PreservesEverythingElse) {
    // Same immediate, a different destination register: not a call target,
    // so it must survive untouched.
    const u32 movz_x0 = kMovzX | (0x1234u << 5) | 0u;
    const u32 movz_x0_other = kMovzX | (0x9ABCu << 5) | 0u;
    EXPECT_EQ(normalize_word(movz_x0), movz_x0);
    EXPECT_NE(normalize_word(movz_x0), normalize_word(movz_x0_other))
        << "immediates outside X16 are guest values and must be hashed";

    // The dominant helper-call form loads through JitContext and needs no
    // masking at all: LDR X16, [X20, #off] then BLR X16.
    const u32 ldr_x16_x20 = 0xF9400000u | (7u << 10) | (20u << 5) | kIp0;
    const u32 blr_x16 = 0xD63F0000u | (kIp0 << 5);
    EXPECT_EQ(normalize_word(ldr_x16_x20), ldr_x16_x20)
        << "a JitContext helper load carries a constant offset, not an"
           " address; masking it would hide a changed helper";
    EXPECT_EQ(normalize_word(blr_x16), blr_x16);

    // Ordinary arithmetic must be untouched.
    const u32 add_x0_x1_x2 = 0x8B020020u;
    EXPECT_EQ(normalize_word(add_x0_x1_x2), add_x0_x1_x2);
}

}  // namespace
