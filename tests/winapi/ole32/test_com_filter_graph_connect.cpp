// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_filter_graph_connect.cpp
/// @brief Intelligent Connect and the host DirectShow objects (File Source
/// (Async), DirectSound Renderer, memory allocator, system clock), x86 and x64.
///
/// Expected values come from Windows 11 (MSVC builds on winoracle):
///   tests/oracle_data/dshow_renderfile_probe64*.expected  - graph shape
///   tests/oracle_data/dshow_asyncreader_probe{32,64}.expected
///   tests/oracle_data/dshow_allocator_probe{32,64}.expected

#include "com_test_common.hpp"
#include "com_test_audio_doubles.hpp"
#include "com_test_filter_graph_fixture.hpp"

#include <winapi/com/ole32/dshow_objects.hpp>

#include <functional>
#include <map>

namespace {

namespace ds = vitreo::winapi::dshow;
using ds::Iface;
using ds::MediaType;
using ds::guid_eq;

constexpr u32 kVfwSAudioNotRendered = 0x00040258u;
constexpr u32 kVfwEUnsupportedStream = 0x80040265u;
constexpr u32 kVfwEBadAlign = 0x8004020Eu;
constexpr u32 kVfwEWrongState = 0x80040227u;
constexpr u32 kVfwETimeout = 0x8004022Eu;
constexpr u32 kVfwENotCommitted = 0x80040211u;
constexpr u32 kVfwEAlreadyCommitted = 0x8004020Fu;
constexpr u32 kVfwEBuffersOutstanding = 0x80040210u;
constexpr u32 kVfwESizeNotSet = 0x80040212u;
constexpr u32 kVfwENotConnected = 0x80040209u;
constexpr u32 kHandleEof = 0x80070026u;
constexpr u32 kFileNotFound = 0x80070002u;
constexpr u32 kEUnexpected = 0x8000FFFFu;
constexpr u32 kEInvalidArgHr = 0x80070057u;
constexpr i64 kUnits = 10'000'000;

constexpr Guid kSubVp80{0x30385056u, 0x0000u, 0x0010u, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr Guid kSubVorbis{0x8D2FD10Bu, 0x5841u, 0x4A6Bu, {0x89, 0x05, 0x58, 0x8F, 0xEC, 0x1A, 0xDE, 0xD9}};
constexpr Guid kSubYv12{0x32315659u, 0x0000u, 0x0010u, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr Guid kFormatVideoInfo{0x05589F80u, 0xC356u, 0x11CEu, {0xBF, 0x01, 0x00, 0xAA, 0x00, 0x55, 0x59, 0x5A}};

MediaType video_type(const Guid& sub) {
    MediaType t;
    t.major = ds::sdk::MEDIATYPE_Video;
    t.sub = sub;
    t.format = kFormatVideoInfo;
    t.format_bytes.assign(88, 0);
    return t;
}
MediaType audio_vorbis() {
    MediaType t;
    t.major = ds::sdk::MEDIATYPE_Audio;
    t.sub = kSubVorbis;
    return t;
}
/// The Vorbis decoder's output, measured: IEEE float, 2 ch, 48000 Hz,
/// 32 bits, block 8, 384000 bytes/s, cbSize 0.
MediaType audio_float() {
    MediaType t;
    t.major = ds::sdk::MEDIATYPE_Audio;
    t.sub = ds::sdk::MEDIASUBTYPE_IEEE_FLOAT;
    t.format = ds::sdk::FORMAT_WaveFormatEx;
    t.fixed = true;
    t.sample_size = 8;
    t.format_bytes = {0x03, 0x00, 0x02, 0x00, 0x80, 0xBB, 0x00, 0x00, 0x00, 0xDC,
                      0x05, 0x00, 0x08, 0x00, 0x20, 0x00, 0x00, 0x00};
    return t;
}

} // namespace

/// Fake guest filters whose pins negotiate through the host bridge, like
/// the WebM splitter / VP8 / Vorbis decoders and Planet Coaster's renderer.
class FilterGraphConnectTest : public FilterGraphBitnessTest {
protected:
    static constexpr GuestAddr OBJS = DATA_BASE + 0x3000;  // 0x20 apart

    struct Pin {
        GuestAddr addr{0};
        int filter{0};
        bool output{false};
        std::u16string name;
        std::vector<MediaType> offers;
        std::function<bool(const MediaType&)> accepts;
        bool visible{true};
        GuestAddr seek{0};  // IMediaSeeking object (the splitter-side seeking a renderer passes to)
        bool has_seeking{false};
        GuestAddr peer{0};
        MediaType type;
    };
    struct Filter {
        GuestAddr addr{0};
        GuestAddr misc{0};
        GuestAddr seek{0};  // IMediaSeeking on the filter (a renderer's pass-through)
        bool has_seeking{false};
        std::u16string name;
        std::vector<int> pins;
        u32 state{0};
        GuestAddr clock{0};
        GuestAddr graph{0};
        bool renderer_flag{false};
        i32 refs{1};
        i64 run_start{0};
        std::function<void()> on_input_connected;
        /// The WebM decoders' Outpin::OnInpinDisconnect: when the input pin
        /// is disconnected, the filter calls IFilterGraph::Disconnect on its
        /// output pin's peer and on the output pin, asserting both succeed.
        bool disconnect_downstream_through_graph{false};
    };

    void SetUp() override {
        FilterGraphBitnessTest::SetUp();
        mgr_.set_guest_call([this](GuestAddr fn, const u64* args, usize count, bool is64) -> u64 {
            EXPECT_EQ(is64, x64_);
            return dispatch(fn, args, count);
        });
    }

    GuestAddr new_object() {
        const GuestAddr a = OBJS + next_object_++ * 0x20u;
        wptr(a, FILTER_VTBL);
        return a;
    }
    int add_fake_filter(std::u16string name, bool renderer_flag = false) {
        Filter f;
        f.addr = new_object();
        f.misc = new_object();
        f.seek = new_object();
        f.name = std::move(name);
        f.renderer_flag = renderer_flag;
        filters_.push_back(f);
        return static_cast<int>(filters_.size() - 1);
    }
    int add_fake_pin(int filter, bool output, std::u16string name) {
        Pin p;
        p.addr = new_object();
        p.seek = new_object();
        p.filter = filter;
        p.output = output;
        p.name = std::move(name);
        pins_.push_back(p);
        filters_[filter].pins.push_back(static_cast<int>(pins_.size() - 1));
        return static_cast<int>(pins_.size() - 1);
    }

    /// Splitter + VP8 decoder + Vorbis decoder + renderer, added in that order
    /// (indexes S, V, A, R), as Planet Coaster builds its graph.
    void build_game_filters(bool renderer_misc) {
        S = add_fake_filter(u"WebM Splitter");
        const int s_in = add_fake_pin(S, false, u"WebM");
        pins_[s_in].accepts = [](const MediaType& t) { return guid_eq(t.major, ds::sdk::MEDIATYPE_Stream); };
        const int t1 = add_fake_pin(S, true, u"Track1");
        const int t2 = add_fake_pin(S, true, u"Track2");
        pins_[t1].offers = {video_type(kSubVp80)};
        pins_[t2].offers = {audio_vorbis()};
        pins_[t1].visible = pins_[t2].visible = false;  // appear once the input connects
        filters_[S].on_input_connected = [this, t1, t2] { pins_[t1].visible = pins_[t2].visible = true; };
        V = add_fake_filter(u"VP8 Decoder");
        const int v_in = add_fake_pin(V, false, u"VP80");
        pins_[v_in].accepts = [](const MediaType& t) { return guid_eq(t.sub, kSubVp80); };
        const int v_out = add_fake_pin(V, true, u"YV12");
        pins_[v_out].offers = {video_type(kSubYv12)};
        A = add_fake_filter(u"Vorbis Decoder");
        const int a_in = add_fake_pin(A, false, u"Vorbis");
        pins_[a_in].accepts = [](const MediaType& t) { return guid_eq(t.sub, kSubVorbis); };
        const int a_out = add_fake_pin(A, true, u"PCM (IEEE Float)");
        pins_[a_out].offers = {audio_float()};
        R = add_fake_filter(u"Frontier Texture Renderer", renderer_misc);
        const int r_in = add_fake_pin(R, false, u"In");
        pins_[r_in].accepts = [](const MediaType& t) { return guid_eq(t.sub, kSubYv12); };
    }

    void add_to_graph(GuestAddr graph, int f) {
        put_wide(NAME_ADDR, filters_[f].name);
        ASSERT_EQ(invoke(&ComCoreManager::filter_graph_add_filter, {graph, filters_[f].addr, NAME_ADDR}),
                  com::S_OK);
    }

    /// C:\movies\clip.webm with the EBML magic and @p bytes in all.
    void make_file(u32 bytes) {
        char temp_template[] = "/tmp/vitreo_connect_XXXXXX";
        char* dir = ::mkdtemp(temp_template);
        ASSERT_NE(dir, nullptr);
        root_ = dir;
        std::filesystem::create_directories(root_ / "movies");
        file_bytes_.resize(bytes);
        for (u32 i = 0; i < bytes; ++i) file_bytes_[i] = static_cast<u8>(i * 7u + 3u);
        file_bytes_[0] = 0x1A; file_bytes_[1] = 0x45; file_bytes_[2] = 0xDF; file_bytes_[3] = 0xA3;
        std::ofstream out(root_ / "movies" / "clip.webm", std::ios::binary);
        out.write(reinterpret_cast<const char*>(file_bytes_.data()), bytes);
        out.close();
        ASSERT_TRUE(vfs_.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root_.string())).is_ok());
        mgr_.set_vfs(&vfs_);
    }
    void TearDown() override {
        if (!root_.empty()) std::filesystem::remove_all(root_);
        FilterGraphBitnessTest::TearDown();
    }

