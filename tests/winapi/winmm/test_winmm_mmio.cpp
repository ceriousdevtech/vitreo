/// @file test_winmm_mmio.cpp
/// @brief winmm mmio* buffered I/O engine and aux* contract tests.
///
/// Every expected value comes from Microsoft Learn or from the Windows 11
/// run of tools/oracle_probe/winmm_mmio_probe (.expected = x64,
/// .x86.expected = x86; both bitnesses agree on everything asserted here
/// except the MMIOINFO layout). Each assertion names the probe line it
/// mirrors. The probe's test data is pat[k] = k * 7 + 1.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/media/winmm/winmm.hpp>
#include <winapi/media/winmm/winmm_mmio_abi.hpp>
#include <winapi/core/marshaler.hpp>
#include <windowing/contracts/wave_output.hpp>
#include <capsule/vfs/vfs.hpp>
#include <capsule/vfs/host_filesystem.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::mmio_sdk;

namespace {

class NullWaveOutput final : public windowing::WaveOutput {
public:
    u32 device_count() const noexcept override { return 0; }
    bool supports(const windowing::WaveOutputFormat&) const noexcept override { return false; }
    bool open(const windowing::WaveOutputFormat&, CompletionCallback) override { return false; }
    bool is_open() const noexcept override { return false; }
    void close() override {}
    bool submit(u64, std::span<const Byte>) override { return false; }
    void reset() override {}
    void set_paused(bool) override {}
    bool set_volume(float) override { return false; }
};

constexpr GuestAddr kData = 0x10000;   // test scratch (names, buffers, structs)
constexpr GuestAddr kStack = 0x40000;
constexpr GuestAddr kHeap = 0x100000;  // bump allocator for the mmio services
constexpr u64 kHeapSize = 0x200000;
constexpr GuestAddr kInfo = kData;          // an MMIOINFO the test owns
constexpr GuestAddr kCk = kData + 0x100;    // MMCKINFO
constexpr GuestAddr kCk2 = kData + 0x120;   // a second MMCKINFO (parent)
constexpr GuestAddr kName = kData + 0x200;  // file name
constexpr GuestAddr kName2 = kData + 0x400;
constexpr GuestAddr kBuf = kData + 0x1000;  // read/write data
constexpr GuestAddr kUserBuf = kData + 0x3000;
constexpr GuestAddr kProcA = 0x7F001000;  // fake guest MMIOPROC addresses
constexpr GuestAddr kProcB = 0x7F002000;
constexpr GuestAddr kDosProc = 0x7F00F000;
constexpr GuestAddr kMemProc = 0x7F00F100;
constexpr u32 kNeg1 = 0xFFFFFFFFu;
// mmsyscom.h; the probe prints the same numbers (risky r=5 / r=b, k_flag_bad r=a).
constexpr u32 MMSYSERR_INVALHANDLE = 5;
constexpr u32 MMSYSERR_INVALFLAG = 10;
constexpr u32 MMSYSERR_INVALPARAM = 11;

u8 pat(usize k) { return static_cast<u8>(k * 7 + 1); }
std::vector<u8> pattern(usize n, usize from = 0) {
    std::vector<u8> v(n);
    for (usize i = 0; i < n; ++i) v[i] = pat(from + i);
    return v;
}
u32 fourcc(const char (&s)[5]) {
    return static_cast<u32>(static_cast<u8>(s[0])) | static_cast<u32>(static_cast<u8>(s[1])) << 8 |
           static_cast<u32>(static_cast<u8>(s[2])) << 16 | static_cast<u32>(static_cast<u8>(s[3])) << 24;
}
std::vector<u8> unhex(std::string_view h) {
    std::vector<u8> out;
    for (usize i = 0; i + 1 < h.size(); i += 2) out.push_back(static_cast<u8>(std::stoi(std::string(h.substr(i, 2)), nullptr, 16)));
    return out;
}

/// MMIOINFO read back from guest memory.
struct GInfo {
    u32 flags, fcc;
    u64 proc;
    u32 err;
    u64 task;
    i32 cch;
    u64 buf, next, endr, endw;
    i32 bufoff, diskoff;
    u32 adw[3];
    u32 res1, res2;
    u64 hmmio;
    i32 rnext() const { return static_cast<i32>(static_cast<i64>(next - buf)); }
    i32 rendr() const { return static_cast<i32>(static_cast<i64>(endr - buf)); }
    i32 rendw() const { return static_cast<i32>(static_cast<i64>(endw - buf)); }
};

/// One message a fake MMIOPROC received (probe p_log fields).
struct ProcCall {
    u32 msg;
    u64 l1, l2;
    i32 disk;
    u32 a0;
    u32 flags;
    bool buf;
    bool info_is_h;
    std::string name;
};

class MmioHarness {
public:
    explicit MmioHarness(bool x64) : x64_(x64), manager(memory, wave, vfs) {
        EXPECT_TRUE(memory.map_region(kData, 0x30000, MemoryProtection::ReadWrite).is_ok());
        EXPECT_TRUE(memory.map_region(kStack, 0x10000, MemoryProtection::ReadWrite).is_ok());
        EXPECT_TRUE(memory.map_region(kHeap, kHeapSize, MemoryProtection::ReadWrite).is_ok());
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        static std::atomic<int> serial{0};
        root = std::filesystem::temp_directory_path() /
               ("vitreo_mmio_" + std::to_string(::getpid()) + "_" + std::to_string(serial++));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        EXPECT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
        vfs.set_cwd("C:\\");

        WinmmManager::MmioServices s;
        s.alloc = [this](u64 size) -> GuestAddr {
            const u64 aligned = (size + 15) & ~15ull;
            if (heap_next + aligned > kHeap + kHeapSize) return 0;
            const GuestAddr a = heap_next;
            heap_next += aligned == 0 ? 16 : aligned;
            std::vector<Byte> zero(static_cast<usize>(aligned), 0);
            if (!zero.empty()) (void)memory.write_bytes(a, zero.data(), zero.size());
            sizes[a] = size;
            ++live_blocks;
            return a;
        };
        s.realloc = [this, alloc = s.alloc](GuestAddr block, u64 size) -> GuestAddr {
            const GuestAddr n = alloc(size);
            if (n == 0) return 0;
            const u64 old = sizes[block];
            std::vector<Byte> tmp(static_cast<usize>(std::min(old, size)));
            (void)memory.read_bytes(block, tmp.data(), tmp.size());
            (void)memory.write_bytes(n, tmp.data(), tmp.size());
            sizes.erase(block);
            --live_blocks;
            return n;
        };
        s.free = [this](GuestAddr block) {
            if (sizes.erase(block) != 0) --live_blocks;
        };
        s.adopt_file = [this](u32 vfs_handle, std::string_view, u32 access) -> u32 {
            const u32 h = next_handle;
            next_handle += 4;
            handles[h] = vfs_handle;
            access_of[h] = access;
            return h;
        };
        s.vfs_handle_for = [this](u32 h) -> u32 {
            auto it = handles.find(h);
            return it == handles.end() ? 0u : it->second;
        };
        s.close_file = [this](u32 h) {
            auto it = handles.find(h);
            if (it == handles.end()) return;
            (void)vfs.close(it->second);
            handles.erase(it);
        };
        s.call = [this](GuestAddr proc, const u64* args, usize count, bool is64) -> u64 {
            EXPECT_EQ(count, 4u);
            EXPECT_EQ(is64, x64_);
            auto it = procs.find(proc);
            EXPECT_NE(it, procs.end());
            if (it == procs.end()) return ~0ull;
            return it->second(args);
        };
        manager.set_mmio_services(std::move(s));
        manager.set_mmio_builtin_procs(kDosProc, kMemProc);
        exports = create_winmm_thunks(manager);
    }
    ~MmioHarness() {
        for (auto& [h, f] : handles) (void)vfs.close(f);
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    u64 call(std::string_view name, std::initializer_list<u64> args) {
        const auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                                     [name](const ExportEntry& e) { return e.name == name; });
        EXPECT_NE(it, exports.entries.end()) << name;
        if (it == exports.entries.end()) return ~0ull;
        const GuestAddr sp = kStack + 0x8000;
        cpu.set_sp(sp);
        usize i = 0;
        for (u64 a : args) {
            if (x64_) {
                switch (i) {
                case 0: cpu.gpr.rcx = a; break;
                case 1: cpu.gpr.rdx = a; break;
                case 2: cpu.gpr.r8 = a; break;
                case 3: cpu.gpr.r9 = a; break;
                default: EXPECT_TRUE(memory.write_u64(sp + 0x28 + (i - 4) * 8, a).is_ok());
                }
            } else {
                EXPECT_TRUE(memory.write_u32(sp + 4 + i * 4, static_cast<u32>(a)).is_ok());
            }
            ++i;
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        it->handler(m);
        return x64_ ? cpu.gpr.rax : (cpu.gpr.rax & 0xFFFFFFFFull);
    }
    /// The low 32 bits: MMRESULT / LONG results.
    u32 call32(std::string_view name, std::initializer_list<u64> args) {
        return static_cast<u32>(call(name, args));
    }

    // ---- guest data ----
    const MmioInfoLayout& L() const { return mmioinfo_layout(x64_); }
    u32 u32_at(GuestAddr a) { return memory.read_u32(a).value_or(0xDEADBEEF); }
    u8 u8_at(GuestAddr a) { return memory.read_u8(a).value_or(0xEE); }
    u64 ptr_at(GuestAddr a) { return x64_ ? memory.read_u64(a).value_or(0) : u32_at(a); }
    void put_ptr(GuestAddr a, u64 v) {
        if (x64_) ASSERT_TRUE(memory.write_u64(a, v).is_ok());
        else ASSERT_TRUE(memory.write_u32(a, static_cast<u32>(v)).is_ok());
    }
    void put_str(GuestAddr a, std::string_view s) {
        if (!s.empty()) ASSERT_TRUE(memory.write_bytes(a, reinterpret_cast<const Byte*>(s.data()), s.size()).is_ok());
        ASSERT_TRUE(memory.write_u8(a + s.size(), 0).is_ok());
    }
    void put_wstr(GuestAddr a, std::u16string_view s) {
        for (usize i = 0; i < s.size(); ++i) ASSERT_TRUE(memory.write_u16(a + 2 * i, s[i]).is_ok());
        ASSERT_TRUE(memory.write_u16(a + 2 * s.size(), 0).is_ok());
    }
    void put_bytes(GuestAddr a, const std::vector<u8>& b) {
        ASSERT_TRUE(memory.write_bytes(a, reinterpret_cast<const Byte*>(b.data()), b.size()).is_ok());
    }
    std::vector<u8> bytes_at(GuestAddr a, usize n) {
        std::vector<u8> v(n);
        EXPECT_TRUE(memory.read_bytes(a, reinterpret_cast<Byte*>(v.data()), n).is_ok());
        return v;
    }
    std::string str_at(GuestAddr a) {
        std::string s;
        for (u8 c; (c = u8_at(a)) != 0; ++a) s.push_back(static_cast<char>(c));
        return s;
    }
    void zero(GuestAddr a, usize n, u8 fill = 0) {
        std::vector<Byte> z(n, fill);
        ASSERT_TRUE(memory.write_bytes(a, z.data(), n).is_ok());
    }
    /// A zeroed MMIOINFO at @p a.
    void clear_info(GuestAddr a = kInfo) { zero(a, L().size); }
    void set_info_u32(u32 off, u32 v, GuestAddr a = kInfo) { ASSERT_TRUE(memory.write_u32(a + off, v).is_ok()); }
    GInfo info_at(GuestAddr a) {
        const MmioInfoLayout& l = L();
        GInfo g{};
        g.flags = u32_at(a + l.off_dwFlags);
        g.fcc = u32_at(a + l.off_fccIOProc);
        g.proc = ptr_at(a + l.off_pIOProc);
        g.err = u32_at(a + l.off_wErrorRet);
        g.task = ptr_at(a + l.off_htask);
        g.cch = static_cast<i32>(u32_at(a + l.off_cchBuffer));
        g.buf = ptr_at(a + l.off_pchBuffer);
        g.next = ptr_at(a + l.off_pchNext);
        g.endr = ptr_at(a + l.off_pchEndRead);
        g.endw = ptr_at(a + l.off_pchEndWrite);
        g.bufoff = static_cast<i32>(u32_at(a + l.off_lBufOffset));
        g.diskoff = static_cast<i32>(u32_at(a + l.off_lDiskOffset));
        for (u32 i = 0; i < 3; ++i) g.adw[i] = u32_at(a + l.off_adwInfo + 4 * i);
        g.res1 = u32_at(a + l.off_dwReserved1);
        g.res2 = u32_at(a + l.off_dwReserved2);
        g.hmmio = ptr_at(a + l.off_hmmio);
        return g;
    }
    /// mmioGetInfo into kInfo + 0x80 and return it.
    GInfo get(u64 h) {
        const GuestAddr at = kData + 0x80;
        EXPECT_EQ(call32("mmioGetInfo", {h, at, 0}), 0u);
        return info_at(at);
    }
    /// The MMCKINFO at @p a: ckid, cksize, fccType, dwDataOffset, dwFlags.
    std::array<u32, 5> ck_at(GuestAddr a) {
        return {u32_at(a), u32_at(a + 4), u32_at(a + 8), u32_at(a + 12), u32_at(a + 16)};
    }
    void set_ck(GuestAddr a, u32 id, u32 size, u32 type, u32 data = 0, u32 flags = 0) {
        for (u32 i = 0; i < 5; ++i) {
            const u32 v = i == 0 ? id : i == 1 ? size : i == 2 ? type : i == 3 ? data : flags;
            ASSERT_TRUE(memory.write_u32(a + 4 * i, v).is_ok());
        }
    }

    // ---- host files ----
    void write_file(const std::string& name, const std::vector<u8>& b) {
        std::ofstream f(root / name, std::ios::binary);
        f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    }
    std::vector<u8> read_file(const std::string& name) {
        std::ifstream f(root / name, std::ios::binary);
        return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    bool exists(const std::string& name) { return std::filesystem::exists(root / name); }

    /// mmioOpenA(name, kInfo (when @p with_info), flags) with kInfo prepared
    /// by the caller.
    u64 open(std::string_view name, u32 flags, bool with_info = false) {
        put_str(kName, name);
        return call("mmioOpenA", {kName, with_info ? kInfo : 0, flags});
    }

    /// The probe's tproc: a 64-byte virtual file of k*3, logging every
    /// message (winmm_mmio_probe.c tproc).
    void install_tproc(GuestAddr addr) {
        procs[addr] = [this](const u64* a) -> u64 {
            const GuestAddr info = static_cast<GuestAddr>(a[0]);
            const u32 msg = static_cast<u32>(a[1]);
            const MmioInfoLayout& l = L();
            ProcCall c{};
            c.msg = msg;
            c.l1 = a[2];
            c.l2 = a[3];
            c.disk = static_cast<i32>(u32_at(info + l.off_lDiskOffset));
            c.a0 = u32_at(info + l.off_adwInfo);
            c.flags = u32_at(info + l.off_dwFlags);
            c.buf = ptr_at(info + l.off_pchBuffer) != 0;
            c.info_is_h = ptr_at(info + l.off_hmmio) == info;
            if ((msg == MMIOM_OPEN || msg == MMIOM_RENAME) && c.l1 != 0) c.name = str_at(static_cast<GuestAddr>(c.l1));
            log.push_back(c);
            auto disk = [&]() { return static_cast<i32>(u32_at(info + l.off_lDiskOffset)); };
            auto set_disk = [&](i32 v) { (void)memory.write_u32(info + l.off_lDiskOffset, static_cast<u32>(v)); };
            switch (msg) {
            case MMIOM_OPEN:
                set_disk(0);
                (void)memory.write_u32(info + l.off_adwInfo, 0x55);
                return 0;
            case MMIOM_CLOSE: return 0;
            case MMIOM_READ: {
                i32 n = static_cast<i32>(c.l2);
                if (disk() + n > vsize) n = vsize - disk();
                if (n < 0) n = 0;
                (void)memory.write_bytes(static_cast<GuestAddr>(c.l1),
                                         reinterpret_cast<const Byte*>(vfile.data() + disk()), static_cast<usize>(n));
                set_disk(disk() + n);
                return static_cast<u64>(n);
            }
            case MMIOM_WRITE:
            case MMIOM_WRITEFLUSH: {
                i32 n = static_cast<i32>(c.l2);
                if (disk() + n > static_cast<i32>(vfile.size())) n = static_cast<i32>(vfile.size()) - disk();
                (void)memory.read_bytes(static_cast<GuestAddr>(c.l1),
                                        reinterpret_cast<Byte*>(vfile.data() + disk()), static_cast<usize>(n));
                set_disk(disk() + n);
                vsize = std::max(vsize, disk());
                return static_cast<u64>(n);
            }
            case MMIOM_SEEK: {
                const i32 off = static_cast<i32>(c.l1);
                const i32 base = c.l2 == SEEK_SET_VALUE ? 0 : c.l2 == SEEK_CUR_VALUE ? disk() : vsize;
                set_disk(base + off);
                return static_cast<u64>(static_cast<i64>(disk()));
            }
            case MMIOM_RENAME: return 0;
            default: return 0x1234;
            }
        };
    }

    bool x64_;
    GuestMemory memory;
    capsule::Vfs vfs;
    NullWaveOutput wave;
    WinmmManager manager;
    DllExports exports;
    cpu::CpuState cpu;
    std::filesystem::path root;
    GuestAddr heap_next{kHeap};
    std::map<GuestAddr, u64> sizes;
    int live_blocks{0};
    u32 next_handle{0x200};
    std::map<u32, u32> handles;
    std::map<u32, u32> access_of;
    std::map<GuestAddr, std::function<u64(const u64*)>> procs;
    std::vector<ProcCall> log;
    std::vector<u8> vfile = [] { std::vector<u8> v(96); for (usize k = 0; k < 96; ++k) v[k] = static_cast<u8>(k * 3); return v; }();
    i32 vsize{64};
};

class WinmmMmioTest : public ::testing::TestWithParam<bool> {};
INSTANTIATE_TEST_SUITE_P(Bitness, WinmmMmioTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

} // namespace

// =============================================================================
// Layouts: probe "layout" lines (x86 / x64); the SDK audit checks the same.
// =============================================================================
TEST(WinmmMmioAbiTest, LayoutsMatchProbe) {
    // layout mmioinfo=48 ... hmmio=44 (x86)
    const MmioInfoLayout& a = mmioinfo_layout_32;
    EXPECT_EQ(a.size, 0x48u);
    EXPECT_EQ(a.off_wErrorRet, 0xcu);
    EXPECT_EQ(a.off_cchBuffer, 0x14u);
    EXPECT_EQ(a.off_pchBuffer, 0x18u);
    EXPECT_EQ(a.off_pchNext, 0x1cu);
    EXPECT_EQ(a.off_pchEndRead, 0x20u);
    EXPECT_EQ(a.off_pchEndWrite, 0x24u);
    EXPECT_EQ(a.off_lBufOffset, 0x28u);
    EXPECT_EQ(a.off_lDiskOffset, 0x2cu);
    EXPECT_EQ(a.off_adwInfo, 0x30u);
    EXPECT_EQ(a.off_hmmio, 0x44u);
    // layout mmioinfo=64 ... (x64, pshpack1: pointers unaligned)
    const MmioInfoLayout& b = mmioinfo_layout_64;
    EXPECT_EQ(b.size, 0x64u);
    EXPECT_EQ(b.off_pIOProc, 0x8u);
    EXPECT_EQ(b.off_wErrorRet, 0x10u);
    EXPECT_EQ(b.off_htask, 0x14u);
    EXPECT_EQ(b.off_cchBuffer, 0x1cu);
    EXPECT_EQ(b.off_pchBuffer, 0x20u);
    EXPECT_EQ(b.off_pchNext, 0x28u);
    EXPECT_EQ(b.off_pchEndRead, 0x30u);
    EXPECT_EQ(b.off_pchEndWrite, 0x38u);
    EXPECT_EQ(b.off_lBufOffset, 0x40u);
    EXPECT_EQ(b.off_lDiskOffset, 0x44u);
    EXPECT_EQ(b.off_adwInfo, 0x48u);
    EXPECT_EQ(b.off_dwReserved1, 0x54u);
    EXPECT_EQ(b.off_dwReserved2, 0x58u);
    EXPECT_EQ(b.off_hmmio, 0x5cu);
    // layout mmckinfo=14 auxcapsa=30 auxcapsw=50
    EXPECT_EQ(mmckinfo_layout.size, 0x14u);
    EXPECT_EQ(auxcaps_layout.size_a, 0x30u);
    EXPECT_EQ(auxcaps_layout.size_w, 0x50u);
}

// =============================================================================
// aux*: no auxiliary device (probe aux_*).
// =============================================================================
TEST_P(WinmmMmioTest, AuxReportsNoDevice) {
    MmioHarness h(GetParam());
    EXPECT_EQ(h.call32("auxGetNumDevs", {}), 0u);                        // aux_numdevs
    for (u64 id : {0ull, h.x64_ ? 0xFFFFFFFFull : 0xFFFFFFFFull, 5ull}) {
        h.zero(kBuf, 0x50, 0xAB);
        EXPECT_EQ(h.call32("auxGetDevCapsA", {id, kBuf, 0x30}), 2u);       // aux_capsa r=2
        EXPECT_EQ(h.call32("auxGetDevCapsW", {id, kBuf, 0x50}), 2u);       // aux_capsw r=2
        EXPECT_EQ(h.u8_at(kBuf), 0xABu);                                    // touched=0
        ASSERT_TRUE(h.memory.write_u32(kBuf, 0xAAAAAAAA).is_ok());
        EXPECT_EQ(h.call32("auxGetVolume", {id, kBuf}), 2u);               // aux_getvol
        EXPECT_EQ(h.u32_at(kBuf), 0xAAAAAAAAu);
        EXPECT_EQ(h.call32("auxSetVolume", {id, 0x80008000}), 2u);         // aux_setvol
        EXPECT_EQ(h.call32("auxOutMessage", {id, 0x4000, 0, 0}), 2u);      // aux_msg
    }
    EXPECT_EQ(h.call32("auxGetDevCapsA", {0, kBuf, 0}), 0u);   // aux_capsa_cb0
    EXPECT_EQ(h.call32("auxGetDevCapsA", {0, 0, 0}), 0u);      // aux_capsa_null_cb0
    EXPECT_EQ(h.call32("auxGetDevCapsA", {5, kBuf, 0}), 0u);   // aux_capsa_id5_cb0
    EXPECT_EQ(h.call32("auxGetDevCapsA", {0, kBuf, 4}), 2u);   // aux_capsa_cb4
    EXPECT_EQ(h.call32("auxGetDevCapsA", {0, kBuf, 4096}), 2u);  // aux_capsa_big
    EXPECT_EQ(h.call32("auxGetDevCapsW", {AUX_MAPPER, 0, 0x50}), 0xbu);  // aux_capsw_null_map
    EXPECT_EQ(h.call32("auxGetDevCapsA", {0, 0, 0x30}), 0xbu);           // risky 0x1b
    EXPECT_EQ(h.call32("auxGetVolume", {AUX_MAPPER, 0}), 0xbu);          // aux_getvol_null_map
    EXPECT_EQ(h.call32("auxGetVolume", {0, 0}), 0xbu);                   // risky 0x1a
}

// =============================================================================
// mmioStringToFOURCC (probe fourcc_*).
// =============================================================================
TEST_P(WinmmMmioTest, StringToFourcc) {
    MmioHarness h(GetParam());
    const auto a = [&](std::string_view s, u32 f) { h.put_str(kName, s); return h.call32("mmioStringToFOURCCA", {kName, f}); };
    const auto w = [&](std::u16string_view s, u32 f) { h.put_wstr(kName, s); return h.call32("mmioStringToFOURCCW", {kName, f}); };
    EXPECT_EQ(a("abc", 0), 0x20636261u);
    EXPECT_EQ(a("wave", MMIO_TOUPPER), 0x45564157u);
    EXPECT_EQ(a("wave", 0), 0x65766177u);
    EXPECT_EQ(a("toolong", 0), 0x6c6f6f74u);
    EXPECT_EQ(a("", 0), 0x20202020u);
    EXPECT_EQ(a("WaV1", MMIO_TOUPPER), 0x31564157u);
    EXPECT_EQ(a("\xe9z", MMIO_TOUPPER), 0x20205ac9u);  // fourcc_hi_up
    EXPECT_EQ(a("ab", 2), 0x20206261u);                 // fourcc_flag2
    EXPECT_EQ(w(u"wave", MMIO_TOUPPER), 0x45564157u);
    EXPECT_EQ(w(u"abcdef", 0), 0x64636261u);
    EXPECT_EQ(w(u"\u00e9b\u0416", MMIO_TOUPPER), 0x203f42c9u);  // fourcc_w_hi
    EXPECT_EQ(w(u"", 0), 0x20202020u);
    EXPECT_EQ(h.call32("mmioStringToFOURCCA", {0, 0}), 0u);      // risky 0x19
}

// =============================================================================
// Create and write unbuffered (probe c_*).
// =============================================================================
TEST_P(WinmmMmioTest, CreateWriteUnbuffered) {
    MmioHarness h(GetParam());
    h.clear_info();
    const u64 f = h.open("mmio_p1.dat", MMIO_CREATE | MMIO_WRITE, true);
    ASSERT_NE(f, 0u);
    // c_open_info: only wErrorRet is written to the caller's MMIOINFO.
    const GInfo ci = h.info_at(kInfo);
    EXPECT_EQ(ci.err, 0u);
    EXPECT_EQ(ci.hmmio, 0u);
    EXPECT_EQ(ci.fcc, 0u);
    // c_get flags=1001 fcc=DOS proc=1 cch=0 buf=0 ... a0_nz=1 hmmio_eq=1
    GInfo g = h.get(f);
    EXPECT_EQ(g.flags, 0x1001u);
    EXPECT_EQ(g.fcc, FOURCC_DOS);
    EXPECT_EQ(g.proc, kDosProc);
    EXPECT_EQ(g.cch, 0);
    EXPECT_EQ(g.buf, 0u);
    EXPECT_EQ(g.endw, 0u);
    EXPECT_EQ(g.bufoff, 0);
    EXPECT_EQ(g.diskoff, 0);
    EXPECT_NE(g.adw[0], 0u);
    EXPECT_EQ(g.task, 0u);
    EXPECT_EQ(g.hmmio, f);
    EXPECT_EQ(h.access_of[g.adw[0]], GENERIC_WRITE);
    h.put_bytes(kBuf, pattern(100));
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf, 100}), 100u);  // c_write
    g = h.get(f);
    EXPECT_EQ(g.bufoff, 100);  // c_get2
    EXPECT_EQ(g.diskoff, 100);
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_CUR_VALUE}), 100u);
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_END_VALUE}), 100u);
    EXPECT_EQ(h.call32("mmioSeek", {f, 50, SEEK_SET_VALUE}), 50u);
    g = h.get(f);
    EXPECT_EQ(g.bufoff, 50);  // c_get3: lazy, the disk stays at 100
    EXPECT_EQ(g.diskoff, 100);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 4}), kNeg1);  // c_read_wo
    EXPECT_EQ(h.call32("mmioAdvance", {f, kData + 0x80, MMIO_READ}), MMIOERR_UNBUFFERED);
    EXPECT_EQ(h.call32("mmioFlush", {f, 0}), 0u);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    EXPECT_EQ(h.read_file("mmio_p1.dat"), pattern(100));  // c_file size=64
    EXPECT_TRUE(h.handles.empty());
    EXPECT_EQ(h.live_blocks, 0);
}

