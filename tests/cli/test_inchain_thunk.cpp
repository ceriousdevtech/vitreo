// SPDX-License-Identifier: LGPL-2.1-only
//
// L3: API crossings served from translated code (in-chain thunk calls).
//
// An API-call tail whose target is a class-A direct export calls it through
// the in-chain stub and resumes at the guest return address without leaving
// JitCompiler::execute() (core/cpu/jit/jit_inchain_thunk.cpp,
// runtime/execution/thunk_inchain.hpp). These tests run real guest code
// through run_execution_loop three ways -- JIT with the in-chain stub, JIT
// without it (the dispatcher crossing), and the interpreter (the CPU
// semantic reference) -- and require the same guest-visible end state, with
// expected values computed from the guest program and the ABI, not from a
// Vitreo run:
//   * Win64 (Learn, "x64 ABI conventions"): RCX/RDX/R8/R9 then [rsp+0x28..]
//     carry the arguments; RBX RBP RDI RSI R12-R15 and XMM6-XMM15 are
//     callee-saved; MXCSR bits 6-15 (control) are nonvolatile.
//   * x86 stdcall (Learn, "__stdcall"): arguments on the stack, the callee
//     pops them; EBX ESI EDI EBP are callee-saved.
//   * The thunk dispatch leaves the guest's flags alone (the interpreter
//     reference), so a CF set before the call is still set after it.

#include <gtest/gtest.h>

#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/jit_stats.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "runtime/execution/guest_run_loop.hpp"
#include "runtime/execution/thunk_direct.hpp"
#include "runtime/execution/thunk_inchain.hpp"
#include "winapi/core/com_direct.hpp"
#include "winapi/core/host_in_chain.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"

#include <vitreo/ab_cycle.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::cli;
using vitreo::cpu::jit::JitCompiler;
using vitreo::winapi::InChainClass;

namespace {

constexpr GuestAddr kMemBase = 0x20000;
constexpr usize kMemSize = 0x10000;
constexpr GuestAddr kCode = 0x21000;
constexpr GuestAddr kNested = 0x22000;    // a guest COM callback
constexpr GuestAddr kTransfer = 0x22800;  // a control-transfer target
constexpr GuestAddr kIat = 0x23000;       // import slots
constexpr GuestAddr kThunkBase = 0x70000000;
constexpr u64 kWitness = 0x1212343456567878ull;
constexpr u32 kLoops = 48;
// Entry stack pointers: [sp] holds the process-return sentinel the program
// RETs to (the run loop then ends with success).
constexpr u32 kSp64 = 0x2FF08;
constexpr u32 kSp32 = 0x2FF00;

/// What the export saw, and what it does to the CPU.
struct Probe {
    u64 calls{0};
    u64 last_args[6]{};
    u64 bad_args{0};
    // SuspendThread during one call (call number, 1-based; 0 = never).
    u64 suspend_on_call{0};
    std::atomic<bool> suspend_returned{false};
    std::atomic<bool> parked_seen{false};
    u64 parked_rbx{0};
    u64 rbx_at_suspend{0};
    // A guest callback from inside the export (AGuestComRef).
    GuestMemory* memory{nullptr};
    winapi::ThunkRegistry* registry{nullptr};
    winapi::ProcessManager* process{nullptr};
    u64 nested_on_call{0};
    u64 nested_result{0};
    // A control transfer from inside the export.
    u64 transfer_on_call{0};
    // Self-modifying code during one call (1-based; 0 = never): the export
    // itself (a host write, class A) or the nested guest callback (a guest
    // store, AGuestComRef) rewrites the loop's `adc r14/esi, imm8` to
    // kSmcImm. smc_addr is that imm8 byte.
    u64 smc_on_call{0};
    bool smc_from_guest{false};
    GuestAddr smc_addr{0};
    // ...and rewrites the loop head's first byte with its own value: no
    // semantic change, but the blocks over it are invalidated too, so the
    // block the chain loop entered is freed whichever block that is.
    GuestAddr smc_loop_addr{0};
    u8 smc_loop_byte{0};
};
constexpr u8 kSmcImm = 0x10;
Probe* g_probe = nullptr;

void clobber_callee_saved(cpu::CpuState& cpu) {
    cpu.gpr.rbx = 0xB0B0B0B0ull;
    cpu.gpr.rbp = 0xB1B1B1B1ull;
    cpu.gpr.rsi = 0xB2B2B2B2ull;
    cpu.gpr.rdi = 0xB3B3B3B3ull;
    if (cpu.is_64bit()) {
        cpu.gpr.r12 = 0xC2;
        cpu.gpr.r13 = 0xC3;
        cpu.gpr.r14 = 0xC4;
        cpu.gpr.r15 = 0xC5;
        cpu.gpr.r10 = 0xD10;  // volatile: the guest must not rely on it
        cpu.gpr.r11 = 0xD11;
        for (int x = 6; x < 16; ++x) {
            cpu.sse.xmm[x].u64_[0] = 0xEEEE0000ull + static_cast<u64>(x);
            cpu.sse.xmm[x].u64_[1] = 0xFFFF0000ull + static_cast<u64>(x);
        }
        // Rounding control changed (down) and a status flag (PE) raised.
        cpu.sse.mxcsr = 0x3FA0u;
    }
}

/// The export behind every probe method: returns the sum of its arguments
/// (six on x64, the three stdcall arguments on x86).
void probe_method(void* /*context*/, winapi::Marshaler& m, u32 /*slot*/) {
    Probe& probe = *g_probe;
    cpu::CpuState& cpu = *m.cpu();
    const bool is64 = cpu.is_64bit();
    const u64 call = ++probe.calls;
    const usize argc = is64 ? 6 : 3;
    u64 sum = 0;
    for (usize i = 0; i < argc; ++i) {
        probe.last_args[i] = m.arg(i);
        sum += probe.last_args[i];
    }
    const u64 expect_tail[6] = {0, 0x22, 0x33, 0x44, 0x55, 0x66};
    for (usize i = 1; i < argc; ++i) {
        if (probe.last_args[i] != expect_tail[i]) ++probe.bad_args;
    }
    if (call == probe.suspend_on_call) {
        // A peer's SuspendThread lands while this thread is in the export.
        const u32 tid = winapi::ThreadManager::get_current_thread_id_static();
        probe.rbx_at_suspend = probe.last_args[0];
        std::thread([tid, &probe] {
            auto& registry = winapi::ThreadSuspendRegistry::instance();
            (void)registry.request_suspend(tid);
            probe.suspend_returned = true;
            // Wait for the target to park, note where, then let it go.
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < deadline) {
                for (const auto& sample : registry.sample_threads()) {
                    if (sample.tid == tid && sample.parked) {
                        probe.parked_rbx = sample.ebx;
                        probe.parked_seen = true;
                    }
                }
                if (probe.parked_seen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            registry.request_resume(tid);
        }).detach();
        // SuspendThread returns while the target is inside a host call.
        while (!probe.suspend_returned) std::this_thread::yield();
    }
    if (call == probe.smc_on_call && !probe.smc_from_guest &&
        probe.memory != nullptr) {
        // An export that writes guest memory holding translated code: the
        // writer's observer invalidates the cached blocks synchronously.
        const u8 imm = kSmcImm;
        if (!probe.memory->write_bytes(probe.smc_addr, &imm, 1).is_ok() ||
            !probe.memory->write_bytes(probe.smc_loop_addr,
                                       &probe.smc_loop_byte, 1).is_ok()) {
            ++probe.bad_args;
        }
    }
    if (call == probe.nested_on_call && probe.memory != nullptr) {
        // A guest COM Release run from inside the export.
        bool completed = false;
        probe.nested_result = run_guest_callback(
            cpu, *probe.memory, *probe.registry, *probe.process, kNested,
            nullptr, 0, is64, &completed);
    }
    clobber_callee_saved(cpu);
    if (call == probe.transfer_on_call) {
        cpu.rip = kTransfer;
        cpu.thunk_control_transfer = true;
        return;
    }
    cpu.gpr.rax = sum;
}

/// Map the registry's thunk slab and give every slot its unmodified 8-byte
/// NOP entry stub, the bytes the runtime publishes there.
void map_thunk_slab(GuestMemory& memory, const winapi::ThunkRegistry& registry) {
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

/// A tiny guest-code builder.
struct Code {
    GuestAddr base;
    std::vector<u8> bytes;
    GuestAddr adc_imm{0};  // the loop's `adc r14/esi, 0` immediate byte
    GuestAddr loop_at{0};  // the loop head
    explicit Code(GuestAddr at) : base(at) {}
    [[nodiscard]] GuestAddr here() const { return base + bytes.size(); }
    void raw(std::initializer_list<u8> b) { bytes.insert(bytes.end(), b); }
    void u32le(u32 v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<u8>(v >> (8 * i)));
    }
    void u64le(u64 v) {
        for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<u8>(v >> (8 * i)));
    }
    void rel32_to(GuestAddr target) {  // after the opcode bytes
        u32le(static_cast<u32>(static_cast<i64>(target) -
                               static_cast<i64>(here() + 4)));
    }
};

/// How the guest reaches the export.
enum class CallForm {
    IndirectMem,  // x64: call [rip+disp32]   x86: call [abs32]
    Rel32,        // call rel32 to the import's entry stub
    JmpTail,      // call helper; helper: jmp [import slot]
    JmpRel32Tail, // call helper; helper: jl/jmp rel32 to the stub (flags live)
    JmpRel32,     // call helper; helper: jmp rel32 to the stub (its own block)
    Gateway,      // call [slot] holding the dispatch gateway itself
};

enum class Arm { InChain, Dispatcher, Interpreter };

struct Outcome {
    RunResult result;
    cpu::CpuState cpu;
    u64 calls{0};
    u64 bad_args{0};
    u64 in_chain_served{0};
    u64 resumed{0};
    u64 left{0};
    u64 broke_outer{0};
    u64 direct{0};
};

class InChainThunkTest : public ::testing::Test {
protected:
    void SetUp() override {
        winapi::com_direct::force_for_test(winapi::com_direct::Force::On);
        set_direct_thunk_counting(true);
        set_in_chain_crossing_counting(true);
        JitCompiler::set_inchain_counting_for_test(true);
        // These tests measure the in-chain path, which a process-wide
        // diagnostic switches off by design (/tmp/vitreo-hostcall-ring or an
        // inherited trace variable): name it instead of failing wholesale.
        if (const char* off = in_chain_full_path_diagnostic()) {
            GTEST_SKIP() << "in-chain crossings off in this process: " << off;
        }
    }
    void TearDown() override {
        winapi::com_direct::force_for_test(winapi::com_direct::Force::None);
        set_direct_thunk_counting(false);
        set_in_chain_crossing_counting(false);
        JitCompiler::set_inchain_counting_for_test(false);
        JitCompiler::set_inchain_thunks_codegen_for_test(-1);
        JitCompiler::set_jit_hot_threshold_for_test(0);
        diagnostics::ab_cycle_force_for_test(diagnostics::AbForce::None);
        g_probe = nullptr;
    }

