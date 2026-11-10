/// @file test_crt_legacy_exports.cpp
/// @brief msvcrt exports VC6-era images import: type_info::name, _mbctype and
///        the VC6 locale globals, _find*, _filelength, getc, _putch,
///        _mbsrchr, __crtCompareStringA / __crtLCMapStringA, _EH_prolog.
///
/// Expected values are Windows' own output, recorded by
/// tools/oracle_probe/crt_legacy_exports_probe.c and
/// crt_findfirst_errors_probe.c (tests/oracle_data/*.expected).

#include "crt_stdio_test_fixture.hpp"

#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/locale/locale_string.hpp"
#include "winapi/msvcrt/io/crt_find.hpp"
#include "winapi/msvcrt/runtime/crt_init.hpp"
#include "winapi/msvcrt/system/crt_environment.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/msvcrt/runtime/crt_type_name.hpp"
#include "winapi/msvcrt/text/crt_nls_helpers.hpp"
#include "winapi/msvcrt/text/crt_string.hpp"

#include <sys/time.h>

#include <map>

namespace {

std::vector<std::vector<std::string>> oracle_rows(const char* file) {
    std::ifstream in(std::string(VITREO_ORACLE_PROBE_DIR) + "/" + file);
    EXPECT_TRUE(in.good()) << file;
    std::vector<std::vector<std::string>> rows;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> fields;
        std::size_t start = 0;
        for (;;) {
            const auto tab = line.find('\t', start);
            fields.push_back(line.substr(start, tab - start));
            if (tab == std::string::npos) break;
            start = tab + 1;
        }
        rows.push_back(std::move(fields));
    }
    return rows;
}

const char* oracle_file(bool x64) {
    return x64 ? "crt_legacy_exports_probe64.expected"
               : "crt_legacy_exports_probe32.expected";
}

std::vector<u8> from_hex(const std::string& hex) {
    std::vector<u8> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<u8>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

const ExportEntry* find_entry(const DllExports& exports, const char* name) {
    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                           [name](const ExportEntry& e) { return e.name == name; });
    return it == exports.entries.end() ? nullptr : &*it;
}

/// A guest with memory at 0x10000-0x30000 (stack top 0x2f000) that calls an
/// export with cdecl (x86) or Microsoft x64 arguments.
struct Guest {
    explicit Guest(bool x64_abi) : x64(x64_abi) {
        EXPECT_TRUE(memory.map_region(0x10000, 0x20000, MemoryProtection::ReadWrite).is_ok());
        reset();
    }
    void reset() {
        cpu = cpu::CpuState{};
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = kStack;
    }
    u64 call(const ExportEntry* e, std::initializer_list<u64> args) {
        EXPECT_NE(e, nullptr);
        if (e == nullptr) return 0;
        reset();
        usize i = 0;
        for (u64 a : args) {
            if (x64) {
                if (i == 0) cpu.gpr.rcx = a;
                else if (i == 1) cpu.gpr.rdx = a;
                else if (i == 2) cpu.gpr.r8 = a;
                else if (i == 3) cpu.gpr.r9 = a;
                else EXPECT_TRUE(memory.write_u64(kStack + 8 * (i + 1), a).is_ok());
            } else {
                EXPECT_TRUE(memory.write_u32(kStack + 4 + 4 * static_cast<u32>(i),
                                             static_cast<u32>(a)).is_ok());
            }
            ++i;
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        e->handler(m);
        return x64 ? cpu.gpr.rax : (cpu.gpr.rax & 0xFFFFFFFFu);
    }
    GuestAddr bytes(GuestAddr at, const std::string& s) {
        for (usize i = 0; i < s.size(); ++i) {
            EXPECT_TRUE(memory.write_u8(at + i, static_cast<u8>(s[i])).is_ok());
        }
        EXPECT_TRUE(memory.write_u8(at + s.size(), 0).is_ok());
        return at;
    }
    GuestAddr wide(GuestAddr at, const std::string& s) {
        for (usize i = 0; i < s.size(); ++i) {
            EXPECT_TRUE(memory.write_u16(at + 2 * i, static_cast<u8>(s[i])).is_ok());
        }
        EXPECT_TRUE(memory.write_u16(at + 2 * s.size(), 0).is_ok());
        return at;
    }
    std::vector<u8> read(GuestAddr at, usize n) {
        std::vector<u8> out(n);
        for (usize i = 0; i < n; ++i) out[i] = memory.read_u8(at + i).value_or(0);
        return out;
    }
    static constexpr GuestAddr kStack = 0x2f000;
    bool x64;
    GuestMemory memory;
    cpu::CpuState cpu;
};

}  // namespace

