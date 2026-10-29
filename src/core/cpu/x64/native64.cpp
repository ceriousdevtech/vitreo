// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - x86_64 native backend: Win64 guest execution

#include "core/cpu/x64/native64.hpp"

#include <atomic>

#include <cstring>

extern "C" {
unsigned long long native64_enter(unsigned short fs_selector);
void native64_collect(void);
void native64_sentinel(void);
void native64_fault_return(void);
void native64_fs_repair(void);
void native64_enter_fs_probe(void);
void native64_enter_fs_probe_end(void);
void native64_enter_fs_trap(void);
void native64_collect_fs_probe(void);
void native64_collect_fs_probe_end(void);
void native64_collect_fs_trap(void);
void native64_sentinel_fs_probe(void);
void native64_sentinel_fs_probe_end(void);
void native64_sentinel_fs_trap(void);
}

namespace vitreo::cpu::x64 {


void publish_gs_hooks(void* teb_base) noexcept {
    auto* t = Native64Transfer::at(teb_base);
    t->collect  = collect_address();
    t->sentinel = sentinel_return_address();

    // Seed the FXSAVE image the first entry will restore from. A zero image is
    // not merely uninitialised, it is actively wrong: FCW 0 and MXCSR 0 unmask
    // every x87 and SSE exception, so the guest's first ordinary floating-point
    // operation raises one. Windows starts an x64 thread with FCW 0x027F and
    // MXCSR 0x1F80.
    u8* fx = static_cast<u8*>(teb_base) + kN64FxSave;
    std::memset(fx, 0, kN64FxSaveSize);
    const u16 fcw = 0x027F;
    const u32 mxcsr = 0x1F80;
    const u32 mxcsr_mask = 0x0000FFBF;   // reserved bits must be writable-clear
    std::memcpy(fx + 0, &fcw, sizeof(fcw));
    std::memcpy(fx + 24, &mxcsr, sizeof(mxcsr));
    std::memcpy(fx + 28, &mxcsr_mask, sizeof(mxcsr_mask));
}

u64 sentinel_return_address() noexcept {
    return reinterpret_cast<u64>(&native64_sentinel);
}

namespace {
std::atomic<DispatchAliasResolver> g_alias_resolver{nullptr};
std::atomic<NativeServiceResolver> g_service_resolver{nullptr};
}  // namespace

void native64_set_dispatch_alias_resolver(DispatchAliasResolver fn) noexcept {
    g_alias_resolver.store(fn, std::memory_order_release);
}

u64 native64_dispatch_alias_gateway(u64 rip) noexcept {
    const auto fn = g_alias_resolver.load(std::memory_order_acquire);
    return fn != nullptr ? fn(rip) : 0;
}

void native64_set_service_resolver(NativeServiceResolver fn) noexcept {
    g_service_resolver.store(fn, std::memory_order_release);
}

u64 native64_service_gateway(u32 service_id) noexcept {
    const auto fn = g_service_resolver.load(std::memory_order_acquire);
    return fn != nullptr ? fn(service_id) : 0;
}

void emit_get_last_error_stub(u8* dest) noexcept {
    // 64 8B 04 25 68 00 00 00   mov eax, fs:[0x68]
    // C3                        ret
    static const u8 kCode[] = {0x64, 0x8B, 0x04, 0x25,
                               0x68, 0x00, 0x00, 0x00, 0xC3};
    std::memcpy(dest, kCode, sizeof(kCode));
}

void emit_set_last_error_stub(u8* dest) noexcept {
    // 64 89 0C 25 68 00 00 00   mov fs:[0x68], ecx   (Win64 arg 1)
    // C3                        ret                  (returns void)
    static const u8 kCode[] = {0x64, 0x89, 0x0C, 0x25,
                               0x68, 0x00, 0x00, 0x00, 0xC3};
    std::memcpy(dest, kCode, sizeof(kCode));
}

u64 fault_return_address() noexcept {
    return reinterpret_cast<u64>(&native64_fault_return);
}

u64 fs_repair_address() noexcept {
    return reinterpret_cast<u64>(&native64_fs_repair);
}

u64 fs_probe_resume_address(u64 fault_rip) noexcept {
    struct Probe {
        u64 begin;
        u64 end;
        u64 trap;
    };
    const Probe probes[] = {
        {reinterpret_cast<u64>(&native64_enter_fs_probe),
         reinterpret_cast<u64>(&native64_enter_fs_probe_end),
         reinterpret_cast<u64>(&native64_enter_fs_trap)},
        {reinterpret_cast<u64>(&native64_collect_fs_probe),
         reinterpret_cast<u64>(&native64_collect_fs_probe_end),
         reinterpret_cast<u64>(&native64_collect_fs_trap)},
        {reinterpret_cast<u64>(&native64_sentinel_fs_probe),
         reinterpret_cast<u64>(&native64_sentinel_fs_probe_end),
         reinterpret_cast<u64>(&native64_sentinel_fs_trap)},
    };
    for (const auto& probe : probes) {
        if ((fault_rip >= probe.begin && fault_rip < probe.end) ||
            fault_rip == probe.trap) {
            return probe.begin;
        }
    }
    return 0;
}

u64 collect_address() noexcept {
    return reinterpret_cast<u64>(&native64_collect);
}

void emit_thunk_stub(u8* dest) noexcept {
    // call qword ptr fs:[kN64Collect]
    //   64          FS prefix
    //   FF /2       CALL r/m64 (64-bit operand size is the default; no REX.W)
    //   ModRM 14    mod=00 reg=010 rm=100 -> SIB follows
    //   SIB   25    scale=0 index=none base=none -> disp32 is absolute
    //   disp32      offset within the GS segment, i.e. from the TEB base
    static constexpr u8 kPrefixAndOpcode[4] = {0x64, 0xFF, 0x14, 0x25};
    std::memcpy(dest, kPrefixAndOpcode, sizeof(kPrefixAndOpcode));
    const u32 disp = static_cast<u32>(kN64Collect);
    std::memcpy(dest + 4, &disp, sizeof(disp));
}

void emit_guard_dispatch_stub(u8* dest) noexcept {
    // jmp rax / six-byte NOP padding. The guest's CALL to the dispatch pointer
    // already placed the final target's return address on the stack, so this
    // must be a tail jump rather than another call.
    static constexpr u8 kStub[8] = {
        0xFF, 0xE0,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    };
    std::memcpy(dest, kStub, sizeof(kStub));
}

Native64Exit enter(u16 fs_selector) noexcept {
    return static_cast<Native64Exit>(native64_enter(fs_selector));
}

} // namespace vitreo::cpu::x64
