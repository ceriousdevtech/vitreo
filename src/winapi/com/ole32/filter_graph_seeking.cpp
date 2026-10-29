// SPDX-License-Identifier: LGPL-2.1-only
/// @file filter_graph_seeking.cpp
/// @brief IMediaSeeking / IMediaPosition of a Filter Graph Manager whose
/// graph was built from its filters (Intelligent Connect), and the graph
/// shape milestone log.
///
/// Measured on Windows 11 x64 with Planet Coaster's WebM filters and a
/// renderer whose IMediaSeeking passes through to its upstream pin
/// (tools/oracle_probe/dshow_seeking_probe64*.expected; the renderer is used
/// with and without IAMFilterMiscFlags, with the same result):
///  * The FGM answers QueryPreferredFormat / GetTimeFormat /
///    IsUsingTimeFormat / ConvertTimeFormat(NULL, NULL) itself
///    (TIME_FORMAT_MEDIA_TIME) and asks the sink filters' IMediaSeeking for
///    GetCapabilities, IsFormatSupported, GetDuration, GetStopPosition,
///    GetAvailable, GetRate, GetPreroll and SetPositions.
///  * CheckCapabilities: the requested bits that are supported, S_OK when
///    all of them, S_FALSE when only some.
///  * GetCurrentPosition is the FGM's own stream time: the position the run
///    started from plus the clock time since tStart, the stop position once
///    the application has EC_COMPLETE, frozen while stopped.
///  * Stop passes SetPositions(current, AbsolutePositioning | ReturnTime,
///    NULL, NoPositioning) to the renderers; SetPositions from the
///    application reaches them with ReturnTime added and returns the time
///    the splitter chose (a key frame).
///  * IMediaPosition is the same in seconds; CanSeekForward/Backward OATRUE.

#include "filter_graph_internal.hpp"
#include "winapi/com/ole32/dshow_host.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace vitreo::winapi {

namespace {

using dshow::Iface;

namespace sdk {
#include "winapi/com/ole32/dshow_guids.inc"
}

constexpr u32 AM_SEEKING_PositioningBitsMask = 0x3;
constexpr u32 AM_SEEKING_AbsolutePositioning = 0x1;
constexpr u32 AM_SEEKING_RelativePositioning = 0x2;
constexpr u32 AM_SEEKING_IncrementalPositioning = 0x3;
constexpr u32 AM_SEEKING_ReturnTime = 0x8;
constexpr u32 AM_SEEKING_CanSeekForwards = 0x4;
constexpr u32 AM_SEEKING_CanSeekBackwards = 0x8;
constexpr i64 kUnits = 10'000'000;

[[nodiscard]] bool is_media_time(Marshaler& m, GuestAddr format) {
    if (format == 0) return true;
    auto g = m.read_struct<Guid>(format);
    return g.is_ok() && guid_equal(g.value(), sdk::TIME_FORMAT_MEDIA_TIME);
}

void log_seek_once(u32 slot, u32 hr) {
    static std::atomic<u32> seen{0};
    const u32 bit = 1u << (slot & 31u);
    if ((seen.fetch_or(bit) & bit) != 0) return;
    static constexpr const char* kNames[] = {
        "", "", "", "GetCapabilities", "CheckCapabilities", "IsFormatSupported",
        "QueryPreferredFormat", "GetTimeFormat", "IsUsingTimeFormat", "SetTimeFormat",
        "GetDuration", "GetStopPosition", "GetCurrentPosition", "ConvertTimeFormat",
        "SetPositions", "GetPositions", "GetAvailable", "SetRate", "GetRate", "GetPreroll"};
    std::fprintf(stderr, "[quartz] IMediaSeeking::%s (first call) -> 0x%x\n",
                 slot < std::size(kNames) ? kNames[slot] : "?", hr);
}

} // namespace

