// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_vcruntime_interpose_stub.cpp
/// @brief The vcruntime140 interposer: stub encoding, the accel arena split,
///        export-directory identity, the get_proc_address hook, and
///        end-to-end execution of both stub branches through the JIT run
///        loop (in-chain, dispatcher, interpreter) on x86 and x64.
///
/// Expected values come from the C contracts (Learn: memcpy/memmove/memset/
/// memcmp/memchr; tests/oracle_data/crt_memcmp_sign_probe*: memcmp is
/// exactly -1/0/+1; crt_memcpy_overlap_probe*: memcpy is overlap-safe), the
/// calling conventions (Learn __cdecl, x64 calling convention) and the PE
/// format (IMAGE_EXPORT_DIRECTORY.Name), never from a Vitreo run.

#include "vcruntime_interpose_test_support.hpp"

#include "app/cli/commands/dll_registry.hpp"
#include "capsule/vfs/vfs.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "runtime/execution/guest_run_loop.hpp"
#include "runtime/execution/thunk_inchain.hpp"
#include "winapi/core/host_in_chain.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/process/vectored_exception.hpp"
#include "winapi/msvcrt/guest_accel.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/text/crt_string.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace vitreo::winapi {
/// ModuleManager's own friend hook (module.hpp): reaches the companion
/// resolver import binding uses.
class ModuleManagerTestAccessor {
public:
    static pe::ImportCallback companion_resolver(ModuleManager& mm, std::string dir) {
        return mm.make_companion_resolver(std::move(dir), nullptr);
    }
};
}  // namespace vitreo::winapi

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vcrt_test;
using vitreo::cpu::jit::JitCompiler;
namespace vcrt = vitreo::winapi::vcrt;

namespace {

constexpr GuestAddr kThunkBase = 0x70000000;
constexpr usize kThunkSlab = 0x20000;
constexpr GuestAddr kX64Arena = kThunkBase + X64_ACCEL_OFFSET;
constexpr GuestAddr kX86Stubs = CRT_ACCEL_BASE_X86 + CRT_ACCEL_STUB_OFFSET_X86;
constexpr usize kX86StubSlots =
    (CRT_ACCEL_RESUME_OFFSET_X86 - CRT_ACCEL_STUB_OFFSET_X86) / CRT_ACCEL_STUB_SLOT_X86;
// Images: x86 low; x64 one low and one 6 GiB above the accelerator arena
// (out of rel32 reach of it, as every real x64 image is).
constexpr GuestAddr kImg32 = 0x10000000;
constexpr GuestAddr kImg64Near = 0x40000000;
constexpr GuestAddr kImg64Far = kX64Arena + 0x180000000ull;
/// An x64 user address space (find_free_gap searches below it).
constexpr GuestAddr kMaxUser64 = 0x00007FFF'FFFEFFFFull;

/// |a - b| <= the reach of a rel32 branch (with the interposer's margin).
bool within_rel32(GuestAddr a, GuestAddr b) {
    return (a > b ? a - b : b - a) <= 0x7FFF0000ull;
}
// Data: its own range (the JIT's SMC churn record is keyed by address).
constexpr GuestAddr kMemBase = 0x60000;
constexpr usize kMemSize = 0x60000;
constexpr GuestAddr kBuf = 0x70000;        // 0x40000 bytes of buffers
constexpr GuestAddr kStackTop32 = 0xBF000;
constexpr GuestAddr kStackTop64 = 0xBF008;

/// Thunk slab with every slot's unmodified NOP sled (the dispatcher and the
/// in-chain map recognise slot addresses by range).
void map_thunk_slab(GuestMemory& memory) {
    ASSERT_TRUE(memory.map_region(kThunkBase, kThunkSlab, MemoryProtection::ReadWrite));
    std::vector<u8> slab(kThunkSlab, 0);
    for (usize slot = 0; slot + ThunkRegistry::STUB_SIZE <= kThunkSlab; slot += 16) {
        std::fill_n(slab.begin() + static_cast<std::ptrdiff_t>(slot),
                    ThunkRegistry::STUB_SIZE, u8{0x90});
    }
    ASSERT_TRUE(memory.write_bytes(kThunkBase, slab.data(), slab.size()));
}

/// One guest process worth of state: memory, a registry holding the CRT
/// string exports (and so the internal __vitreo_vcrt_* entries) as both
/// msvcrt.dll and ucrtbase.dll, and a ModuleManager bound to the memory.
struct World {
    GuestMemory memory;
    capsule::Vfs vfs;
    std::unique_ptr<CrtMemoryManager> crt_mem;
    CrtStringManager strings;
    ThunkRegistry registry{kThunkBase};
    std::unique_ptr<ModuleManager> mm;
    ProcessManager process;

    World() {
        crt_mem = std::make_unique<CrtMemoryManager>(memory);
        EXPECT_TRUE(registry.register_dll("msvcrt.dll",
                                          create_crt_string_thunks(strings, *crt_mem, vfs)));
        EXPECT_TRUE(registry.register_dll("ucrtbase.dll",
                                          create_crt_string_thunks(strings, *crt_mem, vfs)));
        mm = std::make_unique<ModuleManager>(registry);
        mm->bind_guest_memory(memory);
        mm->set_main_module(0x400000, "C:\\app\\test.exe");
        EXPECT_TRUE(memory.map_region(kMemBase, kMemSize, MemoryProtection::ReadWrite));
        memory.set_maximum_user_address(kMaxUser64);
    }

    /// Map @p img at @p base (RX) and register it as @p path.
    void load(const Image& img, GuestAddr base, bool x64, const std::string& path) {
        ASSERT_TRUE(memory.map_region(base, kImageSize, MemoryProtection::ReadWrite));
        ASSERT_TRUE(memory.write_bytes(base, img.bytes.data(), img.bytes.size()));
        ASSERT_TRUE(memory.protect_region(base, kImageSize, MemoryProtection::ReadExecute));
        mm->register_main_module_exports(base, path, base + kImageSize, x64 ? 64 : 32,
                                         std::span<const u8>(img.bytes.data(), img.bytes.size()));
    }

