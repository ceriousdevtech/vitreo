// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_buffer_ring.cpp
/// @brief WS6: guest-mapped CPU-visible heaps that are the storage of
///        mappable D3D11 buffers (see d3d11_buffer_ring.hpp).

#include "winapi/graphics/d3d11/d3d11_buffer_ring.hpp"

#include "core/memory/guest_memory.hpp"
#include "graphics/contracts/backend_device.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vitreo::winapi {

namespace {

/// -1: the environment decides; 0 / 1: forced (d3d11_ws6_override_for_test).
std::atomic<int> g_ws6_override{-1};

[[nodiscard]] usize round_up(usize value, usize align) noexcept {
    return (value + align - 1u) / align * align;
}

}  // namespace

bool d3d11_ws6_parse(const char* value) noexcept {
    return value == nullptr || std::strcmp(value, "0") != 0;
}

bool d3d11_ws6_enabled() noexcept {
    const int forced = g_ws6_override.load(std::memory_order_relaxed);
    if (forced >= 0) return forced != 0;
    static const bool enabled = d3d11_ws6_parse(std::getenv("VITREO_WS6"));
    return enabled;
}

void d3d11_ws6_override_for_test(std::optional<bool> enabled) noexcept {
    g_ws6_override.store(enabled ? (*enabled ? 1 : 0) : -1,
                         std::memory_order_relaxed);
}

D3D11BufferRing::D3D11BufferRing(GuestMemory& memory,
                                 graphics::GraphicsBackendDevice& backend,
                                 bool is_64bit)
    : m_memory(memory),
      m_backend(backend),
      m_is_64bit(is_64bit),
      m_small_budget(is_64bit ? kSmallBudget64 : kSmallBudget32),
      m_large_budget(is_64bit ? kLargeBudget64 : kLargeBudget32) {}

D3D11BufferRing::~D3D11BufferRing() {
    std::lock_guard<std::mutex> lock(m_mu);
    for (auto& slab : m_slabs) unmap_heap(slab->mapping);
    for (auto& large : m_large) unmap_heap(large->mapping);
}

u32 D3D11BufferRing::class_for(usize byte_width) noexcept {
    u32 cls = 0;
    while (class_bytes(cls) < byte_width) ++cls;
    return cls;
}

bool D3D11BufferRing::map_heap(usize bytes, Mapped& out) {
    graphics::BackendHeapDesc desc{};
    desc.size = bytes;
    desc.cpu_visible = true;
    desc.buffers_only = true;
    auto heap = m_backend.create_heap(desc);
    if (!heap || !heap->valid() || heap->buffer() == nullptr ||
        heap->host_pointer() == nullptr) {
        // A backend that never made one cannot lend windows at all; one
        // that did is out of memory now.
        if (m_slabs.empty() && m_large.empty() && m_created == 0) {
            m_supported = false;
        }
        return false;
    }
    void* const host = heap->host_pointer();
    GuestAddr base = 0;
    // As d3d12_resource.cpp: the hint moves past a range another mapping
    // took between the gap search and the map.
    for (int attempt = 0; attempt < 8 && base == 0; ++attempt) {
        GuestAddr candidate = m_memory.find_free_gap(m_hint, bytes,
                                                     kLargeGranule);
        if (candidate == 0 && m_hint != kGuestHint) {
            m_hint = kGuestHint;
            candidate = m_memory.find_free_gap(m_hint, bytes, kLargeGranule);
        }
        if (candidate == 0) break;
        if (m_memory
                .map_region_backed(candidate, bytes, host,
                                   MemoryProtection::ReadWrite)
                .is_ok()) {
            base = candidate;
        }
        m_hint = candidate + bytes;
    }
    if (base == 0) {
        std::fprintf(stderr,
                     "[d3d11] buffer ring: no guest range for %zu bytes\n",
                     bytes);
        return false;
    }
    out.heap = std::move(heap);
    out.guest = base;
    out.host = static_cast<u8*>(host);
    out.bytes = bytes;
    ++m_created;
    return true;
}