i64 ComCoreManager::graph_clock_now(Marshaler& m, GuestAddr clock) {
    if (clock == 0) return 0;
    dshow::DShowHost& host = dshow_host();
    const GuestAddr t = host.scratch(16);
    if (t == 0) return 0;
    (void)m.write_u64(t, 0);
    (void)host.call(m, clock, Iface::ReferenceClock, 3, {t});
    const i64 now = static_cast<i64>(m.memory().read_u64(t).value_or(0));
    host.free_scratch(t);
    return now;
}

std::vector<GuestAddr> ComCoreManager::graph_seekers(Marshaler& m, FilterGraphState& st) {
    // The sink filters (no connected output pin, a connected input pin) that
    // expose IMediaSeeking; the stock renderers pass it upstream.
    dshow::DShowHost& host = dshow_host();
    std::vector<GuestAddr> out;
    const GuestAddr slot = host.scratch(320);
    if (slot == 0) return out;
    for (const GuestAddr f : guest_filter_list(st)) {
        bool sink = true;
        bool fed = false;
        (void)m.write_ptr(slot, 0);
        if (host.call(m, f, Iface::BaseFilter, 10, {slot}) != com::S_OK) continue;
        const GuestAddr e = static_cast<GuestAddr>(m.read_ptr(slot).value_or(0));
        for (u32 n = 0; e != 0 && n < 64; ++n) {
            (void)m.write_ptr(slot + 8, 0);
            if (host.call(m, e, Iface::EnumPins, 3, {1, slot + 8, slot + 16}) != com::S_OK) break;
            const GuestAddr pin = static_cast<GuestAddr>(m.read_ptr(slot + 8).value_or(0));
            if (pin == 0) break;
            (void)m.write_u32(slot + 24, 0xFFFFFFFFu);
            (void)host.call(m, pin, Iface::Pin, 9, {slot + 24});
            const u32 dir = m.read_u32(slot + 24).value_or(0xFFFFFFFFu);
            (void)m.write_ptr(slot + 32, 0);
            const bool connected = host.call(m, pin, Iface::Pin, 6, {slot + 32}) == com::S_OK;
            const GuestAddr peer = static_cast<GuestAddr>(m.read_ptr(slot + 32).value_or(0));
            if (peer != 0) (void)host.release(m, peer);
            if (connected && dir == 1) sink = false;
            if (connected && dir == 0) fed = true;
            (void)host.release(m, pin);
        }
        if (e != 0) (void)host.release(m, e);
        if (!sink || !fed) continue;
        const GuestAddr seeking = host.query(m, f, sdk::IID_IMediaSeeking);
        if (seeking != 0) out.push_back(seeking);
    }
    host.free_scratch(slot);
    return out;
}

i64 ComCoreManager::graph_stop_position(Marshaler& m, FilterGraphState& st, u32* hr_out) {
    dshow::DShowHost& host = dshow_host();
    const std::vector<GuestAddr> seekers = graph_seekers(m, st);
    const GuestAddr t = host.scratch(16);
    i64 best = 0;
    u32 result = E_NOTIMPL;
    for (const GuestAddr s : seekers) {
        (void)m.write_u64(t, 0);
        const u32 hr = host.call(m, s, Iface::MediaSeeking, 11, {t});
        if (hr == com::S_OK) {
            best = std::max(best, static_cast<i64>(m.memory().read_u64(t).value_or(0)));
            result = com::S_OK;
        } else if (result != com::S_OK) {
            result = hr;
        }
        (void)host.release(m, s);
    }
    host.free_scratch(t);
    if (hr_out != nullptr) *hr_out = result;
    return best;
}

i64 ComCoreManager::graph_position(Marshaler& m, FilterGraphState& st) {
    if (st.filter_state != 2) return st.seek_position;
    if (st.seek_stop_cache < 0) st.seek_stop_cache = graph_stop_position(m, st);
    const i64 stop = st.seek_stop_cache;
    if (st.seek_completed) return stop;
    const i64 elapsed = std::max<i64>(0, graph_clock_now(m, st.clock) - st.seek_run_start);
    const i64 position = st.seek_position + elapsed;
    return stop > 0 ? std::min(position, stop) : position;
}

