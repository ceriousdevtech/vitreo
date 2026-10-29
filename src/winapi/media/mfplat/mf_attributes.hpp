// SPDX-License-Identifier: LGPL-2.1-only

/// @file mf_attributes.hpp
/// @brief The Media Foundation platform state and its guest-visible attribute
/// objects: MFStartup/MFShutdown/MFLockPlatform bookkeeping, IMFAttributes
/// (MFCreateAttributes) and IMFMediaType (MFCreateMediaType).
///
/// Contract sources: Microsoft Learn (mfapi.h MFStartup/MFShutdown/MFTEnumEx,
/// mfobjects.h IMFAttributes::* and IMFMediaType::*), the Windows SDK
/// 10.0.26100 headers (vtable order, MF_VERSION, MF_E_* values, attribute
/// GUIDs) and tools/oracle_probe/mf_startup_probe.c recorded on Windows
/// (mf_startup_probe{64,32}.expected) for everything Learn leaves open:
/// which startup versions succeed, what a short buffer writes, the PROPVARIANT
/// types handed back, the empty item a rejected SetItem leaves behind, the
/// IsEqual flag combinations, and reference counts on stored IUnknowns.
///
/// Vitreo has no Media Foundation pipeline (no transforms, no sources, no
/// sinks): the platform starts up like a Windows with the Media Feature Pack
/// installed and no codecs registered, and the object stores below are real
/// because they are plain in-process data structures.

#pragma once

#include <vitreo/common.hpp>
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

/// A GUID as laid out in guest memory (Data1/Data2/Data3 little-endian).
struct MfGuid {
    u32 data1{0};
    u16 data2{0};
    u16 data3{0};
    std::array<u8, 8> data4{};
    [[nodiscard]] bool operator==(const MfGuid& o) const noexcept {
        return data1 == o.data1 && data2 == o.data2 && data3 == o.data3 &&
               data4 == o.data4;
    }
    [[nodiscard]] bool operator!=(const MfGuid& o) const noexcept {
        return !(*this == o);
    }
};
static_assert(sizeof(MfGuid) == 16);

/// Interfaces this component hands out; the value indexes the vtable table.
enum class MfIface : u32 {
    Attributes = 0,  ///< IMFAttributes (33 slots, SDK mfobjects.h)
    MediaType,       ///< IMFMediaType = IMFAttributes + 5 (38 slots)
    Count,
};

/// Media Foundation platform state and attribute-store objects.
class MfPlatManager {
public:
    /// Guest scratch for interface cells and vtables (after cabinet's 2 MiB
    /// at 0x5F800000; relocated by the registry when a guest holds it).
    static constexpr GuestAddr MFPLAT_SCRATCH_BASE = 0x5FC00000u;
    static constexpr u32       MFPLAT_SCRATCH_SIZE = 0x00100000u; // 1 MiB

    /// CoTaskMemAlloc / CoTaskMemFree in guest memory: GetItem, GetAllocated*
    /// and GetItemByIndex hand the caller buffers it frees with
    /// PropVariantClear / CoTaskMemFree, so they must come from ole32's task
    /// allocator rather than from this component's scratch.
    using TaskAlloc = std::function<GuestAddr(u64 size)>;
    using TaskFree  = std::function<void(GuestAddr address)>;
    /// Call slot @p slot of the guest COM object @p punk with @p args
    /// (args[0] is the interface pointer). Returns the guest's HRESULT.
    using GuestVtableCall = std::function<u32(Marshaler& m, GuestAddr punk,
                                              u32 slot, const u64* args,
                                              usize count)>;

    void set_thunk_registry(ThunkRegistry* thunks) noexcept { m_thunks = thunks; }
    void set_scratch_base(GuestAddr base) noexcept {
        m_scratch_base = base;
        m_next_addr = base;
    }
    void set_task_allocator(TaskAlloc alloc, TaskFree free) {
        m_task_alloc = std::move(alloc);
        m_task_free = std::move(free);
    }
    void set_guest_vtable_call(GuestVtableCall call) { m_guest_call = std::move(call); }

    // ---- platform (mfapi.h) ---------------------------------------------
    /// MFStartup(Version, dwFlags). Measured (mf_startup_probe.expected): a
    /// major version (high word) above MF_SDK_VERSION (2) is
    /// MF_E_BAD_STARTUP_VERSION; everything else, any flags, succeeds and
    /// counts one reference.
    [[nodiscard]] u32 startup(u32 version);
    /// MFShutdown(): releases one reference; S_OK even when none is held
    /// (measured), and objects stay usable afterwards (measured).
    [[nodiscard]] u32 shutdown();
    [[nodiscard]] u32 lock_platform();
    [[nodiscard]] u32 unlock_platform();
    [[nodiscard]] u32 startup_count() const noexcept { return m_startups.load(); }
    [[nodiscard]] u32 platform_locks() const noexcept { return m_locks.load(); }