    /// Run the loop program once in a fresh address space.
    Outcome run(bool is64, CallForm form, InChainClass cls, Arm arm,
                Probe& probe, u32 loops = kLoops) {
        g_probe = &probe;
        JitCompiler::set_inchain_thunks_codegen_for_test(
            arm == Arm::InChain ? 1 : 0);
        JitCompiler::set_jit_hot_threshold_for_test(
            arm == Arm::Interpreter ? JitCompiler::kJitHotThresholdMax : 1);

        GuestMemory memory;
        EXPECT_TRUE(memory.map_region(kMemBase, kMemSize,
                                      MemoryProtection::ReadWrite).is_ok());
        winapi::ThunkRegistry registry{kThunkBase};
        winapi::DllExports exports;
        exports.entries.push_back(winapi::make_direct_export(
            "Probe", winapi::DirectHandler{&probe_method, nullptr, 1},
            // Win32 __stdcall pops its three 4-byte arguments; on x64 the
            // count only bounds the stack arguments (six, two on the stack).
            /*stdcall_bytes=*/is64 ? 24 : 12, cls));
        EXPECT_TRUE(registry.register_dll("probe.dll", std::move(exports))
                        .is_ok());
        const GuestAddr stub =
            registry.get_thunk_address("probe.dll", "Probe").value();
        const GuestAddr gateway = stub + winapi::ThunkRegistry::STUB_SIZE;
        // The guest-visible entry stub is guest memory holding its NOP sled,
        // as the runtime maps it.
        map_thunk_slab(memory, registry);
        const GuestAddr slot_value = form == CallForm::Gateway ? gateway : stub;
        if (is64) {
            EXPECT_TRUE(memory.write_u64(kIat, slot_value).is_ok());
        } else {
            EXPECT_TRUE(memory.write_u32(kIat, static_cast<u32>(slot_value))
                            .is_ok());
        }

        // The control-transfer target: mov ebx, 0xABC; then return to the
        // process sentinel (mov esp, entry sp; ret).
        const u32 entry_sp = is64 ? kSp64 : kSp32;
        const u8 transfer[] = {0xBB, 0xBC, 0x0A, 0x00, 0x00,
                               0xBC, static_cast<u8>(entry_sp),
                               static_cast<u8>(entry_sp >> 8),
                               static_cast<u8>(entry_sp >> 16),
                               static_cast<u8>(entry_sp >> 24), 0xC3};
        EXPECT_TRUE(
            memory.write_bytes(kTransfer, transfer, sizeof transfer).is_ok());

        Code code(kCode);
        // With SMC, the loop head is a block of its own: then the chain
        // settles entered at the continuation, the very block rewritten.
        const bool loop_head_block = probe.smc_on_call != 0;
        if (is64) build_x64(code, form, stub, loops, loop_head_block);
        else build_x86(code, form, stub, loops, loop_head_block);
        EXPECT_TRUE(memory.write_bytes(kCode, code.bytes.data(),
                                       code.bytes.size()).is_ok());
        probe.smc_addr = code.adc_imm;
        probe.smc_loop_addr = code.loop_at;
        probe.smc_loop_byte = code.bytes[code.loop_at - code.base];
        if (probe.smc_on_call != 0) {
            // Code pages are executable, so a write to them is SMC
            // (GuestMemory::notify_executable_write).
            EXPECT_NE(code.adc_imm, 0u);
            EXPECT_TRUE(memory.protect_region(kCode & ~GuestAddr{0xFFF},
                                              0x1000,
                                              MemoryProtection::ReadWriteExecute)
                            .is_ok());
        }

        // The guest COM callback (x64 and x86 alike): mov eax, 7; ret --
        // after `mov byte [smc_addr], kSmcImm` when the guest does the SMC.
        Code nested(kNested);
        if (probe.smc_from_guest) {
            const auto store_byte = [&](GuestAddr at, u8 value) {
                if (is64) nested.raw({0xC6, 0x04, 0x25});  // mov byte [disp32]
                else nested.raw({0xC6, 0x05});             // mov byte [abs32]
                nested.u32le(static_cast<u32>(at));
                nested.raw({value});
            };
            store_byte(code.adc_imm, kSmcImm);
            store_byte(code.loop_at, probe.smc_loop_byte);
        }
        nested.raw({0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3});
        EXPECT_TRUE(memory.write_bytes(kNested, nested.bytes.data(),
                                       nested.bytes.size()).is_ok());

        Outcome out;
        cpu::CpuState& cpu = out.cpu;
        if (is64) {
            cpu.init_long_mode();
            cpu.set_sp(kSp64);  // RSP == 8 (mod 16) at the thunk, as Win64
            EXPECT_TRUE(memory.write_u64(kSp64, PROCESS_RETURN_SENTINEL).is_ok());
            for (int x = 6; x < 16; ++x) {
                cpu.sse.xmm[x].u64_[0] = 0x6000u + static_cast<u64>(x);
                cpu.sse.xmm[x].u64_[1] = 0x7000u + static_cast<u64>(x);
            }
            cpu.sse.mxcsr = 0x1F80u;
        } else {
            cpu.init_protected_mode();
            cpu.set_sp(kSp32);
            EXPECT_TRUE(memory.write_u32(kSp32, static_cast<u32>(
                                                    PROCESS_RETURN_SENTINEL))
                            .is_ok());
        }
        cpu.rip = kCode;
        winapi::ProcessManager process_mgr;
        probe.memory = &memory;
        probe.registry = &registry;
        probe.process = &process_mgr;
        if (probe.nested_on_call != 0) {
            // Resolve the callback loop's fast-thunk set first (setup, not a
            // crossing), as the direct-path tests do.
            const u8 ret = 0xC3;
            EXPECT_TRUE(memory.write_bytes(kNested + 0x100, &ret, 1).is_ok());
            cpu::CpuState warm;
            if (is64) warm.init_long_mode(); else warm.init_protected_mode();
            warm.set_sp(kMemBase + 0x6000);
            bool warm_completed = false;
            (void)run_guest_callback(warm, memory, registry, process_mgr,
                                     kNested + 0x100, nullptr, 0, is64,
                                     &warm_completed);
        }
        RunConfig config;
        config.max_instructions = 1'000'000;
        const u64 served0 = in_chain_crossings_this_thread();
        const u64 resumed0 = JitCompiler::inchain_resumed_this_thread();
        const u64 left0 = JitCompiler::inchain_left_this_thread();
        const u64 outer0 = JitCompiler::inchain_broke_outer_this_thread();
        const u64 direct0 = direct_thunk_crossings_this_thread();
        out.result = run_execution_loop(cpu, memory, registry, process_mgr,
                                        config);
        out.calls = probe.calls;
        out.bad_args = probe.bad_args;
        out.in_chain_served = in_chain_crossings_this_thread() - served0;
        out.resumed = JitCompiler::inchain_resumed_this_thread() - resumed0;
        out.left = JitCompiler::inchain_left_this_thread() - left0;
        out.broke_outer =
            JitCompiler::inchain_broke_outer_this_thread() - outer0;
        out.direct = direct_thunk_crossings_this_thread() - direct0;
        probe.memory = nullptr;
        return out;
    }

