// SPDX-License-Identifier: LGPL-2.1-only
/// @file dshow_host.cpp
/// @brief Registry, vtables, thunks and the argument bridge for the
/// host-implemented DirectShow objects (dshow_host.hpp).

#include "winapi/com/ole32/dshow_host.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace vitreo::winapi::dshow {

namespace {

namespace sdk {
#include "winapi/com/ole32/dshow_guids.inc"
}

struct MethodRow {
    std::string_view iface;
    std::string_view method;
    u32 slot;
    u16 bytes;
};

constexpr MethodRow kMethods[] = {
#define DSHOW_METHOD(i, f, s, b) {#i, #f, s, b},
#include "winapi/com/ole32/dshow_method_table.inc"
#undef DSHOW_METHOD
};

constexpr std::string_view kIfaceNames[] = {
    "IBaseFilter",    "IFileSourceFilter", "IPin",           "IAsyncReader",
    "IEnumPins",      "IEnumMediaTypes",   "IAMFilterMiscFlags", "IReferenceClock",
    "IBasicAudio",    "IMemInputPin",      "IMemAllocator",  "IMediaSample",
    "IMediaSeeking",
};
static_assert(std::size(kIfaceNames) == static_cast<usize>(Iface::Count));

const MethodRow* row(Iface iface, u32 slot) {
    const std::string_view name = kIfaceNames[static_cast<usize>(iface)];
    for (const MethodRow& r : kMethods) {
        if (r.iface == name && r.slot == slot) return &r;
    }
    return nullptr;
}

/// The SDK signatures with 64-bit or floating parameters (strmif.h); every
/// other parameter of the implemented interfaces is pointer-sized or 32-bit.
/// Their stdcall byte counts in dshow_method_table.inc agree (checked by
/// param_count's arithmetic and the tests).
struct WideMethod {
    Iface iface;
    u32 slot;
    std::array<Param, 4> params;
    u32 count;
};
constexpr WideMethod kWide[] = {
    {Iface::BaseFilter, 6, {Param::Wide}, 1},                                    // Run(tStart)
    {Iface::Pin, 17, {Param::Wide, Param::Wide, Param::Double}, 3},              // NewSegment
    {Iface::AsyncReader, 7, {Param::Wide, Param::Narrow, Param::Narrow}, 3},     // SyncRead
    {Iface::ReferenceClock, 4, {Param::Wide, Param::Wide, Param::Narrow, Param::Narrow}, 4},
    {Iface::ReferenceClock, 5, {Param::Wide, Param::Wide, Param::Narrow, Param::Narrow}, 4},
    // ConvertTimeFormat(LONGLONG*, const GUID*, LONGLONG Source, const GUID*)
    {Iface::MediaSeeking, 13, {Param::Narrow, Param::Narrow, Param::Wide, Param::Narrow}, 4},
    {Iface::MediaSeeking, 17, {Param::Double}, 1},                               // SetRate(double)
};

const WideMethod* wide(Iface iface, u32 slot) {
    for (const WideMethod& w : kWide) {
        if (w.iface == iface && w.slot == slot) return &w;
    }
    return nullptr;
}

[[nodiscard]] bool guid_eq(const Guid& a, const Guid& b) {
    return std::memcmp(&a, &b, sizeof(Guid)) == 0;
}

constexpr Guid IID_IUNKNOWN_{0x00000000u, 0x0000u, 0x0000u,
                             {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr u32 S_OK_ = 0;
constexpr u32 E_POINTER_ = 0x80004003u;
constexpr u32 E_NOINTERFACE_ = 0x80004002u;
constexpr u32 E_INVALIDARG_ = 0x80070057u;
constexpr u32 E_FAIL_ = 0x80004005u;

} // namespace

std::string_view iface_name(Iface iface) { return kIfaceNames[static_cast<usize>(iface)]; }

u32 iface_slots(Iface iface) {
    u32 slots = 3;
    const std::string_view name = iface_name(iface);
    for (const MethodRow& r : kMethods) {
        if (r.iface == name) slots = std::max(slots, r.slot + 1);
    }
    return slots;
}

u16 stdcall_bytes(Iface iface, u32 slot) {
    if (slot == 0) return 12;  // QueryInterface(this, riid, ppv)
    if (slot <= 2) return 4;   // AddRef / Release
    const MethodRow* r = row(iface, slot);
    return r != nullptr ? r->bytes : 4;
}

u32 param_count(Iface iface, u32 slot) {
    if (slot == 0) return 2;
    if (slot <= 2) return 0;
    const u32 bytes = stdcall_bytes(iface, slot) - 4u;
    const WideMethod* w = wide(iface, slot);
    if (w == nullptr) return bytes / 4u;
    u32 wide_count = 0;
    for (u32 i = 0; i < w->count; ++i) {
        if (w->params[i] != Param::Narrow) ++wide_count;
    }
    return (bytes - 8u * wide_count) / 4u + wide_count;
}

Param param_kind(Iface iface, u32 slot, u32 index) {
    const WideMethod* w = wide(iface, slot);
    if (w == nullptr || index >= w->count) return Param::Narrow;
    return w->params[index];
}

std::vector<u64> to_logical(const Marshaler& m, Iface iface, u32 slot) {
    const u32 count = param_count(iface, slot);
    std::vector<u64> out;
    out.reserve(count);
    const bool x64 = m.cpu_is_64bit();
    usize position = 1;  // after `this`
    for (u32 i = 0; i < count; ++i) {
        const Param kind = param_kind(iface, slot, i);
        if (x64) {
            if (kind == Param::Double && position < 4) {
                double value = m.arg_f64(position);
                u64 bits = 0;
                std::memcpy(&bits, &value, sizeof(bits));
                out.push_back(bits);
            } else {
                out.push_back(m.arg(position));
            }
            ++position;
        } else if (kind == Param::Narrow) {
            out.push_back(m.arg(position) & 0xFFFFFFFFull);
            ++position;
        } else {
            out.push_back((m.arg(position) & 0xFFFFFFFFull) | (m.arg(position + 1) << 32));
            position += 2;
        }
    }
    return out;
}

std::vector<u64> to_guest(bool x64, Iface iface, u32 slot, std::span<const u64> logical) {
    std::vector<u64> out;
    for (u32 i = 0; i < logical.size(); ++i) {
        if (!x64 && param_kind(iface, slot, i) != Param::Narrow) {
            out.push_back(logical[i] & 0xFFFFFFFFull);
            out.push_back(logical[i] >> 32);
        } else {
            out.push_back(logical[i]);
        }
    }
    return out;
}

std::vector<u64> from_guest(bool x64, Iface iface, u32 slot, std::span<const u64> guest) {
    if (x64) return {guest.begin(), guest.end()};
    std::vector<u64> out;
    usize k = 0;
    for (u32 i = 0; k < guest.size(); ++i) {
        if (param_kind(iface, slot, i) != Param::Narrow && k + 1 < guest.size()) {
            out.push_back((guest[k] & 0xFFFFFFFFull) | (guest[k + 1] << 32));
            k += 2;
        } else {
            out.push_back(guest[k] & 0xFFFFFFFFull);
            ++k;
        }
    }
    return out;
}

std::string thunk_name(Iface iface, u32 slot) {
    return "Vitreo_DShow_" + std::string(iface_name(iface)) + "_" + std::to_string(slot);
}

GuestAddr HostObject::pointer(Iface iface) const {
    for (const auto& [kind, address] : m_ifaces) {
        if (kind == iface) return address;
    }
    return 0;
}

GuestAddr DShowHost::vtable(Marshaler& m, Iface iface) {
    const bool x64 = m.cpu_is_64bit();
    GuestAddr& cached = m_vtables[static_cast<usize>(iface) * 2 + (x64 ? 1 : 0)];
    if (cached != 0) return cached;
    ThunkRegistry* thunks = m_mgr.thunk_registry();
    if (thunks == nullptr) return 0;
    const u32 slots = iface_slots(iface);
    const GuestAddr ps = x64 ? 8u : 4u;
    const GuestAddr block = m_mgr.co_task_mem_alloc_guest(slots * ps + 16u);
    if (block == 0) return 0;
    const GuestAddr table = (block + 15u) & ~GuestAddr{15u};
    for (u32 slot = 0; slot < slots; ++slot) {
        auto address = thunks->get_thunk_address("ole32.dll", thunk_name(iface, slot), true);
        if (address.is_err()) return 0;
        (void)m.write_ptr(table + slot * ps, address.value());
    }
    cached = table;
    return table;
}

bool DShowHost::publish(Marshaler& m, const std::shared_ptr<HostObject>& object,
                        std::initializer_list<Iface> ifaces) {
    object->host = this;
    for (const Iface iface : ifaces) {
        const GuestAddr table = vtable(m, iface);
        if (table == 0) return false;
        const GuestAddr pointer = m_mgr.co_task_mem_alloc_guest(16);
        if (pointer == 0) return false;
        (void)m.write_ptr(pointer, table);
        object->m_ifaces.emplace_back(iface, pointer);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pointers[pointer] = Binding{object, iface};
    }
    return true;
}

std::shared_ptr<HostObject> DShowHost::find(GuestAddr pointer, Iface* iface) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_pointers.find(pointer);
    if (it == m_pointers.end()) return nullptr;
    if (iface != nullptr) *iface = it->second.iface;
    return it->second.object;
}

u32 DShowHost::unknown(Marshaler& m, GuestAddr self, u32 slot) {
    Iface kind{};
    auto object = find(self, &kind);
    if (object == nullptr) return slot == 0 ? E_NOINTERFACE_ : 0;
    if (object->count_owner != nullptr) {
        object = object->count_owner->shared_from_this();
    }
    if (std::getenv("VITREO_DSHOW_DIAG")) std::fprintf(stderr, "[dshow] %s identity=0x%llx via 0x%llx refs=%u\n", slot == 1 ? "addref" : "release", (unsigned long long)object->identity(), (unsigned long long)self, object->refs());
    if (slot == 1) return object->add_ref();
    if (slot == 2) {
        u32 current = object->m_refs.load();
        do {
            if (current == 0) return 0;  // over-release: Windows' counts do not wrap either
        } while (!object->m_refs.compare_exchange_weak(current, current - 1));
        const u32 left = current - 1;
        if (left != 0) return left;
        if (object->recycle(m)) return 0;
        if (std::getenv("VITREO_DSHOW_DIAG")) std::fprintf(stderr, "[dshow] destroy identity=0x%llx via 0x%llx\n", (unsigned long long)object->identity(), (unsigned long long)self);
        object->final_release(m);
        discard(object);
        return 0;
    }
    return E_FAIL_;
}

void DShowHost::discard(const std::shared_ptr<HostObject>& object) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& [iface, pointer] : object->m_ifaces) {
        m_pointers.erase(pointer);
        m_mgr.co_task_mem_free_guest(pointer);
    }
    object->m_ifaces.clear();
}