// ---------------------------------------------------------------------------
// type_info::name
// ---------------------------------------------------------------------------

TEST(CrtTypeInfoName, EveryRecordedEncodingMatchesWindowsBothAbis) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        usize count = 0;
        for (const auto& row : oracle_rows(oracle_file(x64))) {
            if (row.size() < 4 || row[0] != "tname") continue;
            ++count;
            EXPECT_EQ(msvcrt_type_info_name(row[1]), row[2]) << "encoding " << row[1];
        }
        EXPECT_GE(count, 216u);
    }
}

TEST(CrtTypeInfoName, ExportUndecoratesOnceAndReturnsTheCachedText) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        Guest g(x64);
        CrtMemoryManager mm(g.memory, 0x40000000, 0x100000);
        const DllExports exports = create_crt_memory_thunks(mm);
        const ExportEntry* name =
            find_entry(exports, x64 ? "?name@type_info@@QEBAPEBDXZ" : "?name@type_info@@QBEPBDXZ");
        ASSERT_NE(name, nullptr);
        // struct type_info { vfptr; _M_data; char _M_d_name[]; }
        const u32 ps = x64 ? 8 : 4;
        constexpr GuestAddr self = 0x11000;
        for (u32 i = 0; i < 0x80; ++i) ASSERT_TRUE(g.memory.write_u8(self + i, 0).is_ok());
        g.bytes(self + 2 * ps, ".?AV?$vector@HV?$allocator@H@std@@@std@@");
        g.reset();
        g.cpu.gpr.rcx = self;  // thiscall `this` (x86) / first argument (x64)
        Marshaler m(g.memory);
        m.attach_cpu(g.cpu);
        name->handler(m);
        const GuestAddr text = x64 ? g.cpu.gpr.rax : (g.cpu.gpr.rax & 0xFFFFFFFFu);
        ASSERT_NE(text, 0u);
        const auto bytes = g.read(text, 64);
        EXPECT_STREQ(reinterpret_cast<const char*>(bytes.data()),
                     "class std::vector<int,class std::allocator<int> >");
        const u64 cached = x64 ? g.memory.read_u64(self + ps).value()
                               : g.memory.read_u32(self + ps).value();
        EXPECT_EQ(cached, text);
        // A second call hands back the cached pointer unchanged.
        g.cpu.gpr.rcx = self;
        g.cpu.gpr.rax = 0;
        name->handler(m);
        EXPECT_EQ(x64 ? g.cpu.gpr.rax : (g.cpu.gpr.rax & 0xFFFFFFFFu), text);
    }
}

// ---------------------------------------------------------------------------
// _mbctype and the VC6 locale globals
// ---------------------------------------------------------------------------

TEST(CrtLegacyData, MbctypeAndLocaleGlobalsMatchWindowsStartupState) {
    std::map<std::string, std::string> oracle;
    for (const auto& row : oracle_rows(oracle_file(false))) {
        if (row.size() >= 2) oracle[row[0]] = row[1];
    }
    GuestMemory memory;
    CrtDataBlock data;
    ASSERT_TRUE(data.initialize(memory));
    const std::vector<u8> want = from_hex(oracle["_mbctype"]);
    ASSERT_EQ(want.size(), CrtDataBlock::MBCTYPE_SIZE);
    const u64 table = data.resolve_data_import("_mbctype");
    ASSERT_EQ(table, data.mbctype_addr());
    for (usize i = 0; i < want.size(); ++i) {
        EXPECT_EQ(memory.read_u8(table + i).value(), want[i]) << "index " << i;
    }
    for (const char* name : {"__lc_codepage", "__lc_collate_cp", "__setlc_active",
                             "__unguarded_readlc_active"}) {
        const u64 addr = data.resolve_data_import(name);
        ASSERT_NE(addr, 0u) << name;
        EXPECT_EQ(std::to_string(memory.read_u32(addr).value()), oracle[name]) << name;
    }
    const u64 handles = data.resolve_data_import("__lc_handle");
    ASSERT_NE(handles, 0u);
    std::string handle_text;
    for (u32 i = 0; i < 6; ++i) {
        handle_text += std::to_string(memory.read_u32(handles + 4 * i).value()) + " ";
    }
    EXPECT_EQ(handle_text, oracle["__lc_handle"]);
    // The globals are distinct cells.
    EXPECT_NE(data.resolve_data_import("__lc_codepage"),
              data.resolve_data_import("__lc_collate_cp"));
}

