/// @file test_guest_accel_x64.cpp
/// @brief Executes the x64 guest accelerators and checks them against the
///        contracts they implement, in the interpreter and in the JIT.
///
/// Hand-written machine code in the guest's address space is not trusted by
/// inspection. Each case runs the emitted bytes and compares the result with
/// the contract's source: Microsoft Learn TlsGetValue and the TEB layout
/// Vitreo's own TlsSetValue writes (tls.cpp), and, for _finite/_isnan, the
/// values Windows returned (tools/oracle_probe/crt_classify_probe.expected).
/// Both engines run every case: production reaches this code through the JIT,
/// which addresses the TEB through GS.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/msvcrt/guest_accel.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <winapi/kernel32/thread/tls.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/cpu/interpreter/interpreter.hpp>
#include <core/cpu/jit/jit_compiler.hpp>
#include "../cpu/jit_dispatch_execute.hpp"
#include <core/memory/guest_memory.hpp>
#include "runtime/execution/block_decoder.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kCodeBase = 0x0000000140100000ull;
constexpr GuestAddr kStackBase = 0x0000000140200000ull;
constexpr GuestAddr kTebBase = 0x0000000140300000ull;
constexpr GuestAddr kExpansion = 0x0000000140310000ull;
constexpr GuestAddr kRetMagic = 0x0000000140DEAD00ull;  // never mapped
constexpr u32 kSentinelError = 0x1234u;

enum class Engine { Interpreter, Jit };

class GuestAccelX64Test : public ::testing::TestWithParam<Engine> {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kCodeBase, 0x1000,
                                      MemoryProtection::ReadWriteExecute).is_ok());
        ASSERT_TRUE(memory.map_region(kStackBase, 0x2000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kTebBase, 0x2000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kExpansion, 0x4000,
                                      MemoryProtection::ReadWrite).is_ok());
        blob = build_guest_x64_code(kCodeBase);
        ASSERT_FALSE(blob.code.empty());
        ASSERT_TRUE(memory.write_bytes(kCodeBase, blob.code.data(),
                                       blob.code.size()).is_ok());
    }

    [[nodiscard]] GuestAddr entry(const std::string& name) const {
        for (const auto& [n, offset] : blob.entries) {
            if (n == name) return kCodeBase + offset;
        }
        return 0;
    }

    /// Call one accelerator with RCX/XMM0 set and run it until it returns to
    /// the sentinel. Returns the final CPU state.
    cpu::CpuState call(const std::string& name, u64 rcx, u64 xmm0_bits) {
        const GuestAddr fn = entry(name);
        EXPECT_NE(fn, 0u) << name << " was not emitted";
        cpu::CpuState cpu;
        cpu.init_long_mode();
        cpu.seg.gs_desc.base = kTebBase;
        cpu.gpr.rcx = rcx;
        cpu.gpr.rdx = 0x5555555555555555ull;
        cpu.sse.xmm[0].u64_[0] = xmm0_bits;
        const GuestAddr sp = kStackBase + 0x1000;
        EXPECT_TRUE(memory.write_u64(sp, kRetMagic).is_ok());
        cpu.gpr.rsp = sp;
        cpu.rip = fn;
        for (int blocks = 0; blocks < 16 && cpu.rip != kRetMagic; ++blocks) {
            if (GetParam() == Engine::Interpreter) {
                cpu::Interpreter interp(cpu, memory);
                const cpu::ExecResult r = interp.run(1);
                EXPECT_TRUE(r == cpu::ExecResult::Continue ||
                            r == cpu::ExecResult::Branch)
                    << "step result " << static_cast<int>(r);
                continue;
            }
            cpu::jit::BasicBlock block =
                cli::decode_basic_block(memory, cpu.rip, /*is_64bit=*/true);
            EXPECT_GT(block.instruction_count, 0u);
            if (block.instruction_count == 0) break;
            cpu::jit::JitCompiler jit(memory);
            cpu::jit::CompiledBlock* compiled = jit.compile(block);
            EXPECT_NE(compiled, nullptr) << "JIT declined at 0x" << std::hex
                                         << cpu.rip;
            if (compiled == nullptr) break;
            EXPECT_EQ(cpu::jit::test::execute_like_dispatcher(jit, *compiled, cpu),
                      cpu::jit::JitExecResult::Continue);
        }
        EXPECT_EQ(cpu.rip, kRetMagic) << name << " did not return";
        EXPECT_EQ(cpu.gpr.rsp, sp + 8) << name << " left the stack unbalanced";
        EXPECT_FALSE(cpu.flags.df()) << name << " left DF set";
        return cpu;
    }

    void set_last_error(u32 value) {
        ASSERT_TRUE(memory.write_u32(kTebBase + 0x68, value).is_ok());
    }
    [[nodiscard]] u32 last_error() {
        return memory.read_u32(kTebBase + 0x68).value_or(0xDEADDEADu);
    }

    GuestMemory memory;
    CrtAccelBlob blob;
};

