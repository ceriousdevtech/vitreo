// SPDX-License-Identifier: LGPL-2.1-only

/// @file compat32.cpp
/// @brief Native 32-bit guest execution. See compat32.hpp for the hardware
///        contract this implements.

#include "core/cpu/x64/compat32.hpp"
#include "core/cpu/x64/x64_exception_bridge.hpp"
#include "core/cpu/x64/x64_segments.hpp"

#include <cerrno>
#include <cstring>
#include <mutex>
#include <new>
#include <pthread.h>
#include <sys/mman.h>

namespace vitreo::cpu::x64 {

namespace {

// Trampoline page layout. Fixed offsets because the stubs address these slots
// absolutely: at the moment of transfer every general register belongs to the
// guest, so no stub can afford a base register.
constexpr u32 kPageSize = 0x1000;

constexpr u32 kOffEntry32   = 0x000;   // 32-bit: load guest regs, jump to EIP
constexpr u32 kOffExit32    = 0x080;   // 32-bit: capture regs, leave 32-bit
constexpr u32 kOffSentinel  = 0x0C0;   // 32-bit: guest returns here -> exit32
constexpr u32 kOffRet64     = 0x100;   // 64-bit: restore host, resume
constexpr u32 kOffRegs      = 0x200;   // Compat32State mirror
constexpr u32 kOffFarEntry  = 0x300;   // m16:32 far pointer to entry32
constexpr u32 kOffResume    = 0x320;   // 8 bytes: 64-bit resume address
constexpr u32 kOffHostRsp   = 0x328;   // 8 bytes: full 64-bit host RSP
constexpr u32 kOffHostDs    = 0x330;   // 2 bytes: host DS selector
constexpr u32 kOffHostEs    = 0x332;   // 2 bytes: host ES selector
constexpr u32 kOffHostFs    = 0x348;   // 2 bytes: host FS selector
constexpr u32 kOffGuestDs   = 0x34A;   // 2 bytes: guest 32-bit data selector
constexpr u32 kOffGuestFs   = 0x34C;   // 2 bytes: guest TEB selector (0 = none)
constexpr u32 kOffReason    = 0x334;   // 4 bytes: why 32-bit mode was left
constexpr u32 kOffThunkTag  = 0x338;   // 4 bytes: return addr pushed by a stub
constexpr u32 kOffCollect32 = 0x140;   // 32-bit: thunk stub landing pad
constexpr u32 kOffResume32  = 0x1C0;   // 64-bit: far-jump back into entry32

// Values written into kOffReason by the two exit paths.
constexpr u32 kReasonSentinel = 0;
constexpr u32 kReasonThunk    = 1;
constexpr u32 kReasonFault    = 2;   // written by the exception bridge, not a stub

// Fault detail, filled by the bridge rather than by guest-mode code.
constexpr u32 kOffFaultStatus = 0x33C;   // NTSTATUS
constexpr u32 kOffFaultAddr   = 0x340;   // faulting linear address

// 512-byte FXSAVE image, 16-byte aligned. The hand-off used to carry GPRs,
// EFLAGS and EIP only; the guest's x87/MMX/SSE state stayed in the hardware
// registers across every exit to the host, where any host code -- memcpy,
// the Mach exception bridge, an interpreter helper -- clobbers it (SIMD is
// caller-saved in the host ABI). A codec inner loop that faults (a 32-bit
// title's GC-armed write-watch fires on every decoded output page) resumed
// with scrambled MM/XMM registers and computed garbage with no fault: garbled
// video, burst-then-silence audio, and corrupted decompressed manifests
// feeding VirtualAlloc. A real context
// switch preserves the full FP state; ret64 FXSAVEs it, entry paths FXRSTOR
// it, and the exception bridge captures x86_FLOAT_STATE64 into this slot so
// a faulting thread's state survives the kernel round-trip too.
constexpr u32 kOffFxSave = 0x400;
constexpr u32 kOffFaultWrite  = 0x344;   // 1 = store, 0 = load

// Register slot offsets within kOffRegs, in Compat32State field order.
constexpr u32 kSlotEax = kOffRegs + 0x00;
constexpr u32 kSlotEcx = kOffRegs + 0x04;
constexpr u32 kSlotEdx = kOffRegs + 0x08;
constexpr u32 kSlotEbx = kOffRegs + 0x0C;
constexpr u32 kSlotEsp = kOffRegs + 0x10;
constexpr u32 kSlotEbp = kOffRegs + 0x14;
constexpr u32 kSlotEsi = kOffRegs + 0x18;
constexpr u32 kSlotEdi = kOffRegs + 0x1C;
constexpr u32 kSlotEip = kOffRegs + 0x20;
constexpr u32 kSlotFlg = kOffRegs + 0x24;

/// Small emitter. The stubs are short and fixed, so a byte writer with named
/// helpers is clearer than pulling in an assembler.
struct Emitter {
    u8* page;
    u32 pos;

