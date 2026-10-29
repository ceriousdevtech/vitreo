// SPDX-License-Identifier: LGPL-2.1-only
//
// L3-K: hot kernel32 / CRT exports served from translated code (in-chain),
// winapi/core/host_in_chain.hpp. Expected values come from the guest program
// and the Win32 contracts it exercises, never from a Vitreo run:
//   * TlsSetValue/TlsGetValue (Learn): the value set is the value got;
//     SetLastError/GetLastError round-trip through TEB.LastErrorValue
//     (x64 gs:[0x68], x86 fs:[0x34]), which guest code may also read and
//     write directly;
//   * GetCurrentThreadId is the calling thread's id;
//   * QueryPerformanceCounter returns nonzero;
//   * x86 stdcall: the callee pops its arguments; Win64: RBX RBP RDI RSI
//     R12-R15 are callee-saved.
// Each program runs three ways -- in-chain, the dispatcher crossing (no
// in-chain stub), the interpreter -- and must end in the same state.

#include <gtest/gtest.h>

#include "app/cli/commands/dll_registry.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "runtime/execution/guest_run_loop.hpp"
#include "runtime/execution/thunk_direct.hpp"
#include "runtime/execution/thunk_fast_path.hpp"
#include "runtime/execution/thunk_inchain.hpp"
#include "runtime/process/runtime_context.hpp"
#include "winapi/core/com_direct.hpp"
#include "winapi/core/host_in_chain.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/kernel32_in_chain.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/synchronization/critical_section.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/msvcrt/guest_accel.hpp"
#include "winapi/msvcrt/math/crt_math.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"

#include <vitreo/ab_cycle.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::cli;
using vitreo::cpu::jit::JitCompiler;
using vitreo::cpu::jit::InChainThunkResult;
using vitreo::winapi::InChainClass;

namespace {

// Its own address range: the JIT's SMC churn record is process-wide and
// keyed by guest address, so code placed where another suite's
// self-modifying test ran (tests/cli/test_inchain_thunk.cpp uses
// 0x20000-0x30000) could start out interpreted.
constexpr GuestAddr kMemBase = 0x40000;
constexpr usize kMemSize = 0x20000;
constexpr GuestAddr kCode = 0x41000;
constexpr GuestAddr kNested = 0x42000;  // the guest callback (AGuestCallback)
constexpr GuestAddr kIat = 0x43000;     // import slots, one per export
constexpr GuestAddr kData = 0x44000;    // QPC output, x86 accumulators
constexpr GuestAddr kTeb = 0x46000;     // TEB (TLS slots at +0x1480 / +0xE10)
constexpr GuestAddr kThunkBase = 0x70000000;
constexpr u32 kLoops = 40;
constexpr u32 kTlsIndex = 3;
constexpr u32 kErrMix = 0x55;           // SetEvent's synthetic last error
constexpr u32 kSp64 = 0x4FF08;
constexpr u32 kSp32 = 0x4FF00;

/// The exports, in import-slot order.
enum Import : u32 {
    kTlsSetValue,
    kTlsGetValue,
    kSetLastError,
    kGetLastError,
    kGetCurrentThreadId,
    kQueryPerformanceCounter,
    kSetEvent,
    kResetEvent,
    kTryAcquireSrwShared,
    kMathProbe,
    kImportCount
};

/// What the handlers saw. The fast-set exports' handlers must never run:
/// their inline bodies serve every arm.
struct Probe {
    u64 fast_set_handler_calls{0};
    u64 set_event_calls{0};
    u64 reset_event_calls{0};
    u64 srw_handler_calls{0};
    u64 math_calls{0};
    u64 nested_on_call{0};
    u64 nested_result{0};
    vitreo::GuestMemory* memory{nullptr};
    winapi::ThunkRegistry* registry{nullptr};
    winapi::ProcessManager* process{nullptr};
};
Probe* g_probe = nullptr;

/// Map the registry's thunk slab with every slot's unmodified NOP stub.
void map_thunk_slab(vitreo::GuestMemory& memory, const winapi::ThunkRegistry& registry) {
    const GuestAddr base = registry.thunk_base();
    const usize size = 0x1000;
    ASSERT_TRUE(memory.map_region(base, size, MemoryProtection::ReadWrite)
                    .is_ok());
    std::vector<u8> slab(size, 0);
    for (usize slot = 0; slot + winapi::ThunkRegistry::STUB_SIZE <= size;
         slot += 16) {
        std::fill_n(slab.begin() + static_cast<std::ptrdiff_t>(slot),
                    winapi::ThunkRegistry::STUB_SIZE, u8{0x90});
    }
    ASSERT_TRUE(memory.write_bytes(base, slab.data(), slab.size()).is_ok());
}

struct Code {
    GuestAddr base;
    std::vector<u8> bytes;
    explicit Code(GuestAddr at) : base(at) {}
    [[nodiscard]] GuestAddr here() const { return base + bytes.size(); }
    void raw(std::initializer_list<u8> b) { bytes.insert(bytes.end(), b); }
    void u32le(u32 v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<u8>(v >> (8 * i)));
    }
    void u64le(u64 v) {
        for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<u8>(v >> (8 * i)));
    }
    void rel32_to(GuestAddr target) {
        u32le(static_cast<u32>(static_cast<i64>(target) -
                               static_cast<i64>(here() + 4)));
    }
    /// x64: call [rip+disp32] to import @p i.   x86: call [abs32].
    void call_import(bool is64, Import i) { call_slot(is64, i); }
    /// The same through IAT slot @p slot.
    void call_slot(bool is64, u32 slot) {
        const GuestAddr at = kIat + slot * (is64 ? 8u : 4u);
        raw({0xFF, 0x15});
        if (is64) rel32_to(at); else u32le(static_cast<u32>(at));
    }
    /// inc dword [abs32] (x64: SIB, no base; x86: disp32).
    void inc_mem(bool is64, GuestAddr at) {
        if (is64) raw({0xFF, 0x04, 0x25}); else raw({0xFF, 0x05});
        u32le(static_cast<u32>(at));
    }
    /// jnz rel32 back to @p target.
    void jnz_to(GuestAddr target) {
        raw({0x0F, 0x85});
        rel32_to(target);
    }
    /// A short Jcc (@p op) whose rel8 patch8 fills in later.
    [[nodiscard]] usize jcc8(u8 op) {
        raw({op, 0x00});
        return bytes.size() - 1;
    }
    void patch8(usize at) {
        bytes[at] = static_cast<u8>(bytes.size() - (at + 1));
    }
    /// x64: sub rsp, 0x28 (the callee's shadow space).  x86: nothing.
    void prologue(bool is64) {
        if (is64) raw({0x48, 0x83, 0xEC, 0x28});
    }
    /// x64: add rsp, 0x28; ret.  x86: ret.
    void epilogue(bool is64) {
        if (is64) raw({0x48, 0x83, 0xC4, 0x28});
        raw({0xC3});
    }
    void mov_ebx(u32 v) { raw({0xBB}); u32le(v); }
    /// dec ebx (sets ZF).
    void dec_ebx(bool is64) {
        if (is64) raw({0xFF, 0xCB}); else raw({0x4B});
    }
    /// The first integer argument: x64 mov ecx, imm32; x86 push imm32.
    void arg0(bool is64, u64 v) {
        raw({static_cast<u8>(is64 ? 0xB9 : 0x68)});
        u32le(static_cast<u32>(v));
    }
    /// mov dword [abs32], imm32.
    void mov_mem(bool is64, GuestAddr at, u32 v) {
        if (is64) raw({0xC7, 0x04, 0x25}); else raw({0xC7, 0x05});
        u32le(static_cast<u32>(at));
        u32le(v);
    }
    /// cmp dword [abs32], 0; je rel32 back to @p target.
    void loop_while_zero(bool is64, GuestAddr at, GuestAddr target) {
        if (is64) raw({0x83, 0x3C, 0x25}); else raw({0x83, 0x3D});
        u32le(static_cast<u32>(at));
        raw({0x00, 0x0F, 0x84});
        rel32_to(target);
    }
};

enum class Arm { InChain, Dispatcher, Interpreter };

struct Outcome {
    RunResult result;
    cpu::CpuState cpu;
    u64 host_served{0};
    u64 offers{0};  // stub call-outs, served or declined
    u64 resumed{0};
    u64 left{0};
    u64 broke_outer{0};
    u32 tid{0};
    u32 acc[4]{};  // x86 memory accumulators at kData + 8
};

class InChainKernel32Test : public ::testing::Test {
protected:
    void SetUp() override {
        winapi::host_in_chain::force_for_test(winapi::host_in_chain::Force::On);
        // No process managers here: the critical-section and SRW fast bodies
        // step aside (TryAcquireSRWLockShared exercises exactly that). The
        // process-wide bindings are put back in TearDown.
        saved_cs_mgr_ = fast_thunk_cs_manager();
        saved_sync_mgr_ = fast_thunk_sync_manager();
        set_fast_thunk_cs_manager(nullptr);
        set_fast_thunk_sync_manager(nullptr);
        set_in_chain_crossing_counting(true);
        JitCompiler::set_inchain_counting_for_test(true);
        saved_tid_ = winapi::ThreadManager::get_current_thread_id_static();
        // These tests measure the in-chain path, which a process-wide
        // diagnostic switches off by design (/tmp/vitreo-hostcall-ring or an
        // inherited trace variable): name it instead of failing wholesale.
        if (const char* off = in_chain_full_path_diagnostic()) {
            GTEST_SKIP() << "in-chain crossings off in this process: " << off;
        }
    }
    void TearDown() override {
        set_fast_thunk_cs_manager(saved_cs_mgr_);
        set_fast_thunk_sync_manager(saved_sync_mgr_);
        winapi::ThreadManager::set_current_thread_id(saved_tid_);
        winapi::com_direct::force_for_test(winapi::com_direct::Force::None);
        winapi::host_in_chain::force_for_test(
            winapi::host_in_chain::Force::None);
        set_in_chain_crossing_counting(false);
        JitCompiler::set_inchain_counting_for_test(false);
        JitCompiler::set_inchain_thunks_codegen_for_test(-1);
        JitCompiler::set_jit_hot_threshold_for_test(0);
        diagnostics::ab_cycle_force_for_test(diagnostics::AbForce::None);
        g_probe = nullptr;
    }