    GuestAddr host_slot(vcrt::Fn fn) {
        return registry.get_thunk_address("ucrtbase.dll", vcrt::internal_export_name(fn), true)
            .value();
    }
};

class VcrtStub : public ::testing::Test {
protected:
    // The baked thresholds interpose only memcmp/memchr (the measured
    // crossovers); the mechanism tests give all five sized functions one,
    // so memcpy/memmove/memset stubs exercise the copy paths too.
    void SetUp() override {
        vcrt::force_for_test(vcrt::Force::On);
        for (usize i = 0; i <= static_cast<usize>(vcrt::Fn::Memchr); ++i) {
            vcrt::set_threshold_for_test(static_cast<vcrt::Fn>(i), 32, 256);
            vcrt::set_threshold_for_test(static_cast<vcrt::Fn>(i), 64, 256);
        }
    }
    void TearDown() override {
        vcrt::force_for_test(vcrt::Force::None);
        for (usize i = 0; i < vcrt::kFnCount; ++i) {
            vcrt::set_threshold_for_test(static_cast<vcrt::Fn>(i), 32, std::nullopt);
            vcrt::set_threshold_for_test(static_cast<vcrt::Fn>(i), 64, std::nullopt);
        }
    }
};

// ---------------------------------------------------------------------------
// Task 3: arena split and stub encodings
// ---------------------------------------------------------------------------

TEST_F(VcrtStub, AccelBlobLeavesTheStubRegionFree) {
    EXPECT_LE(build_guest_crt_code(CRT_ACCEL_BASE_X86, true).code.size(),
              CRT_ACCEL_STUB_OFFSET_X86);
    EXPECT_LE(build_guest_x64_code(kX64Arena).code.size(), X64_ACCEL_RESUME_OFFSET);
}

/// Step the interpreter from @p entry until RIP is one of @p stops.
GuestAddr step_until(cpu::CpuState& cpu, GuestMemory& memory, GuestAddr entry,
                     std::initializer_list<GuestAddr> stops) {
    cpu.rip = entry;
    cpu::Interpreter interp(cpu, memory);
    for (int i = 0; i < 64; ++i) {
        for (GuestAddr s : stops) {
            if (cpu.rip == s) return s;
        }
        const auto r = interp.step();
        if (r != cpu::ExecResult::Continue && r != cpu::ExecResult::Branch) return ~GuestAddr{0};
    }
    return ~GuestAddr{0};
}

TEST_F(VcrtStub, X86StubBranchesOnThirdArg) {
    GuestMemory memory;
    constexpr GuestAddr kAt = 0x100000, kHost = 0x7000ABC0, kOrig = 0x10001000;
    ASSERT_TRUE(memory.map_region(kAt, 0x1000, MemoryProtection::ReadWriteExecute));
    ASSERT_TRUE(memory.map_region(0x200000, 0x1000, MemoryProtection::ReadWrite));
    const auto code = vcrt::encode_stub_x86(kAt, 256, kHost, kOrig);
    ASSERT_EQ(code.size(), CRT_ACCEL_STUB_SLOT_X86);
    ASSERT_TRUE(memory.write_bytes(kAt, code.data(), code.size()));
    for (u32 n : {255u, 256u, 257u, 0xFFFFFFFFu, 0u}) {
        cpu::CpuState cpu;
        cpu.init_protected_mode();
        const GuestAddr sp = 0x200800;
        cpu.set_sp(sp);
        ASSERT_TRUE(memory.write_u32(sp, 0x401234));
        ASSERT_TRUE(memory.write_u32(sp + 12, n));
        for (u8 r = 0; r < 8; ++r) {
            if (r != 4) cpu.gpr.set32(r, 0xA0A0A000u + r);
        }
        const GuestAddr got = step_until(cpu, memory, kAt, {kHost, kOrig});
        EXPECT_EQ(got, n >= 256 ? kHost : kOrig) << n;
        EXPECT_EQ(cpu.sp(), sp);
        for (u8 r = 0; r < 8; ++r) {
            if (r != 4) EXPECT_EQ(cpu.gpr.get32(r), 0xA0A0A000u + r) << int(r);
        }
    }
}

TEST_F(VcrtStub, X64StubHasNoFarForm) {
    // A far guest branch would cost every small call (encode_stub_x64).
    EXPECT_TRUE(vcrt::encode_stub_x64(0x80010000, 256, 0x70001230, 0x80010000 + 0x180000000ull)
                    .empty());
    EXPECT_TRUE(vcrt::encode_stub_x64(0x180010000ull, 256, 0x70001230, 0x80000000).empty());
    EXPECT_FALSE(vcrt::encode_stub_x64(0x80010000, 256, 0x70001230, 0x10000000).empty());
}

TEST_F(VcrtStub, X64StubBranchesOnR8) {
    GuestMemory memory;
    constexpr GuestAddr kAt = 0x80010000, kHost = 0x7FFA00001230ull;
    ASSERT_TRUE(memory.map_region(kAt, 0x1000, MemoryProtection::ReadWriteExecute));
    ASSERT_TRUE(memory.map_region(0x200000, 0x1000, MemoryProtection::ReadWrite));
    for (GuestAddr orig : {GuestAddr{0x40001000}, GuestAddr{0x80020000}}) {
        const auto code = vcrt::encode_stub_x64(kAt, 256, kHost, orig);
        ASSERT_EQ(code.size(), X64_ACCEL_STUB_SLOT);
        ASSERT_TRUE(memory.write_bytes(kAt, code.data(), code.size()));
        for (u64 n : {u64{255}, u64{256}, u64{257}, u64{1} << 63, ~u64{0}, u64{0}}) {
            cpu::CpuState cpu;
            cpu.init_long_mode();
            cpu.set_sp(0x200808);
            for (u8 r = 0; r < 16; ++r) {
                if (r != 4) cpu.gpr.set64(r, 0xB0B0B0B0B0B0B000ull + r);
            }
            cpu.gpr.r8 = n;
            const GuestAddr got = step_until(cpu, memory, kAt, {kHost, orig});
            EXPECT_EQ(got, n >= 256 ? kHost : orig) << std::hex << orig << " n=" << n;
            EXPECT_EQ(cpu.sp(), 0x200808u);
            for (u8 r = 0; r < 16; ++r) {
                if (r == 4) continue;
                const u64 want = r == 8 ? n : 0xB0B0B0B0B0B0B000ull + r;
                EXPECT_EQ(cpu.gpr.get64(r), want) << int(r);
            }
        }
    }
}

TEST_F(VcrtStub, EnsureArenaThenInstallStillWritesBlob) {
    GuestMemory memory;
    capsule::Vfs vfs;
    CrtMemoryManager crt_mem(memory);
    CrtStringManager strings;
    ThunkRegistry registry{kThunkBase};
    // The accelerators' fast paths tail-jump into named internal exports
    // (stdio refill, critical-section slow paths); give each a target so
    // the install is not refused for want of one.
    std::map<std::string, DllExports> dlls;
    dlls["msvcrt.dll"] = create_crt_string_thunks(strings, crt_mem, vfs);
    dlls["ucrtbase.dll"] = create_crt_string_thunks(strings, crt_mem, vfs);
    dlls["kernel32.dll"];
    for (const auto& [slot, q] : build_guest_crt_code(CRT_ACCEL_BASE_X86, true).extern_fixups) {
        const std::string dll = q.substr(0, q.find('!')), fn = q.substr(q.find('!') + 1);
        auto& ex = dlls[dll].entries;
        if (std::any_of(ex.begin(), ex.end(), [&](const ExportEntry& x) { return x.name == fn; })) {
            continue;
        }
        ExportEntry e{fn, 0, [](Marshaler&) {}};
        e.internal = true;
        ex.push_back(std::move(e));
    }
    for (auto& [dll, ex] : dlls) ASSERT_TRUE(registry.register_dll(dll, std::move(ex)));
    map_thunk_slab(memory);

    ASSERT_TRUE(ensure_crt_accel_arena_x86(memory));
    ASSERT_TRUE(ensure_crt_accel_arena_x86(memory));  // idempotent
    EXPECT_GT(install_guest_crt_accelerators(memory, registry), 0u);
    const auto blob = build_guest_crt_code(CRT_ACCEL_BASE_X86, false);
    const Byte* live = memory.fetch_ptr(CRT_ACCEL_BASE_X86, 16);
    ASSERT_NE(live, nullptr);
    EXPECT_EQ(std::memcmp(live, blob.code.data(), 16), 0);
    // A second install on the same memory is still effective.
    EXPECT_GT(install_guest_crt_accelerators(memory, registry), 0u);
    ASSERT_TRUE(ensure_x64_accel_arena(memory, registry.thunk_base()));
    ASSERT_TRUE(ensure_x64_accel_arena(memory, registry.thunk_base()));
}

// ---------------------------------------------------------------------------
// Task 4: x86 accel memcpy is overlap-safe (every Windows memcpy is:
// tests/oracle_data/crt_memcpy_overlap_probe32.expected)
// ---------------------------------------------------------------------------

TEST_F(VcrtStub, AccelMemcpyIsMemmoveSafe) {
    GuestMemory memory;
    constexpr GuestAddr kCode = 0x100000, kData = 0x200000, kStack = 0x300000;
    constexpr u32 kRet = 0x00DEAD00;
    ASSERT_TRUE(memory.map_region(kCode, 0x1000, MemoryProtection::ReadWriteExecute));
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite));
    const auto blob = build_guest_crt_code(kCode, false);
    ASSERT_TRUE(memory.write_bytes(kCode, blob.code.data(), blob.code.size()));
    for (const char* name : {"msvcrt.dll!memcpy", "ucrtbase.dll!memcpy"}) {
        u32 entry = 0;
        for (const auto& [n, a] : blob.entries) if (n == name) entry = a;
        ASSERT_NE(entry, 0u) << name;
        for (int delta : {4, -4}) {
            for (u32 i = 0; i < 64; ++i) ASSERT_TRUE(memory.write_u8(kData + 0x100 + i, static_cast<u8>(i)));
            const GuestAddr src = kData + 0x100 + 16, dst = src + delta;
            cpu::CpuState cpu;
            cpu.init_protected_mode();
            const GuestAddr sp = kStack + 0x800;
            cpu.set_sp(sp);
            ASSERT_TRUE(memory.write_u32(sp, kRet));
            ASSERT_TRUE(memory.write_u32(sp + 4, static_cast<u32>(dst)));
            ASSERT_TRUE(memory.write_u32(sp + 8, static_cast<u32>(src)));
            ASSERT_TRUE(memory.write_u32(sp + 12, 32));
            cpu.rip = entry;
            cpu::Interpreter interp(cpu, memory);
            for (int s = 0; s < 10000 && static_cast<u32>(cpu.rip) != kRet; ++s) (void)interp.step();
            ASSERT_EQ(static_cast<u32>(cpu.rip), kRet) << name;
            EXPECT_EQ(cpu.gpr.get32(0), static_cast<u32>(dst));
            for (u32 i = 0; i < 32; ++i) {
                EXPECT_EQ(memory.read_u8(dst + i).value(), static_cast<u8>(16 + i))
                    << name << " delta=" << delta << " i=" << i;
            }
            EXPECT_FALSE(cpu.flags.df());
        }
    }
}