TEST(CrtLegacyData, PMbctypeReturnsTheDataExport) {
    Guest g(false);
    CrtDataBlock data;
    ASSERT_TRUE(data.initialize(g.memory));
    CrtInitManager init;
    EnvironmentManager environment("test.exe", g.memory);
    CrtMemoryManager mm(g.memory);
    CrtEnvironmentManager env(environment, g.memory, mm);
    const DllExports exports = create_crt_init_thunks(init, data, env);
    EXPECT_EQ(g.call(find_entry(exports, "__p__mbctype"), {}), data.mbctype_addr());
}

// ---------------------------------------------------------------------------
// _find*
// ---------------------------------------------------------------------------

namespace {

struct FindRig {
    explicit FindRig(bool x64_abi) : g(x64_abi) {
        dir = std::filesystem::temp_directory_path() /
              ("vitreo_crt_find_" + std::to_string(::getpid()) + (x64_abi ? "_64" : "_32"));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir / "fprobe" / "sub");
        write(dir / "fprobe" / "a.txt", 5);
        write(dir / "fprobe" / "b_archive.dat", 70000);
        write(dir / "fprobe" / "Long Name File.data", 3);
        write(dir / "fprobe" / "sub" / "d_2040.txt", 0);
        { std::ofstream f(dir / "fprobe" / "sub" / "e_5g.bin"); }
        std::filesystem::resize_file(dir / "fprobe" / "sub" / "e_5g.bin", 5368709120ull);
        // access 2002-01-01, write 2003-01-01; d_2040 is written 2040-01-01.
        set_times(dir / "fprobe" / "a.txt", 1009843200, 1041379200);
        set_times(dir / "fprobe" / "b_archive.dat", 1009843200, 1041379200);
        set_times(dir / "fprobe" / "Long Name File.data", 1009843200, 1041379200);
        set_times(dir / "fprobe" / "sub" / "d_2040.txt", 1009843200, 2208988800);
        EXPECT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(dir)).is_ok());
        vfs.set_cwd("C:\\");
        search.set_vfs(&vfs);
        exports = create_crt_find_thunks(search, mm);
    }
    ~FindRig() { std::filesystem::remove_all(dir); }
    static void write(const std::filesystem::path& p, usize size) {
        std::ofstream f(p, std::ios::binary);
        for (usize i = 0; i < size; ++i) f.put(static_cast<char>(i));
    }
    static void set_times(const std::filesystem::path& p, long access, long write) {
        timeval tv[2] = {{access, 0}, {write, 0}};
        EXPECT_EQ(::utimes(p.c_str(), tv), 0);
    }
    const ExportEntry* e(const char* name) { return find_entry(exports, name); }
    i64 first(const char* fn, const std::string& pattern, bool wide) {
        for (u32 i = 0; i < 0x280; ++i) (void)g.memory.write_u8(kOut + i, 0xcc);
        if (wide) g.wide(kPattern, pattern); else g.bytes(kPattern, pattern);
        mm.set_errno(0);
        const u64 r = g.call(e(fn), {kPattern, kOut});
        return g.x64 ? static_cast<i64>(r) : static_cast<i32>(r);
    }
    i32 next(const char* fn, u64 handle) {
        for (u32 i = 0; i < 0x280; ++i) (void)g.memory.write_u8(kOut + i, 0xcc);
        mm.set_errno(0);
        return static_cast<i32>(g.call(e(fn), {handle, kOut}));
    }
    std::string name_at(u32 offset, bool wide) {
        std::string s;
        for (u32 i = 0; i < 260; ++i) {
            const u32 c = wide ? g.memory.read_u16(kOut + offset + 2 * i).value()
                               : g.memory.read_u8(kOut + offset + i).value();
            if (c == 0) break;
            s.push_back(static_cast<char>(c));
        }
        return s;
    }
    static constexpr GuestAddr kPattern = 0x12000;
    static constexpr GuestAddr kOut = 0x13000;
    Guest g;
    std::filesystem::path dir;
    capsule::Vfs vfs;
    FileSearchManager search;
    CrtMemoryManager mm{g.memory, 0x40000000, 0x100000};
    DllExports exports;
};

struct FindLayout {
    u32 time[3];
    bool time64;
    u32 size;
    bool size64;
    u32 name;
};

