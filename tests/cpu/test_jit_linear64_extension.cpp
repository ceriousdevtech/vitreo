// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// The x64 reservation extension (GuestMemory::enable_linear_reservation with
// extend_to_va_limit): guest addresses above the power-of-two core but below
// linear_size() -- the 47-bit high half where allocators that choose their
// own random addresses put half of their reservations -- are backed at
// linear_base + address and served by the range-by-fault fast path itself:
// the check map covers them, so the map load does not fault and no site is
// redirected to the checked helper. Faults, protection, page crossings and
// translated-code pages behave exactly as in the core.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

class JitLinear64ExtensionTest : public ::testing::Test {
protected:
    static constexpr usize kCore = usize{1} << 40;  // too big for the low host VA, so placed like production
    static constexpr GuestAddr kCode64 = 0x0000000140000000ull;
    // Above the 2^46 core: a hinted pool in the high half (Steam renderer
    // pools land at 0x4087... to 0x7e7f...).
    static constexpr GuestAddr kHigh = 0x00005A1040000000ull;
    static constexpr GuestAddr kHighCode = 0x00006B0000000000ull;

    static void SetUpTestSuite() {
        setenv("VITREO_JIT_LINEAR64", "1", 1);
        ASSERT_TRUE(install_linear_fault_handler());
    }
    static void TearDownTestSuite() { unsetenv("VITREO_JIT_LINEAR64"); }

    void SetUp() override {
        if (const char* knob = std::getenv("VITREO_JIT_RANGE_FAULT");
            knob != nullptr && knob[0] == '0') {
            GTEST_SKIP() << "VITREO_JIT_RANGE_FAULT=0";
        }
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->enable_linear_reservation(kCore, true));
        if (!m_memory->linear_check_map_ends_at_va_limit()) {
            GTEST_SKIP() << "the VA limit is held by another reservation";
        }
        ASSERT_GT(m_memory->linear_size(), kHighCode + 0x10000);
        ASSERT_TRUE(m_memory->enable_linear_hardware_protection());
        ASSERT_TRUE(register_linear_reservation(m_memory->linear_base(),
                                                m_memory->linear_size()));
        ASSERT_TRUE(m_memory->map_region(kCode64, 0x10000,
                                         MemoryProtection::ReadWrite));
        ASSERT_TRUE(m_memory->map_region(kHigh, 0x10000,
                                         MemoryProtection::ReadWrite));
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_cache = std::make_unique<BlockCache>();
        m_jit->set_block_cache(m_cache.get());
    }

    void TearDown() override {
        if (m_memory && m_memory->linear_base() != nullptr) {
            unregister_linear_reservation(m_memory->linear_base());
        }
    }

    BasicBlock build64(GuestAddr at, const std::vector<u8>& code) {
        EXPECT_TRUE(m_memory->write_bytes(at, code.data(), code.size()));
        BasicBlock b;
        b.start_addr = at;
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

    CpuState cpu64(GuestAddr at, GuestAddr rbx) {
        CpuState cpu;
        cpu.init_long_mode();
        cpu.rip = at;
        cpu.gpr.set64(EBX, rbx);
        return cpu;
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
    std::unique_ptr<BlockCache> m_cache;
};

struct Form {
    std::vector<u8> code;
    bool store;
    int width;
};

const Form kForms[] = {
    {{0x48, 0x8B, 0x03}, false, 8},   // mov rax,[rbx]
    {{0x48, 0x89, 0x03}, true, 8},    // mov [rbx],rax
    {{0x8B, 0x03}, false, 4},         // mov eax,[rbx]
    {{0x89, 0x03}, true, 4},          // mov [rbx],eax
    {{0x66, 0x8B, 0x03}, false, 2},   // mov ax,[rbx]
    {{0x66, 0x89, 0x03}, true, 2},    // mov [rbx],ax
    {{0x8A, 0x03}, false, 1},         // mov al,[rbx]
    {{0x88, 0x03}, true, 1},          // mov [rbx],al
    {{0x0F, 0x10, 0x03}, false, 16},  // movups xmm0,[rbx]
    {{0x0F, 0x11, 0x03}, true, 16},   // movups [rbx],xmm0
};

// Every scalar width and the 128-bit form, in the core and in the extension,
// through one compiled site: the values match, and the site is never
// redirected (its map load did not fault for the high-half address).
TEST_F(JitLinear64ExtensionTest, HighHalfAccessesStayOnTheFastPath) {
    GuestAddr code_at = kCode64;
    for (const Form& f : kForms) {
        SCOPED_TRACE(::testing::Message() << "width=" << f.width
                                          << " store=" << f.store);
        code_at += 0x40;
        BasicBlock block = build64(code_at, f.code);
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        const u64 before = range_backpatch_count();
        for (const GuestAddr at : {kHigh + 0x100, kCode64 + 0x8000,
                                   kHigh + 0x7F00}) {
            SCOPED_TRACE(::testing::Message() << std::hex << at);
            ASSERT_TRUE(m_memory->write_u64(at, 0x1122334455667788ull));
            ASSERT_TRUE(m_memory->write_u64(at + 8, 0x99AABBCCDDEEFF00ull));
            CpuState cpu = cpu64(code_at, at);
            cpu.gpr.rax = 0x0123456789ABCDEFull;
            for (int i = 0; i < 16; ++i) {
                cpu.sse.xmm[0].u8_[i] = static_cast<u8>(0xC0u + i);
            }
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu),
                      JitExecResult::Continue);
            ASSERT_FALSE(cpu.pending_raise.active);
            if (f.store) {
                const u64 mask = f.width >= 8 ? ~0ull
                                              : (1ull << (8 * f.width)) - 1;
                const u64 value = f.width == 16 ? 0xC7C6C5C4C3C2C1C0ull
                                                : 0x0123456789ABCDEFull;
                EXPECT_EQ(m_memory->read_u64(at).value(),
                          (0x1122334455667788ull & ~mask) | (value & mask));
                u64 host = 0;
                std::memcpy(&host, m_memory->linear_base() + at, 8);
                EXPECT_EQ(host, m_memory->read_u64(at).value())
                    << "the store did not land at linear_base + address";
            } else if (f.width == 16) {
                EXPECT_EQ(cpu.sse.xmm[0].u64_[0], 0x1122334455667788ull);
                EXPECT_EQ(cpu.sse.xmm[0].u64_[1], 0x99AABBCCDDEEFF00ull);
            } else {
                const u64 mask = f.width == 8 ? ~0ull
                                              : (1ull << (8 * f.width)) - 1;
                EXPECT_EQ(cpu.gpr.rax & mask, 0x1122334455667788ull & mask);
            }
        }
        EXPECT_EQ(range_backpatch_count(), before)
            << "a high-half access inside the extension left the fast path";
    }
}

