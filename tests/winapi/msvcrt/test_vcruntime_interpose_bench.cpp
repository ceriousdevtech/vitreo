// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_vcruntime_interpose_bench.cpp
/// @brief Size-sweep microbenchmark of the vcruntime140 interposer: guest
///        AVX2 reference loops (written here, standing in for vcruntime's own
///        vector paths) against the host body behind the stub, through the
///        JIT run loop, x86 and x64. Gives each function's crossover by the
///        program rule and checks the WS1 gate.
///
/// The timing tests run only with VITREO_WS1_BENCH=1 (otherwise skipped), on
/// an otherwise idle machine. `VcrtBenchFixtures` always runs: it proves the
/// guest loops compute the C contract, so a timing is never of wrong code.

#include "vcruntime_interpose_test_support.hpp"

#include "app/cli/commands/dll_registry.hpp"
#include "capsule/vfs/vfs.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "runtime/execution/guest_run_loop.hpp"
#include "winapi/core/host_in_chain.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/msvcrt/guest_accel.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/text/crt_string.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vcrt_test;
using vitreo::cpu::jit::JitCompiler;
namespace vcrt = vitreo::winapi::vcrt;

namespace {

constexpr GuestAddr kThunkBase = 0x70000000;
constexpr usize kThunkSlab = 0x20000;
constexpr GuestAddr kLow = 0x60000;       // driver code, cells, stack
constexpr usize kLowSize = 0x60000;
constexpr GuestAddr kDriver = 0x62000;
constexpr GuestAddr kCells = 0x63000;     // +0 target, +8 a0, +16 a1, +24 a2, +32 K
constexpr GuestAddr kStack32 = 0xBF000;
constexpr GuestAddr kStack64 = 0xBF008;
constexpr GuestAddr kData = 0x1000000;    // 4 MiB of buffers
constexpr usize kDataSize = 0x400000;
constexpr GuestAddr kA = kData;           // source / first operand
constexpr GuestAddr kB = kData + 0x200000;  // destination / second operand
constexpr GuestAddr kImg32 = 0x10000000;
constexpr GuestAddr kImg64 = kThunkBase + X64_ACCEL_OFFSET + 0x180000000ull;  // far

// ---------------------------------------------------------------------------
// A tiny assembler: raw bytes plus rel8 labels.
// ---------------------------------------------------------------------------
struct Asm {
    std::vector<u8> b;
    struct Label {
        long at{-1};
        std::vector<usize> uses;
    };
    void raw(std::initializer_list<u8> x) { b.insert(b.end(), x); }
    void j8(u8 op, Label& l) {
        b.push_back(op);
        b.push_back(0);
        if (l.at >= 0) {
            b.back() = static_cast<u8>(l.at - static_cast<long>(b.size()));
        } else {
            l.uses.push_back(b.size() - 1);
        }
    }
    void bind(Label& l) {
        l.at = static_cast<long>(b.size());
        for (usize u : l.uses) b[u] = static_cast<u8>(l.at - static_cast<long>(u + 1));
        l.uses.clear();
    }
};
constexpr u8 JB = 0x72, JAE = 0x73, JZ = 0x74, JNZ = 0x75, JMP = 0xEB;

const std::initializer_list<u8> kVzeroupper = {0xC5, 0xF8, 0x77};

/// copy(dst, src, n) -> dst: 32-byte VMOVDQU loop + byte tail; an overlapping
/// dst above src copies backward (STD; REP MOVSB).
std::vector<u8> avx2_copy(bool x64) {
    Asm a;
    Asm::Label back, tail, loop, done, t;
    if (x64) {
        a.raw({0x48, 0x89, 0xC8});              // mov rax,rcx
        a.raw({0x49, 0x89, 0xC9});              // mov r9,rcx
        a.raw({0x49, 0x29, 0xD1});              // sub r9,rdx
        a.raw({0x4D, 0x39, 0xC1});              // cmp r9,r8
        a.j8(JB, back);
        a.raw({0x49, 0x83, 0xF8, 0x20});        // cmp r8,32
        a.j8(JB, tail);
        a.bind(loop);
        a.raw({0xC5, 0xFE, 0x6F, 0x02});        // vmovdqu ymm0,[rdx]
        a.raw({0xC5, 0xFE, 0x7F, 0x01});        // vmovdqu [rcx],ymm0
        a.raw({0x48, 0x83, 0xC2, 0x20});        // add rdx,32
        a.raw({0x48, 0x83, 0xC1, 0x20});        // add rcx,32
        a.raw({0x49, 0x83, 0xE8, 0x20});        // sub r8,32
        a.raw({0x49, 0x83, 0xF8, 0x20});        // cmp r8,32
        a.j8(JAE, loop);
        a.bind(tail);
        a.raw({0x4D, 0x85, 0xC0});              // test r8,r8
        a.j8(JZ, done);
        a.bind(t);
        a.raw({0x44, 0x8A, 0x0A});              // mov r9b,[rdx]
        a.raw({0x44, 0x88, 0x09});              // mov [rcx],r9b
        a.raw({0x48, 0xFF, 0xC2});              // inc rdx
        a.raw({0x48, 0xFF, 0xC1});              // inc rcx
        a.raw({0x49, 0xFF, 0xC8});              // dec r8
        a.j8(JNZ, t);
        a.bind(done);
        a.raw(kVzeroupper);
        a.raw({0xC3});
        a.bind(back);
        a.raw({0x56, 0x57});                    // push rsi; push rdi
        a.raw({0x4A, 0x8D, 0x74, 0x02, 0xFF});  // lea rsi,[rdx+r8-1]
        a.raw({0x4A, 0x8D, 0x7C, 0x01, 0xFF});  // lea rdi,[rcx+r8-1]
        a.raw({0x4C, 0x89, 0xC1});              // mov rcx,r8
        a.raw({0xFD, 0xF3, 0xA4, 0xFC});        // std; rep movsb; cld
        a.raw({0x5F, 0x5E, 0xC3});              // pop rdi; pop rsi; ret
        return a.b;
    }
    a.raw({0x56, 0x57});                        // push esi; push edi
    a.raw({0x8B, 0x7C, 0x24, 0x0C});            // mov edi,[esp+0Ch]
    a.raw({0x8B, 0x74, 0x24, 0x10});            // mov esi,[esp+10h]
    a.raw({0x8B, 0x4C, 0x24, 0x14});            // mov ecx,[esp+14h]
    a.raw({0x89, 0xF8});                        // mov eax,edi
    a.raw({0x89, 0xFA});                        // mov edx,edi
    a.raw({0x29, 0xF2});                        // sub edx,esi
    a.raw({0x39, 0xCA});                        // cmp edx,ecx
    a.j8(JB, back);
    a.raw({0x83, 0xF9, 0x20});                  // cmp ecx,32
    a.j8(JB, tail);
    a.bind(loop);
    a.raw({0xC5, 0xFE, 0x6F, 0x06});            // vmovdqu ymm0,[esi]
    a.raw({0xC5, 0xFE, 0x7F, 0x07});            // vmovdqu [edi],ymm0
    a.raw({0x83, 0xC6, 0x20});                  // add esi,32
    a.raw({0x83, 0xC7, 0x20});                  // add edi,32
    a.raw({0x83, 0xE9, 0x20});                  // sub ecx,32
    a.raw({0x83, 0xF9, 0x20});                  // cmp ecx,32
    a.j8(JAE, loop);
    a.bind(tail);
    a.raw({0xF3, 0xA4});                        // rep movsb
    a.raw(kVzeroupper);
    a.raw({0x5F, 0x5E, 0xC3});                  // pop edi; pop esi; ret
    a.bind(back);
    a.raw({0x8D, 0x74, 0x0E, 0xFF});            // lea esi,[esi+ecx-1]
    a.raw({0x8D, 0x7C, 0x0F, 0xFF});            // lea edi,[edi+ecx-1]
    a.raw({0xFD, 0xF3, 0xA4, 0xFC});            // std; rep movsb; cld
    a.raw({0x5F, 0x5E, 0xC3});
    return a.b;
}

/// fill(dst, c, n) -> dst: VPBROADCASTB + 32-byte stores + tail.
std::vector<u8> avx2_fill(bool x64) {
    Asm a;
    Asm::Label tail, loop, done, t;
    if (x64) {
        a.raw({0x48, 0x89, 0xC8});              // mov rax,rcx
        a.raw({0xC5, 0xF9, 0x6E, 0xC2});        // vmovd xmm0,edx
        a.raw({0xC4, 0xE2, 0x7D, 0x78, 0xC0});  // vpbroadcastb ymm0,xmm0
        a.raw({0x49, 0x83, 0xF8, 0x20});        // cmp r8,32
        a.j8(JB, tail);
        a.bind(loop);
        a.raw({0xC5, 0xFE, 0x7F, 0x01});        // vmovdqu [rcx],ymm0
        a.raw({0x48, 0x83, 0xC1, 0x20});        // add rcx,32
        a.raw({0x49, 0x83, 0xE8, 0x20});        // sub r8,32
        a.raw({0x49, 0x83, 0xF8, 0x20});        // cmp r8,32
        a.j8(JAE, loop);
        a.bind(tail);
        a.raw({0x4D, 0x85, 0xC0});              // test r8,r8
        a.j8(JZ, done);
        a.bind(t);
        a.raw({0x88, 0x11});                    // mov [rcx],dl
        a.raw({0x48, 0xFF, 0xC1});              // inc rcx
        a.raw({0x49, 0xFF, 0xC8});              // dec r8
        a.j8(JNZ, t);
        a.bind(done);
        a.raw(kVzeroupper);
        a.raw({0xC3});
        return a.b;
    }
    a.raw({0x57});                              // push edi
    a.raw({0x8B, 0x7C, 0x24, 0x08});            // mov edi,[esp+8]
    a.raw({0x8B, 0x54, 0x24, 0x0C});            // mov edx,[esp+0Ch]
    a.raw({0x8B, 0x4C, 0x24, 0x10});            // mov ecx,[esp+10h]
    a.raw({0xC5, 0xF9, 0x6E, 0xC2});            // vmovd xmm0,edx
    a.raw({0xC4, 0xE2, 0x7D, 0x78, 0xC0});      // vpbroadcastb ymm0,xmm0
    a.raw({0x83, 0xF9, 0x20});                  // cmp ecx,32
    a.j8(JB, tail);
    a.bind(loop);
    a.raw({0xC5, 0xFE, 0x7F, 0x07});            // vmovdqu [edi],ymm0
    a.raw({0x83, 0xC7, 0x20});                  // add edi,32
    a.raw({0x83, 0xE9, 0x20});                  // sub ecx,32
    a.raw({0x83, 0xF9, 0x20});                  // cmp ecx,32
    a.j8(JAE, loop);
    a.bind(tail);
    a.raw({0x89, 0xD0});                        // mov eax,edx
    a.raw({0xF3, 0xAA});                        // rep stosb
    a.raw(kVzeroupper);
    a.raw({0x8B, 0x44, 0x24, 0x08});            // mov eax,[esp+8]
    a.raw({0x5F, 0xC3});                        // pop edi; ret
    return a.b;
}

/// compare(a, b, n) -> -1/0/+1: VPCMPEQB + VPMOVMSKB, BSF to the first
/// difference, byte tail.
std::vector<u8> avx2_compare(bool x64) {
    Asm a;
    Asm::Label tail, loop, diff, bytediff, equal, t, out;
    if (x64) {
        a.raw({0x49, 0x83, 0xF8, 0x20});        // cmp r8,32
        a.j8(JB, tail);
        a.bind(loop);
        a.raw({0xC5, 0xFE, 0x6F, 0x01});        // vmovdqu ymm0,[rcx]
        a.raw({0xC5, 0xFD, 0x74, 0x02});        // vpcmpeqb ymm0,ymm0,[rdx]
        a.raw({0xC5, 0xFD, 0xD7, 0xC0});        // vpmovmskb eax,ymm0
        a.raw({0x83, 0xF8, 0xFF});              // cmp eax,-1
        a.j8(JNZ, diff);
        a.raw({0x48, 0x83, 0xC1, 0x20});        // add rcx,32
        a.raw({0x48, 0x83, 0xC2, 0x20});        // add rdx,32
        a.raw({0x49, 0x83, 0xE8, 0x20});        // sub r8,32
        a.raw({0x49, 0x83, 0xF8, 0x20});        // cmp r8,32
        a.j8(JAE, loop);
        a.bind(tail);
        a.raw({0x4D, 0x85, 0xC0});              // test r8,r8
        a.j8(JZ, equal);
        a.bind(t);
        a.raw({0x44, 0x8A, 0x09});              // mov r9b,[rcx]
        a.raw({0x44, 0x3A, 0x0A});              // cmp r9b,[rdx]
        a.j8(JNZ, bytediff);
        a.raw({0x48, 0xFF, 0xC1});              // inc rcx
        a.raw({0x48, 0xFF, 0xC2});              // inc rdx
        a.raw({0x49, 0xFF, 0xC8});              // dec r8
        a.j8(JNZ, t);
        a.bind(equal);
        a.raw({0x31, 0xC0});                    // xor eax,eax
        a.raw(kVzeroupper);
        a.raw({0xC3});
        a.bind(diff);
        a.raw({0xF7, 0xD0});                    // not eax
        a.raw({0x0F, 0xBC, 0xC0});              // bsf eax,eax
        a.raw({0x44, 0x8A, 0x0C, 0x01});        // mov r9b,[rcx+rax]
        a.raw({0x44, 0x3A, 0x0C, 0x02});        // cmp r9b,[rdx+rax]
        a.bind(bytediff);
        a.raw({0x19, 0xC0});                    // sbb eax,eax
        a.raw({0x83, 0xC8, 0x01});              // or eax,1
        a.raw(kVzeroupper);
        a.raw({0xC3});
        return a.b;
    }
    a.raw({0x56, 0x57});                        // push esi; push edi
    a.raw({0x8B, 0x74, 0x24, 0x0C});            // mov esi,[esp+0Ch]
    a.raw({0x8B, 0x7C, 0x24, 0x10});            // mov edi,[esp+10h]
    a.raw({0x8B, 0x4C, 0x24, 0x14});            // mov ecx,[esp+14h]
    a.raw({0x83, 0xF9, 0x20});                  // cmp ecx,32
    a.j8(JB, tail);
    a.bind(loop);
    a.raw({0xC5, 0xFE, 0x6F, 0x06});            // vmovdqu ymm0,[esi]
    a.raw({0xC5, 0xFD, 0x74, 0x07});            // vpcmpeqb ymm0,ymm0,[edi]
    a.raw({0xC5, 0xFD, 0xD7, 0xC0});            // vpmovmskb eax,ymm0
    a.raw({0x83, 0xF8, 0xFF});                  // cmp eax,-1
    a.j8(JNZ, diff);
    a.raw({0x83, 0xC6, 0x20});                  // add esi,32
    a.raw({0x83, 0xC7, 0x20});                  // add edi,32
    a.raw({0x83, 0xE9, 0x20});                  // sub ecx,32
    a.raw({0x83, 0xF9, 0x20});                  // cmp ecx,32
    a.j8(JAE, loop);
    a.bind(tail);
    a.raw({0x31, 0xC0});                        // xor eax,eax
    a.raw({0x85, 0xC9});                        // test ecx,ecx
    a.j8(JZ, out);
    a.raw({0xF3, 0xA6});                        // repe cmpsb
    a.j8(JZ, out);
    a.j8(JMP, bytediff);
    a.bind(diff);
    a.raw({0xF7, 0xD0});                        // not eax
    a.raw({0x0F, 0xBC, 0xC0});                  // bsf eax,eax
    a.raw({0x8A, 0x14, 0x06});                  // mov dl,[esi+eax]
    a.raw({0x3A, 0x14, 0x07});                  // cmp dl,[edi+eax]
    a.bind(bytediff);
    a.raw({0x19, 0xC0});                        // sbb eax,eax
    a.raw({0x83, 0xC8, 0x01});                  // or eax,1
    a.bind(out);
    a.raw(kVzeroupper);
    a.raw({0x5F, 0x5E, 0xC3});                  // pop edi; pop esi; ret
    return a.b;
}

/// scan(p, c, n) -> first byte == (u8)c or NULL: VPCMPEQB against a
/// broadcast + VPMOVMSKB + BSF, byte tail.
std::vector<u8> avx2_scan(bool x64) {
    Asm a;
    Asm::Label tail, loop, found, bytefound, nf, t, out;
    if (x64) {
        a.raw({0xC5, 0xF9, 0x6E, 0xCA});        // vmovd xmm1,edx
        a.raw({0xC4, 0xE2, 0x7D, 0x78, 0xC9});  // vpbroadcastb ymm1,xmm1
        a.raw({0x49, 0x83, 0xF8, 0x20});        // cmp r8,32
        a.j8(JB, tail);
        a.bind(loop);
        a.raw({0xC5, 0xF5, 0x74, 0x01});        // vpcmpeqb ymm0,ymm1,[rcx]
        a.raw({0xC5, 0xFD, 0xD7, 0xC0});        // vpmovmskb eax,ymm0
        a.raw({0x85, 0xC0});                    // test eax,eax
        a.j8(JNZ, found);
        a.raw({0x48, 0x83, 0xC1, 0x20});        // add rcx,32
        a.raw({0x49, 0x83, 0xE8, 0x20});        // sub r8,32
        a.raw({0x49, 0x83, 0xF8, 0x20});        // cmp r8,32
        a.j8(JAE, loop);
        a.bind(tail);
        a.raw({0x4D, 0x85, 0xC0});              // test r8,r8
        a.j8(JZ, nf);
        a.bind(t);
        a.raw({0x38, 0x11});                    // cmp [rcx],dl
        a.j8(JZ, bytefound);
        a.raw({0x48, 0xFF, 0xC1});              // inc rcx
        a.raw({0x49, 0xFF, 0xC8});              // dec r8
        a.j8(JNZ, t);
        a.bind(nf);
        a.raw({0x31, 0xC0});                    // xor eax,eax
        a.raw(kVzeroupper);
        a.raw({0xC3});
        a.bind(found);
        a.raw({0x0F, 0xBC, 0xC0});              // bsf eax,eax
        a.raw({0x48, 0x01, 0xC8});              // add rax,rcx
        a.raw(kVzeroupper);
        a.raw({0xC3});
        a.bind(bytefound);
        a.raw({0x48, 0x89, 0xC8});              // mov rax,rcx
        a.raw(kVzeroupper);
        a.raw({0xC3});
        return a.b;
    }
    a.raw({0x57});                              // push edi
    a.raw({0x8B, 0x7C, 0x24, 0x08});            // mov edi,[esp+8]
    a.raw({0x8B, 0x54, 0x24, 0x0C});            // mov edx,[esp+0Ch]
    a.raw({0x8B, 0x4C, 0x24, 0x10});            // mov ecx,[esp+10h]
    a.raw({0xC5, 0xF9, 0x6E, 0xCA});            // vmovd xmm1,edx
    a.raw({0xC4, 0xE2, 0x7D, 0x78, 0xC9});      // vpbroadcastb ymm1,xmm1
    a.raw({0x83, 0xF9, 0x20});                  // cmp ecx,32
    a.j8(JB, tail);
    a.bind(loop);
    a.raw({0xC5, 0xF5, 0x74, 0x07});            // vpcmpeqb ymm0,ymm1,[edi]
    a.raw({0xC5, 0xFD, 0xD7, 0xC0});            // vpmovmskb eax,ymm0
    a.raw({0x85, 0xC0});                        // test eax,eax
    a.j8(JNZ, found);
    a.raw({0x83, 0xC7, 0x20});                  // add edi,32
    a.raw({0x83, 0xE9, 0x20});                  // sub ecx,32
    a.raw({0x83, 0xF9, 0x20});                  // cmp ecx,32
    a.j8(JAE, loop);
    a.bind(tail);
    a.raw({0x89, 0xD0});                        // mov eax,edx
    a.raw({0x85, 0xC9});                        // test ecx,ecx
    a.j8(JZ, nf);
    a.raw({0xF2, 0xAE});                        // repne scasb
    a.j8(JNZ, nf);
    a.raw({0x8D, 0x47, 0xFF});                  // lea eax,[edi-1]
    a.j8(JMP, out);
    a.bind(nf);
    a.raw({0x31, 0xC0});                        // xor eax,eax
    a.j8(JMP, out);
    a.bind(found);
    a.raw({0x0F, 0xBC, 0xC0});                  // bsf eax,eax
    a.raw({0x01, 0xF8});                        // add eax,edi
    a.bind(out);
    a.raw(kVzeroupper);
    a.raw({0x5F, 0xC3});                        // pop edi; ret
    return a.b;
}

/// strchr-shaped unit scans (byte or UTF-16 unit; @p needle_ptr: the second
/// argument is a pointer whose first unit is the target, as for a
/// one-unit strstr/wcsstr needle). A unit-at-a-time loop is SLOWER than
/// vcruntime's vector scans, which only biases the comparison toward the
/// host; the str*/wcs* rule (host faster at every length, from 0) is
/// therefore decided conservatively.
std::vector<u8> unit_scan(bool x64, bool wide, bool needle_ptr) {
    std::vector<u8> v;
    auto raw = [&](std::initializer_list<u8> x) { v.insert(v.end(), x); };
    if (!x64) {
        raw({0x8B, 0x4C, 0x24, 0x04});          // mov ecx,[esp+4]
        raw({0x8B, 0x54, 0x24, 0x08});          // mov edx,[esp+8]
    }
    if (needle_ptr) {
        if (wide) raw({0x0F, 0xB7, 0x12}); else raw({0x0F, 0xB6, 0x12});  // movzx edx,[rdx]
    }
    if (!wide) {
        raw({0x0F, 0xB6, 0x01, 0x38, 0xD0});    // L: movzx eax,byte [rcx]; cmp al,dl
        raw({0x74, static_cast<u8>(x64 ? 9 : 7)});
        raw({0x84, 0xC0});                      // test al,al
        raw({0x74, static_cast<u8>(x64 ? 9 : 6)});
        if (x64) raw({0x48, 0xFF, 0xC1, 0xEB, 0xF0}); else raw({0x41, 0xEB, 0xF2});
    } else {
        raw({0x0F, 0xB7, 0x01, 0x66, 0x39, 0xD0});  // L: movzx eax,word [rcx]; cmp ax,dx
        raw({0x74, static_cast<u8>(x64 ? 0x0B : 0x0A)});
        raw({0x66, 0x85, 0xC0});                // test ax,ax
        raw({0x74, static_cast<u8>(x64 ? 0x0A : 0x08)});
        if (x64) raw({0x48, 0x83, 0xC1, 0x02, 0xEB, 0xED}); else raw({0x83, 0xC1, 0x02, 0xEB, 0xEE});
    }
    if (x64) raw({0x48, 0x89, 0xC8, 0xC3}); else raw({0x89, 0xC8, 0xC3});  // found: mov rax,rcx; ret
    raw({0x31, 0xC0, 0xC3});                    // nf: xor eax,eax; ret
    return v;
}

std::vector<std::pair<std::string, std::vector<u8>>> bench_exports(bool x64) {
    // rep_*: a second guest arm, the string-instruction forms (REP MOVSB /
    // STOSB / REPE CMPSB / REPNE SCASB, vcruntime_interpose_test_support.hpp)
    // that ERMS-era CRTs use for mid sizes; the crossover is taken against
    // the faster of the two guest arms at each size.
    return {{"rep_memcpy", ref_memmove(x64)}, {"rep_memmove", ref_memmove(x64)},
            {"rep_memset", ref_memset(x64)},  {"rep_memcmp", ref_memcmp(x64)},
            {"rep_memchr", ref_memchr(x64)},
            {"memcpy", avx2_copy(x64)},     {"memmove", avx2_copy(x64)},
            {"memset", avx2_fill(x64)},     {"memcmp", avx2_compare(x64)},
            {"memchr", avx2_scan(x64)},     {"strchr", unit_scan(x64, false, false)},
            {"strrchr", unit_scan(x64, false, false)},
            {"strstr", unit_scan(x64, false, true)},
            {"wcschr", unit_scan(x64, true, false)},
            {"wcsrchr", unit_scan(x64, true, false)},
            {"wcsstr", unit_scan(x64, true, true)}};
}

/// Driver: K x (target(a0, a1, a2)) through an indirect CALL, as an IAT call.
std::vector<u8> driver(bool x64) {
    auto d32 = [](std::vector<u8>& v, u32 x) {
        for (int i = 0; i < 4; ++i) v.push_back(static_cast<u8>(x >> (8 * i)));
    };
    std::vector<u8> v;
    const u32 c = static_cast<u32>(kCells);
    if (x64) {
        v.insert(v.end(), {0x53, 0x48, 0x83, 0xEC, 0x20});      // push rbx; sub rsp,0x20
        v.insert(v.end(), {0x8B, 0x1C, 0x25}); d32(v, c + 32);  // mov ebx,[K]
        const usize loop = v.size();
        v.insert(v.end(), {0x48, 0x8B, 0x0C, 0x25}); d32(v, c + 8);   // mov rcx,[a0]
        v.insert(v.end(), {0x48, 0x8B, 0x14, 0x25}); d32(v, c + 16);  // mov rdx,[a1]
        v.insert(v.end(), {0x4C, 0x8B, 0x04, 0x25}); d32(v, c + 24);  // mov r8,[a2]
        v.insert(v.end(), {0xFF, 0x14, 0x25}); d32(v, c);              // call [target]
        v.insert(v.end(), {0xFF, 0xCB, 0x0F, 0x85});                    // dec ebx; jnz loop
        d32(v, static_cast<u32>(static_cast<i64>(loop) - static_cast<i64>(v.size() + 4)));
        v.insert(v.end(), {0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3});       // add rsp,0x20; pop rbx; ret
        return v;
    }
    v.insert(v.end(), {0x53, 0x8B, 0x1D}); d32(v, c + 32);       // push ebx; mov ebx,[K]
    const usize loop = v.size();
    v.insert(v.end(), {0xFF, 0x35}); d32(v, c + 24);             // push dword [a2]
    v.insert(v.end(), {0xFF, 0x35}); d32(v, c + 16);             // push dword [a1]
    v.insert(v.end(), {0xFF, 0x35}); d32(v, c + 8);              // push dword [a0]
    v.insert(v.end(), {0xFF, 0x15}); d32(v, c);                  // call [target]
    v.insert(v.end(), {0x83, 0xC4, 0x0C, 0x4B, 0x0F, 0x85});      // add esp,12; dec ebx; jnz
    d32(v, static_cast<u32>(static_cast<i64>(loop) - static_cast<i64>(v.size() + 4)));
    v.insert(v.end(), {0x5B, 0xC3});                             // pop ebx; ret
    return v;
}

struct Bench {
    bool x64;
    GuestMemory memory;
    capsule::Vfs vfs;
    std::unique_ptr<CrtMemoryManager> crt_mem;
    CrtStringManager strings;
    ThunkRegistry registry{kThunkBase};
    std::unique_ptr<ModuleManager> mm;
    ProcessManager process;
    Image img;
    GuestAddr base;
    GuestAddr stub_region;
    usize stub_slot;

