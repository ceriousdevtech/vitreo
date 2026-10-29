/// @file test_tls_processor.cpp
/// @brief Tests for TLS directory processing (PE-006)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <core/pe/loader/tls_processor.hpp>
#include <core/memory/guest_memory.hpp>

using namespace vitreo;
using namespace vitreo::pe;

// =============================================================================
// Fixture
// =============================================================================

class TlsProcessorTest : public ::testing::Test {
protected:
    void SetUp() override {
        mem = std::make_unique<GuestMemory>();

        // Region for TLS directory data (template, index, callbacks)
        auto r1 = mem->map_region(0x10000, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(r1.is_ok());

        // Region for TLS storage allocations (at the processor's base)
        auto r2 = mem->map_region(0x00800000, 0x100000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(r2.is_ok());

        proc = std::make_unique<TlsProcessor>(*mem);
    }

    std::unique_ptr<GuestMemory> mem;
    std::unique_ptr<TlsProcessor> proc;
};

// =============================================================================
// Tests
// =============================================================================

// 1. ParseEmptyDirectory — all-zero directory returns success with empty data
TEST_F(TlsProcessorTest, ParseEmptyDirectory) {
    TlsDirectory dir{};
    auto res = proc->parse(0x400000, dir);
    ASSERT_TRUE(res.is_ok());

    const auto& info = res.value();
    EXPECT_TRUE(info.template_data.empty());
    EXPECT_TRUE(info.callbacks.empty());
    EXPECT_EQ(info.zero_fill_size, 0u);
}

// 2. ParseWithTemplateData — 8 bytes of template data round-trip
TEST_F(TlsProcessorTest, ParseWithTemplateData) {
    const u8 pattern[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};
    auto wr = mem->write_bytes(0x10100, pattern, sizeof(pattern));
    ASSERT_TRUE(wr.is_ok());

    TlsDirectory dir{};
    dir.raw_data_start     = 0x10100;
    dir.raw_data_end       = 0x10108;
    dir.address_of_index   = 0x10200;
    dir.address_of_callbacks = 0;

    auto res = proc->parse(0x400000, dir);
    ASSERT_TRUE(res.is_ok());

    const auto& td = res.value().template_data;
    ASSERT_EQ(td.size(), 8u);
    for (usize i = 0; i < sizeof(pattern); ++i) {
        EXPECT_EQ(td[i], pattern[i]) << "mismatch at byte " << i;
    }
}

// 3. TlsIndexWrittenToMemory — index is stored at address_of_index
TEST_F(TlsProcessorTest, TlsIndexWrittenToMemory) {
    TlsDirectory dir{};
    dir.raw_data_start     = 0x10100;
    dir.raw_data_end       = 0x10100; // zero template
    dir.address_of_index   = 0x10200;
    dir.address_of_callbacks = 0;

    // First parse → index 0
    auto res = proc->parse(0x400000, dir);
    ASSERT_TRUE(res.is_ok());
    EXPECT_EQ(res.value().tls_index, 0u);

    auto idx = mem->read_u32(0x10200);
    ASSERT_TRUE(idx.is_ok());
    EXPECT_EQ(idx.value(), 0u);
}

// 4. CallbacksExtracted — two callbacks + null terminator
TEST_F(TlsProcessorTest, CallbacksExtracted) {
    const GuestAddr cb_table = 0x10300;
    ASSERT_TRUE(mem->write_u64(cb_table,      0x401000).is_ok());
    ASSERT_TRUE(mem->write_u64(cb_table + 8,  0x402000).is_ok());
    ASSERT_TRUE(mem->write_u64(cb_table + 16, 0).is_ok()); // terminator

    TlsDirectory dir{};
    dir.address_of_callbacks = cb_table;
    dir.address_of_index     = 0x10200;

    auto res = proc->parse(0x400000, dir);
    ASSERT_TRUE(res.is_ok());

    const auto& cbs = res.value().callbacks;
    ASSERT_EQ(cbs.size(), 2u);
    EXPECT_EQ(cbs[0], 0x401000u);
    EXPECT_EQ(cbs[1], 0x402000u);

    // Also reflected in pending_callbacks()
    const auto& pending = proc->pending_callbacks();
    ASSERT_EQ(pending.size(), 2u);
    EXPECT_EQ(pending[0], 0x401000u);
    EXPECT_EQ(pending[1], 0x402000u);
}

// 5. NoCallbacks — immediate null terminator
TEST_F(TlsProcessorTest, NoCallbacks) {
    const GuestAddr cb_table = 0x10300;
    ASSERT_TRUE(mem->write_u64(cb_table, 0).is_ok());

    TlsDirectory dir{};
    dir.address_of_callbacks = cb_table;
    dir.address_of_index     = 0x10200;

    auto res = proc->parse(0x400000, dir);
    ASSERT_TRUE(res.is_ok());
    EXPECT_TRUE(res.value().callbacks.empty());
    EXPECT_TRUE(proc->pending_callbacks().empty());
}

// 6. TemplateCopiedCorrectly — byte-by-byte verification
TEST_F(TlsProcessorTest, TemplateCopiedCorrectly) {
    constexpr usize len = 16;
    u8 data[len];
    for (usize i = 0; i < len; ++i) {
        data[i] = static_cast<u8>(i * 7 + 3);
    }
    ASSERT_TRUE(mem->write_bytes(0x10100, data, len).is_ok());

    TlsDirectory dir{};
    dir.raw_data_start     = 0x10100;
    dir.raw_data_end       = 0x10100 + len;
    dir.address_of_index   = 0x10200;
    dir.address_of_callbacks = 0;

    auto res = proc->parse(0x400000, dir);
    ASSERT_TRUE(res.is_ok());

    const auto& td = res.value().template_data;
    ASSERT_EQ(td.size(), len);
    for (usize i = 0; i < len; ++i) {
        EXPECT_EQ(td[i], data[i]) << "mismatch at index " << i;
    }
}

// 7. ZeroFillRecorded — zero_fill_size propagated into TlsInfo
TEST_F(TlsProcessorTest, ZeroFillRecorded) {
    TlsDirectory dir{};
    dir.raw_data_start       = 0x10100;
    dir.raw_data_end         = 0x10104; // 4 bytes template
    dir.address_of_index     = 0x10200;
    dir.address_of_callbacks = 0;
    dir.zero_fill_size       = 16;

    // Write 4 bytes so parse succeeds
    ASSERT_TRUE(mem->write_u32(0x10100, 0xAABBCCDD).is_ok());

    auto res = proc->parse(0x400000, dir);
    ASSERT_TRUE(res.is_ok());
    EXPECT_EQ(res.value().zero_fill_size, 16u);
}

// 8. AllocateThreadTls — template data readable at returned address
TEST_F(TlsProcessorTest, AllocateThreadTls) {
    const u8 pattern[] = {0x11, 0x22, 0x33, 0x44};
    ASSERT_TRUE(mem->write_bytes(0x10100, pattern, sizeof(pattern)).is_ok());

    TlsDirectory dir{};
    dir.raw_data_start       = 0x10100;
    dir.raw_data_end         = 0x10104;
    dir.address_of_index     = 0x10200;
    dir.address_of_callbacks = 0;
    dir.zero_fill_size       = 0;

    auto parse_res = proc->parse(0x400000, dir);
    ASSERT_TRUE(parse_res.is_ok());

    // Unmap the pre-mapped storage region so allocate_thread_tls can map it
    auto unmap_res = mem->unmap_region(0x00800000, 0x100000);
    ASSERT_TRUE(unmap_res.is_ok());

    auto alloc_res = proc->allocate_thread_tls(parse_res.value());
    ASSERT_TRUE(alloc_res.is_ok());

    const GuestAddr addr = alloc_res.value();
    for (usize i = 0; i < sizeof(pattern); ++i) {
        auto b = mem->read_u8(addr + i);
        ASSERT_TRUE(b.is_ok());
        EXPECT_EQ(b.value(), pattern[i]) << "mismatch at offset " << i;
    }
}

// 9. MultipleAllocationsIndependent — writes to one don't affect the other
TEST_F(TlsProcessorTest, MultipleAllocationsIndependent) {
    const u8 tmpl[] = {0xAA, 0xBB};
    ASSERT_TRUE(mem->write_bytes(0x10100, tmpl, sizeof(tmpl)).is_ok());

    TlsDirectory dir{};
    dir.raw_data_start       = 0x10100;
    dir.raw_data_end         = 0x10102;
    dir.address_of_index     = 0x10200;
    dir.address_of_callbacks = 0;

    auto parse_res = proc->parse(0x400000, dir);
    ASSERT_TRUE(parse_res.is_ok());
    const auto& info = parse_res.value();

    // Unmap pre-mapped region
    auto unmap_res = mem->unmap_region(0x00800000, 0x100000);
    ASSERT_TRUE(unmap_res.is_ok());

    auto a1 = proc->allocate_thread_tls(info);
    ASSERT_TRUE(a1.is_ok());
    auto a2 = proc->allocate_thread_tls(info);
    ASSERT_TRUE(a2.is_ok());

    // Addresses must differ
    EXPECT_NE(a1.value(), a2.value());

    // Overwrite first allocation
    ASSERT_TRUE(mem->write_u8(a1.value(), 0xFF).is_ok());

    // Second allocation is untouched
    auto b = mem->read_u8(a2.value());
    ASSERT_TRUE(b.is_ok());
    EXPECT_EQ(b.value(), 0xAA);
}

// 10. SequentialTlsIndices — successive parses get incrementing indices
TEST_F(TlsProcessorTest, SequentialTlsIndices) {
    TlsDirectory dir{};
    dir.raw_data_start       = 0x10100;
    dir.raw_data_end         = 0x10100; // empty template
    dir.address_of_index     = 0x10200;
    dir.address_of_callbacks = 0;

    auto r0 = proc->parse(0x400000, dir);
    ASSERT_TRUE(r0.is_ok());
    EXPECT_EQ(r0.value().tls_index, 0u);

    // Move index target so we can verify second write
    dir.address_of_index = 0x10204;
    auto r1 = proc->parse(0x400000, dir);
    ASSERT_TRUE(r1.is_ok());
    EXPECT_EQ(r1.value().tls_index, 1u);

    // Verify via memory
    auto v0 = mem->read_u32(0x10200);
    ASSERT_TRUE(v0.is_ok());
    EXPECT_EQ(v0.value(), 0u);

    auto v1 = mem->read_u32(0x10204);
    ASSERT_TRUE(v1.is_ok());
    EXPECT_EQ(v1.value(), 1u);
}