// ---------------------------------------------------------------------------
// Task 5: identity, interposer, lookup hook
// ---------------------------------------------------------------------------

TEST_F(VcrtStub, IdentityIsTheExportDirectoryName) {
    GuestMemory memory;
    auto check = [&](bool x64, const char* name) {
        const auto img = build_image(x64, name, reference_exports(x64));
        const GuestAddr base = 0x10000000;
        EXPECT_TRUE(memory.map_region(base, kImageSize, MemoryProtection::ReadWrite));
        EXPECT_TRUE(memory.write_bytes(base, img.bytes.data(), img.bytes.size()));
        const bool r = vcrt::is_vcruntime140_image(memory, base, base + kImageSize);
        // A truncated image (end before the export directory) is refused.
        const bool truncated = vcrt::is_vcruntime140_image(memory, base, base + 0x800);
        EXPECT_FALSE(truncated) << name;
        EXPECT_TRUE(memory.unmap_region(base, kImageSize));
        return r;
    };
    for (bool x64 : {false, true}) {
        EXPECT_TRUE(check(x64, "VCRUNTIME140.dll"));
        EXPECT_TRUE(check(x64, "vcruntime140.DLL"));
        EXPECT_FALSE(check(x64, "vcruntime140_1.dll"));
        EXPECT_FALSE(check(x64, "vcruntime140d.dll"));
        EXPECT_FALSE(check(x64, "VCRUNTIME140_CLR0400.dll"));
        EXPECT_FALSE(check(x64, "msvcrt.dll"));
    }
    // An export directory RVA outside the image.
    auto img = build_image(true, "VCRUNTIME140.dll", reference_exports(true));
    const u32 lfanew = 0x80;
    const usize dir = lfanew + 4 + sizeof(pe::CoffHeader) + 112;
    const u32 bad = 0x10000;
    std::memcpy(img.bytes.data() + dir, &bad, 4);
    ASSERT_TRUE(memory.map_region(0x10000000, kImageSize, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.write_bytes(0x10000000, img.bytes.data(), img.bytes.size()));
    EXPECT_FALSE(vcrt::is_vcruntime140_image(memory, 0x10000000, 0x10000000 + kImageSize));
}

TEST_F(VcrtStub, GetProcAddressHandsOutTheStub) {
    for (bool x64 : {false, true}) {
        World w;
        const GuestAddr base = x64 ? kImg64Near : kImg32;
        const auto img = build_image(x64, "VCRUNTIME140.dll", reference_exports(x64));
        w.load(img, base, x64, "C:\\app\\VCRUNTIME140.dll");
        const GuestAddr stub = w.mm->get_proc_address(base, "memcpy");
        EXPECT_TRUE(stub < base || stub >= base + kImageSize) << x64;
        if (x64) {
            EXPECT_TRUE(within_rel32(stub, base) && within_rel32(stub, base + kImageSize));
        } else {
            EXPECT_GE(stub, kX86Stubs);
            EXPECT_LT(stub, kX86Stubs + kX86StubSlots * CRT_ACCEL_STUB_SLOT_X86);
        }
        EXPECT_EQ(w.mm->get_proc_address(base, "memcpy"), stub) << "stable";
        // Each sized function gets its own stub; strchr stays the module's.
        EXPECT_NE(w.mm->get_proc_address(base, "memset"), stub);
        EXPECT_EQ(w.mm->get_proc_address(base, "strchr"), base + img.rva("strchr"));
        // The stub's bytes are exactly the encoding for its slot.
        const auto want = x64 ? vcrt::encode_stub_x64(stub, vcrt::threshold(vcrt::Fn::Memcpy, 64),
                                                      w.host_slot(vcrt::Fn::Memcpy),
                                                      base + img.rva("memcpy"))
                              : vcrt::encode_stub_x86(stub, vcrt::threshold(vcrt::Fn::Memcpy, 32),
                                                      w.host_slot(vcrt::Fn::Memcpy),
                                                      base + img.rva("memcpy"));
        const Byte* live = w.memory.fetch_ptr(stub, want.size());
        ASSERT_NE(live, nullptr);
        EXPECT_EQ(std::memcmp(live, want.data(), want.size()), 0);
    }
}

TEST_F(VcrtStub, KillSwitchReturnsTheRealBody) {
    vcrt::force_for_test(vcrt::Force::Off);
    World w;
    const auto img = build_image(true, "VCRUNTIME140.dll", reference_exports(true));
    w.load(img, kImg64Near, true, "C:\\app\\VCRUNTIME140.dll");
    EXPECT_EQ(w.mm->get_proc_address(kImg64Near, "memcpy"), kImg64Near + img.rva("memcpy"));
}

TEST_F(VcrtStub, NonVcruntimeModuleUntouched) {
    World w;
    const auto img = build_image(true, "vcruntime140_1.dll", reference_exports(true));
    w.load(img, kImg64Near, true, "C:\\app\\VCRUNTIME140.dll");
    EXPECT_EQ(w.mm->get_proc_address(kImg64Near, "memcpy"), kImg64Near + img.rva("memcpy"));
}

TEST_F(VcrtStub, CompanionResolverBindsTheStub) {
    World w;
    const auto img = build_image(true, "VCRUNTIME140.dll", reference_exports(true));
    w.load(img, kImg64Near, true, "C:\\app\\VCRUNTIME140.dll");
    auto resolve = ModuleManagerTestAccessor::companion_resolver(*w.mm, "C:\\app");
    const u64 bound = resolve("VCRUNTIME140.dll", "memcpy", 0);
    EXPECT_EQ(bound, w.mm->get_proc_address(kImg64Near, "memcpy"));
    EXPECT_NE(bound, kImg64Near + img.rva("memcpy"));
}

TEST_F(VcrtStub, ByOrdinalReturnsTheRealBody) {
    World w;
    const auto img = build_image(true, "VCRUNTIME140.dll", reference_exports(true));
    w.load(img, kImg64Near, true, "C:\\app\\VCRUNTIME140.dll");
    // Sorted export order: memchr memcmp memcpy memmove memset strchr; Base 1.
    EXPECT_EQ(w.mm->get_proc_address_by_ordinal(kImg64Near, 3), kImg64Near + img.rva("memcpy"));
}

TEST_F(VcrtStub, FullStubRegionFallsBack) {
    World w;
    std::vector<GuestAddr> got;
    const auto img = build_image(false, "VCRUNTIME140.dll", reference_exports(false));
    // 31 x86 slots; 7 images x 5 sized functions = 35 stubs requested.
    for (u32 i = 0; i < 7; ++i) {
        const GuestAddr base = kImg32 + 0x10000 * i;
        w.load(img, base, false, "C:\\app\\vc" + std::to_string(i) + ".dll");
        for (const char* f : {"memcpy", "memmove", "memset", "memcmp", "memchr"}) {
            const GuestAddr a = w.mm->get_proc_address(base, f);
            if (got.size() < kX86StubSlots) {
                EXPECT_GE(a, kX86Stubs) << i << f;
            } else {
                EXPECT_EQ(a, base + img.rva(f)) << i << f;
            }
            got.push_back(a);
        }
    }
}

// ---------------------------------------------------------------------------
// Task 6: end-to-end through the run loop
// ---------------------------------------------------------------------------

enum class Arm { InChain, Dispatcher, Interpreter };

class VcrtStubRun : public VcrtStub {
protected:
    void SetUp() override {
        VcrtStub::SetUp();
        host_in_chain::force_for_test(host_in_chain::Force::On);
        cli::set_in_chain_crossing_counting(true);
        if (const char* off = cli::in_chain_full_path_diagnostic()) {
            GTEST_SKIP() << "in-chain crossings off in this process: " << off;
        }
    }
    void TearDown() override {
        host_in_chain::force_for_test(host_in_chain::Force::None);
        cli::set_in_chain_crossing_counting(false);
        JitCompiler::set_inchain_thunks_codegen_for_test(-1);
        JitCompiler::set_jit_hot_threshold_for_test(0);
        VcrtStub::TearDown();
    }

    struct Rig {
        World w;
        bool x64;
        GuestAddr base;
        Image img;
        Rig(bool is64, GuestAddr at)
            : x64(is64), base(at),
              img(build_image(is64, "VCRUNTIME140.dll", reference_exports(is64))) {
            map_thunk_slab(w.memory);
            w.load(img, base, x64, "C:\\app\\VCRUNTIME140.dll");
        }
        GuestAddr original(const char* f) const { return base + img.rva(f); }
    };

    struct Outcome {
        cpu::CpuState cpu;
        u64 host_served{0};
        bool ok{false};
    };

    static void select(Arm arm) {
        JitCompiler::set_inchain_thunks_codegen_for_test(arm == Arm::InChain ? 1 : 0);
        JitCompiler::set_jit_hot_threshold_for_test(
            arm == Arm::Interpreter ? JitCompiler::kJitHotThresholdMax : 1);
    }

    /// Call @p target(a0, a1, a2) once through the run loop.
    static Outcome call(Rig& rig, GuestAddr target, u64 a0, u64 a1, u64 a2) {
        Outcome out;
        cpu::CpuState& cpu = out.cpu;
        GuestMemory& mem = rig.w.memory;
        if (rig.x64) {
            cpu.init_long_mode();
            cpu.set_sp(kStackTop64);
            EXPECT_TRUE(mem.write_u64(kStackTop64, cli::PROCESS_RETURN_SENTINEL));
            cpu.gpr.rcx = a0;
            cpu.gpr.rdx = a1;
            cpu.gpr.r8 = a2;
            for (u8 r : {3, 5, 6, 7, 12, 13, 14, 15}) cpu.gpr.set64(r, 0xC5C5C5C500000000ull + r);
        } else {
            cpu.init_protected_mode();
            cpu.set_sp(kStackTop32);
            EXPECT_TRUE(mem.write_u32(kStackTop32, static_cast<u32>(cli::PROCESS_RETURN_SENTINEL)));
            EXPECT_TRUE(mem.write_u32(kStackTop32 + 4, static_cast<u32>(a0)));
            EXPECT_TRUE(mem.write_u32(kStackTop32 + 8, static_cast<u32>(a1)));
            EXPECT_TRUE(mem.write_u32(kStackTop32 + 12, static_cast<u32>(a2)));
            for (u8 r : {3, 5, 6, 7}) cpu.gpr.set32(r, 0xC5C5C500u + r);
        }
        cpu.rip = target;
        cli::RunConfig config;
        config.max_instructions = 50'000'000;
        const u64 h0 = cli::in_chain_host_crossings_this_thread();
        const auto r = cli::run_execution_loop(cpu, mem, rig.w.registry, rig.w.process, config);
        out.host_served = cli::in_chain_host_crossings_this_thread() - h0;
        out.ok = r.success || r.error_message.empty();
        // Callee-saved registers and the stack pointer (caller pops cdecl).
        if (rig.x64) {
            for (u8 r2 : {3, 5, 6, 7, 12, 13, 14, 15}) {
                EXPECT_EQ(cpu.gpr.get64(r2), 0xC5C5C5C500000000ull + r2) << "gpr " << int(r2);
            }
        } else {
            for (u8 r2 : {3, 5, 6, 7}) EXPECT_EQ(cpu.gpr.get32(r2), 0xC5C5C500u + r2) << int(r2);
        }
        EXPECT_FALSE(cpu.flags.df());
        return out;
    }
};

TEST_F(VcrtStubRun, BothBranchesGiveIdenticalResults) {
    constexpr u32 kT = 64;
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        for (usize i = 0; i < 5; ++i) vcrt::set_threshold_for_test(static_cast<vcrt::Fn>(i), x64 ? 64 : 32, kT);
        for (Arm arm : {Arm::InChain, Arm::Dispatcher, Arm::Interpreter}) {
            SCOPED_TRACE(static_cast<int>(arm));
            select(arm);
            Rig rig(x64, x64 ? kImg64Far : kImg32);
            GuestMemory& mem = rig.w.memory;
            const u64 sizes[] = {0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 33, 63,
                                 kT - 1, kT, kT + 1, 4096, 4097, 65536};
            const GuestAddr kSrc = kBuf + 0x10000, kFar = kBuf + 0x28000;
            std::vector<u8> pattern(0x40000);
            for (usize k = 0; k < pattern.size(); ++k) pattern[k] = static_cast<u8>(k * 7 + (k >> 9));
            auto reset = [&] { ASSERT_TRUE(mem.write_bytes(kBuf, pattern.data(), pattern.size())); };
            auto snapshot = [&] {
                std::vector<u8> v(pattern.size());
                EXPECT_TRUE(mem.read_bytes(kBuf, v.data(), v.size()));
                return v;
            };
            const u64 ret_mask = x64 ? ~u64{0} : 0xFFFFFFFFull;
            for (const char* f : {"memcpy", "memmove"}) {
                const GuestAddr stub = rig.w.mm->get_proc_address(rig.base, f);
                ASSERT_NE(stub, rig.original(f));
                for (u64 n : sizes) {
                    for (long delta : {0L, 1L, -1L, 17L, -17L, 4096L, -4096L}) {
                        for (u32 off : {0u, 3u}) {
                            const GuestAddr src = kSrc + off;
                            const GuestAddr dst = delta == 0 ? kFar + 1 + off
                                                             : static_cast<GuestAddr>(static_cast<i64>(src) + delta);
                            reset();
                            std::vector<u8> want = pattern;
                            std::memmove(want.data() + (dst - kBuf), want.data() + (src - kBuf), n);
                            const auto o = call(rig, stub, dst, src, n);
                            ASSERT_EQ(o.cpu.gpr.rax & ret_mask, dst) << f << " n=" << n;
                            ASSERT_EQ(snapshot(), want) << f << " n=" << n << " delta=" << delta << " off=" << off;
                        }
                    }
                }
            }
            {
                const GuestAddr stub = rig.w.mm->get_proc_address(rig.base, "memset");
                for (u64 n : sizes) {
                    for (u32 off : {0u, 1u, 15u}) {
                        reset();
                        std::vector<u8> want = pattern;
                        std::memset(want.data() + 0x10000 + off, 0xA5, n);
                        const auto o = call(rig, stub, kSrc + off, 0x1A5, n);
                        ASSERT_EQ(o.cpu.gpr.rax & ret_mask, kSrc + off);
                        ASSERT_EQ(snapshot(), want) << "memset n=" << n;
                    }
                }
            }
            {
                const GuestAddr stub = rig.w.mm->get_proc_address(rig.base, "memcmp");
                reset();
                ASSERT_TRUE(mem.write_bytes(kFar, pattern.data() + 0x10000, 0x10010));
                for (u64 n : sizes) {
                    for (u64 pos : {u64{0}, n / 2, n ? n - 1 : 0}) {
                        for (int sign : {0, -1, 1}) {
                            ASSERT_TRUE(mem.write_bytes(kFar, pattern.data() + 0x10000, n + 1));
                            int want = 0;
                            if (sign != 0 && n > 0) {
                                const u8 a = pattern[0x10000 + pos];
                                const u8 b = sign < 0 ? static_cast<u8>(a + 1 == 0 ? 0 : a + 1)
                                                      : static_cast<u8>(a == 0 ? 0 : a - 1);
                                ASSERT_TRUE(mem.write_u8(kFar + pos, b));
                                want = a < b ? -1 : (a > b ? 1 : 0);
                            }
                            const auto o = call(rig, stub, kSrc, kFar, n);
                            EXPECT_EQ(static_cast<i32>(static_cast<u32>(o.cpu.gpr.rax)), want)
                                << "memcmp n=" << n << " pos=" << pos << " sign=" << sign;
                        }
                    }
                }
            }
            {
                const GuestAddr stub = rig.w.mm->get_proc_address(rig.base, "memchr");
                for (u64 n : sizes) {
                    reset();
                    std::vector<u8> zone(n + 1, 0x11);
                    if (n > 2) zone[n - 2] = 0x99;
                    ASSERT_TRUE(mem.write_bytes(kSrc, zone.data(), zone.size()));
                    const GuestAddr want = n > 2 ? kSrc + n - 2 : 0;
                    const auto o = call(rig, stub, kSrc, 0x199, n);  // c -> 0x99
                    EXPECT_EQ(o.cpu.gpr.rax & ret_mask, want) << "memchr n=" << n;
                    const auto miss = call(rig, stub, kSrc, 0x42, n);
                    EXPECT_EQ(miss.cpu.gpr.rax & ret_mask, 0u) << "memchr miss n=" << n;
                }
            }
        }
    }
}