    u32 render_file(GuestAddr graph, std::u16string_view path = u"C:\\movies\\clip.webm") {
        put_wide(DATA_BASE + 0x200, path);
        return invoke(&ComCoreManager::filter_graph_render_file, {graph, DATA_BASE + 0x200, 0});
    }

    /// Host-side calls with logical arguments on the main CPU.
    u32 call(GuestAddr p, Iface iface, u32 slot, std::initializer_list<u64> args) {
        auto m = make_marshaler();
        return mgr_.dshow_host().call(m, p, iface, slot, args);
    }
    GuestAddr out_ptr(GuestAddr slot) { return static_cast<GuestAddr>(ptr(slot)); }

    /// Every filter in the graph by name (EnumFilters / FindFilterByName).
    GuestAddr find(GuestAddr graph, std::u16string_view name) {
        put_wide(NAME_ADDR + 0x300, name);
        (void)memory_.write_u64(OUT3_ADDR, 0);
        const u32 hr = invoke(&ComCoreManager::filter_graph_find_filter_by_name,
                              {graph, NAME_ADDR + 0x300, OUT3_ADDR});
        if (hr != com::S_OK) return 0;
        const GuestAddr f = out_ptr(OUT3_ADDR);
        (void)call(f, Iface::BaseFilter, 2, {});
        return f;
    }
    /// Pins of a filter through IBaseFilter::EnumPins (host or guest).
    std::vector<GuestAddr> pins_of(GuestAddr filter) {
        std::vector<GuestAddr> out;
        constexpr GuestAddr E = DATA_BASE + 0x7000;
        (void)memory_.write_u64(E, 0);
        if (call(filter, Iface::BaseFilter, 10, {E}) != com::S_OK) return out;
        const GuestAddr e = out_ptr(E);
        for (int i = 0; i < 16; ++i) {
            if (call(e, Iface::EnumPins, 3, {1, E + 8, E + 16}) != com::S_OK) break;
            out.push_back(out_ptr(E + 8));
            (void)call(out.back(), Iface::Pin, 2, {});
        }
        (void)call(e, Iface::EnumPins, 2, {});
        return out;
    }
    GuestAddr peer_of(GuestAddr pin) {
        constexpr GuestAddr P = DATA_BASE + 0x7100;
        (void)memory_.write_u64(P, 0);
        if (call(pin, Iface::Pin, 6, {P}) != com::S_OK) return 0;
        const GuestAddr p = out_ptr(P);
        (void)call(p, Iface::Pin, 2, {});
        return p;
    }
    std::u16string pin_name(GuestAddr pin) {
        constexpr GuestAddr I = DATA_BASE + 0x7200;
        (void)call(pin, Iface::Pin, 8, {I});
        const GuestAddr owner = out_ptr(I);
        (void)call(owner, Iface::BaseFilter, 2, {});
        return wstr(I + (x64_ ? 12u : 8u));
    }

    // ---------------------------------------------------------- dispatch
    u64 dispatch(GuestAddr fn, const u64* args, usize count) {
        const u32 slot = static_cast<u32>((fn - FILTER_FN) / 0x10u);
        const GuestAddr self = static_cast<GuestAddr>(args[0]);
        auto m = make_marshaler();
        ds::DShowHost& host = mgr_.dshow_host();
        // x86 splits a REFERENCE_TIME into two slots.
        const auto wide = [&](usize i) {
            return x64_ ? args[i] : ((args[i] & 0xFFFFFFFFull) | (args[i + 1] << 32));
        };
        for (usize fi = 0; fi < filters_.size(); ++fi) {
            Filter& f = filters_[fi];
            if (self == f.seek) return seeking(std::string(f.name.begin(), f.name.end()), slot, args, count);
            if (self == f.misc) {
                if (slot == 3) return f.renderer_flag ? 1u : 0u;
                if (slot == 1 || slot == 2) return 1;
                return 0x80004001u;
            }
            if (self != f.addr) continue;
            const std::string who(f.name.begin(), f.name.end());
            switch (slot) {
            case 0: {
                const Guid iid = read_guid_at(static_cast<GuestAddr>(args[1]));
                if (guid_eq(iid, ds::sdk::IID_IMediaSeeking) && f.has_seeking) {
                    wptr(static_cast<GuestAddr>(args[2]), f.seek);
                    return 0;
                }
                if (guid_eq(iid, ds::sdk::IID_IAMFilterMiscFlags) && f.renderer_flag) {
                    wptr(static_cast<GuestAddr>(args[2]), f.misc);
                    return 0;
                }
                if (guid_eq(iid, ds::sdk::IID_IBaseFilter) || iid.Data1 == 0) {
                    wptr(static_cast<GuestAddr>(args[2]), f.addr);
                    ++f.refs;
                    return 0;
                }
                wptr(static_cast<GuestAddr>(args[2]), 0);
                return 0x80004002u;
            }
            case 1: return static_cast<u64>(++f.refs);
            case 2: return static_cast<u64>(--f.refs);
            case 4: log_.push_back(who + ".Stop"); f.state = 0; return 0;
            case 5: log_.push_back(who + ".Pause"); f.state = 1; return 0;
            case 6: log_.push_back(who + ".Run"); f.state = 2; f.run_start = static_cast<i64>(wide(1)); EXPECT_EQ(count, x64_ ? 2u : 3u); return 0;
            case 7: (void)memory_.write_u32(static_cast<GuestAddr>(args[2]), f.state); return 0;
            case 8: log_.push_back(who + (args[1] != 0 ? ".SetSyncSource" : ".SetSyncSourceNULL")); f.clock = static_cast<GuestAddr>(args[1]); return 0;
            case 10: {
                std::vector<GuestAddr> list;
                for (const int p : f.pins) if (pins_[p].visible) list.push_back(pins_[p].addr);
                wptr(static_cast<GuestAddr>(args[1]), ds::create_pin_enum(host, m, list));
                return 0;
            }
            case 13:
                f.graph = static_cast<GuestAddr>(args[1]);
                log_.push_back(who + (args[1] != 0 ? ".Join" : ".JoinNULL"));
                return 0;
            default: return 0x80004001u;
            }
        }
        for (Pin& p : pins_) {
            if (self == p.seek) return seeking(std::string(p.name.begin(), p.name.end()), slot, args, count);
            if (self != p.addr) continue;
            Filter& f = filters_[p.filter];
            const std::string who(f.name.begin(), f.name.end());
            const std::string pn(p.name.begin(), p.name.end());
            switch (slot) {
            case 0: {
                const Guid iid = read_guid_at(static_cast<GuestAddr>(args[1]));
                if (guid_eq(iid, ds::sdk::IID_IMediaSeeking) && p.has_seeking) {
                    wptr(static_cast<GuestAddr>(args[2]), p.seek);
                    return 0;
                }
                if (guid_eq(iid, ds::sdk::IID_IPin) || iid.Data1 == 0) {
                    wptr(static_cast<GuestAddr>(args[2]), p.addr);
                    return 0;
                }
                wptr(static_cast<GuestAddr>(args[2]), 0);
                return 0x80004002u;
            }
            case 1: return static_cast<u64>(++f.refs);
            case 2: return static_cast<u64>(--f.refs);
            case 3: {  // Connect(receiver, pmt)
                const GuestAddr receiver = static_cast<GuestAddr>(args[1]);
                if (p.peer != 0) return 0x80040204u;
                const GuestAddr mt = host.scratch(96);
                for (const MediaType& t : p.offers) {
                    (void)ds::write_media_type(host, m, mt, t);
                    if (host.call(m, receiver, Iface::Pin, 4, {p.addr, mt}) == com::S_OK) {
                        p.peer = receiver;
                        p.type = t;
                        (void)host.add_ref(m, receiver);
                        host.free_scratch(mt);
                        return 0;
                    }
                }
                host.free_scratch(mt);
                return 0x80040207u;  // VFW_E_NO_ACCEPTABLE_TYPES
            }
            case 4: {  // ReceiveConnection(connector, pmt)
                const MediaType t = ds::read_media_type(m, static_cast<GuestAddr>(args[2]));
                attempts_.push_back(who + "." + pn);
                if (p.peer != 0) return 0x80040204u;
                if (!p.accepts || !p.accepts(t)) return 0x8004022Au;
                p.peer = static_cast<GuestAddr>(args[1]);
                p.type = t;
                if (f.on_input_connected) f.on_input_connected();
                return 0;
            }
            case 5: {  // Disconnect
                log_.push_back(who + "." + pn + ".Disconnect");
                if (p.peer == 0) return 1;
                if (p.output) (void)host.release(m, p.peer);  // Connect's reference
                p.peer = 0;
                if (!p.output && f.disconnect_downstream_through_graph && f.graph != 0) {
                    for (const int q : f.pins) {
                        if (!pins_[q].output || pins_[q].peer == 0) continue;
                        const GuestAddr peer = pins_[q].peer;
                        graph_disconnect_hrs_.push_back(nested_graph_disconnect(f.graph, peer));
                        graph_disconnect_hrs_.push_back(nested_graph_disconnect(f.graph, pins_[q].addr));
                    }
                }
                return 0;
            }
            case 6: {  // ConnectedTo: AddRef'd, as IPin requires
                wptr(static_cast<GuestAddr>(args[1]), p.peer);
                if (p.peer != 0) (void)host.add_ref(m, p.peer);
                return p.peer != 0 ? 0u : kVfwENotConnected;
            }
            case 7: return p.peer != 0 && ds::write_media_type(host, m, static_cast<GuestAddr>(args[1]), p.type) ? 0u : kVfwENotConnected;
            case 8: {  // QueryPinInfo
                const GuestAddr info = static_cast<GuestAddr>(args[1]);
                wptr(info, f.addr);
                ++f.refs;
                (void)memory_.write_u32(info + (x64_ ? 8u : 4u), p.output ? 1u : 0u);
                put_wide(info + (x64_ ? 12u : 8u), p.name);
                return 0;
            }
            case 9: (void)memory_.write_u32(static_cast<GuestAddr>(args[1]), p.output ? 1u : 0u); return 0;
            case 11: return p.accepts && p.accepts(ds::read_media_type(m, static_cast<GuestAddr>(args[1]))) ? 0u : 1u;
            case 12: wptr(static_cast<GuestAddr>(args[1]), ds::create_type_enum(host, m, p.offers)); return 0;
            default: return 0;
            }
        }
        ADD_FAILURE() << "unexpected guest object 0x" << std::hex << self;
        return 0x80004005u;
    }
    /// A splitter-backed IMediaSeeking with the measured stream: duration
    /// 125416669 (12.54 s), capabilities 0x3f, a seek to 5 s lands on the key
    /// frame at 3.583 s (dshow_seeking_probe64.expected).
    u64 seeking(const std::string& who, u32 slot, const u64* args, usize count) {
        (void)count;
        static constexpr const char* kNames[] = {"QI", "AddRef", "Release", "GetCapabilities",
            "CheckCapabilities", "IsFormatSupported", "QueryPreferredFormat", "GetTimeFormat",
            "IsUsingTimeFormat", "SetTimeFormat", "GetDuration", "GetStopPosition",
            "GetCurrentPosition", "ConvertTimeFormat", "SetPositions", "GetPositions", "GetAvailable",
            "SetRate", "GetRate", "GetPreroll"};
        if (slot >= 3) seek_log_.push_back(who + "." + kNames[slot < 20 ? slot : 0]);
        const GuestAddr a1 = static_cast<GuestAddr>(args[1]);
        constexpr i64 kDuration = 125416669;
        switch (slot) {
        case 1: case 2: return 1;
        case 3: (void)memory_.write_u32(a1, 0x3f); return 0;
        case 5: return read_guid_at(a1).Data1 == ds::sdk::TIME_FORMAT_MEDIA_TIME.Data1 ? 0u : 1u;
        case 10: case 11: (void)memory_.write_u64(a1, kDuration); return 0;
        case 14: {
            const i64 cur = a1 != 0 ? static_cast<i64>(memory_.read_u64(a1).value_or(0)) : -1;
            last_set_cur_ = cur;
            last_set_flags_ = static_cast<u32>(args[2]);
            last_set_stop_flags_ = static_cast<u32>(args[4]);
            if (a1 != 0 && (last_set_flags_ & 8u) != 0 && cur == 50'000'000) {
                (void)memory_.write_u64(a1, 35'830'000);
            }
            return 0;
        }
        case 16: (void)memory_.write_u64(a1, 0); (void)memory_.write_u64(static_cast<GuestAddr>(args[2]), kDuration); return 0;
        case 18: { const double one = 1.0; u64 bits; std::memcpy(&bits, &one, 8); (void)memory_.write_u64(a1, bits); return 0; }
        case 19: (void)memory_.write_u64(a1, 0); return 0;
        default: return 0x80004001u;
        }
    }
    /// IFilterGraph::Disconnect from inside a guest call, on a CPU state of
    /// its own (the outer call's frame stays intact).
    u32 nested_graph_disconnect(GuestAddr graph, GuestAddr pin) {
        cpu::CpuState cpu;
        const GuestAddr sp = STACK_BASE + 0xC00;
        if (x64_) {
            cpu.init_long_mode();
            cpu.set_sp(sp);
            cpu.gpr.rcx = graph;
            cpu.gpr.rdx = pin;
        } else {
            cpu.init_protected_mode();
            cpu.set_sp(sp);
            (void)memory_.write_u32(sp + 4, static_cast<u32>(graph));
            (void)memory_.write_u32(sp + 8, static_cast<u32>(pin));
        }
        Marshaler nm(memory_);
        nm.attach_cpu(cpu);
        return mgr_.filter_graph_disconnect(nm);
    }
    [[nodiscard]] Guid read_guid_at(GuestAddr a) {
        Guid g{};
        (void)memory_.read_bytes(a, reinterpret_cast<Byte*>(&g), sizeof(g));
        return g;
    }
    GuestAddr pin_addr(int filter, std::u16string_view name) {
        for (const int p : filters_[filter].pins) if (pins_[p].name == name) return pins_[p].addr;
        return 0;
    }
    Pin& pin(int filter, std::u16string_view name) {
        for (const int p : filters_[filter].pins) if (pins_[p].name == name) return pins_[p];
        return pins_[0];
    }

