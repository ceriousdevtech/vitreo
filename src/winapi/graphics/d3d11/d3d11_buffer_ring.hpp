// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d11_buffer_ring.hpp
/// @brief WS6: the zero-copy storage of mappable D3D11 buffers.
///
/// Every DYNAMIC buffer, every STAGING buffer, and every DEFAULT buffer
/// created with CPU access (Learn D3D11_FEATURE_DATA_D3D11_OPTIONS1,
/// MapOnDefaultBuffers: the mapping is the buffer's own memory) lives in a
/// CPU-visible backend heap (GraphicsBackendDevice::create_heap, host pages
/// the GPU reads and writes directly) that is mapped into the guest once,
/// the way D3D12 maps its UPLOAD / READBACK heaps (d3d12_resource.cpp). Map
/// hands the guest a pointer into those pages, so Unmap neither diffs nor
/// publishes and a Map(READ) needs no readback copy: the slice IS the GPU's
/// storage. A DEFAULT buffer with no CPU access is not mappable and keeps
/// ordinary storage.
///
/// A buffer leases one slice at a time (D3D11RingLease). Learn
/// (D3D11_MAP_WRITE_DISCARD): the previous contents are undefined and the
/// GPU keeps reading them for work already queued, and the memory "might
/// not even be the same for successive calls to Map" (D3D11_USAGE) -- so a
/// DISCARD of a slice recorded work may still use moves the buffer to a
/// fresh slice. A retired slice is handed out again only once the GPU has
/// completed the generation it retired in (generation_complete), never on a
/// CPU guess.
///
/// Slices of up to 64 KiB (constant buffers, most dynamic vertex data) are
/// power-of-two classes carved from 1 MiB slabs; larger buffers get a heap
/// of their own. The budgets bound only the bytes NOT leased by a live
/// buffer -- slices retiring after a DISCARD, and free ones -- since Windows
/// caps neither the number of DYNAMIC buffers nor their renames:
///  - CreateBuffer's lease grows the ring as far as the guest window allows
///    (x86 guests have a 4 GB one); past it the buffer takes the plain path.
///  - A DISCARD past the budget waits for the oldest retiring slice; with
///    nothing retiring it waits for the buffer's own slice and reuses it in
///    place (a wait is a valid outcome of any Map). It fails only when the
///    queue shut down.

#include <vitreo/common.hpp>
#include "graphics/contracts/backend_buffer.hpp"
#include "graphics/contracts/backend_memory.hpp"

#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace vitreo {
class GuestMemory;
}
namespace vitreo::graphics {
class GraphicsBackendDevice;
}

namespace vitreo::winapi {

/// The WS6 kill switch: false when VITREO_WS6 is exactly "0" (read once),
/// or as the test override says.
[[nodiscard]] bool d3d11_ws6_enabled() noexcept;
/// How a VITREO_WS6 value reads (null: unset).
[[nodiscard]] bool d3d11_ws6_parse(const char* value) noexcept;
/// Tests only: force the switch (nullopt: the environment again). Applies
/// to buffers created from now on.
void d3d11_ws6_override_for_test(std::optional<bool> enabled) noexcept;

class D3D11RingLease;

class D3D11BufferRing : public std::enable_shared_from_this<D3D11BufferRing> {
public:
    /// Constant-buffer binding offsets and linear-texture bases.
    static constexpr usize kMinSlice = 256;
    /// Larger buffers get a heap of their own.
    static constexpr usize kMaxSmallSlice = 65536;
    static constexpr usize kSlabBytes = usize{1} << 20;
    /// MetalBackendHeap's CPU-visible granule (and D3D12's placement one).
    static constexpr usize kLargeGranule = 65536;
    static constexpr usize kSmallBudget64 = usize{256} << 20;
    static constexpr usize kSmallBudget32 = usize{128} << 20;
    static constexpr usize kLargeBudget64 = usize{512} << 20;
    static constexpr usize kLargeBudget32 = usize{256} << 20;
    /// Slices one buffer may have retiring at once.
    static constexpr u32 kMaxSlicesPerBuffer = 4096;
    /// A bind can be stamped one generation early across a submit
    /// (metal_buffer.mm kTagMargin): a slice retires one generation later.
    static constexpr u64 kRetireMargin = 1;
    /// An idle slab or large heap is freed after this many generations.
    static constexpr u64 kStaleGenerations = 4096;
    static constexpr GuestAddr kGuestHint = 0x20000000;
    static constexpr u32 kClasses = 9;  // 256 B .. 64 KiB

    enum class LeaseStatus { Ok, Unsupported, OutOfMemory };
    struct Stats {
        usize small_bytes{0};
        usize large_bytes{0};
        u32 slabs{0};
        u32 large_heaps{0};
        /// Bytes of slices live buffers lease (the rest is retiring or free).
        usize live_small_bytes{0};
        usize live_large_bytes{0};
        u64 created{0};
        u64 reused{0};
        u64 waits{0};
        /// DISCARDs that waited for the buffer's own slice and reused it.
        u64 in_place{0};
    };