void D3D11BufferRing::unmap_heap(Mapped& mapping) noexcept {
    if (mapping.guest != 0) {
        (void)m_memory.unmap_region(mapping.guest, mapping.bytes);
        mapping.guest = 0;
    }
    // The heap's storage outlives every command buffer still using it
    // (MetalBackendHeap's deallocator), and window buffers retain it.
    mapping.heap.reset();
    mapping.host = nullptr;
}

bool D3D11BufferRing::complete(u64 gen, u64 ticket) {
    if (gen != 0) {
        return gen <= m_waited_through || m_backend.generation_complete(gen);
    }
    return ticket <= m_idle_ticket;
}

bool D3D11BufferRing::wait_for(u64 gen, u64 ticket) {
    ++m_waits;
    if (gen != 0) {
        if (!m_backend.wait_generation(gen)) return false;
        m_waited_through = std::max(m_waited_through, gen);
        return true;
    }
    (void)ticket;
    // Untracked: only a full wait covers it.
    const u64 issued = m_ticket;
    if (!m_backend.wait_idle()) return false;
    m_idle_ticket = std::max(m_idle_ticket, issued);
    return true;
}

void D3D11BufferRing::stamp(u64& gen, u64& ticket) {
    const u64 recording = m_backend.recording_generation();
    if (recording != 0) {
        gen = recording + kRetireMargin;
        ticket = 0;
    } else {
        gen = 0;
        ticket = ++m_ticket;
    }
}

bool D3D11BufferRing::carve_slab(u32 cls) {
    Mapped mapping;
    if (!map_heap(kSlabBytes, mapping)) return false;
    auto slab = std::make_unique<Slab>();
    slab->mapping = std::move(mapping);
    slab->cls = cls;
    slab->last_gen = m_backend.recording_generation();
    auto& free = m_classes[cls].free;
    const usize step = class_bytes(cls);
    // Highest offset first: back() hands out the slab from its start.
    for (usize offset = kSlabBytes; offset >= step; offset -= step) {
        free.push_back({slab.get(), offset - step});
    }
    m_small_bytes += kSlabBytes;
    m_slabs.push_back(std::move(slab));
    return true;
}

void D3D11BufferRing::collect() {
    for (auto& state : m_classes) {
        while (!state.retired.empty() &&
               complete(state.retired.front().gen,
                        state.retired.front().ticket)) {
            const Retired done = state.retired.front();
            state.retired.pop_front();
            --done.slab->outstanding;
            state.free.push_back({done.slab, done.offset});
        }
    }
}

bool D3D11BufferRing::recarve_free_slab(u32 cls) {
    collect();
    for (auto& slab : m_slabs) {
        if (slab->outstanding != 0 || slab->cls == cls) continue;
        auto& old_free = m_classes[slab->cls].free;
        Slab* const target = slab.get();
        old_free.erase(std::remove_if(old_free.begin(), old_free.end(),
                                      [target](const Free& entry) {
                                          return entry.slab == target;
                                      }),
                       old_free.end());
        slab->cls = cls;
        auto& free = m_classes[cls].free;
        const usize step = class_bytes(cls);
        for (usize offset = kSlabBytes; offset >= step; offset -= step) {
            free.push_back({target, offset - step});
        }
        return true;
    }
    return false;
}

void D3D11BufferRing::free_slab(Slab* slab) {
    auto& free = m_classes[slab->cls].free;
    free.erase(std::remove_if(free.begin(), free.end(),
                              [slab](const Free& entry) {
                                  return entry.slab == slab;
                              }),
               free.end());
    unmap_heap(slab->mapping);
    m_small_bytes -= kSlabBytes;
    m_slabs.erase(std::find_if(m_slabs.begin(), m_slabs.end(),
                               [slab](const std::unique_ptr<Slab>& entry) {
                                   return entry.get() == slab;
                               }));
}

