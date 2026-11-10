/// @file test_avifil32.cpp
/// @brief avifil32.dll against its Windows recording.
///
/// tools/oracle_probe/avifil32_probe.c ran on Windows 11 (x86 and x64) over
/// the files avifil32_fixtures.h builds; its output is
/// avifil32_probe{32,64}.expected. This test builds the same files, replays
/// the probe's calls through Vitreo's exports and COM vtables in both
/// bitnesses, prints the same lines, and requires each of them to appear in
/// the recording in order. Expected values therefore come from Windows only.
/// Sections the replay leaves out (the export walk and layouts, checked
/// separately below; the EditStream rounding section, whose EditStream*
/// exports are not implemented) are skipped, not re-derived.

#include <gtest/gtest.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/media/avifil32/avifil32.hpp"
#include "winapi/media/avifil32/avifil32_contract.hpp"
#include "winapi/media/msvfw32/msvfw32.hpp"

#include "../../oracle_data/avifil32_fixtures.h"

using namespace vitreo;
using namespace vitreo::winapi;
namespace av = vitreo::winapi::avifil32;

namespace {

constexpr GuestAddr kHeap = 0x10000000;
constexpr u64 kHeapSize = 32u << 20;
constexpr GuestAddr kStack = 0x80000;
constexpr GuestAddr kThunks = 0x70000000;
constexpr GuestAddr kUntouched = 0x1234;
constexpr u32 kAviIfKeyframe = 0x10;

std::vector<std::string> read_lines(const std::string& path) {
    std::ifstream in(path);
    std::vector<std::string> out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
    }
    return out;
}

std::string expected_path(bool x64) {
    return std::string(VITREO_SOURCE_DIR) + "/tests/oracle_data/avifil32_probe" +
           (x64 ? "64" : "32") + ".expected";
}