    static void fast_set_handler(winapi::Marshaler& m) {
        ++g_probe->fast_set_handler_calls;
        m.set_return(0);
    }

    winapi::CriticalSectionManager* saved_cs_mgr_{nullptr};
    winapi::SyncManager* saved_sync_mgr_{nullptr};
    u32 saved_tid_{0};

    /// The exports, classified by the kernel32 table (by name) exactly as
    /// dll_registry.cpp classifies the real ones. SetEvent/ResetEvent have
    /// synthetic bodies that make the last-error sync observable:
    ///   SetEvent(h):   SetLastError(h ^ kErrMix), returns 3 * h;
    ///   ResetEvent(h): returns the last error it finds (the guest wrote it
    ///                  to the TEB directly just before the call).
    static winapi::DllExports kernel32_exports() {
        winapi::DllExports exports;
        auto add = [&](const char* name, u16 stdcall_bytes,
                       winapi::ApiHandler handler) {
            exports.entries.push_back(winapi::ExportEntry{
                name, 0, std::move(handler), stdcall_bytes});
        };
        add("TlsSetValue", 8, &fast_set_handler);
        add("TlsGetValue", 4, &fast_set_handler);
        add("SetLastError", 4, &fast_set_handler);
        add("GetLastError", 0, &fast_set_handler);
        add("GetCurrentThreadId", 0, &fast_set_handler);
        add("QueryPerformanceCounter", 4, &fast_set_handler);
        add("SetEvent", 4, [](winapi::Marshaler& m) {
            ++g_probe->set_event_calls;
            const u64 h = m.arg(0);
            winapi::set_last_error(static_cast<u32>(h) ^ kErrMix);
            m.set_return(3 * h);
        });
        add("ResetEvent", 4, [](winapi::Marshaler& m) {
            ++g_probe->reset_event_calls;
            m.set_return(winapi::get_last_error());
        });
        // A fast-set export whose inline body steps aside here (no
        // SyncManager is bound in this test): every arm runs the handler.
        add("TryAcquireSRWLockShared", 4, [](winapi::Marshaler& m) {
            ++g_probe->srw_handler_calls;
            m.set_return(1);
        });
        winapi::kernel32::apply_in_chain_classes(exports);
        return exports;
    }

    /// A CRT-style export of class AGuestCallback: returns its argument
    /// doubled and, on one call, runs a guest function (as a user _matherr
    /// would run) through run_guest_callback.
    static winapi::DllExports crt_exports() {
        winapi::DllExports exports;
        winapi::ExportEntry entry{"MathProbe", 0, [](winapi::Marshaler& m) {
            Probe& probe = *g_probe;
            const u64 call = ++probe.math_calls;
            if (call == probe.nested_on_call && probe.memory != nullptr) {
                bool completed = false;
                probe.nested_result = run_guest_callback(
                    *m.cpu(), *probe.memory, *probe.registry, *probe.process,
                    kNested, nullptr, 0, m.cpu()->is_64bit(), &completed);
            }
            m.set_return(2 * m.arg(0));
        }, 4};
        entry.in_chain = InChainClass::AGuestCallback;
        exports.entries.push_back(std::move(entry));
        return exports;
    }

    Outcome run(bool is64, Arm arm, Probe& probe) {
        g_probe = &probe;
        JitCompiler::set_inchain_thunks_codegen_for_test(
            arm == Arm::InChain ? 1 : 0);
        JitCompiler::set_jit_hot_threshold_for_test(
            arm == Arm::Interpreter ? JitCompiler::kJitHotThresholdMax : 1);

        vitreo::GuestMemory memory;
        EXPECT_TRUE(memory.map_region(kMemBase, kMemSize,
                                      MemoryProtection::ReadWrite).is_ok());
        winapi::ThunkRegistry registry{kThunkBase};
        EXPECT_TRUE(registry.register_dll("kernel32.dll", kernel32_exports())
                        .is_ok());
        EXPECT_TRUE(registry.register_dll("ucrtbase.dll", crt_exports())
                        .is_ok());
        map_thunk_slab(memory, registry);
        const char* const names[kImportCount] = {
            "TlsSetValue", "TlsGetValue", "SetLastError", "GetLastError",
            "GetCurrentThreadId", "QueryPerformanceCounter", "SetEvent",
            "ResetEvent", "TryAcquireSRWLockShared", "MathProbe"};
        for (u32 i = 0; i < kImportCount; ++i) {
            const char* dll = i == kMathProbe ? "ucrtbase.dll" : "kernel32.dll";
            const GuestAddr stub = registry.get_thunk_address(dll, names[i]).value();
            if (is64) {
                EXPECT_TRUE(memory.write_u64(kIat + i * 8, stub).is_ok());
            } else {
                EXPECT_TRUE(memory.write_u32(kIat + i * 4,
                                             static_cast<u32>(stub)).is_ok());
            }
        }
        // The guest callback: mov eax, 7; ret.
        const u8 nested[] = {0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3};
        EXPECT_TRUE(memory.write_bytes(kNested, nested, sizeof nested).is_ok());

        Code code(kCode);
        if (is64) build_x64(code); else build_x86(code);
        EXPECT_TRUE(memory.write_bytes(kCode, code.bytes.data(),
                                       code.bytes.size()).is_ok());

        Outcome out;
        cpu::CpuState& cpu = out.cpu;
        if (is64) {
            cpu.init_long_mode();
            cpu.seg.gs_desc.base = kTeb;
            cpu.set_sp(kSp64);
            EXPECT_TRUE(memory.write_u64(kSp64, PROCESS_RETURN_SENTINEL).is_ok());
        } else {
            cpu.init_protected_mode();
            cpu.seg.fs_desc.base = kTeb;
            cpu.set_sp(kSp32);
            EXPECT_TRUE(memory.write_u32(
                            kSp32, static_cast<u32>(PROCESS_RETURN_SENTINEL))
                            .is_ok());
        }
        cpu.rip = kCode;
        winapi::ProcessManager process_mgr;
        probe.memory = &memory;
        probe.registry = &registry;
        probe.process = &process_mgr;
        if (probe.nested_on_call != 0) {
            // Resolve the callback loop's fast-thunk set first (setup).
            const u8 ret = 0xC3;
            EXPECT_TRUE(memory.write_bytes(kNested + 0x100, &ret, 1).is_ok());
            cpu::CpuState warm;
            if (is64) warm.init_long_mode(); else warm.init_protected_mode();
            warm.set_sp(kMemBase + 0xC000);
            bool warm_completed = false;
            (void)run_guest_callback(warm, memory, registry, process_mgr,
                                     kNested + 0x100, nullptr, 0, is64,
                                     &warm_completed);
        }
        RunConfig config;
        config.max_instructions = 1'000'000;
        const u64 host0 = in_chain_host_crossings_this_thread();
        const u64 offers0 = in_chain_offers_this_thread();
        const u64 resumed0 = JitCompiler::inchain_resumed_this_thread();
        const u64 left0 = JitCompiler::inchain_left_this_thread();
        const u64 outer0 = JitCompiler::inchain_broke_outer_this_thread();
        out.result = run_execution_loop(cpu, memory, registry, process_mgr,
                                        config);
        out.host_served = in_chain_host_crossings_this_thread() - host0;
        out.offers = in_chain_offers_this_thread() - offers0;
        out.resumed = JitCompiler::inchain_resumed_this_thread() - resumed0;
        out.left = JitCompiler::inchain_left_this_thread() - left0;
        out.broke_outer = JitCompiler::inchain_broke_outer_this_thread() - outer0;
        out.tid = winapi::ThreadManager::get_current_thread_id_static();
        for (u32 i = 0; i < 4; ++i) {
            out.acc[i] = memory.read_u32(kData + 8 + 4 * i).value_or(0xDEAD);
        }
        probe.memory = nullptr;
        return out;
    }

    // x64, for (rbx = N; rbx; --rbx):
    //   r15 += TlsSetValue(3, rbx)           (1)   r14 += TlsGetValue(3)  (rbx)
    //   SetLastError(ebx); r13 += GetLastError()   (ebx)
    //   r12 += SetEvent(rbx)                 (3*rbx); esi += TEB+0x68 (ebx^0x55)
    //   TEB+0x68 = ebx + 1; ebp += ResetEvent(rbx) (ebx+1)
    //   edi = GetCurrentThreadId(); r15 += QueryPerformanceCounter(&data)
    //   r15 += TryAcquireSRWLockShared(&data) (1)  r14 += MathProbe(rbx) (2*rbx)
    static void build_x64(Code& c) {
        c.raw({0xBB}); c.u32le(kLoops);             // mov ebx, N
        for (u8 r : {0xFF, 0xF6, 0xED, 0xE4}) {     // xor r15d/r14d/r13d/r12d
            c.raw({0x45, 0x31, r});
        }
        c.raw({0x31, 0xF6});                        // xor esi, esi
        c.raw({0x31, 0xED});                        // xor ebp, ebp
        c.raw({0x48, 0x83, 0xEC, 0x28});            // sub rsp, 0x28 (shadow)
        const GuestAddr loop = c.here();
        auto ecx_imm = [&](u32 v) { c.raw({0xB9}); c.u32le(v); };
        ecx_imm(kTlsIndex);
        c.raw({0x48, 0x89, 0xDA});                  // mov rdx, rbx
        c.call_import(true, kTlsSetValue);
        c.raw({0x49, 0x01, 0xC7});                  // add r15, rax
        ecx_imm(kTlsIndex);
        c.call_import(true, kTlsGetValue);
        c.raw({0x49, 0x01, 0xC6});                  // add r14, rax
        c.raw({0x89, 0xD9});                        // mov ecx, ebx
        c.call_import(true, kSetLastError);
        c.call_import(true, kGetLastError);
        c.raw({0x49, 0x01, 0xC5});                  // add r13, rax
        c.raw({0x48, 0x89, 0xD9});                  // mov rcx, rbx
        c.call_import(true, kSetEvent);
        c.raw({0x49, 0x01, 0xC4});                  // add r12, rax
        // TEB.LastErrorValue read and written as guest code may, at the
        // TEB's linear address (gs base + 0x68).
        c.raw({0x8B, 0x04, 0x25}); c.u32le(static_cast<u32>(kTeb + 0x68));
        c.raw({0x01, 0xC6});                        // add esi, eax
        c.raw({0x8D, 0x43, 0x01});                  // lea eax, [rbx+1]
        c.raw({0x89, 0x04, 0x25}); c.u32le(static_cast<u32>(kTeb + 0x68));
        c.call_import(true, kResetEvent);
        c.raw({0x01, 0xC5});                        // add ebp, eax
        c.call_import(true, kGetCurrentThreadId);
        c.raw({0x89, 0xC7});                        // mov edi, eax
        c.raw({0x48, 0xB9}); c.u64le(kData);        // mov rcx, kData
        c.call_import(true, kQueryPerformanceCounter);
        c.raw({0x49, 0x01, 0xC7});                  // add r15, rax
        c.raw({0x48, 0xB9}); c.u64le(kData + 0x40); // mov rcx, &srw
        c.call_import(true, kTryAcquireSrwShared);
        c.raw({0x49, 0x01, 0xC7});                  // add r15, rax
        c.raw({0x48, 0x89, 0xD9});                  // mov rcx, rbx
        c.call_import(true, kMathProbe);
        c.raw({0x49, 0x01, 0xC6});                  // add r14, rax
        c.raw({0xFF, 0xCB});                        // dec ebx
        c.raw({0x0F, 0x85}); c.rel32_to(loop);      // jnz loop
        c.raw({0x48, 0x83, 0xC4, 0x28});            // add rsp, 0x28
        c.raw({0xC3});                              // ret to the sentinel
    }