// =============================================================================
// Buffered read, seeks, and the direct-access (WAVEREAD) loop (probe r_*).
// =============================================================================
TEST_P(WinmmMmioTest, BufferedReadSeekAndDirectAccess) {
    MmioHarness h(GetParam());
    h.write_file("mmio_p1.dat", pattern(100));
    const u64 f = h.open("mmio_p1.dat", MMIO_READ | MMIO_ALLOCBUF);
    ASSERT_NE(f, 0u);
    GInfo g = h.get(f);
    // r_get0 flags=10000 cch=2000 next=0 endr=0 endw=2000
    EXPECT_EQ(g.flags, MMIO_ALLOCBUF);
    EXPECT_EQ(g.cch, 0x2000);
    EXPECT_NE(g.buf, 0u);
    EXPECT_EQ(g.rnext(), 0);
    EXPECT_EQ(g.rendr(), 0);
    EXPECT_EQ(g.rendw(), 0x2000);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 10}), 10u);
    EXPECT_EQ(h.bytes_at(kBuf, 10), pattern(10));
    g = h.get(f);  // r_get1 next=a endr=64 bufoff=0 diskoff=64
    EXPECT_EQ(g.rnext(), 10);
    EXPECT_EQ(g.rendr(), 100);
    EXPECT_EQ(g.diskoff, 100);
    EXPECT_EQ(h.call32("mmioSeek", {f, 5, SEEK_SET_VALUE}), 5u);
    EXPECT_EQ(h.get(f).rnext(), 5);  // r_get2
    EXPECT_EQ(h.call32("mmioSeek", {f, 3, SEEK_CUR_VALUE}), 8u);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 1}), 1u);
    EXPECT_EQ(h.u8_at(kBuf), 0x39u);  // r_read1 b=39
    // SEEK_END counts back: -4 from the end of 100 is 104 (r_seek_endm4).
    EXPECT_EQ(h.call32("mmioSeek", {f, static_cast<u32>(-4), SEEK_END_VALUE}), 0x68u);
    g = h.get(f);  // r_get3 next=0 endr=0 bufoff=68 diskoff=64
    EXPECT_EQ(g.rnext(), 0);
    EXPECT_EQ(g.rendr(), 0);
    EXPECT_EQ(g.bufoff, 0x68);
    EXPECT_EQ(g.diskoff, 0x64);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 10}), 0u);  // r_read10_eof
    EXPECT_EQ(h.get(f).diskoff, 0x68);                    // r_get4
    EXPECT_EQ(h.call32("mmioSeek", {f, 200, SEEK_SET_VALUE}), 200u);
    EXPECT_EQ(h.get(f).bufoff, 200);                      // r_get5
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 10}), 0u);   // r_read_past
    EXPECT_EQ(h.call32("mmioSeek", {f, kNeg1, SEEK_SET_VALUE}), kNeg1);  // r_seek_neg
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_CUR_VALUE}), kNeg1);
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, 3}), 0u);                      // r_seek_origin3
    EXPECT_EQ(h.call32("mmioSeek", {f, 10, SEEK_SET_VALUE}), 10u);
    EXPECT_EQ(h.call32("mmioSeek", {f, static_cast<u32>(-20), SEEK_CUR_VALUE}), 0xfffffff6u);
    EXPECT_EQ(h.call32("mmioSeek", {f, 9, SEEK_SET_VALUE}), 9u);
    EXPECT_EQ(h.call32("mmioSeek", {f, 7, 3}), 0u);                      // seek_origin3_7
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_CUR_VALUE}), 0u);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 0}), 0u);                   // r_read_zero
    EXPECT_EQ(h.call32("mmioGetInfo", {f, kData + 0x80, 1}), 0u);        // r_getinfo_flag1

    // The WAVEREAD loop: pchNext == pchEndRead -> mmioAdvance(MMIO_READ).
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_SET_VALUE}), 0u);
    ASSERT_EQ(h.call32("mmioGetInfo", {f, kInfo, 0}), 0u);
    std::vector<u8> got;
    int advs = 0;
    for (int k = 0; k < 100; ++k) {
        GInfo c = h.info_at(kInfo);
        if (c.next == c.endr) {
            ASSERT_EQ(h.call32("mmioAdvance", {f, kInfo, MMIO_READ}), 0u);
            ++advs;
            c = h.info_at(kInfo);
            if (advs == 1) {  // r_direct_adv1 next=0 endr=64 bufoff=0 diskoff=64
                EXPECT_EQ(c.rnext(), 0);
                EXPECT_EQ(c.rendr(), 100);
                EXPECT_EQ(c.bufoff, 0);
                EXPECT_EQ(c.diskoff, 100);
                EXPECT_EQ(c.hmmio, f);
            }
            if (c.next == c.endr) break;
        }
        got.push_back(h.u8_at(static_cast<GuestAddr>(c.next)));
        h.put_ptr(kInfo + h.L().off_pchNext, c.next + 1);
    }
    EXPECT_EQ(got, pattern(100));  // r_direct ok=1 advs=1
    EXPECT_EQ(advs, 1);
    // r_adv_eof r=0: at the end the buffer comes back empty at 100.
    EXPECT_EQ(h.call32("mmioAdvance", {f, kInfo, MMIO_READ}), 0u);
    GInfo e = h.info_at(kInfo);
    EXPECT_EQ(e.rnext(), 0);
    EXPECT_EQ(e.rendr(), 0);
    EXPECT_EQ(e.bufoff, 100);
    EXPECT_EQ(h.call32("mmioSetInfo", {f, kInfo, 0}), 0u);
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_CUR_VALUE}), 100u);  // r_seek_cur_after_direct

    // mmioSetInfo takes pchNext: +7 then a read returns byte 7 (r_setinfo7).
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_SET_VALUE}), 0u);
    ASSERT_EQ(h.call32("mmioGetInfo", {f, kInfo, 0}), 0u);
    ASSERT_EQ(h.call32("mmioAdvance", {f, kInfo, MMIO_READ}), 0u);
    h.put_ptr(kInfo + h.L().off_pchNext, h.info_at(kInfo).next + 7);
    EXPECT_EQ(h.call32("mmioSetInfo", {f, kInfo, 0}), 0u);
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_CUR_VALUE}), 7u);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 1}), 1u);
    EXPECT_EQ(h.u8_at(kBuf), 0x32u);  // r_read_after7 b=32
    EXPECT_EQ(h.call32("mmioFlush", {f, 0}), 0u);
    EXPECT_EQ(h.call32("mmioFlush", {f, MMIO_EMPTYBUF}), 0u);
    g = h.get(f);  // r_get_after_empty next=0 endr=0 bufoff=8 diskoff=64
    EXPECT_EQ(g.rnext(), 0);
    EXPECT_EQ(g.rendr(), 0);
    EXPECT_EQ(g.bufoff, 8);
    EXPECT_EQ(g.diskoff, 100);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    EXPECT_EQ(h.live_blocks, 0);
}