    std::vector<Filter> filters_;
    std::vector<Pin> pins_;
    std::vector<std::string> log_;
    std::vector<std::string> attempts_;
    std::vector<std::string> seek_log_;
    std::vector<u32> graph_disconnect_hrs_;
    i64 last_set_cur_{-1};
    u32 last_set_flags_{0};
    u32 last_set_stop_flags_{0};
    u32 next_object_{0};
    int S{0}, V{0}, A{0}, R{0};
    capsule::Vfs vfs_;
    std::filesystem::path root_;
    std::vector<u8> file_bytes_;
    TestAudioRenderer audio_;
};

// -------------------------------------------------------------- the graph

TEST_P(FilterGraphConnectTest, RenderFileBuildsTheGraphFromTheFiltersInIt) {
    make_file(4096);
    mgr_.set_audio_renderer(&audio_);
    build_game_filters(true);
    const GuestAddr graph = create_graph();
    for (const int f : {S, V, A, R}) add_to_graph(graph, f);
    EXPECT_EQ(render_file(graph), com::S_OK);

    // The source is a File Source (Async) named with the path as given; its
    // MEDIATYPE_Stream pin is offered to the filters most recent first.
    const GuestAddr source = find(graph, u"C:\\movies\\clip.webm");
    ASSERT_NE(source, 0u);
    constexpr GuestAddr C = DATA_BASE + 0x7300;
    ASSERT_EQ(call(source, Iface::BaseFilter, 3, {C}), com::S_OK);
    EXPECT_TRUE(guid_eq(read_guid_at(C), ds::sdk::CLSID_AsyncReader));
    // (Each refused filter sees the type twice: the pin enumerates it twice.)
    std::vector<std::string> order;
    for (const std::string& a : attempts_) {
        if (order.empty() || order.back() != a) order.push_back(a);
    }
    ASSERT_GE(order.size(), 4u);
    EXPECT_EQ(attempts_[0], attempts_[1]);
    EXPECT_EQ(order[0], "Frontier Texture Renderer.In");
    EXPECT_EQ(order[1], "Vorbis Decoder.Vorbis");
    EXPECT_EQ(order[2], "VP8 Decoder.VP80");
    EXPECT_EQ(order[3], "WebM Splitter.WebM");
    const std::vector<GuestAddr> source_pins = pins_of(source);
    ASSERT_EQ(source_pins.size(), 1u);
    EXPECT_EQ(pin_name(source_pins[0]), u"Output");
    EXPECT_EQ(peer_of(source_pins[0]), pin_addr(S, u"WebM"));
    EXPECT_TRUE(guid_eq(pin(S, u"WebM").type.major, ds::sdk::MEDIATYPE_Stream));

    // Video: Track1 -> VP8 -> renderer. Audio: Track2 -> Vorbis -> a
    // DirectSound Renderer the graph adds.
    EXPECT_EQ(pin(S, u"Track1").peer, pin_addr(V, u"VP80"));
    EXPECT_EQ(pin(V, u"YV12").peer, pin_addr(R, u"In"));
    EXPECT_EQ(pin(S, u"Track2").peer, pin_addr(A, u"Vorbis"));
    const GuestAddr dsound = find(graph, u"Default DirectSound Device");
    ASSERT_NE(dsound, 0u);
    ASSERT_EQ(call(dsound, Iface::BaseFilter, 3, {C}), com::S_OK);
    EXPECT_TRUE(guid_eq(read_guid_at(C), ds::sdk::CLSID_DSoundRender));
    const std::vector<GuestAddr> ds_pins = pins_of(dsound);
    ASSERT_EQ(ds_pins.size(), 1u);
    EXPECT_EQ(pin(A, u"PCM (IEEE Float)").peer, ds_pins[0]);
    ASSERT_NE(audio_.last_push, nullptr);
    EXPECT_TRUE(audio_.last_format.float_samples);
    EXPECT_EQ(audio_.last_format.sample_rate, 48000u);
    EXPECT_EQ(audio_.last_format.channels, 2u);
    EXPECT_EQ(audio_.last_format.bits_per_sample, 32u);
    (void)invoke(&ComCoreManager::filter_graph_release, {graph});
}