    // x86, the same calls with stdcall arguments; the accumulators that do
    // not fit in registers live at kData + 8 (acc[0..3]):
    //   esi += TlsSetValue (1) + QPC (1) + TryAcquireSRWLockShared (1)
    //   acc0 += TlsGetValue (ebx) + MathProbe (2*ebx)
    //   acc1 += GetLastError (ebx);  acc2 += SetEvent (3*ebx)
    //   acc3 += TEB+0x34 after SetEvent (ebx^0x55);  ebp += ResetEvent (ebx+1)
    //   edi = GetCurrentThreadId
    static void build_x86(Code& c) {
        c.raw({0xBB}); c.u32le(kLoops);             // mov ebx, N
        c.raw({0x31, 0xF6});                        // xor esi, esi
        c.raw({0x31, 0xED});                        // xor ebp, ebp
        const GuestAddr loop = c.here();
        auto push_imm = [&](u32 v) { c.raw({0x68}); c.u32le(v); };
        auto add_acc = [&](u32 i) {                 // add [kData+8+4i], eax
            c.raw({0x01, 0x05}); c.u32le(static_cast<u32>(kData + 8 + 4 * i));
        };
        c.raw({0x53}); push_imm(kTlsIndex);         // push ebx; push 3
        c.call_import(false, kTlsSetValue);
        c.raw({0x01, 0xC6});                        // add esi, eax
        push_imm(kTlsIndex);
        c.call_import(false, kTlsGetValue);
        add_acc(0);
        c.raw({0x53});                              // push ebx
        c.call_import(false, kSetLastError);
        c.call_import(false, kGetLastError);
        add_acc(1);
        c.raw({0x53});                              // push ebx
        c.call_import(false, kSetEvent);
        add_acc(2);
        // TEB.LastErrorValue at the TEB's linear address (fs base + 0x34).
        c.raw({0xA1}); c.u32le(static_cast<u32>(kTeb + 0x34));  // mov eax, [..]
        add_acc(3);
        c.raw({0x8D, 0x43, 0x01});                  // lea eax, [ebx+1]
        c.raw({0xA3}); c.u32le(static_cast<u32>(kTeb + 0x34));  // mov [..], eax
        c.raw({0x53});                              // push ebx (hEvent)
        c.call_import(false, kResetEvent);
        c.raw({0x01, 0xC5});                        // add ebp, eax
        c.call_import(false, kGetCurrentThreadId);
        c.raw({0x89, 0xC7});                        // mov edi, eax
        push_imm(static_cast<u32>(kData));
        c.call_import(false, kQueryPerformanceCounter);
        c.raw({0x01, 0xC6});                        // add esi, eax
        push_imm(static_cast<u32>(kData + 0x40));
        c.call_import(false, kTryAcquireSrwShared);
        c.raw({0x01, 0xC6});                        // add esi, eax
        c.raw({0x53});                              // push ebx
        c.call_import(false, kMathProbe);
        add_acc(0);
        c.raw({0x4B});                              // dec ebx
        c.raw({0x0F, 0x85}); c.rel32_to(loop);      // jnz loop
        c.raw({0xC3});                              // ret to the sentinel
    }

    static u32 sum_to(u32 n) { return n * (n + 1) / 2; }
    static u32 sum_mixed(u32 n) {
        u32 s = 0;
        for (u32 k = 1; k <= n; ++k) s += k ^ kErrMix;
        return s;
    }

    /// The end state the program defines.
    static void expect_program_result(const Outcome& o, const Probe& p,
                                      bool is64, const char* arm) {
        SCOPED_TRACE(arm);
        ASSERT_TRUE(o.result.success) << o.result.error_message;
        const u32 n = kLoops;
        EXPECT_EQ(p.fast_set_handler_calls, 0u)
            << "a fast-set export reached its handler";
        EXPECT_EQ(p.set_event_calls, n);
        EXPECT_EQ(p.reset_event_calls, n);
        EXPECT_EQ(p.srw_handler_calls, n);
        EXPECT_EQ(p.math_calls, n);
        if (is64) {
            EXPECT_EQ(o.cpu.gpr.r15, 3u * n);           // TlsSet + QPC + SRW
            EXPECT_EQ(o.cpu.gpr.r14, 3u * sum_to(n));   // TlsGet + MathProbe
            EXPECT_EQ(o.cpu.gpr.r13, sum_to(n)) << "GetLastError";
            EXPECT_EQ(o.cpu.gpr.r12, 3u * sum_to(n));
            EXPECT_EQ(o.cpu.gpr.get32(6), sum_mixed(n))
                << "TEB.LastErrorValue not written back after the handler";
            EXPECT_EQ(o.cpu.gpr.get32(5), sum_to(n) + n)
                << "TEB.LastErrorValue not read into the handler";
            EXPECT_EQ(o.cpu.gpr.get32(7), o.tid);
            EXPECT_EQ(o.cpu.gpr.rbx, 0u);
            EXPECT_EQ(o.cpu.sp(), kSp64 + 8u);
        } else {
            EXPECT_EQ(o.cpu.gpr.get32(6), 3u * n);      // ESI
            EXPECT_EQ(o.acc[0], 3u * sum_to(n));
            EXPECT_EQ(o.acc[1], sum_to(n)) << "GetLastError";
            EXPECT_EQ(o.acc[2], 3u * sum_to(n));
            EXPECT_EQ(o.acc[3], sum_mixed(n))
                << "TEB.LastErrorValue not written back after the handler";
            EXPECT_EQ(o.cpu.gpr.get32(5), sum_to(n) + n)  // EBP
                << "TEB.LastErrorValue not read into the handler";
            EXPECT_EQ(o.cpu.gpr.get32(7), o.tid);       // EDI
            EXPECT_EQ(o.cpu.gpr.get32(3), 0u);          // EBX
            EXPECT_EQ(o.cpu.sp(), kSp32 + 4u) << "stdcall pops";
        }
    }

    static void expect_same_state(const Outcome& a, const Outcome& b,
                                  const char* what) {
        SCOPED_TRACE(what);
        for (u8 r = 0; r < 16; ++r) {
            EXPECT_EQ(a.cpu.gpr.get64(r), b.cpu.gpr.get64(r)) << "gpr " << int(r);
        }
        for (u32 i = 0; i < 4; ++i) EXPECT_EQ(a.acc[i], b.acc[i]) << i;
    }

    void parity(bool is64) {
        Probe p_in, p_disp, p_interp;
        const Outcome in = run(is64, Arm::InChain, p_in);
        const Outcome disp = run(is64, Arm::Dispatcher, p_disp);
        const Outcome interp = run(is64, Arm::Interpreter, p_interp);
        expect_program_result(in, p_in, is64, "in-chain");
        expect_program_result(disp, p_disp, is64, "dispatcher");
        expect_program_result(interp, p_interp, is64, "interpreter");
        expect_same_state(in, interp, "in-chain vs interpreter");
        expect_same_state(disp, interp, "dispatcher vs interpreter");
        // Per iteration, nine of the ten calls are L3-K exports served
        // in-chain once the loop is compiled (the tenth, whose fast body
        // steps aside, is declined to the dispatcher); they resume in
        // translated code.
        EXPECT_GE(in.host_served, 9u * (kLoops - 4));
        EXPECT_GE(in.resumed, 9u * (kLoops - 4));
        EXPECT_EQ(disp.host_served, 0u);
        EXPECT_EQ(interp.host_served, 0u);
    }
};

}  // namespace

TEST_F(InChainKernel32Test, X64HotExportsMatchInterpreterAndStayInChain) {
    parity(true);
}

TEST_F(InChainKernel32Test, X86HotExportsPopStdcallArgumentsInChain) {
    parity(false);
}

TEST_F(InChainKernel32Test, OffArmsTakeTheDispatcherPath) {
    for (const bool is64 : {true, false}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        // VITREO_AB_CYCLE_ONLY=l3k's off arm declines at run time.
        {
            diagnostics::ab_cycle_force_for_test(diagnostics::AbForce::Off);
            Probe probe;
            const Outcome o = run(is64, Arm::InChain, probe);
            expect_program_result(o, probe, is64, "ab-cycle off");
            EXPECT_EQ(o.host_served, 0u);
            // The slot bytes stay, so the stub calls out and is declined.
            EXPECT_GT(o.offers, 0u);
            diagnostics::ab_cycle_force_for_test(diagnostics::AbForce::None);
        }
        // VITREO_L3K=0 builds no in-chain-only entry at all.
        {
            winapi::host_in_chain::force_for_test(
                winapi::host_in_chain::Force::Off);
            Probe probe;
            const Outcome o = run(is64, Arm::InChain, probe);
            expect_program_result(o, probe, is64, "L3-K off");
            EXPECT_EQ(o.host_served, 0u);
            EXPECT_EQ(o.offers, 0u) << "the stub still called out";
            EXPECT_EQ(o.resumed + o.left, 0u);
            winapi::host_in_chain::force_for_test(
                winapi::host_in_chain::Force::On);
        }
    }
}