// =============================================================================
// A 16-byte buffer: reads larger than the buffer bypass it (probe s_*), and
// SEEK_END with a positive offset (se_*).
// =============================================================================
TEST_P(WinmmMmioTest, SmallBufferReadsAndSeekEnd) {
    MmioHarness h(GetParam());
    h.write_file("mmio_p1.dat", pattern(100));
    h.clear_info();
    h.set_info_u32(h.L().off_cchBuffer, 16);
    const u64 f = h.open("mmio_p1.dat", MMIO_READ | MMIO_ALLOCBUF, true);
    ASSERT_NE(f, 0u);
    GInfo g = h.get(f);
    EXPECT_EQ(g.cch, 16);  // s_get0
    EXPECT_EQ(g.rendw(), 16);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 20}), 20u);
    EXPECT_EQ(h.bytes_at(kBuf, 20), pattern(20));
    g = h.get(f);  // s_get1 next=0 endr=0 bufoff=14 diskoff=14
    EXPECT_EQ(g.rendr(), 0);
    EXPECT_EQ(g.bufoff, 20);
    EXPECT_EQ(g.diskoff, 20);
    EXPECT_EQ(h.call32("mmioSeek", {f, 3, SEEK_SET_VALUE}), 3u);
    EXPECT_EQ(h.get(f).bufoff, 3);  // s_get2
    EXPECT_EQ(h.call32("mmioSeek", {f, 18, SEEK_SET_VALUE}), 18u);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 1}), 1u);
    EXPECT_EQ(h.u8_at(kBuf), 0x7fu);  // s_read1
    g = h.get(f);  // s_get4 next=1 endr=10 bufoff=12 diskoff=22
    EXPECT_EQ(g.rnext(), 1);
    EXPECT_EQ(g.rendr(), 16);
    EXPECT_EQ(g.bufoff, 18);
    EXPECT_EQ(g.diskoff, 34);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 40}), 40u);
    EXPECT_EQ(h.bytes_at(kBuf, 40), pattern(40, 19));
    g = h.get(f);  // s_get5 next=0 endr=0 bufoff=3b diskoff=3b
    EXPECT_EQ(g.rendr(), 0);
    EXPECT_EQ(g.bufoff, 59);
    EXPECT_EQ(g.diskoff, 59);
    ASSERT_EQ(h.call32("mmioGetInfo", {f, kInfo, 0}), 0u);
    EXPECT_EQ(h.call32("mmioAdvance", {f, kInfo, MMIO_READ}), 0u);
    g = h.info_at(kInfo);  // s_adv_info next=0 endr=10 bufoff=3b diskoff=4b
    EXPECT_EQ(g.rendr(), 16);
    EXPECT_EQ(g.bufoff, 59);
    EXPECT_EQ(g.diskoff, 75);
    EXPECT_EQ(h.call32("mmioSetInfo", {f, kInfo, 0}), 0u);
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_CUR_VALUE}), 59u);
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_SET_VALUE}), 0u);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 16}), 16u);  // s_read16: through the buffer
    g = h.get(f);
    EXPECT_EQ(g.rnext(), 16);
    EXPECT_EQ(g.rendr(), 16);
    EXPECT_EQ(g.bufoff, 0);
    EXPECT_EQ(g.diskoff, 16);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 15}), 15u);  // s_read15
    g = h.get(f);
    EXPECT_EQ(g.rnext(), 15);
    EXPECT_EQ(g.bufoff, 16);
    EXPECT_EQ(g.diskoff, 32);
    // Writing a read-only file copies into the buffer until it must be
    // written (s_write20 -1, s_getw20 next=10 endr=10 dirty); the close then
    // fails and the file stays open (s_close 106, ro_write_after_close).
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_SET_VALUE}), 0u);
    h.put_bytes(kBuf, pattern(20));
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf, 20}), kNeg1);
    g = h.get(f);
    EXPECT_EQ(g.flags, MMIO_ALLOCBUF | MMIO_DIRTY);
    EXPECT_EQ(g.rnext(), 16);
    EXPECT_EQ(g.rendr(), 16);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), MMIOERR_CANNOTWRITE);
    EXPECT_EQ(h.call32("mmioGetInfo", {f, kData + 0x80, 0}), 0u);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), MMIOERR_CANNOTWRITE);
    EXPECT_EQ(h.read_file("mmio_p1.dat"), pattern(100));

    // SEEK_END: unbuffered -4 -> 104, then +4 from a buffer end of 104 ->
    // 100 (se_unbuf_*); buffered +4 -> 96 (se_buf_p4).
    const u64 u = h.open("mmio_p1.dat", MMIO_READ);
    EXPECT_EQ(h.call32("mmioSeek", {u, static_cast<u32>(-4), SEEK_END_VALUE}), 0x68u);
    EXPECT_EQ(h.call32("mmioSeek", {u, 4, SEEK_END_VALUE}), 0x64u);
    EXPECT_EQ(h.call32("mmioRead", {u, kBuf, 1}), 0u);
    EXPECT_EQ(h.call32("mmioClose", {u, 0}), 0u);
    const u64 b = h.open("mmio_p1.dat", MMIO_READ | MMIO_ALLOCBUF);
    EXPECT_EQ(h.call32("mmioSeek", {b, 4, SEEK_END_VALUE}), 0x60u);
    EXPECT_EQ(h.call32("mmioRead", {b, kBuf, 1}), 1u);
    EXPECT_EQ(h.u8_at(kBuf), 0xa1u);  // se_buf_read
    g = h.get(b);  // se_buf_get next=1 endr=4 bufoff=60 diskoff=64
    EXPECT_EQ(g.rnext(), 1);
    EXPECT_EQ(g.rendr(), 4);
    EXPECT_EQ(g.bufoff, 0x60);
    EXPECT_EQ(g.diskoff, 0x64);
    EXPECT_EQ(h.call32("mmioSeek", {b, static_cast<u32>(-200), SEEK_CUR_VALUE}), 0xffffff99u);  // se_cur_neg
    EXPECT_EQ(h.call32("mmioRead", {b, kBuf, 1}), kNeg1);  // se_read_neg
    EXPECT_EQ(h.call32("mmioClose", {b, 0}), 0u);
}

// =============================================================================
// Buffer choices at open (probe s_get100, s_get_neg, s_user*).
// =============================================================================
TEST_P(WinmmMmioTest, OpenBufferChoices) {
    MmioHarness h(GetParam());
    h.write_file("mmio_p1.dat", pattern(100));
    h.clear_info();
    h.set_info_u32(h.L().off_cchBuffer, 100);
    u64 f = h.open("mmio_p1.dat", MMIO_READ | MMIO_ALLOCBUF, true);
    EXPECT_EQ(h.get(f).cch, 100);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    // s_get_neg flags=0 cch=-5 buf=0 endw=-5
    h.clear_info();
    h.set_info_u32(h.L().off_cchBuffer, static_cast<u32>(-5));
    f = h.open("mmio_p1.dat", MMIO_READ | MMIO_ALLOCBUF, true);
    ASSERT_NE(f, 0u);
    GInfo g = h.get(f);
    EXPECT_EQ(g.flags, 0u);
    EXPECT_EQ(g.cch, -5);
    EXPECT_EQ(g.buf, 0u);
    EXPECT_EQ(g.endw, h.x64_ ? 0xfffffffffffffffbull : 0xfffffffbull);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    // A caller buffer (s_user0/1): used as is, flags 0, kept after close.
    for (u32 extra : {0u, MMIO_ALLOCBUF}) {
        h.clear_info();
        h.set_info_u32(h.L().off_cchBuffer, 24);
        h.put_ptr(kInfo + h.L().off_pchBuffer, kUserBuf);
        f = h.open("mmio_p1.dat", MMIO_READ | extra, true);
        ASSERT_NE(f, 0u);
        g = h.get(f);
        EXPECT_EQ(g.flags, 0u);
        EXPECT_EQ(g.buf, kUserBuf);
        EXPECT_EQ(g.rendw(), 24);
        EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 5}), 5u);
        EXPECT_EQ(h.u8_at(kUserBuf), 1u);      // s_user_read5 ub0=1
        EXPECT_EQ(h.u8_at(kUserBuf + 23), 0xa2u);  // ub23=a2
        g = h.get(f);
        EXPECT_EQ(g.rendr(), 24);
        EXPECT_EQ(g.diskoff, 24);
        EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
        EXPECT_EQ(h.u8_at(kUserBuf), 1u);  // s_user_after_close
    }
    EXPECT_EQ(h.live_blocks, 0);
}

// =============================================================================
// Unbuffered files and mmioSetBuffer (probe u_*).
// =============================================================================
TEST_P(WinmmMmioTest, UnbufferedAndSetBuffer) {
    MmioHarness h(GetParam());
    h.write_file("mmio_p1.dat", pattern(100));
    const u64 f = h.open("mmio_p1.dat", MMIO_READ);
    ASSERT_EQ(h.call32("mmioGetInfo", {f, kInfo, 0}), 0u);
    EXPECT_EQ(h.call32("mmioAdvance", {f, kInfo, MMIO_READ}), MMIOERR_UNBUFFERED);   // u_adv
    EXPECT_EQ(h.call32("mmioAdvance", {f, kInfo, MMIO_WRITE}), MMIOERR_UNBUFFERED);  // u_adv_w
    EXPECT_EQ(h.call32("mmioSetInfo", {f, kInfo, 0}), 0u);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 10}), 10u);
    GInfo g = h.get(f);  // u_get1 bufoff=a diskoff=a
    EXPECT_EQ(g.bufoff, 10);
    EXPECT_EQ(g.diskoff, 10);
    EXPECT_EQ(h.call32("mmioSetBuffer", {f, 0, 32, 0}), 0u);
    g = h.get(f);  // u_get2 flags=10000 cch=20 endw=20 bufoff=a
    EXPECT_EQ(g.flags, MMIO_ALLOCBUF);
    EXPECT_EQ(g.cch, 32);
    EXPECT_EQ(g.rendw(), 32);
    EXPECT_EQ(g.bufoff, 10);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 1}), 1u);
    EXPECT_EQ(h.u8_at(kBuf), 0x47u);  // u_read1
    g = h.get(f);  // u_get3 next=1 endr=20 diskoff=2a
    EXPECT_EQ(g.rendr(), 32);
    EXPECT_EQ(g.diskoff, 42);
    EXPECT_EQ(h.call32("mmioSetBuffer", {f, kUserBuf, 16, 0}), 0u);
    g = h.get(f);  // u_get4 flags=0 cch=10 buf_user=1 bufoff=b diskoff=2a
    EXPECT_EQ(g.flags, 0u);
    EXPECT_EQ(g.buf, kUserBuf);
    EXPECT_EQ(g.cch, 16);
    EXPECT_EQ(g.bufoff, 11);
    EXPECT_EQ(g.diskoff, 42);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 1}), 1u);
    EXPECT_EQ(h.u8_at(kBuf), 0x4eu);  // u_read1b
    EXPECT_EQ(h.get(f).diskoff, 27);  // u_get5 diskoff=1b
    EXPECT_EQ(h.call32("mmioSetBuffer", {f, 0, 0, 0}), 0u);
    g = h.get(f);  // u_get6 buf=0 bufoff=c diskoff=1b
    EXPECT_EQ(g.buf, 0u);
    EXPECT_EQ(g.bufoff, 12);
    EXPECT_EQ(g.diskoff, 27);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 1}), 1u);
    EXPECT_EQ(h.u8_at(kBuf), 0x55u);  // u_read1c
    EXPECT_EQ(h.call32("mmioSetBuffer", {f, 0, 0, 1}), 0u);
    EXPECT_EQ(h.call32("mmioSetBuffer", {f, 0, kNeg1, 0}), 0u);
    g = h.get(f);  // u_get7 cch=ffffffff endw=ffffffff bufoff=d diskoff=d
    EXPECT_EQ(g.cch, -1);
    EXPECT_EQ(g.endw, h.x64_ ? ~0ull : 0xffffffffull);
    EXPECT_EQ(g.bufoff, 13);
    EXPECT_EQ(g.diskoff, 13);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    EXPECT_EQ(h.live_blocks, 0);
}