    void u8b(u8 v)  { page[pos++] = v; }
    void u16b(u16 v) { std::memcpy(page + pos, &v, 2); pos += 2; }
    void u32b(u32 v) { std::memcpy(page + pos, &v, 4); pos += 4; }

    // --- 32-bit forms, absolute [disp32] addressing (no base register) ---

    /// mov r32, [abs32]
    void load_reg_abs(u8 reg, u32 addr) {
        if (reg == 0) { u8b(0xA1); u32b(addr); return; }   // mov eax, [moffs]
        u8b(0x8B);                                          // mov r32, r/m32
        u8b(static_cast<u8>(0x05 | (reg << 3)));            // mod=00 rm=101
        u32b(addr);
    }

    /// mov [abs32], r32
    void store_reg_abs(u8 reg, u32 addr) {
        if (reg == 0) { u8b(0xA3); u32b(addr); return; }   // mov [moffs], eax
        u8b(0x89);                                          // mov r/m32, r32
        u8b(static_cast<u8>(0x05 | (reg << 3)));
        u32b(addr);
    }

    void push_abs(u32 addr)  { u8b(0xFF); u8b(0x35); u32b(addr); }  // push [abs]
    void pop_abs(u32 addr)   { u8b(0x8F); u8b(0x05); u32b(addr); }  // pop  [abs]
    void pushfd()            { u8b(0x9C); }
    void popfd()             { u8b(0x9D); }
    void jmp_indirect_abs(u32 addr) { u8b(0xFF); u8b(0x25); u32b(addr); }

    /// ljmp ptr16:32 -- valid in 32-bit mode only; how we leave for 64-bit.
    void ljmp_far_direct(u32 offset, u16 sel) {
        u8b(0xEA); u32b(offset); u16b(sel);
    }

    /// mov ds/es/fs, r16 (from a GPR). Sreg encoding: ES=0,CS=1,SS=2,DS=3,
    /// FS=4, GS=5.
    void mov_sreg_from_reg(u8 sreg, u8 gpr) {
        u8b(0x8E);
        u8b(static_cast<u8>(0xC0 | (sreg << 3) | gpr));
    }

    // --- 64-bit forms, [rip + disp32] addressing ---
    //
    // The 64-bit stub CANNOT use the absolute [disp32] form the 32-bit stubs
    // use. In 64-bit mode a disp32 is SIGN-extended, so the moment a
    // trampoline sits at or above 0x80000000 every absolute reference lands
    // at 0xFFFFFFFF_xxxxxxxx. Measured: a pooled page at 0xF0002000 faulted in
    // ret64 reading its saved host RSP at 0xFFFFFFFF_F0002328 -- and it
    // faulted *after* the mode switch, with no usable stack and no signal
    // handler, so the report blamed the trampoline entry rather than the
    // operand. RIP-relative is immune and costs nothing: every slot is in the
    // same page as the instruction referencing it.