    explicit Bench(bool is64, GuestAddr at = 0)
        : x64(is64), img(build_image(is64, "VCRUNTIME140.dll", bench_exports(is64))),
          base(at != 0 ? at : (!is64 ? kImg32 : kImg64)) {
        crt_mem = std::make_unique<CrtMemoryManager>(memory);
        EXPECT_TRUE(registry.register_dll("ucrtbase.dll",
                                          create_crt_string_thunks(strings, *crt_mem, vfs)));
        mm = std::make_unique<ModuleManager>(registry);
        mm->bind_guest_memory(memory);
        mm->set_main_module(0x400000, "C:\\app\\bench.exe");
        EXPECT_TRUE(memory.map_region(kLow, kLowSize, MemoryProtection::ReadWrite));
        EXPECT_TRUE(memory.map_region(kData, kDataSize, MemoryProtection::ReadWrite));
        EXPECT_TRUE(memory.map_region(kThunkBase, kThunkSlab, MemoryProtection::ReadWrite));
        std::vector<u8> slab(kThunkSlab, 0);
        for (usize s = 0; s + ThunkRegistry::STUB_SIZE <= kThunkSlab; s += 16) {
            std::fill_n(slab.begin() + static_cast<std::ptrdiff_t>(s), ThunkRegistry::STUB_SIZE, u8{0x90});
        }
        EXPECT_TRUE(memory.write_bytes(kThunkBase, slab.data(), slab.size()));
        EXPECT_TRUE(memory.map_region(base, kImageSize, MemoryProtection::ReadWrite));
        EXPECT_TRUE(memory.write_bytes(base, img.bytes.data(), img.bytes.size()));
        EXPECT_TRUE(memory.protect_region(base, kImageSize, MemoryProtection::ReadExecute));
        mm->register_main_module_exports(base, "C:\\app\\VCRUNTIME140.dll", base + kImageSize,
                                         x64 ? 64 : 32,
                                         std::span<const u8>(img.bytes.data(), img.bytes.size()));
        const auto drv = driver(x64);
        EXPECT_TRUE(memory.write_bytes(kDriver, drv.data(), drv.size()));
        if (x64) {
            // The interposer's x64 stubs live on a page within rel32 reach
            // of the image; the bench's own stubs use one just above it.
            EXPECT_TRUE(ensure_x64_accel_arena(memory, kThunkBase));
            stub_region = base + 0x10000;
            EXPECT_TRUE(memory.map_region(stub_region, 0x1000, MemoryProtection::ReadExecute));
            stub_slot = X64_ACCEL_STUB_SLOT;
        } else {
            EXPECT_TRUE(ensure_crt_accel_arena_x86(memory));
            stub_region = CRT_ACCEL_BASE_X86 + CRT_ACCEL_STUB_OFFSET_X86;
            stub_slot = CRT_ACCEL_STUB_SLOT_X86;
        }
    }