// =============================================================================
// Writing through pchNext, MMIO_DIRTY, Advance(MMIO_WRITE), flush (w_*).
// =============================================================================
TEST_P(WinmmMmioTest, DirectWriteAndFlush) {
    MmioHarness h(GetParam());
    const u64 f = h.open("mmio_p2.dat", MMIO_CREATE | MMIO_READWRITE | MMIO_ALLOCBUF);
    ASSERT_NE(f, 0u);
    ASSERT_EQ(h.call32("mmioGetInfo", {f, kInfo, 0}), 0u);
    GInfo g = h.info_at(kInfo);
    EXPECT_EQ(g.flags, 0x11002u);  // w_get0
    for (usize k = 0; k < 50; ++k) ASSERT_TRUE(h.memory.write_u8(static_cast<GuestAddr>(g.next + k), pat(k)).is_ok());
    h.put_ptr(kInfo + h.L().off_pchNext, g.next + 50);
    h.set_info_u32(h.L().off_dwFlags, g.flags | MMIO_DIRTY);
    EXPECT_EQ(h.call32("mmioSetInfo", {f, kInfo, 0}), 0u);
    g = h.get(f);  // w_get1 flags=10011002 next=32 endr=32 diskoff=0
    EXPECT_EQ(g.flags, 0x10011002u);
    EXPECT_EQ(g.rnext(), 50);
    EXPECT_EQ(g.rendr(), 50);
    EXPECT_EQ(g.diskoff, 0);
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_CUR_VALUE}), 50u);
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_END_VALUE}), 50u);  // w_seek_end: unwritten data counts
    // Advance(MMIO_WRITE) with the caller's pchNext 6 bytes on (w_adv_mid).
    ASSERT_EQ(h.call32("mmioGetInfo", {f, kInfo, 0}), 0u);
    g = h.info_at(kInfo);
    for (u8 k = 0; k < 6; ++k) ASSERT_TRUE(h.memory.write_u8(static_cast<GuestAddr>(g.next + k), 0xF0 + k).is_ok());
    h.put_ptr(kInfo + h.L().off_pchNext, g.next + 6);
    EXPECT_EQ(h.call32("mmioAdvance", {f, kInfo, MMIO_WRITE}), 0u);
    g = h.info_at(kInfo);  // w_adv_mid_info flags=11002 next=0 endr=0 bufoff=38 diskoff=38
    EXPECT_EQ(g.flags, 0x11002u);
    EXPECT_EQ(g.rnext(), 0);
    EXPECT_EQ(g.rendr(), 0);
    EXPECT_EQ(g.bufoff, 56);
    EXPECT_EQ(g.diskoff, 56);
    EXPECT_EQ(h.call32("mmioSetInfo", {f, kInfo, 0}), 0u);
    EXPECT_EQ(h.read_file("mmio_p2.dat").size(), 56u);  // w_file_open size=38
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_SET_VALUE}), 0u);
    ASSERT_EQ(h.call32("mmioGetInfo", {f, kInfo, 0}), 0u);
    EXPECT_EQ(h.call32("mmioAdvance", {f, kInfo, MMIO_READ}), 0u);
    g = h.info_at(kInfo);  // w_adv_read_info endr=38 bufoff=0 diskoff=38
    EXPECT_EQ(g.rendr(), 56);
    EXPECT_EQ(g.bufoff, 0);
    EXPECT_EQ(g.diskoff, 56);
    EXPECT_EQ(h.call32("mmioSetInfo", {f, kInfo, 0}), 0u);
    // Written without MMIO_DIRTY: SetInfo still extends pchEndRead.
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_END_VALUE}), 56u);
    ASSERT_EQ(h.call32("mmioGetInfo", {f, kInfo, 0}), 0u);
    g = h.info_at(kInfo);
    for (u32 k = 0; k < 4; ++k) ASSERT_TRUE(h.memory.write_u8(static_cast<GuestAddr>(g.next + k), 'Z').is_ok());
    h.put_ptr(kInfo + h.L().off_pchNext, g.next + 4);
    EXPECT_EQ(h.call32("mmioSetInfo", {f, kInfo, 0}), 0u);
    g = h.get(f);  // w_nodirty1 flags=11002 next=3c endr=3c
    EXPECT_EQ(g.flags, 0x11002u);
    EXPECT_EQ(g.rendr(), 60);
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_END_VALUE}), 60u);
    h.put_str(kBuf, "abc");
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf, 3}), 3u);
    EXPECT_EQ(h.get(f).flags, 0x10011002u);  // w_after_write
    EXPECT_EQ(h.call32("mmioFlush", {f, 0}), 0u);
    g = h.get(f);  // w_after_flush flags=11002 next=3f endr=3f diskoff=3f
    EXPECT_EQ(g.flags, 0x11002u);
    EXPECT_EQ(g.rnext(), 63);
    EXPECT_EQ(g.diskoff, 63);
    EXPECT_EQ(h.read_file("mmio_p2.dat").size(), 63u);  // w_file_flushed
    EXPECT_EQ(h.call32("mmioFlush", {f, MMIO_EMPTYBUF}), 0u);
    g = h.get(f);  // w_after_empty next=0 endr=0 bufoff=3f
    EXPECT_EQ(g.rendr(), 0);
    EXPECT_EQ(g.bufoff, 63);
    EXPECT_EQ(h.call32("mmioSeek", {f, 10, SEEK_END_VALUE}), 53u);  // w_seek_past
    h.put_str(kBuf, "Q");
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf, 1}), 1u);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    EXPECT_EQ(h.read_file("mmio_p2.dat"),
              unhex("01080f161d242b323940474e555c636a71787f868d949ba2a9b0b7bec5ccd3dae1e8eff6fd040b121920272e353c434a5158"
                    "f0f1f251f4f55a5a5a5a616263"));  // w_file

    // Write-only buffered (wo_*), read-only Advance(MMIO_WRITE) (ro_*).
    const u64 wo = h.open("mmio_p2.dat", MMIO_CREATE | MMIO_WRITE | MMIO_ALLOCBUF);
    ASSERT_EQ(h.call32("mmioGetInfo", {wo, kInfo, 0}), 0u);
    EXPECT_EQ(h.call32("mmioAdvance", {wo, kInfo, MMIO_READ}), MMIOERR_CANNOTREAD);
    h.put_bytes(kBuf, pattern(5));
    EXPECT_EQ(h.call32("mmioWrite", {wo, kBuf, 5}), 5u);
    EXPECT_EQ(h.call32("mmioRead", {wo, kBuf, 4}), kNeg1);
    EXPECT_EQ(h.call32("mmioClose", {wo, 0}), 0u);
    EXPECT_EQ(h.read_file("mmio_p2.dat"), pattern(5));  // wo_file
    const u64 ro = h.open("mmio_p2.dat", MMIO_READ | MMIO_ALLOCBUF);
    ASSERT_EQ(h.call32("mmioGetInfo", {ro, kInfo, 0}), 0u);
    EXPECT_EQ(h.call32("mmioAdvance", {ro, kInfo, MMIO_WRITE}), 0u);  // ro_adv_write
    h.set_info_u32(h.L().off_dwFlags, h.info_at(kInfo).flags | MMIO_DIRTY);
    EXPECT_EQ(h.call32("mmioSetInfo", {ro, kInfo, 0}), 0u);
    EXPECT_EQ(h.call32("mmioClose", {ro, 0}), MMIOERR_CANNOTWRITE);  // ro_close
    // MMIO_READWRITE without MMIO_CREATE keeps the contents (rw_file).
    const u64 rw = h.open("mmio_p2.dat", MMIO_READWRITE | MMIO_DENYNONE);
    ASSERT_NE(rw, 0u);
    EXPECT_EQ(h.call32("mmioSeek", {rw, 0, SEEK_END_VALUE}), 5u);
    EXPECT_EQ(h.call32("mmioSeek", {rw, 1, SEEK_SET_VALUE}), 1u);
    h.put_str(kBuf, "xy");
    EXPECT_EQ(h.call32("mmioWrite", {rw, kBuf, 2}), 2u);
    EXPECT_EQ(h.call32("mmioClose", {rw, 0}), 0u);
    EXPECT_EQ(h.read_file("mmio_p2.dat"), unhex("017879161d"));
}

// =============================================================================
// Buffered writes against a 16-byte buffer (sw_*) and Advance semantics
// (am_*, aw_*).
// =============================================================================
TEST_P(WinmmMmioTest, BufferedWritesAndAdvance) {
    MmioHarness h(GetParam());
    h.clear_info();
    h.set_info_u32(h.L().off_cchBuffer, 16);
    const u64 f = h.open("mmio_p4.dat", MMIO_CREATE | MMIO_READWRITE | MMIO_ALLOCBUF, true);
    h.put_bytes(kBuf, pattern(64));
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf, 5}), 5u);
    GInfo g = h.get(f);  // sw_get5 next=5 endr=5 bufoff=0
    EXPECT_EQ(g.rnext(), 5);
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf + 5, 20}), 20u);
    g = h.get(f);  // sw_get20 next=9 endr=9 bufoff=10 diskoff=10
    EXPECT_EQ(g.rnext(), 9);
    EXPECT_EQ(g.rendr(), 9);
    EXPECT_EQ(g.bufoff, 16);
    EXPECT_EQ(g.diskoff, 16);
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf + 25, 16}), 16u);
    g = h.get(f);  // sw_get16 next=9 bufoff=20
    EXPECT_EQ(g.rnext(), 9);
    EXPECT_EQ(g.bufoff, 32);
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf + 41, 3}), 3u);
    EXPECT_EQ(h.get(f).rnext(), 12);  // sw_get3
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf + 44, 14}), 14u);
    g = h.get(f);  // sw_get14 next=a bufoff=30 diskoff=30
    EXPECT_EQ(g.rnext(), 10);
    EXPECT_EQ(g.bufoff, 48);
    EXPECT_EQ(g.diskoff, 48);
    EXPECT_EQ(h.call32("mmioSeek", {f, 2, SEEK_SET_VALUE}), 2u);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf + 0x100, 4}), 4u);
    EXPECT_EQ(h.u8_at(kBuf + 0x100), 0xfu);  // sw_read4 b0=f
    g = h.get(f);  // sw_getr4 flags=11002 next=4 endr=10 bufoff=2 diskoff=12
    EXPECT_EQ(g.flags, 0x11002u);
    EXPECT_EQ(g.rendr(), 16);
    EXPECT_EQ(g.bufoff, 2);
    EXPECT_EQ(g.diskoff, 18);
    h.put_str(kBuf + 0x100, "zz");
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf + 0x100, 2}), 2u);
    g = h.get(f);  // sw_getw2 next=6 endr=10
    EXPECT_EQ(g.rnext(), 6);
    EXPECT_EQ(g.rendr(), 16);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    EXPECT_EQ(h.read_file("mmio_p4.dat"),
              unhex("01080f161d247a7a3940474e555c636a71787f868d949ba2a9b0b7bec5ccd3dae1e8eff6fd040b121920272e353c434a5158"
                    "5f666d747b828990"));  // sw_file

    // am_*: Advance(MMIO_READ) mid-buffer refills from the current position.
    h.write_file("mmio_p1.dat", pattern(100));
    const u64 a = h.open("mmio_p1.dat", MMIO_READWRITE | MMIO_ALLOCBUF);
    EXPECT_EQ(h.call32("mmioRead", {a, kBuf, 10}), 10u);
    ASSERT_EQ(h.call32("mmioGetInfo", {a, kInfo, 0}), 0u);
    EXPECT_EQ(h.call32("mmioAdvance", {a, kInfo, MMIO_READ}), 0u);
    g = h.info_at(kInfo);  // am_adv_mid_info next=0 endr=5a bufoff=a diskoff=64
    EXPECT_EQ(g.rendr(), 90);
    EXPECT_EQ(g.bufoff, 10);
    EXPECT_EQ(g.diskoff, 100);
    EXPECT_EQ(h.call32("mmioSetInfo", {a, kInfo, 0}), 0u);
    EXPECT_EQ(h.call32("mmioSeek", {a, 4, SEEK_SET_VALUE}), 4u);
    EXPECT_EQ(h.call32("mmioWrite", {a, kBuf, 2}), 2u);
    g = h.get(a);  // am_after_write next=2 endr=2 bufoff=4
    EXPECT_EQ(g.rendr(), 2);
    EXPECT_EQ(g.bufoff, 4);
    ASSERT_EQ(h.call32("mmioGetInfo", {a, kInfo, 0}), 0u);
    EXPECT_EQ(h.call32("mmioAdvance", {a, kInfo, MMIO_READ}), 0u);
    g = h.info_at(kInfo);  // am_adv_dirty_info flags=10002 endr=5e bufoff=6
    EXPECT_EQ(g.flags, 0x10002u);
    EXPECT_EQ(g.rendr(), 94);
    EXPECT_EQ(g.bufoff, 6);
    EXPECT_EQ(h.call32("mmioSetInfo", {a, kInfo, 0}), 0u);
    ASSERT_EQ(h.call32("mmioGetInfo", {a, kInfo, 0}), 0u);
    EXPECT_EQ(h.call32("mmioAdvance", {a, kInfo, MMIO_WRITE}), 0u);
    g = h.info_at(kInfo);  // am_adv_w_clean next=0 endr=0 bufoff=6
    EXPECT_EQ(g.rendr(), 0);
    EXPECT_EQ(g.bufoff, 6);
    EXPECT_EQ(h.call32("mmioClose", {a, 0}), 0u);
    std::vector<u8> am = pattern(100);
    am[4] = pat(0);
    am[5] = pat(1);
    EXPECT_EQ(h.read_file("mmio_p1.dat"), am);  // am_file

    // aw_*: a dirty buffer is written up to pchEndRead, not pchNext.
    const u64 w = h.open("mmio_aw.dat", MMIO_CREATE | MMIO_READWRITE | MMIO_ALLOCBUF);
    h.put_bytes(kBuf, pattern(20));
    EXPECT_EQ(h.call32("mmioWrite", {w, kBuf, 20}), 20u);
    EXPECT_EQ(h.call32("mmioSeek", {w, 5, SEEK_SET_VALUE}), 5u);
    ASSERT_EQ(h.call32("mmioGetInfo", {w, kInfo, 0}), 0u);
    EXPECT_EQ(h.call32("mmioAdvance", {w, kInfo, MMIO_WRITE}), 0u);
    g = h.info_at(kInfo);  // aw_after flags=11002 next=0 endr=0 bufoff=5 diskoff=14
    EXPECT_EQ(g.flags, 0x11002u);
    EXPECT_EQ(g.bufoff, 5);
    EXPECT_EQ(g.diskoff, 20);
    EXPECT_EQ(h.read_file("mmio_aw.dat"), pattern(20));  // aw_file_mid
    EXPECT_EQ(h.call32("mmioClose", {w, 0}), 0u);
}