TEST_P(FilterGraphConnectTest, WithoutAnAudioRendererTheAudioBranchIsUndone) {
    // Measured on winoracle (no audio endpoint): VFW_S_AUDIO_NOT_RENDERED,
    // the Vorbis decoder left unconnected, Track2 unconnected.
    make_file(4096);
    build_game_filters(true);
    const GuestAddr graph = create_graph();
    for (const int f : {S, V, A, R}) add_to_graph(graph, f);
    EXPECT_EQ(render_file(graph), kVfwSAudioNotRendered);
    EXPECT_EQ(pin(S, u"Track1").peer, pin_addr(V, u"VP80"));
    EXPECT_EQ(pin(V, u"YV12").peer, pin_addr(R, u"In"));
    EXPECT_EQ(pin(S, u"Track2").peer, 0u);
    EXPECT_EQ(pin(A, u"Vorbis").peer, 0u);
    EXPECT_EQ(pin(A, u"PCM (IEEE Float)").peer, 0u);
    EXPECT_EQ(find(graph, u"Default DirectSound Device"), 0u);
    (void)invoke(&ComCoreManager::filter_graph_release, {graph});
}

TEST_P(FilterGraphConnectTest, WebMWithNothingToRenderItIsAnUnsupportedStream) {
    // Measured: an empty graph and a renderer-only graph both fail with
    // VFW_E_UNSUPPORTED_STREAM and the source filter is removed again.
    make_file(4096);
    const GuestAddr empty = create_graph();
    EXPECT_EQ(render_file(empty), kVfwEUnsupportedStream);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_enum_filters, {empty, OUT2_ADDR}), com::S_OK);
    const GuestAddr e = out_ptr(OUT2_ADDR);
    EXPECT_EQ(invoke_op(&ComCoreManager::filter_graph_enum_filters_method, 3,
                        {e, 1, ARRAY_ADDR, OUT3_ADDR}),
              com::S_FALSE);
    (void)invoke_op(&ComCoreManager::filter_graph_enum_filters_method, 2, {e});
    (void)invoke(&ComCoreManager::filter_graph_release, {empty});

    build_game_filters(true);
    const GuestAddr only_renderer = create_graph();
    add_to_graph(only_renderer, R);
    EXPECT_EQ(render_file(only_renderer), kVfwEUnsupportedStream);
    EXPECT_EQ(find(only_renderer, u"C:\\movies\\clip.webm"), 0u);
    EXPECT_NE(find(only_renderer, u"Frontier Texture Renderer"), 0u);
    (void)invoke(&ComCoreManager::filter_graph_release, {only_renderer});
}

TEST_P(FilterGraphConnectTest, StateChangesClockAndTeardownFollowWindows) {
    make_file(4096);
    build_game_filters(true);
    const GuestAddr graph = create_graph();
    for (const int f : {S, V, A, R}) add_to_graph(graph, f);
    ASSERT_EQ(render_file(graph), kVfwSAudioNotRendered);
    const GuestAddr control = qi(graph, IID_IMEDIACONTROL);
    log_.clear();

    // Pause: every filter gets the graph clock, then Pause, renderers
    // before the filters feeding them.
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_pause, {control}), com::S_OK);
    const auto at = [&](const std::string& entry) {
        return std::find(log_.begin(), log_.end(), entry) - log_.begin();
    };
    for (const char* f : {"WebM Splitter", "VP8 Decoder", "Vorbis Decoder", "Frontier Texture Renderer"}) {
        EXPECT_LT(at(std::string(f) + ".SetSyncSource"), at(std::string(f) + ".Pause")) << f;
    }
    EXPECT_LT(at("Frontier Texture Renderer.Pause"), at("VP8 Decoder.Pause"));
    EXPECT_LT(at("VP8 Decoder.Pause"), at("WebM Splitter.Pause"));
    const GuestAddr clock = filters_[R].clock;
    ASSERT_NE(clock, 0u);
    EXPECT_EQ(filters_[S].clock, clock);

    // Run: tStart is the clock's time plus about 10 ms.
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_run, {control}), com::S_OK);
    constexpr GuestAddr T = DATA_BASE + 0x7400;
    ASSERT_EQ(call(clock, Iface::ReferenceClock, 3, {T}), com::S_OK);
    const i64 now = static_cast<i64>(memory_.read_u64(T).value_or(0));
    EXPECT_GT(filters_[R].run_start, now - 500'000);
    EXPECT_LE(filters_[R].run_start, now + 100'000);
    EXPECT_EQ(filters_[S].run_start, filters_[R].run_start);

    // Stop from Running: Pause, then Stop.
    log_.clear();
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_stop, {control}), com::S_OK);
    EXPECT_LT(at("Frontier Texture Renderer.Pause"), at("Frontier Texture Renderer.Stop"));
    EXPECT_LT(at("WebM Splitter.Pause"), at("WebM Splitter.Stop"));
    EXPECT_EQ(filters_[S].state, 0u);

    // Final Release: pins disconnected, sync source NULL, then left.
    log_.clear();
    (void)invoke(&ComCoreManager::filter_graph_release, {control});
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_release, {graph}), 0u);
    EXPECT_LT(at("Frontier Texture Renderer.In.Disconnect"), at("Frontier Texture Renderer.SetSyncSourceNULL"));
    EXPECT_LT(at("Frontier Texture Renderer.SetSyncSourceNULL"), at("Frontier Texture Renderer.JoinNULL"));
    EXPECT_EQ(pin(V, u"YV12").peer, 0u);
    for (const int f : {S, V, A, R}) EXPECT_EQ(filters_[f].refs, 1) << f;
}

TEST_P(FilterGraphConnectTest, DirectSoundRendererPlaysPcmAndSendsEcComplete) {
    make_file(4096);
    mgr_.set_audio_renderer(&audio_);
    build_game_filters(false);  // the video renderer is not counted
    const GuestAddr graph = create_graph();
    for (const int f : {S, V, A, R}) add_to_graph(graph, f);
    ASSERT_EQ(render_file(graph), com::S_OK);
    const GuestAddr dsound = find(graph, u"Default DirectSound Device");
    ASSERT_NE(dsound, 0u);
    const GuestAddr in = pins_of(dsound).at(0);
    EXPECT_EQ(pin_name(in), u"Audio Input pin (rendered)");
    // IAMFilterMiscFlags: IS_RENDERER.
    auto m = make_marshaler();
    const GuestAddr misc = mgr_.dshow_host().query(m, dsound, ds::sdk::IID_IAMFilterMiscFlags);
    ASSERT_NE(misc, 0u);
    EXPECT_EQ(call(misc, Iface::MiscFlags, 3, {}), 1u);
    (void)call(misc, Iface::MiscFlags, 2, {});
    // The decoder side: the pin's allocator (CBaseInputPin makes one).
    const GuestAddr mem = mgr_.dshow_host().query(m, in, ds::sdk::IID_IMemInputPin);
    ASSERT_NE(mem, 0u);
    constexpr GuestAddr X = DATA_BASE + 0x7500;
    ASSERT_EQ(call(mem, Iface::MemInputPin, 3, {X}), com::S_OK);
    const GuestAddr allocator = out_ptr(X);
    for (u32 i = 0; i < 16; i += 4) (void)memory_.write_u32(X + 16 + i, 0);
    (void)memory_.write_u32(X + 16, 2);
    (void)memory_.write_u32(X + 20, 4096);
    (void)memory_.write_u32(X + 24, 1);
    ASSERT_EQ(call(allocator, Iface::MemAllocator, 3, {X + 16, X + 32}), com::S_OK);
    ASSERT_EQ(call(mem, Iface::MemInputPin, 4, {allocator, 0}), com::S_OK);
    ASSERT_EQ(call(allocator, Iface::MemAllocator, 5, {}), com::S_OK);
    ASSERT_EQ(call(allocator, Iface::MemAllocator, 7, {X + 48, 0, 0, 0}), com::S_OK);
    const GuestAddr sample = out_ptr(X + 48);
    ASSERT_EQ(call(sample, Iface::MediaSample, 3, {X + 56}), com::S_OK);
    const GuestAddr data = out_ptr(X + 56);
    std::vector<u8> pcm(4096);
    for (u32 i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<u8>(i);
    ASSERT_TRUE(memory_.write_bytes(data, pcm.data(), pcm.size()).is_ok());
    ASSERT_EQ(call(sample, Iface::MediaSample, 12, {4096}), com::S_OK);

    // Pause: the renderer has no sample yet, so the graph says S_FALSE.
    const GuestAddr control = qi(graph, IID_IMEDIACONTROL);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_pause, {control}), com::S_FALSE);
    // Receive blocks while paused (the preroll) until Run.
    std::atomic<u32> receive_hr{0xFFFFFFFFu};
    std::thread sender([&] {
        Marshaler tm(memory_);
        tm.attach_cpu(cpu_);
        receive_hr = mgr_.dshow_host().call(tm, mem, Iface::MemInputPin, 6, {sample});
    });
    constexpr GuestAddr ST = DATA_BASE + 0x7600;
    (void)memory_.write_u32(ST, 99);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_get_state, {control, 2000, ST}),
              com::S_OK);
    EXPECT_EQ(memory_.read_u32(ST).value_or(0), 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(receive_hr.load(), 0xFFFFFFFFu);  // still held
    ASSERT_NE(audio_.last_push, nullptr);
    EXPECT_EQ(audio_.last_push->submitted, pcm);
    EXPECT_FALSE(audio_.last_push->started);

    // IBasicAudio on the graph reaches the renderer: -20 dB is a gain of 0.1.
    const GuestAddr basic = qi(graph, IID_IBASICAUDIO);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_basic_audio_put_volume,
                     {basic, static_cast<u64>(static_cast<u32>(-2000))}),
              com::S_OK);
    ASSERT_EQ(audio_.last_push->gains.size(), 2u);
    EXPECT_NEAR(audio_.last_push->gains[0], 0.1f, 1e-4f);

    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_run, {control}), com::S_OK);
    sender.join();
    EXPECT_EQ(receive_hr.load(), com::S_OK);
    EXPECT_TRUE(audio_.last_push->started);
    (void)call(sample, Iface::MediaSample, 2, {});

    // EndOfStream, the queued audio drains, EC_COMPLETE (lParam1 S_OK,
    // lParam2 0) reaches the application.
    EXPECT_EQ(call(in, Iface::Pin, 14, {}), com::S_OK);
    audio_.last_push->padding = 0;
    const GuestAddr events = qi(graph, IID_IMEDIAEVENTEX);
    constexpr GuestAddr CODE = DATA_BASE + 0x7700;
    (void)memory_.write_u64(CODE + 16, 0xCAFEBABECAFEBABEull);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_event_get_event,
                     {events, CODE, CODE + 8, CODE + 16, 3000}),
              com::S_OK);
    EXPECT_EQ(memory_.read_u32(CODE).value_or(0), 1u);
    EXPECT_EQ(ptr(CODE + 8), 0u);
    EXPECT_EQ(ptr(CODE + 16), 0u);
    // Receive after EndOfStream: VFW_E_SAMPLE_REJECTED_EOS.
    ASSERT_EQ(call(allocator, Iface::MemAllocator, 7, {X + 48, 0, 0, 0}), com::S_OK);
    EXPECT_EQ(call(mem, Iface::MemInputPin, 6, {out_ptr(X + 48)}), 0x8004022Cu);
    (void)call(out_ptr(X + 48), Iface::MediaSample, 2, {});

    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_stop, {control}), com::S_OK);
    (void)call(allocator, Iface::MemAllocator, 6, {});
    (void)call(allocator, Iface::MemAllocator, 2, {});
    (void)call(mem, Iface::MemInputPin, 2, {});
    (void)invoke(&ComCoreManager::filter_graph_release, {graph});
}