    /// Emit the disp32 of a [rip + disp32] operand targeting @p target_off.
    /// Call immediately after the ModRM byte; the displacement is measured
    /// from the end of the instruction, which is these four bytes.
    void rip_rel(u32 target_off) {
        u32b(static_cast<u32>(static_cast<i32>(target_off) -
                              static_cast<i32>(pos + 4)));
    }
};

// 32-bit GPR encodings.
constexpr u8 kEAX = 0, kECX = 1, kEDX = 2, kEBX = 3;
constexpr u8 kESP = 4, kEBP = 5, kESI = 6, kEDI = 7;

// ---------------------------------------------------------------------------
// Trampoline page pool
// ---------------------------------------------------------------------------
//
// Every guest thread needs its own trampoline, and every trampoline must sit
// below 4GB because the stubs reach their slots by 32-bit absolute address.
// That makes low address space the scarce resource, so the pages come from one
// reserved extent rather than being scattered by individual mmap calls.
//
// The extent is placed near the top of the 32-bit range on purpose. A Win32
// guest owns only the low 2GB; above `0x80000000` is kernel space it will
// never map, so trampolines cannot collide with a guest image, heap or stack,
// while still being addressable by a flat 32-bit segment (base 0, 4GB limit).
//
// That placement is only safe because the 64-bit half of the trampoline is
// RIP-relative. An absolute [disp32] operand is sign-extended in 64-bit mode,
// which silently confines trampolines to the low 2GB -- exactly the range the
// guest wants. See Emitter::rip_rel.
constexpr u32   kPoolHint  = 0xF0000000;
constexpr usize kPoolPages = 1024;                        // 4MB of address space
constexpr usize kPoolBytes = kPoolPages * kPageSize;

/// Slot state. "Unusable" exists because a slot whose commit fails must not be
/// retried forever, and must not be handed out either.
enum class Slot : u8 { Free = 0, InUse = 1, Unusable = 2 };

std::mutex g_pool_mutex;
u32        g_pool_base   = 0;       // 0 until reserved
bool       g_pool_failed = false;
usize      g_pool_in_use = 0;
Slot       g_pool_slot[kPoolPages] = {};

/// Reserve the extent once. Caller holds g_pool_mutex.
///
/// Deliberately NOT MAP_FIXED: a fixed mapping would silently evict whoever
/// already owns those pages, and there is no MAP_EXCL on this platform. Given
/// a hint and no MAP_FIXED, the kernel searches upward for genuinely free
/// space -- which is what we want -- but it may also land above 4GB, where the
/// reservation would be useless, so the result is checked rather than trusted.
bool reserve_pool_locked() {
    if (g_pool_base != 0) return true;
    if (g_pool_failed)    return false;

    auto accept = [&](void* r) -> bool {
        const uintptr_t addr = reinterpret_cast<uintptr_t>(r);
        if (addr + kPoolBytes > 0x100000000ULL) return false;
        g_pool_base = static_cast<u32>(addr);
        return true;
    };

    // Prefer a real free extent so we do not evict a mapping we do not own.
    // Under identity the whole 4GB is already our PROT_NONE reservation, so
    // this lands above 4GB and we fall through to MAP_FIXED at the hint.
    void* r = mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(kPoolHint)),
                   kPoolBytes, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (r != MAP_FAILED) {
        if (accept(r)) return true;
        munmap(r, kPoolBytes);
    }

    r = mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(kPoolHint)),
             kPoolBytes, PROT_NONE,
             MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (r != MAP_FAILED && accept(r)) return true;

    // Almost always __PAGEZERO: a 64-bit Mach-O owns everything below 4GB
    // unless linked with -pagezero_size.
    g_pool_failed = true;
    return false;
}

/// Take a page from the pool, committed RWX. Returns 0 if none is available.
u32 pool_acquire() {
    std::lock_guard lk(g_pool_mutex);
    if (!reserve_pool_locked()) return 0;

    for (usize i = 0; i < kPoolPages; ++i) {
        if (g_pool_slot[i] != Slot::Free) continue;

        const u32 addr = g_pool_base + static_cast<u32>(i * kPageSize);
        // MAP_FIXED is safe here and only here: the slot is inside our own
        // reservation and the bookkeeping says nobody holds it.
        void* p = mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)),
                       kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (p == MAP_FAILED) {
            g_pool_slot[i] = Slot::Unusable;
            continue;
        }
        g_pool_slot[i] = Slot::InUse;
        ++g_pool_in_use;
        return addr;
    }
    return 0;
}

/// Give a page back. Re-mapping it PROT_NONE both restores the reservation and
/// drops the old stubs, so a stale executable page cannot be reached through
/// an address some guest code still holds.
void pool_release(u32 addr) {
    std::lock_guard lk(g_pool_mutex);
    if (g_pool_base == 0 || addr < g_pool_base) return;

    const usize i = (addr - g_pool_base) / kPageSize;
    if (i >= kPoolPages || g_pool_slot[i] != Slot::InUse) return;

    mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), kPageSize,
         PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    g_pool_slot[i] = Slot::Free;
    --g_pool_in_use;
}

u64 current_thread_id() noexcept {
    u64 tid = 0;
    pthread_threadid_np(nullptr, &tid);
    return tid;
}

/// Owns the calling thread's executor and releases it at thread exit, so a
/// pool page never outlives the thread that entered it.
struct ThreadExecutorSlot {
    Compat32Executor* exec = nullptr;
    ~ThreadExecutorSlot() { delete exec; exec = nullptr; }
};