// =============================================================================
// Writing RIFF chunks and reading them back (probe k_*, d_*).
// =============================================================================
TEST_P(WinmmMmioTest, CreateAscendDescend) {
    MmioHarness h(GetParam());
    const u64 f = h.open("mmio_p3.wav", MMIO_CREATE | MMIO_READWRITE | MMIO_ALLOCBUF);
    ASSERT_NE(f, 0u);
    using Ck = std::array<u32, 5>;
    constexpr u32 D = MMIO_DIRTY;
    h.set_ck(kCk2, 0, 0, fourcc("WAVE"));
    EXPECT_EQ(h.call32("mmioCreateChunk", {f, kCk2, MMIO_CREATERIFF}), 0u);
    EXPECT_EQ(h.ck_at(kCk2), (Ck{FOURCC_RIFF, 0, fourcc("WAVE"), 8, D}));  // k_riff_create
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_CUR_VALUE}), 12u);
    h.set_ck(kCk, fourcc("fmt "), 16, 0);
    EXPECT_EQ(h.call32("mmioCreateChunk", {f, kCk, 0}), 0u);
    EXPECT_EQ(h.ck_at(kCk), (Ck{fourcc("fmt "), 16, 0, 0x14, D}));
    h.put_bytes(kBuf, pattern(64));
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf, 16}), 16u);
    EXPECT_EQ(h.call32("mmioAscend", {f, kCk, 0}), 0u);
    EXPECT_EQ(h.ck_at(kCk), (Ck{fourcc("fmt "), 16, 0, 0x14, D}));
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_CUR_VALUE}), 0x24u);
    h.set_ck(kCk, fourcc("data"), 0, 0);
    EXPECT_EQ(h.call32("mmioCreateChunk", {f, kCk, 0}), 0u);
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf + 32, 5}), 5u);
    EXPECT_EQ(h.call32("mmioAscend", {f, kCk, 0}), 0u);
    EXPECT_EQ(h.ck_at(kCk), (Ck{fourcc("data"), 5, 0, 0x2c, D}));  // k_data_ascend
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_CUR_VALUE}), 0x32u);
    const GuestAddr list = kData + 0x140, nam = kData + 0x160, junk = kData + 0x180;
    h.set_ck(list, 0, 0, fourcc("INFO"));
    EXPECT_EQ(h.call32("mmioCreateChunk", {f, list, MMIO_CREATELIST}), 0u);
    EXPECT_EQ(h.ck_at(list), (Ck{FOURCC_LIST, 0, fourcc("INFO"), 0x3a, D}));
    h.set_ck(nam, fourcc("INAM"), 99, 0);
    EXPECT_EQ(h.call32("mmioCreateChunk", {f, nam, 0}), 0u);
    h.put_str(kBuf + 0x100, "ab");
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf + 0x100, 3}), 3u);
    EXPECT_EQ(h.call32("mmioAscend", {f, nam, 0}), 0u);
    EXPECT_EQ(h.ck_at(nam), (Ck{fourcc("INAM"), 3, 0, 0x46, D}));    // k_nam_ascend
    EXPECT_EQ(h.call32("mmioAscend", {f, list, 0}), 0u);
    EXPECT_EQ(h.ck_at(list), (Ck{FOURCC_LIST, 0x10, fourcc("INFO"), 0x3a, D}));
    h.set_ck(junk, fourcc("junk"), 4, 0);
    EXPECT_EQ(h.call32("mmioCreateChunk", {f, junk, 0}), 0u);
    h.put_str(kBuf + 0x100, "1234");
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf + 0x100, 4}), 4u);
    ASSERT_TRUE(h.memory.write_u32(junk + 16, 0).is_ok());
    EXPECT_EQ(h.call32("mmioAscend", {f, junk, 0}), 0u);
    EXPECT_EQ(h.ck_at(junk), (Ck{fourcc("junk"), 4, 0, 0x52, 0}));  // k_junk_ascend_clean
    EXPECT_EQ(h.call32("mmioAscend", {f, kCk2, 0}), 0u);
    EXPECT_EQ(h.ck_at(kCk2), (Ck{FOURCC_RIFF, 0x4e, fourcc("WAVE"), 8, D}));  // k_riff_ascend
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_CUR_VALUE}), 0x56u);
    EXPECT_EQ(h.call32("mmioCreateChunk", {f, junk, 0x80}), MMSYSERR_INVALFLAG);  // k_flag_bad
    EXPECT_EQ(h.call32("mmioAscend", {f, junk, 1}), MMSYSERR_INVALFLAG);         // k_ascend_flag
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    const std::vector<u8> wav = unhex(
        "524946464e00000057415645666d74201000000001080f161d242b323940474e555c636a6461746105000000e1e8eff6fd00"
        "4c49535410000000494e464f494e414d03000000616200006a756e6b0400000031323334");
    EXPECT_EQ(h.read_file("mmio_p3.wav"), wav);  // k_file

    // An unbuffered read-only file cannot take a chunk (k_ro_create).
    const u64 ro = h.open("mmio_p3.wav", MMIO_READ);
    h.set_ck(kCk, fourcc("xxxx"), 0, 0);
    EXPECT_EQ(h.call32("mmioCreateChunk", {ro, kCk, 0}), MMIOERR_CANNOTWRITE);
    EXPECT_EQ(h.ck_at(kCk), (Ck{fourcc("xxxx"), 0, 0, 8, 0}));
    EXPECT_EQ(h.call32("mmioSeek", {ro, 0, SEEK_CUR_VALUE}), 0u);
    EXPECT_EQ(h.call32("mmioClose", {ro, 0}), 0u);

    // Descend (d_*).
    const u64 d = h.open("mmio_p3.wav", MMIO_READ | MMIO_ALLOCBUF);
    const GuestAddr riff = kCk2;
    h.set_ck(riff, 0, 0, fourcc("WAVE"));
    EXPECT_EQ(h.call32("mmioDescend", {d, riff, 0, MMIO_FINDRIFF}), 0u);
    EXPECT_EQ(h.ck_at(riff), (Ck{FOURCC_RIFF, 0x4e, fourcc("WAVE"), 8, 0}));  // d_riff
    EXPECT_EQ(h.call32("mmioSeek", {d, 0, SEEK_CUR_VALUE}), 12u);
    h.set_ck(kCk, fourcc("data"), 0, 0);
    EXPECT_EQ(h.call32("mmioDescend", {d, kCk, riff, MMIO_FINDCHUNK}), 0u);
    EXPECT_EQ(h.ck_at(kCk), (Ck{fourcc("data"), 5, 0, 0x2c, 0}));
    EXPECT_EQ(h.call32("mmioAscend", {d, kCk, 0}), 0u);
    EXPECT_EQ(h.call32("mmioSeek", {d, 0, SEEK_CUR_VALUE}), 0x32u);  // d_pos3
    h.set_ck(kCk, fourcc("fmt "), 0, 0);
    EXPECT_EQ(h.call32("mmioDescend", {d, kCk, riff, MMIO_FINDCHUNK}), MMIOERR_CHUNKNOTFOUND);
    EXPECT_EQ(h.ck_at(kCk), (Ck{fourcc("junk"), 4, 0, 0x52, 0}));  // d_fmt_after_data
    EXPECT_EQ(h.call32("mmioSeek", {d, 0, SEEK_CUR_VALUE}), 0x56u);
    EXPECT_EQ(h.call32("mmioSeek", {d, 12, SEEK_SET_VALUE}), 12u);
    h.zero(kCk, 20, 0xEE);
    EXPECT_EQ(h.call32("mmioDescend", {d, kCk, riff, 0}), 0u);
    EXPECT_EQ(h.ck_at(kCk), (Ck{fourcc("fmt "), 16, 0, 0x14, 0}));  // d_any
    EXPECT_EQ(h.call32("mmioAscend", {d, kCk, 0}), 0u);
    h.set_ck(list, 0, 0, fourcc("INFO"));
    EXPECT_EQ(h.call32("mmioDescend", {d, list, riff, MMIO_FINDLIST}), 0u);
    EXPECT_EQ(h.ck_at(list), (Ck{FOURCC_LIST, 0x10, fourcc("INFO"), 0x3a, 0}));  // d_list
    h.set_ck(nam, fourcc("INAM"), 0, 0);
    EXPECT_EQ(h.call32("mmioDescend", {d, nam, list, MMIO_FINDCHUNK}), 0u);
    EXPECT_EQ(h.call32("mmioAscend", {d, nam, 0}), 0u);
    EXPECT_EQ(h.call32("mmioSeek", {d, 0, SEEK_CUR_VALUE}), 0x4au);  // d_pos5
    h.zero(nam, 20, 0xEE);
    ASSERT_TRUE(h.memory.write_u32(nam, fourcc("zzzz")).is_ok());
    EXPECT_EQ(h.call32("mmioDescend", {d, nam, list, MMIO_FINDCHUNK}), MMIOERR_CHUNKNOTFOUND);
    EXPECT_EQ(h.ck_at(nam), (Ck{fourcc("junk"), 4, 0xeeeeeeee, 0x52, 0xeeeeeeee}));  // d_missing_in_list
    EXPECT_EQ(h.call32("mmioSeek", {d, 0, SEEK_CUR_VALUE}), 0x52u);
    EXPECT_EQ(h.call32("mmioSeek", {d, 0, SEEK_SET_VALUE}), 0u);
    h.set_ck(kCk, 0, 0, fourcc("AVI "));
    EXPECT_EQ(h.call32("mmioDescend", {d, kCk, 0, MMIO_FINDRIFF}), MMIOERR_CHUNKNOTFOUND);
    EXPECT_EQ(h.ck_at(kCk), (Ck{FOURCC_RIFF, 0x4e, fourcc("WAVE"), 8, 0}));  // d_wrong_riff
    EXPECT_EQ(h.call32("mmioSeek", {d, 0, SEEK_CUR_VALUE}), 0x56u);
    EXPECT_EQ(h.call32("mmioSeek", {d, 0, SEEK_SET_VALUE}), 0u);
    h.set_ck(kCk, fourcc("fmt "), 0, 0);
    EXPECT_EQ(h.call32("mmioDescend", {d, kCk, 0, MMIO_FINDCHUNK}), MMIOERR_CHUNKNOTFOUND);  // d_fmt_noparent
    EXPECT_EQ(h.call32("mmioSeek", {d, 0, SEEK_SET_VALUE}), 0u);
    h.set_ck(kCk, 0, 0, 0);
    EXPECT_EQ(h.call32("mmioDescend", {d, kCk, 0, 0x70}), 0u);
    EXPECT_EQ(h.ck_at(kCk), (Ck{FOURCC_RIFF, 0x4e, fourcc("WAVE"), 8, 0}));  // d_bad_flags
    EXPECT_EQ(h.call32("mmioDescend", {d, kCk, 0, MMIO_FINDRIFF | MMIO_FINDLIST}), MMIOERR_CHUNKNOTFOUND);
    EXPECT_EQ(h.ck_at(kCk), (Ck{fourcc("junk"), 4, fourcc("WAVE"), 0x52, 0}));  // fccType not re-read past LIST
    EXPECT_EQ(h.call32("mmioSeek", {d, 0, SEEK_END_VALUE}), 0x56u);
    h.set_ck(kCk, 0, 0, 0);
    EXPECT_EQ(h.call32("mmioDescend", {d, kCk, 0, 0}), MMIOERR_CHUNKNOTFOUND);  // d_at_eof
    EXPECT_EQ(h.ck_at(kCk), (Ck{0, 0, 0, 0, 0}));
    EXPECT_EQ(h.call32("mmioSeek", {d, 70, SEEK_SET_VALUE}), 70u);
    EXPECT_EQ(h.call32("mmioDescend", {d, kCk, 0, 0}), 0u);
    EXPECT_EQ(h.ck_at(kCk), (Ck{0x6261, 8, 0, 0x4e, 0}));  // d_partial: cut to the file
    // Find variants (d_find*): a zero id / form type matches anything.
    EXPECT_EQ(h.call32("mmioSeek", {d, 12, SEEK_SET_VALUE}), 12u);
    h.set_ck(kCk, FOURCC_LIST, 0, fourcc("XXXX"));
    EXPECT_EQ(h.call32("mmioDescend", {d, kCk, riff, MMIO_FINDCHUNK}), 0u);
    EXPECT_EQ(h.ck_at(kCk), (Ck{FOURCC_LIST, 0x10, fourcc("INFO"), 0x3a, 0}));
    EXPECT_EQ(h.call32("mmioSeek", {d, 0, SEEK_CUR_VALUE}), 0x3eu);  // d_pos_l
    EXPECT_EQ(h.call32("mmioSeek", {d, 0, SEEK_SET_VALUE}), 0u);
    h.set_ck(kCk, 0, 0, 0);
    EXPECT_EQ(h.call32("mmioDescend", {d, kCk, 0, MMIO_FINDRIFF}), 0u);  // d_findriff_type0
    EXPECT_EQ(h.call32("mmioSeek", {d, 12, SEEK_SET_VALUE}), 12u);
    h.set_ck(kCk, 0, 0, 0);
    EXPECT_EQ(h.call32("mmioDescend", {d, kCk, riff, MMIO_FINDLIST}), 0u);  // d_findlist_type0
    EXPECT_EQ(h.ck_at(kCk)[3], 0x3au);
    EXPECT_EQ(h.call32("mmioSeek", {d, 12, SEEK_SET_VALUE}), 12u);
    h.set_ck(kCk, 0, 0, 0);
    EXPECT_EQ(h.call32("mmioDescend", {d, kCk, riff, MMIO_FINDCHUNK}), 0u);
    EXPECT_EQ(h.ck_at(kCk)[0], fourcc("fmt "));  // d_findchunk_id0
    // Mid-chunk: garbage header, its size cut to the file (d_nonaligned...).
    EXPECT_EQ(h.call32("mmioSeek", {d, 0x14, SEEK_SET_VALUE}), 0x14u);
    h.zero(kCk, 20, 0xEE);
    EXPECT_EQ(h.call32("mmioDescend", {d, kCk, riff, 0}), 0u);
    EXPECT_EQ(h.ck_at(kCk), (Ck{0x160f0801, 0x3a, 0, 0x1c, 0}));
    // A chunk before the parent's data is outside it (d_parent_after_pos).
    EXPECT_EQ(h.call32("mmioSeek", {d, 0, SEEK_SET_VALUE}), 0u);
    h.set_ck(kCk, 0, 0, 0);
    EXPECT_EQ(h.call32("mmioDescend", {d, kCk, riff, 0}), MMIOERR_CHUNKNOTFOUND);
    EXPECT_EQ(h.ck_at(kCk), (Ck{FOURCC_RIFF, 0x4e, 0, 8, 0}));
    EXPECT_EQ(h.call32("mmioSeek", {d, 0, SEEK_CUR_VALUE}), 8u);
    // Ascend seeks to the end of the chunk, padded (d_ascend_back / _riff_odd).
    EXPECT_EQ(h.call32("mmioSeek", {d, 0x50, SEEK_SET_VALUE}), 0x50u);
    h.set_ck(kCk, fourcc("fmt "), 16, 0, 0x14);
    EXPECT_EQ(h.call32("mmioAscend", {d, kCk, 0}), 0u);
    EXPECT_EQ(h.call32("mmioSeek", {d, 0, SEEK_CUR_VALUE}), 0x24u);
    h.set_ck(kCk, FOURCC_RIFF, 3, 0, 8);
    EXPECT_EQ(h.call32("mmioAscend", {d, kCk, 0}), 0u);
    EXPECT_EQ(h.call32("mmioSeek", {d, 0, SEEK_CUR_VALUE}), 12u);
    // A dirty chunk on a read-only buffered file: the size goes into the
    // buffer, which can then not be written (d_ascend_dirty_ro, d_close_dirty_ro).
    EXPECT_EQ(h.call32("mmioSeek", {d, 0x20, SEEK_SET_VALUE}), 0x20u);
    h.set_ck(kCk, 0, 16, 0, 0x14, MMIO_DIRTY);
    EXPECT_EQ(h.call32("mmioAscend", {d, kCk, 0}), 0u);
    EXPECT_EQ(h.ck_at(kCk), (Ck{0, 0xc, 0, 0x14, MMIO_DIRTY}));
    EXPECT_EQ(h.call32("mmioSeek", {d, 0, SEEK_CUR_VALUE}), 0x20u);
    EXPECT_EQ(h.call32("mmioClose", {d, 0}), MMIOERR_CANNOTWRITE);
    EXPECT_EQ(h.read_file("mmio_p3.wav"), wav);
}

