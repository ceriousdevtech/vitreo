// SPDX-License-Identifier: LGPL-2.1-only
//
// The direct API-thunk crossing (runtime/execution/thunk_direct.hpp, WP-R1):
// an export with a direct entry is served as one plain call that must keep
// every guest-visible contract of the full dispatch path.
//
// Expected values come from the ABI and from Windows, not from Vitreo:
//   * Win64 nonvolatiles (Learn, "x64 ABI conventions", Register volatility):
//     RBX RBP RDI RSI R12-R15 and XMM6-XMM15 are callee-saved; MXCSR bits
//     6-15 (control) are nonvolatile, bits 0-5 (status) volatile.
//   * x86 stdcall (Learn, "__stdcall"): the callee pops its arguments; EBX,
//     ESI, EDI, EBP are callee-saved.
//   * The TEB last error across a D3D11/DXGI COM method: measured,
//     tools/oracle_probe/d3d11_com_last_error_probe{64,32}.expected.

#include <gtest/gtest.h>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "runtime/execution/guest_run_loop.hpp"
#include "runtime/execution/thunk_direct.hpp"
#include "winapi/core/com_direct.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"

#include <array>
#include <fstream>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::cli;

namespace {

constexpr GuestAddr kMemBase = 0x20000;
constexpr usize kMemSize = 0x10000;
constexpr GuestAddr kThunkBase = 0x70000000;
constexpr GuestAddr kTebBase = 0x2c000;  // inside the mapped range

/// What the direct handler saw, and what it does to the CPU.
struct HandlerProbe {
    u64 calls{0};
    u64 args[8]{};
    u64 return_address{0};
    bool clobber{true};
    bool set_host_last_error{false};
    bool transfer_to{false};
    u64 transfer_rip{0};
    bool raise{false};
    bool unwind{false};
    // Re-entrant guest call made from inside the handler.
    GuestMemory* memory{nullptr};
    winapi::ThunkRegistry* registry{nullptr};
    winapi::ProcessManager* process{nullptr};
    GuestAddr nested_code{0};
    u64 nested_result{0};
    bool nested_completed{false};
    u64 args_after_nested[8]{};
};
HandlerProbe g_probe;

void probe_handler(void* /*context*/, winapi::Marshaler& m, u32 slot) {
    ++g_probe.calls;
    cpu::CpuState& cpu = *m.cpu();
    const usize argc = cpu.is_64bit() ? 6 : 3;
    for (usize i = 0; i < argc; ++i) g_probe.args[i] = m.arg(i);
    g_probe.return_address = m.guest_return_address();
    if (g_probe.nested_code != 0) {
        // A guest COM callback from inside a host method (a Release that
        // runs the guest's destructor): arbitrary guest code on this same
        // CpuState, which itself crosses into a direct export.
        bool completed = false;
        g_probe.nested_result = run_guest_callback(
            cpu, *g_probe.memory, *g_probe.registry, *g_probe.process,
            g_probe.nested_code, nullptr, 0, cpu.is_64bit(), &completed);
        g_probe.nested_completed = completed;
        for (usize i = 0; i < argc; ++i) g_probe.args_after_nested[i] = m.arg(i);
    }
    if (g_probe.clobber) {
        cpu.gpr.rbx = 0xB0B0B0B0ull;
        cpu.gpr.rbp = 0xB1B1B1B1ull;
        cpu.gpr.rsi = 0xB2B2B2B2ull;
        cpu.gpr.rdi = 0xB3B3B3B3ull;
        cpu.gpr.r12 = 0xC2;
        cpu.gpr.r13 = 0xC3;
        cpu.gpr.r14 = 0xC4;
        cpu.gpr.r15 = 0xC5;
        for (int x = 6; x < 16; ++x) {
            cpu.sse.xmm[x].u64_[0] = 0xEEEE0000ull + static_cast<u64>(x);
            cpu.sse.xmm[x].u64_[1] = 0xFFFF0000ull + static_cast<u64>(x);
        }
        // Control bits changed (rounding down) and a status bit (PE) set.
        cpu.sse.mxcsr = 0x3FA0u;
    }
    if (g_probe.set_host_last_error) {
        // Host code inside a COM method that set the host copy (for
        // example a kernel32 helper it used): not the guest's last error.
        winapi::set_last_error(0x1234u);
    }
    if (g_probe.transfer_to) {
        cpu.rip = g_probe.transfer_rip;
        cpu.thunk_control_transfer = true;
    }
    if (g_probe.raise) cpu.pending_raise.active = true;
    if (g_probe.unwind) cpu.pending_unwind.active = true;
    cpu.gpr.rax = 0x700u + slot;
}

/// A direct entry that only counts (the nested crossing's target).
u64 g_nested_direct_calls = 0;
void nested_handler(void*, winapi::Marshaler& m, u32) {
    ++g_nested_direct_calls;
    cpu::CpuState& cpu = *m.cpu();
    cpu.gpr.rbx = 0xDEAD;  // its own crossing must restore this
    cpu.gpr.rax = 0x55;
}

class DirectThunkTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_probe = HandlerProbe{};
        g_nested_direct_calls = 0;
        winapi::com_direct::force_for_test(winapi::com_direct::Force::On);
        set_direct_thunk_counting(true);
        winapi::ThunkRegistry::count_resolve_handler_calls(true);
        ASSERT_TRUE(memory.map_region(kMemBase, kMemSize,
                                      MemoryProtection::ReadWrite).is_ok());
        winapi::DllExports exports;
        exports.entries.push_back(winapi::make_direct_export(
            "ProbeMethod", winapi::DirectHandler{&probe_handler, nullptr, 3},
            12));  // three x86 argument slots
        exports.entries.push_back(winapi::make_direct_export(
            "SixArgMethod", winapi::DirectHandler{&probe_handler, nullptr, 4},
            24));  // six x86 slots: x64 has two stack arguments
        exports.entries.push_back(winapi::make_direct_export(
            "NestedMethod", winapi::DirectHandler{&nested_handler, nullptr, 9},
            4));
        exports.entries.push_back(winapi::ExportEntry{
            "PlainExport", 1, [](winapi::Marshaler& m) { m.set_return(1); }});
        ASSERT_TRUE(registry.register_dll("probe.dll", std::move(exports))
                        .is_ok());
    }
    void TearDown() override {
        winapi::com_direct::force_for_test(winapi::com_direct::Force::None);
        set_direct_thunk_counting(false);
        winapi::ThunkRegistry::count_resolve_handler_calls(false);
    }

    GuestAddr gateway(const char* name) {
        const auto entry = registry.get_thunk_address("probe.dll", name);
        EXPECT_TRUE(entry.is_ok()) << name;
        return entry.value() + winapi::ThunkRegistry::STUB_SIZE;
    }

    GuestMemory memory;
    winapi::ThunkRegistry registry{kThunkBase};
};

}  // namespace