thread_local ThreadExecutorSlot t_executor;

} // namespace

usize Compat32Executor::trampoline_size() noexcept { return kPageSize; }

void Compat32Executor::publish_guest_regs(const u32 gpr[8],
                                          u32 eflags) noexcept {
    if (m_page == nullptr) return;
    auto* regs = reinterpret_cast<u32*>(m_page + kOffRegs);
    for (int i = 0; i < 8; ++i) regs[i] = gpr[i];
    regs[9] = eflags;
}

void Compat32Executor::record_fault(u32 eip, u32 ntstatus, u32 address,
                                    u32 write) noexcept {
    if (m_page == nullptr) return;
    // The reason slot is normally written by a 32-bit stub. A fault never
    // reaches one -- the kernel will not resume into compatibility mode -- so
    // the bridge sets it directly from the handler thread.
    *reinterpret_cast<u32*>(m_page + kOffReason)      = kReasonFault;
    *reinterpret_cast<u32*>(m_page + kOffFaultStatus) = ntstatus;
    *reinterpret_cast<u32*>(m_page + kOffFaultAddr)   = address;
    *reinterpret_cast<u32*>(m_page + kOffFaultWrite)  = write;
    *reinterpret_cast<u32*>(m_page + kSlotEip)        = eip;
}

u32 Compat32Executor::last_fault_status() const noexcept {
    return m_page ? *reinterpret_cast<const u32*>(m_page + kOffFaultStatus) : 0;
}
u32 Compat32Executor::last_fault_address() const noexcept {
    return m_page ? *reinterpret_cast<const u32*>(m_page + kOffFaultAddr) : 0;
}
u32 Compat32Executor::last_fault_write() const noexcept {
    return m_page ? *reinterpret_cast<const u32*>(m_page + kOffFaultWrite) : 0;
}

void Compat32Executor::emit_exit_stub(u8* dest, u32 /*stub_addr*/) noexcept {
    // 64 FF 25 disp32   jmp dword ptr fs:[kTebNative32Sentinel]
    dest[0] = 0x64;
    dest[1] = 0xFF;
    dest[2] = 0x25;
    const u32 disp = kTebNative32Sentinel;
    std::memcpy(dest + 3, &disp, 4);
}

u32 Compat32Executor::resume32_address() const noexcept {
    return m_base + kOffResume32;
}

void Compat32Executor::publish_guest_state(const u32 gpr[8], u32 eflags,
                                           u32 eip) noexcept {
    if (m_page == nullptr) return;
    auto* regs = reinterpret_cast<u32*>(m_page + kOffRegs);
    for (int i = 0; i < 8; ++i) regs[i] = gpr[i];
    regs[8] = eip;
    regs[9] = eflags;
}

u32 Compat32Executor::ret64_address() const noexcept {
    return m_base + kOffRet64;
}

u32 Compat32Executor::last_thunk_address() const noexcept {
    if (m_page == nullptr) return 0;
    // The collector stored the stub's return address; the slot itself is one
    // call instruction earlier.
    const u32 tag = *reinterpret_cast<const u32*>(m_page + kOffThunkTag);
    return tag - static_cast<u32>(thunk_stub_size());
}

void Compat32Executor::emit_thunk_stub(u8* dest, u32 /*slot_addr*/) noexcept {
    // 64 FF 15 disp32   call dword ptr fs:[kTebNative32Collect32]
    dest[0] = 0x64;
    dest[1] = 0xFF;
    dest[2] = 0x15;
    const u32 disp = kTebNative32Collect32;
    std::memcpy(dest + 3, &disp, 4);
}

u32 Compat32Executor::sentinel_return_address() const noexcept {
    return m_base + kOffSentinel;
}

u32 Compat32Executor::collect32_address() const noexcept {
    return m_base + kOffCollect32;
}

void Compat32Executor::publish_fs_hooks(u32 teb_base) const noexcept {
    if (teb_base == 0 || m_page == nullptr) return;
    auto* teb = reinterpret_cast<u8*>(static_cast<uintptr_t>(teb_base));
    const u32 collect = m_base + kOffCollect32;
    const u32 sentinel = m_base + kOffSentinel;
    std::memcpy(teb + kTebNative32Collect32, &collect, 4);
    std::memcpy(teb + kTebNative32Sentinel, &sentinel, 4);
}

usize Compat32Executor::pooled_pages_in_use() noexcept {
    std::lock_guard lk(g_pool_mutex);
    return g_pool_in_use;
}