// =============================================================================
// Chunk bounds, size clamping, pad bytes, CreateChunk details (cb_*, pad_*, cc_*).
// =============================================================================
TEST_P(WinmmMmioTest, ChunkBoundsAndPadding) {
    MmioHarness h(GetParam());
    using Ck = std::array<u32, 5>;
    std::vector<u8> x(160, 0);
    const char hdr[] = "LIST\x0c\0\0\0TESTaaaa";
    std::memcpy(x.data(), hdr, 16);
    x[16] = 100;
    h.write_file("mmio_cb.dat", x);
    const u64 f = h.open("mmio_cb.dat", MMIO_READ);
    for (u32 parent_size : {12u, 8u, 5u}) {  // cb_size_overrun / _header_straddle / _outside
        h.set_ck(kCk2, FOURCC_LIST, parent_size, fourcc("TEST"), 8);
        EXPECT_EQ(h.call32("mmioSeek", {f, 12, SEEK_SET_VALUE}), 12u);
        h.zero(kCk, 20, 0xEE);
        EXPECT_EQ(h.call32("mmioDescend", {f, kCk, kCk2, 0}), 0u);
        EXPECT_EQ(h.ck_at(kCk), (Ck{fourcc("aaaa"), 100, 0, 0x14, 0}));
        EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_CUR_VALUE}), 0x14u);
    }
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    x[16] = 0;
    x[17] = 0x10;  // 4096
    h.write_file("mmio_cb.dat", x);
    const u64 b = h.open("mmio_cb.dat", MMIO_READ | MMIO_ALLOCBUF);
    EXPECT_EQ(h.call32("mmioSeek", {b, 12, SEEK_SET_VALUE}), 12u);
    h.zero(kCk, 20, 0xEE);
    EXPECT_EQ(h.call32("mmioDescend", {b, kCk, 0, 0}), 0u);
    EXPECT_EQ(h.ck_at(kCk), (Ck{fourcc("aaaa"), 0x8c, 0, 0x14, 0}));  // cb_clamp_noparent
    EXPECT_EQ(h.call32("mmioAscend", {b, kCk, 0}), 0u);
    EXPECT_EQ(h.call32("mmioSeek", {b, 0, SEEK_CUR_VALUE}), 0xa0u);  // cb_clamp_pos
    EXPECT_EQ(h.call32("mmioSeek", {b, 156, SEEK_SET_VALUE}), 156u);
    h.zero(kCk, 20, 0xEE);
    EXPECT_EQ(h.call32("mmioDescend", {b, kCk, 0, 0}), MMIOERR_CHUNKNOTFOUND);
    EXPECT_EQ(h.ck_at(kCk), (Ck{0, 0xeeeeeeee, 0xeeeeeeee, 0xeeeeeeee, 0xeeeeeeee}));  // cb_eof_header
    EXPECT_EQ(h.call32("mmioSeek", {b, 0, SEEK_CUR_VALUE}), 0xa0u);
    EXPECT_EQ(h.call32("mmioClose", {b, 0}), 0u);
    std::memcpy(x.data() + 152, "RIFF\x04\0\0\0", 8);
    h.write_file("mmio_cb.dat", x);
    const u64 c = h.open("mmio_cb.dat", MMIO_READ);
    EXPECT_EQ(h.call32("mmioSeek", {c, 152, SEEK_SET_VALUE}), 152u);
    h.zero(kCk, 20, 0xEE);
    EXPECT_EQ(h.call32("mmioDescend", {c, kCk, 0, 0}), MMIOERR_CHUNKNOTFOUND);
    EXPECT_EQ(h.ck_at(kCk), (Ck{FOURCC_RIFF, 0, 0xeeeeeeee, 0xa0, 0xeeeeeeee}));  // cb_riff_no_type
    EXPECT_EQ(h.call32("mmioClose", {c, 0}), 0u);

    // An odd dirty chunk gets a pad byte, a clean one does not (pad_*).
    u64 p = h.open("mmio_pad.dat", MMIO_CREATE | MMIO_WRITE);
    h.set_ck(kCk, fourcc("odd "), 0, 0);
    EXPECT_EQ(h.call32("mmioCreateChunk", {p, kCk, 0}), 0u);
    h.put_str(kBuf, "abc");
    EXPECT_EQ(h.call32("mmioWrite", {p, kBuf, 3}), 3u);
    EXPECT_EQ(h.call32("mmioAscend", {p, kCk, 0}), 0u);
    EXPECT_EQ(h.ck_at(kCk), (Ck{fourcc("odd "), 3, 0, 8, MMIO_DIRTY}));
    EXPECT_EQ(h.call32("mmioSeek", {p, 0, SEEK_CUR_VALUE}), 12u);
    EXPECT_EQ(h.call32("mmioClose", {p, 0}), 0u);
    EXPECT_EQ(h.read_file("mmio_pad.dat"), unhex("6f6464200300000061626300"));
    p = h.open("mmio_pad.dat", MMIO_CREATE | MMIO_WRITE);
    h.set_ck(kCk, fourcc("odd "), 3, 0);
    EXPECT_EQ(h.call32("mmioCreateChunk", {p, kCk, 0}), 0u);
    EXPECT_EQ(h.call32("mmioWrite", {p, kBuf, 3}), 3u);
    ASSERT_TRUE(h.memory.write_u32(kCk + 16, 0).is_ok());
    EXPECT_EQ(h.call32("mmioAscend", {p, kCk, 0}), 0u);
    EXPECT_EQ(h.call32("mmioClose", {p, 0}), 0u);
    EXPECT_EQ(h.read_file("mmio_pad.dat"), unhex("6f64642003000000616263"));
    // CreateChunk keeps cksize, does not align, RIFF beats LIST (cc_*).
    p = h.open("mmio_pad.dat", MMIO_CREATE | MMIO_READWRITE);
    h.put_str(kBuf, "Z");
    EXPECT_EQ(h.call32("mmioWrite", {p, kBuf, 1}), 1u);
    h.set_ck(kCk, fourcc("abcd"), 0x01020304, fourcc("type"));
    EXPECT_EQ(h.call32("mmioCreateChunk", {p, kCk, 0}), 0u);
    EXPECT_EQ(h.ck_at(kCk), (Ck{fourcc("abcd"), 0x01020304, fourcc("type"), 9, MMIO_DIRTY}));
    h.set_ck(kCk, fourcc("abcd"), 0x01020304, fourcc("type"));
    EXPECT_EQ(h.call32("mmioCreateChunk", {p, kCk, MMIO_CREATELIST}), 0u);
    EXPECT_EQ(h.ck_at(kCk)[0], FOURCC_LIST);
    EXPECT_EQ(h.ck_at(kCk)[3], 0x11u);
    EXPECT_EQ(h.call32("mmioCreateChunk", {p, kCk, MMIO_CREATELIST | MMIO_CREATERIFF}), 0u);
    EXPECT_EQ(h.ck_at(kCk)[0], FOURCC_RIFF);
    EXPECT_EQ(h.ck_at(kCk)[3], 0x1du);
    EXPECT_EQ(h.call32("mmioSeek", {p, 0, SEEK_CUR_VALUE}), 0x21u);
    EXPECT_EQ(h.call32("mmioClose", {p, 0}), 0u);
    EXPECT_EQ(h.read_file("mmio_pad.dat"),
              unhex("5a61626364040302014c4953540403020174797065524946460403020174797065"));  // cc_file
}

// =============================================================================
// Memory files (probe m_*, mw_*, mc_*, me_*, mg_*, mro_*, mf_*).
// =============================================================================
TEST_P(WinmmMmioTest, MemoryFiles) {
    MmioHarness h(GetParam());
    const GuestAddr mb = kUserBuf;
    h.put_bytes(mb, pattern(64));
    const auto mem_open = [&](u32 flags, GuestAddr buf, i32 cch, u32 inc) {
        h.clear_info();
        h.set_info_u32(h.L().off_fccIOProc, FOURCC_MEM);
        h.put_ptr(kInfo + h.L().off_pchBuffer, buf);
        h.set_info_u32(h.L().off_cchBuffer, static_cast<u32>(cch));
        h.set_info_u32(h.L().off_adwInfo, inc);
        return h.call("mmioOpenA", {0, kInfo, flags});
    };
    u64 f = mem_open(MMIO_READ, mb, 64, 0);
    ASSERT_NE(f, 0u);
    GInfo g = h.get(f);  // m_get0 flags=0 fcc=MEM proc=1 next=0 endr=40 endw=40
    EXPECT_EQ(g.flags, 0u);
    EXPECT_EQ(g.fcc, FOURCC_MEM);
    EXPECT_EQ(g.proc, kMemProc);
    EXPECT_EQ(g.buf, mb);
    EXPECT_EQ(g.rendr(), 64);
    EXPECT_EQ(g.rendw(), 64);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 10}), 10u);
    EXPECT_EQ(h.bytes_at(kBuf, 10), pattern(10));
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_END_VALUE}), 64u);
    EXPECT_EQ(h.call32("mmioSeek", {f, 100, SEEK_SET_VALUE}), kNeg1);  // m_seek_100
    EXPECT_EQ(h.get(f).rnext(), 64);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 4}), 0u);
    EXPECT_EQ(h.call32("mmioSeek", {f, 60, SEEK_SET_VALUE}), 60u);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 10}), 4u);  // m_read_tail
    ASSERT_EQ(h.call32("mmioGetInfo", {f, kInfo, 0}), 0u);
    EXPECT_EQ(h.call32("mmioAdvance", {f, kInfo, MMIO_READ}), MMIOERR_CANNOTREAD);  // m_adv_eof
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf, 1}), kNeg1);   // m_write_ro (full)
    EXPECT_EQ(h.call32("mmioSetBuffer", {f, 0, 128, 0}), 0u);
    g = h.get(f);  // m_get3 flags=10000 cch=80 buf_user=0 endr=0 bufoff=0
    EXPECT_EQ(g.flags, MMIO_ALLOCBUF);
    EXPECT_EQ(g.cch, 128);
    EXPECT_NE(g.buf, mb);
    EXPECT_EQ(g.rendr(), 0);
    EXPECT_EQ(g.bufoff, 0);
    EXPECT_EQ(h.call32("mmioFlush", {f, 0}), 0u);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);

    // Fixed read/write: a write that does not fit fails whole (mw_*).
    f = mem_open(MMIO_READWRITE, mb, 64, 0);
    h.put_bytes(kBuf, pattern(70, 100));
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf, 70}), kNeg1);
    EXPECT_EQ(h.u8_at(mb), 1u);
    EXPECT_EQ(h.u8_at(mb + 63), 0xbau);
    ASSERT_EQ(h.call32("mmioGetInfo", {f, kInfo, 0}), 0u);
    h.set_info_u32(h.L().off_dwFlags, h.info_at(kInfo).flags | MMIO_DIRTY);
    EXPECT_EQ(h.call32("mmioAdvance", {f, kInfo, MMIO_WRITE}), MMIOERR_CANNOTEXPAND);  // mw_adv_w
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_END_VALUE}), 64u);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    // MMIO_CREATE empties it (mc_get0 endr=0).
    f = mem_open(MMIO_READWRITE | MMIO_CREATE, mb, 64, 0);
    EXPECT_EQ(h.get(f).rendr(), 0);
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_END_VALUE}), 0u);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    // A write into a read-only memory file that fits succeeds (mro_*).
    f = mem_open(MMIO_READ, mb, 64, 0);
    h.put_str(kBuf, "Q");
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf, 1}), 1u);
    EXPECT_EQ(h.u8_at(mb), 'Q');
    EXPECT_EQ(h.get(f).flags, MMIO_DIRTY);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    f = mem_open(MMIO_READWRITE, mb, 64, 0);
    EXPECT_EQ(h.call32("mmioSeek", {f, 60, SEEK_SET_VALUE}), 60u);
    h.put_str(kBuf, "WXYZ");
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf, 4}), 4u);   // mf_write4
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf, 1}), kNeg1);  // mf_write1
    EXPECT_EQ(h.call32("mmioSeek", {f, 62, SEEK_SET_VALUE}), 62u);
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf, 3}), kNeg1);  // mf_write3
    EXPECT_EQ(h.u8_at(mb + 62), 'Y');
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);

    // Expandable: internal buffer of cchBuffer growing by adwInfo[0] (mg_*).
    f = mem_open(MMIO_READWRITE | MMIO_CREATE, 0, 16, 32);
    ASSERT_NE(f, 0u);
    h.put_bytes(kBuf, pattern(64));
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf, 10}), 10u);
    g = h.get(f);  // mg_get10 flags=10011002 cch=10 a0=20
    EXPECT_EQ(g.flags, 0x10011002u);
    EXPECT_EQ(g.cch, 16);
    EXPECT_EQ(g.adw[0], 32u);
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf, 10}), 10u);
    EXPECT_EQ(h.get(f).cch, 0x30);  // mg_get20
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf, 50}), 50u);
    g = h.get(f);  // mg_get70 cch=50 next=46 endr=46
    EXPECT_EQ(g.cch, 0x50);
    EXPECT_EQ(g.rendr(), 0x46);
    EXPECT_EQ(h.call32("mmioSeek", {f, 90, SEEK_SET_VALUE}), kNeg1);
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_END_VALUE}), 0x46u);
    EXPECT_EQ(h.call32("mmioSeek", {f, static_cast<u32>(-2), SEEK_END_VALUE}), kNeg1);
    EXPECT_EQ(h.call32("mmioSeek", {f, 2, SEEK_END_VALUE}), 0x44u);
    EXPECT_EQ(h.call32("mmioSeek", {f, kNeg1, SEEK_SET_VALUE}), kNeg1);
    ASSERT_EQ(h.call32("mmioGetInfo", {f, kInfo, 0}), 0u);
    EXPECT_EQ(h.call32("mmioAdvance", {f, kInfo, MMIO_WRITE}), 0u);
    g = h.info_at(kInfo);  // mg_adv_76_info cch=70 next=44 endr=46
    EXPECT_EQ(g.cch, 0x70);
    EXPECT_EQ(g.rnext(), 0x44);
    EXPECT_EQ(g.rendr(), 0x46);
    h.put_ptr(kInfo + h.L().off_pchNext, g.endw);
    EXPECT_EQ(h.call32("mmioAdvance", {f, kInfo, MMIO_WRITE}), 0u);
    g = h.info_at(kInfo);  // mg_adv_full_info cch=90 next=70 endr=70
    EXPECT_EQ(g.cch, 0x90);
    EXPECT_EQ(g.rnext(), 0x70);
    EXPECT_EQ(g.rendr(), 0x70);
    EXPECT_EQ(h.call32("mmioAdvance", {f, kInfo, MMIO_READ}), MMIOERR_CANNOTREAD);
    EXPECT_EQ(h.call32("mmioSetInfo", {f, kInfo, 0}), 0u);
    EXPECT_EQ(h.call32("mmioFlush", {f, MMIO_EMPTYBUF}), 0u);
    g = h.get(f);  // mg_after_flush: nothing emptied, still dirty
    EXPECT_EQ(g.flags, 0x10011002u);
    EXPECT_EQ(g.rnext(), 0x70);
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_SET_VALUE}), 0u);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf + 0x200, 30}), 30u);
    // Three writes of pat[0..]: 10, 10, then 50 bytes.
    std::vector<u8> expect = pattern(10);
    for (int i = 0; i < 2; ++i) {
        const std::vector<u8> again = pattern(10);
        expect.insert(expect.end(), again.begin(), again.end());
    }
    EXPECT_EQ(h.bytes_at(kBuf + 0x200, 30), expect);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    // One write larger than an increment grows to exactly what it needs
    // (me v=0 cch=64 after 100 bytes); a default buffer does not grow (v=2).
    f = mem_open(MMIO_READWRITE | MMIO_CREATE | MMIO_ALLOCBUF, 0, 16, 32);
    h.put_bytes(kBuf, pattern(100));
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf, 100}), 100u);
    EXPECT_EQ(h.get(f).cch, 100);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    f = mem_open(MMIO_READWRITE | MMIO_CREATE | MMIO_ALLOCBUF, 0, 0, 32);
    g = h.get(f);
    EXPECT_EQ(g.cch, 0x2000);
    EXPECT_EQ(g.flags, 0x11002u);
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf, 100}), 100u);
    ASSERT_EQ(h.call32("mmioGetInfo", {f, kInfo, 0}), 0u);
    EXPECT_EQ(h.call32("mmioAdvance", {f, kInfo, MMIO_WRITE}), MMIOERR_CANNOTEXPAND);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    // No buffer at all still opens (m_nobuf).
    f = mem_open(MMIO_READ, 0, 0, 0);
    ASSERT_NE(f, 0u);
    EXPECT_EQ(h.get(f).buf, 0u);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    EXPECT_EQ(h.live_blocks, 0);
}