TEST_F(VcrtStubRun, HostBranchIsServedInChain) {
    for (bool x64 : {true, false}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        vcrt::set_threshold_for_test(vcrt::Fn::Memcpy, x64 ? 64 : 32, 64);
        select(Arm::InChain);
        Rig rig(x64, x64 ? kImg64Far : kImg32);
        const GuestAddr stub = rig.w.mm->get_proc_address(rig.base, "memcpy");
        u64 big = 0, small = 0;
        for (int i = 0; i < 4; ++i) {  // warm: the first runs compile
            big = call(rig, stub, kBuf + 0x20000, kBuf, 4096).host_served;
            small = call(rig, stub, kBuf + 0x20000, kBuf, 8).host_served;
        }
        EXPECT_GE(big, 1u);
        EXPECT_EQ(small, 0u);
    }
}

GuestMemory* g_reprotect_memory = nullptr;
constexpr GuestAddr kP1 = kBuf + 0x30000;  // page-aligned

TEST_F(VcrtStubRun, HostRaiseReachesGuestVeh) {
    constexpr GuestAddr kHandler = 0x62000, kRec = 0x63000, kIat = 0x63800, kTeb = 0x64000;
    auto d32 = [](std::vector<u8>& v, u64 x) {
        for (int i = 0; i < 4; ++i) v.push_back(static_cast<u8>(x >> (8 * i)));
    };
    for (bool x64 : {true, false}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        for (usize i = 0; i < 5; ++i) vcrt::set_threshold_for_test(static_cast<vcrt::Fn>(i), x64 ? 64 : 32, 64);
        select(Arm::InChain);
        Rig rig(x64, x64 ? kImg64Far : kImg32);
        GuestMemory& mem = rig.w.memory;
        // A host export the guest handler calls to make P1 writable again
        // (standing in for its VirtualProtect).
        DllExports t;
        t.entries.push_back(ExportEntry{"Reprotect", 0, [](Marshaler& m) {
            (void)g_reprotect_memory->protect_region(kP1, 0x1000, MemoryProtection::ReadWrite);
            m.set_return(0);
        }});
        ASSERT_TRUE(rig.w.registry.register_dll("wstest.dll", std::move(t)));
        g_reprotect_memory = &mem;
        const GuestAddr reprotect = rig.w.registry.get_thunk_address("wstest.dll", "Reprotect").value();
        ASSERT_TRUE(mem.write_u64(kIat, reprotect));

        // LONG CALLBACK Veh(EXCEPTION_POINTERS* ep): record ExceptionCode,
        // NumberParameters, ExceptionInformation[0..1] and the byte at P1-1,
        // call Reprotect, return EXCEPTION_CONTINUE_EXECUTION.
        std::vector<u8> h;
        if (x64) {
            h = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x01, 0x8B, 0x10};
            h.insert(h.end(), {0x89, 0x14, 0x25}); d32(h, kRec);
            h.insert(h.end(), {0x8B, 0x50, 0x18, 0x89, 0x14, 0x25}); d32(h, kRec + 4);
            h.insert(h.end(), {0x48, 0x8B, 0x50, 0x20, 0x48, 0x89, 0x14, 0x25}); d32(h, kRec + 8);
            h.insert(h.end(), {0x48, 0x8B, 0x50, 0x28, 0x48, 0x89, 0x14, 0x25}); d32(h, kRec + 16);
            h.insert(h.end(), {0x8A, 0x14, 0x25}); d32(h, kP1 - 1);
            h.insert(h.end(), {0x88, 0x14, 0x25}); d32(h, kRec + 24);
            h.insert(h.end(), {0xFF, 0x14, 0x25}); d32(h, kIat);
            h.insert(h.end(), {0x48, 0x83, 0xC4, 0x28, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3});
        } else {
            h = {0x8B, 0x4C, 0x24, 0x04, 0x8B, 0x01, 0x8B, 0x10, 0x89, 0x15}; d32(h, kRec);
            h.insert(h.end(), {0x8B, 0x50, 0x10, 0x89, 0x15}); d32(h, kRec + 4);
            h.insert(h.end(), {0x8B, 0x50, 0x14, 0x89, 0x15}); d32(h, kRec + 8);
            h.insert(h.end(), {0x8B, 0x50, 0x18, 0x89, 0x15}); d32(h, kRec + 16);
            h.insert(h.end(), {0x8A, 0x15}); d32(h, kP1 - 1);
            h.insert(h.end(), {0x88, 0x15}); d32(h, kRec + 24);
            h.insert(h.end(), {0xFF, 0x15}); d32(h, kIat);
            h.insert(h.end(), {0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xC2, 0x04, 0x00});
        }
        ASSERT_TRUE(mem.write_bytes(kHandler, h.data(), h.size()));
        std::vector<u8> zero(32, 0);
        ASSERT_TRUE(mem.write_bytes(kRec, zero.data(), zero.size()));
        winapi::clear_vectored_exception_handlers_for_test();
        const u64 veh = winapi::add_vectored_exception_handler(1, kHandler);
        ASSERT_NE(veh, 0u);

        // src: 8192 patterned bytes; dst: P1-100, P1 inaccessible.
        const GuestAddr src = kBuf, dst = kP1 - 100;
        std::vector<u8> pat(8192);
        for (usize k = 0; k < pat.size(); ++k) pat[k] = static_cast<u8>(k * 5 + 3);
        ASSERT_TRUE(mem.write_bytes(src, pat.data(), pat.size()));
        ASSERT_TRUE(mem.protect_region(kP1, 0x1000, MemoryProtection::None));

        const GuestAddr stub = rig.w.mm->get_proc_address(rig.base, "memcpy");
        cpu::CpuState cpu;
        if (x64) {
            cpu.init_long_mode();
            cpu.set_sp(kStackTop64);
            ASSERT_TRUE(mem.write_u64(kStackTop64, cli::PROCESS_RETURN_SENTINEL));
            cpu.gpr.rcx = dst;
            cpu.gpr.rdx = src;
            cpu.gpr.r8 = pat.size();
        } else {
            cpu.init_protected_mode();
            cpu.seg.fs_desc.base = kTeb;
            ASSERT_TRUE(mem.write_u32(kTeb, 0xFFFFFFFFu));  // empty SEH chain
            cpu.set_sp(kStackTop32);
            ASSERT_TRUE(mem.write_u32(kStackTop32, static_cast<u32>(cli::PROCESS_RETURN_SENTINEL)));
            ASSERT_TRUE(mem.write_u32(kStackTop32 + 4, static_cast<u32>(dst)));
            ASSERT_TRUE(mem.write_u32(kStackTop32 + 8, static_cast<u32>(src)));
            ASSERT_TRUE(mem.write_u32(kStackTop32 + 12, static_cast<u32>(pat.size())));
        }
        cpu.rip = stub;
        cli::RunConfig config;
        config.max_instructions = 10'000'000;
        const auto r = cli::run_execution_loop(cpu, mem, rig.w.registry, rig.w.process, config);
        EXPECT_TRUE(r.success) << r.error_message;
        EXPECT_EQ(mem.read_u32(kRec).value(), vcrt::kStatusAccessViolation);
        EXPECT_EQ(mem.read_u32(kRec + 4).value(), 2u);
        const u64 info0 = x64 ? mem.read_u64(kRec + 8).value() : mem.read_u32(kRec + 8).value();
        const u64 info1 = x64 ? mem.read_u64(kRec + 16).value() : mem.read_u32(kRec + 16).value();
        EXPECT_EQ(info0, 1u) << "a write";
        EXPECT_EQ(info1, kP1);
        EXPECT_EQ(mem.read_u8(kRec + 24).value(), pat[99]) << "bytes before P1 written first";
        std::vector<u8> got(pat.size());
        ASSERT_TRUE(mem.read_bytes(dst, got.data(), got.size()));
        EXPECT_EQ(got, pat) << "the copy completes after EXCEPTION_CONTINUE_EXECUTION";
        EXPECT_TRUE(winapi::remove_vectored_exception_handler(veh));
        winapi::clear_vectored_exception_handlers_for_test();
        g_reprotect_memory = nullptr;
    }
}

