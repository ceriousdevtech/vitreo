// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include "guest_image_registry.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace vitreo {

namespace {
std::atomic<int> g_enabled_override{-1};
}  // namespace

void GuestImageRegistry::set_enabled_for_test(int value) noexcept {
    g_enabled_override.store(value, std::memory_order_relaxed);
}

bool GuestImageRegistry::enabled() noexcept {
    if (const int forced = g_enabled_override.load(std::memory_order_relaxed);
        forced >= 0) {
        return forced != 0;
    }
    static const bool on = [] {
        const char* v = std::getenv("VITREO_TRANSLATION_STORE");
        return v == nullptr || (std::strcmp(v, "off") != 0 && std::strcmp(v, "0") != 0);
    }();
    return on;
}

u64 GuestImageRegistry::identity_from_file(std::span<const u8> file,
                                           u32 size_of_headers,
                                           GuestAddr preferred_base) noexcept {
    constexpr u64 kOffset = 14695981039346656037ull;
    constexpr u64 kPrime = 1099511628211ull;
    u64 h = kOffset;
    const auto mix = [&](const void* p, usize n) {
        const auto* b = static_cast<const u8*>(p);
        for (usize i = 0; i < n; ++i) {
            h ^= b[i];
            h *= kPrime;
        }
    };
    const usize headers = std::min<usize>(file.size(), size_of_headers);
    mix(file.data(), headers);
    const u64 size = file.size();
    mix(&size, sizeof size);
    mix(&preferred_base, sizeof preferred_base);
    return h;
}

void GuestImageRegistry::refresh_bounds_locked() noexcept {
    GuestAddr lo = ~GuestAddr{0};
    GuestAddr hi = 0;
    for (const auto& e : m_images) {
        lo = std::min(lo, e->image.base);
        hi = std::max(hi, e->image.end);
    }
    m_lo.store(lo, std::memory_order_relaxed);
    m_hi.store(hi, std::memory_order_relaxed);
    m_count.store(m_images.size(), std::memory_order_release);
}

void GuestImageRegistry::register_image(const GuestImageIdentity& image) {
    if (!enabled() || image.end <= image.base) return;
    auto entry = std::make_shared<Entry>();
    entry->image = image;
    const usize pages =
        static_cast<usize>(((image.end - image.base) + (GuestAddr{1} << kPageShift) - 1) >>
                           kPageShift);
    entry->words = (pages + 63) / 64;
    entry->modified = std::make_unique<std::atomic<u64>[]>(entry->words);
    for (usize i = 0; i < entry->words; ++i) {
        entry->modified[i].store(0, std::memory_order_relaxed);
    }
    std::unique_lock lock(m_mutex);
    std::erase_if(m_images, [&](const std::shared_ptr<Entry>& e) {
        return e->image.base < image.end && image.base < e->image.end;
    });
    const auto at = std::lower_bound(
        m_images.begin(), m_images.end(), image.base,
        [](const std::shared_ptr<Entry>& e, GuestAddr base) { return e->image.base < base; });
    m_images.insert(at, std::move(entry));
    refresh_bounds_locked();
}

void GuestImageRegistry::note_modified(GuestAddr addr, usize size) noexcept {
    if (m_count.load(std::memory_order_acquire) == 0) return;
    const bool everything = addr == 0 && size == 0;
    if (!everything) {
        if (size == 0) return;
        const GuestAddr end = addr + size;
        if (end <= m_lo.load(std::memory_order_relaxed) ||
            addr >= m_hi.load(std::memory_order_relaxed)) {
            return;
        }
    }
    std::shared_lock lock(m_mutex);
    for (const auto& e : m_images) {
        if (everything) {
            e->all_modified.store(true, std::memory_order_release);
            continue;
        }
        const GuestAddr lo = std::max(addr, e->image.base);
        const GuestAddr hi = std::min(addr + size, e->image.end);
        if (lo >= hi) continue;
        const usize first = static_cast<usize>((lo - e->image.base) >> kPageShift);
        const usize last = static_cast<usize>((hi - 1 - e->image.base) >> kPageShift);
        for (usize p = first; p <= last; ++p) {
            e->modified[p / 64].fetch_or(u64{1} << (p % 64), std::memory_order_release);
        }
    }
}

void GuestImageRegistry::unregister_overlapping(GuestAddr addr, usize size) {
    if (m_count.load(std::memory_order_acquire) == 0 || size == 0) return;
    std::unique_lock lock(m_mutex);
    const usize before = m_images.size();
    std::erase_if(m_images, [&](const std::shared_ptr<Entry>& e) {
        return e->image.base < addr + size && addr < e->image.end;
    });
    if (m_images.size() != before) refresh_bounds_locked();
}

bool GuestImageRegistry::lookup_clean(GuestAddr addr, usize size,
                                      GuestImageIdentity& out) const noexcept {
    if (m_count.load(std::memory_order_acquire) == 0 || size == 0) return false;
    std::shared_lock lock(m_mutex);
    const auto it = std::upper_bound(
        m_images.begin(), m_images.end(), addr,
        [](GuestAddr a, const std::shared_ptr<Entry>& e) { return a < e->image.base; });
    if (it == m_images.begin()) return false;
    const Entry& e = **(it - 1);
    if (addr < e.image.base || addr + size > e.image.end || addr + size < addr) return false;
    if (e.all_modified.load(std::memory_order_acquire)) return false;
    const usize first = static_cast<usize>((addr - e.image.base) >> kPageShift);
    const usize last = static_cast<usize>((addr + size - 1 - e.image.base) >> kPageShift);
    for (usize p = first; p <= last; ++p) {
        if ((e.modified[p / 64].load(std::memory_order_acquire) >> (p % 64)) & 1u) {
            return false;
        }
    }
    out = e.image;
    return true;
}

}  // namespace vitreo
