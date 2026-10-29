// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Persistent translation store (VITREO_TRANSLATION_STORE=warm): format,
// integrity, identity, invalidation and cold-vs-warm equivalence.

#include "jit_exec_test_fixture.hpp"

#include "core/cpu/jit/jit_compile_memo.hpp"
#include "core/cpu/jit/translation_store.hpp"
#include "core/memory/guest_image_registry.hpp"

#include <crt_externs.h>
#include <dirent.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

using tstats::Event;

// Intel SDM Vol. 2A, DIV r/m8: AL <- AX / r/m8, AH <- AX mod r/m8.
// 100 / 7 = 14 remainder 2, so AX = 0x020E. The 8-bit DIV lowering calls a
// host helper on its divide-error path, so the stored block carries a
// host-address relocation.
constexpr u8 kDivCode[] = {
    0xB8, 0x64, 0x00, 0x00, 0x00,  // mov eax, 100
    0xB1, 0x07,                    // mov cl, 7
    0xF6, 0xF1,                    // div cl
};
constexpr u16 kDivExpectedAx = 0x020E;
// The same instructions with mov eax, 101: AX = (3 << 8) | 14.
constexpr u8 kDivCode101[] = {
    0xB8, 0x65, 0x00, 0x00, 0x00, 0xB1, 0x07, 0xF6, 0xF1,
};
constexpr u16 kDiv101ExpectedAx = 0x030E;
// Intel SDM Vol. 2A, ADC: DEST <- DEST + SRC + CF. `adc eax, 5` (83 /2 ib)
// first in a block reads a CF nothing in the block produced, which the JIT
// answers by calling the process-wide capture-CF stub through JitContext.
constexpr u8 kAdcCode[] = {0x83, 0xD0, 0x05};
constexpr u32 kAdcEaxIn = 100;
constexpr u32 kAdcExpectedEax = 100 + 5 + 1;  // with CF = 1
constexpr GuestAddr kImageBase = 0x1000;
constexpr GuestAddr kImageEnd = 0x11000;
constexpr GuestAddr kX64Block = 0x3000;

u64 outcome_count(TranslationOutcome o) {
    return tstats::totals().compiles[static_cast<usize>(o)];
}

std::vector<std::string> segment_files(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.starts_with("seg-") && name.ends_with(".tss")) out.push_back(e.path().string());
    }
    return out;
}

/// One store directory and warm mode for the life of a test. new_process()
/// models a later process: flushes, drops the store's activation, the
/// in-process memo and the first-translation set, and re-opens the store.
class StoreSession {
public:
    explicit StoreSession(const char* verify = nullptr, const char* dir = nullptr) {
        if (dir != nullptr) {
            m_dir = dir;
            m_owned = false;
        } else {
            char tmpl[] = "/tmp/vitreo-tstore-test-XXXXXX";
            const char* made = ::mkdtemp(tmpl);
            m_dir = made != nullptr ? made : "";
        }
        ::setenv("VITREO_TRANSLATION_STORE_DIR", m_dir.c_str(), 1);
        if (verify != nullptr) {
            ::setenv("VITREO_TRANSLATION_STORE_VERIFY", verify, 1);
        } else {
            ::unsetenv("VITREO_TRANSLATION_STORE_VERIFY");
        }
        set_translation_store_mode_for_test(true, TranslationStoreMode::Warm);
        GuestImageRegistry::set_enabled_for_test(1);
        TranslationStore::reset_for_test();
        tstats::reset_for_test();
        JitCompileMemo::clear_compile_memo();
        JitCompileMemo::enable_compile_memo();
    }
    ~StoreSession() {
        TranslationStore::reset_for_test();
        JitCompileMemo::disable_compile_memo();
        JitCompileMemo::clear_compile_memo();
        GuestImageRegistry::set_enabled_for_test(-1);
        set_translation_store_mode_for_test(false, TranslationStoreMode::Off);
        tstats::reset_for_test();
        ::unsetenv("VITREO_TRANSLATION_STORE_DIR");
        ::unsetenv("VITREO_TRANSLATION_STORE_VERIFY");
        if (m_owned && !m_dir.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(m_dir, ec);
        }
    }
    /// Open the store and wait for its index, eviction and compaction.
    bool open() {
        if (!TranslationStore::active()) return false;
        TranslationStore::wait_index_for_test();
        TranslationStore::wait_gc_for_test();
        return true;
    }
    void new_process() {
        TranslationStore::flush();
        TranslationStore::reset_for_test();
        JitCompileMemo::clear_compile_memo();
        tstats::reset_for_test();
    }
    std::string store_dir() const { return TranslationStore::directory(); }
    const std::string& root() const { return m_dir; }

private:
    std::string m_dir;
    bool m_owned{true};
};