TEST_F(DirectThunkTest, TableAnswersOnlyGatewaysOfExportsWithADirectEntry) {
    const GuestAddr probe = gateway("ProbeMethod");
    const winapi::DirectThunk* direct = registry.direct_thunk(probe);
    ASSERT_NE(direct, nullptr);
    EXPECT_EQ(direct->handler.fn, &probe_handler);
    EXPECT_EQ(direct->handler.slot, 3u);
    EXPECT_EQ(direct->stdcall_bytes, 12u);
    // The guest-visible entry stub is not a gateway; a plain export has no
    // direct entry; an address outside the slab is nothing.
    EXPECT_EQ(registry.direct_thunk(probe - winapi::ThunkRegistry::STUB_SIZE),
              nullptr);
    EXPECT_EQ(registry.direct_thunk(gateway("PlainExport")), nullptr);
    EXPECT_EQ(registry.direct_thunk(kThunkBase - 0x100), nullptr);
    // The export's handler IS its direct entry (make_direct_export).
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.set_sp(kMemBase + 0x8000);
    winapi::Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto handler = registry.resolve_handler(probe);
    ASSERT_TRUE(handler.is_ok());
    g_probe.clobber = false;
    (*handler.value())(marshaler);
    EXPECT_EQ(g_probe.calls, 1u);
    EXPECT_EQ(cpu.gpr.rax, 0x703u);
}