u32 fnv(const std::vector<u8>& b) {
    u32 h = 2166136261u;
    for (u8 c : b) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

std::string fmt(const char* f, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string hexs(const std::vector<u8>& b) {
    std::string s;
    for (u8 c : b) s += fmt("%02x", c);
    return s;
}

const ExportEntry* find_export(const DllExports& exports, const std::string& name) {
    for (const auto& e : exports.entries) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

/// A guest process image for avifil32: memory, a CPU of either bitness, the
/// registered DLL and a bump heap.
class Guest {
public:
    explicit Guest(bool x64) : x64_(x64), thunks_(kThunks), m_(memory_) {
        EXPECT_TRUE(memory_.map_region(kHeap, kHeapSize, MemoryProtection::ReadWrite).is_ok());
        EXPECT_TRUE(memory_.map_region(kStack, 0x10000, MemoryProtection::ReadWrite).is_ok());
        EXPECT_TRUE(memory_.map_region(kThunks, 0x20000, MemoryProtection::ReadWrite).is_ok());
        if (x64) cpu_.init_long_mode(); else cpu_.init_protected_mode();
        cpu_.gpr.rsp = kStack + 0x8000;
        m_.attach_cpu(cpu_);
        mgr_.set_thunk_registry(&thunks_);
        mgr_.set_heap(Avifil32Manager::Heap{[this](u64 n) { return alloc(n); }, [](GuestAddr) {}});
        mgr_.set_file_opener([this](const std::string& path) {
            Avifil32Manager::OpenedFile out;
            const auto slash = path.find_last_of("\\/");
            const std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
            if (leaf.empty()) {
                out.exists = true; // the directory itself
                return out;
            }
            auto it = files_.find(leaf);
            if (it == files_.end()) return out;
            out.exists = true;
            out.source = std::make_shared<av::MemorySource>(it->second);
            return out;
        });
        mgr_.set_com(Avifil32Manager::Com{[this](Marshaler&) { return ole_ > 0; },
                                          [this](Marshaler&) { ++ole_; },
                                          [this](Marshaler&) { --ole_; }});
        exports_ = create_avifil32_thunks(mgr_);
        DllExports copy = exports_;
        EXPECT_TRUE(thunks_.register_dll("avifil32.dll", std::move(copy)).is_ok());
        for (int k = 0; k < AVIFX_COUNT; ++k) {
            std::vector<u8> b(65536);
            b.resize(avifx_build(k, b.data(), b.size()));
            files_[avifx_names[k]] = b;
        }
        files_["rgb24.bin"] = files_["rgb24.avi"];
        files_["tonewav.avi"] = files_["tone.wav"];
    }

    [[nodiscard]] bool x64() const { return x64_; }
    [[nodiscard]] u32 ptr() const { return x64_ ? 8 : 4; }
    Avifil32Manager& mgr() { return mgr_; }
    const DllExports& exports() const { return exports_; }

    GuestAddr alloc(u64 n) {
        const GuestAddr p = next_;
        next_ += (n + 15) & ~u64{15};
        return p;
    }
    GuestAddr str(const std::string& s) {
        const GuestAddr p = alloc(s.size() + 1);
        (void)memory_.write_bytes(p, reinterpret_cast<const Byte*>(s.c_str()), s.size() + 1);
        return p;
    }
    GuestAddr wstr(const std::string& s) {
        const GuestAddr p = alloc((s.size() + 1) * 2);
        for (usize i = 0; i <= s.size(); ++i) {
            (void)memory_.write_u16(p + i * 2, i < s.size() ? static_cast<u8>(s[i]) : 0);
        }
        return p;
    }
    GuestAddr guid(u32 data1) {
        const GuestAddr p = alloc(16);
        const auto g = av::ole_guid(data1);
        (void)memory_.write_bytes(p, reinterpret_cast<const Byte*>(g.data()), 16);
        return p;
    }
    GuestAddr guid_bytes(const std::array<u8, 16>& g) {
        const GuestAddr p = alloc(16);
        (void)memory_.write_bytes(p, reinterpret_cast<const Byte*>(g.data()), 16);
        return p;
    }
    GuestAddr out_ptr(u64 init = kUntouched) {
        const GuestAddr p = alloc(8);
        (void)m_.write_ptr(p, init);
        return p;
    }
    u64 rd_ptr(GuestAddr p) { return m_.read_ptr(p).value(); }
    u32 rd32(GuestAddr p) { return memory_.read_u32(p).value(); }
    i32 rdi(GuestAddr p) { return static_cast<i32>(rd32(p)); }
    void wr32(GuestAddr p, u32 v) { (void)memory_.write_u32(p, v); }
    void wr16(GuestAddr p, u16 v) { (void)memory_.write_u16(p, v); }
    void wrptr(GuestAddr p, u64 v) { (void)m_.write_ptr(p, v); }
    std::map<std::string, std::vector<u8>>& files() { return files_; }
    std::vector<u8> bytes(GuestAddr p, usize n) {
        std::vector<u8> b(n);
        if (n) (void)memory_.read_bytes(p, reinterpret_cast<Byte*>(b.data()), n);
        return b;
    }
    void fill(GuestAddr p, usize n, u8 v) {
        std::vector<u8> b(n, v);
        (void)memory_.write_bytes(p, reinterpret_cast<const Byte*>(b.data()), n);
    }

    void place(const std::vector<u64>& args) {
        if (x64_) {
            u64* regs[4] = {&cpu_.gpr.rcx, &cpu_.gpr.rdx, &cpu_.gpr.r8, &cpu_.gpr.r9};
            for (usize i = 0; i < args.size(); ++i) {
                if (i < 4) *regs[i] = args[i];
                else (void)memory_.write_u64(cpu_.gpr.rsp + 0x28 + (i - 4) * 8, args[i]);
            }
        } else {
            for (usize i = 0; i < args.size(); ++i) {
                (void)memory_.write_u32(cpu_.gpr.rsp + 4 + i * 4, static_cast<u32>(args[i]));
            }
        }
    }
    u64 ret() const { return x64_ ? cpu_.gpr.rax : (cpu_.gpr.rax & 0xFFFFFFFFu); }

    /// Call an avifil32 export.
    u64 call(const std::string& name, const std::vector<u64>& args) {
        const ExportEntry* e = find_export(exports_, name);
        EXPECT_NE(e, nullptr) << name;
        if (e == nullptr) return 0xDEADBEEF;
        place(args);
        call_export(*e, m_);
        return ret();
    }
    /// Call vtable slot @p slot of @p obj the way guest code does.
    u64 vcall(GuestAddr obj, u32 slot, std::vector<u64> args) {
        const GuestAddr vtbl = static_cast<GuestAddr>(rd_ptr(obj));
        const GuestAddr fn = static_cast<GuestAddr>(rd_ptr(vtbl + slot * ptr()));
        auto h = thunks_.resolve_handler(fn);
        EXPECT_TRUE(h.is_ok()) << "slot " << slot;
        if (h.is_err()) return 0xDEADBEEF;
        args.insert(args.begin(), obj);
        place(args);
        (*h.value())(m_);
        return ret();
    }

private:
    bool x64_;
    GuestMemory memory_;
    cpu::CpuState cpu_;
    ThunkRegistry thunks_;
    Marshaler m_;
    Avifil32Manager mgr_;
    DllExports exports_;
    GuestAddr next_{kHeap};
    int ole_{0};
    std::map<std::string, std::vector<u8>> files_;
};

constexpr u32 kAVIFILEINFOA = 108, kAVIFILEINFOW = 172;
constexpr u32 kAVISTREAMINFOA = 140, kAVISTREAMINFOW = 204;
constexpr const char* kDir = "C:\\avifil32_probe";

std::string path_of(const char* name) { return std::string(kDir) + "\\" + name; }

/// The probe's printing, line for line, driven through Vitreo.
class Replay {
public:
    explicit Replay(Guest& g) : g_(g) {}
    std::vector<std::string> lines;

    void out(const std::string& s) { lines.push_back(s); }
    std::string ptr_state(u64 v) {
        return v == 0 ? "NULL" : (v == kUntouched ? "untouched" : "set");
    }
    void hexline(const std::string& tag, const std::vector<u8>& b) {
        out("avi " + tag + " " + hexs(b));
    }
    static std::string fcc_str(u32 v) {
        std::string s;
        for (int i = 0; i < 4; ++i) {
            const unsigned c = (v >> (8 * i)) & 0xFF;
            s.push_back(c >= 32 && c < 127 ? static_cast<char>(c) : '.');
        }
        return s;
    }
    std::string cstr(const std::vector<u8>& b, usize at) {
        std::string s;
        for (usize i = at; i < b.size() && b[i]; ++i) s.push_back(static_cast<char>(b[i]));
        return s;
    }
    std::string wname(const std::vector<u8>& b, usize at) {
        std::string s;
        for (usize i = at; i + 1 < b.size(); i += 2) {
            const u16 c = static_cast<u16>(b[i] | (b[i + 1] << 8));
            if (!c) break;
            s.push_back(static_cast<char>(c));
        }
        return s;
    }
    static u32 d(const std::vector<u8>& b, usize at) {
        return static_cast<u32>(b[at]) | (static_cast<u32>(b[at + 1]) << 8) |
               (static_cast<u32>(b[at + 2]) << 16) | (static_cast<u32>(b[at + 3]) << 24);
    }
    static i32 l(const std::vector<u8>& b, usize at) { return static_cast<i32>(d(b, at)); }

    void dump_file_info(const std::string& tag, const std::vector<u8>& f) {
        out(fmt("avi %s maxbps=%u flags=0x%x caps=0x%x streams=%u sugg=%u w=%u h=%u scale=%u "
                "rate=%u len=%u edit=%u type=\"%s\"",
                tag.c_str(), d(f, 0), d(f, 4), d(f, 8), d(f, 12), d(f, 16), d(f, 20), d(f, 24),
                d(f, 28), d(f, 32), d(f, 36), d(f, 40), cstr(f, 44).c_str()));
    }
    void dump_stream_info(const std::string& tag, const std::vector<u8>& s) {
        out(fmt("avi %s type=%s handler=%s(0x%x) flags=0x%x caps=0x%x prio=%u lang=%u scale=%u "
                "rate=%u start=%u len=%u init=%u sugg=%u quality=0x%x ssize=%u "
                "rc=%d,%d,%d,%d edit=%u fmtchg=%u name=\"%s\"",
                tag.c_str(), fcc_str(d(s, 0)).c_str(), fcc_str(d(s, 4)).c_str(), d(s, 4), d(s, 8),
                d(s, 12), static_cast<unsigned>(s[16] | (s[17] << 8)),
                static_cast<unsigned>(s[18] | (s[19] << 8)), d(s, 20), d(s, 24), d(s, 28),
                d(s, 32), d(s, 36), d(s, 40), d(s, 44), d(s, 48), l(s, 52), l(s, 56), l(s, 60),
                l(s, 64), d(s, 68), d(s, 72), cstr(s, 76).c_str()));
    }
    void dump_dib(const std::string& tag, GuestAddr bi) {
        if (bi == 0) {
            out("avi " + tag + " NULL");
            return;
        }
        const std::vector<u8> h = g_.bytes(bi, 40);
        const u32 size = d(h, 0);
        const i32 w = l(h, 4), ht = l(h, 8);
        const u32 bpp = static_cast<u32>(h[14] | (h[15] << 8));
        const u32 comp = d(h, 16);
        int ct = static_cast<int>(d(h, 32));
        if (ct == 0 && bpp <= 8) ct = 1 << bpp;
        if (comp == 3) ct = 3;
        const int stride = ((w * static_cast<int>(bpp) + 31) / 32) * 4;
        const int image = stride * (ht < 0 ? -ht : ht);
        const std::vector<u8> table = g_.bytes(bi + size, static_cast<usize>(ct) * 4);
        const std::vector<u8> bits = g_.bytes(bi + size + static_cast<u32>(ct) * 4, static_cast<usize>(image));
        out(fmt("avi %s size=%u w=%d h=%d planes=%u bpp=%u comp=0x%x simage=%u xppm=%d yppm=%d "
                "clrused=%u clrimp=%u ct=%d cthash=%08x bitshash=%08x",
                tag.c_str(), size, w, ht, static_cast<unsigned>(h[12] | (h[13] << 8)), bpp, comp,
                d(h, 20), l(h, 24), l(h, 28), d(h, 32), d(h, 36), ct, fnv(table), fnv(bits)));
        hexline(tag + " bits",
                std::vector<u8>(bits.begin(), bits.begin() + std::min(image, 48)));
        if (ct > 0 && ct <= 256) hexline(tag + " ctab", table);
    }
    void qi_report(const std::string& tag, GuestAddr obj, u32 iid, const char* name) {
        const GuestAddr outp = g_.out_ptr(kUntouched);
        const u32 hr = static_cast<u32>(g_.vcall(obj, 0, {g_.guid(iid), outp}));
        const u64 o = g_.rd_ptr(outp);
        if (static_cast<i32>(hr) >= 0 && o != 0) {
            out(fmt("avi %s qi %s hr=0x%08x same=%d off=%ld", tag.c_str(), name, hr,
                    o == obj ? 1 : 0, static_cast<long>(static_cast<i64>(o - obj))));
            (void)g_.vcall(static_cast<GuestAddr>(o), 2, {});
        } else {
            out(fmt("avi %s qi %s hr=0x%08x out=%s", tag.c_str(), name, hr,
                    o == 0 ? "NULL" : (o == kUntouched ? "untouched" : "other")));
        }
    }
    u32 hr(u64 v) { return static_cast<u32>(v); }

    void getframe_modes(const std::string& tag, GuestAddr s, i32 len, i32 w, i32 h) {
        static const char* const names[] = {"null",   "best",    "bpp8",    "bpp16", "bpp24",
                                            "bpp32",  "bpp24td", "bpp24x2", "bf565"};
        for (int mode = 0; mode < 9; ++mode) {
            const GuestAddr want = g_.alloc(40 + 1024);
            g_.fill(want, 40 + 1024, 0);
            g_.wr32(want, 40);
            g_.wr32(want + 4, static_cast<u32>(w));
            g_.wr32(want + 8, static_cast<u32>(h));
            auto set16 = [&](GuestAddr at, u32 v) { g_.wr16(at, static_cast<u16>(v)); };
            set16(want + 12, 1);
            u64 arg = want;
            switch (mode) {
            case 0: arg = 0; break;
            case 1: arg = 1; break;
            case 2: set16(want + 14, 8); break;
            case 3: set16(want + 14, 16); break;
            case 4: set16(want + 14, 24); break;
            case 5: set16(want + 14, 32); break;
            case 6: set16(want + 14, 24); g_.wr32(want + 8, static_cast<u32>(-h)); break;
            case 7: set16(want + 14, 24); g_.wr32(want + 4, static_cast<u32>(w * 2)); break;
            case 8:
                set16(want + 14, 16);
                g_.wr32(want + 16, 3);
                g_.wr32(want + 40, 0xF800);
                g_.wr32(want + 44, 0x07E0);
                g_.wr32(want + 48, 0x001F);
                break;
            }
            const GuestAddr pg = static_cast<GuestAddr>(g_.call("AVIStreamGetFrameOpen", {s, arg}));
            out(fmt("avi %s gf %s open=%s", tag.c_str(), names[mode], pg ? "ok" : "NULL"));
            if (!pg) continue;
            GuestAddr prev = 0;
            for (i32 pos = 0; pos < len; ++pos) {
                const GuestAddr f = static_cast<GuestAddr>(g_.call("AVIStreamGetFrame", {pg, static_cast<u32>(pos)}));
                const std::string t = fmt("%s gf %s pos%d", tag.c_str(), names[mode], pos);
                if (f && prev) out(fmt("avi %s same_ptr=%d", t.c_str(), f == prev ? 1 : 0));
                dump_dib(t, f);
                if (f) prev = f;
            }
            for (auto [label, p] : {std::pair<const char*, i32>{"pos-1", -1}, {"posLen", len},
                                    {"posLen+5", len + 5}}) {
                const u64 f = g_.call("AVIStreamGetFrame", {pg, static_cast<u32>(p)});
                out(fmt("avi %s gf %s %s %s", tag.c_str(), names[mode], label, f ? "frame" : "NULL"));
            }
            if (mode == 0) {
                const u32 r1 = hr(g_.vcall(pg, 1, {}));
                const u32 r2 = hr(g_.vcall(pg, 2, {}));
                const u32 hb = hr(g_.vcall(pg, 4, {0, static_cast<u32>(len), 1000}));
                const u64 vf = g_.vcall(pg, 3, {0});
                const u32 he = hr(g_.vcall(pg, 5, {}));
                out(fmt("avi %s gf vt addref=%u release=%u begin=0x%08x getframe=%s end=0x%08x",
                        tag.c_str(), r1, r2, hb, vf ? "frame" : "NULL", he));
                qi_report(tag, pg, av::IID_IUnknown_Data1, "gf.IUnknown");
                qi_report(tag, pg, av::IID_IGetFrame_Data1, "gf.IGetFrame");
                const GuestAddr o = g_.out_ptr(0);
                const u32 hq = hr(g_.vcall(pg, 0, {g_.guid(av::IID_IAVIStream_Data1), o}));
                out(fmt("avi %s qi gf.IAVIStream hr=0x%08x is_stream=%d", tag.c_str(), hq,
                        g_.rd_ptr(o) == s ? 1 : 0));
                if (static_cast<i32>(hq) >= 0 && g_.rd_ptr(o)) {
                    (void)g_.vcall(static_cast<GuestAddr>(g_.rd_ptr(o)), 2, {});
                }
            }
            out(fmt("avi %s gf %s close=0x%08x", tag.c_str(), names[mode],
                    hr(g_.call("AVIStreamGetFrameClose", {pg}))));
        }
    }

    void stream_probe(const std::string& tag, GuestAddr s, bool video, bool rle) {
        const GuestAddr si = g_.alloc(256);
        g_.fill(si, kAVISTREAMINFOA, 0xCC);
        u32 r = hr(g_.call("AVIStreamInfoA", {s, si, kAVISTREAMINFOA}));
        out(fmt("avi %s infoA hr=0x%08x", tag.c_str(), r));
        const std::vector<u8> sia = g_.bytes(si, kAVISTREAMINFOA);
        dump_stream_info(tag, sia);
        const GuestAddr siw = g_.alloc(256);
        g_.fill(siw, kAVISTREAMINFOW, 0xCC);
        r = hr(g_.call("AVIStreamInfoW", {s, siw, kAVISTREAMINFOW}));
        const std::vector<u8> sw = g_.bytes(siw, kAVISTREAMINFOW);
        out(fmt("avi %s infoW hr=0x%08x name=\"%s\" len=%u same=%d", tag.c_str(), r,
                wname(sw, 76).c_str(), d(sw, 32),
                std::memcmp(sw.data(), sia.data(), 76) == 0 ? 1 : 0));
        const GuestAddr raw = g_.alloc(256);
        g_.fill(raw, 256, 0xCC);
        r = hr(g_.call("AVIStreamInfoA", {s, raw, 20}));
        out(fmt("avi %s infoA20 hr=0x%08x", tag.c_str(), r));
        hexline("  raw24", g_.bytes(raw, 24));
        g_.fill(raw, 256, 0xCC);
        r = hr(g_.call("AVIStreamInfoA", {s, raw, 0}));
        out(fmt("avi %s infoA0 hr=0x%08x first=%02x", tag.c_str(), r, g_.bytes(raw, 1)[0]));
        g_.fill(raw, 256, 0xCC);
        r = hr(g_.call("AVIStreamInfoA", {s, raw, 256}));
        out(fmt("avi %s infoA256 hr=0x%08x tail=%02x", tag.c_str(), r,
                g_.bytes(raw + kAVISTREAMINFOA + 2, 1)[0]));

        const i32 start = static_cast<i32>(g_.call("AVIStreamStart", {s}));
        const i32 len = static_cast<i32>(g_.call("AVIStreamLength", {s}));
        out(fmt("avi %s start=%d length=%d", tag.c_str(), start, len));
        for (i32 i = -1; i <= len + 1; ++i) {
            out(fmt("avi %s s2t %d=%d", tag.c_str(), i,
                    static_cast<i32>(g_.call("AVIStreamSampleToTime", {s, static_cast<u32>(i)}))));
        }
        out(fmt("avi %s s2t 1000=%d", tag.c_str(),
                static_cast<i32>(g_.call("AVIStreamSampleToTime", {s, 1000}))));
        for (i32 t : {-1, 0, 1, 49, 50, 66, 67, 99, 100, 133, 134, 200, 1000, 100000}) {
            out(fmt("avi %s t2s %d=%d", tag.c_str(), t,
                    static_cast<i32>(g_.call("AVIStreamTimeToSample", {s, static_cast<u32>(t)}))));
        }
        const u32 flagsets[] = {0x11, 0x14, 0x21, 0x24, 0x41, 0x44, 0x18, 0, 0x10, 0x20};
        const i32 lim = len < 6 ? len : 6;
        for (u32 f : flagsets) {
            std::string line = fmt("avi %s find flags=0x%x:", tag.c_str(), f);
            for (i32 i = -2; i <= lim + 1; ++i) {
                line += fmt(" %d", static_cast<i32>(g_.call("AVIStreamFindSample",
                                                            {s, static_cast<u32>(i), f})));
            }
            out(line);
        }
        for (u32 rf : {0x1000u, 0x2000u, 0x3000u, 0x4000u}) {
            std::string line = fmt("avi %s findret 0x%x:", tag.c_str(), rf);
            for (i32 i = -1; i <= lim; ++i) {
                line += fmt(" %d", static_cast<i32>(g_.call("AVIStreamFindSample",
                                                            {s, static_cast<u32>(i), 0x24u | rf})));
            }
            out(line);
        }

        const GuestAddr pcb = g_.alloc(8);
        g_.wr32(pcb, 0x7777);
        r = hr(g_.call("AVIStreamReadFormat", {s, 0, 0, pcb}));
        out(fmt("avi %s fmt query hr=0x%08x cb=%d", tag.c_str(), r, g_.rdi(pcb)));
        for (i32 p : {0, len - 1, -1, len, 2}) {
            g_.fill(raw, 256, 0xCC);
            g_.wr32(pcb, 256);
            r = hr(g_.call("AVIStreamReadFormat", {s, static_cast<u32>(p), raw, pcb}));
            const i32 cb = g_.rdi(pcb);
            out(fmt("avi %s fmt pos%d hr=0x%08x cb=%d hash=%08x", tag.c_str(), p, r, cb,
                    fnv(g_.bytes(raw, cb > 0 && cb <= 256 ? static_cast<usize>(cb) : 0))));
        }
        g_.fill(raw, 256, 0xCC);
        g_.wr32(pcb, 8);
        r = hr(g_.call("AVIStreamReadFormat", {s, 0, raw, pcb}));
        out(fmt("avi %s fmt small hr=0x%08x cb=%d", tag.c_str(), r, g_.rdi(pcb)));
        hexline("  fmtsmall", g_.bytes(raw, 12));

        const GuestAddr buf = g_.alloc(4096);
        const GuestAddr pb = g_.alloc(8), ps = g_.alloc(8);
        auto reset = [&] {
            g_.wr32(pb, 0x7777);
            g_.wr32(ps, 0x7777);
        };
        auto bufhash = [&] {
            const i32 b = g_.rdi(pb);
            return fnv(g_.bytes(buf, b > 0 && b <= 4096 ? static_cast<usize>(b) : 0));
        };
        for (i32 st : {0, 1, len - 1, len}) {
            reset();
            r = hr(g_.call("AVIStreamRead", {s, static_cast<u32>(st), 1, 0, 0, pb, ps}));
            out(fmt("avi %s readq start=%d n=1 hr=0x%08x bytes=%d samples=%d", tag.c_str(), st, r,
                    g_.rdi(pb), g_.rdi(ps)));
            reset();
            r = hr(g_.call("AVIStreamRead", {s, static_cast<u32>(st), 0xFFFFFFFFu, 0, 0, pb, ps}));
            out(fmt("avi %s readq start=%d conv hr=0x%08x bytes=%d samples=%d", tag.c_str(), st, r,
                    g_.rdi(pb), g_.rdi(ps)));
            reset();
            g_.fill(buf, 4096, 0xCC);
            r = hr(g_.call("AVIStreamRead", {s, static_cast<u32>(st), 0xFFFFFFFFu, buf, 4096, pb, ps}));
            out(fmt("avi %s read start=%d conv hr=0x%08x bytes=%d samples=%d hash=%08x", tag.c_str(),
                    st, r, g_.rdi(pb), g_.rdi(ps), bufhash()));
            reset();
            g_.fill(buf, 4096, 0xCC);
            r = hr(g_.call("AVIStreamRead", {s, static_cast<u32>(st), 3, buf, 4096, pb, ps}));
            out(fmt("avi %s read start=%d n=3 hr=0x%08x bytes=%d samples=%d hash=%08x", tag.c_str(),
                    st, r, g_.rdi(pb), g_.rdi(ps), bufhash()));
        }
        reset();
        r = hr(g_.call("AVIStreamRead", {s, 0xFFFFFFFFu, 1, buf, 4096, pb, ps}));
        out(fmt("avi %s read start=-1 hr=0x%08x bytes=%d samples=%d", tag.c_str(), r, g_.rdi(pb),
                g_.rdi(ps)));
        reset();
        g_.fill(buf, 4096, 0xCC);
        r = hr(g_.call("AVIStreamRead", {s, 0, 1, buf, 4, pb, ps}));
        out(fmt("avi %s read small hr=0x%08x bytes=%d samples=%d first=%02x", tag.c_str(), r,
                g_.rdi(pb), g_.rdi(ps), g_.bytes(buf, 1)[0]));
        r = hr(g_.call("AVIStreamRead", {s, 0, 1, buf, 4096, 0, 0}));
        out(fmt("avi %s read nullouts hr=0x%08x", tag.c_str(), r));
        if (!video) {
            for (auto [a, n] : {std::pair<i32, i32>{50, 150}, {250, 150}}) {
                reset();
                g_.fill(buf, 4096, 0xCC);
                r = hr(g_.call("AVIStreamRead", {s, static_cast<u32>(a), static_cast<u32>(n), buf, 4096, pb, ps}));
                out(fmt("avi %s read %dx%d hr=0x%08x bytes=%d samples=%d hash=%08x", tag.c_str(), a,
                        n, r, g_.rdi(pb), g_.rdi(ps), bufhash()));
            }
            reset();
            r = hr(g_.call("AVIStreamRead", {s, 10, 150, buf, 7, pb, ps}));
            out(fmt("avi %s read 10x150 into7 hr=0x%08x bytes=%d samples=%d", tag.c_str(), r,
                    g_.rdi(pb), g_.rdi(ps)));
        }

        for (const char* id : {"strd", "strn", "strh", "strf", "vtsd", "indx"}) {
            const u32 ck = av::fourcc(id[0], id[1], id[2], id[3]);
            g_.wr32(pcb, 0x7777);
            r = hr(g_.call("AVIStreamReadData", {s, ck, 0, pcb}));
            out(fmt("avi %s rdata %s query hr=0x%08x cb=%d", tag.c_str(), id, r, g_.rdi(pcb)));
            g_.fill(raw, 256, 0xCC);
            g_.wr32(pcb, 256);
            r = hr(g_.call("AVIStreamReadData", {s, ck, raw, pcb}));
            const i32 cb = g_.rdi(pcb);
            out(fmt("avi %s rdata %s hr=0x%08x cb=%d hash=%08x", tag.c_str(), id, r, cb,
                    fnv(g_.bytes(raw, cb > 0 && cb <= 256 ? static_cast<usize>(cb) : 0))));
            g_.wr32(pcb, 2);
            g_.fill(raw, 256, 0xCC);
            r = hr(g_.call("AVIStreamReadData", {s, ck, raw, pcb}));
            const std::vector<u8> f3 = g_.bytes(raw, 3);
            out(fmt("avi %s rdata %s small hr=0x%08x cb=%d first=%02x%02x%02x", tag.c_str(), id, r,
                    g_.rdi(pcb), f3[0], f3[1], f3[2]));
        }
        {
            const u32 hb = hr(g_.call("AVIStreamBeginStreaming", {s, 0, static_cast<u32>(len), 1000}));
            const u32 he = hr(g_.call("AVIStreamEndStreaming", {s}));
            out(fmt("avi %s begin=0x%08x end=0x%08x", tag.c_str(), hb, he));
        }
        qi_report(tag, s, av::IID_IUnknown_Data1, "IUnknown");
        qi_report(tag, s, av::IID_IAVIStream_Data1, "IAVIStream");
        qi_report(tag, s, av::IID_IAVIStreaming_Data1, "IAVIStreaming");
        qi_report(tag, s, av::IID_IGetFrame_Data1, "IGetFrame");
        qi_report(tag, s, av::IID_IAVIEditStream_Data1, "IAVIEditStream");
        qi_report(tag, s, av::IID_IAVIFile_Data1, "IAVIFile");
        qi_report(tag, s, av::IID_IPersistFile_Data1, "IPersistFile");
        qi_report(tag, s, 0x00000003, "IMarshal");
        {
            const u32 a = hr(g_.vcall(s, 1, {}));
            const u32 rr = hr(g_.vcall(s, 2, {}));
            const i32 ks = static_cast<i32>(g_.vcall(s, 5, {0, 0x11}));
            const u32 hc = hr(g_.vcall(s, 3, {0, 0}));
            const u32 hd = hr(g_.vcall(s, 10, {0, 1}));
            out(fmt("avi %s vt addref=%u release=%u findsample=%d create=0x%08x delete=0x%08x",
                    tag.c_str(), a, rr, ks, hc, hd));
            g_.fill(siw, kAVISTREAMINFOW, 0xCC);
            r = hr(g_.vcall(s, 4, {siw, kAVISTREAMINFOW}));
            out(fmt("avi %s vt info hr=0x%08x len=%u", tag.c_str(), r, g_.rd32(siw + 32)));
            r = hr(g_.vcall(s, 13, {siw, kAVISTREAMINFOW}));
            out(fmt("avi %s vt setinfo hr=0x%08x", tag.c_str(), r));
        }
        {
            const GuestAddr f = g_.alloc(64);
            g_.fill(f, 64, 0);
            out(fmt("avi %s setformat hr=0x%08x", tag.c_str(),
                    hr(g_.call("AVIStreamSetFormat", {s, 0, f, 40}))));
            reset();
            r = hr(g_.call("AVIStreamWrite", {s, static_cast<u32>(len), 1, f, 4, kAviIfKeyframe, ps, pb}));
            out(fmt("avi %s write hr=0x%08x sw=%d bw=%d", tag.c_str(), r, g_.rdi(ps), g_.rdi(pb)));
            out(fmt("avi %s writedata hr=0x%08x", tag.c_str(),
                    hr(g_.call("AVIStreamWriteData", {s, av::fourcc('v', 't', 's', 't'), f, 4}))));
        }
        if (video) {
            i32 w = l(sia, 60) - l(sia, 52), h = l(sia, 64) - l(sia, 56);
            const GuestAddr bih = g_.alloc(40);
            g_.wr32(pcb, 40);
            if (hr(g_.call("AVIStreamReadFormat", {s, 0, bih, pcb})) == 0) {
                w = g_.rdi(bih + 4);
                h = g_.rdi(bih + 8);
            }
            getframe_modes(tag, s, len, w, h);
            if (rle) {
                const GuestAddr pg = static_cast<GuestAddr>(g_.call("AVIStreamGetFrameOpen", {s, 0}));
                if (pg) {
                    dump_dib(tag + " rleorder pos1", static_cast<GuestAddr>(g_.call("AVIStreamGetFrame", {pg, 1})));
                    dump_dib(tag + " rleorder pos0", static_cast<GuestAddr>(g_.call("AVIStreamGetFrame", {pg, 0})));
                    dump_dib(tag + " rleorder pos1b", static_cast<GuestAddr>(g_.call("AVIStreamGetFrame", {pg, 1})));
                    (void)g_.call("AVIStreamGetFrameClose", {pg});
                }
            }
        } else {
            const u64 pg = g_.call("AVIStreamGetFrameOpen", {s, 0});
            out(fmt("avi %s gf audio open=%s", tag.c_str(), pg ? "ok" : "NULL"));
        }
    }

    void file_probe(int which) {
        const std::string name = avifx_names[which];
        const GuestAddr pp = g_.out_ptr();
        u32 r = hr(g_.call("AVIFileOpenA", {pp, g_.str(path_of(name.c_str())), 0x20, 0}));
        const u64 pf64 = g_.rd_ptr(pp);
        out(fmt("avi file %s open hr=0x%08x pf=%s", name.c_str(), r, ptr_state(pf64).c_str()));
        if (static_cast<i32>(r) < 0 || pf64 == 0 || pf64 == kUntouched) return;
        const GuestAddr pf = static_cast<GuestAddr>(pf64);
        const std::string tag = name;
        {
            const u32 a = hr(g_.call("AVIFileAddRef", {pf}));
            const u32 rr = hr(g_.call("AVIFileRelease", {pf}));
            out(fmt("avi %s addref=%u release=%u", tag.c_str(), a, rr));
        }
        const GuestAddr fi = g_.alloc(512);
        g_.fill(fi, kAVIFILEINFOA, 0xCC);
        r = hr(g_.call("AVIFileInfoA", {pf, fi, kAVIFILEINFOA}));
        out(fmt("avi %s infoA hr=0x%08x", tag.c_str(), r));
        const std::vector<u8> fa = g_.bytes(fi, kAVIFILEINFOA);
        dump_file_info(tag, fa);
        const GuestAddr fw = g_.alloc(512);
        g_.fill(fw, kAVIFILEINFOW, 0xCC);
        r = hr(g_.call("AVIFileInfoW", {pf, fw, kAVIFILEINFOW}));
        const std::vector<u8> fwb = g_.bytes(fw, kAVIFILEINFOW);
        out(fmt("avi %s infoW hr=0x%08x type=\"%s\" same=%d", tag.c_str(), r,
                wname(fwb, 44).c_str(), std::memcmp(fwb.data(), fa.data(), 44) == 0 ? 1 : 0));
        const GuestAddr raw = g_.alloc(512);
        g_.fill(raw, 512, 0xCC);
        r = hr(g_.call("AVIFileInfoA", {pf, raw, 12}));
        out(fmt("avi %s infoA12 hr=0x%08x", tag.c_str(), r));
        hexline("  raw16", g_.bytes(raw, 16));
        g_.fill(raw, 512, 0xCC);
        r = hr(g_.call("AVIFileInfoA", {pf, raw, 0}));
        out(fmt("avi %s infoA0 hr=0x%08x first=%02x", tag.c_str(), r, g_.bytes(raw, 1)[0]));
        g_.fill(raw, 512, 0xCC);
        r = hr(g_.call("AVIFileInfoA", {pf, raw, 200}));
        out(fmt("avi %s infoA200 hr=0x%08x tail=%02x", tag.c_str(), r,
                g_.bytes(raw + kAVIFILEINFOA + 2, 1)[0]));
        g_.fill(raw, 512, 0xCC);
        r = hr(g_.call("AVIFileInfo", {pf, raw, kAVIFILEINFOA}));
        out(fmt("avi %s info(plain) hr=0x%08x same=%d", tag.c_str(), r,
                g_.bytes(raw, kAVIFILEINFOA) == fa ? 1 : 0));

        const u32 types[] = {0, av::streamtypeVIDEO, av::streamtypeAUDIO, av::streamtypeMIDI,
                             av::streamtypeTEXT};
        const char* tn[] = {"any", "vids", "auds", "mids", "txts"};
        for (int k = 0; k < 5; ++k) {
            for (i32 i = -1; i < 3; ++i) {
                const GuestAddr ps = g_.out_ptr();
                r = hr(g_.call("AVIFileGetStream", {pf, ps, types[k], static_cast<u32>(i)}));
                const u64 sv = g_.rd_ptr(ps);
                out(fmt("avi %s getstream %s %d hr=0x%08x s=%s", tag.c_str(), tn[k], i, r,
                        ptr_state(sv).c_str()));
                if (static_cast<i32>(r) >= 0 && sv && sv != kUntouched) {
                    (void)g_.call("AVIStreamRelease", {sv});
                }
            }
        }
        GuestAddr first = 0;
        {
            const GuestAddr pa = g_.out_ptr(0), pb = g_.out_ptr(0);
            const u32 h1 = hr(g_.call("AVIFileGetStream", {pf, pa, 0, 0}));
            const u32 h2 = hr(g_.call("AVIFileGetStream", {pf, pb, 0, 0}));
            if (static_cast<i32>(h1) >= 0 && static_cast<i32>(h2) >= 0) {
                const GuestAddr a = static_cast<GuestAddr>(g_.rd_ptr(pa));
                const GuestAddr b = static_cast<GuestAddr>(g_.rd_ptr(pb));
                const u32 ar = hr(g_.call("AVIStreamAddRef", {a}));
                out(fmt("avi %s getstream twice same=%d addref=%u", tag.c_str(), a == b ? 1 : 0, ar));
                const u32 r1 = hr(g_.call("AVIStreamRelease", {a}));
                const u32 r2 = hr(g_.call("AVIStreamRelease", {b}));
                out(fmt("avi %s stream release=%u release=%u", tag.c_str(), r1, r2));
                first = a;
            }
        }
        const GuestAddr pcb = g_.alloc(8);
        for (const char* id : {"vtst", "ISFT", "INFO", "avih", "LIST", "JUNK", "strl"}) {
            const u32 ck = av::fourcc(id[0], id[1], id[2], id[3]);
            g_.wr32(pcb, 0x7777);
            r = hr(g_.call("AVIFileReadData", {pf, ck, 0, pcb}));
            out(fmt("avi %s frdata %s query hr=0x%08x cb=%d", tag.c_str(), id, r, g_.rdi(pcb)));
            g_.fill(raw, 512, 0xCC);
            g_.wr32(pcb, 512);
            r = hr(g_.call("AVIFileReadData", {pf, ck, raw, pcb}));
            const i32 cb = g_.rdi(pcb);
            out(fmt("avi %s frdata %s hr=0x%08x cb=%d hash=%08x", tag.c_str(), id, r, cb,
                    fnv(g_.bytes(raw, cb > 0 && cb <= 512 ? static_cast<usize>(cb) : 0))));
            g_.wr32(pcb, 3);
            g_.fill(raw, 512, 0xCC);
            r = hr(g_.call("AVIFileReadData", {pf, ck, raw, pcb}));
            const std::vector<u8> f4 = g_.bytes(raw, 4);
            out(fmt("avi %s frdata %s small hr=0x%08x cb=%d first=%02x%02x%02x%02x", tag.c_str(), id,
                    r, g_.rdi(pcb), f4[0], f4[1], f4[2], f4[3]));
        }
        {
            const GuestAddr ns = g_.out_ptr();
            const GuestAddr nsi = g_.alloc(kAVISTREAMINFOA);
            g_.fill(nsi, kAVISTREAMINFOA, 0);
            r = hr(g_.call("AVIFileCreateStreamA", {pf, ns, nsi}));
            out(fmt("avi %s createstream hr=0x%08x s=%s", tag.c_str(), r,
                    ptr_state(g_.rd_ptr(ns)).c_str()));
            const u32 hw = hr(g_.call("AVIFileWriteData", {pf, av::fourcc('v', 't', 's', 't'), raw, 4}));
            const u32 hn = hr(g_.call("AVIFileEndRecord", {pf}));
            out(fmt("avi %s writedata hr=0x%08x endrecord=0x%08x", tag.c_str(), hw, hn));
            out(fmt("avi %s vt deletestream hr=0x%08x", tag.c_str(),
                    hr(g_.vcall(pf, 9, {av::streamtypeVIDEO, 0}))));
        }
        qi_report(tag, pf, av::IID_IUnknown_Data1, "IUnknown");
        qi_report(tag, pf, av::IID_IAVIFile_Data1, "IAVIFile");
        qi_report(tag, pf, av::IID_IPersistFile_Data1, "IPersistFile");
        qi_report(tag, pf, av::IID_IAVIPersistFile_Data1, "IAVIPersistFile");
        qi_report(tag, pf, av::IID_IAVIStream_Data1, "IAVIStream");
        qi_report(tag, pf, 0x00000003, "IMarshal");
        {
            g_.fill(fw, kAVIFILEINFOW, 0xCC);
            r = hr(g_.vcall(pf, 3, {fw, kAVIFILEINFOW}));
            out(fmt("avi %s vt info hr=0x%08x streams=%u", tag.c_str(), r, g_.rd32(fw + 12)));
            const GuestAddr vs = g_.out_ptr(0);
            r = hr(g_.vcall(pf, 4, {vs, 0, 0}));
            out(fmt("avi %s vt getstream hr=0x%08x same_as_flat=%d", tag.c_str(), r,
                    g_.rd_ptr(vs) == first ? 1 : 0));
            if (g_.rd_ptr(vs)) (void)g_.vcall(static_cast<GuestAddr>(g_.rd_ptr(vs)), 2, {});
        }
        for (i32 i = 0; i < 4; ++i) {
            const GuestAddr ps = g_.out_ptr(0);
            if (static_cast<i32>(hr(g_.call("AVIFileGetStream", {pf, ps, 0, static_cast<u32>(i)}))) < 0) break;
            const GuestAddr s = static_cast<GuestAddr>(g_.rd_ptr(ps));
            const GuestAddr si = g_.alloc(kAVISTREAMINFOA);
            (void)g_.call("AVIStreamInfoA", {s, si, kAVISTREAMINFOA});
            stream_probe(fmt("%s s%d", tag.c_str(), i), s, g_.rd32(si) == av::streamtypeVIDEO,
                         which == AVIFX_RLE8 || which == AVIFX_RLE4);
            (void)g_.call("AVIStreamRelease", {s});
        }
        refs_block(which, tag, path_of(name.c_str()));
        {
            const GuestAddr ps = g_.out_ptr(0);
            if (static_cast<i32>(hr(g_.call("AVIFileGetStream", {pf, ps, 0, 0}))) >= 0) {
                const GuestAddr s = static_cast<GuestAddr>(g_.rd_ptr(ps));
                const u32 fr = hr(g_.call("AVIFileRelease", {pf}));
                const i32 ln = static_cast<i32>(g_.call("AVIStreamLength", {s}));
                const u32 sr = hr(g_.call("AVIStreamRelease", {s}));
                out(fmt("avi %s lifetime filerelease=%u streamlen=%d streamrelease=%u", tag.c_str(),
                        fr, ln, sr));
                return;
            }
        }
        out(fmt("avi %s final release=%u", tag.c_str(), hr(g_.call("AVIFileRelease", {pf}))));
    }

    void refs_block(int which, const std::string& tag, const std::string& path) {
        const GuestAddr pp = g_.out_ptr(0), ps = g_.out_ptr(0);
        if (static_cast<i32>(hr(g_.call("AVIFileOpenA", {pp, g_.str(path), 0, 0}))) < 0) return;
        const GuestAddr f2 = static_cast<GuestAddr>(g_.rd_ptr(pp));
        if (static_cast<i32>(hr(g_.call("AVIFileGetStream", {f2, ps, 0, 0}))) < 0) return;
        const GuestAddr s2 = static_cast<GuestAddr>(g_.rd_ptr(ps));
        u32 v[8];
        v[0] = hr(g_.call("AVIFileAddRef", {f2}));
        v[1] = hr(g_.call("AVIFileRelease", {f2}));
        v[2] = hr(g_.call("AVIStreamAddRef", {s2}));
        v[3] = hr(g_.call("AVIStreamRelease", {s2}));
        out(fmt("avi %s refs file+%u file-%u stream+%u stream-%u", tag.c_str(), v[0], v[1], v[2], v[3]));
        {
            const GuestAddr o = g_.out_ptr(0);
            if (static_cast<i32>(hr(g_.vcall(f2, 0, {g_.guid(av::IID_IUnknown_Data1), o}))) >= 0) {
                const GuestAddr unk = static_cast<GuestAddr>(g_.rd_ptr(o));
                v[4] = hr(g_.vcall(unk, 1, {}));
                v[5] = hr(g_.vcall(unk, 2, {}));
                v[6] = hr(g_.vcall(unk, 2, {}));
                out(fmt("avi %s refs unk+%u unk-%u unk-%u", tag.c_str(), v[4], v[5], v[6]));
            }
        }
        {
            const GuestAddr o = g_.out_ptr(0);
            if (static_cast<i32>(hr(g_.vcall(f2, 0, {g_.guid(av::IID_IPersistFile_Data1), o}))) >= 0) {
                const GuestAddr pfi = static_cast<GuestAddr>(g_.rd_ptr(o));
                const GuestAddr c = g_.alloc(16);
                g_.fill(c, 16, 0xCC);
                const GuestAddr cur = g_.out_ptr(0);
                const u32 hc = hr(g_.vcall(pfi, 3, {c}));
                const u32 hd = hr(g_.vcall(pfi, 4, {}));
                const u32 hg = hr(g_.vcall(pfi, 8, {cur}));
                out(fmt("avi %s persist classid hr=0x%08x clsid=%08x isdirty=0x%08x "
                        "curfile=0x%08x %s",
                        tag.c_str(), hc, g_.rd32(c), hd, hg, g_.rd_ptr(cur) ? "set" : "NULL"));
                out(fmt("avi %s persist release=%u", tag.c_str(), hr(g_.vcall(pfi, 2, {}))));
            }
        }
        if (which != AVIFX_WAVE) {
            const GuestAddr gfo = static_cast<GuestAddr>(g_.call("AVIStreamGetFrameOpen", {s2, 0}));
            if (gfo) {
                v[0] = hr(g_.vcall(gfo, 1, {}));
                v[1] = hr(g_.vcall(gfo, 1, {}));
                v[2] = hr(g_.vcall(gfo, 2, {}));
                v[3] = hr(g_.vcall(gfo, 2, {}));
                v[4] = hr(g_.call("AVIStreamAddRef", {s2}));
                v[5] = hr(g_.call("AVIStreamRelease", {s2}));
                out(fmt("avi %s refs gf+%u gf+%u gf-%u gf-%u stream+%u stream-%u", tag.c_str(), v[0],
                        v[1], v[2], v[3], v[4], v[5]));
                out(fmt("avi %s refs gfclose=0x%08x", tag.c_str(),
                        hr(g_.call("AVIStreamGetFrameClose", {gfo}))));
            }
        }
        {
            const GuestAddr pb = g_.alloc(8), pss = g_.alloc(8), tiny = g_.alloc(16);
            const u32 st = static_cast<u32>(g_.call("AVIStreamStart", {s2}));
            g_.wr32(pb, 0x7777);
            g_.wr32(pss, 0x7777);
            u32 hz = hr(g_.call("AVIStreamRead", {s2, st, 0, tiny, 3, pb, pss}));
            out(fmt("avi %s read0 hr=0x%08x bytes=%d samples=%d", tag.c_str(), hz, g_.rdi(pb), g_.rdi(pss)));
            g_.wr32(pb, 0x7777);
            g_.wr32(pss, 0x7777);
            hz = hr(g_.call("AVIStreamRead", {s2, st, 1, tiny, 1, pb, pss}));
            out(fmt("avi %s read1into1 hr=0x%08x bytes=%d samples=%d", tag.c_str(), hz, g_.rdi(pb),
                    g_.rdi(pss)));
        }
        v[0] = hr(g_.call("AVIFileRelease", {f2}));
        v[1] = hr(g_.call("AVIStreamRelease", {s2}));
        out(fmt("avi %s refs filerelease=%u streamrelease=%u", tag.c_str(), v[0], v[1]));
    }

    void open_case(const char* label, const char* name, u32 mode, const std::array<u8, 16>* clsid) {
        const GuestAddr pp = g_.out_ptr();
        const u32 r = hr(g_.call("AVIFileOpenA", {pp, g_.str(path_of(name)), mode,
                                                  clsid ? g_.guid_bytes(*clsid) : 0}));
        const u64 pf = g_.rd_ptr(pp);
        out(fmt("avi open %s hr=0x%08x pf=%s", label, r, ptr_state(pf).c_str()));
        if (static_cast<i32>(r) >= 0 && pf && pf != kUntouched) {
            const GuestAddr fi = g_.alloc(kAVIFILEINFOA);
            g_.fill(fi, kAVIFILEINFOA, 0);
            (void)g_.call("AVIFileInfoA", {pf, fi, kAVIFILEINFOA});
            const std::vector<u8> f = g_.bytes(fi, kAVIFILEINFOA);
            const u32 rr = hr(g_.call("AVIFileRelease", {pf}));
            out(fmt("avi open %s streams=%u type=\"%s\" caps=0x%x release=%u", label, d(f, 12),
                    cstr(f, 44).c_str(), d(f, 8), rr));
        }
    }

    void run() {
        {
            const GuestAddr pp = g_.out_ptr();
            const u32 r = hr(g_.call("AVIFileOpenA", {pp, g_.str(path_of("rgb24.avi")), 0, 0}));
            out(fmt("avi preinit open hr=0x%08x pf=%s", r, ptr_state(g_.rd_ptr(pp)).c_str()));
        }
        (void)g_.call("AVIFileInit", {});
        for (int k = 0; k < AVIFX_COUNT; ++k) file_probe(k);
        open_case("missing", "missing.avi", 0, nullptr);
        open_case("missing-share", "missing.avi", 0x20, nullptr);
        open_case("rgb24.bin", "rgb24.bin", 0, nullptr);
        open_case("tonewav.avi", "tonewav.avi", 0, nullptr);
        const auto avi_clsid = av::ole_guid(av::CLSID_AVIFile_Data1);
        open_case("wav-clsid-avifile", "tone.wav", 0, &avi_clsid);
        const std::array<u8, 16> bogus = {0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56,
                                          1,    2,    3,    4,    5,    6,    7,    8};
        open_case("avi-clsid-bogus", "rgb24.avi", 0, &bogus);
        open_case("dir", "", 0, nullptr);
        {
            const GuestAddr a = g_.out_ptr(0), b = g_.out_ptr();
            const std::string p = path_of("pal8.avi");
            u32 h1 = hr(g_.call("AVIFileOpenA", {a, g_.str(p), 0x10, 0}));
            u32 h2 = hr(g_.call("AVIFileOpenA", {b, g_.str(p), 0x10, 0}));
            out(fmt("avi share exclusive h1=0x%08x h2=0x%08x b=%s", h1, h2,
                    ptr_state(g_.rd_ptr(b)).c_str()));
            if (static_cast<i32>(h2) >= 0) (void)g_.call("AVIFileRelease", {g_.rd_ptr(b)});
            g_.wrptr(b, kUntouched);
            h2 = hr(g_.call("AVIFileOpenA", {b, g_.str(p), 0x40, 0}));
            out(fmt("avi share exclusive-then-denynone h2=0x%08x", h2));
            if (static_cast<i32>(h2) >= 0) (void)g_.call("AVIFileRelease", {g_.rd_ptr(b)});
            if (static_cast<i32>(h1) >= 0) (void)g_.call("AVIFileRelease", {g_.rd_ptr(a)});
            h1 = hr(g_.call("AVIFileOpenA", {a, g_.str(p), 0x20, 0}));
            h2 = hr(g_.call("AVIFileOpenA", {b, g_.str(p), 0x20, 0}));
            out(fmt("avi share denywrite h1=0x%08x h2=0x%08x same=%d", h1, h2,
                    g_.rd_ptr(a) == g_.rd_ptr(b) ? 1 : 0));
            (void)g_.call("AVIFileRelease", {g_.rd_ptr(b)});
            (void)g_.call("AVIFileRelease", {g_.rd_ptr(a)});
        }
        {
            const GuestAddr w = g_.out_ptr(0);
            const u32 r = hr(g_.call("AVIFileOpenW", {w, g_.wstr(path_of("rgb24.avi")), 0, 0}));
            out(fmt("avi openW hr=0x%08x", r));
            if (static_cast<i32>(r) >= 0) {
                out(fmt("avi openW release=%u", hr(g_.call("AVIFileRelease", {g_.rd_ptr(w)}))));
            }
        }
        {
            const u32 types[] = {av::streamtypeVIDEO, av::streamtypeAUDIO, av::streamtypeVIDEO, 0};
            const i32 idx[] = {0, 0, 1, 1};
            for (int j = 0; j < 4; ++j) {
                const GuestAddr ps = g_.out_ptr();
                const u32 r = hr(g_.call("AVIStreamOpenFromFileA",
                                         {ps, g_.str(path_of("rgb24.avi")), types[j],
                                          static_cast<u32>(idx[j]), 0, 0}));
                const u64 sv = g_.rd_ptr(ps);
                std::string line = fmt("avi openfromfile %d hr=0x%08x s=%s", j, r, ptr_state(sv).c_str());
                if (static_cast<i32>(r) >= 0 && sv && sv != kUntouched) {
                    const GuestAddr si = g_.alloc(kAVISTREAMINFOA);
                    (void)g_.call("AVIStreamInfoA", {sv, si, kAVISTREAMINFOA});
                    const u32 rr = hr(g_.call("AVIStreamRelease", {sv}));
                    line += fmt(" type=%08x len=%u release=%u", g_.rd32(si), g_.rd32(si + 32), rr);
                }
                out(line);
            }
            const GuestAddr ps = g_.out_ptr(0);
            u32 r = hr(g_.call("AVIStreamOpenFromFileW", {ps, g_.wstr(path_of("rgb24.avi")),
                                                          av::streamtypeAUDIO, 0, 0, 0}));
            out(fmt("avi openfromfileW hr=0x%08x", r));
            if (static_cast<i32>(r) >= 0) (void)g_.call("AVIStreamRelease", {g_.rd_ptr(ps)});
            const GuestAddr pm = g_.out_ptr();
            r = hr(g_.call("AVIStreamOpenFromFileA", {pm, g_.str(path_of("missing.avi")),
                                                      av::streamtypeVIDEO, 0, 0, 0}));
            out(fmt("avi openfromfile missing hr=0x%08x s=%s", r, ptr_state(g_.rd_ptr(pm)).c_str()));
        }
        {
            const GuestAddr ps = g_.out_ptr();
            const u32 r = hr(g_.call("AVIStreamCreate", {ps, 0, 0, 0}));
            out(fmt("avi streamcreate null hr=0x%08x s=%s", r, ptr_state(g_.rd_ptr(ps)).c_str()));
        }
        {
            const GuestAddr pp = g_.out_ptr(0), ps = g_.out_ptr(0);
            const std::string p = path_of("rgb24.avi");
            if (static_cast<i32>(hr(g_.call("AVIFileOpenA", {pp, g_.str(p), 0, 0}))) >= 0 &&
                static_cast<i32>(hr(g_.call("AVIFileGetStream", {g_.rd_ptr(pp), ps, 0, 0}))) >= 0) {
                (void)g_.call("AVIFileExit", {});
                out(fmt("avi exit-live streamlen=%d",
                        static_cast<i32>(g_.call("AVIStreamLength", {g_.rd_ptr(ps)}))));
                const u32 sr = hr(g_.call("AVIStreamRelease", {g_.rd_ptr(ps)}));
                const u32 fr = hr(g_.call("AVIFileRelease", {g_.rd_ptr(pp)}));
                out(fmt("avi exit-live streamrelease=%u filerelease=%u", sr, fr));
                const GuestAddr p2 = g_.out_ptr();
                const u32 r = hr(g_.call("AVIFileOpenA", {p2, g_.str(p), 0, 0}));
                out(fmt("avi after-exit open hr=0x%08x pf=%s", r, ptr_state(g_.rd_ptr(p2)).c_str()));
            }
        }
        out("avi done");
    }

private:
    Guest& g_;
};

/// Each produced line must appear in the recording, in order.
void expect_subsequence(const std::vector<std::string>& got, const std::vector<std::string>& want,
                        const std::string& what) {
    usize w = 0;
    for (usize i = 0; i < got.size(); ++i) {
        usize j = w;
        while (j < want.size() && want[j] != got[i]) ++j;
        if (j == want.size()) {
            // Show the recording around where the line was expected.
            std::ostringstream ctx;
            for (usize k = w; k < want.size() && k < w + 6; ++k) ctx << "\n  windows: " << want[k];
            ADD_FAILURE() << what << ": line " << i << " not in the Windows recording:\n  vitreo:  "
                          << got[i] << ctx.str();
            return;
        }
        w = j + 1;
    }
}

class Avifil32Oracle : public ::testing::TestWithParam<bool> {};

TEST_P(Avifil32Oracle, ReplayMatchesWindows) {
    const bool x64 = GetParam();
    const auto expected = read_lines(expected_path(x64));
    ASSERT_GT(expected.size(), 3000u) << expected_path(x64);
    Guest g(x64);
    Replay r(g);
    r.run();
    EXPECT_GT(r.lines.size(), 2500u);
    expect_subsequence(r.lines, expected, x64 ? "x64" : "x86");
}

TEST_P(Avifil32Oracle, FixturesAreTheMeasuredFiles) {
    const auto expected = read_lines(expected_path(GetParam()));
    for (int k = 0; k < AVIFX_COUNT; ++k) {
        std::vector<u8> b(65536);
        b.resize(avifx_build(k, b.data(), b.size()));
        const std::string line =
            fmt("avi fixture %s size=%u hash=%08x", avifx_names[k], static_cast<unsigned>(b.size()), fnv(b));
        EXPECT_NE(std::find(expected.begin(), expected.end(), line), expected.end()) << line;
    }
}

TEST_P(Avifil32Oracle, ContractConstantsMatchTheHeader) {
    const auto expected = read_lines(expected_path(GetParam()));
    auto has = [&](const std::string& line) {
        return std::find(expected.begin(), expected.end(), line) != expected.end();
    };
    EXPECT_TRUE(has(fmt("avi layout AVIFILEINFOA size=%u szFileType=%u", av::kAviFileInfoASize,
                        av::kAviFileInfoTypeOffset)));
    EXPECT_TRUE(has(fmt("avi layout AVIFILEINFOW size=%u szFileType=%u", av::kAviFileInfoWSize,
                        av::kAviFileInfoTypeOffset)));
    EXPECT_TRUE(has(fmt("avi layout AVISTREAMINFOA size=%u dwCaps=%u wPriority=%u dwScale=%u "
                        "rcFrame=%u dwEditCount=%u szName=%u",
                        av::kAviStreamInfoASize, av::kAviStreamInfoCapsOffset,
                        av::kAviStreamInfoPriorityOffset, av::kAviStreamInfoScaleOffset,
                        av::kAviStreamInfoRectOffset, av::kAviStreamInfoEditCountOffset,
                        av::kAviStreamInfoNameOffset)));
    EXPECT_TRUE(has(fmt("avi layout AVISTREAMINFOW size=%u szName=%u", av::kAviStreamInfoWSize,
                        av::kAviStreamInfoNameOffset)));
    EXPECT_TRUE(has(fmt("avi const AVIERR_UNSUPPORTED=0x%08x BADFORMAT=0x%08x MEMORY=0x%08x "
                        "INTERNAL=0x%08x BADFLAGS=0x%08x BADPARAM=0x%08x BADSIZE=0x%08x",
                        av::AVIERR_UNSUPPORTED, av::AVIERR_BADFORMAT, av::AVIERR_MEMORY,
                        av::AVIERR_INTERNAL, av::AVIERR_BADFLAGS, av::AVIERR_BADPARAM,
                        av::AVIERR_BADSIZE)));
    EXPECT_TRUE(has(fmt("avi const AVIERR_BADHANDLE=0x%08x FILEREAD=0x%08x FILEWRITE=0x%08x "
                        "FILEOPEN=0x%08x COMPRESSOR=0x%08x NOCOMPRESSOR=0x%08x READONLY=0x%08x",
                        av::AVIERR_BADHANDLE, av::AVIERR_FILEREAD, av::AVIERR_FILEWRITE,
                        av::AVIERR_FILEOPEN, av::AVIERR_COMPRESSOR, av::AVIERR_NOCOMPRESSOR,
                        av::AVIERR_READONLY)));
    EXPECT_TRUE(has(fmt("avi const AVIERR_NODATA=0x%08x BUFFERTOOSMALL=0x%08x CANTCOMPRESS=0x%08x "
                        "USERABORT=0x%08x ERROR=0x%08x",
                        av::AVIERR_NODATA, av::AVIERR_BUFFERTOOSMALL, av::AVIERR_CANTCOMPRESS,
                        av::AVIERR_USERABORT, av::AVIERR_ERROR)));
    EXPECT_TRUE(has(fmt("avi const FIND_NEXT=0x%x FIND_PREV=0x%x FIND_FROM_START=0x%x FIND_KEY=0x%x "
                        "FIND_ANY=0x%x FIND_FORMAT=0x%x FIND_LENGTH=0x%x FIND_OFFSET=0x%x "
                        "FIND_SIZE=0x%x FIND_INDEX=0x%x",
                        av::FIND_NEXT, av::FIND_PREV, av::FIND_FROM_START, av::FIND_KEY,
                        av::FIND_ANY, av::FIND_FORMAT, av::FIND_LENGTH, av::FIND_OFFSET,
                        av::FIND_SIZE, av::FIND_INDEX)));
    EXPECT_TRUE(has(fmt("avi const AVISTREAMREAD_CONVENIENT=%d AVIGETFRAMEF_BESTDISPLAYFMT=%d "
                        "AVIFILEINFO_HASINDEX=0x%x MUSTUSEINDEX=0x%x ISINTERLEAVED=0x%x "
                        "TRUSTCKTYPE=0x%x WASCAPTUREFILE=0x%x COPYRIGHTED=0x%x",
                        av::AVISTREAMREAD_CONVENIENT,
                        static_cast<int>(av::AVIGETFRAMEF_BESTDISPLAYFMT),
                        av::AVIFILEINFO_HASINDEX, av::AVIFILEINFO_MUSTUSEINDEX,
                        av::AVIFILEINFO_ISINTERLEAVED, av::AVIFILEINFO_TRUSTCKTYPE,
                        av::AVIFILEINFO_WASCAPTUREFILE, av::AVIFILEINFO_COPYRIGHTED)));
    EXPECT_TRUE(has(fmt("avi const AVIFILECAPS_CANREAD=0x%x CANWRITE=0x%x ALLKEYFRAMES=0x%x "
                        "NOCOMPRESSION=0x%x AVISTREAMINFO_DISABLED=0x%x FORMATCHANGES=0x%x "
                        "AVIIF_LIST=0x%x KEYFRAME=0x%x NOTIME=0x%x",
                        av::AVIFILECAPS_CANREAD, av::AVIFILECAPS_CANWRITE,
                        av::AVIFILECAPS_ALLKEYFRAMES, av::AVIFILECAPS_NOCOMPRESSION,
                        av::AVISTREAMINFO_DISABLED, av::AVISTREAMINFO_FORMATCHANGES,
                        av::AVIIF_LIST, av::AVIIF_KEYFRAME, av::AVIIF_NOTIME)));
    EXPECT_TRUE(has(fmt("avi const streamtypeVIDEO=0x%08x AUDIO=0x%08x MIDI=0x%08x TEXT=0x%08x",
                        av::streamtypeVIDEO, av::streamtypeAUDIO, av::streamtypeMIDI,
                        av::streamtypeTEXT)));
}

TEST_P(Avifil32Oracle, ExportTableMatchesWindows) {
    const auto expected = read_lines(expected_path(GetParam()));
    std::map<u16, std::string> windows;
    for (const auto& line : expected) {
        unsigned ord = 0;
        char name[128];
        if (std::sscanf(line.c_str(), "avi export %u %127s", &ord, name) == 2) {
            windows[static_cast<u16>(ord)] = name;
        }
    }
    ASSERT_EQ(windows.size(), 76u);
    Avifil32Manager mgr;
    const DllExports exports = create_avifil32_thunks(mgr);
    std::map<u16, std::string> ours;
    for (const auto& e : exports.entries) {
        if (!e.internal) ours[e.ordinal] = e.name;
    }
    for (const auto& d : avifil32_data_exports()) ours[d.ordinal] = d.name;
    EXPECT_EQ(ours, windows);

    // x86 argument bytes from the import library (CDECL entries carry none).
    const auto lib = read_lines(std::string(VITREO_SOURCE_DIR) +
                                "/tests/oracle_data/avifil32_importlib32.txt");
    usize checked = 0;
    for (const auto& line : lib) {
        if (line.empty() || line[0] == '#') continue;
        const auto at = line.find('@');
        const std::string name = line.substr(0, at);
        const u16 bytes = at == std::string::npos ? 0 : static_cast<u16>(std::stoi(line.substr(at + 1)));
        const ExportEntry* e = find_export(exports, name);
        if (name.rfind("IID_", 0) == 0) continue;
        ASSERT_NE(e, nullptr) << name;
        EXPECT_EQ(e->stdcall_bytes, bytes) << name;
        ++checked;
    }
    EXPECT_EQ(checked, 70u);
}

TEST_P(Avifil32Oracle, TimeRoundingFollowsTheMeasuredRule) {
    // The "avi round" lines measured AVIStreamSampleToTime / TimeToSample on
    // an editable copy of each rgb24.avi stream with its scale and rate
    // replaced (dwLength stayed 3 / 300). Replay them on rgb24.avi files
    // whose strh carries those rates.
    const bool x64 = GetParam();
    const auto expected = read_lines(expected_path(x64));
    Guest g(x64);
    (void)g.call("AVIFileInit", {});
    const std::vector<u8> base = g.files()["rgb24.avi"];
    std::vector<usize> strh;
    for (usize i = 0; i + 4 <= base.size(); ++i) {
        if (std::memcmp(base.data() + i, "strh", 4) == 0) strh.push_back(i + 8);
    }
    ASSERT_EQ(strh.size(), 2u);
    usize checked = 0;
    for (const auto& line : expected) {
        int si = 0;
        unsigned scale = 0, rate = 0, set = 0;
        long v[10];
        if (std::sscanf(line.c_str(),
                        "avi round s%d scale=%u rate=%u set=0x%x s2t: %ld %ld %ld %ld %ld t2s: "
                        "%ld %ld %ld %ld %ld",
                        &si, &scale, &rate, &set, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
                        &v[7], &v[8], &v[9]) != 14) {
            continue;
        }
        std::vector<u8> file = base;
        const usize h = strh[static_cast<usize>(si)];
        for (int b = 0; b < 4; ++b) {
            file[h + 20 + static_cast<usize>(b)] = static_cast<u8>(scale >> (8 * b));
            file[h + 24 + static_cast<usize>(b)] = static_cast<u8>(rate >> (8 * b));
        }
        const std::string name = fmt("round%d_%u_%u.avi", si, scale, rate);
        g.files()[name] = file;
        const GuestAddr pp = g.out_ptr(0);
        ASSERT_EQ(g.call("AVIStreamOpenFromFileA", {pp, g.str(path_of(name.c_str())), 0,
                                                    static_cast<u32>(si), 0, 0}),
                  0u);
        const GuestAddr s = static_cast<GuestAddr>(g.rd_ptr(pp));
        const i32 samples[] = {1, 2, 3, 7, 1001};
        const i32 times[] = {1, 7, 33, 999, 1001};
        for (int k = 0; k < 5; ++k) {
            EXPECT_EQ(static_cast<i32>(g.call("AVIStreamSampleToTime", {s, static_cast<u32>(samples[k])})),
                      v[k])
                << line << " sample " << samples[k];
            EXPECT_EQ(static_cast<i32>(g.call("AVIStreamTimeToSample", {s, static_cast<u32>(times[k])})),
                      v[5 + k])
                << line << " time " << times[k];
        }
        (void)g.call("AVIStreamRelease", {s});
        ++checked;
    }
    EXPECT_EQ(checked, 16u);
}

// Compressed video goes to a Drivers32 codec through msvfw32 (Learn:
// ICLocate, ICM_DECOMPRESS_QUERY / _GET_FORMAT / _BEGIN, ICM_DECOMPRESS
// with an ICDECOMPRESS {dwFlags, lpbiInput, lpInput, lpbiOutput, lpOutput,
// ckid}; a frame that is not a key frame carries ICDECOMPRESS_NOTKEYFRAME).
// Windows' in-box codecs are not guest DLLs here, so a seeded driver stands
// in for one.
TEST_P(Avifil32Oracle, CompressedFramesDecodeThroughTheGuestCodec) {
    const bool x64 = GetParam();
    Guest g(x64);
    Msvfw32Manager icm;
    const u32 vidc = av::fourcc('v', 'i', 'd', 'c');
    const u32 test = av::fourcc('T', 'E', 'S', 'T');
    icm.add_driver(vidc, test, "testcodec.dll");
    struct Call {
        u32 msg;
        u32 flags;
        u8 first;
    };
    std::vector<Call> calls;
    icm.set_guest_call([&](GuestAddr, const u64* a, usize n, bool is64) -> u64 {
        EXPECT_EQ(n, 5u);
        EXPECT_EQ(is64, x64);
        const u32 msg = static_cast<u32>(a[2]);
        const u32 p = x64 ? 8 : 4;
        switch (msg) {
        case 1: case 2: return 1;            // DRV_LOAD / DRV_ENABLE
        case 3: return 0x5150;               // DRV_OPEN: driver id
        case 0x4000 + 10: {                  // ICM_DECOMPRESS_GET_FORMAT
            if (a[4] == 0) return 40;
            const GuestAddr out = static_cast<GuestAddr>(a[4]);
            g.fill(out, 40, 0);
            g.wr32(out, 40);
            g.wr32(out + 4, 2);
            g.wr32(out + 8, 2);
            g.wr16(out + 12, 1);
            g.wr16(out + 14, 24);
            calls.push_back({msg, 0, 0});
            return 0;
        }
        case 0x4000 + 11: case 0x4000 + 12: case 0x4000 + 14:
            calls.push_back({msg, 0, 0});
            return 0;
        case 0x4000 + 13: {                  // ICM_DECOMPRESS
            const GuestAddr icd = static_cast<GuestAddr>(a[3]);
            EXPECT_EQ(a[4], x64 ? 48u : 24u);
            const u32 flags = g.rd32(icd);
            const GuestAddr in = static_cast<GuestAddr>(g.rd_ptr(icd + 2 * p));
            const GuestAddr out_bits = static_cast<GuestAddr>(g.rd_ptr(icd + 4 * p));
            std::vector<u8> src = g.bytes(in, 16);
            for (auto& c : src) c = static_cast<u8>(c ^ 0xFF);
            for (usize i = 0; i < 16; ++i) g.fill(out_bits + i, 1, src[i]);
            calls.push_back({msg, flags, g.bytes(in, 1)[0]});
            return 0;
        }
        default: return 0;
        }
    });
    g.mgr().set_msvfw32(&icm);
    // multi.avi's video (2x2, frames stored as '00dc', frame 1 not a key
    // frame, dwStart 2) relabelled as 'TEST'-compressed.
    std::vector<u8> file = g.files()["multi.avi"];
    usize strf = 0;
    for (usize i = 0; i + 4 <= file.size(); ++i) {
        if (std::memcmp(file.data() + i, "strf", 4) == 0) {
            strf = i + 8;
            break;
        }
    }
    ASSERT_NE(strf, 0u);
    for (int b = 0; b < 4; ++b) file[strf + 16 + static_cast<usize>(b)] = static_cast<u8>(test >> (8 * b));
    g.files()["codec.avi"] = file;
    (void)g.call("AVIFileInit", {});
    const GuestAddr pp = g.out_ptr(0);
    ASSERT_EQ(g.call("AVIStreamOpenFromFileA", {pp, g.str(path_of("codec.avi")), av::streamtypeVIDEO, 0, 0, 0}), 0u);
    const GuestAddr s = static_cast<GuestAddr>(g.rd_ptr(pp));
    const GuestAddr pg = static_cast<GuestAddr>(g.call("AVIStreamGetFrameOpen", {s, 0}));
    ASSERT_NE(pg, 0u);
    ASSERT_GE(calls.size(), 3u);
    EXPECT_EQ(calls[0].msg, 0x4000u + 11); // ICLocate's query
    calls.clear();
    const GuestAddr dib = static_cast<GuestAddr>(g.call("AVIStreamGetFrame", {pg, 3}));
    ASSERT_NE(dib, 0u);
    // Decoded from the key frame (position 2) on: frame 0 bytes start at
    // 0, frame 1 at 50 (avifx_multi).
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].msg, 0x4000u + 13);
    EXPECT_EQ(calls[0].flags, 0u);
    EXPECT_EQ(calls[0].first, 0u);
    EXPECT_EQ(calls[1].flags, 0x08000000u); // ICDECOMPRESS_NOTKEYFRAME
    EXPECT_EQ(calls[1].first, 50u);
    EXPECT_EQ(g.rd32(dib + 4), 2u);
    EXPECT_EQ(g.rd32(dib + 20), 16u); // biSizeImage filled in from the geometry
    EXPECT_EQ(g.bytes(dib + 40, 1)[0], static_cast<u8>(50 ^ 0xFF));
    calls.clear();
    EXPECT_EQ(g.call("AVIStreamGetFrameClose", {pg}), 0u);
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].msg, 0x4000u + 14); // ICM_DECOMPRESS_END, then ICClose
    (void)g.call("AVIStreamRelease", {s});
    EXPECT_EQ(g.mgr().live_objects(), 0u);
}