// =============================================================================
// mmioOpen flags and failures (probe o_*, fh_*).
// =============================================================================
TEST_P(WinmmMmioTest, OpenFlagsAndErrors) {
    MmioHarness h(GetParam());
    h.write_file("mmio_p1.dat", pattern(100));
    h.clear_info();
    EXPECT_EQ(h.open("no_such_file.dat", MMIO_READ, true), 0u);
    EXPECT_EQ(h.info_at(kInfo).err, MMIOERR_FILENOTFOUND);  // o_missing
    h.clear_info();
    EXPECT_EQ(h.open("no_such_dir\\x.dat", MMIO_CREATE | MMIO_WRITE, true), 0u);
    EXPECT_EQ(h.info_at(kInfo).err, MMIOERR_FILENOTFOUND);  // o_create_nodir
    h.clear_info();
    EXPECT_EQ(h.open("", MMIO_READ, true), 0u);
    EXPECT_EQ(h.info_at(kInfo).err, MMIOERR_FILENOTFOUND);  // o_empty_name
    // o_null_name / o_null_both: a NULL name opens adwInfo[0] (here 0).
    h.clear_info();
    u64 f = h.call("mmioOpenA", {0, kInfo, MMIO_READ});
    EXPECT_NE(f, 0u);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 1}), kNeg1);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    f = h.call("mmioOpenA", {0, 0, MMIO_READ});
    EXPECT_NE(f, 0u);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    // o_garbage_*: mmioOpen writes only wErrorRet back; adwInfo[1..2],
    // dwReserved and htask are copied into the file's MMIOINFO.
    h.zero(kInfo, h.L().size, 0x11);
    h.put_ptr(kInfo + h.L().off_pchBuffer, 0);
    h.set_info_u32(h.L().off_cchBuffer, 0);
    h.set_info_u32(h.L().off_fccIOProc, 0);
    h.put_ptr(kInfo + h.L().off_pIOProc, 0);
    f = h.open("mmio_p1.dat", MMIO_READ, true);
    ASSERT_NE(f, 0u);
    GInfo after = h.info_at(kInfo);
    EXPECT_EQ(after.err, 0u);
    EXPECT_EQ(after.flags, 0x11111111u);
    EXPECT_EQ(after.bufoff, 0x11111111);
    GInfo g = h.get(f);
    EXPECT_EQ(g.flags, 0u);
    EXPECT_EQ(g.adw[1], 0x11111111u);
    EXPECT_EQ(g.res2, 0x11111111u);
    EXPECT_NE(g.task, 0u);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    // MMIO_EXIST (o_exist*): TRUE / FALSE + 0x101, the name untouched.
    h.clear_info();
    EXPECT_EQ(h.open("mmio_p1.dat", MMIO_EXIST, true), 1u);
    EXPECT_EQ(h.str_at(kName), "mmio_p1.dat");
    h.clear_info();
    EXPECT_EQ(h.open("no_such_file.dat", MMIO_EXIST, true), 0u);
    EXPECT_EQ(h.info_at(kInfo).err, MMIOERR_FILENOTFOUND);
    // MMIO_PARSE (o_parse*): fully qualified in place, case kept.
    h.clear_info();
    EXPECT_EQ(h.open("rel.wav", MMIO_PARSE, true), 1u);
    EXPECT_EQ(h.str_at(kName), "C:\\rel.wav");
    EXPECT_EQ(h.open("C:\\Some\\..\\Dir\\File.Wav", MMIO_PARSE), 1u);
    EXPECT_EQ(h.str_at(kName), "C:\\Dir\\File.Wav");
    EXPECT_EQ(h.open("c:/a/./b.wav", MMIO_PARSE), 1u);
    EXPECT_EQ(h.str_at(kName), "c:\\a\\b.wav");
    EXPECT_EQ(h.open("x.abc+inner.wav", MMIO_PARSE), 1u);
    EXPECT_EQ(h.str_at(kName), "C:\\x.abc+inner.wav");
    h.clear_info();
    EXPECT_EQ(h.open("", MMIO_PARSE, true), 0u);
    EXPECT_EQ(h.info_at(kInfo).err, MMIOERR_FILENOTFOUND);
    h.put_wstr(kName, u"rel.wav");
    EXPECT_EQ(h.call("mmioOpenW", {kName, 0, MMIO_PARSE}), 1u);  // o_parsew
    EXPECT_EQ(h.u8_at(kName), 'C');
    // MMIO_DELETE (o_delete*).
    h.write_file("mmio_del.dat", {});
    EXPECT_EQ(h.open("mmio_del.dat", MMIO_DELETE), 1u);
    EXPECT_FALSE(h.exists("mmio_del.dat"));
    h.clear_info();
    EXPECT_EQ(h.open("mmio_del.dat", MMIO_DELETE, true), 0u);
    EXPECT_EQ(h.info_at(kInfo).err, MMIOERR_FILENOTFOUND);
    // Share modes (o_excl ... o_compat_rw).
    h.write_file("mmio_sh.dat", pattern(10));
    const auto pair = [&](u32 first, u32 second) {
        const u64 a = h.open("mmio_sh.dat", first);
        h.clear_info();
        const u64 b = h.open("mmio_sh.dat", second, true);
        const bool ok = b != 0;
        if (!ok) EXPECT_EQ(h.info_at(kInfo).err, MMIOERR_FILENOTFOUND);
        if (b) (void)h.call32("mmioClose", {b, 0});
        if (a) (void)h.call32("mmioClose", {a, 0});
        return ok;
    };
    EXPECT_FALSE(pair(MMIO_READ | MMIO_EXCLUSIVE, MMIO_READ));
    EXPECT_FALSE(pair(MMIO_READ | MMIO_DENYWRITE, MMIO_READWRITE));
    EXPECT_TRUE(pair(MMIO_READ | MMIO_DENYWRITE, MMIO_READ));
    EXPECT_TRUE(pair(MMIO_READ | MMIO_DENYNONE, MMIO_READWRITE | MMIO_DENYNONE));
    EXPECT_FALSE(pair(MMIO_READ | MMIO_DENYREAD, MMIO_READ | MMIO_DENYNONE));
    EXPECT_TRUE(pair(MMIO_READ, MMIO_READ));
    EXPECT_TRUE(pair(MMIO_READ, MMIO_READWRITE));
    // mmioOpenW (o_openw_read b7=32).
    h.put_wstr(kName, u"mmio_p1.dat");
    f = h.call("mmioOpenW", {kName, 0, MMIO_READ | MMIO_ALLOCBUF});
    ASSERT_NE(f, 0u);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 8}), 8u);
    EXPECT_EQ(h.u8_at(kBuf + 7), 0x32u);
    EXPECT_EQ(h.call32("mmioClose", {f, 1}), 0u);  // o_close_flag1
    // mmioSendMessage reaches the DOS procedure (o_send_*).
    f = h.open("mmio_p1.dat", MMIO_READ);
    EXPECT_EQ(h.call32("mmioSendMessage", {f, MMIOM_USER, 1, 2}), 0u);
    EXPECT_EQ(h.call32("mmioSendMessage", {f, 0x100, 1, 2}), 0u);
    EXPECT_EQ(h.call32("mmioSendMessage", {f, MMIOM_SEEK, 10, SEEK_SET_VALUE}), 10u);
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_CUR_VALUE}), 0u);
    EXPECT_EQ(h.call32("mmioSendMessage", {f, MMIOM_READ, kBuf, 2}), 2u);
    EXPECT_EQ(h.u8_at(kBuf), 0x47u);
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_CUR_VALUE}), 0u);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    // An open file handle (fh_*): I/O starts at its pointer; MMIO_FHOPEN
    // leaves it open, a plain close closes it.
    auto opened = h.vfs.open("C:\\mmio_p1.dat", capsule::AccessMode::Read, capsule::ShareMode::ReadWrite,
                             capsule::CreationDisposition::OpenExisting);
    ASSERT_TRUE(opened.is_ok());
    ASSERT_TRUE(h.vfs.seek(opened.value(), 3, capsule::SeekOrigin::Begin).is_ok());
    const u32 kh = 0x9000;
    h.handles[kh] = opened.value();
    h.clear_info();
    h.set_info_u32(h.L().off_adwInfo, kh);
    f = h.call("mmioOpenA", {0, kInfo, MMIO_READ});
    ASSERT_NE(f, 0u);
    g = h.get(f);
    EXPECT_EQ(g.bufoff, 3);
    EXPECT_EQ(g.diskoff, 3);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 1}), 1u);
    EXPECT_EQ(h.u8_at(kBuf), 0x16u);
    EXPECT_EQ(h.call32("mmioClose", {f, MMIO_FHOPEN}), 0u);
    EXPECT_EQ(h.handles.count(kh), 1u);
    f = h.call("mmioOpenA", {0, kInfo, MMIO_READ});
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    EXPECT_EQ(h.handles.count(kh), 0u);
    // mmioRename (o_rename*): MoveFile semantics, 0x101 on failure.
    h.write_file("mmio_ren_a.dat", {});
    h.put_str(kName, "mmio_ren_a.dat");
    h.put_str(kName2, "mmio_ren_b.dat");
    EXPECT_EQ(h.call32("mmioRenameA", {kName, kName2, 0, 0}), 0u);
    EXPECT_FALSE(h.exists("mmio_ren_a.dat"));
    EXPECT_TRUE(h.exists("mmio_ren_b.dat"));
    EXPECT_EQ(h.call32("mmioRenameA", {kName, kName2, 0, 0}), MMIOERR_FILENOTFOUND);
    h.write_file("mmio_ren_t.dat", {});
    h.put_str(kName, "mmio_ren_b.dat");
    h.put_str(kName2, "mmio_ren_t.dat");
    EXPECT_EQ(h.call32("mmioRenameA", {kName, kName2, 0, 0}), MMIOERR_FILENOTFOUND);
    h.put_wstr(kName, u"mmio_ren_b.dat");
    h.put_wstr(kName2, u"mmio_ren_c.dat");
    EXPECT_EQ(h.call32("mmioRenameW", {kName, kName2, 0, 1}), 0u);
    EXPECT_TRUE(h.exists("mmio_ren_c.dat"));
    EXPECT_EQ(h.live_blocks, 0);
}

// =============================================================================
// Invalid handles and NULL arguments (probe "risky" children).
// =============================================================================
TEST_P(WinmmMmioTest, BadHandlesAndNullArguments) {
    MmioHarness h(GetParam());
    const u64 bad = 0x12345;
    EXPECT_EQ(h.call32("mmioClose", {bad, 0}), MMSYSERR_INVALHANDLE);        // 0
    EXPECT_EQ(h.call32("mmioRead", {bad, kBuf, 4}), kNeg1);                  // 1
    EXPECT_EQ(h.call32("mmioWrite", {bad, kBuf, 4}), kNeg1);                 // 2
    EXPECT_EQ(h.call32("mmioSeek", {bad, 0, 0}), kNeg1);                     // 3
    EXPECT_EQ(h.call32("mmioGetInfo", {bad, kInfo, 0}), MMSYSERR_INVALHANDLE);    // 4
    EXPECT_EQ(h.call32("mmioSetInfo", {bad, kInfo, 0}), MMSYSERR_INVALHANDLE);    // 5
    EXPECT_EQ(h.call32("mmioAdvance", {bad, kInfo, 0}), MMSYSERR_INVALHANDLE);    // 6
    EXPECT_EQ(h.call32("mmioFlush", {bad, 0}), MMSYSERR_INVALHANDLE);             // 7
    EXPECT_EQ(h.call32("mmioSetBuffer", {bad, 0, 0, 0}), MMSYSERR_INVALHANDLE);   // 8
    EXPECT_EQ(h.call32("mmioAscend", {bad, kCk, 0}), MMIOERR_CANNOTSEEK);         // a
    EXPECT_EQ(h.call32("mmioCreateChunk", {bad, kCk, 0}), MMIOERR_CANNOTSEEK);    // b
    EXPECT_EQ(h.call32("mmioSendMessage", {bad, MMIOM_USER, 0, 0}), 0u);          // c
    EXPECT_EQ(h.call32("mmioClose", {0, 0}), MMSYSERR_INVALHANDLE);               // d
    EXPECT_EQ(h.call32("mmioRead", {0, kBuf, 4}), kNeg1);                         // e
    EXPECT_EQ(h.call32("mmioGetInfo", {0, kInfo, 0}), MMSYSERR_INVALHANDLE);      // 10
    h.write_file("mmio_p1.dat", pattern(100));
    u64 f = h.open("mmio_p1.dat", MMIO_READ);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), MMSYSERR_INVALHANDLE);  // 11: closed handle
    f = h.open("mmio_p1.dat", MMIO_READ);
    EXPECT_EQ(h.call32("mmioGetInfo", {f, 0, 0}), MMSYSERR_INVALPARAM);      // 12
    EXPECT_EQ(h.call32("mmioDescend", {f, 0, 0, 0}), MMSYSERR_INVALPARAM);   // 14
    EXPECT_EQ(h.call32("mmioRead", {f, 0, 4}), kNeg1);                       // 15
    EXPECT_EQ(h.call32("mmioSetInfo", {f, 0, 0}), MMSYSERR_INVALPARAM);      // 16
    EXPECT_EQ(h.call32("mmioAscend", {f, 0, 0}), MMSYSERR_INVALPARAM);       // 17
    EXPECT_EQ(h.call32("mmioCreateChunk", {f, 0, 0}), MMSYSERR_INVALPARAM);  // 18
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, kNeg1}), kNeg1);                // 1d
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf, kNeg1}), kNeg1);               // 1e
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    f = h.open("mmio_p1.dat", MMIO_READ | MMIO_ALLOCBUF);
    EXPECT_EQ(h.call32("mmioAdvance", {f, 0, MMIO_READ}), 0u);  // 13
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
}