void D3D11BufferRing::sweep_stale() {
    collect();
    std::vector<Slab*> stale;
    for (auto& slab : m_slabs) {
        if (slab->outstanding == 0 && slab->last_gen != 0 &&
            complete(slab->last_gen + m_stale_generations, 0)) {
            stale.push_back(slab.get());
        }
    }
    for (Slab* slab : stale) free_slab(slab);
    for (auto it = m_large.begin(); it != m_large.end();) {
        Large& large = **it;
        if (!large.leased && large.gen != 0 &&
            complete(large.gen + m_stale_generations, 0)) {
            unmap_heap(large.mapping);
            m_large_bytes -= large.mapping.bytes;
            it = m_large.erase(it);
        } else {
            ++it;
        }
    }
}

bool D3D11BufferRing::acquire_small(u32 cls, Slice& out, bool for_create) {
    auto& state = m_classes[cls];
    for (;;) {
        if (!state.retired.empty() &&
            complete(state.retired.front().gen,
                     state.retired.front().ticket)) {
            const Retired done = state.retired.front();
            state.retired.pop_front();
            out = {done.slab, nullptr, done.offset};
            m_live_small += class_bytes(cls);
            ++m_reused;
            return true;
        }
        if (!state.free.empty()) {
            const Free entry = state.free.back();
            state.free.pop_back();
            ++entry.slab->outstanding;
            out = {entry.slab, nullptr, entry.offset};
            m_live_small += class_bytes(cls);
            return true;
        }
        // The budget bounds the bytes no live buffer leases (retiring and
        // free); a new buffer's lease is bounded only by the guest window.
        if ((for_create ||
             m_small_bytes - m_live_small + kSlabBytes <= m_small_budget) &&
            carve_slab(cls)) {
            continue;
        }
        if (!m_supported) return false;
        if (recarve_free_slab(cls)) continue;
        // The bound (or the guest window) is reached: wait for a slice to
        // retire. This class's oldest first; otherwise the newest retiring
        // slice of any class, after which every slab whose slices all
        // retired is re-carved.
        if (!state.retired.empty()) {
            if (!wait_for(state.retired.front().gen,
                          state.retired.front().ticket)) {
                return false;
            }
            continue;
        }
        const Retired* newest = nullptr;
        for (const auto& other : m_classes) {
            if (!other.retired.empty() &&
                (newest == nullptr || other.retired.back().gen > newest->gen ||
                 other.retired.back().gen == 0)) {
                newest = &other.retired.back();
            }
        }
        // Nothing retiring could free a slice: every one is leased.
        if (newest == nullptr) return false;
        if (!wait_for(newest->gen, newest->ticket)) return false;
        if (!recarve_free_slab(cls)) return false;
    }
}

bool D3D11BufferRing::acquire_large(usize bytes, Slice& out, bool for_create) {
    const usize need = round_up(bytes, kLargeGranule);
    for (;;) {
        Large* best = nullptr;
        bool any_idle = false;
        for (auto& large : m_large) {
            if (large->leased) continue;
            any_idle = true;
            if (large->mapping.bytes < need ||
                large->mapping.bytes > 2u * need ||
                !complete(large->gen, large->ticket)) {
                continue;
            }
            if (best == nullptr || large->mapping.bytes < best->mapping.bytes) {
                best = large.get();
            }
        }
        if (best != nullptr) {
            best->leased = true;
            m_live_large += best->mapping.bytes;
            out = {nullptr, best, 0};
            ++m_reused;
            return true;
        }
        // As acquire_small: the budget bounds the heaps no live buffer
        // leases; a request with none of those is admitted whatever its size.
        if (for_create || !any_idle ||
            m_large_bytes - m_live_large + need <= m_large_budget) {
            Mapped mapping;
            if (map_heap(need, mapping)) {
                auto large = std::make_unique<Large>();
                large->mapping = std::move(mapping);
                large->leased = true;
                m_large_bytes += need;
                m_live_large += need;
                out = {nullptr, large.get(), 0};
                m_large.push_back(std::move(large));
                return true;
            }
            if (!m_supported) return false;
        }
        // Over the bound, or out of guest range: retired heaps that cannot
        // serve this size free their bytes.
        bool freed = false;
        for (auto it = m_large.begin(); it != m_large.end();) {
            Large& large = **it;
            if (!large.leased && complete(large.gen, large.ticket)) {
                unmap_heap(large.mapping);
                m_large_bytes -= large.mapping.bytes;
                it = m_large.erase(it);
                freed = true;
            } else {
                ++it;
            }
        }
        if (freed) continue;
        Large* oldest = nullptr;
        for (auto& large : m_large) {
            if (large->leased) continue;
            if (oldest == nullptr || large->gen < oldest->gen) {
                oldest = large.get();
            }
        }
        if (oldest == nullptr) return false;  // everything is leased
        if (!wait_for(oldest->gen, oldest->ticket)) return false;
    }
}

