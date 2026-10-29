// SPDX-License-Identifier: LGPL-2.1-only

/// @file image_placement.cpp
/// @brief Choosing an image base that clears the reserved holes.

#include "image_placement.hpp"

#include <vitreo/common.hpp>

namespace vitreo::pe {

namespace {

/// Windows allocation granularity. Every image base is a multiple of it, and
/// VirtualQuery/VirtualAlloc semantics assume it, so a rebased image must stay
/// aligned even though the holes are only page-aligned.
constexpr GuestAddr kAllocGranularity = 0x10000;

/// Highest address a 32-bit guest image may occupy. Windows gives a 32-bit
/// process a 2 GiB user space by default; the top 64 KiB is the no-access
/// region. Placing an image above this would hand the guest a pointer it
/// cannot form.
constexpr GuestAddr kWin32UserSpaceEnd = 0x7FFF0000;

/// Above 4 GiB the host occupancy scan records nothing, so the only bound
/// that matters for a Win64 image is arithmetic overflow.
constexpr GuestAddr kWin64UserSpaceEnd = 0x0000'7FFF'FFFF'0000ull;

constexpr GuestAddr align_up(GuestAddr v, GuestAddr a) noexcept {
    return (v + (a - 1)) & ~(a - 1);
}

} // namespace

ImagePlacement select_image_base(const GuestMemory& memory,
                                 const ParsedPe& pe,
                                 GuestAddr preferred) noexcept {
    ImagePlacement out{};

    if (pe.size_of_image == 0) {
        out.failure = "image has a zero SizeOfImage";
        return out;
    }
    const usize size = static_cast<usize>(
        align_up(pe.size_of_image, GuestAddr{0x1000}));

    const GuestAddr requested = (preferred != 0) ? preferred : pe.image_base;
    if (!memory.overlaps_reserved_hole(requested, size)) {
        out.base = requested;
        return out;
    }

    // The preferred base is inside memory the host owns. Only a relocatable
    // image can be moved; without a .reloc table its absolute addresses are
    // baked in, and loading it anywhere else would be silent corruption.
    if (!pe.has_relocations()) {
        out.failure =
            "preferred image base is occupied by the host and the image has "
            "no relocation table";
        return out;
    }

    const GuestAddr limit = pe.is_64bit ? kWin64UserSpaceEnd
                                        : kWin32UserSpaceEnd;

    // next_address_clear_of_holes() answers at page granularity; re-aligning
    // to the allocation granularity can push the candidate back into the next
    // hole, so iterate until the two agree. Holes are coalesced and the
    // candidate only ever moves forward, so this terminates.
    GuestAddr candidate = align_up(requested, kAllocGranularity);
    for (;;) {
        if (candidate > limit || size > limit - candidate) {
            out.failure =
                "no address clear of host-owned memory fits the image";
            return out;
        }
        const GuestAddr next =
            memory.next_address_clear_of_holes(candidate, size);
        if (next == candidate) break;
        const GuestAddr aligned = align_up(next, kAllocGranularity);
        if (aligned < next) {   // wrapped
            out.failure =
                "no address clear of host-owned memory fits the image";
            return out;
        }
        candidate = aligned;
    }

    out.base = candidate;
    out.relocated = (candidate != requested);
    return out;
}

} // namespace vitreo::pe
