// mfplay.dll contract (MFPCreateMediaPlayer, IMFPMediaPlayer, IMFPMediaItem,
// the IMFPMediaPlayerCallback events). Expected values come from the Windows
// SDK 10.0.26100 (mfplay.lib exports and x86 stdcall sizes, mfplay.h layouts,
// mferror.h HRESULTs), from tools/oracle_probe/mfplay_probe.c recorded on
// Windows (tests/oracle_data/mfplay_probe{64,32}.expected, x86 == x64; the
// probe row each check mirrors is quoted), and from Microsoft Learn for the
// success path the oracle cannot reach (it has no audio endpoint). Never from
// running Vitreo. Every case runs for x64 and x86.
#include <gtest/gtest.h>

#include <vitreo/common.hpp>
#include "audio/contracts/media_player.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/media/mfplay/mfplay.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::mfplay;

namespace {

// winerror.h / mferror.h
constexpr u32 S_OK_HR = 0, S_FALSE_HR = 1;
constexpr u32 E_NOTIMPL_HR = 0x80004001u, E_NOINTERFACE_HR = 0x80004002u, E_POINTER_HR = 0x80004003u;
constexpr u32 E_INVALIDARG_HR = 0x80070057u;
constexpr u32 FILE_NOT_FOUND_HR = 0x80070002u, PATH_NOT_FOUND_HR = 0x80070003u;
constexpr u32 INVALID_WINDOW_HANDLE_HR = 0x80070578u;
constexpr u32 CLASS_E_CLASSNOTAVAILABLE_HR = 0x80040111u;
constexpr u32 MF_E_INVALIDREQUEST_HR = 0xC00D36B2u;
constexpr u32 MF_E_INVALIDINDEX_HR = 0xC00D36BFu;
constexpr u32 MF_E_UNSUPPORTED_SCHEME_HR = 0xC00D36C3u;
constexpr u32 MF_E_UNSUPPORTED_BYTESTREAM_TYPE_HR = 0xC00D36C4u;
constexpr u32 MF_E_UNSUPPORTED_RATE_HR = 0xC00D36D0u;
constexpr u32 MF_E_NOT_FOUND_HR = 0xC00D36D5u;
constexpr u32 MF_E_INVALID_POSITION_HR = 0xC00D36E5u;
constexpr u32 MF_E_ATTRIBUTENOTFOUND_HR = 0xC00D36E6u;
constexpr u32 MF_E_OUT_OF_RANGE_HR = 0xC00D3702u;
constexpr u32 MF_E_SHUTDOWN_HR = 0xC00D3E85u;
constexpr u32 MF_E_TOPO_UNSUPPORTED_HR = 0xC00D5214u;

constexpr u16 VT_EMPTY = 0, VT_UI4 = 19, VT_I8 = 20, VT_UI8 = 21, VT_CLSID = 72;
constexpr u32 F_ONE = 0x3f800000u;   // 1.0f
constexpr u32 F_MINUS7 = 0xc0e00000u; // -7.0f, the probe's untouched marker

struct G { u32 a; u16 b, c; u8 d[8]; };
// mfplay.h / unknwn.h / mfidl.h / mfapi.h
constexpr G IID_IUnknown_G{0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr G IID_IMFPMediaPlayer_G{0xA714590A, 0x58AF, 0x430a, {0x85, 0xBF, 0x44, 0xF5, 0xEC, 0x83, 0x8D, 0x85}};
constexpr G IID_IMFPMediaItem_G{0x90EB3E6B, 0xECBF, 0x45cc, {0xB1, 0xDA, 0xC6, 0xFE, 0x3E, 0xA7, 0x0D, 0x57}};
constexpr G IID_IMFPMediaPlayerCallback_G{0x766C8FFB, 0x5FDB, 0x4fea, {0xA2, 0x8D, 0xB9, 0x12, 0x99, 0x6F, 0x51, 0xBD}};
constexpr G POS_100NS{0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
constexpr G MF_PD_DURATION_G{0x6c990d33, 0xbb8e, 0x477a, {0x85, 0x98, 0x0d, 0x5d, 0x96, 0xfc, 0xd8, 0x8a}};
constexpr G MF_SD_LANGUAGE_G{0x00af2180, 0xbdc2, 0x423c, {0xab, 0xca, 0xf5, 0x03, 0x59, 0x3b, 0xc1, 0x21}};
constexpr G MF_MT_MAJOR_TYPE_G{0x48eba18e, 0xf8c9, 0x4687, {0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f}};
constexpr G MFMediaType_Audio_G{0x73647561, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

/// Host media backend double: what the item opened, and what playback did.
struct FakeMediaState {
    bool open_ok{true};
    bool has_audio{true};
    bool has_video{false};
    u32 width{0}, height{0};
    i64 duration{2500000};  // the probe's 0.25 s WAV
    int runs{0}, pauses{0}, stops{0};
    i64 last_seek{-1};
    i64 position{0};
    i32 volume{0}, balance{0};
    bool completed{false};
};

class FakeMedia final : public audio::IMediaPlayer {
public:
    explicit FakeMedia(FakeMediaState& s) : s_(s) {}
    bool open(std::string_view) override { return s_.open_ok; }
    audio::MediaStartResult run() override { ++s_.runs; return audio::MediaStartResult::Started; }
    void pause() override { ++s_.pauses; }
    void stop() override { ++s_.stops; }
    bool ready() const override { return true; }
    bool failed() const override { return false; }
    bool take_completed() override { const bool c = s_.completed; s_.completed = false; return c; }
    void set_volume(i32 v) override { s_.volume = v; }
    void set_balance(i32 v) override { s_.balance = v; }
    bool seek_100ns(i64 p) override { s_.last_seek = p; s_.position = p; return true; }
    i64 position_100ns() const override { return s_.position; }
    i64 duration_100ns() const override { return s_.duration; }
    bool has_video() const override { return s_.has_video; }
    bool has_audio() const override { return s_.has_audio; }
    void native_video_size(u32& w, u32& h) const override { w = s_.width; h = s_.height; }
    bool copy_video_frame(std::vector<u8>& rgba, u32& w, u32& h, u64& idx) const override {
        if (!s_.has_video) return false;
        w = s_.width; h = s_.height; idx = static_cast<u64>(s_.position);
        rgba.assign(static_cast<usize>(w) * h * 4, 0x80);
        return true;
    }
private:
    FakeMediaState& s_;
};

struct EventRec {
    u32 type, hr, state;
    u64 player, store, item, extra;
    bool service;
};

class MfplayFixture : public ::testing::Test {
public:
    static constexpr GuestAddr DATA = 0x10000;
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr ALLOC = 0x200000;     // alloc_guest
    static constexpr GuestAddr TASK = 0x300000;      // CoTaskMemAlloc
    static constexpr GuestAddr THUNK_BASE = 0x70000000;
    static constexpr GuestAddr CB = 0x40000;         // guest IMFPMediaPlayerCallback
    static constexpr GuestAddr CB_VT = 0x40100;
    static constexpr GuestAddr EFFECT = 0x40200;     // guest IUnknown (an "effect")
    static constexpr GuestAddr EFFECT_VT = 0x40300;
    static constexpr u64 FN_QI = 0x5000, FN_ADDREF = 0x5001, FN_RELEASE = 0x5002, FN_EVENT = 0x5003;
    static constexpr u64 FN_EFF_ADDREF = 0x6001, FN_EFF_RELEASE = 0x6002;
    static constexpr u64 EVENT_HWND = 0x1234;
    static constexpr u64 VIDEO_HWND = 0x5678;

    void setup(bool is64) {
        ASSERT_TRUE(memory.map_region(DATA, 0x20000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(CB, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(ALLOC, 0x40000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(TASK, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(THUNK_BASE, 0x20000, MemoryProtection::ReadWrite).is_ok());
        if (is64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        marshaler.attach_cpu(cpu);
        put_vtable(CB, CB_VT, {FN_QI, FN_ADDREF, FN_RELEASE, FN_EVENT});
        put_vtable(EFFECT, EFFECT_VT, {FN_QI, FN_EFF_ADDREF, FN_EFF_RELEASE});

        dir = std::filesystem::temp_directory_path() /
              ("vitreo_mfplay_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(dir);
        std::ofstream(dir / "a.wav") << "RIFF....WAVE";
        std::ofstream(dir / "junk.wav") << "not media";

        MfPlayManager::Host host;
        host.call_guest = [this](GuestAddr fn, const u64* a, usize n) { return guest(fn, a, n, false); };
        host.call_guest_service = [this](GuestAddr fn, const u64* a, usize n) { return guest(fn, a, n, true); };
        host.create_event_window = [this](GuestAddr proc) -> u64 {
            wndproc = proc;
            ++windows_created;
            return EVENT_HWND;
        };
        host.destroy_window = [this](u64 h) { destroyed.push_back(h); };
        host.def_window_proc = [](u64, u32, u64, u64) -> u64 { return 0; };
        host.post_message = [this](u64 h, u32 msg, u64, u64) { posted.push_back({h, msg}); return true; };
        host.current_thread_id = [] { return 1u; };
        host.resolve_path = [this](const std::string& p) -> std::optional<std::string> {
            // C:\m\<name> -> the temp dir; anything else under C:\nodir\ is a
            // directory that does not exist.
            if (p.rfind("C:\\m\\", 0) == 0) return (dir / p.substr(5)).string();
            if (p.rfind("C:\\nodir\\", 0) == 0) return (dir / "nodir" / p.substr(9)).string();
            return std::nullopt;
        };
        host.create_media = [this]() -> std::unique_ptr<audio::IMediaPlayer> {
            return std::make_unique<FakeMedia>(media);
        };
        host.present_video = [this](u64 h, const u8*, i32 w, i32 hh) { presented.push_back({h, w, hh}); };
        host.hide_video = [this](u64 h) { hidden.push_back(h); };
        host.client_size = [](u64, i32& w, i32& h) { w = 640; h = 360; return true; };
        host.alloc_guest = [this](u64 size) { const GuestAddr a = alloc_next; alloc_next += size; return a; };
        host.task_alloc = [this](u64 size) { const GuestAddr a = task_next; task_next += (size + 15) & ~u64{15}; return a; };
        mgr.set_host(std::move(host));
        mgr.set_clock_enabled(false);
        mgr.set_memory(&memory);
        mgr.set_thunk_registry(&thunks);
        exports = create_mfplay_thunks(mgr);
        DllExports copy = exports;
        ASSERT_TRUE(thunks.register_dll("mfplay.dll", std::move(copy)).is_ok());
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    u64 guest(GuestAddr fn, const u64* a, usize n, bool service) {
        if (fn == FN_ADDREF) return ++cb_refs;
        if (fn == FN_RELEASE) return --cb_refs;
        if (fn == FN_EFF_ADDREF) return ++effect_refs;
        if (fn == FN_EFF_RELEASE) return --effect_refs;
        if (fn == FN_EVENT) {
            EXPECT_EQ(n, 2u);
            EXPECT_EQ(a[0], CB);
            // Default mode: never inside the call that caused it (measured
            // "incall=0"). The service thread runs beside the test thread.
            if (!service) EXPECT_FALSE(in_api) << "OnMediaPlayerEvent ran inside an API call";
            const GuestAddr b = static_cast<GuestAddr>(a[1]);
            const GuestAddr ps = ptr_size();
            const GuestAddr store = b + (cpu.is_64bit() ? 24u : 16u);
            EventRec r{};
            r.type = rd32(b);
            r.hr = rd32(b + 4);
            r.player = rd_ptr(b + 8);
            r.state = rd32(b + 8 + ps);
            r.store = rd_ptr(store);
            r.item = rd_ptr(store + ps);
            r.extra = r.type == static_cast<u32>(EventType::RateSet) ? rd32(store + 2 * ps)
                                                                       : rd_ptr(store + 2 * ps);
            r.service = service;
            events.push_back(r);
            return 0;
        }
        ADD_FAILURE() << "unexpected guest call 0x" << std::hex << fn;
        return 0;
    }

    // ---- calls ----------------------------------------------------------
    const ExportEntry* find(const std::string& name) {
        for (const auto& e : exports.entries) if (e.name == name) return &e;
        return nullptr;
    }
    u32 call(const std::string& name, std::vector<u64> args, int float_index = -1) {
        const ExportEntry* e = find(name);
        EXPECT_NE(e, nullptr) << name;
        if (e == nullptr) return 0xDEADBEEF;
        place_args(args);
        if (float_index >= 0 && cpu.is_64bit()) {
            cpu.sse.xmm[float_index].u32_[0] = static_cast<u32>(args[float_index]);
        }
        in_api = true;
        e->handler(marshaler);
        in_api = false;
        return static_cast<u32>(cpu.gpr.rax);
    }
    u32 player(GuestAddr self, u32 slot, std::vector<u64> args = {}, int float_index = -1) {
        args.insert(args.begin(), self);
        return call(MfPlayManager::player_thunk_name(slot), std::move(args),
                    float_index >= 0 ? float_index + 1 : -1);
    }
    u32 item(GuestAddr self, u32 slot, std::vector<u64> args = {}) {
        args.insert(args.begin(), self);
        return call(MfPlayManager::item_thunk_name(slot), std::move(args));
    }
    void place_args(const std::vector<u64>& args) {
        if (cpu.is_64bit()) {
            u64* regs[4] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
            for (usize i = 0; i < args.size(); ++i) {
                if (i < 4) *regs[i] = args[i];
                else ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28 + (i - 4) * 8, args[i]).is_ok());
            }
        } else {
            for (usize i = 0; i < args.size(); ++i) {
                ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4 + i * 4, static_cast<u32>(args[i])).is_ok());
            }
        }
    }
    /// The creating thread's message loop: dispatch every posted event message.
    void pump() {
        auto msgs = std::move(posted);
        posted.clear();
        for (const auto& [h, msg] : msgs) {
            if (msg != EVENT_WINDOW_MESSAGE) continue;
            place_args({h, msg, 0, 0});
            find(MfPlayManager::wndproc_thunk_name())->handler(marshaler);
        }
    }

    // ---- guest memory helpers -------------------------------------------
    GuestAddr ptr_size() const { return cpu.is_64bit() ? 8u : 4u; }
    u64 rd_ptr(GuestAddr at) { return marshaler.read_ptr(at).value(); }
    u32 rd32(GuestAddr at) { return memory.read_u32(at).value(); }
    void wr32(GuestAddr at, u32 v) { ASSERT_TRUE(memory.write_u32(at, v).is_ok()); }
    void wr_ptr(GuestAddr at, u64 v) { ASSERT_TRUE(marshaler.write_ptr(at, v).is_ok()); }
    void put_vtable(GuestAddr obj, GuestAddr vt, std::vector<u64> fns) {
        for (usize i = 0; i < fns.size(); ++i) wr_ptr(vt + i * ptr_size(), fns[i]);
        wr_ptr(obj, vt);
    }
    GuestAddr put_guid(GuestAddr at, const G& g) {
        EXPECT_TRUE(memory.write_bytes(at, reinterpret_cast<const Byte*>(&g), 16).is_ok());
        return at;
    }
    GuestAddr put_wstr(GuestAddr at, const std::u16string& s) {
        for (usize i = 0; i <= s.size(); ++i) {
            EXPECT_TRUE(memory.write_u16(at + i * 2, i < s.size() ? s[i] : 0).is_ok());
        }
        return at;
    }
    std::u16string read_wstr(GuestAddr at) {
        std::u16string s;
        for (u16 u; (u = memory.read_u16(at + s.size() * 2).value()) != 0;) s.push_back(static_cast<char16_t>(u));
        return s;
    }
    GuestAddr put_pv(GuestAddr at, u16 vt, u64 v) {
        std::vector<Byte> z(24, 0);
        EXPECT_TRUE(memory.write_bytes(at, z.data(), z.size()).is_ok());
        EXPECT_TRUE(memory.write_u16(at, vt).is_ok());
        EXPECT_TRUE(memory.write_u64(at + 8, v).is_ok());
        return at;
    }
    u16 pv_vt(GuestAddr at) { return memory.read_u16(at).value(); }
    u64 pv_u64(GuestAddr at) { return memory.read_u64(at + 8).value(); }

    // Guest scratch the tests pass to the API.
    static constexpr GuestAddr OUT = DATA + 0x100;
    static constexpr GuestAddr OUT2 = DATA + 0x200;
    static constexpr GuestAddr PV = DATA + 0x300;
    static constexpr GuestAddr GUIDBUF = DATA + 0x400;
    static constexpr GuestAddr URL = DATA + 0x1000;
    static constexpr GuestAddr URL2 = DATA + 0x2000;

    /// MFPCreateMediaPlayer(NULL, FALSE, options, callback, hwnd, &p).
    GuestAddr create(u32 options = 0, GuestAddr callback = 0, u64 hwnd = 0) {
        wr_ptr(OUT, 0);
        EXPECT_EQ(call("MFPCreateMediaPlayer", {0, 0, options, callback, hwnd, OUT}), S_OK_HR);
        return static_cast<GuestAddr>(rd_ptr(OUT));
    }
    /// CreateMediaItemFromURL(url, TRUE, user, &item).
    GuestAddr make_item(GuestAddr p, const std::u16string& url = u"C:\\m\\a.wav", u64 user = 0x1234) {
        put_wstr(URL, url);
        wr_ptr(OUT2, 0);
        EXPECT_EQ(player(p, P_CreateMediaItemFromURL, {URL, 1, user, OUT2}), S_OK_HR);
        return static_cast<GuestAddr>(rd_ptr(OUT2));
    }
    u32 float_get(GuestAddr p, u32 slot) {
        wr32(OUT, F_MINUS7);
        const u32 hr = player(p, slot, {OUT});
        EXPECT_EQ(hr, S_OK_HR);
        return rd32(OUT);
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    ThunkRegistry thunks{THUNK_BASE};
    MfPlayManager mgr;
    DllExports exports;
    FakeMediaState media;
    std::filesystem::path dir;
    GuestAddr alloc_next{ALLOC};
    GuestAddr task_next{TASK};
    GuestAddr wndproc{0};
    int windows_created{0};
    std::vector<u64> destroyed, hidden;
    std::vector<std::pair<u64, u32>> posted;
    struct Present { u64 hwnd; i32 w, h; };
    std::vector<Present> presented;
    std::vector<EventRec> events;
    i32 cb_refs{1};
    i32 effect_refs{1};
    bool in_api{false};
};

class Mfplay64 : public MfplayFixture { public: void SetUp() override { setup(true); } };
class Mfplay32 : public MfplayFixture { public: void SetUp() override { setup(false); } };

#define BOTH(name)                                   \
    void name##_body(MfplayFixture& f);              \
    TEST_F(Mfplay64, name) { name##_body(*this); }   \
    TEST_F(Mfplay32, name) { name##_body(*this); }   \
    void name##_body(MfplayFixture& f)

} // namespace

#define F f

// ===========================================================================
// Exports (mfplay.lib, the real DLL's export table; probe "== exports")
// ===========================================================================

BOTH(ExportsMatchSdkAndDll) {
    // mfplay.lib: _MFPCreateMediaPlayer@24. The DLL also exports the COM-server
    // entry points and the undocumented Ex (WOW64 build: ret 0x1c).
    const std::map<std::string, u16> expected = {
        {"MFPCreateMediaPlayer", 24}, {"MFPCreateMediaPlayerEx", 28}, {"DllCanUnloadNow", 0},
        {"DllGetClassObject", 12}, {"DllRegisterServer", 0}, {"DllUnregisterServer", 0}};
    for (const auto& [name, bytes] : expected) {
        const ExportEntry* e = F.find(name);
        ASSERT_NE(e, nullptr) << name;
        EXPECT_EQ(e->stdcall_bytes, bytes) << name;
    }
    // x86 vtable thunks pop `this` + arguments (mfplay.h prototypes).
    EXPECT_EQ(MfPlayManager::player_slot_stdcall_bytes(P_GetSupportedRates), 16);
    EXPECT_EQ(MfPlayManager::player_slot_stdcall_bytes(P_CreateMediaItemFromURL), 20);
    EXPECT_EQ(MfPlayManager::player_slot_stdcall_bytes(P_Shutdown), 4);
    EXPECT_EQ(MfPlayManager::item_slot_stdcall_bytes(I_GetStartStopPosition), 20);
    EXPECT_EQ(MfPlayManager::item_slot_stdcall_bytes(I_GetStreamAttribute), 16);
    EXPECT_EQ(static_cast<u32>(P_Shutdown), 38u);
    EXPECT_EQ(static_cast<u32>(I_GetMetadata), 21u);

    // "DllGetClassObject(unknown clsid) -> 80040111 0"
    F.put_guid(F.GUIDBUF, IID_IUnknown_G);
    F.wr_ptr(F.OUT, 1);
    EXPECT_EQ(F.call("DllGetClassObject", {F.GUIDBUF, F.GUIDBUF, F.OUT}), CLASS_E_CLASSNOTAVAILABLE_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 0u);
    EXPECT_EQ(F.call("DllCanUnloadNow", {}), S_OK_HR);
}

// ===========================================================================
// MFPCreateMediaPlayer (probe "== MFPCreateMediaPlayer")
// ===========================================================================

BOTH(CreateParameterMatrixMatchesWindows) {
    // "create(all NULL, pp NULL) -> 80004003"
    EXPECT_EQ(F.call("MFPCreateMediaPlayer", {0, 0, 0, 0, 0, 0}), E_POINTER_HR);
    // "create(NULL URL, start TRUE) -> 80070057 0"
    F.wr_ptr(F.OUT, 1);
    EXPECT_EQ(F.call("MFPCreateMediaPlayer", {0, 1, 0, 0, 0, F.OUT}), E_INVALIDARG_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 0u);
    // "create(NULL URL) -> 00000000 1", one hidden window on the thread.
    const GuestAddr p = F.create();
    ASSERT_NE(p, 0u);
    EXPECT_EQ(F.windows_created, 1);
    // "AddRef -> 3", "Release -> 2", "QI(IUnknown) ... 1", QI(item/callback) E_NOINTERFACE NULL.
    EXPECT_EQ(F.player(p, P_AddRef), 3u);
    EXPECT_EQ(F.player(p, P_Release), 2u);
    F.put_guid(F.GUIDBUF, IID_IUnknown_G);
    EXPECT_EQ(F.player(p, P_QueryInterface, {F.GUIDBUF, F.OUT2}), S_OK_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT2), p);
    EXPECT_EQ(F.player(p, P_Release), 2u);
    for (const G& iid : {IID_IMFPMediaItem_G, IID_IMFPMediaPlayerCallback_G}) {
        F.put_guid(F.GUIDBUF, iid);
        F.wr_ptr(F.OUT2, 1);
        EXPECT_EQ(F.player(p, P_QueryInterface, {F.GUIDBUF, F.OUT2}), E_NOINTERFACE_HR);
        EXPECT_EQ(F.rd_ptr(F.OUT2), 0u);
    }
    F.put_guid(F.GUIDBUF, IID_IMFPMediaPlayer_G);
    EXPECT_EQ(F.player(p, P_QueryInterface, {F.GUIDBUF, F.OUT2}), S_OK_HR);
    EXPECT_EQ(F.player(p, P_Release), 2u);
    // "final Release -> 0", "thread windows after release -> 0"
    EXPECT_EQ(F.player(p, P_Release), 0u);
    EXPECT_EQ(F.destroyed, std::vector<u64>{F.EVENT_HWND});
    EXPECT_EQ(F.mgr.live_player_count(), 0u);

    // "create(options=...) -> 00000000 1" for every bit, defined or not.
    for (u32 options : {1u, 2u, 4u, 7u, 8u, 0x80000000u}) {
        const GuestAddr q = F.create(options);
        ASSERT_NE(q, 0u) << options;
        EXPECT_EQ(F.player(q, P_Shutdown), S_OK_HR);
        EXPECT_EQ(F.player(q, P_Release), 0u);
    }
    // FREE_THREADED: no window ("thread windows added -> 0").
    EXPECT_EQ(F.windows_created, 1 + 4);  // options 2, 4, 8, 0x80000000
    // "create(bogus hWnd) -> 00000000 1", "GetVideoWindow -> 00000000 1"
    const GuestAddr w = F.create(0, 0, 0x1234560);
    EXPECT_EQ(F.player(w, P_GetVideoWindow, {F.OUT2}), S_OK_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT2), 0x1234560u);
    EXPECT_EQ(F.player(w, P_Release), 0u);

    // URLs are opened synchronously by the call.
    const auto create_url = [&](const std::u16string& url, u32 start) {
        F.put_wstr(F.URL, url);
        F.wr_ptr(F.OUT, 1);
        const u32 hr = F.call("MFPCreateMediaPlayer", {F.URL, start, 0, 0, 0, F.OUT});
        return hr;
    };
    // "create(missing file) -> 80070002 0"
    EXPECT_EQ(create_url(u"C:\\m\\missing.wav", 0), FILE_NOT_FOUND_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 0u);
    EXPECT_EQ(create_url(u"C:\\nodir\\x.wav", 0), PATH_NOT_FOUND_HR);
    // "create(junk file) -> c00d36c4 0"
    F.media.open_ok = false;
    EXPECT_EQ(create_url(u"C:\\m\\junk.wav", 0), MF_E_UNSUPPORTED_BYTESTREAM_TYPE_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 0u);
    F.media.open_ok = true;
    // "create(unknown scheme) -> c00d36c3 0"
    EXPECT_EQ(create_url(u"bogus-scheme://x/y.wav", 0), MF_E_UNSUPPORTED_SCHEME_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 0u);
    // A file: URL names the same file.
    EXPECT_EQ(create_url(u"file:///C:/m/a.wav", 0), S_OK_HR);
    const GuestAddr u = static_cast<GuestAddr>(F.rd_ptr(F.OUT));
    ASSERT_NE(u, 0u);
    // "create(wav, start FALSE)": "GetState immediately -> 0", "GetMediaItem -> c00d36d5 0"
    F.wr32(F.OUT2, 99);
    EXPECT_EQ(F.player(u, P_GetState, {F.OUT2}), S_OK_HR);
    EXPECT_EQ(F.rd32(F.OUT2), 0u);
    EXPECT_EQ(F.player(u, P_GetMediaItem, {F.OUT2}), MF_E_NOT_FOUND_HR);
    EXPECT_EQ(F.player(u, P_Release), 0u);
    EXPECT_EQ(F.mgr.live_player_count(), 0u);
    EXPECT_EQ(F.mgr.live_item_count(), 0u);
}

// ===========================================================================
// IMFPMediaPlayer with no item (probe "== empty player")
// ===========================================================================

BOTH(EmptyPlayerMatchesWindows) {
    const GuestAddr p = F.create();
    // "Play/Pause/Stop/FrameStep -> 00000000"
    for (u32 slot : {P_Play, P_Pause, P_Stop, P_FrameStep}) EXPECT_EQ(F.player(p, slot), S_OK_HR);
    F.put_guid(F.GUIDBUF, POS_100NS);
    F.put_pv(F.PV, VT_I8, 0);
    EXPECT_EQ(F.player(p, P_SetPosition, {F.GUIDBUF, F.PV}), S_OK_HR);
    EXPECT_EQ(F.player(p, P_SetPosition, {F.GUIDBUF, 0}), E_POINTER_HR);
    // "GetPosition(100NS) -> c00d36b2 0" (PROPVARIANT reset to VT_EMPTY)
    F.put_pv(F.PV, 0xCCCC, 0);
    EXPECT_EQ(F.player(p, P_GetPosition, {F.GUIDBUF, F.PV}), MF_E_INVALIDREQUEST_HR);
    EXPECT_EQ(F.pv_vt(F.PV), VT_EMPTY);
    EXPECT_EQ(F.player(p, P_GetPosition, {F.GUIDBUF, 0}), E_POINTER_HR);
    // "GetPosition(bad guid) -> 80070057 0"
    F.put_guid(F.GUIDBUF + 0x20, IID_IUnknown_G);
    F.put_pv(F.PV, 0xCCCC, 0);
    EXPECT_EQ(F.player(p, P_GetPosition, {F.GUIDBUF + 0x20, F.PV}), E_INVALIDARG_HR);
    EXPECT_EQ(F.pv_vt(F.PV), VT_EMPTY);
    // "GetDuration(100NS) -> c00d36b2 52428" (untouched)
    F.put_pv(F.PV, 0xCCCC, 0);
    EXPECT_EQ(F.player(p, P_GetDuration, {F.GUIDBUF, F.PV}), MF_E_INVALIDREQUEST_HR);
    EXPECT_EQ(F.pv_vt(F.PV), 0xCCCCu);
    // "SetRate(1) -> 0", "SetRate(0) -> c00d3702", "SetRate(2) -> 0", "GetRate -> 3f800000"
    EXPECT_EQ(F.player(p, P_SetRate, {F_ONE}, 0), S_OK_HR);
    EXPECT_EQ(F.player(p, P_SetRate, {0}, 0), MF_E_OUT_OF_RANGE_HR);
    EXPECT_EQ(F.player(p, P_SetRate, {0x40000000u}, 0), S_OK_HR);
    EXPECT_EQ(F.float_get(p, P_GetRate), F_ONE);
    EXPECT_EQ(F.player(p, P_GetRate, {0}), E_POINTER_HR);
    // "GetSupportedRates(fwd|rev) -> c00d36d0 c0e00000 c0e00000"
    for (u32 fwd : {1u, 0u}) {
        F.wr32(F.OUT, F_MINUS7);
        F.wr32(F.OUT2, F_MINUS7);
        EXPECT_EQ(F.player(p, P_GetSupportedRates, {fwd, F.OUT, F.OUT2}), MF_E_UNSUPPORTED_RATE_HR);
        EXPECT_EQ(F.rd32(F.OUT), F_MINUS7);
        EXPECT_EQ(F.rd32(F.OUT2), F_MINUS7);
    }
    // "GetState -> 0 0", "GetState(NULL) -> 80004003"
    F.wr32(F.OUT, 99);
    EXPECT_EQ(F.player(p, P_GetState, {F.OUT}), S_OK_HR);
    EXPECT_EQ(F.rd32(F.OUT), 0u);
    EXPECT_EQ(F.player(p, P_GetState, {0}), E_POINTER_HR);
    // "GetMediaItem -> c00d36d5 0", NULL -> E_POINTER, ClearMediaItem S_OK, SetMediaItem(NULL) E_POINTER
    F.wr_ptr(F.OUT, 1);
    EXPECT_EQ(F.player(p, P_GetMediaItem, {F.OUT}), MF_E_NOT_FOUND_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 0u);
    EXPECT_EQ(F.player(p, P_GetMediaItem, {0}), E_POINTER_HR);
    EXPECT_EQ(F.player(p, P_ClearMediaItem), S_OK_HR);
    EXPECT_EQ(F.player(p, P_SetMediaItem, {0}), E_POINTER_HR);
    // Volume: default 1.0; [0, 1]; out of range keeps the old value.
    EXPECT_EQ(F.float_get(p, P_GetVolume), F_ONE);
    EXPECT_EQ(F.player(p, P_GetVolume, {0}), E_POINTER_HR);
    const std::vector<std::pair<u32, u32>> volumes = {
        {0xbdcccccdu, MF_E_OUT_OF_RANGE_HR}, {0, S_OK_HR}, {0x3f000000u, S_OK_HR},
        {F_ONE, S_OK_HR}, {0x3fc00000u, MF_E_OUT_OF_RANGE_HR}};
    const std::vector<u32> volume_after = {F_ONE, 0, 0x3f000000u, F_ONE, F_ONE};
    for (usize i = 0; i < volumes.size(); ++i) {
        EXPECT_EQ(F.player(p, P_SetVolume, {volumes[i].first}, 0), volumes[i].second) << i;
        EXPECT_EQ(F.float_get(p, P_GetVolume), volume_after[i]) << i;
    }
    // Balance: default 0; [-1, 1].
    EXPECT_EQ(F.float_get(p, P_GetBalance), 0u);
    const std::vector<std::pair<u32, u32>> balances = {
        {0xbfc00000u, MF_E_OUT_OF_RANGE_HR}, {0xbf800000u, S_OK_HR}, {0x3e800000u, S_OK_HR},
        {F_ONE, S_OK_HR}, {0x40000000u, MF_E_OUT_OF_RANGE_HR}};
    const std::vector<u32> balance_after = {0, 0xbf800000u, 0x3e800000u, F_ONE, F_ONE};
    for (usize i = 0; i < balances.size(); ++i) {
        EXPECT_EQ(F.player(p, P_SetBalance, {balances[i].first}, 0), balances[i].second) << i;
        EXPECT_EQ(F.float_get(p, P_GetBalance), balance_after[i]) << i;
    }
    // Mute keeps the BOOL as given ("GetMute after 2 -> 2").
    F.wr32(F.OUT, 7);
    EXPECT_EQ(F.player(p, P_GetMute, {F.OUT}), S_OK_HR);
    EXPECT_EQ(F.rd32(F.OUT), 0u);
    EXPECT_EQ(F.player(p, P_SetMute, {2}), S_OK_HR);
    EXPECT_EQ(F.player(p, P_GetMute, {F.OUT}), S_OK_HR);
    EXPECT_EQ(F.rd32(F.OUT), 2u);
    EXPECT_EQ(F.player(p, P_SetMute, {0}), S_OK_HR);
    // Video, no item: MF_E_INVALIDREQUEST with outputs untouched ...
    F.wr32(F.OUT, 7);
    EXPECT_EQ(F.player(p, P_GetNativeVideoSize, {F.OUT, F.OUT2}), MF_E_INVALIDREQUEST_HR);
    EXPECT_EQ(F.rd32(F.OUT), 7u);
    EXPECT_EQ(F.player(p, P_GetNativeVideoSize, {0, 0}), MF_E_INVALIDREQUEST_HR);
    EXPECT_EQ(F.player(p, P_GetIdealVideoSize, {F.OUT, F.OUT2}), MF_E_INVALIDREQUEST_HR);
    // ... but SetVideoSourceRect without a window: "-> 80070578", even NULL.
    EXPECT_EQ(F.player(p, P_SetVideoSourceRect, {F.PV}), INVALID_WINDOW_HANDLE_HR);
    EXPECT_EQ(F.player(p, P_SetVideoSourceRect, {0}), INVALID_WINDOW_HANDLE_HR);
    EXPECT_EQ(F.player(p, P_GetVideoSourceRect, {F.PV}), MF_E_INVALIDREQUEST_HR);
    F.wr32(F.OUT, 0xCCCC);
    EXPECT_EQ(F.player(p, P_GetAspectRatioMode, {F.OUT}), MF_E_INVALIDREQUEST_HR);
    EXPECT_EQ(F.rd32(F.OUT), 0xCCCCu);
    for (u32 mode : {0u, 1u, 2u, 4u, 7u, 8u}) {
        EXPECT_EQ(F.player(p, P_SetAspectRatioMode, {mode}), MF_E_INVALIDREQUEST_HR);
    }
    F.wr_ptr(F.OUT, 1);
    EXPECT_EQ(F.player(p, P_GetVideoWindow, {F.OUT}), S_OK_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 0u);
    EXPECT_EQ(F.player(p, P_UpdateVideo), MF_E_INVALIDREQUEST_HR);
    EXPECT_EQ(F.player(p, P_SetBorderColor, {0x123456}), MF_E_INVALIDREQUEST_HR);
    EXPECT_EQ(F.player(p, P_GetBorderColor, {F.OUT}), MF_E_INVALIDREQUEST_HR);
    // Effects: "InsertEffect -> 0", duplicate "-> c00d36bf", "RemoveEffect -> 0".
    EXPECT_EQ(F.player(p, P_InsertEffect, {F.EFFECT, 0}), S_OK_HR);
    EXPECT_EQ(F.effect_refs, 2);
    EXPECT_EQ(F.player(p, P_InsertEffect, {F.EFFECT, 1}), MF_E_INVALIDINDEX_HR);
    EXPECT_EQ(F.player(p, P_RemoveEffect, {F.EFFECT}), S_OK_HR);
    EXPECT_EQ(F.effect_refs, 1);
    EXPECT_EQ(F.player(p, P_RemoveEffect, {F.EFFECT}), MF_E_NOT_FOUND_HR);  // Learn
    EXPECT_EQ(F.player(p, P_RemoveAllEffects), S_OK_HR);
    // Item creation errors.
    F.wr_ptr(F.OUT, 1);
    EXPECT_EQ(F.player(p, P_CreateMediaItemFromObject, {0, 1, 0, F.OUT}), E_POINTER_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 1u);
    EXPECT_EQ(F.player(p, P_CreateMediaItemFromURL, {0, 1, 0, F.OUT}), E_POINTER_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 0u);
    F.put_wstr(F.URL, u"C:\\m\\a.wav");
    EXPECT_EQ(F.player(p, P_CreateMediaItemFromURL, {F.URL, 1, 0, 0}), E_POINTER_HR);
    for (const auto& [url, hr] : std::vector<std::pair<std::u16string, u32>>{
             {u"C:\\m\\missing.wav", FILE_NOT_FOUND_HR},
             {u"bogus-scheme://x/y.wav", MF_E_UNSUPPORTED_SCHEME_HR}}) {
        F.put_wstr(F.URL, url);
        F.wr_ptr(F.OUT, 1);
        EXPECT_EQ(F.player(p, P_CreateMediaItemFromURL, {F.URL, 1, 0, F.OUT}), hr);
        EXPECT_EQ(F.rd_ptr(F.OUT), 0u);
    }
    F.media.open_ok = false;
    F.put_wstr(F.URL, u"C:\\m\\junk.wav");
    EXPECT_EQ(F.player(p, P_CreateMediaItemFromURL, {F.URL, 1, 0, F.OUT}), MF_E_UNSUPPORTED_BYTESTREAM_TYPE_HR);
    F.media.open_ok = true;
    // "async,&out no callback -> c00d36b2 1" and with NULL out.
    F.put_wstr(F.URL, u"C:\\m\\a.wav");
    F.wr_ptr(F.OUT, 1);
    EXPECT_EQ(F.player(p, P_CreateMediaItemFromURL, {F.URL, 0, 0, F.OUT}), MF_E_INVALIDREQUEST_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 1u);
    EXPECT_EQ(F.player(p, P_CreateMediaItemFromURL, {F.URL, 0, 0, 0}), MF_E_INVALIDREQUEST_HR);
    EXPECT_EQ(F.mgr.live_item_count(), 0u);

    // ---- "-- after Shutdown" ----
    EXPECT_EQ(F.player(p, P_Shutdown), S_OK_HR);
    EXPECT_EQ(F.player(p, P_Shutdown), S_OK_HR);
    F.wr32(F.OUT, 99);
    EXPECT_EQ(F.player(p, P_GetState, {F.OUT}), S_OK_HR);
    EXPECT_EQ(F.rd32(F.OUT), 4u);  // MFP_MEDIAPLAYER_STATE_SHUTDOWN
    for (u32 slot : {P_Play, P_Pause, P_Stop, P_FrameStep, P_ClearMediaItem, P_UpdateVideo}) {
        EXPECT_EQ(F.player(p, slot), MF_E_SHUTDOWN_HR) << slot;
    }
    F.put_pv(F.PV, 0xCCCC, 0);
    EXPECT_EQ(F.player(p, P_GetPosition, {F.GUIDBUF, F.PV}), MF_E_SHUTDOWN_HR);
    EXPECT_EQ(F.pv_vt(F.PV), VT_EMPTY);
    EXPECT_EQ(F.player(p, P_SetRate, {F_ONE}, 0), MF_E_SHUTDOWN_HR);
    F.wr32(F.OUT, F_MINUS7);
    EXPECT_EQ(F.player(p, P_GetRate, {F.OUT}), MF_E_SHUTDOWN_HR);
    EXPECT_EQ(F.rd32(F.OUT), F_MINUS7);
    EXPECT_EQ(F.player(p, P_GetSupportedRates, {1, F.OUT, F.OUT2}), MF_E_SHUTDOWN_HR);
    F.wr_ptr(F.OUT, 1);
    EXPECT_EQ(F.player(p, P_GetMediaItem, {F.OUT}), MF_E_SHUTDOWN_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 0u);
    F.wr_ptr(F.OUT, 1);
    EXPECT_EQ(F.player(p, P_CreateMediaItemFromURL, {F.URL, 1, 0, F.OUT}), MF_E_SHUTDOWN_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 0u);
    // Still usable: "GetVolume -> 0 3f800000", "SetVolume(0.5) -> 0", balance, mute.
    EXPECT_EQ(F.float_get(p, P_GetVolume), F_ONE);
    EXPECT_EQ(F.player(p, P_SetVolume, {0x3f000000u}, 0), S_OK_HR);
    EXPECT_EQ(F.float_get(p, P_GetBalance), F_ONE);
    EXPECT_EQ(F.player(p, P_SetMute, {1}), S_OK_HR);
    EXPECT_EQ(F.player(p, P_GetNativeVideoSize, {F.OUT, F.OUT2}), MF_E_SHUTDOWN_HR);
    EXPECT_EQ(F.player(p, P_GetVideoWindow, {F.OUT}), S_OK_HR);
    F.wr32(F.OUT, 0xCCCC);
    EXPECT_EQ(F.player(p, P_GetAspectRatioMode, {F.OUT}), MF_E_SHUTDOWN_HR);
    EXPECT_EQ(F.rd32(F.OUT), 0xCCCCu);
    EXPECT_EQ(F.player(p, P_GetBorderColor, {F.OUT}), MF_E_SHUTDOWN_HR);
    EXPECT_EQ(F.player(p, P_RemoveAllEffects), S_OK_HR);
    EXPECT_EQ(F.player(p, P_Release), 0u);
}

// ===========================================================================
// IMFPMediaItem (probe "== media item (wav, sync)")
// ===========================================================================

BOTH(MediaItemMatchesWindows) {
    const GuestAddr p = F.create();
    const GuestAddr it = F.make_item(p);
    ASSERT_NE(it, 0u);
    EXPECT_EQ(F.item(it, I_AddRef), 2u);
    EXPECT_EQ(F.item(it, I_Release), 1u);
    F.put_guid(F.GUIDBUF, IID_IUnknown_G);
    EXPECT_EQ(F.item(it, I_QueryInterface, {F.GUIDBUF, F.OUT}), S_OK_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), it);
    EXPECT_EQ(F.item(it, I_Release), 1u);
    F.put_guid(F.GUIDBUF, IID_IMFPMediaPlayer_G);
    EXPECT_EQ(F.item(it, I_QueryInterface, {F.GUIDBUF, F.OUT}), E_NOINTERFACE_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 0u);
    // "GetMediaPlayer -> 0 1" (the same player, AddRef'd), NULL -> E_POINTER
    EXPECT_EQ(F.item(it, I_GetMediaPlayer, {F.OUT}), S_OK_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), p);
    EXPECT_EQ(F.player(p, P_Release), 2u);
    EXPECT_EQ(F.item(it, I_GetMediaPlayer, {0}), E_POINTER_HR);
    // "GetURL equals -> 0 1": a CoTaskMem copy.
    EXPECT_EQ(F.item(it, I_GetURL, {F.OUT}), S_OK_HR);
    EXPECT_EQ(F.read_wstr(static_cast<GuestAddr>(F.rd_ptr(F.OUT))), u"C:\\m\\a.wav");
    EXPECT_EQ(F.item(it, I_GetURL, {0}), E_POINTER_HR);
    // "GetObject -> c00d36d5 0"
    F.wr_ptr(F.OUT, 1);
    EXPECT_EQ(F.item(it, I_GetObject, {F.OUT}), MF_E_NOT_FOUND_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 0u);
    // "GetUserData -> 0 4660", SetUserData(77)
    EXPECT_EQ(F.item(it, I_GetUserData, {F.OUT}), S_OK_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 0x1234u);
    EXPECT_EQ(F.item(it, I_SetUserData, {77}), S_OK_HR);
    EXPECT_EQ(F.item(it, I_GetUserData, {F.OUT}), S_OK_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 77u);
    EXPECT_EQ(F.item(it, I_GetUserData, {0}), E_POINTER_HR);

    // Start/stop: defaults GUID_NULL + VT_EMPTY; set 0.1 s / 0.2 s VT_I8.
    const GuestAddr g1 = F.GUIDBUF, g2 = F.GUIDBUF + 0x20, pv1 = F.PV, pv2 = F.PV + 0x40;
    F.put_guid(g1, IID_IUnknown_G);
    F.put_pv(pv1, 0xCCCC, 0);
    F.put_guid(g2, IID_IUnknown_G);
    F.put_pv(pv2, 0xCCCC, 0);
    EXPECT_EQ(F.item(it, I_GetStartStopPosition, {g1, pv1, g2, pv2}), S_OK_HR);
    EXPECT_EQ(F.rd32(g1), 0u);
    EXPECT_EQ(F.pv_vt(pv1), VT_EMPTY);
    EXPECT_EQ(F.pv_vt(pv2), VT_EMPTY);
    EXPECT_EQ(F.item(it, I_GetStartStopPosition, {0, 0, 0, 0}), E_POINTER_HR);
    F.put_guid(g1, POS_100NS);
    F.put_guid(g2, POS_100NS);
    F.put_pv(pv1, VT_I8, 1000000);
    F.put_pv(pv2, VT_I8, 2000000);
    EXPECT_EQ(F.item(it, I_SetStartStopPosition, {g1, pv1, g2, pv2}), S_OK_HR);
    F.put_pv(pv1, 0xCCCC, 0);
    F.put_pv(pv2, 0xCCCC, 0);
    EXPECT_EQ(F.item(it, I_GetStartStopPosition, {g1, pv1, g2, pv2}), S_OK_HR);
    EXPECT_EQ(F.pv_vt(pv1), VT_I8);
    EXPECT_EQ(F.pv_u64(pv1), 1000000u);
    EXPECT_EQ(F.pv_vt(pv2), VT_I8);
    EXPECT_EQ(F.pv_u64(pv2), 2000000u);
    // "stop<start -> c00d3702", "VT_UI4 start -> 80070057", "bad guid -> 80070057",
    // "guid, NULL value -> 80004003", "VT_EMPTY both -> 0", "all NULL -> 80004003"
    F.put_pv(pv1, VT_I8, 2000000);
    F.put_pv(pv2, VT_I8, 1000000);
    EXPECT_EQ(F.item(it, I_SetStartStopPosition, {g1, pv1, g2, pv2}), MF_E_OUT_OF_RANGE_HR);
    F.put_pv(pv1, VT_UI4, 5);
    EXPECT_EQ(F.item(it, I_SetStartStopPosition, {g1, pv1, 0, 0}), E_INVALIDARG_HR);
    F.put_guid(F.GUIDBUF + 0x60, IID_IUnknown_G);
    F.put_pv(pv1, VT_I8, 0);
    EXPECT_EQ(F.item(it, I_SetStartStopPosition, {F.GUIDBUF + 0x60, pv1, 0, 0}), E_INVALIDARG_HR);
    EXPECT_EQ(F.item(it, I_SetStartStopPosition, {g1, 0, 0, 0}), E_POINTER_HR);
    // Learn: past the duration is out of range.
    F.put_pv(pv1, VT_I8, 2500001);
    EXPECT_EQ(F.item(it, I_SetStartStopPosition, {g1, pv1, 0, 0}), MF_E_OUT_OF_RANGE_HR);
    F.put_pv(pv1, VT_EMPTY, 0);
    EXPECT_EQ(F.item(it, I_SetStartStopPosition, {g1, pv1, g2, pv1}), S_OK_HR);
    EXPECT_EQ(F.item(it, I_SetStartStopPosition, {0, 0, 0, 0}), E_POINTER_HR);
    F.put_pv(pv1, 0xCCCC, 0);
    F.put_pv(pv2, 0xCCCC, 0);
    EXPECT_EQ(F.item(it, I_GetStartStopPosition, {g1, pv1, g2, pv2}), S_OK_HR);
    EXPECT_EQ(F.pv_vt(pv1), VT_EMPTY);
    EXPECT_EQ(F.pv_vt(pv2), VT_EMPTY);

    // "HasVideo -> 0 0 0", "HasVideo(NULL,NULL) -> 0", "HasAudio -> 0 1 1"
    F.wr32(F.OUT, 7);
    F.wr32(F.OUT2, 7);
    EXPECT_EQ(F.item(it, I_HasVideo, {F.OUT, F.OUT2}), S_OK_HR);
    EXPECT_EQ(F.rd32(F.OUT), 0u);
    EXPECT_EQ(F.rd32(F.OUT2), 0u);
    EXPECT_EQ(F.item(it, I_HasVideo, {0, 0}), S_OK_HR);
    EXPECT_EQ(F.item(it, I_HasAudio, {F.OUT, F.OUT2}), S_OK_HR);
    EXPECT_EQ(F.rd32(F.OUT), 1u);
    EXPECT_EQ(F.rd32(F.OUT2), 1u);
    // "IsProtected -> 00000001 0"
    F.wr32(F.OUT, 7);
    EXPECT_EQ(F.item(it, I_IsProtected, {F.OUT}), S_FALSE_HR);
    EXPECT_EQ(F.rd32(F.OUT), 0u);
    // "GetDuration(100NS) -> 0 vt=21 v=2500000", "GetDuration(bad guid) -> 80070057 0"
    F.put_guid(g1, POS_100NS);
    EXPECT_EQ(F.item(it, I_GetDuration, {g1, pv1}), S_OK_HR);
    EXPECT_EQ(F.pv_vt(pv1), VT_UI8);
    EXPECT_EQ(F.pv_u64(pv1), 2500000u);
    F.put_pv(pv1, 0xCCCC, 0);
    EXPECT_EQ(F.item(it, I_GetDuration, {F.GUIDBUF + 0x60, pv1}), E_INVALIDARG_HR);
    EXPECT_EQ(F.pv_vt(pv1), VT_EMPTY);
    // Streams: "GetNumberOfStreams -> 0 1", selection, "(5) -> 80070057 0"
    F.wr32(F.OUT, 77);
    EXPECT_EQ(F.item(it, I_GetNumberOfStreams, {F.OUT}), S_OK_HR);
    EXPECT_EQ(F.rd32(F.OUT), 1u);
    EXPECT_EQ(F.item(it, I_GetStreamSelection, {0, F.OUT}), S_OK_HR);
    EXPECT_EQ(F.rd32(F.OUT), 1u);
    F.wr32(F.OUT, 7);
    EXPECT_EQ(F.item(it, I_GetStreamSelection, {5, F.OUT}), E_INVALIDARG_HR);
    EXPECT_EQ(F.rd32(F.OUT), 0u);
    EXPECT_EQ(F.item(it, I_SetStreamSelection, {0, 0}), S_OK_HR);
    EXPECT_EQ(F.item(it, I_GetStreamSelection, {0, F.OUT}), S_OK_HR);
    EXPECT_EQ(F.rd32(F.OUT), 0u);
    EXPECT_EQ(F.item(it, I_HasAudio, {F.OUT, F.OUT2}), S_OK_HR);  // "after deselect -> 0 1 0"
    EXPECT_EQ(F.rd32(F.OUT), 1u);
    EXPECT_EQ(F.rd32(F.OUT2), 0u);
    // Nothing selected: the topology is refused synchronously, no event.
    EXPECT_EQ(F.player(p, P_SetMediaItem, {it}), MF_E_TOPO_UNSUPPORTED_HR);
    EXPECT_EQ(F.item(it, I_SetStreamSelection, {0, 1}), S_OK_HR);
    EXPECT_EQ(F.item(it, I_SetStreamSelection, {5, 1}), E_INVALIDARG_HR);
    // Attributes: "GetStreamAttribute(0,MF_SD_LANGUAGE) -> c00d36e6 0",
    // "(5, ...) -> 80070057 0", "MF_PD_DURATION -> vt=21 v=2500000",
    // "unknown key -> c00d36e6 0".
    F.put_guid(F.GUIDBUF + 0x80, MF_SD_LANGUAGE_G);
    F.put_pv(pv1, 0xCCCC, 0);
    EXPECT_EQ(F.item(it, I_GetStreamAttribute, {0, F.GUIDBUF + 0x80, pv1}), MF_E_ATTRIBUTENOTFOUND_HR);
    EXPECT_EQ(F.pv_vt(pv1), VT_EMPTY);
    EXPECT_EQ(F.item(it, I_GetStreamAttribute, {5, F.GUIDBUF + 0x80, pv1}), E_INVALIDARG_HR);
    F.put_guid(F.GUIDBUF + 0xA0, MF_MT_MAJOR_TYPE_G);
    EXPECT_EQ(F.item(it, I_GetStreamAttribute, {0, F.GUIDBUF + 0xA0, pv1}), S_OK_HR);
    EXPECT_EQ(F.pv_vt(pv1), VT_CLSID);
    G major{};
    ASSERT_TRUE(F.memory.read_bytes(static_cast<GuestAddr>(F.rd_ptr(pv1 + 8)),
                                    reinterpret_cast<Byte*>(&major), 16).is_ok());
    EXPECT_EQ(std::memcmp(&major, &MFMediaType_Audio_G, 16), 0);
    F.put_guid(F.GUIDBUF + 0xC0, MF_PD_DURATION_G);
    EXPECT_EQ(F.item(it, I_GetPresentationAttribute, {F.GUIDBUF + 0xC0, pv1}), S_OK_HR);
    EXPECT_EQ(F.pv_vt(pv1), VT_UI8);
    EXPECT_EQ(F.pv_u64(pv1), 2500000u);
    EXPECT_EQ(F.item(it, I_GetPresentationAttribute, {F.GUIDBUF + 0x60, pv1}), MF_E_ATTRIBUTENOTFOUND_HR);
    EXPECT_EQ(F.pv_vt(pv1), VT_EMPTY);
    // "GetCharacteristics -> 0 00000006", "SetStreamSink(0|5,NULL) -> 0"
    EXPECT_EQ(F.item(it, I_GetCharacteristics, {F.OUT}), S_OK_HR);
    EXPECT_EQ(F.rd32(F.OUT), 6u);
    EXPECT_EQ(F.item(it, I_SetStreamSink, {0, 0}), S_OK_HR);
    EXPECT_EQ(F.item(it, I_SetStreamSink, {5, 0}), S_OK_HR);
    EXPECT_EQ(F.item(it, I_SetStreamSink, {0, F.EFFECT}), E_NOTIMPL_HR);
    EXPECT_EQ(F.item(it, I_GetMetadata, {F.OUT}), E_NOTIMPL_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 0u);

    // "other player SetMediaItem(item) -> 80070057"
    const GuestAddr p2 = F.create();
    EXPECT_EQ(F.player(p2, P_SetMediaItem, {it}), E_INVALIDARG_HR);
    EXPECT_EQ(F.player(p2, P_Release), 0u);

    // "-- item after player Shutdown"
    EXPECT_EQ(F.player(p, P_Shutdown), S_OK_HR);
    F.wr_ptr(F.OUT, 1);
    EXPECT_EQ(F.item(it, I_GetMediaPlayer, {F.OUT}), MF_E_SHUTDOWN_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 0u);
    EXPECT_EQ(F.item(it, I_GetURL, {F.OUT}), MF_E_SHUTDOWN_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 0u);
    EXPECT_EQ(F.item(it, I_GetUserData, {F.OUT}), S_OK_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 77u);
    F.wr32(F.OUT, 7);
    F.wr32(F.OUT2, 7);
    EXPECT_EQ(F.item(it, I_HasAudio, {F.OUT, F.OUT2}), MF_E_SHUTDOWN_HR);
    EXPECT_EQ(F.rd32(F.OUT), 0u);
    EXPECT_EQ(F.rd32(F.OUT2), 0u);
    F.wr32(F.OUT, 77);
    EXPECT_EQ(F.item(it, I_GetNumberOfStreams, {F.OUT}), MF_E_SHUTDOWN_HR);
    EXPECT_EQ(F.rd32(F.OUT), 0u);
    EXPECT_EQ(F.item(it, I_GetDuration, {g1, pv1}), MF_E_SHUTDOWN_HR);
    EXPECT_EQ(F.pv_vt(pv1), VT_EMPTY);
    EXPECT_EQ(F.item(it, I_Release), 0u);
    EXPECT_EQ(F.player(p, P_Release), 0u);
    EXPECT_EQ(F.mgr.live_item_count(), 0u);
}

// ===========================================================================
// Events, default mode (probe "== events (default ...)")
// ===========================================================================

BOTH(EventsArriveFromTheMessageLoopOnly) {
    const GuestAddr p = F.create(0, F.CB);
    EXPECT_EQ(F.cb_refs, 2);  // "callback refs after create -> 2"
    // No item: every async call completes S_OK, its event carries the result.
    F.put_guid(F.GUIDBUF, POS_100NS);
    F.put_pv(F.PV, VT_I8, 500000);
    EXPECT_EQ(F.player(p, P_SetRate, {0x40000000u}, 0), S_OK_HR);
    EXPECT_EQ(F.player(p, P_SetRate, {F_ONE}, 0), S_OK_HR);
    EXPECT_EQ(F.player(p, P_SetPosition, {F.GUIDBUF, F.PV}), S_OK_HR);
    EXPECT_EQ(F.player(p, P_Pause), S_OK_HR);
    EXPECT_EQ(F.player(p, P_FrameStep), S_OK_HR);
    EXPECT_EQ(F.player(p, P_Stop), S_OK_HR);
    EXPECT_EQ(F.player(p, P_Play), S_OK_HR);
    EXPECT_EQ(F.player(p, P_ClearMediaItem), S_OK_HR);
    // Synchronous parameter checks: "SetPosition(VT_UI4) -> 80070057",
    // "SetPosition(-5) -> c00d36e5", "SetPosition(bad guid) -> 80070057".
    F.put_pv(F.PV, VT_UI4, 0);
    EXPECT_EQ(F.player(p, P_SetPosition, {F.GUIDBUF, F.PV}), E_INVALIDARG_HR);
    F.put_pv(F.PV, VT_I8, static_cast<u64>(-5));
    EXPECT_EQ(F.player(p, P_SetPosition, {F.GUIDBUF, F.PV}), MF_E_INVALID_POSITION_HR);
    F.put_guid(F.GUIDBUF + 0x20, IID_IUnknown_G);
    F.put_pv(F.PV, VT_I8, 0);
    EXPECT_EQ(F.player(p, P_SetPosition, {F.GUIDBUF + 0x20, F.PV}), E_INVALIDARG_HR);
    // "events before pumping -> 0"
    EXPECT_TRUE(F.events.empty());
    F.pump();
    struct Want { EventType type; u32 hr; u32 extra; };
    const std::vector<Want> want = {
        {EventType::RateSet, MF_E_UNSUPPORTED_RATE_HR, 0},    // "rate 2 ... rate=00000000"
        {EventType::RateSet, S_OK_HR, F_ONE},                 // "rate 1 ... rate=3f800000"
        {EventType::PositionSet, MF_E_INVALIDREQUEST_HR, 0},
        {EventType::Pause, MF_E_INVALIDREQUEST_HR, 0},
        {EventType::FrameStep, MF_E_INVALIDREQUEST_HR, 0},
        {EventType::Stop, S_OK_HR, 0},
        {EventType::Play, MF_E_INVALIDREQUEST_HR, 0},
        {EventType::MediaItemCleared, S_OK_HR, 0},
    };
    ASSERT_EQ(F.events.size(), want.size());
    for (usize i = 0; i < want.size(); ++i) {
        const EventRec& e = F.events[i];
        EXPECT_EQ(e.type, static_cast<u32>(want[i].type)) << i;
        EXPECT_EQ(e.hr, want[i].hr) << i;
        EXPECT_EQ(e.state, 0u) << i;          // "state=0"
        EXPECT_EQ(e.player, p) << i;          // "player=1"
        EXPECT_EQ(e.item, 0u) << i;           // "item=0"
        EXPECT_EQ(e.store, 0u) << i;          // "store=0"
        EXPECT_FALSE(e.service) << i;         // "creator=1"
        if (want[i].type == EventType::RateSet) EXPECT_EQ(e.extra, want[i].extra) << i;
    }
    F.events.clear();

    // Async creation: "MEDIAITEM_CREATED hr=0 ... item=1 user=85"; a missing
    // file: "hr=80070002 ... item=0 user=9"; sync creation raises nothing.
    F.put_wstr(F.URL, u"C:\\m\\a.wav");
    EXPECT_EQ(F.player(p, P_CreateMediaItemFromURL, {F.URL, 0, 0x55, 0}), S_OK_HR);
    F.mgr.drain_service_queue();
    EXPECT_TRUE(F.events.empty());
    F.pump();
    ASSERT_EQ(F.events.size(), 1u);
    EXPECT_EQ(F.events[0].type, static_cast<u32>(EventType::MediaItemCreated));
    EXPECT_EQ(F.events[0].hr, S_OK_HR);
    EXPECT_NE(F.events[0].item, 0u);
    EXPECT_EQ(F.events[0].extra, 0x55u);
    // The event's reference ends with the callback: nobody kept the item.
    EXPECT_EQ(F.mgr.live_item_count(), 0u);
    F.put_wstr(F.URL2, u"C:\\m\\missing.wav");
    EXPECT_EQ(F.player(p, P_CreateMediaItemFromURL, {F.URL2, 0, 9, 0}), S_OK_HR);
    F.mgr.drain_service_queue();
    F.pump();
    ASSERT_EQ(F.events.size(), 2u);
    EXPECT_EQ(F.events[1].hr, FILE_NOT_FOUND_HR);
    EXPECT_EQ(F.events[1].item, 0u);
    EXPECT_EQ(F.events[1].extra, 9u);
    // Learn: an async call with an out pointer is E_INVALIDARG.
    EXPECT_EQ(F.player(p, P_CreateMediaItemFromURL, {F.URL, 0, 0, F.OUT}), E_INVALIDARG_HR);
    F.events.clear();
    const GuestAddr it = F.make_item(p);
    F.pump();
    EXPECT_TRUE(F.events.empty());  // "events from sync create -> 0"

    // Success path (Learn): SetMediaItem -> MEDIAITEM_SET (STOPPED); the item
    // is not visible to GetMediaItem until then.
    EXPECT_EQ(F.player(p, P_SetMediaItem, {it}), S_OK_HR);
    EXPECT_EQ(F.player(p, P_GetMediaItem, {F.OUT}), MF_E_NOT_FOUND_HR);
    F.wr32(F.OUT, 99);
    EXPECT_EQ(F.player(p, P_GetState, {F.OUT}), S_OK_HR);
    EXPECT_EQ(F.rd32(F.OUT), 0u);  // not until the event
    F.pump();
    ASSERT_EQ(F.events.size(), 1u);
    EXPECT_EQ(F.events[0].type, static_cast<u32>(EventType::MediaItemSet));
    EXPECT_EQ(F.events[0].hr, S_OK_HR);
    EXPECT_EQ(F.events[0].state, 1u);  // STOPPED
    EXPECT_EQ(F.events[0].item, it);
    EXPECT_EQ(F.player(p, P_GetMediaItem, {F.OUT}), S_OK_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), it);
    EXPECT_EQ(F.item(it, I_Release), 2u);  // the player holds the item
    F.wr32(F.OUT, 99);
    EXPECT_EQ(F.player(p, P_GetState, {F.OUT}), S_OK_HR);
    EXPECT_EQ(F.rd32(F.OUT), 1u);
    // Duration/rates with an item: player VT_UI8 (Learn), forward [1, 1].
    F.put_pv(F.PV, 0xCCCC, 0);
    EXPECT_EQ(F.player(p, P_GetDuration, {F.GUIDBUF, F.PV}), S_OK_HR);
    EXPECT_EQ(F.pv_vt(F.PV), VT_UI8);
    EXPECT_EQ(F.pv_u64(F.PV), 2500000u);
    EXPECT_EQ(F.player(p, P_GetSupportedRates, {1, F.OUT, F.OUT2}), S_OK_HR);
    EXPECT_EQ(F.rd32(F.OUT), F_ONE);
    EXPECT_EQ(F.rd32(F.OUT2), F_ONE);
    EXPECT_EQ(F.player(p, P_GetSupportedRates, {0, F.OUT, F.OUT2}), MF_E_UNSUPPORTED_RATE_HR);
    // Rate: clamped to the nearest supported rate (Learn), reported in flRate.
    EXPECT_EQ(F.player(p, P_SetRate, {0x40000000u}, 0), S_OK_HR);
    // Seek, play, end.
    F.put_pv(F.PV, VT_I8, 500000);
    EXPECT_EQ(F.player(p, P_SetPosition, {F.GUIDBUF, F.PV}), S_OK_HR);
    EXPECT_EQ(F.media.last_seek, 500000);
    F.put_pv(F.PV, VT_I8, 100000000);
    EXPECT_EQ(F.player(p, P_SetPosition, {F.GUIDBUF, F.PV}), S_OK_HR);
    EXPECT_EQ(F.player(p, P_Play), S_OK_HR);
    EXPECT_EQ(F.media.runs, 1);
    F.events.clear();
    F.pump();
    ASSERT_EQ(F.events.size(), 4u);
    EXPECT_EQ(F.events[0].type, static_cast<u32>(EventType::RateSet));
    EXPECT_EQ(F.events[0].hr, S_OK_HR);
    EXPECT_EQ(F.events[0].extra, F_ONE);
    EXPECT_EQ(F.events[1].type, static_cast<u32>(EventType::PositionSet));
    EXPECT_EQ(F.events[1].hr, S_OK_HR);
    EXPECT_EQ(F.events[2].hr, MF_E_INVALID_POSITION_HR);  // beyond the duration
    EXPECT_EQ(F.events[3].type, static_cast<u32>(EventType::Play));
    EXPECT_EQ(F.events[3].hr, S_OK_HR);
    EXPECT_EQ(F.events[3].state, 2u);  // PLAYING
    EXPECT_EQ(F.events[3].item, it);
    F.put_pv(F.PV, 0xCCCC, 0);
    EXPECT_EQ(F.player(p, P_GetPosition, {F.GUIDBUF, F.PV}), S_OK_HR);
    EXPECT_EQ(F.pv_vt(F.PV), VT_I8);
    EXPECT_EQ(F.pv_u64(F.PV), 500000u);
    F.media.completed = true;
    F.mgr.tick();
    F.events.clear();
    F.pump();
    ASSERT_EQ(F.events.size(), 1u);
    EXPECT_EQ(F.events[0].type, static_cast<u32>(EventType::PlaybackEnded));
    EXPECT_EQ(F.events[0].state, 1u);  // STOPPED
    EXPECT_EQ(F.events[0].item, it);
    EXPECT_EQ(F.media.last_seek, 0);
    // Pause, Stop with an item.
    EXPECT_EQ(F.player(p, P_Pause), S_OK_HR);
    EXPECT_EQ(F.player(p, P_Stop), S_OK_HR);
    F.events.clear();
    F.pump();
    ASSERT_EQ(F.events.size(), 2u);
    EXPECT_EQ(F.events[0].state, 3u);  // PAUSED
    EXPECT_EQ(F.events[1].state, 1u);  // STOPPED
    // Clear: MEDIAITEM_CLEARED carries the cleared item, state EMPTY.
    EXPECT_EQ(F.player(p, P_ClearMediaItem), S_OK_HR);
    F.events.clear();
    F.pump();
    ASSERT_EQ(F.events.size(), 1u);
    EXPECT_EQ(F.events[0].type, static_cast<u32>(EventType::MediaItemCleared));
    EXPECT_EQ(F.events[0].item, it);
    EXPECT_EQ(F.events[0].state, 0u);
    EXPECT_EQ(F.item(it, I_AddRef), 2u);  // the player let go
    EXPECT_EQ(F.item(it, I_Release), 1u);

    // Shutdown drops queued events and releases the callback
    // ("callback refs after Shutdown -> 1").
    EXPECT_EQ(F.player(p, P_Play), S_OK_HR);
    EXPECT_EQ(F.player(p, P_Shutdown), S_OK_HR);
    F.events.clear();
    F.pump();
    EXPECT_TRUE(F.events.empty());
    EXPECT_EQ(F.cb_refs, 1);
    EXPECT_EQ(F.item(it, I_Release), 0u);
    EXPECT_EQ(F.player(p, P_Release), 0u);
}

BOTH(CreateWithUrlRaisesSetAndPlayOnlyWhenStarting) {
    // "create(wav, start TRUE, callback)": MEDIAITEM_SET, then PLAY.
    F.put_wstr(F.URL, u"C:\\m\\a.wav");
    EXPECT_EQ(F.call("MFPCreateMediaPlayer", {F.URL, 1, 0, F.CB, 0, F.OUT}), S_OK_HR);
    const GuestAddr p = static_cast<GuestAddr>(F.rd_ptr(F.OUT));
    EXPECT_TRUE(F.events.empty());
    F.pump();
    ASSERT_EQ(F.events.size(), 2u);
    EXPECT_EQ(F.events[0].type, static_cast<u32>(EventType::MediaItemSet));
    EXPECT_EQ(F.events[1].type, static_cast<u32>(EventType::Play));
    EXPECT_EQ(F.events[1].state, 2u);
    EXPECT_EQ(F.player(p, P_Release), 0u);
    // "create(wav, start FALSE, callback)": nothing.
    F.events.clear();
    EXPECT_EQ(F.call("MFPCreateMediaPlayer", {F.URL, 0, 0, F.CB, 0, F.OUT}), S_OK_HR);
    const GuestAddr q = static_cast<GuestAddr>(F.rd_ptr(F.OUT));
    F.pump();
    EXPECT_TRUE(F.events.empty());
    EXPECT_EQ(F.player(q, P_GetMediaItem, {F.OUT2}), MF_E_NOT_FOUND_HR);
    EXPECT_EQ(F.player(q, P_Release), 0u);
    // "create(missing, start TRUE, callback) -> 80070002 0": no player, no events.
    F.put_wstr(F.URL, u"C:\\m\\missing.wav");
    EXPECT_EQ(F.call("MFPCreateMediaPlayer", {F.URL, 1, 0, F.CB, 0, F.OUT}), FILE_NOT_FOUND_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 0u);
    F.pump();
    EXPECT_TRUE(F.events.empty());
    EXPECT_EQ(F.cb_refs, 1);
    EXPECT_EQ(F.mgr.live_player_count(), 0u);
    // Fire and forget (ppMediaPlayer NULL, Learn): plays, ends, goes away.
    F.put_wstr(F.URL, u"C:\\m\\a.wav");
    EXPECT_EQ(F.call("MFPCreateMediaPlayer", {F.URL, 1, 0, 0, 0, 0}), S_OK_HR);
    EXPECT_EQ(F.mgr.live_player_count(), 1u);
    F.media.completed = true;
    F.mgr.tick();
    F.pump();
    EXPECT_EQ(F.mgr.live_player_count(), 0u);
    EXPECT_EQ(F.mgr.live_item_count(), 0u);
}

// ===========================================================================
// Events, MFP_OPTION_FREE_THREADED_CALLBACK (probe "== events (FREE_THREADED)")
// ===========================================================================

BOTH(FreeThreadedEventsNeedNoMessageLoop) {
    const int windows_before = F.windows_created;
    const GuestAddr p = F.create(MFP_OPTION_FREE_THREADED_CALLBACK, F.CB);
    EXPECT_EQ(F.windows_created, windows_before);  // "thread windows added -> 0"
    F.put_wstr(F.URL, u"C:\\m\\a.wav");
    EXPECT_EQ(F.player(p, P_CreateMediaItemFromURL, {F.URL, 0, 0x77, 0}), S_OK_HR);
    EXPECT_EQ(F.player(p, P_Play), S_OK_HR);
    F.mgr.drain_service_queue();
    EXPECT_TRUE(F.posted.empty());
    ASSERT_EQ(F.events.size(), 2u);
    for (const EventRec& e : F.events) EXPECT_TRUE(e.service);  // "creator=0"
    // Order of the two independent operations is not fixed; find each.
    const auto created = std::find_if(F.events.begin(), F.events.end(), [](const EventRec& e) {
        return e.type == static_cast<u32>(EventType::MediaItemCreated);
    });
    ASSERT_NE(created, F.events.end());
    EXPECT_EQ(created->extra, 0x77u);
    EXPECT_EQ(F.player(p, P_Shutdown), S_OK_HR);
    EXPECT_EQ(F.cb_refs, 1);
    EXPECT_EQ(F.player(p, P_Release), 0u);
}

// ===========================================================================
// Backend: audio controls and video presentation
// ===========================================================================

BOTH(AudioControlsReachTheBackend) {
    const GuestAddr p = F.create();
    const GuestAddr it = F.make_item(p);
    EXPECT_EQ(F.player(p, P_SetMediaItem, {it}), S_OK_HR);
    EXPECT_EQ(F.media.volume, 0);  // 1.0 = 0 dB
    EXPECT_EQ(F.player(p, P_SetVolume, {0x3f000000u}, 0), S_OK_HR);  // 0.5
    EXPECT_EQ(F.media.volume, -602);  // 20*log10(0.5) dB in hundredths
    EXPECT_EQ(F.player(p, P_SetMute, {1}), S_OK_HR);
    EXPECT_EQ(F.media.volume, -10000);
    EXPECT_EQ(F.player(p, P_SetMute, {0}), S_OK_HR);
    EXPECT_EQ(F.player(p, P_SetBalance, {0xbf800000u}, 0), S_OK_HR);  // -1: left only
    EXPECT_EQ(F.media.balance, -10000);
    EXPECT_EQ(F.player(p, P_SetBalance, {F_ONE}, 0), S_OK_HR);
    EXPECT_EQ(F.media.balance, 10000);
    EXPECT_EQ(F.item(it, I_Release), 1u);
    EXPECT_EQ(F.player(p, P_Release), 0u);
}

BOTH(VideoIsPresentedIntoTheWindow) {
    F.media.has_audio = false;
    F.media.has_video = true;
    F.media.width = 320;
    F.media.height = 240;
    const GuestAddr p = F.create(0, F.CB, F.VIDEO_HWND);
    const GuestAddr it = F.make_item(p);
    EXPECT_EQ(F.item(it, I_HasVideo, {F.OUT, F.OUT2}), S_OK_HR);
    EXPECT_EQ(F.rd32(F.OUT), 1u);
    EXPECT_EQ(F.player(p, P_SetMediaItem, {it}), S_OK_HR);
    // GetNativeVideoSize: pixels and the reduced picture aspect ratio.
    EXPECT_EQ(F.player(p, P_GetNativeVideoSize, {F.OUT, F.OUT2}), S_OK_HR);
    EXPECT_EQ(F.rd32(F.OUT), 320u);
    EXPECT_EQ(F.rd32(F.OUT + 4), 240u);
    EXPECT_EQ(F.rd32(F.OUT2), 4u);
    EXPECT_EQ(F.rd32(F.OUT2 + 4), 3u);
    EXPECT_EQ(F.player(p, P_GetNativeVideoSize, {0, 0}), E_POINTER_HR);
    EXPECT_EQ(F.player(p, P_GetIdealVideoSize, {F.OUT, F.OUT2}), E_NOTIMPL_HR);
    // Defaults: whole picture, letterboxed in black (Learn).
    EXPECT_EQ(F.player(p, P_GetAspectRatioMode, {F.OUT}), S_OK_HR);
    EXPECT_EQ(F.rd32(F.OUT), 1u);
    EXPECT_EQ(F.player(p, P_GetBorderColor, {F.OUT}), S_OK_HR);
    EXPECT_EQ(F.rd32(F.OUT), 0u);
    EXPECT_EQ(F.player(p, P_GetVideoSourceRect, {F.PV}), S_OK_HR);
    EXPECT_EQ(F.rd32(F.PV + 8), F_ONE);
    EXPECT_EQ(F.player(p, P_SetAspectRatioMode, {8}), E_INVALIDARG_HR);
    // An inverted source rectangle is refused.
    F.wr32(F.PV, 0x3f19999au);       // 0.6
    F.wr32(F.PV + 4, 0);
    F.wr32(F.PV + 8, 0x3ecccccdu);   // 0.4
    F.wr32(F.PV + 12, F_ONE);
    EXPECT_EQ(F.player(p, P_SetVideoSourceRect, {F.PV}), E_INVALIDARG_HR);
    EXPECT_EQ(F.player(p, P_SetVideoSourceRect, {0}), E_POINTER_HR);
    // Play: the 4:3 frame is letterboxed to the 16:9 client (640x360).
    EXPECT_EQ(F.player(p, P_Play), S_OK_HR);
    F.media.position = 1;
    F.mgr.tick();
    ASSERT_FALSE(F.presented.empty());
    EXPECT_EQ(F.presented.back().hwnd, F.VIDEO_HWND);
    EXPECT_EQ(F.presented.back().w, 427);
    EXPECT_EQ(F.presented.back().h, 240);
    // Stretch: the frame as decoded.
    EXPECT_EQ(F.player(p, P_SetAspectRatioMode, {0}), S_OK_HR);
    EXPECT_EQ(F.presented.back().w, 320);
    // FrameStep on video: the source cannot step (Learn MF_E_UNSUPPORTED_RATE).
    EXPECT_EQ(F.player(p, P_FrameStep), S_OK_HR);
    F.events.clear();
    F.pump();
    ASSERT_FALSE(F.events.empty());
    EXPECT_EQ(F.events.back().type, static_cast<u32>(EventType::FrameStep));
    EXPECT_EQ(F.events.back().hr, MF_E_UNSUPPORTED_RATE_HR);
    // Stop hides the video (MFPlay guide).
    EXPECT_EQ(F.player(p, P_Stop), S_OK_HR);
    EXPECT_EQ(F.hidden.back(), F.VIDEO_HWND);
    EXPECT_EQ(F.item(it, I_Release), 1u);
    EXPECT_EQ(F.player(p, P_Release), 0u);
}

BOTH(UnimplementedPathsFailWithDocumentedCodes) {
    const GuestAddr p = F.create(0, F.CB);
    // CreateMediaItemFromObject: no source path; argument checks first.
    EXPECT_EQ(F.player(p, P_CreateMediaItemFromObject, {F.EFFECT, 0, 0, F.OUT}), E_INVALIDARG_HR);
    F.wr_ptr(F.OUT, 1);
    EXPECT_EQ(F.player(p, P_CreateMediaItemFromObject, {F.EFFECT, 1, 0, F.OUT}), E_NOTIMPL_HR);
    EXPECT_EQ(F.rd_ptr(F.OUT), 0u);
    // A required effect cannot be added: MEDIAITEM_SET fails (Learn).
    const GuestAddr it = F.make_item(p);
    EXPECT_EQ(F.player(p, P_InsertEffect, {F.EFFECT, 0}), S_OK_HR);
    EXPECT_EQ(F.player(p, P_SetMediaItem, {it}), S_OK_HR);
    F.pump();
    ASSERT_EQ(F.events.size(), 1u);
    EXPECT_EQ(F.events[0].type, static_cast<u32>(EventType::MediaItemSet));
    EXPECT_EQ(F.events[0].hr, MF_E_TOPO_UNSUPPORTED_HR);
    EXPECT_EQ(F.player(p, P_GetMediaItem, {F.OUT}), MF_E_NOT_FOUND_HR);
    // QueryInterface(IPropertyStore): not offered.
    const G IID_IPropertyStore_G{0x886d8eeb, 0x8cf2, 0x4446, {0x8d, 0x02, 0xcd, 0xba, 0x1d, 0xbd, 0xcf, 0x99}};
    F.put_guid(F.GUIDBUF, IID_IPropertyStore_G);
    EXPECT_EQ(F.player(p, P_QueryInterface, {F.GUIDBUF, F.OUT}), E_NOINTERFACE_HR);
    EXPECT_EQ(F.call("MFPCreateMediaPlayerEx", {0, 0, 0, 0, 0, 0, 0}), E_NOTIMPL_HR);
    EXPECT_EQ(F.player(p, P_Shutdown), S_OK_HR);
    EXPECT_EQ(F.effect_refs, 1);  // released with the player
    EXPECT_EQ(F.item(it, I_Release), 0u);
    EXPECT_EQ(F.player(p, P_Release), 0u);
}
