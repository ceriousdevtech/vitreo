// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — store-ring diagnostic

#include "jit_exec_test_fixture.hpp"

// Intel SDM Vol. 2 MOV / MOVAPS: stores preserve flags and copy the source
// bytes. The optional diagnostic must observe these bytes without changing
// the guest state, including full-width long-mode destination addresses.
TEST_F(JitExecTest, StoreRingObservesFullWidthStoresWithoutChangingGuestState) {
    const std::vector<std::pair<u32, std::vector<u8>>> forms{
        {1, {0x88, 0x07}}, {2, {0x66, 0x89, 0x07}},
        {4, {0x89, 0x07}}, {8, {0x48, 0x89, 0x07}},
        {16, {0x0f, 0x29, 0x07}},
    };
    for (bool x64 : {false, true}) {
        const GuestAddr destination = x64 ? 0x100004000ull : 0x4000ull;
        if (x64) ASSERT_TRUE(m_memory->map_region(destination, 0x1000,
                                                   MemoryProtection::ReadWrite));
        for (const auto& [width, code] : forms) {
            if (!x64 && width == 8) continue;
            SCOPED_TRACE(width);
            SCOPED_TRACE(x64);
            x64 ? m_cpu->init_long_mode() : m_cpu->init_protected_mode();
            m_cpu->rip = 0x1000;
            m_cpu->gpr.set64(EDI, destination);
            m_cpu->gpr.set64(EAX, 0x8877665544332211ull);
            m_cpu->gpr.set64(ESP, 0x10000);
            m_cpu->sse.xmm[0].u64_[0] = 0x8877665544332211ull;
            m_cpu->sse.xmm[0].u64_[1] = 0x1020304050607080ull;
            m_cpu->flags.set_cf(true);
            m_cpu->flags.set_zf(true);
            m_cpu->flags.set_of(true);
            const auto before = *m_cpu;
            m_jit = std::make_unique<JitCompiler>(*m_memory);
            auto prefixed = code;
            prefixed.insert(prefixed.begin(), 0x90); // NOP; store is at +1
            auto block = x64 ? build_block64(prefixed.data(), prefixed.size())
                             : build_block(prefixed.data(), prefixed.size());
            auto* compiled = m_jit->compile(block);
            ASSERT_NE(compiled, nullptr);
            const u64 first = store_ring_cursor().load();
            mem_watch_cur_rip() = 0xdeadbeef;
            ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu), JitExecResult::Continue);
            m_cpu->materialize_lazy_flags();
            EXPECT_TRUE(m_cpu->flags.cf());
            EXPECT_TRUE(m_cpu->flags.zf());
            EXPECT_TRUE(m_cpu->flags.of());
            for (u8 reg = 0; reg < 16; ++reg)
                EXPECT_EQ(m_cpu->gpr.get64(reg), before.gpr.get64(reg));
            const u64 mask = width >= 8 ? ~u64{0} : (u64{1} << (width * 8)) - 1;
            std::array<u8, 16> bytes{};
            ASSERT_TRUE(m_memory->read_bytes(destination, bytes.data(), width));
            u64 low = 0;
            std::memcpy(&low, bytes.data(), std::min<u32>(width, 8));
            EXPECT_EQ(low, before.gpr.get64(EAX) & mask);
            if (width == 16) {
                u64 high = 0;
                std::memcpy(&high, bytes.data() + 8, 8);
                EXPECT_EQ(high, before.sse.xmm[0].u64_[1]);
            }
            if (store_ring_enabled()) {
                std::lock_guard<std::mutex> lock(store_ring_mutex());
                const u64 last = store_ring_cursor().load();
                bool found = false;
                for (u64 i = first; i < last; ++i) {
                    const auto& record = store_ring_buffer()[i & (store_ring_capacity() - 1)];
                    if (record.addr == destination && record.size == std::min<u32>(width, 8)) {
                        EXPECT_EQ(record.value & mask, low);
                        EXPECT_EQ(record.rip, 0x1001u);
                        found = true;
                    }
                }
                EXPECT_TRUE(found);
            }
            mem_watch_cur_rip() = 0;
        }
    }
}