    D3D11BufferRing(GuestMemory& memory,
                    graphics::GraphicsBackendDevice& backend, bool is_64bit);
    ~D3D11BufferRing();
    D3D11BufferRing(const D3D11BufferRing&) = delete;
    D3D11BufferRing& operator=(const D3D11BufferRing&) = delete;

    /// A lease of @p byte_width bytes holding a fresh slice. The ring must
    /// be owned by a shared_ptr.
    [[nodiscard]] LeaseStatus lease(usize byte_width,
                                    std::shared_ptr<D3D11RingLease>& out);
    /// False once the backend refused a CPU-visible heap.
    [[nodiscard]] bool supported() const noexcept { return m_supported; }
    [[nodiscard]] Stats stats() const;
    /// Bounds on the retiring and free (not live) small / large bytes.
    [[nodiscard]] usize small_budget() const noexcept { return m_small_budget; }
    [[nodiscard]] usize large_budget() const noexcept { return m_large_budget; }
    /// Tests only: other budgets than the bitness's.
    void set_budgets_for_test(usize small, usize large) noexcept {
        std::lock_guard<std::mutex> lock(m_mu);
        m_small_budget = small;
        m_large_budget = large;
    }
    /// Tests only: kStaleGenerations as if it were @p generations.
    void set_stale_generations_for_test(u64 generations) noexcept {
        m_stale_generations = generations;
    }

    /// For D3D11RingLease: its buffer was released.
    void retire(D3D11RingLease& lease) noexcept;

private:
    // For D3D11RingLease (a friend): the two-phase DISCARD rename.
    struct Slab;
    struct Large;
    /// A slice of a slab, or a whole large heap.
    struct Slice {
        Slab* slab{nullptr};
        Large* large{nullptr};
        usize offset{0};
    };

    /// For D3D11RingLease, step 1 of a DISCARD rename: a slice no recorded
    /// work uses, in @p out, not yet the lease's (waiting for a retiring
    /// slice when the budget is reached). False: none to be had without
    /// reusing the lease's own slice, or the queue shut down.
    [[nodiscard]] bool prepare_slice(D3D11RingLease& lease, Slice& out);
    /// The window @p slice lends.
    [[nodiscard]] graphics::BackendBufferWindow window_of(
        const Slice& slice) const noexcept;
    /// Step 2: retire the lease's slice at the recording generation
    /// (+kRetireMargin) and install @p fresh.
    void commit_slice(D3D11RingLease& lease, const Slice& fresh);
    /// Step 2 instead: @p fresh was not installed (the backend refused its
    /// window); it returns unused, and the lease keeps its slice.
    void abandon_slice(const Slice& fresh) noexcept;
    /// No fresh slice and no backend to ask: wait until everything recorded
    /// so far (every use of the lease's slice among it) has completed.
    [[nodiscard]] bool wait_recorded();
    /// A DISCARD reused its lease's slice in place after a wait.
    void note_in_place() noexcept;
    /// A retired slice: reusable once `gen` completed (or, untracked, once
    /// the ring's wait_idle covered `ticket`).
    struct Retired {
        Slab* slab{nullptr};
        usize offset{0};
        u64 gen{0};
        u64 ticket{0};
    };
    struct Free {
        Slab* slab{nullptr};
        usize offset{0};
    };
    struct ClassState {
        std::deque<Retired> retired;
        std::vector<Free> free;
    };
    struct Mapped {
        std::unique_ptr<graphics::BackendHeap> heap;
        GuestAddr guest{0};
        u8* host{nullptr};
        usize bytes{0};
    };
    struct Slab {
        Mapped mapping;
        u32 cls{0};
        /// Slices handed out or retiring (not in the free list).
        u32 outstanding{0};
        /// Newest generation a slice of it retired at, or it was carved.
        u64 last_gen{0};
    };
    struct Large {
        Mapped mapping;
        bool leased{false};
        u64 gen{0};
        u64 ticket{0};
    };
    [[nodiscard]] static u32 class_for(usize byte_width) noexcept;
    [[nodiscard]] static usize class_bytes(u32 cls) noexcept {
        return kMinSlice << cls;
    }
    [[nodiscard]] bool map_heap(usize bytes, Mapped& out);
    void unmap_heap(Mapped& mapping) noexcept;
    [[nodiscard]] bool complete(u64 gen, u64 ticket);
    [[nodiscard]] bool wait_for(u64 gen, u64 ticket);
    void stamp(u64& gen, u64& ticket);
    /// @p for_create: CreateBuffer's lease, which the budget does not bound
    /// (only the guest window does).
    [[nodiscard]] bool acquire_small(u32 cls, Slice& out, bool for_create);
    [[nodiscard]] bool acquire_large(usize bytes, Slice& out, bool for_create);
    [[nodiscard]] bool acquire(usize byte_width, Slice& out, bool for_create);
    void release_slice(const Slice& slice, u64 gen, u64 ticket);
    void collect();
    void sweep_stale();
    [[nodiscard]] bool carve_slab(u32 cls);
    [[nodiscard]] bool recarve_free_slab(u32 cls);
    void free_slab(Slab* slab);
    void install(D3D11RingLease& lease, const Slice& slice);