u32 ComCoreManager::graph_set_positions(Marshaler& m, FilterGraphState& st, GuestAddr current,
                                        u32 current_flags, GuestAddr stop, u32 stop_flags) {
    dshow::DShowHost& host = dshow_host();
    const std::vector<GuestAddr> seekers = graph_seekers(m, st);
    if (seekers.empty()) return E_NOTIMPL;
    const GuestAddr t = host.scratch(16);
    const u32 cur_mode = current_flags & AM_SEEKING_PositioningBitsMask;
    const u32 stop_mode = stop_flags & AM_SEEKING_PositioningBitsMask;
    i64 requested = current != 0 ? static_cast<i64>(m.memory().read_u64(current).value_or(0)) : 0;
    const i64 now_position = graph_position(m, st);
    if (cur_mode == AM_SEEKING_RelativePositioning || cur_mode == AM_SEEKING_IncrementalPositioning) {
        requested += now_position;
    }
    const bool running = st.filter_state == 2;
    const std::vector<GuestAddr> filters = filters_downstream_first(m, st);
    // Learn: a running graph is paused for the seek and run again.
    if (running && cur_mode != 0) (void)call_guest_filters_state(m, filters, 5);
    u32 result = com::S_OK;
    i64 returned = requested;
    for (const GuestAddr s : seekers) {
        (void)m.write_u64(t, static_cast<u64>(requested));
        (void)m.write_u64(t + 8, stop != 0 ? m.memory().read_u64(stop).value_or(0) : 0);
        const u32 flags = cur_mode != 0 ? AM_SEEKING_AbsolutePositioning | AM_SEEKING_ReturnTime |
                                              (current_flags & ~(AM_SEEKING_PositioningBitsMask | AM_SEEKING_ReturnTime))
                                        : 0;
        const u32 hr = host.call(m, s, Iface::MediaSeeking, 14,
                                 {cur_mode != 0 ? t : 0, flags, stop_mode != 0 ? t + 8 : 0, stop_flags});
        if (static_cast<i32>(hr) < 0) {
            if (static_cast<i32>(result) >= 0) result = hr;
        } else if (cur_mode != 0) {
            returned = static_cast<i64>(m.memory().read_u64(t).value_or(static_cast<u64>(requested)));
        }
        (void)host.release(m, s);
    }
    host.free_scratch(t);
    st.seek_stop_cache = -1;
    if (cur_mode != 0 && static_cast<i32>(result) >= 0) {
        st.seek_position = returned;
        st.seek_completed = false;
        st.renderer_completions = 0;
        if ((current_flags & AM_SEEKING_ReturnTime) != 0 && current != 0) {
            (void)m.write_u64(current, static_cast<u64>(returned));
        }
        if (running) {
            const i64 start = graph_run_start(m, st.clock);
            st.seek_run_start = start;
            (void)call_guest_filters_state(m, filters, 6, start);
        }
    }
    std::fprintf(stderr,
                 "[quartz] IMediaSeeking::SetPositions cur=%lld (mode %u) stop_mode=%u -> 0x%x "
                 "position=%lld\n",
                 static_cast<long long>(requested), cur_mode, stop_mode, result,
                 static_cast<long long>(st.seek_position));
    return result;
}