// ---------------------------------------------------------------------------
// x64 stub pages within reach of the image; identity cache revalidation
// ---------------------------------------------------------------------------

TEST_F(VcrtStub, X64FarImageGetsAStubPageInReach) {
    World w;
    const auto img = build_image(true, "VCRUNTIME140.dll", reference_exports(true));
    w.load(img, kImg64Far, true, "C:\\app\\VCRUNTIME140.dll");
    std::vector<GuestAddr> stubs;
    for (const char* f : {"memcpy", "memmove", "memset", "memcmp", "memchr"}) {
        const GuestAddr stub = w.mm->get_proc_address(kImg64Far, f);
        ASSERT_NE(stub, kImg64Far + img.rva(f)) << f;
        EXPECT_TRUE(within_rel32(stub, kImg64Far)) << f;
        // The guest branch is the direct jb rel32 (0F 82) at offset 7.
        const Byte* live = w.memory.fetch_ptr(stub, 13);
        ASSERT_NE(live, nullptr);
        EXPECT_EQ(live[7], 0x0F);
        EXPECT_EQ(live[8], 0x82);
        i32 rel = 0;
        std::memcpy(&rel, live + 9, 4);
        EXPECT_EQ(stub + 13 + static_cast<i64>(rel), kImg64Far + img.rva(f)) << f;
        stubs.push_back(stub);
    }
    // One page serves the image; its slot 0 is the header, not a stub.
    for (GuestAddr a : stubs) EXPECT_EQ(a & ~GuestAddr{0xFFF}, stubs[0] & ~GuestAddr{0xFFF});
    for (GuestAddr a : stubs) EXPECT_NE(a & 0xFFF, 0u);
    // A second image in reach of that page shares it.
    w.load(img, kImg64Far + 0x100000, true, "C:\\app\\other\\VCRUNTIME140.dll");
    const GuestAddr second = w.mm->get_proc_address(kImg64Far + 0x100000, "memcpy");
    EXPECT_EQ(second & ~GuestAddr{0xFFF}, stubs[0] & ~GuestAddr{0xFFF});
    EXPECT_NE(second, stubs[0]);
}