// Above linear_size() nothing changed: the map load faults, the site is
// redirected once, and the scattered region is served by the helper.
TEST_F(JitLinear64ExtensionTest, AboveTheExtensionTheSiteIsRedirected) {
    constexpr GuestAddr kTop = 0x00007FFFFFFE0000ull;
    ASSERT_GE(kTop, m_memory->linear_size());
    ASSERT_TRUE(m_memory->map_region(kTop, 0x10000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u64(kTop + 0x80, 0x5566778899AABBCCull));
    BasicBlock block = build64(kCode64 + 0x40, {0x48, 0x8B, 0x03});
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const u64 before = range_backpatch_count();
    CpuState cpu = cpu64(kCode64 + 0x40, kTop + 0x80);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu),
              JitExecResult::Continue);
    ASSERT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.rax, 0x5566778899AABBCCull);
    EXPECT_EQ(range_backpatch_count(), before + 1);
}

// Guest faults in the extension are the core's: an uncommitted (reserved)
// page and a NOACCESS page fault with the access direction and address
// (STATUS_ACCESS_VIOLATION, ExceptionInformation[0] = 0 read / 1 write,
// [1] = the address), a read-only page refuses a store and keeps its
// contents, and the faulting load leaves its destination untouched.
TEST_F(JitLinear64ExtensionTest, HighHalfFaultsMatchTheCore) {
    constexpr GuestAddr kReserved = kHigh + 0x100000;
    constexpr GuestAddr kReadOnly = kHigh + 0x200000;
    ASSERT_TRUE(m_memory->map_region(kReserved, 0x10000,
                                     MemoryProtection::None));
    ASSERT_TRUE(m_memory->map_region(kReadOnly, 0x10000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u64(kReadOnly + 0x40, 0xFEEDFACECAFEBEEFull));
    ASSERT_TRUE(m_memory->protect_region(kReadOnly, 0x10000,
                                         MemoryProtection::Read));
    struct Case {
        GuestAddr addr;
        bool store;
        bool faults;
    };
    // Never mapped: the extension's host pages there are still the
    // max-protection-NONE placeholder (GuestMemory::m_linear_ext_materialized).
    constexpr GuestAddr kNeverMapped = 0x0000612345678000ull;
    const Case cases[] = {
        {kNeverMapped, false, true},
        {kNeverMapped, true, true},
        {kReserved + 0x40, false, true},
        {kReserved + 0x40, true, true},
        {kReadOnly + 0x40, false, false},
        {kReadOnly + 0x40, true, true},
    };
    GuestAddr code_at = kCode64;
    for (const Case& c : cases) {
        SCOPED_TRACE(::testing::Message() << std::hex << c.addr
                                          << (c.store ? " store" : " load"));
        code_at += 0x40;
        BasicBlock block = build64(code_at, c.store
                                                ? std::vector<u8>{0x48, 0x89, 0x03}
                                                : std::vector<u8>{0x48, 0x8B, 0x03});
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        CpuState cpu = cpu64(code_at, c.addr);
        cpu.gpr.rax = 0x0123456789ABCDEFull;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu),
                  JitExecResult::Continue);
        if (!c.faults) {
            EXPECT_FALSE(cpu.pending_raise.active);
            EXPECT_EQ(cpu.gpr.rax, 0xFEEDFACECAFEBEEFull);
            continue;
        }
        ASSERT_TRUE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
        EXPECT_EQ(cpu.pending_raise.args[0], c.store ? 1u : 0u);
        EXPECT_EQ(cpu.pending_raise.args[1], c.addr);
        EXPECT_EQ(cpu.gpr.rax, 0x0123456789ABCDEFull);
    }
    EXPECT_EQ(m_memory->read_u64(kReadOnly + 0x40).value(),
              0xFEEDFACECAFEBEEFull);
}