TEST_F(DirectThunkTest, X64CrossingRestoresNonvolatilesAndReadsTheStackOnce) {
    constexpr u64 kSp = kMemBase + 0x8000;
    constexpr u64 kRet = 0x0000000140001234ull;
    ASSERT_TRUE(memory.write_u64(kSp, kRet).is_ok());
    ASSERT_TRUE(memory.write_u64(kSp + 0x28, 0x5555555555555555ull).is_ok());
    ASSERT_TRUE(memory.write_u64(kSp + 0x30, 0x6666666666666666ull).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.rip = gateway("SixArgMethod");
    cpu.set_sp(kSp);
    cpu.gpr.rcx = 0x11;
    cpu.gpr.rdx = 0x22;
    cpu.gpr.r8 = 0x33;
    cpu.gpr.r9 = 0x44;
    cpu.gpr.rbx = 1;
    cpu.gpr.rbp = 2;
    cpu.gpr.rsi = 3;
    cpu.gpr.rdi = 4;
    cpu.gpr.r12 = 12;
    cpu.gpr.r13 = 13;
    cpu.gpr.r14 = 14;
    cpu.gpr.r15 = 15;
    for (int x = 0; x < 16; ++x) {
        cpu.sse.xmm[x].u64_[0] = 0x1000u + static_cast<u64>(x);
        cpu.sse.xmm[x].u64_[1] = 0x2000u + static_cast<u64>(x);
    }
    cpu.sse.mxcsr = 0x1F80u;
    winapi::Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    winapi::ThreadSuspendRegistry::PublishGuard guard(0x7101, &cpu);

    const winapi::DirectThunk* direct = registry.direct_thunk(cpu.rip);
    ASSERT_NE(direct, nullptr);
    const u64 resolves = winapi::ThunkRegistry::resolve_handler_calls_this_thread();
    const u64 crossings = direct_thunk_crossings_this_thread();
    EXPECT_EQ(run_direct_thunk(*direct, cpu, memory, marshaler, guard, nullptr),
              DirectThunkResult::Returned);

    // The handler saw the Win64 arguments: four registers, then [rsp+0x28].
    EXPECT_EQ(g_probe.calls, 1u);
    EXPECT_EQ(g_probe.args[0], 0x11u);
    EXPECT_EQ(g_probe.args[3], 0x44u);
    EXPECT_EQ(g_probe.args[4], 0x5555555555555555ull);
    EXPECT_EQ(g_probe.args[5], 0x6666666666666666ull);
    EXPECT_EQ(g_probe.return_address, kRet);
    // The general dispatch was never consulted; one direct crossing ran.
    EXPECT_EQ(winapi::ThunkRegistry::resolve_handler_calls_this_thread(),
              resolves);
    EXPECT_EQ(direct_thunk_crossings_this_thread(), crossings + 1);
    // RET: x64 pops only the return address.
    EXPECT_EQ(cpu.rip, kRet);
    EXPECT_EQ(cpu.sp(), kSp + 8);
    EXPECT_EQ(cpu.gpr.rax, 0x704u);
    // Callee-saved state is the caller's again.
    EXPECT_EQ(cpu.gpr.rbx, 1u);
    EXPECT_EQ(cpu.gpr.rbp, 2u);
    EXPECT_EQ(cpu.gpr.rsi, 3u);
    EXPECT_EQ(cpu.gpr.rdi, 4u);
    EXPECT_EQ(cpu.gpr.r12, 12u);
    EXPECT_EQ(cpu.gpr.r13, 13u);
    EXPECT_EQ(cpu.gpr.r14, 14u);
    EXPECT_EQ(cpu.gpr.r15, 15u);
    for (int x = 6; x < 16; ++x) {
        EXPECT_EQ(cpu.sse.xmm[x].u64_[0], 0x1000u + static_cast<u64>(x)) << x;
        EXPECT_EQ(cpu.sse.xmm[x].u64_[1], 0x2000u + static_cast<u64>(x)) << x;
    }
    // MXCSR: control (bits 6-15) restored, status (bits 0-5) kept.
    EXPECT_EQ(cpu.sse.mxcsr & 0xFFC0u, 0x1F80u);
    EXPECT_EQ(cpu.sse.mxcsr & 0x3Fu, 0x20u);
    // The handler's stack window does not outlive the crossing.
    EXPECT_EQ(marshaler.stack_window().bytes, nullptr);
}

TEST_F(DirectThunkTest, X86CrossingPopsStdcallBytesAndRestoresCalleeSaved) {
    constexpr u32 kSp = kMemBase + 0x8000;
    constexpr u32 kRet = 0x00401234u;
    std::array<u8, 0x400> paint{};
    paint.fill(0xA5);
    ASSERT_TRUE(memory.write_bytes(kSp - 0x300, paint.data(), paint.size())
                    .is_ok());
    ASSERT_TRUE(memory.write_u32(kSp, kRet).is_ok());
    ASSERT_TRUE(memory.write_u32(kSp + 4, 0xA1A1A1A1u).is_ok());
    ASSERT_TRUE(memory.write_u32(kSp + 8, 0xA2A2A2A2u).is_ok());
    ASSERT_TRUE(memory.write_u32(kSp + 12, 0xA3A3A3A3u).is_ok());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.rip = gateway("ProbeMethod");
    cpu.set_sp(kSp);
    cpu.gpr.rbx = 1;
    cpu.gpr.rbp = 2;
    cpu.gpr.rsi = 3;
    cpu.gpr.rdi = 4;
    winapi::Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    winapi::ThreadSuspendRegistry::PublishGuard guard(0x7102, &cpu);
    const winapi::DirectThunk* direct = registry.direct_thunk(cpu.rip);
    ASSERT_NE(direct, nullptr);
    EXPECT_EQ(run_direct_thunk(*direct, cpu, memory, marshaler, guard, nullptr),
              DirectThunkResult::Returned);

    EXPECT_EQ(g_probe.args[0], 0xA1A1A1A1u);
    EXPECT_EQ(g_probe.args[1], 0xA2A2A2A2u);
    EXPECT_EQ(g_probe.args[2], 0xA3A3A3A3u);
    EXPECT_EQ(g_probe.return_address, kRet);
    // stdcall: the callee pops the return address and its 12 argument bytes.
    EXPECT_EQ(cpu.rip, kRet);
    EXPECT_EQ(cpu.sp(), kSp + 4 + 12);
    EXPECT_EQ(cpu.gpr.rbx & 0xFFFFFFFFu, 1u);
    EXPECT_EQ(cpu.gpr.rbp & 0xFFFFFFFFu, 2u);
    EXPECT_EQ(cpu.gpr.rsi & 0xFFFFFFFFu, 3u);
    EXPECT_EQ(cpu.gpr.rdi & 0xFFFFFFFFu, 4u);
    // The full path's x86 callee-stack contract (thunk_fast_path.hpp):
    // 0x200 bytes below ESP are written, [ESP] and above are not.
    EXPECT_EQ(memory.read_u32(kSp - 0x100).value_or(0xFFFFFFFFu), 0u);
    EXPECT_EQ(memory.read_u32(kSp - 0x204).value_or(0), 0xA5A5A5A5u);
    EXPECT_EQ(memory.read_u32(kSp + 4).value_or(0), 0xA1A1A1A1u);
}

namespace {

/// The lines of an oracle .expected file whose method is @p name.
std::string oracle_line(const char* file, const std::string& name) {
    std::ifstream in(std::string(VITREO_ORACLE_PROBE_DIR) + "/" + file);
    EXPECT_TRUE(in.good()) << file;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(name + " ", 0) == 0) return line;
    }
    ADD_FAILURE() << name << " not in " << file;
    return {};
}

}  // namespace

