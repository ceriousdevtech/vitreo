// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// The native-call SP floor and the JIT fault-metadata units: the JIT code-range
// table, the precise RIP map and flag-recovery descriptors. The rest of the
// linear suite lives in the test_jit_linear*.cpp files split from this one;
// shared fixtures and matchers live in jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

TEST(NativeCallFloor, ReservesStackAndCapsContinuationFrames) {
    constexpr uintptr_t low = 0x10000000u;
    constexpr uintptr_t high = low + 1024u * 1024u;
    constexpr uintptr_t safe_low = low + kNativeCallStackReserve;
    constexpr uintptr_t span =
        uintptr_t{kNativeCallMaxFrames} * kNativeCallFrameSize;

    // Deep in a big stack the frame cap binds: at most kNativeCallMaxFrames
    // frames below the entry, and the pad pushes while SP >= floor.
    EXPECT_EQ(native_sp_floor_for_stack(high, low, high),
              high - span + kNativeCallFrameSize);
    // Near the bottom the reserve binds instead.
    EXPECT_EQ(native_sp_floor_for_stack(safe_low + 3 * kNativeCallFrameSize,
                                        low, high),
              safe_low + kNativeCallFrameSize);
    // No room for even one frame above the reserve.
    EXPECT_EQ(native_sp_floor_for_stack(safe_low, low, high),
              kNativeCallNoFrames);
}

TEST(NativeCallFloor, InvalidBoundsAndStackPointersFailClosed) {
    constexpr uintptr_t low = 0x20000000u;
    constexpr uintptr_t high = low + kNativeCallStackReserve;

    EXPECT_EQ(native_sp_floor_for_stack(low, low, low), kNativeCallNoFrames);
    EXPECT_EQ(native_sp_floor_for_stack(low, low, high), kNativeCallNoFrames);
    EXPECT_EQ(native_sp_floor_for_stack(low - 1, low, high + 0x1000u),
              kNativeCallNoFrames);
    EXPECT_EQ(native_sp_floor_for_stack(high + 0x1001u, low, high + 0x1000u),
              kNativeCallNoFrames);
    // The fail-closed floor is above every user-space SP, so SP - floor is
    // negative and the pad's TBNZ #63 skips the push.
    EXPECT_NE(static_cast<u64>(uintptr_t{0x16F000000} - kNativeCallNoFrames) &
                  (u64{1} << 63),
              0u);
}

TEST(JitLinearFaultOwnershipTest, DisjointJitRangesDoNotOwnEnvelopeGap) {
    constexpr uintptr_t first_lo = 0x0000010000000000ull;
    constexpr uintptr_t first_hi = first_lo + 0x4000;
    constexpr uintptr_t second_lo = first_lo + 0x10000;
    constexpr uintptr_t second_hi = second_lo + 0x4000;

    ASSERT_TRUE(note_jit_code_range(first_lo, first_hi));
    ASSERT_TRUE(note_jit_code_range(second_lo, second_hi));

    EXPECT_FALSE(jit_code_range_contains(first_lo - 1));
    EXPECT_TRUE(jit_code_range_contains(first_lo));
    EXPECT_TRUE(jit_code_range_contains(first_hi - 1));
    EXPECT_FALSE(jit_code_range_contains(first_hi));
    EXPECT_FALSE(jit_code_range_contains(first_hi + 0x1000));
    EXPECT_TRUE(jit_code_range_contains(second_lo));
    EXPECT_FALSE(jit_code_range_contains(second_hi));

    ASSERT_TRUE(forget_jit_code_range(first_lo, first_hi));
    EXPECT_FALSE(jit_code_range_contains(first_lo));
    EXPECT_TRUE(jit_code_range_contains(second_lo));
    ASSERT_TRUE(forget_jit_code_range(second_lo, second_hi));
    EXPECT_FALSE(jit_code_range_contains(second_lo));
}