bool Compat32Executor::reserve_trampoline_pool() noexcept {
    std::lock_guard lk(g_pool_mutex);
    return reserve_pool_locked();
}

u32 Compat32Executor::trampoline_pool_base() noexcept {
    std::lock_guard lk(g_pool_mutex);
    return g_pool_base;
}

usize Compat32Executor::trampoline_pool_bytes() noexcept {
    return kPoolBytes;
}

Compat32Executor::~Compat32Executor() {
    if (m_page == nullptr) return;
    if (m_pooled) {
        pool_release(m_base);
    } else {
        munmap(m_page, kPageSize);
    }
    m_page = nullptr;
}

VResult<Compat32Executor*> Compat32Executor::for_current_thread() {
    if (t_executor.exec != nullptr) {
        return VResult<Compat32Executor*>::ok(t_executor.exec);
    }

    const u32 base = pool_acquire();
    if (base == 0) {
        return VResult<Compat32Executor*>::err(Error::OutOfMemory);
    }

    auto built = build_at(base, reinterpret_cast<u8*>(static_cast<uintptr_t>(base)),
                          /*pooled=*/true);
    if (built.is_err()) {
        pool_release(base);
        return built;
    }
    t_executor.exec = built.value();
    return built;
}

VResult<Compat32Executor*> Compat32Executor::create(u32 trampoline_hint) {
    if (trampoline_hint == 0 ||
        (trampoline_hint & (kPageSize - 1)) != 0) {
        return VResult<Compat32Executor*>::err(Error::InvalidArgument);
    }

    // MAP_FIXED without MAP_EXCL would silently replace an existing mapping.
    // Probe first so a collision surfaces as an error rather than as memory
    // quietly taken from whoever owned it.
    void* probe = mmap(reinterpret_cast<void*>(
                           static_cast<uintptr_t>(trampoline_hint)),
                       kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (probe == MAP_FAILED) {
        // Almost always __PAGEZERO: a 64-bit Mach-O owns everything below 4GB
        // unless linked with -pagezero_size.
        return VResult<Compat32Executor*>::err(Error::OutOfMemory);
    }

    auto built = build_at(trampoline_hint, static_cast<u8*>(probe),
                          /*pooled=*/false);
    if (built.is_err()) munmap(probe, kPageSize);
    return built;
}

VResult<Compat32Executor*> Compat32Executor::build_at(u32 base, u8* page,
                                                      bool pooled) {
    // The compatibility-mode selectors are process-wide and allocated once;
    // every guest thread enters through the same flat segments, so a thread
    // costs a page rather than a pair of finite LDT descriptors.
    u16 code_sel = 0, data_sel = 0;
    auto seg = SegmentTable::allocate_compat32_pair(&code_sel, &data_sel);
    if (seg.is_err()) {
        return VResult<Compat32Executor*>::err(seg.error());
    }

    auto* self = new (std::nothrow) Compat32Executor();
    if (self == nullptr) {
        return VResult<Compat32Executor*>::err(Error::OutOfMemory);
    }
    self->m_base     = base;
    self->m_code_sel = code_sel;
    self->m_data_sel = data_sel;
    self->m_page     = page;
    self->m_pooled   = pooled;
    self->m_owner    = current_thread_id();
    std::memset(self->m_page, 0xCC, kPageSize);   // int3 fill: stray flow traps

    Emitter e{self->m_page, 0};

    // ---- entry32: runs in 32-bit mode, loads the guest and jumps to it ----
    //
    // Order matters. ESP is loaded first because the flags restore below needs
    // a valid stack, and EIP is consumed by an indirect jump through memory so
    // that no register has to survive as a scratch.
    e.pos = kOffEntry32;
    e.load_reg_abs(kESP, base + kSlotEsp);
    e.push_abs(base + kSlotFlg);
    e.popfd();
    e.load_reg_abs(kEAX, base + kSlotEax);
    e.load_reg_abs(kECX, base + kSlotEcx);
    e.load_reg_abs(kEDX, base + kSlotEdx);
    e.load_reg_abs(kEBX, base + kSlotEbx);
    e.load_reg_abs(kEBP, base + kSlotEbp);
    e.load_reg_abs(kESI, base + kSlotEsi);
    e.load_reg_abs(kEDI, base + kSlotEdi);
    e.jmp_indirect_abs(base + kSlotEip);

    // ---- sentinel: where guest code returns ----
    //
    // The guest reaches here by `ret` to the address handed out by
    // sentinel_return_address(). A plain jump into exit32 keeps the sentinel a
    // single identifiable address for the fault-classification map.
    e.pos = kOffSentinel;
    // C7 05 <abs32> <imm32>   mov dword [reason], kReasonSentinel
    e.u8b(0xC7); e.u8b(0x05); e.u32b(base + kOffReason); e.u32b(kReasonSentinel);
    e.u8b(0xE9);                                       // jmp rel32 -> exit32
    e.u32b(static_cast<u32>(kOffExit32 - (e.pos + 4)));

    // ---- collect32: where every thunk stub's `call` lands ----
    //
    // The stub is `call dword ptr fs:[kTebNative32Collect32]`, so the top of
    // the guest stack is the stub's own return address -- slot_addr +
    // thunk_stub_size(). That is the thunk's identity, recovered without any
    // per-slot storage. Popping it restores the stack to exactly what the
    // guest's own `call` left, so the host can dispatch and resume without
    // unwinding anything.
    e.pos = kOffCollect32;
    e.u8b(0xC7); e.u8b(0x05); e.u32b(base + kOffReason); e.u32b(kReasonThunk);
    e.pop_abs(base + kOffThunkTag);
    e.u8b(0xE9);                                       // jmp rel32 -> exit32
    e.u32b(static_cast<u32>(kOffExit32 - (e.pos + 4)));

    // ---- exit32: capture the guest and hand back to 64-bit ----
    //
    // Flags are captured before anything else can disturb them; ESP is stored
    // after the pushfd/pop pair so the recorded value is the guest's own.
    e.pos = kOffExit32;
    e.store_reg_abs(kEAX, base + kSlotEax);
    e.store_reg_abs(kECX, base + kSlotEcx);
    e.store_reg_abs(kEDX, base + kSlotEdx);
    e.store_reg_abs(kEBX, base + kSlotEbx);
    e.store_reg_abs(kEBP, base + kSlotEbp);
    e.store_reg_abs(kESI, base + kSlotEsi);
    e.store_reg_abs(kEDI, base + kSlotEdi);
    e.pushfd();
    e.pop_abs(base + kSlotFlg);
    e.store_reg_abs(kESP, base + kSlotEsp);
    // Leaving compatibility mode: a direct far jump to the 64-bit code
    // segment. current_cs() is read at build time so we return to whatever
    // selector this kernel actually uses rather than a hardcoded 0x2b.
    {
        u16 cs64;
        __asm__ volatile("movw %%cs, %0" : "=r"(cs64));
        e.ljmp_far_direct(base + kOffRet64, cs64);
    }

    // ---- ret64: 64-bit again; restore the host and resume ----
    //
    //   mov rsp, [abs64-via-abs32]   -- reload the full 64-bit RSP
    //   jmp  [resume]
    //
    // RSP is reloaded before anything else: it came back truncated to ESP, so
    // until this executes there is no usable stack.
    //
    // Every operand here is RIP-relative, not absolute: see Emitter::rip_rel.
    e.pos = kOffRet64;
    // 48 0F AE 05 <rel32>   fxsave64 [rip + kOffFxSave]
    // First: capture the guest's x87/MMX/SSE file before any host code can
    // touch it. Needs no stack and no data segment in 64-bit mode.
    e.u8b(0x48); e.u8b(0x0F); e.u8b(0xAE); e.u8b(0x05); e.rip_rel(kOffFxSave);
    // 48 8B 25 <rel32>   mov rsp, qword [rip + ...]
    e.u8b(0x48); e.u8b(0x8B); e.u8b(0x25); e.rip_rel(kOffHostRsp);
    // Restore DS/ES here rather than back in C++: by the time control
    // returns, no register is trustworthy enough to carry a selector, and a
    // stack-relative operand is unusable until RSP above has been reloaded.
    // 8E 1D <rel32>      mov ds, word [rip + ...]
    e.u8b(0x8E); e.u8b(0x1D); e.rip_rel(kOffHostDs);
    // 8E 05 <rel32>      mov es, word [rip + ...]
    e.u8b(0x8E); e.u8b(0x05); e.rip_rel(kOffHostEs);
    // 8E 25 <rel32>      mov fs, word [rip + ...]
    // FS carries the guest TEB while 32-bit code runs; leaving a guest
    // selector loaded is the same delayed-signal hazard as DS.
    e.u8b(0x8E); e.u8b(0x25); e.rip_rel(kOffHostFs);
    // FF 25 <rel32>      jmp qword [rip + ...]
    e.u8b(0xFF); e.u8b(0x25); e.rip_rel(kOffResume);

    // ---- resume32: 64-bit; far-jump straight back into entry32 ----
    //
    //   FF 2C 25 <abs32>   ljmp m16:32 [far pointer]
    //
    // Reached only from the exception bridge, which resumes a faulting guest
    // thread here in 64-bit mode (the kernel gives it no choice) after
    // publishing the state entry32 should load.
    // Reload the guest segments first. Exception delivery resets DS, and only
    // run() loads it on a normal entry -- entry32's very first instruction is a
    // memory access, so arriving with a null DS raises #GP immediately. The
    // selectors come from absolute memory because no register is trustworthy
    // on this path.
    // RIP-relative, NOT absolute [disp32]: an absolute displacement is
    // sign-extended in 64-bit mode, and this pool lives at 0xF0000000, so
    // 0xf000034a became 0xfffffffff000034a and faulted. Same trap ret64 hit.
    e.pos = kOffResume32;
    // 48 0F AE 0D <rel32>   fxrstor64 [rip + kOffFxSave]
    // Restore the guest FP file before re-entering 32-bit mode; the bridge
    // refreshed the image from x86_FLOAT_STATE64 when the fault was captured.
    e.u8b(0x48); e.u8b(0x0F); e.u8b(0xAE); e.u8b(0x0D); e.rip_rel(kOffFxSave);
    e.u8b(0x8E); e.u8b(0x1D); e.rip_rel(kOffGuestDs);   // mov ds, [rip+..]
    e.u8b(0x8E); e.u8b(0x05); e.rip_rel(kOffGuestDs);   // mov es, [rip+..]
    e.u8b(0x8E); e.u8b(0x25); e.rip_rel(kOffGuestFs);   // mov fs, [rip+..]
    e.u8b(0xFF); e.u8b(0x2D); e.rip_rel(kOffFarEntry);  // ljmp m16:32 [rip+..]

    *reinterpret_cast<u16*>(self->m_page + kOffGuestDs) = data_sel;
    *reinterpret_cast<u16*>(self->m_page + kOffGuestFs) = 0;

    // ---- far pointer used by the 64-bit side to enter 32-bit mode ----
    std::memcpy(self->m_page + kOffFarEntry, &base, 4);
    {
        const u32 entry = base + kOffEntry32;
        std::memcpy(self->m_page + kOffFarEntry, &entry, 4);
        std::memcpy(self->m_page + kOffFarEntry + 4, &code_sel, 2);
    }

    // Seed the FXSAVE slot with the host's own sane FP state (MXCSR 0x1F80,
    // FCW 0x37F). An all-zero image would FXRSTOR with every SSE exception
    // unmasked and the first guest SSE op would fault.
    __asm__ volatile("fxsave64 %0"
                     : "=m"(*reinterpret_cast<char(*)[512]>(
                           self->m_page + kOffFxSave)));

    return VResult<Compat32Executor*>::ok(self);
}

void* Compat32Executor::fxsave_area() noexcept {
    return m_page + kOffFxSave;
}

Compat32Exit Compat32Executor::run(Compat32State& state) noexcept {
    if (m_page == nullptr) return Compat32Exit::SetupFailed;

    // Every slot the stubs touch -- register mirror, resume address, saved
    // 64-bit host RSP -- is at a fixed address in this one page, because a
    // 32-bit stub has no base register to spare. A second thread entering here
    // would return onto this thread's host stack. Refuse instead: the
    // corruption would surface arbitrarily far from its cause.
    if (current_thread_id() != m_owner) return Compat32Exit::SetupFailed;

    // The Mach exception handler runs on another thread and cannot call
    // for_current_thread(). Register this executor for the duration of the
    // guest entry so a fault can resume into *this* trampoline's ret64.
    struct BindGuard {
        explicit BindGuard(Compat32Executor* e) {
            X64ExceptionBridge::bind_executor(e);
        }
        ~BindGuard() { X64ExceptionBridge::bind_executor(nullptr); }
    } bind{this};

    // Publish the guest registers where the 32-bit stubs can reach them.
    auto* regs = reinterpret_cast<u32*>(m_page + kOffRegs);
    regs[0] = state.eax; regs[1] = state.ecx;
    regs[2] = state.edx; regs[3] = state.ebx;
    regs[4] = state.esp; regs[5] = state.ebp;
    regs[6] = state.esi; regs[7] = state.edi;
    regs[8] = state.eip; regs[9] = state.eflags;

    const u32 far_ptr_addr = m_base + kOffFarEntry;
    auto* resume_slot   = reinterpret_cast<u64*>(m_page + kOffResume);
    auto* host_rsp_slot = reinterpret_cast<u64*>(m_page + kOffHostRsp);

    // Stash the host selectors where ret64 can reload them without needing a
    // register or a stack.
    *reinterpret_cast<u16*>(m_page + kOffHostDs) = current_ds();
    *reinterpret_cast<u16*>(m_page + kOffHostEs) = current_es();
    *reinterpret_cast<u16*>(m_page + kOffHostFs) = current_fs();
    // resume32 reloads FS from here; 0 means "leave the host's".
    *reinterpret_cast<u16*>(m_page + kOffGuestFs) =
        m_fs_sel != 0 ? m_fs_sel : current_fs();
    const u16 data_sel = m_data_sel;

    // The guest owns every general register while it runs, including the
    // host's callee-saved set: a 32-bit write to EBX/EBP/ESI/EDI zero-extends
    // and destroys the full 64-bit register. They cannot simply be listed as
    // clobbers -- RBP is off limits when it is the frame pointer -- so they
    // are pushed here and popped after control returns. RSP is captured after
    // the pushes so ret64 restores a stack that still holds them.
    //
    // One asm block on purpose: between publishing the resume address and
    // coming back, nothing may be reordered or spilled to a stack that is
    // about to become invalid.
    __asm__ volatile(
        "pushq  %%rbx\n\t"
        "pushq  %%rbp\n\t"
        "pushq  %%r12\n\t"
        "pushq  %%r13\n\t"
        "pushq  %%r14\n\t"
        "pushq  %%r15\n\t"
        "leaq   1f(%%rip), %%rax\n\t"      // resume address for ret64
        "movq   %%rax, (%[resume])\n\t"
        "movq   %%rsp, (%[hostrsp])\n\t"   // full 64-bit RSP; returns as ESP
        "movw   %[dsel], %%ds\n\t"         // 32-bit mode dereferences DS again
        "movw   %[dsel], %%es\n\t"
        "testw  %[fsel], %[fsel]\n\t"     // zero = keep the host's FS
        "jz     3f\n\t"
        "movw   %[fsel], %%fs\n\t"
        "3:\n\t"
        // Guest x87/MMX/SSE file. Addressed relative to the resume slot --
        // both live in the same trampoline page -- because this asm block
        // has no spare register for another pointer operand.
        "fxrstor64 %c[fxoff](%[resume])\n\t"
        "ljmpl  *(%[farptr])\n\t"          // -> entry32, now in 32-bit mode
        "1:\n\t"
        "popq   %%r15\n\t"
        "popq   %%r14\n\t"
        "popq   %%r13\n\t"
        "popq   %%r12\n\t"
        "popq   %%rbp\n\t"
        "popq   %%rbx\n\t"
        :
        : [resume]  "r"(resume_slot),
          [hostrsp] "r"(host_rsp_slot),
          [farptr]  "r"(static_cast<uintptr_t>(far_ptr_addr)),
          [dsel]    "r"(data_sel),
          [fsel]    "r"(m_fs_sel),
          [fxoff]   "i"(static_cast<int>(kOffFxSave - kOffResume))
        : "rax", "rcx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "memory", "cc");

    // Capture what the guest left behind.
    state.eax = regs[0]; state.ecx = regs[1];
    state.edx = regs[2]; state.ebx = regs[3];
    state.esp = regs[4]; state.ebp = regs[5];
    state.esi = regs[6]; state.edi = regs[7];
    state.eflags = regs[9];

    const u32 reason = *reinterpret_cast<const u32*>(m_page + kOffReason);
    if (reason == kReasonFault) {
        // EIP was recorded by the bridge at the faulting instruction.
        state.eip = *reinterpret_cast<const u32*>(m_page + kSlotEip);
        return Compat32Exit::Fault;
    }
    if (reason == kReasonThunk) {
        // EIP is left at the thunk slot so the caller sees where the guest
        // was, not where the trampoline happened to route it.
        state.eip = last_thunk_address();
        return Compat32Exit::ThunkCall;
    }
    state.eip = m_base + kOffSentinel;   // where control actually came back
    return Compat32Exit::ReturnedToSentinel;
}

} // namespace vitreo::cpu::x64