TEST_F(InChainKernel32Test, GuestCallbackInsideACrtExportLeavesForTheOuterDispatcher) {
    // AGuestCallback: the export runs a guest function (as a user _matherr)
    // on one call. That crossing breaks to the outer dispatcher; the
    // program's result is unchanged.
    for (const bool is64 : {true, false}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        Probe probe;
        probe.nested_on_call = kLoops / 2;
        const Outcome o = run(is64, Arm::InChain, probe);
        expect_program_result(o, probe, is64, "nested callback");
        EXPECT_EQ(probe.nested_result, 7u);
        EXPECT_GE(o.broke_outer, 1u);
        EXPECT_GE(o.host_served, 9u * (kLoops - 4));
    }
}

// ---------------------------------------------------------------------------
// Registration: the in-chain-only entries and the slot map.
// ---------------------------------------------------------------------------

namespace {

void counting_handler(winapi::Marshaler& m) { m.set_return(1); }

}  // namespace

class HostInChainRegistryTest : public ::testing::Test {
protected:
    void TearDown() override {
        winapi::host_in_chain::force_for_test(
            winapi::host_in_chain::Force::None);
        winapi::com_direct::force_for_test(winapi::com_direct::Force::None);
    }

    static u8 slot_byte(const winapi::ThunkRegistry& registry, GuestAddr stub) {
        const u8* map = registry.in_chain_slot_map();
        u64 count = 0;
        std::memcpy(&count, map, sizeof(count));
        const u64 index =
            (stub - kThunkBase) >> winapi::ThunkRegistry::kInChainSlotShift;
        return index < count ? map[sizeof(u64) + index] : 0;
    }

    static winapi::DllExports exports() {
        winapi::DllExports e;
        e.entries.push_back({"SwitchToThread", 0, &counting_handler, 0});
        e.entries.push_back({"SleepEx", 0, &counting_handler, 8});
        e.entries.push_back({"ExitThread", 0, &counting_handler, 4});
        winapi::kernel32::apply_in_chain_classes(e);
        e.entries.back().in_chain = InChainClass::A;  // a misclassified exit
        return e;
    }
};

TEST_F(HostInChainRegistryTest, ClassifiedExportsGetEntriesOnlyTheInChainCrossingUses) {
    winapi::host_in_chain::force_for_test(winapi::host_in_chain::Force::On);
    winapi::ThunkRegistry registry{kThunkBase};
    ASSERT_TRUE(registry.register_dll("kernel32.dll", exports()).is_ok());
    const GuestAddr yield = registry.get_thunk_address("kernel32.dll",
                                                       "SwitchToThread").value();
    const GuestAddr gateway = yield + winapi::ThunkRegistry::STUB_SIZE;
    bool is_stub = false;
    const winapi::DirectThunk* entry =
        registry.in_chain_candidate(yield, is_stub);
    ASSERT_NE(entry, nullptr);
    EXPECT_TRUE(is_stub);
    EXPECT_TRUE(entry->in_chain_only);
    EXPECT_TRUE(entry->sync_last_error);
    EXPECT_EQ(entry->in_chain, InChainClass::A);
    // The dispatcher's direct crossing never sees it: it keeps the full path.
    EXPECT_EQ(registry.direct_thunk(gateway), nullptr);
    EXPECT_EQ(slot_byte(registry, yield), winapi::kInChainSlotHost);
    // Unclassified (SleepEx: alertable waits run APCs) and an exit (whatever
    // its class): no entry, not marked.
    for (const char* name : {"SleepEx", "ExitThread"}) {
        const GuestAddr stub =
            registry.get_thunk_address("kernel32.dll", name).value();
        EXPECT_EQ(registry.in_chain_candidate(stub, is_stub), nullptr) << name;
        EXPECT_EQ(slot_byte(registry, stub), 0u) << name;
    }
}

TEST_F(HostInChainRegistryTest, L3KOffBuildsNothingAndComDirectOffMarksNoDirectSlot) {
    {
        winapi::host_in_chain::force_for_test(
            winapi::host_in_chain::Force::Off);
        winapi::ThunkRegistry registry{kThunkBase};
        ASSERT_TRUE(registry.register_dll("kernel32.dll", exports()).is_ok());
        const GuestAddr yield = registry.get_thunk_address(
            "kernel32.dll", "SwitchToThread").value();
        bool is_stub = false;
        EXPECT_EQ(registry.in_chain_candidate(yield, is_stub), nullptr);
        EXPECT_EQ(slot_byte(registry, yield), 0u);
    }
    {
        // A direct entry of an eligible class is marked only while the
        // direct crossing is statically on.
        winapi::com_direct::force_for_test(winapi::com_direct::Force::Off);
        winapi::ThunkRegistry registry{kThunkBase};
        winapi::DllExports e;
        e.entries.push_back(winapi::make_direct_export(
            "Method", winapi::DirectHandler{
                          [](void*, winapi::Marshaler& m, u32) { m.set_return(1); },
                          nullptr, 1},
            0, InChainClass::A));
        ASSERT_TRUE(registry.register_dll("probe.dll", std::move(e)).is_ok());
        const GuestAddr stub =
            registry.get_thunk_address("probe.dll", "Method").value();
        EXPECT_NE(registry.direct_thunk(stub + winapi::ThunkRegistry::STUB_SIZE),
                  nullptr);
        EXPECT_EQ(slot_byte(registry, stub), 0u);
    }
}

namespace {
// The Windows export list tools/gen_windows_export_names.py captured from
// Windows's kernelbase.dll.
const char* const kKernelbaseNames[] = {
#include "winapi/kernel32/kernelbase_export_names.inc"
};
}  // namespace

TEST(Kernel32InChainClassTest, EveryRuleIsAClassAExportOfWindows) {
    std::set<std::string_view> windows(std::begin(kKernelbaseNames),
                                       std::end(kKernelbaseNames));
    // kernel32-only on x86 (Learn: on x64 these are compiler intrinsics).
    const std::set<std::string_view> x86_kernel32_only = {
        "InterlockedIncrement", "InterlockedDecrement",
        "InterlockedCompareExchange"};
    std::set<std::string_view> seen;
    for (const auto& rule : winapi::kernel32::in_chain_rules()) {
        SCOPED_TRACE(std::string(rule.name));
        EXPECT_TRUE(seen.insert(rule.name).second) << "named twice";
        EXPECT_FALSE(rule.reason.empty());
        EXPECT_EQ(rule.cls, InChainClass::A);
        EXPECT_FALSE(winapi::ends_thread_or_process(rule.name));
        EXPECT_TRUE(windows.count(rule.name) != 0 ||
                    x86_kernel32_only.count(rule.name) != 0)
            << "not a Windows export name";
        EXPECT_EQ(winapi::kernel32::in_chain_rule(rule.name), &rule);
    }
    // Never in-chain by contract: APC-delivering and object waits, a heap
    // call that may raise, the exits.
    for (const char* name :
         {"SleepEx", "WaitForSingleObject", "WaitForSingleObjectEx",
          "WaitForMultipleObjects", "SleepConditionVariableCS", "HeapAlloc",
          "ExitProcess", "ExitThread", "TerminateProcess"}) {
        EXPECT_EQ(winapi::kernel32::in_chain_rule(name), nullptr) << name;
    }
}

// ---------------------------------------------------------------------------
// Process exit, suspension, the yields, the CRT's _matherr, the last-error
// sync on every exit, the switches and the tables (the L3-K review's M1 and
// its missing tests). One general rig over the layout above.
// ---------------------------------------------------------------------------

namespace {

constexpr GuestAddr kCounter = kData + 0x100;  // guest progress
constexpr GuestAddr kInside = kData + 0x104;   // 1 while the guest holds kCs
constexpr GuestAddr kStop = kData + 0x108;     // the host sets it to end a loop
constexpr GuestAddr kSumA = kData + 0x110;     // f64
constexpr GuestAddr kSumB = kData + 0x118;     // f64
constexpr GuestAddr kHalf = kData + 0x120;     // f64 0.5
constexpr GuestAddr kTwo = kData + 0x128;      // f64 2.0
constexpr GuestAddr kCs = kData + 0x200;       // CRITICAL_SECTION
constexpr GuestAddr kMatherr = kNested + 0x200;
constexpr u32 kGuestTid = 0x7A00;              // the guest thread's id here
constexpr u32 kSpin = 3'000'000;               // bounds a loop that never stops

template <class Pred>
bool wait_until(Pred pred, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) return pred();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

u8 slot_byte_in(const u8* map, GuestAddr stub, GuestAddr base = kThunkBase) {
    u64 count = 0;
    std::memcpy(&count, map, sizeof(count));
    const u64 index =
        (stub - base) >> winapi::ThunkRegistry::kInChainSlotShift;
    return index < count ? map[sizeof(u64) + index] : 0;
}

winapi::ExportEntry handler_export(const char* name, u16 stdcall_bytes,
                                   winapi::ApiHandler handler) {
    return winapi::ExportEntry{name, 0, std::move(handler), stdcall_bytes};
}

/// One guest program over the shared layout: its own memory, registry and
/// process; IAT slot i is the i-th export link() names.
class L3kRig {
public:
    explicit L3kRig(bool is64_bit) : is64(is64_bit) {
        EXPECT_TRUE(memory.map_region(kMemBase, kMemSize,
                                      MemoryProtection::ReadWrite).is_ok());
    }

    void add(const char* dll, winapi::DllExports exports) {
        EXPECT_TRUE(registry.register_dll(dll, std::move(exports)).is_ok())
            << dll;
    }

    void link(std::initializer_list<std::pair<const char*, const char*>> slots) {
        map_thunk_slab(memory, registry);
        u32 i = 0;
        for (const auto& [dll, name] : slots) {
            const GuestAddr s = stub(dll, name);
            EXPECT_NE(s, 0u) << name;
            if (is64) {
                EXPECT_TRUE(memory.write_u64(kIat + i * 8, s).is_ok());
            } else {
                EXPECT_TRUE(
                    memory.write_u32(kIat + i * 4, static_cast<u32>(s)).is_ok());
            }
            ++i;
        }
    }

    [[nodiscard]] GuestAddr stub(const char* dll, const char* name) const {
        return registry.get_thunk_address(dll, name).value_or(0);
    }

