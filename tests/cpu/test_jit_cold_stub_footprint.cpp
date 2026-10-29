// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Emitted footprint of the cold memory slow paths.
//
// Every inline guest access queues a cold stub: the window miss, the
// mixed-protection sub-page decision, the region slot probe, the helper call
// and its raise check. Under the arena soft budget those bytes are what
// retirement evicts, so their size per block is a gate number.
//
// `Report*` cases compile a real PE's code (VITREO_COLD_STUB_CORPUS64 /
// VITREO_COLD_STUB_CORPUS32 name the DLL; skipped when unset) and print hot
// and cold bytes per block plus the per-piece words per site. They assert
// nothing. The synthetic cases hold an upper bound on the per-site cold words
// of the common scalar shapes so a per-site copy of shared slow-path code
// fails them.

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <unordered_set>
#include <vector>
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/linear_fault.hpp"
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "runtime/execution/block_decoder.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

bool link_enabled();

struct Footprint {
    u64 blocks = 0;
    u64 hot_bytes = 0;    // CompiledBlock::code_size of split blocks
    u64 cold_bytes = 0;   // CompiledBlock::cold_size
    u64 whole_bytes = 0;  // every compiled block, hot + cold
    ColdTailFootprint pieces{};
    EmitAttribution attribution{};
};

class ColdStubCorpus {
public:
    explicit ColdStubCorpus(bool is64) : m_is64(is64) {}

    ~ColdStubCorpus() {
        m_jit.reset();
        if (m_memory && m_registered) {
            unregister_linear_reservation(m_memory->linear_base());
        }
    }

    // The production memory model for the bitness (commands/run.cpp): x64 is
    // a 2^46 hardware-protected guest-base window, x86 a 4 GiB guest base.
    bool init() {
        if (m_is64) {
            setenv("VITREO_JIT_LINEAR64", "1", 1);
        } else {
            setenv("VITREO_JIT_LINEAR", "1", 1);
        }
        if (!install_linear_fault_handler()) return false;
        m_memory = std::make_unique<GuestMemory>();
        usize reserved = 0;
        if (m_is64) {
            for (usize c : {usize{1} << 46, usize{1} << 44, usize{1} << 40,
                            usize{1} << 36}) {
                if (m_memory->enable_linear_reservation(c)) {
                    reserved = c;
                    break;
                }
            }
            if (reserved == 0 || !m_memory->enable_linear_hardware_protection()) {
                return false;
            }
        } else {
            reserved = usize{4} << 30;
            if (!m_memory->enable_linear_reservation(reserved)) return false;
        }
        m_registered =
            register_linear_reservation(m_memory->linear_base(), reserved);
        return m_registered;
    }