bool D3D11BufferRing::acquire(usize byte_width, Slice& out, bool for_create) {
    return byte_width <= kMaxSmallSlice
               ? acquire_small(class_for(byte_width), out, for_create)
               : acquire_large(byte_width, out, for_create);
}

void D3D11BufferRing::release_slice(const Slice& slice, u64 gen, u64 ticket) {
    if (slice.slab != nullptr) {
        m_live_small -= class_bytes(slice.slab->cls);
        m_classes[slice.slab->cls].retired.push_back(
            {slice.slab, slice.offset, gen, ticket});
        slice.slab->last_gen = std::max(slice.slab->last_gen, gen);
    } else if (slice.large != nullptr) {
        m_live_large -= slice.large->mapping.bytes;
        slice.large->leased = false;
        slice.large->gen = gen;
        slice.large->ticket = ticket;
    }
}

void D3D11BufferRing::install(D3D11RingLease& lease, const Slice& slice) {
    const Mapped& mapping =
        slice.slab != nullptr ? slice.slab->mapping : slice.large->mapping;
    lease.m_slice = slice;
    lease.m_has_slice = true;
    lease.m_window = {mapping.heap->buffer(), slice.offset};
    lease.m_guest = mapping.guest + slice.offset;
    lease.m_host = mapping.host + slice.offset;
}

D3D11BufferRing::LeaseStatus D3D11BufferRing::lease(
    usize byte_width, std::shared_ptr<D3D11RingLease>& out) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (!m_supported) return LeaseStatus::Unsupported;
    if (byte_width == 0) return LeaseStatus::OutOfMemory;
    sweep_stale();
    auto lease = std::make_shared<D3D11RingLease>(shared_from_this(),
                                                  byte_width);
    Slice slice{};
    if (!acquire(byte_width, slice, /*for_create=*/true)) {
        lease->m_ring.reset();  // nothing to retire
        return m_supported ? LeaseStatus::OutOfMemory
                           : LeaseStatus::Unsupported;
    }
    install(*lease, slice);
    out = std::move(lease);
    return LeaseStatus::Ok;
}

bool D3D11BufferRing::prepare_slice(D3D11RingLease& lease, Slice& out) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (!lease.m_has_slice) return false;
    auto& inflight = lease.m_inflight;
    while (!inflight.empty() && complete(inflight.front(), 0)) {
        inflight.pop_front();
    }
    if (inflight.size() >= kMaxSlicesPerBuffer) {
        if (!wait_for(inflight.front(), 0)) return false;
        inflight.pop_front();
    }
    return acquire(lease.m_byte_width, out, /*for_create=*/false);
}

graphics::BackendBufferWindow D3D11BufferRing::window_of(
    const Slice& slice) const noexcept {
    const Mapped& mapping =
        slice.slab != nullptr ? slice.slab->mapping : slice.large->mapping;
    return {mapping.heap->buffer(), slice.offset};
}

void D3D11BufferRing::commit_slice(D3D11RingLease& lease, const Slice& fresh) {
    std::lock_guard<std::mutex> lock(m_mu);
    u64 gen = 0;
    u64 ticket = 0;
    stamp(gen, ticket);
    release_slice(lease.m_slice, gen, ticket);
    if (gen != 0) lease.m_inflight.push_back(gen);
    install(lease, fresh);
    if ((++m_ops & 1023u) == 0) sweep_stale();
}