    // x64:  for (rbx = N; rbx; --rbx) {
    //         r15 += Probe(rbx, 0x22, 0x33, 0x44, 0x55, 0x66);  // CF set across
    //         r14 += CF; r13 = xmm7 (= r12, set before the call) }
    //       ret      (to the process-return sentinel)
    static void build_x64(Code& c, CallForm form, GuestAddr stub, u32 loops,
                          bool loop_head_block = false) {
        c.raw({0xBB}); c.u32le(loops);              // mov ebx, N
        c.raw({0x45, 0x31, 0xFF});                  // xor r15d, r15d
        c.raw({0x45, 0x31, 0xF6});                  // xor r14d, r14d
        c.raw({0x45, 0x31, 0xED});                  // xor r13d, r13d
        c.raw({0x49, 0xBC}); c.u64le(kWitness);     // mov r12, witness
        if (loop_head_block) c.raw({0xEB, 0x00});   // jmp loop (own block)
        const GuestAddr loop = c.here();
        c.loop_at = loop;
        c.raw({0x48, 0x89, 0xD9});                  // mov rcx, rbx
        c.raw({0xBA, 0x22, 0, 0, 0});               // mov edx, 0x22
        c.raw({0x41, 0xB8, 0x33, 0, 0, 0});         // mov r8d, 0x33
        c.raw({0x41, 0xB9, 0x44, 0, 0, 0});         // mov r9d, 0x44
        c.raw({0x48, 0x83, 0xEC, 0x38});            // sub rsp, 0x38
        c.raw({0x48, 0xC7, 0x44, 0x24, 0x20, 0x55, 0, 0, 0});  // [rsp+0x20]
        c.raw({0x48, 0xC7, 0x44, 0x24, 0x28, 0x66, 0, 0, 0});  // [rsp+0x28]
        c.raw({0x66, 0x49, 0x0F, 0x6E, 0xFC});      // movq xmm7, r12
        if (form != CallForm::JmpRel32Tail) {
            c.raw({0x83, 0xFB, 0x07});              // cmp ebx, 7 (CF iff < 7)
        }
        usize tail_rel = 0;
        switch (form) {
        case CallForm::IndirectMem:
        case CallForm::Gateway:
            c.raw({0xFF, 0x15}); c.rel32_to(kIat);  // call [rip+disp32]
            break;
        case CallForm::Rel32:
            c.raw({0xE8}); c.rel32_to(stub);        // call stub
            break;
        case CallForm::JmpTail:
        case CallForm::JmpRel32Tail:
        case CallForm::JmpRel32:
            c.raw({0xE8}); tail_rel = c.bytes.size(); c.u32le(0);  // call tail
            break;
        }
        if (form == CallForm::JmpRel32Tail) {
            // jge, not jae: a Jcc-first block branches on the carried host
            // NZCV only for a condition every flag writer maps alike
            // (fused_arm_cond, NzcvState::Carried); an unsigned one reads
            // the lazy record and would never see the restore.
            c.raw({0x7D, 0x03});                    // jge +3 (Jcc-first block)
            c.raw({0x49, 0xFF, 0xC6});              // inc r14
        } else {
            c.adc_imm = c.here() + 3;
            c.raw({0x49, 0x83, 0xD6, 0x00});        // adc r14, 0
        }
        c.raw({0x66, 0x49, 0x0F, 0x7E, 0xFD});      // movq r13, xmm7
        c.raw({0x48, 0x83, 0xC4, 0x38});            // add rsp, 0x38
        c.raw({0x49, 0x01, 0xC7});                  // add r15, rax
        c.raw({0xFF, 0xCB});                        // dec ebx
        c.raw({0x75}); c.bytes.push_back(static_cast<u8>(
            static_cast<i64>(loop) - static_cast<i64>(c.here() + 1)));  // jnz
        c.raw({0xC3});                              // ret to the sentinel
        if (form == CallForm::JmpTail || form == CallForm::JmpRel32Tail ||
            form == CallForm::JmpRel32) {
            const GuestAddr tail = c.here();
            const u32 rel = static_cast<u32>(
                static_cast<i64>(tail) -
                static_cast<i64>(c.base + tail_rel + 4));
            for (int i = 0; i < 4; ++i) {
                c.bytes[tail_rel + i] = static_cast<u8>(rel >> (8 * i));
            }
            if (form == CallForm::JmpTail) {
                c.raw({0xFF, 0x25}); c.rel32_to(kIat);  // jmp [rip+disp32]
            } else if (form == CallForm::JmpRel32) {
                c.raw({0xE9}); c.rel32_to(stub);        // jmp stub
            } else {
                // A fused CMP+Jcc whose legs both enter the thunk: the
                // taken leg's constant tail carries the CMP's NZCV live, and
                // so does the fallthrough into the JMP's block. EBX is
                // positive, so the signed compare counts like CF did.
                c.raw({0x83, 0xFB, 0x07});              // cmp ebx, 7
                c.raw({0x0F, 0x8C}); c.rel32_to(stub);  // jl stub
                c.raw({0xE9}); c.rel32_to(stub);        // jmp stub
            }
        }
    }

