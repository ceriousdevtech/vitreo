#include "runtime/execution/thunk_direct.hpp"

#include "graphics/common/autorelease_scope.hpp"
#include "runtime/diagnostics/callstream.hpp"
#include "runtime/diagnostics/hostcall_ring.hpp"
#include "runtime/execution/thunk_fast_path.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "core/math/host_fenv.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vitreo::cli {

namespace detail {

std::atomic<int> g_direct_crossing_allowed{-1};

bool decide_direct_crossing_allowed() noexcept {
    const char* needs_full_path = direct_crossing_blocker();
    if (needs_full_path != nullptr) {
        // A measurement must be able to tell "lever off" from "lever on".
        std::fprintf(stderr,
                     "[wp-r1] direct COM crossing off: %s needs the full "
                     "dispatch path\n",
                     needs_full_path);
    }
    const bool allowed = needs_full_path == nullptr;
    g_direct_crossing_allowed.store(allowed ? 1 : 0,
                                    std::memory_order_relaxed);
    return allowed;
}

}  // namespace detail

const char* direct_crossing_blocker() noexcept {
    // Every diagnostic that reads the full path's bookkeeping: with any of
    // them on, every crossing takes the full path so nothing goes missing.
    return hot_hostcall_diagnostics_enabled() || hostcall_capture_enabled()
               ? "the hostcall ring / hostcall traces"
           : diagnostics::callstream_enabled() ? "VITREO_CALLSTREAM"
           : std::getenv("VITREO_API_TRACE") != nullptr ? "VITREO_API_TRACE"
           : std::getenv("VITREO_ABI_CHECK") != nullptr ? "VITREO_ABI_CHECK"
                                                         : nullptr;
}