TEST_P(FilterGraphConnectTest, SeekingIsForwardedToTheRenderers) {
    // dshow_seeking_probe64.expected: the FGM answers the time format
    // itself, asks the sink filters for the rest, and keeps its own
    // position.
    make_file(4096);
    mgr_.set_audio_renderer(&audio_);
    // The video renderer is counted (IS_RENDERER), so the audio stream's
    // EC_COMPLETE alone does not end the graph's stream time.
    build_game_filters(true);
    filters_[R].has_seeking = true;                  // CRendererPosPassThru on the renderer
    pin(A, u"PCM (IEEE Float)").has_seeking = true;  // what the DirectSound Renderer passes to
    const GuestAddr graph = create_graph();
    for (const int f : {S, V, A, R}) add_to_graph(graph, f);
    ASSERT_EQ(render_file(graph), com::S_OK);
    const GuestAddr seeking = qi(graph, IID_IMEDIASEEKING);
    const auto seek = [&](u32 slot, std::initializer_list<u64> args) {
        std::vector<u64> all{seeking};
        all.insert(all.end(), args.begin(), args.end());
        // x86: ConvertTimeFormat's LONGLONG takes two stack slots.
        if (!x64_ && slot == 13) {
            all = {seeking, args.begin()[0], args.begin()[1], args.begin()[2] & 0xFFFFFFFFull,
                   args.begin()[2] >> 32, args.begin()[3]};
        }
        if (x64_) {
            const GuestAddr frame = STACK_BASE + 0x400;
            cpu_.set_sp(frame);
            for (usize i = 0; i < all.size(); ++i) {
                if (i == 0) cpu_.gpr.rcx = all[i];
                if (i == 1) cpu_.gpr.rdx = all[i];
                if (i == 2) cpu_.gpr.r8 = all[i];
                if (i == 3) cpu_.gpr.r9 = all[i];
                if (i >= 4) (void)memory_.write_u64(frame + 0x28 + 8 * (i - 4), all[i]);
            }
        } else {
            const GuestAddr frame = STACK_BASE + 0x400;
            cpu_.set_sp(frame);
            for (usize i = 0; i < all.size(); ++i) (void)memory_.write_u32(frame + 4 + 4 * i, static_cast<u32>(all[i]));
        }
        auto m = make_marshaler();
        u32 result = 0;
        EXPECT_TRUE(mgr_.intelligent_seeking(m, slot, result));
        return result;
    };
    constexpr GuestAddr V1 = DATA_BASE + 0x7800;
    constexpr GuestAddr V2 = DATA_BASE + 0x7808;
    constexpr GuestAddr G = DATA_BASE + 0x7820;
    EXPECT_EQ(seek(3, {V1}), com::S_OK);
    EXPECT_EQ(memory_.read_u32(V1).value_or(0), 0x3fu);
    (void)memory_.write_u32(V1, 0xFFFFFFFFu);
    EXPECT_EQ(seek(4, {V1}), com::S_FALSE);
    EXPECT_EQ(memory_.read_u32(V1).value_or(0), 0x3fu);
    (void)memory_.write_u32(V1, 0x21);
    EXPECT_EQ(seek(4, {V1}), com::S_OK);
    EXPECT_EQ(memory_.read_u32(V1).value_or(0), 0x21u);
    write_guid(G, ds::sdk::TIME_FORMAT_MEDIA_TIME);
    EXPECT_EQ(seek(5, {G}), com::S_OK);
    write_guid(G, ds::sdk::MEDIATYPE_Video);  // any other format
    EXPECT_EQ(seek(5, {G}), com::S_FALSE);
    EXPECT_EQ(seek(6, {G}), com::S_OK);
    EXPECT_TRUE(guid_eq(read_guid_at(G), ds::sdk::TIME_FORMAT_MEDIA_TIME));
    EXPECT_EQ(seek(8, {G}), com::S_OK);
    EXPECT_EQ(seek(10, {V1}), com::S_OK);
    EXPECT_EQ(memory_.read_u64(V1).value_or(0), 125416669u);
    EXPECT_EQ(seek(11, {V1}), com::S_OK);
    EXPECT_EQ(memory_.read_u64(V1).value_or(0), 125416669u);
    (void)memory_.write_u64(V1, 77);
    EXPECT_EQ(seek(12, {V1}), com::S_OK);
    EXPECT_EQ(memory_.read_u64(V1).value_or(1), 0u);
    EXPECT_EQ(seek(15, {V1, V2}), com::S_OK);
    EXPECT_EQ(memory_.read_u64(V2).value_or(0), 125416669u);
    EXPECT_EQ(seek(16, {V1, V2}), com::S_OK);
    EXPECT_EQ(memory_.read_u64(V2).value_or(0), 125416669u);
    EXPECT_EQ(seek(13, {V1, 0, 12345, 0}), com::S_OK);
    EXPECT_EQ(memory_.read_u64(V1).value_or(0), 12345u);
    // Both sinks were asked; the DirectSound Renderer passed the call to the
    // Vorbis decoder's output pin.
    const auto asked = [&](const std::string& e) {
        return std::find(seek_log_.begin(), seek_log_.end(), e) != seek_log_.end();
    };
    EXPECT_TRUE(asked("Frontier Texture Renderer.GetDuration"));
    EXPECT_TRUE(asked("PCM (IEEE Float).GetDuration"));
    EXPECT_FALSE(asked("Frontier Texture Renderer.GetCurrentPosition"));  // the FGM's own
    // IMediaPosition: seconds; CanSeek* OATRUE.
    put_args({seeking == 0 ? 0 : qi(graph, IID_IMEDIAPOSITION), V1});
    {
        auto m = make_marshaler();
        EXPECT_EQ(mgr_.filter_graph_media_position(m, 0), com::S_OK);
    }
    double seconds = 0;
    const u64 bits = memory_.read_u64(V1).value_or(0);
    std::memcpy(&seconds, &bits, sizeof(seconds));
    EXPECT_NEAR(seconds, 12.5416669, 1e-6);
    put_args({qi(graph, IID_IMEDIAPOSITION), V1});
    {
        auto m = make_marshaler();
        EXPECT_EQ(mgr_.filter_graph_media_position(m, 9), com::S_OK);
    }
    EXPECT_EQ(static_cast<i32>(memory_.read_u32(V1).value_or(0)), -1);

    // Running: the position follows the clock from tStart.
    const GuestAddr control = qi(graph, IID_IMEDIACONTROL);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_pause, {control}), com::S_FALSE);
    // The audio stream is empty: EndOfStream ends the renderer's cue, so Run
    // completes at once (no deferred-Run thread in this fixture).
    const GuestAddr dsound = find(graph, u"Default DirectSound Device");
    ASSERT_EQ(call(pins_of(dsound).at(0), Iface::Pin, 14, {}), com::S_OK);
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_run, {control}), com::S_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(seek(12, {V1}), com::S_OK);
    const i64 running = static_cast<i64>(memory_.read_u64(V1).value_or(0));
    EXPECT_GT(running, 1'000'000);
    EXPECT_LT(running, 20'000'000);
    // Stop: the renderers are repositioned there, AbsolutePositioning | ReturnTime.
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_stop, {control}), com::S_OK);
    EXPECT_EQ(last_set_flags_, 9u);
    EXPECT_EQ(last_set_stop_flags_, 0u);
    EXPECT_GT(last_set_cur_, 1'000'000);
    // SetPositions(5 s, Absolute | ReturnTime): the key frame comes back.
    (void)memory_.write_u64(V1, 50'000'000);
    EXPECT_EQ(seek(14, {V1, 9, 0, 0}), com::S_OK);
    EXPECT_EQ(memory_.read_u64(V1).value_or(0), 35'830'000u);
    EXPECT_EQ(seek(12, {V2}), com::S_OK);
    EXPECT_EQ(memory_.read_u64(V2).value_or(0), 35'830'000u);
    // SetPositions(0, Absolute): ReturnTime is added for the renderers.
    (void)memory_.write_u64(V1, 0);
    EXPECT_EQ(seek(14, {V1, 1, 0, 0}), com::S_OK);
    EXPECT_EQ(last_set_flags_, 9u);
    EXPECT_EQ(seek(12, {V2}), com::S_OK);
    EXPECT_EQ(memory_.read_u64(V2).value_or(1), 0u);
    (void)invoke(&ComCoreManager::filter_graph_release, {graph});
}

