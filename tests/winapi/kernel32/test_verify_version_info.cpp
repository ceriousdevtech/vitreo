/// @file test_verify_version_info.cpp
/// @brief VerifyVersionInfoW/A, RtlVerifyVersionInfo and VerSetConditionMask
///        against Windows' own output.
///
/// Replays tools/oracle_probe/verify_version_info_probe.c case for case
/// through Vitreo's thunks and compares every line with the recordings in
/// tests/oracle_data/verify_version_info_probe{,.x86}{,.manifest}.expected
/// (x64/x86, without and with a Windows 10 manifest). The cases are built
/// relative to the version the thunks report, as the probe does, so the
/// recordings hold no machine version numbers.
///
/// The CLR's IsOS(OS_NT) asks VerifyVersionInfoW whether the platform is
/// VER_PLATFORM_WIN32_WINDOWS; the old always-TRUE stub made the x86 .NET 4
/// fusion believe it ran on Windows 9x, skip GetNativeSystemInfo and refuse
/// every AMD64 assembly with ERROR_BAD_FORMAT.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>
#include <winapi/kernel32/system/sysinfo.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/system/ntdll/ntdll_ext.hpp>
#include <winapi/system/ntdll/ntdll_registration_parts.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <capsule/vfs/vfs.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kStack = 0x80000;
constexpr GuestAddr kInfoW = 0x20000;
constexpr GuestAddr kInfoA = 0x21000;
constexpr u32 kSizeW = 284;  // sizeof(OSVERSIONINFOEXW)
constexpr u32 kSizeA = 156;  // sizeof(OSVERSIONINFOEXA)
constexpr u32 kSizeShortW = 276;
constexpr u32 kSizeShortA = 148;

constexpr u32 VER_MINORVERSION = 0x1, VER_MAJORVERSION = 0x2, VER_BUILDNUMBER = 0x4,
              VER_PLATFORMID = 0x8, VER_SERVICEPACKMINOR = 0x10, VER_SERVICEPACKMAJOR = 0x20,
              VER_SUITENAME = 0x40, VER_PRODUCT_TYPE = 0x80;
constexpr u8 VER_EQUAL = 1, VER_GREATER = 2, VER_GREATER_EQUAL = 3, VER_LESS = 4,
             VER_LESS_EQUAL = 5, VER_AND = 6, VER_OR = 7;

struct Ver {
    u32 major, minor, build;
    u16 spmaj, spmin, suite;
    u8 product;
};