bool ComCoreManager::intelligent_seeking(Marshaler& m, u32 slot, u32& result) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return false;
    if (!st->intelligent && st->player == nullptr && !st->guest_filters.empty()) st->intelligent = true;
    if (!st->intelligent) return false;
    dshow::DShowHost& host = dshow_host();
    const std::vector<u64> a = dshow::to_logical(m, Iface::MediaSeeking, slot);
    const auto arg = [&](usize i) { return i < a.size() ? a[i] : 0; };
    const GuestAddr p0 = static_cast<GuestAddr>(arg(0));
    const auto forward_first = [&](std::span<const u64> args) -> u32 {
        const std::vector<GuestAddr> seekers = graph_seekers(m, *st);
        u32 hr = E_NOTIMPL;
        for (const GuestAddr s : seekers) {
            if (hr == E_NOTIMPL) hr = host.call(m, s, Iface::MediaSeeking, slot, args);
            (void)host.release(m, s);
        }
        return hr;
    };
    switch (slot) {
    case 3:    // GetCapabilities(DWORD*)
    case 4: {  // CheckCapabilities(DWORD*)
        if (p0 == 0) {
            result = E_POINTER;
            break;
        }
        const std::vector<GuestAddr> seekers = graph_seekers(m, *st);
        const GuestAddr t = host.scratch(16);
        u32 caps = 0xFFFFFFFFu;
        bool any = false;
        for (const GuestAddr s : seekers) {
            (void)m.write_u32(t, 0);
            if (host.call(m, s, Iface::MediaSeeking, 3, {t}) == com::S_OK) {
                caps &= m.read_u32(t).value_or(0);
                any = true;
            }
            (void)host.release(m, s);
        }
        host.free_scratch(t);
        if (!any) {
            result = E_NOTIMPL;
            break;
        }
        if (slot == 3) {
            (void)m.write_u32(p0, caps);
            result = com::S_OK;
        } else {
            const u32 wanted = m.read_u32(p0).value_or(0);
            const u32 have = wanted & caps;
            (void)m.write_u32(p0, have);
            result = have == wanted ? com::S_OK : (have != 0 ? com::S_FALSE : E_FAIL);
        }
        break;
    }
    case 5: {  // IsFormatSupported(const GUID*)
        if (p0 == 0) {
            result = E_POINTER;
            break;
        }
        const std::vector<GuestAddr> seekers = graph_seekers(m, *st);
        result = seekers.empty() ? E_NOTIMPL : com::S_OK;
        for (const GuestAddr s : seekers) {
            const u32 hr = host.call(m, s, Iface::MediaSeeking, 5, {p0});
            if (hr != com::S_OK && result == com::S_OK) result = hr;
            (void)host.release(m, s);
        }
        break;
    }
    case 6:    // QueryPreferredFormat(GUID*)
    case 7:    // GetTimeFormat(GUID*)
        if (p0 == 0) {
            result = E_POINTER;
            break;
        }
        (void)m.write_struct<Guid>(p0, sdk::TIME_FORMAT_MEDIA_TIME);
        result = com::S_OK;
        break;
    case 8:  // IsUsingTimeFormat(const GUID*)
        result = p0 == 0 ? E_POINTER : (is_media_time(m, p0) ? com::S_OK : com::S_FALSE);
        break;
    case 9:  // SetTimeFormat(const GUID*)
        if (p0 == 0) {
            result = E_POINTER;
        } else if (is_media_time(m, p0)) {
            result = com::S_OK;
        } else {
            VITREO_QUARTZ_UNIMPLEMENTED_ONCE(
                "IMediaSeeking::SetTimeFormat: formats other than TIME_FORMAT_MEDIA_TIME on a graph "
                "built from its filters");
            result = E_NOTIMPL;
        }
        break;
    case 10:    // GetDuration(LONGLONG*)
    case 11: {  // GetStopPosition(LONGLONG*)
        if (p0 == 0) {
            result = E_POINTER;
            break;
        }
        const std::vector<GuestAddr> seekers = graph_seekers(m, *st);
        const GuestAddr t = host.scratch(16);
        i64 best = 0;
        result = E_NOTIMPL;
        for (const GuestAddr s : seekers) {
            (void)m.write_u64(t, 0);
            const u32 hr = host.call(m, s, Iface::MediaSeeking, slot, {t});
            if (hr == com::S_OK) {
                best = std::max(best, static_cast<i64>(m.memory().read_u64(t).value_or(0)));
                result = com::S_OK;
            } else if (result != com::S_OK) {
                result = hr;
            }
            (void)host.release(m, s);
        }
        host.free_scratch(t);
        if (result == com::S_OK) (void)m.write_u64(p0, static_cast<u64>(best));
        break;
    }
    case 12:  // GetCurrentPosition(LONGLONG*)
        if (p0 == 0) {
            result = E_POINTER;
            break;
        }
        (void)m.write_u64(p0, static_cast<u64>(graph_position(m, *st)));
        result = com::S_OK;
        break;
    case 13: {  // ConvertTimeFormat(LONGLONG* target, const GUID* target_fmt, LONGLONG source, const GUID* source_fmt)
        if (p0 == 0) {
            result = E_POINTER;
            break;
        }
        if (is_media_time(m, static_cast<GuestAddr>(arg(1))) &&
            is_media_time(m, static_cast<GuestAddr>(arg(3)))) {
            (void)m.write_u64(p0, arg(2));
            result = com::S_OK;
        } else {
            result = forward_first(a);
        }
        break;
    }
    case 14:  // SetPositions(LONGLONG*, DWORD, LONGLONG*, DWORD)
        result = graph_set_positions(m, *st, p0, static_cast<u32>(arg(1)),
                                     static_cast<GuestAddr>(arg(2)), static_cast<u32>(arg(3)));
        break;
    case 15: {  // GetPositions(LONGLONG* current, LONGLONG* stop)
        const GuestAddr stop = static_cast<GuestAddr>(arg(1));
        u32 hr = com::S_OK;
        const i64 stop_value = graph_stop_position(m, *st, &hr);
        if (p0 != 0) (void)m.write_u64(p0, static_cast<u64>(graph_position(m, *st)));
        if (stop != 0 && hr == com::S_OK) (void)m.write_u64(stop, static_cast<u64>(stop_value));
        result = hr;
        break;
    }
    case 16:  // GetAvailable(LONGLONG*, LONGLONG*)
    case 18:  // GetRate(double*)
    case 19:  // GetPreroll(LONGLONG*)
        result = forward_first(a);
        break;
    case 17: {  // SetRate(double): every renderer
        const std::vector<GuestAddr> seekers = graph_seekers(m, *st);
        result = seekers.empty() ? E_NOTIMPL : com::S_OK;
        for (const GuestAddr s : seekers) {
            const u32 hr = host.call(m, s, Iface::MediaSeeking, 17, a);
            if (static_cast<i32>(hr) < 0 && static_cast<i32>(result) >= 0) result = hr;
            (void)host.release(m, s);
        }
        break;
    }
    default:
        result = E_NOTIMPL;
        break;
    }
    log_seek_once(slot, result);
    return true;
}

