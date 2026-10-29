// SPDX-License-Identifier: LGPL-2.1-only
/// @file dshow_objects.cpp
/// @brief AM_MEDIA_TYPE marshaling, the IBaseFilter / IPin bases and the
/// pin and media-type enumerators of the host DirectShow filters.

#include "winapi/com/ole32/dshow_objects.hpp"

#include <algorithm>

namespace vitreo::winapi::dshow {

namespace {

struct MtLayout {
    u32 size, punk, cb, pb;
};
[[nodiscard]] MtLayout mt_layout(const Marshaler& m) {
    return m.cpu_is_64bit() ? MtLayout{88, 64, 72, 80} : MtLayout{72, 60, 64, 68};
}

constexpr u32 kMaxName = 128;  // MAX_FILTER_NAME / MAX_PIN_NAME

} // namespace

MediaType read_media_type(Marshaler& m, GuestAddr mt) {
    MediaType type;
    if (mt == 0) return type;
    const MtLayout l = mt_layout(m);
    (void)m.memory().read_bytes(mt + 0, reinterpret_cast<Byte*>(&type.major), 16);
    (void)m.memory().read_bytes(mt + 16, reinterpret_cast<Byte*>(&type.sub), 16);
    type.fixed = m.read_u32(mt + 32).value_or(0) != 0;
    type.temporal = m.read_u32(mt + 36).value_or(0) != 0;
    type.sample_size = m.read_u32(mt + 40).value_or(0);
    (void)m.memory().read_bytes(mt + 44, reinterpret_cast<Byte*>(&type.format), 16);
    const u32 cb = m.read_u32(mt + l.cb).value_or(0);
    const GuestAddr pb = static_cast<GuestAddr>(m.read_ptr(mt + l.pb).value_or(0));
    if (cb != 0 && pb != 0 && cb <= 0x100000u) {
        type.format_bytes.resize(cb);
        (void)m.memory().read_bytes(pb, type.format_bytes.data(), cb);
    }
    return type;
}

bool write_media_type(DShowHost& host, Marshaler& m, GuestAddr mt, const MediaType& type) {
    const MtLayout l = mt_layout(m);
    for (u32 i = 0; i < l.size; i += 4) (void)m.write_u32(mt + i, 0);
    (void)m.write_struct<Guid>(mt + 0, type.major);
    (void)m.write_struct<Guid>(mt + 16, type.sub);
    (void)m.write_u32(mt + 32, type.fixed ? 1u : 0u);
    (void)m.write_u32(mt + 36, type.temporal ? 1u : 0u);
    (void)m.write_u32(mt + 40, type.sample_size);
    (void)m.write_struct<Guid>(mt + 44, type.format);
    (void)m.write_ptr(mt + l.punk, 0);
    GuestAddr pb = 0;
    if (!type.format_bytes.empty()) {
        pb = host.mgr().co_task_mem_alloc_guest(type.format_bytes.size());
        if (pb == 0) return false;
        (void)m.memory().write_bytes(pb, type.format_bytes.data(), type.format_bytes.size());
    }
    (void)m.write_u32(mt + l.cb, static_cast<u32>(type.format_bytes.size()));
    (void)m.write_ptr(mt + l.pb, pb);
    return true;
}

GuestAddr alloc_media_type(DShowHost& host, Marshaler& m, const MediaType& type) {
    const GuestAddr mt = host.mgr().co_task_mem_alloc_guest(mt_layout(m).size);
    if (mt == 0) return 0;
    if (!write_media_type(host, m, mt, type)) {
        host.mgr().co_task_mem_free_guest(mt);
        return 0;
    }
    return mt;
}

std::u16string read_wide(Marshaler& m, GuestAddr ptr, usize limit) {
    std::u16string text;
    if (ptr == 0) return text;
    for (GuestAddr at = ptr; text.size() < limit; at += 2) {
        const u16 unit = m.memory().read_u16(at).value_or(0);
        if (unit == 0) break;
        text.push_back(static_cast<char16_t>(unit));
    }
    return text;
}

void write_wide(Marshaler& m, GuestAddr ptr, const std::u16string& text) {
    for (usize i = 0; i < text.size(); ++i) {
        (void)m.write_u16(ptr + static_cast<GuestAddr>(i * 2), static_cast<u16>(text[i]));
    }
    (void)m.write_u16(ptr + static_cast<GuestAddr>(text.size() * 2), 0);
}

// ------------------------------------------------------------ enumerators

namespace {

class PinEnum final : public HostObject {
public:
    std::vector<GuestAddr> pins;
    u32 position{0};
    std::mutex mutex;

