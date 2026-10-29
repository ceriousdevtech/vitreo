// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file dshow_objects.hpp
/// @brief Shared pieces of the host DirectShow filters: AM_MEDIA_TYPE
/// marshaling, an IBaseFilter base (CBaseFilter behaviour), a pin base with
/// the CBasePin bookkeeping, and the IEnumPins / IEnumMediaTypes objects.
/// Internal to the ole32 dshow_*.cpp files.

#include "winapi/com/ole32/dshow_host.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace vitreo::winapi::dshow {

namespace sdk {
#include "winapi/com/ole32/dshow_guids.inc"
}

namespace hr {
constexpr u32 S_OK = 0;
constexpr u32 S_FALSE = 1;
constexpr u32 E_POINTER = 0x80004003u;
constexpr u32 E_NOTIMPL = 0x80004001u;
constexpr u32 E_UNEXPECTED = 0x8000FFFFu;
constexpr u32 E_INVALIDARG = 0x80070057u;
constexpr u32 E_OUTOFMEMORY = 0x8007000Eu;
constexpr u32 E_FAIL = 0x80004005u;
constexpr u32 VFW_E_ALREADY_CONNECTED = 0x80040204u;
constexpr u32 VFW_E_NO_ACCEPTABLE_TYPES = 0x80040207u;
constexpr u32 VFW_E_NOT_CONNECTED = 0x80040209u;
constexpr u32 VFW_E_NOT_FOUND = 0x80040216u;
constexpr u32 VFW_E_TYPE_NOT_ACCEPTED = 0x8004022Au;
constexpr u32 VFW_E_NOT_STOPPED = 0x80040224u;
constexpr u32 VFW_E_WRONG_STATE = 0x80040227u;
constexpr u32 VFW_E_TIMEOUT = 0x8004022Eu;
constexpr u32 VFW_E_BADALIGN = 0x8004020Eu;
constexpr u32 VFW_S_STATE_INTERMEDIATE = 0x00040237u;
}  // namespace hr

[[nodiscard]] inline bool guid_eq(const Guid& a, const Guid& b) {
    return std::memcmp(&a, &b, sizeof(Guid)) == 0;
}

/// An AM_MEDIA_TYPE by value (pUnk is never carried).
struct MediaType {
    Guid major{};
    Guid sub{};
    Guid format{};
    bool fixed{false};
    bool temporal{false};
    u32 sample_size{0};
    std::vector<u8> format_bytes;
};

/// strmif.h layouts (dshow_layout_probe): x86 72 bytes, pUnk 60, cbFormat
/// 64, pbFormat 68; x64 88 bytes, pUnk 64, cbFormat 72, pbFormat 80.
[[nodiscard]] MediaType read_media_type(Marshaler& m, GuestAddr mt);
/// Fill a caller-owned AM_MEDIA_TYPE; pbFormat is a new CoTaskMem block.
bool write_media_type(DShowHost& host, Marshaler& m, GuestAddr mt, const MediaType& type);
/// A new CoTaskMem AM_MEDIA_TYPE (IEnumMediaTypes::Next), or 0.
GuestAddr alloc_media_type(DShowHost& host, Marshaler& m, const MediaType& type);
/// The UTF-16 name at @p ptr (null-terminated, at most @p limit units).
[[nodiscard]] std::u16string read_wide(Marshaler& m, GuestAddr ptr, usize limit = 0x8000);
void write_wide(Marshaler& m, GuestAddr ptr, const std::u16string& text);

class PinBase;

/// IBaseFilter with CBaseFilter's state, clock, name and graph handling.
class FilterBase : public HostObject {
public:
    Guid clsid{};
    std::mutex filter_mutex;
    u32 state{0};  // FILTER_STATE: 0 stopped, 1 paused, 2 running
    GuestAddr clock{0};
    GuestAddr graph{0};
    std::u16string name;
    i64 start_time{0};
    std::vector<std::shared_ptr<PinBase>> pins;

    u32 call(Marshaler& m, Iface iface, u32 slot, std::span<const u64> args) override;
    bool interface_for(const Guid& iid, Iface& out) const override;
    void final_release(Marshaler& m) override;

protected:
    /// State hooks; the default changes `state` and succeeds.
    virtual u32 on_stop(Marshaler& m);
    virtual u32 on_pause(Marshaler& m);
    virtual u32 on_run(Marshaler& m, i64 start);
    /// GetState(ms): the default reports `state` with S_OK.
    virtual u32 on_get_state(Marshaler& m, u32 timeout_ms, u32& state_out);
    virtual u32 call_other(Marshaler& m, Iface iface, u32 slot, std::span<const u64> args);
    [[nodiscard]] virtual std::vector<std::shared_ptr<PinBase>> visible_pins();
};

/// IPin with CBasePin's connection bookkeeping. Subclasses decide types.
class PinBase : public HostObject {
public:
    FilterBase* filter{nullptr};
    bool output{false};
    std::u16string name;
    std::mutex pin_mutex;
    GuestAddr peer{0};
    MediaType connected_type;
    bool flushing{false};

    u32 call(Marshaler& m, Iface iface, u32 slot, std::span<const u64> args) override;
    bool interface_for(const Guid& iid, Iface& out) const override;
    /// Disconnect without a peer call (graph teardown).
    void drop_peer(Marshaler& m);

protected:
    /// Types this pin enumerates (IPin::EnumMediaTypes).
    [[nodiscard]] virtual std::vector<MediaType> types() { return {}; }
    /// IPin::QueryAccept / the check ReceiveConnection makes.
    virtual bool accept(Marshaler& m, const MediaType& type) = 0;
    /// After a connection is made (either direction).
    virtual u32 complete_connect(Marshaler& m) { (void)m; return hr::S_OK; }
    virtual void on_disconnect(Marshaler& m) { (void)m; }
    virtual u32 on_end_of_stream(Marshaler& m) { (void)m; return hr::E_UNEXPECTED; }
    virtual u32 on_begin_flush(Marshaler& m) { (void)m; return hr::E_UNEXPECTED; }
    virtual u32 on_end_flush(Marshaler& m) { (void)m; return hr::E_UNEXPECTED; }
    virtual u32 on_new_segment(Marshaler& m, i64 start, i64 stop, double rate) {
        (void)m; (void)start; (void)stop; (void)rate;
        return hr::E_UNEXPECTED;
    }
    virtual u32 call_other(Marshaler& m, Iface iface, u32 slot, std::span<const u64> args) {
        (void)m; (void)iface; (void)slot; (void)args;
        return hr::E_NOTIMPL;
    }
    /// Output pin Connect: try `type` on the receiver.
    u32 try_connect(Marshaler& m, GuestAddr receiver, const MediaType& type);
};

/// IEnumPins over a snapshot (pins AddRef'd as handed out).
GuestAddr create_pin_enum(DShowHost& host, Marshaler& m, std::vector<GuestAddr> pins);
/// IEnumMediaTypes over a list of types.
GuestAddr create_type_enum(DShowHost& host, Marshaler& m, std::vector<MediaType> types);

/// Publish @p pin as an IPin (plus @p extra) counted on @p owner.
bool publish_pin(DShowHost& host, Marshaler& m, FilterBase& owner,
                 const std::shared_ptr<PinBase>& pin, std::initializer_list<Iface> ifaces);

} // namespace vitreo::winapi::dshow