TEST_F(DirectThunkTest, CrossingLeavesTheTebLastErrorAsWindowsDoes) {
    // Measured: these D3D11 COM methods -- a failing one among them -- leave
    // the caller's last error exactly as it was, on x64 and x86.
    for (const char* file : {"d3d11_com_last_error_probe64.expected",
                             "d3d11_com_last_error_probe32.expected"}) {
        for (const char* method :
             {"Context::IASetVertexBuffers", "Context::DrawIndexed",
              "Context::Map(default, WRITE)", "Device::CreateBuffer(ByteWidth=0)",
              "SRV::Release", "SwapChain::Present(0,0)",
              "[more2] Context::GetData(DONOTFLUSH, pending)",
              "[more2] Context::CSSetUnorderedAccessViews"}) {
            const std::string line = oracle_line(file, method);
            EXPECT_NE(line.find("le=kept"), std::string::npos) << line;
        }
    }
    for (const bool is64 : {true, false}) {
        constexpr u64 kSp = kMemBase + 0x8000;
        ASSERT_TRUE(memory.write_u64(kSp, 0x401000).is_ok());
        cpu::CpuState cpu;
        if (is64) {
            cpu.init_long_mode();
            cpu.seg.gs_desc.base = kTebBase;
        } else {
            cpu.init_protected_mode();
            cpu.seg.fs_desc.base = kTebBase;
        }
        const GuestAddr last_error = kTebBase + (is64 ? 0x68 : 0x34);
        ASSERT_TRUE(memory.write_u32(last_error, 0x20000510u).is_ok());
        winapi::set_last_error(0x99u);  // a stale host copy
        cpu.rip = gateway("ProbeMethod");
        cpu.set_sp(kSp);
        winapi::Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        winapi::ThreadSuspendRegistry::PublishGuard guard(0x7103, &cpu);
        g_probe.set_host_last_error = true;
        ASSERT_EQ(run_direct_thunk(*registry.direct_thunk(cpu.rip), cpu, memory,
                                   marshaler, guard, nullptr),
                  DirectThunkResult::Returned);
        EXPECT_EQ(memory.read_u32(last_error).value_or(0), 0x20000510u)
            << (is64 ? "x64" : "x86");
    }
}