    bool interface_for(const Guid& iid, Iface& out) const override {
        if (!guid_eq(iid, sdk::IID_IEnumPins)) return false;
        out = Iface::EnumPins;
        return true;
    }
    void final_release(Marshaler& m) override {
        for (const GuestAddr pin : pins) (void)host->release(m, pin);
    }
    u32 call(Marshaler& m, Iface, u32 slot, std::span<const u64> args) override {
        const auto arg = [&](usize i) { return i < args.size() ? args[i] : 0; };
        const GuestAddr ps = m.cpu_is_64bit() ? 8u : 4u;
        std::unique_lock<std::mutex> lock(mutex);
        switch (slot) {
        case 3: {  // Next(ULONG cPins, IPin** ppPins, ULONG* pcFetched)
            const u32 wanted = static_cast<u32>(arg(0));
            const GuestAddr out = static_cast<GuestAddr>(arg(1));
            const GuestAddr fetched = static_cast<GuestAddr>(arg(2));
            if (out == 0) return hr::E_POINTER;
            if (wanted > 1 && fetched == 0) return hr::E_INVALIDARG;
            u32 got = 0;
            std::vector<GuestAddr> handed;
            while (got < wanted && position < pins.size()) {
                handed.push_back(pins[position++]);
                ++got;
            }
            lock.unlock();
            for (u32 i = 0; i < handed.size(); ++i) {
                (void)host->add_ref(m, handed[i]);
                (void)m.write_ptr(out + i * ps, handed[i]);
            }
            if (fetched != 0) (void)m.write_u32(fetched, got);
            return got == wanted ? hr::S_OK : hr::S_FALSE;
        }
        case 4: {  // Skip
            const u32 n = static_cast<u32>(arg(0));
            const u32 left = static_cast<u32>(pins.size()) - position;
            if (n <= left) {
                position += n;
                return hr::S_OK;
            }
            position = static_cast<u32>(pins.size());
            return hr::S_FALSE;
        }
        case 5:  // Reset
            position = 0;
            return hr::S_OK;
        case 6: {  // Clone
            const GuestAddr out = static_cast<GuestAddr>(arg(0));
            if (out == 0) return hr::E_POINTER;
            std::vector<GuestAddr> copy = pins;
            const u32 at = position;
            lock.unlock();
            const GuestAddr clone = create_pin_enum(*host, m, copy);
            if (clone == 0) return hr::E_OUTOFMEMORY;
            if (auto object = std::static_pointer_cast<PinEnum>(host->find(clone))) {
                object->position = at;
            }
            (void)m.write_ptr(out, clone);
            return hr::S_OK;
        }
        default:
            return hr::E_NOTIMPL;
        }
    }
};

class TypeEnum final : public HostObject {
public:
    std::vector<MediaType> types;
    u32 position{0};
    std::mutex mutex;