    void load(const Code& c) {
        EXPECT_TRUE(memory.write_bytes(c.base, c.bytes.data(), c.bytes.size())
                        .is_ok());
        if (c.base == kCode) code_end = c.base + c.bytes.size();
    }

    [[nodiscard]] u32 read32(GuestAddr at) const {
        return memory.read_u32(at).value_or(0xDEADu);
    }

    void reset_cpu(cpu::CpuState& c) const {
        c = cpu::CpuState{};
        if (is64) {
            c.init_long_mode();
            c.seg.gs_desc.base = kTeb;
            c.set_sp(kSp64);
        } else {
            c.init_protected_mode();
            c.seg.fs_desc.base = kTeb;
            c.set_sp(kSp32);
        }
    }

    Outcome run(Arm arm, u64 max_instructions = 400'000'000) {
        JitCompiler::set_inchain_thunks_codegen_for_test(
            arm == Arm::InChain ? 1 : 0);
        JitCompiler::set_jit_hot_threshold_for_test(
            arm == Arm::Interpreter ? JitCompiler::kJitHotThresholdMax : 1);
        reset_cpu(cpu);
        if (is64) {
            EXPECT_TRUE(memory.write_u64(kSp64, PROCESS_RETURN_SENTINEL).is_ok());
        } else {
            EXPECT_TRUE(memory.write_u32(
                            kSp32, static_cast<u32>(PROCESS_RETURN_SENTINEL))
                            .is_ok());
        }
        cpu.rip = kCode;
        RunConfig config;
        config.max_instructions = max_instructions;
        Outcome out;
        const u64 host0 = in_chain_host_crossings_this_thread();
        const u64 offers0 = in_chain_offers_this_thread();
        const u64 resumed0 = JitCompiler::inchain_resumed_this_thread();
        const u64 left0 = JitCompiler::inchain_left_this_thread();
        const u64 outer0 = JitCompiler::inchain_broke_outer_this_thread();
        out.result = run_execution_loop(cpu, memory, registry, process, config);
        out.host_served = in_chain_host_crossings_this_thread() - host0;
        out.offers = in_chain_offers_this_thread() - offers0;
        out.resumed = JitCompiler::inchain_resumed_this_thread() - resumed0;
        out.left = JitCompiler::inchain_left_this_thread() - left0;
        out.broke_outer =
            JitCompiler::inchain_broke_outer_this_thread() - outer0;
        out.tid = winapi::ThreadManager::get_current_thread_id_static();
        out.cpu = cpu;
        return out;
    }

    const bool is64;
    vitreo::GuestMemory memory;
    winapi::ThunkRegistry registry{kThunkBase};
    winapi::ProcessManager process;
    cpu::CpuState cpu;
    GuestAddr code_end{kCode};
};

void write_f64(vitreo::GuestMemory& memory, GuestAddr at, double v) {
    u64 bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    EXPECT_TRUE(memory.write_u64(at, bits).is_ok());
}

double read_f64(const vitreo::GuestMemory& memory, GuestAddr at) {
    const u64 bits = memory.read_u64(at).value_or(0);
    double v = 0;
    std::memcpy(&v, &bits, sizeof v);
    return v;
}

}  // namespace

// --- M1: process exit observed by the in-chain crossing --------------------

class InChainKernel32ExitTest : public InChainKernel32Test {};

// Learn ExitProcess: every other thread of the process ends; none runs more
// of its code. A thread spinning on an in-chain export (SwitchToThread here)
// must stop at its next call once another thread has called ExitProcess, as
// the dispatcher stops a thread after any thunk once has_exited().
TEST_F(InChainKernel32ExitTest, ExitProcessStopsAThreadSpinningInChainOnSwitchToThread) {
    for (const bool is64 : {true, false}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        winapi::ThreadManager::set_current_thread_id(kGuestTid);
        L3kRig rig(is64);
        winapi::DllExports k32;
        k32.entries.push_back(handler_export(
            "SwitchToThread", 0, [](winapi::Marshaler& m) { m.set_return(0); }));
        winapi::kernel32::apply_in_chain_classes(k32);
        rig.add("kernel32.dll", std::move(k32));
        rig.link({{"kernel32.dll", "SwitchToThread"}});
        // for (ebx = kSpin; ebx; --ebx) { ++[kCounter]; SwitchToThread(); }
        Code c(kCode);
        c.prologue(is64);
        c.mov_ebx(kSpin);
        const GuestAddr loop = c.here();
        c.inc_mem(is64, kCounter);
        c.call_slot(is64, 0);
        c.dec_ebx(is64);
        c.jnz_to(loop);
        c.epilogue(is64);
        rig.load(c);

        std::atomic<bool> done{false};
        std::atomic<u32> at_exit{0};
        std::thread exiter([&] {
            (void)wait_until([&] { return done.load() ||
                                          rig.read32(kCounter) >= 2000; },
                             std::chrono::seconds(10));
            rig.process.exit_process(42);
            at_exit.store(rig.read32(kCounter));
        });
        const Outcome o = rig.run(Arm::InChain);
        done = true;
        exiter.join();

        ASSERT_TRUE(o.result.success) << o.result.error_message;
        EXPECT_EQ(o.result.exit_code, 42);
        // At most the iteration in flight when the flag was set.
        EXPECT_LE(rig.read32(kCounter), at_exit.load() + 2u)
            << "guest code ran after ExitProcess";
        EXPECT_LT(rig.read32(kCounter), kSpin);
        EXPECT_GE(o.host_served, 1000u) << "the spin was not in-chain";
        EXPECT_GE(o.broke_outer, 1u);
    }
}

// Learn EnterCriticalSection: "If a critical section is deleted while it is
// still owned, the state of the threads waiting for ownership of the deleted
// critical section is undefined"; and while the process exits a waiter
// terminates. Vitreo models the exit as the wait returning WITHOUT ownership
// (critical_section.cpp, begin_shutdown): the thread must then run no guest
// code -- in particular not the instructions inside the section.
TEST_F(InChainKernel32ExitTest, ExitProcessStopsAThreadBlockedInChainOnAContendedCriticalSection) {
    for (const bool is64 : {true, false}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        winapi::ThreadManager::set_current_thread_id(kGuestTid);
        L3kRig rig(is64);
        winapi::CriticalSectionManager cs(rig.memory);
        cs.note_bitness(is64);
        ASSERT_FALSE(cs.initialize(kCs).has_value());
        set_fast_thunk_cs_manager(&cs);
        rig.process.set_exit_wake([&cs] { cs.begin_shutdown(); });
        static std::atomic<u64> handler_calls{0};
        handler_calls = 0;
        auto never = [](winapi::Marshaler&) { ++handler_calls; };
        winapi::DllExports k32;
        k32.entries.push_back(handler_export("EnterCriticalSection", 4, never));
        k32.entries.push_back(handler_export("LeaveCriticalSection", 4, never));
        winapi::kernel32::apply_in_chain_classes(k32);
        rig.add("kernel32.dll", std::move(k32));
        rig.link({{"kernel32.dll", "EnterCriticalSection"},
                  {"kernel32.dll", "LeaveCriticalSection"}});
        // for (ebx = kSpin; ebx; --ebx) {
        //   EnterCriticalSection(&cs); ++[kCounter]; LeaveCriticalSection(&cs); }
        Code c(kCode);
        c.prologue(is64);
        c.mov_ebx(kSpin);
        const GuestAddr loop = c.here();
        c.arg0(is64, kCs);
        c.call_slot(is64, 0);
        c.inc_mem(is64, kCounter);
        c.arg0(is64, kCs);
        c.call_slot(is64, 1);
        c.dec_ebx(is64);
        c.jnz_to(loop);
        c.epilogue(is64);
        rig.load(c);

        std::atomic<bool> done{false};
        std::atomic<u32> at_exit{0};
        std::atomic<bool> owned{false};
        std::thread exiter([&] {
            winapi::ThreadManager::set_current_thread_id(kGuestTid + 1);
            (void)wait_until([&] { return done.load() ||
                                          rig.read32(kCounter) >= 2000; },
                             std::chrono::seconds(10));
            const u64 waits = cs.contended_waits();
            cs.enter(kCs);  // the guest now blocks in its next Enter
            owned = true;
            (void)wait_until([&] { return done.load() ||
                                          cs.contended_waits() > waits; },
                             std::chrono::milliseconds(500));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            at_exit.store(rig.read32(kCounter));
            rig.process.exit_process(42);
        });
        const Outcome o = rig.run(Arm::InChain);
        done = true;
        exiter.join();
        set_fast_thunk_cs_manager(nullptr);

        ASSERT_TRUE(o.result.success) << o.result.error_message;
        EXPECT_EQ(o.result.exit_code, 42);
        EXPECT_TRUE(owned.load());
        EXPECT_EQ(rig.read32(kCounter), at_exit.load())
            << "the guest ran inside a critical section it does not own";
        EXPECT_EQ(handler_calls.load(), 0u) << "the fast bodies serve both";
        EXPECT_GE(o.host_served, 1000u) << "Enter/Leave were not in-chain";
    }
}

// --- SuspendThread against a thread blocked in-chain ------------------------

class InChainKernel32SuspendTest : public InChainKernel32Test {};