    GuestAddr original(vcrt::Fn fn) const { return base + img.rva(vcrt::export_name(fn)); }
    GuestAddr rep_body(vcrt::Fn fn) const {
        return base + img.rva(std::string("rep_") + vcrt::export_name(fn));
    }
    GuestAddr host_slot(vcrt::Fn fn) const {
        return registry.get_thunk_address("ucrtbase.dll", vcrt::internal_export_name(fn), true).value();
    }

    /// A stub with @p threshold at the top slots (index from the end), so it
    /// never collides with what the interposer allocates from the bottom.
    GuestAddr stub(vcrt::Fn fn, u32 threshold, usize index_from_end) {
        const usize slots =
            (x64 ? 0x1000 : CRT_ACCEL_RESUME_OFFSET_X86 - CRT_ACCEL_STUB_OFFSET_X86) / stub_slot;
        const GuestAddr at = stub_region + (slots - 1 - index_from_end) * stub_slot;
        const auto code = x64 ? vcrt::encode_stub_x64(at, threshold, host_slot(fn), original(fn))
                              : vcrt::encode_stub_x86(at, threshold, host_slot(fn), original(fn));
        EXPECT_FALSE(code.empty());
        EXPECT_TRUE(write_accel_code(memory, at, code.data(), code.size()));
        return at;
    }