// Learn TlsGetValue: the slot's value, LastError cleared on success; an index
// past the process's slots returns 0 with ERROR_INVALID_PARAMETER. Indexes
// 0-63 live in TEB.TlsSlots; 64-1087 in TEB.TlsExpansionSlots, which reads 0
// until the thread's first expansion TlsSetValue allocates it.
TEST_P(GuestAccelX64Test, TlsGetValueMatchesTheTebContract) {
    for (u32 i = 0; i < TLS_MINIMUM_AVAILABLE; ++i) {
        ASSERT_TRUE(memory.write_u64(kTebBase + 0x1480 + 8ull * i,
                                     0xA000000000000000ull | i).is_ok());
    }
    const auto expect_value = [&](u64 rcx, u64 want) {
        SCOPED_TRACE(::testing::Message() << "rcx=0x" << std::hex << rcx);
        set_last_error(kSentinelError);
        const auto cpu = call("kernel32.dll!TlsGetValue", rcx, 0);
        EXPECT_EQ(cpu.gpr.rax, want);
        EXPECT_EQ(last_error(), 0u) << "success clears LastError";
    };
    expect_value(0, 0xA000000000000000ull);
    expect_value(5, 0xA000000000000005ull);
    expect_value(63, 0xA00000000000003Full);
    // DWORD argument: the upper half of RCX is not part of the index.
    expect_value(0xFFFFFFFF00000007ull, 0xA000000000000007ull);

    // Expansion slots, not yet allocated on this thread: 0, success.
    ASSERT_TRUE(memory.write_u64(kTebBase + 0x1780, 0).is_ok());
    expect_value(64, 0);
    expect_value(TLS_TOTAL_SLOTS - 1, 0);
    // Allocated: slot (index - 64) of the array.
    ASSERT_TRUE(memory.write_u64(kTebBase + 0x1780, kExpansion).is_ok());
    ASSERT_TRUE(memory.write_u64(kExpansion + 0, 0xB0).is_ok());
    ASSERT_TRUE(memory.write_u64(kExpansion + 8ull * 36, 0xB36).is_ok());
    ASSERT_TRUE(memory.write_u64(kExpansion + 8ull * (TLS_TOTAL_SLOTS - 65),
                                 0xBFF).is_ok());
    expect_value(64, 0xB0);
    expect_value(100, 0xB36);
    expect_value(TLS_TOTAL_SLOTS - 1, 0xBFF);

    for (const u64 bad : {u64{TLS_TOTAL_SLOTS}, u64{0xFFFFFFFFu}}) {
        SCOPED_TRACE(::testing::Message() << "invalid index " << bad);
        set_last_error(kSentinelError);
        const auto cpu = call("kernel32.dll!TlsGetValue", bad, 0);
        EXPECT_EQ(cpu.gpr.rax, 0u);
        EXPECT_EQ(last_error(), 87u) << "ERROR_INVALID_PARAMETER";
    }
}