u32 ComCoreManager::intelligent_position(Marshaler& m, FilterGraphState& st, u32 op) {
    // IMediaPosition (control.h): REFTIME seconds over the same seeking.
    dshow::DShowHost& host = dshow_host();
    const GuestAddr out = m.arg_ptr(1);
    const auto write_seconds = [&](i64 units) {
        const double seconds = static_cast<double>(units) / static_cast<double>(kUnits);
        u64 bits = 0;
        std::memcpy(&bits, &seconds, sizeof(bits));
        (void)m.write_u64(out, bits);
    };
    const auto seconds_arg = [&] { return static_cast<i64>(m.arg_f64(1) * static_cast<double>(kUnits)); };
    switch (op) {
    case 0: {  // get_Duration
        if (out == 0) return E_POINTER;
        const std::vector<GuestAddr> seekers = graph_seekers(m, st);
        const GuestAddr t = host.scratch(16);
        i64 best = 0;
        u32 result = E_NOTIMPL;
        for (const GuestAddr s : seekers) {
            (void)m.write_u64(t, 0);
            if (host.call(m, s, Iface::MediaSeeking, 10, {t}) == com::S_OK) {
                best = std::max(best, static_cast<i64>(m.memory().read_u64(t).value_or(0)));
                result = com::S_OK;
            }
            (void)host.release(m, s);
        }
        host.free_scratch(t);
        if (result == com::S_OK) write_seconds(best);
        return result;
    }
    case 1:    // put_CurrentPosition(REFTIME)
    case 4: {  // put_StopTime(REFTIME)
        const GuestAddr t = host.scratch(16);
        (void)m.write_u64(t, static_cast<u64>(seconds_arg()));
        const u32 result = op == 1 ? graph_set_positions(m, st, t, AM_SEEKING_AbsolutePositioning, 0, 0)
                                   : graph_set_positions(m, st, 0, 0, t, AM_SEEKING_AbsolutePositioning);
        host.free_scratch(t);
        return result;
    }
    case 2:  // get_CurrentPosition
        if (out == 0) return E_POINTER;
        write_seconds(graph_position(m, st));
        return com::S_OK;
    case 3: {  // get_StopTime
        if (out == 0) return E_POINTER;
        u32 result = com::S_OK;
        const i64 stop = graph_stop_position(m, st, &result);
        if (result == com::S_OK) write_seconds(stop);
        return result;
    }
    case 5:  // get_PrerollTime
    case 8: {  // get_Rate
        if (out == 0) return E_POINTER;
        const std::vector<GuestAddr> seekers = graph_seekers(m, st);
        const GuestAddr t = host.scratch(16);
        u32 result = E_NOTIMPL;
        for (const GuestAddr s : seekers) {
            if (result == E_NOTIMPL) result = host.call(m, s, Iface::MediaSeeking, op == 5 ? 19u : 18u, {t});
            (void)host.release(m, s);
        }
        if (result == com::S_OK) {
            if (op == 5) {
                write_seconds(static_cast<i64>(m.memory().read_u64(t).value_or(0)));
            } else {
                (void)m.write_u64(out, m.memory().read_u64(t).value_or(0));
            }
        }
        host.free_scratch(t);
        return result;
    }
    case 6:  // put_PrerollTime: IMediaSeeking has no setter for it
        VITREO_QUARTZ_UNIMPLEMENTED_ONCE("IMediaPosition::put_PrerollTime on a graph built from its filters");
        return E_NOTIMPL;
    case 7: {  // put_Rate(double)
        const std::vector<GuestAddr> seekers = graph_seekers(m, st);
        u64 bits = 0;
        const double rate = m.arg_f64(1);
        std::memcpy(&bits, &rate, sizeof(bits));
        u32 result = seekers.empty() ? E_NOTIMPL : com::S_OK;
        for (const GuestAddr s : seekers) {
            const u32 hr = host.call(m, s, Iface::MediaSeeking, 17, {bits});
            if (static_cast<i32>(hr) < 0 && static_cast<i32>(result) >= 0) result = hr;
            (void)host.release(m, s);
        }
        return result;
    }
    case 9:     // CanSeekForward(LONG*)
    case 10: {  // CanSeekBackward(LONG*)
        if (out == 0) return E_POINTER;
        const std::vector<GuestAddr> seekers = graph_seekers(m, st);
        const GuestAddr t = host.scratch(16);
        u32 caps = seekers.empty() ? 0u : 0xFFFFFFFFu;
        for (const GuestAddr s : seekers) {
            (void)m.write_u32(t, 0);
            if (host.call(m, s, Iface::MediaSeeking, 3, {t}) == com::S_OK) {
                caps &= m.read_u32(t).value_or(0);
            } else {
                caps = 0;
            }
            (void)host.release(m, s);
        }
        host.free_scratch(t);
        const u32 bit = op == 9 ? AM_SEEKING_CanSeekForwards : AM_SEEKING_CanSeekBackwards;
        (void)m.write_u32(out, (caps & bit) != 0 ? static_cast<u32>(OATRUE) : 0u);
        return com::S_OK;
    }
    default:
        return E_NOTIMPL;
    }
}