TEST_F(VcrtStub, IdentityIsRecheckedAfterTheImageChanges) {
    World w;
    const auto img = build_image(true, "VCRUNTIME140.dll", reference_exports(true));
    w.load(img, kImg64Near, true, "C:\\app\\VCRUNTIME140.dll");
    const vcrt::LookupModule mod{kImg64Near, kImg64Near + kImageSize, 64};
    const GuestAddr orig = kImg64Near + img.rva("memcpy");
    const GuestAddr stub = vcrt::interpose_lookup(w.memory, w.registry, mod, "memcpy", orig);
    ASSERT_NE(stub, orig);
    EXPECT_EQ(vcrt::interpose_lookup(w.memory, w.registry, mod, "memcpy", orig), stub);
    // A different image now at the same base and size (FreeLibrary, then
    // LoadLibrary of msvcrt.dll there): the cached identity must not answer.
    const auto other = build_image(true, "msvcrt.dll", reference_exports(true));
    ASSERT_TRUE(w.memory.protect_region(kImg64Near, kImageSize, MemoryProtection::ReadWrite));
    ASSERT_TRUE(w.memory.write_bytes(kImg64Near, other.bytes.data(), other.bytes.size()));
    EXPECT_EQ(vcrt::interpose_lookup(w.memory, w.registry, mod, "memcpy", orig), orig);
    // And back: vcruntime140 again is interposed again.
    ASSERT_TRUE(w.memory.write_bytes(kImg64Near, img.bytes.data(), img.bytes.size()));
    EXPECT_EQ(vcrt::interpose_lookup(w.memory, w.registry, mod, "memcpy", orig), stub);
}

