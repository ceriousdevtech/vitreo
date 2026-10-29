// SPDX-License-Identifier: LGPL-2.1-only

/// @file guest_accel.cpp
/// @brief Guest-side x86 implementations of hot CRT primitives. See header.

#include "winapi/msvcrt/guest_accel.hpp"
#include "winapi/msvcrt/io/crt_stdio_buffer.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "core/memory/guest_memory.hpp"

#include <cstdio>
#include <cstring>
#include <iterator>
#include <initializer_list>
#include <utility>
#include <cstdlib>
#include <string>
#include <vector>

namespace vitreo::winapi {

namespace {

/// A little emitter, so the implementations below read as instructions rather
/// than as hex. Everything here is 32-bit cdecl: arguments on the stack, the
/// caller pops them, EAX carries the result, and EBX/ESI/EDI/EBP belong to the
/// caller. The Windows ABI guarantees DF=0 on entry and requires it on exit.
struct Asm {
    std::vector<u8> code;
    std::vector<std::pair<usize, std::string>> fixups;

    /// Emit `jmp <named export>`; the rel32 is filled in at install time once
    /// the thunk arena has assigned the export an address.
    void mark_extern(const char* qualified_name) {
        const usize slot = jmp_extern();
        fixups.emplace_back(slot, qualified_name);
    }

    void b(std::initializer_list<u8> bytes) {
        code.insert(code.end(), bytes.begin(), bytes.end());
    }
    void u32le(u32 v) {
        b({static_cast<u8>(v), static_cast<u8>(v >> 8),
           static_cast<u8>(v >> 16), static_cast<u8>(v >> 24)});
    }

    usize here() const { return code.size(); }

    void push_esi() { b({0x56}); }
    void push_edi() { b({0x57}); }
    void pop_esi()  { b({0x5E}); }
    void pop_edi()  { b({0x5F}); }
    void pop_eax()  { b({0x58}); }
    void ret()      { b({0xC3}); }
    void rep_stosb()  { b({0xF3, 0xAA}); }
    void rep_movsb()  { b({0xF3, 0xA4}); }
    void repne_scasb(){ b({0xF2, 0xAE}); }
    void cld()      { b({0xFC}); }
    void std_()     { b({0xFD}); }