// What Vitreo does not implement fails where it is called, with the
// documented AVIERR value and a NULL output.
TEST_P(Avifil32Oracle, UnimplementedCasesFailLoudly) {
    Guest g(GetParam());
    (void)g.call("AVIFileInit", {});
    for (u32 mode : {av::OF_WRITE, av::OF_READWRITE, av::OF_CREATE | av::OF_WRITE}) {
        const GuestAddr pp = g.out_ptr();
        EXPECT_EQ(g.call("AVIFileOpenA", {pp, g.str(path_of("rgb24.avi")), mode, 0}),
                  av::AVIERR_UNSUPPORTED);
        EXPECT_EQ(g.rd_ptr(pp), 0u);
    }
    const GuestAddr pp = g.out_ptr();
    EXPECT_EQ(g.call("CreateEditableStream", {pp, 0}), av::AVIERR_UNSUPPORTED);
    EXPECT_EQ(g.rd_ptr(pp), 0u);
    EXPECT_EQ(g.call("AVISaveVA", {g.str("x.avi"), 0, 0, 0, 0, 0}), av::AVIERR_UNSUPPORTED);
    EXPECT_EQ(g.call("AVIBuildFilterA", {g.alloc(64), 64, 0}), av::AVIERR_ERROR);
    EXPECT_EQ(g.call("AVISaveOptions", {0, 0, 0, 0, 0}), 0u);
    const GuestAddr pf = g.out_ptr();
    EXPECT_EQ(g.call("DllGetClassObject", {g.guid(av::CLSID_AVIFile_Data1),
                                           g.guid(av::IID_IUnknown_Data1), pf}),
              av::CLASS_E_CLASSNOTAVAILABLE);
    EXPECT_EQ(g.rd_ptr(pf), 0u);
    // A stream the application implements cannot feed GetFrame.
    const GuestAddr fake = g.alloc(16);
    EXPECT_EQ(g.call("AVIStreamGetFrameOpen", {fake, 0}), 0u);
    EXPECT_EQ(g.call("DllCanUnloadNow", {}), av::S_OK);
}

INSTANTIATE_TEST_SUITE_P(Bitness, Avifil32Oracle, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

} // namespace