void ComCoreManager::log_graph_shape(Marshaler& m, FilterGraphState& st, const char* reason) {
    dshow::DShowHost& host = dshow_host();
    const GuestAddr s = host.scratch(512);
    if (s == 0) return;
    const bool x64 = m.cpu_is_64bit();
    const auto pin_name = [&](GuestAddr pin, GuestAddr& owner) {
        std::string name;
        owner = 0;
        if (host.call(m, pin, Iface::Pin, 8, {s + 64}) != com::S_OK) return name;
        owner = static_cast<GuestAddr>(m.read_ptr(s + 64).value_or(0));
        for (u32 i = 0; i < 127; ++i) {
            const u16 c = m.memory().read_u16(s + 64 + (x64 ? 12u : 8u) + i * 2).value_or(0);
            if (c == 0) break;
            name.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        }
        if (owner != 0) (void)host.release(m, owner);
        return name;
    };
    const auto filter_name = [&](GuestAddr f) {
        for (const auto& entry : st.guest_filters) {
            if (entry.filter != f) continue;
            std::string name;
            for (const char16_t c : entry.name) name.push_back(c < 0x80 ? static_cast<char>(c) : '?');
            return name;
        }
        return std::string("?");
    };
    std::fprintf(stderr, "[quartz] graph shape (%s): %zu filters\n", reason, st.guest_filters.size());
    for (const GuestAddr f : guest_filter_list(st)) {
        std::fprintf(stderr, "[quartz]   filter 0x%llx '%s'\n", static_cast<unsigned long long>(f),
                     filter_name(f).c_str());
        (void)m.write_ptr(s, 0);
        if (host.call(m, f, Iface::BaseFilter, 10, {s}) != com::S_OK) continue;
        const GuestAddr e = static_cast<GuestAddr>(m.read_ptr(s).value_or(0));
        for (u32 n = 0; e != 0 && n < 32; ++n) {
            (void)m.write_ptr(s + 8, 0);
            if (host.call(m, e, Iface::EnumPins, 3, {1, s + 8, s + 16}) != com::S_OK) break;
            const GuestAddr pin = static_cast<GuestAddr>(m.read_ptr(s + 8).value_or(0));
            if (pin == 0) break;
            GuestAddr owner = 0;
            const std::string name = pin_name(pin, owner);
            (void)m.write_u32(s + 24, 9);
            (void)host.call(m, pin, Iface::Pin, 9, {s + 24});
            const bool output = m.read_u32(s + 24).value_or(0) == 1;
            (void)m.write_ptr(s + 32, 0);
            const GuestAddr peer = host.call(m, pin, Iface::Pin, 6, {s + 32}) == com::S_OK
                                       ? static_cast<GuestAddr>(m.read_ptr(s + 32).value_or(0))
                                       : 0;
            if (peer == 0) {
                std::fprintf(stderr, "[quartz]     %s pin '%s' unconnected\n", output ? "out" : "in",
                             name.c_str());
            } else {
                GuestAddr peer_owner = 0;
                const std::string peer_name = pin_name(peer, peer_owner);
                Guid major{}, sub{};
                if (host.call(m, pin, Iface::Pin, 7, {s + 300}) == com::S_OK) {
                    (void)m.memory().read_bytes(s + 300, reinterpret_cast<Byte*>(&major), 16);
                    (void)m.memory().read_bytes(s + 316, reinterpret_cast<Byte*>(&sub), 16);
                    const GuestAddr pb = static_cast<GuestAddr>(m.read_ptr(s + 300 + (x64 ? 80u : 68u)).value_or(0));
                    if (pb != 0) co_task_mem_free_guest(pb);
                }
                std::fprintf(stderr,
                             "[quartz]     %s pin '%s' -> '%s'.'%s' major=%08x sub=%08x\n",
                             output ? "out" : "in", name.c_str(), filter_name(peer_owner).c_str(),
                             peer_name.c_str(), major.Data1, sub.Data1);
                (void)host.release(m, peer);
            }
            (void)host.release(m, pin);
        }
        if (e != 0) (void)host.release(m, e);
    }
    host.free_scratch(s);
}

} // namespace vitreo::winapi
