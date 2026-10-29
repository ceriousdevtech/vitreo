// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file guest_image_registry.hpp
/// @brief Which guest address ranges hold a loaded PE image whose code pages
///        are unchanged since the loader finished with them.
///
/// The persistent JIT translation store (core/cpu/jit/translation_store.hpp)
/// only stores and adopts translations of image code, keyed by the image's
/// identity. This registry is its view of "image code": a range is
/// registered when a loader has relocated and bound an image, and a page
/// leaves it (becomes "modified") on the first executable write or
/// protection change after that, or when the range is unmapped. Modified is
/// sticky for the life of the mapping, so a hot-patched or self-modifying
/// image page is never served from, or offered to, the store again.
///
/// Registration is skipped entirely while the store is off
/// (VITREO_TRANSLATION_STORE unset or "off"), so the hooks GuestMemory calls
/// on writes and protection changes cost one relaxed load.
#pragma once

#include <vitreo/types.hpp>

#include <atomic>
#include <memory>
#include <shared_mutex>
#include <span>
#include <vector>

namespace vitreo {

struct GuestImageIdentity {
    GuestAddr base{0};
    GuestAddr end{0};
    /// Hash of the image file's headers, size and preferred base: names one
    /// version of one module. A namespace for statistics and eviction; the
    /// store's correctness rests on its per-block byte comparison.
    u64 id{0};
    GuestAddr preferred_base{0};
};

class GuestImageRegistry {
public:
    /// Whether the translation store can use the registry in this process
    /// (VITREO_TRANSLATION_STORE set to anything but "off"/"0"). Read once.
    [[nodiscard]] static bool enabled() noexcept;
    /// TEST-ONLY: force enabled() on (1) or off (0); -1 = environment.
    static void set_enabled_for_test(int value) noexcept;

    /// Identity of an image file: headers (the first @p size_of_headers
    /// bytes), file length and preferred base.
    [[nodiscard]] static u64 identity_from_file(std::span<const u8> file,
                                                u32 size_of_headers,
                                                GuestAddr preferred_base) noexcept;

    /// Register [base, end) as a loaded, unmodified image. A previous entry
    /// overlapping the range is replaced. No-op unless enabled().
    void register_image(const GuestImageIdentity& image);

    /// Mark the pages of [addr, addr + size) modified in every image they
    /// overlap. (0, 0) marks every registered image modified (the lost
    /// executable-write history sentinel).
    void note_modified(GuestAddr addr, usize size) noexcept;

    /// Forget every image overlapping [addr, addr + size).
    void unregister_overlapping(GuestAddr addr, usize size);

    /// The image wholly containing [addr, addr + size) when none of the
    /// range's pages is modified.
    [[nodiscard]] bool lookup_clean(GuestAddr addr, usize size,
                                    GuestImageIdentity& out) const noexcept;

    [[nodiscard]] bool empty() const noexcept {
        return m_count.load(std::memory_order_acquire) == 0;
    }

    static constexpr u32 kPageShift = 12;

private:
    struct Entry {
        GuestImageIdentity image;
        std::unique_ptr<std::atomic<u64>[]> modified;  ///< one bit per page
        usize words{0};
        std::atomic<bool> all_modified{false};
    };
    mutable std::shared_mutex m_mutex;
    std::vector<std::shared_ptr<Entry>> m_images;  ///< sorted by base
    std::atomic<usize> m_count{0};
    std::atomic<GuestAddr> m_lo{~GuestAddr{0}};
    std::atomic<GuestAddr> m_hi{0};
    void refresh_bounds_locked() noexcept;
};

}  // namespace vitreo
