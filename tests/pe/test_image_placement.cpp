// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_image_placement.cpp
/// @brief Choosing an image base against host-owned address space.
///
/// Under identity mapping (the Intel native backend) the guest shares its
/// address space with the host process, so a PE's preferred ImageBase can be
/// structurally unavailable -- the main thread stack at 0x800000-0x1000000 is
/// the measured example, and no linker flag moves it. Windows already lets a
/// relocatable image load elsewhere, so the loader rebases; an image with no
/// .reloc table must fail honestly instead.
///
/// The decision is pure address arithmetic, so it runs on both backends.

#include <gtest/gtest.h>

#include "core/memory/guest_memory.hpp"
#include "core/pe/mapper/image_placement.hpp"

using namespace vitreo;

namespace {
// The measured low-memory layout on the Intel target.
constexpr GuestAddr kStackHoleLo = 0x00800000;
constexpr GuestAddr kStackHoleHi = 0x01000000;
constexpr GuestAddr kDyldHoleLo  = 0x00001000;
constexpr GuestAddr kDyldHoleHi  = 0x00061000;
} // namespace

namespace {

/// Minimal ParsedPe for placement decisions — only ImageBase, SizeOfImage,
/// bitness and the presence of a .reloc directory are consulted.
pe::ParsedPe make_pe(GuestAddr base, u32 size_of_image, bool relocatable,
                     bool is_64bit = false) {
    pe::ParsedPe pe{};
    pe.is_64bit = is_64bit;
    pe.image_base = base;
    pe.size_of_image = size_of_image;
    pe.num_data_directories = pe::DIR_COUNT;
    if (relocatable) {
        pe.data_directories[pe::DIR_BASERELOC].VirtualAddress = 0x1000;
        pe.data_directories[pe::DIR_BASERELOC].Size = 0x100;
    }
    return pe;
}

/// The measured low-memory map of the Intel target (§5.1). Reproduced exactly
/// so the placement tests answer the question the real machine asks.
void apply_measured_intel_holes(GuestMemory& mem) {
    mem.add_reserved_hole(kDyldHoleLo, kDyldHoleHi);   // dyld
    mem.add_reserved_hole(0x100000, 0x200000);
    mem.add_reserved_hole(kStackHoleLo, kStackHoleHi); // main thread stack
}

} // namespace

TEST(ImagePlacementTest, NoHolesLeavesThePreferredBaseAlone) {
    // The translated path: no holes are ever recorded, so placement must be
    // byte-for-byte the old behavior.
    GuestMemory mem;
    const auto pe = make_pe(0x400000, 0x900000, /*relocatable=*/true);
    const auto p = pe::select_image_base(mem, pe);
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(p.base, GuestAddr{0x400000});
    EXPECT_FALSE(p.relocated);
}

TEST(ImagePlacementTest, ExplicitPreferredBaseOverridesImageBase) {
    GuestMemory mem;
    const auto pe = make_pe(0x400000, 0x10000, /*relocatable=*/true);
    const auto p = pe::select_image_base(mem, pe, 0x10000000);
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(p.base, GuestAddr{0x10000000});
    EXPECT_FALSE(p.relocated);
}

TEST(ImagePlacementTest, ClassicWin32ImageKeepsItsPreferredBase) {
    GuestMemory mem;
    apply_measured_intel_holes(mem);
    // 3 MiB at 0x400000 fits the measured 6 MiB gap. This is the case that
    // makes the immovable main thread stack acceptable at all.
    const auto pe = make_pe(0x400000, 0x300000, /*relocatable=*/true);
    const auto p = pe::select_image_base(mem, pe);
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(p.base, GuestAddr{0x400000});
    EXPECT_FALSE(p.relocated);
}

TEST(ImagePlacementTest, ImageTooBigForTheGapIsRebasedAboveTheStack) {
    GuestMemory mem;
    apply_measured_intel_holes(mem);
    // 9 MiB at 0x400000 runs into the stack, so it must move above it rather
    // than fail — Windows already allows a relocatable image to load elsewhere.
    const auto pe = make_pe(0x400000, 0x900000, /*relocatable=*/true);
    const auto p = pe::select_image_base(mem, pe);
    ASSERT_TRUE(p.ok());
    EXPECT_TRUE(p.relocated);
    EXPECT_EQ(p.base, kStackHoleHi);
    EXPECT_FALSE(mem.overlaps_reserved_hole(p.base, 0x900000));
}