    bool interface_for(const Guid& iid, Iface& out) const override {
        if (!guid_eq(iid, sdk::IID_IEnumMediaTypes)) return false;
        out = Iface::EnumMediaTypes;
        return true;
    }
    u32 call(Marshaler& m, Iface, u32 slot, std::span<const u64> args) override {
        const auto arg = [&](usize i) { return i < args.size() ? args[i] : 0; };
        const GuestAddr ps = m.cpu_is_64bit() ? 8u : 4u;
        std::unique_lock<std::mutex> lock(mutex);
        switch (slot) {
        case 3: {  // Next(ULONG, AM_MEDIA_TYPE**, ULONG*)
            const u32 wanted = static_cast<u32>(arg(0));
            const GuestAddr out = static_cast<GuestAddr>(arg(1));
            const GuestAddr fetched = static_cast<GuestAddr>(arg(2));
            if (out == 0) return hr::E_POINTER;
            if (wanted > 1 && fetched == 0) return hr::E_INVALIDARG;
            u32 got = 0;
            while (got < wanted && position < types.size()) {
                const GuestAddr mt = alloc_media_type(*host, m, types[position]);
                if (mt == 0) break;
                (void)m.write_ptr(out + got * ps, mt);
                ++position;
                ++got;
            }
            if (fetched != 0) (void)m.write_u32(fetched, got);
            return got == wanted ? hr::S_OK : hr::S_FALSE;
        }
        case 4: {
            const u32 n = static_cast<u32>(arg(0));
            const u32 left = static_cast<u32>(types.size()) - position;
            if (n <= left) {
                position += n;
                return hr::S_OK;
            }
            position = static_cast<u32>(types.size());
            return hr::S_FALSE;
        }
        case 5:
            position = 0;
            return hr::S_OK;
        case 6: {
            const GuestAddr out = static_cast<GuestAddr>(arg(0));
            if (out == 0) return hr::E_POINTER;
            std::vector<MediaType> copy = types;
            const u32 at = position;
            lock.unlock();
            const GuestAddr clone = create_type_enum(*host, m, copy);
            if (clone == 0) return hr::E_OUTOFMEMORY;
            if (auto object = std::static_pointer_cast<TypeEnum>(host->find(clone))) {
                object->position = at;
            }
            (void)m.write_ptr(out, clone);
            return hr::S_OK;
        }
        default:
            return hr::E_NOTIMPL;
        }
    }
};

} // namespace

GuestAddr create_pin_enum(DShowHost& host, Marshaler& m, std::vector<GuestAddr> pins) {
    auto e = std::make_shared<PinEnum>();
    e->pins = std::move(pins);
    if (!host.publish(m, e, {Iface::EnumPins})) return 0;
    for (const GuestAddr pin : e->pins) (void)host.add_ref(m, pin);
    return e->pointer(Iface::EnumPins);
}

GuestAddr create_type_enum(DShowHost& host, Marshaler& m, std::vector<MediaType> types) {
    auto e = std::make_shared<TypeEnum>();
    e->types = std::move(types);
    if (!host.publish(m, e, {Iface::EnumMediaTypes})) return 0;
    return e->pointer(Iface::EnumMediaTypes);
}

bool publish_pin(DShowHost& host, Marshaler& m, FilterBase& owner,
                 const std::shared_ptr<PinBase>& pin, std::initializer_list<Iface> ifaces) {
    pin->filter = &owner;
    pin->count_owner = &owner;
    if (!host.publish(m, pin, ifaces)) return false;
    owner.pins.push_back(pin);
    return true;
}

// ------------------------------------------------------------ FilterBase

bool FilterBase::interface_for(const Guid& iid, Iface& out) const {
    if (guid_eq(iid, sdk::IID_IPersist) || guid_eq(iid, sdk::IID_IMediaFilter) ||
        guid_eq(iid, sdk::IID_IBaseFilter)) {
        out = Iface::BaseFilter;
        return true;
    }
    return false;
}

void FilterBase::final_release(Marshaler& m) {
    GuestAddr old_clock = 0;
    {
        std::lock_guard<std::mutex> lock(filter_mutex);
        old_clock = clock;
        clock = 0;
    }
    if (old_clock != 0) (void)host->release(m, old_clock);
    for (auto& pin : pins) {
        pin->drop_peer(m);
        host->discard(pin);
    }
    pins.clear();
}

u32 FilterBase::on_stop(Marshaler& m) {
    (void)m;
    std::lock_guard<std::mutex> lock(filter_mutex);
    state = 0;
    return hr::S_OK;
}
u32 FilterBase::on_pause(Marshaler& m) {
    (void)m;
    std::lock_guard<std::mutex> lock(filter_mutex);
    state = 1;
    return hr::S_OK;
}
u32 FilterBase::on_run(Marshaler& m, i64 start) {
    (void)m;
    std::lock_guard<std::mutex> lock(filter_mutex);
    state = 2;
    start_time = start;
    return hr::S_OK;
}
u32 FilterBase::on_get_state(Marshaler& m, u32 timeout_ms, u32& state_out) {
    (void)m;
    (void)timeout_ms;
    std::lock_guard<std::mutex> lock(filter_mutex);
    state_out = state;
    return hr::S_OK;
}
u32 FilterBase::call_other(Marshaler& m, Iface iface, u32 slot, std::span<const u64> args) {
    (void)m; (void)iface; (void)slot; (void)args;
    return hr::E_NOTIMPL;
}
std::vector<std::shared_ptr<PinBase>> FilterBase::visible_pins() { return pins; }

u32 FilterBase::call(Marshaler& m, Iface iface, u32 slot, std::span<const u64> args) {
    if (iface != Iface::BaseFilter) return call_other(m, iface, slot, args);
    const auto arg = [&](usize i) { return i < args.size() ? args[i] : 0; };
    switch (slot) {
    case 3: {  // GetClassID(CLSID*)
        const GuestAddr out = static_cast<GuestAddr>(arg(0));
        if (out == 0) return hr::E_POINTER;
        (void)m.write_struct<Guid>(out, clsid);
        return hr::S_OK;
    }
    case 4: return on_stop(m);
    case 5: return on_pause(m);
    case 6: return on_run(m, static_cast<i64>(arg(0)));
    case 7: {  // GetState(DWORD, FILTER_STATE*)
        const GuestAddr out = static_cast<GuestAddr>(arg(1));
        if (out == 0) return hr::E_POINTER;
        u32 s = 0;
        const u32 result = on_get_state(m, static_cast<u32>(arg(0)), s);
        (void)m.write_u32(out, s);
        return result;
    }
    case 8: {  // SetSyncSource(IReferenceClock*): CBaseFilter holds a reference
        const GuestAddr next = static_cast<GuestAddr>(arg(0));
        if (next != 0) (void)host->add_ref(m, next);
        GuestAddr old = 0;
        {
            std::lock_guard<std::mutex> lock(filter_mutex);
            old = clock;
            clock = next;
        }
        if (old != 0) (void)host->release(m, old);
        return hr::S_OK;
    }
    case 9: {  // GetSyncSource(IReferenceClock**)
        const GuestAddr out = static_cast<GuestAddr>(arg(0));
        if (out == 0) return hr::E_POINTER;
        GuestAddr c = 0;
        {
            std::lock_guard<std::mutex> lock(filter_mutex);
            c = clock;
        }
        if (c != 0) (void)host->add_ref(m, c);
        (void)m.write_ptr(out, c);
        return hr::S_OK;
    }
    case 10: {  // EnumPins(IEnumPins**)
        const GuestAddr out = static_cast<GuestAddr>(arg(0));
        if (out == 0) return hr::E_POINTER;
        std::vector<GuestAddr> list;
        for (auto& pin : visible_pins()) list.push_back(pin->pointer(Iface::Pin));
        const GuestAddr e = create_pin_enum(*host, m, std::move(list));
        (void)m.write_ptr(out, e);
        return e != 0 ? hr::S_OK : hr::E_OUTOFMEMORY;
    }
    case 11: {  // FindPin(LPCWSTR Id, IPin**)
        const GuestAddr id = static_cast<GuestAddr>(arg(0));
        const GuestAddr out = static_cast<GuestAddr>(arg(1));
        if (id == 0 || out == 0) return hr::E_POINTER;
        const std::u16string wanted = read_wide(m, id);
        for (auto& pin : visible_pins()) {
            if (pin->name == wanted) {
                const GuestAddr p = pin->pointer(Iface::Pin);
                (void)host->add_ref(m, p);
                (void)m.write_ptr(out, p);
                return hr::S_OK;
            }
        }
        (void)m.write_ptr(out, 0);
        return hr::VFW_E_NOT_FOUND;
    }
    case 12: {  // QueryFilterInfo(FILTER_INFO*): achName[128], then pGraph (AddRef'd)
        const GuestAddr info = static_cast<GuestAddr>(arg(0));
        if (info == 0) return hr::E_POINTER;
        std::u16string text;
        GuestAddr g = 0;
        {
            std::lock_guard<std::mutex> lock(filter_mutex);
            text = name.substr(0, kMaxName - 1);
            g = graph;
        }
        for (u32 i = 0; i < kMaxName; ++i) (void)m.write_u16(info + i * 2, 0);
        write_wide(m, info, text);
        if (g != 0) (void)host->add_ref(m, g);
        (void)m.write_ptr(info + 2 * kMaxName, g);
        return hr::S_OK;
    }
    case 13: {  // JoinFilterGraph(IFilterGraph*, LPCWSTR): no reference (cycle)
        std::lock_guard<std::mutex> lock(filter_mutex);
        graph = static_cast<GuestAddr>(arg(0));
        name = graph != 0 ? read_wide(m, static_cast<GuestAddr>(arg(1)), kMaxName - 1) : u"";
        return hr::S_OK;
    }
    case 14:  // QueryVendorInfo
        return hr::E_NOTIMPL;
    default:
        return hr::E_NOTIMPL;
    }
}

// ------------------------------------------------------------ PinBase

bool PinBase::interface_for(const Guid& iid, Iface& out) const {
    if (guid_eq(iid, sdk::IID_IPin)) {
        out = Iface::Pin;
        return true;
    }
    return false;
}

void PinBase::drop_peer(Marshaler& m) {
    GuestAddr old = 0;
    {
        std::lock_guard<std::mutex> lock(pin_mutex);
        old = peer;
        peer = 0;
    }
    if (old != 0) {
        on_disconnect(m);
        (void)host->release(m, old);
    }
}

u32 PinBase::try_connect(Marshaler& m, GuestAddr receiver, const MediaType& type) {
    if (!accept(m, type)) return hr::VFW_E_TYPE_NOT_ACCEPTED;
    const GuestAddr mt = host->mgr().co_task_mem_alloc_guest(96);
    if (mt == 0) return hr::E_OUTOFMEMORY;
    (void)write_media_type(*host, m, mt, type);
    const GuestAddr self = pointer(Iface::Pin);
    const u32 result = host->call(m, receiver, Iface::Pin, 4, {self, mt});  // ReceiveConnection
    const GuestAddr pb = static_cast<GuestAddr>(m.read_ptr(mt + (m.cpu_is_64bit() ? 80u : 68u)).value_or(0));
    if (pb != 0) host->mgr().co_task_mem_free_guest(pb);
    host->mgr().co_task_mem_free_guest(mt);
    if (result != hr::S_OK) return result;
    (void)host->add_ref(m, receiver);
    {
        std::lock_guard<std::mutex> lock(pin_mutex);
        peer = receiver;
        connected_type = type;
    }
    const u32 completed = complete_connect(m);
    if (static_cast<i32>(completed) < 0) {
        (void)host->call(m, receiver, Iface::Pin, 5, {});  // receiver Disconnect
        drop_peer(m);
        return completed;
    }
    return hr::S_OK;
}

u32 PinBase::call(Marshaler& m, Iface iface, u32 slot, std::span<const u64> args) {
    if (iface != Iface::Pin) return call_other(m, iface, slot, args);
    const auto arg = [&](usize i) { return i < args.size() ? args[i] : 0; };
    const auto stopped = [&] {
        std::lock_guard<std::mutex> lock(filter->filter_mutex);
        return filter->state == 0;
    };
    const auto connected = [&] {
        std::lock_guard<std::mutex> lock(pin_mutex);
        return peer;
    };
    switch (slot) {
    case 3: {  // Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt)
        const GuestAddr receiver = static_cast<GuestAddr>(arg(0));
        if (receiver == 0) return hr::E_POINTER;
        if (connected() != 0) return hr::VFW_E_ALREADY_CONNECTED;
        if (!stopped()) return hr::VFW_E_NOT_STOPPED;
        if (!output) return hr::E_UNEXPECTED;
        // CBasePin::CheckConnect: the other pin must face the other way.
        const GuestAddr dir = host->scratch(8);
        (void)m.write_u32(dir, 0xFFFFFFFFu);
        (void)host->call(m, receiver, Iface::Pin, 9, {dir});
        const u32 their_dir = m.read_u32(dir).value_or(0xFFFFFFFFu);
        host->free_scratch(dir);
        if (their_dir != 0) return 0x80040208u;  // VFW_E_INVALID_DIRECTION
        const GuestAddr pmt = static_cast<GuestAddr>(arg(1));
        const MediaType wanted = read_media_type(m, pmt);
        const Guid null{};
        const auto matches = [&](const MediaType& t) {
            if (pmt == 0) return true;
            return (guid_eq(wanted.major, null) || guid_eq(wanted.major, t.major)) &&
                   (guid_eq(wanted.sub, null) || guid_eq(wanted.sub, t.sub)) &&
                   (guid_eq(wanted.format, null) || guid_eq(wanted.format, t.format));
        };
        if (pmt != 0 && !guid_eq(wanted.major, null) && !guid_eq(wanted.sub, null) &&
            !guid_eq(wanted.format, null)) {
            return try_connect(m, receiver, wanted);
        }
        u32 last = hr::VFW_E_NO_ACCEPTABLE_TYPES;
        // Measured (dshow_renderfile_probe): File Source (Async) offers its
        // own types first, then the receiver's enumeration.
        for (const MediaType& t : types()) {
            if (!matches(t)) continue;
            const u32 r = try_connect(m, receiver, t);
            if (r == hr::S_OK) return r;
            if (r != hr::VFW_E_TYPE_NOT_ACCEPTED) last = r;
        }
        const GuestAddr enum_out = host->scratch(8);
        (void)m.write_ptr(enum_out, 0);
        if (host->call(m, receiver, Iface::Pin, 12, {enum_out}) == hr::S_OK) {
            const GuestAddr e = static_cast<GuestAddr>(m.read_ptr(enum_out).value_or(0));
            const GuestAddr item = host->scratch(16);
            for (u32 n = 0; e != 0 && n < 64; ++n) {
                (void)m.write_ptr(item, 0);
                if (host->call(m, e, Iface::EnumMediaTypes, 3, {1, item, item + 8}) != hr::S_OK) break;
                const GuestAddr mt = static_cast<GuestAddr>(m.read_ptr(item).value_or(0));
                if (mt == 0) break;
                const MediaType t = read_media_type(m, mt);
                const GuestAddr pb = static_cast<GuestAddr>(m.read_ptr(mt + (m.cpu_is_64bit() ? 80u : 68u)).value_or(0));
                if (pb != 0) host->mgr().co_task_mem_free_guest(pb);
                host->mgr().co_task_mem_free_guest(mt);
                if (!matches(t)) continue;
                const u32 r = try_connect(m, receiver, t);
                if (r == hr::S_OK) {
                    (void)host->release(m, e);
                    host->free_scratch(item);
                    host->free_scratch(enum_out);
                    return r;
                }
                if (r != hr::VFW_E_TYPE_NOT_ACCEPTED) last = r;
            }
            host->free_scratch(item);
            if (e != 0) (void)host->release(m, e);
        }
        host->free_scratch(enum_out);
        return last;
    }
    case 4: {  // ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt)
        const GuestAddr connector = static_cast<GuestAddr>(arg(0));
        const GuestAddr pmt = static_cast<GuestAddr>(arg(1));
        if (connector == 0 || pmt == 0) return hr::E_POINTER;
        if (connected() != 0) return hr::VFW_E_ALREADY_CONNECTED;
        if (!stopped()) return hr::VFW_E_NOT_STOPPED;
        if (output) return 0x80040208u;  // VFW_E_INVALID_DIRECTION
        const MediaType type = read_media_type(m, pmt);
        if (!accept(m, type)) return hr::VFW_E_TYPE_NOT_ACCEPTED;
        (void)host->add_ref(m, connector);
        {
            std::lock_guard<std::mutex> lock(pin_mutex);
            peer = connector;
            connected_type = type;
        }
        const u32 completed = complete_connect(m);
        if (static_cast<i32>(completed) < 0) {
            drop_peer(m);
            return completed;
        }
        return hr::S_OK;
    }
    case 5: {  // Disconnect
        if (!stopped()) return hr::VFW_E_NOT_STOPPED;
        if (connected() == 0) return hr::S_FALSE;
        drop_peer(m);
        return hr::S_OK;
    }
    case 6: {  // ConnectedTo(IPin**)
        const GuestAddr out = static_cast<GuestAddr>(arg(0));
        if (out == 0) return hr::E_POINTER;
        const GuestAddr p = connected();
        if (p == 0) {
            (void)m.write_ptr(out, 0);
            return hr::VFW_E_NOT_CONNECTED;
        }
        (void)host->add_ref(m, p);
        (void)m.write_ptr(out, p);
        return hr::S_OK;
    }
    case 7: {  // ConnectionMediaType(AM_MEDIA_TYPE*)
        const GuestAddr out = static_cast<GuestAddr>(arg(0));
        if (out == 0) return hr::E_POINTER;
        MediaType t;
        {
            std::lock_guard<std::mutex> lock(pin_mutex);
            if (peer == 0) {
                const u32 size = m.cpu_is_64bit() ? 88u : 72u;
                for (u32 i = 0; i < size; i += 4) (void)m.write_u32(out + i, 0);
                return hr::VFW_E_NOT_CONNECTED;
            }
            t = connected_type;
        }
        return write_media_type(*host, m, out, t) ? hr::S_OK : hr::E_OUTOFMEMORY;
    }
    case 8: {  // QueryPinInfo(PIN_INFO*): pFilter (AddRef'd), dir, achName[128]
        const GuestAddr info = static_cast<GuestAddr>(arg(0));
        if (info == 0) return hr::E_POINTER;
        const bool x64 = m.cpu_is_64bit();
        const GuestAddr f = filter->identity();
        (void)host->add_ref(m, f);
        (void)m.write_ptr(info, f);
        (void)m.write_u32(info + (x64 ? 8u : 4u), output ? 1u : 0u);
        const GuestAddr name_at = info + (x64 ? 12u : 8u);
        for (u32 i = 0; i < kMaxName; ++i) (void)m.write_u16(name_at + i * 2, 0);
        write_wide(m, name_at, name.substr(0, kMaxName - 1));
        return hr::S_OK;
    }
    case 9: {  // QueryDirection(PIN_DIRECTION*)
        const GuestAddr out = static_cast<GuestAddr>(arg(0));
        if (out == 0) return hr::E_POINTER;
        (void)m.write_u32(out, output ? 1u : 0u);
        return hr::S_OK;
    }
    case 10: {  // QueryId(LPWSTR*)
        const GuestAddr out = static_cast<GuestAddr>(arg(0));
        if (out == 0) return hr::E_POINTER;
        const GuestAddr text = host->mgr().co_task_mem_alloc_guest((name.size() + 1) * 2);
        if (text == 0) return hr::E_OUTOFMEMORY;
        write_wide(m, text, name);
        (void)m.write_ptr(out, text);
        return hr::S_OK;
    }
    case 11: {  // QueryAccept(const AM_MEDIA_TYPE*)
        const GuestAddr pmt = static_cast<GuestAddr>(arg(0));
        if (pmt == 0) return hr::E_POINTER;
        return accept(m, read_media_type(m, pmt)) ? hr::S_OK : hr::S_FALSE;
    }
    case 12: {  // EnumMediaTypes(IEnumMediaTypes**)
        const GuestAddr out = static_cast<GuestAddr>(arg(0));
        if (out == 0) return hr::E_POINTER;
        const GuestAddr e = create_type_enum(*host, m, types());
        (void)m.write_ptr(out, e);
        return e != 0 ? hr::S_OK : hr::E_OUTOFMEMORY;
    }
    case 13:  // QueryInternalConnections
        return hr::E_NOTIMPL;
    case 14: return on_end_of_stream(m);
    case 15: return on_begin_flush(m);
    case 16: return on_end_flush(m);
    case 17: {
        double rate = 0.0;
        const u64 bits = arg(2);
        std::memcpy(&rate, &bits, sizeof(rate));
        return on_new_segment(m, static_cast<i64>(arg(0)), static_cast<i64>(arg(1)), rate);
    }
    default:
        return hr::E_NOTIMPL;
    }
}

} // namespace vitreo::winapi::dshow