TEST_P(FilterGraphConnectTest, FinalReleaseWhileRunningStopsThenDisconnectsAStoppedGraph) {
    // Planet Coaster releases the graph while it runs. The WebM Vorbis
    // decoder's input pin disconnect calls IFilterGraph::Disconnect on its
    // downstream peer and asserts SUCCEEDED (webmvorbisdecoderoutpin.cc
    // OnInpinDisconnect): the graph must be stopped by then, renderers
    // first, as IMediaControl::Stop does.
    make_file(4096);
    mgr_.set_audio_renderer(&audio_);
    build_game_filters(true);
    filters_[A].disconnect_downstream_through_graph = true;
    const GuestAddr graph = create_graph();
    for (const int f : {S, V, A, R}) add_to_graph(graph, f);
    ASSERT_EQ(render_file(graph), com::S_OK);
    const GuestAddr control = qi(graph, IID_IMEDIACONTROL);
    (void)invoke(&ComCoreManager::filter_graph_media_control_pause, {control});
    const GuestAddr dsound = find(graph, u"Default DirectSound Device");
    ASSERT_EQ(call(pins_of(dsound).at(0), Iface::Pin, 14, {}), com::S_OK);
    ASSERT_EQ(invoke(&ComCoreManager::filter_graph_media_control_run, {control}), com::S_OK);
    log_.clear();
    (void)invoke(&ComCoreManager::filter_graph_release, {control});
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_release, {graph}), 0u);
    const auto at = [&](const std::string& entry) {
        return std::find(log_.begin(), log_.end(), entry) - log_.begin();
    };
    EXPECT_LT(at("Frontier Texture Renderer.Stop"), at("VP8 Decoder.Stop"));
    EXPECT_LT(at("VP8 Decoder.Stop"), at("WebM Splitter.Stop"));
    EXPECT_LT(at("Vorbis Decoder.Stop"), at("WebM Splitter.Stop"));
    EXPECT_LT(at("WebM Splitter.Stop"), at("Vorbis Decoder.Vorbis.Disconnect"));
    ASSERT_FALSE(graph_disconnect_hrs_.empty());
    for (const u32 hr : graph_disconnect_hrs_) EXPECT_GE(static_cast<i32>(hr), 0) << std::hex << hr;
    EXPECT_EQ(pin(A, u"PCM (IEEE Float)").peer, 0u);
}