TEST_F(JitExecTest, StoreRingLowWordFilterPreservesGuestStoresAndFlags) {
    const char* filter = std::getenv("VITREO_STORE_RING_MATCH32");
    if (!store_ring_enabled() || !filter || std::strcmp(filter, "7") != 0)
        GTEST_SKIP() << "Run with VITREO_STORE_RING=256 VITREO_STORE_RING_MATCH32=7";
    struct Case { u32 width; u64 value; bool retained; std::vector<u8> code; };
    const std::vector<Case> cases{
        {1, 7, false, {0x88, 0x07}},
        {2, 7, false, {0x66, 0x89, 0x07}},
        {4, 7, true, {0x89, 0x07}},
        {4, 8, false, {0x89, 0x07}},
        {8, 0x1020304000000007ull, true, {0x48, 0x89, 0x07}},
        {8, 0x1020304000000008ull, false, {0x48, 0x89, 0x07}},
    };
    for (bool x64 : {false, true}) {
        const GuestAddr address = x64 ? 0x100004000ull : 0x4000ull;
        if (x64) ASSERT_TRUE(m_memory->map_region(address, 0x1000,
                                                   MemoryProtection::ReadWrite));
        for (const auto& c : cases) {
            if (!x64 && c.width == 8) continue;
            SCOPED_TRACE(c.width);
            SCOPED_TRACE(c.value);
            SCOPED_TRACE(x64);
            x64 ? m_cpu->init_long_mode() : m_cpu->init_protected_mode();
            m_cpu->rip = 0x1000;
            m_cpu->gpr.set64(EDI, address);
            m_cpu->gpr.set64(EAX, c.value);
            m_cpu->gpr.set64(ESP, 0x10000);
            m_cpu->flags.set_cf(true);
            m_cpu->flags.set_zf(true);
            m_cpu->flags.set_of(true);
            const auto before = *m_cpu;
            m_jit = std::make_unique<JitCompiler>(*m_memory);
            const auto block = x64 ? build_block64(c.code.data(), c.code.size())
                                   : build_block(c.code.data(), c.code.size());
            auto* compiled = m_jit->compile(block);
            ASSERT_NE(compiled, nullptr);
            const u64 first = store_ring_cursor().load();
            ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu), JitExecResult::Continue);
            m_cpu->materialize_lazy_flags();
            EXPECT_TRUE(m_cpu->flags.cf());
            EXPECT_TRUE(m_cpu->flags.zf());
            EXPECT_TRUE(m_cpu->flags.of());
            for (u8 reg = 0; reg < 16; ++reg)
                EXPECT_EQ(m_cpu->gpr.get64(reg), before.gpr.get64(reg));
            std::array<u8, 8> bytes{};
            ASSERT_TRUE(m_memory->read_bytes(address, bytes.data(), c.width));
            u64 stored = 0;
            std::memcpy(&stored, bytes.data(), c.width);
            const u64 mask = c.width == 8 ? ~u64{0} : (u64{1} << (c.width * 8)) - 1;
            EXPECT_EQ(stored, c.value & mask);
            std::lock_guard<std::mutex> lock(store_ring_mutex());
            bool found = false;
            for (u64 i = first; i < store_ring_cursor().load(); ++i)
                found |= store_ring_buffer()[i & (store_ring_capacity() - 1)].addr == address;
            EXPECT_EQ(found, c.retained);
        }
    }
}