void D3D11BufferRing::abandon_slice(const Slice& fresh) noexcept {
    std::lock_guard<std::mutex> lock(m_mu);
    if (fresh.slab != nullptr) {
        // acquire_small counted it outstanding and live either way.
        m_live_small -= class_bytes(fresh.slab->cls);
        --fresh.slab->outstanding;
        m_classes[fresh.slab->cls].free.push_back({fresh.slab, fresh.offset});
    } else if (fresh.large != nullptr) {
        // It was complete when acquired; its gen and ticket still say so.
        m_live_large -= fresh.large->mapping.bytes;
        fresh.large->leased = false;
    }
}

bool D3D11BufferRing::wait_recorded() {
    std::lock_guard<std::mutex> lock(m_mu);
    // The recording holds every use made so far (an untracked one: only a
    // full wait covers it).
    const u64 recording = m_backend.recording_generation();
    return wait_for(recording, recording != 0 ? 0 : ++m_ticket);
}

void D3D11BufferRing::note_in_place() noexcept {
    std::lock_guard<std::mutex> lock(m_mu);
    ++m_in_place;
}

void D3D11BufferRing::retire(D3D11RingLease& lease) noexcept {
    std::lock_guard<std::mutex> lock(m_mu);
    if (!lease.m_has_slice) return;
    u64 gen = 0;
    u64 ticket = 0;
    stamp(gen, ticket);
    release_slice(lease.m_slice, gen, ticket);
    lease.m_has_slice = false;
}

D3D11BufferRing::Stats D3D11BufferRing::stats() const {
    std::lock_guard<std::mutex> lock(m_mu);
    Stats stats{};
    stats.small_bytes = m_small_bytes;
    stats.large_bytes = m_large_bytes;
    stats.live_small_bytes = m_live_small;
    stats.live_large_bytes = m_live_large;
    stats.in_place = m_in_place;
    stats.slabs = static_cast<u32>(m_slabs.size());
    stats.large_heaps = static_cast<u32>(m_large.size());
    stats.created = m_created;
    stats.reused = m_reused;
    stats.waits = m_waits;
    return stats;
}

D3D11RingLease::~D3D11RingLease() {
    if (m_backend != nullptr) m_backend->set_window_source(nullptr);
    if (m_ring) m_ring->retire(*this);
}

bool D3D11RingLease::attach(graphics::BackendBuffer& backend) noexcept {
    if (!backend.set_window(m_window)) return false;
    backend.set_window_source(this);
    m_backend = &backend;
    return true;
}

bool D3D11RingLease::discard() {
    if (!m_ring) return false;
    D3D11BufferRing::Slice fresh{};
    if (m_ring->prepare_slice(*this, fresh)) {
        // The backend takes the window before the lease moves, so the
        // guest's Map and the GPU never name different slices.
        if (m_backend == nullptr ||
            m_backend->set_window(m_ring->window_of(fresh))) {
            m_ring->commit_slice(*this, fresh);
            return true;
        }
        m_ring->abandon_slice(fresh);
    }
    // No fresh slice: wait for the current one's work and reuse it in place.
    const bool waited = m_backend != nullptr ? m_backend->wait_storage_idle()
                                             : m_ring->wait_recorded();
    if (waited) m_ring->note_in_place();
    return waited;
}

bool D3D11RingLease::replace_window(graphics::BackendBuffer& buffer,
                                    graphics::BackendBufferWindow& out) {
    if (!m_ring) return false;
    D3D11BufferRing::Slice fresh{};
    if (!m_ring->prepare_slice(*this, fresh)) return false;
    const graphics::BackendBufferWindow window = m_ring->window_of(fresh);
    if (!buffer.set_window(window)) {
        m_ring->abandon_slice(fresh);
        return false;
    }
    m_ring->commit_slice(*this, fresh);
    out = window;
    return true;
}

}  // namespace vitreo::winapi