class TranslationStoreTest : public JitExecTest {
protected:
    void register_image(GuestAddr base, GuestAddr end, u64 id) {
        m_memory->images().register_image(GuestImageIdentity{base, end, id, base});
    }
    /// Compile with a fresh compiler (a new thread / process) and execute.
    u16 compile_and_run32(const u8* code, usize len, CompiledBlock** out = nullptr) {
        BasicBlock block = build_block(code, len);
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        CompiledBlock* cb = m_jit->compile(block);
        EXPECT_NE(cb, nullptr);
        if (out) *out = cb;
        if (cb == nullptr) return 0xFFFF;
        m_cpu->rip = kImageBase;
        m_cpu->gpr.set32(EAX, 0);
        m_cpu->gpr.set32(ECX, 0);
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *cb, *m_cpu), JitExecResult::Continue);
        return static_cast<u16>(m_cpu->gpr.get32(EAX));
    }
    u16 compile_and_run64(const u8* code, usize len) {
        BasicBlock block = build_block64_at(kX64Block, code, len);
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        CompiledBlock* cb = m_jit->compile(block);
        EXPECT_NE(cb, nullptr);
        if (cb == nullptr) return 0xFFFF;
        CpuState cpu;
        cpu.init_long_mode();
        cpu.rip = kX64Block;
        cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *cb, cpu), JitExecResult::Continue);
        return static_cast<u16>(cpu.gpr.get64(static_cast<u8>(RegIndex::RAX)));
    }
    /// kAdcCode at kImageBase (x86) or kX64Block (x64), entered with
    /// EAX = kAdcEaxIn and CF = 1, with a fresh compiler. Returns EAX.
    u32 compile_and_run_adc(bool is64) {
        BasicBlock block = is64 ? build_block64_at(kX64Block, kAdcCode, sizeof kAdcCode)
                                : build_block(kAdcCode, sizeof kAdcCode);
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        CompiledBlock* cb = m_jit->compile(block);
        EXPECT_NE(cb, nullptr);
        if (cb == nullptr) return 0xFFFFFFFFu;
        CpuState cpu64;
        if (is64) {
            cpu64.init_long_mode();
            cpu64.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
        }
        CpuState& cpu = is64 ? cpu64 : *m_cpu;
        cpu.rip = is64 ? kX64Block : kImageBase;
        cpu.gpr.set32(EAX, kAdcEaxIn);
        cpu.flags.set_cf(true);
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *cb, cpu), JitExecResult::Continue);
        return cpu.gpr.get32(EAX);
    }
    /// Write the code first, then register the image (a loader maps, then
    /// the image is clean).
    void place_image(const u8* code, usize len, u64 id = 0x1234) {
        ASSERT_TRUE(m_memory->write_bytes(kImageBase, code, len));
        register_image(kImageBase, kImageEnd, id);
    }
};

}  // namespace

// ---- format and primitives --------------------------------------------------

// SipHash-2-4 reference vector (Aumasson & Bernstein, "SipHash: a fast
// short-input PRF", Appendix A): key 00..0f, message 00..0e.
TEST(TranslationStoreFormat, SipHashReferenceVector) {
    u8 key[16];
    for (u8 i = 0; i < 16; ++i) key[i] = i;
    u8 msg[15];
    for (u8 i = 0; i < 15; ++i) msg[i] = i;
    EXPECT_EQ(TranslationStore::siphash24(key, msg, sizeof msg), 0xa129ca6149be45e5ull);
}

// Arm ARM C6.2: MOVZ (sf=1, opc=10) 0xD2800000, MOVN (opc=00) 0x92800000,
// MOVK (opc=11) 0xF2800000; hw in bits 22:21, imm16 in 20:5, Rd in 4:0.
TEST(TranslationStoreFormat, MovImm64EncodingFollowsTheArchitecture) {
    u32 w[4];
    ASSERT_EQ(encode_mov_imm64(16, 0, w), 1u);
    EXPECT_EQ(w[0], 0xD2800010u);
    ASSERT_EQ(encode_mov_imm64(16, ~u64{0}, w), 1u);
    EXPECT_EQ(w[0], 0x92800010u);
    ASSERT_EQ(encode_mov_imm64(16, 0x0000000100020003ull, w), 3u);
    EXPECT_EQ(w[0], 0xD2800070u);  // MOVZ X16, #3
    EXPECT_EQ(w[1], 0xF2A00050u);  // MOVK X16, #2, LSL #16
    EXPECT_EQ(w[2], 0xF2C00030u);  // MOVK X16, #1, LSL #32
}

TEST(TranslationStoreFormat, EmissionRoundTripsAndRejectsTruncation) {
    StoredEmission e;
    e.words = {0xD2800010u, 0xF2A00050u, 0xD503201Fu, 0xD65F03C0u};
    e.hot_end_pos = 4;
    e.link_entry_pos = 1;
    e.call_entry_pos = 0xFFFFFFFFu;
    e.insn_pos_count = 1;
    e.insn_arm_pos = {1};
    e.insn_arm_end = {3};
    e.insn_rips = {0x401000};
    e.insn_flag_recovery.resize(1);
    e.insn_rsp_undo = {0};
    e.raise_entry_plain_pos = ~u64{0};
    e.pin_layout = 7;
    e.instruction_count = 1;
    e.relocs.push_back(StoredHostReloc{0, 2, 16, 0x20000});
    e.process_stubs = stored_stub::kCaptureCf | stored_stub::kRaiseExit;
    e.mem_helpers.push_back(StoredMemHelper{0x40, 4, 1});
    std::vector<u8> bytes;
    encode_stored_emission(e, bytes);
    StoredEmission back;
    ASSERT_TRUE(decode_stored_emission(bytes.data(), bytes.size(), back));
    EXPECT_TRUE(stored_emission_equal(e, back));
    StoredEmission no_stubs = back;
    no_stubs.process_stubs = 0;
    EXPECT_FALSE(stored_emission_equal(e, no_stubs));
    StoredEmission no_helpers = back;
    no_helpers.mem_helpers.clear();
    EXPECT_FALSE(stored_emission_equal(e, no_helpers));
    for (usize cut = 1; cut < bytes.size(); cut += 7) {
        StoredEmission partial;
        EXPECT_FALSE(decode_stored_emission(bytes.data(), bytes.size() - cut, partial))
            << "decode accepted a payload truncated by " << cut;
    }
}