// Measured offsets (crt_legacy_exports_probe "entry" rows).
FindLayout layout_of(const std::string& fn, bool x64) {
    const bool sized64 = fn.find("64") != std::string::npos;
    const bool i64 = fn.find("i64") != std::string::npos;
    if (!sized64 && !i64) {  // _findfirst / _wfindfirst
        return x64 ? FindLayout{{8, 16, 24}, true, 32, false, 36}
                   : FindLayout{{4, 8, 12}, false, 16, false, 20};
    }
    if (i64 && fn.find("first64") == std::string::npos && fn.find("next64") == std::string::npos) {
        return x64 ? FindLayout{{8, 16, 24}, true, 32, true, 40}
                   : FindLayout{{4, 8, 12}, false, 16, true, 24};
    }
    return FindLayout{{8, 16, 24}, true, 32, true, 40};
}

}  // namespace

TEST(CrtFind, EveryMsvcrtVariantFillsTheMeasuredLayoutBothAbis) {
    struct Pair { const char* first; const char* next; bool wide; };
    const Pair pairs[] = {
        {"_findfirst", "_findnext", false},       {"_findfirsti64", "_findnexti64", false},
        {"_findfirst64", "_findnext64", false},   {"_wfindfirst", "_wfindnext", true},
        {"_wfindfirsti64", "_wfindnexti64", true}, {"_wfindfirst64", "_wfindnext64", true},
    };
    for (bool x64 : {false, true}) {
        FindRig rig(x64);
        for (const Pair& p : pairs) {
            SCOPED_TRACE(std::string(p.first) + (x64 ? " x64" : " x86"));
            const FindLayout L = layout_of(p.first, x64);
            const i64 handle = rig.first(p.first, "fprobe\\*", p.wide);
            ASSERT_NE(handle, -1);
            EXPECT_EQ(rig.mm.get_errno(), 0u);
            std::map<std::string, u64> sizes;
            std::map<std::string, u32> attribs;
            std::map<std::string, u64> writes;
            for (int guard = 0; guard < 16; ++guard) {
                const std::string name = rig.name_at(L.name, p.wide);
                sizes[name] = L.size64 ? rig.g.memory.read_u64(FindRig::kOut + L.size).value()
                                       : rig.g.memory.read_u32(FindRig::kOut + L.size).value();
                attribs[name] = rig.g.memory.read_u32(FindRig::kOut).value();
                writes[name] = L.time64 ? rig.g.memory.read_u64(FindRig::kOut + L.time[2]).value()
                                        : rig.g.memory.read_u32(FindRig::kOut + L.time[2]).value();
                // Only the fields and the name's terminator are stored.
                if (L.time64) {
                    EXPECT_EQ(rig.g.memory.read_u32(FindRig::kOut + 4).value(), 0xccccccccu);
                }
                const u32 unit = p.wide ? 2 : 1;
                EXPECT_EQ(rig.g.memory.read_u8(FindRig::kOut + L.name + (name.size() + 1) * unit).value(),
                          0xcc);
                if (rig.next(p.next, static_cast<u64>(handle)) != 0) break;
            }
            // End of search: -1, ENOENT, buffer untouched; and again.
            EXPECT_EQ(rig.mm.get_errno(), 2u);
            EXPECT_EQ(rig.g.memory.read_u32(FindRig::kOut).value(), 0xccccccccu);
            EXPECT_EQ(rig.next(p.next, static_cast<u64>(handle)), -1);
            EXPECT_EQ(rig.mm.get_errno(), 2u);
            EXPECT_EQ(rig.g.call(rig.e("_findclose"), {static_cast<u64>(handle)}), 0u);

            EXPECT_EQ(sizes["a.txt"], 5u);
            EXPECT_EQ(sizes["b_archive.dat"], 70000u);
            EXPECT_EQ(sizes["Long Name File.data"], 3u);
            EXPECT_EQ(writes["a.txt"], 1041379200u);
            ASSERT_TRUE(attribs.count("sub"));
            EXPECT_EQ(attribs["sub"] & 0x10u, 0x10u);
            // _A_NORMAL replaces FILE_ATTRIBUTE_NORMAL; other words pass through.
            FindData data{};
            const u64 h = rig.search.find_first_file("C:\\fprobe\\a.txt", data);
            ASSERT_NE(h, 0u);
            (void)rig.search.find_close(h);
            EXPECT_EQ(attribs["a.txt"], data.attributes == 0x80 ? 0u : data.attributes);
        }
    }
}