    // x86:  for (ebx = N; ebx; --ebx) {
    //         edi += Probe(ebx, 0x22, 0x33);  // stdcall; CF set across
    //         esi += CF }   ebp = witness throughout
    //       ret      (to the process-return sentinel)
    static void build_x86(Code& c, CallForm form, GuestAddr stub, u32 loops,
                          bool loop_head_block = false) {
        c.raw({0xBB}); c.u32le(loops);              // mov ebx, N
        c.raw({0x31, 0xFF});                        // xor edi, edi
        c.raw({0x31, 0xF6});                        // xor esi, esi
        c.raw({0xBD}); c.u32le(0x13579BDFu);        // mov ebp, witness
        if (loop_head_block) c.raw({0xEB, 0x00});   // jmp loop (own block)
        const GuestAddr loop = c.here();
        c.loop_at = loop;
        c.raw({0x6A, 0x33});                        // push 0x33
        c.raw({0x6A, 0x22});                        // push 0x22
        c.raw({0x53});                              // push ebx
        if (form != CallForm::JmpRel32Tail) {
            c.raw({0x83, 0xFB, 0x07});              // cmp ebx, 7 (CF iff < 7)
        }
        usize tail_rel = 0;
        switch (form) {
        case CallForm::IndirectMem:
        case CallForm::Gateway:
            c.raw({0xFF, 0x15}); c.u32le(static_cast<u32>(kIat));  // call [abs]
            break;
        case CallForm::Rel32:
            c.raw({0xE8}); c.rel32_to(stub);
            break;
        case CallForm::JmpTail:
        case CallForm::JmpRel32Tail:
        case CallForm::JmpRel32:
            // The helper's JMP leaves the CALL's return address for the
            // export's RET -- and the export pops its stdcall arguments,
            // which sit above that address.
            c.raw({0xE8}); tail_rel = c.bytes.size(); c.u32le(0);
            break;
        }
        if (form == CallForm::JmpRel32Tail) {
            c.raw({0x7D, 0x01});                    // jge +1 (Jcc-first block)
            c.raw({0x46});                          // inc esi
        } else {
            c.adc_imm = c.here() + 2;
            c.raw({0x83, 0xD6, 0x00});              // adc esi, 0
        }
        c.raw({0x01, 0xC7});                        // add edi, eax
        c.raw({0x4B});                              // dec ebx
        c.raw({0x75}); c.bytes.push_back(static_cast<u8>(
            static_cast<i64>(loop) - static_cast<i64>(c.here() + 1)));
        c.raw({0xC3});                              // ret to the sentinel
        if (form == CallForm::JmpTail || form == CallForm::JmpRel32Tail ||
            form == CallForm::JmpRel32) {
            const GuestAddr tail = c.here();
            const u32 rel = static_cast<u32>(
                static_cast<i64>(tail) -
                static_cast<i64>(c.base + tail_rel + 4));
            for (int i = 0; i < 4; ++i) {
                c.bytes[tail_rel + i] = static_cast<u8>(rel >> (8 * i));
            }
            if (form == CallForm::JmpTail) {
                c.raw({0xFF, 0x25}); c.u32le(static_cast<u32>(kIat));
            } else if (form == CallForm::JmpRel32) {
                c.raw({0xE9}); c.rel32_to(stub);    // jmp stub
            } else {
                c.raw({0x83, 0xFB, 0x07});          // cmp ebx, 7
                c.raw({0x0F, 0x8C}); c.rel32_to(stub);  // jl stub (flags live)
                c.raw({0xE9}); c.rel32_to(stub);    // jmp stub
            }
        }
    }

    /// The end state the program defines (independent of how it ran).
    static void expect_program_result(const Outcome& o, bool is64, u32 loops,
                                      const char* arm) {
        SCOPED_TRACE(arm);
        ASSERT_TRUE(o.result.success) << o.result.error_message;
        EXPECT_EQ(o.calls, loops);
        EXPECT_EQ(o.bad_args, 0u);
        const u64 n = loops;
        if (is64) {
            const u64 sum = n * (n + 1) / 2 + n * (0x22 + 0x33 + 0x44 + 0x55 + 0x66);
            EXPECT_EQ(o.cpu.gpr.r15, sum);
            EXPECT_EQ(o.cpu.gpr.r14, std::min<u64>(n, 6))
                << "CF lost across the crossing";
            EXPECT_EQ(o.cpu.gpr.r13, kWitness) << "XMM7 not callee-saved";
            EXPECT_EQ(o.cpu.gpr.r12, kWitness);
            EXPECT_EQ(o.cpu.gpr.rbx, 0u);
            EXPECT_EQ(o.cpu.sp(), kSp64 + 8u);
            EXPECT_EQ(o.cpu.sse.xmm[7].u64_[0], kWitness);
            EXPECT_EQ(o.cpu.sse.xmm[7].u64_[1], 0u);
            for (int x = 6; x < 16; ++x) {
                if (x == 7) continue;
                EXPECT_EQ(o.cpu.sse.xmm[x].u64_[0], 0x6000u + static_cast<u64>(x))
                    << "xmm" << x;
                EXPECT_EQ(o.cpu.sse.xmm[x].u64_[1], 0x7000u + static_cast<u64>(x))
                    << "xmm" << x;
            }
            EXPECT_EQ(o.cpu.sse.mxcsr & 0xFFC0u, 0x1F80u);
        } else {
            const u64 sum = n * (n + 1) / 2 + n * (0x22 + 0x33);
            EXPECT_EQ(o.cpu.gpr.get32(7), static_cast<u32>(sum));  // EDI
            EXPECT_EQ(o.cpu.gpr.get32(6), std::min<u64>(n, 6))
                << "CF lost across the crossing";
            EXPECT_EQ(o.cpu.gpr.get32(5), 0x13579BDFu);             // EBP
            EXPECT_EQ(o.cpu.gpr.get32(3), 0u);                      // EBX
            EXPECT_EQ(o.cpu.sp(), kSp32 + 4u) << "stdcall pop";
        }
    }