// ---- end to end in one test process ----------------------------------------

TEST_F(TranslationStoreTest, WarmAdoptionMatchesColdAndRelocatesHelpers) {
    StoreSession session;
    place_image(kDivCode, sizeof kDivCode);
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    EXPECT_EQ(outcome_count(TranslationOutcome::Fresh), 1u);
    EXPECT_EQ(tstats::count(Event::Produced), 1u);
    EXPECT_GE(tstats::count(Event::ProducedRelocs), 1u);

    session.new_process();
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    EXPECT_EQ(outcome_count(TranslationOutcome::StoreHit), 1u);
    EXPECT_EQ(outcome_count(TranslationOutcome::Fresh), 0u);
    EXPECT_EQ(tstats::count(Event::Hit), 1u);
}

TEST_F(TranslationStoreTest, X64BlockIsStoredAndAdopted) {
    StoreSession session;
    ASSERT_TRUE(m_memory->write_bytes(kX64Block, kDivCode, sizeof kDivCode));
    register_image(kImageBase, kImageEnd, 0x64);
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run64(kDivCode, sizeof kDivCode), kDivExpectedAx);
    EXPECT_EQ(tstats::count(Event::Produced), 1u);
    session.new_process();
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run64(kDivCode, sizeof kDivCode), kDivExpectedAx);
    EXPECT_EQ(outcome_count(TranslationOutcome::StoreHit), 1u);
}

// Cold-entry counter sites (jit_compiler_cold_residency.hpp) are stored with
// raw ordinals and bound to a counter run by each adopting process's
// finalize_code, through the cold-counter dispatch tail the record names.
TEST_F(TranslationStoreTest, X64ColdCounterSitesAreStoredUnboundAndBoundOnAdoption) {
    JitCompiler::set_cold_residency_for_test(1);
    struct Restore {
        ~Restore() { JitCompiler::set_cold_residency_for_test(-1); }
    } restore;
    const auto bound_sites = [](const CompiledBlock& cb) {
        std::vector<u32> data;
        for (const WordRange& r : block_word_ranges(cb)) {
            for (const u32* w = r.begin; w + 2 < r.end; ++w) {
                if (w[0] == JitCompiler::cold_counter_site_word() &&
                    w[1] == 0xD63F03C0u /* BLR X30 */) {
                    data.push_back(w[2]);
                }
            }
        }
        return data;
    };
    // mov rax, [rbx]: one memory access, whose slow path is a cold stub.
    constexpr u8 kLoad[] = {0x48, 0x8B, 0x03};
    constexpr GuestAddr kData = 0x8000;
    constexpr u64 kValue = 0x0123456789ABCDEFull;
    const auto run = [&](std::vector<u32>& sites) {
        BasicBlock block = build_block64_at(kX64Block, kLoad, sizeof kLoad);
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        CompiledBlock* cb = m_jit->compile(block);
        if (cb == nullptr) return u64{0};
        sites = bound_sites(*cb);
        CpuState cpu;
        cpu.init_long_mode();
        cpu.rip = kX64Block;
        cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
        cpu.gpr.set64(static_cast<u8>(RegIndex::RBX), kData);
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *cb, cpu),
                  JitExecResult::Continue);
        return cpu.gpr.get64(static_cast<u8>(RegIndex::RAX));
    };
    StoreSession session("all");
    ASSERT_TRUE(m_memory->write_bytes(kX64Block, kLoad, sizeof kLoad));
    ASSERT_TRUE(m_memory->write_u64(kData, kValue));
    register_image(kImageBase, kImageEnd, 0x64);
    ASSERT_TRUE(session.open());
    std::vector<u32> fresh;
    EXPECT_EQ(run(fresh), kValue);
    EXPECT_EQ(tstats::count(Event::Produced), 1u);
    if (fresh.empty()) GTEST_SKIP() << "the block has no counted cold piece";

    session.new_process();
    ASSERT_TRUE(session.open());
    std::vector<u32> adopted;
    EXPECT_EQ(run(adopted), kValue);
    EXPECT_EQ(outcome_count(TranslationOutcome::StoreVerified), 1u);
    EXPECT_EQ(tstats::count(Event::VerifyMismatch), 0u);
    ASSERT_EQ(adopted.size(), fresh.size());
    for (usize i = 0; i < adopted.size(); ++i) {
        EXPECT_EQ(adopted[i] & 63u, fresh[i] & 63u) << "ordinal " << i;
        EXPECT_NE(adopted[i] >> 6, 0u) << "site " << i << " left unbound";
        EXPECT_NE(adopted[i] >> 6, fresh[i] >> 6)
            << "site " << i << " shares the producer's counter run";
    }
}