TEST(CrtFind, ThirtyTwoBitFieldsFollowTheMeasuredOverflowRules) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        FindRig rig(x64);
        // x86 _findfirst: a 2040 write time is -1 with errno EINVAL, yet the
        // call succeeds; x64's _findfirst has 64-bit times.
        const i64 h = rig.first("_findfirst", "fprobe\\sub\\d_2040.txt", false);
        ASSERT_NE(h, -1);
        const FindLayout L = layout_of("_findfirst", x64);
        if (x64) {
            EXPECT_EQ(rig.mm.get_errno(), 0u);
            EXPECT_EQ(rig.g.memory.read_u64(FindRig::kOut + L.time[2]).value(), 2208988800u);
        } else {
            EXPECT_EQ(rig.mm.get_errno(), 22u);
            EXPECT_EQ(rig.g.memory.read_u32(FindRig::kOut + L.time[2]).value(), 0xFFFFFFFFu);
        }
        EXPECT_EQ(rig.g.call(rig.e("_findclose"), {static_cast<u64>(h)}), 0u);
        // A 5 GiB size in a 32-bit field is truncated without an error.
        const i64 h2 = rig.first("_findfirst", "fprobe\\sub\\e_5g.bin", false);
        ASSERT_NE(h2, -1);
        EXPECT_EQ(rig.mm.get_errno(), 0u);
        EXPECT_EQ(rig.g.memory.read_u32(FindRig::kOut + L.size).value(), 0x40000000u);
        EXPECT_EQ(rig.g.call(rig.e("_findclose"), {static_cast<u64>(h2)}), 0u);
        // _findfirst64 keeps all of it.
        const i64 h3 = rig.first("_findfirst64", "fprobe\\sub\\*.bin", false);
        ASSERT_NE(h3, -1);
        EXPECT_EQ(rig.g.memory.read_u64(FindRig::kOut + 32).value(), 5368709120ull);
        EXPECT_EQ(rig.g.call(rig.e("_findclose"), {static_cast<u64>(h3)}), 0u);
    }
}

TEST(CrtFind, ErrorsMatchWindowsErrnoBothAbis) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        FindRig rig(x64);
        // crt_legacy_exports_probe --errors and crt_findfirst_errors_probe.
        EXPECT_EQ(rig.first("_findfirst", "fprobe\\nothing*.zz", false), -1);
        EXPECT_EQ(rig.mm.get_errno(), 2u);
        EXPECT_EQ(rig.first("_findfirst", "fprobe\\nodir\\*", false), -1);
        EXPECT_EQ(rig.mm.get_errno(), 2u);
        EXPECT_EQ(rig.first("_findfirst", "", false), -1);
        EXPECT_EQ(rig.mm.get_errno(), 2u);
        // A directory named without a wildcard is found itself.
        const i64 h = rig.first("_findfirst", "fprobe", false);
        ASSERT_NE(h, -1);
        EXPECT_EQ(rig.name_at(layout_of("_findfirst", x64).name, false), "fprobe");
        EXPECT_EQ(rig.g.call(rig.e("_findclose"), {static_cast<u64>(h)}), 0u);
        // Bad handles: EINVAL.
        rig.mm.set_errno(0);
        const u64 minus_one = x64 ? ~u64{0} : 0xFFFFFFFFu;
        EXPECT_EQ(rig.g.call(rig.e("_findclose"), {minus_one}), 0xFFFFFFFFu);
        EXPECT_EQ(rig.mm.get_errno(), 22u);
        EXPECT_EQ(rig.next("_findnext", minus_one), -1);
        EXPECT_EQ(rig.mm.get_errno(), 22u);
    }
}

// ---------------------------------------------------------------------------
// _filelength, getc, _putch
// ---------------------------------------------------------------------------

TEST_F(CrtStdioTest, FilelengthIsLongAndRejectsFilesPastLongMaxBothAbis) {
    const auto small = tmp_path("filelength_small");
    const auto large = tmp_path("filelength_large");
    { std::ofstream f(small, std::ios::binary); for (int i = 0; i < 70000; ++i) f.put('x'); }
    { std::ofstream f(large); }
    std::filesystem::resize_file(large, 5368709120ull);
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        OsHandleRig rig(x64, std::filesystem::path(small).parent_path());
        ASSERT_TRUE(rig.m.write_ansi_string(kPathAddr, "C:\\" + std::filesystem::path(small).filename().string()).is_ok());
        const u64 fd = rig.call("_open", {kPathAddr, kO_BINARY});
        ASSERT_NE(static_cast<u32>(fd), ~u32{0});
        EXPECT_EQ(rig.call("_lseek", {fd, 123, 0}), 123u);
        rig.mm.set_errno(0);
        EXPECT_EQ(static_cast<u32>(rig.call("_filelength", {fd})), 70000u);
        EXPECT_EQ(rig.mm.get_errno(), 0u);
        EXPECT_EQ(rig.call("_lseek", {fd, 0, 1}), 123u);  // position unchanged
        // getc is fgetc: the stream reads the byte at the descriptor's start.
        rig.call("_close", {fd});

        ASSERT_TRUE(rig.m.write_ansi_string(kPathAddr, "C:\\" + std::filesystem::path(large).filename().string()).is_ok());
        const u64 big = rig.call("_open", {kPathAddr, kO_BINARY});
        ASSERT_NE(static_cast<u32>(big), ~u32{0});
        EXPECT_EQ(static_cast<u32>(rig.call("_filelength", {big})), 0xFFFFFFFFu);
        EXPECT_EQ(rig.mm.get_errno(), 22u);
        EXPECT_EQ(rig.call("_lseek", {big, 0, 1}), 0u);
        rig.call("_close", {big});

        // msvcrt.dll's own policy: a bad descriptor returns -1/EBADF.
        CrtDllFlavorScope msvcrt(CrtDllFlavor::Msvcrt);
        EXPECT_EQ(static_cast<u32>(rig.call("_filelength", {1234})), 0xFFFFFFFFu);
        EXPECT_EQ(rig.mm.get_errno(), 9u);
    }
    std::filesystem::remove(small);
    std::filesystem::remove(large);
}

