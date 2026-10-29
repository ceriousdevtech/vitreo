// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file dshow_host.hpp
/// @brief Host-implemented DirectShow objects the Filter Graph Manager adds
/// or hands out itself: File Source (Async), the DirectSound Renderer, the
/// memory allocator (CLSID_MemoryAllocator) and its samples, and the system
/// reference clock. Guest code reaches them through ole32 thunks; the graph
/// (and any host object) calls them directly through DShowHost::call.
///
/// Calling convention inside the host: "logical" arguments, one u64 per
/// declared parameter in SDK order after `this` (64-bit integers whole,
/// doubles as their bit pattern). to_logical / to_guest convert to and from
/// the guest ABI (an x86 LONGLONG / REFERENCE_TIME / double takes two stack
/// slots; on x64 every parameter is one slot, a double in the XMM register of
/// its position).

#include "winapi/com/ole32/com_core.hpp"
#include "winapi/core/marshaler.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi::dshow {

/// One entry per interface Vitreo implements on the host side.
enum class Iface : u8 {
    BaseFilter,
    FileSourceFilter,
    Pin,
    AsyncReader,
    EnumPins,
    EnumMediaTypes,
    MiscFlags,
    ReferenceClock,
    BasicAudio,
    MemInputPin,
    MemAllocator,
    MediaSample,
    MediaSeeking,
    Count,
};

/// The interface's SDK name as it appears in dshow_method_table.inc.
[[nodiscard]] std::string_view iface_name(Iface iface);
/// Number of vtable slots including IUnknown.
[[nodiscard]] u32 iface_slots(Iface iface);
/// x86 stdcall bytes of slot @p slot (IUnknown slots included).
[[nodiscard]] u16 stdcall_bytes(Iface iface, u32 slot);
/// Declared parameters after `this` (wide parameters count once).
[[nodiscard]] u32 param_count(Iface iface, u32 slot);
/// Parameter kinds of the few methods with 64-bit or floating parameters.
enum class Param : u8 { Narrow, Wide, Double };
[[nodiscard]] Param param_kind(Iface iface, u32 slot, u32 index);

/// Convert the thunk's guest arguments (after `this`) to logical arguments.
[[nodiscard]] std::vector<u64> to_logical(const Marshaler& m, Iface iface, u32 slot);
/// Convert logical arguments to the guest ABI (for calls into guest code).
[[nodiscard]] std::vector<u64> to_guest(bool x64, Iface iface, u32 slot,
                                        std::span<const u64> logical);
/// Convert guest-ABI argument slots (after `this`) back to logical ones.
[[nodiscard]] std::vector<u64> from_guest(bool x64, Iface iface, u32 slot,
                                          std::span<const u64> guest);

class DShowHost;

/// A host COM object: a set of interface pointers sharing one count.
class HostObject : public std::enable_shared_from_this<HostObject> {
public:
    virtual ~HostObject() = default;
    /// Methods after IUnknown (slot >= 3). `args` are logical.
    virtual u32 call(Marshaler& m, Iface iface, u32 slot, std::span<const u64> args) = 0;
    /// Interfaces this object answers QueryInterface for, by IID.
    [[nodiscard]] virtual bool interface_for(const Guid& iid, Iface& out) const = 0;
    /// Called once when the count reaches zero, before the pointers go.
    virtual void final_release(Marshaler& m) { (void)m; }
    /// The count reached zero: return true to keep the object (and its
    /// pointers) alive, as a pooled media sample returns to its allocator.
    virtual bool recycle(Marshaler& m) { (void)m; return false; }

    [[nodiscard]] GuestAddr pointer(Iface iface) const;
    /// The identity (IUnknown) pointer: the first interface created.
    [[nodiscard]] GuestAddr identity() const { return m_ifaces.empty() ? 0 : m_ifaces[0].second; }
    u32 add_ref() { return m_refs.fetch_add(1) + 1; }

    DShowHost* host{nullptr};
    /// Pins count on their filter (CBasePin::NonDelegatingAddRef): AddRef
    /// and Release on the pin move the owner's count.
    HostObject* count_owner{nullptr};

    void set_refs(u32 refs) { m_refs.store(refs); }
    [[nodiscard]] u32 refs() const { return m_refs.load(); }

protected:
    friend class DShowHost;
    std::atomic<u32> m_refs{1};
    std::vector<std::pair<Iface, GuestAddr>> m_ifaces;
};

/// Registry of host objects and the bridge between thunks, the graph and
/// the objects.
class DShowHost {
public:
    explicit DShowHost(ComCoreManager& mgr) : m_mgr(mgr) {}