GuestAddr DShowHost::query(Marshaler& m, GuestAddr pointer, const Guid& iid) {
    const GuestAddr iid_ptr = scratch(32);
    if (iid_ptr == 0) return 0;
    (void)m.write_struct<Guid>(iid_ptr, iid);
    (void)m.write_ptr(iid_ptr + 16, 0);
    const u32 hr = call(m, pointer, Iface::BaseFilter, 0, {iid_ptr, iid_ptr + 16});
    const GuestAddr out = hr == S_OK_ ? static_cast<GuestAddr>(m.read_ptr(iid_ptr + 16).value_or(0)) : 0;
    free_scratch(iid_ptr);
    return out;
}

u32 DShowHost::add_ref(Marshaler& m, GuestAddr pointer) {
    return call(m, pointer, Iface::BaseFilter, 1, {});
}

u32 DShowHost::release(Marshaler& m, GuestAddr pointer) {
    return call(m, pointer, Iface::BaseFilter, 2, {});
}

u32 DShowHost::call(Marshaler& m, GuestAddr pointer, Iface iface, u32 slot,
                    std::initializer_list<u64> args) {
    return call(m, pointer, iface, slot, std::span<const u64>(args.begin(), args.size()));
}

u32 DShowHost::call(Marshaler& m, GuestAddr pointer, Iface iface, u32 slot,
                    std::span<const u64> args) {
    if (pointer == 0) return E_POINTER_;
    Iface kind{};
    if (auto object = find(pointer, &kind)) {
        if (slot == 0) {
            const GuestAddr riid = args.size() > 0 ? static_cast<GuestAddr>(args[0]) : 0;
            const GuestAddr ppv = args.size() > 1 ? static_cast<GuestAddr>(args[1]) : 0;
            if (ppv == 0) return E_POINTER_;
            (void)m.write_ptr(ppv, 0);
            if (riid == 0) return E_INVALIDARG_;
            auto iid = m.read_struct<Guid>(riid);
            if (iid.is_err()) return E_INVALIDARG_;
            GuestAddr out = 0;
            Iface found{};
            if (guid_eq(iid.value(), IID_IUNKNOWN_)) {
                out = object->identity();
            } else if (object->interface_for(iid.value(), found)) {
                out = object->pointer(found);
            }
            if (out == 0) return E_NOINTERFACE_;
            (object->count_owner != nullptr ? object->count_owner : object.get())->add_ref();
            (void)m.write_ptr(ppv, out);
            return S_OK_;
        }
        if (slot <= 2) return unknown(m, pointer, slot);
        return object->call(m, kind, slot, args);
    }
    // A guest object: the guest ABI.
    const std::vector<u64> guest = to_guest(m.cpu_is_64bit(), iface, slot, args);
    std::vector<u64> full;
    full.reserve(guest.size() + 1);
    full.push_back(pointer);
    full.insert(full.end(), guest.begin(), guest.end());
    if (m.cpu_is_64bit()) {
        // A double by value travels in the XMM register of its position on
        // x64; the guest caller passes integer registers only. Refuse rather
        // than call with an undefined value.
        for (u32 i = 0; i < args.size(); ++i) {
            if (param_kind(iface, slot, i) != Param::Double) continue;
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) {
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED quartz!%s slot %u into guest code on x64: "
                             "double argument in XMM\n",
                             std::string(iface_name(iface)).c_str(), slot);
            }
            return 0x80004001u;  // E_NOTIMPL
        }
    }
    return m_mgr.call_guest_slot(m, pointer, slot, full.data(), full.size());
}

