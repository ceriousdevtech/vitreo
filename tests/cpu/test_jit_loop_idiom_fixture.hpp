// SPDX-License-Identifier: LGPL-2.1-only
// WS2 loop-idiom tests: shared decode helper and the interpreter / JIT
// differential runner.
#pragma once

#include <gtest/gtest.h>

#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/loop_idiom_gate.hpp"
#include "core/cpu/jit/loop_idiom_matcher.hpp"
#include "ir_tier1_first_compile.hpp"
#include "jit_dispatch_execute.hpp"

namespace vitreo::cpu::jit::loop_idiom::test {

inline constexpr u64 kCodeBase = 0x1000;
inline constexpr u64 kDataBase = 0x20000;
inline constexpr u64 kDataSize = 0x40000;

inline BasicBlock decode_block(std::span<const u8> code, bool is64,
                               u64 start = kCodeBase) {
    BasicBlock block;
    block.start_addr = start;
    block.is_64bit = is64;
    MemoryByteReader reader(code.data(), code.size());
    while (reader.remaining() > 0) {
        DecodedInstruction inst;
        const DecodeError err = decode_instruction(
            reader, is64 ? DecodeMode::Mode64 : DecodeMode::Mode32, inst);
        if (err != DecodeError::Success) break;
        block.instructions.push_back(inst);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    return block;
}

inline MatchResult match_bytes(std::span<const u8> code, bool is64,
                               MatchConfig cfg = {}) {
    const BasicBlock b = decode_block(code, is64);
    return match_loop_idiom(b, cfg);
}

/// Guest memory for one run: the code page plus whatever the case maps.
using MemSetup = std::function<void(GuestMemory&)>;
using CpuSetup = std::function<void(CpuState&)>;

/// Default data layout: 0x20000..0x60000 RW, filled with a byte pattern.
inline void default_data(GuestMemory& mem) {
    ASSERT_TRUE(mem.map_region(kDataBase, kDataSize, MemoryProtection::ReadWrite));
    std::vector<u8> fill(kDataSize);
    for (usize i = 0; i < fill.size(); ++i) {
        fill[i] = static_cast<u8>(0x40 + (i * 7 + (i >> 8)) % 0xB0);  // never 0
    }
    ASSERT_TRUE(mem.write_bytes(kDataBase, fill.data(), fill.size()));
}

struct LoopOutcome {
    CpuState cpu{};
    std::vector<u8> data;
    bool faulted{false};
    bool page_fault{false};  ///< interpreter: #PF (others report no address)
    u64 fault_addr{0};
    u32 code{0};
    u64 info0{0}, info1{0};
    bool bulked_first_entry{false};
    u64 entries{0};
    bool tier1_active{false};
};

inline std::unique_ptr<GuestMemory> make_memory(std::span<const u8> code,
                                                const MemSetup& setup) {
    auto mem = std::make_unique<GuestMemory>();
    EXPECT_TRUE(mem->map_region(kCodeBase, 0x1000, MemoryProtection::ReadWrite));
    EXPECT_TRUE(mem->write_bytes(kCodeBase, code.data(), code.size()));
    EXPECT_TRUE(mem->protect_region(kCodeBase, 0x1000, MemoryProtection::ReadExecute));
    if (setup) setup(*mem);
    return mem;
}

inline void read_data(GuestMemory& mem, std::vector<u8>& out) {
    out.assign(kDataSize, 0);
    // Region by region so an unmapped hole does not lose the rest.
    for (u64 off = 0; off < kDataSize; off += 0x1000) {
        if (!mem.read_bytes(kDataBase + off, out.data() + off, 0x1000)) {
            std::memset(out.data() + off, 0xEE, 0x1000);
        }
    }
}

inline void init_cpu(CpuState& cpu, bool is64) {
    if (is64) cpu.init_long_mode();
    else cpu.init_protected_mode();
    cpu.rip = kCodeBase;
}

inline LoopOutcome run_interpreter(std::span<const u8> code, bool is64,
                                   const MemSetup& mem_setup, const CpuSetup& cpu_setup) {
    LoopOutcome out;
    auto mem = make_memory(code, mem_setup);
    init_cpu(out.cpu, is64);
    if (cpu_setup) cpu_setup(out.cpu);
    Interpreter interp(out.cpu, *mem);
    const u64 end = kCodeBase + code.size();
    for (u64 steps = 0; steps < 20'000'000; ++steps) {
        if (out.cpu.rip < kCodeBase || out.cpu.rip >= end) break;
        const ExecResult r = interp.step();
        if (r == ExecResult::Exception) {
            out.faulted = true;
            out.page_fault = interp.context().exception == CpuException::PageFault;
            out.fault_addr = interp.context().exception_addr;
            break;
        }
    }
    out.cpu.materialize_lazy_flags();
    read_data(*mem, out.data);
    return out;
}

inline LoopOutcome run_jit(std::span<const u8> code, bool is64, const MemSetup& mem_setup,
                           const CpuSetup& cpu_setup, Force force, bool tier1 = false) {
    ScopedForce scoped(force);
    LoopOutcome out;
    auto mem = make_memory(code, mem_setup);
    init_cpu(out.cpu, is64);
    if (cpu_setup) cpu_setup(out.cpu);
    // Tier 1 (ADR-0002): a first compile is tier 0 under tiering, so pin the
    // IR to the first compile for this run.
    std::optional<vitreo::cpu::jit::test::IrTier1FirstCompile> tier1_scope;
    if (tier1) {
        JitCompiler::reset_ir_tier_for_test();
        tier1_scope.emplace();
    }
    JitCompiler jit(*mem);
    BasicBlock block = decode_block(code, is64);
    CompiledBlock* compiled = jit.compile(block);
    EXPECT_NE(compiled, nullptr);
    if (compiled == nullptr) return out;
    out.tier1_active = jit.ir_emit_active();
    CompiledBlock head = *compiled;
    // The in-block loop-back cap (MAX_IN_BLOCK_LOOP_ITERS) can leave RIP
    // inside the loop (at the back-edge Jcc); the dispatcher would then run
    // a block starting there. Compile those on demand, like it does.
    std::vector<std::pair<u64, CompiledBlock>> inner;
    auto block_at = [&](u64 rip) -> CompiledBlock* {
        if (rip == kCodeBase) return &head;
        for (auto& [r, b] : inner) if (r == rip) return &b;
        const BasicBlock b = decode_block(code.subspan(rip - kCodeBase), is64, rip);
        CompiledBlock* c = jit.compile(b);
        if (c == nullptr) return nullptr;
        inner.emplace_back(rip, *c);
        return &inner.back().second;
    };
    const u64 end = kCodeBase + code.size();
    for (u64 guard = 0; guard < 1'000'000; ++guard) {
        if (out.cpu.rip < kCodeBase || out.cpu.rip >= end || out.cpu.pending_raise.active) break;
        CompiledBlock* cb = block_at(out.cpu.rip);
        EXPECT_NE(cb, nullptr);
        if (cb == nullptr) break;
        const u64 before = bulk_runs();
        (void)vitreo::cpu::jit::test::execute_like_dispatcher(jit, *cb, out.cpu);
        if (guard == 0) out.bulked_first_entry = bulk_runs() != before;
        ++out.entries;
    }
    if (out.cpu.pending_raise.active) {
        out.faulted = true;
        out.code = out.cpu.pending_raise.code;
        out.info0 = out.cpu.pending_raise.args[0];
        out.info1 = out.cpu.pending_raise.args[1];
        out.fault_addr = out.info1;
    }
    out.cpu.materialize_lazy_flags();
    read_data(*mem, out.data);
    return out;
}

/// Architectural comparison. `exact` adds AF, the exception record and
/// YMM-high (used for JIT On vs JIT Off, which must be identical).
inline void expect_same(const LoopOutcome& a, const LoopOutcome& b, const char* what,
                        bool exact) {
    SCOPED_TRACE(what);
    for (u8 i = 0; i < 16; ++i) {
        EXPECT_EQ(a.cpu.gpr.get64(i), b.cpu.gpr.get64(i)) << "GPR " << int(i);
    }
    EXPECT_EQ(a.cpu.rip, b.cpu.rip) << "RIP";
    EXPECT_EQ(a.cpu.flags.cf(), b.cpu.flags.cf()) << "CF";
    EXPECT_EQ(a.cpu.flags.pf(), b.cpu.flags.pf()) << "PF";
    EXPECT_EQ(a.cpu.flags.zf(), b.cpu.flags.zf()) << "ZF";
    EXPECT_EQ(a.cpu.flags.sf(), b.cpu.flags.sf()) << "SF";
    EXPECT_EQ(a.cpu.flags.of(), b.cpu.flags.of()) << "OF";
    EXPECT_EQ(a.cpu.flags.df(), b.cpu.flags.df()) << "DF";
    for (u8 r = 0; r < 16; ++r) {
        EXPECT_EQ(0, std::memcmp(a.cpu.sse.xmm[r].u8_, b.cpu.sse.xmm[r].u8_, 16))
            << "XMM" << int(r);
    }
    EXPECT_EQ(a.faulted, b.faulted) << "fault";
    // The interpreter reports an address only for #PF; the JIT reports #GP
    // as an access violation at ~0 (the Windows record). Compare the
    // address where both carry one.
    if (a.faulted && b.faulted && (exact || a.page_fault)) {
        EXPECT_EQ(a.fault_addr, b.fault_addr) << "fault address";
    }
    if (exact) {
        EXPECT_EQ(a.cpu.flags.af(), b.cpu.flags.af()) << "AF";
        EXPECT_EQ(a.code, b.code) << "exception code";
        EXPECT_EQ(a.info0, b.info0) << "ExceptionInformation[0]";
        EXPECT_EQ(a.info1, b.info1) << "ExceptionInformation[1]";
    }
    ASSERT_EQ(a.data.size(), b.data.size());
    for (usize i = 0; i < a.data.size(); ++i) {
        if (a.data[i] != b.data[i]) {
            ADD_FAILURE() << "data diverged at 0x" << std::hex << (kDataBase + i)
                          << ": " << int(a.data[i]) << " vs " << int(b.data[i]);
            break;
        }
    }
}

struct Triple {
    LoopOutcome interp, off, on;
};

/// Interpreter, JIT with the kill switch, JIT forced on (thresholds
/// bypassed so small counts reach the bulk path); all three must agree.
inline Triple diff3(std::span<const u8> code, bool is64, const MemSetup& mem_setup,
                    const CpuSetup& cpu_setup, Force on = Force::OnNoThreshold,
                    bool tier1 = false) {
    Triple t;
    t.interp = run_interpreter(code, is64, mem_setup, cpu_setup);
    t.off = run_jit(code, is64, mem_setup, cpu_setup, Force::Off, tier1);
    t.on = run_jit(code, is64, mem_setup, cpu_setup, on, tier1);
    expect_same(t.interp, t.off, "interpreter vs JIT(off)", false);
    expect_same(t.off, t.on, "JIT(off) vs JIT(on)", true);
    EXPECT_FALSE(t.off.bulked_first_entry) << "kill switch still bulked";
    return t;
}

// ---------------------------------------------------------------------------
// In-tree loop catalogue. Assembly is written here; the encodings were
// produced from it with `llvm-mc -filetype=obj` (Intel syntax). "top" is the
// block start, "out" the first byte after the loop. No Microsoft-derived
// bytes: these are hand-written loops in the shapes static CRTs and
// hand-written code use.
// ---------------------------------------------------------------------------
namespace catalogue {
// F1 (32-bit): mov byte ptr [edi],al; inc edi; dec ecx; jnz top
constexpr u8 kF1[] = {0x88, 0x07, 0x47, 0x49, 0x75, 0xFA};
// F2 (32-bit): mov dword ptr [edi+eax*4],edx; inc eax; cmp eax,ecx; jb top
constexpr u8 kF2[] = {0x89, 0x14, 0x87, 0x40, 0x39, 0xC8, 0x72, 0xF8};
// F3 (32-bit): mov word ptr [edi+ecx*2],0x2020; inc ecx; jnz top
constexpr u8 kF3[] = {0x66, 0xC7, 0x04, 0x4F, 0x20, 0x20, 0x41, 0x75, 0xF7};
// F4 (64-bit): mov byte ptr [rdi+rax],cl; inc rax; cmp rdx,rax; jne top
constexpr u8 kF4[] = {0x88, 0x0C, 0x07, 0x48, 0xFF, 0xC0, 0x48, 0x39, 0xC2, 0x75, 0xF5};
// F5 (64-bit): mov qword ptr [rdi+rax*8],rsi; add rax,1; cmp rax,rdx; jb top
constexpr u8 kF5[] = {0x48, 0x89, 0x34, 0xC7, 0x48, 0x83, 0xC0, 0x01, 0x48, 0x39, 0xD0, 0x72, 0xF3};
// F6 (64-bit): movdqu xmmword ptr [rdi+rax],xmm0; add rax,16; cmp rax,rdx; jb top
constexpr u8 kF6[] = {0xF3, 0x0F, 0x7F, 0x04, 0x07, 0x48, 0x83, 0xC0, 0x10, 0x48, 0x39, 0xD0, 0x72, 0xF2};
// C1 (32-bit): mov al,byte ptr [esi]; mov byte ptr [edi],al; inc esi; inc edi; dec ecx; jnz top
constexpr u8 kC1[] = {0x8A, 0x06, 0x88, 0x07, 0x46, 0x47, 0x49, 0x75, 0xF7};
// C2 (32-bit): mov dl,byte ptr [esi]; lea esi,[esi+1]; mov byte ptr [edi],dl; lea edi,[edi+1]; sub ecx,1; jne top
constexpr u8 kC2[] = {0x8A, 0x16, 0x8D, 0x76, 0x01, 0x88, 0x17, 0x8D, 0x7F, 0x01, 0x83, 0xE9, 0x01, 0x75, 0xF1};
// C3 (32-bit): mov edx,dword ptr [esi+eax*4]; mov dword ptr [edi+eax*4],edx; inc eax; cmp eax,ecx; jne top
constexpr u8 kC3[] = {0x8B, 0x14, 0x86, 0x89, 0x14, 0x87, 0x40, 0x39, 0xC8, 0x75, 0xF5};
// C4 (32-bit): dec esi; dec edi; mov al,byte ptr [esi]; mov byte ptr [edi],al; dec ecx; jnz top
constexpr u8 kC4[] = {0x4E, 0x4F, 0x8A, 0x06, 0x88, 0x07, 0x49, 0x75, 0xF7};
// C5 (32-bit): mov al,byte ptr [esi]; mov byte ptr [edi],al; inc esi; inc edi; test al,al; jnz top
constexpr u8 kC5[] = {0x8A, 0x06, 0x88, 0x07, 0x46, 0x47, 0x84, 0xC0, 0x75, 0xF6};
// C6 (32-bit): mov al,byte ptr [esi]; mov byte ptr [edi],al; inc esi; inc edi; loop top
constexpr u8 kC6[] = {0x8A, 0x06, 0x88, 0x07, 0x46, 0x47, 0xE2, 0xF8};
// C7 (64-bit): movzx eax,byte ptr [rsi+rcx]; mov byte ptr [rdi+rcx],al; inc rcx; jnz top
constexpr u8 kC7[] = {0x0F, 0xB6, 0x04, 0x0E, 0x88, 0x04, 0x0F, 0x48, 0xFF, 0xC1, 0x75, 0xF4};
// C8 (64-bit): mov rax,qword ptr [rsi+rcx*8]; mov qword ptr [rdi+rcx*8],rax; inc rcx; cmp rcx,rdx; jne top
constexpr u8 kC8[] = {0x48, 0x8B, 0x04, 0xCE, 0x48, 0x89, 0x04, 0xCF, 0x48, 0xFF, 0xC1, 0x48, 0x39, 0xD1, 0x75, 0xF0};
// C9 (64-bit): movups xmm0,xmmword ptr [rsi+rax]; movups xmmword ptr [rdi+rax],xmm0; add rax,16; cmp rax,rdx; jb top
constexpr u8 kC9[] = {0x0F, 0x10, 0x04, 0x06, 0x0F, 0x11, 0x04, 0x07, 0x48, 0x83, 0xC0, 0x10, 0x48, 0x39, 0xD0, 0x72, 0xEF};
// C10 (64-bit): movdqa xmm0,xmmword ptr [rsi+rax]; movdqa xmmword ptr [rdi+rax],xmm0; add rax,16; cmp rax,rdx; jb top
constexpr u8 kC10[] = {0x66, 0x0F, 0x6F, 0x04, 0x06, 0x66, 0x0F, 0x7F, 0x04, 0x07, 0x48, 0x83, 0xC0, 0x10, 0x48, 0x39, 0xD0, 0x72, 0xED};
// S1 (32-bit): cmp byte ptr [eax],0; je out; inc eax; jmp top
constexpr u8 kS1[] = {0x80, 0x38, 0x00, 0x74, 0x03, 0x40, 0xEB, 0xF8};
// S2 (32-bit): mov cl,byte ptr [eax]; inc eax; test cl,cl; jnz top
constexpr u8 kS2[] = {0x8A, 0x08, 0x40, 0x84, 0xC9, 0x75, 0xF9};
// S3 (32-bit): cmp byte ptr [edi],al; je out; inc edi; dec ecx; jnz top
constexpr u8 kS3[] = {0x38, 0x07, 0x74, 0x04, 0x47, 0x49, 0x75, 0xF8};
// S4 (64-bit): inc rax; cmp byte ptr [rcx+rax],0; jne top
constexpr u8 kS4[] = {0x48, 0xFF, 0xC0, 0x80, 0x3C, 0x01, 0x00, 0x75, 0xF7};
// S5 (64-bit): inc rcx; cmp word ptr [rax+rcx*2],0; jne top
constexpr u8 kS5[] = {0x48, 0xFF, 0xC1, 0x66, 0x83, 0x3C, 0x48, 0x00, 0x75, 0xF6};
// S6 (64-bit): movdqu xmm1,xmmword ptr [rax]; pcmpeqb xmm1,xmm0; pmovmskb edx,xmm1; add rax,16; test edx,edx; jz top
constexpr u8 kS6[] = {0xF3, 0x0F, 0x6F, 0x08, 0x66, 0x0F, 0x74, 0xC8, 0x66, 0x0F, 0xD7, 0xD1, 0x48, 0x83, 0xC0, 0x10, 0x85, 0xD2, 0x74, 0xEC};
// S7 (64-bit): cmp byte ptr [rdi],sil; je out; inc rdi; dec rdx; jnz top
constexpr u8 kS7[] = {0x40, 0x38, 0x37, 0x74, 0x08, 0x48, 0xFF, 0xC7, 0x48, 0xFF, 0xCA, 0x75, 0xF3};
// M1 (32-bit): mov al,byte ptr [esi+ecx]; cmp al,byte ptr [edi+ecx]; jne out; inc ecx; cmp ecx,edx; jb top
constexpr u8 kM1[] = {0x8A, 0x04, 0x0E, 0x3A, 0x04, 0x0F, 0x75, 0x05, 0x41, 0x39, 0xD1, 0x72, 0xF3};
// M2 (32-bit): mov al,byte ptr [esi]; cmp al,byte ptr [edi]; jne out; inc esi; inc edi; test al,al; jnz top
constexpr u8 kM2[] = {0x8A, 0x06, 0x3A, 0x07, 0x75, 0x06, 0x46, 0x47, 0x84, 0xC0, 0x75, 0xF4};
// M3 (64-bit): movzx eax,byte ptr [rdi+rcx]; cmp al,byte ptr [rsi+rcx]; jne out; inc rcx; cmp rcx,rdx; jne top
constexpr u8 kM3[] = {0x0F, 0xB6, 0x04, 0x0F, 0x3A, 0x04, 0x0E, 0x75, 0x08, 0x48, 0xFF, 0xC1, 0x48, 0x39, 0xD1, 0x75, 0xEF};
// M4 (64-bit): mov rax,qword ptr [rdi+rcx*8]; cmp rax,qword ptr [rsi+rcx*8]; jne out; inc rcx; cmp rcx,rdx; jb top
constexpr u8 kM4[] = {0x48, 0x8B, 0x04, 0xCF, 0x48, 0x3B, 0x04, 0xCE, 0x75, 0x08, 0x48, 0xFF, 0xC1, 0x48, 0x39, 0xD1, 0x72, 0xEE};
// NmXor (32-bit): xor edx,edx; mov al,byte ptr [esi]; mov byte ptr [edi],al; inc esi; inc edi; dec ecx; jnz top
constexpr u8 kNmXor[] = {0x31, 0xD2, 0x8A, 0x06, 0x88, 0x07, 0x46, 0x47, 0x49, 0x75, 0xF5};
// NmStride (32-bit): mov al,byte ptr [esi]; mov byte ptr [edi],al; inc esi; add edi,2; dec ecx; jnz top
constexpr u8 kNmStride[] = {0x8A, 0x06, 0x88, 0x07, 0x46, 0x83, 0xC7, 0x02, 0x49, 0x75, 0xF5};
// NmCarried (32-bit): mov byte ptr [edi],al; mov al,byte ptr [esi]; inc esi; inc edi; dec ecx; jnz top
constexpr u8 kNmCarried[] = {0x88, 0x07, 0x8A, 0x06, 0x46, 0x47, 0x49, 0x75, 0xF7};
// NmAdc (32-bit): mov al,byte ptr [esi]; mov byte ptr [edi],al; adc eax,0; inc esi; inc edi; dec ecx; jnz top
constexpr u8 kNmAdc[] = {0x8A, 0x06, 0x88, 0x07, 0x83, 0xD0, 0x00, 0x46, 0x47, 0x49, 0x75, 0xF4};
// NmAddr16 (32-bit): mov al,byte ptr [si]; mov byte ptr [edi],al; inc esi; inc edi; dec ecx; jnz top
constexpr u8 kNmAddr16[] = {0x67, 0x8A, 0x04, 0x88, 0x07, 0x46, 0x47, 0x49, 0x75, 0xF6};
// NmAddr32 (64-bit): mov al,byte ptr [esi]; mov byte ptr [rdi],al; inc rsi; inc rdi; dec rcx; jnz top
constexpr u8 kNmAddr32[] = {0x67, 0x8A, 0x06, 0x88, 0x07, 0x48, 0xFF, 0xC6, 0x48, 0xFF, 0xC7, 0x48, 0xFF, 0xC9, 0x75, 0xF0};
// NmFs (32-bit): mov al,byte ptr fs:[esi]; mov byte ptr [edi],al; inc esi; inc edi; dec ecx; jnz top
constexpr u8 kNmFs[] = {0x64, 0x8A, 0x06, 0x88, 0x07, 0x46, 0x47, 0x49, 0x75, 0xF6};
// NmRip (64-bit): mov byte ptr [rip+0x10],al; inc rax; dec rcx; jnz top
constexpr u8 kNmRip[] = {0x88, 0x05, 0x10, 0x00, 0x00, 0x00, 0x48, 0xFF, 0xC0, 0x48, 0xFF, 0xC9, 0x75, 0xF2};
// NmEsp (32-bit): mov byte ptr [esp+ecx],al; inc ecx; jnz top
constexpr u8 kNmEsp[] = {0x88, 0x04, 0x0C, 0x41, 0x75, 0xFA};
// NmIncJb (32-bit): mov byte ptr [edi],al; inc edi; inc ecx; jb top
constexpr u8 kNmIncJb[] = {0x88, 0x07, 0x47, 0x41, 0x72, 0xFA};
// NmLock (32-bit): lock add dword ptr [edi],1; inc edi; dec ecx; jnz top
constexpr u8 kNmLock[] = {0xF0, 0x83, 0x07, 0x01, 0x47, 0x49, 0x75, 0xF8};
// NmIncCl (32-bit): mov byte ptr [edi],al; inc edi; inc cl; jnz top
constexpr u8 kNmIncCl[] = {0x88, 0x07, 0x47, 0xFE, 0xC1, 0x75, 0xF9};
// NmWidth (32-bit): movzx eax,byte ptr [esi]; mov word ptr [edi],ax; inc esi; inc edi; dec ecx; jnz top
constexpr u8 kNmWidth[] = {0x0F, 0xB6, 0x06, 0x66, 0x89, 0x07, 0x46, 0x47, 0x49, 0x75, 0xF5};
// NmMovRR (32-bit): mov al,dl; mov byte ptr [edi],al; inc edi; dec ecx; jnz top
constexpr u8 kNmMovRR[] = {0x88, 0xD0, 0x88, 0x07, 0x47, 0x49, 0x75, 0xF8};
// NmTwoStores (32-bit): mov byte ptr [edi],al; mov byte ptr [esi],al; inc edi; inc esi; dec ecx; jnz top
constexpr u8 kNmTwoStores[] = {0x88, 0x07, 0x88, 0x06, 0x47, 0x46, 0x49, 0x75, 0xF7};
// NmFillData (32-bit): mov byte ptr [edi],al; cmp byte ptr [esi],0; je out; inc edi; inc esi; dec ecx; jnz top
constexpr u8 kNmFillData[] = {0x88, 0x07, 0x80, 0x3E, 0x00, 0x74, 0x05, 0x47, 0x46, 0x49, 0x75, 0xF4};
// NmSrcStride (32-bit): mov al,byte ptr [esi]; mov byte ptr [edi],al; add esi,2; inc edi; dec ecx; jnz top
constexpr u8 kNmSrcStride[] = {0x8A, 0x06, 0x88, 0x07, 0x83, 0xC6, 0x02, 0x47, 0x49, 0x75, 0xF5};
// NmJo (32-bit): mov byte ptr [edi],al; inc edi; dec ecx; jo top
constexpr u8 kNmJo[] = {0x88, 0x07, 0x47, 0x49, 0x70, 0xFA};
// NmDecJg (32-bit): mov byte ptr [edi],al; inc edi; dec ecx; jg top
constexpr u8 kNmDecJg[] = {0x88, 0x07, 0x47, 0x49, 0x7F, 0xFA};
// NmAddMem (32-bit): add eax,dword ptr [esi]; add esi,4; dec ecx; jnz top
constexpr u8 kNmAddMem[] = {0x03, 0x06, 0x83, 0xC6, 0x04, 0x49, 0x75, 0xF8};
// NmIota (32-bit): mov dword ptr [edi+eax*4],eax; inc eax; cmp eax,ecx; jb top
constexpr u8 kNmIota[] = {0x89, 0x04, 0x87, 0x40, 0x39, 0xC8, 0x72, 0xF8};
// NmNoExit (32-bit): mov byte ptr [edi],al; inc edi; jmp top
constexpr u8 kNmNoExit[] = {0x88, 0x07, 0x47, 0xEB, 0xFB};
// NmRep (32-bit): rep movsb; dec ecx; jnz top
constexpr u8 kNmRep[] = {0xF3, 0xA4, 0x49, 0x75, 0xFB};
// NmQStride (64-bit): mov qword ptr [rdi+rax*4],rsi; inc rax; cmp rax,rdx; jb top
constexpr u8 kNmQStride[] = {0x48, 0x89, 0x34, 0x87, 0x48, 0xFF, 0xC0, 0x48, 0x39, 0xD0, 0x72, 0xF4};
// NmQWidth (64-bit): mov rax,qword ptr [rsi+rcx*8]; mov dword ptr [rdi+rcx*8],eax; inc rcx; cmp rcx,rdx; jne top
constexpr u8 kNmQWidth[] = {0x48, 0x8B, 0x04, 0xCE, 0x89, 0x04, 0xCF, 0x48, 0xFF, 0xC1, 0x48, 0x39, 0xD1, 0x75, 0xF1};
// NmQCmpStride (64-bit): mov rax,qword ptr [rdi+rcx*8]; cmp rax,qword ptr [rsi+rcx*4]; jne out; inc rcx; cmp rcx,rdx; jb top
constexpr u8 kNmQCmpStride[] = {0x48, 0x8B, 0x04, 0xCF, 0x48, 0x3B, 0x04, 0x8E, 0x75, 0x08, 0x48, 0xFF, 0xC1, 0x48, 0x39, 0xD1, 0x72, 0xEE};
// S3h (32-bit): cmp byte ptr [edi],ah; je out; inc edi; dec ecx; jnz top
constexpr u8 kS3h[] = {0x38, 0x27, 0x74, 0x04, 0x47, 0x49, 0x75, 0xF8};
// S6w (64-bit): movdqu xmm1,xmmword ptr [rax]; pcmpeqw xmm1,xmm0; pmovmskb edx,xmm1; add rax,16; test edx,edx; jz top
constexpr u8 kS6w[] = {0xF3, 0x0F, 0x6F, 0x08, 0x66, 0x0F, 0x75, 0xC8, 0x66, 0x0F, 0xD7, 0xD1, 0x48, 0x83, 0xC0, 0x10, 0x85, 0xD2, 0x74, 0xEC};
// S6d (64-bit): movdqu xmm1,xmmword ptr [rax]; pcmpeqd xmm1,xmm0; pmovmskb edx,xmm1; add rax,16; test edx,edx; jz top
constexpr u8 kS6d[] = {0xF3, 0x0F, 0x6F, 0x08, 0x66, 0x0F, 0x76, 0xC8, 0x66, 0x0F, 0xD7, 0xD1, 0x48, 0x83, 0xC0, 0x10, 0x85, 0xD2, 0x74, 0xEC};
// F7 (64-bit): movntdq xmmword ptr [rdi+rax],xmm0; add rax,16; cmp rax,rdx; jb top
constexpr u8 kF7[] = {0x66, 0x0F, 0xE7, 0x04, 0x07, 0x48, 0x83, 0xC0, 0x10, 0x48, 0x39, 0xD0, 0x72, 0xF2};
// F8 (32-bit): mov byte ptr [edi],ah; inc edi; dec ecx; jnz top
constexpr u8 kF8[] = {0x88, 0x27, 0x47, 0x49, 0x75, 0xFA};
// F9 (64-bit): mov dword ptr [rdi],0x41424344; add rdi,4; dec edx; jnz top
constexpr u8 kF9[] = {0xC7, 0x07, 0x44, 0x43, 0x42, 0x41, 0x48, 0x83, 0xC7, 0x04, 0xFF, 0xCA, 0x75, 0xF2};
// C11 (64-bit): mov eax,dword ptr [rsi]; mov dword ptr [rdi],eax; add rsi,4; add rdi,4; sub rdx,1; jnz top
constexpr u8 kC11[] = {0x8B, 0x06, 0x89, 0x07, 0x48, 0x83, 0xC6, 0x04, 0x48, 0x83, 0xC7, 0x04, 0x48, 0x83, 0xEA, 0x01, 0x75, 0xEE};
// C12 (64-bit): movzx eax,word ptr [rsi+rcx*2]; mov word ptr [rdi+rcx*2],ax; inc rcx; test ax,ax; jnz top
constexpr u8 kC12[] = {0x0F, 0xB7, 0x04, 0x4E, 0x66, 0x89, 0x04, 0x4F, 0x48, 0xFF, 0xC1, 0x66, 0x85, 0xC0, 0x75, 0xF0};
// M5 (64-bit): movzx eax,word ptr [rdi+rcx*2]; cmp ax,word ptr [rsi+rcx*2]; jne out; inc rcx; test ax,ax; jnz top
constexpr u8 kM5[] = {0x0F, 0xB7, 0x04, 0x4F, 0x66, 0x3B, 0x04, 0x4E, 0x75, 0x08, 0x48, 0xFF, 0xC1, 0x66, 0x85, 0xC0, 0x75, 0xEE};
// L1 (32-bit): mov byte ptr [edi],al; inc edi; loop top
constexpr u8 kL1[] = {0x88, 0x07, 0x47, 0xE2, 0xFB};
// G1 (32-bit): mov byte ptr [edi+ecx],al; inc ecx; cmp ecx,0x40; jl top
constexpr u8 kG1[] = {0x88, 0x04, 0x0F, 0x41, 0x83, 0xF9, 0x40, 0x7C, 0xF7};
}  // namespace catalogue

}  // namespace vitreo::cpu::jit::loop_idiom::test