    /// Every arm ended in the same guest-visible state.
    static void expect_same_state(const Outcome& a, const Outcome& b,
                                  const char* what) {
        SCOPED_TRACE(what);
        for (u8 r = 0; r < 16; ++r) {
            EXPECT_EQ(a.cpu.gpr.get64(r), b.cpu.gpr.get64(r)) << "gpr " << int(r);
        }
        for (int x = 0; x < 16; ++x) {
            EXPECT_EQ(a.cpu.sse.xmm[x].u64_[0], b.cpu.sse.xmm[x].u64_[0]) << x;
            EXPECT_EQ(a.cpu.sse.xmm[x].u64_[1], b.cpu.sse.xmm[x].u64_[1]) << x;
        }
        EXPECT_EQ(a.cpu.sse.mxcsr & 0xFFC0u, b.cpu.sse.mxcsr & 0xFFC0u);
        EXPECT_EQ(a.calls, b.calls);
    }

    /// The loop's `adc` immediate became kSmcImm during call @p k, so every
    /// iteration from k on adds it: a store to code is seen by the next
    /// fetch of that instruction, which follows the call's return (Intel SDM
    /// Vol. 3A 11.6, "Self-Modifying Code").
    static void expect_smc_result(const Outcome& o, bool is64, u64 k,
                                  const char* arm) {
        SCOPED_TRACE(arm);
        ASSERT_TRUE(o.result.success) << o.result.error_message;
        const u64 n = kLoops;
        EXPECT_EQ(o.calls, n);
        EXPECT_EQ(o.bad_args, 0u);
        const u64 adds = std::min<u64>(n, 6) + kSmcImm * (n - k + 1);
        if (is64) {
            EXPECT_EQ(o.cpu.gpr.r14, adds) << "stale translation ran";
            EXPECT_EQ(o.cpu.gpr.r15, n * (n + 1) / 2 +
                                         n * (0x22 + 0x33 + 0x44 + 0x55 + 0x66));
            EXPECT_EQ(o.cpu.sp(), kSp64 + 8u);
        } else {
            EXPECT_EQ(o.cpu.gpr.get32(6), adds) << "stale translation ran";
            EXPECT_EQ(o.cpu.gpr.get32(7),
                      static_cast<u32>(n * (n + 1) / 2 + n * (0x22 + 0x33)));
            EXPECT_EQ(o.cpu.sp(), kSp32 + 4u);
        }
    }

    /// Code rewritten during call kLoops / 2, by the export (class A) or by
    /// the guest callback it runs (AGuestComRef), in all three arms.
    void smc_during_the_crossing(bool from_guest) {
        const u64 k = kLoops / 2;
        const InChainClass cls =
            from_guest ? InChainClass::AGuestComRef : InChainClass::A;
        for (const bool is64 : {true, false}) {
            SCOPED_TRACE(is64 ? "x64" : "x86");
            Outcome o[3];
            const Arm arms[3] = {Arm::InChain, Arm::Dispatcher,
                                 Arm::Interpreter};
            for (int a = 0; a < 3; ++a) {
                Probe probe;
                probe.smc_on_call = k;
                probe.smc_from_guest = from_guest;
                if (from_guest) probe.nested_on_call = k;
                o[a] = run(is64, CallForm::IndirectMem, cls, arms[a], probe);
                if (from_guest) EXPECT_EQ(probe.nested_result, 7u);
            }
            expect_smc_result(o[0], is64, k, "in-chain");
            expect_smc_result(o[1], is64, k, "dispatcher");
            expect_smc_result(o[2], is64, k, "interpreter");
            expect_same_state(o[0], o[2], "in-chain vs interpreter");
            expect_same_state(o[1], o[2], "dispatcher vs interpreter");
            // The rewriting crossing was served in-chain and left -- for the
            // OUTER dispatcher, not the chain loop, which holds the freed
            // block (it is the return block, so its self-RIP path would run
            // it again); the rest of the loop still crosses in-chain.
            EXPECT_GE(o[0].in_chain_served, kLoops - 4);
            EXPECT_GE(o[0].left, 1u);
            EXPECT_GE(o[0].broke_outer, 1u)
                << "a crossing that freed the chain's block let it continue";
            EXPECT_GE(o[0].resumed, kLoops / 2 - 4);
        }
    }

    void parity(bool is64, CallForm form) {
        Probe p_in, p_disp, p_interp;
        const Outcome in = run(is64, form, InChainClass::A, Arm::InChain, p_in);
        const Outcome disp =
            run(is64, form, InChainClass::A, Arm::Dispatcher, p_disp);
        const Outcome interp =
            run(is64, form, InChainClass::A, Arm::Interpreter, p_interp);
        expect_program_result(in, is64, kLoops, "in-chain");
        expect_program_result(disp, is64, kLoops, "dispatcher");
        expect_program_result(interp, is64, kLoops, "interpreter");
        expect_same_state(in, interp, "in-chain vs interpreter");
        expect_same_state(disp, interp, "dispatcher vs interpreter");
        // Every call was the direct crossing; once the loop is compiled,
        // the calls are served in-chain and resume in translated code: no
        // dispatcher entry between them.
        EXPECT_EQ(in.direct, kLoops);
        EXPECT_GE(in.in_chain_served, kLoops - 4);
        EXPECT_GE(in.resumed, kLoops - 4);
        EXPECT_EQ(in.resumed + in.left, in.in_chain_served);
        // Without the stub nothing is served in-chain.
        EXPECT_EQ(disp.in_chain_served, 0u);
        EXPECT_EQ(disp.resumed, 0u);
        EXPECT_EQ(interp.in_chain_served, 0u);
    }
};

}  // namespace

TEST_F(InChainThunkTest, X64IndirectCallMatchesInterpreterAndStaysInChain) {
    parity(true, CallForm::IndirectMem);
}

TEST_F(InChainThunkTest, X64CallRel32ToTheStubMatchesInterpreter) {
    parity(true, CallForm::Rel32);
}

TEST_F(InChainThunkTest, X64JmpTailCallReturnsToTheCallersCaller) {
    parity(true, CallForm::JmpTail);
}