// IR tier-up (jit_compiler_cold_residency.hpp): a tier-0 record is stored
// with its tier counter site unbound and bound to a fresh slot on adoption;
// the tier-1 compile that follows stores its own record, and a later process
// adopts that record as the block's first translation.
TEST_F(TranslationStoreTest, X64TierZeroAndTierOneRecordsRoundTrip) {
    const char* ir_env = std::getenv("VITREO_JIT_IR");
    if (!(ir_env == nullptr || ir_env[0] == '\0' || JitCompiler::ir_enabled_for(true))) {
        GTEST_SKIP() << "VITREO_JIT_IR excludes 64-bit blocks";
    }
    constexpr u32 kThreshold = 4;
    JitCompiler::set_ir_tier_threshold_for_test(kThreshold);
    JitCompiler::reset_ir_tier_for_test();
    struct Restore {
        ~Restore() {
            JitCompiler::reset_ir_tier_for_test();
            JitCompiler::set_ir_tier_threshold_for_test(-1);
        }
    } restore;
    // mov eax, 7 ; add eax, 5
    constexpr u8 kCode[] = {0xB8, 0x07, 0x00, 0x00, 0x00, 0x83, 0xC0, 0x05};
    const auto fresh_cpu = [] {
        CpuState cpu;
        cpu.init_long_mode();
        cpu.rip = kX64Block;
        cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
        cpu.gpr.set64(static_cast<u8>(RegIndex::RAX), 0xFFFFFFFF00000000ull);
        return cpu;
    };
    const auto tier_data = [](const CompiledBlock& cb) {
        std::vector<u32> data;
        for (const WordRange& r : block_word_ranges(cb)) {
            for (const u32* w = r.begin; w + 2 < r.end; ++w) {
                if (w[0] == JitCompiler::tier_counter_site_word() &&
                    w[1] == 0xD63F03C0u /* BLR X30 */) {
                    data.push_back(w[2]);
                }
            }
        }
        return data;
    };
    StoreSession session("all");
    ASSERT_TRUE(m_memory->write_bytes(kX64Block, kCode, sizeof kCode));
    register_image(kImageBase, kImageEnd, 0x64);
    CpuState expected = fresh_cpu();
    {
        Interpreter interp(expected, *m_memory);
        for (usize i = 0; i < 2; ++i) ASSERT_EQ(interp.step(), ExecResult::Continue);
    }
    const BasicBlock block = build_block64_at(kX64Block, kCode, sizeof kCode);
    const auto run = [&](CompiledBlock& cb) {
        CpuState cpu = fresh_cpu();
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, cb, cpu), JitExecResult::Continue);
        EXPECT_EQ(cpu.gpr.get64(static_cast<u8>(RegIndex::RAX)),
                  expected.gpr.get64(static_cast<u8>(RegIndex::RAX)));
        EXPECT_EQ(cpu.rip, expected.rip);
    };

    // Process 1: tier 0, stored unbound.
    ASSERT_TRUE(session.open());
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    BasicBlock b1 = block;
    CompiledBlock* cb = m_jit->compile(b1);
    ASSERT_NE(cb, nullptr);
    EXPECT_TRUE(m_jit->last_compile_was_tier0());
    const std::vector<u32> p1 = tier_data(*cb);
    ASSERT_EQ(p1.size(), 1u);
    EXPECT_NE(p1[0], 0u);
    EXPECT_EQ(tstats::count(Event::Produced), 1u);
    run(*cb);

    // Process 2: the tier-0 record is adopted with a fresh slot ...
    session.new_process();
    JitCompiler::reset_ir_tier_for_test();
    ASSERT_TRUE(session.open());
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    BasicBlock b2 = block;
    cb = m_jit->compile(b2);
    ASSERT_NE(cb, nullptr);
    EXPECT_TRUE(m_jit->last_compile_was_tier0());
    EXPECT_EQ(outcome_count(TranslationOutcome::StoreVerified), 1u);
    EXPECT_EQ(tstats::count(Event::VerifyMismatch), 0u);
    const std::vector<u32> p2 = tier_data(*cb);
    ASSERT_EQ(p2.size(), 1u);
    EXPECT_NE(p2[0], 0u);
    EXPECT_NE(p2[0], p1[0]) << "the adoption shares the producer's slot";
    CompiledBlock tier0 = *cb;
    // ... tiers up, and its tier-1 compile is stored as its own record.
    const auto s0 = JitCompiler::ir_tier_stats();
    for (u32 k = 0; k < kThreshold; ++k) {
        run(tier0);
        m_jit->service_cold_residency();
    }
    EXPECT_EQ(JitCompiler::ir_tier_stats().promotions - s0.promotions, 1u);
    BasicBlock b3 = block;
    cb = m_jit->compile(b3);
    ASSERT_NE(cb, nullptr);
    EXPECT_FALSE(m_jit->last_compile_was_tier0());
    EXPECT_TRUE(m_jit->ir_emit_active());
    EXPECT_TRUE(tier_data(*cb).empty());
    EXPECT_EQ(tstats::count(Event::Produced), 1u) << "the tier-1 record";
    run(*cb);

    // Process 3: the tier-1 record is the first translation.
    session.new_process();
    JitCompiler::reset_ir_tier_for_test();
    ASSERT_TRUE(session.open());
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    const auto s1 = JitCompiler::ir_tier_stats();
    BasicBlock b4 = block;
    cb = m_jit->compile(b4);
    ASSERT_NE(cb, nullptr);
    EXPECT_FALSE(m_jit->last_compile_was_tier0());
    EXPECT_EQ(JitCompiler::ir_tier_stats().store_tier1 - s1.store_tier1, 1u);
    EXPECT_EQ(outcome_count(TranslationOutcome::StoreVerified), 1u);
    EXPECT_EQ(tstats::count(Event::VerifyMismatch), 0u);
    EXPECT_TRUE(tier_data(*cb).empty());
    run(*cb);
}

TEST_F(TranslationStoreTest, ShadowVerificationComparesEveryAdoption) {
    StoreSession session("all");
    place_image(kDivCode, sizeof kDivCode);
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    session.new_process();
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    EXPECT_EQ(outcome_count(TranslationOutcome::StoreVerified), 1u);
    EXPECT_EQ(tstats::count(Event::VerifyMatch), 1u);
    EXPECT_EQ(tstats::count(Event::VerifyMismatch), 0u);
    EXPECT_FALSE(std::filesystem::exists(session.store_dir() + "/poison"));
}