    GuestMemory& m_memory;
    graphics::GraphicsBackendDevice& m_backend;
    bool m_is_64bit;
    bool m_supported{true};
    usize m_small_budget;
    usize m_large_budget;
    mutable std::mutex m_mu;
    GuestAddr m_hint{kGuestHint};
    std::vector<std::unique_ptr<Slab>> m_slabs;
    std::vector<std::unique_ptr<Large>> m_large;
    std::array<ClassState, kClasses> m_classes{};
    usize m_small_bytes{0};
    usize m_large_bytes{0};
    usize m_live_small{0};
    usize m_live_large{0};
    /// The newest generation a successful wait_generation covered: what it
    /// waited for is complete even when generation_complete still says no
    /// (a retire generation one past the newest submission names an empty
    /// recording, which the wait does not submit).
    u64 m_waited_through{0};
    u64 m_ticket{0};
    u64 m_idle_ticket{0};
    u64 m_created{0};
    u64 m_reused{0};
    u64 m_waits{0};
    u64 m_in_place{0};
    u64 m_ops{0};
    u64 m_stale_generations{kStaleGenerations};

    friend class D3D11RingLease;
};

/// One buffer's hold on the ring: its current slice, and the source of the
/// fresh slices the backend asks for before a GPU write into a busy one.
class D3D11RingLease final : public graphics::BackendBufferWindowSource {
public:
    explicit D3D11RingLease(std::shared_ptr<D3D11BufferRing> ring,
                            usize byte_width) noexcept
        : m_ring(std::move(ring)), m_byte_width(byte_width) {}
    ~D3D11RingLease() override;
    D3D11RingLease(const D3D11RingLease&) = delete;
    D3D11RingLease& operator=(const D3D11RingLease&) = delete;

    [[nodiscard]] GuestAddr guest() const noexcept { return m_guest; }
    [[nodiscard]] u8* host() const noexcept { return m_host; }
    /// What BufferShadow::view follows.
    [[nodiscard]] u8* const* host_slot() const noexcept { return &m_host; }
    [[nodiscard]] usize byte_width() const noexcept { return m_byte_width; }
    [[nodiscard]] const graphics::BackendBufferWindow& window() const noexcept {
        return m_window;
    }
    /// set_window(window()) + set_window_source(this) on @p backend. False:
    /// the backend lends no windows. @p backend must outlive the lease (the
    /// lease detaches itself from it when destroyed).
    [[nodiscard]] bool attach(graphics::BackendBuffer& backend) noexcept;
    /// WRITE_DISCARD of a slice recorded work may use: move the buffer to a
    /// fresh one, or, with none to be had, wait for the current slice's work
    /// and keep it. False only when that wait failed (the queue shut down).
    [[nodiscard]] bool discard();
    bool replace_window(graphics::BackendBuffer& buffer,
                        graphics::BackendBufferWindow& out) override;

private:
    friend class D3D11BufferRing;
    std::shared_ptr<D3D11BufferRing> m_ring;
    usize m_byte_width{0};
    graphics::BackendBuffer* m_backend{nullptr};
    graphics::BackendBufferWindow m_window{};
    GuestAddr m_guest{0};
    u8* m_host{nullptr};
    D3D11BufferRing::Slice m_slice{};
    bool m_has_slice{false};
    /// Retire generations of this lease's slices, oldest first (the
    /// kMaxSlicesPerBuffer bound).
    std::deque<u64> m_inflight;
};

class D3D11Manager;

/// Tests only (WS6): what a D3D11Manager's zero-copy buffers hold. Defined
/// in d3d11_map_buffer.cpp; a friend of D3D11Manager.
struct D3D11Ws6TestPeer {
    /// The guest address of @p buffer's current slice (0: not on the ring).
    [[nodiscard]] static GuestAddr guest(const D3D11Manager& manager,
                                         GuestAddr buffer);
    /// The manager's ring (null before its first DYNAMIC/STAGING buffer).
    [[nodiscard]] static std::shared_ptr<D3D11BufferRing> ring(
        const D3D11Manager& manager);
    /// How often the ring waited for a slice to retire.
    [[nodiscard]] static u64 waits(const D3D11Manager& manager);
};

} // namespace vitreo::winapi
