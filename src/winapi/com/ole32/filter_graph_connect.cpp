// SPDX-License-Identifier: LGPL-2.1-only
/// @file filter_graph_connect.cpp
/// @brief Intelligent Connect: IGraphBuilder::RenderFile / Render / Connect /
/// AddSourceFilter and IFilterGraph::ConnectDirect / Disconnect over the
/// filters already in the graph, the File Source (Async) and the DirectSound
/// Renderer; the graph reference clock; filter order for state changes.
///
/// Measured on Windows 11 x64 with Planet Coaster's WebM splitter, VP8 and
/// Vorbis decoders and a custom renderer in the graph
/// (tools/oracle_probe/dshow_renderfile_probe64*.expected):
///  * RenderFile adds a File Source (Async) named with the path as given,
///    output pin "Output" of type MEDIATYPE_Stream / GUID_NULL, and renders
///    it by offering the pin to the filters already in the graph, most
///    recently added first; each newly connected filter's output pins are
///    rendered the same way; a branch whose outputs cannot be rendered is
///    disconnected again (the unrendered stream's decoder stays unconnected).
///  * An audio stream nobody in the graph takes goes to a DirectSound
///    Renderer; without one the result is VFW_S_AUDIO_NOT_RENDERED.
///  * Nothing rendered: VFW_E_UNSUPPORTED_STREAM, and the source filter is
///    removed again (a WebM file in an empty graph fails this way).
///  * The graph clock is chosen at the first Pause and set on every filter;
///    Run passes tStart = clock time + about 10 ms; final Release disconnects
///    the pins and sets the sync source back to NULL.

#include "filter_graph_internal.hpp"
#include "winapi/com/ole32/dshow_host.hpp"
#include "capsule/vfs/vfs.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace vitreo::winapi {