TEST_F(TranslationStoreTest, CorruptRecordIsRejectedAndCompiledFresh) {
    StoreSession session;
    place_image(kDivCode, sizeof kDivCode);
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    const std::string dir = session.store_dir();
    session.new_process();
    const auto segs = segment_files(dir);
    ASSERT_EQ(segs.size(), 1u);
    {
        // Flip one byte inside the record payload (the last 8-byte group).
        const int fd = ::open(segs[0].c_str(), O_RDWR);
        ASSERT_GE(fd, 0);
        struct stat sb{};
        ASSERT_EQ(::fstat(fd, &sb), 0);
        u8 b = 0;
        ASSERT_EQ(::pread(fd, &b, 1, sb.st_size - 12), 1);
        b ^= 0x40;
        ASSERT_EQ(::pwrite(fd, &b, 1, sb.st_size - 12), 1);
        ::close(fd);
    }
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    EXPECT_EQ(tstats::count(Event::RejectMac), 1u);
    EXPECT_EQ(outcome_count(TranslationOutcome::StoreHit), 0u);
    EXPECT_EQ(outcome_count(TranslationOutcome::Fresh), 1u);
}

TEST_F(TranslationStoreTest, TornTailIsIgnored) {
    StoreSession session;
    place_image(kDivCode, sizeof kDivCode);
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    const std::string dir = session.store_dir();
    session.new_process();
    const auto segs = segment_files(dir);
    ASSERT_EQ(segs.size(), 1u);
    struct stat sb{};
    ASSERT_EQ(::stat(segs[0].c_str(), &sb), 0);
    ASSERT_EQ(::truncate(segs[0].c_str(), sb.st_size - 20), 0);
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    EXPECT_EQ(outcome_count(TranslationOutcome::StoreHit), 0u);
    EXPECT_EQ(tstats::count(Event::MissNoRecord), 1u);
}

TEST_F(TranslationStoreTest, ChangedImageBytesMiss) {
    StoreSession session;
    place_image(kDivCode, sizeof kDivCode);
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    session.new_process();
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode101, sizeof kDivCode101), kDiv101ExpectedAx);
    EXPECT_EQ(outcome_count(TranslationOutcome::StoreHit), 0u);
    EXPECT_EQ(outcome_count(TranslationOutcome::Fresh), 1u);
}

TEST_F(TranslationStoreTest, DifferentImageIdentityMisses) {
    StoreSession session;
    place_image(kDivCode, sizeof kDivCode, 0x1111);
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    session.new_process();
    m_memory->images().unregister_overlapping(kImageBase, kImageEnd - kImageBase);
    register_image(kImageBase, kImageEnd, 0x2222);
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    EXPECT_EQ(outcome_count(TranslationOutcome::StoreHit), 0u);
}