namespace {

/// direct_thunk_crossings_this_thread(): counted only when a test asks
/// (set_direct_thunk_counting), so the crossing itself touches no
/// thread-local storage for it.
std::atomic<bool> g_count_direct_crossings{false};
thread_local u64 t_direct_crossings = 0;
/// note_first_direct_crossing has been called (it prints once itself; this
/// keeps the out-of-line call off every later crossing).
std::atomic<bool> g_first_crossing_noted{false};

/// The longest stack window taken. The largest COM method in the tables
/// takes well under this; a longer window is simply not taken, and every
/// slot is then read on its own.
constexpr u32 kMaxStackWindow = 192;

/// Bytes from the stack pointer at entry that hold the return address and
/// every stack argument of the export.
[[nodiscard]] u32 stack_window_size(bool is_64bit, u16 stdcall_bytes) noexcept {
    // x86: [esp] return address, then the stdcall arguments it pops.
    if (!is_64bit) return 4u + stdcall_bytes;
    // x64: [rsp] return address, [rsp+8, rsp+0x28) the shadow space, then
    // the fifth argument on. The x86 stdcall byte count is one 4-byte slot
    // per argument or more (an 8-byte argument takes two), so it bounds the
    // argument count from above; if an over-long window reaches an
    // unreadable byte no window is taken at all.
    const u32 slots = stdcall_bytes / 4u;
    return slots > 4u ? 0x28u + (slots - 4u) * 8u : 8u;
}

[[nodiscard]] u64 load_return_address(const u8* bytes, bool is_64bit) noexcept {
    if (is_64bit) {
        u64 value = 0;
        std::memcpy(&value, bytes, sizeof(value));
        return value;
    }
    u32 value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

/// Restores the Marshaler's previous stack window on every exit.
class StackWindowScope {
public:
    explicit StackWindowScope(winapi::Marshaler& marshaler) noexcept
        : m_marshaler(marshaler), m_saved(marshaler.stack_window()) {}
    StackWindowScope(const StackWindowScope&) = delete;
    StackWindowScope& operator=(const StackWindowScope&) = delete;
    ~StackWindowScope() { m_marshaler.set_stack_window(m_saved); }

private:
    winapi::Marshaler& m_marshaler;
    winapi::Marshaler::StackWindow m_saved;
};

}  // namespace

u64 direct_thunk_crossings_this_thread() noexcept {
    return t_direct_crossings;
}

void set_direct_thunk_counting(bool on) noexcept {
    g_count_direct_crossings.store(on, std::memory_order_relaxed);
}

DirectThunkResult run_direct_thunk(
    const winapi::DirectThunk& direct,
    cpu::CpuState& cpu,
    GuestMemory& memory,
    winapi::Marshaler& marshaler,
    winapi::ThreadSuspendRegistry::PublishGuard& suspend_guard,
    cpu::jit::JitCodeArena::HostCallSafepoint* safepoint) {
    const bool is64 = cpu.is_64bit();
    // An in-chain-only entry (L3-K) is not a direct COM crossing: it neither
    // proves WP-R1 live nor counts as one.
    if (!direct.in_chain_only) {
        if (!g_first_crossing_noted.load(std::memory_order_relaxed)) {
            g_first_crossing_noted.store(true, std::memory_order_relaxed);
            winapi::com_direct::note_first_direct_crossing(
                winapi::ThreadManager::get_current_thread_id_static(),
                direct.handler.slot, is64);
        }
        if (g_count_direct_crossings.load(std::memory_order_relaxed)) {
            ++t_direct_crossings;
        }
    }
    // The x86 callee-stack contract of every non-lean host thunk: the area
    // below ESP a real callee would have written is clobbered first.
    maybe_clobber_x86_thunk_callee_stack(cpu, memory);

    // The return address and the stack arguments, located once
    // (Marshaler::StackWindow: the handler reads its stack slots and the RET
    // reads the return address from here, not one guest-memory access each).
    //  * With stack arguments (every x86 method; x64 past four), the whole
    //    range is resolved with ONE checked access: host_ptr is
    //    resolve_for_read, which yields a pointer only when every byte is
    //    readable through one mapping, so a guard page or a split range
    //    leaves the per-slot reads in charge, exactly as before.
    //  * With only the return address (x64, at most four arguments): the
    //    caller's CALL -- or the host, for a callback -- has just written
    //    that slot, so its page is readable and not a guard page (the push
    //    would have taken the guard first), and an aligned 8-byte slot never
    //    splits a range: where the linear view backs it, reading it there
    //    returns exactly what the checked read would.
    // The bytes are read in place while the stack pointer is the one they
    // were located at, as the full path's reads were.
    const GuestAddr entry_sp = static_cast<GuestAddr>(cpu.sp());
    const u32 return_size = is64 ? 8u : 4u;
    const u32 window_size = stack_window_size(is64, direct.stdcall_bytes);
    const u8* window = nullptr;
    if (window_size > return_size) {
        if (window_size <= kMaxStackWindow) {
            window = memory.host_ptr(entry_sp, window_size);
        }
    } else if (memory.linear_backs(entry_sp, return_size)) {
        // UNCHECKED, deliberately: only a broken guest can reach a case where
        // this differs from the checked read. The slot was written by the
        // CALL that entered this crossing (or by the host's checked write of
        // a callback's return address), so its page is mapped and readable.
        // Guard pages: an armed PAGE_GUARD page is host PROT_NONE
        // (core/memory/guard_page_traps.hpp), and the CALL's push already
        // tripped any guard on this page (the JIT raised
        // STATUS_GUARD_PAGE_VIOLATION, or a host write trips it
        // kernel-style), so the page is disarmed by now. Only a guest that
        // JMPs to a gateway with RSP on an unmapped page (where the full
        // path's read fails and the thread dies as Fatal) gets a host fault
        // here instead. The bytes are also read live, after any guest
        // callback the handler makes, exactly like the full path's own read.
        window = memory.linear_host(entry_sp);
    }
    StackWindowScope window_scope{marshaler};
    if (window != nullptr) {
        marshaler.set_stack_window({entry_sp, window_size, window});
    }

    const u64 saved_rdi = cpu.gpr.rdi;
    const u64 saved_rsi = cpu.gpr.rsi;
    const u64 saved_rbx = cpu.gpr.rbx;
    const u64 saved_rbp = cpu.gpr.rbp;
    const u64 saved_r12 = cpu.gpr.r12;
    const u64 saved_r13 = cpu.gpr.r13;
    const u64 saved_r14 = cpu.gpr.r14;
    const u64 saved_r15 = cpu.gpr.r15;
    std::array<cpu::XmmReg, 10> saved_xmm_nonvolatile;
    const u32 saved_mxcsr_control = cpu.sse.mxcsr & 0xffc0u;
    if (is64) {
        std::copy_n(cpu.sse.xmm + 6, saved_xmm_nonvolatile.size(),
                    saved_xmm_nonvolatile.begin());
    }

    // DirectThunk::sync_last_error (the L3-K entries around registered
    // handlers): TEB.LastErrorValue (x86 fs:[0x34], x64 gs:[0x68]) is the
    // thread's last error, and guest code reads and writes it directly, so
    // it goes into the host copy before the handler and back out after it,
    // on every exit -- exactly dispatch_slow_api_thunk's sync.
    if (direct.sync_last_error) {
        const GuestAddr teb = is64 ? cpu.seg.gs_desc.base : cpu.seg.fs_desc.base;
        if (const auto teb_err = memory.read_u32(teb + (is64 ? 0x68 : 0x34));
            teb_err.is_ok()) {
            winapi::set_last_error(teb_err.value());
        }
    }

    // The host call is a safepoint: the chain's lease is parked across it
    // (it may block on the device lock, wait on the GPU, or run guest code).
    if (safepoint != nullptr) safepoint->park();
    suspend_guard.enter_host_call();
    {
        // The host call's own autorelease pool (dispatch_slow_api_thunk).
        graphics::AutoreleaseScope host_call_pool;
        // ...and the host FP environment: native code runs under the
        // platform default, the guest's mode is restored after it.
        math::HostFpEnvironmentScope host_call_fp(cpu.fpu.control_word, cpu.sse.mxcsr);
        direct.handler.fn(direct.handler.context, marshaler,
                          direct.handler.slot);
    }
    suspend_guard.leave_host_call();
    if (safepoint != nullptr) (void)safepoint->unpark();

    if (direct.sync_last_error) {
        const GuestAddr teb = is64 ? cpu.seg.gs_desc.base : cpu.seg.fs_desc.base;
        (void)memory.write_u32(teb + (is64 ? 0x68 : 0x34),
                               winapi::get_last_error());
    }

    // From here on, exactly dispatch_slow_api_thunk's exits.
    // RtlUnwind owns the nonvolatiles: nothing is restored.
    if (cpu.pending_unwind.active) {
        cpu.thunk_control_transfer = false;
        return DirectThunkResult::Unwinding;
    }
    const bool transferred = cpu.thunk_control_transfer;
    cpu.thunk_control_transfer = false;
    if (!transferred) {
        cpu.gpr.rdi = saved_rdi;
        cpu.gpr.rsi = saved_rsi;
        cpu.gpr.rbx = saved_rbx;
        cpu.gpr.rbp = saved_rbp;
        cpu.gpr.r12 = saved_r12;
        cpu.gpr.r13 = saved_r13;
        cpu.gpr.r14 = saved_r14;
        cpu.gpr.r15 = saved_r15;
        if (is64) {
            std::copy(saved_xmm_nonvolatile.begin(),
                      saved_xmm_nonvolatile.end(), cpu.sse.xmm + 6);
            // Callee-saved, except for the CRT functions whose documented
            // purpose is to change it (CpuState::thunk_sets_fp_control).
            if (!cpu.thunk_sets_fp_control) {
                cpu.sse.mxcsr = (cpu.sse.mxcsr & 0x3fu) | saved_mxcsr_control;
            }
        }
    }
    cpu.thunk_sets_fp_control = false;
    if (transferred) return DirectThunkResult::ControlTransferred;
    // RaiseException leaves rip at the gateway; the outer SEH path simulates
    // the RET before dispatching.
    if (cpu.pending_raise.active) return DirectThunkResult::Raising;

    // RET: x64 pops 8 (the caller cleans its arguments); x86 pops 4 plus the
    // stdcall bytes. The window answers while the stack pointer is the one it
    // was taken at; otherwise the return address is read where it now is,
    // as finish_thunk_ret does.
    const GuestAddr sp = static_cast<GuestAddr>(cpu.sp());
    u64 return_address = 0;
    if (window != nullptr && sp == entry_sp) {
        return_address = load_return_address(window, is64);
    } else if (is64) {
        const auto read = memory.read_u64(sp);
        if (read.is_err()) return DirectThunkResult::Fatal;
        return_address = read.value();
    } else {
        const auto read = memory.read_u32(sp);
        if (read.is_err()) return DirectThunkResult::Fatal;
        return_address = read.value();
    }
    cpu.rip = return_address;
    cpu.set_sp(cpu.sp() + (is64 ? 8u : 4u + direct.stdcall_bytes));
    return DirectThunkResult::Returned;
}

}  // namespace vitreo::cli