u32 DShowHost::thunk(Marshaler& m, Iface iface, u32 slot) {
    const GuestAddr self = m.arg_ptr(0);
    std::vector<u64> args;
    if (slot == 0) {
        args = {m.arg(1), m.arg(2)};
    } else if (slot > 2) {
        args = to_logical(m, iface, slot);
    }
    return call(m, self, iface, slot, args);
}

GuestAddr DShowHost::scratch(u32 bytes) {
    return m_mgr.co_task_mem_alloc_guest(std::max<u32>(bytes, 16));
}

void DShowHost::free_scratch(GuestAddr address) {
    if (address != 0) m_mgr.co_task_mem_free_guest(address);
}

void DShowHost::append_thunks(DllExports& exports) {
    for (usize i = 0; i < static_cast<usize>(Iface::Count); ++i) {
        const Iface iface = static_cast<Iface>(i);
        for (u32 slot = 0; slot < iface_slots(iface); ++slot) {
            ExportEntry entry{thunk_name(iface, slot), 0,
                              [this, iface, slot](Marshaler& m) {
                                  m.set_return(thunk(m, iface, slot));
                              },
                              stdcall_bytes(iface, slot)};
            entry.internal = true;
            exports.entries.push_back(std::move(entry));
        }
    }
}

} // namespace vitreo::winapi::dshow