TEST_P(FilterGraphConnectTest, StopReleasesAnUpstreamThreadBlockedOnTheRenderer) {
    // CBaseInputPin::Inactive decommits the input pin's allocator: a decoder
    // thread waiting in GetBuffer gets VFW_E_NOT_COMMITTED (measured,
    // dshow_allocator_probe), and one held in the paused renderer's Receive
    // is let go, so the decoder's own Stop can join its thread.
    make_file(4096);
    mgr_.set_audio_renderer(&audio_);
    build_game_filters(true);
    const GuestAddr graph = create_graph();
    for (const int f : {S, V, A, R}) add_to_graph(graph, f);
    ASSERT_EQ(render_file(graph), com::S_OK);
    const GuestAddr dsound = find(graph, u"Default DirectSound Device");
    const GuestAddr in = pins_of(dsound).at(0);
    auto m = make_marshaler();
    const GuestAddr mem = mgr_.dshow_host().query(m, in, ds::sdk::IID_IMemInputPin);
    constexpr GuestAddr X = DATA_BASE + 0x7500;
    ASSERT_EQ(call(mem, Iface::MemInputPin, 3, {X}), com::S_OK);
    const GuestAddr allocator = out_ptr(X);
    for (u32 i = 0; i < 16; i += 4) (void)memory_.write_u32(X + 16 + i, 0);
    (void)memory_.write_u32(X + 16, 1);  // one buffer
    (void)memory_.write_u32(X + 20, 4096);
    (void)memory_.write_u32(X + 24, 1);
    ASSERT_EQ(call(allocator, Iface::MemAllocator, 3, {X + 16, X + 32}), com::S_OK);
    ASSERT_EQ(call(mem, Iface::MemInputPin, 4, {allocator, 0}), com::S_OK);
    ASSERT_EQ(call(allocator, Iface::MemAllocator, 5, {}), com::S_OK);  // the decoder's Active
    ASSERT_EQ(call(allocator, Iface::MemAllocator, 7, {X + 48, 0, 0, 0}), com::S_OK);
    const GuestAddr sample = out_ptr(X + 48);
    ASSERT_EQ(call(sample, Iface::MediaSample, 12, {4096}), com::S_OK);
    const GuestAddr control = qi(graph, IID_IMEDIACONTROL);
    (void)invoke(&ComCoreManager::filter_graph_media_control_pause, {control});
    // The decoder's streaming thread: the preroll Receive (held while
    // paused), then a GetBuffer that waits for the pool's only buffer.
    std::atomic<u32> receive_hr{0xFFFFFFFFu};
    std::atomic<u32> getbuffer_hr{0xFFFFFFFFu};
    std::thread streaming([&] {
        Marshaler tm(memory_);
        tm.attach_cpu(cpu_);
        ds::DShowHost& host = mgr_.dshow_host();
        receive_hr = host.call(tm, mem, Iface::MemInputPin, 6, {sample});
        const GuestAddr out = X + 64;
        getbuffer_hr = host.call(tm, allocator, Iface::MemAllocator, 7, {out, 0, 0, 0});
    });
    // A second upstream thread already waiting in GetBuffer (the pool's
    // only buffer is out).
    std::atomic<u32> waiting_hr{0xFFFFFFFFu};
    std::thread waiter([&] {
        Marshaler tm(memory_);
        tm.attach_cpu(cpu_);
        waiting_hr = mgr_.dshow_host().call(tm, allocator, Iface::MemAllocator, 7, {X + 72, 0, 0, 0});
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(receive_hr.load(), 0xFFFFFFFFu);  // held as the preroll
    EXPECT_EQ(waiting_hr.load(), 0xFFFFFFFFu);  // blocked on the pool
    EXPECT_EQ(invoke(&ComCoreManager::filter_graph_media_control_stop, {control}), com::S_OK);
    waiter.join();
    EXPECT_EQ(waiting_hr.load(), 0x80040211u);  // released by the Decommit
    // Stop let the Receive go; the decoder still holds the sample, so its
    // next GetBuffer waits until the renderer's Stop decommitted the pool.
    streaming.join();
    EXPECT_NE(receive_hr.load(), 0xFFFFFFFFu);
    EXPECT_EQ(getbuffer_hr.load(), 0x80040211u);  // VFW_E_NOT_COMMITTED
    // A stopped renderer refuses samples.
    EXPECT_EQ(call(mem, Iface::MemInputPin, 6, {sample}), 0x80040227u);
    EXPECT_FALSE(audio_.last_push->started);
    (void)call(sample, Iface::MediaSample, 2, {});
    (void)call(allocator, Iface::MemAllocator, 2, {});
    (void)call(mem, Iface::MemInputPin, 2, {});
    (void)invoke(&ComCoreManager::filter_graph_release, {graph});
}

// -------------------------------------------------------- host objects

TEST_P(FilterGraphConnectTest, FileSourceAsyncFollowsTheStockReader) {
    make_file(2627);  // 5 * 512 + 67: a short tail block
    auto m = make_marshaler();
    ds::DShowHost& host = mgr_.dshow_host();
    const GuestAddr f = ds::create_async_reader(host, m);
    ASSERT_NE(f, 0u);
    const GuestAddr src = host.query(m, f, ds::sdk::IID_IFileSourceFilter);
    ASSERT_NE(src, 0u);
    EXPECT_TRUE(pins_of(f).empty());  // measured: no pin before Load
    constexpr GuestAddr Q = DATA_BASE + 0x7800;
    (void)memory_.write_u64(Q, 0xCAFE);
    EXPECT_EQ(call(src, Iface::FileSourceFilter, 4, {Q, 0}), com::S_OK);
    EXPECT_EQ(ptr(Q), 0u);
    put_wide(NAME_ADDR, u"C:\\movies\\nope.webm");
    EXPECT_EQ(call(src, Iface::FileSourceFilter, 3, {NAME_ADDR, 0}), kFileNotFound);
    put_wide(NAME_ADDR, u"C:\\movies\\clip.webm");
    EXPECT_EQ(call(src, Iface::FileSourceFilter, 3, {NAME_ADDR, 0}), com::S_OK);
    EXPECT_EQ(call(src, Iface::FileSourceFilter, 3, {NAME_ADDR, 0}), kEUnexpected);
    EXPECT_EQ(call(src, Iface::FileSourceFilter, 4, {Q, Q + 16}), com::S_OK);
    EXPECT_EQ(wstr(out_ptr(Q)), u"C:\\movies\\clip.webm");
    const MediaType cur = ds::read_media_type(m, Q + 16);
    EXPECT_TRUE(guid_eq(cur.major, ds::sdk::MEDIATYPE_Stream));
    EXPECT_EQ(cur.sub.Data1, 0u);
    EXPECT_TRUE(cur.fixed);
    EXPECT_EQ(cur.sample_size, 1u);
    EXPECT_TRUE(cur.format_bytes.empty());
    // No IAMFilterMiscFlags / IReferenceClock / IAsyncReader on the filter.
    EXPECT_EQ(host.query(m, f, ds::sdk::IID_IAMFilterMiscFlags), 0u);
    EXPECT_EQ(host.query(m, f, ds::sdk::IID_IReferenceClock), 0u);
    EXPECT_EQ(host.query(m, f, ds::sdk::IID_IAsyncReader), 0u);

    const std::vector<GuestAddr> pins = pins_of(f);
    ASSERT_EQ(pins.size(), 1u);
    const GuestAddr out = pins[0];
    EXPECT_EQ(pin_name(out), u"Output");
    // Two identical Stream types; QueryAccept takes any Stream type only.
    ASSERT_EQ(call(out, Iface::Pin, 12, {Q}), com::S_OK);
    const GuestAddr e = out_ptr(Q);
    EXPECT_EQ(call(e, Iface::EnumMediaTypes, 3, {3, Q + 8, Q + 32}), com::S_FALSE);
    EXPECT_EQ(memory_.read_u32(Q + 32).value_or(0), 2u);
    (void)call(e, Iface::EnumMediaTypes, 2, {});
    MediaType q;
    q.major = ds::sdk::MEDIATYPE_Stream;
    const GuestAddr mt = ds::alloc_media_type(host, m, q);
    EXPECT_EQ(call(out, Iface::Pin, 11, {mt}), com::S_OK);
    q.major = ds::sdk::MEDIATYPE_Video;
    (void)ds::write_media_type(host, m, mt, q);
    EXPECT_EQ(call(out, Iface::Pin, 11, {mt}), com::S_FALSE);
    EXPECT_EQ(call(out, Iface::Pin, 6, {Q}), kVfwENotConnected);

    const GuestAddr reader = host.query(m, out, ds::sdk::IID_IAsyncReader);
    ASSERT_NE(reader, 0u);
    EXPECT_EQ(call(reader, Iface::AsyncReader, 8, {Q, Q + 8}), com::S_OK);
    EXPECT_EQ(memory_.read_u64(Q).value_or(0), 2627u);
    EXPECT_EQ(memory_.read_u64(Q + 8).value_or(0), 2627u);
    constexpr GuestAddr B = DATA_BASE + 0x8000;
    EXPECT_EQ(call(reader, Iface::AsyncReader, 7, {3, 5, B}), com::S_OK);  // unaligned is fine
    EXPECT_EQ(memory_.read_u8(B).value_or(0), file_bytes_[3]);
    EXPECT_EQ(call(reader, Iface::AsyncReader, 7, {2625, 8, B}), com::S_FALSE);
    EXPECT_EQ(call(reader, Iface::AsyncReader, 7, {3000, 4, B}), com::S_FALSE);
    // SyncRead through the guest ABI: the LONGLONG position is two stack
    // slots on x86, one register on x64.
    if (x64_) {
        put_args({reader, 0x200, 4, B});
    } else {
        put_args({reader, 0x200, 0, 4, B});
    }
    {
        auto tm = make_marshaler();
        EXPECT_EQ(host.thunk(tm, Iface::AsyncReader, 7), com::S_OK);
    }
    EXPECT_EQ(memory_.read_u8(B).value_or(0), file_bytes_[0x200]);

    // RequestAllocator: the reader asks for 512-byte alignment (measured).
    for (u32 i = 0; i < 16; i += 4) (void)memory_.write_u32(Q + i, 0);
    (void)memory_.write_u32(Q, 4);
    (void)memory_.write_u32(Q + 4, 1024);
    (void)memory_.write_u32(Q + 8, 1);
    ASSERT_EQ(call(reader, Iface::AsyncReader, 3, {0, Q, Q + 16}), com::S_OK);
    const GuestAddr allocator = out_ptr(Q + 16);
    ASSERT_EQ(call(allocator, Iface::MemAllocator, 4, {Q + 32}), com::S_OK);
    EXPECT_EQ(memory_.read_u32(Q + 40).value_or(0), 512u);
    ASSERT_EQ(call(allocator, Iface::MemAllocator, 5, {}), com::S_OK);
    GuestAddr s1 = 0, s2 = 0;
    ASSERT_EQ(call(allocator, Iface::MemAllocator, 7, {Q + 48, 0, 0, 0}), com::S_OK);
    s1 = out_ptr(Q + 48);
    ASSERT_EQ(call(allocator, Iface::MemAllocator, 7, {Q + 48, 0, 0, 0}), com::S_OK);
    s2 = out_ptr(Q + 48);
    const auto set_time = [&](GuestAddr s, i64 a, i64 b) {
        (void)memory_.write_u64(Q + 64, static_cast<u64>(a * kUnits));
        (void)memory_.write_u64(Q + 72, static_cast<u64>(b * kUnits));
        (void)call(s, Iface::MediaSample, 6, {Q + 64, Q + 72});
    };
    set_time(s1, 0, 512);
    EXPECT_EQ(call(reader, Iface::AsyncReader, 4, {s1, 11}), com::S_OK);
    set_time(s2, 1024, 2048);
    EXPECT_EQ(call(reader, Iface::AsyncReader, 4, {s2, 22}), com::S_OK);
    EXPECT_EQ(call(reader, Iface::AsyncReader, 5, {200, Q + 80, Q + 88}), com::S_OK);
    EXPECT_EQ(out_ptr(Q + 80), s1);
    EXPECT_EQ(ptr(Q + 88), 11u);
    EXPECT_EQ(call(s1, Iface::MediaSample, 11, {}), 512u);
    EXPECT_EQ(call(reader, Iface::AsyncReader, 5, {200, Q + 80, Q + 88}), com::S_OK);
    EXPECT_EQ(ptr(Q + 88), 22u);
    EXPECT_EQ(call(s2, Iface::MediaSample, 11, {}), 1024u);
    (void)call(s2, Iface::MediaSample, 3, {Q + 96});
    EXPECT_EQ(memory_.read_u8(out_ptr(Q + 96)).value_or(0), file_bytes_[1024]);
    // Idle: VFW_E_TIMEOUT, NULL sample, user 0.
    (void)memory_.write_u64(Q + 88, 7);
    EXPECT_EQ(call(reader, Iface::AsyncReader, 5, {50, Q + 80, Q + 88}), kVfwETimeout);
    EXPECT_EQ(ptr(Q + 80), 0u);
    EXPECT_EQ(ptr(Q + 88), 0u);
    // Misaligned / too long / past the end.
    set_time(s1, 512, 600);
    EXPECT_EQ(call(reader, Iface::AsyncReader, 4, {s1, 99}), kVfwEBadAlign);
    set_time(s1, 0, 2048);
    EXPECT_EQ(call(reader, Iface::AsyncReader, 4, {s1, 98}), kEInvalidArgHr);
    set_time(s1, 3072, 3584);
    EXPECT_EQ(call(reader, Iface::AsyncReader, 4, {s1, 44}), kHandleEof);
    // The tail: 67 bytes, stop time pulled in to the aligned-up end.
    set_time(s1, 2560, 3072);
    EXPECT_EQ(call(reader, Iface::AsyncReader, 4, {s1, 33}), com::S_OK);
    EXPECT_EQ(call(reader, Iface::AsyncReader, 5, {200, Q + 80, Q + 88}), com::S_OK);
    EXPECT_EQ(call(s1, Iface::MediaSample, 11, {}), 67u);
    (void)call(s1, Iface::MediaSample, 5, {Q + 64, Q + 72});
    EXPECT_EQ(static_cast<i64>(memory_.read_u64(Q + 72).value_or(0)), 3072 * kUnits);
    // Flushing.
    EXPECT_EQ(call(reader, Iface::AsyncReader, 9, {}), com::S_OK);
    set_time(s1, 0, 512);
    EXPECT_EQ(call(reader, Iface::AsyncReader, 4, {s1, 55}), kVfwEWrongState);
    (void)memory_.write_u64(Q + 88, 7);
    EXPECT_EQ(call(reader, Iface::AsyncReader, 5, {200, Q + 80, Q + 88}), kVfwEWrongState);
    EXPECT_EQ(ptr(Q + 88), 7u);  // untouched
    EXPECT_EQ(call(reader, Iface::AsyncReader, 10, {}), com::S_OK);
    EXPECT_EQ(call(reader, Iface::AsyncReader, 4, {s1, 66}), com::S_OK);
    EXPECT_EQ(call(reader, Iface::AsyncReader, 9, {}), com::S_OK);
    EXPECT_EQ(call(reader, Iface::AsyncReader, 5, {200, Q + 80, Q + 88}), kVfwETimeout);
    EXPECT_EQ(out_ptr(Q + 80), s1);
    EXPECT_EQ(ptr(Q + 88), 66u);
    (void)call(reader, Iface::AsyncReader, 10, {});
    (void)call(s1, Iface::MediaSample, 2, {});
    (void)call(s2, Iface::MediaSample, 2, {});
    (void)call(allocator, Iface::MemAllocator, 6, {});
    (void)call(allocator, Iface::MemAllocator, 2, {});
    (void)call(reader, Iface::AsyncReader, 2, {});
    (void)call(src, Iface::FileSourceFilter, 2, {});
    EXPECT_EQ(call(f, Iface::BaseFilter, 2, {}), 0u);
}

TEST_P(FilterGraphConnectTest, MemoryAllocatorFollowsCMemAllocator) {
    auto m = make_marshaler();
    ds::DShowHost& host = mgr_.dshow_host();
    const GuestAddr al = ds::create_memory_allocator(host, m);
    ASSERT_NE(al, 0u);
    constexpr GuestAddr P = DATA_BASE + 0x7800;
    for (u32 i = 0; i < 16; i += 4) (void)memory_.write_u32(P + i, 7);
    EXPECT_EQ(call(al, Iface::MemAllocator, 4, {P}), com::S_OK);
    // Measured: 64-bit quartz writes zeros before SetProperties, 32-bit
    // leaves the structure alone.
    EXPECT_EQ(memory_.read_u32(P).value_or(0), x64_ ? 0u : 7u);
    EXPECT_EQ(call(al, Iface::MemAllocator, 5, {}), kVfwESizeNotSet);
    const auto props = [&](i32 n, i32 size, i32 align, i32 prefix, u32 expect_hr,
                           std::array<i32, 4> expect) {
        (void)memory_.write_u32(P, static_cast<u32>(n));
        (void)memory_.write_u32(P + 4, static_cast<u32>(size));
        (void)memory_.write_u32(P + 8, static_cast<u32>(align));
        (void)memory_.write_u32(P + 12, static_cast<u32>(prefix));
        for (u32 i = 16; i < 32; i += 4) (void)memory_.write_u32(P + i, 0xFFFFFFFFu);
        EXPECT_EQ(call(al, Iface::MemAllocator, 3, {P, P + 16}), expect_hr) << n << " " << size << " " << align;
        for (u32 i = 0; i < 4; ++i) {
            EXPECT_EQ(static_cast<i32>(memory_.read_u32(P + 16 + i * 4).value_or(0)), expect[i]) << i;
        }
    };
    props(2, 1000, 1, 0, com::S_OK, {2, 1000, 1, 0});
    props(2, 1000, 16, 0, com::S_OK, {2, 1008, 16, 0});
    props(2, 1000, 16, 8, com::S_OK, {2, 1000, 16, 8});
    props(2, 1000, 3, 0, com::S_OK, {2, 1002, 3, 0});
    props(2, 1000, 0, 0, kVfwEBadAlign, {0, 0, 0, 0});
    props(2, 1000, 65536, 0, com::S_OK, {2, 65536, 65536, 0});
    props(2, 1000, 131072, 0, kVfwEBadAlign, {0, 0, 0, 0});
    props(2, 1000, 16, 8, com::S_OK, {2, 1000, 16, 8});
    EXPECT_EQ(call(al, Iface::MemAllocator, 5, {}), com::S_OK);
    EXPECT_EQ(call(al, Iface::MemAllocator, 5, {}), com::S_OK);
    props(3, 1000, 1, 0, kVfwEAlreadyCommitted, {0, 0, 0, 0});
    ASSERT_EQ(call(al, Iface::MemAllocator, 7, {P + 32, 0, 0, 0}), com::S_OK);
    const GuestAddr a = out_ptr(P + 32);
    ASSERT_EQ(call(al, Iface::MemAllocator, 7, {P + 32, 0, 0, 0}), com::S_OK);
    const GuestAddr b = out_ptr(P + 32);
    (void)call(a, Iface::MediaSample, 3, {P + 40});
    (void)call(b, Iface::MediaSample, 3, {P + 48});
    const GuestAddr pa = out_ptr(P + 40), pb = out_ptr(P + 48);
    EXPECT_EQ((pa - 8u) % 16u, 0u);  // buffer = aligned base + prefix
    EXPECT_EQ(pb > pa ? pb - pa : pa - pb, 1008u);
    EXPECT_EQ(call(a, Iface::MediaSample, 4, {}), 1000u);
    EXPECT_EQ(call(a, Iface::MediaSample, 11, {}), 1000u);
    // Both out: NOWAIT is VFW_E_TIMEOUT with NULL.
    (void)memory_.write_u64(P + 32, 1);
    EXPECT_EQ(call(al, Iface::MemAllocator, 7, {P + 32, 0, 0, 4}), kVfwETimeout);
    EXPECT_EQ(ptr(P + 32), 0u);
    // A blocked GetBuffer gets the sample another thread releases.
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Marshaler tm(memory_);
        tm.attach_cpu(cpu_);
        (void)mgr_.dshow_host().call(tm, a, Iface::MediaSample, 2, {});
    });
    ASSERT_EQ(call(al, Iface::MemAllocator, 7, {P + 32, 0, 0, 0}), com::S_OK);
    releaser.join();
    const GuestAddr c = out_ptr(P + 32);
    EXPECT_EQ(c, a);
    // Times and flags reset on reuse; the data length does not.
    EXPECT_EQ(call(c, Iface::MediaSample, 5, {P + 56, P + 64}), 0x80040249u);
    EXPECT_EQ(call(c, Iface::MediaSample, 7, {}), com::S_FALSE);
    // Decommit with samples out: S_OK, they stay usable; GetBuffer refused.
    EXPECT_EQ(call(al, Iface::MemAllocator, 6, {}), com::S_OK);
    EXPECT_EQ(call(b, Iface::MediaSample, 3, {P + 48}), com::S_OK);
    (void)memory_.write_u64(P + 32, 1);
    EXPECT_EQ(call(al, Iface::MemAllocator, 7, {P + 32, 0, 0, 0}), kVfwENotCommitted);
    EXPECT_EQ(ptr(P + 32), 0u);
    props(2, 1000, 1, 0, kVfwEBuffersOutstanding, {0, 0, 0, 0});
    EXPECT_EQ(call(b, Iface::MediaSample, 1, {}), 2u);
    EXPECT_EQ(call(b, Iface::MediaSample, 2, {}), 1u);
    (void)call(b, Iface::MediaSample, 2, {});
    (void)call(c, Iface::MediaSample, 2, {});
    props(2, 1000, 1, 0, com::S_OK, {2, 1000, 1, 0});
    EXPECT_EQ(call(al, Iface::MemAllocator, 2, {}), 0u);
}