namespace {

constexpr u32 VFW_E_UNSUPPORTED_STREAM = 0x80040265u;
constexpr u32 VFW_S_PARTIAL_RENDER = 0x00040242u;
constexpr u32 VFW_S_VIDEO_NOT_RENDERED = 0x00040257u;
constexpr u32 VFW_S_AUDIO_NOT_RENDERED = 0x00040258u;
constexpr u32 VFW_E_NOT_IN_GRAPH = 0x8004025Fu;
constexpr u32 VFW_E_NOT_STOPPED = 0x80040224u;
constexpr u32 VFW_E_CANNOT_CONNECT = 0x80040217u;
constexpr Guid IID_IREFERENCECLOCK_{0x56A86897u, 0x0AD4u, 0x11CEu,
                                    {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr Guid IID_IBASICAUDIO_{0x56A868B3u, 0x0AD4u, 0x11CEu,
                                {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr Guid MEDIATYPE_AUDIO_{0x73647561u, 0x0000u, 0x0010u,
                                {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

enum class StreamKind : u32 { Other = 0, Video = 1, Audio = 2 };

[[nodiscard]] std::u16string read_u16_string(Marshaler& m, GuestAddr ptr) {
    std::u16string text;
    if (ptr == 0) return text;
    for (GuestAddr at = ptr; text.size() < 0x8000u; at += 2) {
        const u16 unit = m.memory().read_u16(at).value_or(0);
        if (unit == 0) break;
        text.push_back(static_cast<char16_t>(unit));
    }
    return text;
}

using dshow::Iface;

/// Release an AM_MEDIA_TYPE* from IEnumMediaTypes::Next.
void free_media_type(ComCoreManager& mgr, Marshaler& m, GuestAddr mt) {
    if (mt == 0) return;
    const GuestAddr pb = static_cast<GuestAddr>(m.read_ptr(mt + (m.cpu_is_64bit() ? 80u : 68u)).value_or(0));
    if (pb != 0) mgr.co_task_mem_free_guest(pb);
    mgr.co_task_mem_free_guest(mt);
}

} // namespace

dshow::DShowHost& ComCoreManager::dshow_host() {
    if (m_dshow == nullptr) m_dshow = std::make_shared<dshow::DShowHost>(*this);
    return *m_dshow;
}

void ComCoreManager::filter_graph_post_event(GuestAddr graph_iface, u32 code, u64 p1, u64 p2) {
    FilterGraphState* st = find_filter_graph_by_iface(graph_iface);
    if (st == nullptr) return;
    std::lock_guard<std::mutex> lock(m_filter_graph_clock_mutex);
    report_filter_graph_event_locked(*st, code, p1, p2);
}

bool ComCoreManager::filter_graph_iunknown(Marshaler& m, GuestAddr pointer, u32 slot,
                                           std::span<const u64> args, u32& result) {
    const auto binding = filter_graph_binding(pointer);
    if (!binding.has_value()) return false;
    FilterGraphState* st = find_filter_graph_by_iface(pointer);
    if (st == nullptr) return false;
    const auto kind = binding->kind;
    if (kind == FilterGraphInterfaceKind::OutputPin || kind == FilterGraphInterfaceKind::MediaSample) {
        return false;  // these count on their own; the guest path handles them
    }
    if (slot == 1) {
        result = ++st->ref_count;
        return true;
    }
    if (slot == 2) {
        result = release_filter_graph_ref(m, binding->filter_graph_id);
        return true;
    }
    if (slot == 0) {
        const GuestAddr riid = args.size() > 0 ? static_cast<GuestAddr>(args[0]) : 0;
        const GuestAddr ppv = args.size() > 1 ? static_cast<GuestAddr>(args[1]) : 0;
        if (riid == 0 || ppv == 0) {
            result = com::E_INVALIDARG;
            return true;
        }
        (void)m.write_ptr(ppv, 0);
        auto iid = m.read_struct<Guid>(riid);
        const GuestAddr out = iid.is_ok() ? resolve_filter_graph_iface(*st, iid.value()) : 0;
        if (out == 0) {
            result = E_NOINTERFACE;
            return true;
        }
        ++st->ref_count;
        (void)m.write_ptr(ppv, out);
        result = com::S_OK;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- helpers

namespace {

struct PinFacts {
    GuestAddr filter{0};
    bool output{false};
    bool connected{false};
};

PinFacts pin_facts(ComCoreManager& mgr, Marshaler& m, GuestAddr pin) {
    dshow::DShowHost& host = mgr.dshow_host();
    PinFacts f;
    const GuestAddr info = host.scratch(288);
    if (info == 0) return f;
    if (host.call(m, pin, Iface::Pin, 8, {info}) == com::S_OK) {  // QueryPinInfo
        f.filter = static_cast<GuestAddr>(m.read_ptr(info).value_or(0));
        f.output = m.read_u32(info + (m.cpu_is_64bit() ? 8u : 4u)).value_or(0) == 1;
        if (f.filter != 0) (void)host.release(m, f.filter);
    }
    (void)m.write_ptr(info, 0);
    if (host.call(m, pin, Iface::Pin, 6, {info}) == com::S_OK) {  // ConnectedTo
        const GuestAddr peer = static_cast<GuestAddr>(m.read_ptr(info).value_or(0));
        f.connected = peer != 0;
        if (peer != 0) (void)host.release(m, peer);
    }
    host.free_scratch(info);
    return f;
}

std::vector<GuestAddr> filter_pins(ComCoreManager& mgr, Marshaler& m, GuestAddr filter) {
    dshow::DShowHost& host = mgr.dshow_host();
    std::vector<GuestAddr> pins;
    const GuestAddr out = host.scratch(32);
    if (out == 0) return pins;
    (void)m.write_ptr(out, 0);
    if (host.call(m, filter, Iface::BaseFilter, 10, {out}) == com::S_OK) {  // EnumPins
        const GuestAddr e = static_cast<GuestAddr>(m.read_ptr(out).value_or(0));
        for (u32 n = 0; e != 0 && n < 64; ++n) {
            (void)m.write_ptr(out + 8, 0);
            if (host.call(m, e, Iface::EnumPins, 3, {1, out + 8, out + 16}) != com::S_OK) break;
            const GuestAddr pin = static_cast<GuestAddr>(m.read_ptr(out + 8).value_or(0));
            if (pin == 0) break;
            pins.push_back(pin);
        }
        if (e != 0) (void)host.release(m, e);
    }
    host.free_scratch(out);
    return pins;
}

void release_all(ComCoreManager& mgr, Marshaler& m, const std::vector<GuestAddr>& objects) {
    for (const GuestAddr o : objects) (void)mgr.dshow_host().release(m, o);
}

/// The major type of a pin's first enumerated media type.
StreamKind pin_kind(ComCoreManager& mgr, Marshaler& m, GuestAddr pin) {
    dshow::DShowHost& host = mgr.dshow_host();
    const GuestAddr out = host.scratch(32);
    StreamKind kind = StreamKind::Other;
    if (out == 0) return kind;
    (void)m.write_ptr(out, 0);
    if (host.call(m, pin, Iface::Pin, 12, {out}) == com::S_OK) {
        const GuestAddr e = static_cast<GuestAddr>(m.read_ptr(out).value_or(0));
        (void)m.write_ptr(out + 8, 0);
        if (e != 0 && host.call(m, e, Iface::EnumMediaTypes, 3, {1, out + 8, out + 16}) == com::S_OK) {
            const GuestAddr mt = static_cast<GuestAddr>(m.read_ptr(out + 8).value_or(0));
            if (mt != 0) {
                auto major = m.read_struct<Guid>(mt);
                if (major.is_ok()) {
                    if (guid_equal(major.value(), MEDIATYPE_VIDEO)) kind = StreamKind::Video;
                    if (guid_equal(major.value(), MEDIATYPE_AUDIO_)) kind = StreamKind::Audio;
                }
                free_media_type(mgr, m, mt);
            }
        }
        if (e != 0) (void)host.release(m, e);
    }
    host.free_scratch(out);
    return kind;
}

bool in_graph(const std::vector<GuestAddr>& filters, GuestAddr filter) {
    return std::find(filters.begin(), filters.end(), filter) != filters.end();
}

} // namespace

void ComCoreManager::remove_filter_internal(Marshaler& m, FilterGraphState& st, GuestAddr filter) {
    for (auto it = st.guest_filters.rbegin(); it != st.guest_filters.rend(); ++it) {
        if (it->filter != filter) continue;
        FilterGraphState::GuestFilter one = *it;
        st.guest_filters.erase(std::next(it).base());
        const u64 join[] = {filter, 0, 0};
        (void)guest_vtable_call(m, filter, 13, join, 3);
        (void)dshow_host().release(m, filter);
        if (one.name_ptr != 0) co_task_mem_free_guest(one.name_ptr);
        ++st.filter_list_version;
        return;
    }
}

bool ComCoreManager::render_output_pin(Marshaler& m, FilterGraphState& st, GuestAddr pin,
                                       u32 depth, std::vector<u32>& failures,
                                       std::vector<GuestAddr>& added) {
    dshow::DShowHost& host = dshow_host();
    if (depth > 16) return false;
    const PinFacts facts = pin_facts(*this, m, pin);
    if (facts.connected) return true;
    const auto try_filter = [&](GuestAddr candidate) -> bool {
        const std::vector<GuestAddr> pins = filter_pins(*this, m, candidate);
        bool rendered = false;
        for (const GuestAddr input : pins) {
            if (rendered) break;
            const PinFacts in = pin_facts(*this, m, input);
            if (in.output || in.connected) continue;
            if (host.call(m, pin, Iface::Pin, 3, {input, 0}) != com::S_OK) continue;  // Connect
            // Render the newly connected filter's free outputs (pins may
            // appear only now, as a splitter's do).
            std::vector<u32> branch_failures;
            const std::vector<GuestAddr> outs = filter_pins(*this, m, candidate);
            bool any_output = false;
            bool any_rendered = false;
            for (const GuestAddr out : outs) {
                const PinFacts of = pin_facts(*this, m, out);
                if (!of.output || of.connected) continue;
                any_output = true;
                if (render_output_pin(m, st, out, depth + 1, branch_failures, added)) {
                    any_rendered = true;
                }
            }
            release_all(*this, m, outs);
            if (!any_output || any_rendered) {
                failures.insert(failures.end(), branch_failures.begin(), branch_failures.end());
                rendered = true;
                break;
            }
            // Nothing downstream rendered: undo this connection.
            (void)host.call(m, pin, Iface::Pin, 5, {});
            (void)host.call(m, input, Iface::Pin, 5, {});
        }
        release_all(*this, m, pins);
        return rendered;
    };
    for (const GuestAddr candidate : guest_filter_list(st)) {
        if (candidate == facts.filter) continue;
        if (try_filter(candidate)) return true;
    }
    // No filter in the graph takes the stream: Windows would now try the
    // registered filters. The ones Vitreo provides: the DirectSound
    // Renderer for audio.
    const StreamKind kind = pin_kind(*this, m, pin);
    if (kind == StreamKind::Audio) {
        u32 create_hr = 0;
        const GuestAddr renderer = dshow::create_audio_renderer(host, m, &create_hr);
        if (renderer != 0) {
            const std::u16string name = u"Default DirectSound Device";
            if (static_cast<i32>(add_filter_internal(m, st, renderer, &name)) >= 0) {
                (void)host.release(m, renderer);  // the graph holds it now
                if (try_filter(renderer)) {
                    added.push_back(renderer);
                    return true;
                }
                remove_filter_internal(m, st, renderer);
            } else {
                (void)host.release(m, renderer);
            }
        }
    } else if (kind == StreamKind::Video) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED quartz!IGraphBuilder::Render: no stock video "
                         "renderer/decoder for a stream the graph's filters do not take\n");
        }
    }
    failures.push_back(static_cast<u32>(kind));
    return false;
}

u32 ComCoreManager::intelligent_render_file(Marshaler& m, FilterGraphState& st,
                                            const std::u16string& path) {
    dshow::DShowHost& host = dshow_host();
    const GuestAddr source = dshow::create_async_reader(host, m);
    if (source == 0) return E_OUTOFMEMORY;
    const u32 load = dshow::async_reader_load(host, m, source, path);
    if (load != com::S_OK) {
        (void)host.release(m, source);
        return VFW_E_NOT_FOUND;
    }
    if (static_cast<i32>(add_filter_internal(m, st, source, &path)) < 0) {
        (void)host.release(m, source);
        return E_FAIL;
    }
    (void)host.release(m, source);  // the graph's reference keeps it
    std::vector<u32> failures;
    std::vector<GuestAddr> added;
    bool rendered = false;
    const std::vector<GuestAddr> pins = filter_pins(*this, m, source);
    for (const GuestAddr pin : pins) {
        if (render_output_pin(m, st, pin, 0, failures, added)) rendered = true;
    }
    release_all(*this, m, pins);
    if (!rendered) {
        remove_filter_internal(m, st, source);
        return VFW_E_UNSUPPORTED_STREAM;
    }
    st.intelligent = true;
    log_graph_shape(m, st, "RenderFile");
    if (failures.empty()) return com::S_OK;
    const bool all_audio = std::all_of(failures.begin(), failures.end(), [](u32 k) {
        return k == static_cast<u32>(StreamKind::Audio);
    });
    const bool all_video = std::all_of(failures.begin(), failures.end(), [](u32 k) {
        return k == static_cast<u32>(StreamKind::Video);
    });
    if (all_audio) return VFW_S_AUDIO_NOT_RENDERED;
    if (all_video) return VFW_S_VIDEO_NOT_RENDERED;
    return VFW_S_PARTIAL_RENDER;
}

std::vector<GuestAddr> ComCoreManager::filters_downstream_first(Marshaler& m,
                                                                const FilterGraphState& st) {
    // Renderers before the filters that feed them (Learn: state changes go
    // from the renderers upstream). Filters with no connected output first.
    const std::vector<GuestAddr> all = guest_filter_list(st);
    std::unordered_map<GuestAddr, std::vector<GuestAddr>> downstream;
    for (const GuestAddr f : all) {
        const std::vector<GuestAddr> pins = filter_pins(*this, m, f);
        for (const GuestAddr p : pins) {
            const PinFacts facts = pin_facts(*this, m, p);
            if (!facts.output || !facts.connected) continue;
            const GuestAddr peer_slot = dshow_host().scratch(16);
            (void)m.write_ptr(peer_slot, 0);
            if (dshow_host().call(m, p, Iface::Pin, 6, {peer_slot}) == com::S_OK) {
                const GuestAddr peer = static_cast<GuestAddr>(m.read_ptr(peer_slot).value_or(0));
                const PinFacts pf = pin_facts(*this, m, peer);
                if (pf.filter != 0) downstream[f].push_back(pf.filter);
                if (peer != 0) (void)dshow_host().release(m, peer);
            }
            dshow_host().free_scratch(peer_slot);
        }
        release_all(*this, m, pins);
    }
    std::vector<GuestAddr> order;
    std::vector<GuestAddr> left = all;
    while (!left.empty()) {
        bool progressed = false;
        for (auto it = left.begin(); it != left.end();) {
            const auto& next = downstream[*it];
            const bool ready = std::all_of(next.begin(), next.end(), [&](GuestAddr d) {
                return std::find(order.begin(), order.end(), d) != order.end() ||
                       std::find(left.begin(), left.end(), d) == left.end();
            });
            if (ready) {
                order.push_back(*it);
                it = left.erase(it);
                progressed = true;
            } else {
                ++it;
            }
        }
        if (!progressed) {  // a cycle cannot be connected; keep what is left
            order.insert(order.end(), left.begin(), left.end());
            break;
        }
    }
    return order;
}

u32 ComCoreManager::ensure_graph_clock(Marshaler& m, FilterGraphState& st) {
    if (st.clock != 0) return com::S_OK;
    dshow::DShowHost& host = dshow_host();
    // Learn "Reference Clocks": the Filter Graph Manager prefers a clock
    // exposed by a renderer, then any filter, then the system clock.
    GuestAddr clock = 0;
    for (const GuestAddr f : filters_downstream_first(m, st)) {
        clock = host.query(m, f, IID_IREFERENCECLOCK_);
        if (clock != 0) break;
    }
    if (clock == 0) clock = dshow::create_system_clock(host, m);
    if (clock == 0) return E_OUTOFMEMORY;
    st.clock = clock;
    for (const GuestAddr f : guest_filter_list(st)) {
        (void)host.call(m, f, Iface::BaseFilter, 8, {clock});  // SetSyncSource
    }
    return com::S_OK;
}

void ComCoreManager::disconnect_all_pins(Marshaler& m, FilterGraphState& st) {
    dshow::DShowHost& host = dshow_host();
    for (const GuestAddr f : guest_filter_list(st)) {
        const std::vector<GuestAddr> pins = filter_pins(*this, m, f);
        for (const GuestAddr p : pins) (void)host.call(m, p, Iface::Pin, 5, {});
        release_all(*this, m, pins);
    }
}

// ---------------------------------------------------------------- methods

u32 ComCoreManager::filter_graph_add_source_filter(Marshaler& m) {
    // AddSourceFilter(LPCWSTR lpcwstrFileName, LPCWSTR lpcwstrFilterName, IBaseFilter** ppFilter)
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    const GuestAddr file = m.arg_ptr(1);
    const GuestAddr name_ptr = m.arg_ptr(2);
    const GuestAddr out = m.arg_ptr(3);
    if (file == 0 || out == 0) return E_POINTER;
    dshow::DShowHost& host = dshow_host();
    const std::u16string path = read_u16_string(m, file);
    const GuestAddr source = dshow::create_async_reader(host, m);
    if (source == 0) return E_OUTOFMEMORY;
    if (dshow::async_reader_load(host, m, source, path) != com::S_OK) {
        (void)host.release(m, source);
        return VFW_E_NOT_FOUND;  // measured; *ppFilter untouched
    }
    // Measured: the filter is named with the path as given unless named.
    const std::u16string name = name_ptr != 0 ? read_u16_string(m, name_ptr) : path;
    const u32 add = add_filter_internal(m, *st, source, &name);
    if (static_cast<i32>(add) < 0) {
        (void)host.release(m, source);
        return add;
    }
    (void)m.write_ptr(out, source);  // the caller's reference
    return com::S_OK;
}

u32 ComCoreManager::filter_graph_connect_direct(Marshaler& m) {
    // ConnectDirect(IPin* ppinOut, IPin* ppinIn, const AM_MEDIA_TYPE* pmt)
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    const GuestAddr out = m.arg_ptr(1);
    const GuestAddr in = m.arg_ptr(2);
    if (out == 0 || in == 0) return E_POINTER;
    // The pins refuse while their filters are active; the graph does not
    // check its own state (measured for Disconnect).
    const std::vector<GuestAddr> members = guest_filter_list(*st);
    if (!in_graph(members, pin_facts(*this, m, out).filter) ||
        !in_graph(members, pin_facts(*this, m, in).filter)) {
        return VFW_E_NOT_IN_GRAPH;
    }
    const u32 hr = dshow_host().call(m, out, Iface::Pin, 3, {in, m.arg_ptr(3)});
    std::fprintf(stderr, "[quartz] IFilterGraph::ConnectDirect -> 0x%x\n", hr);
    if (hr == com::S_OK) {
        st->intelligent = true;
        log_graph_shape(m, *st, "ConnectDirect");
    }
    return hr;
}

u32 ComCoreManager::filter_graph_disconnect(Marshaler& m) {
    // Disconnect(IPin* ppin): this pin only; callers disconnect both ends.
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    const GuestAddr pin = m.arg_ptr(1);
    if (pin == 0) return E_POINTER;
    // Measured (dshow_seeking_probe): the graph does not check its own
    // state; the pin refuses if its filter is active (CBasePin::Disconnect).
    const u32 hr = dshow_host().call(m, pin, Iface::Pin, 5, {});
    std::fprintf(stderr, "[quartz] IFilterGraph::Disconnect pin 0x%llx -> 0x%x\n",
                 static_cast<unsigned long long>(pin), hr);
    return hr;
}

u32 ComCoreManager::filter_graph_connect(Marshaler& m) {
    // IGraphBuilder::Connect(IPin* ppinOut, IPin* ppinIn): directly, else
    // through one filter already in the graph.
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    const GuestAddr out = m.arg_ptr(1);
    const GuestAddr in = m.arg_ptr(2);
    if (out == 0 || in == 0) return E_POINTER;
    if (st->filter_state != 0) return VFW_E_NOT_STOPPED;
    dshow::DShowHost& host = dshow_host();
    if (host.call(m, out, Iface::Pin, 3, {in, 0}) == com::S_OK) {
        st->intelligent = true;
        log_graph_shape(m, *st, "Connect");
        return com::S_OK;
    }
    const GuestAddr own = pin_facts(*this, m, out).filter;
    const GuestAddr target = pin_facts(*this, m, in).filter;
    for (const GuestAddr f : guest_filter_list(*st)) {
        if (f == own || f == target) continue;
        const std::vector<GuestAddr> pins = filter_pins(*this, m, f);
        bool done = false;
        for (const GuestAddr p : pins) {
            const PinFacts pf = pin_facts(*this, m, p);
            if (pf.output || pf.connected) continue;
            if (host.call(m, out, Iface::Pin, 3, {p, 0}) != com::S_OK) continue;
            for (const GuestAddr q : pins) {
                const PinFacts qf = pin_facts(*this, m, q);
                if (!qf.output || qf.connected) continue;
                if (host.call(m, q, Iface::Pin, 3, {in, 0}) == com::S_OK) {
                    done = true;
                    break;
                }
            }
            if (done) break;
            (void)host.call(m, out, Iface::Pin, 5, {});
            (void)host.call(m, p, Iface::Pin, 5, {});
        }
        release_all(*this, m, pins);
        if (done) {
            st->intelligent = true;
            log_graph_shape(m, *st, "Connect (through a filter)");
            return com::S_OK;
        }
    }
    std::fprintf(stderr, "[quartz] IGraphBuilder::Connect -> VFW_E_CANNOT_CONNECT\n");
    return VFW_E_CANNOT_CONNECT;
}

u32 ComCoreManager::filter_graph_render(Marshaler& m) {
    // IGraphBuilder::Render(IPin* ppinOut)
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    const GuestAddr pin = m.arg_ptr(1);
    if (pin == 0) return E_POINTER;
    if (st->filter_state != 0) return VFW_E_NOT_STOPPED;
    std::vector<u32> failures;
    std::vector<GuestAddr> added;
    if (!render_output_pin(m, *st, pin, 0, failures, added)) {
        std::fprintf(stderr, "[quartz] IGraphBuilder::Render -> VFW_E_CANNOT_RENDER\n");
        return VFW_E_CANNOT_RENDER;
    }
    st->intelligent = true;
    log_graph_shape(m, *st, "Render");
    return failures.empty() ? com::S_OK : VFW_S_PARTIAL_RENDER;
}

u32 ComCoreManager::filter_graph_set_default_sync_source(Marshaler& m) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    return ensure_graph_clock(m, *st);
}

} // namespace vitreo::winapi

namespace vitreo::winapi {

// ---------------------------------------------------------------- state

i64 ComCoreManager::graph_run_start(Marshaler& m, GuestAddr clock) {
    if (clock == 0) return 0;
    dshow::DShowHost& host = dshow_host();
    const GuestAddr t = host.scratch(16);
    if (t == 0) return 0;
    (void)m.write_u64(t, 0);
    (void)host.call(m, clock, Iface::ReferenceClock, 3, {t});  // GetTime
    const i64 now = static_cast<i64>(m.memory().read_u64(t).value_or(0));
    host.free_scratch(t);
    // Measured: tStart lands 5-10 ms after the clock time Run reads.
    return now + 100'000;
}

u32 ComCoreManager::intelligent_pause(Marshaler& m, FilterGraphState& st, bool from_running) {
    std::fprintf(stderr, "[quartz] IMediaControl::Pause (from %s)\n", from_running ? "running" : "stopped");
    if (from_running) {
        // The stream position freezes where the run reached (state is
        // already Paused for the caller; compute as running).
        st.filter_state = 2;
        st.seek_position = graph_position(m, st);
        st.filter_state = 1;
    }
    const std::vector<GuestAddr> filters = filters_downstream_first(m, st);
    if (!from_running) {
        // Measured: SetSyncSource(clock) reaches every filter before the
        // first Pause.
        const u32 clock_hr = ensure_graph_clock(m, st);
        if (static_cast<i32>(clock_hr) < 0) return clock_hr;
    }
    const u32 hr = call_guest_filters_state(m, filters, 5);
    if (static_cast<i32>(hr) < 0) {
        (void)call_guest_filters_state(m, filters, 4);
        st.filter_state = 0;
        return hr;
    }
    // Measured: Windows also queues EC_CLOCK_CHANGED at the first Pause and
    // EC_PAUSED when the transition completes.
    VITREO_QUARTZ_UNIMPLEMENTED_ONCE(
        "IMediaEvent: EC_CLOCK_CHANGED / EC_PAUSED are not queued for graphs built "
        "from their filters");
    if (hr == com::S_FALSE &&
        query_guest_filters_state(m, filters, 0) == VFW_S_STATE_INTERMEDIATE) {
        return com::S_FALSE;
    }
    return com::S_OK;
}

u32 ComCoreManager::intelligent_run(Marshaler& m, FilterGraphState& st) {
    bool cueing = false;
    if (st.filter_state == 0) {
        st.filter_state = 1;
        const u32 pause_hr = intelligent_pause(m, st, false);
        if (static_cast<i32>(pause_hr) < 0) return pause_hr;
        cueing = pause_hr == com::S_FALSE;
    } else {
        cueing = query_guest_filters_state(m, guest_filter_list(st), 0) == VFW_S_STATE_INTERMEDIATE;
    }
    const std::vector<GuestAddr> filters = filters_downstream_first(m, st);
    // Measured: the renderers are counted at Run (IAMFilterMiscFlags).
    st.renderer_count = count_guest_renderers(m, filters);
    st.renderer_completions = 0;
    st.host_audio_renderer_pending = false;
    ensure_filter_graph_event(st);
    if (cueing) {
        // Measured: Run returns S_FALSE while the renderers cue; their Run
        // arrives later from another thread (the run worker).
        if (st.stream == nullptr) {
            auto control = std::make_shared<FilterGraphStream>();
            control->filter_graph_id = find_filter_graph_id_by_iface(st.graph_builder_iface);
            std::lock_guard<std::mutex> lock(m_filter_graph_clock_mutex);
            st.stream = control;
        }
        FilterGraphStream* s = st.stream.get();
        {
            std::lock_guard<std::mutex> lock(m_filter_graph_map_mutex);
            m_filter_streams[s->filter_graph_id] = st.stream;
        }
        bool launch = false;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            s->filters = filters;
            s->clock = st.clock;
            s->exit = false;
            s->run_pending = true;
            if (!s->run_worker_alive) {
                s->run_worker_alive = true;
                launch = true;
            }
        }
        bool started = !launch;
        if (launch && m_thunks != nullptr && m_audio_activation_thread) {
            auto entry = m_thunks->get_thunk_address("ole32.dll", "Vitreo_FilterGraph_RunWorker", true);
            started = entry.is_ok() && m_audio_activation_thread(entry.value(), s->filter_graph_id);
        }
        if (!started) {
            std::lock_guard<std::mutex> lock(s->mutex);
            s->run_worker_alive = false;
            s->run_pending = false;
            VITREO_QUARTZ_UNIMPLEMENTED_ONCE("IMediaControl::Run: no guest thread for the deferred Run");
            return E_FAIL;
        }
        st.filter_state = 2;
        st.seek_run_start = graph_run_start(m, st.clock);
        st.seek_completed = false;
        std::fprintf(stderr, "[quartz] IMediaControl::Run -> S_FALSE (renderers cueing; Run follows)\n");
        return com::S_FALSE;
    }
    const i64 start = graph_run_start(m, st.clock);
    st.seek_run_start = start;
    st.seek_completed = false;
    st.seek_stop_cache = -1;
    const u32 hr = call_guest_filters_state(m, filters, 6, start);
    st.filter_state = 2;
    std::fprintf(stderr, "[quartz] IMediaControl::Run -> 0x%x renderers=%u position=%lld\n", hr,
                 st.renderer_count, static_cast<long long>(st.seek_position));
    return hr;
}

u32 ComCoreManager::intelligent_stop(Marshaler& m, FilterGraphState& st) {
    if (st.filter_state == 0) return com::S_OK;  // measured: no filter calls
    if (FilterGraphStream* s = st.stream.get()) {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->run_pending = false;
        ++s->generation;
    }
    const std::vector<GuestAddr> filters = filters_downstream_first(m, st);
    const i64 position = graph_position(m, st);
    const bool completed = st.seek_completed && st.filter_state == 2;
    // Measured: Running -> Pause, then Stop; a filter's S_FALSE is the result.
    if (st.filter_state == 2) (void)call_guest_filters_state(m, filters, 5);
    const u32 hr = call_guest_filters_state(m, filters, 4);
    stop_guest_stream(st);  // waits for a pending run worker
    st.filter_state = 0;
    st.renderer_completions = 0;
    // Measured: Stop then repositions the renderers at the current position
    // (the stop position once the stream completed), Absolute | ReturnTime.
    {
        dshow::DShowHost& host = dshow_host();
        const GuestAddr t = host.scratch(16);
        const i64 target = completed ? graph_stop_position(m, st) : position;
        for (const GuestAddr s : graph_seekers(m, st)) {
            (void)m.write_u64(t, static_cast<u64>(target));
            (void)host.call(m, s, Iface::MediaSeeking, 14, {t, 0x9, 0, 0});
            (void)host.release(m, s);
        }
        host.free_scratch(t);
    }
    st.seek_position = position;
    st.seek_completed = false;
    std::fprintf(stderr, "[quartz] IMediaControl::Stop -> 0x%x position=%lld\n", hr,
                 static_cast<long long>(position));
    return hr;
}

} // namespace vitreo::winapi

namespace vitreo::winapi {

u32 ComCoreManager::dshow_co_create_instance(Marshaler& m, const Guid& clsid, const Guid& iid,
                                             GuestAddr outer, GuestAddr ppv, bool& claimed) {
    dshow::DShowHost& host = dshow_host();
    claimed = true;
    GuestAddr object = 0;
    u32 create_hr = com::S_OK;
    if (guid_equal(clsid, dshow::CLSID_ASYNC_READER)) {
        if (outer != 0) return com::CLASS_E_NOAGGREGATION;
        object = dshow::create_async_reader(host, m);
    } else if (guid_equal(clsid, dshow::CLSID_MEMORY_ALLOCATOR)) {
        if (outer != 0) return com::CLASS_E_NOAGGREGATION;
        object = dshow::create_memory_allocator(host, m);
    } else if (guid_equal(clsid, dshow::CLSID_SYSTEM_CLOCK)) {
        if (outer != 0) return com::CLASS_E_NOAGGREGATION;
        object = dshow::create_system_clock(host, m);
    } else if (guid_equal(clsid, dshow::CLSID_DSOUND_RENDER)) {
        if (outer != 0) return com::CLASS_E_NOAGGREGATION;
        object = dshow::create_audio_renderer(host, m, &create_hr);
        if (object == 0) return create_hr;  // measured: VFW_E_NO_AUDIO_HARDWARE
    } else {
        claimed = false;
        return com::S_OK;
    }
    if (object == 0) return E_OUTOFMEMORY;
    const GuestAddr iface = host.query(m, object, iid);
    (void)host.release(m, object);
    if (iface == 0) return E_NOINTERFACE;
    (void)m.write_ptr(ppv, iface);
    return com::S_OK;
}

} // namespace vitreo::winapi