class VerifyVersionInfoOracle
    : public ::testing::TestWithParam<std::tuple<bool, bool>> {
protected:
    void SetUp() override {
        is64 = std::get<0>(GetParam());
        manifested = std::get<1>(GetParam());
        ASSERT_TRUE(memory.map_region(0x10000, 0x80000, MemoryProtection::ReadWrite).is_ok());
        k32 = std::make_unique<Kernel32ExtManager>(memory, vfs);
        sysinfo.set_windows_10_compatible(manifested);
        k32->set_sysinfo_manager(&sysinfo);
        u16 ordinal = 1;
        append_rtl_unwind_version_exports(ntdll, ntdll_exports, ordinal);
        ordinal = 1;
        append_loader_function_table_exports(ntdll, ntdll_exports, ordinal);
        sysinfo_exports = create_sysinfo_thunks(sysinfo);
        std::string path = std::string(VITREO_ORACLE_PROBE_DIR) + "/verify_version_info_probe" +
                           (manifested ? ".manifest" : "") + (is64 ? "" : ".x86") + ".expected";
        std::ifstream in(path);
        ASSERT_TRUE(in.good()) << path;
        for (std::string line; std::getline(in, line);) expected.push_back(line);
    }

    const ExportEntry& find(const DllExports& e, const char* name) {
        auto it = std::find_if(e.entries.begin(), e.entries.end(),
                               [&](const ExportEntry& x) { return x.name == name; });
        EXPECT_NE(it, e.entries.end()) << name;
        return *it;
    }

    // Calls a handler with Windows' calling convention for this bitness.
    // `wide64` lists which arguments are 64-bit (two stack slots on x86).
    u64 call(const std::function<void(Marshaler&)>& handler, std::vector<u64> args,
             std::vector<bool> wide64 = {}) {
        cpu::CpuState cpu;
        if (is64) {
            cpu.init_long_mode();
            cpu.gpr.rsp = kStack;
            u64* regs[4] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
            for (usize i = 0; i < args.size() && i < 4; ++i) *regs[i] = args[i];
        } else {
            cpu.init_protected_mode();
            cpu.set_sp(kStack);
            (void)memory.write_u32(kStack, 0xDEADBEEF);
            GuestAddr at = kStack + 4;
            for (usize i = 0; i < args.size(); ++i) {
                (void)memory.write_u32(at, static_cast<u32>(args[i]));
                at += 4;
                if (i < wide64.size() && wide64[i]) {
                    (void)memory.write_u32(at, static_cast<u32>(args[i] >> 32));
                    at += 4;
                }
            }
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        handler(m);
        if (is64) return cpu.gpr.rax;
        return (static_cast<u64>(static_cast<u32>(cpu.gpr.rdx)) << 32) |
               static_cast<u32>(cpu.gpr.rax);
    }

    u64 vscm(u64 mask, u32 type, u32 cond) {
        return call([this](Marshaler& m) { k32->impl_VerSetConditionMask(m); },
                    {mask, type, cond}, {true});
    }
    u64 c1(u32 t, u8 c) { return vscm(0, t, c); }
    u64 c2(u32 t1, u8 k1, u32 t2, u8 k2) { return vscm(vscm(0, t1, k1), t2, k2); }

    void write_info(const Ver& v, u32 platform, u32 size_w, u32 size_a) {
        std::vector<u8> zero(kSizeW, 0);
        ASSERT_TRUE(memory.write_bytes(kInfoW, zero.data(), kSizeW).is_ok());
        ASSERT_TRUE(memory.write_bytes(kInfoA, zero.data(), kSizeA).is_ok());
        for (auto [base, size, sp] : {std::tuple{kInfoW, size_w, 276u}, std::tuple{kInfoA, size_a, 148u}}) {
            (void)memory.write_u32(base, size);
            (void)memory.write_u32(base + 4, v.major);
            (void)memory.write_u32(base + 8, v.minor);
            (void)memory.write_u32(base + 12, v.build);
            (void)memory.write_u32(base + 16, platform);
            (void)memory.write_u16(base + sp, v.spmaj);
            (void)memory.write_u16(base + sp + 2, v.spmin);
            (void)memory.write_u16(base + sp + 4, v.suite);
            (void)memory.write_u8(base + sp + 6, v.product);
        }
    }

    void run(const std::string& name, u32 type_mask, u64 cond, const Ver& v, u32 platform,
             u32 size_w, u32 size_a) {
        write_info(v, platform, size_w, size_a);
        set_last_error(0xDEAD);
        const u32 rw = static_cast<u32>(
            call([this](Marshaler& m) { k32->impl_VerifyVersionInfoW(m); },
                 {kInfoW, type_mask, cond}, {false, false, true}));
        const u32 ew = get_last_error();
        set_last_error(0xDEAD);
        const u32 ra = static_cast<u32>(
            call([this](Marshaler& m) { k32->impl_VerifyVersionInfoA(m); },
                 {kInfoA, type_mask, cond}, {false, false, true}));
        const u32 ea = get_last_error();
        set_last_error(0xDEAD);
        const u32 st = static_cast<u32>(call(find(ntdll_exports, "RtlVerifyVersionInfo").handler,
                                             {kInfoW, type_mask, cond}, {false, false, true}));
        const u32 er = get_last_error();
        char line[256];
        std::snprintf(line, sizeof line, "%-34s W=%d/%u A=%d/%u Rtl=0x%08x/%u", name.c_str(),
                      rw != 0, ew, ra != 0, ea, st, er);
        out.emplace_back(line);
    }

    void print(const char* fmt, unsigned long long v) {
        char line[256];
        std::snprintf(line, sizeof line, fmt, v);
        out.emplace_back(line);
    }

    void generate();

    bool is64 = false;
    bool manifested = false;
    GuestMemory memory;
    capsule::Vfs vfs;
    SysInfoManager sysinfo;
    NtdllExtManager ntdll;
    std::unique_ptr<Kernel32ExtManager> k32;
    DllExports ntdll_exports;
    DllExports sysinfo_exports;
    std::vector<std::string> expected;
    std::vector<std::string> out;
};

void VerifyVersionInfoOracle::generate() {
    // Current versions as the probe reads them: RtlGetVersion (real) and
    // GetVersionExW (what this process is told), both OSVERSIONINFOEXW.
    auto read_ver = [&](GuestAddr p) {
        return Ver{memory.read_u32(p + 4).value_or(0), memory.read_u32(p + 8).value_or(0),
                   memory.read_u32(p + 12).value_or(0), memory.read_u16(p + 276).value_or(0),
                   memory.read_u16(p + 278).value_or(0), memory.read_u16(p + 280).value_or(0),
                   memory.read_u8(p + 282).value_or(0)};
    };
    std::vector<u8> zero(kSizeW, 0);
    (void)memory.write_bytes(kInfoW, zero.data(), kSizeW);
    (void)memory.write_u32(kInfoW, kSizeW);
    call(find(ntdll_exports, "RtlGetVersion").handler, {kInfoW});
    const Ver real = read_ver(kInfoW);
    (void)memory.write_bytes(kInfoW, zero.data(), kSizeW);
    (void)memory.write_u32(kInfoW, kSizeW);
    call(find(sysinfo_exports, "GetVersionExW").handler, {kInfoW});
    const Ver lie = read_ver(kInfoW);
    char head[200];
    std::snprintf(head, sizeof head,
                  "lie_is_real=%d real_sp=%u.%u real_suite=0x%x real_product=%u lie_suite=0x%x lie_product=%u",
                  lie.major == real.major && lie.minor == real.minor && lie.build == real.build,
                  real.spmaj, real.spmin, real.suite, real.product, lie.suite, lie.product);
    out.emplace_back(head);

    static const char* cname[] = {"0", "EQ", "G", "GE", "L", "LE", "AND", "OR"};
    for (int b = 0; b < 2; ++b) {
        const Ver cur = b ? real : lie;
        const std::string bn = b ? "real" : "lie";
        const u32 types[] = {VER_MAJORVERSION, VER_MINORVERSION, VER_BUILDNUMBER, VER_SERVICEPACKMAJOR,
                             VER_SERVICEPACKMINOR};
        const char* tname[] = {"maj", "min", "bld", "spmaj", "spmin"};
        for (int t = 0; t < 5; ++t) {
            for (int c = 1; c <= 5; ++c) {
                for (int d = -1; d <= 1; ++d) {
                    Ver v = cur;
                    switch (t) {
                    case 0: v.major += d; break;
                    case 1: v.minor += d; break;
                    case 2: v.build += d; break;
                    case 3: v.spmaj = static_cast<u16>(v.spmaj + d); break;
                    case 4: v.spmin = static_cast<u16>(v.spmin + d); break;
                    }
                    char name[96];
                    std::snprintf(name, sizeof name, "%s %s %s%+d", bn.c_str(), tname[t], cname[c], d);
                    run(name, types[t], c1(types[t], static_cast<u8>(c)), v, 2, kSizeW, kSizeA);
                }
            }
        }
        struct H { const char* n; int dmaj, dmin, dspmaj, dspmin; u8 kmaj, kmin, kspmaj, kspmin; };
        const H h[] = {
            {"maj GE-1 min GE+5", -1, 5, 0, 0, VER_GREATER_EQUAL, VER_GREATER_EQUAL, 0, 0},
            {"maj LE0 min GE+1", 0, 1, 0, 0, VER_LESS_EQUAL, VER_GREATER_EQUAL, 0, 0},
            {"maj EQ0 min G0", 0, 0, 0, 0, VER_EQUAL, VER_GREATER, 0, 0},
            {"maj EQ0 min GE0", 0, 0, 0, 0, VER_EQUAL, VER_GREATER_EQUAL, 0, 0},
            {"maj EQ-1 min GE0", -1, 0, 0, 0, VER_EQUAL, VER_GREATER_EQUAL, 0, 0},
            {"maj L+1 min GE+5", 1, 5, 0, 0, VER_LESS, VER_GREATER_EQUAL, 0, 0},
            {"maj G-1 min L-1", -1, -1, 0, 0, VER_GREATER, VER_LESS, 0, 0},
            {"maj GE0 min LE-1", 0, -1, 0, 0, VER_GREATER_EQUAL, VER_LESS_EQUAL, 0, 0},
            {"maj GE0 min L0", 0, 0, 0, 0, VER_GREATER_EQUAL, VER_LESS, 0, 0},
            {"maj GE0 min EQ+1", 0, 1, 0, 0, VER_GREATER_EQUAL, VER_EQUAL, 0, 0},
            {"maj G0 min GE0", 0, 0, 0, 0, VER_GREATER, VER_GREATER_EQUAL, 0, 0},
            {"maj LE0 min G0", 0, 0, 0, 0, VER_LESS_EQUAL, VER_GREATER, 0, 0},
            {"maj GE0 spmaj GE+1", 0, 0, 1, 0, VER_GREATER_EQUAL, 0, VER_GREATER_EQUAL, 0},
            {"maj GE-1 spmaj GE+1", -1, 0, 1, 0, VER_GREATER_EQUAL, 0, VER_GREATER_EQUAL, 0},
            {"maj GE0 min GE0 spmaj GE0 spmin GE+1", 0, 0, 0, 1, VER_GREATER_EQUAL, VER_GREATER_EQUAL,
             VER_GREATER_EQUAL, VER_GREATER_EQUAL},
            {"maj GE0 min GE0 spmaj G-1 spmin GE+1", 0, 0, -1, 1, VER_GREATER_EQUAL, VER_GREATER_EQUAL,
             VER_GREATER, VER_GREATER_EQUAL},
            {"min GE+1 spmaj GE0", 0, 1, 0, 0, 0, VER_GREATER_EQUAL, VER_GREATER_EQUAL, 0},
            {"min LE0 spmaj GE+1", 0, 0, 1, 0, 0, VER_LESS_EQUAL, VER_GREATER_EQUAL, 0},
            {"min L+1 spmaj GE+1", 0, 1, 1, 0, 0, VER_LESS, VER_GREATER_EQUAL, 0},
            {"spmaj GE0 spmin G0", 0, 0, 0, 0, 0, 0, VER_GREATER_EQUAL, VER_GREATER},
            {"spmaj LE0 spmin GE+1", 0, 0, 0, 1, 0, 0, VER_LESS_EQUAL, VER_GREATER_EQUAL},
            {"maj EQ0 min EQ0 spmaj EQ0 spmin EQ0", 0, 0, 0, 0, VER_EQUAL, VER_EQUAL, VER_EQUAL, VER_EQUAL},
            {"maj GE0 min EQ-1", 0, -1, 0, 0, VER_GREATER_EQUAL, VER_EQUAL, 0, 0},
            {"maj EQ0 min L0", 0, 0, 0, 0, VER_EQUAL, VER_LESS, 0, 0},
        };
        for (const H& x : h) {
            Ver v = cur;
            v.major += x.dmaj;
            v.minor += x.dmin;
            v.spmaj = static_cast<u16>(v.spmaj + x.dspmaj);
            v.spmin = static_cast<u16>(v.spmin + x.dspmin);
            u64 cond = 0;
            u32 mask = 0;
            if (x.kmaj) { cond = vscm(cond, VER_MAJORVERSION, x.kmaj); mask |= VER_MAJORVERSION; }
            if (x.kmin) { cond = vscm(cond, VER_MINORVERSION, x.kmin); mask |= VER_MINORVERSION; }
            if (x.kspmaj) { cond = vscm(cond, VER_SERVICEPACKMAJOR, x.kspmaj); mask |= VER_SERVICEPACKMAJOR; }
            if (x.kspmin) { cond = vscm(cond, VER_SERVICEPACKMINOR, x.kspmin); mask |= VER_SERVICEPACKMINOR; }
            run(bn + " " + x.n, mask, cond, v, 2, kSizeW, kSizeA);
        }
        Ver v = cur;
        v.major -= 1;
        v.build += 1;
        run(bn + " maj GE-1 bld GE+1", VER_MAJORVERSION | VER_BUILDNUMBER,
            c2(VER_MAJORVERSION, VER_GREATER_EQUAL, VER_BUILDNUMBER, VER_GREATER_EQUAL), v, 2, kSizeW, kSizeA);
        v = cur;
        v.major += 1;
        v.build -= 1;
        run(bn + " maj GE+1 bld GE-1", VER_MAJORVERSION | VER_BUILDNUMBER,
            c2(VER_MAJORVERSION, VER_GREATER_EQUAL, VER_BUILDNUMBER, VER_GREATER_EQUAL), v, 2, kSizeW, kSizeA);
    }
    const Ver cur{real.major, real.minor, real.build, real.spmaj, real.spmin, lie.suite, lie.product};
    char name[96];
    for (u32 p = 0; p <= 3; ++p) {
        for (int c = 1; c <= 5; ++c) {
            std::snprintf(name, sizeof name, "plat %s %u", cname[c], p);
            run(name, VER_PLATFORMID, c1(VER_PLATFORMID, static_cast<u8>(c)), cur, p, kSizeW, kSizeA);
        }
    }
    {
        const u16 s = lie.suite;
        struct S { const char* n; u16 v; u8 k; };
        const S st[] = {
            {"suite AND cur", s, VER_AND},
            {"suite AND cur|0x200", static_cast<u16>(s | 0x200), VER_AND},
            {"suite OR cur|0x200", static_cast<u16>(s | 0x200), VER_OR},
            {"suite OR 0x200", 0x200, VER_OR},
            {"suite AND 0", 0, VER_AND},
            {"suite OR 0", 0, VER_OR},
            {"suite EQ cur", s, VER_EQUAL},
            {"suite GE cur", s, VER_GREATER_EQUAL},
            {"suite AND 0x4000", 0x4000, VER_AND},
            {"suite OR 0x4000|cur", static_cast<u16>(0x4000 | s), VER_OR},
        };
        for (const S& x : st) {
            Ver v = cur;
            v.suite = x.v;
            run(x.n, VER_SUITENAME, c1(VER_SUITENAME, x.k), v, 2, kSizeW, kSizeA);
        }
    }
    for (u8 p = 1; p <= 3; ++p) {
        for (int c = 1; c <= 5; ++c) {
            Ver v = cur;
            v.product = p;
            std::snprintf(name, sizeof name, "product %s %u%s", cname[c], p, p == lie.product ? "(cur)" : "");
            run(name, VER_PRODUCT_TYPE, c1(VER_PRODUCT_TYPE, static_cast<u8>(c)), v, 2, kSizeW, kSizeA);
        }
    }
    run("err type_mask 0", 0, c1(VER_MAJORVERSION, VER_EQUAL), cur, 2, kSizeW, kSizeA);
    run("err cond_mask 0", VER_MAJORVERSION, 0, cur, 2, kSizeW, kSizeA);
    run("err maj+min only maj cond", VER_MAJORVERSION | VER_MINORVERSION, c1(VER_MAJORVERSION, VER_EQUAL), cur, 2,
        kSizeW, kSizeA);
    run("err maj cond 6", VER_MAJORVERSION, c1(VER_MAJORVERSION, 6), cur, 2, kSizeW, kSizeA);
    run("err maj cond 7", VER_MAJORVERSION, c1(VER_MAJORVERSION, 7), cur, 2, kSizeW, kSizeA);
    run("err suite cond EQ+bad", VER_SUITENAME, c1(VER_SUITENAME, 5), cur, 2, kSizeW, kSizeA);
    run("err unknown type 0x100", 0x100, vscm(0, 0x100, VER_EQUAL), cur, 2, kSizeW, kSizeA);
    run("err maj + unknown 0x100", VER_MAJORVERSION | 0x100, vscm(c1(VER_MAJORVERSION, VER_EQUAL), 0x100, VER_EQUAL),
        cur, 2, kSizeW, kSizeA);
    run("err maj cond in high bits only", VER_MAJORVERSION, u64{VER_EQUAL} << 60, cur, 2, kSizeW, kSizeA);
    run("size 0", VER_MAJORVERSION, c1(VER_MAJORVERSION, VER_EQUAL), cur, 2, 0, 0);
    run("size short", VER_MAJORVERSION, c1(VER_MAJORVERSION, VER_EQUAL), cur, 2, kSizeShortW, kSizeShortA);
    run("size 0 maj LE0", VER_MAJORVERSION, c1(VER_MAJORVERSION, VER_LESS_EQUAL), cur, 2, 0, 0);
    {
        struct R { const char* n; u32 type; u8 k; u32 val; };
        const R r[] = {
            {"range maj GE 0x80000000", VER_MAJORVERSION, VER_GREATER_EQUAL, 0x80000000u},
            {"range maj L 0x80000000", VER_MAJORVERSION, VER_LESS, 0x80000000u},
            {"range maj GE 0x7fffffff", VER_MAJORVERSION, VER_GREATER_EQUAL, 0x7fffffffu},
            {"range maj GE 0x8000000a", VER_MAJORVERSION, VER_GREATER_EQUAL, 0x8000000au},
            {"range maj GE 0xffffffff", VER_MAJORVERSION, VER_GREATER_EQUAL, 0xffffffffu},
            {"range min GE 0x80000000", VER_MINORVERSION, VER_GREATER_EQUAL, 0x80000000u},
            {"range min LE 0x7fffffff", VER_MINORVERSION, VER_LESS_EQUAL, 0x7fffffffu},
            {"range bld GE 0xffffffff", VER_BUILDNUMBER, VER_GREATER_EQUAL, 0xffffffffu},
            {"range bld GE 0x80000000", VER_BUILDNUMBER, VER_GREATER_EQUAL, 0x80000000u},
            {"range bld L 0x80000000", VER_BUILDNUMBER, VER_LESS, 0x80000000u},
            {"range plat GE 0xffffffff", VER_PLATFORMID, VER_GREATER_EQUAL, 0xffffffffu},
            {"range plat L 0x80000000", VER_PLATFORMID, VER_LESS, 0x80000000u},
        };
        for (const R& x : r) {
            Ver v = cur;
            u32 plat = 2;
            if (x.type == VER_MAJORVERSION) v.major = x.val;
            if (x.type == VER_MINORVERSION) v.minor = x.val;
            if (x.type == VER_BUILDNUMBER) v.build = x.val;
            if (x.type == VER_PLATFORMID) plat = x.val;
            run(x.n, x.type, c1(x.type, x.k), v, plat, kSizeW, kSizeA);
        }
    }
    {
        const u32 ft[] = {VER_MAJORVERSION, VER_MINORVERSION, VER_BUILDNUMBER, VER_PLATFORMID,
                          VER_SERVICEPACKMAJOR, VER_SERVICEPACKMINOR, VER_PRODUCT_TYPE};
        const int fb[] = {1, 0, 2, 3, 5, 4, 7};
        const char* fn[] = {"maj", "min", "bld", "plat", "spmaj", "spmin", "product"};
        const u8 bad[] = {0, 6, 7};
        for (int f = 0; f < 7; ++f) {
            for (u8 k : bad) {
                for (int d = -1; d <= 1; ++d) {
                    Ver v = cur;
                    u32 plat = 2;
                    switch (f) {
                    case 0: v.major += d; break;
                    case 1: v.minor += d; break;
                    case 2: v.build += d; break;
                    case 3: plat += d; break;
                    case 4: v.spmaj = static_cast<u16>(v.spmaj + d); break;
                    case 5: v.spmin = static_cast<u16>(v.spmin + d); break;
                    case 6: v.product = static_cast<u8>(v.product + d); break;
                    }
                    const u64 cond = (u64{k} << (fb[f] * 3)) | (u64{1} << 63);
                    std::snprintf(name, sizeof name, "badcond %s %u %+d", fn[f], k, d);
                    run(name, ft[f], cond, v, plat, kSizeW, kSizeA);
                }
            }
        }
        const u8 sk[] = {0, 1, 3, 5};
        for (u8 k : sk) {
            for (int z = 0; z < 2; ++z) {
                Ver v = cur;
                v.suite = z ? static_cast<u16>(cur.suite | 0x200) : 0;
                const u64 cond = (u64{k} << 18) | (u64{1} << 63);
                std::snprintf(name, sizeof name, "badcond suite %u req=%s", k, z ? "cur|0x200" : "0");
                run(name, VER_SUITENAME, cond, v, 2, kSizeW, kSizeA);
            }
        }
    }
    {
        struct G { const char* n; int dmaj, dmin, dspmaj, dspmin; u8 kmaj, kmin, kspmaj, kspmin; u32 mask; };
        const u32 MM = VER_MAJORVERSION | VER_MINORVERSION;
        const G g[] = {
            {"grp maj G0 min L+1", 0, 1, 0, 0, VER_GREATER, VER_LESS, 0, 0, MM},
            {"grp maj EQ0 min GE-1", 0, -1, 0, 0, VER_EQUAL, VER_GREATER_EQUAL, 0, 0, MM},
            {"grp maj 0eq min GE-1", 0, -1, 0, 0, 0, VER_GREATER_EQUAL, 0, 0, MM},
            {"grp maj 0eq min GE+1", 0, 1, 0, 0, 0, VER_GREATER_EQUAL, 0, 0, MM},
            {"grp maj 6eq min GE-1", 0, -1, 0, 0, 6, VER_GREATER_EQUAL, 0, 0, MM},
            {"grp maj GE0 min 0+1", 0, 1, 0, 0, VER_GREATER_EQUAL, 0, 0, 0, MM},
            {"grp maj GE0 min 0-1", 0, -1, 0, 0, VER_GREATER_EQUAL, 0, 0, 0, MM},
            {"grp maj L0 spmin G0", 0, 0, 0, 0, VER_LESS, 0, 0, VER_GREATER, MM | VER_SERVICEPACKMINOR},
            {"grp maj G0 spmin L0", 0, 0, 0, 0, VER_GREATER, 0, 0, VER_LESS, MM | VER_SERVICEPACKMINOR},
            {"grp maj G0 spmin EQ0", 0, 0, 0, 0, VER_GREATER, 0, 0, VER_EQUAL, MM | VER_SERVICEPACKMINOR},
            {"grp maj EQ0 spmin G0", 0, 0, 0, 0, VER_EQUAL, 0, 0, VER_GREATER, MM | VER_SERVICEPACKMINOR},
            {"grp spmaj G0", 0, 0, 0, 0, 0, 0, VER_GREATER, 0, VER_SERVICEPACKMAJOR},
            {"grp spmaj G0 spmin GE0", 0, 0, 0, 0, 0, 0, VER_GREATER, VER_GREATER_EQUAL,
             VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR},
            {"grp spmaj GE0 spmin L0", 0, 0, 0, 0, 0, 0, VER_GREATER_EQUAL, VER_LESS,
             VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR},
            {"grp spmaj GE0 spmin LE-1", 0, 0, 0, -1, 0, 0, VER_GREATER_EQUAL, VER_LESS_EQUAL,
             VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR},
            {"grp min G0 spmin LE+1", 0, 0, 0, 1, 0, VER_GREATER, 0, VER_LESS_EQUAL,
             VER_MINORVERSION | VER_SERVICEPACKMINOR},
            {"grp maj 0+1 min GE0", 1, 0, 0, 0, 0, VER_GREATER_EQUAL, 0, 0, MM},
            {"grp maj 0-1 min GE0", -1, 0, 0, 0, 0, VER_GREATER_EQUAL, 0, 0, MM},
        };
        for (const G& x : g) {
            Ver v = cur;
            v.major += x.dmaj;
            v.minor += x.dmin;
            v.spmaj = static_cast<u16>(v.spmaj + x.dspmaj);
            v.spmin = static_cast<u16>(v.spmin + x.dspmin);
            const u64 cond = (u64{1} << 63) | (u64{x.kmaj} << 3) | u64{x.kmin} | (u64{x.kspmaj} << 15) |
                             (u64{x.kspmin} << 12);
            run(x.n, x.mask, cond, v, 2, kSizeW, kSizeA);
        }
    }
    {
        Ver v = cur;
        v.major += 1;
        run("order maj EQ+1 suite EQ", VER_MAJORVERSION | VER_SUITENAME,
            vscm(0, VER_MAJORVERSION, VER_EQUAL) | (u64{VER_EQUAL} << 18), v, 2, kSizeW, kSizeA);
        run("order plat EQ1 suite EQ", VER_PLATFORMID | VER_SUITENAME,
            vscm(0, VER_PLATFORMID, VER_EQUAL) | (u64{VER_EQUAL} << 18), cur, 1, kSizeW, kSizeA);
        v = cur;
        v.build += 1;
        run("order bld EQ+1 suite EQ", VER_BUILDNUMBER | VER_SUITENAME,
            vscm(0, VER_BUILDNUMBER, VER_EQUAL) | (u64{VER_EQUAL} << 18), v, 2, kSizeW, kSizeA);
        v = cur;
        v.product = 3;
        run("order product EQ3 suite EQ", VER_PRODUCT_TYPE | VER_SUITENAME,
            vscm(0, VER_PRODUCT_TYPE, VER_EQUAL) | (u64{VER_EQUAL} << 18), v, 2, kSizeW, kSizeA);
        run("cond bit63 clear maj EQ", VER_MAJORVERSION, u64{VER_EQUAL} << 3, cur, 2, kSizeW, kSizeA);
        v = cur;
        v.major += 1;
        run("cond bit63 clear maj EQ+1", VER_MAJORVERSION, u64{VER_EQUAL} << 3, v, 2, kSizeW, kSizeA);
    }
    {
        const u32 pairs[][2] = {{1, 2}, {2, 4}, {4, 8}, {8, 0x10}, {0x10, 0x20}, {0x20, 0x40}, {0x40, 0x80},
                                {1, 0x80}, {0x80, 0x100}, {0x3, 0}, {0xff, 0}};
        for (const auto& p : pairs) {
            const u32 t = p[0] | p[1];
            char line[96];
            std::snprintf(line, sizeof line, "VSCM type 0x%x GE = 0x%llx", t,
                          static_cast<unsigned long long>(vscm(0, t, VER_GREATER_EQUAL)));
            out.emplace_back(line);
        }
        for (u32 b = 0; b < 32; ++b) {
            char line[96];
            std::snprintf(line, sizeof line, "VSCM bit %u EQ = 0x%llx", b,
                          static_cast<unsigned long long>(vscm(0, 1u << b, VER_EQUAL)));
            out.emplace_back(line);
        }
        print("VSCM chain maj EQ then maj GE = 0x%llx", vscm(c1(VER_MAJORVERSION, VER_EQUAL), VER_MAJORVERSION,
                                                             VER_GREATER_EQUAL));
        print("VSCM keep 0x1234 maj EQ = 0x%llx", vscm(0x1234, VER_MAJORVERSION, VER_EQUAL));
        print("VSCM keep 0x1234 type0 cond0 = 0x%llx", vscm(0x1234, 0, 0));
        print("VSCM keep 0x1234 maj cond0 = 0x%llx", vscm(0x1234, VER_MAJORVERSION, 0));
        print("VSCM keep 0x1234 type0 EQ = 0x%llx", vscm(0x1234, 0, VER_EQUAL));
        print("VSCM keep 0x0 maj cond0 = 0x%llx", vscm(0, VER_MAJORVERSION, 0));
        print("VSCM maj cond 0xff = 0x%llx", c1(VER_MAJORVERSION, 0xff));
        print("VSCM maj cond 0x0e = 0x%llx", c1(VER_MAJORVERSION, 0x0e));
    }
    print("VerSetConditionMask maj EQ = 0x%llx", c1(VER_MAJORVERSION, VER_EQUAL));
    print("VerSetConditionMask maj|min GE = 0x%llx", vscm(0, VER_MAJORVERSION | VER_MINORVERSION, VER_GREATER_EQUAL));
    print("VerSetConditionMask suite AND, product LE = 0x%llx",
          c2(VER_SUITENAME, VER_AND, VER_PRODUCT_TYPE, VER_LESS_EQUAL));
    print("VerSetConditionMask 0x100 EQ = 0x%llx", vscm(0, 0x100, VER_EQUAL));
    print("VerSetConditionMask maj cond 9 = 0x%llx", c1(VER_MAJORVERSION, 9));
    print("VerSetConditionMask type 0 = 0x%llx", vscm(0x1234, 0, VER_EQUAL));
    print("VerSetConditionMask cond 0 = 0x%llx", vscm(c1(VER_MAJORVERSION, VER_EQUAL), VER_MAJORVERSION, 0));
}

TEST_P(VerifyVersionInfoOracle, MatchesWindowsLineForLine) {
    generate();
    ASSERT_EQ(out.size(), expected.size());
    for (usize i = 0; i < out.size(); ++i) {
        EXPECT_EQ(out[i], expected[i]) << "line " << i + 1;
    }
}

INSTANTIATE_TEST_SUITE_P(Bitness, VerifyVersionInfoOracle,
                         ::testing::Combine(::testing::Bool(), ::testing::Bool()),
                         [](const auto& info) {
                             return std::string(std::get<0>(info.param) ? "x64" : "x86") +
                                    (std::get<1>(info.param) ? "_manifest" : "_plain");
                         });

// ntdll's VerSetConditionMask is the same function kernel32 forwards to.
TEST(VerSetConditionMaskNtdll, MatchesKernel32Encoding) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x10000, 0x80000, MemoryProtection::ReadWrite).is_ok());
    NtdllExtManager ntdll;
    DllExports exports;
    u16 ordinal = 1;
    append_rtl_unwind_version_exports(ntdll, exports, ordinal);
    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                           [](const ExportEntry& e) { return e.name == "VerSetConditionMask"; });
    ASSERT_NE(it, exports.entries.end());
    // x64: VSCM bit 22 EQ = 0x8000000000000004 (shift 66 wraps to 2).
    cpu::CpuState cpu64;
    cpu64.init_long_mode();
    cpu64.gpr.rsp = kStack;
    cpu64.gpr.rcx = 0;
    cpu64.gpr.rdx = 1u << 22;
    cpu64.gpr.r8 = VER_EQUAL;
    Marshaler m64(memory);
    m64.attach_cpu(cpu64);
    it->handler(m64);
    EXPECT_EQ(cpu64.gpr.rax, 0x8000000000000004ull);
    // x86: the same request drops the field (0x8000000000000000), and the
    // input mask is ORed, not replaced (keep 0x1234 maj EQ = 0x...123c).
    cpu::CpuState cpu32;
    cpu32.init_protected_mode();
    cpu32.set_sp(kStack);
    (void)memory.write_u32(kStack + 4, 0x1234);
    (void)memory.write_u32(kStack + 8, 0);
    (void)memory.write_u32(kStack + 12, VER_MAJORVERSION);
    (void)memory.write_u32(kStack + 16, VER_EQUAL);
    Marshaler m32(memory);
    m32.attach_cpu(cpu32);
    it->handler(m32);
    EXPECT_EQ(static_cast<u32>(cpu32.gpr.rax), 0x123cu);
    EXPECT_EQ(static_cast<u32>(cpu32.gpr.rdx), 0x80000000u);
    (void)memory.write_u32(kStack + 12, 1u << 22);
    it->handler(m32);
    EXPECT_EQ(static_cast<u32>(cpu32.gpr.rax), 0x1234u);
    EXPECT_EQ(static_cast<u32>(cpu32.gpr.rdx), 0x80000000u);
}

}  // namespace