TEST_P(FilterGraphConnectTest, SystemClockAdviseFollowsTheStockClock) {
    auto m = make_marshaler();
    ds::DShowHost& host = mgr_.dshow_host();
    const GuestAddr clock = ds::create_system_clock(host, m);
    ASSERT_NE(clock, 0u);
    constexpr GuestAddr T = DATA_BASE + 0x7800;
    ASSERT_EQ(call(clock, Iface::ReferenceClock, 3, {T}), com::S_OK);
    const i64 t0 = static_cast<i64>(memory_.read_u64(T).value_or(0));
    const Handle ev = sync_.create_event(false, false, "");
    ASSERT_NE(ev, 0u);
    // A time already past fires at once.
    EXPECT_EQ(call(clock, Iface::ReferenceClock, 4, {static_cast<u64>(t0), static_cast<u64>(-kUnits), ev, T + 8}),
              com::S_OK);
    EXPECT_NE(ptr(T + 8), 0u);
    EXPECT_EQ(sync_.wait_for_single_object(ev, 500), 0u);
    // +100 ms fires after it, and is then gone (Unadvise S_FALSE).
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(call(clock, Iface::ReferenceClock, 4, {static_cast<u64>(t0), 1'000'000, ev, T + 8}), com::S_OK);
    const u64 cookie = ptr(T + 8);
    EXPECT_EQ(sync_.wait_for_single_object(ev, 2000), 0u);
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(80));
    EXPECT_EQ(call(clock, Iface::ReferenceClock, 6, {cookie}), com::S_FALSE);
    // Pending: Unadvise S_OK and it never fires.
    EXPECT_EQ(call(clock, Iface::ReferenceClock, 4, {static_cast<u64>(t0), 50'000'000, ev, T + 8}), com::S_OK);
    EXPECT_EQ(call(clock, Iface::ReferenceClock, 6, {ptr(T + 8)}), com::S_OK);
    EXPECT_NE(sync_.wait_for_single_object(ev, 100), 0u);
    // Measured refusals.
    EXPECT_EQ(call(clock, Iface::ReferenceClock, 4, {0, 0, 0, T + 8}), kEInvalidArgHr);
    EXPECT_EQ(call(clock, Iface::ReferenceClock, 4, {static_cast<u64>(-1), 0, ev, T + 8}), kEInvalidArgHr);
    const Handle sem = sync_.create_semaphore(0, 100, "");
    EXPECT_EQ(call(clock, Iface::ReferenceClock, 5, {static_cast<u64>(t0), 0, sem, T + 8}), kEInvalidArgHr);
    EXPECT_EQ(call(clock, Iface::ReferenceClock, 6, {12345}), com::S_FALSE);
    // Periodic every 50 ms: about five releases in 275 ms.
    ASSERT_EQ(call(clock, Iface::ReferenceClock, 3, {T}), com::S_OK);
    EXPECT_EQ(call(clock, Iface::ReferenceClock, 5, {memory_.read_u64(T).value_or(0), 500'000, sem, T + 8}),
              com::S_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(275));
    EXPECT_EQ(call(clock, Iface::ReferenceClock, 6, {ptr(T + 8)}), com::S_OK);
    i32 previous = 0;
    ASSERT_TRUE(sync_.release_semaphore(sem, 1, &previous));
    EXPECT_GE(previous, 4);
    EXPECT_LE(previous, 7);
    EXPECT_EQ(call(clock, Iface::ReferenceClock, 2, {}), 0u);
}

INSTANTIATE_TEST_SUITE_P(Bitness, FilterGraphConnectTest, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? "x64" : "x86";
                         });