    /// mov <reg>, [esp + disp8]
    void mov_reg_esp(u8 reg, u8 disp) { b({0x8B, static_cast<u8>(0x44 | (reg << 3)), 0x24, disp}); }
    /// push <reg> already covered; generic register ids: 0=eax 1=ecx 2=edx 3=ebx
    /// 4=esp 5=ebp 6=esi 7=edi
    void xor_eax_eax() { b({0x31, 0xC0}); }
    void mov_ecx_imm32(u32 v) { b({0xB9}); u32le(v); }
    void not_ecx() { b({0xF7, 0xD1}); }
    void dec_ecx() { b({0x49}); }
    void mov_eax_ecx() { b({0x89, 0xC8}); }
    void cmp_edi_esi() { b({0x39, 0xF7}); }
    void cmp_edi_eax() { b({0x39, 0xC7}); }
    /// lea eax, [esi + ecx]
    void lea_eax_esi_ecx() { b({0x8D, 0x04, 0x0E}); }
    /// lea esi, [esi + ecx - 1]
    void lea_esi_esi_ecx_m1() { b({0x8D, 0x74, 0x0E, 0xFF}); }
    /// lea edi, [edi + ecx - 1]
    void lea_edi_edi_ecx_m1() { b({0x8D, 0x7C, 0x0F, 0xFF}); }
    /// mov ecx, [esp+4]  -- the single stdcall argument
    void mov_ecx_arg0() { b({0x8B, 0x4C, 0x24, 0x04}); }
    /// mov eax, fs:[0x24]  -- TEB ClientId.UniqueThread, i.e. the current TID
    void mov_eax_teb_tid() { b({0x64, 0xA1, 0x24, 0x00, 0x00, 0x00}); }
    void cmp_ecx_off_eax(u8 off) { b({0x39, 0x41, off}); }
    void mov_ecx_off_eax(u8 off) { b({0x89, 0x41, off}); }
    void mov_ecx_off_imm32(u8 off, u32 v) { b({0xC7, 0x41, off}); u32le(v); }
    void inc_ecx_off(u8 off) { b({0xFF, 0x41, off}); }
    void dec_ecx_off(u8 off) { b({0xFF, 0x49, off}); }
    void mov_eax_ecx_off(u8 off) { b({0x8B, 0x41, off}); }
    void mov_edx_ecx_off(u8 off) { b({0x8B, 0x51, off}); }
    void xor_edx_edx() { b({0x31, 0xD2}); }
    void test_edx_edx() { b({0x85, 0xD2}); }
    void mov_eax_imm32(u32 v) { b({0xB8}); u32le(v); }
    void mov_edx_imm32(u32 v) { b({0xBA}); u32le(v); }
    void cmp_eax_imm8(u8 v) { b({0x83, 0xF8, v}); }
    /// cmp dword [ecx+off], 0
    void cmp_ecx_off_imm32_zero(u8 off) { b({0x83, 0x79, off, 0x00}); }
    /// mov edx, [esp+4]  -- first stack argument
    void mov_edx_arg0() { b({0x8B, 0x54, 0x24, 0x04}); }
    /// mov [edx+off], <reg>  for ebp/ebx/edi/esi/eax
    void mov_edx_off_ebp(u8 off) { b({0x89, 0x6A, off}); }
    void mov_edx_off_ebx(u8 off) { b({0x89, 0x5A, off}); }
    void mov_edx_off_edi(u8 off) { b({0x89, 0x7A, off}); }
    void mov_edx_off_esi(u8 off) { b({0x89, 0x72, off}); }
    void mov_edx_off_eax(u8 off) { b({0x89, 0x42, off}); }
    void mov_edx_off_imm32(u8 off, u32 v) { b({0xC7, 0x42, off}); u32le(v); }
    /// lea eax, [esp+4]
    void lea_eax_esp4() { b({0x8D, 0x44, 0x24, 0x04}); }
    /// mov eax, [esp]  -- the return address
    void mov_eax_at_esp() { b({0x8B, 0x04, 0x24}); }
    /// mov eax, fs:[0]  -- SEH chain head
    void mov_eax_fs0() { b({0x64, 0xA1, 0x00, 0x00, 0x00, 0x00}); }
    void ret_c3() { b({0xC3}); }
    void push_ebp()  { b({0x55}); }
    void pop_ebp()   { b({0x5D}); }
    void push_ebx()  { b({0x53}); }
    void pop_ebx()   { b({0x5B}); }
    void mov_ebp_esp() { b({0x89, 0xE5}); }
    void mov_esp_ebp() { b({0x89, 0xEC}); }
    void sub_esp_imm8(u8 v) { b({0x83, 0xEC, v}); }
    void add_esp_imm8(u8 v) { b({0x83, 0xC4, v}); }
    void xor_esi_esi() { b({0x31, 0xF6}); }
    /// mov <reg>, [ebp+disp8]  (reg: 0=eax 1=ecx 7=edi)
    void mov_reg_ebp(u8 reg, u8 disp) { b({0x8B, static_cast<u8>(0x45 | (reg << 3)), disp}); }
    void cmp_esi_edi() { b({0x39, 0xFE}); }
    void mov_eax_edi() { b({0x89, 0xF8}); }
    void sub_eax_esi() { b({0x29, 0xF0}); }
    void shr_eax_1()   { b({0xD1, 0xE8}); }
    void add_eax_esi() { b({0x01, 0xF0}); }
    void mov_local_eax(u8 disp) { b({0x89, 0x45, disp}); }
    void mul_ebp_disp(u8 disp) { b({0xF7, 0x65, disp}); }
    void add_eax_ebp_disp(u8 disp) { b({0x03, 0x45, disp}); }
    void mov_ebx_eax() { b({0x89, 0xC3}); }
    void push_ebp_disp(u8 disp) { b({0xFF, 0x75, disp}); }
    void call_ebp_disp(u8 disp) { b({0xFF, 0x55, disp}); }
    void test_eax_eax() { b({0x85, 0xC0}); }
    void lea_esi_ecx1() { b({0x8D, 0x71, 0x01}); }
    void mov_edi_ecx() { b({0x89, 0xCF}); }
    void mov_eax_ebx() { b({0x89, 0xD8}); }
    usize jae_f() { b({0x73, 0x00}); return code.size() - 1; }
    usize js_f()  { b({0x78, 0x00}); return code.size() - 1; }
    /// jmp backwards to an already-emitted label.
    void jmp_back(usize target) {
        b({0xEB, 0x00});
        const isize delta = static_cast<isize>(target) -
                            static_cast<isize>(code.size());
        code.back() = static_cast<u8>(delta);
    }
    /// lock cmpxchg [ecx+off], edx  -- EAX is the comparand
    void lock_cmpxchg_ecx_off_edx(u8 off) { b({0xF0, 0x0F, 0xB1, 0x51, off}); }
    /// lock xchg [ecx+off], eax -- a store that is also a full barrier.
    void lock_xchg_ecx_off_eax(u8 off) { b({0xF0, 0x87, 0x41, off}); }
    void ret_4() { b({0xC2, 0x04, 0x00}); }
    usize je() { b({0x74, 0x00}); return code.size() - 1; }
    usize jne() { b({0x75, 0x00}); return code.size() - 1; }
    usize jg() { b({0x7F, 0x00}); return code.size() - 1; }
    usize jle() { b({0x7E, 0x00}); return code.size() - 1; }
    /// jmp rel32 to a target patched later; returns the rel32 byte offset.
    usize jmp_extern() { b({0xE9, 0, 0, 0, 0}); return code.size() - 4; }