    bool map_pe(const char* path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::vector<u8> file((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        auto u16_at = [&](usize o) { u16 v; std::memcpy(&v, &file[o], 2); return v; };
        auto u32_at = [&](usize o) { u32 v; std::memcpy(&v, &file[o], 4); return v; };
        auto u64_at = [&](usize o) { u64 v; std::memcpy(&v, &file[o], 8); return v; };
        if (file.size() < 0x400) return false;
        const usize pe = u32_at(0x3C);
        const usize opt = pe + 24;
        const u16 magic = u16_at(opt);
        if ((magic == 0x20B) != m_is64) return false;
        const u16 nsect = u16_at(pe + 6);
        const u16 opt_size = u16_at(pe + 20);
        m_base = m_is64 ? u64_at(opt + 24) : u32_at(opt + 28);
        const u32 image_size = u32_at(opt + 56);
        const usize dirs = opt + (m_is64 ? 112 : 96);
        if (!m_memory->map_region(m_base, image_size,
                                  MemoryProtection::ReadWrite)) {
            return false;
        }
        const usize sect = opt + opt_size;
        std::vector<std::pair<u32, u32>> exec;
        for (u16 i = 0; i < nsect; ++i) {
            const usize s = sect + i * 40u;
            const u32 vsize = u32_at(s + 8);
            const u32 va = u32_at(s + 12);
            const u32 raw_size = u32_at(s + 16);
            const u32 raw = u32_at(s + 20);
            const u32 ch = u32_at(s + 36);
            const u32 n = std::min(raw_size, vsize);
            if (n != 0 && raw + n <= file.size()) {
                (void)m_memory->write_bytes(m_base + va, &file[raw], n);
            }
            if ((ch & 0x20000000u) != 0) exec.push_back({va, vsize});
        }
        for (auto [va, size] : exec) {
            (void)m_memory->protect_region(m_base + va, (size + 0xFFFu) & ~0xFFFu,
                                           MemoryProtection::ReadExecute);
            m_exec.push_back({m_base + va, m_base + va + size});
        }
        if (m_is64) {
            const u32 pdata = u32_at(dirs + 3 * 8);
            const u32 pdata_size = u32_at(dirs + 3 * 8 + 4);
            for (u32 off = 0; off + 12 <= pdata_size; off += 12) {
                u8 raw[4]{};
                if (!m_memory->read_bytes(m_base + pdata + off, raw, 4)) break;
                u32 begin = 0;
                std::memcpy(&begin, raw, 4);
                m_seeds.push_back(m_base + begin);
            }
        } else {
            // MSVC x86 function heads: PUSH EBP; MOV EBP,ESP after padding.
            for (auto [lo, hi] : m_exec) {
                std::vector<u8> text(static_cast<usize>(hi - lo));
                if (!m_memory->read_bytes(lo, text.data(), text.size())) continue;
                for (usize i = 1; i + 3 <= text.size(); ++i) {
                    const u8 p = text[i - 1];
                    if ((p == 0xCC || p == 0xC3 || p == 0x90) &&
                        text[i] == 0x55 && text[i + 1] == 0x8B &&
                        text[i + 2] == 0xEC) {
                        m_seeds.push_back(lo + i);
                    }
                }
            }
        }
        return !m_seeds.empty();
    }

    Footprint compile_corpus(usize max_blocks) {
        reset_cold_tail_footprint();
        reset_cold_lazy_stats();
        reset_emit_attribution();
        set_emit_attribution(true);
        // Production (guest_run_loop.cpp) links blocks; exits differ with it.
        if (link_enabled()) JitCompiler::enable_jit_link();
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_cache = std::make_unique<BlockCache>();
        m_jit->set_block_cache(m_cache.get());
        Footprint fp;
        std::unordered_set<GuestAddr> seen;
        std::deque<GuestAddr> work;
        // Spread the seeds over the whole image rather than its first part.
        const usize stride = std::max<usize>(1, m_seeds.size() / (max_blocks / 4 + 1));
        for (usize i = 0; i < m_seeds.size(); i += stride) work.push_back(m_seeds[i]);
        while (!work.empty() && fp.blocks < max_blocks) {
            const GuestAddr at = work.front();
            work.pop_front();
            if (!in_exec(at) || !seen.insert(at).second) continue;
            BasicBlock block = cli::decode_basic_block(*m_memory, at, m_is64);
            if (block.instruction_count == 0) continue;
            CompiledBlock* cb = m_jit->compile(block);
            if (cb == nullptr) continue;
            ++fp.blocks;
            if (m_dump_every != 0 && fp.blocks % m_dump_every == 0) {
                dump_block(block, *cb);
            }
            fp.whole_bytes += cb->code_size + cb->cold_size;
            if (cb->cold_size != 0) {
                fp.hot_bytes += cb->code_size;
                fp.cold_bytes += cb->cold_size;
            }
            const DecodedInstruction& last =
                block.instructions[block.instruction_count - 1];
            const GuestAddr end = at + block.byte_length;
            for (u8 i = 0; i < last.operand_count; ++i) {
                const OperandType t = last.operands[i].type;
                if (t == OperandType::Rel8 || t == OperandType::Rel16 ||
                    t == OperandType::Rel32) {
                    work.push_back(end + static_cast<u64>(last.operands[i].imm));
                }
            }
            if (!ends_unconditionally(end - last.length)) work.push_back(end);
        }
        fp.pieces = cold_tail_footprint();
        fp.attribution = emit_attribution();
        set_emit_attribution(false);
        if (link_enabled()) JitCompiler::disable_jit_link();
        return fp;
    }

    // Compile `code` as one x64 block at a fixed RW address.
    ColdTailFootprint compile_synthetic(const std::vector<u8>& code) {
        constexpr GuestAddr kAt = 0x0000000140000000ull;
        if (!m_memory->map_region(kAt, 0x4000, MemoryProtection::ReadWrite) ||
            !m_memory->write_bytes(kAt, code.data(), code.size())) {
            return {};
        }
        reset_cold_tail_footprint();
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_cache = std::make_unique<BlockCache>();
        m_jit->set_block_cache(m_cache.get());
        BasicBlock block = cli::decode_basic_block(*m_memory, kAt, true);
        if (block.instruction_count == 0 || m_jit->compile(block) == nullptr) {
            return {};
        }
        return cold_tail_footprint();
    }

    // Total emitted bytes (hot + cold) of one linked x64 block at kAt.
    u64 compile_synthetic_bytes(const std::vector<u8>& code) {
        constexpr GuestAddr kAt = 0x0000000140000000ull;
        if (!m_memory->map_region(kAt, 0x4000, MemoryProtection::ReadWrite) ||
            !m_memory->write_bytes(kAt, code.data(), code.size())) {
            return 0;
        }
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_cache = std::make_unique<BlockCache>();
        m_jit->set_block_cache(m_cache.get());
        JitCompiler::enable_jit_link();
        reset_emit_attribution();
        set_emit_attribution(true);
        BasicBlock block = cli::decode_basic_block(*m_memory, kAt, true);
        const bool ok = block.instruction_count != 0 && m_jit->compile(block) != nullptr;
        const EmitAttribution a = emit_attribution();
        set_emit_attribution(false);
        JitCompiler::disable_jit_link();
        if (!ok) return 0;
        u64 bytes = 0;
        for (u32 c = 0; c < EmitAttribution::Count; ++c) bytes += a.hot[c] + a.cold[c];
        return bytes;
    }

    // VITREO_COLD_STUB_DUMP=N: print every Nth block's guest bytes and its
    // hot and cold words, for tools/jit_bench/decode_jit_disasm.py-style
    // reading.
    void set_dump_every(u64 n) { m_dump_every = n; }

private:
    void dump_block(const BasicBlock& block, const CompiledBlock& cb) {
        std::vector<u8> guest(block.byte_length);
        (void)m_memory->read_bytes(block.start_addr, guest.data(), guest.size());
        std::printf("[dump] block 0x%llx insns=%u hot=%u cold=%u guest=",
                    static_cast<unsigned long long>(block.start_addr),
                    block.instruction_count, cb.code_size, cb.cold_size);
        for (u8 b : guest) std::printf("%02x", b);
        std::printf("\n");
        const auto words = [](const char* tag, const void* p, u32 bytes) {
            const u32* w = static_cast<const u32*>(p);
            for (u32 i = 0; i < bytes / 4; ++i) {
                std::printf("[dump] %s +%u 0x%08x\n", tag, i * 4, w[i]);
            }
        };
        words("hot", cb.code, cb.code_size);
        if (cb.cold_code() != nullptr) words("cold", cb.cold_code(), cb.cold_size);
    }

    u64 m_dump_every = 0;

    bool in_exec(GuestAddr a) const {
        for (auto [lo, hi] : m_exec) if (a >= lo && a < hi) return true;
        return false;
    }

    // JMP and RET end a path; Jcc, CALL and the rest fall through.
    bool ends_unconditionally(GuestAddr insn) {
        u8 b[4]{};
        if (!m_memory->read_bytes(insn, b, sizeof(b))) return true;
        usize i = 0;
        while (i < 3 && (b[i] == 0x66 || b[i] == 0xF2 || b[i] == 0xF3 ||
                         (m_is64 && (b[i] & 0xF0) == 0x40))) {
            ++i;
        }
        const u8 op = b[i];
        if (op == 0xE9 || op == 0xEB || op == 0xC3 || op == 0xC2 || op == 0xCC) {
            return true;
        }
        if (op == 0xFF && i + 1 < 4) {
            const u8 reg = (b[i + 1] >> 3) & 7;
            return reg == 4 || reg == 5;
        }
        return false;
    }

    bool m_is64;
    bool m_registered = false;
    GuestAddr m_base = 0;
    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
    std::unique_ptr<BlockCache> m_cache;
    std::vector<GuestAddr> m_seeds;
    std::vector<std::pair<GuestAddr, GuestAddr>> m_exec;
};

void print_footprint(const char* label, const Footprint& fp) {
    using P = ColdTailFootprint;
    const double blocks = static_cast<double>(std::max<u64>(fp.blocks, 1));
    const double placed = static_cast<double>(std::max<u64>(fp.pieces.split_placed, 1));
    const double sites =
        static_cast<double>(std::max<u64>(fp.pieces.words[P::MemSites], 1));
    std::printf("[cold-stub] %s blocks=%llu placed=%llu whole=%.1fB/block "
                "hot=%.1fB/placed cold=%.1fB/placed mem_sites=%.2f/block\n",
                label, static_cast<unsigned long long>(fp.blocks),
                static_cast<unsigned long long>(fp.pieces.split_placed),
                fp.whole_bytes / blocks, fp.pieces.split_hot_bytes / placed,
                fp.pieces.split_cold_bytes / placed,
                fp.pieces.words[P::MemSites] / blocks);
    static const char* const names[P::Count] = {
        "sites", "lean", "range", "crossing", "subpage", "entry", "call",
        "tail", "vector", "core", "shared_body", "lazy", "deferred_arms",
        "linear_stores", "ud2", "align", "raise_exit", "mid_jcc", "blocks"};
    std::printf("[cold-stub] %s mem words/site:", label);
    for (u32 p = P::MemLean; p <= P::MemLazy; ++p) {
        std::printf(" %s=%.2f", names[p], fp.pieces.words[p] / sites);
    }
    std::printf("\n[cold-stub] %s other cold words/block:", label);
    for (u32 p = P::DeferredArms; p <= P::MidJccExits; ++p) {
        std::printf(" %s=%.2f", names[p], fp.pieces.words[p] / blocks);
    }
    std::printf("\n");
    const ColdLazyStats lazy = cold_lazy_stats();
    std::printf("[cold-stub] %s lazy sites=%llu (%.2f/block) materialized=%llu "
                "(%llu B) side_exits=%llu\n",
                label, static_cast<unsigned long long>(lazy.sites),
                lazy.sites / blocks,
                static_cast<unsigned long long>(lazy.materialized),
                static_cast<unsigned long long>(lazy.materialized_bytes),
                static_cast<unsigned long long>(lazy.side_exits));
    using A = EmitAttribution;
    static const char* const cats[A::Count] = {
        "prologue", "body", "address", "mem_fast", "flags", "gpr_state",
        "rip", "helper", "guard", "exit_link", "exit_probe", "exit_disp",
        "exit_other", "raise", "cold_mem", "cold_arms",
        "other"};
    const A& a = fp.attribution;
    const double ab = static_cast<double>(std::max<u64>(a.blocks, 1));
    double th = 0, tc = 0;
    for (u32 c = 0; c < A::Count; ++c) {
        th += static_cast<double>(a.hot[c]);
        tc += static_cast<double>(a.cold[c]);
    }
    std::printf("[attr] %s blocks=%llu (split tail %llu) hot=%.1fB/block "
                "cold=%.1fB/block\n",
                label, static_cast<unsigned long long>(a.blocks),
                static_cast<unsigned long long>(a.split_blocks), th / ab, tc / ab);
    for (u32 c = 0; c < A::Count; ++c) {
        std::printf("[attr] %s %-10s hot=%7.1fB/block (%4.1f%%) cold=%7.1fB/block (%4.1f%%)\n",
                    label, cats[c], a.hot[c] / ab, 100.0 * a.hot[c] / std::max(th, 1.0),
                    a.cold[c] / ab, 100.0 * a.cold[c] / std::max(tc, 1.0));
    }
}

// VITREO_COLD_STUB_CORPUS_LINK=0 measures without block linking.
bool link_enabled() {
    const char* v = std::getenv("VITREO_COLD_STUB_CORPUS_LINK");
    return v == nullptr || v[0] != '0';
}

usize corpus_blocks() {
    const char* v = std::getenv("VITREO_COLD_STUB_CORPUS_BLOCKS");
    const long n = v != nullptr ? std::atol(v) : 0;
    return n > 0 ? static_cast<usize>(n) : 20000;
}

}  // namespace

TEST(JitColdStubFootprint, ReportCorpus64) {
    const char* path = std::getenv("VITREO_COLD_STUB_CORPUS64");
    if (path == nullptr) GTEST_SKIP() << "VITREO_COLD_STUB_CORPUS64 unset";
    ColdStubCorpus corpus(true);
    ASSERT_TRUE(corpus.init());
    if (const char* d = std::getenv("VITREO_COLD_STUB_DUMP")) {
        corpus.set_dump_every(static_cast<u64>(std::atoll(d)));
    }
    ASSERT_TRUE(corpus.map_pe(path));
    print_footprint("x64", corpus.compile_corpus(corpus_blocks()));
}

// The per-site cold words of the scalar shapes that dominate real code
// (loads and stores of every width, base and base+index, through distinct
// registers). The shared helper decides the sub-page case and publishes RIP,
// so a site keeps only its range/crossing variants, operand moves, the call
// and its tail: measured 18.45 words/site against 45.36 for the per-site
// (VITREO_JIT_COLD_STUB_LEGACY=1) emission.
TEST(JitColdStubFootprint, ScalarSiteColdWordsStayShared) {
    if (jit_cold_stub_legacy()) GTEST_SKIP() << "legacy per-site emission";
    ColdStubCorpus corpus(true);
    ASSERT_TRUE(corpus.init());
    const std::vector<u8> code = {
        0x48, 0x8B, 0x03,              // mov rax,[rbx]
        0x8B, 0x4E, 0x08,              // mov ecx,[rsi+8]
        0x0F, 0xB6, 0x17,              // movzx edx,byte [rdi]
        0x66, 0x8B, 0x45, 0x10,        // mov ax,[rbp+0x10]
        0x4F, 0x8B, 0x04, 0xD1,        // mov r8,[r9+r10*8]
        0x48, 0x89, 0x0B,              // mov [rbx],rcx
        0x89, 0x56, 0x0C,              // mov [rsi+0xC],edx
        0x88, 0x07,                    // mov [rdi],al
        0x66, 0x44, 0x89, 0x4D, 0x18,  // mov [rbp+0x18],r9w
        0x4D, 0x89, 0x1C, 0xD1,        // mov [r9+r10*8],r11
        0xC3,                          // ret
    };
    const ColdTailFootprint fp = corpus.compile_synthetic(code);
    using P = ColdTailFootprint;
    ASSERT_GE(fp.words[P::MemSites], 10u);
    const u64 per_site_words =
        fp.words[P::MemLean] + fp.words[P::MemRange] +
        fp.words[P::MemCrossing] + fp.words[P::MemSubpage] +
        fp.words[P::MemEntry] + fp.words[P::MemCall] + fp.words[P::MemTail] +
        fp.words[P::MemCore] + fp.words[P::MemLazy];
    const double per_site =
        static_cast<double>(per_site_words) / fp.words[P::MemSites];
    std::printf("[cold-stub] synthetic scalar sites=%llu words/site=%.2f\n",
                static_cast<unsigned long long>(fp.words[P::MemSites]), per_site);
    EXPECT_EQ(fp.words[P::MemSubpage], 0u)
        << "a scalar site emitted its own sub-page decision";
    EXPECT_LE(per_site, 24.0);
}

// Lazy cold stubs (JitCompiler::cold_lazy_enabled): every scalar linear64
// site of the shape above keeps only its slot and record (five words), plus
// one two-word lazy entry per block; none of its slow path is emitted.
TEST(JitColdStubFootprint, LazyScalarSitesKeepOnlySlotAndRecord) {
    if (jit_cold_stub_legacy()) GTEST_SKIP() << "legacy per-site emission";
    struct Restore {
        ~Restore() { JitCompiler::set_cold_lazy_for_test(-1); }
    } restore;
    JitCompiler::set_cold_lazy_for_test(1);
    ColdStubCorpus corpus(true);
    ASSERT_TRUE(corpus.init());
    const std::vector<u8> code = {
        0x48, 0x8B, 0x03,        // mov rax,[rbx]
        0x8B, 0x4E, 0x08,        // mov ecx,[rsi+8]
        0x0F, 0xB6, 0x17,        // movzx edx,byte [rdi]
        0x48, 0x89, 0x0B,        // mov [rbx],rcx
        0x89, 0x56, 0x0C,        // mov [rsi+0xC],edx
        0x88, 0x07,              // mov [rdi],al
        0xC3,                    // ret
    };
    const ColdTailFootprint fp = corpus.compile_synthetic(code);
    using P = ColdTailFootprint;
    ASSERT_GE(fp.words[P::MemSites], 6u);
    const u64 sites = fp.words[P::MemSites];
    EXPECT_EQ(fp.words[P::MemLazy], 2u + 5u * sites);
    EXPECT_EQ(fp.words[P::MemEntry] + fp.words[P::MemCall] +
                  fp.words[P::MemTail] + fp.words[P::MemRange] +
                  fp.words[P::MemCrossing] + fp.words[P::MemLean],
              0u);
}

// The shared exit stubs remove a standard-pin x64 block's own L1 probe
// (about 20 words), dispatcher tail (persist + four pin stores + epilogue,
// 10) and prologue loads (6), leaving a 4-word probe entry, a 2-word tail and
// a 2-word entry call: at least 25 words fewer for a RET-ending block.
TEST(JitColdStubFootprint, SharedExitStubsShrinkALinkedBlock) {
    struct Restore {
        ~Restore() { JitCompiler::set_shared_exit_stubs_for_test(-1); }
    } restore;
    const std::vector<u8> code = {
        0x48, 0x8B, 0x03,  // mov rax,[rbx]
        0x48, 0x01, 0xC1,  // add rcx,rax
        0xC3,              // ret
    };
    u64 bytes[2]{};
    for (int mode = 0; mode < 2; ++mode) {
        JitCompiler::set_shared_exit_stubs_for_test(mode);
        ColdStubCorpus corpus(true);
        ASSERT_TRUE(corpus.init());
        bytes[mode] = corpus.compile_synthetic_bytes(code);
        ASSERT_NE(bytes[mode], 0u);
    }
    std::printf("[cold-stub] linked ret block legacy=%lluB shared=%lluB\n",
                static_cast<unsigned long long>(bytes[0]),
                static_cast<unsigned long long>(bytes[1]));
    EXPECT_LE(bytes[1] + 25u * 4u, bytes[0]);
}

TEST(JitColdStubFootprint, ReportCorpus32) {
    const char* path = std::getenv("VITREO_COLD_STUB_CORPUS32");
    if (path == nullptr) GTEST_SKIP() << "VITREO_COLD_STUB_CORPUS32 unset";
    ColdStubCorpus corpus(false);
    ASSERT_TRUE(corpus.init());
    ASSERT_TRUE(corpus.map_pe(path));
    print_footprint("x86", corpus.compile_corpus(corpus_blocks()));
}