TEST_F(InChainThunkTest, X64CallThroughTheGatewayMatchesInterpreter) {
    parity(true, CallForm::Gateway);
}

TEST_F(InChainThunkTest, X86StdcallIndirectCallPopsArgumentsInChain) {
    parity(false, CallForm::IndirectMem);
}

TEST_F(InChainThunkTest, X86CallRel32ToTheStubMatchesInterpreter) {
    parity(false, CallForm::Rel32);
}

TEST_F(InChainThunkTest, X86JmpTailCallReturnsToTheCallersCaller) {
    parity(false, CallForm::JmpTail);
}

TEST_F(InChainThunkTest, X64JmpRel32TailCarriesLiveFlagsAcrossTheCrossing) {
    // CMP's flags are live in host NZCV when the constant thunk tail leaves
    // the helper block; the guest's ADC after the return must still see CF.
    parity(true, CallForm::JmpRel32Tail);
}

TEST_F(InChainThunkTest, X86JmpRel32TailCarriesLiveFlagsAcrossTheCrossing) {
    parity(false, CallForm::JmpRel32Tail);
}

TEST_F(InChainThunkTest, ClassNoneExportIsNeverServedInChain) {
    for (const bool is64 : {true, false}) {
        Probe probe;
        const Outcome o = run(is64, CallForm::IndirectMem, InChainClass::None,
                              Arm::InChain, probe);
        expect_program_result(o, is64, kLoops, is64 ? "x64" : "x86");
        EXPECT_EQ(o.in_chain_served, 0u);
        EXPECT_EQ(o.resumed, 0u);
        EXPECT_EQ(o.direct, kLoops);  // still the direct crossing, outside
    }
}

TEST_F(InChainThunkTest, InProcessOffArmAndComDirectOffTakeTheDispatcherPath) {
    // VITREO_AB_CYCLE_ONLY=l3's off arm declines at run time.
    {
        diagnostics::ab_cycle_force_for_test(diagnostics::AbForce::Off);
        Probe probe;
        const Outcome o = run(true, CallForm::IndirectMem, InChainClass::A,
                              Arm::InChain, probe);
        expect_program_result(o, true, kLoops, "ab-cycle off");
        EXPECT_EQ(o.in_chain_served, 0u);
        EXPECT_EQ(o.resumed, 0u);
        // The in-process arms alternate at run time, so the stub stays.
        EXPECT_TRUE(JitCompiler::inchain_thunks_codegen());
        diagnostics::ab_cycle_force_for_test(diagnostics::AbForce::None);
    }
    // In-chain IS the direct crossing: VITREO_D3D11_COM_DIRECT=0 stops both
    // (with L3-K, the kernel32 in-chain lever, off too: nothing is left).
    {
        winapi::com_direct::force_for_test(winapi::com_direct::Force::Off);
        winapi::host_in_chain::force_for_test(
            winapi::host_in_chain::Force::Off);
        Probe probe;
        const Outcome o = run(true, CallForm::IndirectMem, InChainClass::A,
                              Arm::InChain, probe);
        expect_program_result(o, true, kLoops, "com-direct off");
        EXPECT_EQ(o.in_chain_served, 0u);
        EXPECT_EQ(o.direct, 0u);
        // Nothing could be served in-chain, so the run loop closed the
        // codegen gate: API-call tails were emitted without the stub.
        EXPECT_FALSE(JitCompiler::inchain_thunks_codegen())
            << "VITREO_D3D11_COM_DIRECT=0 still emits the in-chain stub";
        winapi::host_in_chain::force_for_test(
            winapi::host_in_chain::Force::None);
        winapi::com_direct::force_for_test(winapi::com_direct::Force::On);
    }
}

TEST_F(InChainThunkTest, PerCallDiagnosticForcesTheFullPath) {
    // VITREO_SLOW_HOSTCALL_DIAG times the full dispatch path per call.
    ASSERT_EQ(::setenv("VITREO_SLOW_HOSTCALL_DIAG", "1", 1), 0);
    Probe probe;
    const Outcome o = run(true, CallForm::IndirectMem, InChainClass::A,
                          Arm::InChain, probe);
    ::unsetenv("VITREO_SLOW_HOSTCALL_DIAG");
    expect_program_result(o, true, kLoops, "slow-hostcall diag");
    EXPECT_EQ(o.in_chain_served, 0u);
    EXPECT_EQ(o.resumed, 0u);
}

TEST_F(InChainThunkTest, GuestCallbackInsideTheExportResumesThroughDispatcher) {
    // AGuestComRef: the export runs a guest COM Release (run_guest_callback)
    // on one call. That crossing leaves through the dispatcher; the others
    // still resume in-chain, and the program's result is unchanged.
    for (const bool is64 : {true, false}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        Probe plain;
        const Outcome base = run(is64, CallForm::IndirectMem,
                                 InChainClass::AGuestComRef, Arm::InChain,
                                 plain);
        Probe probe;
        probe.nested_on_call = kLoops / 2;
        const Outcome o = run(is64, CallForm::IndirectMem,
                              InChainClass::AGuestComRef, Arm::InChain, probe);
        expect_program_result(o, is64, kLoops, "nested callback");
        EXPECT_EQ(probe.nested_result, 7u);
        // Exactly the one crossing that ran guest code left through the
        // dispatcher instead of resuming.
        EXPECT_EQ(o.in_chain_served, base.in_chain_served);
        EXPECT_EQ(o.left, base.left + 1);
        EXPECT_EQ(o.resumed + 1, base.resumed);
    }
}

TEST_F(InChainThunkTest, ExportWritingTranslatedCodeLeavesTheChainLoop) {
    // F1: a write to guest memory holding translated code frees the cached
    // blocks it overlaps on the spot (the writer's executable-write
    // observer). Here the export rewrites the loop's continuation -- the
    // block the chain loop entered and holds. The crossing must leave for
    // the OUTER dispatcher without the chain loop touching that block again
    // (reading it, or re-running it on its self-RIP path), and the
    // rewritten instruction must take effect. (The stale read itself does
    // not fault here: measured 2026-10-12, neither guard malloc nor
    // MallocScribble caught it -- the freed block read back as zeros -- so
    // the test checks the rule instead: this crossing breaks Outer.)
    smc_during_the_crossing(/*from_guest=*/false);
}

TEST_F(InChainThunkTest, GuestCallbackWritingTranslatedCodeLeavesTheChainLoop) {
    // F1, AGuestComRef: the guest COM callback run from inside the export
    // stores over the same block (a nested dispatch frees it), and the
    // crossing reports BreakOuter -- which the chain loop must act on before
    // it reads the block it executed.
    smc_during_the_crossing(/*from_guest=*/true);
}

TEST_F(InChainThunkTest, ControlTransferFromTheExportLeavesTheChain) {
    // A handler that transfers control (a class-A export never does by
    // contract; if one did) must reach the outer dispatcher with the rip it
    // chose: the transfer target sets EBX = 0xABC and halts.
    for (const bool is64 : {true, false}) {
        Probe probe;
        probe.transfer_on_call = 5;
        const Outcome o = run(is64, CallForm::IndirectMem, InChainClass::A,
                              Arm::InChain, probe);
        ASSERT_TRUE(o.result.success) << o.result.error_message;
        EXPECT_EQ(o.calls, 5u);
        EXPECT_EQ(o.cpu.gpr.get32(3), 0xABCu) << (is64 ? "x64" : "x86");
        EXPECT_GE(o.left, 1u);
    }
}