TEST_F(CrtStdioTest, GetcReadsLikeFgetcBothAbis) {
    const auto path = tmp_path("getc_alias");
    { std::ofstream f(path, std::ios::binary); f << "Qz"; }
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        OsHandleRig rig(x64, std::filesystem::path(path).parent_path());
        ASSERT_TRUE(rig.m.write_ansi_string(kPathAddr, "C:\\" + std::filesystem::path(path).filename().string()).is_ok());
        ASSERT_TRUE(rig.m.write_ansi_string(kModeAddr, "rb").is_ok());
        const u64 stream = rig.call("fopen", {kPathAddr, kModeAddr});
        ASSERT_NE(stream, 0u);
        EXPECT_EQ(static_cast<u32>(rig.call("getc", {stream})), static_cast<u32>('Q'));
        EXPECT_EQ(static_cast<u32>(rig.call("fgetc", {stream})), static_cast<u32>('z'));
        EXPECT_EQ(static_cast<u32>(rig.call("getc", {stream})), 0xFFFFFFFFu);  // EOF
        rig.call("fclose", {stream});
    }
    std::filesystem::remove(path);
}

TEST_F(CrtStdioTest, PutchReturnsTheLowByteItWrote) {
    // crt_legacy_exports_probe: putch_A 65, putch_141 65, putch_m1 255.
    for (bool x64 : {false, true}) {
        OsHandleRig rig(x64, std::filesystem::temp_directory_path());
        EXPECT_EQ(static_cast<u32>(rig.call("_putch", {'A'})), 65u);
        EXPECT_EQ(static_cast<u32>(rig.call("_putch", {0x141})), 65u);
        EXPECT_EQ(static_cast<u32>(rig.call("_putch", {0xFFFFFFFFu})), 255u);
    }
}

// ---------------------------------------------------------------------------
// _mbsrchr
// ---------------------------------------------------------------------------

TEST(CrtMbsrchr, SingleByteCodePageMatchesWindowsBothAbis) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        Guest g(x64);
        CrtMemoryManager mm(g.memory, 0x40000000, 0x100000);
        capsule::Vfs vfs;
        CrtStringManager strings;
        const DllExports exports = create_crt_string_thunks(strings, mm, vfs);
        const ExportEntry* fn = find_entry(exports, "_mbsrchr");
        ASSERT_NE(fn, nullptr);
        const GuestAddr s = g.bytes(0x12000, "abcabc\xe9x");
        usize rows = 0;
        for (const auto& row : oracle_rows(oracle_file(x64))) {
            if (row.size() < 5 || row[0] != "mbsrchr") continue;
            ++rows;
            const u64 c = std::stoull(row[1], nullptr, 16);
            mm.set_errno(0);
            const u64 got = g.call(fn, {s, c});
            const long long want = std::stoll(row[2]);
            EXPECT_EQ(want < 0 ? 0u : s + static_cast<u64>(want), got) << "c=" << row[1];
            EXPECT_EQ(std::to_string(mm.get_errno()), row[4]);
        }
        EXPECT_EQ(rows, 8u);
    }
}

// ---------------------------------------------------------------------------
// __crtCompareStringA / __crtLCMapStringA
// ---------------------------------------------------------------------------