TEST(JitLinearFaultOwnershipTest,
     PreciseRipMapUsesHalfOpenIntervalsAndRetires) {
    alignas(16) std::array<u8, 32> native{};
    const uintptr_t base = reinterpret_cast<uintptr_t>(native.data());
    const std::array<JitRipSite, 2> sites{{
        JitRipSite{4, 12, 0x140001000ull},
        JitRipSite{16, 28, 0x140001005ull},
    }};
    JitPinMap pins = kNoJitPins;
    pins.host_reg[8] = 8;
    pins.host_reg[9] = 9;
    JitPinMap duplicate_pin = pins;
    duplicate_pin.host_reg[9] = 8;
    EXPECT_FALSE(publish_jit_rip_block(
        base, static_cast<u32>(native.size()), sites.data(),
        static_cast<u32>(sites.size()), duplicate_pin));
    JitPinMap invalid_host = pins;
    invalid_host.host_reg[9] = 29;
    EXPECT_FALSE(publish_jit_rip_block(
        base, static_cast<u32>(native.size()), sites.data(),
        static_cast<u32>(sites.size()), invalid_host));
    ASSERT_TRUE(publish_jit_rip_block(
        base, static_cast<u32>(native.size()), sites.data(),
        static_cast<u32>(sites.size()), pins));

    GuestAddr rip = 0;
    EXPECT_FALSE(jit_guest_rip_for_pc(base + 3, rip));
    ASSERT_TRUE(jit_guest_rip_for_pc(base + 4, rip));
    EXPECT_EQ(rip, 0x140001000ull);
    JitFaultMetadata recovered{};
    ASSERT_TRUE(jit_fault_metadata_for_pc(base + 16, recovered));
    ASSERT_TRUE(recovered.has_guest_rip);
    EXPECT_EQ(recovered.guest_rip, 0x140001005ull);
    EXPECT_EQ(recovered.pins.host_reg[8], 8u);
    EXPECT_EQ(recovered.pins.host_reg[9], 9u);
    EXPECT_EQ(recovered.pins.host_reg[0], 0xFFu);
    ASSERT_TRUE(jit_fault_metadata_for_pc(base + 12, recovered));
    EXPECT_FALSE(recovered.has_guest_rip);
    EXPECT_EQ(recovered.pins.host_reg[8], 8u);
    ASSERT_TRUE(jit_guest_rip_for_pc(base + 11, rip));
    EXPECT_EQ(rip, 0x140001000ull);
    EXPECT_FALSE(jit_guest_rip_for_pc(base + 12, rip));
    ASSERT_TRUE(jit_guest_rip_for_pc(base + 16, rip));
    EXPECT_EQ(rip, 0x140001005ull);
    EXPECT_FALSE(jit_guest_rip_for_pc(base + 28, rip));

    retire_jit_rip_range(base, base + native.size());
    EXPECT_FALSE(jit_guest_rip_for_pc(base + 4, rip));
}