    ComCoreManager& mgr() { return m_mgr; }

    /// Give @p object guest-visible pointers for @p ifaces (the first is the
    /// identity). Returns false when guest memory or thunks are missing.
    bool publish(Marshaler& m, const std::shared_ptr<HostObject>& object,
                 std::initializer_list<Iface> ifaces);
    /// Unregister an object's pointers and free them (pool teardown).
    void discard(const std::shared_ptr<HostObject>& object);
    /// The object behind an interface pointer, or null for guest objects.
    [[nodiscard]] std::shared_ptr<HostObject> find(GuestAddr pointer, Iface* iface = nullptr) const;

    /// A thunk: `this` is arg 0, the rest are read per the method table.
    u32 thunk(Marshaler& m, Iface iface, u32 slot);
    /// Call slot @p slot of any COM pointer (host objects directly, guest
    /// objects through the guest caller) with logical arguments.
    u32 call(Marshaler& m, GuestAddr pointer, Iface iface, u32 slot,
             std::initializer_list<u64> args);
    u32 call(Marshaler& m, GuestAddr pointer, Iface iface, u32 slot,
             std::span<const u64> args);
    /// IUnknown on any pointer.
    u32 add_ref(Marshaler& m, GuestAddr pointer);
    u32 release(Marshaler& m, GuestAddr pointer);
    /// QueryInterface(iid) on any pointer; 0 when not supported.
    GuestAddr query(Marshaler& m, GuestAddr pointer, const Guid& iid);

    /// Guest scratch for out-parameters of calls the host makes (per call).
    GuestAddr scratch(u32 bytes);
    void free_scratch(GuestAddr address);

    void append_thunks(DllExports& exports);

private:
    u32 unknown(Marshaler& m, GuestAddr self, u32 slot);
    GuestAddr vtable(Marshaler& m, Iface iface);

    ComCoreManager& m_mgr;
    mutable std::mutex m_mutex;
    struct Binding {
        std::shared_ptr<HostObject> object;
        Iface iface;
    };
    std::unordered_map<GuestAddr, Binding> m_pointers;
    std::array<GuestAddr, 2 * static_cast<usize>(Iface::Count)> m_vtables{};
};

/// Thunk export name for a host interface slot.
[[nodiscard]] std::string thunk_name(Iface iface, u32 slot);

// ---------------------------------------------------------------- objects

/// CLSID_MemoryAllocator (quartz CMemAllocator semantics). Returns the
/// IMemAllocator pointer (count 1), or 0.
GuestAddr create_memory_allocator(DShowHost& host, Marshaler& m);
/// CLSID_SystemClock: IReferenceClock (count 1), or 0.
GuestAddr create_system_clock(DShowHost& host, Marshaler& m);
/// CLSID_AsyncReader: IBaseFilter (count 1), or 0. Reads through the VFS.
GuestAddr create_async_reader(DShowHost& host, Marshaler& m);
/// The system clock's time (100 ns units, timeGetTime base).
[[nodiscard]] i64 reference_clock_now();
/// CLSID_DSoundRender: IBaseFilter (count 1), or 0 when there is no host
/// audio device (VFW_E_NO_AUDIO_HARDWARE).
GuestAddr create_audio_renderer(DShowHost& host, Marshaler& m, u32* hr_out);

/// IFileSourceFilter::Load on a host async reader without a guest string
/// (the graph's RenderFile / AddSourceFilter).
u32 async_reader_load(DShowHost& host, Marshaler& m, GuestAddr filter,
                      const std::u16string& guest_path);

// SDK GUIDs used across the host objects (uuids.h / strmif.h).
inline constexpr Guid CLSID_ASYNC_READER{0xE436EBB5u, 0x524Fu, 0x11CEu,
                                         {0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
inline constexpr Guid CLSID_DSOUND_RENDER{0x79376820u, 0x07D0u, 0x11CFu,
                                          {0xA2, 0x4D, 0x00, 0x20, 0xAF, 0xD7, 0x97, 0x67}};
inline constexpr Guid CLSID_MEMORY_ALLOCATOR{0x1E651CC0u, 0xB199u, 0x11D0u,
                                             {0x82, 0x12, 0x00, 0xC0, 0x4F, 0xC3, 0x2C, 0x45}};
inline constexpr Guid CLSID_SYSTEM_CLOCK{0xE436EBB1u, 0x524Fu, 0x11CEu,
                                         {0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};

} // namespace vitreo::winapi::dshow