TEST_F(JitExecTest, StoreRingObservesGuestMemoryScalarAndInteriorBulkWrites) {
    if (!store_ring_enabled()) GTEST_SKIP() << "Requires VITREO_STORE_RING";
    const char* filter = std::getenv("VITREO_STORE_RING_MATCH32");
    if (filter && std::strcmp(filter, "7") != 0) GTEST_SKIP() << "Use filter 7 or unset";
    for (const GuestAddr base : {GuestAddr{0x4000}, GuestAddr{0x100004000ull}}) {
        if (base > 0xffffffffull)
            ASSERT_TRUE(m_memory->map_region(base, 0x1000, MemoryProtection::ReadWrite));
        const std::array<u64, 3> words{0, 0x1020304000000007ull, 0};
        const u64 first = store_ring_cursor().load();
        ASSERT_TRUE(m_memory->write_u32(base + 64, 7));
        ASSERT_TRUE(m_memory->write_u64(base + 72, words[1]));
        ASSERT_TRUE(m_memory->write_bytes(base,
            reinterpret_cast<const u8*>(words.data()), sizeof(words)));
        std::array<u64, 3> actual{};
        ASSERT_TRUE(m_memory->read_bytes(base, reinterpret_cast<u8*>(actual.data()), sizeof(actual)));
        EXPECT_EQ(actual, words);
        bool scalar32 = false, scalar64 = false, interior = false;
        std::lock_guard<std::mutex> lock(store_ring_mutex());
        for (u64 i = first; i < store_ring_cursor().load(); ++i) {
            const auto& record = store_ring_buffer()[i & (store_ring_capacity() - 1)];
            scalar32 |= record.addr == base + 64 && record.value == 7 && record.size == 4;
            scalar64 |= record.addr == base + 72 && record.value == words[1] && record.size == 8;
            interior |= record.addr == base + 8 && record.value == words[1] && record.size == 8;
        }
        EXPECT_TRUE(scalar32);
        EXPECT_TRUE(scalar64);
        EXPECT_TRUE(interior);
    }
}

TEST_F(JitExecTest, StoreRingObservesSlowHelpersAboveTheLinearWindow) {
    if (!store_ring_enabled()) GTEST_SKIP() << "Requires VITREO_STORE_RING";
    if (std::getenv("VITREO_STORE_RING_MATCH32")) GTEST_SKIP() << "Use an unfiltered ring";
    // The long-mode sparse fallback used by allocations above 64 TiB must
    // carry the same full address and diagnostic obligations as inline stores.
    for (const GuestAddr base : {GuestAddr{0x4000}, GuestAddr{0x5eb400b52000ull}}) {
        if (base > 0xffffffffull)
            ASSERT_TRUE(m_memory->map_region(base, 0x1000, MemoryProtection::ReadWrite));
        JitContext context{};
        context.cpu = m_cpu.get();
        context.memory = m_memory.get();
        const u64 first = store_ring_cursor().load();
        jit_mem_write8(&context, base, 0x12);
        jit_mem_write16(&context, base + 8, 0x1234);
        jit_mem_write32(&context, base + 16, 0xbadbad00);
        jit_mem_write64(&context, base + 24, 0xbadbad00badbad00ull);
        EXPECT_EQ(m_memory->read_u8(base).value(), 0x12);
        EXPECT_EQ(m_memory->read_u16(base + 8).value(), 0x1234);
        EXPECT_EQ(m_memory->read_u32(base + 16).value(), 0xbadbad00u);
        EXPECT_EQ(m_memory->read_u64(base + 24).value(), 0xbadbad00badbad00ull);
        std::array<bool, 4> found{};
        std::lock_guard<std::mutex> lock(store_ring_mutex());
        for (u64 i = first; i < store_ring_cursor().load(); ++i) {
            const auto& record = store_ring_buffer()[i & (store_ring_capacity() - 1)];
            for (usize slot = 0; slot < found.size(); ++slot)
                found[slot] |= record.addr == base + slot * 8 && record.size == (1u << slot);
        }
        EXPECT_TRUE(std::all_of(found.begin(), found.end(), [](bool value) { return value; }));
        EXPECT_FALSE(m_cpu->pending_raise.active);
    }
}