TEST_F(VcrtStub, StaleStubIsRewritten) {
    World w;
    const auto img = build_image(false, "VCRUNTIME140.dll", reference_exports(false));
    w.load(img, kImg32, false, "C:\\app\\VCRUNTIME140.dll");
    const GuestAddr stub = w.mm->get_proc_address(kImg32, "memcpy");
    ASSERT_NE(stub, kImg32 + img.rva("memcpy"));
    // Something cleared the slot (the cache must notice, not hand it out).
    std::vector<u8> zero(CRT_ACCEL_STUB_SLOT_X86, 0);
    ASSERT_TRUE(write_accel_code(w.memory, stub, zero.data(), zero.size()));
    const GuestAddr again = w.mm->get_proc_address(kImg32, "memcpy");
    const auto want = vcrt::encode_stub_x86(again, vcrt::threshold(vcrt::Fn::Memcpy, 32),
                                            w.host_slot(vcrt::Fn::Memcpy), kImg32 + img.rva("memcpy"));
    const Byte* live = w.memory.fetch_ptr(again, want.size());
    ASSERT_NE(live, nullptr);
    EXPECT_EQ(std::memcmp(live, want.data(), want.size()), 0);
}

// ---------------------------------------------------------------------------
// A faulted host copy resumes at the fault (EXCEPTION_CONTINUE_EXECUTION)
// ---------------------------------------------------------------------------

std::vector<GuestAddr> g_fix_pages;  // pages the handler's Reprotect fixes, in order
u32 g_fix_calls = 0;

TEST_F(VcrtStubRun, ContinueResumesAnOverlappingMoveMidCopy) {
    constexpr GuestAddr kHandler = 0x62000, kRec = 0x63000, kIat = 0x63800, kTeb = 0x64000;
    auto d32 = [](std::vector<u8>& v, u64 x) {
        for (int i = 0; i < 4; ++i) v.push_back(static_cast<u8>(x >> (8 * i)));
    };
    struct Shape {
        const char* name;
        GuestAddr src, dst;
        u64 n;
        std::vector<GuestAddr> holes;  // inaccessible pages, fixed one per fault
    };
    // P1 = kBuf + 0x30000, P2 = P1 + 0x2000 (both page-aligned).
    const GuestAddr p1 = kP1, p2 = kP1 + 0x2000;
    const Shape shapes[] = {
        // dst below src, overlapping: ascending; the source reaches P1 first.
        {"forward", p1 - 2983, p1 - 3000, 6000, {p1}},
        // dst above src, overlapping: descending from the top; reaches P1's
        // top after 2950 bytes.
        {"backward", p1 + 0x1000 - 50, p1 + 0x1000 - 33, 3000, {p1}},
        // Two faults in one call: P1 then P2 (each continued).
        {"forward-twice", p1 - 1000, p1 - 1017, 0x2000 + 3000, {p1, p2}},
        // memcpy is memmove: the same through memcpy.
        {"memcpy-backward", p1 + 0x1000 - 50, p1 + 0x1000 - 33, 3000, {p1}},
    };
    for (bool x64 : {true, false}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        for (usize i = 0; i < 5; ++i) vcrt::set_threshold_for_test(static_cast<vcrt::Fn>(i), x64 ? 64 : 32, 64);
        for (const Shape& sh : shapes) {
            SCOPED_TRACE(sh.name);
            select(Arm::InChain);
            Rig rig(x64, x64 ? kImg64Far : kImg32);
            GuestMemory& mem = rig.w.memory;
            DllExports t;
            t.entries.push_back(ExportEntry{"Reprotect", 0, [](Marshaler& m) {
                if (g_fix_calls < g_fix_pages.size()) {
                    (void)g_reprotect_memory->protect_region(g_fix_pages[g_fix_calls], 0x1000,
                                                             MemoryProtection::ReadWrite);
                }
                ++g_fix_calls;
                m.set_return(0);
            }});
            ASSERT_TRUE(rig.w.registry.register_dll("wstest.dll", std::move(t)));
            g_reprotect_memory = &mem;
            g_fix_pages = sh.holes;
            g_fix_calls = 0;
            ASSERT_TRUE(mem.write_u64(kIat, rig.w.registry.get_thunk_address("wstest.dll", "Reprotect").value()));
            // LONG CALLBACK Veh(EXCEPTION_POINTERS*): call Reprotect, return
            // EXCEPTION_CONTINUE_EXECUTION.
            std::vector<u8> h;
            if (x64) {
                h = {0x48, 0x83, 0xEC, 0x28, 0xFF, 0x14, 0x25}; d32(h, kIat);
                h.insert(h.end(), {0x48, 0x83, 0xC4, 0x28, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3});
            } else {
                h = {0xFF, 0x15}; d32(h, kIat);
                h.insert(h.end(), {0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xC2, 0x04, 0x00});
            }
            ASSERT_TRUE(mem.write_bytes(kHandler, h.data(), h.size()));
            winapi::clear_vectored_exception_handlers_for_test();
            const u64 veh = winapi::add_vectored_exception_handler(1, kHandler);
            ASSERT_NE(veh, 0u);

            std::vector<u8> pat(0x40000);
            for (usize k = 0; k < pat.size(); ++k) pat[k] = static_cast<u8>(k * 7 + (k >> 8) + 1);
            ASSERT_TRUE(mem.write_bytes(kBuf, pat.data(), pat.size()));
            std::vector<u8> want = pat;
            std::memmove(want.data() + (sh.dst - kBuf), want.data() + (sh.src - kBuf), sh.n);
            for (GuestAddr hole : sh.holes) {
                ASSERT_TRUE(mem.protect_region(hole, 0x1000, MemoryProtection::None));
            }

            const char* f = std::strncmp(sh.name, "memcpy", 6) == 0 ? "memcpy" : "memmove";
            const GuestAddr stub = rig.w.mm->get_proc_address(rig.base, f);
            cpu::CpuState cpu;
            if (x64) {
                cpu.init_long_mode();
                cpu.set_sp(kStackTop64);
                ASSERT_TRUE(mem.write_u64(kStackTop64, cli::PROCESS_RETURN_SENTINEL));
                cpu.gpr.rcx = sh.dst;
                cpu.gpr.rdx = sh.src;
                cpu.gpr.r8 = sh.n;
            } else {
                cpu.init_protected_mode();
                cpu.seg.fs_desc.base = kTeb;
                ASSERT_TRUE(mem.write_u32(kTeb, 0xFFFFFFFFu));
                cpu.set_sp(kStackTop32);
                ASSERT_TRUE(mem.write_u32(kStackTop32, static_cast<u32>(cli::PROCESS_RETURN_SENTINEL)));
                ASSERT_TRUE(mem.write_u32(kStackTop32 + 4, static_cast<u32>(sh.dst)));
                ASSERT_TRUE(mem.write_u32(kStackTop32 + 8, static_cast<u32>(sh.src)));
                ASSERT_TRUE(mem.write_u32(kStackTop32 + 12, static_cast<u32>(sh.n)));
            }
            cpu.rip = stub;
            cli::RunConfig config;
            config.max_instructions = 10'000'000;
            const auto r = cli::run_execution_loop(cpu, mem, rig.w.registry, rig.w.process, config);
            EXPECT_TRUE(r.success) << r.error_message;
            EXPECT_EQ(g_fix_calls, sh.holes.size()) << "one fault per inaccessible page";
            EXPECT_EQ(cpu.gpr.rax & (x64 ? ~u64{0} : 0xFFFFFFFFull), sh.dst) << "returns dst";
            std::vector<u8> got(pat.size());
            ASSERT_TRUE(mem.read_bytes(kBuf, got.data(), got.size()));
            usize first_bad = got.size();
            for (usize k = 0; k < got.size(); ++k) {
                if (got[k] != want[k]) { first_bad = k; break; }
            }
            EXPECT_EQ(first_bad, got.size()) << "memmove result after continue (first wrong byte at kBuf+"
                                             << first_bad << ")";
            EXPECT_TRUE(winapi::remove_vectored_exception_handler(veh));
            winapi::clear_vectored_exception_handlers_for_test();
            g_reprotect_memory = nullptr;
        }
    }
}