// The map is bucketed by the 64 KiB host granule a block STARTS in, so a PC in
// a later granule of a long block is found only by walking back. Blocks here
// straddle one boundary and span five granules; retiring one range must keep
// its neighbours. Addresses come from a PROT_NONE reservation: the map never
// reads the code bytes, and no real JIT block can be published there.
TEST(JitLinearFaultOwnershipTest, PreciseRipMapFindsBlocksAcrossHostGranules) {
    constexpr usize kGranule = usize{1} << 16;
    constexpr usize kReserve = 16 * kGranule;
    void* const raw = mmap(nullptr, kReserve, PROT_NONE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(raw, MAP_FAILED);
    const uintptr_t base =
        (reinterpret_cast<uintptr_t>(raw) + kGranule - 1) & ~(kGranule - 1);

    // A: 0x0F000..0x12000, straddling granules 0 and 1.
    const std::array<JitRipSite, 1> a_sites{{JitRipSite{0x2F00, 0x3000, 0xA000}}};
    ASSERT_TRUE(publish_jit_rip_block(base + 0x0F000, 0x3000, a_sites.data(), 1,
                                      kNoJitPins));
    // B: 0x20000..0x70000, granules 2 through 6; its last site is in granule 6.
    const std::array<JitRipSite, 2> b_sites{{
        JitRipSite{0x0, 0x10, 0xB000},
        JitRipSite{0x4FF00, 0x50000, 0xB0FF},
    }};
    ASSERT_TRUE(publish_jit_rip_block(base + 0x20000, 0x50000, b_sites.data(),
                                      2, kNoJitPins));
    // C: 0x70000..0x70100, starting exactly where B ends.
    const std::array<JitRipSite, 1> c_sites{{JitRipSite{0x0, 0x100, 0xC000}}};
    ASSERT_TRUE(publish_jit_rip_block(base + 0x70000, 0x100, c_sites.data(), 1,
                                      kNoJitPins));

    GuestAddr rip = 0;
    ASSERT_TRUE(jit_guest_rip_for_pc(base + 0x11F00, rip));
    EXPECT_EQ(rip, 0xA000u);
    JitFaultMetadata md{};
    EXPECT_FALSE(jit_fault_metadata_for_pc(base + 0x12000, md))
        << "the first byte past A belongs to no block";
    ASSERT_TRUE(jit_guest_rip_for_pc(base + 0x20008, rip));
    EXPECT_EQ(rip, 0xB000u);
    ASSERT_TRUE(jit_guest_rip_for_pc(base + 0x6FF10, rip));
    EXPECT_EQ(rip, 0xB0FFu);
    ASSERT_TRUE(jit_fault_metadata_for_pc(base + 0x40000, md))
        << "a PC between B's sites is block-owned gap code";
    EXPECT_FALSE(md.has_guest_rip);
    ASSERT_TRUE(jit_guest_rip_for_pc(base + 0x70000, rip));
    EXPECT_EQ(rip, 0xC000u);

    retire_jit_rip_range(base + 0x20000, base + 0x70000);
    EXPECT_FALSE(jit_fault_metadata_for_pc(base + 0x6FF10, md));
    EXPECT_FALSE(jit_fault_metadata_for_pc(base + 0x20008, md));
    ASSERT_TRUE(jit_guest_rip_for_pc(base + 0x11F00, rip));
    EXPECT_EQ(rip, 0xA000u);
    ASSERT_TRUE(jit_guest_rip_for_pc(base + 0x70000, rip));
    EXPECT_EQ(rip, 0xC000u);

    retire_jit_rip_range(base, base + 0x80000);
    EXPECT_FALSE(jit_fault_metadata_for_pc(base + 0x11F00, md));
    EXPECT_FALSE(jit_fault_metadata_for_pc(base + 0x70000, md));
    munmap(raw, kReserve);
}

// A split-placed block (jit_split_layout.hpp) owns two ranges: its hot part
// and, higher in the same 32 KiB window, its cold part. Blocks compiled after
// it in the window put their hot parts in the gap between A's two ranges and
// their cold parts below A's, so the gap must resolve to them -- by exact
// ranges, never by "first node whose span covers the PC". A's cold part here
// also starts in the next 64 KiB granule, so its lookup walks back.
TEST(JitLinearFaultOwnershipTest, PreciseRipMapResolvesSplitBlocksByBothRanges) {
    constexpr usize kGranule = usize{1} << 16;
    constexpr usize kReserve = 4 * kGranule;
    void* const raw = mmap(nullptr, kReserve, PROT_NONE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(raw, MAP_FAILED);
    const uintptr_t base =
        (reinterpret_cast<uintptr_t>(raw) + kGranule - 1) & ~(kGranule - 1);
    // One window at base+0xC000..+0x14000. A: hot [0xC000, 0xC100), cold
    // [0x13E00, 0x14000). B: hot [0xC100, 0xC180), cold [0x13D00, 0x13E00).
    const uintptr_t a = base + 0xC000;
    const uintptr_t b = base + 0xC100;
    JitPinMap a_pins = kNoJitPins;
    a_pins.host_reg[0] = 23;
    JitPinMap b_pins = kNoJitPins;
    b_pins.host_reg[0] = 24;

    const std::array<JitRipSite, 2> a_sites{{
        JitRipSite{0x10, 0x40, 0xA000},       // hot body
        JitRipSite{0x7E40, 0x7E60, 0xA007},   // a site in the cold part
    }};
    // Rejected: the cold range overlaps the hot one; a site in the gap.
    EXPECT_FALSE(publish_jit_rip_block(a, 0x100, a_sites.data(), 2, a_pins,
                                       0x80, 0x200));
    const std::array<JitRipSite, 1> gap_site{{JitRipSite{0x200, 0x210, 0xA000}}};
    EXPECT_FALSE(publish_jit_rip_block(a, 0x100, gap_site.data(), 1, a_pins,
                                       0x7E00, 0x200));
    // Rejected: the cold range leaves the 16-bit offsets a window allows.
    EXPECT_FALSE(publish_jit_rip_block(a, 0x100, a_sites.data(), 1, a_pins,
                                       0xFF00, 0x200));
    ASSERT_TRUE(publish_jit_rip_block(a, 0x100, a_sites.data(), 2, a_pins,
                                      0x7E00, 0x200));
    const std::array<JitRipSite, 1> b_sites{{JitRipSite{0x0, 0x20, 0xB000}}};
    ASSERT_TRUE(publish_jit_rip_block(b, 0x80, b_sites.data(), 1, b_pins,
                                      0x7C00, 0x100));

    JitFaultMetadata md{};
    ASSERT_TRUE(jit_fault_metadata_for_pc(a + 0x10, md));
    ASSERT_TRUE(md.has_guest_rip);
    EXPECT_EQ(md.guest_rip, 0xA000u);
    EXPECT_EQ(md.pins.host_reg[0], 23u);
    ASSERT_TRUE(jit_fault_metadata_for_pc(a + 0xF0, md));
    EXPECT_FALSE(md.has_guest_rip) << "hot stub code owns no guest RIP";
    EXPECT_EQ(md.pins.host_reg[0], 23u);
    // The gap belongs to B (its hot part) or to nobody.
    ASSERT_TRUE(jit_fault_metadata_for_pc(b + 0x8, md));
    ASSERT_TRUE(md.has_guest_rip);
    EXPECT_EQ(md.guest_rip, 0xB000u);
    EXPECT_EQ(md.pins.host_reg[0], 24u);
    EXPECT_FALSE(jit_fault_metadata_for_pc(base + 0xD000, md))
        << "free window space between the parts belongs to no block";
    // A's cold part, in the next granule.
    ASSERT_TRUE(jit_fault_metadata_for_pc(a + 0x7E40, md));
    ASSERT_TRUE(md.has_guest_rip);
    EXPECT_EQ(md.guest_rip, 0xA007u);
    EXPECT_EQ(md.pins.host_reg[0], 23u);
    ASSERT_TRUE(jit_fault_metadata_for_pc(a + 0x7E00, md));
    EXPECT_FALSE(md.has_guest_rip) << "a cold stub owns pins, not a RIP";
    EXPECT_EQ(md.pins.host_reg[0], 23u);
    EXPECT_FALSE(jit_fault_metadata_for_pc(a + 0x8000, md))
        << "the first byte past A's cold part belongs to no block";
    // B's cold part sits just below A's.
    ASSERT_TRUE(jit_fault_metadata_for_pc(b + 0x7CFC, md));
    EXPECT_FALSE(md.has_guest_rip);
    EXPECT_EQ(md.pins.host_reg[0], 24u);

    // Retiring the window retires both parts of both blocks.
    retire_jit_rip_range(base + 0xC000, base + 0x14000);
    EXPECT_FALSE(jit_fault_metadata_for_pc(a + 0x10, md));
    EXPECT_FALSE(jit_fault_metadata_for_pc(a + 0x7E40, md));
    EXPECT_FALSE(jit_fault_metadata_for_pc(b + 0x7CFC, md));
    munmap(raw, kReserve);
}

// The M5 flag-recovery descriptor: publication validation and round-trip.
//
// `JitFlagRecovery` is what lets a block skip the three CpuState stores that
// publish a lazy flag record — the handler rebuilds the record from the
// interrupted register file instead. Nothing produces a descriptor yet (the
// emitter still publishes every record), so these tests are what keep the
// mechanism honest until it has a caller: publication must reject anything
// the signal handler could not safely act on, because the handler's only
// alternatives are a wrong reconstruction or an out-of-bounds read of the
// interrupted register file.
TEST(JitFlagRecoveryTest, PublicationRejectsDescriptorsTheHandlerCannotUse) {
    alignas(4) static u32 native[8]{};
    const uintptr_t base = reinterpret_cast<uintptr_t>(native);
    const u32 size = static_cast<u32>(sizeof(native));

    const auto publish_with = [&](JitFlagRecovery fr) {
        std::array<JitRipSite, 1> sites{{JitRipSite{0, size, 0x1000, fr}}};
        return publish_jit_rip_block(base, size, sites.data(), 1, kNoJitPins);
    };

    JitFlagRecovery good{};
    good.flag_op = 1;          // FlagOp::Add8 — any nonzero, valid op
    good.src_reg = 23;
    good.dst_reg = JitFlagRecovery::kImmediate;
    good.dst_imm = 0x1234;
    good.width = 32;

    // X29 and above are not in `__ss.__x[]`; the handler indexes that array.
    JitFlagRecovery bad_src = good;
    bad_src.src_reg = 29;
    EXPECT_FALSE(publish_with(bad_src)) << "src register out of range";

    JitFlagRecovery bad_dst = good;
    bad_dst.dst_reg = 200;
    EXPECT_FALSE(publish_with(bad_dst)) << "dst register out of range";

    // The descriptor carries FlagOp as a raw byte; the handler casts it back.
    JitFlagRecovery bad_op = good;
    bad_op.flag_op = 0xFE;
    EXPECT_FALSE(publish_with(bad_op)) << "flag op byte out of range";

    // Only 32 and 64 are meaningful widths; anything else would silently
    // truncate the rebuilt record to the wrong operand size.
    JitFlagRecovery bad_width = good;
    bad_width.width = 16;
    EXPECT_FALSE(publish_with(bad_width)) << "width neither 32 nor 64";

    ASSERT_TRUE(publish_with(good));

    JitFaultMetadata recovered{};
    ASSERT_TRUE(jit_fault_metadata_for_pc(base + 4, recovered));
    ASSERT_TRUE(recovered.has_guest_rip);
    ASSERT_TRUE(recovered.flags.active());
    EXPECT_EQ(recovered.flags.flag_op, 1u);
    EXPECT_EQ(recovered.flags.src_reg, 23u);
    EXPECT_EQ(recovered.flags.dst_reg, JitFlagRecovery::kImmediate);
    EXPECT_EQ(recovered.flags.dst_imm, 0x1234u);
    EXPECT_EQ(recovered.flags.width, 32u);

    retire_jit_rip_range(base, base + size);
}

// An ordinary site carries no descriptor, and `active()` must be false for it.
// This is the property the fault handler branches on: every block compiled
// today publishes its flag records normally, so a descriptor that read as
// active by default would make the handler overwrite good flag state with a
// reconstruction of nothing.
TEST(JitFlagRecoveryTest, SitesWithoutADescriptorAreInactive) {
    alignas(4) static u32 native[8]{};
    const uintptr_t base = reinterpret_cast<uintptr_t>(native);
    const u32 size = static_cast<u32>(sizeof(native));
    std::array<JitRipSite, 1> sites{{JitRipSite{0, size, 0x2000}}};
    ASSERT_TRUE(publish_jit_rip_block(base, size, sites.data(), 1,
                                      kNoJitPins));

    JitFaultMetadata recovered{};
    ASSERT_TRUE(jit_fault_metadata_for_pc(base + 4, recovered));
    EXPECT_FALSE(recovered.flags.active());
    EXPECT_EQ(recovered.flags.flag_op, 0u);

    retire_jit_rip_range(base, base + size);
}

}  // namespace