TEST_F(InChainThunkTest, SuspendThreadDuringTheCrossingParksBeforeMoreGuestCode) {
    // SuspendThread returns while the target is inside the export (a host
    // call); the target must then park before executing another guest
    // instruction -- with RBX still the value it passed (the loop's DEC runs
    // after the call) -- and not resume in translated code past it.
    Probe probe;
    probe.suspend_on_call = kLoops / 2;
    const Outcome o = run(true, CallForm::IndirectMem, InChainClass::A,
                          Arm::InChain, probe);
    expect_program_result(o, true, kLoops, "suspended mid-crossing");
    EXPECT_TRUE(probe.suspend_returned.load());
    ASSERT_TRUE(probe.parked_seen.load());
    EXPECT_EQ(probe.parked_rbx, probe.rbx_at_suspend);
    EXPECT_GE(o.left, 1u);
}

TEST_F(InChainThunkTest, ApiCallExitPastItsBranchReachRunsInTheInterpreter) {
    // A block whose in-chain exit cannot be laid out (a B past its imm26
    // reach, JitCompiler::patch_api_call_b) is declined to the interpreter,
    // like any block with a deferred branch that cannot reach -- never a
    // process stop. VITREO_JIT_TEST_BRANCH_REACH_WORDS=2 gives every deferred
    // branch that reach. The helper the loop calls is one `jmp stub`, a block
    // with no other branch that long (only the one-word B into its
    // continue-exit stub), so at full reach it is served in-chain and here it
    // is not, while the program still ends in the state it defines.
    struct ScopedEnv {
        const char* name;
        ScopedEnv(const char* n, const char* value) : name(n) {
            ::setenv(n, value, 1);
            cpu::jit::JitCompiler::reload_branch_reach_test_knobs_for_test();
        }
        ~ScopedEnv() {
            ::unsetenv(name);
            cpu::jit::JitCompiler::reload_branch_reach_test_knobs_for_test();
        }
        ScopedEnv(const ScopedEnv&) = delete;
        ScopedEnv& operator=(const ScopedEnv&) = delete;
    };
    for (const bool is64 : {true, false}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        Probe p_full;
        const Outcome full = run(is64, CallForm::JmpRel32, InChainClass::A,
                                 Arm::InChain, p_full);
        expect_program_result(full, is64, kLoops, "full reach");
        ASSERT_GT(full.in_chain_served, 0u)
            << "the tail is no longer served in-chain at full reach";

        Outcome capped;
        u64 declines = 0;
        {
            ScopedEnv fatal("VITREO_JIT_TEST_BRANCH_REACH_FATAL", "0");
            ScopedEnv reach("VITREO_JIT_TEST_BRANCH_REACH_WORDS", "2");
            const u64 before = cpu::jit::stats::branch_reach_declines();
            Probe p_capped;
            capped = run(is64, CallForm::JmpRel32, InChainClass::A,
                         Arm::InChain, p_capped);
            declines = cpu::jit::stats::branch_reach_declines() - before;
        }
        Probe p_interp;
        const Outcome interp = run(is64, CallForm::JmpRel32,
                                   InChainClass::A, Arm::Interpreter, p_interp);
        expect_program_result(capped, is64, kLoops, "past reach");
        expect_program_result(interp, is64, kLoops, "interpreter");
        expect_same_state(capped, interp, "past reach vs interpreter");
        EXPECT_GT(declines, 0u);
        EXPECT_EQ(capped.in_chain_served, 0u)
            << "a block whose in-chain exit cannot reach was laid out";
        EXPECT_EQ(capped.resumed, 0u);
    }
}

// ---------------------------------------------------------------------------
// The serve hook on its own (no translated code): every outcome it reports.
// ---------------------------------------------------------------------------

namespace {

struct ServeProbe {
    u64 calls{0};
    bool raise{false};
    bool unwind{false};
    bool transfer{false};
};
ServeProbe g_serve{};

class ServeInChainTest : public ::testing::Test {
protected:
    void SetUp() override {
        winapi::com_direct::force_for_test(winapi::com_direct::Force::On);
        ASSERT_TRUE(memory.map_region(kMemBase, kMemSize,
                                      MemoryProtection::ReadWrite).is_ok());
        winapi::DllExports exports;
        exports.entries.push_back(winapi::make_direct_export(
            "A", winapi::DirectHandler{&serve_probe, nullptr, 1}, 12,
            InChainClass::A));
        exports.entries.push_back(winapi::make_direct_export(
            "B", winapi::DirectHandler{&serve_probe, nullptr, 2}, 12,
            InChainClass::None));
        exports.entries.push_back(winapi::ExportEntry{
            "Plain", 1, [](winapi::Marshaler& m) { m.set_return(1); }});
        ASSERT_TRUE(registry.register_dll("serve.dll", std::move(exports))
                        .is_ok());
        map_thunk_slab(memory, registry);
        g_serve = ServeProbe{};
        // These tests measure the in-chain path, which a process-wide
        // diagnostic switches off by design (/tmp/vitreo-hostcall-ring or an
        // inherited trace variable): name it instead of failing wholesale.
        if (const char* off = in_chain_full_path_diagnostic()) {
            GTEST_SKIP() << "in-chain crossings off in this process: " << off;
        }
    }
    void TearDown() override {
        winapi::com_direct::force_for_test(winapi::com_direct::Force::None);
        diagnostics::ab_cycle_force_for_test(diagnostics::AbForce::None);
    }

    static void serve_probe(void*, winapi::Marshaler& m, u32) {
        ++g_serve.calls;
        cpu::CpuState& cpu = *m.cpu();
        if (g_serve.raise) cpu.pending_raise.active = true;
        if (g_serve.unwind) cpu.pending_unwind.active = true;
        if (g_serve.transfer) {
            cpu.rip = 0x22800;
            cpu.thunk_control_transfer = true;
        }
        cpu.gpr.rax = 0x99;
    }

    GuestAddr stub(const char* name) {
        return registry.get_thunk_address("serve.dll", name).value();
    }

    cpu::jit::InChainThunkResult serve(cpu::CpuState& cpu) {
        winapi::Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        winapi::ThreadSuspendRegistry::PublishGuard guard(
            winapi::ThreadManager::get_current_thread_id_static(), &cpu);
        InChainThunkEnv env{&registry, &memory, &marshaler, &guard, nullptr,
                            false};
        return serve_in_chain_thunk(cpu, &env);
    }

    cpu::CpuState x64_at(GuestAddr rip) {
        cpu::CpuState cpu;
        cpu.init_long_mode();
        cpu.set_sp(0x2F000);
        EXPECT_TRUE(memory.write_u64(0x2F000, 0x21234).is_ok());
        cpu.rip = rip;
        return cpu;
    }

    GuestMemory memory;
    winapi::ThunkRegistry registry{kThunkBase};
};

}  // namespace