namespace {

struct CmpCase { u32 lcid; u32 flags; std::string a; i32 na; std::string b; i32 nb; i32 cp; };
struct MapCase { u32 lcid; u32 flags; std::string s; i32 ns; i32 nd; i32 cp; u32 err; };

// The probe's inputs, in row order (crt_legacy_exports_probe.c).
const CmpCase kCmp[] = {
    {0x409, 0, "abc", 3, "abd", 3, 0}, {0x409, 1, "ABC", -1, "abc", -1, 0},
    {0x409, 0, std::string("abc\0x", 5), 5, std::string("abc\0y", 5), 5, 0},
    {0x409, 0, "abc", 3, std::string("abc\0y", 5), 5, 0}, {0x409, 0, "b", 1, "a", 1, 1252},
    {0x409, 0, "a", 1, "ab", 2, 0}, {0x409, 0, "", 0, "", 0, 0},
    {0, 0, "abc", 3, "abd", 3, 0}, {0x12345678, 0, "abc", 3, "abd", 3, 0},
    {0x409, 0, "\xe9", 1, "f", 1, 0}, {0x409, 0, "\xe9", 1, "f", 1, 932},
    {0x409, 0x1000, "a-b", 3, "ab", 2, 0}, {0x409, 0, "a-b", 3, "ab", 2, 0},
    {0x409, 0, "abc", 2, "abd", 2, 0},
};
const MapCase kMap[] = {
    {0x409, 0x200, "abc", 3, 10, 0, 0}, {0x409, 0x200, "abc", 3, 0, 0, 0},
    {0x409, 0x200, "abc", -1, 10, 0, 0}, {0x409, 0x200, std::string("ab\0cd", 5), 5, 10, 0, 0},
    {0x409, 0x400, "abc", 3, 40, 0, 0}, {0x409, 0x400, "abc", 3, 0, 0, 0},
    {0x409, 0x200, "abc", 3, 2, 0, 0}, {0x409, 0x100, "\xc0\xc9Z", 3, 10, 1252, 0},
    {0x409, 0x200, "\xe0\xff", 2, 10, 0, 0}, {0x12345678, 0x200, "abc", 3, 10, 0, 0},
    {0x409, 0x200, "", 0, 10, 0, 0}, {0x409, 0x200, "abc", 3, 10, 0, 1},
    {0x409, 0x400, std::string("ab\0cd", 5), 5, 40, 0, 0}, {0x409, 0x200, "\xe0", 1, 10, 932, 0},
    {0, 0x200, "abc", 3, 10, 0, 0},
};

/// The DBCS code page 932 rows need a DBCS decoder kernel32 does not have
/// yet (codepage::is_valid_code_page(932) is FALSE), so they are skipped.
bool needs_dbcs(i32 cp) { return cp == 932; }

/// LCMAP_SORTKEY bytes are kernel32's LCMapStringW sort keys, which do not
/// yet reproduce Windows' key format; the helper only forwards them, so those
/// rows are kernel32's contract, not this one's.
bool is_sort_key(u32 flags) { return (flags & 0x400) != 0; }

void put_raw(Guest& g, GuestAddr at, const std::string& s) {
    for (usize i = 0; i < s.size(); ++i) (void)g.memory.write_u8(at + i, static_cast<u8>(s[i]));
    (void)g.memory.write_u8(at + s.size(), 0);
}

}  // namespace

TEST(CrtNlsHelpers, CompareStringMatchesWindowsBothSignatures) {
    LocaleStringManager locales;
    const DllExports exports = create_crt_nls_helper_thunks(locales);
    const ExportEntry* fn = find_entry(exports, "__crtCompareStringA");
    ASSERT_NE(fn, nullptr);
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64 (_locale_t first)" : "x86 (VC6)");
        Guest g(x64);
        usize i = 0;
        for (const auto& row : oracle_rows(oracle_file(x64))) {
            if (row.size() < 5 || row[0] != "cmp") continue;
            const CmpCase& c = kCmp[i++];
            if (needs_dbcs(c.cp)) continue;
            SCOPED_TRACE("cmp row " + row[1]);
            put_raw(g, 0x12000, c.a);
            put_raw(g, 0x12100, c.b);
            set_last_error(0xdead);
            const u64 r = x64 ? g.call(fn, {0, c.lcid, c.flags, 0x12000, static_cast<u32>(c.na),
                                            0x12100, static_cast<u32>(c.nb), static_cast<u32>(c.cp)})
                              : g.call(fn, {c.lcid, c.flags, 0x12000, static_cast<u32>(c.na),
                                            0x12100, static_cast<u32>(c.nb), static_cast<u32>(c.cp)});
            EXPECT_EQ(std::to_string(static_cast<i32>(r)), row[2]);
            if (row[2] == "0") {
                EXPECT_EQ(get_last_error(), std::stoul(row[4], nullptr, 16));
            }
        }
        EXPECT_EQ(i, std::size(kCmp));
    }
}

