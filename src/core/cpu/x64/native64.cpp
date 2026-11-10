// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - x86_64 native backend: Win64 guest execution

#include "core/cpu/x64/native64.hpp"

#include <atomic>

#include <cstring>
#include <initializer_list>

extern "C" {
unsigned long long native64_enter(unsigned short fs_selector);
void native64_collect(void);
void native64_sentinel(void);
void native64_process_sentinel(void);
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

u64 process_sentinel_return_address() noexcept {
    return reinterpret_cast<u64>(&native64_process_sentinel);
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

void emit_get_current_thread_id_stub(u8* dest) noexcept {
    // 64 8B 04 25 48 00 00 00   mov eax, fs:[0x48]   TEB ClientId.UniqueThread
    // C3                        ret
    static const u8 kCode[] = {0x64, 0x8B, 0x04, 0x25,
                               0x48, 0x00, 0x00, 0x00, 0xC3};
    std::memcpy(dest, kCode, sizeof(kCode));
}

void emit_absolute_jump(u8* dest, u64 target) noexcept {
    // FF 25 00 00 00 00   jmp qword ptr [rip+0]
    // <8-byte target>
    static const u8 kJmp[] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    std::memcpy(dest, kJmp, sizeof(kJmp));
    std::memcpy(dest + sizeof(kJmp), &target, sizeof(target));
}

namespace {

/// Minimal forward-patching byte assembler for the SRW bodies.
struct Asm {
    u8* out;
    usize n = 0;
    void b(std::initializer_list<u8> bytes) noexcept {
        for (u8 v : bytes) out[n++] = v;
    }
    void u32le(u32 v) noexcept { std::memcpy(out + n, &v, 4); n += 4; }
    void u64le(u64 v) noexcept { std::memcpy(out + n, &v, 8); n += 8; }
    /// Emit `0F 8x rel32` and return the patch position of rel32.
    usize jcc32(u8 cc) noexcept { b({0x0F, cc}); const usize at = n; u32le(0); return at; }
    void patch(usize at, usize target) noexcept {
        const i32 rel = static_cast<i32>(static_cast<i64>(target) -
                                         static_cast<i64>(at + 4));
        std::memcpy(out + at, &rel, 4);
    }
};

constexpr u8 kJz = 0x84, kJnz = 0x85;

/// The shared prologue: hop unless the lock pointer is non-null and
/// 8-aligned (the host's in-word condition under identity mapping) and no
/// thread holds an exclusive-recursion entry. Returns the patch positions.
void srw_prologue(Asm& a, const void* recursion_threads, usize (&fb)[3]) noexcept {
    a.b({0x48, 0x85, 0xC9});                 // test rcx, rcx
    fb[0] = a.jcc32(kJz);                    // jz fallback
    a.b({0xF6, 0xC1, 0x07});                 // test cl, 7
    fb[1] = a.jcc32(kJnz);                   // jnz fallback
    a.b({0x49, 0xBB});                       // mov r11, imm64
    a.u64le(reinterpret_cast<u64>(recursion_threads));
    a.b({0x49, 0x83, 0x3B, 0x00});           // cmp qword [r11], 0
    fb[2] = a.jcc32(kJnz);                   // jne fallback
}

/// The hop: enter the shared collector exactly as the slot's own
/// `call fs:[collect]` would have, with the slot's gateway as the pushed
/// return address. R11 is volatile and never an argument register.
void srw_fallback(Asm& a, u64 gateway) noexcept {
    a.b({0x49, 0xBB});                       // mov r11, gateway
    a.u64le(gateway);
    a.b({0x41, 0x53});                       // push r11
    a.b({0x64, 0xFF, 0x24, 0x25});           // jmp qword fs:[kN64Collect]
    a.u32le(kN64Collect);
}

}  // namespace

usize emit_srw_try_acquire_exclusive_body(u8* dest, u64 gateway,
                                          const void* recursion_threads) noexcept {
    // BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK) on Vitreo's in-word layout
    // (srw_word.hpp: EXCL bit 0, WAITERS bit 1, owner tid from bit 2).
    Asm a{dest};
    usize fb[3];
    srw_prologue(a, recursion_threads, fb);
    const usize load = a.n;
    a.b({0x48, 0x8B, 0x01});                 // mov rax, [rcx]
    a.b({0x48, 0x89, 0xC2});                 // mov rdx, rax
    a.b({0x48, 0x83, 0xE2, 0xFD});           // and rdx, ~WAITERS
    const usize held = a.jcc32(kJnz);        // held (even by us): FALSE
    a.b({0x64, 0x8B, 0x14, 0x25});           // mov edx, fs:[0x48]
    a.u32le(kTebClientIdThread);
    a.b({0x48, 0xC1, 0xE2, 0x02});           // shl rdx, 2
    a.b({0x48, 0x83, 0xCA, 0x01});           // or rdx, EXCL
    a.b({0x49, 0x89, 0xC0});                 // mov r8, rax
    a.b({0x49, 0x83, 0xE0, 0x02});           // and r8, WAITERS
    a.b({0x4C, 0x09, 0xC2});                 // or rdx, r8
    a.b({0xF0, 0x48, 0x0F, 0xB1, 0x11});     // lock cmpxchg [rcx], rdx
    a.patch(a.jcc32(kJnz), load);            // word changed: reload
    a.b({0xB8, 0x01, 0x00, 0x00, 0x00});     // mov eax, 1
    a.b({0xC3});                             // ret
    a.patch(held, a.n);
    a.b({0x31, 0xC0});                       // xor eax, eax
    a.b({0xC3});                             // ret
    for (usize at : fb) a.patch(at, a.n);
    srw_fallback(a, gateway);
    return a.n;
}

usize emit_srw_release_exclusive_body(u8* dest, u64 gateway,
                                      const void* recursion_threads) noexcept {
    // VOID ReleaseSRWLockExclusive(PSRWLOCK). Only the uncontended release by
    // its owner is served here; an unheld word (the host counts it), a word
    // with WAITERS (the host must wake them) and a foreign owner (the host
    // reports it) all take the hop.
    Asm a{dest};
    usize fb[6];
    usize pro[3];
    srw_prologue(a, recursion_threads, pro);
    fb[0] = pro[0]; fb[1] = pro[1]; fb[2] = pro[2];
    const usize load = a.n;
    a.b({0x48, 0x8B, 0x01});                 // mov rax, [rcx]
    a.b({0xA8, 0x01});                       // test al, EXCL
    fb[3] = a.jcc32(kJz);                    // not held: host
    a.b({0xA8, 0x02});                       // test al, WAITERS
    fb[4] = a.jcc32(kJnz);                   // waiters: host wakes them
    a.b({0x48, 0x89, 0xC2});                 // mov rdx, rax
    a.b({0x48, 0xC1, 0xEA, 0x02});           // shr rdx, 2
    a.b({0x64, 0x3B, 0x14, 0x25});           // cmp edx, fs:[0x48]
    a.u32le(kTebClientIdThread);
    fb[5] = a.jcc32(kJnz);                   // foreign owner: host reports
    a.b({0x31, 0xD2});                       // xor edx, edx
    a.b({0xF0, 0x48, 0x0F, 0xB1, 0x11});     // lock cmpxchg [rcx], rdx
    a.patch(a.jcc32(kJnz), load);            // word changed: reload
    a.b({0xC3});                             // ret
    for (usize at : fb) a.patch(at, a.n);
    srw_fallback(a, gateway);
    return a.n;
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