TEST_F(ServeInChainTest, ClassAThroughAnIntactStubReturnsToTheCaller) {
    cpu::CpuState cpu = x64_at(stub("A"));
    EXPECT_EQ(serve(cpu), cpu::jit::InChainThunkResult::Returned);
    EXPECT_EQ(g_serve.calls, 1u);
    EXPECT_EQ(cpu.rip, 0x21234u);
    EXPECT_EQ(cpu.sp(), 0x2F008u);
    EXPECT_EQ(cpu.gpr.rax, 0x99u);
}

TEST_F(ServeInChainTest, AnythingNotProvenClassAIsNotServedAndUntouched) {
    for (const char* name : {"B", "Plain"}) {
        cpu::CpuState cpu = x64_at(stub(name));
        EXPECT_EQ(serve(cpu), cpu::jit::InChainThunkResult::NotServed) << name;
        EXPECT_EQ(cpu.rip, stub(name)) << name;
        EXPECT_EQ(cpu.sp(), 0x2F000u) << name;
    }
    // A stub a guest hook patched is guest code, not the export.
    const GuestAddr patched = stub("A");
    const u8 jmp[] = {0xE9, 0x00, 0x10, 0x00, 0x00};
    ASSERT_TRUE(memory.write_bytes(patched, jmp, sizeof jmp).is_ok());
    cpu::CpuState cpu = x64_at(patched);
    EXPECT_EQ(serve(cpu), cpu::jit::InChainThunkResult::NotServed);
    EXPECT_EQ(cpu.rip, patched);
    EXPECT_EQ(g_serve.calls, 0u);
    // The in-process off arm declines too.
    diagnostics::ab_cycle_force_for_test(diagnostics::AbForce::Off);
    cpu::CpuState gate = x64_at(stub("A") + winapi::ThunkRegistry::STUB_SIZE);
    EXPECT_EQ(serve(gate), cpu::jit::InChainThunkResult::NotServed);
    EXPECT_EQ(g_serve.calls, 0u);
}

TEST_F(ServeInChainTest, RaiseUnwindAndTransferLeaveForTheOuterDispatcher) {
    const GuestAddr gateway = stub("A") + winapi::ThunkRegistry::STUB_SIZE;
    {
        g_serve.raise = true;
        cpu::CpuState cpu = x64_at(gateway);
        EXPECT_EQ(serve(cpu), cpu::jit::InChainThunkResult::BreakOuter);
        EXPECT_EQ(cpu.rip, gateway) << "a raise emulates no RET";
        g_serve.raise = false;
    }
    {
        g_serve.unwind = true;
        cpu::CpuState cpu = x64_at(gateway);
        EXPECT_EQ(serve(cpu), cpu::jit::InChainThunkResult::BreakOuter);
        g_serve.unwind = false;
    }
    {
        g_serve.transfer = true;
        cpu::CpuState cpu = x64_at(gateway);
        EXPECT_EQ(serve(cpu), cpu::jit::InChainThunkResult::BreakOuter);
        EXPECT_EQ(cpu.rip, 0x22800u);
        g_serve.transfer = false;
    }
    {
        // The return address is unreadable: the full path's stack underflow.
        cpu::CpuState cpu = x64_at(gateway);
        cpu.set_sp(0x10);
        EXPECT_EQ(serve(cpu), cpu::jit::InChainThunkResult::Fatal);
    }
}

TEST_F(ServeInChainTest, ExportsThatEndTheThreadOrProcessAreNeverInChain) {
    // No export that ends the thread or process may ever be in-chain: its
    // exit stays the dispatcher's (the serve's own has_exited() test is for
    // another thread's ExitProcess). Structural, not by convention: whatever
    // class such an export is registered with -- here A and AGuestComRef, as
    // a widened L3 might misclassify it -- register_dll registers it None.
    struct Dll {
        const char* name;
        std::vector<const char*> exits;
    };
    const Dll dlls[] = {
        {"kernel32.dll",
         {"ExitProcess", "ExitThread", "FreeLibraryAndExitThread",
          "TerminateProcess", "TerminateThread"}},
        {"ntdll.dll",
         {"RtlExitUserProcess", "RtlExitUserThread", "NtTerminateProcess",
          "ZwTerminateProcess", "NtTerminateThread", "ZwTerminateThread"}},
        {"ucrtbase.dll", {"exit", "_exit", "_Exit", "quick_exit", "abort"}},
    };
    u32 slot = 10;
    for (const Dll& dll : dlls) {
        winapi::DllExports exports;
        for (const char* name : dll.exits) {
            EXPECT_TRUE(winapi::ends_thread_or_process(name)) << name;
            const InChainClass claimed =
                slot % 2 == 0 ? InChainClass::A : InChainClass::AGuestComRef;
            exports.entries.push_back(winapi::make_direct_export(
                name, winapi::DirectHandler{&serve_probe, nullptr, slot}, 0,
                claimed));
            ++slot;
        }
        // A neighbour that is not an exit keeps its class.
        exports.entries.push_back(winapi::make_direct_export(
            "NotAnExit", winapi::DirectHandler{&serve_probe, nullptr, slot++},
            0, InChainClass::A));
        ASSERT_TRUE(registry.register_dll(dll.name, std::move(exports)).is_ok())
            << dll.name;
    }
    const u8* map = registry.in_chain_slot_map();
    const auto map_byte = [&](GuestAddr stub) {
        return map[sizeof(u64) + ((stub - kThunkBase) >>
                                  winapi::ThunkRegistry::kInChainSlotShift)];
    };
    for (const Dll& dll : dlls) {
        for (const char* name : dll.exits) {
            SCOPED_TRACE(name);
            const GuestAddr stub =
                registry.get_thunk_address(dll.name, name).value();
            const winapi::DirectThunk* direct = registry.direct_thunk(
                stub + winapi::ThunkRegistry::STUB_SIZE);
            ASSERT_NE(direct, nullptr) << "still the direct crossing";
            EXPECT_EQ(direct->in_chain, InChainClass::None);
            EXPECT_EQ(map_byte(stub), 0u) << "translated code would call out";
            // Neither the stub nor the gateway is served in-chain.
            for (const GuestAddr at :
                 {stub, stub + winapi::ThunkRegistry::STUB_SIZE}) {
                cpu::CpuState cpu = x64_at(at);
                EXPECT_EQ(serve(cpu), cpu::jit::InChainThunkResult::NotServed);
                EXPECT_EQ(cpu.rip, at);
            }
        }
        const GuestAddr other =
            registry.get_thunk_address(dll.name, "NotAnExit").value();
        EXPECT_EQ(map_byte(other), 1u) << dll.name;
        EXPECT_EQ(registry.direct_thunk(other + winapi::ThunkRegistry::STUB_SIZE)
                      ->in_chain,
                  InChainClass::A)
            << dll.name;
    }
    EXPECT_EQ(g_serve.calls, 0u);
    for (const char* name : {"Draw", "Release", "ExitProcessEx", "Exit", ""}) {
        EXPECT_FALSE(winapi::ends_thread_or_process(name)) << name;
    }
}