    /// Forward jumps, patched once the target is known.
    usize jbe() { b({0x76, 0x00}); return code.size() - 1; }
    usize jae() { b({0x73, 0x00}); return code.size() - 1; }
    usize jmp8() { b({0xEB, 0x00}); return code.size() - 1; }
    void patch(usize slot) {
        const isize delta = static_cast<isize>(code.size()) -
                            static_cast<isize>(slot) - 1;
        code[slot] = static_cast<u8>(delta);
    }
    void jne_back(usize target) {
        b({0x75, 0x00});
        const isize delta = static_cast<isize>(target) -
                            static_cast<isize>(code.size());
        code.back() = static_cast<u8>(delta);
    }
};

/// memset(dst, c, n) -> dst
void emit_memset(Asm& a) {
    a.push_edi();                 // callee-saved
    a.mov_reg_esp(7, 0x08);       // edi = dst   (args shifted by the push)
    a.mov_reg_esp(0, 0x0C);       // eax = c     (stosb uses AL)
    a.mov_reg_esp(1, 0x10);       // ecx = n
    a.push_edi();                 // keep dst for the return value
    a.rep_stosb();
    a.pop_eax();                  // eax = dst
    a.pop_edi();
    a.ret();
}

/// memcpy(dst, src, n) -> dst. Callers must not overlap; that is memmove.
void emit_memcpy(Asm& a) {
    a.push_esi();
    a.push_edi();
    a.mov_reg_esp(7, 0x0C);       // edi = dst
    a.mov_reg_esp(6, 0x10);       // esi = src
    a.mov_reg_esp(1, 0x14);       // ecx = n
    a.push_edi();
    a.rep_movsb();
    a.pop_eax();
    a.pop_edi();
    a.pop_esi();
    a.ret();
}

/// memmove(dst, src, n) -> dst. Copies backwards only when the regions
/// actually overlap in the dangerous direction, so the common case keeps the
/// forward path and leaves DF alone.
void emit_memmove(Asm& a) {
    a.push_esi();
    a.push_edi();
    a.mov_reg_esp(7, 0x0C);       // edi = dst
    a.mov_reg_esp(6, 0x10);       // esi = src
    a.mov_reg_esp(1, 0x14);       // ecx = n
    a.push_edi();

    a.cmp_edi_esi();
    const usize to_fwd_a = a.jbe();      // dst <= src: forward is safe
    a.lea_eax_esi_ecx();                 // eax = src + n
    a.cmp_edi_eax();
    const usize to_fwd_b = a.jae();      // dst >= src + n: disjoint

    // Overlapping with dst inside (src, src+n): copy tail-first.
    a.lea_esi_esi_ecx_m1();
    a.lea_edi_edi_ecx_m1();
    a.std_();
    a.rep_movsb();
    a.cld();                             // restore the ABI's DF=0
    const usize to_done = a.jmp8();

    a.patch(to_fwd_a);
    a.patch(to_fwd_b);
    a.rep_movsb();

    a.patch(to_done);
    a.pop_eax();
    a.pop_edi();
    a.pop_esi();
    a.ret();
}

/// strlen(s) -> length
void emit_strlen(Asm& a) {
    a.push_edi();
    a.mov_reg_esp(7, 0x08);       // edi = s
    a.xor_eax_eax();              // scan for AL = 0
    a.mov_ecx_imm32(0xFFFFFFFFu);
    a.repne_scasb();
    a.not_ecx();                  // ecx = scanned count
    a.dec_ecx();                  // minus the terminator
    a.mov_eax_ecx();
    a.pop_edi();
    a.ret();
}

/// fgetc(FILE*) -- cdecl, caller pops.
///
/// The ordinary path is the public MSVCRT _iobuf algorithm, protected by the
/// same guest-visible per-stream lock used by ungetc and the host refill path.
/// An empty buffer tail-jumps to the real CRT refill export while retaining
/// the lock; that handler fills a complete block and releases it.  Synthetic
/// FILE objects without Vitreo's private sidecar magic stay on the ordinary
/// host implementation.
void emit_fgetc_for(Asm& a, crt_stdio_buffer::Layout layout,
                    const char* refill_target, const char* slow_target) {
    using namespace crt_stdio_buffer;
    const auto public_cells = cells(layout);

    a.b({0x8B, 0x4C, 0x24, 0x04});       // mov ecx,[esp+4]  (stream)
    a.b({0x81, 0x79, static_cast<u8>(kMagic)});
    a.u32le(kMagicValue);                 // cmp [ecx+magic],VFIL
    const usize to_slow = a.jne();
    a.b({0x64, 0x8B, 0x15, 0x24, 0, 0, 0}); // mov edx,fs:[TEB.UniqueThread]

    const usize acquire = a.here();
    a.xor_eax_eax();
    a.b({0xF0, 0x0F, 0xB1, 0x51, static_cast<u8>(kLock)});
    a.jne_back(acquire);                  // spin only across the tiny FILE op

    a.b({0x8B, 0x41, static_cast<u8>(public_cells.count)}); // eax = _cnt
    a.b({0x83, 0xF8, 0x00});                  // cmp eax,0
    const usize to_refill = a.jle();
    a.b({0x83, 0xE8, 0x01});                  // --eax
    a.b({0x89, 0x41, static_cast<u8>(public_cells.count)}); // _cnt = eax
    a.b({0x8B, 0x51, static_cast<u8>(public_cells.ptr)}); // edx = _ptr
    a.b({0x0F, 0xB6, 0x02});              // eax = *(unsigned char*)edx
    a.b({0x83, 0xC2, 0x01});              // ++edx
    a.b({0x89, 0x51, static_cast<u8>(public_cells.ptr)}); // _ptr = edx
    a.b({0xC7, 0x41, static_cast<u8>(kHasRead)}); a.u32le(1);
    a.b({0xC7, 0x41, static_cast<u8>(kPushback)}); a.u32le(0);
    a.xor_edx_edx();
    a.b({0xF0, 0x87, 0x51, static_cast<u8>(kLock)}); // release, full barrier
    a.ret_c3();

    a.patch(to_refill);
    a.mark_extern(refill_target);
    a.patch(to_slow);
    a.mark_extern(slow_target);
}

void emit_fgetc(Asm& a) {
    emit_fgetc_for(a, crt_stdio_buffer::Layout::LegacyIobuf32,
                   "msvcrt.dll!__vitreo_fgetc_refill",
                   "msvcrt.dll!__vitreo_fgetc_slow");
}

void emit_ucrt_fgetc(Asm& a) {
    emit_fgetc_for(a, crt_stdio_buffer::Layout::UcrtOpaque32,
                   "ucrtbase.dll!__vitreo_fgetc_refill",
                   "ucrtbase.dll!__vitreo_fgetc_slow");
}

/// ungetc(int, FILE*) -- cdecl, caller pops.
void emit_ungetc_for(Asm& a, crt_stdio_buffer::Layout layout,
                     const char* slow_target) {
    using namespace crt_stdio_buffer;
    const auto public_cells = cells(layout);

    a.b({0x8B, 0x4C, 0x24, 0x08});       // ecx = stream
    a.b({0x81, 0x79, static_cast<u8>(kMagic)}); a.u32le(kMagicValue);
    const usize to_slow = a.jne();
    a.b({0x8B, 0x44, 0x24, 0x04});       // eax = c
    a.b({0x83, 0xF8, 0xFF});              // EOF cannot be pushed back
    const usize eof_no_lock = a.je();
    a.b({0x64, 0x8B, 0x15, 0x24, 0, 0, 0});

    const usize acquire = a.here();
    a.xor_eax_eax();
    a.b({0xF0, 0x0F, 0xB1, 0x51, static_cast<u8>(kLock)});
    a.jne_back(acquire);

    a.b({0x83, 0x79, static_cast<u8>(kHasRead), 0x00});
    const usize fail_no_read = a.je();
    a.b({0x83, 0x79, static_cast<u8>(kPushback), 0x00});
    const usize fail_double = a.jne();
    a.b({0x8B, 0x51, static_cast<u8>(public_cells.ptr)}); // edx = _ptr
    a.b({0x3B, 0x51, static_cast<u8>(public_cells.base)}); // ptr <= base
    const usize fail_no_room = a.jbe();
    a.b({0x4A});                          // --edx
    a.b({0x8B, 0x44, 0x24, 0x04});       // eax = c
    a.b({0x88, 0x02});                    // *edx = (unsigned char)c
    a.b({0x89, 0x51, static_cast<u8>(public_cells.ptr)}); // _ptr = edx
    a.b({0xFF, 0x41, static_cast<u8>(public_cells.count)}); // ++_cnt
    a.b({0xC7, 0x41, static_cast<u8>(kPushback)}); a.u32le(1);
    a.b({0xC7, 0x41, static_cast<u8>(kEof)}); a.u32le(0);
    a.b({0x25, 0xFF, 0x00, 0x00, 0x00}); // return unsigned-char value
    a.xor_edx_edx();
    a.b({0xF0, 0x87, 0x51, static_cast<u8>(kLock)});
    a.ret_c3();

    a.patch(fail_no_read);
    a.patch(fail_double);
    a.patch(fail_no_room);
    a.mov_eax_imm32(0xFFFFFFFFu);
    a.xor_edx_edx();
    a.b({0xF0, 0x87, 0x51, static_cast<u8>(kLock)});
    a.ret_c3();

    a.patch(eof_no_lock);
    a.mov_eax_imm32(0xFFFFFFFFu);
    a.ret_c3();

    a.patch(to_slow);
    a.mark_extern(slow_target);
}

void emit_ungetc(Asm& a) {
    emit_ungetc_for(a, crt_stdio_buffer::Layout::LegacyIobuf32,
                    "msvcrt.dll!__vitreo_ungetc_slow");
}

void emit_ucrt_ungetc(Asm& a) {
    emit_ungetc_for(a, crt_stdio_buffer::Layout::UcrtOpaque32,
                    "ucrtbase.dll!__vitreo_ungetc_slow");
}

// RTL_CRITICAL_SECTION, 32-bit: DebugInfo +0, LockCount +4, RecursionCount +8,
// OwningThread +0xC, LockSemaphore +0x10, SpinCount +0x14.
//
// LockCount uses the Server 2003 SP1+ encoding -- -1 unlocked, -2 locked with
// no waiters -- because the host half in critical_section.cpp contends for this
// very word. Waiter bits (-2-4*n) are published only by the host slow path.
// LockSemaphore stays NULL until first contention (then (HANDLE)-1); it is
// never a waiter counter.
constexpr u8 kCsLockCount = 0x04;
constexpr u8 kCsRecursion = 0x08;
constexpr u8 kCsOwner     = 0x0C;
constexpr u32 kCsUnlocked = 0xFFFFFFFFu;   // -1
constexpr u32 kCsLocked   = 0xFFFFFFFEu;   // -2

/// EnterCriticalSection(cs) -- stdcall, callee pops 4.
void emit_enter_critical_section(Asm& a) {
    a.mov_ecx_arg0();                       // ecx = cs
    a.mov_eax_teb_tid();                    // eax = current tid
    a.cmp_ecx_off_eax(kCsOwner);            // already ours?
    const usize to_recursive = a.je();
    // Uncontended acquire: -1 -> -2 (SP1+).
    a.mov_edx_imm32(kCsLocked);
    a.mov_eax_imm32(kCsUnlocked);
    a.lock_cmpxchg_ecx_off_edx(kCsLockCount);
    const usize to_slow = a.jne();
    a.mov_eax_teb_tid();                    // cmpxchg clobbered eax
    a.mov_ecx_off_eax(kCsOwner);
    a.mov_ecx_off_imm32(kCsRecursion, 1u);
    a.ret_4();

    a.patch(to_recursive);
    // Reading OwningThread unsynchronised is safe only because a releaser
    // clears it BEFORE unlocking (see emit_leave_critical_section): a stale
    // read can therefore never match a thread that does not hold the section.
    a.inc_ecx_off(kCsRecursion);
    a.ret_4();

    a.patch(to_slow);
    a.mark_extern("kernel32.dll!__vitreo_cs_enter_slow");
}

/// LeaveCriticalSection(cs) -- stdcall, callee pops 4.
void emit_leave_critical_section(Asm& a) {
    a.mov_ecx_arg0();
    a.mov_eax_ecx_off(kCsRecursion);
    a.cmp_eax_imm8(1);
    const usize to_nested = a.jg();
    a.mov_ecx_off_imm32(kCsRecursion, 0u);
    a.mov_ecx_off_imm32(kCsOwner, 0u);      // clear owner BEFORE releasing
    // Learn LeaveCriticalSection: do not access the CRITICAL_SECTION after
    // ownership is released. Unlock with a full barrier, then jmp to the host
    // leave_wake helper which inspects host-only registered_waiters (and pops
    // the stdcall arg on return).
    a.mov_eax_imm32(kCsUnlocked);
    a.lock_xchg_ecx_off_eax(kCsLockCount);
    a.mark_extern("kernel32.dll!__vitreo_cs_leave_wake");

    a.patch(to_nested);
    a.dec_ecx_off(kCsRecursion);
    a.ret_4();
}

// MSVC 32-bit jmp_buf: EBP+0 EBX+4 EDI+8 ESI+12 ESP+16 EIP+20
// Registration+24 TryLevel+28. Mirrors JmpBuf32 in crt_init.cpp -- the host
// longjmp reads these exact slots, so the two must not drift.
constexpr u8 kJbEbp = 0, kJbEbx = 4, kJbEdi = 8, kJbEsi = 12;
constexpr u8 kJbEsp = 16, kJbEip = 20, kJbReg = 24, kJbTry = 28;

/// _setjmp / setjmp / _setjmp3 -- cdecl, caller pops, returns 0.
///
/// The host implementation is a pure save of callee-saved registers, the
/// resume point and the SEH chain head into the caller's jmp_buf: no host
/// state, no side effects outside guest memory. At 8.9M calls on one load
/// (4.85 CPU-seconds, second only to the critical sections) the transition
/// costs far more than the dozen stores it guards.
void emit_setjmp(Asm& a) {
    a.mov_edx_arg0();                   // edx = jmp_buf
    a.mov_edx_off_ebp(kJbEbp);
    a.mov_edx_off_ebx(kJbEbx);
    a.mov_edx_off_edi(kJbEdi);
    a.mov_edx_off_esi(kJbEsi);
    a.lea_eax_esp4();                   // resumed esp is just past the retaddr
    a.mov_edx_off_eax(kJbEsp);
    a.mov_eax_at_esp();                 // retaddr = where setjmp returns to
    a.mov_edx_off_eax(kJbEip);
    a.mov_eax_fs0();                    // SEH chain head, so longjmp out of a
    a.mov_edx_off_eax(kJbReg);          // __try does not leave fs:[0] dangling
    a.mov_edx_off_imm32(kJbTry, 0xFFFFFFFFu);
    a.xor_eax_eax();                    // setjmp returns 0 on the direct path
    a.ret_c3();                         // cdecl: caller pops
}

/// bsearch(key, base, num, width, compare) -- cdecl, caller pops.
///
/// Worth moving even though it is called only ~280k times: each call performs
/// log2(n) comparisons and every one of those is a CALLBACK into guest code.
/// As a host thunk that is a host->guest transition per comparison, which is
/// why it measured 6.7us/call -- ten times the other primitives. Run
/// guest-side, the callbacks are ordinary guest calls and cost nothing.
///
/// Frame: [ebp+8]=key [ebp+12]=base [ebp+16]=num [ebp+20]=width
///        [ebp+24]=compare, with mid spilled to [ebp-4] because the comparator
///        is cdecl and may clobber every caller-saved register.
void emit_bsearch(Asm& a) {
    constexpr u8 kKey = 0x08, kBase = 0x0C, kNum = 0x10;
    constexpr u8 kWidth = 0x14, kCompare = 0x18, kMid = 0xFC;  // -4

    a.push_ebp();
    a.mov_ebp_esp();
    a.sub_esp_imm8(4);          // spill slot for mid
    a.push_ebx();
    a.push_esi();
    a.push_edi();

    a.xor_esi_esi();            // lo = 0
    a.mov_reg_ebp(7, kNum);     // edi = hi = num

    const usize loop_top = a.here();
    a.cmp_esi_edi();
    const usize to_not_found = a.jae_f();

    a.mov_eax_edi();            // mid = lo + (hi - lo) / 2, overflow-free
    a.sub_eax_esi();
    a.shr_eax_1();
    a.add_eax_esi();
    a.mov_local_eax(kMid);

    a.mul_ebp_disp(kWidth);     // eax = mid * width
    a.add_eax_ebp_disp(kBase);  // eax = base + mid * width
    a.mov_ebx_eax();            // ebx = element (callee-saved across the call)

    a.push_ebx();               // compare(key, element)
    a.push_ebp_disp(kKey);
    a.call_ebp_disp(kCompare);
    a.add_esp_imm8(8);          // cdecl: caller pops

    a.test_eax_eax();
    const usize to_found = a.je();
    a.mov_reg_ebp(1, kMid);     // ecx = mid; MOV leaves the flags from TEST
    const usize to_left = a.js_f();
    a.lea_esi_ecx1();           // key is greater: lo = mid + 1
    a.jmp_back(loop_top);

    a.patch(to_left);
    a.mov_edi_ecx();            // key is smaller: hi = mid
    a.jmp_back(loop_top);

    a.patch(to_found);
    a.mov_eax_ebx();
    const usize to_done = a.jmp8();

    a.patch(to_not_found);
    a.xor_eax_eax();            // no match: NULL

    a.patch(to_done);
    a.pop_edi();
    a.pop_esi();
    a.pop_ebx();
    a.mov_esp_ebp();
    a.pop_ebp();
    a.ret_c3();
}

struct Accel {
    const char* name;
    void (*emit)(Asm&);
};

// Deliberately conservative: pure, locale-independent primitives whose
// semantics are fixed by the C standard. Growing this list is cheap; getting a
// subtly wrong implementation into the guest is not.
constexpr Accel kAccelerated[] = {
    {"msvcrt.dll!memset",  emit_memset},
    {"msvcrt.dll!memcpy",  emit_memcpy},
    {"msvcrt.dll!memmove", emit_memmove},
    {"msvcrt.dll!strlen",  emit_strlen},
    {"msvcrt.dll!fgetc",   emit_fgetc},
    {"msvcrt.dll!ungetc",  emit_ungetc},
    {"msvcrt.dll!_setjmp",  emit_setjmp},
    {"msvcrt.dll!setjmp",   emit_setjmp},
    {"msvcrt.dll!_setjmp3", emit_setjmp},
    {"msvcrt.dll!bsearch",  emit_bsearch},
    {"ucrtbase.dll!memset",  emit_memset},
    {"ucrtbase.dll!memcpy",  emit_memcpy},
    {"ucrtbase.dll!memmove", emit_memmove},
    {"ucrtbase.dll!strlen",  emit_strlen},
    {"ucrtbase.dll!fgetc",   emit_ucrt_fgetc},
    {"ucrtbase.dll!ungetc",  emit_ucrt_ungetc},
    {"ucrtbase.dll!_setjmp",  emit_setjmp},
    {"ucrtbase.dll!setjmp",   emit_setjmp},
    {"ucrtbase.dll!_setjmp3", emit_setjmp},
    {"ucrtbase.dll!bsearch",  emit_bsearch},
};

// Gated separately: these change the authority for every critical section in
// the process from a host mutex to the guest's own LockCount word, and the
// host half is itself opt-in (VITREO_CS_ACCEL). Both halves must agree, so
// they share one switch.
constexpr Accel kCsAccelerated[] = {
    {"kernel32.dll!EnterCriticalSection", emit_enter_critical_section},
    {"kernel32.dll!LeaveCriticalSection", emit_leave_critical_section},
};

bool g_arena_ready = false;

} // namespace

CrtAccelBlob build_guest_crt_code(GuestAddr base, bool include_critical_sections) {
    Asm asmbuf;
    CrtAccelBlob blob;
    for (const auto& fn : kAccelerated) {
        blob.entries.emplace_back(fn.name,
                                  static_cast<u32>(base + asmbuf.here()));
        fn.emit(asmbuf);
    }
    if (include_critical_sections) {
        for (const auto& fn : kCsAccelerated) {
            blob.entries.emplace_back(fn.name,
                                      static_cast<u32>(base + asmbuf.here()));
            fn.emit(asmbuf);
        }
    }
    blob.code = std::move(asmbuf.code);
    blob.extern_fixups = std::move(asmbuf.fixups);
    return blob;
}

usize install_guest_crt_accelerators(GuestMemory& memory,
                                     ThunkRegistry& registry) noexcept {
    static const bool cs_accel = std::getenv("VITREO_NO_CS_ACCEL") == nullptr;
    CrtAccelBlob blob = build_guest_crt_code(CRT_ACCEL_BASE_X86, cs_accel);
    const auto& entry = blob.entries;
    if (blob.code.size() > CRT_ACCEL_SIZE) return 0;

    // Resolve every tail-jump target before the code is written: a fast path
    // whose slow branch cannot be reached is worse than no fast path at all,
    // so a missing export disables the whole install rather than shipping a
    // jump into nothing.
    auto split = [](const std::string& q) {
        const auto bang = q.find('!');
        return std::make_pair(q.substr(0, bang), q.substr(bang + 1));
    };
    for (const auto& [slot, name] : blob.extern_fixups) {
        const auto [dll, fn] = split(name);
        auto target = registry.get_thunk_address(dll, fn, true);
        if (target.is_err()) return 0;
        const u32 site = static_cast<u32>(CRT_ACCEL_BASE_X86 + slot);
        const u32 rel = static_cast<u32>(target.value()) - (site + 4u);
        std::memcpy(blob.code.data() + slot, &rel, 4);
    }

    // The arena is built once, but the sled patches are re-applied on every
    // call. native32_prepare rewrites EVERY slot in the thunk arena with its
    // own entry stub, which erases the jumps installed earlier from run.cpp;
    // that path therefore has to call this again afterwards and have it take
    // effect. Guarding the whole function would silently leave those exports
    // pointing back at the host thunks.
    if (!g_arena_ready) {
        if (memory.map_region(CRT_ACCEL_BASE_X86, CRT_ACCEL_SIZE,
                              MemoryProtection::ReadWrite).is_err()) {
            return 0;
        }
        if (memory.write_bytes(CRT_ACCEL_BASE_X86, blob.code.data(),
                               blob.code.size()).is_err()) {
            return 0;
        }
        (void)memory.protect_region(CRT_ACCEL_BASE_X86, CRT_ACCEL_SIZE,
                                    MemoryProtection::ReadExecute);
        g_arena_ready = true;
    }

    // Overwrite each export's entry sled with a jump to its implementation.
    // The sled is STUB_SIZE bytes and a rel32 jump needs five, so it fits.
    usize redirected = 0;
    for (const auto& [name, impl] : entry) {
        const auto [dll, fn] = split(name);
        auto slot = registry.get_thunk_address(dll, fn);
        if (slot.is_err()) continue;
        const GuestAddr sled = slot.value();

        u8 jump[5];
        jump[0] = 0xE9;                                  // jmp rel32
        const u32 rel = impl - (static_cast<u32>(sled) + 5u);
        std::memcpy(jump + 1, &rel, 4);

        if (memory.protect_region(sled & ~static_cast<GuestAddr>(0xFFF), 0x1000,
                                  MemoryProtection::ReadWrite).is_err()) {
            continue;
        }
        const bool ok = memory.write_bytes(sled, jump, sizeof(jump)).is_ok();
        (void)memory.protect_region(sled & ~static_cast<GuestAddr>(0xFFF), 0x1000,
                                    MemoryProtection::ReadExecute);
        if (ok) {
            ++redirected;
            // The sled is guest code now: never an in-chain call (L3-K).
            registry.withdraw_in_chain(sled);
        }
    }

    if (std::getenv("VITREO_LOG_CRT_ACCEL")) {
        std::fprintf(stderr,
                     "[crt-accel] %zu/%zu primitives now run guest-side "
                     "(%zu bytes at 0x%08x)\n",
                     redirected, entry.size(), blob.code.size(),
                     static_cast<unsigned>(CRT_ACCEL_BASE_X86));
    }
    return redirected;
}

} // namespace vitreo::winapi