TEST_F(DirectThunkTest, ControlTransferUnwindAndRaiseExitAsTheFullPathDoes) {
    constexpr u64 kSp = kMemBase + 0x8000;
    ASSERT_TRUE(memory.write_u64(kSp, 0x140009999ull).is_ok());
    const auto run = [&](cpu::CpuState& cpu) {
        cpu.init_long_mode();
        cpu.rip = gateway("ProbeMethod");
        cpu.set_sp(kSp);
        cpu.gpr.rbx = 1;
        winapi::Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        winapi::ThreadSuspendRegistry::PublishGuard guard(0x7104, &cpu);
        return run_direct_thunk(*registry.direct_thunk(cpu.rip), cpu, memory,
                                marshaler, guard, nullptr);
    };
    {
        // longjmp-style transfer: rip and the registers are the handler's.
        cpu::CpuState cpu;
        g_probe.transfer_to = true;
        g_probe.transfer_rip = 0x140007777ull;
        EXPECT_EQ(run(cpu), DirectThunkResult::ControlTransferred);
        EXPECT_EQ(cpu.rip, 0x140007777ull);
        EXPECT_EQ(cpu.sp(), kSp);
        EXPECT_EQ(cpu.gpr.rbx, 0xB0B0B0B0ull);
        EXPECT_FALSE(cpu.thunk_control_transfer);
        g_probe.transfer_to = false;
    }
    {
        // RtlUnwind in flight: nothing restored, no RET.
        cpu::CpuState cpu;
        g_probe.unwind = true;
        EXPECT_EQ(run(cpu), DirectThunkResult::Unwinding);
        EXPECT_EQ(cpu.gpr.rbx, 0xB0B0B0B0ull);
        EXPECT_EQ(cpu.sp(), kSp);
        EXPECT_TRUE(cpu.pending_unwind.active);
        g_probe.unwind = false;
    }
    {
        // RaiseException: registers restored, rip left at the gateway.
        cpu::CpuState cpu;
        g_probe.raise = true;
        EXPECT_EQ(run(cpu), DirectThunkResult::Raising);
        EXPECT_EQ(cpu.rip, gateway("ProbeMethod"));
        EXPECT_EQ(cpu.sp(), kSp);
        EXPECT_EQ(cpu.gpr.rbx, 1u);
        g_probe.raise = false;
    }
}