TEST(ImagePlacementTest, NonRelocatableImageFailsHonestly) {
    GuestMemory mem;
    apply_measured_intel_holes(mem);
    // No .reloc table means the absolute addresses are baked in. Placing it
    // anywhere else would be silent corruption, so this must be a hard error.
    const auto pe = make_pe(0x400000, 0x900000, /*relocatable=*/false);
    const auto p = pe::select_image_base(mem, pe);
    EXPECT_FALSE(p.ok());
    EXPECT_EQ(p.base, GuestAddr{0});
    ASSERT_NE(p.failure, nullptr);
}

TEST(ImagePlacementTest, NonRelocatableImageOutsideTheHolesStillLoads) {
    GuestMemory mem;
    apply_measured_intel_holes(mem);
    const auto pe = make_pe(0x400000, 0x300000, /*relocatable=*/false);
    const auto p = pe::select_image_base(mem, pe);
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(p.base, GuestAddr{0x400000});
}

TEST(ImagePlacementTest, RebasedImageStaysAllocationGranular) {
    GuestMemory mem;
    // A hole ending mid-granule: next_address_clear_of_holes answers at page
    // granularity, but every Windows image base is a 64 KiB multiple, so the
    // result must be rounded up AND re-checked against the next hole.
    mem.add_reserved_hole(0x400000, 0x401000);
    const auto pe = make_pe(0x400000, 0x10000, /*relocatable=*/true);
    const auto p = pe::select_image_base(mem, pe);
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(p.base % 0x10000, GuestAddr{0});
    EXPECT_EQ(p.base, GuestAddr{0x410000});
    EXPECT_FALSE(mem.overlaps_reserved_hole(p.base, 0x10000));
}

TEST(ImagePlacementTest, AlignmentPushBackIntoAHoleIsResolved) {
    GuestMemory mem;
    // Rounding 0x401000 up to 0x410000 lands inside the second hole. A single
    // align-then-query pass would return an occupied base.
    mem.add_reserved_hole(0x400000, 0x401000);
    mem.add_reserved_hole(0x405000, 0x420000);
    const auto pe = make_pe(0x400000, 0x10000, /*relocatable=*/true);
    const auto p = pe::select_image_base(mem, pe);
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(p.base, GuestAddr{0x420000});
    EXPECT_FALSE(mem.overlaps_reserved_hole(p.base, 0x10000));
}

TEST(ImagePlacementTest, NoRoomAnywhereFailsInsteadOfWrapping) {
    GuestMemory mem;
    // The entire 32-bit user space is host-owned. There is no honest answer.
    mem.add_reserved_hole(0x1000, 0x80000000);
    const auto pe = make_pe(0x400000, 0x100000, /*relocatable=*/true);
    const auto p = pe::select_image_base(mem, pe);
    EXPECT_FALSE(p.ok());
    EXPECT_EQ(p.base, GuestAddr{0});
}

TEST(ImagePlacementTest, Win32ImageIsNeverPlacedAboveTheUserSpaceLimit) {
    GuestMemory mem;
    // A 32-bit process's user space ends at 2 GiB; a base above it is a
    // pointer the guest cannot form. Failing beats handing one out.
    mem.add_reserved_hole(0x1000, 0x7FF00000);
    const auto pe = make_pe(0x400000, 0x200000, /*relocatable=*/true);
    const auto p = pe::select_image_base(mem, pe);
    EXPECT_FALSE(p.ok());
}

TEST(ImagePlacementTest, Win64ImageIsUnaffectedByLowHoles) {
    GuestMemory mem;
    apply_measured_intel_holes(mem);
    // Win64 image bases are far above the captured range, so the whole
    // mechanism must be inert for them.
    const auto pe = make_pe(0x140000000, 0x200000, /*relocatable=*/true,
                            /*is_64bit=*/true);
    const auto p = pe::select_image_base(mem, pe);
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(p.base, GuestAddr{0x140000000});
    EXPECT_FALSE(p.relocated);
}

TEST(ImagePlacementTest, ZeroSizeOfImageFailsRatherThanPlacingNothing) {
    GuestMemory mem;
    const auto pe = make_pe(0x400000, 0, /*relocatable=*/true);
    const auto p = pe::select_image_base(mem, pe);
    EXPECT_FALSE(p.ok());
}