// Learn SuspendThread/GetThreadContext: a suspended thread runs no code
// until resumed. Here the guest thread spends its time in EnterCriticalSection
// served in-chain (a host wait with the host call published), contended by
// another thread; a third suspends and resumes it repeatedly. While it is
// suspended its guest progress counter must not move, its captured RIP is
// guest code or the thunk it is in, and the section stays exclusive.
TEST_F(InChainKernel32SuspendTest, ThreadBlockedInChainOnACriticalSectionStaysSuspended) {
    for (const bool is64 : {true, false}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        winapi::ThreadManager::set_current_thread_id(kGuestTid);
        L3kRig rig(is64);
        winapi::CriticalSectionManager cs(rig.memory);
        cs.note_bitness(is64);
        ASSERT_FALSE(cs.initialize(kCs).has_value());
        set_fast_thunk_cs_manager(&cs);
        winapi::DllExports k32;
        auto never = [](winapi::Marshaler&) {};
        k32.entries.push_back(handler_export("EnterCriticalSection", 4, never));
        k32.entries.push_back(handler_export("LeaveCriticalSection", 4, never));
        winapi::kernel32::apply_in_chain_classes(k32);
        rig.add("kernel32.dll", std::move(k32));
        rig.link({{"kernel32.dll", "EnterCriticalSection"},
                  {"kernel32.dll", "LeaveCriticalSection"}});
        // do { Enter(&cs); [kInside] = 1; ++[kCounter]; [kInside] = 0;
        //      Leave(&cs); } while ([kStop] == 0);
        Code c(kCode);
        c.prologue(is64);
        const GuestAddr loop = c.here();
        c.arg0(is64, kCs);
        c.call_slot(is64, 0);
        c.mov_mem(is64, kInside, 1);
        c.inc_mem(is64, kCounter);
        c.mov_mem(is64, kInside, 0);
        c.arg0(is64, kCs);
        c.call_slot(is64, 1);
        c.loop_while_zero(is64, kStop, loop);
        c.epilogue(is64);
        rig.load(c);

        std::atomic<bool> stop_contender{false};
        std::atomic<u32> overlaps{0}, progressed{0}, bad_rip{0}, rounds{0};
        std::thread contender([&] {
            winapi::ThreadManager::set_current_thread_id(kGuestTid + 2);
            while (!stop_contender.load()) {
                cs.enter(kCs);
                if (rig.read32(kInside) != 0) ++overlaps;
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                cs.leave(kCs);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
        std::thread suspender([&] {
            winapi::ThreadManager::set_current_thread_id(kGuestTid + 3);
            auto& reg = winapi::ThreadSuspendRegistry::instance();
            if (wait_until([&] { return rig.read32(kCounter) >= 2000; },
                           std::chrono::seconds(10))) {
                for (int i = 0; i < 8; ++i) {
                    if (!reg.request_suspend(kGuestTid)) break;
                    cpu::CpuState ctx;
                    (void)reg.capture(kGuestTid, ctx);
                    const u32 c1 = rig.read32(kCounter);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    if (rig.read32(kCounter) != c1) ++progressed;
                    const GuestAddr rip = static_cast<GuestAddr>(ctx.rip);
                    if (!(rip >= kCode && rip < rig.code_end) &&
                        !rig.registry.is_in_thunk_region(rip)) {
                        ++bad_rip;
                    }
                    reg.request_resume(kGuestTid);
                    ++rounds;
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                }
            }
            (void)rig.memory.write_u32(kStop, 1);
        });
        const Outcome o = rig.run(Arm::InChain);
        stop_contender = true;
        suspender.join();
        contender.join();
        set_fast_thunk_cs_manager(nullptr);

        ASSERT_TRUE(o.result.success) << o.result.error_message;
        EXPECT_EQ(rounds.load(), 8u);
        EXPECT_EQ(progressed.load(), 0u) << "guest code ran while suspended";
        EXPECT_EQ(bad_rip.load(), 0u);
        EXPECT_EQ(overlaps.load(), 0u) << "the section was not exclusive";
        EXPECT_GE(o.host_served, 1000u) << "Enter/Leave were not in-chain";
    }
}

// --- the yields --------------------------------------------------------------

class InChainKernel32YieldTest : public InChainKernel32Test {};

// SwitchToThread (a registered handler) and Sleep(0) (an inline fast body)
// served in-chain end in the state the interpreter and the dispatcher reach:
// SwitchToThread's BOOL is accumulated, Sleep returns nothing.
TEST_F(InChainKernel32YieldTest, SwitchToThreadAndSleepZeroMatchTheOtherPaths) {
    for (const bool is64 : {true, false}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        static std::atomic<u64> yields{0};
        Outcome outs[3];
        const Arm arms[3] = {Arm::InChain, Arm::Dispatcher, Arm::Interpreter};
        for (int a = 0; a < 3; ++a) {
            yields = 0;
            L3kRig rig(is64);
            winapi::DllExports k32;
            k32.entries.push_back(handler_export(
                "SwitchToThread", 0, [](winapi::Marshaler& m) {
                    ++yields;
                    std::this_thread::yield();
                    m.set_return(1);
                }));
            k32.entries.push_back(handler_export(
                "Sleep", 4, [](winapi::Marshaler&) { ++yields; }));
            winapi::kernel32::apply_in_chain_classes(k32);
            rig.add("kernel32.dll", std::move(k32));
            rig.link({{"kernel32.dll", "SwitchToThread"},
                      {"kernel32.dll", "Sleep"}});
            // esi = 0; for (ebx = N; ebx; --ebx) { esi += SwitchToThread();
            //                                     Sleep(0); }
            Code c(kCode);
            c.prologue(is64);
            c.raw({0x31, 0xF6});  // xor esi, esi
            c.mov_ebx(kLoops);
            const GuestAddr loop = c.here();
            c.call_slot(is64, 0);
            c.raw({0x01, 0xC6});  // add esi, eax
            c.arg0(is64, 0);
            c.call_slot(is64, 1);
            c.dec_ebx(is64);
            c.jnz_to(loop);
            c.epilogue(is64);
            rig.load(c);
            outs[a] = rig.run(arms[a]);
            SCOPED_TRACE(a);
            ASSERT_TRUE(outs[a].result.success) << outs[a].result.error_message;
            // Sleep's handler never runs: its fast body serves every path.
            EXPECT_EQ(yields.load(), u64{kLoops});
            EXPECT_EQ(outs[a].cpu.gpr.get32(6), kLoops);  // ESI
            EXPECT_EQ(outs[a].cpu.sp(), is64 ? kSp64 + 8u : kSp32 + 4u);
        }
        expect_same_state(outs[0], outs[2], "in-chain vs interpreter");
        expect_same_state(outs[1], outs[2], "dispatcher vs interpreter");
        EXPECT_GE(outs[0].host_served, 2u * (kLoops - 4));
        EXPECT_GE(outs[0].resumed, 2u * (kLoops - 4));
        EXPECT_EQ(outs[1].host_served, 0u);
        EXPECT_EQ(outs[2].host_served, 0u);
    }
}

// --- the CRT's _matherr --------------------------------------------------------

class InChainKernel32MatherrTest : public InChainKernel32Test {
protected:
    struct Sums {
        double a{0}, b{0};
        Outcome out;
    };

    /// The real CRT math exports (create_crt_math_thunks) in ucrtbase, with a
    /// user _matherr (Learn, _matherr / __setusermatherr) that sets
    /// e->retval = 42.0 and returns nonzero. The program sums acos(0.5) into
    /// kSumA and acos(2.0) -- a _DOMAIN error -- into kSumB, N times.
    static Sums run_acos(bool is64, Arm arm) {
        L3kRig rig(is64);
        winapi::CrtMathManager math;
        winapi::CrtMemoryManager crt(rig.memory, 0x60000, 0x10000);
        rig.add("ucrtbase.dll", winapi::create_crt_math_thunks(math, crt));
        rig.link({{"ucrtbase.dll", "acos"}});
        crt.set_guest_caller([&rig](GuestAddr fn, const u64* args, usize n) {
            bool completed = false;
            return run_guest_callback(rig.cpu, rig.memory, rig.registry,
                                      rig.process, fn, args, n, rig.is64,
                                      &completed);
        });
        (void)math.exchange_user_matherr(kMatherr);
        // _matherr(struct _exception* e): e->retval = 42.0; return 1.
        // retval is at +32 (x64: int, 4 pad, char*, 3 doubles) or +24 (x86).
        Code h(kMatherr);
        if (is64) {
            h.raw({0x48, 0xB8}); h.u64le(0x4045000000000000ull);  // mov rax, 42.0
            h.raw({0x48, 0x89, 0x41, 0x20});                     // mov [rcx+32], rax
        } else {
            h.raw({0x8B, 0x4C, 0x24, 0x04});                     // mov ecx, [esp+4]
            h.raw({0xC7, 0x41, 0x18}); h.u32le(0);               // retval lo
            h.raw({0xC7, 0x41, 0x1C}); h.u32le(0x40450000u);     // retval hi
        }
        h.raw({0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3});              // mov eax, 1; ret
        rig.load(h);
        // Resolve the callback loop's fast-thunk set first (setup, as above).
        {
            const u8 ret = 0xC3;
            EXPECT_TRUE(rig.memory.write_bytes(kNested + 0x100, &ret, 1).is_ok());
            cpu::CpuState warm;
            if (is64) warm.init_long_mode(); else warm.init_protected_mode();
            warm.set_sp(kMemBase + 0xC000);
            bool warm_completed = false;
            (void)run_guest_callback(warm, rig.memory, rig.registry, rig.process,
                                     kNested + 0x100, nullptr, 0, is64,
                                     &warm_completed);
        }
        write_f64(rig.memory, kHalf, 0.5);
        write_f64(rig.memory, kTwo, 2.0);
        Code c(kCode);
        c.prologue(is64);
        c.mov_ebx(kLoops);
        const GuestAddr loop = c.here();
        for (const auto& [x, sum] : {std::pair{kHalf, kSumA}, std::pair{kTwo, kSumB}}) {
            if (is64) {
                c.raw({0xF2, 0x0F, 0x10, 0x04, 0x25}); c.u32le(static_cast<u32>(x));    // movsd xmm0, [x]
                c.call_slot(true, 0);
                c.raw({0xF2, 0x0F, 0x58, 0x04, 0x25}); c.u32le(static_cast<u32>(sum));  // addsd xmm0, [sum]
                c.raw({0xF2, 0x0F, 0x11, 0x04, 0x25}); c.u32le(static_cast<u32>(sum));  // movsd [sum], xmm0
            } else {
                c.raw({0x83, 0xEC, 0x08});                            // sub esp, 8
                c.raw({0xDD, 0x05}); c.u32le(static_cast<u32>(x));                      // fld qword [x]
                c.raw({0xDD, 0x1C, 0x24});                            // fstp qword [esp]
                c.call_slot(false, 0);
                c.raw({0x83, 0xC4, 0x08});                            // add esp, 8 (cdecl)
                c.raw({0xDC, 0x05}); c.u32le(static_cast<u32>(sum));                    // fadd qword [sum]
                c.raw({0xDD, 0x1D}); c.u32le(static_cast<u32>(sum));                    // fstp qword [sum]
            }
        }
        c.dec_ebx(is64);
        c.jnz_to(loop);
        c.epilogue(is64);
        rig.load(c);
        Sums s;
        s.out = rig.run(arm);
        s.a = read_f64(rig.memory, kSumA);
        s.b = read_f64(rig.memory, kSumB);
        return s;
    }
};

TEST_F(InChainKernel32MatherrTest, UserMatherrRunsFromAnInChainAcosAndItsRetvalIsReturned) {
    for (const bool is64 : {true, false}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        const Sums in = run_acos(is64, Arm::InChain);
        const Sums interp = run_acos(is64, Arm::Interpreter);
        for (const Sums* s : {&in, &interp}) {
            ASSERT_TRUE(s->out.result.success) << s->out.result.error_message;
            EXPECT_EQ(s->b, 42.0 * kLoops) << "_matherr's retval not returned";
            EXPECT_NEAR(s->a, kLoops * (std::acos(-1.0) / 3.0), 1e-9);
        }
        EXPECT_EQ(in.a, interp.a);
        // acos(0.5) resumes in-chain; acos(2.0) ran guest code and left.
        EXPECT_GE(in.out.host_served, 2u * (kLoops - 4));
        EXPECT_GE(in.out.broke_outer, kLoops - 4);
        EXPECT_GE(in.out.resumed, kLoops - 4);
    }
}

// --- TEB.LastErrorValue on every exit of the crossing -----------------------

class InChainKernel32LastErrorTest : public InChainKernel32Test {};

namespace {
std::atomic<int> g_le_mode{0};
std::atomic<u32> g_le_seen{0};
winapi::ProcessManager* g_le_process = nullptr;
constexpr GuestAddr kTransferTo = kCode + 0x40;
}  // namespace

// The full path syncs TEB.LastErrorValue into the host copy before a handler
// and back out after it, whatever the handler then does (return, raise,
// unwind, transfer control). The in-chain crossing of an L3-K export must
// leave the same TEB value on each of those exits; and it must answer
// BreakOuter once the process has exited during the call (M1).
TEST_F(InChainKernel32LastErrorTest, TebLastErrorIsTheHandlersOnEveryExit) {
    // Its own thread: a raise leaves the run loop's thread-local
    // "pending raise from a chain thunk" note set.
    std::thread([this] {
        for (const bool is64 : {true, false}) {
            SCOPED_TRACE(is64 ? "x64" : "x86");
            winapi::ThreadManager::set_current_thread_id(kGuestTid + 4);
            L3kRig rig(is64);
            winapi::DllExports k32;
            k32.entries.push_back(handler_export(
                "SetEvent", 4, [](winapi::Marshaler& m) {
                    g_le_seen = winapi::get_last_error();
                    const int mode = g_le_mode.load();
                    winapi::set_last_error(0xE0u + static_cast<u32>(mode));
                    m.set_return(1);
                    cpu::CpuState& c = *m.cpu();
                    switch (mode) {
                    case 1: c.pending_raise.active = true; break;
                    case 2: c.pending_unwind.active = true; break;
                    case 3:
                        c.thunk_control_transfer = true;
                        c.rip = kTransferTo;
                        break;
                    case 4: g_le_process->exit_process(9); break;
                    default: break;
                    }
                }));
            winapi::kernel32::apply_in_chain_classes(k32);
            rig.add("kernel32.dll", std::move(k32));
            rig.link({{"kernel32.dll", "SetEvent"}});
            const GuestAddr stub = rig.stub("kernel32.dll", "SetEvent");
            g_le_process = &rig.process;

            FastThunkAddrs fast;
            resolve_fast_thunks(fast, rig.registry);
            winapi::Marshaler marshaler(rig.memory);
            cpu::CpuState cpu;
            marshaler.attach_cpu(cpu);
            winapi::ThreadSuspendRegistry::PublishGuard guard(kGuestTid + 4,
                                                             &cpu);
            InChainThunkEnv env{&rig.registry, &rig.memory, &marshaler, &guard,
                                nullptr, false};
            env.fast_thunks = &fast;
            env.process = &rig.process;

            const GuestAddr teb_err = kTeb + (is64 ? 0x68 : 0x34);
            const InChainThunkResult want[] = {
                InChainThunkResult::Returned, InChainThunkResult::BreakOuter,
                InChainThunkResult::BreakOuter, InChainThunkResult::BreakOuter,
                InChainThunkResult::BreakOuter};
            for (int mode = 0; mode < 5; ++mode) {
                SCOPED_TRACE(mode);
                g_le_mode = mode;
                rig.process.clear_exit_request();
                rig.reset_cpu(cpu);
                const GuestAddr sp = is64 ? kSp64 - 0x40 : kSp32 - 0x40;
                cpu.set_sp(sp);
                cpu.gpr.rcx = 0x99;
                if (is64) {
                    ASSERT_TRUE(rig.memory.write_u64(sp, kCode).is_ok());
                } else {
                    ASSERT_TRUE(rig.memory.write_u32(sp, static_cast<u32>(kCode)).is_ok());
                    ASSERT_TRUE(rig.memory.write_u32(sp + 4, 0x99).is_ok());
                }
                ASSERT_TRUE(rig.memory.write_u32(teb_err, 0x31).is_ok());
                cpu.rip = stub;
                EXPECT_EQ(serve_in_chain_thunk(cpu, &env), want[mode]);
                EXPECT_EQ(g_le_seen.load(), 0x31u) << "TEB not synced in";
                EXPECT_EQ(rig.read32(teb_err), 0xE0u + static_cast<u32>(mode))
                    << "TEB not synced out";
                if (mode == 0) {
                    EXPECT_EQ(cpu.rip, kCode);
                    EXPECT_EQ(cpu.sp(), sp + 8u);  // x64 RET; x86 RET + 4 stdcall bytes
                }
                if (mode == 3) EXPECT_EQ(cpu.rip, kTransferTo);
                cpu.pending_raise.active = false;
                cpu.pending_unwind.active = false;
            }
            // The exit test is the process's: without one to observe, the
            // same call resumes.
            g_le_mode = 4;
            rig.process.clear_exit_request();
            env.process = nullptr;
            rig.reset_cpu(cpu);
            const GuestAddr sp = is64 ? kSp64 - 0x40 : kSp32 - 0x40;
            cpu.set_sp(sp);
            if (is64) {
                ASSERT_TRUE(rig.memory.write_u64(sp, kCode).is_ok());
            } else {
                ASSERT_TRUE(rig.memory.write_u32(sp, static_cast<u32>(kCode)).is_ok());
            }
            cpu.rip = stub;
            EXPECT_EQ(serve_in_chain_thunk(cpu, &env),
                      InChainThunkResult::Returned);
            rig.process.clear_exit_request();
            g_le_process = nullptr;
        }
    }).join();
}

// --- VITREO_D3D11_COM_DIRECT=0 with L3-K on ----------------------------------

class InChainKernel32ComDirectOffTest : public InChainKernel32Test {};

// "WP-R1 off, L3 unmarked, L3-K on": with the direct crossing statically off
// a direct entry's slot is 0 and its calls never call out, while the L3-K
// exports are still marked (2) and served in-chain.
TEST_F(InChainKernel32ComDirectOffTest, DirectSlotsStayUnmarkedAndKernel32IsStillInChain) {
    winapi::com_direct::force_for_test(winapi::com_direct::Force::Off);
    for (const bool is64 : {true, false}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        auto make = [is64]() {
            auto rig = std::make_unique<L3kRig>(is64);
            winapi::DllExports probe;
            probe.entries.push_back(winapi::make_direct_export(
                "Method",
                winapi::DirectHandler{
                    [](void*, winapi::Marshaler& m, u32) { m.set_return(1); },
                    nullptr, 1},
                0, InChainClass::A));
            rig->add("probe.dll", std::move(probe));
            winapi::DllExports k32;
            k32.entries.push_back(handler_export(
                "TlsGetValue", 4, [](winapi::Marshaler& m) { m.set_return(0); }));
            winapi::kernel32::apply_in_chain_classes(k32);
            rig->add("kernel32.dll", std::move(k32));
            rig->link({{"probe.dll", "Method"}, {"kernel32.dll", "TlsGetValue"}});
            return rig;
        };
        {
            auto rig = make();
            const u8* map = rig->registry.in_chain_slot_map();
            EXPECT_EQ(slot_byte_in(map, rig->stub("probe.dll", "Method")), 0u);
            EXPECT_EQ(slot_byte_in(map, rig->stub("kernel32.dll", "TlsGetValue")),
                      winapi::kInChainSlotHost);
            EXPECT_TRUE(in_chain_thunks_possible()) << "codegen gate closed";
            // esi = 0; for (ebx = N; ebx; --ebx) esi += Method();
            Code c(kCode);
            c.prologue(is64);
            c.raw({0x31, 0xF6});
            c.mov_ebx(kLoops);
            const GuestAddr loop = c.here();
            c.call_slot(is64, 0);
            c.raw({0x01, 0xC6});
            c.dec_ebx(is64);
            c.jnz_to(loop);
            c.epilogue(is64);
            rig->load(c);
            const Outcome o = rig->run(Arm::InChain);
            ASSERT_TRUE(o.result.success) << o.result.error_message;
            EXPECT_EQ(o.cpu.gpr.get32(6), kLoops);
            EXPECT_EQ(o.offers, 0u) << "an unmarked direct slot called out";
        }
        {
            auto rig = make();
            // for (ebx = N; ebx; --ebx) TlsGetValue(3);
            Code c(kCode);
            c.prologue(is64);
            c.mov_ebx(kLoops);
            const GuestAddr loop = c.here();
            c.arg0(is64, kTlsIndex);
            c.call_slot(is64, 1);
            c.dec_ebx(is64);
            c.jnz_to(loop);
            c.epilogue(is64);
            rig->load(c);
            const Outcome o = rig->run(Arm::InChain);
            ASSERT_TRUE(o.result.success) << o.result.error_message;
            EXPECT_GE(o.host_served, kLoops - 4);
        }
    }
}

// --- the real tables ----------------------------------------------------------

class InChainKernel32TablesTest : public InChainKernel32Test {};

// register_all_dlls: every kernel32 rule gets an in-chain-only entry (slot
// byte 2) in kernel32 and in the kernelbase copy where kernelbase has the
// export; the CRT math exports are AGuestCallback in ucrtbase and msvcrt.
TEST_F(InChainKernel32TablesTest, RegisterAllDllsMarksEveryRuleAndTheCrtMath) {
    for (const bool is64 : {false, true}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        vitreo::GuestMemory memory;
        cli::RuntimeContext ctx{memory, "l3k_tables.exe", "default", is64,
                                false, false, nullptr, false, std::string{},
                                {}, nullptr};
        cli::register_all_dlls(ctx, memory, is64);
        const winapi::ThunkRegistry& reg = ctx.thunk_registry;
        const u8* map = reg.in_chain_slot_map();
        usize marked = 0;
        for (const auto& rule : winapi::kernel32::in_chain_rules()) {
            SCOPED_TRACE(std::string(rule.name));
            for (const char* dll : {"kernel32.dll", "kernelbase.dll"}) {
                const auto stub = reg.get_thunk_address(dll, rule.name);
                if (stub.is_err()) {
                    EXPECT_STRNE(dll, "kernel32.dll") << "not registered";
                    continue;
                }
                bool is_stub = false;
                const winapi::DirectThunk* entry =
                    reg.in_chain_candidate(stub.value(), is_stub);
                ASSERT_NE(entry, nullptr) << dll;
                EXPECT_TRUE(entry->in_chain_only) << dll;
                EXPECT_TRUE(entry->sync_last_error) << dll;
                EXPECT_EQ(entry->in_chain, InChainClass::A) << dll;
                EXPECT_EQ(slot_byte_in(map, stub.value(), reg.thunk_base()),
                          winapi::kInChainSlotHost) << dll;
                ++marked;
            }
        }
        EXPECT_GE(marked, winapi::kernel32::in_chain_rules().size());
        for (const char* dll : {"ucrtbase.dll", "msvcrt.dll"}) {
            for (const char* name : {"acos", "sqrt", "pow", "atan2"}) {
                SCOPED_TRACE(std::string(dll) + "!" + name);
                const auto stub = reg.get_thunk_address(dll, name);
                ASSERT_TRUE(stub.is_ok());
                bool is_stub = false;
                const winapi::DirectThunk* entry =
                    reg.in_chain_candidate(stub.value(), is_stub);
                ASSERT_NE(entry, nullptr);
                EXPECT_EQ(entry->in_chain, InChainClass::AGuestCallback);
                EXPECT_EQ(slot_byte_in(map, stub.value(), reg.thunk_base()),
                          winapi::kInChainSlotHost);
            }
        }
        // Never in-chain: the exits and the alertable or object waits.
        for (const char* name : {"ExitProcess", "ExitThread", "SleepEx",
                                 "WaitForSingleObject"}) {
            const auto stub = reg.get_thunk_address("kernel32.dll", name);
            ASSERT_TRUE(stub.is_ok()) << name;
            EXPECT_EQ(slot_byte_in(map, stub.value(), reg.thunk_base()), 0u) << name;
        }
    }
}

// --- withdraw_in_chain (x86 guest CRT accelerators) ----------------------------

class InChainKernel32WithdrawTest : public InChainKernel32Test {};

// install_guest_crt_accelerators overwrites EnterCriticalSection's and
// LeaveCriticalSection's entry sleds with jumps to guest code: their slots
// are withdrawn in every published map (a reader may hold an older one) and
// in later ones, so translated code never calls out for them;
// TryEnterCriticalSection keeps its stub and its slot.
TEST_F(InChainKernel32WithdrawTest, AcceleratedSledsAreWithdrawnFromEveryMap) {
    if (std::getenv("VITREO_NO_CS_ACCEL") != nullptr) {
        GTEST_SKIP() << "VITREO_NO_CS_ACCEL: no critical-section accelerators";
    }
    winapi::ThreadManager::set_current_thread_id(kGuestTid);
    L3kRig rig(false);
    winapi::CriticalSectionManager cs(rig.memory);
    cs.note_bitness(false);
    ASSERT_FALSE(cs.initialize(kCs).has_value());
    set_fast_thunk_cs_manager(&cs);
    static std::atomic<u64> slow_calls{0};
    slow_calls = 0;
    auto slow = [](winapi::Marshaler&) { ++slow_calls; };
    winapi::DllExports k32;
    k32.entries.push_back(handler_export("EnterCriticalSection", 4, slow));
    k32.entries.push_back(handler_export("LeaveCriticalSection", 4, slow));
    k32.entries.push_back(handler_export("TryEnterCriticalSection", 4, slow));
    k32.entries.push_back(handler_export("__vitreo_cs_enter_slow", 4, slow));
    k32.entries.push_back(handler_export("__vitreo_cs_leave_wake", 4, slow));
    winapi::kernel32::apply_in_chain_classes(k32);
    rig.add("kernel32.dll", std::move(k32));
    // Every tail-jump target the accelerator code names must exist, or the
    // install refuses (guest_accel.cpp).
    for (const char* crt : {"msvcrt.dll", "ucrtbase.dll"}) {
        winapi::DllExports e;
        e.entries.push_back(handler_export("__vitreo_fgetc_refill", 0, slow));
        e.entries.push_back(handler_export("__vitreo_fgetc_slow", 0, slow));
        e.entries.push_back(handler_export("__vitreo_ungetc_slow", 0, slow));
        rig.add(crt, std::move(e));
    }
    rig.link({{"kernel32.dll", "EnterCriticalSection"},
              {"kernel32.dll", "LeaveCriticalSection"},
              {"kernel32.dll", "TryEnterCriticalSection"}});
    const GuestAddr enter = rig.stub("kernel32.dll", "EnterCriticalSection");
    const GuestAddr leave = rig.stub("kernel32.dll", "LeaveCriticalSection");
    const GuestAddr try_enter =
        rig.stub("kernel32.dll", "TryEnterCriticalSection");
    const u8* old_map = rig.registry.in_chain_slot_map();
    for (GuestAddr s : {enter, leave, try_enter}) {
        EXPECT_EQ(slot_byte_in(old_map, s), winapi::kInChainSlotHost);
    }

    EXPECT_GE(winapi::install_guest_crt_accelerators(rig.memory, rig.registry),
              2u);
    // The accelerator code is built once per process; a rig whose memory
    // did not get it (an earlier install in this process) maps it here,
    // exactly as install would have.
    if (rig.memory.read_u8(winapi::CRT_ACCEL_BASE_X86).is_err()) {
        winapi::CrtAccelBlob blob =
            winapi::build_guest_crt_code(winapi::CRT_ACCEL_BASE_X86, true);
        for (const auto& [slot, qualified] : blob.extern_fixups) {
            const auto bang = qualified.find('!');
            const GuestAddr target =
                rig.registry
                    .get_thunk_address(qualified.substr(0, bang),
                                       qualified.substr(bang + 1), true)
                    .value_or(0);
            ASSERT_NE(target, 0u) << qualified;
            const u32 site =
                static_cast<u32>(winapi::CRT_ACCEL_BASE_X86 + slot);
            const u32 rel = static_cast<u32>(target) - (site + 4u);
            std::memcpy(blob.code.data() + slot, &rel, 4);
        }
        ASSERT_TRUE(rig.memory.map_region(winapi::CRT_ACCEL_BASE_X86,
                                          winapi::CRT_ACCEL_SIZE,
                                          MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(rig.memory.write_bytes(winapi::CRT_ACCEL_BASE_X86,
                                           blob.code.data(), blob.code.size())
                        .is_ok());
    }
    // A map published after the withdrawal keeps it.
    winapi::DllExports later;
    later.entries.push_back(handler_export("Later", 0, slow));
    rig.add("later.dll", std::move(later));
    const u8* new_map = rig.registry.in_chain_slot_map();
    for (const u8* map : {old_map, new_map}) {
        EXPECT_EQ(slot_byte_in(map, enter), 0u);
        EXPECT_EQ(slot_byte_in(map, leave), 0u);
        EXPECT_EQ(slot_byte_in(map, try_enter), winapi::kInChainSlotHost);
    }

    // for (ebx = N; ebx; --ebx) { Enter(&cs); ++[kCounter]; Leave(&cs); }
    // runs entirely in the guest's own accelerator code: no call-out.
    Code c(kCode);
    c.mov_ebx(kLoops);
    const GuestAddr loop = c.here();
    c.arg0(false, kCs);
    c.call_slot(false, 0);
    c.inc_mem(false, kCounter);
    c.arg0(false, kCs);
    c.call_slot(false, 1);
    c.dec_ebx(false);
    c.jnz_to(loop);
    c.epilogue(false);
    rig.load(c);
    const Outcome o = rig.run(Arm::InChain);
    set_fast_thunk_cs_manager(nullptr);
    ASSERT_TRUE(o.result.success) << o.result.error_message;
    EXPECT_EQ(rig.read32(kCounter), kLoops);
    EXPECT_EQ(o.offers, 0u) << "a withdrawn slot called out";
    EXPECT_EQ(o.cpu.sp(), kSp32 + 4u);
}

// --- m3: full-path diagnostics build no in-chain-only entry -------------------

TEST_F(HostInChainRegistryTest, FullPathDiagnosticsBuildNoInChainOnlyEntry) {
    for (const char* var : {"VITREO_SLOW_THUNK_HIST", "VITREO_TEB_TRACE",
                            "VITREO_TRACE_BAD_CALLER"}) {
        SCOPED_TRACE(var);
        const char* before = std::getenv(var);
        const std::string saved = before != nullptr ? before : "";
        ::setenv(var, "1", 1);
        winapi::host_in_chain::force_for_test(winapi::host_in_chain::Force::None);
        winapi::host_in_chain::detail::g_enabled.store(-1);
        EXPECT_FALSE(winapi::host_in_chain::enabled());
        EXPECT_NE(winapi::host_in_chain::full_path_diagnostic(), nullptr);
        winapi::ThunkRegistry registry{kThunkBase};
        ASSERT_TRUE(registry.register_dll("kernel32.dll", exports()).is_ok());
        const GuestAddr yield = registry.get_thunk_address(
            "kernel32.dll", "SwitchToThread").value();
        bool is_stub = false;
        EXPECT_EQ(registry.in_chain_candidate(yield, is_stub), nullptr);
        EXPECT_EQ(slot_byte(registry, yield), 0u);
        if (before != nullptr) {
            ::setenv(var, saved.c_str(), 1);
        } else {
            ::unsetenv(var);
        }
    }
    // Decided again, from the restored environment, by the next caller.
    winapi::host_in_chain::detail::g_enabled.store(-1);
}