TEST_F(DirectThunkTest, ReentrantGuestCallFromTheHandlerResumesTheCrossing) {
    // The direct path, which a process-wide diagnostic switches off by
    // design (/tmp/vitreo-hostcall-ring or an inherited trace variable).
    if (const char* off = direct_crossing_blocker()) {
        GTEST_SKIP() << "direct crossing off in this process: " << off;
    }
    // The guest callback: clobbers RBX and XMM6, crosses into a second
    // direct export, returns 0x77. x64:
    //   mov rbx, 0xBAD ; mov rax, <gateway> ; call rax ; pxor xmm6, xmm6
    //   mov eax, 0x77 ; ret
    const GuestAddr nested = gateway("NestedMethod");
    std::vector<u8> code{0x48, 0xC7, 0xC3, 0xAD, 0x0B, 0x00, 0x00,  // mov rbx
                         0x48, 0xB8};
    for (int i = 0; i < 8; ++i) code.push_back(static_cast<u8>(nested >> (8 * i)));
    code.insert(code.end(), {0xFF, 0xD0,                    // call rax
                             0x66, 0x0F, 0xEF, 0xF6,        // pxor xmm6,xmm6
                             0xB8, 0x77, 0x00, 0x00, 0x00,  // mov eax, 0x77
                             0xC3});
    constexpr GuestAddr kCode = kMemBase + 0x1000;
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()).is_ok());

    constexpr u64 kSp = kMemBase + 0x8000;
    constexpr u64 kRet = 0x0000000140001234ull;
    ASSERT_TRUE(memory.write_u64(kSp, kRet).is_ok());
    ASSERT_TRUE(memory.write_u64(kSp + 0x28, 0x5555ull).is_ok());
    ASSERT_TRUE(memory.write_u64(kSp + 0x30, 0x6666ull).is_ok());

    winapi::ProcessManager process_mgr;
    // Warm the callback loop first: its first use on a thread resolves the
    // fast-thunk set (resolve_fast_thunks asks resolve_handler for fgetc and
    // ungetc), which is setup, not a crossing.
    {
        constexpr GuestAddr kRetOnly = kMemBase + 0x2000;
        const u8 ret = 0xC3;
        ASSERT_TRUE(memory.write_bytes(kRetOnly, &ret, 1).is_ok());
        cpu::CpuState warm;
        warm.init_long_mode();
        warm.set_sp(kMemBase + 0x6000);
        bool warm_completed = false;
        (void)run_guest_callback(warm, memory, registry, process_mgr, kRetOnly,
                                 nullptr, 0, true, &warm_completed);
        ASSERT_TRUE(warm_completed);
    }
    g_probe.memory = &memory;
    g_probe.registry = &registry;
    g_probe.process = &process_mgr;
    g_probe.nested_code = kCode;

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.rip = gateway("SixArgMethod");
    cpu.set_sp(kSp);
    cpu.gpr.rbx = 1;
    cpu.sse.xmm[6].u64_[0] = 0x6060;
    winapi::Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    winapi::ThreadSuspendRegistry::PublishGuard guard(0x7105, &cpu);
    const u64 resolves = winapi::ThunkRegistry::resolve_handler_calls_this_thread();
    const u64 crossings = direct_thunk_crossings_this_thread();
    EXPECT_EQ(run_direct_thunk(*registry.direct_thunk(cpu.rip), cpu, memory,
                               marshaler, guard, nullptr),
              DirectThunkResult::Returned);

    // The guest callback ran to completion, its own crossing direct.
    EXPECT_TRUE(g_probe.nested_completed);
    EXPECT_EQ(g_probe.nested_result & 0xFFFFFFFFu, 0x77u);
    EXPECT_EQ(g_nested_direct_calls, 1u);
    EXPECT_EQ(direct_thunk_crossings_this_thread(), crossings + 2);
    EXPECT_EQ(winapi::ThunkRegistry::resolve_handler_calls_this_thread(),
              resolves);
    // The outer method still reads its own arguments after the callback.
    EXPECT_EQ(g_probe.args_after_nested[4], 0x5555u);
    EXPECT_EQ(g_probe.args_after_nested[5], 0x6666u);
    // And the outer crossing returns to ITS caller with its state intact.
    EXPECT_EQ(cpu.rip, kRet);
    EXPECT_EQ(cpu.sp(), kSp + 8);
    EXPECT_EQ(cpu.gpr.rbx, 1u);
    EXPECT_EQ(cpu.sse.xmm[6].u64_[0], 0x6060u);
}