    /// K calls of @p target(a0, a1, a2); returns elapsed ns (and RAX of the run).
    double run(GuestAddr target, u64 a0, u64 a1, u64 a2, u64 k, u64* rax = nullptr) {
        auto cell = [&](u32 off, u64 v) { EXPECT_TRUE(memory.write_u64(kCells + off, v)); };
        cell(0, target);
        cell(8, a0);
        cell(16, a1);
        cell(24, a2);
        cell(32, k);
        cpu::CpuState cpu;
        if (x64) {
            cpu.init_long_mode();
            cpu.set_sp(kStack64);
            EXPECT_TRUE(memory.write_u64(kStack64, cli::PROCESS_RETURN_SENTINEL));
        } else {
            cpu.init_protected_mode();
            cpu.set_sp(kStack32);
            EXPECT_TRUE(memory.write_u32(kStack32, static_cast<u32>(cli::PROCESS_RETURN_SENTINEL)));
        }
        cpu.rip = kDriver;
        cli::RunConfig config;
        config.max_instructions = ~u64{0} >> 2;
        const auto t0 = std::chrono::steady_clock::now();
        (void)cli::run_execution_loop(cpu, memory, registry, process, config);
        const auto t1 = std::chrono::steady_clock::now();
        if (rax) *rax = cpu.gpr.rax;
        return std::chrono::duration<double, std::nano>(t1 - t0).count();
    }
};

struct Case {
    u64 a0, a1, a2;
};

/// The arguments for @p fn at size/length @p n (buffers prepared here).
Case prepare(Bench& b, vcrt::Fn fn, u64 n) {
    GuestMemory& m = b.memory;
    switch (fn) {
        case vcrt::Fn::Memcpy:
        case vcrt::Fn::Memmove:
            return {kB + 1, kA + 1, n};  // unaligned, non-overlapping
        case vcrt::Fn::Memset:
            return {kB + 1, 0x5A, n};
        case vcrt::Fn::Memcmp: {
            std::vector<u8> z(n + 1, 0x33);
            EXPECT_TRUE(m.write_bytes(kA, z.data(), z.size()));
            EXPECT_TRUE(m.write_bytes(kB, z.data(), z.size()));
            return {kA, kB, n};  // equal: the whole range is compared
        }
        case vcrt::Fn::Memchr: {
            std::vector<u8> z(n + 1, 0x11);
            EXPECT_TRUE(m.write_bytes(kA, z.data(), z.size()));
            return {kA, 0x99, n};  // absent: the whole range is scanned
        }
        case vcrt::Fn::Strchr:
        case vcrt::Fn::Strrchr:
        case vcrt::Fn::Strstr: {
            std::vector<u8> z(n + 1, 'x');
            z[n] = 0;
            EXPECT_TRUE(m.write_bytes(kA, z.data(), z.size()));
            EXPECT_TRUE(m.write_bytes(kB, reinterpret_cast<const u8*>("q"), 2));
            return {kA, fn == vcrt::Fn::Strstr ? kB : 'q', 0};
        }
        default: {
            std::vector<u16> z(n + 1, 'x');
            z[n] = 0;
            EXPECT_TRUE(m.write_bytes(kA, reinterpret_cast<const u8*>(z.data()), z.size() * 2));
            const u16 q[2] = {'q', 0};
            EXPECT_TRUE(m.write_bytes(kB, reinterpret_cast<const u8*>(q), 4));
            return {kA, fn == vcrt::Fn::Wcsstr ? kB : 'q', 0};
        }
    }
}

std::vector<u64> mem_sizes() {
    std::vector<u64> v;
    for (u64 n = 0; n <= 64; ++n) v.push_back(n);
    for (u64 n = 128; n <= (u64{1} << 20); n <<= 1) v.push_back(n);
    for (u64 n : {1000u, 4097u, 65537u}) v.push_back(n);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}
std::vector<u64> str_sizes() { return {0, 1, 2, 3, 4, 8, 16, 17, 32, 33, 64, 256, 1000, 4096, 65536}; }

[[nodiscard]] bool bench_enabled() {
    const char* v = std::getenv("VITREO_WS1_BENCH");
    return v != nullptr && std::strcmp(v, "1") == 0;
}

struct Row {
    double guest{0}, host{0}, stub_guest{0}, baked{0};
    double rep{0};  ///< string-instruction guest arm (sized functions only)
    /// The faster guest arm: what the host must beat.
    [[nodiscard]] double best_guest() const noexcept {
        return rep > 0 ? std::min(guest, rep) : guest;
    }
};
using Table = std::map<u64, Row>;

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

/// Measure every arm of @p fn on @p b at each size: 5 interleaved reps,
/// median ns per call. Arms: (a) the guest loop directly, (b) the stub with
/// threshold 0 (host always), (c) the stub forcing its guest branch, (d) the
/// stub with the baked threshold.
Table measure(Bench& b, vcrt::Fn fn, const std::vector<u64>& sizes) {
    const bool sized = static_cast<usize>(fn) <= static_cast<usize>(vcrt::Fn::Memchr);
    const GuestAddr direct = b.original(fn);
    const GuestAddr host_always = sized ? b.stub(fn, 0, 0) : b.host_slot(fn);
    const GuestAddr guest_always = sized ? b.stub(fn, 0x7FFFFFFF, 1) : direct;
    const u32 baked_t = vcrt::threshold(fn, b.x64 ? 64 : 32);
    const GuestAddr baked = !sized ? (baked_t == 0 ? host_always : direct)
                                   : (baked_t == vcrt::kNeverHost ? direct : b.stub(fn, baked_t, 2));
    const GuestAddr arms[5] = {direct, host_always, guest_always, baked,
                               sized ? b.rep_body(fn) : direct};
    const int narms = sized ? 5 : 4;
    std::map<u64, std::array<std::vector<double>, 5>> samples;
    std::map<u64, u64> iters;
    for (u64 n : sizes) {
        const Case c = prepare(b, fn, n);
        u64 k = 16;
        for (;;) {  // size K so the slowest arm's sample lasts >= 2 ms
            (void)b.run(direct, c.a0, c.a1, c.a2, k);  // warm (compile)
            (void)b.run(host_always, c.a0, c.a1, c.a2, k);
            const double t = std::min(b.run(direct, c.a0, c.a1, c.a2, k),
                                      b.run(host_always, c.a0, c.a1, c.a2, k));
            if (t >= 2e6 || k >= (u64{1} << 26)) break;
            k *= 2;
        }
        iters[n] = k;
    }
    for (int rep = 0; rep < 5; ++rep) {
        for (u64 n : sizes) {
            const Case c = prepare(b, fn, n);
            for (int a = 0; a < narms; ++a) {
                samples[n][a].push_back(b.run(arms[a], c.a0, c.a1, c.a2, iters[n]) /
                                        static_cast<double>(iters[n]));
            }
        }
    }
    Table t;
    for (u64 n : sizes) {
        Row& r = t[n];
        r.guest = median(samples[n][0]);
        r.host = median(samples[n][1]);
        r.stub_guest = median(samples[n][2]);
        r.baked = median(samples[n][3]);
        if (sized) r.rep = median(samples[n][4]);
        std::printf("WS1_BENCH bits=%d fn=%s n=%llu guest_ns=%.2f rep_ns=%.2f host_ns=%.2f "
                    "stub_guest_ns=%.2f baked_ns=%.2f\n",
                    b.x64 ? 64 : 32, vcrt::export_name(fn),
                    static_cast<unsigned long long>(n), r.guest, r.rep, r.host, r.stub_guest,
                    r.baked);
    }
    std::fflush(stdout);
    return t;
}

/// The program rule: the smallest sampled n from which host <= 0.9 * guest
/// at that n and every larger sampled n. nullopt when none.
std::optional<u64> crossover(const Table& t) {
    std::optional<u64> best;
    for (auto it = t.rbegin(); it != t.rend(); ++it) {
        if (it->second.host <= 0.9 * it->second.best_guest()) {
            best = it->first;
        } else {
            break;
        }
    }
    return best;
}

class VcrtBench : public ::testing::Test {
protected:
    void SetUp() override {
        if (!bench_enabled()) GTEST_SKIP() << "set VITREO_WS1_BENCH=1 (idle machine)";
        host_in_chain::force_for_test(host_in_chain::Force::On);
        JitCompiler::set_inchain_thunks_codegen_for_test(1);
        JitCompiler::set_jit_hot_threshold_for_test(1);
    }
    void TearDown() override {
        host_in_chain::force_for_test(host_in_chain::Force::None);
        JitCompiler::set_inchain_thunks_codegen_for_test(-1);
        JitCompiler::set_jit_hot_threshold_for_test(0);
    }
};

TEST_F(VcrtBench, SweepAndCrossover) {
    for (bool x64 : {false, true}) {
        Bench b(x64);
        const char* only = std::getenv("VITREO_WS1_BENCH_FN");  // one function
        for (usize i = 0; i < vcrt::kFnCount; ++i) {
            const auto fn = static_cast<vcrt::Fn>(i);
            if (only != nullptr && std::strcmp(only, vcrt::export_name(fn)) != 0) continue;
            const bool sized = i <= static_cast<usize>(vcrt::Fn::Memchr);
            const Table t = measure(b, fn, sized ? mem_sizes() : str_sizes());
            const auto x = crossover(t);
            if (x) {
                std::printf("WS1_CROSSOVER bits=%d fn=%s n=%llu\n", x64 ? 64 : 32,
                            vcrt::export_name(fn), static_cast<unsigned long long>(*x));
            } else {
                std::printf("WS1_CROSSOVER bits=%d fn=%s n=none\n", x64 ? 64 : 32,
                            vcrt::export_name(fn));
            }
        }
    }
}

// Stub overhead probe: a direct call of the body against the stub's guest
// branch, 31 short interleaved reps, min and median (the min approximates an
// uncontended run when the machine is not idle). Arms: x86 (jb rel32 in the
// accel arena), x64-near (jb rel32 from a page beside the image: the shipped
// form) and x64-far (the former far form, jae over a jmp qword [rip+0] to
// the original, hand-encoded here: it costs ~20 ns per call at n >= 1,
// which is why encode_stub_x64 has no far form). WS1_PROBE_ARM=<name> runs
// one arm.
TEST_F(VcrtBench, StubOverheadProbe) {
    struct Arm { bool x64; const char* name; };
    const Arm arms[] = {{false, "x86"}, {true, "x64-near"}, {true, "x64-far"}};
    const char* only = std::getenv("WS1_PROBE_ARM");
    for (const Arm& arm : arms) {
        if (only != nullptr && std::strcmp(only, arm.name) != 0) continue;
        Bench b(arm.x64);
        if (std::strcmp(arm.name, "x64-far") == 0) {
            ASSERT_TRUE(ensure_x64_accel_arena(b.memory, kThunkBase));
        }
        for (auto fn : {vcrt::Fn::Memcpy, vcrt::Fn::Memcmp}) {
            const GuestAddr direct = b.original(fn);
            GuestAddr stub = b.stub(fn, 0x7FFFFFFF, 1);
            if (std::strcmp(arm.name, "x64-far") == 0) {
                stub = kThunkBase + X64_ACCEL_OFFSET + X64_ACCEL_RESUME_OFFSET - 0x100;
                std::vector<u8> v{0x49, 0x81, 0xF8, 0xFF, 0xFF, 0xFF, 0x7F, 0x73, 0x0E,
                                  0xFF, 0x25, 0, 0, 0, 0};
                for (int i = 0; i < 8; ++i) v.push_back(static_cast<u8>(direct >> (8 * i)));
                v.insert(v.end(), {0xFF, 0x25, 0, 0, 0, 0});
                const GuestAddr host = b.host_slot(fn);
                for (int i = 0; i < 8; ++i) v.push_back(static_cast<u8>(host >> (8 * i)));
                ASSERT_TRUE(write_accel_code(b.memory, stub, v.data(), v.size()));
            }
            for (u64 n : {u64{0}, u64{1}, u64{8}, u64{16}, u64{64}}) {
                const Case c = prepare(b, fn, n);
                constexpr u64 k = 20000;
                for (int w = 0; w < 3; ++w) {
                    (void)b.run(direct, c.a0, c.a1, c.a2, k);
                    (void)b.run(stub, c.a0, c.a1, c.a2, k);
                }
                std::vector<double> d, st;
                for (int rep = 0; rep < 31; ++rep) {
                    d.push_back(b.run(direct, c.a0, c.a1, c.a2, k) / k);
                    st.push_back(b.run(stub, c.a0, c.a1, c.a2, k) / k);
                }
                std::printf("WS1_OVERHEAD %s fn=%s n=%llu direct_min=%.2f stub_min=%.2f "
                            "direct_med=%.2f stub_med=%.2f\n",
                            arm.name, vcrt::export_name(fn), static_cast<unsigned long long>(n),
                            *std::min_element(d.begin(), d.end()),
                            *std::min_element(st.begin(), st.end()), median(d), median(st));
            }
        }
    }
    std::fflush(stdout);
}

TEST_F(VcrtBench, Gate) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        Bench b(x64);
        for (usize i = 0; i <= static_cast<usize>(vcrt::Fn::Memchr); ++i) {
            const auto fn = static_cast<vcrt::Fn>(i);
            SCOPED_TRACE(vcrt::export_name(fn));
            const Table t = measure(b, fn, mem_sizes());
            const u32 baked = vcrt::threshold(fn, x64 ? 64 : 32);
            if (baked == vcrt::kNeverHost) {
                // Not interposed: the lookup returns the module's body (no
                // stub, no overhead). The rule must agree: no crossover.
                const auto x = crossover(t);
                EXPECT_FALSE(x.has_value()) << "crossover at " << (x ? *x : 0)
                                            << ": re-bake the threshold";
                continue;
            }
            for (const auto& [n, r] : t) {
                if (n >= 4096) {
                    EXPECT_GE(r.best_guest() / r.host, 2.0) << "n=" << n;
                }
                if (n < baked) {
                    EXPECT_LE(r.stub_guest - r.guest, 1.0) << "stub overhead, n=" << n;
                }
                const double best = std::min(r.guest, r.host);
                EXPECT_LE(r.baked, best + 1.0 + 0.05 * best) << "baked stub, n=" << n;
            }
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// The guest reference loops compute the C contracts (always runs).
// ---------------------------------------------------------------------------

TEST(VcrtBenchFixtures, GuestLoopsMatchTheCContract) {
    JitCompiler::set_jit_hot_threshold_for_test(1);
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        Bench b(x64);
        GuestMemory& m = b.memory;
        const u64 mask = x64 ? ~u64{0} : 0xFFFFFFFFull;
        std::vector<u8> pat(0x2000);
        for (usize i = 0; i < pat.size(); ++i) pat[i] = static_cast<u8>(i * 13 + 1);
        for (u64 n : {u64{0}, u64{1}, u64{31}, u64{32}, u64{33}, u64{100}, u64{4097}}) {
            SCOPED_TRACE(n);
            for (long delta : {0L, 5L, -5L}) {
                ASSERT_TRUE(m.write_bytes(kA, pat.data(), pat.size()));
                const GuestAddr src = kA + 0x100;
                const GuestAddr dst = delta == 0 ? kB + 3 : static_cast<GuestAddr>(static_cast<i64>(src) + delta);
                std::vector<u8> want(pat.size());
                ASSERT_TRUE(m.read_bytes(kA, want.data(), want.size()));
                std::vector<u8> wb(0x2000);
                ASSERT_TRUE(m.read_bytes(kB, wb.data(), wb.size()));
                if (delta == 0) {
                    std::memcpy(wb.data() + 3, want.data() + 0x100, n);
                } else {
                    std::memmove(want.data() + (dst - kA), want.data() + 0x100, n);
                }
                u64 rax = 0;
                b.run(b.original(vcrt::Fn::Memmove), dst, src, n, 1, &rax);
                EXPECT_EQ(rax & mask, dst);
                std::vector<u8> got(pat.size());
                ASSERT_TRUE(m.read_bytes(delta == 0 ? kB : kA, got.data(), got.size()));
                EXPECT_EQ(got, delta == 0 ? wb : want) << "copy delta=" << delta;
            }
            {
                u64 rax = 0;
                b.run(b.original(vcrt::Fn::Memset), kB + 1, 0x1A5, n, 1, &rax);
                EXPECT_EQ(rax & mask, kB + 1);
                if (n > 0) {
                    std::vector<u8> got(n);
                    ASSERT_TRUE(m.read_bytes(kB + 1, got.data(), n));
                    EXPECT_EQ(got, std::vector<u8>(n, 0xA5));
                }
            }
            for (u64 pos : {u64{0}, n / 2, n ? n - 1 : 0}) {
                std::vector<u8> z(n + 1, 0x40);
                ASSERT_TRUE(m.write_bytes(kA, z.data(), z.size()));
                ASSERT_TRUE(m.write_bytes(kB, z.data(), z.size()));
                u64 rax = 0;
                b.run(b.original(vcrt::Fn::Memcmp), kA, kB, n, 1, &rax);
                EXPECT_EQ(static_cast<i32>(rax), 0);
                if (n == 0) continue;
                ASSERT_TRUE(m.write_u8(kB + pos, 0x80));
                b.run(b.original(vcrt::Fn::Memcmp), kA, kB, n, 1, &rax);
                EXPECT_EQ(static_cast<i32>(rax), -1) << "pos=" << pos;
                b.run(b.original(vcrt::Fn::Memcmp), kB, kA, n, 1, &rax);
                EXPECT_EQ(static_cast<i32>(rax), 1) << "pos=" << pos;
                // memchr: first 0x80 is at pos (in kB).
                b.run(b.original(vcrt::Fn::Memchr), kB, 0x180, n, 1, &rax);
                EXPECT_EQ(rax & mask, kB + pos);
                b.run(b.original(vcrt::Fn::Memchr), kB, 0x77, n, 1, &rax);
                EXPECT_EQ(rax & mask, 0u);
            }
        }
        // Unit scans.
        const u8 s[] = "xxxq";
        ASSERT_TRUE(m.write_bytes(kA, s, sizeof s));
        ASSERT_TRUE(m.write_bytes(kB, reinterpret_cast<const u8*>("q"), 2));
        u64 rax = 0;
        b.run(b.original(vcrt::Fn::Strchr), kA, 'q', 0, 1, &rax);
        EXPECT_EQ(rax & mask, kA + 3);
        b.run(b.original(vcrt::Fn::Strstr), kA, kB, 0, 1, &rax);
        EXPECT_EQ(rax & mask, kA + 3);
        b.run(b.original(vcrt::Fn::Strchr), kA, 'z', 0, 1, &rax);
        EXPECT_EQ(rax & mask, 0u);
        const u16 w[] = {'x', 'x', 'q', 0};
        ASSERT_TRUE(m.write_bytes(kA, reinterpret_cast<const u8*>(w), sizeof w));
        b.run(b.original(vcrt::Fn::Wcschr), kA, 'q', 0, 1, &rax);
        EXPECT_EQ(rax & mask, kA + 4);
        b.run(b.original(vcrt::Fn::Wcschr), kA, 'z', 0, 1, &rax);
        EXPECT_EQ(rax & mask, 0u);
    }
    JitCompiler::set_jit_hot_threshold_for_test(0);
}