// The values Windows returned for every class of double, both CRTs
// (crt_classify_probe.expected): exactly 1 or 0.
TEST_P(GuestAccelX64Test, FiniteAndIsnanReturnWhatWindowsReturns) {
    const auto path = std::filesystem::path(__FILE__).parent_path()
                          .parent_path().parent_path() /
                      "tests/oracle_data/crt_classify_probe.expected";
    std::ifstream in(path);
    ASSERT_TRUE(in.good()) << path;
    std::string line;
    usize checked = 0;
    while (std::getline(in, line)) {
        std::istringstream row(line);
        std::string dll, fn, bits, arrow;
        int want = -1;
        ASSERT_TRUE(bool(row >> dll >> fn >> bits >> arrow >> want)) << line;
        SCOPED_TRACE(line);
        const u64 x = std::stoull(bits, nullptr, 16);
        const auto cpu = call(dll + "!" + fn, 0x7777777777777777ull, x);
        EXPECT_EQ(cpu.gpr.rax, static_cast<u64>(want))
            << "the int result, zero-extended into RAX";
        ++checked;
    }
    EXPECT_EQ(checked, 60u);
}

INSTANTIATE_TEST_SUITE_P(Engines, GuestAccelX64Test,
                         ::testing::Values(Engine::Interpreter, Engine::Jit),
                         [](const auto& info) {
                             return info.param == Engine::Interpreter
                                        ? std::string("Interpreter")
                                        : std::string("Jit");
                         });

// Guest-facing resolution hands out the guest implementation; the thunk itself
// is unchanged for internal users.
TEST(GuestAccelX64Install, RegistersTheGuestImplementationForEachExport) {
    GuestMemory memory;
    constexpr GuestAddr kThunks = 0x00007FF000000000ull;
    ThunkRegistry registry(kThunks);
    const auto noop = [](Marshaler&) {};
    for (const char* dll : {"kernel32.dll", "ucrtbase.dll", "msvcrt.dll"}) {
        DllExports exports;
        if (std::string(dll) == "kernel32.dll") {
            exports.entries.push_back({"TlsGetValue", 1, noop});
        } else {
            exports.entries.push_back({"_finite", 1, noop});
            exports.entries.push_back({"_isnan", 2, noop});
        }
        ASSERT_TRUE(registry.register_dll(dll, std::move(exports)).is_ok());
    }
    const GuestAddr tls_thunk =
        registry.get_thunk_address("kernel32.dll", "TlsGetValue").value();
    EXPECT_EQ(registry.guest_entry(tls_thunk), tls_thunk);

    EXPECT_EQ(install_guest_x64_accelerators(memory, registry), 5u);

    const GuestAddr base = kThunks + X64_ACCEL_OFFSET;
    const CrtAccelBlob blob = build_guest_x64_code(base);
    for (const auto& [name, offset] : blob.entries) {
        const auto bang = name.find('!');
        auto thunk = registry.get_thunk_address(name.substr(0, bang),
                                                name.substr(bang + 1));
        if (thunk.is_err()) continue;  // kernelbase.dll is not registered here
        SCOPED_TRACE(name);
        EXPECT_EQ(registry.guest_entry(thunk.value()), base + offset);
        EXPECT_TRUE(registry.is_in_thunk_region(thunk.value()));
        EXPECT_FALSE(registry.is_in_thunk_region(base + offset));
        // An inline-hook check on the resolved function sees ordinary code.
        const u8 first = memory.read_u8(base + offset).value_or(0);
        EXPECT_NE(first, 0xE9u);
        EXPECT_NE(first, 0xEBu);
    }
    // The thunk address itself is what get_thunk_address still returns.
    EXPECT_EQ(registry.get_thunk_address("kernel32.dll", "TlsGetValue").value(),
              tls_thunk);
}

}  // namespace