TEST_F(DirectThunkTest, StackWindowFallsBackLikeResolveForRead) {
    // x86, the arguments' page unreadable (what a guard page is to guest
    // memory): no window is taken, each slot is read on its own -- the
    // readable return address works, the unreadable argument reads 0 -- as
    // the full path's per-slot reads always did.
    constexpr u32 kPage = 0x1000;
    constexpr u32 kSp = kMemBase + 0x9000 - 8;  // ret + arg0 on one page
    ASSERT_TRUE(memory.write_u32(kSp, 0x00402000u).is_ok());
    ASSERT_TRUE(memory.write_u32(kSp + 4, 0xA1A1A1A1u).is_ok());
    ASSERT_TRUE(memory.write_u32(kSp + 8, 0xA2A2A2A2u).is_ok());
    ASSERT_TRUE(memory.protect_region(kMemBase + 0x9000, kPage,
                                      MemoryProtection::None).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.rip = gateway("ProbeMethod");
    cpu.set_sp(kSp);
    winapi::Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    winapi::ThreadSuspendRegistry::PublishGuard guard(0x7106, &cpu);
    g_probe.clobber = false;
    EXPECT_EQ(run_direct_thunk(*registry.direct_thunk(cpu.rip), cpu, memory,
                               marshaler, guard, nullptr),
              DirectThunkResult::Returned);
    EXPECT_EQ(g_probe.args[0], 0xA1A1A1A1u);
    EXPECT_EQ(g_probe.args[1], 0u);
    EXPECT_EQ(cpu.rip, 0x00402000u);
    EXPECT_EQ(cpu.sp(), kSp + 4 + 12);
    ASSERT_TRUE(memory.protect_region(kMemBase + 0x9000, kPage,
                                      MemoryProtection::ReadWrite).is_ok());

    // A window split across two separate mappings: every slot readable,
    // the one-access read refused, the values the same.
    GuestMemory split;
    ASSERT_TRUE(split.map_region(0x40000, 0x4000, MemoryProtection::ReadWrite)
                    .is_ok());
    ASSERT_TRUE(split.map_region(0x44000, 0x4000, MemoryProtection::ReadWrite)
                    .is_ok());
    constexpr u32 kSplitSp = 0x44000 - 8;
    ASSERT_TRUE(split.write_u32(kSplitSp, 0x00403000u).is_ok());
    ASSERT_TRUE(split.write_u32(kSplitSp + 4, 0xB1u).is_ok());
    ASSERT_TRUE(split.write_u32(kSplitSp + 8, 0xB2u).is_ok());
    ASSERT_TRUE(split.write_u32(kSplitSp + 12, 0xB3u).is_ok());
    cpu::CpuState split_cpu;
    split_cpu.init_protected_mode();
    split_cpu.rip = gateway("ProbeMethod");
    split_cpu.set_sp(kSplitSp);
    winapi::Marshaler split_marshaler(split);
    split_marshaler.attach_cpu(split_cpu);
    EXPECT_EQ(run_direct_thunk(*registry.direct_thunk(split_cpu.rip),
                               split_cpu, split, split_marshaler, guard,
                               nullptr),
              DirectThunkResult::Returned);
    EXPECT_EQ(g_probe.args[0], 0xB1u);
    EXPECT_EQ(g_probe.args[1], 0xB2u);
    EXPECT_EQ(g_probe.args[2], 0xB3u);
    EXPECT_EQ(split_cpu.rip, 0x00403000u);
}

TEST_F(DirectThunkTest, SwitchOffSendsEveryCrossingDownTheFullPath) {
    // The direct path, which a process-wide diagnostic switches off by
    // design (/tmp/vitreo-hostcall-ring or an inherited trace variable).
    if (const char* off = direct_crossing_blocker()) {
        GTEST_SKIP() << "direct crossing off in this process: " << off;
    }
    const GuestAddr probe = gateway("ProbeMethod");
    EXPECT_NE(direct_thunk_for(registry, probe, false), nullptr);
    // A slow-hostcall threshold is a diagnostic of the full path.
    EXPECT_EQ(direct_thunk_for(registry, probe, true), nullptr);
    winapi::com_direct::force_for_test(winapi::com_direct::Force::Off);
    EXPECT_EQ(direct_thunk_for(registry, probe, false), nullptr);

    // The same guest call through the run loop, switch off: the full path
    // resolves the handler, which is the same body.
    const GuestAddr nested = gateway("NestedMethod");
    std::vector<u8> code{0x48, 0xB8};
    for (int i = 0; i < 8; ++i) code.push_back(static_cast<u8>(nested >> (8 * i)));
    code.insert(code.end(), {0xFF, 0xD0, 0xC3});  // call rax ; ret
    constexpr GuestAddr kCode = kMemBase + 0x3000;
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()).is_ok());
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.set_sp(kMemBase + 0x8000);
    cpu.gpr.rbx = 7;
    winapi::ProcessManager process_mgr;
    const u64 resolves = winapi::ThunkRegistry::resolve_handler_calls_this_thread();
    const u64 crossings = direct_thunk_crossings_this_thread();
    bool completed = false;
    EXPECT_EQ(run_guest_callback(cpu, memory, registry, process_mgr, kCode,
                                 nullptr, 0, true, &completed) & 0xFFu,
              0x55u);
    EXPECT_TRUE(completed);
    EXPECT_EQ(g_nested_direct_calls, 1u);
    EXPECT_EQ(direct_thunk_crossings_this_thread(), crossings);
    EXPECT_GT(winapi::ThunkRegistry::resolve_handler_calls_this_thread(),
              resolves);

    // Switch on: the run loop's own dispatch serves it directly.
    winapi::com_direct::force_for_test(winapi::com_direct::Force::On);
    const u64 resolves_on =
        winapi::ThunkRegistry::resolve_handler_calls_this_thread();
    EXPECT_EQ(run_guest_callback(cpu, memory, registry, process_mgr, kCode,
                                 nullptr, 0, true, &completed) & 0xFFu,
              0x55u);
    EXPECT_TRUE(completed);
    EXPECT_EQ(g_nested_direct_calls, 2u);
    EXPECT_EQ(direct_thunk_crossings_this_thread(), crossings + 1);
    EXPECT_EQ(winapi::ThunkRegistry::resolve_handler_calls_this_thread(),
              resolves_on);
}