TEST(CrtNlsHelpers, LCMapStringMatchesWindowsBothSignatures) {
    LocaleStringManager locales;
    const DllExports exports = create_crt_nls_helper_thunks(locales);
    const ExportEntry* fn = find_entry(exports, "__crtLCMapStringA");
    ASSERT_NE(fn, nullptr);
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64 (_locale_t first)" : "x86 (VC6)");
        Guest g(x64);
        usize i = 0;
        for (const auto& row : oracle_rows(oracle_file(x64))) {
            if (row.size() < 6 || row[0] != "map") continue;
            const MapCase& c = kMap[i++];
            if (needs_dbcs(c.cp) || is_sort_key(c.flags)) continue;
            SCOPED_TRACE("map row " + row[1]);
            put_raw(g, 0x12000, c.s);
            for (u32 k = 0; k < 64; ++k) (void)g.memory.write_u8(0x12200 + k, 0x7e);
            set_last_error(0xdead);
            const u64 dst = c.nd ? 0x12200 : 0;
            const u64 r = x64 ? g.call(fn, {0, c.lcid, c.flags, 0x12000, static_cast<u32>(c.ns), dst,
                                            static_cast<u32>(c.nd), static_cast<u32>(c.cp), c.err})
                              : g.call(fn, {c.lcid, c.flags, 0x12000, static_cast<u32>(c.ns), dst,
                                            static_cast<u32>(c.nd), static_cast<u32>(c.cp), c.err});
            EXPECT_EQ(std::to_string(static_cast<i32>(r)), row[2]);
            if (row[2] == "0") {
                EXPECT_EQ(get_last_error(), std::stoul(row[4], nullptr, 16));
            }
            const std::vector<u8> want = from_hex(row[5]);
            EXPECT_EQ(g.read(0x12200, want.size()), want);
        }
        EXPECT_EQ(i, std::size(kMap));
    }
}

// ---------------------------------------------------------------------------
// _EH_prolog
// ---------------------------------------------------------------------------

TEST(CrtEhProlog, BuildsTheMeasuredFrameAndReturnsToTheCaller) {
    // msvcrt's body: push -1; push eax; push fs:[0]; mov fs:[0],esp;
    // [esp+0Ch] = ebp; ebp = esp+0Ch; eax = return address; ret
    // (crt_legacy_exports_probe _EH_prolog bytes).
    Guest g(false);
    CrtMemoryManager mm(g.memory, 0x40000000, 0x100000);
    constexpr GuestAddr teb = 0x11000;
    constexpr GuestAddr entry_sp = 0x2e000;
    constexpr u32 ret = 0x00401234;
    constexpr u32 handler = 0x00405678;
    constexpr u32 old_ebp = 0x0012ff80;
    constexpr u32 old_chain = 0x0012ffb0;
    ASSERT_TRUE(g.memory.write_u32(teb, old_chain).is_ok());
    ASSERT_TRUE(g.memory.write_u32(entry_sp, ret).is_ok());
    g.reset();
    g.cpu.seg.fs_desc.base = teb;
    g.cpu.gpr.rsp = entry_sp;
    g.cpu.gpr.rax = handler;
    g.cpu.gpr.rbp = old_ebp;
    Marshaler m(g.memory);
    m.attach_cpu(g.cpu);
    const ExportEntry* fn = nullptr;
    const DllExports exports = create_crt_memory_thunks(mm);
    fn = find_entry(exports, "_EH_prolog");
    ASSERT_NE(fn, nullptr);
    fn->handler(m);
    EXPECT_TRUE(g.cpu.thunk_control_transfer);
    EXPECT_EQ(g.cpu.rip, ret);
    EXPECT_EQ(g.cpu.gpr.rax, ret);
    EXPECT_EQ(g.cpu.gpr.rsp, entry_sp - 12);
    EXPECT_EQ(g.cpu.gpr.rbp, entry_sp);
    EXPECT_EQ(g.memory.read_u32(teb).value(), entry_sp - 12);
    EXPECT_EQ(g.memory.read_u32(entry_sp - 12).value(), old_chain);
    EXPECT_EQ(g.memory.read_u32(entry_sp - 8).value(), handler);
    EXPECT_EQ(g.memory.read_u32(entry_sp - 4).value(), 0xFFFFFFFFu);
    EXPECT_EQ(g.memory.read_u32(entry_sp).value(), old_ebp);
}