// =============================================================================
// Custom I/O procedures (probe p_*, pb_*, pw_*, pp_*, pd_*, it*).
// =============================================================================
TEST_P(WinmmMmioTest, CustomIOProcMessages) {
    MmioHarness h(GetParam());
    h.install_tproc(kProcA);
    const u32 tst = fourcc("TST ");
    const auto install = [&](u32 fcc, GuestAddr proc, u32 flags) {
        return h.call("mmioInstallIOProcA", {fcc, proc, flags});
    };
    EXPECT_EQ(install(tst, 0, MMIO_FINDPROC), 0u);        // p_find_before
    EXPECT_EQ(install(tst, kProcA, MMIO_INSTALLPROC), kProcA);
    EXPECT_EQ(install(tst, 0, MMIO_FINDPROC), kProcA);
    EXPECT_EQ(install(fourcc("tst "), 0, MMIO_FINDPROC), 0u);  // p_find_lower
    EXPECT_EQ(install(FOURCC_DOS, 0, MMIO_FINDPROC), kDosProc);
    EXPECT_EQ(install(FOURCC_MEM, 0, MMIO_FINDPROC), kMemProc);

    // Unbuffered (p_log).
    h.clear_info();
    h.set_info_u32(h.L().off_fccIOProc, tst);
    u64 f = h.open("virt.bin", MMIO_READ, true);
    ASSERT_NE(f, 0u);
    GInfo g = h.get(f);  // p_get0 fcc=TST proc=1 a0=55
    EXPECT_EQ(g.fcc, tst);
    EXPECT_EQ(g.proc, kProcA);
    EXPECT_EQ(g.adw[0], 0x55u);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 10}), 10u);
    EXPECT_EQ(h.u8_at(kBuf + 9), 0x1bu);
    EXPECT_EQ(h.call32("mmioSeek", {f, 30, SEEK_SET_VALUE}), 30u);
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_CUR_VALUE}), 30u);
    EXPECT_EQ(h.call32("mmioSeek", {f, static_cast<u32>(-4), SEEK_END_VALUE}), 0x44u);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 4}), 0u);
    EXPECT_EQ(h.call("mmioSendMessage", {f, MMIOM_USER, 7, 8}), 0x1234u);
    EXPECT_EQ(h.call("mmioSendMessage", {f, 0x100, 7, 8}), 0x1234u);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    struct Want { u32 msg; u64 l1; u64 l2; i32 disk; u32 a0; };
    const std::vector<Want> plog = {
        {MMIOM_OPEN, 1, 0, 0, 0}, {MMIOM_READ, 1, 10, 0, 0x55}, {MMIOM_SEEK, 0, 2, 10, 0x55},
        {MMIOM_SEEK, 0x44, 0, 0x40, 0x55}, {MMIOM_READ, 1, 4, 0x44, 0x55},
        {0x8000, 7, 8, 0x44, 0x55}, {0x100, 7, 8, 0x44, 0x55}, {MMIOM_CLOSE, 0, 0, 0x44, 0x55}};
    ASSERT_EQ(h.log.size(), plog.size());
    for (usize i = 0; i < plog.size(); ++i) {
        const ProcCall& c = h.log[i];
        EXPECT_EQ(c.msg, plog[i].msg) << i;
        const bool pointer = c.msg == MMIOM_OPEN || c.msg == MMIOM_READ;
        EXPECT_EQ(pointer ? static_cast<u64>(c.l1 != 0) : c.l1, plog[i].l1) << i;
        EXPECT_EQ(c.l2, plog[i].l2) << i;
        EXPECT_EQ(c.disk, plog[i].disk) << i;
        EXPECT_EQ(c.a0, plog[i].a0) << i;
        EXPECT_TRUE(c.info_is_h) << i;
        EXPECT_FALSE(c.buf) << i;
    }
    EXPECT_EQ(h.log[0].name, "virt.bin");
    h.log.clear();

    // Buffered: the buffer exists at MMIOM_OPEN (pb_log).
    h.clear_info();
    h.set_info_u32(h.L().off_fccIOProc, tst);
    f = h.open("virt.bin", MMIO_READ | MMIO_ALLOCBUF, true);
    EXPECT_EQ(h.call32("mmioRead", {f, kBuf, 10}), 10u);
    g = h.get(f);  // pb_get1 next=a endr=40 diskoff=40
    EXPECT_EQ(g.rendr(), 64);
    EXPECT_EQ(h.call32("mmioSeek", {f, 30, SEEK_SET_VALUE}), 30u);
    EXPECT_EQ(h.call32("mmioSeek", {f, 0, SEEK_END_VALUE}), 64u);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    ASSERT_EQ(h.log.size(), 4u);
    EXPECT_EQ(h.log[0].msg, MMIOM_OPEN);
    EXPECT_EQ(h.log[0].flags, MMIO_ALLOCBUF);
    EXPECT_TRUE(h.log[0].buf);
    EXPECT_EQ(h.log[1].msg, MMIOM_READ);
    EXPECT_EQ(h.log[1].l2, 0x2000u);
    EXPECT_EQ(h.log[2].msg, MMIOM_SEEK);
    EXPECT_EQ(h.log[2].l2, SEEK_END_VALUE);
    EXPECT_EQ(h.log[3].msg, MMIOM_CLOSE);
    h.log.clear();

    // Buffered writes: flush and close send MMIOM_WRITEFLUSH (pw_log).
    h.clear_info();
    h.set_info_u32(h.L().off_fccIOProc, tst);
    f = h.open("virt.bin", MMIO_READWRITE | MMIO_ALLOCBUF, true);
    h.put_str(kBuf, "hello");
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf, 5}), 5u);
    EXPECT_EQ(h.call32("mmioFlush", {f, 0}), 0u);
    h.put_str(kBuf, "ab");
    EXPECT_EQ(h.call32("mmioWrite", {f, kBuf, 2}), 2u);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    EXPECT_EQ(h.vfile[0], 'h');  // pw_vfile v0=68 v6=62
    EXPECT_EQ(h.vfile[6], 'b');
    const std::vector<Want> pwlog = {
        {MMIOM_OPEN, 1, 0, 0, 0}, {MMIOM_WRITEFLUSH, 1, 5, 0, 0x55}, {MMIOM_SEEK, 0, 0, 5, 0x55},
        {MMIOM_WRITEFLUSH, 1, 7, 0, 0x55}, {MMIOM_CLOSE, 0, 0, 7, 0x55}};
    ASSERT_EQ(h.log.size(), pwlog.size());
    for (usize i = 0; i < pwlog.size(); ++i) {
        EXPECT_EQ(h.log[i].msg, pwlog[i].msg) << i;
        EXPECT_EQ(h.log[i].l2, pwlog[i].l2) << i;
        EXPECT_EQ(h.log[i].disk, pwlog[i].disk) << i;
    }
    EXPECT_EQ(h.log[1].flags, 0x10010002u);
    EXPECT_EQ(h.log[4].flags, 0x10002u);
    h.log.clear();

    // "+" names pick the procedure by extension (pp_*).
    for (const char* name : {"outer.tst+inner.wav", "outer.TST+inner.wav", "a.b.tst+c"}) {
        h.clear_info();
        f = h.open(name, MMIO_READ, true);
        ASSERT_NE(f, 0u) << name;
        EXPECT_EQ(h.get(f).fcc, tst);
        EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
        ASSERT_FALSE(h.log.empty()) << name;
        EXPECT_EQ(h.log[0].name, name);
        h.log.clear();
    }
    for (const char* name : {"noext+c", "outer.zzz+inner.wav"}) {
        h.clear_info();
        EXPECT_EQ(h.open(name, MMIO_READ, true), 0u);
        EXPECT_EQ(h.info_at(kInfo).err, MMIOERR_FILENOTFOUND);
    }
    // pIOProc directly, and ahead of fccIOProc (pd_*).
    h.clear_info();
    h.put_ptr(kInfo + h.L().off_pIOProc, kProcA);
    f = h.open("direct.bin", MMIO_READ, true);
    ASSERT_NE(f, 0u);
    EXPECT_EQ(h.get(f).fcc, 0u);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    h.clear_info();
    h.put_ptr(kInfo + h.L().off_pIOProc, kProcA);
    h.set_info_u32(h.L().off_fccIOProc, FOURCC_DOS);
    f = h.open("direct.bin", MMIO_READ, true);
    ASSERT_NE(f, 0u);
    g = h.get(f);
    EXPECT_EQ(g.fcc, FOURCC_DOS);
    EXPECT_EQ(g.adw[0], 0x55u);
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    h.clear_info();
    h.set_info_u32(h.L().off_fccIOProc, fourcc("QQQQ"));
    EXPECT_EQ(h.open("virt.bin", MMIO_READ, true), 0u);  // p_unknown_fcc
    EXPECT_EQ(h.info_at(kInfo).err, MMIOERR_FILENOTFOUND);
    // mmioOpenW hands an ANSI procedure an ANSI name (pw_openw_log).
    h.log.clear();
    h.clear_info();
    h.set_info_u32(h.L().off_fccIOProc, tst);
    h.put_wstr(kName, u"wide.bin");
    f = h.call("mmioOpenW", {kName, kInfo, MMIO_READ});
    ASSERT_NE(f, 0u);
    ASSERT_FALSE(h.log.empty());
    EXPECT_EQ(h.log[0].name, "wide.bin");
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    // mmioRename through the procedure (p_rename_log info_is_h=0).
    h.log.clear();
    h.clear_info();
    h.set_info_u32(h.L().off_fccIOProc, tst);
    h.put_str(kName, "one.bin");
    h.put_str(kName2, "two.bin");
    EXPECT_EQ(h.call32("mmioRenameA", {kName, kName2, kInfo, 0}), 0u);
    ASSERT_EQ(h.log.size(), 1u);
    EXPECT_EQ(h.log[0].msg, MMIOM_RENAME);
    EXPECT_EQ(h.log[0].name, "one.bin");
    EXPECT_FALSE(h.log[0].info_is_h);
    // An installed DOS procedure replaces the built-in one (p_dos_over_log).
    h.write_file("mmio_p1.dat", pattern(100));
    EXPECT_EQ(install(FOURCC_DOS, kProcA, MMIO_INSTALLPROC), kProcA);
    h.log.clear();
    f = h.open("mmio_p1.dat", MMIO_READ);
    ASSERT_NE(f, 0u);
    ASSERT_FALSE(h.log.empty());
    EXPECT_EQ(h.log[0].name, "mmio_p1.dat");
    EXPECT_EQ(h.call32("mmioClose", {f, 0}), 0u);
    EXPECT_EQ(install(FOURCC_DOS, 0, MMIO_REMOVEPROC), kProcA);  // it14 rm_dos=p
    EXPECT_EQ(install(FOURCC_DOS, 0, MMIO_FINDPROC), kDosProc);
    EXPECT_EQ(h.live_blocks, 0);
}

TEST_P(WinmmMmioTest, InstallIOProcTable) {
    MmioHarness h(GetParam());
    const GuestAddr p = kProcA, p2 = kProcB;
    char n = 'a';
    const auto F = [&]() { return fourcc("ITS ") & 0x00FFFFFFu | static_cast<u32>(static_cast<u8>(n++)) << 24; };
    const auto ins = [&](u32 f, GuestAddr proc, u32 flags, bool wide = false) {
        return h.call(wide ? "mmioInstallIOProcW" : "mmioInstallIOProcA", {f, proc, flags});
    };
    u32 f = F();  // it1 ins=p find0=p findp=0 findp2=0 rm=0 find=p rm2=0
    EXPECT_EQ(ins(f, p, MMIO_INSTALLPROC), p);
    EXPECT_EQ(ins(f, 0, MMIO_FINDPROC), p);
    EXPECT_EQ(ins(f, p, MMIO_FINDPROC), 0u);
    EXPECT_EQ(ins(f, p2, MMIO_FINDPROC), 0u);
    EXPECT_EQ(ins(f, p, MMIO_REMOVEPROC), 0u);
    EXPECT_EQ(ins(f, 0, MMIO_FINDPROC), p);
    f = F();  // it2 ins=p rm0=p find=0
    EXPECT_EQ(ins(f, p, MMIO_INSTALLPROC), p);
    EXPECT_EQ(ins(f, 0, MMIO_REMOVEPROC), p);
    EXPECT_EQ(ins(f, 0, MMIO_FINDPROC), 0u);
    f = F();  // it4 insrm=p find=p
    EXPECT_EQ(ins(f, p, MMIO_INSTALLPROC | MMIO_REMOVEPROC), p);
    EXPECT_EQ(ins(f, 0, MMIO_FINDPROC), p);
    f = F();  // it5 insfind=p find=p
    EXPECT_EQ(ins(f, p, MMIO_INSTALLPROC | MMIO_FINDPROC), p);
    EXPECT_EQ(ins(f, 0, MMIO_FINDPROC), p);
    f = F();  // it6 insg=p rm=0 rmg=0 find3=p
    EXPECT_EQ(ins(f, p, MMIO_INSTALLPROC | MMIO_GLOBALPROC), p);
    EXPECT_EQ(ins(f, p, MMIO_REMOVEPROC), 0u);
    EXPECT_EQ(ins(f, p, MMIO_REMOVEPROC | MMIO_GLOBALPROC), 0u);
    EXPECT_EQ(ins(f, 0, MMIO_FINDPROC), p);
    f = F();  // it7 ins0=0 find=0
    EXPECT_EQ(ins(f, p, 0), 0u);
    EXPECT_EQ(ins(f, 0, MMIO_FINDPROC), 0u);
    f = F();  // it8: newest first
    EXPECT_EQ(ins(f, p2, MMIO_INSTALLPROC), p2);
    EXPECT_EQ(ins(f, p, MMIO_INSTALLPROC), p);
    EXPECT_EQ(ins(f, 0, MMIO_FINDPROC), p);
    EXPECT_EQ(ins(f, 0, MMIO_REMOVEPROC), p);
    EXPECT_EQ(ins(f, 0, MMIO_FINDPROC), p2);
    f = F();  // it9 rm_none=0 find_none=0
    EXPECT_EQ(ins(f, 0, MMIO_REMOVEPROC), 0u);
    EXPECT_EQ(ins(f, 0, MMIO_FINDPROC), 0u);
    f = F();  // it10: MMIO_UNICODEPROC is refused
    EXPECT_EQ(ins(f, p, MMIO_INSTALLPROC | MMIO_UNICODEPROC), 0u);
    EXPECT_EQ(ins(f, p, MMIO_INSTALLPROC | MMIO_UNICODEPROC, true), 0u);
    EXPECT_EQ(ins(f, 0, MMIO_FINDPROC), 0u);
    f = F();  // it11 insw=p finda=p findw=p
    EXPECT_EQ(ins(f, p, MMIO_INSTALLPROC, true), p);
    EXPECT_EQ(ins(f, 0, MMIO_FINDPROC), p);
    EXPECT_EQ(ins(f, 0, MMIO_FINDPROC, true), p);
    // p_install_null / p_install_fcc0 / p_find_fcc0 / p_remove_fcc0
    EXPECT_EQ(ins(F(), 0, MMIO_INSTALLPROC), 0u);
    EXPECT_EQ(ins(0, p, MMIO_INSTALLPROC), 0u);
    EXPECT_EQ(ins(0, 0, MMIO_FINDPROC), 0u);
    EXPECT_EQ(ins(0, p, MMIO_REMOVEPROC), 0u);
    // it14: built-ins are found but cannot be removed.
    EXPECT_EQ(ins(FOURCC_DOS, p, MMIO_FINDPROC), 0u);
    EXPECT_EQ(ins(FOURCC_DOS, 0, MMIO_REMOVEPROC), 0u);
    EXPECT_EQ(ins(FOURCC_DOS, 0, MMIO_FINDPROC), kDosProc);
}

// =============================================================================
// The built-in DOS procedure as a guest-callable MMIOPROC (a custom
// procedure chaining to it).
// =============================================================================
TEST_P(WinmmMmioTest, BuiltinDosProcIsCallable) {
    MmioHarness h(GetParam());
    h.write_file("mmio_p1.dat", pattern(100));
    // MMIOM_OPEN on a scratch MMIOINFO opens the file into adwInfo[0].
    h.clear_info();
    h.set_info_u32(h.L().off_dwFlags, MMIO_READ);
    h.put_str(kName, "mmio_p1.dat");
    EXPECT_EQ(h.call("VitreoMmioDosIOProc", {kInfo, MMIOM_OPEN, kName, 0}), 0u);
    const u32 kh = h.info_at(kInfo).adw[0];
    EXPECT_NE(kh, 0u);
    EXPECT_EQ(h.call("VitreoMmioDosIOProc", {kInfo, MMIOM_SEEK, 5, SEEK_SET_VALUE}), 5u);
    EXPECT_EQ(h.call("VitreoMmioDosIOProc", {kInfo, MMIOM_READ, kBuf, 3}), 3u);
    EXPECT_EQ(h.bytes_at(kBuf, 3), pattern(3, 5));
    EXPECT_EQ(h.info_at(kInfo).diskoff, 8);
    EXPECT_EQ(h.call("VitreoMmioDosIOProc", {kInfo, MMIOM_CLOSE, 0, 0}), 0u);
    EXPECT_EQ(h.handles.count(kh), 0u);
}