TEST_F(TranslationStoreTest, DifferentImageBaseMisses) {
    StoreSession session;
    place_image(kDivCode, sizeof kDivCode);
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    session.new_process();
    // The same image identity and bytes mapped at another base: the record
    // names a different image base, so nothing is rebased or adopted.
    m_memory->images().unregister_overlapping(kImageBase, kImageEnd - kImageBase);
    ASSERT_TRUE(m_memory->map_region(0x20000, 0x10000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_bytes(0x20000, kDivCode, sizeof kDivCode));
    register_image(0x20000, 0x30000, 0x1234);
    ASSERT_TRUE(session.open());
    BasicBlock block = build_block(kDivCode, sizeof kDivCode);
    block.start_addr = 0x20000;
    m_jit = std::make_unique<JitCompiler>(*m_memory);
    ASSERT_NE(m_jit->compile(block), nullptr);
    EXPECT_EQ(outcome_count(TranslationOutcome::StoreHit), 0u);
    EXPECT_EQ(outcome_count(TranslationOutcome::Fresh), 1u);
}

TEST_F(TranslationStoreTest, ProtectionChangeAfterLoadDisablesTheStoreForThePage) {
    StoreSession session;
    place_image(kDivCode, sizeof kDivCode);
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    session.new_process();
    // A guest VirtualProtect on the page (as an unpacker does before
    // rewriting it) makes the page dirty for the rest of the image's life.
    ASSERT_TRUE(m_memory->protect_region(kImageBase, 0x1000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    EXPECT_EQ(outcome_count(TranslationOutcome::StoreHit), 0u);
    EXPECT_EQ(tstats::count(Event::SkipNotImage), 1u);
}

TEST_F(TranslationStoreTest, OnlyTheFirstTranslationInAProcessUsesTheStore) {
    StoreSession session;
    place_image(kDivCode, sizeof kDivCode);
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    session.new_process();
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    EXPECT_EQ(outcome_count(TranslationOutcome::StoreHit), 1u);
    // A re-translation in the same process (after a retirement or a memo
    // flush) compiles fresh.
    JitCompileMemo::clear_compile_memo();
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    EXPECT_EQ(tstats::count(Event::SkipSeen), 1u);
    EXPECT_EQ(outcome_count(TranslationOutcome::Fresh), 1u);
}

TEST_F(TranslationStoreTest, CodeOutsideImagesIsNeverStored) {
    StoreSession session;
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    EXPECT_EQ(tstats::count(Event::SkipNotImage), 1u);
    EXPECT_EQ(tstats::count(Event::Produced), 0u);
}

// A pre-decoded delegate bakes &instructions[i], a heap pointer of this
// process: never stored.
TEST_F(TranslationStoreTest, DelegateBlocksAreNeverStored) {
    StoreSession session;
    const u8 cpuid[] = {0x0F, 0xA2};
    place_image(cpuid, sizeof cpuid);
    ASSERT_TRUE(session.open());
    BasicBlock block = build_block(cpuid, sizeof cpuid);
    ASSERT_NE(m_jit->compile(block), nullptr);
    EXPECT_EQ(tstats::count(Event::Produced), 0u);
    EXPECT_GE(tstats::count(Event::SkipDelegate) + tstats::count(Event::SkipUnrelocatable), 1u);
}

TEST_F(TranslationStoreTest, PoisonStopsAdoptionAndProduction) {
    StoreSession session;
    place_image(kDivCode, sizeof kDivCode);
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    TranslationStore::note_verification(StoredKey{}, false);
    const std::string dir = session.store_dir();
    EXPECT_TRUE(std::filesystem::exists(dir + "/poison"));
    session.new_process();
    ASSERT_TRUE(session.open());
    EXPECT_FALSE(TranslationStore::producing());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    EXPECT_EQ(outcome_count(TranslationOutcome::StoreHit), 0u);
}

TEST_F(TranslationStoreTest, StoreFilesAreOwnerOnly) {
    StoreSession session;
    place_image(kDivCode, sizeof kDivCode);
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    const std::string dir = session.store_dir();
    TranslationStore::flush();
    struct stat sb{};
    ASSERT_EQ(::stat(session.root().c_str(), &sb), 0);
    EXPECT_EQ(sb.st_mode & 0777, 0700u);
    ASSERT_EQ(::stat(dir.c_str(), &sb), 0);
    EXPECT_EQ(sb.st_mode & 0777, 0700u);
    ASSERT_EQ(::stat((session.root() + "/key").c_str(), &sb), 0);
    EXPECT_EQ(sb.st_mode & 0777, 0600u);
    for (const std::string& seg : segment_files(dir)) {
        ASSERT_EQ(::stat(seg.c_str(), &sb), 0);
        EXPECT_EQ(sb.st_mode & 0777, 0600u);
    }
}

TEST_F(TranslationStoreTest, EvictionKeepsTheStoreUnderItsCap) {
    StoreSession session;
    ASSERT_TRUE(session.open());
    const std::string dir = session.store_dir();
    session.new_process();
    // Three dead 1 MB segments (garbage content; eviction is by size and
    // age, and the index refuses them by MAC).
    const std::string blob(1u << 20, 'x');
    for (int i = 0; i < 3; ++i) {
        const std::string path = dir + "/seg-" + std::to_string(1000 + i) + "-1-0000000" +
                                 std::to_string(i) + ".tss";
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(::write(fd, blob.data(), blob.size()), static_cast<ssize_t>(blob.size()));
        ::close(fd);
        struct timeval tv[2] = {{1000 + i, 0}, {1000 + i, 0}};
        ::utimes(path.c_str(), tv);
    }
    ::setenv("VITREO_TRANSLATION_STORE_MAX_MB", "2", 1);
    ASSERT_TRUE(session.open());
    ::unsetenv("VITREO_TRANSLATION_STORE_MAX_MB");
    u64 total = 0;
    const auto segs = segment_files(dir);
    for (const std::string& seg : segs) {
        struct stat sb{};
        ASSERT_EQ(::stat(seg.c_str(), &sb), 0);
        total += static_cast<u64>(sb.st_size);
    }
    EXPECT_LE(total, (2u << 20) / 4 * 3);
    // The oldest went first.
    EXPECT_FALSE(std::filesystem::exists(dir + "/seg-1000-1-00000000.tss"));
}

// ---- across real processes (a fresh host ASLR slide each) -------------------

namespace {

int run_child(const char* role, const std::string& dir, const char* verify,
              const char* test = "TranslationStoreTest.ChildRole",
              const std::vector<std::string>& extra_env = {}) {
    char self[4096];
    u32 size = sizeof self;
    if (_NSGetExecutablePath(self, &size) != 0) return -1;
    std::vector<std::string> env_storage;
    for (char** e = *_NSGetEnviron(); *e != nullptr; ++e) {
        const std::string_view v(*e);
        if (v.starts_with("TSTORE_TEST_CHILD_") ||
            v.starts_with("VITREO_TRANSLATION_STORE")) {
            continue;
        }
        env_storage.emplace_back(*e);
    }
    env_storage.push_back(std::string("TSTORE_TEST_CHILD_ROLE=") + role);
    env_storage.push_back("TSTORE_TEST_CHILD_DIR=" + dir);
    if (verify != nullptr) {
        env_storage.push_back(std::string("TSTORE_TEST_CHILD_VERIFY=") + verify);
    }
    for (const std::string& kv : extra_env) env_storage.push_back(kv);
    std::vector<char*> envp;
    for (std::string& s : env_storage) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::string filter = std::string("--gtest_filter=") + test;
    char* argv[] = {self, filter.data(), nullptr};
    pid_t pid = 0;
    if (posix_spawn(&pid, self, nullptr, nullptr, argv, envp.data()) != 0) return -1;
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/// A child opens the store as the runtime does (TSTORE_TEST_CHILD_NOWAIT:
/// activation only, no test wait for the index or compaction).
bool child_open(StoreSession& session) {
    if (std::getenv("TSTORE_TEST_CHILD_NOWAIT") != nullptr) return TranslationStore::active();
    return session.open();
}

/// TSTORE_TEST_CHILD_RECORDS=n: the index holds exactly n records.
void child_check_records() {
    if (const char* n = std::getenv("TSTORE_TEST_CHILD_RECORDS")) {
        TranslationStore::wait_index_for_test();
        EXPECT_EQ(TranslationStore::indexed_records_for_test(), std::strtoull(n, nullptr, 10));
    }
}

usize count_segments(const std::string& root) {
    usize n = 0;
    std::error_code ec;
    for (const auto& d : std::filesystem::directory_iterator(root, ec)) {
        if (d.is_directory()) n += segment_files(d.path().string()).size();
    }
    return n;
}

}  // namespace

// Runs only when spawned by CrossProcessColdWarmEquivalence.
TEST_F(TranslationStoreTest, ChildRole) {
    const char* role = std::getenv("TSTORE_TEST_CHILD_ROLE");
    const char* dir = std::getenv("TSTORE_TEST_CHILD_DIR");
    if (role == nullptr || dir == nullptr) GTEST_SKIP() << "spawned by the cross-process test";
    StoreSession session(std::getenv("TSTORE_TEST_CHILD_VERIFY"), dir);
    place_image(kDivCode, sizeof kDivCode);
    ASSERT_TRUE(m_memory->write_bytes(kX64Block, kDivCode, sizeof kDivCode));
    ASSERT_TRUE(child_open(session));
    u64 lo = 0, hi = 0;
    TranslationStore::host_image_range(lo, hi);
    std::fprintf(stderr, "[ts-child] role=%s host_image=0x%llx\n", role,
                 static_cast<unsigned long long>(lo));
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    EXPECT_EQ(compile_and_run64(kDivCode, sizeof kDivCode), kDivExpectedAx);
    const std::string_view r(role);
    if (r == "produce") {
        EXPECT_EQ(tstats::count(Event::Produced), 2u);
        EXPECT_GE(tstats::count(Event::ProducedRelocs), 2u);
    } else if (r == "verify") {
        EXPECT_EQ(outcome_count(TranslationOutcome::StoreVerified), 2u);
        EXPECT_EQ(tstats::count(Event::VerifyMatch), 2u);
        EXPECT_EQ(tstats::count(Event::VerifyMismatch), 0u);
    } else {
        EXPECT_EQ(outcome_count(TranslationOutcome::StoreHit), 2u);
        EXPECT_EQ(outcome_count(TranslationOutcome::Fresh), 0u);
        EXPECT_EQ(tstats::count(Event::Produced), 0u);
    }
    child_check_records();
    TranslationStore::flush();
}

TEST(TranslationStoreCrossProcess, ColdWarmEquivalence) {
    char tmpl[] = "/tmp/vitreo-tstore-xproc-XXXXXX";
    const char* made = ::mkdtemp(tmpl);
    ASSERT_NE(made, nullptr);
    const std::string dir = made;
    EXPECT_EQ(run_child("produce", dir, nullptr), 0);
    EXPECT_EQ(run_child("verify", dir, "all"), 0);
    EXPECT_EQ(run_child("adopt", dir, nullptr), 0);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// Runs only when spawned by TranslationStoreCrossProcess.AdoptionPublishesProcessStubs.
TEST_F(TranslationStoreTest, StubChildRole) {
    const char* role = std::getenv("TSTORE_TEST_CHILD_ROLE");
    const char* dir = std::getenv("TSTORE_TEST_CHILD_DIR");
    if (role == nullptr || dir == nullptr) GTEST_SKIP() << "spawned by the cross-process test";
    StoreSession session(nullptr, dir);
    ASSERT_TRUE(m_memory->write_bytes(kImageBase, kAdcCode, sizeof kAdcCode));
    ASSERT_TRUE(m_memory->write_bytes(kX64Block, kAdcCode, sizeof kAdcCode));
    register_image(kImageBase, kImageEnd, 0xADC);
    ASSERT_TRUE(child_open(session));
    EXPECT_EQ(compile_and_run_adc(false), kAdcExpectedEax);
    EXPECT_EQ(compile_and_run_adc(true), kAdcExpectedEax);
    if (std::string_view(role) == "produce") {
        EXPECT_EQ(tstats::count(Event::Produced), 2u);
    } else {
        EXPECT_EQ(outcome_count(TranslationOutcome::StoreHit), 2u);
        EXPECT_EQ(outcome_count(TranslationOutcome::Fresh), 0u);
        EXPECT_EQ(tstats::count(Event::Produced), 0u);
    }
    child_check_records();
    TranslationStore::flush();
}

// The process-wide stubs a block calls through JitContext are published by
// the emitter on first use. A process whose first translation of such a block
// is an adoption never ran the emitter: adoption must publish them, or the
// block's BLR X16 jumps to a null slot (SIGSEGV at PC 0).
TEST(TranslationStoreCrossProcess, AdoptionPublishesProcessStubs) {
    char tmpl[] = "/tmp/vitreo-tstore-stubs-XXXXXX";
    const char* made = ::mkdtemp(tmpl);
    ASSERT_NE(made, nullptr);
    const std::string dir = made;
    constexpr const char* kTest = "TranslationStoreTest.StubChildRole";
    EXPECT_EQ(run_child("produce", dir, nullptr, kTest), 0);
    EXPECT_EQ(run_child("adopt", dir, nullptr, kTest), 0);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// A process's first compiles run before a cold index would be built; the
// activating compile waits for it (bounded), so the second run of a program
// adopts every block the first stored and writes nothing.
TEST(TranslationStoreCrossProcess, SecondRunAdoptsEverythingAndAddsNoSegment) {
    char tmpl[] = "/tmp/vitreo-tstore-second-XXXXXX";
    const char* made = ::mkdtemp(tmpl);
    ASSERT_NE(made, nullptr);
    const std::string dir = made;
    const std::vector<std::string> nowait = {"TSTORE_TEST_CHILD_NOWAIT=1"};
    EXPECT_EQ(run_child("produce", dir, nullptr, "TranslationStoreTest.ChildRole", nowait), 0);
    EXPECT_EQ(count_segments(dir), 1u);
    // adopt: StoreHit == 2 of the 2 produced, Fresh == 0, Produced == 0.
    std::vector<std::string> adopt = nowait;
    adopt.push_back("TSTORE_TEST_CHILD_RECORDS=2");
    for (int run = 0; run < 3; ++run) {
        EXPECT_EQ(run_child("adopt", dir, nullptr, "TranslationStoreTest.ChildRole", adopt), 0);
        EXPECT_EQ(count_segments(dir), 1u) << "adopting run " << run << " wrote a segment";
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// Duplicate records (two processes that stored the same blocks) and many
// small dead segments are merged into one segment holding one copy each,
// relocated from each producer's host slide; later processes adopt from it.
TEST(TranslationStoreCrossProcess, CompactionMergesSegmentsAndDropsDuplicates) {
    char tmpl[] = "/tmp/vitreo-tstore-compact-XXXXXX";
    const char* made = ::mkdtemp(tmpl);
    ASSERT_NE(made, nullptr);
    const std::string dir = made;
    constexpr const char* kDiv = "TranslationStoreTest.ChildRole";
    constexpr const char* kAdc = "TranslationStoreTest.StubChildRole";
    EXPECT_EQ(run_child("produce", dir, nullptr, kDiv), 0);
    EXPECT_EQ(run_child("produce", dir, nullptr, kAdc), 0);
    ASSERT_EQ(count_segments(dir), 2u);
    // Second copies, as concurrent producers of the same blocks would leave.
    std::error_code ec;
    for (const auto& d : std::filesystem::directory_iterator(dir, ec)) {
        if (!d.is_directory()) continue;
        int i = 0;
        for (const std::string& seg : segment_files(d.path().string())) {
            const std::string copy =
                d.path().string() + "/seg-1-1-0000000" + std::to_string(i++) + ".tss";
            std::filesystem::copy_file(seg, copy, ec);
            ASSERT_FALSE(ec) << ec.message();
            ::chmod(copy.c_str(), 0600);
        }
    }
    ASSERT_EQ(count_segments(dir), 4u);
    // This process compacts at activation (4 dead segments, the default
    // minimum) and adopts from the index it built before compacting.
    EXPECT_EQ(run_child("adopt", dir, nullptr, kDiv, {"TSTORE_TEST_CHILD_RECORDS=8"}), 0);
    EXPECT_EQ(count_segments(dir), 1u);
    // The merged segment: 4 records, relocated to the compactor's slide and
    // adopted by processes with their own.
    const std::vector<std::string> four = {"TSTORE_TEST_CHILD_RECORDS=4",
                                           "TSTORE_TEST_CHILD_NOWAIT=1"};
    EXPECT_EQ(run_child("adopt", dir, nullptr, kDiv, four), 0);
    EXPECT_EQ(run_child("adopt", dir, nullptr, kAdc, four), 0);
    EXPECT_EQ(count_segments(dir), 1u);
    std::filesystem::remove_all(dir, ec);
}

// A block compiled before the index was ready is stored only when the index,
// once ready, does not already hold it.
TEST_F(TranslationStoreTest, LateIndexMissIsNotWrittenTwice) {
    StoreSession session;
    place_image(kDivCode, sizeof kDivCode);
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    EXPECT_EQ(tstats::count(Event::Produced), 1u);
    const std::string dir = session.store_dir();
    session.new_process();
    ASSERT_EQ(segment_files(dir).size(), 1u);
    ::setenv("VITREO_TRANSLATION_STORE_INDEX_WAIT_MS", "0", 1);
    TranslationStore::set_index_delay_for_test(300);
    ASSERT_TRUE(TranslationStore::active());
    ::unsetenv("VITREO_TRANSLATION_STORE_INDEX_WAIT_MS");
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    EXPECT_EQ(tstats::count(Event::MissIndexNotReady), 1u);
    TranslationStore::wait_index_for_test();
    TranslationStore::set_index_delay_for_test(0);
    TranslationStore::flush();
    EXPECT_EQ(tstats::count(Event::SkipDuplicate), 1u);
    EXPECT_EQ(segment_files(dir).size(), 1u);
}

// The activating compile waits for an index that is slower than the compile
// but inside the budget, and adopts.
TEST_F(TranslationStoreTest, FirstCompileWaitsForTheIndex) {
    StoreSession session;
    place_image(kDivCode, sizeof kDivCode);
    ASSERT_TRUE(session.open());
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    session.new_process();
    TranslationStore::set_index_delay_for_test(30);
    ASSERT_TRUE(TranslationStore::active());
    TranslationStore::set_index_delay_for_test(0);
    EXPECT_EQ(compile_and_run32(kDivCode, sizeof kDivCode), kDivExpectedAx);
    EXPECT_EQ(tstats::count(Event::MissIndexNotReady), 0u);
    EXPECT_EQ(outcome_count(TranslationOutcome::StoreHit), 1u);
}