TEST_F(VcrtStubRun, ContinueResumesAFill) {
    constexpr GuestAddr kHandler = 0x62000, kIat = 0x63800, kTeb = 0x64000;
    auto d32 = [](std::vector<u8>& v, u64 x) {
        for (int i = 0; i < 4; ++i) v.push_back(static_cast<u8>(x >> (8 * i)));
    };
    for (bool x64 : {true, false}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        vcrt::set_threshold_for_test(vcrt::Fn::Memset, x64 ? 64 : 32, 64);
        select(Arm::Dispatcher);
        Rig rig(x64, x64 ? kImg64Far : kImg32);
        GuestMemory& mem = rig.w.memory;
        DllExports t;
        t.entries.push_back(ExportEntry{"Reprotect", 0, [](Marshaler& m) {
            (void)g_reprotect_memory->protect_region(kP1, 0x1000, MemoryProtection::ReadWrite);
            ++g_fix_calls;
            m.set_return(0);
        }});
        ASSERT_TRUE(rig.w.registry.register_dll("wstest.dll", std::move(t)));
        g_reprotect_memory = &mem;
        g_fix_calls = 0;
        ASSERT_TRUE(mem.write_u64(kIat, rig.w.registry.get_thunk_address("wstest.dll", "Reprotect").value()));
        std::vector<u8> h;
        if (x64) {
            h = {0x48, 0x83, 0xEC, 0x28, 0xFF, 0x14, 0x25}; d32(h, kIat);
            h.insert(h.end(), {0x48, 0x83, 0xC4, 0x28, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3});
        } else {
            h = {0xFF, 0x15}; d32(h, kIat);
            h.insert(h.end(), {0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xC2, 0x04, 0x00});
        }
        ASSERT_TRUE(mem.write_bytes(kHandler, h.data(), h.size()));
        winapi::clear_vectored_exception_handlers_for_test();
        const u64 veh = winapi::add_vectored_exception_handler(1, kHandler);
        std::vector<u8> zero(0x3000, 0);
        ASSERT_TRUE(mem.write_bytes(kP1 - 0x1000, zero.data(), zero.size()));
        ASSERT_TRUE(mem.protect_region(kP1, 0x1000, MemoryProtection::None));
        const GuestAddr stub = rig.w.mm->get_proc_address(rig.base, "memset");
        cpu::CpuState cpu;
        const GuestAddr dst = kP1 - 700;
        const u64 n = 0x1000 + 1400;
        if (x64) {
            cpu.init_long_mode();
            cpu.set_sp(kStackTop64);
            ASSERT_TRUE(mem.write_u64(kStackTop64, cli::PROCESS_RETURN_SENTINEL));
            cpu.gpr.rcx = dst;
            cpu.gpr.rdx = 0x3C;
            cpu.gpr.r8 = n;
        } else {
            cpu.init_protected_mode();
            cpu.seg.fs_desc.base = kTeb;
            ASSERT_TRUE(mem.write_u32(kTeb, 0xFFFFFFFFu));
            cpu.set_sp(kStackTop32);
            ASSERT_TRUE(mem.write_u32(kStackTop32, static_cast<u32>(cli::PROCESS_RETURN_SENTINEL)));
            ASSERT_TRUE(mem.write_u32(kStackTop32 + 4, static_cast<u32>(dst)));
            ASSERT_TRUE(mem.write_u32(kStackTop32 + 8, 0x3C));
            ASSERT_TRUE(mem.write_u32(kStackTop32 + 12, static_cast<u32>(n)));
        }
        cpu.rip = stub;
        cli::RunConfig config;
        config.max_instructions = 10'000'000;
        const auto r = cli::run_execution_loop(cpu, mem, rig.w.registry, rig.w.process, config);
        EXPECT_TRUE(r.success) << r.error_message;
        EXPECT_EQ(g_fix_calls, 1u);
        std::vector<u8> got(0x3000);
        ASSERT_TRUE(mem.read_bytes(kP1 - 0x1000, got.data(), got.size()));
        for (usize k = 0; k < got.size(); ++k) {
            const GuestAddr a = kP1 - 0x1000 + k;
            ASSERT_EQ(got[k], (a >= dst && a < dst + n) ? 0x3C : 0) << "kP1-0x1000+" << k;
        }
        EXPECT_TRUE(winapi::remove_vectored_exception_handler(veh));
        winapi::clear_vectored_exception_handlers_for_test();
        g_reprotect_memory = nullptr;
    }
}

TEST_F(VcrtStub, BakedThresholdsInterposeOnlyTheMeasuredWinners) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        for (usize i = 0; i < vcrt::kFnCount; ++i) {
            vcrt::set_threshold_for_test(static_cast<vcrt::Fn>(i), x64 ? 64 : 32, std::nullopt);
        }
        World w;
        const GuestAddr base = x64 ? kImg64Far : kImg32;
        const auto img = build_image(x64, "VCRUNTIME140.dll", reference_exports(x64));
        w.load(img, base, x64, "C:\\app\\VCRUNTIME140.dll");
        for (const char* f : {"memcpy", "memmove", "memset", "strchr"}) {
            EXPECT_EQ(w.mm->get_proc_address(base, f), base + img.rva(f)) << f;
        }
        // x86 keeps the guest bodies: its stub costs more per small call
        // than the host saves (see kThresholdX86).
        for (const char* f : {"memcmp", "memchr"}) {
            if (x64) {
                EXPECT_NE(w.mm->get_proc_address(base, f), base + img.rva(f)) << f;
            } else {
                EXPECT_EQ(w.mm->get_proc_address(base, f), base + img.rva(f)) << f;
            }
        }
    }
}

}  // namespace