    // ---- factories ------------------------------------------------------
    /// MFCreateAttributes / MFCreateMediaType: write the new interface
    /// pointer to @p out. E_POINTER for a null @p out (defensive: Windows
    /// documents no null case), E_OUTOFMEMORY when the scratch is exhausted.
    [[nodiscard]] u32 create_object(Marshaler& m, MfIface iface, GuestAddr out);

    // ---- vtable dispatch ---------------------------------------------------
    [[nodiscard]] u32 dispatch(MfIface iface, u32 slot, Marshaler& m);
    [[nodiscard]] static u32 slot_count(MfIface iface);
    /// stdcall bytes-to-pop for @p slot (x86), from the SDK prototypes.
    [[nodiscard]] static u32 slot_stdcall_bytes(MfIface iface, u32 slot);
    [[nodiscard]] static std::string slot_thunk_name(MfIface iface, u32 slot);

    /// Objects alive (tests).
    [[nodiscard]] usize live_object_count() const;

    /// Attribute value: one of the seven MF_ATTRIBUTE_TYPE PROPVARIANT types,
    /// or Empty (vt 0) for the key a rejected SetItem leaves behind (measured).
    struct Item {
        MfGuid key{};
        u16 vt{0};
        u32 u32v{0};
        u64 u64v{0};
        double dbl{0.0};
        MfGuid guid{};
        std::u16string str;
        std::vector<u8> blob;
        GuestAddr punk{0};
    };

private:
    struct Store {
        std::recursive_mutex mutex;
        std::vector<Item> items;
        u32 refs{1};
        u32 lock_depth{0};
        MfIface iface{MfIface::Attributes};
    };
    using StorePtr = std::shared_ptr<Store>;

    [[nodiscard]] StorePtr find(GuestAddr self);
    [[nodiscard]] GuestAddr build_vtable(Marshaler& m, MfIface iface);
    [[nodiscard]] GuestAddr alloc_cell(Marshaler& m);
    void free_cell(GuestAddr cell);

    [[nodiscard]] u32 dispatch_unknown(u32 slot, Marshaler& m, GuestAddr self, Store& st);
    [[nodiscard]] u32 dispatch_attributes(u32 slot, Marshaler& m, GuestAddr self, Store& st);
    [[nodiscard]] u32 dispatch_media_type(u32 slot, Marshaler& m, GuestAddr self, Store& st);

    [[nodiscard]] static Item* find_item(Store& st, const MfGuid& key);
    [[nodiscard]] bool read_guid(Marshaler& m, GuestAddr addr, MfGuid& out) const;
    [[nodiscard]] bool write_guid(Marshaler& m, GuestAddr addr, const MfGuid& g) const;
    /// Read a guest PROPVARIANT into an Item (key left untouched).
    [[nodiscard]] bool read_propvariant(Marshaler& m, GuestAddr pv, Item& out);
    /// Write @p item as a PROPVARIANT the guest owns (task memory for
    /// strings, blobs and GUIDs; AddRef on an IUnknown).
    [[nodiscard]] u32 write_propvariant(Marshaler& m, GuestAddr pv, const Item& item);
    /// Replace or append @p item under its key (position kept, measured).
    void put_item(Marshaler& m, Store& st, Item item);
    [[nodiscard]] static bool items_equal(const Item& a, const Item& b);
    [[nodiscard]] static bool contains_equal(const Store& st, const Item& it);

    void guest_add_ref(Marshaler& m, GuestAddr punk);
    void guest_release(Marshaler& m, GuestAddr punk);
    [[nodiscard]] u32 copy_to_foreign(Marshaler& m, Store& src, GuestAddr dest);

    ThunkRegistry* m_thunks{nullptr};
    TaskAlloc m_task_alloc;
    TaskFree m_task_free;
    GuestVtableCall m_guest_call;

    std::atomic<u32> m_startups{0};
    std::atomic<u32> m_locks{0};

    mutable std::mutex m_mutex;
    std::unordered_map<GuestAddr, StorePtr> m_objects;
    std::vector<GuestAddr> m_free_cells;
    GuestAddr m_scratch_base{0};
    GuestAddr m_next_addr{0};
    std::array<GuestAddr, static_cast<usize>(MfIface::Count)> m_vtables{};
};

/// Register the per-slot vtable thunks under mfplat.dll's export table.
void register_mf_object_thunks(DllExports& exports, MfPlatManager& mgr);

} // namespace vitreo::winapi
