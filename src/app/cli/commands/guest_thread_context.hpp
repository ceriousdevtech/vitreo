#pragma once

/// @file guest_thread_context.hpp
/// @brief Allocation of the guest-side state a thread needs to run guest code.
///
/// A guest thread is not just a host thread: it needs a guest stack, a TEB the
/// guest can reach through FS/GS, and a per-thread TLS array with every DLL's
/// __declspec(thread) block replayed into it. Without the last part, DLL TLS
/// accessors read a null slot and fault.
///
/// This was originally inline in CreateThread's run callback. It is factored
/// out because Vitreo itself needs guest-callable service threads - the
/// multimedia timer scheduler has to invoke guest TIMEPROCs - and those must
/// build their context once and reuse it, rather than allocating a stack and
/// TEB per callback and never freeing them.

#include <vitreo/common.hpp>

#include <vector>

namespace vitreo { class GuestMemory; }
namespace vitreo::cpu { struct CpuState; }

namespace vitreo::cli {

struct RuntimeContext;

/// Guest-side allocations backing one thread. A context whose thread has
/// exited must be handed to release_guest_thread_context(); a context reused
/// for repeated guest calls (a Vitreo service thread) is released only when
/// that service stops.
struct GuestThreadContext {
    u64 stack_base{0};
    u64 stack_size{0};
    u64 teb_addr{0};
    /// TEB.ThreadLocalStoragePointer allocation (TLS array + main-EXE block).
    u64 tls_base{0};
    /// Entries in the TLS array at tls_base (ModuleManager grows it later).
    u32 tls_capacity{0};
    /// One per DLL static-TLS slot replayed into this thread's TLS array.
    std::vector<u64> dll_tls_blocks;
    /// TEB layout of teb_addr (the TlsExpansionSlots field moves with it).
    bool is_64bit{false};

    [[nodiscard]] bool valid() const noexcept {
        return stack_base != 0 && teb_addr != 0;
    }
};

/// Inputs that describe the process the thread belongs to. Grouped so the
/// signature does not grow a seventh positional argument.
struct GuestThreadContextParams {
    bool is_64bit{false};
    u64 peb_address{0};
    GuestAddr pe_tls_data_start{0};
    u32 pe_tls_data_size{0};
    /// Stack reservation for the thread (winapi::compute_thread_stack_sizes).
    /// 0 = a Vitreo service thread, which gets the fixed service-thread size.
    u64 stack_reserve{0};
};

/// Allocate stack, TEB and TLS for a new guest thread. Returns an invalid
/// context when any allocation fails; the caller must check valid().
[[nodiscard]] GuestThreadContext create_guest_thread_context(
    GuestMemory& memory, RuntimeContext& ctx,
    const GuestThreadContextParams& params, u32 tid);

/// Release every guest allocation backing an exited thread: its DLL static-TLS
/// blocks, TLS array, TEB and stack, and the TEB's registration for dynamic
/// static-TLS updates.
///
/// Windows reclaims a terminated thread's stack and TEB, and a guest that
/// churns threads (a launcher's content downloader) otherwise grows the guest
/// address space and the region map without bound — every later allocation
/// search then walks a longer map, so the guest gets progressively slower.
/// Call only after the thread's guest code has finished running.
void release_guest_thread_context(GuestMemory& memory, RuntimeContext& ctx,
                                  const GuestThreadContext& context);

/// Build a CpuState positioned to enter guest code on `context`. The stack
/// pointer is placed at the top of the guest stack, with the x86 headroom the
/// guest CRT expects. Safe to call repeatedly on one context to reset the
/// stack between successive guest calls on the same service thread.
[[nodiscard]] cpu::CpuState make_guest_cpu_state(
    GuestMemory& memory, const GuestThreadContext& context, bool is_64bit);

} // namespace vitreo::cli