// An access straddling two 16 KiB host pages of the extension: both halves
// come from the right pages (load) or land on them (store).
TEST_F(JitLinear64ExtensionTest, HostPageCrossingInTheHighHalf) {
    const GuestAddr at = kHigh + 0x4000 - 3;
    ASSERT_TRUE(m_memory->write_u64(at - 5, 0x0706050403020100ull));
    ASSERT_TRUE(m_memory->write_u64(at + 3, 0x0F0E0D0C0B0A0908ull));
    BasicBlock load = build64(kCode64 + 0x40, {0x48, 0x8B, 0x03});
    CompiledBlock* lc = m_jit->compile(load);
    ASSERT_NE(lc, nullptr);
    CpuState cpu = cpu64(kCode64 + 0x40, at);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *lc, cpu),
              JitExecResult::Continue);
    ASSERT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.rax, 0x0C0B0A0908070605ull);  // bytes at..at+7

    BasicBlock store = build64(kCode64 + 0x80, {0x48, 0x89, 0x03});
    CompiledBlock* sc = m_jit->compile(store);
    ASSERT_NE(sc, nullptr);
    CpuState scpu = cpu64(kCode64 + 0x80, at);
    scpu.gpr.rax = 0x8877665544332211ull;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *sc, scpu),
              JitExecResult::Continue);
    ASSERT_FALSE(scpu.pending_raise.active);
    EXPECT_EQ(m_memory->read_u64(at).value(), 0x8877665544332211ull);
}

// Code in the extension: a block translated from a high-half page, then a
// store onto that page through an ordinary compiled site, must invalidate it
// (kLinearCheckHoldsCode is published for extension pages like core ones).
TEST_F(JitLinear64ExtensionTest, StoreOntoHighHalfCodeInvalidatesIt) {
    ASSERT_TRUE(m_memory->map_region(kHighCode, 0x10000,
                                     MemoryProtection::ReadWrite));
    const std::vector<u8> victim = {0xB8, 0x11, 0x22, 0x33, 0x44, 0xC3};
    BasicBlock vblock = build64(kHighCode, victim);
    ASSERT_NE(m_jit->compile(vblock), nullptr);
    m_cache->insert(vblock);
    ASSERT_EQ(m_cache->size(), 1u);
    const u8 check =
        m_memory->linear_check_page_map()[kHighCode >> kLinearHostPageShift];
    EXPECT_NE(check & kLinearCheckHoldsCode, 0)
        << "the extension page holding code is not marked in the check map";

    BasicBlock sblock = build64(kCode64 + 0x40, {0x89, 0x03});  // mov [rbx],eax
    CompiledBlock* scb = m_jit->compile(sblock);
    ASSERT_NE(scb, nullptr);
    CpuState cpu = cpu64(kCode64 + 0x40, kHighCode);
    cpu.gpr.rax = 0x90909090u;
    (void)test::execute_like_dispatcher(*m_jit, *scb, cpu);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(m_memory->read_u32(kHighCode).value_or(0), 0x90909090u);
    EXPECT_TRUE(m_jit->synchronize_executable_writes());
    EXPECT_EQ(m_cache->size(), 0u)
        << "a store onto high-half code did not invalidate its translation";
}

}  // namespace