TEST_F(JitExecTest, StoreRingSlowHelperUsesTheEmittedStoreInstructionAddress) {
    if (!store_ring_enabled()) GTEST_SKIP() << "Requires VITREO_STORE_RING";
    const char* filter = std::getenv("VITREO_STORE_RING_MATCH32");
    if (filter && std::strcmp(filter, "7") != 0) GTEST_SKIP() << "Use filter 7 or unset";
    constexpr GuestAddr destination = 0x500000004000ull;
    ASSERT_TRUE(m_memory->map_region(destination, 0x1000, MemoryProtection::ReadWrite));
    m_cpu->init_long_mode();
    m_cpu->gpr.set64(EAX, 7);
    m_cpu->gpr.set64(EDI, destination);
    const u8 code[]{0x90, 0x48, 0x89, 0x07}; // NOP; MOV [RDI],RAX
    auto block = build_block64(code, sizeof(code));
    auto* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    mem_watch_cur_rip() = 0xdeadbeef;
    const u64 first = store_ring_cursor().load();
    ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu), JitExecResult::Continue);
    bool exact_site = false;
    std::lock_guard<std::mutex> lock(store_ring_mutex());
    for (u64 i = first; i < store_ring_cursor().load(); ++i) {
        const auto& record = store_ring_buffer()[i & (store_ring_capacity() - 1)];
        exact_site |= record.addr == destination && record.value == 7 && record.rip == 0x1001;
    }
    EXPECT_TRUE(exact_site);
    if (!exact_site) {
        for (u64 i = first; i < store_ring_cursor().load(); ++i) {
            const auto& record = store_ring_buffer()[i & (store_ring_capacity() - 1)];
            std::cerr << "record rip=" << std::hex << record.rip << " addr=" << record.addr
                      << " via=" << record.who << std::dec << "\n";
        }
    }
    mem_watch_cur_rip() = 0;
}

TEST(StoreRingDiagnostic, AddressIntervalIncludesAllOverlappingWidths) {
    const StoreAddressFilter filter{0xff, 0x20, 0x40, true};
    EXPECT_TRUE(filter.matches(0x10020, 1));
    EXPECT_TRUE(filter.matches(0x500000000030, 8));
    EXPECT_TRUE(filter.matches(0x10018, 16));
    EXPECT_FALSE(filter.matches(0x10018, 8));
    EXPECT_FALSE(filter.matches(0x10040, 1));
    EXPECT_FALSE(filter.matches(0x10030, 0));
    EXPECT_TRUE(filter.matches(0x100f0, 0x31));
    EXPECT_FALSE(filter.matches(0x100f0, 0x30));
    EXPECT_TRUE(filter.matches(0xfffffffffffffff0ULL, 0x31));
    EXPECT_TRUE(filter.matches(0x10050, 0x200));
    EXPECT_TRUE(StoreAddressFilter{}.matches(0, 0));
}

TEST(StoreRingDiagnostic, AddressFilterRetainsNarrowZeroAndFullWidthValues) {
    if (!store_ring_enabled() || !std::getenv("VITREO_STORE_RING_ADDRESS") ||
        std::getenv("VITREO_STORE_RING_MATCH32")) {
        GTEST_SKIP() << "Use VITREO_STORE_RING=256 VITREO_STORE_RING_ADDRESS=0xff,0x20,0x40";
    }
    const auto& filter = store_ring_address_filter();
    ASSERT_EQ(filter.mask, 0xffu);
    ASSERT_EQ(filter.lo, 0x20u);
    ASSERT_EQ(filter.hi, 0x40u);
    const auto before = store_ring_cursor().load();
    store_ring_push(0x10020, 1, 0, "test");
    store_ring_push(0x500000000038, 8, 0x123456789abcdef0ULL, "test");
    store_ring_push(0x10040, 4, 7, "test");
    ASSERT_EQ(store_ring_cursor().load(), before + 2);
    std::lock_guard<std::mutex> lock(store_ring_mutex());
    const auto& records = store_ring_buffer();
    EXPECT_EQ(records[before & (store_ring_capacity() - 1)].size, 1u);
    EXPECT_EQ(records[before & (store_ring_capacity() - 1)].value, 0u);
    EXPECT_EQ(records[(before + 1) & (store_ring_capacity() - 1)].value,
              0x123456789abcdef0ULL);
}

TEST(StoreRingDiagnostic, RangeIntersectionIncludesInteriorAndDoesNotOverflow) {
    const StoreRecord record{0x1000, 0, 0, 0, 32, nullptr};
    EXPECT_TRUE(store_record_overlaps(record, 0x1010, 0x1018));
    EXPECT_TRUE(store_record_overlaps(record, 0xff8, 0x1001));
    EXPECT_FALSE(store_record_overlaps(record, 0x1020, 0x1021));
    EXPECT_FALSE(store_record_overlaps(record, 0xff0, 0x1000));
    EXPECT_FALSE(store_record_overlaps(record, 0x1010, 0x1010));
    EXPECT_TRUE(store_record_overlaps(StoreRecord{~u64{0} - 7, 0, 0, 0, 8, nullptr},
                                    ~u64{0} - 3, ~u64{0}));
}

TEST_F(JitExecTest, StoreRingObservesRepBulkFillAndCopyBothBitnesses) {
    if (!store_ring_enabled()) GTEST_SKIP() << "Requires VITREO_STORE_RING";
    const char* filter = std::getenv("VITREO_STORE_RING_MATCH32");
    if (filter && std::strcmp(filter, "7") != 0) GTEST_SKIP() << "Use filter 7 or unset";
    for (bool x64 : {false, true}) {
        const GuestAddr base = x64 ? 0x100004000ull : 0x4000ull;
        if (x64) ASSERT_TRUE(m_memory->map_region(base, 0x1000, MemoryProtection::ReadWrite));
        x64 ? m_cpu->init_long_mode() : m_cpu->init_protected_mode();
        m_cpu->gpr.set64(EAX, 7);
        m_cpu->gpr.set64(EDI, base);
        m_cpu->gpr.set64(ECX, 8);
        m_cpu->flags.set_cf(true);
        m_cpu->flags.set_zf(true);
        const u64 first = store_ring_cursor().load();
        jit_rep_stosd(m_cpu.get(), m_memory.get());
        EXPECT_EQ(m_cpu->gpr.get64(EDI), base + 32);
        EXPECT_EQ(m_cpu->gpr.get64(ECX), 0);
        m_cpu->gpr.set64(ESI, base);
        m_cpu->gpr.set64(EDI, base + 64);
        m_cpu->gpr.set64(ECX, 8);
        jit_rep_movsd(m_cpu.get(), m_memory.get());
        EXPECT_EQ(m_cpu->gpr.get64(ESI), base + 32);
        EXPECT_EQ(m_cpu->gpr.get64(EDI), base + 96);
        EXPECT_EQ(m_cpu->gpr.get64(ECX), 0);
        EXPECT_TRUE(m_cpu->flags.cf());
        EXPECT_TRUE(m_cpu->flags.zf());
        EXPECT_FALSE(m_cpu->pending_raise.active);
        std::array<u32, 8> actual{};
        ASSERT_TRUE(m_memory->read_bytes(base + 64, reinterpret_cast<u8*>(actual.data()), sizeof(actual)));
        for (u32 value : actual) EXPECT_EQ(value, 7);
        bool fill = false, copy = false;
        std::lock_guard<std::mutex> lock(store_ring_mutex());
        for (u64 i = first; i < store_ring_cursor().load(); ++i) {
            const auto& record = store_ring_buffer()[i & (store_ring_capacity() - 1)];
            fill |= record.addr == base + 8 && record.value == 0x0000000700000007ull;
            copy |= record.addr == base + 72 && record.value == 0x0000000700000007ull;
        }
        EXPECT_TRUE(fill);
        EXPECT_TRUE(copy);
    }
}
